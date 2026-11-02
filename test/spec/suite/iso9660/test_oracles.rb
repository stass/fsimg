# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fsimg_spec/iso9660"

class Iso9660OraclesTest < Minitest::Test
  Iso = FsimgSpec::Iso9660
  Execution = Data.define(:launch_error, :ended_by, :leftover, :signal, :exit_status, :stdout, :stderr)
  Shell = Data.define(:tools, :execute) do
    def unavailable_reason = nil
    def run(arguments, **_options) = execute.call(arguments)
  end
  Fields = Data.define(:space_le)
  Volume = Data.define(:fields)
  Model = Data.define(:image_length, :volume)

  def test_tiny_image_short_read_is_an_exact_consumer_exception
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: Short read on old image\n")
    assert_equal "not_applicable", result.state
    result = observe(blocks: 48, status: 255, stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: other read failure\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 1, stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
  end

  def test_geometry_checks_both_declared_values
    result = observe(blocks: 96, stdout: "Logical block size is: 2048\nVolume size is: 96\n")
    assert_equal "agrees", result.state
    result = observe(blocks: 96, stdout: "Logical block size is: 1024\nVolume size is: 96\n")
    assert_equal "disagrees", result.state
  end

  def test_explicit_charset_removes_the_measured_freebsd_locale_notice
    short_read = "isoinfo: Short read on old image\n".b
    notice = "Setting input-charset to 'UTF-8' from locale.\n".b
    execution = Execution.new(launch_error: nil, ended_by: nil, leftover: false, signal: nil,
      exit_status: 255, stdout: "".b, stderr: short_read)
    invoke = lambda do |arguments|
      canonical = ["/configured/isoinfo", "-j", "utf-8", "-d", "-i", "image.img"]
      arguments == canonical ? execution : execution.with(stderr: notice + short_read)
    end
    shell = Shell.new(tools: {isoinfo: "/configured/isoinfo"}, execute: invoke)
    model = Model.new(image_length: 27 * 2048, volume: Volume.new(fields: Fields.new(space_le: 27)))
    result = Iso::Oracles.observe(shell, image: "image.img", chdir: "/fixture", model:).fetch("isoinfo")
    assert_equal "not_applicable", result.state
  end

  def test_an_unexpected_charset_notice_is_not_a_short_read_waiver
    diagnostic = "Setting input-charset to 'UTF-8' from locale.\nisoinfo: Short read on old image\n"
    result = observe(blocks: 27, status: 255, stderr: diagnostic)
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: Short read on old image\nother failure\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stdout: "partial report\n", stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
  end

  private

  def observe(blocks:, status: 0, stdout: "", stderr: "")
    execution = Execution.new(launch_error: nil, ended_by: nil, leftover: false, signal: nil,
      exit_status: status, stdout: stdout.b, stderr: stderr.b)
    shell = Shell.new(tools: {isoinfo: "/configured/isoinfo"}, execute: ->(_arguments) { execution })
    model = Model.new(image_length: blocks * 2048, volume: Volume.new(fields: Fields.new(space_le: blocks)))
    Iso::Oracles.observe(shell, image: "image.img", chdir: "/fixture", model:).fetch("isoinfo")
  end
end
