# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_placement_repeats_up_to_eight_and_ninth_names_the_overflow_word
    settings = 8.times.flat_map { ["-o", "place=/file#{_1}@#{_1}M"] }
    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      *settings, "-C", "source", "."], expected: 1)
    assert_includes(result.stderr, "write setting does not apply to the image format".b)
    assert_includes(result.stderr, '"place"'.b)

    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      *settings, "-o", "place=/ninth@9M", "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "setting key is given more times than its list holds".b)
    assert_includes(result.stderr, "place=/ninth@9M".b)
  end

  def test_placement_syntax_errors_name_the_rejected_word
    cases = {
      "/k" => "placement requires a nonempty PATH@OFFSET",
      "@4M" => "placement requires a nonempty PATH@OFFSET",
      "/k@" => "placement offset requires a byte quantity in range",
      "/k@x" => "placement offset requires a byte quantity in range",
      "/k@4Mx" => "placement offset requires a byte quantity in range"
    }
    cases.each do |value, diagnostic|
      word = "place=#{value}"
      result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
        "-o", word, "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, diagnostic.b, word)
      assert_includes(result.stderr, word.b)
    end
  end

  def test_placement_path_past_text_capacity_is_a_syntax_error
    value = "/#{'k' * 1024}@4M"
    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      "-o", "place=#{value}", "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "setting value exceeds its text capacity".b)
  end

  def test_placement_path_containing_at_sign_splits_at_the_last_at_sign
    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      "-o", "place=/a@b@4M", "-C", "source", "."], expected: 1)
    assert_includes(result.stderr, "write setting does not apply to the image format".b)
    assert_includes(result.stderr, '"place"'.b)
  end

  def test_placement_setting_requires_creation
    %w[t x].each do |mode|
      result = invoke(arguments: [mode, "-o", "place=/k@4M", "-f", "unused.img"], expected: 2)
      assert_includes(result.stderr, "setting applies only to creation".b, mode)
      assert_includes(result.stderr, "place=/k@4M".b, mode)
    end
  end

  def test_placement_applicability_is_checked_after_geometry_syntax
    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      "-o", "place=/k@4M", "-o", "cluster=3", "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "geometry byte quantities must be powers of two".b)

    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat16",
      "-o", "place=/k@4M", "-C", "source", "."], expected: 1)
    assert_includes(result.stderr, "write setting does not apply to the image format".b)
    assert_includes(result.stderr, '"place"'.b)
  end

  def test_placement_is_refused_for_formats_without_the_capability_before_worker_start
    %w[fat16 fat32 exfat iso9660].each do |format|
      result = invoke(arguments: ["c", "-n", "-S", "off", "-F", format,
        "-o", "place=/k@4M", "-C", "missing-source", "."], expected: 1)
      assert_includes(result.stderr, "write setting does not apply to the image format".b, format)
      assert_includes(result.stderr, '"place"'.b, format)
    end
  end
end
