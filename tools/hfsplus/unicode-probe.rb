#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Observe how the native macOS HFS Plus driver stores, compares and sorts
# every Unicode value, on a case-insensitive HFS Plus volume and a
# case-sensitive HFSX volume, and write what it saw as tables that the
# generated fold and decomposition tables are compared against row for row.
#
# A run formats a disposable sparse volume per comparator, mounts it, and
# creates one empty file per probe name with O_CREAT | O_EXCL. Each name set
# gets a directory per pass: `SET.0` holds every name that could be created,
# and a name that collided with an earlier one is retried in `SET.1`, then
# `SET.2`, until nothing collides, so that the stored form of every member of
# a collision class is seen as well as the class. The volume is unmounted,
# checked with `fsck_hfs -n`, and its catalog leaves are read back: they hold
# the units actually stored and the order the driver sorted them in.
#
# Tables, in OUT_ROOT/e3-VARIANT/, tab separated, one `#` header line:
#
#   SET.tsv (alone, between, pairs, hangul, surrogates)
#     id              hex scalar value, `+` between the values of a sequence
#     scope           bmp, supp (Unicode 3.2 assigns it), coverage (unassigned,
#                     kept to reach every surrogate unit), pair, hangul,
#                     hangul-jamo, seed or surrogate
#     disposition     created, EEXIST or another errno name, in pass 0;
#                     path-terminator for NUL, which no syscall can carry
#     representative  self, or what it collided with in pass 0: an id, `dir`
#                     for its own directory or `parent` for the one above
#     pass            the pass whose directory holds the name, or `-`
#     readdir_units   the UTF-16 units of the readdir name, `.` separated
#     leaf_units      the UTF-16 units of the catalog key
#     rank            0-based position among the directory's catalog children
#
#   fold.tsv          one row per UTF-16 unit, see `fold_rows`
#   differences.tsv   every disagreement with the Unicode 3.2.0 prediction
#   residual.tsv      what the prediction plus the observed rules leaves unexplained
#   volume.json       comparator, tree shape, fsck verdicts, per-set directories
#   summary.json      counts per set and per difference kind
#   commands.txt      the commands run
#   SHA256SUMS        the digest of each file above
#
# Usage: unicode-probe.rb [--variant hfsplus|hfsx] [--analyze-only] [--sample N]
require "digest"
require "fileutils"
require "json"
require "optparse"
require_relative "capture"

