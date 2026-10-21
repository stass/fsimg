# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"
require "fileutils"
require "securerandom"

module FsimgSpec
  # A base the harness cannot materialize, or one that came out other than
  # its recipe says.
  class FixtureError < Error; end

  # The development tools the harness runs, as the wrapper's environment
  # names them. A tool that is not configured as an absolute path to an
  # executable file is nil: unavailable for its own evidence only. Tools
  # run from other directories, so a relative path would name another
  # file.
  module Tools
    VARIABLES = {
      mkfs_fat: "MKFS_FAT", fsck_fat: "FSCK_FAT", newfs_msdos: "NEWFS_MSDOS", fsck_msdosfs: "FSCK_MSDOSFS",
      mcopy: "MCOPY", mmd: "MMD", mdir: "MDIR", fatcat: "FATCAT"
    }.freeze

    def self.find(environment) =
      VARIABLES.to_h { |name, variable| [name, environment[variable].to_s.then { executable_file?(_1) ? _1 : nil }] }

    private_class_method def self.executable_file?(path) =
      File.absolute_path?(path) && File.file?(path) && File.executable?(path)

    # A tool's version for cache keys: a digest of its executable, since
    # not every tool reports a version of its own.
    def self.version(path) = Digest::SHA256.file(path).hexdigest

    # The version of harness code that shapes a base: a digest of its
    # source files.
    def self.source_version(*files) = Digest::SHA256.hexdigest(files.map { File.binread(_1) }.join)
  end

  # Runs an independent tool under the runner's process lifetime, with the
  # oracles' environment (specs/design-spec-testing.md#runner): the
  # harness's PATH, TZ=UTC, a UTF-8 locale so mtools keeps Unicode long
  # names, SOURCE_DATE_EPOCH so mtools stamps the same times on every run,
  # and HOME and MTOOLSRC in oracle-home/, which holds only an empty
  # mtoolsrc, so nothing a case writes can configure a tool.
  class OracleShell
    LIMITS = Invocation::Limits.new(wall_seconds: 120, cpu_seconds: 120, file_bytes: 64 << 30,
                                    address_space_bytes: nil, capture_bytes: 16 << 20)
    # The time the base trees carry, and the one mtools stamps on the
    # entries it creates.
    EPOCH = Time.utc(2000, 1, 1)

    def initialize(home:, environment:)
      @home = home
      @environment = environment
      @locale = OracleShell.utf8_locale
      FileUtils.mkdir_p(home, mode: 0o700)
      create_mtoolsrc
    end

    # Runs argv once the mtoolsrc is still the empty regular file it was
    # created as, so no earlier oracle run can configure a later one.
    def run(argv, chdir:)
      check_mtoolsrc
      Invocation.run(argv, chdir:, env: environment, limits: LIMITS)
    end

    def environment
      { "PATH" => @environment.fetch("PATH"), "TZ" => "UTC", "LC_ALL" => @locale,
        "SOURCE_DATE_EPOCH" => EPOCH.to_i.to_s, "HOME" => @home, "MTOOLSRC" => mtoolsrc }
    end

    # C.UTF-8 where the host has it, else en_US.UTF-8, under the name the
    # host lists: glibc spells them C.utf8 and en_US.utf8. A host with
    # neither cannot run the oracles, as mtools would silently lose
    # Unicode long names.
    def self.utf8_locale
      listed = begin
        IO.popen(%w[locale -a], &:read).lines(chomp: true)
      rescue Errno::ENOENT
        raise FixtureError, "the host has no locale command to list its locales"
      end
      locale = %w[C en_US].lazy.filter_map { |language| listed.find { _1.match?(/\A#{language}\.utf-?8\z/i) } }.first
      raise FixtureError, "the host has no C or en_US UTF-8 locale for the oracles" unless locale

      locale
    end

    private

    def mtoolsrc = File.join(@home, "mtoolsrc")

    # Creates the empty mtoolsrc without following a link, or checks that
    # an earlier shell left it an empty regular file.
    def create_mtoolsrc
      File.open(mtoolsrc, File::WRONLY | File::CREAT | File::EXCL | File::NOFOLLOW, 0o600, &:close)
    rescue Errno::EEXIST
      check_mtoolsrc
    end

    def check_mtoolsrc
      stat = File.lstat(mtoolsrc)
      raise FixtureError, "#{mtoolsrc} is not an empty regular file" unless stat.file? && stat.size.zero?
    end
  end

  # A materialized base: the cached image, its manifest, and the problems
  # that keep it from serving any case.
  Fixture = Data.define(:image, :manifest, :problems) do
    def usable? = problems.empty?
  end

  # The fixture cache in the build directory (specs/design-spec-testing.md#fixtures).
  # An entry is keyed by a digest of the canonical base recipe and the
  # versions of the tools and harness code that build it, written under a
  # temporary name of its own and published with a no-replace link, so a
  # concurrent run never reads a partial image or replaces one in use. A
  # run killed while building leaves its temporary files behind; make
  # clean removes the cache.
  class FixtureCache
    # A build's temporary image is <key>.<hex>.tmp, and the names that
    # extend it are that build's alone.
    TEMPORARY_HEX_BYTES = 8
    # The longest name a build uses in the cache: its population
    # directory.
    LONGEST_NAME = "#{"0" * 64}.#{"0" * (2 * TEMPORARY_HEX_BYTES)}.tmp.src".length

    # The directory a build whose temporary image is temporary populates
    # its tree in.
    def self.population_directory(temporary) = "#{temporary}.src"

    # Tools run from other directories, so the cache names its entries by
    # absolute paths.
    def initialize(directory)
      @directory = File.expand_path(directory)
      FileUtils.mkdir_p(directory, mode: 0o700)
    end

    def key(base, tool_versions) =
      Digest::SHA256.hexdigest(CanonicalJson.generate(
        { builder: base.builder, parameters: base.parameters, tree: base.tree.map(&:to_h), tools: tool_versions.sort.to_h }
      ))

    def image_path(key) = File.join(@directory, "#{key}.img")

    def manifest_path(key) = File.join(@directory, "#{key}.json")

    # The published image for key, building it with the block into a
    # temporary path first when no run has published one. The block may
    # use names that extend the temporary path, which no other build
    # shares, and must remove them.
    def fetch(key)
      final = image_path(key)
      return final if published?(final)

      temporary = File.join(@directory, "#{key}.#{SecureRandom.hex(TEMPORARY_HEX_BYTES)}.tmp")
      begin
        yield temporary
        link_unless_published(temporary, final)
      ensure
        FileUtils.rm_f(temporary)
      end
      raise FixtureError, "#{final} is not a regular file" unless published?(final)

      final
    end

    # Removes every entry, for a cache whose owner alone builds into it,
    # one build at a time, such as a run's drawn bases.
    def clear = Dir.children(@directory).each { FileUtils.rm_rf(File.join(@directory, _1), secure: true) }

    def publish_manifest(key, manifest)
      temporary = "#{manifest_path(key)}.#{SecureRandom.hex(8)}.tmp"
      File.write(temporary, "#{CanonicalJson.generate(manifest)}\n")
      File.rename(temporary, manifest_path(key))
    end

    private

    # Publishes temporary as final unless a concurrent run published it
    # first.
    def link_unless_published(temporary, final)
      File.link(temporary, final)
    rescue Errno::EEXIST
      nil
    end

    # Whether path is a published image; anything else at its name is an
    # error, never an image.
    def published?(path)
      stat = File.lstat(path)
      raise FixtureError, "#{path} is not a regular file" unless stat.file?

      true
    rescue Errno::ENOENT
      false
    end
  end
end
