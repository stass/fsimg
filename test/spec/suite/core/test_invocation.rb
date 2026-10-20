# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"

# Each stub stands in for fsimg and demonstrates one failure; the runner's
# process lifetime and outcome classes must name it, and no process of the
# stub's group may survive (specs/design-spec-testing.md#outcome-classes).
class TestInvocation < Minitest::Test
  # Fast stubs get a generous wall clock, so a loaded host never stops
  # them; the stubs that must hit the deadline get a short one.
  LIMITS = FsimgSpec::Invocation::Limits.new(wall_seconds: 30, cpu_seconds: 10, file_bytes: 1 << 20,
                                             address_space_bytes: nil, capture_bytes: 4096)
  DEADLINE_LIMITS = LIMITS.with(wall_seconds: 2)
  SUMMARY = '{"schema":1,"type":"summary","complete":true,"sandbox":"unavailable"}'
  EIO = '{"schema":1,"type":"error","code":-3,"status":"I/O error","resource":"none","sandbox":"unavailable"}'
  LIMIT = '{"schema":1,"type":"error","code":-8,"resource":"entries","sandbox":"unavailable"}'

  STUBS = {
    "floods stdout" => ["exec yes", "output_exceeded"],
    "floods stdout and exits" => ["head -c 3000 /dev/zero; head -c 2000 /dev/zero", "output_exceeded"],
    "dies by SIGSEGV" => ["kill -SEGV $$", "crashed"],
    "prints a sanitizer report" => ["echo '==1==ERROR: AddressSanitizer: heap-use-after-free' >&2; echo '#{SUMMARY}'", "sanitizer_failure"],
    "exits with the sanitizer status" => ["exit 87", "sanitizer_failure"],
    "exits 1 with an FSIMG_EIO error record" => ["echo '#{EIO}'; exit 1", "faulted:FSIMG_EIO"],
    "exits 1 with an FSIMG_ELIMIT error record" => ["echo '#{LIMIT}'; exit 1", "rejected:FSIMG_ELIMIT:entries"],
    "exits 1 with no error record" => ["echo '#{SUMMARY}'; exit 1", "harness_error"],
    "exits 2 at a reading stage" => ["echo '#{EIO}'; exit 2", "harness_error"],
    "emits malformed NDJSON" => ["echo '{\"type\":'; exit 0", "harness_error"],
    "exits 0 without a summary" => ["exit 0", "harness_error"],
    "reports another sandbox status" => ["echo '#{SUMMARY.sub("unavailable", "active")}'", "harness_error"],
    "completes with sandbox unknown" => ["echo '#{SUMMARY.sub("unavailable", "unknown")}'", "harness_error"],
    "faults with sandbox unknown" => ["echo '#{EIO.sub("unavailable", "unknown")}'; exit 1", "faulted:FSIMG_EIO"],
    "rejects with sandbox unknown" => ["echo '#{LIMIT.sub("unavailable", "unknown")}'; exit 1", "harness_error"],
    "faults with another sandbox" => ["echo '#{EIO.sub("unavailable", "active")}'; exit 1", "harness_error"],
    "completes" => ["echo '#{SUMMARY}'", "accepted"]
  }.freeze

  def test_each_stub_is_classified
    STUBS.each do |behaviour, (script, expected)|
      execution, survivors = run_stub(script)

      assert_equal expected, FsimgSpec::Outcome.classify(execution, stage: "info", sandbox: "unavailable").to_s, behaviour
      refute survivors, "#{behaviour}: a process of the group survived"
    end
  end

  # A stub, or a child it waits on, that never exits is stopped at the
  # runner's wall clock.
  def test_deadline_stops_a_stub_that_never_finishes
    { "never exits" => "exec sleep 30", "waits on a child that never exits" => "sleep 30; echo '#{SUMMARY}'" }.each do |behaviour, script|
      execution, survivors = run_stub(script, limits: DEADLINE_LIMITS)

      assert_equal "timed_out", FsimgSpec::Outcome.classify(execution, stage: "info", sandbox: "unavailable").kind, behaviour
      refute survivors, "#{behaviour}: a process of the group survived"
    end
  end

  # create refuses a setting before any worker runs, so its error record
  # reports the sandbox as unknown; that is still a rejection. A create
  # that completes ran a worker, which reported its confinement.
  def test_create_refused_before_a_worker_ran_is_a_rejection
    einval = '{"schema":1,"type":"error","code":-1,"resource":"none","sandbox":"unknown"}'
    refused, = run_stub("echo '#{einval}'; exit 2")
    completed, = run_stub("echo '#{SUMMARY.sub("unavailable", "unknown")}'")

    assert_equal "rejected:FSIMG_EINVAL", FsimgSpec::Outcome.classify(refused, stage: "create", sandbox: "unavailable").to_s
    assert_equal "harness_error", FsimgSpec::Outcome.classify(completed, stage: "create", sandbox: "unavailable").to_s
  end

  # A child that outlives the stub is found by the probe after the reap,
  # recorded and killed.
  def test_child_left_alive_is_recorded_and_killed
    execution, survivors = run_stub("sleep 30 & echo '#{SUMMARY}'")

    assert execution.leftover
    refute survivors
  end

  def test_launch_failure_is_a_harness_error
    Dir.mktmpdir do |directory|
      execution = FsimgSpec::Invocation.run([File.join(directory, "missing")], chdir: directory, env: {},
                                                                               limits: LIMITS)

      assert_equal "harness_error", FsimgSpec::Outcome.classify(execution, stage: "info", sandbox: nil).kind
    end
  end

  # fsimg's own deadline is a timeout, not a rejection.
  def test_cancelled_error_record_is_a_timeout
    cancelled = '{"schema":1,"type":"error","code":-10,"resource":"none","sandbox":"unavailable"}'
    execution, = run_stub("echo '#{cancelled}'; exit 1")

    assert_equal "timed_out:FSIMG_ECANCELLED", FsimgSpec::Outcome.classify(execution, stage: "ls", sandbox: nil).to_s
  end

  # Exit 2 is a rejection only at create, for refused input.
  def test_usage_error_counts_only_as_a_refused_creation
    einval = '{"schema":1,"type":"error","code":-1,"resource":"none","sandbox":"unavailable"}'
    execution, = run_stub("echo '#{einval}'; exit 2")

    assert_equal "rejected:FSIMG_EINVAL", FsimgSpec::Outcome.classify(execution, stage: "create", sandbox: nil).to_s
    assert_equal "harness_error", FsimgSpec::Outcome.classify(execution, stage: "verify", sandbox: nil).to_s
  end

  # An exception that ends supervision early, such as an interrupt, kills
  # and reaps the stage's group instead of leaving it running.
  def test_interrupted_run_leaves_no_group
    Dir.mktmpdir do |directory|
      stub = File.join(directory, "stub")
      File.write(stub, "#!/bin/sh\nsleep 30\n")
      File.chmod(0o700, stub)
      observer = Recording.new
      thread = Thread.new do
        FsimgSpec::Invocation.run([stub], chdir: directory, env: { "PATH" => "/bin:/usr/bin" }, limits: LIMITS, observer:)
      end
      thread.report_on_exception = false
      Thread.pass until observer.seen.any? || !thread.alive?
      assert observer.seen.any?, "the stub never started"
      thread.raise(Interrupt)

      assert_raises(Interrupt) { thread.join }
      assert(wait_until_gone(observer.seen.first), "the interrupted group survived")
    end
  end

  # A harness process that supervises the stub and nothing else.
  HARNESS = <<~RUBY
    limits = FsimgSpec::Invocation::Limits.new(wall_seconds: 600, cpu_seconds: 10, file_bytes: 1 << 20,
                                               address_space_bytes: nil, capture_bytes: 4096)
    FsimgSpec::Invocation.run([ARGV[0]], chdir: ARGV[1], env: { "PATH" => "/bin:/usr/bin" }, limits:)
  RUBY

  # SIGTERM to the harness unwinds the stage it supervises, with no trap:
  # the harness ends by the signal within seconds, long before the stub
  # would exit, and leaves no process of the group.
  def test_sigterm_to_the_harness_leaves_no_group
    Dir.mktmpdir do |directory|
      stub = File.join(directory, "stub")
      File.write(stub, "#!/bin/sh\necho $$ > pid.tmp\nmv pid.tmp pid\nexec sleep 300\n")
      File.chmod(0o700, stub)
      harness = Process.spawn(RbConfig.ruby, "-I", File.expand_path("../../lib", __dir__), "-rfsimg_spec",
                              "-e", HARNESS, stub, directory)
      group = nil
      begin
        group = wait_for_pid(File.join(directory, "pid"))
        Process.kill(:TERM, harness)
        status = wait_for_exit(harness, seconds: 10)
        assert status, "the harness still ran ten seconds after SIGTERM"
        harness = nil

        assert_equal Signal.list.fetch("TERM"), status.termsig, status.inspect
        assert(wait_until_gone(group), "the stage's group outlived the harness")
      ensure
        clean_up(harness, group)
      end
    end
  end

  private

  # The process ID the stub writes once it runs, waiting up to ten
  # seconds for the harness to start it.
  def wait_for_pid(path)
    deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 10
    sleep 0.01 until File.exist?(path) || Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
    Integer(File.read(path), 10)
  end

  # The status of the child pid once it exits, or nil if it still runs
  # after seconds.
  def wait_for_exit(pid, seconds:)
    deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + seconds
    loop do
      _, status = Process.wait2(pid, Process::WNOHANG)
      return status if status
      return nil if Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline

      sleep 0.01
    end
  end

  # Kills and reaps a harness the test did not see to its end, and kills
  # a stub group a failed check left running.
  def clean_up(harness, group)
    if harness
      quietly { Process.kill(:KILL, harness) }
      quietly { Process.wait(harness) }
    end
    quietly { Process.kill(:KILL, -group) } if group && FsimgSpec::Invocation.group_alive?(group)
  end

  # Runs the block, ignoring a process that is already gone.
  def quietly
    yield
  rescue Errno::ESRCH, Errno::ECHILD, Errno::EPERM
    nil
  end

  # Whether group is gone within two seconds; killed members disappear
  # once the system reaps them.
  def wait_until_gone(group)
    deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 2
    sleep 0.01 while FsimgSpec::Invocation.group_alive?(group) && Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
    !FsimgSpec::Invocation.group_alive?(group)
  end

  # Runs script as the stub and answers its execution and whether any
  # process of its group is alive afterwards.
  def run_stub(script, limits: LIMITS)
    Dir.mktmpdir do |directory|
      stub = File.join(directory, "stub")
      File.write(stub, "#!/bin/sh\n#{script}\n")
      File.chmod(0o700, stub)
      observer = Recording.new
      execution = FsimgSpec::Invocation.run([stub], chdir: directory, env: { "PATH" => "/bin:/usr/bin" },
                                                    limits:, observer:)
      [execution, observer.seen.any? { FsimgSpec::Invocation.group_alive?(_1) }]
    end
  end

  # An observer that remembers every group it saw start.
  class Recording < FsimgSpec::GroupObserver
    attr_reader :seen

    def initialize
      super
      @seen = []
    end

    def started(pgid)
      @seen << pgid
    end
  end
end
