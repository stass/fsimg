# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The FAT rule set (docs/spec-testing-plan.md#initial-fat-rule-set). Each
    # normative rule cites the Microsoft FAT Specification of August 30
    # 2005 by section and field, as the PDF numbers them; each profile rule
    # cites the fsimg design that states the policy.
    #
    # Every rule but the name and destination rules reads the boot sector,
    # which the reader checks when it opens the volume
    # (specs/design-fat-reader.md#identification-and-geometry), so its
    # scope is open. That includes the range of BPB_FSInfo and
    # BPB_BkBootSec; the structures they locate are checked within the
    # verification scope. The name rules read the tree's names, which the
    # reader checks as it prepares each directory. A field rule's domain is a Domain over the
    # field's values, and FIELDS says where the field lies and which
    # builder parameter renders it. A relational rule's
    # domain is a function of the bases that answers its witnesses
    # (Witnesses::Variation), since its legal values depend on other fields.
    module Rules
      SPECIFICATION = "Microsoft FAT Specification (August 30 2005)"
      ALWAYS = ->(_model, _environment) { true }
      FAT32_EXTENSION = ->(model, _environment) { model.fat32_extension }
      # The value one past the largest cluster number each variant can
      # address: the bad-cluster marker (section 4, the table of FAT entry
      # values).
      BAD_CLUSTER = { "fat12" => 0xFF7, "fat16" => 0xFFF7, "fat32" => 0x0FFF_FFF7 }.freeze
      # The FSInfo and backup boot sector values that compatible readers
      # take to mean "none"; the specification gives 0xFFFF no meaning.
      ABSENT_SECTOR = 0xFFFF
      # FSInfo's three signatures (section 5), by field.
      FSINFO_SIGNATURES = { fsi_lead_sig: 0x4161_5252, fsi_struc_sig: 0x6141_7272, fsi_trail_sig: 0xAA55_0000 }.freeze
      # The reserved bits of BPB_ExtFlags, 4-6 and 8-15.
      EXT_FLAGS_RESERVED_BITS = 0xFF70

      # A boot sector field: its record and name in the layout, and the
      # builder parameter that renders it, or nil when only a mutation of
      # the rendered bytes can set it.
      class Field < Data.define(:record, :name, :parameter)
        OPS = { 1 => "set_u8", 2 => "set_u16le", 4 => "set_u32le" }.freeze
        OFFSETS = Layout.field_offsets.freeze

        def offset = OFFSETS.fetch(record).fetch(name)

        def width = record.new.public_send(name).num_bytes

        def max = (1 << (8 * width)) - 1

        def fits?(value) = value.between?(0, max)

        def mutation(value) = Mutation::SetInteger.new(op: OPS.fetch(width), offset:, value:)

        # value as a witness label: decimal for an 8-bit field, and
        # zero-padded hexadecimal for a wider one.
        def label(value) = width == 1 ? value.to_s : format("%0#{2 * width}x", value)

        # The field's value in model, or Undefined.
        def read(model) = FsimgSpec.derive(model.public_send(holder)) { _1.public_send(name) }

        private def holder = { Layout::CommonBpb => :common, Layout::Signature => :signature }.fetch(record, :extension)
      end

      FIELDS = {
        "fat.boot.signature" => Field.new(record: Layout::Signature, name: :signature_word, parameter: nil),
        "fat.bpb.bytes_per_sector" => Field.new(record: Layout::CommonBpb, name: :bpb_byts_per_sec, parameter: :bytes_per_sector),
        "fat.bpb.sectors_per_cluster" => Field.new(record: Layout::CommonBpb, name: :bpb_sec_per_clus, parameter: :sectors_per_cluster),
        "fat.bpb.reserved_sectors" => Field.new(record: Layout::CommonBpb, name: :bpb_rsvd_sec_cnt, parameter: :reserved_sectors),
        "fat.bpb.num_fats" => Field.new(record: Layout::CommonBpb, name: :bpb_num_fats, parameter: :fat_count),
        "fat.bpb.num_fats.supported" => Field.new(record: Layout::CommonBpb, name: :bpb_num_fats, parameter: :fat_count),
        "fat.bpb.media" => Field.new(record: Layout::CommonBpb, name: :bpb_media, parameter: :media),
        "fat32.bpb.fs_version" => Field.new(record: Layout::Fat32Extension, name: :bpb_fs_ver, parameter: nil)
      }.freeze

      # The registry's rules. Rules of one field are listed normative
      # first: a value that several of them exclude targets the first
      # (Witnesses.deterministic).
      def self.all = [*boot_rules, *bpb_rules, *geometry_rules, *fat32_rules, *name_rules, *destination_rules, *creation_rules]

      # The rules of the host's names, with FAT's witnesses.
      def self.destination_rules =
        DestinationRules.with_domains(collision: Witnesses::HOST_COLLISION, unrepresentable: Witnesses::HOST_UNREPRESENTABLE)

      def self.registry = Registry.new(all)

      # A rule over one field: it holds when the field's value is legal in
      # domain, and does not apply where the field is undefined.
      def self.field_rule(id, domain:, applies_to: ALWAYS, **fields)
        field = FIELDS.fetch(id)
        Rule.new(id:, domain:, predicate: ->(model, _environment) { FsimgSpec.derive(field.read(model)) { domain.legal?(_1) } },
                 scopes: ["open"], applies_to:, **fields)
      end

      def self.cite(section, field) = "#{SPECIFICATION}, section #{section}, #{field}"

      # The boot sector and the volume's identity signature: without them,
      # no FAT decoder claims the image (specs/design-formats.md).
      def self.boot_rules = [
        Rule.new(
          id: "fat.boot.present", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT", scopes: ["open"],
          applies_to: ALWAYS, domain: Witnesses::BOOT_PRESENT,
          predicate: ->(model, _) { model.image_length >= Model::BOOT_SECTOR_BYTES },
          source: "#{SPECIFICATION}, section 3, Boot Sector and BPB: all FAT volumes must have a BPB in the boot sector, " \
                  "the first sector of the volume"
        ),
        Rule.new(
          id: "fat.boot.jump", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT", scopes: ["open"],
          applies_to: ALWAYS, domain: Witnesses::JUMP,
          predicate: ->(model, _) { FsimgSpec.derive(model.common) { Rules.jump?(_1.bs_jmp_boot) } },
          source: cite("3.1", "BS_jmpBoot")
        ),
        field_rule("fat.boot.signature", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                         domain: Domain.new(legal: [0xAA55], invalid: [0, 0x55AA], boundaries: [0xAA55]),
                                         source: cite("3.2 and 3.3", "Signature_word"))
      ]

      NAMES_SOURCE = "specs/design-fat-reader.md, Names and metadata"

      # The names of a directory as fsimg's reader takes them (Names): each
      # one it accepts, and no two siblings with one key, which makes the
      # directory ambiguous (specs/design-fat-reader.md#directory-preparation).
      # Both read the tree, so neither applies where the tree is unknown.
      # A directory with a name whose key Ruby cannot give leaves the
      # second rule undefined.
      def self.name_rules = [
        Rule.new(
          id: "fat.directory.name", category: "profile", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["directories"], applies_to: ALWAYS, domain: Witnesses::NAME,
          predicate: ->(model, _) { FsimgSpec.derive(model.tree) { |tree| tree.none? { Names.problem(base_name(_1)) } } },
          source: "#{NAMES_SOURCE}: forbidden FAT characters, empty names, dot/dotdot, and leading/trailing spaces " \
                  "or trailing periods are rejected; #{SPECIFICATION}, section 7, a long name of at most 255 characters"
        ),
        Rule.new(
          id: "fat.directory.unique_names", category: "profile", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["directories"], applies_to: ALWAYS, domain: Witnesses::UNIQUE_NAMES,
          predicate: ->(model, _) { FsimgSpec.derive(model.tree) { distinct_siblings(_1) } },
          source: "#{NAMES_SOURCE}: the unicode-17-simple-upper comparison, under which a key may not belong to two " \
                  "entries; Directory preparation: equal keys from different entries make the directory ambiguous"
        )
      ]

      # The last component of a known tree entry's path, and the path of
      # the directory holding it, empty for the root.
      def self.base_name(entry) = entry.path.rpartition("/".b).last

      def self.parent_path(entry) = entry.path.rpartition("/".b).first

      # Whether no two entries of one directory of tree share a key, or
      # Undefined where a key is unknown.
      def self.distinct_siblings(tree)
        keys_by_directory = tree.group_by { parent_path(_1) }.values.map { |siblings| siblings.map { Names.key(base_name(_1)) } }
        return Undefined.new(reason: "a name's simple upper case is unknown") if keys_by_directory.flatten.include?(nil)

        keys_by_directory.all? { _1.uniq.size == _1.size }
      end

      WRITER = "specs/design-fat-writer.md"
      # The sector sizes BPB_BytsPerSec may hold, the sector size create
      # takes without -o sector=, the most sectors a cluster holds
      # (BPB_SecPerClus), and the largest cluster the writer's profile
      # writes.
      SECTOR_BYTES = [512, 1024, 2048, 4096].freeze
      DEFAULT_SECTOR_BYTES = 512
      MAX_SECTORS_PER_CLUSTER = 128
      MAX_WRITER_CLUSTER_BYTES = 32 << 10
      # DIR_FileSize is a 32-bit count of bytes, and BPB_TotSec32 of sectors.
      MAX_FILE_BYTES = 0xFFFF_FFFF
      MAX_TOTAL_SECTORS = 0xFFFF_FFFF

      # The domain of creation input (specs/design-spec-testing.md#cases):
      # create's settings, the names and sizes of the tree it reads, and
      # the volume it can lay out. Each rule reads a SourceModel and has
      # scope input, so create checks it, and fsimg refuses a violation
      # before it writes an image. A setting rule applies only when the
      # case gives the setting. The name rules state for the writer what
      # fat.directory.name and fat.directory.unique_names state for the
      # reader, since the writer's name profile is the reader's. The
      # sizing rules apply only to a source create gets as far as laying
      # out, one that breaks none of the rules before them.
      def self.creation_rules
        refusals = source_refusals
        laid_out = ->(source, environment) { refusals.all? { _1.violations(source, environment).empty? } }
        refusals + sizing_rules(laid_out)
      end

      def self.source_refusals = [
        Rule.new(
          id: "fat.create.sector_bytes", category: "normative", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: setting?(:sector), domain: Witnesses::CREATE_SECTOR,
          predicate: ->(source, _) { SECTOR_BYTES.include?(source.settings.fetch(:sector)) },
          source: "#{cite("3.1", "BPB_BytsPerSec")}; #{WRITER}#geometry-and-allocation: legal sectors are 512, 1024, " \
                  "2048 or 4096 bytes"
        ),
        Rule.new(
          id: "fat.create.cluster_bytes", category: "normative", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: sectored_setting?(:cluster), domain: Witnesses::CREATE_CLUSTER,
          predicate: lambda { |source, _|
            sectors, rest = source.settings.fetch(:cluster).divmod(sector_bytes(source))
            rest.zero? && (1..MAX_SECTORS_PER_CLUSTER).cover?(sectors) && sectors.nobits?(sectors - 1)
          },
          source: "#{cite("3.1", "BPB_SecPerClus")}: a power of two from 1 to 128 sectors; create's cluster is in " \
                  "bytes (specs/design-cli.md, typed settings)"
        ),
        Rule.new(
          id: "fat.create.cluster_bytes.supported", category: "profile", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: setting?(:cluster), domain: Witnesses::CREATE_CLUSTER_SUPPORTED,
          predicate: ->(source, _) { source.settings.fetch(:cluster) <= MAX_WRITER_CLUSTER_BYTES },
          source: "#{WRITER}#geometry-and-allocation: the interoperable writer profile limits clusters to powers of " \
                  "two between the sector size and 32 KiB"
        ),
        Rule.new(
          id: "fat.create.volume_bytes", category: "profile", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: sectored_setting?(:size), domain: Witnesses::CREATE_VOLUME,
          predicate: lambda { |source, _|
            sectors, rest = source.settings.fetch(:size).divmod(sector_bytes(source))
            rest.zero? && (1..MAX_TOTAL_SECTORS).cover?(sectors)
          },
          source: "specs/design-cli.md, creation options: -s is a positive exact logical volume size; " \
                  "#{cite("3.1", "BPB_TotSec16 and BPB_TotSec32")}: the volume is a count of whole sectors that " \
                  "BPB_TotSec32 holds"
        ),
        Rule.new(
          id: "fat.create.name", category: "profile", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: ALWAYS, domain: Witnesses::CREATE_NAME,
          predicate: ->(source, _) { source.tree.none? { Names.problem(base_name(_1)) } },
          source: "#{WRITER}#names-and-directory-layout: the name profile matches the FAT reader's; #{NAMES_SOURCE}"
        ),
        Rule.new(
          id: "fat.create.unique_names", category: "profile", policy: "must_reject", status: "FSIMG_EINVAL",
          scopes: ["input"], applies_to: ALWAYS, domain: Witnesses::CREATE_UNIQUE_NAMES,
          predicate: ->(source, _) { distinct_siblings(source.tree) },
          source: "#{WRITER}#names-and-directory-layout: duplicate and case-equivalent siblings are rejected before " \
                  "an image is allocated, under the reader's simple-uppercase comparison"
        ),
        Rule.new(
          id: "fat.create.file_size", category: "normative", policy: "must_reject", status: "FSIMG_ERANGE",
          scopes: ["input"], applies_to: ALWAYS, domain: Witnesses::CREATE_FILE_SIZE,
          predicate: ->(source, _) { source.tree.none? { _1.type == "file" && _1.size > MAX_FILE_BYTES } },
          source: "#{SPECIFICATION}, section 6, DIR_FileSize: a 32-bit count of the file's bytes"
        )
      ]

      # The volume create lays out (Sizing): some cluster size it may
      # choose must give a cluster count of the variant that holds the
      # tree, within what the BPB fields count, and without -s a count
      # the writer's margin admits. A count beyond the margin that the
      # variant still admits violates only the margin rule.
      def self.sizing_rules(laid_out)
        variant_range = Rule.new(
          id: "fat.create.cluster_count", category: "normative", policy: "must_reject", status: "FSIMG_ERANGE",
          scopes: ["input"], applies_to: laid_out, domain: Witnesses::CREATE_CLUSTER_COUNT,
          predicate: lambda { |source, _|
            range = Sizing::RANGES.fetch(source.format)
            Sizing.candidates(source, range:).any? { _1.fits?(range) }
          },
          source: "#{cite("3.5", "CountofClusters")}: the cluster count alone selects the variant, so FAT12 holds " \
                  "fewer than 4085 clusters, FAT16 fewer than 65525 and FAT32 at least 65525; #{WRITER}#geometry-and-" \
                  "allocation: each permitted cluster size is tried, and an exact size keeps the whole legal range"
        )
        margin = Rule.new(
          id: "fat.create.cluster_count.automatic", category: "profile", policy: "must_reject", status: "FSIMG_ERANGE",
          scopes: ["input"], domain: Witnesses::CREATE_AUTOMATIC_COUNT,
          applies_to: lambda { |source, environment|
            !source.settings.key?(:size) && laid_out.call(source, environment) &&
              variant_range.violations(source, environment).empty?
          },
          predicate: ->(source, _) { !Sizing.plan(source).nil? },
          source: "#{WRITER}#geometry-and-allocation: automatic counts stay at least 16 clusters away from the " \
                  "4085/65525 type cutovers, following Microsoft's formatting recommendation"
        )
        [variant_range, margin]
      end

      # A rule that applies when the case gives the setting name.
      def self.setting?(name) = ->(source, _) { source.settings.key?(name) }

      # A rule of a setting counted in sectors, which applies when the case
      # gives the setting beside a legal sector size; beside an illegal
      # one it defers to fat.create.sector_bytes.
      def self.sectored_setting?(name) =
        ->(source, _) { source.settings.key?(name) && SECTOR_BYTES.include?(sector_bytes(source)) }

      def self.sector_bytes(source) = source.settings.fetch(:sector, DEFAULT_SECTOR_BYTES)

      # BS_jmpBoot takes two forms: EB ?? 90 and E9 ?? ??.
      def self.jump?(bytes) = (bytes.getbyte(0) == 0xEB && bytes.getbyte(2) == 0x90) || bytes.getbyte(0) == 0xE9

      # The fixed BPB fields of the identity signature, checked when the
      # volume is probed, and the BPB fields checked when it opens.
      def self.bpb_rules = [
        field_rule("fat.bpb.bytes_per_sector", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                               domain: Domain.new(legal: [512, 1024, 2048, 4096], invalid: [0, 256], boundaries: [512, 4096]),
                                               source: cite("3.1", "BPB_BytsPerSec")),
        field_rule("fat.bpb.sectors_per_cluster", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                                  domain: Domain.new(legal: (0..7).map { 1 << _1 }, invalid: [0, 3, 255], boundaries: [1, 128]),
                                                  source: cite("3.1", "BPB_SecPerClus")),
        field_rule("fat.bpb.reserved_sectors", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                               domain: Domain.new(legal: 1..0xFFFF, invalid: [0]),
                                               source: cite("3.1", "BPB_RsvdSecCnt")),
        field_rule("fat.bpb.num_fats", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                       domain: Domain.new(legal: 1..0xFF, invalid: [0]),
                                       source: "#{cite("3.1", "BPB_NumFATs")}: \"A value of 2 is recommended although " \
                                               "a value of 1 is acceptable.\" The region arithmetic of section 3.5 " \
                                               "takes any count, and zero leaves no FAT"),
        # Zero FATs breaks fat.bpb.num_fats, not this rule.
        field_rule("fat.bpb.num_fats.supported", category: "profile", policy: "optional_feature",
                                                 domain: Domain.new(legal: 0..2, invalid: [0xFF]),
                                                 source: "specs/design-fat-reader.md: the supported profile reads one or two FATs"),
        field_rule("fat.bpb.media", category: "normative", policy: "must_reject", status: "FSIMG_EFORMAT",
                                    domain: Domain.new(legal: [0xF0, *0xF8..0xFF], invalid: [0], boundaries: [0xF0, 0xF8]),
                                    source: cite("3.1", "BPB_Media")),
        Rule.new(
          id: "fat.bpb.total_sectors", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::TOTAL_SECTORS,
          predicate: ->(model, _) { FsimgSpec.derive(model.common) { Rules.total_sectors_hold(_1, model.geometry.variant) } },
          source: cite("3.1", "BPB_TotSec16 and BPB_TotSec32")
        ),
        Rule.new(
          id: "fat.bpb.fat_size", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::FAT_SIZE,
          predicate: ->(model, _) { FsimgSpec.derive(model.common, model.geometry.variant) { |c, v| c.bpb_fat_sz16.zero? == (v == "fat32") } },
          source: "#{cite("3.1", "BPB_FATSz16")} and section 3.3, BPB_FATSz32: zero on FAT32. On FAT12/16 the " \
                  "rule set infers the other half: section 3.5 reads BPB_FATSz32 when BPB_FATSz16 is zero, and the " \
                  "FAT12/16 BPB of section 3.2 has no BPB_FATSz32"
        ),
        Rule.new(
          id: "fat.bpb.root_entries", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::ROOT_ENTRIES,
          predicate: ->(model, _) { FsimgSpec.derive(model.common, model.geometry.variant) { |c, v| c.bpb_root_ent_cnt.zero? == (v == "fat32") } },
          source: "#{cite("3.1", "BPB_RootEntCnt")}; section 6.6, Root Directory"
        ),
        Rule.new(
          id: "fat.bpb.root_entries.aligned", category: "advisory", policy: "must_accept", scopes: ["open"],
          applies_to: ->(model, _) { FsimgSpec.derive(model.geometry.variant) { _1 != "fat32" } },
          domain: Witnesses::ROOT_ALIGNMENT,
          predicate: lambda { |model, _|
            FsimgSpec.derive(model.common) { |c| ((c.bpb_root_ent_cnt * 32) % c.bpb_byts_per_sec).zero? }
          },
          source: cite("3.1", "BPB_RootEntCnt (\"should always specify\")")
        )
      ]

      # The reserved area, the FATs and the root region end within the
      # volume.
      def self.regions_fit(geometry) = FsimgSpec.derive(geometry.first_data_sector, geometry.total_sectors) { _1 <= _2 }

      # Exactly one of BPB_TotSec16 and BPB_TotSec32 counts the sectors, and
      # a FAT32 volume uses BPB_TotSec32. Both set, or both zero, breaks the
      # first part whatever the variant.
      def self.total_sectors_hold(common, variant)
        return false if common.bpb_tot_sec16.zero? == common.bpb_tot_sec32.zero?

        FsimgSpec.derive(variant) { _1 != "fat32" || common.bpb_tot_sec16.zero? }
      end

      # The regions the BPB declares (sections 3.5 and 4.1).
      def self.geometry_rules = [
        Rule.new(
          id: "fat.geometry.regions", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::REGIONS,
          predicate: ->(model, _) { Rules.regions_fit(model.geometry) },
          source: "#{cite("3.1", "BPB_TotSec16 and BPB_TotSec32")}: the count includes all sectors of all four regions"
        ),
        # The reader requires at least one data cluster, which the
        # specification leaves open; a volume whose regions overflow it
        # breaks fat.geometry.regions instead.
        Rule.new(
          id: "fat.geometry.data_region", category: "profile", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ->(model, _) { Rules.regions_fit(model.geometry) }, domain: Witnesses::DATA_REGION,
          predicate: ->(model, _) { FsimgSpec.derive(model.geometry.cluster_count) { _1.positive? } },
          source: "specs/design-fat-reader.md#identification-and-geometry: a data region of at least one cluster"
        ),
        Rule.new(
          id: "fat.geometry.within_image", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::WITHIN_IMAGE,
          predicate: lambda { |model, _|
            FsimgSpec.derive(model.geometry.total_sectors, model.geometry.bytes_per_sector) { _1 * _2 <= model.image_length }
          },
          source: "#{cite("3.1", "BPB_TotSec16 and BPB_TotSec32")}: the count includes every sector of the volume"
        ),
        Rule.new(
          id: "fat.geometry.fat_capacity", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::FAT_CAPACITY,
          predicate: ->(model, _) { model.geometry.fat_fits },
          source: "#{SPECIFICATION}, section 4.4: each FAT holds the entries through CountOfClusters + 1, and " \
                  "may be larger; section 3.5, CountofClusters"
        ),
        # Only a FAT32 volume can declare so many clusters. The
        # specification says no volume should; the reader may refuse it or
        # exclude those cluster numbers from allocation
        # (specs/design-fat-reader.md#identification-and-geometry).
        Rule.new(
          id: "fat.geometry.max_cluster", category: "normative", policy: "safety_only",
          scopes: ["open"], applies_to: ALWAYS, domain: Witnesses::MAX_CLUSTER,
          predicate: ->(model, _) { FsimgSpec.derive(model.geometry.max_cluster, model.geometry.variant) { _1 < BAD_CLUSTER.fetch(_2) } },
          source: "#{SPECIFICATION}, section 4, FAT entry values: the bad-cluster marker, and the note that no " \
                  "FAT32 volume should contain cluster numbers available for allocation >= 0xFFFFFF7"
        ),
        Rule.new(
          id: "fat.geometry.forced_variant", category: "profile", policy: "must_reject", status: "FSIMG_EFORMAT",
          scopes: ["open"], applies_to: ->(_model, environment) { !environment.forced_format.nil? },
          domain: Witnesses::FORCED_VARIANT,
          predicate: ->(model, environment) { FsimgSpec.derive(model.geometry.variant) { _1 == environment.forced_format } },
          source: "specs/design-fat-reader.md: an explicit format must match the validated variant; " \
                  "specs/architecture.md#specification-driven-testing, FSIMG_EFORMAT"
        )
      ]

      # A FAT32 field that locates a structure in the reserved area: its
      # rule id, the field, the values the specification recommends, and
      # the value that says there is no structure, or nil. A pointer's three
      # rules each test one value of kind, so they cannot disagree about
      # what a sector means.
      SectorPointer = Data.define(:id, :field, :recommended, :none) do
        # What sector means in model, one of a closed set: :absent for
        # 0xFFFF, which compatible readers take to mean no structure;
        # :illegal for a value that neither says there is none nor names a
        # sector of the reserved area after the boot sector; :recommended
        # for a legal value the specification recommends; :other for any
        # other legal value.
        def kind(model, sector)
          if sector == ABSENT_SECTOR then :absent
          elsif sector != none && !Model.reserved_sector?(model.common, sector) then :illegal
          elsif recommended.include?(sector) then :recommended
          else :other
          end
        end

        # The kind of the field's value in model, or Undefined.
        def kind_in(model) = FsimgSpec.derive(field.read(model)) { kind(model, _1) }
      end

      FSINFO = SectorPointer.new(
        id: "fat32.bpb.fsinfo_sector", recommended: [1], none: nil,
        field: Field.new(record: Layout::Fat32Extension, name: :bpb_fs_info, parameter: :fs_info)
      )
      BACKUP_BOOT = SectorPointer.new(
        id: "fat32.bpb.backup_boot_sector", recommended: [0, 6], none: 0,
        field: Field.new(record: Layout::Fat32Extension, name: :bpb_bk_boot_sec, parameter: :backup_boot)
      )

      # A pointer's normative rule: its value is legal, or 0xFFFF.
      def self.reserved_area_rule(pointer, source:, domain:)
        Rule.new(
          id: pointer.id, category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT", scopes: ["open"],
          applies_to: FAT32_EXTENSION, domain:, source:,
          predicate: ->(model, _) { FsimgSpec.derive(pointer.kind_in(model)) { _1 != :illegal } }
        )
      end

      # The specification recommends values for a sector field, but a
      # reader finds the structure wherever the field points within the
      # reserved area, so any other legal value is a recommendation fsimg
      # must accept. An illegal value breaks the reserved-area rule instead,
      # and 0xFFFF the absent-sector rule.
      def self.recommended_sector_rule(pointer, source:)
        Rule.new(
          id: "#{pointer.id}.recommended", category: "advisory", policy: "must_accept", scopes: ["open"],
          applies_to: FAT32_EXTENSION, domain: Witnesses.recommended(pointer.field), source:,
          predicate: ->(model, _) { FsimgSpec.derive(pointer.kind_in(model)) { _1 != :other } }
        )
      end

      # 0xFFFF in a sector field: the specification gives it no meaning,
      # but compatible readers take it to mean the structure is absent,
      # and so does the rule set: a reader accepts the volume without the
      # structure (docs/spec-testing-plan.md#initial-fat-rule-set).
      def self.absent_sector_rule(pointer, source:)
        Rule.new(
          id: "#{pointer.id}.absent", category: "advisory", policy: "must_accept", scopes: ["open"],
          applies_to: FAT32_EXTENSION, domain: Witnesses.absent(pointer.field),
          source: "#{source}, which gives 0xFFFF no meaning; reading it as absent is a compatibility convention",
          predicate: ->(model, _) { FsimgSpec.derive(pointer.kind_in(model)) { _1 != :absent } }
        )
      end

      # The FAT32 extension (section 3.3), read only over a FAT32 BPB.
      def self.fat32_rules = [
        Rule.new(
          id: "fat32.bpb.ext_flags", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: FAT32_EXTENSION, domain: Witnesses::EXT_FLAGS,
          predicate: lambda { |model, _|
            FsimgSpec.derive(model.extension) { Rules.active_fat_exists(_1.bpb_ext_flags, model.common.bpb_num_fats) }
          },
          source: cite("3.3", "BPB_ExtFlags: with mirroring disabled (bit 7), bits 0-3 are the zero-based number " \
                              "of the one active FAT")
        ),
        # Section 3.3 calls the other bits reserved without saying what they
        # hold, unlike BPB_Reserved, which "must be set to 0x0".
        Rule.new(
          id: "fat32.bpb.ext_flags.reserved", category: "profile", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: FAT32_EXTENSION, domain: Witnesses::EXT_FLAGS_RESERVED,
          predicate: ->(model, _) { FsimgSpec.derive(model.extension) { _1.bpb_ext_flags.nobits?(EXT_FLAGS_RESERVED_BITS) } },
          source: "specs/design-fat-reader.md#identification-and-geometry: the reader refuses a FAT32 BPB_ExtFlags " \
                  "with a reserved bit (4-6 or 8-15) set as corrupt"
        ),
        field_rule("fat32.bpb.fs_version", category: "normative", policy: "must_reject", status: "FSIMG_EUNSUPPORTED",
                                           domain: Domain.new(legal: [0], invalid: [0x0100, 0xFFFF], boundaries: [0]),
                                           source: cite("3.3", "BPB_FSVer"), applies_to: FAT32_EXTENSION),
        Rule.new(
          id: "fat32.bpb.reserved", category: "normative", policy: "must_reject", status: "FSIMG_EUNSUPPORTED",
          scopes: ["open"], applies_to: FAT32_EXTENSION, domain: Witnesses::FAT32_RESERVED,
          predicate: ->(model, _) { FsimgSpec.derive(model.extension) { _1.bpb_reserved.bytes.all?(&:zero?) } },
          source: cite("3.3", "BPB_Reserved")
        ),
        Rule.new(
          id: "fat32.bpb.root_cluster", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["open"], applies_to: FAT32_EXTENSION, domain: Witnesses::ROOT_CLUSTER,
          predicate: ->(model, _) { FsimgSpec.derive(model.extension, model.geometry.max_cluster) { (2.._2).cover?(_1.bpb_root_clus) } },
          source: cite("3.3", "BPB_RootClus")
        ),
        reserved_area_rule(FSINFO, domain: Witnesses::FSINFO_SECTOR, source: cite("3.3", "BPB_FSInfo")),
        recommended_sector_rule(
          FSINFO,
          source: "#{cite("3.3", "BPB_FSInfo")} (\"Usually 1\"); section 5: \"The structure must be located at " \
                  "sector #1\". The field locates FSInfo, so the rule set reads section 5 as a requirement of " \
                  "formatting, not of reading (docs/spec-testing-plan.md#initial-fat-rule-set)"
        ),
        absent_sector_rule(FSINFO, source: cite("3.3", "BPB_FSInfo")),
        # The FSInfo record a legal BPB_FSInfo locates carries three
        # signatures. The reader checks them when it verifies the volume
        # (specs/design-fat-reader.md), and so does this rule on fsimg's
        # side, whatever the host's checker reports.
        Rule.new(
          id: "fat32.fsinfo.signatures", category: "normative", policy: "must_reject", status: "FSIMG_ECORRUPT",
          scopes: ["verification"], applies_to: ->(model, _) { FsimgSpec.derive(model.fsinfo) { !_1.nil? } },
          domain: Witnesses::FSINFO_SIGNATURES,
          predicate: lambda { |model, _|
            FsimgSpec.derive(model.fsinfo) { |info| FSINFO_SIGNATURES.all? { info.public_send(_1) == _2 } }
          },
          source: cite("5", "FSI_LeadSig, FSI_StrucSig and FSI_TrailSig")
        ),
        # Section 3.6 places a copy of the whole three-sector boot record
        # at BPB_BkBootSec, but a reader uses only the BPB copy in its first
        # sector, so the rule set requires only that sector in the reserved
        # area; the rest of the record is formatting, which the recommended
        # sector 6 leaves room for.
        reserved_area_rule(
          BACKUP_BOOT, domain: Witnesses::BACKUP_BOOT_SECTOR,
          source: "#{cite("3.3", "BPB_BkBootSec")}; section 3.6, Backup BPB Structure"
        ),
        recommended_sector_rule(
          BACKUP_BOOT,
          source: "#{cite("3.3", "BPB_BkBootSec")} (\"Set to 0 or 6\"); section 3.6: \"On FAT32 formatted volumes, " \
                  "sector #6 must contain a copy of the BPB\". The field locates the copy, or with 0 says there is " \
                  "none, so the rule set reads 3.6 as a requirement of formatting " \
                  "(docs/spec-testing-plan.md#initial-fat-rule-set)"
        ),
        absent_sector_rule(BACKUP_BOOT, source: cite("3.3", "BPB_BkBootSec"))
      ]

      # With mirroring disabled (bit 7), bits 0-3 of BPB_ExtFlags are the
      # zero-based number of the one active FAT, which must exist. A
      # mirrored volume ignores them.
      def self.active_fat_exists(flags, fat_count) = flags.nobits?(0x80) || (flags & 0x0F) < fat_count

      private_class_method :field_rule, :cite, :boot_rules, :bpb_rules, :geometry_rules, :fat32_rules, :name_rules,
                           :creation_rules, :source_refusals, :sizing_rules, :setting?, :sectored_setting?, :base_name, :parent_path, :distinct_siblings, :reserved_area_rule,
                           :recommended_sector_rule, :absent_sector_rule
    end
  end
end
