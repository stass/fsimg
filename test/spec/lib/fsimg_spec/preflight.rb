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
  # creation and every reading stage on the created image through fsimg
  # with the run's options. It answers the Runner every case uses: one
  # bound to the sandbox status the preflight saw, the coordinator's text
  # ceiling and whether RLIMIT_AS applies.
  class Preflight
    # The case directory roots the runner walks by path, beyond the results
    # directory, with the longest id a recipe may carry.
    LONGEST_CASE_ROOT = "/cases/#{"x" * 128}/source".length
    # A population tree, test/spec-cache/<key>.src/, beyond the build
    # directory; a key is a SHA-256 in hex.
    LONGEST_CACHE_ROOT = "/test/spec-cache/#{"0" * 64}.src".length
    # Before the first summary reports the coordinator's text ceiling, a
    # capture may hold this much.
    INITIAL_TEXT_BYTES = 64 << 20
    SENTINEL = "fsimg-spec-sentinel"

    def initialize(results:, build_directory:, profile:, fsimg:, environment:)
      @results = results
      @build_directory = build_directory
      @profile = profile
      @fsimg = fsimg
      @environment = environment
    end

    # Runs every check and answers the run's Runner and the sandbox status
    # every invocation must report.
    def call
      check_path_max
      check_descriptors
      check_devices
      directory = File.join(@results, "preflight")
      Dir.mkdir(directory, 0o700)
      begin
        check_confinement(directory)
        run_valid_case(File.join(directory, "case"), address_space: address_space?)
      ensure
        FileUtils.rm_r(directory, secure: true)
      end
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

      longest = [@results.bytesize + LONGEST_CASE_ROOT, @build_directory.bytesize + LONGEST_CACHE_ROOT].max
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

    # A one-file FAT16 creation, then every reading stage on its image.
    # Every stage must accept, and every record must repeat the sandbox
    # status the first one reported.
    def run_valid_case(directory, address_space:)
      recipe = Recipe.new(
        id: "preflight", format: "fat16", kind: "valid", stages: RUN_ORDER,
        source: Source.new(tree: [TreeEntry.new(path: "file.txt", type: "file", size: 4096, seed: 1)],
                           settings: { size: 16 << 20 }),
        expected: RUN_ORDER.to_h { [_1, Expectation.parse("accept")] }
      )
      runner = runner_for(text_bytes: INITIAL_TEXT_BYTES, address_space:)
      sandbox = nil
      refusal = CaseDirectory.prepare(directory, recipe:, image: nil)
      begin
        raise PreflightError, "the host cannot hold the preflight's source tree: #{refusal}" if refusal

        recipe.stages.each do |stage|
          result = runner.run(stage, recipe:, case_directory: directory, sandbox:)
          raise PreflightError, "preflight #{stage}: #{result.outcome} #{result.outcome.detail} #{result.problems.join("; ")}".strip unless
            result.outcome.kind == "accepted" && result.safe?

          sandbox ||= reported_sandbox(result)
          runner = runner_for(text_bytes: text_limit(result), address_space:) if stage == "create"
        end
      ensure
        FileUtils.rm_r(directory, secure: true)
      end
      [runner, sandbox]
    end

    # The sandbox status an accepted stage's summary reports; every later
    # record of the run must repeat it.
    def reported_sandbox(result)
      status = result.outcome.records.last["sandbox"]
      raise PreflightError, "preflight #{result.stage}: the summary reports no sandbox status" unless status.is_a?(String)

      status
    end

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
