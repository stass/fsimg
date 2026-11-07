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
# rule rejects them, a fixture with a manifest included, and `W` must
# refuse each of them with the same reason.
# A fixture whose `i` fails with the
# "unsupported capability" status is counted and named as not yet supported,
# never a pass, and only for a reason listed in KNOWN_REFUSALS: any other
# refusal, or fewer than PASS_FLOOR passes, fails the run. A refusal naming
# a compression library (liblzma 5.4, libzstd) is a pass only for a build
# whose `--formats` listing lacks that library's EROFS codec, and only when
# the volume lists and `W` refuses it for the same reason; each such fixture
# lowers the floor by one. The hand-built head-2 fixtures are also read with
# COMPR_HEAD2 set (tools/erofs/head2-bit.rb), where their head-2 records are
# legal (HEAD2_WITH_BIT). A few
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

require_relative "../tools/erofs/head2-bit"

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
# The epoch a fixture was built with, whether its extended inodes keep source
# times, and whether it has the 48-bit layout, whose compact inodes keep them too.
TimeRule = Data.define(:epoch, :source, :wide)
COMPACT_TIME_RANGE = 1 << 32

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
# The passes of a build with every library; a fixture slipping to
# "unsupported" must not keep the run green.
PASS_FLOOR = 151
# Corpus entries left out: capture.rb's in-progress directories (`.tmp-`
# in the name), and the 5 GiB E13 and E10 files, which test-erofs-scale
# reads by streaming them to a digest, since extracting one here writes
# 5 GiB.
STREAMED_FIXTURES = /\A(e13-big5g|e10-48-lz4-big\z)/
# Fixtures covering geometry, inline data, and an attribute-bearing tree.
WRAPPED_FIXTURES = %w[e1-geom-b512 e3-b4096-inline e6-xattr-default].freeze
SECTOR = 512
FIRST_SECTOR = 2048

# The only reasons the reader may give for an image it cannot read yet.
KNOWN_REFUSALS = [/extra devices\z/].freeze

# A cluster of an algorithm whose library this build lacks, and the codec
# name `--formats` lists for the library.
LIBRARY_REFUSAL = /cluster needs (liblzma 5\.4|libzstd)\z/
LIBRARY_CODECS = { "liblzma 5.4" => "lzma", "libzstd" => "zstd" }.freeze

# The hand-built head-2 images read with COMPR_HEAD2 set: the digest /y must
# have (e9-head2-lzma's expected.txt), or the refusal of an LZ4 stream that
# its head-2 record calls MicroLZMA; and the codec /y's record names, which
# a build may lack. Evidence: E9.md.
HEAD2_WITH_BIT = { "e9-head2-lzma" => [:digest, "lzma"], "e9-head2-lz4-ctrl" => [:digest, "lz4"],
                   "e9-head2-wrongalg" => [/MicroLZMA stream/, "lzma"] }.freeze
HEAD2_DIGEST_SOURCE = "e9-head2-lzma"

# Fixtures without a manifest that end in a refusal, and its class. Evidence:
# ~/.cache/fsimg-erofs/evidence/E8.md (the cleared-flag edits), E14.md
# (extra devices), E15.md (logical clusters of several blocks, compact
# advice 0x01 clear), E10.md (48-bit volumes and their edits). A fixture
# listed neither here nor in EXPECTED_CORRUPT must open, list and extract.
EXPECTED_REFUSALS = {
  "e14-blobdev-b4096" => /extra devices/, "e14-tar-index-b4096" => /extra devices/,
  # Bit 4 on extended files, which the kernel ignores: refused by policy
  # (specs/design-erofs.md#48-bit-layout).
  "e10-edit-kinds-extbit4" => /extended inode format bit 4\z/,
  "e8-lz4-flag-cleared" => /LZ4 cluster without LZ4_0PADDING/,
  "e8-lz4-flag-cleared-distance0" => /LZ4 cluster without LZ4_0PADDING/,
  "e8-lz4-unpadded" => /LZ4 cluster without LZ4_0PADDING/,
  **%w[a-compact-b2048-l4096-adv1 a-compact-b2048-l4096-adv1-lcount a-compact-b4096-l16384-adv0
       a-compact-b4096-l8192-adv0 a-compact-b4096-l8192-adv1flip a-full-b4096-l16384 a-full-b4096-l8192
       c-l2b-tail-at-B c-l2b-tail-at-L].to_h { ["e15-#{_1}", /logical cluster size other than the block size/] },
  **%w[b-compact-b2048-adv0 b-compact-b4096-adv0].to_h { ["e15-#{_1}", /compact index without 2-byte groups/] }
}.freeze

