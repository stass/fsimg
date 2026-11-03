# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_isohybrid_gpt_lists_iso_and_reports_table
    isohybrid_image(scheme: "gpt")

    result = invoke(arguments: ["t", "-f", "hybrid.img"])

    assert_includes(result.stdout, "ISO.TXT".b)
    refute_includes(result.stdout, "FAT.TXT".b)
    assert_equal(hybrid_note(scheme: "gpt"), image_stderr(result))
  end

  def test_isohybrid_forced_gpt_lists_fat_without_note
    isohybrid_image(scheme: "gpt")

    result = invoke(arguments: ["t", "-F", "gpt", "-f", "hybrid.img"])

    assert_includes(result.stdout, "FAT.TXT".b)
    refute_includes(result.stdout, "ISO.TXT".b)
    assert_empty(image_stderr(result))
  end

  def test_isohybrid_forced_bare_iso_lists_iso_without_note
    isohybrid_image(scheme: "gpt")

    result = invoke(arguments: ["t", "-F", "iso9660/-", "-f", "hybrid.img"])

    assert_includes(result.stdout, "ISO.TXT".b)
    refute_includes(result.stdout, "FAT.TXT".b)
    assert_empty(image_stderr(result))
  end

  def test_isohybrid_info_reports_table
    isohybrid_image(scheme: "gpt")

    result = invoke(arguments: ["i", "-f", "hybrid.img"])

    assert_includes(result.stdout, "Format:       ISO9660".b)
    assert_equal(hybrid_note(scheme: "gpt"), image_stderr(result))
  end

  def test_isohybrid_json_listing_reports_table
    isohybrid_image(scheme: "gpt")

    result = invoke(arguments: ["t", "--json", "-f", "hybrid.img"])

    assert_includes(result.stdout, "ISO.TXT".b)
    refute_includes(result.stdout, "FAT.TXT".b)
    completed(result)
    assert_equal(hybrid_note(scheme: "gpt"), image_stderr(result))
  end

  def test_isohybrid_mbr_lists_iso_and_reports_table
    isohybrid_image(scheme: "mbr")

    result = invoke(arguments: ["t", "-f", "hybrid.img"])

    assert_includes(result.stdout, "ISO.TXT".b)
    refute_includes(result.stdout, "FAT.TXT".b)
    assert_equal(hybrid_note(scheme: "mbr"), image_stderr(result))
  end

  def test_fat_with_zero_lba_mbr_entry_lists_without_note
    fat_with_mbr_entry(first_lba: 0)

    result = invoke(arguments: ["t", "-f", "fat-mbr.img"])

    assert_includes(result.stdout, "HELLO.TXT".b)
    assert_empty(image_stderr(result))
  end

  def test_fat_with_nonzero_lba_mbr_entry_stays_ambiguous
    fat_with_mbr_entry(first_lba: 1)

    result = invoke(arguments: ["t", "-f", "fat-mbr.img"], expected: 1)

    assert_includes(result.stderr,
      "the disk matches both a partition table and a filesystem".b)
    refute_includes(result.stderr, "fsimg: note:".b)
  end

  private

  # Match image diagnostics exactly after the optional host sandbox warning.
  def image_stderr(result) =
    result.stderr.sub(/\Afsimg: warning: OS sandbox unavailable; continuing without confinement\n/n, "".b)

  def hybrid_note(scheme:) =
    "fsimg: note: the disk also carries a #{scheme} partition table; use -F #{scheme} to read its partitions\n".b

  def isohybrid_image(scheme:)
    %w[iso fat].each do |kind|
      directory = @root / kind
      directory.mkdir
      (directory / "#{kind.upcase}.TXT").binwrite("#{kind}\n".b)
      arguments = ["c", "-R", "-F", kind == "iso" ? "iso9660" : "fat12"]
      arguments += ["-s", "1M"] if kind == "fat"
      invoke(arguments: [*arguments, "-f", "#{kind}.img", "-C", kind, "."])
    end
    iso = (@root / "iso.img").binread
    fat = (@root / "fat.img").binread
    # The FAT partition follows the entire ISO at a 1 MiB boundary. The
    # table lives in ISO's zero system area; the backup GPT follows FAT.
    first = (iso.bytesize + (1 << 20) - 1) / (1 << 20) * 2048
    count = fat.bytesize / 512
    sectors = first + count + 34
    disk = if scheme == "gpt"
      gpt_disk(sectors: sectors, partitions: [
        GptPartition.new(first_lba: first, last_lba: first + count - 1,
          type: ESP_TYPE, name: "EFI", payload: fat)
      ])
    else
      mbr_disk(sectors: sectors, partitions: [
        MbrPartition.new(first_lba: first, sector_count: count, type: 0x01, payload: fat)
      ])
    end
    table_bytes = scheme == "gpt" ? 34 * 512 : 512
    disk[table_bytes...iso.bytesize] = iso[table_bytes..]
    (@root / "hybrid.img").binwrite(disk)
  end

  def fat_with_mbr_entry(first_lba:)
    image_fixture(name: "fat.img", format: "fat12", size: "1M")
    fat = (@root / "fat.img").binread
    sectors = fat.bytesize / 512
    table = mbr_disk(sectors: sectors, partitions: [
      MbrPartition.new(first_lba: first_lba, sector_count: sectors - first_lba,
        type: 0x01, payload: "".b)
    ])
    fat[446...512] = table[446...512]
    (@root / "fat-mbr.img").binwrite(fat)
  end
end
