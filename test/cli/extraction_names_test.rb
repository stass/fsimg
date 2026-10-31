# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_extraction_names_strip_suffixes_and_preserve_existing_objects
    human_image
    root = @root
    # x names its fresh directory after the image, removing one compression,
    # one container and one image suffix in that order, ASCII
    # case-insensitively. An existing name of any kind fails untouched; an
    # image with no such suffix needs -E.
    images = (root / "stems")
    parent = (root / "stem-parent")
    images.mkdir
    parent.mkdir
    stems = {
      "boot.IMG.GZ" => "boot",
      "disk.DMG" => "disk",
      "x.vhd.gz" => "x",
      "y.RAW" => "y",
      "a.b.img" => "a.b",
      "q.simg" => "q",
      "rootfs.vhd.img" => "rootfs.vhd",
      "s.bin" => "s",
      "t.dd" => "t",
      "u.img.img" => "u.img",
      "v.gz" => "v",
      "w.dmg.img.gz" => "w.dmg"
    }
    stems.to_a.each_with_index do |(name, stem), index|
      File.link((root / "human.img"), (images / name))
      existing = (parent / stem)
      if (index % 3) == 0
        existing.binwrite("keep".b)
      else
        if (index % 3) == 1
          existing.mkdir
        else
          File.symlink("absent", existing)
        end
      end

      result = invoke(arguments: ["xf", "stems/#{name}", "-C", "stem-parent"], expected: 1)
      expected = "cannot create extraction directory \"stem-parent/#{stem}\": File exists"
      assert_includes(result.stderr, expected.encode(Encoding::UTF_8).b, [name, result.stderr])
    end

    assert_equal(stems.values.sort, Dir.children(parent).sort)
    stems.values.each_with_index do |stem, index|
      existing = (parent / stem)
      if (index % 3) == 0
        assert_equal("keep".b, existing.binread)
      else
        if (index % 3) == 1
          assert_empty(existing.children.to_a, "existing children to a empty")
        else
          assert_equal("absent", File.readlink(existing))
        end
      end
    end

    ["noext", ".img", "..img"].each do |name|
      File.link((root / "human.img"), (images / name))
      result = invoke(arguments: ["xf", "stems/#{name}", "-C", "stem-parent"], expected: 2)
      expected = "cannot name the extraction directory after the image; use -E \"stems/#{name}\""
      assert_includes(result.stderr, expected.encode(Encoding::UTF_8).b, [name, result.stderr])
    end

    assert_equal(stems.values.sort, Dir.children(parent).sort)
  end
end
