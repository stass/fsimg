# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "tmpdir"
require "fsimg_spec/iso9660"

class Iso9660OraclesTest < Minitest::Test
  Iso = FsimgSpec::Iso9660
  Execution = Data.define(:launch_error, :ended_by, :leftover, :signal, :exit_status, :stdout, :stderr)
  Shell = Data.define(:tools, :execute) do
    def unavailable_reason = nil
    def run(arguments, **_options) = execute.call(arguments)
  end
  Fields = Data.define(:space_le)
  Volume = Data.define(:fields)
  Model = Data.define(:image_length, :volume)

  def test_tiny_image_short_read_is_an_exact_consumer_exception
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: Short read on old image\n")
    assert_equal "not_applicable", result.state
    result = observe(blocks: 48, status: 255, stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: other read failure\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 1, stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
  end

  def test_geometry_checks_both_declared_values
    result = observe(blocks: 96, stdout: "Logical block size is: 2048\nVolume size is: 96\n")
    assert_equal "agrees", result.state
    result = observe(blocks: 96, stdout: "Logical block size is: 1024\nVolume size is: 96\n")
    assert_equal "disagrees", result.state
  end

  def test_explicit_charset_removes_the_measured_freebsd_locale_notice
    short_read = "isoinfo: Short read on old image\n".b
    notice = "Setting input-charset to 'UTF-8' from locale.\n".b
    execution = Execution.new(launch_error: nil, ended_by: nil, leftover: false, signal: nil,
      exit_status: 255, stdout: "".b, stderr: short_read)
    invoke = lambda do |arguments|
      canonical = ["/configured/isoinfo", "-j", "utf-8", "-d", "-i", "image.img"]
      arguments == canonical ? execution : execution.with(stderr: notice + short_read)
    end
    shell = Shell.new(tools: {isoinfo: "/configured/isoinfo"}, execute: invoke)
    model = Model.new(image_length: 27 * 2048, volume: Volume.new(fields: Fields.new(space_le: 27)))
    result = Iso::Oracles.observe(shell, image: "image.img", chdir: "/fixture", model:).fetch("isoinfo")
    assert_equal "not_applicable", result.state
  end

  def test_an_unexpected_charset_notice_is_not_a_short_read_waiver
    diagnostic = "Setting input-charset to 'UTF-8' from locale.\nisoinfo: Short read on old image\n"
    result = observe(blocks: 27, status: 255, stderr: diagnostic)
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stderr: "isoinfo: Short read on old image\nother failure\n")
    assert_equal "disagrees", result.state
    result = observe(blocks: 27, status: 255, stdout: "partial report\n", stderr: "isoinfo: Short read on old image\n")
    assert_equal "disagrees", result.state
  end

  def test_descriptor_oracles_compare_text_and_dates_instead_of_exit_status
    raw = "\0".b * 2048
    Iso::DescriptorMetadata::TEXTS.each_value { |offset, width| raw[offset, width] = " ".b * width }
    raw[190, 3] = "SET"
    raw[574, 3] = "APP"
    raw[813, 17] = "1970010100000000\0".b
    volume = Iso::Descriptor.new(offset: 16 * 2048, raw:, fields: Fields.new(space_le: 96))
    model = Model.new(image_length: 96 * 2048, volume:)
    xorriso = "Volume Set Id: 'SET'\nApp Id: 'APP'\nCreation Time: 1970010100000000\n"
    isoinfo = "Volume set id: SET\nApplication id: APP\n"

    [[:xorriso, xorriso], [:isoinfo, isoinfo]].each do |tool, report|
      assert_equal("agrees", Iso::Oracles.metadata_report(report, model:, tool:, settings: {application: "APP", created: 0}).state)
      bad = report.sub("APP", "OTHER")
      assert_equal("disagrees", Iso::Oracles.metadata_report(bad, model:, tool:).state)
      assert_equal("disagrees", Iso::Oracles.metadata_report(report, model:, tool:, settings: {application: "WRONG"}).state)
    end
    bad_date = xorriso.sub("1970010100000000", "1970010200000000")
    assert_equal("disagrees", Iso::Oracles.metadata_report(bad_date, model:, tool: :xorriso).state)
    assert_equal("disagrees", Iso::Oracles.metadata_report(xorriso, model:, tool: :xorriso, settings: {created: 1}).state)
  end

  def test_descriptor_recipe_text_ignores_trailing_spaces
    model = descriptor_model(application: "APP ")
    reports = {xorriso: "App Id: 'APP'\n", isoinfo: "Application id: APP\n"}

    reports.each do |tool, report|
      result = Iso::Oracles.metadata_report(report, model:, tool:, settings: {application: "APP "})
      assert_equal("agrees", result.state, "#{tool}: #{result.detail}")
    end
  end

  def test_descriptor_recipe_all_space_text_is_not_reported
    model = descriptor_model(publisher: " ")

    %i[xorriso isoinfo].each do |tool|
      result = Iso::Oracles.metadata_report("", model:, tool:, settings: {publisher: " "})
      assert_equal("agrees", result.state, "#{tool}: #{result.detail}")
    end
  end

  def test_descriptor_recipe_preserves_trailing_control_whitespace
    model = descriptor_model(application: "APP")
    reports = {xorriso: "App Id: 'APP'\n", isoinfo: "Application id: APP\n"}

    reports.each do |tool, report|
      ["\t", "\n", "\v", "\f", "\r"].each do |suffix|
        result = Iso::Oracles.metadata_report(report, model:, tool:, settings: {application: "APP#{suffix} \0"})
        assert_equal("disagrees", result.state, "#{tool}: #{suffix.inspect}")
      end
    end
  end

  def test_descriptor_oracles_explicitly_report_absent_tools
    shell = Shell.new(tools: {}, execute: ->(_arguments) { flunk("an absent oracle must not run") })
    model = Model.new(image_length: 96 * 2048, volume: Volume.new(fields: Fields.new(space_le: 96)))
    observed = Iso::Oracles.observe(shell, image: "image.iso", chdir: "/fixture", model:)
    assert_equal(%w[isoinfo xorriso], observed.keys)
    assert(observed.values.all? { _1.state == "unavailable" })
  end

  def test_descriptor_creation_agrees_with_xorriso
    check_descriptor_creation_oracle(name: "xorriso")
  end

  def test_descriptor_creation_agrees_with_isoinfo
    check_descriptor_creation_oracle(name: "isoinfo")
  end

  private

  def descriptor_model(**texts)
    raw = "\0".b * 2048
    Iso::DescriptorMetadata::TEXTS.each do |name, (offset, width)|
      raw[offset, width] = texts.fetch(name.to_sym, "").ljust(width, " ").b
    end
    volume = Iso::Descriptor.new(offset: 16 * 2048, raw:, fields: Fields.new(space_le: 96))
    Model.new(image_length: 96 * 2048, volume:)
  end

  def check_descriptor_creation_oracle(name:)
    fsimg = ENV.fetch("FSIMG_BIN", "")
    skip("FSIMG_BIN names no fsimg binary") unless File.executable?(fsimg)
    tool = FsimgSpec::Tools.find(ENV.to_h)[name.to_sym]
    skip("configured #{name} is absent") unless tool

    Dir.mktmpdir("iso-descriptor-oracle-") do |directory|
      profile = FsimgSpec.fetch_profile("quick")
      setup = FsimgSpec::CasePhase.prepare(results: directory, profile:, build_directory: directory, environment: ENV.to_h)
      skip(setup.shell.unavailable_reason) if setup.shell.unavailable_reason
      registry = Iso::Rules.registry
      settings = %w[descriptor_text descriptor_dates].map do |suffix|
        registry.fetch("iso9660.create.#{suffix}").domain.call.find { _1.intended.nil? }.source.settings
      end.reduce(&:merge)
      # A 64 KiB generated file keeps the image beyond isoinfo's 48-block
      # descriptor window, without creating a payload file on the host.
      source = FsimgSpec::Source.new(tree: [FsimgSpec::TreeEntry.file("FILE.TXT", size: 65536, content: "zero")], settings:)
      witness = Iso::Witnesses::Creation.new(id: "iso9660.create.metadata.#{name}", source:, intended: nil,
        rules: %w[iso9660.create.descriptor_text iso9660.create.descriptor_dates])
      cases = Iso::Cases.new(registry:, cache: setup.cache, ceilings: profile.ceilings,
        name_trials: setup.name_trials, shell: setup.shell)
      classified = cases.create(witness)
      result = FsimgSpec::CasePhase.result_of(classified, setup:, cases_directory: File.join(directory, "cases"))
      observation = result.observations.fetch(name)
      assert_equal("agrees", observation.state, observation.detail)
      assert_equal("passed", result.verdict, result.detail)
    end
  end

  def observe(blocks:, status: 0, stdout: "", stderr: "")
    execution = Execution.new(launch_error: nil, ended_by: nil, leftover: false, signal: nil,
      exit_status: status, stdout: stdout.b, stderr: stderr.b)
    shell = Shell.new(tools: {isoinfo: "/configured/isoinfo"}, execute: ->(_arguments) { execution })
    model = Model.new(image_length: blocks * 2048, volume: Volume.new(fields: Fields.new(space_le: blocks)))
    Iso::Oracles.observe(shell, image: "image.img", chdir: "/fixture", model:).fetch("isoinfo")
  end
end
