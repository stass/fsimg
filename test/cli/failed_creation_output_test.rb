# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_failed_creation_reports_geometry_without_publication
    root = @root
    payload = @payload
    source = (root / "oversized-source")
    source.mkdir
    # The sparse payload exceeds the FAT12 writer's capacity. Planning must
    # reject it before any image creation or payload copying. Reproducible
    # timestamp conversion emits loss examples before geometry rejection.
    (source / "large.bin").open("wb") do |payload|
      payload.truncate((256 * 1024) * 1024)
    end

    message = "fsimg: source tree is too large for FAT12\n".b
    arguments = ["c", "-R", "-F", "fat12", "-f", "oversized.img"]
    operands = ["-C", "oversized-source", "."]
    rejected = invoke(arguments: [*arguments, *operands], expected: 1)
    assert_equal("".b, rejected.stdout, "rejected stdout")
    assert(rejected.stderr.end_with?(message), "rejected stderr end with message")
    lines = rejected.stderr.lines
    assert_equal(message, lines[-1])
    assert(lines[0...-1].all? { |line| line.start_with?("fsimg: warning: OS sandbox".b) },
      "Only sandbox warnings may precede the geometry error; got #{lines[0...-1].inspect}")
    refute_includes(rejected.stderr, "metadata".b, "rejected stderr")
    refute_includes(rejected.stderr, "0 bytes".b, "rejected stderr")
    refute((root / "oversized.img").exist?, "a failed size check leaves no output image")
    assert_empty(root.glob(".fsimg-*").to_a, "root glob fsimg to a empty")
    verbose = invoke(arguments: [*arguments, "-vv", *operands], expected: 1)
    assert_includes(verbose.stderr, "metadata example:".b, "verbose stderr")
    assert(verbose.stderr.end_with?(message), "verbose stderr end with message")
    refute_includes(verbose.stderr, "final count unavailable".b, "verbose stderr")
    decoded = failed(invoke(arguments: [*arguments, "--json", *operands], expected: 1))
    assert_equal(-7, decoded.fetch("code"), "decoded code")
    assert_equal("absent", decoded.fetch("output_state"), "decoded output state")
    assert_equal("source tree is too large for FAT12", decoded.fetch("message"))
    assert_equal("none", decoded.fetch("resource"), "decoded resource")
    assert_equal("0", decoded.fetch("limit"), "decoded limit")
    constrained = invoke(arguments: [*arguments, "-o", "cluster=512", *operands], expected: 1)
    assert_includes(constrained.stderr, "source tree is too large for FAT12 with 512-byte clusters".b, "constrained stderr")
    small = invoke(
      arguments: [
        "c",
        "-n",
        "-R",
        "-F",
        "fat32",
        "-s",
        "8M",
        "-o",
        "cluster=512",
        *operands
      ],
      expected: 1
    )
    assert_includes(small.stderr, "too small for FAT32 with 512-byte clusters".b, "small stderr")
  end
end
