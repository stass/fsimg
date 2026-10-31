# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_gzip_creation_preserves_headers_payload_and_descriptor_limits
    root = @root
    # c -z, and -a with a .gz name, write one gzip member around any stack:
    # gzip -t accepts it, its header is fsimg's (no name, the -R epoch as
    # MTIME, XFL by level, OS 3), it decodes to the image written without
    # gzip, and it reads back as that image. -f - streams the same bytes, cW
    # verifies the member before publishing, -n reports gzip, its bound and
    # zlib, and gzip:level outside 1 to 9 is refused before anything is
    # written.
    stacks = [
      ["fat12", "gz-bare.img"],
      ["fat12/gpt", "gz-disk.img"],
      ["fat12/gpt/dmg", "gz-disk.dmg"]
    ]
    stacks.each do |stack, name|
      invoke(arguments: ["cf", name, "-R", "-F", stack, "-C", "source", "."])
      invoke(arguments: ["cf", (name + ".gz"), "-R", "-a", "-F", stack, "-C", "source", "."])
      packed = (root / (name + ".gz")).binread
      assert_equal(
        (("\x1f\x8b\b\x00".b + [315532800].pack("L<")) + "\x00\x03".b),
        packed[0...10],
        packed[0...10]
      )
      assert_equal((root / name).binread, gunzip(packed), stack)
      streamed = invoke(arguments: ["cf", "-", "-z", "-R", "-F", stack, "-C", "source", "."]).stdout
      assert_equal(packed, streamed, stack)
      listed = invoke(arguments: ["tf", (name + ".gz")]).stdout
      assert_includes(listed, "/HELLO.TXT".b, listed)
      (root / name).unlink
      (root / (name + ".gz")).unlink
    end

    [["1", 4], ["9", 2]].each do |level, xfl|
      invoke(
        arguments: [
          "cf",
          "level.gz",
          "-z",
          "-R",
          "-F",
          "fat12",
          "-o",
          "gzip:level=#{level}",
          "-C",
          "source",
          "."
        ]
      )
      assert_equal(xfl, (root / "level.gz").binread.getbyte(8))
      (root / "level.gz").unlink
    end

    result = invoke(
      arguments: [
        "cf",
        "level.gz",
        "-z",
        "-F",
        "fat12",
        "-o",
        "gzip:level=10",
        "-C",
        "source",
        "."
      ],
      expected: 1
    )
    assert_includes(result.stderr, "gzip:level".b, "result stderr")
    refute((root / "level.gz").exist?, "an invalid compression level leaves no output image")
    invoke(arguments: ["cWf", "checked.img.gz", "-a", "-F", "fat12/gpt", "-C", "source", "."])
    assert_equal("U\xaa".b, gunzip((root / "checked.img.gz").binread)[510...512])
    (root / "checked.img.gz").unlink
    plan = records(
      invoke(
        arguments: [
          "c",
          "-n",
          "-z",
          "--json",
          "-R",
          "-F",
          "fat12/dmg",
          "-C",
          "source",
          "."
        ]
      )
    )
    stack = plan.find { |record| record.fetch("type") == "stack" }
    planned = plan.find { |record| record.fetch("type") == "plan" }
    assert_equal("gzip", stack.fetch("compression"), "stack compression")
    assert(planned.fetch("codec").start_with?("zlib "), "planned codec")
    assert(Integer(planned.fetch("output_bytes"), 10) > Integer(planned.fetch("volume_bytes"), 10), "Integer planned output bytes 10")
    # An encoded image's worker holds the raw intermediate beside its
    # staging directory, so the coordinator counts both against fds and
    # refuses, naming no source path, a limit the worker would exceed. The
    # operand is one directory of files, which the count covers; a deeper
    # tree holds one more descriptor per level while it is scanned or copied.
    [["fat12", "fds.img.gz"], ["fat12/gpt/dmg", "fds.dmg"]].each do |stack, name|
      compress = (name.end_with?(".gz") ? ["-z"] : [])
      error = failed(
        invoke(
          arguments: [
            "cf",
            name,
            *compress,
            "--json",
            "-F",
            stack,
            "-l",
            "fds=5",
            "-C",
            "source",
            "nested"
          ],
          expected: 1
        )
      )
      assert_equal("", error.fetch("path"), error)
      assert_equal("fds", error.fetch("resource"), error)
      assert_equal(["5", "6"], [error.fetch("limit"), error.fetch("observed")], error)
      invoke(
        arguments: [
          "cf",
          name,
          *compress,
          "-F",
          stack,
          "-l",
          "fds=6",
          "-C",
          "source",
          "nested"
        ]
      )
      (root / name).unlink
    end
    # Planning writes no raw intermediate and stages nothing.
    invoke(
      arguments: [
        "c",
        "-n",
        "-z",
        "-F",
        "fat12/gpt/dmg",
        "-l",
        "fds=4",
        "-C",
        "source",
        "nested"
      ]
    )
    # The worker charges every descriptor it holds from the moment it holds
    # it, so scanning nested empty directories counts the raw intermediate
    # and every root not yet scanned: the compressed image needs one
    # descriptor more than the plain one, whatever the operand order.
    FileUtils.mkdir_p(root / "levels/deep/a/b/c")
    (root / "levels/file").binwrite("x".b)
    fewest = -> (*arguments, name:) do
      (4...16).each do |count|
        result = invoke(
          arguments: [
            "cf",
            name,
            "-F",
            "fat12",
            "-l",
            "fds=#{count}",
            *arguments
          ],
          expected: nil
        )
        if result.status == 0
          (root / name).unlink
          return count
        end

        assert_equal("fds", failed(result).fetch("resource"), result)
      end

      raise CliTest::Error, arguments.inspect
    end

    plain = fewest.call("--json", "-C", "levels", "deep", name: "levels.img")
    assert_equal(
      (plain + 1),
      fewest.call(
        "--json",
        "-z",
        "-C",
        "levels",
        "deep",
        name: "levels.img.gz"
      )
    )
    assert_equal(
      (plain + 1),
      fewest.call(
        "--json",
        "-C",
        "levels",
        "deep",
        "file",
        name: "levels.img"
      )
    )
    assert_equal(
      (plain + 1),
      fewest.call(
        "--json",
        "-C",
        "levels",
        "file",
        "deep",
        name: "levels.img"
      )
    )
  end
end
