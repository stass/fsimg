# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # An expectation that is not well formed.
  class ExpectationError < Error; end

  # What one stage must do with a case: the outcomes it may end in. An
  # outcome is "accept" (with records that match the model),
  # "reject:STATUS" for one allowed status, or "reject:*" for any verdict
  # status of the stage. A stage that checks a scope the model could not
  # decode completely is "unclassified": only safety is asserted there.
  class Expectation < Data.define(:outcomes)
    include Checked

    UNCLASSIFIED = "unclassified"
    ANY_REJECTION = "reject:*"

    def self.parse(text)
      outcomes = text.split("|", -1)
      outcomes.each { check_outcome(_1) }
      raise ExpectationError, "unclassified excludes every other outcome in #{FsimgSpec.shown(text)}" if outcomes.include?(UNCLASSIFIED) && outcomes.size > 1

      new(outcomes:)
    end

    # Raises unless outcome is well formed; Status.parse raises for an
    # unknown status.
    private_class_method def self.check_outcome(outcome)
      case outcome
      in "accept" | UNCLASSIFIED | ANY_REJECTION then nil
      in String if outcome.start_with?("reject:") then Status.parse(outcome.delete_prefix("reject:"))
      else raise ExpectationError, "unknown outcome #{FsimgSpec.shown(outcome)}"
      end
    end

    def initialize(outcomes:) = super(outcomes: outcomes.uniq.sort.freeze)

    # The derivation of specs/design-spec-testing.md#expectation-derivation.
    # Only violations whose scope the stage checks count. A must_reject or
    # optional_feature violation demands rejection with its status, unless a
    # safety_only or diagnostic_only violation, which fsimg may meet first,
    # widens that to any verdict status. Such a violation alone allows either
    # outcome, and no violation demands acceptance.
    def self.derive(classification, stage)
      raise ExpectationError, "unknown stage #{stage.inspect}" unless STAGES.include?(stage)
      return new(outcomes: [UNCLASSIFIED]) if classification.unclassified.any? { SCOPE_STAGES.fetch(_1).include?(stage) }

      checked = classification.violations.select { SCOPE_STAGES.fetch(_1.scope).include?(stage) }
      open_ended = checked.any? { %w[safety_only diagnostic_only].include?(_1.rule.policy) }
      statuses = checked.filter_map { _1.rule.rejection_status }

      if statuses.any?
        new(outcomes: open_ended ? [ANY_REJECTION] : statuses.map { "reject:#{_1}" })
      elsif open_ended
        new(outcomes: ["accept", ANY_REJECTION])
      else
        new(outcomes: ["accept"])
      end
    end

    def unclassified? = outcomes == [UNCLASSIFIED]

    def to_s = outcomes.join("|")
  end
end
