# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "prop_check"
require "fsimg_spec"
require "fsimg_spec/fat"

# FAT geometry derived from the BPB: the variant thresholds, FAT capacity
# and the 16/32-bit field selection, and no input that makes it raise.
class TestFatGeometry < Minitest::Test
  # A FAT16-sized volume with 512-byte sectors, one sector per cluster,
  # one reserved sector, two FATs of 256 sectors and a 512-entry root
  # directory (32 sectors): metadata occupies 1 + 512 + 32 = 545 sectors.
  BASE = FsimgSpec::Fat::Geometry.new(
    bytes_per_sector: 512, sectors_per_cluster: 1, reserved_sectors: 1, fat_count: 2, root_entries: 512,
    total_sectors_16: 0, total_sectors_32: 545 + 20_000, fat_size_16: 256, fat_size_32: 0
  )
  METADATA_SECTORS = 545

  def test_cluster_count_selects_the_variant_at_each_threshold
    variants = [4084, 4085, 65_524, 65_525].to_h { [_1, with_clusters(_1).variant] }

    assert_equal({ 4084 => "fat12", 4085 => "fat16", 65_524 => "fat16", 65_525 => "fat32" }, variants)
  end

  def test_partial_cluster_at_the_end_of_the_volume_does_not_count
    geometry = BASE.with(sectors_per_cluster: 4, total_sectors_32: METADATA_SECTORS + (4085 * 4) + 3)

    assert_equal 4085, geometry.cluster_count
  end

  def test_fat16_capacity_fits_exactly_and_fails_one_entry_short
    # 255 FAT sectors hold 65280 entries, enough for clusters 2..65279.
    exact = BASE.with(fat_size_16: 255, total_sectors_32: 1 + 510 + 32 + 65_278)
    short = exact.with(total_sectors_32: exact.total_sectors_32 + 1)

    assert_equal({ cluster_count: 65_278, fat_bytes_required: 130_560, fat_bytes: 130_560, fat_fits: true }, capacity(exact))
    assert_equal({ cluster_count: 65_279, fat_bytes_required: 130_562, fat_bytes: 130_560, fat_fits: false }, capacity(short))
  end

  def test_fat12_capacity_packs_two_entries_into_three_bytes
    # 12 FAT sectors hold 6144 bytes, 4096 packed entries: clusters 2..4095.
    exact = BASE.with(fat_size_16: 12, total_sectors_32: 1 + 24 + 32 + 4084)
    odd = BASE.with(fat_size_16: 12, total_sectors_32: 1 + 24 + 32 + 4083)

    assert_equal({ cluster_count: 4084, fat_bytes_required: 6129, fat_bytes: 6144, fat_fits: true }, capacity(exact))
    assert_equal 6128, odd.fat_bytes_required, "4085 entries take 6127.5 bytes, rounded up"
  end

  # One 512-byte FAT12 sector holds exactly 341 packed entries: clusters
  # 2..340 fit, and one more cluster needs 513 bytes.
  def test_fat12_capacity_fits_exactly_and_fails_one_entry_short
    exact = BASE.with(fat_size_16: 1, total_sectors_32: 1 + 2 + 32 + 339)
    short = exact.with(total_sectors_32: exact.total_sectors_32 + 1)

    assert_equal({ cluster_count: 339, fat_bytes_required: 512, fat_bytes: 512, fat_fits: true }, capacity(exact))
    assert_equal({ cluster_count: 340, fat_bytes_required: 513, fat_bytes: 512, fat_fits: false }, capacity(short))
  end

  # 512 FAT32 sectors hold 65536 four-byte entries: clusters 2..65535.
  def test_fat32_capacity_fits_exactly_and_fails_one_entry_short
    exact = BASE.with(root_entries: 0, fat_size_16: 0, fat_size_32: 512, total_sectors_32: 1 + 1024 + 65_534)
    short = exact.with(total_sectors_32: exact.total_sectors_32 + 1)

    assert_equal "fat32", exact.variant
    assert_equal({ cluster_count: 65_534, fat_bytes_required: 262_144, fat_bytes: 262_144, fat_fits: true }, capacity(exact))
    assert_equal({ cluster_count: 65_535, fat_bytes_required: 262_148, fat_bytes: 262_144, fat_fits: false }, capacity(short))
  end

  def test_sixteen_bit_fields_win_unless_zero
    assert_equal [1000, 9], BASE.with(total_sectors_16: 1000, fat_size_16: 9, fat_size_32: 77).then { [_1.total_sectors, _1.fat_size] }
    assert_equal [20_545, 77], BASE.with(fat_size_16: 0, fat_size_32: 77).then { [_1.total_sectors, _1.fat_size] }
  end

  def test_fat12_16_boot_sector_with_a_zero_fat_size_has_no_fat_size
    geometry = BASE.with(fat_size_16: 0, fat_size_32: FsimgSpec::Undefined.new(reason: "no FAT32 extension"))

    assert FsimgSpec.undefined?(geometry.fat_size)
    assert FsimgSpec.undefined?(geometry.variant)
  end

  def test_zero_divisors_and_oversized_metadata_are_undefined
    undefined = {
      "zero bytes per sector" => BASE.with(bytes_per_sector: 0).root_dir_sectors,
      "zero sectors per cluster" => BASE.with(sectors_per_cluster: 0).cluster_count,
      "metadata beyond the volume" => BASE.with(total_sectors_32: 100).data_sectors
    }

    undefined.each { |reason, value| assert FsimgSpec.undefined?(value), reason }
  end

  # Every field across its full encoded width: the derivations answer a
  # value or Undefined and never raise.
  # Each field is drawn at zero, at its maximum or between, and
  # fat_size_32 is sometimes Undefined, as for a FAT12/16 boot sector.
  def test_random_bpb_fields_never_make_the_geometry_raise
    g = PropCheck::Generators
    edge = ->(max) { g.one_of(g.constant(0), g.constant(max), g.choose(0..max)) }
    fields = {
      bytes_per_sector: edge.(0xffff), sectors_per_cluster: edge.(0xff), reserved_sectors: edge.(0xffff),
      fat_count: edge.(0xff), root_entries: edge.(0xffff), total_sectors_16: edge.(0xffff),
      total_sectors_32: edge.(0xffff_ffff), fat_size_16: edge.(0xffff),
      fat_size_32: g.one_of(edge.(0xffff_ffff), g.constant(FsimgSpec::Undefined.new(reason: "FAT12/16 boot sector")))
    }

    PropCheck.forall(**fields) do |**values|
      geometry = FsimgSpec::Fat::Geometry.new(**values)
      %i[total_sectors fat_size root_dir_sectors first_data_sector data_sectors cluster_count variant
         max_cluster fat_bytes_required fat_bytes fat_fits].each { geometry.public_send(_1) }
    end
  end

  private

  def with_clusters(clusters) = BASE.with(total_sectors_32: METADATA_SECTORS + clusters)

  def capacity(geometry) = %i[cluster_count fat_bytes_required fat_bytes fat_fits].to_h { [_1, geometry.public_send(_1)] }
end
