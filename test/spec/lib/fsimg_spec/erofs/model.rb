# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"

module FsimgSpec
  module Erofs
    # One decoded inode: its NID, volume offset, the scope of the check that
    # reaches it (the root is loaded by open, every other inode by listing
    # its directory), the fixed part and the bytes of its attribute region.
    Inode = Data.define(:nid, :offset, :scope, :fields, :extended, :region) do
      def format = fields.format
      def version_bit = format & 1
      def layout = (format >> 1) & 7
      def mode = fields.mode
      def size = fields.size_bytes
      def type_bits = mode & 0o170000
      def directory? = type_bits == 0o040000
      def fixed_bytes = extended ? 64 : 32
      def region_bytes = fields.xattr_count.zero? ? 0 : 12 + 4 * (fields.xattr_count - 1)
      def inline? = layout == 2
      # The layout bytes after the attribute region: the flat-inline tail.
      def tail_bytes(block) = inline? ? size % block : 0
      def body_offset = offset + fixed_bytes + region_bytes
      def body_bytes(block) = fixed_bytes + region_bytes + tail_bytes(block)
    end

    # One directory entry as stored. The name is nil while the block's
    # offsets do not delimit it.
    Entry = Data.define(:nid, :name_offset, :type, :name)
    # One directory block: its valid length, the entries when the first
    # offset frames an array (nil otherwise), and whether the name offsets
    # delimit every name.
    DirectoryBlock = Data.define(:valid, :first_offset, :entries, :delimited)
    Directory = Data.define(:inode, :parent, :blocks) do
      def entries = blocks.flat_map { _1.entries || [] }
    end

    # The independent model holds decoded bytes, not fsimg's interpretation.
    # Bad framing remains a fact for rules to judge; only reaching a work
    # bound makes a scope unclassified.
    class Model < Data.define(:image_length, :magic, :sb, :block, :domain, :checksum, :inodes, :directories, :tree, :cut_off)
      UNKNOWN = Undefined.new(reason: "structure is absent or truncated")
      # The kind fsimg lists for each mode type.
      KINDS = {
        0o100000 => "file", 0o040000 => "directory", 0o120000 => "symlink", 0o010000 => "fifo",
        0o140000 => "socket", 0o020000 => "char_device", 0o060000 => "block_device"
      }.freeze
      # The file type a directory entry stores for each mode type.
      TYPES = { 0o100000 => 1, 0o040000 => 2, 0o020000 => 3, 0o060000 => 4, 0o010000 => 5, 0o140000 => 6, 0o120000 => 7 }.freeze

      def self.decode(image, ceilings: nil) = Decoder.new(image, ceilings:).call

      def unclassified_scopes = cut_off

      def sb_defined? = !FsimgSpec.undefined?(sb)

      def record_problems(result)
        records = result.outcome.records
        case result.stage
        in "info"
          info = records.select { _1["type"] == "info" }
          info.one? ? [] : ["info does not contain exactly one volume"]
        in _ if FsimgSpec.undefined?(tree) then []
        in "ls"
          actual = records.select { _1["type"] == "entry" }.map do |row|
            WalkedEntry.new(path: row.fetch("path").delete_prefix("/").b, type: row.fetch("kind"),
                            size: row.fetch("kind") == "file" ? Integer(row.fetch("size_bytes"), 10) : nil, sha256: nil)
          end
          KnownTree.differences("ls", actual:, expected: tree.map { _1.with(sha256: nil) })
        in "extract" then KnownTree.differences("extract", actual: KnownTree.from_walk(result.tree), expected: tree)
        else []
        end
      end
    end

    class Decoder
      CHUNK = 1 << 16
      # The scopes that read what a cut-off structure held: a scope and
      # every scope after it.
      DEPENDENTS = {
        "open" => %w[open directories files verification destination],
        "directories" => %w[directories files verification destination],
        "files" => %w[files verification destination]
      }.freeze

      def initialize(image, ceilings:)
        @image = image
        @entries_left = ceilings ? [ceilings.entries, 100_000].min : 100_000
        @digest_limit = ceilings ? ceilings.output : 16 << 20
        @digested = 0
        @cut_off = []
        @clean = true
        @inodes = {}
        @directories = []
        @tree = []
      end

      def call
        magic = @image.read(1024, 4)
        sb = @image.length >= 1024 + 128 ? Layout.decode(Layout::Superblock, @image.read(1024, 128)) : Model::UNKNOWN
        return result(magic, sb, Model::UNKNOWN, Model::UNKNOWN) if FsimgSpec.undefined?(sb) || magic != [Layout::MAGIC].pack("V")

        @sb = sb
        @block = sb.blkszbits.between?(9, 12) ? 1 << sb.blkszbits : Model::UNKNOWN
        @domain = FsimgSpec.derive(@block) { sb.blocks.zero? ? @image.length : sb.blocks * _1 }
        walk unless FsimgSpec.undefined?(@block)
        result(magic, sb, @block, @domain)
      end

      # [stored, computed] superblock checksums, over the range the design
      # gives, or Undefined when the volume does not feature a checksum or
      # does not hold the range.
      def checksum_of
        return Model::UNKNOWN unless @sb && @sb.compat.allbits?(1) && !FsimgSpec.undefined?(@block)

        last = @block > 1024 ? @block : 1024 + @block
        bytes = @image.read(1024, last - 1024)
        return Model::UNKNOWN if bytes.bytesize < last - 1024

        [@sb.checksum, Crc32c.stored(bytes.b.tap { _1[4, 4] = "\0\0\0\0".b })]
      end

      private

      def result(magic, sb, block, domain)
        Model.new(image_length: @image.length, magic:, sb:, block:, domain:, checksum: checksum_of, inodes: @inodes.freeze,
                  directories: @directories.freeze, tree: @clean ? @tree.freeze : Model::UNKNOWN, cut_off: @cut_off.freeze)
      end

      # Breadth first from the root, once for every name a directory has: the
      # parent rule makes two names in one parent legal, and ordinary reading
      # lists the shared subtree under each. The rules judge each descent's
      # parent. A directory below itself is recorded for the rules but not
      # entered again, so a cycle ends; the entry bound ends a wide tree.
      def walk
        root = load_inode(@sb.root_nid, "open")
        return @clean = false unless root&.directory?

        queue = [[root, "".b, root.nid, []]]
        until queue.empty?
          inode, path, parent, ancestors = queue.shift
          directory = Directory.new(inode:, parent:, blocks: read_blocks(inode))
          @directories << directory
          next if ancestors.include?(inode.nid)

          directory.entries.each do |entry|
            next if entry.name.nil? || %w[. ..].include?(entry.name)
            break incomplete("directories") if (@entries_left -= 1).negative?

            child = load_inode(entry.nid, "directories")
            next @clean = false unless child

            visit(child, path.empty? ? entry.name : "#{path}/#{entry.name}".b, queue, directory, ancestors + [inode.nid])
          end
        end
      end

      def visit(child, path, queue, directory, ancestors)
        kind = Model::KINDS[child.type_bits]
        @clean = false unless kind
        @tree << WalkedEntry.new(path: path.freeze, type: kind || "other", size: kind == "file" ? child.size : nil,
                                 sha256: kind == "file" ? digest(child) : nil)
        queue << [child, path, directory.inode.nid, ancestors] if child.directory?
      end

      # An inode, or nil when the image does not hold it; the cut-off is
      # recorded against the scope that needed it.
      def load_inode(nid, scope)
        return @inodes[nid] if @inodes.key?(nid)

        offset = @sb.meta_blkaddr * @block + 32 * nid
        head = held(offset, 2, scope)
        return @inodes[nid] = nil unless head

        extended = head.unpack1("v").odd?
        bytes = held(offset, extended ? 64 : 32, scope)
        return @inodes[nid] = nil unless bytes

        fields = Layout.decode(extended ? Layout::ExtendedInode : Layout::CompactInode, bytes)
        inode = Inode.new(nid:, offset:, scope:, fields:, extended:, region: "".b)
        region = inode.region_bytes.zero? ? "".b : held(offset + inode.fixed_bytes, inode.region_bytes, scope)
        @inodes[nid] = inode.with(region: region || "".b)
      end

      # The count bytes at offset, or nil when the image lacks them. A range
      # inside the volume's declared extent that the input does not hold is a
      # truncation, which leaves scope unclassified; a range outside the
      # extent is for the rules (inode.body, inode.data_domain) to judge.
      def held(offset, count, scope)
        bytes = @image.read(offset, count)
        return bytes if bytes.bytesize == count

        incomplete(scope) if !FsimgSpec.undefined?(@domain) && offset + count <= @domain
        nil
      end

      def read_blocks(inode)
        return [] if inode.layout > 2 || inode.size.zero?

        (0...((inode.size + @block - 1) / @block)).filter_map do |index|
          valid = [@block, inode.size - index * @block].min
          bytes = data(inode, index * @block, valid, "directories")
          block(bytes, valid) if bytes
        end
      end

      def block(bytes, valid)
        first = bytes.unpack1("v", offset: 8) || 0
        return DirectoryBlock.new(valid:, first_offset: first, entries: nil, delimited: false) unless
          first.positive? && (first % 12).zero? && first < valid

        entries = (0...(first / 12)).map { Layout.decode(Layout::DirectoryEntry, bytes.byteslice(12 * _1, 12)) }
        offsets = entries.map(&:name_offset)
        delimited = offsets.each_cons(2).all? { _1 < _2 } && offsets.first >= 12 * entries.size && offsets.last <= valid
        names = delimited ? names(bytes, offsets, valid) : Array.new(entries.size)
        DirectoryBlock.new(valid:, first_offset: first, delimited:,
                           entries: entries.zip(names).map { |e, name| Entry.new(nid: e.nid, name_offset: e.name_offset, type: e.file_type, name:) })
      end

      # Each name runs to the next offset; the last to the first NUL or the
      # valid end.
      def names(bytes, offsets, valid)
        ends = offsets.drop(1) + [valid]
        offsets.zip(ends).each_with_index.map do |(from, to), index|
          name = bytes.byteslice(from, to - from)
          index == offsets.size - 1 ? name.sub(/\0.*\z/m, "").b : name.b
        end
      end

      # count bytes at offset of the inode's content, or nil when the image
      # does not hold them. Flat data is read from its start block; the
      # inline tail comes from the inode's body.
      def data(inode, offset, count, scope)
        return "".b if count.zero?
        return nil if offset + count > inode.size

        full = inode.inline? ? inode.size / @block * @block : inode.size
        stored = offset < full ? [count, full - offset].min : 0
        head = stored.zero? ? "".b : held(inode.fields.layout_word * @block + offset, stored, scope)
        tail = count > stored ? held(inode.body_offset + (offset + stored - full), count - stored, scope) : "".b
        head && tail && head + tail
      end

      def digest(inode)
        return nil if inode.layout > 2

        digest = Digest::SHA256.new
        0.step(inode.size - 1, CHUNK) do |offset|
          bytes = data(inode, offset, [CHUNK, inode.size - offset].min, "files")
          return nil unless bytes

          digest << bytes
          @digested += bytes.bytesize
          return incomplete("files") && nil if @digested > @digest_limit
        end
        digest.hexdigest
      end

      def incomplete(scope) = @cut_off |= DEPENDENTS.fetch(scope)
    end
  end
end
