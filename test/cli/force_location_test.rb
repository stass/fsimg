# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  FORCE_LOCATION_ERROR = "--force needs the volume's location: use -F fs/- or --partition".b.freeze

  # Each format and stack row generates its own test so a refusal cannot hide
  # the remaining cases. Minitest invokes every generated method separately.
  %w[fat12 qnx4 exfat qnx6 iso9660].each do |format|
    define_method("test_force_location_intact_partition_#{format}") do
      scheme = format == "iso9660" ? "gpt" : "mbr"
      volume = force_location_volume(format: format)
      force_location_disk(volumes: [volume], scheme: scheme)
      arguments = ["-F", "#{format}/#{scheme}", "--partition", "1"]
      expected = force_location_entries(arguments: arguments)
      refute_empty(expected)
      assert_equal(expected, force_location_entries(arguments: [*arguments, "--force"]))
    end

    define_method("test_force_location_refuses_unlocated_#{format}") do
      volume = force_location_volume(format: format)
      force_location_disk(volumes: [volume], scheme: format == "iso9660" ? "gpt" : "mbr")
      result = invoke(arguments: ["t", "-F", format, "--force", "-f", "disk.img"], expected: 2)
      assert_includes(result.stderr.lines(chomp: true), "fsimg: ".b + FORCE_LOCATION_ERROR)
    end
  end

  {
    named_scheme: ["-F", "fat12/mbr"],
    detected_scheme_in_container: ["-F", "fat12/vhd"],
    forced_compression: ["-F", "fat12", "--compress=gzip"]
  }.each do |name, stack|
    define_method("test_force_location_refuses_#{name}") do
      result = invoke(arguments: ["t", *stack, "--force", "-f", "unused.img"], expected: 2)
      assert_includes(result.stderr.lines(chomp: true), "fsimg: ".b + FORCE_LOCATION_ERROR)
    end
  end

  %w[fat12 qnx4 qnx6].each do |format|
    %w[bare mbr].each do |location|
      define_method("test_force_location_damaged_#{format}_#{location}") do
        volume = force_location_volume(format: format)
        arguments = ["-F", "#{format}/#{location == 'bare' ? '-' : 'mbr'}"]
        if location == "bare"
          (@root / "disk.img").binwrite(volume)
        else
          force_location_disk(volumes: [volume])
          arguments += ["--partition", "1"]
        end
        expected = force_location_entries(arguments: arguments)
        damaged = force_location_damage(volume: volume, format: format)
        if location == "bare"
          (@root / "disk.img").binwrite(damaged)
        else
          force_location_disk(volumes: [damaged])
        end
        invoke(arguments: ["t", *arguments, "-f", "disk.img"], expected: 1)
        assert_equal(expected, force_location_entries(arguments: [*arguments, "--force"]))
        result = invoke(arguments: ["W", *arguments, "--force", "-f", "disk.img"], expected: 1)
        assert_includes(result.stderr, "#{format} identity was assumed:".b)
      end
    end
  end

  def test_force_location_detection_ignores_damaged_second_partition
    volume = force_location_volume(format: "fat12")
    damaged = force_location_damage(volume: volume, format: "fat12")
    force_location_disk(volumes: [volume])
    expected = force_location_entries(arguments: ["-F", "fat12/mbr", "--partition", "1"])
    force_location_disk(volumes: [volume, damaged])
    assert_equal(expected, force_location_entries(arguments: ["-F", "mbr", "--force"]))
    assert_equal(expected, force_location_entries(
      arguments: ["-F", "fat12/mbr", "--partition", "2", "--force"]))
  end

  def test_force_location_reads_damaged_second_partition
    volume = force_location_volume(format: "fat12")
    force_location_disk(volumes: [volume])
    expected = force_location_entries(arguments: ["-F", "fat12/mbr", "--partition", "1"])
    force_location_disk(volumes: [volume, force_location_damage(volume: volume, format: "fat12")])
    assert_equal(expected, force_location_entries(
      arguments: ["-F", "fat12/mbr", "--partition", "2", "--force"]))
  end

  def test_force_location_partition_selector_detects_scheme
    force_location_disk(volumes: [force_location_volume(format: "qnx4")])
    arguments = ["-F", "qnx4", "--partition", "1"]
    assert_equal(force_location_entries(arguments: arguments),
      force_location_entries(arguments: [*arguments, "--force"]))
  end

  %w[fat12 fat12/- fat12/-/-].each do |stack|
    define_method("test_force_location_hint_bare_#{stack.tr('/', '_')}") do
      volume = force_location_volume(format: "fat12")
      (@root / "disk.img").binwrite(volume)
      expected = force_location_entries(arguments: ["-F", stack])
      (@root / "disk.img").binwrite(force_location_damage(volume: volume, format: "fat12"))
      suggested = stack == "fat12/-/-" ? stack : "fat12/-"
      force_location_hint(arguments: ["-F", stack],
        hint: "use -F #{suggested} --force to read it as fat12 anyway")
      assert_equal(expected, force_location_entries(arguments: ["-F", suggested, "--force"]))
    end
  end

  %w[qnx4/mbr qnx4].each do |stack|
    define_method("test_force_location_hint_index_#{stack.tr('/', '_')}") do
      fat = force_location_volume(format: "fat12")
      qnx = force_location_volume(format: "qnx4")
      force_location_disk(volumes: [fat, force_location_damage(volume: qnx, format: "qnx4")])
      force_location_hint(arguments: ["-F", stack, "--partition", "2"],
        hint: "use -F #{stack} --partition 2 --force to read it as qnx4 anyway")
    end
  end

  def test_force_location_hint_requires_partition_placeholder
    force_location_disk(volumes: [force_location_volume(format: "fat12")])
    force_location_hint(arguments: ["-F", "qnx4/mbr"],
      hint: "use -F qnx4/mbr --partition N --force to read it as qnx4 anyway")
  end

  { name: ["--partition", "name=volume1"], type: ["--partition", "type=#{CliImages::BASIC_TYPE}"],
    offset: ["--offset", "20480", "--length", "1048576"] }.each do |name, selector|
    define_method("test_force_location_hint_#{name}") do
      volume = force_location_volume(format: "fat12")
      force_location_disk(volumes: [force_location_damage(volume: volume, format: "fat12")], scheme: "gpt")
      force_location_hint(arguments: ["-F", "fat12", *selector],
        hint: "use --force to read it as fat12 anyway")
    end
  end

  def test_force_location_hint_preserves_failed_layer
    force_location_disk(volumes: [force_location_volume(format: "fat12")])
    File.open(@root / "disk.img", "r+b") { _1.seek(510); _1.write("\0\0".b) }
    force_location_hint(arguments: ["-F", "mbr"],
      hint: "use --force to read it as mbr anyway")
  end

  %w[fat12/mbr fat12/mbr/-].each do |stack|
    { automatic: [], index: ["--partition", "1"] }.each do |selector, arguments|
      define_method("test_force_location_hint_failed_mbr_#{stack.tr('/', '_')}_#{selector}") do
        force_location_disk(volumes: [force_location_volume(format: "fat12")])
        expected = force_location_entries(arguments: ["-F", stack, "--partition", "1"])
        File.open(@root / "disk.img", "r+b") { _1.seek(510); _1.write("\0\0".b) }
        partition = selector == :automatic ? "N" : "1"
        force_location_hint(arguments: ["-F", stack, *arguments],
          hint: "use -F #{stack} --partition #{partition} --force to read it as mbr anyway")
        assert_equal(expected, force_location_entries(
          arguments: ["-F", stack, "--partition", "1", "--force"]))
      end
    end
  end

  { container: ["-F", "vhd"], compression: ["--compress=gzip"],
    empty_scheme_container: ["-F", "fat12/-/vhd"] }.each do |name, arguments|
    define_method("test_force_location_accepts_#{name}") do
      volume = force_location_volume(format: "fat12")
      bytes = name == :compression ? gzip_bytes(volume) : vhd_image(disk: volume)
      (@root / "disk.img").binwrite(bytes)
      expected = force_location_entries(arguments: [])
      assert_equal(expected, force_location_entries(arguments: [*arguments, "--force"]))
    end
  end

  private

  def force_location_volume(format:)
    image_fixture(name: "#{format}.img", format: format, size: format == "fat12" ? "1M" : nil)
    (@root / "#{format}.img").binread
  end

  def force_location_damage(volume:, format:)
    damaged = volume.dup
    case format
    when "fat12"
      damaged[0, 3] = "\0".b * 3
      damaged[510, 2] = "\0\0".b
      reserved = damaged[14, 2].unpack1("v")
      sectors = damaged[22, 2].unpack1("v")
      damaged.getbyte(16).times { damaged[(reserved + _1 * sectors) * 512, 3] = "\0".b * 3 }
    when "qnx4"
      damaged[512, 16] = "\0".b * 16
    when "qnx6"
      damaged[8192, 4] = "\0".b * 4
    end
    damaged
  end

  def force_location_disk(volumes:, scheme: "mbr")
    first = 40
    partitions = volumes.each_with_index.map do |volume, index|
      count = (volume.bytesize + 511) / 512
      partition = if scheme == "mbr"
        MbrPartition.new(first_lba: first, sector_count: count, type: 0x83, payload: volume)
      else
        GptPartition.new(first_lba: first, last_lba: first + count - 1,
          type: BASIC_TYPE, name: "volume#{index + 1}", payload: volume)
      end
      first += count
      partition
    end
    bytes = scheme == "mbr" ? mbr_disk(sectors: first + 40, partitions: partitions) :
      gpt_disk(sectors: first + 40, partitions: partitions)
    (@root / "disk.img").binwrite(bytes)
  end

  def force_location_entries(arguments:)
    completed(invoke(arguments: ["t", "--json", *arguments, "-f", "disk.img"]))
      .select { _1.fetch("type") == "entry" }
  end

  def force_location_hint(arguments:, hint:)
    result = invoke(arguments: ["t", *arguments, "-f", "disk.img"], expected: 1)
    assert_equal("; #{hint}\n".b, result.stderr.lines.last[/; use .*\n\z/n], result.stderr)
  end
end
