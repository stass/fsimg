# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"

module FsimgSpec
  module Fat
    # FAT bases (specs/design-spec-testing.md#fixtures): a base recipe is
    # materialized once into the fixture cache. Population is separate from
    # formatting: the independent builder or mkfs.fat formats the volume,
    # and the tree is either rendered by the builder or, for a base whose
    # population is mtools, piped into the empty volume with mmd and mcopy.
    # A base is then refused if its decoded geometry differs from its
    # parameters, and read by the oracles: the read-only checker and the
    # mtools read-back must agree with its tree, unless a documented
    # exception excuses their disagreement (Oracles.disagreements), and
    # fatcat is recorded.
    # The manifest records the command, the observations and the image
    # hash.
    module Bases
      FORMATTER = "mkfs.fat"
      VOLUME_ID = "12345678"
      # mkfs.fat options for the formatter parameters other than size.
      FORMATTER_OPTIONS = {
        bytes_per_sector: "-S", sectors_per_cluster: "-s", fat_count: "-f", root_entries: "-r",
        reserved_sectors: "-R", media: "-M"
      }.freeze
      BUILDER_SOURCES = [File.join(__dir__, "builder.rb"), File.expand_path("../tree.rb", __dir__)].freeze
      # The harness code that runs a base's tools: mkfs.fat's command, the
      # population, the file contents, the stdin feed and the oracles'
      # environment.
      HARNESS_SOURCES = [__FILE__, File.join(__dir__, "oracles.rb"), File.expand_path("../tree.rb", __dir__),
                         File.expand_path("../process.rb", __dir__), File.expand_path("../fixtures.rb", __dir__)].freeze
      # The tools that format a formatter base, populate a base and read a
      # populated tree back.
      FORMAT_TOOLS = { FORMATTER => %i[mkfs_fat] }.freeze
      POPULATION_TOOLS = %i[mmd mcopy].freeze
      READ_BACK_TOOLS = %i[mdir mtype].freeze
      # The preflight sample's parameters, and a bound on its few MiB.
      SAMPLE_PARAMETERS = { clusters: 5000 }.freeze
      SAMPLE_BYTES = 16 << 20

      # An 8.3 name, a long Unicode name, a nested directory and a
      # multi-cluster file.
      STANDARD_TREE = [
        TreeEntry.file("HELLO.TXT", size: 13, seed: 1),
        TreeEntry.file("Long name \u00e9t\u00e9.txt", size: 700, seed: 2),
        TreeEntry.directory("DIR"),
        TreeEntry.directory("DIR/Nested"),
        TreeEntry.file("DIR/Nested/blocks.bin", size: 5000, seed: 3)
      ].freeze

      # The formatter base of each variant, holding the standard tree: an
      # image that neither fsimg nor the independent builder wrote any byte
      # of, which the case phase reads as a deterministic case.
      def self.standard = {
        "fat12" => formatter_base(blocks: 1440, fat_bits: 12),
        "fat16" => formatter_base(blocks: 16_384, fat_bits: 16),
        "fat32" => formatter_base(blocks: 65_536, fat_bits: 32)
      }

      # A formatter base of blocks KiB with one sector per cluster. A
      # formatter writes no tree, so mtools populates it.
      private_class_method def self.formatter_base(blocks:, fat_bits:) =
        Base.new(builder: FORMATTER, parameters: { blocks:, fat_bits:, sectors_per_cluster: 1 }, tree: STANDARD_TREE,
                 population: "mtools")

      # The preflight's reading sample (Preflight): a FAT16 image of the
      # standard tree that the independent builder writes into directory,
      # with its valid reading recipe, so the preflight's reading stages do
      # not depend on the fsimg writer under test.
      def self.preflight_sample(directory)
        base = Base.new(builder: Builder::NAME, parameters: SAMPLE_PARAMETERS, tree: STANDARD_TREE)
        path = File.join(directory, "sample.img")
        Builder.write(path, parameters: base.parameters, tree: base.tree, max_bytes: SAMPLE_BYTES)
        recipe = Recipe.new(id: "preflight-sample", format: "fat16", kind: "valid", stages: READING_STAGES,
                            base:, base_sha256: Digest::SHA256.file(path).hexdigest,
                            tool_versions: builder_versions.transform_keys(&:to_sym), mutations: [],
                            expected: READING_STAGES.to_h { [_1, Expectation.parse("accept")] })
        [recipe, ImageView.open(path)]
      end

      # Builds base into cache unless a run already published it, and
      # answers the Fixture, with the tools shell carries. A formatter base
      # needs mkfs.fat, and a base mtools populates mmd and mcopy. The
      # image must stay within the profile's image ceiling. content gives
      # each file's bytes (FileContent); a test may pass a faulty one, into
      # a private cache only, since content is no part of the cache key.
      def self.materialize(base, cache:, shell:, ceilings:, content: FileContent)
        check_buildable(base, ceilings)
        versions = tool_versions(base, shell.tools)
        key = cache.key(base, versions)
        image = cache.fetch(key) { build(base, _1, shell:, ceilings:, content:) }
        model = Model.decode(ImageView.open(image), tree: KnownTree.of(base.tree))
        observations = Oracles.observe(shell, image: File.basename(image), chdir: File.dirname(image), model:)
        problems = geometry_problems(base, image) + Oracles.disagreements(observations)
        manifest = manifest(base, image, versions:, command: command(base, shell.tools), observations:)
        cache.publish_manifest(key, manifest)
        Fixture.new(image:, manifest:, observations:, problems:)
      end

      # Why this host cannot build base and check its tree, or nil: shell
      # is nil where no tool may run, or the host lacks a tool that formats
      # or populates it, the read-back that checks a populated tree, or the
      # UTF-8 locale mtools needs for Unicode names.
      def self.unbuildable(base, shell)
        needed = FORMAT_TOOLS.fetch(base.builder, [])
        needed += POPULATION_TOOLS + READ_BACK_TOOLS if base.population
        return nil if needed.empty?
        return "no tool runs here to build this #{base.builder} base" if shell.nil?

        missing = needed.reject { shell.tools[_1] }
        return "#{missing.join(" and ")} unavailable" unless missing.empty?

        base.population ? shell.unavailable_reason : nil
      end

      # The FAT variant base's parameters fix.
      def self.variant(base) = expected(base).fetch(:variant)

      # Renders a builder base into cache unless a run already published
      # it, and answers the image's path. Unlike materialize it runs no
      # tool: a witness base may break the rules on purpose. A base loaded
      # from a recipe can declare any geometry, so its image must stay
      # within the profile's image ceiling.
      def self.render(base, cache:, ceilings:)
        raise FixtureError, "only the #{Builder::NAME} builder renders its own tree without tools" unless
          base.builder == Builder::NAME && base.population.nil?

        check_buildable(base, ceilings)
        cache.fetch(cache.key(base, builder_versions)) do
          Builder.write(_1, parameters: base.parameters, tree: base.tree, max_bytes: ceilings.image)
        end
      end

      # How the decoded image differs from what base's parameters demand.
      def self.geometry_problems(base, image)
        model = Model.decode(ImageView.open(image))
        return ["the boot sector does not decode"] if FsimgSpec.undefined?(model.common)

        expected(base).filter_map do |field, value|
          actual = decoded(model, field)
          "#{field} is #{actual.inspect}, not #{value}" unless actual == value
        end
      end

      # The version a builder base records: a digest of the builder's
      # sources.
      def self.builder_versions = { Builder::NAME => Tools.source_version(*BUILDER_SOURCES) }

      # The versions of what formats base and of what populates it.
      private_class_method def self.tool_versions(base, tools) =
        format_versions(base, tools).merge(population_versions(base, tools))

      # The builder's sources, or mkfs.fat's executable and the harness
      # code that spells its command.
      private_class_method def self.format_versions(base, tools)
        return builder_versions if base.builder == Builder::NAME

        executable_versions(FORMAT_TOOLS.fetch(FORMATTER), tools, base:).merge(harness_version)
      end

      # Nothing for a tree the builder renders; for one mtools populates,
      # mmd, mcopy and the harness code that pipes the tree.
      private_class_method def self.population_versions(base, tools)
        return {} unless base.population

        executable_versions(POPULATION_TOOLS, tools, base:).merge(harness_version)
      end

      private_class_method def self.harness_version = { "harness" => Tools.source_version(*HARNESS_SOURCES) }

      private_class_method def self.executable_versions(names, tools, base:)
        missing = names.reject { tools[_1] }
        raise FixtureError, "#{missing.join(", ")} unavailable for this #{base.builder} base" unless missing.empty?

        names.to_h { [_1.to_s, Tools.version(tools.fetch(_1))] }
      end

      # Refuses a base before anything is built for it: an unknown builder,
      # a formatter base mtools does not populate, without its size and
      # variant or with a parameter mkfs.fat does not take, an image beyond
      # the profile's image ceiling, or a tree whose files outgrow the
      # image. The builder refuses its own parameters when expected lays
      # them out.
      private_class_method def self.check_buildable(base, ceilings)
        case base.builder
        in Builder::NAME then nil
        in FORMATTER
          raise FixtureError, "a #{FORMATTER} base writes no tree, so mtools must populate it" unless base.population
          check_formatter_parameters(base.parameters)
        else raise FixtureError, "unknown builder #{base.builder}"
        end
        bytes = expected(base).fetch(:image_length)
        raise FixtureError, "the base's #{bytes}-byte image exceeds the #{ceilings.image}-byte ceiling" if bytes > ceilings.image

        tree_bytes = base.tree.sum { _1.type == "file" ? _1.size : 0 }
        raise FixtureError, "the base's #{tree_bytes}-byte tree exceeds its #{bytes}-byte image" if tree_bytes > bytes
      end

      private_class_method def self.check_formatter_parameters(parameters)
        unknown = parameters.keys - FORMATTER_OPTIONS.keys - %i[blocks fat_bits]
        raise FixtureError, "unknown #{FORMATTER} parameters #{unknown.join(", ")}" unless unknown.empty?

        missing = %i[blocks fat_bits] - parameters.keys
        raise FixtureError, "a #{FORMATTER} base needs #{missing.join(" and ")}" unless missing.empty?
      end

      # Writes the image for base at path: the builder's volume, holding
      # the tree unless mtools populates it, or mkfs.fat's empty volume,
      # then the population. Nothing is copied: each image is formatted at
      # path.
      private_class_method def self.build(base, path, shell:, ceilings:, content:)
        if base.builder == Builder::NAME
          Builder.write(path, parameters: base.parameters, tree: base.population ? [] : base.tree, max_bytes: ceilings.image)
        else
          run_tool(shell, format_command(base, path, shell.tools), chdir: File.dirname(path))
        end
        populate(base.tree, path, shell:, content:) if base.population
      end

      # How a base is built, for its manifest: the builder's name or the
      # mkfs.fat options, followed by the mtools population.
      private_class_method def self.command(base, tools)
        format = base.builder == Builder::NAME ? "#{Builder::NAME} builder" : format_command(base, "IMAGE", tools).drop(1).join(" ")
        base.population && !base.tree.empty? ? "#{format}; mmd and mcopy - of the tree" : format
      end

      # The mkfs.fat command for a base check_buildable accepted.
      private_class_method def self.format_command(base, path, tools)
        parameters = base.parameters
        options = FORMATTER_OPTIONS.flat_map { |name, flag| parameters.key?(name) ? [flag, parameters[name].to_s] : [] }
        [tools.fetch(:mkfs_fat), "-C", "--invariant", "-i", VOLUME_ID, "-F", parameters.fetch(:fat_bits).to_s, *options,
         path, parameters.fetch(:blocks).to_s]
      end

      # Adds tree to the image at path, parents before children: mmd makes
      # each directory, and mcopy - copies each file from stdin, where the
      # harness pipes the bytes content generates, so population writes no
      # host file. mtools stamps every entry with SOURCE_DATE_EPOCH, the
      # time the trees carry. -D s skips a name that clashes with one
      # already made, such as a case-equivalent sibling, instead of asking
      # on the terminal; the tool then fails, and the build with it. A tool
      # that ran and failed refused the tree: a PopulationError.
      private_class_method def self.populate(tree, path, shell:, content:)
        image = File.basename(path)
        tree.sort_by { [_1.path.count("/"), _1.path] }.each do |entry|
          command, input =
            case entry.type
            in "directory" then [[shell.tools.fetch(:mmd), "-D", "s", "-i", image], nil]
            in "file" then [[shell.tools.fetch(:mcopy), "-D", "s", "-i", image, "-"], content.each_chunk(entry)]
            end
          run_tool(shell, [*command, mtools_target(entry.path)], chdir: File.dirname(path), input:, refusal: PopulationError)
        end
      end

      # The ::/ argument that names path as a new entry. mtools looks its
      # parent directories up as patterns, so their pattern characters are
      # escaped, but takes the new name itself literally.
      private_class_method def self.mtools_target(path)
        parent, _, name = path.b.rpartition("/".b)
        parent.empty? ? "::/".b + name : "::/".b + Oracles.literal(parent) + "/".b + name
      end

      # Runs argv through shell, raising unless it ran to its end and exited
      # 0: refusal for a tool that ran to its end with another status, and a
      # FixtureError for one that could not start, was stopped, died by a
      # signal or left a process behind.
      private_class_method def self.run_tool(shell, argv, chdir:, input: nil, refusal: FixtureError)
        execution = shell.run(argv, chdir:, input:)
        ran_to_end = !execution.exit_status.nil? && execution.ended_by.nil? && !execution.leftover
        return if ran_to_end && execution.exit_status == 0

        error = ran_to_end ? refusal : FixtureError
        raise error, "#{File.basename(argv.first)} #{ending(execution)}: #{FsimgSpec.shown(text(execution.stderr), limit: 400)}"
      end

      # How a tool's run ended: it could not start, was stopped by the
      # shell, died by a signal, left a process behind, or exited with a
      # status.
      private_class_method def self.ending(execution)
        if execution.launch_error then "could not start: #{execution.launch_error}"
        elsif execution.ended_by then "was stopped at its #{execution.ended_by}"
        elsif execution.signal then "died by signal #{execution.signal}"
        elsif execution.leftover then "left a process behind"
        else "exited #{execution.exit_status}"
        end
      end

      # Captured bytes as UTF-8 text, invalid sequences replaced.
      private_class_method def self.text(bytes) = bytes.dup.force_encoding(Encoding::UTF_8).scrub

      # The decoded values base's parameters fix.
      private_class_method def self.expected(base)
        parameters = base.parameters
        return builder_expected(Builder::Layout.new(parameters)) if base.builder == Builder::NAME

        {
          variant: "fat#{parameters.fetch(:fat_bits)}",
          image_length: parameters.fetch(:blocks) * 1024,
          **parameters.slice(*FORMATTER_OPTIONS.keys)
        }
      end

      private_class_method def self.builder_expected(layout) = {
        variant: layout.variant, image_length: layout.total_sectors * layout.bytes_per_sector,
        bytes_per_sector: layout.bytes_per_sector, sectors_per_cluster: layout.sectors_per_cluster,
        reserved_sectors: layout.reserved_sectors, fat_count: layout.fat_count, root_entries: layout.root_entries,
        media: layout.media, fat_size: layout.fat_size, cluster_count: layout.clusters
      }

      private_class_method def self.decoded(model, field)
        case field
        in :image_length then model.image_length
        in :media then model.common.bpb_media.to_i
        else model.geometry.public_send(field)
        end
      end

      private_class_method def self.manifest(base, image, versions:, command:, observations:)
        model = Model.decode(ImageView.open(image))
        geometry = Geometry.members.to_h { [_1, model.geometry.public_send(_1)] }
                           .transform_values { FsimgSpec.undefined?(_1) ? nil : _1 }
        {
          format: model.geometry.variant.then { FsimgSpec.undefined?(_1) ? nil : _1 },
          geometry:,
          builder: base.builder,
          population: base.population,
          parameters: base.parameters,
          tree_sha256: Digest::SHA256.hexdigest(CanonicalJson.generate(base.tree.map(&:to_h))),
          command:,
          validators: observations.transform_values(&:to_h),
          image_sha256: Digest::SHA256.file(image).hexdigest,
          tool_versions: versions
        }.compact
      end
    end
  end
end
