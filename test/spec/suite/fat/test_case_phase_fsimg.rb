# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"
require "json"
require "minitest"
require "rbconfig"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The case phase through the real fsimg
# (specs/design-spec-testing.md#reports-and-regressions): an injected
# fault, here a rule whose status is mis-set, fails its case,
# writes a failure record that names no host path, and replaying the
# record's recipe under the same rules reproduces the failure. A fault in
# one case is that case's harness error, and the phase goes on.
class TestCasePhaseFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  FAULTY = "fat.bpb.bytes_per_sector.0201"
  REPLAY = File.expand_path("../../bin/replay", __dir__)

  # The FAT suite with fat.bpb.bytes_per_sector expecting FSIMG_ECORRUPT,
  # cut to one failing witness and one valid base.
  module FaultySuite
    def self.registry = FsimgSpec::Registry.new(Fat::Rules.registry.rules.map do |rule|
      rule.id == "fat.bpb.bytes_per_sector" ? rule.with(status: FsimgSpec::Status.new(name: "FSIMG_ECORRUPT")) : rule
    end)

    def self.cases(cache:, ceilings:) =
      Fat::Suite.cases(cache:, ceilings:, registry:).select { [FAULTY, VALID].include?(_1.id) }

    def self.replay(recipe, cache:, ceilings:) = Fat::Suite.replay(recipe, cache:, ceilings:, registry:)

    def self.preflight_sample(directory) = Fat::Suite.preflight_sample(directory)
  end

  VALID = "fat.valid.fat12"

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  def test_injected_fault_writes_a_failure_record_that_replays
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      Dir.mkdir(results)
      phase = FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory:, environment: ENV.to_h,
                                        suites: { "fat" => FaultySuite })
      fat = phase.formats.fetch("fat")
      record = File.join(results, "failures", FAULTY)

      assert_equal [FAULTY], fat.failures
      assert_equal({ total: 2, passed: 1, failed: 1 }, fat.cases.slice(:total, :passed, :failed))
      assert_equal %w[case.json fixture.json image.img info.stderr info.stdout replay.sh result.json], Dir.children(record).sort
      result = JSON.parse(File.read(File.join(record, "result.json")))
      assert_equal "info: expected reject:FSIMG_ECORRUPT, got rejected:FSIMG_EFORMAT", result.fetch("detail")
      refute(Dir.children(record).any? { File.binread(File.join(record, _1)).include?(build_directory) },
             "the record names a host path")

      assert_match(/ --replay="\$\(dirname "\$0"\)\/case.json" quick$/, File.read(File.join(record, "replay.sh")))
      assert_equal "quick", result.fetch("profile")

      replayed, drift = FsimgSpec::Replay.call(File.join(record, "case.json"), profile: PROFILE, build_directory:,
                                                                                environment: ENV.to_h, suite: FaultySuite)
      assert_equal ["failed", "info: expected reject:FSIMG_ECORRUPT, got rejected:FSIMG_EFORMAT"],
                   [replayed.verdict, replayed.detail]
      assert_empty drift
    end
  end

  # A case whose classification raises is that case's harness error, with
  # a record of its result alone; the other cases still run, and the rule
  # it counts toward has only its predicate tested by it.
  def test_a_fault_in_one_case_fails_only_that_case
    broken = pending("t.broken") { raise FsimgSpec::RuleError, "unknown rule fat.bpb.renamed" }
    skipped = FsimgSpec::PendingCase.new(id: "t.skipped", rules: ["fat.bpb.media"], skip: "no tools", classify: nil)
    with_phase(suite_of(broken, skipped)) do |phase, results|
      fat = phase.formats.fetch("fat")

      assert_equal({ total: 3, passed: 1, host_skip: 1, harness_error: 1 },
                   fat.cases.slice(:total, :passed, :host_skip, :harness_error))
      assert_equal ["t.broken"], fat.failures
      assert_equal ["result.json"], Dir.children(File.join(results, "failures", "t.broken"))
      result = JSON.parse(File.read(File.join(results, "failures", "t.broken", "result.json")))
      assert_equal ["harness_error", 'RecipeError: "t.broken cannot be classified: RuleError: unknown rule fat.bpb.renamed"'],
                   result.values_at("verdict", "detail")
    end
  end

  # Two cases with one id would share a case directory and a failure
  # record, so the phase refuses them before running either.
  def test_repeated_case_ids_end_the_phase_before_any_case_runs
    repeated = pending(VALID) { flunk "the phase ran" }

    error = assert_raises(FsimgSpec::HarnessError) { with_phase(suite_of(repeated)) { flunk "the phase ran" } }
    assert_equal "case ids repeat: #{VALID}", error.message
  end

  # A failure record that cannot be written makes its case a harness error;
  # the phase goes on.
  def test_a_record_that_cannot_be_written_fails_only_its_case
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      Dir.mkdir(results)
      blocked = pending("t.blocked") do
        Dir.mkdir(File.join(results, "failures", "t.blocked"))
        raise FsimgSpec::RuleError, "unknown rule fat.bpb.renamed"
      end
      phase = FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory:, environment: ENV.to_h,
                                        suites: { "fat" => suite_of(blocked) })

      assert_equal({ total: 2, passed: 1, harness_error: 1 }, phase.formats.fetch("fat").cases.slice(:total, :passed, :harness_error))
      assert_empty Dir.children(File.join(results, "failures", "t.blocked"))
    end
  end

  # A process that outlives SIGKILL is no single case's fault: it passes
  # the per-case funnel and ends the phase, and the run's boundary reports
  # it.
  def test_a_containment_failure_ends_the_phase
    escaped = pending("t.escaped") { raise FsimgSpec::ContainmentError, "process group 1 survived SIGKILL" }
    ending = guarded(suite_of(escaped))

    assert_equal FsimgSpec::CasePhase::Ending.new(
      cases: FsimgSpec::CasesFailed.new(reason: 'ContainmentError: "process group 1 survived SIGKILL"'), fatal: nil
    ), ending
  end

  # A signal ends the phase after its live groups are killed; the boundary
  # answers it, so that the run writes its report before dying by it.
  def test_a_signal_ends_the_phase_and_is_answered
    interrupted = pending("t.interrupted") { raise Interrupt, "injected" }
    ending = guarded(suite_of(interrupted))

    assert_equal FsimgSpec::CasesFailed.new(reason: "interrupted by SIGINT"), ending.cases
    assert_kind_of Interrupt, ending.fatal
  end

  # A fault of Ruby itself is answered with a report and still ends the
  # run.
  def test_a_fault_of_ruby_itself_is_reported_and_fatal
    overflowed = pending("t.overflowed") { raise SystemStackError, "stack level too deep" }
    ending = guarded(suite_of(overflowed))

    assert_equal FsimgSpec::CasesFailed.new(reason: 'SystemStackError: "stack level too deep"'), ending.cases
    assert_kind_of SystemStackError, ending.fatal
  end

  # A fault outside every case, here results that already hold cases/,
  # fails the phase as a whole.
  def test_a_fault_outside_every_case_fails_the_phase
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      FileUtils.mkdir_p(File.join(results, "cases"))
      ending = FsimgSpec::CasePhase.guarded(profile: PROFILE, results:, build_directory:, environment: ENV.to_h)

      assert_match(/\AEEXIST: /, ending.cases.reason)
      assert_nil ending.fatal
    end
  end

  # bin/replay, which test/spec.sh --replay runs, answers git bisect run: 0
  # for a passing case, 125 for one this revision cannot judge (a promoted
  # case that drifted, one the host skips, a recipe that does not load) and
  # 128, which stops the bisection, for a usage error such as a missing
  # recipe. It runs with this interpreter and bundle, as run.rb's tests do,
  # in a build directory of its own.
  def test_replay_exit_status_tells_the_outcomes_apart
    recipe = Fat::Suite.promoted(Fat::Suite::PROMOTED, ceilings: PROFILE.ceilings).first
    drifted = recipe.with(expected: recipe.expected.transform_values { FsimgSpec::Expectation.parse("accept") })
    formatted = recipe.with(base: recipe.base.with(builder: "mkfs.fat"))
    Dir.mktmpdir do |directory|
      paths = { "drifted" => drifted.dump, "formatted" => formatted.dump, "malformed" => "{" }.to_h do |name, text|
        path = File.join(directory, "#{name}.json")
        File.write(path, text)
        [name, path]
      end
      recipes = [File.join(Fat::Suite::PROMOTED, "#{recipe.id}.json"), *paths.values_at("drifted", "formatted", "malformed"),
                 File.join(directory, "absent.json")]
      build_directory = File.join(directory, "build")
      Dir.mkdir(build_directory)

      assert_equal [0, 125, 125, 125, 128], recipes.map { replay_status(_1, build_directory:) }
    end
  end

  private

  def replay_status(path, build_directory:)
    IO.popen({ "FSIMG_BUILDDIR" => build_directory }, [RbConfig.ruby, REPLAY, path, "quick"], err: %i[child out], &:read)
    Process.last_status.exitstatus
  end

  def pending(id, &classify) = FsimgSpec::PendingCase.new(id:, rules: ["fat.bpb.media"], skip: nil, classify:)

  # A suite of the valid witness and the given pending cases.
  def suite_of(*extra)
    Module.new do
      define_singleton_method(:registry) { Fat::Suite.registry }
      define_singleton_method(:preflight_sample) { |directory| Fat::Suite.preflight_sample(directory) }
      define_singleton_method(:cases) do |cache:, ceilings:|
        [Fat::Suite.cases(cache:, ceilings:).find { _1.id == VALID }, *extra]
      end
    end
  end

  # The phase behind its boundary. A signal the test did not inject, such
  # as a Ctrl-C while the preflight runs fsimg, still stops the run.
  def guarded(suite)
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      Dir.mkdir(results)
      ending = FsimgSpec::CasePhase.guarded(profile: PROFILE, results:, build_directory:, environment: ENV.to_h,
                                            suites: { "fat" => suite })
      fatal = ending.fatal
      raise fatal if fatal.is_a?(SignalException) && fatal.message != "injected"

      ending
    end
  end

  def with_phase(suite)
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      Dir.mkdir(results)
      phase = FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory:, environment: ENV.to_h,
                                        suites: { "fat" => suite })
      yield phase, results
    end
  end
end
