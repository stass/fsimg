# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"
require "fileutils"
require "json"
require "minitest/autorun"
require "pathname"
require "tmpdir"
require_relative "process"
require_relative "records"

class HfsplusCreateTest < Minitest::Test
  include CliProcesses
  include CliRecords

  VARIANTS = %w[hfsplus hfsx].freeze
  TIME = 1_700_000_001
  NAME = "cafe\u0301".freeze

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE", "./fsimg")).realpath
    @root = Pathname.new(Dir.mktmpdir("hfsplus-create-", Dir.pwd))
    @source = @root / "source"
    @source.mkdir
  end

  def teardown
    assert_empty(@root.glob(".fsimg-*"), "creation removes staging files")
  ensure
    FileUtils.remove_entry(@root) if @root
  end

  def test_formats_advertise_writer_limits_and_settings
    formats = filesystems(completed(invoke(arguments: ["--formats", "--json"])))
    VARIANTS.each do |variant|
      format = formats.fetch(variant)
      assert_equal(7, format.fetch("capabilities"), variant)
      assert_equal(127, format.fetch("entry_kinds"), variant)
      assert_equal(2, format.fetch("write_settings"), variant)
      assert_equal([512, 1024, 2048, 4096, 8192, 16_384, 32_768, 65_536],
        format.fetch("write_cluster_bytes"), variant)
      %w[max_file_bytes max_volume_bytes].each do |field|
        assert_equal(((2**32 - 1) * 2**31).to_s, format.fetch(field), "#{variant} #{field}")
      end
    end
  end

  def test_create_list_verify_and_extract_preserve_bytes_modes_and_times
    (@source / "dir").mkdir
    (@source / "dir/file").binwrite("payload\x00\xff".b)
    (@source / "empty").binwrite("".b)
    (@source / NAME).binwrite("decomposed name\n")
    File.symlink("dir/file", @source / "link")
    modes = {"dir" => 0o750, "dir/file" => 0o640, "empty" => 0o600, NAME => 0o644}
    modes.each do |path, mode|
      File.chmod(mode, @source / path)
      File.utime(TIME, TIME, @source / path)
    end
    File.lutime(TIME, TIME, @source / "link")

    VARIANTS.each do |variant|
      image = create(variant: variant, name: variant, flags: ["-p"])
      entries = record(image: image, command: "t", type: "entry", many: true).to_h { [_1.fetch("path"), _1] }
      assert_equal(["/dir", "/dir/file", "/empty", "/#{NAME}", "/link"].sort, entries.keys.sort, variant)
      assert_equal("directory", entries.fetch("/dir").fetch("kind"))
      assert_equal("symlink", entries.fetch("/link").fetch("kind"))
      assert_equal(TIME.to_s, entries.fetch("/link").fetch("modified").fetch("seconds"))
      link_mode = File.lstat(@source / "link").mode & 0o777
      assert_equal(link_mode, entries.fetch("/link").fetch("mode"))
      modes.each do |path, mode|
        entry = entries.fetch("/#{path}")
        assert_equal(mode, entry.fetch("mode"), "#{variant} #{path}")
        assert_equal(TIME.to_s, entry.fetch("modified").fetch("seconds"), "#{variant} #{path}")
      end
      report = record(image: image, command: "W", type: "verify", flags: ["-d"])
      assert_equal("initialized-payload", report.fetch("validation_scope"))
      destination = @root / "restore-#{variant}"
      destination.mkdir
      manifest = @root / "#{variant}.mtree"
      completed(invoke(arguments: ["x", "--json", "-S", "off", "-k", "--mtree=#{manifest}",
        "-f", image, "-C", destination]))
      restored = destination / variant
      modes.each_key do |path|
        actual = restored / path
        if (@source / path).file?
          assert_predicate(actual, :file?, "#{variant} #{path}")
          assert_equal((@source / path).binread, actual.binread, "#{variant} #{path}")
        end
      end
      assert_predicate(restored / "dir", :directory?)
      assert_equal("dir/file".b, invoke(arguments: ["x", "-O", "-S", "off", "-f", image, "/link"]).stdout)
      # Extraction stores supported metadata in mtree; host output stays private.
      lines = manifest.read.lines(chomp: true)
      modes.each do |path, mode|
        # mtree quotes each non-ASCII UTF-8 byte as an octal escape.
        escaped = path.b.bytes.map { _1 < 128 ? _1.chr : format("\\%03o", _1) }.join
        line = lines.find { _1.start_with?("./#{escaped} ") }
        refute_nil(line, "#{variant} #{path} manifest entry")
        assert_includes(line, format(" mode=%04o ", mode))
        assert_includes(line, " time=#{TIME}.000000000")
      end
      link = lines.find { _1.start_with?("./link ") }
      refute_nil(link)
      assert_includes(link, "type=link")
      assert_includes(link, format(" mode=%04o ", link_mode))
      assert_includes(link, " time=#{TIME}.000000000 link=dir/file")
    end
  end

  def test_cluster_endpoints_are_honoured_and_oversize_is_refused
    VARIANTS.each do |variant|
      [512, 65_536].each do |cluster|
        image = create(variant: variant, name: "#{variant}-#{cluster}", flags: ["-o", "cluster=#{cluster}"])
        assert_equal(cluster, record(image: image, command: "i", type: "info").fetch("cluster_bytes"), variant)
      end
      image = @root / "bad-#{variant}.img"
      error = failed(invoke(arguments: ["c", "--json", "-S", "off", "-F", variant,
        "-o", "cluster=131072", "-f", image, "-C", @source, "."], expected: 2))
      assert_equal(-1, error.fetch("code"), variant)
      refute_path_exists(image)
    end
  end

  def test_explicit_uuid_is_reported_as_volume_identifier
    uuid = "01234567-89ab-cdef-1032-547698badcfe"
    VARIANTS.each do |variant|
      image = create(variant: variant, name: variant, flags: ["-o", "uuid=#{uuid}"])
      # HFS stores the first eight UUID bytes as its Finder volume identifier.
      info = record(image: image, command: "i", type: "info")
      assert_equal(0x0123456789abcdef.to_s, info.fetch("details").fetch("volume_id"), variant)
    end
  end

  def test_reproducible_creation_and_minimization
    (@source / "file").binwrite("small payload")
    VARIANTS.each do |variant|
      first = create(variant: variant, name: "#{variant}-first", flags: ["-R"])
      second = create(variant: variant, name: "#{variant}-second", flags: ["-R"])
      assert_equal(Digest::SHA256.file(first).hexdigest, Digest::SHA256.file(second).hexdigest, variant)
      minimized = create(variant: variant, name: "#{variant}-min", flags: ["-R", "-M"])
      assert_operator(minimized.size, :<=, first.size, variant)
    end
  end

  def test_free_space_reserves_free_blocks
    VARIANTS.each do |variant|
      image = create(variant: variant, name: variant, flags: ["--free-space", "64K"])
      report = record(image: image, command: "W", type: "verify")
      info = record(image: image, command: "i", type: "info")
      free_bytes = report.fetch("free_clusters").to_i * info.fetch("cluster_bytes")
      assert_operator(free_bytes, :>=, 64 * 1024, variant)
    end
  end

  def test_case_collisions_are_refused_by_hfsplus_and_preserved_by_hfsx
    # A manifest represents both names even on a case-insensitive host.
    manifest = @root / "case.mtree"
    manifest.write("./Zeta type=file size=0 fsimg.content=zero\n./zeta type=file size=0 fsimg.content=zero\n")
    image = @root / "case-plus.img"
    error = failed(invoke(arguments: ["c", "--json", "-S", "off", "-F", "hfsplus",
      "-f", image, "@#{manifest}"], expected: 1))
    assert_includes(error.fetch("message"), "equivalent to a sibling")
    refute_path_exists(image)
    invoke(arguments: ["c", "-S", "off", "-F", "hfsx", "-f", image, "@#{manifest}"])
    entries = record(image: image, command: "t", type: "entry", many: true)
    assert_equal(%w[/Zeta /zeta], entries.map { _1.fetch("path") }.sort)
  end

  def test_fifo_from_manifest_is_stored
    manifest = @root / "fifo.mtree"
    manifest.write("./pipe type=fifo mode=0640 time=#{TIME}\n")
    VARIANTS.each do |variant|
      image = @root / "#{variant}.img"
      invoke(arguments: ["c", "-S", "off", "-F", variant, "-f", image, "@#{manifest}"])
      entry = record(image: image, command: "t", type: "entry")
      assert_equal("/pipe", entry.fetch("path"))
      assert_equal("fifo", entry.fetch("kind"))
      assert_equal(0o640, entry.fetch("mode"))
    end
  end

  private

  def create(variant:, name:, flags: [])
    image = @root / "#{name}.img"
    completed(invoke(arguments: ["c", "--json", "-S", "off", "-F", variant, *flags,
      "-f", image, "-C", @source, "."]))
    image
  end

  def record(image:, command:, type:, flags: [], many: false)
    selected = completed(invoke(arguments: [command, "--json", "-S", "off", *flags, "-f", image]))
      .select { _1.fetch("type") == type }
    return selected if many

    assert_equal(1, selected.size, "one #{type} record")
    selected.fetch(0)
  end
end
