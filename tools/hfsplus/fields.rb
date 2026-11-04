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
#   fields.rb [--json] [--records N] IMAGE
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

  Extent = Data.define(:start, :count)
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
      extents: fork.extents.reject { _1.count.zero? }.map { "#{_1.start}+#{_1.count}" } }
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
    chain, maps = map_chain(volume, fork, desc.flink, node_size)
    all = ([first] + maps).join
    bits = Bits.of(all, total)
    used = bits.set_count
    {
      header_map_bytes: first.bytesize, map_nodes: chain, map_record_bytes: maps.map(&:bytesize),
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
    while link != 0
      raise Malformed, "map node chain loops at #{link}" unless seen.add?(link)
      raise Malformed, "map node chain is longer than #{MAX_MAP_NODES}" if seen.size > MAX_MAP_NODES

      raw = read_node(volume, fork, link, node_size)
      desc = descriptor(raw)
      raise Malformed, "node #{link} on the map chain has kind #{desc.kind}" unless desc.kind == 2

      slices = record_slices(raw, desc.num_records)
      maps << slices.first.to_s
      link = desc.flink
    end
    [seen.to_a, maps]
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
    _, flags, _, id, *dates = fields_at(data, 0, 32, "nnNNNNNNN")
    user_info = bytes_at(data, 48, 16)
    { type: "file", flags: format("0x%04X", flags), id: id, dates: date_fields(dates),
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

  # ---- journal ----

  def journal_report(volume, header)
    block = volume.read(header[:journal_info_block] * header[:block_size], 180)
    flags = fields_at(block, 0, 4, "N").first
    offset, size = fields_at(block, 36, 16, "Q>Q>")
    report = { info_flags: format("0x%08X", flags), device_signature: hex(bytes_at(block, 4, 32)), offset: offset, size: size }
    report.merge(journal_header_report(volume, offset, size))
  end

  def journal_header_report(volume, offset, size)
    return { header: "outside the volume or empty" } if size < 44

    raw = volume.read(offset, 44)
    big = raw.unpack1("N") == 0x4a4e4c78
    little = raw.unpack1("V") == 0x4a4e4c78
    return { header: "no journal magic", first_bytes: hex(raw.byteslice(0, 16)) } unless big || little

    magic, endian, start, finish, jsize, blhdr, checksum, jhdr = raw.unpack(big ? "NNQ>Q>Q>NNN" : "VVQ<Q<Q<VVV")
    { byte_order: big ? "big" : "little", magic: format("0x%08X", magic), endian: format("0x%08X", endian), start: start,
      end: finish, size: jsize, blhdr_size: blhdr, checksum: format("0x%08X", checksum), jhdr_size: jhdr }
  end

  # ---- wrapper ----

  # A classic HFS master directory block with an embedded HFS Plus volume.
  def wrapper_report(volume)
    mdb = volume.read(1024, 162)
    return nil unless mdb.byteslice(0, 2) == "BD".b

    block_size = fields_at(mdb, 20, 4, "N").first
    first_block = fields_at(mdb, 28, 2, "n").first
    embed_sig = fields_at(mdb, 124, 2, "n").first
    start, count = fields_at(mdb, 126, 4, "nn")
    report = { hfs_block_size: block_size, first_allocation_block: first_block, embed_signature: format("0x%04X", embed_sig),
               embed_start: start, embed_count: count }
    # Only a wrapper that declares an embedded HFS Plus volume locates one.
    return report unless embed_sig == EMBED_SIGNATURE

    report.merge(embedded_volume_offset: first_block * 512 + start * block_size)
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

  def tree_sections(volume, header, problems, limit)
    forks = header[:forks]
    %i[extents catalog attributes].to_h do |name|
      fork = forks[name]
      next [name, { present: false }] if fork.empty?

      tree = section(problems, "#{name} tree") { tree_header_report(volume, fork) }
      report = tree.merge(present: true)
      report[:records] = section(problems, "catalog records") { catalog_report(volume, fork, tree, limit) } if name == :catalog && !tree.key?(:error)
      [name, report]
    end
  end

  def volume_sections(volume, problems, limit)
    header = section(problems, "volume header") { read_header(volume, 1024) }
    return { boot_area: section(problems, "boot area") { boot_report(volume) }, volume_header: header } if header.key?(:error)

    volume = volume.with(block_size: header[:block_size])
    alternate = alternate_report(volume, header, problems)
    {
      boot_area: section(problems, "boot area") { boot_report(volume) },
      volume_header: header_report(header),
      alternate_header: alternate,
      layout: section(problems, "layout") { layout_report(volume, header) },
      allocation_bitmap: section(problems, "allocation bitmap") { bitmap_report(volume, header) },
      journal: (section(problems, "journal") { journal_report(volume, header) } if header[:attributes][13] == 1 && header[:journal_info_block] != 0),
      trees: tree_sections(volume, header, problems, limit)
    }.compact
  end

  def alternate_report(volume, header, problems)
    offset = header[:total_blocks] * header[:block_size] - 1024
    section(problems, "alternate header") do
      alt = read_header(volume, offset)
      { offset: offset, differing_fields: alternate_diff(header, alt) }
    end
  end

  def decode(io, limit: 20)
    image = Image.new(io)
    problems = []
    volume = Volume.new(image, 0, 512)
    wrapper = section(problems, "wrapper") { wrapper_report(volume) }
    inner = wrapper.is_a?(Hash) && wrapper.key?(:embedded_volume_offset) ? Volume.new(image, wrapper[:embedded_volume_offset], 512) : volume
    body = volume_sections(inner, problems, limit)
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
    options = { json: false, records: 20 }
    parser = OptionParser.new do |o|
      o.banner = "usage: fields.rb [--json] [--records N] IMAGE"
      o.on("--json", "print JSON") { options[:json] = true }
      o.on("--records N", Integer, "first N catalog records (default 20)") { options[:records] = _1 }
    end
    paths = parser.parse(argv)
    abort(parser.help) unless paths.size == 1

    report = File.open(paths.first, "rb") { decode(_1, limit: options[:records]) }
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
