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

  # A population tool that ran to its end and refused the tree, such as
  # mcopy refusing a DOS device name or a parent directory mtools wrote
  # under another name.
  class PopulationError < FixtureError; end

  # The development tools the harness runs, as the wrapper's environment
  # names them. A tool that is not configured as an absolute path to an
  # executable file is nil: unavailable for its own evidence only. Tools
  # run from other directories, so a relative path would name another
  # file.
  module Tools
    VARIABLES = {
      mkfs_fat: "MKFS_FAT", fsck_fat: "FSCK_FAT", newfs_msdos: "NEWFS_MSDOS", fsck_msdosfs: "FSCK_MSDOSFS",
      mcopy: "MCOPY", mmd: "MMD", mdir: "MDIR", mtype: "MTYPE", fatcat: "FATCAT"
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

  # One oracle's view of one image (specs/design-spec-testing.md#independence-and-oracles):
  # the tool agrees with the tree the harness knows the image to hold,
  # disagrees with it, giving a short detail, does not apply to the
  # image's geometry or names, or is unavailable on this host. A
  # disagreement that a documented oracle exception matches is excused:
  # it keeps its detail and carries the exception's id, so it is recorded
  # as the tool's defect rather than dropped. A detail quotes the image's names
  # and the tool's output only through FsimgSpec.shown, and is cut to
  # DETAIL_LENGTH, so a hostile image cannot flood a report.
  class Observation < Data.define(:state, :detail, :exception_id)
    include Checked

    STATES = %w[agrees disagrees excused not_applicable unavailable].freeze
    DETAIL_LENGTH = 240

    def initialize(state:, detail: nil, exception_id: nil)
      raise HarnessError, "unknown observation #{FsimgSpec.shown(state)}" unless STATES.include?(state)
      raise HarnessError, "an observation names an exception exactly when it is excused" unless
        (state == "excused") == !exception_id.nil?

      super(state:, detail: detail && (detail.length > DETAIL_LENGTH ? "#{detail[0, DETAIL_LENGTH]}..." : detail), exception_id:)
    end

    def self.agrees = new(state: "agrees")

    def self.disagrees(detail) = new(state: "disagrees", detail:)

    def self.not_applicable(detail) = new(state: "not_applicable", detail:)

    def self.unavailable(detail) = new(state: "unavailable", detail:)

    # This disagreement excused by the oracle exception whose id is
    # exception_id.
    def excused_by(exception_id)
      raise HarnessError, "only a disagreement is excused, not #{state}" unless state == "disagrees"

      Observation.new(state: "excused", detail:, exception_id:)
    end

    # The label the coverage report counts this observation under: its
    # state, and for an excused one the exception that excused it, so a
    # report shows which exceptions fired.
    def tally_label = exception_id ? "excused:#{exception_id}" : state

    # The observation as reports and failure records write it.
    def to_h = super.compact

    # How many observations of each tool have each tally_label, over a
    # list of observation maps (tool => Observation), as the coverage
    # report counts them: tools and labels in name order, labels never
    # seen left out.
    def self.tally(observations) =
      observations.flat_map(&:to_a).group_by(&:first).sort.to_h do |tool, pairs|
        [tool, pairs.map { |_tool, observation| observation.tally_label }.tally.sort.to_h]
      end
  end

  # Runs an independent tool under the runner's process lifetime, with the
  # oracles' environment (specs/design-spec-testing.md#runner): the
  # harness's PATH, TZ=UTC, a UTF-8 locale so mtools keeps Unicode long
  # names, SOURCE_DATE_EPOCH so mtools stamps the same times on every run,
  # and HOME in oracle-home/. mtools reads both HOME/.mtoolsrc and the file
  # MTOOLSRC names, so MTOOLSRC names that one file, and oracle-home/ must
  # hold it alone, an empty regular file, before every run: nothing a case
  # writes, in its directory or beside the shell's file, configures a
  # tool. The shell carries the run's tools, each nil where the host lacks
  # it.
  class OracleShell
    LIMITS = Invocation::Limits.new(wall_seconds: 120, cpu_seconds: 120, file_bytes: 64 << 30,
                                    address_space_bytes: nil, capture_bytes: 16 << 20)
    # The time the base trees carry, and the one mtools stamps on the
    # entries it creates.
    EPOCH = Time.utc(2000, 1, 1)
    MTOOLSRC = ".mtoolsrc"

    attr_reader :tools

    def initialize(home:, environment:, tools: Tools.find(environment))
      @home = home
      @environment = environment
      @tools = tools
      FileUtils.mkdir_p(home, mode: 0o700)
      create_mtoolsrc
    end

    # Runs argv once oracle-home/ still holds only the empty mtoolsrc it
    # was created with, so no earlier run can configure a later one. input
    # is nil, for an empty stdin, or an Enumerator of the chunks to write to
    # it; capture_bytes caps each of stdout and stderr.
    def run(argv, chdir:, input: nil, capture_bytes: LIMITS.capture_bytes)
      check_home
      Invocation.run(argv, chdir:, env: environment, limits: LIMITS.with(capture_bytes:), input:)
    end

    def environment
      { "PATH" => @environment.fetch("PATH"), "TZ" => "UTC", "LC_ALL" => locale,
        "SOURCE_DATE_EPOCH" => EPOCH.to_i.to_s, "HOME" => @home, "MTOOLSRC" => mtoolsrc }
    end

    # Why this host cannot run the oracles, or nil: it lists no UTF-8
    # locale for them. The locale is looked up on first use, so such a
    # host still runs every fsimg case, its observations unavailable.
    def unavailable_reason = locale_lookup.last

    # The oracles' locale (OracleShell.utf8_locale).
    def locale = locale_lookup.first || raise(FixtureError, locale_lookup.last)

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

    def mtoolsrc = File.join(@home, MTOOLSRC)

    # The locale and nil, or nil and why the host has none, looked up once
    # whichever it answers.
    def locale_lookup
      @locale_lookup ||= begin
        [OracleShell.utf8_locale, nil]
      rescue FixtureError => e
        [nil, e.message]
      end
    end

    # Creates the empty mtoolsrc without following a link, or checks that
    # an earlier shell left oracle-home/ as it must be.
    def create_mtoolsrc
      File.open(mtoolsrc, File::WRONLY | File::CREAT | File::EXCL | File::NOFOLLOW, 0o600, &:close)
    rescue Errno::EEXIST
      check_home
    end

    def check_home
      raise FixtureError, "#{@home} is not a directory" unless File.lstat(@home).directory?

      names = NameFunnel.children(@home)
      raise FixtureError, "#{@home} holds #{names.map { FsimgSpec.shown(_1) }.join(", ")}, not #{MTOOLSRC} alone" unless
        names == [MTOOLSRC.b]

      stat = File.lstat(mtoolsrc)
      raise FixtureError, "#{mtoolsrc} is not an empty regular file" unless stat.file? && stat.size.zero?
    end
  end

  # A materialized base: the cached image, its manifest, the oracles'
  # observations of it (tool => Observation), and the problems that keep
  # it from serving any case.
  Fixture = Data.define(:image, :manifest, :observations, :problems) do
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
    # Tools run from other directories, so the cache names its entries by
    # absolute paths.
    def initialize(directory)
      @directory = File.expand_path(directory)
      FileUtils.mkdir_p(directory, mode: 0o700)
    end

    def key(base, tool_versions) =
      Digest::SHA256.hexdigest(CanonicalJson.generate(
        { builder: base.builder, population: base.population, parameters: base.parameters, tree: base.tree.map(&:to_h),
          tools: tool_versions.sort.to_h }.compact
      ))

    def image_path(key) = File.join(@directory, "#{key}.img")

    def manifest_path(key) = File.join(@directory, "#{key}.json")

    # The published image for key, building it with the block into a
    # temporary path of its own first when no run has published one. The
    # block writes that one file, so the cache holds nothing but images
    # and their manifests.
    def fetch(key)
      final = image_path(key)
      return final if published?(final)

      temporary = File.join(@directory, "#{key}.#{SecureRandom.hex(8)}.tmp")
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
