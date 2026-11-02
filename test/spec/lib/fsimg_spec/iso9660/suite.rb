# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    module Suite
      PROMOTED = File.expand_path("../../../cases/iso9660", __dir__)

      module_function

      def registry = Rules.registry

      def cases(cache:, ceilings:, name_trials:, shell:, draws:, registry: Suite.registry, directory: PROMOTED)
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:, shell:)
        witnesses = Witnesses.deterministic(registry).map do |witness|
          PendingCase.new(id: witness.id, rules: witness.rules, skip: nil, classify: -> { cases.call(witness) })
        end
        witnesses += Witnesses.creations(registry, ceilings:).map do |witness|
          PendingCase.new(id: witness.id, rules: witness.rules, skip: nil, classify: -> { cases.create(witness) })
        end
        witnesses + drawn(draws, registry:, ceilings:, name_trials:, shell:) + promoted(directory, ceilings:).map do |recipe|
          PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil,
                          classify: -> { cases.replay(recipe) })
        end
      end

      # Classify every bounded draw and retain the configured sample plus
      # every generator defect. Listing holds only one rendered fixture;
      # each pending case rebuilds it and releases it after recording.
      def drawn(draws, registry:, ceilings:, name_trials:, shell:)
        return [] if draws.count.zero?

        building = Cases.new(registry:, cache: draws.cache, ceilings:, name_trials:, shell:)
        Profiles.draw(seed: draws.seed, count: draws.count).each_with_index.flat_map do |witness, index|
          classified = building.call(witness, random_seed: draws.seed)
          defective = !classified.defect.nil?
          draws.cache.clear
          next [] unless defective || index < draws.sample

          reading = PendingCase.new(id: witness.id, rules: [], skip: nil,
            classify: -> { building.call(witness, random_seed: draws.seed) }, release: -> { draws.cache.clear })
          next [reading] if index >= draws.sample

          creation = Witnesses::Creation.new(id: witness.id.sub(".drawn.", ".create.drawn."),
            source: Source.new(tree: Builder.tree(witness.parameters), settings: {}), intended: nil, rules: [])
          # Reader targets include the 1024-byte reader boundary, which
          # deliberately exceeds the writer's retained 1023-byte profile.
          accepted = creation.source.tree.none? { _1.type == "symlink" && _1.target.bytesize > 1023 }
          next [reading] unless accepted

          writing = PendingCase.new(id: creation.id, rules: [], skip: nil,
            classify: -> { building.create(creation, random_seed: draws.seed) })
          [reading, writing]
        end
      end
      private_class_method :drawn

      def replay(recipe, cache:, ceilings:, name_trials:, shell:, registry: Suite.registry)
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:, shell:)
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: nil,
                        classify: -> { cases.replay(recipe) })
      end

      def promoted(directory, ceilings:)
        return [] unless Dir.exist?(directory)

        Dir.children(directory).sort.map do |name|
          raise RecipeError, "ISO promoted entry is not JSON" unless name.end_with?(".json")

          recipe = Recipe.read(File.join(directory, name), ceilings:)
          raise RecipeError, "ISO promoted recipe name differs" unless name == "#{recipe.id}.json" && recipe.promoted?

          recipe
        end
      end
    end
  end
end
