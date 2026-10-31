# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"
require_relative "synthetic"

# Name trials put each sibling set of a tree through the name funnel in a
# directory of its own under trials/, and the destination rules read their
# answers (specs/design-spec-testing.md#host-names). What a host answers
# differs between hosts, so each expectation follows from a probe of this
# host with the plain file API.
class TestNameTrials < Minitest::Test
  NFC = "\u00e9"
  NFD = "e\u0301"
  RULES = [FsimgSpec::DestinationRules::COLLISION, FsimgSpec::DestinationRules::UNREPRESENTABLE].freeze

  # A funnel that counts the names it is asked to create.
  class CountingFunnel
    attr_reader :created

    def initialize
      @created = 0
    end

    def create_file(directory, name)
      @created += 1
      FsimgSpec::NameFunnel.create_file(directory, name)
    end

    def children(directory) = FsimgSpec::NameFunnel.children(directory)
  end

  # A host that stores a name without its trailing dot, as msdosfs does.
  module DotDroppingFunnel
    def self.create_file(directory, name) = FsimgSpec::NameFunnel.create_file(directory, name.b.chomp("."))

    def self.children(directory) = FsimgSpec::NameFunnel.children(directory)
  end

  # A funnel that takes EEXIST for success.
  module ExistsBlindFunnel
    def self.create_file(directory, name) = FsimgSpec::NameFunnel.create_file(directory, name).then { _1 == :exists ? :created : _1 }

    def self.children(directory) = FsimgSpec::NameFunnel.children(directory)
  end

  # A host whose directory refuses every new name.
  module RefusingFunnel
    def self.create_file(_directory, _name) = raise(Errno::EACCES, "refused")
  end

  # e-acute in NFD sorts before NFC by its bytes, so the trial creates it
  # first, and a host that folds normalization refuses the NFC name. The
  # collision rule follows the answer; the trial leaves trials/ empty.
  def test_colliding_siblings_violate_the_collision_rule_where_the_host_folds
    with_trials do |name_trials, directory|
      answers = name_trials.call(tree(["d", "directory"], ["d/#{NFC}", "file"], ["d/#{NFD}", "file"], ["d/x", "file"]))

      expected = host_folds?(NFD, NFC) ? [FsimgSpec::Refusal.new(path: "d/#{NFC}".b, answer: :exists)] : []
      assert_equal [expected, nil], [answers.refused, answers.unheld]
      assert_equal(expected.empty? ? [] : ["host.name.collision"], violated(answers))
      assert_empty Dir.children(directory)
    end
  end

  # A name of 128 e-acutes is 256 bytes of UTF-8: a host that counts bytes
  # refuses it as too long, which the unrepresentable rule reports, as it
  # does a name the host takes for an illegal byte sequence.
  def test_a_name_beyond_the_hosts_limit_violates_the_unrepresentable_rule
    name = NFC * 128
    with_trials do |name_trials, _directory|
      answers = name_trials.call(tree([name, "file"]))

      answer = host_answer(name)
      if answer == :created
        assert_empty answers.refused
      else
        assert_equal [FsimgSpec::Refusal.new(path: name.b, answer:)], answers.refused
        assert_equal ["host.name.unrepresentable"], violated(answers)
      end
    end
  end

  # Answers are cached by the sibling set's bytes: a set seen before, in
  # any order and under any parent, costs no trial.
  def test_a_repeated_sibling_set_costs_no_trial
    funnel = CountingFunnel.new
    with_trials(funnel:) do |name_trials, _directory|
      name_trials.call(tree(["b", "file"], ["a", "file"]))
      name_trials.call(tree(["d", "directory"], ["d/a", "file"], ["d/b", "file"]))

      assert_equal 3, funnel.created
    end
  end

  # A host that stores another name than the one created cannot hold the
  # tree, which makes the case a host skip.
  def test_a_host_that_stores_another_name_cannot_hold_the_tree
    with_trials(funnel: DotDroppingFunnel) do |name_trials, _directory|
      answers = name_trials.call(tree(["a.", "file"]))

      assert_equal "the host stored other names than it created in the root", answers.unheld
    end
  end

  # A funnel that takes EEXIST for success claims a name the listing
  # lacks, so on a host that folds the pair the trial cannot vouch for the
  # tree, and the case is a host skip rather than a false acceptance.
  def test_a_funnel_blind_to_eexist_is_caught_by_the_listing
    with_trials(funnel: ExistsBlindFunnel) do |name_trials, _directory|
      answers = name_trials.call(tree([NFC, "file"], [NFD, "file"]))

      assert_equal [[], host_folds?(NFD, NFC)], [answers.refused, !answers.unheld.nil?]
    end
  end

  # Only a case that extracts a known tree is tried; any other gets
  # NO_TRIAL, which leaves its destination scope unclassified.
  def test_only_a_case_that_extracts_a_known_tree_is_tried
    with_trials do |name_trials, directory|
      known = tree(["a", "file"])
      answers = [[known, %w[info extract]], [known, %w[info ls]], [FsimgSpec::NO_TRIAL, %w[extract]]].map do |tried, stages|
        name_trials.answers_for(tried, stages:)
      end

      assert_equal [FsimgSpec::TrialAnswers::HELD, FsimgSpec::NO_TRIAL, FsimgSpec::NO_TRIAL], answers
      assert_equal FsimgSpec::NO_TRIAL, FsimgSpec::NameTrials::NONE.answers_for(known, stages: %w[extract])
      assert_empty Dir.children(directory)
    end
  end

  # A trial that fails still removes its directory, and its error
  # propagates as the harness's.
  def test_a_failed_trial_removes_its_directory
    with_trials(funnel: RefusingFunnel) do |name_trials, directory|
      assert_raises(Errno::EACCES) { name_trials.call(tree(["a", "file"])) }
      assert_empty Dir.children(directory)
    end
  end

  # Without trial answers the destination scope is unclassified, so no
  # destination expectation stands without a trial behind it; the
  # recorded, host-independent classification leaves it out either way.
  def test_no_trial_leaves_the_destination_scope_unclassified
    registry = FsimgSpec::Registry.new(RULES)
    untried = registry.classify(Synthetic::Model.new, FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::NO_TRIAL))
    tried = registry.classify(Synthetic::Model.new, Synthetic::ENVIRONMENT)

    assert_equal [["destination"], []], [untried.unclassified, tried.unclassified]
    assert_empty untried.without_destination.unclassified
  end

  private

  def with_trials(funnel: FsimgSpec::NameFunnel)
    Dir.mktmpdir do |root|
      directory = File.join(root, "trials")
      Dir.mkdir(directory)
      yield FsimgSpec::NameTrials.new(directory, funnel:), directory
    end
  end

  # A known tree of paths and types.
  def tree(*entries) =
    FsimgSpec::KnownTree.from_walk(entries.map { |path, type| FsimgSpec::WalkedEntry.new(path: path.b, type:, size: 0, sha256: nil) })

  def violated(answers) =
    RULES.flat_map { _1.violations(nil, FsimgSpec::Environment.new(forced_format: nil, trial_answers: answers)) }.map { _1.rule.id }

  # Whether this host stores first and second as one name, probed with the
  # plain file API in a directory of its own.
  def host_folds?(first, second)
    Dir.mktmpdir do |probe|
      File.write(File.join(probe, first), "")
      File.exist?(File.join(probe, second))
    end
  end

  # What this host answers for a file name, probed with the plain file API.
  def host_answer(name)
    Dir.mktmpdir { File.write(File.join(_1, name), "") }
    :created
  rescue Errno::ENAMETOOLONG
    :too_long
  rescue Errno::EILSEQ
    :illegal
  end
end
