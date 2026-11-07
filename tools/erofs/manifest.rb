# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Manifests for the EROFS writer qualification (tools/erofs/qualify.sh).
# Each reader of an image -- the host tree, a kernel mount listing, fsimg's
# listing and rust-fs-erofs -- becomes the same record per path, and
# `compare` checks the fields an oracle can see against the source's.
#
#   manifest.rb tree DIR                     a host tree
#   manifest.rb kernel LISTING               the VM's find(1) output
#   manifest.rb fsimg LISTING                `fsimg t --json` output
#   manifest.rb rust TOOL IMAGE EXPORTED     rust-fs-erofs stat and export
#   manifest.rb compare SOURCE OTHER FIELDS [--no-root]
#                                            exit 1 naming each difference;
#                                            --no-root for a listing that
#                                            has no root record
#
# A record is one JSON line: path ("/" is the root), type (file,
# directory or symlink), mode (permission bits), uid, gid, size (files
# and links), mtime (nanoseconds since the epoch), sha256 (files) and
# target (links). An absent field is null.

require "digest"
require "json"
require "open3"

module ErofsManifest
  Record = Data.define(:path, :type, :mode, :uid, :gid, :size, :mtime, :sha256, :target) do
    def self.build(**fields) = new(**members.to_h { [_1, nil] }.merge(fields))
  end

  class Error < StandardError; end

  module_function

  def from_tree(root)
    paths = Dir.glob("**/*", File::FNM_DOTMATCH, base: root).reject { File.basename(_1) == "." }
    ["", *paths].map { host_record(root, _1) }
  end

  def host_record(root, relative)
    full = relative.empty? ? root : File.join(root, relative)
    stat = File.lstat(full)
    Record.build(path: "/#{relative}", type: host_type(stat), mode: stat.mode & 0o7777,
      uid: stat.uid, gid: stat.gid, size: sized(stat.ftype, stat.size),
      mtime: stat.mtime.to_i * 1_000_000_000 + stat.mtime.nsec,
      sha256: stat.file? ? Digest::SHA256.file(full).hexdigest : nil,
      target: stat.symlink? ? File.readlink(full) : nil)
  end

  def host_type(stat) =
    {"file" => "file", "directory" => "directory", "link" => "symlink"}.fetch(stat.ftype) do
      raise Error, "unexpected #{stat.ftype}"
    end

  def sized(type, size) = %w[file link symlink].include?(type) ? size : nil

  # The VM prints, per path, eight NUL-terminated find(1) fields (%P %y %m
  # %U %G %s %T@ %l) and then one line: the SHA-256 of a file, else empty.
  KERNEL_TYPES = {"f" => "file", "d" => "directory", "l" => "symlink"}.freeze

  KERNEL_RECORD = /\G((?:[^\0]*\0){8})([^\n]*)\n/n

  def from_kernel(listing)
    records = listing.b.scan(KERNEL_RECORD)
    # Each record is its two groups and the newline between them; equal
    # lengths prove the scan consumed the whole listing.
    raise Error, "truncated kernel listing" unless records.sum { _1.sum(&:bytesize) + 1 } == listing.bytesize

    records.map { |fields, sha| kernel_record(fields.split("\0", -1).first(8), sha) }
  end

  def kernel_record((path, type, mode, uid, gid, size, time, target), sha)
    kind = KERNEL_TYPES.fetch(type) { raise Error, "unexpected kind #{type}" }
    Record.build(path: "/#{path.force_encoding(Encoding::UTF_8)}", type: kind, mode: mode.to_i(8),
      uid: Integer(uid), gid: Integer(gid), size: sized(kind, Integer(size)),
      mtime: decimal_ns(time), sha256: kind == "file" ? sha : nil,
      target: kind == "symlink" ? target.force_encoding(Encoding::UTF_8) : nil)
  end

  # GNU find's %T@ is the signed tv_sec, a point and the unsigned tv_nsec:
  # -1.5 s prints as -2.5000000000.
  def decimal_ns(text)
    seconds, fraction = text.split(".")
    Integer(seconds, 10) * 1_000_000_000 + (fraction || "").ljust(9, "0")[0, 9].to_i
  end

  FSIMG_TYPES = {"file" => "file", "directory" => "directory", "symlink" => "symlink"}.freeze

  def from_fsimg(listing)
    listing.each_line.map { JSON.parse(_1) }.select { _1["type"] == "entry" }.map do |entry|
      kind = FSIMG_TYPES.fetch(entry.fetch("kind")) { raise Error, "unexpected kind #{entry["kind"]}" }
      modified = entry.fetch("modified")
      Record.build(path: entry.fetch("path"), type: kind, mode: entry.fetch("mode"),
        uid: entry.fetch("uid"), gid: entry.fetch("gid"),
        size: sized(kind, Integer(entry.fetch("size_bytes"))),
        mtime: Integer(modified.fetch("seconds")) * 1_000_000_000 + modified.fetch("nanoseconds"))
    end
  end

  # Metadata from `fs IMAGE stat PATH` for every exported path, contents
  # and link targets from the exported tree.
  def from_rust(tool, image, exported)
    from_tree(exported).map do |host|
      stat = rust_stat(tool, image, host.path)
      kind = RUST_TYPES.fetch(stat.fetch("type")) { raise Error, "unexpected kind #{stat["type"]}" }
      host.with(type: kind, mode: stat.fetch("mode").to_i(8), uid: stat.fetch("uid"),
        gid: stat.fetch("gid"), size: sized(kind, stat.fetch("size")),
        mtime: stat.fetch("mtime") * 1_000_000_000 + stat.fetch("mtime_nsec"),
        target: kind == "symlink" ? stat.fetch("target") : nil)
    end
  end

  RUST_TYPES = {"file" => "file", "dir" => "directory", "symlink" => "symlink"}.freeze

  def rust_stat(tool, image, path)
    output, status = Open3.capture2(tool, "fs", image, "stat", path)
    raise Error, "rust-fs-erofs stat #{path} failed" unless status.success?

    JSON.parse(output)
  end

  def read_manifest(file) = File.read(file).each_line.map { Record.build(**JSON.parse(_1, symbolize_names: true)) }

  def dump(records) = records.sort_by { _1.path.b }.map { "#{JSON.generate(_1.to_h)}\n" }.join

  # Differences between the source and an oracle on fields, by path; the
  # field list may exclude a field for one kind as FIELD:-KIND.
  def differences(source, other, fields, root: true)
    wanted = fields.split(",").map { _1.split(":-") }
    left = source.reject { !root && _1.path == "/" }.to_h { [_1.path, _1] }
    right = other.reject { !root && _1.path == "/" }.to_h { [_1.path, _1] }
    missing = (left.keys - right.keys).map { "missing #{_1.inspect}" }
    extra = (right.keys - left.keys).map { "unexpected #{_1.inspect}" }
    changed = (left.keys & right.keys).flat_map do |path|
      wanted.filter_map do |field, skipped|
        a = left[path].public_send(field)
        b = right[path].public_send(field)
        next if a == b || left[path].type == skipped

        "#{path.inspect} #{field}: source #{a.inspect}, image #{b.inspect}"
      end
    end
    missing + extra + changed
  end

  def main(argv)
    case argv
    in ["tree", dir] then print dump(from_tree(dir))
    in ["kernel", file] then print dump(from_kernel(File.binread(file)))
    in ["fsimg", file] then print dump(from_fsimg(File.read(file)))
    in ["rust", tool, image, exported] then print dump(from_rust(tool, image, exported))
    in ["compare", source, other, fields, *flags] if flags.difference(["--no-root"]).empty?
      found = differences(read_manifest(source), read_manifest(other), fields, root: flags.empty?)
      found.first(20).each { warn _1 }
      exit(found.empty? ? 0 : 1)
    else
      abort("usage: manifest.rb tree|kernel|fsimg|rust|compare ...")
    end
  end
end

ErofsManifest.main(ARGV) if $PROGRAM_NAME == __FILE__
