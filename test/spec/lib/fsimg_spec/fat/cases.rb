# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"

module FsimgSpec
  module Fat
    # A witness made into a case: its base rendered into the fixture cache,
    # its mutations applied, the image decoded and classified by the whole
    # registry, and the recipe recorded with the classification and the
    # expectations derived from it (specs/design-spec-testing.md#data-flow).
    # defect is why the case cannot be trusted to judge fsimg, or nil.
    Classified = Data.define(:kase, :image, :model, :defect)

    class Cases
      def initialize(registry:, cache:, ceilings:)
        @registry = registry
        @cache = cache
        @ceilings = ceilings
        @base_digests = {}
      end

      # The witness's case, image and model, with the generator defect the
      # case shows, or nil.
      def call(witness)
        base = Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: [])
        path = Bases.render(base, cache: @cache, ceilings: @ceilings)
        image, model, classification = decode(path, witness.mutations, witness.forced_format)
        kase = Case.derive(recipe(witness, base:, path:, classification:), classification, registry: @registry)
        kase = kase.with(recipe: kase.recorded)
        Classified.new(kase:, image:, model:, defect: kase.harness_defect)
      end

      # Why recipe cannot be replayed on this host, or nil: only a reading
      # recipe on a base of the independent builder renders without
      # development tools.
      def self.unreplayable(recipe)
        return "#{recipe.id}: a creation recipe does not replay" unless recipe.reading?
        return "#{recipe.id}: only #{Builder::NAME} bases replay without tools" unless recipe.base.builder == Builder::NAME

        nil
      end

      # A recorded recipe that is not unreplayable, such as a promoted case
      # or a failure record's, classified again under the current rules. Its
      # base must render to the bytes it recorded. How the recorded
      # classification differs from the derived one is the case's drift; a
      # promoted case's drift is a defect, because its recipe must change in
      # the commit that changes its meaning.
      def replay(recipe)
        reason = Cases.unreplayable(recipe)
        raise HarnessError, reason if reason

        path = Bases.render(recipe.base, cache: @cache, ceilings: @ceilings)
        image, model, classification = decode(path, recipe.mutations, recipe.forced_format)
        kase = Case.derive(recipe, classification, registry: @registry)
        Classified.new(kase:, image:, model:, defect: replay_defect(kase, base_digest(path)))
      end

      private

      # The image at path with mutations applied, its model, and its
      # classification with format forced.
      def decode(path, mutations, forced_format)
        image = mutations.reduce(ImageView.open(path)) { |view, mutation| mutation.apply(view) }
        model = Model.decode(image)
        [image, model, @registry.classify(model, Environment.new(forced_format:))]
      end

      # The witness's recipe. Its format is the base's variant, which picks
      # the format suite on replay; a mutation may change the variant the
      # image decodes to, which the classification records.
      def recipe(witness, base:, path:, classification:)
        stages = stages_for(witness)
        Recipe.new(
          id: witness.id, format: Fat.variant_of(witness.parameters.fetch(:clusters)),
          kind: witness.intended ? "targeted" : "valid", intended_violation: witness.intended,
          base:, base_sha256: base_digest(path), tool_versions: Bases.builder_versions.transform_keys(&:to_sym),
          mutations: witness.mutations, forced_format: witness.forced_format, stages:,
          expected: stages.to_h { [_1, Expectation.derive(classification, _1)] }
        )
      end

      # The stages the witness runs. A valid witness, or one whose intended
      # rule is must_accept, runs every reading stage, so a fault at any
      # stage fails it; a stage whose scope the model leaves unclassified
      # asserts no outcome. Any other targeted witness runs the first stage
      # that checks its intended rule.
      def stages_for(witness)
        intended = witness.intended && @registry.fetch(witness.intended)
        return READING_STAGES if intended.nil? || intended.policy == "must_accept"

        [intended.stages.first]
      end

      def base_digest(path) = @base_digests[path] ||= Digest::SHA256.file(path).hexdigest

      def replay_defect(kase, rendered)
        recipe = kase.recipe
        if rendered != recipe.base_sha256 then "#{recipe.id}: its base renders to #{rendered}"
        elsif kase.harness_defect then kase.harness_defect
        elsif recipe.promoted? && kase.drift.any? then "#{recipe.id} drifted: #{kase.drift.join("; ")}"
        end
      end
    end
  end
end
