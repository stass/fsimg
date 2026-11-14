#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Print the container-level structure of an APFS image: every field of the
# block-0 container superblock, the checkpoint descriptor ring (each
# superblock and checkpoint-mapping block, the latest valid superblock
# marked), the ephemeral objects of the selected checkpoint, the container
# object map and its B-tree, the volume superblocks it resolves (address and
# checksum only), the space manager with its chunk-information chain,
# bitmap free-count recounts, internal pool and free-queue trees, and the
# reaper with its reap lists.
#
#   apfsdump.rb [--offset BYTES] [--json] IMAGE
#
# IMAGE is a raw container; --offset names where the container starts in it,
# for a container inside a partitioned disk image. --json prints the same
# document as JSON with stable keys, so dumps of two captures can be diffed.
#
# The dumper is written from Apple's "Apple File System Reference"
# (2020-06-22) and shares no code with lib/. Every object it reads has its
# Fletcher-64 checksum verified. A damaged image is reported, not trusted:
# each rule the image breaks is a `problem:` line at the end of the dump (a
# `problems` array in JSON) and the dump goes on with the next structure.
# Every loop is bounded by the size of the object it reads or by a fixed
# limit, never by a count read from the image alone. The exit status is 0
# for a clean dump and 1 when there is any problem or the image cannot be
# read as a container at all.
#
# The volume superblocks and file-system trees are another layer: a later
# module reads them through ObjectMap#lookup and BTree with its own codecs.
require "json"
require "optparse"
require "set"

