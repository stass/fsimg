# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_ifs_creation_preserves_byte_order_metadata_and_mountpoint
    root = @root
    payload = @payload
    # ImageFS creation through the command: the byte order and mountpoint
    # settings cross the worker job record into the image, which stores
    # files, directories and links with owner, mode and time, reads back,
    # verifies and is reproducible. A 1-byte cluster needs no sector size; a
    # size and a label are refused, and a boot setting is an unknown key,
    # since the writer makes nonbootable images.
    source = (root / "ifs-source")
    FileUtils.mkdir_p(source / "nested")
    (source / "nested/caf\u00e9.txt").binwrite(payload)
    (source / "nested/caf\u00e9.txt").chmod(0o640)
    File.symlink("nested/caf\u00e9.txt", (source / "link"))
    arguments = [
      "c",
      "--json",
      "-R",
      "-p",
      "-S",
      "off",
      "-F",
      "ifs",
      "-o",
      "endian=big",
      "-o",
      "mountpoint=/proc/boot"
    ]
    operands = ["-C", "ifs-source", "."]
    completed(invoke(arguments: [*arguments, "-f", "made.ifs", *operands]))
    image = (root / "made.ifs").binread
    assert_equal("imagefs\x05".b, image[0...8], "image 0 8")
    assert_equal("/proc/boot\x00".b, image[0x58...0x63], "image 0x58 0x63")
    info = completed(invoke(arguments: ["i", "--json", "-f", "made.ifs"]))[1]
    assert_equal("ifs", info.fetch("format"), "info format")
    assert_equal("big-endian", info.fetch("profile"), "info profile")
    assert_equal("/proc/boot", info.fetch("mountpoint"), "info mountpoint")
    assert_nil(info.fetch("label"), "info label nil")
    listed = completed(invoke(arguments: ["t", "--json", "-f", "made.ifs"]))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal(["/link", "/nested", "/nested/caf\u00e9.txt"].to_set, entries.keys.to_set)
    assert_equal("symlink", entries.fetch("/link").fetch("kind"))
    assert_equal(0o640, entries.fetch("/nested/caf\u00e9.txt").fetch("mode"))
    assert_equal(
      payload,
      invoke(
        arguments: [
          "x",
          "-O",
          "-f",
          "made.ifs",
          "/nested/caf\u00e9.txt"
        ]
      )
        .stdout
    )
    verification = completed(invoke(arguments: ["W", "--json", "-d", "-f", "made.ifs"]))
    report = verification.find { |record| record.fetch("type") == "verify" }
    assert_equal("initialized-payload", report.fetch("validation_scope"))
    completed(invoke(arguments: [*arguments, "-o", "cluster=1", "-f", "second.ifs", *operands]))
    same_bytes(first: (root / "made.ifs"), second: (root / "second.ifs"))
    [
      [["-s", "1M"], "an ImageFS image has no free space".b],
      [["-L", "LABEL"], "the image format has no volume label".b]
    ].each do |extra, detail|
      result = invoke(
        arguments: [
          "c",
          "-S",
          "off",
          "-F",
          "ifs",
          *extra,
          "-f",
          "refused.ifs",
          *operands
        ],
        expected: 1
      )
      assert_includes(result.stderr, detail, [extra, result.stderr])
      refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    end

    ["boot1", "script"].each do |key|
      result = invoke(
        arguments: [
          "c",
          "-S",
          "off",
          "-F",
          "ifs",
          "-o",
          (key + "=/nested/caf\u00e9.txt"),
          "-f",
          "refused.ifs",
          *operands
        ],
        expected: 2
      )
      assert_includes(result.stderr, "unknown setting key".b, [key, result.stderr])
      refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    end
    # gzip encodes the image byte for byte, so it verifies; a scheme or a
    # container that pads the volume is refused before any output, even
    # with a partition type.
    completed(invoke(arguments: [*arguments, "-a", "-f", "made.ifs.gz", *operands]))
    assert_equal(image, gunzip((root / "made.ifs.gz").binread))

    verification = completed(invoke(arguments: ["W", "--json", "-d", "-f", "made.ifs.gz"]))
    report = verification.find { |record| record.fetch("type") == "verify" }
    assert_equal("initialized-payload", report.fetch("validation_scope"))
    [
      ["ifs/gpt", ["-o", "gpt:type=linux"]],
      ["ifs/mbr", ["-o", "mbr:type=0x83"]],
      ["ifs/vhd", []],
      ["ifs/dmg", []],
      ["ifs/simg", []]
    ].each do |stack, extra|
      result = invoke(
        arguments: [
          "c",
          "-R",
          "-S",
          "off",
          "-F",
          stack,
          *extra,
          "-f",
          "refused.img",
          *operands
        ],
        expected: 1
      )
      assert_includes(result.stderr,
        "the filesystem cannot sit in a scheme or a padding container \"#{stack}\"".b,
        stack)
      refute((root / "refused.img").exist?, "refused creation leaves no output image")
    end
  end
end