module HfsPlusUnicodeProbe
  class Error < StandardError; end

  UNICODE_DATA_SHA256 = "5e444028b6e76d96f9dc509609c5e3222bf609056f35e5fcde7e6fb8a58cd446"
  DEFAULT_DATA = File.join(Dir.home, "hfsplus-captures/unicode-3.2.0/UnicodeData-3.2.0.txt")
  DEFAULT_ROOT = File.join(Dir.home, "hfsplus-captures")
  DEFAULT_WORK = File.join(Dir.home, ".cache/fsimg-hfs/e3")
  VOLUME_BYTES = 8 << 30
  BLOCK_BYTES = 4096
  CATALOG_CLUMP_BLOCKS = 256 * 1024 # 1 GiB, one extent: fields.rb reads only the first eight.
  VARIANTS = { "hfsplus" => [], "hfsx" => ["-s"] }.freeze
  SURROGATES = (0xD800..0xDFFF)
  HANGUL = (0xAC00..0xD7A3)
  COLUMNS = %w[id scope disposition representative pass readdir_units leaf_units rank].freeze
  # POSIX spellings the system stores as another unit: the Mac path separator
  # and the visible stand-in for U+0000.
  SYSTEM_MAPPED = { 0x3A => 0x2F, 0x2400 => 0x0000 }.freeze
  # Valid scalar values the system stores as `%XX` escapes of their UTF-8.
  NONCHARACTERS_ESCAPED = (0xFFFE..0xFFFF)
  # The only probe names `fsck_hfs -n` calls illegal; removing them lets its
  # verdict and its key order check speak for every other name.
  FSCK_ILLEGAL_NAMES = ["pairs.0/a\u0306\u0307", "pairs.0/a\u0308\u030D"].freeze
  # Representatives that are not another probe name.
  NOT_A_NAME = %w[self dir parent - ?].freeze
  ASCII_A = 0x61
  ASCII_B = 0x62

  Config = Data.define(:unicode_data, :root, :work, :variants, :sample, :analyze_only, :keep_image)

  # One probe: the bytes given to the system, and the scalar values they spell.
  Item = Data.define(:id, :scope, :cps, :name)
  # What the driver did with one probe; filled in pass by pass.
  Outcome = Struct.new(:disposition, :representative, :pass, :ino)
  # One catalog leaf record of a file or folder.
  Leaf = Data.define(:parent, :units, :rank)
  # One row of a SET.tsv file, as text.
  Row = Data.define(*COLUMNS.map(&:to_sym))

  module_function

  # ---- Unicode 3.2.0 data ----

  # The fields of the pinned file a prediction needs. `assigned` includes the
  # values of its First/Last ranges, which carry no properties of their own.
  class UnicodeTables
    attr_reader :assigned, :ccc

    def initialize(path)
      bytes = File.binread(path)
      digest = Digest::SHA256.hexdigest(bytes)
      raise Error, "#{path} has digest #{digest}, not the pinned #{UNICODE_DATA_SHA256}" unless digest == UNICODE_DATA_SHA256

      @assigned = []
      @ccc = {}
      @canonical = {}
      @lower = {}
      range_first = nil
      bytes.force_encoding("US-ASCII").each_line(chomp: true) do |line|
        code, name, _, ccc, _, decomposition, _, _, _, _, _, _, _, lower = line.split(";", -1)
        cp = code.to_i(16)
        if name.end_with?(", First>")
          range_first = cp
        elsif name.end_with?(", Last>")
          @assigned.concat((range_first..cp).to_a)
        else
          @assigned << cp
          @ccc[cp] = ccc.to_i unless ccc == "0"
          @canonical[cp] = decomposition.split.map { _1.to_i(16) } unless decomposition.empty? || decomposition.start_with?("<")
          @lower[cp] = lower.to_i(16) unless lower.empty?
        end
      end
    end

    def marks = @ccc.keys.sort
    def class_of(cp) = @ccc.fetch(cp, 0)
    def canonical(cp) = @canonical[cp]
    def lower(cp) = @lower[cp]
  end

  def hex(cp) = format("%04X", cp)
  def utf16(cps) = cps.flat_map { _1 > 0xFFFF ? [0xD800 + ((_1 - 0x10000) >> 10), 0xDC00 + ((_1 - 0x10000) & 0x3FF)] : [_1] }
  def units_text(units) = units.empty? ? "-" : units.map { hex(_1) }.join(".")
  def parse_units(text) = text == "-" ? [] : text.split(".").map { _1.to_i(16) }
  def hangul_jamo(cp)
    index = cp - HANGUL.first
    trailing = index % 28
    [0x1100 + index / 588, 0x1161 + (index % 588) / 28, *(trailing.zero? ? [] : [0x11A7 + trailing])]
  end

  # ---- probe names ----

  def item(cps:, scope:, id: cps.map { hex(_1) }.join("+")) = Item.new(id: id, scope: scope, cps: cps, name: cps.pack("U*").b)

  # Every value of the probe scope with its scope label: all BMP scalar values,
  # the supplementary values the data assigns, and one more value for each high
  # and each low surrogate unit no assigned value reaches.
  def scoped_values(data)
    bmp = (0..0xFFFF).reject { SURROGATES.cover?(_1) }.map { [_1, "bmp"] }
    supp = data.assigned.select { _1 > 0xFFFF }
    assigned = supp.to_set
    every_unit = (0...1024).flat_map { [0x10000 + (_1 << 10), 0x10000 + _1] }.uniq.sort
    bmp + (supp.map { [_1, "supp"] } + every_unit.reject { assigned.include?(_1) }.map { [_1, "coverage"] }).sort
  end

  # The sets, in creation order. Ascending values make a class's representative
  # its smallest member. The seed `ab` is what an ignorable `aXb` collides with.
  def probe_sets(data, sample)
    values = scoped_values(data)
    marks = data.marks
    sets = {
      "alone" => values.map { |cp, scope| item(cps: [cp], scope: scope) },
      "between" => [item(id: "ab", cps: [ASCII_A, ASCII_B], scope: "seed")] +
        values.map { |cp, scope| item(id: hex(cp), cps: [ASCII_A, cp, ASCII_B], scope: scope) },
      "pairs" => marks.product(marks).map { |m1, m2| item(id: "#{hex(m1)}+#{hex(m2)}", cps: [ASCII_A, m1, m2], scope: "pair") },
      "hangul" => HANGUL.flat_map { [item(cps: [_1], scope: "hangul"), item(cps: hangul_jamo(_1), scope: "hangul-jamo")] },
      "surrogates" => SURROGATES.map { item(cps: [_1], scope: "surrogate") }
    }
    sample == 1 ? sets : sets.transform_values { |items| items.each_slice(sample).map(&:first) }
  end

  # ---- the mounted volume ----

  # Create one empty file, returning its inode or the name of the failure. A
  # NUL ends a C path, so a name holding one cannot reach the system at all.
  def create(path)
    io = IO.new(IO.sysopen(path, File::WRONLY | File::CREAT | File::EXCL, 0o644))
    ino = io.stat.ino
    io.close
    ino
  rescue SystemCallError => e
    e.class.name.split("::").last
  rescue ArgumentError
    "path-terminator"
  end

  # Which existing thing an unsuccessful name resolves to: an earlier name of
  # this directory by inode, the directory itself, or the one above.
  def collided_with(path, created, own, parent)
    ino = File.lstat(path).ino
    created[ino] || { own => "dir", parent => "parent" }.fetch(ino, "?")
  rescue SystemCallError, ArgumentError
    "-"
  end

  # One pass in the current directory; returns the items to retry in the next,
  # those that collided with a name this pass created.
  def attempt_pass(pending, pass, outcomes)
    own = File.stat(".").ino
    parent = File.stat("..").ino
    created = {}
    pending.select do |probe|
      outcome = outcomes[probe.id] ||= Outcome.new
      path = "./".b + probe.name
      result = create(path)
      if result.is_a?(Integer)
        created[result] = probe.id
        outcome.pass = pass
        outcome.ino = result
        outcome.disposition ||= "created"
        outcome.representative ||= "self"
        next false
      end
      collision = collided_with(path, created, own, parent)
      outcome.disposition ||= result
      outcome.representative ||= collision
      result == "EEXIST" && !NOT_A_NAME.include?(collision)
    end
  end

  def probe_set(set, items)
    outcomes = {}
    directories = {}
    pending = items
    pass = 0
    until pending.empty?
      directory = "#{set}.#{pass}"
      Dir.mkdir(directory)
      directories[directory] = File.stat(directory).ino
      pending = Dir.chdir(directory) { attempt_pass(pending, pass, outcomes) }
      pass += 1
    end
    [outcomes, directories]
  end

  # The names of a directory as the system returns them, in its order, and those
  # the system cannot then find again by the name it returned. Ruby's Dir class
  # composes the names it reads from an HFS volume, so find(1) reads them here.
  def read_listing(directory)
    output, status = Open3.capture2("find", ".", "-mindepth", "1", "-maxdepth", "1", "-print0", chdir: directory, binmode: true)
    raise Error, "find failed in #{directory}" unless status.success?

    names = output.split("\x00".b).map { _1.delete_prefix("./") }
    lost = Dir.chdir(directory) { names.reject { found?("./".b + _1) } }
    [names.map { name_units(_1) }, lost.map { name_units(_1) }]
  end

  def found?(path)
    File.lstat(path)
    true
  rescue SystemCallError
    false
  end

  def name_units(name) = utf16(name.dup.force_encoding(Encoding::UTF_8).unpack("U*"))

  def observe_volume(mount, sets)
    Dir.chdir(mount) do
      File.write(".metadata_never_index", "")
      sets.to_h do |set, items|
        started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        outcomes, directories = probe_set(set, items)
        listings = directories.to_h { |directory, ino| [ino, read_listing(directory)] }
        seconds = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started).round(1)
        warn "#{set}: #{items.size} names in #{seconds} s"
        [set, { outcomes: outcomes, directories: directories, listings: listings, seconds: seconds }]
      end
    end
  end

  # ---- the catalog leaves ----

  # Every file and folder record by node ID, with its rank among its parent's
  # children in leaf order, read through the fields.rb primitives.
  def read_leaves(image)
    File.open(image, "rb") do |io|
      unsized = HfsPlusFields::Volume.new(image: HfsPlusFields::Image.new(io), base: 0, block_size: BLOCK_BYTES)
      header = HfsPlusFields.read_header(unsized, 1024)
      volume = unsized.with(block_size: header[:block_size])
      fork = header[:forks].fetch(:catalog)
      tree = HfsPlusFields.tree_header_report(volume, fork)
      leaves = walk_leaf_chain(volume, fork, tree)
      extents = fork.extents.count { _1.count.positive? }
      info = { signature: header[:signature], key_compare_type: format("0x%02X", tree[:key_compare_type]), node_size: tree[:node_size],
               depth: tree[:depth], leaf_records: tree[:leaf_records], total_nodes: tree[:total_nodes], free_nodes: tree[:free_nodes],
               catalog_extents: extents, catalog_bytes: fork.logical_size, file_count: header[:file_count], folder_count: header[:folder_count] }
      [leaves, info]
    end
  end

  def walk_leaf_chain(volume, fork, tree)
    leaves = {}
    ranks = Hash.new(0)
    seen = Set.new
    node = tree[:first_leaf]
    until node.zero?
      raise Error, "leaf chain loops at #{node}" unless seen.add?(node)

      raw = HfsPlusFields.read_node(volume, fork, node, tree[:node_size])
      desc = HfsPlusFields.descriptor(raw)
      raise Error, "node #{node} on the leaf chain has kind #{desc.kind}" unless desc.kind == -1

      HfsPlusFields.record_slices(raw, desc.num_records).each do |slice|
        parent, _, data_at = HfsPlusFields.catalog_key(slice)
        type, _, _, cnid = slice.byteslice(data_at, 12).unpack("nnNN")
        next unless [1, 2].include?(type)

        units = slice.byteslice(8, data_at - 8).unpack("n*").first(slice.byteslice(6, 2).unpack1("n"))
        leaves[cnid] = Leaf.new(parent: parent, units: units, rank: ranks[parent])
        ranks[parent] += 1
      end
      node = desc.flink
    end
    leaves
  end

  # ---- writing the observation ----

  def out_dir(cfg, variant) = File.join(cfg.root, "e3-#{variant}")

  def set_rows(items, observed, leaves)
    items.map do |probe|
      outcome = observed[:outcomes].fetch(probe.id)
      leaf = leaves[outcome.ino]
      listed = leaf && observed[:listings].dig(leaf.parent, 0, leaf.rank)
      [probe.id, probe.scope, outcome.disposition, outcome.representative, outcome.pass || "-",
       listed ? units_text(listed) : "-", leaf ? units_text(leaf.units) : "-", leaf ? leaf.rank : "-"]
    end
  end

  def write_tsv(path, columns, rows)
    File.open(path, "w") do |file|
      file.write("# #{columns.join("\t")}\n")
      rows.each { file.write("#{_1.join("\t")}\n") }
    end
  end

  # A directory whose readdir count differs from its catalog children, which
  # would make the join of listing position and leaf rank meaningless.
  def listing_count_mismatches(observed, leaves)
    counts = leaves.values.map(&:parent).tally
    observed[:listings].reject { |parent, (names, _)| counts[parent] == names.size }.keys
  end

  def fsck_verdict(attachments, log, image)
    attachments.with(image) do |device|
      output, ok = log.run("fsck_hfs", "-d", "-n", device.sub("/dev/disk", "/dev/rdisk"))
      { ok: ok, illegal_names: output.lines.grep(/illegal name is|replacement name is/).map(&:strip), last_line: output.lines.last.strip }
    end
  end

  def run_volume(cfg, variant, sets)
    log = HfsPlusCapture::Log.new
    attachments = HfsPlusCapture::Attachments.new(log)
    image = File.join(cfg.work, "#{variant}.img")
    mount = File.join(cfg.work, "#{variant}.mnt")
    out = out_dir(cfg, variant)
    FileUtils.mkdir_p([cfg.work, mount, out])
    raise Error, "cannot create #{image}" unless log.run("mkfile", "-n", VOLUME_BYTES.to_s, image).last

    attachments.with(image) do |device|
      raw = device.sub("/dev/disk", "/dev/rdisk")
      format_args = ["newfs_hfs", *VARIANTS.fetch(variant), "-v", "E3", "-b", BLOCK_BYTES.to_s, "-c", "c=#{CATALOG_CLUMP_BLOCKS}", raw]
      raise Error, "newfs_hfs failed on #{raw}" unless log.run(*format_args).last
    end
    observed = attachments.with(image, mountpoint: mount) { observe_volume(mount, sets) }
    verdict = fsck_verdict(attachments, log, image)
    leaves, info = read_leaves(image)
    sets.each { |set, items| write_tsv(File.join(out, "#{set}.tsv"), COLUMNS, set_rows(items, observed.fetch(set), leaves)) }
    attachments.with(image, mountpoint: mount) { FileUtils.rm_f(FSCK_ILLEGAL_NAMES.map { File.join(mount, _1) }) }
    File.write(File.join(out, "volume.json"), JSON.pretty_generate(
      variant: variant, sample: cfg.sample, image_allocated_bytes: File.stat(image).blocks * 512, tree: info,
      fsck: verdict, fsck_without_illegal_names: fsck_verdict(attachments, log, image),
      sets: observed.transform_values do |o|
        { directories: o[:directories].keys, seconds: o[:seconds], listing_count_mismatches: listing_count_mismatches(o, leaves),
          readdir_names_not_found: o[:listings].values.flat_map(&:last).map { units_text(_1) } }
      end
    ))
  ensure
    attachments&.release
    File.write(File.join(out, "commands.txt"), log.to_s) if log && out && File.directory?(out)
    FileUtils.rm_f(image) if image && !cfg.keep_image
    FileUtils.rm_rf(mount) if mount
  end

  # ---- the Unicode 3.2.0 prediction ----

  # What specs/design-hfsplus.md says the driver does, computed from the pinned
  # data: the disagreements are the candidates for the exceptions file.
  class Prediction
    UNCHANGED_DECOMPOSITION = [0x2000..0x2FFF, 0xF900..0xFAFF, 0x2F800..0x2FAFF].freeze
    FOLDED_PAGES = [0x00, 0x01, 0x03, 0x04, 0x05, 0x10, 0x21, 0xFF].freeze
    IGNORABLE = [0x200C..0x200F, 0x202A..0x202E, 0x206A..0x206F, 0xFEFF..0xFEFF].freeze
    GEORGIAN_CAPITALS = (0x10A0..0x10C5)
    NUL_SORT_KEY = 0x10000 # After every unit of a folded key.

    def initialize(data, folded:)
      @data = data
      @folded = folded
    end

    def stored(cps) = HfsPlusUnicodeProbe.utf16(canonical_order(cps.flat_map { decompose(_1) }))
    def sort_key(units) = units.filter_map { fold(_1) }

    # The design's rule that an all-ignorable name folds onto its parent's thread.
    def meets_thread?(key) = key.empty?

    # Nil when the unit is ignorable.
    def fold(unit)
      return unit unless @folded
      return NUL_SORT_KEY if unit.zero?
      return nil if IGNORABLE.any? { _1.cover?(unit) }
      return unit - GEORGIAN_CAPITALS.first + 0x10D0 if GEORGIAN_CAPITALS.cover?(unit)

      FOLDED_PAGES.include?(unit >> 8) ? @data.lower(unit) || unit : unit
    end

    private

    def combining_class(cp) = @data.class_of(cp)

    def decompose(cp)
      return HfsPlusUnicodeProbe.hangul_jamo(cp) if HANGUL.cover?(cp)
      return [cp] if UNCHANGED_DECOMPOSITION.any? { _1.cover?(cp) }

      mapped = @data.canonical(cp)
      mapped ? mapped.flat_map { decompose(_1) } : [cp]
    end

    # Stable sort of each run of nonzero combining classes.
    def canonical_order(cps)
      cps.chunk_while { combining_class(_1).positive? == combining_class(_2).positive? }.flat_map do |run|
        next run if combining_class(run.first).zero?

        run.each_with_index.sort_by { |cp, index| [combining_class(cp), index] }.map(&:first)
      end
    end
  end

  # The prediction plus every rule the observation added: the system's two
  # substitutions, percent escapes for what it will not store, supplementary
  # values taken as opaque, and the fold rows that differ. What this still gets
  # wrong is unexplained, and is written to residual.tsv.
  class Explained < Prediction
    def initialize(data, folded:, fold_exceptions:)
      super(data, folded: folded)
      @fold_exceptions = fold_exceptions
    end

    def fold(unit) = @fold_exceptions.fetch(unit) { super }

    # The driver compares an empty key by length, so only a thread equals a thread.
    def meets_thread?(_key) = false

    private

    def combining_class(cp) = cp > 0xFFFF ? 0 : super

    def decompose(cp)
      return [SYSTEM_MAPPED.fetch(cp)] if SYSTEM_MAPPED.key?(cp)
      return [cp].pack("U").bytes.flat_map { format("%%%02X", _1).bytes } if SURROGATES.cover?(cp) || NONCHARACTERS_ESCAPED.cover?(cp)
      return [cp] if cp > 0xFFFF

      super
    end
  end

  # ---- comparing a stored observation ----

  def read_rows(path)
    File.foreach(path, chomp: true).drop(1).map { Row.new(*_1.split("\t", -1)) }
  end

  # Replay pass 0 of a set under a prediction: what each name should store and
  # which earlier name it should collide with.
  def differences_of_set(set, items, rows, prediction)
    by_id = rows.to_h { [_1.id, _1] }
    first_with_key = {}
    items.flat_map do |probe|
      row = by_id.fetch(probe.id)
      stored = prediction.stored(probe.cps)
      key = prediction.sort_key(stored)
      expected = prediction.meets_thread?(key) ? "dir" : first_with_key.fetch(key, "self")
      first_with_key[key] ||= probe.id if row.disposition == "created"
      next [[set, row.id, "refused", "created", "#{row.disposition}:#{row.representative}"]] unless %w[created EEXIST].include?(row.disposition)

      suffix = probe.cps.any? { _1 > 0xFFFF } ? "-supplementary" : ""
      [([set, row.id, "units#{suffix}", units_text(stored), row.leaf_units] if row.leaf_units != "-" && row.leaf_units != units_text(stored)),
       ([set, row.id, "representative#{suffix}", expected, row.representative] if expected != row.representative)].compact
    end
  end

  # Adjacent names of one pass directory whose predicted keys are out of order.
  def order_differences(set, rows, prediction)
    rows.reject { _1.rank == "-" }.group_by(&:pass).flat_map do |pass, group|
      group.sort_by { _1.rank.to_i }.each_cons(2).filter_map do |a, b|
        ka = prediction.sort_key(parse_units(a.leaf_units))
        kb = prediction.sort_key(parse_units(b.leaf_units))
        [set, "#{set}.#{pass}", "order", "#{b.id} before #{a.id}", "#{a.id} then #{b.id}"] if (ka <=> kb) != -1
      end
    end
  end

  def middle_unit(row)
    units = parse_units(row.leaf_units)
    units[1] if units.size == 3 && units.first == ASCII_A && units.last == ASCII_B
  end

  # One row per UTF-16 unit, 0000 to FFFF, in the terms of a 65536-entry table,
  # read from the `between` set, whose names reach `.` and `/`:
  #
  #   unit    the table index
  #   status  determined: `fold` is the only value that catalog order and the
  #                       collision class allow
  #           ambiguous:  `fold` lists the class members order leaves possible;
  #                       each gives the same comparison over every stored name
  #           ignorable:  `aXb` collides with `ab`; `fold` is 0000
  #           position:   U+0000, whose fold is known only by the unit that
  #                       sorts just before it (`after:UNIT`), or `first`
  #           unobservable: no name stores this unit; `fold` says why: decomposes,
  #                       escaped, system-mapped, or surrogate for the units that
  #                       appear only inside pairs
  def fold_rows(between)
    index = between.to_h { [_1.id, _1] }
    ignorable = between.select { _1.representative == "ab" }.to_set { _1.id.to_i(16) }
    classes = between.select { middle_unit(_1) }.group_by { _1.representative == "self" ? _1.id : _1.representative }
    chain = classes.filter_map do |rep, members|
      head = index[rep]
      [head.rank.to_i, members.map { middle_unit(_1) }.uniq.sort] if head && head.pass == "0" && middle_unit(head)
    end.sort_by(&:first).map(&:last)
    nul_at = chain.index([0])
    resolved = resolve_chain(chain.reject { _1 == [0] })
    (0..0xFFFF).map do |unit|
      if unit.zero? && nul_at then [hex(unit), "position", nul_at.zero? ? "first" : "after:#{hex(chain[nul_at - 1].last)}"]
      elsif ignorable.include?(unit) then [hex(unit), "ignorable", "0000"]
      elsif resolved.key?(unit) then [hex(unit), *resolved[unit]]
      else [hex(unit), "unobservable", unobservable_reason(unit)]
      end
    end
  end

  # Each class's fold is one of its members, and the folds ascend along the
  # chain: bound every class from both sides, and it is determined when the
  # bounds meet.
  def resolve_chain(chain)
    lows = chain.each_with_object([]) { |members, acc| acc << members.find { _1 > (acc.last || -1) } }
    highs = chain.reverse.each_with_object([]) { |members, acc| acc << members.reverse.find { _1 < (acc.last || 0x10000) } }.reverse
    chain.zip(lows, highs).each_with_object({}) do |(members, low, high), resolved|
      possible = members.select { low && high && _1.between?(low, high) }
      answer = possible.size == 1 ? ["determined", hex(possible.first)] : ["ambiguous", possible.map { hex(_1) }.join("|")]
      members.each { resolved[_1] = answer }
    end
  end

  def unobservable_reason(unit)
    return "surrogate" if SURROGATES.cover?(unit)
    return "escaped" if NONCHARACTERS_ESCAPED.cover?(unit)

    SYSTEM_MAPPED.key?(unit) ? "system-mapped" : "decomposes"
  end

  # Fold rows that disagree with a prediction, as [set, id, check, predicted, observed].
  def fold_differences(fold, prediction)
    fold.filter_map do |unit, status, value|
      predicted = hex(prediction.fold(unit.to_i(16)) || 0)
      agrees = case status
               when "determined", "ignorable" then predicted == value
               when "ambiguous" then value.split("|").include?(predicted)
               else true
               end
      ["fold", unit, "fold", predicted, value] unless agrees
    end
  end

  def all_differences(sets, rows, fold, prediction)
    sets.flat_map do |set, items|
      differences_of_set(set, items, rows.fetch(set), prediction) + order_differences(set, rows.fetch(set), prediction)
    end + fold_differences(fold, prediction)
  end

  def ranges_text(units)
    units.sort.slice_when { _2 != _1 + 1 }.map { _1.size == 1 ? hex(_1.first) : "#{hex(_1.first)}-#{hex(_1.last)}" }
  end

  def set_summary(rows)
    resolved_to = ->(row) { NOT_A_NAME.include?(row.representative) ? row.representative : "name" }
    {
      rows: rows.size,
      by_scope: rows.map(&:scope).tally,
      dispositions: rows.map { "#{_1.disposition}:#{resolved_to[_1]}" }.tally,
      collision_classes: rows.map(&:representative).uniq.count { !NOT_A_NAME.include?(_1) },
      deepest_pass: rows.map(&:pass).reject { _1 == "-" }.map(&:to_i).max,
      readdir_differs_from_leaf: rows.count { _1.readdir_units != _1.leaf_units }
    }
  end

  # Rewrite every derived table from the SET.tsv files of `out`.
  def analyze(cfg, variant, sets, data)
    out = out_dir(cfg, variant)
    folded = variant == "hfsplus"
    rows = sets.keys.to_h { [_1, read_rows(File.join(out, "#{_1}.tsv"))] }
    fold = fold_rows(rows.fetch("between"))
    write_tsv(File.join(out, "fold.tsv"), %w[unit status fold], fold)
    differences = all_differences(sets, rows, fold, Prediction.new(data, folded: folded))
    write_tsv(File.join(out, "differences.tsv"), %w[set id check predicted observed], differences)
    fold_exceptions = differences.select { _1[2] == "fold" && !_1[4].empty? }.to_h { [_1[1].to_i(16), _1[4].split("|").first.to_i(16)] }
    residual = all_differences(sets, rows, fold, Explained.new(data, folded: folded, fold_exceptions: fold_exceptions))
    write_tsv(File.join(out, "residual.tsv"), %w[set id check predicted observed], residual)
    File.write(File.join(out, "summary.json"), JSON.pretty_generate(
      variant: variant,
      sets: rows.transform_values { set_summary(_1) },
      ignorable: ranges_text(rows.fetch("between").select { _1.representative == "ab" }.map { _1.id.to_i(16) }),
      fold_status: fold.map { _1[1] }.tally,
      fold_changed: ranges_text(fold.select { |unit, status, value| status == "determined" && unit != value }.map { _1.first.to_i(16) }),
      fold_exceptions: ranges_text(fold_exceptions.keys),
      unobservable: fold.select { _1[1] == "unobservable" }.map(&:last).tally,
      differences: differences.map { "#{_1[0]}:#{_1[2]}" }.tally,
      residual: residual.map { "#{_1[0]}:#{_1[1]}:#{_1[2]}" }
    ))
    write_digests(out)
  end

  def write_digests(out)
    files = Dir.children(out).reject { _1 == "SHA256SUMS" }.sort
    File.write(File.join(out, "SHA256SUMS"), files.map { "#{Digest::SHA256.file(File.join(out, _1)).hexdigest}  #{_1}\n" }.join)
  end

  # ---- command line ----

  def parse(argv)
    options = { unicode_data: DEFAULT_DATA, root: DEFAULT_ROOT, work: DEFAULT_WORK, variants: VARIANTS.keys, sample: 1, analyze_only: false, keep_image: false }
    OptionParser.new do |parser|
      parser.banner = "usage: unicode-probe.rb [options]"
      parser.on("--unicode-data PATH", "the pinned UnicodeData-3.2.0.txt") { options[:unicode_data] = _1 }
      parser.on("--root DIR", "where e3-VARIANT/ is written") { options[:root] = _1 }
      parser.on("--work DIR", "where the disposable volumes live") { options[:work] = _1 }
      parser.on("--variant NAME", VARIANTS.keys, "only this comparator") { options[:variants] = [_1] }
      parser.on("--sample N", Integer, "keep every Nth probe, to measure") { options[:sample] = _1 }
      parser.on("--analyze-only", "rewrite the derived tables without a volume") { options[:analyze_only] = true }
      parser.on("--keep-image", "leave the image in the work directory") { options[:keep_image] = true }
    end.parse!(argv)
    Config.new(**options)
  end

  def main(argv)
    cfg = parse(argv)
    data = UnicodeTables.new(cfg.unicode_data)
    sets = probe_sets(data, cfg.sample)
    cfg.variants.each do |variant|
      run_volume(cfg, variant, sets) unless cfg.analyze_only
      analyze(cfg, variant, sets, data)
    end
    0
  rescue Error, HfsPlusFields::Error, HfsPlusCapture::Error, OptionParser::ParseError => e
    warn "unicode-probe: #{e.message}"
    1
  end
end

exit(HfsPlusUnicodeProbe.main(ARGV)) if $PROGRAM_NAME == __FILE__
