# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

# The specification-driven test harness. specs/design-spec-testing.md
# describes its design; run.rb is its entry point.
module FsimgSpec
  # The root of the harness's own errors.
  class Error < StandardError; end

  # A run was started with an argument or environment it cannot use.
  class UsageError < Error; end
end

require_relative "fsimg_spec/profile"
require_relative "fsimg_spec/report"
require_relative "fsimg_spec/results_directory"
