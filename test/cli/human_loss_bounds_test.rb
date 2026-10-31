# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_human_metadata_loss_examples_are_bounded
    root = @root
    source = (root / "loss-source")
    source.mkdir
    (0...40).each do |index|
      (source / "f#{format("%02d", index)}").binwrite("".b)
    end
    # Host import exposes access/modification times everywhere and birth time
    # on macOS/FreeBSD. Reproducible FAT creation substitutes every such field.
    expected_losses = (RUBY_PLATFORM.include?("linux") ? 80 : 120)
    notice = "#{expected_losses} metadata fields".encode(Encoding::UTF_8).b
    arguments = ["c", "-n", "-R", "-F", "fat12"]
    operands = ["-C", "loss-source", "."]
    ordinary = invoke(arguments: [*arguments, *operands])
    assert_includes(ordinary.stderr, notice, "ordinary stderr")
    refute_includes(ordinary.stderr, "metadata example:".b, "ordinary stderr")
    verbose = invoke(arguments: [*arguments, "-vv", *operands])
    assert_includes(verbose.stderr, notice, "verbose stderr")
    assert_equal(32, verbose.stderr.scan("metadata example:".b).size)
    refute_includes(verbose.stderr, "fields=0x".b, "verbose stderr")
  end
end
