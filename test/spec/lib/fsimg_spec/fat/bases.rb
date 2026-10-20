# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"
require "fileutils"

module FsimgSpec
  module Fat
    # FAT bases (specs/design-spec-testing.md#fixtures): a base recipe is
    # materialized once into the fixture cache by the independent builder
    # or by mkfs.fat populated with mtools, then refused if its decoded
    # geometry differs from its parameters, and checked with the read-only
    # checker and mdir. The manifest records the command, the checker
    # results and the image hash.
    module Bases
      FORMATTER = "mkfs.fat"
      VOLUME_ID = "12345678"
      # Source files carry the oracles' epoch as their modification time,
      # so mcopy -m stores the same timestamps on every run.
      EPOCH = OracleShell::EPOCH
      # mkfs.fat options for the formatter parameters other than size.
      FORMATTER_OPTIONS = {
        bytes_per_sector: "-S", sectors_per_cluster: "-s", fat_count: "-f", root_entries: "-r",
        reserved_sectors: "-R", media: "-M"
      }.freeze
      BUILDER_SOURCES = [File.join(__dir__, "builder.rb"), File.expand_path("../tree.rb", __dir__)].freeze
      # The harness code that writes and copies a formatter base's tree, and
      # the oracles' environment.
      POPULATION_SOURCES = [__FILE__, File.expand_path("../tree.rb", __dir__),
                            File.expand_path("../fixtures.rb", __dir__)].freeze
      # The preflight sample's parameters, and a bound on its few MiB.
      SAMPLE_PARAMETERS = { clusters: 5000 }.freeze
      SAMPLE_BYTES = 16 << 20

      # An 8.3 name, a long Unicode name, a nested directory and a
      # multi-cluster file.
      STANDARD_TREE = [
        TreeEntry.new(path: "HELLO.TXT", type: "file", size: 13, seed: 1),
        TreeEntry.new(path: "Long name \u00e9t\u00e9.txt", type: "file", size: 700, seed: 2),
        TreeEntry.new(path: "DIR", type: "directory", size: 0, seed: 0),
        TreeEntry.new(path: "DIR/Nested", type: "directory", size: 0, seed: 0),
        TreeEntry.new(path: "DIR/Nested/blocks.bin", type: "file", size: 5000, seed: 3)
      ].freeze

      # The base set every run starts from. FAT12 and FAT16 are empty; the
      # FAT32 base holds the standard tree.
      def self.standard = {
        "fat12" => formatter_base(blocks: 1440, fat_bits: 12, tree: []),
        "fat16" => formatter_base(blocks: 16_384, fat_bits: 16, tree: []),
        "fat32" => formatter_base(blocks: 65_536, fat_bits: 32, tree: STANDARD_TREE)
      }

      # A formatter base of blocks KiB with one sector per cluster.
      private_class_method def self.formatter_base(blocks:, fat_bits:, tree:) =
        Base.new(builder: FORMATTER, parameters: { blocks:, fat_bits:, sectors_per_cluster: 1 }, tree:)

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
      # answers the Fixture. A formatter base needs mkfs.fat, mmd and mcopy.
      # The image must stay within the profile's image ceiling.
      def self.materialize(base, cache:, shell:, tools:, ceilings:)
        check_buildable(base, ceilings)
        versions = tool_versions(base, tools)
        key = cache.key(base, versions)
        image = cache.fetch(key) { build(base, _1, shell:, tools:, ceilings:) }
        checks = checker_results(image, shell:, tools:)
        problems = geometry_problems(base, image) + checks.filter_map { |name, result| "#{name}: #{result[:status]}" unless result[:passed] }
        manifest = manifest(base, image, versions:, command: command(base, tools), checks:)
        cache.publish_manifest(key, manifest)
        Fixture.new(image:, manifest:, problems:)
      end

      # Renders a builder base into cache unless a run already published
      # it, and answers the image's path. Unlike materialize it runs no
      # checker: a witness base may break the rules on purpose. A base
      # loaded from a recipe can declare any geometry, so its image must
      # stay within the profile's image ceiling.
      def self.render(base, cache:, ceilings:)
        raise FixtureError, "only the #{Builder::NAME} builder renders without tools" unless base.builder == Builder::NAME

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

      private_class_method def self.tool_versions(base, tools)
        case base.builder
        in Builder::NAME then builder_versions
        in FORMATTER
          needed = %i[mkfs_fat mmd mcopy]
          missing = needed.reject { tools[_1] }
          raise FixtureError, "#{missing.join(", ")} unavailable for a #{FORMATTER} base" unless missing.empty?

          needed.to_h { [_1.to_s, Tools.version(tools.fetch(_1))] }
            .merge("harness" => Tools.source_version(*POPULATION_SOURCES))
        else raise FixtureError, "unknown builder #{base.builder}"
        end
      end

      # Refuses a base before anything is built for it: an unknown builder,
      # a formatter base without its size and variant or with a parameter
      # mkfs.fat does not take, an image beyond the profile's image
      # ceiling, or a tree whose files outgrow the image. The builder
      # refuses its own parameters when expected lays them out.
      private_class_method def self.check_buildable(base, ceilings)
        case base.builder
        in Builder::NAME then nil
        in FORMATTER then check_formatter_parameters(base.parameters)
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

      # Writes the image for base at path.
      private_class_method def self.build(base, path, shell:, tools:, ceilings:)
        if base.builder == Builder::NAME
          Builder.write(path, parameters: base.parameters, tree: base.tree, max_bytes: ceilings.image)
        else
          run_tool(shell, format_command(base, path, tools), chdir: File.dirname(path))
          populate(base, path, shell:, tools:)
        end
      end

      # How a base is built, for its manifest: the builder's name, or the
      # mkfs.fat options followed by the mmd and mcopy -m population.
      private_class_method def self.command(base, tools)
        return "#{Builder::NAME} builder" if base.builder == Builder::NAME

        format = format_command(base, "IMAGE", tools).drop(1).join(" ")
        base.tree.empty? ? format : "#{format}; mmd and mcopy -m of the tree"
      end

      # The mkfs.fat command for a base check_buildable accepted.
      private_class_method def self.format_command(base, path, tools)
        parameters = base.parameters
        options = FORMATTER_OPTIONS.flat_map { |name, flag| parameters.key?(name) ? [flag, parameters[name].to_s] : [] }
        [tools.fetch(:mkfs_fat), "-C", "--invariant", "-i", VOLUME_ID, "-F", parameters.fetch(:fat_bits).to_s, *options,
         path, parameters.fetch(:blocks).to_s]
      end

      # Copies the tree into the image at path with mmd and mcopy -m, from
      # source files the name funnel writes beside it, in a directory no
      # other build shares, which is removed afterwards. Host paths are
      # joined as bytes, as the funnel created them.
      private_class_method def self.populate(base, path, shell:, tools:)
        return if base.tree.empty?

        source = FixtureCache.population_directory(path).b
        Dir.mkdir(source, 0o700)
        begin
          refusal = SourceTree.write(source, base.tree)
          raise FixtureError, "the host cannot hold the base tree: #{refusal}" if refusal

          base.tree.each { File.utime(EPOCH, EPOCH, source + "/".b + _1.path.b) }
          base.tree.sort_by { [_1.path.count("/"), _1.path] }.each do |entry|
            run_tool(shell, copy_command(entry, image: path, source:, tools:), chdir: source)
          end
        ensure
          FileUtils.rm_rf(source)
        end
      end

      # The mtools command that adds entry to image: mmd for a directory,
      # mcopy -m for a file, keeping its modification time.
      private_class_method def self.copy_command(entry, image:, source:, tools:)
        target = "::/#{entry.path}"
        case entry.type
        in "directory" then [tools.fetch(:mmd), "-i", image, target]
        in "file" then [tools.fetch(:mcopy), "-m", "-i", image, source + "/".b + entry.path.b, target]
        end
      end

      private_class_method def self.run_tool(shell, argv, chdir:)
        execution = shell.run(argv, chdir:)
        return if execution.exit_status == 0

        raise FixtureError, "#{File.basename(argv.first)} #{ending(execution)}: " \
                            "#{FsimgSpec.shown(text(execution.stderr), limit: 400)}"
      end

      # How a tool's run ended: it could not start, died by a signal, or
      # exited with a status.
      private_class_method def self.ending(execution)
        if execution.launch_error then "could not start: #{execution.launch_error}"
        elsif execution.signal then "died by signal #{execution.signal}"
        else "exited #{execution.exit_status}"
        end
      end

      # Captured bytes as UTF-8 text, invalid sequences replaced.
      private_class_method def self.text(bytes) = bytes.dup.force_encoding(Encoding::UTF_8).scrub

      # The FAT counts each read-only checker supports: dosfstools reads
      # one or two FATs and refuses more.
      CHECKER_FAT_COUNTS = { fsck_fat: 1..2, fsck_msdosfs: 1..2 }.freeze

      # The read-only checker and an mdir listing, each with how it ended. A
      # tool that is unavailable, or a checker that does not support the
      # image's geometry, is recorded as such and is no problem.
      private_class_method def self.checker_results(image, shell:, tools:)
        checker = %i[fsck_fat fsck_msdosfs].find { tools[_1] }
        fat_count = Model.decode(ImageView.open(image)).geometry.fat_count
        fsck =
          if checker.nil? then { passed: true, status: "unavailable" }
          elsif !CHECKER_FAT_COUNTS.fetch(checker).cover?(fat_count) then { passed: true, status: "unsupported: #{fat_count} FATs" }
          else check(shell, [tools.fetch(checker), "-n", image], image:)
          end
        mdir = tools[:mdir] ? check(shell, [tools[:mdir], "-/", "-i", image, "::/"], image:) : { passed: true, status: "unavailable" }
        { "fsck -n" => fsck, "mdir" => mdir }
      end

      # Runs a checker of image from the image's directory.
      private_class_method def self.check(shell, argv, image:)
        execution = shell.run(argv, chdir: File.dirname(image))
        { passed: execution.exit_status == 0, status: ending(execution) }
      end

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

      private_class_method def self.manifest(base, image, versions:, command:, checks:)
        model = Model.decode(ImageView.open(image))
        geometry = Geometry.members.to_h { [_1, model.geometry.public_send(_1)] }
                           .transform_values { FsimgSpec.undefined?(_1) ? nil : _1 }
        {
          format: model.geometry.variant.then { FsimgSpec.undefined?(_1) ? nil : _1 },
          geometry:,
          builder: base.builder,
          parameters: base.parameters,
          tree_sha256: Digest::SHA256.hexdigest(CanonicalJson.generate(base.tree.map(&:to_h))),
          command:,
          validators: checks.transform_values { _1.slice(:passed, :status) },
          image_sha256: Digest::SHA256.file(image).hexdigest,
          tool_versions: versions
        }.compact
      end
    end
  end
end
