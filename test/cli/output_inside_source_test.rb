# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_output_and_scratch_staging_are_excluded_from_source
    root = @root
    # An image created inside the tree it imports, with scratch there too:
    # the staging and scratch directories fsimg prepares are passed over, and
    # ordinary directories with similar names are imported.
    source = (root / "inside-source")
    FileUtils.mkdir_p(source / ".fsimg-output-ordinary")
    (source / ".fsimg-output-ordinary/kept.txt").binwrite("kept\n".b)
    (source / ".fsimg-scratch-ordinary").mkdir
    (source / "hello.txt").binwrite("hello\n".b)
    private_directory(root: source, name: "tmp")
    before = source.children.map { |path| path.basename.to_s }.sort
    invoke(root: source, arguments: ["cf", "out.img", "-F", "fat32", "-T", "tmp", "."])
    assert_equal(
      [
        "\"/.fsimg-output-ordinary\"",
        "\"/.fsimg-output-ordinary/kept.txt\"",
        "\"/.fsimg-scratch-ordinary\"",
        "\"/hello.txt\"",
        "\"/tmp\""
      ],
      listing(root: source, image: "out.img").sort
    )
    assert_equal([*before, "out.img"].sort, source.children.map { |path| path.basename.to_s }.sort)
    assert_empty((source / "tmp").children.to_a, "source tmp children to a empty")
    FileUtils.remove_entry(source)
  end
end
