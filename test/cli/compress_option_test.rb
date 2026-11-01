# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_compress_requires_a_nonempty_name
    result = invoke(arguments: ["cn", "--compress=", "-F", "fat12", "source"], expected: 2)
    assert_includes(result.stderr, "--compress needs a compression name".b)
    refute_includes(result.stderr, "--compress names no registered compression".b)
    assert_empty(result.stdout)
  end

  def test_named_compression_suffix_is_checked_before_settings
    [["--compress=zstd", "x.img.gz", "gzip:level=9"],
      ["--compress=xz", "x.img.gz", "gzip:level=9"],
      ["-z", "x.img.zst", "zstd:level=9"]].each do |selection, image, setting|
      result = invoke(arguments: ["c", selection, "-a", "-f", image,
        "-o", setting, "-F", "fat12", "source"], expected: 2)
      assert_includes(result.stderr,
        "-a: the image name does not end in the named compression's suffix".b)
      assert_includes(result.stderr, "\"#{image}\"".b)
      refute_includes(result.stderr, "setting names a layer".b)
      assert_empty(result.stdout)
      refute_path_exists(@root / image)
    end
  end

  def test_invalid_compression_name_is_checked_before_suffix_and_settings
    result = invoke(arguments: ["c", "--compress=bogus", "-a", "-f", "x.img.gz",
      "-o", "gzip:level=9", "-F", "fat12", "source"], expected: 2)
    assert_includes(result.stderr, '--compress names no registered compression "bogus"'.b)
    refute_includes(result.stderr, "-a: the image name".b)
    assert_empty(result.stdout)
    refute_path_exists(@root / "x.img.gz")
  end

  def test_conflicting_compressions_are_checked_before_auto_suffix
    result = invoke(arguments: ["c", "-z", "--compress=xz", "-a", "-f", "x.img.zst",
      "-F", "fat12", "source"], expected: 2)
    assert_includes(result.stderr, "-z and --compress name different compressions".b)
    refute_includes(result.stderr, "-a: the image name".b)
    assert_empty(result.stdout)
    refute_path_exists(@root / "x.img.zst")
  end

  def test_empty_compression_name_is_checked_before_auto_suffix
    result = invoke(arguments: ["c", "--compress=", "-a", "-f", "x.img.gz",
      "-F", "fat12", "source"], expected: 2)
    assert_includes(result.stderr, "--compress needs a compression name".b)
    refute_includes(result.stderr, "-a: the image name".b)
    assert_empty(result.stdout)
    refute_path_exists(@root / "x.img.gz")
  end

  def test_named_compression_auto_suffix_requires_an_image_name
    ["gzip", "xz", "zstd"].each do |name|
      result = invoke(arguments: ["c", "--compress=#{name}", "-a", "-f", "-",
        "-F", "fat12", "source"], expected: 2)
      assert_includes(result.stderr, "-a needs an image name, not standard output".b)
      assert_empty(result.stdout)
    end
  end

  def test_empty_image_name_is_refused_before_auto_suffix
    result = invoke(arguments: ["c", "-z", "-a", "-f", "", "-F", "fat12", "source"], expected: 2)
    assert_includes(result.stderr, "-f requires a nonempty image name".b)
    assert_empty(result.stdout)
  end

  def test_unavailable_compression_refusals_name_the_compression
    human_image
    {"xz" => "xz", "zstd" => "zst"}.each do |name, suffix|
      next if compression_supported?(name: name)

      image = "x.img.#{suffix}"
      [["-a", "-f", image], ["--compress=#{name}", "-f", "x.img"]].each do |selection|
        result = invoke(arguments: ["c", *selection, "-F", "fat12", "source"], expected: 1)
        assert_includes(result.stderr.lines,
          "fsimg: writing this compression is not supported \"#{name}\"\n".b)
        assert_empty(result.stdout)
      end
      result = invoke(arguments: ["t", "--compress=#{name}", "-f", "human.img"], expected: 1)
      assert_includes(result.stderr.lines,
        "fsimg: reading this compression is not supported \"#{name}\"\n".b)
      assert_empty(result.stdout)
      refute_path_exists(@root / image)
      refute_path_exists(@root / "x.img")
    end
  end

  def test_compress_requires_a_registered_compression_name
    ["unknown", "fat12", "dmg"].each do |name|
      result = invoke(arguments: ["cn", "--compress=#{name}", "-F", "fat12", "source"], expected: 2)
      assert_includes(result.stderr, "--compress names no registered compression".b, name)
      assert_includes(result.stderr, "\"#{name}\"".b)
      assert_empty(result.stdout)
    end
  end

  def test_compress_gzip_agrees_with_z_and_conflicts_with_other_names
    invoke(arguments: ["c", "-z", "--compress=gzip", "-F", "fat12", "-f", "gzip.img", "source"])
    assert_equal("\x1f\x8b".b, (@root / "gzip.img").binread(2))
    ["xz", "zstd"].each do |name|
      result = invoke(arguments: ["cn", "-z", "--compress=#{name}", "-F", "fat12", "source"], expected: 2)
      assert_includes(result.stderr, "-z and --compress name different compressions".b)
    end
  end

  def test_compress_and_auto_suffix_must_agree
    [["-z"], ["--compress=xz"]].each do |selection|
      result = invoke(arguments: ["c", *selection, "-a", "-f", "x.img.zst", "-F", "fat12", "source"], expected: 2)
      assert_includes(result.stderr, "-a: the image name does not end in the named compression's suffix".b)
      assert_includes(result.stderr, '"x.img.zst"'.b)
      refute_path_exists(@root / "x.img.zst")
    end
    ["fat12", "fat12/dmg"].each do |stack|
      image = stack.include?("dmg") ? "x.dmg.zst" : "x.img.zst"
      result = invoke(arguments: ["c", "--compress=zstd", "-a", "-f", image,
        "-F", stack, "source"], expected: zstd_supported? ? 0 : 1)
      if zstd_supported?
        invoke(arguments: ["W", "-f", image])
      else
        assert_includes(result.stderr, 'writing this compression is not supported "zstd"'.b)
        refute_path_exists(@root / image)
      end
    end
  end

  def test_compress_stream_creation_and_auto_suffix_refusal
    result = invoke(arguments: ["c", "-a", "-f", "-", "-F", "fat12", "source"], expected: 2)
    assert_empty(result.stdout)
    assert_includes(result.stderr, "-a needs an image name, not standard output".b)

    ["gzip", "xz", "zstd"].each do |name|
      supported = compression_supported?(name: name)
      result = invoke(arguments: ["c", "--compress=#{name}", "-f", "-", "-F", "fat12", "source"],
        expected: supported ? 0 : 1)
      if supported
        (@root / "streamed").binwrite(result.stdout)
        invoke(arguments: ["W", "--compress=#{name}", "-f", "streamed"])
        wrong = name == "gzip" ? "xz" : "gzip"
        wrong_supported = compression_supported?(name: wrong)
        refused = invoke(arguments: ["t", "--compress=#{wrong}", "-f", "streamed"], expected: 1)
        assert_includes(refused.stderr, wrong_supported ? "no #{wrong} signature".b :
          "reading this compression is not supported \"#{wrong}\"".b)
      else
        assert_includes(result.stderr, "writing this compression is not supported \"#{name}\"".b)
        assert_empty(result.stdout)
      end
    end
  end

  def test_compress_forces_each_read_mode_and_reports_missing_decoder
    human_image
    invoke(arguments: ["c", "--compress=gzip", "-F", "fat12", "-f", "gzip.img", "source"])
    ["t", "x", "W", "i"].each do |mode|
      destination = "forced-#{mode}"
      options = mode == "x" ? ["-E", "-C", destination] : []
      private_directory(root: @root, name: destination) if mode == "x"
      invoke(arguments: [mode, "--compress=gzip", "-f", "gzip.img", *options])
      private_directory(root: @root, name: "wrong-#{mode}") if mode == "x"
      options = mode == "x" ? ["-E", "-C", "wrong-#{mode}"] : []
      refused = invoke(arguments: [mode, "--compress=gzip", "-f", "human.img", *options], expected: 1)
      assert_includes(refused.stderr, "no gzip signature".b)
      ["xz", "zstd"].each do |name|
        result = invoke(arguments: [mode, "--compress=#{name}", "-f", "human.img", *options], expected: 1)
        if !compression_supported?(name: name)
          assert_includes(result.stderr, "reading this compression is not supported \"#{name}\"".b)
          refute_includes(result.stderr, "invalid worker job description".b)
        else
          assert_includes(result.stderr, "no #{name} signature".b)
        end
      end
    end
  end
end
