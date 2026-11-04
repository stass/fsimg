#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Write a copy of an image with fields of one catalog file record rewritten,
# so that macOS can be shown a record it will not create itself: a file whose
# Finder type and creator are `hlnk`/`hfs+`, a chosen creation date or link
# reference, a flag word. The file record is found by its name, which must
# occur exactly once as the key of a file record. Only research copies are
# patched; no checksum covers a catalog record, so the volume still mounts.
#
#   patch_record.rb SRC DEST NAME [--type CODE] [--creator CODE] [--create-date SECONDS_SINCE_1904]
#                                 [--flags N] [--special N]
#
# SRC is only read. DEST is created exclusively: it must not exist, must not be
# SRC and must not lie under the captures root, so no kept capture can be
# altered. A code is four characters or `0x` and eight hex digits.
require "optparse"

module HfsPlusPatchRecord
  class Error < StandardError; end

  # Offsets within a catalog file record (after its key): flags, createDate,
  # the BSD `special` word and the Finder type and creator.
  FLAGS_AT = 2
  CREATE_DATE_AT = 12
  SPECIAL_AT = 44
  TYPE_AT = 48
  CREATOR_AT = 52
  FILE_RECORD_BYTES = 248
  CAPTURES_ROOT = File.join(Dir.home, "hfsplus-captures")
  # Each option's value range: the width of the field it is packed into.
  RANGES = { create_date: 0..0xFFFF_FFFF, flags: 0..0xFFFF, special: 0..0xFFFF_FFFF }.freeze

  module_function

  # The offset of the record data of the one file record named `name`.
  def find_record(image, name)
    units = name.encode("UTF-16BE").b
    needle = [units.bytesize / 2].pack("n") + units
    hits = occurrences(image, needle).filter_map do |at|
      next if at < 6

      key_length = image.byteslice(at - 6, 2).unpack1("n")
      data = at - 6 + 2 + key_length
      data if key_length == 6 + units.bytesize && image.byteslice(data, 2) == [2].pack("n") && data + FILE_RECORD_BYTES <= image.bytesize
    end
    raise Error, "#{name.inspect} is the key of #{hits.size} file records, not one" unless hits.size == 1

    hits.first
  end

  # Four characters, or `0x` and eight hex digits for codes that are not text.
  def code_bytes(text)
    return [text.delete_prefix("0x")].pack("H8") if text.match?(/\A0x\h{8}\z/)
    raise Error, "#{text.inspect} is not four characters or 0x and eight hex digits" unless text.bytesize == 4

    text.b
  end

  # Refuse a value its field cannot hold, so `pack` does not truncate it.
  def check_ranges(options)
    RANGES.each do |name, range|
      raise Error, "--#{name.to_s.tr('_', '-')} #{options[name]} is outside #{range}" if options[name] && !range.cover?(options[name])
    end
  end

  # The path with symlinks resolved in every existing part; DEST does not exist yet.
  def resolved(path)
    dir = File.realpath(File.dirname(path))
    File.join(dir, File.basename(path))
  end

  # DEST must be a new file, not SRC and not inside the captures root.
  def check_dest(src, dest)
    target = resolved(dest)
    root = File.realpath(CAPTURES_ROOT) if File.exist?(CAPTURES_ROOT)
    raise Error, "#{dest} is #{src}" if target == File.realpath(src)
    raise Error, "#{dest} is under the captures root #{root}" if root && (target == root || target.start_with?("#{root}/"))
  end

  # Every offset where `needle` occurs in `image`, overlapping ones included.
  def occurrences(image, needle)
    Enumerator.produce(image.index(needle)) { |at| image.index(needle, at + 1) }.take_while { _1 }
  rescue StopIteration
    []
  end

  def patch(image, at, options)
    put = ->(offset, packed) { image[at + offset, packed.bytesize] = packed }
    put.call(TYPE_AT, code_bytes(options[:type])) if options[:type]
    put.call(CREATOR_AT, code_bytes(options[:creator])) if options[:creator]
    put.call(CREATE_DATE_AT, [options[:create_date]].pack("N")) if options[:create_date]
    put.call(FLAGS_AT, [options[:flags]].pack("n")) if options[:flags]
    put.call(SPECIAL_AT, [options[:special]].pack("N")) if options[:special]
  end

  def main(argv)
    options = {}
    parser = OptionParser.new do |o|
      o.banner = "usage: patch_record.rb SRC DEST NAME [options]"
      o.on("--type CODE") { options[:type] = _1 }
      o.on("--creator CODE") { options[:creator] = _1 }
      o.on("--create-date N", Integer) { options[:create_date] = _1 }
      o.on("--flags N", Integer) { options[:flags] = _1 }
      o.on("--special N", Integer) { options[:special] = _1 }
    end
    src, dest, name = parser.parse(argv)
    abort(parser.help) unless src && dest && name

    check_ranges(options)
    check_dest(src, dest)
    image = File.binread(src)
    patch(image, find_record(image, name), options)
    File.open(dest, File::WRONLY | File::CREAT | File::EXCL, 0o644) { _1.write(image) }
    0
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "patch_record.rb: #{e.message}"
    1
  end
end

exit(HfsPlusPatchRecord.main(ARGV)) if $PROGRAM_NAME == __FILE__
