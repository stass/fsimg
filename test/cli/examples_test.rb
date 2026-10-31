# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_examples_create_read_extract_and_report_failures
    executable = @executable
    root = @root
    programs = executable.parent
    output = (root / "example-output")
    scratch = (root / "example-scratch")
    output.mkdir(0o700)
    scratch.mkdir(0o700)
    invoke(
      executable: (programs / "example-create-tree"),
      root: root,
      arguments: ["source", output.to_s, "tree.img", scratch.to_s]
    )
    completed(invoke(arguments: ["W", "--json", "-d", "-f", (output / "tree.img").to_s]))
    invoke(
      executable: (programs / "example-provider"),
      root: root,
      arguments: [output.to_s, "provider.img", scratch.to_s]
    )
    image = (output / "provider.img")
    sampled = invoke(
      executable: (programs / "example-read-slice"),
      root: root,
      arguments: [image.to_s, "0", image.stat.size.to_s, scratch.to_s]
    )
    assert_includes(sampled.stdout, "sampled_bytes=29".b, "sampled stdout")
    read_fd, write_fd = IO.pipe
    read_fd.close
    begin
      broken = run_process(
        [
          (programs / "example-read-slice").to_s,
          image.to_s,
          "0",
          image.stat.size.to_s,
          scratch.to_s
        ],
        cwd: root,
        stdout: write_fd,
        stderr: :capture,
        ignore_pipe: true,
        timeout: 10
      )
      assert_equal(1, broken.status, ["example ignored stdout failure", broken.status])
    ensure
      write_fd.close
    end

    invoke(
      executable: (programs / "example-read-slice"),
      root: root,
      arguments: [image.to_s, "0", (image.stat.size + 1).to_s, scratch.to_s],
      expected: 1
    )
    invoke(
      executable: (programs / "example-extract"),
      root: root,
      arguments: [image.to_s, output.to_s, "extracted", scratch.to_s]
    )
    assert_equal("hello from a source provider\n".b, (output / "extracted/provider.txt").binread)
    invoke(
      executable: (programs / "example-extract"),
      root: root,
      arguments: [image.to_s, output.to_s, "extracted", scratch.to_s],
      expected: 1
    )
    invoke(
      executable: (programs / "example-provider"),
      root: root,
      arguments: [output.to_s, "provider.img", scratch.to_s],
      expected: 1
    )
    assert_empty(scratch.children.to_a, "scratch children to a empty")
  end
end
