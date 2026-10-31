# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Native ImageFS evidence for test/ifs-integration.sh.
# Run `ifs-native.rb self-test` for the manifest and body helper tests.
#
# ifs-native.rb compare FSIMG TEST_IFS IFSDUMP IMAGE...
#
#   Compare fsimg's view of each bare ImageFS with an independent decoder.
#   The research decoder tools/qnx/decode/ifsdump.py --manifest lists every
#   record of the table in order, one line each of tab-separated fields: the
#   path relative to the mountpoint, the root's empty; the kind (file, dir,
#   link, fifo); the octal mode; uid; gid; mtime; and a file's POSIX cksum
#   and size separated by a blank, a link's target, or "-". This script
#   relies on that format and nothing else of the decoder. fsimg lists the
#   image through `fsimg t --json`, which also gives the info counts, and
#   `test-ifs --manifest` reads every file's and link's content through the
#   library.
#
#   The comparison derives fsimg's whole tree from the decoder's records
#   alone, by the rules of specs/design-ifs.md, and compares both ways:
#
#   * placement: the last record of a path decides it; earlier records of
#     the path are shadowed, and so is every path below a non-directory;
#   * implied parents: each proper prefix of a placed path that no record
#     names is synthesized, and the root when no record names it; their
#     number is the implicit_directories count, hidden ones included;
#   * omission: a placed or synthesized node whose own name lies outside
#     the name profile of lib/core/unicode.c while every ancestor's lies
#     inside is omitted with its subtree, and the omitted count is the
#     number of such subtree roots;
#   * listing: fsimg lists exactly the placed and synthesized nodes no
#     omission hides, a recorded one with its last record's kind,
#     permissions, owner, time and size, and a file's cksum or a link's
#     target, and a synthesized one with the implicit-directory values.
#
#   A file that does not begin with an ImageFS, such as a boot image, is
#   skipped and counted. An image fsimg refuses, exiting with status 1 and
#   a corrupt or unsupported error record, is counted as refused; test-ifs
#   --native checks the library's refusal of each capture against its own
#   model. Any other failure of fsimg, of the decoder or of the library
#   reading, such as a crash or a leak report, is a mismatch. Exit status 0
#   means every image agreed.
#
#   The decoder's manifest is line- and tab-separated, so a name or link
#   target holding a tab or newline byte makes an image's comparison fail
#   as a decoder-manifest mismatch.
#
# ifs-native.rb container FSIMG TEST_IFS IFSDUMP IMAGE DECODED
#
#   Compare fsimg's view of the ImageFS inside a container file, such as an
#   hbcifs image, with the decoder's manifest of DECODED, an oracle's decode
#   of the container's payload: the listing and content comparison above,
#   with fsimg and test-ifs reading IMAGE through its layers, and then an
#   extraction. fsimg extracts IMAGE into a private temporary directory
#   with -k, which skips the links safe extraction does not create and
#   would also omit a name outside the extraction profile. Every placed
#   directory and file must be extracted, so an omission is a mismatch,
#   each file with the cksum and size of its last record, and nothing
#   else. cksum(1) reads each extracted file on its standard input, so no
#   name reaches a command line. Exit status 0 means both comparisons
#   agreed.
#
# ifs-native.rb body IMAGE
#
#   Print where the ImageFS of an image file lies, as "OFFSET LENGTH PATH",
#   PATH the file's real path, since fsimg opens no symbolic link: at 0 in a
#   bare ImageFS, or after the startup section of an uncompressed boot
#   image, such as a `dumpifs -u` copy, whose little-endian startup header
#   the ifsboot profile of specs/design-layers.md describes. Print
#   "compressed PATH" for a boot image with a compressed body, which fsimg
#   reads whole through that container, and "none" for anything else.
#
# Member names are data of the images and never printed or passed on a
# command line: each line gives counts only, a mismatch is reported by
# category and count, and an unexpected failure by its exception type alone.
# Subprocess stderr is discarded because it can quote untrusted member bytes.
require "find"
require "json"
require "open3"
require "pathname"
require "set"
require "tmpdir"

