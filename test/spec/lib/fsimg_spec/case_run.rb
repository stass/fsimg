# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # A second case with one id in a run, or another fault of the harness
  # rather than of fsimg.
  class HarnessError < Error; end

  # A case's directory (specs/design-spec-testing.md#runner): created
  # exclusively, with scratch/ and out/, and what the case reads: a reading
  # case's image; a creation case's manifest and the same lines reversed,
  # so the tree never touches the host; or a host-tree creation case's
  # source tree, written through the name funnel. dest/ stays absent until
  # the runner creates it for extract.
  module CaseDirectory
    SUBDIRECTORIES = %w[scratch out].freeze

    # Prepares directory for recipe, writing image, an ImageView, for a
    # reading case. manifest writes the lines of a tree (Manifest); a test
    # may pass a faulty one. Answers nil, or why the host cannot hold a
    # host-tree case's source tree, which makes the case a host skip.
    def self.prepare(directory, recipe:, image:, manifest: Manifest)
      Dir.mkdir(directory, 0o700)
      SUBDIRECTORIES.each { Dir.mkdir(File.join(directory, _1), 0o700) }
      if recipe.reading?
        image.write(File.join(directory, Runner::IMAGE))
        nil
      elsif recipe.source.manifest?
        lines = manifest.lines(recipe.source.tree)
        write(directory, Manifest::NAME, lines.join)
        write(directory, Manifest::REVERSED, lines.reverse.join)
        nil
      else
        Dir.mkdir(File.join(directory, "source"), 0o700)
        SourceTree.write(File.join(directory, "source"), recipe.source.tree)
      end
    rescue Errno::EEXIST
      raise HarnessError, "#{FsimgSpec.shown(File.basename(directory))}: a case directory with this id exists"
    end

    private_class_method def self.write(directory, name, bytes)
      answer = NameFunnel.create_file(directory, name) { _1.write(bytes) }
      raise HarnessError, "#{name} could not be created in a new case directory (#{answer})" unless answer == :created
    end
  end

  # How one case ended: passed, failed, host_skip or harness_error, with
  # each stage's result, for a verdict other than passed why, and the
  # oracles' observations of its image, tool => Observation. Only the
  # checker and the read-back of an image fsimg created give a verdict
  # (Observed); every other observation is recorded only, though an
  # oracle that cannot run is a harness error.
  class CaseResult < Data.define(:id, :verdict, :stages, :detail, :observations)
    include Checked

    def initialize(observations: {}, **fields) = super
  end

  # What the oracles found in a case's image
  # (specs/design-spec-testing.md#independence-and-oracles): one
  # Observation per tool, and problem, why the oracles with a verdict, the
  # checker and the read-back of an image fsimg created, fail the case, or
  # nil.
  class Observed < Data.define(:observations, :problem)
    include Checked

    def initialize(observations:, problem: nil) = super

    NONE = new(observations: {}.freeze)
  end

  # Runs one case through its stages and judges each against its derived
  # expectation. A stage that fails a safety check ends the case, as does a
  # create that does not accept, which leaves the reading stages no image.
  # Only accepted and rejected outcomes can satisfy an expectation, and an
  # accepted stage's records must also match the model. A passing case's
  # directory is deleted, as is a host skip's.
  #
  # A creation case needs created, which decodes the image create wrote
  # and answers its model, its Classification, and why its layout is not
  # the one the creation's settings and tree call for, or nil: create is
  # judged by the source model, and the reading stages after it by the
  # image it created. An accepting create must also write an image that
  # violates no rule (Case#created_image_problem) and has that layout; one
  # that does not fails create and ends the case. A manifest case then
  # runs the second create, from the manifest's lines reversed, which must
  # accept and write the same bytes (specs/design-spec-testing.md#cases),
  # or the case fails and ends.
  #
  # oracles, when the case's image is one the oracles read, takes the case
  # directory and the image's model, a creation case's created image's,
  # and answers what they observed in the image, an Observed. They run
  # after the last stage, unless a stage failed a safety check, which ends
  # the case before any oracle runs, or create wrote no image. An oracle
  # that cannot run makes a case fsimg passed a harness error, and leaves
  # a failed case failed.
  module CaseRun
    # One stage's result with what judges it: its expectation, the model
    # its records must match, and problem, why its output fails it beyond
    # those, or nil.
    Judged = Data.define(:result, :expected, :model, :problem)

    # What the second create of a manifest case must do: accept.
    REPRODUCED = Expectation.new(accept: true)

    def self.call(kase, model:, image:, runner:, cases_directory:, sandbox:, created: nil, oracles: nil,
                  stages: kase.recipe.stages)
      raise HarnessError, "#{kase.recipe.id}: a creation case needs the created hook" if !kase.recipe.reading? && created.nil?

      directory = File.join(cases_directory, kase.recipe.id)
      refusal = CaseDirectory.prepare(directory, recipe: kase.recipe, image:)
      if refusal
        FileUtils.rm_r(directory, secure: true)
        return CaseResult.new(id: kase.recipe.id, verdict: "host_skip", stages: [], detail: refusal)
      end

      judged, image_model = run_stages(kase, model:, runner:, directory:, sandbox:, stages:, created:)
      observed, oracle_error = if image_model && judged.all? { _1.result.safe? }
                                 observe(oracles, directory, image_model)
                               else
                                 [Observed::NONE, nil]
                               end
      failures = [judged.lazy.filter_map { failure(_1) }.first, observed.problem].compact
      failure = failures.empty? ? nil : failures.join("; ")
      verdict = if failure then "failed"
                elsif oracle_error then "harness_error"
                else "passed"
                end
      FileUtils.rm_r(directory, secure: true) if verdict == "passed"
      CaseResult.new(id: kase.recipe.id, verdict:, stages: judged.map(&:result),
                     detail: [failure, oracle_error].compact.join("; ").then { _1.empty? ? nil : _1 },
                     observations: observed.observations)
    end

    # What the oracles observed in the case directory, and nil; or nothing
    # observed and the harness error that kept them from running, such as
    # a tool that could not start. The error never hides fsimg's own
    # verdict or the stages that ran, so this is the oracles' boundary: it
    # records any StandardError as the case's harness error, where the case
    # phase's funnel would discard the stages. A ContainmentError or a
    # signal still ends the phase.
    private_class_method def self.observe(oracles, directory, model)
      return [Observed::NONE, nil] unless oracles

      [oracles.call(directory, model), nil]
    rescue StandardError => e
      [Observed::NONE, "oracles: #{FsimgSpec.error_label(e)}"]
    end

    # Each stage's Judged, up to the stage that ends the case, and the model
    # of the image the stages read, or nil while there is none. model is a
    # reading case's image model, or a creation case's source model, which
    # judges create; once create has accepted, the created image's case
    # and model judge the stages after it (after_create).
    private_class_method def self.run_stages(kase, model:, runner:, directory:, sandbox:, stages:, created:)
      judged = []
      image_model = kase.recipe.reading? ? model : nil
      stages.each do |stage|
        result = runner.run(stage, recipe: kase.recipe, case_directory: directory, sandbox:)
        ran = [Judged.new(result:, expected: kase.expected.fetch(stage), model: image_model || model, problem: nil)]
        if stage == "create" && !ends_case?(ran.first)
          ran, kase, image_model = after_create(ran.first, kase:, created:, runner:, directory:, sandbox:)
        end
        judged.concat(ran)
        break if ends_case?(judged.last)
      end
      [judged, image_model]
    end

    # What follows an accepting create: the created hook decodes the image
    # it wrote, whose rules or layout may fail create; a manifest case then runs its
    # second create, unless create already failed. Answers the Judged of
    # create and of the second create, and the created image's case and
    # model.
    private_class_method def self.after_create(create, kase:, created:, runner:, directory:, sandbox:)
      image_model, classification, image_problem = created.call(File.join(directory, Runner::CREATED))
      kase = kase.created(classification)
      problems = [kase.created_image_problem, image_problem].compact
      create = create.with(problem: problems.empty? ? nil : problems.join("; "))
      reproduced = reproduction(runner, kase.recipe, directory:, sandbox:, model: create.model) if
        create.problem.nil? && kase.recipe.source.manifest?
      [[create, *reproduced], kase, image_model]
    end

    # The second create of a manifest case, judged by the bytes it wrote.
    # Its records describe a plan, as create's do, so the source model
    # judges them, which compares none.
    private_class_method def self.reproduction(runner, recipe, directory:, sandbox:, model:)
      result = runner.run(Runner::REPRODUCTION, recipe:, case_directory: directory, sandbox:)
      problem = runner.settle_reproduction(directory) if result.safe? && result.outcome.kind == "accepted"
      Judged.new(result:, expected: REPRODUCED, model:, problem:)
    end

    # A stage that failed a safety check ends the case, and so does a
    # create that did not accept, since the reading stages have no image,
    # and any stage whose output is wrong beyond its outcome.
    private_class_method def self.ends_case?(judged)
      result = judged.result
      !result.safe? || !judged.problem.nil? ||
        (Runner::WRITES.key?(result.stage) && result.outcome.kind != "accepted")
    end

    # Why the judged stage fails the case, or nil.
    private_class_method def self.failure(judged)
      result = judged.result
      expected = judged.expected
      if !result.safe?
        "#{result.stage}: #{result.problems.join("; ")}"
      elsif !expected.satisfied_by?(result.outcome)
        "#{result.stage}: expected #{expected}, got #{result.outcome}#{" (#{result.outcome.detail})" if result.outcome.detail}"
      elsif judged.problem
        "#{result.stage}: #{judged.problem}"
      elsif result.outcome.kind == "accepted" && !expected.unclassified
        judged.model.record_problems(result).then { _1.empty? ? nil : "#{result.stage}: #{_1.join("; ")}" }
      end
    end
  end
end
