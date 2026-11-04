#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Create, populate and unmount one HFS Plus or HFSX volume through the native
# macOS tools, and write its capture directory:
#
#   OUT_ROOT/h-VARIANT-BYTES-OPTION/
#     image.img      the volume (omitted with --discard-image)
#     commands.txt   every command run, with its exit status and output
#     manifest.json  names, kinds, sizes, modes, times and checksums of the
#                    populated tree, the attempts the system refused, the boot
#                    area after each formatting step and the fsck_hfs results
#     fields.json    fields.rb of the image after the last step, and
#     fields-formatted.json before the volume was mounted, when it was
#
# Everything the script attaches it detaches again, on failure too. Example:
#
#   capture.rb --variant hfsplus --size 8388608 --option newfs-b4096 \
#     --format 'newfs_hfs -v FSIMG -b 4096 @DEV@' --populate e2
#
# `@DEV@` in a --format command stands for the raw device of the attached
# image. --hdiutil-fs FS formats with `hdiutil create` instead, and --import
# IMAGE records an image another tool made (--command says how).
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "shellwords"
require "tmpdir"
require_relative "fields"

module HfsPlusCapture
  class Error < StandardError; end

  DEFAULT_ROOT = File.join(Dir.home, "hfsplus-captures")
  FIELD_RECORDS = 400
  OUTPUT_LINES = 40

  Config = Data.define(:root, :variant, :size, :option, :formats, :hdiutil_fs, :import, :command, :populate, :discard_image) do
    def name = "h-#{variant}-#{size}-#{option}"
    def dir = File.join(root, name)
    def image = File.join(dir, "image.img")
  end

  # Appends what was run to commands.txt and runs it.
  class Log
    def initialize = @lines = []
    def to_s = @lines.join("\n") << "\n"
    def note(text) = @lines << text

    def shown_command(argv) = argv.map { _1.match?(%r{\A[\w/.@:=+,-]+\z}) ? _1 : _1.shellescape }.join(" ")

    # Run argv, log it, return [combined output, success].
    def run(*argv)
      out, status = Open3.capture2e(*argv)
      shown = out.lines.first(OUTPUT_LINES).map(&:chomp)
      @lines.push("$ #{shown_command(argv)}", *shown.map { "  #{_1}" }, "  [exit #{status.exitstatus}]")
      [out, status.success?]
    rescue SystemCallError => e
      @lines.push("$ #{shown_command(argv)}", "  [#{e.message}]")
      ["", false]
    end
  end

  # The images this process attached, detached again by `release`.
  class Attachments
    def initialize(log)
      @log = log
      @devices = []
    end

    # Attach `image` and return its whole-disk device; with `mountpoint` the
    # volume is also mounted there, otherwise it is left unmounted.
    def attach(image, mountpoint: nil)
      args = %w[hdiutil attach -imagekey diskimage-class=CRawDiskImage]
      args += mountpoint ? ["-mountpoint", mountpoint] : ["-nomount"]
      out, ok = @log.run(*args, image)
      device = out[%r{/dev/disk\d+}]
      raise Error, "cannot attach #{image}: #{out.lines.first}" unless ok && device

      @devices << device
      device
    end

    def detach(device)
      attempts = [[], [], [], ["-force"]]
      done = attempts.any? do |extra|
        _, ok = @log.run("hdiutil", "detach", device, *extra)
        sleep(0.5) unless ok
        ok
      end
      raise Error, "cannot detach #{device}" unless done

      @devices.delete(device)
    end

    def release = @devices.dup.each { detach(_1) }

    # Attach, yield the device, and detach whatever happens.
    def with(image, mountpoint: nil)
      device = attach(image, mountpoint: mountpoint)
      yield device
    ensure
      detach(device) if device && @devices.include?(device)
    end
  end

  # What E2 puts on a volume of block size B. Each step is attempted; a
  # refusal by the system is recorded as an observation, not an error.
  module Populate
    TIME_1904 = -2_082_844_800
    TIME_1969 = -17_000_000
    TIME_2040_LIMIT = 2_212_122_495
    TIME_2040_BEYOND = 2_222_222_222

    module_function

    def pattern(length, seed) = Array.new(length) { (_1 * 7 + seed) & 0xff }.pack("C*")

    def attempt(attempts, step, path)
      yield
      attempts << { step: step, path: path, ok: true }
    rescue SystemCallError => e
      attempts << { step: step, path: path, ok: false, error: e.class.name.delete_prefix("Errno::"), message: e.message }
    end

    def write_file(root, rel, content, mode: nil)
      File.binwrite(File.join(root, rel), content)
      File.chmod(mode, File.join(root, rel)) if mode
    end

    # Entry lists as [step, relative path, lambda].
    def e2(block_size)
      sizes = [0, 1, block_size - 1, block_size, block_size + 1]
      names = { "u001" => "a", "u254" => "b" * 254, "u255" => "c" * 255, "u256" => "d" * 256 }
      colon_names = ["colon:mid", ":lead", "trail:", "a::b"]
      links = [1, 255, 1023, 1024, 1025, 2048].map { ["symlink-#{_1}", "t" * _1] }
      depth = %w[n1 n2 n3 n4 n5 n6 n7 n8]
      times = { "time-1904" => TIME_1904, "time-1969" => TIME_1969, "time-2040-limit" => TIME_2040_LIMIT, "time-2040-beyond" => TIME_2040_BEYOND }
      modes = { "mode-0600" => 0o600, "mode-0755" => 0o755, "mode-4755" => 0o4755, "mode-0000" => 0o000 }

      sizes.map { |n| ["file-size", "size-#{n}", ->(root) { write_file(root, "size-#{n}", pattern(n, n)) }] } +
        [["nested-dirs", File.join(*depth), lambda { |root|
          FileUtils.mkdir_p(File.join(root, *depth))
          write_file(root, File.join(*depth, "leaf"), "leaf\n")
        }]] +
        names.map { |label, name| ["name-#{label}", name[0, 20], ->(root) { write_file(root, name, label) }] } +
        colon_names.map { |name| ["name-colon", name, ->(root) { write_file(root, name, name) }] } +
        links.map { |name, target| ["symlink", name, ->(root) { File.symlink(target, File.join(root, name)) }] } +
        [["fifo", "fifo", ->(root) { system("mkfifo", File.join(root, "fifo")) || raise(Errno::EPERM, "mkfifo") }]] +
        times.map { |name, t| ["file-time", name, ->(root) { write_file(root, name, name); File.utime(t, t, File.join(root, name)) }] } +
        [["dir-time", "dir-time-1904", ->(root) { Dir.mkdir(File.join(root, "dir-time-1904")); File.utime(TIME_1904, TIME_1904, File.join(root, "dir-time-1904")) }],
         ["symlink-time", "symlink-time-1969", ->(root) { File.symlink("size-1", File.join(root, "symlink-time-1969")); File.lutime(TIME_1969, TIME_1969, File.join(root, "symlink-time-1969")) }]] +
        modes.map { |name, mode| ["file-mode", name, ->(root) { write_file(root, name, name, mode: mode) }] } +
        [["dir-mode", "dir-mode-1777", ->(root) { Dir.mkdir(File.join(root, "dir-mode-1777")); File.chmod(0o1777, File.join(root, "dir-mode-1777")) }],
         ["dir-mode", "dir-mode-0500", ->(root) { Dir.mkdir(File.join(root, "dir-mode-0500")); File.chmod(0o500, File.join(root, "dir-mode-0500")) }],
         ["chown", "size-1", ->(root) { File.chown(0, nil, File.join(root, "size-1")) }],
         ["chgrp", "size-1", ->(root) { File.chown(nil, Process.groups.reject { _1 == Process.gid }.first || 0, File.join(root, "size-1")) }]]
    end
  end

  module_function

  # One manifest entry per path under `root`, depth first in name order.
  def walk(root, rel = "")
    dir = File.join(root, rel)
    Dir.children(dir).sort.flat_map do |name|
      path = rel.empty? ? name : File.join(rel, name)
      st = File.lstat(File.join(root, path))
      [entry(root, path, st)] + (st.directory? ? walk(root, path) : [])
    end
  end

  def entry(root, path, st)
    base = { path: path, kind: st.ftype, size: st.size, mode: format("%04o", st.mode & 0o7777),
             mtime: st.mtime.to_i, nlink: st.nlink, owner: st.uid == Process.uid ? "mounter" : "other" }
    base[:target] = File.readlink(File.join(root, path)) if st.symlink?
    base[:sha256] = File.readable?(File.join(root, path)) ? file_digest(File.join(root, path)) : nil if st.file?
    base
  end

  def file_digest(path) = Digest::SHA256.file(path).hexdigest
  def boot_area(image) = File.open(image, "rb") { _1.read(1024).to_s }
  def boot_summary(bytes) = { zero: bytes.count("\x00".b) == 1024, first_16: bytes.byteslice(0, 16).unpack1("H*"), sha256: Digest::SHA256.hexdigest(bytes) }
  def fields_of(image) = File.open(image, "rb") { HfsPlusFields.decode(_1, limit: FIELD_RECORDS) }
  def fields_text(report) = JSON.pretty_generate(report)

  def fsck(attachments, log, image)
    attachments.with(image) do |device|
      out, ok = log.run("fsck_hfs", "-n", device.sub("/dev/", "/dev/r"))
      { passed: ok, summary: out.lines.last(2).map(&:strip).reject(&:empty?) }
    end
  end

  # Format `image` with each --format command in turn, or by hdiutil, noting
  # the boot area after every step.
  def format_image(config, attachments, log)
    FileUtils.rm_f(config.image)
    return hdiutil_create(config, log) if config.hdiutil_fs

    log.run("mkfile", "-n", config.size.to_s, config.image)
    config.formats.map do |command|
      attachments.with(config.image) do |device|
        log.run(*command.shellsplit.map { _1.gsub("@DEV@", device.sub("/dev/", "/dev/r")) })
      end
      { after: command, boot: boot_summary(boot_area(config.image)) }
    end
  end

  def hdiutil_create(config, log)
    Dir.mktmpdir("hfsplus-capture") do |tmp|
      sparse = File.join(tmp, "v")
      _, ok = log.run("hdiutil", "create", "-size", config.size.to_s, "-fs", config.hdiutil_fs, "-volname", "FSIMG", "-layout", "NONE", "-type", "SPARSE", sparse)
      raise Error, "hdiutil create failed" unless ok

      raw = File.join(tmp, "raw")
      _, ok = log.run("hdiutil", "convert", "#{sparse}.sparseimage", "-format", "UDTO", "-o", raw)
      raise Error, "hdiutil convert failed" unless ok

      FileUtils.mv("#{raw}.cdr", config.image)
    end
    [{ after: "hdiutil create -fs #{config.hdiutil_fs}", boot: boot_summary(boot_area(config.image)) }]
  end

  # Mount the volume writable, run the population, record the tree, unmount.
  def populate(config, attachments, log, block_size)
    steps = Populate.e2(block_size)
    attempts = []
    Dir.mktmpdir("hfsplus-mount") do |mount|
      attachments.with(config.image, mountpoint: mount) do |device|
        log.note("# mounted: #{`mount`.lines.grep(/#{Regexp.escape(mount)}/).map { _1[/\(([^)]*)\)/, 1] }.first}")
        steps.each { |step, path, action| Populate.attempt(attempts, step, path) { action.call(mount) } }
        system("sync")
        tree = walk(mount)
        attachments.detach(device)
        return { attempts: attempts, tree: tree }
      end
    end
  end

  def capture(config)
    log = Log.new
    attachments = Attachments.new(log)
    FileUtils.mkdir_p(config.dir)
    manifest = { name: config.name, variant: config.variant, size: config.size, option: config.option, commands: config.formats }
    if config.import
      FileUtils.cp(config.import, config.image)
      manifest[:imported_with] = config.command
      manifest[:boot_after_format] = [{ after: "import", boot: boot_summary(boot_area(config.image)) }]
    else
      manifest[:boot_after_format] = format_image(config, attachments, log)
    end
    formatted = fields_of(config.image)
    File.write(File.join(config.dir, "fields-formatted.json"), fields_text(formatted))
    manifest[:fsck_formatted] = fsck(attachments, log, config.image)
    if config.populate
      block_size = formatted.dig(:volume_header, :block_size) or raise Error, "formatted image has no volume header"
      manifest.merge!(populate(config, attachments, log, block_size))
      manifest[:fsck_populated] = fsck(attachments, log, config.image)
    end
    File.write(File.join(config.dir, "fields.json"), fields_text(fields_of(config.image)))
    manifest
  ensure
    attachments&.release
    File.write(File.join(config.dir, "commands.txt"), log.to_s) if log && Dir.exist?(config.dir)
  end

  def finish(config, manifest)
    File.write(File.join(config.dir, "manifest.json"), JSON.pretty_generate(manifest))
    FileUtils.rm_f(config.image) if config.discard_image
  end

  def parse(argv)
    opts = { root: DEFAULT_ROOT, formats: [], discard_image: false }
    OptionParser.new do |o|
      o.banner = "usage: capture.rb --variant V --size BYTES --option NAME (--format CMD... | --hdiutil-fs FS | --import IMAGE) [--populate e2]"
      o.on("--root DIR") { opts[:root] = _1 }
      o.on("--variant V", %w[hfsplus hfsx]) { opts[:variant] = _1 }
      o.on("--size BYTES", Integer) { opts[:size] = _1 }
      o.on("--option NAME") { opts[:option] = _1 }
      o.on("--format CMD") { opts[:formats] << _1 }
      o.on("--hdiutil-fs FS") { opts[:hdiutil_fs] = _1 }
      o.on("--import IMAGE") { opts[:import] = _1 }
      o.on("--command TEXT") { opts[:command] = _1 }
      o.on("--populate PROFILE", %w[e2]) { opts[:populate] = _1 }
      o.on("--discard-image") { opts[:discard_image] = true }
    end.parse!(argv)
    missing = %i[variant size option].reject { opts[_1] }
    raise Error, "missing --#{missing.first}" unless missing.empty?
    raise Error, "give --format, --hdiutil-fs or --import" if opts[:formats].empty? && !opts[:hdiutil_fs] && !opts[:import]

    Config.new(**{ hdiutil_fs: nil, import: nil, command: nil, populate: nil }.merge(opts))
  end

  def main(argv)
    config = parse(argv)
    manifest = capture(config)
    finish(config, manifest)
    puts config.dir
    0
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "capture.rb: #{e.message}"
    1
  end
end

exit(HfsPlusCapture.main(ARGV)) if $PROGRAM_NAME == __FILE__
