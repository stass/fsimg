# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # One resolved volume as its own decoder reads it (design: Volumes): the
    # volume superblock, the volume object map and that map's tree, the
    # filesystem tree whose virtual nodes the map resolves, and the records
    # of the tree's leaves. A part the volume cannot reach is Undefined with
    # the reason. slot is the volume array index that names the volume.
    class Volume < Data.define(:resolution, :slot, :superblock, :object_map, :object_map_tree, :tree, :records)
      MAGIC = 0x42535041
      CASE_INSENSITIVE = 0x1
      NORMALIZATION_INSENSITIVE = 0x8
      SEALED = 0x20

      def self.decode(checkpoint, resolution, bound:)
        superblock = resolution.object.body(Layout::VolumeSuperblock)
        object_map, map_tree = object_map_parts(checkpoint, superblock, bound)
        tree = filesystem_tree(checkpoint, superblock, map_tree, bound)
        new(resolution:, slot: resolution.slot(checkpoint.superblock), superblock:, object_map:, object_map_tree: map_tree, tree:,
            records: FsimgSpec.undefined?(tree) ? tree : Records.decode(tree))
      end

      # The volume object map, a physical object at the superblock's
      # address, and its tree.
      def self.object_map_parts(checkpoint, superblock, bound)
        object = checkpoint.reader.object(superblock.object_map_oid)
        valid = object&.valid?(kind: :object_map, storage: PHYSICAL, oid: object.address, xid: checkpoint.xid)
        return [object || Undefined.new(reason: "the volume object map lies past the image"), Undefined.new(reason: "no valid volume object map")] unless valid

        [object, BTree.walk(BTree::VOLUME_OBJECT_MAP, object.body(Layout::ObjectMap).tree_oid, checkpoint:, bound:)]
      end

      # The filesystem tree from the superblock's root OID. A sealed
      # volume's headerless nodes need the sealed profile, which this model
      # does not decode.
      def self.filesystem_tree(checkpoint, superblock, map_tree, bound)
        return map_tree if FsimgSpec.undefined?(map_tree)
        return Undefined.new(reason: "the sealed profile is not modelled") if superblock.incompatible_features.anybits?(SEALED)

        hashed = superblock.incompatible_features.anybits?(CASE_INSENSITIVE | NORMALIZATION_INSENSITIVE)
        BTree.walk(BTree.filesystem(hashed:), superblock.root_tree_oid, checkpoint:, bound:, object_map: map_tree)
      end

      private_class_method :object_map_parts, :filesystem_tree

      def sealed? = superblock.incompatible_features.anybits?(SEALED)
      def hashed? = superblock.incompatible_features.anybits?(CASE_INSENSITIVE | NORMALIZATION_INSENSITIVE)
      def case_insensitive? = superblock.incompatible_features.anybits?(CASE_INSENSITIVE)
      def object_map_fields = object_map.body(Layout::ObjectMap)
      def map_decoded? = !FsimgSpec.undefined?(object_map_tree)
      def tree_decoded? = !FsimgSpec.undefined?(tree)

      # The volume name: the field's bytes up to its first NUL, or nil when
      # the field holds none.
      def name
        field = superblock.name
        terminator = field.index("\0")
        terminator && field.byteslice(0, terminator)
      end
    end
  end
end