# Fixtures that the reader rejects as corrupt, and the rule it names
# (specs/design-erofs.md#chunk-based, #compression and #features), judged
# by that refusal whether or not they have a manifest. Evidence:
# ~/.cache/fsimg-erofs/evidence/E7.md (edited chunk indexes and formats),
# E8.md (the hand-written block ends), E9.md (two-header edits), E15.md
# (hand-placed inline tails on a volume without ZTAILPACKING), E12.md (a
# box of encoded extents without 48BIT, which the kernel reads).
EXPECTED_CORRUPT = {
  "e7-edit-block-oor" => /data blocks outside the volume/, "e7-edit-device-id" => /chunk device identifier/,
  "e7-edit-format-0x40" => /chunk format 48-bit bit/, "e7-edit-format-0x80" => /chunk format\z/,
  "e7-edit-high-part" => /chunk block high bits/,
  # E10.md: encoded extents without 48BIT, and a count high half of 1.
  "e10-edit-lz4-no48" => /encoded extents without 48BIT\z/,
  "e12-flat-box32-lz4" => /encoded extents without 48BIT\z/,
  "e10-edit-lz4-counthi" => /encoded extent count above the size's blocks\z/,
  "e8-lz4-block-end" => /lz4: /, "e9-head2-head1-wrong" => /lz4: /,
  "e9-head2-lz4-ctrl" => /head-2 record without COMPR_HEAD2/, "e9-head2-lzma" => /head-2 record without COMPR_HEAD2/,
  "e9-head2-wrongalg" => /head-2 record without COMPR_HEAD2/,
  **%w[c-il-nowrap c-il-plain-wrap c-il-rot-wrap c-il-short c-shift-nowrap c-shift-short c-shift-wrap]
    .to_h { ["e15-#{_1}", /compressed inline tail without ZTAILPACKING/] }
}.freeze

# Files of a pinned fixture that must still read, by their expected.txt
# digests: the hand-written LZ4 blocks that meet both end conditions (E8).
READABLE_DESPITE_REFUSAL = { "e8-lz4-block-end" => %w[./c0-baseline-tail5 ./c8-lastmatch12-tail5] }.freeze

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

# A command with --json: its output, or Unsupported when it refused with the
# "unsupported capability" status, which means the reader does not handle
# this image, or part of it, yet.
def run(*args)
  out, err, status = fsimg(*args)
  return out if status.success?

  error = parse_lines(out).find { _1["type"] == "error" }
  raise Unsupported, error["message"] if error && error["status"] == "unsupported capability"

  raise "#{args.first} failed: #{failure_line(out, err)}"
end

# `i` is the support probe.
def probe(image) = parse_lines(run("i", "--json", "-f", image)).find { _1["type"] == "info" }

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
  run("x", "--json", "-E", "-k", *limit, "-f", image, "-C", dir)
end

# The output limit for extracting a tree, from its manifest rows or its
# listing: the files' logical bytes, holes included, which a sparse or
# highly compressed file (E8 zeros40m) can raise past the default of 16
# times the volume's length, plus room for link targets.
def output_limit(rows) = ["-l", "output=#{rows.select { _1.kind == 'file' }.sum(&:size) + (1 << 20)}"]

# A path with a ".." component, which no reader should report, is never
# followed out of dir.
def file_sha(dir, path)
  return nil if path.split("/").include?("..")

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
  TimeRule.new(epoch: epoch.to_i, source: keeps && !flattened, wide: command.match?(/-E\s*\S*48bit/))
end

