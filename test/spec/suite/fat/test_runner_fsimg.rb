# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "fileutils"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The runner drives the real fsimg: the preflight runs a creation, and every
# reading stage on the builder's sample, and the fault side of the outcome
# classes holds for real invocations
# (specs/design-spec-testing.md#outcome-classes).
class TestRunnerFsimg < Minitest::Test
  PROFILE = FsimgSpec.fetch_profile("quick")

  def setup
    @fsimg = ENV.fetch("FSIMG_BIN", "")
    skip "FSIMG_BIN names no fsimg binary" unless File.executable?(@fsimg)
  end

  def test_preflight_runs_every_stage_on_a_valid_image
    with_results do |results|
      runner, sandbox = preflight(results)

      assert_kind_of FsimgSpec::Runner, runner
      assert_includes %w[active unavailable disabled], sandbox
      refute File.exist?(File.join(results, "preflight"))
    end
  end

  # A host without confinement cannot satisfy -S required; the preflight
  # stops the run instead of retrying without it.
  def test_required_sandbox_without_confinement_fails_the_preflight
    with_results do |results|
      _, sandbox = preflight(results)
      skip "this host confines fsimg (#{sandbox})" unless sandbox == "unavailable"

      FileUtils.rm_r(Dir.glob(File.join(results, "*")))
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      environment = ENV.to_h.merge("FSIMG_SPEC_SANDBOX" => "required")
      assert_raises(FsimgSpec::PreflightError) { preflight(results, environment:) }
    end
  end

  # A tree as deep as the profile's depth ceiling extracts: extraction
  # holds one descriptor per level, and the fds limit covers them.
  def test_depth_witness_at_the_ceiling_extracts
    with_results do |results|
      runner, sandbox = preflight(results)
      directories = (1..PROFILE.ceilings.depth - 1).map { |level| entry(Array.new(level, "d").join("/"), "directory") }
      tree = [*directories, entry("#{directories.last.path}/f", "file")]
      kase = creation_case("depth", tree)

      result = FsimgSpec::CaseRun.call(kase, model: NoRecords.new, image: nil, runner:, sandbox:,
                                             cases_directory: File.join(results, "cases"), created: CREATED_VALID)
      assert_equal "passed", result.verdict, result.detail
    end
  end

  # The preflight's reading stages run on the builder's sample, never on
  # an image the writer under test created.
  def test_preflight_reads_the_builder_sample
    with_results do |results|
      read = []
      sample = lambda do |directory|
        FsimgSpec::Fat::Bases.preflight_sample(directory).tap { read << _1.first.id }
      end
      preflight(results, sample:)

      assert_equal ["preflight-sample"], read
    end
  end

  # A worker the harness kills is a fault of fsimg's plumbing, never a
  # verdict on the image.
  def test_killed_worker_is_a_fault
    with_results do |results|
      _, sandbox = preflight(results)
      killer = WorkerKiller.new
      runner = runner_for(observer: killer)
      kase = creation_case("killed", (1..3000).map { entry("f#{_1}", "file") }, stages: %w[create ls])

      result = begin
        FsimgSpec::CaseRun.call(kase, model: NoRecords.new, image: nil, runner:, sandbox:,
                                      cases_directory: File.join(results, "cases"), created: CREATED_VALID)
      ensure
        killer.join
      end
      assert killer.armed?, "the runner never reported the ls stage's group"
      ls = result.stages.find { _1.stage == "ls" }
      skip "ls finished before a worker appeared" unless killer.killed?

      assert_equal "faulted", ls.outcome.kind, ls.outcome.detail
    end
  end

  private

  def with_results
    Dir.mktmpdir do |results|
      %w[cases trials].each { Dir.mkdir(File.join(results, _1)) }
      yield results
    end
  end

  def preflight(results, environment: ENV.to_h, sample: FsimgSpec::Fat::Bases.method(:preflight_sample)) =
    FsimgSpec::Preflight.new(results:, build_directory: results, profile: PROFILE, fsimg: @fsimg,
                             environment:, sample:).call

  def runner_for(observer:) =
    FsimgSpec::Runner.new(fsimg: @fsimg, profile: PROFILE, environment: ENV.to_h, observer:,
                          text_bytes: 64 << 20, address_space: false)

  def entry(path, type) = FsimgSpec::TreeEntry.new(path:, type:, size: type == "file" ? 10 : 0, seed: type == "file" ? 1 : 0)

  def creation_case(id, tree, stages: %w[create extract])
    recipe = FsimgSpec::Recipe.new(
      id:, format: "fat32", kind: "valid", stages:,
      source: FsimgSpec::Source.new(tree:, settings: {}),
      expected: stages.to_h { [_1, FsimgSpec::Expectation.parse("accept")] }
    )
    FsimgSpec::Case.new(recipe:, classification: FsimgSpec::Classification.new(violations: [], unclassified: []),
                        intended_rule: nil, expected: recipe.expected)
  end

  # A model whose stages report no records to compare.
  class NoRecords
    def record_problems(_stage, _records) = []
  end

  # The created hook for an image these cases do not classify.
  CREATED_VALID = ->(_image) { [NoRecords.new, FsimgSpec::Classification.new(violations: [], unclassified: [])] }

  # An observer that, for the second invocation (ls after create), kills
  # every member of its group but the coordinator once one appears, and
  # stops looking once that invocation ends. A group with members keeps
  # its ID, so pgrep -g names only that invocation's processes.
  class WorkerKiller < FsimgSpec::GroupObserver
    def initialize
      super
      @invocations = 0
    end

    def started(pgid)
      @invocations += 1
      return unless @invocations == 2

      @target = pgid
      @thread = Thread.new { kill_workers(pgid) }
      @thread.report_on_exception = false
    end

    def ended(pgid)
      @ended = true if pgid == @target
    end

    # Waits for the killer, which stops once the invocation ends, and
    # raises what failed it, such as a missing pgrep.
    def join = @thread&.join

    # Whether the killer saw the ls stage's group start.
    def armed? = !@thread.nil?

    # Whether the killer killed a worker.
    def killed? = @killed == true

    private

    def kill_workers(group)
      until @ended
        workers = IO.popen(["pgrep", "-g", group.to_s], &:read).split.map { Integer(_1) } - [group]
        if workers.any?
          @killed = true
          return workers.each { Process.kill(:KILL, _1) }
        end

        sleep 0.005
      end
    rescue Errno::ESRCH
      nil
    end
  end
end
