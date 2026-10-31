# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_containers_decode_verify_and_detect_corruption
    partition_fixtures
    root = @root
    # t, x, i and W read through a DMG, a fixed VHD and a sparse image, the
    # partition table inside included; a forced container slot runs only its
    # own probe; a range addresses the container's disk, longer than the
    # file; and a damaged chunk fails the read instead of passing raw bytes.
    # Incompressible bytes outside every partition give the DMG raw runs.
    disk = (root / "gpt.img").binread.dup
    disk[(6144 * 512)...(6400 * 512)] = Random.new(7).bytes(256 * 512)
    disk = disk.dup
    names = listing(image: "gpt.img")
    (root / "gpt.dmg").binwrite(
      dmg_image(
        disk: disk,
        regions: [
          [0, 2048],
          [2048, 2048],
          [4096, 4096]
        ]
      )
    )
    (root / "gpt.simg").binwrite(simg_image(disk: disk))
    (root / "part.vhd").binwrite(vhd_image(disk: (root / "part.img").binread))
    assert((root / "gpt.dmg").stat.size < (disk.size / 2), "root gpt dmg stat size")
    [
      ["gpt.dmg", "fat12/gpt/dmg"],
      ["gpt.simg", "fat12/gpt/simg"],
      ["part.vhd", "fat12/vhd"]
    ].each do |image, stack|
      assert_equal(names, listing(image: image), image)
      info = invoke(arguments: ["i", "-f", image]).stdout.dup.force_encoding(Encoding::UTF_8)
      assert(info.start_with?("Stack:        #{stack}\nCompression:  none\nContainer:    "), info)
      verified = invoke(arguments: ["W", "-d", "-f", image]).stdout
      assert_includes(verified, "\"#{image}\": #{stack.split("/").last} container,".encode(Encoding::UTF_8).b, verified)
      assert_includes(verified, "initialized file data verified".b, verified)
      assert_equal(
        names,
        invoke(
          arguments: [
            "tf",
            image,
            "-F",
            stack
          ]
        )
          .stdout
          .dup
          .force_encoding(Encoding::UTF_8)
          .lines(chomp: true)
      )
    end

    info = invoke(arguments: ["i", "-f", "gpt.dmg"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert_includes(info, "Container:    dmg, 4 MiB disk in ", info)
    assert_includes(info, "chunks (raw,zero,zlib), ID #{SEGMENT_ID}\n", info)
    assert_includes(info, "Scheme:       GPT, 512-byte sectors\nDisk size:    4 MiB\n", info)
    stack = completed(
      invoke(
        arguments: [
          "i",
          "--json",
          "-f",
          "gpt.dmg"
        ]
      )
    )
      .find { |record| record.fetch("type") == "stack" }
    assert_equal("fat12/gpt/dmg", stack.fetch("stack"), "stack stack")
    assert_equal("dmg", stack.fetch("container"), "stack container")
    assert_equal("raw,zero,zlib", stack.fetch("container_codecs"), "stack container codecs")
    assert_equal(SEGMENT_ID, stack.fetch("container_guid"), "stack container guid")
    assert(Integer(stack.fetch("container_chunks"), 10) > 3, "Integer stack container chunks 10")
    assert_equal(disk.size.to_s, stack.fetch("disk_bytes"), "stack disk bytes")
    report = completed(
      invoke(
        arguments: [
          "W",
          "-d",
          "--json",
          "-f",
          "gpt.dmg"
        ]
      )
    )
      .find { |record| record.fetch("type") == "verify" }
    assert_equal("dmg", report.fetch("container"), "report container")
    assert_equal("gpt", report.fetch("scheme"), "report scheme")
    plain = completed(
      invoke(
        arguments: [
          "i",
          "--json",
          "-f",
          "gpt.img"
        ]
      )
    )
      .find { |record| record.fetch("type") == "stack" }
    assert_nil(plain.fetch("container"), "plain container nil")
    assert_nil(plain.fetch("container_chunks"), "plain container chunks nil")
    (root / "from-dmg").mkdir
    invoke(arguments: ["xf", "gpt.dmg", "-C", "from-dmg"])
    assert_equal("hello\n".b, (root / "from-dmg/gpt/HELLO.TXT").binread)
    # The range lies inside the 4 MiB disk though the file is far shorter.
    assert_equal(
      names,
      invoke(
        arguments: [
          "tf",
          "gpt.dmg",
          "--offset=1M",
          "--length=1M"
        ]
      )
        .stdout
        .dup
        .force_encoding(Encoding::UTF_8)
        .lines(chomp: true)
    )
    result = invoke(arguments: ["tf", "gpt.dmg", "--offset=5M"], expected: 1)
    assert_includes(result.stderr, "the range starts past the end of the disk".b, result.stderr)
    result = invoke(arguments: ["tf", "gpt.dmg", "-F", "fat12/-/vhd"], expected: 1)
    assert_includes(result.stderr, "no VHD footer".b, result.stderr)
    # A flipped byte in the first zlib stream fails the read that decodes
    # it; one in raw bytes no read needs fails W on the data fork checksum.
    image = (root / "gpt.dmg").binread
    damaged = image.dup
    damaged.setbyte((damaged.index("x\xda".b) + 40), damaged.getbyte(damaged.index("x\xda".b) + 40) ^ 0xFF)
    (root / "damaged.dmg").binwrite(damaged)
    result = invoke(arguments: ["tf", "damaged.dmg"], expected: 1)
    assert_includes(result.stderr, "compressed chunk is not a valid zlib stream".b, result.stderr)
    damaged = image.dup
    damaged.setbyte(
      image.index(disk[(6144 * 512)...((6144 * 512) + 64)]),
      damaged.getbyte(image.index(disk[(6144 * 512)...((6144 * 512) + 64)])) ^ 0xFF
    )
    (root / "damaged.dmg").binwrite(damaged)
    assert_equal(names, listing(image: "damaged.dmg"))
    result = invoke(arguments: ["W", "-f", "damaged.dmg"], expected: 1)
    assert_includes(result.stderr, "DMG data fork checksum does not match".b, result.stderr)
  end
end
