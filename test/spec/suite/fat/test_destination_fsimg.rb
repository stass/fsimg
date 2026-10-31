# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "digest"
require "json"
require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# An extract case is classified with the host's answers to its name
# trials (specs/design-spec-testing.md#host-names), here on a FAT volume
# that holds e-acute both as U+00E9 and as e with U+0301. A host that
# folds normalization, such as APFS, takes the two for one name, so
# extract must reject with FSIMG_EEXISTS; one that does not, such as ext4,
# extracts both. Each expectation follows from a probe of this host, and
# the real fsimg runs every case.
class TestDestinationFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  WITNESS = "host.name.collision.nfc_nfd"
  NFC = "\u00e9"
  NFD = "e\u0301"

  # A trial that takes EEXIST for success: it drops every collision from
  # the host's answers.
  class ExistsBlindTrials < FsimgSpec::NameTrials
    def call(tree) = super.then { _1.with(refused: _1.refused.reject { |refusal| refusal.answer == :exists }) }
  end

  # A host that stores every name decomposed, as HFS+ does.
  module DecomposingFunnel
    def self.create_file(directory, name) =
      FsimgSpec::NameFunnel.create_file(directory, name.dup.force_encoding(Encoding::UTF_8).unicode_normalize(:nfd))

    def self.children(directory) = FsimgSpec::NameFunnel.children(directory)
  end

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  # The witness passes against the host's own answers, and a record of
  # the case shows them beside extract's result.
  def test_collision_witness_passes_on_the_hosts_own_answers
    with_phase do |phase|
      classified, result = phase.run(witness, phase.setup.name_trials)
      record = FsimgSpec::FailureRecord.write(File.join(phase.results, "failures"), id: WITNESS, kase: classified.kase, result:,
                                                                                image: classified.image, profile: PROFILE)

      assert_equal ["passed", [["extract", extract_outcome]]], [result.verdict, outcomes(result)]
      refused = host_folds? ? [{ "path" => NFC.inspect, "answer" => "exists" }] : []
      assert_equal({ "refused" => refused, "refused_count" => refused.size, "unheld" => nil },
                   JSON.parse(File.read(File.join(record, "result.json"))).dig("stages", 0, "name_trials"))
    end
  end

  # The seeded failure: a trial blind to EEXIST derives acceptance, which
  # fsimg's FSIMG_EEXISTS contradicts on a host that folds the pair.
  def test_trial_blind_to_eexist_fails_the_witness_where_the_host_folds
    with_phase do |phase|
      _, result = phase.run(witness, ExistsBlindTrials.new(File.join(phase.results, "trials")))

      if host_folds?
        assert_equal ["failed", "extract: expected accept, got rejected:FSIMG_EEXISTS"], [result.verdict, result.detail]
      else
        assert_equal "passed", result.verdict
      end
    end
  end

  # A host that stores other names than the trial created cannot hold the
  # case's tree: the case is a host skip, and fsimg does not run.
  def test_host_storing_other_names_skips_the_case
    composed = Fat::Witnesses::Witness.new(
      id: "fat.test.composed", parameters: { clusters: 100 }, mutations: [], forced_format: nil, intended: nil, rule: nil,
      tree: [FsimgSpec::TreeEntry.file(NFC, size: 1, seed: 1)]
    )
    with_phase do |phase|
      _, result = phase.run(composed, FsimgSpec::NameTrials.new(File.join(phase.results, "trials"), funnel: DecomposingFunnel))

      assert_equal ["host_skip", [], "the host stored other names than it created in the root"],
                   [result.verdict, result.stages, result.detail]
    end
  end

  # A promoted recipe whose tree holds the pair records no destination
  # violation and expects extract to accept, the same on every host. It
  # replays without drift, and passes against the host's answers.
  def test_promoted_nfc_nfd_recipe_replays_without_drift_and_passes
    with_phase do |phase|
      Dir.mktmpdir do |directory|
        recipe = promoted_recipe(phase.setup.cache)
        File.write(File.join(directory, "#{recipe.id}.json"), recipe.dump)
        promoted = Fat::Suite.promoted(directory, ceilings: PROFILE.ceilings).first
        classified = FsimgSpec::CasePhase.classify(Fat::Suite.replay(promoted, cache: phase.setup.cache, ceilings: PROFILE.ceilings,
                                                                                 name_trials: phase.setup.name_trials, shell: phase.setup.shell))
        result = FsimgSpec::CasePhase.result_of(classified, setup: phase.setup, cases_directory: File.join(phase.results, "cases"))

        assert_equal [nil, []], [classified.defect, classified.kase.drift]
        assert_equal "passed", result.verdict, result.detail
        assert_equal extract_outcome, outcomes(result).to_h.fetch("extract")
      end
    end
  end

  private

  # One preflight for a test: its Setup, results directory, and run, which
  # classifies a witness with the given name trials and runs it, answering the
  # Classified case and its CaseResult.
  Phase = Data.define(:setup, :results) do
    def run(witness, name_trials)
      classified = Fat::Cases.new(registry: Fat::Suite.registry, cache: setup.cache, ceilings: PROFILE.ceilings, name_trials:,
                                 shell: setup.shell)
                             .call(witness)
      [classified, FsimgSpec::CasePhase.result_of(classified, setup:, cases_directory: File.join(results, "cases"))]
    end
  end

  def with_phase
    Dir.mktmpdir do |root|
      results = File.join(root, "results")
      Dir.mkdir(results)
      setup = FsimgSpec::CasePhase.prepare(results:, profile: PROFILE, build_directory: root, environment: ENV.to_h)
      yield Phase.new(setup:, results:)
    end
  end

  def witness = Fat::Witnesses.deterministic(Fat::Suite.registry).find { _1.id == WITNESS }

  def outcomes(result) = result.stages.map { [_1.stage, _1.outcome.to_s] }

  def extract_outcome = host_folds? ? "rejected:FSIMG_EEXISTS" : "accepted"

  # A valid recipe on a builder base whose root holds the pair, run
  # through every reading stage, as a regression would be promoted.
  def promoted_recipe(cache)
    tree = [NFC, NFD].each_with_index.map { FsimgSpec::TreeEntry.file(_1, size: 10, seed: _2 + 1) }
    base = FsimgSpec::Base.new(builder: Fat::Builder::NAME, parameters: { clusters: 100 }, tree:)
    path = Fat::Bases.render(base, cache:, ceilings: PROFILE.ceilings)
    FsimgSpec::Recipe.new(
      id: "regression.fat.nfc_nfd_siblings", format: "fat12", kind: "valid", stages: FsimgSpec::READING_STAGES,
      base:, base_sha256: Digest::SHA256.file(path).hexdigest, tool_versions: Fat::Bases.builder_versions.transform_keys(&:to_sym),
      mutations: [], expected: FsimgSpec::READING_STAGES.to_h { [_1, FsimgSpec::Expectation.parse("accept")] },
      reason: "A recipe written for this test: its tree's names collide on some hosts and not on others."
    )
  end

  # Whether this host stores the NFD and NFC spellings as one name,
  # probed with the plain file API in a directory of its own.
  def host_folds?
    Dir.mktmpdir do |probe|
      File.write(File.join(probe, NFD), "")
      File.exist?(File.join(probe, NFC))
    end
  end
end
