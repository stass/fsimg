# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Requirements of the volume level over each resolved Volume, citing
    # the design's Volume superblock and Object maps sections. Open reads
    # the volume superblock and resolves the root tree through the volume
    # object map, so these rules have scope open, except the resolution of
    # a non-root filesystem-tree node, which directory traversal makes.
    # The volume's B-tree nodes are judged by the node rules of Rules.
    module VolumeRules
      KNOWN_INCOMPATIBLE = 0x1 | 0x2 | 0x4 | 0x8 | 0x20
      UNENCRYPTED = 0x1
      KNOWN_FLAGS = UNENCRYPTED | 0x40
      VOLUME_MAP_FLAGS = Rules::MANUALLY_MANAGED | 0x10
      VIRTUAL_ROOT = KINDS.fetch(:root_node) | VIRTUAL
      PHYSICAL_ROOT = KINDS.fetch(:root_node) | PHYSICAL
      UUID_BYTES = 16

      VOLUMES = ->(m, _) { Rules::CHECKPOINT.call(m, nil) && m.filesystems.any? }
      MAPS = ->(m, _) { VOLUMES.call(m, nil) && m.filesystems.any?(&:map_decoded?) }
      TREES = ->(m, _) { VOLUMES.call(m, nil) && m.filesystems.any?(&:tree_decoded?) }

      module_function

      def all = [*superblock_rules, *object_map_rules]

      # A rule that holds when check holds on every volume the model
      # decoded, or on every volume whose object map it decoded.
      # A sealed volume's mappings follow the sealed profile, which this
      # model does not decode, so the mapped rules skip it.
      def each_volume(id, check, policy: :rule, anchor: "volume-superblock", mapped: false)
        volumes = ->(m) { mapped ? m.filesystems.select { _1.map_decoded? && !_1.sealed? } : m.filesystems }
        Rules.method(policy).call(id:, scope: "open", anchor:, applies_to: mapped ? MAPS : VOLUMES,
                                  predicate: ->(m, _) { volumes.call(m).all? { check.call(_1, m) } })
      end

      def superblock_rules = [
        each_volume("apsb.magic", ->(v, _) { v.superblock.magic == Volume::MAGIC }),
        each_volume("apsb.slot", ->(v, _) { v.superblock.slot == v.slot }),
        each_volume("apsb.uuid", ->(v, _) { v.superblock.uuid != ("\0" * UUID_BYTES).b }),
        each_volume("apsb.name", ->(v, _) { v.name&.dup&.force_encoding(Encoding::UTF_8)&.valid_encoding? || false }),
        each_volume("apsb.tree_types", lambda do |v, _|
          sb = v.superblock
          sb.root_tree_type == VIRTUAL_ROOT && [sb.extent_tree_type, sb.snapshot_tree_type].all?(PHYSICAL_ROOT)
        end),
        each_volume("apsb.transition", lambda do |v, _|
          sb = v.superblock
          [sb.revert_xid, sb.revert_superblock_oid, sb.rolling_state_oid, sb.root_to_xid].all?(&:zero?)
        end, policy: :unsupported),
        each_volume("apsb.incompatible", ->(v, _) { (v.superblock.incompatible_features & ~KNOWN_INCOMPATIBLE).zero? }, policy: :unsupported),
        each_volume("apsb.flags", lambda do |v, _|
          flags = v.superblock.flags
          flags.anybits?(UNENCRYPTED) && (flags & ~KNOWN_FLAGS).zero?
        end, policy: :unsupported)
      ]

      def object_map_rules = [
        each_volume("vomap.object", ->(v, _) { v.map_decoded? }, anchor: "object-maps"),
        each_volume("vomap.manual", ->(v, _) { !v.object_map_fields.flags.anybits?(Rules::MANUALLY_MANAGED) }, anchor: "object-maps", mapped: true),
        each_volume("vomap.flags", ->(v, _) { (v.object_map_fields.flags & ~VOLUME_MAP_FLAGS).zero? },
                    policy: :unsupported, anchor: "object-maps", mapped: true),
        each_volume("vomap.tree_type", ->(v, _) { v.object_map_fields.tree_type == PHYSICAL_ROOT }, anchor: "object-maps", mapped: true),
        each_volume("vomap.value_range", ->(v, m) { live_values(v).all? { Rules.value_inside?(_1, m.checkpoint) } },
                    anchor: "object-maps", mapped: true),
        # Saved is the one flag an unsealed volume's mappings may carry;
        # encrypted is unsupported and judged on its own.
        each_volume("vomap.value_flags", ->(v, _) { live_values(v).all? { (_1.flags & ~(Rules::SAVED | Rules::ENCRYPTED_VALUE)).zero? } },
                    anchor: "object-maps", mapped: true),
        each_volume("vomap.value_encrypted", ->(v, _) { live_values(v).none? { _1.flags.anybits?(Rules::ENCRYPTED_VALUE) } },
                    policy: :unsupported, anchor: "object-maps", mapped: true),
        Rules.rule(id: "vomap.resolution", scope: %w[open directories], anchor: "object-maps", applies_to: TREES,
                   predicate: ->(m, _) { unresolved_scopes(m) })
      ]

      def live_values(volume) =
        volume.object_map_tree.records.map(&:last).reject { _1 == :ghost || _1.flags.anybits?(BTree::DELETED) }

      # Each filesystem-tree node resolves through the volume object map to
      # a valid node with the OID its parent names and its record's XID:
      # the root at open, every other node in directory traversal.
      def unresolved_scopes(model)
        model.filesystems.select(&:tree_decoded?).flat_map { _1.tree.nodes }.reject(&:revisit).filter_map do |node|
          kind = node.root ? :root_node : :node
          next if ObjectMapLookup.resolved?(node.mapping, node.object, kind)

          node.root ? "open" : "directories"
        end.uniq
      end
    end
  end
end