# A compact inode holds no time of its own: it delivers the epoch (mkfs
# chooses it for a source time equal to the epoch in whole seconds). Under
# the 48-bit layout the superblock's epoch is zero and mkfs.erofs 1.9.4
# keeps whole seconds of the source time: modulo 2^32 in a compact inode
# (E10, e10-48-small's 1950 and 2110 files), whole in an extended one, which
# only a hard link or a wide start forces (e10-48-large's z-hl1, whose
# nanoseconds dump.erofs also shows as zero). Otherwise an extended inode
# delivers the source time, nanoseconds included when the manifest carries
# them.
def wide_time_check(row, seen, compact:)
  want = compact ? row.seconds % COMPACT_TIME_RANGE : row.seconds
  return NONE if seen.time == [want, 0]

  Comparison.of("#{row.path}: time #{seen.time.join('.')} != #{want}.0 (whole seconds of #{row.seconds})")
end

def time_check(row, seen, rule:, mtime:)
  want_key = mtime ? "modified" : "changed"
  unless seen.time_key == want_key
    return Comparison.of("#{row.path}: time delivered as #{seen.time_key.inspect}, expected #{want_key}")
  end

  compact = seen.precision == COMPACT_PRECISION_NS
  # Without the MTIME bit an extended inode stores the source's change time,
  # which no manifest column carries.
  return Comparison.of(skipped: [:change_time]) if !mtime && !compact

  return wide_time_check(row, seen, compact:) if rule.source && rule.wide

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

# The files READABLE_DESPITE_REFUSAL names, read one by one with `x -O`,
# against their expected.txt digests.
def readable_differences(name, dir, image)
  paths = READABLE_DESPITE_REFUSAL.fetch(name, [])
  return [] if paths.empty?

  sums = File.read(File.join(dir, "expected.txt")).each_line(chomp: true).to_h { _1.split("  ", 2).reverse }
  paths.filter_map do |path|
    out, _, status = fsimg("x", "-O", "-f", image, path.delete_prefix("."))
    got = status.success? ? Digest::SHA256.hexdigest(out) : nil
    "#{path}: sha256 #{got.inspect} != #{sums[path]}" unless got == sums[path]
  end
end

# A fixture without a manifest, or one pinned as corrupt: it opens, lists
# and extracts, or ends in the refusal pinned for it.
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
    diffs = [*verify_refusal_difference(image, want), *readable_differences(name, dir, image)]
    refused = EXPECTED_CORRUPT.key?(name) ? :rejected : :unsupported
    Result.new(name:, verdict: diffs.empty? ? refused : :fail, notes: [text, *diffs], skipped: [])
  in [String => text, _]
    Result.new(name:, verdict: :fail, notes: ["unexpected refusal: #{text}"], skipped: [])
  end
end

CAP_READ = 0x01

def format_records = parse_lines(run("--formats", "--json")).select { _1["type"] == "format" }

# The EROFS codecs this build decodes, by the names `--formats` lists.
def build_codecs(records) =
  records.find { _1["name"] == "erofs" }.fetch("codecs").to_s.split("; ").map { _1.split(",").first }

# The compressions this build reads, which a build without liblzma lacks xz of.
def readable_compressions(records) =
  records.select { _1["kind"] == "compression" && _1["capabilities"].anybits?(CAP_READ) }.map { _1["name"] }

# A refusal naming a library this build lacks, when the volume still lists
# and verification refuses it for the same reason; the difference otherwise.
def library_difference(message, image)
  library = message[LIBRARY_REFUSAL, 1]
  return "#{library} refused, but this build decodes #{LIBRARY_CODECS[library]}" if BUILD_CODECS.include?(LIBRARY_CODECS[library])

  listing(image)
  verify_refusal_difference(image, Regexp.new(Regexp.escape(message)))
rescue RuntimeError, Unsupported => e
  "the rest of the volume does not read: #{e.message}"
end

def library_refused(name, dir, message)
  difference = library_difference(message, File.join(dir, "image.erofs"))
  Result.new(name:, verdict: difference ? :fail : :no_library, notes: [message, *difference], skipped: [])
end

def check(dir, work)
  name = File.basename(dir)
  pinned_corrupt = EXPECTED_CORRUPT.key?(name)
  return compare_unlisted(name, dir, work) if pinned_corrupt || !File.exist?(File.join(dir, "manifest.txt"))

  compare(name, dir, work)
rescue Unsupported => e
  return library_refused(name, dir, e.message) if e.message.match?(LIBRARY_REFUSAL)

  known = KNOWN_REFUSALS.any? { e.message.match?(_1) }
  note = known ? e.message : "unexpected refusal: #{e.message}"
  Result.new(name:, verdict: known ? :unsupported : :fail, notes: [note], skipped: [])
