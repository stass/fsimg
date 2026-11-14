# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/apfs"

# The layout records place every field where specs/design-apfs.md does. The
# table below is transcribed by hand from that document's field tables
# (Object header and checksum, Container superblock, Ephemeral objects and
# the space manager, Free queues, Reaper, Object maps, Node layout), never
# from fsimg's C sources or from the layouts under test. Where the design
# gives an offset but no width (the tier-2 device entry, the chunk record,
# the pool and ring fields), the width is the reference's [A1]. Offsets are from
# the start of the object for the records that start at Layout::BODY, and
# from the start of the record otherwise.
class TestApfsLayout < Minitest::Test
  Layout = FsimgSpec::Apfs::Layout
  Fletcher64 = FsimgSpec::Apfs::Fletcher64

  # Field name (the layout's snake_case name for the table's field), byte
  # offset and size in bytes.
  SPECIFICATION = {
    Layout::ObjectHeader => [[:checksum, 0, 8], [:oid, 8, 8], [:xid, 16, 8], [:object_type, 24, 4], [:subtype, 28, 4]],
    Layout::ContainerSuperblock => [
      [:magic, 32, 4], [:block_size, 36, 4], [:block_count, 40, 8], [:optional_features, 48, 8],
      [:readonly_features, 56, 8], [:incompatible_features, 64, 8], [:uuid, 72, 16], [:next_oid, 88, 8],
      [:next_xid, 96, 8], [:descriptor_blocks, 104, 4], [:data_blocks, 108, 4], [:descriptor_base, 112, 8],
      [:data_base, 120, 8], [:descriptor_next, 128, 4], [:data_next, 132, 4], [:descriptor_index, 136, 4],
      [:descriptor_length, 140, 4], [:data_index, 144, 4], [:data_length, 148, 4], [:space_manager_oid, 152, 8],
      [:object_map_oid, 160, 8], [:reaper_oid, 168, 8], [:test_type, 176, 4], [:max_volumes, 180, 4],
      [:volume_oids, 184, 800], [:counters, 984, 256], [:blocked_out_range, 1240, 16], [:eviction_tree_oid, 1256, 8],
      [:flags, 1264, 8], [:efi_jumpstart, 1272, 8], [:fusion_uuid, 1280, 16], [:keylocker_start, 1296, 8],
      [:keylocker_count, 1304, 8], [:ephemeral_info, 1312, 32], [:test_oid, 1344, 8], [:fusion_trees, 1352, 32],
      [:newest_mounted_version, 1384, 8], [:media_keylocker_start, 1392, 8], [:media_keylocker_count, 1400, 8]
    ],
    Layout::CheckpointMapHead => [[:flags, 32, 4], [:entry_count, 36, 4]],
    Layout::CheckpointMapping => [
      [:object_type, 0, 4], [:subtype, 4, 4], [:byte_size, 8, 4], [:reserved, 12, 4], [:volume_oid, 16, 8], [:oid, 24, 8],
      [:address, 32, 8]
    ],
    Layout::SpaceManager => [
      [:block_size, 32, 4], [:blocks_per_chunk, 36, 4], [:chunks_per_cib, 40, 4], [:cibs_per_cab, 44, 4], [:main, 48, 48],
      [:tier2, 96, 48], [:flags, 144, 4], [:pool_tx_multiplier, 148, 4], [:pool_block_count, 152, 8],
      [:pool_bitmap_size, 160, 4], [:pool_bitmap_blocks, 164, 4], [:pool_bitmap_base, 168, 8], [:pool_base, 176, 8],
      [:reserve_block_count, 184, 8], [:reserve_alloc_count, 192, 8], [:queues, 200, 120], [:pool_free_head, 320, 2],
      [:pool_free_tail, 322, 2], [:pool_xid_offset, 324, 4], [:pool_index_offset, 328, 4], [:pool_free_next_offset, 332, 4],
      [:version, 336, 4], [:structure_size, 340, 4], [:data_zones, 344, 2176]
    ],
    Layout::SpaceDevice => [
      [:block_count, 0, 8], [:chunk_count, 8, 8], [:cib_count, 16, 4], [:cab_count, 20, 4], [:free_count, 24, 8],
      [:address_offset, 32, 4]
    ],
    Layout::FreeQueueEntry => [[:queued, 0, 8], [:tree_oid, 8, 8], [:oldest_xid, 16, 8], [:node_limit, 24, 2]],
    Layout::ChunkBlockHead => [[:block_index, 32, 4], [:entry_count, 36, 4]],
    Layout::ChunkInfo => [[:xid, 0, 8], [:first_block, 8, 8], [:block_count, 16, 4], [:free_count, 20, 4], [:bitmap_block, 24, 8]],
    Layout::ObjectMap => [
      [:flags, 32, 4], [:snapshot_count, 36, 4], [:tree_type, 40, 4], [:snapshot_tree_type, 44, 4], [:tree_oid, 48, 8],
      [:snapshot_tree_oid, 56, 8], [:latest_snapshot_xid, 64, 8], [:revert_min, 72, 8], [:revert_max, 80, 8]
    ],
    Layout::ObjectMapKey => [[:oid, 0, 8], [:xid, 8, 8]],
    Layout::ObjectMapValue => [[:flags, 0, 4], [:byte_size, 4, 4], [:address, 8, 8]],
    Layout::FreeQueueKey => [[:xid, 0, 8], [:first_block, 8, 8]],
    Layout::NodeHead => [
      [:flags, 32, 2], [:level, 34, 2], [:key_count, 36, 4], [:toc_offset, 40, 2], [:toc_length, 42, 2],
      [:free_offset, 44, 2], [:free_length, 46, 2], [:key_free_offset, 48, 2], [:key_free_length, 50, 2],
      [:value_free_offset, 52, 2], [:value_free_length, 54, 2]
    ],
    Layout::NodeFooter => [
      [:tree_flags, 0, 4], [:node_size, 4, 4], [:key_size, 8, 4], [:value_size, 12, 4], [:longest_key, 16, 4],
      [:longest_value, 20, 4], [:key_count, 24, 8], [:node_count, 32, 8]
    ],
    Layout::FixedEntry => [[:key_offset, 0, 2], [:value_offset, 2, 2]],
    Layout::VariableEntry => [[:key_offset, 0, 2], [:key_length, 2, 2], [:value_offset, 4, 2], [:value_length, 6, 2]],
    Layout::Reaper => [
      [:next_reap_id, 32, 8], [:completed_reap_id, 40, 8], [:head, 48, 8], [:tail, 56, 8], [:flags, 64, 4],
      [:list_count, 68, 4], [:reaped_type, 72, 4], [:reaped_size, 76, 4], [:reaped_volume_oid, 80, 8], [:reaped_oid, 88, 8],
      [:reaped_xid, 96, 8], [:entry_flags, 104, 4], [:state_size, 108, 4]
    ],
    Layout::ReapList => [
      [:next_oid, 32, 8], [:flags, 40, 4], [:capacity, 44, 4], [:entry_count, 48, 4], [:first_index, 52, 4],
      [:last_index, 56, 4], [:free_index, 60, 4]
    ],
    Layout::ReapListEntry => [
      [:next_index, 0, 4], [:flags, 4, 4], [:object_type, 8, 4], [:byte_size, 12, 4], [:volume_oid, 16, 8], [:oid, 24, 8],
      [:xid, 32, 8]
    ]
  }.freeze

  # Where each record ends in its object: the end the design gives.
  ENDS = { Layout::ContainerSuperblock => 1408, Layout::SpaceManager => 2520, Layout::ObjectMap => 88, Layout::NodeHead => 56,
           Layout::Reaper => 112, Layout::ReapList => 64, Layout::SpaceDevice => 48, Layout::FreeQueueEntry => 40 }.freeze

  def test_every_field_lies_where_the_design_places_it
    assert_empty SPECIFICATION.flat_map { |record, rows| mismatches(record, rows) }
  end

  def test_object_records_end_where_the_design_tables_end
    ENDS.each { |record, finish| assert_equal finish, base(record) + record.new.num_bytes, record.name }
  end

  def test_two_swapped_offsets_are_found
    rows = SPECIFICATION.fetch(Layout::NodeHead)
    swapped = [rows[1].dup.tap { _1[1] = 36 }, rows[2].dup.tap { _1[1] = 34 }]
    assert_equal 2, mismatches(Layout::NodeHead, swapped).size
  end

  def test_bulk_records_decode_as_single_records_do
    bytes = (0...64).map { (_1 * 37) & 0xFF }.pack("C*")
    single = [0, 32].map { Layout.find(Layout::ChunkInfo, bytes, at: _1) }
    assert_equal single, Layout.records(Layout::ChunkInfo, bytes, at: 0, count: 3)
  end

  def test_a_block_of_zeros_computes_the_all_ones_checksum
    assert_equal 0xFFFF_FFFF_FFFF_FFFF, Fletcher64.compute("\0".b * 4096)
    refute Fletcher64.valid?("\0".b * 4096)
  end

  def test_sealing_makes_a_checksum_valid_and_a_flipped_byte_invalid
    sealed = Fletcher64.seal((0...4096).map { _1 & 0xFF }.pack("C*"))
    assert Fletcher64.valid?(sealed)
    refute Fletcher64.valid?(sealed.dup.tap { _1.setbyte(100, sealed.getbyte(100) ^ 1) })
  end

  private

  def base(record) = Layout::OBJECT_BODIES.include?(record) ? Layout::BODY : 0

  def mismatches(record, rows)
    sample = record.new
    rows.reject { |name, offset, size| sample[name].rel_offset + base(record) == offset && sample[name].num_bytes == size }
        .map { |name, *| "#{record.name.split("::").last}.#{name}" }
  end
end
