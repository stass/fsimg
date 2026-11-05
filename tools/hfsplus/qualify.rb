#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# The native qualification cycle of the HFS Plus writer, on macOS. For each
# image of `images` the writer makes a raw volume, and then:
#
#   1. fsck_before  attach without mounting, `fsck_hfs -fn`
#   2. mount        mount read-only; compare find, stat and checksums with what
#                   was given to the writer
#   3. modify       mount read-write; create, append until a file fragments,
#                   rename, mkdir, rmdir, unlink, set attributes, write a
#                   resource fork, make hard links, compress with ditto; unmount
#   4. fsck_after   `fsck_hfs -fn` again
#   5. remount      mount read-only; compare again, with the modifications
#   6. fsimg        `fsimg W` verifies the volume, `t --json` and `t -v` list it
#                   with the extra-data marks expected, `x -O` delivers each file
#
# A step's failure does not stop the steps after it, so that one result does not
# hide another. A smallest or nearly full volume may refuse a modification for
# lack of room; the refusal is recorded, every other one is a failure.
#
# Stored ownership shows only under `hdiutil attach -owners on`, where the
# mounting user can read only what the stored mode lets others read. So each
# compare is two mounts: metadata with owners on, checksums with owners off.
#
# Only images this script generated are attached, all inside --root. Example:
#
#   qualify.rb --generator BUILD/hfsplus-write-gen --fsimg BUILD/fsimg \
#     --root ~/.cache/fsimg-hfs/s9-tmp --only 'hfsx-block'
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "timeout"
require_relative "capture"

