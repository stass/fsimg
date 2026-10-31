# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_extract_here_requires_an_empty_private_directory
    human_image
    root = @root
    # -E extracts into the -C directory itself, which must be empty and
    # private: owned by the user and not writable by group or others. A
    # symbolic link is not followed, and it or any other existing object that
    # is not a directory is refused as unsupported.
    private_directory(root: root, name: "into-empty")
    invoke(arguments: ["xEf", "human.img", "-C", "into-empty"])
    same_bytes(first: (root / "source/nested/caf\u00e9.txt"), second: (root / "into-empty/nested/caf\u00e9.txt"))
    private_directory(root: root, name: "into-readable", mode: 0o755)
    invoke(arguments: ["xEf", "human.img", "-C", "into-readable"])
    assert_equal("hello\n".b, (root / "into-readable/HELLO.TXT").binread)
    (private_directory(root: root, name: "into-dotted") / ".keep").binwrite("".b)
    private_directory(root: root, name: "into-group", mode: 0o770)
    private_directory(root: root, name: "into-other", mode: 0o707)
    [
      ["into-dotted", "extraction destination is not empty"],
      ["into-group", "extraction destination is writable by group or others"],
      ["into-other", "extraction destination is writable by group or others"]
    ].each do |name, detail|
      result = invoke(arguments: ["xEf", "human.img", "-C", name], expected: 1)
      assert_includes(result.stderr, "#{detail} \"#{name}\"".encode(Encoding::UTF_8).b, [name, result.stderr])
    end

    assert_equal([".keep"], Dir.children(root / "into-dotted"))
    assert_empty((root / "into-group").children.to_a, "root into group children to a empty")
    assert_empty((root / "into-other").children.to_a, "root into other children to a empty")
    private_directory(root: root, name: "into-target")
    File.symlink("into-target", (root / "into-link"))
    result = invoke(arguments: ["xEf", "human.img", "-C", "into-link"], expected: 1)
    assert_includes(result.stderr, "-C directory is a symbolic link \"into-link\"".b, result.stderr)
    ["into-link/", "into-link/."].each do |spelling|
      result = invoke(arguments: ["xEf", "human.img", "-C", spelling], expected: 1)
      assert_includes(result.stderr, "-C directory is a symbolic link".b, [spelling, result.stderr])
    end

    error = failed(
      invoke(
        arguments: [
          "xEf",
          "human.img",
          "--json",
          "-C",
          "into-link"
        ],
        expected: 1
      )
    )
    assert_equal("unsupported capability", error.fetch("status"), error)
    assert_empty((root / "into-target").children.to_a, "root into target children to a empty")
    # So is any other existing object that is not a directory, however it
    # is spelled; a path through one names nothing and fails to open.
    (root / "into-file").binwrite("".b)
    File.mkfifo(root / "into-fifo")
    listener = Socket.new(:UNIX, :STREAM)
    listener.bind(Socket.sockaddr_un(relative_path(root / "into-socket")))
    ["into-file", "into-file/", "into-file/.", "into-fifo", "into-socket"].each do |name|
      error = failed(invoke(arguments: ["xEf", "human.img", "--json", "-C", name], expected: 1))
      assert_equal("unsupported capability", error.fetch("status"), [name, error])
      assert_equal("-C directory is not a directory", error.fetch("message"), [name, error])
      assert_equal(name, error.fetch("path"), [name, error])
    end

    error = failed(
      invoke(
        arguments: [
          "xEf",
          "human.img",
          "--json",
          "-C",
          "into-file/inner"
        ],
        expected: 1
      )
    )
    assert_equal("I/O failure", error.fetch("status"), error)
    # A link looping earlier in the path is no link at -C, only a failed open.
    File.symlink("into-loop", (root / "into-loop"))
    error = failed(
      invoke(
        arguments: [
          "xEf",
          "human.img",
          "--json",
          "-C",
          "into-loop/inner"
        ],
        expected: 1
      )
    )
    assert_equal("I/O failure", error.fetch("status"), error)
    assert_equal("cannot open -C directory", error.fetch("message"), error)
    assert_equal(Errno::ELOOP::Errno, error.fetch("os_error"), error)
    (root / "into-loop").unlink
    listener.close
    ["into-file", "into-fifo", "into-socket"].each do |name|
      (root / name).unlink
    end

    result = invoke(arguments: ["xEf", "human.img"], expected: 1)
    assert_includes(result.stderr, "extraction destination is not empty \".\"".b, "result stderr")
  end
end
