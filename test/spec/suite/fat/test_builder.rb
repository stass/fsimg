# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "open3"
require "rbconfig"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The builder's own refusals and its FAT32 boot records: it renders any
# layout whose records stay apart inside the reserved region, and writes
# the backup boot record only where BPB_BkBootSec names one.
class TestFatBuilder < Minitest::Test
  Fat = FsimgSpec::Fat
  FAT32 = { clusters: 65_525 }.freeze
  # Larger than any image these tests build; each image is sparse.
  MAX_BYTES = 1 << 30

  # FSInfo on the boot sector or the backup, a backup over FSInfo, an
  # FSInfo copy past the reserved region, a three-sector backup record
  # reaching the first FAT, and 128-byte sectors that put FSInfo inside
  # the 512-byte boot sector.
  def test_records_that_overlap_or_leave_the_reserved_region_are_refused
    refused = [{ fs_info: 0 }, { fs_info: 6 }, { backup_boot: 1 }, { fs_info: 26 }, { backup_boot: 30 },
               { bytes_per_sector: 128, reserved_sectors: 128 }]

    refused.each do |parameters|
      assert_raises(Fat::BuilderError, parameters.inspect) { Fat::Builder::Layout.new(FAT32.merge(parameters)) }
    end
  end

  # A recipe can name any parameter value; one its field cannot hold,
  # such as a negative tail that would shrink the declared image below
  # what the builder writes, is refused before anything is written.
  def test_parameters_their_fields_cannot_hold_are_refused
    refused = [{ tail_sectors: -1 }, { clusters: 0 }, { clusters: -5 }, { fat_size: 1 << 32 }, { media: 256 },
               { root_cluster: 0 }, { root_cluster: 65_527 }, { sectors_per_cluster: "1" }, { clusters: 2.5 }]

    refused.each do |parameters|
      assert_raises(Fat::BuilderError, parameters.inspect) do
        Dir.mktmpdir { write_image(File.join(_1, "image.img"), parameters: FAT32.merge(parameters)) }
      end
    end
  end

  # Entries 0 and 1, then a file's chain 2 -> 3 -> 4, packed as section
  # 4.1 packs FAT12 entries: F8 FF FF, 03 40 00, FF 0F.
  def test_fat12_entries_share_their_middle_bytes
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      tree = [FsimgSpec::TreeEntry.file("THREE.BIN", size: 1500, seed: 1)]
      layout = write_image(path, parameters: { clusters: 100 }, tree:)

      assert_equal "f8ffff034000ff0f00", File.binread(path, 9, layout.fat_offset(0)).unpack1("H*")
      assert_equal File.binread(path, 9, layout.fat_offset(0)), File.binread(path, 9, layout.fat_offset(1))
    end
  end

  def test_backup_boot_zero_writes_no_backup_record
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      layout = write_image(path, parameters: FAT32.merge(fs_info: 31, backup_boot: 0))
      image = File.binread(path, 32 * 512)

      assert_equal "\0".b * 512, image.byteslice(layout.sector_offset(6), 512)
      assert_equal [0x4161_5252], image.byteslice(layout.sector_offset(31), 4).unpack("V")
    end
  end

  def test_the_backup_record_copies_the_boot_sector_and_fsinfo
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      layout = write_image(path, parameters: FAT32.merge(fs_info: 2))
      image = File.binread(path, 32 * 512)
      sector = ->(number) { image.byteslice(layout.sector_offset(number), 512) }

      assert_equal sector.(0), sector.(6)
      assert_equal sector.(2), sector.(8)
    end
  end

  # The builder runs in-process, under no file-size limit, so a recipe's
  # geometry could otherwise ask for an image of terabytes.
  def test_an_image_larger_than_max_bytes_is_refused_before_anything_is_written
    layout = Fat::Builder::Layout.new({ clusters: 100 })
    image_bytes = layout.total_sectors * layout.bytes_per_sector

    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      assert_raises(Fat::BuilderError) do
        Fat::Builder.write(path, parameters: { clusters: 100 }, tree: [], max_bytes: image_bytes - 1)
      end
      refute File.exist?(path)

      Fat::Builder.write(path, parameters: { clusters: 100 }, tree: [], max_bytes: image_bytes)
      assert_equal image_bytes, File.size(path)
    end
  end

  # Section 3.5 divides the data sectors by the cluster size, rounding
  # down; a tail of a whole cluster would add a cluster to that count.
  def test_tail_sectors_that_would_add_a_cluster_are_refused
    layout = Fat::Builder::Layout.new({ clusters: 100, sectors_per_cluster: 4, tail_sectors: 3 })
    assert_equal 3, layout.total_sectors - layout.first_data_sector - (100 * 4)

    assert_raises(Fat::BuilderError) do
      Fat::Builder::Layout.new({ clusters: 100, sectors_per_cluster: 4, tail_sectors: 4 })
    end
  end

  # LDIR_Ord numbers a name's entries below LAST_LONG_ENTRY (0x40), so a
  # name fits at most 63 entries of 13 units. A longer name than the
  # specification's 255 characters still fits, so a reader's limit can
  # be tested; one that would reach 0x40 is refused.
  def test_a_long_name_beyond_63_entries_is_refused
    longest = "a" * (63 * 13)

    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      layout = write_image(path, parameters: { clusters: 100 }, tree: [file(longest)])
      assert_equal 0x40 | 63, File.binread(path, 1, layout.root_offset).unpack1("C")

      assert_raises(Fat::BuilderError) do
        write_image(File.join(directory, "longer.img"), parameters: { clusters: 100 }, tree: [file("#{longest}a")])
      end
    end
  end

  # HELLO. would fit the short name HELLO, but that spells HELLO, so the
  # name takes a long-name entry; HELLO.TXT is spelled by its short name
  # and takes none.
  def test_a_name_its_short_name_cannot_spell_takes_a_long_name
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      layout = write_image(path, parameters: { clusters: 100 }, tree: [file("HELLO."), file("HELLO.TXT")])
      root = File.binread(path, 3 * 32, layout.root_offset)
      long, short, plain = root.scan(/.{32}/mn)

      assert_equal [0x0F, 0x20, 0x20], [long, short, plain].map { _1.getbyte(11) }
      assert_equal "HELLO.", long_name_part(long)
      assert_equal ["HELLO~1    ", "HELLO   TXT"], [short, plain].map { _1.byteslice(0, 11) }
    end
  end

  # basis~N keeps eight characters by cutting the basis; at eight digits
  # nothing of it is left to cut, so no short name can be formed.
  def test_a_numbered_short_name_beyond_seven_digits_is_refused
    assert_equal "~9999999TXT".b, Fat::Builder::ShortNames.numbered("BASIS", "TXT", 9_999_999)

    assert_raises(Fat::BuilderError) { Fat::Builder::ShortNames.numbered("BASIS", "TXT", 10_000_000) }
  end

  # The basis-name algorithm (section 6.1, the generation of short names):
  # upper case, spaces and leading periods stripped, the primary part up
  # to the first period cut to fit its numeric tail, the extension after
  # the last period, and any character no short name holds lossily "_".
  # The numeric tail follows unless the name in upper case is itself a
  # valid 8.3 name, as readme is.
  def test_generated_short_names_follow_the_basis_name_algorithm
    names = ["name file.txt", ".profile", "a.b.c", "x+y", "readme"]

    assert_equal ["NAMEFI~1TXT", "PROFIL~1   ", "A~1     C  ", "X_Y~1      ", "README     "],
                 names.map { Fat::Builder::ShortNames.assign([_1]).first.short }
  end

  # Every name of an entry, long or short, is a key of its directory, and
  # a key that two entries share makes the directory ambiguous
  # (specs/design-fat-reader.md#directory-preparation). A generated short
  # name therefore never spells a sibling's name, whichever comes first:
  # abcdef~1 keeps ABCDEF~1 from abc def, and the literal NAMEFI~1.TXT
  # keeps its own short name from the long name before it.
  def test_a_generated_short_name_never_spells_a_sibling_name
    spelled = lambda { |names|
      Fat::Builder::ShortNames.assign(names).map { Fat::Builder::ShortNames.spelled(_1.short) }
    }

    assert_equal ["ABCDEF~2", "ABCDEF~1"], spelled.(["abc def", "abcdef~1"])
    assert_equal ["NAMEFI~2.TXT", "NAMEFI~1.TXT"], spelled.(["NAME FILE.TXT", "NAMEFI~1.TXT"])
  end

  # The builder is evidence about the geometry decoder only if it does
  # not share it: it must choose the variant from section 3.5's cluster
  # thresholds with fsimg_spec/fat/geometry.rb never loaded.
  def test_the_builder_selects_variants_without_the_geometry_decoder
    program = <<~RUBY
      require "fsimg_spec"
      require "fsimg_spec/fat/builder"
      abort "geometry is loaded" if defined?(FsimgSpec::Fat::Geometry)
      puts [4084, 4085, 65_524, 65_525].map { FsimgSpec::Fat::Builder::Layout.new({ clusters: _1 }).variant }.join(" ")
    RUBY
    variants, errors, status = Open3.capture3(RbConfig.ruby, "-I", File.expand_path("../../lib", __dir__), "-e", program)

    assert status.success?, errors
    assert_equal "fat12 fat16 fat16 fat32\n", variants
  end

  private

  def write_image(path, parameters:, tree: []) = Fat::Builder.write(path, parameters:, tree:, max_bytes: MAX_BYTES)

  def file(name) = FsimgSpec::TreeEntry.file(name, size: 0, content: "zero")

  # The units one long-name entry holds (section 7: LDIR_Name1, 2 and 3),
  # up to the 0x0000 that ends the name.
  def long_name_part(entry)
    units = [entry.byteslice(1, 10), entry.byteslice(14, 12), entry.byteslice(28, 4)].join.unpack("v*")
    units.take_while { _1 != 0 }.pack("U*")
  end
end
