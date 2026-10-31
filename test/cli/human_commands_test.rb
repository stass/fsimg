# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_human_commands_report_images_and_metadata
    root = @root
    payload = @payload
    image = "human.img"
    created = invoke(arguments: ["c", "-F", "fat12", "-f", image, "-C", "source", "."])
    assert_equal("".b, created.stdout)
    assert_equal(1, created.stderr.scan("metadata fields".b).size)
    refute_includes(created.stderr, "fields=0x".b, "image creation warning")
    assert_includes(created.stderr, "use -v".b, "image creation warning")
    if created.stderr.include?("sandbox unavailable".b)
      assert_equal(1, created.stderr.scan("sandbox unavailable".b).size)
    end

    names = ["\"/HELLO.TXT\"", "\"/nested\"", "\"/nested/caf\u00e9.txt\""]
    assert_equal(names, listing(image: image))
    assert_equal(
      names,
      invoke(
        arguments: [
          "t",
          "-f",
          image
        ]
      )
        .stdout
        .dup
        .force_encoding(Encoding::UTF_8)
        .lines(chomp: true)
    )
    one_level = invoke(arguments: ["t", "--no-recursion", "-f", image])
    assert_equal(names[0...2], one_level.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true))
    # FAT stores no permission bits; the sizes and times are the image's.
    columns = long_listing(invoke(arguments: ["tvf", image]))
    assert_equal(names, columns.keys)
    assert_equal(["-", "?????????", "6"], columns[names[0]][0...3])
    assert_equal(["d", "?????????", "-"], columns[names[1]][0...3])
    assert_equal(["-", "?????????", "131101"], columns[names[2]][0...3])
    assert(columns.values.all? { |column| (column[3] != "-") }, "every FAT listing row includes a timestamp")
    info = invoke(arguments: ["i", "-f", image])
    assert(info.stdout.start_with?("Stack:        fat12\nCompression:  none\n".b), "image information identifies uncompressed FAT12")
    assert_includes(info.stdout, "Format:       FAT12\n".b, "FAT12 image information")
    assert_includes(info.stdout, "Validation:   header only\n".b, "FAT12 image information")
    assert_includes(info.stdout, "Free space:   unknown (not scanned)\n".b, "FAT12 image information")
    refute_includes(info.stdout, "complete:".b, "FAT12 image information")
    refute_includes(info.stdout, "profile=".b, "FAT12 image information")
    assert_equal(info.stdout, invoke(arguments: ["--info", "-f", image]).stdout)
    plan = invoke(arguments: ["c", "-n", "-R", "-F", "fat12", "-C", "source", "."])
    assert_includes(plan.stdout, "Volume size:".b, "creation dry run")
    assert_includes(plan.stdout, "Metadata:".b, "creation dry run")
    refute_includes(plan.stdout, "source profile".b, "creation dry run")
    refute_includes(plan.stdout, "complete:".b, "creation dry run")
    verbose = invoke(
      arguments: [
        "c",
        "-vv",
        "-F",
        "fat12",
        "-f",
        "human-verbose.img",
        "-C",
        "source",
        "."
      ]
    )
    assert_equal("".b, verbose.stdout, "verbose creation writes no standard output")
    assert_includes(verbose.stderr, "Volume size:".b, "verbose creation diagnostics")
    assert_equal(1, verbose.stderr.scan("source profile requires".b).size)
    assert_includes(verbose.stderr, "metadata example:".b, "verbose creation diagnostics")
    assert_includes(verbose.stderr, "modification time".b, "verbose creation diagnostics")
    refute_includes(verbose.stderr, "fields=0x".b, "verbose creation diagnostics")
    verified = invoke(arguments: ["W", "-f", image])
    assert_equal("\"human.img\": filesystem structure verified (2 files, 2 directories)\n".b, verified.stdout)
    payload = invoke(arguments: ["W", "-d", "-f", image])
    assert_includes(payload.stdout, "filesystem structure and initialized file data verified".b, "verification with payload reads")
    # The fresh directory is named after the image, beside the working one.
    extracted = invoke(arguments: ["x", "-f", image])
    assert_equal("".b, extracted.stdout, "extraction writes no standard output")
    assert_equal(1, extracted.stderr.scan("metadata fields".b).size, "extraction emits one metadata loss warning")
    same_bytes(first: (root / "source/nested/caf\u00e9.txt"), second: (root / "human/nested/caf\u00e9.txt"))
    assert_equal(0o700, ((root / "human").stat.mode & 0o777))
    raw = invoke(arguments: ["x", "-O", "-f", image, "/HELLO.TXT"])
    assert_equal("hello\n".b, raw.stdout)
    missing = invoke(arguments: ["i", "-f", "missing\n\u001b.img"], expected: 1)
    assert_equal("".b, missing.stdout, "a missing image writes no standard output")
    assert_includes(missing.stderr, "cannot open image \"missing\\u000a\\u001b.img\"".b, "missing image diagnostics")
    assert_includes(missing.stderr, "No such file or directory".b, "missing image diagnostics")
    refute_includes(missing.stderr, "errno=".b, "missing image diagnostics")
    refute_includes(missing.stderr, "\x1b".b, "missing image diagnostics")
    assert_equal(1, missing.stderr.lines(chomp: true).size, "a missing image produces one escaped diagnostic line")
    geometry = invoke(
      arguments: [
        "c",
        "-R",
        "-F",
        "fat32",
        "-s",
        "8M",
        "-f",
        "small.img",
        "-C",
        "source",
        "."
      ],
      expected: 1
    )
    assert_includes(geometry.stderr, "too small for FAT32".b, "undersized FAT32 image diagnostics")
    assert_includes(geometry.stderr, "8388608 bytes".b, "undersized FAT32 image diagnostics")
    refute_includes(geometry.stderr, "errno".b, "undersized FAT32 image diagnostics")
    refute((root / "small.img").exist?, "an invalid FAT32 size leaves no output image")
    [
      [["-F", "fat12", "-s", "1G"], "too large for FAT12".b],
      [
        [
          "-F",
          "fat12",
          "-s",
          "1M",
          "--free-space=1M"
        ],
        "cannot fit the source tree and requested free space".b
      ],
      [
        [
          "-F",
          "fat12",
          "--free-space=1G"
        ],
        "source tree and requested free space are too large for FAT12".b
      ],
      [
        [
          "-F",
          "fat32",
          "-o",
          "cluster=32K",
          "--free-space=4T"
        ],
        "required image exceeds FAT32 size limits".b
      ],
      [
        [
          "-F",
          "fat32",
          "--free-space=4T"
        ],
        "requested size and allocation settings conflict".b
      ]
    ].each do |settings, explanation|
      rejected = invoke(arguments: ["c", "-n", "-R", *settings, "-C", "source", "."], expected: 1)
      assert_includes(rejected.stderr, explanation, [settings, rejected.stderr])
      refute_includes(rejected.stderr, "image size 0".b, "refused free-space plan")
      refute_includes(rejected.stderr, "free-space reserve 0".b, "refused free-space plan")
    end
  end
end
