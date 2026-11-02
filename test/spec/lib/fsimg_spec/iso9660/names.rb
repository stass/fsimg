# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # One displayed-name derivation serves traversal and namespace rules.
    # Unpaired UTF-16 units become WTF-8, preserving their spelling so the
    # portable-name profile can omit them rather than invent a name.
    module Names
      module_function

      def decode(record, view:)
        entries = view == "rockridge" ? record.entries.select { _1.fields.signature == "NM" } : []
        return entries.map { _1.raw.byteslice(5, [_1.raw.bytesize - 5, 0].max) }.join.b if entries.any?
        return Model::UNKNOWN if FsimgSpec.undefined?(record.identifier)

        bytes = view == "joliet" ? utf16(record.identifier) : record.identifier
        FsimgSpec.undefined?(bytes) ? bytes : bytes.delete_suffix(";1".b)
      end

      def utf16(bytes)
        return Model::UNKNOWN if bytes.bytesize.odd?

        bytes.unpack("n*").slice_when { |a, b| !(high?(a) && low?(b)) }.map do |units|
          codepoint = units.size == 2 ? 0x10000 + ((units.first - 0xd800) << 10) + units.last - 0xdc00 : units.first
          [codepoint].pack("U").b
        end.join.b
      end

      def native?(name, view:)
        !FsimgSpec.undefined?(name) && !name.empty? && !name.include?("\0".b) && !name.include?("/".b) &&
          ![".".b, "..".b].include?(name) && (view == "rockridge" || !name.end_with?(";1".b))
      end

      def portable?(name) = name.dup.force_encoding(Encoding::UTF_8).valid_encoding? &&
        name.bytes.none? { _1 < 0x20 || _1 == 0x7f }

      def high?(unit) = unit.between?(0xd800, 0xdbff)
      def low?(unit) = unit.between?(0xdc00, 0xdfff)
      private_class_method :high?, :low?
    end
  end
end
