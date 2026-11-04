# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_exfat_truncated_info_reports_missing_bytes
    missing = exfat_truncated_fixture
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("exfat", info.fetch("format"))
    assert_equal("1048576", info.fetch("volume_bytes"))
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
    human = invoke(arguments: ["i", "-S", "off", "-f", "tail.img"]).stdout
    assert_match(/^Missing:.*\(#{missing} bytes\)/, human)
  end

  def test_exfat_truncated_listing_and_extraction_preserve_bytes
    exfat_truncated_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/Data.bin"], entries.map { _1.fetch("path") })
    payload = (0...512).map { (_1 * 37) & 255 }.pack("C*") + "\x7b".b + "\0".b * 511
    assert_equal(payload,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/Data.bin"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img", "-C", "restore"]))
    assert_equal(payload, (@root / "restore/tail/Data.bin").binread)
  end

  def test_exfat_truncated_lost_initialized_byte_fails_extraction
    exfat_truncated_fixture
    # Root and first file fragment survive; the 513th initialized byte is
    # in cluster 262, beyond this slice. The remaining 511 bytes are zeros.
    File.truncate(@root / "tail.img", (40 + 261 - 2) * 512)
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/Data.bin"], entries.map { _1.fetch("path") })
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img",
      "-C", "restore"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_equal("incomplete", error.fetch("output_state"))
    result = invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/Data.bin"], expected: 1)
    assert_includes(result.stderr, "truncated")
  end

  def test_exfat_truncated_verification_refuses_missing_tail
    check_exfat_truncated_verification(payload: [])
  end

  def test_exfat_truncated_payload_verification_refuses_missing_tail
    check_exfat_truncated_verification(payload: ["-d"])
  end

  private

  def exfat_truncated_fixture
    invoke(executable: @executable.dirname / "test-exfat",
      arguments: ["--seed-truncated", (@root / "tail.img").to_s])
    1_048_576 - File.size(@root / "tail.img")
  end

  def check_exfat_truncated_verification(payload:)
    missing = exfat_truncated_fixture
    error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "tail.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_includes(error.fetch("message"), missing.to_s)
  end
end
