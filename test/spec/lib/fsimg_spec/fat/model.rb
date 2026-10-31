# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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
    # fsinfo is the FSInfo record (section 5) of a volume whose cluster
    # count makes it FAT32, read at the sector BPB_FSInfo names. It is nil
    # where there is none to read:
    #   * the boot sector has no FAT32 extension, so no BPB_FSInfo;
    #   * the cluster count makes the volume FAT12 or FAT16;
    #   * BPB_FSInfo names no sector of the reserved area after the boot
    #     sector, as 0xFFFF, which compatible readers take for none, does
    #     not; the pointer's own rules judge that value.
    # It is Undefined with the boot sector, with the variant of a volume
    # that has a FAT32 extension, or when the image ends before the record.
    #
    # The model does not decode the allocation tables or directories. tree
    # is the known tree the case says its image holds, an Array of
    # WalkedEntry that KnownTree builds: an unmutated base its recipe's
    # tree, an image fsimg created the source tree the harness wrote
    # (specs/design-spec-testing.md#cases). A mutated image's tree is
    # Undefined, and every scope that reads it is unclassified, derived from
    # it rather than listed.
    class Model < Data.define(:image_length, :common, :fat32_extension, :extension, :signature, :fsinfo, :geometry,
                             :tree)
      # The boot sector records lie in the volume's first 512 bytes,
      # whatever BPB_BytsPerSec says.
      BOOT_SECTOR_BYTES = 512
      # What each scope of an image (SCOPE_STAGES but input, which a
      # SourceModel answers) reads beyond the boot sector. Every scope is
      # listed, so a scope missing here can never pass for decoded. The
      # destination scope reads the tree's names; the host's answers to
      # their name trials, which decide its violations, are the run
      # environment's (specs/design-spec-testing.md#host-names).
      SCOPE_INPUTS = {
        "open" => [], "directories" => %i[tree], "files" => %i[tree], "verification" => %i[tree fsinfo], "destination" => %i[tree]
      }.freeze
      UNKNOWN_TREE = Undefined.new(reason: "the tree of an image no case vouches for, such as a mutated one, is unknown")
      # The entry kinds a known tree holds.
      ENTRY_KINDS = %w[file directory].freeze
      # The validation scope each verification stage must report
      # (specs/design-cli.md#verification-information-and-formats).
      VALIDATION_SCOPES = { "verify" => "structural", "verify-deep" => "initialized-payload" }.freeze

      def self.decode(image, tree: UNKNOWN_TREE)
        sector = image.read(0, BOOT_SECTOR_BYTES)
        common = record(Layout::CommonBpb, sector)
        fat32_extension = FsimgSpec.derive(common) { _1.bpb_fat_sz16.zero? }
        extension = FsimgSpec.derive(fat32_extension) { record(_1 ? Layout::Fat32Extension : Layout::Fat16Extension, sector) }
        geometry = geometry(common, fat32_extension, extension)
        new(
          image_length: image.length,
          common:, fat32_extension:, extension:,
          signature: record(Layout::Signature, sector),
          fsinfo: fsinfo(image, common:, fat32_extension:, extension:, variant: geometry.variant),
          geometry:,
          tree:
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

      # How fsimg's records for an accepting stage, a StageResult, disagree
      # with the model (specs/design-spec-testing.md#expectation-derivation):
      # info with the boot sector, ls with the known tree's paths, types and
      # file sizes, extract's walked destination with the whole known tree,
      # SHA-256 digests included, and verify with its file, directory and
      # logical-byte counts. fsimg writes large integers as JSON strings,
      # so values compare as text. The records are fsimg's, the output under
      # test, so every value a message quotes is shown, and a stage must
      # write exactly one info or verify record. The stages after info are
      # judged only where the tree is known, since otherwise their scopes
      # are unclassified and no expectation asks for records.
      def record_problems(result)
        records = result.outcome.records
        case result.stage
        in "create" then raise Error, "an image model does not judge create; its SourceModel does"
        in "info" then info_problems(records)
        in _ if FsimgSpec.undefined?(tree) then []
        in "ls" then KnownTree.differences("ls", actual: actual_listing(records), expected: expected_listing(tree))
        in "extract" then KnownTree.differences("extract", actual: KnownTree.from_walk(result.tree), expected: tree)
        in "verify" | "verify-deep" then count_problems(result.stage, records)
        end
      end

      private

      def info_problems(records)
        info, problem = sole_record(records, "info", stage: "info")
        return [problem] if problem

        INFO_FIELDS.filter_map do |field, derive|
          expected = derive.call(self)
          next if FsimgSpec.undefined?(expected) || info[field].to_s == expected.to_s

          "info reports #{field} #{FsimgSpec.shown(info[field])}, the model derives #{expected}"
        end
      end

      # The one record of type that stage wrote and nil, or nil and why
      # there is not exactly one.
      def sole_record(records, type, stage:)
        found = records.select { _1["type"] == type }
        found.one? ? [found.first, nil] : [nil, "#{stage} wrote #{found.size} #{type} records, not one"]
      end

      # The known tree as ls lists it: each path from the root, and a
      # file's size without its digest.
      def expected_listing(tree) = tree.map { _1.with(path: "/".b + _1.path, sha256: nil) }

      # ls's entry records in the form of expected_listing. A kind or size
      # that is not one a known tree holds is kept shown, so it differs: a
      # size must be the decimal JSON string fsimg writes for a large
      # integer, since a size is compared as an Integer, not as text.
      def actual_listing(records)
        records.select { _1["type"] == "entry" }.map do |entry|
          kind = entry["kind"]
          WalkedEntry.new(path: entry["path"].to_s.b, type: ENTRY_KINDS.include?(kind) ? kind : FsimgSpec.shown(kind),
                          size: kind == "file" ? decimal(entry["size_bytes"]) : nil, sha256: nil)
        end
      end

      # A JSON string of decimal digits as its Integer, or the value shown.
      def decimal(value) = value.is_a?(String) && value.match?(/\A(0|[1-9][0-9]{0,19})\z/) ? Integer(value, 10) : FsimgSpec.shown(value)

      # The verify record's counts, completeness and validation scope.
      # fsimg counts the root among the directories (include/fsimg.h,
      # fsimg_verify_report_t), which the tree does not list.
      def count_problems(stage, records)
        verify, problem = sole_record(records, "verify", stage:)
        return [problem] if problem

        files = tree.select { _1.type == "file" }
        expected = {
          "complete" => true, "validation_scope" => VALIDATION_SCOPES.fetch(stage),
          "files" => files.size, "directories" => tree.size - files.size + 1, "logical_bytes" => files.sum(&:size)
        }
        expected.filter_map do |field, value|
          "#{stage} reports #{field} #{FsimgSpec.shown(verify[field])}, not #{value}" unless verify[field].to_s == value.to_s
        end
      end

      # Whether sector is one of the reserved area after the boot sector,
      # where a FAT32 volume's FSInfo and backup boot record lie.
      def self.reserved_sector?(common, sector) = (1...common.bpb_rsvd_sec_cnt).cover?(sector)

      # The plain values of layout at its position in sector, or Undefined
      # when sector ends before the record does.
      private_class_method def self.record(layout, sector)
        position = Layout::POSITIONS.fetch(layout)
        size = layout.new.num_bytes
        return Undefined.new(reason: "#{layout.name} needs bytes #{position}-#{position + size - 1}") if sector.bytesize < position + size

        Layout.decode(layout, sector.byteslice(position, size))
      end

      # The FSInfo record BPB_FSInfo locates in image (Model's fsinfo), from
      # the decoded common BPB, whether the extension is FAT32's, the
      # extension itself and the variant the cluster count gives.
      private_class_method def self.fsinfo(image, common:, fat32_extension:, extension:, variant:)
        FsimgSpec.derive(fat32_extension) do |fat32|
          next nil unless fat32

          FsimgSpec.derive(extension, variant) do |fields, kind|
            sector = fields.bpb_fs_info
            next nil unless kind == "fat32" && reserved_sector?(common, sector)

            record(Layout::FsInfo, image.read(sector * common.bpb_byts_per_sec, Layout::FsInfo.new.num_bytes))
          end
        end
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
