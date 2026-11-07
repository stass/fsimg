# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fileutils"
require "minitest/autorun"
require "pathname"
require "tmpdir"
require_relative "process"
require_relative "records"

class ErofsCreateTest < Minitest::Test
  include CliProcesses
  include CliRecords

  TIME = 1_700_000_001
  UUID = "0123abcd-4567-89ef-0123-456789abcdef"

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE", "./fsimg")).realpath
    @root = Pathname.new(Dir.mktmpdir("erofs-create-", Dir.pwd))
    @source = @root / "source"
    @source.mkdir
    (@source / "dir").mkdir
    (@source / "dir/file").binwrite("payload\x00\xff".b * 1000)
    (@source / "empty").binwrite("".b)
    File.symlink("dir/file", @source / "link")
    {"dir" => 0o750, "dir/file" => 0o640, "empty" => 0o600}.each do |path, mode|
      File.chmod(mode, @source / path)
      File.utime(TIME, TIME, @source / path)
    end
  end

  def teardown
    assert_empty(@root.glob(".fsimg-*"), "creation removes staging files")
  ensure
    FileUtils.remove_entry(@root) if @root
  end

  def test_formats_advertise_the_writer
    format = filesystems(completed(invoke(arguments: ["--formats", "--json"]))).fetch("erofs")
    assert_equal(7, format.fetch("capabilities"))
    assert_equal(0x402, format.fetch("write_settings"))
    assert_equal([512, 1024, 2048, 4096], format.fetch("write_cluster_bytes"))
  end

  def test_each_block_size_creates_a_volume_that_verifies_and_reads_back
    [512, 1024, 2048, 4096].each do |bytes|
      image = create(name: "block-#{bytes}", flags: ["-R", "-o", "cluster=#{bytes}"])
      assert_equal(bytes, record(image: image, command: "i", type: "info").fetch("cluster_bytes"))
      assert_equal("initialized-payload", record(image: image, command: "W", type: "verify", flags: ["-d"])
        .fetch("validation_scope"), bytes.to_s)
      assert_equal((@source / "dir/file").binread,
        invoke(arguments: ["x", "-O", "-S", "off", "-f", image, "/dir/file"]).stdout, bytes.to_s)
    end
  end

  def test_block_size_beyond_4096_is_outside_the_profile
    result = invoke(arguments: ["c", "-n", "-S", "off", "-R", "-F", "erofs",
      "-o", "cluster=8192", "-C", @source, "."], expected: 2)
    assert_includes(result.stderr, "outside the selected writer profile".b)
  end

  def test_create_list_verify_and_extract_preserve_bytes_modes_and_times
    image = create(name: "plain", flags: ["-p", "-o", "uuid=#{UUID}"])
    entries = record(image: image, command: "t", type: "entry", many: true).to_h { [_1.fetch("path"), _1] }
    assert_equal(%w[/dir /dir/file /empty /link], entries.keys.sort)
    assert_equal("symlink", entries.fetch("/link").fetch("kind"))
    {"/dir" => 0o750, "/dir/file" => 0o640, "/empty" => 0o600}.each do |path, mode|
      assert_equal(mode, entries.fetch(path).fetch("mode"), path)
      assert_equal(TIME.to_s, entries.fetch(path).fetch("modified").fetch("seconds"), path)
    end
    assert_equal("initialized-payload", record(image: image, command: "W", type: "verify", flags: ["-d"])
      .fetch("validation_scope"))
    assert_equal((@source / "dir/file").binread,
      invoke(arguments: ["x", "-O", "-S", "off", "-f", image, "/dir/file"]).stdout)
  end

  def test_reproducible_images_are_identical
    first = create(name: "first", flags: ["-R", "-e", TIME.to_s])
    second = create(name: "second", flags: ["-R", "-e", TIME.to_s])
    assert_equal(first.binread, second.binread)
  end

  def test_fixed_gpt_disk_holds_a_volume_of_whole_sectors
    image = create(name: "disk", variant: "erofs/gpt", flags: ["-R", "-o", "gpt:disk=2M"])
    assert_equal(2 << 20, image.stat.size)
    assert_equal("initialized-payload", record(image: image, command: "W", type: "verify", flags: ["-d"])
      .fetch("validation_scope"))
  end

  def test_unwritten_layouts_are_refused
    %w[compression=lz4 addressing=48].each do |setting|
      result = invoke(arguments: ["c", "-n", "-S", "off", "-R", "-F", "erofs",
        "-o", setting, "-C", @source, "."], expected: 1)
      assert_includes(result.stderr, setting.split("=").first.b, setting)
    end
  end

  private

  def create(name:, variant: "erofs", flags: [])
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
