# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fileutils"
require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The fuzz seeds (specs/design-spec-testing.md#profiles):
# one per targeted or promoted FAT case that does not force a format, each
# its image when the image fits the fuzzer's input, else the image without
# the trailing zeros the fuzzer's zero-filled reading supplies.
class TestFatSeeds < Minitest::Test
  Fat = FsimgSpec::Fat
  CEILINGS = FsimgSpec.fetch_profile("quick").ceilings
  FUZZ_SH = File.expand_path("../../../../build-aux/fuzz.sh", __dir__)
  Recipe = Data.define(:id, :kind, :forced_format, :reading) do
    def initialize(reading: true, **fields) = super

    def promoted? = id.start_with?(FsimgSpec::Recipe::PROMOTED_PREFIX)

    def reading? = reading
  end
  Exported = Data.define(:corpus, :names, :cases)

  # One export, shared by the tests: classifying every case is the
  # expensive step. Its directory is removed when the process exits.
  def self.exported
    @exported ||= begin
      root = Dir.mktmpdir
      at_exit { FileUtils.rm_rf(root) }
      corpus = File.join(root, "corpus")
      Dir.mkdir(corpus)
      cases = Fat::Seeds.seeded(cache: FsimgSpec::FixtureCache.new(File.join(root, "cache")), ceilings: CEILINGS)
      Exported.new(corpus:, names: Fat::Seeds.export(corpus, cases), cases:)
    end
  end

  # A seed is its case's image when the image fits the fuzzer's input, and
  # otherwise the image cut where only zeros follow.
  def test_every_seed_is_its_image_or_its_image_without_trailing_zeros
    exported = TestFatSeeds.exported
    refute_empty exported.cases

    assert_equal exported.cases.map { "spec-#{_1.kase.recipe.id}.seed" }.sort, exported.names.sort
    assert_equal exported.names.sort, Dir.children(exported.corpus).sort
    exported.cases.each do |classified|
      image = classified.image
      seed = File.binread(File.join(exported.corpus, "spec-#{classified.kase.recipe.id}.seed"))

      assert_operator seed.bytesize, :<=, Fat::Seeds::MAX_BYTES
      assert_equal image.read(0, seed.bytesize), seed
      if image.length <= Fat::Seeds::MAX_BYTES
        assert_equal image.length, seed.bytesize, "#{classified.kase.recipe.id} keeps its length"
      else
        assert_equal 0, image.read(seed.bytesize, image.length).count("^\0"), "#{classified.kase.recipe.id} loses data"
      end
    end
  end

  # Targeted and promoted reading cases seed the fuzzer, unless they
  # force a format, which the fuzzer cannot; a creation case has no image.
  def test_targeted_and_promoted_cases_without_a_forced_format_are_seeded
    seeds = { Recipe.new(id: "fat.x.1", kind: "targeted", forced_format: nil) => true,
              Recipe.new(id: "fat.valid.1", kind: "valid", forced_format: nil) => false,
              Recipe.new(id: "regression.fat.valid.1", kind: "valid", forced_format: nil) => true,
              Recipe.new(id: "regression.fat.m.1", kind: "malformed", forced_format: nil) => true,
              Recipe.new(id: "fat.x.2", kind: "targeted", forced_format: "fat16") => false,
              Recipe.new(id: "fat.create.x.1", kind: "targeted", forced_format: nil, reading: false) => false }

    assert_equal seeds, seeds.keys.to_h { [_1, Fat::Seeds.seeds?(_1)] }
  end

  # The seed ceiling is the input ceiling build-aux/fuzz.sh gives the FAT
  # fuzzer, so libFuzzer never cuts a seed. The match is on fuzz.sh's own
  # text, so that changing either ceiling makes someone check the other.
  def test_the_seed_ceiling_is_the_fuzzers_input_ceiling
    assert_match(/run_fuzzer "\$work_dir\/fuzz-fat" fuzz-corpus #{Fat::Seeds::MAX_BYTES} /, File.read(FUZZ_SH))
  end

  # A larger image is cut after its data, which may end at the ceiling but
  # not past it.
  def test_data_past_the_ceiling_refuses_the_seed
    Dir.mktmpdir do |root|
      base = File.join(root, "base")
      File.binwrite(base, "\0" * (Fat::Seeds::MAX_BYTES + 2))
      at_ceiling = FsimgSpec::ImageView.open(base).patch(Fat::Seeds::MAX_BYTES - 1, "x")
      past = FsimgSpec::ImageView.open(base).patch(Fat::Seeds::MAX_BYTES, "x")

      assert_equal Fat::Seeds::MAX_BYTES, Fat::Seeds.seed(classified("t.at", at_ceiling)).bytesize
      assert_raises(FsimgSpec::HarnessError) { Fat::Seeds.seed(classified("t.past", past)) }
    end
  end

  # An export replaces a seed, even one that is a symbolic link, without
  # writing through it, and removes the seeds of cases it no longer has;
  # files not named like seeds stay.
  def test_export_replaces_seeds_and_removes_stale_ones
    Dir.mktmpdir do |root|
      corpus = File.join(root, "corpus")
      Dir.mkdir(corpus)
      victim = File.join(root, "victim")
      File.write(victim, "kept")
      cases = TestFatSeeds.exported.cases.first(2)
      name = "spec-#{cases.first.kase.recipe.id}.seed"
      File.symlink(victim, File.join(corpus, name))
      File.write(File.join(corpus, "spec-fat.removed.seed"), "stale")
      File.write(File.join(corpus, "fat16.seed"), "other")

      names = Fat::Seeds.export(corpus, cases)

      assert_equal "kept", File.read(victim)
      refute File.symlink?(File.join(corpus, name))
      assert_equal File.binread(File.join(TestFatSeeds.exported.corpus, name)), File.binread(File.join(corpus, name))
      assert_equal (names + ["fat16.seed"]).sort, Dir.children(corpus).sort
    end
  end

  # A seed whose write or rename does not finish leaves no temporary file
  # behind, whether an error or an interrupt ends it.
  def test_an_unfinished_replacement_leaves_no_temporary
    Dir.mktmpdir do |corpus|
      FileUtils.mkdir_p(File.join(corpus, "spec-t.blocked.seed", "occupied"))
      blocked = classified("t.blocked", OneByteImage.new(bytes: "x"))
      interrupted = classified("t.interrupted", OneByteImage.new(bytes: Interrupting.new))

      assert_raises(SystemCallError) { Fat::Seeds.export(corpus, [blocked]) }
      assert_raises(Interrupt) { Fat::Seeds.export(corpus, [interrupted]) }
      assert_equal ["spec-t.blocked.seed"], Dir.children(corpus)
    end
  end

  # An image of one byte, whose read answers bytes as they are.
  OneByteImage = Data.define(:bytes) do
    def length = 1
    def read(_offset, _length) = bytes
  end

  # Seed bytes that raise Interrupt when IO#write converts them, as Ctrl-C
  # would in the middle of the write.
  class Interrupting
    def to_s = raise(Interrupt)
  end

  private

  def classified(id, image) =
    Data.define(:kase, :image).new(kase: Data.define(:recipe).new(recipe: Recipe.new(id:, kind: "targeted", forced_format: nil)),
                                   image:)
end
