# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "open3"
require "tmpdir"
require "timeout"
require_relative "../tools/hfsplus/capture"

module FsimgSpec
  class Error < StandardError; end
end

module HfsPlusIntegration
  class Error < FsimgSpec::Error; end

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

  def command(*argv, timeout: 30)
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
        raise Error, "#{argv.join(' ')}: timed out after #{timeout} seconds"
      ensure
        stdout.join
        stderr.join
      end
    end
    raise Error, "#{argv.join(' ')}: exit #{status.exitstatus}: #{err}\n#{out}" unless status.success?

    out.b
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

  def run(generator)
    available = oracles
    return 77 unless available.values.any?

    failures = []
    Dir.mktmpdir("fsimg-hfs-integration-", ENV["TMPDIR"]) do |directory|
      matrix(large: ENV["FSIMG_HFS_LARGE"] == "1").each do |variant, block, shape, size|
        name = "#{variant}-#{block}-#{shape}-#{size || 'auto'}"
        image = File.join(directory, "#{name}.img")
        begin
          generate(generator: generator, image: image, variant: variant, block: block,
                   shape: shape, size: size, directory: directory)
          puts image_line(image, name: name)
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

            if oracle == :sleuth && %w[mapnodes mapnodes-small].include?(shape)
              puts "SKIP sleuth #{image}: fsstat is too slow on a catalog of this size"
              next
            end

            begin
              check.call
              puts "  PASS #{oracle}"
            rescue Error, HfsPlusCapture::Error => e
              failures << "#{name} #{oracle}: #{e.message}"
              warn "FINDING #{failures.last}"
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
    failures.empty? ? 0 : 1
  end
end

exit HfsPlusIntegration.run(File.expand_path(ARGV.fetch(0, "./hfsplus-write-gen"))) if $PROGRAM_NAME == __FILE__
