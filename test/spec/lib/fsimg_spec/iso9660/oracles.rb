# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    # Observe only independently built valid images or accepted creations.
    # Isoinfo checks declared geometry; xorriso loads the descriptor/tree.
    # These observations do not claim a file-content read-back, which the
    # separate writer-qualification suite owns.
    module Oracles
      module_function

      def disagreements(observations) = observations.filter_map do |tool, result|
        "#{tool}: #{result.detail}" if result.state == "disagrees"
      end

      def observe(shell, image:, chdir:, model:, settings: {})
        {
          "isoinfo" => observation(shell, :isoinfo) do |tool|
            geometry(shell, tool:, image:, chdir:, model:, settings:)
          end,
          "xorriso" => observation(shell, :xorriso) do |tool|
            text = output(shell, [tool, "-no_rc", "-indev", image, "-pvd_info"], chdir:)
            metadata_report(text, model:, tool: :xorriso, settings:)
          end
        }
      end

      def geometry(shell, tool:, image:, chdir:, model:, settings:)
        # An explicit charset avoids a locale-selection notice on stderr,
        # keeping the bounded short-read diagnostic exact across hosts.
        execution = shell.run([tool, "-j", "utf-8", "-d", "-i", image], chdir:, capture_bytes: 1 << 20)
        # cdrtools 3.02a09 reads 32 blocks from block 16 before describing
        # the volume. Accept no other diagnostic as this consumer limit.
        if model.image_length < 48 * Model::BLOCK && execution.exit_status == 255 &&
            !execution.launch_error && !execution.ended_by && !execution.leftover && !execution.signal &&
            execution.stdout.empty? && execution.stderr == "isoinfo: Short read on old image\n".b
          return Observation.not_applicable("isoinfo's descriptor window requires 48 physical blocks")
        end

        text = successful(execution)
        block = text[/^Logical block size is:\s*(\d+)/, 1]
        count = text[/^Volume size is:\s*(\d+)/, 1]
        if block && count && block.to_i == 2048 && count.to_i == model.volume.fields.space_le
          metadata_report(text, model:, tool: :isoinfo, settings:)
        else
          Observation.disagrees("declared geometry differs")
        end
      end

      TEXT_LABELS = {
        "volume_set" => ["Volume Set Id", "Volume set id"],
        "publisher" => ["Publisher Id", "Publisher id"],
        "preparer" => ["Preparer Id", "Data preparer id"],
        "application" => ["App Id", "Application id"],
        "copyright_file" => ["CopyrightFile", "Copyright File id"],
        "abstract_file" => ["Abstract File", "Abstract File id"],
        "biblio_file" => ["Biblio File", "Bibliographic File id"]
      }.freeze
      DATE_LABELS = {"created" => "Creation Time", "modified" => "Modif. Time",
        "expires" => "Expir. Time", "effective" => "Eff. Time"}.freeze
      SETTING_NAMES = {volset: "volume_set", copyright: "copyright_file", abstract: "abstract_file", biblio: "biblio_file"}.freeze

      # xorriso reports all four digit dates. isoinfo -d exposes text only.
      # Compare the tool's parsed result, not just its successful exit.
      def metadata_report(text, model:, tool:, settings: {})
        return Observation.agrees unless model.volume.respond_to?(:raw)

        raw = model.volume.raw
        fields = DescriptorMetadata.texts(raw)
        fields = fields.merge(DescriptorMetadata.dates(raw)) if tool == :xorriso
        problems = recipe_problems(settings, fields:, tool:) + tool_problems(text, fields:, raw:, tool:)
        problems.empty? ? Observation.agrees : Observation.disagrees(problems.join("; "))
      end

      # The descriptor fields the recipe set that differ from the image.
      def recipe_problems(settings, fields:, tool:)
        settings
          .map { |key, value| [SETTING_NAMES.fetch(key, key.to_s), value] }
          .select { |name, _| TEXT_LABELS.key?(name) || (DATE_LABELS.key?(name) && tool == :xorriso) }
          .reject { |name, value| fields.fetch(name, "") == recipe_value(name, value) }
          .map { |name, _| "#{name} differs from recipe" }
      end

      def recipe_value(name, value) =
        DATE_LABELS.key?(name) ? Time.at(value).utc.strftime("%Y-%m-%d %H:%M:%S") : DescriptorMetadata.trim_text(value)

      # The descriptor fields the tool reports differently from the image.
      def tool_problems(text, fields:, raw:, tool:)
        fields
          .map { |name, expected| [name, expected, reported_field(text, name:, tool:)] }
          .map { |name, expected, actual| [name, DATE_LABELS.key?(name) ? raw.byteslice(DescriptorMetadata::DATES.fetch(name), 16) : expected, actual] }
          .reject { |_, expected, actual| actual == expected }
          .map { |name, expected, actual| "#{name}: expected #{FsimgSpec.shown(expected)}, observed #{FsimgSpec.shown(actual.to_s)}" }
      end

      def reported_field(text, name:, tool:)
        label = TEXT_LABELS.key?(name) ? TEXT_LABELS.fetch(name).fetch(tool == :xorriso ? 0 : 1) : DATE_LABELS.fetch(name)
        actual = text[/^#{Regexp.escape(label)}\s*:\s*(.*?)\s*$/, 1]
        tool == :xorriso && TEXT_LABELS.key?(name) ? actual&.delete_prefix("'")&.delete_suffix("'") : actual
      end

      def observation(shell, name)
        tool = shell.tools[name]
        return Observation.unavailable("configured #{name} is absent") unless tool
        return Observation.unavailable(shell.unavailable_reason) if shell.unavailable_reason

        yield tool
      rescue FixtureError => error
        Observation.disagrees(FsimgSpec.error_label(error))
      end

      def output(shell, arguments, chdir:)
        successful(shell.run(arguments, chdir:, capture_bytes: 1 << 20))
      end

      def successful(execution)
        raise HarnessError, "ISO oracle could not start" if execution.launch_error
        if execution.ended_by || execution.leftover || execution.signal || execution.exit_status != 0
          raise FixtureError, "ISO oracle did not finish successfully: #{FsimgSpec.shown(execution.stderr)}"
        end
        execution.stdout
      end
      private_class_method :geometry, :observation, :output, :successful, :recipe_problems, :recipe_value,
        :tool_problems, :reported_field
    end
  end
end
