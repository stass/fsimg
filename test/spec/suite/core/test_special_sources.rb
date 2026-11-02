# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest"
require "fsimg_spec"

class TestSpecialSources < Minitest::Test
  def test_link_target_is_immutable_bytes_with_derived_size_and_digest
    target = "../caf\xff".b
    entry = FsimgSpec::TreeEntry.symlink("link", target:)
    target.setbyte(0, 0)
    assert_equal "../caf\xff".b, entry.target
    assert entry.target.frozen?
    observed = FsimgSpec::KnownTree.of([entry]).first
    assert_equal "symlink", observed.type
    assert_equal entry.target.bytesize, observed.size
    assert_equal Digest::SHA256.hexdigest(entry.target), observed.sha256
  end

  def test_special_kind_rejects_regular_file_content
    assert_raises(FsimgSpec::RecipeError) do
      FsimgSpec::TreeEntry.new(path: "link", type: "symlink", target: "file", size: 4)
    end
    assert_raises(FsimgSpec::RecipeError) do
      FsimgSpec::TreeEntry.new(path: "fifo", type: "fifo", content: "zero", size: 0)
    end
    assert_raises(FsimgSpec::RecipeError) { FsimgSpec::TreeEntry.symlink("link", target: "\0".b) }
  end

  def test_directory_source_does_not_silently_become_manifest_source
    link = FsimgSpec::TreeEntry.symlink("link", target: "file")
    assert_raises(FsimgSpec::RecipeError) { FsimgSpec::Source.new(tree: [link], settings: {}, root: "directory") }
  end

  def test_manifest_escapes_link_target_and_has_no_fifo_content
    tree = [FsimgSpec::TreeEntry.symlink("link", target: "a b#\\\xff".b), FsimgSpec::TreeEntry.fifo("pipe")]
    lines = FsimgSpec::Manifest.lines(tree)
    assert_equal "./link type=link link=a\\040b\\043\\\\\\377\n".b, lines.first
    assert_equal "./pipe type=fifo\n".b, lines.last
  end

  def test_recipe_roundtrip_preserves_binary_target_and_path_settings
    source = FsimgSpec::Source.new(tree: [FsimgSpec::TreeEntry.symlink("link", target: "\xff/file".b)],
                                  settings: { level: 3, bootload: 4, boot: "/a,b=c", efiboot: "/EFI" })
    recipe = FsimgSpec::Recipe.new(id: "iso9660.create.special", format: "iso9660", kind: "valid",
                                  source:, stages: ["create"], expected: {"create" => FsimgSpec::Expectation.parse("accept")})
    loaded = FsimgSpec::Recipe.load(recipe.dump, ceilings: FsimgSpec.fetch_profile("quick").ceilings)
    assert_equal recipe.dump, loaded.dump
    assert_equal "\xff/file".b, loaded.source.tree.first.target
    assert_equal "/a,b=c", loaded.source.settings.fetch(:boot)
  end

  def test_boot_setting_obeys_the_cli_text_capacity
    source = FsimgSpec::Source.new(tree: [], settings: {boot: "/" + "a" * 1023})
    assert_equal 1024, source.settings.fetch(:boot).bytesize
    assert_raises(FsimgSpec::RecipeError) do
      FsimgSpec::Source.new(tree: [], settings: {boot: "/" + "a" * 1024})
    end
    assert_raises(FsimgSpec::RecipeError) do
      FsimgSpec::Source.new(tree: [], settings: {efiboot: "/" + "a" * 1024})
    end
  end
end
