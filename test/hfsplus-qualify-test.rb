# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "rbconfig"
require "tmpdir"
require_relative "../tools/hfsplus/qualify"

# The integration wrapper supplies these build artifacts, not source paths.
HFSPLUS_QUALIFY_GENERATOR = File.expand_path(ARGV.shift || "./hfsplus-write-gen").freeze
HFSPLUS_QUALIFY_FSIMG = File.expand_path(ARGV.shift || "./fsimg").freeze

class HfsPlusQualifyTest < Minitest::Test
  # These attach refusals identify hosts that cannot provide disk devices.
  NATIVE_ATTACH_RESTRICTION = /\AFAIL: cannot attach .*attach failed - (Operation not permitted|Device not configured)/

  def test_content_matches_fixture_byte_samples_across_periods
    # hwf_byte uses provider id 1000 + spec index. Pin offsets around its
    # 251-byte period so a wrong provider id or truncated period is visible.
    {0 => [184, 197, 171, 184, 24], 7 => [52, 65, 39, 52, 143]}.each do |index, bytes|
      actual = HfsPlusQualify.content(index, 510)
      assert_equal(bytes, [0, 1, 250, 251, 509].map { actual.getbyte(_1) }, "spec index #{index}")
      assert_equal(510, actual.bytesize)
      assert_equal(Encoding::BINARY, actual.encoding)
    end
    assert_equal("".b, HfsPlusQualify.content(0, 0))
  end

  def test_native_path_decomposes_and_orders_bmp_marks
    assert_equal("/cafe\u0301".b, HfsPlusQualify.native_path("/caf\u00e9"))
    assert_equal("a\u0f71\u0f72\u0f72".b, HfsPlusQualify.native_path("a\u0f72\u0f73"))
  end

  def test_native_path_keeps_supplementary_marks_unordered
    first = "a\u0300\u{1d165}"
    second = "a\u{1d165}\u0300"
    assert_equal(first.b, HfsPlusQualify.native_path(first))
    assert_equal(second.b, HfsPlusQualify.native_path(second))
    refute_equal(HfsPlusQualify.native_path(first), HfsPlusQualify.native_path(second))
  end

  def test_differences_reports_missing_unexpected_and_changed_fields
    expected = {"/missing" => view(size: 1), "/changed" => view(size: 2)}
    observed = {"/extra" => view(size: 3), "/changed" => view(size: 4)}
    assert_equal(['"/missing" missing', '"/extra" unexpected', '"/changed" size 4, not 2'],
      HfsPlusQualify.differences(expected, observed, %i[kind size]))
  end

  def test_differences_ignores_nil_expected_fields
    assert_empty(HfsPlusQualify.differences({"/file" => view(size: nil)},
      {"/file" => view(size: 99)}, %i[kind size mode]))
  end

  def test_collision_pairs_follow_each_variants_comparator
    %w[hfsplus hfsx].each do |variant|
      pairs = HfsPlusQualify.collision_pairs(variant)
      assert_equal(15, pairs.size, variant)
      assert_equal(15, pairs.map { [_1.first, _1.second] }.uniq.size, variant)
      expected = [true] * 8 + [variant == "hfsplus"] * 5 + [false] * 2
      assert_equal(expected, pairs.map(&:collide), variant)
      case_pair = pairs.find { _1.first == "Zeta" }
      assert_equal("zeta", case_pair.second)
      assert_equal(variant == "hfsplus", case_pair.collide)
    end
  end

  def test_length_names_reach_255_utf16_units_in_each_spelling
    names = HfsPlusQualify.length_names
    assert_equal(names.size, names.uniq.size)
    assert(names.all? { utf16_units(_1) <= 255 })
    ["x" * 255, "#{"\u00e9" * 127}x", "#{"\u{1f600}" * 127}x",
     "#{"\uac00" * 127}x", "\ud55c" * 85].each do |name|
      assert_includes(names, name)
      assert_equal(255, utf16_units(name))
    end
  end

  def test_overlong_names_are_exactly_256_utf16_units
    assert_equal(5, HfsPlusQualify::OVERLONG_NAMES.size)
    assert_includes(HfsPlusQualify::OVERLONG_NAMES, "\ud55c" * 85 + "x")
    assert_includes(HfsPlusQualify::OVERLONG_NAMES, "\uac00" * 128)
    HfsPlusQualify::OVERLONG_NAMES.each do |name|
      assert_equal(256, utf16_units(name), name.inspect)
    end
  end

  def test_smallest_empty_native_cycle_passes_all_seven_steps
    return native_skip("not macOS") unless RUBY_PLATFORM.include?("darwin")
    return native_skip("hdiutil is absent") unless program?("hdiutil")

    assert(File.executable?(HFSPLUS_QUALIFY_GENERATOR), "built generator is required")
    assert(File.executable?(HFSPLUS_QUALIFY_FSIMG), "built fsimg is required")
    Dir.mktmpdir("hfsplus-qualify-", Dir.pwd) do |root|
      script = File.expand_path("../tools/hfsplus/qualify.rb", __dir__)
      out, status = Open3.capture2e(RbConfig.ruby, script,
        "--generator", HFSPLUS_QUALIFY_GENERATOR, "--fsimg", HFSPLUS_QUALIFY_FSIMG,
        "--root", root, "--only", "^hfsplus-smallest-empty$")
      rows = JSON.parse(File.read(File.join(root, "results.json")))
      assert_equal(1, rows.size, out)
      row = rows.fetch(0)
      assert_equal("hfsplus-smallest-empty", row.fetch("image"))
      assert_equal("pass", row.fetch("generate"), out)
      assert_equal("pass", row.fetch("detach"), out)
      if NATIVE_ATTACH_RESTRICTION.match?(row.fetch("fsck_before"))
        return native_skip(row.fetch("fsck_before"))
      end
      assert(status.success?, out)
      steps = %w[generate fsck_before mount modify fsck_after remount fsimg]
      assert_equal(["pass"] * 7, row.values_at(*steps), out)
    end
  end

  def test_main_returns_failure_for_modify_or_detach_failure_and_zero_for_pass
    [{modify: "FAIL: no room"}, {detach: "FAIL: busy"}, {}].product([false, true]).each do |changes, keep|
      Dir.mktmpdir do |root|
        row = passing_row.merge(changes)
        image_path = File.join(root, fixture_image.name, "image.img")
        cycle = Object.new
        cycle.define_singleton_method(:run) do
          File.binwrite(image_path, "image bytes")
          row
        end
        with_stub(HfsPlusQualify, :selected, replacement: [fixture_image]) do
          with_stub(HfsPlusQualify, :leftovers, replacement: []) do
            with_stub(HfsPlusQualify::Cycle, :new, replacement: cycle) do
              args = main_args(root) + (keep ? ["--keep"] : [])
              capture_io { assert_equal(changes.empty? ? 0 : 1, HfsPlusQualify.main(args)) }
            end
          end
        end
        if keep || !changes.empty?
          assert_equal("image bytes", File.binread(image_path))
        else
          refute_path_exists(image_path)
        end
      end
    end
  end

  def test_main_removes_old_results_before_first_cycle_is_interrupted
    Dir.mktmpdir do |root|
      results = File.join(root, "results.json")
      File.write(results, '[{"image":"old"}]')
      cycle = Object.new
      cycle.define_singleton_method(:run) { raise Interrupt }
      with_stub(HfsPlusQualify, :selected, replacement: [fixture_image]) do
        with_stub(HfsPlusQualify, :leftovers, replacement: []) do
          with_stub(HfsPlusQualify::Cycle, :new, replacement: cycle) do
            capture_io { assert_raises(Interrupt) { HfsPlusQualify.main(main_args(root)) } }
          end
        end
      end
      refute_path_exists(results)
    end
  end

  def test_main_returns_failure_for_bad_regexp_and_unknown_option
    [["--only", "["], ["--unknown-option"]].each do |args|
      _, errors = capture_io { assert_equal(1, HfsPlusQualify.main(args)) }
      assert_match(/qualify\.rb:/, errors)
    end
  end

  def test_results_preserve_first_row_when_second_cycle_is_interrupted
    Dir.mktmpdir do |root|
      row = passing_row
      first = Object.new
      first.define_singleton_method(:run) { row }
      second = Object.new
      second.define_singleton_method(:run) { raise Interrupt }
      cycles = [first, second]
      with_stub(HfsPlusQualify, :selected, replacement: [fixture_image, fixture_image.with(name: "second")]) do
        with_stub(HfsPlusQualify, :leftovers, replacement: []) do
          with_stub(HfsPlusQualify::Cycle, :new, replacement: ->(*) { cycles.shift }) do
            capture_io { assert_raises(Interrupt) { HfsPlusQualify.main(main_args(root)) } }
          end
        end
      end
      assert_equal([JSON.parse(row.to_json)], JSON.parse(File.read(File.join(root, "results.json"))))
    end
  end

  def test_main_refuses_failed_attachment_inventory_and_already_attached_images
    [->(*) { raise HfsPlusQualify::Error, "hdiutil info failed" }, ["attached.img"]].each do |inventory|
      Dir.mktmpdir do |root|
        results = File.join(root, "results.json")
        File.write(results, "old results")
        with_stub(HfsPlusQualify, :selected, replacement: [fixture_image]) do
          with_stub(HfsPlusQualify, :leftovers, replacement: inventory) do
            _, errors = capture_io { assert_equal(1, HfsPlusQualify.main(main_args(root))) }
            assert_match(/hdiutil info failed|already attached/, errors)
            assert_equal("old results", File.read(results))
          end
        end
      end
    end
  end

  def test_leftovers_raises_when_hdiutil_info_fails
    calls = []
    log = Object.new
    log.define_singleton_method(:run) do |*args|
      calls << args
      ["inventory unavailable", false]
    end
    Dir.mktmpdir do |root|
      with_stub(HfsPlusCapture::Log, :new, replacement: log) do
        error = assert_raises(HfsPlusQualify::Error) { HfsPlusQualify.leftovers(root) }
        assert_includes(error.message, "inventory unavailable")
      end
    end
    assert_equal([["hdiutil", "info"]], calls)
  end

  class SingleStepModification < HfsPlusQualify::Modification
    def initialize(step:, **options)
      @step = step
      super(**options)
    end

    private

    def steps = [@step]
  end

  def test_enospc_is_refused_only_on_limited_volumes_and_removes_partial_folder
    [false, true].each do |limited|
      Dir.mktmpdir do |root|
        tree = {"/original" => view(size: 7)}.freeze
        step = HfsPlusQualify::Modification::Step.new(name: "create", folder: "partial", run: ->(_) {
          Dir.mkdir(File.join(root, "partial"))
          File.write(File.join(root, "partial/file"), "partial data")
          raise Errno::ENOSPC
        })
        modification = modification_for(root: root, tree: tree, step: step, limited: limited)
        if limited
          outcome = modification.run
          assert_equal(tree, outcome.tree)
          assert_equal({"/original" => view(size: 7)}, tree)
          assert_empty(outcome.completed)
          assert_match(/\Acreate: /, outcome.refused.fetch(0))
          refute_path_exists(File.join(root, "partial"))
        else
          assert_raises(Errno::ENOSPC) { modification.run }
        end
      end
    end
  end

  def test_enotsup_outside_link_is_not_a_limited_volume_refusal
    Dir.mktmpdir do |root|
      step = HfsPlusQualify::Modification::Step.new(name: "create", folder: nil, run: ->(_) { raise Errno::ENOTSUP })
      modification = modification_for(root: root, tree: {}, step: step, limited: true)
      assert_raises(Errno::ENOTSUP) { modification.run }
    end
  end

  def test_link_enotsup_becomes_link_refused_and_only_limited_volumes_record_it
    [false, true].each do |limited|
      Dir.mktmpdir do |root|
        modification = nil
        step = HfsPlusQualify::Modification::Step.new(name: "hard-link", folder: nil,
          run: ->(_) { modification.send(:link, "existing", "added") })
        modification = modification_for(root: root, tree: {}, step: step, limited: limited)
        with_stub(File, :link, replacement: ->(*) { raise Errno::ENOTSUP }) do
          assert_raises(HfsPlusQualify::Modification::LinkRefused) { modification.send(:link, "existing", "added") }
          if limited
            outcome = modification.run
            assert_equal({}, outcome.tree)
            assert_empty(outcome.completed)
            assert_match(/\Ahard-link: /, outcome.refused.fetch(0))
          else
            assert_raises(HfsPlusQualify::Modification::LinkRefused) { modification.run }
          end
        end
      end
    end
  end

  def test_room_profiles_accept_matching_outcomes
    {none: [[], ["create: full"]], unlink_only: [["unlink-original"], ["create: full"]],
     partial: [["create"], ["fragment: full"]], ample: [["create"], []]}.each do |room, (completed, refused)|
      assert_nil(HfsPlusQualify.check_room(room, outcome(completed: completed, refused: refused)), room.to_s)
    end
  end

  def test_room_profiles_reject_mismatched_outcomes
    [[:none, ["create"], []], [:unlink_only, ["create"], ["full"]],
     [:unlink_only, ["unlink-original", "create"], ["full"]], [:unlink_only, [], ["full"]],
     [:partial, [], ["full"]], [:partial, ["create"], []], [:ample, ["create"], ["full"]]].each do |room, completed, refused|
      assert_raises(HfsPlusQualify::Error, room.to_s) do
        HfsPlusQualify.check_room(room, outcome(completed: completed, refused: refused))
      end
    end
  end

  def test_uncompressed_files_are_allowed_only_with_partial_room
    {none: [], unlink_only: ["unlink-original"], partial: ["create"], ample: ["create"]}.each do |room, completed|
      refused = room == :ample ? [] : ["compress: full"]
      result = outcome(completed: completed, refused: refused, uncompressed: ["inline"])
      if room == :partial
        HfsPlusQualify.check_room(room, result)
      else
        assert_raises(HfsPlusQualify::Error) { HfsPlusQualify.check_room(room, result) }
      end
    end
  end

  def test_differences_reports_private_folders_and_unknown_root_additions
    ["/.HFS+ Private Directory Data\r", "/\u2400\u2400\u2400\u2400HFS+ Private Data", "/unknown"].each do |path|
      assert_equal(["#{path.inspect} unexpected"], HfsPlusQualify.differences({}, {path => view(size: 0)}, [:kind]))
    end
  end

  def test_fsimg_entries_excludes_only_observed_native_additions_and_keeps_private_folder
    Dir.mktmpdir do |root|
      paths = ["/.Trashes", "/.Trashes/child", "/.DS_Store", "/.HFS+ Private Directory Data\r"]
      records = paths.map do |path|
        {type: "entry", path: path, kind: "directory", mode: 0o755, uid: 0, gid: 0, extra_data: nil,
         **%i[created modified changed accessed].to_h { [_1, {seconds: "0"}] }}
      end
      executable = fake_fsimg(root: root, output: "#{records.map(&:to_json).join("\n")}\n")
      cycle = HfsPlusQualify::Cycle.new(fixture_image, HfsPlusQualify::Tools.new(generator: "unused", fsimg: executable), root)
      cycle.instance_variable_set(:@native_additions, [".Trashes"])
      entries = cycle.send(:fsimg_entries)
      assert_equal(paths.drop(2), entries.keys)
      error = assert_raises(HfsPlusQualify::Error) { cycle.send(:report_differences, "listing", entries, [:kind]) }
      assert_includes(error.message, "unexpected")
      assert_includes(error.message, "Private Directory Data")
    end
  end

  def test_list_rejects_empty_selection_in_regular_and_large_catalog_modes
    [[], ["--large-catalog"]].each do |mode|
      with_stub(HfsPlusQualify, :map_node_entries, replacement: []) do
        _, errors = capture_io do
          assert_equal(1, HfsPlusQualify.main([*mode, "--only", "^nothing$", "--list"]))
        end
        assert_match(/selects no image/, errors)
      end
    end
  end

  def test_list_large_catalog_selects_only_catalog_image
    with_stub(HfsPlusQualify, :map_node_entries, replacement: []) do
      output, errors = capture_io do
        assert_equal(0, HfsPlusQualify.main(["--large-catalog", "--only", "catalog", "--list"]))
      end
      assert_equal("hfsplus-catalog-4gib\n", output)
      assert_empty(errors)
    end
  end

  def test_list_includes_35_images_and_cli_defaults
    with_stub(HfsPlusQualify, :map_node_entries, replacement: []) do
      output, errors = capture_io { assert_equal(0, HfsPlusQualify.main(["--list"])) }
      assert_equal(35, output.lines.size)
      assert_includes(output.lines.map(&:chomp), "hfsplus-cli-defaults")
      assert_empty(errors)
    end
  end

  def test_image_rooms_match_available_modification_space
    with_stub(HfsPlusQualify, :map_node_entries, replacement: []) do
      HfsPlusQualify::VARIANTS.each do |variant|
        HfsPlusQualify.images(variant).each do |image|
          expected = if image.name.include?("smallest-") then :none
                     elsif image.name.end_with?("nearly-full") then :unlink_only
                     elsif image.name.end_with?("low-space") then :partial
                     else :ample
                     end
          assert_equal(expected, image.room, image.name)
        end
      end
    end
    assert_equal(:none, HfsPlusQualify.cli_image.room)
    assert(HfsPlusQualify.cli_image.cli)
  end

  def test_attempt_records_any_step_failure_but_propagates_interrupt
    Dir.mktmpdir do |root|
      cycle = HfsPlusQualify::Cycle.new(fixture_image, nil, root)
      [HfsPlusFields::Error, KeyError, NoMethodError].each do |error|
        cycle.define_singleton_method(:broken_step) { raise error, "broken field" }
        assert_equal("FAIL: broken field", cycle.send(:attempt, :broken_step))
      end
      cycle.define_singleton_method(:broken_step) { raise Interrupt }
      assert_raises(Interrupt) { cycle.send(:attempt, :broken_step) }
    end
  end

  def test_cycle_releases_attachments_when_a_step_is_interrupted
    Dir.mktmpdir do |root|
      cycle = HfsPlusQualify::Cycle.new(fixture_image, nil, root)
      releases = []
      attachments = Object.new
      attachments.define_singleton_method(:release) { releases << :release }
      cycle.instance_variable_set(:@attachments, attachments)
      cycle.define_singleton_method(:generate) { raise Interrupt }
      assert_raises(Interrupt) { cycle.run }
      assert_equal([:release], releases)
    end
  end

  def test_refused_entries_rejects_success_wrong_reason_and_leftover_probe
    [{code: 0, output: "expected refusal", leaves_probe: false},
     {code: 1, output: "unrelated failure", leaves_probe: false},
     {code: 1, output: "expected refusal", leaves_probe: true}].each do |scenario|
      Dir.mktmpdir do |root|
        generator = fake_generator(root: root, **scenario)
        cycle = cycle_for(root: root, generator: generator)
        error = assert_raises(HfsPlusQualify::Error, scenario.inspect) do
          cycle.send(:refused_entries, [HfsPlusQualify::Entry.of("file", "/bad")], "bad", /expected refusal/)
        end
        assert_match(/the writer did not refuse/, error.message)
      end
    end
  end

  def test_refused_entries_accepts_matching_refusal_without_probe
    Dir.mktmpdir do |root|
      generator = fake_generator(root: root, code: 1, output: "expected refusal", leaves_probe: false)
      cycle = cycle_for(root: root, generator: generator)
      cycle.send(:refused_entries, [HfsPlusQualify::Entry.of("file", "/bad")], "bad", /expected refusal/)
      refute_path_exists(File.join(root, "refused.img"))
      assert_includes(cycle.instance_variable_get(:@log).to_s, 'refused "bad"')
    end
  end

  def test_refused_pair_rejects_second_name_that_is_refused_alone
    Dir.mktmpdir do |root|
      generator = fake_generator(root: root, code: 1, output: "equivalent to a sibling", leaves_probe: false)
      cycle = cycle_for(root: root, generator: generator)
      pair = HfsPlusQualify::Pair.new(first: "Zeta", second: "zeta", collide: true)
      error = assert_raises(HfsPlusQualify::Error) { cycle.send(:refused_pair, pair) }
      assert_match(/generator exit 1/, error.message)
      spec = File.read(File.join(root, "spec.tsv")).lines
      assert_equal(1, spec.size)
      assert_equal("/zeta".unpack1("H*"), spec.first.split.last)
      refute_path_exists(File.join(root, "refused.img"))
    end
  end

  def test_check_marks_rejects_unexpected_and_missing_marks
    [{output: "-rw-r--r--@ 1 \"/unexpected\"\n", extra: false},
     {output: "-rw-r--r-- 1 \"/file\"\n", extra: true}].each do |scenario|
      Dir.mktmpdir do |root|
        executable = fake_fsimg(root: root, output: scenario.fetch(:output))
        cycle = cycle_for(root: root, fsimg: executable)
        cycle.instance_variable_set(:@tree, {"/file" => view(size: 1).with(extra: scenario.fetch(:extra))})
        error = assert_raises(HfsPlusQualify::Error) { cycle.send(:check_marks) }
        assert_match(/t -v marks/, error.message)
      end
    end
  end

  def test_check_delivery_rejects_changed_file_bytes
    Dir.mktmpdir do |root|
      executable = fake_fsimg(root: root, output: "other bytes")
      cycle = cycle_for(root: root, fsimg: executable)
      expected = view(size: 14).with(sha256: Digest::SHA256.hexdigest("expected bytes"))
      cycle.instance_variable_set(:@tree, {"/file" => expected})
      error = assert_raises(HfsPlusQualify::Error) { cycle.send(:check_delivery) }
      assert_match(%r{x -O "/file" delivers}, error.message)
    end
  end

  def test_check_modified_rejects_missing_extents_and_attribute_records
    {extents_leaf_records: /no extents overflow record/,
     attributes_leaf_records: /no attribute record/}.each do |field, message|
      Dir.mktmpdir do |root|
        cycle = cycle_for(root: root)
        after = {extents_leaf_records: 1, attributes_leaf_records: 1}.merge(field => 0)
        cycle.instance_variable_set(:@facts, {fields_after: after})
        error = assert_raises(HfsPlusQualify::Error, field.to_s) { cycle.send(:check_modified) }
        assert_match(message, error.message)
      end
    end
  end

  def test_fsck_rejects_findings_nonzero_status_and_missing_clean_verdict
    clean = "** The volume fixture appears to be OK.\n"
    [{output: "Invalid record count\n#{clean}", ok: true, message: /findings: Invalid record count/},
     {output: clean, ok: false, message: /did not pass/},
     {output: "** Checking catalog file.\n", ok: true, message: /did not pass/}].each do |scenario|
      Dir.mktmpdir do |root|
        cycle = fsck_cycle(root: root, output: scenario.fetch(:output), ok: scenario.fetch(:ok))
        error = assert_raises(HfsPlusQualify::Error) { cycle.send(:fsck, :fsck_before_output) }
        assert_match(scenario.fetch(:message), error.message)
      end
    end
  end

  def test_fsck_accepts_clean_verdict
    Dir.mktmpdir do |root|
      output = "** Checking catalog file.\n** The volume fixture appears to be OK.\n"
      cycle = fsck_cycle(root: root, output: output, ok: true)
      assert_nil(cycle.send(:fsck, :fsck_before_output))
      assert_equal("** The volume fixture appears to be OK.",
        cycle.instance_variable_get(:@facts).fetch(:fsck_before_output))
    end
  end

  def test_report_differences_rejects_changed_field
    Dir.mktmpdir do |root|
      cycle = cycle_for(root: root)
      cycle.instance_variable_set(:@tree, {"/file" => view(size: 2)})
      error = assert_raises(HfsPlusQualify::Error) do
        cycle.send(:report_differences, "metadata", {"/file" => view(size: 3)}, [:size])
      end
      assert_equal('metadata: "/file" size 3, not 2 (1 differences)', error.message)
    end
  end

  def test_release_reports_forced_detach
    Dir.mktmpdir do |root|
      cycle = cycle_for(root: root)
      log = HfsPlusCapture::Log.new
      log.note("$ hdiutil detach /dev/disk9 -force")
      attachments = Object.new
      attachments.define_singleton_method(:release) {}
      cycle.instance_variable_set(:@log, log)
      cycle.instance_variable_set(:@attachments, attachments)
      assert_equal("FAIL: forced 1 detach", cycle.send(:release))
    end
  end

  def test_release_reports_attachment_release_error
    Dir.mktmpdir do |root|
      cycle = cycle_for(root: root)
      attachments = Object.new
      attachments.define_singleton_method(:release) { raise HfsPlusCapture::Error, "detach busy" }
      cycle.instance_variable_set(:@attachments, attachments)
      assert_equal("FAIL: detach busy", cycle.send(:release))
    end
  end

  def test_materialize_and_cli_tree_preserve_content_and_source_times_with_cli_defaults
    Dir.mktmpdir do |root|
      entries = [HfsPlusQualify::Entry.of("directory", "/dir", mode: 0o700),
                 HfsPlusQualify::Entry.of("file", "/dir/file", size: 517, mode: 0o600),
                 HfsPlusQualify::Entry.of("symlink", "/link", target: "dir/file", mode: 0o777)]
      source = File.join(root, "source")
      HfsPlusQualify.materialize(entries, source)
      assert_equal(HfsPlusQualify.content(1, 517), File.binread(File.join(source, "dir/file")))
      assert(File.directory?(File.join(source, "dir")))
      assert_equal("dir/file", File.readlink(File.join(source, "link")))
      tree = HfsPlusQualify.cli_tree(entries, source)
      entries.zip([0o755, 0o644, 0o755]).each do |entry, mode|
        seen = tree.fetch(entry.path)
        stat = File.lstat(File.join(source, entry.path))
        assert_equal(entry.kind, seen.kind)
        assert_equal(mode, seen.mode)
        assert_equal(0, seen.uid)
        assert_equal(0, seen.gid)
        assert_nil(seen.changed)
        assert_nil(seen.accessed)
        assert_equal(stat.birthtime.to_i, seen.created)
        assert_equal(stat.mtime.to_i, seen.modified)
        assert_equal(entry.modified, seen.modified)
      end
    end
  end

  def test_materialize_rejects_fifo_entries
    Dir.mktmpdir do |root|
      assert_raises(NoMatchingPatternError) do
        HfsPlusQualify.materialize([HfsPlusQualify::Entry.of("fifo", "/pipe")], File.join(root, "source"))
      end
    end
  end

  private

  def cycle_for(root:, generator: "unused", fsimg: "unused") =
    HfsPlusQualify::Cycle.new(fixture_image, HfsPlusQualify::Tools.new(generator: generator, fsimg: fsimg), root)

  def fake_generator(root:, code:, output:, leaves_probe:)
    executable = File.join(root, "generator-double")
    File.write(executable, <<~RUBY)
      #!#{RbConfig.ruby}
      File.binwrite(ARGV.fetch(0), "probe") if #{leaves_probe}
      puts #{output.dump}
      exit #{code}
    RUBY
    File.chmod(0o755, executable)
    executable
  end

  def fake_fsimg(root:, output:)
    executable = File.join(root, "fsimg-double")
    File.write(executable, "#!#{RbConfig.ruby}\nprint #{output.dump}\n")
    File.chmod(0o755, executable)
    executable
  end

  def fsck_cycle(root:, output:, ok:)
    cycle = cycle_for(root: root)
    attachments = Object.new
    attachments.define_singleton_method(:with) { |*_, **_, &block| block.call("/dev/disk9") }
    log = Object.new
    log.define_singleton_method(:run) do |*args, **options|
      raise "unexpected fsck command: #{args.inspect}" unless args == ["fsck_hfs", "-fn", "/dev/rdisk9"]
      raise "missing fsck timeout" unless options == {timeout: HfsPlusCapture::FSCK_TIMEOUT}

      [output, ok]
    end
    cycle.instance_variable_set(:@attachments, attachments)
    cycle.instance_variable_set(:@log, log)
    cycle
  end

  # Override one singleton method for the block, restoring its original owner
  # even when the block raises. Replacements are return values or callables.
  def with_stub(object, name, replacement:)
    singleton = object.singleton_class
    original = object.method(name)
    owned = singleton.instance_methods(false).include?(name)
    singleton.define_method(name) do |*args, **kwargs, &block|
      replacement.respond_to?(:call) ? replacement.call(*args, **kwargs, &block) : replacement
    end
    begin
      yield
    ensure
      if owned
        singleton.define_method(name, original)
      else
        singleton.remove_method(name)
      end
    end
  end

  def fixture_image = HfsPlusQualify::Image.of(name: "fixture", variant: "hfsplus", block: 4096, volume_bytes: nil, entries: [])

  def passing_row = {image: "fixture", **HfsPlusQualify::Cycle::STEPS.to_h { [_1, "pass"] },
                    refused: [], uncompressed: [], facts: {}, detach: "pass"}

  def main_args(root) = ["--root", root, "--generator", "unused-generator", "--fsimg", "unused-fsimg"]

  def outcome(completed:, refused:, uncompressed: []) =
    HfsPlusQualify::Outcome.new(tree: {}, completed: completed, refused: refused, uncompressed: uncompressed)

  def modification_for(root:, tree:, step:, limited:) =
    SingleStepModification.new(step: step, root: root, tree: tree, limited: limited,
      scratch: root, block: 4096, log: HfsPlusCapture::Log.new)

  def view(size:) = HfsPlusQualify::View.of(kind: "file", size: size)

  def utf16_units(name) = name.unicode_normalize(:nfd).encode(Encoding::UTF_16BE).bytesize / 2

  def program?(name)
    ENV.fetch("PATH", "").split(File::PATH_SEPARATOR).any? do |directory|
      path = File.join(directory, name)
      File.file?(path) && File.executable?(path)
    end
  end

  def native_skip(reason)
    skip("HFS Plus native qualification: #{reason}")
  end
end
