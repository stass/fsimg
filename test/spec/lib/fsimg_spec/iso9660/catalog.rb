# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # Decode the count-authoritative catalog grammar within its one block.
    # The facts preserve media and platform bytes; rules decide their domain.
    module Catalog
      Entry = Data.define(:fields, :platform, :initial)
      Facts = Data.define(:validation, :entries, :headers, :framed, :overflow)
      module_function

      def decode(raw)
        absent = Undefined.new(reason: "boot catalog entry is truncated")
        validation = raw.bytesize >= 32 ? Layout.decode(Layout::BootValidation, raw.byteslice(0, 32)) : absent
        entries = []
        headers = []
        return Facts.new(validation:, entries: entries.freeze, headers: headers.freeze, framed: false, overflow: false) if raw.bytesize < 64

        entries << Entry.new(fields: Layout.decode(Layout::BootEntry, raw.byteslice(32, 32)),
                             platform: validation.platform, initial: true)
        index = 2
        remaining = 0
        last = false
        platform = 0
        framed = true
        overflow = false
        if [0x90, 0x91].include?(raw.getbyte(64))
          while remaining.positive? || !last
            if index >= 64
              overflow = true
              break
            end
            piece = raw.byteslice(index * 32, 32)
            if !piece || piece.bytesize < 32
              framed = false
              break
            end
            if remaining.zero?
              header = Layout.decode(Layout::BootSection, piece)
              headers << header
              unless [0x90, 0x91].include?(header.header_id) && header.entry_count.positive?
                framed = false
                break
              end
              remaining = header.entry_count
              platform = header.platform
              last = header.header_id == 0x91
            else
              entries << Entry.new(fields: Layout.decode(Layout::BootEntry, piece), platform:, initial: false)
              remaining -= 1
            end
            index += 1
          end
        end
        Facts.new(validation:, entries: entries.freeze, headers: headers.freeze, framed:, overflow:)
      end
    end
  end
end
