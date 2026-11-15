# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"
require "fsimg_spec/apfs"

# The container rules hold on native captures and each is targeted by a
# mutation it flags. The captures are E1, E8 and E12's, read from
# $FSIMG_APFS_NATIVE, the directory holding one directory per capture with
# its image.img; without it the capture tests skip. A rule a mutation
# targets must be among the mutated image's violations; other rules may
# fire with it, as on any formatter base.
class TestApfsRules < Minitest::Test
  Apfs = FsimgSpec::Apfs
  ENVIRONMENT = FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::NO_TRIAL)
  # E1's plain containers from 4 MiB to 100 GiB, the hdiutil one, E8's
  # mount history and E12's 1 TiB and 128 TiB containers.
  VALID_CAPTURES = %w[
    apfs-newfs-4194304-plain apfs-newfs-33554432-plain apfs-newfs-536870912-plain apfs-newfs-1073741824-plain
    apfs-newfs-8589934592-plain apfs-newfs-107374182400-plain apfs-hdiutil-33554432-plain
    apfs-newfs-1073741824-e8-history apfs-newfs-1099511627776-e12-1t apfs-newfs-140737488355328-e12-128t
  ].freeze
  # Volumes with records: E2's both personalities, E3's 5087 inodes, E4's
  # names, E5's links and attributes, E6's clones and holes, E7's
  # compressed files, E10's fourteen volumes and E14's long names.
  RECORD_CAPTURES = %w[
    apfs-newfs-67108864-e2-ci apfs-newfs-67108864-e2-cs apfs-newfs-268435456-e3-ci apfs-newfs-268435456-e3-cs
    apfs-newfs-33554432-e4-prefix-ci apfs-newfs-33554432-e4-prefix-cs apfs-newfs-33554432-e4-u17marks-ci
    apfs-newfs-33554432-e4-u18-cs apfs-newfs-33554432-e4-unassigned-ci apfs-newfs-67108864-e5-links
    apfs-newfs-67108864-e5-xattr apfs-newfs-67108864-e5-xlong apfs-newfs-67108864-e6-clone apfs-newfs-67108864-e6-sparse
    apfs-newfs-67108864-e6-xclone apfs-newfs-67108864-e7-compression apfs-diskutil-8589934592-e10-allroles
    apfs-newfs-67108864-e14-ci apfs-newfs-67108864-e14-cs
  ].freeze
  # E4's 20000 names in each personality, whose hashes the record rules
  # recompute: the slowest captures, about half a minute each.
  NAME_CAPTURES = %w[apfs-newfs-268435456-e4-names-ci apfs-newfs-268435456-e4-names-cs].freeze
  # E1's 91-block container, below the 1 MiB minimum.
  BELOW_MINIMUM = "apfs-newfs-372736-plain"

  def registry = @registry ||= Apfs::Rules.registry

  def test_registry_accepts_every_container_rule
    assert_operator registry.rules.size, :>=, 50
    format_rules, extraction_rules = registry.rules.partition { Apfs::Rules.format_rule?(_1) }
    assert(format_rules.all? { _1.id.start_with?("apfs.") && _1.source.start_with?("specs/design-apfs.md#") })
    assert_equal %w[apfs.extract.safe_kinds host.name.collision host.name.unrepresentable], extraction_rules.map(&:id).sort
  end

  def test_an_image_without_the_magic_violates_only_identity
    view = FsimgSpec::ImageView.new(base: File::NULL, length: 0, patches: [])
    assert_equal ["apfs.identity"], registry.classify(Apfs::Model.decode(view), ENVIRONMENT).rule_ids
  end

  def test_every_rule_holds_on_the_native_captures
    found = VALID_CAPTURES.to_h { [_1, classify(native(_1))] }
    assert_equal VALID_CAPTURES.to_h { [_1, [[], []]] }, found
  end

  def test_every_rule_holds_on_the_native_volumes_with_records
    found = RECORD_CAPTURES.to_h { [_1, classify(native(_1))] }
    assert_equal RECORD_CAPTURES.to_h { [_1, [[], []]] }, found
  end

  def test_every_stored_name_hash_of_e4_is_the_hash_of_its_form
    skip "the hash rules need Unicode #{Apfs::Names::UNICODE_VERSION} tables" unless Apfs::Names::TABLES_MATCH
    found = NAME_CAPTURES.to_h { [_1, classify(native(_1))] }
    assert_equal NAME_CAPTURES.to_h { [_1, [[], []]] }, found
  end

  def test_the_documented_names_hash_to_their_documented_values
    forms = { "root" => 2_989_177, "private-dir" => 2_828_707 }
    assert_equal forms, forms.to_h { |name, _| [name, Apfs::Names.name_hash(Apfs::Names.form(name.b, case_insensitive: true))] }
  end

  def test_a_starter_that_decomposes_into_marks_reorders_with_the_marks_before_it
    # n with dot below (class 220) then U+0F75, which decomposes to U+0F71
    # (129) and U+0F74 (132): canonical order puts both before U+0323.
    assert_equal [0x6E, 0xF71, 0xF74, 0x323], Apfs::Names.form("\u1E47\u0F75".b, case_insensitive: false).codepoints
  end

  def test_a_native_container_below_the_minimum_violates_only_the_block_count
    assert_equal [["apfs.nx.block_count"], []], classify(native(BELOW_MINIMUM))
  end

  def test_every_rule_is_targeted_by_a_mutation_it_flags
    bases = Apfs::Witnesses::BASES.map { Apfs::Witnesses::Base.open(_1, native(_1)) }
    # The hash rules judge names only with the design's Unicode tables.
    unjudged = Apfs::Names::TABLES_MATCH ? [] : %w[apfs.drec.hash apfs.drec.hash_unassigned]
    # Creations target the input and destination rules (test_writer).
    targeted = registry.rules.reject { Apfs::Rules.created?(_1) || Apfs::Witnesses::UNTARGETED.key?(_1.id) || unjudged.include?(_1.id) }
    witnesses = targeted.to_h { |rule| [rule.id, bases.flat_map { |base| rule.domain.call(base).map { [_1, base] } }] }
    assert_empty witnesses.select { _2.empty? }.keys, "rules without a witness"
    missed = witnesses.values.flatten(1).reject { |witness, base| flagged?(witness, base) }.map { _1.first.id }
    assert_empty missed, "witnesses their rule does not flag"
  end

  def test_untargeted_rules_exist_and_name_why
    assert(Apfs::Witnesses::UNTARGETED.all? { |id, reason| registry.fetch(id) && !reason.empty? })
  end

  def test_a_two_level_object_map_with_its_child_in_a_free_block_is_valid
    base = open_base
    model = Apfs::Model.decode(Apfs::Witnesses.two_level_object_map(base).reduce(base.view) { |view, mutation| mutation.apply(view) })
    assert_equal [[], []], [registry.classify(model, ENVIRONMENT).rule_ids, model.unclassified_scopes]
    assert_equal 2, model.object_map_tree.nodes.size
  end

  def test_a_root_without_fixed_sizes_decodes_and_violates_the_flags_rule
    base = open_base
    flags = base.model.object_map_tree.nodes.first.head.flags
    assert_includes classify_mutated(base, Apfs::Witnesses.resealed(base, base.object_map_root, [32, :u16, flags & ~Apfs::BTree::FIXED])),
                    "apfs.btree.flags"
  end

  def test_a_reap_list_naming_itself_ends_and_is_not_well_formed
    base = open_base
    oid = base.selected.superblock.reaper_oid
    mutations = Apfs::Witnesses.resealed(base, base.reaper, [32, :u64, oid], [48, :u64, oid], [56, :u64, oid], [68, :u32, 0xFFFF_FFFF])
    assert_includes classify_mutated(base, mutations), "apfs.reaper.list"
  end

  def test_a_ring_larger_than_the_image_decodes_without_reading_past_it
    base = open_base
    mutations = Apfs::Witnesses.resealed(base, 0, [40, :u64, 1 << 36], [104, :u32, 1 << 16], [112, :u64, 1 << 35])
    assert_includes classify_mutated(base, mutations), "apfs.checkpoint.ring_present"
  end

  def test_a_key_past_the_node_end_is_a_misplaced_table_entry
    name = Apfs::VolumeWitnesses::RECORD_BASE
    base = Apfs::Witnesses::Base.open(name, native(name))
    leaf = Apfs::VolumeWitnesses.leaf(base)
    table_start = Apfs::BTree::NODE_DATA + leaf.head.toc_offset
    key_offset = base.block_size
    # Keep the key below the declared free space, but beyond the block.
    mutations = Apfs::VolumeWitnesses.in_leaf(base, [44, :u16, 2 * base.block_size],
                                            [table_start, :u16, key_offset])
    view = mutations.reduce(base.view) { |image, mutation| mutation.apply(image) }

    model = Apfs::Model.decode(view)
    entry = model.filesystems.first.tree.nodes.first.entries.first

    refute entry.placed
    assert_nil entry.key
    assert_includes registry.classify(model, ENVIRONMENT).rule_ids, "apfs.btree.table"
  end

  private

  def open_base = Apfs::Witnesses::Base.open(Apfs::Witnesses::BASE, native(Apfs::Witnesses::BASE))

  def classify_mutated(base, mutations) =
    registry.classify(Apfs::Model.decode(mutations.reduce(base.view) { |view, mutation| mutation.apply(view) }), ENVIRONMENT).rule_ids

  def native(name)
    root = ENV.fetch("FSIMG_APFS_NATIVE") { skip "FSIMG_APFS_NATIVE names no native capture directory" }
    path = File.join(root, name, "image.img")
    skip "no native capture #{path}" unless File.file?(path)
    path
  end

  def classify(path)
    model = Apfs::Model.decode(FsimgSpec::ImageView.open(path))
    [registry.classify(model, ENVIRONMENT).rule_ids, model.unclassified_scopes]
  end

  def flagged?(witness, base)
    view = witness.mutations.reduce(base.view) { |image, mutation| mutation.apply(image) }
    registry.classify(Apfs::Model.decode(view), ENVIRONMENT).rule_ids.include?(witness.intended)
  end
end
