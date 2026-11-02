# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require_relative "spec/lib/fsimg_spec"
require_relative "iso9660-corpus"
require_relative "iso9660-integration"
require "time"
require "shellwords"

# Writer qualification uses synthetic, public fixtures, independently checked
# against their source specifications by test-iso9660-write. Manifests retain
# complete paths and stream file digests; payload bytes never accumulate here.
module IsoWriter
  class Error < FsimgSpec::Error; end
  class PrimaryProjectionCollision < Error; end
  Fixture = Data.define(:profile, :level)
  VIEWS = %w[rockridge joliet primary].freeze
  LONG_COMPONENT = (("0123456789" * 25) + "abcde").freeze
  LONG_TARGET = ("/usr/" + ([LONG_COMPONENT] * 3).join("/") + "/" + ("0123456789" * 25)).freeze
  LIBARCHIVE_LONG_TARGET = ("/usr" + (LONG_COMPONENT * 3) + ("0123456789" * 25)).freeze
  LIBARCHIVE_LINK_EXCEPTIONS = { "/long-target" => [LONG_TARGET, LIBARCHIVE_LONG_TARGET].freeze }.freeze
  XORRISO_LINK_EXCEPTIONS = LIBARCHIVE_LINK_EXCEPTIONS.merge(
    "/odd" => ["a//b/./../c/", "a/b/./../c/"].freeze
  ).freeze

  module_function

  def command(*arguments, combined: false)
    output = IsoCorpus.capture(*arguments, environment: IsoIntegration::COMMAND_ENVIRONMENT, combined: combined)
    IsoIntegration.utf8_text(output)
  rescue IsoCorpus::Error => error
    raise Error, error.message
  end

  def tool(name:, fallback:)
    configured = ENV.fetch("FSIMG_TOOL_#{name}", "")
    candidates = configured.empty? ? Array(fallback) : [configured]
    candidates.filter_map { IsoCorpus.executable(_1) || (File.executable?(_1) ? _1 : nil) }.first
  end

  def options(image:, view:) = ["-S", "off", "-F", "iso9660", "-o", "view=#{view}", "-f", image]

  def unique(pairs)
    result = pairs.to_h
    raise Error, "duplicate manifest path" unless pairs.size == result.size

    result
  end

  def canonical_path(path:, view:) = view == "rockridge" ? path : IsoIntegration.display(path)

  def digest(command:)
    hash, size = IsoCorpus.content_digest(command, environment: IsoIntegration::COMMAND_ENVIRONMENT)
    { "sha256" => hash, "size" => size }
  rescue IsoCorpus::Error => error
    raise Error, error.message
  end

  def image_manifest(binary:, image:, view:)
    arguments = options(image: image, view: view)
    records = IsoIntegration.records(command(binary, "t", "--json", *arguments))
    entries = records.select { _1.fetch("type") == "entry" }
    unique(entries.map do |entry|
      path = entry.fetch("path")
      kind = entry.fetch("kind")
      content = case kind
      when "file"
        digest(command: [binary, "x", "-O", *arguments, path])
      when "symlink"
        { "target" => command(binary, "x", "-O", *arguments, path),
          "size" => Integer(entry.fetch("size_bytes"), 10) }
      else
        { "size" => 0 }
      end
      metadata = %w[modified accessed changed].to_h do |field|
        time = entry.fetch(field)
        [field, time && Integer(time.fetch("seconds"), 10)]
      end
      [path, { "kind" => kind, "uid" => entry.fetch("uid"), "gid" => entry.fetch("gid"),
               "mode" => entry.fetch("mode") }.merge(metadata, content)]
    end)
  end

  def file_digest(path)
    hash = Digest::SHA256.new
    File.open(path, "rb") do |file|
      while (bytes = file.read(1024 * 1024))
        hash.update(bytes)
      end
    end
    hash.hexdigest
  end

  def host_manifest(root:, view:)
    unique(Find.find(root).reject { _1 == root }.map do |path|
      stat = File.lstat(path)
      kind = { "file" => "file", "directory" => "directory", "link" => "symlink", "fifo" => "fifo" }
        .fetch(stat.ftype) { raise Error, "unexpected host entry kind #{stat.ftype}" }
      content = case kind
      when "file" then { "size" => stat.size, "sha256" => file_digest(path) }
      when "symlink"
        target = File.readlink(path)
        { "size" => target.bytesize, "target" => target }
      else { "size" => 0 }
      end
      relative = path.delete_prefix(root)
      [canonical_path(path: relative, view: view),
       { "kind" => kind, "uid" => stat.uid, "gid" => stat.gid, "mode" => stat.mode & 0o7777,
         "modified" => stat.mtime.to_i, "accessed" => stat.atime.to_i,
         "changed" => stat.ctime.to_i }.merge(content)]
    end)
  end

  def compare(expected:, actual:, context:, metadata: true, target_exceptions: {}, omitted_directory: nil)
    missing = expected.keys - actual.keys
    extra = actual.keys - expected.keys
    # Affected libarchive versions suppress the root rr_moved directory
    # entry, including its metadata. Only that exact entry is exempted;
    # every descendant and every unrelated entry remains mandatory.
    omit_container = omitted_directory && missing == [omitted_directory] &&
      expected.fetch(omitted_directory).fetch("kind") == "directory"
    unless (missing.empty? || omit_container) && extra.empty?
      raise Error, "#{context}: paths differ: missing=#{missing.inspect}, extra=#{extra.inspect}"
    end
    expected.each do |path, row|
      if omit_container && path == omitted_directory
        puts "EXCEPTION: #{context} omits directory entry and metadata #{path.inspect}; descendants remain required"
        next
      end

      other = actual.fetch(path)
      source_target, observed_target = target_exceptions[path]
      target_exception = source_target && row["kind"] == "symlink" && other["kind"] == "symlink" &&
        row["target"] == source_target && other["target"] == observed_target &&
        (!row.key?("size") || row.fetch("size") == source_target.bytesize) &&
        (!other.key?("size") || other.fetch("size") == observed_target.bytesize)
      puts "EXCEPTION: #{context} measured link target #{path.inspect}" if target_exception
      row.each do |field, value|
        next if value.nil?
        next if !metadata && %w[uid gid mode accessed changed].include?(field)
        # Only the exact separator-loss result of this known source target
        # is an exception. Other targets, sizes and missing bytes still fail.
        next if target_exception && %w[target size].include?(field)
        raise Error, "#{context}: #{path.inspect} #{field} differs" unless value == other[field]
      end
    end
  end

  def fixtures
    ordinary = (1..3).flat_map do |level|
      %i[full short relocated case].map { Fixture.new(profile: _1, level: level) }
    end
    ordinary += [Fixture.new(profile: :nonempty_container, level: 3)]
    ENV["FSIMG_ISO_LARGE"] == "1" ? ordinary + [Fixture.new(profile: :large, level: 3)] : ordinary
  end

  def create(helper:, binary:, fixture:, image:)
    if fixture.profile == :nonempty_container
      source = File.join(File.dirname(image), "source")
      FileUtils.mkdir_p(File.join(source, "rr_moved"))
      File.binwrite(File.join(source, "rr_moved", "member.txt"), "ordinary directory member\n".b)
      # Optical consumers may read a whole transfer window past a tiny
      # hierarchy. Padding this fixture does not change the writer's minimum.
      command(binary, "c", "-S", "off", "-R", "-e", "1700000000", "-F", "iso9660", "-s", "128K",
              "-o", "level=#{fixture.level}", "-f", image, "-C", source, ".")
      return
    end
    arguments = case fixture.profile
    when :full then ["--create", image, fixture.level.to_s]
    when :short then ["--create", image, fixture.level.to_s, "short"]
    when :relocated then ["--create-relocated", image, fixture.level.to_s]
    when :case then ["--create-case", image, fixture.level.to_s]
    when :large then ["--create-large", image]
    else raise Error, "unknown writer fixture profile"
    end
    command(helper, *arguments)
  end

  def case_sensitive?(directory:)
    Dir.mktmpdir("case-check-", directory) do |root|
      File.binwrite(File.join(root, "Case"), "".b)
      !File.exist?(File.join(root, "case"))
    end
  end

  def bsdtar_options(view:)
    case view
    when "rockridge" then []
    when "joliet" then ["--options", "iso9660:!rockridge"]
    when "primary" then ["--options", "iso9660:!rockridge,iso9660:!joliet"]
    end
  end

  def find_libarchive_directory_exception(fixture:, view:)
    return unless view == "rockridge"

    case fixture.profile
    when :full, :short, :nonempty_container then "/rr_moved"
    when :relocated then "/.rr_moved"
    end
  end

  # macOS libarchive converts Joliet names to decomposed UTF-8. This adapter
  # compares canonical equivalence, not original byte spelling. Applying it
  # independently to both sets rejects any many-to-one name normalization.
  def libarchive_joliet_projection(rows:)
    unique(rows.map { |path, row| [path.unicode_normalize(:nfc), row] })
  end

  # Libarchive displays an extensionless primary file without the mandatory
  # ISO trailing dot. A directory keeps its spelling; a file/directory pair
  # collapsing to one displayed name is refused rather than hidden.
  def libarchive_primary_projection(rows:)
    pairs = rows.map do |path, row|
      [row.fetch("kind") == "file" ? path.delete_suffix(".") : path, row]
    end
    projected = pairs.to_h
    raise PrimaryProjectionCollision, "libarchive primary file-dot display collapses distinct entries" unless projected.size == pairs.size

    projected
  end

  def libarchive_paths(rows:, view:)
    if view == "primary"
      libarchive_primary_projection(rows: rows)
    elsif view == "joliet" && RUBY_PLATFORM.include?("darwin")
      libarchive_joliet_projection(rows: rows)
    else
      rows
    end
  end

  def isoinfo(image:, view:, expected:, program:)
    rows = IsoIntegration.isoinfo_tree(image, view: view, program: program)
    actual = unique(rows.map do |path, row|
      [canonical_path(path: path, view: view),
       { "kind" => row.fetch(:kind), "size" => row.fetch(:kind) == "file" ? row.fetch(:size) : 0 }]
    end)
    basic = expected.transform_values do |row|
      { "kind" => row.fetch("kind"), "size" => row.fetch("kind") == "file" ? row.fetch("size") : 0 }
    end
    compare(expected: basic, actual: actual, context: "isoinfo #{view}", metadata: false)
  end

  def xorriso_listing(output)
    unique(output.lines.filter_map do |line|
      next unless line.match?(/\A[dlp-][rwxstST-]{9}\s/)

      fields = Shellwords.split(line)
      case fields
      in [mode, _links, uid, gid, size, month, day, clock, path, *tail]
        next if path == "/"

        kind = IsoIntegration.kind(mode)
        target = if kind == "symlink"
          case tail
          in ["->", text] then text
          else raise Error, "unframed xorriso link target"
          end
        end
        [path, { "kind" => kind, "mode" => IsoIntegration.mode_bits(mode),
                 "uid" => Integer(uid, 10), "gid" => Integer(gid, 10),
                 "size" => kind == "file" ? Integer(size, 10) : target&.bytesize || 0, "target" => target,
                 "modified_date" => [month, day, clock].join(" ") }]
      else
        raise Error, "unframed xorriso listing row"
      end
    end)
  rescue ArgumentError => error
    raise Error, "unframed xorriso listing: #{error.message}"
  end

  def xorriso(image:, expected:, program:)
    output = command(program, "-no_rc", "-abort_on", "WARNING", "-indev", image,
                     "-check_media", "--", "-find", "/", "-exec", "lsdl", "--")
    actual = xorriso_listing(output)
    wanted = expected.to_h do |path, row|
      date = actual[path]&.fetch("modified_date") || ""
      precision = date.include?(":") ? "%b %-d %H:%M" : "%b %-d %Y"
      fields = row.slice("kind", "uid", "gid", "mode", "target")
        .merge("modified_date" => Time.at(row.fetch("modified")).utc.strftime(precision))
      fields = fields.merge("size" => row.fetch("size")) if row.fetch("kind") == "file"
      [path, fields]
    end
    compare(expected: wanted, actual: actual, context: "xorriso Rock Ridge metadata",
            target_exceptions: XORRISO_LINK_EXCEPTIONS)
    expected.each do |path, row|
      next unless row.fetch("kind") == "file"

      content = digest(command: [program, "-no_rc", "-abort_on", "WARNING", "-indev", image,
                                 "-osirrox", "on", "-concat", "overwrite", "-", path, "--"])
      compare(expected: { path => row.slice("size", "sha256") }, actual: { path => content },
              context: "xorriso Rock Ridge content")
    end
  end

  def bsdtar(image:, view:, expected:, program:, scratch:, fixture:)
    flags = bsdtar_options(view: view)
    begin
      expected = libarchive_paths(rows: expected, view: view)
    rescue PrimaryProjectionCollision => error
      puts "UNSUPPORTED: #{error.message}; this view is not qualified by libarchive"
      return :unsupported
    end
    listing = IsoIntegration.bsdtar_tree(image, view: view, program: program)
    listed = unique(listing.map do |path, row|
      [canonical_path(path: path, view: view),
       { "kind" => row.fetch(:kind), "mode" => row.fetch(:mode), "uid" => row.fetch(:uid),
         "gid" => row.fetch(:gid), "target" => row.fetch(:target), "modified_date" => row.fetch(:date),
         "archive_path" => path }]
    end)
    if view == "joliet" && RUBY_PLATFORM.include?("darwin")
      puts "ADAPTER: macOS libarchive Joliet path spelling is compared by canonical Unicode equivalence"
    elsif view == "primary"
      puts "ADAPTER: libarchive primary trailing-dot display convention; injective paths and exact content compared"
    end
    listed = libarchive_paths(rows: listed, view: view)
    wanted = expected.to_h do |path, row|
      date = listed[path]&.fetch("modified_date") || ""
      precision = date.include?(":") ? "%b %-d %H:%M" : "%b %-d %Y"
      [path, row.slice("kind", "mode", "uid", "gid", "target")
        .merge("modified_date" => Time.at(row.fetch("modified")).utc.strftime(precision))]
    end
    exception = find_libarchive_directory_exception(fixture: fixture, view: view)
    exception = nil if listed.key?(exception)
    compare(expected: wanted, actual: listed, context: "bsdtar listing #{view}",
            target_exceptions: LIBARCHIVE_LINK_EXCEPTIONS, omitted_directory: exception)
    if fixture.profile == :large || !case_sensitive?(directory: scratch)
      puts "SKIP: bsdtar whole-tree extraction on case-insensitive host filesystem" unless fixture.profile == :large
      bsdtar_stream(image: image, view: view, expected: expected, program: program,
                    members: listed.transform_values { _1.fetch("archive_path") })
      return
    end
    root = File.join(scratch, "bsdtar-#{view}")
    Dir.mkdir(root, 0o700)
    command(program, "-x", *flags, "-f", image, "-C", root)
    actual = host_manifest(root: root, view: view)
    actual = libarchive_paths(rows: actual, view: view)
    # When the archive omits this header, extracting its children may create
    # the parent. Its kind remains observable; stored metadata is unavailable.
    extracted_expected = if exception
      expected.merge(exception => expected.fetch(exception).slice("kind", "size"))
    else
      expected
    end
    # Extraction as an ordinary user does not restore foreign ownership;
    # the numeric listing below checks metadata without privilege.
    compare(expected: extracted_expected, actual: actual, context: "bsdtar #{view}", metadata: false,
            target_exceptions: view == "rockridge" ? LIBARCHIVE_LINK_EXCEPTIONS : {},
            omitted_directory: exception)
  end

  def bsdtar_stream(image:, view:, expected:, program:, members:)
    flags = bsdtar_options(view: view)
    expected.each do |path, row|
      next unless row.fetch("kind") == "file"

      actual = digest(command: [program, "-xO", *flags, "-f", image, members.fetch(path).delete_prefix("/")])
      compare(expected: { path => row.slice("size", "sha256") }, actual: { path => actual },
              context: "bsdtar content #{view}")
    end
  end

  def sevenzip(image:, expected:, program:, scratch:, fixture:)
    output = command(program, "l", "-slt", "-ba", image)
    actual = unique(output.split(/\n\s*\n/).reject(&:empty?).map do |block|
      row = block.lines.map { _1.chomp.split(" = ", 2) }.to_h
      path = canonical_path(path: "/#{row.fetch('Path')}", view: "joliet")
      kind = row.fetch("Folder") == "+" ? "directory" : "file"
      [path, { "kind" => kind, "size" => kind == "file" ? Integer(row.fetch("Size"), 10) : 0,
               "modified" => Time.iso8601(row.fetch("Modified").tr(" ", "T") + "Z").to_i }]
    end)
    wanted = expected.transform_values { _1.slice("kind", "size", "modified") }
    compare(expected: wanted, actual: actual, context: "7z Joliet listing", metadata: false)
    if fixture.profile == :large || !case_sensitive?(directory: scratch)
      puts "SKIP: 7z whole-tree extraction on case-insensitive host filesystem" unless fixture.profile == :large
      expected.each do |path, row|
        next unless row.fetch("kind") == "file"

        observed = digest(command: [program, "x", "-so", "-ssc", "-y", image, path.delete_prefix("/")])
        compare(expected: { path => row.slice("size", "sha256") }, actual: { path => observed },
                context: "7z Joliet content")
      end
      return
    end
    root = File.join(scratch, "7z")
    Dir.mkdir(root, 0o700)
    command(program, "x", "-y", "-o#{root}", image)
    compare(expected: expected, actual: host_manifest(root: root, view: "joliet"),
            context: "7z Joliet extraction", metadata: false)
  end

  def call(build:)
    binary = ENV.fetch("FSIMG_ISO_BINARY", File.join(build, "fsimg"))
    helper = ENV.fetch("FSIMG_ISO_WRITER_HELPER", File.join(build, "test-iso9660-write"))
    programs = { isoinfo: tool(name: "ISOINFO", fallback: "isoinfo"),
                 isovfy: tool(name: "ISOVFY", fallback: "isovfy"),
                 xorriso: tool(name: "XORRISO", fallback: "xorriso"),
                 bsdtar: tool(name: "BSDTAR", fallback: "bsdtar"),
                 sevenzip: tool(name: "SEVENZIP", fallback: %w[7zz 7z]) }
    programs.each { |name, path| puts "SKIP: #{name} is unavailable" unless path }
    raise Error, "no independent ISO consumers are available" if programs.values.all?(&:nil?)

    fixtures.each do |fixture|
      Dir.mktmpdir("iso-writer-", build) do |scratch|
        ran = []
        image = File.join(scratch, "#{fixture.profile}-#{fixture.level}.iso")
        create(helper: helper, binary: binary, fixture: fixture, image: image)
        manifests = VIEWS.to_h do |view|
          [view, image_manifest(binary: binary, image: image, view: view)]
        end
        if programs[:xorriso]
          xorriso(image: image, expected: manifests.fetch("rockridge"), program: programs.fetch(:xorriso))
          ran << :xorriso
        end
        if fixture.profile == :short && programs[:isovfy]
          output = command(programs.fetch(:isovfy), image, combined: true)
          raise Error, "isovfy reported findings" unless output.match?(/No errors found/i)
          ran << :isovfy
        end
        VIEWS.each do |view|
          expected = manifests.fetch(view)
          if programs[:isoinfo]
            isoinfo(image: image, view: view, expected: expected, program: programs[:isoinfo])
            ran << :isoinfo
          end
          if programs[:bsdtar]
            result = bsdtar(image: image, view: view, expected: expected, program: programs[:bsdtar],
                            scratch: scratch, fixture: fixture)
            ran << :bsdtar unless result == :unsupported
          end
        end
        if programs[:sevenzip]
          sevenzip(image: image, expected: manifests.fetch("joliet"),
                   program: programs.fetch(:sevenzip), scratch: scratch, fixture: fixture)
          ran << :sevenzip
        end
        if ran.empty?
          puts "SKIP: ISO writer #{fixture.profile} level #{fixture.level}: no applicable independent consumer"
        else
          puts "ISO writer #{fixture.profile} level #{fixture.level}: #{ran.uniq.join(', ')} passed"
        end
      end
    end
    puts "SKIP: large external image (set FSIMG_ISO_LARGE=1)" unless ENV["FSIMG_ISO_LARGE"] == "1"
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    case ARGV
    in [build]
      IsoWriter.call(build: File.expand_path(build))
    in ["--manifest", image, build]
      binary = ENV.fetch("FSIMG_ISO_BINARY", File.join(File.expand_path(build), "fsimg"))
      puts JSON.generate(IsoWriter::VIEWS.to_h do |view|
        [view, IsoWriter.image_manifest(binary: binary, image: File.expand_path(image), view: view)]
      end)
    else
      raise IsoWriter::Error, "usage: iso9660-writer.rb BUILD | --manifest IMAGE BUILD"
    end
  rescue IsoWriter::Error, IsoIntegration::Error, KeyError, Errno::ENOENT => error
    warn error.message
    exit 1
  end
end
