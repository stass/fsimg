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
  COMMAND_ENVIRONMENT = { "LC_ALL" => "C.UTF-8", "TZ" => "UTC" }.freeze

  module_function

  # Metadata commands emit UTF-8 bytes regardless of the process locale.
  # Payload commands use the separate streaming digest path.
  def utf8_text(output) = output.dup.force_encoding(Encoding::UTF_8)

  def command(*arguments)
    output, error, status = Open3.capture3(COMMAND_ENVIRONMENT, *arguments)
    raise Error, "command failed (#{status.exitstatus}): #{error.strip}" unless status.success?

    utf8_text(output)
  end

  def display(path) = path.split("/", -1).map { _1.delete_suffix(";1") }.join("/")

  def records(output) = utf8_text(output).lines.map { JSON.parse(_1) }

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
    { "-" => "file", "d" => "directory", "l" => "symlink", "p" => "fifo", "f" => "fifo",
      "c" => "char_device", "b" => "block_device", "s" => "socket" }.fetch(text[0])
  end

  def manifest_kind(type)
    { "link" => "symlink", "characterSpecial" => "char_device", "blockSpecial" => "block_device" }.fetch(type, type)
  end

  def listing_row(mode:, uid:, gid:, size:, date:, member:)
    name, target = mode.start_with?("l") ? member.split(" -> ", 2) : [member, nil]
    raise Error, "symlink without target: #{member.inspect}" if mode.start_with?("l") && target.nil?

    [name, { kind: kind(mode), mode: mode_bits(mode), uid: Integer(uid, 10), gid: Integer(gid, 10),
             size: Integer(size, 10), date: date.split.join(" "), target: target }]
  end

  def isoinfo_tree(image, view: "rockridge", program: "isoinfo")
    header_pattern = /^Directory listing of (.*)\n$/
    flags = { "rockridge" => ["-R"], "joliet" => ["-J"], "primary" => [] }.fetch(view)
    sections = command(program, *flags, "-j", "utf-8", "-l", "-i", image).lines.slice_before { header_pattern.match?(_1) }
    row_pattern = Regexp.new(
      '^\s*(?:\d+\s+)?([bcdlfps-][rwxstST-]{9})\s+\d+\s+(\d+)\s+(\d+)\s+(\d+)\s+' \
      '(\w+\s+\d+\s+\d{4})\s+\[.*?\](.*)$'
    )
    pairs = sections.flat_map do |section|
      next [] if section.all? { _1.strip.empty? }

      header = section.first.match(header_pattern)
      raise Error, "isoinfo section lacks directory header" unless header

      parent = header[1].delete_suffix("/")
      rows = section.drop(1).reject { _1.strip.empty? }.map do |line|
        row = line.match(row_pattern)
        raise Error, "unparsed isoinfo row: #{line.inspect}" unless row

        row
      end
      # The known first dot entry identifies the tool's fixed delimiter;
      # removing that exact prefix preserves spaces belonging to a filename.
      dot = rows.first&.[](6)&.delete_suffix(" ")&.match(/\A( +)\.\z/)
      raise Error, "isoinfo section lacks first dot entry" unless dot

      delimiter = dot[1]
      rows.filter_map do |row|
        # Ordinary rows append one display space; symlink rows end directly
        # after the target, including an empty or space-terminated target.
        member = row[1].start_with?("l") ? row[6] : row[6].delete_suffix(" ")
        raise Error, "isoinfo row lacks fixed delimiter" unless member.start_with?(delimiter)

        name, values = listing_row(mode: row[1], uid: row[2], gid: row[3], size: row[4],
                                  date: row[5], member: member.delete_prefix(delimiter))
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

  def bsdtar_tree(image, view: "rockridge", program: "bsdtar")
    options = case view
    when "rockridge" then []
    when "joliet" then ["--options", "iso9660:!rockridge"]
    when "primary" then ["--options", "iso9660:!rockridge,iso9660:!joliet"]
    else raise Error, "unknown ISO view"
    end
    # libarchive selects Joliet when its ISO reader has Rock Ridge disabled.
    pairs = command(program, "-tv", *options, "--numeric-owner", "-f", image).lines.filter_map do |line|
      row = line.match(Regexp.new(
        '^([bcdlps-][rwxstST-]{9})\s+\d+\s+(\d+)\s+(\d+)\s+(\d+(?:,\s*\d+)?)\s+' \
        '(\w+\s+\d+\s+(?:\d{2}:\d{2}|\d{4}))\s(.*)\n$'
      ))
      raise Error, "unparsed bsdtar row: #{line.inspect}" unless row

      size = %w[b c].include?(row[1][0]) ? "0" : row[4]
      name, values = listing_row(mode: row[1], uid: row[2], gid: row[3], size: size,
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

  def fsimg_tree(binary:, options:, profile: "rockridge")
    opened = records(command(binary, "i", "--json", *options)).find { _1.fetch("type") == "info" }
    raise Error, "info profile must be #{profile}" unless opened && opened.fetch("profile") == profile

    decoded = records(command(binary, "t", "--json", *options))
    info = decoded.find { _1.fetch("type") == "info" }
    unless info && info.fetch("profile") == profile
      raise Error, "profile must be #{profile}: #{info&.fetch('profile').inspect}"
    end

    unique_pairs(decoded.select { _1.fetch("type") == "entry" }.map do |row|
      target = row.fetch("kind") == "symlink" ? command(binary, "x", "-O", *options, row.fetch("path")) : nil
      [row.fetch("path"), { kind: row.fetch("kind"), size: Integer(row.fetch("size_bytes"), 10),
                             uid: row.fetch("uid"), gid: row.fetch("gid"), mode: row.fetch("mode"),
                             dev_major: row.fetch("dev_major"), dev_minor: row.fetch("dev_minor"),
                             modified: Integer(row.fetch("modified").fetch("seconds"), 10),
                             offset_minutes: row.fetch("modified").fetch("offset_minutes"), target: target }]
    end)
  end

  def compare_verbose(binary:, options:, actual:)
    pairs = command(binary, "t", "-v", *options).lines.map do |line|
      row = line.match(/^([bcdlps-][rwxstST?-]{9})\s+(\d+(?:,\s*\d+)?|-)\s+(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\s+(".*")[\/@|*=]?\n$/)
      raise Error, "unparsed fsimg verbose row: #{line.inspect}" unless row

      [JSON.parse(row[4]), { kind: kind(row[1]), mode: row[1].end_with?("?????????") ? nil : mode_bits(row[1]), date: row[3], size: row[2] }]
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
      expected_kind = manifest_kind(row.fetch("type"))
      field_equal(path: path, field: "manifest kind", expected: expected_kind, actual: entry.fetch(:kind))
      if row.fetch("type") == "link"
        field_equal(path: path, field: "manifest target", expected: row.fetch("target"), actual: entry.fetch(:target))
      end
    end
    devices = actual.select { |_path, entry| %w[char_device block_device].include?(entry.fetch(:kind)) }
    unless devices.empty?
      device_path = image.delete_suffix(".iso") + ".devices.jsonl"
      raise Error, "device manifest absent" unless File.file?(device_path)

      device_rows = records(File.read(device_path))
      expected_devices = unique_pairs(device_rows.map { [_1.fetch("path"), _1] })
      same_paths(expected: expected_devices, actual: devices, oracle: "device manifest")
      expected_devices.each do |path, row|
        %i[dev_major dev_minor].each do |field|
          field_equal(path: path, field: "device manifest #{field}",
                      expected: row.fetch(field.to_s), actual: devices.fetch(path).fetch(field))
        end
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
      kind = manifest_kind(row.fetch("type"))
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
    skipped = expected.reject { |_path, entry| %w[file directory].include?(entry.fetch(:kind)) }
    expected = ancestors.merge(expected).reject { |path, _entry| skipped.key?(path) }
    supported = actual.select { |_path, entry| %w[file directory].include?(entry.fetch(:kind)) }
    Dir.mktmpdir("rr-extraction-", Dir.pwd) do |destination|
      output, diagnostic, status = Open3.capture3(
        { "LC_ALL" => "C", "TZ" => "UTC" }, binary, "x", *options, "-E", "-k", "--json", "-C", destination
      )
      raise Error, "extraction failed (#{status.exitstatus}): #{diagnostic.strip}" unless status.success?

      summary = records(output).last
      field_equal(path: image, field: "skipped count", expected: skipped.size,
                  actual: Integer(summary.fetch("skipped_entries"), 10))
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
    forced = fsimg_tree(binary: binary, options: [*options, "-o", "view=rockridge"])
    raise Error, "forced Rock Ridge and automatic view differ" unless forced == actual

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

  # Isoinfo maps each non-BMP UTF-16 code unit to an underscore. Only
  # that documented projection is allowed; all BMP names compare exactly.
  def isoinfo_joliet_path(path)
    path.codepoints.map { _1 > 0xffff ? "__" : _1.chr(Encoding::UTF_8) }.join
  end

  def joliet_manifest(image)
    records(File.read(image.delete_suffix(".iso") + ".joliet.jsonl")).to_h do |row|
      [display(row.fetch("path")), row]
    end
  end

  def compare_view_contents(binary:, options:, image:, joliet_files:)
    primary_rows = records(File.read(image.delete_suffix(".iso") + ".primary.jsonl"))
    views = { "primary" => primary_rows }
    rr_path = image.delete_suffix(".iso") + ".rockridge.jsonl"
    if command("isoinfo", "-d", "-i", image).include?("Rock Ridge signatures version 1 found")
      views["rockridge"] = records(File.read(rr_path))
    end
    # Manifests pair projected names by source digest and size. Compare
    # multiplicities as well: duplicated or missing content must not pass.
    views.each do |view, rows|
      files = rows.select { _1.fetch("type") == "file" }
      expected = files.map { [_1.fetch("size"), _1.fetch("sha256")] }.tally
      if view == "primary"
        field_equal(path: image, field: "#{view}/Joliet file content multiset",
                    expected: expected, actual: joliet_files.values.tally)
      else
        # RR links and FIFOs are projected to zero-length regular files
        # in the other trees. Pair regular files common to both views.
        expected.each do |content, count|
          available = joliet_files.values.tally.fetch(content, 0)
          raise Error, "#{view}/Joliet common content #{content.inspect}: need #{count}, found #{available}" if count > available
        end
      end
      files.each do |row|
        path = view == "primary" ? display(row.fetch("path")) : row.fetch("path")
        observed = content_digest(binary: binary, options: [*options, "-o", "view=#{view}"], path: path)
        field_equal(path: path, field: "#{view}/Joliet paired SHA-256 and size",
                    expected: [row.fetch("size"), row.fetch("sha256")], actual: observed)
      end
    end
    views.keys.join("/")
  end

  def xorriso_joliet(binary:, image:, options:)
    forced = [*options, "-o", "view=joliet"]
    _output, error, status = Open3.capture3(binary, "t", "-v", *forced)
    unless status.exitstatus == 1 && error.include?("parent record does not name its parent")
      raise Error, "forced Joliet expected ECORRUPT exit 1 and parent diagnostic, " \
                   "observed exit #{status.exitstatus}: #{error.strip}"
    end
    files = joliet_manifest(image).select { |_path, row| row.fetch("type") == "file" }
      .transform_values { [_1.fetch("size"), _1.fetch("sha256")] }
    paired = compare_view_contents(binary: binary, options: options, image: image, joliet_files: files)
    puts "#{File.basename(image)}: Joliet ECORRUPT (parent names itself); " \
         "#{paired} content agrees with Joliet source manifest; auto Rock Ridge checked separately"
  end

  def joliet(binary:, image:, options:)
    if File.basename(image).start_with?("xorriso-")
      return xorriso_joliet(binary: binary, image: image, options: options)
    end

    forced = [*options, "-o", "view=joliet"]
    actual = fsimg_tree(binary: binary, options: forced, profile: "joliet")
    compare_verbose(binary: binary, options: forced, actual: actual)
    unicode = bsdtar_tree(image, view: "joliet")
    # macOS libarchive decomposes BMP names (cafe + U+0301), whereas
    # isoinfo preserves their on-disc spelling. Use bsdtar for non-BMP
    # paths only; isoinfo checks every BMP path without normalization.
    nonbmp = unicode.select { |path, _row| path.codepoints.any? { _1 > 0xffff } }
    actual_nonbmp = actual.select { |path, _row| path.codepoints.any? { _1 > 0xffff } }
    same_paths(expected: nonbmp, actual: actual_nonbmp, oracle: "bsdtar Joliet non-BMP")
    oracle = isoinfo_tree(image, view: "joliet").to_h { |path, row| [display(path), row] }
    projected = unique_pairs(actual.map { |path, row| [isoinfo_joliet_path(path), row] })
    same_paths(expected: oracle, actual: projected, oracle: "isoinfo Joliet (non-BMP projection)")
    manifest = joliet_manifest(image)
    same_paths(expected: manifest, actual: projected, oracle: "Joliet manifest")
    files = {}
    actual.each do |path, entry|
      projected_path = isoinfo_joliet_path(path)
      reference = oracle.fetch(projected_path)
      %i[kind size].each do |field|
        next if field == :size && entry.fetch(:kind) != "file"

        field_equal(path: path, field: "isoinfo Joliet #{field}",
                    expected: reference.fetch(field), actual: entry.fetch(field))
        if nonbmp.key?(path)
          field_equal(path: path, field: "bsdtar Joliet non-BMP #{field}",
                      expected: nonbmp.fetch(path).fetch(field), actual: entry.fetch(field))
        end
      end
      row = manifest.fetch(projected_path)
      field_equal(path: path, field: "Joliet manifest kind", expected: row.fetch("type"), actual: entry.fetch(:kind))
      next unless entry.fetch(:kind) == "file"

      observed = content_digest(binary: binary, options: forced, path: path)
      field_equal(path: path, field: "Joliet manifest SHA-256 and size",
                  expected: [row.fetch("size"), row.fetch("sha256")], actual: observed)
      files[path] = observed
    end
    paired = compare_view_contents(binary: binary, options: options, image: image, joliet_files: files)
    puts "#{File.basename(image)}: Joliet verbose/isoinfo/bsdtar/manifest ok " \
         "(#{actual.size} paths, #{files.size} SHA-256); #{paired}/Joliet content agrees"
  end

  def verify_corpus_image(binary:, image:)
    parent_rule_images = %w[xorriso-joliet.iso xorriso-joliet-long.iso xorriso-joliet-utf16.iso]
    parent_rule = parent_rule_images.include?(File.basename(image))
    failures = %w[auto primary].filter_map do |view|
      selection = view == "auto" ? [] : ["-o", "view=primary"]
      _output, diagnostic, status = Open3.capture3(binary, "W", *selection, "-f", image)
      note = diagnostic.delete_prefix("fsimg: warning: OS sandbox unavailable; continuing without confinement\n")
      if parent_rule
        unless status.exitstatus == 1 && diagnostic.include?("directory's parent record does not name its parent")
          "W #{view}: expected Joliet parent-rule failure; exit=#{status.exitstatus}: #{diagnostic.strip}"
        end
      elsif !status.success?
        "W #{view}: expected success; exit=#{status.exitstatus}: #{diagnostic.strip}"
      elsif File.basename(image).include?("isohybrid") &&
          !note.match?(/\Afsimg: note: the disk also carries a partition table \((gpt|mbr|apm)\); use -F \1 to read its partitions\n\z/)
        "W #{view}: expected one hybrid partition-table note: #{diagnostic.inspect}"
      end
    end
    raise Error, failures.join("; ") unless failures.empty?

    puts "#{File.basename(image)}: W auto/primary #{parent_rule ? 'expected Joliet parent-rule rejection' : 'ok'}"
  end

  def corpus(binary:, directory:, scratch:)
    images = Dir.glob(File.join(directory, "*.iso")).sort
    raise Error, "no producer images" if images.empty?

    failed = false
    images.each_with_index do |image, index|
      label = File.basename(image)
      begin
        verify_corpus_image(binary: binary, image: image)
      rescue Error => error
        warn "#{label}: verification failed: #{error.message}"
        failed = true
      end

      if command("isoinfo", "-d", "-i", image).match?(/^Rock Ridge signatures version 1 found/)
        begin
          agreement = rockridge(binary: binary, image: image, options: ["-f", image])
          failed = true unless agreement
        rescue Error, JSON::ParserError, KeyError, ArgumentError => error
          warn "#{label}: Rock Ridge comparison failed: #{error.message}"
          puts "#{label}: rockridge FAIL"
          failed = true
        end
      end
      if command("isoinfo", "-d", "-i", image).match?(/Joliet with UCS level [123] found/)
        begin
          joliet(binary: binary, image: image, options: ["-f", image])
        rescue Error, JSON::ParserError, KeyError, ArgumentError => error
          warn "#{label}: Joliet comparison failed: #{error.message}"
          puts "#{label}: Joliet FAIL"
          failed = true
        end
      end
      options = ["-o", "view=primary", "-f", image]
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
    # Discard both streams at the process boundary: private member names
    # must never enter an integration diagnostic or a stored transcript.
    %w[auto primary].each do |view|
      selection = view == "auto" ? [] : ["-o", "view=primary"]
      verified = system(binary, "W", *selection, "-f", image, out: File::NULL, err: File::NULL)
      raise Error, "H3 W #{view}: verification failed" unless verified

      puts "H3 W #{view}: successful verifications=1"
    end

    profile = counts([binary, "i", "-f", image],
                     ["awk", '/^Profile:[[:space:]]+joliet$/ { n++ } END { print n+0 }'])
    raise Error, "H3 auto profile must be Joliet" unless profile == [1]

    oracle = counts(["isoinfo", "-J", "-l", "-i", image],
                    ["awk", '/^d/ && $NF != "." && $NF != ".." { d++ } /^-/ { f++ } END { print f+0, d+1 }'])
    %w[auto joliet].each do |view|
      fsimg = counts([binary, "t", "-v", "-o", "view=#{view}", "-f", image],
                     ["awk", '/^d/ { d++; next } /^-/ { f++ } END { print f+0, d+1 }'])
      puts "H3 #{view}: fsimg files=#{fsimg[0]} directories=#{fsimg[1]}; " \
           "isoinfo -J files=#{oracle[0]} directories=#{oracle[1]} (root included)"
      raise Error, "H3 #{view} counts differ" unless fsimg == oracle && oracle == [7763, 2204]
    end
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

if $PROGRAM_NAME == __FILE__
  begin
    exit(IsoIntegration.call(binary: ARGV.fetch(0)) ? 0 : 1)
  rescue IsoIntegration::Error => error
    warn error.message
    exit 1
  end
end
