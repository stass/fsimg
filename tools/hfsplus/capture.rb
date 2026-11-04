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
require_relative "populate"

module HfsPlusCapture
  class Error < StandardError; end

  DEFAULT_ROOT = File.join(Dir.home, "hfsplus-captures")
  FIELD_RECORDS = 400
  OUTPUT_LINES = 40
  # What --option may hold: it becomes part of the capture directory's name.
  OPTION_PATTERN = /\A[\w.+-]+\z/

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

    # Run argv, log it, return [combined output, success]. The [cmd, cmd] form
    # of the program keeps a lone element from being handed to a shell.
    def run(*argv)
      out, status = Open3.capture2e([argv.first, argv.first], *argv.drop(1))
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
    # volume is also mounted there, otherwise it is left unmounted. A device
    # named in the output is recorded before the exit status is judged, since
    # hdiutil can attach a disk and still fail, for example to mount it.
    def attach(image, mountpoint: nil)
      args = %w[hdiutil attach -imagekey diskimage-class=CRawDiskImage]
      args += mountpoint ? ["-mountpoint", mountpoint] : ["-nomount"]
      out, ok = @log.run(*args, image)
      device = out[%r{/dev/disk\d+}]
      @devices << device if device && !@devices.include?(device)
      raise Error, "cannot attach #{image}: #{out.lines.first}" unless ok && device

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

    # Detach every device still attached. A failure does not stop the others;
    # the failures are raised together at the end.
    def release
      errors = @devices.dup.filter_map { detach_error(_1) }
      raise Error, errors.join("; ") unless errors.empty?
    end

    # Attach, yield the device, and detach it again: a failed detach on the
    # success path is an error. When the block or the attach fails, the device
    # is detached on a best-effort basis and that error is the one raised; a
    # device that cannot be detached stays listed for `release` to report.
    def with(image, mountpoint: nil)
      device = nil
      device = attach(image, mountpoint: mountpoint)
      result = yield device
      detach(device)
      result
    rescue StandardError
      detach_error(device) if device && @devices.include?(device)
      raise
    end

    private

    def detach_error(device)
      detach(device)
      nil
    rescue Error => e
      e.message
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
    full = File.join(root, path)
    base = { path: path, kind: st.ftype, size: st.size, mode: format("%04o", st.mode & 0o7777),
             mtime: st.mtime.to_i, nlink: st.nlink, owner: st.uid == Process.uid ? "mounter" : "other" }
    base[:target] = File.readlink(full) if st.symlink?
    base[:sha256] = file_digest(full) if st.file?
    base
  end

  # A file the mounting user cannot read, such as mode 0000, still belongs in
  # the manifest; its checksum is nil rather than a reason to stop.
  def file_digest(path) = File.readable?(path) ? Digest::SHA256.file(path).hexdigest : nil
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
  # The mount directory is removed with rmdir, never recursively, and only
  # after the volume is detached: were the detach to fail, a recursive removal
  # would delete the contents of a still-mounted volume. On failure the
  # directory is left in place.
  def populate(config, attachments, log, block_size)
    steps = HfsPlusPopulate.e2(block_size)
    mount = Dir.mktmpdir("hfsplus-mount")
    result = attachments.with(config.image, mountpoint: mount) do
      log.note("# mounted: #{`mount`.lines.grep(/#{Regexp.escape(mount)}/).map { _1[/\(([^)]*)\)/, 1] }.first}")
      attempts = []
      steps.each { HfsPlusPopulate.attempt(attempts, _1, mount) }
      system("sync")
      { attempts: attempts, tree: walk(mount) }
    end
    Dir.rmdir(mount)
    result
  end

  # The part of the manifest that says how the image came to be: a copy of
  # another tool's image or the output of the formatting commands.
  def image_origin(config, attachments, log)
    if config.import
      FileUtils.cp(config.import, config.image)
      { imported_with: config.command, boot_after_format: [{ after: "import", boot: boot_summary(boot_area(config.image)) }] }
    else
      { boot_after_format: format_image(config, attachments, log) }
    end
  end

  # The population and the fsck that follows it.
  def populated(config, attachments, log, formatted)
    block_size = formatted.dig(:volume_header, :block_size) or raise Error, "formatted image has no volume header"
    tree = populate(config, attachments, log, block_size)
    tree.merge(fsck_populated: fsck(attachments, log, config.image))
  end

  # The manifest, put together from what each step returns, and the fields
  # files written along the way.
  def build_manifest(config, attachments, log)
    identity = { name: config.name, variant: config.variant, size: config.size, option: config.option, commands: config.formats }
    origin = image_origin(config, attachments, log)
    formatted = fields_of(config.image)
    File.write(File.join(config.dir, "fields-formatted.json"), fields_text(formatted))
    fsck_formatted = { fsck_formatted: fsck(attachments, log, config.image) }
    population = config.populate ? populated(config, attachments, log, formatted) : {}
    File.write(File.join(config.dir, "fields.json"), fields_text(fields_of(config.image)))
    [identity, origin, fsck_formatted, population].reduce(:merge)
  end

  # Build the manifest, then detach whatever is still attached and write
  # commands.txt, whether or not the build succeeded. A failure to detach is
  # reported, but never replaces the error that ended the build.
  def capture(config)
    log = Log.new
    attachments = Attachments.new(log)
    FileUtils.mkdir_p(config.dir)
    manifest = nil
    begin
      manifest = build_manifest(config, attachments, log)
    ensure
      leftover = release_error(attachments)
      warn "capture.rb: #{leftover.message}" if leftover && manifest.nil?
      File.write(File.join(config.dir, "commands.txt"), log.to_s)
    end
    raise leftover if leftover

    manifest
  end

  def release_error(attachments)
    attachments.release
    nil
  rescue Error => e
    e
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
      o.on("--option NAME", OPTION_PATTERN) { opts[:option] = _1 }
      o.on("--format CMD") { opts[:formats] << _1 }
      o.on("--hdiutil-fs FS") { opts[:hdiutil_fs] = _1 }
      o.on("--import IMAGE") { opts[:import] = _1 }
      o.on("--command TEXT") { opts[:command] = _1 }
      o.on("--populate PROFILE", %w[e2]) { opts[:populate] = _1 }
      o.on("--discard-image") { opts[:discard_image] = true }
    end.parse!(argv)
    missing = %i[variant size option].reject { opts[_1] }
    raise Error, "missing --#{missing.first}" unless missing.empty?
    raise Error, "--size must be positive" unless opts[:size].positive?
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
