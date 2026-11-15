# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # Requirements of the record level over each volume's decoded records,
    # citing the design's Filesystem records sections and its Verification
    # section. A check the reader makes on a record it reads in directory
    # traversal has scope directories, one on file extents scope files, and
    # a reconciliation of records with each other scope verification.
    module RecordRules
      # The kinds a filesystem tree holds (design: Key families); a crypto
      # state is unsupported in an unencrypted volume, the only kind this
      # model reads.
      CRYPTO_STATE = 7
      TREE_KINDS = [Records::INODE, Records::ATTRIBUTE, Records::SIBLING_LINK, Records::STREAM_ID, CRYPTO_STATE, Records::EXTENT,
                    Records::DIRECTORY_RECORD, Records::DIRECTORY_STATS, Records::SIBLING_MAP, Records::FILE_INFO].freeze
      ROOT = 2
      PRIVATE = 3
      ROOT_PARENT = 1
      FIRST_ORDINARY = 16
      KNOWN_INTERNAL_FLAGS = 0x7F_FFFF & ~0x20
      DATALESS = 0x4000_0000
      TYPE_SHIFT = 12
      DIRECTORY_TYPE = 4
      # Mode type => directory-record type (design: Inodes).
      MODE_TYPES = { 0o10 => 8, 0o04 => 4, 0o12 => 10, 0o01 => 1, 0o02 => 2, 0o06 => 6, 0o14 => 12 }.freeze
      SYMLINK_TYPE = 10
      FILE_TYPE = 8
      # Inode extended-field sizes (design: Extended fields); nil is any
      # size, and the name, type 4, has a rule of its own.
      INODE_FIELD_SIZES = { 1 => 8, 2 => 8, 3 => 4, 4 => nil, 5 => 8, 6 => nil, 7 => 32, 8 => 40, 9 => nil, 10 => 32, 11 => 16,
                            12 => nil, 13 => 8, 14 => 4, 15 => nil, 16 => 8 }.freeze
      ENTRY_FIELD_SIZES = { 1 => 8 }.freeze
      DATA_DEPENDENT = 0x1
      # The fixed part each value kind is checked against before a field is
      # read; nil judges the kind's value elsewhere or not at all.
      VALUE_MINIMUM = { Records::INODE => 92, Records::DIRECTORY_RECORD => 18, Records::EXTENT => 24, Records::STREAM_ID => 4,
                        Records::ATTRIBUTE => 4, Records::SIBLING_LINK => 10, Records::SIBLING_MAP => 8,
                        Records::DIRECTORY_STATS => 32 }.freeze
      VALUE_EXACT = { Records::EXTENT => 24, Records::STREAM_ID => 4, Records::SIBLING_MAP => 8 }.freeze
      EXTENT_FLAGS_SHIFT = 56

      DECODED = ->(m, _) { Rules::CHECKPOINT.call(m, nil) && m.filesystems.any?(&:tree_decoded?) }
      HASHES = ->(m, _) { DECODED.call(m, nil) && Names::TABLES_MATCH }

      module_function

      def all = [*key_rules, *inode_rules, *field_rules, *entry_rules, *extent_rules, *reconciliation_rules]

      def volumes(model) = model.filesystems.select(&:tree_decoded?)

      # A rule over each decoded volume's records with a fixed scope.
      def each_set(id, scope, anchor, check, policy: :rule, applies_to: DECODED)
        Rules.method(policy).call(id:, scope:, anchor:, applies_to:,
                                  predicate: ->(m, _) { volumes(m).all? { check.call(_1.records, _1, m) } })
      end

      def key_rules = [
        each_set("fs.kind", "directories", "key-families", ->(r, _, _) { r.pairs.all? { TREE_KINDS.include?(_1.first.kind) } }),
        each_set("fs.lengths", "directories", "key-families", ->(r, _, _) { r.pairs.all? { |key, value| lengths_fit?(key, value) } }),
        each_set("fs.crypto_state", "directories", "key-families", ->(r, _, _) { r.pairs.none? { _1.first.kind == CRYPTO_STATE } },
                 policy: :unsupported)
      ]

      def inode_rules = [
        each_set("inode.reserved", "directories", "inodes", ->(r, _, _) { reserved_unnamed?(r) }),
        each_set("inode.root", "directories", "inodes", lambda do |r, _, _|
          [ROOT, PRIVATE].all? { (inode = r.inode(_1)&.fields) && inode.parent_id == ROOT_PARENT && type_of(inode) == DIRECTORY_TYPE }
        end),
        each_set("inode.mode", "directories", "inodes", ->(r, _, _) { inode_fields(r).all? { type_of(_1) } }),
        each_set("inode.flags", "directories", "inodes", ->(r, _, _) { inode_fields(r).all? { (_1.internal_flags & ~KNOWN_INTERNAL_FLAGS).zero? } },
                 policy: :unsupported),
        each_set("inode.dataless", "directories", "inodes", ->(r, _, _) { inode_fields(r).none? { _1.bsd_flags.anybits?(DATALESS) } },
                 policy: :unsupported)
      ]

      def field_rules = [
        each_set("xfield.blob", "directories", "extended-fields", lambda do |r, _, _|
          r.inodes.each_value.all? { fields_valid?(_1.extended, INODE_FIELD_SIZES, named: true) } &&
            r.entries.all? { fields_valid?(_1.extended, ENTRY_FIELD_SIZES, named: false) }
        end),
        each_set("xfield.unknown", "directories", "extended-fields", lambda do |r, _, _|
          [[r.inodes.values, INODE_FIELD_SIZES], [r.entries, ENTRY_FIELD_SIZES]].all? do |records, sizes|
            records.filter_map(&:extended).flat_map(&:entries).none? { |type, flags, _| unknown_dependent?(type, flags, sizes) }
          end
        end, policy: :unsupported)
      ]

      def entry_rules = [
        each_set("drec.variant", "directories", "directory-records-and-names", ->(r, v, _) { r.pairs.none? { other_variant?(_1.first, v) } }),
        each_set("drec.name", "directories", "directory-records-and-names",
                 ->(r, _, _) { r.entries.all? { (name = _1.key.name) && Names.valid?(name) } }),
        each_set("drec.hash", "directories", "directory-records-and-names",
                 ->(r, v, _) { judged_names(r, v, assigned: true).all? { hash_matches?(_1, v) } }, applies_to: HASHES),
        each_set("drec.hash_unassigned", "directories", "directory-records-and-names",
                 ->(r, v, _) { judged_names(r, v, assigned: false).all? { hash_matches?(_1, v) } }, policy: :unsupported, applies_to: HASHES),
        each_set("drec.type", "directories", "inodes", lambda do |r, _, _|
          r.entries.all? { (inode = r.inode(_1.child)&.fields).nil? || _1.type == type_of(inode) }
        end)
      ]

      def extent_rules = [
        each_set("extent.tiling", "files", "data-streams-and-file-extents", ->(r, _, m) { streams_tiled?(r, m.checkpoint.block_size) }),
        each_set("extent.range", "files", "data-streams-and-file-extents", lambda do |r, _, m|
          r.extents.reject(&:hole?).all? { |e| e.fields.physical_block + (e.length / m.checkpoint.block_size) <= m.checkpoint.block_count }
        end),
        each_set("extent.flags", "files", "data-streams-and-file-extents", ->(r, _, _) { r.extents.all? { _1.flags.zero? } },
                 policy: :unsupported),
        each_set("extent.crypto", "files", "data-streams-and-file-extents", ->(r, _, _) { r.extents.all? { _1.fields.crypto_id.zero? } },
                 policy: :unsupported)
      ]

      def reconciliation_rules = [
        # Traversal reads each record's inode, as drec.type does.
        each_set("drec.target", "directories", "inodes", ->(r, _, _) { r.entries.all? { r.inode(_1.child)&.fields } }),
        each_set("inode.children", "verification", "verification", lambda do |r, _, _|
          directories(r).all? { _1.fields.child_or_link_count == r.children_of(_1.number).size }
        end),
        each_set("inode.links", "verification", "verification", lambda do |r, _, _|
          named_files(r).all? { _1.fields.child_or_link_count == r.names_of(_1.number).size }
        end),
        each_set("inode.parent", "verification", "verification", ->(r, _, _) { r.inodes.each_value.all? { parent_agrees?(_1, r) } }),
        each_set("dstream.references", "verification", "verification", ->(r, _, _) { stream_counts_agree?(r) }),
        each_set("extent.owner", "verification", "verification", lambda do |r, _, _|
          live = streams(r).keys | r.attributes.filter_map(&:stream_id)
          r.extents.all? { live.include?(_1.owner) }
        end),
        each_set("apsb.counts", "verification", "verification", ->(r, v, _) { object_counts(r) == volume_counts(v) })
      ]

      # The inode's directory-record type from its mode, nil for a mode
      # outside the table.
      def type_of(inode) = MODE_TYPES[inode.mode >> TYPE_SHIFT]

      def inode_fields(records) = records.inodes.each_value.filter_map(&:fields)

      def directories(records) = records.inodes.each_value.select { _1.fields && type_of(_1.fields) == DIRECTORY_TYPE }

      def named_files(records) =
        records.inodes.each_value.select { _1.fields && type_of(_1.fields) != DIRECTORY_TYPE && !records.names_of(_1.number).empty? }

      # Every key and value length fits its record's fixed part and every
      # variable part the record's end (design: Key families).
      def lengths_fit?(key, value)
        minimum = VALUE_MINIMUM[key.kind]
        exact = VALUE_EXACT[key.kind]
        !key.suffix.nil? && (minimum.nil? || value.bytesize >= minimum) && (exact.nil? || value.bytesize == exact)
      end

      # No inode is 0 or 1, and no directory record names an inode below
      # 16 except the root and private-directory records under parent 1.
      def reserved_unnamed?(records)
        records.inodes.keys.none? { _1 <= ROOT_PARENT } &&
          records.entries.all? { _1.child.nil? || _1.child >= FIRST_ORDINARY || (_1.parent == ROOT_PARENT && [ROOT, PRIVATE].include?(_1.child)) }
      end

      # A blob that fits exactly and whose known fields have their sizes;
      # in an inode (named) the name field is NUL-terminated and at least
      # 2 bytes. A value too short for its fixed part has no blob to judge.
      def fields_valid?(fields, sizes, named:)
        return true if fields.nil?

        fields.well_formed && fields.entries.all? do |type, _, data|
          next data.bytesize >= 2 && data.end_with?("\0") if named && type == Records::NAME_FIELD

          sizes[type].nil? || data.bytesize == sizes[type]
        end
      end

      # A field type outside the record's table whose flags mark it data
      # dependent.
      def unknown_dependent?(type, flags, sizes) = !sizes.key?(type) && flags.anybits?(DATA_DEPENDENT)

      # A directory-record key that does not fit the volume's variant but
      # fits the other one.
      def other_variant?(key, volume) =
        key.directory_record? && key.suffix.nil? && !Records.key(key.bytes, hashed: !volume.hashed?).suffix.nil?

      # The valid hashed names of a volume whose code points the harness
      # tables assign, or (assigned: false) those holding one they do not.
      def judged_names(records, volume, assigned:)
        return [] unless volume.hashed?

        records.entries.filter_map { _1.key.name && Names.valid?(_1.key.name) ? _1.key : nil }.select { Names.assigned?(_1.name) == assigned }
      end

      def hash_matches?(key, volume) = key.stored_hash == Names.name_hash(Names.form(key.name, case_insensitive: volume.case_insensitive?))

      # Each inode's data stream by its private ID: [inodes using it, its
      # data-stream field].
      def streams(records)
        records.inodes.each_value.filter_map { (stream = records.data_stream(_1)) && [_1, stream] }
               .group_by { _1.first.fields.private_id }
      end

      # Each stream's extents tile a prefix of its logical range in order,
      # each a positive multiple of the block size, none past the larger of
      # the rounded size and the allocated bytes. Attribute streams have no
      # data-stream ID record and are judged by their extents alone.
      def streams_tiled?(records, block_size)
        limits = streams(records).transform_values do |users|
          stream = users.first.last
          [(stream.logical_size + block_size - 1) / block_size * block_size, stream.allocated].max
        end
        records.extents.group_by(&:owner).all? do |owner, extents|
          ordered = extents.sort_by(&:offset)
          ends = ordered.each_with_object([0]) { |extent, starts| starts << starts.last + extent.length }
          tiled = ordered.zip(ends).all? { |extent, start| extent.offset == start && extent.length.positive? && (extent.length % block_size).zero? }
          tiled && (limits[owner].nil? || ends.last <= limits[owner])
        end
      end

      # Every inode stream has a data-stream ID record counting the inodes
      # that use it, and no other stream has one.
      def stream_counts_agree?(records)
        counts = streams(records).transform_values(&:size)
        records.stream_counts == counts
      end

      # A named inode's parent equals the parent of one of its records, and
      # its name field, when present, that record's name; a directory has
      # exactly one record and always a name field (design: Hard links and
      # siblings).
      def parent_agrees?(inode, records)
        names = records.names_of(inode.number)
        return true if names.empty? || inode.fields.nil?

        name = inode.extended&.field(Records::NAME_FIELD)&.then { _1.byteslice(0, _1.bytesize - 1) }
        directory = type_of(inode.fields) == DIRECTORY_TYPE
        return false if directory && (name.nil? || names.size != 1)

        names.any? { _1.parent == inode.fields.parent_id && (name.nil? || _1.key.name == name) }
      end

      # The inodes by the superblock's count classes, the root and private
      # directories excluded.
      def object_counts(records)
        ordinary = records.inodes.reject { |number, _| [ROOT, PRIVATE].include?(number) }.values.filter_map(&:fields)
        classes = ordinary.map do |inode|
          case type_of(inode)
          when FILE_TYPE then :files
          when DIRECTORY_TYPE then :directories
          when SYMLINK_TYPE then :symlinks
          else :other
          end
        end
        { files: 0, directories: 0, symlinks: 0, other: 0 }.merge(classes.tally)
      end

      def volume_counts(volume)
        sb = volume.superblock
        { files: sb.file_count, directories: sb.directory_count, symlinks: sb.symlink_count, other: sb.other_count }
      end
    end
  end
end
