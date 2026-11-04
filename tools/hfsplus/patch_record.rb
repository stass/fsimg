#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Write a copy of an image with fields of one catalog file record rewritten,
# so that macOS can be shown a record it will not create itself: a file whose
# Finder type and creator are `hlnk`/`hfs+`, a chosen creation date or link
# reference, a flag word. The file record is found by walking the catalog's
# leaf chain (fields.rb), so that no other bytes of the image can match; its
# name must be the key of exactly one file record. Only research copies are
# patched; no checksum covers a catalog record, so the volume still mounts.
#
#   patch_record.rb SRC DEST NAME [--type CODE] [--creator CODE] [--create-date SECONDS_SINCE_1904]
#                                 [--flags N] [--special N]
#
# SRC is only read. DEST is created exclusively: it must not exist, must not be
# SRC and must not lie under the captures root, so no kept capture can be
# altered. A code is four characters or `0x` and eight hex digits.
require "optparse"
require_relative "fields"

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

  # The image offset of the record data of the one file record named `name`,
  # found in the catalog's leaf chain. A record that the fork's first eight
  # extents split across ranges is refused.
  def find_record(catalog, name)
    fields = HfsPlusFields
    node_size = catalog.tree[:node_size]
    hits = []
    fields.each_leaf(catalog.volume, catalog.fork, catalog.tree) do |slices, node|
      start = fields::NODE_DESCRIPTOR_BYTES
      slices.each do |slice|
        data_at = file_record_data(slice, name)
        hits << node * node_size + start + data_at if data_at
        start += slice.bytesize
      end
    end
    raise Error, "#{name.inspect} is the key of #{hits.size} file records, not one" unless hits.size == 1

    ranges = catalog.volume.fork_ranges(catalog.fork, hits.first, FILE_RECORD_BYTES)
    raise Error, "the record of #{name.inspect} is not within one extent" unless ranges.size == 1 && ranges.first.last == FILE_RECORD_BYTES

    catalog.volume.base + ranges.first.first
  end

  # Where the data of a catalog leaf record starts, if it is a file record
  # keyed by `name`. The key's stored units are compared, never its escaped
  # display text; a stored name is decomposed, so `name` must be too.
  def file_record_data(slice, name)
    _, _, data_at = HfsPlusFields.catalog_key(slice)
    stored = HfsPlusFields.bytes_at(slice, 8, data_at - 8).byteslice(0, slice.unpack1("@6n") * 2)
    data_at if stored == name.encode("UTF-16BE").b && slice.bytesize - data_at == FILE_RECORD_BYTES && slice.byteslice(data_at, 2) == [2].pack("n")
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
    at = File.open(src, "rb") { find_record(HfsPlusFields.open_catalog(_1), name) }
    patch(image, at, options)
    File.open(dest, File::WRONLY | File::CREAT | File::EXCL, 0o644) { _1.write(image) }
    0
  rescue Error, HfsPlusFields::Error, OptionParser::ParseError, SystemCallError => e
    warn "patch_record.rb: #{e.message}"
    1
  end
end

exit(HfsPlusPatchRecord.main(ARGV)) if $PROGRAM_NAME == __FILE__
