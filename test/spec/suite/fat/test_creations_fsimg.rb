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

  # The checker's refusal in the form FreeBSD's fsck_msdosfs gives for a
  # FAT32 volume of 65525 clusters. A checker that prints it before
  # extra, the lines of extra, and exits 8.
  def self.refusing_checker(path, extra: "")
    File.write(path, "#!/bin/sh\necho \"** $2\"\necho 'Invalid signature in fsinfo block'\necho 'Fix? no'\n" \
                     "#{extra}echo 'Filesystem too small (65525 clusters) for FAT32 partition'\nexit 8\n")
    File.chmod(0o700, path)
    path
  end

  # The seeded failure of the checker's verdict: a checker that refuses
  # as FreeBSD's fsck_msdosfs refuses a FAT32 volume of 65525 clusters
  # fails a created image of any other count, and the documented
  # exception for that refusal excuses it at 65525, where the case
  # passes and records the exception, since the host's mtools reads that
  # volume back.
  def test_a_checker_disagreement_fails_a_created_image_unless_an_exception_excuses_it
    Dir.mktmpdir do |stubs|
      checker = self.class.refusing_checker(File.join(stubs, "fsck_msdosfs"))
      with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, failed = run.call("fat.create.valid.fat16")
        _, excused = run.call("fat.create.cluster_count.fat32_65525")
        skip "no oracle runs on this host" if failed.observations.fetch("fsck").state == "unavailable"

        assert_equal "failed", failed.verdict
        assert_match(/fsck: fsck_msdosfs -n exited 8: "\*\* out\/created.img \| Invalid signature in fsinfo block \|/, failed.detail)
        assert_equal ["passed", "excused", "fsck_msdosfs.fat32_65525"],
                     [excused.verdict, *excused.observations.fetch("fsck").to_h.values_at(:state, :exception_id)], excused.detail
      end
    end
  end

  # An exception excuses the checker's complete output only: at 65525
  # clusters, a checker that refuses the volume but also reports a
  # backup boot sector that differs from the primary, a diagnostic the
  # refusal does not explain, fails the case, and its detail names that
  # line first.
  def test_a_checker_diagnostic_no_exception_explains_fails_a_created_image
    Dir.mktmpdir do |stubs|
      checker = self.class.refusing_checker(File.join(stubs, "fsck_msdosfs"),
                                            extra: "echo 'backup (block 6) mismatch with primary bootblock'\n")
      with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, result = run.call("fat.create.cluster_count.fat32_65525")
        checked = result.observations.fetch("fsck")
        skip "no oracle runs on this host" if checked.state == "unavailable"

        assert_equal %w[failed disagrees], [result.verdict, checked.state]
        assert_match(/\Afsck_msdosfs\.fat32_65525 does not explain "backup \(block 6\) mismatch with primary bootblock": /,
                     checked.detail)
      end
    end
  end

  # FreeBSD's fsck_msdosfs cannot read a FAT16 volume of 4085 clusters,
  # so it does not judge one, whatever it would print, even lines that
  # mimic its misread chains; the observation names the blind spot. The
  # rules still judge the image: one whose boot signature is cleared
  # fails create.
  def test_fsck_msdosfs_does_not_judge_a_created_image_of_4085_clusters
    Dir.mktmpdir do |stubs|
      checker = File.join(stubs, "fsck_msdosfs")
      File.write(checker, "#!/bin/sh\nprintf '** %s\\nCluster 8 crossed another chain at 7\\n/x is 10\\n' \"$2\"\nexit 8\n")
      File.chmod(0o700, checker)
      unsigned = ->(path) { File.open(path, "r+b") { _1.pwrite("\0\0".b, 510) } }
      with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, result = run.call("fat.create.cluster_count.fat16_4085")
        checked = result.observations.fetch("fsck")
        skip "no oracle runs on this host" if checked.state == "unavailable"

        assert_equal ["passed", "not_applicable", "fsck_msdosfs.fat16_4085"],
                     [result.verdict, *checked.to_h.values_at(:state, :exception_id)], result.detail
      end
      with_run(tools: { fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, result = run.call("fat.create.cluster_count.fat16_4085", alter: unsigned)

        assert_equal ["failed", ["create"]], [result.verdict, result.stages.map(&:stage)]
        assert_match(/\Acreate: the image it wrote violates fat\.boot\.signature/, result.detail)
      end
    end
  end

  # A created FAT32 image with any one of its three FSInfo signatures
  # wrong violates fat32.fsinfo.signatures, so create fails it on the
  # harness's own reading, whatever the checker reports: here
  # fsck_msdosfs, whose one FSInfo warning at 65525 clusters the
  # exception for its refusal excuses.
  def test_a_created_image_with_a_bad_fsinfo_signature_fails_whatever_the_checker_reports
    Dir.mktmpdir do |stubs|
      checker = self.class.refusing_checker(File.join(stubs, "fsck_msdosfs"))
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

  # A created image with a stale byte past its root directory's end
  # marker, the last byte of the FAT16 root region, violates
  # fat.directory.tail_zeroed, so create fails it on the harness's own
  # reading, although every reader stops at the marker.
  def test_a_created_image_with_a_stale_byte_past_a_directory_end_fails_create
    stale = lambda do |path|
      File.open(path, "r+b") do |image|
        root, entries = fat16_root(image)
        image.pwrite("A", root + entries.bytesize - 1)
      end
    end
    with_run do |run|
      _, result = run.call("fat.create.valid.fat16", alter: stale)

      assert_equal ["failed", ["create"]], [result.verdict, result.stages.map(&:stage)]
      assert_match(/\Acreate: the image it wrote violates fat\.directory\.tail_zeroed/, result.detail)
    end
  end

  # A created image whose directories the harness cannot follow fails
  # create, rather than leaving the stages that read them unclassified:
  # here the root's end marker becomes a subdirectory "X" whose first
  # cluster, 1, is no data cluster. The checker and the read-back are off,
  # so create's own judgement is the only one.
  def test_a_created_image_whose_directories_cannot_be_followed_fails_create
    unfollowable = lambda do |path|
      File.open(path, "r+b") do |image|
        root, entries = fat16_root(image)
        marker = entries.scan(/.{32}/mn).index { _1.getbyte(0).zero? }
        image.pwrite(["X".ljust(11), 0x10, 1].pack("a11Cx14v") + "\0\0\0\0".b, root + (marker * 32))
      end
    end
    with_run(tools: { fsck_fat: nil, fsck_msdosfs: nil, mdir: nil, mtype: nil }) do |run|
      _, result = run.call("fat.create.valid.fat16", alter: unfollowable)

      assert_equal ["failed", ["create"]], [result.verdict, result.stages.map(&:stage)], result.detail
      assert_match(/\Acreate: the image it wrote leaves directories unclassified/, result.detail)
    end
  end

  # The seeded failure of the read-back's verdict at a cutover: an mdir
  # that lists nothing fails the created FAT16 volume of exactly 4085
  # clusters, which no exception excuses, as at any other count. The
  # only checker is fsck_msdosfs, which does not apply there, so the
  # read-back alone fails the case, and an empty listing reads no file,
  # so mtype is never run.
  def test_a_read_back_disagreement_fails_a_created_image_at_a_cutover
    Dir.mktmpdir do |stubs|
      mdir, mtype = %w[mdir mtype].map { File.join(stubs, _1) }
      [mdir, mtype].each do
        File.write(_1, "#!/bin/sh\nexit 0\n")
        File.chmod(0o700, _1)
      end
      checker = File.join(stubs, "fsck_msdosfs")
      File.write(checker, "#!/bin/sh\nexit 8\n")
      File.chmod(0o700, checker)
      with_run(tools: { mdir:, mtype:, fsck_fat: nil, fsck_msdosfs: checker }) do |run|
        _, result = run.call("fat.create.cluster_count.fat16_4085")
        read_back = result.observations.fetch("mtools")
        skip "no oracle runs on this host" if read_back.state == "unavailable"

        assert_equal %w[failed disagrees not_applicable],
                     [result.verdict, read_back.state, result.observations.fetch("fsck").state]
        assert_match(/\Athe oracles disagree with the created image: mtools: mtools lacks directory "DIR"/, result.detail)
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

  # The offset of a FAT16 volume's root region, after the reserved
  # sectors and the FATs, and its BPB_RootEntCnt entries' bytes.
  def fat16_root(image)
    bpb = Fat::Layout::CommonBpb.read(image.pread(36, 0))
    offset = (bpb.bpb_rsvd_sec_cnt + (bpb.bpb_num_fats * bpb.bpb_fat_sz16)) * bpb.bpb_byts_per_sec
    [offset, image.pread(bpb.bpb_root_ent_cnt * 32, offset)]
  end
end
