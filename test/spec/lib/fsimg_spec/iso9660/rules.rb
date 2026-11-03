# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # Logical requirements over independently decoded records. Each rule
    # cites the format contract and the scope at which its reader checks it.
    # The generator attaches a valid and an invalid witness to each rule.
    module Rules
      ALWAYS = ->(_model, _environment) { true }
      VOLUME = ->(model, _) { !model.volume.nil? }
      IDENTITY = ->(model, _) { identity?(model) }
      BLOCK = Model::BLOCK
      KNOWN_RR = %w[PX PN TF NM SL CL PL RE RR SF ZF].freeze

      module_function

      def registry = Registry.new(all)

      def rule(id:, scope:, anchor:, predicate:, status: "FSIMG_ECORRUPT", category: "normative", applies_to: ALWAYS)
        Rule.new(id: "iso9660.#{id}", category:, policy: "must_reject", status:,
                 scopes: Array(scope), applies_to:, predicate:,
                 domain: -> { Witnesses.for_rule("iso9660.#{id}") },
                 source: "specs/design-iso9660.md##{anchor}")
      end

      def all = [
        rule(id: "identity", scope: "open", anchor: "the-volume-descriptor-set", status: "FSIMG_EFORMAT",
             predicate: ->(m, _) { identity?(m) }),
        rule(id: "descriptor.identity", scope: "open", anchor: "descriptor-scan-rule", applies_to: IDENTITY,
             predicate: ->(m, _) { m.descriptors.drop(1).all? { _1.fields.identifier == "CD001" } }),
        rule(id: "descriptor.profile", scope: "open", anchor: "reader-profile", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: IDENTITY,
             predicate: ->(m, _) { m.descriptors.all? { descriptor_supported?(_1.fields) } }),
        rule(id: "descriptor.limit", scope: "open", anchor: "descriptor-scan-rule", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: IDENTITY,
             predicate: ->(m, _) { m.descriptors.count { _1.fields.descriptor_type != 255 } <= 64 }),
        rule(id: "descriptor.terminator", scope: "open", anchor: "descriptor-scan-rule", applies_to: ->(m, _) { identity?(m) && (m.descriptors.size < 65 || m.descriptors.last.fields.descriptor_type == 255) },
             predicate: ->(m, _) { m.descriptors.last.fields.descriptor_type == 255 && m.descriptors.last.raw.byteslice(7, BLOCK - 7).bytes.all?(&:zero?) }),
        rule(id: "primary.present", scope: "open", anchor: "descriptor-scan-rule", applies_to: IDENTITY,
             predicate: ->(m, _) { !m.volume.nil? }),
        rule(id: "primary.duplicates", scope: "open", anchor: "descriptor-scan-rule", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: VOLUME,
             predicate: ->(m, _) { m.descriptors.select { _1.fields.descriptor_type == 1 }.all? { _1.raw == m.volume.raw } }),
        rule(id: "numeric.copies", scope: "open", anchor: "both-byte-order-fields", applies_to: VOLUME,
             predicate: ->(m, _) { volumes(m).all? { descriptor_copies?(_1.fields) } }),
        rule(id: "geometry.profile", scope: "open", anchor: "reader-profile", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: VOLUME,
             predicate: ->(m, _) { volumes(m).all? { _1.fields.block_bytes_le == BLOCK && _1.fields.set_size_le == 1 && _1.fields.sequence_le == 1 && _1.fields.structure_version == 1 } }),
        rule(id: "geometry.volume", scope: "open", anchor: "logical-blocks-and-addresses", applies_to: VOLUME,
             predicate: ->(m, _) { m.volume.fields.space_le.positive? }),
        rule(id: "root.descriptor", scope: "open", anchor: "dot-records", applies_to: VOLUME,
             predicate: ->(m, _) { volumes(m).all? { root_shape?(_1.fields.root_record) } }),
        rule(id: "root.range", scope: "open", anchor: "logical-blocks-and-addresses", applies_to: VOLUME,
             predicate: ->(m, _) { volumes(m).all? { root_range?(_1.fields) } && Layout.decode(Layout::DirectoryHeader, m.volume.fields.root_record).extent_le < m.image_length / BLOCK }),
        rule(id: "descriptor.dates", scope: "verification", anchor: "descriptor-dates", applies_to: VOLUME,
             predicate: ->(m, _) { volumes(m).all? { _1.fields.dates.bytes.each_slice(17).all? { |bytes| date17?(bytes.pack("C*")) } } }),
        rule(id: "joliet.geometry", scope: "open", anchor: "supplementary-descriptors-and-joliet", applies_to: VOLUME,
             predicate: ->(m, _) { joliets(m).all? { _1.fields.volume_flags.zero? && geometry(_1.fields) == geometry(m.volume.fields) } }),
        rule(id: "joliet.count", scope: "open", anchor: "supplementary-descriptors-and-joliet", status: "FSIMG_EUNSUPPORTED",
             category: "profile", predicate: ->(m, _) { joliets(m).size <= 1 }),
        rule(id: "directory.framing", scope: %w[open directories verification], anchor: "framing-rule",
             predicate: ->(m, _) { record_scopes(m) { framed?(_1) } + directory_scopes(m) { _1.available == _1.bytes && _1.tails.all? { |tail| tail.bytes.all?(&:zero?) } } }),
        rule(id: "directory.copies", scope: %w[open directories verification], anchor: "both-byte-order-fields",
             predicate: ->(m, _) { record_scopes(m) { FsimgSpec.derive(_1.fields) { |f| f.extent_le == f.extent_be && f.data_bytes_le == f.data_bytes_be && f.sequence_le == f.sequence_be } != false } }),
        rule(id: "directory.profile", scope: %w[open directories verification], anchor: "reader-profile", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { record_scopes(m) { FsimgSpec.derive(_1.fields) { |f| f.unit_bytes.zero? && f.interleave_gap.zero? && f.flags & 4 == 0 && f.sequence_le == 1 && (f.flags & 2 == 0 || f.attribute_blocks.zero?) && (f.flags & 0x18 == 0 || f.attribute_blocks.zero?) } != false } }),
        rule(id: "directory.flags", scope: %w[open directories verification], anchor: "flags",
             predicate: ->(m, _) { record_scopes(m) { FsimgSpec.derive(_1.fields) { |f| f.flags & 0x60 == 0 && !(f.flags & 2 != 0 && f.flags & 0x80 != 0) && (f.flags & 0x18 == 0 || f.attribute_blocks.positive?) } != false } }),
        rule(id: "directory.dates", scope: %w[open directories verification], anchor: "recording-dates",
             predicate: ->(m, _) { record_scopes(m) { FsimgSpec.derive(_1.fields) { |f| date7?(f.date) } != false } }),
        rule(id: "directory.dots", scope: %w[directories verification], anchor: "dot-records",
             predicate: ->(m, _) { dot_scopes(m) }),
        rule(id: "names.primary", scope: %w[directories verification], anchor: "primary-identifiers",
             predicate: ->(m, _) { primary_name_scopes(m) }),
        rule(id: "names.joliet", scope: %w[directories verification], anchor: "joliet-identifiers",
             predicate: ->(m, _) { joliet_name_scopes(m) }),
        rule(id: "names.unique", scope: %w[directories verification], anchor: "displayed-names-and-keys",
             predicate: ->(m, _) { namespace_scopes(m) }),
        rule(id: "sections.framing", scope: %w[directories verification], anchor: "file-sections",
             predicate: ->(m, _) { directory_scopes(m) { sections?(_1) } }),
        rule(id: "sections.ascending", scope: %w[directories verification], anchor: "file-sections", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { directory_scopes(m) { ascending?(_1) } }),
        rule(id: "payload.bounds", scope: %w[directories verification], anchor: "logical-blocks-and-addresses",
             predicate: ->(m, _) { payload_scopes(m) }),
        rule(id: "path.tables", scope: "verification", anchor: "path-tables",
             predicate: ->(m, _) { m.hierarchies.all? { path_tables?(_1) } }),
        rule(id: "primary.order", scope: "verification", anchor: "ordering", category: "profile",
             predicate: ->(m, _) { FsimgSpec.undefined?(m.primary) || m.primary.directories.all? { ordered?(_1) } }),
        rule(id: "ownership", scope: "verification", anchor: "ownership",
             predicate: ->(m, _) { ownership?(m) }),
        rule(id: "susp.framing", scope: %w[open directories verification], anchor: "entry-framing-and-continuation",
             predicate: ->(m, _) { scopes_of(m, m.susp_entries.reject { susp_framed?(_1) }) }),
        rule(id: "susp.er", scope: "open", anchor: "the-handshake",
             predicate: ->(m, _) { m.susp_entries.select { _1.scope == "open" && _1.fields.signature == "ER" }.all? { RockRidge.extension_shape?(_1.raw) } }),
        rule(id: "susp.ce", scope: %w[open directories verification], anchor: "entry-framing-and-continuation",
             predicate: ->(m, _) { invalid_ce_scopes(m) }),
        rule(id: "susp.ce_profile", scope: %w[open directories verification], anchor: "entry-framing-and-continuation", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { continuation_profile_scopes(m) }),
        rule(id: "susp.skip", scope: "verification", anchor: "the-handshake", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { m.susp_entries.select { _1.fields.signature == "SP" && _1.scope == "open" }.all? { _1.raw.getbyte(6) == 0 } }),
        rule(id: "rr.version", scope: %w[open directories], anchor: "entries",
             predicate: ->(m, _) { rr_version_scopes(m) }),
        rule(id: "rr.lengths", scope: "directories", anchor: "entries",
             predicate: ->(m, _) { m.rr_entries.all? { rr_length?(_1) } }),
        rule(id: "rr.px", scope: "directories", anchor: "entries",
             predicate: ->(m, _) { m.rr_entries.select { _1.fields.signature == "PX" }.all? { |entry| [36, 44].include?(entry.raw.bytesize) && (4...entry.raw.bytesize).step(8).all? { |at| both32?(entry.raw, at) } } }),
        rule(id: "rr.tf", scope: "directories", anchor: "entries",
             predicate: ->(m, _) { m.rr_entries.select { _1.fields.signature == "TF" }.all? { tf?(_1.raw) } }),
        rule(id: "rr.singletons", scope: "directories", anchor: "entries",
             predicate: ->(m, _) { m.rr_records.all? { _1.entries.map { |entry| entry.fields.signature }.select { |name| %w[PX PN CL PL RE TF].include?(name) }.tally.values.all? { |count| count == 1 } } }),
        rule(id: "rr.names", scope: "directories", anchor: "rock-ridge-names",
             predicate: ->(m, _) { m.rr_records.all? { nm?(_1) } }),
        rule(id: "rr.name_profile", scope: "directories", anchor: "rock-ridge-names", category: "profile", status: "FSIMG_EUNSUPPORTED",
             predicate: ->(m, _) { m.rr_records.all? { !nm?(_1) || name_bytes(_1) <= 1024 } }),
        rule(id: "rr.pointers", scope: "directories", anchor: "relocation",
             predicate: ->(m, _) { m.rr_entries.select { %w[CL PL].include?(_1.fields.signature) }.all? { _1.raw.bytesize == 12 && both32?(_1.raw, 4) } }),
        rule(id: "rr.kind", scope: "directories", anchor: "kinds-and-metadata",
             predicate: ->(m, _) { m.rr_records.all? { rr_kind?(_1) } }),
        rule(id: "rr.kind_profile", scope: "directories", anchor: "reader-profile", category: "profile", status: "FSIMG_EUNSUPPORTED",
             predicate: ->(m, _) { m.rr_records.none? { !FsimgSpec.undefined?(_1.fields) && rr_kind?(_1) && RockRidge.kind(_1) == "unsupported" } }),
        rule(id: "rr.sl.framing", scope: "directories", anchor: "symbolic-links",
             predicate: ->(m, _) { link_records(m).all? { RockRidge.target(_1).framed } }),
        rule(id: "rr.sl.components", scope: "directories", anchor: "symbolic-links",
             predicate: ->(m, _) { link_records(m).all? { RockRidge.components_valid?(RockRidge.target(_1)) } }),
        rule(id: "rr.sl.profile", scope: "directories", anchor: "symbolic-links", category: "profile", status: "FSIMG_EUNSUPPORTED",
             predicate: ->(m, _) { link_records(m).all? { target = RockRidge.target(_1); !target.framed || !RockRidge.components_valid?(target) || (target.length <= 1024 && target.supported) } }),
        rule(id: "extract.safe_kinds", scope: "destination", anchor: "kinds-and-metadata", category: "profile", status: "FSIMG_EUNSUPPORTED",
             predicate: ->(m, _) { FsimgSpec.derive(m.tree) { |tree| tree.none? { %w[symlink fifo].include?(_1.type) } } }),
        rule(id: "extract.omitted_names", scope: "destination", anchor: "displayed-names-and-keys", category: "profile", status: "FSIMG_EUNSUPPORTED",
             predicate: ->(m, _) { m.omitted_names.empty? }),
        rule(id: "rr.unsupported", scope: "directories", anchor: "entries", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { m.rr_entries.none? { %w[SF ZF].include?(_1.fields.signature) } }),
        rule(id: "relocation.parents", scope: "directories", anchor: "relocation",
             predicate: ->(m, _) { relocation_parents?(m) }),
        rule(id: "relocation.targets", scope: "verification", anchor: "relocation",
             predicate: ->(m, _) { relocation_targets?(m) }),
        rule(id: "boot.validation", scope: "open", anchor: "boot-catalog",
             predicate: ->(m, _) { m.catalogs.all? { boot_validation?(_1.last) } }),
        rule(id: "boot.profile", scope: "open", anchor: "reader-profile", status: "FSIMG_EUNSUPPORTED", category: "profile",
             predicate: ->(m, _) { boot_profile?(m) }),
        rule(id: "boot.sections", scope: "open", anchor: "boot-catalog",
             predicate: ->(m, _) { m.catalogs.all? { facts = Catalog.decode(_1.last); facts.framed || facts.overflow } }),
        rule(id: "boot.entry", scope: "open", anchor: "boot-catalog",
             predicate: ->(m, _) { m.catalogs.all? { boot_entry?(_1.last, m) } })
      ] + creation_rules

      def input_rule(id:, anchor:, status:, predicate:)
        Rule.new(id: "iso9660.create.#{id}", category: "profile", policy: "must_reject", status:,
                 scopes: ["input"], applies_to: ALWAYS, predicate:,
                 domain: -> { Witnesses.creation_for_rule("iso9660.create.#{id}") },
                 source: "specs/design-iso9660.md##{anchor}")
      end

      # Character classes and capacities are independent of the C codec.
      A_TEXT_KEYS = %i[volset publisher preparer application].freeze
      FILE_TEXT_KEYS = %i[copyright abstract biblio].freeze
      DATE_KEYS = %i[created modified expires effective].freeze
      A_CHARACTERS = /\A[A-Z0-9_ !"%&'()*+,\.\/:;<=>?\-]*\z/
      FILE_CHARACTERS = /\A[A-Z0-9_.; ]*\z/

      def descriptor_text?(settings)
        settings.slice(*A_TEXT_KEYS, *FILE_TEXT_KEYS).all? do |key, value|
          capacity, characters = A_TEXT_KEYS.include?(key) ? [64, A_CHARACTERS] : [18, FILE_CHARACTERS]
          value.bytesize <= capacity && characters.match?(value)
        end
      end

      def creation_rules = [
        input_rule(id: "descriptor_text", anchor: "descriptor-text-and-dates", status: "FSIMG_EINVAL",
          predicate: ->(source, _) { descriptor_text?(source.settings) }),
        input_rule(id: "descriptor_dates", anchor: "descriptor-text-and-dates", status: "FSIMG_EINVAL",
          predicate: ->(source, _) { source.settings.slice(*DATE_KEYS).values.all? { (-62_135_596_800..253_402_300_799).cover?(_1) } }),
        input_rule(id: "geometry", anchor: "settings-and-sizing", status: "FSIMG_EINVAL",
          predicate: ->(source, _) { (!source.settings.key?(:sector) || source.settings[:sector] == BLOCK) &&
            (!source.settings.key?(:cluster) || source.settings[:cluster] == BLOCK) &&
            (!source.settings.key?(:size) || source.settings[:size].positive? && source.settings[:size] % BLOCK == 0) }),
        input_rule(id: "name_bytes", anchor: "rock-ridge-entries-and-continuation", status: "FSIMG_EUNSUPPORTED",
          predicate: ->(source, _) { source.tree.all? { _1.path.split("/").all? { |component| component.bytesize <= 255 } } }),
        input_rule(id: "target_bytes", anchor: "rock-ridge-entries-and-continuation", status: "FSIMG_EUNSUPPORTED",
          predicate: ->(source, _) { source.tree.select { _1.type == "symlink" }.all? { _1.target.bytesize <= 1023 && _1.target.split("/".b, -1).all? { |component| component.bytesize <= 255 } } }),
        input_rule(id: "boot_paths", anchor: "el-torito-1", status: "FSIMG_EINVAL",
          predicate: ->(source, _) { %i[boot efiboot].all? { !source.settings.key?(_1) || boot_file(source, source.settings[_1]) } }),
        input_rule(id: "bootload", anchor: "settings-and-sizing", status: "FSIMG_EINVAL",
          predicate: ->(source, _) { bootload?(source) }),
        input_rule(id: "file_sections", anchor: "file-sections-1", status: "FSIMG_EUNSUPPORTED",
          predicate: ->(source, _) { source.tree.none? { _1.type == "file" && _1.size > 0xffff_ffff && source.settings.fetch(:level, 3) < 3 } }),
        input_rule(id: "volume_lower_bound", anchor: "layout", status: "FSIMG_ERANGE",
          predicate: ->(source, _) { !source.settings.key?(:size) || source.settings[:size] % BLOCK != 0 || source.settings[:size] >= minimum_volume(source) }),
        input_rule(id: "volume_ceiling", anchor: "settings-and-sizing", status: "FSIMG_ERANGE",
          predicate: ->(source, _) { !source.settings.key?(:size) || source.settings[:size] <= 0xffff_ffff * BLOCK })
      ]

      def boot_file(source, path)
        return nil unless path.start_with?("/") && !path.end_with?("/") &&
          path.delete_prefix("/").split("/", -1).none? { ["", ".", ".."].include?(_1) }

        source.tree.find { _1.path == path.delete_prefix("/") && _1.type == "file" && _1.size.between?(1, 0xffff_ffff) }
      end

      def bootload?(source)
        return true unless source.settings.key?(:bootload)
        return false unless source.settings.key?(:boot)

        file = boot_file(source, source.settings[:boot])
        !file || source.settings[:bootload] <= 4 * ((file.size + BLOCK - 1) / BLOCK)
      end

      # Every writer image has 16 system blocks, three descriptors, four
      # root path tables, two root directories, and the root ER continuation.
      # Booting adds a descriptor and catalog. Files are never deduplicated.
      # Other directories and continuation entries can only increase this
      # lower bound; satisfying it does not establish exact capacity fit.
      def minimum_volume(source)
        metadata = 26 + (%i[boot efiboot].any? { source.settings.key?(_1) } ? 2 : 0)
        payload = source.tree.select { _1.type == "file" }.sum { (_1.size + BLOCK - 1) / BLOCK }
        (metadata + payload) * BLOCK
      end

      def identity?(model) = model.image_length >= 17 * BLOCK && !model.descriptors.empty? &&
        model.descriptors.first.fields.identifier == "CD001" && [0, 1, 2, 3, 255].include?(model.descriptors.first.fields.descriptor_type)
      def descriptor_supported?(f) = [0, 1, 2, 3, 255].include?(f.descriptor_type) &&
        (f.descriptor_version == 1 || (f.descriptor_type == 2 && f.descriptor_version == 2))
      def joliets(model) = model.descriptors.select { _1.fields.descriptor_type == 2 && ["%/@".b, "%/C".b, "%/E".b].include?(_1.fields.escapes.byteslice(0, 3)) }
      def volumes(model) = model.descriptors.select { _1.fields.descriptor_type == 1 } + joliets(model)
      def geometry(f) = [f.space_le, f.set_size_le, f.sequence_le, f.block_bytes_le]
      def descriptor_copies?(f) = f.space_le == f.space_be && f.set_size_le == f.set_size_be && f.sequence_le == f.sequence_be &&
        f.block_bytes_le == f.block_bytes_be && f.path_bytes_le == f.path_bytes_be
      def root_shape?(raw)
        f = Layout.decode(Layout::DirectoryHeader, raw)
        f.record_bytes == 34 && f.identifier_bytes == 1 && raw.getbyte(33).zero? && f.flags & 2 != 0
      end
      def root_range?(f)
        root = Layout.decode(Layout::DirectoryHeader, f.root_record)
        root.data_bytes_le.positive? && root.extent_le < f.space_le &&
          root.extent_le + (root.data_bytes_le + BLOCK - 1) / BLOCK <= f.space_le
      end
      def date7?(raw)
        return true if raw.bytes.all?(&:zero?)
        return false unless raw.bytesize == 7

        year, month, day, hour, minute, second, zone = raw.unpack("C6c")
        Date.valid_date?(1900 + year, month, day) && hour <= 23 && minute <= 59 && second <= 59 && zone.between?(-48, 52)
      end
      def date17?(raw)
        return false unless raw.bytesize == 17
        return true if ["0" * 16, "\0" * 16].include?(raw.byteslice(0, 16)) && raw.getbyte(16).zero?
        return false unless raw.byteslice(0, 16).match?(/\A[0-9]{16}\z/n)

        year, month, day, hour, minute, second = [raw.byteslice(0, 4), *[4, 6, 8, 10, 12].map { raw.byteslice(_1, 2) }].map(&:to_i)
        Date.valid_date?(year, month, day) && year.positive? && hour <= 23 && minute <= 59 && second <= 59 && raw.byteslice(16, 1).unpack1("c").between?(-48, 52)
      end
      def framed?(record)
        return false if FsimgSpec.undefined?(record.fields) || FsimgSpec.undefined?(record.identifier)

        f = record.fields
        used = 33 + f.identifier_bytes + (f.identifier_bytes.even? ? 1 : 0)
        f.record_bytes >= 34 && f.record_bytes.even? && f.record_bytes == record.raw.bytesize && f.identifier_bytes.positive? &&
          used <= record.raw.bytesize && record.offset % BLOCK + f.record_bytes <= BLOCK &&
          (f.identifier_bytes.odd? || record.raw.getbyte(33 + f.identifier_bytes).zero?)
      end
      def record_scope_index(model)
        visible = model.prepared_records.to_set(&:offset)
        model.records.to_h do |record|
          scope = if record.scope == "open" then "open"
                  elsif visible.include?(record.offset) then "directories"
                  else "verification"
                  end
          [record.offset, scope]
        end
      end
      def record_scopes(model)
        scopes = record_scope_index(model)
        model.records.filter_map { scopes.fetch(_1.offset) unless yield(_1) }.uniq
      end
      def payload_scopes(model)
        records = data_records(model)
        return [] if records.empty?

        scopes = record_scope_index(model)
        limit = [model.image_length / BLOCK, model.volume.fields.space_le].min
        records.filter_map do |record|
          fields = record.fields
          end_block = fields.extent_le + fields.attribute_blocks + (fields.data_bytes_le + BLOCK - 1) / BLOCK
          scopes.fetch(record.offset) if end_block > limit
        end.uniq
      end
      def directory_scopes(model)
        scopes = record_scope_index(model)
        model.hierarchies.flat_map(&:directories).filter_map do |directory|
          next if yield(directory)

          first = directory.records.first
          first ? scopes.fetch(first.offset) : (directory.joliet && model.view != "joliet" ? "verification" : "directories")
        end.uniq
      end
      def primary_name_scopes(model)
        return [] if FsimgSpec.undefined?(model.primary)

        visible = model.rr_records.to_set(&:offset)
        model.primary.directories.flat_map { groups(_1).map(&:first) }.filter_map do |record|
          next if primary_name?(record.identifier, directory: !FsimgSpec.undefined?(record.fields) && record.fields.flags & 2 != 0)

          uses_primary = model.view == "primary" || (visible.include?(record.offset) && record.entries.none? { _1.fields.signature == "NM" })
          uses_primary ? "directories" : "verification"
        end.uniq
      end
      def joliet_name_scopes(model)
        return [] if FsimgSpec.undefined?(model.joliet)

        invalid = model.joliet.directories.flat_map { groups(_1).map(&:first) }.any? do |record|
          !FsimgSpec.undefined?(record.identifier) &&
            (record.identifier.bytesize.odd? || !Names.native?(Names.decode(record, view: "joliet"), view: "joliet"))
        end
        invalid ? [model.view == "joliet" ? "directories" : "verification"] : []
      end
      def dots?(directory)
        pair = directory.records.take(2)
        return false unless pair.size == 2 && pair.all? { !FsimgSpec.undefined?(_1.fields) }

        pair.each_with_index.all? do |record, index|
          record.identifier == [index].pack("C") && record.fields.flags & 2 != 0 &&
            (!index.zero? || record.fields.extent_le == directory.extent) &&
            (!index.zero? || record.fields.data_bytes_le == directory.bytes)
        end && directory.records.drop(2).none? { ["\0".b, "\x01".b].include?(_1.identifier) }
      end
      def dot_scopes(model)
        scopes = record_scope_index(model)
        model.hierarchies.flat_map do |hierarchy|
          directories = hierarchy.directories.to_h { [_1.extent, _1] }
          root = hierarchy.root.fields
          hierarchy.directories.flat_map do |directory|
            own_scope = directory.records.first ? scopes.fetch(directory.records.first.offset) : "directories"
            broken = []
            broken << own_scope unless dots?(directory)
            parent_record = directory.records.fetch(1, nil)
            if parent_record && !FsimgSpec.undefined?(parent_record.fields) && parent_record.fields.extent_le != directory.parent
              relocated = parent_record.entries.any? { _1.fields.signature == "PL" }
              broken << (model.view == "rockridge" && relocated ? "verification" : own_scope)
            end
            directory.records.drop(2).each do |record|
              next if FsimgSpec.undefined?(record.fields) || record.fields.flags & 2 == 0

              target = directories[record.fields.extent_le]
              target_parent = target&.records&.fetch(1, nil)
              relation = target_parent && !FsimgSpec.undefined?(target_parent.fields) &&
                target_parent.fields.extent_le == directory.extent &&
                (FsimgSpec.undefined?(root) || record.fields.extent_le != root.extent_le)
              next if relation

              extension = record.entries.any? { %w[CL RE].include?(_1.fields.signature) }
              broken << (model.view == "rockridge" && extension ? "verification" : scopes.fetch(record.offset))
            end
            broken
          end
        end.uniq
      end
      def primary_name?(name, directory:)
        return true if FsimgSpec.undefined?(name)
        return false if name.empty? || name.include?("\0") || name.include?("/")
        return false unless Names.native?(name.delete_suffix(";1".b), view: "primary")

        version = directory ? nil : name.split(";", -1).drop(1).last
        !version || version.match?(/\A[1-9][0-9]{0,4}\z/n) && version.to_i <= 32767
      end
      def groups(directory)
        directory.records.drop(2).each_with_object([]) do |record, runs|
          if runs.any? && !FsimgSpec.undefined?(runs.last.last.fields) && runs.last.last.fields.flags & 0x80 != 0
            runs.last << record
          else
            runs << [record]
          end
        end
      end
      def unique_names?(directory, view:)
        records = groups(directory).map(&:first)
        records = records.reject { _1.entries.any? { |entry| entry.fields.signature == "RE" } } if view == "rockridge"
        defined = records.map { Names.decode(_1, view:) }.reject { FsimgSpec.undefined?(_1) }
        extents = records.filter_map do |record|
          next if FsimgSpec.undefined?(record.fields)

          link = view == "rockridge" && record.entries.find { _1.fields.signature == "CL" && _1.raw.bytesize == 12 }
          if link then link.raw.byteslice(4, 4).unpack1("V")
          elsif record.fields.flags & 2 != 0 then record.fields.extent_le
          end
        end
        defined.uniq.size == defined.size && extents.uniq.size == extents.size
      end
      def namespace_scopes(model)
        scopes = model.hierarchies.flat_map do |hierarchy|
          view = hierarchy.descriptor.fields.descriptor_type == 2 ? "joliet" : "primary"
          scope = model.view == view ? "directories" : "verification"
          hierarchy.directories.filter_map { scope unless unique_names?(_1, view:) }
        end
        unless FsimgSpec.undefined?(model.primary) || !model.primary.rockridge
          visible = model.rr_records.map(&:offset).to_set
          scopes += model.primary.directories.filter_map do |directory|
            next unless directory.records.any? { visible.include?(_1.offset) }

            "directories" unless unique_names?(directory, view: "rockridge")
          end
        end
        scopes.uniq
      end
      def sections?(directory)
        groups(directory).all? do |run|
          next true if run.any? { FsimgSpec.undefined?(_1.fields) }

          run.map(&:identifier).uniq.one? && run.last.fields.flags & 0x80 == 0 &&
            run.take(run.size - 1).all? { _1.fields.data_bytes_le.positive? && _1.fields.data_bytes_le % BLOCK == 0 }
        end
      end
      def ascending?(directory)
        groups(directory).all? do |run|
          defined = run.reject { FsimgSpec.undefined?(_1.fields) || _1.fields.data_bytes_le.zero? }
          defined.each_cons(2).all? { |a, b| b.fields.extent_le >= a.fields.extent_le + (a.fields.data_bytes_le + BLOCK - 1) / BLOCK }
        end
      end
      def data_records(model) = model.records.reject { FsimgSpec.undefined?(_1.fields) || _1.fields.flags & 2 != 0 || _1.fields.data_bytes_le.zero? }
      def path_tables?(hierarchy)
        left, right = hierarchy.paths_l, hierarchy.paths_m
        return false unless left.size == right.size && left.size == hierarchy.directories.size
        return false unless left.sum { _1.raw.bytesize } == hierarchy.descriptor.fields.path_bytes_le
        return false unless left.zip(right).all? { |a, b| a.identifier == b.identifier && a.extent == b.extent && a.parent == b.parent && a.attribute_blocks == b.attribute_blocks && a.raw.bytesize == 8 + a.identifier.bytesize + a.identifier.bytesize % 2 && (a.identifier.bytesize.even? || a.raw.getbyte(a.raw.bytesize - 1).zero?) }
        if hierarchy.descriptor.fields.descriptor_type == 1
          return false unless left.drop(1).each_cons(2).all? { |a, b| a.parent < b.parent || (a.parent == b.parent && compare_identifiers(a.identifier, b.identifier, directory_a: true, directory_b: true) <= 0) }
        end

        directories = hierarchy.directories.to_h { [_1.extent, _1] }
        left.each_with_index.all? do |record, index|
          directory = directories[record.extent]
          parent = record.parent - 1
          directory && record.attribute_blocks.zero? && record.identifier == directory.identifier &&
            (index.zero? ? record.parent == 1 : parent.between?(0, index - 1)) &&
            directory.parent == left.fetch(parent, record).extent
        end
      end
      def ordered?(directory)
        records = groups(directory).map(&:first).reject { FsimgSpec.undefined?(_1.identifier) || FsimgSpec.undefined?(_1.fields) }
        previous = nil
        file_version = nil
        records.all? do |record|
          directory_kind = record.fields.flags & 2 != 0
          parts = identifier_parts(record.identifier, directory: directory_kind)
          order = previous ? compare_parts(previous, parts) : -1
          next false if order.positive?

          file_version = nil if order.negative?
          if !directory_kind && file_version && parts[2] > file_version
            next false
          end
          file_version = parts[2] unless directory_kind
          previous = parts
          true
        end
      end
      def compare_identifiers(a, b, directory_a:, directory_b:)
        one = identifier_parts(a, directory: directory_a)
        two = identifier_parts(b, directory: directory_b)
        order = compare_parts(one, two)
        return order unless order.zero?

        directory_a || directory_b ? 0 : two[2] <=> one[2]
      end
      def compare_parts(one, two)
        [0, 1].each do |part|
          width = [one[part].bytesize, two[part].bytesize].max
          order = one[part].ljust(width, " ") <=> two[part].ljust(width, " ")
          return order unless order.zero?
        end
        0
      end
      def identifier_parts(identifier, directory:)
        return [identifier, "".b, 0] if directory

        stem, separator, version = identifier.rpartition(";".b)
        stem = identifier if separator.empty?
        name, _, extension = stem.partition(".".b)
        [name, extension, version.to_i]
      end
      def ownership?(model)
        metadata = [[0, 16 * BLOCK, :system]]
        metadata += model.hierarchies.flat_map(&:directories).map { [_1.extent * BLOCK, (_1.extent + (_1.bytes + BLOCK - 1) / BLOCK) * BLOCK, :directory] }
        metadata += model.descriptors.map { [_1.offset, _1.offset + BLOCK, :descriptor] }
        model.hierarchies.each do |hierarchy|
          descriptor = hierarchy.descriptor.fields
          metadata += [descriptor.path_l, descriptor.path_m, descriptor.path_l_copy, descriptor.path_m_copy].reject(&:zero?)
            .map { [_1 * BLOCK, (_1 + (descriptor.path_bytes_le + BLOCK - 1) / BLOCK) * BLOCK, :path] }
        end
        metadata += model.continuations.uniq { [_1.offset, _1.length, _1.previous.first] }
          .map { [_1.offset, _1.offset + _1.length, :continuation] }
        metadata += model.catalogs.map { |_, block, _raw| [block * BLOCK, (block + 1) * BLOCK, :catalog] }
        metadata = metadata.select { |start, stop, _| start < stop }.sort_by { |start, stop, _| [start, stop] }
        return false if metadata.each_cons(2).any? { |a, b| b.first < a[1] }

        data = data_records(model).map do |record|
          start = record.fields.extent_le * BLOCK
          stop = start + (record.fields.attribute_blocks + (record.fields.data_bytes_le + BLOCK - 1) / BLOCK) * BLOCK
          [start, stop]
        end
        limit = (model.volume&.fields&.space_le || 0) * BLOCK
        data += model.catalogs.flat_map do |_, _, raw|
          Catalog.decode(raw).entries.filter_map do |entry|
            start = entry.fields.image_block * BLOCK
            stop = [start + [1, (entry.fields.sector_count + 3) / 4].max * BLOCK, limit].min
            [start, stop] if start < stop
          end
        end
        cursor = 0
        data.sort_by(&:first).all? do |start, stop|
          cursor += 1 while cursor < metadata.size && metadata[cursor][1] <= start
          first = metadata[cursor]
          !first || first[0] >= stop || (first[2] == :catalog && first[0] == start && first[1] == stop)
        end
      end
      # Only the Rock Ridge view interprets child system-use bytes while
      # preparing a directory. Other views leave those bytes to verification;
      # the root handshake remains an open-time check in every view.
      def susp_record_scope_index(model)
        record_scope_index(model).transform_values do |scope|
          scope == "directories" && model.view != "rockridge" ? "verification" : scope
        end
      end
      def scopes_of(model, entries)
        records = susp_record_scope_index(model)
        scopes = model.records.flat_map do |record|
          record.entries.map { [_1.offset, records.fetch(record.offset)] }
        end.to_h
        entries.map { _1.scope == "open" ? "open" : scopes.fetch(_1.offset, "verification") }.uniq
      end
      def continuation?(area, model)
        return false unless area.length.positive? && area.within_le < BLOCK && area.block_le == area.block_be &&
          area.within_le == area.within_be && area.length == area.length_be

        prefix = [area.length, BLOCK - area.within_le].min
        area.offset + prefix <= [model.image_length, (model.volume&.fields&.space_le || 0) * BLOCK].min &&
          area.previous.none? { |start, bytes| area.offset < start + bytes && start < area.offset + prefix }
      end
      def susp_framed?(entry)
        entry.fields.entry_bytes >= 4 && entry.fields.entry_bytes == entry.raw.bytesize
      end
      def invalid_ce_scopes(model)
        malformed = model.susp_entries.select do |entry|
          entry.fields.signature == "CE" && (entry.fields.entry_bytes != 28 || entry.fields.entry_version != 1)
        end
        duplicated = model.susp_entries.select { _1.fields.signature == "CE" }.group_by { [_1.area, _1.scope] }
          .values.select { _1.size > 1 }.flatten
        scopes = continuation_scope_index(model)
        (scopes_of(model, malformed + duplicated) +
          model.continuations.reject { continuation?(_1, model) }.map { continuation_scope(scopes, _1) }).uniq
      end
      def continuation_profile_scopes(model)
        scopes = continuation_scope_index(model)
        model.continuations.select { continuation?(_1, model) }.reject do |area|
          area.length <= BLOCK && area.offset / BLOCK == (area.offset + area.length - 1) / BLOCK && area.previous.size <= 16
        end.map { continuation_scope(scopes, _1) }.uniq
      end
      def continuation_scope_index(model)
        records = susp_record_scope_index(model)
        model.records.reject { FsimgSpec.undefined?(_1.system_use) }.to_h do |record|
          [record.offset + record.raw.bytesize - record.system_use.bytesize, records.fetch(record.offset)]
        end
      end
      def continuation_scope(scopes, area)
        return "open" if area.scope == "open"

        scopes.fetch(area.previous.first.first, "verification")
      end
      def both32?(raw, offset) = raw.byteslice(offset, 4)&.unpack1("V") == raw.byteslice(offset + 4, 4)&.unpack1("N")
      def rr_version_scopes(model)
        extensions = model.susp_entries.select { _1.scope == "open" && _1.fields.signature == "ER" }
        root = extensions.any? { _1.fields.entry_version != 1 } ? ["open"] : []
        records = model.rr_entries.any? { KNOWN_RR.include?(_1.fields.signature) && _1.fields.entry_version != 1 } ? ["directories"] : []
        root + records
      end
      def rr_length?(entry)
        length = entry.raw.bytesize
        case entry.fields.signature
        in "PX" then [36, 44].include?(length)
        in "PN" then length == 20 && both32?(entry.raw, 4) && both32?(entry.raw, 12)
        in "CL" | "PL" then length == 12
        in "RE" then length == 4
        in "RR" then length == 5
        in "NM" | "SL" | "TF" then length >= 5
        else true
        end
      end
      def tf?(raw)
        return false if raw.bytesize < 5

        flags = raw.getbyte(4)
        width = flags & 0x80 == 0 ? 7 : 17
        count = (flags & 0x7f).digits(2).sum
        raw.bytesize == 5 + width * count && count.times.all? { width == 7 ? date7?(raw.byteslice(5 + _1 * width, width)) : date17?(raw.byteslice(5 + _1 * width, width)) }
      end
      def nm?(record)
        entries = record.entries.select { _1.fields.signature == "NM" }
        return true if entries.empty? || record.identifier == "\0".b || record.identifier == "\x01".b

        name = entries.map { _1.raw.byteslice(5, [_1.raw.bytesize - 5, 0].max) }.join.b
        Names.native?(name, view: "rockridge") &&
          entries.all? { _1.raw.bytesize >= 5 && _1.raw.getbyte(4) & 6 == 0 } &&
          entries.take(entries.size - 1).all? { _1.raw.getbyte(4) & 1 != 0 } && entries.last.raw.getbyte(4) & 1 == 0
      end
      def name_bytes(record) = record.entries.select { _1.fields.signature == "NM" }.sum { [_1.raw.bytesize - 5, 0].max }
      def link_records(model) = model.rr_records.reject { ["\0".b, "\x01".b].include?(_1.identifier) }
        .select { _1.entries.any? { |entry| entry.fields.signature == "SL" } }
      def rr_kind?(record)
        return true if FsimgSpec.undefined?(record.fields)

        kind = RockRidge.kind(record)
        signatures = record.entries.map { _1.fields.signature }
        px = record.entries.find { _1.fields.signature == "PX" && [36, 44].include?(_1.raw.bytesize) }
        device = px && [0o020000, 0o060000].include?(px.raw.byteslice(4, 4).unpack1("V") & 0o170000)
        return false if signatures.include?("PN") && !device

        directory = record.fields.flags & 2 != 0
        child = signatures.include?("CL")
        return false if px && !child && (kind == "directory") != directory

        if ["\0".b, "\x01".b].include?(record.identifier)
          return kind == "directory" && (signatures & %w[PN CL RE]).empty? &&
            (record.identifier == "\x01".b || !signatures.include?("PL"))
        end
        return false if signatures.include?("RE") && !directory
        return false if child && (kind != "directory" || signatures.include?("RE"))
        return false if kind == "directory" && !directory && !child
        return false if %w[file symlink fifo].include?(kind) && directory
        return false if kind == "symlink" && !signatures.include?("SL")
        return false if kind != "symlink" && signatures.include?("SL")

        true
      end
      def relocation_parents?(model)
        return true if FsimgSpec.undefined?(model.primary) || !model.primary.rockridge

        # PL belongs only to a directory opened through a CL placeholder.
        # Ordinary records and the root must open a parent dot without it.
        ordinary = model.rr_records.reject { ["\0".b, "\x01".b].include?(_1.identifier) }
          .select { !FsimgSpec.undefined?(_1.fields) && _1.fields.flags & 2 != 0 &&
            _1.entries.none? { |entry| %w[CL RE].include?(entry.fields.signature) } }
          .map { _1.fields.extent_le }
        root = model.primary.root.fields
        ordinary << root.extent_le unless FsimgSpec.undefined?(root)
        directories = model.primary.directories.to_h { [_1.extent, _1] }
        return false if ordinary.any? do |extent|
          directories[extent]&.records&.fetch(1, nil)&.entries&.any? { _1.fields.signature == "PL" }
        end

        model.primary.directories.all? do |parent|
          parent.records.drop(2).all? do |record|
            child = record.entries.find { _1.fields.signature == "CL" }
            next true unless child && child.raw.bytesize == 12

            target = model.primary.directories.find { _1.extent == child.raw.byteslice(4, 4).unpack1("V") }
            link = target&.records&.fetch(1, nil)&.entries&.find { _1.fields.signature == "PL" }
            target && link && link.raw.bytesize == 12 && link.raw.byteslice(4, 4).unpack1("V") == parent.extent
          end
        end
      end
      def relocation_targets?(model)
        return true if FsimgSpec.undefined?(model.primary) || !model.primary.rockridge

        targets = model.primary.records.flat_map(&:entries).select { _1.fields.signature == "CL" && _1.raw.bytesize == 12 }
          .map { _1.raw.byteslice(4, 4).unpack1("V") }
        relocated = model.primary.directories.select { _1.records.fetch(1, nil)&.entries&.any? { |entry| entry.fields.signature == "PL" } }.map(&:extent)
        targets.uniq.size == targets.size && targets.sort == relocated.sort
      end
      def boot_validation?(raw) = raw.bytesize == BLOCK && raw.getbyte(0) == 1 && raw.byteslice(2, 2) == "\0\0".b &&
        raw.byteslice(30, 2) == "\x55\xaa".b && raw.byteslice(0, 32).unpack("v16").sum % 65536 == 0
      def boot_entry?(raw, model)
        Catalog.decode(raw).entries.all? do |entry|
          fields = entry.fields
          media = entry.initial ? fields.media : fields.media & 0x1f
          [0, 0x88].include?(fields.indicator) && media.between?(0, 4) && fields.reserved.zero? &&
            fields.image_block > (model.descriptors.last&.offset || 0) / BLOCK
        end
      end
      def boot_profile?(model)
        model.catalogs.size <= 1 && model.catalogs.all? do |_, _, raw|
          facts = Catalog.decode(raw)
          platforms = facts.headers.map(&:platform)
          platforms << facts.validation.platform unless FsimgSpec.undefined?(facts.validation)
          !facts.overflow && platforms.all? { [0, 1, 2, 0xef].include?(_1) } &&
            facts.entries.none? { !_1.initial && _1.fields.media & 0x20 != 0 }
        end
      end
    end
  end
end
