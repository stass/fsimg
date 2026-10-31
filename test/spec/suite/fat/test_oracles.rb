# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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

  # A checker's detail names the program and quotes the lines it wrote
  # that are not blank, stderr first.
  def test_a_failing_checker_is_quoted_by_program_and_its_lines
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

  # FreeBSD's fsck_msdosfs misreads a FAT16 volume of 4085 clusters and
  # refuses a FAT32 volume of 65525: at each it does not apply and never
  # runs, whatever it would print, and the observation gives the blind
  # spot's reason, which cites readboot. fsck.fat runs there, under
  # either checker's slot and before fsck_msdosfs when both are
  # configured, and fsck_msdosfs runs at every neighbouring count.
  def test_fsck_msdosfs_does_not_apply_at_the_cutovers
    Dir.mktmpdir do |stubs|
      ran = File.join(stubs, "ran")
      msdosfs, fat = %w[fsck_msdosfs fsck.fat].map do |name|
        path = File.join(stubs, name)
        File.write(path, "#!/bin/sh\ntouch '#{ran}'\necho 'Cluster 8 crossed another chain at 7'\nexit 8\n")
        File.chmod(0o700, path)
        path
      end
      # What the checker did on a volume of clusters, as "blind" or "ran".
      checked = lambda do |clusters, fsck_msdosfs:, fsck_fat: nil|
        File.unlink(ran) if File.exist?(ran)
        observe_image(tree: [], parameters: { clusters: }, tools: @tools.merge(fsck_fat:, fsck_msdosfs:)) do
          fsck = _1.fetch("fsck")
          case [fsck.state, File.exist?(ran)]
          in ["not_applicable", false] if fsck.detail.include?("readboot") then "blind"
          in ["disagrees", true] then "ran"
          end
        end
      end

      [4085, 65_525].each do |cutover|
        observed = {
          "fsck_msdosfs at the cutover" => checked.(cutover, fsck_msdosfs: msdosfs),
          "fsck.fat at the cutover" => checked.(cutover, fsck_msdosfs: fat),
          "fsck.fat configured beside fsck_msdosfs" => checked.(cutover, fsck_msdosfs: msdosfs, fsck_fat: fat),
          "fsck_msdosfs one below" => checked.(cutover - 1, fsck_msdosfs: msdosfs),
          "fsck_msdosfs one above" => checked.(cutover + 1, fsck_msdosfs: msdosfs)
        }

        assert_equal({ "fsck_msdosfs at the cutover" => "blind" }, observed.reject { _2 == "ran" }, cutover)
      end
    end
  end

  # A blind spot names the exact counts the checker's source misreads: a
  # range of counts, or none, is refused when the blind spot is built.
  def test_a_blind_spot_names_exact_cluster_counts
    [[4070..4100], 4070..4100, [], [4085.0]].each do |clusters|
      assert_raises(FsimgSpec::HarnessError, clusters.inspect) do
        Fat::CheckerBlindSpot.new(program: "fsck_msdosfs", clusters:, reason: "a test")
      end
    end
  end

  # Only the checker's and the read-back's disagreements count against
  # an image; fatcat's and a tool that does not apply do not.
  def test_only_the_verdict_tools_disagreements_count
    observations = {
      "fsck" => FsimgSpec::Observation.disagrees("fsck.fat -n exited 1: \"x\""),
      "mtools" => FsimgSpec::Observation.not_applicable("mtools drops long names beyond the BMP"),
      "fatcat" => FsimgSpec::Observation.disagrees("fatcat lacks file \"a\"")
    }

    assert_equal ['fsck: fsck.fat -n exited 1: "x"'], Fat::Oracles.disagreements(observations)
    assert_empty Fat::Oracles.disagreements(observations.except("fsck"))
  end

  # Every blind spot is documented: a program, exact cluster counts no
  # other blind spot of that program names, and a reason that cites
  # where the program departs from the specification.
  def test_every_blind_spot_is_documented
    blind_spots = Fat::Oracles::BLIND_SPOTS
    covered = blind_spots.flat_map { |blind| blind.clusters.map { [blind.program, _1] } }

    assert_equal covered.size, covered.uniq.size
    blind_spots.each do |blind|
      assert_kind_of String, blind.program
      assert_match(%r{https://.*section \d}, blind.reason, blind.program)
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
