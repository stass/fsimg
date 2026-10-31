# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"
require "fileutils"
require "json"
require "minitest/autorun"
require "pathname"
require "pty"
require "set"
require "socket"
require "stringio"
require "tmpdir"
require "zlib"

require_relative "process"
require_relative "fixtures"
require_relative "images"
require_relative "records"

class CliTest < Minitest::Test
  include CliProcesses
  include CliFixtures
  include CliImages
  include CliRecords

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE")).realpath
    @root = Pathname.new(Dir.mktmpdir("cli-fixture-", Dir.pwd))
    @payload = (0...131101).map { _1 % 251 }.pack("C*").freeze
    FileUtils.mkdir_p(@root / "source/nested")
    (@root / "source/HELLO.TXT").binwrite("hello\n")
    (@root / "source/nested/caf\u00e9.txt").binwrite(@payload)
  end

  def teardown
    assert_empty(@root.glob(".fsimg-*"), "command staging must be removed")
  ensure
    FileUtils.remove_entry(@root) if @root
  end

end

ENV["FSIMG_CLI_EXECUTABLE"] = File.expand_path(ARGV.shift || "./fsimg")
test_files = Dir.children(__dir__).select { _1.end_with?("_test.rb") }.sort
abort("CLI suite found no test files") if test_files.empty?
test_files.each { require File.join(__dir__, _1) }
test_count = Minitest::Runnable.runnables.sum { _1.public_instance_methods.grep(/^test_/).size }
abort("CLI suite found no test methods") if test_count.zero?
