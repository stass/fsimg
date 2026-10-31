# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_qnx4_preserves_labels_links_and_posix_metadata
    root = @root
    payload = @payload
    # QNX4 creation through the command: a 48-byte label and an inodes=
    # setting cross the worker job record into the image, which stores the
    # POSIX fields and a symbolic link, reads back, verifies and is
    # reproducible.
    source = (root / "qnx4-source")
    FileUtils.mkdir_p(source / "nested")
    (source / "nested/caf\u00e9.txt").binwrite(payload)
    (source / "nested/caf\u00e9.txt").chmod(0o640)
    File.symlink("nested/caf\u00e9.txt", (source / "link"))
    label = (("L" * 47) + "Q")
    arguments = [
      "c",
      "--json",
      "-R",
      "-p",
      "-S",
      "off",
      "-F",
      "qnx4",
      "-L",
      label,
      "-o",
      "inodes=200"
    ]
    operands = ["-C", "qnx4-source", "."]
    completed(invoke(arguments: [*arguments, "-f", "qnx4.img", *operands]))
    image = (root / "qnx4.img").binread
    # The root block, block 2 at byte 512, holds .inodes in record 1, 200
    # slots of 64 bytes, and the label in 48 bytes at 0x1c0, unterminated.
    assert_equal((200 * 64), image[(512 + 0x50)...(512 + 0x54)].unpack1("L<"))
    assert_equal(label.encode(Encoding::UTF_8).b, image[(512 + 0x1c0)...(512 + 0x1f0)])
    info = completed(invoke(arguments: ["i", "--json", "-f", "qnx4.img"]))[1]
    assert_equal("qnx4", info.fetch("format"), "info format")
    assert_equal(label, info.fetch("label"), "info label")
    listed = completed(invoke(arguments: ["t", "--json", "-f", "qnx4.img"]))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal(
      [
        "/.boot",
        "/.altboot",
        "/link",
        "/nested",
        "/nested/caf\u00e9.txt"
      ].to_set,
      entries.keys.to_set
    )
    assert_equal("symlink", entries.fetch("/link").fetch("kind"))
    assert_equal(0o640, entries.fetch("/nested/caf\u00e9.txt").fetch("mode"))
    # The long listing shows the stored permission bits and link length.
    columns = long_listing(invoke(arguments: ["tvf", "qnx4.img"]))
    assert_equal(["-", "rw-r-----", payload.size.to_s], columns.fetch("\"/nested/caf\u00e9.txt\"")[0...3])
    target_bytes = "nested/caf\u00e9.txt".encode(Encoding::UTF_8).b.size
    assert_equal("l", columns.fetch("\"/link\"")[0], "columns fetch link 0")
    assert_equal(target_bytes.to_s, columns.fetch("\"/link\"")[2], "columns fetch link 2")
    assert_equal("d", columns.fetch("\"/nested\"")[0], "columns fetch nested 0")
    assert_equal("-", columns.fetch("\"/nested\"")[2], "columns fetch nested 2")
    assert_equal(
      payload,
      invoke(
        arguments: [
          "x",
          "-O",
          "-f",
          "qnx4.img",
          "/nested/caf\u00e9.txt"
        ]
      )
        .stdout
    )
    verification = completed(invoke(arguments: ["W", "--json", "-d", "-f", "qnx4.img"]))
    report = verification.find { |record| record.fetch("type") == "verify" }
    assert_equal("initialized-payload", report.fetch("validation_scope"))
    completed(invoke(arguments: [*arguments, "-f", "qnx4-second.img", *operands]))
    same_bytes(first: (root / "qnx4.img"), second: (root / "qnx4-second.img"))
  end
end
