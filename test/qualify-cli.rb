# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Measure the complete CLI process group on a generated disk-backed tree.
require "fileutils"
require "pathname"
require "tmpdir"
require "timeout"

module CliQualification
  class Error < StandardError
  end

  module_function

  def groups(files) = (0...files).each_slice(1000)

  def call(executable:, files:)
    raise CliQualification::Error, "file count must be between 1 and 100000" unless (1..100000).cover?(files)
    root = Pathname.new(Dir.mktmpdir("qualification-cli-", Dir.pwd))
    program = root / "fsimg-rss"
    FileUtils.cp(executable, program, preserve: true)
    source = root / "source"
    scratch = root / "scratch"
    source.mkdir(0o700)
    scratch.mkdir(0o700)
    groups(files).each_with_index do |ordinals, index|
      directory = source / format("d%08d", index)
      directory.mkdir(0o700)
      ordinals.each { File.open(directory / format("f%08d", _1), File::WRONLY | File::CREAT | File::EXCL) { } }
    end

    marker = (0..255).to_a.pack("C*") + "!".b
    (source / "MARKER.BIN").binwrite(marker)
    image = (root / "image.img").to_s
    sandbox = RUBY_PLATFORM.include?("darwin") ? "auto" : "required"
    common = ["-S", sandbox, "-T", scratch.to_s, "-l", "seconds=600"]
    commands = [
      ["c", "--json", "-R", "-F", "exfat", "-f", image, *common, "-C", source.to_s, "."],
      ["i", "--json", "-f", image, *common],
      ["t", "--json", "-f", image, *common],
      ["x", "-O", "-f", image, *common, "/MARKER.BIN"],
      ["W", "--json", "-d", "-f", image, *common],
      ["x", "--json", "-f", image, *common, "-C", root.to_s]
    ]
    success = false
    begin
      commands.each_with_index {
        run(
          program: program,
          command: _1,
          log: root / "#{_2}-#{_1.first}.log"
        )
      }
      verify(destination: root / "image", files: files, marker: marker)
      raise CliQualification::Error, "scratch retains files" unless scratch.children.empty?
      success = true
    ensure
      if success
        FileUtils.remove_entry(root)
      else
        warn("CLI qualification fixtures retained: #{root}")
      end
    end

    puts("CLI disk-backed source files=#{files + 1}; all six operations satisfy RSS allowance")
  end

  def run(program:, command:, log:)
    result = File.open(log, "wb") do |diagnostics|
      pid = Process.spawn(program.to_s, "16777216", *command, out: File::NULL, err: diagnostics)
      begin
        status = Timeout.timeout(900) { Process.wait2(pid).last }
      ensure
        unless status
          Process.kill("KILL", pid)
          Process.wait(pid)
        end
      end

      status
    end

    measurements = File.readlines(log).select { _1.start_with?("CLI RSS ") }
    measurements.each { puts(_1.strip) }
    raise CliQualification::Error, "operation #{command.first} failed" unless result.success?
    raise CliQualification::Error, "expected one RSS measurement" unless measurements.size == 1
  end

  def verify(destination:, files:, marker:)
    raise CliQualification::Error, "marker bytes differ" unless (destination / "MARKER.BIN").binread == marker
    groups(files).each_with_index do |ordinals, index|
      directory = destination / format("d%08d", index)
      ordinals.each do |ordinal|
        path = directory / format("f%08d", ordinal)
        raise CliQualification::Error, "missing or nonempty file #{ordinal}" unless path.file? && path.size.zero?
      end
    end
  end
end

CliQualification.call(executable: File.realpath(ARGV.fetch(0)), files: Integer(ARGV.fetch(1, "100000"), 10))