rescue StandardError => e
  Result.new(name:, verdict: :fail, notes: ["#{e.class}: #{e.message}"], skipped: [])
end

# The rust-fs-erofs comparison. Every fixture fsimg reads (a pass or an
# unchecked fixture) is also listed and its files read by rust-fs-erofs, an
# independent reader, and the two readings must agree on each portable path:
# kind, the size of a file or symbolic link, permission bits, modification
# seconds, device numbers, link target and the SHA-256 of a file's content.
# Owners, nanoseconds and attributes are not compared, since rust-fs-erofs
# does not list them, nor a directory's size, which fsimg delivers as 0. The
# work is bounded by the files' listed bytes, never the image's length, so a
# sparse multi-terabyte fixture reads only its few files.
RUST_FS_EROFS = File.join(ENV.fetch("FSIMG_ORACLES", File.join(Dir.home, ".local/share/fsimg-oracles")),
                          "rust-erofs/bin/rust-fs-erofs")
RUST_KINDS = {
  "file" => "file", "dir" => "dir", "symlink" => "symlink", "fifo" => "fifo",
  "socket" => "socket", "char" => "chardev", "block" => "blockdev"
}.freeze
# Bounds far past every fixture, so rust-fs-erofs never stops a walk short.
RUST_DEPTH = 4096
RUST_LIMIT = 100_000_000
# A fixture with more file bytes or more files than these has each nonempty
# file streamed through `export PATH -` and hashed, rather than exported as
# a tree to the work directory, which holds fsimg's extraction of the same
# files: rust-fs-erofs syncs every file it exports, so e13-dir100k's 100000
# empty files take minutes that comparing their sizes makes redundant.
RUST_EXPORT_BYTES = 256 << 20
RUST_EXPORT_FILES = 10_000
EMPTY_SHA = Digest::SHA256.hexdigest("")
# Fixtures rust-fs-erofs refuses although fsimg and mkfs.erofs agree on
# them: name => refusal. rust-fs-erofs 0.5.1 reads 64 bytes for every
# inode, so a compact (32-byte) inode in the volume's last 32 bytes, which
# e4-nearfull-b4096 holds, is a short read to it. It also fails to export
# dozens of LZ4 files of the two e8-lz4-compact fixtures ("LZ4 decompression
# failed", "decompressed size mismatch", "input is all zeros") and the
# files of e6-xattr-pfx-packed whose tails are fragments ("inline PLAIN tail
# shorter than its extent"); fsck.erofs 1.9.4 extracts every regular file of
# those three images with the bytes fsimg extracts, which match the source
# manifests. The cause inside rust-fs-erofs is not diagnosed.
RUST_REFUSALS = {
  # rust-fs-erofs 0.5.1 has no 48-bit layout.
  **%w[e1-bt-48bit e2-vm-48bit e13-limits-48bit e10-48-chunk e10-48-deflate e10-48-dotomit e10-48-frag e10-48-kinds e10-48-large
       e10-48-lz4 e10-48-lz4-b512 e10-48-small e10-edit-small-nlink1 e10-edit-small-wide e10-hand-lz4-bytes
       e10-hand-lz4-exact e10-hand-lz4-padded].to_h { [_1, /feature_incompat 48bit/] },
  # Nor the metadata box, which it names first on a volume with both bits.
  **%w[e10-hand-box32 e12-box-chunk e12-box-lz4 e12-box-lz4-b1024 e12-box-lz4-frag e12-box-lz4-m16k
       e12-box-lz4-pfx-frag e12-box-pfx-lz4 e12-box-pfx-lz4-plainpfx e12-box-pfx-plain e12-box-pfx-plain-b512
       e12-box-plain e12-box-plain-b512 e12-box-plain-poolbox e12-flat-box-lz4 e12-flat-box-plain
       e12-flat-box-poolbox e12-flat-box32-plain e12-flat-box32-poolbox e13-limits-metabox
       e9-mixed-lz4-lzma-meta e9-mixed-lzma-lz4-meta]
    .to_h { [_1, /feature_incompat metabox/] },
  "e4-nearfull-b4096" => /ShortRead \{ offset: 8160, want: 64, got: 32 \}/,
  "e6-xattr-pfx-packed" => /malformed inode: inline PLAIN tail shorter than its extent/,
  "e8-lz4-compact-counts" => /malformed inode: LZ4 /,
  "e8-lz4-compact-xattr" => /malformed inode: LZ4 /
}.freeze

