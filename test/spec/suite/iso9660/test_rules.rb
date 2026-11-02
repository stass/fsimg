# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "tmpdir"
require "fsimg_spec/iso9660"

class Iso9660RulesTest < Minitest::Test
  Iso = FsimgSpec::Iso9660

  def test_every_rule_has_a_sound_positive_and_negative_witness
    with_cases do |cases, registry|
      registry.rules.each do |rule|
        witnesses = rule.domain.call
        assert witnesses.any? { _1.intended.nil? }, "#{rule.id} has no valid witness"
        assert witnesses.any? { _1.intended == rule.id }, "#{rule.id} has no invalid witness"
        witnesses.each do |witness|
          classified = rule.input? ? cases.create(witness) : cases.call(witness)
          classification = rule.input? ? classified.kase.source_classification : classified.kase.classification
          assert_nil classified.defect, "#{witness.id}: #{classified.defect}"
          if witness.intended
            assert_includes classification.rule_ids, rule.id, witness.id
            refute classified.kase.expected.values.all?(&:unclassified), witness.id
          else
            assert_empty classification.without_destination.violations, witness.id
          end
        end
      end
    end
  end

  def test_child_susp_violations_run_directory_stage
    with_cases do |cases, registry|
      witness = registry.fetch("iso9660.rr.version").domain.call.find(&:intended)
      classified = cases.call(witness)
      assert_equal ["ls"], classified.kase.recipe.stages
      refute classified.kase.expected.fetch("ls").accept
      assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], classified.kase.expected.fetch("ls").statuses
    end
  end

  def test_rule_sources_name_existing_format_anchors
    source = File.read(File.expand_path("../../../../specs/design-iso9660.md", __dir__))
    counts = Hash.new(0)
    anchors = source.lines.filter_map { _1[/\A\#{2,3} (.+)/, 1] }.map do |heading|
      base = heading.downcase.gsub(/[^a-z0-9 -]/, "").tr(" ", "-")
      count = counts[base]
      counts[base] += 1
      count.zero? ? base : "#{base}-#{count}"
    end
    Iso::Rules.registry.rules.each do |rule|
      assert_includes anchors, rule.source.split("#", 2).last, rule.id
    end
  end

  def test_seeded_profiles_repeat_their_feature_parameters
    first = Iso::Profiles.draw(seed: 7, count: 20)
    second = Iso::Profiles.draw(seed: 7, count: 20)
    assert_equal first, second
    assert_equal 20, first.map(&:id).uniq.size
    assert first.all? { Iso::Builder.options(_1.parameters).frozen? }
  end

  def test_preparation_scope_witnesses_classify_without_generator_defects
    with_cases do |cases, _registry|
      Iso::Witnesses.scope_regressions.each do |witness|
        result = cases.call(witness)
        assert_nil result.defect, witness.id
        if witness.intended
          assert_equal ["ls"], result.kase.recipe.stages, witness.id
          assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], result.kase.expected.fetch("ls").statuses, witness.id
        else
          assert_empty result.kase.classification.without_destination.violations, witness.id
        end
      end
    end
  end

  private

  def with_cases
    Dir.mktmpdir("iso-rules-") do |directory|
      Dir.mkdir(File.join(directory, "trials"), 0o700)
      registry = Iso::Rules.registry
      cases = Iso::Cases.new(registry:, cache: FsimgSpec::FixtureCache.new(File.join(directory, "cache")),
                             ceilings: FsimgSpec.fetch_profile("quick").ceilings,
                             name_trials: FsimgSpec::NameTrials.new(File.join(directory, "trials")), shell: nil)
      yield cases, registry
    end
  end
end
