# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The FAT cases of a run: every deterministic witness of the rules, the
    # sample of the run's drawn trees
    # (specs/design-spec-testing.md#generation-and-shrinking), and every
    # promoted regression recipe under test/spec/cases/fat/
    # (specs/design-spec-testing.md#reports-and-regressions).
    module Suite
      PROMOTED = File.expand_path("../../../cases/fat", __dir__)
      # The rules a drawn tree is a valid witness of: its names are drawn
      # from their domain.
      DRAWN_RULES = %w[fat.directory.name fat.directory.unique_names].freeze

      def self.registry = Rules.registry

      # The preflight's reading sample, written into directory.
      def self.preflight_sample(directory) = Bases.preflight_sample(directory)

      # The pending cases of a run under registry: every deterministic
      # witness, then the drawn trees that run, then every promoted recipe
      # in directory, loaded within ceilings, each classified with the
      # run's name trials (Cases.new). A promoted file that does not load is
      # no case, so it fails the whole phase, before any case runs.
      # tree_generator draws one tree (Trees.draw); a test may pass a
      # faulty one.
      def self.cases(cache:, ceilings:, name_trials:, draws:, registry: Suite.registry, directory: PROMOTED,
                     tree_generator: Trees.method(:tree))
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:)
        witnesses = Witnesses.deterministic(registry).map do |witness|
          PendingCase.new(id: witness.id, rules: [witness.rule, witness.intended].compact.uniq, skip: nil,
                          classify: -> { cases.call(witness) })
        end
        witnesses + drawn(draws, cases:, registry:, ceilings:, name_trials:, tree_generator:) +
          promoted(directory, ceilings:).map { pending(_1, cases) }
      end

      # The drawn trees that run as cases (specs/design-spec-testing.md#generation-and-shrinking):
      # cases classifies every draw in memory, and the first draws.sample
      # of them, with every later one that shows a generator defect, run
      # as cases whose recipes record the run's seed. Only those render,
      # into the draws' own cache, so a draw found sound costs neither a
      # file nor an fsimg run.
      def self.drawn(draws, cases:, registry:, ceilings:, name_trials:, tree_generator:)
        rendering = Cases.new(registry:, cache: draws.cache, ceilings:, name_trials:)
        Trees.draw(seed: draws.seed, count: draws.count, generator: tree_generator).each_with_index.filter_map do |witness, index|
          defective = cases.defective?(witness)
          next unless index < draws.sample || defective

          drawn_case(witness, defective:, cases: rendering, draws:)
        end
      end

      # The pending case of a drawn witness, which cases renders into the
      # draws' cache and classifies. Its case must find a generator defect
      # exactly where the in-memory classification, defective, did, or
      # that classification is wrong, and the case reports that instead.
      # Only drawn cases render into the draws' cache, one at a time, so
      # emptying it once the case is recorded leaves nothing of the case
      # behind, however its classification ended.
      def self.drawn_case(witness, defective:, cases:, draws:)
        classify = lambda do
          classified = cases.call(witness, random_seed: draws.seed)
          found_defect = !classified.defect.nil?
          next classified if found_defect == defective

          memory = defective ? "the in-memory classification found a defect" : "the in-memory classification found none"
          classified.with(defect: "#{witness.id}: #{memory}, but its case #{found_defect ? "shows #{classified.defect}" : "is sound"}")
        end
        PendingCase.new(id: witness.id, rules: DRAWN_RULES, skip: nil, classify:, release: -> { draws.cache.clear })
      end

      # recipe as a pending case under registry.
      def self.replay(recipe, cache:, ceilings:, name_trials:, registry: Suite.registry) =
        pending(recipe, Cases.new(registry:, cache:, ceilings:, name_trials:))

      # The promoted recipes in directory, in file name order. Every entry
      # is one: a recipe file named after its id, which replays on every
      # host. A promoted recipe on a formatter's base, which only a host with
      # that formatter could replay, is refused rather than skipped, so that
      # no regression case is silently never run.
      def self.promoted(directory, ceilings:)
        Dir.children(directory).sort.map do |name|
          raise RecipeError, "#{FsimgSpec.shown(name)} is no recipe file" unless name.end_with?(".json")

          recipe = Recipe.read(File.join(directory, name), ceilings:)
          raise RecipeError, "#{FsimgSpec.shown(name)} holds the recipe #{recipe.id}" unless name == "#{recipe.id}.json"
          raise RecipeError, "#{name} is no promoted case" unless recipe.promoted?

          reason = Cases.unreplayable(recipe)
          raise RecipeError, reason if reason

          recipe
        end
      end

      private_class_method :drawn, :drawn_case

      private_class_method def self.pending(recipe, cases) =
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: Cases.unreplayable(recipe),
                        classify: -> { cases.replay(recipe) })
    end
  end
end
