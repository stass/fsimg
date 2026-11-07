#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Print the structure of an EROFS image: every superblock field, the
# superblock checksum verdict, the compression configuration records, and for
# each inode reachable from the root its header, extended attributes, data
# layout, directory entries and, for compressed inodes, every index record.
#
#   erofsdump.rb [--no-tree] IMAGE
#
# The dumper is written from specs/design-erofs.md alone and shares no code
# with lib/. It is a development oracle for the format design: it prints what
# the image holds and reports each rule the image breaks as a `problem:` line
# instead of stopping. A read outside the image or an unusable superblock is
# an error: the message goes to standard error and the exit status is 1.
# A `problem:` line also makes the exit status 1. A feature the design
# classifies as unsupported (or an image whose tree this dumper cannot
# decode: the 48-bit layout, the metadata box) is reported as an
# `unsupported:` or `note:` line and, absent any problem, exits with status 2.
#
# Where the design marks a rule provisional the dumper implements the design's
# default and prints a `note:`.
require "optparse"

module ErofsDump
  class Error < StandardError; end

  # A volume the design refuses as unsupported, where reading cannot go on.
  class Unsupported < Error; end

  MAGIC = 0xE0F5E1E2
  SB_OFFSET = 1024
  SB_REGION_END = 4096
  SB_BASE_SIZE = 128
  MAX_DIRECTORY_BYTES = 64 << 20
  MAX_NAME = 255
  SYMLINK_MAX = 1024

  COMPAT = {
    0x01 => "SB_CHKSUM", 0x02 => "MTIME", 0x04 => "XATTR_FILTER",
    0x08 => "SHARED_EA_IN_METABOX", 0x10 => "PLAIN_XATTR_PFX", 0x20 => "ISHARE_XATTRS"
  }.freeze
  INCOMPAT = {
    0x001 => "LZ4_0PADDING", 0x002 => "COMPR_CFGS", 0x004 => "CHUNKED_FILE",
    0x008 => "DEVICE_TABLE/COMPR_HEAD2", 0x010 => "ZTAILPACKING",
    0x020 => "FRAGMENTS/DEDUPE", 0x040 => "XATTR_PREFIXES", 0x080 => "48BIT",
    0x100 => "METABOX"
  }.freeze
  COMPAT_SB_CHKSUM = 0x01
  COMPAT_XATTR_FILTER = 0x04
  COMPAT_PLAIN_XATTR_PFX = 0x10
  INCOMPAT_LZ4_0PADDING = 0x001
  INCOMPAT_COMPR_CFGS = 0x002
  INCOMPAT_CHUNKED_FILE = 0x004
  INCOMPAT_COMPR_HEAD2 = 0x008
  INCOMPAT_ZTAILPACKING = 0x010
  INCOMPAT_FRAGMENTS = 0x020
  INCOMPAT_XATTR_PREFIXES = 0x040
  INCOMPAT_48BIT = 0x080
  INCOMPAT_METABOX = 0x100

  LAYOUT_FLAT = 0
  LAYOUT_FULL_INDEX = 1
  LAYOUT_INLINE = 2
  LAYOUT_COMPACT_INDEX = 3
  LAYOUT_CHUNK = 4
  LAYOUTS = {
    LAYOUT_FLAT => "flat", LAYOUT_FULL_INDEX => "compressed-full-index", LAYOUT_INLINE => "flat-inline",
    LAYOUT_COMPACT_INDEX => "compressed-compact-index", LAYOUT_CHUNK => "chunk-based"
  }.freeze

  KIND_FILE = 0o100000
  KIND_DIR = 0o040000
  KIND_SYMLINK = 0o120000
  KIND_FIFO = 0o010000
  KIND_CHARDEV = 0o020000
  KIND_BLOCKDEV = 0o060000
  KIND_SOCKET = 0o140000
  KINDS = {
    KIND_FILE => "file", KIND_DIR => "dir", KIND_SYMLINK => "symlink", KIND_FIFO => "fifo",
    KIND_CHARDEV => "chardev", KIND_BLOCKDEV => "blockdev", KIND_SOCKET => "socket"
  }.freeze
  DIRENT_TYPES = {
    0 => "unknown", 1 => "file", 2 => "dir", 3 => "chardev", 4 => "blockdev",
    5 => "fifo", 6 => "socket", 7 => "symlink"
  }.freeze
  # Directory entry file type for each inode kind.
  KIND_TO_DIRENT = {
    KIND_FILE => 1, KIND_DIR => 2, KIND_CHARDEV => 3, KIND_BLOCKDEV => 4,
    KIND_FIFO => 5, KIND_SOCKET => 6, KIND_SYMLINK => 7
  }.freeze

  ALGORITHMS = { 0 => "LZ4", 1 => "MicroLZMA", 2 => "DEFLATE", 3 => "Zstandard" }.freeze
  TYPE_PLAIN = 0
  TYPE_HEAD1 = 1
  TYPE_NONHEAD = 2
  TYPE_HEAD2 = 3
  RECORD_TYPES = { TYPE_PLAIN => "plain", TYPE_HEAD1 => "head1", TYPE_NONHEAD => "nonhead", TYPE_HEAD2 => "head2" }.freeze

  XATTR_PREFIXES = {
    1 => "user.", 2 => "system.posix_acl_access", 3 => "system.posix_acl_default",
    4 => "trusted.", 6 => "security."
  }.freeze

  # The advice bits this dumper knows; any other is reserved.
  ADVICE_KNOWN = 0x3F
  # Bits 2 to 14 of a full index record's advice word are reserved.
  RECORD_RESERVED_BITS = 0x7FFC
  ADVICE_TWO_BYTE = 0x01
  ADVICE_HEAD1_BIG = 0x02
  ADVICE_HEAD2_BIG = 0x04
  ADVICE_BIG = ADVICE_HEAD1_BIG | ADVICE_HEAD2_BIG
  ADVICE_INLINE_TAIL = 0x08
  ADVICE_FRAGMENT_TAIL = 0x20
  ADVICE = {
    ADVICE_TWO_BYTE => "compact-2byte/48bit-extents", ADVICE_HEAD1_BIG => "head1-big",
    ADVICE_HEAD2_BIG => "head2-big", ADVICE_INLINE_TAIL => "inline-tail", 0x10 => "interlaced",
    ADVICE_FRAGMENT_TAIL => "fragment-tail"
  }.freeze

  # A compact index record's third type value, and its flag in the value
  # field: the marker that a physical cluster spans several blocks.
  BIG_CLUSTER_FLAG = 0x800
  BIG_CLUSTER_COUNT = 0x7FF
  # A physical cluster is at most 1 MiB stored.
  MAX_CLUSTER_BYTES = 1 << 20
  INT64_MAX = (1 << 63) - 1
  # Name filter hash seed; the built-in prefix index is added to it.
  FILTER_SEED = 0x25BBE08F

  # Superblock fields: name, byte offset, unpack directive. The metabox NID
  # and the word after it lie in the first extension slot and are read apart.
  SB_FIELDS = [
    [:magic, 0x00, "L<"], [:checksum, 0x04, "L<"], [:compat, 0x08, "L<"],
    [:blkszbits, 0x0C, "C"], [:ext_slots, 0x0D, "C"], [:root_nid16, 0x0E, "S<"],
    [:inodes, 0x10, "Q<"], [:epoch, 0x18, "q<"], [:fixed_nsec, 0x20, "L<"],
    [:blocks, 0x24, "L<"], [:meta_blkaddr, 0x28, "L<"], [:xattr_blkaddr, 0x2C, "L<"],
    [:uuid, 0x30, "a16"], [:volume_name, 0x40, "a16"], [:incompat, 0x50, "L<"],
    [:u1, 0x54, "S<"], [:extra_devices, 0x56, "S<"], [:devt_slotoff, 0x58, "S<"],
    [:dirblkbits, 0x5A, "C"], [:prefix_count, 0x5B, "C"], [:prefix_start, 0x5C, "L<"],
    [:packed_nid, 0x60, "Q<"], [:filter_reserved, 0x68, "C"], [:ishare_prefix, 0x69, "C"],
    [:reserved_6a, 0x6A, "S<"], [:build_time, 0x6C, "L<"], [:wide_root_nid, 0x70, "Q<"],
    [:reserved_78, 0x78, "Q<"]
  ].freeze

  Superblock = Data.define(*SB_FIELDS.map(&:first), :metabox_nid, :reserved_88) do
    def block_size = 1 << blkszbits
    def size = SB_BASE_SIZE + 16 * ext_slots
    def wide? = incompat.anybits?(INCOMPAT_48BIT)
    def root_nid = wide? ? wide_root_nid : root_nid16
  end

  # `reserved` holds the bytes of the inode's reserved fields.
  Inode = Data.define(
    :nid, :offset, :extended, :layout, :format_word, :xcount, :mode, :nlink, :size,
    :layout_word, :ino, :uid, :gid, :time, :nsec, :reserved
  ) do
    def fixed_size = extended ? 64 : 32
    def xattr_size = xcount.zero? ? 0 : 12 + 4 * (xcount - 1)
    def attrs_end = offset + fixed_size + xattr_size
    def kind = mode & 0o170000
  end

  # The 16-bit chunk format at 0x10 of a chunk-based inode (the upper half of
  # the word is not part of it).
  ChunkFormat = Data.define(:word) do
    def format16 = word & 0xFFFF
    def bits = word & 0x1F
    def wide_entries? = word.anybits?(0x20)
    def wide_blocks? = word.anybits?(0x40)
    def width = wide_entries? ? 8 : 4
    def reserved_bits = format16 & ~0x7F
    def upper_half = word >> 16
  end

  # One chunk index entry; `block` is nil for a hole and `extra` is nonzero
  # when the entry holds a device id or an unusable high part.
  ChunkEntry = Data.define(:index, :logical, :length, :block, :extra)

  # A run of directory data: at image offset `pos`, or a hole when nil.
  Segment = Data.define(:pos, :len)

  Dirent = Data.define(:nid, :type, :name)
  Pending = Data.define(:nid, :expected_type, :parent)
  # `bit` is the entry's name-filter bit, or nil when its long prefix is unknown.
  XattrEntry = Data.define(:text, :stop, :bit)
  # A long xattr prefix: a built-in base index and the infix after it.
  LongPrefix = Data.define(:base, :infix) do
    def full = "#{XATTR_PREFIXES.fetch(base, '')}#{infix.b}"
  end

  # Compressed map header, the 8 bytes before the index.
  MapHeader = Data.define(:raw) do
    def advice = raw.unpack1("@4S<")
    def algorithms = raw.getbyte(6)
    def cluster_byte = raw.getbyte(7)
    def head1_algorithm = algorithms & 0xF
    def head2_algorithm = algorithms >> 4
    def word0 = raw.unpack1("L<")
    def tail_size = raw.unpack1("@2S<")
    def whole_file_fragment? = cluster_byte.anybits?(0x80)
    def whole_file_fragment_offset = raw.unpack1("Q<") & ~(1 << 63)
    def cluster_extra_bits = cluster_byte & 0xF
  end

  # A head or plain index record (type 0, 1 or 3) and a non-head record.
  HeadRecord = Data.define(:index, :type, :clusterofs, :blkaddr, :partial)
  NonheadRecord = Data.define(:index, :delta0, :delta1, :big_blocks) do
    def type = TYPE_NONHEAD
  end
  # The index records of an inode and the offset just past the index.
  Index = Data.define(:records, :end_pos)

  # One field of a compact group: the record type and the value bits.
  CompactField = Data.define(:type, :value) do
    def big? = type == TYPE_NONHEAD && value.anybits?(BIG_CLUSTER_FLAG)
    def big_count = value & BIG_CLUSTER_COUNT
  end

  # CRC-32C (Castagnoli polynomial, reflected), without the final inversion.
  module Crc32c
    TABLE = Array.new(256) do |n|
      8.times.reduce(n) { |c, _| c.odd? ? (c >> 1) ^ 0x82F63B78 : c >> 1 }
    end.freeze

    module_function

    def update(seed, bytes) = bytes.each_byte.reduce(seed) { |c, b| TABLE[(c ^ b) & 0xFF] ^ (c >> 8) }
  end

  # XXH32 as doc/xxhash_spec.md in the Cyan4973/xxHash repository (commit
  # 2ec67caeb18ec15ae60c0853174331089acddfc0) describes it.
  module Xxh32
    PRIME1 = 0x9E3779B1
    PRIME2 = 0x85EBCA77
    PRIME3 = 0xC2B2AE3D
    PRIME4 = 0x27D4EB2F
    PRIME5 = 0x165667B1
    MASK = 0xFFFFFFFF

    module_function

    def rotl(value, bits) = ((value << bits) | (value >> (32 - bits))) & MASK
    def round(acc, lane) = (rotl((acc + lane * PRIME2) & MASK, 13) * PRIME1) & MASK

    # The hash of `bytes` under `seed`: stripes of four lanes, then the
    # remaining words and bytes, then the avalanche.
    def hash(bytes, seed: 0)
      data = bytes.b
      stripes = data.bytesize / 16
      acc = stripes.zero? ? (seed + PRIME5) & MASK : converge(data, stripes, seed)
      acc = (acc + data.bytesize) & MASK
      tail = data.byteslice(stripes * 16..)
      words = tail.bytesize / 4
      acc = tail.unpack("L<#{words}").reduce(acc) { |a, lane| (rotl((a + lane * PRIME3) & MASK, 17) * PRIME4) & MASK }
      acc = tail.byteslice(words * 4..).each_byte.reduce(acc) { |a, byte| (rotl((a + byte * PRIME5) & MASK, 11) * PRIME1) & MASK }
      avalanche(acc)
    end

    def converge(data, stripes, seed)
      start = [seed + PRIME1 + PRIME2, seed + PRIME2, seed, seed - PRIME1].map { _1 & MASK }
      accs = data.byteslice(0, stripes * 16).unpack("L<*").each_slice(4).reduce(start) do |current, lanes|
        current.zip(lanes).map { |acc, lane| round(acc, lane) }
      end
      accs.zip([1, 7, 12, 18]).sum { |acc, bits| rotl(acc, bits) } & MASK
    end

    def avalanche(acc)
      acc = ((acc ^ (acc >> 15)) * PRIME2) & MASK
      acc = ((acc ^ (acc >> 13)) * PRIME3) & MASK
      acc ^ (acc >> 16)
    end
  end

  # Bounds-checked access to the image.
  class Volume
    attr_reader :size

    def initialize(io)
      @io = io
      @size = io.size
    end

    def read(off, len)
      check_span(off, len)
      return "".b if len.zero?

      data = @io.pread(len, off)
      raise Error, "short read of #{len} bytes at #{off}" if data.bytesize != len

      data
    end

    # Raises unless `len` bytes at `off` lie inside the image; reads nothing.
    def check_span(off, len)
      return unless off.negative? || len.negative? || off + len > @size

      raise Error, "#{len} bytes at #{off} are outside the image (#{@size} bytes)"
    end
  end

  module Fmt
    module_function

    def names(table, word)
      known = table.filter_map { |bit, name| name if word.anybits?(bit) }
      unknown = word & ~table.keys.reduce(0, :|)
      (known + (unknown.zero? ? [] : [format("unknown(0x%x)", unknown)])).join(",")
    end

    def time(sec, nsec)
      format("%s.%09d", Time.at(sec).utc.strftime("%Y-%m-%dT%H:%M:%S"), nsec)
    rescue RangeError, ArgumentError
      "#{sec}.#{format('%09d', nsec)}"
    end

    def name(bytes) = bytes.b.dump

    def hex(bytes, limit = 32)
      shown = bytes.byteslice(0, limit).unpack1("H*")
      bytes.bytesize > limit ? "#{shown}..." : shown
    end
  end

  # Walks an image and prints what it holds. Every read after the superblock
  # lies inside the declared volume (blocks * block size, or the file when
  # the superblock declares no block count).
  class Dumper
    def initialize(volume, out)
      @vol = volume
      @out = out
      @problems = 0
      @unsupported = 0
      @lz4_max_blocks = nil
      @seen = Set.new
      @directories = Set.new
      @noted = Set.new
    end

    attr_reader :problems, :unsupported

    # Prints the image. Returns :tree_not_decoded when the walk is skipped
    # because the image uses a layout this dumper does not decode, else :ok.
    def dump(tree: true)
      @sb = read_superblock
      print_superblock
      verify_checksum
      read_configs
      read_prefix_table
      return :ok unless tree

      # A volume refused at open has no readable tree.
      if @unsupported.positive?
        note "the tree is not decoded: the volume is refused as unsupported"
        return :tree_not_decoded
      end
      if @sb.incompat.anybits?(INCOMPAT_48BIT | INCOMPAT_METABOX)
        note "the tree is not decoded: the 48-bit layout and metadata-box inodes are not supported by this dumper"
        return :tree_not_decoded
      end
      walk(@sb.root_nid)
      :ok
    end

    private

    def say(text = "") = @out.puts(text)
    def note(text) = say("note: #{text}")

    def note_once(text)
      note(text) if @noted.add?(text)
    end

    # A feature the design refuses as unsupported rather than corrupt.
    def unsupported_feature(text)
      @unsupported += 1
      say "unsupported: #{text}" if @noted.add?("unsupported: #{text}")
    end

    def problem(text)
      @problems += 1
      say("problem: #{text}")
    end

    # ---- bounded reads -----------------------------------------------

    # Raises unless the bytes lie inside the declared volume and the image.
    def span(off, len)
      if off.negative? || len.negative? || off + len > @domain
        raise Error, "#{len} bytes at #{off} are outside the volume (#{@domain} bytes)"
      end

      @vol.check_span(off, len)
    end

    def read(off, len)
      span(off, len)
      @vol.read(off, len)
    end

    def u16(off) = read(off, 2).unpack1("S<")
    def u32(off) = read(off, 4).unpack1("L<")
    def align(value, unit) = (value + unit - 1) / unit * unit

    # ---- superblock -------------------------------------------------

    def read_superblock
      base = @vol.read(SB_OFFSET, SB_BASE_SIZE)
      fields = SB_FIELDS.to_h { |name, off, fmt| [name, base.unpack1(fmt, offset: off)] }
      raise Error, format("not EROFS: magic 0x%08x at offset 1024", fields[:magic]) if fields[:magic] != MAGIC

      full = @vol.read(SB_OFFSET, SB_BASE_SIZE + 16 * fields[:ext_slots]).b
      sb = Superblock.new(**fields, metabox_nid: full.bytesize >= 0x88 ? full.unpack1("Q<", offset: 0x80) : nil,
                                    reserved_88: full.bytesize >= 0x90 ? full.unpack1("Q<", offset: 0x88) : nil)
      raise Error, "blkszbits #{sb.blkszbits} is below 9" if sb.blkszbits < 9
      raise Unsupported, "blkszbits #{sb.blkszbits} is above 12" if sb.blkszbits > 12
      raise Unsupported, "superblock of #{sb.size} bytes ends past byte #{SB_REGION_END}" if SB_OFFSET + sb.size > SB_REGION_END

      @domain = sb.blocks.zero? ? @vol.size : sb.blocks * sb.block_size
      sb
    end

    def print_superblock
      sb = @sb
      say "superblock (offset 1024, #{sb.size} bytes)"
      rows = [
        ["magic", format("0x%08X", sb.magic)],
        ["checksum", format("0x%08X", sb.checksum)],
        ["compatible features", "#{format('0x%x', sb.compat)} #{Fmt.names(COMPAT, sb.compat)}"],
        ["blkszbits", "#{sb.blkszbits} (block size #{sb.block_size})"],
        ["extension slots", sb.ext_slots],
        ["root nid (0x0E)", sb.root_nid16],
        ["inode count", sb.inodes],
        ["epoch", "#{sb.epoch} #{Fmt.time(sb.epoch, sb.fixed_nsec)}"],
        ["fixed nanoseconds", sb.fixed_nsec],
        ["block count (0x24)", sb.blocks],
        ["metadata block", sb.meta_blkaddr],
        ["shared-attribute block", sb.xattr_blkaddr],
        ["uuid", sb.uuid.unpack1("H*").sub(/\A(.{8})(.{4})(.{4})(.{4})(.{12})\z/, '\1-\2-\3-\4-\5')],
        ["volume name", Fmt.name(sb.volume_name.sub(/\0.*\z/m, ""))],
        ["incompatible features", "#{format('0x%x', sb.incompat)} #{Fmt.names(INCOMPAT, sb.incompat)}"],
        ["algorithm mask / lz4 max distance (0x54)", sb.u1],
        ["extra device count", sb.extra_devices],
        ["device table slot", sb.devt_slotoff],
        ["dirblkbits", sb.dirblkbits],
        ["long-prefix count", sb.prefix_count],
        ["long-prefix start", sb.prefix_start],
        ["packed nid", sb.packed_nid],
        ["attribute filter reserved byte", sb.filter_reserved],
        ["image-share prefix index", sb.ishare_prefix],
        ["reserved (0x6A)", sb.reserved_6a],
        ["build time (0x6C)", sb.build_time],
        ["wide root nid (0x70)", sb.wide_root_nid],
        ["reserved (0x78)", sb.reserved_78],
        ["metabox nid (0x80)", sb.metabox_nid.inspect],
        ["reserved (0x88)", sb.reserved_88.inspect]
      ]
      rows.each { |label, value| say format("  %-42s %s", label, value) }
      note "build time at 0x6C is provisional in the design: reported, not interpreted"
      superblock_problems
    end

    def superblock_problems
      sb = @sb
      unknown_incompat = sb.incompat & ~INCOMPAT.keys.reduce(0, :|)
      unknown_compat = sb.compat & ~COMPAT.keys.reduce(0, :|)
      problem "fixed nanoseconds #{sb.fixed_nsec} is not below 1000000000" if sb.fixed_nsec >= 1_000_000_000
      unsupported_feature "dirblkbits #{sb.dirblkbits} is nonzero" unless sb.dirblkbits.zero?
      unsupported_feature "extra device count #{sb.extra_devices} is nonzero" unless sb.extra_devices.zero?
      unsupported_feature "unknown incompatible bits #{format('0x%x', unknown_incompat)}" unless unknown_incompat.zero?
      note "unknown compatible bits #{format('0x%x', unknown_compat)}" unless unknown_compat.zero?
      note "reserved bytes are nonzero" unless sb.reserved_6a.zero? && sb.reserved_78.zero? && sb.reserved_88.to_i.zero?
      note "image is #{@vol.size} bytes; the superblock declares #{@domain}" if @domain != @vol.size
      problem "the image is shorter than the declared #{@domain} bytes" if @domain > @vol.size
    end

    # The design's covered range is [1024, B) when B > 1024, else
    # [1024, 1024 + B). Seed all ones, no final inversion. Every variant that
    # matches is listed, since the range and the inversion are provisional.
    def verify_checksum
      unless @sb.compat.anybits?(COMPAT_SB_CHKSUM)
        say "checksum: SB_CHKSUM is not set; the superblock is unprotected"
        return
      end
      b = @sb.block_size
      design_end = b > 1024 ? b : 1024 + b
      other_end = b > 1024 ? 1024 + b : b
      matches = [["design range", design_end], ["other range", other_end]].flat_map do |label, last|
        covered(last).flat_map do |bytes|
          crc = Crc32c.update(0xFFFFFFFF, bytes)
          [["no inversion", crc], ["inverted", crc ^ 0xFFFFFFFF]].filter_map { |inv, v| "#{label} #{inv}" if v == @sb.checksum }
        end
      end
      if matches.first&.start_with?("design range no inversion")
        say "checksum: ok (CRC-32C over [1024, #{design_end}), seed all ones, no final inversion)"
      else
        problem "superblock checksum mismatch under the design's rule; matching variants: #{matches.empty? ? 'none' : matches.join(', ')}"
      end
      note "the checksum range, seed and inversion are provisional in the design (E1)"
    end

    # The covered bytes with the checksum field read as zero, or nothing when
    # the range runs past the image.
    def covered(last)
      return [] if SB_OFFSET + 4 > last || last > @vol.size

      bytes = @vol.read(SB_OFFSET, last - SB_OFFSET).b
      [bytes.dup.tap { _1[4, 4] = "\0\0\0\0" }]
    end

    # ---- compression configuration -----------------------------------

    CONFIG_LENGTHS = { 0 => 14, 1 => 14, 2 => 6, 3 => 6 }.freeze

    # The algorithms an inode may name: the superblock's mask under
    # COMPR_CFGS, LZ4 alone otherwise.
    def allowed_algorithms
      return [0] unless @sb.incompat.anybits?(INCOMPAT_COMPR_CFGS)

      ALGORITHMS.keys.select { @sb.u1.anybits?(1 << _1) }
    end

    def read_configs
      return unless @sb.incompat.anybits?(INCOMPAT_COMPR_CFGS)

      say "compression configuration (algorithm mask #{format('0x%x', @sb.u1)})"
      unsupported_feature "algorithm mask has bits above 3" if @sb.u1 > 0xF
      allowed_algorithms.reduce(SB_OFFSET + @sb.size) do |pos, alg|
        pos = read_config(alg, pos)
        break unless pos

        pos
      end
    end

    # Prints the record of `alg` at or after `pos`; returns the position
    # after it, or nil when the record cannot be read.
    def read_config(alg, pos)
      pos = align(pos, 4)
      return config_outside(alg, "header") if pos + 2 > SB_REGION_END
      return config_truncated(alg) if pos + 2 > @domain

      len = u16(pos)
      return config_outside(alg, "record of #{len} bytes") if pos + 2 + len > SB_REGION_END
      return config_truncated(alg) if pos + 2 + len > @domain

      payload = read(pos + 2, len)
      unsupported_feature "configuration record of #{len} bytes for #{ALGORITHMS[alg]}, expected #{CONFIG_LENGTHS[alg]}" if len != CONFIG_LENGTHS[alg]
      detail = len == CONFIG_LENGTHS[alg] ? config_fields(alg, payload) : Fmt.hex(payload)
      say format("  %-10s offset %d length %d: %s", ALGORITHMS[alg], pos, len, detail)
      check_config_bounds(alg, payload) if len == CONFIG_LENGTHS[alg]
      @lz4_max_blocks = payload.unpack1("S<", offset: 2) if alg.zero? && len == CONFIG_LENGTHS[alg]
      pos + 2 + len
    end

    def config_outside(alg, what)
      unsupported_feature "configuration #{what} for #{ALGORITHMS[alg]} ends past byte #{SB_REGION_END}"
      nil
    end

    def config_truncated(alg)
      problem "configuration record for #{ALGORITHMS[alg]} runs past the end of the volume"
      nil
    end

    # The bounds of the design's configuration table; a field outside them is
    # unsupported.
    def check_config_bounds(alg, payload)
      case alg
      when 1
        dict = payload.unpack1("L<")
        unsupported_feature "MicroLZMA dictionary #{dict} is outside 4096..8 MiB" unless (4096..(8 << 20)).cover?(dict)
        unsupported_feature "MicroLZMA format #{payload.unpack1('S<', offset: 4)} is not 0" unless payload.unpack1("S<", offset: 4).zero?
      when 2
        unsupported_feature "DEFLATE window bits #{payload.getbyte(0)} are above 15" if payload.getbyte(0) > 15
      when 3
        unsupported_feature "Zstandard format #{payload.getbyte(0)} is not 0" unless payload.getbyte(0).zero?
        unsupported_feature "Zstandard window log less 10 is #{payload.getbyte(1)}, above 10" if payload.getbyte(1) > 10
      end
    end

    def config_fields(alg, payload)
      case alg
      when 0 then "max distance #{payload.unpack1('S<')}, max pcluster blocks #{payload.unpack1('S<', offset: 2)}"
      when 1 then "dictionary #{payload.unpack1('L<')}, format #{payload.unpack1('S<', offset: 4)}"
      when 2 then "window bits #{payload.getbyte(0)}"
      else "format #{payload.getbyte(0)}, window log less 10 #{payload.getbyte(1)}"
      end
    end

    # The long-prefix table when it is a plain region. Sets @prefixes and
    # @prefixes_decoded; a table in the packed inode or the metadata box is
    # not decoded.
    def read_prefix_table
      @prefixes = []
      @prefixes_decoded = true
      return unless @sb.incompat.anybits?(INCOMPAT_XATTR_PREFIXES) && @sb.prefix_count.positive?

      unless @sb.compat.anybits?(COMPAT_PLAIN_XATTR_PFX)
        note prefix_table_note
        @prefixes_decoded = false
        return
      end
      say "long prefixes (plain table at byte #{@sb.prefix_start * 4})"
      pos = @sb.prefix_start * 4
      @prefixes = Array.new(@sb.prefix_count) do |i|
        record_len = u16(pos)
        problem "long prefix #{i} is an empty record" if record_len.zero?
        base = record_len.zero? ? nil : read(pos + 2, 1).getbyte(0)
        infix = record_len < 2 ? "".b : read(pos + 3, record_len - 1)
        pos += align(2 + record_len, 4)
        problem "long prefix #{i} has no built-in base #{base.inspect}" if base && !XATTR_PREFIXES.key?(base)
        prefix = LongPrefix.new(base: base || 0, infix: infix.b)
        say "  #{i}: #{Fmt.name(prefix.full)}"
        prefix
      end
    end

    # The note naming where a table that is no plain region lives: the
    # metadata box when the volume has one, whether or not a packed inode
    # exists (E12, e12-box-pfx-plain, e12-box-lz4-pfx-frag), else the packed
    # inode. With neither the volume is corrupt, and this also records the
    # problem.
    def prefix_table_note
      return "long-prefix table lives in the metadata box; not decoded" if @sb.incompat.anybits?(INCOMPAT_METABOX)
      return "long-prefix table lives in the packed inode; not decoded" if
        @sb.incompat.anybits?(INCOMPAT_FRAGMENTS) && @sb.packed_nid.positive?

      problem "long prefixes without a packed inode or PLAIN_XATTR_PFX"
      "long-prefix table has no place; not decoded"
    end

    # ---- inodes -------------------------------------------------------

    def nid_offset(nid)
      raise Error, "nid #{nid} lies in the metadata box (not decoded)" if nid >> 63 == 1

      off = @sb.meta_blkaddr * @sb.block_size + 32 * nid
      raise Error, "nid #{nid} at byte #{off} is outside the volume" if off >= @domain

      off
    end

    def read_inode(nid)
      off = nid_offset(nid)
      fw = u16(off)
      fixed = read(off, fw.odd? ? 64 : 32)
      if fw.odd?
        f = fixed.unpack("S<S<S<S<Q<L<L<L<L<q<L<L<")
        Inode.new(nid:, offset: off, extended: true, layout: (fw >> 1) & 7, format_word: fw, xcount: f[1], mode: f[2],
                  nlink: f[11], size: f[4], layout_word: f[5], ino: f[6], uid: f[7], gid: f[8], time: f[9],
                  nsec: f[10], reserved: fixed.byteslice(6, 2) + fixed.byteslice(48, 16))
      else
        f = fixed.unpack("S<S<S<S<L<L<L<L<S<S<")
        Inode.new(nid:, offset: off, extended: false, layout: (fw >> 1) & 7, format_word: fw, xcount: f[1], mode: f[2],
                  nlink: f[3], size: f[4], layout_word: f[6], ino: f[7], uid: f[8], gid: f[9], time: @sb.epoch + f[5],
                  nsec: @sb.fixed_nsec, reserved: fixed.byteslice(28, 4))
      end
    end

    def inode_problems(ino)
      # Bit 4 of the format word is defined under the 48-bit layout.
      reserved_mask = @sb.wide? ? ~0x1F : ~0x0F
      problem "format word #{format('0x%04x', ino.format_word)} has reserved bits set" unless (ino.format_word & reserved_mask).zero?
      problem "layout #{ino.layout} is reserved" unless LAYOUTS.key?(ino.layout)
      problem "mode type #{format('%o', ino.kind)} is not a file type" unless KINDS.key?(ino.kind)
      problem "nanoseconds #{ino.nsec} is not below 1000000000" if ino.nsec >= 1_000_000_000
      problem "time #{ino.time} overflows a signed 64-bit count" if ino.time > INT64_MAX
      problem "reserved fields are nonzero" unless ino.reserved.each_byte.all?(&:zero?)
      device_inode_problems(ino)
    end

    def device_inode_problems(ino)
      return unless [KIND_FIFO, KIND_SOCKET, KIND_CHARDEV, KIND_BLOCKDEV].include?(ino.kind)

      problem "#{KINDS[ino.kind]} has size #{ino.size} or layout #{ino.layout}" unless ino.size.zero? && ino.layout == LAYOUT_FLAT
      problem "fifo or socket with a layout word" if [KIND_FIFO, KIND_SOCKET].include?(ino.kind) && !ino.layout_word.zero?
    end

    def print_inode(ino)
      say
      say format("inode nid=%d offset=0x%x %s layout=%d(%s)", ino.nid, ino.offset,
                 ino.extended ? "extended" : "compact", ino.layout, LAYOUTS.fetch(ino.layout, "reserved"))
      say format("  format_word=0x%04x mode=%07o(%s) perm=%04o nlink=%d size=%d uid=%d gid=%d ino=%d",
                 ino.format_word, ino.mode, KINDS.fetch(ino.kind, "unknown"), ino.mode & 0o7777,
                 ino.nlink, ino.size, ino.uid, ino.gid, ino.ino)
      say format("  layout_word=0x%08x time=%s xattr_count=%d (region %d bytes)", ino.layout_word,
                 Fmt.time(ino.time, ino.nsec), ino.xcount, ino.xattr_size)
      return unless [KIND_CHARDEV, KIND_BLOCKDEV].include?(ino.kind)

      say format("  device: major=%d minor=%d", (ino.layout_word >> 8) & 0xFFF,
                 (ino.layout_word & 0xFF) | ((ino.layout_word >> 12) & 0xFFF00))
    end

    # ---- extended attributes -------------------------------------------

    def print_xattrs(ino)
      return if ino.xcount.zero?

      start = ino.offset + ino.fixed_size
      stop = ino.attrs_end
      span(start, stop - start)
      filter, shared = read(start, 12).unpack("L<C")
      say format("  xattr region [%d,%d): name filter 0x%08x, %d shared index(es)", start, stop, filter, shared)
      problem "shared indexes run past the attribute region" if start + 12 + 4 * shared > stop
      entries = shared_entries(start:, shared:, stop:)
      pos = start + 12 + 4 * shared
      while pos < stop
        entry = read_xattr_entry(pos)
        problem "attribute entry at #{pos} ends past its region" if entry.stop > stop
        say "    inline: #{entry.text}"
        entries << entry
        pos = entry.stop
      end
      check_filter(filter, entries.map(&:bit))
    end

    # Prints the shared entries an inode names and returns them.
    def shared_entries(start:, shared:, stop:)
      return [] if start + 12 + 4 * shared > stop

      Array.new(shared) do |i|
        idx = u32(start + 12 + 4 * i)
        read_xattr_entry(@sb.xattr_blkaddr * @sb.block_size + 4 * idx).tap { say "    shared index #{idx}: #{_1.text}" }
      end
    end

    def read_xattr_entry(pos)
      suffix_len, index, value_len = read(pos, 4).unpack("CCS<")
      suffix = read(pos + 4, suffix_len)
      value = read(pos + 4 + suffix_len, value_len)
      problem "ACL attribute index #{index} has a name suffix" if [2, 3].include?(index) && !suffix.empty?
      text = format("%s value=%d bytes %s", Fmt.name(xattr_prefix(index) + suffix), value_len, Fmt.hex(value))
      XattrEntry.new(text:, stop: align(pos + 4 + suffix_len + value_len, 4), bit: filter_bit(index, suffix))
    end

    # The bit of the 32-bit name filter an attribute name hashes to: XXH32
    # of the name without its built-in prefix, seeded with the prefix's
    # index; nil when a long prefix cannot be resolved.
    def filter_bit(index, suffix)
      return nil if index < 0x80 && !XATTR_PREFIXES.key?(index)
      return Xxh32.hash(suffix, seed: FILTER_SEED + index) % 32 if index < 0x80

      long = @prefixes_decoded && @prefixes[index & 0x7F]
      long ? Xxh32.hash(long.infix + suffix, seed: FILTER_SEED + long.base) % 32 : nil
    end

    # A set filter bit promises that no attribute hashes to it. A word that
    # claims a present attribute's bit is wrong; a nonzero word other than
    # the producers' complement of the present bits is only reported.
    def check_filter(filter, bits)
      return unless @sb.compat.anybits?(COMPAT_XATTR_FILTER) && @sb.filter_reserved.zero?
      return note_once("name filter not checked: a long prefix is not resolved") if bits.include?(nil)

      present = bits.reduce(0) { |mask, bit| mask | (1 << bit) }
      expected = ~present & 0xFFFFFFFF
      problem format("name filter 0x%08x claims a present attribute's bit (present 0x%08x)", filter, present) unless (filter & present).zero?
      note format("name filter 0x%08x is not the complement 0x%08x of the present bits", filter, expected) unless filter.zero? || filter == expected
    end

    def xattr_prefix(index)
      if index < 0x80
        return XATTR_PREFIXES.fetch(index) { unsupported_feature("xattr name index #{index}"); "?#{index}?" }
      end

      problem "long-prefix reference without XATTR_PREFIXES" unless @sb.incompat.anybits?(INCOMPAT_XATTR_PREFIXES)
      long = index & 0x7F
      unless @prefixes_decoded
        note_once "long-prefix references are not resolved: the table is not decoded"
        return "?long#{long}?"
      end
      @prefixes.fetch(long) { problem("long prefix #{long} does not exist"); return "?long#{long}?" }.full
    end

    # ---- data layouts ---------------------------------------------------

    def block_count(bytes) = (bytes + @sb.block_size - 1) / @sb.block_size

    def check_blocks(first, count)
      return unless (first + count) * @sb.block_size > @domain

      problem "blocks #{first}..#{first + count - 1} lie outside the volume"
    end

    def print_flat(ino)
      b = @sb.block_size
      full = ino.layout == LAYOUT_INLINE ? ino.size / b : block_count(ino.size)
      say "  data: empty" if ino.size.zero?
      # The start word is an address only where it is read; elsewhere it
      # addresses nothing and is not validated.
      if full.positive?
        check_blocks(ino.layout_word, full)
        say format("  data: blocks %d..%d (%d)", ino.layout_word, ino.layout_word + full - 1, full)
      end
      return if ino.layout == LAYOUT_FLAT

      problem "flat-inline size #{ino.size} is a nonzero multiple of the block size" if ino.size.positive? && (ino.size % b).zero?

      tail = ino.size % b
      say format("  inline tail: offset 0x%x length %d", ino.attrs_end, tail)
      problem "inline tail crosses a block boundary" if tail.positive? && (ino.attrs_end % b) + tail > b
      problem "inline tail runs past the volume" if ino.attrs_end + tail > @domain
    end

    # The chunk entries of a chunk-based inode, in file order.
    def chunk_entries(ino)
      fmt = ChunkFormat.new(ino.layout_word)
      chunk = @sb.block_size << fmt.bits
      count = block_count_of(ino.size, chunk)
      start = align(ino.attrs_end, fmt.width)
      span(start, fmt.width * count)
      Array.new(count) do |i|
        block, extra = decode_chunk(start + fmt.width * i, fmt)
        ChunkEntry.new(index: i, logical: i * chunk, length: [chunk, ino.size - i * chunk].min, block:, extra:)
      end
    end

    def block_count_of(bytes, unit) = (bytes + unit - 1) / unit

    # [block or nil for a hole, nonzero when the entry holds a device id or an
    # unusable high part].
    def decode_chunk(pos, fmt)
      unless fmt.wide_entries?
        v = u32(pos)
        return [v == 0xFFFFFFFF ? nil : v, 0]
      end

      high, device, low = read(pos, 8).unpack("S<S<L<")
      if fmt.wide_blocks?
        block = (high << 32) | low
        [block == (1 << 48) - 1 ? nil : block, device]
      else
        [low == 0xFFFFFFFF ? nil : low, device | high]
      end
    end

    def print_chunks(ino)
      fmt = ChunkFormat.new(ino.layout_word)
      say format("  chunk format 0x%x: chunkbits %d (chunk %d bytes), %d-byte entries", fmt.format16, fmt.bits,
                 @sb.block_size << fmt.bits, fmt.width)
      problem "chunk format has reserved bits set" unless fmt.reserved_bits.zero?
      unless fmt.upper_half.zero?
        note_once "chunk layout word bits 16..31 (0x12) are nonzero; mkfs.erofs 1.9.4 writes nondeterministic bytes there"
      end
      problem "chunk-based inode without CHUNKED_FILE" unless @sb.incompat.anybits?(INCOMPAT_CHUNKED_FILE)
      problem "chunk format bit 6 (wide block numbers) needs the 48-bit layout" if fmt.wide_blocks? && !@sb.wide?
      entries = chunk_entries(ino)
      say "  chunk index at 0x#{align(ino.attrs_end, fmt.width).to_s(16)}: #{entries.size} chunk(s)"
      entries.each { print_chunk(_1) }
    end

    def print_chunk(entry)
      problem "chunk #{entry.index} has a nonzero high part or device id" unless entry.extra.zero?
      where = "hole"
      if entry.block
        check_blocks(entry.block, block_count(entry.length))
        where = "block #{entry.block}"
      end
      say format("    chunk %d: logical %d length %d %s", entry.index, entry.logical, entry.length, where)
    end

    # ---- compressed layouts ----------------------------------------------

    def print_compressed(ino)
      hdr_at = align(ino.attrs_end, 8)
      header = MapHeader.new(read(hdr_at, 8))
      advice = header.advice
      say format("  map header at 0x%x: word0 0x%08x advice 0x%04x (%s) algorithms 0x%02x (head1 %s, head2 %s) cluster byte 0x%02x",
                 hdr_at, header.word0, advice, Fmt.names(ADVICE, advice), header.algorithms,
                 algo_name(header.head1_algorithm), algo_name(header.head2_algorithm), header.cluster_byte)
      unsupported_feature "reserved advice bits #{format('0x%x', advice & ~ADVICE_KNOWN)}" unless (advice & ~ADVICE_KNOWN).zero?
      if header.whole_file_fragment?
        say "  whole-file fragment: packed-inode offset #{header.whole_file_fragment_offset} (no index)"
        problem "fragment without FRAGMENTS" unless @sb.incompat.anybits?(INCOMPAT_FRAGMENTS)
        return
      end
      say "  lclusterbits #{@sb.blkszbits + header.cluster_extra_bits} (logical cluster #{1 << (@sb.blkszbits + header.cluster_extra_bits)} bytes)"
      unless header.cluster_extra_bits.zero?
        unsupported_feature "logical cluster larger than the block (L != B)"
        return
      end
      index = if ino.layout == LAYOUT_FULL_INDEX
                full_index(ino:, hdr_at:)
              else
                compact_index(ino:, hdr_at:, header:)
              end
      print_records(ino:, index:, header:)
    end

    def algo_name(id) = ALGORITHMS.fetch(id) { "unknown(#{id})" }

    # L is the block size, the only logical cluster size supported.
    def record_count(ino) = block_count(ino.size)

    def full_index(ino:, hdr_at:)
      count = record_count(ino)
      start = hdr_at + 16
      span(start, 8 * count)
      say "  full index: #{count} record(s) of 8 bytes at 0x#{start.to_s(16)}"
      records = Array.new(count) do |i|
        adv, clusterofs, d0, d1 = read(start + 8 * i, 8).unpack("S<S<S<S<")
        unsupported_feature "record #{i} has reserved advice bits #{format('0x%x', adv & RECORD_RESERVED_BITS)}" unless (adv & RECORD_RESERVED_BITS).zero?
        type = adv & 3
        if type == TYPE_NONHEAD
          big = d0.anybits?(BIG_CLUSTER_FLAG)
          NonheadRecord.new(index: i, delta0: big ? 1 : d0, delta1: d1, big_blocks: big ? d0 & BIG_CLUSTER_COUNT : nil)
        else
          HeadRecord.new(index: i, type:, clusterofs:, blkaddr: d0 | (d1 << 16), partial: adv.anybits?(0x8000))
        end
      end
      Index.new(records:, end_pos: start + 8 * count)
    end

    # Groups follow the design: 8-byte groups of 2 records until the offset
    # reaches a multiple of 32, then 32-byte groups of 16 while 16 or more
    # records remain, then 8-byte groups. Without advice 0x01 every group is
    # 8 bytes, which the design refuses.
    def compact_index(ino:, hdr_at:, header:)
      total = record_count(ino)
      two_byte = header.advice.anybits?(ADVICE_TWO_BYTE)
      unsupported_feature "compact index without advice 0x01 (8-byte groups only)" unless two_byte
      problem "compact advice 0x02 and 0x04 differ" if header.advice.anybits?(ADVICE_HEAD1_BIG) != header.advice.anybits?(ADVICE_HEAD2_BIG)
      pos = hdr_at + 8
      span(pos, 2 * total)
      say "  compact index: #{total} record(s) from 0x#{pos.to_s(16)}#{two_byte ? ' (16-record groups permitted)' : ''}"
      big = header.advice.anybits?(ADVICE_BIG)
      records = []
      while records.size < total
        wide = two_byte && (pos % 32).zero? && total - records.size >= 16
        group = compact_group(pos:, wide:, first: records.size, big:)
        records.concat(group.records)
        pos = group.end_pos
      end
      Index.new(records: records.first(total), end_pos: pos)
    end

    # L = B <= 4096 fixes the value width at 12 bits.
    def compact_group(pos:, wide:, first:, big:)
      size, count, bits = wide ? [32, 16, 14] : [8, 2, 16]
      raw = read(pos, size)
      anchor = raw.unpack1("L<", offset: size - 4)
      # The bytes before the anchor, reversed and read as hex, give the
      # little-endian packed record fields as one integer.
      packed = raw.byteslice(0, size - 4).reverse.unpack1("H*").to_i(16)
      fields = Array.new(count) do |i|
        v = (packed >> (i * bits)) & ((1 << bits) - 1)
        CompactField.new(type: (v >> 12) & 3, value: v & 0xFFF)
      end
      Index.new(records: group_records(fields:, anchor:, first:, big:), end_pos: pos + size)
    end

    # The records of one group. A head or plain record's block is its
    # group's anchor plus the blocks of the clusters before it in the group.
    def group_records(fields:, anchor:, first:, big:)
      fields.each_with_index.map do |field, i|
        index = first + i
        if field.type == TYPE_NONHEAD
          forward = i == fields.size - 1 && !field.big? ? field.value : nil
          NonheadRecord.new(index:, delta0: backward_distance(fields, i), delta1: forward, big_blocks: field.big? ? field.big_count : nil)
        else
          HeadRecord.new(index:, type: field.type, clusterofs: field.value, blkaddr: anchor + blocks_before(fields, i, big:), partial: false)
        end
      end
    end

    # A non-head record's backward distance. A count marker is one back; the
    # group's last slot holds the forward distance, so its backward distance
    # is derived from the slot before it: 1 after a head or plain record,
    # 2 after a marker, else that slot's distance plus one.
    def backward_distance(fields, i)
      field = fields[i]
      return 1 if field.big?
      return field.value unless i == fields.size - 1

      before = fields[i - 1]
      return 1 unless before.type == TYPE_NONHEAD

      before.big? ? 2 : backward_distance(fields, i - 1) + 1
    end

    # The sum the group's records before `target` add to the anchor. Without
    # big clusters it starts at one, a head or plain record adds one and a
    # non-head jumps to its head. With them it starts at zero, a marker adds
    # its count and skips the head before it, a head or plain record met
    # directly is a one-block cluster, and a non-head jumps to its marker.
    # The walk never leaves the group; a malformed jump ends it (the shape
    # rules report it).
    def blocks_before(fields, target, big:)
      sum = big ? 0 : 1
      j = target - 1
      while j >= 0
        field = fields[j]
        if field.type != TYPE_NONHEAD
          sum += 1
          j -= 1
        elsif big && field.big?
          sum += field.big_count
          j -= 2
        else
          jump = backward_distance(fields, j) - (big ? 1 : 0)
          land = j - jump
          break if jump < 1 || (land >= 0 && !jump_target?(fields[land], big:))

          j = land
        end
      end
      sum
    end

    def jump_target?(field, big:) = big ? field.type == TYPE_NONHEAD && field.big? : field.type != TYPE_NONHEAD

    def print_records(ino:, index:, header:)
      records = index.records
      lsize = @sb.block_size
      records.each do |rec|
        say "    record #{rec.index}: #{record_text(rec)}"
        next unless rec.is_a?(HeadRecord) && rec.clusterofs >= lsize

        problem "head or plain record has clusterofs #{rec.clusterofs} >= #{lsize}"
      end
      check_record_rules(records:, header:, size: ino.size)
      say "  extents (head to next head):"
      heads = records.grep(HeadRecord)
      # A head at or past `size` is the end marker of the last cluster.
      marker = heads.select { _1.index * lsize + _1.clusterofs >= ino.size }
      say "    end marker record #{marker.map(&:index).join(',')}" unless marker.empty?
      heads -= marker
      heads.each_with_index do |head, i|
        start = head.index * lsize + head.clusterofs
        following = heads[i + 1]
        stop = following ? following.index * lsize + following.clusterofs : ino.size
        blocks = cluster_blocks(records, head)
        # A tail cluster is stored in the inode or the packed inode: its
        # record's block number is no address.
        check_blocks(head.blkaddr, blocks) unless head.equal?(heads.last) && header.advice.anybits?(ADVICE_INLINE_TAIL | ADVICE_FRAGMENT_TAIL)
        say format("    logical %d..%d (%d) %s block %d count %d", start, stop, stop - start,
                   extent_algo(head, header), head.blkaddr, blocks)
      end
      problem "no head or plain record in a nonempty file" if heads.empty? && ino.size.positive?
      problem "first extent does not start at offset 0" if heads.first&.index&.zero? && !heads.first.clusterofs.zero?
      tail_notes(ino:, header:, index_end: index.end_pos)
    end

    # Rules over the whole index, for the full and the compact form alike.
    def check_record_rules(records:, header:, size:)
      check_end_marker(records, size)
      big = header.advice.anybits?(ADVICE_BIG)
      records.grep(NonheadRecord).each { check_nonhead(records, _1, big:) }
      check_markers(records, header)
      used = records.grep(HeadRecord).filter_map { head_algorithm(_1, header) }.uniq
      (used - allowed_algorithms).each do |alg|
        problem "inode algorithm #{algo_name(alg)} is absent from the superblock's algorithm mask"
      end
      problem "head-2 record without COMPR_HEAD2" if records.any? { _1.is_a?(HeadRecord) && _1.type == TYPE_HEAD2 } &&
                                                      !@sb.incompat.anybits?(INCOMPAT_COMPR_HEAD2)
      return unless used.include?(0) && !@sb.incompat.anybits?(INCOMPAT_LZ4_0PADDING)

      unsupported_feature "LZ4 cluster on a volume without LZ4_0PADDING"
    end

    # When size mod L is nonzero the last record is a head or plain record at
    # or before the size: the end marker at the size, or the last cluster's
    # own record.
    def check_end_marker(records, size)
      rest = size % @sb.block_size
      last = records.last
      return if rest.zero? || (last.is_a?(HeadRecord) && last.clusterofs <= rest)

      problem "the last record is not a head or plain record at or before the size (clusterofs #{rest})"
    end

    # A non-head's distances: backward is nonzero and reaches a head or
    # plain record (a marker's cluster-mate with big clusters), forward is
    # nonzero.
    def check_nonhead(records, rec, big:)
      problem "record #{rec.index}: backward distance is zero" if rec.delta0.zero?
      problem "record #{rec.index}: forward distance is zero" if rec.delta1&.zero?
      return if rec.delta0.zero?

      if rec.big_blocks
        problem "record #{rec.index}: count marker with count zero" if rec.big_blocks.zero?
        after_head = rec.index.positive? && records[rec.index - 1].type != TYPE_NONHEAD
        problem "record #{rec.index}: count marker is not directly after a head or plain record" unless after_head
        return
      end
      jump = rec.delta0 - (big ? 1 : 0)
      problem "record #{rec.index}: non-head without the count marker directly after a head" if big && jump.zero?
      return if jump.zero?

      land = records[rec.index - jump] if rec.index >= jump
      problem "record #{rec.index}: backward distance #{rec.delta0} passes record 0" unless land
      return unless land

      right = big ? land.is_a?(NonheadRecord) && land.big_blocks : land.is_a?(HeadRecord)
      problem "record #{rec.index}: backward distance #{rec.delta0} lands on a record of the wrong type" unless right
    end

    # Count markers: only where the cluster's algorithm permits big clusters,
    # with COMPR_CFGS, within the cluster limits, and the first non-head
    # record of such a cluster always carries one.
    def check_markers(records, header)
      records.each_cons(2) do |head, rec|
        next unless head.is_a?(HeadRecord) && rec.is_a?(NonheadRecord)

        permitted = big_permitted?(head, header)
        problem "record #{rec.index}: count marker without the advice bit of its algorithm" if rec.big_blocks && !permitted
        problem "record #{rec.index}: first non-head record of a cluster has no count marker" if !rec.big_blocks && permitted
        check_marker_count(head, rec, header) if rec.big_blocks
      end
    end

    def big_permitted?(head, header)
      bit = { TYPE_HEAD1 => ADVICE_HEAD1_BIG, TYPE_HEAD2 => ADVICE_HEAD2_BIG, TYPE_PLAIN => ADVICE_BIG }.fetch(head.type)
      header.advice.anybits?(bit)
    end

    def check_marker_count(head, rec, header)
      problem "count marker without COMPR_CFGS" unless @sb.incompat.anybits?(INCOMPAT_COMPR_CFGS)
      count = rec.big_blocks
      unsupported_feature "cluster of #{count} blocks is over 1 MiB" if count * @sb.block_size > MAX_CLUSTER_BYTES
      return unless head_algorithm(head, header) == 0 && @lz4_max_blocks&.positive? && count > @lz4_max_blocks

      problem "record #{rec.index}: count #{count} exceeds the LZ4 maximum of #{@lz4_max_blocks} blocks"
    end

    # The algorithm id of a head record, or nil for a plain record.
    def head_algorithm(head, header)
      case head.type
      when TYPE_PLAIN then nil
      when TYPE_HEAD1 then header.head1_algorithm
      else header.head2_algorithm
      end
    end

    def record_text(rec)
      case rec
      in NonheadRecord
        big = rec.big_blocks ? " big-cluster-blocks=#{rec.big_blocks}" : ""
        format("%s backward=%s forward=%s%s", RECORD_TYPES[rec.type], rec.delta0, rec.delta1.inspect, big)
      in HeadRecord
        format("%s clusterofs=%d block=%d%s", RECORD_TYPES[rec.type], rec.clusterofs, rec.blkaddr,
               rec.partial ? " partial-reference" : "")
      end
    end

    def extent_algo(head, header)
      alg = head_algorithm(head, header)
      alg ? algo_name(alg) : "plain"
    end

    def cluster_blocks(records, head)
      following = records[head.index + 1]
      following.is_a?(NonheadRecord) && following.big_blocks ? following.big_blocks : 1
    end

    # The compressed inline tail lies after the index, inside one block. (The
    # design says the block holding the inode's start; mkfs.erofs 1.9.4 puts
    # the tail in the next block when the inode starts near a block's end,
    # e8-lz4-ztail, so only crossing a block boundary is reported.)
    def tail_notes(ino:, header:, index_end:)
      advice = header.advice
      if advice.anybits?(ADVICE_INLINE_TAIL)
        size = header.tail_size
        say format("  compressed inline tail: offset 0x%x size %d", index_end, size)
        problem "inline tail size is zero or crosses a block" if size.zero? || (index_end % @sb.block_size) + size > @sb.block_size
        problem "inline tail without ZTAILPACKING" unless @sb.incompat.anybits?(INCOMPAT_ZTAILPACKING)
      end
      return unless advice.anybits?(ADVICE_FRAGMENT_TAIL)

      say "  last cluster is a packed fragment at offset #{header.word0}"
      problem "fragment without FRAGMENTS" unless @sb.incompat.anybits?(INCOMPAT_FRAGMENTS)
    end

    # ---- directories -------------------------------------------------------

    # The runs of a directory's data, or nil when its layout is compressed.
    def directory_segments(ino)
      b = @sb.block_size
      case ino.layout
      when LAYOUT_FLAT
        [Segment.new(pos: ino.layout_word * b, len: ino.size)]
      when LAYOUT_INLINE
        full = ino.size / b * b
        [Segment.new(pos: ino.layout_word * b, len: full), Segment.new(pos: ino.attrs_end, len: ino.size - full)]
          .reject { _1.len.zero? }
      when LAYOUT_CHUNK
        chunk_entries(ino).map { Segment.new(pos: _1.block && _1.block * b, len: _1.length) }
      end
    end

    # The directory's blocks, read one at a time. Every segment but the last
    # is a whole number of blocks.
    def directory_blocks(segments)
      b = @sb.block_size
      Enumerator.new do |yielder|
        segments.each do |seg|
          (0...seg.len).step(b) do |off|
            len = [b, seg.len - off].min
            yielder << (seg.pos ? read(seg.pos + off, len) : "\0".b * len)
          end
        end
      end
    end

    def parse_block(block)
      first = block.unpack1("@8S<")
      if first.zero? || (first % 12).nonzero? || first >= block.bytesize
        problem "directory block's first name offset #{first} is not a positive multiple of 12 below #{block.bytesize}"
        return []
      end

      raw = Array.new(first / 12) { block.unpack("Q<S<CC", offset: 12 * _1) }
      raw.each_with_index.map do |(nid, off, type, _), i|
        stop = i + 1 < raw.size ? raw[i + 1][1] : (block.index("\0".b, off) || block.bytesize)
        problem "name offsets do not increase or run past the block" if off < first || stop < off || stop > block.bytesize
        Dirent.new(nid:, type:, name: block.byteslice(off, [stop - off, 0].max) || "".b)
      end
    end

    # Prints a directory and returns its entries (empty for a layout that is
    # not decoded).
    def print_directory(ino)
      unless ino.size.positive? && ino.size <= MAX_DIRECTORY_BYTES
        problem "directory size #{ino.size} is empty or over #{MAX_DIRECTORY_BYTES}"
        return []
      end
      segments = directory_segments(ino)
      unless segments
        note "compressed directory data is not decoded"
        return []
      end

      entries = directory_blocks(segments).flat_map { parse_block(_1) }
      say "  entries: #{entries.size}"
      previous = nil
      entries.each do |entry|
        say format("    nid=%d type=%d(%s) name=%s", entry.nid, entry.type, DIRENT_TYPES.fetch(entry.type, "invalid"),
                   Fmt.name(entry.name))
        problem "entry name #{Fmt.name(entry.name)} is empty, over #{MAX_NAME} bytes or holds / or NUL" if
          entry.name.empty? || entry.name.bytesize > MAX_NAME || entry.name.include?("/") || entry.name.include?("\0")
        problem "entry type #{entry.type} is invalid" unless DIRENT_TYPES.key?(entry.type)
        problem "names do not increase strictly at #{Fmt.name(entry.name)}" if previous && (previous <=> entry.name) >= 0
        previous = entry.name
      end
      entries
    end

    # The parent rule: `.` names the directory, `..` its parent (the root's
    # own parent is the root), and no other entry names either.
    def check_parent_rule(ino:, entries:, parent:)
      dot = entries.find { _1.name == "." }
      dotdot = entries.find { _1.name == ".." }
      problem "directory has no . entry" unless dot
      problem "directory has no .. entry" unless dotdot
      problem "'.' names nid #{dot.nid}, not the directory itself (#{ino.nid})" if dot && dot.nid != ino.nid
      problem "'..' names nid #{dotdot.nid}, not the parent (#{parent})" if dotdot && dotdot.nid != parent
      children(entries).each do |child|
        problem "entry #{Fmt.name(child.name)} names the directory or its parent" if [ino.nid, parent].include?(child.nid)
      end
    end

    def children(entries) = entries.reject { [".", ".."].include?(_1.name) }

    # ---- symbolic links ----------------------------------------------------

    def print_symlink(ino)
      return unless ino.kind == KIND_SYMLINK

      if ino.size.zero?
        problem "symlink target is empty"
        return
      end
      if ino.size > SYMLINK_MAX
        unsupported_feature "symlink target of #{ino.size} bytes is over #{SYMLINK_MAX}"
        return
      end
      target = symlink_target(ino)
      unless target
        note "symlink target in layout #{ino.layout} is not decoded"
        return
      end
      say "  target: #{Fmt.name(target)}"
      problem "symlink target holds a NUL byte" if target.include?("\0")
    end

    # The target bytes of a flat or inline symlink, else nil.
    def symlink_target(ino)
      case ino.layout
      when LAYOUT_FLAT
        read(ino.layout_word * @sb.block_size, ino.size)
      when LAYOUT_INLINE
        full = ino.size / @sb.block_size * @sb.block_size
        (full.zero? ? "".b : read(ino.layout_word * @sb.block_size, full)) + read(ino.attrs_end, ino.size - full)
      end
    end

    # ---- traversal ----------------------------------------------------------

    def walk(root)
      say
      say "tree from root nid #{root}"
      pending = [Pending.new(nid: root, expected_type: nil, parent: root)]
      until pending.empty?
        item = pending.pop
        if @seen.add?(item.nid)
          pending.concat(dump_inode(item))
        else
          problem "directory nid #{item.nid} has a second name" if @directories.include?(item.nid)
          say "(nid #{item.nid} already shown)"
        end
      end
    rescue Error => e
      problem e.message
    end

    # Prints one inode; returns its children as Pending items, last first so
    # that the traversal prints them in order.
    def dump_inode(item)
      ino = read_inode(item.nid)
      print_inode(ino)
      inode_problems(ino)
      expected = item.expected_type
      problem "directory entry type #{expected} disagrees with the mode" if expected&.positive? && expected != KIND_TO_DIRENT[ino.kind]
      print_xattrs(ino)
      print_layout(ino)
      return [] unless ino.kind == KIND_DIR

      @directories.add(ino.nid)
      entries = print_directory(ino)
      check_parent_rule(ino:, entries:, parent: item.parent) unless entries.empty?
      children(entries).reverse.map { Pending.new(nid: _1.nid, expected_type: _1.type, parent: ino.nid) }
    rescue StandardError => e
      # Per-inode boundary: a hostile inode is reported and the walk goes on.
      problem "nid #{item.nid}: #{e.is_a?(Error) ? '' : "#{e.class}: "}#{e.message}"
      []
    end

    def print_layout(ino)
      case ino.layout
      when LAYOUT_FLAT, LAYOUT_INLINE then print_flat(ino)
      when LAYOUT_CHUNK then print_chunks(ino)
      when LAYOUT_FULL_INDEX, LAYOUT_COMPACT_INDEX then print_compressed(ino)
      end
      print_symlink(ino)
    end
  end

  # Exit status: 0 clean, 1 an error or a `problem:` line, 2 an unsupported
  # feature or a tree not decoded (the image is valid as far as it was read).
  def self.main(argv)
    tree = true
    parser = OptionParser.new("usage: erofsdump.rb [--no-tree] IMAGE") { _1.on("--[no-]tree") { |v| tree = v } }
    paths = parser.parse(argv)
    raise Error, parser.banner unless paths.size == 1

    File.open(paths.first, "rb") do |io|
      dumper = Dumper.new(Volume.new(io), $stdout)
      outcome = dumper.dump(tree:)
      warn "erofsdump: the tree is not decoded (unsupported volume, 48-bit layout or metadata box)" if outcome == :tree_not_decoded
      next 1 unless dumper.problems.zero?

      outcome == :tree_not_decoded || dumper.unsupported.positive? ? 2 : 0
    end
  rescue Unsupported => e
    warn "erofsdump: unsupported: #{e.message}"
    2
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "erofsdump: #{e.message}"
    1
  end
end

exit ErofsDump.main(ARGV) if $PROGRAM_NAME == __FILE__
