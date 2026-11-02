# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "bindata"

module FsimgSpec
  module Iso9660
    # Fixed records from the field tables in the ISO format design. Layouts
    # represent bytes only: invalid values remain representable for mutations.
    module Layout
      class Descriptor < BinData::Record
        uint8 :descriptor_type
        string :identifier, length: 5
        uint8 :descriptor_version
        uint8 :volume_flags
        string :system_identifier, length: 32
        string :volume_identifier, length: 32
        string :reserved_a, length: 8
        uint32le :space_le
        uint32be :space_be
        string :escapes, length: 32
        uint16le :set_size_le
        uint16be :set_size_be
        uint16le :sequence_le
        uint16be :sequence_be
        uint16le :block_bytes_le
        uint16be :block_bytes_be
        uint32le :path_bytes_le
        uint32be :path_bytes_be
        uint32le :path_l
        uint32le :path_l_copy
        uint32be :path_m
        uint32be :path_m_copy
        string :root_record, length: 34
        string :identifiers, length: 623
        string :dates, length: 68
        uint8 :structure_version
        string :tail, length: 1166
      end

      class DirectoryHeader < BinData::Record
        uint8 :record_bytes
        uint8 :attribute_blocks
        uint32le :extent_le
        uint32be :extent_be
        uint32le :data_bytes_le
        uint32be :data_bytes_be
        string :date, length: 7
        uint8 :flags
        uint8 :unit_bytes
        uint8 :interleave_gap
        uint16le :sequence_le
        uint16be :sequence_be
        uint8 :identifier_bytes
      end

      class SuspHeader < BinData::Record
        string :signature, length: 2
        uint8 :entry_bytes
        uint8 :entry_version
      end

      class BootValidation < BinData::Record
        uint8 :header_id
        uint8 :platform
        uint16le :reserved
        string :identifier, length: 24
        uint16le :checksum
        string :validation_key, length: 2
      end

      class BootEntry < BinData::Record
        uint8 :indicator
        uint8 :media
        uint16le :load_segment
        uint8 :system_type
        uint8 :reserved
        uint16le :sector_count
        uint32le :image_block
        string :criteria, length: 20
      end

      class BootSection < BinData::Record
        uint8 :header_id
        uint8 :platform
        uint16le :entry_count
        string :identifier, length: 28
      end

      RECORDS = [Descriptor, DirectoryHeader, SuspHeader, BootValidation, BootEntry, BootSection].freeze
      # Generate immutable plain value classes from the layout field tables.
      # Generated members support normal methods and pattern matching.
      VALUES = RECORDS.to_h { [_1, Data.define(*_1.fields.field_names)] }.freeze

      def self.decode(record, bytes)
        fields = record.read(bytes).snapshot.transform_values { _1.is_a?(String) ? _1.b.freeze : _1 }
        VALUES.fetch(record).new(**fields)
      end
    end
  end
end
