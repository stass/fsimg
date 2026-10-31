# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module CliRecords
  private

  LONG_LISTING = Regexp
    .new(
      "\\A([-dlp?])([-rwx?]{9}) ([ 0-9-]{12}) " \
        "(\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d|- {18}) (\".*\")([@|]?)\\z"
    )
    .freeze

  def records(result)
    decoded = result.stdout.lines(chomp: true).map { |line| JSON.parse(line) }
    refute_empty(decoded, "CLI must emit JSON records")
    decoded.each_with_index do |record, index|
      assert_equal(1, record.fetch("schema"), "JSON record #{index} schema")
    end
    decoded
  end

  def completed(result)
    decoded = records(result)
    assert_equal("summary", decoded[-1].fetch("type"), "successful command must end with a summary")
    assert_equal(true, decoded[-1].fetch("complete"), "successful command summary must be complete")
    refute(decoded.any? { |record| record.fetch("type") == "error" }, "successful command must emit no error record")
    decoded
  end

  def failed(result)
    decoded = records(result)
    assert_equal("error", decoded[-1].fetch("type"), "failed command must end with an error")
    assert_equal(false, decoded[-1].fetch("complete"), "failed command error must be incomplete")
    refute(decoded.any? { |record| record.fetch("type") == "summary" }, "failed command must emit no summary")
    decoded[-1]
  end

  def long_listing(result)
    # Map each quoted name to its (kind, permissions, size, time) columns.
    rows = {}
    result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true).each do |line|
      match = LONG_LISTING.match(line)
      assert(match, line)
      kind, permissions, size, modified, name, marker = match.captures
      assert(/\A *(\d+|-)\z/.match?(size), line)
      assert_equal({"l" => "@", "p" => "|"}.fetch(kind, ""), marker, line)
      rows[name] = [kind, permissions, size.strip, modified.strip]
    end

    rows
  end

  def filesystems(decoded)
    # The --formats JSON records of the filesystem formats, by name.
    decoded
      .select { |record| (record.fetch("type") == "format" && record.fetch("kind") == "filesystem") }
      .map { |record| [record.fetch("name"), record] }
      .to_h
  end

end
