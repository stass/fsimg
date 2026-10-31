# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_partition_selection_validates_tables_and_escapes_names
    root = @root
    invoke(arguments: ["c", "-R", "-F", "fat12", "-s", "1M", "-f", "part.img", "-C", "source", "."])
    volume = (root / "part.img").binread
    names = listing(image: "part.img")
    (root / "gpt.img").binwrite(
      gpt_disk(
        sectors: 8192,
        partitions: [
          GptPartition.new(first_lba: 2048, last_lba: 4095, type: ESP_TYPE, name: "EFI", payload: volume),
          GptPartition.new(first_lba: 4096, last_lba: 6143, type: LINUX_TYPE, name: "data", payload: "".b)
        ]
      )
    )
    info = invoke(arguments: ["i", "-f", "gpt.img"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert(info.start_with?(
        "Stack:        fat12/gpt\nCompression:  none\nScheme:       GPT, 512-byte sectors\nDisk size:    4 MiB\nDisk GUID:    #{DISK_GUID}\nPartition 1:  esp, 1 MiB at 1 MiB, fat12, name \"EFI\", GUID 00000000-0000-0000-0000-000000000001, selected\nPartition 2:  linux, 1 MiB at 2 MiB, no filesystem, name \"data\", GUID 00000000-0000-0000-0000-000000000002\nFormat:       FAT12\n"
      ), info)
    decoded = completed(invoke(arguments: ["i", "--json", "-f", "gpt.img"]))
    assert_equal(
      [
        "stack",
        "partition",
        "partition",
        "info",
        "summary"
      ],
      decoded.map { |record| record.fetch("type") }
    )
    stack, first, second, volume_info = decoded[0...4]
    assert_equal("fat12/gpt", stack.fetch("stack"), "stack stack")
    assert_equal("gpt", stack.fetch("scheme"), "stack scheme")
    assert_equal(DISK_GUID, stack.fetch("disk_guid"), "stack disk guid")
    assert_equal(1, stack.fetch("selected_partition"), "stack selected partition")
    assert_equal("esp", first.fetch("partition_type"), "first partition type")
    assert_equal(ESP_TYPE, first.fetch("type_guid"), "first type guid")
    assert_equal("EFI", first.fetch("name"), "first name")
    assert_equal("fat12", first.fetch("format"), "first format")
    assert(first.fetch("selected"), "first selected")
    assert_equal((1 << 20).to_s, first.fetch("offset_bytes"), "first offset bytes")
    assert_equal((1 << 20).to_s, first.fetch("length_bytes"), "first length bytes")
    assert_equal("none", second.fetch("filesystem_claim"), "second filesystem claim")
    assert_nil(second.fetch("format"), "second format nil")
    assert_equal("fat12/gpt", volume_info.fetch("stack"))
    [
      [],
      ["--partition=1"],
      ["--partition=name=EFI"],
      ["--partition=type=esp"],
      ["--partition", "type=#{ESP_TYPE}"],
      ["-F", "fat12/gpt", "-o", "gpt:sector=512"],
      ["--offset=1M", "--length=1M"]
    ].each do |selection|
      assert_equal(
        names,
        invoke(
          arguments: [
            "tf",
            "gpt.img",
            *selection
          ]
        )
          .stdout
          .dup
          .force_encoding(Encoding::UTF_8)
          .lines(chomp: true),
        selection
      )
    end

    [
      [["--partition=3"], "the selected partition slot is unused".b],
      [["--partition=200"], "the partition table has no slot of that number".b],
      [["--partition=name=efi"], "no partition has that name".b],
      [["--partition=type=0x0c"], "an MBR type byte cannot select a GPT partition".b],
      [["-F", "gpt", "-o", "gpt:sector=4096"], "GPT".b],
      [["-F", "fat12/mbr"], "no MBR partition table".b]
    ].each do |selection, detail|
      result = invoke(arguments: ["tf", "gpt.img", *selection], expected: 1)
      assert_includes(result.stderr, detail, [selection, result.stderr])
    end

    invoke(arguments: ["tf", "gpt.img", "--partition=2"], expected: 1)
    verified = invoke(arguments: ["Wf", "gpt.img"]).stdout
    assert_includes(verified, "\"gpt.img\": GPT partition table (2 partitions) and partition 1 filesystem structure verified".b, verified)
    report = completed(
      invoke(
        arguments: [
          "W",
          "--json",
          "-f",
          "gpt.img"
        ]
      )
    )
      .find { |record| record.fetch("type") == "verify" }
    assert_equal("gpt", report.fetch("scheme"), "report scheme")
    assert_equal(2, report.fetch("partitions_checked"), "report partitions checked")
    assert_equal(1, report.fetch("partition"), "report partition")
    assert(stack.fetch("sector_bytes").is_a?(Integer), "stack sector bytes is a Integer")
    invoke(arguments: ["xf", "gpt.img"])
    assert_equal("hello\n".b, (root / "gpt/HELLO.TXT").binread)
    # A partition name is untrusted text: terminal controls are escaped.
    (root / "hostile.img").binwrite(
      gpt_disk(
        sectors: 8192,
        partitions: [
          GptPartition.new(first_lba: 2048, last_lba: 4095, type: ESP_TYPE, name: "E\u001b]0;x\u0007\u009b2J", payload: volume)
        ]
      )
    )
    info = invoke(arguments: ["i", "-f", "hostile.img"]).stdout
    assert_includes(info, "Partition 1:  esp".b, info)
    refute(["\x1b".b, "\x07".b, "\xc2\x9b".b].any? { |control| (info.include?(control)) }, info)
    # A damaged table fails every partition selection, never a raw range.
    damaged = (root / "gpt.img").binread.dup
    damaged.setbyte((512 + 16), damaged.getbyte(512 + 16) ^ 1)
    (root / "damaged-gpt.img").binwrite(damaged)
    result = invoke(arguments: ["tf", "damaged-gpt.img"], expected: 1)
    assert_includes(result.stderr, "GPT primary header CRC32 mismatch".b, result.stderr)
    assert_equal(
      names,
      invoke(
        arguments: [
          "tf",
          "damaged-gpt.img",
          "--offset=1M",
          "--length=1M"
        ]
      )
        .stdout
        .dup
        .force_encoding(Encoding::UTF_8)
        .lines(chomp: true)
    )
    # Two filesystems leave i without a volume and every other mode failing.
    (root / "mbr.img").binwrite(
      mbr_disk(
        sectors: 8192,
        partitions: [
          MbrPartition.new(first_lba: 2048, sector_count: 2048, type: 0x01, payload: volume),
          MbrPartition.new(first_lba: 4096, sector_count: 2048, type: 0x01, payload: volume)
        ]
      )
    )
    info = invoke(arguments: ["i", "-f", "mbr.img"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert_equal(
      "Stack:        mbr\nCompression:  none\nScheme:       MBR, 512-byte sectors\nDisk size:    4 MiB\nDisk ID:      0xa1b2c3d4\nPartition 1:  fat12, 1 MiB at 1 MiB, fat12, active\nPartition 2:  fat12, 1 MiB at 2 MiB, fat12\nVolume:       none selected: several partitions hold a filesystem: 1, 2; choose one with --partition\n",
      info,
      info
    )
    decoded = completed(invoke(arguments: ["i", "--json", "-f", "mbr.img"]))
    assert_equal(
      [
        "stack",
        "partition",
        "partition",
        "summary"
      ],
      decoded.map { |record| record.fetch("type") }
    )
    assert_equal(false, decoded[0].fetch("volume_selected"), "decoded 0 volume selected")
    assert_equal(0xA1B2C3D4, decoded[0].fetch("mbr_disk_id"), "decoded 0 mbr disk id")
    assert_nil(decoded[0].fetch("selected_partition"), "decoded 0 selected partition nil")
    assert_equal(1, decoded[1].fetch("index"), "decoded 1 index")
    assert_equal(1, decoded[1].fetch("mbr_type"), "decoded 1 mbr type")
    assert_equal(true, decoded[1].fetch("active"), "decoded 1 active")
    result = invoke(arguments: ["tf", "mbr.img"], expected: 1)
    assert_includes(result.stderr, "several partitions hold a filesystem: 1, 2".b, "result stderr")
    assert_equal(
      names,
      invoke(
        arguments: [
          "tf",
          "mbr.img",
          "--partition=2"
        ]
      )
        .stdout
        .dup
        .force_encoding(Encoding::UTF_8)
        .lines(chomp: true)
    )
    result = invoke(arguments: ["tf", "mbr.img", "--partition=name=EFI"], expected: 1)
    assert_includes(result.stderr, "partition names exist only in GPT".b, "result stderr")
    info = invoke(arguments: ["i", "-f", "mbr.img", "--partition=type=0x01"], expected: 1)
    assert_includes(info.stderr, "several partitions have that type".b, "info stderr")
  end
end
