# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # A rule definition the registry refuses.
  class RuleError < Error; end

  # Which policies each category may carry (specs/design-spec-testing.md#rules).
  # equivalent_contents states a freedom of the format and has no violations.
  CATEGORY_POLICIES = {
    "normative" => %w[must_reject safety_only equivalent_contents],
    "profile" => %w[must_reject optional_feature],
    "advisory" => %w[must_accept diagnostic_only]
  }.transform_values(&:freeze).freeze

  # The legal values of a field, its explicit invalid examples and its
  # boundaries. legal is a Range or an Array. A Range's ends are boundaries
  # of their own; an Array is small enough to enumerate. Every value is an
  # Integer, so the witnesses sort and have neighbours.
  class Domain < Data.define(:legal, :invalid, :boundaries)
    include Checked

    def initialize(legal:, invalid: [], boundaries: [])
      values = (legal.is_a?(Range) ? [legal.begin, legal.end] : legal) + invalid + boundaries
      raise RuleError, "a domain holds integers only" unless values.all?(Integer)

      super
    end

    def legal?(value) = legal.include?(value)

    # The deterministic witnesses: every enumerable legal value, every invalid
    # example, and n-1, n and n+1 at every boundary.
    def witnesses
      enumerated = legal.is_a?(Range) ? [] : legal
      neighbours = all_boundaries.flat_map { [_1 - 1, _1, _1 + 1] }
      (enumerated + invalid + neighbours).uniq.sort
    end

    private def all_boundaries = legal.is_a?(Range) ? boundaries + [legal.begin, legal.end] : boundaries
  end

  # One violation of a rule, with the scope of the check that must find it.
  Violation = Data.define(:rule, :scope) do
    def to_h = { rule: rule.id, scope: }
  end

  # A named, cited statement about images or creation input. applies_to and
  # predicate are called with a model and the run environment: the decoded
  # image's, or for a rule of the input scope a creation case's SourceModel.
  # The predicate runs only when applies_to holds. A rule with one scope has a
  # predicate that answers whether the rule holds; a rule with several
  # reports the scope of each violation, and an empty list when it holds.
  # Either may answer Undefined, as FsimgSpec.derive does when an input is
  # undefined, and the rule then does not apply: a rule applies only when
  # every input it reads is defined, without listing those inputs twice.
  class Rule < Data.define(:id, :applies_to, :predicate, :domain, :category, :policy, :scopes,
                           :status, :source, :related)
    include Checked

    ID_PATTERN = /\A[a-z0-9]+(\.[a-z0-9_]+)+\z/

    def initialize(domain: nil, status: nil, related: [], **fields)
      status = Status.parse(status) if status.is_a?(String)
      super(domain:, status:, related: related.dup.freeze, **fields)
    end

    def violations(model, environment)
      return [] if policy == "equivalent_contents"

      applies = applies_to.call(model, environment)
      return [] if FsimgSpec.undefined?(applies) || !applies

      result = predicate.call(model, environment)
      case [scopes.one?, result]
      in [_, Undefined] then []
      in [true, true] then []
      in [true, false] then [Violation.new(rule: self, scope: scopes.first)]
      in [false, Array] if result.all? { scopes.include?(_1) } then result.map { Violation.new(rule: self, scope: _1) }
      else raise RuleError, "#{id}: predicate answered #{result.inspect} for scopes #{scopes.inspect}"
      end
    end

    # The status a stage that checks a violation must reject with, or nil.
    def rejection_status
      case policy
      in "must_reject" then status
      in "optional_feature" then Status.new(name: "FSIMG_EUNSUPPORTED")
      else nil
      end
    end

    def stages = scopes.flat_map { SCOPE_STAGES.fetch(_1) }.uniq

    # Whether the rule states the domain of creation input, which it reads
    # from a SourceModel rather than from an image.
    def input? = scopes.include?("input")
  end

  # What rules read about a run beyond the decoded model: the format a
  # case forces with -t, or nil.
  Environment = Data.define(:forced_format)

  # What the input rules read about a creation case: its settings and the
  # tree the harness wrote into source/ and walked back
  # (specs/design-spec-testing.md#cases). Every input is known, so it
  # leaves no scope unclassified. It judges create, whose records describe
  # a plan no rule models; the image create wrote is judged by the image
  # model the created hook decodes, so no record is compared here.
  SourceModel = Data.define(:settings, :tree) do
    def unclassified_scopes = []

    def record_problems(_result) = []
  end

  # The decoded model's violations and the scopes it could not decode
  # completely; together they decide every expectation of a case.
  Classification = Data.define(:violations, :unclassified) do
    def rule_ids = violations.map { _1.rule.id }.uniq.sort

    # The classification without destination violations, which depend on
    # the host's name trials rather than on the image.
    def without_destination = with(violations: violations.reject { _1.scope == "destination" })
  end

  # The checked set of rules a run evaluates. Construction refuses a rule
  # set that would make an expectation ambiguous: a duplicate or malformed
  # id, a category and policy that do not pair, an unknown scope, an input
  # rule with another scope, a status the policy does not call for or no
  # stage of its scopes can report, or a related rule that does not exist.
  #
  # The input rules read a SourceModel and every other rule an image
  # model, so each kind of model is classified by its own rules alone.
  class Registry
    def initialize(rules)
      @rules = rules.to_h { [_1.id, _1] }.freeze
      raise RuleError, "duplicate rule id among #{rules.map(&:id).tally.select { _2 > 1 }.keys}" if @rules.size != rules.size

      rules.each { check(_1) }
    end

    def rules = @rules.values

    def fetch(id) = @rules.fetch(id) { raise RuleError, "unknown rule #{id}" }

    # The classification of a decoded image by every rule but the input
    # rules.
    def classify(model, environment) = classification(model, environment, rules.reject(&:input?))

    # The classification of a creation case's SourceModel by the input
    # rules.
    def classify_source(model, environment) = classification(model, environment, rules.select(&:input?))

    private

    def classification(model, environment, rules)
      unknown = model.unclassified_scopes - SCOPE_STAGES.keys
      raise RuleError, "the model names unknown unclassified scopes #{unknown}" unless unknown.empty?

      Classification.new(
        violations: rules.flat_map { _1.violations(model, environment) },
        unclassified: model.unclassified_scopes.sort.freeze
      )
    end

    def check(rule)
      found = problem(rule)
      raise RuleError, "#{rule.id}: #{found}" if found
    end

    # The first reason rule cannot join the registry, or nil.
    def problem(rule) = identity_problem(rule) || scope_problem(rule) || status_problem(rule) || predicate_problem(rule)

    def identity_problem(rule)
      return "malformed id" unless Rule::ID_PATTERN.match?(rule.id)
      return "category #{rule.category} cannot carry policy #{rule.policy}" unless
        CATEGORY_POLICIES.fetch(rule.category, []).include?(rule.policy)
      return "no source" if rule.source.to_s.empty?

      "unknown related rule among #{rule.related}" unless rule.related.all? { @rules.key?(_1) }
    end

    # Every later check reads the scopes' stages, so this one runs first.
    def scope_problem(rule)
      return "unknown scope among #{rule.scopes}" unless rule.scopes.all? { SCOPE_STAGES.key?(_1) }
      return "a scope repeats in #{rule.scopes}" if rule.scopes.uniq.size != rule.scopes.size
      return "an input rule reads the source model and has no other scope" if rule.input? && rule.scopes.size > 1

      "no scope" if rule.scopes.empty? && rule.policy != "equivalent_contents"
    end

    def status_problem(rule)
      return "#{rule.policy} needs a status" if rule.policy == "must_reject" && rule.status.nil?
      return "#{rule.policy} takes no status" if rule.policy != "must_reject" && rule.status
      return nil unless rule.status

      refusing = rule.stages.reject { rule.status.verdict_at?(_1) }
      "#{rule.status} is no verdict status at #{refusing.join(", ")}" unless refusing.empty?
    end

    def predicate_problem(rule)
      return nil if rule.policy == "equivalent_contents"

      "applies_to and predicate must be callable" unless [rule.applies_to, rule.predicate].all? { _1.respond_to?(:call) }
    end
  end
end
