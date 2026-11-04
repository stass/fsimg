#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Observe how the native macOS HFS Plus driver stores, compares and sorts
# every Unicode value, on a case-insensitive HFS Plus volume and a
# case-sensitive HFSX volume, and write what it saw as tables that the
# generated fold and decomposition tables are compared against row for row.
#
# A run formats a disposable sparse volume per comparator, mounts it, and
# creates one empty file per probe name with O_CREAT | O_EXCL. Each name set
# gets a directory per pass: `SET.0` holds every name that could be created,
# and a name that collided with an earlier one is retried in `SET.1`, then
# `SET.2`, until nothing collides, so that the stored form of every member of
# a collision class is seen as well as the class. The volume is unmounted,
# checked with `fsck_hfs -n`, and its catalog leaves are read back: they hold
# the units actually stored and the order the driver sorted them in.
#
# Tables, in OUT_ROOT/e3-VARIANT/, tab separated, one `#` header line:
#
#   SET.tsv (alone, between, pairs, hangul, surrogates)
#     id              hex scalar value, `+` between the values of a sequence
#     scope           bmp, supp (Unicode 3.2 assigns it), coverage (unassigned,
#                     kept to reach every surrogate unit), pair, hangul,
#                     hangul-jamo, seed or surrogate
#     disposition     created, EEXIST or another errno name, in pass 0;
#                     path-terminator for NUL, which no syscall can carry
#     representative  self, or what it collided with in pass 0: an id, `dir`
#                     for its own directory or `parent` for the one above
#     pass            the pass whose directory holds the name, or `-`
#     readdir_units   the UTF-16 units of the readdir name, `.` separated
#     leaf_units      the UTF-16 units of the catalog key
#     rank            0-based position among the directory's catalog children
#
#   fold.tsv          one row per UTF-16 unit, see `fold_rows`
#   differences.tsv   every disagreement with the Unicode 3.2.0 prediction
#   residual.tsv      what the prediction plus the observed rules leaves unexplained
#   volume.json       comparator, tree shape, fsck verdicts, per-set directories
#   summary.json      counts per set and per difference kind
#   commands.txt      the commands run
#   SHA256SUMS        the digest of each file above
#
# `--small-names` instead writes OUT_ROOT/e3-small-names/: one small volume per
# probe name for `fsck_hfs -n` verdicts and stored U+0000 (fsck.tsv), and one
# volume per comparator for marks that decompose into marks (class0-VARIANT.tsv,
# class0-differences.tsv), see `run_small_names`.
#
# Usage: unicode-probe.rb [--variant hfsplus|hfsx] [--analyze-only] [--sample N]
#                         [--overwrite] [--small-names]
require "digest"
require "fileutils"
require "json"
require "optparse"
require_relative "capture"

