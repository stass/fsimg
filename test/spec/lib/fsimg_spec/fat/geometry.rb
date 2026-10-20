# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The variant a data-cluster count selects (section 3.5): below 4085
    # clusters is FAT12, below 65525 is FAT16, and 65525 or more is FAT32.
    # The type label never decides it.
    def self.variant_of(clusters)
      if clusters < Geometry::FAT16_MINIMUM_CLUSTERS then "fat12"
      elsif clusters < Geometry::FAT32_MINIMUM_CLUSTERS then "fat16"
      else "fat32"
      end
    end

    # The quantities the Microsoft FAT specification derives from the BPB
    # (section 3.5, "Determination of FAT type when mounting the volume",
    # and section 4.1, "Determination of FAT entry for a cluster"), written
    # from that document. No method raises: a zero divisor, a region larger
    # than the volume or an undefined field makes the result Undefined, so
    # a method that answers a yes or no can also answer Undefined and has
    # no ? suffix.
    #
    # The fields hold BPB values as decoded; fat_size_32 is Undefined when
    # the boot sector carries the FAT12/16 extension, which has no BPB_FATSz32.
    class Geometry < Data.define(:bytes_per_sector, :sectors_per_cluster, :reserved_sectors, :fat_count,
                                 :root_entries, :total_sectors_16, :total_sectors_32, :fat_size_16, :fat_size_32)
      FAT16_MINIMUM_CLUSTERS = 4085
      FAT32_MINIMUM_CLUSTERS = 65_525
      DIRECTORY_ENTRY_BYTES = 32
      ENTRY_BITS = { "fat12" => 12, "fat16" => 16, "fat32" => 32 }.freeze

      # TotSec: BPB_TotSec16 unless it is zero, then BPB_TotSec32.
      def total_sectors = FsimgSpec.derive(total_sectors_16) { _1.zero? ? total_sectors_32 : _1 }

      # FATSz: BPB_FATSz16 unless it is zero, then BPB_FATSz32.
      def fat_size = FsimgSpec.derive(fat_size_16) { _1.zero? ? fat_size_32 : _1 }

      # RootDirSectors: the root directory's entries rounded up to sectors.
      def root_dir_sectors
        FsimgSpec.derive(root_entries, bytes_per_sector) do |entries, sector|
          next Undefined.new(reason: "zero bytes per sector") if sector.zero?

          ((entries * DIRECTORY_ENTRY_BYTES) + sector - 1) / sector
        end
      end

      # The first sector of the data region, after the reserved sectors,
      # every FAT and the FAT12/16 root directory.
      def first_data_sector
        FsimgSpec.derive(reserved_sectors, fat_count, fat_size, root_dir_sectors) do |reserved, fats, size, root|
          reserved + (fats * size) + root
        end
      end

      # DataSec: the sectors left for clusters. It is Undefined when an
      # input is or when the metadata does not fit the volume, so a rule
      # about that reads total_sectors and first_data_sector instead.
      def data_sectors
        FsimgSpec.derive(total_sectors, first_data_sector) do |total, first|
          next Undefined.new(reason: "metadata of #{first} sectors exceeds #{total} total sectors") if first > total

          total - first
        end
      end

      # CountofClusters: whole clusters in the data region.
      def cluster_count
        FsimgSpec.derive(data_sectors, sectors_per_cluster) do |data, per_cluster|
          next Undefined.new(reason: "zero sectors per cluster") if per_cluster.zero?

          data / per_cluster
        end
      end

      def variant = FsimgSpec.derive(cluster_count) { Fat.variant_of(_1) }

      # The largest valid cluster number; clusters are numbered from 2.
      def max_cluster = FsimgSpec.derive(cluster_count) { _1 + 1 }

      # The bytes one FAT needs for entries 0 through max_cluster: 12 bits
      # per entry rounded up to whole bytes, 16 bits or 32 bits.
      def fat_bytes_required
        FsimgSpec.derive(max_cluster, variant) { |maximum, kind| (((maximum + 1) * ENTRY_BITS.fetch(kind)) + 7) / 8 }
      end

      # The bytes one FAT occupies.
      def fat_bytes = FsimgSpec.derive(fat_size, bytes_per_sector) { _1 * _2 }

      # Whether one FAT holds an entry for every cluster, or Undefined.
      def fat_fits = FsimgSpec.derive(fat_bytes, fat_bytes_required) { _1 >= _2 }
    end
  end
end
