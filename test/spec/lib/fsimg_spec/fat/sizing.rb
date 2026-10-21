# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The volume layout create gives a FAT volume
    # (specs/design-fat-writer.md#geometry-and-allocation), computed with
    # the region arithmetic of the FAT specification (section 3.5) from
    # the creation's variant, settings and tree. The writer's layout is
    # fixed but for the cluster size and count: one reserved sector, or 32
    # on FAT32, two FATs, and on FAT12 and FAT16 a root region of at least
    # 512 entries, rounded up to whole sectors, that holds the root's
    # entries.
    #
    # Each cluster size create may choose, the -o cluster= setting or
    # every power of two from the sector size to 32 KiB in increasing
    # order, gives one Candidate volume. An exact -s volume's cluster count
    # follows from its sectors once the FAT size is known, and the FAT is
    # the smallest that holds an entry for every cluster the rest of the
    # volume leaves, with the two reserved entries. Without -s, the count
    # is the least the variant's range admits that holds the tree's
    # clusters, and the FAT and the volume follow from it. Create writes
    # the first candidate whose count is within the variant's range and
    # holds the tree; under automatic sizing that range keeps MARGIN
    # clusters away from each variant threshold.
    module Sizing
      # The cluster counts each variant can hold (section 3.5, with the
      # largest cluster number below the bad-cluster marker for FAT32's
      # 28-bit entries), and those automatic sizing keeps to, as the writer
      # design states, following Microsoft's formatting recommendation.
      FAT16_LEAST = Geometry::FAT16_MINIMUM_CLUSTERS
      FAT32_LEAST = Geometry::FAT32_MINIMUM_CLUSTERS
      FAT32_MOST = 0x0FFF_FFF5
      RANGES = { "fat12" => 1..(FAT16_LEAST - 1), "fat16" => FAT16_LEAST..(FAT32_LEAST - 1),
                 "fat32" => FAT32_LEAST..FAT32_MOST }.freeze
      MARGIN = 16
      AUTOMATIC_RANGES = {
        "fat12" => 1..(FAT16_LEAST - MARGIN), "fat16" => (FAT16_LEAST + MARGIN)..(FAT32_LEAST - MARGIN),
        "fat32" => (FAT32_LEAST + MARGIN)..FAT32_MOST
      }.freeze
      MIN_ROOT_ENTRIES = 512
      FAT_COUNT = 2
      # The two FAT entries before the first cluster, which is cluster 2.
      RESERVED_ENTRIES = 2
      # The writer's directory headers and their 32-byte slots: the root
      # starts with its volume label, every other directory with . and ..,
      # and each ends with a zero terminator.
      ROOT_HEADER_SLOTS = 2
      DIRECTORY_HEADER_SLOTS = 3
      SLOT_BYTES = 32
      LONG_NAME_UNITS = 13

      # The volume one candidate cluster size gives: its variant, its
      # cluster size, the clusters the tree takes in it and its Geometry.
      # Its count need not fit; fits? says whether create may write it.
      Candidate = Data.define(:variant, :cluster_bytes, :allocated, :geometry) do
        def clusters = geometry.cluster_count

        # Whether the volume's cluster count lies in range and holds the
        # tree, and its BPB fields can count it: a 32-bit total sector
        # count, and on FAT12 and FAT16 a 16-bit FAT size and root entry
        # count (section 3.1).
        def fits?(range)
          fields = geometry.total_sectors <= Rules::MAX_TOTAL_SECTORS &&
                   (variant == "fat32" || (geometry.fat_size <= 0xFFFF && geometry.root_entries <= 0xFFFF))
          fields && clusters.is_a?(Integer) && range.cover?(clusters) && allocated <= clusters
        end
      end

      # The Candidate volumes create tries for source, a SourceModel with
      # legal settings and valid names, one per cluster size in the order
      # create tries them. Under automatic sizing each holds the least
      # count of range that holds the tree.
      def self.candidates(source, range:)
        sector = source.settings.fetch(:sector, Rules::DEFAULT_SECTOR_BYTES)
        root_entries = root_entries(source)
        cluster_sizes(source.settings, sector).map do |cluster_bytes|
          allocated = allocated(source, cluster_bytes)
          shape = { variant: source.format, sector:, cluster_bytes:, root_entries: }
          geometry = if source.settings.key?(:size)
                       exact(total_sectors: source.settings.fetch(:size) / sector, **shape)
                     else
                       counted(clusters: [range.begin, allocated].max, **shape)
                     end
          Candidate.new(variant: source.format, cluster_bytes:, allocated:, geometry:)
        end
      end

      # The Candidate create writes for source, or nil when it must refuse
      # it: the first that fits the variant's range, less the margin under
      # automatic sizing.
      def self.plan(source)
        range = (source.settings.key?(:size) ? RANGES : AUTOMATIC_RANGES).fetch(source.format)
        candidates(source, range:).find { _1.fits?(range) }
      end

      # The quantities of a created volume that the writer's layout fixes,
      # by the names a difference reports.
      COMPARED = {
        "bytes per sector" => :bytes_per_sector, "sectors per cluster" => :sectors_per_cluster,
        "reserved sectors" => :reserved_sectors, "FATs" => :fat_count, "root entries" => :root_entries,
        "sectors per FAT" => :fat_size, "total sectors" => :total_sectors, "clusters" => :cluster_count
      }.freeze

      # Why the decoded Geometry of a created image is not that of planned,
      # the Candidate Sizing.plan gives its source, or nil.
      def self.layout_problem(planned, decoded)
        differences = COMPARED.filter_map do |name, field|
          expected, found = [planned.geometry, decoded].map { _1.public_send(field) }
          "#{name} #{found.inspect} where the writer's sizing gives #{expected}" unless expected == found
        end
        "the image it wrote has #{differences.join(", ")}" if differences.any?
      end

      # The Geometry of a volume of variant holding exactly clusters, with
      # the smallest FAT that has an entry for each; witnesses choose an
      # exact -s with it, by the generation order of
      # specs/design-spec-testing.md#constraint-solving.
      def self.counted(variant:, sector:, cluster_bytes:, root_entries:, clusters:)
        bits = Geometry::ENTRY_BITS.fetch(variant)
        fat_size = (((clusters + RESERVED_ENTRIES) * bits) + (sector * 8) - 1) / (sector * 8)
        geometry(variant:, sector:, cluster_bytes:, root_entries:, fat_size:,
                 total_sectors: reserved_sectors(variant) + region_sectors(root_entries, sector) +
                                (FAT_COUNT * fat_size) + (clusters * (cluster_bytes / sector)))
      end

      # The Geometry of an exact volume of total_sectors, whose FAT is the
      # smallest that holds an entry for each cluster the sectors beside
      # the FATs leave. A larger FAT leaves fewer clusters and holds more,
      # so the first sufficient size is found by bisection. A volume with
      # no room beside its metadata for two FAT sectors keeps a FAT of one
      # sector, and its cluster count is Undefined or zero.
      def self.exact(variant:, sector:, cluster_bytes:, root_entries:, total_sectors:)
        bits = Geometry::ENTRY_BITS.fetch(variant)
        spare = total_sectors - reserved_sectors(variant) - region_sectors(root_entries, sector)
        fat_size = (1..[spare / FAT_COUNT, 1].max).bsearch do |size|
          size * sector * 8 / bits >= ((spare - (FAT_COUNT * size)) / (cluster_bytes / sector)) + RESERVED_ENTRIES
        end
        geometry(variant:, sector:, cluster_bytes:, root_entries:, total_sectors:, fat_size:)
      end

      # The writer's FAT12 and FAT16 root region: the root's slots, at
      # least MIN_ROOT_ENTRIES, rounded up to whole sectors. The FAT32 root
      # is a cluster chain, counted among the allocated clusters.
      def self.root_entries(source)
        return 0 if source.format == "fat32"

        per_sector = source.settings.fetch(:sector, Rules::DEFAULT_SECTOR_BYTES) / SLOT_BYTES
        [(slots(source.tree, nil) + per_sector - 1) / per_sector * per_sector, MIN_ROOT_ENTRIES].max
      end

      # The -o cluster= setting, or every cluster of 1 to 128 sectors
      # (BPB_SecPerClus) the writer's 32 KiB limit admits, smallest first.
      private_class_method def self.cluster_sizes(settings, sector) =
        settings.key?(:cluster) ? [settings.fetch(:cluster)] :
          (0..7).map { sector << _1 }.select { _1 <= Rules::MAX_WRITER_CLUSTER_BYTES }

      # The clusters the tree takes: each file's bytes and each directory's
      # slots in whole clusters, an empty file none, and the FAT32 root's
      # slots beside them.
      private_class_method def self.allocated(source, cluster_bytes)
        clusters = ->(bytes) { (bytes + cluster_bytes - 1) / cluster_bytes }
        children = source.tree.group_by(&:parent)
        root = source.format == "fat32" ? clusters.call(slots(source.tree, nil, children:) * SLOT_BYTES) : 0
        root + source.tree.sum do |entry|
          clusters.call(entry.type == "file" ? entry.size : slots(source.tree, entry.path, children:) * SLOT_BYTES)
        end
      end

      # The slots of directory, nil for the root: its header, and one short
      # entry per child, after the long-name entries of a name that is not
      # an upper-case 8.3 name its short entry spells exactly
      # (specs/design-fat-writer.md#names-and-directory-layout); the
      # independent builder's short names follow the same rule.
      private_class_method def self.slots(tree, directory, children: tree.group_by(&:parent))
        header = directory ? DIRECTORY_HEADER_SLOTS : ROOT_HEADER_SLOTS
        header + children.fetch(directory, []).sum { 1 + long_name_slots(_1.path.rpartition("/".b).last) }
      end

      private_class_method def self.long_name_slots(name)
        return 0 if Builder::ShortNames.plain(name)

        units = name.dup.force_encoding(Encoding::UTF_8).encode(Encoding::UTF_16LE).bytesize / 2
        (units + LONG_NAME_UNITS - 1) / LONG_NAME_UNITS
      end

      private_class_method def self.reserved_sectors(variant) = variant == "fat32" ? 32 : 1

      private_class_method def self.region_sectors(root_entries, sector) =
        ((root_entries * SLOT_BYTES) + sector - 1) / sector

      private_class_method def self.geometry(variant:, sector:, cluster_bytes:, root_entries:, total_sectors:, fat_size:) =
        Geometry.new(bytes_per_sector: sector, sectors_per_cluster: cluster_bytes / sector,
                     reserved_sectors: reserved_sectors(variant), fat_count: FAT_COUNT, root_entries:,
                     total_sectors_16: 0, total_sectors_32: total_sectors, fat_size_16: 0, fat_size_32: fat_size)
    end
  end
end
