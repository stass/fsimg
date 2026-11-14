# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Generate the APFS combining class, canonical decomposition, case-folding
# and assigned-code-point tables from the Unicode Character Database. The
# rules are those of specs/design-apfs.md, "Directory records and names": a
# name's comparison form is its NFD, and on a case-insensitive volume the NFD
# of the full case folding (CaseFolding.txt statuses C and F) of its NFD; the
# writer refuses a name holding a noncharacter or a code point the pinned
# version does not assign.
#
#   ruby tools/gen-apfs-tables.rb VERSION UCD-DIRECTORY unicode-tables.h
#
# VERSION has no default: it must be a key of PINNED, whose digests the
# inputs UCD-DIRECTORY/UnicodeData.txt and CaseFolding.txt must match.
# Pinning another version means adding its digests here and to
# docs/references.md.
require "digest"

module ApfsTables
  INPUTS = %w[UnicodeData.txt CaseFolding.txt].freeze
  PINNED = {
    "17.0.0" => {
      "UnicodeData.txt" => "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c",
      "CaseFolding.txt" => "ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183"
    }.freeze
  }.freeze

  SCALAR_LIMIT = 0x110000
  SURROGATES = (0xD800..0xDFFF)
  HANGUL_FIRST = 0xAC00
  HANGUL_COUNT = 11_172
  HANGUL_L = 0x1100
  HANGUL_V = 0x1161
  HANGUL_T = 0x11A7
  HANGUL_TAILS = 28
  HANGUL_V_T = 21 * HANGUL_TAILS
  PAGE_POINTS = 256
  # Noncharacters: U+FDD0 to U+FDEF and the last two points of each plane.
  NONCHARACTERS = [*0xFDD0..0xFDEF, *(0..16).flat_map { [(_1 << 16) | 0xFFFE, (_1 << 16) | 0xFFFF] }].freeze
  # A mapping entry is the offset of its points shifted over their count.
  LENGTH_BITS = 3
  # The format design's bound on a comparison form's UTF-8 length, as a
  # multiple of its name's.
  EXPANSION_LIMIT = 3

  Record = Data.define(:code, :name, :combining_class, :canonical)
  Paged = Data.define(:index, :pages)
  Page = Data.define(:number, :values)
  Pool = Data.define(:entries, :points)

  module_function

  def generate(version:, directory:, output_path:)
    digests = PINNED.fetch(version) { abort("No pinned inputs for Unicode #{version}") }
    sources = INPUTS.to_h { [_1, pinned(File.join(directory, _1), digests.fetch(_1))] }
    records = parse_records(sources.fetch("UnicodeData.txt"))
    decompose = decomposer(records)
    folds = parse_folds(sources.fetch("CaseFolding.txt"))
      .transform_values { |points| points.flat_map(&decompose) }
    File.write(output_path, render(
      version:, digests:,
      classes: class_values(records),
      decompositions: table_decompositions(records, decompose),
      folds:,
      assigned: assigned_flags(records),
      expansion: expansion(decompose, folds)
    ))
  end

  def pinned(path, expected)
    File.binread(path).tap do |bytes|
      abort("Unexpected source hash for #{path}") unless Digest::SHA256.hexdigest(bytes) == expected
    end
  end

  def parse_records(text)
    text.each_line.map do |line|
      fields = line.chomp.split(";", -1)
      Record.new(
        code: fields[0].to_i(16),
        name: fields[1],
        combining_class: fields[3].to_i,
        # A tagged mapping is a compatibility one, which NFD never applies.
        canonical: fields[5].start_with?("<") ? [] : fields[5].split.map { _1.to_i(16) }
      )
    end
  end

  # Common (C) and full (F) foldings; simple (S) and Turkic (T) rows are the
  # alternatives a full, locale-independent folding does not apply.
  def parse_folds(text)
    text.each_line
      .map { _1.sub(/#.*/, "").split(";").map(&:strip) }
      .select { |fields| fields.size >= 3 && %w[C F].include?(fields[1]) }
      .to_h { |fields| [fields[0].to_i(16), fields[2].split.map { _1.to_i(16) }] }
  end

  # The full canonical decomposition of one point, Hangul syllables included.
  def decomposer(records)
    direct = records.select { _1.canonical.any? }.to_h { [_1.code, _1.canonical] }
    full = lambda do |point|
      next hangul(point) if hangul?(point)

      direct.key?(point) ? direct[point].flat_map(&full) : [point]
    end
  end

  def hangul?(point) = (HANGUL_FIRST...HANGUL_FIRST + HANGUL_COUNT).cover?(point)

  def hangul(point)
    s = point - HANGUL_FIRST
    tail = s % HANGUL_TAILS
    [HANGUL_L + (s / HANGUL_V_T), HANGUL_V + ((s % HANGUL_V_T) / HANGUL_TAILS), *([HANGUL_T + tail] unless tail.zero?)]
  end

  def class_values(records)
    classes = records.select { _1.combining_class.positive? }.to_h { [_1.code, _1.combining_class] }
    (0...SCALAR_LIMIT).map { classes.fetch(_1, 0) }
  end

  # Each decomposing point but the Hangul syllables, which unicode.c derives.
  def table_decompositions(records, decompose)
    records.select { _1.canonical.any? && !hangul?(_1.code) }.to_h { [_1.code, decompose.call(_1.code)] }
  end

  # Whether each point below SCALAR_LIMIT is a scalar value the records
  # assign: a listed point, or one in the range of a "<..., First>" record
  # and the "<..., Last>" record after it. Private-use points are assigned
  # (E4: macOS accepts them); surrogates are listed but are not scalar
  # values; Unicode never assigns a noncharacter, which this checks.
  def assigned_flags(records)
    points = records.chunk_while { |first, last| range_pair?(first, last) }
      .flat_map do |run|
        abort(format("Unpaired range record at U+%04X", run.first.code)) if run.size == 1 && range_name(run.first)
        (run.first.code..run.last.code).to_a
      end
      .reject { SURROGATES.cover?(_1) }
    abort("A noncharacter is assigned") if points.intersect?(NONCHARACTERS)
    points.each_with_object(Array.new(SCALAR_LIMIT, false)) { |point, flags| flags[point] = true }
  end

  # The range a "<NAME, First>" or "<NAME, Last>" record bounds, as
  # [NAME, "First" or "Last"], or nil for a record of one point.
  def range_name(record) = record.name.match(/\A<(.+), (First|Last)>\z/)&.captures

  def range_pair?(first, last) =
    ([range_name(first), range_name(last)] in [[name, "First"], [^name, "Last"]])

  # The largest ratio, rounded up, of the UTF-8 length of a point's NFD or
  # case-insensitive comparison form to its own. Either can be the longer:
  # folding expands some points and shrinks others.
  def expansion(decompose, folds)
    (0...SCALAR_LIMIT).reject { SURROGATES.cover?(_1) }.map do |point|
      nfd = decompose.call(point)
      caseless = nfd.flat_map { folds.fetch(_1, [_1]) }
      [nfd, caseless].map { utf8_length(_1).fdiv(utf8_length([point])).ceil }.max
    end.max.tap do |ratio|
      abort("A comparison form expands #{ratio} times, above #{EXPANSION_LIMIT}") if ratio > EXPANSION_LIMIT
    end
  end

  def utf8_length(points) = points.sum { _1 < 0x80 ? 1 : _1 < 0x800 ? 2 : _1 < 0x10000 ? 3 : 4 }

  # The pool of mapped points in point order, and each point's entry into it.
  def pool(mappings)
    mappings.sort.each_with_object(Pool.new(entries: Array.new(SCALAR_LIMIT, 0), points: [])) do |(point, points), out|
      abort(format("The mapping of U+%04X does not fit its entry", point)) unless points.size < (1 << LENGTH_BITS)
      out.entries[point] = (out.points.size << LENGTH_BITS) | points.size
      abort("The mapping pool does not fit its entries") unless out.entries[point] <= 0xFFFF
      out.points.concat(points)
    end
  end

  # Keep each distinct page that is not all zero once, as the page of its
  # first number. An index entry is 1 + the kept page's position, and 0 for
  # an all-zero page; the index ends after the last page that is not all
  # zero, and every page beyond it is zero.
  def paged(pages)
    kept = pages.each_with_index
      .filter_map { |values, number| Page.new(number:, values:) if values.any?(&:positive?) }
      .uniq(&:values)
    abort("More distinct pages than a uint8_t index holds") if kept.size > 255
    positions = kept.each_with_index.to_h { |page, position| [page.values, position + 1] }
    last = pages.rindex { _1.any?(&:positive?) }
    Paged.new(index: pages.first(last + 1).map { positions.fetch(_1, 0) }, pages: kept)
  end

  def value_pages(values) = values.each_slice(PAGE_POINTS).to_a

  # Pack flags into one 256-bit bitmap of 32 bytes per page, bit p & 7 of
  # byte (p & 0xff) >> 3. The fully assigned pages of the ideograph and
  # private-use blocks are equal, so paged keeps one copy of them.
  def bitmap_pages(flags)
    flags.each_slice(PAGE_POINTS).map do |page|
      page.each_slice(8).map { |bits| bits.each_with_index.sum { |set, bit| set ? 1 << bit : 0 } }
    end
  end

  def render(version:, digests:, classes:, decompositions:, folds:, assigned:, expansion:)
    decomposition_pool = pool(decompositions)
    fold_pool = pool(folds)
    [
      preamble(version, digests),
      "#include <stdint.h>\n",
      constants(version:, digests:, expansion:),
      class_comment,
      paged_table(name: "apfs_class", table: paged(value_pages(classes)),
                  type: "uint8_t", per_row: 16, value_format: "%3d"),
      decomposition_comment,
      paged_table(name: "apfs_decomp", table: paged(value_pages(decomposition_pool.entries)),
                  type: "uint16_t", per_row: 8, value_format: "0x%04x"),
      points_table("apfs_decomp_points", decomposition_pool.points),
      fold_comment,
      paged_table(name: "apfs_fold", table: paged(value_pages(fold_pool.entries)),
                  type: "uint16_t", per_row: 8, value_format: "0x%04x"),
      points_table("apfs_fold_points", fold_pool.points),
      assigned_comment,
      paged_table(name: "apfs_assigned", table: paged(bitmap_pages(assigned)),
                  type: "uint8_t", per_row: 8, value_format: "0x%02x")
    ].join("\n")
  end

  def preamble(version, digests)
    <<~C
      /* SPDX-License-Identifier: Unicode-3.0
       * Copyright (c) 1991-2026 Unicode, Inc.
       * Generated by tools/gen-apfs-tables.rb; do not edit. See
       * third-party/Unicode-LICENSE.txt. Regenerate with:
       *   ruby tools/gen-apfs-tables.rb #{version} UCD-DIRECTORY \\
       *       lib/formats/apfs/unicode-tables.h
      #{digests.map { |name, digest| " * #{name} #{version} SHA256: #{digest}" }.join("\n")}
       *
       * Each table is two-level, for any scalar value p: a page p >> 8 at or
       * beyond the length of NAME_index is all zero; otherwise let i be
       * NAME_index[p >> 8]; i == 0 means the page is all zero, and otherwise
       * the value is NAME_pages[i - 1][p & 0xff], except that apfs_assigned
       * holds bits (see its comment). Pages with equal values share one,
       * labelled by its first page number. Every index entry is in bounds of
       * its pages by construction.
       */
    C
  end

  def constants(version:, digests:, expansion:)
    <<~C
      #define APFS_UNICODE_VERSION "#{version}"
      #{digests.map { |name, digest| "#define APFS_UCD_#{name.delete_suffix(".txt").upcase}_SHA256 \"#{digest}\"" }.join("\n")}
      /* An entry of apfs_decomp or apfs_fold: (offset << SHIFT) | count. */
      #define APFS_MAP_OFFSET_SHIFT #{LENGTH_BITS}
      #define APFS_MAP_COUNT_MASK #{(1 << LENGTH_BITS) - 1}
      /* The largest ratio, rounded up, of the UTF-8 length of any scalar
       * value's comparison form to its own. The generator fails above 3.
       */
      #define APFS_UNICODE_EXPANSION #{expansion}
    C
  end

  def class_comment
    <<~C
      /*
       * Canonical combining classes, read after decomposing to sort each run
       * of nonzero classes stably; an all-zero page is all class 0.
       */
    C
  end

  def decomposition_comment
    <<~C
      /*
       * Full recursive canonical decompositions; an entry of zero means the
       * point stands for itself. Hangul syllables U+AC00 to U+D7A3 are not
       * tabled: with s = p - 0xAC00 they decompose to 0x1100 + s / 588,
       * 0x1161 + (s % 588) / 28, and 0x11A7 + s % 28 unless s % 28 is zero.
       */
    C
  end

  def fold_comment
    <<~C
      /*
       * Full case folding (CaseFolding.txt statuses C and F), each folding
       * already fully decomposed, so that folding a point of an NFD string
       * and reordering yields the NFD of its folding. An entry of zero means
       * the point folds to itself.
       */
    C
  end

  def assigned_comment
    <<~C
      /*
       * The scalar values the pinned version assigns, private use included,
       * as bitmaps: with i the index entry of page p >> 8 as above, p is
       * assigned when i != 0 and bit p & 7 of
       * apfs_assigned_pages[i - 1][(p & 0xff) >> 3] is set. No noncharacter
       * (U+FDD0 to U+FDEF, U+nFFFE and U+nFFFF) or surrogate is assigned.
       */
    C
  end

  def paged_table(name:, table:, type:, per_row:, value_format:)
    rows = table.pages.flat_map do |page|
      [
        format("\t/* page 0x%04x */ {", page.number),
        *page.values.each_slice(per_row).map { "\t\t#{_1.map { |value| format(value_format, value) }.join(", ")}," },
        "\t},"
      ]
    end
    [
      "static const uint8_t #{name}_index[#{table.index.size}] = {",
      *table.index.each_slice(16).map { "\t#{_1.map { |value| format("%3d", value) }.join(", ")}," },
      "};",
      "",
      "static const #{type} #{name}_pages[#{table.pages.size}][#{table.pages.first.values.size}] = {",
      *rows,
      "};",
      ""
    ].join("\n")
  end

  def points_table(name, points)
    [
      "static const uint32_t #{name}[#{points.size}] = {",
      *points.each_slice(8).map { "\t#{_1.map { |value| format("0x%05x", value) }.join(", ")}," },
      "};",
      ""
    ].join("\n")
  end
end

abort("usage: gen-apfs-tables.rb VERSION UCD-DIRECTORY unicode-tables.h") unless ARGV.size == 3
ApfsTables.generate(version: ARGV[0], directory: ARGV[1], output_path: ARGV[2])
