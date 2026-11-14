# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/apfs"

# The checker adapters: a host without a checker reports it unavailable,
# and where a checker runs it agrees with a native capture
# ($FSIMG_APFS_NATIVE).
class TestApfsOracles < Minitest::Test
  Apfs = FsimgSpec::Apfs
  MISSING = { "PATH" => "", "FSCK_APFS" => "/nonexistent/fsck_apfs", "HDIUTIL" => "/nonexistent/hdiutil" }.freeze

  def test_a_host_without_the_checkers_reports_each_unavailable
    observations = Apfs::Oracles.observe("image.img", environment: MISSING)
    assert_equal Apfs::Oracles::TOOLS, observations.keys
    assert(observations.values.all? { _1.state == "unavailable" })
  end

  def test_each_available_checker_agrees_with_a_native_capture
    observations = Apfs::Oracles.observe(native)
    skip "no APFS checker on this host" if observations.values.all? { _1.state == "unavailable" }
    assert(observations.values.all? { %w[agrees unavailable].include?(_1.state) }, observations.inspect)
  end

  private

  def native
    root = ENV.fetch("FSIMG_APFS_NATIVE") { skip "FSIMG_APFS_NATIVE names no native capture directory" }
    path = File.join(root, Apfs::Witnesses::BASE, "image.img")
    skip "no native capture #{path}" unless File.file?(path)
    path
  end
end
