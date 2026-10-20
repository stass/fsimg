# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"

# Mutations apply at their bounds and never beyond, and an image view
# writes a sparse copy that keeps the base's holes.
class TestImage < Minitest::Test
  MIB = 1 << 20
  CEILING = 64 * MIB

  def test_integer_mutations_write_little_endian_values_up_to_the_last_byte
    with_base("\0".b * 16) do |image|
      mutated = [
        mutation(op: "set_u8", offset: 15, value: 0xff),
        mutation(op: "set_u16le", offset: 0, value: 0x1234),
        mutation(op: "set_u32le", offset: 12, value: 0xffff_fffe)
      ].reduce(image) { |view, change| change.apply(view) }

      assert_equal "\x34\x12".b, mutated.read(0, 2)
      assert_equal "\xfe\xff\xff\xff".b, mutated.read(12, 4)
    end
  end

  def test_mutation_past_the_image_end_is_refused
    with_base("\0".b * 16) do |image|
      past_end = [
        mutation(op: "set_u8", offset: 16, value: 0),
        mutation(op: "set_u16le", offset: 15, value: 0),
        mutation(op: "set_u32le", offset: 13, value: 0),
        mutation(op: "replace_bytes", offset: 15, bytes: "0000"),
        mutation(op: "truncate", length: 17)
      ]

      past_end.each do |change|
        assert_raises(FsimgSpec::MutationError, change.to_h.to_s) { change.apply(image) }
      end
    end
  end

  def test_patch_at_a_negative_offset_is_refused
    with_base("\0".b * 16) do |image|
      assert_raises(FsimgSpec::MutationError) { image.patch(-1, "x") }
    end
  end

  def test_read_at_a_negative_offset_is_refused
    with_base("\0".b * 16) do |image|
      assert_raises(FsimgSpec::MutationError) { image.read(-1, 2) }
    end
  end

  # Data#with goes through the checking initialize, on Ruby 3.2 as well.
  def test_changed_copy_of_a_mutation_is_checked_again
    assert_raises(FsimgSpec::MutationError) { mutation(op: "set_u8", offset: 0, value: 1).with(value: 300) }
  end

  def test_failed_write_leaves_no_file
    with_base("abcdefgh") do |image, directory|
      copy = File.join(directory, "copy.img")
      File.unlink(image.base)

      assert_raises(Errno::ENOENT) { image.write(copy) }
      refute File.exist?(copy)
    end
  end

  def test_value_that_does_not_fit_its_width_is_refused
    [{ op: "set_u8", offset: 0, value: 256 }, { op: "set_u16le", offset: 0, value: 65_536 },
     { op: "set_u32le", offset: 0, value: 1 << 32 }, { op: "set_u8", offset: 0, value: -1 }].each do |fields|
      assert_raises(FsimgSpec::MutationError, fields.to_s) { mutation(**fields) }
    end
  end

  def test_recipe_mutation_is_refused_when_unknown_malformed_or_beyond_the_ceiling
    [{ op: "set_u64le", offset: 0, value: 0 }, { op: "set_u8", offset: 0 },
     { op: "set_u8", offset: 0, value: 0, extra: 1 }, { op: "set_u8", offset: "0", value: 0 },
     { op: "replace_bytes", offset: 0, bytes: "ABCD" }, { op: "replace_bytes", offset: 0, bytes: "abc" },
     { op: "truncate", length: CEILING + 1 }, { op: "set_u32le", offset: CEILING - 3, value: 0 }].each do |fields|
      assert_raises(FsimgSpec::MutationError, fields.to_s) { FsimgSpec::Mutation.load(fields, ceiling: CEILING) }
    end
  end

  def test_later_patch_wins_and_truncation_cuts_reads
    with_base("abcdefgh") do |image|
      view = mutation(op: "replace_bytes", offset: 2, bytes: "5858").apply(image)
      view = mutation(op: "set_u8", offset: 3, value: 0x59).apply(view)
      view = mutation(op: "truncate", length: 5).apply(view)

      assert_equal "abXYe", view.read(0, 100)
      assert_equal "", view.read(5, 1)
    end
  end

  def test_written_copy_equals_the_view_and_its_length
    with_base("abcdefgh") do |image, directory|
      view = mutation(op: "set_u8", offset: 6, value: 0x5a).apply(image)
      view = mutation(op: "truncate", length: 7).apply(view)
      copy = File.join(directory, "copy.img")
      view.write(copy)

      assert_equal "abcdefZ", File.binread(copy)
    end
  end

  # The base holds data at its start and in its middle, with holes
  # elsewhere. The view's copy must keep a hole; a dense copy of the same
  # base is the seeded failure that shows the check can fail. A filesystem
  # that stores written zeros as holes, such as ZFS with compression,
  # cannot show that failure, so the test skips there.
  def test_written_copy_keeps_the_base_holes
    Dir.mktmpdir do |directory|
      base = File.join(directory, "base.img")
      File.open(base, "wb") do |file|
        file.truncate(16 * MIB)
        file.pwrite("boot".b, 0)
        file.pwrite("data".b, 8 * MIB)
      end
      skip "#{directory}'s filesystem reports no holes" unless holes?(base)

      dense = File.join(directory, "dense.img")
      File.binwrite(dense, File.binread(base))
      skip "#{directory}'s filesystem turns written zeros into holes" if holes?(dense)

      sparse = File.join(directory, "sparse.img")
      FsimgSpec::ImageView.open(base).write(sparse)

      assert holes?(sparse), "the view's copy keeps a hole"
      assert_equal File.binread(base), File.binread(sparse)
    end
  end

  private

  def mutation(op:, **fields) = FsimgSpec::Mutation.load({ op:, **fields }, ceiling: CEILING)

  def with_base(bytes)
    Dir.mktmpdir do |directory|
      base = File.join(directory, "base.img")
      File.binwrite(base, bytes)
      yield FsimgSpec::ImageView.open(base), directory
    end
  end

  # Whether the file has a hole before its end; on a file without one,
  # SEEK_HOLE answers the end of the file. A filesystem that cannot seek
  # to holes keeps none the test can see, so the test skips.
  def holes?(path)
    File.open(path, "rb") { _1.seek(0, IO::SEEK_HOLE) && _1.pos < File.size(path) }
  rescue Errno::EINVAL, Errno::EOPNOTSUPP
    skip "#{File.dirname(path)}'s filesystem cannot seek to holes"
  end
end