# What one reader delivers for a path.
Entry = Data.define(:kind, :size, :mode, :seconds, :dev, :target, :sha)

class RustRefusal < StandardError; end

def rust_available? = File.executable?(RUST_FS_EROFS)

def rust_version = Open3.capture2(RUST_FS_EROFS, "--version").first.strip

# The error text of a failed rust-fs-erofs command: its JSON record on
# stderr, or the raw stderr.
def rust_error(err)
  JSON.parse(err.lines.last.to_s).fetch("error")
rescue JSON::ParserError, KeyError, TypeError
  err.strip
end

def rust_json(*args)
  out, err, status = Open3.capture3(RUST_FS_EROFS, "fs", *args)
  raise RustRefusal, rust_error(err) unless status.success?

  JSON.parse(out)
end

# rust-fs-erofs 0.5.1 prints an extended inode's signed 64-bit seconds as
# unsigned, so a time before the epoch arrives near 2^64.
def signed64(value) = value >= 1 << 63 ? value - (1 << 64) : value

def rust_listing(image) =
  rust_json(image, "ls", "-R", "--max-depth", RUST_DEPTH.to_s, "--limit", RUST_LIMIT.to_s, "/").to_h do |item|
    kind = RUST_KINDS.fetch(item.fetch("type"), item["type"])
    dev = DEVICES.include?(kind) ? [item["major"], item["minor"]] : nil
    size = SIZED.include?(kind) ? item.fetch("size") : nil
    [item.fetch("path"), Entry.new(kind:, size:, mode: item.fetch("mode").to_i(8) & 0o7777,
                                   seconds: signed64(item.fetch("mtime")), dev:, target: item["target"], sha: nil)]
  end

# SHA-256 of a file read by rust-fs-erofs through a pipe.
def rust_streamed_sha(image, path)
  Open3.popen3(RUST_FS_EROFS, "fs", image, "export", path, "-") do |stdin, out, err, thread|
    stdin.close
    errors = Thread.new { err.read }
    digest = Digest::SHA256.new
    while (chunk = out.read(1 << 20))
      digest << chunk
    end
    raise RustRefusal, "export #{path}: #{rust_error(errors.value)}" unless thread.value.success?

    digest.hexdigest
  end
end

# SHA-256 of every file, from one recursive export's report. A second name
# of a hard-linked inode is linked, not copied, and reports no digest, so it
# is hashed from the exported tree. Special files are skipped, which makes
# the command exit with status 5 (rust-fs-erofs 0.5.1), so the report, not
# the status, says whether the export is whole: nothing refused, failed or
# cancelled.
def rust_exported_shas(image, dir)
  out, err, = Open3.capture3(RUST_FS_EROFS, "fs", image, "export", "--recursive", "--preserve=links",
                             "--max-depth", RUST_DEPTH.to_s, "/", dir)
  report = find_export_report(out)
  raise RustRefusal, "export: #{rust_error(err)}" unless report

  items = report.fetch("items").select { rust_portable?(export_path(_1)) }
  failed = items.reject { %w[completed skipped].include?(_1.fetch("status")) }
  unless failed.empty? && !report.fetch("cancelled")
    raise RustRefusal, "export #{failed.first&.then { export_path(_1) }}: #{failed.first&.fetch('reason')}"
  end

  items.to_h { |item| [export_path(item), item.dig("copy", "sha256") || file_sha(dir, export_path(item))] }
end

# An export item's path: a failed item names only its parent and name.
def export_path(item) = item["path"] || File.join(item.fetch("parent"), item.fetch("name"))

# rust-fs-erofs prints each byte of a name that is not UTF-8 as U+FFFD, and
# cannot export it to a macOS volume, so such a path stands for a name the
# portable profile leaves out. A real name holding U+FFFD is lost with
# them, which fails the comparison as "listed by fsimg only", never passes
# it.
def rust_portable?(path) = portable?(path) && !path.include?("\uFFFD")

# The export report on stdout, or nil when there is none.
def find_export_report(out)
  JSON.parse(out).then { _1.is_a?(Hash) && _1.key?("items") ? _1 : nil }
