# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fileutils"
require "minitest"
require "tmpdir"
require "fsimg_spec"

# Concurrent runs of one profile take turns on its results directory, and
# each starts from an empty one.
class TestResultsDirectory < Minitest::Test
  def test_hold_waits_for_the_run_holding_the_lock_and_then_empties_the_directory
    Dir.mktmpdir do |root|
      path = File.join(root, "quick")
      FileUtils.mkdir_p(File.join(path, "cases", "stale"))

      File.open(path) do |earlier_run|
        earlier_run.flock(File::LOCK_EX)
        later_run = Thread.new { FsimgSpec::ResultsDirectory.hold(path) { Dir.children(path) } }

        assert_nil later_run.join(0.5), "the later run enters while the earlier one holds the lock"
        earlier_run.flock(File::LOCK_UN)
        assert_empty later_run.value
      end
    end
  end

  def test_hold_removes_a_symbolic_link_without_following_it
    Dir.mktmpdir do |root|
      outside = File.join(root, "outside")
      FileUtils.mkdir_p(outside)
      File.write(File.join(outside, "kept"), "")
      path = File.join(root, "quick")
      FileUtils.mkdir_p(path)
      File.symlink(outside, File.join(path, "link"))

      FsimgSpec::ResultsDirectory.hold(path) { nil }

      assert_empty Dir.children(path)
      assert File.file?(File.join(outside, "kept")), "the link's target survives"
    end
  end
end
