# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_salvage_fat_retains_exact_prefix_and_readable_sibling_under_default_sandbox
    prefix = salvage_fat_fixture
    result = invoke(arguments: %w[x --salvage -f tail.img], expected: 1)

    assert_equal(prefix, (@root / "tail/FOO.partial").binread)
    refute_path_exists(@root / "tail/FOO")
    assert_equal(fat_truncated_payload, (@root / "tail/HELLO.TXT").binread)
    assert_salvage_closing_lines(result, total: "noticed 1 loss", output: "./tail")
    assert_includes(result.stderr,
      "fsimg: salvage: \"/FOO.partial\": shortened at the first noticed damage\n")
  end

  def test_salvage_exfat_recovers_only_the_first_initialized_fragment
    salvage_exfat_fixture
    entries = completed(invoke(arguments: %w[t --json -f tail.img]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(%w[/Data.bin /Good.bin], entries.map { _1.fetch("path") })
    result = invoke(arguments: %w[x --salvage -f tail.img], expected: 1)

    prefix = (0...512).map { (_1 * 37) & 255 }.pack("C*")
    assert_equal(prefix, (@root / "tail/Data.bin.partial").binread)
    refute_path_exists(@root / "tail/Data.bin")
    assert_equal("good".b, (@root / "tail/Good.bin").binread)
    assert_salvage_closing_lines(result, total: "noticed 1 loss", output: "./tail")
  end

  def test_salvage_file_without_surviving_bytes_is_skipped
    salvage_fat_fixture(surviving_bytes: 0)
    result = invoke(arguments: %w[x --salvage -f tail.img], expected: 1)

    assert_includes(result.stderr,
      'fsimg: salvage: "/FOO": skipped: no recoverable bytes, or its name is too long')
    refute_path_exists(@root / "tail/FOO.partial")
    refute_path_exists(@root / "tail/FOO")
    assert_equal(fat_truncated_payload, (@root / "tail/HELLO.TXT").binread)
  end

  def test_salvage_caps_examples_at_32_and_reports_all_40_losses_in_walk_order
    salvage_fat_fixture(damaged_files: 40)
    result = invoke(arguments: %w[x --salvage -f tail.img], expected: 1)

    examples = result.stderr.lines.grep(/^fsimg: salvage: "/)
    expected = (0...32).map do |index|
      "fsimg: salvage: \"/F#{index.to_s.rjust(7, '0')}.partial\": shortened at the first noticed damage\n"
    end
    assert_equal(expected, examples)
    assert_equal(40, (@root / "tail").glob("*.partial").size)
    assert_salvage_closing_lines(result,
      total: "noticed 40 losses; 8 examples omitted", output: "./tail")
  end

  def test_salvage_verbose_entry_names_the_shortened_output
    salvage_fat_fixture
    result = invoke(arguments: %w[x --salvage -v -f tail.img], expected: 1)

    assert_match(/^.*"\/FOO\.partial"\n/, result.stderr)
    refute_match(/^.*"\/FOO"\n/, result.stderr)
  end

  def test_salvage_verbose_skips_partial_name_beyond_path_limit_like_nonverbose
    salvage_fat_fixture
    # Keep the healthy sibling within the path budget, including its NUL.
    boot = File.binread(@root / "tail.img", 512)
    root_offset = (boot[14, 2].unpack1("v") + boot.getbyte(16) * boot[22, 2].unpack1("v")) * 512
    File.open(@root / "tail.img", "r+b") { _1.pwrite("OK".ljust(11).b, root_offset) }
    plain = invoke(arguments: %w[x --salvage -l path=10 -f tail.img], expected: 1)
    File.rename(@root / "tail", @root / "plain")
    verbose = invoke(arguments: %w[x --salvage -v -l path=10 -f tail.img], expected: 1)

    [verbose, plain].each do |result|
      assert_includes(result.stderr,
        "fsimg: salvage: \"/FOO\": skipped: no recoverable bytes, or its name is too long\n")
      assert_includes(result.stderr, "fsimg: salvage: noticed 1 loss\n")
      refute_includes(result.stderr, "invalid worker")
    end
    %w[plain tail].each do |directory|
      refute_path_exists(@root / directory / "FOO.partial")
      assert_equal(["OK"], (@root / directory).children.map { _1.basename.to_s })
      assert_equal(fat_truncated_payload, (@root / directory / "OK").binread)
    end
    assert_equal(plain.stderr.lines.grep(/^fsimg: salvage:/),
      verbose.stderr.lines.grep(/^fsimg: salvage:/))
  end

  def test_salvage_extract_here_retains_the_given_directory
    prefix = salvage_fat_fixture
    private_directory(root: @root, name: "restore")
    result = invoke(arguments: %w[x --salvage -E -C restore -f tail.img], expected: 1)

    assert_equal(prefix, (@root / "restore/FOO.partial").binread)
    assert_salvage_closing_lines(result, total: "noticed 1 loss", output: "restore")
  end

  def test_salvage_json_reports_loss_and_incomplete_extraction_total
    salvage_fat_fixture
    result = invoke(arguments: %w[x --salvage --json -f tail.img], expected: 1)
    decoded = records(result, stream: :stderr)

    assert_equal([{"schema" => 1, "type" => "salvage_loss", "kind" => "shortened",
      "path_truncated" => false, "path_excerpt" => "/FOO.partial"}],
      decoded.select { _1.fetch("type") == "salvage_loss" })
    error = failed(result)
    assert_equal("incomplete extraction", error.fetch("status"))
    assert_equal(-14, error.fetch("code"))
    assert_equal("incomplete", error.fetch("output_state"))
    assert_equal("1", error.fetch("salvage_losses"))
    refute_empty(error.fetch("retained_output"))
    assert_path_exists(@root / error.fetch("retained_output"))
  end

  def test_salvage_conflicting_options_and_selection_are_usage_errors_before_output_creation
    fat_truncated_fixture
    cases = [
      [%w[x --salvage -O], "--salvage conflicts with -O"],
      [%w[x --salvage --mtree=out.mtree], "--salvage conflicts with --mtree"],
      [%w[x --salvage], "--salvage takes no member selection", "/HELLO.TXT"],
      *%w[t i W c].map { |mode| [[mode, "--salvage"], "option does not apply to mode #{mode}"] }
    ]
    cases.each do |flags, message, member|
      result = invoke(arguments: [*flags, "-f", "tail.img", *Array(member)], expected: 2)
      assert_includes(result.stderr, message)
      refute_path_exists(@root / "tail")
      refute_path_exists(@root / "out.mtree")
    end
  end

  def test_clean_salvage_matches_strict_extraction_without_loss_diagnostics
    fat_truncated_fixture
    %w[strict recovered].each { (@root / _1).mkdir }
    invoke(arguments: %w[x -f tail.img -C strict])
    result = invoke(arguments: %w[x --salvage -f tail.img -C recovered])

    refute_includes(result.stderr, "salvage")
    strict = @root / "strict/tail"
    recovered = @root / "recovered/tail"
    assert_equal(strict.glob("**/*").map { _1.relative_path_from(strict).to_s }.sort,
      recovered.glob("**/*").map { _1.relative_path_from(recovered).to_s }.sort)
    strict.glob("**/*").select(&:file?).each do |file|
      assert_equal(file.binread, (recovered / file.relative_path_from(strict)).binread)
    end
  end

  def test_salvage_terminal_output_limit_keeps_ordinary_error_without_loss_total
    fat_truncated_fixture
    result = invoke(arguments: %w[x --salvage -l output=1000 -f tail.img], expected: 1)

    refute_includes(result.stderr, "salvage extraction lost part of the image")
    refute_includes(result.stderr, "noticed")
    assert_includes(result.stderr, "limit")
  end

  def test_salvage_json_terminal_failure_has_no_salvage_total
    fat_truncated_fixture
    result = invoke(arguments: %w[x --salvage --json -l output=1000 -f tail.img], expected: 1)
    error = failed(result)

    assert_equal(-8, error.fetch("code"))
    refute(error.key?("salvage_losses"))
    refute_includes(result.stderr, "noticed")
    refute_includes(result.stderr, "salvage extraction lost part of the image")
  end

  def test_salvage_implies_skip_unsupported_and_still_warns_about_the_link
    (@root / "linked").mkdir
    (@root / "linked/file").binwrite("data".b)
    File.symlink("file", @root / "linked/link")
    invoke(arguments: %w[c -S off -F qnx4 -f link.img -C linked .])
    result = invoke(arguments: %w[x --salvage -f link.img], expected: 1)

    assert_equal("data".b, (@root / "link/file").binread)
    refute_path_exists(@root / "link/link")
    assert_includes(result.stderr,
      'fsimg: salvage: "/link": skipped with any children: safe extraction cannot create its kind or name')
    assert_includes(result.stderr, "1 entry of an unsupported kind was skipped")
    assert_salvage_closing_lines(result, total: "noticed 1 loss", output: "./link")
  end

  def test_salvage_counts_an_omitted_out_of_profile_name_as_an_unsupported_loss
    (@root / "omitted.img").binwrite(ifs_image(payload: "data".b, motd: "mo\xe9td".b))
    result = invoke(arguments: %w[x --salvage --json -f omitted.img], expected: 1)
    losses = records(result, stream: :stderr).select { _1.fetch("type") == "salvage_loss" }

    assert_equal(["/bin/sh", '/etc/mo\xe9td'], losses.map { _1.fetch("path_excerpt") })
    assert_equal(%w[unsupported unsupported], losses.map { _1.fetch("kind") })
    error = failed(result)
    assert_equal("2", error.fetch("salvage_losses"))
    refute_path_exists(@root / "omitted/etc/mo\xe9td".b)
  end

  private

  def assert_salvage_closing_lines(result, total:, output:)
    assert_equal(["fsimg: salvage: #{total}\n",
      "fsimg: salvage extraction lost part of the image\n",
      "fsimg: output is incomplete\n", "fsimg: retained output: \"#{output}\"\n"],
      result.stderr.lines.last(4))
    assert_path_exists(@root / output)
  end

  # Allocate the seed's unused cluster between the two Data.bin fragments
  # to a readable sibling, then cut just before the second fragment.
  def salvage_exfat_fixture
    exfat_truncated_fixture
    path = @root / "tail.img"
    image = path.binread
    heap = image[88, 4].unpack1("V") * 512
    root = heap + (image[96, 4].unpack1("V") - 2) * 512
    sibling = image.byteslice(root + 96, 96)
    sibling[36, 2] = [salvage_exfat_checksum("GOOD.BIN".encode("UTF-16LE").b)].pack("v")
    sibling[40, 8] = [4].pack("Q<")
    sibling[52, 4] = [261].pack("V")
    sibling[56, 8] = [4].pack("Q<")
    sibling[66, 16] = "Good.bin".encode("UTF-16LE").b
    checksum_bytes = sibling.bytes.each_with_index.reject { |_, index| [2, 3].include?(index) }.map(&:first)
    sibling[2, 2] = [salvage_exfat_checksum(checksum_bytes.pack("C*"))].pack("v")
    File.open(path, "r+b") do |file|
      file.pwrite(sibling, root + 192)
      file.pwrite([0xffffffff].pack("V"), 24 * 512 + 261 * 4)
      bitmap_offset = heap + (261 - 2) / 8
      file.pwrite([image.getbyte(bitmap_offset) | (1 << ((261 - 2) % 8))].pack("C"), bitmap_offset)
      file.pwrite("good".b, heap + (261 - 2) * 512)
      file.truncate(heap + (262 - 2) * 512)
    end
  end

  def salvage_exfat_checksum(bytes)
    bytes.bytes.reduce(0) { |sum, byte| (((sum >> 1) | ((sum & 1) << 15)) + byte) & 0xffff }
  end

  # Keep the seed's readable HELLO.TXT, replace its remaining root entries
  # with files sharing a new two-cluster chain, and cut that chain's payload.
  # Sharing bytes makes the 40-loss fixture small without changing its oracle.
  def salvage_fat_fixture(surviving_bytes: nil, damaged_files: 1)
    fat_truncated_fixture
    path = @root / "tail.img"
    boot = File.binread(path, 512)
    reserved = boot[14, 2].unpack1("v")
    fat_bytes = boot[22, 2].unpack1("v") * 512
    root_offset = reserved * 512 + boot.getbyte(16) * fat_bytes
    data_offset = root_offset + boot[17, 2].unpack1("v") * 32
    cluster_bytes = boot.getbyte(13) * 512
    first_offset = data_offset + (6 - 2) * cluster_bytes
    payload = (0...(cluster_bytes + 512)).map { (_1 * 29) & 255 }.pack("C*")
    surviving_bytes ||= cluster_bytes + 137
    File.open(path, "r+b") do |file|
      file.pwrite("\0".b * (boot[17, 2].unpack1("v") * 32 - 32), root_offset + 32)
      boot.getbyte(16).times do |table|
        file.pwrite([7, 0xffff].pack("v*"), reserved * 512 + table * fat_bytes + 6 * 2)
      end
      damaged_files.times do |index|
        name = damaged_files == 1 ? "FOO" : "F#{index.to_s.rjust(7, '0')}"
        entry = name.ljust(11).b + "\x20".b + "\0".b * 14 + [6, payload.bytesize].pack("vV")
        file.pwrite(entry, root_offset + (index + 1) * 32)
      end
      file.pwrite(payload, first_offset)
      file.truncate(first_offset + surviving_bytes)
    end
    payload.byteslice(0, surviving_bytes)
  end
end
