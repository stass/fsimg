# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  module Fat
    # The FAT cases of a run: every deterministic witness of the rules,
    # reading and creation, the formatter bases mtools populates
    # (specs/design-spec-testing.md#fixtures), the sample of the run's drawn
    # trees (specs/design-spec-testing.md#generation-and-shrinking), each
    # read from a builder volume and created from a manifest, and every
    # promoted regression recipe under test/spec/cases/fat/
    # (specs/design-spec-testing.md#reports-and-regressions).
    module Suite
      PROMOTED = File.expand_path("../../../cases/fat", __dir__)
      # The rules a drawn tree is a valid witness of: its names are drawn
      # from their domain, which the reader and the writer share.
      DRAWN_RULES = %w[fat.directory.name fat.directory.unique_names].freeze
      CREATED_DRAWN_RULES = %w[fat.create.name fat.create.unique_names].freeze

      def self.registry = Rules.registry

      # The preflight's reading sample, written into directory.
      def self.preflight_sample(directory) = Bases.preflight_sample(directory)

      # The pending cases of a run under registry: every deterministic
      # reading witness, then every creation witness within ceilings, then
      # the formatter bases mtools populates, then the drawn trees that
      # run, then every promoted recipe in directory, loaded within
      # ceilings, each classified with the run's name trials and oracle
      # shell (Cases.new). A promoted file that does not load is no case,
      # so it fails the whole phase, before any case runs. tree_generator
      # draws one tree the builder renders (Trees.draw); a test may pass a
      # faulty one.
      def self.cases(cache:, ceilings:, name_trials:, shell:, draws:, registry: Suite.registry, directory: PROMOTED,
                     tree_generator: Trees.method(:tree))
        cases = Cases.new(registry:, cache:, ceilings:, name_trials:, shell:)
        witnesses = Witnesses.deterministic(registry).map do |witness|
          PendingCase.new(id: witness.id, rules: [witness.rule, witness.intended].compact.uniq, skip: nil,
                          classify: -> { cases.call(witness) })
        end
        creations = Witnesses.creations(registry, ceilings:).map do |witness|
          PendingCase.new(id: witness.id, rules: [witness.rule, witness.intended].compact.uniq, skip: nil,
                          classify: -> { cases.creation_witness(witness) })
        end
        witnesses + creations + external(cases, shell:) +
          drawn(draws, cases:, registry:, ceilings:, name_trials:, shell:, tree_generator:) +
          promoted(directory, ceilings:).map { pending(_1, cases) }
      end

      # The formatter base of each variant with the standard tree mtools
      # populates (Bases.standard), as valid reading cases: images no byte
      # of which fsimg or the independent builder wrote. A host without
      # the tools skips them. The standard tree holds only names every
      # mtools writes, so these cases guard population itself: mtools
      # refusing or altering it is a harness error, not a host skip.
      def self.external(cases, shell:) =
        Bases.standard.map do |variant, base|
          id = "fat.mtools.#{variant}"
          classify = lambda do
            cases.populated(base, id:)
          rescue HostSkip => e
            raise FixtureError, "the standard tree must populate: #{e.message}"
          end
          PendingCase.new(id:, rules: [], skip: Bases.unbuildable(base, shell), classify:)
        end

      # The drawn trees that run as cases (specs/design-spec-testing.md#generation-and-shrinking),
      # whose recipes record the run's seed: the trees the builder renders,
      # then those mtools populates. Each image is built into the draws' own
      # cache, formatted and populated where it lies.
      def self.drawn(draws, cases:, registry:, ceilings:, name_trials:, shell:, tree_generator:)
        building = Cases.new(registry:, cache: draws.cache, ceilings:, name_trials:, shell:)
        rendered_draws(draws, cases:, building:, tree_generator:) + populated_draws(draws, cases:, building:, shell:)
      end

      # cases classifies every draw of tree_generator in memory, and the
      # first draws.sample of them, with every later one that shows a
      # generator defect, run as cases. Only those render, so a draw found
      # sound costs neither a file nor an fsimg run. Each draw of the
      # sample is also created from a manifest (created_draw).
      def self.rendered_draws(draws, cases:, building:, tree_generator:) =
        Trees.draw(seed: draws.seed, count: draws.count, generator: tree_generator).each_with_index.flat_map do |witness, index|
          defective = cases.defective?(witness)
          sampled = index < draws.sample
          next [] unless sampled || defective

          [drawn_case(witness, defective:, draws:, skip: nil) { building.call(witness, random_seed: draws.seed) },
           *(sampled ? [created_draw(witness, index:, cases:, draws:)] : [])]
        end

      # A drawn tree, the index-th of the draws, as a creation's source, in
      # its variant with automatic sizing. Its manifest is written into its
      # case directory and nothing into the cache, and its names' validity
      # is the input rules' to judge: a draw they refuse is a generator
      # defect its case reports.
      def self.created_draw(witness, index:, cases:, draws:)
        variant = Fat.variant_of(witness.parameters.fetch(:clusters))
        creation = Witnesses::CreationWitness.new(
          id: "fat.create.drawn.#{variant}.#{draws.seed}.#{index}", format: variant,
          tree: witness.tree, settings: {}, root: "manifest", intended: nil, rule: nil
        )
        PendingCase.new(id: creation.id, rules: CREATED_DRAWN_RULES, skip: nil,
                        classify: -> { cases.creation_witness(creation, random_seed: draws.seed) })
      end

      # draws.populated trees drawn for mtools (Trees.populated_tree), each
      # populated into an empty builder volume of its variant.
      # Their generator is the builder draws' with fewer names, so the
      # builder's many in-memory draws already test it for defects, and
      # only these few are drawn. A host without mtools skips them, which
      # the listing decides before any case is classified.
      def self.populated_draws(draws, cases:, building:, shell:) =
        Trees.draw(seed: draws.seed, count: draws.populated, generator: Trees.method(:populated_tree),
                   prefix: "fat.mtools.drawn").map do |witness|
          base = Base.new(builder: Builder::NAME, parameters: witness.parameters, tree: witness.tree, population: "mtools")
          defective = cases.defective?(witness)
          skip = Bases.unbuildable(base, shell)
          drawn_case(witness, defective:, draws:, skip:) { building.populated(base, id: witness.id, random_seed: draws.seed) }
        end

      # The pending case of a drawn witness, which the block builds into
      # the draws' cache and classifies, or a host skip for skip. A case
      # the block classifies must find a generator defect exactly where the
      # in-memory classification, defective, did, or that classification
      # is wrong, and the case reports that instead; a HostSkip while
      # building ends the case before that comparison. Only drawn cases build into the
      # draws' cache, one at a time, so emptying it once the case is
      # recorded leaves nothing of the case behind, however its
      # classification ended.
      def self.drawn_case(witness, defective:, draws:, skip:, &build)
        classify = lambda do
          classified = build.call
          found_defect = !classified.defect.nil?
          next classified if found_defect == defective

          memory = defective ? "the in-memory classification found a defect" : "the in-memory classification found none"
          classified.with(defect: "#{witness.id}: #{memory}, but its case #{found_defect ? "shows #{classified.defect}" : "is sound"}")
        end
        PendingCase.new(id: witness.id, rules: DRAWN_RULES, skip:, classify:, release: -> { draws.cache.clear })
      end

      # recipe as a pending case under registry.
      def self.replay(recipe, cache:, ceilings:, name_trials:, shell:, registry: Suite.registry) =
        pending(recipe, Cases.new(registry:, cache:, ceilings:, name_trials:, shell:))

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

      private_class_method :external, :drawn, :rendered_draws, :created_draw, :populated_draws, :drawn_case

      private_class_method def self.pending(recipe, cases) =
        PendingCase.new(id: recipe.id, rules: [recipe.intended_violation].compact, skip: Cases.unreplayable(recipe),
                        classify: -> { cases.replay(recipe) })
    end
  end
end
