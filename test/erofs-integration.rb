# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

# Compare fsimg's reading of the mkfs.erofs fixtures with their source
# manifests. Usage: erofs-integration.rb FSIMG SGDISK CORPUS
#
# For each fixture the reader supports it runs the built fsimg (`i`,
# `t --json`, `x -O`, `x`, `W` and `W -d`) and checks every listed entry against
# manifest.txt: kind, size, mode, owner, device numbers, time, SHA-256 of file
# content, link target and the extended-attribute mark. Times are derived
# from the fixture's command.txt, never from the reader: under `-T N` without
# --mkfs-time every time is N, and otherwise an extended
# inode keeps the source time and a compact inode holds the epoch N. On a
# volume without the MTIME bit an extended inode holds the change time, which
# is not compared.
# Both verifications must succeed on every fixture that reads. A fixture
# without a manifest must open, list and extract, and its
# expected.txt digests must match where it has one; the few that end in a
# refusal are pinned in EXPECTED_REFUSALS, or in EXPECTED_CORRUPT when a
# rule rejects them, and `W` must refuse each of them with the same reason.
# A fixture whose `i` fails with the
# "unsupported capability" status is counted and named as not yet supported,
# never a pass, and only for a reason listed in KNOWN_REFUSALS: any other
# refusal, or fewer than PASS_FLOOR passes, fails the run. A few
# representative fixtures are then wrapped in a GPT, gzip and xz, and must
# list exactly as the bare image.
#
# Two manifest layouts exist (tools/erofs/capture.rb): eight tab-separated
# columns (path kind size mode uid gid mtime-seconds content), which carry no
# nanoseconds and no attribute mark, and ten (path kind size mode uid gid
# mtime-nanoseconds link-group xattrs content). What a layout cannot compare
# is counted in the summary, not silently passed.
# The images are untrusted: they are only passed as path arguments.

require "digest"
require "fileutils"
require "json"
require "open3"
require "tmpdir"

FSIMG, SGDISK, CORPUS = ARGV.fetch(0, nil), ARGV.fetch(1, nil), ARGV.fetch(2, nil)
abort "usage: erofs-integration.rb FSIMG SGDISK CORPUS" unless FSIMG && CORPUS

Row = Data.define(:path, :kind, :size, :mode, :uid, :gid, :seconds, :nanos, :xattrs, :content)
Seen = Data.define(:path, :kind, :size, :mode, :uid, :gid, :time_key, :time, :precision, :dev, :extra, :bits)
# What one comparison found: differences, and checks the manifest could not make.
Comparison = Data.define(:diffs, :skipped) do
  def self.of(*diffs, skipped: []) = new(diffs:, skipped:)

  def +(other) = Comparison.new(diffs: diffs + other.diffs, skipped: skipped + other.skipped)
end
NONE = Comparison.of
Result = Data.define(:name, :verdict, :notes, :skipped)
# The epoch a fixture was built with, and whether its extended inodes keep source times.
TimeRule = Data.define(:epoch, :source)

LISTING_KINDS = {
  "file" => "file", "directory" => "dir", "symlink" => "symlink", "fifo" => "fifo",
  "socket" => "socket", "char_device" => "chardev", "block_device" => "blockdev"
}.freeze
EXTRA_DATA_BIT = 0x100
COMPACT_PRECISION_NS = 1_000_000_000
SIZED = %w[file symlink].freeze
DEVICES = %w[chardev blockdev].freeze
MAX_NOTES = 4
# The verifications run on every readable fixture: structure, then file data.
VERIFY_MODES = [[], ["-d"]].freeze
# Passes today; a fixture slipping to "unsupported" must not keep the run green.
PASS_FLOOR = 58
# Fixtures covering geometry, inline data, and an attribute-bearing tree.
WRAPPED_FIXTURES = %w[e1-geom-b512 e3-b4096-inline e6-xattr-default].freeze
SECTOR = 512
FIRST_SECTOR = 2048

# The only reasons the reader may give for an image it cannot read yet.
KNOWN_REFUSALS = [
  /feature (48BIT|LZ4_0PADDING)\z/, /extra devices\z/, /compressed data layout\z/, /metadata box/
].freeze

