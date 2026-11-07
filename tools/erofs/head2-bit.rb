# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

# Copy an EROFS image with the COMPR_HEAD2 incompatible feature set.
# Usage: head2-bit.rb SRC DST
#
# No producer writes a head-2 record, and the hand-built E9 images
# (e9-head2-*) carry head-2 records with the bit clear, which fsimg refuses
# (specs/design-erofs.md#compression). This makes the same images with the
# bit set: the incompatible word at superblock offset 0x50 gains 0x0008,
# and when SB_CHKSUM is set the checksum at 0x04 is recomputed over the
# design's range, [1024, B) when the block size B exceeds 1024 and
# [1024, 1024 + B) otherwise, with the checksum field zeroed, CRC-32C
# seeded with all ones and not inverted. The output is reproducible: the
# same input gives the same bytes. SRC is untrusted and only read.

require_relative "erofsdump"

module Head2Bit
  SUPER = 1024
  COMPAT_SB_CHKSUM = 0x0001
  INCOMPAT_COMPR_HEAD2 = 0x0008
  SB_MAGIC = 0xE0F5E1E2

  class Error < StandardError; end

  module_function

  # The image's bytes with the bit set and the checksum, if any, renewed.
  def with_bit(image)
    raise Error, "not an EROFS image" unless image.bytesize >= SUPER + 128 && field(image, 0, "L<") == SB_MAGIC

    incompat = field(image, 0x50, "L<") | INCOMPAT_COMPR_HEAD2
    image.dup.tap { _1[SUPER + 0x50, 4] = [incompat].pack("L<") }.then { checksummed(_1) }
  end

  def checksummed(image)
    return image unless field(image, 0x08, "L<").anybits?(COMPAT_SB_CHKSUM)

    block = 1 << image.getbyte(SUPER + 0x0C)
    last = block > SUPER ? block : SUPER + block
    raise Error, "image shorter than its checksum range" if last > image.bytesize

    covered = image.byteslice(SUPER, last - SUPER).tap { _1[4, 4] = "\0\0\0\0" }
    crc = ErofsDump::Crc32c.update(0xFFFFFFFF, covered)
    image.dup.tap { _1[SUPER + 4, 4] = [crc].pack("L<") }
  end

  def field(image, offset, directive) = image.byteslice(SUPER + offset, 4).unpack1(directive)

  def main(argv)
    raise Error, "usage: head2-bit.rb SRC DST" unless argv.size == 2

    src, dst = argv
    File.binwrite(dst, with_bit(File.binread(src)))
    0
  rescue Error => e
    warn "head2-bit.rb: #{e.message}"
    1
  end

  private_class_method :checksummed, :field
end

exit Head2Bit.main(ARGV) if $PROGRAM_NAME == __FILE__