rescue JSON::ParserError
  nil
end

# The SHA-256 of each file rust-fs-erofs lists, by path. An empty file is
# not read: both readers list its size, and its content is nothing.
def rust_shas(image, files:, work:)
  if files.values.sum > RUST_EXPORT_BYTES || files.size > RUST_EXPORT_FILES
    return files.to_h { |path, size| [path, size.zero? ? EMPTY_SHA : rust_streamed_sha(image, path)] }
  end

  dir = File.join(work, "rust-export")
  begin
    rust_exported_shas(image, dir)
  ensure
    FileUtils.rm_rf(dir)
  end
end

# fsimg's reading of the same paths, from its listing and its extraction.
def fsimg_entries(image, out) =
  listing(image).to_h do |seen|
    target = seen.kind == "symlink" ? link_target(image, seen.path) : nil
    sha = seen.kind == "file" ? file_sha(out, seen.path) : nil
    dev = DEVICES.include?(seen.kind) ? seen.dev : nil
    size = SIZED.include?(seen.kind) ? seen.size : nil
    [seen.path, Entry.new(kind: seen.kind, size:, mode: seen.mode, seconds: seen.time&.first, dev:,
                          target:, sha:)]
  end

def entry_differences(path, ours:, theirs:) =
  ours.to_h.filter_map do |field, value|
    "#{path}: #{field} #{value.inspect} (fsimg) != #{theirs.public_send(field).inspect} (rust-fs-erofs)" \
      unless value == theirs.public_send(field)
  end

def reading_differences(ours, theirs)
  missing = (ours.keys - theirs.keys).map { "#{_1}: listed by fsimg only" }
  extra = (theirs.keys - ours.keys).map { "#{_1}: listed by rust-fs-erofs only" }
  missing + extra + (ours.keys & theirs.keys).flat_map { entry_differences(_1, ours: ours[_1], theirs: theirs[_1]) }
end

# Both readings of one fixture, restricted to the portable paths fsimg
# delivers, and the count of paths left out.
def rust_readings(name, image:, work:)
  ours = fsimg_entries(image, File.join(work, name))
  listed = rust_listing(image)
  theirs = listed.select { |path, _| rust_portable?(path) }
  files = theirs.select { _2.kind == "file" }.transform_values(&:size)
  shas = rust_shas(image, files:, work:)
  theirs = theirs.to_h { |path, entry| [path, entry.with(sha: entry.kind == "file" ? shas[path] : nil)] }
  portable = (ours.keys | theirs.keys).select { portable?(_1) }
  [ours.slice(*portable), theirs.slice(*portable), (ours.keys | listed.keys).size - portable.size]
end

# The comparison for a fixture fsimg read, or nil for one it did not.
def rust_result(result, dir:, work:)
  return nil unless %i[pass unchecked].include?(result.verdict)

  name = File.basename(dir)
  ours, theirs, dropped = rust_readings(name, image: File.join(dir, "image.erofs"), work:)
  diffs = reading_differences(ours, theirs)
  Result.new(name: "#{name} (rust-fs-erofs)", verdict: diffs.empty? ? :agree : :fail, notes: diffs.first(MAX_NOTES),
             skipped: [:non_portable_path] * dropped)
rescue RustRefusal => e
  pinned = RUST_REFUSALS[name]&.then { e.message.match?(_1) }
  Result.new(name: "#{name} (rust-fs-erofs)", verdict: pinned ? :rust_refused : :fail,
             notes: ["rust-fs-erofs refused: #{e.message}"], skipped: [])
rescue StandardError => e
  # The per-comparison boundary, as check's: any other error fails this
  # fixture's comparison and the run goes on.
  Result.new(name: "#{name} (rust-fs-erofs)", verdict: :fail, notes: ["#{e.class}: #{e.message}"], skipped: [])
end

# One fixture's results: fsimg's, then the rust-fs-erofs comparison's. The
# fixture's extraction is removed afterwards, so the work directory holds one
# fixture's files at a time.
def fixture_results(dir, work)
  result = check(dir, work)
  [result, *(rust_available? ? rust_result(result, dir:, work:) : nil)]
ensure
  FileUtils.rm_rf(File.join(work, File.basename(dir)))
