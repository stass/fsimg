# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # A recipe with the classifications of what its stages read and what
  # each of its stages must do, derived from the rules
  # (specs/design-spec-testing.md#data-flow). No case carries a
  # hand-written expectation.
  #
  # classification is the image's: a reading case's image, or the image a
  # creation case's create writes. Until create has written it, that is
  # CREATED_IMAGE, the classification every created image must have, and
  # #created replaces it with the classification of the image itself.
  # source_classification is a creation case's SourceModel classified by
  # the input rules, which create follows, or nil for a reading case.
  #
  # intended_rule is the Rule the recipe names as its intended violation,
  # or nil for a case meant valid.
  class Case < Data.define(:recipe, :classification, :source_classification, :intended_rule, :expected)
    include Checked

    # What an image fsimg created must classify to: no violation, and no
    # scope left unclassified, since the harness knows the tree it holds
    # (specs/design-spec-testing.md#cases). It is a template: each creation
    # case sets the trial_answers of its own tree's name trials, which say
    # whether the host holds its names at the destination.
    CREATED_IMAGE = Classification.new(violations: [].freeze, unclassified: [].freeze, trial_answers: TrialAnswers::HELD)

    def initialize(recipe:, classification:, intended_rule:, expected:, source_classification: nil)
      raise Error, "#{recipe.id}: only a creation case has, and must have, a source classification" if
        recipe.reading? != source_classification.nil?

      super
    end

    def self.derive(recipe, classification, registry:, source_classification: nil) = new(
      recipe:, classification:, source_classification:,
      intended_rule: recipe.intended_violation && registry.fetch(recipe.intended_violation),
      expected: expectations(recipe, image: classification, source: source_classification)
    )

    # What each of recipe's stages must do: create under source, the
    # source model's classification, and every reading stage under image.
    # A general malformed case asserts only safety, because the model may
    # not see everything a random edit breaks.
    def self.expectations(recipe, image:, source: nil)
      return recipe.stages.to_h { [_1, Expectation.new(unclassified: true)] } if recipe.kind == "malformed"

      recipe.stages.to_h { [_1, Expectation.derive(_1 == "create" ? source : image, _1)] }
    end

    # The case once create has accepted, with image_classification, the
    # classification of the image create wrote: create keeps the source
    # model's expectation, and the reading stages follow the image.
    def created(image_classification) =
      with(classification: image_classification,
           expected: Case.expectations(recipe, image: image_classification, source: source_classification))

    # Why the image create wrote fails create, or nil: an accepting create
    # must write an image that violates no rule and that the model decodes
    # completely, as CREATED_IMAGE states
    # (specs/design-spec-testing.md#expectation-derivation). A scope left
    # unclassified would otherwise pass every stage that reads it
    # unjudged. Destination violations and scopes depend on the host, not
    # on the image.
    def created_image_problem
      image = classification.without_destination
      found = [("violates #{image.rule_ids.join(", ")}" if image.rule_ids.any?),
               ("leaves #{image.unclassified.join(", ")} unclassified" if image.unclassified.any?)].compact
      "the image it wrote #{found.join(" and ")}" if found.any?
    end

    # Why the host cannot hold the names the case extracts, or nil: its
    # name trials found the host storing other names than it created
    # (specs/design-spec-testing.md#host-names). Such a case is a host
    # skip, and fsimg does not run.
    def host_skip = FsimgSpec.undefined?(classification.trial_answers) ? nil : classification.trial_answers.unheld

    # The recipe as written for this case: the derived violations,
    # isolation and expectations replace whatever the recipe recorded.
    # Destination violations depend on the host's name trials, so the
    # recorded fields leave them out (specs/design-spec-testing.md#host-names).
    def recorded =
      recipe.with(actual_violations: recorded_violations, isolation:,
                  expected: Case.expectations(recipe, image: recorded_image, source: source_classification))

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
    # rule. An unmutated base's case must also leave no scope but the
    # destination unclassified, since its tree is known, so a scope the
    # model cannot decode never passes its stages unjudged; a mutated
    # valid case may, while the model does not decode the allocation
    # tables and directory tree (docs/spec-testing-plan.md, B2).
    #
    # A targeted case must name a rule with violations, run a stage that
    # checks it, and violate it. A violation the case alone cannot show is
    # exempt from that last check: one of a destination rule depends on
    # the host's name trials, and one in a scope the model could not
    # decode is never found. A malformed case may violate anything. The
    # image a creation case's create writes is fsimg's output, which
    # create's own judgement covers (#created_image_problem).
    def harness_defect
      case recipe.kind
      in "valid" if rule_ids.any?
        "#{recipe.id} is meant valid but violates #{rule_ids.join(", ")}"
      in "valid" if recipe.reading? && recipe.mutations.empty? && undecoded.any?
        "#{recipe.id} is meant valid but leaves #{undecoded.join(", ")} unclassified"
      in "targeted" if intended_rule.policy == "equivalent_contents"
        "#{recipe.id} targets #{intended_rule.id}, a freedom of the format with no violations"
      in "targeted" if (intended_rule.stages & recipe.stages).empty?
        "#{recipe.id} runs no stage that checks its intended rule #{intended_rule.id}"
      in "targeted" if decides_intended_rule? && !rule_ids.include?(intended_rule.id)
        "#{recipe.id} does not violate its intended rule #{intended_rule.id}"
      else nil
      end
    end

    # For a targeted case, single_rule when the case violates no rule but
    # the intended one and multi_rule when it violates others too; nil for
    # any other case. Destination violations never count, so the answer is
    # the same on every host.
    def isolation
      return nil unless recipe.kind == "targeted"

      (rule_ids - [intended_rule.id]).empty? ? "single_rule" : "multi_rule"
    end

    private

    # The image classification the recipe stands for: a reading case's
    # image without its destination violations, and for a creation case
    # CREATED_IMAGE, whatever image a run wrote, since create judges that
    # image and the recipe never records it.
    def recorded_image = recipe.reading? ? classification.without_destination : CREATED_IMAGE

    def violations = [*source_classification&.violations, *recorded_image.violations]

    def rule_ids = violations.map { _1.rule.id }.uniq.sort

    # The scopes besides the host's destination that the model could not
    # decode on the case image.
    def undecoded = recorded_image.unclassified

    # Whether the case alone decides a scope of the intended rule: one that
    # is neither the host's destination nor left unclassified.
    def decides_intended_rule?
      undecided = ["destination", *recorded_image.unclassified, *source_classification&.unclassified]
      (intended_rule.scopes - undecided).any?
    end

    def recorded_violations =
      violations.map { ViolationRecord.new(rule: _1.rule.id, scope: _1.scope) }.uniq.sort_by { [_1.rule, _1.scope] }

    def describe(value)
      case value
      in Hash then value.map { "#{_1}=#{_2}" }.join(" ")
      in Array then value.join(" ")
      else value.inspect
      end
    end
  end
end
