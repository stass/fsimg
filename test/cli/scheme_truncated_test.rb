# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  SCHEME_DISK_BYTES = 4 * 1024 * 1024
  SCHEME_PARTITION_OFFSET = 1024 * 1024
  SCHEME_PARTITION_END = 2 * 1024 * 1024
  SCHEME_CUT_BYTES = SCHEME_PARTITION_END - 123

  def test_mbr_mid_sector_cut_reports_table_partition_and_volume_loss
    scheme_fixture(scheme: "mbr", cut: SCHEME_CUT_BYTES)
    assert_scheme_counts(scheme: "mbr", cut: SCHEME_CUT_BYTES,
      disk_missing: 123, partition_missing: 123)
  end

  def test_gpt_mid_sector_cut_reports_table_partition_and_volume_loss
    scheme_fixture(scheme: "gpt", cut: SCHEME_CUT_BYTES)
    assert_scheme_counts(scheme: "gpt", cut: SCHEME_CUT_BYTES,
      disk_missing: SCHEME_DISK_BYTES - SCHEME_CUT_BYTES, partition_missing: 123)
  end

  def test_gpt_backup_loss_leaves_partition_whole
    cut = SCHEME_DISK_BYTES - 512
    scheme_fixture(scheme: "gpt", cut: cut)
    assert_scheme_counts(scheme: "gpt", cut: cut, disk_missing: 512, partition_missing: 0)
  end

  def test_mbr_missing_partition_is_listed_and_automatic_selection_reads_first
    scheme_fixture(scheme: "mbr", cut: SCHEME_PARTITION_END, missing_partition: true)
    assert_missing_partition(scheme: "mbr")
  end

  def test_gpt_missing_partition_is_listed_and_automatic_selection_reads_first
    scheme_fixture(scheme: "gpt", cut: SCHEME_PARTITION_END, missing_partition: true)
    assert_missing_partition(scheme: "gpt")
  end

  def test_mbr_truncated_last_partition_lists_and_extracts_surviving_bytes
    scheme_fixture(scheme: "mbr", cut: SCHEME_CUT_BYTES)
    assert_scheme_files
  end

  def test_gpt_truncated_last_partition_lists_and_extracts_surviving_bytes
    scheme_fixture(scheme: "gpt", cut: SCHEME_CUT_BYTES)
    assert_scheme_files
  end

  def test_mbr_text_places_missing_after_disk_size_and_on_partition
    scheme_fixture(scheme: "mbr", cut: SCHEME_CUT_BYTES)
    assert_scheme_text(missing: "123 bytes")
  end

  def test_gpt_text_places_missing_after_disk_size_and_names_backup_loss
    scheme_fixture(scheme: "gpt", cut: SCHEME_CUT_BYTES)
    assert_scheme_text(missing: "2.0 MiB (2097275 bytes), backup table lost")
  end

  def test_mbr_partial_partition_verification_reports_truncation
    scheme_fixture(scheme: "mbr", cut: SCHEME_CUT_BYTES)
    assert_scheme_verification_corrupt
  end

  def test_mbr_missing_partition_verification_reports_truncation
    scheme_fixture(scheme: "mbr", cut: SCHEME_PARTITION_END, missing_partition: true)
    assert_scheme_verification_corrupt
  end

  def test_gpt_partial_partition_verification_reports_truncation
    scheme_fixture(scheme: "gpt", cut: SCHEME_CUT_BYTES)
    assert_scheme_verification_corrupt
  end

  def test_gpt_missing_partition_verification_reports_truncation
    scheme_fixture(scheme: "gpt", cut: SCHEME_PARTITION_END, missing_partition: true)
    assert_scheme_verification_corrupt
  end

  def test_gpt_backup_loss_verification_reports_truncation
    scheme_fixture(scheme: "gpt", cut: SCHEME_DISK_BYTES - 512)
    assert_scheme_verification_corrupt
  end

  def test_gpt_4096_sector_cut_reports_bytes_and_refuses_verification
    scheme_fixture(scheme: "gpt", cut: SCHEME_CUT_BYTES, sector_bytes: 4096)
    assert_scheme_counts(scheme: "gpt", cut: SCHEME_CUT_BYTES,
      disk_missing: SCHEME_DISK_BYTES - SCHEME_CUT_BYTES, partition_missing: 123)
    stack = scheme_records.find { _1.fetch("type") == "stack" }
    assert_equal(4096, stack.fetch("sector_bytes"))
    assert_scheme_verification_corrupt
  end

  def test_whole_schemes_verify_and_report_zero_missing_bytes
    ["mbr", "gpt"].each do |scheme|
      scheme_fixture(scheme: scheme, cut: SCHEME_DISK_BYTES)
      stack = scheme_records.find { _1.fetch("type") == "stack" }
      assert_equal("0", stack.fetch("missing_bytes"))
      if scheme == "gpt"
        assert_equal(false, stack.fetch("backup_missing"))
      else
        assert_nil(stack.fetch("backup_missing"))
      end
      invoke(arguments: ["W", "-S", "off", "-f", "scheme.img"])
    end
  end

  def test_bare_volume_has_no_backup_table_state
    scheme_volume
    stack = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "volume.img"]))
      .find { _1.fetch("type") == "stack" }
    assert_equal("0", stack.fetch("missing_bytes"))
    assert_nil(stack.fetch("backup_missing"))
  end

  def test_truncated_simg_container_is_corrupt
    disk = scheme_fixture(scheme: "gpt", cut: SCHEME_DISK_BYTES)
    (@root / "cut.simg").binwrite(simg_image(disk: disk)[0...-1])
    error = failed(invoke(arguments: ["W", "--json", "-S", "off", "-F", "fat12/gpt/simg",
      "-f", "cut.simg"], expected: 1))
    assert_equal(-5, error.fetch("code"))
  end

  def test_verification_without_surviving_filesystems_reports_disk_loss
    { "mbr" => 512, "gpt" => 17_408 }.each do |scheme, cut|
      scheme_fixture(scheme: scheme, cut: cut)
      missing = (scheme == "gpt" ? SCHEME_DISK_BYTES : SCHEME_PARTITION_END) - cut
      error = failed(invoke(arguments: ["W", "--json", "-S", "off", "-f", "scheme.img"], expected: 1))
      assert_equal(-5, error.fetch("code"))
      assert_includes(error.fetch("message"), "truncated")
      assert_includes(error.fetch("message"), missing.to_s)
      refute_includes(error.fetch("message"), "no recognized filesystem")
    end
  end

  def test_verification_of_whole_range_ignores_truncated_table
    ["mbr", "gpt"].each do |scheme|
      scheme_fixture(scheme: scheme, cut: SCHEME_PARTITION_END, missing_partition: true)
      result = invoke(arguments: ["W", "-S", "off", "-f", "scheme.img", "--offset=1M", "--length=1M"])
      assert_includes(result.stdout, "filesystem structure verified")
      refute_includes(result.stdout, "partition table")
    end
  end

  def test_verification_with_past_end_partition_reports_disk_loss
    ["mbr", "gpt"].each do |scheme|
      scheme_fixture(scheme: scheme, cut: SCHEME_PARTITION_END, missing_partition: true)
      missing = scheme == "gpt" ? SCHEME_PARTITION_END : 1_572_864
      error = failed(invoke(arguments: ["W", "--json", "-S", "off", "-f", "scheme.img",
        "--partition=2"], expected: 1))
      assert_equal(-5, error.fetch("code"))
      assert_includes(error.fetch("message"), "truncated")
      assert_includes(error.fetch("message"), missing.to_s)
    end
  end

  def test_info_lists_every_partition_when_all_are_at_or_past_end
    { "mbr" => [512, SCHEME_PARTITION_OFFSET],
      "gpt" => [17_408, SCHEME_PARTITION_OFFSET] }.each do |scheme, cuts|
      cuts.each do |cut|
        scheme_fixture(scheme: scheme, cut: cut, missing_partition: true)
        partitions = scheme_records.select { _1.fetch("type") == "partition" }
        assert_equal([1, 2], partitions.map { _1.fetch("index") })
        assert_equal(["1048576", "3145728"], partitions.map { _1.fetch("offset_bytes") })
        assert_equal(["0", "0"], partitions.map { _1.fetch("length_bytes") })
        assert_equal(["1048576", "524288"], partitions.map { _1.fetch("missing_bytes") })
        assert_equal(["none", "none"], partitions.map { _1.fetch("filesystem_claim") })
      end
    end
  end

  private

  def scheme_volume
    return (@root / "volume.img").binread if (@root / "volume.img").exist?

    invoke(arguments: ["c", "-R", "-S", "off", "-F", "fat12", "-s", "1M",
      "-f", "volume.img", "-C", "source", "HELLO.TXT"])
    (@root / "volume.img").binread
  end

  def scheme_fixture(scheme:, cut:, missing_partition: false, sector_bytes: 512)
    volume = scheme_volume
    partitions = if scheme == "gpt"
      [GptPartition.new(first_lba: SCHEME_PARTITION_OFFSET / sector_bytes,
        last_lba: SCHEME_PARTITION_END / sector_bytes - 1, type: ESP_TYPE,
        name: "EFI", payload: volume)]
    else
      [MbrPartition.new(first_lba: 2048, sector_count: 2048, type: 1, payload: volume)]
    end
    if missing_partition
      partitions += if scheme == "gpt"
        [GptPartition.new(first_lba: 6144, last_lba: 7167, type: LINUX_TYPE,
          name: "lost", payload: "".b)]
      else
        [MbrPartition.new(first_lba: 6144, sector_count: 1024, type: 0x83, payload: "".b)]
      end
    end
    disk = if scheme == "gpt"
      gpt_disk(sectors: SCHEME_DISK_BYTES / sector_bytes, partitions: partitions, sector_bytes: sector_bytes)
    else
      mbr_disk(sectors: 8192, partitions: partitions)
    end
    (@root / "scheme.img").binwrite(disk.byteslice(0, cut))
    disk
  end

  def scheme_records = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "scheme.img"]))

  def assert_scheme_counts(scheme:, cut:, disk_missing:, partition_missing:)
    records = scheme_records
    stack = records.find { _1.fetch("type") == "stack" }
    partition = records.find { _1.fetch("type") == "partition" }
    volume = records.find { _1.fetch("type") == "info" }
    assert_equal(disk_missing.to_s, stack.fetch("missing_bytes"))
    if scheme == "gpt"
      assert_equal(true, stack.fetch("backup_missing"))
    else
      assert_nil(stack.fetch("backup_missing"))
    end
    assert_equal([cut - SCHEME_PARTITION_OFFSET, SCHEME_PARTITION_OFFSET].min.to_s,
      partition.fetch("length_bytes"))
    assert_equal(partition_missing.to_s, partition.fetch("missing_bytes"))
    assert_equal(partition_missing.to_s, volume.fetch("missing_bytes"))
  end

  def assert_missing_partition(scheme:)
    records = scheme_records
    partition = records.select { _1.fetch("type") == "partition" }.fetch(1)
    assert_equal("3145728", partition.fetch("offset_bytes"))
    assert_equal("0", partition.fetch("length_bytes"))
    assert_equal("524288", partition.fetch("missing_bytes"))
    assert_equal("none", partition.fetch("filesystem_claim"))
    stack = records.find { _1.fetch("type") == "stack" }
    expected_missing = scheme == "gpt" ? SCHEME_PARTITION_END : 1572864
    assert_equal(expected_missing.to_s, stack.fetch("missing_bytes"))
    error = failed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "scheme.img",
      "--partition=2"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_scheme_files
  end

  def assert_scheme_files
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "scheme.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/HELLO.TXT"], entries.map { _1.fetch("path") })
    assert_equal("hello\n".b,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "scheme.img", "/HELLO.TXT"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "scheme.img", "-C", "restore"]))
    assert_equal("hello\n".b, (@root / "restore/scheme/HELLO.TXT").binread)
  end

  def assert_scheme_text(missing:)
    lines = invoke(arguments: ["i", "-S", "off", "-f", "scheme.img"]).stdout.lines
    disk_index = lines.index { _1.start_with?("Disk size:") }
    refute_nil(disk_index)
    assert_equal("Missing:      #{missing}\n".b, lines.fetch(disk_index + 1))
    partition = lines.find { _1.start_with?("Partition 1:") }
    assert_includes(partition, ", missing 123 bytes")
  end

  def assert_scheme_verification_corrupt
    result = invoke(arguments: ["W", "-S", "off", "-f", "scheme.img"], expected: 1)
    assert_includes(result.stderr, "truncated")
    error = failed(invoke(arguments: ["W", "--json", "-S", "off", "-f", "scheme.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
  end
end
