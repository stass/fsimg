# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_iso_devices_short_listing_preserves_order_and_marks_socket
    iso_devices_fixture
    result = invoke(arguments: ["t", "-f", "devices.iso"])
    expected = <<~LISTING
      "/A.CHAR"
      "/B.BLOCK"
      "/C.SOCKET"=
      "/D.TXT"
      "/E.TXT"
    LISTING
    assert_equal(expected, result.stdout)
  end

  def test_iso_devices_long_listing_shows_native_words_and_zero_content_kinds
    iso_devices_fixture
    result = invoke(arguments: ["t", "-v", "-f", "devices.iso"])
    expected = [
      'crw-r-----         4, 2 1970-01-01 00:00:01 "/A.CHAR"',
      'brw-r-----      0, 1026 1970-01-01 00:00:01 "/B.BLOCK"',
      'srw-r-----            - 1970-01-01 00:00:01 "/C.SOCKET"=',
      '-rw-r-----            5 1970-01-01 00:00:01 "/D.TXT"',
      '-rw-r-----            5 1970-01-01 00:00:01 "/E.TXT"'
    ]
    assert_equal(expected, result.stdout.lines(chomp: true))
  end

  def test_iso_devices_json_reports_device_words_only_on_devices
    iso_devices_fixture
    entries = completed(invoke(arguments: ["t", "--json", "-f", "devices.iso"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(%w[char_device block_device socket file file], entries.map { _1.fetch("kind") })
    assert_equal([[4, 2], [0, 1026], [nil, nil], [nil, nil], [nil, nil]],
      entries.map { [_1.fetch("dev_major"), _1.fetch("dev_minor")] })
    assert_equal(%w[0 0 0 5 5], entries.map { _1.fetch("size_bytes") })
  end

  def test_iso_formats_advertises_read_only_devices
    formats = filesystems(completed(invoke(arguments: ["--formats", "--json"])))
    assert_equal(127, formats.fetch("iso9660").fetch("entry_kinds"))
    assert_equal(3, formats.fetch("fat16").fetch("entry_kinds"))
  end

  def test_iso_formats_names_read_only_devices
    lines = invoke(arguments: ["--formats", "-v"]).stderr.lines
    records = lines.slice_before(/\A\S+:/).to_a
    iso = records.find { _1.first.start_with?("iso9660:") }
    fat = records.find { _1.first.start_with?("fat16:") }
    refute_nil(iso)
    refute_nil(fat)
    iso_kinds = iso.find { _1.start_with?("  Entry kinds:") }.split(":", 2).last.strip.split(", ")
    fat_kinds = fat.find { _1.start_with?("  Entry kinds:") }.split(":", 2).last.strip.split(", ")
    %w[char_device block_device socket].each do |kind|
      assert_includes(iso_kinds, kind)
      refute_includes(fat_kinds, kind)
    end
  end

  def test_iso_devices_extraction_refuses_first_device_and_is_incomplete
    iso_devices_fixture
    (@root / "restore").mkdir
    error = failed(invoke(arguments: ["x", "--json", "-f", "devices.iso", "-C", "restore"], expected: 1))
    assert_equal(-6, error.fetch("code"))
    assert_equal("/A.CHAR", error.fetch("path"))
    assert_equal("incomplete", error.fetch("output_state"))
    assert_empty((@root / "restore/devices").children)
  end

  def test_iso_devices_extraction_skips_and_records_devices_in_manifest
    iso_devices_fixture
    (@root / "restore").mkdir
    result = invoke(arguments: ["x", "-k", "--json", "-f", "devices.iso", "-C", "restore", "--mtree=devices.mtree"])
    summary = completed(result).last
    assert_equal("3", summary.fetch("skipped_entries"))
    assert_equal(%w[D.TXT E.TXT], (@root / "restore/devices").children.map { _1.basename.to_s }.sort)
    %w[D.TXT E.TXT].each { assert_equal("hello", (@root / "restore/devices" / _1).binread) }
    manifest = (@root / "devices.mtree").read
    char = manifest.lines.find { _1.start_with?("./A.CHAR ") }
    refute_nil(char)
    assert_includes(char, "type=char")
    assert_includes(char, "device=native,4,2")
    assert_includes(manifest, "type=block")
    assert_includes(manifest, "device=native,0,1026")
    assert_includes(manifest, "type=socket")
    refused = invoke(arguments: ["c", "-F", "iso9660", "-f", "copy.iso", "@devices.mtree"], expected: 1)
    assert_includes(refused.stderr.lines(chomp: true),
      'fsimg: manifest line 3: unsupported keyword \"type\" value: block, char and socket entries are not stored "/A.CHAR" at byte 23')
  end

  def test_iso_devices_verbose_extraction_names_skipped_entries
    iso_devices_fixture
    (@root / "restore").mkdir
    result = invoke(arguments: ["x", "-k", "-v", "-f", "devices.iso", "-C", "restore"])
    assert_includes(result.stderr.lines(chomp: true), "fsimg: warning: 3 entries of unsupported kinds were skipped")
    ['"/A.CHAR"', '"/B.BLOCK"', '"/C.SOCKET"='].each do |name|
      assert_includes(result.stderr.lines(chomp: true), name)
    end
  end

  def test_iso_device_to_stdout_refuses_wrong_kind
    iso_devices_fixture
    result = invoke(arguments: ["x", "-O", "-f", "devices.iso", "/A.CHAR"], expected: 1)
    assert_empty(result.stdout)
    assert_includes(result.stderr, "wrong entry kind")
    assert_includes(result.stderr, "/A.CHAR")
  end

  def test_iso_devices_verification_counts_zero_byte_special_entries
    iso_devices_fixture
    records = completed(invoke(arguments: ["W", "--json", "-f", "devices.iso"]))
    report = records.find { _1.fetch("type") == "verify" }
    assert_equal("5", report.fetch("files"))
    assert_equal("10", report.fetch("logical_bytes"))
    summary = records.last
    assert_equal("W", summary.fetch("command"))
  end

  private

  def iso_devices_fixture
    helper = @executable.dirname / "test-iso9660"
    invoke(executable: helper, arguments: ["--seed-devices", (@root / "devices.iso").to_s])
  end
end
