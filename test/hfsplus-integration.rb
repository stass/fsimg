# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "open3"
require "tmpdir"
require "timeout"
require "json"
require "digest"
require "find"
require_relative "../tools/hfsplus/capture"

module FsimgSpec
  class Error < StandardError; end
end

module HfsPlusIntegration
  class Error < FsimgSpec::Error; end
  class OracleTimeout < Error; end
  class Skip < Error; end

  READER_TIMEOUT = 60
  # An uncapped e7 capture needs minutes of per-member CLI process startup.
  FILE_READ_CAP = 64
  Volume = Data.define(:image, :name, :source, :local)
  Comparison = Data.define(:value, :result)
  # TSK exposes synthetic system forks, private catalog storage and journal
  # files. fsimg omits these format-owned objects.
  ORACLE_HIDDEN = {
    "sleuth" => %r{\A/(?:\$(?:ExtentsFile|CatalogFile|BadBlockFile|AllocationFile|AttributesFile)|\.HFS\+ Private Directory Data\^|\^{4}HFS\+ Private Data|\.journal|\.journal_info_block)(?:/|\z)}
  }.freeze
  # These are observed oracle defects, scoped to named captures and errors.
  # They yield skips, never agreements. No fsimg failure is exempted.
  ORACLE_LIMITATIONS = [
    # TSK cannot read the fork-backed attribute on attr-sizes in either E5 tree.
    ["sleuth", /\Ah-hfs(?:plus|x)-16777216-e5\z/, "/attr-sizes",
     /Unsupported record type: \(32\)/],
    # TSK omits a directory-hard-link alias child; HFSX lookup fails entirely.
    ["sleuth", /\Ah-hfsplus-8388608-e4\z/, nil,
     /fls paths: missing=\["\/u1\/a"\] \(1\) extra=\[\] \(0\)/],
    ["sleuth", /\Ah-hfsx-8388608-e4\z/, nil,
     /hfs_cat_file_lookup: Error finding thread node for file \(1\)/],
    # TSK lacks LZFSE inline/resource decoding (types 11 and 12).
    ["sleuth", /\Ah-hfs(?:plus|x)-8388608-e6-lzfse\z/,
     %r{\A/(?:forced-mixed-196608|forced-random-1|mixed-196608|random-1|shape-(?:131073|262145|393217|589825)|(?:text|zeros)-(?:1|200000|65535|65536|65537))\z},
     /Attribute 4352 not found/],
    # TSK misreads the resource framing/chunk bounds of these type 8 files.
    ["sleuth", /\Ah-hfsplus-8388608-e6-lzvn\z/,
     %r{\A/(?:forced-mixed-196608|mixed-196608|text-200000|zeros-200000)\z},
     /block size is too large: (?:66145|4294888110|4294965730)/],
    # hfsdump emits bad bytes for these type 8 final partial resource chunks.
    # fsimg_snapshot checks their digests against the native manifest first.
    ["hfsdump", /\Ah-hfsplus-8388608-e6-ditto\z/,
     %r{\A/shape-(?:262145|393217|589825)\z}, /SHA256 differs:/]
  ].map(&:freeze).freeze

  GENERATOR_TIMEOUT = 30 * 60
  BLOCKS = [512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536].freeze
  VARIANTS = %w[hfsplus hfsx].freeze
  LABEL = "Writer volume".freeze
  ROOT_NAMES = ["dir", "empty", "equal", "above", "two", "hello", "cafe\u0301",
                "link", "pipe", "a" * 200, "z" * 200, "Zeta", "alpha",
                "\u03a9", "\u03b1", ":", "\u2400", "\u{1f600}", "\uff21",
                "a\u200cx", "ay"].freeze
  HELLO = (0...5).map { (1 + (8 * 17 + _1 * 13) % 251).chr }.join.b.freeze
  # fsck findings sometimes start with "**" too. Enumerate the progress
  # phases, rather than accepting all lines with that prefix.
  # Apple's fsck_hfs.c (lines 533-534) logs a failed unprivileged read-write
  # open before reopening O_RDONLY, which is sufficient for -n.
  FSCK_PROGRESS = [
    %r{\ACan't open /dev/rdisk\d+: Permission denied\z},
    /\A\*\* Checking non-journaled HFS Plus Volume\.\z/,
    /\A\*\* Detected a case-sensitive volume\.\z/,
    %r{\A\*\* /dev/rdisk\d+(?:s\d+)?(?: \(NO WRITE\))?\z},
    /\A\*\* Checking (?:HFS Plus|HFSX) volume\.\z/,
    /\A\*\* Checking (?:extents overflow file|catalog file|multi-linked files|catalog hierarchy|extended attributes file|volume bitmap|volume information)\.\z/,
    /\A\*\* The volume .+ appears to be OK\.\z/,
    /\A\*\* The volume .+ was formatted by .+\z/,
    /\A(?:\*\* )?The volume name is #{Regexp.escape(LABEL)}\z/,
    /\AExecuting fsck_hfs \(version .+\)\.?\z/
  ].freeze

  module_function

  def find_program(name)
    return name if name.include?("/") && File.file?(name) && File.executable?(name)

    ENV.fetch("PATH", "").split(File::PATH_SEPARATOR).map { File.join(_1, name) }
       .find { File.file?(_1) && File.executable?(_1) }
  end

  def command(*argv, timeout: 30, statuses: [0])
    out, err, status = Open3.popen3({ "LC_ALL" => "C", "TZ" => "UTC" },
                                  [argv.first, argv.first], *argv.drop(1), pgroup: true) do |input, output, errors, child|
      input.close
      stdout = Thread.new { output.read.b }
      stderr = Thread.new { errors.read.b }
      begin
        Timeout.timeout(timeout) { [stdout.value, stderr.value, child.value] }
      rescue Timeout::Error
        begin
          Process.kill("KILL", -child.pid)
        rescue Errno::ESRCH
          nil
        end
        child.value
        raise OracleTimeout, "#{argv.join(' ')}: timed out after #{timeout} seconds"
      ensure
        stdout.join
        stderr.join
      end
    end
    unless statuses.include?(status.exitstatus)
      raise Error, "#{argv.join(' ')}: exit #{status.exitstatus}: #{err.byteslice(0, 8192)}\nstdout excerpt: #{out.byteslice(0, 2048).inspect}"
    end

    out.b
  end

  def oracles
    candidates = {
      native: RUBY_PLATFORM.include?("darwin") && find_program("hdiutil") && find_program("fsck_hfs"),
      hfsdump: find_program(ENV.fetch("HFSDUMP", File.join(Dir.home, ".local/share/fsimg-oracles/hfsfuse/bin/hfsdump"))),
      sleuth: %w[fsstat fls icat].all? { find_program(_1) },
      linux: find_program("fsck.hfsplus"),
      mount: RUBY_PLATFORM.include?("linux") && Process.uid.zero? && find_program("mount") && find_program("umount")
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
        findings = out.lines.map(&:strip).reject do |line|
          line.empty? || FSCK_PROGRESS.any? { _1.match?(line) }
        end
        raise Error, "fsck_hfs findings:\n#{findings.join("\n")}" unless findings.empty?
        raise Error, "fsck_hfs failed or did not check the volume" unless ok && out.include?("appears to be OK")
      end
    ensure
      attachments.release
    end
  end

  def hfsdump_check(tool:, image:, shape:)
    if %w[empty empty-min mapnodes mapnodes-small].include?(shape)
      # hfsdump read / hangs on empty folders, including native newfs_hfs
      # volumes. stat avoids that oracle quirk and still verifies the root.
      root = command(tool, image, "stat", "/")
      unless root.match?(/^type: folder\r?$/) && root.match?(/^cnid: 2\r?$/)
        raise Error, "hfsdump root is not a folder with cnid 2"
      end
      if %w[mapnodes mapnodes-small].include?(shape)
        # hwf_add_mapnodes emits eight digits followed by 232 'n' bytes.
        path = "/00000000#{'n' * 232}"
        raise Error, "hfsdump synthetic file payload differs" unless command(tool, image, "read", path).empty?
      end
      return
    end

    listing = command(tool, image, "read", "/")

    # hfsdump maps stored slash to colon. puts truncates the one-unit NUL
    # name to an empty line; that line represents U+2400 in path spelling.
    names = listing.force_encoding("UTF-8").lines.map { _1.chomp.empty? ? "\u2400" : _1.chomp }
    ROOT_NAMES.each do |name|
      raise Error, "hfsdump listing omits #{name.inspect}" unless names.include?(name)
    end
    raise Error, "hfsdump hello payload differs" unless command(tool, image, "read", "/hello") == HELLO
  end

  def sleuth_check(image:, empty:)
    command("fsstat", image)
    listing = command("fls", image)
    return if empty

    names = listing.force_encoding("UTF-8").lines.filter_map { _1.split(":\t", 2)[1]&.strip }
    ROOT_NAMES.each do |name|
      # fls versions expose stored slash/NUL or their printable replacement
      # '^'; accept those spellings only for the two mapped catalog names.
      displayed = case name
                  when ":" then [":", "/", "^"]
                  when "\u2400" then ["\u2400", "^", ""]
                  else [name]
                  end
      raise Error, "fls listing omits #{name.inspect}" unless displayed.any? { names.include?(_1) }
    end
    inode = listing.lines.filter_map { _1[/\Ar\/r\s+([^:]+):\s+hello\s*\z/, 1] }.first
    raise Error, "fls listing omits hello" unless inode
    raise Error, "icat hello payload differs" unless command("icat", image, inode) == HELLO
  end

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
    check_mapnodes(image) if %w[mapnodes mapnodes-small].include?(shape)
    check_nearlyfull(image) if %w[nearlyfull nearlyfull-small].include?(shape)
  end

  def image_line(image, name:)
    block_bytes = File.binread(image, 4, 1024 + 40)
    raise Error, "truncated volume header blockSize" unless block_bytes.bytesize == 4

    "IMAGE #{name} size=#{File.size(image)} block=#{block_bytes.unpack1('N')}"
  end

  def reader_versions(available)
    if available[:hfsdump]
      puts "VERSION hfsdump hfsfuse eed780cbe0445893de6f4aa8d74461da00756984 (build-hfsfuse.sh pin)"
    end
    puts "VERSION sleuth #{command('fls', '-V').strip}" if available[:sleuth]
    puts "VERSION linux-mount #{command('uname', '-sr').strip}"
    puts "SKIP linux-mount: needs root on Linux" unless available[:mount]
  end

  def verification_rows
    File.readlines(File.join(__dir__, "hfsplus-verify-expected.tsv"), chomp: true)
      .reject { _1.start_with?("#") }.map { _1.split("\t", -1) }
  end

  def reader_volumes(directory)
    native = ENV["FSIMG_HFSPLUS_NATIVE"]
    corpus = ENV["FSIMG_HFSPLUS_CORPUS"]
    sources = {
      "native" => native && File.join(File.expand_path(native), "*", "*.img"),
      "linux-mkfs-hfsplus" => corpus && File.join(File.expand_path(corpus), "linux-mkfs-hfsplus", "*.img")
    }
    volumes = sources.flat_map do |source, pattern|
      paths = pattern ? Dir[pattern].sort : []
      puts "SKIP #{source}: #{pattern || 'environment variable unset'}: no images" if paths.empty?
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
    verification_rows.find { _1[0] == source && _1[1] == name }
  end

  def fsimg_snapshot(fsimg:, volume:, stats:)
    expected = expected_verification(volume)
    status = expected ? expected.fetch(3) : "ok"
    verification = json_records(command(fsimg, "W", "--json", "-f", volume.image,
      timeout: READER_TIMEOUT, statuses: status == "ok" ? [0] : [1]))
    if status != "ok"
      error = verification.find { _1["type"] == "error" }
      unless error && error["status"] == status && error.fetch("message").include?(expected.fetch(4))
        raise Error, "verification expected #{expected[3..4].inspect}, got #{verification.inspect}"
      end
    else
      report = verification.find { _1["type"] == "verify" }
      raise Error, "verification incomplete" unless report && report["complete"]

      if expected
        fields = %w[files directories allocated_clusters free_clusters bad_clusters]
        fields.zip(expected.drop(5)).each do |field, count|
          raise Error, "verification #{field}: expected #{count}, got #{report[field]}" unless report[field].to_s == count
        end
      end
    end
    listing = json_records(command(fsimg, "t", "--json", "-f", volume.image,
      timeout: READER_TIMEOUT, statuses: status == "ok" ? [0] : [0, 1]))
    if status != "ok"
      puts "EXPECTED fsimg #{volume.source}/#{volume.name}: #{status}: #{expected.fetch(4)}"
      raise Skip, "expected verification refusal; no verified comparison baseline"
    end
    entries = listing.select { _1["type"] == "entry" }.to_h { [_1.fetch("path"), _1] }
    count = listing.count { _1["type"] == "entry" }
    raise Error, "duplicate fsimg listing paths" unless count == entries.length

    stats[:entries] = entries.length
    manifest_path = File.join(File.dirname(volume.image), "manifest.json")
    manifest = if volume.source == "native" && File.file?(manifest_path)
      JSON.parse(File.read(manifest_path)).fetch("tree", []).to_h { ["/#{_1.fetch('path')}", _1] }
    else
      {}
    end
    files = entries.values.select { %w[file symlink].include?(_1.fetch("kind")) }
    if files.length > FILE_READ_CAP
      puts "CAP fsimg #{volume.source}/#{volume.name}: reading #{FILE_READ_CAP}/#{files.length} files and symlinks (per-file CLI startup cost)"
    end
    hashes = files.first(FILE_READ_CAP).to_h do |entry|
      path = entry.fetch("path")
      bytes = command(fsimg, "x", "-O", "-f", volume.image, "--", path, timeout: READER_TIMEOUT)
      size = Integer(entry.fetch("size_bytes"))
      raise Error, "#{path.inspect}: fsimg read #{bytes.bytesize} bytes, listing says #{size}" unless bytes.bytesize == size

      stats[:files] += 1
      stats[:bytes] += bytes.bytesize
      digest = Digest::SHA256.hexdigest(bytes)
      observation = manifest[path]
      if observation && observation["sha256"] && observation["sha256"] != digest
        raise Error, "#{path.inspect}: fsimg SHA256 #{digest} differs from native #{observation.fetch('sha256')}"
      end
      if observation && observation["target"] && observation["target"].b != bytes
        raise Error, "#{path.inspect}: fsimg symlink target differs from native manifest"
      end
      [path, digest]
    end
    { entries: entries.freeze, hashes: hashes.freeze }.freeze
  end

  def same_paths(expected:, actual:, context:)
    missing, extra = expected - actual, actual - expected
    return if missing.empty? && extra.empty?

    raise Error, "#{context}: missing=#{missing.first(20).inspect} (#{missing.length}) extra=#{extra.first(20).inspect} (#{extra.length})"
  end

  def same_bytes(bytes:, path:, snapshot:, stats:)
    actual = Digest::SHA256.hexdigest(bytes)
    expected = snapshot.fetch(:hashes).fetch(path)
    unless actual == expected
      raise Error, "#{path.inspect}: SHA256 differs: fsimg=#{expected} oracle=#{actual}"
    end
    stats[:files] += 1
    stats[:bytes] += bytes.bytesize
  end

  def known_limitation?(oracle:, volume:, path:, message:)
    volume.source == "native" && ORACLE_LIMITATIONS.any? do |tool, name, member, error|
      tool == oracle && name.match?(volume.name) && member === path && error.match?(message)
    end
  end

  # Continue after a member disagreement so a single bad file cannot hide
  # other findings. A timeout ends the volume rather than restarting a budget.
  def compare_members(entries, oracle:, volume:)
    findings = []
    skips = []
    entries.each do |path, entry|
      begin
        yield(path, entry)
      rescue OracleTimeout
        raise if findings.empty?

        puts "SKIP #{oracle} #{volume.source}/#{volume.name}: timed out after #{READER_TIMEOUT} seconds"
        raise Error, findings.join("\n")
      rescue Error => e
        if known_limitation?(oracle: oracle, volume: volume, path: path, message: e.message)
          skips << path
          puts "SKIP #{oracle} #{volume.source}/#{volume.name} #{path.inspect}: known oracle limitation: #{e.message}"
        else
          findings << "#{path.inspect}: #{e.message}"
        end
      end
    end
    raise Error, findings.join("\n") unless findings.empty?
    raise Skip, "#{skips.length} members have known oracle limitations" unless skips.empty?
  end

  def hfsdump_reader(tool:, volume:, snapshot:, stats:, invoke:)
    entries = snapshot.fetch(:entries)
    # Include root even when fsimg's member listing is empty. Only directories
    # with known children are read: hfsdump hangs reading an empty directory.
    root = { "kind" => "directory", "size_bytes" => "0" }
    compare_members(entries.merge("/" => root), oracle: "hfsdump", volume: volume) do |path, entry|
      stat = invoke.call(tool, volume.image, "stat", path)
      folder = entry.fetch("kind") == "directory"
      wanted = folder ? "folder" : "file"
      raise Error, "#{path.inspect}: hfsdump type differs: #{stat}" unless stat[/^type: (.+)$/, 1] == wanted

      unless folder
        size = stat[/^(?:decmpfs.logical_size|data_fork.logical_size): (\d+)$/, 1]
        # Compressed size is emitted after the raw fork size.
        size = stat[/^decmpfs.logical_size: (\d+)$/, 1] || size
        unless size && Integer(size) == Integer(entry.fetch("size_bytes"))
          raise Error, "#{path.inspect}: hfsdump size #{size.inspect}, fsimg #{entry.fetch('size_bytes')}"
        end
        mode = stat[/^permissions.file_mode: ([0-7]+)$/, 1]
        type = { "file" => 0o100000, "symlink" => 0o120000, "fifo" => 0o010000 }.fetch(entry.fetch("kind"))
        if mode && (Integer(mode, 8) & 0o170000) != type
          raise Error, "#{path.inspect}: hfsdump mode #{mode}, fsimg kind #{entry.fetch('kind')}"
        end
      end
      stats[:entries] += 1
      if folder
        children = entries.keys.select { File.dirname(_1) == path && _1 != path }.map { File.basename(_1) }
        next if children.empty?

        names = invoke.call(tool, volume.image, "read", path).force_encoding("UTF-8").lines.map(&:chomp)
        same_paths(expected: children, actual: names, context: "#{path.inspect}: hfsdump children")
      elsif snapshot.fetch(:hashes).key?(path)
        same_bytes(bytes: invoke.call(tool, volume.image, "read", path), path: path, snapshot: snapshot, stats: stats)
      end
    end
  end

  def sleuth_reader(volume:, snapshot:, stats:, invoke:)
    invoke.call("fsstat", volume.image)
    listing = invoke.call("fls", "-r", "-p", volume.image).force_encoding("UTF-8")
    nodes = listing.lines.filter_map do |line|
      match = /\A(\S+)\s+(\S+):\t(.*)\n?\z/.match(line)
      raise Error, "unrecognized fls record: #{line.inspect}" unless match

      ["/#{match[3]}", { inode: match[2], type: match[1] }]
    end.to_h.reject { |path, _| ORACLE_HIDDEN.fetch("sleuth").match?(path) }
    path_problem = nil
    begin
      same_paths(expected: snapshot.fetch(:entries).keys, actual: nodes.keys, context: "fls paths")
    rescue Error => e
      raise unless known_limitation?(oracle: "sleuth", volume: volume, path: nil, message: e.message)

      path_problem = e.message
      puts "SKIP sleuth #{volume.source}/#{volume.name}: known oracle limitation: #{path_problem}"
    end
    reported = snapshot.fetch(:entries).select { |path, _| nodes.key?(path) }
    compare_members(reported, oracle: "sleuth", volume: volume) do |path, entry|
      node = nodes.fetch(path)
      type = { "directory" => "d", "file" => "r", "symlink" => "l", "fifo" => "p" }.fetch(entry.fetch("kind"))
      raise Error, "#{path.inspect}: fls type #{node[:type]}, fsimg #{type}" unless node.fetch(:type).split("/").last == type

      stats[:entries] += 1
      next unless snapshot.fetch(:hashes).key?(path)

      same_bytes(bytes: invoke.call("icat", volume.image, node.fetch(:inode)), path: path, snapshot: snapshot, stats: stats)
    end
    raise Skip, path_problem if path_problem
  end

  def mounted_reader(volume:, snapshot:, stats:, directory:)
    raise Skip, "only locally generated volumes may be mounted" unless volume.local

    mountpoint = File.join(directory, "mount")
    Dir.mkdir(mountpoint) unless File.directory?(mountpoint)
    mounted = false
    begin
      begin
        command("mount", "-t", "hfsplus", "-o", "ro,loop", volume.image, mountpoint)
        mounted = true
      rescue OracleTimeout
        raise
      rescue Error => e
        raise Skip, "mount refused: #{e.message}"
      end
      paths = []
      Find.find(mountpoint) { paths << _1.delete_prefix(mountpoint) unless _1 == mountpoint }
      same_paths(expected: snapshot.fetch(:entries).keys, actual: paths, context: "mounted paths")
      snapshot.fetch(:entries).each do |path, entry|
        host = File.join(mountpoint, path.delete_prefix("/"))
        stat = File.lstat(host)
        kind = { "directory" => "directory", "file" => "file", "symlink" => "link", "fifo" => "fifo" }.fetch(entry.fetch("kind"))
        raise Error, "#{path.inspect}: mounted type #{stat.ftype}, fsimg #{kind}" unless stat.ftype == kind

        stats[:entries] += 1
        next unless snapshot.fetch(:hashes).key?(path)

        bytes = stat.symlink? ? File.readlink(host).b : File.binread(host)
        same_bytes(bytes: bytes, path: path, snapshot: snapshot, stats: stats)
      end
    ensure
      command("umount", mountpoint) if mounted
    end
  end

  # This per-oracle boundary records decoding, host I/O and command failures.
  # It reports partial counts; a timeout can never produce an agreement.
  def comparison(oracle:, volume:, failures:)
    stats = { entries: 0, files: 0, bytes: 0 }
    result = "agree"
    value = nil
    begin
      value = yield(stats)
    rescue OracleTimeout
      result = "skip"
      puts "SKIP #{oracle} #{volume.source}/#{volume.name}: timed out after #{READER_TIMEOUT} seconds"
    rescue Skip => e
      result = "skip"
      puts "SKIP #{oracle} #{volume.source}/#{volume.name}: #{e.message}"
    rescue StandardError => e
      if known_limitation?(oracle: oracle, volume: volume, path: nil, message: e.message)
        result = "skip"
        puts "SKIP #{oracle} #{volume.source}/#{volume.name}: known oracle limitation: #{e.message}"
      else
        result = "finding"
        failures << "#{oracle} #{volume.source}/#{volume.name}: #{e.message}"
        warn "FINDING #{failures.last}"
      end
    ensure
      puts "COMPARE #{oracle} #{volume.source}/#{volume.name} entries=#{stats[:entries]} files=#{stats[:files]} bytes=#{stats[:bytes]} result=#{result}"
    end
    Comparison.new(value: value, result: result)
  end

  def reader_volume(fsimg:, volume:, available:, failures:, directory:, checks: nil)
    baseline = comparison(oracle: "fsimg", volume: volume, failures: failures) do |stats|
      fsimg_snapshot(fsimg: fsimg, volume: volume, stats: stats)
    end
    snapshot = baseline.value
    checks ||= { "hfsdump" => available[:hfsdump], "sleuth" => available[:sleuth], "linux-mount" => available[:mount] }
    comparisons = checks.map do |oracle, present|
      comparison(oracle: oracle, volume: volume, failures: failures) do |stats|
        raise Skip, "oracle absent or host ineligible" unless present
        raise Skip, "fsimg baseline unavailable" unless snapshot

        deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + READER_TIMEOUT
        invoke = lambda do |*args|
          remaining = deadline - Process.clock_gettime(Process::CLOCK_MONOTONIC)
          raise OracleTimeout if remaining <= 0

          command(*args, timeout: remaining)
        end
        case oracle
        when "hfsdump"
          hfsdump_reader(tool: present, volume: volume, snapshot: snapshot, stats: stats, invoke: invoke)
        when "sleuth"
          sleuth_reader(volume: volume, snapshot: snapshot, stats: stats, invoke: invoke)
        when "linux-mount"
          mounted_reader(volume: volume, snapshot: snapshot, stats: stats, directory: directory)
        end
      end
    end
    [baseline, *comparisons]
  end

  def run(generator, fsimg)
    available = oracles
    reader_versions(available)

    failures = []
    outcomes = []
    Dir.mktmpdir("fsimg-hfs-integration-", ENV["TMPDIR"]) do |directory|
      reader_volumes(directory).each do |volume|
        outcomes.concat(reader_volume(fsimg: fsimg, volume: volume, available: available, failures: failures, directory: directory))
      end
      writer_cases = if ENV["FSIMG_HFS_ONLY"] == "reader" || !available.values.any?
        []
      else
        matrix(large: ENV["FSIMG_HFS_LARGE"] == "1")
      end
      writer_cases.each do |variant, block, shape, size|
        name = "#{variant}-#{block}-#{shape}-#{size || 'auto'}"
        image = File.join(directory, "#{name}.img")
        begin
          generate(generator: generator, image: image, variant: variant, block: block,
                   shape: shape, size: size, directory: directory)
          puts image_line(image, name: name)
          volume = Volume.new(image: image, name: name, source: "writer", local: true)
          outcomes.concat(reader_volume(fsimg: fsimg, volume: volume, available: available,
            failures: failures, directory: directory, checks: { "linux-mount" => available[:mount] }))
          empty = %w[empty empty-min].include?(shape)
          # Each present oracle runs even if another oracle reports a finding.
          checks = {
            native: -> { native_check(image, oracle_log: ENV["FSIMG_HFS_ORACLE_LOG"]) },
            hfsdump: -> { hfsdump_check(tool: available.fetch(:hfsdump), image: image, shape: shape) },
            sleuth: -> { sleuth_check(image: image, empty: empty) },
            linux: -> { command(available.fetch(:linux), "-n", image) }
          }
          checks.each do |oracle, check|
            next unless available.fetch(oracle)

            outcomes << comparison(oracle: oracle.to_s, volume: volume, failures: failures) do |_stats|
              if oracle == :sleuth && %w[mapnodes mapnodes-small].include?(shape)
                raise Skip, "fsstat is too slow on a catalog of this size"
              end
              check.call
            end
          end
        rescue Error => e
          failures << "#{name}: #{e.message}"
          warn "FINDING #{failures.last}"
        ensure
          File.unlink(image) if File.exist?(image)
        end
      end
    end
    totals = outcomes.map(&:result).tally
    puts "SUMMARY agree=#{totals.fetch('agree', 0)} finding=#{totals.fetch('finding', 0)} skip=#{totals.fetch('skip', 0)}"
    return 1 unless failures.empty?

    applicable = ENV["FSIMG_HFS_ONLY"] == "reader" ? available.values_at(:hfsdump, :sleuth, :mount) : available.values
    applicable.any? ? 0 : 77
  end
end

if $PROGRAM_NAME == __FILE__
  exit HfsPlusIntegration.run(File.expand_path(ARGV.fetch(0, "./hfsplus-write-gen")),
    File.expand_path(ARGV.fetch(1, "./fsimg")))
end
