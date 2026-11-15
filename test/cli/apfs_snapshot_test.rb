# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_apfs_missing_snapshot_reports_invalid_argument
    invoke(arguments: ["c", "-F", "apfs", "-f", "snapshots.img", "-C", "source", "."])
    listed = invoke(arguments: ["t", "-f", "snapshots.img"])
    assert_includes(listed.stdout, "HELLO.TXT".b)

    missing = invoke(arguments: ["t", "-o", "snapshot=missing", "-f", "snapshots.img"], expected: 1)
    assert_empty(missing.stdout)
    assert_includes(missing.stderr, "has no snapshot of that name".b)
    error = failed(invoke(arguments: ["t", "--json", "-o", "snapshot=missing", "-f", "snapshots.img"], expected: 1))
    assert_equal("invalid argument", error.fetch("status"))
  end
end
