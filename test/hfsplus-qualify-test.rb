# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "tmpdir"
require_relative "../tools/hfsplus/qualify"

# The integration wrapper supplies these build artifacts, not source paths.
HFSPLUS_QUALIFY_GENERATOR = File.expand_path(ARGV.shift || "./hfsplus-write-gen").freeze
HFSPLUS_QUALIFY_FSIMG = File.expand_path(ARGV.shift || "./fsimg").freeze

class HfsPlusQualifyTest < Minitest::Test
  def test_content_matches_fixture_byte_samples_across_periods
    # hwf_byte uses provider id 1000 + spec index. Pin offsets around its
    # 251-byte period so a wrong provider id or truncated period is visible.
    {0 => [184, 197, 171, 184, 24], 7 => [52, 65, 39, 52, 143]}.each do |index, bytes|
      actual = HfsPlusQualify.content(index, 510)
      assert_equal(bytes, [0, 1, 250, 251, 509].map { actual.getbyte(_1) }, "spec index #{index}")
      assert_equal(510, actual.bytesize)
      assert_equal(Encoding::BINARY, actual.encoding)
    end
    assert_equal("".b, HfsPlusQualify.content(0, 0))
  end

  def test_native_path_decomposes_and_orders_bmp_marks
    assert_equal("/cafe\u0301".b, HfsPlusQualify.native_path("/caf\u00e9"))
    assert_equal("a\u0f71\u0f72\u0f72".b, HfsPlusQualify.native_path("a\u0f72\u0f73"))
  end

  def test_native_path_keeps_supplementary_marks_unordered
    first = "a\u0300\u{1d165}"
    second = "a\u{1d165}\u0300"
    assert_equal(first.b, HfsPlusQualify.native_path(first))
    assert_equal(second.b, HfsPlusQualify.native_path(second))
    refute_equal(HfsPlusQualify.native_path(first), HfsPlusQualify.native_path(second))
  end

  def test_differences_reports_missing_unexpected_and_changed_fields
    expected = {"/missing" => view(size: 1), "/changed" => view(size: 2)}
    observed = {"/extra" => view(size: 3), "/changed" => view(size: 4)}
    assert_equal(['"/missing" missing', '"/extra" unexpected', '"/changed" size 4, not 2'],
      HfsPlusQualify.differences(expected, observed, %i[kind size]))
  end

  def test_differences_ignores_nil_expected_fields
    assert_empty(HfsPlusQualify.differences({"/file" => view(size: nil)},
      {"/file" => view(size: 99)}, %i[kind size mode]))
  end

  def test_collision_pairs_follow_each_variants_comparator
    %w[hfsplus hfsx].each do |variant|
      pairs = HfsPlusQualify.collision_pairs(variant)
      assert_equal(15, pairs.size, variant)
      assert_equal(15, pairs.map { [_1.first, _1.second] }.uniq.size, variant)
      expected = [true] * 8 + [variant == "hfsplus"] * 5 + [false] * 2
      assert_equal(expected, pairs.map(&:collide), variant)
      case_pair = pairs.find { _1.first == "Zeta" }
      assert_equal("zeta", case_pair.second)
      assert_equal(variant == "hfsplus", case_pair.collide)
    end
  end

  def test_length_names_reach_255_utf16_units_in_each_spelling
    names = HfsPlusQualify.length_names
    assert_equal(names.size, names.uniq.size)
    assert(names.all? { utf16_units(_1) <= 255 })
    ["x" * 255, "#{"\u00e9" * 127}x", "#{"\u{1f600}" * 127}x",
     "#{"\uac00" * 127}x", "\ud55c" * 85].each do |name|
      assert_includes(names, name)
      assert_equal(255, utf16_units(name))
    end
  end

  def test_overlong_names_exceed_255_utf16_units
    assert_equal(4, HfsPlusQualify::OVERLONG_NAMES.size)
    HfsPlusQualify::OVERLONG_NAMES.each do |name|
      assert_operator(utf16_units(name), :>, 255, name.inspect)
    end
  end

  def test_smallest_empty_native_cycle_passes_all_seven_steps
    return native_skip("not macOS") unless RUBY_PLATFORM.include?("darwin")
    return native_skip("hdiutil is absent") unless program?("hdiutil")

    assert(File.executable?(HFSPLUS_QUALIFY_GENERATOR), "built generator is required")
    assert(File.executable?(HFSPLUS_QUALIFY_FSIMG), "built fsimg is required")
    Dir.mktmpdir("hfsplus-qualify-", Dir.pwd) do |root|
      script = File.expand_path("../tools/hfsplus/qualify.rb", __dir__)
      out, status = Open3.capture2e(RbConfig.ruby, script,
        "--generator", HFSPLUS_QUALIFY_GENERATOR, "--fsimg", HFSPLUS_QUALIFY_FSIMG,
        "--root", root, "--only", "^hfsplus-smallest-empty$")
      rows = JSON.parse(File.read(File.join(root, "results.json")))
      assert_equal(1, rows.size, out)
      row = rows.fetch(0)
      assert_equal("hfsplus-smallest-empty", row.fetch("image"))
      assert_equal("pass", row.fetch("generate"), out)
      assert_equal("pass", row.fetch("detach"), out)
      # No separate probe attaches an external image. The cycle itself only
      # attaches its generated image and records an attach refusal explicitly.
      if row.fetch("fsck_before").start_with?("FAIL: cannot attach ")
        return native_skip(row.fetch("fsck_before"))
      end
      assert(status.success?, out)
      steps = %w[generate fsck_before mount modify fsck_after remount fsimg]
      assert_equal(["pass"] * 7, row.values_at(*steps), out)
    end
  end

  private

  def view(size:) = HfsPlusQualify::View.of(kind: "file", size: size)

  def utf16_units(name) = name.unicode_normalize(:nfd).encode(Encoding::UTF_16BE).bytesize / 2

  def program?(name)
    ENV.fetch("PATH", "").split(File::PATH_SEPARATOR).any? do |directory|
      path = File.join(directory, name)
      File.file?(path) && File.executable?(path)
    end
  end

  def native_skip(reason)
    puts "SKIP: HFS Plus native qualification: #{reason}"
    assert(true, "native qualification unavailable")
  end
end