module ApfsDump
  class Error < StandardError; end

  NX_MAGIC = 0x4253584E # "NXSB"
  MIN_BLOCK_SIZE = 4096
  MAX_BLOCK_SIZE = 65_536
  OBJ_HEADER_SIZE = 32

  # Bounds of this dumper, not of the format: no loop runs longer than these.
  MAX_RING_BLOCKS = 1 << 12
  MAX_TREE_NODES = 1 << 17
  MAX_REAP_LISTS = 1 << 12
  # Native ephemeral objects are a few blocks; this bounds one read.
  MAX_OBJECT_BLOCKS = 256
  # All ephemeral objects of a checkpoint together; native ones hold a few
  # blocks.
  MAX_EPHEMERAL_BYTES = 64 << 20
  # Leaf records one tree walk keeps.
  MAX_TREE_RECORDS = 1 << 20
  # Problems kept and printed; any further ones are only counted.
  MAX_PROBLEMS = 10_000
  # Chunk-information and address blocks of one device.
  MAX_CHAIN_BLOCKS = 1 << 16
  # A B-tree is at most 16 levels (root level 15).
  MAX_TREE_LEVEL = 15

  KIND_NX_SUPERBLOCK = 0x01
  KIND_BTREE = 0x02
  KIND_BTREE_NODE = 0x03
  KIND_SPACEMAN = 0x05
  KIND_SPACEMAN_CAB = 0x06
  KIND_SPACEMAN_CIB = 0x07
  KIND_SPACEMAN_FREE_QUEUE = 0x09
  KIND_OMAP = 0x0B
  KIND_CHECKPOINT_MAP = 0x0C
  KIND_FS = 0x0D
  KIND_NX_REAPER = 0x11
  KIND_NX_REAP_LIST = 0x12

  OBJECT_KINDS = {
    0x00 => "INVALID", 0x01 => "NX_SUPERBLOCK", 0x02 => "BTREE", 0x03 => "BTREE_NODE",
    0x05 => "SPACEMAN", 0x06 => "SPACEMAN_CAB", 0x07 => "SPACEMAN_CIB", 0x08 => "SPACEMAN_BITMAP",
    0x09 => "SPACEMAN_FREE_QUEUE", 0x0A => "EXTENT_LIST_TREE", 0x0B => "OMAP", 0x0C => "CHECKPOINT_MAP",
    0x0D => "FS", 0x0E => "FSTREE", 0x0F => "BLOCKREFTREE", 0x10 => "SNAPMETATREE", 0x11 => "NX_REAPER",
    0x12 => "NX_REAP_LIST", 0x13 => "OMAP_SNAPSHOT", 0x14 => "EFI_JUMPSTART", 0x15 => "FUSION_MIDDLE_TREE",
    0x16 => "NX_FUSION_WBC", 0x17 => "NX_FUSION_WBC_LIST", 0x18 => "ER_STATE", 0x19 => "GBITMAP",
    0x1A => "GBITMAP_TREE", 0x1B => "GBITMAP_BLOCK", 0x1C => "ER_RECOVERY_BLOCK", 0x1D => "SNAP_META_EXT",
    0x1E => "INTEGRITY_META", 0x1F => "FEXT_TREE", 0x20 => "RESERVED_20", 0xFF => "TEST"
  }.freeze

  STORAGE_MASK = 0xC000_0000
  STORAGE_VIRTUAL = 0x0000_0000
  STORAGE_EPHEMERAL = 0x8000_0000
  STORAGE_PHYSICAL = 0x4000_0000
  STORAGE = { STORAGE_VIRTUAL => "virtual", STORAGE_EPHEMERAL => "ephemeral", STORAGE_PHYSICAL => "physical" }.freeze
  TYPE_FLAGS = { 0x2000_0000 => "NOHEADER", 0x1000_0000 => "ENCRYPTED", 0x0800_0000 => "NONPERSISTENT" }.freeze

  NX_FEATURES = { 0x1 => "DEFRAG", 0x2 => "LCFD" }.freeze
  NX_INCOMPAT = { 0x1 => "VERSION1", 0x2 => "VERSION2", 0x100 => "FUSION" }.freeze
  NX_FLAGS = { 0x1 => "RESERVED_1", 0x2 => "RESERVED_2", 0x4 => "CRYPTO_SW" }.freeze
  CHECKPOINT_FLAGS = { 0x1 => "LAST" }.freeze
  CHECKPOINT_MAP_LAST = 0x1
  OMAP_FLAGS = {
    0x1 => "MANUALLY_MANAGED", 0x2 => "ENCRYPTING", 0x4 => "DECRYPTING", 0x8 => "KEYROLLING",
    0x10 => "CRYPTO_GENERATION"
  }.freeze
  OMAP_VAL_FLAGS = { 0x1 => "DELETED", 0x2 => "SAVED", 0x4 => "ENCRYPTED", 0x8 => "NOHEADER", 0x10 => "CRYPTO_GENERATION" }.freeze
  OMAP_VAL_DELETED = 0x1
  NODE_FLAGS = {
    0x1 => "ROOT", 0x2 => "LEAF", 0x4 => "FIXED_KV_SIZE", 0x8 => "HASHED", 0x10 => "NOHEADER",
    0x8000 => "CHECK_KOFF_INVAL"
  }.freeze
  NODE_ROOT = 0x1
  NODE_LEAF = 0x2
  NODE_FIXED = 0x4
  TREE_FLAGS = {
    0x1 => "UINT64_KEYS", 0x2 => "SEQUENTIAL_INSERT", 0x4 => "ALLOW_GHOSTS", 0x8 => "EPHEMERAL",
    0x10 => "PHYSICAL", 0x20 => "NONPERSISTENT", 0x40 => "KV_NONALIGNED", 0x80 => "HASHED", 0x100 => "NOHEADER"
  }.freeze
  TREE_ALLOW_GHOSTS = 0x4
  BTOFF_INVALID = 0xFFFF
  SM_FLAGS = { 0x1 => "VERSIONED" }.freeze
  REAPER_FLAGS = { 0x1 => "BHM_FLAG", 0x2 => "CONTINUE" }.freeze
  NRLE_FLAGS = { 0x1 => "VALID", 0x2 => "REAP_ID_RECORD", 0x4 => "CALL", 0x8 => "COMPLETION", 0x10 => "CLEANUP" }.freeze
  FREE_QUEUE_NAMES = %w[internal-pool main tier2].freeze
  DEVICE_NAMES = %w[main tier2].freeze

  # Field tables: [name, byte offset, codec]. A codec is an unpack directive
  # (one value, or an array when it ends in a count), a Symbol naming an
  # entry of Layout::CODECS, or [table, stride, count] for an array of
  # nested structures.
  OBJ_FIELDS = [
    [:cksum, 0, "Q<"], [:oid, 8, "Q<"], [:xid, 16, "Q<"], [:type, 24, "L<"], [:subtype, 28, "L<"]
  ].freeze

  NX_FIELDS = [
    [:magic, 32, "L<"], [:block_size, 36, "L<"], [:block_count, 40, "Q<"],
    [:features, 48, "Q<"], [:readonly_compatible_features, 56, "Q<"], [:incompatible_features, 64, "Q<"],
    [:uuid, 72, :uuid], [:next_oid, 88, "Q<"], [:next_xid, 96, "Q<"],
    [:xp_desc_blocks, 104, "L<"], [:xp_data_blocks, 108, "L<"], [:xp_desc_base, 112, "Q<"],
    [:xp_data_base, 120, "Q<"], [:xp_desc_next, 128, "L<"], [:xp_data_next, 132, "L<"],
    [:xp_desc_index, 136, "L<"], [:xp_desc_len, 140, "L<"], [:xp_data_index, 144, "L<"],
    [:xp_data_len, 148, "L<"], [:spaceman_oid, 152, "Q<"], [:omap_oid, 160, "Q<"], [:reaper_oid, 168, "Q<"],
    [:test_type, 176, "L<"], [:max_file_systems, 180, "L<"], [:fs_oid, 184, "Q<100"],
    [:counters, 984, "Q<32"], [:blocked_out_prange, 1240, :prange], [:evict_mapping_tree_oid, 1256, "Q<"],
    [:flags, 1264, "Q<"], [:efi_jumpstart, 1272, "Q<"], [:fusion_uuid, 1280, :uuid],
    [:keylocker, 1296, :prange], [:ephemeral_info, 1312, "Q<4"], [:test_oid, 1344, "Q<"],
    [:fusion_mt_oid, 1352, "Q<"], [:fusion_wbc_oid, 1360, "Q<"], [:fusion_wbc, 1368, :prange],
    [:newest_mounted_version, 1384, "Q<"], [:mkb_locker, 1392, :prange]
  ].freeze

  CHECKPOINT_MAP_FIELDS = [[:flags, 32, "L<"], [:count, 36, "L<"]].freeze
  CHECKPOINT_MAPPING_SIZE = 40
  CHECKPOINT_MAPPING_FIELDS = [
    [:type, 0, "L<"], [:subtype, 4, "L<"], [:size, 8, "L<"], [:pad, 12, "L<"],
    [:fs_oid, 16, "Q<"], [:oid, 24, "Q<"], [:paddr, 32, "Q<"]
  ].freeze

  OMAP_FIELDS = [
    [:flags, 32, "L<"], [:snap_count, 36, "L<"], [:tree_type, 40, "L<"], [:snapshot_tree_type, 44, "L<"],
    [:tree_oid, 48, "Q<"], [:snapshot_tree_oid, 56, "Q<"], [:most_recent_snap, 64, "Q<"],
    [:pending_revert_min, 72, "Q<"], [:pending_revert_max, 80, "Q<"]
  ].freeze

  NODE_HEADER_END = 56
  TREE_INFO_SIZE = 40
  NODE_FIELDS = [
    [:flags, 32, "S<"], [:level, 34, "S<"], [:nkeys, 36, "L<"], [:table_space, 40, :nloc],
    [:free_space, 44, :nloc], [:key_free_list, 48, :nloc], [:val_free_list, 52, :nloc]
  ].freeze
  # The btree_info_t footer, at the end of a root node.
  TREE_INFO_FIELDS = [
    [:flags, 0, "L<"], [:node_size, 4, "L<"], [:key_size, 8, "L<"], [:val_size, 12, "L<"],
    [:longest_key, 16, "L<"], [:longest_val, 20, "L<"], [:key_count, 24, "Q<"], [:node_count, 32, "Q<"]
  ].freeze

  DEVICE_FIELDS = [
    [:block_count, 0, "Q<"], [:chunk_count, 8, "Q<"], [:cib_count, 16, "L<"], [:cab_count, 20, "L<"],
    [:free_count, 24, "Q<"], [:addr_offset, 32, "L<"], [:reserved, 36, "L<"], [:reserved2, 40, "Q<"]
  ].freeze
  FREE_QUEUE_FIELDS = [
    [:count, 0, "Q<"], [:tree_oid, 8, "Q<"], [:oldest_xid, 16, "Q<"], [:tree_node_limit, 24, "S<"],
    [:pad16, 26, "S<"], [:pad32, 28, "L<"], [:reserved, 32, "Q<"]
  ].freeze
  ALLOCATION_ZONE_FIELDS = [
    [:zone_start, 0, "Q<"], [:zone_end, 8, "Q<"], [:previous_boundaries, 16, "Q<14"],
    [:zone_id, 128, "S<"], [:previous_boundary_index, 130, "S<"], [:reserved, 132, "L<"]
  ].freeze
  ALLOCATION_ZONES_PER_DEVICE = 8
  SPACEMAN_FIELDS = [
    [:block_size, 32, "L<"], [:blocks_per_chunk, 36, "L<"], [:chunks_per_cib, 40, "L<"],
    [:cibs_per_cab, 44, "L<"], [:dev, 48, [DEVICE_FIELDS, 48, 2]], [:flags, 144, "L<"],
    [:ip_bm_tx_multiplier, 148, "L<"], [:ip_block_count, 152, "Q<"], [:ip_bm_size_in_blocks, 160, "L<"],
    [:ip_bm_block_count, 164, "L<"], [:ip_bm_base, 168, "Q<"], [:ip_base, 176, "Q<"],
    [:fs_reserve_block_count, 184, "Q<"], [:fs_reserve_alloc_count, 192, "Q<"],
    [:fq, 200, [FREE_QUEUE_FIELDS, 40, 3]], [:ip_bm_free_head, 320, "S<"], [:ip_bm_free_tail, 322, "S<"],
    [:ip_bm_xid_offset, 324, "L<"], [:ip_bitmap_offset, 328, "L<"], [:ip_bm_free_next_offset, 332, "L<"],
    [:version, 336, "L<"], [:struct_size, 340, "L<"],
    [:datazone, 344, [ALLOCATION_ZONE_FIELDS, 136, 2 * ALLOCATION_ZONES_PER_DEVICE]]
  ].freeze
  # Internal-pool ring blocks are indexed by u16; 0xffff is the end marker.
  IP_BM_INDEX_INVALID = 0xFFFF
  IP_BM_BLOCK_COUNT_MAX = 0xFFFE

  ADDRESS_BLOCK_FIELDS = [[:index, 32, "L<"], [:count, 36, "L<"]].freeze
  CHUNK_INFO_SIZE = 32
  CHUNK_INFO_FIELDS = [
    [:xid, 0, "Q<"], [:addr, 8, "Q<"], [:block_count, 16, "L<"], [:free_count, 20, "L<"], [:bitmap_addr, 24, "Q<"]
  ].freeze

  REAPER_FIELDS = [
    [:next_reap_id, 32, "Q<"], [:completed_id, 40, "Q<"], [:head, 48, "Q<"], [:tail, 56, "Q<"],
    [:flags, 64, "L<"], [:rlcount, 68, "L<"], [:reap_type, 72, "L<"], [:reap_size, 76, "L<"],
    [:reap_fs_oid, 80, "Q<"], [:reap_oid, 88, "Q<"], [:reap_xid, 96, "Q<"], [:nrle_flags, 104, "L<"],
    [:state_buffer_size, 108, "L<"]
  ].freeze
  REAPER_STATE_OFFSET = 112
  REAP_LIST_FIELDS = [
    [:next, 32, "Q<"], [:flags, 40, "L<"], [:max, 44, "L<"], [:count, 48, "L<"], [:first, 52, "L<"],
    [:last, 56, "L<"], [:free, 60, "L<"]
  ].freeze
  REAP_LIST_ENTRIES_OFFSET = 64
  REAP_LIST_ENTRY_SIZE = 40
  REAP_LIST_ENTRY_FIELDS = [
    [:next, 0, "L<"], [:flags, 4, "L<"], [:type, 8, "L<"], [:size, 12, "L<"], [:fs_oid, 16, "Q<"],
    [:oid, 24, "Q<"], [:xid, 32, "Q<"]
  ].freeze

  # Fletcher-64 as APFS uses it: over the object's bytes from offset 8 to its
  # end, read as little-endian 32-bit words, both sums modulo 2^32 - 1.
  module Fletcher64
    MOD = 0xFFFF_FFFF

    module_function

    def compute(object)
      s1, s2 = object.byteslice(8..).unpack("L<*").reduce([0, 0]) do |(a, b), word|
        a = (a + word) % MOD
        [a, (b + a) % MOD]
      end
      c1 = MOD - ((s1 + s2) % MOD)
      c2 = MOD - ((s1 + c1) % MOD)
      (c2 << 32) | c1
    end
  end

  # Decoding of field tables and naming of flag words.
  module Layout
    CODECS = {
      uuid: [16, ->(b) { b.unpack("H8H4H4H4H12").join("-") }],
      prange: [16, ->(b) { b.unpack("q<Q<").then { |start, count| { start_paddr: start, block_count: count } } }],
      nloc: [4, ->(b) { b.unpack("S<S<").then { |off, len| { off:, len: } } }]
    }.freeze

    module_function

    # Decodes `table` from `bytes` at `base`. Raises Error when the bytes end
    # before a field does.
    def decode(bytes, table, base: 0) = table.to_h { |name, off, codec| [name, field(bytes, base + off, codec)] }

    def field(bytes, off, codec)
      value =
        case codec
        in Symbol
          CODECS.fetch(codec).then { |len, fn| fn.(slice(bytes, off, len)) }
        in [Array => table, Integer => stride, Integer => count]
          Array.new(count) { decode(bytes, table, base: off + _1 * stride) }
        in /\d\z/ # a repeat count: "Q<4" is an array of four values
          bytes.unpack("@#{off}#{codec}")
        else
          bytes.unpack1("@#{off}#{codec}")
        end
      raise Error, "structure ends before byte #{off}" if value.nil? || (value.is_a?(Array) && value.include?(nil))

      value
    rescue ArgumentError
      raise Error, "structure ends before byte #{off}"
    end

    def slice(bytes, off, len)
      raise Error, "structure ends before byte #{off + len}" if off + len > bytes.bytesize

      bytes.byteslice(off, len)
    end

    ARRAY_WIDTHS = { "Q<" => 8, "S<" => 2 }.freeze

    # `count` values of `directive` at `off` in `bytes`, or Error when the
    # bytes end first.
    def array(bytes, off:, count:, directive:)
      slice(bytes, off, count * ARRAY_WIDTHS.fetch(directive)).unpack("#{directive}#{count}")
    end

    def names(table, word)
      known = table.filter_map { |bit, name| name if word.anybits?(bit) }
      unknown = word & ~table.keys.reduce(0, :|)
      (known + (unknown.zero? ? [] : [format("unknown(0x%x)", unknown)])).join(",")
    end

    # `fields` with a `<name>_names` entry after each flag word in `tables`.
    def with_names(fields, tables)
      fields.each_with_object({}) do |(key, value), out|
        out[key] = value
        out[:"#{key}_names"] = names(tables.fetch(key), value) if tables.key?(key)
      end
    end

    def type_name(type)
      kind = OBJECT_KINDS.fetch(type & 0xFFFF) { format("unknown(0x%x)", type & 0xFFFF) }
      storage = STORAGE.fetch(type & STORAGE_MASK, "invalid-storage")
      flags = names(TYPE_FLAGS, type & ~STORAGE_MASK & 0xFFFF_0000)
      [kind, storage, flags].reject(&:empty?).join(" ")
    end

    def kind_name(kind) = OBJECT_KINDS.fetch(kind & 0xFFFF) { format("unknown(0x%x)", kind & 0xFFFF) }

    # Zero bits among the first `bits` bits of a bitmap, least significant bit
    # of each byte first: the free blocks it records.
    def zero_bits(bitmap, bits) = bitmap.unpack1("b*")[0, bits].count("0")
  end

  # One object as read from the image: its block address, bytes, decoded
  # header, and whether its stored checksum matches.
  Obj = Data.define(:addr, :bytes, :header, :computed) do
    def self.from(addr, bytes) = new(addr:, bytes:, header: Layout.decode(bytes, OBJ_FIELDS), computed: Fletcher64.compute(bytes))

    def cksum_ok? = header[:cksum] == computed
    def oid = header[:oid]
    def xid = header[:xid]
    def kind = header[:type] & 0xFFFF
    def storage = header[:type] & STORAGE_MASK
    def subtype = header[:subtype]

    def summary
      { addr:, **header, cksum_ok: cksum_ok?, type_name: Layout.type_name(header[:type]),
        subtype_name: Layout.kind_name(subtype) }
    end

    # The summary followed by `fields`, which may not reuse a header key.
    def document(fields)
      clash = summary.keys & fields.keys
      raise ArgumentError, "fields #{clash.join(', ')} shadow the object header" unless clash.empty?

      summary.merge(fields)
    end

    # The rules of the reference this object breaks, as messages.
    def header_problems(kind:, storage:, subtype: nil, oid: nil)
      [
        ("kind #{Layout.kind_name(self.kind)}, expected #{Layout.kind_name(kind)}" if self.kind != kind),
        ("storage #{STORAGE.fetch(self.storage, 'invalid')}, expected #{STORAGE.fetch(storage)}" if self.storage != storage),
        ("subtype #{Layout.kind_name(self.subtype)}, expected #{Layout.kind_name(subtype)}" if subtype && self.subtype != subtype),
        ("oid #{self.oid}, expected #{oid}" if oid && self.oid != oid)
      ].compact
    end
  end

  # Bounds-checked access to a container that starts `offset` bytes into
  # the image file.
  class Image
    attr_reader :size

    def initialize(io, offset: 0)
      raise Error, "offset #{offset} is outside the image (#{io.size} bytes)" if offset.negative? || offset > io.size

      @io = io
      @offset = offset
      @size = io.size - offset
    end

    def read(off, len)
      raise Error, "#{len} bytes at #{off} are outside the image (#{@size} bytes)" if off.negative? || len.negative? || off + len > @size

      data = @io.pread(len, @offset + off)
      raise Error, "short read of #{len} bytes at #{off}" if data.bytesize != len

      data.b
    end
  end

  # How a tree's records decode: fixed key and value sizes (nil in a tree of
  # variable-size records) and `record`, called with the key bytes and the
  # value bytes (nil for a ghost) and returning the record's fields.
  Codec = Data.define(:name, :key_size, :value_size, :record)

  OMAP_CODEC = Codec.new(
    name: "omap", key_size: 16, value_size: 16,
    record: lambda do |key, value|
      oid, xid = key.unpack("Q<Q<")
      flags, size, paddr = value ? value.unpack("L<L<q<") : [nil, nil, nil]
      { oid:, xid:, flags:, flags_names: flags && Layout.names(OMAP_VAL_FLAGS, flags), size:, paddr: }
    end
  )

  # A free-queue record: a run of `count` blocks freed at `xid`. A ghost
  # record has no value and stands for one block.
  FREE_QUEUE_CODEC = Codec.new(
    name: "free queue", key_size: 16, value_size: 8,
    record: lambda do |key, value|
      xid, paddr = key.unpack("Q<q<")
      { xid:, paddr:, count: value ? value.unpack1("Q<") : 1, ghost: value.nil? }
    end
  )

  # An object map's records indexed by OID, resolving virtual OIDs. A record
  # without a value (a ghost) maps nothing.
  class ObjectMap
    def initialize(records)
      @by_oid = records.reject { _1[:paddr].nil? }.group_by { _1[:oid] }
    end

    # The newest record of `oid` not above `xid`, or nil when there is none.
    def find(oid, xid) = @by_oid.fetch(oid, []).select { _1[:xid] <= xid }.max_by { _1[:xid] }
  end

  # Walks a B-tree from its root and returns its nodes and leaf records.
  # `fetch` maps a child OID to its Obj (raising Error when it cannot);
  # `problem` receives a message for each rule a node breaks. The walk is
  # bounded: levels fall by one per step from at most MAX_TREE_LEVEL, a node
  # is visited once, and at most MAX_TREE_NODES nodes are read.
  class BTree
    Entry = Data.define(:key, :value)

    def initialize(fetch:, problem:, codec:, what:, block_size:, storage:, subtype:)
      @fetch = fetch
      @problem = problem
      @codec = codec
      @what = what
      @block_size = block_size
      @storage = storage
      @subtype = subtype
    end

    def walk(root_oid)
      @nodes = []
      @records = []
      @seen = Set.new
      @full = false
      visit(root_oid, level: nil, info: nil)
      { nodes: @nodes, records: @records }
    end

    private

    # Reports `text` and returns nil, whatever the callback returns, so a
    # rejected entry or child is `nil` to the code that skips it.
    def problem(text)
      @problem.("#{@what}: #{text}")
      nil
    end

    # `level` is the level the parent implies, nil for the root; `info` is
    # the root's footer, which fixes the record sizes of every node.
    def visit(oid, level:, info:)
      return problem("node limit #{MAX_TREE_NODES} reached at oid #{oid}") if @seen.size >= MAX_TREE_NODES
      return problem("node #{oid} is reached twice") unless @seen.add?(oid)

      obj = @fetch.(oid)
      root = info.nil?
      obj.header_problems(kind: root ? KIND_BTREE : KIND_BTREE_NODE, storage: @storage, subtype: @subtype)
         .each { problem("node #{oid}: #{_1}") }
      node = Layout.decode(obj.bytes, NODE_FIELDS)
      info = tree_info(oid, obj) if root
      return unless node_shape_ok?(oid, node, level:, root:)

      entries = entries(oid, obj.bytes, node, info:, root:)
      @nodes << node_summary(obj, node, info:, root:, entries:)
      node[:level].zero? ? leaf(entries) : children(entries, level: node[:level], info:)
    rescue Error => e
      problem("node #{oid}: #{e.message}")
    end

    def tree_info(oid, obj)
      info = Layout.decode(obj.bytes, TREE_INFO_FIELDS, base: @block_size - TREE_INFO_SIZE)
      @ghosts = info[:flags].anybits?(TREE_ALLOW_GHOSTS)
      problem("root #{oid}: footer node size #{info[:node_size]}, expected #{@block_size}") if info[:node_size] != @block_size
      if @codec.key_size && [info[:key_size], info[:val_size]] != [@codec.key_size, @codec.value_size]
        problem("root #{oid}: footer key and value sizes #{info[:key_size]}/#{info[:val_size]}, " \
                "expected #{@codec.key_size}/#{@codec.value_size}")
      end
      info
    end

    # Reports every flag and level rule the node breaks. Returns whether its
    # level is usable: a wrong level would let the walk recurse without bound.
    def node_shape_ok?(oid, node, level:, root:)
      as_implied = level.nil? || node[:level] == level
      in_bound = node[:level] <= MAX_TREE_LEVEL
      problems = [
        ("ROOT flag #{node[:flags].anybits?(NODE_ROOT)}, expected #{root}" if node[:flags].anybits?(NODE_ROOT) != root),
        ("LEAF flag disagrees with level #{node[:level]}" if node[:flags].anybits?(NODE_LEAF) != node[:level].zero?),
        ("level #{node[:level]}, expected #{level}" unless as_implied),
        ("level #{node[:level]} is above #{MAX_TREE_LEVEL}" unless in_bound)
      ].compact
      problems.each { problem("node #{oid}: #{_1}") }
      as_implied && in_bound
    end

    # The node's entries, each a key slice and a value slice (nil for a
    # ghost). An entry whose ranges leave the key or value area is reported
    # and skipped. Offsets are those of the reference: table of contents at
    # 56 + table_space.off, keys from its end, values back from the end of
    # the value area (the node end, or the footer start in a root).
    def entries(oid, bytes, node, info:, root:)
      fixed = node[:flags].anybits?(NODE_FIXED)
      width = fixed ? 4 : 8
      toc = NODE_HEADER_END + node[:table_space][:off]
      keys = toc + node[:table_space][:len]
      values_end = @block_size - (root ? TREE_INFO_SIZE : 0)
      if keys > values_end
        problem("node #{oid}: key area starts at #{keys}, past the value area end #{values_end}")
        return []
      end

      capacity = node[:table_space][:len] / width
      if node[:nkeys] > capacity
        problem("node #{oid}: #{node[:nkeys]} keys, the table of contents holds #{capacity}")
      end
      Array.new([node[:nkeys], capacity].min) do |i|
        locate(oid, bytes, toc + i * width, fixed:, leaf: node[:level].zero?, info:, area: keys...values_end)
      end.compact
    end

    def locate(oid, bytes, at, fixed:, leaf:, info:, area:)
      # A fixed-size entry (kvoff_t) holds two offsets and takes its lengths
      # from the footer; an index value is a child OID. A variable entry
      # (kvloc_t) holds offset and length of both.
      if fixed
        k_off, v_off = bytes.unpack("@#{at}S<S<")
        k_len = info[:key_size]
        v_len = leaf ? info[:val_size] : 8
      else
        k_off, k_len, v_off, v_len = bytes.unpack("@#{at}S<S<S<S<")
      end
      key_at = area.begin + k_off
      return problem("node #{oid}: key at #{key_at}+#{k_len} leaves the key area") unless inside?(area, key_at, k_len)
      if v_off == BTOFF_INVALID
        return problem("node #{oid}: ghost record in a tree that does not allow ghosts") unless @ghosts

        return Entry.new(key: bytes.byteslice(key_at, k_len), value: nil)
      end

      value_at = area.end - v_off
      return problem("node #{oid}: value at #{value_at}+#{v_len} leaves the value area") unless inside?(area, value_at, v_len)

      Entry.new(key: bytes.byteslice(key_at, k_len), value: bytes.byteslice(value_at, v_len))
    end

    def inside?(area, at, len) = at >= area.begin && at + len <= area.end

    def node_summary(obj, node, info:, root:, entries:)
      summary = obj.document({ **Layout.with_names(node, flags: NODE_FLAGS), entries: entries.size })
      summary[:info] = Layout.with_names(info, flags: TREE_FLAGS) if root
      summary
    end

    def leaf(entries)
      entries.each do |entry|
        next problem("a #{entry.key.bytesize}/#{entry.value&.bytesize}-byte record is not a #{@codec.name} record") unless fits?(entry)
        break full if @records.size >= MAX_TREE_RECORDS

        @records << @codec.record.(entry.key, entry.value)
      end
    end

    # Reports, once per walk, that the record limit stopped keeping records.
    def full
      problem("record limit #{MAX_TREE_RECORDS} reached; further records are not kept") unless @full
      @full = true
      nil
    end

    # Whether an entry has the codec's fixed sizes; a ghost has no value.
    def fits?(entry)
      return true unless @codec.key_size

      entry.key.bytesize == @codec.key_size && (entry.value.nil? || entry.value.bytesize == @codec.value_size)
    end

    def children(entries, level:, info:)
      entries.each do |entry|
        next problem("index entry without a child") if entry.value.nil? || entry.value.bytesize < 8

        visit(entry.value.unpack1("Q<"), level: level - 1, info:)
      end
    end
  end

  # Reads the container layer of an image into one document (nested hashes
  # with stable keys) and collects every problem met on the way.
  class Container
    attr_reader :problems

    def initialize(image)
      @image = image
      @problems = []
      @unrecorded = 0
      @ephemeral_bytes = 0
      @ephemeral = {}
      @claimed = Set.new
    end

    def dump
      doc = { image: { size: @image.size } }
      doc[:superblock] = block_zero
      doc[:checkpoints] = section(:checkpoints) { checkpoints }
      doc[:omap] = section(:omap) { container_omap }
      doc[:volumes] = section(:volumes) { volumes }
      doc[:spaceman] = section(:spaceman) { spaceman }
      doc[:reaper] = section(:reaper) { reaper }
      @problems << "#{@unrecorded} further problems are not recorded" if @unrecorded.positive?
      doc[:problems] = @problems
      doc
    end

    private

    # Records a problem; past MAX_PROBLEMS it is only counted, and #dump
    # ends the list with that count.
    def problem(text)
      if @problems.size < MAX_PROBLEMS
        @problems << text
      else
        @unrecorded += 1
      end
      nil
    end

    # Records a problem and returns the section value that stands for it.
    def failure(text, **fields)
      problem(text)
      { **fields, error: text }
    end

    # A section boundary: a failure inside one structure is recorded and the
    # dump goes on with the next.
    def section(name)
      yield
    rescue StandardError => e
      problem("#{name}: #{e.is_a?(Error) ? '' : "#{e.class}: "}#{e.message}")
      { error: e.message }
    end

    # ---- objects --------------------------------------------------------

    def in_container?(addr, blocks) = addr.positive? && blocks.positive? && addr + blocks <= @block_count

    # Reads the object at `addr`, `blocks` long, and reports a checksum
    # mismatch. Block 0 is read through #block_zero.
    def read_object(addr, what, blocks: 1)
      raise Error, "#{what}: blocks #{addr}+#{blocks} are outside the container (#{@block_count} blocks)" unless in_container?(addr, blocks)

      checked(Obj.from(addr, @image.read(addr * @block_size, blocks * @block_size)), what)
    end

    def checked(obj, what)
      return obj if obj.cksum_ok?

      problem(format("%s at block %d: checksum 0x%016x, computed 0x%016x", what, obj.addr, obj.header[:cksum], obj.computed))
      obj
    end

    # Marks the blocks of `span` as read for one structure. Returns false,
    # claiming nothing, when any of them already was. The ephemeral objects
    # and the space manager's chain claim their blocks, so naming one block
    # many times cannot multiply their work; every other read is bounded by
    # a fixed limit of this dumper.
    def claim(span)
      return false if span.any? { @claimed.include?(_1) }

      @claimed.merge(span)
      true
    end

    def raw_block(addr, what)
      raise Error, "#{what}: block #{addr} is outside the container (#{@block_count} blocks)" unless in_container?(addr, 1)

      @image.read(addr * @block_size, @block_size)
    end

    def check_header(obj, what, **expected)
      obj.header_problems(**expected).each { problem("#{what} at block #{obj.addr}: #{_1}") }
    end

    def ephemeral(oid, what)
      @ephemeral.fetch(oid) { raise Error, "#{what}: ephemeral oid #{oid} has no mapping in the selected checkpoint" }
    end

    def superblock_fields(obj)
      fields = Layout.decode(obj.bytes, NX_FIELDS)
      Layout.with_names(fields, features: NX_FEATURES, incompatible_features: NX_INCOMPAT, flags: NX_FLAGS)
    end

    # ---- block 0 ---------------------------------------------------------

    # Block 0 fixes the block size and count every later read is judged by;
    # an image without a usable one is not dumped further.
    def block_zero
      @block_size = @image.read(0, MIN_BLOCK_SIZE).unpack1("@36L<")
      unless @block_size.between?(MIN_BLOCK_SIZE, MAX_BLOCK_SIZE) && (@block_size & (@block_size - 1)).zero?
        raise Error, "block 0: block size #{@block_size} is not a power of two from #{MIN_BLOCK_SIZE} to #{MAX_BLOCK_SIZE}"
      end

      obj = Obj.from(0, @image.read(0, @block_size))
      sb = superblock_fields(obj)
      raise Error, format("block 0: magic 0x%08x is not NXSB", sb[:magic]) if sb[:magic] != NX_MAGIC

      problem(format("block 0: checksum 0x%016x, computed 0x%016x", obj.header[:cksum], obj.computed)) unless obj.cksum_ok?
      check_header(obj, "block 0", kind: KIND_NX_SUPERBLOCK, storage: STORAGE_EPHEMERAL, oid: 1)
      @block_count = sb[:block_count]
      if @block_count * @block_size > @image.size
        problem("block 0: the container is #{@block_count * @block_size} bytes, the image holds #{@image.size}")
      end
      @sb = obj.document(sb)
    end

    # ---- checkpoints -----------------------------------------------------

    def checkpoints
      desc_blocks = @sb[:xp_desc_blocks]
      return failure("checkpoints: the descriptor area is noncontiguous; not decoded") if desc_blocks.anybits?(0x8000_0000)

      ring = Array.new([desc_blocks, MAX_RING_BLOCKS].min) { descriptor(_1, @sb[:xp_desc_base] + _1) }
      problem("checkpoints: descriptor area of #{desc_blocks} blocks; only #{MAX_RING_BLOCKS} read") if desc_blocks > MAX_RING_BLOCKS
      latest = select_latest(ring)
      area = ring.map { |slot| slot[:role] == "superblock" ? { **slot.except(:superblock), latest: slot[:index] == latest&.dig(:index) } : slot }
      return { descriptor_area: area, selected: nil } unless latest

      # From here on the sections read the selected checkpoint's superblock.
      @sb = latest[:superblock]
      maps = checkpoint_maps(ring, latest)
      { descriptor_area: area, selected: latest[:index], superblock: @sb,
        objects: ephemeral_objects(maps.flat_map { _1[:mappings] }) }
    end

    # One slot of the descriptor ring: a container superblock, a checkpoint
    # mapping block, a block never written (all zero, not an object), or
    # anything else.
    def descriptor(index, addr)
      bytes = raw_block(addr, "descriptor #{index}")
      return { index:, addr:, role: "empty" } if bytes.count("\0") == bytes.bytesize

      obj = checked(Obj.from(addr, bytes), "descriptor #{index}")
      case obj.kind
      when KIND_NX_SUPERBLOCK then obj.document({ index:, **superblock_slot(obj, "descriptor #{index}") })
      when KIND_CHECKPOINT_MAP then obj.document({ index:, role: "map", **checkpoint_map(obj, "descriptor #{index}") })
      else obj.document({ index:, role: "other" })
      end
    rescue Error => e
      failure("checkpoints: #{e.message}", index:, addr:, role: "unreadable")
    end

    # A checkpoint superblock is a candidate only with block 0's block size,
    # which every read depends on. Block 0 is only typically a copy of the
    # latest superblock, so a different block count (a resize) is shown but
    # does not disqualify it; reads stay bounded by block 0's count.
    def superblock_slot(obj, what)
      sb = superblock_fields(obj)
      same_size = sb[:block_size] == @block_size
      problem("#{what}: block size #{sb[:block_size]} differs from block 0's #{@block_size}") unless same_size
      valid = obj.cksum_ok? && sb[:magic] == NX_MAGIC && same_size
      fields = sb.slice(:magic, :next_xid, :xp_desc_index, :xp_desc_len, :xp_data_index, :xp_data_len)
      { role: "superblock", valid:, **fields, superblock: obj.document(sb) }
    end

    def checkpoint_map(obj, what)
      head = Layout.with_names(Layout.decode(obj.bytes, CHECKPOINT_MAP_FIELDS), flags: CHECKPOINT_FLAGS)
      capacity = (@block_size - 40) / CHECKPOINT_MAPPING_SIZE
      problem("#{what}: #{head[:count]} mappings, the block holds #{capacity}") if head[:count] > capacity
      mappings = Array.new([head[:count], capacity].min) do |i|
        fields = Layout.decode(obj.bytes, CHECKPOINT_MAPPING_FIELDS, base: 40 + i * CHECKPOINT_MAPPING_SIZE)
        { **fields, type_name: Layout.type_name(fields[:type]) }
      end
      { **head, mappings: }
    end

    # The ring slot of the valid superblock with the greatest XID, or nil.
    def select_latest(ring)
      valid = ring.select { _1[:role] == "superblock" && _1[:valid] }
      return problem("checkpoints: no valid superblock in the descriptor area") if valid.empty?

      latest = valid.max_by { _1[:xid] }
      ties = valid.count { _1[:xid] == latest[:xid] }
      problem("checkpoints: #{ties} superblocks share the latest xid #{latest[:xid]}") if ties > 1
      if @sb[:xid] > latest[:xid]
        problem("checkpoints: block 0 xid #{@sb[:xid]} is newer than the latest checkpoint #{latest[:xid]}")
      end
      latest
    end

    # The mapping blocks of the latest checkpoint's descriptor run, which
    # ends at the superblock's own slot, judged from the ring already read.
    def checkpoint_maps(ring, latest)
      sb = latest[:superblock]
      len = sb[:xp_desc_len]
      unless len.between?(1, ring.size)
        problem("checkpoints: descriptor run of #{len} blocks does not fit the ring of #{ring.size}")
        return []
      end

      run = Array.new(len) { ring[(sb[:xp_desc_index] + _1) % ring.size] }
      if run.last[:index] != latest[:index]
        problem("checkpoints: the run ends at slot #{run.last[:index]}, not the superblock's slot #{latest[:index]}")
      end
      maps = run[0...-1]
      maps.each_with_index do |slot, i|
        next problem("checkpoints: run slot #{slot[:index]} is not a mapping block") if slot[:role] != "map"

        problem("checkpoints: mapping block #{slot[:index]} has xid #{slot[:xid]}, the checkpoint #{sb[:xid]}") if slot[:xid] != sb[:xid]
        last = i == maps.size - 1
        problem("checkpoints: mapping block #{slot[:index]} LAST flag is #{!last}") if slot[:flags].anybits?(CHECKPOINT_MAP_LAST) != last
      end
      maps.select { _1[:role] == "map" }
    end

    # Reads each ephemeral object of the selected checkpoint once, keeps it
    # for the sections that resolve ephemeral OIDs, and lists it. Each block
    # of the data area is read for at most one object, so the work is bounded
    # by the data area whatever the mappings say.
    def ephemeral_objects(mappings)
      if @sb[:xp_data_blocks].anybits?(0x8000_0000)
        return [failure("checkpoints: the data area is noncontiguous; its objects are not read")]
      end

      data = @sb[:xp_data_base]...(@sb[:xp_data_base] + @sb[:xp_data_blocks])
      mappings.map { ephemeral_object(_1, data) }
    end

    def ephemeral_object(mapping, data)
      what = "ephemeral oid #{mapping[:oid]}"
      blocks = mapping[:size] / @block_size
      span = mapping[:paddr]...(mapping[:paddr] + blocks)
      if blocks.zero? || (mapping[:size] % @block_size).nonzero? || blocks > MAX_OBJECT_BLOCKS
        return failure("#{what}: size #{mapping[:size]} is not 1 to #{MAX_OBJECT_BLOCKS} whole blocks", **mapping)
      end
      return failure("#{what}: blocks #{span} are outside the checkpoint data area", **mapping) unless data.cover?(span)
      return failure("#{what}: mapped twice", **mapping) if @ephemeral.key?(mapping[:oid])
      if @ephemeral_bytes + mapping[:size] > MAX_EPHEMERAL_BYTES
        return failure("#{what}: ephemeral objects exceed this dumper's #{MAX_EPHEMERAL_BYTES} bytes", **mapping)
      end
      return failure("#{what}: blocks #{span} overlap another object", **mapping) unless claim(span)

      obj = read_object(mapping[:paddr], what, blocks:)
      check_header(obj, what, kind: mapping[:type] & 0xFFFF, storage: STORAGE_EPHEMERAL, subtype: mapping[:subtype],
                              oid: mapping[:oid])
      @ephemeral[mapping[:oid]] = obj
      @ephemeral_bytes += mapping[:size]
      obj.document(mapping.slice(:size, :fs_oid))
    rescue Error => e
      failure("checkpoints: #{e.message}", **mapping)
    end

    # ---- object map and volumes -----------------------------------------

    def container_omap
      obj = read_object(@sb[:omap_oid], "container object map")
      check_header(obj, "container object map", kind: KIND_OMAP, storage: STORAGE_PHYSICAL, oid: obj.addr)
      fields = Layout.with_names(Layout.decode(obj.bytes, OMAP_FIELDS), flags: OMAP_FLAGS)
      tree = physical_tree("container omap tree", OMAP_CODEC, subtype: KIND_OMAP).walk(fields[:tree_oid])
      @omap = ObjectMap.new(tree[:records])
      obj.document({ **fields, tree: })
    end

    def physical_tree(what, codec, subtype:)
      BTree.new(fetch: ->(oid) { read_object(oid, what) }, problem: method(:problem), codec:, what:,
                block_size: @block_size, storage: STORAGE_PHYSICAL, subtype:)
    end

    def ephemeral_tree(what, codec, subtype:)
      BTree.new(fetch: ->(oid) { ephemeral(oid, what) }, problem: method(:problem), codec:, what:,
                block_size: @block_size, storage: STORAGE_EPHEMERAL, subtype:)
    end

    # Each volume superblock OID resolved through the container object map:
    # where it lives and whether its checksum holds. Its fields are another
    # layer's.
    def volumes
      raise Error, "the container object map is unreadable" unless @omap

      @sb[:fs_oid].each_with_index.reject { |oid, _| oid.zero? }.map { |oid, slot| volume(oid, slot) }
    end

    def volume(oid, slot)
      what = "volume #{slot} (oid #{oid})"
      found = @omap.find(oid, @sb[:xid])
      return failure("#{what}: not in the container object map", slot:, oid:) unless found
      return failure("#{what}: deleted in the object map", slot:, oid:) if found[:flags].anybits?(OMAP_VAL_DELETED)

      # A volume superblock is one block; a mapping says otherwise is reported
      # and its first block read.
      problem("#{what}: mapped size #{found[:size]}, expected #{@block_size}") if found[:size] != @block_size
      obj = read_object(found[:paddr], what)
      check_header(obj, what, kind: KIND_FS, storage: STORAGE_VIRTUAL, oid:)
      obj.document({ slot:, omap_xid: found[:xid], magic: obj.bytes.unpack1("@32L<") })
    rescue Error => e
      failure("volumes: #{e.message}", slot:, oid:)
    end

    # ---- space manager ---------------------------------------------------

    def spaceman
      obj = ephemeral(@sb[:spaceman_oid], "space manager")
      check_header(obj, "space manager", kind: KIND_SPACEMAN, storage: STORAGE_EPHEMERAL, oid: @sb[:spaceman_oid])
      sm = Layout.decode(obj.bytes, SPACEMAN_FIELDS)
      problem("space manager: block size #{sm[:block_size]}, the container's #{@block_size}") if sm[:block_size] != @block_size
      obj.document({
        **Layout.with_names(sm.except(:dev, :fq, :datazone), flags: SM_FLAGS),
        datazone: allocation_zones(sm[:datazone]),
        devices: sm[:dev].each_with_index.map { |dev, i| device(obj, dev:, index: i) },
        internal_pool: section(:internal_pool) { internal_pool(obj, sm) },
        free_queues: sm[:fq].each_with_index.map { |fq, i| free_queue(fq, i) }
      })
    end

    def allocation_zones(zones)
      zones.each_with_index.filter_map do |zone, i|
        next if zone.values.flatten.all?(&:zero?)

        { device: DEVICE_NAMES[i / ALLOCATION_ZONES_PER_DEVICE], index: i % ALLOCATION_ZONES_PER_DEVICE, **zone }
      end
    end

    # A device's chunk-information chain: the address array in the space
    # manager names chunk-information blocks directly when there are no
    # address blocks, else address blocks that name them. Every chunk's
    # bitmap is recounted against its stored free count, and the sum against
    # the device's. Each block of the chain is read once (#claim), so the
    # work is bounded by the image, not by the counts it states.
    def device(obj, dev:, index:)
      name = DEVICE_NAMES[index]
      what = "space manager #{name} device"
      return { name:, **dev, cibs: [] } if dev[:block_count].zero? && dev[:cib_count].zero?
      if [dev[:cib_count], dev[:cab_count]].max > MAX_CHAIN_BLOCKS
        return failure("#{what}: #{dev[:cib_count]} chunk-information and #{dev[:cab_count]} address blocks, " \
                       "above this dumper's #{MAX_CHAIN_BLOCKS}", name:, **dev)
      end

      direct = dev[:cab_count].zero?
      addrs = Layout.array(obj.bytes, off: dev[:addr_offset], count: direct ? dev[:cib_count] : dev[:cab_count], directive: "Q<")
      named = direct ? addrs : addrs.each_with_index.lazy.flat_map { |addr, i| address_block(addr, label: "#{what} address block #{i}") }
      # One address past the stated count is taken to tell "more" from "equal".
      cib_addrs = named.first(dev[:cib_count] + 1)
      problem("#{what}: the chain names more than the device's #{dev[:cib_count]} chunk-information blocks") if cib_addrs.size > dev[:cib_count]
      problem("#{what}: the chain names #{cib_addrs.size} chunk-information blocks, the device says #{dev[:cib_count]}") if cib_addrs.size < dev[:cib_count]
      cibs = cib_addrs.first(dev[:cib_count]).each_with_index.map do |addr, i|
        chunk_info_block(addr, label: "#{what} chunk-information block #{i}")
      end
      recount = cibs.sum { |cib| cib.fetch(:chunks, []).sum { _1[:bitmap_free] || _1[:free_count] } }
      problem("#{what}: bitmaps record #{recount} free blocks, the device says #{dev[:free_count]}") if recount != dev[:free_count]
      { name:, **dev, bitmap_free: recount, cabs: direct ? [] : addrs, cibs: }
    rescue Error => e
      failure("#{what}: #{e.message}", name:, **dev)
    end

    def address_block(addr, label:)
      unless claim(addr..addr)
        problem("#{label}: block #{addr} is already read for another structure")
        return []
      end

      obj = read_object(addr, label)
      check_header(obj, label, kind: KIND_SPACEMAN_CAB, storage: STORAGE_PHYSICAL, oid: addr)
      head = Layout.decode(obj.bytes, ADDRESS_BLOCK_FIELDS)
      capacity = (@block_size - 40) / 8
      problem("#{label}: #{head[:count]} entries, the block holds #{capacity}") if head[:count] > capacity
      Layout.array(obj.bytes, off: 40, count: [head[:count], capacity].min, directive: "Q<")
    rescue Error => e
      problem(e.message)
      []
    end

    def chunk_info_block(addr, label:)
      return failure("#{label}: block #{addr} is already read for another structure", addr:) unless claim(addr..addr)

      obj = read_object(addr, label)
      check_header(obj, label, kind: KIND_SPACEMAN_CIB, storage: STORAGE_PHYSICAL, oid: addr)
      head = Layout.decode(obj.bytes, ADDRESS_BLOCK_FIELDS)
      capacity = (@block_size - 40) / CHUNK_INFO_SIZE
      problem("#{label}: #{head[:count]} chunks, the block holds #{capacity}") if head[:count] > capacity
      chunks = Array.new([head[:count], capacity].min) do |i|
        chunk(Layout.decode(obj.bytes, CHUNK_INFO_FIELDS, base: 40 + i * CHUNK_INFO_SIZE), label: "#{label} chunk #{i}")
      end
      obj.document({ **head, chunks: })
    rescue Error => e
      failure(e.message, addr:)
    end

    # A chunk with no bitmap block is entirely free.
    def chunk(info, label:)
      if info[:bitmap_addr].zero?
        problem("#{label}: no bitmap, but #{info[:free_count]} of #{info[:block_count]} blocks free") if info[:free_count] != info[:block_count]
        return { **info, bitmap_free: nil }
      end
      unless claim(info[:bitmap_addr]..info[:bitmap_addr])
        return failure("#{label}: bitmap block #{info[:bitmap_addr]} is already read for another structure", **info, bitmap_free: nil)
      end

      bits = [info[:block_count], @block_size * 8].min
      problem("#{label}: #{info[:block_count]} blocks, more than a bitmap block covers") if info[:block_count] > bits
      free = Layout.zero_bits(raw_block(info[:bitmap_addr], label), bits)
      problem("#{label}: bitmap records #{free} free blocks, the chunk says #{info[:free_count]}") if free != info[:free_count]
      { **info, bitmap_free: free }
    rescue Error => e
      failure(e.message, **info, bitmap_free: nil)
    end

    # The internal pool's bitmap ring. The reference names these fields but
    # does not describe them; this reading is that the space manager holds,
    # at the three offsets, one XID and one u16 ring index per block of the
    # current pool bitmap (ip_bm_size_in_blocks of each) and a u16 free-list
    # successor per ring block (ip_bm_block_count), ended by 0xffff.
    def internal_pool(obj, sm)
      size = sm[:ip_bm_size_in_blocks]
      ring = sm[:ip_bm_block_count]
      raise Error, "internal pool: ring of #{ring} blocks is above #{IP_BM_BLOCK_COUNT_MAX}" if ring > IP_BM_BLOCK_COUNT_MAX
      raise Error, "internal pool: bitmap of #{size} blocks is larger than its ring of #{ring}" if size > ring

      xids = Layout.array(obj.bytes, off: sm[:ip_bm_xid_offset], count: size, directive: "Q<")
      indexes = Layout.array(obj.bytes, off: sm[:ip_bitmap_offset], count: size, directive: "S<")
      free_next = Layout.array(obj.bytes, off: sm[:ip_bm_free_next_offset], count: ring, directive: "S<")
      per_block = @block_size * 8
      covered = [sm[:ip_block_count], size * per_block].min
      problem("internal pool: #{sm[:ip_block_count]} blocks, its bitmap covers #{covered}") if covered < sm[:ip_block_count]
      # Block j of the bitmap holds bits j * per_block onward; it is read only
      # when some of those bits cover the pool.
      free = indexes.each_with_index.sum do |ring_index, j|
        raise Error, "internal pool: bitmap index #{ring_index} is outside the ring of #{ring}" if ring_index >= ring

        bits = (covered - j * per_block).clamp(0, per_block)
        bits.zero? ? 0 : Layout.zero_bits(raw_block(sm[:ip_bm_base] + ring_index, "internal-pool bitmap"), bits)
      end
      { bitmap_xids: xids, bitmap_indexes: indexes, free_next:, used: covered - free, free: }
    end

    def free_queue(fq, index)
      name = FREE_QUEUE_NAMES[index]
      return { name:, **fq, tree: nil } if fq[:tree_oid].zero?

      what = "#{name} free queue"
      tree = ephemeral_tree("#{what} tree", FREE_QUEUE_CODEC, subtype: KIND_SPACEMAN_FREE_QUEUE).walk(fq[:tree_oid])
      queued = tree[:records].sum { _1[:count] }
      problem("#{what}: records hold #{queued} blocks, the queue says #{fq[:count]}") if queued != fq[:count]
      { name:, **fq, queued:, tree: }
    end

    # ---- reaper ----------------------------------------------------------

    def reaper
      obj = ephemeral(@sb[:reaper_oid], "reaper")
      check_header(obj, "reaper", kind: KIND_NX_REAPER, storage: STORAGE_EPHEMERAL, oid: @sb[:reaper_oid])
      fields = Layout.with_names(Layout.decode(obj.bytes, REAPER_FIELDS), flags: REAPER_FLAGS, nrle_flags: NRLE_FLAGS)
      room = obj.bytes.bytesize - REAPER_STATE_OFFSET
      problem("reaper: state buffer of #{fields[:state_buffer_size]} bytes, the object holds #{room}") if fields[:state_buffer_size] > room
      # The state buffer in hexadecimal, its trailing zero bytes left out.
      state = obj.bytes.byteslice(REAPER_STATE_OFFSET, [fields[:state_buffer_size], room].min)
      state = state.byteslice(0, (state.rindex(/[^\0]/n) || -1) + 1)
      obj.document({ **fields, state_buffer: state.unpack1("H*"), reap_lists: reap_lists(fields[:head]) })
    end

    # The reap lists from the head, each an ephemeral object naming the next.
    def reap_lists(head)
      seen = Set.new
      oid = head
      lists = []
      until oid.zero?
        break problem("reaper: reap list #{oid} is reached twice") unless seen.add?(oid)
        break problem("reaper: more than #{MAX_REAP_LISTS} reap lists") if lists.size >= MAX_REAP_LISTS

        list = reap_list(oid)
        lists << list
        oid = list.fetch(:next, 0)
      end
      lists
    end

    def reap_list(oid)
      what = "reap list #{oid}"
      obj = ephemeral(oid, what)
      check_header(obj, what, kind: KIND_NX_REAP_LIST, storage: STORAGE_EPHEMERAL, oid:)
      fields = Layout.decode(obj.bytes, REAP_LIST_FIELDS)
      capacity = (obj.bytes.bytesize - REAP_LIST_ENTRIES_OFFSET) / REAP_LIST_ENTRY_SIZE
      problem("#{what}: room for #{fields[:max]} entries, the object holds #{capacity}") if fields[:max] > capacity
      entries = Array.new([fields[:max], capacity].min) do |i|
        entry = Layout.decode(obj.bytes, REAP_LIST_ENTRY_FIELDS, base: REAP_LIST_ENTRIES_OFFSET + i * REAP_LIST_ENTRY_SIZE)
        { index: i, **Layout.with_names(entry, flags: NRLE_FLAGS) } unless entry.values.all?(&:zero?)
      end
      obj.document({ **fields, entries: entries.compact })
    rescue Error => e
      failure(e.message, oid:)
    end
  end

  # Prints a document as indented `key: value` lines. Integers whose key
  # names a checksum, type or flag word print in hexadecimal. A hash of
  # scalars inside an array prints on one line; trailing zeros of an integer
  # array are counted instead of printed.
  module Text
    HEX_KEY = /cksum|type\z|flags\z|features\z|magic\z|subtype\z/

    module_function

    def render(doc, out) = lines(doc, 0).each { out.puts(_1) }

    def lines(hash, depth)
      pad = "  " * depth
      hash.flat_map do |key, value|
        case value
        in Hash then ["#{pad}#{key}:", *lines(value, depth + 1)]
        in [] then ["#{pad}#{key}: none"]
        in [Hash, *] then value.each_with_index.flat_map { |item, i| element("#{pad}#{key}[#{i}]", item, depth) }
        in Array then ["#{pad}#{key}: #{scalars(key, value)}"]
        else ["#{pad}#{key}: #{scalar(key, value)}"]
        end
      end
    end

    def element(label, item, depth)
      return ["#{label}: #{item.map { |k, v| "#{k}=#{scalar(k, v)}" }.join(' ')}"] if item.values.none? { _1.is_a?(Hash) || _1.is_a?(Array) }

      ["#{label}:", *lines(item, depth + 1)]
    end

    def scalars(key, values)
      shown = values.reverse.drop_while { _1 == 0 }.reverse
      omitted = values.size - shown.size
      parts = shown.map { scalar(key, _1) }
      parts << "(+#{omitted} zero)" unless omitted.zero?
      parts.join(" ")
    end

    def scalar(key, value)
      case value
      when nil then "-"
      when Integer then key.to_s.match?(HEX_KEY) ? format("0x%x", value) : value.to_s
      when String then value.ascii_only? ? value : value.dump
      else value.to_s
      end
    end
  end

  def self.main(argv)
    offset = 0
    json = false
    parser = OptionParser.new("usage: apfsdump.rb [--offset BYTES] [--json] IMAGE") do |o|
      o.on("--offset BYTES", Integer) { offset = _1 }
      o.on("--json") { json = true }
    end
    paths = parser.parse(argv)
    raise Error, parser.banner unless paths.size == 1

    File.open(paths.first, "rb") do |io|
      container = Container.new(Image.new(io, offset:))
      doc = container.dump
      json ? $stdout.puts(JSON.pretty_generate(doc)) : Text.render(doc.except(:problems), $stdout)
      container.problems.each { $stdout.puts("problem: #{_1}") } unless json
      container.problems.empty? ? 0 : 1
    end
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "apfsdump: #{e.message}"
    1
  end
end

exit ApfsDump.main(ARGV) if $PROGRAM_NAME == __FILE__
