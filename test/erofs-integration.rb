# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

# Compare fsimg's reading of the mkfs.erofs fixtures with their source
# manifests. Usage: erofs-integration.rb FSIMG SGDISK CORPUS
#
# For each fixture the reader supports it runs the built fsimg (`i`,
# `t --json`, `x -O`, `x`) and checks every listed entry against
# manifest.txt: kind, size, mode, owner, time, SHA-256 of file content,
# link target and the extended-attribute mark. A fixture without a
# manifest has nothing to compare and is counted as unchecked. A fixture whose `i` fails
# with the "unsupported capability" status is counted and named as not yet
# supported; it is never a pass. A few representative fixtures are then
# wrapped in a GPT, gzip and xz, and must list exactly as the bare image.
#
# Two manifest layouts exist (tools/erofs/capture.rb): eight tab-separated
# columns (path kind size mode uid gid mtime-seconds content) and ten (path
# kind size mode uid gid mtime-nanoseconds link-group xattrs content).
# The images are untrusted: they are only passed as path arguments.

require "digest"
require "fileutils"
require "json"
require "open3"
require "tmpdir"

FSIMG, SGDISK, CORPUS = ARGV.fetch(0, nil), ARGV.fetch(1, nil), ARGV.fetch(2, nil)
abort "usage: erofs-integration.rb FSIMG SGDISK CORPUS" unless FSIMG && CORPUS

Row = Data.define(:path, :kind, :size, :mode, :uid, :gid, :seconds, :nanos, :xattrs, :content)
Seen = Data.define(:path, :kind, :size, :mode, :uid, :gid, :time_key, :time, :extra, :bits)
Result = Data.define(:name, :verdict, :notes, :tolerated)
Volume = Data.define(:mtime, :epoch)

LISTING_KINDS = {
  "file" => "file", "directory" => "dir", "symlink" => "symlink", "fifo" => "fifo",
  "socket" => "socket", "char_device" => "chardev", "block_device" => "blockdev"
}.freeze
EXTRA_DATA_BIT = 0x100
SIZED = %w[file symlink].freeze
MAX_NOTES = 4
WRAPPED_FIXTURES = 3
SECTOR = 512
FIRST_SECTOR = 2048

# The reader refuses the image with the "unsupported capability" status.
class Unsupported < StandardError; end

def fsimg(mode, *args) = Open3.capture3(FSIMG, mode, *args)

def split_time(text)
  digits = text.to_i.abs.to_s.rjust(10, "0")
  [text.to_i.negative? ? -digits[0...-9].to_i : digits[0...-9].to_i, digits[-9..].to_i]
end

def parse_row(cols)
  path, kind, size, mode, uid, gid, *rest = cols
  seconds, nanos, xattrs, content =
    case rest
    in [secs, content] then [secs.to_i, nil, nil, content]
    in [ns, _group, xattrs, content] then [*split_time(ns), xattrs, content]
    end
  path = path.start_with?('"') ? path.undump : path
  Row.new(path:, kind:, size: size == "-" ? nil : size.to_i, mode: mode.to_i(8),
          uid: uid.to_i, gid: gid.to_i, seconds:, nanos:, xattrs:, content:)
end

def read_manifest(dir) = File.readlines(File.join(dir, "manifest.txt"), chomp: true).map { parse_row(_1.split("\t", -1)) }

# The portable name profile delivers strict UTF-8 without control bytes.
def portable?(path) = path.b.force_encoding("UTF-8").then { _1.valid_encoding? && !_1.match?(/[[:cntrl:]]/) }

def stamp(object) = object && [object.fetch("seconds").to_i, object.fetch("nanoseconds")]

def parse_seen(json)
  time_key = %w[modified changed].find { json[_1] }
  Seen.new(path: json.fetch("path"), kind: LISTING_KINDS.fetch(json.fetch("kind"), json["kind"]),
           size: json.fetch("size_bytes").to_i, mode: json.fetch("mode") & 0o7777,
           uid: json.fetch("uid"), gid: json.fetch("gid"), time_key:, time: stamp(json[time_key]),
           extra: json["extra_data"], bits: json.fetch("metadata_valid"))
end

def parse_lines(text) = text.each_line.select { _1.start_with?("{") }.map { JSON.parse(_1) }

def failure_line(err)
  line = err.lines.reject { _1.include?("warning:") }.first.to_s.strip
  line.empty? ? err.strip : line
end

def run(label, *args)
  out, err, status = fsimg(*args)
  raise "#{label} failed: #{failure_line(err)}" unless status.success?

  out
end

