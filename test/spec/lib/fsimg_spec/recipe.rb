# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "set"

module FsimgSpec
  # A recipe, or a part of one, that breaks the recipe format. Recipes are
  # untrusted, so the message is cut to a length a report can carry,
  # whatever value a check quoted in it.
  class RecipeError < Error
    MESSAGE_LIMIT = 400

    def initialize(message = nil)
      super(message && message.length > MESSAGE_LIMIT ? "#{message[0, MESSAGE_LIMIT]}..." : message)
    end
  end

  # The fsimg format names the harness models.
  MODELED_FORMATS = %w[fat12 fat16 fat32].freeze

  # Parameter and setting names, and builder and tool names, which may also
  # hold dots and dashes, as in mkfs.fat. Neither kind can be empty, name
  # . or .., or start an option.
  PARAMETER_NAME = /\A[a-z][a-z0-9_]{0,63}\z/
  TOOL_NAME = /\A[a-z][a-z0-9._-]{0,63}\z/
  MAX_NAMES = 256
  MAX_SEED = (1 << 64) - 1

  # Every recipe value checks what it can know alone when it is built, so
  # a recipe a generator builds in code obeys the same format as one the
  # loader reads. Only the profile's ceilings are left to the loader.
  module RecipeValues
    # map, a Hash of names to nonnegative integers, with its keys in sorted
    # order so that it dumps canonically.
    def self.named_integers(map, what:)
      check_names(map, what:, pattern: PARAMETER_NAME)
      bad = map.find { |_, value| !(value.is_a?(Integer) && value.between?(0, MAX_SEED)) }
      raise RecipeError, "#{what} #{bad.first} #{FsimgSpec.shown(bad.last)} is not a nonnegative integer" if bad

      map.sort.to_h.freeze
    end

    def self.check_names(map, what:, pattern:)
      raise RecipeError, "#{what}s are not a map" unless map.is_a?(Hash)
      raise RecipeError, "more than #{MAX_NAMES} #{what}s" if map.size > MAX_NAMES

      bad = map.keys.find { !(_1.is_a?(Symbol) && pattern.match?(_1.to_s)) }
      raise RecipeError, "malformed #{what} name #{FsimgSpec.shown(bad.to_s)}" if bad
    end

    def self.text?(value) = value.is_a?(String) && !value.empty?
  end

  # One entry of a file tree: a relative path that never names . or .. and
  # has no empty component or NUL, a type, and for a file the size and seed
  # its bytes come from. A directory has neither.
  class TreeEntry < Data.define(:path, :type, :size, :seed)
    include Checked

    TYPES = %w[file directory].freeze

    def initialize(path:, type:, size:, seed:)
      problem = TreeEntry.path_problem(path)
      raise RecipeError, "tree path #{FsimgSpec.shown(path)} #{problem}" if problem
      raise RecipeError, "#{path}: unknown type #{FsimgSpec.shown(type)}" unless TYPES.include?(type)
      raise RecipeError, "#{path}: size or seed is not an integer in 0..#{MAX_SEED}" unless
        [size, seed].all? { _1.is_a?(Integer) && _1.between?(0, MAX_SEED) }
      raise RecipeError, "#{path}: a directory has no size or seed" if type == "directory" && (size + seed).positive?

      super
    end

    def depth = path.count("/") + 1

    # The path of the directory holding this entry, or nil at the top.
    def parent = path.rpartition("/").first.then { _1.empty? ? nil : _1 }

    def self.path_problem(path)
      return "is not UTF-8 text" unless path.is_a?(String) && path.encoding == Encoding::UTF_8 && path.valid_encoding?

      if path.empty? then "is empty"
      elsif path.include?("\0") then "contains NUL"
      elsif path.start_with?("/") then "is absolute"
      elsif path.split("/", -1).any? { ["", ".", ".."].include?(_1) } then "has an empty, . or .. component"
      end
    end

    # entries, once no path repeats and each lies in a directory entry of
    # the same tree.
    def self.tree(entries)
      raise RecipeError, "a tree is not a list of entries" unless entries.is_a?(Array) && entries.all?(TreeEntry)
      raise RecipeError, "a tree path repeats" if entries.map(&:path).uniq.size != entries.size

      directories = entries.select { _1.type == "directory" }.to_set(&:path)
      orphan = entries.find { _1.parent && !directories.include?(_1.parent) }
      raise RecipeError, "the parent of tree path #{FsimgSpec.shown(orphan.path)} is no directory of the tree" if orphan

      entries.dup.freeze
    end
  end

  # How a reading case's base image is built: a builder, its integer
  # parameters and the tree it populates.
  class Base < Data.define(:builder, :parameters, :tree)
    include Checked

    def initialize(builder:, parameters:, tree:)
      raise RecipeError, "malformed builder name #{FsimgSpec.shown(builder)}" unless
        builder.is_a?(String) && TOOL_NAME.match?(builder)

      super(builder:, parameters: RecipeValues.named_integers(parameters, what: "parameter"), tree: TreeEntry.tree(tree))
    end
  end

  # A creation case's input: the tree fsimg create reads and its settings.
  class Source < Data.define(:tree, :settings)
    include Checked

    def initialize(tree:, settings:) =
      super(tree: TreeEntry.tree(tree), settings: RecipeValues.named_integers(settings, what: "setting"))
  end

  # A violation as a recipe records it: a rule id and a scope.
  class ViolationRecord < Data.define(:rule, :scope)
    include Checked

    def initialize(rule:, scope:)
      raise RecipeError, "malformed rule id #{FsimgSpec.shown(rule)}" unless rule.is_a?(String) && Rule::ID_PATTERN.match?(rule)
      raise RecipeError, "unknown scope #{FsimgSpec.shown(scope)}" unless SCOPE_STAGES.key?(scope)

      super
    end

    def to_s = "#{rule}@#{scope}"
  end

  # The checks a Recipe makes when it is built, as functions of its fields;
  # each answers the first problem it finds, or nil.
  module RecipeChecks
    READING_ONLY = %i[base_sha256 tool_versions mutations forced_format].freeze

    # The id, format, case class and its intended violation, and the reason
    # a promoted case needs.
    def self.identity_problem(fields)
      fields => { id:, format:, kind:, intended_violation:, isolation:, reason: }
      return "id #{FsimgSpec.shown(id)} is not a string of [a-z0-9._-] up to 128 bytes, starting with neither . nor -" unless
        id.is_a?(String) && Recipe::ID_PATTERN.match?(id)
      return "unknown format #{FsimgSpec.shown(format)}" unless MODELED_FORMATS.include?(format)
      return "unknown kind #{FsimgSpec.shown(kind)}" unless Recipe::KINDS.include?(kind)
      return "a targeted case names its intended violation" if kind == "targeted" && intended_violation.nil?
      return "only a targeted case names an intended violation" if kind != "targeted" && !intended_violation.nil?
      return "malformed rule id #{FsimgSpec.shown(intended_violation)}" unless
        intended_violation.nil? || (intended_violation.is_a?(String) && Rule::ID_PATTERN.match?(intended_violation))
      return "unknown isolation #{FsimgSpec.shown(isolation)}" unless isolation.nil? || Recipe::ISOLATIONS.include?(isolation)
      return "reason is not a non-empty string" unless reason.nil? || RecipeValues.text?(reason)

      "promoted case #{id} needs a reason" if id.start_with?(Recipe::PROMOTED_PREFIX) && reason.nil?
    end

    # What the case reads or creates, and the stages that run on it.
    def self.input_problem(fields)
      fields => { base:, source:, stages: }
      return "a recipe has a base or a source, not both" if base.nil? == source.nil?
      return "stages are not a list of stage names" unless stages.is_a?(Array) && stages.all? { STAGES.include?(_1) }
      return "stages are empty or repeat" if stages.empty? || stages.uniq.size != stages.size
      return "a reading case cannot run create" if base && stages.include?("create")
      return "a creation case needs create" if source && !stages.include?("create")
      return "stages are not in the order #{RUN_ORDER.join(", ")}" unless stages == stages.sort_by { RUN_ORDER.index(_1) }

      base ? reading_problem(fields) : creation_problem(fields)
    end

    def self.reading_problem(fields)
      fields => { base:, base_sha256:, tool_versions:, mutations:, forced_format: }
      return "base is not a Base" unless base.is_a?(Base)
      return "base_sha256 is not 64 lowercase hexadecimal digits" unless base_sha256.is_a?(String) && Recipe::SHA256.match?(base_sha256)
      return "tool versions are not strings" unless tool_versions.is_a?(Hash) && tool_versions.values.all? { RecipeValues.text?(_1) }
      return "mutations are not a list of mutations" unless mutations.is_a?(Array) && mutations.all? { _1.respond_to?(:apply) }

      RecipeValues.check_names(tool_versions, what: "tool", pattern: TOOL_NAME)
      "unknown format #{FsimgSpec.shown(forced_format)}" unless forced_format.nil? || MODELED_FORMATS.include?(forced_format)
    end

    def self.creation_problem(fields)
      return "source is not a Source" unless fields[:source].is_a?(Source)

      present = READING_ONLY.reject { fields[_1].nil? }
      "a creation case takes no #{present.join(", ")}" unless present.empty?
    end

    # The recorded classification and the optional fields.
    def self.record_problem(fields)
      fields => { stages:, expected:, actual_violations:, random_seed:, source_revision:, resource_limits: }
      return "expected must name exactly the stages" unless expected.is_a?(Hash) && expected.keys.sort == stages.sort
      return "expected holds something other than expectations" unless expected.values.all?(Expectation)
      return "actual violations are not violation records" unless
        actual_violations.is_a?(Array) && actual_violations.all?(ViolationRecord)
      return "random_seed is not an integer in 0..#{MAX_SEED}" unless
        random_seed.nil? || (random_seed.is_a?(Integer) && random_seed.between?(0, MAX_SEED))
      return "source_revision is not a non-empty string" unless source_revision.nil? || RecipeValues.text?(source_revision)

      "resource_limits name an unknown resource or a negative limit" unless resource_limits.nil? || limits?(resource_limits)
    end

    def self.limits?(map) =
      map.is_a?(Hash) && map.all? { |key, value| LIMIT_KEYS.include?(key.to_s) && value.is_a?(Integer) && !value.negative? }

    # The fields whose canonical form differs from what the caller passed:
    # sorted maps and violation records, frozen lists.
    def self.canonical(fields) = {
      stages: fields[:stages].dup.freeze,
      expected: fields[:expected].dup.freeze,
      tool_versions: fields[:tool_versions]&.sort&.to_h&.freeze,
      mutations: fields[:mutations]&.dup&.freeze,
      resource_limits: fields[:resource_limits]&.sort&.to_h&.freeze,
      actual_violations: fields[:actual_violations].uniq.sort_by { [_1.rule, _1.scope] }.freeze
    }
  end
  private_constant :RecipeChecks

  # A case serialized as versioned JSON (specs/design-spec-testing.md#recipes).
  # A reading case has base, base_sha256, tool_versions and mutations; a
  # creation case has source instead. kind is the case class: a valid case,
  # a targeted case, which alone names an intended_violation, or a general
  # malformed one. actual_violations, isolation and expected record the
  # classification and derivation made when the recipe was written; replay
  # re-derives and compares them.
  class Recipe < Data.define(:id, :format, :base, :base_sha256, :tool_versions, :mutations, :forced_format,
                             :source, :stages, :kind, :intended_violation, :actual_violations, :isolation,
                             :expected, :random_seed, :source_revision, :resource_limits, :reason)
    include Checked

    SCHEMA_VERSION = 1
    ID_PATTERN = /\A[a-z0-9_][a-z0-9._-]{0,127}\z/
    PROMOTED_PREFIX = "regression."
    KINDS = %w[valid targeted malformed].freeze
    ISOLATIONS = %w[single_rule multi_rule].freeze
    SHA256 = /\A[0-9a-f]{64}\z/

    def initialize(id:, format:, stages:, kind:, expected:, base: nil, base_sha256: nil, tool_versions: nil,
                   mutations: nil, forced_format: nil, source: nil, intended_violation: nil,
                   actual_violations: [], isolation: nil, random_seed: nil, source_revision: nil,
                   resource_limits: nil, reason: nil)
      fields = { id:, format:, stages:, kind:, expected:, base:, base_sha256:, tool_versions:, mutations:,
                 forced_format:, source:, intended_violation:, actual_violations:, isolation:, random_seed:,
                 source_revision:, resource_limits:, reason: }
      problem = RecipeChecks.identity_problem(fields) || RecipeChecks.input_problem(fields) || RecipeChecks.record_problem(fields)
      raise RecipeError, problem if problem

      super(**fields, **RecipeChecks.canonical(fields))
    end

    def reading? = !base.nil?

    def promoted? = id.start_with?(PROMOTED_PREFIX)

    # Canonical JSON: fixed field order, sorted map keys except expected,
    # which follows stages, optional fields only when present.
    def dump = "#{CanonicalJson.generate(to_json_fields)}\n"

    def self.load(text, ceilings:) = RecipeLoader.new(ceilings).load(text)

    # The recipe in the file at path, which is untrusted: it must be a
    # regular file, and no more than the loader accepts is read from it.
    # The open does not block, so a FIFO is refused rather than waited on.
    def self.read(path, ceilings:)
      text = File.open(path, File::RDONLY | File::NONBLOCK | File::BINARY) do |file|
        raise RecipeError, "#{FsimgSpec.shown(path)} is no regular file" unless file.stat.file?

        file.read(RecipeLoader::TEXT_BYTES + 1) || ""
      end
      load(text, ceilings:)
    rescue SystemCallError => e
      raise RecipeError, "cannot read #{FsimgSpec.shown(path)}: #{e.message}"
    end

    private

    def to_json_fields
      fields = { schema_version: SCHEMA_VERSION, id:, format: }
      fields.merge!(reading? ? reading_fields : { source: source_fields })
      fields.merge!(stages:, kind:, intended_violation:, actual_violations: actual_violations.map(&:to_h),
                    isolation:, expected: stages.to_h { [_1, expected.fetch(_1).to_s] })
      fields.merge!({ random_seed:, source_revision:, resource_limits:, reason: }.compact)
    end

    def reading_fields = {
      base: { builder: base.builder, tree: base.tree.map(&:to_h), parameters: base.parameters },
      base_sha256:, tool_versions:, mutations: mutations.map(&:to_h), forced_format:
    }.compact

    def source_fields = { tree: source.tree.map(&:to_h), settings: source.settings }
  end
end
