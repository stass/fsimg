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

      def observe(shell, image:, chdir:, model:)
        {
          "isoinfo" => observation(shell, :isoinfo) do |tool|
            geometry(shell, tool:, image:, chdir:, model:)
          end,
          "xorriso" => observation(shell, :xorriso) do |tool|
            output(shell, [tool, "-no_rc", "-indev", image, "-pvd_info"], chdir:)
            Observation.agrees
          end
        }
      end

      def geometry(shell, tool:, image:, chdir:, model:)
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
          Observation.agrees
        else
          Observation.disagrees("declared geometry differs")
        end
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
      private_class_method :geometry, :observation, :output, :successful
    end
  end
end
