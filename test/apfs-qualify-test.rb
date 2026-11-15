# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "tmpdir"
require_relative "../tools/apfs/qualify"

# The pure parts of the native qualification tool: manifests, comparison,
# fsck and `--formats` judgments, image layouts, step planning, the
# modifications on a host directory, and the fsimg step against a double. The
# attach and mount steps need macOS and are exercised by running the tool.
class ApfsQualifyTest < Minitest::Test
  View = ApfsQualify::View

  E3_MANIFEST = <<~JSON
    [{"path": ".", "kind": "directory", "size": 416, "mode": 493, "mtime_ns": 5},
     {"path": ".fseventsd", "kind": "directory", "size": 96, "mode": 448},
     {"path": ".fseventsd/0000", "kind": "file", "size": 1, "mode": 420, "sha256": "ff"},
     {"path": "dir", "kind": "directory", "size": 64, "mode": 493, "mtime_ns": 10, "btime_ns": 9},
     {"path": "dir/a", "kind": "file", "size": 3, "mode": 3565, "mtime_ns": 12, "btime_ns": 11, "sha256": "aa"},
     {"path": "ln", "kind": "symlink", "size": 3, "mode": 493, "target": "dir"},
     {"path": "pipe", "kind": "fifo", "size": 0, "mode": 416},
     {"path": "tty", "kind": "char_device", "size": 0, "mode": 416}]
  JSON

  E5_MANIFEST = <<~NDJSON
    {"path":"/","kind":"dir","mode":"40755","mtime_ns":1}
    {"path":"/x","kind":"dir","mode":"40755","mtime_ns":2,"birthtime_ns":1}
    {"path":"/x/f","kind":"file","size":4,"mode":"100644","mtime_ns":3,"birthtime_ns":2,"sha256":"bb"}
    {"path":"/x/l","kind":"symlink","size":1,"mode":"120755","sha256":"cc"}
    {"path":"/.Trashes","kind":"dir","mode":"40755"}
    {"path":"/blk","kind":"blk","mode":"60600"}
  NDJSON

  def test_e3_manifest_gives_views_by_path_without_the_root_and_native_additions
    tree = ApfsQualify.parse_manifest(E3_MANIFEST)
    assert_equal ["/dir", "/dir/a", "/ln", "/pipe", "/tty"], tree.keys
    assert_equal View.of(kind: "file", size: 3, mode: 3565, created: 11, modified: 12, sha256: "aa"), tree["/dir/a"]
    assert_equal "dir".b, tree["/ln"].target
    assert_equal "char", tree["/tty"].kind
    assert_nil tree["/dir"].size
  end

  def test_ndjson_manifest_reads_permission_bits_of_the_octal_mode_and_ignores_a_link_checksum
    tree = ApfsQualify.parse_manifest(E5_MANIFEST)
    assert_equal ["/x", "/x/f", "/x/l", "/blk"], tree.keys
    assert_equal View.of(kind: "file", size: 4, mode: 0o644, created: 2, modified: 3, sha256: "bb"), tree["/x/f"]
    assert_equal "directory", tree["/x"].kind
    assert_equal "cc", tree["/x/l"].sha256
    assert_equal "block", tree["/blk"].kind
  end

  def test_manifest_without_a_required_field_or_not_json_is_an_error
    assert_raises(ApfsQualify::Error) { ApfsQualify.parse_manifest('[{"kind": "file"}]') }
    assert_raises(ApfsQualify::Error) { ApfsQualify.parse_manifest("[not json") }
    assert_raises(ApfsQualify::Error) { ApfsQualify.parse_manifest("[1]") }
    assert_raises(ApfsQualify::Error) { ApfsQualify.parse_manifest(%([{"path": "\xff", "kind": "file", "size": 0}]).b) }
    repeated = %([{"path": "a", "kind": "file", "size": 0}, {"path": "./a", "kind": "file", "size": 0}])
    assert_raises(ApfsQualify::Error) { ApfsQualify.parse_manifest(repeated) }
    ["../x", "a/../../x", "a//b", "a/./b"].each do |path|
      assert_raises(ApfsQualify::Error, path) { ApfsQualify.parse_manifest(%([{"path": "#{path}", "kind": "file", "size": 0}])) }
    end
  end

  def test_find_manifest_prefers_the_given_one_then_the_one_beside_the_image_unless_skipped
    Dir.mktmpdir do |dir|
      image = File.join(dir, "image.img")
      assert_nil ApfsQualify.find_manifest(image, given: nil, skip: false)
      File.write(File.join(dir, "native-manifest.ndjson"), "")
      assert_equal File.join(dir, "native-manifest.ndjson"), ApfsQualify.find_manifest(image, given: nil, skip: false)
      assert_equal "/given", ApfsQualify.find_manifest(image, given: "/given", skip: false)
      assert_nil ApfsQualify.find_manifest(image, given: "/given", skip: true)
    end
  end

  def test_differences_reports_missing_unexpected_and_changed_fields
    expected = { "/a".b => View.of(kind: "file", size: 1), "/b".b => View.of(kind: "file", size: 2) }
    observed = { "/a".b => View.of(kind: "file", size: 9), "/c".b => View.of(kind: "file", size: 1) }
    assert_equal ['"/b" missing', '"/c" unexpected', '"/a" size 9, not 1'], ApfsQualify.differences(expected, observed, [:size])
  end

  def test_differences_skips_expected_nil_and_unlisted_observed_fields
    expected = { "/a".b => View.of(kind: "file", size: nil, sha256: "aa", target: "t".b) }
    observed = { "/a".b => View.of(kind: "file", size: 5, sha256: nil, target: nil) }
    assert_empty ApfsQualify.differences(expected, observed, %i[size sha256 target])
  end

  def test_view_of_a_host_tree_reports_kinds_sizes_targets_and_checksums
    Dir.mktmpdir do |dir|
      File.binwrite(File.join(dir, "f"), "abc")
      File.symlink("f", File.join(dir, "l"))
      file = ApfsQualify.view_of(File.join(dir, "f"), digests: true)
      assert_equal [3, Digest::SHA256.hexdigest("abc")], [file.size, file.sha256]
      assert_equal "f".b, ApfsQualify.view_of(File.join(dir, "l"), digests: false).target
      assert_equal Digest::SHA256.hexdigest("f"), ApfsQualify.view_of(File.join(dir, "l"), digests: true).sha256
      assert_nil ApfsQualify.view_of(File.join(dir, "f"), digests: false).sha256
    end
  end

  def test_view_of_record_joins_seconds_and_nanoseconds
    record = { "kind" => "directory", "mode" => 0o40755, "created" => { "seconds" => "2", "nanoseconds" => 5 },
               "modified" => { "seconds" => "3", "nanoseconds" => 7 } }
    view = ApfsQualify.view_of_record(record)
    assert_equal [0o755, 2_000_000_005, 3_000_000_007], [view.mode, view.created, view.modified]
    unset = ApfsQualify.view_of_record(record.merge("mode" => nil, "created" => nil))
    assert_equal [nil, nil], [unset.mode, unset.created]
  end

  def test_fsck_findings_are_the_lines_that_are_not_progress
    clean = <<~OUT
      ** Checking the container superblock.
         Checking the checkpoint with transaction ID 5.
      ** Checking volume /dev/rdisk7s1.
         The volume FSIMG was formatted by newfs_apfs (1) and last modified by apfs_kext (1).
      ** The volume /dev/rdisk7s1 with UUID 39A2 appears to be OK.
      ** Verifying allocated space.
      ** The container /dev/rdisk6 appears to be OK.
    OUT
    assert_empty ApfsQualify.fsck_findings(clean)
    assert_equal ["error: extent 5 is not allocated"], ApfsQualify.fsck_findings("#{clean}error: extent 5 is not allocated\n")
  end

  def test_formats_support_needs_apfs_readable_and_verifiable
    table = ->(row) { "Format Read Write Verify\nfat12    yes   yes    yes\n#{row}" }
    assert ApfsQualify.supports_apfs?(table.call("apfs     yes   yes    yes\n"))
    assert ApfsQualify.supports_apfs?(table.call("apfs     yes   no     yes\n"))
    refute ApfsQualify.supports_apfs?(table.call("apfs     yes   yes    no\n"))
    refute ApfsQualify.supports_apfs?(table.call(""))
  end

  def test_layout_of_names_udif_gpt_and_raw_images_and_refuses_others
    Dir.mktmpdir do |dir|
      raw = write_image(dir, "raw", 32 => "NXSB")
      gpt = write_image(dir, "gpt", 512 => "EFI PART")
      udif = write_image(dir, "udif", 32 => "NXSB", 4096 - 512 => "koly")
      assert_equal %w[raw gpt dmg], [raw, gpt, udif].map { ApfsQualify.layout_of(_1) }
      assert_raises(ApfsQualify::Error) { ApfsQualify.layout_of(write_image(dir, "other")) }
      File.binwrite(File.join(dir, "short"), "NX")
      assert_raises(ApfsQualify::Error) { ApfsQualify.layout_of(File.join(dir, "short")) }
    end
  end

  def test_image_name_is_the_capture_directory_for_image_img
    assert_equal "apfs-newfs-1-x", ApfsQualify.image_name("/c/apfs-newfs-1-x/image.img")
    assert_equal "e13-gpt_udrw", ApfsQualify.image_name("/c/e13-gpt udrw.dmg")
  end

  def test_images_refuse_duplicate_names_and_one_manifest_for_several_images
    Dir.mktmpdir do |dir|
      first = write_image(dir, "same.img", 32 => "NXSB")
      second = File.join(dir, "sub", "same.img")
      FileUtils.mkdir_p(File.dirname(second))
      FileUtils.cp(first, second)
      assert_raises(ApfsQualify::Error) { ApfsQualify.images([first, second], manifest: nil, skip_manifest: true) }
      assert_raises(ApfsQualify::Error) { ApfsQualify.images([first, second], manifest: "m", skip_manifest: false) }
      assert_raises(ApfsQualify::Error) { ApfsQualify.images([File.join(dir, "missing")], manifest: nil, skip_manifest: true) }
    end
  end

  def test_plan_says_which_steps_skip_their_work
    with = ApfsQualify::Image.new(name: "a", source: "a", layout: "raw", manifest: "m", tree: {})
    without = with.with(manifest: nil, tree: nil)
    assert_empty ApfsQualify.plan(with, fsimg: nil)
    assert_equal [:mount], ApfsQualify.plan(without, fsimg: nil).keys
    assert_equal [:fsimg], ApfsQualify.plan(with, fsimg: "no --fsimg given").keys
  end

  def test_fsimg_skip_names_a_missing_binary_and_a_binary_without_apfs
    assert_match(/no --fsimg/, ApfsQualify.fsimg_skip(ApfsQualify::Tools.new(fsimg: nil), ""))
    assert_match(/does not list apfs/, ApfsQualify.fsimg_skip(ApfsQualify::Tools.new(fsimg: "/x/fsimg"), "Format Read\n"))
    assert_nil ApfsQualify.fsimg_skip(ApfsQualify::Tools.new(fsimg: "/x/fsimg"), "apfs yes yes yes\n")
  end

  def test_modifications_leave_exactly_the_tree_they_report
    Dir.mktmpdir do |dir|
      File.binwrite(File.join(dir, "one"), "1")
      File.binwrite(File.join(dir, "two"), "22")
      File.binwrite(File.join(dir, "three"), "333")
      tree = ApfsQualify.parse_manifest(%([{"path":"one","kind":"file","size":1,"sha256":"#{Digest::SHA256.hexdigest('1')}"},
        {"path":"two","kind":"file","size":2,"sha256":"#{Digest::SHA256.hexdigest('22')}"},
        {"path":"three","kind":"file","size":3,"sha256":"#{Digest::SHA256.hexdigest('333')}"}]))
      outcome = ApfsQualify::Modification.new(root: dir, tree: tree, tolerate_full: false, large_bytes: (3 << 20) / 2).run
      assert_empty outcome.refused
      assert_includes outcome.completed, "large-file"
      assert_includes outcome.completed, "unlink-original"
      assert_includes outcome.tree.keys, "/q-renamed".b
      assert_includes outcome.tree.keys, "/q-rename/to/moved".b
      observed = Dir.glob("**/*", File::FNM_DOTMATCH, base: dir).reject { _1.end_with?("/.") || _1 == "." }
                    .to_h { ["/#{_1}".b, ApfsQualify.view_of(File.join(dir, _1), digests: true)] }
      assert_empty ApfsQualify.differences(outcome.tree, observed, %i[kind size sha256 target])
      assert_equal 1536 << 10, observed["/q-large/big".b].size
      assert_equal "ok", outcome.failure || "ok"
    end
  end

  def test_a_full_volume_refusal_is_recorded_and_removes_the_step_folder_only_when_tolerated
    refusing = Class.new(ApfsQualify::Modification) do
      private

      def create(_tree) = FileUtils.mkdir_p(File.join(@root, "q-create")).then { raise Errno::ENOSPC }
    end
    Dir.mktmpdir do |dir|
      outcome = refusing.new(root: dir, tree: {}, tolerate_full: true, large_bytes: 1).run
      assert_match(/\Acreate: /, outcome.refused.first)
      refute_includes outcome.completed, "create"
      refute File.exist?(File.join(dir, "q-create"))
      assert_includes outcome.completed, "append"
      failed = refusing.new(root: File.join(dir, "again").tap { Dir.mkdir(_1) }, tree: {}, tolerate_full: false, large_bytes: 1).run
      assert_match(/\Acreate: /, failed.failure)
      assert_empty failed.completed
    end
  end

  def test_modifications_do_not_follow_a_symbolic_link_from_the_image_out_of_the_volume
    Dir.mktmpdir do |dir|
      volume = File.join(dir, "volume")
      outside = File.join(dir, "outside")
      [volume, outside].each { Dir.mkdir(_1) }
      File.binwrite(File.join(outside, "gone"), "keep")
      File.symlink(outside, File.join(volume, "q-unlink"))
      outcome = ApfsQualify::Modification.new(root: volume, tree: {}, tolerate_full: false, large_bytes: 1).run
      assert_match(/\Aunlink: /, outcome.failure)
      assert_equal ["keep"], Dir.children(outside).map { File.read(File.join(outside, _1)) }
      assert_equal %w[create append rename mkdir-rmdir], outcome.completed
    end
  end

  def test_a_read_only_volume_is_skipped_only_for_a_udif_file
    refusing = lambda do |layout|
      cycle = cycle_for(Dir.tmpdir, layout: layout, tree: {})
      cycle.define_singleton_method(:mounted) { |readonly:| raise Errno::EROFS }
      cycle.send(:attempt, :modify)
    end
    assert_match(/\Askipped: the image is read-only/, refusing.call("dmg"))
    assert_match(/\AFAIL: Read-only file system/, refusing.call("gpt"))
  end

  def test_steps_after_a_failed_mount_name_the_missing_baseline
    Dir.mktmpdir do |dir|
      cycle = cycle_for(dir, manifest: nil)
      assert_match(/\AFAIL: no baseline/, cycle.send(:attempt, :modify))
      assert_match(/\AFAIL: no baseline/, cycle.send(:attempt, :remount))
    end
  end

  def test_attempt_records_a_failure_as_the_step_result_but_propagates_an_interrupt
    Dir.mktmpdir do |dir|
      cycle = cycle_for(dir)
      cycle.define_singleton_method(:fsck_before) { raise ApfsQualify::Error, "fsck findings\nsecond line" }
      cycle.define_singleton_method(:fsck_after) { raise Interrupt }
      assert_equal "FAIL: fsck findings\nsecond line", cycle.send(:attempt, :fsck_before)
      assert_raises(Interrupt) { cycle.send(:attempt, :fsck_after) }
      assert_match(/\Askipped: no --fsimg/, cycle.send(:attempt, :fsimg))
    end
  end

  def test_mount_without_a_manifest_takes_the_first_view_as_the_baseline_and_remount_compares_it
    Dir.mktmpdir do |dir|
      cycle = cycle_for(dir, manifest: nil)
      views = [{ "/a".b => View.of(kind: "file", size: 1) }, { "/a".b => View.of(kind: "file", size: 2) }]
      cycle.define_singleton_method(:read_only) { |&block| block.call }
      cycle.define_singleton_method(:observe) { |digests:| views.shift }
      assert_match(/\Askipped: no manifest/, cycle.send(:attempt, :mount))
      assert_match(/\AFAIL: remount: "\/a" size 2, not 1/, cycle.send(:attempt, :remount))
    end
  end

  def test_fsimg_step_verifies_lists_and_delivers_through_the_double
    Dir.mktmpdir do |dir|
      listing = { "type" => "entry", "path" => "/f", "kind" => "file", "size_bytes" => 3, "mode" => 0o100644,
                  "created" => { "seconds" => "1", "nanoseconds" => 2 }, "modified" => { "seconds" => "3", "nanoseconds" => 4 } }
      fsimg = fake_fsimg(dir, listing: listing, bytes: "abc")
      tree = { "/f".b => View.of(kind: "file", size: 3, mode: 0o644, created: 1_000_000_002, modified: 3_000_000_004,
                                 sha256: Digest::SHA256.hexdigest("abc")) }
      cycle = cycle_for(dir, fsimg: fsimg, tree: tree, skip_fsimg: nil)
      assert_equal "pass", cycle.send(:attempt, :fsimg)
      cycle.instance_variable_set(:@tree, tree.transform_values { _1.with(sha256: "0" * 64) })
      assert_match(/\AFAIL: x -O "\/f" delivers/, cycle.send(:attempt, :fsimg))
    end
  end

  def test_root_inside_a_git_work_tree_is_refused_before_it_is_created
    inside = File.join(__dir__, "apfs-qualify-root-that-must-not-exist")
    assert_raises(ApfsQualify::Error) { ApfsQualify.private_directory(inside) }
    refute File.exist?(inside)
  end

  def test_root_must_be_private_to_the_user
    Dir.mktmpdir do |dir|
      shared = File.join(dir, "shared")
      Dir.mkdir(shared, 0o777)
      File.chmod(0o777, shared)
      assert_raises(ApfsQualify::Error) { ApfsQualify.private_directory(shared) }
    end
  end

  def test_acl_detects_an_access_control_entry_on_a_directory
    Dir.mktmpdir do |dir|
      refute ApfsQualify.acl?(dir)
      assert system(*["/bin/chmod", "+a", "everyone allow add_file", dir])
      assert ApfsQualify.acl?(dir)
    end
  end

  def test_private_directory_check_rejects_an_acl_even_with_mode_0700
    Dir.mktmpdir do |dir|
      File.chmod(0o700, dir)
      assert ApfsQualify.private?(dir)
      assert system(*["/bin/chmod", "+a", "everyone allow add_file", dir])
      refute ApfsQualify.private?(dir)
    end
  end

  def test_private_directory_check_rejects_mode_0777
    Dir.mktmpdir do |dir|
      File.chmod(0o777, dir)
      refute ApfsQualify.private?(dir)
    end
  end

  def test_private_directory_check_rejects_a_symbolic_link_to_a_private_directory
    Dir.mktmpdir do |dir|
      Dir.mktmpdir do |target|
        File.chmod(0o700, target)
        link = File.join(dir, "link")
        File.symlink(target, link)
        refute ApfsQualify.private?(link)
      end
    end
  end

  def test_root_rejects_a_directory_with_an_acl
    Dir.mktmpdir do |dir|
      assert system(*["/bin/chmod", "+a", "everyone allow add_file", dir])
      assert_raises(ApfsQualify::Error) { ApfsQualify.private_directory(dir) }
    end
  end

  def test_main_refuses_a_source_inside_root_and_an_empty_image_list
    Dir.mktmpdir do |dir|
      inside = write_image(File.join(dir, "root"), "image.img", 32 => "NXSB")
      assert_equal 1, quietly { ApfsQualify.main(["--root", File.join(dir, "root"), "--no-manifest", inside]) }
      assert_equal 1, quietly { ApfsQualify.main(["--root", File.join(dir, "root")]) }
    end
  end

  def test_list_prints_each_image_with_its_layout_and_skipped_steps
    Dir.mktmpdir do |dir|
      image = write_image(dir, "image.img", 32 => "NXSB")
      out, = capture_io { assert_equal 0, ApfsQualify.main(["--list", "--no-manifest", image]) }
      name, layout, manifest, skipped = out.chomp.split("\t")
      assert_equal [File.basename(dir).gsub(/[^\w.+-]/, "_"), "raw", "-"], [name, layout, manifest]
      assert_match(/mount: no manifest.*fsimg: no --fsimg given/, skipped)
    end
  end

  def test_large_bytes_must_be_positive
    assert_raises(OptionParser::InvalidArgument) { ApfsQualify.parse(["--large-bytes", "-1"]) }
    assert_equal 5, ApfsQualify.parse(["--large-bytes", "5", "image"]).first[:large_bytes]
  end

  def test_a_skipped_step_is_not_a_failed_image
    row = ApfsQualify::STEPS.to_h { [_1, "pass"] }.merge(detach: "pass", mount: "skipped: baseline")
    assert ApfsQualify.passed?(row)
    refute ApfsQualify.passed?(row.merge(remount: "FAIL: x"))
  end

  def test_result_table_has_a_row_per_image_and_a_column_per_step
    row = { image: "i", layout: "raw", **ApfsQualify::STEPS.to_h { [_1, "pass"] }, detach: "pass", refused: ["create: full"] }
    lines = ApfsQualify.result_table([row]).lines
    assert_equal 3, lines.size
    assert_equal lines[0].count("|"), lines[2].count("|")
    assert_includes lines[2], "| create |"
  end

  private

  def write_image(dir, name, **at)
    FileUtils.mkdir_p(dir)
    path = File.join(dir, name)
    bytes = "\0".b * 4096
    at.each { |offset, text| bytes[offset, text.bytesize] = text.b }
    File.binwrite(path, bytes)
    path
  end

  def cycle_for(dir, manifest: "m", layout: "raw", fsimg: nil, tree: nil, skip_fsimg: "no --fsimg given")
    image = ApfsQualify::Image.new(name: "fixture", source: "unused", layout: layout, manifest: manifest, tree: tree)
    ApfsQualify::Cycle.new(image, ApfsQualify::Tools.new(fsimg: fsimg), dir, mounts: dir,
                           options: { tolerate_full: false, large_bytes: 1 }, skip_fsimg: skip_fsimg)
  end

  # A stand-in for fsimg: W succeeds, t prints the listing, x delivers the bytes.
  def fake_fsimg(dir, listing:, bytes:)
    executable = File.join(dir, "fsimg-double")
    File.write(executable, <<~RUBY)
      #!#{RbConfig.ruby}
      case ARGV.first
      when "t" then puts #{JSON.generate(listing).dump}
      when "x" then print #{bytes.dump}
      end
    RUBY
    File.chmod(0o755, executable)
    executable
  end

  def quietly
    status = nil
    capture_io { status = yield }
    status
  end
end
