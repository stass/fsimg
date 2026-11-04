# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Native trees omit format-owned objects. Owner is masked by noowners; nlink
# has no reader field. Each hard-link name must have the node's bytes and mode.
class HfsplusNativeTest < Minitest::Test
  include CliProcesses
  include CliRecords

  EXPECTED = [
    [/e6-ditto\z/, :refused, -6, "HFS Plus compressed file is not supported", "/mixed-196608"],
    [/e6-(?:zlib|lzvn|lzfse)\z/, :refused, -6, "HFS Plus compressed file is not supported", "/forced-mixed-196608"],
    [/e4-dirlinks-J\z/, :refused, -6, "HFS Plus directory hard link is not supported", "/dl1/a/d"],
    [/e8-interrupted\z/, :refused, -6, "HFS Plus journal holds transactions; mount the volume to replay", ""],
    [/e6-sweep\z/, :refused, -6, "HFS Plus compressed file is not supported", /\A\/(?:fit|sweep)-/],
    [/mkfs-w(?:-J(?:-mounted)?)?\z/, :refused, -6, "HFS Plus volume inside an HFS wrapper is not supported", ""],
    [/\Ah-(?:hfsplus|hfsx)-/, :ok]
  ].map(&:freeze).freeze
  TOTALS = { captures: 0, entries: 0, bytes: 0, refusals: 0, skips: 0, fseventsd_logs: 0 }.freeze
  @totals = TOTALS.dup
  @groups = Hash.new { |groups, name| groups[name] = Hash.new(0) }

  class << self
    attr_reader :totals, :groups
  end

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE")).realpath
    @root = Pathname.new(Dir.pwd)
  end

  private

  def compare_capture(manifest_path)
    directory = File.dirname(manifest_path)
    name = File.basename(directory)
    image = %w[image.img image.img.gz].map { File.join(directory, _1) }.find { File.file?(_1) }
    unless image
      self.class.totals[:skips] += 1
      warn("SKIP: native manifest without image: #{manifest_path}")
      skip("native manifest without image: #{manifest_path}")
    end
    manifest = JSON.parse(File.read(manifest_path))
    expectation = EXPECTED.find { _1.first.match?(name) }
    refute_nil(expectation, "native capture needs an expected status: #{name}")
    refusal = expectation[1] == :refused
    result = invoke(arguments: ["t", "--json", "-f", image], expected: refusal ? 1 : 0, timeout: 120)
    if refusal
      error = failed(result)
      _, _, code, message, path = expectation
      assert_equal(code, error.fetch("code"), name)
      assert_equal(message, error.fetch("message"), name)
      if path.is_a?(Regexp)
        assert_match(path, error.fetch("path"), name)
        assert(manifest.fetch("tree").any? { "/#{_1.fetch("path")}" == error.fetch("path") }, name)
      else
        assert_equal(path, error.fetch("path"), name)
      end
      self.class.totals[:refusals] += 1
      return
    end
    entries = completed(result).select { _1.fetch("type") == "entry" }
    unless manifest.key?("tree")
      self.class.totals[:skips] += 1
      skip("native capture has no tree observation: #{manifest_path}")
    end
    expected = manifest.fetch("tree").to_h { ["/#{_1.fetch("path")}", _1] }
    actual = entries.to_h { [_1.fetch("path"), _1] }
    assert_equal(entries.length, actual.length, "#{name}: duplicate listing paths")
    late_logs = actual.keys.select do |path|
      !expected.key?(path) && %r{\A/?\.fseventsd/\h{16}\z}.match?(path) && actual.fetch(path).fetch("kind") == "file"
    end
    self.class.totals[:fseventsd_logs] += late_logs.length
    differing = (expected.keys - actual.keys + actual.keys - expected.keys - late_logs).sort.first
    # Native captures include host-policy timestamps (1904 shown as 1970,
    # post-2040 wrap) and .fseventsd changes at unmount, so mtime is not a
    # stable manifest oracle. Synthetic tests check all four stored times.
    entries.each do |entry|
      path = entry.fetch("path")
      next unless expected.key?(path)

      wanted = expected.fetch(path)
      assert_equal(wanted.fetch("kind") == "link" ? "symlink" : wanted.fetch("kind"), entry.fetch("kind"), "#{name}: #{path}: kind")
      unless entry.fetch("kind") == "directory"
        assert_equal(wanted.fetch("size"), Integer(entry.fetch("size_bytes")), "#{name}: #{path}: size")
      end
      assert_equal(Integer(wanted.fetch("mode"), 8), entry.fetch("mode"), "#{name}: #{path}: mode")
      self.class.totals[:entries] += 1
    end
    compare_content(image: image, entries: entries, expected: expected, name: name)
    self.class.totals[:captures] += 1
    assert_nil(differing, "#{name}: first differing path #{differing} (manifest=#{expected.key?(differing)}, image=#{actual.key?(differing)})")
  end

  def compare_content(image:, entries:, expected:, name:)
    content = entries.select { %w[file symlink].include?(_1.fetch("kind")) }
    helper = @executable.dirname / "test-hfsplus"
    result = invoke(executable: helper, arguments: ["--native-content", image], timeout: 120)
    offset = 0
    content.each do |entry|
      path = entry.fetch("path")
      size = Integer(entry.fetch("size_bytes"))
      bytes = result.stdout.byteslice(offset, size)
      assert_equal(size, bytes&.bytesize, "#{name}: #{path}: stream length")
      if expected.key?(path)
        wanted = expected.fetch(path)
        if entry.fetch("kind") == "file" && wanted["sha256"]
          assert_equal(wanted.fetch("sha256"), Digest::SHA256.hexdigest(bytes), "#{name}: #{path}: sha256")
          self.class.totals[:bytes] += size
        elsif wanted.key?("target")
          assert_equal(wanted.fetch("target").b, bytes, "#{name}: #{path}: target")
        end
      end
      offset += size
    end
    assert_equal(offset, result.stdout.bytesize, "#{name}: no trailing stream bytes")
  end

  public

  directory = ENV["FSIMG_HFSPLUS_NATIVE"]
  if directory
    manifests = Dir[File.join(File.expand_path(directory), "h-*", "manifest.json")].sort
    raise ArgumentError, "no native capture manifests in #{directory}" if manifests.empty?

    # Generates one independently counted test per manifest, including missing
    # images. Minitest discovers the generated methods through methods/respond_to?.
    manifests.each do |manifest|
      define_method("test_native_#{File.basename(File.dirname(manifest)).tr('-', '_')}") do
        group = File.basename(File.dirname(manifest)).sub(/\Ah-(?:hfsplus|hfsx)-\d+-/, "").split("-").first
        begin
          compare_capture(manifest)
          self.class.groups[group][:passed] += 1
        rescue Minitest::Skip
          self.class.groups[group][:skipped] += 1
          raise
        rescue Minitest::Assertion, StandardError
          self.class.groups[group][:failed] += 1
          raise
        end
      end
    end
  else
    def test_native_captures_require_explicit_directory
      self.class.totals[:skips] += 1
      skip("FSIMG_HFSPLUS_NATIVE unset; native trees not compared")
    end
  end
end

Minitest.after_run do
  totals = HfsplusNativeTest.totals
  puts "Native group | passed | failed | skipped"
  HfsplusNativeTest.groups.sort.each do |name, counts|
    puts "#{name} | #{counts[:passed]} | #{counts[:failed]} | #{counts[:skipped]}"
  end
  puts "Native trees: #{totals[:captures]} captures compared, #{totals[:entries]} entries compared, " \
    "#{totals[:bytes]} bytes hashed, #{totals[:refusals]} refusals as expected, #{totals[:skips]} skips, fseventsd logs after the manifest: #{totals[:fseventsd_logs]}"
end
