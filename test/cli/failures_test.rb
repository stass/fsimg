# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_failures_report_resources_and_preserve_alias_targets
    exfat_image
    root = @root
    [
      ["i", "--json", "-f", "exfat.img", "extra"],
      ["x", "-O", "--json", "-f", "exfat.img", "/HELLO.TXT", "/nested"],
      ["c", "--json", "-F", "exfat", "-e", "315532800", "-n", "-C", "source", "."],
      ["i", "--json", "-f", "exfat.img", "-l", "work=1", "-l", "work=1"],
      ["i", "--json", "-f", "exfat.img", "-l", "seconds=18446744073709551615"],
      [
        "c",
        "--json",
        "-n",
        "-F",
        "qnx4",
        "-L",
        ("A" * 49),
        "-C",
        "source",
        "."
      ]
    ].each do |arguments|
      failed(invoke(arguments: arguments, expected: 2))
    end
    # An empty standard input is an unrecognised image, not a hang on
    # whatever input the test runner happens to hold.
    assert_equal(
      -4,
      failed(
        invoke(
          arguments: ["i", "--json", "-f", "-"],
          expected: 1,
          stdin: File::NULL
        )
      )
        .fetch("code")
    )
    # The command carries QNX4's 48-byte label; exFAT's own limit refuses it.
    assert_equal(
      "planned",
      completed(
        invoke(
          arguments: [
            "c",
            "--json",
            "-n",
            "-F",
            "qnx4",
            "-L",
            ("A" * 48),
            "-C",
            "source",
            "."
          ]
        )
      )[-1]
        .fetch("validation_scope")
    )
    failed(
      invoke(
        arguments: [
          "c",
          "--json",
          "-n",
          "-F",
          "exfat",
          "-L",
          ("A" * 48),
          "-C",
          "source",
          "."
        ],
        expected: 1
      )
    )
    planned = completed(
      invoke(
        arguments: [
          "c",
          "--json",
          "-n",
          "-R",
          "-F",
          "exfat",
          "-f",
          "unused.img",
          "-C",
          "source",
          "."
        ]
      )
    )
    assert_equal("planned", planned[-1].fetch("validation_scope"), "the final dry-run record reports planned validation")
    refute((root / "unused.img").exist?, "planning leaves no output image")
    assert_equal(
      "work",
      failed(
        invoke(
          arguments: [
            "W",
            "--json",
            "-f",
            "exfat.img",
            "-l",
            "work=1"
          ],
          expected: 1
        )
      )
        .fetch("resource")
    )
    assert_equal(
      "path",
      failed(
        invoke(
          arguments: [
            "t",
            "--json",
            "-f",
            "exfat.img",
            "-l",
            "path=4",
            "/abc"
          ],
          expected: 1
        )
      )
        .fetch("resource")
    )
    # A range addresses the disk, which only the worker sees once it has
    # peeled any container, so the library's selection refuses it.
    past_end = failed(
      invoke(
        arguments: [
          "i",
          "--json",
          "-f",
          "exfat.img",
          "--offset=1T"
        ],
        expected: 1
      )
    )
    assert_equal(-1, past_end.fetch("code"))
    assert_equal("the range starts past the end of the disk", past_end.fetch("message"))
    (root / "truncated.img").binwrite("\0".b * 511)
    failed(invoke(arguments: ["i", "--json", "-f", "truncated.img"], expected: 1))
    File.mkfifo(root / "stream")
    failed(invoke(arguments: ["i", "--json", "-f", "stream"], expected: 1))
    (root / "unsafe-source").mkdir
    (root / "sentinel").binwrite("outside".b)
    File.symlink("../sentinel", (root / "unsafe-source/pivot"))
    error = failed(
      invoke(
        arguments: [
          "c",
          "--json",
          "-R",
          "-F",
          "exfat",
          "-f",
          "unsafe.img",
          "-C",
          "unsafe-source",
          "."
        ],
        expected: 1
      )
    )
    assert_equal("absent", error.fetch("output_state"), "error output state")
    refute((root / "unsafe.img").exist?, "an unsafe source leaves no output image")
    assert_equal("outside".b, (root / "sentinel").binread)
    File.symlink("sentinel", (root / "output-pivot"))
    [[], ["-E"]].each do |flags|
      failed(
        invoke(
          arguments: [
            "x",
            *flags,
            "--json",
            "-f",
            "exfat.img",
            "-C",
            "output-pivot"
          ],
          expected: 1
        )
      )
    end

    assert_equal("outside".b, (root / "sentinel").binread)
    # The outer file contains a deliberately offset volume and unrelated tail.
    (root / "sliced.img").open("wb") do |output|
      (root / "exfat.img").open("rb") do |source|
        output.write("\0".b * 4096)
        IO.copy_stream(source, output)

        output.write("outside".b)
      end
    end

    volume_bytes = (root / "exfat.img").stat.size.to_s
    completed(
      invoke(
        arguments: [
          "W",
          "--json",
          "-d",
          "--offset=4096",
          "--length=#{volume_bytes}",
          "-f",
          "sliced.img"
        ]
      )
    )
  end
end
