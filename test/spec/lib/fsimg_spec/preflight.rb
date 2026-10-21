# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # The host cannot run the profile's cases; the run ends as a harness
  # error.
  class PreflightError < Error; end

  # Establishes the host before any case (specs/design-spec-testing.md#runner)
  # in the results directory's preflight/, which it removes when it
  # finishes. It checks the host's path and descriptor limits, the trials
  # and cases filesystems and any oracle confinement, then runs one valid
  # creation, and every reading stage on a sample image no fsimg writer
  # produced, through fsimg with the run's options. It answers the Runner
  # every case uses: one bound to the sandbox status the preflight saw,
  # the coordinator's text ceiling and whether RLIMIT_AS applies.
  class Preflight
    # The case directory roots the runner walks by path, beyond the results
    # directory, with the longest id a recipe may carry.
    LONGEST_CASE_ROOT = "/cases/#{"x" * 128}/source".length
    # Before the first summary reports the coordinator's text ceiling, a
    # capture may hold this much.
    INITIAL_TEXT_BYTES = 64 << 20
    SENTINEL = "fsimg-spec-sentinel"
    # The confinement statuses an accepted stage can report: fsimg's
    # summaries also name unknown (cmd/fsimg/display.c), but only before a
    # worker has reported its confinement, and a worker makes every
    # acceptance.
    SANDBOX_STATUSES = %w[active unavailable disabled].freeze
    # The creation the preflight runs: one file on FAT16.
    CREATION = Recipe.new(
      id: "preflight", format: "fat16", kind: "valid", stages: ["create"],
      source: Source.new(tree: [TreeEntry.new(path: "file.txt", type: "file", size: 4096, seed: 1)],
                         settings: { size: 16 << 20 }),
      expected: { "create" => Expectation.parse("accept") }
    )

    # sample renders the reading sample into a directory and answers its
    # valid reading recipe and image (Fat::Bases.preflight_sample).
    def initialize(results:, profile:, fsimg:, environment:, sample:)
      @results = results
      @profile = profile
      @fsimg = fsimg
      @environment = environment
      @sample = sample
    end

    # Runs every check and answers the run's Runner and the sandbox status
    # every invocation must report.
    def call
      check_path_max
      check_descriptors
      check_devices
      directory = File.join(@results, "preflight")
      Dir.mkdir(directory, 0o700)
      checked = begin
        check_confinement(directory)
        run_valid_cases(directory, address_space: address_space?)
      rescue Exception
        # A cleanup that raised here would replace the exception in flight,
        # such as a ContainmentError or a signal; rm_rf raises no
        # StandardError.
        FileUtils.rm_rf(directory, secure: true)
        raise
      end
      # After success, a directory that cannot be removed fails the preflight.
      FileUtils.rm_r(directory, secure: true)
      checked
    end

    # status, which reaches the report, when an accepted stage may report
    # it; otherwise the preflight fails.
    def self.accepted_sandbox(status, stage:)
      return status if SANDBOX_STATUSES.include?(status)

      raise PreflightError, "preflight #{stage}: the summary reports the sandbox status #{FsimgSpec.shown(status)}, " \
                            "not one of #{SANDBOX_STATUSES.join(", ")}"
    end

    private

    # The coordinator's text ceiling that create's summary reports, a
    # positive integer that fsimg writes as a decimal JSON string when it
    # may exceed what JSON numbers hold exactly.
    def text_limit(result)
      reported = result.outcome.records.last&.fetch("text_limit_bytes", nil)
      bytes =
        case reported
        when Integer then reported
        when /\A[0-9]{1,20}\z/ then Integer(reported, 10)
        end
      raise PreflightError, "create's summary reports no positive text_limit_bytes" unless bytes&.positive?

      bytes
    end

    # A walked path is a tree root, a slash and a relative path up to the
    # path ceiling, and PATH_MAX counts its terminating NUL too.
    def check_path_max
      answer = begin
        IO.popen(["getconf", "PATH_MAX", @results], &:read)
      rescue SystemCallError => e
        raise PreflightError, "getconf cannot run: #{e.message}"
      end
      host = Integer(answer, exception: false)
      raise PreflightError, "getconf reports no PATH_MAX for #{@results}" unless host

      longest = @results.bytesize + LONGEST_CASE_ROOT
      return if longest + 1 + @profile.ceilings.path + 1 <= host

      raise PreflightError, "a #{longest}-byte tree root plus the #{@profile.ceilings.path}-byte path ceiling " \
                            "reaches the host's PATH_MAX of #{host}"
    end

    def check_descriptors
      soft, = Process.getrlimit(:NOFILE)
      return if soft >= @profile.ceilings.fds

      raise PreflightError, "the soft RLIMIT_NOFILE #{soft} is below the fds limit #{@profile.ceilings.fds}"
    end

    def check_devices
      devices = %w[trials cases].map { File.stat(File.join(@results, _1)).dev }.uniq
      raise PreflightError, "trials/ and cases/ lie on different filesystems" unless devices.one?
    end

    # FSIMG_SPEC_ORACLE_CONFINE names a command that runs an oracle inside
    # an OS sandbox. A probe run through it must write inside its directory
    # and exit 0, the positive control that shows the command runs programs
    # at all, and must neither write outside the directory nor read a
    # sentinel placed outside it.
    PROBE_SCRIPT = 'echo x > inside || exit 3; echo x > "$1" 2>/dev/null; cat "$2" 2>/dev/null; exit 0'

    def check_confinement(directory)
      confine = @environment["FSIMG_SPEC_ORACLE_CONFINE"].to_s.split
      return if confine.empty?

      sentinel = File.join(@results, "#{SENTINEL}.txt")
      outside = File.join(@results, "#{SENTINEL}.written")
      File.write(sentinel, SENTINEL)
      begin
        probe = Invocation.run([*confine, "/bin/sh", "-c", PROBE_SCRIPT, "probe", outside, sentinel],
                               chdir: directory, env: { "PATH" => @environment.fetch("PATH") },
                               limits: probe_limits)
        problem = confinement_problem(probe, directory:, outside:)
        raise PreflightError, "#{confine.join(" ")} #{problem}" if problem
      ensure
        FileUtils.rm_f([sentinel, outside])
      end
    end

    def confinement_problem(probe, directory:, outside:)
      if probe.launch_error then "cannot start: #{probe.launch_error}"
      elsif probe.exit_status != 0 || probe.leftover || !File.file?(File.join(directory, "inside"))
        "did not run the probe to completion inside its directory"
      elsif File.exist?(outside) || probe.stdout.include?(SENTINEL)
        "lets an oracle write or read outside its case directory"
      end
    end

    def probe_limits = Invocation::Limits.new(wall_seconds: 30, cpu_seconds: 30, file_bytes: 1 << 20,
                                              address_space_bytes: nil, capture_bytes: 1 << 20)

    # A one-file FAT16 creation, whose summary reports the coordinator's
    # text ceiling and the sandbox status, then every reading stage on the
    # sample, so the reading stages do not depend on the writer under
    # test. Every stage must accept, and every record must repeat the
    # sandbox status create reported. Answers the run's Runner and that
    # status.
    def run_valid_cases(directory, address_space:)
      runner = runner_for(text_bytes: INITIAL_TEXT_BYTES, address_space:)
      create, = run_case(File.join(directory, "create"), CREATION, image: nil, runner:, sandbox: nil)
      sandbox = reported_sandbox(create)
      runner = runner_for(text_bytes: text_limit(create), address_space:)
      recipe, image = @sample.(directory)
      run_case(File.join(directory, "read"), recipe, image:, runner:, sandbox:)
      [runner, sandbox]
    end

    # Runs every stage of recipe in a new case directory and answers the
    # results; a stage that does not accept safely fails the preflight.
    def run_case(directory, recipe, image:, runner:, sandbox:)
      refusal = CaseDirectory.prepare(directory, recipe:, image:)
      raise PreflightError, "the host cannot hold the preflight's source tree: #{refusal}" if refusal

      recipe.stages.map do |stage|
        result = runner.run(stage, recipe:, case_directory: directory, sandbox:)
        unless result.outcome.kind == "accepted" && result.safe?
          detail = [result.outcome, result.outcome.detail, *result.problems].compact.join(" ")
          raise PreflightError, "preflight #{recipe.id} #{stage}: #{detail}"
        end
        result
      end
    end

    # The sandbox status an accepted stage's summary reports; every later
    # record of the run must repeat it. It reaches the report, so only the
    # statuses fsimg defines are taken.
    def reported_sandbox(result) = Preflight.accepted_sandbox(result.outcome.records.last["sandbox"], stage: result.stage)


    def runner_for(text_bytes:, address_space:) =
      Runner.new(fsimg: @fsimg, profile: @profile, environment: @environment,
                 text_bytes:, address_space:)

    # RLIMIT_AS applies where the host enforces it and fsimg starts under
    # it; a sanitizer build's address reservation keeps it from starting.
    def address_space?
      return false unless RUBY_PLATFORM.match?(/linux|freebsd/)

      limits = Invocation::Limits.new(wall_seconds: 30, cpu_seconds: 30, file_bytes: 1 << 20,
                                      address_space_bytes: Runner::ADDRESS_SPACE_BYTES, capture_bytes: 1 << 20)
      Invocation.run([@fsimg, "-V"], chdir: @results, env: { "PATH" => @environment.fetch("PATH") },
                                     limits:).exit_status == 0
    end
  end
end
