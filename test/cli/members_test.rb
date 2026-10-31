# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_list_overlapping_and_duplicate_members_once
    members_image
    [
      %w[EFI efi/boot],
      %w[EFI EFI]
    ].each do |members|
      result = invoke(arguments: ["t", "-f", "fat.img", *members])
      assert_equal(quoted_member_listing(efi_member_paths), result.stdout,
        "Selection #{members.inspect} must list each EFI entry once")
    end
  end

  def test_list_nonrecursive_overlapping_members_excludes_deeper_descendants
    members_image
    result = invoke(arguments: %w[t -f fat.img --no-recursion EFI efi/boot])
    paths = ["/EFI", "/EFI/BOOT", "/EFI/BOOT/BOOTX64.EFI",
      "/EFI/BOOT/DEEP", "/EFI/BOOT/Long File Name.txt", "/EFI/README.TXT"]
    assert_equal(quoted_member_listing(paths), result.stdout)
  end

  def test_list_short_alias_and_long_name_once
    members_image
    result = invoke(arguments: ["t", "-f", "fat.img",
      "/EFI/BOOT/F0000000", "EFI/BOOT/Long File Name.txt"])
    assert_equal(quoted_member_listing(["/EFI/BOOT/Long File Name.txt"]), result.stdout)
  end

  def test_list_members_in_walk_order
    members_image
    result = invoke(arguments: %w[t -f fat.img docs/sub EFI/BOOT])
    paths = ["/EFI/BOOT", "/EFI/BOOT/BOOTX64.EFI",
      "/EFI/BOOT/DEEP", "/EFI/BOOT/DEEP/c.txt",
      "/EFI/BOOT/Long File Name.txt", "/docs/sub", "/docs/sub/b.txt"]
    assert_equal(quoted_member_listing(paths), result.stdout)
  end

  def test_list_missing_member_reports_incomplete_output_after_matches
    members_image
    result = invoke(arguments: %w[t -f fat.img EFI missing], expected: 1)
    assert_equal(quoted_member_listing(efi_member_paths), result.stdout)
    assert_includes(result.stderr.lines,
      "fsimg: selected image path was not found \"/missing\"\n".b)
    assert_includes(result.stderr.lines, "fsimg: output is incomplete\n".b)
  end

  def test_list_without_members_includes_all_entries_without_root
    members_image
    result = invoke(arguments: %w[t -f fat.img])
    paths = efi_member_paths + %w[/docs /docs/a.txt /docs/sub /docs/sub/b.txt]
    assert_equal(quoted_member_listing(paths), result.stdout)
  end

  def test_extract_overlapping_members_and_disjoint_file_with_exact_bytes
    files = members_image
    destination = private_directory(root: @root, name: "out1")
    invoke(arguments: %w[x -f fat.img -E -C out1 EFI efi/BOOT docs/a.txt])

    paths = %w[EFI EFI/BOOT EFI/BOOT/DEEP EFI/BOOT/DEEP/c.txt] +
      ["EFI/BOOT/BOOTX64.EFI", "EFI/BOOT/Long File Name.txt",
        "EFI/README.TXT", "docs", "docs/a.txt"]
    assert_equal(paths.sort, destination.glob("**/*").map { _1.relative_path_from(destination).to_s }.sort)
    files.reject { |path, _bytes| path == "docs/sub/b.txt" }.each do |path, bytes|
      assert_equal(bytes, (destination / path).binread, "Extracted #{path} must preserve its bytes")
    end
    %w[EFI EFI/BOOT EFI/BOOT/DEEP docs].each do |path|
      assert((destination / path).directory?, "Extracted #{path} must be a directory")
    end
  end

  def test_extract_overlapping_members_writes_each_manifest_entry_once
    members_image
    private_directory(root: @root, name: "out2")
    invoke(arguments: %w[x -f fat.img -E -C out2 --mtree=out2.mtree EFI efi/boot])

    paths = (@root / "out2.mtree").read.lines(chomp: true)
      .reject { _1.start_with?("#") }.map { _1.split.first }
    assert_equal([".", "./EFI", "./EFI/BOOT", "./EFI/BOOT/BOOTX64.EFI",
      "./EFI/BOOT/DEEP", "./EFI/BOOT/DEEP/c.txt",
      './EFI/BOOT/Long\040File\040Name.txt', "./EFI/README.TXT"], paths)
  end

  def test_extract_to_stdout_rejects_two_aliases_of_one_member
    members_image
    result = invoke(arguments: %w[x -f fat.img -O EFI/README.TXT efi/readme.txt], expected: 2)
    assert_empty(result.stdout)
    assert_includes(result.stderr.lines, "fsimg: -O requires exactly one member\n".b)
  end

  def test_extract_missing_member_reports_retained_incomplete_output
    files = members_image
    destination = private_directory(root: @root, name: "out3")
    result = invoke(arguments: %w[x -f fat.img -E -C out3 EFI nothing-here], expected: 1)
    assert_includes(result.stderr.lines,
      "fsimg: selected image path was not found \"/nothing-here\"\n".b)
    assert_includes(result.stderr.lines, "fsimg: output is incomplete\n".b)
    assert_includes(result.stderr.lines, "fsimg: retained output: \"out3\"\n".b)
    assert(destination.directory?, "Incomplete extraction must retain out3")
    files.select { |path, _bytes| path.start_with?("EFI/") }.each do |path, bytes|
      assert_equal(bytes, (destination / path).binread, "Retained #{path} must preserve its bytes")
    end
  end

  def test_json_listing_and_extraction_count_overlapping_members_once
    members_image
    listed = completed(invoke(arguments: %w[t -f fat.img --json EFI efi/boot]))
    entries = listed.select { _1.fetch("type") == "entry" }
    assert_equal(efi_member_paths, entries.map { _1.fetch("path") })
    assert_equal(7, entries.length)
    assert_equal("7", listed.last.fetch("entries_processed"))
    assert_equal("20", listed.last.fetch("logical_bytes"))

    private_directory(root: @root, name: "out4")
    extracted = completed(invoke(arguments: %w[x -f fat.img --json -E -C out4 EFI efi/boot]))
    assert_equal("7", extracted.last.fetch("entries_processed"))
    assert_equal(listed.last.fetch("entries_processed"), extracted.last.fetch("entries_processed"))
  end

  private

  def members_image
    files = {
      "EFI/BOOT/BOOTX64.EFI" => "boot".b,
      "EFI/BOOT/Long File Name.txt" => "long name".b,
      "EFI/BOOT/DEEP/c.txt" => "c".b,
      "EFI/README.TXT" => "readme".b,
      "docs/a.txt" => "a".b,
      "docs/sub/b.txt" => "b".b
    }
    files.each do |path, bytes|
      target = @root / "tree" / path
      FileUtils.mkdir_p(target.dirname)
      target.binwrite(bytes)
    end
    invoke(arguments: %w[cf fat.img -F fat32 -C tree .])
    files
  end

  def efi_member_paths = ["/EFI", "/EFI/BOOT", "/EFI/BOOT/BOOTX64.EFI",
    "/EFI/BOOT/DEEP", "/EFI/BOOT/DEEP/c.txt",
    "/EFI/BOOT/Long File Name.txt", "/EFI/README.TXT"]

  def quoted_member_listing(paths) = paths.map { "\"#{_1}\"\n" }.join.b
end
