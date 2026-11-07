# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "tmpdir"
require "fsimg_spec/erofs"

class ErofsRulesTest < Minitest::Test
  Erofs = FsimgSpec::Erofs

  def test_crc32c_register_of_the_check_string_is_the_stored_form
    assert_equal 0x1CF96D7C, Erofs::Crc32c.stored("123456789")
    assert_equal 0xE3069283, Erofs::Crc32c.stored("123456789") ^ 0xFFFFFFFF
  end

  def test_every_rule_has_a_valid_and_an_invalid_witness_that_classify_as_intended
    with_cases do |cases, registry|
      registry.rules.each do |rule|
        witnesses = rule.domain.call
        assert witnesses.any? { _1.intended.nil? }, "#{rule.id} has no valid witness"
        assert witnesses.any? { _1.intended == rule.id }, "#{rule.id} has no invalid witness"
        witnesses.each do |witness|
          classified = cases.call(witness)
          assert_nil classified.defect, "#{witness.id}: #{classified.defect}"
          rule_ids = classified.kase.classification.rule_ids
          if witness.intended
            assert_includes rule_ids, rule.id, witness.id
            refute classified.kase.expected.values.all?(&:unclassified), witness.id
          else
            assert_empty rule_ids, witness.id
          end
        end
      end
    end
  end

  def test_valid_images_decode_to_the_tree_the_builder_wrote
    with_cases do |cases, _registry|
      [{}, { block_bits: 9, xattr: 1, subdirectory: 1 }, { block_bits: 11, checksum: 0 }].each do |parameters|
        witness = Erofs::Witnesses::Witness.new(id: "erofs.test.tree", parameters:, mutations: [], intended: nil, rules: [])
        model = cases.call(witness).model
        expected = Erofs::Builder.tree(parameters).map { [_1.path.b, _1.type, _1.type == "file" ? _1.size : nil] }
        assert_equal expected.sort, model.tree.map { [_1.path, _1.type, _1.size] }.sort, parameters.inspect
      end
    end
  end

  def test_unrecognised_volume_is_rejected_with_the_format_class_at_every_stage
    expected = FsimgSpec::Status.parse("FSIMG_EFORMAT")
    with_cases do |cases, _registry|
      cases.call(witness("erofs.identity.half_magic")).kase.expected.each_value { assert_equal [expected], _1.statuses }
    end
  end

  def test_unsupported_features_are_refused_with_the_unsupported_class_at_open
    expected = FsimgSpec::Status.parse("FSIMG_EUNSUPPORTED")
    with_cases do |cases, _registry|
      %w[erofs.sb.incompat.unknown_bit erofs.sb.devices.extra_device erofs.sb.dirblkbits.set
         erofs.sb.block_size.above_maximum erofs.xattr.index.unknown_index].each do |id|
        kase = cases.call(witness(id)).kase
        assert_equal [expected], kase.expected.values.flat_map(&:statuses).uniq, id
      end
    end
  end

  def test_content_domain_violations_are_found_by_extraction_not_by_listing
    with_cases do |cases, _registry|
      kase = cases.call(witness("erofs.inode.data_domain.start_block")).kase
      assert_equal ["extract"], kase.recipe.stages
      assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], kase.expected.fetch("extract").statuses
    end
  end

  def test_an_empty_root_directory_is_found_by_open_and_a_root_directory_outside_the_domain_by_listing
    with_cases do |cases, _registry|
      assert_equal ["info"], cases.call(witness("erofs.inode.directory_size.empty")).kase.recipe.stages
      outside = Erofs::Witnesses::Witness.new(
        id: "erofs.test.root_data_outside", parameters: {}, intended: nil, rules: [],
        mutations: [Erofs::Witnesses.u32(Erofs::Witnesses.inode(Erofs::Builder::ROOT, 0x10), 0xFFFFFFFF)]
      )
      kase = cases.call(outside).kase
      assert_includes kase.classification.rule_ids, "erofs.inode.data_domain"
      assert kase.expected.fetch("info").accept, "open reads the root inode, not its data"
      assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], kase.expected.fetch("ls").statuses
    end
  end

  def test_directory_violations_are_found_by_listing_not_by_open
    with_cases do |cases, _registry|
      kase = cases.call(witness("erofs.dir.type.unknown_type")).kase
      assert_equal ["ls"], kase.recipe.stages
      assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], kase.expected.fetch("ls").statuses
    end
  end

  def test_checksum_range_ends_at_the_block_size_and_is_exclusive
    with_cases do |cases, _registry|
      assert_includes cases.call(witness("erofs.sb.checksum.last_byte_of_range")).kase.classification.rule_ids, "erofs.sb.checksum"
      assert_empty cases.call(witness("erofs.sb.checksum.first_byte_past_range")).kase.classification.rule_ids
    end
  end

  def test_rule_sources_name_existing_format_anchors
    source = File.read(File.expand_path("../../../../specs/design-erofs.md", __dir__))
    counts = Hash.new(0)
    anchors = source.lines.filter_map { _1[/\A\#{2,3} (.+)/, 1] }.map do |heading|
      base = heading.downcase.gsub(/[^a-z0-9 -]/, "").tr(" ", "-")
      count = counts[base]
      counts[base] += 1
      count.zero? ? base : "#{base}-#{count}"
    end
    Erofs::Rules.registry.rules.each { assert_includes anchors, _1.source.split("#", 2).last, _1.id }
  end

  private

  def witness(id)
    Erofs::Witnesses.deterministic(Erofs::Rules.registry).find { _1.id == id } || flunk("no witness #{id}")
  end

  def with_cases
    Dir.mktmpdir("erofs-rules-") do |directory|
      Dir.mkdir(File.join(directory, "trials"), 0o700)
      registry = Erofs::Rules.registry
      cases = Erofs::Cases.new(registry:, cache: FsimgSpec::FixtureCache.new(File.join(directory, "cache")),
                               ceilings: FsimgSpec.fetch_profile("quick").ceilings,
                               name_trials: FsimgSpec::NameTrials.new(File.join(directory, "trials")))
      yield cases, registry
    end
  end
end
