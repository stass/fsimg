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
# paths. Without test files, every suite/**/test_*.rb file runs. The exit
# status is 0 when the run passes, 1 when it does not, 2 for a usage error
# and 77 when a locked gem is not installed. Minitest's SEED environment
# variable fixes the test order.

# Only the locked gems: a run never rewrites Gemfile.lock, installs a gem or
# reaches the network. Temporary settings outrank every Bundler
# configuration, a local test/spec/.bundle/config included. Any other
# failure to load the bundle, such as a Gemfile changed without relocking,
# raises and fails the run.
# Ruby matches a rescue list in order, so Bundler::GemNotFound is not looked
# up when LoadError reports a missing Bundler.
begin
  require "bundler"
  Bundler.settings.temporary(frozen: true, auto_install: false) { Bundler.setup }
rescue LoadError, Bundler::GemNotFound => e
  warn e.message
  puts "SKIP: specification tests require the locked gems; run bundle install --gemfile=test/spec/Gemfile"
  exit 77
end

require "minitest"
require "optparse"
require "stringio"

$LOAD_PATH.unshift(File.join(__dir__, "lib"))
require "fsimg_spec"

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
      ResultsDirectory.hold(directory) do
        report = run_tests(profile, test_files)
        report.write(directory)
        puts report.summary
        report.passed? ? 0 : 1
      end
    end

    private_class_method def self.parse(argv)
      profile_name = nil
      parser = OptionParser.new("usage: run.rb --profile=NAME [TEST_FILE...]")
      parser.on("--profile=NAME") { profile_name = _1 }
      test_files = parser.parse(argv)
      raise UsageError, "--profile is required" if profile_name.nil?

      [FsimgSpec.fetch_profile(profile_name), test_files.empty? ? suite_files : checked(test_files)]
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

    private_class_method def self.run_tests(profile, test_files)
      test_files.each { require _1 }
      Minitest.register_plugin(MinitestCounts)
      # Minitest.run answers nil, not false, for an interrupted run.
      minitest_passed = Minitest.run([]) == true
      Report.new(profile:, seed: Minitest.seed, minitest_passed:, tests: MinitestCounts.fetch)
    end
  end
end

begin
  exit FsimgSpec::Run.call(ARGV)
rescue FsimgSpec::UsageError => e
  warn "run.rb: #{e.message}"
  exit 2
end
