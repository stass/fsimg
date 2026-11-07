# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Erofs
    class BuilderError < FixtureError; end

    # Small images with fixed, separated metadata, written from the design's
    # field tables. The model follows the fields it reads, never this map.
    #
    #   byte 0     boot area; the superblock is at byte 1024
    #   byte 4096  metadata: NID n is the slot at 4096 + 32 * n
    #   byte 8192  root directory data
    #   byte 12288 the block of the extended inode's file
    #   byte 16384 the subdirectory's data
    #
    # Metadata slots (NID): 1 root, 2 file.txt (compact, flat-inline),
    # 5 ext.bin (extended, flat-inline), 8 sub, 9 sub/leaf, and with the
    # chunked parameter 12 chunk4.bin and 16 chunk8.bin. Slot 0 is unused
    # so that the byte after the superblock checksum range is padding.
    #
    # chunk4.bin has 4-byte entries and four chunks of one block: the block
    # of ext.bin, a hole, the block of sub's data, and the block of ext.bin
    # again, which the last, short chunk shares. chunk8.bin has 8-byte entries
    # and chunks of two blocks: the two blocks from ext.bin's, and a hole.
    module Builder
      NAME = "erofs-independent"
      MAGIC = 0xE0F5E1E2
      META = 4096
      ROOT = 1
      FILE = 2
      EXT = 5
      SUB = 8
      LEAF = 9
      CHUNK4 = 12
      CHUNK8 = 16
      ROOT_DATA = 8192
      EXT_DATA = 12288
      SUB_DATA = 16384
      LENGTH = 20480
      EPOCH = 1_700_000_000
      EXT_TIME = 1_600_000_000
      FILE_SIZE = 9
      LEAF_SIZE = 7
      TAIL = 5
      CHUNK4_TAIL = 5
      EIGHT_BYTE_ENTRIES = 0x20
      INCOMPAT_CHUNKED_FILE = 4
      HOLE = 0xFFFFFFFF
      DIRECTORY = 0o040755
      REGULAR = 0o100644
      COMPAT_CHECKSUM = 1
      COMPAT_MTIME = 2
      # An attribute region of two inline entries, user.a = "b" and
      # user.c = "d": 12 + 4 * (5 - 1) bytes.
      ATTRIBUTE_COUNT = 5
      PARAMETERS = %i[block_bits checksum xattr subdirectory chunked].freeze
      DEFAULTS = { block_bits: 12, checksum: 1, xattr: 0, subdirectory: 0, chunked: 0 }.freeze

      module_function

      def options(parameters)
        unknown = parameters.keys - PARAMETERS
        raise BuilderError, "unknown EROFS builder parameters #{unknown.inspect}" unless unknown.empty?

        DEFAULTS.merge(parameters).tap do |selected|
          raise BuilderError, "EROFS block_bits is outside 9..12" unless selected[:block_bits].between?(9, 12)
          raise BuilderError, "EROFS flags must be 0 or 1" unless
            selected.values_at(:checksum, :xattr, :subdirectory, :chunked).all? { [0, 1].include?(_1) }
        end.freeze
      end

      def tree(parameters)
        selected = options(parameters)
        block = 1 << selected[:block_bits]
        [*chunk_entries(selected, block),
         TreeEntry.file("ext.bin", size: block + TAIL, content: "zero"),
         TreeEntry.file("file.txt", size: FILE_SIZE, content: "zero"),
         *(selected[:subdirectory] == 1 ? [TreeEntry.directory("sub"), TreeEntry.file("sub/leaf", size: LEAF_SIZE, content: "zero")] : [])].freeze
      end

      # The chunk-based files of a chunked volume, none otherwise.
      def chunk_entries(selected, block)
        return [] unless selected[:chunked] == 1

        [TreeEntry.file("chunk4.bin", size: 3 * block + CHUNK4_TAIL, content: "zero"),
         TreeEntry.file("chunk8.bin", size: 3 * block, content: "zero")]
      end

      def write(path, parameters:, tree:, max_bytes:)
        raise BuilderError, "EROFS fixture exceeds the image ceiling" if LENGTH > max_bytes
        raise BuilderError, "EROFS fixture tree disagrees with its profile" unless tree == Builder.tree(parameters)

        selected = options(parameters)
        image = "".b.ljust(LENGTH, "\0".b)
        slots(selected).each { |nid, bytes| image[META + 32 * nid, bytes.bytesize] = bytes }
        directories(selected).each { |at, bytes| image[at, bytes.bytesize] = bytes }
        image[1024, 128] = superblock(selected)
        image[1024, 128] = superblock(selected, checksum: Crc32c.stored(checksum_range(image, selected[:block_bits]))) if selected[:checksum] == 1
        File.binwrite(path, image, mode: "wbx", perm: 0o600)
      end

      # The bytes the checksum covers, with the checksum field read as zero
      # (design: Superblock checksum).
      def checksum_range(image, block_bits)
        block = 1 << block_bits
        last = block > 1024 ? block : 1024 + block
        image.byteslice(1024, last - 1024).b.tap { _1[4, 4] = "\0\0\0\0".b }
      end

      def superblock(selected, checksum: 0)
        block = 1 << selected[:block_bits]
        Layout::Superblock.new(
          magic: MAGIC, checksum:, compat: COMPAT_MTIME | (selected[:checksum] == 1 ? COMPAT_CHECKSUM : 0),
          blkszbits: selected[:block_bits], root_nid: ROOT, inode_count: 3 + 2 * selected[:subdirectory] + 2 * selected[:chunked],
          epoch: EPOCH, blocks: LENGTH / block, meta_blkaddr: META / block, volume_name: "fsimg-spec",
          incompat: selected[:chunked] == 1 ? INCOMPAT_CHUNKED_FILE : 0
        ).to_binary_s
      end

      def compact(mode:, size:, layout:, layout_word:, links: 1, xattr_count: 0)
        Layout::CompactInode.new(format: layout << 1, xattr_count:, mode:, nlink: links, size_bytes: size, layout_word:).to_binary_s
      end

      def extended(mode:, size:, layout:, layout_word:)
        Layout::ExtendedInode.new(format: (layout << 1) | 1, mode:, size_bytes: size, layout_word:, time: EXT_TIME, nsec: 5, nlink: 1).to_binary_s
      end

      # The attribute region of file.txt: a header, then the two entries.
      def attributes = Layout::AttributeHeader.new.to_binary_s + attribute("a", "b") + attribute("c", "d")

      # A user. entry padded to a multiple of four bytes.
      def attribute(suffix, value) =
        (Layout::AttributeEntry.new(suffix_length: suffix.bytesize, name_index: 1, value_length: value.bytesize).to_binary_s + suffix.b + value.b).then { _1.ljust((_1.bytesize + 3) & ~3, "\0".b) }

      # A chunk index entry: the high part, the device identifier and the
      # block number of an 8-byte entry, or the block number of a 4-byte one.
      def chunk_entry(block, width:) = width == 8 ? [0, 0, block].pack("v2V") : [block].pack("V")

      # The chunk-based files. Blocks are counted from the start of the image.
      def chunk_slots(selected)
        block = 1 << selected[:block_bits]
        index4 = [EXT_DATA / block, HOLE, SUB_DATA / block, EXT_DATA / block].map { chunk_entry(_1, width: 4) }.join
        index8 = [EXT_DATA / block, HOLE].map { chunk_entry(_1, width: 8) }.join
        { CHUNK4 => compact(mode: REGULAR, size: 3 * block + CHUNK4_TAIL, layout: 4, layout_word: 0) + index4,
          CHUNK8 => compact(mode: REGULAR, size: 3 * block, layout: 4, layout_word: EIGHT_BYTE_ENTRIES | 1) + index8 }
      end

      def slots(selected)
        block = 1 << selected[:block_bits]
        sub = selected[:subdirectory] == 1
        file_attributes = selected[:xattr] == 1
        {
          ROOT => compact(mode: DIRECTORY, size: root_entries(selected).sum { 12 + _1.first.bytesize }, layout: 0,
                          layout_word: ROOT_DATA / block, links: sub ? 3 : 2),
          FILE => compact(mode: REGULAR, size: FILE_SIZE, layout: 2, layout_word: 0xFFFFFFFF,
                          xattr_count: file_attributes ? ATTRIBUTE_COUNT : 0) +
                  (file_attributes ? attributes : "".b) + "\0".b * FILE_SIZE,
          EXT => extended(mode: REGULAR, size: block + TAIL, layout: 2, layout_word: EXT_DATA / block) + "\0".b * TAIL,
          **(sub ? {
            SUB => compact(mode: DIRECTORY, size: sub_entries.sum { 12 + _1.first.bytesize }, layout: 0,
                           layout_word: SUB_DATA / block, links: 2),
            LEAF => compact(mode: REGULAR, size: LEAF_SIZE, layout: 2, layout_word: 0xFFFFFFFF) + "\0".b * LEAF_SIZE
          } : {}),
          **(selected[:chunked] == 1 ? chunk_slots(selected) : {})
        }
      end

      def directories(selected)
        sub = selected[:subdirectory] == 1
        { ROOT_DATA => directory_block(root_entries(selected)), **(sub ? { SUB_DATA => directory_block(sub_entries) } : {}) }
      end

      # [name, nid, file type], in byte order.
      def root_entries(selected) = [[".", ROOT, 2], ["..", ROOT, 2],
                                    *(selected[:chunked] == 1 ? [["chunk4.bin", CHUNK4, 1], ["chunk8.bin", CHUNK8, 1]] : []),
                                    ["ext.bin", EXT, 1], ["file.txt", FILE, 1],
                                    *(selected[:subdirectory] == 1 ? [["sub", SUB, 2]] : [])]
      def sub_entries = [[".", SUB, 2], ["..", ROOT, 2], ["leaf", LEAF, 1]]

      # The entry array, then the names concatenated.
      def directory_block(entries)
        starts = entries.map { _1.first.bytesize }.each_with_object([12 * entries.size]) { |length, all| all << all.last + length }
        array = entries.zip(starts).map do |(_, nid, type), start|
          Layout::DirectoryEntry.new(nid:, name_offset: start, file_type: type).to_binary_s
        end
        (array + entries.map { _1.first.b }).join.b
      end
    end
  end
end
