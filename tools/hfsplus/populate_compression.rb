# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E6 puts on a mounted volume: files of several sizes and
# contents compressed by one tool, which is afsctool with a codec or ditto.
# Each step writes a file, compresses it, and returns what `afsctool -v`
# reports of the result (the kernel hides the attribute and the fork, so the
# bytes themselves are read from the unmounted image by fields.rb).
#
#   text-N random-N zeros-N    contents of N bytes, one file each
#   mixed-N                    text chunks of 65536 bytes with a random one between
#   forced-*-N                 the same with afsctool -L, which keeps chunks that grew
#   shape-N                    text of N bytes, to see the chunk table at 3 to 10 chunks
#   fit-N                      text of N bytes, to find where inline payloads end
#   sweep-N                    zeros of N bytes, to find the largest inline size
#
# `e6(tool)` takes "zlib", "lzvn", "lzfse" or "ditto"; `e6_sweep` runs the
# sweep with every tool.
require "tmpdir"
require_relative "populate"

module HfsPlusPopulateCompression
  P = HfsPlusPopulate
  SIZES = [0, 1, 65_535, 65_536, 65_537, 200_000].freeze
  FORCED_SIZES = [1, 65_536, 65_537, 200_000].freeze
  SHAPE_SIZES = [131_073, 262_145, 393_217, 589_825].freeze
  MIXED_SIZE = 196_608
  SWEEP_SIZES = [65_536, 65_537, 100_000, 262_144, 1 << 20, 1 << 22, 1 << 24].freeze
  FIT_SIZES = [2000, 4000, 6000, 8000, 10_000, 12_000, 16_000].freeze
  TOOLS = %w[zlib lzvn lzfse ditto].freeze
  WORDS = %w[the of and a to in is you that it he was for on are as with his they at be this from have or by one had not but what all were when we there can].freeze

  module_function

  def e6(tool)
    plain = SIZES.product(%w[text random zeros]).map { |size, kind| compress_step(tool, "#{kind}-#{size}", content(kind, size)) }
    plain += SHAPE_SIZES.map { compress_step(tool, "shape-#{_1}", content("text", _1)) }
    plain << compress_step(tool, "mixed-#{MIXED_SIZE}", content("mixed", MIXED_SIZE))
    return plain if tool == "ditto"

    plain + FORCED_SIZES.map { compress_step(tool, "forced-random-#{_1}", content("random", _1), force: true) } +
      [compress_step(tool, "forced-mixed-#{MIXED_SIZE}", content("mixed", MIXED_SIZE), force: true)]
  end

  def e6_sweep
    TOOLS.product(SWEEP_SIZES).map { |tool, size| compress_step(tool, "sweep-#{tool}-#{size}", content("zeros", size)) } +
      TOOLS.product(FIT_SIZES).map { |tool, size| compress_step(tool, "fit-#{tool}-#{size}", content("text", size)) }
  end

  def compress_step(tool, name, bytes, force: false)
    P.step("compress-#{tool}", name) do |root|
      path = File.join(root, name)
      write_and_compress(tool, path, bytes, force)
      P.tool("afsctool", "-v", path)
    rescue P::ToolFailed => e
      "#{e.message}\n#{File.exist?(path) ? "file size #{File.size(path)}" : 'no file'}"
    end
  end

  def write_and_compress(tool, path, bytes, force)
    if tool == "ditto"
      Dir.mktmpdir("e6-src") do |src|
        File.binwrite(File.join(src, "s"), bytes)
        P.tool("ditto", "--hfsCompression", File.join(src, "s"), path)
      end
    else
      File.binwrite(path, bytes)
      P.tool("afsctool", "-c", *(force ? ["-L"] : []), "-T", tool.upcase, "-s", "0", "-v", path)
    end
  end

  # Deterministic content of a kind: running text, incompressible bytes or zeros.
  def content(kind, size)
    rng = Random.new(size)
    case kind
    when "text" then Array.new(size / 3 + 1) { WORDS[rng.rand(WORDS.size)] }.join(" ").byteslice(0, size)
    when "random" then rng.bytes(size)
    when "mixed" then Array.new((size + 65_535) / 65_536) { |i| content(i.odd? ? "random" : "text", [size - i * 65_536, 65_536].min) }.join.b
    else "\0".b * size
    end
  end
end
