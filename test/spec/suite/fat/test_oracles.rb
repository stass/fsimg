# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The FAT oracles (specs/design-spec-testing.md#independence-and-oracles):
# each tool's observation of an image whose tree the harness knows agrees,
# disagrees with a short detail, does not apply, or is unavailable; tool
# output is read within caps; and nothing a case directory holds
# configures a tool.
class TestFatOracles < Minitest::Test
  Fat = FsimgSpec::Fat
  CEILINGS = FsimgSpec.fetch_profile("quick").ceilings
  TOOLS = Fat::Oracles::TOOLS

  # A file of size bytes, or a directory without one.
  def self.entry(path, size: nil, seed: 1) =
    (size ? FsimgSpec::TreeEntry.file(path, size:, seed:) : FsimgSpec::TreeEntry.directory(path))

  # ASCII names that every tool reads, among them pattern characters
  # mtools must take literally and a directory whose name starts with -.
  ASCII_TREE = [
    entry("HELLO.TXT", size: 13), entry("a[1].b", size: 700), entry("empty file", size: 0),
    entry("-dash"), entry("-dash/inner [x]"), entry("-dash/inner [x]/blocks.bin", size: 5000, seed: 3)
  ].freeze

  def setup
    @tools = FsimgSpec::Tools.find(ENV)
  end

  def test_every_tool_agrees_with_an_image_of_its_tree
    skip_without(:fsck, :mdir, :mtype, :fatcat)
    observe_image(tree: ASCII_TREE) do |observations|
      assert_equal TOOLS.to_h { [_1, FsimgSpec::Observation.agrees] }, observations
    end
  end

  # A file whose bytes differ from the known tree's makes the read-backs
  # disagree, naming the file; the checker, which knows no tree, agrees.
  def test_a_file_other_than_the_tree_says_disagrees
    skip_without(:fsck, :mdir, :mtype, :fatcat)
    known = ASCII_TREE.map { _1.path == "a[1].b" ? _1.with(seed: 9) : _1 }
    observe_image(tree: ASCII_TREE, known:) do |observations|
      assert_equal "agrees", observations.fetch("fsck").state
      %w[mtools fatcat].each do |tool|
        assert_equal "disagrees", observations.fetch(tool).state, tool
        assert_match(/lacks file "a\[1\].b" of 700 bytes SHA-256/, observations.fetch(tool).detail, tool)
      end
    end
  end

  # A file far longer than the tree says is read only up to the cap, and
  # the stream past it is a disagreement.
  def test_a_stream_past_its_cap_disagrees
    skip_without(:mdir, :mtype)
    large = [self.class.entry("BIG.BIN", size: 100_000)]
    observe_image(tree: large, known: [self.class.entry("BIG.BIN", size: 1)]) do |observations|
      assert_equal FsimgSpec::Observation.disagrees(%(mtype "BIG.BIN" wrote more than #{1 + Fat::Oracles::READ_SLACK_BYTES} bytes)),
                   observations.fetch("mtools")
    end
  end

  # Each tool states the geometry or names it cannot read: the checkers
  # three FATs, mtools a long name beyond the BMP, fatcat a 4096-byte
  # sector, a name that is not ASCII, and a one-sector FAT16 root region
  # beside two-sector clusters, which the pinned fatcat reads on into
  # cluster 2 and lists that directory's entries as the root's.
  def test_a_tool_that_cannot_read_the_image_does_not_apply
    skip_without(:fsck, :mdir, :mtype, :fatcat)
    observe_image(tree: [self.class.entry("\u{10400} beyond.txt", size: 5)], parameters: { clusters: 5000, fat_count: 3 }) do |observations|
      assert_equal %w[not_applicable not_applicable not_applicable], observations.values_at(*TOOLS).map(&:state)
    end
    observe_image(tree: ASCII_TREE, parameters: { clusters: 5000, bytes_per_sector: 4096 }) do |observations|
      assert_equal "fatcat reads 512-byte sectors, not 4096", observations.fetch("fatcat").detail
    end
    observe_image(tree: ASCII_TREE, parameters: { clusters: 5000, sectors_per_cluster: 2, root_entries: 16 }) do |observations|
      assert_equal "fatcat reads a FAT12 or FAT16 root region in whole clusters", observations.fetch("fatcat").detail
    end
  end

  def test_a_tool_the_host_lacks_is_unavailable
    observe_image(tree: ASCII_TREE, tools: {}) do |observations|
      assert_equal %w[unavailable] * 3, observations.values_at(*TOOLS).map(&:state)
    end
  end

  # The seeded failure: a .mtoolsrc planted in the directory an oracle
  # runs in, which would make mtools list HELLO.TXT as hello.txt,
  # configures no oracle, since the oracles' HOME and MTOOLSRC name
  # oracle-home/.mtoolsrc alone.
  def test_a_planted_mtoolsrc_does_not_reach_the_oracle
    skip_without(:mdir, :mtype)
    observe_image(tree: ASCII_TREE, plant: { ".mtoolsrc" => "MTOOLS_LOWER_CASE=1\n" }) do |observations|
      assert_equal FsimgSpec::Observation.agrees, observations.fetch("mtools")
    end
  end

  # An empty volume lists nothing, and every tool agrees with that.
  def test_every_tool_agrees_with_an_empty_volume
    skip_without(:fsck, :mdir, :mtype, :fatcat)
    observe_image(tree: []) do |observations|
      assert_equal TOOLS.to_h { [_1, FsimgSpec::Observation.agrees] }, observations
    end
  end

  # A tool that exits otherwise than 0 disagrees, quoting the lines
  # it wrote, here a checker that finds the volume dirty.
  def test_a_tool_that_fails_disagrees_quoting_what_it_wrote
    Dir.mktmpdir do |stubs|
      checker = File.join(stubs, "fsck")
      File.write(checker, "#!/bin/sh\necho >&2\necho 'dirty bit is set' >&2\nexit 4\n")
      File.chmod(0o700, checker)
      observe_image(tree: [], tools: { fsck_fat: checker }) do |observations|
        assert_equal FsimgSpec::Observation.disagrees('fsck -n exited 4: "dirty bit is set"'), observations.fetch("fsck")
      end
    end
  end

  # fatcat finds a FAT32 root directory only at cluster 2, though the
  # specification allows any, so it does not apply to another.
  def test_fatcat_does_not_apply_to_a_root_directory_outside_cluster_2
    skip_without(:fsck, :mdir, :mtype, :fatcat)
    observe_image(tree: [], parameters: { clusters: 70_000, root_cluster: 3 }) do |observations|
      assert_equal %w[agrees agrees not_applicable], observations.values_at(*TOOLS).map(&:state)
    end
  end

  # A host without the oracles' UTF-8 locale observes every tool as
  # unavailable, and runs none.
  def test_a_host_without_a_utf8_locale_observes_every_tool_unavailable
    observe_image(tree: ASCII_TREE, unavailable: "the host has no C or en_US UTF-8 locale for the oracles") do |observations|
      assert_equal [FsimgSpec::Observation.unavailable("the host has no C or en_US UTF-8 locale for the oracles")] * 3,
                   observations.values_at(*TOOLS)
    end
  end

  # A checker's detail quotes every line it wrote, stderr first, so that
  # a diagnostic after a progress line, as FreeBSD's fsck_msdosfs writes,
  # is in the detail an exception matches; its label names the program.
  def test_a_failing_checker_is_quoted_by_program_and_every_line
    Dir.mktmpdir do |stubs|
      checker = File.join(stubs, "fsck_msdosfs")
      File.write(checker, "#!/bin/sh\necho '** image.img'\necho\necho 'Filesystem too small'\necho 'dirty' >&2\nexit 8\n")
      File.chmod(0o700, checker)
      observe_image(tree: [], tools: { fsck_msdosfs: checker }) do |observations|
        assert_equal FsimgSpec::Observation.disagrees('fsck_msdosfs -n exited 8: "dirty | ** image.img | Filesystem too small"'),
                     observations.fetch("fsck")
      end
    end
  end

  # A disagreement an exception's tool, cluster counts and diagnostic
  # all match is excused and names the exception; any other observation
  # stays as it was.
  def test_a_disagreement_a_documented_exception_matches_is_excused
    exceptions = [Fat::OracleException.new(id: "t.crossed", tool: "fsck", clusters: [4085, 5005], diagnostic: /crossed/,
                                           reason: "a test")]
    crossed = FsimgSpec::Observation.disagrees("fsck_msdosfs -n exited 8: crossed chain")
    excused = ->(observations, count) { Fat::Oracles.excused(observations, cluster_count: count, exceptions:) }

    assert_equal crossed.excused_by("t.crossed"), excused.call({ "fsck" => crossed }, 4085).fetch("fsck")
    assert_equal "excused", excused.call({ "fsck" => crossed }, 5005).fetch("fsck").state
    unexcused = [[{ "fsck" => crossed }, 4084], [{ "mtools" => crossed }, 4085],
                 [{ "fsck" => FsimgSpec::Observation.disagrees("exited 8: lost cluster") }, 4085],
                 [{ "fsck" => crossed }, FsimgSpec::Undefined.new(reason: "no BPB")], [{ "fsck" => FsimgSpec::Observation.agrees }, 4085]]
    unexcused.each { |observations, count| assert_equal observations, excused.call(observations, count) }
  end

  # An exception names the exact counts a defect was observed at: a range
  # of counts, or none, is refused when the exception is built.
  def test_an_exception_names_exact_cluster_counts
    [[4070..4100], 4070..4100, [], [4085.0]].each do |clusters|
      assert_raises(FsimgSpec::HarnessError, clusters.inspect) do
        Fat::OracleException.new(id: "t.range", tool: "mtools", clusters:, diagnostic: //, reason: "a test")
      end
    end
  end

  # The read-back is a verdict at every cluster count: no exception
  # excuses its disagreement, not even at the variant cutovers or within
  # the 16 clusters automatic sizing keeps from them, and none excuses
  # fsck.fat's there.
  def test_no_exception_excuses_the_read_back_or_fsck_fat_near_the_cutovers
    observations = { "mtools" => FsimgSpec::Observation.disagrees("mtools lacks file \"a\""),
                     "fsck" => FsimgSpec::Observation.disagrees('fsck.fat -n exited 1: "FATs differ"') }
    counts = [4069, 4070, 4084, 4085, 4086, 4100, 4101, 65_509, 65_510, 65_524, 65_525, 65_526, 65_540, 65_541]

    assert_empty counts.reject { Fat::Oracles.excused(observations, cluster_count: _1) == observations }
  end

  # FreeBSD's fsck_msdosfs takes the variant one cluster off section
  # 3.5's cutovers: it decodes a FAT16 volume of 4085 clusters with a
  # 12-bit table and refuses a FAT32 volume of 65525. Those disagreements
  # alone are excused, a misread chain in any phase among them, and the
  # refusal after its warning about the sector after FSInfo. A boot
  # sector diagnostic before the FAT is read or before the refusal is not
  # excused, nor is fsck.fat's at the same counts, nor fsck_msdosfs's at
  # any other count.
  def test_fsck_msdosfs_is_excused_only_at_its_off_by_one_cutovers
    observed = lambda do |detail, count|
      Fat::Oracles.excused({ "fsck" => FsimgSpec::Observation.disagrees(detail) }, cluster_count: count).fetch("fsck").exception_id
    end
    crossed = 'fsck_msdosfs -n exited 8: "** out/created.img | ** Phase 1 - Read FAT and checking connectivity | ' \
              'Cluster 8 crossed another chain at 7 | Truncate? no"'
    lost = 'fsck_msdosfs -n exited 8: "** image.img | ** Phase 1 - Read FAT and checking connectivity | ' \
           '** Phase 2 - Checking Directories | ** Phase 3 - Checking for Lost Files | Lost cluster chain at cluster 4096"'
    boot = 'fsck_msdosfs -n exited 8: "** out/created.img | Invalid signature in boot block: 0000"'
    refused = 'fsck_msdosfs -n exited 8: "** out/created.img | Filesystem too small (65525 clusters) for FAT32 partition"'
    warned = refused.sub("img | ", "img | Invalid signature in fsinfo block | Fix? no | ")
    backup = refused.sub("img | ", "img | backup (block 6) mismatch with primary bootblock | ")

    assert_equal ["fsck_msdosfs.fat16_4085", "fsck_msdosfs.fat16_4085", nil, nil, nil],
                 [observed.call(crossed, 4085), observed.call(lost, 4085), observed.call(boot, 4085), observed.call(crossed, 4086),
                  observed.call(crossed.sub("fsck_msdosfs", "fsck.fat"), 4085)]
    assert_equal ["fsck_msdosfs.fat32_65525", "fsck_msdosfs.fat32_65525", nil, nil, nil],
                 [observed.call(refused, 65_525), observed.call(warned, 65_525), observed.call(backup, 65_525),
                  observed.call(crossed, 65_525), observed.call(warned, 65_524)]
  end

  # Only the checker's and the read-back's disagreements count against
  # an image; an excused one, fatcat's and a tool that does not apply do
  # not.
  def test_only_the_verdict_tools_disagreements_count
    observations = {
      "fsck" => FsimgSpec::Observation.disagrees("fsck.fat -n exited 1: \"x\""),
      "mtools" => FsimgSpec::Observation.disagrees("mtools lacks file \"a\"").excused_by("t.exception"),
      "fatcat" => FsimgSpec::Observation.disagrees("fatcat lacks file \"a\"")
    }

    assert_equal ['fsck: fsck.fat -n exited 1: "x"'], Fat::Oracles.disagreements(observations)
    assert_empty Fat::Oracles.disagreements(observations.except("fsck"))
  end

  # Every exception is documented: a unique id, a verdict tool, cluster
  # counts and a diagnostic to match, and a reason that cites where the
  # tool departs from the specification.
  def test_every_exception_is_documented
    exceptions = Fat::Oracles::EXCEPTIONS

    assert_equal exceptions.size, exceptions.map(&:id).uniq.size
    exceptions.each do |exception|
      assert_includes Fat::Oracles::VERDICT_TOOLS, exception.tool, exception.id
      assert_kind_of Regexp, exception.diagnostic, exception.id
      assert_match(%r{https://|specs/}, exception.reason, exception.id)
    end
  end

  private

  # Writes a builder image of tree into a case directory, plants the
  # named files beside it, and yields the observations of it as the known
  # tree known describes it. unavailable stands in for the host's locale
  # lookup failing with that message.
  def observe_image(tree:, known: tree, parameters: { clusters: 5000 }, tools: @tools, plant: {}, unavailable: nil)
    Dir.mktmpdir do |root|
      directory = File.join(root, "case")
      Dir.mkdir(directory)
      image = File.join(directory, "image.img")
      Fat::Builder.write(image, parameters:, tree:, max_bytes: CEILINGS.image)
      plant.each { |name, text| File.write(File.join(directory, name), text) }
      shell = FsimgSpec::OracleShell.new(home: File.join(root, "oracle-home"), environment: ENV.to_h, tools:)
      shell.define_singleton_method(:locale_lookup) { [nil, unavailable] } if unavailable
      model = Fat::Model.decode(FsimgSpec::ImageView.open(image), tree: FsimgSpec::KnownTree.of(known))
      yield Fat::Oracles.observe(shell, image: "image.img", chdir: directory, model:)
    end
  end

  def skip_without(*names)
    missing = names.reject { _1 == :fsck ? @tools[:fsck_fat] || @tools[:fsck_msdosfs] : @tools[_1] }
    skip "#{missing.join(", ")} unavailable" unless missing.empty?
  end
end
