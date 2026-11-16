# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "json"
require "minitest/autorun"
require "pathname"
require_relative "../cli/process"

require "digest"
require "fileutils"
require "tmpdir"

class ApfsContentNativeTest < Minitest::Test
  include CliProcesses

  KINDS = { "dir" => "directory", "link" => "symlink",
            "char" => "char_device", "block" => "block_device" }.freeze
  TIMES = { "modified" => "mtime_ns", "changed" => "ctime_ns",
            "accessed" => "atime_ns", "created" => "btime_ns" }.freeze

  private

  def native_records(path)
    text = File.read(path)
    text.lstrip.start_with?("[") ? JSON.parse(text) : text.lines.reject { _1.strip.empty? }.map { JSON.parse(_1) }
  end

  def clone_or_copy_image(source:, destination:)
    if RUBY_PLATFORM.include?("darwin")
      begin
        system("cp", "-c", source, destination, exception: true)
        return
      rescue RuntimeError
        # The temporary filesystem may not support cloning.
      end
    end
    File.open(source, "rb") do |input|
      File.open(destination, "wb") { IO.copy_stream(input, _1) }
    end
  end

  def tamper_node_free_space(path:, block:)
    File.open(path, "r+b") do |image|
      image.seek(block * 4096 + 40)
      toc_offset, toc_length, free_offset, free_length = image.read(8).unpack("v4")
      free_start = 56 + toc_offset + toc_length + free_offset
      assert_operator(free_length, :>, 0, "block #{block} has free space")
      assert_operator(free_start + free_length, :<=, 4096, "block #{block} free space is in bounds")
      position = block * 4096 + free_start + free_length / 2
      image.seek(position)
      byte = image.read(1).unpack1("C")
      image.seek(position)
      image.write([byte ^ 0x5a].pack("C"))
    end
  end

  def image_path(path)
    relative = path.delete_prefix("/").delete_prefix("./")
    ["", "."].include?(relative) ? "/" : "/#{relative}"
  end

  def excluded?(path) = path == "/" || path == "/.fseventsd" || path.start_with?("/.fseventsd/")

  def index_paths(records, include_event_logs: false)
    entries = records.filter_map do |record|
      path = image_path(record.fetch("path"))
      [path, record] unless path == "/" || (!include_event_logs && excluded?(path))
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

  def setup
    @executable = File.expand_path(ENV.fetch("FSIMG_CLI_EXECUTABLE", "./fsimg"))
    @root = Pathname.new(Dir.mktmpdir("apfs-content-", Dir.pwd))
    @partition_arguments = []
  end

  def teardown
    FileUtils.remove_entry(@root) if @root
  end

  def native_directory
    directory = ENV["FSIMG_APFS_NATIVE"]
    skip("FSIMG_APFS_NATIVE unset; APFS native contents not compared") unless directory
    File.expand_path(directory)
  end

  def listed_entries(image, include_event_logs: false)
    result = invoke(arguments: ["t", "--json", *@partition_arguments, "-f", image], expected: nil, timeout: 180)
    assert_equal(0, result.status, "#{image}: t --json\n#{result.stdout}\n#{result.stderr}")
    records = result.stdout.lines.map { JSON.parse(_1) }
    assert_equal("summary", records.last.fetch("type"), image)
    assert_equal(true, records.last.fetch("complete"), image)
    index_paths(records.select { _1.fetch("type") == "entry" }, include_event_logs: include_event_logs)
  end

  def compare_listing(image:, manifest:, directory:)
    source = native_records(manifest)
    expected = index_paths(source)
    root = source.find { image_path(_1.fetch("path")) == "/" }
    refute_nil(root, "manifest includes root")
    actual = listed_entries(image)
    assert_equal(expected.keys.sort, actual.keys.sort, "#{directory}: exact listing paths")
    owners = disk_owners(directory)
    errors = expected.flat_map do |path, entry|
      compare_entry(path: path, expected: entry, actual: actual.fetch(path), owners: owners, mounter: root)
    end
    assert_empty(errors, "#{directory}:\n#{errors.join("\n")}")
    expected
  end

  # Drain stdout in bounded chunks. Even a multi-GiB hole uses only a digest
  # and one chunk; no output file or complete stdout string is retained.
  def stdout_digest(image:, path:)
    reader, writer = IO.pipe
    digest_reader = Thread.new do
      digest = Digest::SHA256.new
      count = 0
      begin
        loop do
          chunk = reader.readpartial(1024 * 1024)
          digest.update(chunk)
          count += chunk.bytesize
        end
      rescue EOFError
        [count, digest.hexdigest]
      ensure
        reader.close
      end
    end
    child = start_process([@executable, "x", "-O", *@partition_arguments, "-l", "output=unlimited", "-f", image, path],
      cwd: @root, stdout: writer)
    writer.close
    _output, errors = child.finish(timeout: 600)
    assert_equal(0, child.status, "#{image}: #{path}: #{errors}")
    digest_reader.value
  ensure
    writer&.close unless writer&.closed?
    digest_reader&.join
  end

  def extract_and_compare(image:, expected:)
    destination = @root / "extract"
    destination.mkdir
    # Select leaves so the excluded event log and multi-GiB file cannot be
    # pulled in recursively through a selected parent directory. Selecting
    # a leaf also preserves its ancestors. Empty directories need operands.
    selected = expected.reject { |_path, entry| entry.fetch("kind") == "file" && entry.fetch("size") > 0xffffffff }
    members = selected.filter_map do |path, entry|
      directory = KINDS.fetch(entry.fetch("kind"), entry.fetch("kind")) == "directory"
      path unless directory && expected.keys.any? { _1.start_with?("#{path}/") }
    end
    arguments = ["x", "-k", *@partition_arguments, "-f", image, "-C", destination.to_s, *members]
    invoke(arguments: arguments, timeout: 300)
    roots = destination.children
    assert_equal(1, roots.length, "one extracted image root")
    extracted = roots.first
    wanted = selected.select { |_path, entry| %w[file dir directory].include?(entry.fetch("kind")) }
    actual = extracted.glob("**/*", File::FNM_DOTMATCH).reject { _1.relative_path_from(extracted).to_s == "." }
      .map { "/#{_1.relative_path_from(extracted)}" }.reject { excluded?(_1) }.sort
    assert_equal(wanted.keys.sort, actual, "exact safe extraction paths")
    wanted.each do |path, entry|
      output = extracted / path.delete_prefix("/")
      if entry.fetch("kind") == "file"
        assert(output.file?, path)
        assert_equal(entry.fetch("size"), output.size, path)
        assert_equal(entry.fetch("sha256"), Digest::SHA256.file(output).hexdigest, path)
      else
        assert(output.directory?, path)
      end
    end
  ensure
    FileUtils.remove_entry(destination) if destination&.exist?
  end

  def compare_capture(manifest)
    directory = File.dirname(manifest)
    image = File.join(directory, "image.img")
    REPORT[:compared] += 1
    expected = compare_listing(image: image, manifest: manifest, directory: directory)
    extract_and_compare(image: image, expected: expected)
    expected.select { |_path, entry| entry.fetch("kind") == "file" }.each do |path, entry|
      size, digest = stdout_digest(image: image, path: path)
      assert_equal(entry.fetch("size"), size, "#{directory}: #{path}: stdout length")
      assert_equal(entry.fetch("sha256"), digest, "#{directory}: #{path}: stdout digest")
    end
    REPORT[:matched] += 1
    puts "APFS content matched: #{File.basename(directory)} (#{expected.length} entries)"
  end

  # Capture paths can contain spaces; only the fixed prefix is tokenized.
  def sealed_manifest(path)
    kinds = { "Directory" => "directory", "Regular File" => "file", "Symbolic Link" => "symlink" }
    lines = File.readlines(path, chomp: true)
    assert_match(/\A# mount: /, lines.first, "sealed manifest mount header")
    entries = {}
    digests = {}
    lines.drop(1).each do |line|
      case line
      when /\Aentry (Directory|Regular File|Symbolic Link) ([0-7]+) (\d+) (\..*)\z/
        kind, mode, size, member = Regexp.last_match.captures
        member = image_path(member)
        next if member == "/"

        refute(entries.key?(member), "duplicate sealed entry: #{member}")
        entries[member] = { kind: kinds.fetch(kind), mode: Integer(mode, 8), size: Integer(size, 10) }.freeze
      when /\Asha256 ([0-9a-f]{64})  (\..*)\z/
        digest, member = Regexp.last_match.captures
        member = image_path(member)
        refute(digests.key?(member), "duplicate sealed digest: #{member}")
        digests[member] = digest
      else
        flunk("unrecognized sealed manifest line: #{line.inspect}")
      end
    end
    [entries.freeze, digests.freeze]
  end

  public

  REPORT = { compared: 0, matched: 0 }
  directory = ENV["FSIMG_APFS_NATIVE"]
  if directory
    manifests = Dir[File.join(File.expand_path(directory), "**", "native-manifest.{json,ndjson}")]
      .sort.group_by { File.dirname(_1) }.values.map(&:first)
    raise ArgumentError, "no native manifests in #{directory}" if manifests.empty?

    # Generate a discoverable test for each capture so a failure cannot hide
    # the remaining specimens. Each member is exercised by these tests.
    manifests.each do |manifest|
      name = File.basename(File.dirname(manifest)).tr("-", "_")
      define_method("test_native_contents_match_manifest_#{name}") { compare_capture(manifest) }
    end
  else
    def test_native_contents_require_capture_directory
      native_directory
    end
  end

  def test_native_decmpfs_supported_files_match_digests_and_lzbitmap_is_refused
    directory = File.join(native_directory, "apfs-newfs-67108864-e7-compression")
    image = File.join(directory, "image.img")
    records = native_records(File.join(directory, "native-decmpfs.jsonl"))
    supported, refused = records.partition { ![13, 14].include?(_1.fetch("type")) }
    assert_equal(59, supported.length, "supported native compressed files")
    assert_equal(11, refused.length, "native LZBITMAP files")
    supported.each do |record|
      path = record.fetch("path").sub(%r{\A.*?/apfs-mount[^/]*}, "")
      assert_equal([record.fetch("usize"), record.fetch("sha256")],
        stdout_digest(image: image, path: path), "type #{record.fetch("type")}: #{path}")
    end
    refused.each do |record|
      path = record.fetch("path").sub(%r{\A.*?/apfs-mount[^/]*}, "")
      result = invoke(arguments: ["x", "-O", "-f", image, path], expected: nil)
      refute_equal(0, result.status, "type #{record.fetch("type")}: #{path}")
      assert_includes(result.stderr, "not supported", path)
    end
  end

  def test_structural_verification_completes_for_native_compressed_captures
    %w[e7-compression e6-xclone].each do |capture|
      image = File.join(native_directory, "apfs-newfs-67108864-#{capture}", "image.img")
      result = invoke(arguments: ["W", "-f", image], expected: nil, timeout: 180)
      assert_equal(0, result.status, "#{capture}: #{result.stdout}\n#{result.stderr}")
      refute_match(/chunk tables|incomplete/i, result.stdout + result.stderr, capture)
    end
  end

  def test_sealed_capture_structural_verification_completes
    image = File.join(native_directory, "e11-sealed", "app.dmg")
    skip("sealed capture unavailable: #{image}") unless File.file?(image)

    result = invoke(arguments: ["W", "-f", image], expected: 0, timeout: 180)
    assert_includes(result.stdout, "filesystem structure verified")
    refute_match(/incomplete/i, result.stdout + result.stderr)
    report = invoke(arguments: ["W", "--json", "-f", image], expected: 0, timeout: 180)
    verification = report.stdout.lines.map { JSON.parse(_1) }.find { _1.fetch("type") == "verify" }
    refute_nil(verification, "structural verification emits a report")
    assert_equal(true, verification.fetch("complete"))
  end

  def test_sealed_capture_structural_verification_rejects_tampered_nodes
    source = File.join(native_directory, "e11-sealed", "app.dmg")
    skip("sealed capture unavailable: #{source}") unless File.file?(source)

    cases = [
      { name: "index", block: 692, message: "does not match its digest" },
      { name: "leaf", block: 366, message: "does not match its digest" },
      { name: "sealed extents", block: 233, message: "bad checksum" }
    ]
    cases.each do |damage|
      image = (@root / "tampered-#{damage.fetch(:block)}.img").to_s
      clone_or_copy_image(source: source, destination: image)
      tamper_node_free_space(path: image, block: damage.fetch(:block))
      result = invoke(arguments: ["W", "--json", "-f", image], expected: 1, timeout: 180)
      error = result.stdout.lines.map { JSON.parse(_1) }.find { _1.fetch("type") == "error" }
      refute_nil(error, damage.fetch(:name))
      assert_equal(-5, error.fetch("code"), "#{damage.fetch(:name)}: FSIMG_ECORRUPT")
      assert_includes(error.fetch("message"), damage.fetch(:message), damage.fetch(:name))
    end
  end

  def test_sealed_capture_lists_manifest_and_salvages_supported_files
    directory = File.join(native_directory, "e11-sealed")
    image = File.join(directory, "app.dmg")
    skip("sealed capture unavailable: #{image}") unless File.file?(image)

    entries, digests = sealed_manifest(File.join(directory, "native-manifest.txt"))
    actual = listed_entries(image, include_event_logs: true)
    assert_equal(entries.keys.sort, actual.keys.sort, "sealed manifest has exactly the listed entries")
    entries.each do |path, entry|
      listed = actual.fetch(path)
      assert_equal(entry.fetch(:kind), listed.fetch("kind"), path)
      assert_equal(entry.fetch(:mode) & 0o7777, listed.fetch("mode") & 0o7777, path)
      next if entry.fetch(:kind) == "directory"

      assert_equal(entry.fetch(:size), Integer(listed.fetch("size_bytes"), 10), path)
    end
    files = entries.select { |_path, entry| entry.fetch(:kind) == "file" }
    assert_equal(files.keys.sort, digests.keys.sort, "one digest for every regular file")

    destination = @root / "sealed-extract"
    destination.mkdir(0o700)
    result = invoke(arguments: ["x", "--salvage", "--json", "-f", image, "-C", destination.to_s],
      expected: 1, timeout: 600)
    errors = result.stdout.lines.map { JSON.parse(_1) }.select { _1.fetch("type") == "error" }
    assert_equal(1, errors.length, "one incomplete extraction result")
    error = errors.first
    assert_equal(-14, error.fetch("code"), "FSIMG_EPARTIAL")
    assert_equal(false, error.fetch("complete"))
    assert_equal("incomplete", error.fetch("output_state"))
    assert_equal(0, error.fetch("cleanup_error"))
    assert_equal("1394", error.fetch("salvage_losses"))
    roots = destination.children
    assert_equal(1, roots.length, "one extracted sealed image root")
    extracted = roots.first
    present, absent = files.keys.partition { (extracted / _1.delete_prefix("/")).file? }
    # E11: the application cryptex's files other than its LZBITMAP ones.
    assert_equal(1238, present.length, "supported regular files extracted")
    assert_equal(1394, absent.length, "LZBITMAP regular files refused")
    extracted_kinds = extracted.glob("**/*", File::FNM_DOTMATCH)
      .reject { _1.relative_path_from(extracted).to_s == "." }.to_h do |output|
        kind = output.lstat.ftype
        ["/#{output.relative_path_from(extracted)}", kind == "link" ? "symlink" : kind]
      end
    wanted_kinds = entries.reject { |path, _entry| absent.include?(path) }.transform_values { _1.fetch(:kind) }
    assert_equal(wanted_kinds, extracted_kinds, "exact extracted paths and kinds, including empty directories")
    present.each do |path|
      output = extracted / path.delete_prefix("/")
      assert_equal(files.fetch(path).fetch(:size), output.size, path)
      assert_equal(digests.fetch(path), Digest::SHA256.file(output).hexdigest, path)
    end
    absent.sort.first(25).each do |path|
      refusal = invoke(arguments: ["x", "-O", "-f", image, path], expected: nil, timeout: 180)
      refute_equal(0, refusal.status, path)
      assert_includes(refusal.stderr, "compression type is not supported", path)
    end
    REPORT[:compared] += 1
    REPORT[:matched] += 1
  end

  def test_unused_truncated_tail_preserves_listing_and_extraction
    directory = File.join(native_directory, "apfs-newfs-67108864-e6-sparse")
    image = (@root / "cut.img").to_s
    # Independent apfsdump records place the last mapped data at block 232,
    # the filesystem root at 239, the APSB at 245, and the container map
    # and its root at 246 and 247. Keep all 248 blocks; the remaining
    # declared container tail is unreferenced by listing and content reads.
    File.open(File.join(directory, "image.img"), "rb") do |source|
      File.open(image, "wb") { IO.copy_stream(source, _1, 248 * 4096) }
    end
    expected = compare_listing(image: image,
      manifest: File.join(directory, "native-manifest.ndjson"), directory: directory)
    extract_and_compare(image: image, expected: expected)
    expected.select { |_path, entry| entry.fetch("kind") == "file" }.each do |path, entry|
      assert_equal([entry.fetch("size"), entry.fetch("sha256")],
        stdout_digest(image: image, path: path), path)
    end

    # Cut through the container object-map root, which both commands read.
    File.truncate(image, 247 * 4096 + 2048)
    %w[t x].each do |command|
      arguments = [command, "-f", image]
      arguments += ["-k", "-C", @root.to_s] if command == "x"
      result = invoke(arguments: arguments, expected: 1)
      assert_includes(result.stderr, "image is truncated", command)
    end
  end

  def test_gpt_volume_beside_efi_lists_and_extracts_with_selection
    image = File.join(native_directory, "apfs-diskutil-17179869184-e18-gpt", "s5.img")
    skip("GPT resize specimen unavailable: #{image}") unless File.file?(image)

    # The disk also contains a readable EFI filesystem in partition 1.
    @partition_arguments = ["--partition=2"]
    entries = listed_entries(image)
    assert_empty(entries, "GPT resize specimen has only excluded event logs")
    extract_and_compare(image: image, expected: entries)
    puts "APFS wrapper apfs/gpt: E18 has only excluded event logs; no native manifest"
  end

  def test_wrapped_empty_volumes_list_and_extract_identically
    directory = File.join(native_directory, "e13-layers")
    forms = {
      "e13-gpt-udrw.dmg" => "apfs/gpt",
      "e13-gpt-udzo.dmg" => "apfs/gpt/dmg",
      "e13-gpt-udbz.dmg" => "apfs/gpt/dmg",
      "e13-apm-native.img" => "apfs/apm",
      "e13-apm-typed.img" => "apfs/apm",
      "e13-apm-udzo.dmg" => "apfs/apm/dmg",
      "e13-none-udzo.dmg" => "apfs/dmg"
    }
    reference = nil
    forms.each do |name, form|
      image = File.join(directory, name)
      assert(File.file?(image), "missing #{form} specimen: #{name}")
      entries = listed_entries(image)
      assert_empty(entries, "#{name}: captured E13 volume has no members")
      reference ||= entries
      assert_equal(reference, entries, "#{name}: identical entries across forms")
      extract_and_compare(image: image, expected: entries)
      puts "APFS wrapper #{form}: #{name}; empty E13 specimen"
    end
  end
end

Minitest.after_run do
  report = ApfsContentNativeTest::REPORT
  puts "APFS native content: #{report.fetch(:compared)} captures compared, #{report.fetch(:matched)} matched"
end
