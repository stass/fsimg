# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "stringio"
require_relative "../tools/erofs/erofsdump"

class ErofsDumpTest < Minitest::Test
  include ErofsDump

  def dumper(out = StringIO.new) = Dumper.new(nil, out)

  def field(type, value) = CompactField.new(type:, value:)

  def test_crc32c_of_the_standard_check_string
    assert_equal 0xE3069283, Crc32c.update(0xFFFFFFFF, "123456789") ^ 0xFFFFFFFF
  end

  def test_xxh32_matches_the_reference_vectors
    vectors = { "" => 0x02CC5D05, "abc" => 0x32D153FF, "0123456789abcdef" => 0xC2C45B69,
                "The quick brown fox jumps over the lazy dog" => 0xE85EA4DE }

    assert_equal vectors, vectors.to_h { |text, _| [text, Xxh32.hash(text)] }
  end

  # Seed 0x9E3779B1 (the xxHash sanity-check seed) with an empty input and a
  # 43-byte input, which exercises the four-lane loop and the seed - PRIME1
  # lane. Values come from an independent implementation of the specification.
  def test_xxh32_matches_seeded_vectors
    fox = "The quick brown fox jumps over the lazy dog"

    assert_equal [0x36B78AE7, 0x98C7F3BF], [Xxh32.hash("", seed: 0x9E3779B1), Xxh32.hash(fox, seed: 0x9E3779B1)]
  end

  def test_filter_claiming_a_present_attribute_bit_is_a_problem
    out = StringIO.new
    checker = dumper(out)
    checker.instance_variable_set(:@sb, Struct.new(:compat, :filter_reserved).new(0x04, 0))
    checker.send(:check_filter, 0xFFFFFFFF, [3])
    checker.send(:check_filter, ~(1 << 3) & 0xFFFFFFFF, [3])
    checker.send(:check_filter, 0, [3])

    assert_equal 1, checker.problems
    assert_match(/claims a present attribute's bit/, out.string)
  end

  def test_a_table_outside_a_plain_region_is_placed_or_a_problem
    placements = {
      packed: { incompat: INCOMPAT_FRAGMENTS, packed_nid: 40 },
      box: { incompat: INCOMPAT_METABOX, packed_nid: 0 },
      box_beside_a_packed_inode: { incompat: INCOMPAT_METABOX | INCOMPAT_FRAGMENTS, packed_nid: 40 },
      fragments_without_a_packed_inode: { incompat: INCOMPAT_FRAGMENTS, packed_nid: 0 },
      neither: { incompat: 0, packed_nid: 0 }
    }
    found = placements.transform_values do |sb|
      checker = dumper
      checker.instance_variable_set(:@sb, Struct.new(:incompat, :packed_nid).new(sb[:incompat], sb[:packed_nid]))
      [checker.send(:prefix_table_note)[/packed inode|metadata box|no place/], checker.problems]
    end

    assert_equal({ packed: ["packed inode", 0], box: ["metadata box", 0],
                   box_beside_a_packed_inode: ["metadata box", 0],
                   fragments_without_a_packed_inode: ["no place", 1], neither: ["no place", 1] }, found)
  end

  def test_last_nonhead_after_a_big_cluster_marker_is_two_back
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 3), field(TYPE_NONHEAD, 5)]
    head, marker, last = dumper.send(:group_records, fields:, anchor: 10, first: 0, big: true)

    assert_equal 10, head.blkaddr
    assert_equal [1, nil, 3], [marker.delta0, marker.delta1, marker.big_blocks]
    assert_equal [2, 5, nil], [last.delta0, last.delta1, last.big_blocks]
  end

  def test_head_after_a_big_cluster_marker_skips_its_blocks
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 3), field(TYPE_HEAD1, 7)]
    _, _, second = dumper.send(:group_records, fields:, anchor: 10, first: 0, big: true)

    assert_equal 13, second.blkaddr
  end

  def test_big_cluster_whose_head_lies_in_the_previous_group_adds_its_count_at_the_marker
    fields = [field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 3), field(TYPE_HEAD1, 0),
              field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 2), field(TYPE_HEAD1, 0)]
    records = dumper.send(:group_records, fields:, anchor: 20, first: 6, big: true)

    assert_equal [23, 25], records.grep(HeadRecord).map(&:blkaddr)
  end

  def test_group_beginning_at_a_head_takes_the_anchor_without_counting_it_again
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 4), field(TYPE_HEAD1, 0)]
    records = dumper.send(:group_records, fields:, anchor: 30, first: 0, big: true)

    assert_equal [30, 34], records.grep(HeadRecord).map(&:blkaddr)
  end

  def test_blocks_without_big_clusters_start_at_one_and_follow_heads_back
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, 1), field(TYPE_NONHEAD, 2), field(TYPE_HEAD1, 0)]
    records = dumper.send(:group_records, fields:, anchor: 7, first: 0, big: false)

    assert_equal [8, 9], records.grep(HeadRecord).map(&:blkaddr)
  end

  def test_directory_block_names_are_split_at_the_next_offset
    names = [[".", 5], ["..", 1], ["a", 9]]
    offset = 12 * names.size
    entries = names.map do |name, nid|
      "".b.tap { _1 << [nid, offset, 2, 0].pack("Q<S<CC") }.tap { offset += name.bytesize }
    end
    block = (entries.join + names.map(&:first).join).ljust(512, "\0").b

    parsed = dumper.send(:parse_block, block)

    assert_equal [[5, "."], [1, ".."], [9, "a"]], parsed.map { [_1.nid, _1.name] }
  end
end
