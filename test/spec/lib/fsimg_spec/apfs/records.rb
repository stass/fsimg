# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The filesystem records of the design's Filesystem records section:
    # the key word and each kind's suffix, the inode, directory-record,
    # file-extent, data-stream-ID and attribute values, and the extended
    # fields that end inode and directory-record values. Decoding is total:
    # a part whose bytes do not fit its record decodes as nil, and the
    # record rules judge it.
    module Records
      KIND_SHIFT = 60
      OWNER_MASK = (1 << KIND_SHIFT) - 1
      INODE = 3
      ATTRIBUTE = 4
      SIBLING_LINK = 5
      STREAM_ID = 6
      EXTENT = 8
      DIRECTORY_RECORD = 9
      DIRECTORY_STATS = 10
      SIBLING_MAP = 12
      FILE_INFO = 13
      HASHED_LENGTH_BITS = 10
      HASHED_LENGTH_MASK = (1 << HASHED_LENGTH_BITS) - 1
      # Kinds whose key is the 8-byte word alone.
      BARE_KINDS = [1, 2, INODE, STREAM_ID, 7, DIRECTORY_STATS, SIBLING_MAP].freeze

      # A filesystem-tree key: owner and kind from its word, the suffix
      # its kind orders by after them (design: Comparators), nil when the
      # suffix does not fit the key, and whether a directory record's key
      # is hashed. A name in a suffix keeps its terminating NUL, the form
      # names compare in.
      Key = Data.define(:owner, :kind, :suffix, :hashed, :bytes) do
        def directory_record? = kind == DIRECTORY_RECORD

        # A plain directory record's name order is undecided (design:
        # Comparators), so equal plain keys are legal.
        def undecided? = directory_record? && !hashed

        # A directory record's or attribute's stored name without its NUL,
        # or nil when the suffix holds none.
        def name
          return nil unless suffix && [DIRECTORY_RECORD, ATTRIBUTE].include?(kind)

          stored = suffix.last
          stored.end_with?("\0") ? stored.byteslice(0, stored.bytesize - 1) : nil
        end

        def stored_hash = directory_record? && hashed && suffix ? suffix.first : nil
      end

      # An inode record: its number, the fixed part, nil when the value is
      # shorter, and its extended fields.
      Inode = Data.define(:number, :fields, :extended)

      # A directory record: its parent (the key's owner), its key, the
      # value's fixed part and its extended fields.
      Entry = Data.define(:parent, :key, :fields, :extended) do
        def child = fields&.child
        def type = fields && (fields.flags & 0xF)
      end

      Extent = Data.define(:owner, :offset, :fields) do
        def length = fields.length_and_flags & ((1 << 56) - 1)
        def flags = fields.length_and_flags >> 56
        def hole? = fields.physical_block.zero?
      end

      # An attribute record: its owner, its flags and the stream ID a
      # stream attribute names (nil otherwise).
      Attribute = Data.define(:owner, :flags, :stream_id)
      STREAM_ATTRIBUTE = 0x1

      # Every record of one tree's leaves, each pair [Key, value bytes],
      # and the decoded families the record rules reconcile: inodes by
      # number, directory records also grouped by child and by parent,
      # extents, each data-stream ID record's reference count by its owner
      # (nil when the value is not 4 bytes) and attributes.
      RecordSet = Data.define(:pairs, :inodes, :entries, :by_child, :by_parent, :extents, :stream_counts, :attributes) do
        def inode(number) = inodes[number]
        def names_of(number) = by_child.fetch(number, [])
        def children_of(parent) = by_parent.fetch(parent, [])

        # The data-stream field of an inode, or nil when it has none.
        def data_stream(inode) = inode.extended&.field(DATA_STREAM_FIELD)&.then { Layout.find(Layout::DataStream, _1) }
      end
      DATA_STREAM_FIELD = 8
      NAME_FIELD = 4

      module_function

      # The key at bytes, at least the 8-byte word; hashed says which
      # directory-record variant the volume uses.
      def key(bytes, hashed:)
        word = bytes.unpack1("Q<")
        kind = word >> KIND_SHIFT
        Key.new(owner: word & OWNER_MASK, kind:, suffix: suffix(kind, bytes, hashed), hashed:, bytes:)
      end

      def suffix(kind, bytes, hashed)
        rest = bytes.byteslice(8..)
        case kind
        when *BARE_KINDS then rest.empty? ? [] : nil
        when EXTENT, SIBLING_LINK, FILE_INFO then rest.bytesize == 8 ? [rest.unpack1("Q<")] : nil
        when ATTRIBUTE then counted_name(rest, rest.unpack1("v"), 2)&.then { [_1] }
        when DIRECTORY_RECORD then hashed ? hashed_suffix(rest) : counted_name(rest, rest.unpack1("v"), 2)&.then { [_1] }
        end
      end

      # A name with its NUL after a header of width bytes, whose length
      # field counts it exactly.
      def counted_name(rest, length, width)
        return nil if rest.bytesize < width || length.nil?

        name = rest.byteslice(width..)
        name.bytesize == length ? name : nil
      end

      def hashed_suffix(rest)
        word = rest.unpack1("V")
        name = word && counted_name(rest, word & HASHED_LENGTH_MASK, 4)
        name && [word >> HASHED_LENGTH_BITS, name]
      end

      # The record families of a decoded filesystem tree.
      # A ghost value, which no filesystem tree may hold, is left to the
      # node rule that judges it and decodes as no record.
      def decode(tree)
        pairs = tree.records.reject { _1.last == :ghost }
        by_kind = pairs.group_by { _1.first.kind }
        of = ->(kind) { by_kind.fetch(kind, []) }
        entries = of.call(DIRECTORY_RECORD).map { |key, value| entry(key, value) }
        RecordSet.new(
          pairs:,
          inodes: of.call(INODE).to_h { |key, value| [key.owner, inode(key, value)] },
          entries:, by_child: entries.group_by(&:child), by_parent: entries.group_by(&:parent),
          extents: of.call(EXTENT).select { _1.first.suffix && _1.last.bytesize == Layout.size(Layout::FileExtent) }
                                .map { |key, value| Extent.new(owner: key.owner, offset: key.suffix.first, fields: Layout.find(Layout::FileExtent, value)) },
          stream_counts: of.call(STREAM_ID).to_h { |key, value| [key.owner, value.bytesize == 4 ? value.unpack1("V") : nil] },
          attributes: of.call(ATTRIBUTE).map { |key, value| attribute(key, value) }
        )
      end

      def inode(key, value)
        fixed = Layout.size(Layout::Inode)
        Inode.new(number: key.owner, fields: Layout.find(Layout::Inode, value), extended: value.bytesize >= fixed ? Fields.parse(value, fixed) : nil)
      end

      def entry(key, value)
        fixed = Layout.size(Layout::DirectoryRecord)
        Entry.new(parent: key.owner, key:, fields: Layout.find(Layout::DirectoryRecord, value),
                  extended: value.bytesize >= fixed ? Fields.parse(value, fixed) : nil)
      end

      # A stream attribute's value: u16 flags, u16 length, then the stream
      # OID and a data-stream field (design: Extended attributes).
      def attribute(key, value)
        flags = value.unpack1("v").to_i
        stream = flags.anybits?(STREAM_ATTRIBUTE) && value.bytesize >= 12 ? value.unpack1("Q<", offset: 4) : nil
        Attribute.new(owner: key.owner, flags:, stream_id: stream)
      end

      private_class_method :suffix, :counted_name, :hashed_suffix, :inode, :entry, :attribute
    end

    # An extended-field blob (design: Extended fields): the fields as
    # [type, flags, data], and whether the counts, sizes and padding fit
    # the record exactly with no type repeated. A value that ends at its
    # fixed part has no blob.
    class Fields < Data.define(:entries, :well_formed)
      ALIGNMENT = 8

      def self.parse(value, at)
        return new(entries: [], well_formed: true) if value.bytesize == at

        head = Layout.find(Layout::FieldsHead, value, at:)
        descriptors = head ? Layout.records(Layout::FieldEntry, value, at: at + 4, count: head.field_count) : []
        return new(entries: [], well_formed: false) unless head && descriptors.size == head.field_count

        data_start = at + 4 + Layout.size(Layout::FieldEntry) * head.field_count
        padded = descriptors.map { (_1.data_size + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT }
        offsets = padded.each_with_object([0]) { |size, starts| starts << starts.last + size }
        entries = descriptors.zip(offsets).map { |d, offset| [d.field_type, d.flags, value.byteslice(data_start + offset, d.data_size)] }
        padding = descriptors.zip(offsets, padded).map { |d, offset, size| value.byteslice(data_start + offset + d.data_size, size - d.data_size) }
        exact = padded.sum == head.used_bytes && data_start + head.used_bytes == value.bytesize
        new(entries:, well_formed: exact && padding.all? { _1&.bytes&.all?(&:zero?) } && entries.map(&:first).uniq.size == entries.size)
      end

      # The data of the field of type, or nil.
      def field(type) = entries.find { _1.first == type }&.last
    end

    # Directory-record names and their hash (design: Directory records and
    # names). The comparison form uses Ruby's Unicode tables, written
    # independently of fsimg's; the hash rules judge only names whose code
    # points those tables assign, and only when the tables are the 17.0.0
    # the design applies.
    module Names
      CASTAGNOLI = 0x82F63B78
      TABLE = (0..255).map { |byte| 8.times.reduce(byte) { |crc, _| crc.odd? ? (crc >> 1) ^ CASTAGNOLI : crc >> 1 } }.freeze
      HASH_MASK = (1 << 22) - 1
      UNICODE_VERSION = "17.0.0"
      TABLES_MATCH = RbConfig::CONFIG["UNICODE_VERSION"] == UNICODE_VERSION

      module_function

      # The comparison form of a valid UTF-8 name.
      def form(name, case_insensitive:)
        decomposed = nfd(name.dup.force_encoding(Encoding::UTF_8))
        case_insensitive ? nfd(decomposed.downcase(:fold)) : decomposed
      end

      # NFD: each character's full canonical decomposition, then the
      # canonical ordering of every run of non-starters by combining class.
      # String#unicode_normalize alone does not reorder marks across a
      # starter that decomposes into non-starters, such as U+0F73, U+0F75
      # and U+0F81 after a mark of class 220: E4's names hold such runs.
      def nfd(text)
        decomposed = text.each_char.map { _1.unicode_normalize(:nfd) }.join.each_char
        decomposed.slice_before { combining_class(_1).zero? }.flat_map do |run|
          starter = combining_class(run.first).zero? ? run.take(1) : []
          starter + run.drop(starter.size).each_with_index.sort_by { |char, i| [combining_class(char), i] }.map(&:first)
        end.join
      end

      # A code point's canonical combining class, from the table Ruby's
      # normalization carries, which String#unicode_normalize loads.
      def combining_class(char) = UnicodeNormalize::CLASS_TABLE.fetch(char, 0)

      # CRC-32C over the form's code points as little-endian 32-bit units,
      # from 0xFFFFFFFF without the final complement, low 22 bits.
      def name_hash(form) =
        form.codepoints.pack("V*").bytes.reduce(0xFFFFFFFF) { |crc, byte| TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8) } & HASH_MASK

      # Nonempty valid UTF-8 without NUL or slash, neither . nor ..
      def valid?(bytes)
        text = bytes.dup.force_encoding(Encoding::UTF_8)
        !bytes.empty? && text.valid_encoding? && !bytes.match?(%r{[\0/]}n) && !%w[. ..].include?(bytes)
      end

      # Whether the harness tables assign every code point of the name: a
      # noncharacter or an unassigned code point is general category Cn.
      def assigned?(bytes) = !bytes.dup.force_encoding(Encoding::UTF_8).match?(/\p{Cn}/)
    end
  end
end
