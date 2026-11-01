# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_xz_suffix_creates_an_image_or_refuses_without_output
    arguments = xz_creation_arguments(image: "out.img.XZ")

    if xz_supported?
      invoke(arguments: arguments)
      assert_equal("\xfd7zXZ\x00".b, (@root / "out.img.XZ").binread(6))
      assert_includes(listing(image: "out.img.XZ").join("\n"), "HELLO.TXT")
      damage_xz_index(source: "out.img.XZ", destination: "damaged.xz")
      assert_xz_corrupt(image: "damaged.xz")
    else
      result = invoke(arguments: arguments, expected: 1)
      assert_includes(result.stderr, "writing this compression is not supported".b)
      refute_path_exists(@root / "out.img.XZ")
    end
  end

  def test_xz_file_and_stdin_list_the_same_names_and_verify
    require_xz_support
    invoke(arguments: xz_creation_arguments(image: "out.img.xz"))
    names = listing(image: "out.img.xz")

    (@root / "out.img.xz").open("rb") do |stream|
      result = invoke(arguments: ["t", "-f", "-"], stdin: stream)
      assert_equal(names, result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true))
    end
    invoke(arguments: ["W", "-f", "out.img.xz"])
    info = invoke(arguments: ["i", "-f", "out.img.xz"]).stdout
    assert_includes(info, "Compression:  xz\n".b)

    damage_xz_index(source: "out.img.xz", destination: "damaged.xz")
    (@root / "damaged.xz").open("rb") do |stream|
      result = invoke(arguments: ["t", "-f", "-"], stdin: stream, expected: 1)
      assert_includes(result.stderr, "compressed byte".b)
    end
  end

  def test_xz_staged_verification_does_not_publish_after_spool_exhaustion
    require_xz_support
    invoke(arguments: xz_creation_arguments(image: "verified.xz"))
    assert_path_exists(@root / "verified.xz")

    arguments = xz_creation_arguments(image: "blocked.xz", options: ["-l", "spool=512K"])
    result = invoke(arguments: arguments, expected: 1)
    assert_includes(result.stderr, "spool limit 524288".b)
    refute_path_exists(@root / "blocked.xz")
  end

  def test_xz_required_sandbox_creates_and_reads_files_and_stdin
    require_xz_support
    probe = human_image
    if probe.stderr.include?("sandbox unavailable".b)
      skip_xz("xz required sandbox gate: OS sandbox unavailable")
    end

    invoke(arguments: xz_creation_arguments(image: "x.img.xz", options: ["-S", "required"]))
    assert_equal("\xfd7zXZ\x00".b, (@root / "x.img.xz").binread(6))
    expected_names = listing(image: "human.img")

    ["file", "stdin"].each do |input|
      destination = "from-#{input}"
      private_directory(root: @root, name: destination)
      (@root / "x.img.xz").open("rb") do |stream|
        image = input == "file" ? "x.img.xz" : "-"
        options = input == "file" ? {} : {stdin: stream}
        listed = invoke(arguments: ["t", "-S", "required", "-f", image], **options)
        assert_equal(expected_names, listed.stdout.force_encoding(Encoding::UTF_8).lines(chomp: true), input)
        stream.rewind
        verified = invoke(arguments: ["W", "-S", "required", "-f", image], **options)
        assert_includes(verified.stdout, "filesystem structure verified (2 files, 2 directories)".b, input)
        stream.rewind
        invoke(arguments: ["x", "-S", "required", "-f", image,
          "-E", "-C", destination], **options)
      end

      ["HELLO.TXT", "nested/caf\u00e9.txt"].each do |name|
        same_bytes(first: @root / "source" / name, second: @root / destination / name)
      end
    end
  end

  def test_xz_settings_require_a_selected_compression
    {"xz:level=9" => "xz:level", "xz:check=crc32" => "xz:check"}.each do |setting, key|
      result = invoke(arguments: ["cn", "-F", "fat12", "-o", setting, "source"], expected: 2)
      assert_includes(result.stderr, key.b)
      assert_includes(result.stderr, "setting names a layer the stack does not contain".b)
    end
  end

  def test_xz_preset_nine_and_crc32_plan_or_report_missing_library
    # Planning proves preset 9 is valid without allocating its 674 MiB encoder.
    arguments = ["cn", "-a", "-f", "planned.xz", "-F", "fat12",
      "-o", "xz:level=9", "-o", "xz:check=crc32", "source"]
    result = invoke(arguments: arguments, expected: xz_supported? ? 0 : 1)

    if xz_supported?
      assert_includes(result.stdout, "liblzma".b)
    else
      assert_includes(result.stderr, "writing this compression is not supported".b)
    end
    refute_path_exists(@root / "planned.xz")
  end

  def test_xz_invalid_level_and_check_fail_without_creating_output
    supported = xz_supported?
    [{setting: "xz:level=10", status: 1}, {setting: "xz:check=none", status: 2}].each do |entry|
      result = invoke(arguments: ["c", "-a", "-f", "bad.xz", "-F", "fat12",
        "-o", entry.fetch(:setting), "source"], expected: supported ? entry.fetch(:status) : 1)

      detail = supported ? entry.fetch(:setting).split("=").first : "writing this compression is not supported"
      assert_includes(result.stderr, detail.b)
      refute_path_exists(@root / "bad.xz")
    end
  end

  def test_xz_check_none_reports_incomplete_verification_in_json_and_text
    require_xz_support
    compressor = ENV["XZ_PROGRAM"] || ENV.fetch("PATH").split(File::PATH_SEPARATOR)
      .map { File.join(_1, "xz") }.find { File.file?(_1) && File.executable?(_1) }
    skip_xz("check-none CLI fixture requires xz") unless compressor
    human_image
    (@root / "human.img").open("rb") do |input|
      result = run_process([compressor, "-c", "-0", "--check=none"], cwd: @root, stdin: input)
      assert_equal(0, result.status, result.stderr)
      (@root / "unchecked.xz").binwrite(result.stdout)
    end

    result = invoke(arguments: ["W", "--json", "-f", "unchecked.xz"])
    report = completed(result).find { _1["unverified"] == "xz:check" }
    refute_nil(report, result.stdout)
    assert_equal(false, report.fetch("complete"))
    human = invoke(arguments: ["W", "-f", "unchecked.xz"]).stdout
    assert_includes(human, '"unchecked.xz": xz compression (incomplete: xz:check unverified), filesystem structure verified ('.b)

    damage_xz_index(source: "unchecked.xz", destination: "damaged.xz")
    assert_xz_corrupt(image: "damaged.xz")
  end

  def test_xz_formats_reports_capabilities_and_library_version_together
    format = xz_format
    assert_equal(["level", "check"], format.fetch("settings"))
    assert_equal(".xz", format.fetch("suffix"))
    lines = invoke(arguments: ["--formats"]).stdout.lines
    index = lines.index { _1.match?(/\Axz\s/) }
    refute_nil(index, lines.join)

    if xz_supported?
      assert_equal(7, format.fetch("capabilities"))
      assert_match(/\Aliblzma \S+\z/, format.fetch("codec_version"))
      assert_includes(lines[index + 1], format.fetch("codec_version"))
    else
      assert_nil(format.fetch("codec_version"))
      assert_match(/\bxz\s+compression\s+no\s+no\s+no\b/, lines[index])
      refute_match(/liblzma/, lines[index + 1].to_s)
    end
  end

  private

  def xz_format
    @xz_format ||= completed(invoke(arguments: ["--formats", "--json"]))
      .find { _1["type"] == "format" && _1["name"] == "xz" }
    refute_nil(@xz_format, "xz must have a format catalogue row")
    @xz_format
  end

  def xz_supported? = xz_format.fetch("capabilities") != 0

  def require_xz_support
    skip_xz("xz profile requires liblzma") unless xz_supported?
  end

  def skip_xz(reason)
    puts("SKIP: #{reason}")
    skip(reason)
  end

  def xz_creation_arguments(image:, options: [])
    ["cW", *options, "-a", "-f", image, "-F", "fat12", "-s", "1M",
      "-o", "xz:level=0", "-C", "source", "."]
  end

  def damage_xz_index(source:, destination:)
    bytes = (@root / source).binread
    offset = bytes.bytesize - 16 # Last byte of the index CRC32, before the footer.
    bytes.setbyte(offset, bytes.getbyte(offset) ^ 1)
    (@root / destination).binwrite(bytes)
  end

  def assert_xz_corrupt(image:)
    result = invoke(arguments: ["W", "-f", image], expected: 1)
    assert_includes(result.stderr, "compressed byte".b)
  end
end
