# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "prop_check"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The model decodes any byte prefix of a boot sector without raising,
# reads each record only when all of it is present, and picks the BPB
# extension the way the specification defines BPB_FATSz32's use.
class TestFatModel < Minitest::Test
  Layout = FsimgSpec::Fat::Layout

  # A FAT16 BPB: 512-byte sectors, 4 per cluster, 4 reserved, 2 FATs of
  # 64 sectors, 512 root entries (32 sectors) and 65536 total sectors in
  # BPB_TotSec32. Data starts at sector 164, leaving 16343 clusters, which
  # the 16384-entry FATs hold.
  FAT16_BPB = {
    bs_jmp_boot: "\xEB\x3C\x90".b, bs_oem_name: "MSWIN4.1", bpb_byts_per_sec: 512, bpb_sec_per_clus: 4,
    bpb_rsvd_sec_cnt: 4, bpb_num_fats: 2, bpb_root_ent_cnt: 512, bpb_tot_sec16: 0, bpb_media: 0xF8,
    bpb_fat_sz16: 64, bpb_sec_per_trk: 63, bpb_num_heads: 255, bpb_hidd_sec: 0, bpb_tot_sec32: 65_536
  }.freeze
  FAT16_EXTENSION = {
    bs_drv_num: 0x80, bs_reserved1: 0, bs_boot_sig: 0x29, bs_vol_id: 0x1234_5678, bs_vol_lab: "NO NAME    ",
    bs_fil_sys_type: "FAT16   "
  }.freeze
  # A FAT32 BPB: BPB_FATSz16 is zero and BPB_FATSz32 is 1024, enough for
  # the 130812 clusters of 8 sectors after 2080 metadata sectors.
  FAT32_BPB = FAT16_BPB.merge(bs_jmp_boot: "\xEB\x58\x90".b, bpb_sec_per_clus: 8, bpb_rsvd_sec_cnt: 32,
                              bpb_root_ent_cnt: 0, bpb_fat_sz16: 0, bpb_tot_sec32: 1_048_576).freeze
  FAT32_EXTENSION = {
    bpb_fat_sz32: 1024, bpb_ext_flags: 0, bpb_fs_ver: 0, bpb_root_clus: 2, bpb_fs_info: 1, bpb_bk_boot_sec: 6,
    bpb_reserved: "\0" * 12, bs_drv_num: 0x80, bs_reserved1: 0, bs_boot_sig: 0x29, bs_vol_id: 0x1234_5678,
    bs_vol_lab: "NO NAME    ", bs_fil_sys_type: "FAT32   "
  }.freeze

  def self.boot_sector(common, extension_layout, extension) =
    (Layout::CommonBpb.new(common).to_binary_s + extension_layout.new(extension).to_binary_s)
    .ljust(510, "\0") + Layout::Signature.new(signature_word: 0xAA55).to_binary_s

  FAT16_SECTOR = boot_sector(FAT16_BPB, Layout::Fat16Extension, FAT16_EXTENSION)
  FAT32_SECTOR = boot_sector(FAT32_BPB, Layout::Fat32Extension, FAT32_EXTENSION)

  def test_fat16_boot_sector_decodes_its_fields_and_geometry
    model = decode(FAT16_SECTOR)
    geometry = model.geometry

    assert_equal 512, model.common.bpb_byts_per_sec
    assert_equal 0xF8, model.common.bpb_media
    assert_equal "FAT16   ", model.extension.bs_fil_sys_type
    assert_equal 0xAA55, model.signature.signature_word
    assert_equal({ first_data_sector: 164, cluster_count: 16_343, variant: "fat16", fat_fits: true },
                 %i[first_data_sector cluster_count variant fat_fits].to_h { [_1, geometry.public_send(_1)] })
  end

  def test_zero_fat_size_16_selects_the_fat32_extension
    model = decode(FAT32_SECTOR)

    assert_equal true, model.fat32_extension
    assert_equal 2, model.extension.bpb_root_clus
    assert_equal({ fat_size: 1024, cluster_count: 130_812, fat_fits: true },
                 %i[fat_size cluster_count fat_fits].to_h { [_1, model.geometry.public_send(_1)] })
  end

  def test_nonzero_fat_size_16_never_reads_the_fat32_extension
    model = decode(FAT16_SECTOR)

    assert_equal false, model.fat32_extension
    assert FsimgSpec.undefined?(model.geometry.fat_size_32)
  end

  # The model holds frozen plain values, so no rule can change what
  # another reads, and equal bytes decode to equal, hashable models.
  def test_records_are_frozen_plain_values
    model = decode(FAT16_SECTOR)

    assert model.common.frozen?
    refute_respond_to model.common, :bpb_media=
    assert_kind_of Integer, model.common.bpb_media
    assert_equal model.hash, decode(FAT16_SECTOR).hash
  end

  # The prefix lengths around each record's end: one byte short of a
  # record leaves it Undefined, and a complete record decodes.
  def test_each_record_decodes_only_when_complete
    presence = { 35 => [false, false, false], 36 => [true, false, false], 61 => [true, false, false],
                 62 => [true, true, false], 511 => [true, true, false], 512 => [true, true, true] }

    presence.each do |length, expected|
      model = decode(FAT16_SECTOR.byteslice(0, length))
      decoded = [model.common, model.extension, model.signature].map { !FsimgSpec.undefined?(_1) }
      assert_equal expected, decoded, "prefix of #{length} bytes"
    end
  end

  def test_fat32_extension_needs_ninety_bytes
    assert FsimgSpec.undefined?(decode(FAT32_SECTOR.byteslice(0, 89)).extension)
    refute FsimgSpec.undefined?(decode(FAT32_SECTOR.byteslice(0, 90)).extension)
  end

  # A sector too short for the BPB says nothing about the extension.
  def test_empty_image_decodes_to_undefined_records
    model = decode("".b)

    assert FsimgSpec.undefined?(model.common)
    assert FsimgSpec.undefined?(model.fat32_extension)
    assert FsimgSpec.undefined?(model.geometry.variant)
  end

  # The scopes beyond the boot sector follow from the tree: unknown, it
  # leaves them unclassified.
  def test_unknown_tree_leaves_its_scopes_unclassified
    assert_equal %w[directories files verification destination], decode(FAT16_SECTOR).unclassified_scopes
    assert_empty decode(FAT16_SECTOR).with(tree: []).unclassified_scopes
  end

  # A FAT32 volume's FSInfo record is read at the sector BPB_FSInfo
  # names, and is Undefined, leaving verification unclassified, when the
  # image ends before it or the FAT32 boot sector gives no variant. A
  # FAT16 volume, a FAT32 one whose BPB_FSInfo is 0xFFFF or outside the
  # reserved area, and a FAT32 extension over a FAT16 cluster count have
  # none to read.
  def test_fsinfo_is_read_where_bpb_fsinfo_points
    fsinfo = [0x4161_5252, "\0" * 480, 0x6141_7272, 0xFFFF_FFFF, 0xFFFF_FFFF, "\0" * 12, 0xAA55_0000].pack("Va480VVVa12V")
    pointing = lambda do |sector|
      self.class.boot_sector(FAT32_BPB, Layout::Fat32Extension, FAT32_EXTENSION.merge(bpb_fs_info: sector))
    end
    read = decode(FAT32_SECTOR + fsinfo).fsinfo

    assert_equal [0x4161_5252, 0x6141_7272, 0xAA55_0000], [read.fsi_lead_sig, read.fsi_struc_sig, read.fsi_trail_sig]
    assert_equal [0, 0x4161_5252], [decode(pointing.(2) + fsinfo + ("\0" * 512)).fsinfo.fsi_lead_sig,
                                    decode(pointing.(2) + ("\0" * 512) + fsinfo).fsinfo.fsi_lead_sig]
    short = decode(FAT32_SECTOR + fsinfo.byteslice(0, 511)).with(tree: [])
    assert_equal [true, ["verification"]], [FsimgSpec.undefined?(short.fsinfo), short.unclassified_scopes]
    assert_equal [nil] * 4, [decode(FAT16_SECTOR + fsinfo), decode(pointing.(0xFFFF) + fsinfo),
                             decode(pointing.(32) + fsinfo), decode(pointing.(0) + fsinfo)].map(&:fsinfo)
    small = self.class.boot_sector(FAT32_BPB.merge(bpb_tot_sec32: 65_536), Layout::Fat32Extension, FAT32_EXTENSION)
    unsized = self.class.boot_sector(FAT32_BPB.merge(bpb_sec_per_clus: 0), Layout::Fat32Extension, FAT32_EXTENSION)
    assert_equal [nil, true], [decode(small + fsinfo).fsinfo, FsimgSpec.undefined?(decode(unsized + fsinfo).fsinfo)]
  end

  # Every scope of an image names its inputs; input is the source model's.
  def test_every_image_scope_names_its_inputs
    assert_equal (FsimgSpec::SCOPE_STAGES.keys - ["input"]).sort, FsimgSpec::Fat::Model::SCOPE_INPUTS.keys.sort
  end

  # fsimg's info record must agree with every field the model derives,
  # and is not compared where the model has nothing to say.
  def test_info_record_is_compared_with_the_derived_geometry
    model = decode(FAT16_SECTOR)
    info = { "type" => "info", "format" => "fat16", "sector_bytes" => 512, "cluster_bytes" => 2048,
             "volume_bytes" => "33554432", "allocation_units" => "16343", "clean_flag" => false }

    assert_empty model.record_problems(accepted("info", [info]))
    assert_equal ["info reports cluster_bytes 512, the model derives 2048"],
                 model.record_problems(accepted("info", [info.merge("cluster_bytes" => 512)]))
    assert_empty model.record_problems(accepted("ls", []))
    assert_empty decode(FAT16_SECTOR.byteslice(0, 10)).record_problems(accepted("info", [info.merge("format" => "fat32")]))
  end

  # ls, extract and verify are compared with the known tree, and a tree
  # that differs everywhere is named in a few differences and a count.
  def test_records_are_compared_with_the_known_tree
    tree = FsimgSpec::KnownTree.of([FsimgSpec::TreeEntry.directory("d"),
                                    *(1..5).map { FsimgSpec::TreeEntry.file("d/f#{_1}", size: _1, seed: _1) }])
    model = decode(FAT16_SECTOR).with(tree:)
    listed = tree.map { { "type" => "entry", "kind" => _1.type, "path" => "/#{_1.path}", "size_bytes" => _1.size.to_s } }
    counts = { "type" => "verify", "complete" => true, "validation_scope" => "initialized-payload", "files" => "5",
               "directories" => "2", "logical_bytes" => "15" }
    as_file = tree.first.with(type: "file", size: 0, sha256: Digest::SHA256.hexdigest(""))

    assert_empty model.record_problems(accepted("ls", listed))
    assert_empty model.record_problems(accepted("extract", [], tree:))
    assert_empty model.record_problems(accepted("verify-deep", [counts]))
    assert_equal ['ls lacks file "/d/f1" of 1 bytes', 'ls lacks file "/d/f2" of 2 bytes', 'ls lacks file "/d/f3" of 3 bytes',
                  "ls lacks 2 more"],
                 model.record_problems(accepted("ls", listed.first(1)))
    assert_equal ['ls has unexpected file "/d/f1" of 1 bytes'], model.record_problems(accepted("ls", listed + listed.values_at(1)))
    malformed = [listed[0].merge("kind" => "link"), listed[1].merge("size_bytes" => "01")]
    assert_equal ['ls lacks directory "/d"', 'ls lacks file "/d/f1" of 1 bytes', 'ls has unexpected "link" "/d"',
                  'ls has unexpected file "/d/f1" of "01" bytes'],
                 model.record_problems(accepted("ls", malformed + listed.drop(2)))
    assert_equal ['extract lacks directory "d"', "extract has unexpected file \"d\" of 0 bytes SHA-256 #{as_file.sha256}"],
                 model.record_problems(accepted("extract", [], tree: [as_file, *tree.drop(1)]))
    assert_equal ['verify reports validation_scope "initialized-payload", not structural', 'verify reports directories "1", not 2',
                  'verify reports logical_bytes "14", not 15'],
                 model.record_problems(accepted("verify", [counts.merge("directories" => "1", "logical_bytes" => "14")]))
    assert_equal ["verify wrote 2 verify records, not one"], model.record_problems(accepted("verify", [counts, counts]))
  end

  # BPB field tuples, with BPB_FATSz16 often zero so the FAT32 branch is
  # taken, cut at random prefix lengths and at every record's end: decoding
  # and every derived quantity answer a value or Undefined, never raise.
  def test_random_boot_sectors_never_make_the_model_raise
    g = PropCheck::Generators
    edge = ->(max) { g.one_of(g.constant(0), g.constant(max), g.choose(0..max)) }
    fields = {
      bpb_byts_per_sec: edge.(0xffff), bpb_sec_per_clus: edge.(0xff), bpb_rsvd_sec_cnt: edge.(0xffff),
      bpb_num_fats: edge.(0xff), bpb_root_ent_cnt: edge.(0xffff), bpb_tot_sec16: edge.(0xffff),
      bpb_fat_sz16: edge.(0xffff), bpb_tot_sec32: edge.(0xffff_ffff), bpb_fat_sz32: edge.(0xffff_ffff),
      length: g.one_of(g.choose(0..600), *[0, 35, 36, 61, 62, 89, 90, 511, 512].map { g.constant(_1) })
    }

    PropCheck.forall(**fields) do |length:, bpb_fat_sz32:, **common|
      bpb = FAT16_BPB.merge(common)
      sector = TestFatModel.boot_sector(bpb, Layout::Fat32Extension, FAT32_EXTENSION.merge(bpb_fat_sz32:))
      model = decode(sector.byteslice(0, length))
      %i[variant fat_fits first_data_sector max_cluster].each { model.geometry.public_send(_1) }
      model.unclassified_scopes
      model.record_problems(accepted("info", [{ "type" => "info" }]))
    end
  end

  private

  def accepted(stage, records, tree: []) =
    FsimgSpec::StageResult.new(stage:, outcome: FsimgSpec::Outcome.new(kind: "accepted", records:), execution: nil,
                               problems: [], tree:, footprint: FsimgSpec::Footprint.new(files: 0, bytes: 0))

  def decode(bytes)
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      File.binwrite(path, bytes)
      FsimgSpec::Fat::Model.decode(FsimgSpec::ImageView.open(path))
    end
  end
end
