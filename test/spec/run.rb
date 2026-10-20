# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

# Runs one profile of the specification-driven test harness and writes its
# report into test/spec-results/PROFILE/ under the build directory:
#
#   run.rb --profile=NAME [TEST_FILE...]
#
# test/spec.sh is the entry point. It checks the Ruby version, names the
# Gemfile and exports FSIMG_BUILDDIR, FSIMG_BIN and the development tool
# paths. Without test files, every suite/**/test_*.rb file runs and then
# the case phase runs every format's cases through fsimg; naming test files
# runs only those and skips the case phase. The exit status is 0 when the
# run passes, 1 when it does not, 2 for a usage error and 77 when a locked
# gem is not installed. Minitest's SEED environment variable fixes the test
# order.

require_relative "bundle"

require "minitest"
require "optparse"
require "stringio"

require "fsimg_spec"
require "fsimg_spec/fat"

module FsimgSpec
  # Minitest attaches reporters only through plugins: Minitest.run calls
  # minitest_plugin_init on each registered plugin once it has built its own
  # reporters. This one adds a statistics reporter, whose counts are complete
  # once Minitest.run has returned; it prints nothing, so its output is
  # discarded.
  module MinitestCounts
    def self.minitest_plugin_init(_options) = Minitest.reporter << statistics

    def self.fetch = TestCounts.new(
      runs: statistics.count,
      assertions: statistics.assertions,
      failures: statistics.failures,
      errors: statistics.errors,
      skips: statistics.skips
    )

    private_class_method def self.statistics =
      @statistics ||= Minitest::StatisticsReporter.new(StringIO.new)
  end

  # One run of one profile, from the command line to the exit status.
  module Run
    SUITE_GLOB = "suite/**/test_*.rb"

    def self.call(argv)
      profile, test_files = parse(argv)
      directory = profile.results_directory(build_directory)
      fsimg_binary if test_files.nil?
      ResultsDirectory.hold(directory) do
        report, interrupted = run_tests(profile, test_files || suite_files)
        ending = case_phase(profile, directory, named: !test_files.nil?, interrupted:)
        report = report.with(cases: ending.cases)
        begin
          report.write(directory)
          puts report.summary
        rescue StandardError => e
          # Dying by a signal prints nothing, so the failed write is said
          # here before the signal is raised again.
          warn "run.rb: the report was not written: #{FsimgSpec.error_label(e)}" if ending.fatal
          raise
        ensure
          raise ending.fatal if ending.fatal
        end

        report.passed? ? 0 : 1
      end
    end

    private_class_method def self.parse(argv)
      profile_name = nil
      parser = OptionParser.new("usage: run.rb --profile=NAME [TEST_FILE...]")
      parser.on("--profile=NAME") { profile_name = _1 }
      test_files = parser.parse(argv)
      raise UsageError, "--profile is required" if profile_name.nil?

      [FsimgSpec.fetch_profile(profile_name), test_files.empty? ? nil : checked(test_files)]
    rescue OptionParser::ParseError => e
      raise UsageError, e.message
    end

    # Every suite file, as an absolute path, in a stable load order.
    private_class_method def self.suite_files =
      Dir.glob(SUITE_GLOB, base: __dir__).sort.map { File.join(__dir__, _1) }

    private_class_method def self.checked(test_files)
      missing = test_files.reject { File.file?(_1) }
      raise UsageError, "no such test file: #{missing.join(", ")}" unless missing.empty?

      test_files.map { File.expand_path(_1) }
    end

    private_class_method def self.build_directory =
      ENV.fetch("FSIMG_BUILDDIR") { raise UsageError, "FSIMG_BUILDDIR is not set; run through test/spec.sh" }

    # A full run needs the fsimg binary, which is checked before the
    # results directory is emptied.
    private_class_method def self.fsimg_binary
      fsimg = ENV.fetch("FSIMG_BIN", "")
      raise UsageError, "FSIMG_BIN names no fsimg binary; run through test/spec.sh" unless File.executable?(fsimg)
    end

    # How the case phase ended (CasePhase.guarded): a run that names its
    # test files or was interrupted skips it.
    private_class_method def self.case_phase(profile, directory, named:, interrupted:)
      return CasePhase::Ending.new(cases: CasesSkipped.new(reason: "test files named"), fatal: nil) if named
      return CasePhase::Ending.new(cases: CasesSkipped.new(reason: "interrupted"), fatal: nil) if interrupted

      CasePhase.guarded(profile:, results: directory, build_directory:, environment: ENV.to_h)
    end

    # The report of the Minitest run, and whether it was interrupted.
    private_class_method def self.run_tests(profile, test_files)
      test_files.each { require _1 }
      Minitest.register_plugin(MinitestCounts)
      # Minitest.run answers nil, not false, for an interrupted run.
      answer = Minitest.run([])
      [Report.new(profile:, seed: Minitest.seed, minitest_passed: answer == true, tests: MinitestCounts.fetch), answer.nil?]
    end
  end
end

begin
  exit FsimgSpec::Run.call(ARGV)
rescue FsimgSpec::UsageError => e
  warn "run.rb: #{e.message}"
  exit 2
end
