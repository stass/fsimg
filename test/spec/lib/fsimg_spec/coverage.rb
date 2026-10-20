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
      registry.rules.sort_by(&:id).map do |rule|
        generated = entries.select { _1.rules.include?(rule.id) }
        rule_entry(rule, generated:, exercising: generated.select { exercises?(_1, rule) })
      end
    end

    # The counts over every case of the run.
    def self.cases(entries) = {
      total: entries.size,
      **VERDICTS.to_h { |verdict| [verdict.to_sym, entries.count { _1.result.verdict == verdict }] }
    }

    # The counts of one rule over the cases generated for it, and those of
    # them that exercise it. valid_witnesses, attempted and the verdicts
    # count cases; targeted_witnesses and isolation are states. The witness
    # fields describe what was generated; the state, attempts, verdicts and
    # asserted stages what exercised the rule. A case never classified has
    # no kind, so it counts toward neither witness field.
    private_class_method def self.rule_entry(rule, generated:, exercising:)
      recipes = generated.filter_map { _1.kase&.recipe }
      targeted = recipes.select { _1.kind == "targeted" }
      ran = exercising.select { ran?(_1) }
      {
        id: rule.id, category: rule.category, policy: rule.policy, scopes: rule.scopes, status: rule.status&.to_s,
        state: state(exercising, ran),
        valid_witnesses: recipes.count { _1.kind == "valid" },
        targeted_witnesses: targeted.empty? ? "not_generated" : "available",
        isolation: isolation(targeted),
        attempted: exercising.size,
        **VERDICTS.to_h { |verdict| [verdict.to_sym, exercising.count { _1.result.verdict == verdict }] },
        stages_asserted: asserted_stages(ran),
        witnesses: generated.map(&:id).sort,
        promoted: generated.map(&:id).select { _1.start_with?(Recipe::PROMOTED_PREFIX) }.sort
      }
    end

    # Whether a case generated for rule exercises it. A destination rule's
    # violations depend on the host's name trials, so its witness
    # exercises it only on a host whose trials violate it; on any other
    # host it runs as an ordinary case, and the rule is not exercised there
    # (specs/design-spec-testing.md#host-names). A case never classified
    # still counts, so that its harness error shows under the rule.
    private_class_method def self.exercises?(entry, rule) =
      !rule.scopes.include?("destination") || entry.kase.nil? || entry.kase.classification.rule_ids.include?(rule.id)

    # Whether fsimg ran the case: a host skip, and a harness error, which
    # runs nothing or stops in the harness, did not.
    private_class_method def self.ran?(entry) =
      %w[passed failed].include?(entry.result.verdict) && !entry.result.stages.empty?

    # not_exercised: no case exercises the rule; predicate_only: cases, but fsimg ran none of
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
