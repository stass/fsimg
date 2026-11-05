# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "set"
require_relative "hfsplus-integration"

class HfsPlusIntegrationTest < Minitest::Test
  H = HfsPlusIntegration

  # What the harness calls answers from a value or a callable for the block's
  # length. minitest 6 moved minitest/mock to a separate gem, and requiring the
  # copy left in minitest 5 reloads minitest and silently runs no tests.
  def stubbed(object, name, answer)
    original = object.method(name)
    object.define_singleton_method(name) do |*args, **options, &block|
      answer.respond_to?(:call) ? answer.call(*args, **options, &block) : answer
    end
    yield
  ensure
    object.define_singleton_method(name, original)
  end
  
  def test_command_non_ascii_stderr_and_argument_are_readable
    path = "/cafe\u0301"
    error = assert_raises(H::Error) do
      H.command(RbConfig.ruby, "-e", '$stderr.write(ARGV.fetch(0)); exit 1', path)
    end
    assert_includes error.message, path
    assert error.message.valid_encoding?
  end

  def test_member_key_error_does_not_end_volume
    snapshot = H::Snapshot.new(entries: {}, digests: {}, matched: Set.new)
    visited = []
    result = H.compare_members({ "/bad" => {}, "/good" => {} },
      oracle: "hfsdump", volume: volume, snapshot: snapshot) do |path, entry|
      visited << path
      entry.fetch("absent") if path == "/bad"
      H::Tally.new(entries: 1)
    end
    assert_equal ["/bad", "/good"], visited
    assert_equal 1, result.findings.length
    assert_equal 1, result.tally.entries
  end

  def volume(name: "test", source: "test", local: true, image: "unused") =
    H::Volume.new(image: image, name: name, source: source, local: local)

  def test_command_timeout_has_its_own_error
    error = assert_raises(H::CommandTimeout) do
      H.command(RbConfig.ruby, "-e", "sleep 10", timeout: 1)
    end
    assert_includes error.message, "after 1 seconds"
  end

  def test_command_timeout_rounds_fractional_budget_to_one_decimal
    error = assert_raises(H::CommandTimeout) do
      H.command(RbConfig.ruby, "-e", "sleep 10", timeout: 0.14)
    end
    assert_includes error.message, "after 0.1 seconds"
  end

  def test_timeout_verdict_depends_on_command_owner_and_volume
    refute H.timeout_skip?(oracle: "fsimg", local: false)
    assert H.timeout_skip?(oracle: "hfsdump", local: false)
    refute H.timeout_skip?(oracle: "hfsdump", local: true)
  end

  def test_command_retains_only_a_bounded_failure_excerpt
    error = assert_raises(H::Error) do
      H.command(RbConfig.ruby, "-e", '$stderr.write("x" * (4 << 20)); exit 1')
    end
    assert_operator error.message.bytesize, :<, 10_000
  end

  def test_digest_output_streams_bytes_into_a_digest
    result = H.digest_output(RbConfig.ruby, "-e", '$stdout.write("abc" * 100_000)')
    assert_equal Digest::SHA256.hexdigest("abc" * 100_000), result.sha256
    assert_equal 300_000, result.bytes
  end

  def test_same_paths_rejects_repeated_actual_paths
    assert_raises(H::Error) do
      H.same_paths(expected: ["/a"], actual: ["/a", "/a"], context: "paths")
    end
    H.same_paths(expected: ["^", "^"], actual: ["^", "^"], context: "mapped paths")
  end

  def test_member_limitation_requires_manifest_match
    volume = volume(name: "h-hfsplus-16777216-e5", source: "native", local: false)
    options = { oracle: "sleuth", volume: volume, path: "/attr-sizes", message: "Unsupported record type: (32)" }
    refute H.known_limitation?(**options, matched: Set.new)
    assert H.known_limitation?(**options, matched: Set["/attr-sizes"])
  end

  def test_volume_limitation_has_no_member_digest_requirement
    volume = volume(name: "h-hfsx-8388608-e4", source: "native", local: false)
    assert H.known_limitation?(oracle: "sleuth", volume: volume, path: nil,
      message: "hfs_cat_file_lookup: Error finding thread node for file (1)", matched: Set.new)
  end

  def test_volume_limitation_can_match_nearlyfull_writer_volumes
    %w[hfsplus hfsx].each do |variant|
      volume = volume(name: "#{variant}-4096-nearlyfull-small-auto", source: "writer", local: true)
      options = { oracle: "sleuth", volume: volume, path: nil, matched: Set.new }
      assert H.known_limitation?(**options,
        message: 'fls paths: missing=["/dir/sub/below"] (1) extra=[] (0)')
      refute H.known_limitation?(**options,
        message: 'fls paths: missing=["/other"] (1) extra=[] (0)')
    end
  end

  def test_extraction_requires_a_summary_record
    volume = volume(name: "summary", source: "test", local: true)
    Dir.mktmpdir("hfs-summary-", ENV["TMPDIR"]) do |directory|
      fsimg = File.join(directory, "no-summary")
      File.write(fsimg, "#!/bin/sh\nexit 0\n")
      File.chmod(0o700, fsimg)
      error = assert_raises(H::Error) do
        H.extract_digests(fsimg: fsimg, volume: volume, entries: {}, directory: directory)
      end
      assert_includes error.message, "summary"
    end
  end

  def test_hfsdump_restricted_members_compare_children_against_all_entries
    volume = volume(name: "restricted", source: "test", local: true)
    entries = { "/a" => { "kind" => "file" }, "/b" => { "kind" => "file" } }
    snapshot = H::Snapshot.new(entries: entries, digests: {}, matched: Set.new)
    invoke = lambda do |_operation, _tool, _image, action, path|
      assert_equal "/", path
      action == "stat" ? "type: folder\ncnid: 2\nvalence: 2\n" : "a\nb\n"
    end

    result = H.hfsdump_reader(tool: "unused", volume: volume, snapshot: snapshot,
      invoke: invoke, members: {})
    assert_empty result.findings
    assert_equal 1, result.tally.entries
  end

  def test_mount_environment_errors_are_distinct_from_real_refusals
    ["unknown filesystem type 'hfsplus'", "failed to setup loop device", "Operation not permitted", "permission denied"].each do |message|
      assert H.mount_environment_error?(message), message
    end
    refute H.mount_environment_error?("bad superblock")
  end

  # PATH contains only disposable scripts; mounted? supplies the observed state.
  def with_mount_stubs(mount:, umount:, mounted:)
    Dir.mktmpdir("hfs-mount-", ENV["TMPDIR"]) do |directory|
      log = File.join(directory, "calls")
      { "mount" => mount, "umount" => umount }.each do |name, body|
        path = File.join(directory, name)
        File.write(path, "#!/bin/sh\necho #{name} \"$@\" >> '#{log}'\n#{body}\n")
        File.chmod(0o700, path)
      end
      original_path = ENV["PATH"]
      begin
        ENV["PATH"] = directory
        stubbed(H, :mounted?, mounted) { yield directory, log }
      ensure
        ENV["PATH"] = original_path
      end
    end
  end

  def test_refused_mount_is_a_finding
    with_mount_stubs(mount: "echo 'bad superblock' >&2; exit 1", umount: "exit 0", mounted: false) do |directory, log|
      output, errors = capture_io do
        row = H.comparison(oracle: "linux-mount", volume: volume) do
          H.with_mount(image: "unused", mountpoint: directory) { flunk "mount refused" }
        end
        assert_equal "finding", row.verdict
        assert_includes row.findings.first, "bad superblock"
      end
      assert_includes errors, "FINDING"
      refute_includes output, "SKIP"
      refute_includes File.read(log), "umount"
    end
  end

  def test_environment_mount_refusal_is_a_skip
    with_mount_stubs(mount: "echo 'permission denied' >&2; exit 1", umount: "exit 0", mounted: false) do |directory, _log|
      capture_io do
        row = H.comparison(oracle: "linux-mount", volume: volume) do
          H.with_mount(image: "unused", mountpoint: directory) { flunk "mount refused" }
        end
        assert_equal "skip", row.verdict
        assert_empty row.findings
      end
    end
  end

  def test_failed_unmount_followed_by_lazy_success_is_a_finding
    with_mount_stubs(mount: "exit 0", umount: '[ "$1" = "-l" ]', mounted: true) do |directory, log|
      result = H.with_mount(image: "unused", mountpoint: directory) { H::Result.new }
      assert_equal 1, result.findings.length
      assert_includes result.findings.first, "unmount failed"
      assert_equal ["mount", "umount", "umount"], File.readlines(log).map { _1.split.first }
      assert_includes File.readlines(log).last, "-l"
    end
  end

  def test_failed_unmount_preserves_earlier_error
    with_mount_stubs(mount: "exit 0", umount: '[ "$1" = "-l" ]', mounted: true) do |directory, log|
      original = H::Error.new("earlier error")
      _output, errors = capture_io do
        error = assert_raises(H::Error) do
          H.with_mount(image: "unused", mountpoint: directory) { raise original }
        end
        assert_same original, error
      end
      assert_includes errors, "unmount failed while handling earlier error"
      assert_includes File.readlines(log).last, "-l"
    end
  end

  def test_mount_timeout_with_live_mount_unmounts
    with_mount_stubs(mount: "while :; do :; done", umount: "exit 0", mounted: true) do |directory, log|
      assert_raises(H::CommandTimeout) do
        H.with_mount(image: "unused", mountpoint: directory, timeout: 1) { flunk "mount timed out" }
      end
      assert_equal ["mount", "umount"], File.readlines(log).map { _1.split.first }
    end
  end

  def test_unmatched_sources_are_explicit_sources_without_volumes
    sources = { "native" => "captures/*/*.img", "corpus" => "corpus/*.img", "unset" => nil }.freeze
    volumes = [volume(source: "native")].freeze
    assert_equal ["corpus"], H.unmatched_sources(sources: sources, volumes: volumes)
  end

  def test_extraction_counts_every_special_kind_as_skipped
    entries = H::KINDS.keys.reject { %w[file directory].include?(_1) }.to_h do |kind|
      ["/#{kind}", { "kind" => kind, "size_bytes" => "0" }]
    end
    summary = JSON.generate(type: "summary", skipped_entries: 5, omitted_entries: 0)
    empty = H::FileDigest.new(sha256: Digest::SHA256.hexdigest(""), bytes: 0)
    Dir.mktmpdir("hfs-kinds-", ENV["TMPDIR"]) do |directory|
      stubbed(H, :command, summary) do
        stubbed(H, :digest_output, empty) do
          digests = H.extract_digests(fsimg: "unused", volume: volume,
            entries: entries.freeze, directory: directory)
          assert_equal({ "/symlink" => empty }, digests)
        end
      end
    end
  end

  def test_extraction_rejects_a_symlink_at_a_regular_file_path
    Dir.mktmpdir("hfs-kind-", ENV["TMPDIR"]) do |directory|
      invoke = lambda do |*argv, **_options|
        extraction = argv.fetch(argv.index("-C") + 1)
        File.symlink("missing", File.join(extraction, "file"))
        JSON.generate(type: "summary", skipped_entries: 0, omitted_entries: 0)
      end
      stubbed(H, :command, invoke) do
        error = assert_raises(H::Error) do
          H.extract_digests(fsimg: "unused", volume: volume,
            entries: { "/file" => { "kind" => "file" } }, directory: directory)
        end
        assert_includes error.message, "not a regular file"
      end
    end
  end

  def test_mounted_reader_preserves_colliding_normalized_names
    entries = { "/caf\u00e9" => { "kind" => "file" }, "/cafe\u0301" => { "kind" => "file" } }
    snapshot = H::Snapshot.new(entries: entries, digests: {}, matched: Set.new)
    visited = []
    Dir.mktmpdir("hfs-names-", ENV["TMPDIR"]) do |directory|
      mountpoint = File.join(directory, "mount")
      hosts = entries.keys.map { mountpoint + _1 }
      traverse = ->(_root, &visit) { hosts.each { visit.call(_1) } }
      stat = ->(host) { visited << host; Struct.new(:ftype).new("file") }
      mounting = ->(**_options, &read) { read.call }
      stubbed(H, :with_mount, mounting) do
        stubbed(Find, :find, traverse) do
          stubbed(File, :lstat, stat) do
            result = H.mounted_reader(volume: volume, snapshot: snapshot, directory: directory)
            assert_empty result.findings
            assert_equal 2, result.tally.entries
          end
        end
      end
      assert_equal hosts, visited
    end
  end

  def test_each_oracle_has_its_own_volume_budget
    snapshot = H::Snapshot.new(entries: {}, digests: {}, matched: Set.new)
    baseline = H::Result.new(value: snapshot)
    time = 0
    budgets = []
    clock = ->(_clock) { time }
    command = ->(*_args, timeout:) { budgets << timeout }
    hfsdump = lambda do |invoke:, **_options|
      invoke.call(:command, "hfsdump")
      time += H::READER_TIMEOUT + 1
      H::Result.new
    end
    sleuth = lambda do |invoke:, **_options|
      invoke.call(:command, "fls")
      H::Result.new
    end
    stubbed(H, :fsimg_snapshot, baseline) do
      stubbed(Process, :clock_gettime, clock) do
        stubbed(H, :command, command) do
          stubbed(H, :hfsdump_reader, hfsdump) do
            stubbed(H, :sleuth_reader, sleuth) do
              capture_io do
                rows = H.reader_volume(fsimg: "unused", volume: volume,
                  available: { hfsdump: "hfsdump", sleuth: true, mount: false }, directory: "unused")
                assert_equal %w[agree agree agree skip], rows.map(&:verdict)
              end
            end
          end
        end
      end
    end
    assert_equal [H::READER_TIMEOUT, H::READER_TIMEOUT], budgets
  end

  def test_native_manifest_requires_missing_digest_and_decodes_utf8
    Dir.mktmpdir("hfs-manifest-", ENV["TMPDIR"]) do |directory|
      path = "cafe\u0301"
      sha256 = Digest::SHA256.hexdigest("bytes")
      File.write(File.join(directory, "manifest.json"), JSON.generate(tree: [{ path: path, sha256: sha256 }]))
      native = volume(source: "native", image: File.join(directory, "image.img"))
      error = assert_raises(H::Error) { H.manifest_matches(volume: native, digests: {}) }
      assert_includes error.message, "no fsimg digest"
      digest = H::FileDigest.new(sha256: sha256, bytes: 5)
      assert_equal Set["/#{path}"], H.manifest_matches(volume: native, digests: { "/#{path}" => digest })
    end
  end

  def test_exit_code_requires_independent_agreement_and_matching_sources
    agreed = H::Comparison.new(value: nil, verdict: "agree", oracle: "hfsdump", findings: [])
    finding = agreed.with(verdict: "finding", findings: ["wrong bytes"])
    assert_equal 1, H.exit_code(rows: [finding, agreed], unmatched: [])
    assert_equal 77, H.exit_code(rows: [agreed.with(oracle: "fsimg")], unmatched: [])
    assert_equal 1, H.exit_code(rows: [agreed], unmatched: ["native"])
    assert_equal 0, H.exit_code(rows: [agreed], unmatched: [])
  end

  def test_comparison_reports_timeout_verdict_and_actual_duration
    [{ oracle: "fsimg", local: false, verdict: "finding" },
     { oracle: "hfsdump", local: false, verdict: "skip" },
     { oracle: "hfsdump", local: true, verdict: "finding" }].each do |example|
      example => { oracle:, local:, verdict: }
      volume = volume(name: "timeout", source: "test", local: local)
      output, errors = capture_io do
        row = H.comparison(oracle: oracle, volume: volume) do
          raise H::CommandTimeout, "timed out after 0.75 seconds"
        end
        assert_equal verdict, row.verdict
      end
      assert_includes output + errors, "0.75 seconds"
    end
  end

  def test_member_findings_and_timeout_form_one_finding
    volume = volume(name: "members", source: "test", local: false)
    snapshot = H::Snapshot.new(entries: {}, digests: {}, matched: Set.new)
    output, errors = capture_io do
      result = H.compare_members({ "/bad" => {}, "/slow" => {} },
        oracle: "hfsdump", volume: volume, snapshot: snapshot) do |path, _entry|
        raise H::Error, "wrong bytes" if path == "/bad"

        raise H::CommandTimeout, "per-volume budget of 60 seconds ran out"
      end
      row = H.comparison(oracle: "hfsdump", volume: volume) { result }
      assert_equal "finding", row.verdict
    end
    refute_includes output, "SKIP"
    assert_includes errors, "wrong bytes"
    assert_includes errors, "per-volume budget of 60 seconds ran out"
  end

  def test_hfsdump_empty_nonroot_folder_with_positive_valence_is_a_finding
    volume = volume(name: "folder", source: "test", local: true)
    entries = { "/empty" => { "kind" => "directory" } }
    snapshot = H::Snapshot.new(entries: entries, digests: {}, matched: Set.new)
    invoke = lambda do |_operation, _tool, _image, action, path|
      if action == "stat"
        "type: folder\ncnid: 2\nvalence: 1\n"
      else
        assert_equal "/", path
        "empty\n"
      end
    end

    result = H.hfsdump_reader(tool: "unused", volume: volume, snapshot: snapshot, invoke: invoke)
    assert_equal 1, result.findings.length
    assert_includes result.findings.first, "non-root valence 1"
  end

  def test_hfsdump_root_may_count_hidden_children
    volume = volume(name: "root", source: "test", local: true)
    snapshot = H::Snapshot.new(entries: {}, digests: {}, matched: Set.new)
    invoke = ->(_operation, _tool, _image, action, _path) { action == "stat" ? "type: folder\ncnid: 2\nvalence: 2\n" : "" }

    result = H.hfsdump_reader(tool: "unused", volume: volume, snapshot: snapshot, invoke: invoke)
    assert_empty result.findings
    assert_equal 1, result.tally.entries
  end

  def test_hfsdump_missing_folder_valence_is_a_finding
    volume = volume(name: "folder", source: "test", local: true)
    snapshot = H::Snapshot.new(entries: {}, digests: {}, matched: Set.new)
    invoke = lambda do |_operation, _tool, _image, action, _path|
      assert_equal "stat", action
      "type: folder\ncnid: 2\n"
    end

    result = H.hfsdump_reader(tool: "unused", volume: volume, snapshot: snapshot, invoke: invoke)
    assert_equal ["\"/\": hfsdump folder lacks valence"], result.findings
  end

  def test_hfsdump_skips_only_the_unaddressable_stored_nul_member
    volume = volume(name: "nul", source: "writer", local: true)
    entries = { "/\u2400" => { "kind" => "file", "size_bytes" => "0" } }
    snapshot = H::Snapshot.new(entries: entries, digests: {}, matched: Set.new)
    calls = []
    invoke = lambda do |_operation, _tool, _image, action, path|
      assert_equal "/", path
      refute_includes path, "\u2400"
      calls << { action: action, path: path }
      action == "stat" ? "type: folder\ncnid: 2\nvalence: 1\n" : "\n"
    end

    result = H.hfsdump_reader(tool: "unused", volume: volume, snapshot: snapshot, invoke: invoke)
    assert_empty result.findings
    assert_equal 1, result.skips.length
    assert_includes result.skips.first, 'stored-NUL name'
    assert_equal 1, result.tally.entries
    assert_equal [{ action: "stat", path: "/" }, { action: "read", path: "/" }], calls
  end
end
