# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"

module FsimgSpec
  module Erofs
    Classified = Data.define(:kase, :image, :model, :created, :defect, :oracles)

    # Materialization and classification use the same Case/Registry funnel
    # as every format; only construction and decoding are EROFS's. The
    # reader has no writer to create with and no verifier yet, so a case
    # runs the stages the reader answers.
    class Cases
      STAGES = %w[info ls extract].freeze
      BUILDER_SOURCES = %w[layout.rb builder.rb].map { File.join(__dir__, _1) }.freeze

      def initialize(registry:, cache:, ceilings:, name_trials:)
        @registry = registry
        @name_trials = name_trials
        @cache = cache
        @ceilings = ceilings
      end

      def call(witness, random_seed: nil)
        base = Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: Builder.tree(witness.parameters))
        path = image_path(base)
        view = witness.mutations.reduce(ImageView.open(path)) { |image, mutation| mutation.apply(image) }
        model = Model.decode(view, ceilings: @ceilings)
        stages = stages_for(witness, model)
        recipe = Recipe.new(id: witness.id, format: "erofs", kind: witness.intended ? "targeted" : "valid",
                            intended_violation: witness.intended, base:, base_sha256: Digest::SHA256.file(path).hexdigest,
                            tool_versions: versions.transform_keys(&:to_sym), mutations: witness.mutations, stages:,
                            random_seed:, expected: stages.to_h { [_1, Expectation.new(unclassified: true)] })
        classified(recipe, view, model)
      end

      def replay(recipe)
        raise RecipeError, "EROFS replay needs a reading recipe built by the independent builder" unless
          recipe.reading? && recipe.base.builder == Builder::NAME && recipe.base.population.nil?

        path = image_path(recipe.base)
        raise RecipeError, "EROFS replay base changed" unless Digest::SHA256.file(path).hexdigest == recipe.base_sha256

        view = recipe.mutations.reduce(ImageView.open(path)) { |image, mutation| mutation.apply(image) }
        classified(recipe, view, Model.decode(view, ceilings: @ceilings))
      end

      private

      def versions = { Builder::NAME => Tools.source_version(*BUILDER_SOURCES) }

      def image_path(base)
        @cache.fetch(@cache.key(base, versions)) do
          Builder.write(_1, parameters: base.parameters, tree: base.tree, max_bytes: @ceilings.image)
        end
      end

      # A valid case runs every stage; a targeted one the first stage that
      # checks a scope its intended rule is violated in.
      def stages_for(witness, model)
        return STAGES unless witness.intended

        classification = @registry.classify(model, environment)
        scopes = classification.violations.select { _1.rule.id == witness.intended }.map(&:scope)
        first = STAGES.find { |stage| scopes.any? { SCOPE_STAGES.fetch(_1).include?(stage) } }
        [first || STAGES.first]
      end

      def environment = Environment.new(forced_format: nil, trial_answers: NO_TRIAL)

      def classified(recipe, image, model)
        trials = @name_trials.answers_for(model.tree, stages: recipe.stages)
        classification = @registry.classify(model, environment.with(forced_format: recipe.forced_format, trial_answers: trials))
        kase = Case.derive(recipe, classification, registry: @registry)
        kase = kase.with(recipe: kase.recorded) unless recipe.promoted?
        defect = kase.harness_defect
        defect ||= "#{recipe.id}: recipe classification drifted" if recipe.promoted? && kase.drift.any?
        Classified.new(kase:, image:, model:, created: nil, defect:, oracles: nil)
      end
    end
  end
end
