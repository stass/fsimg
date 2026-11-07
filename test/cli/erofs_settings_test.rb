# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_erofs_write_settings_are_refused_for_fat
    %w[compression=lz4 addressing=48 metabox=plain metabox=lz4].each do |setting|
      result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat12",
        "-o", setting, "-C", "source", "."], expected: 1)
      assert_includes(result.stderr, "write setting does not apply to the image format".b)
      assert_includes(result.stderr, "\"#{setting.split("=").first}\"".b)
    end
  end

  def test_erofs_default_choices_are_accepted_for_fat
    %w[compression=none metabox=none].each do |setting|
      invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat12",
        "-o", setting, "-C", "source", "."])
    end
  end

  def test_erofs_setting_syntax_lists_choices
    {
      "compression=bogus" => %w[none lz4],
      "addressing=64" => %w[32 48],
      "metabox=bogus" => %w[none plain lz4]
    }.each do |setting, choices|
      result = invoke(arguments: ["c", "-n", "-o", setting, "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, setting.b)
      choices.each { assert_includes(result.stderr, _1.b, setting) }
    end
  end

  def test_erofs_write_setting_on_read_command_is_refused
    result = invoke(arguments: ["t", "-o", "compression=lz4", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "setting applies only to creation".b)
    assert_includes(result.stderr, "compression=lz4".b)
  end
end
