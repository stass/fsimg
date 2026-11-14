#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Make one APFS container image with the native macOS tools, optionally
# populate it, check it with fsck_apfs, and write its capture directory:
#
#   $FSIMG_APFS_NATIVE/apfs-KIND-SIZE-OPTION/
#     image.img      the raw container, SIZE bytes
#     commands.txt   every command run, with its exit status and the start of
#                    its output
#     fsck.txt       the whole output of fsck_apfs -n
#     manifest.json  the commands as argument lists with their exit statuses,
#                    the host (sw_vers, uname), the identity of each tool run
#                    (path and SHA-256), the volumes asked for, the populate
#                    script's path and SHA-256, the image size and SHA-256, and
#                    the fsck_apfs command, exit status and output
#
# KIND names the producer: `newfs` runs mkfile -n (a sparse file), attaches it
# with hdiutil as a raw disk image and formats it with newfs_apfs; `hdiutil`
# runs hdiutil create -fs APFS -layout NONE and converts the result to a raw
# image, which is as large on disk as SIZE. OPTION labels the capture and
# becomes part of its name. Nothing needs administrator rights.
#
# The capture lives in the directory named by the environment variable
# FSIMG_APFS_NATIVE, which must be set and must lie outside any git work tree.
# Each --volume is a comma-separated list of the VOLUME_KEYS below, for
# example "name=Data,case-sensitive". The first volume formats the container
# (newfs_apfs without -A); each further one is added to it with -A. Without
# --volume the container holds one case-insensitive volume named FSIMG. A
# volume of kind hdiutil takes only `name` and the case keys.
#
# --populate SCRIPT mounts the volume (one volume only), runs SCRIPT with the
# mount point as its only argument, and unmounts it before the check.
#
# Every disk the tool attaches is detached again, on failure and on an
# interrupt between commands too; a kill during `hdiutil attach` itself can
# leave the image attached, and `hdiutil info` shows it. Example:
#
#   FSIMG_APFS_NATIVE=/Volumes/Optaine/apfs-native capture.rb --kind newfs \
#     --size 67108864 --option case-sensitive --volume name=FSIMG,case-sensitive
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "shellwords"
require "tmpdir"
require "timeout"

