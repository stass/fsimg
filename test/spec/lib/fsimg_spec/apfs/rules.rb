# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Logical requirements of the container level over the independently
    # decoded Model, each citing the section of specs/design-apfs.md that
    # states it. Open covers the probe, checkpoint selection, the mapped
    # objects and the container object map, which open reads to resolve
    # the volume superblocks; full verification covers the space manager's
    # chunk structures and bitmaps, the free queues and the reaper's list,
    # which only verification reads (design: Truncated containers). A
    # B-tree rule takes the scope of the tree its node belongs to.
    module Rules
      VERSION_2 = 0x2
      KNOWN_OBJECT_MAP_FLAGS = 0x1 | 0x10
      MANUALLY_MANAGED = 0x1
      SAVED = 0x2
      ENCRYPTED_VALUE = 0x4
      NO_HEADER_VALUE = 0x8
      # The block sizes at which E12 bisected the address-block threshold.
      MEASURED_BLOCK_SIZES = [4096, 16_384].freeze

      IDENTITY = ->(m, _) { m.magic == MAGIC }
      SUPERBLOCK = ->(m, _) { IDENTITY.call(m, nil) && !FsimgSpec.undefined?(m.superblock) }
      BLOCK_SIZE = ->(m, _) { SUPERBLOCK.call(m, nil) && Geometry.block_size?(m.superblock) }
      BLOCK0 = ->(m, _) { SUPERBLOCK.call(m, nil) && !FsimgSpec.undefined?(m.block0) }
      RING = ->(m, _) { BLOCK0.call(m, nil) && !FsimgSpec.undefined?(m.ring) }
      PRESENT_RING = ->(m, _) { RING.call(m, nil) && m.ring.all?(&:object) && !FsimgSpec.undefined?(m.candidates) }
      CHECKPOINT = ->(m, _) { IDENTITY.call(m, nil) && !FsimgSpec.undefined?(m.checkpoint) }
      OBJECT_MAP = ->(m, _) { CHECKPOINT.call(m, nil) && !FsimgSpec.undefined?(m.object_map_tree) }
      MANAGER = ->(m, _) { CHECKPOINT.call(m, nil) && !m.space.manager.nil? }
      REAPER = ->(m, _) { CHECKPOINT.call(m, nil) && !m.space.reaper.nil? }
      TREES = ->(m, _) { CHECKPOINT.call(m, nil) && trees(m).any? }

      module_function

      def registry = Registry.new(all)

      def rule(id:, scope:, anchor:, predicate:, applies_to:, status: "FSIMG_ECORRUPT", category: "normative")
        Rule.new(id: "apfs.#{id}", category:, policy: "must_reject", status:, scopes: Array(scope), applies_to:, predicate:,
                 domain: ->(base) { Witnesses.for_rule("apfs.#{id}", base) }, source: "specs/design-apfs.md##{anchor}")
      end

      # A profile rule whose violation fsimg refuses as unsupported.
      def unsupported(id:, scope:, anchor:, predicate:, applies_to:)
        Rule.new(id: "apfs.#{id}", category: "profile", policy: "optional_feature", scopes: Array(scope), applies_to:,
                 predicate:, domain: ->(base) { Witnesses.for_rule("apfs.#{id}", base) },
                 source: "specs/design-apfs.md##{anchor}")
      end

      # A rule over every node of every decoded tree; a violation has the
      # scope of the check that reads its tree.
      def node_rule(id:, anchor: "validation", policy: :must_reject, applies_to: ->(_node, _model) { true }, &holds)
        predicate = lambda do |model, _|
          trees(model).flat_map { |tree| tree.nodes.map { [tree.spec, _1] } }
                      .select { |spec, node| applies_to.call(node, model) && !holds.call(node, spec, model) }
                      .map { _1.first.scope }.uniq
        end
        scopes = %w[open directories verification]
        policy == :unsupported ? unsupported(id:, scope: scopes, anchor:, applies_to: TREES, predicate:) : rule(id:, scope: scopes, anchor:, applies_to: TREES, predicate:)
      end

      # A node with a decoded head; the header rule alone judges the rest.
      HEADED = ->(node, _) { !node.head.nil? }

      def all = [*container_rules, *checkpoint_rules, *object_map_rules, *node_rules, *space_rules, *queue_rules, *reaper_rules,
                 *VolumeRules.all, *RecordRules.all]

      def container_rules = [
        rule(id: "identity", scope: "open", anchor: "probe-and-identity", status: "FSIMG_EFORMAT", category: "profile",
             applies_to: ->(_, _) { true }, predicate: IDENTITY),
        rule(id: "nx.length", scope: "open", anchor: "probe-and-identity", applies_to: IDENTITY,
             predicate: ->(m, _) { SUPERBLOCK.call(m, nil) && (!Geometry.block_size?(m.superblock) || m.image_length >= m.superblock.block_size) }),
        rule(id: "nx.header", scope: "open", anchor: "container-superblock", applies_to: BLOCK0,
             predicate: ->(m, _) { m.block0.typed?(:superblock, EPHEMERAL) && m.block0.header.oid == 1 }),
        rule(id: "nx.checksum", scope: "open", anchor: "object-header-and-checksum", applies_to: BLOCK0, predicate: ->(m, _) { m.block0.sealed }),
        rule(id: "nx.block_size", scope: "open", anchor: "container-superblock", applies_to: SUPERBLOCK,
             predicate: ->(m, _) { Geometry.block_size?(m.superblock) }),
        rule(id: "nx.block_count", scope: "open", anchor: "container-superblock", applies_to: BLOCK_SIZE,
             predicate: ->(m, _) { Geometry.block_count?(m.superblock) }),
        rule(id: "nx.max_volumes", scope: "open", anchor: "container-superblock", applies_to: SUPERBLOCK,
             predicate: ->(m, _) { Geometry.max_volumes?(m.superblock) }),
        rule(id: "nx.areas", scope: "open", anchor: "container-superblock", applies_to: SUPERBLOCK,
             predicate: ->(m, _) { Geometry.areas?(m.superblock) }),
        unsupported(id: "nx.noncontiguous", scope: "open", anchor: "checkpoint-selection", applies_to: SUPERBLOCK,
                    predicate: ->(m, _) { Geometry.contiguous?(m.superblock) }),
        unsupported(id: "nx.incompatible", scope: "open", anchor: "container-superblock", applies_to: SUPERBLOCK,
                    predicate: ->(m, _) { m.superblock.incompatible_features == VERSION_2 }),
        rule(id: "nx.keylocker", scope: "open", anchor: "container-superblock", applies_to: SUPERBLOCK,
             predicate: ->(m, _) { keylockers_inside?(m.superblock) })
      ]

      def checkpoint_rules = [
        rule(id: "checkpoint.ring_present", scope: "open", anchor: "truncated-containers", applies_to: RING,
             predicate: ->(m, _) { m.ring.all?(&:object) }),
        rule(id: "checkpoint.whole", scope: "open", anchor: "checkpoint-selection", applies_to: PRESENT_RING,
             predicate: ->(m, _) { m.candidates.any?(&:whole) }),
        rule(id: "checkpoint.tie", scope: "open", anchor: "checkpoint-selection", applies_to: PRESENT_RING,
             predicate: ->(m, _) { m.candidates.map(&:xid).then { _1.uniq.size == _1.size } }),
        rule(id: "checkpoint.block0", scope: "open", anchor: "checkpoint-selection", applies_to: CHECKPOINT,
             predicate: ->(m, _) { m.block0.header.xid <= m.checkpoint.xid }),
        rule(id: "checkpoint.mappings", scope: "open", anchor: "checkpoint-selection", applies_to: CHECKPOINT,
             predicate: ->(m, _) { mappings_placed?(m.checkpoint) }),
        rule(id: "checkpoint.required", scope: "open", anchor: "checkpoint-selection", applies_to: CHECKPOINT,
             predicate: ->(m, _) { required_mapped?(m.checkpoint) }),
        rule(id: "checkpoint.objects", scope: "open", anchor: "checkpoint-selection", applies_to: CHECKPOINT,
             predicate: ->(m, _) { m.checkpoint.mappings.all? { mapped_valid?(m.checkpoint, _1) } }),
        unsupported(id: "checkpoint.object_flags", scope: "open", anchor: "object-header-and-checksum", applies_to: CHECKPOINT,
                    predicate: ->(m, _) { m.checkpoint.mappings.none? { _1.object_type.anybits?(ENCRYPTED | NONPERSISTENT) } })
      ]

      def object_map_rules = [
        rule(id: "omap.object", scope: "open", anchor: "checkpoint-selection", applies_to: CHECKPOINT,
             predicate: ->(m, _) { !FsimgSpec.undefined?(m.object_map_tree) }),
        unsupported(id: "omap.flags", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
                    predicate: ->(m, _) { (m.object_map_fields.flags & ~KNOWN_OBJECT_MAP_FLAGS).zero? }),
        rule(id: "omap.manual", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { m.object_map_fields.flags.anybits?(MANUALLY_MANAGED) }),
        rule(id: "omap.snapshots", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { m.object_map_fields.then { _1.snapshot_count.zero? && _1.latest_snapshot_xid.zero? } }),
        unsupported(id: "omap.revert", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
                    predicate: ->(m, _) { m.object_map_fields.then { _1.revert_min.zero? && _1.revert_max.zero? } }),
        rule(id: "omap.tree_type", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { m.object_map_fields.tree_type == (KINDS.fetch(:root_node) | PHYSICAL) }),
        rule(id: "omap.value_range", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { live_values(m).all? { value_inside?(_1, m.checkpoint) } }),
        rule(id: "omap.value_flags", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { live_values(m).all? { (_1.flags & ~(SAVED | ENCRYPTED_VALUE | NO_HEADER_VALUE)).zero? } }),
        unsupported(id: "omap.value_unsupported", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
                    predicate: ->(m, _) { live_values(m).none? { _1.flags.anybits?(ENCRYPTED_VALUE | NO_HEADER_VALUE) } }),
        rule(id: "omap.resolution", scope: "open", anchor: "object-maps", applies_to: OBJECT_MAP,
             predicate: ->(m, _) { m.volumes.all?(&:resolved?) })
      ]

      def node_rules = [
        node_rule(id: "btree.header") { |node, spec, model| node_header?(node, spec, model.checkpoint) },
        node_rule(id: "btree.flags", applies_to: HEADED) do |node, spec, _|
          flags = node.head.flags
          flags.anybits?(BTree::ROOT) == node.root && flags.anybits?(BTree::LEAF) == node.leaf? && node.fixed? == spec.fixed? &&
            (flags & ~BTree::KNOWN_NODE_FLAGS).zero?
        end,
        node_rule(id: "btree.footer", applies_to: ->(node, _) { node.root && node.head }) do |node, spec, model|
          footer_valid?(node.footer, spec, model.checkpoint)
        end,
        node_rule(id: "btree.level", applies_to: ->(node, _) { node.head && !node.root }) { |node, _, _| node.level == node.expected_level },
        node_rule(id: "btree.height", policy: :unsupported, applies_to: ->(node, _) { node.root && node.head }) do |node, _, _|
          node.level <= BTree::HEIGHT_BOUND
        end,
        node_rule(id: "btree.key_count", applies_to: ->(node, _) { node.head && !node.root }) { |node, _, _| node.head.key_count.positive? },
        node_rule(id: "btree.table", anchor: "node-layout", applies_to: HEADED) { |node, _, _| node.table_placed },
        node_rule(id: "btree.ghost", anchor: "node-layout", applies_to: HEADED) do |node, _, _|
          node.entries.none? { _1.value == BTree::GHOST } || (node.leaf? && node.tree_flags.to_i.anybits?(BTree::GHOSTS))
        end,
        node_rule(id: "btree.order", applies_to: HEADED) { |node, spec, _| ordered?(node, spec) },
        rule(id: "btree.footer_counts", scope: "verification", anchor: "validation", applies_to: TREES,
             predicate: ->(m, _) { trees(m).all? { footer_counts?(_1) } })
      ]

      def space_rules = [
        rule(id: "sm.geometry", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { space_geometry?(m.space) }),
        rule(id: "sm.counts", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { space_counts?(m.space) }),
        rule(id: "sm.address_blocks", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager",
             applies_to: ->(m, _) { MANAGER.call(m, nil) && MEASURED_BLOCK_SIZES.include?(m.checkpoint.block_size) },
             predicate: ->(m, _) { m.space.manager.main[:cab_count] == m.space.address_block_count }),
        unsupported(id: "sm.address_blocks_unmeasured", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager",
                    applies_to: ->(m, _) { MANAGER.call(m, nil) && !MEASURED_BLOCK_SIZES.include?(m.checkpoint.block_size) },
                    predicate: ->(m, _) { m.space.manager.main[:cab_count] == m.space.address_block_count }),
        rule(id: "sm.size", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { m.space.then { (_1.required_bytes + _1.block_size - 1) / _1.block_size * _1.block_size == _1.manager_object.bytes.bytesize } }),
        rule(id: "sm.tier2", scope: "verification", anchor: "free-queues", applies_to: MANAGER,
             predicate: ->(m, _) { m.space.manager.then { _1.tier2.except(:address_offset).values.all?(&:zero?) && zero_queue?(_1.queues.fetch(Space::TIER2_QUEUE)) } }),
        rule(id: "sm.chunk_blocks", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { chunk_blocks_valid?(m.space) }),
        rule(id: "sm.free_count", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { free_counts?(m.space) }),
        rule(id: "sm.pool_ring", scope: "verification", anchor: "ephemeral-objects-and-the-space-manager", applies_to: MANAGER,
             predicate: ->(m, _) { pool_ring?(m.space) })
      ]

      def queue_rules = [
        rule(id: "fq.count", scope: "verification", anchor: "free-queues", applies_to: MANAGER,
             predicate: ->(m, _) { m.space.queues.all? { _1.entry.queued == _1.runs.sum(&:last) } }),
        rule(id: "fq.domain", scope: "verification", anchor: "free-queues", applies_to: MANAGER,
             predicate: ->(m, _) { m.space.queues.all? { |q| q.runs.all? { |first, count| first >= q.domain.first && first + count <= q.domain.last } } }),
        rule(id: "fq.oldest_xid", scope: "verification", anchor: "free-queues", applies_to: MANAGER,
             predicate: ->(m, _) { m.space.queues.all? { _1.entry.oldest_xid <= m.checkpoint.xid } })
      ]

      def reaper_rules = [
        rule(id: "reaper.state", scope: "verification", anchor: "reaper", applies_to: REAPER,
             predicate: ->(m, _) { Space::STATE_BUFFER + m.space.reaper.state_size <= m.space.reaper_object.bytes.bytesize }),
        rule(id: "reaper.list", scope: "verification", anchor: "reaper", applies_to: REAPER,
             predicate: ->(m, _) { reap_list_well_formed?(m.space) }),
        unsupported(id: "reaper.idle", scope: "verification", anchor: "reaper",
                    applies_to: ->(m, _) { REAPER.call(m, nil) && reap_list_well_formed?(m.space) }, predicate: ->(m, _) { m.space.idle? })
      ]

      def keylockers_inside?(sb) =
        [[sb.keylocker_start, sb.keylocker_count], [sb.media_keylocker_start, sb.media_keylocker_count]].all? do |start, count|
          count.zero? || (start >= 1 && start + count <= sb.block_count)
        end

      # Every mapped run lies inside the data run without straddling the
      # ring's wrap, OIDs are distinct and no two runs overlap.
      def mappings_placed?(checkpoint)
        sb = checkpoint.superblock
        spans = checkpoint.mappings.map { [_1.address - sb.data_base, _1.byte_size / checkpoint.block_size] }
        inside = spans.all? { |first, count| first >= 0 && first + count <= sb.data_blocks && checkpoint.data_run.holds?(first, count) }
        disjoint = spans.sort.each_cons(2).all? { |(a, a_count), (b, _)| a + a_count <= b }
        inside && disjoint && checkpoint.mappings.map(&:oid).then { _1.uniq.size == _1.size }
      end

      def required_mapped?(checkpoint)
        sb = checkpoint.superblock
        { space_manager: sb.space_manager_oid, reaper: sb.reaper_oid }.all? do |kind, oid|
          checkpoint.find_mapping(oid)&.object_type == (KINDS.fetch(kind) | EPHEMERAL)
        end
      end

      def mapped_valid?(checkpoint, mapping)
        object = checkpoint.objects.fetch(mapping.oid).last
        object && object.sealed && object.header.oid == mapping.oid && object.header.object_type == mapping.object_type &&
          object.header.subtype == mapping.subtype && object.header.xid.between?(1, checkpoint.xid)
      end

      def live_values(model) = model.object_map_tree.records.map(&:last).reject { _1 == :ghost || _1.flags.anybits?(BTree::DELETED) }

      def value_inside?(value, checkpoint)
        size = checkpoint.block_size
        value.byte_size.positive? && (value.byte_size % size).zero? && value.address >= 1 && value.address + value.byte_size / size <= checkpoint.block_count
      end

      # Every tree the model decoded: the container object map's, each
      # free queue's, and each volume's object map and filesystem tree.
      def trees(model)
        object_map = FsimgSpec.undefined?(model.object_map_tree) ? [] : [model.object_map_tree]
        queues = FsimgSpec.undefined?(model.space) ? [] : model.space.queues.filter_map(&:tree)
        volumes = model.filesystems.flat_map { [_1.object_map_tree, _1.tree] }.reject { FsimgSpec.undefined?(_1) }
        object_map + queues + volumes
      end

      # A virtual node's OID is the one its parent names; the volume rules
      # judge its XID against the object-map record that resolved it.
      def node_header?(node, spec, checkpoint)
        object = node.object
        kind = node.root ? :root_node : :node
        oid = spec.storage == PHYSICAL ? object&.address : node.ref
        object && object.valid?(kind:, storage: spec.storage, oid:, xid: checkpoint.xid) && object.header.subtype == spec.subtype
      end

      # The tree flag each storage class sets; a virtual tree sets neither.
      STORAGE_TREE_FLAGS = { PHYSICAL => BTree::PHYSICAL_TREE, EPHEMERAL => BTree::EPHEMERAL_TREE, VIRTUAL => 0 }.freeze

      # A variable-size tree's footer states fixed sizes of zero.
      def footer_valid?(footer, spec, checkpoint)
        return false unless footer

        storage = STORAGE_TREE_FLAGS.fetch(spec.storage)
        others = (BTree::PHYSICAL_TREE | BTree::EPHEMERAL_TREE) & ~storage
        footer.node_size == checkpoint.block_size && (footer.tree_flags & ~BTree::KNOWN_TREE_FLAGS).zero? &&
          (footer.tree_flags & (BTree::PHYSICAL_TREE | BTree::EPHEMERAL_TREE)) == storage && !footer.tree_flags.anybits?(others) &&
          footer.key_size == spec.key_size.to_i && footer.value_size == spec.value_size.to_i
      end

      # The root footer's key and node counts are the tree's, once the walk
      # reached every node (design: Validation, full verification).
      def footer_counts?(tree)
        footer = tree.nodes.first&.footer
        return true unless tree.complete && footer

        footer.key_count == tree.records.size && footer.node_count == tree.nodes.reject(&:revisit).size
      end

      # Keys strictly increase, the first is not below the separator that
      # names the node and every key is below the next separator.
      def ordered?(node, spec)
        keys = node.entries.map(&:key)
        return true if keys.any?(&:nil?)

        orders = keys.map(&spec.order)
        bounds = [node.lower, node.upper].map { _1 && spec.order.call(_1) }
        # Keys of an undecided tie-break may be equal (design: Validation).
        increasing = keys.zip(orders).each_cons(2).all? do |(a, a_order), (b, b_order)|
          comparison = a_order <=> b_order
          comparison.negative? || (comparison.zero? && spec.undecided.call(a) && spec.undecided.call(b))
        end
        increasing &&
          (bounds.first.nil? || orders.empty? || (orders.first <=> bounds.first) >= 0) &&
          (bounds.last.nil? || keys.zip(orders).all? { |key, order| below_separator?(key, order, node.upper, bounds.last, spec) })
      end

      # A key is below the next separator, or equal to it when both have an
      # undecided tie-break (design: Validation).
      def below_separator?(key, order, separator, separator_order, spec)
        comparison = order <=> separator_order
        comparison.negative? || (comparison.zero? && spec.undecided.call(key) && spec.undecided.call(separator))
      end

      def space_geometry?(space)
        manager = space.manager
        manager.block_size == space.block_size && manager.blocks_per_chunk == space.blocks_per_chunk &&
          manager.chunks_per_cib == space.chunks_per_block && manager.cibs_per_cab == space.blocks_per_address_block
      end

      def space_counts?(space)
        main = space.manager.main
        main[:block_count] == space.block_count && main[:chunk_count] == space.chunk_count && main[:cib_count] == space.chunk_block_count
      end

      def zero_queue?(queue) = queue.values.all? { _1.is_a?(String) ? _1.bytes.all?(&:zero?) : _1.zero? }

      # Address blocks and chunk-information blocks are valid physical
      # objects of their kinds, numbered in order, and the chunk records
      # tile the device chunk by chunk.
      def chunk_blocks_valid?(space)
        xid = space.checkpoint.xid
        per_block = space.chunks_per_block
        blocks_valid = space.address_blocks.all? { chunk_block_valid?(_1, :address_block, xid, space.blocks_per_address_block) } &&
                       space.chunk_blocks.all? { chunk_block_valid?(_1, :chunk_block, xid, per_block) } &&
                       space.chunk_blocks.size == space.manager.main[:cib_count]
        blocks_valid && space.chunk_infos.each_with_index.all? { |info, chunk| chunk_info_valid?(info, chunk, space) } &&
          space.chunk_infos.size == space.chunk_count
      end

      def chunk_block_valid?(block, kind, xid, capacity)
        object = block.object
        head = object&.body(Layout::ChunkBlockHead)
        object&.valid?(kind:, storage: PHYSICAL, oid: block.address, xid:) && head.block_index == block.ordinal && head.entry_count <= capacity
      end

      def chunk_info_valid?(info, chunk, space)
        first = chunk * space.blocks_per_chunk
        blocks = [space.blocks_per_chunk, space.block_count - first].min
        info && info.first_block == first && info.block_count == blocks && info.free_count <= blocks &&
          (!info.bitmap_block.zero? || info.free_count == blocks)
      end

      # Each chunk's free count is its bitmap's clear bits, and the device's
      # free count is their sum.
      def free_counts?(space)
        infos = space.chunk_infos.compact
        infos.all? { _1.bitmap_block.zero? || space.bitmap_free[_1.first_block] == _1.free_count } &&
          infos.sum(&:free_count) == space.manager.main[:free_count]
      end

      def pool_ring?(space)
        manager = space.manager
        space.pool_indexes.size == manager.pool_bitmap_size && space.pool_xids.size == manager.pool_bitmap_size &&
          space.pool_indexes.all? { _1 < manager.pool_bitmap_blocks } && space.pool_xids.all? { _1 <= space.checkpoint.xid }
      end

      # The zero shape, or exactly count reap-list objects from the head,
      # each within its capacity, the last naming no next and equal to the
      # tail.
      def reap_list_well_formed?(space)
        reaper = space.reaper
        return reaper.tail.zero? && reaper.list_count.zero? if reaper.head.zero?

        objects = space.reap_list.objects
        lists = objects.map { |oid, object| [oid, object&.typed?(:reap_list, EPHEMERAL) ? object.body(Layout::ReapList) : nil] }
        space.reap_list.ended && objects.size == reaper.list_count && objects.last.first == reaper.tail &&
          lists.all? { |_, list| list && list.entry_count <= list.capacity }
      end
    end
  end
end
