# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # What the case phase of a run found: the sandbox status every invocation
  # reported, and for each format its rule coverage, its case counts and
  # the ids of its failed cases.
  CasePhaseResult = Data.define(:sandbox, :formats)
  FormatResult = Data.define(:rules, :cases, :failures)

  # Why a run has no case phase result: it named its test files and
  # skipped the phase, or the phase failed as a whole.
  CasesSkipped = Data.define(:reason)
  CasesFailed = Data.define(:reason)

  # A case before it is classified, as a format suite lists it: its id, the
  # rules it counts toward in the coverage report, why the host cannot run
  # it or nil, and a callable that classifies it, answering a Classified.
  # Classifying is deferred so that a fault in it is the case's own.
  PendingCase = Data.define(:id, :rules, :skip, :classify)

  # The case phase of a run (specs/design-spec-testing.md#runner): after the
  # preflight, every case of every format suite runs through fsimg, and
  # every final mismatch writes a failure record. Each case is classified,
  # run and recorded inside one funnel, run_case, where any exception is
  # that case's harness error; the phase continues and the run fails at the
  # end. Any exception outside a case ends the phase instead, as do a
  # ContainmentError and a signal; guarded turns each into the run's
  # CasesFailed. SIGINT and SIGTERM kill every live case group before the
  # harness exits.
  module CasePhase
    SUBDIRECTORIES = %w[cases trials failures].freeze

    # The profile, runner, sandbox status and fixture cache every case of a
    # phase uses.
    Setup = Data.define(:profile, :runner, :sandbox, :cache)

    # The format suites, each answering registry, cases(cache:, ceilings:)
    # and replay(recipe, cache:, ceilings:) with PendingCases, and
    # preflight_sample(directory). A run uses these; a test may pass its own.
    def self.suites = { "fat" => Fat::Suite }

    # The suite of a recipe's format: fat12, fat16 and fat32 are FAT's.
    def self.suite_for(format, suites: CasePhase.suites) =
      suites.fetch(format.sub(/\d+\z/, "")) { raise RecipeError, "no suite models the format #{FsimgSpec.shown(format)}" }

    # Creates the subdirectories of results and runs the preflight. The
    # preflight creates an image in the format of its creation recipe and
    # reads a sample of that format, so the sample comes from that format's
    # suite.
    def self.prepare(results:, profile:, build_directory:, environment:, suites: CasePhase.suites)
      SUBDIRECTORIES.each { Dir.mkdir(File.join(results, _1), 0o700) }
      sample = suite_for(Preflight::CREATION.format, suites:).method(:preflight_sample)
      runner, sandbox = Preflight.new(results:, build_directory:, profile:, fsimg: environment.fetch("FSIMG_BIN"),
                                      environment:, sample:).call
      Setup.new(profile:, runner:, sandbox:, cache: FixtureCache.new(File.join(build_directory, "test", "spec-cache")))
    end

    # How the phase ended, as a run takes it.
    Ending = Data.define(:cases, :fatal)

    # The phase behind its boundary: an Ending whose cases is the phase's
    # result, or CasesFailed for any exception that ended it, and whose
    # fatal is that exception when it must still end the run, or nil. The
    # caller writes its report before it raises a fatal exception again, so
    # a cancelled run still leaves a report and still dies by the signal.
    def self.guarded(**arguments)
      Ending.new(cases: call(**arguments), fatal: nil)
    rescue Exception => e
      reason = e.is_a?(SignalException) ? "interrupted by SIG#{Signal.signame(e.signo)}" : FsimgSpec.error_label(e)
      Ending.new(cases: CasesFailed.new(reason:), fatal: fatal?(e) ? e : nil)
    end

    # Whether e must end the run once the report is written: a signal, or a
    # fault of Ruby itself such as SystemStackError. The harness's own
    # errors, ContainmentError among them, are answered by the report, and
    # so is an exit, whose status would otherwise replace the run's verdict.
    private_class_method def self.fatal?(e) =
      case e
      in StandardError | ContainmentError | SystemExit then false
      else true
      end

    def self.call(profile:, results:, build_directory:, environment:, suites: CasePhase.suites)
      setup = prepare(results:, profile:, build_directory:, environment:, suites:)
      pending = suites.transform_values { _1.cases(cache: setup.cache, ceilings: profile.ceilings) }
      check_ids(pending.values.flatten.map(&:id))
      formats = pending.to_h do |format, cases|
        [format, run_suite(suites.fetch(format).registry, cases, setup:, results:)]
      end
      CasePhaseResult.new(sandbox: setup.sandbox, formats:)
    end

    # One case of a run names one case directory and one failure record,
    # so every id must be well formed and unique across the run.
    private_class_method def self.check_ids(ids)
      malformed = ids.grep_v(Recipe::ID_PATTERN)
      raise HarnessError, "malformed case ids: #{malformed.map { FsimgSpec.shown(_1) }.join(", ")}" unless malformed.empty?

      repeated = ids.tally.select { _2 > 1 }.keys
      raise HarnessError, "case ids repeat: #{repeated.join(", ")}" unless repeated.empty?
    end

    private_class_method def self.run_suite(registry, cases, setup:, results:)
      entries = cases.sort_by(&:id).map { run_case(_1, setup:, results:) }
      FormatResult.new(rules: Coverage.rules(registry, entries), cases: Coverage.cases(entries),
                       failures: entries.reject { passing?(_1.result) }.map(&:id))
    end

    # What one case came to: its Case and ImageView once classified, else
    # nil, and its CaseResult.
    Attempt = Data.define(:kase, :image, :result)

    # The funnel every case of a phase passes through: its attempt, and a
    # failure record unless it passed.
    private_class_method def self.run_case(pending, setup:, results:)
      attempt = attempt_case(pending, setup:, cases_directory: File.join(results, "cases"))
      result = if passing?(attempt.result)
                 attempt.result
               else
                 record_failure(pending.id, attempt, failures: File.join(results, "failures"), profile: setup.profile)
               end
      CaseEntry.new(id: pending.id, rules: pending.rules, kase: attempt.kase, result:)
    end

    # Classifies and runs a case. Any StandardError makes the case a harness
    # error; a ContainmentError or a signal is no StandardError, and ends the
    # phase. classified is still nil in the rescue when classifying raised,
    # and holds the classification when running raised, which the failure
    # record keeps so that it can replay the case.
    private_class_method def self.attempt_case(pending, setup:, cases_directory:)
      return Attempt.new(kase: nil, image: nil, result: skipped(pending)) if pending.skip

      classified = classify(pending)
      Attempt.new(kase: classified.kase, image: classified.image, result: result_of(classified, setup:, cases_directory:))
    rescue StandardError => e
      Attempt.new(kase: classified&.kase, image: classified&.image, result: harness_error(pending.id, e))
    end

    # Writes the failure record of a case that did not pass and answers its
    # result. A record that cannot be written makes the case a harness
    # error whose detail keeps the case's own.
    private_class_method def self.record_failure(id, attempt, failures:, profile:)
      FailureRecord.write(failures, id:, kase: attempt.kase, result: attempt.result, image: attempt.image, profile:)
      attempt.result
    rescue StandardError => e
      original = attempt.result
      harness_error(id, e, context: "its failure record, after #{original.verdict}: #{original.detail}")
    end

    # The host skip of a pending case the host cannot run.
    def self.skipped(pending) = CaseResult.new(id: pending.id, verdict: "host_skip", stages: [], detail: pending.skip)

    # The pending case classified. A refusal of the harness's own while
    # classifying, such as a rule the recipe names that these rules lack,
    # means the case cannot be judged under the current rules: a
    # RecipeError. Any other exception is a fault of the harness and
    # propagates, as does a classification under another id.
    def self.classify(pending)
      classified = begin
        pending.classify.call
      rescue Error => e
        raise RecipeError, "#{pending.id} cannot be classified: #{FsimgSpec.error_class(e)}: #{e.message}"
      end
      id = classified.kase.recipe.id
      raise HarnessError, "#{pending.id} was classified as #{id}" unless id == pending.id

      classified
    end

    # The CaseResult of a classified case: a harness error when its
    # generator was wrong or its recipe drifted, which runs nothing, and
    # otherwise its run in cases_directory. Exceptions propagate to the
    # caller's funnel.
    def self.result_of(classified, setup:, cases_directory:)
      id = classified.kase.recipe.id
      return CaseResult.new(id:, verdict: "harness_error", stages: [], detail: classified.defect) if classified.defect

      CaseRun.call(classified.kase, model: classified.model, image: classified.image, runner: setup.runner,
                                    sandbox: setup.sandbox, cases_directory:, created: classified.created)
    end

    private_class_method def self.harness_error(id, error, context: nil) =
      CaseResult.new(id:, verdict: "harness_error", stages: [], detail: [context, FsimgSpec.error_label(error)].compact.join(": "))

    # A host skip is no failure: the host cannot hold the case's tree.
    private_class_method def self.passing?(result) = %w[passed host_skip].include?(result.verdict)
  end
end
