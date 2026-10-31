# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_partition_creation_preserves_volume_and_identifiers
    root = @root
    # c writes a disk around its volume: the volume at 1 MiB is the bare
    # volume byte for byte, the tables around it carry the derived or given
    # identifiers and settings, the plan reports each layer, and an invalid
    # setting publishes nothing.
    common = ["-R", "-e", "1700000000", "-C", "source", "."]
    invoke(arguments: ["c", "-F", "fat32", "-f", "bare.img", *common])
    invoke(arguments: ["c", "-F", "fat32/gpt", "-f", "made-gpt.img", *common])
    bare = (root / "bare.img").binread
    disk = (root / "made-gpt.img").binread
    assert_equal(bare, disk[(1 << 20)...((1 << 20) + bare.size)])
    # Epoch 1700000000 is 0x6553f100: its eight little-endian bytes, the
    # role (0x81 disk, 0x82 partition) and the slot, version 4.
    disk_guid = "00f15365-0000-4000-8100-000000000000"
    partition_guid = "00f15365-0000-4000-8201-000000000000"
    info = invoke(arguments: ["i", "-f", "made-gpt.img"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert_includes(info, "Stack:        fat32/gpt\nCompression:  none\nScheme:       GPT, 512-byte sectors\n", info)
    assert_includes(info, "Disk GUID:    #{disk_guid}\n", info)
    assert(Regexp
        .new(
          "Partition 1:  basic, [^\\n]* at 1 MiB, fat32, name \"Microsoft basic data\", GUID #{partition_guid}, selected\n"
        )
        .match(info), info)
    assert_equal(0xEE, disk.getbyte(450), "protective MBR and GPT signatures")
    assert_equal("U\xaa".b, disk[510...512], "protective MBR and GPT signatures")
    assert_equal("EFI PART".b, disk[512...520], "protective MBR and GPT signatures")
    assert_equal(uuid_bytes(value: disk_guid, little: true), disk[(512 + 56)...(512 + 72)])
    assert_equal(uuid_bytes(value: BASIC_TYPE, little: true), disk[1024...1040])
    assert_equal(uuid_bytes(value: partition_guid, little: true), disk[1040...1056])
    invoke(arguments: ["cW", "-F", "fat32/gpt", "-f", "made-gpt-2.img", *common])
    same_bytes(first: (root / "made-gpt.img"), second: (root / "made-gpt-2.img"))
    assert_equal(listing(image: "bare.img"), listing(image: "made-gpt.img"))
    # The plan reports the disk, the volume's place on it and the volume.
    decoded = completed(
      invoke(
        arguments: [
          "c",
          "-n",
          "--json",
          "-F",
          "fat16/mbr",
          "-o",
          "mbr:disk=64M",
          *common
        ]
      )
    )
    assert_equal(["stack", "partition", "plan", "summary"], decoded.map { |record| record.fetch("type") })
    stack, partition, plan = decoded[0...3]
    assert_equal((64 << 20).to_s, stack.fetch("disk_bytes"), "stack disk bytes")
    assert_equal(0x6553F100, stack.fetch("mbr_disk_id"), "stack mbr disk id")
    assert_equal((1 << 20).to_s, partition.fetch("offset_bytes"), "partition offset bytes")
    assert_equal(6, partition.fetch("mbr_type"), "partition mbr type")
    assert_equal(partition.fetch("length_bytes"), plan.fetch("volume_bytes"), "plan volume bytes")
    assert_equal((63 << 20).to_s, partition.fetch("length_bytes"), "partition length bytes")
    # Settings: type, name and disk length, the MBR status and identifier,
    # and 4096-byte GPT sectors.
    invoke(
      arguments: [
        "c",
        "-F",
        "fat12/gpt",
        "-f",
        "esp.img",
        "-o",
        "gpt:type=esp",
        "-o",
        "gpt:name=EFI",
        "-o",
        "gpt:disk=8M",
        *common
      ]
    )
    assert_equal((8 << 20), (root / "esp.img").stat.size)
    info = invoke(arguments: ["i", "-f", "esp.img"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert_includes(info, "Partition 1:  esp, ", info)
    assert_includes(info, "name \"EFI\"", info)
    invoke(
      arguments: [
        "c",
        "-F",
        "fat16/mbr",
        "-f",
        "made-mbr.img",
        "-o",
        "mbr:active=yes",
        "-o",
        "mbr:id=0x0badcafe",
        *common
      ]
    )
    disk = (root / "made-mbr.img").binread
    assert_equal([0x0BADCAFE].pack("L<"), disk[440...444], "explicit MBR disk identifier")
    assert_equal(0x80, disk.getbyte(446), "first MBR partition is active")
    assert_equal(0x06, disk.getbyte(450), "MBR partition type is FAT16")
    info = invoke(arguments: ["i", "-f", "made-mbr.img"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert_includes(info, "Disk ID:      0x0badcafe\n", info)
    assert_includes(info, "fat16, active, selected\n", info)
    invoke(arguments: ["c", "-F", "exfat/gpt", "-f", "4kn.img", "-o", "gpt:sector=4096", *common])
    assert_equal("EFI PART".b, (root / "4kn.img").binread[4096...4104])
    assert_includes(invoke(arguments: ["i", "-f", "4kn.img"]).stdout, "GPT, 4096-byte sectors".b, "invoke arguments i f 4kn img stdout")
    # Without -R each identifier is random, version 4.
    ["random-1.img", "random-2.img"].each do |name|
      invoke(arguments: ["c", "-F", "fat12/gpt", "-f", name, "-C", "source", "."])
    end

    guids = ["random-1.img", "random-2.img"].map { |name|
      records(
        invoke(
          arguments: [
            "i",
            "--json",
            "-f",
            name
          ]
        )
      )[0]
        .fetch("disk_guid")
    }
    refute_equal(guids[0], guids[1], "successive creations use distinct disk GUIDs")
    assert(guids.all? { |guid| /\A[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\z/i.match?(guid) }, "disk GUIDs must be version-4 UUIDs: #{guids.inspect}")
    [
      [["-F", "qnx4/gpt"], "the partition type has no GPT GUID \"gpt:type\"".b],
      [
        [
          "-F",
          "fat32/gpt",
          "-o",
          ("gpt:name=" + ("x" * 37))
        ],
        "the GPT name exceeds 36 UTF-16 units \"gpt:name\"".b
      ],
      [
        [
          "-F",
          "fat12/gpt",
          "-o",
          "gpt:disk=1M"
        ],
        "the disk is too small for a partition \"gpt:disk\"".b
      ],
      [
        [
          "-F",
          "fat12/mbr",
          "-o",
          "mbr:type=0x85"
        ],
        "extended and GPT protective types cannot be written \"mbr:type\"".b
      ],
      [
        [
          "-F",
          "fat12/mbr",
          "-o",
          "mbr:type=0xee"
        ],
        "extended and GPT protective types cannot be written \"mbr:type\"".b
      ],
      [
        [
          "-F",
          "fat12/mbr",
          "-o",
          "mbr:type=apfs"
        ],
        "the partition type has no MBR byte \"mbr:type\"".b
      ],
      [
        [
          "-F",
          "fat12/mbr",
          "-o",
          "mbr:disk=3T"
        ],
        "an MBR disk holds at most 2^32 sectors \"mbr:disk\"".b
      ]
    ].each do |arguments, detail|
      result = invoke(arguments: ["c", "-f", "refused.img", *arguments, *common], expected: 1)
      assert_includes(result.stderr, detail, [arguments, result.stderr])
      refute((root / "refused.img").exist?, "refused creation leaves no output image")
    end

    [
      [
        [
          "-F",
          "fat32/mbr",
          "-o",
          "gpt:type=esp"
        ],
        "setting names a layer the stack does not contain".b
      ],
      [
        [
          "-F",
          "fat32/gpt",
          "-o",
          "gpt:guid=00000000-0000-0000-0000-000000000000"
        ],
        "GUID setting must be RFC 4122 text other than nil".b
      ],
      [
        [
          "-F",
          "fat32/mbr",
          "-o",
          "mbr:id=12"
        ],
        "setting requires 0x and at most eight hex digits".b
      ],
      [["-F", "fat32/mbr", "-o", "mbr:active=1"], "setting must be yes or no".b],
      [
        [
          "-F",
          "fat32/gpt",
          "-o",
          "gpt:color=red"
        ],
        "unknown layer setting".b
      ]
    ].each do |arguments, detail|
      result = invoke(arguments: ["c", "-f", "refused.img", *arguments, *common], expected: 2)
      assert_includes(result.stderr, detail, [arguments, result.stderr])
    end

    result = invoke(
      arguments: [
        "tf",
        "made-gpt.img",
        "-F",
        "fat32/gpt",
        "-o",
        "gpt:type=esp"
      ],
      expected: 2
    )
    assert_includes(result.stderr, "setting applies only to creation".b, result.stderr)
  end
end
