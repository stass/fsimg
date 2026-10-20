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

  # value.inspect cut to a length a message can carry. Messages about
  # untrusted recipes use it, so a hostile value cannot flood a report.
  # A String is cut before it is inspected, so quoting a long one costs
  # no more than quoting a short one.
  def self.shown(value, limit: 80)
    text = value.is_a?(String) ? value.byteslice(0, limit + 1).inspect : value.inspect
    text.length > limit ? "#{text[0, limit]}..." : text
  end

  # An exception as a report or detail quotes it: its class's own name and
  # its message, shown, so that a path or recipe value in the message can
  # neither flood the report nor carry bytes JSON cannot encode.
  def self.error_label(error) = "#{error_class(error)}: #{shown(error.message, limit: 400)}"

  # The last part of the name of error's class, or of its nearest named
  # superclass when the class is anonymous.
  def self.error_class(error)
    named = error.class
    named = named.superclass while named.name.nil?
    named.name.split("::").last
  end

  # Data#with does not call initialize on Ruby 3.2, the floor; 3.3 and
  # later do. A Data class whose initialize checks or normalizes its
  # fields includes this, so with runs those checks on the floor too.
  module Checked
    def with(**changes) = self.class.new(**self.class.members.to_h { [_1, public_send(_1)] }, **changes)
  end
end

require_relative "fsimg_spec/terminal"
require_relative "fsimg_spec/undefined"
require_relative "fsimg_spec/contract"
require_relative "fsimg_spec/profile"
require_relative "fsimg_spec/rule"
require_relative "fsimg_spec/expectation"
require_relative "fsimg_spec/mutation"
require_relative "fsimg_spec/image"
require_relative "fsimg_spec/canonical_json"
require_relative "fsimg_spec/recipe"
require_relative "fsimg_spec/recipe_loader"
require_relative "fsimg_spec/case"
require_relative "fsimg_spec/process"
require_relative "fsimg_spec/outcome"
require_relative "fsimg_spec/tree"
require_relative "fsimg_spec/name_trials"
require_relative "fsimg_spec/runner"
require_relative "fsimg_spec/case_run"
require_relative "fsimg_spec/fixtures"
require_relative "fsimg_spec/preflight"
require_relative "fsimg_spec/coverage"
require_relative "fsimg_spec/failure_record"
require_relative "fsimg_spec/case_phase"
require_relative "fsimg_spec/replay"
require_relative "fsimg_spec/report"
require_relative "fsimg_spec/results_directory"
