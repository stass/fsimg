# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  HFS_WRAPPER_VOLUME_BYTES = 512 * 4096
  HFS_WRAPPER_OFFSET = 4 * 512 + 4096
  HFS_WRAPPER_PAYLOAD_OFFSET = (512 - 32) * 4096
  HFS_WRAPPER_PREFIX_BYTES = 4096 + 137

  def test_hfs_wrapper_cut_info_reports_embedded_size_and_missing_bytes
    kept = HFS_WRAPPER_PAYLOAD_OFFSET + HFS_WRAPPER_PREFIX_BYTES
    hfs_wrapper_fixture(kept: kept)
    info = completed(invoke(arguments:
      ["i", "--json", "-S", "off", "-f", "wrapped.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal(HFS_WRAPPER_VOLUME_BYTES.to_s, info.fetch("volume_bytes"))
    missing = HFS_WRAPPER_VOLUME_BYTES - kept
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
    human = invoke(arguments:
      ["i", "-S", "off", "-f", "wrapped.img"]).stdout
    assert_match(/^Volume size:\s+2 MiB$/, human)
    assert_match(/^Missing:.*\(#{missing} bytes\)/, human)
  end

  def test_hfs_wrapper_cut_verification_reports_embedded_loss
    kept = HFS_WRAPPER_PAYLOAD_OFFSET + HFS_WRAPPER_PREFIX_BYTES
    hfs_wrapper_fixture(kept: kept)
    [[], ["-d"]].each do |payload|
      error = failed(invoke(arguments:
        ["W", *payload, "--json", "-S", "off", "-f", "wrapped.img"],
        expected: 1))
      assert_equal(-5, error.fetch("code"))
      assert_includes(error.fetch("message"), "truncated")
      missing = HFS_WRAPPER_VOLUME_BYTES - kept
      assert_includes(error.fetch("message"), missing.to_s)
    end
  end

  def test_hfs_wrapper_cut_extraction_reports_truncation
    hfs_wrapper_fixture(kept:
      HFS_WRAPPER_PAYLOAD_OFFSET + HFS_WRAPPER_PREFIX_BYTES)
    (@root / "restore").mkdir
    error = failed(invoke(arguments:
      ["x", "--json", "-S", "off", "-f", "wrapped.img", "-C", "restore"],
      expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_equal("incomplete", error.fetch("output_state"))
    result = invoke(arguments:
      ["x", "-O", "-S", "off", "-f", "wrapped.img", "/a"], expected: 1)
    assert_includes(result.stderr, "truncated")
  end

  def test_hfs_wrapper_cut_outer_tail_verifies_without_embedded_loss
    hfs_wrapper_fixture(kept: HFS_WRAPPER_VOLUME_BYTES + 512)
    [[], ["-d"]].each do |payload|
      completed(invoke(arguments:
        ["W", *payload, "--json", "-S", "off", "-f", "wrapped.img"]))
    end
    info = completed(invoke(arguments:
      ["i", "--json", "-S", "off", "-f", "wrapped.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("0", info.fetch("missing_bytes"))
  end

  private def hfs_wrapper_fixture(kept:)
    invoke(executable: @executable.dirname / "test-hfsplus",
      arguments: ["--seed-wrap", (@root / "wrapped.img").to_s])
    File.truncate(@root / "wrapped.img", HFS_WRAPPER_OFFSET + kept)
  end
end
