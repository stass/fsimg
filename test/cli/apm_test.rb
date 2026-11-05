# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_apm_lists_extracts_verifies_and_selects_hfsplus
    apm_fixture
    info = invoke(arguments: ["i", "-S", "off", "-f", "apple.img"]).stdout
    assert_includes(info, "hfsplus/apm")
    assert_match(/Scheme:\s+APM, 512-byte sectors/, info)
    assert_includes(info, "Apple_HFS")
    assert_includes(info, "Macintosh")

    records = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "apple.img"]))
    stack = records.find { _1.fetch("type") == "stack" }
    assert_equal("hfsplus/apm", stack.fetch("stack"))
    assert_equal("apm", stack.fetch("scheme"))
    assert_equal(512, stack.fetch("sector_bytes"))
    partitions = records.select { _1.fetch("type") == "partition" }
    assert_equal([1, 2], partitions.map { _1.fetch("index") })
    volume = partitions.find { _1.fetch("index") == 2 }
    assert_equal("Apple_HFS", volume.fetch("apm_type"))
    assert_equal("Macintosh", volume.fetch("name"))
    assert_equal("hfs", volume.fetch("partition_type"))
    assert_equal("32768", volume.fetch("offset_bytes"))
    assert_equal("0", volume.fetch("missing_bytes"))

    expected = listing(image: "volume.img")
    assert_includes(expected, '"/HELLO.TXT"')
    [[], ["-F", "hfsplus/apm"], ["-F", "apm", "--partition=2"],
      ["--partition=type=Apple_HFS"]].each do |selector|
      result = invoke(arguments: ["t", "-S", "off", "-f", "apple.img", *selector])
      assert_equal(expected, result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true), selector.inspect)
    end
    (@root / "restore").mkdir
    invoke(arguments: ["x", "-S", "off", "-f", "apple.img", "-C", "restore"])
    assert_equal("hello\n".b, (@root / "restore/apple/HELLO.TXT").binread)
    assert_equal(@payload, (@root / "restore/apple/nested/cafe\u0301.txt").binread)
    invoke(arguments: ["W", "-d", "-S", "off", "-f", "apple.img"])
    error = invoke(arguments: ["t", "-S", "off", "-f", "apple.img", "--partition=1"], expected: 1)
    assert_includes(error.stderr, "not a volume")
  end

  def test_apm_truncated_partition_reports_loss_lists_and_refuses_verification
    disk = apm_fixture
    # Cut a partial sector from the volume tail. The primary header,
    # catalog and all small file data remain within the slice.
    cut = 64 * 512 + (@root / "volume.img").stat.size - 123
    (@root / "cut.img").binwrite(disk[0...cut])
    records = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "cut.img"]))
    stack = records.find { _1.fetch("type") == "stack" }
    volume = records.find { _1.fetch("type") == "partition" && _1.fetch("index") == 2 }
    assert_equal("123", stack.fetch("missing_bytes"))
    assert_equal("123", volume.fetch("missing_bytes"))
    info = invoke(arguments: ["i", "-S", "off", "-f", "cut.img"]).stdout
    assert_includes(info, "123 bytes")
    assert_equal(listing(image: "apple.img"), listing(image: "cut.img"))
    error = failed(invoke(arguments: ["W", "--json", "-S", "off", "-f", "cut.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
  end

  def test_apm_inside_dmg_reads_and_verifies_the_same_volume
    disk = apm_fixture
    (@root / "apple.dmg").binwrite(dmg_image(disk: disk, regions: [[0, disk.bytesize / 512]]))
    assert_equal(listing(image: "apple.img"), listing(image: "apple.dmg"))
    records = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "apple.dmg"]))
    stack = records.find { _1.fetch("type") == "stack" }
    assert_equal("hfsplus/apm/dmg", stack.fetch("stack"))
    invoke(arguments: ["W", "-d", "-S", "off", "-f", "apple.dmg"])
  end

  def test_apm_creation_is_refused_as_read_only
    result = invoke(arguments: ["c", "-S", "off", "-f", "new.img", "-F", "fat32/apm",
      "-C", "source", "."], expected: 1)
    assert_match(/read.only|does not support writ/, result.stderr)
    refute_path_exists(@root / "new.img")
  end

  def test_apm_json_pins_partition_keys_and_scheme_specific_fields
    apm_fixture
    partition_fixtures
    volume = (@root / "part.img").binread
    (@root / "mbr.img").binwrite(mbr_disk(sectors: 8192, partitions: [
      MbrPartition.new(first_lba: 2048, sector_count: 2048, type: 0x01, payload: volume)
    ]))
    keys = %w[schema type index partition_type apm_type type_guid guid name mbr_type active
      offset_bytes length_bytes missing_bytes filesystem_claim format selected].sort

    { "apm" => "apple.img", "gpt" => "gpt.img", "mbr" => "mbr.img" }.each do |scheme, image|
      records = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", image]))
      stack = records.find { _1.fetch("type") == "stack" }
      assert_equal(scheme, stack.fetch("scheme"))
      partition = records.find do |record|
        record.fetch("type") == "partition" && record.fetch("index") == ((scheme == "apm") ? 2 : 1)
      end
      assert_equal(keys, partition.keys.sort, scheme)
      if scheme == "apm"
        assert_equal("Apple_HFS", partition.fetch("apm_type"))
        %w[type_guid guid mbr_type active].each { assert_nil(partition.fetch(_1), _1) }
        %w[mbr_disk_id disk_guid].each { assert_nil(stack.fetch(_1), _1) }
      else
        assert_nil(partition.fetch("apm_type"), scheme)
      end
    end
  end

  private

  def apm_fixture
    image_fixture(name: "volume.img", format: "hfsplus", size: "2M")
    volume = (@root / "volume.img").binread
    blocks = volume.bytesize / 512
    disk = ("\0".b * (64 * 512)) + volume + ("\0".b * 512)
    disk[0, 2] = "ER".b
    disk[2, 2] = [512].pack("n")
    disk[4, 4] = [disk.bytesize / 512].pack("N")
    [
      { start: 1, blocks: 3, name: "Apple", type: "Apple_partition_map" },
      { start: 64, blocks: blocks, name: "Macintosh", type: "Apple_HFS" },
      { start: 64 + blocks, blocks: 1, name: "Free", type: "Apple_Free" }
    ].each_with_index do |entry, index|
      raw = "\0".b * 512
      raw[0, 2] = "PM".b
      raw[4, 12] = [3, entry.fetch(:start), entry.fetch(:blocks)].pack("N3")
      raw[16, 32] = entry.fetch(:name).b.ljust(32, "\0".b)
      raw[48, 32] = entry.fetch(:type).b.ljust(32, "\0".b)
      disk[(index + 1) * 512, 512] = raw
    end
    (@root / "apple.img").binwrite(disk)
    disk
  end
end
