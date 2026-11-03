# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_force_hint_names_mbr_when_filesystem_and_scheme_are_named
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "fat12/mbr/-", "-f", "disk.img"], expected: 1)
    assert_match(/; use --force to read it as mbr anyway\n\z/n, result.stderr)
  end

  def test_force_hint_names_mbr_when_only_scheme_is_named
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "mbr", "-f", "disk.img"], expected: 1)
    assert_match(/; use --force to read it as mbr anyway\n\z/n, result.stderr)
  end

  def test_force_lists_files_in_an_unsigned_mbr_partition
    write_unsigned_mbr_disk
    records = completed(invoke(arguments: ["t", "--json", "-F", "fat12/mbr/-", "--force", "-f", "disk.img"]))
    paths = records.select { _1.fetch("type") == "entry" }.map { _1.fetch("path") }
    assert_equal(["/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"].to_set, paths.to_set)
  end

  def test_force_extracts_identical_bytes_from_an_unsigned_mbr_partition
    write_unsigned_mbr_disk
    invoke(arguments: ["x", "-F", "fat12/mbr/-", "--force", "-f", "disk.img"])
    assert_equal("hello\n".b, (@root / "disk/HELLO.TXT").binread)
    assert_equal(@payload, (@root / "disk/nested/caf\u00e9.txt").binread)
  end

  def test_force_verification_reports_assumed_mbr_identity
    write_unsigned_mbr_disk
    result = invoke(arguments: ["W", "-F", "fat12/mbr/-", "--force", "-f", "disk.img"], expected: 1)
    assert_includes(result.stderr,
      "mbr identity was assumed: the MBR has no 0x55AA signature".b)
  end

  def test_force_accepts_a_stack_naming_only_mbr
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "mbr", "--force", "-f", "disk.img"])
    assert_includes(result.stdout.lines, "\"/HELLO.TXT\"\n".b)
  end

  private

  def write_unsigned_mbr_disk
    image_fixture(name: "partition.img", format: "fat12", size: "1M")
    volume = (@root / "partition.img").binread
    disk = mbr_disk(
      sectors: 2050,
      partitions: [MbrPartition.new(first_lba: 1, sector_count: 2048, type: 0x01, payload: volume)]
    )
    disk[510, 2] = "\0\0".b
    (@root / "disk.img").binwrite(disk)
  end
end