module NativeIfs
  class Error < StandardError
  end

  class Refused < NativeIfs::Error
  end

  class ImageFailure < NativeIfs::Error
  end

  Record = Data.define(:kind, :mode, :uid, :gid, :mtime, :value)
  # last maps paths to their final records; parents holds synthesized paths,
  # including root. shadowed and omitted are info counts; listed is the set
  # of visible paths. Hidden implied parents still contribute to the count.
  Tree = Data.define(:last, :parents, :shadowed, :omitted, :listed)
  KINDS = {"file" => "file", "directory" => "dir", "symlink" => "link", "fifo" => "fifo"}.freeze
  IMPLICIT_DIRECTORY = ["directory", 0o755, 0, 0, 0].freeze
  REFUSALS = [-5, -6].freeze
  DETAILS = {
    "implicit" => "implicit_directories",
    "shadowed" => "shadowed",
    "omitted" => "omitted"
  }.freeze
  STARTUP_PREFIXES = [0, 1088].freeze
  STARTUP_HEADER_BYTES = 256
  STARTUP_SIGNATURE = "\xeb\x7e\xff\x00".b.freeze
  COMPRESSION_BITS = 0x1c

  module_function

  def python
    ENV
      .fetch("PATH", "")
      .split(File::PATH_SEPARATOR)
      .map { File.join(_1, "python3") }
      .find { File.file?(_1) && File.executable?(_1) }
  end

  def run(arguments:, category:, stdin: File::NULL)
    # stdin is an IO for cksum; no member path becomes an option or command.
    reader, writer = IO.pipe
    begin
      pid = Process.spawn(*arguments, in: stdin, out: writer, err: File::NULL)
      writer.close
      output = reader.read.b
      status = Process.wait2(pid).last
    ensure
      reader.close
      writer.close unless writer.closed?
    end

    raise ImageFailure, category unless status.success?
    output
  end

  def fields(line:, count:, category:)
    parts = line.split("\t".b, -1)
    raise ImageFailure, category unless parts.size == count
    parts
  end

  def number(text:, category:, base: 10)
    Integer(text, base)
  rescue ArgumentError
    raise ImageFailure, category
  end

  def in_profile?(name)
    return false if ["", ".", ".."].include?(name)
    return false if name.bytes.any? { _1 < 0x20 || [0x2f, 0x7f].include?(_1) }
    name.dup.force_encoding(Encoding::UTF_8).valid_encoding?
  end

  def oracle_records(ifsdump:, image:)
    category = "decoder manifest"
    output = run(arguments: [python, "-I", ifsdump, "--manifest", image], category: category)
    output.split("\n".b).reject(&:empty?).map do |line|
      path, kind, mode, uid, gid, mtime, value = fields(line: line, count: 7, category: category)
      record = Record.new(
        kind: kind,
        mode: number(text: mode, category: category, base: 8),
        uid: number(text: uid, category: category),
        gid: number(text: gid, category: category),
        mtime: number(text: mtime, category: category),
        value: value
      )
      [path, record]
    end
  end

  def fsimg_view(fsimg:, image:)
    category = "fsimg listing"
    output, status = Open3.capture2(fsimg, "t", "--json", "-f", image, err: File::NULL)
    lines = output.lines.map { JSON.parse(_1) }
    unless status.success?
      errors = lines.select { _1["type"] == "error" }
      raise Refused if status.exitstatus == 1 && !errors.empty? && REFUSALS.include?(errors.last["code"])
      raise ImageFailure, category
    end

    info = lines.select { _1["type"] == "info" }
    listed = lines.select { _1["type"] == "entry" }
    entries = listed.to_h { [_1.fetch("path").encode(Encoding::UTF_8).b[1..], _1] }
    raise ImageFailure, category unless info.size == 1 && entries.size == listed.size
    [info.first.fetch("details").transform_values { Integer(_1, 10) }, entries]
  rescue JSON::ParserError
    raise ImageFailure, category
  end

  def library_content(test_ifs:, image:)
    category = "library content"
    output = run(arguments: [test_ifs, "--manifest", image], category: category)
    output.split("\n".b).reject(&:empty?).to_h do |line|
      case line.split("\t".b, -1)
      in ["F", path, crc, size]
        [path[1..], [number(text: crc, category: category), number(text: size, category: category)]]
      in ["L", path, target]
        raise ImageFailure, category unless /\A(?:[0-9a-fA-F]{2})*\z/.match?(target)
        [path[1..], [target].pack("H*")]
      else
        raise ImageFailure, category
      end
    end
  end

  def prefixes(path)
    parts = path.split("/".b, -1)
    (1...parts.size).map { parts.first(_1).join("/".b) }
  end

  def below_non_directory?(path:, last:)
    prefixes(path).any? { last.key?(_1) && last.fetch(_1).kind != "dir" }
  end

  # Derive placement and visibility from the independent manifest before
  # comparing either direction against the CLI listing.
  def expected_tree(records)
    last = records.to_h.freeze
    placed = last.keys.reject { !_1.empty? && below_non_directory?(path: _1, last: last) }.to_set
    parents = placed.include?("".b) ? Set.new : Set["".b]
    placed.reject(&:empty?).each do |path|
      parents.merge(prefixes(path).reject { last.key?(_1) })
    end

    listed = Set.new
    omitted = 0
    ((placed | parents) - Set["".b]).each do |path|
      inside = path.split("/".b, -1).map { in_profile?(_1) }
      if inside[0...-1].all? && !inside.last
        omitted += 1
      elsif inside.all?
        listed.add(path)
      end
    end

    Tree.new(
      last: last,
      parents: parents.freeze,
      shadowed: records.size - placed.size,
      omitted: omitted,
      listed: listed.freeze
    )
  end

  def entry_values(entry)
    [
      entry["kind"],
      entry["mode"],
      entry["uid"],
      entry["gid"],
      Integer(entry.fetch("modified").fetch("seconds"), 10)
    ]
  end

  def compare_entry(entry:, expected:, content:)
    counts = Hash.new(0)
    mismatches = Hash.new(0)
    path = entry.fetch("path").encode(Encoding::UTF_8).b[1..]
    kind, mode, uid, gid, mtime = entry_values(entry)
    if [KINDS[kind], mode, uid, gid, mtime] != [
          expected.kind,
          expected.mode & 0o7777,
          expected.uid,
          expected.gid,
          expected.mtime
        ]
      mismatches["kind, mode, owner or time"] += 1
    end

    case expected.kind
    when "file"
      crc, _, size = expected.value.partition(" ".b)
      value = [number(text: crc, category: "decoder manifest"), number(text: size, category: "decoder manifest")]
      counts["files"] += 1
      if content[path] != value || Integer(entry.fetch("size_bytes"), 10) != value.last
        mismatches["file size or bytes"] += 1
      end

    when "link"
      counts["links"] += 1
      mismatches["link target"] += 1 if content[path] != expected.value
    end

    [counts, mismatches]
  end

  def compare(fsimg:, test_ifs:, ifsdump:, image:, decoded: image)
    details, entries = fsimg_view(fsimg: fsimg, image: image)
    content = library_content(test_ifs: test_ifs, image: image)
    tree = expected_tree(oracle_records(ifsdump: ifsdump, image: decoded))
    counts = Hash.new(0).merge(
      "implicit" => tree.parents.size,
      "shadowed" => tree.shadowed,
      "omitted" => tree.omitted,
      "entries" => entries.size
    )
    mismatches = Hash.new(0)
    mismatches["listed entries the table does not place"] = (entries.keys.to_set - tree.listed).size
    mismatches["placed entries fsimg does not list"] = (tree.listed - entries.keys).size
    (entries.keys.to_set & tree.listed).each do |path|
      if tree.last.key?(path)
        entry_counts, entry_mismatches = compare_entry(
          entry: entries.fetch(path),
          expected: tree.last.fetch(path),
          content: content
        )
        entry_counts.each { |name, count| counts[name] += count }
        entry_mismatches.each { |name, count| mismatches[name] += count }
      elsif entry_values(entries.fetch(path)) != IMPLICIT_DIRECTORY
        mismatches["implicit directory values"] += 1
      end
    end

    DETAILS.each { |name, detail| mismatches["#{name} count"] += 1 if counts[name] != details[detail] }
    [counts, mismatches.select { _2.positive? }, tree]
  end

  def file_cksum(path)
    category = "extracted content"
    parts = File.open(path, "rb") { run(arguments: ["cksum"], category: category, stdin: _1).split }
    raise ImageFailure, category unless parts.size == 2
    parts.map { number(text: _1, category: category) }
  end

  def extracted_entries(root)
    found = {}
    Find.find(root) do |path|
      next if path == root
      relative = Pathname.new(path).relative_path_from(Pathname.new(root)).to_s.b
      stat = File.lstat(path)
      found[relative] = if stat.directory?
        ["dir"]
      elsif stat.file?
        ["file", *file_cksum(path)]
      else
        ["other"]
      end
    end

    found
  end

  def expected_extraction(tree)
    tree
      .listed
      .filter_map do |path|
        record = tree.last[path]
        if record.nil? || record.kind == "dir"
          [path, ["dir"]]
        elsif record.kind == "file"
          crc, _, size = record.value.partition(" ".b)
          [
            path,
            [
              "file",
              number(text: crc, category: "decoder manifest"),
              number(text: size, category: "decoder manifest")
            ]
          ]
        end
      end
      .to_h
  end

  def compare_extraction(fsimg:, image:, tree:)
    found = Dir.mktmpdir("fsimg-ifs-extract-") do |root|
      run(
        arguments: [
          fsimg,
          "x",
          "-k",
          "-E",
          "-C",
          root,
          "-f",
          image
        ],
        category: "fsimg extraction"
      )
      extracted_entries(root)
    end

    expected = expected_extraction(tree)
    mismatches = {
      "extracted entries the table does not place" => (found.keys - expected.keys).size,
      "placed entries fsimg does not extract" => (expected.keys - found.keys).size,
      "extracted kind, size or bytes" => (found.keys & expected.keys).count { found[_1] != expected[_1] }
    }.select { _2.positive? }
    counts = {
      "extracted_files" => found.values.count { _1.first == "file" },
      "extracted_directories" => found.values.count { _1.first == "dir" }
    }
    [counts, mismatches]
  end

  def describe(counts)
    "#{counts["entries"]} entries, #{counts["files"]} files' bytes, " \
      "#{counts["links"]} link targets, #{counts["implicit"]} implicit directories, " \
      "#{counts["shadowed"]} shadowed records, #{counts["omitted"]} omitted subtrees"
  end

  def report(name:, mismatches:)
    mismatches.sort.each { |category, count| puts("#{name}: MISMATCH #{category}: #{count}") }
  end

  def compare_images(fsimg:, test_ifs:, ifsdump:, images:)
    skipped = refused = failures = 0
    total = Hash.new(0)
    images.each do |image|
      name = File.basename(image)
      unless File.open(image, "rb") { _1.read(7) == "imagefs".b }
        skipped += 1
        next
      end

      begin
        counts, mismatches, = compare(
          fsimg: fsimg,
          test_ifs: test_ifs,
          ifsdump: ifsdump,
          image: image
        )
      rescue Refused
        refused += 1
        puts("#{name}: refused by fsimg")
        next
      rescue ImageFailure => failure
        counts = Hash.new(0)
        mismatches = {"#{failure} failure" => 1}
      end

      counts.each { |key, value| total[key] += value }
      puts("#{name}: #{describe(counts)}")
      report(name: name, mismatches: mismatches)
      failures += mismatches.size
    end

    puts(
      "compare: #{images.size - skipped} images, #{refused} refused, " \
        "#{total["entries"]} entries, #{total["files"]} files' bytes, " \
        "#{total["links"]} link targets, #{failures} mismatches; " \
        "#{skipped} files without an ImageFS at offset 0"
    )
    failures.zero? ? 0 : 1
  end

  def compare_container(fsimg:, test_ifs:, ifsdump:, image:, decoded:)
    name = File.basename(image)
    counts, mismatches, tree = compare(
      fsimg: fsimg,
      test_ifs: test_ifs,
      ifsdump: ifsdump,
      image: image,
      decoded: decoded
    )
    extracted, extra = compare_extraction(fsimg: fsimg, image: image, tree: tree)
    mismatches = mismatches.merge(extra) { |_, left, right| left + right }
    puts(
      "#{name}: #{describe(counts)}; extracted #{extracted["extracted_files"]} files " \
        "and #{extracted["extracted_directories"]} directories"
    )
    report(name: name, mismatches: mismatches)
    mismatches.empty? ? 0 : 1
  rescue Refused
    puts("#{name}: refused by fsimg")
    1
  rescue ImageFailure => failure
    puts("#{name}: MISMATCH #{failure} failure: 1")
    1
  end

  # Locate an ImageFS body and report OFFSET LENGTH PATH, compressed PATH,
  # or none, as specified by the body command contract above.
  def body_report(image)
    File.open(image, "rb") do |stream|
      prefix = stream.read(STARTUP_PREFIXES.max + STARTUP_HEADER_BYTES) || "".b
      size = stream.stat.size
      return "0 #{size} #{File.realpath(image)}" if prefix[0, 7] == "imagefs".b
      STARTUP_PREFIXES.each do |start|
        header = prefix[start, STARTUP_HEADER_BYTES]
        next if header.nil? || header.size < STARTUP_HEADER_BYTES || header[0, 4] != STARTUP_SIGNATURE
        return "compressed #{File.realpath(image)}" unless (header.getbyte(6) & COMPRESSION_BITS).zero?
        offset = start + header[0x20, 4].unpack1("L<")
        length = header[0x2c, 4].unpack1("L<")
        stream.seek(offset)
        return "none" if offset + length > size || stream.read(7) != "imagefs".b
        return "#{offset} #{length} #{File.realpath(image)}"
      end
    end

    "none"
  end

  def call(arguments)
    if %w[compare container].include?(arguments.first) && python.nil?
      warn("SKIP: ifsdump comparison requires optional python3")
      return 77
    end

    case arguments
    in ["compare", fsimg, test_ifs, ifsdump, *images] if !images.empty?
      compare_images(fsimg: fsimg, test_ifs: test_ifs, ifsdump: ifsdump, images: images)
    in ["container", fsimg, test_ifs, ifsdump, image, decoded]
      compare_container(
        fsimg: fsimg,
        test_ifs: test_ifs,
        ifsdump: ifsdump,
        image: image,
        decoded: decoded
      )
    in ["body", image]
      puts(body_report(image))
      0
    else
      abort(
        "usage: ifs-native.rb compare FSIMG TEST_IFS IFSDUMP IMAGE... | container FSIMG TEST_IFS IFSDUMP IMAGE DECODED | body IMAGE"
      )
    end
  end
end

if $PROGRAM_NAME == __FILE__ && ARGV == ["self-test"]
  require "minitest/autorun"

  class NativeIfsTest < Minitest::Test
    def test_empty_final_component_is_omitted
      record = NativeIfs::Record.new(kind: "file", mode: 0o644, uid: 0, gid: 0, mtime: 0, value: "0 0")
      tree = NativeIfs.expected_tree([["parent/".b, record]])
      assert_equal(1, tree.omitted, "an empty final component counts as one omitted subtree")
      assert_equal(Set["parent".b], tree.listed, "only the implicit parent remains visible")
    end

    def test_empty_file_has_no_imagefs_body
      Dir.mktmpdir("ifs-native-self-test-") do |root|
        image = File.join(root, "empty.ifs")
        File.binwrite(image, "".b)
        assert_equal("none", NativeIfs.body_report(image), "an empty file contains no ImageFS body")
      end
    end
  end

  ARGV.clear
elsif $PROGRAM_NAME == __FILE__
  begin
    exit(NativeIfs.call(ARGV))
  rescue StandardError => failure
    # The declared boundary records only the type: a traceback could quote
    # member bytes from an untrusted image.
    warn("ifs-native.rb: #{failure.class}")
    exit(1)
  end
end
