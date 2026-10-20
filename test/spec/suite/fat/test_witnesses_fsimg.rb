# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Every deterministic witness of the FAT rules runs through the real
# fsimg: a valid one, or one of a must_accept rule, through every reading
# stage, and any other targeted one through the first stage that checks
# its intended rule. A rule whose status is mis-set fails its witness with
# both statuses named; a scope mis-set to one the model leaves
# unclassified asserts nothing about fsimg (docs/spec-testing-plan.md, S6).
class TestFatWitnessesFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  REGISTRY = Fat::Rules.registry

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  def test_every_witness_passes
    witnesses = Fat::Witnesses.deterministic(REGISTRY)
    refute_empty witnesses
    with_run do |run|
      failures = witnesses.filter_map do |witness|
        result = run.call(witness, REGISTRY)
        "#{result.id}: #{result.verdict}: #{result.detail}" unless result.verdict == "passed"
      end

      assert_empty failures
    end
  end

  # A rule whose status is mis-set fails its witness and names both
  # statuses.
  def test_misset_status_is_reported
    registry = altered("fat.bpb.bytes_per_sector") { _1.with(status: FsimgSpec::Status.new(name: "FSIMG_ECORRUPT")) }
    with_run do |run|
      result = run.call(witness("fat.bpb.bytes_per_sector.0201", registry), registry)

      assert_equal ["failed", "info: expected reject:FSIMG_ECORRUPT, got rejected:FSIMG_EFORMAT"], [result.verdict, result.detail]
    end
  end

  # A rule whose scope is mis-set to verification, which the model leaves
  # unclassified, moves its witness to verify with no derived
  # expectation: the case passes without asserting anything about fsimg,
  # and the coverage report shows the rule as unclassified rather than
  # asserted.
  def test_misset_scope_to_an_unclassified_scope_asserts_nothing
    registry = altered("fat.bpb.media") { _1.with(scopes: ["verification"]) }
    with_run do |run|
      result = run.call(witness("fat.bpb.media.0", registry), registry)

      assert_equal ["passed", ["verify"]], [result.verdict, result.stages.map(&:stage)]
    end
  end

  private

  def altered(id, &change) = FsimgSpec::Registry.new(REGISTRY.rules.map { _1.id == id ? change.call(_1) : _1 })

  def witness(id, registry) = Fat::Witnesses.deterministic(registry).find { _1.id == id }

  # Yields a callable that classifies a witness under a registry and runs
  # it, after one preflight for the whole test. A witness whose case shows
  # a generator defect fails the test instead.
  def with_run
    Dir.mktmpdir do |root|
      results = File.join(root, "results")
      Dir.mkdir(results)
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      runner, sandbox = FsimgSpec::Preflight.new(results:, build_directory: root, profile: PROFILE, fsimg: @fsimg,
                                                 environment: ENV.to_h, sample: FsimgSpec::Fat::Bases.method(:preflight_sample)).call
      cache = FsimgSpec::FixtureCache.new(File.join(root, "cache"))
      yield lambda { |witness, registry|
        classified = Fat::Cases.new(registry:, cache:, ceilings: PROFILE.ceilings).call(witness)
        flunk classified.defect if classified.defect
        FsimgSpec::CaseRun.call(classified.kase, model: classified.model, image: classified.image, runner:, sandbox:,
                                                 cases_directory: File.join(results, "cases"))
      }
    end
  end
end
