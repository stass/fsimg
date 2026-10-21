# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"

# A creation case's manifest holds its tree in the subset fsimg's import
# reads (specs/design-mtree.md#grammar): one full-form line per entry,
# vis-encoded names, generated content and its digest.
class TestManifest < Minitest::Test
  # Only bytes from ! to ~ stand for themselves; # would start a comment
  # and a backslash an escape, and a space ends a word.
  def test_names_are_vis_encoded
    assert_equal "a\\040b\\043c\\\\d\\303\\251!~".b, FsimgSpec::Manifest.encode("a b#c\\d\u00e9!~")
  end

  def test_each_entry_is_one_full_line_of_plain_words
    file = FsimgSpec::TreeEntry.file("d d/f", size: 9, content: "positional", seed: 4, holes: [[1, 2], [5, 1]])
    lines = FsimgSpec::Manifest.lines([FsimgSpec::TreeEntry.directory("d d"), file,
                                       FsimgSpec::TreeEntry.file("z", size: 3, content: "zero")])

    assert_equal ["./d\\040d type=dir\n",
                  "./d\\040d/f type=file size=9 fsimg.content=positional fsimg.seed=4 fsimg.holes=1:2,5:1 " \
                  "sha256digest=#{FsimgSpec::FileContent.sha256(file)}\n",
                  "./z type=file size=3 fsimg.content=zero sha256digest=#{Digest::SHA256.hexdigest("\0" * 3)}\n"],
                 lines
    assert(lines.all? { _1.chomp.split(" ").all? { |word| word.match?(/\A[!-~]+\z/) } })
  end
end
