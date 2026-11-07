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
# A `problem:` line also makes the exit status 1.
#
# Where the design marks a rule provisional the dumper implements the design's
# default and prints a `note:`.
require "optparse"

module ErofsDump
  class Error < StandardError; end

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
  SB_COMPAT_CHKSUM = 0x01
  SB_COMPAT_PLAIN_XATTR_PFX = 0x10
  INCOMPAT_COMPR_CFGS = 0x002
  INCOMPAT_FRAGMENTS = 0x020
  INCOMPAT_XATTR_PREFIXES = 0x040
  INCOMPAT_48BIT = 0x080
  INCOMPAT_METABOX = 0x100

  LAYOUTS = {
    0 => "flat", 1 => "compressed-full-index", 2 => "flat-inline",
    3 => "compressed-compact-index", 4 => "chunk-based"
  }.freeze
  KINDS = {
    0o100000 => "file", 0o040000 => "dir", 0o120000 => "symlink",
    0o010000 => "fifo", 0o020000 => "chardev", 0o060000 => "blockdev",
    0o140000 => "socket"
  }.freeze
  DIRENT_TYPES = {
    0 => "unknown", 1 => "file", 2 => "dir", 3 => "chardev", 4 => "blockdev",
    5 => "fifo", 6 => "socket", 7 => "symlink"
  }.freeze
  # Directory entry file type for each inode kind.
  KIND_TO_DIRENT = { 0o100000 => 1, 0o040000 => 2, 0o020000 => 3, 0o060000 => 4,
                     0o010000 => 5, 0o140000 => 6, 0o120000 => 7 }.freeze
  ALGORITHMS = { 0 => "LZ4", 1 => "MicroLZMA", 2 => "DEFLATE", 3 => "Zstandard" }.freeze
  RECORD_TYPES = { 0 => "plain", 1 => "head1", 2 => "nonhead", 3 => "head2" }.freeze
  XATTR_PREFIXES = {
    1 => "user.", 2 => "system.posix_acl_access", 3 => "system.posix_acl_default",
    4 => "trusted.", 6 => "security."
  }.freeze
  ADVICE = {
    0x01 => "compact-2byte/48bit-extents", 0x02 => "head1-big", 0x04 => "head2-big",
    0x08 => "inline-tail", 0x10 => "interlaced", 0x20 => "fragment-tail"
  }.freeze

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

  Inode = Data.define(
    :nid, :offset, :extended, :layout, :format_word, :xcount, :mode, :nlink, :size,
    :layout_word, :ino, :uid, :gid, :time, :nsec, :reserved
  ) do
    def fixed_size = extended ? 64 : 32
    def xattr_size = xcount.zero? ? 0 : 12 + 4 * (xcount - 1)
    def attrs_end = offset + fixed_size + xattr_size
    def kind = mode & 0o170000
  end

  Record = Data.define(:index, :type, :partial, :value, :blkaddr, :delta0, :delta1, :big)

  # CRC-32C (Castagnoli polynomial, reflected), without the final inversion.
  module Crc32c
    TABLE = Array.new(256) do |n|
      8.times.reduce(n) { |c, _| c.odd? ? (c >> 1) ^ 0x82F63B78 : c >> 1 }
    end.freeze

    module_function

    def update(seed, bytes) = bytes.each_byte.reduce(seed) { |c, b| TABLE[(c ^ b) & 0xFF] ^ (c >> 8) }
  end

  # Bounds-checked access to the image.
  class Volume
    attr_reader :size

    def initialize(io)
      @io = io
      @size = io.size
    end

    def read(off, len)
      raise Error, "read of #{len} bytes at #{off} is outside the image (#{@size} bytes)" if
        off.negative? || len.negative? || off + len > @size

      return "".b if len.zero?

      data = @io.pread(len, off)
      raise Error, "short read of #{len} bytes at #{off}" if data.bytesize != len

      data
    end

    # Raises unless `len` bytes at `off` lie inside the image; reads nothing.
    def check_span(off, len)
      raise Error, "#{len} bytes at #{off} are outside the image (#{@size} bytes)" if off.negative? || len.negative? || off + len > @size
    end

    def u16(off) = read(off, 2).unpack1("S<")
    def u32(off) = read(off, 4).unpack1("L<")
    def u64(off) = read(off, 8).unpack1("Q<")
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

  class Dumper
    def initialize(volume, out)
      @vol = volume
      @out = out
      @problems = 0
      @seen = {}
    end

    attr_reader :problems

    def dump(tree: true)
      @sb = read_superblock
      print_superblock
      verify_checksum
      read_configs
      @prefixes = read_prefix_table
      return unless tree

      walk(@sb.root_nid)
    end

    private

    def say(text = "") = @out.puts(text)
    def note(text) = say("note: #{text}")

    def problem(text)
      @problems += 1
      say("problem: #{text}")
      nil
    end

    # ---- superblock -------------------------------------------------

    def read_superblock
      base = @vol.read(SB_OFFSET, SB_BASE_SIZE)
      fields = SB_FIELDS.to_h { |name, off, fmt| [name, base.unpack1(fmt, offset: off)] }
      raise Error, format("not EROFS: magic 0x%08x at offset 1024", fields[:magic]) if fields[:magic] != MAGIC

      full = @vol.read(SB_OFFSET, SB_BASE_SIZE + 16 * fields[:ext_slots]).b
      sb = Superblock.new(**fields, metabox_nid: full.bytesize >= 0x88 ? full.unpack1("Q<", offset: 0x80) : nil,
                                    reserved_88: full.bytesize >= 0x90 ? full.unpack1("Q<", offset: 0x88) : nil)
      raise Error, "blkszbits #{sb.blkszbits} is outside 9..12" unless (9..12).cover?(sb.blkszbits)
      raise Error, "superblock of #{sb.size} bytes ends past byte #{SB_REGION_END}" if SB_OFFSET + sb.size > SB_REGION_END

      @domain = sb.blocks.zero? ? @vol.size : sb.blocks * sb.block_size
      @slots = full.byteslice(SB_BASE_SIZE, 16 * sb.ext_slots)
      sb
    end

    def print_superblock
      sb = @sb
      say "superblock (offset 1024, #{sb.size} bytes)"
      fields = [
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
      fields.each { |name, value| say format("  %-42s %s", name, value) }
      note "build time at 0x6C is provisional in the design: reported, not interpreted"
      superblock_problems
    end

    def superblock_problems
      sb = @sb
      problem "fixed nanoseconds #{sb.fixed_nsec} is not below 1000000000" if sb.fixed_nsec >= 1_000_000_000
      problem "dirblkbits #{sb.dirblkbits} is nonzero" unless sb.dirblkbits.zero?
      problem "extra device count #{sb.extra_devices} is nonzero (refused by the design)" unless sb.extra_devices.zero?
      problem "unknown incompatible bits #{format('0x%x', sb.incompat & ~INCOMPAT.keys.reduce(0, :|))}" unless
        (sb.incompat & ~INCOMPAT.keys.reduce(0, :|)).zero?
      note "unknown compatible bits #{format('0x%x', sb.compat & ~COMPAT.keys.reduce(0, :|))}" unless
        (sb.compat & ~COMPAT.keys.reduce(0, :|)).zero?
      note "reserved bytes are nonzero" unless sb.reserved_6a.zero? && sb.reserved_78.zero? && sb.reserved_88.to_i.zero?
      note "48-bit layout and metadata-box inodes are not decoded by this dumper" if
        sb.incompat.anybits?(INCOMPAT_48BIT | INCOMPAT_METABOX)
      note "image is #{@vol.size} bytes; the superblock declares #{@domain}" if @domain != @vol.size
      problem "the image is shorter than the declared #{@domain} bytes" if @domain > @vol.size
    end

    # The design's covered range is [1024, B) when B > 1024, else
    # [1024, 1024 + B). Seed all ones, no final inversion. Every variant that
    # matches is listed, since the range and the inversion are provisional.
    def verify_checksum
      unless @sb.compat.anybits?(SB_COMPAT_CHKSUM)
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

    def read_configs
      @configs = {}
      return unless @sb.incompat.anybits?(INCOMPAT_COMPR_CFGS)

      mask = @sb.u1
      say "compression configuration (algorithm mask #{format('0x%x', mask)})"
      problem "algorithm mask has bits above 3" if mask > 0xF
      pos = SB_OFFSET + @sb.size
      (0..3).select { mask.anybits?(1 << _1) }.each { |alg| pos = read_config(alg, pos) }
    end

    def read_config(alg, pos)
      pos = (pos + 3) & ~3
      len = @vol.u16(pos)
      payload = @vol.read(pos + 2, len)
      problem "configuration record of #{len} bytes for #{ALGORITHMS[alg]}, expected #{CONFIG_LENGTHS[alg]}" if len != CONFIG_LENGTHS[alg]
      problem "configuration record ends past byte #{SB_REGION_END}" if pos + 2 + len > SB_REGION_END
      @configs[alg] = payload
      say format("  %-10s offset %d length %d: %s", ALGORITHMS[alg], pos, len, config_fields(alg, payload))
      pos + 2 + len
    end

    def config_fields(alg, payload)
      case alg
      when 0 then "max distance #{payload.unpack1('S<')}, max pcluster blocks #{payload.unpack1('S<', offset: 2)}"
      when 1 then "dictionary #{payload.unpack1('L<')}, format #{payload.unpack1('S<', offset: 4)}"
      when 2 then "window bits #{payload.getbyte(0)}"
      else "format #{payload.getbyte(0)}, window log less 10 #{payload.getbyte(1)}"
      end
    end

    # The long-prefix table, when it is a plain region; otherwise empty.
    def read_prefix_table
      return [] unless @sb.incompat.anybits?(INCOMPAT_XATTR_PREFIXES) && @sb.prefix_count.positive?

      unless @sb.compat.anybits?(SB_COMPAT_PLAIN_XATTR_PFX)
        note "long-prefix table lives in the packed inode; not decoded"
        return []
      end
      say "long prefixes (plain table at byte #{@sb.prefix_start * 4})"
      pos = @sb.prefix_start * 4
      Array.new(@sb.prefix_count) do |i|
        len = @vol.u16(pos)
        base = @vol.read(pos + 2, 1).getbyte(0)
        infix = @vol.read(pos + 3, [len - 1, 0].max)
        pos += (2 + len + 3) & ~3
        problem "long prefix #{i} has no built-in base #{base}" unless XATTR_PREFIXES.key?(base)
        full = "#{XATTR_PREFIXES[base]}#{infix.b}"
        say "  #{i}: #{Fmt.name(full)}"
        full
      end
    end

    # ---- inodes -------------------------------------------------------

    def nid_offset(nid)
      raise Error, "nid #{nid} lies in the metadata box (not decoded)" if nid >> 63 == 1

      off = @sb.meta_blkaddr * @sb.block_size + 32 * nid
      raise Error, "nid #{nid} at byte #{off} is outside the image" if off >= @domain

      off
    end

    def read_inode(nid)
      off = nid_offset(nid)
      fw = @vol.u16(off)
      fixed = @vol.read(off, fw.odd? ? 64 : 32)
      if fw.odd?
        f = fixed.unpack("S<S<S<S<Q<L<L<L<L<q<L<L<a16")
        Inode.new(nid:, offset: off, extended: true, layout: (fw >> 1) & 7, format_word: fw, xcount: f[1], mode: f[2],
                  nlink: f[11], size: f[4], layout_word: f[5], ino: f[6], uid: f[7], gid: f[8], time: f[9],
                  nsec: f[10], reserved: [f[3], f[12]])
      else
        f = fixed.unpack("S<S<S<S<L<L<L<L<S<S<L<")
        Inode.new(nid:, offset: off, extended: false, layout: (fw >> 1) & 7, format_word: fw, xcount: f[1], mode: f[2],
                  nlink: f[3], size: f[4], layout_word: f[6], ino: f[7], uid: f[8], gid: f[9], time: @sb.epoch,
                  nsec: @sb.fixed_nsec, reserved: [f[5], f[10]])
      end
    end

    def inode_problems(ino)
      reserved_fw = ino.format_word & ~0x0F
      problem "format word #{format('0x%04x', ino.format_word)} has reserved bits set" unless reserved_fw.zero?
      problem "layout #{ino.layout} is reserved" unless LAYOUTS.key?(ino.layout)
      problem "mode type #{format('%o', ino.kind)} is not a file type" unless KINDS.key?(ino.kind)
      problem "nanoseconds #{ino.nsec} is not below 1000000000" if ino.nsec >= 1_000_000_000
      problem "reserved fields are nonzero" unless ino.reserved.flatten.all? { _1 == 0 || _1 == "\0" * 16 }
      return if ino.layout == 4 && ino.kind == 0o100000

      case ino.kind
      when 0o010000, 0o140000, 0o020000, 0o060000
        problem "#{KINDS[ino.kind]} has size #{ino.size} or layout #{ino.layout}" unless ino.size.zero? && ino.layout.zero?
        problem "fifo or socket with a layout word" if [0o010000, 0o140000].include?(ino.kind) && !ino.layout_word.zero?
      end
    end

    def print_inode(ino)
      say
      say format("inode nid=%d offset=0x%x %s layout=%d(%s)", ino.nid, ino.offset,
                 ino.extended ? "extended" : "compact", ino.layout, LAYOUTS.fetch(ino.layout, "reserved"))
      say format("  format_word=0x%04x mode=%07o(%s) perm=%04o nlink=%d size=%d uid=%d gid=%d ino=%d",
                 ino.format_word, ino.mode, KINDS.fetch(ino.kind, "unknown"), ino.mode & 0o7777,
                 ino.nlink, ino.size, ino.uid, ino.gid, ino.ino)
      say format("  layout_word=0x%08x time=%s%s xattr_count=%d (region %d bytes)", ino.layout_word,
                 Fmt.time(ino.time, ino.nsec), ino.extended ? "" : " (superblock epoch)", ino.xcount, ino.xattr_size)
      say format("  device: major=%d minor=%d", (ino.layout_word >> 8) & 0xFFF,
                 (ino.layout_word & 0xFF) | ((ino.layout_word >> 12) & 0xFFF00)) if [0o020000, 0o060000].include?(ino.kind)
    end

    # ---- extended attributes -------------------------------------------

    def print_xattrs(ino)
      return if ino.xcount.zero?

      start = ino.offset + ino.fixed_size
      stop = ino.attrs_end
      hdr = @vol.read(start, 12)
      filter, shared = hdr.unpack("L<C")
      say format("  xattr region [%d,%d): name filter 0x%08x, %d shared index(es)", start, stop, filter, shared)
      problem "shared indexes run past the attribute region" if start + 12 + 4 * shared > stop
      shared.times do |i|
        idx = @vol.u32(start + 12 + 4 * i)
        say "    shared index #{idx}: #{xattr_entry(@sb.xattr_blkaddr * @sb.block_size + 4 * idx, nil)}"
      end
      pos = start + 12 + 4 * shared
      while pos < stop
        text, pos = xattr_entry_at(pos, stop)
        say "    inline: #{text}"
      end
    end

    def xattr_entry(pos, limit) = xattr_entry_at(pos, limit).first

    def xattr_entry_at(pos, limit)
      suffix_len, index, value_len = @vol.read(pos, 4).unpack("CCS<")
      suffix = @vol.read(pos + 4, suffix_len)
      value = @vol.read(pos + 4 + suffix_len, value_len)
      stop = (pos + 4 + suffix_len + value_len + 3) & ~3
      problem "attribute entry at #{pos} ends past its region" if limit && stop > limit
      [format("%s value=%d bytes %s", Fmt.name(xattr_prefix(index) + suffix), value_len, Fmt.hex(value)), stop]
    end

    def xattr_prefix(index)
      return XATTR_PREFIXES.fetch(index) { problem("xattr name index #{index} is unknown"); "?#{index}?" } if index < 0x80

      @prefixes.fetch(index & 0x7F) { problem("long prefix #{index & 0x7F} does not exist"); "?long#{index & 0x7F}?" }
    end

    # ---- data layouts ---------------------------------------------------

    def block_range_text(first, count)
      problem "blocks #{first}..#{first + count - 1} lie outside the image" if (first + count) * @sb.block_size > @domain
      format("blocks %d..%d (%d)", first, first + count - 1, count)
    end

    def print_flat(ino)
      b = @sb.block_size
      full = ino.layout == 2 ? ino.size / b : (ino.size + b - 1) / b
      if ino.size.zero?
        say "  data: empty"
      elsif full.zero?
        note "start block is not interpreted: the whole content is the inline tail" if ino.layout_word == 0xFFFFFFFF
      elsif ino.layout_word == 0xFFFFFFFF
        problem "null flat address (start block all ones) under the design's default (E3)"
      end
      say "  data: #{block_range_text(ino.layout_word, full)}" if full.positive? && ino.layout_word != 0xFFFFFFFF
      return if ino.layout.zero?

      tail = ino.size % b
      say format("  inline tail: offset 0x%x length %d", ino.attrs_end, tail)
      problem "inline tail crosses a block boundary" if tail.positive? && (ino.attrs_end % b) + tail > b
      problem "inline tail runs past the image" if ino.attrs_end + tail > @domain
    end

    # Chunk entries, as [block or nil for a hole] in file order.
    def print_chunks(ino)
      fmt = ino.layout_word
      bits = fmt & 0x1F
      wide = fmt.anybits?(0x20)
      say format("  chunk format 0x%x: chunkbits %d (chunk %d bytes), %d-byte entries", fmt, bits,
                 bits > 47 ? 0 : @sb.block_size << bits, wide ? 8 : 4)
      problem "chunk format has reserved bits set" unless (fmt & ~0x7F).zero?
      problem "chunk-based inode without CHUNKED_FILE" unless @sb.incompat.anybits?(0x004)
      problem "chunk format bit 6 (wide block numbers) needs the 48-bit layout" if fmt.anybits?(0x40) && !@sb.wide?
      return say("  chunk size exceeds 2^47") if (@sb.blkszbits + bits) > 47

      chunk = @sb.block_size << bits
      count = (ino.size + chunk - 1) / chunk
      width = wide ? 8 : 4
      start = (ino.attrs_end + width - 1) / width * width
      @vol.check_span(start, width * count)
      say "  chunk index at 0x#{start.to_s(16)}: #{count} chunk(s)"
      count.times do |i|
        block, extra = chunk_entry(start + width * i, wide, fmt.anybits?(0x40))
        problem "chunk #{i} has a nonzero high part or device id" unless extra.zero?
        say(block ? format("    chunk %d: logical %d length %d block %d", i, i * chunk, [chunk, ino.size - i * chunk].min, block) :
                    format("    chunk %d: logical %d length %d hole", i, i * chunk, [chunk, ino.size - i * chunk].min))
      end
    end

    # One chunk index entry: [block or nil for a hole, nonzero when the entry
    # holds a device id or an unusable high part].
    def chunk_entry(pos, wide, bit6)
      if wide
        hi, dev, lo = @vol.read(pos, 8).unpack("S<S<L<")
        block = bit6 ? (hi << 32) | lo : lo
        [block == (bit6 ? (1 << 48) - 1 : 0xFFFFFFFF) ? nil : block, dev | (bit6 ? 0 : hi)]
      else
        v = @vol.u32(pos)
        [v == 0xFFFFFFFF ? nil : v, 0]
      end
    end

    # ---- compressed layouts ----------------------------------------------

    def print_compressed(ino)
      hdr_at = (ino.attrs_end + 7) & ~7
      hdr = @vol.read(hdr_at, 8)
      advice, algs, cbits = hdr.unpack("@4S<CC")
      say format("  map header at 0x%x: word0 0x%08x advice 0x%04x (%s) algorithms 0x%02x (head1 %s, head2 %s) cluster byte 0x%02x",
                 hdr_at, hdr.unpack1("L<"), advice, Fmt.names(ADVICE, advice), algs, algo_name(algs & 0xF), algo_name(algs >> 4), cbits)
      problem "unknown advice bits #{format('0x%x', advice & ~ADVICE.keys.reduce(0, :|))}" unless (advice & ~0x3F).zero?
      if cbits.anybits?(0x80)
        say "  whole-file fragment: packed-inode offset #{hdr.unpack1('Q<') & ~(1 << 63)} (no index)"
        problem "fragment without FRAGMENTS" unless @sb.incompat.anybits?(INCOMPAT_FRAGMENTS)
        return
      end
      lbits = @sb.blkszbits + (cbits & 0xF)
      say "  lclusterbits #{lbits} (logical cluster #{1 << lbits} bytes)"
      return problem("logical cluster beyond 2^20 bytes") if lbits > 20

      records = ino.layout == 1 ? full_records(ino, hdr_at, lbits) : compact_records(ino, hdr_at, lbits, advice)
      print_records(ino, records, lbits, advice, algs, hdr)
    end

    def algo_name(id) = ALGORITHMS.fetch(id) { "unknown(#{id})" }

    def record_count(ino, lbits) = (ino.size + (1 << lbits) - 1) >> lbits

    def full_records(ino, hdr_at, lbits)
      start = hdr_at + 16
      @vol.check_span(start, 8 * record_count(ino, lbits))
      say "  full index: #{record_count(ino, lbits)} record(s) of 8 bytes at 0x#{start.to_s(16)}"
      Array.new(record_count(ino, lbits)) do |i|
        adv, cofs, d0, d1 = @vol.read(start + 8 * i, 8).unpack("S<S<S<S<")
        type = adv & 3
        head = type != 2
        big = !head && d0.anybits?(0x800)
        Record.new(index: i, type:, partial: adv.anybits?(0x8000), value: cofs,
                   blkaddr: head ? d0 | (d1 << 16) : nil, delta0: head ? nil : (big ? 1 : d0),
                   delta1: head ? nil : d1, big: big ? d0 & 0x7FF : nil)
      end
    end

    # Groups follow the design: 8-byte groups of 2 records until the offset
    # reaches a multiple of 32, then 32-byte groups of 16 while 16 or more
    # records remain, then 8-byte groups. Provisional per the design (E8).
    # A group's first head is at its anchor block when big physical clusters
    # are permitted (advice 0x02 or 0x04) and one block after it otherwise; the
    # design states only the first rule.
    def compact_records(ino, hdr_at, lbits, advice)
      total = record_count(ino, lbits)
      two_byte = advice.anybits?(0x01)
      pos = hdr_at + 8
      @vol.check_span(pos, 2 * total)
      say "  compact index: #{total} record(s) from 0x#{pos.to_s(16)}#{two_byte ? ' (16-record groups permitted)' : ''}"
      note "compact group packing is provisional in the design (E8)"
      records = []
      while records.size < total
        wide = two_byte && (pos % 32).zero? && total - records.size >= 16
        group, pos = compact_group(pos, wide, lbits, records.size, advice.anybits?(0x06) ? 0 : 1)
        records.concat(group)
      end
      @index_end = pos
      records.first(total)
    end

    def compact_group(pos, wide, lbits, first, bias)
      size, count, bits = wide ? [32, 16, 14] : [8, 2, 16]
      lobits = [lbits, 12].max
      problem "logical cluster too large for #{size}-byte groups" if lbits > (wide ? 12 : 14)
      raw = @vol.read(pos, size)
      anchor = raw.unpack1("L<", offset: size - 4)
      packed = raw.byteslice(0, size - 4).unpack1("H*").scan(/../).reverse.join.to_i(16)
      fields = Array.new(count) { (packed >> (_1 * bits)) & ((1 << bits) - 1) }
      [group_records(fields, lobits, anchor + bias, first), pos + size]
    end

    def group_records(fields, lobits, base, first)
      mask = (1 << lobits) - 1
      raw = fields.map { [(_1 >> lobits) & 3, _1 & mask] }
      next_block = base
      raw.each_with_index.map do |(type, value), i|
        last = i == raw.size - 1
        if type == 2
          big = value.anybits?(0x800)
          back = last ? (raw[i - 1][0] == 2 ? raw[i - 1][1] + 1 : 1) : value
          Record.new(index: first + i, type:, partial: false, value: nil, blkaddr: nil,
                     delta0: big ? 1 : back, delta1: last ? value : nil, big: big ? value & 0x7FF : nil)
        else
          blk = next_block
          follow = raw[i + 1]
          next_block += follow && follow[0] == 2 && follow[1].anybits?(0x800) ? follow[1] & 0x7FF : 1
          Record.new(index: first + i, type:, partial: false, value:, blkaddr: blk, delta0: nil, delta1: nil, big: nil)
        end
      end
    end

    def print_records(ino, records, lbits, advice, algs, hdr)
      lsize = 1 << lbits
      records.each do |r|
        say "    record #{r.index}: #{record_text(r)}"
        problem "head or plain record has clusterofs #{r.value} >= #{lsize}" if r.type != 2 && r.value >= lsize
      end
      say "  extents (head to next head):"
      heads = records.select { _1.type != 2 }
      # A head at or past `size` is the end marker of the last cluster.
      marker = heads.select { _1.index * lsize + _1.value >= ino.size }
      say "    end marker record #{marker.map(&:index).join(',')}" unless marker.empty?
      heads -= marker
      heads.each_with_index do |h, i|
        start = h.index * lsize + h.value
        stop = heads[i + 1] ? heads[i + 1].index * lsize + heads[i + 1].value : ino.size
        say format("    logical %d..%d (%d) %s block %d count %d", start, stop, stop - start, extent_algo(h, algs), h.blkaddr, cluster_blocks(records, h))
      end
      problem "no head or plain record in a nonempty file" if heads.empty? && ino.size.positive?
      problem "first extent does not start at offset 0" if heads.first && heads.first.index.zero? && !heads.first.value.zero?
      tail_notes(ino, advice, records, hdr)
    end

    def record_text(rec)
      base = RECORD_TYPES[rec.type]
      if rec.type == 2
        format("%s backward=%s forward=%s%s", base, rec.delta0, rec.delta1.inspect, rec.big ? " big-cluster-blocks=#{rec.big}" : "")
      else
        format("%s clusterofs=%d block=%d%s", base, rec.value, rec.blkaddr, rec.partial ? " partial-reference" : "")
      end
    end

    def extent_algo(head, algs)
      case head.type
      when 0 then "plain"
      when 1 then algo_name(algs & 0xF)
      else algo_name(algs >> 4)
      end
    end

    def cluster_blocks(records, head)
      nxt = records[head.index + 1]
      nxt && nxt.type == 2 && nxt.big ? nxt.big : 1
    end

    def tail_notes(ino, advice, records, hdr)
      idx_end = ino.layout == 1 ? (((ino.attrs_end + 7) & ~7) + 16 + 8 * records.size) : @index_end
      if advice.anybits?(0x08)
        size = hdr.unpack1("@2S<")
        say format("  compressed inline tail: offset 0x%x size %d", idx_end, size)
        problem "inline tail size is zero or crosses a block" if size.zero? || (idx_end % @sb.block_size) + size > @sb.block_size
        problem "inline tail without ZTAILPACKING" unless @sb.incompat.anybits?(0x010)
      end
      return unless advice.anybits?(0x20)

      say "  last cluster is a packed fragment at offset #{hdr.unpack1('L<')}"
      problem "fragment without FRAGMENTS" unless @sb.incompat.anybits?(INCOMPAT_FRAGMENTS)
    end

    # ---- directories -------------------------------------------------------

    # The bytes of a directory whose layout stores plain blocks, or nil.
    def directory_bytes(ino)
      return problem("directory size #{ino.size} is empty or over #{MAX_DIRECTORY_BYTES}") unless
        ino.size.positive? && ino.size <= MAX_DIRECTORY_BYTES

      b = @sb.block_size
      case ino.layout
      when 0
        @vol.read(ino.layout_word * b, ino.size)
      when 2
        full = ino.size / b
        (full.positive? ? @vol.read(ino.layout_word * b, full * b) : "".b) + @vol.read(ino.attrs_end, ino.size - full * b)
      when 4
        chunk_bytes(ino)
      else
        note "compressed directory data is not decoded"
        nil
      end
    end

    def chunk_bytes(ino)
      bits = ino.layout_word & 0x1F
      wide = ino.layout_word.anybits?(0x20)
      width = wide ? 8 : 4
      chunk = @sb.block_size << bits
      start = (ino.attrs_end + width - 1) / width * width
      @vol.check_span(start, width * ((ino.size + chunk - 1) / chunk))
      ((ino.size + chunk - 1) / chunk).times.map do |i|
        block, = chunk_entry(start + width * i, wide, ino.layout_word.anybits?(0x40))
        len = [chunk, ino.size - i * chunk].min
        block ? @vol.read(block * @sb.block_size, len) : "\0".b * len
      end.join.b
    end

    def parse_directory(ino, data)
      b = @sb.block_size
      names = []
      (0...data.bytesize).step(b) do |bpos|
        block = data.byteslice(bpos, b)
        names.concat(parse_block(block))
      end
      names
    end

    def parse_block(block)
      first = block.unpack1("@8S<")
      if first.zero? || (first % 12).nonzero? || first >= block.bytesize
        problem "directory block's first name offset #{first} is not a positive multiple of 12 below #{block.bytesize}"
        return []
      end

      n = first / 12
      ents = Array.new(n) { block.unpack("Q<S<CC", offset: 12 * _1) }
      ents.each_with_index.map do |(nid, off, type, _), i|
        stop = i + 1 < n ? ents[i + 1][1] : (block.index("\0".b, off) || block.bytesize)
        problem "name offsets do not increase or run past the block" if off < first || stop < off || stop > block.bytesize
        [nid, type, block.byteslice(off, [stop - off, 0].max) || "".b]
      end
    end

    def print_directory(ino)
      data = directory_bytes(ino)
      return [] unless data

      entries = parse_directory(ino, data)
      say "  entries: #{entries.size}"
      previous = nil
      entries.each do |nid, type, name|
        say format("    nid=%d type=%d(%s) name=%s", nid, type, DIRENT_TYPES.fetch(type, "invalid"), Fmt.name(name))
        problem "entry name #{Fmt.name(name)} is empty, over #{MAX_NAME} bytes or holds / or NUL" if
          name.empty? || name.bytesize > MAX_NAME || name.include?("/") || name.include?("\0")
        problem "entry type #{type} is invalid" unless DIRENT_TYPES.key?(type)
        problem "names do not increase strictly at #{Fmt.name(name)}" if previous && (previous <=> name) >= 0
        previous = name
      end
      entries
    end

    def print_symlink(ino)
      return unless ino.kind == 0o120000 && ino.layout != 1 && ino.layout != 3 && ino.size.positive? && ino.size <= SYMLINK_MAX

      target = case ino.layout
               when 0 then @vol.read(ino.layout_word * @sb.block_size, ino.size)
               when 2 then symlink_inline(ino)
               end
      say "  target: #{Fmt.name(target)}" if target
    end

    def symlink_inline(ino)
      full = ino.size / @sb.block_size * @sb.block_size
      (full.zero? ? "".b : @vol.read(ino.layout_word * @sb.block_size, full)) + @vol.read(ino.attrs_end, ino.size - full)
    end

    # ---- traversal ----------------------------------------------------------

    def walk(root)
      say
      say "tree from root nid #{root}"
      pending = [[root, nil]]
      until pending.empty?
        nid, expected = pending.pop
        if @seen[nid]
          say "(nid #{nid} already shown)"
          next
        end
        @seen[nid] = true
        pending.concat(dump_inode(nid, expected))
      end
    rescue Error => e
      problem e.message
    end

    # Prints one inode; returns the [nid, expected dirent type] of its children,
    # last first so that the traversal prints them in order.
    def dump_inode(nid, expected)
      ino = read_inode(nid)
      print_inode(ino)
      inode_problems(ino)
      problem "directory entry type #{expected} disagrees with the mode" if expected&.positive? && expected != KIND_TO_DIRENT[ino.kind]
      print_xattrs(ino)
      print_layout(ino)
      return [] unless ino.kind == 0o040000

      print_directory(ino).reject { |_, _, name| [".", ".."].include?(name) }.reverse.map { |n, t, _| [n, t] }
    rescue StandardError => e
      # Per-inode boundary: a hostile inode is reported and the walk goes on.
      problem "nid #{nid}: #{e.class == Error ? '' : "#{e.class}: "}#{e.message}"
      []
    end

    def print_layout(ino)
      case ino.layout
      when 0, 2 then print_flat(ino)
      when 4 then print_chunks(ino)
      when 1, 3 then print_compressed(ino)
      end
      print_symlink(ino)
    end
  end

  def self.main(argv)
    tree = true
    parser = OptionParser.new("usage: erofsdump.rb [--no-tree] IMAGE") { _1.on("--[no-]tree") { |v| tree = v } }
    paths = parser.parse(argv)
    raise Error, parser.banner unless paths.size == 1

    File.open(paths.first, "rb") do |io|
      dumper = Dumper.new(Volume.new(io), $stdout)
      dumper.dump(tree:)
      dumper.problems.zero? ? 0 : 1
    end
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "erofsdump: #{e.message}"
    1
  end
end

exit ErofsDump.main(ARGV) if $PROGRAM_NAME == __FILE__
