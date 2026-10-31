# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_create_operands_follow_each_change_directory
    exfat_image
    root = @root
    # Each operand is stored under its own path, opened relative to the -C
    # before it; -C may appear among the operands, -Cdir is one word, and
    # after -- a word spelled -C is an operand.
    invoke(arguments: ["cf", "nested.img", "-F", "fat12", "-Csource", "nested"])
    assert_equal(["\"/nested\"", "\"/nested/caf\u00e9.txt\""], listing(image: "nested.img"))
    (root / "-C").mkdir
    (root / "-C/dash.txt").binwrite("dash\n".b)
    invoke(arguments: ["cf", "dash.img", "-F", "fat12", "--", "-C"])
    assert_equal(["\"/-C\"", "\"/-C/dash.txt\""], listing(image: "dash.img"))
    [["merge-a", "x"], ["merge-b", "y"]].each do |name, member|
      FileUtils.mkdir_p((root / name) / "sub")
      (((root / name) / "sub") / member).binwrite(member.encode(Encoding::UTF_8).b)
    end

    invoke(
      arguments: [
        "cf",
        "disjoint.img",
        "-F",
        "fat12",
        "-C",
        "merge-a",
        "sub",
        "-C",
        "source",
        "nested"
      ]
    )
    assert_equal(
      [
        "\"/nested\"",
        "\"/nested/caf\u00e9.txt\"",
        "\"/sub\"",
        "\"/sub/x\""
      ],
      listing(image: "disjoint.img").sort
    )
    # A trailing "/" or "/." would resolve a link before O_NOFOLLOW applies.
    File.symlink("source", (root / "source-link"))
    ["source-link", "source-link/", "source-link/.", "source-link//./"].each do |operand|
      result = invoke(arguments: ["cf", "linked.img", "-F", "fat12", operand], expected: 1)
      assert_includes(result.stderr, "operand is a symbolic link".b, [operand, result.stderr])
    end

    refute((root / "linked.img").exist?, "refused link operands leave no image")
    # A refused kind is unsupported, as the scanner reports it, not an I/O
    # failure.
    File.mkfifo(root / "operand-fifo")
    # A socket address holds a short path, so the socket is bound by its
    # path relative to the working directory, which holds root.
    listener = Socket.new(:UNIX, :STREAM)
    listener.bind(Socket.sockaddr_un(relative_path(root / "operand-socket")))
    ["source-link", "source-link//./", "operand-fifo", "operand-socket"].each do |operand|
      error = failed(
        invoke(
          arguments: [
            "cf",
            "kind.img",
            "-F",
            "fat12",
            "--json",
            operand
          ],
          expected: 1
        )
      )
      assert_equal("unsupported capability", error.fetch("status"), error)
      assert_equal(operand, error.fetch("path"), error)
    end

    refute((root / "kind.img").exist?, "unsupported operand kinds leave no image")
    # So is an image that is a link or a FIFO.
    File.symlink("disjoint.img", (root / "image-link"))
    ["image-link", "operand-fifo", "operand-socket"].each do |image|
      error = failed(invoke(arguments: ["tf", image, "--json"], expected: 1))
      assert_equal("unsupported capability", error.fetch("status"), error)
      assert_equal(image, error.fetch("path"), error)
    end

    (root / "image-link").unlink
    (root / "operand-fifo").unlink
    listener.close
    (root / "operand-socket").unlink
    # A trailing slash requires a directory, as it does for the host.
    result = invoke(
      arguments: [
        "cf",
        "slashed.img",
        "-F",
        "fat12",
        "-C",
        "source",
        "HELLO.TXT/"
      ],
      expected: 1
    )
    assert_includes(result.stderr, "Not a directory".b, "trailing slash requires a directory operand")
    refute((root / "slashed.img").exist?, "refused trailing slash leaves no image")
    verified = completed(
      invoke(
        arguments: [
          "cWf",
          "verified.img",
          "-F",
          "exfat",
          "--json",
          "-R",
          "-C",
          "source",
          "."
        ]
      )
    )
    assert_equal("cW", verified[-1].fetch("command"), "creation summary names verification mode")
    assert_equal(true, verified[-1].fetch("published"), "verified creation publishes its image")
    report = verified.find { |record| record.fetch("type") == "verify" }
    assert_equal(true, report.fetch("complete"), "creation verification is complete")
    same_bytes(first: (root / "exfat.img"), second: (root / "verified.img"))
    invoke(
      arguments: [
        "c",
        "-W",
        "-d",
        "-R",
        "-F",
        "exfat",
        "-f",
        "verified-data.img",
        "-C",
        "source",
        "."
      ]
    )
    same_bytes(first: (root / "exfat.img"), second: (root / "verified-data.img"))
  end

  # Staged cW verification failure is covered by test/staged-verify.c.
end
