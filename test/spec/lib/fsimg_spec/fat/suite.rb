# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The FAT cases of a run: every deterministic witness of the rules, and
    # every promoted regression recipe under test/spec/cases/fat/
    # (specs/design-spec-testing.md#reports-and-regressions).
    module Suite
      PROMOTED = File.expand_path("../../../cases/fat", __dir__)

      def self.registry = Rules.registry

      # The preflight's reading sample, written into directory.
      def self.preflight_sample(directory) = Bases.preflight_sample(directory)

      # The pending cases of a run under registry: every deterministic
      # witness, then every promoted recipe in directory, loaded within
      # ceilings, each classified with the run's name trials (Cases.new). A
      # promoted file that does not load is no case, so it fails the whole
      # phase, before any case runs.
      def self.cases(cache:, ceilings:, name_trials:, registry: Suite.registry, directory: PROMOTED)
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:)
        witnesses = Witnesses.deterministic(registry).map do |witness|
          PendingCase.new(id: witness.id, rules: [witness.rule, witness.intended].compact.uniq, skip: nil,
                          classify: -> { cases.call(witness) })
        end
        witnesses + promoted(directory, ceilings:).map { pending(_1, cases) }
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

      private_class_method def self.pending(recipe, cases) =
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: Cases.unreplayable(recipe),
                        classify: -> { cases.replay(recipe) })
    end
  end
end
