# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"

module FsimgSpec
  # The class of one stage's result (specs/design-spec-testing.md#outcome-classes).
  # A rejection is read from the integer code of the CLI's -j error record;
  # neither the exit status nor the prose status string decides it. status
  # is the Status of a rejected or faulted stage, and records are the parsed
  # -j records, empty when they could not be read. detail says why a stage
  # was not accepted or rejected.
  class Outcome < Data.define(:kind, :status, :records, :detail)
    include Checked

    KINDS = %w[accepted rejected faulted crashed timed_out output_exceeded sanitizer_failure harness_error].freeze
    SANITIZER_EXIT = 87
    SANITIZER_MARKERS = ["ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "WARNING: MemorySanitizer",
                         "runtime error:"].freeze

    def initialize(kind:, status: nil, records: [], detail: nil)
      raise Error, "unknown outcome kind #{kind.inspect}" unless KINDS.include?(kind)

      super(kind:, status:, records: records.freeze, detail:)
    end

    # The outcome of execution at stage. sandbox is the sandbox status the
    # preflight established, or nil while the preflight itself runs.
    #
    # Every record repeats sandbox, except where the stage may have ended
    # before a worker reported its confinement: the coordinator reports
    # unknown until then. Two kinds of outcome may report unknown: a
    # create's rejection, since create may refuse a setting before any
    # worker runs, and any outcome but an acceptance or a rejection, such
    # as a fault after a worker died. Every acceptance, and every rejection at a
    # reading stage, comes from a worker and must repeat sandbox.
    #
    # When several conditions hold, the first decides, in this order: a
    # launch failure, a sanitizer report, the runner's wall clock or a
    # capture cap, a signal, then the records.
    def self.classify(execution, stage:, sandbox:)
      if execution.launch_error then new(kind: "harness_error", detail: "launch failed: #{execution.launch_error}")
      elsif sanitizer_report?(execution) then new(kind: "sanitizer_failure")
      elsif execution.ended_by == "deadline" then new(kind: "timed_out", detail: "the runner's wall clock expired")
      elsif execution.ended_by == "capture_cap" then new(kind: "output_exceeded")
      elsif execution.signal then new(kind: "crashed", detail: "signal #{execution.signal}")
      else from_records(execution, stage:, sandbox:)
      end
    end

    def passed? = %w[accepted rejected].include?(kind)

    def to_s = [kind, status].compact.join(":")

    private_class_method def self.sanitizer_report?(execution) =
      execution.exit_status == SANITIZER_EXIT || SANITIZER_MARKERS.any? { execution.stderr.include?(_1) }

    private_class_method def self.from_records(execution, stage:, sandbox:)
      records = parse(execution.stdout)
      return new(kind: "harness_error", detail: "malformed NDJSON on stdout") unless records

      outcome = case execution.exit_status
                in 0 then completion(records)
                in 1 | 2 => exit_status then rejection(records, stage:, exit_status:)
                in exit_status then new(kind: "harness_error", records:, detail: "exit status #{exit_status}")
                end
      may_be_unknown = !outcome.passed? || (stage == "create" && outcome.kind == "rejected")
      unexpected = unexpected_sandbox(records, sandbox:, may_be_unknown:)
      return outcome if unexpected.empty?

      new(kind: "harness_error", records:, detail: "sandbox #{unexpected} differs from the preflight's #{sandbox}")
    end

    # The sandbox statuses records report that the stage may not report.
    # may_be_unknown says the stage may have ended before a worker reported
    # its confinement.
    private_class_method def self.unexpected_sandbox(records, sandbox:, may_be_unknown:)
      return [] unless sandbox

      allowed = may_be_unknown ? [sandbox, "unknown"] : [sandbox]
      records.filter_map { _1["sandbox"] }.uniq - allowed
    end

    # The records of stdout, or nil unless every line is a JSON object
    # with a string type.
    private_class_method def self.parse(stdout)
      text = stdout.dup.force_encoding(Encoding::UTF_8)
      return nil unless text.valid_encoding?

      records = text.each_line(chomp: true).map { JSON.parse(_1) }
      records.all? { _1.is_a?(Hash) && _1["type"].is_a?(String) } ? records : nil
    rescue JSON::ParserError
      nil
    end

    private_class_method def self.completion(records)
      last = records.last
      return new(kind: "accepted", records:) if last && last["type"] == "summary" && last["complete"] == true

      new(kind: "harness_error", records:, detail: "exit 0 without a completion summary")
    end

    # Exit 1, or exit 2 at create, with exactly one error record, which is
    # the last record. fsimg reports its own deadline as FSIMG_ECANCELLED.
    private_class_method def self.rejection(records, stage:, exit_status:)
      errors = records.select { _1["type"] == "error" }
      unless errors.size == 1 && records.last.equal?(errors.first)
        return new(kind: "harness_error", records:, detail: "exit #{exit_status} without exactly one final error record")
      end

      status = reported_status(errors.first)
      usage_error = exit_status == 2 && !(stage == "create" && status&.name == "FSIMG_EINVAL")
      if status.nil? then new(kind: "harness_error", records:, detail: "error record #{FsimgSpec.shown(errors.first)}")
      elsif status.name == "FSIMG_ECANCELLED" then new(kind: "timed_out", status:, records:, detail: "fsimg's deadline expired")
      elsif usage_error then new(kind: "harness_error", records:, detail: "usage error #{status}")
      else new(kind: status.verdict_at?(stage) ? "rejected" : "faulted", status:, records:)
      end
    end

    # The Status an error record reports, or nil when its code is unknown
    # or an FSIMG_ELIMIT record names no known resource.
    private_class_method def self.reported_status(record)
      name = STATUS_CODES.key(record["code"])
      return nil unless name

      Status.new(name:, resource: name == "FSIMG_ELIMIT" ? record["resource"] : nil)
    rescue Error
      nil
    end
  end
end
