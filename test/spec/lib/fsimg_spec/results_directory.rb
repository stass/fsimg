# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # A profile's results directory: test/spec-results/PROFILE/ under the build
  # directory, holding the report, case directories and failure records of
  # the latest run (specs/design-spec-testing.md#reports-and-regressions).
  module ResultsDirectory
    # Holds an exclusive lock on the directory while the block runs, so
    # concurrent runs of one profile take turns, and empties it first, so
    # nothing an earlier run wrote survives into this run's results. Removal
    # unlinks a symbolic link in the directory without following it.
    def self.hold(path)
      FileUtils.mkdir_p(path)
      File.open(path) do |directory|
        directory.flock(File::LOCK_EX)
        Dir.children(path).each { FileUtils.rm_r(File.join(path, _1)) }
        yield
      end
    end
  end
end
