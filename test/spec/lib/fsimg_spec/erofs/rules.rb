# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Erofs
    # Logical requirements over independently decoded records, each citing
    # the section of specs/design-erofs.md that states it and the scope at
    # which the reader checks it. Open checks the superblock and the root
    # inode; listing a directory checks its entries and their inodes.
    module Rules
      IDENTITY = ->(model, _) { identity?(model) }
      GEOMETRY = ->(model, _) { identity?(model) && !FsimgSpec.undefined?(model.block) }
      CHECKSUM = ->(model, _) { GEOMETRY.call(model, nil) && !FsimgSpec.undefined?(model.checksum) }
      INODES = ->(model, _) { GEOMETRY.call(model, nil) && model.inodes.values.any? }
      DIRECTORIES = ->(model, _) { GEOMETRY.call(model, nil) && model.directories.any? }
      NSEC = 1_000_000_000
      # The incompatible feature bits this reader implements (design:
      # Features): only the long-prefix table. Every other bit, known or
      # not, is refused at open. A rule moves to accept as a stage
      # implements its feature.
      READABLE_INCOMPAT = 0x40
      PREFIXES_FEATURE = 0x40
      BUILTIN_PREFIXES = [1, 2, 3, 4, 6].freeze
      LONG_PREFIX = 0x80
      ACL_INDEXES = [2, 3].freeze
      # The compressed layouts (1 and 3) and the chunk-based layout (4) are
      # outside the profile. A chunk-based inode on a volume without
      # CHUNKED_FILE is corrupt, not unsupported (design: Features).
      UNREAD_LAYOUTS = [1, 3, 4].freeze
      CHUNKED_FILE = 0x04
      # A directory's data is read when it is listed, a file's when extracted.
      DATA_SCOPE = ->(inode) { inode.directory? ? "directories" : "files" }

      module_function

      def registry = Registry.new(all)

      def rule(id:, scope:, anchor:, predicate:, status: "FSIMG_ECORRUPT", category: "normative", applies_to: GEOMETRY)
        Rule.new(id: "erofs.#{id}", category:, policy: "must_reject", status:,
                 scopes: Array(scope), applies_to:, predicate:,
                 domain: -> { Witnesses.for_rule("erofs.#{id}") },
                 source: "specs/design-erofs.md##{anchor}")
      end

      # A rule over every decoded inode that applies_to selects. A
      # violation has the scope of the check that reached the inode: open
      # for the root, listing for the rest, unless scope_of names where the
      # design checks the rule instead, or the rule has one scope.
      def inode_rule(id:, anchor:, scope: %w[open directories], scope_of: :scope.to_proc, status: "FSIMG_ECORRUPT",
                     category: "normative", applies_to: nil, &holds)
        selected = applies_to || ->(_inode, _model) { true }
        rule(id:, scope:, anchor:, status:, category:, applies_to: INODES,
             predicate: lambda { |model, _|
               failing = model.inodes.values.compact.select { selected.call(_1, model) && !holds.call(_1, model) }
               Array(scope).one? ? failing.empty? : failing.map(&scope_of).uniq
             })
      end

      def attribute_rule(id:, anchor:, status: "FSIMG_ECORRUPT", category: "normative", applies_to: nil, &holds)
        has_region = ->(inode, model) { inode.region_bytes.positive? && (applies_to.nil? || applies_to.call(inode, model)) }
        inode_rule(id:, anchor:, scope: "directories", status:, category:, applies_to: has_region, &holds)
      end

      def all = [
        rule(id: "identity", scope: "open", anchor: "volume-identity-and-geometry", status: "FSIMG_EFORMAT",
             applies_to: ->(_, _) { true }, predicate: ->(m, _) { identity?(m) }),
        rule(id: "sb.blkszbits", scope: "open", anchor: "volume-identity-and-geometry", applies_to: IDENTITY,
             predicate: ->(m, _) { m.sb.blkszbits >= 9 }),
        rule(id: "sb.block_size", scope: "open", anchor: "volume-identity-and-geometry", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: ->(m, _) { identity?(m) && m.sb.blkszbits >= 9 },
             predicate: ->(m, _) { m.sb.blkszbits <= 12 }),
        rule(id: "sb.dirblkbits", scope: "open", anchor: "volume-identity-and-geometry", status: "FSIMG_EUNSUPPORTED",
             category: "profile", applies_to: IDENTITY, predicate: ->(m, _) { m.sb.dirblkbits.zero? }),
        rule(id: "sb.incompat", scope: "open", anchor: "features", status: "FSIMG_EUNSUPPORTED", category: "profile",
             applies_to: IDENTITY, predicate: ->(m, _) { (m.sb.incompat & ~READABLE_INCOMPAT).zero? }),
        rule(id: "sb.devices", scope: "open", anchor: "external-data-devices", status: "FSIMG_EUNSUPPORTED", category: "profile",
             applies_to: IDENTITY, predicate: ->(m, _) { m.sb.extra_devices.zero? }),
        rule(id: "sb.checksum", scope: "open", anchor: "superblock-checksum", applies_to: CHECKSUM,
             predicate: ->(m, _) { m.checksum.first == m.checksum.last }),
        rule(id: "sb.nanoseconds", scope: "open", anchor: "the-superblock", applies_to: IDENTITY,
             predicate: ->(m, _) { m.sb.fixed_nsec < NSEC }),
        inode_rule(id: "inode.format", anchor: "format-word") { |inode, _| inode.format >> 4 == 0 },
        inode_rule(id: "inode.layout", anchor: "format-word") { |inode, _| inode.layout <= 4 },
        inode_rule(id: "inode.chunk_feature", anchor: "features",
                   applies_to: ->(inode, model) { inode.layout == 4 && !model.sb.incompat.anybits?(CHUNKED_FILE) }) { false },
        inode_rule(id: "inode.layout_profile", anchor: "data-layouts", status: "FSIMG_EUNSUPPORTED", category: "profile",
                   applies_to: ->(inode, model) { unread_layout?(inode, model) }) { false },
        inode_rule(id: "inode.reserved", anchor: "compact-inode") do |inode, _|
          inode.extended ? inode.fields.reserved_a.zero? && inode.fields.reserved_b.bytes.all?(&:zero?) : inode.fields.reserved.zero?
        end,
        inode_rule(id: "inode.nanoseconds", anchor: "extended-inode", applies_to: ->(inode, _) { inode.extended }) { |inode, _| inode.fields.nsec < NSEC },
        inode_rule(id: "inode.mode", anchor: "kinds-and-fields") { |inode, _| Model::TYPES.key?(inode.type_bits) },
        inode_rule(id: "inode.body", anchor: "body") do |inode, model|
          FsimgSpec.derive(model.domain) { inode.offset + inode.body_bytes(model.block) <= _1 }
        end,
        inode_rule(id: "inode.tail", anchor: "flat-and-flat-inline",
                   applies_to: ->(inode, model) { inode.inline? && inode.tail_bytes(model.block).positive? }) do |inode, model|
          inode.body_offset / model.block == (inode.body_offset + inode.tail_bytes(model.block) - 1) / model.block
        end,
        inode_rule(id: "inode.inline_size", anchor: "flat-and-flat-inline",
                   applies_to: ->(inode, _) { inode.inline? && inode.size.positive? }) { |inode, model| (inode.size % model.block).positive? },
        inode_rule(id: "inode.data_domain", anchor: "flat-and-flat-inline", scope: %w[directories files], scope_of: DATA_SCOPE,
                   applies_to: ->(inode, model) { (inode.layout.zero? || inode.inline?) && stored_bytes(inode, model).positive? }) do |inode, model|
          FsimgSpec.derive(model.domain) { inode.fields.layout_word * model.block + stored_bytes(inode, model) <= _1 }
        end,
        inode_rule(id: "inode.directory_size", anchor: "directories",
                   applies_to: ->(inode, _) { inode.directory? }) { |inode, _| inode.size.positive? },
        attribute_rule(id: "xattr.framing", anchor: "inline-region") { |inode, _| attributes(inode).framed },
        attribute_rule(id: "xattr.index", anchor: "names-1", status: "FSIMG_EUNSUPPORTED", category: "profile",
                       applies_to: ->(inode, _) { attributes(inode).framed }) do |inode, _|
          attributes(inode).entries.all? { _1.name_index >= LONG_PREFIX || BUILTIN_PREFIXES.include?(_1.name_index) }
        end,
        attribute_rule(id: "xattr.acl_suffix", anchor: "names-1", applies_to: ->(inode, _) { attributes(inode).framed }) do |inode, _|
          attributes(inode).entries.none? { ACL_INDEXES.include?(_1.name_index) && _1.suffix_length.positive? }
        end,
        attribute_rule(id: "xattr.prefix", anchor: "names-1", applies_to: ->(inode, _) { attributes(inode).framed }) do |inode, model|
          attributes(inode).entries.select { _1.name_index >= LONG_PREFIX }.all? do
            model.sb.incompat.anybits?(PREFIXES_FEATURE) && (_1.name_index & 0x7F) < model.sb.prefix_count
          end
        end,
        rule(id: "dir.framing", scope: "directories", anchor: "blocks", applies_to: DIRECTORIES,
             predicate: ->(m, _) { m.directories.all? { |d| d.blocks.all? { !_1.entries.nil? } } }),
        rule(id: "dir.offsets", scope: "directories", anchor: "blocks", applies_to: DIRECTORIES,
             predicate: ->(m, _) { m.directories.all? { |d| d.blocks.all? { _1.entries.nil? || _1.delimited } } }),
        rule(id: "dir.names", scope: "directories", anchor: "blocks", applies_to: DIRECTORIES,
             predicate: ->(m, _) { named(m).all? { |d| d.entries.all? { name?(_1.name) } } }),
        rule(id: "dir.order", scope: "directories", anchor: "order-and-dot-entries", applies_to: DIRECTORIES,
             predicate: ->(m, _) { named(m).all? { |d| d.entries.map(&:name).each_cons(2).all? { _1 < _2 } } }),
        rule(id: "dir.type", scope: "directories", anchor: "blocks", applies_to: DIRECTORIES,
             predicate: ->(m, _) { m.directories.all? { |d| d.entries.all? { _1.type <= 7 } } }),
        rule(id: "dir.type_mismatch", scope: "directories", anchor: "blocks", applies_to: DIRECTORIES,
             predicate: ->(m, _) { m.directories.all? { |d| d.entries.all? { type_agrees?(_1, m) } } }),
        rule(id: "dir.parent", scope: "directories", anchor: "the-parent-rule", applies_to: DIRECTORIES,
             predicate: ->(m, _) { named(m).all? { parent_rule?(_1) } })
      ]

      def identity?(model) = model.sb_defined? && model.magic == [Layout::MAGIC].pack("V") && model.image_length >= 1028

      def unread_layout?(inode, model) =
        UNREAD_LAYOUTS.include?(inode.layout) && (inode.layout != 4 || model.sb.incompat.anybits?(CHUNKED_FILE))

      # The bytes of an inode's content held in whole blocks.
      def stored_bytes(inode, model) = inode.inline? ? inode.size / model.block * model.block : inode.size

      Attributes = Data.define(:framed, :entries)

      # The inline attribute region (design: Inline region). It is framed
      # when its shared indexes and every entry with its padding end inside
      # the region, and no bytes remain.
      def attributes(inode)
        region = inode.region
        return UNFRAMED if region.bytesize < 12

        indexes = 12 + 4 * Layout.decode(Layout::AttributeHeader, region.byteslice(0, 12)).shared_count
        return UNFRAMED if indexes > region.bytesize

        starts = Enumerator.produce(indexes) { _1 + entry_span(region, _1) }.take_while { _1 + 4 <= region.bytesize }
        whole = starts.map { [Layout.decode(Layout::AttributeEntry, region.byteslice(_1, 4)), _1 + entry_span(region, _1)] }
                      .take_while { |_, last| last <= region.bytesize }
        ends = whole.empty? ? indexes : whole.last.last
        Attributes.new(framed: whole.size == starts.size && ends == region.bytesize, entries: whole.map(&:first))
      end

      UNFRAMED = Attributes.new(framed: false, entries: []).freeze

      # An entry's head, name, value and padding to a multiple of 4.
      def entry_span(region, at)
        entry = Layout.decode(Layout::AttributeEntry, region.byteslice(at, 4))
        (4 + entry.suffix_length + entry.value_length + 3) & ~3
      end

      # The directories whose every block has delimited names.
      def named(model) = model.directories.select { |d| d.blocks.all? { !_1.entries.nil? && _1.delimited } }

      def name?(name) = (1..255).cover?(name.bytesize) && !name.match?(%r{[/\0]}n)

      def type_agrees?(entry, model)
        child = model.inodes[entry.nid]
        entry.type.zero? || entry.type > 7 || child.nil? || Model::TYPES[child.type_bits] == entry.type
      end

      # "." names the directory, ".." its parent, and no other entry names
      # either of them.
      def parent_rule?(directory)
        entries = directory.entries
        dot = entries.find { _1.name == "." }
        dotdot = entries.find { _1.name == ".." }
        others = entries.reject { %w[. ..].include?(_1.name) }
        dot&.nid == directory.inode.nid && dotdot&.nid == directory.parent &&
          others.none? { [directory.inode.nid, directory.parent].include?(_1.nid) }
      end
    end
  end
end
