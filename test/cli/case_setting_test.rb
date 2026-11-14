# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_forced_fat_refuses_case_sensitive_before_worker_start
    result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat12",
      "-o", "case=sensitive", "-C", "missing-source", "."], expected: 1)
    assert_empty(result.stdout)
    assert_includes(result.stderr, '"case"'.b)
    assert_includes(result.stderr, "write setting does not apply to the image format".b)
    refute_includes(result.stderr, "No such file".b)
  end

  def test_case_insensitive_produces_the_default_image
    arguments = ["c", "-S", "off", "-R", "-F", "fat12", "-s", "1M"]
    invoke(arguments: [*arguments, "-f", "default.img", "-C", "source", "."])
    invoke(arguments: [*arguments, "-o", "case=insensitive", "-f", "insensitive.img",
      "-C", "source", "."])
    assert_equal((@root / "default.img").binread, (@root / "insensitive.img").binread)
  end

  def test_bad_case_choice_is_a_usage_error
    %w[case=SENSITIVE case= case=yes case=1].each do |setting|
      result = invoke(arguments: ["c", "-n", "-o", setting, "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, setting.b)
      assert_includes(result.stderr, "insensitive or sensitive".b, setting)
    end
  end

  def test_case_setting_requires_creation
    result = invoke(arguments: ["t", "-o", "case=sensitive", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "setting applies only to creation".b)
  end
end
