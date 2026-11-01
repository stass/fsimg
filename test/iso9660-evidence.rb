# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require 'json'

# Counts and numeric metadata only; never emit member names or payloads.
# This evidence reader handles the corpus's 2048-byte, SP-skip-zero layout.
# It is independent of isoinfo and is not a general ISO verifier.
class IsoEvidence
  class Error < StandardError; end
  Identifier = Data.define(:units, :directory)

  def initialize(file)
    @file = file
    @counts = Hash.new(0)
    @values = Hash.new { |hash, key| hash[key] = [] }
  end

  def call
    descriptors = (16...80).lazy.map { bytes(offset: _1 * 2048, length: 2048) }
    descriptors.take_while { _1.getbyte(0) != 255 }.each do |descriptor|
      raise Error, 'descriptor signature' unless descriptor.byteslice(1, 5) == 'CD001'
      next unless [1, 2].include?(descriptor.getbyte(0))

      joliet = descriptor.getbyte(0) == 2
      next if joliet && !%w[%/@ %/C %/E].include?(descriptor.byteslice(88, 3))

      view = joliet ? 'joliet' : 'primary'
      walk(descriptor: descriptor, view: view, joliet: joliet)
      %i[little big].each do |endian|
        @counts["#{view}_path_inversions_#{endian}"] = path_inversions(
          descriptor: descriptor, joliet: joliet, endian: endian)
      end
    end
    { counts: @counts, values: @values }
  end

  private

  def bytes(offset:, length:)
    unless offset >= 0 && length.between?(0, 16 * 1024 * 1024) && offset + length <= @file.size
      raise Error, 'metadata range outside image or evidence read limit'
    end
    return ''.b if length.zero?

    data = @file.pread(length, offset)
    raise Error, 'short metadata read' unless data.bytesize == length

    data
  end

  def number(data, offset, format: 'V') = data.unpack1(format, offset: offset)
  def add(key, value) = @values[key] |= [value]
  def identifier(name, joliet:, directory:)
    Identifier.new(units: joliet ? name.unpack('n*') : name.bytes, directory: directory)
  end

  # Names/extensions compare after space padding; file versions descend.
  # A directory and a file with the same stem can compare equal.
  def compare(left, right)
    parts = [left, right].map do |entry|
      name = entry.units
      semi = entry.directory ? nil : name.index(59)
      version = semi ? name[(semi + 1)..].pack('U*').to_i : nil
      stem = semi ? name.take(semi) : name
      dot = entry.directory ? nil : stem.index(46)
      [dot ? stem.take(dot) : stem, dot ? stem[(dot + 1)..] : [], version]
    end
    [0, 1].each do |index|
      a, b = parts.map { _1[index] }
      width = [a.size, b.size].max
      order = (a + [32] * (width - a.size)) <=> (b + [32] * (width - b.size))
      return order unless order.zero?
    end
    a, b = parts.map { _1[2] }
    a && b ? b <=> a : 0
  end

  def susp(area, depth: 0)
    raise Error, 'excessive SUSP continuation chain' if depth > 32

    entries = []
    offset = 0
    while offset + 4 <= area.bytesize && area.getbyte(offset) != 0
      length = area.getbyte(offset + 2)
      raise Error, 'short SUSP entry' if length < 4 || offset + length > area.bytesize

      entry = area.byteslice(offset, length)
      entries << entry
      signature = entry.byteslice(0, 2)
      case signature
      when 'PX' then add('px_lengths', length)
      when 'ER'
        @counts['rrip_1991a'] += 1 if entry.byteslice(8, entry.getbyte(4)) == 'RRIP_1991A'
      when 'SP' then add('sp_skip', entry.getbyte(6))
      when 'CE'
        block, start, count = [4, 12, 20].map { number(entry, _1) }
        entries.concat(susp(bytes(offset: block * 2048 + start, length: count), depth: depth + 1))
      end
      break if signature == 'ST'

      offset += length
    end
    entries
  end

  def records(data)
    offset = 0
    rows = []
    while offset < data.bytesize
      length = data.getbyte(offset)
      if length.zero?
        offset = (offset / 2048 + 1) * 2048
        next
      end
      raise Error, 'short directory record' if length < 34 || offset + length > data.bytesize

      rows << data.byteslice(offset, length)
      offset += length
    end
    rows
  end

  def walk(descriptor:, view:, joliet:)
    queue = [descriptor.byteslice(156, 34)]
    seen = {}
    until queue.empty?
      directory = queue.shift
      block, length = [2, 10].map { number(directory, _1) }
      next if seen[block]
      raise Error, 'too many directories' if seen.size >= 100_000

      seen[block] = true
      children = records(bytes(offset: block * 2048, length: length)).filter_map do |record|
        name = record.byteslice(33, record.getbyte(32))
        raise Error, 'short identifier' unless name.bytesize == record.getbyte(32)

        start = 33 + name.bytesize + (name.bytesize.even? ? 1 : 0)
        entries = joliet ? [] : susp(record.byteslice(start..))
        if entries.any? { _1.start_with?('CL') }
          px = entries.find { _1.start_with?('PX') }
          add('relocation_shapes', { flags: record.getbyte(25), extent: number(record, 2),
                                    size: number(record, 10), px_mode: px && number(px, 4).to_s(8) })
        end
        next if name.bytesize == 1 && name.getbyte(0) < 2

        queue << record if record.getbyte(25) & 2 != 0
        identifier(name, joliet: joliet, directory: record.getbyte(25) & 2 != 0)
      end
      @counts["#{view}_directory_inversions"] += children.each_cons(2).count { compare(*_1).positive? }
    end
  end

  def path_inversions(descriptor:, joliet:, endian:)
    little = endian == :little
    block = number(descriptor, little ? 140 : 148, format: little ? 'V' : 'N')
    data = bytes(offset: block * 2048, length: number(descriptor, 132))
    offset = 0
    rows = []
    while offset < data.bytesize
      length = data.getbyte(offset)
      raise Error, 'short path-table record' if length.zero? || offset + 8 + length > data.bytesize

      parent = number(data, offset + 6, format: little ? 'v' : 'n')
      name = identifier(data.byteslice(offset + 8, length), joliet: joliet, directory: true)
      rows << [parent, name]
      offset += 8 + length + length % 2
    end
    rows.drop(1).each_cons(2).count do |left, right|
      order = left[0] <=> right[0]
      (order.zero? ? compare(left[1], right[1]) : order).positive?
    end
  end
end

if $PROGRAM_NAME == __FILE__
  abort 'usage: ruby test/iso9660-evidence.rb CORPUS-DIRECTORY-OR-IMAGE ...' if ARGV.empty?
  ARGV.flat_map { File.directory?(_1) ? Dir.glob(File.join(_1, '*.iso')).sort : [_1] }.each do |path|
    result = File.open(path, 'rb') { IsoEvidence.new(_1).call }
    puts JSON.generate(image: File.basename(path), evidence: result)
  end
end
