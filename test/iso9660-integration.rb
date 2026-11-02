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
    output, error, status = Open3.capture3(*arguments)
    raise Error, "command failed (#{status.exitstatus}): #{error.lines.first&.strip}" unless status.success?

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
      raise Error, "unexpected extracted kind" unless stat.file? || stat.directory?

      [path.delete_prefix(root), { kind: stat.directory? ? "directory" : "file",
                                   size: stat.file? ? stat.size : 0,
                                   sha256: stat.file? ? Digest::SHA256.file(path).hexdigest : nil }]
    end
  end

  def compare(expected:, actual:, content:)
    raise Error, "path set differs (expected #{expected.size}, observed #{actual.size})" unless expected.keys.sort == actual.keys.sort

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

  def corpus(binary:, directory:, scratch:)
    images = Dir.glob(File.join(directory, "*.iso")).sort
    raise Error, "no producer images" if images.empty?

    failed = false
    images.each_with_index do |image, index|
      label = File.basename(image)
      forcing = label.include?("isohybrid") ? ["-F", "iso9660/-"] : []
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
    info = counts([binary, "i", "-o", "view=primary", "-f", image],
                  ["awk", '/^Label:/ { next } /^Format:[[:space:]]+ISO9660$/ { n++ } END { print n+0 }'])
    raise Error, "H3 format differs" unless info == [1]

    fsimg = counts([binary, "t", "-v", "-o", "view=primary", "-f", image],
                   ["awk", '/^d/ { d++; next } /^-/ { f++ } END { print f+0, d+1 }'])
    # Primary files have a numeric version; directory identifiers do not.
    # This oracle classification includes empty directories and the root.
    oracle = counts(["isoinfo", "-f", "-i", image],
                    ["awk", '/;[0-9]+$/ { f++; next } /^\// { d++ } END { print f+0, d+1 }'])
    puts "H3: fsimg files=#{fsimg[0]} directories=#{fsimg[1]}; isoinfo files=#{oracle[0]} directories=#{oracle[1]} (root included)"
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
