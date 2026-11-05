#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Decode a static NDIF disk image, the `bcem` resource of a self-mounting .smi in
# a MacBinary II wrapper, to a raw image for the corpus specimens fsimg's own
# container layer does not read. Handles ADC, zero and raw chunks. The input is
# untrusted: a read outside a structure or a chunk that does not fill its sectors
# raises, and nothing but the output path is written. docs/build.md gives the
# specimen this produces and its digest.
#
#   ndif.rb IN.smi.bin OUT.img
module Ndif
  class Error < StandardError; end

  MACBINARY_BYTES = 128
  SECTOR_BYTES = 512
  BCEM_HEADER_BYTES = 128
  CHUNK_BYTES = 12
  CHUNK_ZERO = [0x00, 0x02].freeze
  CHUNK_RAW = 0x01
  CHUNK_ADC = 0x83
  CHUNK_END = 0xff

  Chunk = Data.define(:sector, :type, :offset, :length)

  module_function

  def slice(bytes, offset, length)
    piece = bytes.byteslice(offset, length)
    raise Error, "read past the end at #{offset}" if piece.nil? || piece.bytesize != length

    piece
  end

  def word(bytes, offset) = slice(bytes, offset, 4).unpack1("N")

  def half(bytes, offset) = slice(bytes, offset, 2).unpack1("n")

  # The resource fork follows the data fork, padded to MacBinary's 128 bytes.
  def resource_fork(image)
    header = slice(image, 0, MACBINARY_BYTES)
    data_length = word(header, 83)
    padded = (data_length + MACBINARY_BYTES - 1) / MACBINARY_BYTES * MACBINARY_BYTES
    slice(image, MACBINARY_BYTES + padded, word(header, 87))
  end

  # The classic resource map: a type list whose entries each name a reference
  # list, whose entries hold 24-bit offsets into the resource data.
  def bcem_resource(fork)
    data_offset = word(fork, 0)
    map = slice(fork, word(fork, 4), word(fork, 12))
    type_list = half(map, 24)
    types = (0..half(map, 28)).map { slice(map, 30 + (8 * _1), 8) }
    entry = types.find { _1.start_with?("bcem") } or raise Error, "no bcem resource"
    reference = slice(map, type_list + entry.unpack1("@6n"), CHUNK_BYTES)
    start = data_offset + (reference.unpack1("@4N") & 0xffffff)
    slice(fork, start + 4, word(fork, start))
  end

  def chunks(bcem)
    (0...word(bcem, 124)).map do
      raw = slice(bcem, BCEM_HEADER_BYTES + (CHUNK_BYTES * _1), CHUNK_BYTES)
      Chunk.new(sector: raw.unpack1("N") >> 8, type: raw.getbyte(3),
                offset: raw.unpack1("@4N"), length: raw.unpack1("@8N"))
    end
  end

  # ADC's three token forms: a literal run, and copies with a two or one byte
  # distance. Returns the next token's position.
  def adc_token(input, position, out)
    token = input.getbyte(position) or raise Error, "ADC stream ends early"
    return adc_literal(input, position, token, out) if token.anybits?(0x80)
    return adc_copy(out, (token & 0x3f) + 4, half(input, position + 1), position + 3) if token.anybits?(0x40)

    distance = ((token & 0x03) << 8) + slice(input, position + 1, 1).ord
    adc_copy(out, ((token & 0x3f) >> 2) + 3, distance, position + 2)
  end

  def adc_literal(input, position, token, out)
    length = (token & 0x7f) + 1
    out << slice(input, position + 1, length)
    position + 1 + length
  end

  def adc_copy(out, length, distance, next_position)
    raise Error, "ADC distance precedes the output" if distance >= out.bytesize

    length.times { out << out.getbyte(out.bytesize - distance - 1) }
    next_position
  end

  def adc(input, want)
    out = String.new(capacity: want, encoding: Encoding::BINARY)
    position = 0
    position = adc_token(input, position, out) while out.bytesize < want
    out
  end

  def chunk_bytes(image, chunk, want)
    case chunk.type
    when CHUNK_ADC then adc(slice(image, MACBINARY_BYTES + chunk.offset, chunk.length), want)
    when CHUNK_RAW then slice(image, MACBINARY_BYTES + chunk.offset, want)
    when *CHUNK_ZERO then "\0".b * want
    else raise Error, format("chunk type 0x%02x", chunk.type)
    end
  end

  def decode(image, output)
    bcem = bcem_resource(resource_fork(image))
    sectors = word(bcem, 68)
    table = chunks(bcem)
    written = table.take_while { _1.type != CHUNK_END }
    written.zip(table.drop(1).map(&:sector) << sectors).each do |chunk, finish|
      raise Error, "chunks are out of order" if finish < chunk.sector

      want = (finish - chunk.sector) * SECTOR_BYTES
      bytes = chunk_bytes(image, chunk, want)
      raise Error, "chunk at sector #{chunk.sector} overruns" if bytes.bytesize != want

      output.write(bytes)
    end
  end
end

if $PROGRAM_NAME == __FILE__
  source, destination = ARGV
  abort("usage: ndif.rb IN.smi.bin OUT.img") if destination.nil?
  File.open(destination, "wb") { Ndif.decode(File.binread(source), _1) }
end
