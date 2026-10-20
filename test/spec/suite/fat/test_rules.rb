# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"
require "minitest"
require "prop_check"
require "tmpdir"
require "fsimg_spec"
require "fsimg_spec/fat"

# The FAT rules and their deterministic witnesses, without fsimg: every
# witness classifies as its generator meant, every rule has a witness that
# violates it, the witnesses follow the domains, and a recorded recipe
# replays to the same bytes (specs/design-spec-testing.md#data-flow).
class TestFatRules < Minitest::Test
  Fat = FsimgSpec::Fat
  REGISTRY = Fat::Rules.registry
  CEILINGS = FsimgSpec.fetch_profile("quick").ceilings
  WITNESSES = Fat::Witnesses.deterministic(REGISTRY)

  def test_witness_ids_are_unique
    assert_equal WITNESSES.size, WITNESSES.map(&:id).uniq.size
  end

  # Classification is the generator's check: a valid witness violates
  # nothing, and a targeted one violates its intended rule.
  def test_every_witness_classifies_as_its_generator_meant
    with_cases do |cases|
      defects = WITNESSES.filter_map { cases.call(_1).defect }

      assert_empty defects
    end
  end

  def test_every_rule_has_a_witness_that_violates_it
    targeted = WITNESSES.filter_map(&:intended).uniq

    assert_equal REGISTRY.rules.map(&:id).sort, targeted.sort
  end

  # Every field value the domains name, and nothing else, is a witness.
  def test_field_witnesses_follow_the_domains
    field = Fat::Rules::FIELDS.fetch("fat.bpb.media")
    values = WITNESSES.filter_map { |witness| witness.parameters[:media] if witness.id.start_with?("fat.bpb.media.") }

    assert_equal REGISTRY.fetch("fat.bpb.media").domain.witnesses.select { field.fits?(_1) }, values.sort
  end

  # The gate's first point: a rule's allowed values decide its witnesses,
  # with no per-case code. Here 4096-byte sectors move from the legal set
  # to the invalid examples: the witness of 4096 becomes a targeted case of
  # the rule, and 2048 gains neighbours as the new largest legal value.
  def test_changing_a_domain_changes_the_witnesses
    narrowed = REGISTRY.rules.map do |rule|
      next rule unless rule.id == "fat.bpb.bytes_per_sector"

      rule.with(domain: FsimgSpec::Domain.new(legal: [512, 1024, 2048], invalid: [0, 256, 4096], boundaries: [512, 2048]))
    end
    witnesses = Fat::Witnesses.deterministic(FsimgSpec::Registry.new(narrowed))
    by_id = witnesses.to_h { [_1.id, _1] }

    assert_equal "fat.bpb.bytes_per_sector", by_id.fetch("fat.bpb.bytes_per_sector.1000").intended
    assert_nil by_id.fetch("fat.bpb.bytes_per_sector.0800").intended
    assert by_id.key?("fat.bpb.bytes_per_sector.0801")
    refute by_id.key?("fat.bpb.bytes_per_sector.1001")
  end

  # Two rules of one field share its witnesses: zero FATs breaks the
  # normative rule, and three FATs only the supported profile.
  def test_field_witnesses_target_the_first_rule_that_excludes_them
    intended = WITNESSES.select { _1.parameters.key?(:fat_count) }.to_h { [_1.parameters[:fat_count], _1.intended] }

    assert_equal({ 1 => nil, 2 => nil, 3 => "fat.bpb.num_fats.supported", 255 => "fat.bpb.num_fats.supported" },
                 intended.slice(1, 2, 3, 255))
    assert_equal "fat.bpb.num_fats", witness("fat.bpb.num_fats.0").intended
  end

  # A multi-rule case records its collateral violations: a sector size
  # that does not divide the root region breaks the advisory alignment
  # rule as well.
  def test_targeted_cases_record_collateral_violations
    with_cases do |cases|
      recipe = cases.call(witness("fat.bpb.bytes_per_sector.0201")).kase.recipe

      assert_equal "multi_rule", recipe.isolation
      assert_equal %w[fat.bpb.bytes_per_sector fat.bpb.root_entries.aligned], recipe.actual_violations.map(&:rule)
    end
  end

  # PropCheck draws each field rule's field over its full width, as a
  # mutation of the base. The field's rules read the value back through
  # the model, so a drawn value breaks them exactly where their domains
  # exclude it: this checks how each field is written, decoded and
  # applied, and that no draw makes the model or a predicate raise, not
  # the domains themselves.
  def test_drawn_field_values_classify_by_their_domains
    g = PropCheck::Generators
    with_cases do |cases|
      Fat::Rules::FIELDS.values.uniq.each do |field|
        rules = REGISTRY.rules.select { Fat::Rules::FIELDS[_1.id] == field }
        PropCheck.forall(g.one_of(g.constant(0), g.constant(field.max), g.choose(0..field.max))) do |value|
          violated = cases.call(Fat::Witnesses.drawn(REGISTRY, field:, value:)).kase.recipe.actual_violations.map(&:rule)

          assert_equal rules.reject { _1.domain.legal?(value) }.map(&:id), rules.map(&:id) & violated, "#{field.name} #{value}"
        end
      end
    end
  end

  # A FAT32 witness never differs from its backup boot sector, which a
  # verifier compares with the primary, unless it moves or drops it.
  def test_fat32_witnesses_keep_the_backup_boot_sector_equal
    fat32 = WITNESSES.select do |witness|
      layout = Fat::Builder::Layout.new(witness.parameters)
      layout.fat32? && layout.backup_boot == 6 && witness.mutations.none?(FsimgSpec::Mutation::Truncate)
    end
    with_cases do |cases|
      differing = fat32.reject { |witness| cases.call(witness).image.then { _1.read(0, 512) == _1.read(6 * 512, 512) } }

      assert_empty differing.map(&:id)
    end
  end

  # 0xFFFF in BPB_FSInfo means no FSInfo, and the absent-sector rule is
  # must_accept: its witness runs every reading stage, as a valid one
  # does, so a fault at any stage fails it, and info must accept. A
  # must_reject witness runs only the first stage that checks its rule.
  def test_must_accept_witnesses_run_every_reading_stage
    with_cases do |cases|
      absent = cases.call(witness("fat32.bpb.fsinfo_sector.absent.ffff")).kase
      zero_fats = cases.call(witness("fat.bpb.num_fats.0")).kase

      assert_equal FsimgSpec::READING_STAGES, absent.recipe.stages
      assert_equal "accept", absent.expected.fetch("info").to_s
      assert_equal ["info"], zero_fats.recipe.stages
    end
  end

  # The known tree's gate: every valid and must_accept witness on an
  # unmutated base asserts acceptance at ls, extract and verify, whose
  # records the case phase compares with the tree; a mutated base's tree
  # is unknown, which leaves those stages unclassified.
  def test_unmutated_accepting_witnesses_assert_acceptance_beyond_info
    accepting = WITNESSES.select { _1.intended.nil? || REGISTRY.fetch(_1.intended).policy == "must_accept" }
    with_cases do |cases|
      outcomes = accepting.group_by { _1.mutations.empty? ? "unmutated" : "mutated" }.transform_values do |witnesses|
        witnesses.flat_map { cases.call(_1).kase.expected.values_at("ls", "extract", "verify").map(&:to_s) }.uniq
      end

      assert_equal({ "unmutated" => ["accept"], "mutated" => ["unclassified"] }, outcomes)
    end
  end

  # Section 3.3 calls ExtFlags bits 4-6 and 8-15 reserved without saying
  # what they must hold, so a set reserved bit breaks the reader's profile
  # rule, and an active FAT that does not exist the specification's rule.
  def test_reserved_ext_flags_bits_break_the_profile_rule
    with_cases do |cases|
      violated = %w[fat32.bpb.ext_flags.reserved.0010 fat32.bpb.ext_flags.0082].to_h do |id|
        [id, cases.call(witness(id)).kase.recipe.actual_violations.map(&:rule)]
      end

      assert_equal({ "fat32.bpb.ext_flags.reserved.0010" => ["fat32.bpb.ext_flags.reserved"],
                     "fat32.bpb.ext_flags.0082" => ["fat32.bpb.ext_flags"] }, violated)
      assert_equal "profile", REGISTRY.fetch("fat32.bpb.ext_flags.reserved").category
    end
  end

  # Section 3.3 says to set BPB_BkBootSec "to 0 or 6", so a FAT32 volume
  # without a backup boot record violates no rule, and the FSInfo witness
  # at sector 31, which leaves no room for a backup, breaks only the
  # FSInfo recommendation.
  def test_fat32_without_a_backup_boot_record_violates_no_rule
    no_backup = Fat::Witnesses::Witness.new(id: "fat.test.no_backup", parameters: { clusters: 65_525, backup_boot: 0 },
                                            mutations: [], forced_format: nil, intended: nil, rule: nil)
    with_cases do |cases|
      violated = cases.call(no_backup).kase.recipe.actual_violations
      fsinfo_31 = cases.call(witness("fat32.bpb.fsinfo_sector.recommended.001f")).kase.recipe

      assert_empty violated
      assert_equal ["single_rule", ["fat32.bpb.fsinfo_sector.recommended"]],
                   [fsinfo_31.isolation, fsinfo_31.actual_violations.map(&:rule)]
    end
  end

  # A FAT12 or FAT16 volume may count its sectors in BPB_TotSec32 alone.
  def test_fat12_counting_only_in_tot_sec32_is_valid
    with_cases do |cases|
      recipe = cases.call(witness("fat.bpb.total_sectors.tot_sec32_only")).kase.recipe

      assert_equal ["valid", []], [recipe.kind, recipe.actual_violations]
    end
  end

  # The FAT12 volume without BPB_FATSz16 reads the rest of its boot
  # sector as a valid FAT32 extension, so it breaks fat.bpb.fat_size alone.
  def test_fat12_without_fat_sz16_isolates_its_rule
    with_cases do |cases|
      assert_equal "single_rule", cases.call(witness("fat.bpb.fat_size.fat12_no_fat_sz16")).kase.recipe.isolation
    end
  end

  # The backup boot sector repeats the boot sector's integer and byte
  # changes. A mutation of a kind it does not know, or one that follows a
  # truncation, is a generator defect, named as such.
  Flip = Data.define(:offset)

  def test_backup_copies_refuse_unknown_mutations_and_changes_after_truncation
    truncated = [FsimgSpec::Mutation::Truncate.new(length: 1024), FsimgSpec::Mutation::SetInteger.new(op: "set_u8", offset: 0, value: 0)]
    unknown = assert_raises(FsimgSpec::HarnessError) { Fat::Witnesses.deterministic(generating([Flip.new(offset: 0)])) }
    late = assert_raises(FsimgSpec::HarnessError) { Fat::Witnesses.deterministic(generating(truncated)) }

    assert_match(/TestFatRules::Flip/, unknown.message)
    assert_match(/truncation/, late.message)
  end

  # The gate's last point: a recipe written to text and loaded back
  # renders its base, in a fresh fixture cache, to the bytes it recorded
  # and, with its mutations, to the same image.
  def test_saved_recipe_replays_to_identical_bytes
    with_cases do |cases|
      classified = cases.call(witness("fat.bpb.total_sectors.both_set"))
      loaded = FsimgSpec::Recipe.load(classified.kase.recipe.dump, ceilings: CEILINGS)
      Dir.mktmpdir do |directory|
        base = Fat::Bases.render(loaded.base, cache: FsimgSpec::FixtureCache.new(directory), ceilings: CEILINGS)
        replayed = loaded.mutations.reduce(FsimgSpec::ImageView.open(base)) { |view, mutation| mutation.apply(view) }

        assert_equal loaded.base_sha256, Digest::SHA256.file(base).hexdigest
        assert_equal image_digest(classified.image), image_digest(replayed)
      end
    end
  end

  private

  def witness(id) = WITNESSES.find { _1.id == id } || flunk("no witness #{id}")

  # A registry of one rule whose one witness varies the FAT32 base by
  # mutations.
  def generating(mutations)
    variation = Fat::Witnesses::Variation.new(label: "probe", base: "fat32", violates: true, mutations:)
    FsimgSpec::Registry.new([REGISTRY.fetch("fat.boot.present").with(id: "fat.test.probe", domain: ->(_layouts) { [variation] })])
  end

  def with_cases
    Dir.mktmpdir do |directory|
      cache = FsimgSpec::FixtureCache.new(directory)
      yield Fat::Cases.new(registry: REGISTRY, cache:, ceilings: CEILINGS)
    end
  end

  def image_digest(image)
    Dir.mktmpdir do |directory|
      path = File.join(directory, "image.img")
      image.write(path)
      Digest::SHA256.file(path).hexdigest
    end
  end
end
