# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "tmpdir"
require_relative "../tools/apfs/outputs"

# Source descriptions, manifests and writer arguments need no mounted image.
# Case-only names stay in the mtree and hex-named content files so these tests
# also run on a case-insensitive scratch volume.
class ApfsOutputsTest < Minitest::Test
  def test_vis_preserves_printable_ascii_except_backslash
    printable = (33..126).reject { _1 == 92 }.map(&:chr).join
    assert_equal printable, ApfsOutputs.vis(printable)
    assert_equal "a\\040b", ApfsOutputs.vis("a b")
    assert_equal "a\\134b", ApfsOutputs.vis("a\\b")
  end

  def test_vis_escapes_each_utf8_byte_as_three_octal_digits
    assert_equal "pipe-\\303\\251", ApfsOutputs.vis("pipe-\u00e9")
    assert_equal "\\346\\274\\242\\360\\237\\230\\200", ApfsOutputs.vis("\u6f22\u{1f600}")
  end

  def test_extras_come_from_tree_descriptions_and_every_nonempty_tree_has_a_fifo
    assert_empty ApfsOutputs.extras("empty")
    ApfsOutputs::TREES.each do |name, tree|
      assert_instance_of ApfsOutputs::Tree, tree
      assert_respond_to tree.build, :call
      assert_same tree.extras, ApfsOutputs.extras(name)
      next if name == "empty"

      assert_includes tree.extras.map { [_1.path, _1.kind, _1.bytes] }, ["pipe", "fifo", nil], name
    end
  end

  def test_only_mixed_trees_have_the_fifo_under_names
    trees = ApfsOutputs::TREES.keys.select do |name|
      ApfsOutputs.extras(name).any? { _1.path == "names/pipe-\u00e9" && _1.kind == "fifo" }
    end
    assert_equal %w[mixed mixed-case], trees.sort
  end

  def test_only_mixed_case_has_case_directory_entries
    ApfsOutputs::TREES.keys.reject { _1 == "mixed-case" }.each do |name|
      refute ApfsOutputs.extras(name).any? { _1.path == "case" || _1.path.start_with?("case/") }, name
    end
  end

  def test_extra_parents_exist_in_the_host_tree_or_as_extra_directories
    Dir.mktmpdir("apfs-outputs-") do |sources|
      %w[mixed mixed-case deep empty].each do |tree|
        source = ApfsOutputs.build_tree(sources, tree)
        extras = ApfsOutputs.extras(tree)
        extra_directories = extras.select { _1.kind == "dir" }.map(&:path)
        extras.each do |extra|
          parent = File.dirname(extra.path)
          next if parent == "."

          assert File.directory?(File.join(source, parent)) || extra_directories.include?(parent),
                 "#{tree}: missing parent #{parent} for #{extra.path}"
        end
      end
    end
  end

  def test_mixed_extras_are_two_fifos
    assert_equal [["pipe", "fifo", nil], ["names/pipe-\u00e9", "fifo", nil]],
                 ApfsOutputs.extras("mixed").map { [_1.path, _1.kind, _1.bytes] }
  end

  def test_mixed_case_adds_a_directory_and_three_distinct_files
    extras = ApfsOutputs.extras("mixed-case")
    assert_equal ApfsOutputs.extras("mixed"), extras.take(2)
    assert_equal ["case", "dir", nil], extras.drop(2).first.then { [_1.path, _1.kind, _1.bytes] }
    assert_equal [["case/Readme", "file", "Readme\n"],
                  ["case/README", "file", "README\nREADME\n"],
                  ["case/readme", "file", "readme\nreadme\nreadme\n"]],
                 extras.drop(3).map { [_1.path, _1.kind, _1.bytes] }
  end

  def test_mtree_describes_fifo_modes_times_and_hex_named_file_contents
    lines = ApfsOutputs.mtree("mixed-case").lines.map(&:chomp)
    assert_equal "#mtree", lines.first
    assert_includes lines, "./pipe type=fifo mode=0640 time=#{ApfsOutputs::EPOCH}.0"
    assert_includes lines, "./names/pipe-\\303\\251 type=fifo mode=0640 time=#{ApfsOutputs::EPOCH}.0"
    assert_includes lines, "./case type=dir mode=0755 time=#{ApfsOutputs::EPOCH}.0"
    ApfsOutputs.extras("mixed-case").select(&:bytes).each do |extra|
      hex = extra.path.bytes.map { format("%02x", _1) }.join
      assert_equal hex, ApfsOutputs.content_name(extra)
      assert_includes lines, "./#{extra.path} type=file mode=0644 time=#{ApfsOutputs::EPOCH}.0 size=#{extra.bytes.bytesize} contents=#{hex}"
    end
  end

  def test_fifo_record_has_no_file_fields_or_birth_time
    record = ApfsOutputs.extra_record(ApfsOutputs.extras("deep").first)
    assert_equal({ path: "pipe", kind: "fifo", mode: 0o640, mtime_ns: ApfsOutputs::EPOCH * 1_000_000_000 }, record)
    %i[size sha256 btime_ns].each { refute record.key?(_1) }
  end

  def test_file_record_has_its_byte_size_digest_and_fixed_modification_time
    extra = ApfsOutputs.extras("mixed-case").find { _1.path == "case/README" }
    record = ApfsOutputs.extra_record(extra)
    assert_equal "file", record.fetch(:kind)
    assert_equal extra.bytes.bytesize, record.fetch(:size)
    assert_equal Digest::SHA256.hexdigest("README\nREADME\n"), record.fetch(:sha256)
    assert_equal ApfsOutputs::EPOCH * 1_000_000_000, record.fetch(:mtime_ns)
    refute record.key?(:btime_ns)
  end

  def test_build_tree_stamps_all_host_objects_and_keeps_case_only_bytes_separate
    Dir.mktmpdir("apfs-outputs-") do |sources|
      source = ApfsOutputs.build_tree(sources, "mixed-case")
      assert_equal File.join(sources, "mixed-case"), source
      assert File.directory?(source)
      assert File.file?("#{source}.mtree")
      assert_equal ApfsOutputs.mtree("mixed-case"), File.read("#{source}.mtree")
      assert File.directory?("#{source}.bytes")
      paths = host_paths(source).map { File.join(source, _1) }.push(source)
      assert paths.any? { File.symlink?(_1) }, "fixture must exercise symlink timestamps"
      paths.each { assert_equal Time.at(ApfsOutputs::EPOCH), File.lstat(_1).mtime, _1 }

      contents = ApfsOutputs.extras("mixed-case").select(&:bytes).map do |extra|
        path = File.join("#{source}.bytes", ApfsOutputs.content_name(extra))
        assert File.file?(path), path
        File.binread(path).tap { assert_equal extra.bytes.b, _1 }
      end
      assert_equal 3, contents.uniq.size
      assert_raises(ApfsOutputs::Error) { ApfsOutputs.build_tree(sources, "mixed-case") }
    end
  end

  def test_build_tree_rejects_an_existing_bytes_directory
    Dir.mktmpdir("apfs-outputs-") do |sources|
      bytes = File.join(sources, "empty.bytes")
      Dir.mkdir(bytes)
      marker = File.join(bytes, "keep")
      File.write(marker, "existing contents")

      assert_raises(ApfsOutputs::Error) { ApfsOutputs.build_tree(sources, "empty") }
      assert_equal "existing contents", File.read(marker)
    end
  end

  def test_manifest_covers_host_paths_and_extras_and_parses_to_qualification_kinds
    Dir.mktmpdir("apfs-outputs-") do |root|
      source = ApfsOutputs.build_tree(root, "mixed-case")
      records = ApfsOutputs.manifest(source, "mixed-case")
      paths = host_paths(source)
      expected = paths + ApfsOutputs.extras("mixed-case").map(&:path)
      assert_equal expected.sort, records.map { _1.fetch(:path) }.sort
      assert records.none? { _1.fetch(:path).start_with?("/") }

      by_path = records.to_h { [_1.fetch(:path), _1] }
      paths.each do |path|
        kind = File.lstat(File.join(source, path)).ftype
        assert_equal kind == "link" ? "symlink" : kind, by_path.fetch(path).fetch(:kind), path
      end
      assert_equal "t" * 1023, by_path.fetch("links/longest").fetch(:target)
      records.select { _1.fetch(:kind) == "file" }.each do |record|
        extra = ApfsOutputs.extras("mixed-case").find { _1.path == record.fetch(:path) }
        bytes = extra ? extra.bytes : File.binread(File.join(source, record.fetch(:path)))
        assert_equal Digest::SHA256.hexdigest(bytes), record.fetch(:sha256), record.fetch(:path)
      end

      parsed = ApfsQualify.parse_manifest(JSON.generate(records))
      assert_equal expected.map { "/#{_1}".b }.sort, parsed.keys.sort
      assert_equal %w[directory fifo file symlink], parsed.values.map(&:kind).uniq.sort
      # The synthetic directory uses mtree's "dir" alias, accepted by qualify.
      assert_equal "directory", parsed.fetch("/case").kind
      assert_equal "fifo", parsed.fetch("/pipe").kind
      assert_equal "t" * 1023, parsed.fetch("/links/longest").target
    end
  end

  def test_write_image_passes_stack_sizing_case_label_and_ordered_sources
    Dir.mktmpdir("apfs-outputs-") do |root|
      source = ApfsOutputs.build_tree(root, "empty")
      outputs = File.join(root, "outputs")
      Dir.mkdir(outputs)
      fsimg = fake_fsimg(root)
      rows = [ApfsOutputs.row("automatic", "empty"),
              ApfsOutputs.row("sized", "empty", stack: "apfs/gpt", size: 4096),
              ApfsOutputs.row("free", "empty", stack: "apfs/gpt/dmg", free: 1024, case_sensitive: true),
              ApfsOutputs.row("sized-free", "empty", size: 8192, free: 2048, case_sensitive: true)]
      rows.each do |row|
        image = ApfsOutputs.write_image(fsimg, outputs, row, source)
        assert_equal File.join(outputs, row.name, "image.#{ApfsOutputs::EXTENSIONS.fetch(row.stack)}"), image
        assert File.file?(image)
        expected = ["c", "-W", "-p", "-F", row.stack, "-f", image]
        expected += ["-s", row.size.to_s] if row.size
        expected += ["--free-space=#{row.free}"] if row.free
        expected += ["-o", "case=#{row.case_sensitive ? 'sensitive' : 'insensitive'}", "-L", row.name,
                     "-C", source, ".", "-C", "#{source}.bytes", "@#{source}.mtree"]
        assert_equal expected, File.readlines("#{fsimg}.argv", chomp: true)
        manifest = File.join(File.dirname(image), "native-manifest.json")
        assert File.file?(manifest)
        assert_equal JSON.parse(JSON.generate(ApfsOutputs.manifest(source, row.tree))), JSON.parse(File.read(manifest))
      end
    end
  end

  def test_create_argv_includes_sizing_case_label_and_ordered_sources
    Dir.mktmpdir("apfs-outputs-") do |root|
      fsimg = File.join(root, "fsimg")
      source = File.join(root, "source with spaces")
      image = File.join(root, "image.dmg")
      cases = [
        { row: ApfsOutputs.row("automatic", "empty"), sizing: [], personality: "insensitive" },
        { row: ApfsOutputs.row("sized", "empty", size: 4096), sizing: ["-s", "4096"], personality: "insensitive" },
        { row: ApfsOutputs.row("free", "empty", free: 1024, case_sensitive: true),
          sizing: ["--free-space=1024"], personality: "sensitive" },
        { row: ApfsOutputs.row("both", "empty", stack: "apfs/gpt/dmg", size: 8192, free: 2048, case_sensitive: true),
          sizing: ["-s", "8192", "--free-space=2048"], personality: "sensitive" }
      ]
      cases.each do |test_case|
        row = test_case.fetch(:row)
        expected = [fsimg, "c", "-W", "-p", "-F", row.stack, "-f", image, *test_case.fetch(:sizing),
                    "-o", "case=#{test_case.fetch(:personality)}", "-L", row.name,
                    "-C", source, ".", "-C", "#{source}.bytes", "@#{source}.mtree"]
        assert_equal expected, ApfsOutputs.create_argv(fsimg, row, source, image), row.name
      end
    end
  end

  def test_write_image_rejects_an_existing_row_directory_before_running_fsimg
    Dir.mktmpdir("apfs-outputs-") do |root|
      source = ApfsOutputs.build_tree(root, "empty")
      outputs = File.join(root, "outputs")
      Dir.mkdir(outputs)
      row = ApfsOutputs.row("existing", "empty")
      Dir.mkdir(File.join(outputs, row.name))
      fsimg = fake_fsimg(root)

      assert_raises(ApfsOutputs::Error) { ApfsOutputs.write_image(fsimg, outputs, row, source) }
      refute File.exist?("#{fsimg}.invoked"), "fsimg must not run when the row already exists"
      assert_empty Dir.children(File.join(outputs, row.name))
    end
  end

  def test_write_image_raises_when_fsimg_exits_unsuccessfully
    Dir.mktmpdir("apfs-outputs-") do |root|
      source = ApfsOutputs.build_tree(root, "empty")
      outputs = File.join(root, "outputs")
      Dir.mkdir(outputs)
      fsimg = File.join(root, "failing-fsimg")
      File.write(fsimg, "#!/bin/sh\nexit 1\n")
      File.chmod(0o755, fsimg)
      row = ApfsOutputs.row("failure", "empty")
      assert_raises(ApfsOutputs::Error) { ApfsOutputs.write_image(fsimg, outputs, row, source) }
      refute File.exist?(File.join(outputs, row.name, "native-manifest.json"))
    end
  end

  def test_case_sensitive_output_rows_enable_case_sensitivity
    rows = ApfsOutputs::OUTPUTS.to_h { [_1.name, _1] }
    %w[minimum-cs small-cs multichunk-cs].each { assert rows.fetch(_1).case_sensitive, _1 }
  end

  def test_huge_output_uses_the_huge_tree_and_eight_gibibytes
    row = ApfsOutputs::OUTPUTS.find { _1.name == "huge" }
    refute_nil row
    assert_equal "huge", row.tree
    assert_equal 8 * (1 << 30), row.size
  end

  def test_output_rows_have_unique_names_and_known_trees_and_stacks
    rows = ApfsOutputs::OUTPUTS
    assert_equal rows.size, rows.map(&:name).uniq.size
    rows.each do |row|
      assert ApfsOutputs::TREES.key?(row.tree), row.name
      assert ApfsOutputs::EXTENSIONS.key?(row.stack), row.name
      assert row.full, row.name if row.size.nil?
    end
  end

  def test_row_defaults_to_full_only_for_automatic_sizing
    assert ApfsOutputs.row("automatic", "empty").full
    refute ApfsOutputs.row("sized", "empty", size: 4096).full
    assert ApfsOutputs.row("free", "empty", free: 1024).full
    refute ApfsOutputs.row("override", "empty", full: false).full
    assert ApfsOutputs.row("sized-override", "empty", size: 4096, full: true).full
  end

  def test_private_child_creates_a_directory_with_mode_0700
    Dir.mktmpdir("apfs-outputs-") do |root|
      child = ApfsOutputs.private_child(root, "sources")
      assert_equal File.join(root, "sources"), child
      assert File.lstat(child).directory?
      assert_equal 0o700, File.lstat(child).mode & 0o777
    end
  end

  def test_private_child_accepts_an_existing_private_directory
    Dir.mktmpdir("apfs-outputs-") do |root|
      child = File.join(root, "sources")
      Dir.mkdir(child, 0o700)
      marker = File.join(child, "keep")
      File.write(marker, "existing contents")
      assert_equal child, ApfsOutputs.private_child(root, "sources")
      assert_equal "existing contents", File.read(marker)
    end
  end

  def test_private_child_rejects_a_symlink_to_another_directory
    Dir.mktmpdir("apfs-outputs-") do |root|
      Dir.mktmpdir("apfs-outputs-target-") do |target|
        File.symlink(target, File.join(root, "sources"))
        assert_raises(ApfsOutputs::Error) { ApfsOutputs.private_child(root, "sources") }
        assert_empty Dir.children(target)
      end
    end
  end

  def test_private_child_rejects_a_world_writable_directory
    Dir.mktmpdir("apfs-outputs-") do |root|
      child = File.join(root, "sources")
      Dir.mkdir(child)
      File.chmod(0o777, child)
      assert_raises(ApfsOutputs::Error) { ApfsOutputs.private_child(root, "sources") }
    end
  end

  def test_private_child_rejects_an_existing_directory_with_an_acl
    Dir.mktmpdir("apfs-outputs-") do |root|
      child = File.join(root, "sources")
      Dir.mkdir(child, 0o700)
      assert system(*["/bin/chmod", "+a", "everyone allow add_file", child])
      assert_raises(ApfsOutputs::Error) { ApfsOutputs.private_child(root, "sources") }
    end
  end

  def test_content_yields_at_most_one_mib_per_chunk_and_exactly_the_requested_size
    [1, 31, 32, 33, ApfsOutputs::MIB, 2 * ApfsOutputs::MIB + 17].each do |size|
      chunks = ApfsOutputs.content(size, "seed").to_a
      assert_equal size, chunks.sum(&:bytesize)
      assert chunks.all? { (1..ApfsOutputs::MIB).cover?(_1.bytesize) }, "size #{size}"
    end
  end

  def test_zero_size_content_yields_nothing
    assert_empty ApfsOutputs.content(0, "seed").to_a
  end

  def test_content_depends_on_seed_and_starts_with_its_sha256_digest
    first = ApfsOutputs.content(64, "first").to_a.join
    second = ApfsOutputs.content(64, "second").to_a.join
    refute_equal first, second
    assert_equal Digest::SHA256.digest("first:0"), first.byteslice(0, 32)
    assert_equal Digest::SHA256.digest("second:0"), second.byteslice(0, 32)
    assert_equal first, ApfsOutputs.content(64, "first").to_a.join
  end

  def test_parse_preserves_its_arguments_and_returns_options_and_row_names
    Dir.mktmpdir("apfs-outputs-") do |root|
      argv = ["--fsimg", File.join(root, "fsimg"), "--root", root, "minimum", "small-cs"].freeze
      original = argv.dup
      options, names = ApfsOutputs.parse(argv)
      assert_equal original, argv
      assert_equal({ fsimg: File.join(root, "fsimg"), root: root }, options)
      assert_equal %w[minimum small-cs], names
    end
  end

  private

  def host_paths(source)
    Dir.glob("**/*", File::FNM_DOTMATCH, base: source).reject { %w[. ..].include?(File.basename(_1)) }.sort
  end

  def fake_fsimg(root)
    executable = File.join(root, "fsimg-double")
    File.write(executable, <<~SH)
      #!/bin/sh
      : > "$0.invoked"
      printf '%s\\n' "$@" > "$0.argv"
      while [ "$#" -gt 0 ]; do
        if [ "$1" = '-f' ]; then
          : > "$2" || exit 1
          exit 0
        fi
        shift
      done
      exit 1
    SH
    File.chmod(0o755, executable)
    executable
  end
end
