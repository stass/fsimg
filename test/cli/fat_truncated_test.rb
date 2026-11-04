# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  FAT_TRUNCATED_VOLUME_BYTES = 136_314_880
  SLICE_HOLDING_A_AND_B = 300_000
  SLICE_LOSING_A = 100_000

  def test_fat_truncated_info_reports_exact_missing_bytes_in_json
    missing = fat_truncated_fixture
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("fat16", info.fetch("format"))
    assert_equal(FAT_TRUNCATED_VOLUME_BYTES.to_s, info.fetch("volume_bytes"))
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
  end

  def test_fat_truncated_text_places_missing_size_after_volume_size
    missing = fat_truncated_fixture
    lines = invoke(arguments: ["i", "-S", "off", "-f", "tail.img"]).stdout.lines
    volume_index = lines.index { _1.start_with?("Volume size:") }
    refute_nil(volume_index)
    assert_equal("Missing:      129.8 MiB (#{missing} bytes)\n".b, lines.fetch(volume_index + 1))
  end

  def test_fat_truncated_listing_and_extraction_preserve_names_and_bytes
    fat_truncated_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/HELLO.TXT", "/SUB", "/SUB/EMPTY"], entries.map { _1.fetch("path") })
    assert_equal(fat_truncated_payload,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/HELLO.TXT"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img", "-C", "restore"]))
    assert_equal(fat_truncated_payload, (@root / "restore/tail/HELLO.TXT").binread)
    assert_equal("".b, (@root / "restore/tail/SUB/EMPTY").binread)
  end

  def test_fat_truncated_verification_refuses_missing_tail
    check_fat_truncated_verification(payload: [])
  end

  def test_fat_truncated_payload_verification_refuses_missing_tail
    check_fat_truncated_verification(payload: ["-d"])
  end

  def test_fat_missing_file_lists_but_partial_extraction_fails
    fat_truncated_fixture
    path = @root / "tail.img"
    boot = File.binread(path, 512)
    reserved = boot[14, 2].unpack1("v")
    fat_sectors = boot[22, 2].unpack1("v")
    root_sector = reserved + boot.getbyte(16) * fat_sectors
    data_sector = root_sector + boot[17, 2].unpack1("v") * 32 / 512
    # Keep only HELLO.TXT in the fixed root; cut inside its first cluster.
    File.open(path, "r+b") do |file|
      file.pwrite("\0".b * 32, root_sector * 512 + 32)
      file.truncate(data_sector * 512 + boot.getbyte(13) * 512 + 500)
    end
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/HELLO.TXT"], entries.map { _1.fetch("path") })
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img",
      "-C", "restore"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_equal("incomplete", error.fetch("output_state"))
  end

  def test_fat_whole_and_longer_slices_report_zero_without_a_missing_line
    fat_truncated_fixture
    [FAT_TRUNCATED_VOLUME_BYTES, FAT_TRUNCATED_VOLUME_BYTES + 123].each do |length|
      File.truncate(@root / "tail.img", length)
      info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
        .find { _1.fetch("type") == "info" }
      assert_equal("0", info.fetch("missing_bytes"))
      refute_match(/^Missing:/, invoke(arguments: ["i", "-S", "off", "-f", "tail.img"]).stdout)
    end
  end

  def test_mtools_reads_truncated_fat16_names_and_bytes
    fat_truncated_fixture
    tools = File.readlines(@executable.dirname / "Makefile", chomp: true)
      .filter_map { /\A(MDIR|MCOPY) = (.+)\z/.match(_1)&.captures }.to_h
    skip("independent truncated FAT check requires configured mtools mdir and mcopy") unless
      %w[MDIR MCOPY].all? { tools.key?(_1) && File.executable?(tools.fetch(_1)) }

    listing = invoke(executable: "/usr/bin/env", arguments: ["MTOOLS_SKIP_CHECK=1",
      tools.fetch("MDIR"), "-i", "tail.img", "-b", "-s", "::"], expected: nil)
    copied = invoke(executable: "/usr/bin/env", arguments: ["MTOOLS_SKIP_CHECK=1",
      tools.fetch("MCOPY"), "-i", "tail.img", "::HELLO.TXT", "oracle.txt"], expected: nil)
    if listing.status != 0 || copied.status != 0
      # Some mtools builds require the declared device length even when the
      # requested directory and file are wholly present.
      [listing, copied].reject { _1.status.zero? }.each do |result|
        assert_match(/(size|sector|read|device|FAT|initialize)/i, result.stderr)
      end
      skip("mtools refuses the truncated device even with MTOOLS_SKIP_CHECK=1")
    end
    assert_equal(["::/HELLO.TXT", "::/SUB/", "::/SUB/EMPTY"], listing.stdout.lines(chomp: true).sort)
    assert_equal(fat_truncated_payload, (@root / "oracle.txt").binread)
    extracted = invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/HELLO.TXT"])
    assert_equal((@root / "oracle.txt").binread, extracted.stdout)
    invoke(executable: "/usr/bin/env", arguments: ["MTOOLS_SKIP_CHECK=1",
      tools.fetch("MCOPY"), "-i", "tail.img", "::SUB/EMPTY", "empty.txt"])
    empty = invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/SUB/EMPTY"])
    assert_equal((@root / "empty.txt").binread, empty.stdout)
  end

  def test_fat_truncated_directory_budget_reports_corruption
    fat_output_budget_fixture(slice_bytes: SLICE_HOLDING_A_AND_B)
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "budget.img",
      "-C", "restore"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
  end

  def test_fat_truncated_stdout_budget_reports_corruption
    fat_output_budget_fixture(slice_bytes: SLICE_LOSING_A)
    result = invoke(arguments: ["x", "-O", "-S", "off", "-f", "budget.img", "/A.BIN"], expected: 1)
    assert_includes(result.stderr, "truncated")
  end

  def test_fat_missing_file_with_explicit_small_output_budget_reports_corruption
    fat_output_budget_fixture(slice_bytes: SLICE_LOSING_A)
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "budget.img",
      "-l", "output=1000", "-C", "restore"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
  end

  def test_fat_truncated_present_file_with_explicit_small_output_budget_remains_a_limit
    fat_output_budget_fixture(slice_bytes: SLICE_HOLDING_A_AND_B)
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "budget.img",
      "-l", "output=1000", "-C", "restore"], expected: 1))
    assert_equal(-8, error.fetch("code"))
    assert_equal("configured resource limit exceeded", error.fetch("message"))
  end

  private

  def check_fat_truncated_verification(payload:)
    missing = fat_truncated_fixture
    error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "tail.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_includes(error.fetch("message"), missing.to_s)
  end

  def fat_output_budget_fixture(slice_bytes:)
    path = @root / "budget.img"
    invoke(executable: @executable.dirname / "test-fat",
      arguments: ["--seed-output-budget", path.to_s])
    File.truncate(path, slice_bytes)
  end

  def fat_truncated_fixture
    invoke(executable: @executable.dirname / "test-fat",
      arguments: ["--seed-truncated", (@root / "tail.img").to_s])
    FAT_TRUNCATED_VOLUME_BYTES - File.size(@root / "tail.img")
  end

  def fat_truncated_payload = (0...1100).map { (_1 * 37) & 255 }.pack("C*")
end
