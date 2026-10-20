# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "fileutils"
require "tmpdir"
require "fsimg_spec"

# The preflight refuses a host that cannot run the profile's cases before
# it runs fsimg at all (specs/design-spec-testing.md#runner), so these
# checks need no fsimg binary.
class TestPreflight < Minitest::Test
  PROFILE = FsimgSpec.fetch_profile("quick")
  # Every check here fails before the preflight runs fsimg or reads its
  # sample.
  NO_SAMPLE = ->(_directory) { raise "the preflight read its sample" }

  # A results directory so deep that a case tree at the path ceiling would
  # pass the host's PATH_MAX.
  def test_results_path_too_long_for_the_path_ceiling_fails
    with_results do |root|
      host = Integer(IO.popen(["getconf", "PATH_MAX", root], &:read))
      results = deep_directory(root, beyond: host - PROFILE.ceilings.path - FsimgSpec::Preflight::LONGEST_CASE_ROOT)
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }

      assert_match(/PATH_MAX/, assert_raises(FsimgSpec::PreflightError) { preflight(results) }.message)
    end
  end

  def test_descriptor_limit_below_the_fds_ceiling_fails
    with_results do |results|
      soft, hard = Process.getrlimit(:NOFILE)
      Process.setrlimit(:NOFILE, PROFILE.ceilings.fds - 1, hard)
      begin
        error = assert_raises(FsimgSpec::PreflightError) { preflight(results) }
      ensure
        Process.setrlimit(:NOFILE, soft, hard)
      end

      assert_match(/RLIMIT_NOFILE/, error.message)
    end
  end

  # /dev is its own filesystem on every supported host.
  def test_trials_on_another_filesystem_fails
    with_results do |results|
      Dir.rmdir(File.join(results, "trials"))
      File.symlink("/dev", File.join(results, "trials"))

      assert_match(/different filesystems/, assert_raises(FsimgSpec::PreflightError) { preflight(results) }.message)
    end
  end

  # env confines nothing, and false runs nothing: the probe's positive
  # control refuses a command that cannot run the oracle at all.
  def test_confinement_that_confines_nothing_or_runs_nothing_fails
    { "env" => /outside its case directory/, "false" => /did not run the probe/ }.each do |command, message|
      with_results do |results|
        environment = { "PATH" => ENV.fetch("PATH"), "FSIMG_SPEC_ORACLE_CONFINE" => command }
        error = assert_raises(FsimgSpec::PreflightError) { preflight(results, environment:) }

        assert_match message, error.message, command
        assert_equal %w[cases trials], Dir.children(results).sort, "#{command}: the probe left files behind"
      end
    end
  end

  private

  def with_results
    Dir.mktmpdir do |results|
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      yield results
    end
  end

  # A directory under root whose path is longer than beyond bytes, built
  # from components short enough for any host.
  def deep_directory(root, beyond:)
    components = ((beyond - root.bytesize) / 201) + 1
    (1..components).reduce(root) { |path, _| File.join(path, "d" * 200).tap { Dir.mkdir(_1) } }
  end

  def preflight(results, environment: { "PATH" => ENV.fetch("PATH") }) =
    FsimgSpec::Preflight.new(results:, build_directory: results, profile: PROFILE, fsimg: "/nonexistent/fsimg",
                             environment:, sample: NO_SAMPLE).call
end
