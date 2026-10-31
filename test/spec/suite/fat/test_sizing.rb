# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The layout create gives a FAT volume (Fat::Sizing), against sector
# counts worked by hand from section 3.5: an exact size's cluster count,
# the writer's directory slots and root region, the cluster size create
# falls back to, and a created image off its layout.
class TestFatSizing < Minitest::Test
  Fat = FsimgSpec::Fat
  ONE_FILE = [FsimgSpec::TreeEntry.file("A", size: 1, content: "zero")].freeze
  CEILINGS = FsimgSpec.fetch_profile("quick").ceilings

  # 512-byte sectors and clusters, one reserved sector, a 32-sector root
  # region of 512 entries, and two FATs: 4084 FAT12 clusters need 4086
  # packed entries, 6129 bytes in 12 sectors; 4085 FAT16 clusters 8174
  # bytes in 16; 65525 FAT32 clusters 262108 bytes in 512 sectors, beside
  # 32 reserved sectors and no root region.
  def test_exact_sizes_give_the_counts_worked_by_hand
    counts = { "fat12" => 1 + 32 + 24 + 4084, "fat16" => 1 + 32 + 32 + 4085, "fat32" => 32 + 1024 + 65_525 }
                 .to_h { |variant, sectors| [variant, layout(variant, size: sectors * 512).clusters] }

    assert_equal({ "fat12" => 4084, "fat16" => 4085, "fat32" => 65_525 }, counts)
  end

  # 4168 sectors beside a 16-sector FAT would leave 4103 clusters, more
  # than its 4096 entries hold with the two reserved ones; a 17-sector FAT
  # leaves 4101 and holds them.
  def test_an_exact_size_takes_the_smallest_fat_that_holds_its_clusters
    geometry = layout("fat16", size: 4168 * 512).geometry

    assert_equal [17, 4101], [geometry.fat_size, geometry.cluster_count]
  end

  # Automatic sizing pads FAT16 and FAT32 to the margin above their
  # thresholds, and FAT12 lays out what the tree takes.
  def test_automatic_sizing_takes_the_least_count_the_margin_admits
    counts = %w[fat12 fat16 fat32].to_h { [_1, layout(_1).clusters] }

    assert_equal({ "fat12" => 1, "fat16" => 4101, "fat32" => 65_541 }, counts)
  end

  # A long name takes a long-name slot per 13 UTF-16 units before its
  # short entry; an upper-case 8.3 name takes its short entry alone.
  # DIR holds . and .., the long name's two slots and short entry, the
  # short name's entry and a terminator: 7 slots, 224 bytes, one cluster;
  # the 1000-byte file takes two, the empty one none, and the FAT32 root
  # its label, DIR and its terminator in one more.
  def test_the_tree_takes_its_files_and_directory_slots
    tree = [FsimgSpec::TreeEntry.directory("DIR"), FsimgSpec::TreeEntry.file("DIR/a long name.txt", size: 1000, content: "zero"),
            FsimgSpec::TreeEntry.file("DIR/SHORT.TXT", size: 0, content: "zero")]
    allocated = %w[fat12 fat32].to_h { [_1, layout(_1, tree:).allocated] }

    assert_equal({ "fat12" => 3, "fat32" => 4 }, allocated)
  end

  # The root region holds at least 512 entries and grows by whole sectors:
  # 600 short names and the label and terminator take 602 slots, 38
  # sectors of 16.
  def test_a_wide_root_grows_by_whole_sectors
    tree = (1..600).map { FsimgSpec::TreeEntry.file(format("F%04d", _1), size: 0, content: "zero") }

    assert_equal 608, Fat::Sizing.root_entries(source("fat16", tree:))
  end

  # 4070 clusters of 512 bytes are past the FAT12 margin; without a
  # cluster size create may choose 1024 bytes, where they are 2035.
  def test_a_count_past_the_margin_gives_way_to_a_larger_cluster
    tree = [FsimgSpec::TreeEntry.file("Z", size: 4070 * 512, content: "zero")]
    chosen = [{ cluster: 512 }, {}].map { Fat::Sizing.plan(source("fat12", tree:, settings: _1))&.cluster_bytes }

    assert_equal [nil, 1024], chosen
  end

  # A created image one cluster short of its layout is a problem of
  # create, named field by field, which the FAT creation case's created
  # hook reports: the independent builder renders FAT16 with the writer's
  # reserved sector, root region and FATs, so only the count and the
  # total differ, and its 4101 clusters are the planned volume.
  def test_a_created_image_off_its_layout_fails_its_creation
    witness = Fat::Witnesses.creations(Fat::Rules.registry, ceilings: CEILINGS).find { _1.id == "fat.create.valid.fat16" }
    with_cases do |cases|
      created = cases.creation_witness(witness).created
      problems = [4100, 4101].map { |clusters| built(clusters) { |path| created.call(path).last } }

      assert_equal ["the image it wrote has total sectors 4167 where the writer's sizing gives 4168, clusters 4100 " \
                    "where the writer's sizing gives 4101", nil], problems
    end
  end

  private

  # Yields the path of an empty FAT16 volume of clusters the independent
  # builder renders.
  def built(clusters)
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image")
      Fat::Builder.write(path, parameters: { clusters: }, tree: [], max_bytes: CEILINGS.image)
      yield path
    end
  end

  def with_cases
    Dir.mktmpdir do |directory|
      Dir.mkdir(File.join(directory, "trials"))
      yield Fat::Cases.new(registry: Fat::Rules.registry, cache: FsimgSpec::FixtureCache.new(File.join(directory, "cache")),
                           ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials.new(File.join(directory, "trials")), shell: nil)
    end
  end

  def source(variant, tree: ONE_FILE, settings: {}) = FsimgSpec::SourceModel.new(format: variant, settings:, tree:)

  def layout(variant, tree: ONE_FILE, **settings) = Fat::Sizing.plan(source(variant, tree:, settings:))
end
