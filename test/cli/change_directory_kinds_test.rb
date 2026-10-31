# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_change_directory_reports_unsupported_kinds_and_open_failures
    human_image
    root = @root
    # A -C naming an existing object that is not a directory is refused as
    # unsupported, as with -E, for x and c alike. The -C directory is followed
    # through a symbolic link, as tar's -C is, so a link to a file is that
    # file; a path through a non-directory names nothing and fails to open.
    work = (root / "change-kinds")
    work.mkdir
    (work / "file").binwrite("x".b)
    File.symlink("file", (work / "to-file"))
    File.mkfifo(work / "fifo")
    File.symlink("missing", (work / "dangling"))
    File.symlink("loop", (work / "loop"))
    FileUtils.copy_file((root / "human.img"), (work / "human.img"))
    refusals = -> (directory:) do
      return [
        [
          "xf",
          "human.img",
          "--json",
          "-C",
          directory
        ],
        ["cf", "out.img", "-F", "fat12", "--json", "-C", directory, "."]
      ]
    end

    ["file", "to-file", "file/", "fifo"].each do |directory|
      refusals.call(directory: directory).each do |arguments|
        error = failed(invoke(root: work, arguments: arguments, expected: 1))
        assert_equal("unsupported capability", error.fetch("status"), [arguments, error])
        assert_equal("-C directory is not a directory", error.fetch("message"), error)
        assert_equal(directory, error.fetch("path"), error)
      end
    end

    [
      ["file/inner", Errno::ENOTDIR::Errno],
      ["dangling", Errno::ENOENT::Errno],
      ["loop", Errno::ELOOP::Errno]
    ].each do |directory, os_error|
      refusals.call(directory: directory).each do |arguments|
        error = failed(invoke(root: work, arguments: arguments, expected: 1))
        assert_equal("I/O failure", error.fetch("status"), [arguments, error])
        assert_equal("cannot open -C directory", error.fetch("message"), error)
        assert_equal(os_error, error.fetch("os_error"), error)
      end
    end

    assert_equal(
      [
        "dangling",
        "fifo",
        "file",
        "human.img",
        "loop",
        "to-file"
      ],
      work.children.map { |path| path.basename.to_s }.sort
    )
  end
end
