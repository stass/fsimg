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
  # E1's 91-block container, below the 1 MiB minimum.
  BELOW_MINIMUM = "apfs-newfs-372736-plain"

  def registry = @registry ||= Apfs::Rules.registry

  def test_registry_accepts_every_container_rule
    assert_operator registry.rules.size, :>=, 50
    assert(registry.rules.all? { _1.id.start_with?("apfs.") && _1.source.start_with?("specs/design-apfs.md#") })
  end

  def test_an_image_without_the_magic_violates_only_identity
    view = FsimgSpec::ImageView.new(base: File::NULL, length: 0, patches: [])
    assert_equal ["apfs.identity"], registry.classify(Apfs::Model.decode(view), ENVIRONMENT).rule_ids
  end

  def test_every_rule_holds_on_the_native_captures
    found = VALID_CAPTURES.to_h { [_1, classify(native(_1))] }
    assert_equal VALID_CAPTURES.to_h { [_1, [[], []]] }, found
  end

  def test_a_native_container_below_the_minimum_violates_only_the_block_count
    assert_equal [["apfs.nx.block_count"], []], classify(native(BELOW_MINIMUM))
  end

  def test_every_rule_is_targeted_by_a_mutation_it_flags
    base = Apfs::Witnesses::Base.open(Apfs::Witnesses::BASE, native(Apfs::Witnesses::BASE))
    targeted = registry.rules.reject { Apfs::Witnesses::UNTARGETED.key?(_1.id) }
    witnesses = targeted.to_h { [_1.id, _1.domain.call(base)] }
    assert_empty witnesses.select { _2.empty? }.keys, "rules without a witness"
    missed = witnesses.values.flatten.reject { flagged?(_1, base) }.map(&:id)
    assert_empty missed, "witnesses their rule does not flag"
  end

  def test_untargeted_rules_exist_and_name_why
    assert(Apfs::Witnesses::UNTARGETED.all? { |id, reason| registry.fetch(id) && !reason.empty? })
  end

  def test_a_two_level_object_map_with_its_child_in_a_free_block_is_valid
    base = Apfs::Witnesses::Base.open(Apfs::Witnesses::BASE, native(Apfs::Witnesses::BASE))
    free = base.free_block
    record = base.model.object_map_tree.records.first.map(&:to_h)
    entry = [Apfs::Layout::ObjectMapKey.new(**record.first).to_binary_s, Apfs::Layout::ObjectMapValue.new(**record.last).to_binary_s]
    mutations = [Apfs::Witnesses.bytes(free * base.block_size, Apfs::Witnesses.child_node(base, free, [entry])),
                 *Apfs::Witnesses.root_naming(base, free)]
    model = Apfs::Model.decode(mutations.reduce(base.view) { |view, mutation| mutation.apply(view) })
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
