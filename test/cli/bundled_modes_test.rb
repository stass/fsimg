# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_bundled_modes_match_separate_options
    root = @root
    # The first argument may bundle the mode with option letters, whose
    # arguments follow in letter order; a leading dash spells the same bundle.
    created = completed(
      invoke(
        arguments: [
          "cfF",
          "bundled.img",
          "fat32",
          "--json",
          "-R",
          "-C",
          "source",
          "."
        ]
      )
    )
    assert_equal("c", created[-1].fetch("command"), "the final creation record identifies the create command")
    assert_equal(true, created[-1].fetch("published"), "the final creation record confirms image publication")
    information = completed(invoke(arguments: ["if", "bundled.img", "--json"]))
    assert_equal("stack", information[0].fetch("type"), "information 0 type")
    assert_nil(information[0].fetch("scheme"), "information 0 scheme nil")
    assert_equal("fat32", information[1].fetch("format"), "information 1 format")
    assert_equal("fat32", information[1].fetch("stack"), "information 1 stack")
    assert_nil(information[1].fetch("compression"), "information 1 compression nil")
    assert_equal("i", information[-1].fetch("command"), "the final information record identifies the info command")
    assert_equal(
      invoke(
        arguments: [
          "t",
          "-f",
          "bundled.img"
        ]
      )
        .stdout
        .dup
        .force_encoding(Encoding::UTF_8)
        .lines(chomp: true),
      listing(image: "bundled.img")
    )
    assert_equal("hello\n".b, invoke(arguments: ["xOf", "bundled.img", "/HELLO.TXT"]).stdout)
    [["xvf", "bundled-plain"], ["-xf", "bundled-dashed"]].each do |spelling, parent|
      (root / parent).mkdir
      invoke(arguments: [spelling, "bundled.img", "-C", parent])
      same_bytes(
        first: (root / "source/nested/caf\u00e9.txt"),
        second: ((root / parent) / "bundled/nested/caf\u00e9.txt")
      )
    end
  end
end
