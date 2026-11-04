# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_qnx4_truncated_info_reports_missing_bytes
    declared, missing = qnx4_truncated_fixture
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("qnx4", info.fetch("format"))
    assert_equal(declared.to_s, info.fetch("volume_bytes"))
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
    assert_includes(invoke(arguments: ["i", "-S", "off", "-f", "tail.img"]).stdout,
      "#{missing} bytes")
  end

  def test_qnx4_truncated_listing_and_extraction_preserve_bytes
    qnx4_truncated_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/.altboot", "/.boot", "/hello"], entries.map { _1.fetch("path") }.sort)
    assert_equal("hello\n".b,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/hello"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "tail.img", "-C", "restore"]))
    assert_equal("hello\n".b, (@root / "restore/tail/hello").binread)
    assert_equal("".b, (@root / "restore/tail/.boot").binread)
    assert_equal("".b, (@root / "restore/tail/.altboot").binread)
  end

  def test_qnx4_truncated_verification_names_missing_byte_count
    _declared, missing = qnx4_truncated_fixture
    [[], ["-d"]].each do |payload|
      error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "tail.img"], expected: 1))
      assert_equal(-5, error.fetch("code"))
      assert_includes(error.fetch("message"), "truncated")
      assert_includes(error.fetch("message"), missing.to_s)
    end
  end

  private

  def qnx4_truncated_fixture
    (@root / "small").mkdir
    (@root / "small/hello").binwrite("hello\n".b)
    invoke(arguments: ["c", "-R", "-S", "off", "-F", "qnx4", "-s", "128K",
      "-f", "tail.img", "-C", "small", "."])
    path = @root / "tail.img"
    blocks = File.size(path) / 512
    bitmap = File.binread(path, (blocks + 7) / 8, 1024)
    last_used = (1..blocks).reverse_each.find do |block|
      (bitmap.getbyte((block - 1) / 8) & (1 << ((block - 1) % 8))).positive?
    end
    refute_nil(last_used)
    assert_operator(blocks - last_used, :>=, 8)
    # Retain every allocated block and part of the following free block.
    cut = last_used * 512 + 100
    declared = bitmap.bytesize * 8 * 512
    File.truncate(path, cut)
    [declared, declared - cut]
  end
end