# Fixtures without a manifest that end in a refusal, and its class. Evidence:
# ~/.cache/fsimg-erofs/evidence/E8.md (LZ4 images, the cleared-flag edits),
# E9.md (two-header edits), E14.md (extra devices). A fixture listed neither
# here nor in EXPECTED_CORRUPT must open, list and extract.
EXPECTED_REFUSALS = {
  "e14-blobdev-b4096" => /extra devices/, "e14-tar-index-b4096" => /extra devices/,
  "e2-vm-48bit" => /48BIT/,
  "e8-lz4-block-end" => /LZ4_0PADDING/, "e8-lz4-distance0" => /LZ4_0PADDING/,
  "e9-head2-head1-wrong" => /LZ4_0PADDING/, "e9-head2-lz4-ctrl" => /LZ4_0PADDING/,
  "e9-head2-lzma" => /LZ4_0PADDING/, "e9-head2-wrongalg" => /LZ4_0PADDING/,
  "e8-lz4-flag-cleared" => /compressed data layout/, "e8-lz4-flag-cleared-distance0" => /compressed data layout/,
  "e8-lz4-unpadded" => /compressed data layout/,
  # Hand-built E15 images (~/.cache/fsimg-erofs/evidence/E15.md): large
  # logical clusters, compact advice 0x01 clear and interlaced inline tails,
  # all refused by policy once compression is read.
  "e15-a-compact-b2048-l4096-adv1" => /LZ4_0PADDING/,
  "e15-a-compact-b2048-l4096-adv1-lcount" => /LZ4_0PADDING/,
  "e15-a-compact-b4096-l16384-adv0" => /LZ4_0PADDING/,
  "e15-a-compact-b4096-l8192-adv0" => /LZ4_0PADDING/,
  "e15-a-compact-b4096-l8192-adv1flip" => /LZ4_0PADDING/,
  "e15-a-full-b4096-l16384" => /LZ4_0PADDING/,
  "e15-a-full-b4096-l8192" => /LZ4_0PADDING/,
  "e15-b-compact-b2048-adv0" => /LZ4_0PADDING/,
  "e15-b-compact-b4096-adv0" => /LZ4_0PADDING/,
  "e15-b-compact-b4096-adv1" => /LZ4_0PADDING/,
  "e15-c-il-nowrap" => /LZ4_0PADDING/,
  "e15-c-il-plain-wrap" => /LZ4_0PADDING/,
  "e15-c-il-rot-wrap" => /LZ4_0PADDING/,
  "e15-c-il-short" => /LZ4_0PADDING/,
  "e15-c-l2b-tail-at-B" => /LZ4_0PADDING/,
  "e15-c-l2b-tail-at-L" => /LZ4_0PADDING/,
  "e15-c-shift-nowrap" => /LZ4_0PADDING/,
  "e15-c-shift-short" => /LZ4_0PADDING/,
  "e15-c-shift-wrap" => /LZ4_0PADDING/
}.freeze

# Fixtures without a manifest that the reader rejects as corrupt, and the
# rule it names (specs/design-erofs.md#chunk-based). Evidence:
# ~/.cache/fsimg-erofs/evidence/E7.md, edited chunk indexes and formats.
EXPECTED_CORRUPT = {
  "e7-edit-block-oor" => /data blocks outside the volume/, "e7-edit-device-id" => /chunk device identifier/,
  "e7-edit-format-0x40" => /chunk format 48-bit bit/, "e7-edit-format-0x80" => /chunk format\z/,
  "e7-edit-high-part" => /chunk block high bits/
}.freeze

# The reader refuses the image with the "unsupported capability" status.
class Unsupported < StandardError; end

def fsimg(mode, *args) = Open3.capture3(FSIMG, mode, *args)

# Nanoseconds since the epoch as floor seconds and non-negative nanoseconds.
def split_time(text) = text.to_i.divmod(1_000_000_000)

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

def read_manifest(dir) =
  File.read(File.join(dir, "manifest.txt")).each_line(chomp: true).map { parse_row(_1.split("\t", -1)) }

# The portable name profile delivers strict UTF-8 without control bytes.
def portable?(path) = path.b.force_encoding("UTF-8").then { _1.valid_encoding? && !_1.match?(/[[:cntrl:]]/) }

def stamp(object) = object && [object.fetch("seconds").to_i, object.fetch("nanoseconds")]

def parse_seen(json)
  time_key = %w[modified changed].find { json[_1] }
  Seen.new(path: json.fetch("path"), kind: LISTING_KINDS.fetch(json.fetch("kind"), json["kind"]),
           size: json.fetch("size_bytes").to_i, mode: json.fetch("mode") & 0o7777,
           uid: json.fetch("uid"), gid: json.fetch("gid"), time_key:, time: stamp(json[time_key]),
           precision: json.dig(time_key, "precision_ns").to_i, dev: [json["dev_major"], json["dev_minor"]],
           extra: json["extra_data"], bits: json.fetch("metadata_valid"))
end

def parse_lines(text) = text.each_line.select { _1.start_with?("{") }.map { JSON.parse(_1) }

