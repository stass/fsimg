# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"

module FsimgSpec
  # Outcome counts of the Minitest tests one run executed.
  TestCounts = Data.define(:runs, :assertions, :failures, :errors, :skips)

  # The report of one run (specs/design-spec-testing.md#reports-and-regressions):
  # coverage.json for tools and summary.txt for people, both in the profile's
  # results directory. minitest_passed is Minitest's own verdict, which is
  # false when a test failed or raised and when the run was interrupted.
  class Report < Data.define(:profile, :seed, :minitest_passed, :tests)
    # Bumped by any change to the meaning or shape of coverage.json.
    SCHEMA_VERSION = 1

    # A run that executed no test proves nothing, so it does not pass.
    def passed? = minitest_passed && tests.runs.positive?

    def write(directory)
      File.write(File.join(directory, "coverage.json"), "#{JSON.pretty_generate(coverage)}\n")
      File.write(File.join(directory, "summary.txt"), summary)
    end

    def coverage = {
      schema_version: SCHEMA_VERSION,
      profile: profile.name,
      ruby: RUBY_VERSION,
      seed:,
      passed: passed?,
      tests: tests.to_h
    }

    def summary = <<~TEXT
      fsimg specification tests: profile #{profile.name}, Ruby #{RUBY_VERSION}, seed #{seed}
      tests: #{tests.runs} runs, #{tests.assertions} assertions, #{tests.failures} failures, #{tests.errors} errors, #{tests.skips} skips
      result: #{passed? ? "PASS" : "FAIL"}
    TEXT
  end
end
