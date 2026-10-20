# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # A named run configuration (specs/design-spec-testing.md#profiles). Each
  # profile writes its report into its own results directory under the
  # build directory, so runs of different profiles never share one.
  Profile = Data.define(:name) do
    def results_directory(build_directory) =
      File.join(build_directory, "test", "spec-results", name)
  end

  PROFILES = %w[quick nightly deep].to_h { [_1, Profile.new(name: _1)] }.freeze

  def self.fetch_profile(name) =
    PROFILES.fetch(name) do
      raise UsageError, "unknown profile #{name.inspect}; expected one of #{PROFILES.keys.join(", ")}"
    end
end
