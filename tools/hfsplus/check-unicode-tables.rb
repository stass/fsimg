# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# The gate on lib/formats/hfsplus/unicode-tables.h: compare what the header says
# with the E3 observation and the NetBSD witness (docs/hfsplus-research.md).
# It parses the generated C, shares nothing with tools/gen-hfsplus-tables.rb, and
# exits nonzero on any violation. A development tool run by hand, because its
# inputs live outside the tree:
#
#   ruby tools/hfsplus/check-unicode-tables.rb [CAPTURES] [HEADER]
require "digest"

module CheckUnicodeTables
  ROOT = File.expand_path("../..", __dir__)
  DEFAULT_CAPTURES = File.expand_path("~/hfsplus-captures")
  DEFAULT_HEADER = File.join(ROOT, "lib/formats/hfsplus/unicode-tables.h")
  EXCEPTIONS = File.join(ROOT, "tools/hfsplus-unicode-exceptions.txt")

  # Each SHA256SUMS pins its directory's tables.
  PINS = {
    "e3-hfsplus/SHA256SUMS" => "6f33f14e50f361139c8457b009a11764fb950141753c0e4ac17d051ad16c02e2",
    "e3-hfsx/SHA256SUMS" => "90bfe9d53fc699366c03fb25b9135522419d82eef858521d922aa307ab2dc404",
    "e3-small-names/SHA256SUMS" => "f60315f4096dcb8d7f3ae5cbf4c8d5c47fc17dae02c79b9bc793dcb449b7f6e9",
    "unicode-3.2.0/UnicodeData-3.2.0.txt" => "5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446",
    "netbsd-libhfs/libhfs.c" => "d4aeef80a6b06232340182da3d74bc91e03bfc39de084713eac2d714f10d7f94"
  }.freeze

  UNITS = (0..0xFFFF)
  HANGUL = (0xAC00..0xD7A3)
  HIGH_SURROGATES = (0xD800..0xDBFF)
  LOW_SURROGATES = (0xDC00..0xDFFF)
  LOWER_A = 0x61
  LOWER_B = 0x62

  # The values the system stores as something else than their decomposition:
  # docs/hfsplus-research.md's "U+0000" and "Refused spellings".
  SYSTEM_MAPPED = { 0x3A => [0x2F], 0x2400 => [0x0000] }.freeze
  ESCAPED = [0xFFFE, 0xFFFF].freeze

  # The rows of a set that no leaf holds: U+0000, which no syscall can carry, and
  # path syntax. Every other row must store a name.
  UNSTORED = {
    "alone" => %w[0000 002E 002F],
    "between" => %w[0000 002F]
  }.freeze

  # The tables as the header gives them, flat per unit.
  Tables = Data.define(:fold, :classes, :decompositions)
  Comparator = Data.define(:name, :fold, :folded)
  FoldRow = Data.define(:unit, :status, :fold, :chain)
  NameRow = Data.define(:id, :disposition, :representative, :pass, :leaf, :rank)
  # What one check found: named counts and violation messages.
  Result = Data.define(:counts, :violations) do
    def +(other) = with(
      counts: counts.merge(other.counts) { |_, mine, theirs| mine + theirs },
      violations: violations + other.violations
    )
  end
  NO_RESULT = Result.new(counts: {}, violations: [])

  module_function

  def run(captures:, header:)
    verify_inputs(captures)
    text = File.read(header)
    tables = parse_header(text)
    print_digests(tables)
    finish(check_header_digests(text, captures) + run_checks(captures, tables))
  end

  def run_checks(captures, tables)
    plus_rows = fold_rows(File.join(captures, "e3-hfsplus/fold.tsv"))
    hfsx_rows = fold_rows(File.join(captures, "e3-hfsx/fold.tsv"))
    plus = Comparator.new(name: "hfsplus", fold: tables.fold, folded: true)
    # HFSX with compare type 0xBC is the binary comparison: identity, no skips.
    hfsx = Comparator.new(name: "hfsx", fold: UNITS.to_a, folded: false)
    [
      check_exceptions(plus_rows),
      self_test(plus_rows, tables.fold),
      report_fold(plus, plus_rows),
      report_fold(hfsx, hfsx_rows),
      check_sets(captures, tables, plus),
      check_sets(captures, tables, hfsx),
      witness(captures, tables, plus_rows),
      self_test_marks(captures, tables)
    ].sum(NO_RESULT)
  end

  def finish(result)
    result.counts.sort.each { |name, count| puts format("%-52s %d", name, count) }
    puts result.violations.first(200)
    puts "violations: #{result.violations.size}"
    exit(result.violations.empty? ? 0 : 1)
  end

  def verify_inputs(captures)
    PINS.each do |path, expected|
      verify_digest(File.join(captures, path), expected)
      next unless path.end_with?("SHA256SUMS")

      File.foreach(File.join(captures, path), chomp: true) do |line|
        digest, name = line.split(/\s+/, 2)
        verify_digest(File.join(captures, File.dirname(path), name), digest)
      end
    end
  end

  def verify_digest(path, expected)
    abort("Unexpected digest for #{path}") unless Digest::SHA256.file(path).hexdigest == expected
  end

  def check_header_digests(text, captures)
    unicode = Digest::SHA256.file(File.join(captures, "unicode-3.2.0/UnicodeData-3.2.0.txt")).hexdigest
    exceptions = Digest::SHA256.file(EXCEPTIONS).hexdigest
    missing = [unicode, exceptions].reject { text.include?(_1) }
    Result.new(counts: {}, violations: missing.map { "the header does not carry input digest #{_1}" })
  end

  # Parsing the header ------------------------------------------------------

  def parse_header(text)
    source = text.gsub(%r{/\*.*?\*/}m, "")
    shift = define(source, "HFSPLUS_DECOMP_OFFSET_SHIFT")
    mask = define(source, "HFSPLUS_DECOMP_COUNT_MASK")
    pool = numbers(source, "hfsplus_decomp_units")
    entries = flat(source, "hfsplus_decomp") { 0 }
    Tables.new(
      fold: flat(source, "hfsplus_fold") { _1 },
      classes: flat(source, "hfsplus_class") { 0 },
      decompositions: entries.each_with_index.reject { |entry, _| entry.zero? }.to_h do |entry, unit|
        [unit, pool.slice(entry >> shift, entry & mask)]
      end
    )
  end

  def define(source, name) = Integer(source[/^#define #{name} (\S+)$/, 1])

  # One table's value for every unit; the block gives a plain page's.
  def flat(source, name)
    index = numbers(source, "#{name}_index")
    pages = numbers(source, "#{name}_pages").each_slice(256).to_a
    UNITS.map do |unit|
      position = index.fetch(unit >> 8)
      position.zero? ? yield(unit) : pages.fetch(position - 1).fetch(unit & 0xFF)
    end
  end

  def numbers(source, name)
    body = source[/\b#{name}\[[^=]*= \{(.*?)^\};/m, 1] or abort("The header has no #{name}")
    body.scan(/0x\h+|\d+/).map { Integer(_1) }
  end

  # The fold observation ----------------------------------------------------

  def fold_rows(path)
    File.readlines(path, chomp: true).drop(1).map do |line|
      unit, status, fold, chain = line.split("\t")
      FoldRow.new(unit: unit.to_i(16), status:, fold:, chain: chain == "-" ? nil : chain.to_i)
    end
  end

  # Every row of the observation against one table, as the record's rules state.
  def fold_violations(rows, table)
    rows.flat_map { row_violations(_1, table) } +
      chain_violations(rows, table) +
      pair_only_violations(rows, table) +
      position_violations(rows, table)
  end

  def row_violations(row, table)
    case row.status
    in "determined" unless table[row.unit] == row.fold.to_i(16)
      [format("U+%04X folds to %04X, observed %s", row.unit, table[row.unit], row.fold)]
    in "ignorable" unless table[row.unit].zero?
      [format("U+%04X is ignorable, the table folds it to %04X", row.unit, table[row.unit])]
    else
      []
    end
  end

  # The joint rule: one fold per chain position, a listed value, strictly
  # ascending over the whole chain. Row membership alone would accept a table
  # that breaks it, so self_test holds this function to rejecting such tables.
  def chain_violations(rows, table)
    positions = rows.select(&:chain).group_by(&:chain).sort.map(&:last)
    folds = positions.map { |members| members.map { table[_1.unit] }.uniq }
    classes = positions.zip(folds)
    split = classes.select { |_, values| values.size > 1 }
      .map { |members, _| format("chain %d has more than one fold", members.first.chain) }
    unlisted = classes.reject { |members, values| listed?(members.first, values.first) }
      .map { |members, values| format("chain %d folds to %04X, not one of %s", members.first.chain, values.first, members.first.fold) }
    descents = folds.map(&:first).each_cons(2).with_index.reject { |(a, b), _| a < b }
      .map { |(a, b), position| format("chain %d folds to %04X, chain %d to %04X", position, a, position + 1, b) }
    split + unlisted + descents
  end

  def listed?(row, fold) = row.fold.split("|").map { _1.to_i(16) }.include?(fold)

  def pair_only_violations(rows, table)
    groups = rows.select { _1.status == "pair-only" }.group_by(&:fold).values
    unit_order = groups.flat_map do |group|
      group.map(&:unit).each_cons(2).reject { |a, b| table[a] < table[b] }.map do |a, b|
        format("pair-only U+%04X folds to %04X, U+%04X to %04X", a, table[a], b, table[b])
      end
    end
    # A high unit stands where a BMP unit can: its run lies between these two.
    bounds = [[0xD7FF, HIGH_SURROGATES.first], [HIGH_SURROGATES.last, 0xE000]]
      .reject { |a, b| table[a] < table[b] }
      .map { |a, b| format("U+%04X folds to %04X, not below U+%04X's %04X", a, table[a], b, table[b]) }
    unit_order + bounds
  end

  def position_violations(rows, table)
    position = rows.find { _1.status == "position" }
    highest = rows.select(&:chain).map { table[_1.unit] }.max
    case position.fold
    in "first" then table[0].zero? ? [] : ["U+0000 is not first"]
    in /\Aafter:(\h+)\z/
      after = Regexp.last_match(1).to_i(16)
      table[0] > [table[after], highest].max ? [] : [format("U+0000 folds to %04X, not after every unit", table[0])]
    end
  end

  def report_fold(comparator, rows)
    statuses = rows.map(&:status).tally.to_h { |status, count| ["#{comparator.name} fold #{status}", count] }
    reasons = rows.select { _1.status == "unobservable" }.map(&:fold).tally
      .to_h { |reason, count| ["#{comparator.name} fold unobservable #{reason}", count] }
    chains = { "#{comparator.name} fold chain positions" => rows.filter_map(&:chain).uniq.size }
    Result.new(
      counts: statuses.merge(reasons, chains),
      violations: fold_violations(rows, comparator.fold).map { "#{comparator.name}: #{_1}" }
    )
  end

  # Every row of the exceptions file is an observed unit that keeps itself.
  def check_exceptions(rows)
    by_unit = rows.to_h { [_1.unit, _1] }
    units = File.readlines(EXCEPTIONS, chomp: true).reject { _1.start_with?("#") }.map { _1.split("\t").first.to_i(16) }
    bad = units.reject { by_unit.fetch(_1).status == "determined" && by_unit.fetch(_1).fold.to_i(16) == _1 }
    Result.new(
      counts: { "exceptions rows" => units.size },
      violations: bad.map { format("exception U+%04X is not an observed self-fold", _1) }
    )
  end

  # Altered tables each pass a row-by-row membership test yet break one part of
  # the joint rule; the checker must reject them.
  def self_test(rows, table)
    mutants = {
      "one fold per chain" => table.dup.tap { _1[0x0410] = 0x0410 },
      "folds ascend" => table.dup.tap { _1[0x0411] = 0x0411; _1[0x0431] = 0x0411 }
    }
    rejected, accepted = mutants.partition { |_, mutant| fold_violations(rows, mutant).any? }
    vacuous = mutants.select { |_, mutant| membership_only(rows, mutant).any? }
    Result.new(
      counts: { "self-test mutants rejected" => rejected.size },
      violations: accepted.map { "self-test #{_1.first}: the mutated table was accepted" } +
        vacuous.map { "self-test #{_1.first}: membership alone rejects the mutant, so it proves nothing" }
    )
  end

  # The marks vectors must notice a wrong combining class: with U+0323 given
  # class 230, the stable sort no longer puts it before U+0300.
  def self_test_marks(captures, tables)
    rows = name_rows(File.join(captures, "e3-small-names/marks-hfsx.tsv"))
    mutant = tables.with(classes: tables.classes.dup.tap { _1[0x323] = 230 })
    caught = check_stored("self-test", rows, ->(id) { scalars(id) }, mutant).violations
    Result.new(
      counts: { "self-test marks mutants rejected" => caught.empty? ? 0 : 1 },
      violations: caught.empty? ? ["self-test marks: the mutated class table was accepted"] : []
    )
  end

  # What the joint rule is stronger than: each row's own fold is a listed value.
  def membership_only(rows, table) =
    rows.select(&:chain).reject { listed?(_1, table[_1.unit]) }

  # The name sets ------------------------------------------------------------

  def name_rows(path)
    File.readlines(path, chomp: true).drop(1).map do |line|
      id, _scope, disposition, representative, pass, _readdir, leaf, rank = line.split("\t")
      NameRow.new(id:, disposition:, representative:, pass:, leaf:, rank:)
    end
  end

  def sets(captures, comparator)
    directory = File.join(captures, "e3-#{comparator.name}")
    small = ->(prefix) { File.join(captures, "e3-small-names/#{prefix}-#{comparator.name}.tsv") }
    {
      "alone" => [File.join(directory, "alone.tsv"), ->(id) { scalars(id) }],
      "between" => [File.join(directory, "between.tsv"), ->(id) { between(id) }],
      "pairs" => [File.join(directory, "pairs.tsv"), ->(id) { [LOWER_A, *scalars(id)] }],
      "hangul" => [File.join(directory, "hangul.tsv"), ->(id) { scalars(id) }],
      "surrogates" => [File.join(directory, "surrogates.tsv"), ->(id) { [:escaped, id.to_i(16)] }],
      "class0" => [small.call("class0"), ->(id) { scalars(id) }],
      # Runs of marks and marks after a precomposed starter: the vectors of the
      # decomposition check that the pairs set does not reach.
      "marks" => [small.call("marks"), ->(id) { scalars(id) }]
    }
  end

  def scalars(id) = id.split("+").map { _1.to_i(16) }

  # The seed row's id is the name itself; every other row is `aXb`.
  def between(id) = id == "ab" ? [LOWER_A, LOWER_B] : [LOWER_A, *scalars(id), LOWER_B]

  def check_sets(captures, tables, comparator)
    sets(captures, comparator).sum(NO_RESULT) do |set, (path, spell)|
      check_set(set:, rows: name_rows(path), spell:, tables:, comparator:)
    end
  end

  def check_set(set:, rows:, spell:, tables:, comparator:)
    label = "#{comparator.name} #{set}"
    unstored, stored = rows.partition { _1.leaf == "-" }
    [
      check_unstored(set, label, unstored),
      check_stored(label, stored, spell, tables),
      check_collisions(label, stored, comparator),
      check_order(label, stored, comparator)
    ].sum(NO_RESULT)
  end

  def check_unstored(set, label, rows)
    expected = UNSTORED.fetch(set, [])
    wrong = rows.map(&:id) == expected ? [] : ["#{label}: rows without a leaf are #{rows.map(&:id)}"]
    Result.new(counts: { "#{label} rows without a leaf" => rows.size }, violations: wrong)
  end

  # Decomposing and sorting the spelled name must give the units of its leaf.
  def check_stored(label, rows, spell, tables)
    wrong = rows.reject { stored_units(spell.call(_1.id), tables) == units(_1.leaf) }
    Result.new(
      counts: rows.map { "#{label} leaf #{kind(spell.call(_1.id))}" }.tally,
      violations: wrong.first(20).map { "#{label}: #{_1.id} stores #{_1.leaf}" } +
        (wrong.size > 20 ? ["#{label}: #{wrong.size} rows do not match in all"] : [])
    )
  end

  def units(dotted) = dotted.split(".").map { _1.to_i(16) }

  def kind(spelled)
    case spelled
    in [:escaped, *] then "escaped"
    in _ if spelled.any? { ESCAPED.include?(_1) } then "escaped"
    in _ if spelled.any? { SYSTEM_MAPPED.key?(_1) } then "system-mapped"
    in _ if spelled.any? { _1 > 0xFFFF } then "supplementary"
    in _ if spelled.any? { HANGUL.cover?(_1) } then "hangul"
    else "table"
    end
  end

  def stored_units(spelled, tables)
    return percent_escape(spelled.last) if spelled.first == :escaped

    class_sort(spelled.flat_map { spelling(_1, tables) }, tables.classes)
  end

  def spelling(scalar, tables)
    case scalar
    when *SYSTEM_MAPPED.keys then SYSTEM_MAPPED.fetch(scalar)
    when *ESCAPED then percent_escape(scalar)
    when HANGUL then hangul(scalar)
    when UNITS then tables.decompositions.fetch(scalar, [scalar])
    else surrogate_pair(scalar)
    end
  end

  def surrogate_pair(scalar)
    offset = scalar - 0x10000
    [HIGH_SURROGATES.first + (offset >> 10), LOW_SURROGATES.first + (offset & 0x3FF)]
  end

  # The system stores a refused value as the ASCII escape of its UTF-8 bytes. A
  # surrogate has no UTF-8, so pack's lenient U gives its three-byte encoding.
  def percent_escape(scalar) =
    [scalar].pack("U").bytes.map { format("%%%02X", _1) }.join.unpack("C*")

  def hangul(unit)
    index = unit - HANGUL.first
    trailing = index % 28
    [
      0x1100 + (index / 588),
      0x1161 + ((index % 588) / 28),
      *(trailing.zero? ? [] : [0x11A7 + trailing])
    ]
  end

  # Stable sort of each run of nonzero classes; class 0 units stay where they are.
  def class_sort(units, classes)
    units
      .slice_when { |a, b| classes[a].zero? || classes[b].zero? }
      .flat_map { |run| run.each_with_index.sort_by { |unit, at| [classes[unit], at] }.map(&:first) }
  end

  # Comparing these bytes is the volume's catalog comparison: unsigned units,
  # shorter prefix first, skipping what folds to zero on a folded volume.
  def catalog_key(leaf, comparator)
    mapped = units(leaf)
    return mapped.pack("n*") unless comparator.folded

    mapped.map { comparator.fold[_1] }.reject(&:zero?).pack("n*")
  end

  # A name that collided has the key of the name it collided with.
  def check_collisions(label, rows, comparator)
    leaves = rows.select { _1.representative == "self" }.to_h { [_1.id, _1.leaf] }
    colliding = rows.reject { _1.representative == "self" }
    wrong = colliding.reject do |row|
      catalog_key(row.leaf, comparator) == catalog_key(leaves.fetch(row.representative), comparator)
    end
    Result.new(
      counts: { "#{label} collisions" => colliding.size },
      violations: wrong.first(20).map { "#{label}: #{_1.id} collided with #{_1.representative}, its key differs" }
    )
  end

  # Each directory's names, sorted by the comparator, must fall in leaf rank
  # order, and no two may compare equal.
  def check_order(label, rows, comparator)
    directories = rows.group_by(&:pass).to_h { |pass, members| ["#{label}.#{pass}", order_violations(members, comparator)] }
    Result.new(
      counts: {
        "#{label} directories" => directories.size,
        "#{label} directories in catalog order" => directories.count { |_, wrong| wrong.empty? },
        "#{label} names sorted" => rows.size
      },
      violations: directories.flat_map { |directory, wrong| wrong.first(20).map { "#{directory}: #{_1}" } }
    )
  end

  def order_violations(rows, comparator)
    keyed = rows.map { [catalog_key(_1.leaf, comparator), _1] }.sort_by(&:first)
    equal = keyed.each_cons(2).select { |(a, _), (b, _)| a == b }
      .map { |_, (_, row)| "#{row.id} compares equal to a sibling" }
    misplaced = keyed.each_with_index.reject { |(_, row), at| row.rank.to_i == at }
      .map { |(_, row), at| "#{row.id} has rank #{row.rank}, the tables say #{at}" }
    equal + misplaced
  end

  # The NetBSD witness ------------------------------------------------------

  # Interpret libhfs.c's table-building function, a closed set of statement
  # shapes, to obtain its table. Any other statement stops the comparison: the
  # file is pinned, read as data only, and no text of it enters the tree. Its
  # table is 11 pages: page 0 maps a high byte to a page offset, 0 for identity.
  def netbsd_fold(captures)
    lines = File.readlines(File.join(captures, "netbsd-libhfs/libhfs.c"), chomp: true)
    start = lines.index("hfslib_create_casefolding_table(void)") + 1
    body = lines.drop(start).take_while { _1 != "}" }
    table = Array.new(5632 / 2, 0)
    page = nil
    loop_range = nil
    zeroed = body.join(" ")[/zi\[15\] = \{([^}]*)\}/, 1].scan(/\d+/).map(&:to_i)
    body.each do |line|
      case line.strip
      when /\As = (\d+) \* 256;\z/ then page = Regexp.last_match(1).to_i * 256
      when /\Afor \(i = (\d+); i < (\d+); i(\+\+| \+= 2)\)\z/
        step = Regexp.last_match(3) == "++" ? 1 : 2
        loop_range = (Regexp.last_match(1).to_i...Regexp.last_match(2).to_i).step(step)
      when /\At\[s\+i\] = i(?: \+ (0x\h+))?;\z/
        base = Regexp.last_match(1).to_s.to_i(16)
        loop_range.each { table[page + _1] = _1 + base }
      when /\At\[s\+ *(\d+)\] = (0x\h+);\z/ then table[page + Regexp.last_match(1).to_i] = Regexp.last_match(2).to_i(16)
      when "t[s+zi[i]] = 0x0000;" then zeroed.each { table[page + _1] = 0 }
      when "memset(t, 0x00, 512);", "", "{", "}", /\A\/?\*/, /\A(hfs_callback_args|unichar_t\*|uint16_t)\s/,
           /\A(uint8_t zi\[15\]|[\d, ]+[,}]|hfslib_init_cbargs|t = hfs_gcft|hfs_gcft = |return 0;|if \(hfs_gcft|HFS_LIBERR|error:|return 1;)/
        next
      else abort("libhfs.c: an unexpected statement: #{line}")
      end
    end
    UNITS.map do |unit|
      position = table[unit >> 8]
      position.zero? ? unit : table[position + (unit & 0xFF)]
    end
  end

  # The table equals NetBSD's on every unit: observable rows by the observation,
  # unobservable ones by the rule (lowercase on the folded pages unless the unit
  # decomposes), so any difference fails.
  def witness(captures, tables, rows)
    theirs = netbsd_fold(captures)
    status = rows.to_h { [_1.unit, _1] }
    differences = UNITS.reject { tables.fold[_1] == theirs[_1] }
    puts "NetBSD witness differences (unit, ours, NetBSD, status, observed fold):"
    differences.each do |unit|
      puts format("  %04X %04X %04X %-12s %s", unit, tables.fold[unit], theirs[unit], status[unit]&.status, status[unit]&.fold)
    end
    observable = differences.reject { status[_1]&.status == "unobservable" }
    Result.new(
      counts: {
        "witness units compared" => theirs.size,
        "witness differences" => differences.size,
        "witness differences on observable rows" => observable.size,
        "witness unobservable rows equal" => rows.count { _1.status == "unobservable" && tables.fold[_1.unit] == theirs[_1.unit] },
        "witness violations of the observation" => fold_violations(rows, theirs).size
      },
      violations: differences.map { format("witness: U+%04X folds to %04X, NetBSD to %04X", _1, tables.fold[_1], theirs[_1]) }
    )
  end

  # The digests docs/hfsplus-research.md pins, over serializations any
  # implementation can compute: the folds as big-endian units, the classes as
  # bytes, and each decomposition as its unit, its count and its units.
  def print_digests(tables)
    fold = tables.fold.pack("n*")
    classes = tables.classes.pack("C*")
    decompositions = tables.decompositions.sort.flat_map { |unit, units| [unit, units.size, *units] }.pack("n*")
    { "fold" => fold, "classes" => classes, "decompositions" => decompositions }.each do |name, bytes|
      puts format("SHA-256 %-15s %s", name, Digest::SHA256.hexdigest(bytes))
    end
  end
end

abort("usage: check-unicode-tables.rb [CAPTURES] [HEADER]") if ARGV.size > 2
CheckUnicodeTables.run(
  captures: ARGV[0] || CheckUnicodeTables::DEFAULT_CAPTURES,
  header: ARGV[1] || CheckUnicodeTables::DEFAULT_HEADER
)
