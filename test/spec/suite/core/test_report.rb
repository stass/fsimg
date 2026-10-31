# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "json"
require "minitest"
require "rbconfig"
require "tmpdir"
require "fsimg_spec"

# The report's pass gate and the case half of coverage.json
# (specs/design-spec-testing.md#reports-and-regressions): a run passes only
# when its tests pass, its case phase did not fail as a whole and no case
# failed; a skipped phase fails nothing.
class TestReport < Minitest::Test
  PROFILE = FsimgSpec.fetch_profile("quick")
  BIN_REPORT = File.expand_path("../../bin/report", __dir__)
  TESTS = FsimgSpec::TestCounts.new(runs: 3, assertions: 9, failures: 0, errors: 0, skips: 0)

  def test_a_skipped_phase_passes_and_says_why
    report = report(FsimgSpec::CasesSkipped.new(reason: "test files named"))

    assert_predicate report, :passed?
    assert_equal({ skipped: "test files named" }, report.coverage.fetch(:cases))
    assert_match(/^cases: not run \(test files named\)$/, report.summary)
  end

  def test_a_failed_phase_fails_the_run
    report = report(FsimgSpec::CasesFailed.new(reason: "HarnessError: case ids repeat: x"))

    refute_predicate report, :passed?
    assert_equal({ error: "HarnessError: case ids repeat: x" }, report.coverage.fetch(:cases))
    assert_match(/^cases: failed \(HarnessError: case ids repeat: x\)$/, report.summary)
  end

  # A failed case and a case's harness error each fail the run; a host
  # skip does not.
  def test_a_failed_case_fails_the_run
    passing = phase(failures: [], cases: { total: 2, passed: 1, host_skip: 1, drawn: 0 })
    failing = phase(failures: ["fat.x"], cases: { total: 2, passed: 1, harness_error: 1, drawn: 0 })

    assert_predicate report(passing), :passed?
    refute_predicate report(failing), :passed?
    assert_equal({ "fat" => ["fat.x"] }, report(failing).failures)
    assert_match(/^not passed: fat fat\.x$/, report(failing).summary)
  end

  # bin/report reads what Report#write writes, at the same schema version,
  # and refuses any other version.
  def test_bin_report_reads_the_written_report
    report = report(phase(failures: [], cases: { total: 1, passed: 1, failed: 0, drawn: 1 }))
    Dir.mktmpdir do |directory|
      report.write(directory)
      path = File.join(directory, "coverage.json")
      output = IO.popen([RbConfig.ruby, BIN_REPORT, path], err: %i[child out], &:read)
      assert_equal 0, Process.last_status.exitstatus, output
      assert_match(/^fat: 1 \(1 passed, 0 failed\), 1 drawn$/, output)
      assert_match(/^fat cases: 1 \(1 passed, 0 failed\), 1 drawn; /, report.summary)
      oracles = "fsck 1 agrees; mtools 1 disagrees, 2 not_applicable"
      assert_match(/^fat oracles: #{oracles}$/, output)
      assert_match(/^fat oracles: #{oracles}$/, report.summary)

      File.write(path, JSON.generate(report.coverage.merge(schema_version: FsimgSpec::Report::SCHEMA_VERSION + 1)))
      output = IO.popen([RbConfig.ruby, BIN_REPORT, path], err: %i[child out], &:read)
      assert_equal 2, Process.last_status.exitstatus, output
    end
  end

  # A downloaded coverage.json is untrusted: bin/report escapes control and
  # format characters, such as a right-to-left override, in what it prints.
  def test_bin_report_escapes_what_it_prints
    Dir.mktmpdir do |directory|
      path = File.join(directory, "coverage.json")
      File.write(path, JSON.generate(report(FsimgSpec::CasesSkipped.new(reason: "x\u202ey\ez")).coverage))
      output = IO.popen([RbConfig.ruby, BIN_REPORT, path], err: %i[child out], &:read)

      assert_equal 0, Process.last_status.exitstatus, output
      assert_includes output, 'cases: not run (x\u{202e}y\u{1b}z)'
      refute_match(/[\u202e\e]/, output)
    end
  end

  # A reason names an exception's class, or the nearest named ancestor of
  # an anonymous one, whose own name would carry a heap address.
  def test_an_error_label_names_a_class_without_an_address
    anonymous = Class.new(FsimgSpec::RecipeError)

    assert_equal 'RecipeError: "bad"', FsimgSpec.error_label(anonymous.new("bad"))
  end

  # A replay holds test/spec-results/replay/, which no profile's results
  # directory may share.
  def test_no_profile_takes_the_replay_directory
    refute_includes FsimgSpec::PROFILES.keys, FsimgSpec::Replay::DIRECTORY
  end

  private

  def report(cases) = FsimgSpec::Report.new(profile: PROFILE, seed: 1, minitest_passed: true, tests: TESTS, cases:)

  # A phase whose FAT cases the oracles read: the checker agreed once, and
  # the mtools read-back disagreed once and applied to two images not at
  # all.
  ORACLES = { "fsck" => { "agrees" => 1 }, "mtools" => { "disagrees" => 1, "not_applicable" => 2 } }.freeze

  def phase(failures:, cases:) =
    FsimgSpec::CasePhaseResult.new(sandbox: "unavailable",
                                   formats: { "fat" => FsimgSpec::FormatResult.new(rules: [], cases:, oracles: ORACLES, failures:) })
end
