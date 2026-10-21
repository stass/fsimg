# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"
require "minitest"
require "tmpdir"
require "fsimg_spec"
require_relative "synthetic"

# The runner gives fsimg relative operands, a fixed environment and the
# profile's limits, and its checks after every stage catch a tool that
# misbehaves in its case directory (specs/design-spec-testing.md#runner).
# A stub script stands in for fsimg.
class TestRunner < Minitest::Test
  CEILINGS = FsimgSpec::Ceilings.new(image: 1 << 20, memory: 64 << 20, scratch: 2 << 20, entries: 100, depth: 8,
                                     path: 256, fds: 64, output: 1 << 20, work: 1 << 20, seconds: 10)
  PROFILE = FsimgSpec::Profile.new(name: "test", ceilings: CEILINGS, tree_draws: 0, tree_sample: 0, populated_trees: 0)
  SUMMARY = '{"schema":1,"type":"summary","complete":true,"sandbox":"unavailable"}'
  ACCEPT = "echo '#{SUMMARY}'".freeze
  STAGES = %w[info ls extract verify].freeze

  def test_reading_commands_use_relative_operands_and_the_profiles_limits
    runner = runner_for("/opt/fsimg")
    limits = %w[-l seconds=10 -l entries=100 -l depth=8 -l path=256 -l output=1048576 -l scratch=2097152 -l fds=64]

    assert_equal ["/opt/fsimg", "i", "--json", "-m", "67108864", *limits, "-T", "scratch", "-f", "image.img"],
                 runner.command("info", recipe)
    assert_equal %w[-E -f image.img -C dest], runner.command("extract", recipe).last(5)
    assert_equal %w[-d -f image.img], runner.command("verify-deep", recipe).last(3)
    assert_equal %w[-F fat12 -f image.img], runner.command("ls", recipe.with(forced_format: "fat12")).last(4)
  end

  def test_each_stage_runs_its_mode
    modes = %w[info ls extract verify verify-deep].to_h { [_1, runner_for("/opt/fsimg").command(_1, recipe)[1]] }

    assert_equal({ "info" => "i", "ls" => "t", "extract" => "x", "verify" => "W", "verify-deep" => "W" }, modes)
  end

  # create reads the manifest as an @ operand, the second create its
  # reversal, and a host-tree case the contents of source/.
  def test_create_command_maps_the_settings_and_the_source_root
    runner = runner_for("/opt/fsimg")
    recipe = creation_recipe(settings: { size: 65_536, cluster: 4096 })
    host_tree = recipe.with(source: recipe.source.with(root: "directory"))

    assert_equal "c", runner.command("create", recipe)[1]
    assert_equal %w[-R -F fat16 -o cluster=4096 -s 65536 -f out/created.img @source.mtree], runner.command("create", recipe).last(10)
    assert_equal %w[-f out/reversed.img @source-reversed.mtree], runner.command("create-reversed", recipe).last(3)
    assert_equal %w[-f out/created.img -C source .], runner.command("create", host_tree).last(5)
  end

  def test_required_sandbox_passes_s_required
    runner = runner_for("/opt/fsimg", environment: { "PATH" => "/bin", "FSIMG_SPEC_SANDBOX" => "required" })

    assert_equal %w[-S required], runner.command("info", recipe).each_cons(2).find { _1 == %w[-S required] }
  end

  # fsimg sees PATH, LC_ALL, TZ, HOME, TMPDIR and the four sanitizer
  # variables, each ending with the options that send a report to stderr
  # and exit 87, and nothing else.
  def test_stub_sees_only_the_fixed_environment
    with_case(script: "env >&2; exit 87") do |result, directory|
      environment = result.execution.stderr.lines(chomp: true).to_h { _1.split("=", 2) }
      sanitizers = FsimgSpec::Runner::SANITIZER_VARIABLES

      assert_equal "sanitizer_failure", result.outcome.kind
      assert_equal %w[HOME LC_ALL PATH TMPDIR TZ].concat(sanitizers).sort, environment.keys.reject { _1 == "PWD" || _1 == "SHLVL" || _1 == "_" }.sort
      assert_equal [directory, "#{directory}/scratch", "C", "UTC"], environment.values_at("HOME", "TMPDIR", "LC_ALL", "TZ")
      assert(sanitizers.all? { environment.fetch(_1).end_with?("abort_on_error=0:log_path=stderr:exitcode=87") })
    end
  end

  def test_sanitizer_options_from_the_harness_come_first
    environment = { "PATH" => "/bin", "ASAN_OPTIONS" => "detect_leaks=1" }

    assert_equal "detect_leaks=1:abort_on_error=0:log_path=stderr:exitcode=87",
                 runner_for("/x", environment:).fsimg_environment("/c").fetch("ASAN_OPTIONS")
  end

  # Each stub misbehaves in one way the checks after the stage must find.
  def test_checks_after_a_stage_find_each_misbehaviour
    misbehaviours = {
      "a link in the destination" => ["ln -s /etc dest/link; #{ACCEPT}", /dest holds the link/],
      "a hard link in the destination" => ["echo x > dest/a && ln dest/a dest/b; #{ACCEPT}", /dest holds the hard link/],
      "a file outside the destination" => ["echo x > stray; #{ACCEPT}", /unexpected "stray"/],
      "a changed image" => ["echo x >> image.img; #{ACCEPT}", /image.img changed/],
      "a scratch file left behind" => ["echo x > scratch/left; #{ACCEPT}", %r{scratch/"left" was left behind}],
      "a child left alive" => ["sleep 30 & #{ACCEPT}", /outlived fsimg/],
      "an oracle configuration" => ["echo x > .mtoolsrc; #{ACCEPT}", /unexpected ".mtoolsrc"/],
      "dest as a link" => ["rmdir dest && ln -s / dest; #{ACCEPT}", /"dest" is a link, not a directory/],
      "scratch as a link" => ["rmdir scratch && ln -s / scratch; #{ACCEPT}", /"scratch" is a link, not a directory/]
    }

    misbehaviours.each do |behaviour, (script, problem)|
      with_case(script:, stage: "extract") do |result|
        assert_equal "accepted", result.outcome.kind, behaviour
        assert_match problem, result.problems.join("; "), behaviour
      end
    end
  end

  # The names a case may hold follow its progress: no dest/ before
  # extract, no capture of a stage still to run, no created.img in a
  # reading case, and no oracle/, since the oracles read from and write to
  # pipes.
  def test_entries_a_stage_had_no_business_writing_are_found
    script = "mkdir dest; mkdir oracle; echo x > verify.stdout; echo x > out/created.img; #{ACCEPT}"
    with_case(script:) do |result|
      problems = result.problems.join("; ")

      [/unexpected "dest"/, /unexpected "oracle"/, /unexpected "verify.stdout"/,
       %r{out/"created.img" was left behind}].each { assert_match _1, problems }
    end
  end

  # A stage records the files the case directory holds after it and
  # their logical bytes: here the image and the stage's two captures.
  def test_stage_records_the_case_directory_footprint
    with_case(script: ACCEPT) do |result|
      captured = result.execution.stdout.bytesize + result.execution.stderr.bytesize

      assert_equal FsimgSpec::Footprint.new(files: 3, bytes: 512 + captured), result.footprint
    end
  end

  # A directory the stage left unreadable is still a left-behind entry,
  # never a fault of the footprint's walk. The ensure restores its mode so
  # the temporary directory can be removed; run as root, the test passes
  # trivially.
  def test_unreadable_directory_left_behind_is_a_problem
    with_case(script: "mkdir scratch/x; chmod 000 scratch/x; #{ACCEPT}") do |result, directory|
      assert_match %r{scratch/"x" was left behind}, result.problems.join("; ")
    ensure
      FileUtils.chmod(0o700, File.join(directory, "scratch", "x")) if directory && File.directory?(File.join(directory, "scratch", "x"))
    end
  end

  # An oracle that raises, a harness error or any other StandardError, is
  # the harness's fault, but it neither hides fsimg's own verdict nor the
  # stages that ran.
  def test_an_oracle_that_raises_keeps_the_stages_and_fsimg_verdict
    Dir.mktmpdir do |cases|
      passed = run_case(write_stub(cases, ACCEPT), cases, oracles: ->(_directory, _model) { raise "fsck answered nothing" })
      FileUtils.rm_rf(File.join(cases, "t.case"))
      raising = ->(_directory, _model) { raise FsimgSpec::HarnessError, "fsck could not start" }
      failed = run_case(write_stub(cases, "echo x; #{ACCEPT}"), cases, oracles: raising)

      assert_equal ["harness_error", STAGES, 'oracles: RuntimeError: "fsck answered nothing"'],
                   [passed.verdict, passed.stages.map(&:stage), passed.detail]
      assert_equal ["failed", STAGES], [failed.verdict, failed.stages.map(&:stage)]
      assert_match(/\Ainfo: expected accept, got harness_error .*; oracles: HarnessError: "fsck could not start"\z/, failed.detail)
    end
  end

  # A dest that is a link to / is reported, never walked, so the checks
  # cannot hash the host's files.
  def test_destination_link_is_not_walked
    with_case(script: "rmdir dest && ln -s / dest; #{ACCEPT}", stage: "extract") do |result|
      assert_empty result.tree
      refute_empty result.problems
    end
  end

  # extract -E fills only an empty directory private to its caller, so the
  # runner creates dest/ with mode 0700 just before extract runs.
  def test_extract_finds_an_empty_private_destination
    with_case(script: "ls -A dest >&2; #{ACCEPT}", stage: "extract") do |result, directory|
      assert_equal ["accepted", "", []], [result.outcome.kind, result.execution.stderr, result.problems]
      assert_equal 0o700, File.lstat(File.join(directory, "dest")).mode & 0o7777
    end
  end

  # A failed extract leaves its dest/, perhaps partly filled; the walk
  # still checks what it holds.
  def test_rejected_extract_leaves_a_checked_destination
    error = '{"schema":1,"type":"error","code":-12,"resource":"none","sandbox":"unavailable"}'

    with_case(script: "echo x > dest/a; ln -s /etc dest/link; echo '#{error}'; exit 1", stage: "extract") do |result|
      assert_equal "rejected:FSIMG_EEXISTS", result.outcome.to_s
      assert_equal ["dest holds the link \"link\""], result.problems
    end
  end

  # dest/ already in the case directory before extract means the checks
  # after an earlier stage were bypassed: a harness error, not a verdict.
  def test_destination_before_extract_is_a_harness_error
    Dir.mktmpdir do |root|
      directory = File.join(root, "case")
      FsimgSpec::CaseDirectory.prepare(directory, recipe:, image: image(root))
      Dir.mkdir(File.join(directory, "dest"))

      assert_raises(FsimgSpec::Error) do
        runner_for(write_stub(root, ACCEPT)).run("extract", recipe:, case_directory: directory, sandbox: "unavailable")
      end
    end
  end

  # A link planted where a capture goes is reported, and the capture never
  # writes through it to the host path it names.
  def test_capture_never_writes_through_a_planted_link
    with_case(script: "ln -s ../victim extract.stdout; #{ACCEPT}", stage: "extract") do |result, directory|
      assert_includes result.problems, "extract.stdout existed before its capture"
      refute File.exist?(File.join(File.dirname(directory), "victim"))
    end
  end

  # RLIMIT_FSIZE admits a file as large as the scratch ceiling, the largest
  # of the image, output and scratch ceilings here, so a legitimate
  # FSIMG_ELIMIT never becomes SIGXFSZ.
  def test_scratch_file_at_the_ceiling_is_allowed
    script = "dd if=/dev/zero of=scratch/big bs=1048576 count=2 2>/dev/null && rm scratch/big && #{ACCEPT}"

    with_case(script:) { |result| assert_equal ["accepted", []], [result.outcome.kind, result.problems] }
  end

  def test_error_record_names_what_it_retained
    error = '{"schema":1,"type":"error","code":-4,"resource":"none","sandbox":"unavailable",' \
            '"retained_scratch":"scratch/kept","retained_output":""}'

    with_case(script: "mkdir scratch/kept; echo '#{error}'; exit 1") do |result|
      assert_equal ["rejected:FSIMG_EFORMAT", []], [result.outcome.to_s, result.problems]
    end
  end

  # A failed safety check ends the case: no later stage runs, no oracle
  # reads the image, so the .mtoolsrc the stage planted reaches none, and
  # the case directory stays for the failure record.
  def test_unsafe_stage_ends_the_case_before_any_oracle_runs
    Dir.mktmpdir do |cases|
      stub = write_stub(cases, "echo x > .mtoolsrc; #{ACCEPT}")
      observed = []
      result = run_case(stub, cases, oracles: ->(directory, _model) { observed << directory })

      assert_equal "failed", result.verdict
      assert_equal ["info"], result.stages.map(&:stage)
      assert_empty observed
      assert_empty result.observations
      assert File.directory?(File.join(cases, "t.case"))
    end
  end

  # The oracles read the case directory after the last stage, before a
  # passing case's directory is removed, and the case result keeps what
  # they observed.
  def test_passing_case_runs_every_stage_then_the_oracles_and_removes_its_directory
    Dir.mktmpdir do |cases|
      oracles = lambda do |directory, _model|
        state = File.file?(File.join(directory, "image.img")) ? "agrees" : "disagrees"
        FsimgSpec::Observed.new(observations: { "t.tool" => FsimgSpec::Observation.new(state:) })
      end
      result = run_case(write_stub(cases, ACCEPT), cases, oracles:)

      assert_equal ["passed", STAGES], [result.verdict, result.stages.map(&:stage)]
      assert_equal({ "t.tool" => FsimgSpec::Observation.agrees }, result.observations)
      refute File.exist?(File.join(cases, "t.case"))
    end
  end

  def test_two_cases_with_one_id_are_a_harness_error
    Dir.mktmpdir do |cases|
      stub = write_stub(cases, "echo x > stray; #{ACCEPT}")
      run_case(stub, cases)

      assert_raises(FsimgSpec::HarnessError) { run_case(stub, cases) }
    end
  end

  # A host tree whose names collide on the host cannot be held there: the
  # case is a host skip and fsimg never runs.
  def test_host_tree_the_host_cannot_hold_is_a_host_skip
    Dir.mktmpdir do |cases|
      marker = File.join(cases, "ran")
      stub = write_stub(cases, "touch '#{marker}'; : > out/created.img; #{ACCEPT}")
      tree = %w[README readme].map { FsimgSpec::TreeEntry.file(_1, size: 1, seed: 1) }
      kase = plain_case(creation_recipe(settings: {}).with(source: FsimgSpec::Source.new(tree:, settings: {}, root: "directory")))
      result = FsimgSpec::CaseRun.call(kase, model: NoRecords.new, image: nil, runner: runner_for(stub),
                                             cases_directory: cases, sandbox: "unavailable", created: CREATED_VALID)

      assert_equal (folds_case?(cases) ? "host_skip" : "passed"), result.verdict
      assert_equal !folds_case?(cases), File.exist?(marker)
      refute File.exist?(File.join(cases, "t.create"))
    end
  end

  # A stage that may accept or reject, as under a safety_only violation,
  # still fails when it accepts with records the model contradicts.
  def test_accepting_stage_records_must_match_the_model
    Dir.mktmpdir do |cases|
      either = FsimgSpec::Expectation.parse("accept|reject:*")
      kase = FsimgSpec::Case.new(recipe:, classification: CLEAN,
                                 intended_rule: nil, expected: STAGES.to_h { [_1, either] })
      result = FsimgSpec::CaseRun.call(kase, model: Contradicting.new, image: image(cases), runner: runner_for(write_stub(cases, ACCEPT)),
                                             cases_directory: cases, sandbox: "unavailable")

      assert_equal ["failed", "info: the model disagrees"], [result.verdict, result.detail]
    end
  end

  # An accepting create must write an image that violates no rule: here
  # the image violates a verification rule, which fails create and ends
  # the case before verify runs. A create that already fails its own
  # expectation is reported by that.
  def test_created_image_that_violates_a_rule_fails_create
    Dir.mktmpdir do |cases|
      stub = write_stub(cases, "test -e out/created.img || : > out/created.img; #{ACCEPT}")
      registry = FsimgSpec::Registry.new([Synthetic.rule("t.crosslink", policy: "must_reject", scopes: ["verification"],
                                                                         status: "FSIMG_ECORRUPT")])
      accepting = creation_recipe(settings: {}).with(stages: %w[create verify], expected: %w[create verify].to_h { [_1, FsimgSpec::Expectation.parse("accept")] })
      rejecting = accepting.with(id: "t.rejecting", expected: accepting.expected.merge("create" => FsimgSpec::Expectation.parse("reject:FSIMG_EINVAL")))
      created = ->(_image) { [NoRecords.new, Synthetic.classify(registry, { "t.crosslink" => true })] }
      results = [accepting, rejecting].map do
        FsimgSpec::CaseRun.call(plain_case(_1), model: NoRecords.new, image: nil, runner: runner_for(stub),
                                                cases_directory: cases, sandbox: "unavailable", created:)
      end

      assert_equal [["failed", "create: the image it wrote violates t.crosslink", ["create"]],
                    ["failed", "create: expected reject:FSIMG_EINVAL, got accepted", ["create"]]],
                   results.map { [_1.verdict, _1.detail, _1.stages.map(&:stage)] }
    end
  end

  # A created image that is no regular file fails the case before the
  # created hook could open it.
  def test_created_image_must_be_a_regular_file
    Dir.mktmpdir do |cases|
      stub = write_stub(cases, "mkfifo out/created.img; #{ACCEPT}")
      opened = false
      created = lambda { |image|
        opened = true
        CREATED_VALID.call(image)
      }
      result = FsimgSpec::CaseRun.call(plain_case(creation_recipe(settings: {})), model: NoRecords.new, image: nil,
                                       runner: runner_for(stub), cases_directory: cases, sandbox: "unavailable", created:)

      assert_equal ["failed", "create: out/created.img is a fifo, not a file"], [result.verdict, result.detail]
      refute opened
    end
  end

  # The second create reads the manifest's lines reversed and must write
  # the bytes create wrote: a stub that writes other bytes for it fails
  # the case, and one that writes the same passes.
  def test_second_create_must_write_the_same_bytes
    Dir.mktmpdir do |cases|
      results = %w[a b].map do |reversed|
        FileUtils.rm_rf(File.join(cases, "t.create"))
        stub = write_stub(cases, <<~SH)
          case "$*" in
          *@source-reversed.mtree*) printf #{reversed} > out/reversed.img ;;
          *) printf a > out/created.img ;;
          esac
          #{ACCEPT}
        SH
        FsimgSpec::CaseRun.call(plain_case(creation_recipe(settings: {})), model: NoRecords.new, image: nil,
                                runner: runner_for(stub), cases_directory: cases, sandbox: "unavailable", created: CREATED_VALID)
      end

      assert_equal [["passed", nil], ["failed", "create-reversed: out/reversed.img differs from out/created.img in the " \
                                                "1048576-byte block at 0"]],
                   results.map { [_1.verdict, _1.detail] }
      assert_equal %w[create create-reversed], results.last.stages.map(&:stage)
    end
  end

  # out/ may hold created.img only once create has accepted, and
  # reversed.img only once the second create has: a rejected create that
  # leaves an image, or a create that writes the reversal's, left it behind.
  def test_created_images_are_allowed_only_after_their_create_accepts
    rejection = '{"schema":1,"type":"error","code":-1,"resource":"none","sandbox":"unavailable"}'
    {
      "printf a > out/created.img; echo '#{rejection}'; exit 1" => %r{out/"created.img" was left behind},
      "printf a > out/created.img; printf a > out/reversed.img; #{ACCEPT}" => %r{out/"reversed.img" was left behind}
    }.each do |script, problem|
      Dir.mktmpdir do |root|
        stub = write_stub(root, script)
        directory = File.join(root, "case")
        FsimgSpec::CaseDirectory.prepare(directory, recipe: creation_recipe(settings: {}), image: nil)
        result = runner_for(stub).run("create", recipe: creation_recipe(settings: {}), case_directory: directory,
                                                sandbox: "unavailable")

        assert_match problem, result.problems.join("; "), script
      end
    end
  end

  # A second image that matches the first is removed once compared, so the
  # case holds one image from there on, and a later stage that writes one
  # left it behind.
  def test_matching_second_image_is_removed
    Dir.mktmpdir do |root|
      stub = write_stub(root, <<~SH)
        case "$*" in
        *@source-reversed.mtree*|"i "*) printf a > out/reversed.img ;;
        *) printf a > out/created.img ;;
        esac
        #{ACCEPT}
      SH
      directory = File.join(root, "case")
      recipe = creation_recipe(settings: {}).with(stages: %w[create info], expected: %w[create info].to_h { [_1, FsimgSpec::Expectation.parse("accept")] })
      FsimgSpec::CaseDirectory.prepare(directory, recipe:, image: nil)
      runner = runner_for(stub)
      run = ->(stage) { runner.run(stage, recipe:, case_directory: directory, sandbox: "unavailable") }
      reproduced = ["create", FsimgSpec::Runner::REPRODUCTION].map(&run)

      assert_equal [[], []], reproduced.map(&:problems)
      assert_nil runner.settle_reproduction(directory)
      assert_equal ["created.img"], Dir.children(File.join(directory, "out"))
      assert_equal ['out/"reversed.img" was left behind'], run.call("info").problems
    end
  end

  # A manifest case's directory holds its manifest and the same lines
  # reversed, and no source tree.
  def test_creation_case_directory_holds_the_manifest_and_its_reversal
    Dir.mktmpdir do |root|
      tree = [FsimgSpec::TreeEntry.directory("d"), FsimgSpec::TreeEntry.file("d/f", size: 3, seed: 1)]
      recipe = creation_recipe(settings: {}).with(source: FsimgSpec::Source.new(tree:, settings: {}))
      directory = File.join(root, "case")
      FsimgSpec::CaseDirectory.prepare(directory, recipe:, image: nil)

      lines = FsimgSpec::Manifest.lines(tree)
      assert_equal %w[out scratch source-reversed.mtree source.mtree], Dir.children(directory).sort
      assert_equal [lines.join, lines.reverse.join], %w[source.mtree source-reversed.mtree].map { File.binread(File.join(directory, _1)) }
    end
  end

  def test_creation_case_without_the_created_hook_is_a_harness_error
    Dir.mktmpdir do |cases|
      assert_raises(FsimgSpec::HarnessError) do
        FsimgSpec::CaseRun.call(plain_case(creation_recipe(settings: {})), model: NoRecords.new, image: nil,
                                runner: runner_for(write_stub(cases, ACCEPT)), cases_directory: cases, sandbox: "unavailable")
      end
    end
  end

  private

  # The created hook for an image that violates nothing.
  CREATED_VALID = ->(_image) { [NoRecords.new, FsimgSpec::Case::CREATED_IMAGE] }

  # A classification with no violation and nothing unclassified, on a
  # host that held every name.
  CLEAN = FsimgSpec::Classification.new(violations: [], unclassified: [], trial_answers: FsimgSpec::TrialAnswers::HELD)

  def plain_case(recipe) =
    FsimgSpec::Case.new(recipe:, classification: CLEAN, intended_rule: nil, expected: recipe.expected,
                        source_classification: recipe.reading? ? nil : CLEAN)

  def folds_case?(directory)
    probe = File.join(directory, "Probe")
    File.write(probe, "")
    File.exist?(File.join(directory, "probe")).tap { File.unlink(probe) }
  end

  # A model that contradicts every record.
  class Contradicting
    def record_problems(_result) = ["the model disagrees"]
  end

  def runner_for(fsimg, environment: { "PATH" => "/bin:/usr/bin" }) =
    FsimgSpec::Runner.new(fsimg:, profile: PROFILE, environment:,
                          text_bytes: 1 << 16, address_space: false)

  def recipe = FsimgSpec::Recipe.new(
    id: "t.case", format: "fat16", kind: "valid", stages: STAGES,
    expected: STAGES.to_h { [_1, FsimgSpec::Expectation.parse("accept")] },
    base: FsimgSpec::Base.new(builder: "independent", parameters: {}, tree: []),
    base_sha256: "0" * 64, tool_versions: {}, mutations: []
  )

  def creation_recipe(settings:) = FsimgSpec::Recipe.new(
    id: "t.create", format: "fat16", kind: "valid", stages: %w[create],
    expected: { "create" => FsimgSpec::Expectation.parse("accept") },
    source: FsimgSpec::Source.new(tree: [], settings:)
  )

  def write_stub(directory, script)
    stub = File.join(directory, "stub")
    File.write(stub, "#!/bin/sh\n#{script}\n")
    File.chmod(0o700, stub)
    stub
  end

  # Runs one stage of the reading recipe with the stub as fsimg, in a
  # prepared case directory.
  def with_case(script:, stage: "info")
    Dir.mktmpdir do |root|
      stub = write_stub(root, script)
      directory = File.join(root, "case")
      FsimgSpec::CaseDirectory.prepare(directory, recipe:, image: image(root))
      yield runner_for(stub).run(stage, recipe:, case_directory: directory, sandbox: "unavailable"), directory
    end
  end

  def run_case(stub, cases, oracles: nil)
    kase = FsimgSpec::Case.new(recipe:, classification: CLEAN,
                               intended_rule: nil, expected: recipe.expected)
    FsimgSpec::CaseRun.call(kase, model: NoRecords.new, image: image(cases), runner: runner_for(stub),
                                  cases_directory: cases, sandbox: "unavailable", oracles:)
  end

  def image(directory)
    base = File.join(directory, "base.img")
    File.binwrite(base, "\0" * 512) unless File.exist?(base)
    FsimgSpec::ImageView.open(base)
  end

  # A model whose stages report no records to compare.
  class NoRecords
    def record_problems(_result) = []
  end
end
