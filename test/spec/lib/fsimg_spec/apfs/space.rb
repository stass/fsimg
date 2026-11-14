# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The selected checkpoint's ephemeral container state: the space manager
    # with its chunk structures and bitmaps (design: Ephemeral objects and
    # the space manager), its free queues (Free queues) and the reaper with
    # its reap list (Reaper). Every part is nil when its object is missing.
    #
    # chunk_blocks are the chunk-information blocks in address-array order,
    # each with its ordinal; address_blocks the address blocks likewise.
    # bitmap_free maps a chunk's first block to the clear bits of its bitmap
    # within the chunk's block count.
    class Space < Data.define(:checkpoint, :manager, :manager_object, :address_blocks, :chunk_blocks, :bitmap_free,
                              :chunk_infos, :queues, :reaper, :reaper_object, :reap_list, :complete)
      POOL_QUEUE = 0
      MAIN_QUEUE = 1
      TIER2_QUEUE = 2
      REAPING = 0x2
      STATE_BUFFER = 112
      REAP_LIST_HEAD = 64
      REAP_ENTRY_BYTES = 40

      # One free queue: its entry, its domain [first, last), and its tree,
      # nil for a zero tree OID.
      Queue = Data.define(:entry, :domain, :tree) do
        def runs = tree ? tree.records.map { |key, value| [key.first_block, value == :ghost ? 1 : value] } : []

        def records = tree ? tree.records : []
      end

      ChunkBlock = Data.define(:ordinal, :address, :object)

      # The reap list read from its head: the objects reached, at most one
      # more than the reaper's count, and whether the walk ended at a zero
      # next OID.
      ReapList = Data.define(:objects, :ended)

      def self.decode(checkpoint)
        return checkpoint if FsimgSpec.undefined?(checkpoint)

        manager_object = checkpoint.find_ephemeral(checkpoint.superblock.space_manager_oid)
        manager = manager_object&.body(Layout::SpaceManager)
        reaper_object = checkpoint.find_ephemeral(checkpoint.superblock.reaper_oid)
        reaper = reaper_object&.body(Layout::Reaper)
        parts = manager ? chunk_parts(checkpoint, manager, manager_object) : { address_blocks: [], chunk_blocks: [], chunk_infos: [], bitmap_free: {}, complete: true }
        queues = manager ? queues(checkpoint, manager) : []
        new(checkpoint:, manager:, manager_object:, **parts, queues:, reaper:, reaper_object:,
            reap_list: reaper && reap_list(checkpoint, reaper),
            complete: parts[:complete] && queues.all? { _1.tree.nil? || _1.tree.complete })
      end

      # The address array names chunk-information blocks directly while the
      # address-block count is zero, and address blocks otherwise.
      def self.chunk_parts(checkpoint, manager, manager_object)
        main = manager.main
        direct = main[:cab_count].zero?
        names = u64s(manager_object.bytes, main[:address_offset], direct ? main[:cib_count] : main[:cab_count])
        return { address_blocks: [], chunk_blocks: [], chunk_infos: [], bitmap_free: {}, complete: false } if names.size > Model::CHUNK_BLOCK_BOUND

        reader = checkpoint.reader
        address_blocks = direct ? [] : names.each_with_index.map { |address, ordinal| ChunkBlock.new(ordinal:, address:, object: reader.object(address)) }
        cib_addresses = direct ? names : address_blocks.flat_map { cab_entries(_1) }
        complete = cib_addresses.size <= Model::CHUNK_BLOCK_BOUND
        chunk_blocks = cib_addresses.take(Model::CHUNK_BLOCK_BOUND).each_with_index.map do |address, ordinal|
          ChunkBlock.new(ordinal:, address:, object: address.between?(1, checkpoint.block_count - 1) ? reader.object(address) : nil)
        end
        chunk_infos = chunk_blocks.flat_map { chunk_infos(_1) }
        { address_blocks:, chunk_blocks:, chunk_infos:, bitmap_free: bitmap_free(reader, chunk_infos), complete: }
      end

      def self.cab_entries(block)
        head = block.object&.body(Layout::ChunkBlockHead)
        head ? u64s(block.object.bytes, 40, head.entry_count) : []
      end

      # The u64 values of count slots from offset at, as many as the bytes hold.
      def self.u64s(bytes, at, count)
        held = [[(bytes.bytesize - at) / 8, 0].max, count].min
        bytes.byteslice(at, 8 * held).to_s.unpack("Q<*")
      end

      # Clear bits of every chunk's bitmap within its block count, least
      # significant bit first; a missing bitmap block has no entry.
      def self.bitmap_free(reader, chunk_infos)
        chunk_infos.reject { _1.bitmap_block.zero? }.each_with_object({}) do |info, free|
          bitmap = reader.blocks(info.bitmap_block)
          free[info.first_block] = bitmap.unpack1("b*")[0, info.block_count].count("0") if bitmap
        end
      end

      def self.chunk_infos(block)
        head = block.object&.body(Layout::ChunkBlockHead)
        return [] unless head

        Layout.records(Layout::ChunkInfo, block.object.bytes, at: 40, count: head.entry_count)
      end

      def self.queues(checkpoint, manager)
        domains = { POOL_QUEUE => [manager.pool_base, manager.pool_base + manager.pool_block_count],
                    MAIN_QUEUE => [1, checkpoint.block_count] }
        domains.map do |index, domain|
          entry = Layout::VALUES.fetch(Layout::FreeQueueEntry).new(**manager.queues.fetch(index))
          tree = entry.tree_oid.zero? ? nil : BTree.walk(BTree::FREE_QUEUE, entry.tree_oid, checkpoint:, bound: Model::NODE_BOUND)
          Queue.new(entry:, domain:, tree:)
        end
      end

      # Follows the list from its head for at most count + 1 objects, so a
      # list that runs past its count shows as one object too many. Every
      # list object is a distinct mapped object, so a walk longer than the
      # mappings is a cycle and stops one past them.
      def self.reap_list(checkpoint, reaper)
        reached = Enumerator.produce(reaper.head) { next_oid(checkpoint, _1) }.lazy.take_while(&:positive?)
                            .first([reaper.list_count, checkpoint.mappings.size].min + 1)
        objects = reached.map { [_1, checkpoint.find_ephemeral(_1)] }
        ReapList.new(objects:, ended: reached.size <= reaper.list_count && (reached.empty? || next_oid(checkpoint, reached.last).zero?))
      end

      def self.next_oid(checkpoint, oid) = oid.zero? ? 0 : checkpoint.find_ephemeral(oid)&.body(Layout::ReapList)&.next_oid || 0

      private_class_method :chunk_parts, :cab_entries, :bitmap_free, :queues, :reap_list, :next_oid

      def block_size = checkpoint.block_size
      def block_count = checkpoint.block_count
      def blocks_per_chunk = 8 * block_size
      def chunks_per_block = (block_size - 40) / 32
      def blocks_per_address_block = (block_size - 40) / 8
      def chunk_count = (block_count + blocks_per_chunk - 1) / blocks_per_chunk
      def chunk_block_count = (chunk_count + chunks_per_block - 1) / chunks_per_block

      # The address-block count the design's relation gives (E12).
      def address_block_count
        cibs = chunk_block_count
        cibs <= blocks_per_address_block ? 0 : (cibs + blocks_per_address_block - 1) / blocks_per_address_block
      end

      # The bytes the structure and its arrays reach within the object.
      def required_bytes
        main = manager.main
        names = main[:cab_count].zero? ? main[:cib_count] : main[:cab_count]
        [manager.structure_size, manager.pool_xid_offset + 8 * manager.pool_bitmap_size,
         manager.pool_index_offset + 2 * manager.pool_bitmap_size,
         manager.pool_free_next_offset + 2 * manager.pool_bitmap_blocks, main[:address_offset] + 8 * names].max
      end

      # The pool ring's XID and bitmap-index arrays, as many as the object holds.
      def pool_xids = Space.u64s(manager_object.bytes, manager.pool_xid_offset, manager.pool_bitmap_size)

      def pool_indexes
        bytes = manager_object.bytes
        held = [[(bytes.bytesize - manager.pool_index_offset) / 2, 0].max, manager.pool_bitmap_size].min
        bytes.byteslice(manager.pool_index_offset, 2 * held).to_s.unpack("v*")
      end

      def idle? = !reaper.flags.anybits?(REAPING) && reap_list.objects.all? { |_, object| object&.body(Layout::ReapList)&.entry_count.to_i.zero? }
    end
  end
end