module HfsPlusQualify
  class Error < HfsPlusCapture::Error; end

  C = HfsPlusCapture

  VARIANTS = %w[hfsplus hfsx].freeze
  BLOCKS = [512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536].freeze
  # Room for every modification at the largest block.
  ORDINARY_BYTES = 32 << 20
  LARGE_BYTES = 1 << 30
  LARGE_BLOCKS = [4096, 8192].freeze
  NEARLY_FULL_BYTES = 4 << 20
  NEARLY_FULL_FREE_BLOCKS = 2
  # Modifications run out of room part of the way through.
  LOW_SPACE_BYTES = 512 << 10
  # One entry more than the catalog header's map holds, with free space beyond.
  MAP_NODE_FILES = 86_770
  MAP_NODE_BYTES = 160 << 20
  # A catalog whose fork passes 2^32 bytes, where its clumpSize saturates.
  LARGE_CATALOG_FILES = 2_970_000
  LARGE_CATALOG_BYTES = (1 << 32) + (400 << 20)
  BASE_TIME = 1_700_000_000
  COMMAND_TIMEOUT = 30 * 60
  # Each round leaves one more extent on the file, so 24 rounds need the extents
  # overflow tree: a catalog record holds eight.
  FRAGMENT_ROUNDS = 24
  # What macOS adds to a volume's root that readdir shows. The folders it uses
  # for hard links are hidden from readdir but appear in the catalog.
  NATIVE_ROOT = %w[.fseventsd .Spotlight-V100 .Trashes .TemporaryItems .DS_Store].freeze
  PRIVATE_ROOT = ["\u2400\u2400\u2400\u2400HFS+ Private Data", ".HFS+ Private Directory Data\r"].freeze
  OWNER_VIEW = %w[-nobrowse -owners on].freeze
  CONTENT_VIEW = %w[-nobrowse -owners off].freeze
  METADATA = %i[kind size mode uid gid created modified changed accessed target].freeze

  # An entry as the writer is given it, one line of the generator's spec.
  Entry = Data.define(:kind, :path, :size, :mode, :uid, :gid, :created, :modified, :changed, :accessed, :target) do
    def self.of(kind, path, size: 0, target: nil, mode: 0o644, uid: 123, gid: 456, time: BASE_TIME)
      new(kind: kind, path: path, size: size, mode: mode, uid: uid, gid: gid, created: time, modified: time + 1,
          changed: time + 2, accessed: time + 3, target: target)
    end

    def spec_line
      [kind, size, mode.to_s(8), uid, gid, created, modified, changed, accessed,
       target ? target.unpack1("H*") : "-", path.unpack1("H*")].join("\t")
    end
  end

  # What a path should show, or what one shows; nil means it is not compared.
  View = Data.define(:kind, :size, :mode, :uid, :gid, :created, :modified, :changed, :accessed, :target, :sha256, :extra) do
    def self.of(kind:, size: nil, mode: nil, uid: nil, gid: nil, created: nil, modified: nil, changed: nil,
                accessed: nil, target: nil, sha256: nil, extra: nil)
      new(kind: kind, size: size, mode: mode, uid: uid, gid: gid, created: created, modified: modified, changed: changed,
          accessed: accessed, target: target, sha256: sha256, extra: extra)
    end

    def regular? = kind == "file"
  end

  Image = Data.define(:name, :variant, :block, :volume_bytes, :entries, :limited, :fill, :pairs, :refused) do
    def self.of(name:, variant:, block:, volume_bytes:, entries:, limited: false, fill: false, pairs: [], refused: [])
      new(name: name, variant: variant, block: block, volume_bytes: volume_bytes, entries: entries, limited: limited,
          fill: fill, pairs: pairs, refused: refused)
    end
  end

  # A pair of names and whether macOS should resolve both to one file.
  Pair = Data.define(:first, :second, :collide)

  Tools = Data.define(:generator, :fsimg)

  module_function

  # hwf_byte of test/hfsplus-write-fixture.c: the content of the file on line
  # `index` of a spec.
  def content(index, size)
    return "".b if size.zero?

    id = 1000 + index
    period = (0...251).map { 1 + (id * 17 + _1 * 13) % 251 }.pack("C*")
    (period * (size / 251 + 1)).byteslice(0, size)
  end

  # What readdir returns for a name given to the writer: E3's canonical
  # decomposition, in which no supplementary value decomposes or reorders. The
  # names here avoid the BMP ranges macOS also leaves alone. Ruby orders marks
  # before expanding U+0F73 and its like, which have class 0 but decompose into
  # marks, so a second pass sorts what the first expanded, as E3 observes.
  def native_path(path) =
    path.gsub(/[^\u{10000}-\u{10ffff}]+/) { _1.unicode_normalize(:nfd).unicode_normalize(:nfd) }.b

  # The tree expected on a volume made from `entries`.
  def expected_tree(entries)
    entries.each_with_index.to_h do |entry, index|
      file = entry.kind == "file"
      [native_path(entry.path),
       View.of(kind: entry.kind, size: file ? entry.size : nil, mode: entry.mode, uid: entry.uid, gid: entry.gid,
               created: entry.created, modified: entry.modified, changed: entry.changed, accessed: entry.accessed,
               target: entry.target&.b, sha256: file ? Digest::SHA256.hexdigest(content(index, entry.size)) : nil,
               extra: false)]
    end
  end

  # The kinds, sizes, owners, modes and times of test/hfsplus-write-fixture.c's
  # tree, varied so that a field the writer swapped would show.
  def block_tree(block)
    files = { "/dir/sub/below" => block - 1, "/equal" => block, "/above" => block + 1, "/two" => 2 * block + 1,
              "/big" => 40 * block, "/hello" => 5, "/caf\u00e9" => 19, "/empty" => 0, "/Zeta" => 0, "/alpha" => 0,
              "/\u03a9" => 0, "/\u03b1" => 0, "/:" => 0, "/\u2400" => 0, "/\u{1f600}" => 0, "/\uff21" => 0,
              "/a\u200cx" => 0, "/ay" => 0, "/#{'a' * 200}" => 0, "/#{'z' * 200}" => 0 }
    modes = [0o644, 0o640, 0o751, 0o444, 0o600, 0o755].cycle
    owners = [[123, 456], [0, 0], [65_000, 20], [501, 20]].cycle
    times = (0..).lazy.map { BASE_TIME - 86_400 * 3650 + 1000 * _1 }
    entry = lambda do |kind, path, **fields|
      uid, gid = owners.next
      Entry.of(kind, path, uid: uid, gid: gid, time: times.next, **fields)
    end
    [entry.call("directory", "/dir", mode: 0o755), entry.call("directory", "/dir/sub", mode: 0o775),
     entry.call("directory", "/dir/vacant", mode: 0o757)] +
      files.map { |path, size| entry.call("file", path, size: size, mode: modes.next) } +
      [entry.call("symlink", "/link", target: "dir/sub/below", mode: 0o755),
       entry.call("symlink", "/dangling", target: "../#{'t' * 900}", mode: 0o777),
       entry.call("fifo", "/pipe", mode: 0o640)]
  end

  # Names at every length after decomposition, 1 to 255 UTF-16 units, spelled
  # with units that are one, two (a decomposing letter, a surrogate pair, a
  # two-jamo syllable) and three to a character.
  def length_names
    plain = (1..255).map { "x" * _1 }
    doubled = %W[\u00e9 \u{1f600} \uac00].flat_map do |unit|
      (1..127).flat_map { [unit * _1, "#{unit * _1}x"] }
    end
    (plain + doubled + (1..85).map { "\ud55c" * _1 }).uniq
  end

  # A name one unit too long, in each spelling.
  OVERLONG_NAMES = ["x" * 256, "\u00e9" * 128, "#{"\u{1f600}" * 127}xx", "\ud55c" * 86].freeze

  # E3's collision sets. These pairs collide on both comparators: decomposition,
  # the Hangul algorithm, combining order, and the canonical singletons.
  BOTH_COLLIDE = [["cafe\u0301", "caf\u00e9"], ["\uac00", "\u1100\u1161"],
                  ["a\u0f72\u0f73", "a\u0f73\u0f72"], ["a\u0f73", "a\u0f72\u0f71"],
                  ["\u0300", "\u0340"], [";", "\u037e"], ["\u00b7", "\u0387"], ["\u03ac", "\u1f71"]].freeze
  # These collide on `hfsplus` only: case folding and ignorable units.
  PLUS_COLLIDE = [%w[Zeta zeta], ["\u03a9", "\u03c9"], ["\uff21", "\uff41"], %W[ab a\u200cb], %W[pq p\ufeffq]].freeze
  # These collide on neither: supplementary values neither fold nor reorder.
  NONE_COLLIDE = [["\u{10400}", "\u{10428}"], ["a\u0300\u{1d165}", "a\u{1d165}\u0300"]].freeze

  def collision_pairs(variant)
    both = BOTH_COLLIDE.map { Pair.new(first: _1, second: _2, collide: true) }
    plus = PLUS_COLLIDE.map { Pair.new(first: _1, second: _2, collide: variant == "hfsplus") }
    both + plus + NONE_COLLIDE.map { Pair.new(first: _1, second: _2, collide: false) }
  end

  def collision_entries(pairs)
    names = pairs.flat_map { _1.collide ? [_1.first] : [_1.first, _1.second] }
    names.each_with_index.map { |name, index| Entry.of("file", "/#{name}", size: index + 1) }
  end

  def map_node_entries(files) =
    (0...files).map { Entry.of("file", format("/%08d%s", _1, "n" * 232)) }

  def images(variant)
    blocks = BLOCKS.map do |block|
      Image.of(name: "#{variant}-block-#{block}", variant: variant, block: block, volume_bytes: ORDINARY_BYTES,
               entries: block_tree(block))
    end
    large = LARGE_BLOCKS.map do |block|
      Image.of(name: "#{variant}-1gib-block-#{block}", variant: variant, block: block, volume_bytes: LARGE_BYTES,
               entries: block_tree(block))
    end
    pairs = collision_pairs(variant)
    blocks + large + [
      Image.of(name: "#{variant}-smallest-empty", variant: variant, block: 0, volume_bytes: nil, entries: [], limited: true),
      Image.of(name: "#{variant}-smallest-tree", variant: variant, block: 0, volume_bytes: nil, entries: block_tree(4096), limited: true),
      Image.of(name: "#{variant}-nearly-full", variant: variant, block: 4096, volume_bytes: NEARLY_FULL_BYTES,
               entries: block_tree(4096), limited: true, fill: true),
      Image.of(name: "#{variant}-low-space", variant: variant, block: 4096, volume_bytes: LOW_SPACE_BYTES,
               entries: block_tree(4096), limited: true),
      Image.of(name: "#{variant}-map-nodes", variant: variant, block: 4096, volume_bytes: MAP_NODE_BYTES, entries: map_node_entries(MAP_NODE_FILES)),
      Image.of(name: "#{variant}-names", variant: variant, block: 4096, volume_bytes: ORDINARY_BYTES,
               entries: length_names.map { Entry.of("file", "/#{_1}") }, refused: OVERLONG_NAMES),
      Image.of(name: "#{variant}-collisions", variant: variant, block: 4096, volume_bytes: ORDINARY_BYTES,
               entries: collision_entries(pairs), pairs: pairs)
    ]
  end

  # One image's trip through the cycle. It holds the tree the volume should
  # show, which the modifications change; everything it attaches it detaches.
  class Cycle
    STEPS = %i[generate fsck_before mount modify fsck_after remount fsimg].freeze

    def initialize(image, tools, directory)
      @image = image
      @tools = tools
      @directory = directory
      @path = File.join(directory, "image.img")
      @mountpoint = File.join(directory, "mnt")
      @log = C::Log.new
      @attachments = C::Attachments.new(@log)
      @tree = expected_tree_of(image)
      @refusals = []
      @facts = {}
    end

    # Run every step and return the image's row of the results.
    def run
      FileUtils.mkdir_p(@mountpoint)
      steps = STEPS.to_h { [_1, attempt(_1)] }
      released = release
      { image: @image.name, **steps, refused: @refusals, facts: @facts, detach: released }
    ensure
      File.write(File.join(@directory, "commands.txt"), @log.to_s)
    end

    private

    def expected_tree_of(image)
      return {} if image.fill

      HfsPlusQualify.expected_tree(image.entries)
    end

    # The boundary of one step: it records the failure as the step's result.
    def attempt(step)
      send(step)
      "pass"
    rescue C::Error, SystemCallError, Timeout::Error, JSON::ParserError => e
      "FAIL: #{HfsPlusQualify.text(e.message.lines.first(12).join.strip)}"
    end

    def release
      @attachments.release
      forced = @log.to_s.lines.grep(/\$ hdiutil detach .* -force/)
      forced.empty? ? "pass" : "FAIL: forced #{forced.size} detach"
    rescue C::Error => e
      "FAIL: #{HfsPlusQualify.text(e.message)}"
    end

    # Generate the image, and first the images the writer must refuse.
    def generate
      @image.refused.each { refused_name(_1) }
      @image.pairs.select(&:collide).each { refused_pair(_1) }
      entries = @image.fill ? with_filler : @image.entries
      @tree = HfsPlusQualify.expected_tree(entries)
      generator(@path, entries, @image.block, @image.volume_bytes)
      @facts[:image_bytes] = File.size(@path)
      @facts[:fields_before] = fields
    end

    def refused_name(name) = refused_entries([Entry.of("file", "/#{name}")], name)
    def refused_pair(pair) = refused_entries([Entry.of("file", "/#{pair.first}"), Entry.of("file", "/#{pair.second}")], pair.second)

    def refused_entries(entries, name)
      probe = File.join(@directory, "refused.img")
      out, code = generator_status(probe, entries, 4096, nil)
      raise Error, "the writer accepted #{name.dump}" unless code == 1 && !File.exist?(probe)

      @log.note("  refused #{name.dump}: #{HfsPlusQualify.text(out.lines.first.to_s)}")
    end

    # A filler that leaves NEARLY_FULL_FREE_BLOCKS blocks: the volume the entries
    # and an empty filler minimize to says how much remains.
    def with_filler
      filler = Entry.of("file", "/filler")
      probe = File.join(@directory, "probe.img")
      generator(probe, @image.entries + [filler], @image.block, nil)
      remaining = @image.volume_bytes - File.size(probe) - NEARLY_FULL_FREE_BLOCKS * @image.block
      File.unlink(probe)
      raise Error, "nearly full volume has no room for a filler" unless remaining.positive?

      @image.entries + [filler.with(size: remaining)]
    end

    def generator(path, entries, block, volume_bytes)
      out, code = generator_status(path, entries, block, volume_bytes)
      raise Error, "generator exit #{code}: #{out}" unless code == 0
    end

    def generator_status(path, entries, block, volume_bytes)
      spec = File.join(@directory, "spec.tsv")
      File.write(spec, entries.map { "#{_1.spec_line}\n" }.join)
      args = [@tools.generator, path, @image.variant, block.to_s, "spec:#{spec}"]
      args << volume_bytes.to_s if volume_bytes
      @log.run_status(*args, timeout: COMMAND_TIMEOUT)
    end

    def fields = File.open(@path, "rb") { HfsPlusFields.decode(_1, limit: 1) }.then { tree_facts(_1) }

    def tree_facts(report)
      header = report.fetch(:volume_header)
      trees = report.fetch(:trees)
      { last_mounted: header.fetch(:last_mounted), write_count: header.fetch(:write_count),
        free_blocks: header.fetch(:free_blocks), total_blocks: header.fetch(:total_blocks),
        block_size: header.fetch(:block_size),
        extents_leaf_records: trees.dig(:extents, :leaf_records), extents_nodes: trees.dig(:extents, :total_nodes),
        attributes_leaf_records: trees.dig(:attributes, :leaf_records), attributes_nodes: trees.dig(:attributes, :total_nodes),
        catalog_nodes: trees.dig(:catalog, :total_nodes), catalog_free_nodes: trees.dig(:catalog, :free_nodes),
        catalog_node_bytes: trees.dig(:catalog, :node_size), catalog_clump: trees.dig(:catalog, :clump_size) }
    end

    def fsck_before = fsck(:fsck_before_output)
    def fsck_after = fsck(:fsck_after_output)

    def fsck(fact)
      out, ok = @attachments.with(@path, readonly: true) do |device|
        @log.run("fsck_hfs", "-fn", device.sub("/dev/", "/dev/r"), timeout: C::FSCK_TIMEOUT)
      end
      @facts[fact] = HfsPlusQualify.text(out.lines.last.to_s.strip)
      findings = C.fsck_findings(out)
      raise Error, "fsck_hfs findings: #{findings.join('; ')}" unless findings.empty?
      raise Error, "fsck_hfs did not pass: #{out}" unless ok && out.include?("appears to be OK")
    end

    def mount = compare(first: true)
    def remount = compare(first: false)

    # The metadata mount and the checksum mount, both read-only. A mount never
    # precedes the first metadata read, so access times are the writer's until
    # the read-write mount.
    def compare(first:)
      @attachments.with(@path, mountpoint: @mountpoint, readonly: true, options: OWNER_VIEW) do
        report_differences("metadata", observe(digests: false), METADATA)
        check_pairs if first
      end
      @attachments.with(@path, mountpoint: @mountpoint, readonly: true, options: CONTENT_VIEW) do
        report_differences("checksums", observe(digests: true), [:sha256])
      end
    end

    def report_differences(what, observed, fields)
      differences = HfsPlusQualify.differences(@tree, observed, fields)
      raise Error, "#{what}: #{differences.first(8).join('; ')} (#{differences.size} differences)" unless differences.empty?
    end

    # The volume, by `find`, with macOS's own additions to the root left out.
    def observe(digests:)
      out, ok = @log.run("find", @mountpoint, "-mindepth", "1", *native_prune, "-print0", timeout: COMMAND_TIMEOUT)
      raise Error, "find failed: #{out}" unless ok

      out.split("\0".b).to_h { [_1.byteslice(@mountpoint.bytesize..).b, HfsPlusQualify.view_of(_1, digests: digests)] }
    end

    # What macOS adds is left out unread: its .Trashes refuses the mounting user.
    def native_prune
      paths = NATIVE_ROOT.flat_map { ["-o", "-path", File.join(@mountpoint, _1)] }.drop(1)
      ["(", *paths, ")", "-prune", "-o"]
    end

    # Each pair resolves as E3 says: colliding names to one file, others to two.
    def check_pairs
      @image.pairs.each do |pair|
        first, second = [pair.first, pair.second].map { File.join(@mountpoint, _1) }
        same = File.lstat(first).ino == File.lstat(second).ino
        raise Error, "#{pair.first.dump} and #{pair.second.dump}: collide is #{same}, not #{pair.collide}" unless same == pair.collide
      end
    end

    def modify
      @attachments.with(@path, mountpoint: @mountpoint, options: CONTENT_VIEW) do
        @tree = HfsPlusQualify.relaxed(@tree)
        Modification.new(root: @mountpoint, log: @log, tree: @tree, limited: @image.limited, scratch: @directory,
                         block: @facts.fetch(:fields_before).fetch(:block_size)).run.then do |tree, refusals|
          @tree = tree
          @refusals = refusals
        end
      end
      @facts[:fields_after] = fields
      check_modified unless @image.limited
    end

    # The steps the provisional one-node trees answer to did reach the trees.
    def check_modified
      after = @facts.fetch(:fields_after)
      raise Error, "no extents overflow record: the file did not fragment" unless after.fetch(:extents_leaf_records).to_i.positive?
      raise Error, "no attribute record" unless after.fetch(:attributes_leaf_records).to_i.positive?
    end

    def fsimg
      verify, code = @log.run_status(@tools.fsimg, "W", "-f", @path, timeout: COMMAND_TIMEOUT)
      raise Error, "fsimg W exit #{code}: #{verify}" unless code == 0

      listed = fsimg_entries
      report_differences("fsimg t --json", listed, METADATA - %i[changed accessed] + [:extra])
      check_marks
      check_delivery
    end

    def fsimg_entries
      fsimg_output("t", "--json", "-f", @path).lines.filter_map do |line|
        record = JSON.parse(line)
        next unless record["type"] == "entry"

        path = record.fetch("path")
        [path.b, HfsPlusQualify.view_of_record(record)] unless HfsPlusQualify.native_extra?(path)
      end.to_h
    end

    # The `@` of `t -v` marks exactly the entries whose extra data is expected.
    def check_marks
      marked = fsimg_output("t", "-v", "-f", @path).lines.filter_map do |line|
        line[/\A\S+@\s.*?"(.*)"\s*\z/, 1]&.then { JSON.parse("\"#{_1}\"") }
      end
      kept = marked.reject { HfsPlusQualify.native_extra?(_1) }.map(&:b).sort
      wanted = @tree.select { |_, view| view.extra }.keys.sort
      raise Error, "t -v marks #{kept.inspect}, not #{wanted.inspect}" unless kept == wanted
    end

    # `x -O` delivers each stored file's bytes, decoded if compressed.
    def check_delivery
      @tree.each do |path, view|
        next unless view.regular? && view.sha256 && view.size.to_i.positive?

        digest = Digest::SHA256.hexdigest(fsimg_output("x", "-O", "-f", @path, path.dup.force_encoding("UTF-8")))
        raise Error, "x -O #{path.inspect} delivers #{digest}, not #{view.sha256}" unless digest == view.sha256
      end
    end

    def fsimg_output(*args)
      out, err, status = Timeout.timeout(COMMAND_TIMEOUT) { Open3.capture3(@tools.fsimg, *args, binmode: true) }
      raise Error, "fsimg #{args.first} exit #{status.exitstatus}: #{err}" unless status.success?

      out.force_encoding("UTF-8")
    end
  end

  # The read-write mount's work: each step changes the volume and returns the
  # tree it now shows. A step's own folder goes away when a limited volume
  # refuses it, so that a refused step leaves the tree as it was: ENOSPC, or
  # ENOTSUP from link(2), seen where macOS had no room for its hard-link folder.
  class Modification
    PATTERN = "fsimg qualification\n"
    Step = Data.define(:name, :folder, :run)
    # E6: ditto stores 65536 zeros inline in the decmpfs attribute, and text of
    # this size in the resource fork.
    COMPRESSED = { "inline" => ("\0" * 65_536).b.freeze, "fork" => (PATTERN * 8192).b.freeze }.freeze

    def initialize(root:, log:, tree:, limited:, scratch:, block:)
      @root = root
      @log = log
      @tree = tree
      @limited = limited
      @scratch = scratch
      @originals = tree.select { |path, view| view.regular? && path.count("/") == 1 }.keys.sort
      @block = block
    end

    # The final tree and the steps refused.
    def run
      steps.reduce([@tree, []]) do |(tree, refusals), step|
        [apply(step, tree), refusals]
      rescue Errno::ENOSPC, Errno::ENOTSUP => e
        raise unless @limited

        FileUtils.rm_rf(File.join(@root, step.folder)) if step.folder
        [tree, refusals + ["#{step.name}: #{e.message}"]]
      end
    end

    private

    def apply(step, tree) = step.run.call(tree)

    def steps
      [Step.new(name: "create", folder: "q-create", run: method(:create)),
       Step.new(name: "fragment", folder: "q-fragment", run: method(:fragment)),
       Step.new(name: "mkdir-rmdir", folder: "q-folders", run: method(:folders)),
       Step.new(name: "unlink", folder: "q-unlink", run: method(:unlink_new)),
       Step.new(name: "attribute", folder: "q-attr", run: method(:attribute)),
       Step.new(name: "resource-fork", folder: "q-rsrc", run: method(:resource_fork)),
       Step.new(name: "hard-link", folder: "q-link", run: method(:hard_link)),
       Step.new(name: "compress", folder: "q-comp", run: method(:compress)),
       *original_steps]
    end

    # The same operations on what the writer stored, while enough files exist.
    def original_steps
      [["rename-original", :rename_original], ["unlink-original", :unlink_original],
       ["link-original", :link_original], ["attribute-original", :attribute_original]].zip(@originals)
        .select { _2 }.map { |(name, op), path| Step.new(name: name, folder: nil, run: ->(tree) { method(op).call(tree, path) }) }
    end

    def full(path) = File.join(@root, path.dup.force_encoding("UTF-8"))
    def folder(name) = FileUtils.mkdir_p(File.join(@root, name)).then { ["/#{name}".b, directory_view] }
    def directory_view = View.of(kind: "directory", extra: false)
    def file_view(bytes) = View.of(kind: "file", size: bytes.bytesize, sha256: Digest::SHA256.hexdigest(bytes), extra: false)

    def write(path, bytes)
      File.binwrite(File.join(@root, path), bytes)
      ["/#{path}".b, file_view(bytes)]
    end

    def create(tree)
      tree.merge([folder("q-create"), write("q-create/new", PATTERN * 500)].to_h)
    end

    # Each round fsyncs one more block onto `data` and a block of a new file
    # right after it, so that the next round's block cannot join the extent.
    def fragment(tree)
      made = [folder("q-fragment")]
      chunk = (PATTERN * (@block / PATTERN.bytesize + 1)).byteslice(0, @block)
      FRAGMENT_ROUNDS.times do |round|
        File.open(File.join(@root, "q-fragment/data"), "ab") { sync(_1, chunk) }
        spacer = format("q-fragment/spacer-%02d", round)
        File.open(File.join(@root, spacer), "wb") { sync(_1, chunk) }
        made << ["/#{spacer}".b, file_view(chunk)]
      end
      tree.merge((made + [["/q-fragment/data".b, file_view(chunk * FRAGMENT_ROUNDS)]]).to_h)
    end

    def sync(file, bytes)
      file.write(bytes)
      file.fsync
    end

    def folders(tree)
      kept = folder("q-folders")
      Dir.mkdir(File.join(@root, "q-folders/kept"))
      Dir.mkdir(File.join(@root, "q-folders/gone"))
      Dir.rmdir(File.join(@root, "q-folders/gone"))
      tree.merge([kept, ["/q-folders/kept".b, directory_view]].to_h)
    end

    def unlink_new(tree)
      kept = folder("q-unlink")
      made = write("q-unlink/before", PATTERN * 2)
      write("q-unlink/gone", PATTERN)
      File.unlink(File.join(@root, "q-unlink/gone"))
      tree.merge([kept, made].to_h)
    end

    # An attribute inline in its record and one too large for a record.
    def attribute(tree)
      made = [folder("q-attr"), write("q-attr/file", PATTERN)]
      tool("xattr", "-w", "org.fsimg.small", "value", File.join(@root, "q-attr/file"))
      tool("xattr", "-w", "org.fsimg.large", "v" * 8192, File.join(@root, "q-attr/file"))
      tree.merge(made.to_h.transform_values { _1.regular? ? _1.with(extra: true) : _1 })
    end

    def resource_fork(tree)
      kept = folder("q-rsrc")
      path, view = write("q-rsrc/file", PATTERN)
      File.binwrite(File.join(@root, "q-rsrc/file/..namedfork/rsrc"), PATTERN * 300)
      tree.merge([kept, [path, view.with(extra: true)]].to_h)
    end

    def hard_link(tree)
      kept = folder("q-link")
      first, view = write("q-link/first", PATTERN * 3)
      File.link(File.join(@root, "q-link/first"), File.join(@root, "q-link/second"))
      tree.merge([kept, [first, view], ["/q-link/second".b, view]].to_h)
    end

    def compress(tree)
      kept = folder("q-comp")
      tree.merge(kept.first => kept.last).merge(COMPRESSED.to_h { |name, bytes| compress_file(name, bytes) })
    end

    def compress_file(name, bytes)
      source = File.join(@scratch, "compress-#{name}")
      File.binwrite(source, bytes)
      target = File.join(@root, "q-comp", name)
      tool("ditto", "--hfsCompression", source, target)
      flags = tool("stat", "-f", "%Sf", target)
      raise Error, "ditto left #{name} uncompressed: #{flags.strip}" unless @limited || flags.include?("compressed")

      ["/q-comp/#{name}".b, file_view(bytes)]
    end

    def rename_original(tree, path)
      File.rename(full(path), File.join(@root, "q-renamed"))
      tree.except(path).merge("/q-renamed".b => tree.fetch(path))
    end

    def unlink_original(tree, path)
      File.unlink(full(path))
      tree.except(path)
    end

    def link_original(tree, path)
      File.link(full(path), File.join(@root, "q-original-link"))
      tree.merge("/q-original-link".b => tree.fetch(path))
    end

    def attribute_original(tree, path)
      tool("xattr", "-w", "org.fsimg.original", "value", full(path))
      tree.merge(path => tree.fetch(path).with(extra: true))
    end

    # A native tool's failure: ENOSPC if it says so, for the step to judge.
    def tool(*argv)
      out, ok = @log.run(*argv)
      raise Errno::ENOSPC, argv.first if !ok && out.match?(/no space left/i)
      raise Error, "#{argv.first} failed: #{out}" unless ok

      out
    end
  end

  # Modifications and reads change these, whatever the volume then shows.
  def relaxed(tree) = tree.transform_values { _1.with(changed: nil, accessed: nil) }

  # Tool output as text for the record, whatever bytes it held.
  def text(bytes) = bytes.dup.force_encoding("UTF-8").scrub

  def native_extra?(relative)
    first = relative.dup.force_encoding("UTF-8").delete_prefix("/").split("/", 2).first
    NATIVE_ROOT.include?(first) || PRIVATE_ROOT.include?(first)
  end

  KINDS = { "link" => "symlink", "characterSpecial" => "char", "blockSpecial" => "block" }.freeze

  def view_of(full, digests:)
    st = File.lstat(full)
    kind = KINDS.fetch(st.ftype, st.ftype)
    View.of(kind: kind, size: kind == "file" ? st.size : nil, mode: st.mode & 0o7777, uid: st.uid, gid: st.gid,
            created: st.birthtime.to_i, modified: st.mtime.to_i, changed: st.ctime.to_i, accessed: st.atime.to_i,
            target: kind == "symlink" ? File.readlink(full).b : nil,
            sha256: digests && kind == "file" ? C.file_digest(full) : nil)
  end

  # A `t --json` entry record; its symlink target is not listed, so nil there
  # leaves the expected target uncompared by `differences`.
  def view_of_record(record)
    kind = record.fetch("kind")
    View.of(kind: kind, size: kind == "file" ? record.fetch("size_bytes").to_i : nil, mode: record.fetch("mode") & 0o7777,
            uid: record.fetch("uid"), gid: record.fetch("gid"), created: seconds(record, "created"),
            modified: seconds(record, "modified"), changed: seconds(record, "changed"), accessed: seconds(record, "accessed"),
            extra: !record.fetch("extra_data").nil?)
  end

  def seconds(record, field) = record.fetch(field).fetch("seconds").to_i

  # Each difference between the expected tree and an observed one, in the given
  # fields. A field expected as nil is not compared; `target` is compared only
  # where the observed view lists it.
  def differences(expected, observed, fields)
    missing = (expected.keys - observed.keys).map { "#{_1.inspect} missing" }
    extra = (observed.keys - expected.keys).map { "#{_1.inspect} unexpected" }
    changed = expected.flat_map do |path, want|
      got = observed[path]
      next [] unless got

      fields.filter_map do |field|
        wanted = want.public_send(field)
        seen = got.public_send(field)
        next if wanted.nil? || (field == :target && seen.nil?)

        "#{path.inspect} #{field} #{seen.inspect}, not #{wanted.inspect}" unless wanted == seen
      end
    end
    missing + extra + changed
  end

  # The disk images attached from under `root`, by `hdiutil info`.
  def leftovers(root)
    out, = C::Log.new.run("hdiutil", "info")
    out.lines.filter_map { _1[/\Aimage-path\s*:\s*(.*)$/, 1] }.select { _1.start_with?(root) }
  end

  def result_table(rows)
    columns = Cycle::STEPS + [:detach]
    header = "| Image | #{columns.join(' | ')} | Refused |"
    rule = "|#{' --- |' * (columns.size + 2)}"
    lines = rows.map do |row|
      cells = columns.map { row.fetch(_1)[0, 60] }
      "| #{row.fetch(:image)} | #{cells.join(' | ')} | #{row.fetch(:refused).map { _1.split(':').first }.join(', ')} |"
    end
    [header, rule, *lines].join("\n")
  end

  def parse(argv)
    options = { only: nil }
    OptionParser.new do |o|
      o.on("--generator PATH", "the built hfsplus-write-gen") { options[:generator] = File.expand_path(_1) }
      o.on("--fsimg PATH", "the built fsimg") { options[:fsimg] = File.expand_path(_1) }
      o.on("--root DIR", "a disposable directory for the images") { options[:root] = File.expand_path(_1) }
      o.on("--only REGEX", "only images whose name matches") { options[:only] = Regexp.new(_1) }
      o.on("--large-catalog", "only the volume whose catalog passes 2^32 bytes") { options[:large_catalog] = true }
      o.on("--keep", "keep each image directory") { options[:keep] = true }
      o.on("--list", "print the image names and stop") { options[:list] = true }
    end.parse!(argv)
    options
  end

  # The volume that closes the clumpSize saturation, which takes hours and
  # about 8 GiB of disk, runs only when asked for.
  def large_catalog =
    Image.of(name: "hfsplus-catalog-4gib", variant: "hfsplus", block: 4096, volume_bytes: LARGE_CATALOG_BYTES,
             entries: map_node_entries(LARGE_CATALOG_FILES))

  def selected(options)
    return [large_catalog] if options[:large_catalog]

    VARIANTS.flat_map { images(_1) }.select { options[:only].nil? || options[:only].match?(_1.name) }
  end

  def main(argv)
    options = parse(argv)
    chosen = selected(options)
    if options[:list]
      puts chosen.map(&:name)
      return 0
    end
    root = options.fetch(:root)
    tools = Tools.new(generator: options.fetch(:generator), fsimg: options.fetch(:fsimg))
    FileUtils.mkdir_p(root)
    rows = chosen.map do |image|
      directory = File.join(root, image.name)
      FileUtils.rm_rf(directory)
      FileUtils.mkdir_p(directory)
      row = Cycle.new(image, tools, directory).run
      puts row.slice(:image, *Cycle::STEPS, :refused).to_json
      FileUtils.rm_rf(File.join(directory, "image.img")) unless options[:keep]
      row
    end
    File.write(File.join(root, "results.json"), JSON.pretty_generate(rows))
    puts result_table(rows)
    remaining = leftovers(root)
    warn "still attached: #{remaining.join(', ')}" unless remaining.empty?
    rows.all? { |row| (Cycle::STEPS + [:detach]).all? { row.fetch(_1) == "pass" } } && remaining.empty? ? 0 : 1
  end
end

exit HfsPlusQualify.main(ARGV) if $PROGRAM_NAME == __FILE__
