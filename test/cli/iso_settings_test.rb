# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_view_auto_reads_fat_and_joliet_is_inapplicable
    image_fixture(name: "view.img", format: "fat12")
    result = invoke(arguments: ["t", "-S", "off", "-o", "view=auto", "-f", "view.img"])
    assert_includes(result.stdout, "HELLO.TXT".b)
    result = invoke(arguments: ["t", "-S", "off", "-o", "view=joliet", "-f", "view.img"], expected: 1)
    assert_includes(result.stderr, '"view"'.b)
    assert_includes(result.stderr, "read setting does not apply to the image format".b)
  end

  def test_forced_fat_rejects_joliet_before_consuming_stdin
    payload = "unread image bytes".b
    IO.pipe do |reader, writer|
      writer.write(payload)
      writer.close
      reader.nonblock = false

      result = invoke(arguments: ["-t", "-F", "fat12", "-o", "view=joliet", "-f", "-"],
        stdin: reader, expected: 1)

      assert_empty(result.stdout)
      assert_includes(result.stderr, '"view"'.b)
      assert_includes(result.stderr, "read setting does not apply to the image format".b)
      assert_equal(payload, reader.read, "settings rejection must leave stdin unread")
    end
  end

  def test_forced_fat_rejects_joliet_before_opening_file_image
    # A missing image makes input preparation observable: the settings error
    # must take precedence over a host open failure.
    result = invoke(arguments: ["-t", "-F", "fat12", "-o", "view=joliet",
      "-f", "missing.img"], expected: 1)

    assert_empty(result.stdout)
    assert_includes(result.stderr, '"view"'.b)
    assert_includes(result.stderr, "read setting does not apply to the image format".b)
    refute_includes(result.stderr, "No such file".b)
  end

  def test_view_syntax_lists_choices_and_rejects_repeated_keys
    result = invoke(arguments: ["t", "-o", "view=bogus", "-f", "unused.img"], expected: 2)
    %w[view auto rockridge joliet primary].each { assert_includes(result.stderr, _1.b) }
    result = invoke(arguments: ["t", "-o", "view=auto", "-o", "view=primary", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "setting key is given more than once".b)
    assert_includes(result.stderr, "view=primary".b)
    result = invoke(arguments: ["c", "-n", "-o", "view=", "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "setting applies only to reading".b)
    assert_includes(result.stderr, "view=".b)
  end

  def test_iso_numeric_settings_reject_values_outside_their_syntax
    {
      "level=0" => "setting requires a positive decimal count in range",
      "level=4" => "setting must be 1, 2 or 3",
      "bootload=0" => "setting requires a positive decimal count in range",
      "bootload=65536" => "setting requires a positive decimal count in range"
    }.each do |setting, message|
      result = invoke(arguments: ["c", "-n", "-F", "fat12", "-o", setting,
        "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, setting.b)
      assert_includes(result.stderr, message.b)
    end
    result = invoke(arguments: ["c", "-n", "-o", "boot=#{'x' * 1025}",
      "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "setting value exceeds its text capacity".b)
  end

  def test_iso_write_settings_are_refused_for_fat
    %w[level=3 boot=/x efiboot=/x bootload=4].each do |setting|
      result = invoke(arguments: ["c", "-n", "-S", "off", "-F", "fat12",
        "-o", setting, "-C", "source", "."], expected: 1)
      assert_includes(result.stderr, "write setting does not apply to the image format".b)
      assert_includes(result.stderr, "\"#{setting.split('=').first}\"".b)
    end
  end

  def test_iso_write_setting_on_read_command_is_refused
    result = invoke(arguments: ["t", "-o", "level=3", "-f", "unused.img"], expected: 2)
    assert_includes(result.stderr, "setting applies only to creation".b)
    assert_includes(result.stderr, "level=3".b)
  end

  def test_filesystem_json_lists_read_settings_mask
    formats = filesystems(completed(invoke(arguments: ["--formats", "--json"])))
    refute_empty(formats)
    formats.each_value { assert_kind_of(Integer, _1.fetch("read_settings")) }
  end
end
