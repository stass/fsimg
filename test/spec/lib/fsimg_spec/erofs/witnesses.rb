# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Erofs
    # Explicit mutations state the broken contract, not an expected result.
    # Rule classification derives the status and the earliest stage that
    # must find it. Mutations that change a superblock field use an image
    # without a checksum, so that the stale checksum does not also fire.
    module Witnesses
      Change = Data.define(:parameters, :mutations, :valid)
      Witness = Data.define(:id, :parameters, :mutations, :intended, :rules)
      B = 4096
      NO_CHECKSUM = { checksum: 0 }.freeze
      SMALL_BLOCKS = { block_bits: 9 }.freeze
      ATTRIBUTES = { xattr: 1 }.freeze
      SUBDIRECTORY = { subdirectory: 1 }.freeze
      # A volume extent of four blocks ends at byte 16384; its last metadata
      # slot is where a 64-byte inode cannot fit.
      SHORT_EXTENT = 16384 / B
      LAST_SLOT = (SHORT_EXTENT * B - Builder::META - 32) / 32

      module_function

      def change(mutations:, parameters: {}, valid: false) = Change.new(parameters: parameters.freeze, mutations: mutations.freeze, valid:)
      def u8(offset, value) = Mutation::SetInteger.new(op: "set_u8", offset:, value:)
      def u16(offset, value) = Mutation::SetInteger.new(op: "set_u16le", offset:, value:)
      def u32(offset, value) = Mutation::SetInteger.new(op: "set_u32le", offset:, value:)
      def bytes(offset, value) = Mutation::ReplaceBytes.new(offset:, bytes: value)

      # Offsets of the fixed facts of the builder's image.
      def sb(offset) = 1024 + offset
      def inode(nid, offset = 0) = Builder::META + 32 * nid + offset
      def entry(base, index, offset = 0) = base + 12 * index + offset
      def root(index, offset = 0) = entry(Builder::ROOT_DATA, index, offset)
      def sub(index, offset = 0) = entry(Builder::SUB_DATA, index, offset)
      def region(offset = 0) = inode(Builder::FILE, 32 + offset)
      def entry_bytes(nid:, name_offset:, type:) = Layout::DirectoryEntry.new(nid:, name_offset:, file_type: type).to_binary_s
      def nid_bytes(nid) = [nid].pack("Q<")

      # Rule => witness name => change.
      def changes = {
        "identity" => {
          "short_prefix" => change(mutations: [Mutation::Truncate.new(length: 1027)]),
          "half_magic" => change(mutations: [u16(sb(0), 0)])
        },
        "sb.blkszbits" => { "below_minimum" => change(parameters: NO_CHECKSUM, mutations: [u8(sb(0x0C), 8)]) },
        "sb.block_size" => { "above_maximum" => change(parameters: NO_CHECKSUM, mutations: [u8(sb(0x0C), 13)]) },
        "sb.dirblkbits" => { "set" => change(parameters: NO_CHECKSUM, mutations: [u8(sb(0x5A), 1)]) },
        "sb.incompat" => { "unknown_bit" => change(parameters: NO_CHECKSUM, mutations: [u32(sb(0x50), 0x200)]) },
        "sb.devices" => { "extra_device" => change(parameters: NO_CHECKSUM, mutations: [u16(sb(0x56), 1)]) },
        "sb.checksum" => {
          "last_byte_of_range" => change(mutations: [u8(B - 1, 1)]),
          "superblock_byte" => change(mutations: [u8(sb(0x78), 1)]),
          "first_byte_past_range" => change(mutations: [u8(B, 1)], valid: true)
        },
        "sb.nanoseconds" => { "out_of_range" => change(parameters: NO_CHECKSUM, mutations: [u32(sb(0x20), 1_000_000_000)]) },
        "inode.format" => { "reserved_bit" => change(mutations: [u16(inode(Builder::FILE), 0x24)]) },
        "inode.layout" => {
          "layout_5" => change(mutations: [u16(inode(Builder::FILE), 5 << 1)]),
          "layout_7" => change(mutations: [u16(inode(Builder::FILE), 7 << 1)])
        },
        "inode.layout_profile" => {
          "compressed_full" => change(mutations: [u16(inode(Builder::FILE), 1 << 1)]),
          "compressed_compact" => change(mutations: [u16(inode(Builder::FILE), 3 << 1)])
        },
        "inode.reserved" => {
          "compact_word" => change(mutations: [u32(inode(Builder::FILE, 0x1C), 1)]),
          "extended_word" => change(mutations: [u16(inode(Builder::EXT, 0x06), 1)]),
          "extended_bytes" => change(mutations: [u8(inode(Builder::EXT, 0x30), 1)])
        },
        "inode.nanoseconds" => { "out_of_range" => change(mutations: [u32(inode(Builder::EXT, 0x28), 1_000_000_000)]) },
        "inode.mode" => { "unknown_type" => change(mutations: [u16(inode(Builder::FILE, 4), 0o030644)]) },
        "inode.body" => {
          "root_past_domain" => change(parameters: NO_CHECKSUM, mutations: [
            u32(sb(0x24), SHORT_EXTENT), u16(sb(0x0E), LAST_SLOT), u16(inode(LAST_SLOT), 1), u16(inode(LAST_SLOT, 4), Builder::DIRECTORY)
          ]),
          "attributes_past_domain" => change(mutations: [u16(inode(Builder::FILE, 2), 0xFFFF)])
        },
        "inode.tail" => {
          "crosses_block" => change(parameters: SMALL_BLOCKS, mutations: [u32(inode(Builder::FILE, 8), 480)]),
          "ends_at_block_end" => change(parameters: SMALL_BLOCKS, mutations: [u32(inode(Builder::FILE, 8), 416)], valid: true)
        },
        "inode.inline_size" => { "whole_block" => change(mutations: [u32(inode(Builder::FILE, 8), B)]) },
        "inode.data_domain" => {
          "start_block" => change(mutations: [u32(inode(Builder::EXT, 0x10), 0xFFFFFFFF)]),
          "short_extent" => change(parameters: NO_CHECKSUM, mutations: [u32(sb(0x24), 3)])
        },
        "inode.directory_size" => { "empty" => change(mutations: [u32(inode(Builder::ROOT, 8), 0)]) },
        "xattr.framing" => { "overlong_entry" => change(parameters: ATTRIBUTES, mutations: [u16(region(14), 0x100)]) },
        "xattr.index" => { "unknown_index" => change(parameters: ATTRIBUTES, mutations: [u8(region(13), 7)]) },
        "xattr.acl_suffix" => {
          "access_with_suffix" => change(parameters: ATTRIBUTES, mutations: [u8(region(13), 2)]),
          "default_with_suffix" => change(parameters: ATTRIBUTES, mutations: [u8(region(13), 3)])
        },
        "xattr.prefix" => { "missing_prefix" => change(parameters: ATTRIBUTES, mutations: [u8(region(13), 0x81)]) },
        "dir.framing" => {
          "zero_offset" => change(mutations: [u16(root(0, 8), 0)]),
          "unaligned_offset" => change(mutations: [u16(root(0, 8), 13)]),
          "offset_past_block" => change(mutations: [u16(root(0, 8), 4092)])
        },
        "dir.offsets" => { "overlapping_names" => change(mutations: [u16(root(2, 8), 49)]) },
        "dir.names" => {
          "slash" => change(mutations: [u8(Builder::ROOT_DATA + 51 + 3, "/".ord)]),
          "nul" => change(mutations: [u8(Builder::ROOT_DATA + 51 + 3, 0)])
        },
        "dir.order" => {
          "unordered" => change(mutations: [bytes(Builder::ROOT_DATA + 51, "zzz.bin")]),
          "repeated" => change(parameters: SUBDIRECTORY, mutations: [bytes(Builder::SUB_DATA + 39, "..\0\0".b)])
        },
        "dir.type" => { "unknown_type" => change(mutations: [u8(root(2, 10), 8)]) },
        "dir.type_mismatch" => { "file_as_directory" => change(mutations: [u8(root(3, 10), 2)]) },
        "dir.parent" => {
          "dotdot_names_itself" => change(parameters: SUBDIRECTORY, mutations: [bytes(sub(1), nid_bytes(Builder::SUB))]),
          "dot_names_parent" => change(parameters: SUBDIRECTORY, mutations: [bytes(sub(0), nid_bytes(Builder::ROOT))]),
          "entry_names_parent" => change(parameters: SUBDIRECTORY, mutations: [
            bytes(sub(2), entry_bytes(nid: Builder::ROOT, name_offset: 39, type: 2))
          ]),
          "entry_names_directory" => change(parameters: SUBDIRECTORY, mutations: [
            bytes(sub(2), entry_bytes(nid: Builder::SUB, name_offset: 39, type: 2))
          ])
        }
      }.freeze

      # A valid witness and the invalid witnesses of one rule. A change
      # marked valid is a second valid witness, such as the first byte past
      # a checked range.
      def for_rule(id)
        variants = changes.fetch(id.delete_prefix("erofs."))
        invalid, valid = variants.partition { !_2.valid }
        base = variants.values.first.parameters
        [Witness.new(id: "#{id}.valid", parameters: base, mutations: [], intended: nil, rules: [id]),
         *valid.map { |name, change| Witness.new(id: "#{id}.#{name}", parameters: change.parameters, mutations: change.mutations, intended: nil, rules: [id]) },
         *invalid.map { |name, change| Witness.new(id: "#{id}.#{name}", parameters: change.parameters, mutations: change.mutations, intended: id, rules: [id]) }]
      end

      # Valid images with the same parameters share one run; their coverage
      # associations still name every rule they witness. Every block size
      # and the optional features also appear on their own.
      def deterministic(registry)
        witnesses = registry.rules.flat_map { _1.domain.call }
        valid, invalid = witnesses.partition { _1.intended.nil? }
        plain, mutated = valid.partition { _1.mutations.empty? }
        combined = plain.group_by(&:parameters).sort_by { |parameters, _| parameters.to_a }.each_with_index.map do |(parameters, group), index|
          Witness.new(id: "erofs.profile.#{index}", parameters:, mutations: [], intended: nil, rules: group.flat_map(&:rules).uniq.freeze)
        end
        (combined + profiles + mutated + invalid).uniq(&:id)
      end

      # The block sizes and feature combinations the matrix lists as valid.
      def profiles = (9..12).map do |bits|
        Witness.new(id: "erofs.block_size.#{1 << bits}", parameters: { block_bits: bits, xattr: 1, subdirectory: 1 }.freeze,
                    mutations: [], intended: nil, rules: [])
      end
    end
  end
end
