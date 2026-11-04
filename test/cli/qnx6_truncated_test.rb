# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_qnx6_truncated_info_reports_declaration_missing_bytes_and_detail
    missing = qnx6_truncated_fixture
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "cut.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("qnx6", info.fetch("format"))
    assert_equal(QNX6_TRUNCATED_VOLUME_BYTES.to_s, info.fetch("volume_bytes"))
    assert_equal(missing.to_s, info.fetch("missing_bytes"))
    assert_equal({"second_header_missing" => "1"}, info.fetch("details"))
  end

  def test_qnx6_truncated_text_info_names_the_missing_byte_count
    missing = qnx6_truncated_fixture
    lines = invoke(arguments: ["i", "-S", "off", "-f", "cut.img"]).stdout.lines
    volume_index = lines.index { _1.start_with?("Volume size:") }
    refute_nil(volume_index)
    assert_equal(968 * 1024, missing)
    assert_equal("Missing:      968 KiB\n".b, lines.fetch(volume_index + 1))
    assert_includes(lines, "second_header_missing: 1\n".b)
  end

  def test_qnx6_truncated_listing_and_extraction_preserve_names_and_bytes
    qnx6_truncated_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "cut.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/.boot", "/payload"], entries.map { _1.fetch("path") })
    assert_equal(qnx6_truncated_payload,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", "cut.img", "/payload"]).stdout)
    (@root / "restore").mkdir
    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "cut.img", "-C", "restore"]))
    assert_equal(qnx6_truncated_payload, (@root / "restore/cut/payload").binread)
  end

  def test_qnx6_truncated_verification_names_the_exact_missing_byte_count
    missing = qnx6_truncated_fixture
    [[], ["-d"]].each do |payload|
      error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "cut.img"], expected: 1))
      assert_equal(-5, error.fetch("code"))
      assert_equal("image is truncated: #{missing} declared bytes are missing", error.fetch("message"))
    end
  end

  def test_qnx6_cut_inside_a_file_cannot_extract_successfully
    qnx6_truncated_fixture
    image = (@root / "cut.img").binread
    # The small fixture's inode tree has direct leaves. Locate the payload
    # through the root directory rather than assuming formatter inode order.
    inode_leaf = image[8192 + 80, 4].unpack1("V")
    root = image[12_288 + inode_leaf * 4096, 128]
    root_block = root[36, 4].unpack1("V")
    slots = image[12_288 + root_block * 4096, root[0, 8].unpack1("Q<")]
    slot = slots.bytes.each_slice(32).map { _1.pack("C*") }
      .find { _1.getbyte(4) == 7 && _1[5, 7] == "payload" }
    refute_nil(slot)
    inode = slot[0, 4].unpack1("V")
    leaf_index, within_leaf = ((inode - 1) * 128).divmod(4096)
    leaf = image[8192 + 80 + leaf_index * 4, 4].unpack1("V")
    record = image[12_288 + leaf * 4096 + within_leaf, 128]
    last_block = record[36 + 2 * 4, 4].unpack1("V")
    # Keep two complete payload blocks; lose part of its third block.
    File.truncate(@root / "cut.img", 12_288 + last_block * 4096 + 100)
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "cut.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/.boot", "/payload"], entries.map { _1.fetch("path") })
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "cut.img",
      "-C", "restore"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    # Extraction may retain a private partial tree (design-cli.md); it must
    # report incomplete output, and no complete payload may remain there.
    assert_equal("incomplete", error.fetch("output_state"))
    (@root / "restore").glob("**/*", File::FNM_DOTMATCH).select(&:file?).each do |file|
      refute_equal(qnx6_truncated_payload, file.binread)
    end
  end

  private


  QNX6_TRUNCATED_VOLUME_BYTES = 1_048_576

  def qnx6_truncated_payload = (0...12_288).map { (_1 * 37) & 255 }.pack("C*")

  def qnx6_truncated_fixture
    (@root / "qnx-source").mkdir
    (@root / "qnx-source/payload").binwrite(qnx6_truncated_payload)
    completed(invoke(arguments: ["c", "--json", "-R", "-S", "off", "-F", "qnx6",
      "-s", "1M", "-o", "cluster=4K", "-o", "inodes=64", "-o", "reserve=0",
      "-o", "endian=little", "-f", "cut.img", "-C", "qnx-source", "."]))
    image = (@root / "cut.img").binread
    assert_equal(QNX6_TRUNCATED_VOLUME_BYTES, image.bytesize)
    blocks = image[8192 + 60, 4].unpack1("V")
    # This fixture's bitmap fits one direct leaf. Decode its allocation bits
    # to retain exactly the allocated prefix and discard only free tail.
    assert_equal(0, image.getbyte(8192 + 152 + 72))
    bitmap_block = image[8192 + 152 + 8, 4].unpack1("V")
    bitmap = image[12_288 + bitmap_block * 4096, (blocks + 7) / 8]
    last = (0...blocks).select { ((bitmap.getbyte(_1 / 8) >> (_1 % 8)) & 1) == 1 }.max
    refute_nil(last)
    length = 12_288 + (last + 1) * 4096
    assert_operator(length, :<, image.bytesize - 4096)
    File.truncate(@root / "cut.img", length)
    QNX6_TRUNCATED_VOLUME_BYTES - length
  end
end
