# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  EXTRA_DATA_PATHS = %w[/dir /dir/clean /dir/inner /forked /plain /tagged].freeze
  EXTRA_DATA_MARKED_PATHS = %w[/dir/inner /forked /tagged].freeze
  EXTRA_DATA_SINGULAR_NOTICE =
    "fsimg: note: 1 entry carries extra data not listed; t -v marks it\n".freeze
  EXTRA_DATA_PLURAL_NOTICE =
    "fsimg: note: 3 entries carry extra data not listed; t -v marks them\n".freeze

  def test_extra_data_verbose_listing_marks_only_entries_with_forks_or_attributes
    extra_data_image
    result = invoke(arguments: %w[t -v -f marks.img])
    lines = result.stdout.lines

    assert_equal(EXTRA_DATA_PATHS.size, lines.size)
    EXTRA_DATA_PATHS.zip(lines).each do |path, line|
      marked = EXTRA_DATA_MARKED_PATHS.include?(path)
      mode = path == "/dir" ? "drwxr-xr-x" : "-rw-r--r--"
      size = path == "/dir" ? "           -" : "           5"
      # Stored zero dates denote the HFS epoch, 1904-01-01 UTC.
      expected = "#{mode}#{marked ? '@' : ' '}#{size} 1904-01-01 00:00:00 \"#{path}\"\n"
      assert_equal(expected, line, path)
      assert_equal(marked ? "@" : " ", line.byteslice(10, 1), path)
    end
    assert_empty(extra_data_stderr(result))
  end

  def test_extra_data_plain_listing_preserves_names_and_reports_three_marked_entries
    extra_data_image
    result = invoke(arguments: %w[t -f marks.img])

    assert_equal(extra_data_names(EXTRA_DATA_PATHS), result.stdout)
    refute_includes(result.stdout, "@")
    assert_match(/\Afsimg: note: \d+ entr(y|ies) carr(y|ies)/, extra_data_stderr(result))
    assert_equal(EXTRA_DATA_PLURAL_NOTICE, extra_data_stderr(result))
  end

  def test_extra_data_member_listing_reports_one_marked_file
    extra_data_image
    result = invoke(arguments: %w[t -f marks.img /forked])

    assert_equal("\"/forked\"\n", result.stdout)
    assert_equal(EXTRA_DATA_SINGULAR_NOTICE, extra_data_stderr(result))
  end

  def test_extra_data_member_listing_of_plain_file_has_no_notice
    extra_data_image
    result = invoke(arguments: %w[t -f marks.img /plain])

    assert_equal("\"/plain\"\n", result.stdout)
    assert_empty(extra_data_stderr(result))
  end

  def test_extra_data_directory_listing_counts_only_its_marked_descendant
    extra_data_image
    result = invoke(arguments: %w[t -f marks.img /dir])

    assert_equal(extra_data_names(%w[/dir /dir/clean /dir/inner]), result.stdout)
    assert_equal(EXTRA_DATA_SINGULAR_NOTICE, extra_data_stderr(result))
  end

  def test_extra_data_deep_member_listing_includes_ancestor_without_counting_it
    extra_data_image
    result = invoke(arguments: %w[t -f marks.img /dir/inner])

    assert_equal(extra_data_names(%w[/dir /dir/inner]), result.stdout)
    assert_equal(EXTRA_DATA_SINGULAR_NOTICE, extra_data_stderr(result))
  end

  def test_extra_data_double_verbose_listing_has_no_notice
    extra_data_image
    result = invoke(arguments: %w[t -vv -f marks.img])

    refute_empty(result.stdout)
    refute_includes(extra_data_stderr(result), "extra data not listed")
  end

  def test_extra_data_json_listing_reports_marks_without_human_notice
    extra_data_image
    [[], ["-v"]].each do |verbosity|
      result = invoke(arguments: ["t", *verbosity, "--json", "-f", "marks.img"])
      entries = completed(result).select { _1.fetch("type") == "entry" }

      assert_empty(extra_data_stderr(result))
      assert_equal(EXTRA_DATA_PATHS, entries.map { _1.fetch("path") })
      entries.each do |entry|
        if EXTRA_DATA_MARKED_PATHS.include?(entry.fetch("path"))
          assert_equal(true, entry.fetch("extra_data"), entry.fetch("path"))
        else
          assert_nil(entry.fetch("extra_data"), entry.fetch("path"))
        end
      end
    end
  end

  def test_extra_data_fat_listing_retains_unmarked_columns_and_has_no_notice
    human_image
    verbose = invoke(arguments: %w[t -v -f human.img /HELLO.TXT])
    plain = invoke(arguments: %w[t -f human.img /HELLO.TXT])

    # The FAT fixture has six known bytes and the reproducible 1980 epoch;
    # FAT does not store POSIX modes.
    assert_equal("-?????????            6 1980-01-01 00:00:00 \"/HELLO.TXT\"\n", verbose.stdout)
    assert_equal(" ", verbose.stdout.byteslice(10, 1))
    assert_empty(extra_data_stderr(verbose))
    assert_equal("\"/HELLO.TXT\"\n", plain.stdout)
    assert_empty(extra_data_stderr(plain))
  end

  private

  # Match image diagnostics exactly after the optional host sandbox warning.
  def extra_data_stderr(result) =
    result.stderr.sub(/\Afsimg: warning: OS sandbox unavailable; continuing without confinement\n/n, "".b)

  def extra_data_image
    helper = @executable.dirname / "hfsplus-marks"
    result = run_process([helper, @root / "marks.img"], cwd: @root)
    assert_equal(0, result.status, result.stderr)
    assert_empty(result.stdout)
    assert_empty(result.stderr)
  end

  def extra_data_names(paths) = paths.map { "\"#{_1}\"\n" }.join
end
