# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"
require "fileutils"
require "find"
require "json"
require "open3"
require "tmpdir"

# Corpus names are public. H3 output is sent directly through counting
# filters; neither names nor its label enter this process or a log file.
module IsoIntegration
  class Error < StandardError; end

  module_function

  def command(*arguments)
    output, error, status = Open3.capture3({ "LC_ALL" => "C", "TZ" => "UTC" }, *arguments)
    raise Error, "command failed (#{status.exitstatus}): #{error.strip}" unless status.success?

    output
  end

  def display(path) = path.split("/", -1).map { _1.delete_suffix(";1") }.join("/")

  def records(output) = output.lines.map { JSON.parse(_1) }

  def manifest(image)
    base = image.delete_suffix(".iso")
    paths = File.readlines("#{base}.primary.paths", chomp: true).sort
    rows = File.readlines("#{base}.primary.jsonl", chomp: true).map { JSON.parse(_1) }
    raise Error, "oracle paths and JSON disagree" unless rows.map { _1.fetch("path") }.sort == paths.uniq

    unique_pairs(rows.map do |row|
      kind = row.fetch("type")
      [display(row.fetch("path")), { kind: kind, size: kind == "file" ? row.fetch("size") : 0,
                                     sha256: row.fetch("sha256") }]
    end)
  end

  def unique_pairs(pairs)
    result = pairs.to_h
    raise Error, "duplicate displayed path" unless pairs.size == result.size

    result
  end

  def extracted(root)
    Find.find(root).reject { _1 == root }.to_h do |path|
      stat = File.lstat(path)
      kind = if stat.directory?
        "directory"
      elsif stat.file?
        "file"
      else
        raise Error, "unexpected extracted kind"
      end
      [path.delete_prefix(root), { kind: kind, size: stat.file? ? stat.size : 0,
                                   sha256: stat.file? ? Digest::SHA256.file(path).hexdigest : nil }]
    end
  end

  def compare(expected:, actual:, content:)
    unless expected.keys.sort == actual.keys.sort
      raise Error, "path set differs (expected #{expected.size}, observed #{actual.size})"
    end

    expected.each do |path, row|
      other = actual.fetch(path)
      fields = content ? %i[kind size sha256] : %i[kind size]
      fields.each do |field|
        raise Error, "#{path.inspect}: #{field} differs" unless row.fetch(field) == other.fetch(field)
      end
    end
  end

  def boot_catalog_block(descriptors)
    sectors = descriptors.scan(/boot catalog is in sector (\d+)/i).flatten
    raise Error, "multiple boot catalogs in oracle" if sectors.size > 1

    sectors.empty? ? 0 : Integer(sectors.fetch(0), 10)
  end

  def compare_boot(image:, binary:, options:)
    report = command("xorriso", "-indev", image, "-report_el_torito", "plain")
    rows = report.lines.grep(/(?:El Torito )?Boot img\s*:/i)
    platforms = rows.map do |row|
      match = row.match(/Boot img\s*:\s+\d+\s+(\S+)/i)
      raise Error, "unrecognized xorriso boot row" unless match

      match[1]
    end
    sector = boot_catalog_block(command("isoinfo", "-d", "-i", image))
    saved_sector = boot_catalog_block(File.read("#{image}.descriptors"))
    raise Error, "saved and live isoinfo boot catalogs differ" unless sector == saved_sector
    raise Error, "xorriso and isoinfo disagree on boot presence" unless sector.positive? == !rows.empty?

    expected = { "boot_catalog_block" => sector, "boot_entries" => rows.size,
                 "boot_x86" => platforms.count("BIOS"), "boot_efi" => platforms.count("UEFI") }
    info = records(command(binary, "i", "--json", *options)).select { _1.fetch("type") == "info" }
    raise Error, "expected one volume information record" unless info.size == 1

    details = info.fetch(0).fetch("details")
    actual = expected.keys.to_h { [_1, Integer(details.fetch(_1), 10)] }
    raise Error, "boot details differ: oracle #{expected.inspect}, fsimg #{actual.inspect}" unless expected == actual

    "ok (isoinfo catalog=#{sector}; xorriso entries=#{rows.size} BIOS=#{platforms.count('BIOS')} " \
      "UEFI=#{platforms.count('UEFI')}; fsimg #{actual.values.join('/')})"
  end

  # Permission strings are decoded independently of fsimg's numeric JSON.
  def mode_bits(text)
    ordinary = text[1, 9].chars.zip([256, 128, 64, 32, 16, 8, 4, 2, 1]).sum do |char, bit|
      %w[- S T].include?(char) ? 0 : bit
    end
    ordinary | (%w[s S].include?(text[3]) ? 0o4000 : 0) |
      (%w[s S].include?(text[6]) ? 0o2000 : 0) | (%w[t T].include?(text[9]) ? 0o1000 : 0)
  end

  def kind(text)
    { "-" => "file", "d" => "directory", "l" => "symlink", "p" => "fifo", "f" => "fifo" }.fetch(text[0])
  end

  def listing_row(mode:, uid:, gid:, size:, date:, member:)
    name, target = mode.start_with?("l") ? member.split(" -> ", 2) : [member, nil]
    raise Error, "symlink without target: #{member.inspect}" if mode.start_with?("l") && target.nil?

    [name, { kind: kind(mode), mode: mode_bits(mode), uid: Integer(uid, 10), gid: Integer(gid, 10),
             size: Integer(size, 10), date: date.split.join(" "), target: target }]
  end

  def isoinfo_tree(image)
    header_pattern = /^Directory listing of (.*)\n$/
    sections = command("isoinfo", "-R", "-l", "-i", image).lines.slice_before { header_pattern.match?(_1) }
    row_pattern = Regexp.new(
      '^\s*(?:\d+\s+)?([dlfp-][rwxstST-]{9})\s+\d+\s+(\d+)\s+(\d+)\s+(\d+)\s+' \
      '(\w+\s+\d+\s+\d{4})\s+\[.*?\]\s(.*)$'
    )
    pairs = sections.flat_map do |section|
      next [] if section.all? { _1.strip.empty? }

      header = section.first.match(header_pattern)
      raise Error, "isoinfo section lacks directory header" unless header

      parent = header[1].delete_suffix("/")
      section.drop(1).reject { _1.strip.empty? }.filter_map do |line|
        row = line.match(row_pattern)
        raise Error, "unparsed isoinfo row: #{line.inspect}" unless row

        name, values = listing_row(mode: row[1], uid: row[2], gid: row[3], size: row[4],
                                  date: row[5], member: row[6].delete_suffix(" "))
        next if %w[. ..].include?(name)

        ["#{parent}/#{name}", values]
      end
    end
    # -R -l supplies logical CL placeholders even when -R -f omits them.
    # The relocation directory, when emitted, remains an ordinary directory;
    # no path is filtered by a magic rr_moved spelling.
    unique_pairs(pairs)
  end

  def bsdtar_name(text)
    # In the C locale bsdtar quotes non-ASCII bytes as octal escapes.
    text.b.gsub(/\\([0-7]{3}|\\)/n) do
      escaped = Regexp.last_match(1)
      escaped == "\\" ? "\\".b : Integer(escaped, 8).chr(Encoding::BINARY)
    end.force_encoding(Encoding::UTF_8)
  end

  def bsdtar_tree(image)
    pairs = command("bsdtar", "-tv", "--numeric-owner", "-f", image).lines.filter_map do |line|
      row = line.match(Regexp.new(
        '^([dlp-][rwxstST-]{9})\s+\d+\s+(\d+)\s+(\d+)\s+(\d+)\s+' \
        '(\w+\s+\d+\s+(?:\d{2}:\d{2}|\d{4}))\s(.*)\n$'
      ))
      raise Error, "unparsed bsdtar row: #{line.inspect}" unless row

      name, values = listing_row(mode: row[1], uid: row[2], gid: row[3], size: row[4],
                                date: row[5], member: bsdtar_name(row[6]))
      next if name == "." || name == "./"

      ["/#{name.delete_prefix('./').delete_suffix('/')}", values]
    end
    unique_pairs(pairs)
  end

  def same_paths(expected:, actual:, oracle:)
    missing = expected.keys - actual.keys
    extra = actual.keys - expected.keys
    return if missing.empty? && extra.empty?

    raise Error, "#{oracle} paths differ: missing=#{missing.sort.inspect}, extra=#{extra.sort.inspect}"
  end

  def field_equal(path:, field:, expected:, actual:)
    return if expected == actual

    raise Error, "#{path.inspect}: #{field} expected #{expected.inspect}, observed #{actual.inspect}"
  end

  def compare_oracle(expected:, actual:, oracle:)
    same_paths(expected: expected, actual: actual, oracle: oracle)
    expected.each do |path, row|
      entry = actual.fetch(path)
      %i[kind mode uid gid target].each do |field|
        field_equal(path: path, field: "#{oracle} #{field}", expected: row.fetch(field), actual: entry.fetch(field))
      end
      if row.fetch(:kind) == "file"
        field_equal(path: path, field: "#{oracle} size", expected: row.fetch(:size), actual: entry.fetch(:size))
      end
      time = Time.at(entry.fetch(:modified)).utc
      # isoinfo prints a calendar day; bsdtar prints minute precision for
      # recent members and only a calendar year for older members.
      date = if oracle == "isoinfo"
        # isoinfo prints the recorded calendar date without applying TZ.
        time.getlocal(entry.fetch(:offset_minutes) * 60).strftime("%b %-d %Y")
      elsif row.fetch(:date).include?(":")
        time.strftime("%b %-d %H:%M")
      else
        time.strftime("%b %-d %Y")
      end
      field_equal(path: path, field: "#{oracle} modified", expected: row.fetch(:date), actual: date)
    end
  end

  def fsimg_tree(binary:, options:)
    opened = records(command(binary, "i", "--json", *options)).find { _1.fetch("type") == "info" }
    raise Error, "info auto profile must be rockridge" unless opened && opened.fetch("profile") == "rockridge"

    decoded = records(command(binary, "t", "--json", *options))
    info = decoded.find { _1.fetch("type") == "info" }
    unless info && info.fetch("profile") == "rockridge"
      raise Error, "auto profile must be rockridge: #{info&.fetch('profile').inspect}"
    end

    unique_pairs(decoded.select { _1.fetch("type") == "entry" }.map do |row|
      target = row.fetch("kind") == "symlink" ? command(binary, "x", "-O", *options, row.fetch("path")) : nil
      [row.fetch("path"), { kind: row.fetch("kind"), size: Integer(row.fetch("size_bytes"), 10),
                             uid: row.fetch("uid"), gid: row.fetch("gid"), mode: row.fetch("mode"),
                             modified: Integer(row.fetch("modified").fetch("seconds"), 10),
                             offset_minutes: row.fetch("modified").fetch("offset_minutes"), target: target }]
    end)
  end

  def compare_verbose(binary:, options:, actual:)
    pairs = command(binary, "t", "-v", *options).lines.map do |line|
      row = line.match(/^([dlp-][rwxstST-]{9})\s+(\d+|-)\s+(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\s+(".*")[\/@|*]?\n$/)
      raise Error, "unparsed fsimg verbose row: #{line.inspect}" unless row

      [JSON.parse(row[4]), { kind: kind(row[1]), mode: mode_bits(row[1]), date: row[3], size: row[2] }]
    end
    verbose = unique_pairs(pairs)
    same_paths(expected: actual, actual: verbose, oracle: "fsimg verbose")
    actual.each do |path, entry|
      row = verbose.fetch(path)
      %i[kind mode].each do |field|
        field_equal(path: path, field: "verbose #{field}", expected: entry.fetch(field), actual: row.fetch(field))
      end
      field_equal(path: path, field: "verbose modified",
                  expected: Time.at(entry.fetch(:modified)).utc.strftime("%Y-%m-%d %H:%M:%S"), actual: row.fetch(:date))
      next unless %w[file symlink].include?(entry.fetch(:kind))

      field_equal(path: path, field: "verbose size",
                  expected: entry.fetch(:size), actual: Integer(row.fetch(:size), 10))
    end
  end

  def content_digest(binary:, options:, path:)
    digest = Digest::SHA256.new
    size = 0
    Open3.popen3(binary, "x", "-O", *options, path) do |input, output, error, worker|
      input.close
      while (bytes = output.read(1024 * 1024))
        size += bytes.bytesize
        digest.update(bytes)
      end
      diagnostic = error.read
      raise Error, "#{path.inspect}: content read failed: #{diagnostic.strip}" unless worker.value.success?
    end
    [size, digest.hexdigest]
  end

  def compare_rr_manifest(binary:, options:, image:, actual:)
    path = image.delete_suffix(".iso") + ".rockridge.jsonl"
    raise Error, "Rock Ridge manifest absent" unless File.file?(path)

    rows = records(File.read(path))
    # Isoinfo's -f manifests omit CL directory placeholders. Compare the
    # complete logical path set with -l above and all file paths here.
    expected_files = rows.select { _1.fetch("type") == "file" }.to_h { [_1.fetch("path"), _1] }
    actual_files = actual.select { |_path, entry| entry.fetch(:kind) == "file" }
    same_paths(expected: expected_files, actual: actual_files, oracle: "manifest files")
    rows.each do |row|
      path = row.fetch("path")
      entry = actual.fetch(path) { raise Error, "manifest path missing: #{path.inspect}" }
      expected_kind = row.fetch("type") == "link" ? "symlink" : row.fetch("type")
      field_equal(path: path, field: "manifest kind", expected: expected_kind, actual: entry.fetch(:kind))
      if row.fetch("type") == "link"
        field_equal(path: path, field: "manifest target", expected: row.fetch("target"), actual: entry.fetch(:target))
      end
    end
    expected_files.each do |path, row|
      size, digest = content_digest(binary: binary, options: options, path: path)
      field_equal(path: path, field: "manifest byte count", expected: row.fetch("size"), actual: size)
      field_equal(path: path, field: "manifest SHA-256", expected: row.fetch("sha256"), actual: digest)
    end
    "ok (#{expected_files.size} SHA-256)"
  end

  def compare_rr_extraction(binary:, options:, image:, actual:)
    manifest_path = image.delete_suffix(".iso") + ".rockridge.jsonl"
    raise Error, "Rock Ridge extraction manifest absent" unless File.file?(manifest_path)

    rows = records(File.read(manifest_path))
    expected = unique_pairs(rows.map do |row|
      kind = row.fetch("type") == "link" ? "symlink" : row.fetch("type")
      [row.fetch("path"), { kind: kind, size: kind == "file" ? row.fetch("size") : 0,
                            sha256: row.fetch("sha256") }]
    end)
    # The -R -f manifest omits CL placeholders. Their descendant paths
    # independently imply those directory ancestors; -R -l checks empty
    # directories and the complete logical listing separately.
    ancestors = expected.keys.flat_map do |path|
      parts = path.split("/").drop(1)
      (1...parts.size).map { "/#{parts.take(_1).join('/')}" }
    end.uniq.to_h do |path|
      [path, { kind: "directory", size: 0, sha256: nil }]
    end
    skipped = expected.select { |_path, entry| %w[symlink fifo].include?(entry.fetch(:kind)) }
    expected = ancestors.merge(expected).reject { |path, _entry| skipped.key?(path) }
    supported = actual.select { |_path, entry| %w[file directory].include?(entry.fetch(:kind)) }
    Dir.mktmpdir("rr-extraction-", Dir.pwd) do |destination|
      _output, diagnostic, status = Open3.capture3(
        { "LC_ALL" => "C", "TZ" => "UTC" }, binary, "x", *options, "-E", "-k", "-v", "-C", destination
      )
      raise Error, "extraction failed (#{status.exitstatus}): #{diagnostic.strip}" unless status.success?

      skipped_names = diagnostic.lines.filter_map do |line|
        row = line.match(/\A(".*")[@|]\n\z/)
        JSON.parse(row[1]) if row
      end
      field_equal(path: image, field: "skipped paths", expected: skipped.keys.sort, actual: skipped_names.sort)
      observed = extracted(destination)
      same_paths(expected: expected, actual: observed, oracle: "extraction manifest")
      same_paths(expected: supported, actual: observed, oracle: "extraction listing")
      expected.each do |path, entry|
        fields = entry.fetch(:kind) == "file" ? %i[kind size sha256] : %i[kind]
        fields.each do |field|
          field_equal(path: path, field: "extracted #{field}",
                      expected: entry.fetch(field), actual: observed.fetch(path).fetch(field))
        end
      end
    end
    files = expected.count { |_path, row| row.fetch(:kind) == "file" }
    "ok (#{expected.size} paths, #{files} SHA-256, #{skipped.size} skipped)"
  end

  def rockridge(binary:, image:, options:)
    actual = fsimg_tree(binary: binary, options: options)
    results = []
    failures = []
    steps = {
      "verbose" => -> { compare_verbose(binary: binary, options: options, actual: actual) },
      "isoinfo" => -> { compare_oracle(expected: isoinfo_tree(image), actual: actual, oracle: "isoinfo") },
      "bsdtar" => -> { compare_oracle(expected: bsdtar_tree(image), actual: actual, oracle: "bsdtar") },
      "manifest" => -> { compare_rr_manifest(binary: binary, options: options, image: image, actual: actual) },
      "extraction" => -> { compare_rr_extraction(binary: binary, options: options, image: image, actual: actual) }
    }
    steps.each do |label, check|
      # This is the single documented libarchive CL/RE connection failure.
      if label == "bsdtar" && File.basename(image) == "xorriso-deep.iso"
        results << "bsdtar excluded (documented xorriso CL/RE failure)"
        next
      end
      begin
        detail = check.call
        results << "#{label} #{%w[manifest extraction].include?(label) ? detail : 'ok'}"
      rescue Error, JSON::ParserError, KeyError, ArgumentError => error
        results << "#{label} FAIL"
        failures << "#{label}: #{error.message}"
      end
    end
    puts "#{File.basename(image)}: rockridge auto; #{results.join(', ')}"
    failures.each { warn "#{File.basename(image)}: #{_1}" }
    failures.empty?
  end

  def corpus(binary:, directory:, scratch:)
    images = Dir.glob(File.join(directory, "*.iso")).sort
    raise Error, "no producer images" if images.empty?

    failed = false
    images.each_with_index do |image, index|
      label = File.basename(image)
      forcing = label.include?("isohybrid") ? ["-F", "iso9660/-"] : []
      if command("isoinfo", "-d", "-i", image).match?(/^Rock Ridge signatures version 1 found/)
        begin
          agreement = rockridge(binary: binary, image: image, options: [*forcing, "-f", image])
          failed = true unless agreement
        rescue Error, JSON::ParserError, KeyError, ArgumentError => error
          warn "#{label}: Rock Ridge comparison failed: #{error.message}"
          puts "#{label}: rockridge FAIL"
          failed = true
        end
      end
      options = [*forcing, "-o", "view=primary", "-f", image]
      listing = "FAIL"
      extraction = "FAIL"
      boot = "FAIL"
      reasons = []
      begin
        expected = manifest(image)
        info = command(binary, "i", *options)
        raise Error, "format detection differs" unless info.match?(/^Format:\s+ISO9660$/)

        decoded = records(command(binary, "t", "--json", *options))
        actual = unique_pairs(decoded.select { _1.fetch("type") == "entry" }.map do |row|
          [row.fetch("path"), { kind: row.fetch("kind"), size: Integer(row.fetch("size_bytes"), 10) }]
        end)
        compare(expected: expected, actual: actual, content: false)
        listing = "ok"
      rescue Error, JSON::ParserError, KeyError, Errno::ENOENT => error
        reasons << "listing: #{error.message}"
      end
      begin
        expected = manifest(image)
        destination = File.join(scratch, "image-#{index}")
        Dir.mkdir(destination, 0o700)
        command(binary, "x", *options, "-E", "-C", destination)
        compare(expected: expected, actual: extracted(destination), content: true)
        extraction = "ok"
      rescue Error, JSON::ParserError, KeyError, Errno::ENOENT => error
        reasons << "extraction: #{error.message}"
      ensure
        FileUtils.remove_entry_secure(destination) if destination && File.directory?(destination)
      end
      begin
        boot = compare_boot(image: image, binary: binary, options: options)
      rescue Error, JSON::ParserError, KeyError, ArgumentError, Errno::ENOENT => error
        reasons << "boot: #{error.message}"
      end
      puts "#{label}: listing #{listing}, extraction #{extraction}, boot #{boot}"
      reasons.each { warn "#{label}: #{_1}" }
      failed ||= !reasons.empty?
    end
    !failed
  end

  # A filter's stdout consists only of counts. Check *every* pipeline
  # status so an oracle failure cannot masquerade as an empty tree.
  def counts(*pipeline)
    output = nil
    statuses = nil
    Open3.pipeline_r(*pipeline, err: File::NULL) do |stream, workers|
      output = stream.read
      statuses = workers.map(&:value)
    end
    raise Error, "private-input pipeline failed" unless statuses.all?(&:success?)
    raise Error, "count filter returned non-count data" unless output.match?(/\A[0-9]+(?: [0-9]+)*\n\z/)

    output.split.map { Integer(_1, 10) }
  end

  def private_disc(binary:, image:)
    info = counts([binary, "i", "-f", image],
                  ["awk", '/^Label:/ { next } /^Format:[[:space:]]+ISO9660$/ { n++ } END { print n+0 }'])
    raise Error, "H3 format differs" unless info == [1]

    fsimg = counts([binary, "t", "-v", "-f", image],
                   ["awk", '/^d/ { d++; next } /^-/ { f++ } END { print f+0, d+1 }'])
    # Primary files have a numeric version; directory identifiers do not.
    # This oracle classification includes empty directories and the root.
    oracle = counts(["isoinfo", "-f", "-i", image],
                    ["awk", '/;[0-9]+$/ { f++; next } /^\// { d++ } END { print f+0, d+1 }'])
    puts "H3: fsimg files=#{fsimg[0]} directories=#{fsimg[1]}; " \
         "isoinfo files=#{oracle[0]} directories=#{oracle[1]} (root included)"
    raise Error, "H3 counts differ" unless fsimg == oracle && oracle == [7763, 2204]
  end

  def other_probes(binary:, scratch:)
    programs = %w[fat12 fat16 fat32 exfat qnx6 qnx4 ifs]
    programs.each do |format|
      seed = File.join(scratch, "#{format}.seed")
      arguments = if format.start_with?("fat")
        ["test-fat", "--seed-variant", seed, format.delete_prefix("fat")]
      else
        ["test-#{format}", "--seed", seed]
      end
      command(File.join(File.dirname(binary), arguments.first), *arguments.drop(1))
      command(File.join(File.dirname(binary), "test-iso9660"), "--reject", seed)
      File.unlink(seed)
    end
    puts "Exclusivity: ISO probe rejects all 7 existing nonempty format seeds"
  end

  def call(binary:)
    success = Dir.mktmpdir("iso-integration-", Dir.pwd) do |scratch|
      other_probes(binary: binary, scratch: scratch)
      corpus(binary: binary, directory: ENV.fetch("FSIMG_ISO_CORPUS"), scratch: scratch)
    end
    private_disc(binary: binary, image: ENV.fetch("FSIMG_ISO_H3"))
    success
  end
end

begin
  exit(IsoIntegration.call(binary: ARGV.fetch(0)) ? 0 : 1)
rescue IsoIntegration::Error => error
  warn error.message
  exit 1
end
