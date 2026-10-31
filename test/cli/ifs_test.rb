# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_ifs_reads_implicit_shadowed_and_omitted_entries
    root = @root
    payload = @payload
    # A read-only ImageFS through the command: i reports the mountpoint
    # and the named header details, the listing synthesizes the missing
    # parent, and the file reads back byte for byte.
    (root / "boot.ifs").binwrite(ifs_image(payload: payload))
    info = completed(invoke(arguments: ["i", "--json", "-f", "boot.ifs"]))[1]
    assert_equal("ifs", info.fetch("format"), "info format")
    assert_equal("big-endian, bootable", info.fetch("profile"), "info profile")
    assert_equal("/proc/boot", info.fetch("mountpoint"), "info mountpoint")
    assert_nil(info.fetch("label"), "info label nil")
    assert_equal("3", info.fetch("details").fetch("mount_flags"))
    assert_equal("2", info.fetch("details").fetch("boot_ino_0"))
    # The root and bin have no record.
    assert_equal("2", info.fetch("details").fetch("implicit_directories"))
    assert_equal("0", info.fetch("details").fetch("shadowed_references"))
    human = invoke(arguments: ["i", "-f", "boot.ifs"]).stdout
    assert_includes(human, "Format:       IFS\n".b, "human")
    assert_includes(human, "Mountpoint:   \"/proc/boot\"\n".b, "human")
    assert_includes(human, "implicit_directories: 2\n".b, "human")
    listed = completed(invoke(arguments: ["t", "--json", "-f", "boot.ifs"]))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal(["/bin", "/bin/sh", "/etc", "/etc/motd"].to_set, entries.keys.to_set)
    assert_equal(0o755, entries.fetch("/bin").fetch("mode"), "entries bin mode")
    assert_equal(0, entries.fetch("/bin").fetch("uid"), "entries bin uid")
    assert_equal("symlink", entries.fetch("/bin/sh").fetch("kind"))
    assert_equal(0o640, entries.fetch("/etc/motd").fetch("mode"), "entries etc motd mode")
    assert_equal(9, entries.fetch("/etc/motd").fetch("uid"), "entries etc motd uid")
    assert_equal(7, entries.fetch("/etc/motd").fetch("gid"))
    assert_equal(payload, invoke(arguments: ["x", "-O", "-f", "boot.ifs", "/etc/motd"]).stdout)
    # W checks the checksum and the boot reference and walks the tree; a
    # record below the link counts as shadowed, and a flipped data byte
    # fails the checksum.
    verified = invoke(arguments: ["W", "-d", "-f", "boot.ifs"]).stdout
    assert_equal(
      "\"boot.ifs\": filesystem structure and initialized file data verified (2 files, 3 directories)\n".b,
      verified,
      verified
    )
    report = completed(
      invoke(
        arguments: [
          "W",
          "--json",
          "-f",
          "boot.ifs"
        ]
      )
    )
      .find { |record| record.fetch("type") == "verify" }
    assert_equal("2", report.fetch("files"), report)
    assert_equal("3", report.fetch("directories"), report)
    (root / "shadowed.ifs").binwrite(ifs_image(payload: payload, shadowed: 1))
    verified = invoke(arguments: ["W", "-f", "shadowed.ifs"]).stdout
    assert_equal(
      "\"shadowed.ifs\": filesystem structure verified (2 files, 3 directories, 1 shadowed record)\n".b,
      verified,
      verified
    )
    # A shadowed record carrying the boot program's inode word is counted
    # beside the reference, which still names the delivered file.
    (root / "twin.ifs").binwrite(ifs_image(payload: payload, shadowed: 1, twin: true))
    info = completed(invoke(arguments: ["i", "--json", "-f", "twin.ifs"]))[1]
    assert_equal("1", info.fetch("details").fetch("shadowed_references"))
    assert_includes(invoke(
        arguments: [
          "i",
          "-f",
          "twin.ifs"
        ]
      )
        .stdout, "shadowed_references: 1\n".b, "invoke arguments i f twin ifs stdout")
    verified = invoke(arguments: ["W", "-f", "twin.ifs"]).stdout
    assert_equal(
      "\"twin.ifs\": filesystem structure verified (2 files, 3 directories, 1 shadowed record, 1 boot or script reference matching a shadowed record)\n"
        .b,
      verified,
      verified
    )
    # A boot program whose name is outside the profile is omitted, still
    # verified, and its reference counted.
    (root / "omitted.ifs").binwrite(ifs_image(payload: payload, shadowed: 2, motd: "mo\xe9td".b))
    verified = invoke(arguments: ["W", "-f", "omitted.ifs"])
    assert_equal(
      "\"omitted.ifs\": filesystem structure verified (1 files, 3 directories, 2 shadowed records, 1 boot or script reference to an omitted entry)\n"
        .b,
      verified.stdout,
      verified
    )
    (root / "omitted2.ifs").binwrite(ifs_image(payload: payload, motd: "mo\xe9td".b, script: 2))
    verified = invoke(arguments: ["W", "-f", "omitted2.ifs"])
    assert_equal(
      "\"omitted2.ifs\": filesystem structure verified (1 files, 3 directories, 2 boot or script references to omitted entries)\n"
        .b,
      verified.stdout,
      verified
    )
    damaged = ifs_image(payload: payload).dup
    damaged.setbyte(damaged.index(payload), damaged.getbyte(damaged.index(payload)) ^ 0x01)
    (root / "damaged.ifs").binwrite(damaged)
    result = invoke(arguments: ["W", "-f", "damaged.ifs"], expected: 1)
    assert_includes(result.stderr, "ImageFS checksum does not match".b, result.stderr)
  end
end
