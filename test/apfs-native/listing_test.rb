# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "json"
require "minitest/autorun"
require "pathname"
require_relative "../cli/process"

# Compare host observations to t --json without mounting or reading content.
# The dump's inode owners avoid the host mount's noowners translation. Times
# remain the manifest's observations, including atime; discrepancies fail.
class ApfsListingNativeTest < Minitest::Test
  include CliProcesses

  KINDS = { "dir" => "directory", "link" => "symlink",
            "char" => "char_device", "block" => "block_device" }.freeze
  TIMES = { "modified" => "mtime_ns", "changed" => "ctime_ns",
            "accessed" => "atime_ns", "created" => "btime_ns" }.freeze

  class Report
    def initialize
      @compared = 0
      @matched = 0
    end

    def record(matched:)
      @compared += 1
      @matched += 1 if matched
    end

    def to_s = "APFS native listings: #{@compared} captures compared, #{@matched} matched"
  end
  REPORT = Report.new

  private

  def native_records(path)
    text = File.read(path)
    text.lstrip.start_with?("[") ? JSON.parse(text) : text.lines.reject { _1.strip.empty? }.map { JSON.parse(_1) }
  end

  def image_path(path)
    relative = path.delete_prefix("/").delete_prefix("./")
    ["", "."].include?(relative) ? "/" : "/#{relative}"
  end

  def excluded?(path) = path == "/" || path == "/.fseventsd" || path.start_with?("/.fseventsd/")

  def index_paths(records)
    entries = records.filter_map do |record|
      path = image_path(record.fetch("path"))
      [path, record] unless excluded?(path)
    end
    assert_equal(entries.length, entries.map(&:first).uniq.length, "duplicate listing or manifest paths")
    entries.to_h
  end

  def disk_owners(directory)
    path = File.join(directory, "apfsdump.json")
    return nil unless File.file?(path)

    dump = JSON.parse(File.read(path))
    volumes = dump.fetch("volumes")
    assert_equal(1, volumes.length, "native manifest must identify one volume")
    volumes.first.fetch("fs_tree").fetch("records").select { _1.fetch("type") == 3 }.to_h do |inode|
      [inode.fetch("obj_id"), [inode.fetch("owner"), inode.fetch("group")]]
    end
  end

  def time_ns(time)
    return nil if time.nil?

    Integer(time.fetch("seconds"), 10) * 1_000_000_000 + time.fetch("nanoseconds")
  end

  def difference(errors:, path:, field:, expected:, actual:)
    errors << "#{path.inspect} #{field}: expected #{expected.inspect}, got #{actual.inspect}" unless expected == actual
  end

  def compare_entry(path:, expected:, actual:, owners:, mounter:)
    errors = []
    kind = KINDS.fetch(expected.fetch("kind"), expected.fetch("kind"))
    difference(errors: errors, path: path, field: "kind", expected: kind, actual: actual.fetch("kind"))
    if %w[file symlink].include?(kind)
      size = kind == "symlink" && expected.key?("target") ? expected.fetch("target").bytesize : expected.fetch("size")
      difference(errors: errors, path: path, field: "size", expected: size, actual: Integer(actual.fetch("size_bytes"), 10))
    end
    mode = expected.fetch("mode")
    mode = mode.is_a?(String) ? Integer(mode, 8) : mode
    difference(errors: errors, path: path, field: "mode", expected: mode & 0o7777, actual: actual.fetch("mode"))
    TIMES.each do |field, native|
      observed = native == "btime_ns" ? expected.fetch(native) { expected.fetch("birthtime_ns") } : expected.fetch(native)
      difference(errors: errors, path: path, field: field, expected: observed, actual: time_ns(actual.fetch(field)))
    end
    inode = owners&.fetch(expected.fetch("ino"))
    %w[uid gid].each_with_index do |field, index|
      wanted = inode ? inode.fetch(index) : expected.fetch(field)
      got = actual.fetch(field)
      # Without a dump, accept the disk's nobody ID only for the mounter's
      # own translated ID, identified by the manifest's root entry.
      next if inode.nil? && got == 99 && wanted == mounter.fetch(field)

      difference(errors: errors, path: path, field: field, expected: wanted, actual: got)
    end
    errors
  end

  def compare_capture(manifest)
    directory = File.dirname(manifest)
    image = File.join(directory, "image.img")
    assert(File.file?(image), "native manifest lacks image.img: #{directory}")
    source = native_records(manifest)
    root = source.find { image_path(_1.fetch("path")) == "/" }
    refute_nil(root, "manifest must include its root to identify the mounter")
    expected = index_paths(source)
    owners = disk_owners(directory)
    puts "APFS owners #{File.basename(directory)}: #{owners.nil? ? 'manifest, accepting 99 for mounter IDs' : 'apfsdump.json inode owners'}"
    executable = File.expand_path(ENV.fetch("FSIMG_CLI_EXECUTABLE", "./fsimg"))
    result = run_process([executable, "t", "--json", "-f", File.realpath(image)], timeout: 120)
    if result.status != 0
      REPORT.record(matched: false)
      flunk("#{directory}: t --json exited #{result.status}\n#{result.stdout}\n#{result.stderr}")
    end
    decoded = result.stdout.lines.map { JSON.parse(_1) }
    assert_equal("summary", decoded.last.fetch("type"), "listing must end in summary")
    assert_equal(true, decoded.last.fetch("complete"), "listing must be complete")
    actual = index_paths(decoded.select { _1.fetch("type") == "entry" })
    errors = (expected.keys - actual.keys).sort.map { "missing path #{_1.inspect}" } +
      (actual.keys - expected.keys).sort.map { "unexpected path #{_1.inspect}" }
    (expected.keys & actual.keys).sort.each do |path|
      errors.concat(compare_entry(path: path, expected: expected.fetch(path), actual: actual.fetch(path), owners: owners, mounter: root))
    end
    REPORT.record(matched: errors.empty?)
    assert_empty(errors, "#{directory}:\n#{errors.join("\n")}")
  end

  public

  directory = ENV["FSIMG_APFS_NATIVE"]
  if directory
    manifests = Dir[File.join(File.expand_path(directory), "**", "native-manifest.{json,ndjson}")]
      .sort.group_by { File.dirname(_1) }.values.map(&:first)
    raise ArgumentError, "no native manifests in #{directory}" if manifests.empty?

    # Generate one public test per capture; Minitest discovers each through
    # methods/respond_to?, so a failed capture does not hide later captures.
    manifests.each_with_index do |manifest, index|
      define_method("test_apfs_native_listing_#{index}_#{File.basename(File.dirname(manifest)).tr('-', '_')}") do
        compare_capture(manifest)
      end
    end
  else
    def test_apfs_native_listing_requires_capture_directory
      skip("FSIMG_APFS_NATIVE unset; APFS native listings not compared")
    end
  end
end

Minitest.after_run { puts ApfsListingNativeTest::REPORT }
