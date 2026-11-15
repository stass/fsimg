# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The APFS suite of the case phase: the writer's deterministic
    # creations, the run's drawn trees and the promoted regressions of
    # cases/apfs.
    module Suite
      PROMOTED = File.expand_path("../../../cases/apfs", __dir__)

      module_function

      def registry = Rules.registry

      def cases(ceilings:, name_trials:, draws:, registry: Suite.registry, directory: PROMOTED, **)
        cases = Cases.new(registry:, ceilings:, name_trials:)
        deterministic = Creations.deterministic.map { pending(cases, _1) }
        deterministic + drawn(cases, draws) + promoted(directory, ceilings:).map do |recipe|
          PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil, classify: -> { cases.replay(recipe) })
        end
      end

      def replay(recipe, ceilings:, name_trials:, registry: Suite.registry, **)
        cases = Cases.new(registry:, ceilings:, name_trials:)
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil, classify: -> { cases.replay(recipe) })
      end

      def pending(cases, creation) =
        PendingCase.new(id: creation.id, rules: creation.rules, skip: nil, classify: -> { cases.create(creation) })

      # Every draw's source is classified, which costs no fsimg run; the
      # first draws.sample of them run, and so does any draw the input
      # rules flag, which is a generator defect the case then reports.
      def drawn(cases, draws)
        Creations.draw(seed: draws.seed, count: draws.count).each_with_index.filter_map do |creation, index|
          defective = cases.source_classification(creation.source).violations.any?
          next unless defective || index < draws.sample

          PendingCase.new(id: creation.id, rules: [], skip: nil, classify: -> { cases.create(creation, random_seed: draws.seed) })
        end
      end

      def promoted(directory, ceilings:)
        return [] unless Dir.exist?(directory)

        Dir.children(directory).sort.map do |name|
          recipe = Recipe.read(File.join(directory, name), ceilings:)
          raise RecipeError, "APFS promoted recipe name differs" unless name == "#{recipe.id}.json" && recipe.promoted?

          recipe
        end
      end

      private_class_method :pending, :drawn
    end
  end
end
