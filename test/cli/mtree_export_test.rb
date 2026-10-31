# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_mtree_export_publishes_reproducible_metadata_after_extraction
    root = @root
    # x --mtree publishes a manifest of the extracted selection only after a
    # complete extraction: a failed one leaves no file, and an existing name is
    # refused before any destination exists. With -k the skipped link and
    # FIFO are recorded with their metadata, and each file with the digest of
    # the bytes written; the tree and its manifest re-create the image byte
    # for byte; two extractions write the same manifest; it is published
    # with mode 0600, rejected with -O, and holds one descriptor, which the
    # worker receives and the extractor writes through.
    work = (root / "mtree-export")
    work.mkdir
    (work / "source.mtree").write(
      "/set uid=0 gid=0 mode=0644 time=1500000000\n./sp\\040ace type=dir mode=0750 uid=7 gid=8\n./sp\\040ace/f\\043 type=file fsimg.content=seeded fsimg.seed=3 size=70000 fsimg.holes=100:5000\n./link type=link link=sp\\040ace\n./fifo type=fifo mode=0620\n"
    )
    invoke(root: work, arguments: ["cf", "q.img", "-F", "qnx6", "-R", "@source.mtree"])
    error = failed(
      invoke(
        root: work,
        arguments: ["xf", "q.img", "--json", "--mtree=failed.mtree"],
        expected: 1
      )
    )
    assert_equal("unsupported capability", error.fetch("status"), error)
    assert_equal("incomplete", error.fetch("output_state"), error)
    refute((work / "failed.mtree").exist?, "a metadata failure leaves no published manifest")
    FileUtils.remove_entry(work / "q")
    invoke(root: work, arguments: ["xkf", "q.img", "--mtree=q.mtree"])
    text = (work / "q.mtree").read
    assert(text.start_with?("#mtree v2.0\n. type=dir\n"), text)
    assert_equal(0o600, ((work / "q.mtree").stat.mode & 0o777))
    written = (work / "q/sp ace/f#").binread
    assert_equal(70000, written.size, "written size")
    assert_equal("\0".b * 5000, written[100...5100], "written 100 5100")
    assert_includes(text, "./sp\\040ace/f\\043 type=file mode=0644 uid=0 gid=0 time=315532800.000000000 size=70000 sha256digest=#{written.then { Digest::SHA256.hexdigest(_1) }}\n", text)
    assert_includes(text, "./sp\\040ace type=dir mode=0750 uid=7 gid=8 ", text)
    assert_includes(text, "./link type=link mode=0644 uid=0 gid=0 time=315532800.000000000 link=sp\\040ace\n", text)
    assert_includes(text, "./fifo type=fifo mode=0620 uid=0 gid=0 time=315532800.000000000\n", text)
    other = private_directory(root: work, name: "other")
    result = invoke(
      root: work,
      arguments: ["xkf", "q.img", "-C", "other", "--mtree=q.mtree"],
      expected: 1
    )
    assert_includes(result.stderr, "cannot create --mtree file".b, result.stderr)
    assert_empty(other.children.to_a, "other children to a empty")
    invoke(root: work, arguments: ["xkf", "q.img", "-C", "other", "--mtree=again.mtree"])
    assert_equal((work / "q.mtree").binread, (work / "again.mtree").binread)
    invoke(
      root: work,
      arguments: [
        "cf",
        "rebuilt.img",
        "-F",
        "qnx6",
        "-R",
        "-C",
        "q",
        "@../q.mtree"
      ]
    )
    same_bytes(first: (work / "q.img"), second: (work / "rebuilt.img"))
    result = invoke(
      root: work,
      arguments: ["xOf", "q.img", "--mtree=o.mtree", "/link"],
      expected: 2
    )
    assert_includes(result.stderr, "-O rejects".b, result.stderr)
    result = invoke(root: work, arguments: ["xkf", "q.img", "--mtree="], expected: 2)
    assert_includes(result.stderr, "--mtree requires a file name".b, result.stderr)
    # Names refused before anything is extracted: a dangling symbolic link
    # is never followed, the destination's own new name is taken, and a
    # file inside an -E destination would make it non-empty.
    File.symlink("nowhere", (work / "dangling.mtree"))
    names = private_directory(root: work, name: "names")
    [
      [["-C", "names", "--mtree=dangling.mtree"], "cannot create --mtree file"],
      [["-C", "names", "--mtree=names/q"], "cannot create --mtree file"],
      [
        [
          "-E",
          "-C",
          "names",
          "--mtree=names/inside.mtree"
        ],
        "extraction destination is not empty"
      ]
    ].each do |arguments, message|
      error = failed(
        invoke(
          root: work,
          arguments: ["xkf", "q.img", "--json", *arguments],
          expected: 1
        )
      )
      assert_equal(message, error.fetch("message"), [arguments, error])
      assert(error.fetch("output_state") != "incomplete", [arguments, error])
      assert_empty(names.children.to_a, arguments)
      refute((work / "nowhere").exist?, "a refused manifest parent is not created")
    end

    fewest = -> (*arguments) do
      (3...16).each do |count|
        target = private_directory(root: work, name: "fds-#{count}-#{arguments.size}")
        result = invoke(
          root: work,
          arguments: [
            "xkf",
            "q.img",
            "--json",
            "-C",
            target.basename.to_s,
            "-l",
            "fds=#{count}",
            *arguments
          ],
          expected: nil
        )
        if result.status == 0
          return count
        end

        assert_equal("fds", failed(result).fetch("resource"), result)
        refute((work / "fds.mtree").exist?, "a descriptor limit failure leaves no published manifest")
      end

      raise CliTest::Error, arguments.inspect
    end

    plain = fewest.call
    assert_equal((plain + 1), fewest.call("--mtree=fds.mtree"))
    assert_empty(work.glob(".fsimg-*").to_a, "work glob fsimg to a empty")
  end
end
