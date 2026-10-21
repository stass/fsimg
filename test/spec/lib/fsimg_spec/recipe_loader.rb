# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"

module FsimgSpec
  # Parses one recipe. Recipes are untrusted input, so the loader works in
  # three steps:
  #
  # 1. One funnel checks the whole parsed value before any field is read:
  #    every string and object key is valid UTF-8 and at most STRING_BYTES
  #    long, and every number is an integer of at most 64 bits. Later
  #    checks therefore match patterns, split strings and quote values
  #    without meeting an encoding error or a huge number.
  # 2. The JSON shapes become recipe values, whose constructors check the
  #    recipe format, and the profile's ceilings bound every quantity.
  # 3. The text must be the recipe's canonical form, the bytes Recipe#dump
  #    writes. That refuses duplicate keys, which JSON.parse would resolve
  #    silently, and keeps a recipe's bytes, which key the fixture cache,
  #    the same however it was written.
  class RecipeLoader
    READING_KEYS = %i[base base_sha256 tool_versions mutations].freeze
    COMMON_KEYS = %i[schema_version id format stages kind intended_violation actual_violations isolation expected].freeze
    OPTIONAL_KEYS = %i[random_seed source_revision resource_limits reason].freeze
    TREE_CONTENT_KEYS = %i[size content seed holes].freeze
    TEXT_BYTES = 64 << 20
    STRING_BYTES = 1 << 20
    MAX_MUTATIONS = 4096

    def initialize(ceilings)
      @ceilings = ceilings
    end

    # text may carry any encoding tag, such as File.binread's; the loader
    # reads a UTF-8 copy and never changes the caller's string.
    def load(text)
      raise RecipeError, "a recipe of #{text.bytesize} bytes exceeds #{TEXT_BYTES}" if text.bytesize > TEXT_BYTES

      text = text.b.force_encoding(Encoding::UTF_8)

      fields = checked(parse(text))
      raise RecipeError, "a recipe is a JSON object" unless fields.is_a?(Hash)

      raise RecipeError, "the recipe has no schema_version" unless fields.key?(:schema_version)

      version = fields[:schema_version]
      raise RecipeError, "unknown schema version #{FsimgSpec.shown(version)}" unless version == Recipe::SCHEMA_VERSION

      check_keys(fields)
      recipe = build(fields, creation: fields.key?(:source))
      raise RecipeError, "the recipe is not in canonical form; Recipe#dump writes it" unless recipe.dump == text

      recipe
    end

    private

    def parse(text)
      JSON.parse(text)
    rescue JSON::ParserError, EncodingError => e
      raise RecipeError, "malformed JSON: #{FsimgSpec.shown(e.message, limit: 200)}"
    end

    # The parsed value with symbol keys, once every string, key and number
    # in it has passed the funnel.
    def checked(value)
      case value
      in Hash then value.to_h { [checked_string(_1).to_sym, checked(_2)] }
      in Array then value.map { checked(_1) }
      in String then checked_string(value)
      in Integer if value.bit_length > 64 then raise RecipeError, "an integer of #{value.bit_length} bits"
      in Float then raise RecipeError, "a number that is not an integer"
      else value
      end
    end

    def checked_string(text)
      raise RecipeError, "a string of #{text.bytesize} bytes exceeds #{STRING_BYTES}" if text.bytesize > STRING_BYTES
      raise RecipeError, "a string is not UTF-8: #{FsimgSpec.shown(text.b)}" unless text.valid_encoding?

      text
    end

    def check_keys(fields)
      raise RecipeError, "a recipe has a base or a source, not both" if fields.key?(:base) && fields.key?(:source)

      required = COMMON_KEYS + (fields.key?(:source) ? %i[source] : READING_KEYS)
      allowed = required + OPTIONAL_KEYS + (fields.key?(:source) ? [] : %i[forced_format])
      missing = required - fields.keys
      unknown = fields.keys - allowed
      raise RecipeError, "missing #{missing.join(", ")}" unless missing.empty?
      raise RecipeError, "unknown or misplaced #{unknown.first(4).map { FsimgSpec.shown(_1.to_s) }.join(", ")}" unless unknown.empty?
    end

    def build(fields, creation:)
      Recipe.new(
        id: fields[:id],
        format: fields[:format],
        **(creation ? { source: source(fields[:source]) } : reading(fields)),
        stages: fields[:stages],
        kind: fields[:kind],
        intended_violation: fields[:intended_violation],
        actual_violations: list(fields[:actual_violations], "actual_violations") { violation(_1) },
        isolation: fields[:isolation],
        expected: expected(fields[:expected]),
        random_seed: fields[:random_seed],
        source_revision: fields[:source_revision],
        resource_limits: optional(fields[:resource_limits]) { resource_limits(_1) },
        reason: fields[:reason]
      )
    end

    def reading(fields) = {
      base: base(fields[:base]),
      base_sha256: fields[:base_sha256],
      tool_versions: fields[:tool_versions],
      mutations: mutations(fields[:mutations]),
      forced_format: fields[:forced_format]
    }

    def base(value)
      case value
      in { builder:, tree: Array => tree, parameters: Hash => parameters, **rest } if rest.keys - [:population] == []
        Base.new(builder:, tree: tree(tree), parameters: within_image(parameters, "parameter"), population: rest[:population])
      else
        raise RecipeError, "base is not an object of builder, an optional population, tree and parameters"
      end
    end

    def source(value)
      case value
      in { root:, tree: Array => tree, settings: Hash => settings, **nil }
        size = settings[:size]
        # An exact volume is output fsimg writes, which the profile's
        # output ceiling bounds as it bounds the tree's content.
        raise RecipeError, "setting size #{size} exceeds #{@ceilings.output}" if size.is_a?(Integer) && size > @ceilings.output

        Source.new(root:, tree: tree(tree), settings: within_image(settings, "setting"))
      else
        raise RecipeError, "source is not an object of root, tree and settings"
      end
    end

    # A tree within the entries, depth, path and output ceilings.
    def tree(entries)
      raise RecipeError, "#{entries.size} tree entries exceed #{@ceilings.entries}" if entries.size > @ceilings.entries

      loaded = entries.map { tree_entry(_1) }
      deep = loaded.find { _1.depth > @ceilings.depth || _1.image_path_bytes > @ceilings.path }
      raise RecipeError, "tree path #{FsimgSpec.shown(deep.path)} is deeper than #{@ceilings.depth} " \
                         "or, as an image path, longer than #{@ceilings.path} bytes" if deep
      raise RecipeError, "tree content exceeds #{@ceilings.output} bytes" if loaded.sum { _1.size || 0 } > @ceilings.output

      loaded
    end

    # An entry's path and type, and the content fields TreeEntry checks.
    def tree_entry(value)
      case value
      in { path:, type:, **content } if (content.keys - TREE_CONTENT_KEYS).empty? then TreeEntry.new(path:, type:, **content)
      else raise RecipeError, "a tree entry is not an object of path, type and optionally #{TREE_CONTENT_KEYS.join(", ")}"
      end
    end

    def within_image(map, what)
      big = map.find { |_, value| value.is_a?(Integer) && value > @ceilings.image }
      raise RecipeError, "#{what} #{big.first} #{big.last} exceeds #{@ceilings.image}" if big

      map
    end

    def mutations(value)
      raise RecipeError, "more than #{MAX_MUTATIONS} mutations" if value.is_a?(Array) && value.size > MAX_MUTATIONS

      list(value, "mutations") { Mutation.load(_1, ceiling: @ceilings.image) }
    rescue MutationError => e
      raise RecipeError, e.message
    end

    def expected(value)
      raise RecipeError, "expected is not an object" unless value.is_a?(Hash)

      value.to_h do |stage, text|
        raise RecipeError, "expected #{stage} is not a string" unless text.is_a?(String)

        [stage.to_s, Expectation.parse(text)]
      end
    rescue ExpectationError, StatusError => e
      raise RecipeError, "expected: #{e.message}"
    end

    def violation(value)
      case value
      in { rule:, scope:, **nil } then ViolationRecord.new(rule:, scope:)
      else raise RecipeError, "a violation is not an object of rule and scope"
      end
    end

    def resource_limits(value)
      raise RecipeError, "resource_limits is not an object" unless value.is_a?(Hash)

      value.each do |resource, limit|
        raise RecipeError, "unknown resource #{FsimgSpec.shown(resource.to_s)}" unless LIMIT_KEYS.include?(resource.to_s)

        ceiling = @ceilings.limit(resource)
        raise RecipeError, "resource limit #{resource} #{FsimgSpec.shown(limit)} is not an integer in 0..#{ceiling}" unless
          limit.is_a?(Integer) && limit.between?(0, ceiling)
      end
    end

    def list(value, what, &)
      raise RecipeError, "#{what} is not a list" unless value.is_a?(Array)

      value.map(&)
    end

    def optional(value) = value.nil? ? nil : yield(value)
  end
end
