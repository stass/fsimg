# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Erofs
    # The EROFS suite of the case phase: the rules' deterministic witnesses
    # and the promoted regressions of cases/erofs.
    module Suite
      PROMOTED = File.expand_path("../../../cases/erofs", __dir__)

      module_function

      def registry = Rules.registry

      def cases(cache:, ceilings:, name_trials:, registry: Suite.registry, directory: PROMOTED, **)
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:)
        pending = Witnesses.deterministic(registry).map do |witness|
          PendingCase.new(id: witness.id, rules: witness.rules, skip: nil, classify: -> { cases.call(witness) })
        end
        pending + promoted(directory, ceilings:).map do |recipe|
          PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil, classify: -> { cases.replay(recipe) })
        end
      end

      def replay(recipe, cache:, ceilings:, name_trials:, registry: Suite.registry, **)
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:)
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil, classify: -> { cases.replay(recipe) })
      end

      def promoted(directory, ceilings:)
        return [] unless Dir.exist?(directory)

        Dir.children(directory).sort.map do |name|
          recipe = Recipe.read(File.join(directory, name), ceilings:)
          raise RecipeError, "EROFS promoted recipe name differs" unless name == "#{recipe.id}.json" && recipe.promoted?

          recipe
        end
      end
    end
  end
end
