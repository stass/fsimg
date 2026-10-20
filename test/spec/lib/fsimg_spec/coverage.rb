# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # One case of a run as the report counts it: its id, the rules it counts
  # toward (the rule whose domain generated it and the rule it targets), its
  # Case or nil when it was never classified, and its CaseResult.
  CaseEntry = Data.define(:id, :rules, :kase, :result)

  # The per-rule half of coverage.json (specs/design-spec-testing.md#reports-and-regressions).
  # A rule's cases are the cases its domain generated and the targeted
  # cases that name it. A declared rule never counts as covered until fsimg
  # has run one of its cases with an expectation the model could derive.
  module Coverage
    VERDICTS = %w[passed failed host_skip harness_error].freeze

    # Every rule of registry with its counts, in rule id order.
    def self.rules(registry, entries)
      registry.rules.sort_by(&:id).map { |rule| rule_entry(rule, entries.select { belongs?(_1, rule) }) }
    end

    # The counts over every case of the run.
    def self.cases(entries) = {
      total: entries.size,
      **VERDICTS.to_h { |verdict| [verdict.to_sym, entries.count { _1.result.verdict == verdict }] }
    }

    # The counts of one rule. valid_witnesses, attempted and the verdicts
    # count cases; targeted_witnesses and isolation are states. A case
    # never classified has no kind, so it counts toward neither witness
    # field.
    private_class_method def self.rule_entry(rule, entries)
      recipes = entries.filter_map { _1.kase&.recipe }
      targeted = recipes.select { _1.kind == "targeted" }
      ran = entries.select { ran?(_1) }
      {
        id: rule.id, category: rule.category, policy: rule.policy, scopes: rule.scopes, status: rule.status&.to_s,
        state: state(entries, ran),
        valid_witnesses: recipes.count { _1.kind == "valid" },
        targeted_witnesses: targeted.empty? ? "not_generated" : "available",
        isolation: isolation(targeted),
        attempted: entries.size,
        **VERDICTS.to_h { |verdict| [verdict.to_sym, entries.count { _1.result.verdict == verdict }] },
        stages_asserted: asserted_stages(ran),
        witnesses: entries.map(&:id).sort,
        promoted: entries.map(&:id).select { _1.start_with?(Recipe::PROMOTED_PREFIX) }.sort
      }
    end

    private_class_method def self.belongs?(entry, rule) = entry.rules.include?(rule.id)

    # Whether fsimg ran the case: a host skip, and a harness error, which
    # runs nothing or stops in the harness, did not.
    private_class_method def self.ran?(entry) =
      %w[passed failed].include?(entry.result.verdict) && !entry.result.stages.empty?

    # not_exercised: no case; predicate_only: cases, but fsimg ran none of
    # them; unclassified: fsimg ran some, but no stage had a derived
    # expectation; asserted otherwise.
    private_class_method def self.state(entries, ran)
      if entries.empty? then "not_exercised"
      elsif ran.empty? then "predicate_only"
      elsif asserted_stages(ran).empty? then "unclassified"
      else "asserted"
      end
    end

    # The stages fsimg ran with an expectation the model derived.
    private_class_method def self.asserted_stages(ran)
      asserted = ran.flat_map do |entry|
        entry.result.stages.map(&:stage).reject { |stage| entry.kase.expected.fetch(stage).unclassified }
      end
      asserted.uniq.sort_by { STAGES.index(_1) }
    end

    # single_rule when every targeted witness isolates the rule, partial
    # when some do, multi_rule_only when none do; nil without one.
    private_class_method def self.isolation(targeted)
      return nil if targeted.empty?

      single = targeted.count { _1.isolation == "single_rule" }
      if single == targeted.size then "single_rule"
      elsif single.positive? then "partial"
      else "multi_rule_only"
      end
    end
  end
end
