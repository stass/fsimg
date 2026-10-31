# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_mtree_failures_remove_manifest_staging
    root = @root
    # However x --mtree fails, the failure is reported, no manifest is
    # published and its staging is gone: a host write of the manifest refused
    # by the file size limit, which the extracted files fit and the manifest
    # outgrows, and a work limit reached while the image is still being
    # read, before anything is written.
    work = (root / "mtree-failures")
    work.mkdir
    # Long names exceed the manifest's 64 KiB output buffer with fewer
    # files to create and sync under sanitizers.
    name = ("n" * 240)
    (work / "many.mtree").write(
      (0...193)
        .map { |index|
          "./#{name}#{format("%03d", index)} type=file fsimg.content=seeded fsimg.seed=#{index} size=1\n"
        }
        .join("")
    )
    invoke(root: work, arguments: ["cf", "many.img", "-F", "qnx6", "-R", "@many.mtree"])
    private_directory(root: work, name: "sized")
    error = failed(
      invoke(
        root: work,
        arguments: ["xf", "many.img", "--json", "-C", "sized", "--mtree=sized.mtree"],
        expected: 1,
        file_size_limit: 32768
      )
    )
    assert_equal(Errno::EFBIG::Errno, error.fetch("os_error"), error)
    assert_equal("incomplete", error.fetch("output_state"), error)
    assert_equal("", error.fetch("retained_manifest"), error)
    private_directory(root: work, name: "worked")
    error = failed(
      invoke(
        root: work,
        arguments: [
          "xf",
          "many.img",
          "--json",
          "-C",
          "worked",
          "-l",
          "work=100",
          "--mtree=worked.mtree"
        ],
        expected: 1
      )
    )
    assert_equal("work", error.fetch("resource"), error)
    assert_equal("", error.fetch("retained_manifest"), error)
    refute((work / "sized.mtree").exist?, "a manifest write failure leaves no published manifest")
    refute((work / "worked.mtree").exist?, "a work limit failure leaves no published manifest")
    assert_empty(work.glob(".fsimg-*").to_a, "work glob fsimg to a empty")
  end
end
