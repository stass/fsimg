# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The deterministic witnesses of the FAT rules
    # (specs/design-spec-testing.md#generation-and-shrinking). Every witness
    # varies one of three small valid bases, rendered by the independent
    # builder, either by rebuilding with one parameter changed, which
    # recomputes every dependent field, or by mutating one field of the
    # rendered bytes where the builder cannot render the value or the field
    # is not a parameter. A FAT32 boot record and its backup copy are one
    # record: a mutation of the boot sector applies to both, so a witness
    # never differs from its backup by accident.
    #
    # A field rule's witnesses follow from its Domain: every enumerable
    # legal value, every invalid example and the neighbours of every
    # boundary that the field can hold. Rules of one field share its values,
    # and a value targets the first of them that excludes it, so changing a
    # domain changes the witnesses without editing any case. A relational
    # rule's domain answers its witnesses from the bases' layouts.
    #
    # A witness's id is its rule's id and a label. A label that is only a
    # field value is decimal for an 8-bit field and zero-padded hexadecimal
    # for a wider one, as in fat32.bpb.fsinfo_sector.fffe; a label that names
    # a variation, such as fat32_16, counts in decimal.
    module Witnesses
      # The valid bases: the smallest FAT12 volume worth reading, and the
      # smallest FAT16 and FAT32 cluster counts.
      BASES = { "fat12" => { clusters: 100 }, "fat16" => { clusters: 4085 }, "fat32" => { clusters: 65_525 } }.freeze

      # One witness of a relational rule: the base it varies, builder
      # parameters that replace the base's, the tree the builder writes
      # into it, mutations of the rendered image, the format forced with
      # -t, and whether the witness is meant to violate the rule.
      class Variation < Data.define(:label, :base, :parameters, :tree, :mutations, :forced_format, :violates)
        include Checked

        def initialize(label:, base:, violates:, parameters: {}, tree: [], mutations: [], forced_format: nil) = super
      end

      # A case before classification: its id, its builder parameters, tree
      # and mutations, the forced format, the rule it targets, or nil for a
      # case meant valid, and the rule whose domain generated it, or nil
      # for a valid volume of no one rule.
      Witness = Data.define(:id, :parameters, :tree, :mutations, :forced_format, :intended, :rule)

      # Valid volumes beyond the bases: both sides of each variant threshold
      # (section 3.5), and the largest allocation unit fsimg reads, 4096-byte
      # sectors in 128-sector clusters.
      VALID = {
        "fat.valid.fat12" => BASES.fetch("fat12"), "fat.valid.fat16" => BASES.fetch("fat16"),
        "fat.valid.fat32" => BASES.fetch("fat32"), "fat.valid.clusters_4084" => { clusters: 4084 },
        "fat.valid.clusters_65524" => { clusters: 65_524 },
        "fat.valid.cluster_bytes_524288" => { clusters: 16, bytes_per_sector: 4096, sectors_per_cluster: 128 }
      }.freeze

      # Every deterministic witness of registry's rules.
      def self.deterministic(registry)
        layouts = BASES.transform_values { Builder::Layout.new(_1) }
        valid = VALID.map { |id, parameters| Witness.new(id:, parameters:, tree: [], mutations: [], forced_format: nil, intended: nil, rule: nil) }
        relational = registry.rules.reject { Rules::FIELDS.key?(_1.id) || _1.domain.nil? }
        valid + field_rules(registry).flat_map { |field, rules| field_witnesses(field, rules) } +
          relational.flat_map { |rule| rule.domain.call(layouts).map { witness(rule, _1) } }
      end

      # The witness of value drawn for field: always a mutation of the
      # base, so that random draws render no new base
      # (specs/design-spec-testing.md#generation-and-shrinking).
      def self.drawn(registry, field:, value:) =
        field_witness(field, rules: field_rules(registry).fetch(field), value:, rebuild: false)

      # registry's field rules grouped by their field.
      private_class_method def self.field_rules(registry) =
        registry.rules.select { Rules::FIELDS.key?(_1.id) }.group_by { Rules::FIELDS.fetch(_1.id) }

      # The witnesses of one field's rules. A value the builder can render
      # is a rebuild; any other is a mutation of the base.
      private_class_method def self.field_witnesses(field, rules) =
        rules.flat_map { _1.domain.witnesses }.uniq.sort.select { field.fits?(_1) }
             .map { field_witness(field, rules:, value: _1, rebuild: true) }

      # value in field, targeting the first of rules that excludes it.
      private_class_method def self.field_witness(field, rules:, value:, rebuild:)
        base = base_for(field)
        intended = rules.find { !_1.domain.legal?(value) }
        rebuilt = rebuild && field.parameter && renders?(base.merge(field.parameter => value))
        parameters = rebuilt ? base.merge(field.parameter => value) : base
        rule = (intended || rules.first).id
        Witness.new(id: "#{rule}.#{field.label(value)}", parameters:, tree: [],
                    mutations: with_backup_copies(parameters, rebuilt ? [] : [field.mutation(value)]),
                    forced_format: nil, intended: intended&.id, rule:)
      end

      # A FAT32 extension field is varied on the FAT32 base, and every other
      # field on the FAT12 base, the smallest.
      private_class_method def self.base_for(field) =
        BASES.fetch(field.record == Layout::Fat32Extension ? "fat32" : "fat12")

      private_class_method def self.renders?(parameters)
        Builder::Layout.new(parameters)
        true
      rescue BuilderError
        false
      end

      private_class_method def self.witness(rule, variation)
        parameters = BASES.fetch(variation.base).merge(variation.parameters)
        Witness.new(id: "#{rule.id}.#{variation.label}", parameters:, tree: variation.tree,
                    mutations: with_backup_copies(parameters, variation.mutations), forced_format: variation.forced_format,
                    intended: variation.violates ? rule.id : nil, rule: rule.id)
      end

      # mutations, with each change to the boot sector followed by the same
      # change to the backup boot sector of a FAT32 volume rendered from
      # parameters that has one. The copies follow the rendered layout: a
      # mutation that moves the reserved area or BPB_BkBootSec, as
      # fat.bpb.total_sectors.fat32_tot_sec16_only does, does not move them.
      # A truncation must be the last mutation, so no change and no backup
      # copy applies to a cut image.
      private_class_method def self.with_backup_copies(parameters, mutations)
        truncation = mutations.index { _1.is_a?(Mutation::Truncate) }
        raise HarnessError, "a truncation must be a witness's last mutation" unless
          truncation.nil? || truncation == mutations.size - 1

        layout = Builder::Layout.new(parameters)
        return mutations unless layout.fat32? && layout.backup_boot.positive?

        backup = layout.sector_offset(layout.backup_boot)
        mutations.flat_map { [_1, *backup_copy(_1, backup)] }
      end

      # The copy of mutation in the backup boot sector at offset backup, as
      # a list: empty for a truncation or a change beyond the boot sector.
      # A change that straddles the end of the boot sector would be half a
      # record change, and a mutation of any other kind has no known copy,
      # so a generator makes neither.
      private_class_method def self.backup_copy(mutation, backup)
        case mutation
        in Mutation::Truncate then []
        in Mutation::SetInteger | Mutation::ReplaceBytes if mutation.offset >= Model::BOOT_SECTOR_BYTES then []
        in Mutation::SetInteger | Mutation::ReplaceBytes if mutation.extent <= Model::BOOT_SECTOR_BYTES
          [mutation.with(offset: mutation.offset + backup)]
        in Mutation::SetInteger | Mutation::ReplaceBytes
          raise HarnessError, "a mutation at #{mutation.offset} straddles the end of the boot sector"
        else raise HarnessError, "a #{mutation.class} mutation has no backup boot sector copy"
        end
      end

      # [value, violates] pairs: each valid value, then each violating one.
      private_class_method def self.outcomes(valid:, violating:) =
        valid.map { [_1, false] } + violating.map { [_1, true] }

      # A field of the common BPB or of the FAT32 extension, and a mutation
      # setting one. A label that is a field value comes from the field
      # (Rules::Field#label), so it cannot drift from the value.
      private_class_method def self.common_field(name) = Rules::Field.new(record: Layout::CommonBpb, name:, parameter: nil)

      private_class_method def self.fat32_field(name) = Rules::Field.new(record: Layout::Fat32Extension, name:, parameter: nil)

      private_class_method def self.common(name, value) = common_field(name).mutation(value)

      private_class_method def self.fat32(name, value) = fat32_field(name).mutation(value)

      # A variation of base with field mutated to value, labelled by it.
      private_class_method def self.mutated(field, value, base:, violates:) =
        Variation.new(label: field.label(value), base:, violates:, mutations: [field.mutation(value)])

      # The image cut to fewer bytes than the boot sector: none, the common
      # BPB, the FAT12/16 and the FAT32 extensions, and one byte short.
      BOOT_PRESENT = lambda { |_layouts|
        { "fat12" => [0, 36, 62, 511], "fat32" => [90] }.flat_map do |base, lengths|
          lengths.map do |length|
            Variation.new(label: "#{base}_#{length}", base:, violates: true, mutations: [Mutation::Truncate.new(length:)])
          end
        end
      }

      JUMP = lambda { |_layouts|
        outcomes(valid: %w[eb0090 e90000 e9ffff], violating: %w[eb3c00 eb3c91 000000 e83c90 ea3c90]).map do |hex, violates|
          Variation.new(label: hex, base: "fat12", violates:,
                        mutations: [Mutation::ReplaceBytes.new(offset: 0, bytes: [hex].pack("H*"))])
        end
      }

      # In order: a FAT12 volume that counts only in BPB_TotSec32, which
      # the rule allows; both counts set; both zero; and both set on the
      # FAT32 base. There BPB_TotSec16 counts too few sectors for FAT32, so
      # the volume decodes as FAT16 and breaks the FAT12/16 rules of its
      # FAT32 fields as well.
      #
      # Last, a volume that counts only in BPB_TotSec16 and still has
      # FAT32's 65525 clusters, which breaks the FAT32 clause: two reserved
      # sectors without a backup record and one one-sector FAT leave room
      # for them, but the FAT cannot hold them, so the case also breaks
      # fat.geometry.fat_capacity.
      TOTAL_SECTORS = lambda { |layouts|
        [
          Variation.new(label: "tot_sec32_only", base: "fat12", violates: false,
                        mutations: [common(:bpb_tot_sec16, 0), common(:bpb_tot_sec32, layouts.fetch("fat12").total_sectors)]),
          Variation.new(label: "both_set", base: "fat16", violates: true,
                        mutations: [common(:bpb_tot_sec32, layouts.fetch("fat16").total_sectors)]),
          Variation.new(label: "both_zero", base: "fat12", violates: true, mutations: [common(:bpb_tot_sec16, 0)]),
          Variation.new(label: "fat32_both_set", base: "fat32", violates: true,
                        mutations: [common(:bpb_tot_sec16, 0xFFFF)]),
          Variation.new(label: "fat32_tot_sec16_only", base: "fat32", violates: true,
                        mutations: [common(:bpb_rsvd_sec_cnt, 2), common(:bpb_num_fats, 1), fat32(:bpb_fat_sz32, 1),
                                    fat32(:bpb_bk_boot_sec, 0), common(:bpb_tot_sec16, 0xFFFF), common(:bpb_tot_sec32, 0)])
        ]
      }

      # A FAT32 volume with BPB_FATSz16 set to its FAT size, and a FAT12
      # volume with none. A zero BPB_FATSz16 selects the FAT32 extension, so
      # the FAT12 witness writes a valid one over its FAT12 extension: the
      # root at cluster 2, FSInfo at sector 1 of two reserved sectors and no
      # backup boot record. Its BPB_FATSz32 is the base layout's FAT size,
      # which the second reserved sector does not change. The pointers are
      # only in range: a FAT12 image holds no FSInfo structure at sector 1,
      # and the open-scope rules read no more. The geometry stays FAT12, and
      # only fat.bpb.fat_size breaks.
      FAT_SIZE = lambda { |layouts|
        extension = Layout::Fat32Extension.new(bpb_fat_sz32: layouts.fetch("fat12").fat_size, bpb_root_clus: 2,
                                               bpb_fs_info: 1, bpb_bk_boot_sec: 0)
        [
          Variation.new(label: "fat32_fat_sz16", base: "fat32", violates: true,
                        mutations: [common(:bpb_fat_sz16, layouts.fetch("fat32").fat_size)]),
          Variation.new(label: "fat12_no_fat_sz16", base: "fat12", violates: true, parameters: { reserved_sectors: 2 },
                        mutations: [common(:bpb_fat_sz16, 0),
                                    Mutation::ReplaceBytes.new(offset: Layout::POSITIONS.fetch(Layout::Fat32Extension),
                                                               bytes: extension.to_binary_s)])
        ]
      }

      ROOT_ENTRIES = lambda { |_layouts|
        [
          Variation.new(label: "fat32_16", base: "fat32", violates: true, parameters: { root_entries: 16 }),
          Variation.new(label: "fat12_0", base: "fat12", violates: true, parameters: { root_entries: 0 })
        ]
      }

      # With 512-byte sectors a root region is aligned at multiples of 16.
      ROOT_ALIGNMENT = lambda { |_layouts|
        outcomes(valid: [16, 528], violating: [1, 511]).map do |entries, violates|
          Variation.new(label: common_field(:bpb_root_ent_cnt).label(entries), base: "fat12", violates:,
                        parameters: { root_entries: entries })
        end
      }

      # The FAT12 base with BPB_TotSec16 set to total.
      private_class_method def self.total_sectors(total, violates:)
        field = common_field(:bpb_tot_sec16)
        Variation.new(label: "total_#{field.label(total)}", base: "fat12", violates:, mutations: [field.mutation(total)])
      end

      # A volume one sector shorter than its regions.
      REGIONS = lambda { |layouts|
        total = layouts.fetch("fat12").first_data_sector - 1
        [total_sectors(total, violates: true)]
      }

      # A volume with a single cluster, and one that ends with its regions,
      # leaving none.
      DATA_REGION = lambda { |layouts|
        layout = layouts.fetch("fat12")
        first = layout.first_data_sector
        outcomes(valid: [first + layout.sectors_per_cluster], violating: [first]).map do |total, violates|
          total_sectors(total, violates:)
        end
      }

      WITHIN_IMAGE = lambda { |layouts|
        layout = layouts.fetch("fat12")
        bytes = layout.total_sectors * layout.bytes_per_sector
        [1, layout.bytes_per_sector, bytes - Model::BOOT_SECTOR_BYTES].map do |short|
          Variation.new(label: "short_#{short}", base: "fat12", violates: true, mutations: [Mutation::Truncate.new(length: bytes - short)])
        end
      }

      # Cluster counts whose entries, with the two reserved ones, fill a FAT
      # of fat_size 512-byte sectors exactly, and one more cluster in the
      # same FAT: 1022 FAT12 clusters fill three sectors, 4094 FAT16
      # clusters 16 and 65534 FAT32 clusters 512. The 682 entries of 680
      # FAT12 clusters take 1023 of two sectors' 1024 bytes, and one more
      # entry, a byte and a half, does not fit.
      FAT_CAPACITY = lambda { |_layouts|
        [["fat12", 1022, 3], ["fat12", 680, 2], ["fat16", 4094, 16], ["fat32", 65_534, 512]].flat_map do |base, clusters, fat_size|
          outcomes(valid: [clusters], violating: [clusters + 1]).map do |count, violates|
            Variation.new(label: "#{base}_#{count}", base:, violates:, parameters: { clusters: count, fat_size: })
          end
        end
      }

      # A FAT32 BPB declaring 0x0FFFFFF6 clusters, whose largest cluster
      # number is the bad-cluster marker; such a volume exceeds the image
      # and its FAT too.
      MAX_CLUSTER = lambda { |layouts|
        layout = layouts.fetch("fat32")
        total = layout.first_data_sector + (0x0FFF_FFF6 * layout.sectors_per_cluster)
        [Variation.new(label: "bad_marker", base: "fat32", violates: true, mutations: [common(:bpb_tot_sec32, total)])]
      }

      FORCED_VARIANT = lambda { |_layouts|
        BASES.keys.product(BASES.keys).map do |base, forced|
          Variation.new(label: "#{base}_as_#{forced}", base:, forced_format: forced, violates: base != forced)
        end
      }

      # The FAT32 base with BPB_ExtFlags set to flags.
      private_class_method def self.ext_flags(flags, violates:) =
        mutated(fat32_field(:bpb_ext_flags), flags, base: "fat32", violates:)

      # The base has two FATs. Mirrored, the active-FAT bits are ignored;
      # with mirroring disabled, the zero-based active FAT 1, the second,
      # exists, and 2 does not.
      EXT_FLAGS = lambda { |_layouts|
        outcomes(valid: [0x000F, 0x0080, 0x0081], violating: [0x0082]).map { |flags, violates| ext_flags(flags, violates:) }
      }

      # One bit set in each reserved range, bits 4-6 and 8-15, at each end.
      EXT_FLAGS_RESERVED = lambda { |_layouts|
        [0x0010, 0x0040, 0x0100, 0x8000].map { ext_flags(_1, violates: true) }
      }

      FAT32_RESERVED = lambda { |_layouts|
        { "byte_52" => [52, "\x01"], "byte_63" => [63, "\x01"], "all" => [52, "\xFF" * 12] }.map do |label, (offset, bytes)|
          Variation.new(label:, base: "fat32", violates: true, mutations: [Mutation::ReplaceBytes.new(offset:, bytes: bytes.b)])
        end
      }

      # Clusters 2 through the maximum; the builder renders a valid root at
      # any of them.
      ROOT_CLUSTER = lambda { |layouts|
        max = layouts.fetch("fat32").clusters + 1
        field = fat32_field(:bpb_root_clus)
        valid = [3, max].map do
          Variation.new(label: field.label(_1), base: "fat32", violates: false, parameters: { root_cluster: _1 })
        end
        valid + [0, 1, max + 1, 0xFFFF_FFFF].map { mutated(field, _1, base: "fat32", violates: true) }
      }

      # The base reserves 32 sectors. The sectors around the reserved area
      # that the builder cannot render are mutations; the legal ones are
      # the witnesses of the recommended-sector rules, and 0xFFFF is the
      # absent-sector rule's.
      FSINFO_SECTOR = lambda { |_layouts|
        [0, 32, 0xFFFE].map { mutated(Rules::FSINFO.field, _1, base: "fat32", violates: true) }
      }

      BACKUP_BOOT_SECTOR = lambda { |_layouts|
        [32, 0xFFFE].map { mutated(Rules::BACKUP_BOOT.field, _1, base: "fat32", violates: true) }
      }

      # Rebuilt volumes with the structure elsewhere in the reserved area,
      # each legal but not recommended, by the pointer's field name: FSInfo
      # at the third sector and at the last, which leaves no room for a
      # backup record, and the latest backup record the builder renders,
      # whose three-sector record with its FSInfo copy ends with the
      # reserved area.
      RECOMMENDED = {
        bpb_fs_info: [{ fs_info: 2 }, { fs_info: 31, backup_boot: 0 }],
        bpb_bk_boot_sec: [{ backup_boot: 29 }]
      }.freeze

      # The witnesses of a recommended-sector rule over field, each
      # labelled by the value its parameters give the field.
      def self.recommended(field) = lambda { |_layouts|
        RECOMMENDED.fetch(field.name).map do |parameters|
          Variation.new(label: field.label(parameters.fetch(field.parameter)), base: "fat32", violates: true, parameters:)
        end
      }

      # The one witness of an absent-sector rule: field set to 0xFFFF.
      def self.absent(field) = lambda { |_layouts|
        [mutated(field, Rules::ABSENT_SECTOR, base: "fat32", violates: true)]
      }

      # A destination witness is a valid FAT volume whose names some hosts
      # cannot hold (specs/design-spec-testing.md#host-names). Each is meant
      # to violate its rule, but only the host's name trials decide whether
      # it does; on a host that holds its names it runs as an ordinary case.
      private_class_method def self.files(*names) =
        names.each_with_index.map { TreeEntry.new(path: _1, type: "file", size: 1, seed: _2 + 1) }

      # U+00E9 and e with U+0301: distinct FAT names, since FAT compares
      # names by simple uppercase and not by normalization, which a
      # normalization-insensitive host such as APFS takes for one. A case
      # pair is no witness: FAT takes it for one name, an ambiguous
      # directory that fsimg rejects before it creates anything.
      HOST_COLLISION = lambda { |_layouts|
        [Variation.new(label: "nfc_nfd", base: "fat12", violates: true, tree: files("\u00e9", "e\u0301"))]
      }

      # 128 U+00E9: 128 UTF-16 units in FAT, well within its 255, but 256
      # bytes of UTF-8, beyond the 255-byte component limit of hosts such
      # as ext4 and UFS. APFS counts characters and holds it.
      HOST_UNREPRESENTABLE = lambda { |_layouts|
        [Variation.new(label: "utf8_256", base: "fat12", violates: true, tree: files("\u00e9" * 128))]
      }
    end
  end
end
