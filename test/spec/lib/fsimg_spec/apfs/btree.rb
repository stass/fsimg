# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The generic B-tree of the design's B-trees section: a walk from a root
    # that decodes every node it reaches into a Node, whose fields the node
    # rules judge, and the records of its leaves. The walk follows a child
    # only from an index node of a legal height that is not already on its
    # path, so a cycle ends after one repeat, whose level then shows it.
    module BTree
      ROOT = 0x1
      LEAF = 0x2
      FIXED = 0x4
      TRANSIENT = 0x8000
      KNOWN_NODE_FLAGS = 0x1F
      KNOWN_TREE_FLAGS = 0x1FF
      GHOSTS = 0x4
      EPHEMERAL_TREE = 0x8
      PHYSICAL_TREE = 0x10
      UNALIGNED = 0x40
      GHOST = 0xFFFF
      DELETED = 0x1
      # The root's level bound, fsimg's: 16 levels.
      HEIGHT_BOUND = 15
      NODE_DATA = 56
      FOOTER_BYTES = 40
      INDEX_VALUE_BYTES = 8

      # A tree's codec: its subtype and storage class, fixed key and value
      # sizes and their decoders, the order of its keys and the scope of the
      # check that reads its nodes.
      Spec = Data.define(:name, :subtype, :storage, :key_size, :value_size, :key, :value, :order, :scope)

      OBJECT_MAP = Spec.new(
        name: "container object map", subtype: KINDS.fetch(:object_map), storage: PHYSICAL, key_size: 16, value_size: 16,
        key: ->(bytes) { Layout.find(Layout::ObjectMapKey, bytes) }, value: ->(bytes) { Layout.find(Layout::ObjectMapValue, bytes) },
        order: ->(key) { [key.oid, key.xid] }, scope: "open"
      )
      FREE_QUEUE = Spec.new(
        name: "free queue", subtype: 9, storage: EPHEMERAL, key_size: 16, value_size: 8,
        key: ->(bytes) { Layout.find(Layout::FreeQueueKey, bytes) }, value: ->(bytes) { bytes.unpack1("Q<") },
        order: ->(key) { [key.xid, key.first_block] }, scope: "verification"
      )

      # One table-of-contents entry: its decoded key, nil when its range
      # leaves the node, and its value bytes, GHOST, or nil likewise.
      Entry = Data.define(:key, :value, :placed)

      # One node as the walk reached it. ref is the address or OID its
      # parent named, expected_level the level its parent requires (nil at
      # the root), lower and upper the separators that bound its keys,
      # tree_flags the root footer's, and revisit whether ref is already on
      # its path. head, footer and entries are nil and empty when the object
      # is missing.
      Node = Data.define(:ref, :object, :root, :expected_level, :lower, :upper, :tree_flags, :revisit, :head, :footer,
                         :entries, :table_placed) do
        def level = head.level
        def leaf? = head.level.zero?
        def fixed? = head.flags.anybits?(FIXED)

        def follows? = !revisit && !leaf? && level <= HEIGHT_BOUND && object.sealed
      end

      Tree = Data.define(:spec, :nodes, :records, :complete)

      module_function

      # The tree under root_ref in checkpoint. Physical nodes are blocks of
      # the container, ephemeral ones the checkpoint's mapped objects.
      # The walk takes one node past bound, so that complete says whether
      # the tree held more.
      def walk(spec, root_ref, checkpoint:, bound:)
        nodes = Enumerator.new { visit(_1, spec, checkpoint, root_ref, path: [], parent: nil, lower: nil, upper: nil) }.take(bound + 1)
        records = nodes.take(bound).select { _1.head && _1.leaf? && !_1.revisit }.flat_map do |node|
          node.entries.filter_map do |entry|
            value = entry.value == GHOST ? :ghost : entry.value && spec.value.call(entry.value)
            [entry.key, value] if entry.key && value
          end
        end
        Tree.new(spec:, nodes: nodes.take(bound), records:, complete: nodes.size <= bound)
      end

      def visit(out, spec, checkpoint, ref, path:, parent:, lower:, upper:)
        node = node(spec, checkpoint, ref, parent:, revisit: path.include?(ref), lower:, upper:)
        out << node
        # Levels fall by one per step, so no valid path is deeper than the
        # height bound; a deeper one ends here.
        return unless node.head && node.follows? && path.size <= HEIGHT_BOUND

        keys = node.entries.map(&:key)
        node.entries.zip(keys.drop(1) + [upper]).each do |entry, following|
          next unless entry.value.is_a?(String) && entry.key

          visit(out, spec, checkpoint, entry.value.unpack1("Q<"), path: path + [ref], parent: node, lower: entry.key, upper: following)
        end
      end

      def node(spec, checkpoint, ref, parent:, revisit:, lower:, upper:)
        object = resolve(spec, checkpoint, ref)
        head = object&.body(Layout::NodeHead)
        root = parent.nil?
        footer = root && object ? Layout.find(Layout::NodeFooter, object.bytes, at: checkpoint.block_size - FOOTER_BYTES) : nil
        fields = { ref:, object:, root:, expected_level: parent && parent.level - 1, lower:, upper:, revisit:,
                   tree_flags: root ? footer&.tree_flags : parent.tree_flags, head:, footer: }
        return Node.new(**fields, entries: [], table_placed: false) unless head

        Table.new(spec:, head:, bytes: object.bytes, root:, tree_flags: fields[:tree_flags].to_i)
             .then { Node.new(**fields, entries: _1.entries, table_placed: _1.placed?) }
      end

      def resolve(spec, checkpoint, ref)
        return checkpoint.find_ephemeral(ref) if spec.storage == EPHEMERAL

        ref.between?(1, checkpoint.block_count - 1) ? checkpoint.reader.object(ref) : nil
      end

      # The table of contents of one node and the ranges it gives (design:
      # Node layout). The key area follows the table; value offsets count
      # back from the end of the value area, the footer's start in a root.
      class Table < Data.define(:spec, :head, :bytes, :root, :tree_flags)
        def entry_size = head.flags.anybits?(FIXED) ? 4 : 8
        def table_start = NODE_DATA + head.toc_offset
        def key_area = table_start + head.toc_length
        def free_start = key_area + head.free_offset
        def free_end = free_start + head.free_length
        def value_end = bytes.bytesize - (root ? FOOTER_BYTES : 0)

        # Entries the table's length holds, never more than the key count.
        def entries
          count = [head.key_count, head.toc_length / entry_size, bytes.bytesize / entry_size].min
          count.times.map { entry(_1) }
        end

        # Whether the table and every entry's ranges lie where the design
        # places them.
        def placed? =
          key_area <= value_end && free_end <= value_end && head.toc_length >= head.key_count * entry_size &&
          entries.size == head.key_count && entries.all?(&:placed)

        private

        def entry(index)
          fixed = head.flags.anybits?(FIXED)
          raw = Layout.find(fixed ? Layout::FixedEntry : Layout::VariableEntry, bytes, at: table_start + entry_size * index)
          return Entry.new(key: nil, value: nil, placed: false) unless raw

          key_size = fixed ? spec.key_size : raw.key_length
          value_size = fixed ? value_size_of : raw.value_length
          key_at = key_area + raw.key_offset
          value_at = value_end - raw.value_offset
          key_placed = aligned?(raw.key_offset) && key_at + key_size <= free_start
          value_placed = raw.value_offset == GHOST || (aligned?(raw.value_offset) && value_at >= free_end && value_at + value_size <= value_end)
          # A key or value whose length is not the codec's is not decoded.
          sized = key_size == spec.key_size && value_size == value_size_of
          Entry.new(key: key_placed && sized ? spec.key.call(bytes.byteslice(key_at, key_size)) : nil,
                    value: value(raw.value_offset, at: value_at, size: value_size, placed: value_placed && sized),
                    placed: key_placed && value_placed)
        end

        def value_size_of = head.level.zero? ? spec.value_size : INDEX_VALUE_BYTES

        def value(offset, at:, size:, placed:)
          return GHOST if offset == GHOST

          placed ? bytes.byteslice(at, size) : nil
        end

        def aligned?(offset) = tree_flags.anybits?(UNALIGNED) || (offset % 8).zero?
      end
    end
  end
end