# The refusal text: the --json error record when there is one, else stderr.
def failure_line(out, err)
  json = parse_lines(out).find { _1["type"] == "error" }&.fetch("message", nil)
  line = json || err.lines.reject { _1.include?("warning:") }.first.to_s.strip
  line.empty? ? err.strip : line
end

def run(*args)
  out, err, status = fsimg(*args)
  raise "#{args.first} failed: #{failure_line(out, err)}" unless status.success?

  out
end

# `i` is the support probe: a refusal with the unsupported-capability
# status means the reader does not handle this image yet.
def probe(image)
  out, err, status = fsimg("i", "--json", "-f", image)
  return parse_lines(out).find { _1["type"] == "info" } if status.success?

  error = parse_lines(out).find { _1["type"] == "error" }
  raise Unsupported, error["message"] if error && error["status"] == "unsupported capability"

  raise "i failed: #{failure_line(out, err)}"
end

# One verification of the image: the failure text, or nil when it succeeds.
def verify_once(image, flags)
  out, err, status = fsimg("W", *flags, "-f", image)
  status.success? ? nil : "#{['W', *flags].join(' ')}: #{failure_line(out, err)}"
end

# The text of the first verification mode that fails, or nil when every mode passes.
def verify_failure(image) = VERIFY_MODES.lazy.filter_map { verify_once(image, _1) }.first

# A pinned refusal must be a verification refusal too, for the same reason.
def verify_refusal_difference(image, want)
  text = verify_failure(image)
  return "verification accepted an image pinned as refused" unless text

  text.match?(want) ? nil : "verification refused for another reason: #{text}"
end

def listing(image) =
  parse_lines(run("t", "--json", "-f", image)).select { _1["type"] == "entry" }.map { parse_seen(_1) }

# Whether the volume stores modification times (the MTIME compatible feature).
def mtime_volume?(info) = info.dig("texts", "compat_features").to_s.split.include?("MTIME")

def extract(image, dir, limit: [])
  FileUtils.mkdir_p(dir, mode: 0o700)
  run("x", "-E", "-k", *limit, "-f", image, "-C", dir)
end

# The output limit for extracting a tree, from its manifest rows or its
# listing: the files' logical bytes, holes included, which a sparse file
# can raise past the default of 16 times the volume's length, plus room for
# link targets.
def output_limit(rows) = ["-l", "output=#{rows.select { _1.kind == 'file' }.sum(&:size) + (1 << 20)}"]

def file_sha(dir, path)
  full = File.join(dir, path)
  File.file?(full) && !File.symlink?(full) ? Digest::SHA256.file(full).hexdigest : nil
end

def link_target(image, path) = fsimg("x", "-O", "-f", image, path).then { |out, _, status| status.success? ? out : nil }

# The epoch is the last -T on the command line. Source times survive only
# under --mkfs-time, and not with --ignore-mtime or with every inode forced
# compact; --preserve-mtime flattens them to the epoch like a bare -T
# (dump.erofs 1.9.4 shows 32-byte inodes at the epoch for e2-preserve).
def time_rule(command)
  epoch = command.scan(/(?:\A|\s)-T(\d+)(?=\s)/).flatten.last
  raise "command.txt has no -T epoch" unless epoch

  keeps = command.match?(/--mkfs-time(?=\s)/)
  flattened = command.match?(/--ignore-mtime(?=\s)|force-inode-compact/)
  TimeRule.new(epoch: epoch.to_i, source: keeps && !flattened)
end

# A compact inode holds no time of its own: it delivers the epoch (mkfs
# chooses it for a source time equal to the epoch in whole seconds). An
# extended inode delivers the source time, nanoseconds included when the
# manifest carries them.
def time_check(row, seen, rule:, mtime:)
  want_key = mtime ? "modified" : "changed"
  unless seen.time_key == want_key
    return Comparison.of("#{row.path}: time delivered as #{seen.time_key.inspect}, expected #{want_key}")
  end

  compact = seen.precision == COMPACT_PRECISION_NS
  # Without the MTIME bit an extended inode stores the source's change time,
  # which no manifest column carries.
  return Comparison.of(skipped: [:change_time]) if !mtime && !compact

  keeps = rule.source && !compact
  want = [keeps ? row.seconds : rule.epoch, keeps ? row.nanos : 0]
  sec, nsec = seen.time
  diffs = []
  if compact && rule.source && (row.seconds != rule.epoch || row.nanos.to_i.positive?)
    diffs << "#{row.path}: compact inode for a source time of #{row.seconds}, epoch #{rule.epoch}"
  end
  unless sec == want[0] && (want[1].nil? || nsec == want[1])
    diffs << "#{row.path}: time #{sec}.#{nsec} != #{want[0]}.#{want[1] || '?'}"
  end
  Comparison.of(*diffs, skipped: want[1].nil? ? [:nanoseconds] : [])
