# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "bindata"

module FsimgSpec
  module Erofs
    # Fixed records from the field tables of specs/design-erofs.md. Layouts
    # represent bytes only: invalid values stay representable for mutations.
    module Layout
      # The base superblock, at volume byte 1024 (design: The superblock).
      class Superblock < BinData::Record
        endian :little
        uint32 :magic
        uint32 :checksum
        uint32 :compat
        uint8 :blkszbits
        uint8 :extension_slots
        uint16 :root_nid
        uint64 :inode_count
        int64 :epoch
        uint32 :fixed_nsec
        uint32 :blocks
        uint32 :meta_blkaddr
        uint32 :xattr_blkaddr
        string :uuid, length: 16
        string :volume_name, length: 16
        uint32 :incompat
        uint16 :algorithm_mask
        uint16 :extra_devices
        uint16 :device_slot
        uint8 :dirblkbits
        uint8 :prefix_count
        uint32 :prefix_start
        uint64 :packed_nid
        uint8 :filter_reserved
        uint8 :ishare_prefix
        uint16 :reserved_a
        uint32 :build_time
        uint64 :wide_root_nid
        uint64 :reserved_b
      end

      # The 32-byte inode (design: Compact inode).
      class CompactInode < BinData::Record
        endian :little
        uint16 :format
        uint16 :xattr_count
        uint16 :mode
        uint16 :nlink
        uint32 :size_bytes
        uint32 :time
        uint32 :layout_word
        uint32 :serial
        uint16 :owner
        uint16 :group
        uint32 :reserved
      end

      # The 64-byte inode (design: Extended inode).
      class ExtendedInode < BinData::Record
        endian :little
        uint16 :format
        uint16 :xattr_count
        uint16 :mode
        uint16 :reserved_a
        uint64 :size_bytes
        uint32 :layout_word
        uint32 :serial
        uint32 :owner
        uint32 :group
        int64 :time
        uint32 :nsec
        uint32 :nlink
        string :reserved_b, length: 16
      end

      # One 12-byte directory entry (design: Blocks).
      class DirectoryEntry < BinData::Record
        endian :little
        uint64 :nid
        uint16 :name_offset
        uint8 :file_type
        uint8 :reserved
      end

      # The 12-byte header of an inline attribute region.
      class AttributeHeader < BinData::Record
        endian :little
        uint32 :name_filter
        uint8 :shared_count
        string :reserved, length: 7
      end

      # The 4-byte head of one attribute entry, before its name and value.
      class AttributeEntry < BinData::Record
        endian :little
        uint8 :suffix_length
        uint8 :name_index
        uint16 :value_length
      end

      RECORDS = [Superblock, CompactInode, ExtendedInode, DirectoryEntry, AttributeHeader, AttributeEntry].freeze
      # Immutable values generated from the layouts' field tables, so
      # decoded records support methods and pattern matching.
      VALUES = RECORDS.to_h { [_1, Data.define(*_1.fields.field_names)] }.freeze

      def self.decode(record, bytes)
        fields = record.read(bytes).snapshot.transform_values { _1.is_a?(String) ? _1.b.freeze : _1 }
        VALUES.fetch(record).new(**fields)
      end
    end

    # CRC-32C (Castagnoli), reflected, as the superblock checksum uses it:
    # the register is seeded with all ones and stored without the final
    # inversion (design: Superblock checksum).
    module Crc32c
      TABLE = Array.new(256) do |byte|
        8.times.reduce(byte) { |crc, _| crc.odd? ? (crc >> 1) ^ 0x82F63B78 : crc >> 1 }
      end.freeze

      def self.register(bytes) = bytes.each_byte.reduce(0xFFFFFFFF) { |crc, byte| (crc >> 8) ^ TABLE[(crc ^ byte) & 0xFF] }

      # The value the superblock stores for bytes.
      def self.stored(bytes) = register(bytes)
    end
  end
end
