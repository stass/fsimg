# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"

module FsimgSpec
  module Iso9660
    Classified = Data.define(:kase, :image, :model, :created, :defect, :oracles)
    # Local constraints and provable capacity refusals are classified. A
    # syntactically valid explicit capacity that passes those constraints
    # has no exact independent layout plan, so input remains unclassified.
    SourceModel = Data.define(:format, :settings, :tree, :unclassified) do
      def unclassified_scopes = unclassified
      def record_problems(_result) = []
    end

    # Materialization and classification use the same Case/Registry funnel
    # as every format. Only construction, decoding and oracle adapters vary.
    class Cases
      BUILDER_SOURCES = %w[layout.rb builder.rb].map { File.join(__dir__, _1) }.freeze

      def initialize(registry:, cache:, ceilings:, name_trials:, shell:)
        @registry = registry
        @cache = cache
        @ceilings = ceilings
        @name_trials = name_trials
        @shell = shell
      end

      def call(witness, random_seed: nil)
        base = Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: Builder.tree(witness.parameters))
        versions = { Builder::NAME => Tools.source_version(*BUILDER_SOURCES) }
        path = @cache.fetch(@cache.key(base, versions)) do
          Builder.write(_1, parameters: base.parameters, tree: base.tree, max_bytes: @ceilings.image)
        end
        image = witness.mutations.reduce(ImageView.open(path)) { |view, mutation| mutation.apply(view) }
        model = Model.decode(image, ceilings: @ceilings)
        stages = stages_for(witness, model)
        recipe = Recipe.new(id: witness.id, format: "iso9660", kind: witness.intended ? "targeted" : "valid",
                            intended_violation: witness.intended, base:, base_sha256: Digest::SHA256.file(path).hexdigest,
                            tool_versions: versions.transform_keys(&:to_sym), mutations: witness.mutations, stages:,
                            random_seed:,
                            expected: stages.to_h { [_1, Expectation.new(unclassified: true)] })
        classified(recipe, image, model)
      end

      def replay(recipe)
        return creation(recipe) unless recipe.reading?
        raise RecipeError, "ISO replay requires its independent builder" unless recipe.base.builder == Builder::NAME && recipe.base.population.nil?

        versions = { Builder::NAME => Tools.source_version(*BUILDER_SOURCES) }
        path = @cache.fetch(@cache.key(recipe.base, versions)) do
          Builder.write(_1, parameters: recipe.base.parameters, tree: recipe.base.tree, max_bytes: @ceilings.image)
        end
        raise RecipeError, "ISO replay base changed" unless Digest::SHA256.file(path).hexdigest == recipe.base_sha256

        classify(recipe, path)
      end

      def create(witness, random_seed: nil)
        stages = witness.intended ? ["create"] : ["create", *READING_STAGES]
        recipe = Recipe.new(id: witness.id, format: "iso9660", kind: witness.intended ? "targeted" : "valid",
          intended_violation: witness.intended, source: witness.source, stages:,
          random_seed:,
          expected: stages.to_h { [_1, Expectation.new(unclassified: true)] })
        classified = creation(recipe)
        kase = classified.kase.with(recipe: classified.kase.recorded)
        classified.with(kase:, defect: kase.harness_defect)
      end

      private

      def creation(recipe)
        source = FsimgSpec::SourceModel.new(format: recipe.format, settings: recipe.source.settings, tree: recipe.source.tree)
        environment = Environment.new(forced_format: nil, trial_answers: NO_TRIAL)
        preliminary = @registry.classify_source(source, environment)
        unclassified = source.settings.key?(:size) && preliminary.violations.empty? ? ["input"].freeze : [].freeze
        source = SourceModel.new(format: source.format, settings: source.settings, tree: source.tree, unclassified:)
        source_classification = @registry.classify_source(source, environment)
        trials = @name_trials.answers_for(recipe.source.tree, stages: recipe.stages)
        kase = Case.derive(recipe, Case::CREATED_IMAGE.with(trial_answers: trials), registry: @registry, source_classification:)
        created = lambda do |path|
          model = Model.decode(ImageView.open(path), ceilings: @ceilings)
          [model, @registry.classify(model, environment.with(trial_answers: trials)), nil]
        end
        defect = kase.harness_defect
        defect ||= "#{recipe.id}: recipe classification drifted" if recipe.promoted? && kase.drift.any?
        Classified.new(kase:, image: nil, model: source, created:, defect:, oracles: oracle_hook(recipe))
      end

      def classify(recipe, path)
        image = recipe.mutations.reduce(ImageView.open(path)) { |view, mutation| mutation.apply(view) }
        model = Model.decode(image, ceilings: @ceilings)
        classified(recipe, image, model)
      end

      def stages_for(witness, model)
        return READING_STAGES unless witness.intended

        classification = @registry.classify(model, Environment.new(forced_format: nil, trial_answers: NO_TRIAL))
        scopes = classification.violations.select { _1.rule.id == witness.intended }.map(&:scope)
        first = READING_STAGES.find { |stage| scopes.any? { SCOPE_STAGES.fetch(_1).include?(stage) } }
        return ["ls", "extract"] if first == "extract"

        [first || @registry.fetch(witness.intended).stages.first]
      end

      def classified(recipe, image, model)
        trials = @name_trials.answers_for(model.tree, stages: recipe.stages)
        classification = @registry.classify(model, Environment.new(forced_format: recipe.forced_format, trial_answers: trials))
        kase = Case.derive(recipe, classification, registry: @registry)
        kase = kase.with(recipe: kase.recorded) unless recipe.promoted?
        defect = kase.harness_defect
        defect ||= "#{recipe.id}: recipe classification drifted" if recipe.promoted? && kase.drift.any?
        Classified.new(kase:, image:, model:, created: nil, defect:, oracles: oracle_hook(recipe))
      end

      def oracle_hook(recipe)
        return nil unless @shell && recipe.kind == "valid" && (recipe.reading? ? recipe.mutations.empty? : true)

        ->(directory, model) do
          image = recipe.reading? ? Runner::IMAGE : Runner::CREATED
          observations = Oracles.observe(@shell, image:, chdir: directory, model:, settings: recipe.source&.settings || {})
          disagreements = recipe.reading? ? [] : Oracles.disagreements(observations)
          problem = disagreements.empty? ? nil : "the oracles disagree with the created image: #{disagreements.join('; ')}"
          Observed.new(observations:, problem:)
        end
      end
    end
  end
end
