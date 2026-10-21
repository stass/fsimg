# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"
require "minitest"
require "fsimg_spec"

# Recipes round-trip to identical JSON, and the loader refuses every recipe
# the design lists (specs/design-spec-testing.md#recipes).
class TestRecipe < Minitest::Test
  # Small ceilings, so that one past each is a short recipe.
  CEILINGS = FsimgSpec::Ceilings.new(image: 1 << 20, memory: 64 << 20, scratch: 1 << 20, entries: 4, depth: 3,
                                     path: 32, fds: 128, output: 4096, work: 1 << 20, seconds: 10)

  READING = <<~JSON
    {
      "schema_version": 1,
      "id": "fat16.bpb.bytes_per_sector.invalid.1000",
      "format": "fat16",
      "base": {
        "builder": "independent",
        "tree": [
          {"path": "DIR", "type": "directory"},
          {"path": "DIR/HELLO.TXT", "type": "file", "size": 12, "content": "positional", "seed": 7, "holes": [[0, 2], [4, 1]]}
        ],
        "parameters": {"bytes_per_sector": 1000, "fat_count": 2}
      },
      "base_sha256": "#{"ab" * 32}",
      "tool_versions": {"independent": "digest"},
      "mutations": [
        {"op": "set_u16le", "offset": 11, "value": 1000},
        {"op": "replace_bytes", "offset": 3, "bytes": "4d53444f53"},
        {"op": "truncate", "length": 4096}
      ],
      "forced_format": "fat16",
      "stages": ["info", "ls", "verify"],
      "kind": "targeted",
      "intended_violation": "fat.bpb.bytes_per_sector",
      "actual_violations": [{"rule": "fat.bpb.bytes_per_sector", "scope": "open"}],
      "isolation": "single_rule",
      "expected": {"info": "reject:FSIMG_EFORMAT", "ls": "reject:FSIMG_EFORMAT", "verify": "reject:FSIMG_EFORMAT"},
      "random_seed": 42,
      "resource_limits": {"entries": 4}
    }
  JSON

  CREATION = <<~JSON
    {
      "schema_version": 1,
      "id": "regression.fat32.create.case_pair",
      "format": "fat32",
      "source": {"root": "manifest", "tree": [{"path": "a", "type": "file", "size": 1, "content": "zero"}], "settings": {"size": 4096}},
      "stages": ["create", "verify"],
      "kind": "valid",
      "intended_violation": null,
      "actual_violations": [],
      "isolation": null,
      "expected": {"create": "accept", "verify": "accept"},
      "reason": "a promoted creation case"
    }
  JSON

  def test_recipe_round_trips_to_identical_json
    [READING, CREATION].each do |text|
      assert_equal canonical(text), load(canonical(text)).dump
    end
  end

  # The loader reads only the canonical form, which also refuses the
  # duplicate keys JSON.parse would resolve silently.
  def test_loader_refuses_text_that_is_not_canonical
    compact = JSON.generate(JSON.parse(READING))
    duplicate = canonical(READING).sub(%(  "format": "fat16",\n), %(  "format": "fat12",\n  "format": "fat16",\n))

    [compact, duplicate].each do |text|
      assert_match(/not in canonical form/, assert_raises(FsimgSpec::RecipeError) { load(text) }.message)
    end
  end

  # A recipe read with File.binread is binary-tagged; with a non-ASCII
  # field it still loads, and the caller's string keeps its tag.
  def test_binary_tagged_text_loads
    text = generate(JSON.parse(CREATION).merge("reason" => "caf\u00e9 r\u00e9gression")).b

    assert_equal "caf\u00e9 r\u00e9gression", load(text).reason
    assert_equal Encoding::BINARY, text.encoding
  end

  def test_refusal_message_is_cut_whatever_it_quotes
    error = assert_raises(FsimgSpec::RecipeError) { load(generate(TestRecipe.with_entry(JSON.parse(READING), "path" => "a" * 100_000, "type" => "x"))) }

    assert_operator error.message.length, :<=, 403
  end

  # The canonical form is fixed bytes on every Ruby: empty containers stay
  # on one line and map keys are sorted whatever order the recipe was
  # built with.
  def test_dump_is_the_same_bytes_on_every_ruby
    recipe = FsimgSpec::Recipe.new(
      id: "regression.fat32.create.case_pair", format: "fat32", kind: "valid", stages: %w[create verify],
      source: FsimgSpec::Source.new(tree: [], settings: { size: 1, cluster: 2 }),
      expected: { "create" => FsimgSpec::Expectation.parse("accept"), "verify" => FsimgSpec::Expectation.parse("accept") },
      reason: "a promoted creation case"
    )

    assert_equal <<~JSON, recipe.dump
      {
        "schema_version": 1,
        "id": "regression.fat32.create.case_pair",
        "format": "fat32",
        "source": {
          "root": "manifest",
          "tree": [],
          "settings": {
            "cluster": 2,
            "size": 1
          }
        },
        "stages": [
          "create",
          "verify"
        ],
        "kind": "valid",
        "intended_violation": null,
        "actual_violations": [],
        "isolation": null,
        "expected": {
          "create": "accept",
          "verify": "accept"
        },
        "reason": "a promoted creation case"
      }
    JSON
  end

  def test_loaded_recipe_keeps_its_fields
    recipe = load(canonical(READING))

    assert recipe.reading?
    assert_equal({ bytes_per_sector: 1000, fat_count: 2 }, recipe.base.parameters)
    assert_equal "\x4d\x53\x44\x4f\x53".b, recipe.mutations[1].bytes
    assert_equal "reject:FSIMG_EFORMAT", recipe.expected.fetch("ls").to_s
  end

  # A placeholder the tests replace with a lone surrogate escape, which
  # JSON.parse decodes to a string that is not valid UTF-8.
  NOT_UTF8 = "@not-utf-8@"
  # A placeholder the tests replace with 1.5, which the canonical writer
  # never emits.
  FRACTION = "@fraction@"

  # The valid recipe with its second tree entry's fields replaced.
  def self.with_entry(recipe, fields)
    tree = recipe["base"]["tree"]
    recipe.merge("base" => recipe["base"].merge("tree" => [tree.first, tree.last.merge(fields)]))
  end

  def self.with_base(recipe, fields) = recipe.merge("base" => recipe["base"].merge(fields))

  def self.entry(path) = { "path" => path, "type" => "file", "size" => 0, "content" => "zero" }

  def self.creation(source) = JSON.parse(CREATION).merge("source" => source)

  # Each entry edits a valid recipe and names the message the loader's
  # refusal must carry, so that an unrelated check cannot satisfy it.
  REFUSALS = {
    "unknown schema version" => [/unknown schema version 2/, ->(r) { r.merge("schema_version" => 2) }],
    "fractional schema version" => [/a number that is not an integer/, ->(r) { r.merge("schema_version" => FRACTION) }],
    "integer beyond 64 bits" => [/an integer of 100 bits/, ->(r) { r.merge("random_seed" => 1 << 99) }],
    "unknown mutation" => [/malformed mutation/, ->(r) { r.merge("mutations" => [{ "op" => "flip_bits", "offset" => 0 }]) }],
    "more mutations than the loader takes" =>
      [/more than 4096 mutations/, ->(r) { r.merge("mutations" => [{ "op" => "truncate", "length" => 0 }] * 4097) }],
    "missing required field" => [/missing base_sha256/, ->(r) { r.except("base_sha256") }],
    "unknown field" => [/unknown or misplaced "comment"/, ->(r) { r.merge("comment" => "x") }],
    "id longer than 128 bytes" => [/id "a{20}/, ->(r) { r.merge("id" => "a" * 129) }],
    "id outside the alphabet" => [/id "Fat16"/, ->(r) { r.merge("id" => "Fat16") }],
    "id starting with a dot" => [/id ".fat16"/, ->(r) { r.merge("id" => ".fat16") }],
    "id starting with a dash" => [/id "-fat16"/, ->(r) { r.merge("id" => "-fat16") }],
    "unmodeled format" => [/unknown format "ext4"/, ->(r) { r.merge("format" => "ext4") }],
    "unmodeled forced format" => [/unknown format "exfat"/, ->(r) { r.merge("forced_format" => "exfat") }],
    "short base_sha256" => [/base_sha256 is not 64/, ->(r) { r.merge("base_sha256" => "ab" * 31) }],
    "builder naming a parent directory" => [/malformed builder name "\.\."/, ->(r) { with_base(r, "builder" => "..") }],
    "parameter that is not an integer" =>
      [/parameter fat_count "2" is not a nonnegative integer/, ->(r) { with_base(r, "parameters" => { "fat_count" => "2" }) }],
    "parameter beyond the ceiling" =>
      [/parameter clusters 1048577 exceeds 1048576/, ->(r) { with_base(r, "parameters" => { "clusters" => (1 << 20) + 1 }) }],
    "parameter name that is no identifier" =>
      [/malformed parameter name "-o x"/, ->(r) { with_base(r, "parameters" => { "-o x" => 1 }) }],
    "tool name naming a parent directory" => [/malformed tool name "\.\."/, ->(r) { r.merge("tool_versions" => { ".." => "x" }) }],
    "unknown tree entry type" => [/unknown type "symlink"/, ->(r) { with_entry(r, "type" => "symlink") }],
    "tree path that is not UTF-8" => [/not UTF-8/, ->(r) { with_entry(r, "path" => NOT_UTF8) }],
    "empty tree path" => [/tree path "" is empty/, ->(r) { with_entry(r, "path" => "") }],
    "absolute tree path" => [/is absolute/, ->(r) { with_entry(r, "path" => "/etc/passwd") }],
    "tree path with .." => [/component/, ->(r) { with_entry(r, "path" => "DIR/../x") }],
    "tree path with ." => [/component/, ->(r) { with_entry(r, "path" => "./x") }],
    "tree path with an empty component" => [/component/, ->(r) { with_entry(r, "path" => "DIR//x") }],
    "tree path with NUL" => [/contains NUL/, ->(r) { with_entry(r, "path" => "a\0b") }],
    "tree path deeper than the ceiling" => [/is deeper than 3/, ->(r) { with_entry(r, "path" => "a/b/c/d") }],
    "tree path longer than the ceiling" => [/longer than 32 bytes/, ->(r) { with_entry(r, "path" => "a" * 33) }],
    "tree path whose image path with its NUL exceeds the ceiling" =>
      [/longer than 32 bytes/, ->(r) { with_entry(r, "path" => "a" * 31) }],
    "tree path that repeats" => [/a tree path repeats/, ->(r) { with_entry(r, "path" => "DIR") }],
    "tree path under an undeclared directory" => [/no directory of the tree/, ->(r) { with_entry(r, "path" => "OTHER/x") }],
    "tree path under a file" =>
      [/no directory of the tree/, ->(r) { with_base(r, "tree" => [entry("f"), entry("f/x")]) }],
    "directory with a size" =>
      [/a directory has no size, content, seed or holes/, ->(r) { with_entry(r, "path" => "SUB", "type" => "directory") }],
    "unknown content" => [/unknown content "random"/, ->(r) { with_entry(r, "content" => "random") }],
    "file without a size" => [/size is not an integer/, ->(r) { with_entry(r, "size" => nil) }],
    "generated content without a seed" => [/positional content needs a seed/, ->(r) { with_entry(r, "seed" => nil) }],
    "zero content with a seed" => [/zero content takes no seed or holes/, ->(r) { with_entry(r, "content" => "zero", "holes" => []) }],
    "zero content with holes" => [/zero content takes no seed or holes/, ->(r) { with_entry(r, "content" => "zero", "seed" => nil) }],
    "unknown tree entry field" => [/a tree entry is not an object/, ->(r) { with_entry(r, "mode" => 420) }],
    "empty hole" => [/a hole is empty/, ->(r) { with_entry(r, "holes" => [[0, 0]]) }],
    "touching holes" => [/out of order, overlap or touch/, ->(r) { with_entry(r, "holes" => [[0, 2], [2, 1]]) }],
    "holes out of order" => [/out of order, overlap or touch/, ->(r) { with_entry(r, "holes" => [[4, 1], [0, 2]]) }],
    "hole beyond the file" => [/a hole ends beyond the file's 12 bytes/, ->(r) { with_entry(r, "holes" => [[10, 3]]) }],
    "hole that is no pair" => [/holes are not a list/, ->(r) { with_entry(r, "holes" => [[1, 2, 3]]) }],
    "more holes than a manifest names" =>
      [/holes are not a list of at most 64/, ->(r) { with_entry(r, "size" => 200, "holes" => (0..64).map { [2 * _1, 1] }) }],
    "file size beyond the ceiling" => [/tree content exceeds 4096 bytes/, ->(r) { with_entry(r, "size" => 4097) }],
    "more entries than the ceiling" => [/5 tree entries exceed 4/, ->(r) { with_base(r, "tree" => (1..5).map { entry("f#{_1}") }) }],
    "mutation beyond the ceiling" =>
      [/reaches beyond 1048576 bytes/, ->(r) { r.merge("mutations" => [{ "op" => "truncate", "length" => (1 << 20) + 1 }]) }],
    "resource limit beyond the ceiling" => [/resource limit entries 5/, ->(r) { r.merge("resource_limits" => { "entries" => 5 }) }],
    "unknown resource" => [/unknown resource "memory"/, ->(r) { r.merge("resource_limits" => { "memory" => 1 }) }],
    "expected missing a stage" => [/expected must name exactly the stages/, ->(r) { r.merge("expected" => r["expected"].except("verify")) }],
    "expected naming an extra stage" =>
      [/expected must name exactly the stages/, ->(r) { r.merge("expected" => r["expected"].merge("extract" => "accept")) }],
    "both a base and a source" =>
      [/a base or a source, not both/, ->(r) { r.merge("source" => JSON.parse(CREATION)["source"]) }],
    "reading case running create" =>
      [/a reading case cannot run create/, ->(r) { r.merge("stages" => %w[info create], "expected" => { "info" => "accept", "create" => "accept" }) }],
    "stages out of run order" =>
      [/stages are not in the order/, ->(r) { r.merge("stages" => %w[verify info], "expected" => { "verify" => "accept", "info" => "accept" }) }],
    "creation case not running create" =>
      [/a creation case needs create/, ->(_) { JSON.parse(CREATION).merge("stages" => ["verify"], "expected" => { "verify" => "accept" }) }],
    "unknown kind" => [/unknown kind "fuzzed"/, ->(r) { r.merge("kind" => "fuzzed") }],
    "targeted case without an intended violation" =>
      [/a targeted case names its intended violation/, ->(r) { r.merge("intended_violation" => nil) }],
    "valid case naming an intended violation" =>
      [/only a targeted case names an intended violation/, ->(r) { r.merge("kind" => "valid") }],
    "promoted case without a reason" => [/needs a reason/, ->(r) { r.merge("id" => "regression.fat16.x") }],
    "creation setting that is not an integer" =>
      [/a number that is not an integer/, ->(_) { creation("root" => "manifest", "tree" => [], "settings" => { "size" => FRACTION }) }],
    "exact volume beyond the output ceiling" =>
      [/setting size 4097 exceeds 4096/, ->(_) { creation("root" => "manifest", "tree" => [], "settings" => { "size" => 4097 }) }],
    "unknown creation setting" =>
      [/unknown setting "label"/, ->(_) { creation("root" => "manifest", "tree" => [], "settings" => { "label" => 1 }) }],
    "unknown source root" =>
      [/unknown source root "archive"/, ->(_) { creation("root" => "archive", "tree" => [], "settings" => {}) }],
    "source without a root" => [/source is not an object of root/, ->(_) { creation("tree" => [], "settings" => {}) }]
  }.freeze

  def test_loader_refuses_each_listed_recipe
    valid = JSON.parse(READING)

    REFUSALS.each do |reason, (message, edit)|
      error = assert_raises(FsimgSpec::RecipeError, reason) { load(with_placeholders(generate(edit.call(valid)))) }
      assert_match message, error.message, reason
    end
  end

  # Every string and key that is not UTF-8 is refused before any field
  # check reads it, whichever field holds it.
  def test_loader_refuses_text_that_is_not_utf8_in_any_field
    valid = JSON.parse(READING)
    edits = [
      ->(r) { r.merge("id" => NOT_UTF8) },
      ->(r) { r.merge("reason" => NOT_UTF8) },
      ->(r) { r.merge("source_revision" => NOT_UTF8) },
      ->(r) { r.merge("intended_violation" => NOT_UTF8) },
      ->(r) { r.merge("actual_violations" => [{ "rule" => NOT_UTF8, "scope" => "open" }]) },
      ->(r) { r.merge("base_sha256" => NOT_UTF8) },
      ->(r) { r.merge("tool_versions" => { "independent" => NOT_UTF8 }) },
      ->(r) { r.merge("tool_versions" => { NOT_UTF8 => "x" }) },
      ->(r) { r.merge("mutations" => [{ "op" => "replace_bytes", "offset" => 0, "bytes" => NOT_UTF8 }]) },
      ->(r) { r.merge("expected" => r["expected"].merge("info" => NOT_UTF8)) },
      ->(r) { r.merge("expected" => r["expected"].merge(NOT_UTF8 => "accept")) },
      ->(r) { r.merge("resource_limits" => { NOT_UTF8 => 1 }) },
      ->(r) { TestRecipe.with_base(r, "builder" => NOT_UTF8) },
      ->(r) { TestRecipe.with_base(r, "parameters" => { NOT_UTF8 => 1 }) }
    ]

    edits.each_with_index do |edit, index|
      error = assert_raises(FsimgSpec::RecipeError, "edit #{index}") { load(with_placeholders(generate(edit.call(valid)))) }
      assert_match(/not UTF-8/, error.message, "edit #{index}")
    end
  end

  def test_loader_refuses_oversized_text_and_strings
    long_reason = JSON.parse(CREATION).merge("reason" => "x" * ((1 << 20) + 1))

    assert_raises(FsimgSpec::RecipeError) { load(" " * ((64 << 20) + 1)) }
    assert_match(/exceeds 1048576/, assert_raises(FsimgSpec::RecipeError) { load(generate(long_reason)) }.message)
  end

  # A recipe a generator builds in code obeys the same format as a loaded
  # one: each value refuses what the loader would.
  def test_recipe_values_check_themselves_when_built
    built = {
      "absolute tree path" => -> { FsimgSpec::TreeEntry.file("/etc/x", size: 0, content: "zero") },
      "seeded file without a seed" => -> { FsimgSpec::TreeEntry.file("x", size: 1) },
      "builder that is no name" => -> { FsimgSpec::Base.new(builder: "a b; rm", parameters: {}, tree: []) },
      "setting name that is no identifier" => -> { FsimgSpec::Source.new(tree: [], settings: { "-o x": 1 }) },
      "id naming a parent" => -> { FsimgSpec::Recipe.new(**load(canonical(READING)).to_h, id: "../x") },
      "creation case with mutations" => -> { FsimgSpec::Recipe.new(**load(canonical(CREATION)).to_h, mutations: []) }
    }

    built.each { |reason, construct| assert_raises(FsimgSpec::RecipeError, reason) { construct.call } }
  end

  def test_loader_refuses_malformed_json
    assert_raises(FsimgSpec::RecipeError) { load("{") }
    assert_raises(FsimgSpec::RecipeError) { load("[]") }
  end

  # A hostile value never reaches a message whole.
  def test_refusal_messages_cut_long_values
    error = assert_raises(FsimgSpec::RecipeError) { load(generate(JSON.parse(READING).merge("id" => "A" * 100_000))) }

    assert_operator error.message.length, :<, 200
  end

  private

  def load(text) = FsimgSpec::Recipe.load(text, ceilings: CEILINGS)

  # A parsed recipe written in canonical form, keeping its key order.
  def generate(fields) = "#{FsimgSpec::CanonicalJson.generate(fields)}\n"

  def canonical(text) = generate(JSON.parse(text))

  def with_placeholders(text) = text.gsub(%("#{NOT_UTF8}"), '"\\udcff"').gsub(%("#{FRACTION}"), "1.5")
end
