# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require_relative "apfs-integration"

class ApfsIntegrationTest < Minitest::Test
  VOLUME_UUID = "e581cdef-bfb0-4b30-ba08-fd2f3f8a886d"
  SECOND_UUID = "11111111-2222-3333-4444-555555555555"
  ERROR_RECORD = { "type" => "error", "status" => "corrupt image", "code" => -5,
                   "message" => "bad tree" }.freeze
  SUMMARY_RECORD = { "type" => "summary", "complete" => true }.freeze

  FSAPFSINFO = <<~TEXT.freeze
    fsapfsinfo 20261030

    Apple File System (APFS) information:

    Container information:
    \tIdentifier\t\t\t: ccf14b5c-899a-4441-8339-41afb0246849
    \tNumber of volumes\t\t: 1

    Volume: 1 information:
    \tIdentifier\t\t\t: e581cdef-bfb0-4b30-ba08-fd2f3f8a886d
    \tName\t\t\t\t: FSIMG
    \tCompatible features\t\t: 0x00000002
    \t\t(APFS_FEATURE_HARDLINK_MAP_RECORDS)

    \tIncompatible features\t\t: 0x00000001
    \t\t(APFS_INCOMPAT_CASE_INSENSITIVE)

    \tRead-only compatible features\t: 0x00000000

    \tNumber of snapshots\t\t: 0

  TEXT

  DUMP = <<~TEXT.freeze
    0024 u32 block_size       : 00001000
    0028 u64 block_count      : 0000000000004000
    0030 u64   block_count        : 0000000000004000
    0060 u64   block_count        : 0000000000000000
    num_files        : 0000000000000009
    num_directories  : 0000000000000019
    num_symlinks     : 0000000000000000
    num_other_fsobjs : 0000000000000000
    num_snapshots    : 0000000000000000
    vol_uuid         : E581CDEF-BFB0-4B30-BA08-FD2F3F8A886D
    volname          : FSIMG
  TEXT

  def test_fsapfsinfo_parses_tabbed_volume_without_container_identifier
    expected = facts(block_size: nil, blocks: nil,
                     volumes: [volume(counts: nil)])

    assert_equal expected, ApfsIntegration.fsapfsinfo_facts(FSAPFSINFO)
  end

  def test_fsapfsinfo_preserves_two_volume_order_and_snapshot_counts
    second = FSAPFSINFO.split("Volume: 1 information:").last
                       .sub(VOLUME_UUID, SECOND_UUID).sub("FSIMG", "Archive")
                       .sub("Number of snapshots\t\t: 0", "Number of snapshots\t\t: 3")
    text = FSAPFSINFO.sub("Number of volumes\t\t: 1", "Number of volumes\t\t: 2") +
           "Volume: 2 information:\n#{second}"
    expected = facts(block_size: nil, blocks: nil, volumes: [
      volume(counts: nil),
      volume(uuid: SECOND_UUID, name: "Archive", counts: nil, snapshots: 3)
    ])

    assert_equal expected, ApfsIntegration.fsapfsinfo_facts(text)
  end

  def test_dump_parses_hex_geometry_counts_and_lowercase_uuid
    assert_equal facts, ApfsIntegration.dump_facts(DUMP)
  end

  def test_dump_ignores_later_indented_block_counts
    text = DUMP + "0030 u64   block_count        : 0000000000000020\n" +
           "0060 u64   block_count        : 0000000000000000\n"

    assert_equal 16_384, ApfsIntegration.dump_facts(text).blocks
  end

  def test_dump_preserves_volume_order_and_distinct_counters
    text = DUMP + <<~TEXT
      num_files        : 000000000000000a
      num_directories  : 000000000000000b
      num_symlinks     : 000000000000000c
      num_other_fsobjs : 000000000000000d
      num_snapshots    : 000000000000000e
      vol_uuid         : #{SECOND_UUID}
      volname          : Archive
    TEXT
    expected = facts(volumes: [
      volume,
      volume(uuid: SECOND_UUID, name: "Archive", counts: [10, 11, 12, 13], snapshots: 14)
    ])

    assert_equal expected, ApfsIntegration.dump_facts(text)
  end

  def test_differences_is_empty_for_matching_facts
    assert_empty ApfsIntegration.differences(facts, facts)
  end

  def test_differences_reports_each_non_nil_mismatch
    oracle = facts(block_size: 8192, blocks: 32_768, volumes: [
      volume(name: "Archive", counts: [1, 2, 3, 4], snapshots: 5)
    ])
    expected = [
      "block_size: oracle 8192, fsimg 4096",
      "blocks: oracle 32768, fsimg 16384",
      "#{VOLUME_UUID} name: oracle \"Archive\", fsimg \"FSIMG\"",
      "#{VOLUME_UUID} counts: oracle [1, 2, 3, 4], fsimg [9, 25, 0, 0]",
      "#{VOLUME_UUID} snapshots: oracle 5, fsimg 0"
    ]

    assert_equal expected, ApfsIntegration.differences(facts, oracle)
  end

  def test_differences_ignores_nil_oracle_fields
    oracle = facts(block_size: nil, blocks: nil, volumes: [
      volume(name: nil, counts: nil, snapshots: nil)
    ])

    assert_empty ApfsIntegration.differences(facts, oracle)
  end

  def test_differences_matches_volumes_by_uuid_regardless_of_order
    second = volume(uuid: SECOND_UUID, name: "Archive")
    fsimg = facts(volumes: [volume, second])
    oracle = facts(volumes: [second, volume])

    assert_empty ApfsIntegration.differences(fsimg, oracle)
  end

  def test_differences_reports_one_volume_difference_for_different_uuid_sets
    oracle = facts(volumes: [volume(uuid: SECOND_UUID, name: "Archive")])
    expected = ["volumes: oracle [\"#{SECOND_UUID}\"], fsimg [\"#{VOLUME_UUID}\"]"]

    assert_equal expected, ApfsIntegration.differences(facts, oracle)
  end

  def test_matrix_rows_reads_observed_cells_and_ignores_other_tables
    text = <<~TEXT
      # MATRIX
      | Specimen | i exp | t exp | x exp | W exp | i obs | t obs | x obs | W obs |
      |---|---|---|---|---|---|---|---|---|
      | plain | expected i | expected t | expected x | expected W | ok | refused (bad tree) | no file | refused (checksum) |
      | short | ok | ok |
      | wide | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
    TEXT
    expected = { "plain" => {
      "i" => "ok", "t" => "refused (bad tree)",
      "x" => "no file", "W" => "refused (checksum)"
    } }

    assert_equal expected, ApfsIntegration.matrix_rows(text)
  end

  def test_matrix_rows_preserves_empty_observed_cells
    text = "| pending | ok | ok | ok | ok | | ok | | |\n"
    expected = { "pending" => { "i" => "", "t" => "ok", "x" => "", "W" => "" } }

    assert_equal expected, ApfsIntegration.matrix_rows(text)
  end

  def test_matrix_rows_preserves_empty_final_cell_without_padding
    text = "|pending|ok|ok|ok|ok|ok|ok|ok||\n"
    expected = { "pending" => { "i" => "ok", "t" => "ok", "x" => "ok", "W" => "" } }

    assert_equal expected, ApfsIntegration.matrix_rows(text)
  end

  def test_verdict_formats_success_as_ok
    verdict = ApfsIntegration::Verdict.new(outcome: :accepted, reason: "")

    assert_equal "ok", verdict.to_s
  end

  def test_verdict_formats_refusal_with_reason
    verdict = ApfsIntegration::Verdict.new(outcome: :refused, reason: "invalid block count")

    assert_equal "refused (invalid block count)", verdict.to_s
  end

  def test_fsimg_timeout_has_no_verdict
    assert_fsimg_has_no_verdict(:timeout)
  end

  def test_fsimg_signal_death_has_no_verdict
    assert_fsimg_has_no_verdict(:signal)
  end

  def test_fsimg_failure_to_start_has_no_verdict
    assert_fsimg_has_no_verdict(:start)
  end

  def test_checker_timeout_has_no_verdict
    assert_checker_has_no_verdict(:timeout)
  end

  def test_checker_signal_death_has_no_verdict
    assert_checker_has_no_verdict(:signal)
  end

  def test_checker_failure_to_start_has_no_verdict
    assert_checker_has_no_verdict(:start)
  end

  def test_extraction_timeout_has_no_verdict
    assert_extraction_has_no_verdict(:timeout)
  end

  def test_extraction_signal_death_has_no_verdict
    assert_extraction_has_no_verdict(:signal)
  end

  def test_extraction_failure_to_start_has_no_verdict
    assert_extraction_has_no_verdict(:start)
  end

  def test_error_record_before_process_death_is_not_a_refusal
    output = "#{JSON.generate(ERROR_RECORD)}\n"
    stubbed(ApfsIntegration, :run, answer: [output, nil]) do
      assert_predicate ApfsIntegration.command_verdict("fsimg", "W"), :no_verdict?
    end
  end

  def test_docker_checker_output_before_process_death_has_no_verdict
    environment = ->(name) { name == "FSIMG_APFS_DOCKER" ? "1" : nil }
    stubbed(ENV, :[], answer: environment) do
      stubbed(ApfsIntegration, :find_oracle, answer: nil) do
        stubbed(ApfsIntegration, :on_path?, answer: true) do
          stubbed(ApfsIntegration, :run, answer: ["image: FAIL\nbad tree\n", nil]) do
            checker = ApfsIntegration.apfsck_verdicts({ "capture" => "image" }).fetch("capture")
            assert_predicate checker, :no_verdict?
          end
        end
      end
    end
  end

  def test_verification_does_not_hide_missing_verdict_in_another_volume
    runs = [
      ["#{JSON.generate(ERROR_RECORD)}\n", 1],
      ["timeout", nil],
      ["{\"type\":\"summary\",\"complete\":true}\n", 0]
    ]
    runner = ->(*) { runs.shift }
    records = [[[], nil], [["volume=1"], nil], [["volume=2"], nil]]

    stubbed(ApfsIntegration, :run, answer: runner) do
      assert_predicate ApfsIntegration.fsimg_verify("fsimg", "image", records), :no_verdict?
    end
  end

  def test_two_missing_verdicts_never_agree
    missing = ApfsIntegration::Verdict.new(outcome: :no_verdict, reason: "timeout")

    assert_equal :fail, ApfsIntegration.verdict_result("checker", "image", missing, missing).verdict
  end

  def test_partial_info_before_process_death_is_not_a_refusal
    output = "#{JSON.generate(ERROR_RECORD)}\n"
    stubbed(ApfsIntegration, :run, answer: [output, nil]) do
      assert_raises(ApfsCapture::Error) { ApfsIntegration.volume_records("fsimg", "image") }
    end
  end

  def test_completed_verification_preserves_acceptance_and_refusal
    cases = [
      { output: "#{JSON.generate(SUMMARY_RECORD)}\n", status: 0, outcome: :accepted },
      { output: "#{JSON.generate(ERROR_RECORD)}\n", status: 1, outcome: :refused }
    ]
    cases.each do |scenario|
      scenario => { output:, status:, outcome: }
      checker = ApfsIntegration::Verdict.new(outcome: outcome, reason: "independent judgment")
      opposite = outcome == :accepted ? :refused : :accepted
      disagreeing = ApfsIntegration::Verdict.new(outcome: opposite, reason: "opposite judgment")
      stubbed(ApfsIntegration, :run, answer: [output, status]) do
        verify = ApfsIntegration.fsimg_verify("fsimg", "image", [[[], nil]])
        assert_equal outcome, verify.outcome
        expected = outcome == :accepted ? :pass : :skip
        assert_equal expected, ApfsIntegration.verdict_result("checker", "image", checker, verify).verdict
        assert_equal :fail, ApfsIntegration.verdict_result("checker", "image", disagreeing, verify).verdict
      end
    end
  end

  def test_corpus_missing_verdict_fails_even_when_recorded_as_matching
    missing = ApfsIntegration::Verdict.new(outcome: :no_verdict, reason: "timeout")

    assert_equal :fail, corpus_comparison(verdict: missing, observed: missing.to_s)
  end

  def test_corpus_missing_verdict_fails_before_unrecorded_result
    missing = ApfsIntegration::Verdict.new(outcome: :no_verdict, reason: "timeout")

    assert_equal :fail, corpus_comparison(verdict: missing, observed: "")
  end

  def test_corpus_compares_completed_verdicts_with_recorded_text
    accepted = ApfsIntegration::Verdict.new(outcome: :accepted, reason: "")

    assert_equal :pass, corpus_comparison(verdict: accepted, observed: "ok")
  end

  def test_verification_exit_two_has_no_verdict
    stubbed(ApfsIntegration, :run, answer: ["", 2]) do
      verify = ApfsIntegration.fsimg_verify("fsimg", "image", [[[], nil]])
      assert_predicate verify, :no_verdict?
      refute_equal ":", verify.reason
    end
  end

  def test_verification_without_summary_has_no_verdict
    stubbed(ApfsIntegration, :run, answer: ["", 0]) do
      assert_predicate ApfsIntegration.fsimg_verify("fsimg", "image", [[[], nil]]), :no_verdict?
    end
  end

  def test_fsck_nonzero_without_findings_has_no_verdict
    assert_fsck_has_no_verdict(output: "** Checking the container superblock.\n", status: 8)
  end

  def test_fsck_zero_without_ok_line_has_no_verdict
    assert_fsck_has_no_verdict(output: "** Checking the container superblock.\n", status: 0)
  end

  def test_apfsck_nonzero_without_findings_has_no_verdict
    stubbed(ApfsIntegration, :run, answer: ["", 1]) do
      assert_predicate ApfsIntegration.apfsck_direct("apfsck", "image"), :no_verdict?
    end
  end

  def test_docker_checker_error_line_has_no_verdict
    environment = ->(name) { name == "FSIMG_APFS_DOCKER" ? "1" : nil }
    stubbed(ENV, :[], answer: environment) do
      stubbed(ApfsIntegration, :find_oracle, answer: nil) do
        stubbed(ApfsIntegration, :on_path?, answer: true) do
          stubbed(ApfsIntegration, :run, answer: ["image: error\nkilled\n", 1]) do
            checker = ApfsIntegration.apfsck_verdicts({ "capture" => "image" }).fetch("capture")
            assert_predicate checker, :no_verdict?
            assert_equal "killed", checker.reason
          end
        end
      end
    end
  end

  def test_agrees_requires_both_tools_to_accept
    expected = {
      accepted: { accepted: true, refused: false, no_verdict: false },
      refused: { accepted: false, refused: false, no_verdict: false },
      no_verdict: { accepted: false, refused: false, no_verdict: false }
    }
    expected.each do |left, combinations|
      combinations.each do |right, agreement|
        first = ApfsIntegration::Verdict.new(outcome: left, reason: "first reason")
        second = ApfsIntegration::Verdict.new(outcome: right, reason: "different reason")
        assert_equal agreement, first.agrees?(second), "#{left} beside #{right}"
      end
    end
  end

  def test_apfsck_abnormal_exit_is_not_a_refusal_even_with_output
    [126, 127, 139].each do |status|
      stubbed(ApfsIntegration, :run, answer: ["Segmentation fault", status]) do
        assert_predicate ApfsIntegration.apfsck_direct("apfsck", "image"), :no_verdict?
      end
    end
  end

  def test_fsck_accepts_only_a_successful_ok_report
    output = "** The container /dev/rdisk9 appears to be OK.\n"
    assert_equal :pass, fsck_comparison(output: output, status: 0, expected: :accepted).verdict
    assert_fsck_has_no_verdict(output: output, status: 8)
  end

  def test_fsck_refuses_findings_and_recognised_corruption
    ["error: bad tree\n", "** The container could not be verified completely.\n"].each do |output|
      assert_equal :skip, fsck_comparison(output: output, status: 8, expected: :refused).verdict
    end
  end

  def test_verdict_rejects_unknown_outcomes
    assert_raises(ArgumentError) { ApfsIntegration::Verdict.new(outcome: :unknown, reason: "") }
  end

  def test_both_refused_skips_with_both_reasons
    checker = ApfsIntegration::Verdict.new(outcome: :refused, reason: "bad checksum")
    verify = ApfsIntegration::Verdict.new(outcome: :refused, reason: "corrupt image")

    comparison = ApfsIntegration.verdict_result("fsck_apfs", "image", checker, verify)
    assert_equal :skip, comparison.verdict
    assert_equal "both refused: fsimg corrupt image; fsck_apfs bad checksum", comparison.detail
    assert_equal :skip, ApfsIntegration.apfsck_result("image", checker, verify).verdict
  end

  def test_apfsck_named_acceptance_skips_and_other_acceptance_fails
    accepted = ApfsIntegration::Verdict.new(outcome: :accepted, reason: "")
    refused = ApfsIntegration::Verdict.new(outcome: :refused, reason: "APFS container has an invalid block count")

    assert_equal :skip, ApfsIntegration.apfsck_result("apfs-newfs-372736-plain", accepted, refused).verdict
    assert_equal :fail, ApfsIntegration.apfsck_result("apfs-newfs-4194304-plain", accepted, refused).verdict
  end

  def test_apfsck_missing_verdict_cannot_use_named_skip_rules
    missing = ApfsIntegration::Verdict.new(outcome: :no_verdict, reason: "timeout")
    accepted = ApfsIntegration::Verdict.new(outcome: :accepted, reason: "")
    refused = ApfsIntegration::Verdict.new(outcome: :refused, reason: "invalid block count")
    name = "apfs-newfs-268435456-e3-ci"

    assert_equal :fail, ApfsIntegration.apfsck_result(name, missing, accepted).verdict
    assert_equal :fail, ApfsIntegration.apfsck_result("apfs-newfs-372736-plain", missing, refused).verdict
    checker = ApfsIntegration::Verdict.new(outcome: :refused, reason: "name is too long")
    assert_equal :fail, ApfsIntegration.apfsck_result(name, checker, missing).verdict
  end

  def test_runtime_error_records_fail_against_accepting_checker
    errors = [
      { record: ERROR_RECORD.merge("status" => "I/O failure", "code" => -3), status: 1 },
      { record: ERROR_RECORD.merge("status" => "invalid argument", "code" => -1), status: 2 }
    ]
    checker = ApfsIntegration::Verdict.new(outcome: :accepted, reason: "")
    errors.each do |scenario|
      scenario => { record:, status: }
      stubbed(ApfsIntegration, :run, answer: ["#{JSON.generate(record)}\n", status]) do
        verify = ApfsIntegration.command_verdict("fsimg", "W")
        assert_predicate verify, :refused?
        assert_equal :fail, ApfsIntegration.verdict_result("checker", "image", checker, verify).verdict
      end
    end
  end

  def test_accepted_and_refused_volumes_combine_to_refused
    runs = [["#{JSON.generate(SUMMARY_RECORD)}\n", 0], ["#{JSON.generate(ERROR_RECORD)}\n", 1]]
    stubbed(ApfsIntegration, :run, answer: ->(*) { runs.shift }) do
      verify = ApfsIntegration.fsimg_verify("fsimg", "image", [[[], nil], [["volume=1"], nil]])
      assert_predicate verify, :refused?
      assert_includes verify.reason, "volume=1: corrupt image"
    end
  end

  def test_verification_requires_at_least_one_volume
    assert_raises(ApfsCapture::Error) { ApfsIntegration.fsimg_verify("fsimg", "image", []) }
  end

  def test_corpus_matches_recorded_refusal_text_exactly
    refused = ApfsIntegration::Verdict.new(outcome: :refused, reason: "bad tree")
    assert_equal :pass, corpus_comparison(verdict: refused, observed: "refused (bad tree)")
    assert_equal :fail, corpus_comparison(verdict: refused, observed: "refused (crash)")
  end

  def test_corpus_matches_no_file_without_a_missing_verdict
    assert_equal :pass, corpus_comparison(verdict: ApfsIntegration::NO_FILE, observed: "no file")
  end

  def test_info_without_successful_complete_summary_cannot_supply_facts
    [0, 1, 2].each do |status|
      output = "#{JSON.generate({ "type" => "info" })}\n"
      stubbed(ApfsIntegration, :run, answer: [output, status]) do
        assert_raises(ApfsCapture::Error) { ApfsIntegration.volume_records("fsimg", "image") }
      end
    end
  end

  def test_info_error_record_supplies_refused_facts
    stubbed(ApfsIntegration, :run, answer: ["#{JSON.generate(ERROR_RECORD)}\n", 1]) do
      records = ApfsIntegration.volume_records("fsimg", "image")
      assert_predicate ApfsIntegration.fsimg_facts(records), :refused?
    end
  end

  def test_info_error_after_partial_info_supplies_refused_facts
    output = [{ "type" => "info" }, ERROR_RECORD].map { JSON.generate(_1) }.join("\n")
    stubbed(ApfsIntegration, :run, answer: [output, 1]) do
      records = ApfsIntegration.volume_records("fsimg", "image")
      assert_predicate ApfsIntegration.fsimg_facts(records), :refused?
    end
  end

  def test_exit_status_requires_a_pass_and_prioritizes_failures
    { [:skip] => 77, [:unrecorded] => 77, [:pass, :skip] => 0, [:pass, :fail] => 1 }.each do |verdicts, status|
      results = verdicts.map { ApfsIntegration.result("checker", "image", _1) }
      stubbed(ApfsIntegration, :manifests, answer: results) do
        stubbed(ApfsIntegration, :captures, answer: []) do
          stubbed(ApfsIntegration, :corpus, answer: []) do
            capture_io { assert_equal status, ApfsIntegration.main(["fsimg"]) }
          end
        end
      end
    end
  end

  def test_docker_skipped_and_missing_image_lines_have_no_verdict
    environment = ->(name) { name == "FSIMG_APFS_DOCKER" ? "1" : nil }
    stubbed(ENV, :[], answer: environment) do
      stubbed(ApfsIntegration, :find_oracle, answer: nil) do
        stubbed(ApfsIntegration, :on_path?, answer: true) do
          output = "image: skipped: not a bare container\nother: pass\n"
          stubbed(ApfsIntegration, :run, answer: [output, 0]) do
            verdicts = ApfsIntegration.apfsck_verdicts({ "skipped" => "image", "missing" => "absent" })
            assert_predicate verdicts.fetch("skipped"), :no_verdict?
            assert_equal "skipped: not a bare container", verdicts.fetch("skipped").reason
            assert_predicate verdicts.fetch("missing"), :no_verdict?
            assert_includes verdicts.fetch("missing").reason, "no verdict"
          end
        end
      end
    end
  end

  def test_extraction_timeout_reaps_the_child_when_group_kill_races
    [Errno::ESRCH, Errno::EPERM].each do |error|
      kill = Process.method(:kill)
      raced_kill = lambda do |*args|
        kill.call(*args)
        raise error
      end
      stubbed(Process, :kill, answer: raced_kill) do
        assert_extraction_has_no_verdict(:timeout)
      end
    end
  end

  private

  def assert_fsck_has_no_verdict(output:, status:)
    comparison = fsck_comparison(output: output, status: status, expected: :accepted)
    assert_equal :fail, comparison.verdict
    assert_includes comparison.detail, "did not run"
  end

  def fsck_comparison(output:, status:, expected:)
    checker = ApfsIntegration::Verdict.new(outcome: expected, reason: "independent judgment")
    attachments = Object.new
    attachments.define_singleton_method(:with) { |*_, **_, &_block| [output, status] }
    Dir.mktmpdir("apfs-fsck-verdict-") do |work|
      stubbed(ApfsIntegration, :darwin?, answer: true) do
        stubbed(ApfsQualify, :clone_image, answer: nil) do
          stubbed(ApfsCapture::Attachments, :new, answer: attachments) do
            ApfsIntegration.fsck_apfs("capture", "image", checker, work)
          end
        end
      end
    end
  end

  def corpus_comparison(verdict:, observed:)
    Dir.mktmpdir("apfs-corpus-verdict-") do |work|
      File.write(File.join(work, "MATRIX.md"), "|sample|ok|ok|ok|ok|#{([observed] * 4).join("|")}|\n")
      seen = ApfsIntegration::COMMANDS.to_h { [_1, verdict] }
      stubbed(ApfsIntegration, :specimen_file, answer: "source") do
        stubbed(ApfsIntegration, :specimen_copy, answer: File.join(work, "copy")) do
          stubbed(ApfsIntegration, :observe, answer: seen) do
            ApfsIntegration.corpus("fsimg", work, work).first.verdict
          end
        end
      end
    end
  end

  # Replace a collaborator for one scenario and restore it even on failure.
  def stubbed(object, name, answer:)
    original = object.method(name)
    object.define_singleton_method(name) do |*args, **options, &block|
      answer.respond_to?(:call) ? answer.call(*args, **options, &block) : answer
    end
    yield
  ensure
    object.define_singleton_method(name, original)
  end

  # Exercise the runner's real nil-status contract for each kind of failure.
  def without_process_verdict(cause)
    command = case cause
              in :timeout then [RbConfig.ruby, "-e", "sleep 60"]
              in :signal then [RbConfig.ruby, "-e", "Process.kill(:KILL, Process.pid)"]
              in :start then ["/nonexistent/fsimg-apfs-integration"]
              end
    timeout = cause == :timeout ? 0.1 : 10
    runner = ->(*) { ApfsIntegration.log.run_status(*command, timeout: timeout) }
    stubbed(ApfsIntegration, :run, answer: runner) { yield }
  end

  def assert_fsimg_has_no_verdict(cause)
    refused = ApfsIntegration::Verdict.new(outcome: :refused, reason: "bad tree")
    without_process_verdict(cause) do
      verify = ApfsIntegration.fsimg_verify("fsimg", "image", [[[], nil]])
      assert_predicate verify, :no_verdict?
      assert_equal :fail, ApfsIntegration.verdict_result("checker", "image", refused, verify).verdict
      assert_predicate ApfsIntegration.command_verdict("fsimg", "W"), :no_verdict?
    end
  end

  def assert_extraction_has_no_verdict(cause)
    Dir.mktmpdir("apfs-verdict-") do |work|
      executable = File.join(work, "fsimg")
      if cause != :start
        command = cause == :timeout ? "sleep 60" : 'kill -KILL $$'
        File.write(executable, "#!/bin/sh\necho 'fsimg: bad tree' >&2\n#{command}\n")
        File.chmod(0o700, executable)
      end
      timer = Timeout.method(:timeout)
      budget = cause == :timeout ? 0.1 : 10
      bounded_wait = ->(_seconds, &block) { timer.call(budget, &block) }

      stubbed(Timeout, :timeout, answer: bounded_wait) do
        verdict = ApfsIntegration.extract_verdict(executable, ["-f", "image"], "/file", work)
        assert_predicate verdict, :no_verdict?
      end
    end
  end

  def assert_checker_has_no_verdict(cause)
    refused = ApfsIntegration::Verdict.new(outcome: :refused, reason: "bad tree")
    without_process_verdict(cause) do
      checker = ApfsIntegration.apfsck_direct("apfsck", "image")
      assert_predicate checker, :no_verdict?
      assert_equal :fail, ApfsIntegration.verdict_result("checker", "image", checker, refused).verdict
      assert_equal :fail, ApfsIntegration.apfsck_result("image", checker, refused).verdict
    end
  end

  def volume(uuid: VOLUME_UUID, name: "FSIMG", counts: [9, 25, 0, 0], snapshots: 0)
    ApfsIntegration::Volume.new(uuid: uuid, name: name, counts: counts, snapshots: snapshots)
  end

  def facts(block_size: 4096, blocks: 16_384, volumes: [volume])
    ApfsIntegration::Facts.new(block_size: block_size, blocks: blocks, volumes: volumes)
  end
end
