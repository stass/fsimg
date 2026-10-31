# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fileutils"

module FsimgSpec
  # The name trials of one run (specs/design-spec-testing.md#host-names):
  # before an extract case is classified, each sibling set of its tree is
  # created through the name funnel as empty files in a fresh flat
  # directory under the results directory's trials/, and listed back. No
  # trial path nests, so neither path traversal nor path length can arise.
  # The host's answers are cached for the run by the set's bytes, so a set
  # that repeats costs no second trial.
  #
  # A trial directory is removed when its trial ends, normally or by an
  # exception, a signal's included, so trials/ holds nothing between
  # trials. What a signal between the steps of a trial or a crash leaves
  # behind goes with the results directory, which the next run empties.
  # A run classifies its cases one at a time, so one instance serves it
  # without locking; parallel classification would need a lock around
  # the cache and the trial count.
  class NameTrials
    # One sibling set's answers: the names the host refused, each mapped
    # to the funnel's answer, and whether the listing showed exactly the
    # names created.
    SetAnswers = Data.define(:refused_names, :held)

    # The trials of a caller that extracts nothing, such as the fuzz seed
    # export: none runs, and every case is classified with NO_TRIAL.
    NONE = Module.new do
      def self.answers_for(_tree, stages:) = NO_TRIAL
    end

    # directory is the run's trials/, which must exist. funnel creates
    # and lists the names; a test may pass one that stands for another
    # host's behaviour.
    def initialize(directory, funnel: NameFunnel)
      @directory = directory
      @funnel = funnel
      @cache = {}
      @trial_count = 0
    end

    # The answers that classify a case running stages over tree, a known
    # tree, a recipe tree or Undefined, of which only the paths are read:
    # the trials of its sibling sets when the case extracts a known tree,
    # and NO_TRIAL for any other case, which creates no host names or
    # whose names are unknown. Every format's cases ask here, so the rule
    # of which case is tried has one owner.
    def answers_for(tree, stages:)
      return NO_TRIAL if FsimgSpec.undefined?(tree) || !stages.include?("extract")

      call(tree)
    end

    # The TrialAnswers of tree, entries with a path: every refused name
    # with its binary path, and the first directory whose names the host
    # stored otherwise, if any.
    def call(tree)
      sets = sibling_sets(tree).transform_values { set_answers(_1) }
      refused = sets.flat_map { |parent, set| refusals_under(parent, set) }
      unheld = sets.keys.find { !sets.fetch(_1).held }
      TrialAnswers.new(refused: refused.sort_by(&:path).freeze,
                       unheld: unheld && "the host stored other names than it created in #{shown_directory(unheld)}")
    end

    private

    # Each directory's path, "" for the root, with the names of the
    # entries directly in it.
    def sibling_sets(tree) =
      tree.map { _1.path.b.rpartition("/".b) }.group_by(&:first).transform_values { |parts| parts.map(&:last) }

    # The answers for the sibling names, from the cache or a new trial. The
    # key is the set's bytes in byte order, which is also the order the
    # trial creates them in, so a set answers alike whatever order its
    # tree lists it in.
    def set_answers(names)
      key = names.map(&:b).sort.freeze
      @cache[key] ||= trial(key)
    end

    # Creates names, in order, in a fresh directory, lists it back and
    # removes it. A name answered EEXIST collides with one created before
    # it; the host held the set when the listing shows exactly the names
    # created, byte for byte.
    def trial(names)
      @trial_count += 1
      directory = File.join(@directory, @trial_count.to_s)
      Dir.mkdir(directory, 0o700)
      set = begin
        answered = names.to_h { [_1, @funnel.create_file(directory, _1)] }
        created = answered.select { _2 == :created }.keys
        SetAnswers.new(refused_names: answered.reject { _2 == :created }.freeze,
                       held: @funnel.children(directory) == created.sort)
      rescue Exception
        # A cleanup that raised here would replace the exception in flight,
        # such as a signal; rm_rf raises no StandardError.
        FileUtils.rm_rf(directory, secure: true)
        raise
      end
      FileUtils.rm_r(directory, secure: true)
      set
    end

    # The refused names of the set in the directory parent, by path.
    def refusals_under(parent, set) =
      set.refused_names.map { |name, answer| Refusal.new(path: parent.empty? ? name : parent + "/".b + name, answer:) }

    # A directory as a message names it: the tree's paths are UTF-8, so
    # only a byte that is not escapes.
    def shown_directory(parent) = parent.empty? ? "the root" : FsimgSpec.shown(parent.dup.force_encoding(Encoding::UTF_8))
  end

  # The rules of the destination scope (specs/design-spec-testing.md#host-names).
  # They read the host's name-trial answers, not the image, so the same
  # image violates them on one host and not on another: an NFC/NFD pair
  # collides on APFS but not on ext4. fsimg's extractor creates every name
  # exclusively and reports a host collision as FSIMG_EEXISTS and a name the
  # host cannot represent as FSIMG_EUNSUPPORTED. A format's rule set adds
  # the domain of its own witnesses.
  module DestinationRules
    SOURCE = "specs/design-security.md, Extraction: host case-folding and normalization collisions fail through " \
             "exclusive creation, and names the host cannot represent are rejected; " \
             "specs/design-spec-testing.md#host-names"

    # Two names of one directory the host takes for one.
    COLLISION = Rule.new(
      id: "host.name.collision", category: "profile", policy: "must_reject", scopes: ["destination"],
      status: "FSIMG_EEXISTS", source: SOURCE,
      applies_to: ->(_model, _environment) { true },
      predicate: ->(_model, environment) { FsimgSpec.derive(environment.trial_answers) { !_1.refused?(%i[exists]) } }
    )

    # A name the host refuses as too long or as an illegal byte sequence.
    UNREPRESENTABLE = Rule.new(
      id: "host.name.unrepresentable", category: "profile", policy: "must_reject", scopes: ["destination"],
      status: "FSIMG_EUNSUPPORTED", source: SOURCE,
      applies_to: ->(_model, _environment) { true },
      predicate: ->(_model, environment) { FsimgSpec.derive(environment.trial_answers) { !_1.refused?(%i[too_long illegal]) } }
    )

    # The destination rules, each with the domain that generates its
    # witnesses in a format.
    def self.with_domains(collision:, unrepresentable:) =
      [COLLISION.with(domain: collision), UNREPRESENTABLE.with(domain: unrepresentable)]
  end
end
