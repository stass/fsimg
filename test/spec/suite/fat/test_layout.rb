# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/fat"

# The layout records place every field where the Microsoft FAT
# specification does. The table below is transcribed by hand from that
# document, never from fsimg's C sources.
class TestFatLayout < Minitest::Test
  Layout = FsimgSpec::Fat::Layout

  # Field name, byte offset and size in bytes, as the specification's
  # tables give them (fat.pdf, sha256 c0aa36d1...d677): section 3.1 on
  # pp. 7-9, 3.2 on p. 10, 3.3 on pp. 11-12, the FSInfo table of section
  # 5 on pp. 21-22 and the directory entry table of section 6 on pp.
  # 23-24. Offsets of the boot sector records are from the start of the
  # volume, FSInfo's from the start of its sector and a directory entry's
  # from the start of the entry. The tables of appendices 1
  # and 2 place BPB_TotSec16 at 29; that is an erratum, and section 3.1
  # places it at 19. The boot code fields Reserved (62, 448 and 90, 420)
  # are not decoded and not listed.
  SPECIFICATION = {
    Layout::CommonBpb => [
      ["BS_jmpBoot", 0, 3], ["BS_OEMName", 3, 8], ["BPB_BytsPerSec", 11, 2],
      ["BPB_SecPerClus", 13, 1], ["BPB_RsvdSecCnt", 14, 2], ["BPB_NumFATs", 16, 1],
      ["BPB_RootEntCnt", 17, 2], ["BPB_TotSec16", 19, 2], ["BPB_Media", 21, 1],
      ["BPB_FATSz16", 22, 2], ["BPB_SecPerTrk", 24, 2], ["BPB_NumHeads", 26, 2],
      ["BPB_HiddSec", 28, 4], ["BPB_TotSec32", 32, 4]
    ],
    Layout::Fat16Extension => [
      ["BS_DrvNum", 36, 1], ["BS_Reserved1", 37, 1], ["BS_BootSig", 38, 1],
      ["BS_VolID", 39, 4], ["BS_VolLab", 43, 11], ["BS_FilSysType", 54, 8]
    ],
    Layout::Fat32Extension => [
      ["BPB_FATSz32", 36, 4], ["BPB_ExtFlags", 40, 2], ["BPB_FSVer", 42, 2],
      ["BPB_RootClus", 44, 4], ["BPB_FSInfo", 48, 2], ["BPB_BkBootSec", 50, 2],
      ["BPB_Reserved", 52, 12], ["BS_DrvNum", 64, 1], ["BS_Reserved1", 65, 1],
      ["BS_BootSig", 66, 1], ["BS_VolID", 67, 4], ["BS_VolLab", 71, 11],
      ["BS_FilSysType", 82, 8]
    ],
    Layout::Signature => [["Signature_word", 510, 2]],
    Layout::FsInfo => [
      ["FSI_LeadSig", 0, 4], ["FSI_Reserved1", 4, 480], ["FSI_StrucSig", 484, 4],
      ["FSI_Free_Count", 488, 4], ["FSI_Nxt_Free", 492, 4], ["FSI_Reserved2", 496, 12],
      ["FSI_TrailSig", 508, 4]
    ],
    Layout::DirectoryEntry => [
      ["DIR_Name", 0, 11], ["DIR_Attr", 11, 1], ["DIR_NTRes", 12, 1],
      ["DIR_CrtTimeTenth", 13, 1], ["DIR_CrtTime", 14, 2], ["DIR_CrtDate", 16, 2],
      ["DIR_LstAccDate", 18, 2], ["DIR_FstClusHI", 20, 2], ["DIR_WrtTime", 22, 2],
      ["DIR_WrtDate", 24, 2], ["DIR_FstClusLO", 26, 2], ["DIR_FileSize", 28, 4]
    ]
  }.freeze

  def test_every_field_lies_where_the_specification_places_it
    assert_empty mismatches(SPECIFICATION)
  end

  # The seeded failure: swapping two offsets in the table must be found.
  def test_two_swapped_offsets_are_found
    common = SPECIFICATION.fetch(Layout::CommonBpb)
    swapped = common.map do |name, offset, size|
      case name
      in "BPB_SecPerTrk" then [name, 26, size]
      in "BPB_NumHeads" then [name, 24, size]
      else [name, offset, size]
      end
    end

    assert_equal %w[bpb_num_heads bpb_sec_per_trk], mismatches(SPECIFICATION.merge(Layout::CommonBpb => swapped)).map(&:first).sort
  end

  def test_snake_case_names_follow_the_specification_names
    names = %w[BS_jmpBoot BS_OEMName BPB_NumFATs BPB_FATSz16 BS_VolID DIR_NTRes DIR_FstClusHI FSI_Free_Count]

    assert_equal %i[bs_jmp_boot bs_oem_name bpb_num_fats bpb_fat_sz16 bs_vol_id dir_nt_res dir_fst_clus_hi fsi_free_count],
                 names.map { snake(_1) }
  end

  private

  # The fields whose offset or size differs from table, and the fields
  # one side has and the other lacks, as [field, expected, actual].
  def mismatches(table)
    offsets = Layout.field_offsets
    table.flat_map do |record, rows|
      expected = rows.to_h { |name, offset, size| [snake(name), [offset, size]] }
      actual = record.fields.field_names.to_h { [_1, [offsets.fetch(record).fetch(_1), field_size(record, _1)]] }
      (expected.keys | actual.keys).filter_map do |field|
        [field.to_s, expected[field], actual[field]] if expected[field] != actual[field]
      end
    end
  end

  def field_size(record, field) = record.new.public_send(field).num_bytes

  # BPB_BytsPerSec -> bpb_byts_per_sec. The specification writes one
  # plural acronym, BPB_NumFATs, which the case split would cut wrongly.
  def snake(name) =
    name.sub("FATs", "Fats")
        .gsub(/([A-Z]+)([A-Z][a-z])/, '\1_\2')
        .gsub(/([a-z\d])([A-Z])/, '\1_\2')
        .downcase.squeeze("_").to_sym
end