end

# mkfs.erofs can rewrite owners; the command line says how, and the manifest
# holds the source's owners.
def owner_rule(command)
  flags = command.scan(/--(force-uid|force-gid|uid-offset|gid-offset)\\?=(\d+)/).to_h
  lambda do |kind, id|
    forced = flags["force-#{kind}"]
    forced ? forced.to_i : id + flags.fetch("#{kind}-offset", "0").to_i
  end
end

Expectations = Data.define(:time, :owner, :mtime)

def compare_entry(row, seen, image:, dir:, expect:)
  owner = expect.owner
  checks = {
    kind: [row.kind, seen.kind], mode: [row.mode, seen.mode],
    uid: [owner.("uid", row.uid), seen.uid], gid: [owner.("gid", row.gid), seen.gid]
  }
  checks[:size] = [row.size, seen.size] if SIZED.include?(row.kind)
  diffs = checks.reject { |_, (want, got)| want == got }
                .map { |k, (want, got)| "#{row.path}: #{k} #{got.inspect} != #{want.inspect}" }
  Comparison.of(*diffs, *content_difference(row, image:, dir:)) + device_check(row, seen) + extra_check(row, seen) +
    time_check(row, seen, rule: expect.time, mtime: expect.mtime)
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

# The e5 fixtures name each node for its numbers: b_MAJOR_MINOR, c_MAJOR_MINOR.
def device_check(row, seen)
  return NONE unless DEVICES.include?(row.kind)

  want = File.basename(row.path)[/\A[bc]_(\d+)_(\d+)\z/]&.then { Regexp.last_match.captures.map(&:to_i) }
  return Comparison.of(skipped: [:device_numbers]) unless want

  seen.dev == want ? NONE : Comparison.of("#{row.path}: device #{seen.dev.inspect} != #{want.inspect}")
end

def extra_check(row, seen)
  return Comparison.of(skipped: [:attributes]) if row.xattrs.nil? || row.xattrs == "unavailable"

  want = row.xattrs != "-"
  got = (!seen.extra.nil? && seen.extra != false) || (seen.bits & EXTRA_DATA_BIT) != 0
  want == got ? NONE : Comparison.of("#{row.path}: extra-data mark #{got} != #{want} (xattrs #{row.xattrs[0, 40]})")
end

def compare(name, dir, work)
  image = File.join(dir, "image.erofs")
  command = File.read(File.join(dir, "command.txt"))
  expect = Expectations.new(time: time_rule(command), owner: owner_rule(command), mtime: mtime_volume?(probe(image)))
  rows = read_manifest(dir).reject { _1.path == "/" }.select { portable?(_1.path) }
  seen = listing(image).to_h { [_1.path, _1] }
  out = File.join(work, name)
  extract(image, out, limit: output_limit(rows))
  found = rows.map do |row|
    got = seen.delete(row.path)
    got ? compare_entry(row, got, image:, dir: out, expect:) : Comparison.of("#{row.path}: not listed")
  end.sum(NONE)
  found += Comparison.of(*seen.keys.map { "#{_1}: listed but not in the manifest" })
  found += Comparison.of(*Array(verify_failure(image)))
  verdict = found.diffs.empty? ? :pass : :fail
  Result.new(name:, verdict:, notes: found.diffs.first(MAX_NOTES), skipped: found.skipped)
end

# The refusal text of the first of i, t and x that fails, or nil when the
# whole image reads.
def refusal_of(image, out)
  probe(image)
  extract(image, out, limit: output_limit(listing(image)))
  nil
rescue StandardError => e
  e.message
end

# sha256sum lines ("SUM  ./path") of the files an image must extract.
def digest_differences(dir, out)
  file = File.join(dir, "expected.txt")
  return [] unless File.exist?(file)

  File.read(file).each_line(chomp: true).filter_map do |line|
    sum, path = line.split("  ", 2)
    "#{path}: sha256 #{file_sha(out, path).inspect} != #{sum}" unless file_sha(out, path) == sum
  end
end

