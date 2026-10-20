# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # The record of one final mismatch, failures/ID/ in the results directory
  # (specs/design-spec-testing.md#reports-and-regressions). Everything in it
  # comes from the case and from what the runner captured in memory, never
  # from a file fsimg could have replaced. CI uploads failures/, so the
  # record carries no environment, and its own fields name no host path;
  # the stage captures and a harness error's detail quote fsimg and Ruby,
  # whose messages can.
  module FailureRecord
    # The largest image the record keeps.
    IMAGE_BYTES = 64 << 20

    # replay.sh, created mode 0600 like every file of the record, so it runs
    # as sh PATH/replay.sh from the build directory. It holds nothing from
    # the case, and the profile's name, which it interpolates, is checked to
    # be a plain word.
    def self.replay_script(profile)
      raise HarnessError, "profile name #{FsimgSpec.shown(profile.name)} is no plain word" unless
        profile.name.match?(/\A[a-z]+\z/)

      <<~SH
        #!/bin/sh
        # Rebuilds and reruns this failure's case with the current rules, under
        # the profile that recorded it. Run it from the build directory.
        exec test/spec.sh --replay="$(dirname "$0")/case.json" #{profile.name}
      SH
    end

    # Writes the record of the case id, which ended as result under
    # profile. kase is its Case, or nil when it could not be classified,
    # which leaves only result.json to record; image is its ImageView, or
    # nil for a creation case, whose image create writes in the case
    # directory. result.json is written last, so a record that could not
    # be completed holds no verdict that the run's report contradicts.
    def self.write(failures, id:, kase:, result:, image:, profile:)
      directory = File.join(failures, id)
      Dir.mkdir(directory, 0o700)
      if kase
        write_file(directory, "case.json", kase.recipe.dump)
        write_file(directory, "fixture.json", "#{CanonicalJson.generate(fixture(kase.recipe))}\n")
        result.stages.each do |stage|
          write_file(directory, "#{stage.stage}.stdout", stage.execution.stdout)
          write_file(directory, "#{stage.stage}.stderr", stage.execution.stderr)
        end
        image.write(File.join(directory, "image.img")) if image && image.length <= IMAGE_BYTES
        write_file(directory, "replay.sh", replay_script(profile))
      end
      write_file(directory, "result.json", "#{JSON.pretty_generate(result_fields(result, profile))}\n")
      directory
    end

    private_class_method def self.write_file(directory, name, bytes)
      answer = NameFunnel.create_file(directory, name) { _1.write(bytes) }
      raise HarnessError, "#{name} exists in the new failure record #{File.basename(directory)}" unless answer == :created
    end

    # A reading case's base, its digest and the versions of the tools that
    # built it. A creation case has no base and builds with no tool, so its
    # record names no tool version.
    private_class_method def self.fixture(recipe)
      return { tool_versions: {} } unless recipe.reading?

      { base: { builder: recipe.base.builder, parameters: recipe.base.parameters, tree: recipe.base.tree.map(&:to_h) },
        base_sha256: recipe.base_sha256, tool_versions: recipe.tool_versions }
    end

    # The profile, the verdict, and each stage's outcome, timings and
    # safety problems.
    private_class_method def self.result_fields(result, profile) = {
      profile: profile.name, verdict: result.verdict, detail: result.detail,
      stages: result.stages.map do |stage|
        execution = stage.execution
        { stage: stage.stage, outcome: stage.outcome.to_s, detail: stage.outcome.detail, problems: stage.problems,
          exit_status: execution.exit_status, signal: execution.signal, ended_by: execution.ended_by,
          wall_seconds: execution.wall_seconds.round(3), cpu_seconds: execution.cpu_seconds.round(3) }
      end
    }
  end
end
