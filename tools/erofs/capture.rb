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
#     command.txt    the exact mkfs.erofs command line
#     mkfs.txt       what mkfs.erofs printed
#     versions.txt   mkfs.erofs and dump.erofs versions
#     sha256.txt     SHA-256 of the image
#     dump.txt       dump.erofs superblock, statistics and root listing
#     manifest.txt   the source tree, one tab-separated line per entry:
#                    path, kind, size, mode, uid, gid, mtime, and the SHA-256
#                    of a regular file or "-> target" of a symbolic link,
#                    sorted by path bytes
#
# The build time (-T, SOURCE_DATE_EPOCH or 0) and the UUID (-U) are fixed so
# that producers which are deterministic give the same bytes. The tools come
# from $EROFS_UTILS_BIN, default ~/.local/share/fsimg-oracles/erofs-utils/bin.
# NAME must not exist yet. Example:
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
  DEFAULT_BIN = File.join(Dir.home, ".local/share/fsimg-oracles/erofs-utils/bin")
  KINDS = {
    file: "file", directory: "dir", link: "symlink", characterSpecial: "chardev",
    blockSpecial: "blockdev", fifo: "fifo", socket: "socket"
  }.freeze

  Plan = Data.define(:name, :source, :mkfs_args, :epoch, :uuid, :root, :bin) do
    def dir = File.join(root, name)
    def image = File.join(dir, "image.erofs")
    def tool(base) = File.join(bin, base)
    def mkfs_command = [tool("mkfs.erofs"), "-T#{epoch}", "-U#{uuid}", *mkfs_args, image, source]
  end

  module_function

  # One manifest line per entry below `source`, sorted by path bytes.
  def manifest(source)
    Find.find(source).filter_map { manifest_line(source, _1) }.sort_by { _1.split("\t", 2).first.b }
  end

  def manifest_line(source, path)
    st = File.lstat(path)
    rel = path == source ? "/" : "/#{path.delete_prefix("#{source.chomp('/')}/")}"
    detail = if st.file? then Digest::SHA256.file(path).hexdigest
             elsif st.symlink? then "-> #{File.readlink(path)}"
             else "-"
             end
    [rel, KINDS.fetch(st.ftype.to_sym, st.ftype), st.size, format("%04o", st.mode & 0o7777),
     st.uid, st.gid, st.mtime.to_i, detail].join("\t")
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

  # Creates the fixture directory, which must not exist, and fills it. A
  # failed build removes the directory it created.
  def build(plan)
    raise Error, "source #{plan.source} is not a directory" unless File.directory?(plan.source)

    begin
      Dir.mkdir(plan.dir)
    rescue Errno::EEXIST
      raise Error, "#{plan.dir} exists; fixtures are never overwritten"
    end
    begin
      write_fixture(plan)
    rescue Error, SystemCallError
      FileUtils.rm_rf(plan.dir)
      raise
    end
  end

  def write_fixture(plan)
    command = plan.mkfs_command
    File.write(File.join(plan.dir, "command.txt"), "#{command.shelljoin}\n")
    File.write(File.join(plan.dir, "mkfs.txt"), checked(*command))
    File.write(File.join(plan.dir, "versions.txt"),
               [checked(plan.tool("mkfs.erofs"), "-V"), checked(plan.tool("dump.erofs"), "-V")].join)
    File.write(File.join(plan.dir, "sha256.txt"), "#{Digest::SHA256.file(plan.image).hexdigest}  image.erofs\n")
    File.write(File.join(plan.dir, "dump.txt"), dump_text(plan))
    File.write(File.join(plan.dir, "manifest.txt"), "#{manifest(plan.source).join("\n")}\n")
  end

  def dump_text(plan)
    dump = plan.tool("dump.erofs")
    [["-s"], ["-S"], ["--ls", "--path=/"]].map do |args|
      "$ dump.erofs #{args.join(' ')} image.erofs\n#{checked(dump, *args, plan.image)}"
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
                    bin: ENV.fetch("EROFS_UTILS_BIN", DEFAULT_BIN), **options)
    build(plan)
    puts plan.dir
    0
  rescue Error, OptionParser::ParseError, SystemCallError, ArgumentError => e
    warn "capture: #{e.message}"
    1
  end
end

exit ErofsCapture.main(ARGV) if $PROGRAM_NAME == __FILE__