# A fixture without a manifest: it opens, lists and extracts, or ends in the
# refusal pinned for it.
def compare_unlisted(name, dir, work)
  out = File.join(work, name)
  image = File.join(dir, "image.erofs")
  refusal = refusal_of(image, out)
  pinned = EXPECTED_REFUSALS[name] || EXPECTED_CORRUPT[name]
  case [refusal, pinned]
  in [nil, _]
    diffs = digest_differences(dir, out) + Array(verify_failure(image))
    Result.new(name:, verdict: diffs.empty? ? :unchecked : :fail, notes: diffs.first(MAX_NOTES), skipped: [])
  in [String => text, Regexp => want] if text.match?(want)
    diffs = Array(verify_refusal_difference(image, want))
    refused = EXPECTED_CORRUPT.key?(name) ? :rejected : :unsupported
    Result.new(name:, verdict: diffs.empty? ? refused : :fail, notes: [text, *diffs], skipped: [])
  in [String => text, _]
    Result.new(name:, verdict: :fail, notes: ["unexpected refusal: #{text}"], skipped: [])
  end
end

def check(dir, work)
  name = File.basename(dir)
  return compare_unlisted(name, dir, work) unless File.exist?(File.join(dir, "manifest.txt"))

  compare(name, dir, work)
rescue Unsupported => e
  known = KNOWN_REFUSALS.any? { e.message.match?(_1) }
  note = known ? e.message : "unexpected refusal: #{e.message}"
  Result.new(name:, verdict: known ? :unsupported : :fail, notes: [note], skipped: [])
rescue StandardError => e
  Result.new(name:, verdict: :fail, notes: ["#{e.class}: #{e.message}"], skipped: [])
end

def report(result)
  note = result.notes.join("; ")
  tail = result.skipped.empty? ? "" : " [not compared: #{result.skipped.tally.map { |k, n| "#{n} #{k}" }.join(', ')}]"
  puts "#{result.verdict.to_s.upcase.ljust(11)} #{result.name}#{note.empty? ? '' : " -- #{note}"}#{tail}"
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

def gpt_wrapper? = !SGDISK.to_s.empty? && File.executable?(SGDISK)

def wrappers_for(image)
  list = [["gzip", ->(path) { compress("gzip", image, path) }], ["xz", ->(path) { compress("xz", image, path) }]]
  gpt_wrapper? ? list.unshift(["gpt", ->(path) { wrap_gpt(image, path) }]) : list
end

def wrap_check(name, dir, work)
  image = File.join(dir, "image.erofs")
  bare = listing(image)
  base = File.join(work, "wrap-#{name}")
  wrappers_for(image).filter_map do |label, build|
    wrapped = "#{base}.#{label}"
    build.call(wrapped) && wrapped_difference("#{name}/#{label}", wrapped, bare)
  end
end

def wrap_failures(results, work)
  WRAPPED_FIXTURES.flat_map do |name|
    passed = results.any? { _1.name == name && _1.verdict == :pass }
    next ["#{name}: not a passing fixture, nothing to wrap"] unless passed

    wrap_check(name, File.join(CORPUS, name), work)
  rescue StandardError => e
    ["#{name}: #{e.message}"]
  end
end

def summary(results, wrapped)
  counts = results.map(&:verdict).tally
  skipped = results.flat_map(&:skipped).tally.map { |kind, n| "#{n} #{kind}" }.join(", ")
  floor = counts.fetch(:pass, 0) < PASS_FLOOR ? ["fewer than #{PASS_FLOOR} passes"] : []
  line = "summary: #{counts.fetch(:pass, 0)} pass, #{counts.fetch(:fail, 0)} fail, " \
         "#{counts.fetch(:unsupported, 0)} unsupported, #{counts.fetch(:rejected, 0)} rejected as corrupt, " \
         "#{counts.fetch(:unchecked, 0)} unchecked " \
         "(opened, no manifest); times compared against the command line (change times of extended inodes without MTIME skipped); " \
         "not compared: #{skipped.empty? ? 'nothing' : skipped}"
  [line, counts.fetch(:fail, 0).zero? && wrapped.empty? && floor.empty?, floor]
end

Dir.mktmpdir("fsimg-erofs-") do |work|
  dirs = Dir.children(CORPUS).sort.map { File.join(CORPUS, _1) }.select { File.directory?(_1) }
  results = dirs.map { |dir| check(dir, work).tap { report(_1) } }
  wrapped = wrap_failures(results, work)
  wrapped.each { puts "FAIL        #{_1}" }
  gpt = gpt_wrapper? ? "gpt, " : ""
  puts "wrapped: #{WRAPPED_FIXTURES.size} fixtures through #{gpt}gzip, xz; #{wrapped.size} failures"
  puts "unsupported (not a pass): #{results.select { _1.verdict == :unsupported }.map(&:name).join(' ')}"
  line, ok, floor = summary(results, wrapped)
  puts line
  floor.each { puts "FAIL        #{_1}" }
  exit(ok ? 0 : 1)
end
