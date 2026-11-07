# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "stringio"
require_relative "../tools/erofs/erofsdump"

class ErofsDumpTest < Minitest::Test
  include ErofsDump

  def dumper = Dumper.new(nil, StringIO.new)

  def field(type, value) = CompactField.new(type:, value:)

  def test_crc32c_of_the_standard_check_string
    assert_equal 0xE3069283, Crc32c.update(0xFFFFFFFF, "123456789") ^ 0xFFFFFFFF
  end

  def test_last_nonhead_after_a_big_cluster_marker_is_two_back
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 3), field(TYPE_NONHEAD, 5)]
    head, marker, last = dumper.send(:group_records, fields, 10, 0)

    assert_equal 10, head.blkaddr
    assert_equal [1, nil, 3], [marker.delta0, marker.delta1, marker.big_blocks]
    assert_equal [2, 5, nil], [last.delta0, last.delta1, last.big_blocks]
  end

  def test_head_after_a_big_cluster_marker_skips_its_blocks
    fields = [field(TYPE_HEAD1, 0), field(TYPE_NONHEAD, BIG_CLUSTER_FLAG | 3), field(TYPE_HEAD1, 7)]
    _, _, second = dumper.send(:group_records, fields, 10, 0)

    assert_equal 13, second.blkaddr
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
