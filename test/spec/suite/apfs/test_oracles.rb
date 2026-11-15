# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/apfs"

# The checker adapters: they read only an Unmutated image, a host without
# a checker reports it unavailable, and where a checker runs it agrees
# with the native captures the witnesses mutate ($FSIMG_APFS_NATIVE).
class TestApfsOracles < Minitest::Test
  Apfs = FsimgSpec::Apfs
  Unmutated = Apfs::Oracles::Unmutated
  MISSING = { "PATH" => "", "FSCK_APFS" => "/nonexistent/fsck_apfs", "HDIUTIL" => "/nonexistent/hdiutil" }.freeze

  def test_a_host_without_the_checkers_reports_each_unavailable
    observations = Apfs::Oracles.observe(Unmutated.created("/nonexistent"), environment: MISSING)
    assert_equal Apfs::Oracles::TOOLS, observations.keys
    assert(observations.values.all? { _1.state == "unavailable" })
  end

  # Neither a path nor an image view reaches a checker: an Unmutated image
  # has no constructor that takes one.
  def test_the_checkers_read_only_an_unmutated_image
    view = FsimgSpec::ImageView.new(base: File::NULL, length: 0, patches: [])
    ["image.img", view].each do |image|
      assert_raises(FsimgSpec::HarnessError, image.inspect) { Apfs::Oracles.observe(image, environment: MISSING) }
    end
    assert_raises(NoMethodError) { Unmutated.new("image.img") }
  end

  def test_each_available_checker_agrees_with_the_native_bases
    observations = Apfs::Witnesses::BASES.to_h { [_1, Apfs::Oracles.observe(native(_1))] }
    skip "no APFS checker on this host" if observations.values.flat_map(&:values).all? { _1.state == "unavailable" }
    assert(observations.values.flat_map(&:values).all? { %w[agrees unavailable].include?(_1.state) }, observations.inspect)
  end

  private

  def native(name)
    root = ENV.fetch("FSIMG_APFS_NATIVE") { skip "FSIMG_APFS_NATIVE names no native capture directory" }
    image = Unmutated.native(name, root:)
    skip "no native capture #{image.path}" unless File.file?(image.path)
    image
  end
end
