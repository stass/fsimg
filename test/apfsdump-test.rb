# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "stringio"
require_relative "../tools/apfs/apfsdump"

class ApfsDumpTest < Minitest::Test
  include ApfsDump

  BLOCK = 4096

  # An in-memory image with the two calls Image makes of a file.
  BytesIO = Data.define(:bytes) do
    def size = bytes.bytesize
    def pread(len, off) = bytes.byteslice(off, len)
  end

  # The checkpoint-mapping block at block 1 of a 64 MiB container formatted
  # by newfs_apfs on macOS, trailing zeros left out. Its checksum was written
  # by Apple's implementation, so it is a vector independent of this one.
  NATIVE_MAP_BLOCK = [
    "96b2613f2d259e80010000000000000001000000000000000c00004000000000" \
    "0100000002000000050000800000000000100000000000000000000000000000" \
    "0004000000000000090000000000000011000080000000000010000000000000" \
    "000000000000000001040000000000000a"
  ].pack("H*").ljust(BLOCK, "\0")

  def test_fletcher64_of_a_zero_object_is_all_ones
    assert_equal 0xFFFF_FFFF_FFFF_FFFF, Fletcher64.compute("\0".b * BLOCK)
  end

  def test_fletcher64_reproduces_a_native_checksum
    obj = Obj.from(1, NATIVE_MAP_BLOCK)

    assert_equal [0x809E_252D_3F61_B296, true], [obj.computed, obj.cksum_ok?]
  end

  def test_a_changed_byte_breaks_the_checksum
    damaged = NATIVE_MAP_BLOCK.dup.tap { _1.setbyte(100, 0xFF) }

    refute_predicate Obj.from(1, damaged), :cksum_ok?
  end

  def test_native_map_block_decodes_two_mappings
    head = Layout.decode(NATIVE_MAP_BLOCK, CHECKPOINT_MAP_FIELDS)
    second = Layout.decode(NATIVE_MAP_BLOCK, CHECKPOINT_MAPPING_FIELDS, base: 40 + CHECKPOINT_MAPPING_SIZE)

    assert_equal({ flags: 1, count: 2 }, head)
    assert_equal [0x8000_0011, 1025, 10], second.values_at(:type, :oid, :paddr)
  end

  def test_superblock_fields_decode_at_their_reference_offsets
    sb = Layout.decode(superblock_block(block_count: 300, fs_oid: [1026, 1027]), NX_FIELDS)

    assert_equal [NX_MAGIC, BLOCK, 300], sb.values_at(:magic, :block_size, :block_count)
    assert_equal "00112233-4455-6677-8899-aabbccddeeff", sb[:uuid]
    assert_equal [1026, 1027] + [0] * 98, sb[:fs_oid]
    assert_equal({ start_paddr: 7, block_count: 2 }, sb[:keylocker])
    assert_equal 0xABCD, sb[:newest_mounted_version]
  end

  def test_a_field_past_the_end_of_its_bytes_is_an_error
    assert_raises(Error) { Layout.decode("\0".b * 1000, NX_FIELDS) }
  end

  def test_omap_leaf_records_are_decoded_from_a_root_node
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), [0, BLOCK, 50].pack("L<L<q<")],
                                                  [[1027, 3].pack("Q<Q<"), [1, BLOCK, 51].pack("L<L<q<")]])
    tree, problems = walk(9 => node)

    assert_empty problems
    assert_equal [[1026, 2, 50, ""], [1027, 3, 51, "DELETED"]], tree[:records].map { _1.values_at(:oid, :xid, :paddr, :flags_names) }
  end

  def test_a_key_count_beyond_the_table_of_contents_is_reported_and_bounded
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), [0, BLOCK, 50].pack("L<L<q<")]],
                      nkeys: 0xFFFF_FFFF)
    tree, problems = walk(9 => node)

    assert_match(/4294967295 keys, the table of contents holds #{TOC_ENTRIES}/, problems.first)
    assert_equal 1, tree[:records].size
  end

  def test_a_ghost_in_a_tree_without_ghosts_is_reported_and_dropped
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), nil]])
    tree, problems = walk(9 => node)

    assert_equal [], tree[:records]
    assert(problems.any? { _1.include?("ghost record in a tree that does not allow ghosts") })
  end

  def test_an_index_node_naming_itself_stops_the_walk
    node = node_block(oid: 9, level: 1, entries: [[[1, 0].pack("Q<Q<"), [9].pack("Q<")]])
    tree, problems = walk(9 => node)

    assert_equal 1, tree[:nodes].size
    assert(problems.any? { _1.include?("reached twice") })
  end

  def test_a_damaged_container_is_reported_section_by_section
    image = superblock_block(block_count: 16, omap_oid: 5).then { seal(_1) } + ("\0".b * (BLOCK * 15))
    container = Container.new(Image.new(BytesIO.new(bytes: image)))
    doc = container.dump

    assert_equal %i[image superblock checkpoints omap volumes spaceman reaper problems], doc.keys
    assert_includes container.problems, "checkpoints: no valid superblock in the descriptor area"
    assert(container.problems.any? { _1.start_with?("container object map at block 5: checksum") })
    assert(container.problems.any? { _1.include?("ephemeral oid 1024 has no mapping") })
  end

  def test_a_whole_container_dumps_without_problems
    container = Container.new(Image.new(BytesIO.new(bytes: container_image)))
    doc = container.dump

    assert_empty container.problems
    assert_equal [1, true], [doc[:checkpoints][:selected], doc[:checkpoints][:descriptor_area][1][:latest]]
    assert_equal [1026, 12, 0x42535041], doc[:volumes].first.values_at(:oid, :addr, :magic)
    assert_equal [39, 39], doc[:spaceman][:devices].first.values_at(:free_count, :bitmap_free)
    assert_equal({ used: 1, free: 1 }, doc[:spaceman][:internal_pool].slice(:used, :free))
    assert_equal 0x1, doc[:reaper][:flags]
  end

  # A crafted chain that names one block many times reads it once.
  def test_a_repeated_chunk_information_block_is_read_once
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(cibs: [20, 20]))))
    device = container.dump[:spaceman][:devices].first

    assert_equal [20, nil], device[:cibs].map { _1[:oid] }
    assert(container.problems.any? { _1.include?("block 20 is already read for another structure") })
  end

  def test_an_oversized_ephemeral_mapping_is_not_read
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(spaceman_size: BLOCK * 1000))))
    doc = container.dump

    assert(container.problems.any? { _1.include?("ephemeral oid 1024: size 4096000 is not 1 to 256 whole blocks") })
    assert_includes doc[:spaceman][:error], "ephemeral oid 1024 has no mapping"
  end

  def test_an_image_without_a_container_superblock_is_an_error
    image = Image.new(BytesIO.new(bytes: "\0".b * (BLOCK * 2)))

    assert_raises(Error) { Container.new(image).dump }
  end

  def test_text_prints_hex_flag_words_and_counts_trailing_zeros
    out = StringIO.new
    Text.render({ sb: { flags: 0x12, fs_oid: [1026, 0, 0], rows: [{ oid: 1, type: 0x40000002 }] } }, out)

    assert_equal "sb:\n  flags: 0x12\n  fs_oid: 1026 (+2 zero)\n  rows[0]: oid=1 type=0x40000002\n", out.string
  end

  private

  def walk(nodes)
    problems = []
    tree = BTree.new(fetch: ->(oid) { Obj.from(oid, nodes.fetch(oid)) }, problem: ->(text) { problems << text },
                     codec: OMAP_CODEC, what: "tree", block_size: BLOCK, storage: STORAGE_PHYSICAL,
                     subtype: KIND_OMAP).walk(nodes.keys.first)
    [tree, problems]
  end

  # Table-of-contents entries in a test node: 4-byte kvoff_t entries.
  TOC_ENTRIES = 16

  # A sealed root node of a fixed 16/16 physical tree. Keys fill the key
  # area from its start; values fill the value area back from the footer. A
  # nil value is a ghost.
  def node_block(oid:, level:, entries:, nkeys: entries.size)
    toc_len = TOC_ENTRIES * 4
    flags = NODE_ROOT | NODE_FIXED | (level.zero? ? NODE_LEAF : 0)
    block = "\0".b * BLOCK
    put(block, 0, [0, oid, 1, KIND_BTREE | STORAGE_PHYSICAL, KIND_OMAP, flags, level, nkeys, 0, toc_len].pack("Q<Q<Q<L<L<S<S<L<S<S<"))
    entries.each_with_index do |(key, value), i|
      put(block, NODE_HEADER_END + i * 4, [i * 16, value ? (i + 1) * 16 : BTOFF_INVALID].pack("S<S<"))
      put(block, NODE_HEADER_END + toc_len + i * 16, key)
      put(block, BLOCK - TREE_INFO_SIZE - (i + 1) * 16, value) if value
    end
    put(block, BLOCK - TREE_INFO_SIZE, [0x12, BLOCK, 16, 16, 16, 16, entries.size, 1].pack("L<6Q<2"))
    seal(block)
  end

  def superblock_block(block_count:, fs_oid: [], omap_oid: 0)
    block = "\0".b * BLOCK
    put(block, 8, [1, 1, 0x8000_0001, 0].pack("Q<Q<L<L<"))
    put(block, 32, [NX_MAGIC, BLOCK, block_count].pack("L<L<Q<"))
    put(block, 72, ["00112233445566778899aabbccddeeff"].pack("H*"))
    put(block, 104, [2, 4, 1, 3].pack("L<L<Q<Q<"))
    put(block, 136, [0, 2, 0, 2].pack("L<4"))
    put(block, 152, [1024, omap_oid, 1025].pack("Q<3"))
    put(block, 180, [100, *fs_oid].pack("L<Q<*"))
    put(block, 1296, [7, 2].pack("q<Q<"))
    put(block, 1384, [0xABCD].pack("Q<"))
    block
  end

  # A 64-block container: block 0 and the descriptor ring (a mapping block
  # at 1, the checkpoint superblock at 2), the space manager and reaper in
  # the data area at 3 and 4, the object map at 10 with its tree at 11
  # naming the volume superblock at 12, one chunk-information block at 20
  # with its bitmap at 21, and the internal-pool bitmap ring at 22.
  def container_image(cibs: [20], spaceman_size: BLOCK)
    sb = seal(superblock_block(block_count: 64, fs_oid: [1026], omap_oid: 10))
    omap_record = [[1026, 1].pack("Q<Q<"), [0, BLOCK, 12].pack("L<L<q<")] # (oid, xid) -> (flags, size, paddr)
    blocks = {
      0 => sb, 1 => checkpoint_map(spaceman_size:), 2 => sb,
      3 => spaceman(cibs:), 4 => object(oid: 1025, type: 0x8000_0011, fields: { 64 => [1].pack("L<") }), # reaper flags
      10 => omap, 11 => node_block(oid: 11, level: 0, entries: [omap_record]),
      12 => object(oid: 1026, type: KIND_FS, fields: { 32 => "APSB" }),
      20 => chunk_info, 21 => bitmap(25), 22 => bitmap(1)
    }
    Array.new(64) { blocks.fetch(_1) { "\0".b * BLOCK } }.join
  end

  def object(oid:, type:, fields: {}, subtype: 0)
    block = "\0".b * BLOCK
    put(block, 8, [oid, 1, type, subtype].pack("Q<Q<L<L<"))
    fields.each { |off, bytes| put(block, off, bytes) }
    seal(block)
  end

  # Mappings: type, subtype, size, pad, fs_oid, oid, paddr.
  def checkpoint_map(spaceman_size:)
    mappings = [[0x8000_0005, 0, spaceman_size, 0, 0, 1024, 3], [0x8000_0011, 0, BLOCK, 0, 0, 1025, 4]]
    object(oid: 1, type: 0x4000_000C, fields: { 32 => [CHECKPOINT_MAP_LAST, 2].pack("L<L<"),
                                                40 => mappings.map { _1.pack("L<4Q<3") }.join })
  end

  # The space manager of the 64-block container. The main device's
  # chunk-information addresses (`cibs`) sit at 2568; the internal pool's
  # bitmap XID, ring index and free-next arrays at 2520, 2528 and 2536.
  def spaceman(cibs:)
    geometry = [BLOCK, 32_768, 126, 507].pack("L<4") # block size, blocks per chunk, chunks per CIB, CIBs per CAB
    # block count, chunk count, CIB count, CAB count, free count, address offset, reserved
    device = [64, 1, cibs.size, 0, 39, 2568, 0, 0].pack("Q<Q<L<L<Q<L<L<Q<")
    # flags VERSIONED; pool: tx multiplier, block count, bitmap blocks, ring blocks, ring base, pool base
    pool = [1, 16, 2, 1, 1, 22, 23].pack("L<L<Q<L<L<Q<Q<")
    # ring free head and tail, the three array offsets, version, structure size
    ring = [0xFFFF, 0, 2520, 2528, 2536, 1, 2520].pack("S<S<L<5")
    arrays = [1, 0, 0xFFFF].pack("Q<S<x6S<") # bitmap XID 1, ring index 0, free-next end marker
    object(oid: 1024, type: 0x8000_0005,
           fields: { 32 => geometry + device, 144 => pool, 320 => ring, 2520 => arrays, 2568 => cibs.pack("Q<*") })
  end

  # Index 0, one chunk: xid 1, first block 0, 64 blocks, 39 free, bitmap at 21.
  def chunk_info = object(oid: 20, type: 0x4000_0007, fields: { 32 => [0, 1, 1, 0, 64, 39, 21].pack("L<L<Q<Q<L<L<Q<") })

  # Manually managed, no snapshots, physical tree types, tree at 11.
  def omap = object(oid: 10, type: 0x4000_000B, fields: { 32 => [1, 0, 0x4000_0002, 0x4000_0002, 11].pack("L<4Q<") })

  # A bitmap block whose first `used` bits are set.
  def bitmap(used) = (("1" * used).ljust(BLOCK * 8, "0")).then { [_1].pack("b*") }

  def put(block, off, bytes) = block.bytesplice(off, bytes.bytesize, bytes)

  def seal(block) = block.tap { put(_1, 0, [Fletcher64.compute(_1)].pack("Q<")) }
end
