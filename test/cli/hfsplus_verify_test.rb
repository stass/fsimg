# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class HfsplusVerifyTest < Minitest::Test
  include CliProcesses
  include CliRecords

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE")).realpath
    @root = Pathname.new(Dir.mktmpdir("hfsplus-verify-", Dir.pwd))
  end

  def teardown
    FileUtils.remove_entry(@root) if @root
  end

  def test_structural_verification_reports_exact_counts
    report = verify_fixture(name: "clean", payload: false)
    assert_equal("structural", report.fetch("validation_scope"))
    assert_counts(report)
  end

  def test_payload_verification_reports_exact_counts
    report = verify_fixture(name: "clean", payload: true)
    assert_equal("initialized-payload", report.fetch("validation_scope"))
    assert_counts(report)
    assert_equal((9 * 4096 + 3).to_s, report.fetch("initialized_bytes_read"))
  end

  def test_corrupt_alternate_header_reports_corruption
    error = failed(invoke(root: @root, arguments: ["W", "--json", "-f", fixture("corrupt")], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "alternate header")
  end

  def test_refused_label_reports_unsupported
    error = failed(invoke(root: @root, arguments: ["W", "--json", "-f", fixture("refused")], expected: 1))
    assert_equal(-6, error.fetch("code"))
    assert_includes(error.fetch("message"), "label")
  end

  private

  def fixture(name)
    path = @root / "#{name}.img"
    invoke(executable: @executable.dirname / "test-hfsplus-verify", root: @root,
      arguments: ["--write-fixture", name, path], expected: 0)
    path
  end

  def verify_fixture(name:, payload:)
    arguments = ["W", "--json", "-f", fixture(name)]
    arguments << "-d" if payload
    completed(invoke(root: @root, arguments: arguments, expected: 0))
      .find { _1.fetch("type") == "verify" }
  end

  def assert_counts(report)
    assert_equal("2", report.fetch("files"))
    assert_equal("2", report.fetch("directories"))
    assert_equal((9 * 4096 + 3).to_s, report.fetch("logical_bytes"))
    assert_equal("2", report.fetch("bad_clusters"))
    assert_equal(512, %w[allocated_clusters free_clusters bad_clusters].sum { report.fetch(_1).to_i })
  end
end
