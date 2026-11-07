# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "tmpdir"
require "fsimg_spec/erofs"

# What the independent model decodes from images that need more than the
# builder's plain tree: a directory with two names, and truncated input.
class ErofsModelTest < Minitest::Test
  Erofs = FsimgSpec::Erofs
  W = Erofs::Witnesses

  def test_a_directory_with_two_names_in_one_parent_is_listed_under_each
    model = decode(mutations: [W.bytes(W.root(2), W.nid_bytes(Erofs::Builder::SUB)), W.u8(W.root(2, 10), 2)])

    paths = model.tree.map(&:path)
    assert_includes paths, "ext.bin/leaf".b
    assert_includes paths, "sub/leaf".b
    assert_empty classification(model).rule_ids
  end

  def test_a_directory_below_itself_is_recorded_for_the_rules_and_not_entered_again
    model = decode(mutations: [W.bytes(W.sub(2), W.entry_bytes(nid: Erofs::Builder::SUB, name_offset: 39, type: 2))])

    assert_equal 1, model.tree.count { _1.path == "sub/leaf".b }
    assert_includes classification(model).rule_ids, "erofs.dir.parent"
  end

  def test_a_directory_block_the_input_lacks_inside_the_volume_leaves_the_directories_scope_unclassified
    model = decode(mutations: [FsimgSpec::Mutation::Truncate.new(length: Erofs::Builder::SUB_DATA + 8)])

    assert_equal %w[destination directories files verification], model.unclassified_scopes.sort
  end

  def test_a_file_the_input_lacks_inside_the_volume_leaves_only_the_file_scopes_unclassified
    model = decode(parameters: {}, mutations: [FsimgSpec::Mutation::Truncate.new(length: Erofs::Builder::EXT_DATA + 8)])

    assert_equal %w[destination files verification], model.unclassified_scopes.sort
  end

  def test_data_past_the_declared_volume_is_for_the_rules_not_a_cut_off
    model = decode(parameters: W::NO_CHECKSUM, mutations: [W.u32(W.sb(0x24), 3)])

    assert_empty model.unclassified_scopes
    assert_includes classification(model).rule_ids, "erofs.inode.data_domain"
  end

  def test_a_hole_reads_as_zeros_and_chunks_read_from_their_blocks_whether_or_not_shared
    data = "chunk data".b
    model = decode(parameters: W::CHUNKED, mutations: [W.bytes(Erofs::Builder::EXT_DATA, data), W.bytes(Erofs::Builder::SUB_DATA, "sub".b)])

    block = "\0".b * W::B
    # chunk4.bin: ext.bin's block, a hole, sub's block, then 5 bytes of ext.bin's block again.
    four = data.ljust(W::B, "\0".b) + block + "sub".b.ljust(W::B, "\0".b) + data.byteslice(0, 5)
    # chunk8.bin: ext.bin's block and the one after it, then a hole of one block.
    eight = data.ljust(W::B, "\0".b) + "sub".b.ljust(W::B, "\0".b) + block
    assert_equal [Digest::SHA256.hexdigest(four), Digest::SHA256.hexdigest(eight)],
                 %w[chunk4.bin chunk8.bin].map { |name| model.tree.find { _1.path == name.b }.sha256 }
    assert_empty classification(model).rule_ids
  end

  def test_the_high_half_of_a_chunk_layout_word_changes_nothing
    plain = decode(parameters: W::CHUNKED, mutations: [])
    high = decode(parameters: W::CHUNKED, mutations: [W.u32(W.inode(Erofs::Builder::CHUNK4, 0x10), 0xABCD0000)])

    assert_equal plain.tree, high.tree
  end

  def test_a_chunk_based_body_includes_its_index_rounded_to_the_entry_width
    model = decode(parameters: W::CHUNKED, mutations: [W.u16(W.inode(Erofs::Builder::CHUNK8, 2), 1)])

    inode = model.inodes.fetch(Erofs::Builder::CHUNK8)
    # 32 fixed bytes and a 12-byte region end at 44; the index starts at 48.
    assert_equal 32 + 12 + 4 + 2 * 8, inode.body_bytes(W::B)
    assert_equal inode.offset + 48, inode.index_offset
  end

  def test_every_mode_type_has_a_listed_kind
    assert_equal Erofs::Model::TYPES.keys.sort, Erofs::Model::KINDS.keys.sort
  end

  private

  def decode(mutations:, parameters: W::SUBDIRECTORY)
    witness = W::Witness.new(id: "erofs.test.model", parameters:, mutations:, intended: nil, rules: [])
    with_cases { |cases| @result = cases.call(witness) }
    @result.model
  end

  def classification(_model) = @result.kase.classification

  def with_cases
    Dir.mktmpdir("erofs-model-") do |directory|
      Dir.mkdir(File.join(directory, "trials"), 0o700)
      yield Erofs::Cases.new(registry: Erofs::Rules.registry, cache: FsimgSpec::FixtureCache.new(File.join(directory, "cache")),
                             ceilings: FsimgSpec.fetch_profile("quick").ceilings,
                             name_trials: FsimgSpec::NameTrials.new(File.join(directory, "trials")))
    end
  end
end
