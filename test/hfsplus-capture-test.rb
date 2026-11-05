# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "tmpdir"
require_relative "../tools/hfsplus/capture"

class HfsPlusCaptureTimeoutTest < Minitest::Test
  # Only hdiutil is simulated. The checked command really times out, with a
  # grandchild retaining stdout after the leader exits.
  class DeviceLog < HfsPlusCapture::Log
    attr_reader :calls

    def initialize(attach_timeout: false)
      @attach_timeout = attach_timeout
      super()
      @calls = []
    end

    def run(*argv, timeout: HfsPlusCapture::COMMAND_TIMEOUT)
      return super unless argv.first == "hdiutil"

      @calls << argv.drop(1)
      if @attach_timeout && argv[1] == "attach"
        return super(RbConfig.ruby, "-e", '$stdout.sync = true; puts "/dev/disk123"; sleep 60', timeout: 5)
      end
      ["/dev/disk123\n", true]
    end
  end

  def test_timeout_preserves_output_and_detaches
    log = DeviceLog.new
    attachments = HfsPlusCapture::Attachments.new(log)
    out, ok = attachments.with("fixture.img") do |_device|
      log.run(RbConfig.ruby, "-e",
              '$stdout.sync = true; puts "before timeout"; fork { sleep 60 }; exit!',
              timeout: 5)
    end

    refute ok
    assert_includes out, "before timeout"
    assert_includes out, "timeout after 5 seconds"
    assert_equal "attach", log.calls.first.first
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end

  def test_partial_attach_timeout_detaches
    log = DeviceLog.new(attach_timeout: true)
    attachments = HfsPlusCapture::Attachments.new(log)
    yielded = false

    assert_raises(HfsPlusCapture::Error) do
      attachments.with("fixture.img") { yielded = true }
    end
    refute yielded
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end

  def test_failure_after_timeout_detaches
    log = DeviceLog.new
    attachments = HfsPlusCapture::Attachments.new(log)
    assert_raises(HfsPlusCapture::Error) do
      attachments.with("fixture.img") do |_device|
        _, ok = log.run(RbConfig.ruby, "-e", 'sleep 60', timeout: 5)
        raise HfsPlusCapture::Error, "check failed" unless ok
      end
    end
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end
  def test_interrupt_in_attachment_block_detaches_and_propagates
    log = DeviceLog.new
    attachments = HfsPlusCapture::Attachments.new(log)
    assert_raises(Interrupt) do
      attachments.with("fixture.img") { raise Interrupt, "stop" }
    end
    assert_equal(["detach", "/dev/disk123"], log.calls.last)
  ensure
    attachments&.release
  end

  def test_run_status_redirects_stderr_without_mixing_it_into_stdout
    Dir.mktmpdir do |root|
      errors = File.join(root, "stderr")
      output, status = HfsPlusCapture::Log.new.run_status(RbConfig.ruby, "-e",
        '$stdout.write "stdout"; $stderr.write "stderr"', stderr: errors)
      assert_equal(0, status)
      assert_equal("stdout", output)
      assert_equal("stderr", File.read(errors))
    end
  end

  def test_run_status_combines_stdout_and_stderr_by_default
    output, status = HfsPlusCapture::Log.new.run_status(RbConfig.ruby, "-e",
      '$stdout.sync = true; $stdout.write "stdout"; $stderr.write "stderr"')
    assert_equal(0, status)
    assert_equal("stdoutstderr", output)
  end

  def test_run_status_timeout_kills_leader_and_child_in_its_process_group
    started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    output, status = HfsPlusCapture::Log.new.run_status(RbConfig.ruby, "-e",
      '$stdout.sync = true; child = fork { sleep 60 }; puts child; puts Process.pid; sleep 60', timeout: 5)
    assert_nil(status)
    assert_operator(Process.clock_gettime(Process::CLOCK_MONOTONIC) - started, :<, 30)
    pids = output.lines.first(2).map { Integer(_1) }
    assert_equal(2, pids.size)
    pids.each { assert_process_gone(_1) }
  ensure
    pids&.each { kill_if_present(_1) }
  end

  def test_run_status_interrupt_kills_process_group_and_propagates
    Dir.mktmpdir do |root|
      ready = File.join(root, "ready")
      log = HfsPlusCapture::Log.new
      worker = Thread.new do
        Thread.current.report_on_exception = false
        log.run_status(RbConfig.ruby, "-e",
          '$stdout.sync = true; child = fork { sleep 60 }; puts child; File.write(ARGV.fetch(0), [child, Process.pid].join(" ")); sleep 60',
          ready, timeout: 20)
      end
      begin
        Timeout.timeout(5) { sleep(0.01) until File.exist?(ready) && !File.read(ready).empty? }
        pids = File.read(ready).split.map { Integer(_1) }
        assert_equal(2, pids.size)
        worker.raise(Interrupt, "stop")
        assert_raises(Interrupt) { Timeout.timeout(5) { worker.value } }
        pids.each { assert_process_gone(_1) }
      ensure
        pids&.each { kill_if_present(_1) }
        if worker.alive?
          worker.raise(Interrupt)
          assert_raises(Interrupt) { Timeout.timeout(5) { worker.value } }
        end
      end
    end
  end

  private

  def assert_process_gone(pid)
    Timeout.timeout(5) do
      loop do
        begin
          Process.kill(0, pid)
        rescue Errno::ESRCH
          break
        end
        sleep(0.01)
      end
    end
    assert_raises(Errno::ESRCH) { Process.kill(0, pid) }
  end

  def kill_if_present(pid)
    Process.kill("KILL", pid)
  rescue Errno::ESRCH
    nil
  end
end
