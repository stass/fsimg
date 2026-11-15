# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The object kinds of [A1] the container level names (design: Object
    # header and checksum), and the storage classes and flags of the type
    # word.
    KINDS = { superblock: 1, root_node: 2, node: 3, space_manager: 5, address_block: 6, chunk_block: 7,
              object_map: 11, checkpoint_map: 12, volume: 13, reaper: 17, reap_list: 18 }.freeze
    VIRTUAL = 0
    EPHEMERAL = 0x8000_0000
    PHYSICAL = 0x4000_0000
    STORAGE = 0xC000_0000
    ENCRYPTED = 0x1000_0000
    NONPERSISTENT = 0x0800_0000
    MAGIC = "NXSB".b.freeze
    # The probe inspects at most the largest legal block (design: Probe and
    # identity).
    PROBE_BYTES = 65_536

    # One object as read from the image: the block it starts at, its bytes,
    # its decoded header and whether its stored checksum is the one its
    # bytes compute.
    ObjectView = Data.define(:address, :bytes, :header, :sealed) do
      def kind = header.object_type & 0xFFFF
      def storage = header.object_type & STORAGE

      # Whether the type word is exactly kind in storage class, no flags.
      def typed?(kind, storage) = header.object_type == (KINDS.fetch(kind) | storage)

      def body(record, at: Layout::BODY) = Layout.find(record, bytes, at:)

      # Whether this is a valid object of kind and storage at the given XID
      # bound with OID oid: the header rules of the design's Object header
      # and checksum section.
      def valid?(kind:, storage:, oid:, xid:) = sealed && typed?(kind, storage) && header.oid == oid && header.xid.between?(1, xid)
    end

    # Reads whole blocks of the image view. Blocks the image does not hold
    # read as nil, so no decoder raises on a truncated image.
    Reader = Data.define(:view, :block_size) do
      def blocks(address, count = 1)
        length = count * block_size
        return nil if address.negative? || count < 1

        bytes = view.read(address * block_size, length)
        bytes.bytesize == length ? bytes : nil
      end

      def object(address, count = 1)
        bytes = blocks(address, count)
        bytes && ObjectView.new(address:, bytes:, header: Layout.find(Layout::ObjectHeader, bytes),
                                sealed: Fletcher64.valid?(bytes))
      end
    end

    # The geometry rules of a container superblock (design: Container
    # superblock), shared by block 0's rules and by checkpoint candidates.
    module Geometry
      MINIMUM_BYTES = 1 << 20
      BLOCK_SIZES = (12..16).map { 1 << _1 }.freeze
      MAX_VOLUMES = 100
      NONCONTIGUOUS = 0x8000_0000

      module_function

      def block_size?(sb) = BLOCK_SIZES.include?(sb.block_size)

      # N * B fits 64 bits and N is at least 1 MiB worth of blocks.
      def block_count?(sb) = sb.block_count * sb.block_size < (1 << 64) && sb.block_count * sb.block_size >= MINIMUM_BYTES

      def max_volumes?(sb) = sb.max_volumes.between?(1, MAX_VOLUMES) && sb.volume_oids.drop(sb.max_volumes).all?(&:zero?)

      def contiguous?(sb) = [sb.descriptor_blocks, sb.data_blocks].none? { _1.anybits?(NONCONTIGUOUS) }

      # The descriptor and data areas lie inside [1, N) and do not overlap.
      def areas?(sb)
        areas = [[sb.descriptor_base, sb.descriptor_blocks & ~NONCONTIGUOUS], [sb.data_base, sb.data_blocks & ~NONCONTIGUOUS]]
        inside = areas.all? { |base, count| count.positive? && base >= 1 && base + count <= sb.block_count }
        (first, first_count), (second, second_count) = areas
        inside && (first + first_count <= second || second + second_count <= first)
      end

      def valid?(sb) = block_size?(sb) && block_count?(sb) && max_volumes?(sb) && contiguous?(sb) && areas?(sb)
    end

    # The container as its own decoder reads it, every part total: a part
    # it could not read is Undefined with the reason. block0 is the block-0
    # object, superblock its decoded body; ring, candidates and checkpoint
    # are the checkpoint selection's (Checkpoints); object_map and
    # object_map_tree the container object map and its tree; volumes the
    # resolution of each volume slot; filesystems the Volume each resolved
    # slot holds; space the space manager, its chunk structures, free queues
    # and reaper (Space); tree the known tree of the one volume's namespace
    # (Namespace), which decode walks only when asked, since only an image
    # whose stages extract it needs one.
    class Model < Data.define(:image_length, :magic, :block0, :superblock, :ring, :candidates, :checkpoint,
                              :object_map, :object_map_tree, :volumes, :filesystems, :space, :tree)
      # The bound on nodes one tree walk visits and on chunk structures,
      # far above every native capture's; a walk that reaches one leaves
      # the scope that reads the rest unclassified.
      NODE_BOUND = 4096
      CHUNK_BLOCK_BOUND = 1 << 16

      UNWALKED = Undefined.new(reason: "the namespace was not walked")

      def self.decode(view, namespace: false)
        head = view.read(0, [view.length, PROBE_BYTES].min)
        superblock = Layout.find(Layout::ContainerSuperblock, head, at: Layout::BODY) ||
                     Undefined.new(reason: "the image ends inside the block-0 superblock")
        block0 = block0(view, head, superblock)
        checkpoints = Checkpoints.decode(view, superblock, block0)
        checkpoint = checkpoints.checkpoint
        object_map, tree, volumes = object_map_parts(checkpoint)
        filesystems = filesystems(checkpoint, volumes)
        new(image_length: view.length, magic: head.byteslice(32, 4) || "".b, block0:, superblock:,
            ring: checkpoints.ring, candidates: checkpoints.candidates, checkpoint:, object_map:,
            object_map_tree: tree, volumes:, filesystems:, space: Space.decode(checkpoint),
            tree: namespace ? namespace(checkpoint, filesystems) : UNWALKED)
      end

      # The namespace of the container's one volume, the writer's profile.
      def self.namespace(checkpoint, filesystems)
        return Undefined.new(reason: "the container resolves #{filesystems.size} volumes, not one") unless filesystems.one?

        Namespace.decode(filesystems.first, checkpoint.reader)
      end

      def self.block0(view, head, superblock)
        return superblock if FsimgSpec.undefined?(superblock)
        return Undefined.new(reason: "block size #{superblock.block_size} is no legal block size") unless Geometry.block_size?(superblock)
        return Undefined.new(reason: "the image is shorter than one block") if head.bytesize < superblock.block_size

        Reader.new(view:, block_size: superblock.block_size).object(0)
      end

      # The container object map of the selected checkpoint, its tree and
      # the resolution of every volume slot through it.
      def self.object_map_parts(checkpoint)
        return Array.new(3, checkpoint) if FsimgSpec.undefined?(checkpoint)

        object = checkpoint.reader.object(checkpoint.superblock.object_map_oid)
        valid = object&.valid?(kind: :object_map, storage: PHYSICAL, oid: object.address, xid: checkpoint.xid)
        return [object || Undefined.new(reason: "the object map lies past the image"), *Array.new(2, Undefined.new(reason: "no valid object map"))] unless valid

        tree = BTree.walk(BTree::OBJECT_MAP, object.body(Layout::ObjectMap).tree_oid, checkpoint:, bound: NODE_BOUND)
        [object, tree, VolumeResolution.resolve(checkpoint, tree)]
      end

      def self.filesystems(checkpoint, volumes)
        return [] if FsimgSpec.undefined?(volumes)

        volumes.select(&:resolved?).map { Volume.decode(checkpoint, _1, bound: NODE_BOUND) }
      end

      private_class_method :block0, :object_map_parts, :filesystems, :namespace

      def object_map_fields = object_map.body(Layout::ObjectMap)

      RECORD_SCOPES = %w[directories files verification].freeze

      # The scopes a reached bound or an unreadable part left undecided. A
      # volume object map read only in part leaves open undecided and every
      # scope that reads records, since the nodes it did not map read as
      # missing. So does a sealed volume, whose profile is not modelled, and
      # a filesystem tree read only in part leaves the record scopes
      # undecided; so do a hashed volume's names when the harness's Unicode
      # tables are not the design's, since the hash rules cannot judge them.
      def unclassified_scopes
        ring_bounded = FsimgSpec.undefined?(ring) && ring.reason == Checkpoints::RING_BOUNDED
        partial_maps = filesystems.select(&:map_decoded?).reject { _1.object_map_tree.complete }
        open = ring_bounded || partial_object_map? || !partial_maps.empty? || filesystems.any?(&:sealed?) ? ["open"] : []
        unread = !partial_maps.empty? || filesystems.any? { _1.sealed? || (_1.tree_decoded? && !_1.tree.complete) }
        unjudged_names = !Names::TABLES_MATCH && filesystems.any? { _1.tree_decoded? && _1.hashed? }
        record_scopes = unread ? RECORD_SCOPES : []
        names = unjudged_names ? ["directories"] : []
        verification = !FsimgSpec.undefined?(space) && !space.complete ? ["verification"] : []
        open | record_scopes | names | verification
      end

      def partial_object_map? = !FsimgSpec.undefined?(object_map_tree) && !object_map_tree.complete

      def record_problems(_result) = []
    end

    # Resolution through a decoded object-map tree (design: Object maps):
    # the greatest key not above (oid, xid), a miss when it names another
    # OID. A ghost record, which no object-map tree may hold, is a miss.
    module ObjectMapLookup
      module_function

      # The [key, value] record the lookup lands on, or nil for a miss.
      def find(tree, oid, xid)
        bound = [oid, xid]
        found = tree.records.select { |key, _| ([key.oid, key.xid] <=> bound) <= 0 }.max_by { |key, _| [key.oid, key.xid] }
        found && found.first.oid == oid && found.last != :ghost ? found : nil
      end

      # A found record that is not deleted, whose object is the one read.
      def live?(found) = !found.last.flags.anybits?(BTree::DELETED)

      # Whether object is a valid virtual object of kind with the OID the
      # lookup asked for and its record's XID.
      def resolved?(found, object, kind)
        return false unless found && live?(found) && object

        key = found.first
        object.sealed && object.typed?(kind, VIRTUAL) && object.header.oid == key.oid && object.header.xid == key.xid
      end
    end

    # The object a volume slot of the selected superblock resolves to
    # through the container object map at the checkpoint's XID: found is
    # the record the lookup lands on, or nil for a miss, and object what
    # its address holds.
    VolumeResolution = Data.define(:oid, :found, :object) do
      def self.resolve(checkpoint, tree)
        sb = checkpoint.superblock
        sb.volume_oids.take([sb.max_volumes, Geometry::MAX_VOLUMES].min).reject(&:zero?).map do |oid|
          found = ObjectMapLookup.find(tree, oid, checkpoint.xid)
          new(oid:, found:, object: found && checkpoint.reader.object(found.last.address))
        end
      end

      # The slot index the volume array gives this OID.
      def slot(superblock) = superblock.volume_oids.index(oid)

      # The design's resolution: a live record whose object is a valid
      # virtual volume superblock with OID oid and the record's XID.
      def resolved? = ObjectMapLookup.resolved?(found, object, :volume)
    end
  end
end