module ApfsCapture
  class Error < StandardError; end

  ENV_ROOT = "FSIMG_APFS_NATIVE"
  KINDS = %w[newfs hdiutil].freeze
  OUTPUT_LINES = 40
  OUTPUT_LINE_BYTES = 4096
  COMMAND_TIMEOUT = 30 * 60
  FSCK_TIMEOUT = 30 * 60
  ATTACH_TIMEOUT = 5 * 60
  DETACH_TIMEOUT = 5 * 60
  # What --option may hold: it becomes part of the capture directory's name.
  OPTION_PATTERN = /\A[\w.+-]+\z/
  DEFAULT_VOLUME_NAME = "FSIMG"
  # A magic number of container superblocks, at byte 32 of block 0.
  CONTAINER_MAGIC = "NXSB"
  # The start of the partition type hdiutil lists for the synthesized disk of a
  # container (Apple_APFS_Container).
  CONTAINER_GUID = "EF57347C"
  SPACE = /\A\d+[kKmMgGtT]?\z/

  # The newfs_apfs flag each --volume key stands for and the pattern its value
  # must match (nil for a key without a value). Encryption is left out: it
  # needs a password on the command line or a prompt.
  VOLUME_KEYS = {
    "name" => ["-v", /\A\w[\w. -]{0,126}\z/],
    "case-sensitive" => ["-e", nil],
    "case-insensitive" => ["-i", nil],
    "container-only" => ["-C", nil],
    "option" => ["-o", /\Aconformance\z/],
    "block-size" => ["-b", /\A\d+\z/],
    "volume-size" => ["-s", SPACE],
    "quota" => ["-q", SPACE],
    "reserve" => ["-r", SPACE],
    "uid" => ["-U", /\A\d+\z/],
    "gid" => ["-G", /\A\d+\z/],
    "role" => ["-R", /\A[brv]\z/],
    "role-uuid" => ["-D", nil],
    "wait" => ["-w", nil]
  }.freeze

  # A volume to create: the --volume keys and their values in the order given.
  Volume = Data.define(:settings) do
    def name = settings.fetch("name", DEFAULT_VOLUME_NAME)
    def case_sensitive? = settings.key?("case-sensitive")
    def container_only? = settings.key?("container-only")

    # The newfs_apfs arguments before the device; `first` formats the container,
    # any other volume is added to it.
    def newfs_args(first:)
      named = container_only? ? settings : { "name" => name }.merge(settings)
      flags = named.flat_map { |key, value| [VOLUME_KEYS.fetch(key).first, value].compact }
      first ? flags : ["-A", *flags]
    end

    def hdiutil_fs = case_sensitive? ? "Case-sensitive APFS" : "APFS"
  end

  Config = Data.define(:root, :kind, :size, :option, :volumes, :populate, :overwrite) do
    def name = "apfs-#{kind}-#{size}-#{option}"
    def dir = File.join(root, name)
    def image = File.join(dir, "image.img")
  end

  # Records what was run and appends it to commands.txt.
  class Log
    attr_reader :runs

    def initialize
      @lines = []
      @runs = []
    end

    def to_s = @lines.map(&:b).join("\n".b) << "\n"

    def shown_command(argv) = argv.map { _1.match?(%r{\A[\w/.@:=+,-]+\z}) ? _1 : _1.shellescape }.join(" ")

    # Run argv, log it, return [combined output, success].
    def run(*argv, timeout: COMMAND_TIMEOUT)
      out, code = run_status(*argv, timeout: timeout)
      [out, code == 0]
    end

    # Run argv, log it, return [output, exit status]; the status is nil when the
    # program cannot start, dies of a signal or runs past `timeout`. The output
    # holds its standard error too. The program has its own process group: a
    # timeout or an interrupt kills the group and reaps it, so that nothing it
    # started keeps running. The [cmd, cmd] form of the program keeps a lone
    # element from being handed to a shell. With `stderr`, a file path, the
    # standard error goes there instead of into the output.
    def run_status(*argv, timeout: COMMAND_TIMEOUT, stderr: nil)
      out = +"".b
      error = stderr ? [stderr, "w"] : [:child, :out]
      code = Open3.popen2([argv.first, argv.first], *argv.drop(1), pgroup: true, err: error) do |input, output, child|
        reader = nil
        begin
          input.close
          reader = Thread.new do
            loop { out << output.readpartial(16_384) }
          rescue EOFError
            nil
          end
          Timeout.timeout(timeout) do
            reader.value
            child.value.exitstatus
          end
        rescue Timeout::Error
          stop_group(child, reader)
          out << "\n[timeout after #{timeout} seconds: #{shown_command(argv)}]\n"
          nil
        rescue SignalException
          stop_group(child, reader)
          note(argv, out, nil)
          raise
        ensure
          reader&.join
        end
      end
      note(argv, out, code)
      [out, code]
    rescue SystemCallError => e
      @lines.push("$ #{shown_command(argv)}", "  [#{e.message}]")
      @runs << { argv: argv, exit_status: nil }
      ["", nil]
    end

    private

    def note(argv, out, code)
      shown = out.lines.first(OUTPUT_LINES).map { _1.chomp.byteslice(0, OUTPUT_LINE_BYTES) }
      @lines.push("$ #{shown_command(argv)}", *shown.map { "  #{_1}" }, "  [exit #{code}]")
      @runs << { argv: argv, exit_status: code }
    end

    # Kill the group even if the leader exited: descendants can retain the output
    # pipe and keep the reader waiting for EOF. macOS answers EPERM for a group
    # whose only member is an unreaped leader, and ESRCH once it is reaped.
    def stop_group(child, reader)
      begin
        Process.kill("KILL", -child.pid)
      rescue Errno::ESRCH, Errno::EPERM
        nil
      end
      child.value
      reader&.value
    end
  end

  # What attaching an image gives: the image's own disk, the synthesized disk of
  # the APFS container in it once the container exists (nil before), and the
  # disk `fsck_apfs` checks: the Apple_APFS partition of a partitioned image,
  # else the image's own disk.
  Attached = Data.define(:device, :container, :store)

  # The images this process attached, detached again by `release`.
  class Attachments
    def initialize(log)
      @log = log
      @devices = []
    end

    def none? = @devices.empty?

    # Attach `image` and return its Attached; with `mountpoint` the volume is
    # also mounted there, otherwise it is left unmounted. `raw` attaches the file
    # as a raw disk image, which is right for everything but a UDIF file;
    # `readonly` and `options` (hdiutil flags) are passed on. A device named in the
    # output is recorded before the exit status is judged, since hdiutil can
    # attach a disk and still fail, for example to mount it. The first device
    # listed is the image's own disk; the synthesized disks of the container
    # follow and go away with it.
    def attach(image, mountpoint: nil, raw: true, readonly: false, options: [])
      args = [ApfsCapture.tool("hdiutil"), "attach"]
      args += %w[-imagekey diskimage-class=CRawDiskImage] if raw
      args += ["-readonly"] if readonly
      args += options
      args += mountpoint ? ["-mountpoint", mountpoint] : ["-nomount"]
      out, ok = @log.run(*args, image, timeout: ATTACH_TIMEOUT)
      device = out[%r{/dev/disk\d+}]
      container = out.lines.map(&:split).find { |_, type| type.to_s.start_with?(CONTAINER_GUID) }&.first
      @devices << device if device && !@devices.include?(device)
      unless ok && device
        detach_error(device) if device
        raise Error, "cannot attach #{image}: #{out.lines.first}"
      end

      store = out.lines.map(&:split).find { |_, type| type == "Apple_APFS" }&.first || device
      Attached.new(device: device, container: container, store: store)
    end

    # Detach `device`: three plain attempts and then a forced one.
    def detach(device)
      done = [[], [], [], ["-force"]].any? do |extra|
        _, ok = @log.run(ApfsCapture.tool("hdiutil"), "detach", device, *extra, timeout: DETACH_TIMEOUT)
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

    # Attach, yield the Attached, and detach it again: a failed detach on the
    # success path is an error. When the block is left any other way, an
    # interrupt included, the device is detached on a best-effort basis and what
    # left the block is what is raised; a device that cannot be detached stays
    # listed for `release` to report.
    def with(image, **how)
      attached = attach(image, **how)
      result = yield attached
      detach(attached.device)
      result
    ensure
      detach_error(attached.device) if attached && @devices.include?(attached.device)
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

  # The absolute path of the first executable `name` in PATH. Commands run by
  # this path, so the record names the program that ran.
  def tool(name)
    dirs = ENV.fetch("PATH", "").split(":").select { _1.start_with?("/") }
    found = dirs.map { File.join(_1, name) }.find { File.file?(_1) && File.executable?(_1) }
    raise Error, "#{name} not found in PATH" unless found

    found
  end

  # The directory named by FSIMG_APFS_NATIVE, which must be absolute and lie
  # outside every git work tree, this tool's own among them.
  def native_root(env)
    root = env[ENV_ROOT].to_s
    raise Error, "#{ENV_ROOT} is not set" if root.empty?
    raise Error, "#{ENV_ROOT} must be an absolute path" unless root.start_with?("/")

    # Lexical `..` is resolved first, as mkdir_p will, so the check sees the
    # directory that gets created.
    root = File.expand_path(root)
    raise Error, "#{ENV_ROOT}=#{root} lies inside a git work tree" if inside_work_tree?(root)

    root
  end

  # Whether `path` is, or would be created, inside a git work tree. The nearest
  # existing ancestor decides, symbolic links resolved.
  def inside_work_tree?(path)
    ancestor = path
    ancestor = File.dirname(ancestor) until File.directory?(ancestor)
    real = File.realpath(ancestor)
    own = File.realpath(File.expand_path("../..", __dir__))
    return true if real == own || real.start_with?("#{own}/")

    out, status = Open3.capture2e("git", "-C", real, "rev-parse", "--is-inside-work-tree")
    status.success? && out.strip == "true"
  rescue Errno::ENOENT
    false
  end

  # One Volume from "key[=value],key...", each key from VOLUME_KEYS.
  def parse_volume(text)
    items = text.split(",").map { _1.split("=", 2) }
    keys = items.map(&:first)
    raise Error, "empty --volume" if keys.empty?
    raise Error, "duplicate key in --volume #{text}" unless keys.uniq == keys
    raise Error, "case-sensitive and case-insensitive together" if (keys & %w[case-sensitive case-insensitive]).size == 2

    settings = items.to_h do |key, value|
      _, pattern = VOLUME_KEYS.fetch(key) { raise Error, "unknown --volume key #{key}" }
      raise Error, "#{key} takes no value" if pattern.nil? && value
      raise Error, "#{key} needs a value matching #{pattern.inspect}" if pattern && !value&.match?(pattern)

      [key, value]
    end
    Volume.new(settings: settings.freeze)
  end

  # What the producer and the populate hook can do with `volumes`.
  def check_volumes(kind, volumes, populate)
    if kind == "hdiutil"
      extra = volumes.flat_map { _1.settings.keys } - %w[name case-sensitive case-insensitive]
      raise Error, "hdiutil takes only name and the case keys, not #{extra.first}" unless extra.empty?
      raise Error, "hdiutil makes one volume" unless volumes.size == 1
    end
    return unless populate
    raise Error, "--populate needs exactly one volume" unless volumes.size == 1
    raise Error, "--populate needs a volume, not container-only" if volumes.first.container_only?
    raise Error, "#{populate} is not an executable file" unless File.file?(populate) && File.executable?(populate)
  end

  def hdiutil_image(config, log)
    Dir.mktmpdir("apfs-capture") do |tmp|
      volume = config.volumes.first
      sparse = File.join(tmp, "v")
      _, ok = log.run(tool("hdiutil"), "create", "-size", config.size.to_s, "-fs", volume.hdiutil_fs,
                      "-volname", volume.name, "-layout", "NONE", "-type", "SPARSE", sparse)
      raise Error, "hdiutil create failed" unless ok

      raw = File.join(tmp, "raw")
      _, ok = log.run(tool("hdiutil"), "convert", "#{sparse}.sparseimage", "-format", "UDTO", "-o", raw)
      raise Error, "hdiutil convert failed" unless ok

      FileUtils.mv("#{raw}.cdr", config.image)
    end
  end

  # The character device of a disk, which the formatter and checker take.
  def raw_device(device) = device.sub("/dev/", "/dev/r")

  # A sparse file of SIZE bytes formatted by newfs_apfs, one run per volume,
  # each on a fresh attachment. The first run formats the image's disk; a
  # volume added to the container by -A is named by the container's own disk.

  def newfs_image(config, attachments, log)
    _, ok = log.run(tool("mkfile"), "-n", config.size.to_s, config.image)
    raise Error, "mkfile failed" unless ok

    config.volumes.each_with_index do |volume, index|
      ok = attachments.with(config.image) do |attached|
        target = index.zero? ? attached.device : attached.container
        raise Error, "no APFS container in #{config.image} to add #{volume.name} to" unless target

        _, formatted = log.run(tool("newfs_apfs"), *volume.newfs_args(first: index.zero?), raw_device(target))
        formatted
      end
      raise Error, "newfs_apfs failed for volume #{volume.name}" unless ok
    end
  end

  # An image another tool left in a form other than a raw container, such as a
  # UDIF file, would be attached but is not what the capture claims to be.
  def check_container(image)
    magic = File.open(image, "rb") { _1.pread(4, 32) }
    raise Error, "#{image} is not a raw APFS container (magic #{magic.inspect})" unless magic == CONTAINER_MAGIC
  end

  # Mount the volume, yield the mount directory, unmount. The directory is
  # removed with rmdir, never recursively, and only once nothing is attached:
  # were a detach to fail, a recursive removal would delete the contents of a
  # still-mounted volume.
  def mounted(image, attachments)
    mount = Dir.mktmpdir("apfs-mount")
    result = attachments.with(image, mountpoint: mount) { yield mount }
    Dir.rmdir(mount)
    result
  ensure
    remove_empty(mount) if mount && attachments.none?
  end

  def remove_empty(dir)
    Dir.rmdir(dir)
  rescue SystemCallError
    nil
  end

  def populate(config, attachments, log)
    script = config.populate
    mounted(config.image, attachments) do |mount|
      _, ok = log.run(script, mount)
      raise Error, "populate script failed: #{script}" unless ok
    end
    { script: script, sha256: Digest::SHA256.file(script).hexdigest }
  end

  # The read-only check of the container. A failed check is evidence to record,
  # not a reason to stop.
  def fsck(attachments, log, image)
    attachments.with(image) do |attached|
      command = [tool("fsck_apfs"), "-n", raw_device(attached.device)]
      out, status = log.run_status(*command, timeout: FSCK_TIMEOUT)
      { command: command, exit_status: status, output: out.dup.force_encoding("UTF-8").scrub }
    end
  end

  def host_record(log)
    sw_vers, = log.run(tool("sw_vers"))
    uname, = log.run(tool("uname"), "-a")
    { sw_vers: sw_vers.strip, uname: uname.strip }
  end

  # The steps of one capture, in order: host, image, populate, check. The
  # detaches that follow, and so the image's checksum, come after.
  def build(config, attachments, log)
    host = host_record(log)
    config.kind == "newfs" ? newfs_image(config, attachments, log) : hdiutil_image(config, log)
    check_container(config.image)
    populated = config.populate && populate(config, attachments, log)
    { host: host, populate: populated, fsck: fsck(attachments, log, config.image) }
  end

  # A new, empty capture directory, created exclusively: every file of the
  # capture is then written inside a directory nothing else has put entries in,
  # so no existing file or symbolic link is replaced or followed. An existing
  # destination is refused; --overwrite removes it first, but only a real
  # directory (never a symbolic link, whose target would not be touched).
  def claim_directory(config)
    FileUtils.mkdir_p(config.root)
    Dir.mkdir(config.dir)
  rescue Errno::EEXIST
    raise Error, "#{config.dir} exists; --overwrite replaces it" unless config.overwrite
    raise Error, "#{config.dir} is not a directory" if File.symlink?(config.dir) || !File.directory?(config.dir)

    FileUtils.rm_rf(config.dir)
    Dir.mkdir(config.dir)
  end

  # Build the capture, then detach whatever is still attached and write
  # commands.txt, whether or not the build succeeded. A failure to detach is
  # reported, but never replaces the error that ended the build.
  def capture(config)
    log = Log.new
    attachments = Attachments.new(log)
    claim_directory(config)
    built = nil
    begin
      built = build(config, attachments, log)
    ensure
      leftover = release_error(attachments)
      warn "capture.rb: #{leftover.message}" if leftover && built.nil?
      File.write(File.join(config.dir, "commands.txt"), log.to_s)
    end
    raise leftover if leftover

    manifest(config, log, built)
  end

  def release_error(attachments)
    attachments.release
    nil
  rescue Error => e
    e
  end

  def tool_identity(path) = { path: path, sha256: Digest::SHA256.file(path).hexdigest }

  # Everything the capture record says, taken once the image is at rest.
  def manifest(config, log, built)
    tools = (log.runs.map { _1[:argv].first }.uniq - [config.populate]).select { _1.start_with?("/") && File.file?(_1) }
    { name: config.name, kind: config.kind, size: config.size, option: config.option,
      volumes: config.volumes.map(&:settings),
      commands: log.runs,
      tools: tools.map { tool_identity(_1) },
      image: { file: File.basename(config.image), bytes: File.size(config.image), sha256: Digest::SHA256.file(config.image).hexdigest } }
      .merge(built)
  end

  def finish(config, manifest)
    File.write(File.join(config.dir, "fsck.txt"), manifest.dig(:fsck, :output))
    File.write(File.join(config.dir, "manifest.json"), JSON.pretty_generate(manifest))
  end

  def parse(argv, env)
    opts = { volumes: [], overwrite: false }
    OptionParser.new do |o|
      o.banner = "usage: capture.rb --kind #{KINDS.join('|')} --size BYTES --option NAME [--volume SPEC]... [--populate SCRIPT] [--overwrite]"
      o.separator "  SPEC is comma-separated, each of: #{VOLUME_KEYS.keys.join(', ')} (key[=value])"
      o.on("--kind KIND", KINDS) { opts[:kind] = _1 }
      o.on("--size BYTES", Integer) { opts[:size] = _1 }
      o.on("--option NAME", OPTION_PATTERN) { opts[:option] = _1 }
      o.on("--volume SPEC") { opts[:volumes] << parse_volume(_1) }
      o.on("--populate SCRIPT", "run SCRIPT with the mount point as its argument") { opts[:populate] = File.expand_path(_1) }
      o.on("--overwrite", "replace an existing capture directory") { opts[:overwrite] = true }
    end.parse!(argv)
    missing = %i[kind size option].reject { opts[_1] }
    raise Error, "missing --#{missing.first}" unless missing.empty?
    raise Error, "--size must be positive" unless opts[:size].positive?

    volumes = opts[:volumes].empty? ? [Volume.new(settings: {}.freeze)] : opts[:volumes]
    check_volumes(opts[:kind], volumes, opts[:populate])
    Config.new(root: native_root(env), kind: opts[:kind], size: opts[:size], option: opts[:option],
               volumes: volumes, populate: opts[:populate], overwrite: opts[:overwrite])
  end

  def main(argv, env = ENV)
    config = parse(argv, env)
    manifest = capture(config)
    finish(config, manifest)
    puts config.dir
    0
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "capture.rb: #{e.message}"
    1
  end
end

exit(ApfsCapture.main(ARGV)) if $PROGRAM_NAME == __FILE__
