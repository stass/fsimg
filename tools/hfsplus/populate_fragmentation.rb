# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E7 puts on a mounted volume: forks that need extents
# overflow records, made by filling the volume with one-block files, deleting
# every other one, and writing the forks into the single-block holes.
#
#   fill/fNNNN            one-block files until the volume is full
#   decoy                 a file that takes the free runs longer than one block
#   data-9 data-17        files whose data fork should take 9 and 17 extents
#   rsrc-9 rsrc-17        files whose resource fork should take 9 and 17 extents
#
# The holes are the odd-numbered fill files among the first HOLE_FILES; every
# fork block comes from a hole, so each hole is one extent unless macOS joins
# neighbours. Each step returns the SHA-256 of what it wrote and what the
# volume reads back.
require "digest"
require "fileutils"
require_relative "populate"

module HfsPlusPopulateFragmentation
  P = HfsPlusPopulate
  HOLE_FILES = 120
  DECOY_BLOCKS = 20
  FORK_BLOCKS = { "data-9" => 9, "data-17" => 17, "rsrc-9" => 9, "rsrc-17" => 17 }.freeze

  module_function

  def e7(block_size)
    [fill(block_size), punch, decoy(block_size)] + FORK_BLOCKS.map { |name, blocks| fork_file(name, blocks, block_size) }
  end

  # Write one-block files until the volume refuses another.
  def fill(block_size)
    P.step("fill", "fill/f0000") do |root|
      P.mkdir_p(root, "fill")
      count = 0
      loop do
        File.binwrite(File.join(root, format("fill/f%04d", count)), P.pattern(block_size, count))
        count += 1
      end
    rescue Errno::ENOSPC
      FileUtils.rm_f(File.join(root, format("fill/f%04d", count)))
      P.observe("filled with #{count} files")
    end
  end

  def punch
    P.step("punch", "fill/f0001") do |root|
      holes = (1...HOLE_FILES).step(2).to_a
      holes.each { File.delete(File.join(root, format("fill/f%04d", _1))) }
      P.observe("deleted #{holes.size} files")
    end
  end

  # Free runs longer than one block remain after the fill, such as the slack
  # beside the catalog; this file takes them so that the forks fall into holes.
  def decoy(block_size)
    P.step("decoy", "decoy") do |root|
      P.write_file(root, "decoy", P.pattern(DECOY_BLOCKS * block_size, 0))
    end
  end

  def fork_file(name, blocks, block_size)
    P.step("fork-extents", name) do |root|
      path = File.join(root, name)
      bytes = P.pattern(blocks * block_size, blocks)
      File.binwrite(path, "")
      target = name.start_with?("rsrc") ? "#{path}/..namedfork/rsrc" : path
      File.binwrite(target, bytes)
      P.observe("wrote #{Digest::SHA256.hexdigest(bytes)}, reads #{Digest::SHA256.file(target).hexdigest}")
    end
  end
end
