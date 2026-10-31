# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_fat12_round_trip_is_reproducible_and_preserves_existing_outputs
    check_format(executable: @executable, root: @root, profile: "fat12", payload: @payload)
  end

  def test_fat16_round_trip_is_reproducible_and_preserves_existing_outputs
    check_format(executable: @executable, root: @root, profile: "fat16", payload: @payload)
  end

  def test_fat32_round_trip_is_reproducible_and_preserves_existing_outputs
    check_format(executable: @executable, root: @root, profile: "fat32", payload: @payload)
  end

  def test_exfat_round_trip_is_reproducible_and_preserves_existing_outputs
    check_format(executable: @executable, root: @root, profile: "exfat", payload: @payload)
  end

  private

  def check_format(executable:, root:, profile:, payload:)
    image = "#{profile}.img"
    decoded = completed(
      invoke(
        executable: executable,
        root: root,
        arguments: ["c", "--json", "-R", "-F", profile, "-f", image, "-C", "source", "."]
      )
    )
    plan = decoded.find { |record| record.fetch("type") == "plan" }
    assert_equal(profile, plan.fetch("format"), "#{profile}: planned filesystem")
    assert_equal(payload.size + 6, Integer(plan.fetch("payload_bytes"), 10), "#{profile}: planned payload bytes")
    assert_equal(true, decoded[-1].fetch("published"), "#{profile}: successful creation publishes its image")
    assert_equal(0, ((root / image).stat.mode & 0o077))
    information = completed(
      invoke(
        executable: executable,
        root: root,
        arguments: ["i", "--json", "-T", "-", "-f", image]
      )
    )
    info = information[1]
    assert_equal(profile, information[0].fetch("stack"), "#{profile}: detected layer stack")
    assert(information[0].fetch("volume_selected"), "#{profile}: bare filesystem selects its volume")
    assert_equal(profile, info.fetch("format"), "#{profile}: reported filesystem")
    assert_equal("FSIMG", info.fetch("label"), "#{profile}: default volume label")
    assert_equal(profile, info.fetch("stack"), "#{profile}: reported layer stack")
    assert_nil(info.fetch("compression"), "#{profile}: bare image has no compression")
    assert_equal("header", info.fetch("validation_scope"), "#{profile}: info validates the header")
    assert_nil(info.fetch("entry_count"), "#{profile}: header inspection does not scan directory entries")
    assert_nil(info.fetch("free_bytes"), "#{profile}: header inspection does not scan free space")
    listed = completed(
      invoke(
        executable: executable,
        root: root,
        arguments: ["t", "--json", "-f", image]
      )
    )
    assert_equal("t", listed[-1].fetch("command"))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal(["/HELLO.TXT", "/nested", "/nested/caf\u00e9.txt"].to_set, entries.keys.to_set)
    assert(entries.values.all? { |record| record.fetch("complete") }, "#{profile}: every listed entry is complete")
    assert(entries.fetch("/nested/caf\u00e9.txt").fetch("size_bytes").is_a?(String), "#{profile}: JSON byte sizes use decimal strings")
    modified = entries.fetch("/HELLO.TXT").fetch("modified")
    assert_equal(((profile == "exfat") ? "utc" : "wall-clock"), modified.fetch("basis"))
    assert_equal(
      payload,
      invoke(
        executable: executable,
        root: root,
        arguments: ["x", "-O", "-f", image, "/NESTED/CAF\u00c9.TXT"]
      )
        .stdout
    )
    # Extraction creates ./<profile> from <profile>.img.
    destination = (root / profile)
    completed(invoke(executable: executable, root: root, arguments: ["x", "--json", "-f", image]))
    same_bytes(first: (root / "source/nested/caf\u00e9.txt"), second: (destination / "nested/caf\u00e9.txt"))
    assert_equal("hello\n".b, (destination / "HELLO.TXT").binread)
    assert_equal(0, (destination.stat.mode & 0o077))
    verification = completed(
      invoke(
        executable: executable,
        root: root,
        arguments: ["W", "--json", "-d", "-f", image]
      )
    )
    assert_equal("W", verification[-1].fetch("command"))
    report = verification.find { |record| record.fetch("type") == "verify" }
    assert_equal("initialized-payload", report.fetch("validation_scope"), "#{profile}: verification reads initialized payload")
    assert_equal("2", report.fetch("files"), "#{profile}: verification counts both files")
    assert_equal((payload.size + 6), Integer(report.fetch("initialized_bytes_read"), 10))
    second = "#{profile}-second.img"
    completed(
      invoke(
        executable: executable,
        root: root,
        arguments: [
          "c",
          "--json",
          "-R",
          "-m",
          "16M",
          "-F",
          profile,
          "-f",
          second,
          "-C",
          "source",
          "."
        ]
      )
    )
    same_bytes(first: (root / image), second: (root / second))
    before = (root / image).stat
    error = failed(
      invoke(
        executable: executable,
        root: root,
        arguments: ["c", "--json", "-R", "-F", profile, "-f", image, "-C", "source", "."],
        expected: 1
      )
    )
    assert_equal(-12, error.fetch("code"), "#{profile}: creation refuses an existing image")
    assert_equal(before.ino, (root / image).stat.ino, "#{profile}: refused creation preserves the existing inode")
    assert_equal(
      -12,
      failed(
        invoke(
          executable: executable,
          root: root,
          arguments: ["x", "--json", "-f", image],
          expected: 1
        )
      )
        .fetch("code")
    )
  end

end
