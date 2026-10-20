# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"
require "minitest"
require "rbconfig"
require "shellwords"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# fsimg's ls, extract and verify records are compared with the tree the
# harness knows an image holds (specs/design-spec-testing.md#expectation-derivation).
# Here a creation case's image, which the real fsimg writes from the
# source tree, is read through a wrapper that seeds one fault per stage:
# ls drops an entry, extract leaves a file one byte off, and verify counts
# one file too few. Each fails its case, and a failing creation recipe
# round-trips through its failure record and bin/replay.
class TestKnownTreeFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  REPLAY = File.expand_path("../../bin/replay", __dir__)
  TREE = [
    FsimgSpec::TreeEntry.new(path: "d", type: "directory", size: 0, seed: 0),
    FsimgSpec::TreeEntry.new(path: "d/a.txt", type: "file", size: 5, seed: 1),
    FsimgSpec::TreeEntry.new(path: "d/b.bin", type: "file", size: 3000, seed: 2),
    FsimgSpec::TreeEntry.new(path: "empty.txt", type: "file", size: 0, seed: 0)
  ].freeze

  # The FAT suite cut to one creation case, which runs create and ls.
  module CreationSuite
    def self.registry = Fat::Suite.registry

    def self.preflight_sample(directory) = Fat::Suite.preflight_sample(directory)

    def self.cases(cache:, ceilings:, name_trials:) =
      [Fat::Suite.replay(TestKnownTreeFsimg.creation("fat.create.listed", %w[create ls]), cache:, ceilings:, name_trials:)]
  end

  def self.creation(id, stages) = FsimgSpec::Recipe.new(
    id:, format: "fat16", kind: "valid", stages:, source: FsimgSpec::Source.new(tree: TREE, settings: {}),
    expected: stages.to_h { [_1, FsimgSpec::Expectation.parse("accept")] }
  )

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  def test_seeded_faults_each_fail_their_case
    Dir.mktmpdir do |root|
      environment = ENV.to_h.merge("FSIMG_BIN" => faulty_fsimg(root))
      setup = FsimgSpec::CasePhase.prepare(results: root, profile: PROFILE, build_directory: root, environment:)
      details = %w[ls extract verify].to_h do |stage|
        classified = Fat::Suite.replay(TestKnownTreeFsimg.creation("fat.create.#{stage}", ["create", stage]),
                                       cache: setup.cache, ceilings: PROFILE.ceilings, name_trials: setup.name_trials).classify.call
        result = FsimgSpec::CasePhase.result_of(classified, setup:, cases_directory: File.join(root, "cases"))
        [stage, "#{result.verdict}: #{result.detail}"]
      end

      assert_equal 'failed: ls: ls lacks file "/d/b.bin" of 3000 bytes', details.fetch("ls")
      assert_match(%r{\Afailed: extract: extract lacks file "d/a.txt" of 5 bytes SHA-256 \h{64}; extract has unexpected file "d/a.txt" of 5 bytes SHA-256 \h{64}\z},
                   details.fetch("extract"))
      assert_equal 'failed: verify: verify reports files "2", not 3', details.fetch("verify")
    end
  end

  # The case phase writes the failing creation case's record, with no
  # image and no base; bin/replay reruns its case.json to the same failure
  # through the faulty wrapper, and to a pass through fsimg itself.
  def test_failing_creation_recipe_round_trips_through_its_record_and_replay
    Dir.mktmpdir do |build_directory|
      results = File.join(build_directory, "results")
      Dir.mkdir(results)
      faulty = faulty_fsimg(build_directory)
      FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory:, environment: ENV.to_h.merge("FSIMG_BIN" => faulty),
                                suites: { "fat" => CreationSuite })
      record = File.join(results, "failures", "fat.create.listed")

      assert_equal %w[case.json create.stderr create.stdout fixture.json ls.stderr ls.stdout replay.sh result.json],
                   Dir.children(record).sort
      assert_equal({ "tool_versions" => {} }, JSON.parse(File.read(File.join(record, "fixture.json"))))
      case_json = File.join(record, "case.json")
      assert_equal [3, 'fat.create.listed: failed: ls: ls lacks file "/d/b.bin" of 3000 bytes'], replay(case_json, build_directory:, fsimg: faulty)
      assert_equal [0, "fat.create.listed: passed"], replay(case_json, build_directory:, fsimg: @fsimg)
    end
  end

  private

  # bin/replay's exit status and verdict line for the recipe at path, run
  # with fsimg as the binary under test.
  def replay(path, build_directory:, fsimg:)
    output = IO.popen({ "FSIMG_BUILDDIR" => build_directory, "FSIMG_BIN" => fsimg }, [RbConfig.ruby, REPLAY, path, "quick"],
                      err: File::NULL, &:read)
    [Process.last_status.exitstatus, output.chomp]
  end

  # A wrapper around fsimg, written into directory, that seeds one fault
  # into the output of each of ls (t), extract (x) and verify (W) where the
  # image holds TREE, keeps fsimg's exit status, and leaves every other stage
  # and tree, the preflight's among them, to fsimg alone.
  def faulty_fsimg(directory)
    flipped = FsimgSpec::FileContent.bytes(seed: 1, offset: 0, length: 1).ord ^ 0xff
    path = File.join(directory, "faulty-fsimg")
    File.write(path, <<~SH)
      #!/bin/sh
      fsimg=#{Shellwords.escape(@fsimg)}
      case $1 in
      t)
          out=$("$fsimg" "$@"); status=$?
          printf '%s\\n' "$out" | grep -v '"path":"/d/b.bin"'
          exit $status ;;
      x)
          "$fsimg" "$@" || exit
          if test -f dest/d/a.txt; then
              printf '\\#{format("%03o", flipped)}' | dd of=dest/d/a.txt bs=1 count=1 conv=notrunc 2>/dev/null
          fi ;;
      W)
          out=$("$fsimg" "$@"); status=$?
          printf '%s\\n' "$out" | sed 's/"files":"3","directories":"2","logical_bytes":"3005"/"files":"2","directories":"2","logical_bytes":"3005"/'
          exit $status ;;
      *) exec "$fsimg" "$@" ;;
      esac
    SH
    File.chmod(0o700, path)
    path
  end
end
