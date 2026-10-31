# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_generated_manifests_are_reproducible_and_streamable
    root = @root
    # An @file operand is an mtree manifest opened relative to the -C in
    # effect. Generated files need no content directory, so a working
    # directory holding other files is never a source; reordered lines
    # produce the same reproducible image; after -- an @ word is still a
    # manifest; and a manifest image streams to stdout and compresses like
    # any other.
    work = (root / "manifest-generated")
    seeded = generated_manifest(directory: work, name: "gen.mtree")
    created = completed(
      invoke(
        root: work,
        arguments: [
          "cf",
          "gen.img",
          "-F",
          "fat12",
          "--json",
          "-R",
          "@gen.mtree"
        ]
      )
    )
    assert_equal("3", created[-1].fetch("entries_processed"))
    assert_equal(["\"/sub\"", "\"/sub/gen\"", "\"/zero\""], listing(root: work, image: "gen.img"))
    assert_equal(seeded, invoke(root: work, arguments: ["xOf", "gen.img", "/sub/gen"]).stdout)
    assert_equal(("\0".b * 5000), invoke(root: work, arguments: ["xOf", "gen.img", "/zero"]).stdout)
    invoke(
      root: work,
      arguments: [
        "cf",
        "reversed.img",
        "-F",
        "fat12",
        "-R",
        "@reversed-gen.mtree"
      ]
    )
    same_bytes(first: (work / "gen.img"), second: (work / "reversed.img"))
    invoke(
      arguments: [
        "cf",
        "dashed.img",
        "-F",
        "fat12",
        "-R",
        "-C",
        "manifest-generated",
        "--",
        "@gen.mtree"
      ]
    )
    same_bytes(first: (work / "gen.img"), second: (root / "dashed.img"))
    streamed = invoke(
      root: work,
      arguments: [
        "cf",
        "-",
        "-z",
        "-F",
        "fat12",
        "-R",
        "@gen.mtree"
      ]
    )
      .stdout
    assert_equal((work / "gen.img").binread, gunzip(streamed))
  end
end
