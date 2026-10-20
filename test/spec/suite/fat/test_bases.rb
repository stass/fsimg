# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"
require "fileutils"
require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# FAT bases (specs/design-spec-testing.md#fixtures): every base passes the
# read-only checker and mdir, a builder base and a formatter base of one
# geometry decode to equal models, a base off its parameters is refused,
# and fsimg accepts every base and extracts its tree.
class TestFatBases < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  CEILINGS = PROFILE.ceilings
  READING_STAGES = %w[info ls extract verify].freeze
  # Boot-sector fields a formatter chooses freely: the jump's target, the
  # OEM name and the CHS geometry,
  FORMATTER_CHOICES = %i[bs_jmp_boot bs_oem_name bpb_sec_per_trk bpb_num_heads].freeze
  # and the BIOS drive number, which mkfs.fat sets to 0 on a floppy-sized
  # volume.
  EXTENSION_CHOICES = %i[bs_drv_num].freeze

  # Builder bases at each variant threshold, with one, two and three FATs,
  # and with 4096-byte sectors.
  BUILDER_BASES = {
    "fat12 below the FAT16 threshold" => { clusters: 4084 },
    "fat16 at the threshold" => { clusters: 4085 },
    "fat16 below the FAT32 threshold" => { clusters: 65_524 },
    "fat32 at the threshold" => { clusters: 65_525 },
    "fat16 with one FAT" => { clusters: 5000, fat_count: 1 },
    "fat16 with three FATs" => { clusters: 5000, fat_count: 3 },
    "fat32 with one FAT" => { clusters: 70_000, fat_count: 1 },
    "fat12 with 4096-byte sectors" => { clusters: 4084, bytes_per_sector: 4096 },
    "fat32 with 4096-byte sectors" => { clusters: 65_525, bytes_per_sector: 4096 }
  }.freeze

  def setup
    @tools = FsimgSpec::Tools.find(ENV)
  end

  def test_builder_bases_pass_the_checkers
    skip_without_checkers
    with_cache do |cache, shell|
      BUILDER_BASES.each do |name, parameters|
        fixture = Fat::Bases.materialize(builder_base(parameters, Fat::Bases.standard.fetch("fat32").tree), cache:, shell:, tools: @tools, ceilings: CEILINGS)

        assert fixture.usable?, "#{name}: #{fixture.problems.join("; ")}"
      end
    end
  end

  def test_formatter_bases_pass_the_checkers
    skip_without(:mkfs_fat, :mmd, :mcopy)
    skip_without_checkers
    with_cache do |cache, shell|
      Fat::Bases.standard.each do |name, base|
        fixture = Fat::Bases.materialize(base, cache:, shell:, tools: @tools, ceilings: CEILINGS)

        assert fixture.usable?, "#{name}: #{fixture.problems.join("; ")}"
        assert_equal name, fixture.manifest.fetch(:format)
      end
    end
  end

  # mtools stamps the entries it creates at SOURCE_DATE_EPOCH, so one
  # formatter base built twice, further apart than FAT's two-second time
  # resolution, is the same image, and a recipe's base_sha256 replays.
  def test_formatter_base_built_twice_is_identical
    skip_without(:mkfs_fat, :mmd, :mcopy)
    base = Fat::Bases.standard.fetch("fat32")
    digests = [0, 2.1].map do |delay|
      sleep delay
      with_cache do |cache, shell|
        Digest::SHA256.file(Fat::Bases.materialize(base, cache:, shell:, tools: @tools, ceilings: CEILINGS).image).hexdigest
      end
    end

    assert_equal digests.first, digests.last
  end

  # Two runs may build one base at once: a build populates its tree in a
  # directory of its own, and never touches another build's.
  def test_a_build_leaves_another_builds_population_alone
    skip_without(:mkfs_fat, :mmd, :mcopy)
    base = Fat::Bases.standard.fetch("fat32")
    with_cache do |cache, shell, _results, directory|
      Fat::Bases.materialize(base, cache:, shell:, tools: @tools, ceilings: CEILINGS)
      key = File.basename(Dir.glob(File.join(directory, "cache", "*.json")).first, ".json")
      other = FsimgSpec::FixtureCache.new(File.join(directory, "other"))
      planted = File.join(directory, "other", "#{key}.src", "keep")
      FileUtils.mkdir_p(File.dirname(planted))
      File.write(planted, "")
      Fat::Bases.materialize(base, cache: other, shell:, tools: @tools, ceilings: CEILINGS)

      assert File.exist?(planted), "the build removed another build's population directory"
    end
  end

  # The builder renders each formatter base's decoded geometry; the two
  # images decode to equal geometry, BPB, extension and signature. The
  # formatter's geometry is the builder's input, so this compares how the
  # two encode one geometry, not how each would choose it.
  def test_builder_and_formatter_bases_of_one_geometry_decode_to_equal_models
    skip_without(:mkfs_fat, :mmd, :mcopy)
    with_cache do |cache, shell|
      Fat::Bases.standard.each_value do |base|
        formatter = decode(Fat::Bases.materialize(base, cache:, shell:, tools: @tools, ceilings: CEILINGS).image)
        builder = decode(Fat::Bases.materialize(builder_base(parameters_of(formatter), []), cache:, shell:, tools: @tools, ceilings: CEILINGS).image)

        assert_equal formatter.geometry, builder.geometry
        assert_equal formatter.common.to_h.except(*FORMATTER_CHOICES), builder.common.to_h.except(*FORMATTER_CHOICES)
        assert_equal formatter.extension.to_h.except(*EXTENSION_CHOICES), builder.extension.to_h.except(*EXTENSION_CHOICES)
        assert_equal formatter.signature, builder.signature
      end
    end
  end

  # Bases the harness cannot build are refused as fixture errors before
  # any tool runs, so a malformed recipe base is refused, never a crash.
  REFUSED_BASES = {
    "a formatter base without its size" => [{ builder: "mkfs.fat", parameters: { fat_bits: 16 }, tree: [] }, "needs blocks"],
    "a formatter base with an unknown parameter" =>
      [{ builder: "mkfs.fat", parameters: { blocks: 1440, fat_bits: 12, color: 1 }, tree: [] }, "unknown mkfs.fat parameters color"],
    "a formatter base whose tree outgrows its image" =>
      [{ builder: "mkfs.fat", parameters: { blocks: 1440, fat_bits: 12 },
         tree: [FsimgSpec::TreeEntry.new(path: "BIG", type: "file", size: 2 << 20, seed: 1)] }, "tree exceeds"],
    "an unknown builder" => [{ builder: "mformat", parameters: {}, tree: [] }, "unknown builder mformat"],
    "a builder base its builder refuses" => [{ builder: "independent", parameters: { clusters: 0 }, tree: [] }, "clusters"],
    "a base beyond the image ceiling" =>
      [{ builder: "independent", parameters: { clusters: 1 << 22, sectors_per_cluster: 128 }, tree: [] }, "exceeds the"]
  }.freeze

  def test_bases_the_harness_cannot_build_are_refused
    with_cache do |cache, shell|
      REFUSED_BASES.each do |name, (fields, message)|
        error = assert_raises(FsimgSpec::FixtureError, name) do
          Fat::Bases.materialize(FsimgSpec::Base.new(**fields), cache:, shell:, tools: @tools, ceilings: CEILINGS)
        end
        assert_includes error.message, message, name
      end
    end
  end

  # render writes a builder base within the image ceiling and refuses one
  # beyond it.
  def test_render_bounds_the_image_by_the_ceiling
    with_cache do |cache|
      small = builder_base({ clusters: 100 }, [])
      large = builder_base({ clusters: 1 << 22, sectors_per_cluster: 128 }, [])

      assert File.file?(Fat::Bases.render(small, cache:, ceilings: CEILINGS))
      assert_raises(FsimgSpec::FixtureError) { Fat::Bases.render(large, cache:, ceilings: CEILINGS) }
    end
  end

  # The seeded failure: an image rendered with two sectors per cluster
  # under a recipe that says one is refused.
  def test_base_off_its_parameters_is_refused
    Dir.mktmpdir do |directory|
      image = File.join(directory, "base.img")
      Fat::Builder.write(image, parameters: { clusters: 5000, sectors_per_cluster: 2 }, tree: [], max_bytes: CEILINGS.image)

      problems = Fat::Bases.geometry_problems(builder_base({ clusters: 5000 }, []), image)
      assert_includes problems, "sectors_per_cluster is 2, not 1"
    end
  end

  def test_fsimg_accepts_every_base_and_extracts_its_tree
    fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(fsimg)

    with_cache do |cache, shell, results|
      runner, sandbox = FsimgSpec::Preflight.new(results:, build_directory: results, profile: PROFILE, fsimg:,
                                                 environment: ENV.to_h, sample: FsimgSpec::Fat::Bases.method(:preflight_sample)).call
      bases.each do |name, base|
        fixture = Fat::Bases.materialize(base, cache:, shell:, tools: @tools, ceilings: CEILINGS)
        image = FsimgSpec::ImageView.open(fixture.image)
        model = Fat::Model.decode(image, tree: FsimgSpec::KnownTree.of(base.tree))
        result = FsimgSpec::CaseRun.call(reading_case(name, base), model:, image:, runner:,
                                                                   cases_directory: File.join(results, "cases"), sandbox:)

        assert_equal "passed", result.verdict, "#{name}: #{result.detail}"
      end
    end
  end

  private

  def bases
    formatters = [@tools[:mkfs_fat], @tools[:mmd], @tools[:mcopy]].all? ? Fat::Bases.standard.transform_keys { "#{_1} formatter" } : {}
    builders = { "fat12" => 2000, "fat16" => 20_000, "fat32" => 70_000 }.to_h do |name, clusters|
      ["#{name} builder", builder_base({ clusters: }, Fat::Bases.standard.fetch("fat32").tree)]
    end
    formatters.merge(builders)
  end

  def builder_base(parameters, tree) = FsimgSpec::Base.new(builder: Fat::Builder::NAME, parameters:, tree:)

  # The builder parameters that reproduce model's geometry.
  def parameters_of(model)
    geometry = model.geometry
    used = geometry.first_data_sector + (geometry.cluster_count * geometry.sectors_per_cluster)
    fat32 = model.fat32_extension ? { fs_info: model.extension.bpb_fs_info, backup_boot: model.extension.bpb_bk_boot_sec } : {}
    {
      bytes_per_sector: geometry.bytes_per_sector, sectors_per_cluster: geometry.sectors_per_cluster,
      reserved_sectors: geometry.reserved_sectors, fat_count: geometry.fat_count, root_entries: geometry.root_entries,
      media: model.common.bpb_media, clusters: geometry.cluster_count, fat_size: geometry.fat_size,
      tail_sectors: geometry.total_sectors - used, hidden_sectors: model.common.bpb_hidd_sec,
      volume_id: model.extension.bs_vol_id, **fat32
    }
  end

  def reading_case(name, base)
    recipe = FsimgSpec::Recipe.new(
      id: name.tr(" ", "_"), format: decode_format(base), kind: "valid", stages: READING_STAGES,
      expected: READING_STAGES.to_h { [_1, FsimgSpec::Expectation.parse("accept")] },
      base:, base_sha256: "0" * 64, tool_versions: {}, mutations: []
    )
    clean = FsimgSpec::Classification.new(violations: [], unclassified: [], trial_answers: FsimgSpec::TrialAnswers::HELD)
    FsimgSpec::Case.new(recipe:, classification: clean,
                        intended_rule: nil, expected: recipe.expected)
  end

  def decode_format(base) = base.builder == Fat::Builder::NAME ? Fat.variant_of(base.parameters.fetch(:clusters)) : "fat#{base.parameters.fetch(:fat_bits)}"

  def decode(image) = Fat::Model.decode(FsimgSpec::ImageView.open(image))

  def skip_without(*names)
    missing = names.reject { @tools[_1] }
    skip "#{missing.join(", ")} unavailable" unless missing.empty?
  end

  # A base checked by no tool passes vacuously, so the checker tests need
  # a read-only checker and mdir.
  def skip_without_checkers
    skip "no read-only checker" unless @tools[:fsck_fat] || @tools[:fsck_msdosfs]
    skip_without(:mdir)
  end

  def with_cache
    Dir.mktmpdir do |directory|
      results = File.join(directory, "results")
      Dir.mkdir(results)
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      cache = FsimgSpec::FixtureCache.new(File.join(directory, "cache"))
      shell = FsimgSpec::OracleShell.new(home: File.join(results, "oracle-home"), environment: ENV.to_h)
      yield cache, shell, results, directory
    end
  end
end
