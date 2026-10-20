# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # What the harness's programs print to a terminal. It depends on nothing,
  # so bin/report, which loads none of the rest of the harness, shares it
  # with bin/replay.
  module Terminal
    # Backslashes and the control, format and separator characters, such as
    # a right-to-left override, that could rewrite or disguise a line.
    UNSAFE = /[\\[:cntrl:]\p{Cf}\p{Zl}\p{Zp}]/

    # text read as UTF-8 with any invalid byte replaced and every UNSAFE
    # character escaped as \u{HEX}.
    def self.escaped(text) =
      text.to_s.dup.force_encoding(Encoding::UTF_8).scrub.gsub(UNSAFE) { format("\\u{%x}", _1.ord) }
  end
end