# `i` is the support probe: a refusal with the unsupported-capability
# status means the reader does not handle this image yet.
def probe(image)
  out, err, status = fsimg("i", "--json", "-f", image)
  return parse_lines(out).find { _1["type"] == "info" } if status.success?

  error = parse_lines(out).find { _1["type"] == "error" }
  raise Unsupported, error["message"] if error && error["status"] == "unsupported capability"

  raise "i failed: #{failure_line(err)}"
end

def listing(image) = parse_lines(run("t", "t", "--json", "-f", image)).select { _1["type"] == "entry" }.map { parse_seen(_1) }

# The volume's compatible feature names and its epoch in seconds.
def volume_of(info)
  texts = info.fetch("texts", {})
  Volume.new(mtime: texts.fetch("compat_features", "").split.include?("MTIME"), epoch: texts.fetch("epoch", "0").to_i)
end

def extract(image, dir)
  FileUtils.mkdir_p(dir, mode: 0o700)
  run("x", "x", "-E", "-k", "-f", image, "-C", dir)
end

def file_sha(dir, path)
  full = File.join(dir, path)
  File.file?(full) && !File.symlink?(full) ? Digest::SHA256.file(full).hexdigest : nil
end

def link_target(image, path) = fsimg("x", "-O", "-f", image, path).then { |out, _, status| status.success? ? out : nil }

# Compare one listed time with the manifest. nil is a match, a string a difference.
#
# A compact inode holds no source time: its time is the volume's epoch, so an
# epoch time is tolerated. A volume without MTIME stores the source's change
# time in an extended inode, which the manifest does not carry, so any time
# there is tolerated. Both are counted, not compared.
def time_difference(row, seen, volume:)
  want_key = volume.mtime ? "modified" : "changed"
  return "time delivered as #{seen.time_key.inspect}, expected #{want_key}" unless seen.time_key == want_key

  sec, nsec = seen.time
  return nil if sec == row.seconds && (row.nanos.nil? || nsec == row.nanos || nsec.zero?)
  return :tolerated if !volume.mtime || (sec == volume.epoch && nsec.zero?)

  "time #{sec}.#{nsec} != #{row.seconds}.#{row.nanos}"
end

# mkfs.erofs can rewrite owners; the command line says how, and the manifest
# holds the source's owners.
def owner_rule(dir)
  flags = File.read(File.join(dir, "command.txt")).scan(/--(force-uid|force-gid|uid-offset|gid-offset)\\?=(\d+)/).to_h
  ->(kind, id) { flags.key?("force-#{kind}") ? flags["force-#{kind}"].to_i : id + flags.fetch("#{kind}-offset", "0").to_i }
end

def compare_entry(row, seen, image:, dir:, volume:, owner:)
  checks = {
    kind: [row.kind, seen.kind], mode: [row.mode, seen.mode],
    uid: [owner.("uid", row.uid), seen.uid], gid: [owner.("gid", row.gid), seen.gid]
  }
  checks[:size] = [row.size, seen.size] if SIZED.include?(row.kind)
  diffs = checks.reject { |_, (want, got)| want == got }.map { |k, (want, got)| "#{row.path}: #{k} #{got.inspect} != #{want.inspect}" }
  diffs + content_difference(row, image:, dir:) + extra_difference(row, seen) + time_notes(row, seen, volume:)
end

def content_difference(row, image:, dir:)
  case row.kind
  when "file"
    got = file_sha(dir, row.path)
    got == row.content ? [] : ["#{row.path}: sha256 #{got.inspect} != #{row.content}"]
  when "symlink"
    want = row.content.delete_prefix("-> ").then { _1.start_with?('"') ? _1.undump : _1 }
    got = link_target(image, row.path)
    got == want ? [] : ["#{row.path}: link target #{got.inspect} != #{want.inspect}"]
  else []
  end
end

def extra_difference(row, seen)
  return [] if row.xattrs.nil? || row.xattrs == "unavailable"

  want = row.xattrs != "-"
  got = !seen.extra.nil? && seen.extra != false || (seen.bits & EXTRA_DATA_BIT) != 0
  want == got ? [] : ["#{row.path}: extra-data mark #{got} != #{want} (xattrs #{row.xattrs[0, 40]})"]
end

def time_notes(row, seen, volume:)
  case time_difference(row, seen, volume:)
  in nil then []
  in :tolerated then [:tolerated]
  in String => text then ["#{row.path}: #{text}"]
  end
end

