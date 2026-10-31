# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_qnx6_preserves_posix_metadata_and_typed_settings
    root = @root
    payload = @payload
    # QNX6 creation with its typed settings: a big-endian volume with a
    # chosen UUID, inode capacity and reserve stores the POSIX fields and a
    # symbolic link, reads back and verifies, and is reproducible.
    source = (root / "qnx6-source")
    FileUtils.mkdir_p(source / "nested")
    (source / "nested/caf\u00e9.txt").binwrite(payload)
    (source / "nested/caf\u00e9.txt").chmod(0o640)
    File.symlink("nested/caf\u00e9.txt", (source / "link"))
    uuid = "89abcdef-0123-4567-89ab-cdef01234567"
    arguments = [
      "c",
      "--json",
      "-R",
      "-p",
      "-S",
      "off",
      "-F",
      "qnx6",
      "-o",
      "cluster=2K",
      "-o",
      "endian=big",
      "-o",
      ("uuid=" + uuid.upcase),
      "-o",
      "inodes=100",
      "-o",
      "reserve=0"
    ]
    operands = ["-C", "qnx6-source", "."]
    decoded = completed(invoke(arguments: [*arguments, "-f", "qnx6.img", *operands]))
    plan = decoded.find { |record| record.fetch("type") == "plan" }
    assert_equal("qnx6", plan.fetch("format"), "plan format")
    assert_equal(2048, plan.fetch("cluster_bytes"), "plan cluster bytes")
    image = (root / "qnx6.img").binread
    assert_equal(["68191122"].pack("H*"), image[8192...8196])
    assert_equal([uuid.gsub("-", "")].pack("H*"), image[(8192 + 32)...(8192 + 48)])
    assert_equal(112, image[(8192 + 52)...(8192 + 56)].unpack1("L>"))
    info = completed(invoke(arguments: ["i", "--json", "-f", "qnx6.img"]))[1]
    assert_equal("qnx6", info.fetch("format"), "info format")
    assert_equal(uuid, info.fetch("uuid"), "info uuid")
    listed = completed(invoke(arguments: ["t", "--json", "-f", "qnx6.img"]))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal(
      [
        "/.boot",
        "/link",
        "/nested",
        "/nested/caf\u00e9.txt"
      ].to_set,
      entries.keys.to_set
    )
    assert_equal("symlink", entries.fetch("/link").fetch("kind"))
    assert_equal(0o640, entries.fetch("/nested/caf\u00e9.txt").fetch("mode"))
    assert_equal(
      payload,
      invoke(
        arguments: [
          "x",
          "-O",
          "-f",
          "qnx6.img",
          "/nested/caf\u00e9.txt"
        ]
      )
        .stdout
    )
    assert_equal(
      "nested/caf\u00e9.txt".encode(Encoding::UTF_8).b,
      invoke(
        arguments: [
          "x",
          "-O",
          "-f",
          "qnx6.img",
          "/link"
        ]
      )
        .stdout
    )
    verification = completed(invoke(arguments: ["W", "--json", "-d", "-f", "qnx6.img"]))
    report = verification.find { |record| record.fetch("type") == "verify" }
    assert_equal("initialized-payload", report.fetch("validation_scope"))
    completed(invoke(arguments: [*arguments, "-m", "16M", "-f", "qnx6-second.img", *operands]))
    same_bytes(first: (root / "qnx6.img"), second: (root / "qnx6-second.img"))
    # Without -R and uuid= the volume gets a random version 4 UUID.
    completed(
      invoke(
        arguments: [
          "c",
          "--json",
          "-S",
          "off",
          "-F",
          "qnx6",
          "-f",
          "qnx6-random.img",
          *operands
        ]
      )
    )
    random_uuid = completed(invoke(arguments: ["i", "--json", "-f", "qnx6-random.img"]))[1].fetch("uuid")
    assert_equal("4", random_uuid[14], "random uuid 14")
    assert_includes("89ab", random_uuid[19], "89ab")
    error = failed(
      invoke(
        arguments: [
          "c",
          "-n",
          "--json",
          "-F",
          "qnx6",
          "-L",
          "LABEL",
          *operands
        ],
        expected: 1
      )
    )
    assert_equal(-6, error.fetch("code"))
  end
end
