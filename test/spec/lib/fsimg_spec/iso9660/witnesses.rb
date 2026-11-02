# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # Explicit mutations state the broken contract, not an expected result.
    # Rule classification derives the status and earliest required stage.
    module Witnesses
      Change = Data.define(:parameters, :mutations)
      Witness = Data.define(:id, :parameters, :mutations, :intended, :rules)
      Creation = Data.define(:id, :source, :intended, :rules)
      BLOCK = Builder::BLOCK
      RR = { rockridge: 1 }.freeze
      JOLIET = { joliet: 1 }.freeze
      SECTIONS = { sections: 1 }.freeze
      SUBDIRECTORY = { subdirectory: 1 }.freeze
      CONTINUATION = { continuation: 1 }.freeze
      RELOCATION = { relocation: 1 }.freeze
      BOOT = { boot: 1 }.freeze
      SYMLINK = { symlink: 1 }.freeze

      module_function

      def change(parameters: {}, mutations:) = Change.new(parameters: parameters.freeze, mutations: mutations.freeze)
      def u8(offset, value) = Mutation::SetInteger.new(op: "set_u8", offset:, value:)
      def u16(offset, value) = Mutation::SetInteger.new(op: "set_u16le", offset:, value:)
      def bytes(offset, value) = Mutation::ReplaceBytes.new(offset:, bytes: value)
      def both16(offset, value) = bytes(offset, Builder.both16(value))
      def both32(offset, value) = bytes(offset, Builder.both32(value))
      def pvd(offset) = 16 * BLOCK + offset
      def file(parameters: {}, index: 2)
        Builder::ROOT * BLOCK + Builder.root_records(Builder.options(parameters), joliet: false).take(index).sum(&:bytesize)
      end
      def system_use(parameters: {}) = file(parameters:) + 44
      def ce = system_use(parameters: CONTINUATION) + 48

      def changes = {
        "identity" => change(mutations: [u8(pvd(1), 0)]),
        "descriptor.identity" => change(mutations: [u8(17 * BLOCK + 1, 0)]),
        "descriptor.profile" => change(mutations: [u8(17 * BLOCK + 6, 3)]),
        "descriptor.limit" => change(mutations: descriptor_limit),
        "descriptor.terminator" => change(mutations: [u8(17 * BLOCK + 7, 1)]),
        "primary.present" => change(mutations: [u8(pvd(0), 3)]),
        "primary.duplicates" => change(mutations: duplicate_primary),
        "numeric.copies" => change(mutations: [bytes(pvd(84), [Builder::BLOCKS + 1].pack("N"))]),
        "geometry.profile" => change(mutations: [both16(pvd(128), 1024)]),
        "geometry.volume" => change(mutations: [both32(pvd(80), 0)]),
        "root.descriptor" => change(mutations: [u8(pvd(156), 36)]),
        "root.range" => change(mutations: [both32(pvd(158), Builder::BLOCKS)]),
        "descriptor.dates" => change(mutations: [bytes(pvd(813), "2024130203040500\0".b)]),
        "joliet.geometry" => change(parameters: JOLIET, mutations: [both32(17 * BLOCK + 80, Builder::BLOCKS - 1)]),
        "joliet.count" => change(parameters: JOLIET, mutations: duplicate_joliet),
        "directory.framing" => change(mutations: [u8(file, 35)]),
        "directory.copies" => change(mutations: [bytes(file + 14, [10].pack("N"))]),
        "directory.profile" => change(mutations: [u8(file + 26, 1)]),
        "directory.flags" => change(mutations: [u8(file + 25, 0x20)]),
        "directory.dates" => change(mutations: [u8(file + 19, 13)]),
        "directory.dots" => change(mutations: [u8(Builder::ROOT * BLOCK + 34 + 25, 0)]),
        "names.primary" => change(mutations: [u8(file + 33, 47)]),
        "names.joliet" => change(parameters: JOLIET, mutations: [u8(Builder::JOLIET_ROOT * BLOCK + 68 + 32, 19)]),
        "names.unique" => change(mutations: [bytes(file + primary_file.bytesize, primary_file)]),
        "sections.framing" => change(parameters: SECTIONS, mutations: [both32(file(parameters: SECTIONS) + 10, 3)]),
        "sections.ascending" => change(parameters: SECTIONS,
          mutations: [both32(file(parameters: SECTIONS, index: 3) + 2, Builder::DATA)]),
        "payload.bounds" => change(mutations: [Mutation::Truncate.new(length: Builder::DATA * BLOCK)]),
        "path.tables" => change(mutations: [bytes(Builder::PATH_M * BLOCK + 6, [2].pack("n"))]),
        "primary.order" => change(parameters: SUBDIRECTORY, mutations: swap_children),
        "ownership" => change(mutations: [both32(file + 2, Builder::ROOT)]),
        "susp.framing" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 2, 3)]),
        "susp.er" => change(parameters: RR, mutations: [u8(Builder::ROOT * BLOCK + 34 + 7 + 48 + 5, 255)]),
        "susp.ce" => change(parameters: CONTINUATION, mutations: [both32(ce + 20, 0)]),
        "susp.ce_profile" => change(parameters: CONTINUATION, mutations: [both32(ce + 20, BLOCK + 1)]),
        "susp.skip" => change(parameters: RR, mutations: [u8(Builder::ROOT * BLOCK + 34 + 6, 1)]),
        "rr.version" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 3, 2)]),
        "rr.lengths" => change(parameters: RR, mutations: [bytes(system_use(parameters: RR) + 36, "RR".b)]),
        "rr.px" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 2, 35)]),
        "rr.tf" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 36 + 4, 0)]),
        "rr.singletons" => change(parameters: RR, mutations: duplicate_px),
        "rr.names" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 48 + 5, 47)]),
        "rr.name_profile" => change(parameters: CONTINUATION, mutations: long_name),
        "rr.pointers" => change(parameters: RELOCATION, mutations: [bytes(file(parameters: RELOCATION, index: 4) + 100, [Builder::RELOCATED + 1].pack("N"))]),
        "rr.kind" => change(parameters: RR, mutations: [both32(system_use(parameters: RR) + 4, 0o40755)]),
        "rr.kind_profile" => change(parameters: RR, mutations: [both32(system_use(parameters: RR) + 4, 0o140644)]),
        "rr.sl.framing" => change(parameters: SYMLINK, mutations: [u8(link_sl + 6, 255)]),
        "rr.sl.components" => change(parameters: SYMLINK, mutations: [u8(link_sl + 5, 6)]),
        "rr.sl.profile" => change(parameters: SYMLINK, mutations: refused_link),
        "extract.safe_kinds" => change(parameters: SYMLINK, mutations: []),
        "extract.omitted_names" => change(parameters: RR, mutations: [u8(system_use(parameters: RR) + 48 + 5, 255)]),
        "rr.unsupported" => change(parameters: RR, mutations: [bytes(system_use(parameters: RR) + 48, "ZF".b)]),
        "relocation.parents" => change(parameters: RELOCATION,
          mutations: [both32(Builder::RELOCATED * BLOCK + 82 + 34 + 48 + 4, Builder::SUB)]),
        "relocation.targets" => change(parameters: RELOCATION, mutations: orphan_relocation),
        "boot.validation" => change(parameters: BOOT, mutations: [u8(Builder::CATALOG * BLOCK + 28, Builder.boot_catalog.getbyte(28) ^ 1)]),
        "boot.profile" => change(parameters: BOOT, mutations: [bytes(Builder::CATALOG * BLOCK, validation(platform: 0xff))]),
        "boot.entry" => change(parameters: BOOT, mutations: [u8(Builder::CATALOG * BLOCK + 33, 5)]),
        "boot.sections" => change(parameters: {boot_sections: 1}, mutations: [u16(Builder::CATALOG * BLOCK + 66, 0)])
      }.freeze

      def for_rule(id)
        key = id.delete_prefix("iso9660.")
        selected = changes.fetch(key)
        positive = key == "extract.safe_kinds" ? {} : selected.parameters
        [Witness.new(id: "#{id}.valid", parameters: positive, mutations: [], intended: nil, rules: [id]),
         Witness.new(id: "#{id}.invalid", parameters: selected.parameters, mutations: selected.mutations, intended: id, rules: [id])]
      end

      # Valid cases with the same fixture profile share one full-stage run;
      # their coverage associations still name every rule they witness.
      def deterministic(registry)
        witnesses = registry.rules.reject(&:input?).flat_map { _1.domain.call }
        valid, invalid = witnesses.partition { _1.intended.nil? }
        combined = valid.group_by(&:parameters).sort_by { |parameters, _| parameters.to_a }.each_with_index.map do |(parameters, group), index|
          Witness.new(id: "iso9660.profile.#{index}", parameters:, mutations: [], intended: nil,
                      rules: group.flat_map(&:rules).uniq.freeze)
        end
        boundaries = [{symlink: 1, target_length: 1023}, {symlink: 1, target_length: 1024},
                      {symlink: 1, target_shape: 1}, {symlink: 1, target_shape: 2}, {fifo: 1}]
        combined + boundaries.each_with_index.map do |parameters, index|
          Witness.new(id: "iso9660.link_boundary.#{index}", parameters:, mutations: [], intended: nil,
            rules: %w[iso9660.rr.sl.framing iso9660.rr.sl.components iso9660.rr.sl.profile])
        end + invalid + scope_regressions
      end

      # These paired facts distinguish framing scope from RR semantics,
      # and declared directory bytes from its allocated block padding.
      def scope_regressions
        sub = {rockridge: 1, subdirectory: 1}
        sections = {rockridge: 1, sections: 1}
        count = Builder.root_records(Builder.options({}), joliet: false).sum(&:bytesize)
        partial = [both32(pvd(166), count), both32(Builder::ROOT * BLOCK + 10, count),
                   both32(Builder::ROOT * BLOCK + 34 + 10, count)]
        controls = %w[ST PD].map do |signature|
          control = Builder.entry(signature, "\0".b * 4).dup
          control.setbyte(3, 2)
          raw = Builder.record(identifier: "FILE.TXT;1", extent: Builder::DATA, bytes: 9, flags: 0,
            system_use: Builder.rr(directory: false) + control + Builder.nm("FILE.TXT"))
          Witness.new(id: "iso9660.susp.control.#{signature.downcase}", parameters: RR,
            mutations: [bytes(file(parameters: RR), raw)], intended: nil, rules: ["iso9660.susp.framing"])
        end
        controls + [
          Witness.new(id: "iso9660.rr.kind.directory_device", parameters: sub,
            mutations: [both32(file(parameters: sub, index: 3) + 40, 0o020644)],
            intended: "iso9660.rr.kind", rules: ["iso9660.rr.kind"]),
          Witness.new(id: "iso9660.relocation.parents.ordinary_pl", parameters: sub,
            mutations: [bytes(Builder::SUB * BLOCK, ordinary_parent_link)],
            intended: "iso9660.relocation.parents", rules: ["iso9660.relocation.parents"]),
          Witness.new(id: "iso9660.directory.copies.later_section", parameters: sections,
            mutations: [bytes(file(parameters: sections, index: 3) + 14, [10].pack("N"))],
            intended: "iso9660.directory.copies", rules: ["iso9660.directory.copies"]),
          Witness.new(id: "iso9660.directory.partial.valid", parameters: {}, mutations: partial,
            intended: nil, rules: ["iso9660.directory.framing"]),
          Witness.new(id: "iso9660.directory.partial.invalid", parameters: {},
            mutations: partial + [u8(Builder::ROOT * BLOCK + count, 1)],
            intended: "iso9660.directory.framing", rules: ["iso9660.directory.framing"])
        ]
      end

      def ordinary_parent_link
        own = Builder.record(identifier: "\0", extent: Builder::SUB, bytes: BLOCK, flags: 2,
          system_use: Builder.rr(directory: true, links: 2))
        parent = Builder.record(identifier: "\x01", extent: Builder::ROOT, bytes: BLOCK, flags: 2,
          system_use: Builder.rr(directory: true, links: 3) + Builder.pointer("PL", Builder::ROOT))
        leaf = Builder.record(identifier: "LEAF.BIN;1", extent: Builder::LEAF, bytes: 7, flags: 0,
          system_use: Builder.rr(directory: false, name: "LEAF.BIN"))
        (own + parent + leaf).ljust(BLOCK, "\0")
      end

      def primary_file = Builder.root_records(Builder.options({}), joliet: false).fetch(2)
      def refused_link
        metadata = Builder.px(kind: "symlink") + Builder.tf + Builder.nm("LINK") + Builder.entry("SL", "\0\x10\0".b)
        raw = Builder.record(identifier: "LINK.;1", extent: 0, bytes: 0, flags: 0, system_use: metadata)
        original = Builder.root_records(Builder.options(SYMLINK), joliet: false).last
        [bytes(file(parameters: SYMLINK, index: 3), raw.ljust(original.bytesize, "\0"))]
      end
      def long_name
        stream = Array.new(5) do |index|
          Builder.entry("NM", [index < 4 ? 1 : 0].pack("C") + "n".b * 220)
        end.join.b
        [bytes(Builder::CONTINUATION * BLOCK, stream), both32(ce + 20, stream.bytesize)]
      end
      def link_sl = file(parameters: SYMLINK, index: 3) + 40 + 57
      def descriptor_limit
        primary = Builder.descriptor(Builder.options({}), joliet: false)
        [bytes(16 * BLOCK, primary * 65)]
      end
      def duplicate_primary
        primary = Builder.descriptor(Builder.options({}), joliet: false)
        changed = primary.dup
        changed[40, 32] = "OTHER".b.ljust(32)
        [bytes(17 * BLOCK, changed), bytes(18 * BLOCK, "\xffCD001\x01".b.ljust(BLOCK, "\0"))]
      end
      def duplicate_joliet
        supplementary = Builder.descriptor(Builder.options(JOLIET), joliet: true)
        [bytes(18 * BLOCK, supplementary), bytes(19 * BLOCK, "\xffCD001\x01".b.ljust(BLOCK, "\0"))]
      end
      def swap_children
        children = Builder.root_records(Builder.options(SUBDIRECTORY), joliet: false).drop(2)
        [bytes(file(parameters: SUBDIRECTORY), children.reverse.join.b)]
      end
      def duplicate_px
        raw = Builder.record(identifier: "FILE.TXT;1", extent: Builder::DATA, bytes: 9, flags: 0,
          system_use: Builder.rr(directory: false, name: "FILE.TXT") + Builder.px(kind: "file"))
        [bytes(file(parameters: RR), raw)]
      end
      def orphan_relocation
        offset = file(parameters: RELOCATION, index: 4)
        [bytes(offset + 40, Builder.both32(0o100644)), bytes(offset + 36 + 48 + 8, "PD".b)]
      end
      def validation(platform:)
        raw = Builder.boot_catalog.byteslice(0, 32).dup
        raw.setbyte(1, platform)
        raw[28, 2] = "\0\0".b
        raw[28, 2] = [(-raw.unpack("v16").sum) & 0xffff].pack("v")
        raw
      end

      def creation_for_rule(id)
        standard = [TreeEntry.file("FILE.TXT", size: 9, content: "zero")]
        invalid_tree, settings = case id.delete_prefix("iso9660.create.")
                                in "geometry" then [standard, {sector: 1024}]
                                in "name_bytes" then [[TreeEntry.file("n" * 256, size: 1, content: "zero")], {}]
                                in "target_bytes" then [[TreeEntry.symlink("LINK", target: "t" * 256)], {}]
                                in "boot_paths" then [standard, {boot: "/missing"}]
                                in "bootload" then [standard, {bootload: 4}]
                                in "file_sections" then [[TreeEntry.file("LARGE", size: 0x1_0000_0000, content: "zero")], {level: 1}]
                                in "volume_lower_bound" then [standard, {size: 2048}]
                                in "volume_ceiling" then [standard, {size: 0x1_0000_0000 * BLOCK}]
                                end
        [Creation.new(id: "#{id}.valid", source: Source.new(tree: standard, settings: {}), intended: nil, rules: [id]),
         Creation.new(id: "#{id}.invalid", source: Source.new(tree: invalid_tree, settings:), intended: id, rules: [id])]
      end

      def creations(registry, ceilings:)
        declared = registry.rules.select(&:input?).flat_map { _1.domain.call }
        standard = [TreeEntry.file("FILE.TXT", size: 9, content: "zero")]
        settings = [{level: 1}, {level: 2}, {level: 3}, {boot: "/FILE.TXT"}, {efiboot: "/FILE.TXT"},
                    {boot: "/FILE.TXT", efiboot: "/FILE.TXT", bootload: 4}]
        settings.each_with_index do |values, index|
          declared << Creation.new(id: "iso9660.create.profile.#{index}", source: Source.new(tree: standard, settings: values),
                                   intended: nil, rules: registry.rules.select(&:input?).map(&:id))
        end
        declared << Creation.new(id: "iso9660.create.special", source: Source.new(tree: standard +
          [TreeEntry.symlink("LINK", target: ".././FILE.TXT"), TreeEntry.fifo("PIPE")], settings: {}),
          intended: nil, rules: registry.rules.select(&:input?).map(&:id))
        declared << Creation.new(id: "iso9660.create.boot_punctuation", source: Source.new(
          tree: [TreeEntry.file("a,b=c", size: 2048, content: "zero")], settings: {boot: "/a,b=c"}),
          intended: nil, rules: %w[iso9660.create.boot_paths iso9660.create.bootload])
        declared.select { _1.source.tree.sum(&:logical_size) <= ceilings.output &&
          (!_1.source.settings.key?(:size) || _1.source.settings[:size] <= ceilings.image) }
      end
    end
  end
end
