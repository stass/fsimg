#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Decode the structures of an HFS Plus or HFSX image for the research record:
# the boot area, the volume header and its alternate, each B-tree's header node
# and node map, the private folders, Finder information and the first catalog
# records. The decoder is written from the format design, not from lib/, and
# treats the image as untrusted: every read is bounds-checked (record decoders
# read only through `bytes_at` and `fields_at`), every walk is bounded, and a
# section that cannot be decoded is reported in "problems" while the other
# sections still print.
#
#   fields.rb [--json] [--records N] [--index] IMAGE
require "digest"
require "json"
require "optparse"
require "set"

module HfsPlusFields
  class Error < StandardError; end
  # A read outside the image or outside a fork's extents.
  class Truncated < Error; end
  # A structure whose contents contradict the format.
  class Malformed < Error; end

  EPOCH_1904 = 2_082_844_800
  MAX_NODES = 1 << 22
  MAX_MAP_NODES = 4096
  MAX_ZERO_CHECK = 4096
  ROOT_ID = 2
  NODE_DESCRIPTOR_BYTES = 14
  EMBED_SIGNATURE = 0x482B
  MAX_ROOT_LISTING = 200
  MAX_BITMAP_BYTES = 64 << 20
  MAX_FREE_BYTES = 256 << 20

  VOLUME_ATTRIBUTES = {
    7 => "hardware-lock", 8 => "unmounted", 9 => "spared-blocks",
    10 => "no-cache", 11 => "boot-inconsistent", 12 => "ids-reused",
    13 => "journaled", 14 => "inconsistent", 15 => "software-lock",
    29 => "expanded-times", 30 => "content-protection", 31 => "nodes-zeroed"
  }.freeze

  TREE_ATTRIBUTES = { 0 => "bad-close", 1 => "big-keys", 2 => "variable-index-keys" }.freeze
  NODE_KINDS = { -1 => "leaf", 0 => "index", 1 => "header", 2 => "map" }.freeze
  FORK_NAMES = %i[allocation extents catalog attributes startup].freeze

  Extent = Data.define(:start, :count) do
    def to_s = "#{start}+#{count}"
  end
  Fork = Data.define(:logical_size, :clump_size, :total_blocks, :extents) do
    def empty? = logical_size.zero? && total_blocks.zero? && extents.all? { _1.count.zero? }
  end
  NodeDescriptor = Data.define(:flink, :blink, :kind, :height, :num_records)

  # The image bytes, read only through a bounds check.
  class Image
    attr_reader :size

    def initialize(io)
      @io = io
      @size = io.size
    end

    def read(offset, length)
      unless offset >= 0 && length >= 0 && offset + length <= size
        raise Truncated, "read of #{length} bytes at #{offset} is outside the image of #{size}"
      end

      @io.seek(offset)
      data = @io.read(length) || +""
      raise Truncated, "short read at #{offset}" unless data.bytesize == length

      data.b
    end
  end

  # One volume inside an image: its byte offset (nonzero inside a wrapper) and
  # its allocation block size.
  Volume = Data.define(:image, :base, :block_size) do
    def read(offset, length) = image.read(base + offset, length)

    # Read `length` bytes of a fork from `offset`, through its first eight
    # extents; the overflow tree is not consulted.
    def read_fork(fork, offset, length)
      parts = fork_ranges(fork, offset, length)
      raise Truncated, "fork range #{offset}+#{length} is not in its first eight extents" unless parts.sum(&:last) == length

      parts.map { read(*_1) }.join
    end

    private

    # The byte ranges of the volume that hold [offset, offset + length) of the
    # fork. `firsts[i]` is the fork block where extent i starts: extents follow
    # each other in fork order, so extent i covers fork bytes
    # [first * block_size, (first + count) * block_size). Each range is the
    # overlap of the request with one extent, translated to its volume position;
    # an extent without overlap contributes nothing. The ranges are in fork
    # order and sum to `length` only if the first eight extents cover the request.
    def fork_ranges(fork, offset, length)
      firsts = fork.extents.map(&:count).each_with_object([0]) { |count, acc| acc << acc.last + count }
      fork.extents.zip(firsts).filter_map do |ext, first|
        lo = [offset, first * block_size].max
        hi = [offset + length, (first + ext.count) * block_size].min
        [ext.start * block_size + lo - first * block_size, hi - lo] if hi > lo
      end
    end
  end

  POPCOUNT = Array.new(256) { _1.digits(2).sum }.freeze
  # A byte that is not 0xFF, and a byte that is not 0x00.
  NOT_FULL = /[^\xFF]/n
  NOT_EMPTY = /[^\x00]/n

  # The first `count` bits of a bitmap, most significant bit of each byte
  # first. The bits of the last byte beyond `count` are cleared, so `bytes`
  # never holds a set bit outside the bitmap. Searches skip whole bytes in the
  # string rather than expanding it to one character per bit.
  Bits = Data.define(:bytes, :count) do
    def self.of(raw, count)
      count = [count, raw.bytesize * 8].min
      whole, extra = count.divmod(8)
      tail = extra.zero? ? "".b : [raw.getbyte(whole) & (0xff << (8 - extra))].pack("C")
      new(raw.byteslice(0, whole) + tail, count)
    end

    def set_count = HfsPlusFields.popcount(bytes)
    def set?(number) = number.between?(0, count - 1) && bytes.getbyte(number / 8)[7 - number % 8] == 1
    def first_clear = clear_numbers.first

    # The highest set bit: the last nonzero byte, then its lowest set bit.
    def last_set
      at = bytes.rindex(NOT_EMPTY) or return nil
      at * 8 + 7 - (bytes.getbyte(at) & -bytes.getbyte(at)).bit_length + 1
    end

    # The numbers of the clear bits, ascending, found by jumping to each byte
    # that is not all ones.
    def clear_numbers
      Enumerator.new do |out|
        at = 0
        while (at = bytes.index(NOT_FULL, at))
          (at * 8...at * 8 + 8).each { |number| out << number if number < count && !set?(number) }
          at += 1
        end
      end
    end
  end

  module_function

  # ---- bounded reads ----

  # The `length` bytes of `bytes` at `offset`. A record shorter than its layout
  # raises Truncated here, so no decoder sees the nil that `byteslice` returns
  # or does arithmetic on a field `unpack` could not fill.
  def bytes_at(bytes, offset, length)
    unless offset >= 0 && length >= 0 && offset + length <= bytes.bytesize
      raise Truncated, "structure of #{bytes.bytesize} bytes has no #{length} bytes at #{offset}"
    end

    bytes.byteslice(offset, length)
  end

  # Unpack exactly `length` bytes at `offset` with `template`.
  def fields_at(bytes, offset, length, template) = bytes_at(bytes, offset, length).unpack(template)

  def popcount(bytes) = bytes.each_byte.sum { POPCOUNT[_1] }

  # ---- primitive decoders ----

  def date_text(value) = Time.at(value - EPOCH_1904).utc.strftime("%Y-%m-%dT%H:%M:%SZ")
  def date_field(value) = { raw: value, utc: date_text(value) }
  def hex(bytes) = bytes.unpack1("H*")
  def flag_names(value, names) = names.filter_map { |bit, name| name if value[bit] == 1 }
  def unknown_bits(value, names) = (0...32).select { value[_1] == 1 && !names.key?(_1) }
  def four_chars(bytes) = bytes.match?(/\A[\x20-\x7e]{4}\z/n) ? bytes.dup.force_encoding("US-ASCII") : "0x#{hex(bytes)}"

  # Printable ASCII stays; every other UTF-16 unit is written \uXXXX.
  def name_text(units_bytes)
    units_bytes.unpack("n*").map { _1.between?(0x20, 0x7e) && _1 != 0x5c ? _1.chr : format("\\u%04X", _1) }.join
  end

  def parse_fork(bytes)
    size, clump, blocks = fields_at(bytes, 0, 16, "Q>L>L>")
    extents = fields_at(bytes, 16, 64, "L>*").each_slice(2).map { Extent.new(*_1) }
    Fork.new(size, clump, blocks, extents)
  end

  def fork_report(fork)
    { logical_size: fork.logical_size, clump_size: fork.clump_size, total_blocks: fork.total_blocks,
      extents: fork.extents.reject { _1.count.zero? }.map(&:to_s) }
  end

  # ---- boot area and volume header ----

  def boot_report(volume)
    boot = volume.read(0, 1024)
    { zero: boot.count("\x00".b) == 1024, signature: four_chars(boot.byteslice(0, 4)),
      first_16_bytes: hex(boot.byteslice(0, 16)), sha256: Digest::SHA256.hexdigest(boot) }
  end

  def parse_header(bytes)
    sig, version, attrs, last, jib, create, modify, backup, checked, files, folders, bsize, total, free,
      next_alloc, rsrc_clump, data_clump, next_cnid, writes, encodings = fields_at(bytes, 0, 80, "a2nNa4NNNNNNNNNNNNNNNQ>")
    forks = FORK_NAMES.each_with_index.to_h { |name, i| [name, parse_fork(bytes_at(bytes, 112 + 80 * i, 80))] }
    { signature: sig, version: version, attributes: attrs, last_mounted: last, journal_info_block: jib,
      dates: { create: create, modify: modify, backup: backup, checked: checked },
      file_count: files, folder_count: folders, block_size: bsize, total_blocks: total, free_blocks: free,
      next_allocation: next_alloc, rsrc_clump: rsrc_clump, data_clump: data_clump, next_catalog_id: next_cnid,
      write_count: writes, encodings_bitmap: encodings, finder_info: bytes_at(bytes, 80, 32), forks: forks }
  end

  def header_report(header)
    header.except(:forks, :dates, :finder_info, :last_mounted, :signature, :attributes).merge(
      signature: header[:signature].dup.force_encoding("US-ASCII").scrub("?"),
      attributes: format("0x%08X", header[:attributes]),
      attribute_flags: flag_names(header[:attributes], VOLUME_ATTRIBUTES),
      attribute_unknown_bits: unknown_bits(header[:attributes], VOLUME_ATTRIBUTES),
      last_mounted: four_chars(header[:last_mounted]),
      dates: header[:dates].transform_values { date_field(_1) },
      finder_info_words: header[:finder_info].unpack("N*").map { format("0x%08X", _1) },
      forks: header[:forks].transform_values { fork_report(_1) }
    )
  end

  def read_header(volume, offset)
    bytes = volume.read(offset, 512)
    parse_header(bytes).tap do |h|
      raise Malformed, "no HFS Plus signature at #{offset}: #{hex(bytes.byteslice(0, 4))}" unless %w[H+ HX].include?(h[:signature])
      raise Malformed, "block size #{h[:block_size]} is not a power of two from 512" unless valid_block_size?(h[:block_size])
    end
  end

  def valid_block_size?(size) = size >= 512 && size.nobits?(size - 1)

  # Fields the design requires equal in the alternate header, plus those native
  # volumes keep equal after use; the report lists those that differ.
  def alternate_diff(primary, alternate)
    (primary.keys - %i[forks dates]).filter_map { |key| key if primary[key] != alternate[key] } +
      primary[:forks].filter_map { |name, fork| :"fork_#{name}" if fork != alternate[:forks][name] } +
      primary[:dates].filter_map { |name, value| :"date_#{name}" if value != alternate[:dates][name] }
  end

  # ---- B-tree header nodes ----

  def descriptor(raw)
    NodeDescriptor.new(*fields_at(raw, 0, 14, "NNcCn"))
  end

  # The record offsets of a node, validated: ascending, inside the node and
  # below the offset table. The table holds count + 1 big-endian offsets at the
  # end of the node, last record first; the extra one marks the free space. The
  # first record starts right after the 14-byte node descriptor.
  def record_offsets(raw, count)
    table = raw.bytesize - 2 * (count + 1)
    raise Malformed, "#{count} records do not fit a node of #{raw.bytesize}" if table < NODE_DESCRIPTOR_BYTES

    offsets = fields_at(raw, table, 2 * (count + 1), "n*").reverse
    ok = offsets.first == NODE_DESCRIPTOR_BYTES && offsets.each_cons(2).all? { |a, b| a <= b } && offsets.last <= table
    raise Malformed, "record offsets #{offsets.inspect} are inconsistent" unless ok

    offsets
  end

  def record_slices(raw, count)
    record_offsets(raw, count).each_cons(2).map { |a, b| raw.byteslice(a, b - a) }
  end

  def read_node(volume, fork, number, node_size)
    volume.read_fork(fork, number * node_size, node_size)
  end

  def tree_header_report(volume, fork)
    probe = volume.read_fork(fork, 0, 512)
    node_size = fields_at(probe, 32, 2, "n").first
    raise Malformed, "node size #{node_size} is not a power of two from 512 to 32768" unless (512..32_768).cover?(node_size) && node_size.nobits?(node_size - 1)

    raw = read_node(volume, fork, 0, node_size)
    desc = descriptor(raw)
    depth, root, leaf_records, first_leaf, last_leaf, _, max_key, total, free, _, clump, btype, ctype, attrs =
      fields_at(raw, 14, 42, "nNNNNnnNNnNCCN")
    offsets = record_offsets(raw, desc.num_records)
    map = node_map_report(volume, fork, desc, header_map_record(raw, offsets), node_size, total, free)
    {
      node_kind: NODE_KINDS.fetch(desc.kind, desc.kind), node_size: node_size, record_offsets: offsets,
      depth: depth, root_node: root, leaf_records: leaf_records, first_leaf: first_leaf, last_leaf: last_leaf,
      max_key_length: max_key, total_nodes: total, free_nodes: free, clump_size: clump, tree_type: btype,
      key_compare_type: ctype, attributes: format("0x%08X", attrs),
      attribute_flags: flag_names(attrs, TREE_ATTRIBUTES), attribute_unknown_bits: unknown_bits(attrs, TREE_ATTRIBUTES),
      reserved1: fields_at(raw, 44, 2, "n").first, reserved3_zero: bytes_at(raw, 56, 64).count("\x00".b) == 64,
      user_data_zero: bytes_at(raw, 120, 128).count("\x00".b) == 128,
      fork_size_matches_nodes: fork.logical_size == total * node_size,
      node_map: map
    }
  end

  # A header node holds three records: the tree header, 128 bytes of user data
  # and the map record, which begins the node allocation bitmap. Offsets 2 and 3
  # bound the third.
  def header_map_record(raw, offsets)
    raise Malformed, "header node has #{offsets.size - 1} records, not the tree header, user data and map" if offsets.size < 4

    raw.byteslice(offsets[2], offsets[3] - offsets[2])
  end

  # The header node's map record, then any map nodes its forward links name.
  def node_map_report(volume, fork, desc, first, node_size, total, free)
    chain, maps, details = map_chain(volume, fork, desc.flink, node_size)
    all = ([first] + maps).join
    bits = Bits.of(all, total)
    used = bits.set_count
    {
      header_map_bytes: first.bytesize, map_nodes: chain, map_record_bytes: maps.map(&:bytesize), map_node_details: details,
      bits_available: all.bytesize * 8, used_bits: used, used_matches_total_minus_free: used == total - free,
      first_clear_bit: bits.first_clear, bits_beyond_total_set: popcount(all) - used,
      free_nodes_all_zero: free_nodes_zero?(volume, fork, bits, node_size)
    }
  end

  # Whether the unused nodes (map bit clear) hold only zero bytes; the first
  # MAX_ZERO_CHECK nodes are examined.
  def free_nodes_zero?(volume, fork, bits, node_size)
    bits.clear_numbers.lazy.take_while { _1 < MAX_ZERO_CHECK }.all? do |number|
      read_node(volume, fork, number, node_size).count("\x00".b) == node_size
    end
  end

  def map_chain(volume, fork, link, node_size)
    seen = Set.new
    maps = []
    details = []
    while link != 0
      raise Malformed, "map node chain loops at #{link}" unless seen.add?(link)
      raise Malformed, "map node chain is longer than #{MAX_MAP_NODES}" if seen.size > MAX_MAP_NODES

      raw = read_node(volume, fork, link, node_size)
      desc = descriptor(raw)
      raise Malformed, "node #{link} on the map chain has kind #{desc.kind}" unless desc.kind == 2

      slices = record_slices(raw, desc.num_records)
      maps << slices.first.to_s
      details << { node: link, flink: desc.flink, blink: desc.blink, height: desc.height, num_records: desc.num_records,
                   record_offsets: record_offsets(raw, desc.num_records) }
      link = desc.flink
    end
    [seen.to_a, maps, details]
  end

  # ---- index nodes ----

  # How many examples of a finding a report lists, however many there are.
  EXAMPLES_SHOWN = 8

  # The key at the front of a record, its two-byte length field included.
  def record_key(slice)
    raise Malformed, "record of #{slice.bytesize} bytes is shorter than a key length" if slice.bytesize < 2

    key = slice.byteslice(0, 2 + slice.unpack1("n"))
    raise Malformed, "key of #{key.bytesize} bytes overruns its record of #{slice.bytesize}" if key.bytesize > slice.bytesize

    key
  end

  # The child number that ends an index record, after its key padded to an even length.
  def index_child(slice, key)
    raise Malformed, "index record of #{slice.bytesize} bytes does not end with a child number" unless slice.bytesize == ((key.bytesize + 1) & ~1) + 4

    slice.unpack1("@#{slice.bytesize - 4}N")
  end

  IndexEntry = Data.define(:position, :key, :child)
  IndexNode = Data.define(:number, :height, :entries)
  IndexChild = Data.define(:height, :kind, :first_key)

  # A count of findings and the first EXAMPLES_SHOWN of them: the report stays
  # small however many findings a hostile image forces.
  Findings = Data.define(:count, :examples) do
    def plus(items) = Findings.new(count + items.size, (examples + items).first(EXAMPLES_SHOWN))
  end

  # What the walk has accumulated over the levels read so far.
  IndexWalk = Data.define(:levels, :lone, :mismatches, :reads)

  # One index node's records: each key and the child it names. Nothing is
  # read for the children here.
  def index_node(volume, fork, tree, number)
    raw = read_node(volume, fork, number, tree[:node_size])
    desc = descriptor(raw)
    raise Malformed, "node #{number} on an index level has kind #{desc.kind}" unless desc.kind.zero?

    entries = record_slices(raw, desc.num_records).each_with_index.map do |slice, position|
      key = record_key(slice)
      IndexEntry.new(position, key, index_child(slice, key))
    end
    IndexNode.new(number, desc.height, entries)
  end

  # A child's height, kind and the key of its first record.
  def index_child_probe(volume, fork, tree, number)
    raw = read_node(volume, fork, number, tree[:node_size])
    desc = descriptor(raw)
    first = desc.num_records.zero? ? "".b : record_key(record_slices(raw, desc.num_records).first)
    IndexChild.new(desc.height, desc.kind, first)
  end

  # The children one level names must be distinct, and all the children of the
  # walk must fit in the tree: each node is then read as a child at most once.
  def check_index_children(children, reads, limit)
    twice = children.tally.find { _2 > 1 }
    raise Malformed, "child #{twice.first} is named by two index records" if twice
    raise Malformed, "the index walk names #{reads + children.size} children in a tree of #{limit} nodes" if reads + children.size > limit
  end

  # The reducer: fold one level (its nodes and the probes of their children)
  # into the walk.
  def fold_index_level(walk, root, nodes, probes)
    nodes.each do |node|
      node.entries.each do |e|
        next if probes.fetch(e.child).height == node.height - 1

        raise Malformed, "child #{e.child} of #{node.number} has height #{probes.fetch(e.child).height}, not #{node.height - 1}"
      end
    end
    counts = nodes.map { _1.entries.size }
    lone = nodes.filter_map { _1.number if _1.entries.size == 1 && _1.number != root }
    differing = nodes.flat_map do |node|
      node.entries.reject { _1.key == probes.fetch(_1.child).first_key }.map do |e|
        { node: node.number, record: e.position, child: e.child, index_key: hex(e.key), first_key: hex(probes.fetch(e.child).first_key) }
      end
    end
    level = { height: nodes.first.height, nodes: nodes.size, records: counts.sum, min_records: counts.min, max_records: counts.max }
    walk.with(levels: walk.levels + [level], lone: walk.lone.plus(lone), mismatches: walk.mismatches.plus(differing),
              reads: walk.reads + probes.size)
  end

  # The index levels of a tree from the root down. Every index node is read once
  # and every distinct child once, for its first key, and the children together
  # cannot outnumber the tree's nodes. The report counts nodes and records per
  # level, the index nodes other than the root with a single record, and the
  # index records whose key differs from the first key of the child they name;
  # the first EXAMPLES_SHOWN of each are listed with their keys in hex.
  def index_report(volume, fork, tree)
    return { levels: [] } if tree[:depth] < 2

    limit = [tree[:total_nodes], MAX_NODES].min
    walk = IndexWalk.new(levels: [], lone: Findings.new(0, []), mismatches: Findings.new(0, []), reads: 0)
    level = [tree[:root_node]]
    until level.empty?
      nodes = level.map { index_node(volume, fork, tree, _1) }
      children = nodes.flat_map { |node| node.entries.map(&:child) }
      check_index_children(children, walk.reads, limit)
      probes = children.to_h { [_1, index_child_probe(volume, fork, tree, _1)] }
      walk = fold_index_level(walk, tree[:root_node], nodes, probes)
      level = children.select { probes.fetch(_1).kind.zero? }
    end
    { levels: walk.levels, one_record_non_root_index_nodes: walk.lone.count, one_record_examples: walk.lone.examples,
      key_mismatches: walk.mismatches.count, key_mismatch_examples: walk.mismatches.examples }
  end

  # ---- allocation bitmap and layout ----

  def bitmap_report(volume, header)
    fork = header[:forks][:allocation]
    total = header[:total_blocks]
    bytes = (total + 7) / 8
    raise Malformed, "bitmap fork of #{fork.logical_size} bytes is too short for #{total} blocks" if fork.logical_size < bytes
    raise Malformed, "bitmap of #{bytes} bytes is over the #{MAX_BITMAP_BYTES} byte limit" if bytes > MAX_BITMAP_BYTES

    bits = Bits.of(volume.read_fork(fork, 0, bytes), total)
    set = bits.set_count
    { bitmap_bytes: fork.logical_size, set_bits: set, clear_bits: total - set,
      clear_matches_free_blocks: total - set == header[:free_blocks],
      first_clear_block: bits.first_clear, last_set_block: bits.last_set,
      block_0_set: bits.set?(0), last_block_set: bits.set?(total - 1) }.merge(free_blocks_report(volume, header, bits))
  end

  # Whether the free blocks hold only zero bytes: stale data left by an earlier
  # volume or a formatter would show here. Examines free blocks until
  # MAX_FREE_BYTES have been read.
  def free_blocks_report(volume, header, bits)
    block_size = header[:block_size]
    free = bits.clear_numbers.first(MAX_FREE_BYTES / block_size)
    dirty = free.reject { volume.read(_1 * block_size, block_size).count("\x00".b) == block_size }
    { free_blocks_examined: free.size, free_blocks_nonzero: dirty.size, first_nonzero_free_block: dirty.first }.compact
  end

  # Where the format-owned objects sit, in block order.
  def layout_report(volume, header)
    block_size = header[:block_size]
    objects = header[:forks].reject { |_, fork| fork.empty? }.flat_map do |name, fork|
      fork.extents.reject { _1.count.zero? }.map { |e| { object: name.to_s, start: e.start, count: e.count } }
    end
    { volume_bytes: header[:total_blocks] * block_size, alternate_header_offset: header[:total_blocks] * block_size - 1024,
      objects_in_block_order: objects.sort_by { _1[:start] } }
  end

  # ---- catalog ----

  def catalog_key(slice)
    raise Malformed, "catalog record of #{slice.bytesize} bytes is shorter than a key" if slice.bytesize < 8

    key_length, parent, name_length = fields_at(slice, 0, 8, "nNn")
    raise Malformed, "key length #{key_length} disagrees with name length #{name_length}" unless key_length == 6 + 2 * name_length && name_length <= 255

    raise Malformed, "key of #{key_length} bytes overruns its record" if 2 + key_length > slice.bytesize

    [parent, name_text(bytes_at(slice, 8, 2 * name_length)), (2 + key_length + 1) & ~1]
  end

  def bsd_report(bytes)
    owner, group, admin, owner_flags, mode, special = fields_at(bytes, 0, 16, "NNCCnN")
    { owner: owner, group: group, admin_flags: admin, owner_flags: owner_flags, mode: format("0%o", mode), special: special }
  end

  FOLDER_RECORD_BYTES = 88
  FILE_RECORD_BYTES = 248
  DATE_NAMES = %i[create content_mod attribute_mod access backup].freeze

  def date_fields(values) = DATE_NAMES.zip(values).to_h { |name, value| [name, date_field(value)] }

  def folder_record(data)
    _, flags, valence, id, *dates = fields_at(data, 0, 32, "nnNNNNNNN")
    { type: "folder", flags: format("0x%04X", flags), valence: valence, id: id, dates: date_fields(dates),
      bsd: bsd_report(bytes_at(data, 32, 16)), user_info: hex(bytes_at(data, 48, 16)),
      finder_info: hex(bytes_at(data, 64, 16)), text_encoding: fields_at(data, 80, 4, "N").first,
      reserved: fields_at(data, 84, 4, "N").first }
  end

  def file_record(data)
    _, flags, link_word, id, *dates = fields_at(data, 0, 32, "nnNNNNNNN")
    user_info = bytes_at(data, 48, 16)
    # The word after the flags is the hard-link chain head on an indirect node
    # file; elsewhere it is reserved.
    { type: "file", flags: format("0x%04X", flags), link_word: link_word, id: id, dates: date_fields(dates),
      bsd: bsd_report(bytes_at(data, 32, 16)), file_type: four_chars(user_info.byteslice(0, 4)),
      creator: four_chars(user_info.byteslice(4, 4)), user_info: hex(user_info), finder_info: hex(bytes_at(data, 64, 16)),
      text_encoding: fields_at(data, 80, 4, "N").first,
      data_fork: fork_report(parse_fork(bytes_at(data, 88, 80))), rsrc_fork: fork_report(parse_fork(bytes_at(data, 168, 80))) }
  end

  # A thread record names the parent and name of its folder or file, which are
  # not the parent and name of the thread's own key; the fields are called
  # thread_parent and thread_name so the key's parent and name stay distinct.
  def thread_record(data, kind)
    parent, name_length = fields_at(data, 4, 6, "Nn")
    raise Malformed, "thread name of #{name_length} units overruns its record" if 10 + 2 * name_length > data.bytesize

    { type: kind, thread_parent: parent, thread_name: name_text(bytes_at(data, 10, 2 * name_length)) }
  end

  def sized_record(data, type, bytes)
    raise Malformed, "record type #{type} has #{data.bytesize} bytes, expected #{bytes}" unless data.bytesize == bytes

    data
  end

  def catalog_record(slice)
    parent, name, data_at = catalog_key(slice)
    data = slice.byteslice(data_at..) || +""
    raise Malformed, "record for #{name.inspect} has no type" if data.bytesize < 2

    type = fields_at(data, 0, 2, "n").first
    body =
      case type
      when 1 then folder_record(sized_record(data, type, FOLDER_RECORD_BYTES))
      when 2 then file_record(sized_record(data, type, FILE_RECORD_BYTES))
      when 3 then thread_record(data, "folder-thread")
      when 4 then thread_record(data, "file-thread")
      else raise Malformed, "record type #{type} for #{name.inspect}"
      end
    { parent: parent, name: name }.merge(body)
  end

  # Walk the leaf chain, counting every record and keeping the first `limit`
  # and every child of the root. A damaged chain ends the walk and is reported.
  def catalog_report(volume, fork, header, limit)
    node_size = header[:node_size]
    stats = { leaf_nodes: 0, records: 0, by_type: Hash.new(0) }
    first = []
    root_children = []
    problem = nil
    seen = Set.new
    node = header[:first_leaf]
    begin
      while node != 0
        raise Malformed, "leaf chain loops at #{node}" unless seen.add?(node)
        raise Malformed, "leaf chain is longer than #{MAX_NODES}" if seen.size > MAX_NODES

        raw = read_node(volume, fork, node, node_size)
        desc = descriptor(raw)
        raise Malformed, "node #{node} on the leaf chain has kind #{desc.kind}" unless desc.kind == -1

        stats[:leaf_nodes] += 1
        record_slices(raw, desc.num_records).each do |slice|
          rec = catalog_record(slice)
          stats[:records] += 1
          stats[:by_type][rec[:type]] += 1
          first << rec if first.size < limit
          root_children << rec if rec[:parent] == ROOT_ID && !rec[:type].end_with?("thread") && root_children.size < MAX_ROOT_LISTING
        end
        node = desc.flink
      end
    rescue Error => e
      problem = "catalog walk stopped at node #{node}: #{e.message}"
    end
    { stats: stats, first_records: first, root_children: root_children, walk_problem: problem }.compact
  end

  # ---- attributes, extents overflow and compression ----

  MAX_LEAF_RECORDS = 1 << 20
  MAX_TABLE_ENTRIES = 4096
  PREVIEW_BYTES = 16
  ATTRIBUTE_KINDS = { 0x10 => "inline", 0x20 => "fork", 0x30 => "extents" }.freeze
  DECMPFS_NAME = "com.apple.decmpfs"
  OWNER_FLAG_COMPRESSED = 0x20

  # The raw leaf records of a tree in key order. A loop in the chain, a node
  # that is not a leaf or more than MAX_LEAF_RECORDS records is Malformed.
  def leaf_slices(volume, fork, tree)
    seen = Set.new
    slices = []
    node = tree[:first_leaf]
    until node.zero?
      raise Malformed, "leaf chain loops at #{node}" unless seen.add?(node)

      raw = read_node(volume, fork, node, tree[:node_size])
      desc = descriptor(raw)
      raise Malformed, "node #{node} on the leaf chain has kind #{desc.kind}" unless desc.kind == -1

      slices.concat(record_slices(raw, desc.num_records))
      raise Malformed, "more than #{MAX_LEAF_RECORDS} leaf records" if slices.size > MAX_LEAF_RECORDS

      node = desc.flink
    end
    slices
  end

  # The records of a leaf chain: how many there are and the first `limit`
  # decoded by the block.
  def leaf_report(volume, fork, tree, limit, &decoder)
    slices = leaf_slices(volume, fork, tree)
    { count: slices.size, records: slices.first(limit).map(&decoder) }
  end

  # An attribute leaf record: key (length, pad, fileID, startBlock, name
  # length, name units) then data that begins with its four-byte type.
  AttributeRecord = Data.define(:file_id, :name, :start_block, :type, :data)

  def parse_attribute(slice)
    key_length, _, file_id, start_block, name_length = fields_at(slice, 0, 14, "nnNNn")
    raise Malformed, "attribute key length #{key_length} disagrees with name length #{name_length}" unless key_length == 12 + 2 * name_length

    data = bytes_at(slice, 2 + key_length, slice.bytesize - 2 - key_length)
    AttributeRecord.new(file_id, name_text(bytes_at(slice, 14, 2 * name_length)), start_block, fields_at(data, 0, 4, "N").first, data)
  end

  # The value of an inline attribute: type, two reserved words, size, value.
  def inline_value(attribute)
    size = fields_at(attribute.data, 12, 4, "N").first
    bytes_at(attribute.data, 16, size)
  end

  def attribute_report(attribute)
    base = { file_id: attribute.file_id, name: attribute.name, start_block: attribute.start_block,
             kind: ATTRIBUTE_KINDS.fetch(attribute.type, format("0x%X", attribute.type)) }
    base.merge(attribute_body(attribute))
  end

  def attribute_body(attribute)
    data = attribute.data
    case attribute.type
    when 0x10
      value = inline_value(attribute)
      { reserved: hex(bytes_at(data, 4, 8)), size: value.bytesize, value_head: hex(value.byteslice(0, PREVIEW_BYTES)),
        value_sha256: Digest::SHA256.hexdigest(value), record_bytes_after_value: data.bytesize - 16 - value.bytesize }
    when 0x20
      { reserved: fields_at(data, 4, 4, "N").first, fork: fork_report(parse_fork(bytes_at(data, 8, 80))) }
    when 0x30
      { reserved: fields_at(data, 4, 4, "N").first, extents: extent_list(fields_at(data, 8, 64, "N*")) }
    else
      { data_head: hex(data.byteslice(0, PREVIEW_BYTES)) }
    end
  end

  def extent_list(words) = words.each_slice(2).reject { _2.zero? }.map { "#{_1}+#{_2}" }

  # An extents overflow leaf record: key (length 10, fork type, pad, fileID,
  # startBlock) then eight extent descriptors.
  def extent_record(slice)
    key_length, fork_type, _, file_id, start_block = fields_at(slice, 0, 12, "nCCNN")
    raise Malformed, "extents key length #{key_length} is not 10" unless key_length == 10

    { fork: { 0 => "data", 0xFF => "resource" }.fetch(fork_type, fork_type), file_id: file_id, start_block: start_block,
      extents: extent_list(fields_at(slice, 12, 64, "N*")) }
  end

  # The catalog's file records that carry ownerFlags 0x20, with their forks.
  def compressed_catalog_files(volume, fork, tree)
    errors, files = leaf_slices(volume, fork, tree).filter_map do |slice|
      parent, name, data_at = catalog_key(slice)
      data = slice.byteslice(data_at..) || +""
      next unless data.bytesize == FILE_RECORD_BYTES && fields_at(data, 0, 2, "n").first == 2
      next if fields_at(data, 41, 1, "C").first.nobits?(OWNER_FLAG_COMPRESSED)

      { parent: parent, name: name, id: fields_at(data, 8, 4, "N").first,
        data_fork: parse_fork(bytes_at(data, 88, 80)), rsrc_fork: parse_fork(bytes_at(data, 168, 80)) }
    rescue Error => e
      { error: "catalog record: #{e.message}" }
    end.partition { _1.key?(:error) }
    files + errors.first(EXAMPLES_SHOWN)
  end

  # The decmpfs attribute of each file that has one, keyed by file ID. A record
  # that cannot be decoded is kept as its Error under the file ID in its key,
  # so that file's entry reports it; a later decoded decmpfs record of the same
  # file replaces it.
  def decmpfs_attributes(slices)
    slices.filter_map do |slice|
      attribute = parse_attribute(slice)
      [attribute.file_id, attribute] if attribute.name == DECMPFS_NAME
    rescue Error => e
      [slice.unpack1("@4N"), e] if slice.bytesize >= 8
    end.sort_by { |_, found| found.is_a?(Error) ? 0 : 1 }.to_h
  end

  # Every compressed file with its decmpfs header and the head of its resource
  # fork's chunk table, read from the volume and not through the mount. One
  # undecodable record or fork is reported in its own entry.
  def compression_report(volume, forks, trees)
    decmpfs = decmpfs_attributes(leaf_slices(volume, forks[:attributes], trees[:attributes]))
    compressed_catalog_files(volume, forks[:catalog], trees[:catalog]).map do |file|
      file.key?(:error) ? file : compressed_file_report(volume, file, decmpfs[file[:id]])
    end
  end

  def compressed_file_report(volume, file, attribute)
    entry = file.slice(:parent, :name, :id).merge(data_fork: fork_report(file[:data_fork]), rsrc_fork: fork_report(file[:rsrc_fork]))
    case attribute
    in nil then entry.merge(decmpfs: "absent")
    in Error then entry.merge(error: "attribute record: #{attribute.message}")
    in { type: } if ATTRIBUTE_KINDS[type] != "inline" then entry.merge(decmpfs: "not inline: #{attribute_report(attribute)[:kind]}")
    else
      header = decmpfs_header(inline_value(attribute))
      entry.merge(decmpfs: header, resource_table: resource_table_or_error(volume, file[:rsrc_fork], header))
    end
  rescue Error => e
    entry.merge(error: e.message)
  end

  # The table, or why it could not be read: a fork whose table lies past its
  # first eight extents is reported by that message, not followed.
  def resource_table_or_error(volume, fork, header)
    resource_table(volume, fork, header)
  rescue Error => e
    { error: e.message }
  end

  def decmpfs_header(value)
    magic = bytes_at(value, 0, 4)
    type, size = fields_at(value, 4, 12, "VQ<")
    payload = value.byteslice(16..)
    { magic: hex(magic), type: type, uncompressed_size: size, attribute_bytes: value.bytesize, payload_bytes: payload.bytesize,
      payload_head: hex(payload.byteslice(0, PREVIEW_BYTES)), payload_tail: hex(payload.byteslice([payload.bytesize - 8, 0].max, 8)) }
  end

  # Read up to `length` bytes of a fork from `offset`, stopping at its logical size.
  def fork_prefix(volume, fork, offset, length)
    volume.read_fork(fork, offset, [[length, fork.logical_size - offset].min, 0].max)
  end

  CHUNK_BYTES = 65_536

  def resource_table(volume, fork, header)
    return "no resource fork" if fork.logical_size.zero?

    chunks = header[:uncompressed_size].divmod(CHUNK_BYTES).then { |whole, rest| whole + (rest.zero? ? 0 : 1) }
    case header[:type]
    when 4 then classic_table(volume, fork).merge(expected_chunks: chunks)
    when 8, 12 then offset_table(volume, fork).merge(expected_chunks: chunks)
    else { unexpected_resource_fork_bytes: fork.logical_size }
    end
  end

  # Type 4: 16-byte big-endian header, at the data offset a big-endian
  # length, then a little-endian count and (offset, length) pairs.
  def classic_table(volume, fork)
    data_off, map_off, data_len, map_len = fields_at(fork_prefix(volume, fork, 0, 16), 0, 16, "N4")
    head = fork_prefix(volume, fork, data_off, 8)
    resource_len = fields_at(head, 0, 4, "N").first
    count = fields_at(head, 4, 4, "V").first
    shown = [count, MAX_TABLE_ENTRIES].min
    pairs = fields_at(fork_prefix(volume, fork, data_off + 8, 8 * shown), 0, 8 * shown, "V*").each_slice(2).to_a
    { fork_size: fork.logical_size, data_offset: data_off, map_offset: map_off, data_length: data_len, map_length: map_len,
      resource_length: resource_len, chunk_count: count, pairs: pairs.map { "#{_1}+#{_2}" },
      **(shown < count ? { truncated: "#{shown} of #{count} pairs shown" } : {}),
      chunk_heads: pairs.first(8).map { |offset, _| hex(fork_prefix(volume, fork, data_off + 4 + offset, 4)) },
      header_hex: hex(fork_prefix(volume, fork, 0, 32)), map_head: hex(fork_prefix(volume, fork, map_off, 32)) }
  end

  # Types 8 and 12: a table of little-endian words that ends where the first
  # offset, the first word, says the chunks begin. The words are reported as
  # found, up to MAX_TABLE_WORDS_SHOWN of them.
  MAX_TABLE_WORDS_SHOWN = 24

  def offset_table(volume, fork)
    first = fields_at(fork_prefix(volume, fork, 0, 4), 0, 4, "V").first
    raise Malformed, "first offset #{first} is not a table of 4 to #{4 * MAX_TABLE_ENTRIES} bytes" unless first.between?(4, 4 * MAX_TABLE_ENTRIES) && first.nobits?(3)

    words = fields_at(fork_prefix(volume, fork, 0, first), 0, first, "V*")
    offsets = words.take_while.with_index { |word, i| i.zero? || word > words[i - 1] }
    { fork_size: fork.logical_size, first_offset: first, table_words: words.size, ascending_offsets: offsets.size,
      words: words.first(MAX_TABLE_WORDS_SHOWN), words_after_offsets_are_zero: words.drop(offsets.size).all?(0),
      last_offset_is_fork_size: offsets.last == fork.logical_size,
      chunk_heads: offsets.each_cons(2).first(8).map { |start, _| hex(fork_prefix(volume, fork, start, 4)) } }
  end

  # ---- journal ----

  def journal_report(volume, header)
    block = volume.read(header[:journal_info_block] * header[:block_size], 180)
    flags = fields_at(block, 0, 4, "N").first
    offset, size = fields_at(block, 36, 16, "Q>Q>")
    report = { info_flags: format("0x%08X", flags), device_signature: hex(bytes_at(block, 4, 32)), offset: offset, size: size }
    inside = begin
      journal_header_report(volume, offset, size)
    rescue Error => e
      raise if volume.base.zero?

      { header: "unreadable counting from the volume: #{e.message}" }
    end
    return report.merge(inside) if inside.key?(:magic) || volume.base.zero?

    # Inside a wrapper the offset may count from the start of the image instead.
    outside = journal_header_report(volume.with(base: 0), offset, size)
    report.merge(outside.key?(:magic) ? outside.merge(offset_counts_from: "image") : inside)
  end

  def journal_header_report(volume, offset, size)
    return { header: "outside the volume or empty" } if size < 44

    raw = volume.read(offset, 44)
    big = raw.unpack1("N") == 0x4a4e4c78
    little = raw.unpack1("V") == 0x4a4e4c78
    return { header: "no journal magic", first_bytes: hex(raw.byteslice(0, 16)) } unless big || little

    magic, endian, start, finish, jsize, blhdr, checksum, jhdr = raw.unpack(big ? "NNQ>Q>Q>NNN" : "VVQ<Q<Q<VVV")
    report = { byte_order: big ? "big" : "little", magic: format("0x%08X", magic), endian: format("0x%08X", endian), start: start,
               end: finish, size: jsize, blhdr_size: blhdr, checksum: format("0x%08X", checksum), jhdr_size: jhdr }
    start == finish ? report : report.merge(first_transaction: transaction_report(volume, offset, size, start, jhdr, big))
  end

  BLOCK_LIST_HEADER_BYTES = 16
  BLOCK_INFO_BYTES = 16
  BLOCK_INFO_SHOWN = 4

  # The block list header at the journal's `start`, and its first block infos: a
  # header of two counts, the bytes the transaction uses, a checksum and a pad,
  # then entries of block number (8), size (4) and a four-byte link.
  # The transactions live between the journal header (jhdr_size bytes) and the
  # end of the journal. The result is a Hash whether or not `start` is usable.
  def transaction_report(volume, offset, size, start, jhdr, big)
    return { error: "start #{start} is not in [#{jhdr}, #{size}), after the journal header" } unless (jhdr...size).cover?(start)

    length = [BLOCK_LIST_HEADER_BYTES + BLOCK_INFO_SHOWN * BLOCK_INFO_BYTES, size - start].min
    return { error: "start #{start} leaves no room for a block list header in the journal of #{size}" } if length < BLOCK_LIST_HEADER_BYTES

    raw = volume.read(offset + start, length)
    max_blocks, num_blocks, bytes_used, checksum, pad = raw.unpack(big ? "nnNNN" : "vvVVV")
    entries = [num_blocks, (length - BLOCK_LIST_HEADER_BYTES) / BLOCK_INFO_BYTES, BLOCK_INFO_SHOWN].min
    infos = (0...entries).map do |i|
      bnum, bsize, link = raw.unpack(big ? "@#{BLOCK_LIST_HEADER_BYTES + BLOCK_INFO_BYTES * i}Q>NN" : "@#{BLOCK_LIST_HEADER_BYTES + BLOCK_INFO_BYTES * i}Q<VV")
      { block: bnum, size: bsize, link: link }
    end
    { max_blocks: max_blocks, num_blocks: num_blocks, bytes_used: bytes_used, checksum: format("0x%08X", checksum), pad: pad, block_infos: infos }
  end

  # ---- wrapper ----

  HFS_EXTENT_RECORD_BYTES = 12
  HFS_EXTENTS_NODE_BYTES = 512
  MAX_HFS_EXTENT_NODES = 4096

  # The fixed-width fields of a master directory block: name => [offset, template].
  # The extents and catalog file extent records and the volume name are decoded
  # apart, from their own offsets.
  MDB_FIELDS = {
    create: [2, "N"], modify: [6, "N"], attributes: [10, "n"], file_count: [12, "n"], bitmap_sector: [14, "n"],
    alloc_ptr: [16, "n"], total_blocks: [18, "n"], block_size: [20, "N"], clump_size: [24, "N"], first_block: [28, "n"],
    next_id: [30, "N"], free_blocks: [34, "n"], embed_signature: [124, "n"], embed_start: [126, "n"], embed_count: [128, "n"],
    extents_file_bytes: [130, "N"], catalog_file_bytes: [146, "N"]
  }.freeze
  MDB_BYTES = 162
  MDB_EXTENTS_FILE_AT = 134
  MDB_CATALOG_FILE_AT = 150
  MDB_NAME_AT = 36
  MDB_NAME_MAX = 27
  PACK_WIDTH = { "N" => 4, "n" => 2 }.freeze

  def mdb_fields(mdb) = MDB_FIELDS.transform_values { |offset, template| fields_at(mdb, offset, PACK_WIDTH.fetch(template), template).first }

  # A classic HFS master directory block with an embedded HFS Plus volume. The
  # embedded volume's location is computed before the outer volume's own fields,
  # so a damaged optional outer field cannot hide the volume the caller needs.
  def wrapper_report(volume)
    mdb = volume.read(1024, MDB_BYTES)
    return nil unless mdb.byteslice(0, 2) == "BD".b

    m = mdb_fields(mdb)
    # Only a wrapper that declares an embedded HFS Plus volume locates one.
    located = m[:embed_signature] == EMBED_SIGNATURE ? { embedded_volume_offset: m[:first_block] * 512 + m[:embed_start] * m[:block_size] } : {}
    outer = begin
      hfs_volume_report(volume, mdb, m)
    rescue Error => e
      { error: e.message }
    end
    { hfs_block_size: m[:block_size], first_allocation_block: m[:first_block], embed_signature: format("0x%04X", m[:embed_signature]),
      embed_start: m[:embed_start], embed_count: m[:embed_count], outer: outer, **located }
  end

  # The outer volume's own fields: the master directory block, the extent
  # records it holds for the extents and catalog files, whether its bitmap
  # allocates the embedded range, and the extents file's records, which show the
  # bad block file (file ID 5) if there is one.
  def hfs_volume_report(volume, mdb, m)
    name_length = mdb.getbyte(MDB_NAME_AT)
    extents_extents = hfs_extent_list(mdb.byteslice(MDB_EXTENTS_FILE_AT, HFS_EXTENT_RECORD_BYTES))
    {
      create_date: date_field(m[:create]), modify_date: date_field(m[:modify]), attributes: format("0x%04X", m[:attributes]),
      file_count: m[:file_count], volume_bitmap_sector: m[:bitmap_sector], alloc_ptr: m[:alloc_ptr], total_blocks: m[:total_blocks],
      block_size: m[:block_size], clump_size: m[:clump_size], first_block_sector: m[:first_block], next_catalog_id: m[:next_id],
      free_blocks: m[:free_blocks],
      name: name_length > MDB_NAME_MAX ? "(length #{name_length})" : mdb.byteslice(MDB_NAME_AT + 1, name_length).dup.force_encoding("MacRoman").encode("UTF-8"),
      extents_file_bytes: m[:extents_file_bytes], extents_file_extents: extents_extents.map(&:to_s),
      catalog_file_bytes: m[:catalog_file_bytes],
      catalog_file_extents: hfs_extent_list(mdb.byteslice(MDB_CATALOG_FILE_AT, HFS_EXTENT_RECORD_BYTES)).map(&:to_s),
      bitmap: hfs_bitmap_report(volume, m), extents_file_records: hfs_extents_records(volume, extents_extents, m[:block_size], m[:first_block])
    }
  end

  # Three (start, count) pairs of allocation blocks, the unused ones dropped.
  def hfs_extent_list(raw) = raw.unpack("n*").each_slice(2).reject { _2.zero? }.map { Extent.new(*_1) }

  def hfs_bitmap_report(volume, m)
    blocks = m[:total_blocks]
    bits = Bits.of(volume.read(m[:bitmap_sector] * 512, (blocks + 7) / 8), blocks)
    start = m[:embed_start]
    count = m[:embed_count]
    { set_bits: bits.set_count, first_clear: bits.first_clear, embedded_range_all_set: (start...start + count).all? { bits.set?(_1) } }
  end

  # The leaf records of the outer extents file as "file ID fork start: extents".
  # Its header node says where the first leaf is; nodes are 512 bytes.
  def hfs_extents_records(volume, extents, block_size, first_block)
    return [] if extents.empty?

    read_node = lambda do |number|
      at = number * HFS_EXTENTS_NODE_BYTES
      first = extents.first
      raise Malformed, "outer extents node #{number} is beyond its first extent" if at + HFS_EXTENTS_NODE_BYTES > first.count * block_size

      volume.read(first_block * 512 + first.start * block_size + at, HFS_EXTENTS_NODE_BYTES)
    end
    first_leaf = fields_at(read_node.(0), 14 + 10, 4, "N").first
    seen = Set.new
    node = first_leaf
    records = []
    until node.zero?
      raise Malformed, "outer extents leaf chain loops at #{node}" unless seen.add?(node) && seen.size <= MAX_HFS_EXTENT_NODES

      raw = read_node.(node)
      desc = descriptor(raw)
      records.concat(record_slices(raw, desc.num_records).map { hfs_extents_record(_1) })
      node = desc.flink
    end
    records
  end

  # Key length 7, fork type, file ID, start block; then three extents.
  def hfs_extents_record(slice)
    _, fork, file_id, start = fields_at(slice, 0, 8, "CCNn")
    "file #{file_id} #{fork.zero? ? 'data' : 'resource'} from #{start}: #{hfs_extent_list(bytes_at(slice, 8, HFS_EXTENT_RECORD_BYTES)).join(', ')}"
  end

  # ---- driver ----

  # Run one decoder, returning its report or the reason it failed; the
  # reason is also appended to `problems`.
  def section(problems, name)
    yield
  rescue Error => e
    problems << "#{name}: #{e.message}"
    { error: e.message }
  end

  # The decoded records of one tree; the catalog walk keeps its own statistics.
  def leaf_records(problems, name, volume, fork, tree, limit)
    section(problems, "#{name} records") do
      case name
      when :catalog then catalog_report(volume, fork, tree, limit)
      when :attributes then leaf_report(volume, fork, tree, limit) { attribute_report(parse_attribute(_1)) }
      when :extents then leaf_report(volume, fork, tree, limit) { extent_record(_1) }
      end
    end
  end

  def tree_sections(volume, header, problems, limit, index)
    forks = header[:forks]
    %i[extents catalog attributes].to_h do |name|
      fork = forks[name]
      next [name, { present: false }] if fork.empty?

      tree = section(problems, "#{name} tree") { tree_header_report(volume, fork) }
      report = tree.merge(present: true)
      report[:records] = leaf_records(problems, name, volume, fork, tree, limit) unless tree.key?(:error)
      report[:index] = section(problems, "#{name} index") { index_report(volume, fork, tree) } if index && !tree.key?(:error)
      [name, report]
    end
  end

  def volume_sections(volume, problems, limit, index)
    header = section(problems, "volume header") { read_header(volume, 1024) }
    return { boot_area: section(problems, "boot area") { boot_report(volume) }, volume_header: header } if header.key?(:error)

    volume = volume.with(block_size: header[:block_size])
    alternate = alternate_report(volume, header, problems)
    trees = tree_sections(volume, header, problems, limit, index)
    {
      boot_area: section(problems, "boot area") { boot_report(volume) },
      volume_header: header_report(header),
      alternate_header: alternate,
      layout: section(problems, "layout") { layout_report(volume, header) },
      allocation_bitmap: section(problems, "allocation bitmap") { bitmap_report(volume, header) },
      journal: (section(problems, "journal") { journal_report(volume, header) } if header[:attributes][13] == 1 && header[:journal_info_block] != 0),
      trees: trees,
      compression: compression_section(volume, header, trees, problems)
    }.compact
  end

  # Compressed files, when the volume has an attributes tree to hold decmpfs.
  def compression_section(volume, header, trees, problems)
    return unless %i[catalog attributes].all? { trees[_1][:present] && !trees[_1].key?(:error) }

    section(problems, "compression") { compression_report(volume, header[:forks], trees) }
  end

  def alternate_report(volume, header, problems)
    offset = header[:total_blocks] * header[:block_size] - 1024
    section(problems, "alternate header") do
      alt = read_header(volume, offset)
      { offset: offset, differing_fields: alternate_diff(header, alt) }
    end
  end

  def decode(io, limit: 20, index: false)
    image = Image.new(io)
    problems = []
    volume = Volume.new(image, 0, 512)
    wrapper = section(problems, "wrapper") { wrapper_report(volume) }
    inner = wrapper.is_a?(Hash) && wrapper.key?(:embedded_volume_offset) ? Volume.new(image, wrapper[:embedded_volume_offset], 512) : volume
    body = volume_sections(inner, problems, limit, index)
    { image_bytes: image.size, wrapper: wrapper, **body, problems: problems }.compact
  end

  # ---- text output ----

  def text_lines(value, indent = 0)
    pad = "  " * indent
    case value
    in Hash
      value.flat_map do |key, item|
        if scalar_list?(item) || !(item.is_a?(Hash) || item.is_a?(Array))
          ["#{pad}#{key}: #{inline(item)}"]
        else
          ["#{pad}#{key}:"] + text_lines(item, indent + 1)
        end
      end
    in Array
      value.flat_map { |item| ["#{pad}-"] + text_lines(item, indent + 1) }
    end
  end

  def scalar_list?(item) = item.is_a?(Array) && item.none? { _1.is_a?(Hash) || _1.is_a?(Array) }

  def inline(item) = item.is_a?(Array) ? "[#{item.join(', ')}]" : item.inspect.delete_prefix('"').delete_suffix('"')

  def main(argv)
    options = { json: false, records: 20, index: false }
    parser = OptionParser.new do |o|
      o.banner = "usage: fields.rb [--json] [--records N] [--index] IMAGE"
      o.on("--json", "print JSON") { options[:json] = true }
      o.on("--records N", Integer, "first N catalog records (default 20)") { options[:records] = _1 }
      o.on("--index", "walk the index nodes of each tree and compare each key with its child's first key") { options[:index] = true }
    end
    paths = parser.parse(argv)
    abort(parser.help) unless paths.size == 1

    report = File.open(paths.first, "rb") { decode(_1, limit: options[:records], index: options[:index]) }
    puts options[:json] ? JSON.pretty_generate(report) : text_lines(report)
    0
  rescue OptionParser::ParseError => e
    warn "fields.rb: #{e.message}"
    1
  rescue SystemCallError => e
    warn "fields.rb: #{e.message}"
    2
  end
end

exit(HfsPlusFields.main(ARGV)) if $PROGRAM_NAME == __FILE__
