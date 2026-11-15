# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    Classified = Data.define(:kase, :image, :model, :created, :defect, :oracles)

    # The source model of an APFS creation. Every input but an explicit
    # size is known: whether the writer's layout fits a size has no
    # independent plan short of the writer's own, so an otherwise-valid
    # explicit size leaves the input scope unclassified, as ISO 9660's
    # does (specs/design-spec-testing.md#cases). The created image is
    # still decoded and judged in full.
    SourceModel = Data.define(:format, :settings, :tree, :unclassified) do
      def unclassified_scopes = unclassified
      def record_problems(_result) = []
    end

    # The validation scope each verify stage reports.
    VALIDATION_SCOPES = { "verify" => "structural", "verify-deep" => "initialized-payload" }.freeze

    # The image create wrote, as the reading stages' records are judged
    # against it (specs/design-spec-testing.md#expectation-derivation):
    # model is its Model, decoded with its namespace, and label the volume
    # name info must report.
    CreatedImage = Data.define(:model, :label) do
      def tree = model.tree

      # How fsimg's records for an accepting reading stage disagree with
      # the created image: info names APFS and the label, ls lists the
      # tree's paths, types and sizes, extract walks the whole tree with
      # its digests, and verify counts the tree's non-directories, its
      # directories with the root, and the bytes of its files and link
      # targets. Values from fsimg are shown in every message.
      def record_problems(result)
        records = result.outcome.records
        case result.stage
        in "create" then raise Error, "an image model does not judge create; its SourceModel does"
        in _ if FsimgSpec.undefined?(tree) then ["the model cannot walk the volume's namespace: #{tree.reason}"]
        in "info" then info_problems(records)
        in "ls" then KnownTree.differences("ls", actual: listing(records), expected: tree.map { _1.with(path: "/".b + _1.path, sha256: nil) })
        in "extract" then KnownTree.differences("extract", actual: KnownTree.from_walk(result.tree), expected: tree)
        in "verify" | "verify-deep" then count_problems(result.stage, records)
        end
      end

      private

      def sole(records, type, stage)
        found = records.select { _1["type"] == type }
        found.one? ? [found.first, nil] : [nil, "#{stage} wrote #{found.size} #{type} records, not one"]
      end

      def info_problems(records)
        info, problem = sole(records, "info", "info")
        return [problem] if problem

        { "format" => "apfs", "label" => label }.filter_map do |field, value|
          "info reports #{field} #{FsimgSpec.shown(info[field])}, not #{FsimgSpec.shown(value)}" unless info[field].to_s.b == value.b
        end
      end

      def listing(records) = records.select { _1["type"] == "entry" }.map do |row|
        kind = row["kind"].to_s
        size = %w[file symlink].include?(kind) && row["size_bytes"].to_s.match?(/\A[0-9]{1,20}\z/) ? Integer(row["size_bytes"], 10) : nil
        WalkedEntry.new(path: row["path"].to_s.b, type: kind, size:, sha256: nil)
      end

      def count_problems(stage, records)
        verify, problem = sole(records, "verify", stage)
        return [problem] if problem

        directories = tree.count { _1.type == "directory" }
        expected = {
          "complete" => true, "validation_scope" => VALIDATION_SCOPES.fetch(stage),
          "files" => tree.size - directories, "directories" => directories + 1,
          "logical_bytes" => tree.sum { _1.size || 0 }
        }
        expected.filter_map do |field, value|
          "#{stage} reports #{field} #{FsimgSpec.shown(verify[field])}, not #{value}" unless verify[field].to_s == value.to_s
        end
      end
    end

    # Creation cases through the same Case/Registry funnel as every format:
    # create is judged by the source model and the input rules, and once
    # it accepts, the created image by every other rule, by WriterLayout
    # and by the reading stages' records; a valid creation's image then
    # goes to the native checkers. The writer's profile has no reading
    # fixture of its own: the native captures' mutations stay in the
    # Minitest suite, and no checker reads them.
    class Cases
      def initialize(registry:, ceilings:, name_trials:, oracles: true)
        @registry = registry
        @ceilings = ceilings
        @name_trials = name_trials
        @oracles = oracles
      end

      def create(creation, random_seed: nil)
        # An input rule's violation ends the case at create; any other
        # intended rule is the created image's, which the reading stages
        # check.
        stages = creation.intended && @registry.fetch(creation.intended).input? ? ["create"] : ["create", *READING_STAGES]
        recipe = Recipe.new(id: creation.id, format: "apfs", kind: creation.intended ? "targeted" : "valid",
                            intended_violation: creation.intended, source: creation.source, stages:, random_seed:,
                            expected: stages.to_h { [_1, Expectation.new(unclassified: true)] })
        classified = classify(recipe)
        kase = classified.kase.with(recipe: classified.kase.recorded)
        classified.with(kase:, defect: kase.harness_defect)
      end

      def replay(recipe)
        raise RecipeError, "APFS replays creation recipes only" if recipe.reading?

        classify(recipe)
      end

      # The source model's classification, which a draw checks before it
      # runs anything.
      def source_classification(source) = @registry.classify_source(source_model(source), environment)

      private

      def environment = Environment.new(forced_format: nil, trial_answers: NO_TRIAL)

      def source_model(source)
        plain = FsimgSpec::SourceModel.new(format: "apfs", settings: source.settings, tree: source.tree)
        sized = source.settings.key?(:size) && @registry.classify_source(plain, environment).violations.empty?
        SourceModel.new(format: "apfs", settings: source.settings, tree: source.tree, unclassified: sized ? ["input"].freeze : [].freeze)
      end

      def classify(recipe)
        source = source_model(recipe.source)
        trials = @name_trials.answers_for(recipe.source.tree, stages: recipe.stages)
        kase = Case.derive(recipe, Case::CREATED_IMAGE.with(trial_answers: trials), registry: @registry,
                           source_classification: @registry.classify_source(source, environment))
        created = ->(path) { created(path, source, trials) }
        defect = kase.harness_defect
        defect ||= "#{recipe.id}: recipe classification drifted" if recipe.promoted? && kase.drift.any?
        Classified.new(kase:, image: nil, model: source, created:, defect:, oracles: oracle_hook(recipe))
      end

      # The created image's CreatedImage, its classification and why it is
      # not the profile's layout, or nil.
      def created(path, source, trials)
        view = ImageView.open(path)
        model = Model.decode(view, namespace: true)
        image = CreatedImage.new(model:, label: WriterLayout.label(source.settings))
        problems = WriterLayout.problems(model, view, source)
        [image, @registry.classify(model, environment.with(trial_answers: trials)), problems.empty? ? nil : problems.join("; ")]
      end

      # fsck_apfs -n and apfsck over a valid creation's image, which no
      # mutation touched; a disagreement fails the case.
      def oracle_hook(recipe)
        return nil unless @oracles && recipe.kind == "valid"

        lambda do |directory, _image|
          observations = Oracles.observe(Oracles::Unmutated.created(directory))
          disagreements = observations.filter_map { |tool, seen| "#{tool}: #{seen.detail}" if seen.state == "disagrees" }
          Observed.new(observations:, problem: disagreements.empty? ? nil : "the oracles disagree with the created image: #{disagreements.join("; ")}")
        end
      end
    end
  end
end
