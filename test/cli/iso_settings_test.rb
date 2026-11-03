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

  def test_iso_descriptor_keys_cross_the_worker_and_return_in_table_order
    settings = {"volset" => "SET 1", "publisher" => "_PUBLISHER.TXT", "preparer" => "PREPARER",
      "application" => 'APP "QUOTED"', "copyright" => "COPY.TXT;1", "abstract" => "ABSTRACT.TXT;1",
      "biblio" => "BIBLIO.TXT;1", "created" => "0", "modified" => "-1", "expires" => "1", "effective" => "2"}
    iso_descriptor_create(name: "metadata.iso", settings:)
    expected = settings.to_h do |key, value|
      name = {"volset" => "volume_set", "copyright" => "copyright_file", "abstract" => "abstract_file",
        "biblio" => "biblio_file"}.fetch(key, key)
      [name, %w[created modified expires effective].include?(key) ? Time.at(Integer(value)).utc.strftime("%Y-%m-%d %H:%M:%S") : value]
    end

    %w[primary rockridge joliet].each do |view|
      arguments = ["i", "-S", "off", "-o", "view=#{view}", "-f", "metadata.iso"]
      info = completed(invoke(arguments: [*arguments, "--json"])).find { _1.fetch("type") == "info" }
      assert_equal(expected, info.fetch("texts"), view)
      human = invoke(arguments:).stdout
      lines = human.lines.select { |line| expected.keys.any? { line.start_with?("#{_1}:".b) } }
      assert_equal(expected.map { |key, value| "#{key}: #{JSON.generate(value)}\n".b }, lines, view)
    end
  end

  def test_iso_empty_text_and_zero_dates_are_distinct_from_omission
    iso_descriptor_create(name: "empty.iso", settings: %w[volset publisher preparer application copyright abstract biblio].to_h { [_1, ""] }.merge(
      "created" => "0", "expires" => "0", "effective" => "0"))
    %w[primary joliet].each do |view|
      info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-o", "view=#{view}", "-f", "empty.iso"]))
        .find { _1.fetch("type") == "info" }.fetch("texts")
      assert_equal({"created" => "1970-01-01 00:00:00", "modified" => "2023-11-14 22:13:20",
        "expires" => "1970-01-01 00:00:00", "effective" => "1970-01-01 00:00:00"}, info)
    end
    iso_descriptor_create(name: "default.iso", settings: {})
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "default.iso"]))
      .find { _1.fetch("type") == "info" }.fetch("texts")
    assert_equal("FSIMG", info.fetch("application"))
    refute(info.key?("expires"))
    refute(info.key?("effective"))
  end

  def test_iso_descriptor_syntax_refusals_identify_every_key
    text_keys = %w[volset publisher preparer application copyright abstract biblio]
    date_keys = %w[created modified expires effective]
    text_keys.each do |key|
      capacity = %w[copyright abstract biblio].include?(key) ? 18 : 64
      ["A" * (capacity + 1), "a", "\e", capacity == 18 ? "/" : "@"].each do |value|
        setting = "#{key}=#{value}"
        result = invoke(arguments: ["c", "-n", "-F", "iso9660", "-o", setting, "-C", "source", "."], expected: value.length > capacity ? 2 : 1)
        assert_includes(result.stderr, key.b)
      end
    end
    date_keys.each do |key|
      ["+1", "1s", "", "9223372036854775808"].each do |value|
        result = invoke(arguments: ["c", "-n", "-o", "#{key}=#{value}", "-C", "source", "."], expected: 2)
        assert_includes(result.stderr, key.b)
      end
    end
    (text_keys + date_keys).each do |key|
      value = date_keys.include?(key) ? "0" : "A"
      result = invoke(arguments: ["c", "-n", "-o", "#{key}=#{value}", "-o", "#{key}=#{value}",
        "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, "setting key is given more than once".b)
      assert_includes(result.stderr, key.b)
    end
  end

  def test_iso_info_escapes_quotes_and_never_emits_hostile_text_raw
    invoke(executable: @executable.dirname / "test-iso9660",
      arguments: ["--seed", (@root / "hostile.iso").to_s])
    bytes = (@root / "hostile.iso").binread
    value = 'producer "quoted" \\ path'
    bytes[16 * 2048 + 574, 128] = value.b.ljust(128, " ")
    bytes[16 * 2048 + 318, 128] = "BAD\e[31m".b.ljust(128, " ")
    (@root / "hostile.iso").binwrite(bytes)

    human = invoke(arguments: ["i", "-S", "off", "-f", "hostile.iso"]).stdout
    assert_includes(human, "application: #{JSON.generate(value)}\n".b)
    refute_includes(human, "\e".b)
    refute_includes(human, "publisher:".b)
    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "hostile.iso"]))
      .find { _1.fetch("type") == "info" }.fetch("texts")
    assert_equal({"application" => value}, info)
  end

  private

  def iso_descriptor_create(name:, settings:)
    options = settings.flat_map { |key, value| ["-o", "#{key}=#{value}"] }
    invoke(arguments: ["c", "-S", "off", "-R", "-e", "1700000000", "-F", "iso9660", "-f", name,
      *options, "-C", "source", "."])
  end
end