def compare(name, dir, work)
  image = File.join(dir, "image.erofs")
  volume = volume_of(probe(image))
  rows = read_manifest(dir).reject { _1.path == "/" }.select { portable?(_1.path) }
  owner = owner_rule(dir)
  seen = listing(image).to_h { [_1.path, _1] }
  out = File.join(work, name)
  extract(image, out)
  diffs = rows.flat_map do |row|
    got = seen.delete(row.path)
    got ? compare_entry(row, got, image:, dir: out, volume:, owner:) : ["#{row.path}: not listed"]
  end
  diffs += seen.keys.map { "#{_1}: listed but not in the manifest" }
  real = diffs - [:tolerated]
  Result.new(name:, verdict: real.empty? ? :pass : :fail, notes: real.first(MAX_NOTES), tolerated: diffs.count(:tolerated))
end

def check(dir, work)
  name = File.basename(dir)
  return Result.new(name:, verdict: :unchecked, notes: ["no manifest to compare"], tolerated: 0) unless File.exist?(File.join(dir, "manifest.txt"))

  compare(name, dir, work)
rescue Unsupported => e
  Result.new(name:, verdict: :unsupported, notes: [e.message], tolerated: 0)
rescue StandardError => e
  Result.new(name:, verdict: :fail, notes: ["#{e.class}: #{e.message}"], tolerated: 0)
end

def report(result)
  note = result.notes.join("; ")
  tail = result.tolerated.positive? ? " [#{result.tolerated} times are the volume epoch or an uncarried ctime, not compared]" : ""
  puts "#{result.verdict.to_s.upcase.ljust(11)} #{result.name}#{note.empty? ? "" : " -- #{note}"}#{tail}"
end

# A GPT disk holding the image as its only partition, written by sgdisk.
def wrap_gpt(image, path)
  sectors = (File.size(image) + SECTOR - 1) / SECTOR
  File.open(path, "wb") { _1.truncate((FIRST_SECTOR + sectors + FIRST_SECTOR) * SECTOR) }
  _, err, status = Open3.capture3(SGDISK, "-n", "1:#{FIRST_SECTOR}:#{FIRST_SECTOR + sectors - 1}", "-t", "1:8300", path)
  raise "sgdisk failed: #{err.strip}" unless status.success?

  File.open(path, "r+b") do |disk|
    disk.seek(FIRST_SECTOR * SECTOR)
    IO.copy_stream(image, disk)
  end
end

def compress(program, image, path)
  out, err, status = Open3.capture3(program, "-c", image, binmode: true)
  raise "#{program} failed: #{err.strip}" unless status.success?

  File.binwrite(path, out)
end

def wrapped_difference(label, image, bare)
  got = listing(image)
  got == bare ? nil : "#{label}: listing differs from the bare image (#{got.size} vs #{bare.size} entries)"
rescue StandardError => e
  "#{label}: #{e.message}"
end

def wrap_check(name, dir, work)
  image = File.join(dir, "image.erofs")
  bare = listing(image)
  base = File.join(work, "wrap-#{name}")
  wrappers = [["gzip", ->(path) { compress("gzip", image, path) }], ["xz", ->(path) { compress("xz", image, path) }]]
  wrappers.unshift(["gpt", ->(path) { wrap_gpt(image, path) }]) if SGDISK && File.executable?(SGDISK)
  wrappers.filter_map { |label, build| build.call("#{base}.#{label}") && wrapped_difference("#{name}/#{label}", "#{base}.#{label}", bare) }
end

Dir.mktmpdir("fsimg-erofs-") do |work|
  dirs = Dir.children(CORPUS).sort.map { File.join(CORPUS, _1) }.select { File.directory?(_1) }
  results = dirs.map { |dir| check(dir, work).tap { report(_1) } }
  passed = results.select { _1.verdict == :pass }.first(WRAPPED_FIXTURES)
  wrap_failures = passed.flat_map do |result|
    wrap_check(result.name, File.join(CORPUS, result.name), work)
  rescue StandardError => e
    ["#{result.name}: #{e.message}"]
  end
  wrap_failures.each { puts "FAIL        #{_1}" }
  puts "wrapped: #{passed.size} fixtures through #{SGDISK ? 'gpt, ' : ''}gzip, xz; #{wrap_failures.size} failures"
  counts = results.map(&:verdict).tally
  puts "unsupported (not a pass): #{results.select { _1.verdict == :unsupported }.map(&:name).join(' ')}"
  puts "summary: #{counts.fetch(:pass, 0)} pass, #{counts.fetch(:fail, 0)} fail, " \
       "#{counts.fetch(:unsupported, 0)} unsupported, #{counts.fetch(:unchecked, 0)} unchecked (no manifest), " \
       "#{results.sum(&:tolerated)} times tolerated"
  exit(counts.fetch(:fail, 0).zero? && wrap_failures.empty? ? 0 : 1)
end
