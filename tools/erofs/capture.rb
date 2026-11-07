#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Build one named EROFS fixture from a recipe source tree with mkfs.erofs and
# record how it was made:
#
#   capture.rb [--epoch N] [--uuid UUID] NAME SOURCE [-- MKFS-OPTION...]
#
#   $FSIMG_EROFS_FIXTURES/NAME/
#     image.erofs    the image
#     command.txt    the mkfs.erofs command line, with $SOURCE, $FIXTURE and
#                    $EROFS_UTILS_BIN standing for the host paths
#     mkfs.txt       what mkfs.erofs printed
#     versions.txt   mkfs.erofs and dump.erofs versions
#     sha256.txt     SHA-256 of the image
#     dump.txt       dump.erofs superblock, statistics and root listing
#     manifest.txt   the source tree, one tab-separated line per entry:
#                    path, kind, size (- for a directory), mode, uid, gid,
#                    mtime in nanoseconds, hard-link group, extended
#                    attributes (name=SHA-256 of the value), and the SHA-256
#                    of a regular file or "-> target" of a symbolic link,
#                    sorted by path bytes; paths and targets are escaped
#                    with String#dump
#
# The build time (-T, SOURCE_DATE_EPOCH or 0) and the UUID (-U) are fixed so
# that producers which are deterministic give the same bytes. The tools come
# from $EROFS_UTILS_BIN, default ~/.local/share/fsimg-oracles/erofs-utils/bin.
# The fixture is built in NAME.tmp-PID beside its final place and renamed
# into place last, so an interrupted run leaves nothing. NAME must not exist
# yet and the fixture root must lie outside SOURCE. The source manifest is
# taken before and after mkfs.erofs; a difference fails the run. Extended
# attributes are read with getfattr or xattr; without either the manifest
# says so. Example:
#
#   capture.rb lz4-compact tree -- -zlz4 -b4096
require "digest"
require "fileutils"
require "find"
require "open3"
require "optparse"
require "shellwords"

