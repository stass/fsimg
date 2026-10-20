# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # One recipe rebuilt and rerun (specs/design-spec-testing.md#reports-and-regressions):
  # its classification and expectations are derived again from the current
  # rules, and how they differ from what the recipe recorded is reported
  # beside the result. It works in test/spec-results/replay/ under the
  # build directory, which it holds and empties like a profile's results
  # directory, so a replay never disturbs a run's results; no profile takes
  # that name.
  module Replay
    DIRECTORY = "replay"

    # Replays the recipe at path under profile, with the suite of its format
    # unless one is given, and answers its CaseResult and drift. It takes
    # the steps of a run's case, CasePhase's skipped, classify and
    # result_of, but without the run's funnel: a recipe that cannot be read
    # or classified raises RecipeError, and any other fault propagates, so
    # that the caller can tell a recipe this revision cannot judge from a
    # harness fault.
    def self.call(path, profile:, build_directory:, environment:, suite: nil)
      recipe = Recipe.read(path, ceilings: profile.ceilings)
      suite ||= CasePhase.suite_for(recipe.format)
      results = File.join(build_directory, "test", "spec-results", DIRECTORY)
      ResultsDirectory.hold(results) do
        setup = CasePhase.prepare(results:, profile:, build_directory:, environment:)
        pending = suite.replay(recipe, cache: setup.cache, ceilings: profile.ceilings)
        next [CasePhase.skipped(pending), []] if pending.skip

        classified = CasePhase.classify(pending)
        [CasePhase.result_of(classified, setup:, cases_directory: File.join(results, "cases")), classified.kase.drift]
      end
    end
  end
end
