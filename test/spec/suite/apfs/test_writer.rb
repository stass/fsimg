# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/apfs"

# The writer's layout formulas and source generators, independently of a
# created image, and the reading records judged against a known namespace.
class TestApfsWriter < Minitest::Test
  Apfs = FsimgSpec::Apfs
  ModelDouble = Data.define(:tree)
  OutcomeDouble = Data.define(:records)
  StageResultDouble = Data.define(:stage, :outcome, :tree)

  def test_a_1024_block_container_has_an_83_block_prefix
    # C = 1, A = 0, P = 3 * (1 + 1) = 6; L = I = M = F = 1.
    # E = 1 + 1 + 2 * 1 + 1 + 8 * 1 * 1 = 13.
    regions = Apfs::WriterLayout.regions(1024, 1)

    assert_equal 1, regions.chunks
    assert_equal 1, regions.info_blocks
    assert_equal 0, regions.address_blocks
    assert_equal 1, regions.max_volumes
    assert_equal 1, regions.main_limit
    assert_equal 1, regions.pool_limit
    assert_equal 8, regions.descriptor_blocks
    assert_equal 52, regions.data_blocks
    assert_equal 1, regions.pool_bitmap
    assert_equal 6, regions.pool_blocks
    assert_equal 9, regions.data_base
    assert_equal 61, regions.ring_base
    assert_equal 16, regions.ring_blocks
    assert_equal 77, regions.pool_base
    assert_equal 83, regions.prefix_end
  end

  def test_a_128_tib_container_has_7429_blocks_per_quarter_of_its_data_area
    # 1048576 chunks need C = 8323 information blocks and A = 17
    # address blocks. L = I = 512, M = 8, F = 100, S = 2, so
    # E = 512 + 512 + 4 + 1 + 8 * 8 * 100 = 7429.
    blocks = 34_359_738_368
    chunks = blocks.ceildiv(32_768)
    info_blocks = chunks.ceildiv(126)

    regions = Apfs::WriterLayout.regions(blocks, 2)

    assert_equal 4 * 7429, regions.data_blocks
    assert_operator regions.address_blocks, :>, 0
    assert_equal info_blocks.ceildiv(507), regions.address_blocks
  end

  def test_a_rounded_quotient_of_two_becomes_three
    assert_equal 3, Apfs::WriterLayout.q(4, 2)
    assert_equal 3, Apfs::WriterLayout.q(5, 2)
    assert_equal 4, Apfs::WriterLayout.q(7, 2)
  end

  def test_an_absent_label_uses_the_cli_default
    assert_equal "FSIMG", Apfs::WriterLayout.label({})
  end

  def test_an_empty_label_uses_the_writer_default
    assert_equal "Untitled", Apfs::WriterLayout.label(label: "")
  end

  def test_a_supplied_label_is_kept
    assert_equal "Backup \u00e9".b, Apfs::WriterLayout.label(label: "Backup \u00e9")
  end

  def test_each_input_rule_has_a_valid_source_and_a_source_violating_only_that_rule
    registry.rules.select(&:input?).each do |rule|
      valid, invalid = Apfs::Creations.for_rule(rule.id)

      assert_empty cases.source_classification(valid.source).rule_ids, valid.id
      assert_equal [rule.id], cases.source_classification(invalid.source).rule_ids, invalid.id
    end
  end

  def test_profile_sources_have_no_input_violation_and_only_explicit_size_is_unclassified
    Apfs::Creations.profile.each do |creation|
      classification = cases.source_classification(creation.source)

      assert_empty classification.rule_ids, creation.id
      if creation.id == "apfs.create.explicit_size"
        assert_includes classification.unclassified, "input", creation.id
      else
        refute_includes classification.unclassified, "input", creation.id
      end
    end
  end

  def test_seeded_draws_are_valid_and_repeat_the_same_trees
    [1, 2, 3].each do |seed|
      creations = Apfs::Creations.draw(seed:, count: 50)
      repeated = Apfs::Creations.draw(seed:, count: 50)

      assert_equal 50, creations.size, "seed #{seed}"
      assert_equal creations.map { _1.source.tree }, repeated.map { _1.source.tree }, "seed #{seed}"
      assert_equal creations.map { _1.source.settings }, repeated.map { _1.source.settings }, "seed #{seed}"
      creations.each do |creation|
        assert_empty cases.source_classification(creation.source).rule_ids, creation.id
      end
    end
  end

  def test_an_ls_listing_matching_the_known_tree_has_no_problem
    result = stage_result(stage: "ls", records: listing)

    assert_empty created_image.record_problems(result)
  end

  def test_an_ls_listing_with_the_wrong_size_names_the_entry
    wrong_size = listing.map { _1["path"] == "/docs/readme.txt" ? _1.merge("size_bytes" => "4") : _1 }
    result = stage_result(stage: "ls", records: wrong_size)

    problems = created_image.record_problems(result)

    refute_empty problems
    problems.each { assert_includes _1, "/docs/readme.txt" }
  end

  def test_a_verify_record_with_the_wrong_file_count_names_files
    records = [{ "type" => "verify", "complete" => true,
                 "validation_scope" => "structural", "files" => "2",
                 "directories" => "2", "logical_bytes" => "3" }]
    result = stage_result(stage: "verify", records:)

    problems = created_image.record_problems(result)

    assert_equal 1, problems.size
    assert_includes problems.first, "files"
  end

  def test_decoding_without_namespace_leaves_the_tree_undefined
    with_non_apfs_image do |view|
      model = Apfs::Model.decode(view)

      assert FsimgSpec.undefined?(model.tree)
      assert_equal Apfs::Model::UNWALKED, model.tree
    end
  end

  def test_requesting_the_namespace_of_a_non_apfs_image_leaves_the_tree_undefined
    with_non_apfs_image do |view|
      model = Apfs::Model.decode(view, namespace: true)

      assert FsimgSpec.undefined?(model.tree)
    end
  end

  def test_a_directory_named_twice_leaves_the_namespace_undefined
    records = record_set(
      entries: [entry(2, "a", 16, DIRECTORY), entry(2, "b", 16, DIRECTORY)],
      inodes: [inode(16, DIRECTORY)]
    )

    assert_match(/already named/, namespace(records).reason)
  end

  def test_a_directory_containing_its_parent_leaves_the_namespace_undefined
    records = record_set(
      entries: [entry(2, "a", 16, DIRECTORY), entry(16, "up", 2, DIRECTORY)],
      inodes: [inode(16, DIRECTORY), inode(2, DIRECTORY)]
    )

    assert_match(/already named/, namespace(records).reason)
  end

  def test_a_record_naming_no_inode_leaves_the_namespace_undefined
    records = record_set(entries: [entry(2, "lost", 17, REGULAR)], inodes: [])

    assert_match(/no inode record/, namespace(records).reason)
  end

  def test_a_type_differing_from_the_inode_mode_leaves_the_namespace_undefined
    records = record_set(entries: [entry(2, "f", 16, REGULAR)], inodes: [inode(16, DIRECTORY)])

    assert_match(/inode mode/, namespace(records).reason)
  end

  def test_a_link_attribute_shorter_than_its_header_leaves_the_namespace_undefined
    attribute = [Apfs::Records::Key.new(owner: 16, kind: Apfs::Records::ATTRIBUTE,
                                        suffix: ["com.apple.fs.symlink\0".b], hashed: false, bytes: "".b), "\x02\x00".b]
    records = record_set(entries: [entry(2, "l", 16, LINK)], inodes: [inode(16, LINK)], pairs: [attribute])

    assert_match(/shorter than its header/, namespace(records).reason)
  end

  def test_an_empty_file_and_a_directory_walk_to_a_known_tree
    records = record_set(
      entries: [entry(2, "d", 16, DIRECTORY), entry(16, "f", 17, REGULAR)],
      inodes: [inode(16, DIRECTORY), inode(17, REGULAR)]
    )

    assert_equal [["d", "directory", nil], ["d/f", "file", 0]], namespace(records).map { [_1.path, _1.type, _1.size] }
  end

  private

  # Directory-record types, which are also the inode mode's high nibble.
  DIRECTORY = 4
  REGULAR = 8
  LINK = 10
  FieldsDouble = Data.define(:child, :flags, :mode, :private_id)
  VolumeDouble = Data.define(:records)
  ReaderDouble = Data.define(:view, :block_size)
  ViewDouble = Data.define(:length)

  def entry(parent, name, child, type) =
    Apfs::Records::Entry.new(parent:, key: Apfs::Records::Key.new(owner: parent, kind: Apfs::Records::DIRECTORY_RECORD,
                                                                 suffix: ["#{name}\0".b], hashed: false, bytes: "".b),
                             fields: FieldsDouble.new(child:, flags: type, mode: nil, private_id: nil), extended: nil)

  def inode(number, type) =
    Apfs::Records::Inode.new(number:, fields: FieldsDouble.new(child: nil, flags: nil, mode: type << 12, private_id: number),
                             extended: nil)

  def record_set(entries:, inodes:, pairs: []) =
    Apfs::Records::RecordSet.new(pairs:, inodes: inodes.to_h { [_1.number, _1] }, entries:, by_child: entries.group_by(&:child),
                                 by_parent: entries.group_by(&:parent), extents: [], stream_counts: {}, attributes: [])

  def namespace(records) =
    Apfs::Namespace.decode(VolumeDouble.new(records:), ReaderDouble.new(view: ViewDouble.new(length: 1 << 20), block_size: 4096))

  def registry = @registry ||= Apfs::Rules.registry

  def cases = @cases ||= Apfs::Cases.new(
    registry:, ceilings: FsimgSpec.fetch_profile("quick").ceilings,
    name_trials: FsimgSpec::NameTrials::NONE, oracles: false
  )

  def created_image
    tree = [
      FsimgSpec::WalkedEntry.new(path: "docs".b, type: "directory", size: nil, sha256: nil),
      FsimgSpec::WalkedEntry.new(path: "docs/readme.txt".b, type: "file", size: 3,
                                sha256: Digest::SHA256.hexdigest("abc"))
    ]
    Apfs::CreatedImage.new(model: ModelDouble.new(tree:), label: "FSIMG")
  end

  def listing = [
    { "type" => "entry", "path" => "/docs", "kind" => "directory" },
    { "type" => "entry", "path" => "/docs/readme.txt", "kind" => "file", "size_bytes" => "3" }
  ]

  def stage_result(stage:, records:) =
    StageResultDouble.new(stage:, outcome: OutcomeDouble.new(records:), tree: nil)

  def with_non_apfs_image
    Dir.mktmpdir("apfs-writer-") do |directory|
      path = File.join(directory, "image.img")
      File.binwrite(path, "\0".b * (1 << 20))
      yield FsimgSpec::ImageView.open(path)
    end
  end
end
