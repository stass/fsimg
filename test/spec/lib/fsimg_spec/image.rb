# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # A case image that is never held whole in memory: a base image file, the
  # patches mutations wrote over it in order, and its length. Reading
  # overlays the patches on the base, and writing produces a sparse copy, so
  # a 64 MiB base costs per case only the bytes that hold data.
  class ImageView < Data.define(:base, :length, :patches)
    include Checked

    Patch = Data.define(:offset, :bytes)

    def self.open(base) = new(base:, length: File.size(base), patches: [])

    def initialize(base:, length:, patches:) = super(base:, length:, patches: patches.dup.freeze)

    def read(offset, count)
      raise MutationError, "read offset #{offset} is negative" if offset.negative?

      count = [count, length - offset].min
      return "".b if count <= 0

      patches.reduce(read_base(offset, count)) { |bytes, patch| overlay(bytes, at: offset, patch:) }
    end

    # A view with bytes written at offset, which must lie inside the image.
    def patch(offset, bytes)
      raise MutationError, "patch offset #{offset} is negative" if offset.negative?
      raise MutationError, "#{bytes.bytesize} bytes at #{offset} pass the image end #{length}" if offset + bytes.bytesize > length

      with(patches: patches + [Patch.new(offset:, bytes: bytes.b.freeze)])
    end

    # A view cut to new_length, which must not exceed the current length.
    def truncate(new_length)
      raise MutationError, "truncation to #{new_length} would lengthen a #{length}-byte image" if new_length > length

      with(length: new_length)
    end

    # Writes the view to a new file at path: the base's data regions, found
    # with SEEK_DATA and SEEK_HOLE, then the patches. Holes stay holes. A
    # failed write removes the file, so no partial image is left behind.
    def write(path)
      File.open(path, File::WRONLY | File::CREAT | File::EXCL | File::BINARY, 0o600) do |out|
        written = false
        fill(out)
        written = true
      ensure
        FileUtils.rm_f(path) unless written
      end
    end

    # The [start, stop) ranges of io that hold data, clipped to limit. A
    # filesystem that reports no holes, or does not implement SEEK_DATA,
    # yields one range for the whole file.
    def self.data_regions(io, limit)
      regions = []
      while (region = next_region(io, from: regions.last&.last || 0, limit:))
        regions << region
      end
      regions
    rescue Errno::EINVAL, Errno::EOPNOTSUPP
      limit.positive? ? [[0, limit]] : []
    end

    private_class_method def self.next_region(io, from:, limit:)
      return nil if from >= limit

      io.seek(from, IO::SEEK_DATA)
      start = io.pos
      io.seek(start, IO::SEEK_HOLE)
      start < limit ? [start, [io.pos, limit].min] : nil
    rescue Errno::ENXIO
      nil
    end

    private

    def fill(out)
      raise Error, "base #{base} is shorter than the view" if File.size(base) < length

      out.truncate(length)
      File.open(base, "rb") do |source|
        ImageView.data_regions(source, length).each do |start, stop|
          out.seek(start)
          IO.copy_stream(source, out, stop - start, start)
        end
      end
      patches.each { out.pwrite(inside(_1), _1.offset) }
    end

    # The bytes of patch that lie inside the view; a later truncation can
    # cut a patch short or remove it.
    def inside(patch) = patch.bytes.byteslice(0, [length - patch.offset, 0].max)

    # The base's bytes, which must all be there: a base that shrank under
    # the view would make it disagree with the image it writes.
    def read_base(offset, count)
      bytes = File.open(base, "rb") { _1.pread(count, offset) }.b
      raise Error, "base #{base} is shorter than the view" if bytes.bytesize < count

      bytes
    rescue EOFError
      raise Error, "base #{base} is shorter than the view"
    end

    # bytes, read from image offset at, with the part of patch that
    # overlaps them.
    def overlay(bytes, at:, patch:)
      first = [patch.offset, at].max
      last = [patch.offset + patch.bytes.bytesize, at + bytes.bytesize].min
      return bytes if first >= last

      bytes.byteslice(0, first - at) +
        patch.bytes.byteslice(first - patch.offset, last - first) +
        bytes.byteslice(last - at..)
    end
  end
end
