# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"
require "set"

module FsimgSpec
  module Apfs
    # A namespace the walk cannot follow: a directory record naming no
    # inode, a cycle, an extent past the image, an entry type the walk does
    # not know. The record rules judge such a tree; the walk only says why
    # it has nothing to compare.
    class NamespaceError < Error; end

    # The namespace of one decoded volume as a known tree (KnownTree): the
    # walk from the root directory, inode 2, through the directory records
    # (design: Directory records and names). An entry's type is its
    # directory record's, which must agree with its inode's mode; a file's
    # size is its data-stream field's and its SHA-256 is over the file
    # extents its private ID owns (design: Data streams and file extents);
    # a symbolic link's size and digest are its target's, the inline
    # com.apple.fs.symlink attribute without its NUL (design: Symbolic
    # links). The walk is total: a namespace it cannot follow is Undefined
    # with the reason.
    #
    # It walks the writer's profile, which emits no hard links, clones or
    # holes (design: Profile), and refuses each: an inode named twice, a
    # hole extent, or file bytes beyond the image's length in all. So every
    # inode is visited once, which also ends any cycle, the entries are
    # bounded by the records, and the bytes digested by the image.
    module Namespace
      ROOT = 2
      # Directory-record types, the high nibble of the inode's mode.
      TYPES = { 1 => "fifo", 4 => "directory", 8 => "file", 10 => "symlink" }.freeze
      SYMLINK_ATTRIBUTE = "com.apple.fs.symlink".b.freeze
      EMBEDDED = 0x2
      MODE_TYPE_SHIFT = 12
      # The blocks one read of file data takes.
      CHUNK_BLOCKS = 256
      # The deepest directory the walk enters, far below Ruby's stack.
      MAX_DEPTH = 256

      # The records the walk reads, grouped once: extents by owner and
      # attributes by owner, each attribute a pair [Key, value bytes];
      # visited, the inode numbers walked so far, and budget, the file bytes
      # left to digest. Both are the walk's own and change as it goes.
      Walk = Data.define(:records, :reader, :extents, :attributes, :visited, :budget)

      # The file bytes a walk may still digest.
      class Budget
        def initialize(bytes) = @bytes = bytes

        def spend(bytes, path)
          raise NamespaceError, "the files up to #{FsimgSpec.shown(path)} hold more bytes than the image" if bytes > @bytes

          @bytes -= bytes
        end
      end

      module_function

      # The known tree of volume, read through reader, or Undefined.
      def decode(volume, reader)
        return Undefined.new(reason: "the volume's records are undecoded") if FsimgSpec.undefined?(volume.records)

        records = volume.records
        attributes = records.pairs.select { _1.first.kind == Records::ATTRIBUTE }.group_by { _1.first.owner }
        walk = Walk.new(records:, reader:, extents: records.extents.group_by(&:owner), attributes:, visited: Set[ROOT],
                        budget: Budget.new(reader.view.length))
        children(walk, parent: ROOT, prefix: "".b, depth: 0).sort_by(&:path).freeze
      rescue NamespaceError => e
        Undefined.new(reason: e.message)
      end

      # The entries beneath directory parent, whose path is prefix.
      def children(walk, parent:, prefix:, depth:)
        raise NamespaceError, "the namespace is deeper than #{MAX_DEPTH} directories" if depth > MAX_DEPTH

        walk.records.children_of(parent).flat_map do |record|
          name = record.key.name or raise NamespaceError, "a directory record of inode #{parent} has no name"
          path = prefix + name
          raise NamespaceError, "#{FsimgSpec.shown(path)} names inode #{record.child}, already named" unless walk.visited.add?(record.child)

          entry = entry(walk, record, path)
          next [entry] unless entry.type == "directory"

          [entry, *children(walk, parent: record.child, prefix: path + "/", depth: depth + 1)]
        end
      end

      def entry(walk, record, path)
        shown = FsimgSpec.shown(path)
        inode = walk.records.inode(record.child)
        raise NamespaceError, "#{shown} names inode #{record.child}, which has no inode record" unless inode&.fields

        type = TYPES[record.type] or raise NamespaceError, "#{shown} has directory-record type #{record.type}"
        mode_type = inode.fields.mode >> MODE_TYPE_SHIFT
        raise NamespaceError, "#{shown} is a #{type} whose inode mode has type #{mode_type}" unless mode_type == record.type

        case type
        in "directory" | "fifo" then WalkedEntry.new(path:, type:, size: nil, sha256: nil)
        in "file" then file(walk, inode, path)
        in "symlink" then link(walk, inode, path)
        end
      end

      # A file: its data-stream size, or zero without the field, and the
      # digest of that many bytes of its extents.
      def file(walk, inode, path)
        size = walk.records.data_stream(inode)&.logical_size || 0
        extents = walk.extents.fetch(inode.fields.private_id, []).sort_by(&:offset)
        digest = Digest::SHA256.new
        covered = extents.reduce(0) do |offset, extent|
          raise NamespaceError, "#{FsimgSpec.shown(path)} has an extent at byte #{extent.offset}, not #{offset}" unless extent.offset == offset
          raise NamespaceError, "#{FsimgSpec.shown(path)} has a hole, which the writer never emits" if extent.hole?

          feed(digest, walk, extent:, length: [extent.length, size - offset].min, path:) if offset < size
          offset + extent.length
        end
        raise NamespaceError, "the extents of #{FsimgSpec.shown(path)} cover #{covered} of its #{size} bytes" if covered < size

        WalkedEntry.new(path:, type: "file", size:, sha256: digest.hexdigest)
      end

      # Feeds the first length bytes of extent into digest, CHUNK_BLOCKS
      # blocks at a time, from the walk's budget.
      def feed(digest, walk, extent:, length:, path:)
        walk.budget.spend(length, path)
        reader = walk.reader
        step = CHUNK_BLOCKS * reader.block_size
        (0...length).step(step).each do |start|
          wanted = [step, length - start].min
          bytes = reader.blocks(extent.fields.physical_block + start / reader.block_size, wanted.ceildiv(reader.block_size))
          raise NamespaceError, "an extent of #{FsimgSpec.shown(path)} lies past the image" if bytes.nil?

          digest << bytes.byteslice(0, wanted)
        end
      end

      # A symbolic link: its inline target attribute, u16 flags and u16
      # length, then the target and its one NUL.
      def link(walk, inode, path)
        found = walk.attributes.fetch(inode.number, []).find { |key, _| key.name == SYMLINK_ATTRIBUTE }
        raise NamespaceError, "symbolic link #{FsimgSpec.shown(path)} has no target attribute" unless found

        value = found.last
        raise NamespaceError, "symbolic link #{FsimgSpec.shown(path)} has a target attribute shorter than its header" if value.bytesize < 4

        flags, length = value.unpack("vv")
        data = value.byteslice(4..)
        unless flags.anybits?(EMBEDDED) && data.bytesize == length && data.end_with?("\0") && length > 1
          raise NamespaceError, "symbolic link #{FsimgSpec.shown(path)} has no inline NUL-terminated target"
        end

        target = data.byteslice(0, length - 1)
        WalkedEntry.new(path:, type: "symlink", size: target.bytesize, sha256: Digest::SHA256.hexdigest(target))
      end

      private_class_method :children, :entry, :file, :feed, :link
    end
  end
end
