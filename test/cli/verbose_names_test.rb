# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_verbose_exfat_creation_escapes_del_control_byte_like_listing
    (@root / "control.mtree").write(". type=dir\n" + './control\177byte type=file fsimg.content=zero size=0' + "\n")
    result = invoke(arguments: ["c", "-v", "-R", "-F", "exfat", "-f", "control.img", "@control.mtree"])

    assert_empty(result.stdout)
    assert_equal(['"/control\\u007fbyte"'], listing(image: "control.img"))
    assert_equal(listing(image: "control.img"), verbose_name_lines(result.stderr))
    refute_includes(result.stderr, "\x7f".b)
  end

  def test_verbose_creation_names_match_listing_and_escape_backslash_and_bidi
    verbose_manifest
    result = invoke(arguments: ["c", "-v", "-R", "-F", "qnx6", "-f", "names.img", "@names.mtree"])
    expected = ["\"/a\\\\b\"", "\"/bidi\\u202eend\"", "\"/dir\"", "\"/dir/file\""]

    assert_empty(result.stdout)
    assert_equal(expected, verbose_catalog_listing(image: "names.img"))
    assert_equal(expected, verbose_name_lines(result.stderr))
    %w[Volume\ size: Cluster\ size: Clusters: Inodes:].each do |label|
      refute_includes(result.stderr, label, "one -v prints names without a plan")
    end
    refute_includes(result.stderr, "\u202e".encode(Encoding::UTF_8).b)
  end

  def test_double_verbose_creation_prints_plan_geometry
    %w[fat16 qnx6].each do |format|
      result = invoke(arguments: ["c", "-vv", "-R", "-F", format, "-f", "#{format}.img", "-C", "source", "."])
      assert_empty(result.stdout)
      assert_includes(result.stderr, "Cluster size:")
      assert_includes(result.stderr, "Clusters:")
      assert_includes(result.stderr, "Inodes:") if format == "qnx6"
      expected = listing(image: "#{format}.img").reject { _1 == '"/.boot"' }
      assert_equal(expected, verbose_name_lines(result.stderr))
    end
  end

  def test_verbose_dry_run_sends_plan_to_stdout_and_names_to_stderr
    verbose_manifest
    invoke(arguments: ["c", "-R", "-F", "qnx6", "-f", "names.img", "@names.mtree"])
    result = invoke(arguments: ["c", "-n", "-v", "-R", "-F", "qnx6", "@names.mtree"])

    assert_includes(result.stdout, "Volume size:")
    assert_includes(result.stdout, "Cluster size:")
    assert_includes(result.stdout, "Clusters:")
    assert_empty(verbose_name_lines(result.stdout))
    assert_equal(verbose_catalog_listing(image: "names.img"), verbose_name_lines(result.stderr))
    refute_includes(result.stderr, "Volume size:")
  end

  def test_verbose_multicomponent_operand_reports_synthesized_parent_once
    result = invoke(arguments: ["c", "-v", "-R", "-F", "exfat", "-f", "nested.img",
      "-C", "source", "nested/caf\u00e9.txt"])

    expected = ["\"/nested\"", "\"/nested/caf\u00e9.txt\""]
    assert_equal(expected, verbose_name_lines(result.stderr))
    assert_equal(expected, listing(image: "nested.img"))
  end

  def test_verbose_extraction_reports_files_directories_and_skipped_symlink_and_fifo
    verbose_posix_image
    expected = listing(image: "posix.img")
    result = invoke(arguments: ["x", "-v", "-k", "-f", "posix.img"])
    names = verbose_name_lines(result.stderr)
    marked = names.select { _1.end_with?("@", "|") }

    assert_empty(result.stdout)
    assert_equal(expected, names)
    assert_equal(['"/a/link"@', '"/fifo"|'], marked)
    assert_includes(result.stderr, "#{marked.size} entries of unsupported kinds were skipped")
    assert_equal("\0".b * 4, (@root / "posix/a/file").binread)
    refute((@root / "posix/a/link").exist?)
    refute((@root / "posix/fifo").exist?)
  end

  def test_verbose_extraction_failure_reports_only_entries_before_symlink
    verbose_posix_image
    expected = listing(image: "posix.img").take_while { !_1.end_with?("@") }
    result = invoke(arguments: ["x", "-v", "-f", "posix.img"], expected: 1)

    refute_empty(expected)
    assert_equal(expected, verbose_name_lines(result.stderr))
    assert_includes(result.stderr, '"/a/link"')
    refute_includes(verbose_name_lines(result.stderr), '"/a/link"@')
    refute((@root / "posix/a/link").symlink?, "the failing symlink is never created")
  end

  def test_json_verbose_creation_preserves_stdout_records_and_reports_complete_names
    verbose_manifest
    plain = invoke(arguments: ["c", "--json", "-R", "-F", "qnx6", "-f", "plain.img", "@names.mtree"])
    verbose = invoke(arguments: ["c", "--json", "-v", "-R", "-F", "qnx6", "-f", "verbose.img", "@names.mtree"])
    expected = json_entry_records(invoke(arguments: ["t", "--json", "-f", "verbose.img"]).stdout)
      .reject { _1.fetch("path") == "/.boot" }
    names = json_entry_records(verbose.stderr)

    assert_equal(stable_verbose_records(completed(plain)), stable_verbose_records(completed(verbose)))
    assert_complete_name_records(names)
    assert_equal(expected.map { _1.values_at("path", "kind") }, names.map { _1.values_at("path", "kind") })
    assert_equal(Integer(completed(verbose).find { _1.fetch("type") == "plan" }.fetch("entries")), names.size)
    assert_empty(json_entry_records(plain.stderr))
  end

  def test_json_verbose_extraction_preserves_stdout_records_and_reports_skipped_names
    verbose_posix_image
    expected = json_entry_records(invoke(arguments: ["t", "--json", "-f", "posix.img"]).stdout)
    plain = invoke(arguments: ["x", "--json", "-k", "-f", "posix.img"])
    File.rename(@root / "posix", @root / "plain-output")
    verbose = invoke(arguments: ["x", "--json", "-v", "-k", "-f", "posix.img"])
    names = json_entry_records(verbose.stderr)

    assert_equal(stable_verbose_records(completed(plain)), stable_verbose_records(completed(verbose)))
    assert_complete_name_records(names)
    assert_equal(expected, names)
    summary = completed(verbose).last
    assert_equal(Integer(summary.fetch("entries_processed")) + Integer(summary.fetch("skipped_entries")), names.size)
    assert_empty(json_entry_records(plain.stderr))
  end

  def test_verbose_creation_to_stdout_contains_only_reproducible_image_bytes
    verbose_manifest
    invoke(arguments: ["c", "-R", "-F", "qnx6", "-f", "file.img", "@names.mtree"])
    result = invoke(arguments: ["c", "-v", "-R", "-F", "qnx6", "-f", "-", "@names.mtree"])

    assert_equal((@root / "file.img").binread, result.stdout)
    assert_equal(verbose_catalog_listing(image: "file.img"), verbose_name_lines(result.stderr))
  end

  def test_verbose_stdout_extraction_prints_only_member_bytes
    verbose_posix_image
    result = invoke(arguments: ["x", "-O", "-v", "-f", "posix.img", "/a/file"])

    assert_equal("\0".b * 4, result.stdout)
    assert_empty(verbose_name_lines(result.stderr))
    assert_empty(json_entry_records(result.stderr))
  end

  def test_verbose_names_survive_a_slow_stderr_consumer
    # One manifest creates thousands of entries without a host file per name.
    # Long names exceed pipe capacity, forcing the worker and relay to wait.
    paths = (0...2500).map { format("/directory-%04d-%s", _1, "x" * 64) }
    manifest = [". type=dir", *paths.map { ".#{_1} type=dir" }].join("\n") + "\n"
    (@root / "many.mtree").write(manifest)
    reader, writer = IO.pipe
    drain = Thread.new do
      bytes = String.new(encoding: Encoding::BINARY)
      loop do
        chunk = reader.read(257)
        break if chunk.nil?
        bytes << chunk
        sleep(0.002)
      end
      bytes
    ensure
      reader.close
    end
    child = start_process([@executable, "c", "-n", "-v", "--json", "-R", "-F", "exfat", "@many.mtree"],
      cwd: @root, stderr: writer)
    writer.close
    stdout, = child.finish(timeout: 60)
    stderr = Timeout.timeout(10) { drain.value }
    result = CliProcesses::Result.new(stdout: stdout, stderr: stderr, status: child.status)

    assert_equal(0, result.status, result.stderr)
    plan = completed(result).find { _1.fetch("type") == "plan" }
    names = json_entry_records(stderr)
    assert_equal(paths.size, Integer(plan.fetch("entries")))
    assert_equal(Integer(plan.fetch("entries")), names.size)
    assert_equal(paths, names.map { _1.fetch("path") })
    assert_complete_name_records(names)
  ensure
    writer&.close unless writer&.closed?
    drain&.kill if drain&.alive?
    drain&.join
    reader&.close unless reader&.closed?
  end

  private

  def verbose_manifest
    # QNX6 accepts backslashes and bidi text; human output escapes both.
    # The mtree word grammar needs octal escapes for the UTF-8 bytes.
    lines = [". type=dir", './a\134b type=file fsimg.content=zero size=3',
      './bidi\342\200\256end type=file fsimg.content=zero size=0', "./dir type=dir",
      "./dir/file type=file fsimg.content=zero size=7"]
    (@root / "names.mtree").write(lines.join("\n") + "\n")
  end

  def verbose_posix_image
    lines = [". type=dir", "./a type=dir", "./a/file type=file fsimg.content=zero size=4",
      "./a/link type=link link=file", "./fifo type=fifo", "./z type=file fsimg.content=zero size=0"]
    (@root / "posix.mtree").write(lines.join("\n") + "\n")
    invoke(arguments: ["c", "-R", "-F", "qnx6", "-f", "posix.img", "@posix.mtree"])
  end

  # QNX6's format-owned boot directory is absent from the creation catalog.
  def verbose_catalog_listing(image:) = listing(image: image).reject { _1 == '"/.boot"' }

  def verbose_name_lines(bytes) = bytes.dup.force_encoding(Encoding::UTF_8).lines(chomp: true).select { _1.start_with?('"') }

  def json_entry_records(bytes)
    bytes.lines.select { _1.start_with?("{") }.map { JSON.parse(_1) }.select { _1.fetch("type") == "entry" }
  end

  def assert_complete_name_records(records)
    refute_empty(records)
    records.each do |record|
      assert_equal(1, record.fetch("schema"), record)
      assert_equal("entry", record.fetch("type"), record)
      assert_equal(true, record.fetch("complete"), record)
    end
  end

  def stable_verbose_records(records)
    # Verbosity changes the output accounting, including the summary's size.
    # Keep every semantic field and the original record sequence in the oracle.
    accounting = %w[text_bytes_queued stdout_bytes_confirmed stderr_bytes_confirmed]
    records.map { |record| record.reject { |key, _value| accounting.include?(key) } }
  end
end
