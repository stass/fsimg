#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# The writer outputs of the APFS native qualification
# (docs/apfs-plan.md, "Native qualification and writer registration"). For
# each row of OUTPUTS it builds the row's source tree under --root, has
# `fsimg c` write the image, and leaves beside it the native-manifest.json
# that tools/apfs/qualify.rb compares the mounted volume against:
#
#   ROOT/sources/TREE/         the host tree a row stores
#   ROOT/sources/TREE.mtree    entries no host tree supplies (see Tree)
#   ROOT/sources/TREE.bytes/   their file contents
#   ROOT/outputs/ROW/image.*   the image and its manifest
#
# The manifest is taken from the source tree as qualify.rb reads a mount:
# kind, size, mode, birth and modification times, link target and SHA-256.
# An mtree entry's birth time is not compared, since it has none. Each row is
# printed as NAME, full or room, and the image path, so that only the rows
# expected to run out of space are qualified with --tolerate-full:
#
#   outputs.rb --fsimg BUILD/fsimg --root DIR > rows.tsv
#   qualify.rb --root QDIR --fsimg BUILD/fsimg --tolerate-full \
#     $(awk '$2 == "full" { print $3 }' rows.tsv)
#   qualify.rb --root QDIR --fsimg BUILD/fsimg $(awk '$2 == "room" { print $3 }' rows.tsv)
#
# --root must be private to this user, as qualify.rb requires of its own, and
# each tree and output directory is created exclusively: nothing is reused,
# replaced or removed.
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require_relative "qualify"

