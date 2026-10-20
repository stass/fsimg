# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"

module FsimgSpec
  module Fat
    # A case ready to run (specs/design-spec-testing.md#data-flow): its
    # Case, its image, an ImageView or nil for a creation case, and the
    # model that judges its first stage, with the created hook a creation
    # case needs, or nil. defect is why the case cannot be trusted to judge
    # fsimg, or nil.
    Classified = Data.define(:kase, :image, :model, :created, :defect)

    # Witnesses and recorded recipes made into cases. A reading case's base
    # is rendered into the fixture cache, its mutations applied, the image
    # decoded and classified by the whole registry, and the recipe recorded
    # with the classification and the expectations derived from it. A
    # creation case is classified from its source, and the image create
    # writes is decoded and classified by the created hook.
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
        image, model, classification = decode(path, base.tree, witness.mutations, witness.forced_format)
        kase = Case.derive(recipe(witness, base:, path:, classification:), classification, registry: @registry)
        kase = kase.with(recipe: kase.recorded)
        Classified.new(kase:, image:, model:, created: nil, defect: kase.harness_defect)
      end

      # Why recipe cannot be replayed on this host, or nil: only a reading
      # recipe on a base of the independent builder renders without
      # development tools, and a creation recipe needs fsimg alone.
      def self.unreplayable(recipe)
        return nil unless recipe.reading?
        return "#{recipe.id}: only #{Builder::NAME} bases replay without tools" unless recipe.base.builder == Builder::NAME

        nil
      end

      # A recorded recipe that is not unreplayable, such as a promoted case
      # or a failure record's, classified again under the current rules. A
      # reading recipe's base must render to the bytes it recorded. How the
      # recorded classification differs from the derived one is the case's
      # drift; a promoted case's drift is a defect, because its recipe must
      # change in the commit that changes its meaning.
      def replay(recipe)
        reason = Cases.unreplayable(recipe)
        raise HarnessError, reason if reason
        return creation(recipe) unless recipe.reading?

        path = Bases.render(recipe.base, cache: @cache, ceilings: @ceilings)
        image, model, classification = decode(path, recipe.base.tree, recipe.mutations, recipe.forced_format)
        kase = Case.derive(recipe, classification, registry: @registry)
        digest = base_digest(path)
        defect = digest == recipe.base_sha256 ? replay_defect(kase) : "#{recipe.id}: its base renders to #{digest}"
        Classified.new(kase:, image:, model:, created: nil, defect:)
      end

      private

      # A creation recipe as a case: create follows its source model, and
      # the reading stages the image it writes, which the created hook
      # decodes once create has accepted. The source tree the case
      # directory holds is the recipe's, since CaseDirectory makes a tree
      # the host stores under other names a host skip.
      def creation(recipe)
        source = SourceModel.new(settings: recipe.source.settings, tree: recipe.source.tree)
        source_classification = @registry.classify_source(source, Environment.new(forced_format: nil))
        kase = Case.derive(recipe, Case::CREATED_IMAGE, registry: @registry, source_classification:)
        known_tree = KnownTree.of(recipe.source.tree)
        created = lambda do |path|
          model = Model.decode(ImageView.open(path), tree: known_tree)
          [model, @registry.classify(model, Environment.new(forced_format: nil))]
        end
        Classified.new(kase:, image: nil, model: source, created:, defect: replay_defect(kase))
      end

      # The image at path with mutations applied, its model, and its
      # classification with format forced. An unmutated image holds tree,
      # the tree its base was populated with; a mutation may have changed
      # anything, so a mutated image's tree is unknown.
      def decode(path, tree, mutations, forced_format)
        image = mutations.reduce(ImageView.open(path)) { |view, mutation| mutation.apply(view) }
        model = Model.decode(image, tree: mutations.empty? ? KnownTree.of(tree) : Model::UNKNOWN_TREE)
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

      def replay_defect(kase)
        if kase.harness_defect then kase.harness_defect
        elsif kase.recipe.promoted? && kase.drift.any? then "#{kase.recipe.id} drifted: #{kase.drift.join("; ")}"
        end
      end
    end
  end
end
