# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require_relative "synthetic"

# A case's recorded classification is derived, never written by hand, and
# drift between a recipe and the current rules is reported.
class TestCase < Minitest::Test
  include Synthetic

  REGISTRY = FsimgSpec::Registry.new([
    Synthetic.rule("t.format", policy: "must_reject", scopes: ["open"], status: "FSIMG_EFORMAT"),
    Synthetic.rule("t.crosslink", policy: "must_reject", scopes: ["verification"], status: "FSIMG_ECORRUPT"),
    Synthetic.rule("t.collision", policy: "must_reject", scopes: ["destination"], status: "FSIMG_EEXISTS"),
    Synthetic.rule("t.free", policy: "equivalent_contents", scopes: [])
  ])

  def test_recorded_recipe_carries_the_derived_classification
    recorded = derive(recipe(intended: "t.format"), "t.format" => true).recorded

    assert_equal [FsimgSpec::ViolationRecord.new(rule: "t.format", scope: "open")], recorded.actual_violations
    assert_equal "single_rule", recorded.isolation
    assert_equal %w[reject:FSIMG_EFORMAT reject:FSIMG_EFORMAT], recorded.expected.values_at("info", "verify").map(&:to_s)
  end

  def test_targeted_case_violating_another_rule_too_is_multi_rule
    targeted = derive(recipe(intended: "t.format"), "t.format" => true, "t.crosslink" => true)

    assert_equal "multi_rule", targeted.isolation
    assert_nil targeted.harness_defect
  end

  def test_recorded_recipe_shows_no_drift_and_an_edited_one_does
    recorded = derive(recipe(intended: "t.format"), "t.format" => true).recorded
    edited = recorded.with(expected: recorded.expected.merge("info" => FsimgSpec::Expectation.parse("accept")))

    assert_empty derive(recorded, "t.format" => true).drift
    assert_equal ["expected: recorded info=accept verify=reject:FSIMG_EFORMAT, " \
                  "derived info=reject:FSIMG_EFORMAT verify=reject:FSIMG_EFORMAT"],
                 derive(edited, "t.format" => true).drift
  end

  def test_case_meant_valid_that_violates_a_rule_is_a_harness_defect
    assert_match(/meant valid but violates t.crosslink/, derive(recipe, "t.crosslink" => true).harness_defect)
  end

  def test_targeted_case_missing_its_rule_is_a_harness_defect
    assert_match(/does not violate its intended rule t.format/, derive(recipe(intended: "t.format")).harness_defect)
  end

  # A destination collision depends on the host: it decides extract's
  # expectation on this host, but the recipe never records it.
  def test_destination_violation_shapes_the_run_but_not_the_recording
    collided = derive(recipe(stages: %w[info extract]), "t.collision" => true)

    assert_equal "reject:FSIMG_EEXISTS", collided.expected.fetch("extract").to_s
    assert_empty collided.recorded.actual_violations
    assert_equal "accept", collided.recorded.expected.fetch("extract").to_s
    assert_nil collided.harness_defect
  end

  # A name-collision witness targets a destination rule. Whether it
  # collides depends on the host, so a miss is no generator defect and the
  # isolation is the same on every host.
  def test_destination_targeted_case_is_judged_by_the_image_alone
    witness = recipe(intended: "t.collision", stages: %w[info extract])
    runs = [derive(witness), derive(witness, "t.collision" => true)]

    assert_equal [nil, nil], runs.map(&:harness_defect)
    assert_equal %w[single_rule single_rule], runs.map(&:isolation)
  end

  # The model never finds a violation in a scope it could not decode, so
  # its absence there says nothing about the generator.
  def test_intended_rule_in_an_unclassified_scope_is_no_harness_defect
    classification = classify(REGISTRY, unclassified: ["verification"])

    assert_nil FsimgSpec::Case.derive(recipe(intended: "t.crosslink"), classification, registry: REGISTRY).harness_defect
  end

  # A general malformed case may violate anything, and only safety is
  # asserted at every stage.
  def test_malformed_case_asserts_only_safety
    malformed = derive(recipe(kind: "malformed"), "t.format" => true, "t.crosslink" => true)

    assert_nil malformed.harness_defect
    assert_nil malformed.isolation
    assert malformed.expected.values.all?(&:unclassified?)
    assert_equal 2, malformed.recorded.actual_violations.size
  end

  def test_recipe_kind_and_intended_violation_must_agree
    assert_raises(FsimgSpec::RecipeError) { recipe(intended: "t.format", kind: "valid") }
    assert_raises(FsimgSpec::RecipeError) { recipe(kind: "targeted") }
  end

  def test_valid_case_with_an_unclassified_scope_is_a_harness_defect
    classification = classify(REGISTRY, unclassified: ["files"])

    assert_match(/leaves files unclassified/, FsimgSpec::Case.derive(recipe, classification, registry: REGISTRY).harness_defect)
  end

  def test_targeted_case_its_stages_cannot_check_is_a_harness_defect
    assert_match(/runs no stage that checks/, derive(recipe(intended: "t.crosslink", stages: %w[info ls]), "t.crosslink" => true).harness_defect)
  end

  def test_targeted_case_naming_a_freedom_is_a_harness_defect
    assert_match(/a freedom of the format/, derive(recipe(intended: "t.free")).harness_defect)
  end

  # A creation case's reading stages follow the image create wrote, while
  # create keeps the source tree's expectation.
  def test_created_image_decides_the_reading_stages
    created = derive(creation_recipe).created(classify(REGISTRY, { "t.crosslink" => true }))

    assert_equal({ "create" => "accept", "verify" => "reject:FSIMG_ECORRUPT" }, created.expected.transform_values(&:to_s))
  end

  # recorded derives expectations from the classification the case was
  # derived with, which for a creation case is the source tree's rather
  # than the created image's, so it refuses a creation case, and so does
  # drift, which compares with it.
  def test_a_creation_case_is_not_recorded
    created = derive(creation_recipe).created(classify(REGISTRY, { "t.crosslink" => true }))

    assert_raises(FsimgSpec::Error) { created.recorded }
    assert_raises(FsimgSpec::Error) { created.drift }
  end

  private

  def recipe(intended: nil, kind: intended ? "targeted" : "valid", stages: %w[info verify]) = FsimgSpec::Recipe.new(
    id: "t.case", format: "fat16", stages:, kind:, intended_violation: intended,
    expected: stages.to_h { [_1, FsimgSpec::Expectation.parse("accept")] },
    base: FsimgSpec::Base.new(builder: "independent", parameters: {}, tree: []),
    base_sha256: "0" * 64, tool_versions: {}, mutations: []
  )

  def creation_recipe = FsimgSpec::Recipe.new(
    id: "t.create", format: "fat16", kind: "valid", stages: %w[create verify],
    source: FsimgSpec::Source.new(tree: [], settings: {}),
    expected: %w[create verify].to_h { [_1, FsimgSpec::Expectation.parse("accept")] }
  )

  def derive(recipe, facts = {}) = FsimgSpec::Case.derive(recipe, classify(REGISTRY, facts), registry: REGISTRY)
end
