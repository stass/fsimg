# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "tmpdir"
require_relative "../tools/apfs/capture"

class ApfsCaptureAttachmentsTest < Minitest::Test
  # Only hdiutil is simulated: attach lists the image's disk, then a container.
  class DeviceLog < ApfsCapture::Log
    attr_reader :calls

    def initialize
      super
      @calls = []
    end

    def run(*argv, timeout: ApfsCapture::COMMAND_TIMEOUT)
      return super unless File.basename(argv.first) == "hdiutil"

      @calls << argv.drop(1)
      return ["/dev/disk123\n/dev/disk124\tEF57347C-0000-11AA-AA11-0030654\n", true] if argv[1] == "attach"

      ["", true]
    end
  end

  def test_attach_names_the_image_disk_and_the_container_disk
    log = DeviceLog.new
    attachments = ApfsCapture::Attachments.new(log)
    attachments.with("fixture.img") do |attached|
      assert_equal "/dev/disk123", attached.device
      assert_equal "/dev/disk124", attached.container
    end
    assert_equal ["detach", "/dev/disk123"], log.calls.last
    assert_predicate attachments, :none?
  end

  def test_failure_in_the_block_detaches
    log = DeviceLog.new
    attachments = ApfsCapture::Attachments.new(log)
    assert_raises(ApfsCapture::Error) do
      attachments.with("fixture.img") { raise ApfsCapture::Error, "check failed" }
    end
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end

  def test_interrupt_in_the_block_detaches_and_propagates
    log = DeviceLog.new
    attachments = ApfsCapture::Attachments.new(log)
    assert_raises(Interrupt) { attachments.with("fixture.img") { raise Interrupt, "stop" } }
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end

  def test_run_status_timeout_kills_leader_and_child_in_its_process_group
    output, status = ApfsCapture::Log.new.run_status(RbConfig.ruby, "-e",
      '$stdout.sync = true; child = fork { sleep 60 }; puts child; puts Process.pid; sleep 60', timeout: 5)
    assert_nil status
    pids = output.lines.first(2).map { Integer(_1) }
    assert_equal 2, pids.size
    pids.each { assert_process_gone(_1) }
  ensure
    pids&.each { kill_if_present(_1) }
  end

  def test_run_status_combines_stdout_and_stderr
    output, status = ApfsCapture::Log.new.run_status(RbConfig.ruby, "-e",
      '$stdout.sync = true; $stdout.write "out"; $stderr.write "err"')
    assert_equal 0, status
    assert_equal "outerr", output
  end

  private

  def assert_process_gone(pid)
    Timeout.timeout(5) do
      loop do
        Process.kill(0, pid)
        sleep(0.01)
      rescue Errno::ESRCH
        break
      end
    end
  end

  def kill_if_present(pid)
    Process.kill("KILL", pid)
  rescue Errno::ESRCH
    nil
  end
end

class ApfsCaptureOptionsTest < Minitest::Test
  def test_volume_keys_become_newfs_flags_with_the_name_first
    volume = ApfsCapture.parse_volume("case-sensitive,block-size=4096,name=Data")
    assert_equal %w[-v Data -e -b 4096], volume.newfs_args(first: true)
  end

  def test_a_volume_after_the_first_is_added_with_dash_a
    volume = ApfsCapture.parse_volume("name=Data")
    assert_equal %w[-A -v Data], volume.newfs_args(first: false)
  end

  def test_volume_without_a_name_is_named_fsimg
    assert_equal %w[-v FSIMG], ApfsCapture::Volume.new(settings: {}).newfs_args(first: true)
  end

  def test_volume_keys_outside_the_declared_list_are_refused
    assert_raises(ApfsCapture::Error) { ApfsCapture.parse_volume("name=A,encrypt") }
  end

  def test_volume_values_cannot_carry_shell_or_flag_text
    assert_raises(ApfsCapture::Error) { ApfsCapture.parse_volume("name=-rf") }
    assert_raises(ApfsCapture::Error) { ApfsCapture.parse_volume("name=a;b") }
    assert_raises(ApfsCapture::Error) { ApfsCapture.parse_volume("case-sensitive=1") }
  end

  def test_both_case_keys_are_refused
    assert_raises(ApfsCapture::Error) { ApfsCapture.parse_volume("case-sensitive,case-insensitive") }
  end

  def test_hdiutil_takes_only_name_and_case
    volumes = [ApfsCapture.parse_volume("wait")]
    assert_raises(ApfsCapture::Error) { ApfsCapture.check_volumes("hdiutil", volumes, nil) }
  end

  def test_native_root_must_be_set_and_outside_a_work_tree
    assert_raises(ApfsCapture::Error) { ApfsCapture.native_root({}) }
    assert_raises(ApfsCapture::Error) { ApfsCapture.native_root("FSIMG_APFS_NATIVE" => "relative") }
    inside = File.join(__dir__, "no-such-capture-dir")
    assert_raises(ApfsCapture::Error) { ApfsCapture.native_root("FSIMG_APFS_NATIVE" => inside) }
    dotted = File.join(__dir__, "no-such-dir", "..", "no-such-capture-dir")
    assert_raises(ApfsCapture::Error) { ApfsCapture.native_root("FSIMG_APFS_NATIVE" => dotted) }
    Dir.mktmpdir { |dir| assert_equal dir, ApfsCapture.native_root("FSIMG_APFS_NATIVE" => dir) }
  end
end
