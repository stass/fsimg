# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Drawn trees through the real fsimg
# (specs/design-spec-testing.md#generation-and-shrinking): the case phase
# runs the sample through every reading stage against its known tree,
# counts it as drawn and leaves none of its images behind, and a drawn
# recipe, with the seed it records removed, replays to the same case and
# the same verdict.
class TestFatDrawnFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick").with(tree_draws: 6, tree_sample: 3)
  SEED = 4242

  # The FAT suite cut to its drawn cases.
  module DrawnSuite
    def self.registry = Fat::Suite.registry

    def self.preflight_sample(directory) = Fat::Suite.preflight_sample(directory)

    def self.cases(**arguments) = Fat::Suite.cases(**arguments).select { _1.id.start_with?("fat.drawn.") }
  end

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  def test_the_sample_runs_through_fsimg_and_leaves_no_image
    with_results do |root, results|
      phase = FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory: root, environment: ENV.to_h,
                                        seed: SEED, suites: { "fat" => DrawnSuite })

      assert_equal({ total: 3, passed: 3, drawn: 3 }, phase.formats.fetch("fat").cases.slice(:total, :passed, :drawn))
      assert_empty Dir.children(File.join(results, "drawn"))
    end
  end

  def test_a_drawn_recipe_replays_without_its_seed
    with_results do |root, results|
      setup = FsimgSpec::CasePhase.prepare(results:, profile: PROFILE, build_directory: root, environment: ENV.to_h)
      arguments = { cache: setup.cache, ceilings: PROFILE.ceilings, name_trials: setup.name_trials, shell: setup.shell }
      draws = FsimgSpec::Draws.new(seed: SEED, count: 1, sample: 1, cache: FsimgSpec::FixtureCache.new(File.join(results, "drawn")))
      classified = DrawnSuite.cases(**arguments, draws:).first.classify.call
      recipe = classified.kase.recipe
      assert_equal SEED, recipe.random_seed

      unseeded = FsimgSpec::Recipe.load(recipe.with(random_seed: nil).dump, ceilings: PROFILE.ceilings)
      replayed = Fat::Suite.replay(unseeded, **arguments).classify.call
      assert_equal unseeded, replayed.kase.recipe
      result = run_case(replayed, setup:, directory: File.join(results, "cases"))
      assert_equal ["passed", FsimgSpec::READING_STAGES], [result.verdict, result.stages.map(&:stage)]
    end
  end

  private

  def with_results
    Dir.mktmpdir do |root|
      results = File.join(root, "results")
      Dir.mkdir(results)
      yield root, results
    end
  end

  def run_case(classified, setup:, directory:)
    flunk classified.defect if classified.defect
    FsimgSpec::CasePhase.result_of(classified, setup:, cases_directory: directory)
  end
end