module ApfsOutputs
  class Error < ApfsQualify::Error; end

  # The mtime and birth time every source object gets, so that a manifest
  # and a remount agree whatever the host clock did in between. Setting a
  # time earlier than an object's birth time moves the birth time with it.
  EPOCH = 1_700_000_000
  MIB = 1 << 20
  GIB = 1 << 30

  # A row: the source tree, the stack given to -F, the exact size or, when
  # nil, automatic sizing with at least `free` bytes left free (none when nil,
  # the smallest container that holds the tree), the case setting, and
  # whether native writes are expected to run out of space.
  Row = Data.define(:name, :tree, :stack, :size, :free, :case_sensitive, :full)

  # A source tree: the host objects `build` makes in its directory, and the
  # extras, the entries no host tree supplies, given by an mtree manifest
  # merged with it: FIFOs, and names that differ only by case, which a
  # case-insensitive scratch volume cannot hold. An extra file's bytes are
  # stored under TREE.bytes/ and named by its contents keyword.
  Tree = Data.define(:build, :extras)
  Extra = Data.define(:path, :kind, :bytes)

  module_function

  def row(name, tree, stack: "apfs", size: nil, free: nil, case_sensitive: false, full: size.nil?) =
    Row.new(name: name, tree: tree, stack: stack, size: size, free: free, case_sensitive: case_sensitive, full: full)

  # ---- Source trees ---------------------------------------------------------

  def file(path, size, seed:)
    FileUtils.mkdir_p(File.dirname(path))
    File.open(path, "wbx") { |io| content(size, seed).each { io.write(_1) } }
  end

  # Bytes that differ per file and per MiB, so a misplaced extent shows.
  def content(size, seed)
    Enumerator.new do |out|
      (0...size).step(MIB) do |offset|
        length = [MIB, size - offset].min
        digest = Digest::SHA256.digest("#{seed}:#{offset}")
        out << (digest * ((length + 31) / 32)).byteslice(0, length)
      end
    end
  end

  # Names that test the name rules: the longest ASCII name, two- and four-byte
  # UTF-8 near the 255-byte host limit (the four-byte ones are UTF-16
  # surrogate pairs), decomposed and precomposed accents, and CJK. No two
  # differ only by normalization, which both personalities ignore.
  NAMES = ["n" * 255, "\u0416" * 127, "\u{1F600}" * 63, "caf\u00e9", "e\u0301tude", "\u6f22\u5b57",
           "space name", "dot.", "-dash"].freeze

  def mixed(dir, seed:)
    [0, 1, 4095, 4096, 4097, 65_536, 1_000_003].each do
      file(File.join(dir, "files", "f#{_1}"), _1, seed: "#{seed}#{_1}")
    end
    NAMES.each_with_index { file(File.join(dir, "names", _1), _2 * 100, seed: "#{seed}n#{_2}") }
    file(File.join(dir, "a", "b", "c", "d", "leaf"), 12_345, seed: "#{seed}leaf")
    FileUtils.mkdir_p(File.join(dir, "empty-dir"))
    FileUtils.mkdir_p(File.join(dir, "links"))
    File.symlink("../files/f4096", File.join(dir, "links", "relative"))
    File.symlink("/nowhere/at/all", File.join(dir, "links", "dangling"))
    File.symlink("t" * 1023, File.join(dir, "links", "longest"))
    File.symlink("\u00e9\u6f22", File.join(dir, "links", "unicode"))
  end

  def fifo(path) = Extra.new(path: path, kind: "fifo", bytes: nil)

  PIPE = [fifo("pipe")].freeze
  MIXED_EXTRAS = [*PIPE, fifo("names/pipe-\u00e9")].freeze
  CASED_EXTRAS = [Extra.new(path: "case", kind: "dir", bytes: nil),
                  *%w[Readme README readme].each_with_index.map do |name, index|
                    Extra.new(path: "case/#{name}", kind: "file", bytes: "#{name}\n" * (index + 1))
                  end].freeze

  TREES = {
    "empty" => Tree.new(build: ->(_dir) {}, extras: []),
    "mixed" => Tree.new(build: ->(dir) { mixed(dir, seed: "m") }, extras: MIXED_EXTRAS),
    "mixed-case" => Tree.new(build: ->(dir) { mixed(dir, seed: "c") }, extras: MIXED_EXTRAS + CASED_EXTRAS),
    "large" => Tree.new(build: ->(dir) { file(File.join(dir, "cross-chunk"), 200 * MIB + 17, seed: "large") },
                        extras: PIPE),
    # One file longer than the 5 GiB extent native captures show.
    "huge" => Tree.new(build: ->(dir) { file(File.join(dir, "past-5g"), 6 * GIB + 17, seed: "huge") }, extras: PIPE),
    "full" => Tree.new(build: lambda { |dir|
      48.times { file(File.join(dir, "data", format("%02d", _1)), MIB, seed: "full#{_1}") }
    }, extras: PIPE),
    "deep" => Tree.new(build: lambda { |dir|
      100.times.reduce(dir) do |parent, level|
        File.join(parent, format("d%03d", level)).tap { file(File.join(_1, "file"), level, seed: "deep#{level}") }
      end
    }, extras: PIPE),
    "wide" => Tree.new(build: lambda { |dir|
      6000.times do
        file(File.join(dir, "wide", format("entry-with-a-longer-name-%05d", _1)), _1 % 7, seed: "w#{_1}")
      end
    }, extras: PIPE)
  }.freeze

  OUTPUTS = [
    row("minimum", "empty"),
    row("minimum-cs", "empty", case_sensitive: true),
    row("mixed-ci", "mixed"),
    row("mixed-cs", "mixed-case", case_sensitive: true),
    row("small", "mixed", size: 64 * MIB),
    row("small-cs", "mixed-case", size: 64 * MIB, case_sensitive: true),
    row("multichunk", "large", size: GIB),
    row("multichunk-cs", "large", size: GIB, case_sensitive: true),
    row("huge", "huge", size: 8 * GIB),
    row("full", "full", free: 4 * MIB),
    row("deep", "deep"),
    row("wide-cs", "wide", free: 4 * MIB, case_sensitive: true),
    row("gpt-mixed", "mixed", stack: "apfs/gpt", size: 64 * MIB),
    row("gpt-wide", "wide", stack: "apfs/gpt", size: 64 * MIB),
    row("dmg-mixed-cs", "mixed-case", stack: "apfs/gpt/dmg", size: 64 * MIB, case_sensitive: true),
    row("dmg-deep", "deep", stack: "apfs/gpt/dmg")
  ].freeze

  MODES = { "dir" => 0o755, "fifo" => 0o640, "file" => 0o644 }.freeze

  def extras(tree) = TREES.fetch(tree).extras

  def mtree(tree)
    lines = extras(tree).map do |extra|
      line = "./#{vis(extra.path)} type=#{extra.kind} mode=#{format('%04o', MODES.fetch(extra.kind))} time=#{EPOCH}.0"
      extra.bytes ? "#{line} size=#{extra.bytes.bytesize} contents=#{vis(content_name(extra))}" : line
    end
    ["#mtree", *lines, ""].join("\n")
  end

  # Hex, since the scratch volume would fold names that differ by case.
  def content_name(extra) = extra.path.unpack1("H*")

  # mtree names escape every byte outside the printable ASCII as \ooo.
  def vis(name) = name.b.each_byte.map { (33..126).cover?(_1) && _1 != 92 ? _1.chr : format("\\%03o", _1) }.join

  def build_tree(sources, tree)
    dir = claim(File.join(sources, tree))
    TREES.fetch(tree).build.call(dir)
    File.write("#{dir}.mtree", mtree(tree), mode: "wx")
    bytes = claim("#{dir}.bytes")
    extras(tree).select(&:bytes).each { File.binwrite(File.join(bytes, content_name(_1)), _1.bytes, mode: "wbx") }
    settle(dir)
    dir
  end

  # Every object of the tree and the tree itself get EPOCH, links included.
  # Nothing is created in the tree afterwards, so no directory's time moves.
  def settle(dir)
    Dir.glob("**/*", File::FNM_DOTMATCH, base: dir).reject { %w[. ..].include?(File.basename(_1)) }
       .map { File.join(dir, _1) }.push(dir).each { File.lutime(EPOCH, EPOCH, _1) }
  end

  # A directory this run creates, so that it holds only what the run writes.
  def claim(path)
    Dir.mkdir(path, 0o700)
    path
  rescue Errno::EEXIST
    raise Error, "#{path} exists; outputs.rb never reuses or removes a tree or output"
  end

  # A directory beneath the private root, made by this or an earlier run; it
  # must be a directory of this user's that no one else may write.
  def private_child(root, name)
    path = File.join(root, name)
    Dir.mkdir(path, 0o700) unless File.exist?(path) || File.symlink?(path)
    return path if ApfsQualify.private?(path)

    raise Error, "#{path} is not a directory private to this user"
  end

  # ---- Manifests and images -------------------------------------------------

  def manifest(dir, tree)
    hosted = Dir.glob("**/*", File::FNM_DOTMATCH, base: dir).reject { %w[. ..].include?(File.basename(_1)) }.sort
    host_records = hosted.map { record(_1, ApfsQualify.view_of(File.join(dir, _1), digests: true)) }
    host_records + extras(tree).map { extra_record(_1) }
  end

  # An mtree entry has no birth time, so none is compared.
  def extra_record(extra)
    { path: extra.path, kind: extra.kind, size: extra.bytes&.bytesize, mode: MODES.fetch(extra.kind),
      mtime_ns: EPOCH * 1_000_000_000, sha256: extra.bytes && Digest::SHA256.hexdigest(extra.bytes) }.compact
  end

  def record(path, view)
    { path: path, kind: view.kind, size: view.size, mode: view.mode, btime_ns: view.created, mtime_ns: view.modified,
      target: view.target&.dup&.force_encoding("UTF-8"), sha256: view.sha256 }.compact
  end

  EXTENSIONS = { "apfs" => "img", "apfs/gpt" => "img", "apfs/gpt/dmg" => "dmg" }.freeze

  def create_argv(fsimg, row, source, image)
    sizing = [*(row.size ? ["-s", row.size.to_s] : []), *(row.free ? ["--free-space=#{row.free}"] : [])]
    [fsimg, "c", "-W", "-p", "-F", row.stack, "-f", image, *sizing,
     "-o", "case=#{row.case_sensitive ? 'sensitive' : 'insensitive'}", "-L", row.name,
     "-C", source, ".", "-C", "#{source}.bytes", "@#{source}.mtree"]
  end

  def write_image(fsimg, outputs, row, source)
    dir = claim(File.join(outputs, row.name))
    image = File.join(dir, "image.#{EXTENSIONS.fetch(row.stack)}")
    out, status = Open3.capture2e(*create_argv(fsimg, row, source, image))
    raise Error, "fsimg c #{row.name}: #{out}" unless status.success?

    File.write(File.join(dir, "native-manifest.json"), JSON.pretty_generate(manifest(source, row.tree)), mode: "wx")
    image
  end

  def parse(argv)
    options = {}
    names = OptionParser.new do |o|
      o.banner = "usage: outputs.rb --fsimg PATH --root DIR [ROW...]"
      o.on("--fsimg PATH", "the built fsimg") { options[:fsimg] = File.expand_path(_1) }
      o.on("--root DIR", "a directory private to this user for the trees and images") do
        options[:root] = File.expand_path(_1)
      end
    end.parse(argv)
    [options, names]
  end

  def main(argv)
    options, names = parse(argv)
    fsimg = options.fetch(:fsimg) { raise Error, "missing --fsimg" }
    root = options.fetch(:root) { raise Error, "missing --root" }
    rows = names.empty? ? OUTPUTS : names.map { |name| OUTPUTS.find { _1.name == name } || raise(Error, "no row #{name}") }
    ApfsQualify.private_directory(root)
    sources_dir = private_child(root, "sources")
    outputs_dir = private_child(root, "outputs")
    sources = rows.map(&:tree).uniq.to_h { [_1, build_tree(sources_dir, _1)] }
    rows.each do |row|
      image = write_image(fsimg, outputs_dir, row, sources.fetch(row.tree))
      puts "#{row.name}\t#{row.full ? 'full' : 'room'}\t#{image}"
    end
    0
  rescue Error, OptionParser::ParseError => e
    warn "outputs.rb: #{e.message}"
    1
  end
end

exit ApfsOutputs.main(ARGV) if $PROGRAM_NAME == __FILE__
