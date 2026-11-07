# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/erofs"

# The layout records place every field where specs/design-erofs.md does. The
# table below is transcribed by hand from that document's field tables (The
# superblock, Compact inode, Extended inode, Blocks, Inline region), never
# from fsimg's C sources or from the layouts under test. Offsets are from the
# start of the record: the superblock's from volume byte 1024.
class TestErofsLayout < Minitest::Test
  Layout = FsimgSpec::Erofs::Layout

  # Field name (the layout's snake_case name for the table's field), byte
  # offset and size in bytes.
  SPECIFICATION = {
    Layout::Superblock => [
      [:magic, 0x00, 4], [:checksum, 0x04, 4], [:compat, 0x08, 4], [:blkszbits, 0x0C, 1],
      [:extension_slots, 0x0D, 1], [:root_nid, 0x0E, 2], [:inode_count, 0x10, 8], [:epoch, 0x18, 8],
      [:fixed_nsec, 0x20, 4], [:blocks, 0x24, 4], [:meta_blkaddr, 0x28, 4], [:xattr_blkaddr, 0x2C, 4],
      [:uuid, 0x30, 16], [:volume_name, 0x40, 16], [:incompat, 0x50, 4], [:algorithm_mask, 0x54, 2],
      [:extra_devices, 0x56, 2], [:device_slot, 0x58, 2], [:dirblkbits, 0x5A, 1], [:prefix_count, 0x5B, 1],
      [:prefix_start, 0x5C, 4], [:packed_nid, 0x60, 8], [:filter_reserved, 0x68, 1], [:ishare_prefix, 0x69, 1],
      [:reserved_a, 0x6A, 2], [:build_time, 0x6C, 4], [:wide_root_nid, 0x70, 8], [:reserved_b, 0x78, 8]
    ],
    Layout::CompactInode => [
      [:format, 0x00, 2], [:xattr_count, 0x02, 2], [:mode, 0x04, 2], [:nlink, 0x06, 2],
      [:size_bytes, 0x08, 4], [:time, 0x0C, 4], [:layout_word, 0x10, 4], [:serial, 0x14, 4],
      [:owner, 0x18, 2], [:group, 0x1A, 2], [:reserved, 0x1C, 4]
    ],
    Layout::ExtendedInode => [
      [:format, 0x00, 2], [:xattr_count, 0x02, 2], [:mode, 0x04, 2], [:reserved_a, 0x06, 2],
      [:size_bytes, 0x08, 8], [:layout_word, 0x10, 4], [:serial, 0x14, 4], [:owner, 0x18, 4],
      [:group, 0x1C, 4], [:time, 0x20, 8], [:nsec, 0x28, 4], [:nlink, 0x2C, 4], [:reserved_b, 0x30, 16]
    ],
    Layout::DirectoryEntry => [[:nid, 0, 8], [:name_offset, 8, 2], [:file_type, 10, 1], [:reserved, 11, 1]],
    Layout::AttributeHeader => [[:name_filter, 0, 4], [:shared_count, 4, 1], [:reserved, 5, 7]],
    Layout::AttributeEntry => [[:suffix_length, 0, 1], [:name_index, 1, 1], [:value_length, 2, 2]]
  }.freeze

  def test_every_field_lies_where_the_design_places_it
    assert_empty mismatches(SPECIFICATION)
  end

  def test_each_record_has_the_size_its_table_ends_at
    ends = SPECIFICATION.transform_values { |rows| rows.map { |_, offset, size| offset + size }.max }

    assert_equal({ Layout::Superblock => 128, Layout::CompactInode => 32, Layout::ExtendedInode => 64,
                   Layout::DirectoryEntry => 12, Layout::AttributeHeader => 12, Layout::AttributeEntry => 4 }, ends)
    assert_equal ends, ends.to_h { |record, _| [record, record.new.num_bytes] }
  end

  # The seeded failure: swapping two offsets in the table must be found.
  def test_two_swapped_offsets_are_found
    compact = SPECIFICATION.fetch(Layout::CompactInode)
    swapped = compact.map do |name, offset, size|
      case name
      in :owner then [name, 0x1A, size]
      in :group then [name, 0x18, size]
      else [name, offset, size]
      end
    end

    assert_equal %w[group owner], mismatches(SPECIFICATION.merge(Layout::CompactInode => swapped)).map(&:first).sort
  end

  private

  # The fields whose offset or size differs from table, and the fields one
  # side has and the other lacks, as [field, expected, actual].
  def mismatches(table)
    table.flat_map do |record, rows|
      expected = rows.to_h { |name, offset, size| [name, [offset, size]] }
      sample = record.new
      actual = record.fields.field_names.to_h { [_1, [sample.public_send(_1).rel_offset, sample.public_send(_1).num_bytes]] }
      (expected.keys | actual.keys).filter_map do |field|
        [field.to_s, expected[field], actual[field]] if expected[field] != actual[field]
      end
    end
  end
end
