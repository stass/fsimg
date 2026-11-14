# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "bindata"

module FsimgSpec
  module Apfs
    # Fixed records of the container level, from the field tables of
    # specs/design-apfs.md. Layouts represent bytes only: invalid values
    # stay representable for mutations. Every object record starts at
    # object byte BODY, after the common header, and names its fields from
    # the design's tables; offsets in the tables are from the object start.
    module Layout
      # The 32-byte header every object starts with (design: Object header
      # and checksum).
      class ObjectHeader < BinData::Record
        endian :little
        uint64 :checksum
        uint64 :oid
        uint64 :xid
        uint32 :object_type
        uint32 :subtype
      end

      BODY = ObjectHeader.new.num_bytes

      # The container superblock from offset 32 (design: Container
      # superblock).
      class ContainerSuperblock < BinData::Record
        endian :little
        uint32 :magic
        uint32 :block_size
        uint64 :block_count
        uint64 :optional_features
        uint64 :readonly_features
        uint64 :incompatible_features
        string :uuid, length: 16
        uint64 :next_oid
        uint64 :next_xid
        uint32 :descriptor_blocks
        uint32 :data_blocks
        uint64 :descriptor_base
        uint64 :data_base
        uint32 :descriptor_next
        uint32 :data_next
        uint32 :descriptor_index
        uint32 :descriptor_length
        uint32 :data_index
        uint32 :data_length
        uint64 :space_manager_oid
        uint64 :object_map_oid
        uint64 :reaper_oid
        uint32 :test_type
        uint32 :max_volumes
        array :volume_oids, type: :uint64, initial_length: 100
        array :counters, type: :uint64, initial_length: 32
        string :blocked_out_range, length: 16
        uint64 :eviction_tree_oid
        uint64 :flags
        uint64 :efi_jumpstart
        string :fusion_uuid, length: 16
        uint64 :keylocker_start
        uint64 :keylocker_count
        array :ephemeral_info, type: :uint64, initial_length: 4
        uint64 :test_oid
        string :fusion_trees, length: 32
        uint64 :newest_mounted_version
        uint64 :media_keylocker_start
        uint64 :media_keylocker_count
      end

      # A checkpoint-mapping block's counts, from offset 32 (design:
      # Ephemeral objects and the space manager).
      class CheckpointMapHead < BinData::Record
        endian :little
        uint32 :flags
        uint32 :entry_count
      end

      # One 40-byte mapping, the first at offset 40 of its block.
      class CheckpointMapping < BinData::Record
        endian :little
        uint32 :object_type
        uint32 :subtype
        uint32 :byte_size
        uint32 :reserved
        uint64 :volume_oid
        uint64 :oid
        uint64 :address
      end

      # One 48-byte device entry of the space manager, at offset 48 (main)
      # and 96 (tier 2).
      class SpaceDevice < BinData::Record
        endian :little
        uint64 :block_count
        uint64 :chunk_count
        uint32 :cib_count
        uint32 :cab_count
        uint64 :free_count
        uint32 :address_offset
        uint32 :reserved
        uint64 :reserved_b
      end

      # One 40-byte free-queue entry, from offset 200 (design: Free queues).
      class FreeQueueEntry < BinData::Record
        endian :little
        uint64 :queued
        uint64 :tree_oid
        uint64 :oldest_xid
        uint16 :node_limit
        string :reserved, length: 14
      end

      # The space manager from offset 32 to the end of its structure, 2520
      # (design: Ephemeral objects and the space manager).
      class SpaceManager < BinData::Record
        endian :little
        uint32 :block_size
        uint32 :blocks_per_chunk
        uint32 :chunks_per_cib
        uint32 :cibs_per_cab
        space_device :main
        space_device :tier2
        uint32 :flags
        uint32 :pool_tx_multiplier
        uint64 :pool_block_count
        uint32 :pool_bitmap_size
        uint32 :pool_bitmap_blocks
        uint64 :pool_bitmap_base
        uint64 :pool_base
        uint64 :reserve_block_count
        uint64 :reserve_alloc_count
        array :queues, type: :free_queue_entry, initial_length: 3
        uint16 :pool_free_head
        uint16 :pool_free_tail
        uint32 :pool_xid_offset
        uint32 :pool_index_offset
        uint32 :pool_free_next_offset
        uint32 :version
        uint32 :structure_size
        string :data_zones, length: 2176
      end

      # The index and count at offset 32 of a chunk-information block (kind
      # 7) or a chunk-information address block (kind 6).
      class ChunkBlockHead < BinData::Record
        endian :little
        uint32 :block_index
        uint32 :entry_count
      end

      # One 32-byte chunk-information record, the first at offset 40.
      class ChunkInfo < BinData::Record
        endian :little
        uint64 :xid
        uint64 :first_block
        uint32 :block_count
        uint32 :free_count
        uint64 :bitmap_block
      end

      # The object map from offset 32 (design: Object maps).
      class ObjectMap < BinData::Record
        endian :little
        uint32 :flags
        uint32 :snapshot_count
        uint32 :tree_type
        uint32 :snapshot_tree_type
        uint64 :tree_oid
        uint64 :snapshot_tree_oid
        uint64 :latest_snapshot_xid
        uint64 :revert_min
        uint64 :revert_max
      end

      # An object-map key and value, 16 bytes each.
      class ObjectMapKey < BinData::Record
        endian :little
        uint64 :oid
        uint64 :xid
      end

      class ObjectMapValue < BinData::Record
        endian :little
        uint32 :flags
        uint32 :byte_size
        uint64 :address
      end

      # A free-queue key: the XID that freed the run and its first block.
      class FreeQueueKey < BinData::Record
        endian :little
        uint64 :xid
        uint64 :first_block
      end

      # A B-tree node's fields from offset 32 to its table of contents at 56
      # (design: Node layout).
      class NodeHead < BinData::Record
        endian :little
        uint16 :flags
        uint16 :level
        uint32 :key_count
        uint16 :toc_offset
        uint16 :toc_length
        uint16 :free_offset
        uint16 :free_length
        uint16 :key_free_offset
        uint16 :key_free_length
        uint16 :value_free_offset
        uint16 :value_free_length
      end

      # The 40-byte information footer that ends a root node.
      class NodeFooter < BinData::Record
        endian :little
        uint32 :tree_flags
        uint32 :node_size
        uint32 :key_size
        uint32 :value_size
        uint32 :longest_key
        uint32 :longest_value
        uint64 :key_count
        uint64 :node_count
      end

      # A table-of-contents entry of a fixed-size tree, and of any other.
      class FixedEntry < BinData::Record
        endian :little
        uint16 :key_offset
        uint16 :value_offset
      end

      class VariableEntry < BinData::Record
        endian :little
        uint16 :key_offset
        uint16 :key_length
        uint16 :value_offset
        uint16 :value_length
      end

      # The reaper from offset 32 to its state buffer at 112 (design:
      # Reaper).
      class Reaper < BinData::Record
        endian :little
        uint64 :next_reap_id
        uint64 :completed_reap_id
        uint64 :head
        uint64 :tail
        uint32 :flags
        uint32 :list_count
        uint32 :reaped_type
        uint32 :reaped_size
        uint64 :reaped_volume_oid
        uint64 :reaped_oid
        uint64 :reaped_xid
        uint32 :entry_flags
        uint32 :state_size
      end

      # One reap-list object from offset 32 to its entries at 64.
      class ReapList < BinData::Record
        endian :little
        uint64 :next_oid
        uint32 :flags
        uint32 :capacity
        uint32 :entry_count
        uint32 :first_index
        uint32 :last_index
        uint32 :free_index
      end

      class ReapListEntry < BinData::Record
        endian :little
        uint32 :next_index
        uint32 :flags
        uint32 :object_type
        uint32 :byte_size
        uint64 :volume_oid
        uint64 :oid
        uint64 :xid
      end

      # The records that start at object byte BODY, and those that are
      # read at an offset their container gives.
      OBJECT_BODIES = [ContainerSuperblock, CheckpointMapHead, SpaceManager, ChunkBlockHead, ObjectMap, NodeHead,
                       Reaper, ReapList].freeze
      RECORDS = [ObjectHeader, *OBJECT_BODIES, CheckpointMapping, SpaceDevice, FreeQueueEntry, ChunkInfo,
                 ObjectMapKey, ObjectMapValue, FreeQueueKey, NodeFooter, FixedEntry, VariableEntry,
                 ReapListEntry].freeze

      # Immutable values generated from each layout's field table, one
      # member per field, so decoded records support methods and pattern
      # matching. Nested records and arrays decode to Hashes and Arrays.
      VALUES = RECORDS.to_h { [_1, Data.define(*_1.new.field_names)] }.freeze

      # The record decoded from bytes at offset at, or nil when the bytes
      # end before it does.
      def self.find(record, bytes, at: 0)
        size = record.new.num_bytes
        return nil if bytes.nil? || at.negative? || at + size > bytes.bytesize

        fields = record.read(bytes.byteslice(at, size)).snapshot.transform_values { _1.is_a?(String) ? _1.b.freeze : _1 }
        VALUES.fetch(record).new(**fields)
      end

      def self.size(record) = record.new.num_bytes

      # The unpack directive of each integer field type the layouts use.
      DIRECTIVES = { BinData::Uint64le => "Q<", BinData::Uint32le => "V", BinData::Uint16le => "v", BinData::Uint8 => "C" }.freeze

      # The unpack directive of a flat record, generated from its field
      # table, so that a table of thousands of records decodes in one call:
      # the chunk records of a large container number in the millions.
      def self.directive(record)
        sample = record.new
        sample.field_names.map { |name| sample[name].then { _1.is_a?(BinData::String) ? "a#{_1.num_bytes}" : DIRECTIVES.fetch(_1.class) } }.join
      end

      # The count records of a flat record from offset at, as many as the
      # bytes hold.
      def self.records(record, bytes, at:, count:)
        size = size(record)
        held = [[(bytes.bytesize - at) / size, 0].max, count].min
        names = record.new.field_names
        bytes.byteslice(at, size * held).to_s.unpack(directive(record) * held).each_slice(names.size)
             .map { VALUES.fetch(record).new(**names.zip(_1).to_h) }
      end
    end

    # Fletcher-64 as APFS checksums an object: over its bytes from offset 8
    # read as little-endian 32-bit words, both sums modulo 2^32 - 1
    # (design: Object header and checksum). s2 adds s1 after every word, so
    # word i of n contributes (n - i) times to it.
    module Fletcher64
      MODULUS = 0xFFFFFFFF

      module_function

      def compute(object)
        words = object.byteslice(8..).unpack("V*")
        s1 = words.sum % MODULUS
        s2 = words.each_with_index.sum { |word, i| (words.size - i) * word } % MODULUS
        c1 = MODULUS - ((s1 + s2) % MODULUS)
        c2 = MODULUS - ((s1 + c1) % MODULUS)
        c1 | (c2 << 32)
      end

      def valid?(object) = object.bytesize >= 16 && compute(object) == object.unpack1("Q<")

      # The object with its checksum recomputed from the bytes it covers.
      def seal(object) = [compute(object)].pack("Q<") + object.byteslice(8..)
    end
  end
end
