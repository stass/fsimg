# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"
require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Promoted regression recipes (specs/design-spec-testing.md#reports-and-regressions):
# each replays to the bytes and classification it recorded, and one whose
# recorded expectation no longer matches the rules fails as drifted.
class TestFatPromoted < Minitest::Test
  Fat = FsimgSpec::Fat
  CEILINGS = FsimgSpec.fetch_profile("quick").ceilings

  def test_every_promoted_recipe_replays_without_drift
    recipes = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: CEILINGS)
    refute_empty recipes

    with_cache do |cache|
      recipes.each do |recipe|
        classified = Fat::Suite.replay(recipe, cache:, ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials::NONE).classify.call

        assert_nil classified.defect, recipe.id
        assert_empty classified.kase.drift, recipe.id
      end
    end
  end

  # The gate's drift check: a promoted recipe whose recorded expectation
  # was edited by hand fails as a defect of the run.
  def test_edited_expectation_fails_the_drift_check
    recipe = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: CEILINGS).first
    edited = recipe.with(expected: recipe.expected.transform_values { FsimgSpec::Expectation.parse("accept") })
    Dir.mktmpdir do |directory|
      File.write(File.join(directory, "#{recipe.id}.json"), edited.dump)
      with_cache do |cache|
        promoted = Fat::Suite.cases(cache:, ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials::NONE,
                                    draws: FsimgSpec::Draws::NONE, directory:).find { _1.id == recipe.id }

        assert_match(/drifted: expected: recorded info=accept, derived info=reject:/, promoted.classify.call.defect)
      end
    end
  end

  # A rule renamed or removed without updating the promoted recipe that
  # names it leaves a recipe the rules cannot classify, which the case
  # phase records as that case's harness error and a replay as a recipe
  # this revision cannot judge.
  def test_recipe_naming_a_removed_rule_cannot_be_classified
    recipe = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: CEILINGS).first
    registry = FsimgSpec::Registry.new(Fat::Suite.registry.rules.reject { _1.id == recipe.intended_violation })
    with_cache do |cache|
      pending = Fat::Suite.replay(recipe, cache:, ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials::NONE, registry:)

      error = assert_raises(FsimgSpec::RecipeError) { FsimgSpec::CasePhase.classify(pending) }
      assert_match(/#{Regexp.escape(recipe.intended_violation)}/, error.message)
    end
  end

  # Only the independent builder renders a base without development tools:
  # a replay skips a recipe on any other base, and a promoted one is
  # refused, so that no regression case is silently never run.
  def test_recipe_on_a_formatter_base_is_skipped_and_never_promoted
    recipe = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: CEILINGS).first
    formatted = recipe.with(base: recipe.base.with(builder: "mkfs.fat"))
    with_cache do |cache|
      assert_match(/only independent bases replay without tools/, Fat::Suite.replay(formatted, cache:, ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials::NONE).skip)
    end
    Dir.mktmpdir do |directory|
      File.write(File.join(directory, "#{recipe.id}.json"), formatted.dump)

      error = assert_raises(FsimgSpec::RecipeError) { Fat::Suite.promoted(directory, ceilings: CEILINGS) }
      assert_match(/only independent bases replay without tools/, error.message)
    end
  end

  # Every entry of the promoted directory is a recipe, so a stray or
  # misnamed copy cannot drop out of the regression gate unnoticed.
  def test_promoted_directory_holds_only_recipes
    Dir.mktmpdir do |directory|
      File.write(File.join(directory, "regression.fat.x.json~"), "")

      assert_raises(FsimgSpec::RecipeError) { Fat::Suite.promoted(directory, ceilings: CEILINGS) }
    end
  end

  # A promoted creation recipe needs no base and replays without drift;
  # one whose recorded expectation was edited by hand fails as drifted.
  def test_edited_creation_expectation_fails_the_drift_check
    Dir.mktmpdir do |directory|
      { "accept" => "recorded", "reject:FSIMG_ECORRUPT" => "edited" }.each do |ls, name|
        File.write(File.join(directory, "regression.fat.create.#{name}.json"), promoted_creation(name, ls:).dump)
      end
      with_cache do |cache|
        edited, recorded = Fat::Suite.promoted(directory, ceilings: CEILINGS).map do
          Fat::Suite.replay(_1, cache:, ceilings: CEILINGS, name_trials: FsimgSpec::NameTrials::NONE).classify.call
        end

        assert_nil recorded.defect
        assert_match(/drifted: expected: recorded create=accept ls=reject:FSIMG_ECORRUPT, derived create=accept ls=accept/,
                     edited.defect)
      end
    end
  end

  def test_promoted_file_must_be_named_after_its_recipe
    recipe = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: CEILINGS).first
    Dir.mktmpdir do |directory|
      File.write(File.join(directory, "regression.other.json"), recipe.dump)

      assert_raises(FsimgSpec::RecipeError) { Fat::Suite.promoted(directory, ceilings: CEILINGS) }
    end
  end

  private

  def promoted_creation(name, ls:) = FsimgSpec::Recipe.new(
    id: "regression.fat.create.#{name}", format: "fat16", kind: "valid", stages: %w[create ls],
    source: FsimgSpec::Source.new(tree: [FsimgSpec::TreeEntry.new(path: "a.txt", type: "file", size: 5, seed: 1)], settings: {}),
    expected: { "create" => "accept", "ls" => ls }.transform_values { FsimgSpec::Expectation.parse(_1) },
    reason: "A creation recipe written for this test."
  )

  def with_cache
    Dir.mktmpdir { yield FsimgSpec::FixtureCache.new(_1) }
  end
end
