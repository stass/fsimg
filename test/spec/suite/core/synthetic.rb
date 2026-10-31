# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fsimg_spec"

# Synthetic rules over a synthetic model, for the core self-tests. The model
# is a set of named facts; a rule with the same name as a fact is violated
# when the fact is present, at the scope the fact names.
module Synthetic
  # The run environment of the synthetic tests: a host whose name trials
  # held every name, so the destination scope is classified and a
  # destination fact decides it like any other.
  ENVIRONMENT = FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::TrialAnswers::HELD)

  Model = Data.define(:facts, :unclassified_scopes) do
    def initialize(facts: {}, unclassified_scopes: []) = super
  end

  module_function

  # A rule named id that the fact id violates; a fact naming one scope of
  # several reports that violation's scope.
  def rule(id, policy:, scopes:, status: nil, category: category_for(policy), related: [])
    FsimgSpec::Rule.new(
      id:, category:, policy:, scopes:, status:, related:,
      applies_to: ->(model, _environment) { true },
      predicate: lambda do |model, _environment|
        scope = model.facts[id]
        scopes.one? ? scope.nil? : [scope].compact
      end,
      source: "synthetic rule for the core self-tests"
    )
  end

  def category_for(policy) = FsimgSpec::CATEGORY_POLICIES.find { |_, policies| policies.include?(policy) }.first

  # facts maps a rule id to true, or for a rule with several scopes to the
  # scope of its violation.
  def classify(registry, facts = {}, unclassified: []) =
    registry.classify(Model.new(facts:, unclassified_scopes: unclassified), ENVIRONMENT)
end
