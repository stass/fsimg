# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_zstd_suffix_creates_an_image_or_refuses_without_output
    arguments = zstd_creation_arguments(image: "out.img.ZST")

    if zstd_supported?
      invoke(arguments: arguments)
      assert_equal("\x28\xb5\x2f\xfd".b, (@root / "out.img.ZST").binread(4))
      assert_includes(listing(image: "out.img.ZST").join("\n"), "HELLO.TXT")
      damage_zstd_checksum(source: "out.img.ZST", destination: "damaged.zst")
      assert_zstd_corrupt(image: "damaged.zst")
    else
      result = invoke(arguments: arguments, expected: 1)
      assert_includes(result.stderr, 'writing this compression is not supported "zstd"'.b)
      refute_path_exists(@root / "out.img.ZST")
    end
  end

  def test_zstd_file_and_stdin_list_the_same_names_and_verify
    require_zstd_support
    invoke(arguments: zstd_creation_arguments(image: "out.img.zst"))
    names = listing(image: "out.img.zst")

    (@root / "out.img.zst").open("rb") do |stream|
      result = invoke(arguments: ["t", "-f", "-"], stdin: stream)
      assert_equal(names, result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true))
    end
    ["t", "W", "i"].each do |mode|
      invoke(arguments: [mode, "--compress=zstd", "-f", "out.img.zst"])
    end
    private_directory(root: @root, name: "decoded")
    invoke(arguments: ["x", "--compress=zstd", "-f", "out.img.zst", "-E", "-C", "decoded"])
    ["HELLO.TXT", "nested/caf\u00e9.txt"].each do |name|
      same_bytes(first: @root / "source" / name, second: @root / "decoded" / name)
    end
    info = invoke(arguments: ["i", "-f", "out.img.zst"]).stdout
    assert_includes(info, "Compression:  zstd\n".b)

    damage_zstd_checksum(source: "out.img.zst", destination: "damaged.zst")
    (@root / "damaged.zst").open("rb") do |stream|
      result = invoke(arguments: ["t", "-f", "-"], stdin: stream, expected: 1)
      assert_includes(result.stderr, "compressed byte".b)
    end
  end

  def test_zstd_staged_verification_does_not_publish_after_spool_exhaustion
    require_zstd_support
    invoke(arguments: zstd_creation_arguments(image: "verified.zst"))
    assert_path_exists(@root / "verified.zst")

    arguments = zstd_creation_arguments(image: "blocked.zst", options: ["-l", "spool=512K"])
    result = invoke(arguments: arguments, expected: 1)
    assert_includes(result.stderr, "spool limit 524288".b)
    refute_path_exists(@root / "blocked.zst")
  end

  def test_zstd_required_sandbox_creates_and_reads_files_and_stdin
    require_zstd_support
    probe = human_image
    if probe.stderr.include?("sandbox unavailable".b)
      skip_zstd("zstd required sandbox gate: OS sandbox unavailable")
    end

    invoke(arguments: zstd_creation_arguments(image: "x.img.zst", options: ["-S", "required"]))
    assert_equal("\x28\xb5\x2f\xfd".b, (@root / "x.img.zst").binread(4))
    expected_names = listing(image: "human.img")

    ["file", "stdin"].each do |input|
      destination = "from-#{input}"
      private_directory(root: @root, name: destination)
      (@root / "x.img.zst").open("rb") do |stream|
        image = input == "file" ? "x.img.zst" : "-"
        options = input == "file" ? {} : {stdin: stream}
        listed = invoke(arguments: ["t", "-S", "required", "-f", image], **options)
        assert_equal(expected_names, listed.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true), input)
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

  def test_zstd_settings_require_a_selected_compression
    ["zstd:level=19", "zstd:level=0"].each do |setting|
      result = invoke(arguments: ["cn", "-F", "fat12", "-o", setting, "source"], expected: 2)
      assert_includes(result.stderr, "zstd:level".b)
      assert_includes(result.stderr, "setting names a layer the stack does not contain".b)
    end
  end

  def test_zstd_level_nineteen_plans_or_reports_missing_library
    arguments = ["cn", "-a", "-f", "planned.zst", "-F", "fat12",
      "-o", "zstd:level=19", "source"]
    result = invoke(arguments: arguments, expected: zstd_supported? ? 0 : 1)

    if zstd_supported?
      assert_includes(result.stdout, "zstd".b)
      invoke(arguments: ["c", "--compress=zstd", "-f", "level19.zst", "-F", "fat12",
        "-o", "zstd:level=19", "source"])
      invoke(arguments: ["W", "-f", "level19.zst"])
    else
      assert_includes(result.stderr, 'writing this compression is not supported "zstd"'.b)
    end
    refute_path_exists(@root / "planned.zst")
  end

  def test_zstd_level_twenty_is_refused_without_output
    result = invoke(arguments: ["c", "--compress=zstd", "-f", "bad.zst", "-F", "fat12",
      "-o", "zstd:level=20", "source"], expected: 1)
    detail = zstd_supported? ? "zstd:level" : 'writing this compression is not supported "zstd"'
    assert_includes(result.stderr, detail.b)
    refute_path_exists(@root / "bad.zst")
  end

  def test_zstd_level_zero_is_refused_by_the_settings_catalogue
    result = invoke(arguments: ["c", "--compress=zstd", "-f", "zero.zst", "-F", "fat12",
      "-o", "zstd:level=0", "source"], expected: zstd_supported? ? 2 : 1)
    detail = zstd_supported? ? "zstd:level" : 'writing this compression is not supported "zstd"'
    assert_includes(result.stderr, detail.b)
    refute_path_exists(@root / "zero.zst")
  end

  def test_zstd_without_checksum_reports_incomplete_verification_in_json_and_text
    require_zstd_support
    compressor = ENV["ZSTD_PROGRAM"] || find_program("zstd")
    skip_zstd("unchecked CLI fixture requires zstd") unless compressor
    human_image
    (@root / "human.img").open("rb") do |input|
      result = run_process([compressor, "-c", "-3", "--no-check"], cwd: @root, stdin: input)
      assert_equal(0, result.status, result.stderr)
      (@root / "unchecked.zst").binwrite(result.stdout)
    end

    result = invoke(arguments: ["W", "--json", "-f", "unchecked.zst"])
    report = completed(result).find { _1["unverified"] == "zstd:checksum" }
    refute_nil(report, result.stdout)
    assert_equal(false, report.fetch("complete"))
    human = invoke(arguments: ["W", "-f", "unchecked.zst"]).stdout
    assert_includes(human, '"unchecked.zst": zstd compression (incomplete: zstd:checksum unverified), filesystem structure verified ('.b)

    bytes = (@root / "unchecked.zst").binread
    (@root / "damaged.zst").binwrite(bytes.byteslice(0, bytes.bytesize - 1))
    assert_zstd_corrupt(image: "damaged.zst")
  end

  def test_zstd_formats_reports_capabilities_and_library_version_together
    format = zstd_format
    assert_equal(["level"], format.fetch("settings"))
    assert_equal(".zst", format.fetch("suffix"))
    lines = invoke(arguments: ["--formats"]).stdout.lines
    index = lines.index { _1.match?(/\Azstd\s/) }
    refute_nil(index, lines.join)

    if zstd_supported?
      assert_equal(7, format.fetch("capabilities"))
      assert_match(/\Azstd \S+\z/, format.fetch("codec_version"))
      assert_includes(lines[index + 1], format.fetch("codec_version"))
    else
      assert_nil(format.fetch("codec_version"))
      assert_match(/\bzstd\s+compression\s+no\s+no\s+no\b/, lines[index])
      refute_match(/zstd/, lines[index + 1].to_s)
    end
  end

  private

  def require_zstd_support
    skip_zstd("zstd profile requires libzstd") unless zstd_supported?
  end

  def skip_zstd(reason)
    puts("SKIP: #{reason}")
    skip(reason)
  end

  def zstd_creation_arguments(image:, options: [])
    ["cW", *options, "-a", "-f", image, "-F", "fat12", "-s", "1M",
      "-o", "zstd:level=3", "-C", "source", "."]
  end

  def damage_zstd_checksum(source:, destination:)
    bytes = (@root / source).binread
    offset = bytes.bytesize - 1 # The last byte of the content checksum.
    bytes.setbyte(offset, bytes.getbyte(offset) ^ 1)
    (@root / destination).binwrite(bytes)
  end

  def assert_zstd_corrupt(image:)
    result = invoke(arguments: ["W", "-f", image], expected: 1)
    assert_includes(result.stderr, "compressed byte".b)
  end
end
