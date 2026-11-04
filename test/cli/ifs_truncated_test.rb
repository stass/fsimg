# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_ifs_truncated_info_reports_missing_bytes
    declared, missing = ifs_truncated_fixture
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("ifs", info.fetch("format"))
    assert_equal(declared.to_s, info.fetch("volume_bytes"))
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
    human = invoke(arguments: ["i", "-S", "off", "-f", "tail.img"]).stdout
    assert_match(/^Missing:      #{missing} bytes$/, human)
  end

  def test_ifs_truncated_listing_and_extraction_preserve_bytes
    ifs_truncated_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/bin", "/bin/sh", "/etc", "/etc/motd"], entries.map { _1.fetch("path") })
    assert_equal(@payload,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/etc/motd"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img", "-C", "restore", "/etc/motd"]))
    assert_equal(@payload, (@root / "restore/tail/etc/motd").binread)
  end

  def test_ifs_truncated_lost_file_lists_but_extraction_fails
    ifs_truncated_fixture
    File.truncate(@root / "tail.img", File.size(@root / "tail.img") - 1)
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_includes(entries.map { _1.fetch("path") }, "/etc/motd")
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img",
      "-C", "restore", "/etc/motd"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_equal("incomplete", error.fetch("output_state"))
  end

  def test_ifs_truncated_verification_refuses_missing_tail
    check_ifs_truncated_verification(payload: [])
  end

  def test_ifs_truncated_payload_verification_refuses_missing_tail
    check_ifs_truncated_verification(payload: ["-d"])
  end

  private

  def ifs_truncated_fixture
    image = ifs_image(payload: @payload)
    data_offset = image[12, 4].unpack1("L>")
    slice = image.byteslice(0, data_offset + @payload.bytesize)
    (@root / "tail.img").binwrite(slice)
    [image.bytesize, image.bytesize - slice.bytesize]
  end

  def check_ifs_truncated_verification(payload:)
    _, missing = ifs_truncated_fixture
    error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "tail.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_includes(error.fetch("message"), missing.to_s)
  end
end
