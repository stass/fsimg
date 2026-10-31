# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The drawn trees of a run (specs/design-spec-testing.md#generation-and-shrinking):
# every name kind and file size class the design lists, within the
# generator's bounds and the profile's ceilings; every draw a valid
# witness, classified in memory; and a generator that emits a name the
# reader refuses reported as a generator defect.
class TestFatTrees < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  SEED = 20_261_020

  def test_draws_cover_every_name_kind_and_size_class_within_their_bounds
    draws = Fat::Trees.draw(seed: SEED, count: PROFILE.tree_draws)
    names = draws.flat_map { |witness| witness.tree.map { _1.path.split("/").last } }

    draws.each { assert_within_bounds(_1) }
    assert_equal({ "fat12" => 50, "fat16" => 50, "fat32" => 50 }, draws.map { Fat.variant_of(_1.parameters.fetch(:clusters)) }.tally)
    kinds = names.map { kind(_1) }.tally
    assert_equal %i[short long bmp non_bmp device], kinds.keys.sort_by { %i[short long bmp non_bmp device].index(_1) }
    assert(draws.any? { reserved_pair?(_1.tree) }, "no long name beside the literal its short name would take")
    draws.group_by(&:parameters).each do |parameters, witnesses|
      cluster = Fat::Builder::Layout.new(parameters).cluster_bytes
      classes = witnesses.flat_map(&:tree).select { _1.type == "file" }.map { size_class(_1.size, cluster) }.uniq.sort
      assert_equal %i[cluster cluster_less_one empty one several], classes, parameters.inspect
    end
  end

  # A tree mtools populates draws only names mtools writes as drawn: none
  # beyond the BMP, which it would drop unseen, and no device name
  # without an extension, which it refuses. Device names with one remain.
  def test_populated_trees_draw_only_names_mtools_writes
    draws = Fat::Trees.draw(seed: SEED, count: PROFILE.tree_draws, generator: Fat::Trees.method(:populated_tree))
    names = draws.flat_map { |witness| witness.tree.map { _1.path.split("/").last } }

    draws.each { assert_within_bounds(_1) }
    assert_equal %i[short long bmp device], names.map { kind(_1) }.uniq.sort_by { %i[short long bmp device].index(_1) }
    assert(names.select { kind(_1) == :device }.all? { _1.include?(".") }, "a device name without an extension")
  end

  # A tree's draw follows the seed alone, so the run's SEED repeats it.
  def test_one_seed_draws_one_set_of_trees
    assert_equal Fat::Trees.draw(seed: SEED, count: 6), Fat::Trees.draw(seed: SEED, count: 6)
    refute_equal Fat::Trees.draw(seed: SEED, count: 6), Fat::Trees.draw(seed: SEED + 1, count: 6)
  end

  # Classified in memory, every draw is a valid witness: no rule refuses
  # its names and the builder lays it out.
  def test_every_draw_classifies_valid_in_memory
    with_cases do |cases|
      defective = Fat::Trees.draw(seed: SEED, count: PROFILE.tree_draws).select { cases.defective?(_1) }

      assert_empty defective.map(&:id)
    end
  end

  # A generator that adds a name ending in a space draws trees the
  # reader refuses. Every such draw joins the run beyond the sample, and
  # its case reports the generator defect rather than running fsimg.
  def test_a_generator_that_emits_a_trailing_space_name_is_a_generator_defect
    faulty = lambda { |cluster_bytes:|
      Fat::Trees.tree(cluster_bytes:).map { _1 + [FsimgSpec::TreeEntry.file("trailing ", size: 1, seed: 1)] }
    }
    with_cases do |_cases, cache, directory|
      draws = FsimgSpec::Draws.new(seed: SEED, count: 3, sample: 1, populated: 0, cache:)
      promoted = File.join(directory, "promoted")
      Dir.mkdir(promoted)
      drawn = Fat::Suite.cases(cache:, ceilings: PROFILE.ceilings, name_trials: FsimgSpec::NameTrials::NONE, shell: nil, draws:,
                               directory: promoted, tree_generator: faulty).select { _1.id.start_with?("fat.drawn.") }
      defects = drawn.map { _1.classify.call.defect }

      assert_equal (0..2).map { "fat.drawn.#{%w[fat12 fat16 fat32][_1]}.#{SEED}.#{_1}" }, drawn.map(&:id)
      defects.zip(drawn) { |defect, pending| assert_equal "#{pending.id} is meant valid but violates fat.directory.name", defect }
    end
  end

  private

  def assert_within_bounds(witness)
    tree = witness.tree
    ceilings = PROFILE.ceilings
    assert_operator tree.size, :<=, Fat::Trees::ENTRIES
    assert_operator tree.map(&:depth).max, :<=, [Fat::Trees::DEPTH, ceilings.depth].min
    assert_operator tree.map { _1.path.bytesize }.max, :<=, [Fat::Trees::MAX_PATH_BYTES, ceilings.path].min
    assert_operator tree.group_by(&:parent).values.map(&:size).max, :<=, Fat::Trees::WIDTH
    order = tree.each_with_index.to_h { |entry, index| [entry.path, index] }
    assert(tree.all? { _1.parent.nil? || order.fetch(_1.parent) < order.fetch(_1.path) }, "a parent follows its entry")
  end

  # The kind of a drawn name, by its characters.
  def kind(name)
    if Fat::Trees::DEVICES.include?(name.upcase.partition(".").first) then :device
    elsif name.each_char.any? { _1.ord > 0xFFFF } then :non_bmp
    elsif !name.ascii_only? then :bmp
    elsif name.match?(/\A[^.]{1,8}(\.[^.]{1,3})?\z/) && !name.include?(" ") then :short
    else :long
    end
  end

  # Whether tree holds a long name beside the literal 8.3 name the basis
  # name algorithm's first numeric tail spells for it.
  def reserved_pair?(tree)
    short = Fat::Builder::ShortNames
    tree.group_by(&:parent).values.any? do |siblings|
      names = siblings.map { _1.path.split("/").last }
      names.any? { |name| name.include?(" ") && names.include?(short.spelled(short.numbered(*short.basis(name), 1))) }
    end
  end

  def size_class(size, cluster)
    case size
    in 0 then :empty
    in 1 then :one
    in Integer if size == cluster - 1 then :cluster_less_one
    in Integer if size == cluster then :cluster
    else :several
    end
  end

  def with_cases
    Dir.mktmpdir do |directory|
      cache = FsimgSpec::FixtureCache.new(File.join(directory, "cache"))
      yield Fat::Cases.new(registry: Fat::Rules.registry, cache:, ceilings: PROFILE.ceilings,
                           name_trials: FsimgSpec::NameTrials::NONE, shell: nil), cache, directory
    end
  end
end