end

def rust_summary(results)
  return "rust-fs-erofs: skipped, #{RUST_FS_EROFS} is not executable" unless rust_available?

  counts = results.select { _1.name.end_with?("(rust-fs-erofs)") }.map(&:verdict).tally
  "rust-fs-erofs: #{rust_version}; #{counts.fetch(:agree, 0)} agree, " \
    "#{counts.fetch(:rust_refused, 0)} refused as pinned, #{counts.fetch(:fail, 0)} differ"
end

# `x -O` of one path: its SHA-256, or the refusal text.
def extracted_digest(image, path)
  out, err, status = fsimg("x", "-O", "-f", image, path)
  status.success? ? Digest::SHA256.hexdigest(out) : failure_line(out, err)
end

def head2_expected_digest =
  File.read(File.join(CORPUS, HEAD2_DIGEST_SOURCE, "expected.txt")).split.first

# One hand-built head-2 image with the bit set: /y reads to the digest, or
# fails as pinned; a build without liblzma refuses MicroLZMA naming it.
def head2_check(name:, want:, codec:, work:)
  image = File.join(work, "#{name}-head2-bit.erofs")
  File.binwrite(image, Head2Bit.with_bit(File.binread(File.join(CORPUS, name, "image.erofs"))))
  got = extracted_digest(image, "/y")
  expected = BUILD_CODECS.include?(codec) ? want : LIBRARY_REFUSAL
  ok = expected == :digest ? got == head2_expected_digest && verify_failure(image).nil? : got.match?(expected)
  Result.new(name: "#{name} (COMPR_HEAD2 set)", verdict: ok ? :pass : :fail, notes: ok ? [] : ["/y: #{got}"], skipped: [])
end

def head2_results(work) =
  HEAD2_WITH_BIT.filter_map do |name, (want, codec)|
    head2_check(name:, want:, codec:, work:) if File.exist?(File.join(CORPUS, name))
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
  list = %w[gzip xz].select { READABLE_COMPRESSIONS.include?(_1) }.map do |program|
    [program, ->(path) { compress(program, image, path) }]
  end
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
  want = PASS_FLOOR - counts.fetch(:no_library, 0)
  floor = counts.fetch(:pass, 0) < want ? ["fewer than #{want} passes"] : []
  line = "summary: #{counts.fetch(:pass, 0)} pass, #{counts.fetch(:fail, 0)} fail, " \
         "#{counts.fetch(:no_library, 0)} need a library this build lacks, " \
         "#{counts.fetch(:unsupported, 0)} unsupported, #{counts.fetch(:rejected, 0)} rejected as corrupt, " \
         "#{counts.fetch(:unchecked, 0)} unchecked " \
         "(opened, no manifest); times compared against the command line (change times of extended inodes without MTIME skipped); " \
         "not compared: #{skipped.empty? ? 'nothing' : skipped}"
  [line, counts.fetch(:fail, 0).zero? && wrapped.empty? && floor.empty?, floor]
end

BUILD_CODECS, READABLE_COMPRESSIONS = format_records.then { [build_codecs(_1), readable_compressions(_1)] }.map(&:freeze)

Dir.mktmpdir("fsimg-erofs-") do |work|
  dirs = Dir.children(CORPUS).sort.reject { _1.include?(".tmp-") || _1.match?(STREAMED_FIXTURES) }
            .map { File.join(CORPUS, _1) }.select { File.directory?(_1) }
  results = dirs.flat_map { |dir| fixture_results(dir, work).each { report(_1) } } +
            head2_results(work).each { report(_1) }
  wrapped = wrap_failures(results, work)
  wrapped.each { puts "FAIL        #{_1}" }
  gpt = gpt_wrapper? ? "gpt, " : ""
  puts "wrapped: #{WRAPPED_FIXTURES.size} fixtures through #{gpt}#{(READABLE_COMPRESSIONS & %w[gzip xz]).join(', ')}; " \
       "#{wrapped.size} failures"
  puts "unsupported (not a pass): #{results.select { _1.verdict == :unsupported }.map(&:name).join(' ')}"
  line, ok, floor = summary(results, wrapped)
  puts rust_summary(results)
  puts line
  floor.each { puts "FAIL        #{_1}" }
  exit(ok ? 0 : 1)
end
