# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "shellwords"

module HfsplusVerifyManifest
  Row = Data.define(:source, :identifier, :arguments, :status, :message, :counts)
  PATH = File.expand_path("../hfsplus-verify-expected.tsv", __dir__).freeze
  VARIABLES = {
    "native" => "FSIMG_HFSPLUS_NATIVE",
    "corpus" => "FSIMG_HFSPLUS_CORPUS",
    "corpus-partition" => "FSIMG_HFSPLUS_CORPUS",
    "decoded" => "FSIMG_HFSPLUS_DECODED"
  }.freeze
  COUNTS = %w[files directories allocated_clusters free_clusters bad_clusters].freeze
  STATUSES = ["ok", "unknown format", "unsupported capability", "corrupt image"].freeze

  module_function

  def rows
    File.readlines(PATH, chomp: true).reject { _1.start_with?("#") }.map do |line|
      source, identifier, arguments, status, message, *counts = line.split("\t", -1)
      valid = VARIABLES.key?(source) && STATUSES.include?(status) &&
        identifier && !identifier.empty? &&
        identifier.split("/").none? { _1.empty? || %w[. ..].include?(_1) || _1.start_with?("~") } &&
        !identifier.start_with?("/") && !/(?:\A|\s)(?:\/|~)/.match?(line) && !line.include?("/Users/") && !line.include?("/home/") &&
        (status == "ok" ? message == "" && counts.length == 5 && counts.all? { /\A\d+\z/.match?(_1) } :
          message && !message.empty? && counts.empty?)
      raise CliTest::Error, "malformed verification manifest row: #{line}" unless valid

      tokens = Shellwords.split(arguments)
      unless /\A(?:|(?:--compress=gzip )?-F hfsplus\/-\/dmg --offset=\d+ --length=\d+)\z/.match?(arguments)
        raise CliTest::Error, "unexpected manifest arguments: #{arguments}"
      end
      Row.new(source: source, identifier: identifier, arguments: tokens.freeze,
        status: status, message: message, counts: counts.freeze)
    end.freeze
  end
end

class HfsplusVerifyManifestTest < Minitest::Test
  include CliProcesses
  include CliRecords

  ROWS = HfsplusVerifyManifest.rows

  def setup
    @executable = Pathname.new(ENV.fetch("FSIMG_CLI_EXECUTABLE")).realpath
  end

  def test_manifest_is_well_formed_and_contains_named_refusals
    refute_empty(ROWS)
    identities = ROWS.map { [_1.source, _1.identifier] }
    assert_equal(identities.uniq, identities, "manifest identifiers must be unique per source")
    {
      ["native", "h-hfsplus-33554432-e8-interrupted"] => "unsupported capability",
      ["native", "h-hfsplus-16777216-e4-dirlinks-J"] => "unsupported capability",
      ["decoded", "mozilla-1.0-smi"] => "corrupt image"
    }.each do |identity, status|
      row = ROWS.find { [_1.source, _1.identifier] == identity }
      refute_nil(row, "missing manifest refusal #{identity.join(':')}")
      assert_equal(status, row.status)
      assert_includes(row.message, "alternate") if identity.first == "decoded"
    end
    # Generated members remain visible to Minitest and ordinary reflection.
    assert_respond_to(self, "test_manifest_native_h_hfsplus_1048576_newfs_b4096")
    assert_includes(methods, :test_manifest_native_h_hfsplus_1048576_newfs_b4096)
  end

  private

  def specimen(row)
    variable = HfsplusVerifyManifest::VARIABLES.fetch(row.source)
    directory = ENV[variable]
    skip("#{variable} unset: #{row.source}:#{row.identifier}") unless directory

    base = File.join(directory, row.identifier)
    candidates = case row.source
    when "native" then %w[image.img image.img.gz].map { File.join(base, _1) }
    when "decoded" then ["#{base}.img"]
    else [base]
    end
    path = candidates.find { File.file?(_1) }
    skip("#{variable}: specimen absent: #{candidates.join(' or ')}") unless path

    File.realpath(path)
  end

  def verify_row(row)
    path = specimen(row)
    [[], ["-d"]].each do |scope|
      result = invoke(root: Dir.pwd, arguments: ["W", "--json", *row.arguments, "-f", path, *scope],
        expected: nil, timeout: 600)
      context = "#{row.source}:#{row.identifier} #{scope.empty? ? 'structural' : 'payload'}"
      if row.status == "ok"
        assert_equal(0, result.status, "#{context}: #{result.stdout} #{result.stderr}")
        report = completed(result).find { _1.fetch("type") == "verify" }
        refute_nil(report, context)
        assert_equal(true, report.fetch("complete"), context)
        assert_equal(scope.empty? ? "structural" : "initialized-payload", report.fetch("validation_scope"), context)
        HfsplusVerifyManifest::COUNTS.zip(row.counts).each do |field, count|
          assert_equal(count, report.fetch(field), "#{context}: #{field}")
        end
      else
        refute_equal(0, result.status, context)
        decoded = records(result)
        error = decoded.find { _1.fetch("type") == "error" }
        refute_nil(error, context)
        assert_equal(row.status, error.fetch("status"), context)
        assert_includes(error.fetch("message"), row.message, context)
        refute(decoded.any? { _1.fetch("type") == "verify" && _1.fetch("complete") }, context)
      end
    end
  end

  public

  # Generates one test per TSV row, named by source and identifier. Minitest
  # discovers each member through methods and respond_to?.
  ROWS.each do |row|
    define_method("test_manifest_#{row.source}_#{row.identifier}".gsub(/[^a-zA-Z0-9_]/, "_")) do
      verify_row(row)
    end
  end
end
