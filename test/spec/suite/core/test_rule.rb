# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require_relative "synthetic"

# The registry refuses every rule set that would make an expectation
# ambiguous, and a rule reports its violations at the scopes it declares.
class TestRule < Minitest::Test
  include Synthetic

  VALID = { policy: "must_reject", scopes: ["open"], status: "FSIMG_EFORMAT" }.freeze

  def test_registry_accepts_every_category_and_policy_pairing_the_design_allows
    rules = FsimgSpec::CATEGORY_POLICIES.flat_map do |category, policies|
      policies.map do |policy|
        status = policy == "must_reject" ? "FSIMG_ECORRUPT" : nil
        scopes = policy == "equivalent_contents" ? [] : ["files"]
        rule("t.#{category}.#{policy}", category:, policy:, scopes:, status:)
      end
    end

    assert_equal 7, FsimgSpec::Registry.new(rules).rules.size
  end

  def test_registry_refuses_each_inconsistent_rule
    refusals = {
      "duplicate id" => [rule("t.same", **VALID), rule("t.same", **VALID)],
      "malformed id" => [rule("Bad", **VALID)],
      "normative optional_feature" => [rule("t.r", category: "normative", policy: "optional_feature", scopes: ["open"])],
      "advisory must_reject" => [rule("t.r", **VALID, category: "advisory")],
      "unknown scope" => [rule("t.r", **VALID, scopes: ["nowhere"])],
      "no scope" => [rule("t.r", **VALID, scopes: [])],
      "must_reject without status" => [rule("t.r", **VALID, status: nil)],
      "safety_only with status" => [rule("t.r", policy: "safety_only", scopes: ["open"], status: "FSIMG_ECORRUPT")],
      "EEXISTS where info checks" => [rule("t.r", **VALID, status: "FSIMG_EEXISTS")],
      "EINVAL at a reading stage" => [rule("t.r", **VALID, status: "FSIMG_EINVAL")],
      "unknown related rule" => [rule("t.r", **VALID, related: ["t.missing"])],
      "input rule with an image scope" => [rule("t.r", **VALID, scopes: %w[input open], status: "FSIMG_ERANGE")]
    }

    refusals.each do |reason, rules|
      assert_raises(FsimgSpec::RuleError, reason) { FsimgSpec::Registry.new(rules) }
    end
  end

  def test_status_names_a_resource_exactly_when_it_is_a_limit
    assert_equal "FSIMG_ELIMIT:entries", FsimgSpec::Status.parse("FSIMG_ELIMIT:entries").to_s
    %w[FSIMG_ELIMIT FSIMG_ELIMIT:bogus FSIMG_EFORMAT:entries FSIMG_EBOGUS].each do |text|
      assert_raises(FsimgSpec::Error, text) { FsimgSpec::Status.parse(text) }
    end
  end

  # An input rule reads a creation case's source model and every other
  # rule an image's model, so each is classified by its own rules alone.
  def test_input_rules_classify_the_source_and_the_others_the_image
    registry = FsimgSpec::Registry.new([rule("t.name", policy: "must_reject", scopes: ["input"], status: "FSIMG_EINVAL"),
                                        rule("t.format", **VALID)])
    model = Model.new(facts: { "t.name" => true, "t.format" => true })

    assert_equal [["t.format"], ["t.name"]], [registry.classify(model, nil).rule_ids, registry.classify_source(model, nil).rule_ids]
  end

  def test_registry_accepts_a_collision_status_only_at_the_destination
    registry = FsimgSpec::Registry.new([rule("t.collision", **VALID, scopes: ["destination"], status: "FSIMG_EEXISTS")])

    assert_equal %w[extract], registry.fetch("t.collision").stages
  end

  def test_rule_reports_each_violation_at_the_scope_the_predicate_names
    registry = FsimgSpec::Registry.new([rule("t.chain", policy: "must_reject", scopes: %w[files verification], status: "FSIMG_ECORRUPT")])

    violations = classify(registry, { "t.chain" => "verification" }).violations

    assert_equal [{ rule: "t.chain", scope: "verification" }], violations.map(&:to_h)
  end

  def test_rule_refuses_a_scope_it_does_not_declare
    registry = FsimgSpec::Registry.new([rule("t.chain", policy: "must_reject", scopes: %w[files verification], status: "FSIMG_ECORRUPT")])

    assert_raises(FsimgSpec::RuleError) { classify(registry, { "t.chain" => "open" }) }
  end

  def test_rule_whose_inputs_are_absent_is_not_evaluated
    never_called = ->(*) { flunk("predicate ran without its inputs") }
    rule = FsimgSpec::Rule.new(id: "t.absent", category: "normative", policy: "must_reject", scopes: ["open"],
                               status: "FSIMG_EFORMAT", source: "test", applies_to: ->(*) { false },
                               predicate: never_called)

    assert_empty rule.violations(Model.new, nil)
  end

  # A predicate or applicability test over an undefined input answers
  # Undefined, and the rule does not apply.
  def test_rule_answering_undefined_does_not_apply
    undefined = ->(*) { FsimgSpec::Undefined.new(reason: "absent") }
    [[undefined, ->(*) { false }], [->(*) { true }, undefined]].each do |applies_to, predicate|
      rule = FsimgSpec::Rule.new(id: "t.undefined", category: "normative", policy: "must_reject", scopes: ["open"],
                                 status: "FSIMG_EFORMAT", source: "test", applies_to:, predicate:)

      assert_empty rule.violations(Model.new, nil)
    end
  end

  def test_domain_witnesses_cover_legal_values_invalid_examples_and_boundary_neighbours
    domain = FsimgSpec::Domain.new(legal: [512, 1024], invalid: [0, 1000], boundaries: [4085])

    assert_equal [0, 512, 1000, 1024, 4084, 4085, 4086], domain.witnesses
    assert_equal [0, 1, 2, 9, 10, 11], FsimgSpec::Domain.new(legal: 1..10).witnesses
  end
end
