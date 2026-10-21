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
    # fsimg, or nil. oracles is the hook CaseRun calls with the case
    # directory and the image's model for the oracles' observations, or
    # nil when no oracle reads the case's image.
    class Classified < Data.define(:kase, :image, :model, :created, :defect, :oracles)
      include Checked

      def initialize(oracles: nil, **fields) = super
    end

    # Witnesses and recorded recipes made into cases. A reading case's base
    # is rendered into the fixture cache, its mutations applied, the image
    # decoded and classified by the whole registry, and the recipe recorded
    # with the classification and the expectations derived from it. A
    # case that extracts a known tree is classified with the host's answers
    # to its name trials. A creation case is classified from its source,
    # and the image create writes is decoded and classified by the created
    # hook. The oracles read a valid case's unmutated image
    # (specs/design-spec-testing.md#process-boundary-and-host-safety),
    # whose tree is known.
    class Cases
      # name_trials is the run's NameTrials, or NameTrials::NONE where
      # nothing extracts, as when fuzz seeds are exported. shell is the
      # run's OracleShell, or nil where no oracle runs. Bases render into
      # cache.
      def initialize(registry:, cache:, ceilings:, name_trials:, shell:)
        @registry = registry
        @cache = cache
        @ceilings = ceilings
        @name_trials = name_trials
        @shell = shell
        @base_digests = {}
      end

      # The witness's case, image and model, with the generator defect the
      # case shows, or nil. A drawn witness's recipe records the seed it
      # was drawn from as random_seed; its tree and parameters, not the
      # seed, reproduce it.
      def call(witness, random_seed: nil)
        base = base_of(witness)
        path = Bases.render(base, cache: @cache, ceilings: @ceilings)
        stages = stages_for(witness)
        image, model, classification = decode(path, tree: base.tree, mutations: witness.mutations,
                                              forced_format: witness.forced_format, stages:)
        kase = Case.derive(recipe(witness, base:, path:, stages:, classification:, random_seed:), classification,
                           registry: @registry)
        kase = kase.with(recipe: kase.recorded)
        Classified.new(kase:, image:, model:, created: nil, defect: kase.harness_defect, oracles: oracles(kase.recipe))
      end

      # A valid reading case, named id, of base, whose tree mtools
      # populates (specs/design-spec-testing.md#fixtures): an image whose
      # directories neither fsimg nor the independent builder wrote, read
      # through every reading stage. A volume off its parameters is the
      # harness's fault, a FixtureError. The tree judges fsimg only once
      # mtools wrote it, its read-back reproduced it and the checker did not
      # refuse the image; otherwise mtools did not write the tree the recipe
      # states, and the case is a HostSkip naming why. The read-back must
      # agree, since an excused one does not show that mtools wrote the
      # tree, while an excused checker refused nothing: its defect is no
      # fault of the image (specs/design-spec-testing.md#oracle-exceptions).
      # The oracles observed the very bytes the case reads, unmutated, when
      # the base was built, so those observations are the case's.
      def populated(base, id:, random_seed: nil)
        fixture = begin
          Bases.materialize(base, cache: @cache, shell: @shell, ceilings: @ceilings)
        rescue PopulationError => e
          raise HostSkip, "mtools did not write the tree: #{e.message}"
        end
        geometry = Bases.geometry_problems(base, fixture.image)
        raise FixtureError, "#{id}: its volume is off its recipe: #{geometry.join("; ")}" unless geometry.empty?

        read_back, checker = fixture.observations.values_at("mtools", "fsck")
        raise HostSkip, "the mtools read-back did not agree (#{read_back.state}): #{read_back.detail}" unless
          read_back.state == "agrees"
        raise HostSkip, "the checker refuses the image: #{checker.detail}" if checker.state == "disagrees"

        image, model, classification = decode(fixture.image, tree: base.tree, mutations: [], forced_format: nil,
                                              stages: READING_STAGES)
        recipe = Recipe.new(
          id:, format: Bases.variant(base), kind: "valid", base:, base_sha256: base_digest(fixture.image),
          tool_versions: fixture.manifest.fetch(:tool_versions).transform_keys(&:to_sym), mutations: [],
          stages: READING_STAGES, expected: READING_STAGES.to_h { [_1, Expectation.derive(classification, _1)] }, random_seed:
        )
        kase = Case.derive(recipe, classification, registry: @registry)
        kase = kase.with(recipe: kase.recorded)
        Classified.new(kase:, image:, model:, created: nil, defect: kase.harness_defect,
                       oracles: ->(_directory, _model) { Observed.new(observations: fixture.observations) })
      end

      # A creation witness (Witnesses.creations) as a case: valid, or
      # targeted at its intended input rule, which create alone checks, so
      # nothing runs after a create that must reject. A drawn tree's
      # creation records the seed it was drawn from as random_seed.
      def creation_witness(witness, random_seed: nil)
        stages = witness.intended ? ["create"] : ["create", *READING_STAGES]
        recipe = Recipe.new(
          id: witness.id, format: witness.format, kind: witness.intended ? "targeted" : "valid",
          intended_violation: witness.intended, stages:, random_seed:,
          source: Source.new(tree: witness.tree, settings: witness.settings, root: witness.root),
          expected: stages.to_h { [_1, Expectation.new(unclassified: true)] }
        )
        classified = creation(recipe)
        kase = classified.kase.with(recipe: classified.kase.recorded)
        classified.with(kase:, defect: kase.harness_defect)
      end

      # Whether a witness meant valid, classified in memory, shows a
      # generator defect: a tree the builder cannot lay out, or a rule it
      # violates. Nothing of the witness is written. The builder writes a
      # tree only into the tables, the directories, the data region and
      # FSInfo's free-cluster hints. The model decodes none of the first
      # three, and no rule reads the hints, only FSInfo's signatures, which
      # no tree changes. So the model is the empty base's, rendered once
      # into the cache, with the witness's tree as its known tree; a rule
      # that reads the hints would need the witness's own image instead.
      # No name trial runs, so only the host's destination rules, which no
      # witness's validity depends on, stay unclassified.
      # A defective witness's own case reports how, and a sampled one's
      # case checks this answer (Suite.drawn). A fault in rendering the
      # empty base is no defect of the witness, and propagates.
      def defective?(witness)
        begin
          Builder.plan(parameters: witness.parameters, tree: witness.tree)
        rescue BuilderError
          return true
        end
        empty = Bases.render(Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: []),
                             cache: @cache, ceilings: @ceilings)
        model = Model.decode(ImageView.open(empty), tree: KnownTree.of(witness.tree))
        @registry.classify(model, Environment.new(forced_format: nil, trial_answers: NO_TRIAL)).violations.any?
      end

      # Why recipe cannot be replayed on this host, or nil: only a reading
      # recipe on a base the independent builder renders, tree and all,
      # replays without development tools, and a creation recipe needs
      # fsimg alone.
      def self.unreplayable(recipe)
        return nil unless recipe.reading?
        return "#{recipe.id}: only #{Builder::NAME} bases replay without tools" unless recipe.base.builder == Builder::NAME
        return "#{recipe.id}: a base mtools populates does not replay without tools" if recipe.base.population

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
        image, model, classification = decode(path, tree: recipe.base.tree, mutations: recipe.mutations,
                                              forced_format: recipe.forced_format, stages: recipe.stages)
        kase = Case.derive(recipe, classification, registry: @registry)
        digest = base_digest(path)
        defect = digest == recipe.base_sha256 ? replay_defect(kase) : "#{recipe.id}: its base renders to #{digest}"
        Classified.new(kase:, image:, model:, created: nil, defect:, oracles: oracles(recipe))
      end

      private

      # The oracles' hook for a reading case, when the run has a shell and
      # the case's image is valid and unmutated, so its tree is known; an
      # image of another class would reach an oracle only under the
      # confinement the nightly and deep profiles name.
      def oracles(recipe)
        return nil unless @shell && recipe.reading? && recipe.kind == "valid" && recipe.mutations.empty?

        ->(directory, model) { Observed.new(observations: Oracles.observe(@shell, image: Runner::IMAGE, chdir: directory, model:)) }
      end

      # A creation recipe as a case: create follows its source model, and
      # the reading stages the image it writes, which the created hook
      # decodes once create has accepted. A manifest case's tree reaches
      # fsimg without touching the host, so the name trials of its tree
      # classify its extract like any other case's; a host-tree case's
      # source tree, written through the funnel and walked back, stands for
      # them, since CaseDirectory makes a tree the host stores under other
      # names a host skip. The oracles read the image create wrote, and
      # their checker and read-back give a verdict. The known tree, which
      # hashes every file, is built only once create has accepted, never
      # to classify the case: a file create must refuse may be one past
      # FAT's 4 GiB. A source create must accept has a planned volume
      # (Sizing.plan), whose layout the created image must have.
      def creation(recipe)
        tree = recipe.source.tree
        source = SourceModel.new(format: recipe.format, settings: recipe.source.settings, tree:)
        source_classification = @registry.classify_source(source, Environment.new(forced_format: nil, trial_answers: NO_TRIAL))
        trial_answers = recipe.source.manifest? ? @name_trials.answers_for(tree, stages: recipe.stages) : TrialAnswers::HELD
        kase = Case.derive(recipe, Case::CREATED_IMAGE.with(trial_answers:), registry: @registry, source_classification:)
        planned = Sizing.plan(source) if source_classification.violations.empty?
        created = lambda do |path|
          model = Model.decode(ImageView.open(path), tree: KnownTree.of(tree))
          [model, @registry.classify(model, Environment.new(forced_format: nil, trial_answers:)),
           planned && Sizing.layout_problem(planned, model.geometry)]
        end
        Classified.new(kase:, image: nil, model: source, created:, defect: replay_defect(kase), oracles: created_oracles)
      end

      # The oracles' hook for the image fsimg created, whose model the
      # created hook decoded; nil when no oracle runs. A disagreement of
      # the checker or the read-back that no exception excuses fails the
      # case (specs/design-spec-testing.md#independence-and-oracles).
      def created_oracles
        return nil unless @shell

        lambda do |directory, model|
          observations = Oracles.observe(@shell, image: Runner::CREATED, chdir: directory, model:)
          disagreements = Oracles.disagreements(observations)
          Observed.new(observations:,
                       problem: disagreements.empty? ? nil : "the oracles disagree with the created image: #{disagreements.join("; ")}")
        end
      end

      # The image at path with mutations applied, its model, and its
      # classification with format forced. An unmutated image holds tree,
      # the tree its base was populated with; a mutation may have changed
      # anything, so a mutated image's tree is unknown.
      def decode(path, tree:, mutations:, forced_format:, stages:)
        image = mutations.reduce(ImageView.open(path)) { |view, mutation| mutation.apply(view) }
        known_tree = mutations.empty? ? KnownTree.of(tree) : Model::UNKNOWN_TREE
        model = Model.decode(image, tree: known_tree)
        trial_answers = @name_trials.answers_for(known_tree, stages:)
        [image, model, @registry.classify(model, Environment.new(forced_format:, trial_answers:))]
      end

      # The witness's recipe. Its format is the base's variant, which picks
      # the format suite on replay; a mutation may change the variant the
      # image decodes to, which the classification records.
      def recipe(witness, base:, path:, stages:, classification:, random_seed:)
        Recipe.new(
          id: witness.id, format: Fat.variant_of(witness.parameters.fetch(:clusters)),
          kind: witness.intended ? "targeted" : "valid", intended_violation: witness.intended,
          base:, base_sha256: base_digest(path), tool_versions: Bases.builder_versions.transform_keys(&:to_sym),
          mutations: witness.mutations, forced_format: witness.forced_format, stages:,
          expected: stages.to_h { [_1, Expectation.derive(classification, _1)] }, random_seed:
        )
      end

      def base_of(witness) = Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: witness.tree)

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
