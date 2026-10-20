# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # A second case with one id in a run, or another fault of the harness
  # rather than of fsimg.
  class HarnessError < Error; end

  # A case's directory (specs/design-spec-testing.md#runner): created
  # exclusively, with scratch/, out/ and oracle/, and either the case image
  # or, for a creation case, its source tree written through the name
  # funnel. dest/ stays absent until the runner creates it for extract.
  module CaseDirectory
    SUBDIRECTORIES = %w[scratch out oracle].freeze

    # Prepares directory for recipe, writing image, an ImageView, for a
    # reading case. Answers nil, or why the host cannot hold the case's
    # source tree, which makes the case a host skip.
    def self.prepare(directory, recipe:, image:)
      Dir.mkdir(directory, 0o700)
      SUBDIRECTORIES.each { Dir.mkdir(File.join(directory, _1), 0o700) }
      if recipe.reading?
        image.write(File.join(directory, Runner::IMAGE))
        nil
      else
        Dir.mkdir(File.join(directory, "source"), 0o700)
        SourceTree.write(File.join(directory, "source"), recipe.source.tree)
      end
    rescue Errno::EEXIST
      raise HarnessError, "#{FsimgSpec.shown(File.basename(directory))}: a case directory with this id exists"
    end
  end

  # How one case ended: passed, failed or host_skip, with each stage's
  # result and, for a failure, why.
  CaseResult = Data.define(:id, :verdict, :stages, :detail)

  # Runs one case through its stages and judges each against its derived
  # expectation. A stage that fails a safety check ends the case, as does a
  # create that does not accept, which leaves the reading stages no image.
  # Only accepted and rejected outcomes can satisfy an expectation, and an
  # accepted stage's records must also match the model. A passing case's
  # directory is deleted, as is a host skip's.
  #
  # A creation case needs created, which decodes the image create wrote
  # and answers its model and Classification: create is judged by the
  # source model, and the reading stages after it by the image it created.
  # An accepting create must also write an image that violates no rule
  # (Case#created_image_problem); one that does fails create and ends the
  # case.
  module CaseRun
    # One stage's result with the case and model that judge it, and for an
    # accepting create, why the image it wrote fails it, or nil.
    Judged = Data.define(:result, :kase, :model, :image_problem)

    def self.call(kase, model:, image:, runner:, cases_directory:, sandbox:, created: nil, stages: kase.recipe.stages)
      raise HarnessError, "#{kase.recipe.id}: a creation case needs the created hook" if !kase.recipe.reading? && created.nil?

      directory = File.join(cases_directory, kase.recipe.id)
      refusal = CaseDirectory.prepare(directory, recipe: kase.recipe, image:)
      if refusal
        FileUtils.rm_r(directory, secure: true)
        return CaseResult.new(id: kase.recipe.id, verdict: "host_skip", stages: [], detail: refusal)
      end

      judged = run_stages(kase, model:, runner:, directory:, sandbox:, stages:, created:)
      failure = judged.lazy.filter_map { failure(_1) }.first
      FileUtils.rm_r(directory, secure: true) unless failure
      CaseResult.new(id: kase.recipe.id, verdict: failure ? "failed" : "passed", stages: judged.map(&:result), detail: failure)
    end

    # Each stage's Judged, up to the stage that ends the case. Once create
    # has accepted, the case and model that judge the later stages are the
    # created image's.
    private_class_method def self.run_stages(kase, model:, runner:, directory:, sandbox:, stages:, created:)
      judged = []
      stages.each do |stage|
        result = runner.run(stage, recipe: kase.recipe, case_directory: directory, sandbox:)
        if result.stage == "create" && !ends_case?(result)
          image_model, classification = created.call(File.join(directory, Runner::CREATED))
          image_case = kase.created(classification)
          judged << Judged.new(result:, kase:, model:, image_problem: image_case.created_image_problem)
          kase = image_case
          model = image_model
        else
          judged << Judged.new(result:, kase:, model:, image_problem: nil)
        end
        break if ends_case?(result) || judged.last.image_problem
      end
      judged
    end

    # A stage that failed a safety check ends the case, and so does a
    # create that did not accept: the reading stages have no image.
    private_class_method def self.ends_case?(result) =
      !result.safe? || (result.stage == "create" && result.outcome.kind != "accepted")

    # Why the judged stage fails the case, or nil.
    private_class_method def self.failure(judged)
      result = judged.result
      expected = judged.kase.expected.fetch(result.stage)
      if !result.safe?
        "#{result.stage}: #{result.problems.join("; ")}"
      elsif !expected.satisfied_by?(result.outcome)
        "#{result.stage}: expected #{expected}, got #{result.outcome}#{" (#{result.outcome.detail})" if result.outcome.detail}"
      elsif judged.image_problem
        "#{result.stage}: #{judged.image_problem}"
      elsif result.outcome.kind == "accepted" && !expected.unclassified
        judged.model.record_problems(result).then { _1.empty? ? nil : "#{result.stage}: #{_1.join("; ")}" }
      end
    end
  end
end
