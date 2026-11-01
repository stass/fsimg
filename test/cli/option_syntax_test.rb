# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_invalid_options_fail_before_work
    human_image
    root = @root
    # A usage error exits 2 before any work and names the offending word; a
    # recognized request the build does not support exits 1 with its reason.
    [
      [["x", "-k", "-k", "-f", "human.img"], "option is given more than once \"-k\"".b],
      [
        [
          "t",
          "-l",
          "work=1",
          "-l",
          "work=1",
          "-f",
          "human.img"
        ],
        "resource-limit key is given more than once \"work=1\"".b
      ],
      [
        [
          "c",
          "-n",
          "-F",
          "qnx4",
          "-o",
          "inodes=200",
          "-o",
          "inodes=200",
          "-C",
          "source",
          "."
        ],
        "setting key is given more than once \"inodes=200\"".b
      ],
      [["t", "-k", "-f", "human.img"], "option does not apply to mode t \"-k\"".b],
      [["i", "-d", "-f", "human.img"], "option does not apply to mode i \"-d\"".b],
      [["x", "-a", "-f", "human.img"], "option does not apply to mode x \"-a\"".b],
      [["tx", "-f", "human.img"], "two modes are given; only c and W combine".b],
      [["t", "-x", "-f", "human.img"], "two modes are given; only c and W combine".b],
      [["tt", "-f", "human.img"], "a mode is given more than once".b],
      [["-f", "human.img"], "a mode is required".b],
      [["if", "human.img", "extra"], "unexpected operand \"extra\"".b],
      [["if", "human.img", "back\\x41"], "unexpected operand \"back\\\\x41\"".b],
      [
        [
          "x",
          "-O",
          "-C",
          "source",
          "-f",
          "human.img",
          "/HELLO.TXT"
        ],
        "-O rejects --json, --mtree, -C, -E and -k".b
      ],
      [
        [
          "x",
          "-O",
          "-E",
          "-f",
          "human.img",
          "/HELLO.TXT"
        ],
        "-O rejects --json, --mtree, -C, -E and -k".b
      ],
      [
        [
          "x",
          "-O",
          "-k",
          "-f",
          "human.img",
          "/HELLO.TXT"
        ],
        "-O rejects --json, --mtree, -C, -E and -k".b
      ],
      [["x", "-O", "-f", "human.img"], "-O requires exactly one member".b],
      [["x", "-O", "-f", "human.img", "/HELLO.TXT", "/nested"], "-O requires exactly one member".b],
      [
        [
          "cf",
          "syntax.img",
          "-F",
          "fat12",
          "source/../source"
        ],
        "an operand contains a .. component \"source/../source\"".b
      ],
      [
        [
          "cf",
          "syntax.img",
          "-F",
          "fat12",
          "-C",
          "source",
          ".",
          "-C",
          "source"
        ],
        "-C applies to no operand".b
      ],
      [["cf", "syntax.img", "-F", "fat12"], "c requires at least one operand".b],
      [["cf", "syntax.img", "-C", "source", "."], "c requires -F with a filesystem".b],
      [["c", "-F", "fat12", "-C", "source", "."], "c requires -f image unless -n is given".b],
      [["c", "-M", "-s", "1M", "-F", "fat12", "-f", "syntax.img", "-C", "source", "."],
       "-M conflicts with -s".b],
      [["c", "-s", "1M", "-M", "-F", "fat12", "-f", "syntax.img", "-C", "source", "."],
       "-M conflicts with -s".b],
      [
        [
          "c",
          "-n",
          "-F",
          "fat12",
          "-e",
          "5",
          "-C",
          "source",
          "."
        ],
        "-e requires -R".b
      ]
    ].each do |arguments, detail|
      result = invoke(arguments: arguments, expected: 2)
      assert_includes(result.stderr, detail, [arguments, result.stderr])
      assert_includes(result.stderr, "usage: fsimg".b, [arguments, result.stderr])
      assert_equal("".b, result.stdout)
    end

    assert_equal(
      "-O rejects --json, --mtree, -C, -E and -k",
      failed(
        invoke(
          arguments: [
            "x",
            "-O",
            "--json",
            "-f",
            "human.img",
            "/HELLO.TXT"
          ],
          expected: 2
        )
      )
        .fetch("message")
    )
    [
      [["t", "-F", "fat32/-/vhd", "-f", "human.img"], "no VHD footer".b],
      [
        [
          "cf",
          "syntax.img.zst",
          "-a",
          "-F",
          "fat32",
          "-C",
          "source",
          "."
        ],
        "writing this compression is not supported".b
      ],
      [
        [
          "cf",
          "syntax.img",
          "-F",
          "apfs",
          "-C",
          "source",
          "."
        ],
        "the named filesystem is reserved and not supported".b
      ]
    ].each do |arguments, detail|
      result = invoke(arguments: arguments, expected: 1)
      assert_includes(result.stderr, detail, [arguments, result.stderr])
      assert_equal("".b, result.stdout, [arguments, result.stderr])
    end

    result = invoke(arguments: ["t", "-f", "human.img", "/nested", "-v"], expected: 1)
    assert_equal("\"/nested\"\n\"/nested/caf\u00e9.txt\"\n".b, result.stdout)
    assert_includes(result.stderr.lines, "fsimg: selected image path was not found \"/-v\"\n".b)
    assert_includes(result.stderr.lines, "fsimg: output is incomplete\n".b)

    syntax_outputs = root.children.select { |path| path.basename.to_s.start_with?("syntax.") }
    assert_empty(syntax_outputs, "Rejected creation options must leave no syntax.* outputs; found #{syntax_outputs.inspect}")
    refute((root / "-").exist?, "standard output creation leaves no file named dash")
    (root / "dotdot-parent").mkdir
    result = invoke(arguments: ["xf", "human.img", "-C", "dotdot-parent", "../x"], expected: 2)
    assert_includes(result.stderr, "a member contains a .. component \"../x\"".b, "result stderr")
    assert_empty((root / "dotdot-parent").children.to_a, "root dotdot parent children to a empty")
  end
end
