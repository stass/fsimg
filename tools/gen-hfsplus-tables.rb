# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Generate the HFS Plus fold, decomposition and combining class tables from
# UnicodeData 3.2.0 and tools/hfsplus-unicode-exceptions.txt. The rules are
# specs/design-hfsplus.md's; docs/hfsplus-research.md's E3 is the observation
# they answer to, and tools/hfsplus/check-unicode-tables.rb the check.
require "digest"

module HfsplusTables
  UNICODE_SHA256 = "5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446"
  EXCEPTIONS_SHA256 = "b7317b3da963c6e6b02fc188b56a17302ff1bbd3ec2374044119cc3094bb9319"

  FOLDED_PAGES = [0x00, 0x01, 0x03, 0x04, 0x05, 0x21, 0xFF].freeze
  GEORGIAN_CAPITALS = (0x10A0..0x10C5)
  GEORGIAN_TO_MKHEDRULI = 0x30
  IGNORABLE = [*0x200C..0x200F, *0x202A..0x202E, *0x206A..0x206F, 0xFEFF].freeze
  # A fold of zero means ignorable, so U+0000 needs another value: the largest,
  # which is what sorts it after every storable unit on a folded volume.
  NUL_FOLD = 0xFFFF
  NOT_DECOMPOSED = [0x2000..0x2FFF, 0xF900..0xFAFF].freeze
  BMP_LIMIT = 0x10000
  PAGE_UNITS = 256
  # A decomposition entry is the offset of its units shifted over their count.
  LENGTH_BITS = 3

  Record = Data.define(:code, :combining_class, :canonical, :lower)
  ExceptionRow = Data.define(:unit, :lower)
  Paged = Data.define(:index, :pages)
  Page = Data.define(:number, :values)

  module_function

  def generate(unicode_path:, exceptions_path:, output_path:)
    unicode = pinned(unicode_path, UNICODE_SHA256)
    exceptions = pinned(exceptions_path, EXCEPTIONS_SHA256)
    records = parse_records(unicode)
    File.write(output_path, render(
      fold: fold_values(records, parse_exceptions(exceptions)),
      classes: class_values(records),
      decompositions: decompositions(records)
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
        combining_class: fields[3].to_i,
        # A tagged mapping is a compatibility one, which HFS Plus never applies.
        canonical: fields[5].start_with?("<") ? [] : fields[5].split.map { _1.to_i(16) },
        lower: fields[13].empty? ? nil : fields[13].to_i(16)
      )
    end
  end

  def parse_exceptions(text)
    text.each_line.reject { _1.start_with?("#") }.map do |line|
      unit, lower, observation = line.chomp.split("\t", 3)
      abort("Exceptions row without an observation: #{line}") if observation.to_s.empty?
      ExceptionRow.new(unit: unit.to_i(16), lower: lower.to_i(16))
    end
  end

  # 65536 values, U+0000 included: lowercase on the folded pages, less the
  # exceptions; Georgian capitals, which Unicode 3.2.0 does not case-map; the
  # ignorable units; identity elsewhere.
  def fold_values(records, exceptions)
    lowercase = records
      .select { _1.lower && FOLDED_PAGES.include?(_1.code >> 8) }
      .to_h { [_1.code, _1.lower] }
    exceptions.each do |row|
      abort(format("Exception U+%04X names the wrong lowercase", row.unit)) unless lowercase[row.unit] == row.lower
    end
    folds = lowercase
      .except(*exceptions.map(&:unit))
      .merge(GEORGIAN_CAPITALS.to_h { [_1, _1 + GEORGIAN_TO_MKHEDRULI] })
      .merge(IGNORABLE.to_h { [_1, 0] })
      .merge(0 => NUL_FOLD)
    (0...BMP_LIMIT).map { folds.fetch(_1, _1) }
  end

  def class_values(records)
    classes = records
      .select { _1.code < BMP_LIMIT && _1.combining_class.positive? }
      .to_h { [_1.code, _1.combining_class] }
    (0...BMP_LIMIT).map { classes.fetch(_1, 0) }
  end

  # Each decomposing BMP unit and its full recursive canonical decomposition.
  def decompositions(records)
    direct = records
      .select { |record| record.code < BMP_LIMIT && record.canonical.any? && NOT_DECOMPOSED.none? { _1.cover?(record.code) } }
      .to_h { [_1.code, _1.canonical] }
    full = ->(unit) { direct.key?(unit) ? direct[unit].flat_map(&full) : [unit] }
    direct.keys.sort.to_h { [_1, full.call(_1)] }.tap do |result|
      abort("A decomposition leaves the BMP") unless result.values.flatten.all? { _1 < BMP_LIMIT }
    end
  end

  # The pool of decomposed units in unit order, and each unit's entry into it.
  def pool_entries(decompositions)
    decompositions.each_with_object({ pool: [], entries: Array.new(BMP_LIMIT, 0) }) do |(unit, units), out|
      abort("A decomposition does not fit its entry") unless units.size < (1 << LENGTH_BITS)
      out[:entries][unit] = (out[:pool].size << LENGTH_BITS) | units.size
      abort("The decomposition pool does not fit its entries") unless out[:entries][unit] <= 0xFFFF
      out[:pool].concat(units)
    end
  end

  # Split values into pages, keeping those that differ from the page `plain`
  # gives for their number. An index entry is 1 + the kept page's position, and
  # 0 for a plain page.
  def paged(values, plain:)
    pages = values.each_slice(PAGE_UNITS).with_index.filter_map do |page, number|
      Page.new(number:, values: page) unless page == plain.call(number)
    end
    positions = pages.each_with_index.to_h { |page, position| [page.number, position + 1] }
    Paged.new(index: (0...PAGE_UNITS).map { positions.fetch(_1, 0) }, pages:)
  end

  def render(fold:, classes:, decompositions:)
    zeros = Array.new(PAGE_UNITS, 0)
    identity = ->(number) { (0...PAGE_UNITS).map { (number * PAGE_UNITS) + _1 } }
    pool = pool_entries(decompositions)
    longest = decompositions.values.map(&:size).max
    [
      preamble,
      "#include <stdint.h>\n",
      constants(longest),
      fold_comment,
      paged_table("hfsplus_fold", paged(fold, plain: identity), "uint16_t", 8, "0x%04x"),
      decomposition_comment,
      paged_table("hfsplus_decomp", paged(pool[:entries], plain: ->(_) { zeros }), "uint16_t", 8, "0x%04x"),
      units_table(pool[:pool]),
      class_comment,
      paged_table("hfsplus_class", paged(classes, plain: ->(_) { zeros }), "uint8_t", 16, "%3d")
    ].join("\n")
  end

  def preamble
    <<~C
      /* SPDX-License-Identifier: Unicode-3.0
       * Copyright (c) 1991-2026 Unicode, Inc.
       * Generated by tools/gen-hfsplus-tables.rb; do not edit. See
       * third-party/Unicode-LICENSE.txt. Regenerate with:
       *   ruby tools/gen-hfsplus-tables.rb UnicodeData-3.2.0.txt \\
       *       tools/hfsplus-unicode-exceptions.txt \\
       *       lib/formats/hfsplus/unicode-tables.h
       * UnicodeData 3.2.0 SHA256: #{UNICODE_SHA256}
       * hfsplus-unicode-exceptions.txt SHA256: #{EXCEPTIONS_SHA256}
       *
       * Each table is two-level, for any 16-bit unit u: let i be
       * NAME_index[u >> 8]; i == 0 means the page is plain, and otherwise the
       * value is NAME_pages[i - 1][u & 0xff]. Every index entry is in bounds of
       * its pages by construction, so no input can reach outside a table.
       */
    C
  end

  def constants(longest)
    <<~C
      /* A hfsplus_decomp entry: (offset in hfsplus_decomp_units << SHIFT) | count. */
      #define HFSPLUS_DECOMP_OFFSET_SHIFT #{LENGTH_BITS}
      #define HFSPLUS_DECOMP_COUNT_MASK #{(1 << LENGTH_BITS) - 1}
      /* The most units a table entry decomposes into. */
      #define HFSPLUS_DECOMP_MAX_UNITS #{longest}
    C
  end

  def fold_comment
    <<~C
      /*
       * The folded comparator's table; a plain page folds to itself. A value of
       * zero marks an ignorable unit, which the comparison skips. U+0000 folds to
       * #{format("0x%04X", NUL_FOLD)}: it is not ignorable and sorts after every storable unit, tying
       * only with U+FFFF, which no POSIX name stores. The binary comparator uses no
       * table: it compares stored units, nothing is ignorable, and U+0000 sorts
       * first as itself.
       */
    C
  end

  def decomposition_comment
    <<~C
      /*
       * Full recursive canonical decompositions of BMP units; a plain page and an
       * entry of zero both mean the unit stands for itself. Supplementary values
       * never decompose. Hangul syllables U+AC00 to U+D7A3 are not tabled: a few
       * lines of arithmetic yield their jamo, where a table would hold 11172
       * entries. With s = u - 0xAC00 they are 0x1100 + s / 588,
       * 0x1161 + (s % 588) / 28, and 0x11A7 + s % 28 unless s % 28 is zero.
       */
    C
  end

  def class_comment
    <<~C
      /*
       * Canonical combining classes of BMP units, read after decomposing to sort
       * each run of nonzero classes stably; a plain page is all class 0. A
       * surrogate unit has class 0, so a supplementary mark acts as a starter.
       */
    C
  end

  def paged_table(name, table, type, per_row, value_format)
    rows = table.pages.flat_map do |page|
      [
        format("\t/* page 0x%02x */ {", page.number),
        *page.values.each_slice(per_row).map { "\t\t#{_1.map { |value| format(value_format, value) }.join(", ")}," },
        "\t},"
      ]
    end
    [
      "static const uint8_t #{name}_index[#{PAGE_UNITS}] = {",
      *table.index.each_slice(16).map { "\t#{_1.map { |value| format("%2d", value) }.join(", ")}," },
      "};",
      "",
      "static const #{type} #{name}_pages[#{table.pages.size}][#{PAGE_UNITS}] = {",
      *rows,
      "};",
      ""
    ].join("\n")
  end

  def units_table(units)
    [
      "static const uint16_t hfsplus_decomp_units[#{units.size}] = {",
      *units.each_slice(8).map { "\t#{_1.map { |value| format("0x%04x", value) }.join(", ")}," },
      "};",
      ""
    ].join("\n")
  end
end

abort("usage: gen-hfsplus-tables.rb UnicodeData-3.2.0.txt hfsplus-unicode-exceptions.txt unicode-tables.h") unless ARGV.size == 3
HfsplusTables.generate(unicode_path: ARGV[0], exceptions_path: ARGV[1], output_path: ARGV[2])
