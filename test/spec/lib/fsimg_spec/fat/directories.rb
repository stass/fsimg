# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The directories of a FAT volume, walked by the harness's own reading
    # of the specification (sections 4 and 6), and where each one's end
    # lies. A directory is the FAT12/16 root region, or a chain of clusters
    # the FAT links: the FAT32 root from BPB_RootClus, and each
    # subdirectory from its short entry's first cluster. An entry whose
    # DIR_Name[0] is 0x00 ends the directory: it and every entry after it
    # are free (section 6, DIR_Name).
    #
    # The walk reads the volume where its BPB places it, so on a BPB whose
    # fields shift the layout it reports what lies at those places. It is
    # total, and bounded by the image: it reads the active FAT entry by
    # entry, follows no chain longer than a directory may be, claims each
    # directory cluster once and holds one directory at a time, so an
    # image cannot make it loop or read any cluster twice. A volume whose
    # directories it cannot follow gives Undefined: a BPB whose FAT32
    # extension disagrees with the variant, an active FAT the volume does
    # not have, a broken or cyclic chain, a cluster two directories share
    # or a short image. Such an image has other faults, and the scope that
    # reads its directories is unclassified.
    module Directories
      # One directory: its path, "" for the root and otherwise the short
      # names from the root joined by "/", and the offset within the
      # directory of the first byte that is not zero from its end marker
      # on, or nil when every such byte is zero or the directory has no end
      # marker.
      Directory = Data.define(:path, :stale_at)

      # A directory holds at most 65536 entries (section 6), 2 MiB.
      MAX_DIRECTORY_BYTES = 65_536 * Geometry::DIRECTORY_ENTRY_BYTES
      ATTR_DIRECTORY = 0x10
      ATTR_VOLUME_ID = 0x08
      ATTR_LONG_NAME = 0x0F
      ATTR_LONG_NAME_MASK = 0x3F
      FREE_ENTRY = 0xE5
      # The smallest FAT entry value that ends a chain, by variant, and the
      # bad-cluster mark below it, which no chain links to (section 4).
      END_OF_CHAIN = { "fat12" => 0xFF8, "fat16" => 0xFFF8, "fat32" => 0x0FFF_FFF8 }.freeze
      BAD_CLUSTER = { "fat12" => 0xFF7, "fat16" => 0xFFF7, "fat32" => 0x0FFF_FFF7 }.freeze
      FAT32_ENTRY_MASK = 0x0FFF_FFFF
      # Mirroring disabled, and the active FAT, in BPB_ExtFlags (section 3.3).
      NO_MIRRORING = 0x80
      ACTIVE_FAT = 0x0F
      ENTRY = /.{#{Geometry::DIRECTORY_ENTRY_BYTES}}/mn
      NONZERO = /[^\0]/n

      # Raised inside the walk where the volume's directories cannot be
      # followed; decode turns it into Undefined.
      class Unfollowable < Error; end

      # Every directory of the volume in image, an ImageView, whose boot
      # sector decoded to common, extension and geometry, with
      # fat32_extension true when the extension is FAT32's; Undefined with
      # any of them, or where the walk cannot follow the directories.
      def self.decode(image, common:, fat32_extension:, extension:, geometry:)
        FsimgSpec.derive(common, fat32_extension, extension, geometry.variant, geometry.first_data_sector,
                         geometry.fat_size) do
          raise Unfollowable, "the BPB's extension disagrees with the #{geometry.variant} cluster count" unless
            fat32_extension == (geometry.variant == "fat32")

          Walk.new(image, common:, extension:, geometry:).directories.freeze
        end
      rescue Unfollowable => e
        Undefined.new(reason: "the directories cannot be followed: #{e.message}")
      end

      # One walk of a volume's directories, breadth first from the root.
      # The FAT32 root is a chain from BPB_RootClus; the FAT12/16 root is
      # BPB_RootEntCnt entries from the sector after the FATs.
      class Walk
        def initialize(image, common:, extension:, geometry:)
          @image = image
          @geometry = geometry
          @variant = geometry.variant
          @fat32 = @variant == "fat32"
          @sector = common.bpb_byts_per_sec
          @sectors_per_cluster = common.bpb_sec_per_clus
          @fat_offset = (common.bpb_rsvd_sec_cnt + (active_fat(common, extension) * geometry.fat_size)) * @sector
          @root_cluster = @fat32 ? extension.bpb_root_clus : nil
          @root_offset = (geometry.first_data_sector - geometry.root_dir_sectors) * @sector
          @root_entries = common.bpb_root_ent_cnt
          @links = 2..[geometry.max_cluster, BAD_CLUSTER.fetch(@variant) - 1].min
          @claimed = {}
          raise Unfollowable, "a cluster holds no bytes" unless cluster_bytes.positive?
        end

        # Each directory, the root first, as a Directory. A subdirectory's
        # chain is read only when its turn comes, so the walk holds the
        # bytes of one directory at a time.
        def directories
          pending = [["".b, nil]]
          found = []
          until pending.empty?
            path, cluster = pending.shift
            stale_at, children = scan(path.empty? ? root_bytes : chain_bytes(cluster))
            found << Directory.new(path: path.freeze, stale_at:)
            pending.concat(children.map { |name, first| [path.empty? ? name : "#{path}/#{name}".b, first] })
          end
          found
        end

        private

        def cluster_bytes = @sector * @sectors_per_cluster

        # The FAT the volume reads: on FAT32 with mirroring disabled, the
        # one BPB_ExtFlags names, which must be one of its FATs; otherwise
        # the first.
        def active_fat(common, extension)
          return 0 unless @fat32 && extension.bpb_ext_flags.anybits?(NO_MIRRORING)

          active = extension.bpb_ext_flags & ACTIVE_FAT
          raise Unfollowable, "BPB_ExtFlags names FAT #{active} of #{common.bpb_num_fats}" unless
            active < common.bpb_num_fats

          active
        end

        def root_bytes =
          @fat32 ? chain_bytes(@root_cluster) : read(@root_offset, @root_entries * Geometry::DIRECTORY_ENTRY_BYTES)

        # The bytes of the directory chain from cluster, each of whose
        # clusters no other directory claimed, which must end within the
        # largest directory.
        def chain_bytes(cluster)
          clusters = []
          until cluster.nil?
            raise Unfollowable, "cluster #{cluster} is no data cluster" unless @links.cover?(cluster)
            raise Unfollowable, "directory cluster #{cluster} is claimed twice" if @claimed[cluster]
            raise Unfollowable, "a directory chain is longer than #{MAX_DIRECTORY_BYTES} bytes" if
              (clusters.size + 1) * cluster_bytes > [MAX_DIRECTORY_BYTES, cluster_bytes].max

            @claimed[cluster] = true
            clusters << cluster
            cluster = successor(cluster)
          end
          clusters.map { read(cluster_offset(_1), cluster_bytes) }.join.b
        end

        # The cluster after cluster in its chain, or nil at its end.
        def successor(cluster)
          entry = fat_entry(cluster)
          return nil if entry >= END_OF_CHAIN.fetch(@variant)
          raise Unfollowable, "cluster #{cluster} links to #{entry}" unless @links.cover?(entry)

          entry
        end

        # FAT entry cluster of the active FAT (section 4): 12 bits from the
        # two bytes at cluster * 3 / 2, the high ones for an odd cluster;
        # 16 bits; or the low 28 of 32 bits.
        def fat_entry(cluster)
          case @variant
          in "fat12"
            pair = read(@fat_offset + (cluster * 3 / 2), 2).unpack1("v")
            cluster.odd? ? pair >> 4 : pair & 0xFFF
          in "fat16" then read(@fat_offset + (cluster * 2), 2).unpack1("v")
          in "fat32" then read(@fat_offset + (cluster * 4), 4).unpack1("V") & FAT32_ENTRY_MASK
          end
        end

        def cluster_offset(cluster) = (@geometry.first_data_sector + ((cluster - 2) * @sectors_per_cluster)) * @sector

        # count bytes at offset, all of which the image must hold.
        def read(offset, count)
          bytes = @image.read(offset, count)
          raise Unfollowable, "the image ends before byte #{offset + count}" if bytes.bytesize < count

          bytes
        end

        # The offset of the first byte that is not zero from the end marker
        # on, or nil, and each subdirectory listed before the marker, as its
        # short name and first cluster.
        def scan(bytes)
          entries = bytes.scan(ENTRY)
          marker = entries.index { _1.getbyte(0).zero? }
          stale_at = marker && bytes.index(NONZERO, marker * Geometry::DIRECTORY_ENTRY_BYTES)
          [stale_at, entries.first(marker || entries.size).filter_map { subdirectory(_1) }]
        end

        # The short name and first cluster of a live subdirectory entry,
        # or nil for any other entry: a free one, a long-name entry, the
        # volume label, a file, or a dot entry. FAT12 and FAT16 keep no high
        # cluster word.
        def subdirectory(entry)
          attributes = entry.getbyte(11)
          return nil if entry.getbyte(0) == FREE_ENTRY || (attributes & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME
          return nil if attributes.anybits?(ATTR_VOLUME_ID) || attributes.nobits?(ATTR_DIRECTORY)

          name = short_name(entry.byteslice(0, 11))
          return nil if [".", ".."].include?(name)

          high = @fat32 ? entry.byteslice(20, 2).unpack1("v") : 0
          [name.b, (high << 16) | entry.byteslice(26, 2).unpack1("v")]
        end

        # An 8.3 name as a path component: its base and extension without
        # padding, joined by a period when it has an extension.
        def short_name(raw)
          base = raw.byteslice(0, 8).sub(/ +\z/n, "")
          extension = raw.byteslice(8, 3).sub(/ +\z/n, "")
          extension.empty? ? base : "#{base}.#{extension}"
        end
      end
    end
  end
end