module ErofsCapture
  class Error < StandardError; end

  NAME_PATTERN = /\A[\w.+-]+\z/
  DEFAULT_UUID = "11111111-2222-3333-4444-555555555555"
  KINDS = {
    file: "file", directory: "dir", link: "symlink", characterSpecial: "chardev",
    blockSpecial: "blockdev", fifo: "fifo", socket: "socket"
  }.freeze

  def self.default_bin = File.join(Dir.home, ".local/share/fsimg-oracles/erofs-utils/bin")

  Plan = Data.define(:name, :source, :mkfs_args, :epoch, :uuid, :root, :bin) do
    def dir = File.join(root, name)
    def tmp_dir = "#{dir}.tmp-#{Process.pid}"
    def tool(base) = File.join(bin, base)
    def mkfs_flags = ["-T#{epoch}", "-U#{uuid}", *mkfs_args]

    # The command that runs, building in `work`, the directory being filled.
    def mkfs_command(work) = [tool("mkfs.erofs"), *mkfs_flags, File.join(work, "image.erofs"), source]

    # The command as recorded: host paths replaced by placeholders.
    def recorded_command
      ["$EROFS_UTILS_BIN/mkfs.erofs", *mkfs_flags.map(&:shellescape), "$FIXTURE/image.erofs", "$SOURCE"].join(" ")
    end
  end

  # Extended attributes of a path as [[name, value bytes]], read with the
  # first tool that exists; nil when none does.
  module Xattrs
    module_function

    def tool = @tool ||= %w[getfattr xattr].find { |t| ENV.fetch("PATH", "").split(":").any? { |d| File.executable?(File.join(d, t)) } }

    def list(path)
      case tool
      when "getfattr" then via_getfattr(path)
      when "xattr" then via_xattr(path)
      end
    end

    def via_getfattr(path)
      out = ErofsCapture.checked("getfattr", "-h", "-d", "-m", "-", "-e", "hex", "--absolute-names", path)
      out.lines.filter_map do |line|
        next unless (m = line.chomp.match(/\A([^#=][^=]*)(?:=0x(\h*))?\z/))

        [m[1], [m[2].to_s].pack("H*")]
      end
    end

    def via_xattr(path)
      ErofsCapture.checked("xattr", "-s", path).lines.map(&:chomp).reject(&:empty?).map do |name|
        hex = ErofsCapture.checked("xattr", "-s", "-p", "-x", name, path)
        [name, [hex.delete(" \n")].pack("H*")]
      end
    end
  end

  module_function

  # One manifest line per entry below `source`, sorted by path bytes. A path
  # that cannot be read fails the walk.
  def manifest(source)
    entries = Find.find(source, ignore_error: false).map { [_1, File.lstat(_1)] }
    entries.sort_by! { |path, _| relative(source, path).b }
    groups = entries.select { |_, st| st.nlink > 1 && !st.directory? }.map { |_, st| [st.dev, st.ino] }.uniq
                    .each_with_index.to_h
    entries.map { |path, st| manifest_line(relative(source, path), path, st, groups[[st.dev, st.ino]]) }
  end

  def relative(source, path) = path == source ? "/" : "/#{path.delete_prefix("#{source.chomp('/')}/")}"

  def manifest_line(rel, path, st, group)
    [rel.b.dump, KINDS.fetch(st.ftype.to_sym, st.ftype), st.directory? ? "-" : st.size,
     format("%04o", st.mode & 0o7777), st.uid, st.gid, format("%d%09d", st.mtime.to_i, st.mtime.nsec),
     group ? "link#{group}" : "-", xattr_text(path, st), content_text(path, st)].join("\t")
  end

  def content_text(path, st)
    if st.file? then Digest::SHA256.file(path).hexdigest
    elsif st.symlink? then "-> #{File.readlink(path).b.dump}"
    else "-"
    end
  end

  def xattr_text(path, st)
    return "unavailable" unless Xattrs.tool
    # A device node, fifo or socket may refuse attribute calls; none is expected there.
    return "-" unless st.file? || st.directory? || st.symlink?

    list = Xattrs.list(path)
    list.empty? ? "-" : list.sort.map { |name, value| "#{name.b.dump}=#{Digest::SHA256.hexdigest(value)}" }.join(",")
  end

  def run(*command)
    out, status = Open3.capture2e(*command)
    [out, status]
  rescue SystemCallError => e
    raise Error, "cannot run #{command.first}: #{e.message}"
  end

  def checked(*command)
    out, status = run(*command)
    raise Error, "#{command.shelljoin} failed (#{status.exitstatus}):\n#{out}" unless status.success?

    out
  end

  # Builds the fixture in a temporary directory beside its final place and
  # renames it there last. The temporary directory is removed however the
  # build ends, so a failure or interrupt leaves no partial fixture.
  def build(plan)
    raise Error, "source #{plan.source} is not a directory" unless File.directory?(plan.source)
    raise Error, "fixture root #{plan.root} lies inside the source tree" if inside?(plan.source, plan.root)
    raise Error, "#{plan.dir} exists; fixtures are never overwritten" if File.exist?(plan.dir) || File.symlink?(plan.dir)

    Dir.mkdir(plan.tmp_dir)
    begin
      write_fixture(plan)
      raise Error, "#{plan.dir} exists; fixtures are never overwritten" if File.exist?(plan.dir)

      File.rename(plan.tmp_dir, plan.dir)
    ensure
      FileUtils.rm_rf(plan.tmp_dir)
    end
  end

  def inside?(source, dir)
    root = File.realpath(source)
    here = File.realpath(dir)
    here == root || here.start_with?("#{root.chomp('/')}/")
  end

  def write_fixture(plan)
    work = plan.tmp_dir
    before = manifest(plan.source)
    mkfs = checked(*plan.mkfs_command(work))
    raise Error, "the source tree changed while mkfs.erofs ran" unless manifest(plan.source) == before

    put = ->(name, text) { File.write(File.join(work, name), text) }
    put.("command.txt", "#{plan.recorded_command}\n")
    put.("mkfs.txt", mkfs.gsub(work, "$FIXTURE").gsub(plan.source, "$SOURCE"))
    put.("versions.txt", [checked(plan.tool("mkfs.erofs"), "-V"), checked(plan.tool("dump.erofs"), "-V")].join)
    put.("sha256.txt", "#{Digest::SHA256.file(File.join(work, 'image.erofs')).hexdigest}  image.erofs\n")
    put.("dump.txt", dump_text(plan, File.join(work, "image.erofs")))
    put.("manifest.txt", "#{before.join("\n")}\n")
  end

  def dump_text(plan, image)
    dump = plan.tool("dump.erofs")
    [["-s"], ["-S"], ["--ls", "--path=/"]].map do |args|
      "$ dump.erofs #{args.join(' ')} image.erofs\n#{checked(dump, *args, image)}"
    end.join("\n")
  end

  def main(argv)
    options = { epoch: Integer(ENV.fetch("SOURCE_DATE_EPOCH", "0")), uuid: DEFAULT_UUID }
    parser = OptionParser.new("usage: capture.rb [--epoch N] [--uuid UUID] NAME SOURCE [-- MKFS-OPTION...]") do |o|
      o.on("--epoch N", Integer) { options[:epoch] = _1 }
      o.on("--uuid UUID") { options[:uuid] = _1 }
    end
    name, source, *mkfs_args = parser.parse(argv)
    raise Error, parser.banner unless name && source
    raise Error, "NAME must match #{NAME_PATTERN.inspect}" unless NAME_PATTERN.match?(name)

    root = ENV.fetch("FSIMG_EROFS_FIXTURES") { raise Error, "FSIMG_EROFS_FIXTURES is not set" }
    plan = Plan.new(name:, source: File.expand_path(source), mkfs_args:, root: File.expand_path(root),
                    bin: ENV.fetch("EROFS_UTILS_BIN") { default_bin }, **options)
    build(plan)
    puts plan.dir
    0
  rescue Error, OptionParser::ParseError, SystemCallError, ArgumentError => e
    warn "capture: #{e.message}"
    1
  end
end

exit ErofsCapture.main(ARGV) if $PROGRAM_NAME == __FILE__
