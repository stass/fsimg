# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_coincident_operand_directories_merge
    merge_fixtures

    # Operands whose directories coincide merge into one image directory,
    # and a regular-file operand is stored as a file.
    invoke(
      arguments: [
        "cf",
        "merged.img",
        "-F",
        "fat12",
        "-C",
        "merge-a",
        ".",
        "-C",
        "merge-b",
        "sub"
      ]
    )
    assert_equal(["\"/sub\"", "\"/sub/x\"", "\"/sub/y\""], listing(image: "merged.img").sort)
    invoke(arguments: ["cf", "file.img", "-F", "fat12", "-C", "source", "HELLO.TXT"])
    assert_equal(["\"/HELLO.TXT\""], listing(image: "file.img"))
    assert_equal("hello\n".b, invoke(arguments: ["xOf", "file.img", "/HELLO.TXT"]).stdout)
  end
end
