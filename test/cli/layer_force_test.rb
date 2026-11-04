# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_force_hint_names_mbr_when_filesystem_and_scheme_are_named
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "fat12/mbr/-", "-f", "disk.img"], expected: 1)
    assert_match(/; use -F fat12\/mbr\/- --partition N --force to read it as mbr anyway\n\z/n, result.stderr)
  end

  def test_force_hint_names_mbr_when_only_scheme_is_named
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "mbr", "-f", "disk.img"], expected: 1)
    assert_match(/; use --force to read it as mbr anyway\n\z/n, result.stderr)
  end

  def test_force_lists_files_in_an_unsigned_mbr_partition
    write_unsigned_mbr_disk
    records = completed(invoke(arguments: ["t", "--json", "-F", "fat12/mbr/-", "--partition", "1", "--force", "-f", "disk.img"]))
    paths = records.select { _1.fetch("type") == "entry" }.map { _1.fetch("path") }
    assert_equal(["/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"].to_set, paths.to_set)
  end

  def test_force_extracts_identical_bytes_from_an_unsigned_mbr_partition
    write_unsigned_mbr_disk
    invoke(arguments: ["x", "-F", "fat12/mbr/-", "--partition", "1", "--force", "-f", "disk.img"])
    assert_equal("hello\n".b, (@root / "disk/HELLO.TXT").binread)
    assert_equal(@payload, (@root / "disk/nested/caf\u00e9.txt").binread)
  end

  def test_force_verification_reports_assumed_mbr_identity
    write_unsigned_mbr_disk
    result = invoke(arguments: ["W", "-F", "fat12/mbr/-", "--partition", "1", "--force", "-f", "disk.img"], expected: 1)
    assert_includes(result.stderr,
      "mbr identity was assumed: the MBR has no 0x55AA signature".b)
  end

  def test_force_accepts_a_stack_naming_only_mbr
    write_unsigned_mbr_disk
    result = invoke(arguments: ["t", "-F", "mbr", "--force", "-f", "disk.img"])
    assert_includes(result.stdout.lines, "\"/HELLO.TXT\"\n".b)
  end

  def test_force_reads_vhd_with_missing_cookie_but_refuses_verification
    image_fixture(name: "volume.img", format: "fat12", size: "1M")
    image = vhd_image(disk: (@root / "volume.img").binread)
    footer = image.byteslice(-512, 512)
    footer[0, 8] = "\0".b * 8
    footer[64, 4] = "\0".b * 4
    footer[64, 4] = [(~footer.bytes.sum & 0xFFFFFFFF)].pack("L>")
    image[-512, 512] = footer
    (@root / "disk.vhd").binwrite(image)

    result = invoke(arguments: ["t", "-F", "fat12/-/vhd", "-f", "disk.vhd"], expected: 1)
    assert_match(/; use -F fat12\/-\/vhd --force to read it as vhd anyway\n\z/n, result.stderr)

    records = completed(invoke(arguments: ["t", "--json", "-F", "fat12/-/vhd", "--force", "-f", "disk.vhd"]))
    paths = records.select { _1.fetch("type") == "entry" }.map { _1.fetch("path") }
    assert_equal(["/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"].to_set, paths.to_set)

    result = invoke(arguments: ["W", "-F", "fat12/-/vhd", "--force", "-f", "disk.vhd"], expected: 1)
    assert_includes(result.stderr,
      "vhd identity was assumed: the footer has no conectix cookie".b)
  end

  def test_force_reads_gzip_with_missing_magic_but_refuses_verification
    image_fixture(name: "volume.img", format: "fat12", size: "1M")
    image = gzip_bytes((@root / "volume.img").binread)
    image[0, 2] = "\0\0".b
    (@root / "disk.gz").binwrite(image)

    result = invoke(arguments: ["t", "-z", "-F", "fat12", "-f", "disk.gz"], expected: 1)
    assert_match(/; use -F fat12\/- --force to read it as gzip anyway\n\z/n, result.stderr)

    [[], ["-F", "fat12/-"]].each do |stack|
      records = completed(invoke(arguments: ["t", "-z", "--force", "--json", *stack, "-f", "disk.gz"]))
      paths = records.select { _1.fetch("type") == "entry" }.map { _1.fetch("path") }
      assert_equal(["/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"].to_set, paths.to_set)
    end

    result = invoke(arguments: ["W", "-z", "--force", "-f", "disk.gz"], expected: 1)
    assert_includes(result.stderr,
      "gzip identity was assumed: the first member has no 1f 8b magic".b)
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
