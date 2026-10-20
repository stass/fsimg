# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"
require "json"
require "minitest"
require "rbconfig"
require "tmpdir"

# test/spec.sh and run.rb: the environment the wrapper establishes, its skip
# when the gems are absent, profile selection, the pass verdict and report
# writing. Each run.rb case starts a separate run on stub suites under a
# temporary build directory, so it never touches this run's results.
class TestRun < Minitest::Test
  RUN_RB = File.expand_path("../../run.rb", __dir__)
  GEMFILE = File.expand_path("../../Gemfile", __dir__)
  TOOL_VARIABLES = %w[MKFS_FAT FSCK_FAT NEWFS_MSDOS FSCK_MSDOSFS MCOPY MMD MDEL MDIR FATCAT].freeze

  PASSING_SUITE = <<~RUBY
    # frozen_string_literal: true

    require "minitest"

    class TestStub < Minitest::Test
      def test_passes = assert(true)
    end
  RUBY

  FAILING_SUITE = <<~RUBY
    # frozen_string_literal: true

    require "minitest"

    class TestStubFailure < Minitest::Test
      def test_fails = flunk("seeded failure")
    end
  RUBY

  # What SIGINT raises in the test that is running when it arrives.
  INTERRUPTED_SUITE = <<~RUBY
    # frozen_string_literal: true

    require "minitest"

    class TestStubInterrupt < Minitest::Test
      def test_interrupts = raise(Interrupt)
    end
  RUBY

  EMPTY_SUITE = "# frozen_string_literal: true\n"

  Outcome = Data.define(:status, :output)

  def test_wrapper_exports_the_build_environment
    build_directory = ENV.fetch("FSIMG_BUILDDIR")

    assert File.executable?(ENV.fetch("FSIMG_BIN")), "FSIMG_BIN names an executable fsimg"
    assert File.file?(File.join(build_directory, "test", "spec.sh")), "FSIMG_BUILDDIR holds test/spec.sh"
    assert_equal File.realpath(GEMFILE), File.realpath(ENV.fetch("BUNDLE_GEMFILE"))
    assert_empty TOOL_VARIABLES.reject { ENV.key?(_1) }, "every development tool path is exported, empty when absent"
  end

  # The nested wrapper is the real one, so its run must skip for the gems
  # alone. BUNDLE_APP_CONFIG keeps a local Bundler configuration, such as an
  # install path set in test/spec/.bundle, from overriding BUNDLE_PATH, and
  # GEM_HOME and GEM_PATH hide gems installed in the interpreter's own gem
  # directory. BUNDLER_FORCE_TTY guards the check's form: bundler/setup
  # exits 7 on a terminal instead of raising. The unknown profile makes a
  # bundle that wrongly loads end in run.rb's usage error, before it would
  # wait for the results lock this run holds.
  def test_wrapper_skips_when_the_locked_gems_are_absent
    Dir.mktmpdir do |home|
      wrapper = File.join(ENV.fetch("FSIMG_BUILDDIR"), "test", "spec.sh")
      environment = {
        "PATH" => ENV.fetch("PATH"),
        "HOME" => home,
        "BUNDLE_APP_CONFIG" => home,
        "BUNDLER_FORCE_TTY" => "1",
        "BUNDLE_PATH" => File.join(home, "gems"),
        "GEM_HOME" => File.join(home, "gems"),
        "GEM_PATH" => File.join(home, "gems")
      }
      outcome = capture(environment, [wrapper, "absent"], unsetenv_others: true)

      assert_equal 77, outcome.status, outcome.output
      assert_match(%r{^SKIP: .*bundle install --gemfile=test/spec/Gemfile}, outcome.output)
    end
  end

  def test_run_writes_the_report_of_the_selected_profile
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "test", "spec-results", "nightly")
      FileUtils.mkdir_p(File.join(results, "cases", "stale"))
      outcome = run_profile(build_directory:, profile: "nightly", suites: [PASSING_SUITE], seed: 1234)

      assert_equal 0, outcome.status, outcome.output
      assert_equal %w[coverage.json summary.txt], Dir.children(results).sort, "earlier results are removed"
      refute File.exist?(File.join(build_directory, "test", "spec-results", "quick"))

      coverage = JSON.parse(File.read(File.join(results, "coverage.json")))
      assert_equal({ "schema_version" => 1, "profile" => "nightly", "seed" => 1234, "passed" => true },
                   coverage.slice("schema_version", "profile", "seed", "passed"))
      assert_equal({ "runs" => 1, "failures" => 0, "errors" => 0, "skips" => 0 },
                   coverage.fetch("tests").except("assertions"))

      summary = File.read(File.join(results, "summary.txt"))
      assert_match(/profile nightly/, summary)
      assert_match(/^result: PASS$/, summary)
      assert_includes outcome.output, summary, "the run prints its summary"
    end
  end

  def test_run_fails_when_a_test_fails
    assert_run_fails(suites: [PASSING_SUITE, FAILING_SUITE])
  end

  def test_run_fails_when_interrupted
    assert_run_fails(suites: [PASSING_SUITE, INTERRUPTED_SUITE])
  end

  def test_run_fails_when_no_test_runs
    assert_run_fails(suites: [EMPTY_SUITE])
  end

  def test_run_refuses_an_unknown_profile
    Dir.mktmpdir do |build_directory|
      outcome = run_profile(build_directory:, profile: "weekly", suites: [PASSING_SUITE])

      assert_equal 2, outcome.status, outcome.output
      assert_match(/unknown profile "weekly"/, outcome.output)
      refute File.exist?(File.join(build_directory, "test", "spec-results"))
    end
  end

  def test_run_refuses_a_missing_test_file
    Dir.mktmpdir do |build_directory|
      missing = File.join(build_directory, "test_missing.rb")
      outcome = capture({ "FSIMG_BUILDDIR" => build_directory }, [RbConfig.ruby, RUN_RB, "--profile=quick", missing])

      assert_equal 2, outcome.status, outcome.output
      assert_match(/no such test file/, outcome.output)
    end
  end

  private def assert_run_fails(suites:)
    Dir.mktmpdir do |build_directory|
      outcome = run_profile(build_directory:, profile: "quick", suites:)

      assert_equal 1, outcome.status, outcome.output
      results = File.join(build_directory, "test", "spec-results", "quick")
      assert_equal false, JSON.parse(File.read(File.join(results, "coverage.json"))).fetch("passed")
      assert_match(/^result: FAIL$/, File.read(File.join(results, "summary.txt")))
    end
  end

  # Writes each suite source into the build directory and runs the profile
  # on those files alone, with this process's interpreter and gem bundle.
  private def run_profile(build_directory:, profile:, suites:, seed: nil)
    files = suites.each_with_index.map do |source, index|
      path = File.join(build_directory, "test_stub_#{index}.rb")
      File.write(path, source)
      path
    end
    # A nil value unsets the variable, so this run's SEED never leaks in.
    environment = { "FSIMG_BUILDDIR" => build_directory, "SEED" => seed&.to_s }
    capture(environment, [RbConfig.ruby, RUN_RB, "--profile=#{profile}", *files])
  end

  private def capture(environment, command, **options)
    output = IO.popen(environment, command, err: %i[child out], **options, &:read)
    Outcome.new(status: Process.last_status.exitstatus, output:)
  end
end
