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

class CliTest < Minitest::Test
  def test_named_fat_without_force_suggests_assuming_identity
    write_atari_floppy
    {
      "fat12/-/-" => "unrecognized volume boot record",
      "fat12" => "no partition table or filesystem recognized"
    }.each do |stack, reason|
      result = invoke(arguments: ["t", "-F", stack, "-f", "atari.img"], expected: 1)
      assert_includes(result.stderr.lines,
        "fsimg: #{reason}; use --force to read it as fat12 anyway\n".b, stack)
    end
  end

  def test_force_lists_and_extracts_atari_file_across_clusters
    payload = write_atari_floppy
    result = invoke(arguments: ["t", "-F", "fat12", "--force", "-f", "atari.img"])
    assert_equal(["\"/HELLO.TXT\"\n".b], result.stdout.lines)
    invoke(arguments: ["x", "-F", "fat12", "--force", "-f", "atari.img"])
    assert_equal(payload, (@root / "atari/HELLO.TXT").binread)
  end

  def test_force_verification_reports_assumed_identity
    write_atari_floppy
    result = invoke(arguments: ["W", "-F", "fat12", "--force", "-f", "atari.img"], expected: 1)
    assert_includes(result.stderr, "fat12 identity was assumed: the boot record has no PC boot " \
      "signature and the first FAT does not repeat the media byte".b)
  end

  def test_force_requires_a_named_filesystem_and_a_read_mode
    result = invoke(arguments: ["t", "--force", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "--force requires a filesystem in -F".b)
    result = invoke(arguments: ["c", "--force", "-F", "fat12", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "does not apply to mode".b)
  end

  def test_automatic_detection_failure_does_not_suggest_force
    write_atari_floppy
    result = invoke(arguments: ["t", "-f", "atari.img"], expected: 1)
    assert_includes(result.stderr, "no partition table or filesystem recognized".b)
    refute_includes(result.stderr, "--force".b)
  end

  def test_force_verifies_a_signed_fat_volume
    human_image
    invoke(arguments: ["W", "-F", "fat12", "--force", "-f", "human.img"])
  end

  def test_force_preserves_iso_listing_and_verification
    image_fixture(name: "force.iso", format: "iso9660")
    plain = invoke(arguments: ["t", "-F", "iso9660", "-f", "force.iso"])
    forced = invoke(arguments: ["t", "-F", "iso9660", "--force", "-f", "force.iso"])
    assert_includes(plain.stdout.lines, "\"/HELLO.TXT\"\n".b)
    assert_equal(plain.stdout, forced.stdout)
    invoke(arguments: ["W", "-F", "iso9660", "--force", "-f", "force.iso"])
  end

  private

  def write_atari_floppy
    # Independent FAT12 layout: 720 sectors, two three-sector FATs, a
    # one-sector fixed root, and 712 data clusters. Cluster 2 points to 3.
    # Neither boot signature nor reserved FAT entries vouch for identity.
    image = "\0".b * (720 * 512)
    image[0, 3] = "\x60\x38\0".b
    image[11, 2] = [512].pack("v")
    image.setbyte(13, 1)
    image[14, 2] = [1].pack("v")
    image.setbyte(16, 2)
    image[17, 2] = [16].pack("v")
    image[19, 2] = [720].pack("v")
    image.setbyte(21, 0xF8)
    image[22, 2] = [3].pack("v")
    [512, 4 * 512].each do |offset|
      image[offset, 6] = [0, 0, 3, 3, 0xF0, 0xFF].pack("C*")
    end
    root = 7 * 512
    image[root, 11] = "HELLO   TXT".b
    image.setbyte(root + 11, 0x20)
    image[root + 26, 2] = [2].pack("v")
    payload = (0...900).map { (_1 * 37) % 256 }.pack("C*")
    image[root + 28, 4] = [payload.bytesize].pack("V")
    image[8 * 512, payload.bytesize] = payload
    (@root / "atari.img").binwrite(image)
    payload
  end
end
