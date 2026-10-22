# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# Creation cases through the real fsimg (specs/design-spec-testing.md#cases):
# the tree reaches create as a manifest, so it runs on every host and
# writes no source tree; the input rules decide what create must refuse;
# and a manifest writer that mis-encodes a name stops the run in the
# preflight rather than failing a case.
class TestFatCreationsFsimg < Minitest::Test
  Fat = FsimgSpec::Fat
  PROFILE = FsimgSpec.fetch_profile("quick")
  REGISTRY = Fat::Rules.registry
  # Where section 5 places FSInfo's three signatures within its sector.
  FSINFO_SIGNATURE_OFFSETS = { "lead" => 0, "structure" => 484, "trail" => 508 }.freeze

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  # A trailing period breaks the FAT name rules, from a manifest and from
  # a host tree alike: each classifies as an input violation, and create
  # refuses it with FSIMG_EINVAL. A case pair runs from its manifest on a
  # host that folds case too, and create refuses it.
  def test_create_refuses_what_the_input_rules_refuse
    with_run do |run|
      ids = %w[fat.create.name.trailing_period fat.create.name.host_trailing_period fat.create.unique_names.ascii_case_pair]
      results = ids.to_h { [_1, run.call(_1)] }

      assert_equal({ "fat.create.name.trailing_period" => ["fat.create.name@input"],
                     "fat.create.name.host_trailing_period" => ["fat.create.name@input"],
                     "fat.create.unique_names.ascii_case_pair" => ["fat.create.unique_names@input"] },
                   results.transform_values { |(kase, _)| kase.recipe.actual_violations.map(&:to_s) })
      results.each do |id, (_, result)|
        assert_equal ["passed", ["create"], ["rejected:FSIMG_EINVAL"]],
                     [result.verdict, result.stages.map(&:stage), result.stages.map { _1.outcome.to_s }], id
      end
    end
  end

  # A valid manifest case runs create, the second create from the
  # reversed manifest, which writes the same bytes, and every reading
  # stage, and the oracles read the image create wrote; its case
  # directory never holds a source tree, which the runner's checks would
  # name.
  def test_a_valid_manifest_case_runs_every_stage
    with_run do |run|
      _, result = run.call("fat.create.valid.fat16")

      assert_equal ["passed", ["create", "create-reversed", *FsimgSpec::READING_STAGES]],
                   [result.verdict, result.stages.map(&:stage)], result.detail
      read_back = result.observations.fetch("mtools")
      assert_includes %w[agrees unavailable], read_back.state, read_back.detail
    end
  end

  # A checker at path that prints the image line and a diagnostic, as
  # fsck_msdosfs does, and exits 8: it refuses every image it reads.
  def self.failing_checker(path)
    File.write(path, "#!/bin/sh\nprintf '** %s\\nCluster 8 crossed another chain at 7\\n' \"$2\"\nexit 8\n")
    File.chmod(0o700, path)
    path
  end

  # The seeded failure of the checker's verdict: a checker that refuses
  # every image fails a created FAT16 image of an ordinary count, quoting
  # what it wrote.
  def test_a_checker_disagreement_fails_a_created_image
    Dir.mktmpdir do |stubs|
      checker = self.class.failing_checker(File.join(stubs, "fsck_msdosfs"))
      with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, result = run.call("fat.create.valid.fat16")
        skip "no oracle runs on this host" if result.observations.fetch("fsck").state == "unavailable"

        assert_equal "failed", result.verdict
        assert_match(/fsck: fsck_msdosfs -n exited 8: "\*\* out\/created.img \| Cluster 8 crossed another chain at 7"/,
                     result.detail)
      end
    end
  end

  # FreeBSD's fsck_msdosfs misreads a FAT16 volume of 4085 clusters and
  # refuses a FAT32 volume of 65525, so at each it does not apply and
  # never runs, whatever it would print, and the observation gives the
  # blind spot's reason. The read-back still judges the volume and,
  # where the host has it, agrees, so the case passes; and the rules
  # still judge it: an image whose boot signature is cleared fails
  # create.
  def test_fsck_msdosfs_does_not_judge_a_created_image_at_the_cutovers
    Dir.mktmpdir do |stubs|
      checker = self.class.failing_checker(File.join(stubs, "fsck_msdosfs"))
      unsigned = ->(path) { File.open(path, "r+b") { _1.pwrite("\0\0".b, 510) } }
      { "fat16_4085" => /4085 clusters FAT16/, "fat32_65525" => /65525 clusters FAT32/ }.each do |cutover, reason|
        with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
          _, result = run.call("fat.create.cluster_count.#{cutover}")
          checked = result.observations.fetch("fsck")
          skip "no oracle runs on this host" if checked.state == "unavailable"

          assert_equal %w[passed not_applicable], [result.verdict, checked.state], result.detail
          assert_match reason, checked.detail
          assert_includes %w[agrees unavailable], result.observations.fetch("mtools").state, cutover
        end
        with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
          _, result = run.call("fat.create.cluster_count.#{cutover}", alter: unsigned)

          assert_equal ["failed", ["create"]], [result.verdict, result.stages.map(&:stage)], cutover
          assert_match(/\Acreate: the image it wrote violates fat\.boot\.signature/, result.detail)
        end
      end
    end
  end

  # A created FAT32 image with any one of its three FSInfo signatures
  # wrong violates fat32.fsinfo.signatures, so create fails it on the
  # harness's own reading, whatever the checker reports: here
  # fsck_msdosfs at 65525 clusters, which a blind spot keeps from the
  # volume.
  def test_a_created_image_with_a_bad_fsinfo_signature_fails_whatever_the_checker_reports
    Dir.mktmpdir do |stubs|
      checker = self.class.failing_checker(File.join(stubs, "fsck_msdosfs"))
      # A failed case keeps its directory, so each corruption runs apart.
      FSINFO_SIGNATURE_OFFSETS.each do |label, offset|
        corrupt = lambda do |path|
          File.open(path, "r+b") do |image|
            sector, fsinfo = [11, 48].map { image.pread(2, _1).unpack1("v") }
            image.pwrite("\0\0\0\0".b, (sector * fsinfo) + offset)
          end
        end
        with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
          _, result = run.call("fat.create.cluster_count.fat32_65525", alter: corrupt)

          assert_equal ["failed", ["create"]], [result.verdict, result.stages.map(&:stage)], label
          assert_match(/\Acreate: the image it wrote violates fat32\.fsinfo\.signatures/, result.detail, label)
        end
      end
    end
  end

  # The seeded failure of the read-back's verdict at the cutovers: an
  # mdir that lists nothing fails the created FAT16 volume of exactly
  # 4085 clusters and the FAT32 volume of 65525, as at any other count.
  # The only checker is fsck_msdosfs, which a blind spot keeps from both,
  # so the read-back alone fails each case, and an empty listing reads no
  # file, so mtype is never run.
  def test_a_read_back_disagreement_fails_a_created_image_at_the_cutovers
    Dir.mktmpdir do |stubs|
      mdir, mtype = %w[mdir mtype].map { File.join(stubs, _1) }
      [mdir, mtype].each do
        File.write(_1, "#!/bin/sh\nexit 0\n")
        File.chmod(0o700, _1)
      end
      checker = self.class.failing_checker(File.join(stubs, "fsck_msdosfs"))
      %w[fat16_4085 fat32_65525].each do |cutover|
        with_run(tools: { mdir:, mtype:, fsck_fat: nil, fsck_msdosfs: checker }) do |run|
          _, result = run.call("fat.create.cluster_count.#{cutover}")
          read_back = result.observations.fetch("mtools")
          skip "no oracle runs on this host" if read_back.state == "unavailable"

          assert_equal %w[failed disagrees not_applicable],
                       [result.verdict, read_back.state, result.observations.fetch("fsck").state], cutover
          assert_match(/\Athe oracles disagree with the created image: mtools: mtools lacks /, result.detail)
        end
      end
    end
  end

  # A manifest writer that leaves # unescaped turns the rest of a name
  # into a comment; fsimg refuses the manifest, and the preflight, whose
  # names need that escape, stops the run, so no case reads the refusal
  # as its own.
  def test_a_manifest_writer_that_leaves_hash_unescaped_fails_the_preflight
    unescaped = Module.new do
      def self.lines(tree) = FsimgSpec::Manifest.lines(tree).map { _1.gsub("\\043".b, "#".b) }
    end
    Dir.mktmpdir do |results|
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      error = assert_raises(FsimgSpec::PreflightError) do
        FsimgSpec::Preflight.new(results:, profile: PROFILE, fsimg: @fsimg, environment: ENV.to_h, manifest: unescaped,
                                 sample: Fat::Bases.method(:preflight_sample)).call
      end
      assert_match(/\Apreflight preflight create: rejected:FSIMG_EINVAL/, error.message)
    end
  end

  private

  # Yields a callable that classifies the creation witness of an id and
  # runs it, answering its Case and CaseResult, after one preflight for
  # the whole test. A witness whose case shows a generator defect fails
  # the test instead. tools replaces those of the host's tools it names in
  # the oracles' shell. alter, when given, changes the image create wrote
  # before the harness decodes it, standing in for a defect of fsimg.
  def with_run(tools: {})
    Dir.mktmpdir do |root|
      results = File.join(root, "results")
      Dir.mkdir(results)
      setup = FsimgSpec::CasePhase.prepare(results:, profile: PROFILE, build_directory: root, environment: ENV.to_h)
      unknown = tools.keys - setup.shell.tools.keys
      flunk "no such tool: #{unknown.join(", ")}" unless unknown.empty?
      shell = FsimgSpec::OracleShell.new(home: File.join(root, "checker-home"), environment: ENV.to_h,
                                         tools: setup.shell.tools.merge(tools))
      cases = Fat::Cases.new(registry: REGISTRY, cache: setup.cache, ceilings: PROFILE.ceilings, name_trials: setup.name_trials,
                             shell:)
      witnesses = Fat::Witnesses.creations(REGISTRY, ceilings: PROFILE.ceilings).to_h { [_1.id, _1] }
      yield lambda { |id, alter: nil|
        classified = cases.creation_witness(witnesses.fetch(id))
        flunk classified.defect if classified.defect
        if alter
          decode = classified.created
          classified = classified.with(created: lambda { |path|
            alter.call(path)
            decode.call(path)
          })
        end
        [classified.kase, FsimgSpec::CasePhase.result_of(classified, setup:, cases_directory: File.join(results, "cases"))]
      }
    end
  end
end
