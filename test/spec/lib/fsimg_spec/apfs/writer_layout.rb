# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The writer's one-volume profile as specs/design-apfs.md states it
    # (Writer: Profile, Settings and identities, Layout and Writer values),
    # computed from the container's block count and the creation's settings
    # and tree. A created image must decode to it: problems answers each
    # way the image differs, which the created hook reports as the reason
    # its layout is not the one the creation calls for.
    module WriterLayout
      BLOCK = 4096
      MIB = 1 << 20
      BLOCKS_PER_CHUNK = 32_768
      CHUNKS_PER_INFO_BLOCK = 126
      INFO_BLOCKS_PER_ADDRESS_BLOCK = 507
      MAPPINGS_PER_BLOCK = 101
      POOL_TRANSACTIONS = 16
      CONTAINER_INCOMPATIBLE = 0x2
      VOLUME_INCOMPATIBLE = { "insensitive" => Volume::CASE_INSENSITIVE, "sensitive" => Volume::NORMALIZATION_INSENSITIVE }.freeze
      # The label the CLI passes when -L is absent (specs/design-cli.md),
      # and the writer's default for an empty one (Writer values).
      CLI_LABEL = "FSIMG".b.freeze
      DEFAULT_LABEL = "Untitled".b.freeze
      MAIN_QUEUE = 1
      POOL_QUEUE = 0
      ZERO_SCAN_BLOCKS = 256
      # The writer's minimum container (Writer values).
      MINIMUM_BLOCKS = 1024

      # The fixed regions of a container of n blocks whose space manager
      # maps s blocks (Writer values: Checkpoint and container superblock,
      # Space manager). These are the design's fits to the native captures,
      # which hold from 92 blocks to 128 TiB.
      Regions = Data.define(:chunks, :info_blocks, :address_blocks, :max_volumes, :main_limit, :pool_limit,
                            :descriptor_blocks, :data_blocks, :pool_bitmap, :pool_blocks) do
        def data_base = 1 + descriptor_blocks
        def ring_base = data_base + data_blocks
        def ring_blocks = POOL_TRANSACTIONS * pool_bitmap
        def pool_base = ring_base + ring_blocks
        # The first block after the gapless prefix, where the run starts.
        def prefix_end = pool_base + pool_blocks
      end

      module_function

      # q(x, k) of the design: ceil(x / k), except that 2 becomes 3.
      def q(x, k) = x.ceildiv(k).then { _1 == 2 ? 3 : _1 }

      def regions(n, s)
        chunks = n.ceildiv(BLOCKS_PER_CHUNK)
        info_blocks = chunks.ceildiv(CHUNKS_PER_INFO_BLOCK)
        address_blocks = info_blocks <= INFO_BLOCKS_PER_ADDRESS_BLOCK ? 0 : info_blocks.ceildiv(INFO_BLOCKS_PER_ADDRESS_BLOCK)
        pool_blocks = 3 * (chunks + info_blocks + address_blocks)
        main_limit = main_queue_limit(n)
        pool_limit = [512, q(pool_blocks, 1136)].min
        max_volumes = [100, (n * BLOCK).ceildiv(512 * MIB)].min
        ephemeral = n * BLOCK <= 128 * MIB ? main_limit : 8
        e = main_limit + pool_limit + 2 * s + 1 + 8 * ephemeral * max_volumes
        Regions.new(chunks:, info_blocks:, address_blocks:, max_volumes:, main_limit:, pool_limit:,
                    descriptor_blocks: 4 * (e.ceildiv(MAPPINGS_PER_BLOCK) + 1), data_blocks: 4 * e,
                    pool_bitmap: pool_blocks.ceildiv(BLOCKS_PER_CHUNK), pool_blocks:)
      end

      def main_queue_limit(n)
        if n < (1 << 18) then q(n, 4544)
        elsif n < (1 << 20) then q(n, 2272)
        else 512
        end
      end

      # How the created image, its decoded model and the view it came
      # from, differs from the profile the source, a SourceModel, calls
      # for. The model must hold the namespace (Model.decode's namespace:).
      def problems(model, view, source)
        return ["the created image has no decodable checkpoint: #{model.checkpoint.reason}"] if FsimgSpec.undefined?(model.checkpoint)

        container_problems(model, view, source) + volume_problems(model, source)
      end

      def container_problems(model, view, source)
        return ["the created image has no space manager"] if FsimgSpec.undefined?(model.space) || model.space.manager.nil?

        sb = model.checkpoint.superblock
        n = sb.block_count
        manager = model.space.manager
        s = model.checkpoint.find_mapping(sb.space_manager_oid).byte_size / BLOCK
        r = regions(n, s)
        allocated = n - manager.main.free_count
        expected = {
          "block size" => [sb.block_size, BLOCK],
          "image length" => [view.length, n * BLOCK],
          "block count" => [n, source.settings.key?(:size) ? source.settings[:size] / BLOCK : [n, MINIMUM_BLOCKS].max],
          "checkpoint XID" => [model.checkpoint.xid, 1],
          "next XID" => [sb.next_xid, 2],
          "incompatible features" => [sb.incompatible_features, CONTAINER_INCOMPATIBLE],
          "maximum volume count" => [sb.max_volumes, r.max_volumes],
          "descriptor area" => [[sb.descriptor_base, sb.descriptor_blocks], [1, r.descriptor_blocks]],
          "data area" => [[sb.data_base, sb.data_blocks], [r.data_base, r.data_blocks]],
          "descriptor ring" => [[sb.descriptor_index, sb.descriptor_length, sb.descriptor_next], [0, 2, 2]],
          "data ring" => [[sb.data_index, sb.data_length, sb.data_next], [0, s + 1, s + 1]],
          "chunk counts" => [[manager.main.chunk_count, manager.main.cib_count, manager.main.cab_count],
                             [r.chunks, r.info_blocks, r.address_blocks]],
          "internal pool" => [[manager.pool_bitmap_base, manager.pool_bitmap_size, manager.pool_bitmap_blocks, manager.pool_base,
                               manager.pool_block_count], [r.ring_base, r.pool_bitmap, r.ring_blocks, r.pool_base, r.pool_blocks]],
          "free-queue node limits" => [[manager.queues[POOL_QUEUE].node_limit, manager.queues[MAIN_QUEUE].node_limit],
                                       [r.pool_limit, r.main_limit]]
        }
        mismatches = mismatches(expected)
        return mismatches + ["#{allocated} allocated blocks are not between the #{r.prefix_end}-block fixed prefix and #{n}"] unless
          allocated.between?(r.prefix_end, n)

        mismatches + unused_problems(view, allocated:, n:)
      end

      # The writer allocates one range from block 0, so every block from the
      # allocated count to n is free and zero (Writer values: Unused bytes).
      # The scan stops at the image's end, whose length is judged apart.
      def unused_problems(view, allocated:, n:)
        step = ZERO_SCAN_BLOCKS * BLOCK
        last = [n * BLOCK, view.length].min
        dirty = (allocated * BLOCK...last).step(step).find do |offset|
          view.read(offset, [step, last - offset].min).match?(/[^\0]/n)
        end
        dirty ? ["a free block at or after byte #{dirty} holds a nonzero byte"] : []
      end

      # One volume in slot 0 with the case setting's flag, the label, the
      # source tree's object counts and its namespace.
      def volume_problems(model, source)
        volumes = model.filesystems
        return ["the created image resolves #{volumes.size} volumes, not one"] unless volumes.one?

        volume = volumes.first
        counts = source.tree.map(&:type).tally
        settings = source.settings
        sb = volume.superblock
        expected = {
          "volume slot" => [volume.slot, 0],
          "volume incompatible features" => [sb.incompatible_features, VOLUME_INCOMPATIBLE.fetch(settings.fetch(:case, "insensitive"))],
          "volume name" => [volume.name, label(settings)],
          "object counts" => [[sb.file_count, sb.directory_count, sb.symlink_count, sb.other_count],
                              %w[file directory symlink fifo].map { counts.fetch(_1, 0) }]
        }
        mismatches = mismatches(expected)
        mismatches + namespace_problems(model.tree, source)
      end

      # Each "what actual, not wanted" for an expected entry [actual,
      # wanted] that differs, its values shown since they come from the
      # image.
      def mismatches(expected) = expected.filter_map do |what, (actual, wanted)|
        "#{what} #{FsimgSpec.shown(actual)}, not #{FsimgSpec.shown(wanted)}" unless actual == wanted
      end

      # The volume name: the label setting, the CLI's when there is none,
      # and the writer's default when it is empty.
      def label(settings)
        given = settings.fetch(:label, CLI_LABEL).b
        given.empty? ? DEFAULT_LABEL : given
      end

      def namespace_problems(tree, source)
        return ["the model cannot walk the created volume's namespace: #{tree.reason}"] if FsimgSpec.undefined?(tree)

        KnownTree.differences("the created volume", actual: tree, expected: KnownTree.of(source.tree))
      end

      private_class_method :mismatches, :container_problems, :unused_problems, :volume_problems, :namespace_problems, :main_queue_limit
    end
  end
end
