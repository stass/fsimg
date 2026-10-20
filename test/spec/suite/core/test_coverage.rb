# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require_relative "synthetic"

# The coverage report keeps a rule's states apart: declared but not
# exercised, generated but never run by fsimg, run without a derived
# expectation, and asserted against fsimg; and it reports how well the rule's targeted
# witnesses isolate it (specs/design-spec-testing.md#reports-and-regressions).
class TestCoverage < Minitest::Test
  REGISTRY = FsimgSpec::Registry.new([
    Synthetic.rule("t.format", policy: "must_reject", scopes: ["open"], status: "FSIMG_EFORMAT"),
    Synthetic.rule("t.crosslink", policy: "must_reject", scopes: ["verification"], status: "FSIMG_ECORRUPT"),
    Synthetic.rule("t.unused", policy: "must_reject", scopes: ["open"], status: "FSIMG_ECORRUPT")
  ])

  def test_each_rule_reports_its_state
    entries = [
      entry("t.format.0", intended: "t.format", facts: { "t.format" => true }, verdict: "passed"),
      entry("t.crosslink.0", intended: "t.crosslink", facts: { "t.crosslink" => true }, verdict: "harness_error",
                             unclassified: ["verification"])
    ]

    assert_equal({ "t.crosslink" => "predicate_only", "t.format" => "asserted", "t.unused" => "not_exercised" },
                 rules(entries).to_h { [_1[:id], _1[:state]] })
  end

  # A case whose only stage checks a scope the model left undecoded asserts
  # nothing about the rule.
  def test_run_without_a_derived_expectation_is_unclassified
    entries = [entry("t.crosslink.0", intended: "t.crosslink", facts: { "t.crosslink" => true }, verdict: "passed",
                                      unclassified: ["verification"], stages: ["verify"])]

    assert_equal "unclassified", rules(entries).find { _1[:id] == "t.crosslink" }[:state]
  end

  # A rule whose cases fsimg never ran, because the host skipped them or
  # the harness failed them, has only its predicate tested, however many
  # cases it was attempted with.
  def test_cases_fsimg_never_ran_test_only_the_predicate
    entries = [
      entry("t.format.0", intended: "t.format", facts: { "t.format" => true }, verdict: "host_skip"),
      entry("t.format.1", intended: "t.format", facts: { "t.format" => true }, verdict: "harness_error")
    ]
    format = rules(entries).find { _1[:id] == "t.format" }

    assert_equal({ state: "predicate_only", attempted: 2, host_skip: 1, harness_error: 1, stages_asserted: [] },
                 format.slice(:state, :attempted, :host_skip, :harness_error, :stages_asserted))
  end

  # A case that could not be classified has no recipe, so it has no kind:
  # it counts toward its rules' cases but toward neither witness field.
  def test_an_unclassified_case_counts_without_a_kind
    unclassified = FsimgSpec::CaseEntry.new(id: "t.format.0", rules: ["t.format"], kase: nil,
                                            result: result("t.format.0", "harness_error", []))
    format = rules([unclassified]).find { _1[:id] == "t.format" }

    assert_equal({ state: "predicate_only", valid_witnesses: 0, targeted_witnesses: "not_generated", attempted: 1,
                   harness_error: 1, witnesses: ["t.format.0"] },
                 format.slice(:state, :valid_witnesses, :targeted_witnesses, :attempted, :harness_error, :witnesses))
  end

  def test_isolation_summarizes_the_targeted_witnesses
    single = entry("t.format.0", intended: "t.format", facts: { "t.format" => true }, verdict: "passed")
    multi = entry("t.format.1", intended: "t.format", facts: { "t.format" => true, "t.crosslink" => true }, verdict: "passed")

    assert_equal %w[single_rule partial multi_rule_only],
                 [[single], [single, multi], [multi]].map { rules(_1).find { |rule| rule[:id] == "t.format" }[:isolation] }
  end

  # A case counts for the rule whose domain generated it, whatever its id
  # says: a valid case named under t.format that t.crosslink generated is
  # t.crosslink's.
  def test_a_case_counts_for_the_rule_that_generated_it
    entries = [entry("t.format.aligned", intended: nil, facts: {}, verdict: "passed", rule: "t.crosslink")]
    witnesses = rules(entries).to_h { [_1[:id], _1[:witnesses]] }

    assert_equal [[], ["t.format.aligned"]], witnesses.values_at("t.format", "t.crosslink")
  end

  # The report is ordered by rule and case id, whatever the order cases ran
  # in, and counts every case of the run by verdict.
  def test_counts_and_order_do_not_depend_on_execution_order
    entries = [
      entry("t.format.1", intended: "t.format", facts: { "t.format" => true }, verdict: "failed"),
      entry("t.format.0", intended: "t.format", facts: { "t.format" => true }, verdict: "passed"),
      entry("t.format.2", intended: "t.format", facts: { "t.format" => true }, verdict: "host_skip")
    ]
    format = rules(entries).find { _1[:id] == "t.format" }

    assert_equal rules(entries), rules(entries.reverse)
    assert_equal %w[t.format.0 t.format.1 t.format.2], format[:witnesses]
    assert_equal({ attempted: 3, passed: 1, failed: 1, host_skip: 1 }, format.slice(:attempted, :passed, :failed, :host_skip))
    assert_equal({ total: 3, passed: 1, failed: 1, host_skip: 1, harness_error: 0 },
                 FsimgSpec::Coverage.cases(entries))
  end

  private

  def rules(entries) = FsimgSpec::Coverage.rules(REGISTRY, entries)

  # A case of rule, targeting intended or valid when intended is nil, under
  # facts, which ended with verdict after running stages; fsimg runs no
  # stage of a host skip or a harness error.
  def entry(id, intended:, facts:, verdict:, unclassified: [], stages: ["info"], rule: intended)
    classification = Synthetic.classify(REGISTRY, facts, unclassified:)
    recipe = FsimgSpec::Recipe.new(
      id:, format: "fat16", kind: intended ? "targeted" : "valid", intended_violation: intended, stages:,
      expected: stages.to_h { [_1, FsimgSpec::Expectation.derive(classification, _1)] },
      base: FsimgSpec::Base.new(builder: "independent", parameters: {}, tree: []),
      base_sha256: "0" * 64, tool_versions: {}, mutations: []
    )
    kase = FsimgSpec::Case.derive(recipe, classification, registry: REGISTRY)
    kase = kase.with(recipe: kase.recorded)
    FsimgSpec::CaseEntry.new(id:, rules: [rule, intended].compact.uniq, kase:,
                             result: result(id, verdict, ran?(verdict) ? stages : []))
  end

  def ran?(verdict) = %w[passed failed].include?(verdict)

  def result(id, verdict, stages) =
    FsimgSpec::CaseResult.new(id:, verdict:, detail: nil, stages: stages.map do |stage|
      FsimgSpec::StageResult.new(stage:, outcome: FsimgSpec::Outcome.new(kind: "accepted"), execution: nil, problems: [], tree: [])
    end)
end
