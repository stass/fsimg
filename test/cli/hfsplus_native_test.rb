# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module HfsplusNativePaths
  module_function

  def find_difference(expected:, actual:, late_logs:)
    ((expected - actual) | (actual - expected - late_logs)).min
  end
end

class HfsplusNativePathsTest < Minitest::Test
  def test_missing_image_entry_is_reported
    assert_equal("/missing", HfsplusNativePaths.find_difference(
      expected: ["/a", "/missing"], actual: ["/a"], late_logs: []))
  end

  def test_extra_image_entry_is_reported
    assert_equal("/extra", HfsplusNativePaths.find_difference(
      expected: ["/a"], actual: ["/a", "/extra"], late_logs: []))
  end

  def test_late_event_log_is_ignored
    assert_nil(HfsplusNativePaths.find_difference(
      expected: ["/a"], actual: ["/a", "/.fseventsd/0123456789abcdef"],
      late_logs: ["/.fseventsd/0123456789abcdef"]))
  end
end

# Native trees omit format-owned objects. Owner is masked by noowners; nlink
# has no reader field. Each hard-link name must have the node's bytes and mode.
class HfsplusNativeTest < Minitest::Test
  include CliProcesses
  include CliRecords
  include CliFixtures

  CLI_SAMPLE_PER_CAPTURE = 8
  CLI_MEMBER_BYTE_CAP = 1024 * 1024
  # Smallest observed file payload among captures with directories and links:
  # 204 file bytes, 21 entries, in a 16 MiB image.
  HOST_EXTRACTION_CAPTURE = "h-hfsplus-16777216-e5"

  Expectation = Data.define(:pattern, :status, :code, :message, :path)
  EXPECTED = [
    Expectation.new(pattern: /e6-ditto\z/, status: :refused, code: -6,
      message: "HFS Plus compressed file is not supported", path: "/mixed-196608"),
    Expectation.new(pattern: /e6-(?:zlib|lzvn|lzfse)\z/, status: :refused, code: -6,
      message: "HFS Plus compressed file is not supported", path: "/forced-mixed-196608"),
    Expectation.new(pattern: /e4-dirlinks-J\z/, status: :refused, code: -6,
      message: "HFS Plus directory hard link is not supported", path: "/dl1/a/d"),
    Expectation.new(pattern: /e8-interrupted\z/, status: :refused, code: -6,
      message: "HFS Plus journal holds transactions; mount the volume to replay", path: ""),
    Expectation.new(pattern: /e6-sweep\z/, status: :refused, code: -6,
      message: "HFS Plus compressed file is not supported", path: /\A\/(?:fit|sweep)-/),
    Expectation.new(pattern: /mkfs-w(?:-J(?:-mounted)?)?\z/, status: :refused, code: -6,
      message: "HFS Plus volume inside an HFS wrapper is not supported", path: ""),
    Expectation.new(pattern: /\Ah-(?:hfsplus|hfsx)-/, status: :ok, code: nil,
      message: nil, path: nil)
  ].freeze
  # One recorder owns the counters for this run and its per-group outcomes.
  class Tally
    attr_reader :totals, :groups

    def initialize
      @totals = Hash.new(0)
      @groups = Hash.new { |groups, name| groups[name] = Hash.new(0) }
    end

    def add(key, count = 1) = @totals[key] += count
    def record(group, outcome) = @groups[group][outcome] += 1
  end
  TALLY = Tally.new

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE")).realpath
  end

  private

  def compare_capture(manifest_path)
    directory = File.dirname(manifest_path)
    name = File.basename(directory)
    image = %w[image.img image.img.gz].map { File.join(directory, _1) }.find { File.file?(_1) }
    unless image
      TALLY.add(:skips, 1)
      warn("SKIP: native manifest without image: #{manifest_path}")
      skip("native manifest without image: #{manifest_path}")
    end
    # Capture fixtures may link an image; the CLI requires its regular-file path.
    image = File.realpath(image)
    manifest = JSON.parse(File.read(manifest_path))
    expectation = EXPECTED.find { _1.pattern.match?(name) }
    refute_nil(expectation, "native capture needs an expected status: #{name}")
    refusal = expectation.status == :refused
    result = invoke(root: Dir.pwd, arguments: ["t", "--json", "-f", image], expected: refusal ? 1 : 0, timeout: 120)
    if refusal
      error = failed(result)
      code, message, path = expectation.code, expectation.message, expectation.path
      assert_equal(code, error.fetch("code"), name)
      assert_equal(message, error.fetch("message"), name)
      if path.is_a?(Regexp)
        assert_match(path, error.fetch("path"), name)
        assert(manifest.fetch("tree").any? { "/#{_1.fetch("path")}" == error.fetch("path") }, name)
      else
        assert_equal(path, error.fetch("path"), name)
      end
      TALLY.add(:refusals, 1)
      return
    end
    entries = completed(result).select { _1.fetch("type") == "entry" }
    unless manifest.key?("tree")
      TALLY.add(:skips, 1)
      skip("native capture has no tree observation: #{manifest_path}")
    end
    expected = manifest.fetch("tree").to_h { ["/#{_1.fetch("path")}", _1] }
    actual = entries.to_h { [_1.fetch("path"), _1] }
    assert_equal(entries.length, actual.length, "#{name}: duplicate listing paths")
    late_logs = actual.keys.select do |path|
      !expected.key?(path) && %r{\A/?\.fseventsd/\h{16}\z}.match?(path) && actual.fetch(path).fetch("kind") == "file"
    end
    TALLY.add(:fseventsd_logs, late_logs.length)
    differing = HfsplusNativePaths.find_difference(
      expected: expected.keys, actual: actual.keys, late_logs: late_logs)
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
      TALLY.add(:entries, 1)
    end
    compare_content(image: image, entries: entries, expected: expected, name: name)
    compare_cli(image: image, expected: expected, manifest: manifest, name: name)
    compare_host_extraction(image: image, expected: expected, late_logs: late_logs, name: name) if name == HOST_EXTRACTION_CAPTURE
    TALLY.add(:captures, 1)
    assert_nil(differing, "#{name}: first differing path #{differing} (manifest=#{expected.key?(differing)}, image=#{actual.key?(differing)})")
  end

  def compare_content(image:, entries:, expected:, name:)
    content = entries.select { %w[file symlink].include?(_1.fetch("kind")) }
    helper = @executable.dirname / "test-hfsplus"
    result = invoke(executable: helper, root: Dir.pwd, arguments: ["--native-content", image], timeout: 120)
    offset = 0
    content.each do |entry|
      path = entry.fetch("path")
      size = Integer(entry.fetch("size_bytes"))
      bytes = result.stdout.byteslice(offset, size)
      assert_equal(size, bytes&.bytesize, "#{name}: #{path}: stream length")
      if expected.key?(path)
        wanted = expected.fetch(path)
        if entry.fetch("kind") == "file" && wanted.fetch("sha256").nil?
          # Native capture cannot hash mode-0000 through the host's access
          # policy. Its size and mode remain comparable to the image reader.
          assert_equal("mode-0000", File.basename(path), "#{name}: #{path}: null hash")
          TALLY.add(:null_hashes)
        elsif entry.fetch("kind") == "file"
          assert_equal(wanted.fetch("sha256"), Digest::SHA256.hexdigest(bytes), "#{name}: #{path}: sha256")
          TALLY.add(:bytes, size)
        elsif wanted.key?("target")
          assert_equal(wanted.fetch("target").b, bytes, "#{name}: #{path}: target")
          TALLY.add(:targets)
        end
      end
      offset += size
    end
    assert_equal(offset, result.stdout.bytesize, "#{name}: no trailing stream bytes")
  end

  def cli_sample(expected)
    eligible = expected.sort.select do |path, entry|
      %w[file link].include?(entry.fetch("kind")) && entry.fetch("size") <= CLI_MEMBER_BYTE_CAP
    end
    nonempty = eligible.select { |path, entry| entry.fetch("kind") == "file" && entry.fetch("size").positive? }
    by_size = nonempty.sort_by { |path, entry| [entry.fetch("size"), path] }
    # Reserve the size extremes and a link, then spread the remaining choices
    # across sorted paths. Deduplication keeps small captures within the cap.
    required = [by_size.first, by_size.last, eligible.find { |path, entry| entry.fetch("kind") == "link" }].compact.uniq
    remaining = eligible - required
    slots = [CLI_SAMPLE_PER_CAPTURE - required.length, remaining.length].min
    spread = Array.new(slots) do |index|
      remaining.fetch(slots == 1 ? remaining.length / 2 : index * (remaining.length - 1) / (slots - 1))
    end
    (required + spread).sort
  end

  def compare_cli_bytes(image:, path:, wanted:, name:)
    # As in the sequential oracle, a mode-0000 null hash cannot attest bytes.
    return if wanted.fetch("kind") == "file" && wanted.fetch("sha256").nil?

    result = invoke(root: Dir.pwd, arguments: ["x", "-O", "-f", image, path])
    assert_equal(wanted.fetch("size"), result.stdout.bytesize, "#{name}: #{path}: x -O size")
    if wanted.fetch("kind") == "link"
      assert_equal(wanted.fetch("target").b, result.stdout, "#{name}: #{path}: x -O target")
    else
      assert_equal(wanted.fetch("sha256"), Digest::SHA256.hexdigest(result.stdout), "#{name}: #{path}: x -O sha256")
    end
    TALLY.add(:cli_content_members)
    TALLY.add(:cli_content_bytes, result.stdout.bytesize)
  end

  def compare_cli(image:, expected:, manifest:, name:)
    # i inspects a volume, not members. Selected t records supply the member
    # metadata; x -O supplies raw bytes and does not accept --json.
    information = completed(invoke(root: Dir.pwd, arguments: ["i", "--json", "-f", image]))
      .find { _1.fetch("type") == "info" }
    refute_nil(information, "#{name}: i volume information")
    assert_equal(manifest.fetch("variant"), information.fetch("format"), "#{name}: i format")
    assert_equal(manifest.fetch("size"), Integer(information.fetch("volume_bytes")), "#{name}: i volume size")
    TALLY.add(:cli_info_captures)
    cli_sample(expected).each do |path, wanted|
      records = completed(invoke(root: Dir.pwd, arguments: ["t", "--json", "-f", image, path]))
      entry = records.find { _1.fetch("type") == "entry" && _1.fetch("path") == path }
      refute_nil(entry, "#{name}: #{path}: selected member")
      kind = wanted.fetch("kind") == "link" ? "symlink" : wanted.fetch("kind")
      assert_equal(kind, entry.fetch("kind"), "#{name}: #{path}: selected kind")
      assert_equal(wanted.fetch("size"), Integer(entry.fetch("size_bytes")), "#{name}: #{path}: selected size")
      assert_equal(Integer(wanted.fetch("mode"), 8), entry.fetch("mode"), "#{name}: #{path}: selected mode")
      TALLY.add(:cli_metadata_members)
      compare_cli_bytes(image: image, path: path, wanted: wanted, name: name)
    end
  end

  def compare_host_extraction(image:, expected:, late_logs:, name:)
    Dir.mktmpdir("cli-native-", Dir.pwd) do |temporary|
      root = Pathname.new(temporary)
      destination = private_directory(root: root, name: "extracted")
      extracted = completed(invoke(root: root, arguments: ["x", "-k", "--json", "-f", image, "-E", "-C", "extracted"]))
      # Safe extraction skips links and other unsupported kinds and creates
      # private modes, without restoring image modes or timestamps. Link
      # targets are checked by x -O. Only files/directories belong on the host.
      supported = expected.select { |path, entry| %w[file directory].include?(entry.fetch("kind")) }
      assert_equal(expected.length - supported.length, Integer(extracted.last.fetch("skipped_entries")), "#{name}: host skipped kinds")
      paths = destination.glob("**/*", File::FNM_DOTMATCH)
        .reject { _1 == destination / "." }
        .map { "/#{_1.relative_path_from(destination)}" }.sort
      # Ignore only unmount-created event logs already identified by listing;
      # all manifest paths, including .fseventsd, must still be present.
      assert_nil(HfsplusNativePaths.find_difference(expected: supported.keys, actual: paths, late_logs: late_logs), "#{name}: host paths differ")
      supported.each do |path, wanted|
        host = destination / path.delete_prefix("/")
        stat = host.lstat
        assert_equal(wanted.fetch("kind"), stat.ftype, "#{name}: #{path}: host kind")
        assert_equal(wanted.fetch("kind") == "directory" ? 0o700 : 0o600, stat.mode & 0o7777, "#{name}: #{path}: private host mode")
        if wanted.fetch("kind") == "file"
          assert_equal(wanted.fetch("size"), stat.size, "#{name}: #{path}: host size")
          # A null manifest hash has the same host-policy limitation as the
          # sequential check; size and kind remain independently comparable.
          unless wanted.fetch("sha256").nil?
            assert_equal(wanted.fetch("sha256"), Digest::SHA256.file(host).hexdigest, "#{name}: #{path}: host sha256")
          end
        end
        TALLY.add(:host_entries)
      end
      TALLY.add(:host_extractions)
    end
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
          TALLY.record(group, :passed)
        rescue Minitest::Skip
          TALLY.record(group, :skipped)
          raise
        rescue Minitest::Assertion, CliTest::Error, JSON::ParserError, KeyError, ArgumentError, TypeError, SystemCallError, Timeout::Error
          TALLY.record(group, :failed)
          raise
        end
      end
    end
  else
    def test_native_captures_require_explicit_directory
      TALLY.add(:skips, 1)
      skip("FSIMG_HFSPLUS_NATIVE unset; native trees not compared")
    end
  end
end

Minitest.after_run do
  totals = HfsplusNativeTest::TALLY.totals
  puts "Native group | passed | failed | skipped"
  HfsplusNativeTest::TALLY.groups.sort.each do |name, counts|
    puts "#{name} | #{counts[:passed]} | #{counts[:failed]} | #{counts[:skipped]}"
  end
  puts "Native trees: #{totals[:captures]} captures compared, #{totals[:entries]} entries compared, " \
    "#{totals[:bytes]} file bytes hashed, #{totals[:targets]} symlink targets compared, #{totals[:refusals]} refusals checked; " \
    "#{totals[:cli_info_captures]} CLI info captures checked, #{totals[:cli_metadata_members]} CLI member metadata checks, " \
    "#{totals[:cli_content_members]} x -O members compared (#{totals[:cli_content_bytes]} bytes), " \
    "#{totals[:host_extractions]} host extractions (#{totals[:host_entries]} entries compared); " \
    "skipped #{totals[:skips]} captures, #{totals[:null_hashes]} null file hashes (mode-0000), #{totals[:fseventsd_logs]} late fseventsd logs; timestamps and ownership not compared"
end
