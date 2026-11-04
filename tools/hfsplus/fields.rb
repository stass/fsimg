#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Decode the structures of an HFS Plus or HFSX image for the research record:
# the boot area, the volume header and its alternate, each B-tree's header node
# and node map, the private folders, Finder information and the first catalog
# records. The decoder is written from the format design, not from lib/, and
# treats the image as untrusted: every read is bounds-checked, every walk is
# bounded, and a section that cannot be decoded is reported in "problems" while
# the other sections still print.
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

    def fork_ranges(fork, offset, length)
      firsts = fork.extents.map(&:count).each_with_object([0]) { |count, acc| acc << acc.last + count }
      fork.extents.zip(firsts).filter_map do |ext, first|
        lo = [offset, first * block_size].max
        hi = [offset + length, (first + ext.count) * block_size].min
        [ext.start * block_size + lo - first * block_size, hi - lo] if hi > lo
      end
    end
  end

  module_function

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
    size, clump, blocks = bytes.unpack("Q>L>L>")
    extents = bytes.byteslice(16, 64).unpack("L>*").each_slice(2).map { Extent.new(*_1) }
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
      next_alloc, rsrc_clump, data_clump, next_cnid, writes, encodings = bytes.unpack("a2nNa4NNNNNNNNNNNNNNNQ>")
    forks = FORK_NAMES.each_with_index.to_h { |name, i| [name, parse_fork(bytes.byteslice(112 + 80 * i, 80))] }
    { signature: sig, version: version, attributes: attrs, last_mounted: last, journal_info_block: jib,
      dates: { create: create, modify: modify, backup: backup, checked: checked },
      file_count: files, folder_count: folders, block_size: bsize, total_blocks: total, free_blocks: free,
      next_allocation: next_alloc, rsrc_clump: rsrc_clump, data_clump: data_clump, next_catalog_id: next_cnid,
      write_count: writes, encodings_bitmap: encodings, finder_info: bytes.byteslice(80, 32), forks: forks }
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
    NodeDescriptor.new(*raw.unpack("NNcCn"))
  end

  # The record offsets of a node, validated: ascending, inside the node and
  # below the offset table.
  def record_offsets(raw, count)
    table = raw.bytesize - 2 * (count + 1)
    raise Malformed, "#{count} records do not fit a node of #{raw.bytesize}" if table < 14

    offsets = raw.byteslice(table, 2 * (count + 1)).unpack("n*").reverse
    ok = offsets.first == 14 && offsets.each_cons(2).all? { |a, b| a <= b } && offsets.last <= table
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
    node_size = probe.byteslice(32, 2).unpack1("n")
    raise Malformed, "node size #{node_size} is not a power of two from 512 to 32768" unless (512..32_768).cover?(node_size) && node_size.nobits?(node_size - 1)

    raw = read_node(volume, fork, 0, node_size)
    desc = descriptor(raw)
    depth, root, leaf_records, first_leaf, last_leaf, _, max_key, total, free, _, clump, btype, ctype, attrs =
      raw.byteslice(14, 42).unpack("nNNNNnnNNnNCCN")
    offsets = record_offsets(raw, desc.num_records)
    map = node_map_report(volume, fork, raw, desc, offsets, node_size, total, free)
    {
      node_kind: NODE_KINDS.fetch(desc.kind, desc.kind), node_size: node_size, record_offsets: offsets,
      depth: depth, root_node: root, leaf_records: leaf_records, first_leaf: first_leaf, last_leaf: last_leaf,
      max_key_length: max_key, total_nodes: total, free_nodes: free, clump_size: clump, tree_type: btype,
      key_compare_type: ctype, attributes: format("0x%08X", attrs),
      attribute_flags: flag_names(attrs, TREE_ATTRIBUTES), attribute_unknown_bits: unknown_bits(attrs, TREE_ATTRIBUTES),
      reserved1: raw.byteslice(44, 2).unpack1("n"), reserved3_zero: raw.byteslice(56, 64).count("\x00".b) == 64,
      user_data_zero: raw.byteslice(120, 128).count("\x00".b) == 128,
      fork_size_matches_nodes: fork.logical_size == total * node_size,
      node_map: map
    }
  end

  # The header node's map record, then any map nodes its forward links name.
  def node_map_report(volume, fork, raw, desc, offsets, node_size, total, free)
    first = raw.byteslice(offsets[2], offsets[3] - offsets[2])
    chain, maps = map_chain(volume, fork, desc.flink, node_size)
    bits = ([first] + maps).join.unpack1("B*")
    used = bits[0, total].count("1")
    {
      header_map_bytes: first.bytesize, map_nodes: chain, map_record_bytes: maps.map(&:bytesize),
      bits_available: bits.bytesize, used_bits: used, used_matches_total_minus_free: used == total - free,
      first_clear_bit: bits[0, total].index("0"), bits_beyond_total_set: bits[total..].to_s.count("1"),
      free_nodes_all_zero: free_nodes_zero?(volume, fork, bits[0, total], node_size)
    }
  end

  # Whether the unused nodes (map bit clear) hold only zero bytes; the first
  # MAX_ZERO_CHECK nodes are examined.
  def free_nodes_zero?(volume, fork, bits, node_size)
    bits[0, MAX_ZERO_CHECK].each_char.with_index.all? do |bit, number|
      bit == "1" || read_node(volume, fork, number, node_size).count("\x00".b) == node_size
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
    bytes = [(header[:total_blocks] + 7) / 8, fork.logical_size].min
    raise Malformed, "bitmap of #{bytes} bytes is over the #{MAX_BITMAP_BYTES} byte limit" if bytes > MAX_BITMAP_BYTES

    bits = volume.read_fork(fork, 0, bytes).unpack1("B*")[0, header[:total_blocks]]
    set = bits.count("1")
    { bitmap_bytes: fork.logical_size, set_bits: set, clear_bits: header[:total_blocks] - set,
      clear_matches_free_blocks: header[:total_blocks] - set == header[:free_blocks],
      first_clear_block: bits.index("0"), last_set_block: bits.rindex("1"),
      block_0_set: bits[0] == "1", last_block_set: bits[-1] == "1" }.merge(free_blocks_report(volume, header, bits))
  end

  # Whether the free blocks hold only zero bytes: stale data left by an earlier
  # volume or a formatter would show here. Examines free blocks until
  # MAX_FREE_BYTES have been read.
  def free_blocks_report(volume, header, bits)
    block_size = header[:block_size]
    free = bits.each_char.lazy.with_index.filter_map { |bit, number| number if bit == "0" }.first(MAX_FREE_BYTES / block_size)
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

    key_length, parent, name_length = slice.unpack("nNn")
    raise Malformed, "key length #{key_length} disagrees with name length #{name_length}" unless key_length == 6 + 2 * name_length && name_length <= 255

    raise Malformed, "key of #{key_length} bytes overruns its record" if 2 + key_length > slice.bytesize

    [parent, name_text(slice.byteslice(8, 2 * name_length)), (2 + key_length + 1) & ~1]
  end

  def bsd_report(bytes)
    owner, group, admin, owner_flags, mode, special = bytes.unpack("NNCCnN")
    { owner: owner, group: group, admin_flags: admin, owner_flags: owner_flags, mode: format("0%o", mode), special: special }
  end

  def folder_record(data)
    _, flags, valence, id, *dates = data.unpack("nnNNNNNNN")
    { type: "folder", flags: format("0x%04X", flags), valence: valence, id: id,
      dates: %i[create content_mod attribute_mod access backup].zip(dates).to_h { |k, v| [k, date_field(v)] },
      bsd: bsd_report(data.byteslice(32, 16)), user_info: hex(data.byteslice(48, 16)),
      finder_info: hex(data.byteslice(64, 16)), text_encoding: data.byteslice(80, 4).unpack1("N"),
      reserved: data.byteslice(84, 4).unpack1("N") }
  end

  def file_record(data)
    _, flags, _, id, *dates = data.unpack("nnNNNNNNN")
    user_info = data.byteslice(48, 16)
    { type: "file", flags: format("0x%04X", flags), id: id,
      dates: %i[create content_mod attribute_mod access backup].zip(dates).to_h { |k, v| [k, date_field(v)] },
      bsd: bsd_report(data.byteslice(32, 16)), file_type: four_chars(user_info.byteslice(0, 4)),
      creator: four_chars(user_info.byteslice(4, 4)), user_info: hex(user_info), finder_info: hex(data.byteslice(64, 16)),
      text_encoding: data.byteslice(80, 4).unpack1("N"),
      data_fork: fork_report(parse_fork(data.byteslice(88, 80))), rsrc_fork: fork_report(parse_fork(data.byteslice(168, 80))) }
  end

  def thread_record(data, kind)
    parent, name_length = data.byteslice(4, 6).unpack("Nn")
    raise Malformed, "thread name of #{name_length} units overruns its record" if 10 + 2 * name_length > data.bytesize

    { type: kind, parent: parent, name: name_text(data.byteslice(10, 2 * name_length)) }
  end

  RECORD_TYPES = { 1 => [88, :folder_record], 2 => [248, :file_record] }.freeze

  def catalog_record(slice)
    parent, name, data_at = catalog_key(slice)
    data = slice.byteslice(data_at..) || +""
    raise Malformed, "record for #{name.inspect} has no type" if data.bytesize < 2

    type = data.unpack1("n")
    body =
      case type
      when 1, 2
        want, decoder = RECORD_TYPES.fetch(type)
        raise Malformed, "record type #{type} has #{data.bytesize} bytes, expected #{want}" unless data.bytesize == want

        send(decoder, data)
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
    flags = block.unpack1("N")
    offset, size = block.byteslice(36, 16).unpack("Q>Q>")
    report = { info_flags: format("0x%08X", flags), device_signature: hex(block.byteslice(4, 32)), offset: offset, size: size }
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

    block_size = mdb.byteslice(20, 4).unpack1("N")
    first_block = mdb.byteslice(28, 2).unpack1("n")
    embed_sig = mdb.byteslice(124, 2).unpack1("n")
    start, count = mdb.byteslice(126, 4).unpack("nn")
    { hfs_block_size: block_size, first_allocation_block: first_block, embed_signature: format("0x%04X", embed_sig),
      embed_start: start, embed_count: count, embedded_volume_offset: first_block * 512 + start * block_size }
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
