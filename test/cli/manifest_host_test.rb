# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_manifest_host_bytes_require_a_content_directory
    root = @root
    # A manifest's host files are read beneath the -C directory in effect,
    # by the entry's own path or its contents= path; without a -C the manifest
    # has no content directory, so an entry needing host bytes is refused even
    # where the working directory holds the file; a manifest merges with a
    # host root; and ./@name names a host object whose name begins with @.
    work = (root / "manifest-host")
    FileUtils.mkdir_p(work / "content/sub")
    (work / "content/sub/file.txt").binwrite("host bytes\n".b)
    (work / "content/elsewhere.bin").binwrite("other bytes\n".b)
    (work / "content/host.mtree").write(
      ". type=dir\n./sub type=dir\n./sub/file.txt type=file size=11\n./sub/named type=file contents=elsewhere.bin\n"
    )
    FileUtils.mkdir_p(work / "tree/sub")
    (work / "tree/sub/x").binwrite("x".b)
    operands = ["-C", "content", "@host.mtree", "-C", "tree", "."]
    invoke(root: work, arguments: ["cf", "host.img", "-F", "fat12", *operands])
    assert_equal(
      [
        "\"/sub\"",
        "\"/sub/file.txt\"",
        "\"/sub/named\"",
        "\"/sub/x\""
      ],
      listing(root: work, image: "host.img").sort
    )
    assert_equal(
      "host bytes\n".b,
      invoke(
        root: work,
        arguments: [
          "xOf",
          "host.img",
          "/sub/file.txt"
        ]
      )
        .stdout
    )
    assert_equal(
      "other bytes\n".b,
      invoke(
        root: work,
        arguments: [
          "xOf",
          "host.img",
          "/sub/named"
        ]
      )
        .stdout
    )
    error = failed(
      invoke(
        root: (work / "content"),
        arguments: ["cf", "unanchored.img", "-F", "fat12", "--json", "@host.mtree"],
        expected: 1
      )
    )
    assert_equal("invalid argument", error.fetch("status"), error)
    assert_equal("/sub/file.txt", error.fetch("path"), error)
    assert_equal(
      "manifest line 3: entry needs host bytes but the manifest has no content directory",
      error.fetch("message"),
      error
    )
    refute((work / "content/unanchored.img").exist?, "an unanchored manifest source leaves no output image")
    invoke(
      root: (work / "content"),
      arguments: [
        "cf",
        "anchored.img",
        "-F",
        "fat12",
        "-C",
        ".",
        "@host.mtree"
      ]
    )
    assert_equal(
      "host bytes\n".b,
      invoke(
        root: work,
        arguments: [
          "xOf",
          "content/anchored.img",
          "/sub/file.txt"
        ]
      )
        .stdout
    )
    # The path limit bounds image paths only: a limit shorter than the
    # worker's tag for the content directory still admits /x.
    (work / "content/short.mtree").write("./x type=file fsimg.content=zero size=1\n")
    invoke(
      root: work,
      arguments: [
        "c",
        "-n",
        "-F",
        "fat12",
        "-l",
        "path=4",
        "-C",
        "content",
        "@short.mtree"
      ]
    )
    (work / "@odd").mkdir
    (work / "@odd/inside").binwrite("odd\n".b)
    invoke(root: work, arguments: ["cf", "odd.img", "-F", "fat12", "./@odd"])
    assert_equal(["\"/@odd\"", "\"/@odd/inside\""], listing(root: work, image: "odd.img"))
  end
end