module HfsPlusUnicodeProbe
  class Error < StandardError; end

  UNICODE_DATA_SHA256 = "5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446"
  DEFAULT_DATA = File.join(Dir.home, "hfsplus-captures/unicode-3.2.0/UnicodeData-3.2.0.txt")
  DEFAULT_ROOT = File.join(Dir.home, "hfsplus-captures")
  DEFAULT_WORK = File.join(Dir.home, ".cache/fsimg-hfs/e3")
  VOLUME_BYTES = 8 << 30
  BLOCK_BYTES = 4096
  CATALOG_CLUMP_BLOCKS = 256 * 1024 # 1 GiB, one extent: fields.rb reads only the first eight.
  VARIANTS = { "hfsplus" => [], "hfsx" => ["-s"] }.freeze
  SURROGATES = (0xD800..0xDFFF)
  HIGH_SURROGATES = (0xD800..0xDBFF)
  LOW_SURROGATES = (0xDC00..0xDFFF)
  HANGUL = (0xAC00..0xD7A3)
  COLUMNS = %w[id scope disposition representative pass readdir_units leaf_units rank].freeze
  # POSIX spellings the system stores as another unit: the Mac path separator
  # and the visible stand-in for U+0000.
  SYSTEM_MAPPED = { 0x3A => 0x2F, 0x2400 => 0x0000 }.freeze
  # Valid scalar values the system stores as `%XX` escapes of their UTF-8.
  NONCHARACTERS_ESCAPED = (0xFFFE..0xFFFF)
  # The only probe names `fsck_hfs -n` calls illegal; removing them lets its
  # verdict and its key order check speak for every other name.
  FSCK_ILLEGAL_NAMES = ["pairs.0/a\u0306\u0307", "pairs.0/a\u0308\u030D"].freeze
  # Representatives that are not another probe name.
  NOT_A_NAME = %w[self dir parent - ?].freeze
  ASCII_A = 0x61
  ASCII_B = 0x62

  # The `between` names the system may refuse, by value: U+0000 cannot reach a
  # C path, and `a/b` names a directory that does not exist.
  BETWEEN_REFUSALS = { 0x0000 => "path-terminator", 0x2F => "ENOENT" }.freeze
  SEED_ID = "ab"
  SUPPLEMENTARY_SCOPES = %w[supp coverage].freeze
  # Volumes for `--small-names`: one name each, so a verdict belongs to one name.
  SMALL_VOLUME_BYTES = 8 << 20

  Config = Data.define(:unicode_data, :root, :work, :variants, :sample, :analyze_only, :keep_image, :overwrite, :small_names)

  # One probe: the bytes given to the system, and the scalar values they spell.
  Item = Data.define(:id, :scope, :cps, :name)
  # One attempt to create a file: its inode, or the name of the failure.
  Attempt = Data.define(:ino, :failure)
  # What the driver did with one probe. Pass 0 decides the disposition and
  # representative; `pass` and `ino` say where the probe was finally created.
  Outcome = Data.define(:disposition, :representative, :pass, :ino)
  # One catalog leaf record of a file or folder.
  Leaf = Data.define(:parent, :units, :rank)
  # One row of a SET.tsv file.
  Row = Data.define(*COLUMNS.map(&:to_sym)) do
    def cells = to_h.values
  end

  # One row of fold.tsv; `unit` is the table index and the other fields are text.
  FoldRow = Data.define(:unit, :status, :fold, :chain) do
    def cells = [HfsPlusUnicodeProbe.hex(unit), status, fold, chain]
  end

  # One row of differences.tsv and residual.tsv.
  Difference = Data.define(:set, :id, :check, :predicted, :observed) do
    def cells = to_h.values
  end

  # One collision class on the catalog chain: its position (`rank`), the stored
  # units its names show (`members`, ascending) and what order leaves of its fold.
  ChainClass = Data.define(:rank, :members, :status, :fold)

  module_function

  # ---- Unicode 3.2.0 data ----

  # The fields of the pinned file a prediction needs. `assigned` includes the
  # values of its First/Last ranges, which carry no properties of their own.
  class UnicodeTables
    attr_reader :assigned, :ccc

    def initialize(path)
      bytes = File.binread(path)
      digest = Digest::SHA256.hexdigest(bytes)
      raise Error, "#{path} has digest #{digest}, not the pinned #{UNICODE_DATA_SHA256}" unless digest == UNICODE_DATA_SHA256

      @assigned = []
      @ccc = {}
      @canonical = {}
      @lower = {}
      range_first = nil
      bytes.force_encoding("US-ASCII").each_line(chomp: true) do |line|
        code, name, _, ccc, _, decomposition, _, _, _, _, _, _, _, lower = line.split(";", -1)
        cp = code.to_i(16)
        if name.end_with?(", First>")
          range_first = cp
        elsif name.end_with?(", Last>")
          @assigned.concat((range_first..cp).to_a)
        else
          @assigned << cp
          @ccc[cp] = ccc.to_i unless ccc == "0"
          @canonical[cp] = decomposition.split.map { _1.to_i(16) } unless decomposition.empty? || decomposition.start_with?("<")
          @lower[cp] = lower.to_i(16) unless lower.empty?
        end
      end
    end

    def marks = @ccc.keys.sort
    def class_of(cp) = @ccc.fetch(cp, 0)
    def canonical(cp) = @canonical[cp]
    def lower(cp) = @lower[cp]
  end

  def hex(cp) = format("%04X", cp)
  def utf16(cps) = cps.flat_map { _1 > 0xFFFF ? [0xD800 + ((_1 - 0x10000) >> 10), 0xDC00 + ((_1 - 0x10000) & 0x3FF)] : [_1] }
  def units_text(units) = units.empty? ? "-" : units.map { hex(_1) }.join(".")
  def parse_units(text) = text == "-" ? [] : text.split(".").map { _1.to_i(16) }
  def hangul_jamo(cp)
    index = cp - HANGUL.first
    trailing = index % 28
    [0x1100 + index / 588, 0x1161 + (index % 588) / 28, *(trailing.zero? ? [] : [0x11A7 + trailing])]
  end

  # ---- probe names ----

  def item(cps:, scope:, id: cps.map { hex(_1) }.join("+")) = Item.new(id: id, scope: scope, cps: cps, name: cps.pack("U*").b)

  # Every value of the probe scope with its scope label: all BMP scalar values,
  # the supplementary values the data assigns, and one more value for each high
  # and each low surrogate unit no assigned value reaches.
  def scoped_values(data)
    bmp = (0..0xFFFF).reject { SURROGATES.cover?(_1) }.map { [_1, "bmp"] }
    supp = data.assigned.select { _1 > 0xFFFF }
    assigned = supp.to_set
    every_unit = (0...1024).flat_map { [0x10000 + (_1 << 10), 0x10000 + _1] }.uniq.sort
    bmp + (supp.map { [_1, "supp"] } + every_unit.reject { assigned.include?(_1) }.map { [_1, "coverage"] }).sort
  end

  # The sets, in creation order. Ascending values make a class's representative
  # its smallest member. The seed `ab` is what an ignorable `aXb` collides with.
  def probe_sets(data, sample)
    values = scoped_values(data)
    marks = data.marks
    sets = {
      "alone" => values.map { |cp, scope| item(cps: [cp], scope: scope) },
      "between" => [item(id: SEED_ID, cps: [ASCII_A, ASCII_B], scope: "seed")] +
        values.map { |cp, scope| item(id: hex(cp), cps: [ASCII_A, cp, ASCII_B], scope: scope) },
      "pairs" => marks.product(marks).map { |m1, m2| item(id: "#{hex(m1)}+#{hex(m2)}", cps: [ASCII_A, m1, m2], scope: "pair") },
      "hangul" => HANGUL.flat_map { [item(cps: [_1], scope: "hangul"), item(cps: hangul_jamo(_1), scope: "hangul-jamo")] },
      "surrogates" => SURROGATES.map { item(cps: [_1], scope: "surrogate") }
    }
    sample == 1 ? sets : sets.transform_values { |items| items.each_slice(sample).map(&:first) }
  end

  # ---- the mounted volume ----

  # Create one empty file. A NUL ends a C path, so a name holding one cannot
  # reach the system at all.
  def create(path)
    io = IO.new(IO.sysopen(path, File::WRONLY | File::CREAT | File::EXCL, 0o644))
    ino = io.stat.ino
    io.close
    Attempt.new(ino: ino, failure: nil)
  rescue SystemCallError => e
    Attempt.new(ino: nil, failure: e.class.name.split("::").last)
  rescue ArgumentError
    Attempt.new(ino: nil, failure: "path-terminator")
  end

  # Which existing thing an unsuccessful name resolves to: an earlier name of
  # this directory by inode, the directory itself, or the one above.
  def collided_with(path, created, own, parent)
    ino = File.lstat(path).ino
    created[ino] || { own => "dir", parent => "parent" }.fetch(ino, "?")
  rescue SystemCallError, ArgumentError
    "-"
  end

  # One pass in the current directory; returns the items to retry in the next,
  # those that collided with a name this pass created. Pass 0 decides each
  # probe's disposition and representative: a later pass only records where a
  # retried probe was finally created.
  def attempt_pass(pending, pass, outcomes)
    own = File.stat(".").ino
    parent = File.stat("..").ino
    created = {}
    retry_next = []
    pending.each do |probe|
      path = "./".b + probe.name
      attempt = create(path)
      if attempt.ino
        created[attempt.ino] = probe.id
        decided = pass.zero? ? Outcome.new(disposition: "created", representative: "self", pass: nil, ino: nil) : outcomes.fetch(probe.id)
        outcomes[probe.id] = decided.with(pass: pass, ino: attempt.ino)
      else
        collision = collided_with(path, created, own, parent)
        outcomes[probe.id] = Outcome.new(disposition: attempt.failure, representative: collision, pass: nil, ino: nil) if pass.zero?
        retry_next << probe if attempt.failure == "EEXIST" && !NOT_A_NAME.include?(collision)
      end
    end
    retry_next
  end

  def probe_set(set, items)
    outcomes = {}
    directories = {}
    pending = items
    pass = 0
    until pending.empty?
      directory = "#{set}.#{pass}"
      Dir.mkdir(directory)
      directories[directory] = File.stat(directory).ino
      pending = Dir.chdir(directory) { attempt_pass(pending, pass, outcomes) }
      pass += 1
    end
    [outcomes, directories]
  end

  # The names of a directory as the system returns them, in its order, and those
  # the system cannot then find again by the name it returned. Ruby's Dir class
  # composes the names it reads from an HFS volume, so find(1) reads them here.
  def read_listing(directory)
    output, status = Open3.capture2("find", ".", "-mindepth", "1", "-maxdepth", "1", "-print0", chdir: directory, binmode: true)
    raise Error, "find failed in #{directory}" unless status.success?

    names = output.split("\x00".b).map { _1.delete_prefix("./") }
    lost = Dir.chdir(directory) { names.reject { found?("./".b + _1) } }
    [names.map { name_units(_1) }, lost.map { name_units(_1) }]
  end

  def found?(path)
    File.lstat(path)
    true
  rescue SystemCallError
    false
  end

  def name_units(name) = utf16(name.dup.force_encoding(Encoding::UTF_8).unpack("U*"))

  # A directory that is not a mount point would take the probe names onto the
  # host's own volume, so it is refused before any name is created.
  def require_mount_point(mount)
    return if File.stat(mount).dev != File.stat(File.dirname(mount)).dev

    raise Error, "#{mount} is not a mount point"
  end

  def observe_volume(mount, sets)
    require_mount_point(mount)
    Dir.chdir(mount) do
      File.write(".metadata_never_index", "")
      sets.to_h do |set, items|
        started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        outcomes, directories = probe_set(set, items)
        listings = directories.to_h { |directory, ino| [ino, read_listing(directory)] }
        seconds = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started).round(1)
        warn "#{set}: #{items.size} names in #{seconds} s"
        [set, { outcomes: outcomes, directories: directories, listings: listings, seconds: seconds }]
      end
    end
  end

  # ---- the catalog leaves ----

  # Every file and folder record by node ID, with its rank among its parent's
  # children in leaf order, read through the fields.rb primitives.
  def read_leaves(image)
    File.open(image, "rb") do |io|
      unsized = HfsPlusFields::Volume.new(image: HfsPlusFields::Image.new(io), base: 0, block_size: BLOCK_BYTES)
      header = HfsPlusFields.read_header(unsized, 1024)
      volume = unsized.with(block_size: header[:block_size])
      fork = header[:forks].fetch(:catalog)
      tree = HfsPlusFields.tree_header_report(volume, fork)
      leaves, records = walk_leaf_chain(volume, fork, tree)
      raise Error, "the leaf chain holds #{records} records, the tree header says #{tree[:leaf_records]}" unless records == tree[:leaf_records]

      extents = fork.extents.count { _1.count.positive? }
      info = { signature: header[:signature], key_compare_type: format("0x%02X", tree[:key_compare_type]), node_size: tree[:node_size],
               depth: tree[:depth], leaf_records: tree[:leaf_records], total_nodes: tree[:total_nodes], free_nodes: tree[:free_nodes],
               catalog_extents: extents, catalog_bytes: fork.logical_size, file_count: header[:file_count], folder_count: header[:folder_count] }
      [leaves, info]
    end
  end

  # The leaves by node ID, and the number of leaf records of every kind
  # (threads included), which the tree header also counts.
  def walk_leaf_chain(volume, fork, tree)
    leaves = {}
    ranks = Hash.new(0)
    seen = Set.new
    records = 0
    node = tree[:first_leaf]
    until node.zero?
      raise Error, "leaf chain loops at #{node}" unless seen.add?(node)

      raw = HfsPlusFields.read_node(volume, fork, node, tree[:node_size])
      desc = HfsPlusFields.descriptor(raw)
      raise Error, "node #{node} on the leaf chain has kind #{desc.kind}" unless desc.kind == -1

      records += desc.num_records
      HfsPlusFields.record_slices(raw, desc.num_records).each do |slice|
        parent, _, data_at = HfsPlusFields.catalog_key(slice)
        type, _, _, cnid = slice.byteslice(data_at, 12).unpack("nnNN")
        next unless [1, 2].include?(type)

        # catalog_key has checked the name length: the key's units end at data_at.
        leaves[cnid] = Leaf.new(parent: parent, units: slice.byteslice(8, data_at - 8).unpack("n*"), rank: ranks[parent])
        ranks[parent] += 1
      end
      node = desc.flink
    end
    [leaves, records]
  end

  # ---- writing the observation ----

  # Sampled runs measure only; they never share a directory with the evidence.
  def out_dir(cfg, variant) = File.join(cfg.root, "e3-#{variant}#{cfg.sample == 1 ? "" : "-sample#{cfg.sample}"}")

  def set_rows(items, observed, leaves)
    items.map do |probe|
      outcome = observed[:outcomes].fetch(probe.id)
      leaf = leaves[outcome.ino]
      raise Error, "#{probe.id} was created but has no catalog leaf" if outcome.ino && !leaf

      listed = leaf && observed[:listings].dig(leaf.parent, 0, leaf.rank)
      raise Error, "#{probe.id} has no readdir entry at rank #{leaf.rank}" if leaf && !listed

      Row.new(id: probe.id, scope: probe.scope, disposition: outcome.disposition, representative: outcome.representative,
              pass: outcome.pass || "-", readdir_units: listed ? units_text(listed) : "-",
              leaf_units: leaf ? units_text(leaf.units) : "-", rank: leaf ? leaf.rank : "-")
    end
  end

  def write_tsv(path, columns, rows)
    File.open(path, "w") do |file|
      file.write("# #{columns.join("\t")}\n")
      rows.each { file.write("#{_1.cells.join("\t")}\n") }
    end
  end

  # Directories whose readdir count differs from their catalog children, which
  # would make the join of listing position and leaf rank meaningless.
  def listing_count_mismatches(observed, leaves)
    counts = leaves.values.map(&:parent).tally
    observed[:listings].reject { |parent, (names, _)| counts[parent] == names.size }.keys
  end

  def fsck_verdict(attachments, log, image)
    attachments.with(image) do |device|
      output, ok = log.run("fsck_hfs", "-d", "-n", device.sub("/dev/disk", "/dev/rdisk"))
      { ok: ok, illegal_names: output.lines.grep(/illegal name is|replacement name is/).map(&:strip), last_line: output.lines.last.to_s.strip }
    end
  end

  # What the tables rest on. A volume that fails one of these is not evidence,
  # so each failure stops the run before anything is written.
  def check_integrity(observed, leaves, clean)
    problems = readdir_problems(observed, leaves)
    problems << "fsck_hfs -n still fails after the illegal names are removed: #{clean[:last_line]}" unless clean[:ok]
    raise Error, problems.join("; ") unless problems.empty?
  end

  # Where what readdir returned cannot be joined to the catalog leaves.
  def readdir_problems(observed, leaves)
    observed.each_with_object([]) do |(set, o), problems|
      mismatched = listing_count_mismatches(o, leaves)
      lost = o[:listings].values.flat_map(&:last)
      problems << "#{set}: readdir lists a different number of names than the catalog holds in #{mismatched.size} directories" unless mismatched.empty?
      problems << "#{set}: #{lost.size} names readdir returned cannot be found again, such as #{units_text(lost.first)}" unless lost.empty?
    end
  end

  # ---- one disposable volume ----

  def format_volume(log, attachments, image, variant, bytes:, catalog_clump: nil)
    raise Error, "cannot create #{image}" unless log.run("mkfile", "-n", bytes.to_s, image).last

    attachments.with(image) do |device|
      raw = device.sub("/dev/disk", "/dev/rdisk")
      clump = catalog_clump ? ["-c", "c=#{catalog_clump}"] : []
      raise Error, "newfs_hfs failed on #{raw}" unless log.run("newfs_hfs", *VARIANTS.fetch(variant), "-v", "E3", "-b", BLOCK_BYTES.to_s, *clump, raw).last
    end
  end

  def remove_illegal_names(mount)
    FSCK_ILLEGAL_NAMES.map { File.join(mount, _1) }.each { File.unlink(_1) if File.exist?(_1) }
  end

  def refuse_existing_capture(out, cfg)
    return if cfg.overwrite || !Dir.exist?(out) || Dir.empty?(out)

    raise Error, "#{out} already holds a capture; --overwrite replaces it"
  end

  # Releases every device this run attached, keeps the commands run and removes
  # the scratch files. It returns a failed release instead of raising it, so
  # that it never hides the error already on its way out.
  def finish_run(attachments, log, out, images, mount, keep_image:)
    leftover = begin
      attachments.release
      nil
    rescue HfsPlusCapture::Error => e
      warn "unicode-probe: #{e.message}"
      e
    end
    File.write(File.join(out, "commands.txt"), log.to_s)
    FileUtils.rm_f(images) unless keep_image
    remove_mount_directory(mount) unless leftover
    leftover
  end

  # Never rm_rf: a mount point that is still mounted would lose the volume's files.
  def remove_mount_directory(mount)
    Dir.rmdir(mount)
  rescue SystemCallError => e
    warn "unicode-probe: cannot remove #{mount}: #{e.message}"
  end

  # Format, probe, check and read back one volume; write its tables.
  def capture_volume(cfg, variant, sets, log, attachments, image, mount, out)
    format_volume(log, attachments, image, variant, bytes: VOLUME_BYTES, catalog_clump: CATALOG_CLUMP_BLOCKS)
    observed = attachments.with(image, mountpoint: mount) { observe_volume(mount, sets) }
    verdict = fsck_verdict(attachments, log, image)
    # The leaves are read before the mount that removes the two illegal names:
    # that mount rewrites the catalog, and the ranks and inodes recorded while
    # probing belong to the catalog as the probe left it.
    leaves, info = read_leaves(image)
    attachments.with(image, mountpoint: mount) { remove_illegal_names(mount) }
    clean = fsck_verdict(attachments, log, image)
    check_integrity(observed, leaves, clean)
    sets.each { |set, items| write_tsv(File.join(out, "#{set}.tsv"), COLUMNS, set_rows(items, observed.fetch(set), leaves)) }
    File.write(File.join(out, "volume.json"), JSON.pretty_generate(
      variant: variant, sample: cfg.sample, image_allocated_bytes: File.stat(image).blocks * 512, tree: info,
      fsck: verdict, fsck_without_illegal_names: clean,
      sets: observed.transform_values do |o|
        { directories: o[:directories].keys, seconds: o[:seconds], listing_count_mismatches: listing_count_mismatches(o, leaves),
          readdir_names_not_found: o[:listings].values.flat_map(&:last).map { units_text(_1) } }
      end
    ))
  end

  def run_volume(cfg, variant, sets)
    log = HfsPlusCapture::Log.new
    attachments = HfsPlusCapture::Attachments.new(log)
    image = File.join(cfg.work, "#{variant}.img")
    mount = File.join(cfg.work, "#{variant}.mnt")
    out = out_dir(cfg, variant)
    refuse_existing_capture(out, cfg)
    FileUtils.mkdir_p([cfg.work, mount, out])
    leftover = nil
    begin
      capture_volume(cfg, variant, sets, log, attachments, image, mount, out)
    ensure
      leftover = finish_run(attachments, log, out, [image], mount, keep_image: cfg.keep_image)
    end
    raise leftover if leftover
  end

  # ---- the Unicode 3.2.0 prediction ----

  # What specs/design-hfsplus.md says the driver does, computed from the pinned
  # data: the disagreements are the candidates for the exceptions file.
  class Prediction
    UNCHANGED_DECOMPOSITION = [0x2000..0x2FFF, 0xF900..0xFAFF, 0x2F800..0x2FAFF].freeze
    FOLDED_PAGES = [0x00, 0x01, 0x03, 0x04, 0x05, 0x10, 0x21, 0xFF].freeze
    IGNORABLE = [0x200C..0x200F, 0x202A..0x202E, 0x206A..0x206F, 0xFEFF..0xFEFF].freeze
    GEORGIAN_CAPITALS = (0x10A0..0x10C5)
    NUL_SORT_KEY = 0x10000 # After every unit of a folded key.

    def initialize(data, folded:)
      @data = data
      @folded = folded
    end

    def stored(cps) = HfsPlusUnicodeProbe.utf16(canonical_order(cps.flat_map { decompose(_1) }))

    # Whether the driver stores something other than the value itself.
    def decomposes?(cp) = HANGUL.cover?(cp) || (!unchanged?(cp) && !@data.canonical(cp).nil?)
    def sort_key(units) = units.filter_map { fold(_1) }

    # The design's rule that an all-ignorable name folds onto its parent's thread.
    def meets_thread?(key) = key.empty?

    # Nil when the unit is ignorable.
    def fold(unit)
      return unit unless @folded
      return NUL_SORT_KEY if unit.zero?
      return nil if IGNORABLE.any? { _1.cover?(unit) }
      return unit - GEORGIAN_CAPITALS.first + 0x10D0 if GEORGIAN_CAPITALS.cover?(unit)

      FOLDED_PAGES.include?(unit >> 8) ? @data.lower(unit) || unit : unit
    end

    private

    def combining_class(cp) = @data.class_of(cp)
    def unchanged?(cp) = UNCHANGED_DECOMPOSITION.any? { _1.cover?(cp) }

    def decompose(cp)
      return HfsPlusUnicodeProbe.hangul_jamo(cp) if HANGUL.cover?(cp)
      return [cp] if unchanged?(cp)

      mapped = @data.canonical(cp)
      mapped ? mapped.flat_map { decompose(_1) } : [cp]
    end

    # Stable sort of each run of nonzero combining classes.
    def canonical_order(cps)
      cps.chunk_while { combining_class(_1).positive? == combining_class(_2).positive? }.flat_map do |run|
        next run if combining_class(run.first).zero?

        run.each_with_index.sort_by { |cp, index| [combining_class(cp), index] }.map(&:first)
      end
    end
  end

  # The prediction plus every rule the observation added: the system's two
  # substitutions, percent escapes for what it will not store, supplementary
  # values taken as opaque, and the fold rows that differ. What this still gets
  # wrong is unexplained, and is written to residual.tsv.
  class Explained < Prediction
    def initialize(data, folded:, fold_exceptions:)
      super(data, folded: folded)
      @fold_exceptions = fold_exceptions
    end

    def fold(unit) = @fold_exceptions.fetch(unit) { super }

    # The driver compares an empty key by length, so only a thread equals a thread.
    def meets_thread?(_key) = false

    private

    def combining_class(cp) = cp > 0xFFFF ? 0 : super

    def decompose(cp)
      return [SYSTEM_MAPPED.fetch(cp)] if SYSTEM_MAPPED.key?(cp)
      return [cp].pack("U").bytes.flat_map { format("%%%02X", _1).bytes } if SURROGATES.cover?(cp) || NONCHARACTERS_ESCAPED.cover?(cp)
      return [cp] if cp > 0xFFFF

      super
    end
  end

  # ---- comparing a stored observation ----

  def read_rows(path)
    File.foreach(path, chomp: true).drop(1).map do |line|
      fields = line.split("\t", -1)
      raise Error, "#{path}: #{fields.size} fields in #{line.inspect}" unless fields.size == COLUMNS.size

      row = Row.new(**COLUMNS.map(&:to_sym).zip(fields).to_h)
      raise Error, "#{path}: #{row.id} is created but has no leaf" if row.disposition == "created" && [row.pass, row.readdir_units, row.leaf_units, row.rank].include?("-")

      row
    end
  end

  def disagreement(set, id, check, predicted, observed) = (Difference.new(set: set, id: id, check: check, predicted: predicted, observed: observed) unless predicted == observed)

  # Replay pass 0 of a set under a prediction, which is where every probe's
  # disposition and representative were decided: the probes in creation order,
  # each colliding with the first earlier probe that has the same predicted
  # key. The later passes hold only probes that pass 0 already refused.
  def differences_of_set(set, items, rows, prediction)
    by_id = rows.to_h { [_1.id, _1] }
    first_with_key = {}
    items.flat_map do |probe|
      row = by_id.fetch(probe.id)
      stored = prediction.stored(probe.cps)
      key = prediction.sort_key(stored)
      expected = prediction.meets_thread?(key) ? "dir" : first_with_key.fetch(key, "self")
      first_with_key[key] ||= probe.id if row.disposition == "created"
      next [disagreement(set, row.id, "refused", "created", "#{row.disposition}:#{row.representative}")] unless %w[created EEXIST].include?(row.disposition)

      suffix = probe.cps.any? { _1 > 0xFFFF } ? "-supplementary" : ""
      units = disagreement(set, row.id, "units#{suffix}", units_text(stored), row.leaf_units) unless row.leaf_units == "-"
      [units, disagreement(set, row.id, "representative#{suffix}", expected, row.representative)].compact
    end
  end

  # Adjacent names of one pass directory whose predicted keys are out of order.
  def order_differences(set, rows, prediction)
    rows.reject { _1.rank == "-" }.group_by(&:pass).flat_map do |pass, group|
      group.sort_by { _1.rank.to_i }.each_cons(2).filter_map do |a, b|
        ka = prediction.sort_key(parse_units(a.leaf_units))
        kb = prediction.sort_key(parse_units(b.leaf_units))
        Difference.new(set: set, id: "#{set}.#{pass}", check: "order", predicted: "#{b.id} before #{a.id}", observed: "#{a.id} then #{b.id}") if (ka <=> kb) != -1
      end
    end
  end

  def middle_unit(row)
    units = parse_units(row.leaf_units)
    units[1] if units.size == 3 && units.first == ASCII_A && units.last == ASCII_B
  end

  # Every `between` row is created, collides with a name of the set, or is one
  # of the two refusals that are path syntax; anything else stops the analysis
  # rather than being read as an unobservable unit.
  def check_between(between)
    ids = between.to_set(&:id)
    between.each do |row|
      case row.disposition
      when "created" then nil
      when "EEXIST" then raise Error, "between: #{row.id} collided with #{row.representative}, which is not a name" unless ids.include?(row.representative)
      else raise Error, "between: #{row.id} was refused with #{row.disposition}" unless BETWEEN_REFUSALS[row.id.to_i(16)] == row.disposition
      end
    end
  end

  # The collision classes of `between` in catalog order, each as the ascending
  # stored units its names show. A class is named by its head, the pass 0 name
  # that created it, and sits where its head sits. The ignorable units are not
  # on the chain: their names collide with the seed `ab`, which stores no
  # middle unit.
  def catalog_chain(between)
    index = between.to_h { [_1.id, _1] }
    classes = between.select { middle_unit(_1) }.group_by { _1.representative == "self" ? _1.id : _1.representative }
    classes.filter_map do |head_id, members|
      next if head_id == SEED_ID

      head = index.fetch(head_id) { raise Error, "between: #{members.first.id} collided with unknown #{head_id}" }
      raise Error, "between: class head #{head_id} stores #{head.leaf_units}, not a single middle unit" unless middle_unit(head) && head.pass == "0"

      [head.rank.to_i, members.map { middle_unit(_1) }.uniq.sort]
    end.sort_by(&:first).map(&:last)
  end

  # The surrogate units that the supplementary names store, each with its group:
  # `high` or `low`. Raises unless the names are as `check_pair_names` requires.
  def pair_groups(between)
    index = between.to_h { [_1.id, _1] }
    pairs = between.select { SUPPLEMENTARY_SCOPES.include?(_1.scope) }.sort_by { _1.rank.to_i }
    stored = pairs.map { parse_units(_1.leaf_units) }
    check_pair_names(index, pairs, stored)
    stored.flat_map { [[_1[1], "high"], [_1[2], "low"]] }.to_h
  end

  # Every supplementary value must store its own pair `a H L b`, created in
  # pass 0, so that no two collide. The pairs must sort in strictly ascending
  # unit order, in one run of ranks that starts right after the name U+D7FF and
  # ends right before U+E000. And that order must force the units of each group
  # apart (adjacent lows occur under one high, adjacent highs have overlapping
  # lows), so that a table that folds two of them together cannot match.
  def check_pair_names(index, pairs, stored)
    own_pairs = pairs.all? { _1.disposition == "created" && _1.pass == "0" } &&
                stored.all? { _1.size == 4 && _1.values_at(1, 2).zip([HIGH_SURROGATES, LOW_SURROGATES]).all? { |unit, group| group.cover?(unit) } }
    raise Error, "between: a supplementary value does not store its own pair" unless own_pairs

    ranks = pairs.map { _1.rank.to_i }
    spans_between = ranks.each_cons(2).all? { _2 == _1 + 1 } && index.fetch("D7FF").rank.to_i + 1 == ranks.first && index.fetch("E000").rank.to_i - 1 == ranks.last
    raise Error, "between: the supplementary names are not one rank span between D7FF and E000" unless spans_between
    raise Error, "between: the supplementary names are not in ascending unit order" unless stored.map { _1[1, 2] }.each_cons(2).all? { (_1 <=> _2) == -1 }

    lows_under = stored.group_by { _1[1] }.transform_values { |names| names.to_set { _1[2] } }
    highs_forced = lows_under.keys.sort.each_cons(2).all? { |a, b| lows_under[a].max >= lows_under[b].min }
    lows_together = lows_under.values.flat_map { |lows| lows.select { lows.include?(_1 + 1) } }.to_set
    lows_forced = (LOW_SURROGATES.first...LOW_SURROGATES.last).all? { lows_together.include?(_1) }
    raise Error, "between: the pair order does not force the surrogate units apart" unless highs_forced && lows_forced
  end

  # One row per UTF-16 unit, 0000 to FFFF, in the terms of a 65536-entry table,
  # read from the `between` set, whose names reach `.` and `/`. Columns:
  #
  #   unit    the table index
  #   status  determined: `fold` is the only value that catalog order allows
  #                       (for a class of one member, by the axiom of `resolve_chain`)
  #           ambiguous:  `fold` lists the members of the class that catalog
  #                       order allows, `|` separated; see `chain` below
  #           ignorable:  `aXb` collides with `ab`; `fold` is 0000
  #           position:   U+0000, whose fold is known only by the unit that
  #                       sorts just before it (`after:UNIT`), or `first`
  #           pair-only:  the unit occurs in names only inside a surrogate pair;
  #                       `fold` is its group, `high` (D800-DBFF) or `low`
  #                       (DC00-DFFF); no name has one alone, so only the order
  #                       within a group is observed, never a value
  #           unobservable: no name stores this unit; `fold` says why: decomposes,
  #                       escaped or system-mapped
  #   fold    the value, `|`-separated values, `after:UNIT`, `first`, the group
  #           of a pair-only unit, or the reason
  #   chain   for determined and ambiguous rows, the position of the unit's
  #           collision class on the catalog chain, 0-based, in catalog order
  #           (U+0000's class is not on it); `-` otherwise
  #
  # What a table must satisfy to match this file: (1) all units with one chain
  # value fold to one value; (2) that value is `fold` for a determined row, and
  # one of the listed values for an ambiguous one; (3) the values of successive
  # chain positions strictly ascend. Rule 3 makes the ambiguous choices joint:
  # a row-by-row membership test is not enough, since in a class such as
  # 0410|0430 the choice must stay below the next class's (0411|0431). For
  # pair-only rows, the table's folds ascend strictly over each group in unit
  # order.
  def fold_rows(between, prediction)
    check_between(between)
    ordered = catalog_chain(between)
    # U+0000 is the one unit whose fold is not a member of its class: the
    # class [0] is the name U+2400, and its key sorts after everything
    # storable. It is placed by its neighbour, apart from the resolved chain.
    nul = ordered.index([0])
    chain = resolve_chain(ordered.reject { _1 == [0] })
    placed = chain.flat_map { |entry| entry.members.map { [_1, entry] } }.to_h
    groups = pair_groups(between)
    ignorable = between.select { _1.representative == SEED_ID }.to_set { _1.id.to_i(16) }
    (0..0xFFFF).map do |unit|
      entry = placed[unit]
      if unit.zero? && nul
        # The largest member names the class just before it, whatever its size.
        FoldRow.new(unit: unit, status: "position", fold: nul.zero? ? "first" : "after:#{hex(ordered[nul - 1].last)}", chain: "-")
      elsif ignorable.include?(unit)
        FoldRow.new(unit: unit, status: "ignorable", fold: "0000", chain: "-")
      elsif entry
        FoldRow.new(unit: unit, status: entry.status, fold: entry.fold, chain: entry.rank.to_s)
      elsif groups.key?(unit)
        FoldRow.new(unit: unit, status: "pair-only", fold: groups[unit], chain: "-")
      else
        FoldRow.new(unit: unit, status: "unobservable", fold: unobservable_reason(unit, prediction), chain: "-")
      end
    end
  end

  # The classes' folds ascend along the chain, and a class's fold is one of its
  # members (an axiom of this analysis; U+0000, whose class sorts after
  # everything but whose fold is not a member of it, shows it is not true in
  # general). Bound every class from both sides: the lowest value a class can
  # take above its predecessor's lowest, and the highest below its successor's
  # highest. A value is possible when it lies between its bounds, and a class
  # is determined when one value is. A class with one member is therefore
  # determined by the axiom alone. Several possible values are interchangeable
  # only jointly, through a monotone choice along the chain. Classes that
  # cannot ascend raise.
  def resolve_chain(member_lists)
    lows = lowest_ascending(member_lists)
    highs = lowest_ascending(member_lists.reverse.map { |members| members.reverse.map(&:-@) }).map(&:-@).reverse
    member_lists.zip(lows, highs).each_with_index.map do |(members, low, high), rank|
      possible = members.select { _1.between?(low, high) }
      raise Error, "chain class #{rank} (#{members.map { hex(_1) }.join("|")}) has no possible fold" if possible.empty?

      ChainClass.new(rank: rank, members: members, status: possible.size == 1 ? "determined" : "ambiguous", fold: possible.map { hex(_1) }.join("|"))
    end
  end

  # The smallest ascending choice: each class's lowest member above the one
  # chosen before it. Member lists are ascending.
  def lowest_ascending(member_lists)
    member_lists.each_with_index.each_with_object([]) do |(members, rank), lows|
      low = members.find { _1 > (lows.last || -Float::INFINITY) }
      raise Error, "chain class #{rank} (#{members.map { hex(_1) }.join("|")}) cannot ascend above the classes before it" unless low

      lows << low
    end
  end

  def unobservable_reason(unit, prediction)
    return "escaped" if NONCHARACTERS_ESCAPED.cover?(unit)
    return "system-mapped" if SYSTEM_MAPPED.key?(unit)
    return "decomposes" if prediction.decomposes?(unit)

    raise Error, "U+#{hex(unit)} is stored by no name and no rule explains why"
  end

  # Fold rows that disagree with a prediction, row by row.
  def fold_row_differences(fold, prediction)
    fold.filter_map do |row|
      predicted = hex(prediction.fold(row.unit) || 0)
      agrees = case row.status
               when "determined", "ignorable" then predicted == row.fold
               when "ambiguous" then row.fold.split("|").include?(predicted)
               else true
               end
      Difference.new(set: "fold", id: hex(row.unit), check: "fold", predicted: predicted, observed: row.fold) unless agrees
    end
  end

  # The joint rule of `fold_rows`: every chain class folds to one value, and the
  # values ascend along the chain. Row-by-row agreement does not imply it.
  def chain_differences(fold, prediction)
    classes = fold.reject { _1.chain == "-" }.group_by { _1.chain.to_i }.sort.map { |rank, rows| [rank, rows.map { prediction.fold(_1.unit) || 0 }.uniq] }
    split = classes.select { _2.size > 1 }.map { |rank, values| Difference.new(set: "fold", id: "chain:#{rank}", check: "chain", predicted: values.map { hex(_1) }.join("|"), observed: "one value") }
    descents = classes.select { _2.size == 1 }.each_cons(2).select { |(_, a), (_, b)| a.first >= b.first }.map do |(ra, a), (rb, b)|
      Difference.new(set: "fold", id: "chain:#{ra},#{rb}", check: "chain", predicted: "#{hex(a.first)} then #{hex(b.first)}", observed: "ascending")
    end
    split + descents
  end

  def fold_differences(fold, prediction) = fold_row_differences(fold, prediction) + chain_differences(fold, prediction)

  def all_differences(sets, rows, fold, prediction)
    sets.flat_map do |set, items|
      differences_of_set(set, items, rows.fetch(set), prediction) + order_differences(set, rows.fetch(set), prediction)
    end + fold_differences(fold, prediction)
  end

  def ranges_text(units)
    units.sort.slice_when { _2 != _1 + 1 }.map { _1.size == 1 ? hex(_1.first) : "#{hex(_1.first)}-#{hex(_1.last)}" }
  end

  def set_summary(rows)
    resolved_to = ->(row) { NOT_A_NAME.include?(row.representative) ? row.representative : "name" }
    {
      rows: rows.size,
      by_scope: rows.map(&:scope).tally,
      dispositions: rows.map { "#{_1.disposition}:#{resolved_to[_1]}" }.tally,
      collision_classes: rows.map(&:representative).uniq.count { !NOT_A_NAME.include?(_1) },
      deepest_pass: rows.map(&:pass).reject { _1 == "-" }.map(&:to_i).max,
      readdir_differs_from_leaf: rows.count { _1.readdir_units != _1.leaf_units }
    }
  end

  # Rewrite every derived table from the SET.tsv files of `out`.
  def analyze(cfg, variant, sets, data)
    out = out_dir(cfg, variant)
    folded = variant == "hfsplus"
    rows = sets.keys.to_h { [_1, read_rows(File.join(out, "#{_1}.tsv"))] }
    fold = fold_rows(rows.fetch("between"), Prediction.new(data, folded: folded))
    write_tsv(File.join(out, "fold.tsv"), %w[unit status fold chain], fold)
    differences = all_differences(sets, rows, fold, Prediction.new(data, folded: folded))
    write_tsv(File.join(out, "differences.tsv"), %w[set id check predicted observed], differences)
    # The first value of an ambiguous row would be an arbitrary choice; no such row disagrees.
    fold_exceptions = differences.select { _1.check == "fold" && !_1.observed.empty? }.to_h { [_1.id.to_i(16), _1.observed.split("|").first.to_i(16)] }
    residual = all_differences(sets, rows, fold, Explained.new(data, folded: folded, fold_exceptions: fold_exceptions))
    write_tsv(File.join(out, "residual.tsv"), %w[set id check predicted observed], residual)
    File.write(File.join(out, "summary.json"), JSON.pretty_generate(
      variant: variant,
      sets: rows.transform_values { set_summary(_1) },
      ignorable: ranges_text(rows.fetch("between").select { _1.representative == SEED_ID }.map { _1.id.to_i(16) }),
      fold_status: fold.map(&:status).tally,
      fold_changed: ranges_text(fold.select { _1.status == "determined" && hex(_1.unit) != _1.fold }.map(&:unit)),
      fold_exceptions: ranges_text(fold_exceptions.keys),
      unobservable: fold.select { _1.status == "unobservable" }.map(&:fold).tally,
      pair_only: fold.select { _1.status == "pair-only" }.map(&:fold).tally,
      differences: differences.map { "#{_1.set}:#{_1.check}" }.tally,
      residual: residual.map { "#{_1.set}:#{_1.id}:#{_1.check}" }
    ))
    write_digests(out)
  end

  def write_digests(out)
    files = Dir.children(out).reject { _1 == "SHA256SUMS" }.sort
    File.write(File.join(out, "SHA256SUMS"), files.map { "#{Digest::SHA256.file(File.join(out, _1)).hexdigest}  #{_1}\n" }.join)
  end

  # ---- small volumes ----

  # What `fsck_hfs -n` says about one name on a volume of its own.
  FsckRow = Data.define(:name, :variant, :values, :stored_units, :fsck_ok, :fsck_result, :illegal) do
    def cells = to_h.values
  end
  FSCK_COLUMNS = %w[name variant values stored_units fsck_ok fsck_result illegal].freeze

  # The two sequences `fsck_hfs` flags, where they sit in a name, and the
  # controls: the reversed and the longer sequences it accepts, and the names
  # that hold a stored U+0000, an ignorable unit and the system-mapped colon.
  FSCK_SEQUENCES = [[0x306, 0x307], [0x308, 0x30D]].freeze
  FSCK_POSITIONS = { "after-a" => [[0x61], []], "after-A" => [[0x41], []], "after-b" => [[0x62], []], "after-e" => [[0x65], []],
                     "bare" => [[], []], "mid-name" => [[0x61, 0x62], [0x63]],
                     "after-U+0103" => [[0x103], []], "after-U+00E4" => [[0xE4], []] }.freeze
  FSCK_CONTROLS = { "reversed-0307-0306" => [0x61, 0x307, 0x306], "reversed-030D-0308" => [0x61, 0x30D, 0x308],
                    "triple-0308-0301-030D" => [0x61, 0x308, 0x301, 0x30D], "plain" => [0x61],
                    "nul-first" => [0x2400, 0x61], "nul-middle" => [0x61, 0x2400, 0x62], "nul-last" => [0x61, 0x2400],
                    "ignorable-alone" => [0x200C], "colon" => [0x61, 0x3A, 0x62] }.freeze

  def fsck_cases
    flagged = FSCK_SEQUENCES.product(FSCK_POSITIONS.to_a).map do |sequence, (position, (before, after))|
      ["#{position}:#{sequence.map { hex(_1) }.join("+")}", [*before, *sequence, *after]]
    end
    flagged + FSCK_CONTROLS.to_a
  end

  # Create the one name `cps` on a copy of the formatted `template`, unmount,
  # check, and read the catalog back.
  def fsck_row(log, attachments, template, image, mount, variant, name, cps)
    FileUtils.cp(template, image)
    attempt = attachments.with(image, mountpoint: mount) do
      require_mount_point(mount)
      Dir.chdir(mount) { create("./".b + cps.pack("U*").b) }
    end
    verdict = fsck_verdict(attachments, log, image)
    leaves, = read_leaves(image)
    leaf = leaves[attempt.ino]
    raise Error, "#{name} on #{variant}: #{attempt.failure || "created but not in the catalog"}" unless leaf

    FsckRow.new(name: name, variant: variant, values: cps.map { hex(_1) }.join("+"), stored_units: units_text(leaf.units),
                fsck_ok: verdict[:ok], fsck_result: verdict[:last_line], illegal: verdict[:illegal_names].join(" / "))
  end

  # Values of class 0 that decompose into marks of nonzero class, with no
  # starter before them: the one place where a class sort applied before
  # decomposing would differ from one applied after.
  def mark_only_decompositions(data, prediction)
    data.assigned.select { _1 <= 0xFFFF && data.class_of(_1).zero? && prediction.decomposes?(_1) && data.class_of(prediction.stored([_1]).first).positive? }
  end

  # For each such value X that decomposes to D1 D2: X alone, its decomposition,
  # the decomposition reversed, and the last mark before and after X.
  def class0_items(data, prediction)
    mark_only_decompositions(data, prediction).flat_map do |x|
      d1, d2 = prediction.stored([x])
      [[x], [d1, d2], [d2, d1], [d2, x], [x, d2]].map { item(cps: [ASCII_A, *_1], scope: "class0") }
    end
  end

  def class0_table(log, attachments, template, image, mount, variant, data)
    prediction = Prediction.new(data, folded: variant == "hfsplus")
    items = class0_items(data, prediction)
    FileUtils.cp(template, image)
    observed = attachments.with(image, mountpoint: mount) { observe_volume(mount, { "class0" => items }) }
    leaves, = read_leaves(image)
    problems = readdir_problems(observed, leaves)
    raise Error, problems.join("; ") unless problems.empty?

    rows = set_rows(items, observed.fetch("class0"), leaves)
    [rows, differences_of_set("class0", items, rows, prediction) + order_differences("class0", rows, prediction)]
  end

  # Evidence for the small-volume claims of the record, written to
  # OUT_ROOT/e3-small-names/: fsck.tsv (one row per name and comparator),
  # class0-VARIANT.tsv (the SET.tsv columns) and class0-VARIANT-differences.tsv
  # (where the prediction, a stable class sort after decomposing, disagrees).
  def run_small_names(cfg, data)
    out = File.join(cfg.root, "e3-small-names")
    refuse_existing_capture(out, cfg)
    log = HfsPlusCapture::Log.new
    attachments = HfsPlusCapture::Attachments.new(log)
    template = File.join(cfg.work, "small-template.img")
    image = File.join(cfg.work, "small.img")
    mount = File.join(cfg.work, "small.mnt")
    FileUtils.mkdir_p([cfg.work, mount, out])
    leftover = nil
    begin
      fsck_rows = cfg.variants.flat_map do |variant|
        format_volume(log, attachments, template, variant, bytes: SMALL_VOLUME_BYTES)
        rows, differences = class0_table(log, attachments, template, image, mount, variant, data)
        write_tsv(File.join(out, "class0-#{variant}.tsv"), COLUMNS, rows)
        write_tsv(File.join(out, "class0-#{variant}-differences.tsv"), %w[set id check predicted observed], differences)
        fsck_cases.map { |name, cps| fsck_row(log, attachments, template, image, mount, variant, name, cps) }
      end
      write_tsv(File.join(out, "fsck.tsv"), FSCK_COLUMNS, fsck_rows)
    ensure
      leftover = finish_run(attachments, log, out, [template, image], mount, keep_image: cfg.keep_image)
    end
    raise leftover if leftover

    write_digests(out)
  end

  # ---- command line ----

  def parse(argv)
    options = { unicode_data: DEFAULT_DATA, root: DEFAULT_ROOT, work: DEFAULT_WORK, variants: VARIANTS.keys, sample: 1, analyze_only: false, keep_image: false, overwrite: false, small_names: false }
    OptionParser.new do |parser|
      parser.banner = "usage: unicode-probe.rb [options]"
      parser.on("--unicode-data PATH", "the pinned UnicodeData-3.2.0.txt") { options[:unicode_data] = _1 }
      parser.on("--root DIR", "where e3-VARIANT/ is written") { options[:root] = _1 }
      parser.on("--work DIR", "where the disposable volumes live") { options[:work] = _1 }
      parser.on("--variant NAME", VARIANTS.keys, "only this comparator") { options[:variants] = [_1] }
      parser.on("--sample N", Integer, "keep every Nth probe, to measure") { options[:sample] = _1 }
      parser.on("--analyze-only", "rewrite the derived tables without a volume") { options[:analyze_only] = true }
      parser.on("--keep-image", "leave the image in the work directory") { options[:keep_image] = true }
      parser.on("--overwrite", "replace an existing capture directory") { options[:overwrite] = true }
      parser.on("--small-names", "run the small-volume probes into e3-small-names/") { options[:small_names] = true }
    end.parse!(argv)
    raise Error, "--sample must be at least 1" if options[:sample] < 1
    raise Error, "--analyze-only reads the whole probe set; it cannot be combined with --sample" if options[:analyze_only] && options[:sample] != 1

    Config.new(**options)
  end

  def main(argv)
    cfg = parse(argv)
    data = UnicodeTables.new(cfg.unicode_data)
    if cfg.small_names
      run_small_names(cfg, data)
      return 0
    end

    sets = probe_sets(data, cfg.sample)
    cfg.variants.each do |variant|
      run_volume(cfg, variant, sets) unless cfg.analyze_only
      # A sampled run measures only: its tables would describe a subset.
      analyze(cfg, variant, sets, data) if cfg.sample == 1
    end
    0
  rescue Error, HfsPlusFields::Error, HfsPlusCapture::Error, OptionParser::ParseError, SystemCallError => e
    warn "unicode-probe: #{e.message}"
    1
  end
end

exit(HfsPlusUnicodeProbe.main(ARGV)) if $PROGRAM_NAME == __FILE__
