# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "open3"
require "tmpdir"
require "timeout"
require "json"
require "digest"
require "set"
require "fileutils"
require_relative "../tools/hfsplus/capture"

module FsimgSpec
  class Error < StandardError; end
end

module HfsPlusIntegration
  class Error < FsimgSpec::Error; end
  class CommandTimeout < Error; end
  class Skip < Error; end

  READER_TIMEOUT = 60
  Volume = Data.define(:image, :name, :source, :local)
  Snapshot = Data.define(:entries, :digests, :matched)
  ExpectedVerification = Data.define(:source, :name, :arguments, :status, :message,
    :files, :directories, :allocated_clusters, :free_clusters, :bad_clusters)
  Limitation = Data.define(:oracle, :volume, :member, :error)
  Tally = Data.define(:entries, :files, :bytes) do
    def initialize(entries: 0, files: 0, bytes: 0) = super
    def +(other) = Tally.new(entries: entries + other.entries,
      files: files + other.files, bytes: bytes + other.bytes)
  end
  FileDigest = Data.define(:sha256, :bytes)
  Result = Data.define(:value, :tally, :findings, :skips, :timeout) do
    def initialize(value: nil, tally: Tally.new, findings: [], skips: [], timeout: nil) = super
  end
  Comparison = Data.define(:value, :verdict, :oracle, :findings)
  # TSK exposes synthetic system forks, private catalog storage and journal
  # files. fsimg omits these format-owned objects.
  SLEUTH_HIDDEN = %r{\A/(?:\$(?:ExtentsFile|CatalogFile|BadBlockFile|AllocationFile|AttributesFile)|\.HFS\+ Private Directory Data\^|\^{4}HFS\+ Private Data|\.journal|\.journal_info_block)(?:/|\z)}.freeze
  # These are observed oracle defects, scoped to named volumes and errors.
  # They yield skips, never agreements. A skip does not fail the run, so each
  # entry must pin its exact diagnostic.
  ORACLE_LIMITATIONS = [
    # TSK cannot read the fork-backed attribute on attr-sizes in either E5 tree.
    Limitation.new(oracle: "sleuth", volume: /\Ah-hfs(?:plus|x)-16777216-e5\z/,
      member: "/attr-sizes", error: /Unsupported record type: \(32\)/),
    # TSK abandons folders whose thread starts in the last 520 catalog-node bytes.
    Limitation.new(oracle: "sleuth", volume: /\Ah-hfsplus-8388608-e4\z/,
      member: nil, error: /fls paths: missing=\["\/u1\/a"\] \(1\) extra=\[\] \(0\)/),
    # writer_tree_check compares root names and /hello only, so it would not
    # catch a phantom member here. The row is a skip, never an agreement, and
    # hfsdump, when present, lists /dir/sub on these volumes.
    Limitation.new(oracle: "sleuth", volume: /\Ahfs(?:plus|x)-4096-nearlyfull-small-/,
      member: nil, error: /fls paths: missing=\["\/dir\/sub\/below"\] \(1\) extra=\[\] \(0\)/),
    # HFSX catalog thread lookup fails entirely in this capture.
    Limitation.new(oracle: "sleuth", volume: /\Ah-hfsx-8388608-e4\z/,
      member: nil, error: /hfs_cat_file_lookup: Error finding thread node for file \(1\)/),
    # TSK lacks LZFSE inline/resource decoding (types 11 and 12).
    Limitation.new(oracle: "sleuth", volume: /\Ah-hfs(?:plus|x)-8388608-e6-lzfse\z/,
      member: %r{\A/(?:forced-mixed-196608|forced-random-1|mixed-196608|random-1|shape-(?:131073|262145|393217|589825)|(?:text|zeros)-(?:1|200000|65535|65536|65537))\z},
      error: /Attribute 4352 not found/),
    # TSK misreads the resource framing/chunk bounds of these type 8 files.
    Limitation.new(oracle: "sleuth", volume: /\Ah-hfsplus-8388608-e6-lzvn\z/,
      member: %r{\A/(?:forced-mixed-196608|mixed-196608|text-200000|zeros-200000)\z},
      error: /block size is too large: (?:66145|4294888110|4294965730)/),
    # hfsdump emits bad bytes for these type 8 final partial resource chunks.
    Limitation.new(oracle: "hfsdump", volume: /\Ah-hfsplus-8388608-e6-ditto\z/,
      member: %r{\A/shape-(?:262145|393217|589825)\z}, error: /SHA256 differs:/)
  ].freeze

  Kind = Data.define(:mode, :fls)
  KINDS = {
    "file" => Kind.new(mode: 0o100000, fls: "r"),
    "directory" => Kind.new(mode: 0o040000, fls: "d"),
    "symlink" => Kind.new(mode: 0o120000, fls: "l"),
    "fifo" => Kind.new(mode: 0o010000, fls: "p"),
    "char_device" => Kind.new(mode: 0o020000, fls: "c"),
    "block_device" => Kind.new(mode: 0o060000, fls: "b"),
    "socket" => Kind.new(mode: 0o140000, fls: "s")
  }.freeze
  # hwf_add_mapnodes emits eight digits followed by 232 'n' bytes.
  MAPNODES_MEMBER = "/00000000#{'n' * 232}".freeze

  GENERATOR_TIMEOUT = 30 * 60
  BLOCKS = [512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536].freeze
  VARIANTS = %w[hfsplus hfsx].freeze
  LABEL = "Writer volume".freeze
  ROOT_NAMES = ["dir", "empty", "equal", "above", "two", "hello", "cafe\u0301",
                "link", "pipe", "a" * 200, "z" * 200, "Zeta", "alpha",
                "\u03a9", "\u03b1", ":", "\u2400", "\u{1f600}", "\uff21",
                "a\u200cx", "ay"].freeze
  HELLO = (0...5).map { (1 + (8 * 17 + _1 * 13) % 251).chr }.join.b.freeze
  module_function

  def text(bytes) = bytes.dup.force_encoding("UTF-8").scrub

  def find_program(name)
    return name if name.include?("/") && File.file?(name) && File.executable?(name)

    ENV.fetch("PATH", "").split(File::PATH_SEPARATOR).map { File.join(_1, name) }
       .find { File.file?(_1) && File.executable?(_1) }
  end

  # Drain both pipes even after retaining an excerpt: a noisy child must neither
  # block on stderr nor make failure diagnostics consume unbounded memory.
  def run_command(argv, stdout:, timeout:, statuses:)
    excerpt = String.new.b
    err = String.new.b
    status = Open3.popen3({ "LC_ALL" => "C", "TZ" => "UTC" },
      [argv.first, argv.first], *argv.drop(1), pgroup: true) do |input, output, errors, child|
      input.close
      reader = Thread.new do
        while (chunk = output.read(64 * 1024))
          excerpt << chunk.byteslice(0, [2048 - excerpt.bytesize, 0].max)
          stdout.call(chunk)
        end
      end
      diagnostics = Thread.new do
        while (chunk = errors.read(64 * 1024))
          err << chunk.byteslice(0, [4096 - err.bytesize, 0].max)
        end
      end
      begin
        Timeout.timeout(timeout) do
          reader.value
          diagnostics.value
          child.value
        end
      rescue Timeout::Error
        begin
          Process.kill("KILL", -child.pid)
        rescue Errno::ESRCH
          nil
        end
        child.value
        raise CommandTimeout, "#{argv.join(' ')}: timed out after #{timeout.is_a?(Integer) ? timeout : format("%.1f", timeout)} seconds"
      ensure
        reader.join
        diagnostics.join
      end
    end
    unless statuses.include?(status.exitstatus)
      raise Error, "#{argv.join(' ')}: exit #{status.exitstatus}: #{text(err)}\nstdout excerpt: #{text(excerpt).inspect}"
    end
  end

  def command(*argv, timeout: 30, statuses: [0])
    output = String.new.b
    run_command(argv, stdout: ->(chunk) { output << chunk }, timeout: timeout, statuses: statuses)
    output
  end

  def digest_output(*argv, timeout: 30)
    digest = Digest::SHA256.new
    bytes = 0
    consume = ->(chunk) { digest.update(chunk); bytes += chunk.bytesize }
    run_command(argv, stdout: consume, timeout: timeout, statuses: [0])
    FileDigest.new(sha256: digest.hexdigest, bytes: bytes)
  end

  def oracles
    candidates = {
      native: RUBY_PLATFORM.include?("darwin") && find_program("hdiutil") && find_program("fsck_hfs"),
      hfsdump: find_program(ENV.fetch("HFSDUMP", File.join(Dir.home, ".local/share/fsimg-oracles/hfsfuse/bin/hfsdump"))),
      sleuth: %w[fsstat fls icat].all? { find_program(_1) },
      linux: find_program("fsck.hfsplus")
    }
    candidates.each { |name, available| puts "SKIP: #{name} oracle absent" unless available }
    candidates
  end

  def matrix(large:)
    ordinary = VARIANTS.flat_map do |variant|
      BLOCKS.map { [variant, _1, "tree", nil] } +
        [512, 4096].map { [variant, _1, "empty", nil] } +
        %w[empty-min tree-min].map { [variant, 0, _1, nil] } +
        [[variant, 4096, "tree", (4 << 20) + 512],
         [variant, 4096, "tree", (4 << 20) + 2048],
         [variant, 512, "tree", 4 << 20],
         [variant, 4096, "nearlyfull-small", nil]]
    end
    return ordinary unless large

    ordinary + VARIANTS.flat_map do |variant|
      [[variant, 4096, "tree", 1 << 30], [variant, 8192, "tree", 1 << 30],
       [variant, 4096, "mapnodes-small", nil],
       [variant, 4096, "mapnodes", nil], [variant, 4096, "nearlyfull", nil]]
    end
  end

  # Log.run retains only a display excerpt; save the returned complete output
  # separately. Even attachment or fsck failure reaches release in ensure.
  def native_check(image, oracle_log:)
    log = HfsPlusCapture::Log.new
    attachments = HfsPlusCapture::Attachments.new(log)
    begin
      attachments.with(image, readonly: true) do |device|
        out, ok = log.run("fsck_hfs", "-fn", device.sub("/dev/", "/dev/r"), timeout: HfsPlusCapture::FSCK_TIMEOUT)
        if oracle_log
          File.open(oracle_log, "ab") do |file|
            file.write("\nIMAGE #{image}\n")
            file.write(out)
          end
        end
        puts out.lines.last(6).map { "  #{_1.rstrip}" }
        findings = HfsPlusCapture.fsck_findings(out)
        raise Error, "fsck_hfs findings:\n#{findings.join("\n")}" unless findings.empty?
        raise Error, "fsck_hfs failed or did not check the volume" unless ok && out.include?("appears to be OK")
      end
    ensure
      attachments.release
    end
  end

  def mapnodes?(shape) = %w[mapnodes mapnodes-small].include?(shape)

  def check_mapnodes(image)
    File.open(image, "rb") do |file|
      file.seek(1024 + 272 + 16)
      catalog_block = file.read(4).unpack1("N")
      file.seek(catalog_block * 4096 + 14 + 22)
      total, free = file.read(8).unpack("N2")
      file.seek(catalog_block * 4096)
      map_node = file.read(4).unpack1("N")
      raise Error, "mapnodes fixture has no map node" if map_node.zero?
      raise Error, "mapnodes fixture has only #{total - free} used nodes" unless total - free > 30_720
    end
  end

  def check_nearlyfull(image)
    File.open(image, "rb") do |file|
      file.seek(1024 + 48)
      free = file.read(4).unpack1("N")
      raise Error, "nearlyfull fixture has #{free} free blocks" unless free <= 4
    end
  end

  def generate(generator:, image:, variant:, block:, shape:, size:, directory:)
    if %w[nearlyfull nearlyfull-small].include?(shape)
      probe = File.join(directory, "size-probe.img")
      command(generator, probe, variant, block.to_s, "tree")
      size = File.size(probe) + 2 * block
      size = [size, 4 << 20].max if shape == "nearlyfull-small"
      File.unlink(probe)
    end
    args = [generator, image, variant, block.to_s, shape]
    args << size.to_s if size
    command(*args, timeout: GENERATOR_TIMEOUT)
    check_mapnodes(image) if mapnodes?(shape)
    check_nearlyfull(image) if %w[nearlyfull nearlyfull-small].include?(shape)
  end

  def image_line(image, name:)
    block_bytes = File.binread(image, 4, 1024 + 40)
    raise Error, "truncated volume header blockSize" unless block_bytes.bytesize == 4

    "IMAGE #{name} size=#{File.size(image)} block=#{block_bytes.unpack1('N')}"
  end

  def reader_versions(available)
    if available[:hfsdump]
      pin = File.read(File.join(__dir__, "../build-aux/build-hfsfuse.sh"))[/^revision=(.+)$/, 1]
      raise Error, "hfsfuse build pin absent" unless pin

      puts "VERSION hfsdump hfsfuse #{pin} (build-hfsfuse.sh pin)"
    end
    puts "VERSION sleuth #{text(command('fls', '-V')).strip}" if available[:sleuth]
  end

  def verification_rows
    File.readlines(File.join(__dir__, "hfsplus-verify-expected.tsv"), chomp: true)
      .reject { _1.start_with?("#") }.map do |line|
        fields = ExpectedVerification.members.zip(line.split("\t", -1)).to_h
        ExpectedVerification.new(**fields)
      end
  end

  def reader_sources(native: ENV["FSIMG_HFSPLUS_NATIVE"], corpus: ENV["FSIMG_HFSPLUS_CORPUS"])
    {
      "native" => native && File.join(File.expand_path(native), "*", "*.img"),
      "linux-mkfs-hfsplus" => corpus && File.join(File.expand_path(corpus), "linux-mkfs-hfsplus", "*.img")
    }
  end

  def unmatched_sources(sources:, volumes:)
    sources.keys.select { |source| sources[source] && volumes.none? { _1.source == source } }
  end

  def reader_volumes(directory)
    volumes = reader_sources.flat_map do |source, pattern|
      paths = pattern ? Dir[pattern].sort : []
      if paths.empty?
        puts "SKIP #{source}: #{pattern || 'environment variable unset'}: no images"
      end
      paths.map do |image|
        name = source == "native" ? File.basename(File.dirname(image)) : File.basename(image)
        Volume.new(image: File.realpath(image), name: name, source: source, local: false)
      end
    end
    mkfs = find_program("mkfs.hfsplus")
    unless mkfs
      puts "SKIP local-mkfs: mkfs.hfsplus absent"
      return volumes
    end

    image = File.join(directory, "local-mkfs.img")
    # Sixteen MiB fits hfsprogs' default catalog without allocating payload.
    File.open(image, "wb") { _1.truncate(16 << 20) }
    command(mkfs, "-v", "Reader volume", image)
    volumes + [Volume.new(image: image, name: "plain.img", source: "local-mkfs", local: true)]
  end

  def json_records(output) = output.lines.map { JSON.parse(_1) }

  def expected_verification(volume)
    source = volume.source == "native" ? "native" : "corpus"
    name = source == "native" ? volume.name : "linux-mkfs-hfsplus/#{volume.name}"
    verification_rows.find { _1.source == source && _1.name == name }
  end

  def verify_volume(fsimg:, volume:)
    expected = expected_verification(volume)
    status = expected&.status || "ok"
    records = json_records(command(fsimg, "W", "--json", "-f", volume.image,
      timeout: READER_TIMEOUT, statuses: status == "ok" ? [0] : [1]))
    if status != "ok"
      error = records.find { _1["type"] == "error" }
      unless error && error["status"] == status && error.fetch("message").include?(expected.message)
        raise Error, "verification expected #{status}: #{expected.message}, got #{text(records.inspect.byteslice(0, 2048))}"
      end
      puts "EXPECTED fsimg #{volume.source}/#{volume.name}: #{status}: #{expected.message}"
      raise Skip, "expected verification refusal; no verified comparison baseline"
    end
    report = records.find { _1["type"] == "verify" }
    raise Error, "verification incomplete" unless report && report["complete"]

    return unless expected

    %i[files directories allocated_clusters free_clusters bad_clusters].each do |field|
      count = expected.public_send(field)
      raise Error, "verification #{field}: expected #{count}, got #{report[field.to_s]}" unless report[field.to_s].to_s == count
    end
  end

  def list_volume(fsimg:, volume:)
    records = json_records(command(fsimg, "t", "--json", "-f", volume.image, timeout: READER_TIMEOUT))
    entries = records.filter_map do |record|
      case [record["type"], record["path"]]
      in ["entry", String => path] then [path, record]
      else nil
      end
    end
    raise Error, "duplicate fsimg listing paths" unless entries.map(&:first).tally.values.all? { _1 == 1 }

    entries.to_h.freeze
  end

  def extract_digests(fsimg:, volume:, entries:, directory:)
    extraction = Dir.mktmpdir("extract-", directory)
    begin
      records = json_records(command(fsimg, "x", "--json", "-E", "-k", "-f", volume.image,
        "-C", extraction, timeout: READER_TIMEOUT))
      summary = records.find { _1["type"] == "summary" }
      raise Error, "extraction lacks summary record" unless summary

      skipped = entries.values.count { !%w[file directory].include?(_1.fetch("kind")) }
      raise Error, "extraction skipped count: #{summary.inspect}, expected #{skipped}" unless Integer(summary.fetch("skipped_entries")) == skipped
      raise Error, "extraction omitted names: #{summary.inspect}" unless Integer(summary.fetch("omitted_entries")).zero?
      entries.filter_map do |path, entry|
        next unless %w[file symlink].include?(entry.fetch("kind"))

        digest = if entry.fetch("kind") == "symlink"
          digest_output(fsimg, "x", "-O", "-f", volume.image, "--", path, timeout: READER_TIMEOUT)
        else
          host = File.join(extraction, path.delete_prefix("/"))
          raise Error, "#{path.inspect}: extracted member is not a regular file" unless File.lstat(host).file?

          FileDigest.new(sha256: Digest::SHA256.file(host).hexdigest, bytes: File.size(host))
        end
        size = Integer(entry.fetch("size_bytes"))
        raise Error, "#{path.inspect}: fsimg read #{digest.bytes} bytes, listing says #{size}" unless digest.bytes == size

        [path, digest]
      end.to_h.freeze
    ensure
      FileUtils.remove_entry(extraction)
    end
  end

  def manifest_matches(volume:, digests:)
    manifest_path = File.join(File.dirname(volume.image), "manifest.json")
    return Set.new.freeze unless volume.source == "native" && File.file?(manifest_path)

    manifest = JSON.parse(File.read(manifest_path, encoding: "UTF-8")).fetch("tree", [])
    manifest.filter_map do |observation|
      path = "/#{observation.fetch('path')}"
      expected = observation["sha256"] || (Digest::SHA256.hexdigest(observation["target"].b) if observation["target"])
      next unless expected

      digest = digests[path]
      raise Error, "#{path.inspect}: native manifest member has no fsimg digest" unless digest
      raise Error, "#{path.inspect}: fsimg SHA256 #{digest.sha256} differs from native #{expected}" unless expected == digest.sha256

      path
    end.to_set.freeze
  end

  def fsimg_snapshot(fsimg:, volume:, directory:)
    verify_volume(fsimg: fsimg, volume: volume)
    entries = list_volume(fsimg: fsimg, volume: volume)
    digests = extract_digests(fsimg: fsimg, volume: volume, entries: entries, directory: directory)
    matched = manifest_matches(volume: volume, digests: digests)
    snapshot = Snapshot.new(entries: entries, digests: digests, matched: matched)
    Result.new(value: snapshot, tally: Tally.new(entries: entries.length,
      files: digests.length, bytes: digests.values.sum(&:bytes)))
  end

  # Compares as multisets. unattributed counts the actual lines the caller
  # cannot tie to an expected path; exactly that many must be left over.
  def same_paths(expected:, actual:, context:, unattributed: 0)
    wanted, got = expected.tally, actual.tally
    missing = expected.uniq.flat_map { |path| [path] * [wanted[path] - got.fetch(path, 0), 0].max }
    extra = actual.uniq.flat_map { |path| [path] * [got[path] - wanted.fetch(path, 0), 0].max }
    return if missing.empty? && extra.length == unattributed

    suffix = unattributed.zero? ? "" : ", unattributed expected=#{unattributed}"
    raise Error, "#{context}: missing=#{missing.first(20).inspect} (#{missing.length}) extra=#{extra.first(20).inspect} (#{extra.length})#{suffix}"
  end

  def same_digest(actual:, path:, snapshot:)
    expected = snapshot.digests.fetch(path)
    unless actual == expected
      raise Error, "#{path.inspect}: SHA256 differs: fsimg=#{expected.sha256} oracle=#{actual.sha256}; bytes=#{expected.bytes}/#{actual.bytes}"
    end
    Tally.new(files: 1, bytes: actual.bytes)
  end

  # A member limitation needs its digest matched against the native manifest,
  # so it can only apply to a native volume. A volume-wide limitation matches
  # its name and error.
  def known_limitation?(oracle:, volume:, path:, message:, matched: Set.new)
    ORACLE_LIMITATIONS.any? do |limitation|
      limitation.oracle == oracle && limitation.volume.match?(volume.name) &&
        limitation.error.match?(message) &&
        (limitation.member.nil? || (limitation.member === path && matched.include?(path)))
    end
  end

  # This per-member StandardError boundary records decoding and host I/O errors.
  # Keep completed member checks when another member fails. A timeout ends
  # traversal and travels with earlier findings, so it cannot hide them.
  def compare_members(entries, oracle:, volume:, snapshot:)
    tally = Tally.new
    findings = []
    skips = []
    timeout = nil
    entries.each do |path, entry|
      begin
        tally += yield(path, entry)
      rescue CommandTimeout => e
        timeout = e.message
        break
      rescue Skip => e
        skips << "#{path.inspect}: #{e.message}"
      rescue StandardError => e
        if known_limitation?(oracle: oracle, volume: volume, path: path, message: e.message, matched: snapshot.matched)
          skips << "#{path.inspect}: known oracle limitation: #{e.message}"
        else
          findings << "#{path.inspect}: #{e.message}"
        end
      end
    end
    Result.new(tally: tally, findings: findings, skips: skips, timeout: timeout)
  end

  # fls spells stored slash and NUL as ^ and can collapse distinct names, so
  # paths remain a multiset.
  def sleuth_path(path) = path.gsub(/[:\u2400]/, "^")

  def hfsdump_reader(tool:, volume:, snapshot:, invoke:, members: snapshot.entries)
    children = snapshot.entries.keys.group_by { File.dirname(_1) }
    root = { "kind" => "directory", "size_bytes" => "0" }
    compare_members(members.merge("/" => root), oracle: "hfsdump", volume: volume, snapshot: snapshot) do |path, entry|
      raise Skip, "hfsdump cannot address literal stored-NUL name #{path.inspect}" if path.include?("\u2400")

      stat = invoke.call(:command, tool, volume.image, "stat", path)
      if entry.fetch("kind") == "directory"
        hfsdump_folder(tool: tool, volume: volume, invoke: invoke, path: path, stat: stat, children: children.fetch(path, []))
      else
        hfsdump_file(tool: tool, volume: volume, snapshot: snapshot, invoke: invoke, path: path, entry: entry, stat: stat)
      end
    end
  end

  def hfsdump_folder(tool:, volume:, invoke:, path:, stat:, children:)
    raise Error, "hfsdump type differs: #{text(stat)}" unless stat[/^type: (.+)$/, 1] == "folder"

    raise Error, "hfsdump root CNID differs" if path == "/" && stat[/^cnid: (\d+)$/, 1] != "2"

    valence = stat[/^valence: (\d+)$/, 1]
    raise Error, "hfsdump folder lacks valence" unless valence

    # Journal and private catalog objects count in root valence but neither
    # reader lists them. Only root may have valence without visible children.
    if Integer(valence).positive? && children.empty? && path != "/"
      raise Error, "hfsdump non-root valence #{valence} with no fsimg children"
    end
    names = if Integer(valence).positive?
      invoke.call(:command, tool, volume.image, "read", path).dup.force_encoding("UTF-8").lines.map(&:chomp)
    else
      []
    end
    # hfsdump prints a name through puts, which stops at a stored NUL, so its
    # line is not that name. Such a name leaves the comparison, as its member
    # does in hfsdump_reader: it accounts for one line no other name claims,
    # whatever that line holds, and fls, when present, compares the name itself.
    nul_names, plain = children.map { File.basename(_1) }.partition { _1.include?("\u2400") }
    same_paths(expected: plain, actual: names, context: "#{path.inspect}: hfsdump children", unattributed: nul_names.length)
    Tally.new(entries: 1)
  end

  def hfsdump_file(tool:, volume:, snapshot:, invoke:, path:, entry:, stat:)
    raise Error, "hfsdump type differs: #{text(stat)}" unless stat[/^type: (.+)$/, 1] == "file"

    size = stat[/^decmpfs.logical_size: (\d+)$/, 1] || stat[/^data_fork.logical_size: (\d+)$/, 1]
    unless size && Integer(size) == Integer(entry.fetch("size_bytes"))
      raise Error, "hfsdump size #{size.inspect}, fsimg #{entry.fetch('size_bytes')}"
    end
    mode = stat[/^permissions.file_mode: ([0-7]+)$/, 1]
    type = KINDS.fetch(entry.fetch("kind")).mode
    if mode && (Integer(mode, 8) & 0o170000) != type
      raise Error, "hfsdump mode #{mode}, fsimg kind #{entry.fetch('kind')}"
    end
    tally = Tally.new(entries: 1)
    if snapshot.digests.key?(path)
      tally += same_digest(actual: invoke.call(:digest_output, tool, volume.image, "read", path), path: path, snapshot: snapshot)
    end
    tally
  end

  def sleuth_reader(volume:, snapshot:, invoke:)
    invoke.call(:command, "fsstat", volume.image)
    listing = invoke.call(:command, "fls", "-r", "-p", volume.image).dup.force_encoding("UTF-8")
    nodes = listing.lines.map do |line|
      match = /\A(\S+)\s+(\S+):\t(.*)\n?\z/.match(line)
      raise Error, "unrecognized fls record: #{line.inspect}" unless match

      [sleuth_path("/#{match[3]}"), { inode: match[2], type: match[1] }]
    end.reject { |path, _| SLEUTH_HIDDEN.match?(path) }
    skips = []
    begin
      expected = snapshot.entries.keys.map { sleuth_path(_1) }
      same_paths(expected: expected, actual: nodes.map(&:first), context: "fls paths")
    rescue Error => e
      raise unless known_limitation?(oracle: "sleuth", volume: volume, path: nil, message: e.message)

      skips << "known oracle limitation: #{e.message}"
    end
    # Preserve colliding fls spellings and consume one inode per fsimg member.
    by_path = nodes.group_by(&:first).transform_values { _1.map(&:last) }
    reported = snapshot.entries.select { |path, _| by_path.key?(sleuth_path(path)) }
    result = compare_members(reported, oracle: "sleuth", volume: volume, snapshot: snapshot) do |path, entry|
      node = by_path.fetch(sleuth_path(path)).shift
      type = KINDS.fetch(entry.fetch("kind")).fls
      raise Error, "fls type #{node[:type]}, fsimg #{type}" unless node.fetch(:type).split("/").last == type

      tally = Tally.new(entries: 1)
      if snapshot.digests.key?(path)
        tally += same_digest(actual: invoke.call(:digest_output, "icat", volume.image, node.fetch(:inode)), path: path, snapshot: snapshot)
      end
      tally
    end
    result.with(skips: skips + result.skips)
  end

  def timeout_skip?(oracle:, local:) = oracle != "fsimg" && !local

  def exit_code(rows:, unmatched:)
    return 1 if rows.any? { _1.verdict == "finding" } || !unmatched.empty?

    rows.any? { _1.oracle != "fsimg" && _1.verdict == "agree" } ? 0 : 77
  end

  # This per-comparison boundary records command, decoding and host I/O
  # exceptions. Raised comparisons have no completed result and report zeros.
  # Local volumes are stricter: nothing but this project made them, so a hang
  # is ours to explain.
  def comparison(oracle:, volume:)
    result = begin
      yield || Result.new
    rescue CommandTimeout => e
      Result.new(timeout: e.message)
    rescue Skip => e
      Result.new(skips: [e.message])
    rescue StandardError => e
      if known_limitation?(oracle: oracle, volume: volume, path: nil, message: e.message)
        Result.new(skips: ["known oracle limitation: #{e.message}"])
      else
        Result.new(findings: [e.message])
      end
    end
    findings = result.findings
    skips = result.skips
    if result.timeout
      if findings.empty? && timeout_skip?(oracle: oracle, local: volume.local)
        skips += [result.timeout]
      else
        findings += [result.timeout]
      end
    end
    verdict = if !findings.empty?
      "finding"
    elsif !skips.empty?
      "skip"
    else
      "agree"
    end
    skips.each { puts "SKIP #{oracle} #{volume.source}/#{volume.name}: #{_1}" }
    warn "FINDING #{oracle} #{volume.source}/#{volume.name}: #{findings.join("\n")}" unless findings.empty?
    tally = result.tally
    puts "COMPARE #{oracle} #{volume.source}/#{volume.name} entries=#{tally.entries} files=#{tally.files} bytes=#{tally.bytes} result=#{verdict}"
    Comparison.new(value: result.value, verdict: verdict, oracle: oracle, findings: findings)
  end

  def writer_tree_check(snapshot:, shape:)
    if mapnodes?(shape)
      path = MAPNODES_MEMBER
      expected = FileDigest.new(sha256: Digest::SHA256.hexdigest(""), bytes: 0)
      raise Error, "mapnodes synthetic file absent or nonempty" unless snapshot.digests[path] == expected
    elsif !%w[empty empty-min].include?(shape)
      names = snapshot.entries.keys.select { File.dirname(_1) == "/" }.map { File.basename(_1) }
      raise Error, "writer tree lacks root names" unless (ROOT_NAMES - names).empty?
      raise Error, "writer hello digest differs" unless snapshot.digests.fetch("/hello").sha256 == Digest::SHA256.hexdigest(HELLO)
    end
  end

  def reader_volume(fsimg:, volume:, available:, directory:, shape: nil)
    baseline = comparison(oracle: "fsimg", volume: volume) do
      result = fsimg_snapshot(fsimg: fsimg, volume: volume, directory: directory)
      writer_tree_check(snapshot: result.value, shape: shape) if shape
      result
    end
    snapshot = baseline.value
    checks = { "hfsdump" => !!available[:hfsdump], "sleuth" => !!available[:sleuth] }
    comparisons = checks.map do |oracle, present|
      deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + READER_TIMEOUT
      invoke = lambda do |operation, *args|
        remaining = deadline - Process.clock_gettime(Process::CLOCK_MONOTONIC)
        raise CommandTimeout, "per-volume budget of #{READER_TIMEOUT} seconds ran out" if remaining <= 0

        public_send(operation, *args, timeout: remaining)
      end

      comparison(oracle: oracle, volume: volume) do
        raise Skip, "oracle absent" unless present
        raise Skip, "fsimg baseline unavailable" unless snapshot
        if oracle == "sleuth" && mapnodes?(shape)
          raise Skip, "fsstat is too slow on a catalog of this size"
        end

        case oracle
        when "hfsdump"
          members = snapshot.entries
          if mapnodes?(shape)
            puts "CAP hfsdump #{volume.name}: root listing and one member of #{members.length}; a process per member takes over 30 minutes on this catalog"
            members = members.slice(MAPNODES_MEMBER)
          end
          hfsdump_reader(tool: available[:hfsdump], volume: volume, snapshot: snapshot, invoke: invoke, members: members)
        when "sleuth" then sleuth_reader(volume: volume, snapshot: snapshot, invoke: invoke)
        end
      end
    end
    [baseline, *comparisons]
  end

  def run(generator, fsimg)
    available = oracles
    reader_versions(available)

    outcomes = []
    volumes = []
    Dir.mktmpdir("fsimg-hfs-integration-", ENV["TMPDIR"]) do |directory|
      volumes = reader_volumes(directory)
      volumes.each do |volume|
        outcomes.concat(reader_volume(fsimg: fsimg, volume: volume, available: available, directory: directory))
      end
      writer_cases = if ENV["FSIMG_HFS_ONLY"] == "reader" || !available.values.any?
        []
      else
        matrix(large: ENV["FSIMG_HFS_LARGE"] == "1")
      end
      writer_cases.each do |variant, block, shape, size|
        name = "#{variant}-#{block}-#{shape}-#{size || 'auto'}"
        image = File.join(directory, "#{name}.img")
        volume = Volume.new(image: image, name: name, source: "writer", local: true)
        begin
          begin
            generate(generator: generator, image: image, variant: variant, block: block,
              shape: shape, size: size, directory: directory)
            puts image_line(image, name: name)
          rescue StandardError => e
            outcomes << comparison(oracle: "generator", volume: volume) { raise e }
            next
          end
          outcomes.concat(reader_volume(fsimg: fsimg, volume: volume, available: available,
            directory: directory, shape: shape))
          # Each present oracle runs even if another oracle reports a finding.
          checks = {
            native: -> { native_check(image, oracle_log: ENV["FSIMG_HFS_ORACLE_LOG"]) },
            linux: -> { command(available.fetch(:linux), "-n", image) }
          }
          checks.each do |oracle, check|
            next unless available.fetch(oracle)

            outcomes << comparison(oracle: oracle.to_s, volume: volume) do
              check.call
              Result.new
            end
          end
        ensure
          File.unlink(image) if File.exist?(image)
        end
      end
    end
    unmatched = unmatched_sources(sources: reader_sources, volumes: volumes)
    unmatched.each { warn "FINDING explicit #{_1} source matched no images" }
    totals = outcomes.map(&:verdict).tally
    puts "SUMMARY agree=#{totals.fetch('agree', 0)} finding=#{totals.fetch('finding', 0) + unmatched.length} skip=#{totals.fetch('skip', 0)}"
    exit_code(rows: outcomes, unmatched: unmatched)
  end
end

if $PROGRAM_NAME == __FILE__
  exit HfsPlusIntegration.run(File.expand_path(ARGV.fetch(0, "./hfsplus-write-gen")),
    File.expand_path(ARGV.fetch(1, "./fsimg")))
end
