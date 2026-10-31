# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Trees mtools populates, read by the real fsimg
# (specs/design-spec-testing.md#fixtures): the formatter bases and a
# sample of drawn trees run through every reading stage, and fsimg's ls,
# extract and verify records match each known tree. Every drawn tree is
# populated into a builder volume rendered in the draws' own cache, so
# nothing of a draw outlives its case, and the fixture cache gains only
# the formatter bases and the empty builder base of each drawn variant,
# which classification renders. A tree mtools refuses or alters is a host
# skip that names why, never an fsimg result.
class TestFatPopulatedFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick").with(tree_draws: 0, tree_sample: 0, populated_trees: 3)
  # A seed whose three draws mtools 4.0.49 writes as drawn, so each one
  # runs fsimg; another mtools may skip some of them.
  SEED = 4243

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
      assert_equal Fat::Trees::BASES.size + Fat::Bases.standard.size, cached_images(root).size,
                   "an empty builder base per drawn variant, and a populated image per formatter base"
    end
  end

  # The seeded failure: a draw whose population piped each file one byte
  # short, through the content seam into this test's own cache, is the
  # image the case finds there, so the read-back finds a tree other than
  # the recipe's, and the case is a host skip that names the difference
  # before fsimg runs.
  def test_a_tree_the_read_back_does_not_reproduce_is_a_host_skip
    short = Module.new do
      def self.each_chunk(entry) = FsimgSpec::FileContent.each_chunk(entry.with(size: [entry.size - 1, 0].max))
    end
    with_populating do |cases, drawn, shell|
      base = populated_base(Fat::Trees::BASES.fetch("fat12"), Fat::Bases::STANDARD_TREE)
      Fat::Bases.materialize(base, cache: drawn, shell:, ceilings: PROFILE.ceilings, content: short)

      error = assert_raises(FsimgSpec::HostSkip) { cases.populated(base, id: "t.short") }
      assert_match(/\Athe mtools read-back did not agree \(disagrees\): .*lacks file/, error.message)
    end
  end

  # mcopy refuses a DOS device name without an extension, which the
  # generator never offers it; a tree that holds one anyway is a host skip
  # naming the refusal, not a harness error.
  def test_a_tree_mtools_refuses_is_a_host_skip_that_names_why
    with_populating do |cases|
      tree = [FsimgSpec::TreeEntry.file("CON", size: 1, seed: 1)]

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

  # Cases that build populated draws into a cache of their own, drawn,
  # with an oracle shell for mtools.
  def with_populating
    Dir.mktmpdir do |directory|
      drawn = FsimgSpec::FixtureCache.new(File.join(directory, "drawn"))
      shell = FsimgSpec::OracleShell.new(home: File.join(directory, "oracle-home"), environment: ENV.to_h)
      cases = Fat::Cases.new(registry: Fat::Suite.registry, cache: drawn, ceilings: PROFILE.ceilings,
                             name_trials: FsimgSpec::NameTrials::NONE, shell:)
      yield cases, drawn, shell
    end
  end

  def populated_base(parameters, tree) =
    FsimgSpec::Base.new(builder: Fat::Builder::NAME, parameters:, tree:, population: "mtools")

  def cached_images(root) = Dir.children(File.join(root, "test", "spec-cache")).select { _1.end_with?(".img") }
end
