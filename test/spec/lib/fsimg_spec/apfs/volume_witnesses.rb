# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Targeted witnesses of the volume and record rules, in the shape of
    # Witnesses: field mutations resealed into the object they change.
    # Volume rules mutate the empty volume of Witnesses::BASE; record rules
    # mutate RECORD_BASE, whose one-leaf filesystem tree holds directories,
    # files with one extent each, their data-stream ID records and hashed
    # directory records of a normalization-insensitive volume. Offsets come
    # from the base's model: an entry's key_at and value_at.
    module VolumeWitnesses
      # E4's prefix capture: names p, pirhv, q and qodfh in /e4, and the
      # .fseventsd directory with three files of one block each.
      RECORD_BASE = "apfs-newfs-33554432-e4-prefix-cs"
      KIND_SHIFT = Records::KIND_SHIFT
      # U+0378, a code point no Unicode version has assigned.
      UNASSIGNED = "\xCD\xB8".b.freeze
      # The inode-value offsets the witnesses write (design: Inodes), and
      # the first extended-field descriptor after the 4-byte blob head.
      INODE_FIXED = 92
      FIRST_DESCRIPTOR = INODE_FIXED + 4

      module_function

      def changes(base)
        case base.name
        when Witnesses::BASE then [superblock(base), object_map(base)].reduce(:merge)
        when RECORD_BASE then [keys(base), inodes(base), entries(base), extents(base), reconciliation(base)].reduce(:merge)
        else {}
        end
      end

      def volume(base) = base.model.filesystems.first
      def superblock_at(base) = volume(base).resolution.object.address
      def leaf(base) = volume(base).tree.nodes.first

      # The mutations writing fields at offsets of the filesystem tree's
      # one leaf, resealed.
      def in_leaf(base, *fields) = Witnesses.resealed(base, leaf(base).object.address, *fields)

      # The leaf entry whose key satisfies the block, and its table index.
      def find_entry(base, &) = leaf(base).entries.each_with_index.find { |entry, _| yield(entry.key) }

      def inode(base, number) = find_entry(base) { _1.kind == Records::INODE && _1.owner == number }.first
      def directory_record(base, name) = find_entry(base) { _1.directory_record? && _1.name == name }.first
      def extent(base) = find_entry(base) { _1.kind == Records::EXTENT }.first
      def stream_id(base) = find_entry(base) { _1.kind == Records::STREAM_ID }

      def superblock(base)
        sb = volume(base).superblock
        apsb = ->(*fields) { Witnesses.resealed(base, superblock_at(base), *fields) }
        {
          "apfs.apsb.magic" => { "apsa" => apsb.call([32, :u32, Volume::MAGIC ^ 1]) },
          "apfs.apsb.slot" => { "second_slot" => apsb.call([36, :u32, 1]) },
          "apfs.apsb.uuid" => { "zero" => apsb.call([240, :bytes, ("\0" * VolumeRules::UUID_BYTES).b]) },
          "apfs.apsb.name" => { "invalid_utf8" => apsb.call([704, :bytes, "\xFF\xFE\0".b]) },
          "apfs.apsb.tree_types" => { "physical_root" => apsb.call([116, :u32, VolumeRules::PHYSICAL_ROOT]) },
          "apfs.apsb.transition" => { "root_to" => apsb.call([968, :u64, 5]) },
          "apfs.apsb.incompatible" => { "incomplete_restore" => apsb.call([56, :u64, sb.incompatible_features | 0x10]) },
          "apfs.apsb.flags" => { "encrypted" => apsb.call([264, :u64, sb.flags & ~VolumeRules::UNENCRYPTED]) },
          "apfs.apsb.counts" => { "one_more_file" => apsb.call([184, :u64, sb.file_count + 1]) }
        }
      end

      def object_map(base)
        map = volume(base).superblock.object_map_oid
        root = volume(base).object_map_tree.nodes.first
        value = root.entries.first.value_at
        omap = ->(*fields) { Witnesses.resealed(base, map, *fields) }
        node = ->(*fields) { Witnesses.resealed(base, root.object.address, *fields) }
        fs_root = leaf(base)
        {
          "apfs.vomap.object" => { "zero_xid" => omap.call([16, :u64, 0]) },
          "apfs.vomap.manual" => { "manually_managed" => omap.call([32, :u32, Rules::MANUALLY_MANAGED]) },
          "apfs.vomap.flags" => { "encrypting" => omap.call([32, :u32, 0x2]) },
          "apfs.vomap.tree_type" => { "ephemeral" => omap.call([40, :u32, EPHEMERAL | KINDS.fetch(:root_node)]) },
          "apfs.vomap.value_range" => { "past_end" => node.call([value + 8, :u64, base.block_count + 10]) },
          "apfs.vomap.value_flags" => { "no_header" => node.call([value, :u32, Rules::NO_HEADER_VALUE]) },
          "apfs.vomap.value_encrypted" => { "encrypted" => node.call([value, :u32, Rules::ENCRYPTED_VALUE]) },
          "apfs.vomap.resolution" => {
            "older_root" => Witnesses.resealed(base, fs_root.object.address, [16, :u64, fs_root.mapping.first.xid - 1])
          }
        }
      end

      def keys(base)
        _, index = stream_id(base)
        value_length = BTree::NODE_DATA + leaf(base).head.toc_offset + Layout.size(Layout::VariableEntry) * index + 6
        {
          "apfs.fs.kind" => { "kind_fourteen" => in_leaf(base, [inode(base, 24).key_at, :u64, (14 << KIND_SHIFT) | 24]) },
          "apfs.fs.crypto_state" => {
            "inode_as_crypto_state" => in_leaf(base, [inode(base, 24).key_at, :u64, (RecordRules::CRYPTO_STATE << KIND_SHIFT) | 24])
          },
          "apfs.fs.lengths" => { "long_stream_value" => in_leaf(base, [value_length, :u16, 8]) }
        }
      end

      def inodes(base)
        root = inode(base, 2)
        file = inode(base, 19)
        fields = Layout.find(Layout::Inode, file.value)
        at = ->(record, offset) { record.value_at + offset }
        name_flags = file.value.getbyte(FIRST_DESCRIPTOR + 1)
        {
          "apfs.inode.reserved" => { "child_five" => in_leaf(base, [directory_record(base, "p").value_at, :u64, 5]) },
          "apfs.inode.root" => { "root_parent" => in_leaf(base, [at.call(root, 0), :u64, 7]) },
          "apfs.inode.mode" => { "type_fifteen" => in_leaf(base, [at.call(file, 80), :u16, (fields.mode & 0o7777) | 0o170000]) },
          "apfs.inode.flags" => { "unused_flag" => in_leaf(base, [at.call(file, 48), :u64, fields.internal_flags | 0x20]) },
          "apfs.inode.dataless" => { "dataless" => in_leaf(base, [at.call(file, 68), :u32, RecordRules::DATALESS]) },
          "apfs.xfield.blob" => {
            "used_past_end" => in_leaf(base, [at.call(file, INODE_FIXED + 2), :u16, file.value.unpack1("v", offset: INODE_FIXED + 2) + 8])
          },
          "apfs.xfield.unknown" => {
            "data_dependent" => in_leaf(base, [at.call(file, FIRST_DESCRIPTOR), :u8, 200],
                                        [at.call(file, FIRST_DESCRIPTOR + 1), :u8, name_flags | RecordRules::DATA_DEPENDENT])
          },
          "apfs.inode.children" => { "root_one_more" => in_leaf(base, [at.call(root, 56), :u32, 3]) },
          "apfs.inode.links" => { "two_links" => in_leaf(base, [at.call(file, 56), :u32, 2]) },
          "apfs.inode.parent" => { "private_parent" => in_leaf(base, [at.call(file, 0), :u64, 3]) }
        }
      end

      def entries(base)
        record = directory_record(base, "pirhv")
        word = record.key.bytes.unpack1("V", offset: 8)
        incompatible = volume(base).superblock.incompatible_features
        {
          "apfs.drec.variant" => {
            "plain_volume" => Witnesses.resealed(base, superblock_at(base), [56, :u64, incompatible & ~Volume::NORMALIZATION_INSENSITIVE])
          },
          "apfs.drec.name" => { "slash" => in_leaf(base, [record.key_at + 13, :u8, "/".ord]) },
          "apfs.drec.hash" => { "hash_bit" => in_leaf(base, [record.key_at + 8, :u32, word ^ (1 << Records::HASHED_LENGTH_BITS)]) },
          "apfs.drec.hash_unassigned" => { "unassigned" => in_leaf(base, [record.key_at + 13, :bytes, UNASSIGNED]) },
          "apfs.drec.type" => { "directory_type" => in_leaf(base, [record.value_at + 16, :u16, RecordRules::DIRECTORY_TYPE]) },
          "apfs.drec.target" => { "missing_inode" => in_leaf(base, [record.value_at, :u64, 999_999]) }
        }
      end

      def extents(base)
        record = extent(base)
        length = Layout.find(Layout::FileExtent, record.value).length_and_flags
        {
          "apfs.extent.tiling" => { "gap_at_start" => in_leaf(base, [record.key_at + 8, :u64, base.block_size]) },
          "apfs.extent.range" => { "past_end" => in_leaf(base, [record.value_at + 8, :u64, base.block_count]) },
          "apfs.extent.flags" => { "flagged" => in_leaf(base, [record.value_at, :u64, length | (1 << RecordRules::EXTENT_FLAGS_SHIFT)]) },
          "apfs.extent.crypto" => { "crypto_id" => in_leaf(base, [record.value_at + 16, :u64, 5]) }
        }
      end

      def reconciliation(base)
        record, = stream_id(base)
        {
          "apfs.dstream.references" => { "two_users" => in_leaf(base, [record.value_at, :u32, 2]) },
          "apfs.extent.owner" => { "orphan" => in_leaf(base, [extent(base).key_at, :u64, (Records::EXTENT << KIND_SHIFT) | 999_999]) }
        }
      end
    end
  end
end
