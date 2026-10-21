# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Trees mtools populates, read by the real fsimg
# (specs/design-spec-testing.md#fixtures): the formatter bases and a
# sample of drawn trees run through every reading stage, and fsimg's ls,
# extract and verify records match each known tree. Every drawn tree is
# populated into a copy of its variant's one cached empty volume, so the
# fixture cache gains that volume alone, and nothing of a draw outlives its
# case. A tree mtools refuses or alters is a host skip that names why,
# never an fsimg result; a volume off its recipe is the harness's fault.
class TestFatPopulatedFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick").with(tree_draws: 0, tree_sample: 0, populated_trees: 3)
  # A seed whose three draws mtools 4.0.49 writes as drawn, so each one
  # runs fsimg; another mtools may skip some of them.
  SEED = 4243
  STALE = FsimgSpec::TreeEntry.new(path: "STALE.TXT", type: "file", size: 1, seed: 1)

  # The FAT suite cut to the cases whose trees mtools populates.
  module PopulatedSuite
    def self.registry = Fat::Suite.registry

    def self.preflight_sample(directory) = Fat::Suite.preflight_sample(directory)

    def self.cases(**arguments) =
      Fat::Suite.cases(**arguments).select { _1.id.start_with?("fat.mtools.") }
  end

  def setup
    tools = FsimgSpec::Tools.find(ENV)
    missing = %i[mkfs_fat mmd mcopy mdir mtype].reject { tools[_1] }
    skip "#{missing.join(", ")} unavailable" unless missing.empty?
    begin
      FsimgSpec::OracleShell.utf8_locale
    rescue FsimgSpec::FixtureError => e
      skip e.message
    end
  end

  # Every populated case that mtools wrote runs fsimg and passes, carrying
  # the read-back's agreement observed when its base was built.
  def test_populated_trees_read_back_through_fsimg
    fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(fsimg)
    with_results do |root, results|
      phase = FsimgSpec::CasePhase.call(profile: PROFILE, results:, build_directory: root, environment: ENV.to_h,
                                        seed: SEED, suites: { "fat" => PopulatedSuite })
      fat = phase.formats.fetch("fat")
      cases = fat.cases

      assert_equal({ total: 6, failed: 0, harness_error: 0 }, cases.slice(:total, :failed, :harness_error))
      assert_operator cases.fetch(:passed), :>=, Fat::Bases.standard.size + 1, "the formatter bases and a draw pass"
      assert_equal cases.fetch(:passed), fat.oracles.dig("mtools", "agrees")
      assert_empty Dir.children(File.join(results, "drawn"))
      assert_equal Fat::Trees::BASES.size + (2 * Fat::Bases.standard.size), cached_images(root).size,
                   "an empty volume per drawn variant, and an empty and a populated image per formatter base"
    end
  end

  # The seeded failure: the cached empty volume a draw is populated into,
  # planted already holding a file, is the volume the case copies, so the
  # read-back finds a tree other than the recipe's, and the case is a host
  # skip that names the difference before fsimg runs.
  def test_a_tree_the_read_back_does_not_reproduce_is_a_host_skip
    with_populating do |cases, volumes|
      parameters = Fat::Trees::BASES.fetch("fat12")
      plant_volume(volumes, parameters:, planted: parameters, tree: [STALE])

      error = assert_raises(FsimgSpec::HostSkip) { cases.populated(populated_base(parameters, Fat::Bases::STANDARD_TREE), id: "t.stale") }
      assert_match(/\Athe mtools read-back did not agree \(disagrees\): .*unexpected file "STALE.TXT"/, error.message)
    end
  end

  # A volume rendered with two sectors per cluster where its recipe says
  # one is no tool's limit but the harness's fault: a fixture error.
  def test_a_volume_off_its_recipe_is_a_fixture_error
    with_populating do |cases, volumes|
      parameters = Fat::Trees::BASES.fetch("fat12")
      plant_volume(volumes, parameters:, planted: parameters.merge(sectors_per_cluster: 2), tree: [])

      error = assert_raises(FsimgSpec::FixtureError) { cases.populated(populated_base(parameters, Fat::Bases::STANDARD_TREE), id: "t.planted") }
      assert_match(/\At.planted: its volume is off its recipe: .*sectors_per_cluster is 2, not 1/, error.message)
    end
  end

  # mcopy refuses a DOS device name without an extension, which the
  # generator never offers it; a tree that holds one anyway is a host skip
  # naming the refusal, not a harness error.
  def test_a_tree_mtools_refuses_is_a_host_skip_that_names_why
    with_populating do |cases|
      tree = [FsimgSpec::TreeEntry.new(path: "CON", type: "file", size: 1, seed: 1)]

      error = assert_raises(FsimgSpec::HostSkip) { cases.populated(populated_base(Fat::Trees::BASES.fetch("fat12"), tree), id: "t.device") }
      assert_match(/\Amtools did not write the tree: mcopy exited 1: /, error.message)
    end
  end

  private

  def with_results
    Dir.mktmpdir do |root|
      results = File.join(root, "results")
      Dir.mkdir(results)
      yield root, results
    end
  end

  # Cases that build populated draws into a cache of their own, from the
  # empty volumes of volumes, with an oracle shell for mtools.
  def with_populating
    Dir.mktmpdir do |directory|
      volumes = FsimgSpec::FixtureCache.new(File.join(directory, "cache"))
      shell = FsimgSpec::OracleShell.new(home: File.join(directory, "oracle-home"), environment: ENV.to_h)
      cases = Fat::Cases.new(registry: Fat::Suite.registry, cache: FsimgSpec::FixtureCache.new(File.join(directory, "drawn")),
                             volume_cache: volumes, ceilings: PROFILE.ceilings, name_trials: FsimgSpec::NameTrials::NONE, shell:)
      yield cases, volumes
    end
  end

  def populated_base(parameters, tree) =
    FsimgSpec::Base.new(builder: Fat::Builder::NAME, parameters:, tree:, population: "mtools")

  def cached_images(root) = Dir.children(File.join(root, "test", "spec-cache")).select { _1.end_with?(".img") }

  # Publishes the builder's image of planted and tree under the cache key
  # of the empty builder volume of parameters, before anything builds
  # that volume.
  def plant_volume(cache, parameters:, planted:, tree:)
    empty = FsimgSpec::Base.new(builder: Fat::Builder::NAME, parameters:, tree: [])
    cache.fetch(cache.key(empty, Fat::Bases.builder_versions)) do
      Fat::Builder.write(_1, parameters: planted, tree:, max_bytes: PROFILE.ceilings.image)
    end
  end
end
