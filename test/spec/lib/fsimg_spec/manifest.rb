# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

module FsimgSpec
  # A creation case's tree as the mtree(5) manifest create reads
  # (specs/design-spec-testing.md#cases), in the subset fsimg's manifest
  # import accepts (specs/design-mtree.md#grammar). Each entry is one
  # full-form line: ./ and its vis-encoded path, then every keyword it
  # needs, with no /set, no .. and no comment, so the manifest means the
  # same in any line order and its reversal is a second manifest of the
  # same tree. A directory line is type=dir; a file line names its size,
  # its generated content (TreeEntry), and the sha256digest of its bytes
  # (FileContent), which fsimg checks against its own generator before it
  # writes anything. No line names an owner, mode or time: FAT stores
  # none of them, and -R fixes every time. Every line, the last too, ends
  # with LF, which the parser requires.
  module Manifest
    NAME = "source.mtree"
    REVERSED = "source-reversed.mtree"

    # The lines of tree, binary strings in the tree's order.
    def self.lines(tree) = tree.map { line(_1) }

    def self.line(entry)
      keywords = entry.type == "directory" ? ["type=dir"] : file_keywords(entry)
      "#{["./".b + encode(entry.path), *keywords].join(" ")}\n".b
    end

    # path's bytes as the first word of a line (specs/design-mtree.md#escapes):
    # a byte from ! to ~ stands for itself, except a backslash, written \\,
    # and #, which would start a comment, written \043; every other byte,
    # space and each byte of a non-ASCII character among them, is a
    # backslash and three octal digits.
    def self.encode(path) =
      path.b.each_byte.map do |byte|
        if byte == 0x5C then "\\\\"
        elsif (0x21..0x7E).cover?(byte) && byte != 0x23 then byte.chr
        else format("\\%03o", byte)
        end
      end.join.b

    private_class_method def self.file_keywords(entry) = [
      "type=file", "size=#{entry.size}", "fsimg.content=#{entry.content}",
      *("fsimg.seed=#{entry.seed}" if entry.seed),
      *("fsimg.holes=#{entry.holes.map { _1.join(":") }.join(",")}" unless entry.holes.empty?),
      "sha256digest=#{FileContent.sha256(entry)}"
    ]
  end
end
