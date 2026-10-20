# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # A recipe with the classification of its image and what each of its
  # stages must do, derived from the rules (specs/design-spec-testing.md#data-flow).
  # No case carries a hand-written expectation.
  #
  # intended_rule is the Rule the recipe names as its intended violation,
  # or nil for a case meant valid.
  class Case < Data.define(:recipe, :classification, :intended_rule, :expected)
    def self.derive(recipe, classification, registry:) = new(
      recipe:, classification:,
      intended_rule: recipe.intended_violation && registry.fetch(recipe.intended_violation),
      expected: expectations(recipe, classification)
    )

    # What each of recipe's stages must do under classification. A general
    # malformed case asserts only safety, because the model may not see
    # everything a random edit breaks.
    def self.expectations(recipe, classification)
      return recipe.stages.to_h { [_1, Expectation.new(outcomes: [Expectation::UNCLASSIFIED])] } if recipe.kind == "malformed"

      recipe.stages.to_h { [_1, Expectation.derive(classification, _1)] }
    end

    # The case once create has accepted: create keeps the expectation the
    # source tree's classification derived, and the reading stages follow
    # image_classification, the classification of the image create wrote.
    def created(image_classification) =
      with(expected: expected.merge(Case.expectations(recipe, image_classification).except("create")))

    # The recipe as written for this case: the derived violations,
    # isolation and expectations replace whatever the recipe recorded.
    # Destination violations depend on the host's name trials, so the
    # recorded fields leave them out (specs/design-spec-testing.md#host-names).
    # A creation case's reading stages follow the image create wrote,
    # which this case's classification does not describe, so a creation
    # case is refused.
    def recorded
      raise Error, "#{recipe.id}: a creation case is not recorded from its source classification" if recipe.source

      recipe.with(actual_violations: recorded_violations, isolation:,
                  expected: Case.expectations(recipe, classification.without_destination))
    end

    # How the recipe's recorded classification differs from the one the
    # current rules derive, as messages; a promoted case must have none.
    def drift
      derived = recorded
      %i[actual_violations isolation expected].filter_map do |field|
        ours = recipe.public_send(field)
        theirs = derived.public_send(field)
        "#{field}: recorded #{describe(ours)}, derived #{describe(theirs)}" if ours != theirs
      end
    end

    # A defect of the generator or builder that made this case, or nil
    # (specs/design-spec-testing.md#cases). A valid case must violate no
    # rule and leave no scope unclassified, since the model's bounds exceed
    # the run's limits. A targeted case must name a rule with violations,
    # run a stage that checks it, and violate it. A violation the image
    # alone cannot show is exempt from that last check: one of a
    # destination rule depends on the host's name trials, and one in a
    # scope the model could not decode is never found. A malformed case
    # may violate anything.
    def harness_defect
      violated = classification.without_destination.rule_ids
      case recipe.kind
      in "valid" if violated.any?
        "#{recipe.id} is meant valid but violates #{violated.join(", ")}"
      in "valid" if classification.unclassified.any?
        "#{recipe.id} is meant valid but leaves #{classification.unclassified.join(", ")} unclassified"
      in "targeted" if intended_rule.policy == "equivalent_contents"
        "#{recipe.id} targets #{intended_rule.id}, a freedom of the format with no violations"
      in "targeted" if (intended_rule.stages & recipe.stages).empty?
        "#{recipe.id} runs no stage that checks its intended rule #{intended_rule.id}"
      in "targeted" if image_checks_intended_rule? && !violated.include?(intended_rule.id)
        "#{recipe.id} does not violate its intended rule #{intended_rule.id}"
      else nil
      end
    end

    # For a targeted case, single_rule when the image violates no rule but
    # the intended one and multi_rule when it violates others too; nil for
    # any other case. Destination violations never count, so the answer is
    # the same on every host.
    def isolation
      return nil unless recipe.kind == "targeted"

      (classification.without_destination.rule_ids - [intended_rule.id]).empty? ? "single_rule" : "multi_rule"
    end

    private

    def image_checks_intended_rule? =
      (intended_rule.scopes - ["destination"] - classification.unclassified).any?

    def recorded_violations
      classification.without_destination.violations
                    .map { ViolationRecord.new(rule: _1.rule.id, scope: _1.scope) }
                    .uniq.sort_by { [_1.rule, _1.scope] }
    end

    def describe(value)
      case value
      in Hash then value.map { "#{_1}=#{_2}" }.join(" ")
      in Array then value.join(" ")
      else value.inspect
      end
    end
  end
end
