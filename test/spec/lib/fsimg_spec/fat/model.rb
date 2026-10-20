# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # What the harness reads from a FAT image, decoded by its own reading of
    # the specification. The decoder is total: it decodes any byte prefix,
    # reading a record only when every byte of it is present and marking
    # every other record Undefined. Records are frozen plain values
    # (Layout::VALUES).
    #
    # The boot sector carries one of two extensions after the common BPB.
    # The specification takes BPB_FATSz32 only when BPB_FATSz16 is zero, so
    # a nonzero BPB_FATSz16 selects the FAT12/16 extension and a zero one the
    # FAT32 extension; a FAT32 extension is never read over a FAT12/16 BPB.
    # fat32_extension says which was read, and is Undefined with the BPB.
    #
    # fat and root_directory stand for the structures this model does not
    # decode; they are Undefined, and every scope that reads them is
    # unclassified, derived from them rather than listed.
    class Model < Data.define(:image_length, :common, :fat32_extension, :extension, :signature, :geometry,
                              :fat, :root_directory)
      # The boot sector records lie in the volume's first 512 bytes,
      # whatever BPB_BytsPerSec says.
      BOOT_SECTOR_BYTES = 512
      # The structures each scope of the contract (SCOPE_STAGES) reads
      # beyond the boot sector. Every scope is listed, so a scope missing
      # here can never pass for decoded.
      SCOPE_INPUTS = {
        "open" => [], "directories" => %i[fat root_directory], "files" => %i[fat root_directory],
        "verification" => %i[fat root_directory], "destination" => %i[root_directory], "input" => []
      }.freeze

      def self.decode(image)
        sector = image.read(0, BOOT_SECTOR_BYTES)
        common = record(Layout::CommonBpb, sector)
        fat32_extension = FsimgSpec.derive(common) { _1.bpb_fat_sz16.zero? }
        extension = FsimgSpec.derive(fat32_extension) { record(_1 ? Layout::Fat32Extension : Layout::Fat16Extension, sector) }
        new(
          image_length: image.length,
          common:, fat32_extension:, extension:,
          signature: record(Layout::Signature, sector),
          geometry: geometry(common, fat32_extension, extension),
          fat: Undefined.new(reason: "the model does not decode the allocation tables"),
          root_directory: Undefined.new(reason: "the model does not decode directories")
        )
      end

      # The info record's fields the model derives. Fields of structures
      # the model leaves undefined, such as the clean flag in FAT[1], are
      # not listed, and a field whose value is Undefined is not compared.
      INFO_FIELDS = {
        "format" => ->(model) { model.geometry.variant },
        "sector_bytes" => ->(model) { model.geometry.bytes_per_sector },
        "cluster_bytes" => ->(model) { FsimgSpec.derive(model.geometry.bytes_per_sector, model.geometry.sectors_per_cluster) { _1 * _2 } },
        "volume_bytes" => ->(model) { FsimgSpec.derive(model.geometry.total_sectors, model.geometry.bytes_per_sector) { _1 * _2 } },
        "allocation_units" => ->(model) { model.geometry.cluster_count }
      }.freeze

      # The scopes whose inputs the model could not decode.
      def unclassified_scopes =
        SCOPE_INPUTS.select { |_, inputs| inputs.any? { FsimgSpec.undefined?(public_send(_1)) } }.keys

      # How fsimg's records for an accepting stage disagree with the model.
      # fsimg writes large integers as JSON strings, so values compare as
      # text.
      def record_problems(stage, records)
        return [] unless stage == "info"

        info = records.find { _1["type"] == "info" }
        return ["info wrote no info record"] unless info

        INFO_FIELDS.filter_map do |field, derive|
          expected = derive.call(self)
          next if FsimgSpec.undefined?(expected) || info[field].to_s == expected.to_s

          "info reports #{field} #{FsimgSpec.shown(info[field])}, the model derives #{expected}"
        end
      end

      # The plain values of layout at its position in sector, or Undefined
      # when sector ends before the record does.
      private_class_method def self.record(layout, sector)
        position = Layout::POSITIONS.fetch(layout)
        size = layout.new.num_bytes
        return Undefined.new(reason: "#{layout.name} needs bytes #{position}-#{position + size - 1}") if sector.bytesize < position + size

        Layout.decode(layout, sector.byteslice(position, size))
      end

      # The BPB fields geometry derives from; every one is Undefined when
      # the common BPB is.
      private_class_method def self.geometry(common, fat32_extension, extension)
        return Geometry.new(**Geometry.members.to_h { [_1, common] }) if FsimgSpec.undefined?(common)

        Geometry.new(
          bytes_per_sector: common.bpb_byts_per_sec,
          sectors_per_cluster: common.bpb_sec_per_clus,
          reserved_sectors: common.bpb_rsvd_sec_cnt,
          fat_count: common.bpb_num_fats,
          root_entries: common.bpb_root_ent_cnt,
          total_sectors_16: common.bpb_tot_sec16,
          total_sectors_32: common.bpb_tot_sec32,
          fat_size_16: common.bpb_fat_sz16,
          fat_size_32: fat_size_32(fat32_extension, extension)
        )
      end

      private_class_method def self.fat_size_32(fat32_extension, extension)
        if FsimgSpec.undefined?(extension) then extension
        elsif fat32_extension then extension.bpb_fat_sz32
        else Undefined.new(reason: "a FAT12/16 boot sector has no BPB_FATSz32")
        end
      end
    end
  end
end
