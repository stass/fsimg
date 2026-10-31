# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "json"

module FsimgSpec
  # Outcome counts of the Minitest tests one run executed.
  TestCounts = Data.define(:runs, :assertions, :failures, :errors, :skips)

  # The report of one run (specs/design-spec-testing.md#reports-and-regressions):
  # coverage.json for tools and summary.txt for people, both in the profile's
  # results directory. minitest_passed is Minitest's own verdict, which is
  # false when a test failed or raised and when the run was interrupted.
  # cases is how the case phase ended: a CasePhaseResult; CasesSkipped when
  # the run named its test files or was interrupted; or CasesFailed when an
  # exception outside every case, or a signal, ended the phase. A report
  # made before the phase holds CasesSkipped.
  class Report < Data.define(:profile, :seed, :minitest_passed, :tests, :cases)
    include Checked

    # Bumped by any change to the meaning or shape of coverage.json.
    SCHEMA_VERSION = 4

    def initialize(cases: CasesSkipped.new(reason: "not run"), **fields) = super

    # A run that executed no test proves nothing, so it does not pass.
    def passed? = minitest_passed && tests.runs.positive? && !cases.is_a?(CasesFailed) && failures.empty?

    # The ids of the failed cases, by format.
    def failures = cases.is_a?(CasePhaseResult) ? cases.formats.transform_values(&:failures).reject { _2.empty? } : {}

    # Both files are written beside their names before either is renamed
    # into place, so a write that fails leaves no truncated or half-new
    # report.
    def write(directory)
      paths = { "coverage.json" => "#{JSON.pretty_generate(coverage)}\n", "summary.txt" => summary }.map do |name, text|
        path = File.join(directory, name)
        File.write("#{path}.tmp", text)
        path
      end
      paths.each { File.rename("#{_1}.tmp", _1) }
    end

    def coverage = {
      schema_version: SCHEMA_VERSION,
      profile: profile.name,
      ruby: RUBY_VERSION,
      seed:,
      passed: passed?,
      tests: tests.to_h,
      cases: case_coverage
    }

    def summary = <<~TEXT
      fsimg specification tests: profile #{profile.name}, Ruby #{RUBY_VERSION}, seed #{seed}
      tests: #{tests.runs} runs, #{tests.assertions} assertions, #{tests.failures} failures, #{tests.errors} errors, #{tests.skips} skips
      #{case_lines.join("\n")}
      result: #{passed? ? "PASS" : "FAIL"}
    TEXT

    private

    # A format's case counts in summary.txt: the total, the count of each
    # verdict, and how many drawn cases fsimg ran. bin/report, which loads
    # none of the harness, prints them in the same form.
    def case_counts(counts) = "#{counts.fetch(:total)} (#{counted(counts.except(:total, :drawn))}), #{counts.fetch(:drawn)} drawn"

    # Each oracle's observation counts in summary.txt, "none" when no
    # oracle read a case; bin/report prints them in the same form.
    def oracle_counts(oracles) =
      oracles.empty? ? "none" : oracles.map { |tool, states| "#{tool} #{counted(states)}" }.join("; ")

    # "COUNT LABEL" for each label and count.
    def counted(pairs) = pairs.map { |label, count| "#{count} #{label}" }.join(", ")

    def case_coverage
      case cases
      in CasesSkipped(reason:) then { skipped: reason }
      in CasesFailed(reason:) then { error: reason }
      in CasePhaseResult(sandbox:, formats:)
        { sandbox:, formats: formats.transform_values { { cases: _1.cases, oracles: _1.oracles, failures: _1.failures, rules: _1.rules } } }
      end
    end

    # Why the phase did not run, or one line per format with its case
    # counts, its oracles' observations and how many rules fsimg asserted,
    # then each case that did not pass, whether fsimg failed it or the
    # harness could not judge it.
    def case_lines
      case cases
      in CasesSkipped(reason:) then ["cases: not run (#{reason})"]
      in CasesFailed(reason:) then ["cases: failed (#{reason})"]
      in CasePhaseResult(sandbox:, formats:)
        formats.flat_map do |format, result|
          states = result.rules.map { _1[:state] }.tally.sort
          ["#{format} cases: #{case_counts(result.cases)}; sandbox #{sandbox}",
           "#{format} oracles: #{oracle_counts(result.oracles)}",
           "#{format} rules: #{counted(states)}",
           *result.failures.map { "not passed: #{format} #{_1}" }]
        end
      end
    end
  end
end
