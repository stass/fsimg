# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

module FsimgSpec
  # A mutation that does not fit its recipe or its image.
  class MutationError < Error; end

  # The byte-level mutations of specs/design-spec-testing.md#mutations. Each
  # refuses an offset or value that does not fit its target and never wraps;
  # none consults the rules, so any invalid encoding can be written.
  module Mutation
    INTEGER_FORMATS = { "set_u8" => "C", "set_u16le" => "v", "set_u32le" => "V" }.freeze
    INTEGER_WIDTHS = { "set_u8" => 1, "set_u16le" => 2, "set_u32le" => 4 }.freeze

    # Writes one integer: a byte, or a little-endian 16- or 32-bit value.
    class SetInteger < Data.define(:op, :offset, :value)
      include Checked

      def initialize(op:, offset:, value:)
        raise MutationError, "unknown op #{FsimgSpec.shown(op)}" unless INTEGER_FORMATS.key?(op)
        raise MutationError, "#{op} offset #{offset} is negative" if offset.negative?
        raise MutationError, "#{op} value #{value} does not fit #{INTEGER_WIDTHS.fetch(op)} bytes" unless
          value.between?(0, (1 << (8 * INTEGER_WIDTHS.fetch(op))) - 1)

        super
      end

      def apply(image) = image.patch(offset, [value].pack(INTEGER_FORMATS.fetch(op)))

      # The end of the byte range the mutation writes.
      def extent = offset + INTEGER_WIDTHS.fetch(op)

      def to_h = { op:, offset:, value: }
    end

    # Overwrites a byte range; a recipe gives the bytes in lowercase hex.
    class ReplaceBytes < Data.define(:offset, :bytes)
      include Checked

      def initialize(offset:, bytes:)
        raise MutationError, "replace_bytes offset #{offset} is negative" if offset.negative?
        raise MutationError, "replace_bytes writes no bytes" if bytes.empty?

        super(offset:, bytes: bytes.b.freeze)
      end

      def apply(image) = image.patch(offset, bytes)

      def extent = offset + bytes.bytesize

      def to_h = { op: "replace_bytes", offset:, bytes: bytes.unpack1("H*") }
    end

    # Shortens the image; it never lengthens one.
    class Truncate < Data.define(:length)
      include Checked

      def initialize(length:)
        raise MutationError, "truncate length #{length} is negative" if length.negative?

        super
      end

      def apply(image) = image.truncate(length)

      def extent = length

      def to_h = { op: "truncate", length: }
    end

    # Decodes one recipe mutation. It refuses an unknown op, a missing,
    # extra or mistyped field, and a mutation reaching beyond ceiling bytes.
    def self.load(fields, ceiling:)
      mutation =
        case fields
        in { op: "set_u8" | "set_u16le" | "set_u32le" => op, offset: Integer => offset, value: Integer => value, **nil }
          SetInteger.new(op:, offset:, value:)
        in { op: "replace_bytes", offset: Integer => offset, bytes: /\A(?:[0-9a-f]{2})+\z/ => hex, **nil }
          ReplaceBytes.new(offset:, bytes: [hex].pack("H*"))
        in { op: "truncate", length: Integer => length, **nil }
          Truncate.new(length:)
        else
          raise MutationError, "unknown or malformed mutation #{FsimgSpec.shown(fields)}"
        end
      raise MutationError, "#{FsimgSpec.shown(mutation.to_h)} reaches beyond #{ceiling} bytes" if mutation.extent > ceiling

      mutation
    end
  end
end
