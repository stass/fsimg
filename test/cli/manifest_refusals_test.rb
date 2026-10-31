# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_manifest_refusals_preserve_outputs_and_charge_descriptors
    root = @root
    # A manifest operand must name an existing regular file, never followed,
    # and a bare @ is a syntax error. A manifest counts once against fds, and
    # its content directory, given by -C, once more. -n checks a generated
    # digest, which needs no payload, but not a host file's, which would.
    work = (root / "manifest-refused")
    generated_manifest(directory: work, name: "gen.mtree")
    (work / "directory").mkdir
    File.symlink("gen.mtree", (work / "link.mtree"))
    File.mkfifo(work / "fifo.mtree")
    [
      ["@link.mtree", "manifest is a symbolic link".b],
      ["@missing.mtree", "cannot open manifest".b],
      ["@directory", "manifest is not a regular file".b],
      ["@fifo.mtree", "manifest is not a regular file".b]
    ].each do |operand, detail|
      result = invoke(
        root: work,
        arguments: ["cf", "refused.img", "-F", "fat12", operand],
        expected: 1
      )
      assert_includes(result.stderr, detail, [operand, result.stderr])
      error = failed(
        invoke(
          root: work,
          arguments: ["cf", "refused.img", "-F", "fat12", "--json", operand],
          expected: 1
        )
      )
      assert_equal(
        ((operand == "@missing.mtree") ? "I/O failure" : "unsupported capability"),
        error.fetch("status"),
        [operand, error]
      )
    end

    result = invoke(root: work, arguments: ["cf", "refused.img", "-F", "fat12", "@"], expected: 2)
    assert_includes(result.stderr, "@ requires a manifest name".b, "result stderr")
    result = invoke(
      root: work,
      arguments: ["cf", "refused.img", "-F", "fat12", "-l", "fds=5", "-C", ".", "@gen.mtree"],
      expected: 1
    )
    assert_includes(result.stderr, "fds limit 5".b, result.stderr)
    refute((work / "refused.img").exist?, "a refused manifest leaves no output image")
    invoke(root: work, arguments: ["cf", "fds.img", "-F", "fat12", "-l", "fds=5", "unrelated.txt"])
    invoke(
      root: work,
      arguments: [
        "cf",
        "fds-manifest.img",
        "-F",
        "fat12",
        "-l",
        "fds=5",
        "@gen.mtree"
      ]
    )
    wrong = ("0" * 64)
    (work / "wrong.mtree").write("./gen type=file fsimg.content=seeded fsimg.seed=7 size=100 sha256digest=#{wrong}\n")
    error = failed(
      invoke(
        root: work,
        arguments: ["cn", "-F", "fat12", "--json", "@wrong.mtree"],
        expected: 1
      )
    )
    assert(error.fetch("message").start_with?("manifest line 1: "), error)
    (work / "unrelated.mtree").write("./unrelated.txt type=file sha256digest=#{wrong}\n")
    plan = completed(
      invoke(
        root: work,
        arguments: [
          "cn",
          "-F",
          "fat12",
          "--json",
          "-C",
          ".",
          "@unrelated.mtree"
        ]
      )
    )
    assert_equal("1", plan.find { |record| record.fetch("type") == "plan" }.fetch("entries"))
    result = invoke(
      root: work,
      arguments: ["cf", "wrong-host.img", "-F", "fat12", "-C", ".", "@unrelated.mtree"],
      expected: 1
    )
    assert_includes(result.stderr, "do not match the manifest's sha256digest".b, result.stderr)
    refute((work / "wrong-host.img").exist?, "a host digest mismatch leaves no output image")
  end
end
