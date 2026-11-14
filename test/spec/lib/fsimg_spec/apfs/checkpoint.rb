# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Checkpoint selection (design: Checkpoint selection): one pass over the
    # descriptor ring records each slot, candidates are the valid ring
    # superblocks that agree with block 0, and the checkpoint is the
    # candidate of largest XID whose descriptor run is whole.
    module Checkpoints
      LAST_MAP = 0x1
      MAPPING_BYTES = 40
      # The fields in which a candidate agrees with block 0.
      AGREEING = %i[block_size uuid descriptor_base descriptor_blocks].freeze
      # The largest mapped object the decoder reads, in blocks; native space
      # managers take at most two.
      OBJECT_BLOCK_BOUND = 64

      Result = Data.define(:ring, :candidates, :checkpoint)

      # The largest ring the decoder reads; a larger one leaves open
      # unclassified.
      RING_BOUND = 1 << 16
      RING_BOUNDED = "the descriptor ring is larger than the decoder's bound"

      # The data run: the slots of the data area from first for length,
      # wrapping modulo size.
      Run = Data.define(:first, :length, :size) do
        # Whether the count slots from slot, which do not pass the area's
        # end, all lie in the run.
        def holds?(slot, count) = ((slot - first) % size) + count <= length
      end

      # One descriptor slot: its ring index and object, nil when the image
      # ends before it.
      Slot = Data.define(:index, :object) do
        def superblock?
          object && object.sealed && object.typed?(:superblock, EPHEMERAL) && object.header.oid == 1 &&
            object.header.xid.positive? && object.bytes.byteslice(32, 4) == MAGIC
        end

        def map?
          object && object.sealed && object.typed?(:checkpoint_map, PHYSICAL) && object.header.oid == object.address &&
            object.header.xid.positive?
        end

        def map_head = object.body(Layout::CheckpointMapHead)

        def mappings = map_head.entry_count.times.filter_map { object.body(Layout::CheckpointMapping, at: 40 + MAPPING_BYTES * _1) }
      end

      # A ring superblock that agrees with block 0, and whether its run is
      # whole.
      Candidate = Data.define(:slot, :superblock, :whole) do
        def xid = slot.object.header.xid
      end

      # The selected checkpoint: its superblock, XID, every mapping of its
      # run in order, the data-area slots its data run covers and the object
      # each mapping names, OID => [mapping, ObjectView or nil].
      class Checkpoint < Data.define(:superblock, :xid, :reader, :mappings, :data_run, :objects)
        def block_size = reader.block_size
        def block_count = superblock.block_count

        # The ephemeral object with OID oid, or nil.
        def find_ephemeral(oid) = objects[oid]&.last

        def find_mapping(oid) = objects[oid]&.first
      end

      module_function

      def decode(view, block0_superblock, block0)
        missing = [block0_superblock, block0].find { FsimgSpec.undefined?(_1) }
        return Result.new(ring: missing, candidates: missing, checkpoint: missing) if missing
        unless Geometry.valid?(block0_superblock)
          gone = Undefined.new(reason: "block 0's geometry locates no ring")
          return Result.new(ring: gone, candidates: gone, checkpoint: gone)
        end

        bounded = Undefined.new(reason: RING_BOUNDED)
        return Result.new(ring: bounded, candidates: bounded, checkpoint: bounded) if block0_superblock.descriptor_blocks > RING_BOUND

        reader = Reader.new(view:, block_size: block0_superblock.block_size)
        # The ring is read up to its first slot past the image: the read
        # of that slot fails the open (design: Truncated containers).
        ring = block0_superblock.descriptor_blocks.times.lazy
                                .map { Slot.new(index: _1, object: reader.object(block0_superblock.descriptor_base + _1)) }
                                .slice_after { _1.object.nil? }.first.to_a
        unless ring.last&.object
          cut = Undefined.new(reason: "the image ends inside the descriptor ring")
          return Result.new(ring:, candidates: cut, checkpoint: cut)
        end

        candidates = ring.select(&:superblock?).filter_map { candidate(_1, ring, block0_superblock) }
        Result.new(ring:, candidates:, checkpoint: select(candidates, view))
      end

      def candidate(slot, ring, block0_superblock)
        sb = slot.object.body(Layout::ContainerSuperblock)
        return nil unless sb && Geometry.valid?(sb) && AGREEING.all? { sb.public_send(_1) == block0_superblock.public_send(_1) }

        Candidate.new(slot:, superblock: sb, whole: whole?(slot, sb, ring))
      end

      # The four conditions of a whole descriptor run.
      def whole?(slot, sb, ring)
        size = ring.size
        length = sb.descriptor_length
        return false unless length.between?(2, size) && sb.descriptor_index < size &&
                            (sb.descriptor_index + length - 1) % size == slot.index

        maps = run_maps(sb, ring)
        maps.all? { map_whole?(_1, slot.object.header.xid, sb.block_size) } &&
          maps.map { _1.map_head.flags }.then { _1[...-1].all?(&:zero?) && _1.last == LAST_MAP } &&
          maps.flat_map(&:mappings).all? { _1.byte_size.positive? && (_1.byte_size % sb.block_size).zero? } &&
          data_run_inside?(sb) && sb.next_xid > slot.object.header.xid
      end

      # The mapping blocks of a run: every slot of it but the last.
      def run_maps(sb, ring) = (sb.descriptor_length - 1).times.map { ring[(sb.descriptor_index + _1) % ring.size] }

      def map_whole?(slot, xid, block_size) =
        slot.map? && slot.object.header.xid == xid && 40 + MAPPING_BYTES * slot.map_head.entry_count <= block_size

      def data_run_inside?(sb)
        size = sb.data_blocks
        sb.data_index < size && sb.data_length <= size && sb.descriptor_next < sb.descriptor_blocks && sb.data_next < size
      end

      def select(candidates, view)
        chosen = candidates.select(&:whole).max_by(&:xid)
        return Undefined.new(reason: "no candidate's descriptor run is whole") unless chosen

        sb = chosen.superblock
        reader = Reader.new(view:, block_size: sb.block_size)
        ring = sb.descriptor_blocks
        mappings = (sb.descriptor_length - 1).times.flat_map do
          Slot.new(index: 0, object: reader.object(sb.descriptor_base + ((sb.descriptor_index + _1) % ring))).mappings
        end
        data_run = Run.new(first: sb.data_index, length: sb.data_length, size: sb.data_blocks)
        objects = mappings.to_h { [_1.oid, [_1, mapped_object(reader, _1)]] }
        Checkpoint.new(superblock: sb, xid: chosen.xid, reader:, mappings:, data_run:, objects:)
      end

      def mapped_object(reader, mapping)
        blocks = mapping.byte_size / reader.block_size
        blocks <= OBJECT_BLOCK_BOUND ? reader.object(mapping.address, blocks) : nil
      end
    end
  end
end
