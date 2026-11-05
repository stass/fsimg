# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
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
        return super(RbConfig.ruby, "-e", '$stdout.sync = true; puts "/dev/disk123"; sleep 60', timeout: 1)
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
              timeout: 1)
    end

    refute ok
    assert_includes out, "before timeout"
    assert_includes out, "timeout after 1 seconds"
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
        _, ok = log.run(RbConfig.ruby, "-e", 'sleep 60', timeout: 1)
        raise HfsPlusCapture::Error, "check failed" unless ok
      end
    end
    assert_equal ["detach", "/dev/disk123"], log.calls.last
  ensure
    attachments&.release
  end
end
