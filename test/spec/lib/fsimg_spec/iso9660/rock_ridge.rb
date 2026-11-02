# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # Decoded link content is metadata. Keep framing, semantic flags and
    # byte count distinct, so a profile refusal cannot hide corruption in
    # a later component. Retained target bytes stop one beyond the reader's
    # limit, while every component is still framed and counted.
    module RockRidge
      Target = Data.define(:bytes, :length, :framed, :supported, :components)
      Component = Data.define(:flags, :bytes)
      KINDS = { 0o100000 => "file", 0o040000 => "directory", 0o120000 => "symlink", 0o010000 => "fifo" }.freeze
      module_function

      def extension_shape?(raw) = raw.bytesize >= 8 &&
        8 + raw.getbyte(4) + raw.getbyte(5) + raw.getbyte(6) <= raw.bytesize

      def kind(record)
        px = record.entries.find { _1.fields.signature == "PX" }
        return record.fields.flags & 2 != 0 ? "directory" : "file" unless px && [36, 44].include?(px.raw.bytesize)

        KINDS.fetch(px.raw.byteslice(4, 4).unpack1("V") & 0o170000, "unsupported")
      end

      def target(record)
        entries = record.entries.select { _1.fields.signature == "SL" }
        components = []
        bytes = +"".b
        length = 0
        framed = !entries.empty?
        supported = true
        has_component = joining = after_root = false
        entries.each_with_index do |entry, index|
          raw = entry.raw
          previous = index.positive? ? entries[index - 1].raw : nil
          if raw.bytesize < 5 || (previous && (previous.bytesize < 5 || previous.getbyte(4) & 1 == 0))
            framed = false
            next
          end
          offset = 5
          while offset < raw.bytesize
            if offset + 2 > raw.bytesize || offset + 2 + raw.getbyte(offset + 1) > raw.bytesize
              framed = false
              break
            end
            flags, count = raw.byteslice(offset, 2).unpack("CC")
            text = raw.byteslice(offset + 2, count).freeze
            components << Component.new(flags:, bytes: text)
            special = flags & 0x3e
            supported = false if special & 0x30 != 0
            addition = +"".b
            addition << "/" if has_component && !joining && !after_root
            addition << case special
                        in 2 then ".".b
                        in 4 then "..".b
                        in 8 then "/".b
                        in 16 | 32 then "".b
                        else text
                        end
            length += addition.bytesize
            bytes << addition.byteslice(0, [1025 - bytes.bytesize, 0].max)
            has_component = true
            joining = flags & 1 != 0
            after_root = special == 8
            offset += count + 2
          end
        end
        framed = false if entries.empty? || entries.last.raw.bytesize < 5 || entries.last.raw.getbyte(4) & 1 != 0 ||
          joining || (length.zero? && supported)
        Target.new(bytes: bytes.freeze, length:, framed:, supported:, components: components.freeze)
      end

      def components_valid?(target) = target.components.all? do |component|
        special = component.flags & 0x3e
        special & (special - 1) == 0 && !component.bytes.include?("\0".b)
      end
    end
  end
end
