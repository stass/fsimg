# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  INAPPLICABLE_READ_SETTING = "read setting does not apply to the image format".b

  # A detected format is known only inside the worker, so these refusals
  # show that the job carried each setting to the reader.
  def test_detected_fat_refuses_volume_and_snapshot_in_the_worker
    image_fixture(name: "selection.img", format: "fat12")
    { "volume=0" => '"volume"', "volume=4294967295" => '"volume"',
      "snapshot=before-upgrade" => '"snapshot"' }.each do |setting, key|
      result = invoke(arguments: ["t", "-S", "off", "-o", setting, "-f", "selection.img"],
        expected: 1)
      assert_empty(result.stdout, setting)
      assert_includes(result.stderr, key.b, setting)
      assert_includes(result.stderr, INAPPLICABLE_READ_SETTING, setting)
    end
  end

  def test_forced_fat_refuses_volume_and_snapshot_before_opening_the_image
    { "volume=1" => '"volume"', "snapshot=s" => '"snapshot"' }.each do |setting, key|
      result = invoke(arguments: ["i", "-F", "fat12", "-o", setting, "-f", "missing.img"],
        expected: 1)
      assert_includes(result.stderr, key.b, setting)
      assert_includes(result.stderr, INAPPLICABLE_READ_SETTING, setting)
      refute_includes(result.stderr, "No such file".b, setting)
    end
  end

  def test_malformed_snapshot_names_are_refused_before_opening_the_image
    ["snapshot=", "snapshot=\xff".b, "snapshot=\xc0\xaf".b].each do |setting|
      result = invoke(arguments: ["t", "-o", setting, "-f", "missing.img"], expected: 1)
      assert_includes(result.stderr, '"snapshot"'.b, setting)
      assert_includes(result.stderr, "setting must be UTF-8 of 1 to 255 bytes".b, setting)
      refute_includes(result.stderr, "No such file".b, setting)
    end
  end

  def test_snapshot_name_capacity_is_255_bytes
    result = invoke(arguments: ["t", "-o", "snapshot=#{"n" * 256}", "-f", "unused.img"],
      expected: 2)
    assert_includes(result.stderr, "setting value exceeds its text capacity".b)
    result = invoke(arguments: ["i", "-F", "fat12", "-o", "snapshot=#{"n" * 255}",
      "-f", "missing.img"], expected: 1)
    assert_includes(result.stderr, INAPPLICABLE_READ_SETTING)
  end

  def test_volume_syntax_and_mode_refusals
    %w[volume=-1 volume=4294967296 volume=1x volume=].each do |setting|
      result = invoke(arguments: ["t", "-o", setting, "-f", "unused.img"], expected: 2)
      assert_includes(result.stderr, "setting requires a decimal count in range".b, setting)
    end
    result = invoke(arguments: ["t", "-o", "volume=1", "-o", "volume=2", "-f", "unused.img"],
      expected: 2)
    assert_includes(result.stderr, "setting key is given more than once".b)
    %w[volume=1 snapshot=s].each do |setting|
      result = invoke(arguments: ["c", "-n", "-o", setting, "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, "setting applies only to reading".b, setting)
    end
  end
end
