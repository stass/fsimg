# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require_relative "synthetic"

# Expectation derivation (specs/design-spec-testing.md#expectation-derivation)
# over synthetic rule sets: what each stage must do follows from the
# violations it checks and nothing else.
class TestExpectation < Minitest::Test
  include Synthetic

  REGISTRY = FsimgSpec::Registry.new([
    Synthetic.rule("t.format", policy: "must_reject", scopes: ["open"], status: "FSIMG_EFORMAT"),
    Synthetic.rule("t.corrupt", policy: "must_reject", scopes: ["open"], status: "FSIMG_ECORRUPT"),
    Synthetic.rule("t.crosslink", policy: "must_reject", scopes: ["verification"], status: "FSIMG_ECORRUPT"),
    Synthetic.rule("t.chain", policy: "must_reject", scopes: %w[files verification], status: "FSIMG_ECORRUPT"),
    Synthetic.rule("t.entries", policy: "must_reject", scopes: ["directories"], status: "FSIMG_ELIMIT:entries"),
    Synthetic.rule("t.feature", policy: "optional_feature", scopes: ["open"]),
    Synthetic.rule("t.ambiguous", policy: "safety_only", scopes: ["open"]),
    Synthetic.rule("t.hint", policy: "must_accept", scopes: ["open"]),
    Synthetic.rule("t.untested", policy: "diagnostic_only", scopes: ["verification"]),
    Synthetic.rule("t.collision", policy: "must_reject", scopes: ["destination"], status: "FSIMG_EEXISTS")
  ])

  def test_case_without_violations_is_accepted_everywhere
    assert_equal ["accept"] * 6, expectations({}).values
  end

  def test_open_violation_is_rejected_by_every_reading_stage
    expected = FsimgSpec::READING_STAGES.to_h { [_1, "reject:FSIMG_EFORMAT"] }

    assert_equal expected.merge("create" => "accept"), expectations({ "t.format" => true })
  end

  def test_verification_violation_is_rejected_only_by_verify
    assert_equal({ "info" => "accept", "ls" => "accept", "extract" => "accept",
                   "verify" => "reject:FSIMG_ECORRUPT", "verify-deep" => "reject:FSIMG_ECORRUPT", "create" => "accept" },
                 expectations({ "t.crosslink" => true }))
  end

  def test_violations_at_one_scope_allow_each_of_their_statuses
    assert_equal "reject:FSIMG_ECORRUPT|reject:FSIMG_EFORMAT",
                 expectations({ "t.format" => true, "t.corrupt" => true }).fetch("info")
  end

  def test_safety_only_violation_widens_a_rejection_to_any_verdict_status
    assert_equal "reject:*", expectations({ "t.format" => true, "t.ambiguous" => true }).fetch("info")
  end

  def test_safety_only_violation_alone_allows_either_outcome
    assert_equal "accept|reject:*", expectations({ "t.ambiguous" => true }).fetch("ls")
  end

  def test_diagnostic_only_violation_allows_either_outcome_where_it_is_checked
    assert_equal %w[accept accept|reject:*], expectations({ "t.untested" => true }).values_at("extract", "verify")
  end

  def test_optional_feature_is_rejected_as_unsupported
    assert_equal "reject:FSIMG_EUNSUPPORTED", expectations({ "t.feature" => true }).fetch("extract")
  end

  def test_must_accept_rule_keeps_its_case_accepted
    assert_equal ["accept"] * 6, expectations({ "t.hint" => true }).values
  end

  def test_limit_rejection_names_its_resource
    assert_equal "reject:FSIMG_ELIMIT:entries", expectations({ "t.entries" => true }).fetch("ls")
  end

  def test_per_violation_scope_decides_which_stages_reject
    at_files = expectations({ "t.chain" => "files" })
    at_verification = expectations({ "t.chain" => "verification" })

    assert_equal "reject:FSIMG_ECORRUPT", at_files.fetch("extract")
    assert_equal "accept", at_verification.fetch("extract")
    assert_equal "reject:FSIMG_ECORRUPT", at_verification.fetch("verify")
  end

  def test_destination_collision_is_rejected_only_by_extract
    assert_equal({ "info" => "accept", "ls" => "accept", "extract" => "reject:FSIMG_EEXISTS", "verify" => "accept",
                   "verify-deep" => "accept", "create" => "accept" },
                 expectations({ "t.collision" => true }))
  end

  def test_unclassified_scope_leaves_its_stages_without_an_expectation
    derived = expectations({ "t.format" => true }, unclassified: ["files"])

    assert_equal %w[reject:FSIMG_EFORMAT reject:FSIMG_EFORMAT], derived.values_at("info", "ls")
    assert_equal ["unclassified"] * 3, derived.values_at("extract", "verify", "verify-deep")
  end

  # Moving a rule from one scope to another changes the expectation of
  # exactly the stages that check one scope but not the other. The rule's
  # FSIMG_ERANGE is a verdict at every stage, so every scope can hold it.
  def test_moving_a_rule_scope_changes_exactly_the_stages_that_check_it
    scopes = FsimgSpec::SCOPE_STAGES.keys

    scopes.product(scopes).each do |from, to|
      changed = FsimgSpec::STAGES.reject do |stage|
        derive_with_scope(from, stage) == derive_with_scope(to, stage)
      end
      from_stages = FsimgSpec::SCOPE_STAGES.fetch(from)
      to_stages = FsimgSpec::SCOPE_STAGES.fetch(to)
      moved = (from_stages - to_stages) + (to_stages - from_stages)

      assert_equal moved.sort, changed.sort, "#{from} -> #{to}"
    end
  end

  # The gate of stage S2: generated cases follow a domain. Each witness of
  # the domain becomes a case; changing the domain's legal values changes
  # the derived expectation of exactly the witnesses that changed sides.
  def test_changing_a_domain_changes_the_derived_expectations
    narrow = FsimgSpec::Domain.new(legal: [512, 1024], boundaries: [512])
    wide = narrow.with(legal: [512, 1024, 2048])

    narrow_expected = domain_expectations(narrow, [511, 512, 513, 1024, 2048])
    wide_expected = domain_expectations(wide, [511, 512, 513, 1024, 2048])

    assert_equal({ 511 => "reject:FSIMG_EFORMAT", 512 => "accept", 513 => "reject:FSIMG_EFORMAT",
                   1024 => "accept", 2048 => "reject:FSIMG_EFORMAT" }, narrow_expected)
    assert_equal narrow_expected.merge(2048 => "accept"), wide_expected
  end

  def test_expectation_text_round_trips_and_refuses_unknown_outcomes
    %w[accept reject:* accept|reject:* reject:FSIMG_ELIMIT:depth unclassified].each do |text|
      assert_equal text, FsimgSpec::Expectation.parse(text).to_s
    end
    %w[maybe reject:FSIMG_EBOGUS accept|unclassified reject].each do |text|
      assert_raises(FsimgSpec::Error, text) { FsimgSpec::Expectation.parse(text) }
    end
  end

  private

  def expectations(facts, unclassified: [])
    classification = classify(REGISTRY, facts, unclassified:)
    FsimgSpec::STAGES.to_h { [_1, FsimgSpec::Expectation.derive(classification, _1).to_s] }
  end

  # An input rule classifies a source model, any other rule an image.
  def derive_with_scope(scope, stage)
    registry = FsimgSpec::Registry.new([rule("t.moved", policy: "must_reject", scopes: [scope], status: "FSIMG_ERANGE")])
    model = Synthetic::Model.new(facts: { "t.moved" => true })
    classification = scope == "input" ? registry.classify_source(model, nil) : registry.classify(model, nil)
    FsimgSpec::Expectation.derive(classification, stage)
  end

  # The info expectation of a case whose sector size is each value, under
  # a rule whose legal values are the domain's.
  def domain_expectations(domain, values)
    sector_rule = FsimgSpec::Rule.new(
      id: "t.sector_size", category: "normative", policy: "must_reject", scopes: ["open"], status: "FSIMG_EFORMAT",
      domain:, source: "test", applies_to: ->(*) { true },
      predicate: ->(model, _) { domain.legal?(model.facts.fetch(:sector_size)) }
    )
    registry = FsimgSpec::Registry.new([sector_rule])
    values.to_h do |value|
      [value, FsimgSpec::Expectation.derive(classify(registry, { sector_size: value }), "info").to_s]
    end
  end
end
