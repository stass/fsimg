# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The names a FAT directory holds as fsimg's reader takes them
    # (specs/design-fat-reader.md#names-and-metadata): which names it
    # accepts, and the key it compares names by, its simple upper case
    # without normalization. Names arrive as UTF-8, binary or not, since a
    # known tree holds the binary paths a walk gives.
    module Names
      # A long name holds at most 255 UTF-16 units (section 7).
      MAX_UNITS = 255
      # The characters no name holds: the C0 controls, DEL, and the
      # characters section 6.1 excludes from short names and section 7
      # does not readmit for long names.
      FORBIDDEN = %r{[\u0000-\u001F\u007F"*/:<>?\\|]}

      # Why fsimg's reader refuses name, or nil when it accepts it.
      def self.problem(name)
        text = utf8(name)
        if !text.valid_encoding? then "is not UTF-8"
        elsif text.empty? then "is empty"
        elsif [".", ".."].include?(text) then "names . or .."
        elsif FORBIDDEN.match?(text) then "holds a forbidden character"
        elsif text.start_with?(" ") then "starts with a space"
        elsif text.end_with?(" ", ".") then "ends with a space or a period"
        elsif text.encode(Encoding::UTF_16LE).bytesize / 2 > MAX_UNITS then "is longer than #{MAX_UNITS} UTF-16 units"
        end
      end

      # The key fsimg compares name by, or nil where Ruby cannot give it.
      # Ruby's String#upcase is the full mapping; it agrees with the simple
      # mapping for every character it maps to one character, and keeps a
      # caseless one. A character whose full upper case is several
      # characters, such as U+00DF, may have a simple mapping of its own,
      # so its key is unknown. So is that of a character Ruby's Unicode
      # version leaves unassigned, which fsimg's later one may case-map.
      # The generators draw only characters both versions map alike.
      def self.key(name)
        text = utf8(name)
        return nil if !text.valid_encoding? || text.match?(/\p{Cn}/)

        characters = text.each_char.map(&:upcase)
        characters.join if characters.all? { _1.length == 1 }
      end

      private_class_method def self.utf8(name) = name.dup.force_encoding(Encoding::UTF_8)
    end
  end
end
