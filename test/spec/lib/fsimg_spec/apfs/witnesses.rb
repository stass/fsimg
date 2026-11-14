# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Targeted witnesses over a native formatter base: each is a list of
    # mutations that breaks one rule's statement. A field mutation inside a
    # checksummed object is followed by the object's checksum recomputed
    # from the mutated bytes, the design's reseal, written as the explicit
    # bytes a recipe carries, so the stale checksum does not also fire.
    # Mutations state the broken contract, never an expected result:
    # classification derives that. Offsets come from the base's own model.
    module Witnesses
      Witness = Data.define(:id, :base, :mutations, :intended)

      # A base: its name, its image view and the model decoded from it.
      Base = Data.define(:name, :view, :model) do
        def self.open(name, path)
          view = ImageView.open(path)
          new(name:, view:, model: Model.decode(view))
        end

        def checkpoint = model.checkpoint
        def block_size = checkpoint.block_size
        def block_count = checkpoint.block_count
        def xid = checkpoint.xid
        def selected = model.candidates.select(&:whole).max_by(&:xid)
        def older = model.candidates.reject { _1 == selected }.max_by(&:xid)
        def map_slot = model.ring[selected.superblock.descriptor_index]
        def mapping_index(oid) = checkpoint.mappings.index { _1.oid == oid }
        def space_manager = checkpoint.find_mapping(selected.superblock.space_manager_oid).address
        def reaper = checkpoint.find_mapping(selected.superblock.reaper_oid).address
        def object_map = checkpoint.superblock.object_map_oid
        def object_map_root = model.object_map_tree.nodes.first.object.address
        def main_queue_root = model.space.queues.fetch(Space::MAIN_QUEUE).tree.nodes.first.object.address
        def chunk_block = model.space.chunk_blocks.first.address

        # The highest block below N that no chunk bitmap marks allocated.
        def free_block
          infos = model.space.chunk_infos
          (block_count - 1).downto(1).find do |block|
            info = infos[block / (8 * block_size)]
            bitmap = info.bitmap_block.zero? ? nil : checkpoint.reader.blocks(info.bitmap_block)
            bitmap.nil? || bitmap.unpack1("b*")[block % (8 * block_size)] == "0"
          end
        end
      end

      # The native capture every witness mutates: the 4 MiB container of
      # E1 with one empty volume, two committed checkpoints and both free
      # queues holding one record.
      BASE = "apfs-newfs-4194304-plain"

      # Rules no native base can target, and why.
      UNTARGETED = {
        "apfs.sm.address_blocks_unmeasured" =>
          "needs a container whose block size is neither 4096 nor 16384, which no native capture holds"
      }.freeze

      module_function

      def for_rule(id, base) = changes(base).fetch(id, {}).map { |name, mutations| Witness.new(id: "#{id}.#{name}", base: base.name, mutations:, intended: id) }

      def u8(offset, value) = Mutation::SetInteger.new(op: "set_u8", offset:, value:)
      def u16(offset, value) = Mutation::SetInteger.new(op: "set_u16le", offset:, value:)
      def u32(offset, value) = Mutation::SetInteger.new(op: "set_u32le", offset:, value:)
      def u64(offset, value) = Mutation::ReplaceBytes.new(offset:, bytes: [value].pack("Q<"))
      def bytes(offset, value) = Mutation::ReplaceBytes.new(offset:, bytes: value)

      # The mutations writing fields, a list of [offset in the object,
      # mutation builder, value], into the object at block address,
      # followed by its checksum recomputed from the mutated bytes.
      def resealed(base, address, *fields, blocks: 1)
        at = address * base.block_size
        mutations = fields.map { |offset, builder, value| method(builder).call(at + offset, value) }
        view = mutations.reduce(base.view) { |image, mutation| mutation.apply(image) }
        mutations + [bytes(at, Fletcher64.seal(view.read(at, blocks * base.block_size)).byteslice(0, 8))]
      end

      # A non-root node of the container object map at block address,
      # with entries [key bytes, value bytes] in a fixed-size table.
      def child_node(base, address, entries)
        size = base.block_size
        header = Layout::ObjectHeader.new(oid: address, xid: base.xid, object_type: KINDS.fetch(:node) | PHYSICAL,
                                          subtype: KINDS.fetch(:object_map)).to_binary_s
        table = entries.each_with_index.map { |_, i| Layout::FixedEntry.new(key_offset: 16 * i, value_offset: 16 * (i + 1)).to_binary_s }.join
        toc_length = 4 * [entries.size, 1].max.then { (_1 + 3) / 4 * 4 }
        keys = entries.map(&:first).join
        head = Layout::NodeHead.new(flags: BTree::LEAF | BTree::FIXED, key_count: entries.size, toc_length:,
                                    free_offset: keys.bytesize, free_length: size - 56 - toc_length - 32 * entries.size).to_binary_s
        body = header + head + table.ljust(toc_length, "\0") + keys
        values = entries.reverse.map(&:last).join
        Fletcher64.seal(body.ljust(size - values.bytesize, "\0") + values)
      end

      # The object map's root made an index node naming child at block
      # address through its one entry, its footer counting two nodes.
      def root_naming(base, child)
        resealed(base, base.object_map_root, [32, :u16, BTree::ROOT | BTree::FIXED], [34, :u16, 1], [omap_value_at(base), :u64, child],
                 [base.block_size - BTree::FOOTER_BYTES + 32, :u64, 2])
      end

      def raw_value_offset(base, address) = base.view.read(address * base.block_size + 58, 2).unpack1("v")

      def key_offset(base, address) =
        base.view.read(address * base.block_size + 40, 4).unpack("vv").then { |offset, length| 56 + offset + length } +
        base.view.read(address * base.block_size + 56, 2).unpack1("v")

      def omap_value_at(base) = base.block_size - BTree::FOOTER_BYTES - raw_value_offset(base, base.object_map_root)

      # Rule => witness name => mutations.
      def changes(base)
        return {} unless base.name == BASE

        [container(base), checkpoint(base), object_map(base), nodes(base), space(base), reaper(base)].reduce(:merge)
      end

      def container(base)
        sb = base.model.superblock
        {
          "apfs.identity" => { "magic" => [u8(32, 0)] },
          "apfs.nx.length" => { "short_block" => [Mutation::Truncate.new(length: base.block_size - 1)] },
          "apfs.nx.header" => { "kind" => resealed(base, 0, [24, :u32, EPHEMERAL | KINDS.fetch(:root_node)]) },
          "apfs.nx.checksum" => { "stale" => [u8(0, base.view.read(0, 1).ord ^ 1)] },
          "apfs.nx.block_size" => { "half" => resealed(base, 0, [36, :u32, 2048]) },
          "apfs.nx.block_count" => { "below_minimum" => resealed(base, 0, [40, :u64, 100]) },
          "apfs.nx.max_volumes" => { "zero" => resealed(base, 0, [180, :u32, 0]) },
          "apfs.nx.areas" => { "overlap" => resealed(base, 0, [120, :u64, sb.descriptor_base]) },
          "apfs.nx.noncontiguous" => { "data_area" => resealed(base, 0, [108, :u32, sb.data_blocks | Geometry::NONCONTIGUOUS]) },
          "apfs.nx.incompatible" => { "fusion" => resealed(base, 0, [64, :u64, 0x102]) },
          "apfs.nx.keylocker" => { "past_end" => resealed(base, 0, [1296, :u64, sb.block_count], [1304, :u64, 1]) }
        }
      end

      def checkpoint(base)
        selected = base.selected.slot.object.address
        map = base.map_slot.object.address
        mapping = ->(oid, field) { 40 + 40 * base.mapping_index(oid) + field }
        sb = base.selected.superblock
        maps = base.model.ring.select(&:map?).map { u8(_1.object.address * base.block_size, _1.object.bytes.ord ^ 1) }
        {
          "apfs.checkpoint.ring_present" => { "cut_ring" => [Mutation::Truncate.new(length: (sb.descriptor_base + 1) * base.block_size)] },
          "apfs.checkpoint.whole" => { "every_map_stale" => maps },
          "apfs.checkpoint.tie" => { "equal_xid" => resealed(base, base.older.slot.object.address, [16, :u64, base.xid]) },
          "apfs.checkpoint.block0" => { "newer" => resealed(base, 0, [16, :u64, base.xid + 1]) },
          "apfs.checkpoint.mappings" => { "overlap" => resealed(base, map, [mapping.call(sb.reaper_oid, 32), :u64, base.space_manager]) },
          "apfs.checkpoint.required" => { "reaper_unmapped" => resealed(base, selected, [168, :u64, sb.reaper_oid + 1000]) },
          "apfs.checkpoint.objects" => { "future_xid" => resealed(base, base.reaper, [16, :u64, base.xid + 1]) },
          "apfs.checkpoint.object_flags" => {
            "encrypted" => resealed(base, map, [mapping.call(sb.reaper_oid, 0), :u32, EPHEMERAL | ENCRYPTED | KINDS.fetch(:reaper)])
          }
        }
      end

      def object_map(base)
        omap = base.object_map
        value = omap_value_at(base)
        root = base.object_map_root
        volume = base.model.volumes.first.object.address
        {
          "apfs.omap.object" => { "zero_xid" => resealed(base, omap, [16, :u64, 0]) },
          "apfs.omap.flags" => { "encrypting" => resealed(base, omap, [32, :u32, 0x3]) },
          "apfs.omap.manual" => { "cleared" => resealed(base, omap, [32, :u32, 0]) },
          "apfs.omap.snapshots" => { "count" => resealed(base, omap, [36, :u32, 1]) },
          "apfs.omap.revert" => { "pending" => resealed(base, omap, [72, :u64, 5]) },
          "apfs.omap.tree_type" => { "ephemeral" => resealed(base, omap, [40, :u32, EPHEMERAL | KINDS.fetch(:root_node)]) },
          "apfs.omap.value_range" => { "past_end" => resealed(base, root, [value + 8, :u64, base.block_count + 10]) },
          "apfs.omap.value_flags" => { "unknown" => resealed(base, root, [value, :u32, 0x20]) },
          "apfs.omap.value_unsupported" => { "encrypted" => resealed(base, root, [value, :u32, 0x4]) },
          "apfs.omap.resolution" => { "older_object" => resealed(base, volume, [16, :u64, base.model.volumes.first.found.first.xid - 1]) }
        }
      end

      def nodes(base)
        root = base.object_map_root
        footer = base.block_size - BTree::FOOTER_BYTES
        flags = base.model.object_map_tree.nodes.first.head.flags
        free = base.free_block
        child = ->(entries) { [bytes(free * base.block_size, child_node(base, free, entries)), *root_naming(base, free)] }
        low_key = Layout::ObjectMapKey.new(oid: 1, xid: 1).to_binary_s
        value = Layout::ObjectMapValue.new(byte_size: base.block_size, address: free).to_binary_s
        {
          "apfs.btree.header" => { "subtype" => resealed(base, root, [28, :u32, 9]) },
          "apfs.btree.flags" => { "leaf_cleared" => resealed(base, root, [32, :u16, flags & ~BTree::LEAF]) },
          "apfs.btree.footer" => { "node_size" => resealed(base, root, [footer + 4, :u32, 2 * base.block_size]) },
          "apfs.btree.footer_counts" => { "node_count" => resealed(base, root, [footer + 32, :u64, 2]) },
          "apfs.btree.level" => { "self_child" => root_naming(base, root) },
          "apfs.btree.height" => { "seventeen_levels" => resealed(base, root, [32, :u16, flags & ~BTree::LEAF], [34, :u16, 16]) },
          "apfs.btree.key_count" => { "empty_child" => child.call([]) },
          "apfs.btree.table" => { "short_table" => resealed(base, root, [42, :u16, 0]) },
          "apfs.btree.ghost" => { "unflagged" => resealed(base, root, [58, :u16, BTree::GHOST]) },
          "apfs.btree.order" => { "child_below_separator" => child.call([[low_key, value]]) }
        }
      end

      def space(base)
        sm = base.space_manager
        manager = base.model.space.manager
        queue = ->(index, field) { 200 + 40 * index + field }
        fq_key = key_offset(base, base.main_queue_root)
        {
          "apfs.sm.geometry" => { "block_size" => resealed(base, sm, [32, :u32, 2 * base.block_size]) },
          "apfs.sm.counts" => { "chunk_count" => resealed(base, sm, [56, :u64, manager.main[:chunk_count] + 1]) },
          "apfs.sm.address_blocks" => { "one_address_block" => resealed(base, sm, [68, :u32, 1]) },
          "apfs.sm.size" => { "array_past_object" => resealed(base, sm, [80, :u32, base.block_size - 4]) },
          "apfs.sm.tier2" => { "blocks" => resealed(base, sm, [96, :u64, 1]) },
          "apfs.sm.chunk_blocks" => { "short_chunk" => resealed(base, base.chunk_block, [56, :u32, base.model.space.chunk_infos.first.block_count - 1]) },
          "apfs.sm.free_count" => { "device" => resealed(base, sm, [72, :u64, manager.main[:free_count] + 1]) },
          "apfs.sm.pool_ring" => { "index_past_ring" => resealed(base, sm, [manager.pool_index_offset, :u16, manager.pool_bitmap_blocks]) },
          "apfs.fq.count" => { "main" => resealed(base, sm, [queue.call(Space::MAIN_QUEUE, 0), :u64, manager.queues[Space::MAIN_QUEUE][:queued] + 1]) },
          "apfs.fq.domain" => { "past_end" => resealed(base, base.main_queue_root, [fq_key + 8, :u64, base.block_count]) },
          "apfs.fq.oldest_xid" => { "future" => resealed(base, sm, [queue.call(Space::MAIN_QUEUE, 16), :u64, base.xid + 1]) }
        }
      end

      def reaper(base)
        reaper = base.reaper
        {
          "apfs.reaper.state" => { "past_object" => resealed(base, reaper, [108, :u32, base.block_size]) },
          "apfs.reaper.list" => { "count_without_head" => resealed(base, reaper, [68, :u32, 1]) },
          "apfs.reaper.idle" => { "reaping" => resealed(base, reaper, [64, :u32, 0x3]) }
        }
      end
    end
  end
end
