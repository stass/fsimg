# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # An expectation that is not well formed.
  class ExpectationError < Error; end

  # What one stage must do with a case: the outcomes it may end in. accept
  # allows acceptance, with records that match the model; statuses lists
  # each Status a rejection may report; any_rejection allows any verdict
  # status of the stage. An unclassified stage checks a scope the model
  # could not decode completely, and only safety is asserted there.
  #
  # A recipe writes an expectation as its outcomes joined by "|": "accept",
  # "reject:STATUS", "reject:*" or "unclassified" alone.
  class Expectation < Data.define(:accept, :statuses, :any_rejection, :unclassified)
    include Checked

    UNCLASSIFIED = "unclassified"
    ANY_REJECTION = "reject:*"

    def initialize(accept: false, statuses: [], any_rejection: false, unclassified: false)
      raise ExpectationError, "unclassified excludes every other outcome" if
        unclassified && (accept || any_rejection || statuses.any?)
      raise ExpectationError, "an expectation allows no outcome" unless unclassified || accept || any_rejection || statuses.any?
      raise ExpectationError, "reject:* already allows every status" if any_rejection && statuses.any?

      super(accept:, statuses: statuses.uniq.sort_by(&:to_s).freeze, any_rejection:, unclassified:)
    end

    def self.parse(text)
      outcomes = text.split("|", -1)
      known = ["accept", ANY_REJECTION, UNCLASSIFIED]
      statuses = (outcomes - known).map do |outcome|
        raise ExpectationError, "unknown outcome #{FsimgSpec.shown(outcome)}" unless outcome.start_with?("reject:")

        Status.parse(outcome.delete_prefix("reject:"))
      end
      new(accept: outcomes.include?("accept"), statuses:, any_rejection: outcomes.include?(ANY_REJECTION),
          unclassified: outcomes.include?(UNCLASSIFIED))
    end

    # The derivation of specs/design-spec-testing.md#expectation-derivation.
    # Only violations whose scope the stage checks count. A must_reject or
    # optional_feature violation demands rejection with its status, unless a
    # safety_only or diagnostic_only violation, which fsimg may meet first,
    # widens that to any verdict status. Such a violation alone allows either
    # outcome, and no violation demands acceptance.
    def self.derive(classification, stage)
      raise ExpectationError, "unknown stage #{stage.inspect}" unless STAGES.include?(stage)
      return new(unclassified: true) if classification.unclassified.any? { SCOPE_STAGES.fetch(_1).include?(stage) }

      checked = classification.violations.select { SCOPE_STAGES.fetch(_1.scope).include?(stage) }
      open_ended = checked.any? { %w[safety_only diagnostic_only].include?(_1.rule.policy) }
      statuses = checked.filter_map { _1.rule.rejection_status }

      if statuses.any? then open_ended ? new(any_rejection: true) : new(statuses:)
      elsif open_ended then new(accept: true, any_rejection: true)
      else new(accept: true)
      end
    end

    # Whether outcome meets the expectation. Only an accepted or rejected
    # stage can; every other outcome fails the case under every policy.
    def satisfied_by?(outcome)
      case outcome.kind
      in "accepted" then accept || unclassified
      in "rejected" then unclassified || any_rejection || statuses.include?(outcome.status)
      else false
      end
    end


    def to_s
      outcomes = [("accept" if accept), *statuses.map { "reject:#{_1}" }, (ANY_REJECTION if any_rejection),
                  (UNCLASSIFIED if unclassified)]
      outcomes.compact.sort.join("|")
    end
  end
end
