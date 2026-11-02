# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "tmpdir"
require "fsimg_spec/iso9660"

class Iso9660ModelTest < Minitest::Test
  Iso = FsimgSpec::Iso9660

  def test_layout_values_expose_generated_members
    raw = Iso::Builder.descriptor(Iso::Builder.options({}), joliet: false)
    fields = Iso::Layout.decode(Iso::Layout::Descriptor, raw)
    assert_equal 2048, fields.block_bytes_le
    assert_respond_to fields, :block_bytes_be
    assert_equal 2048, fields.block_bytes_be
    assert_equal 2048, raw.bytesize
  end

  def test_builder_emits_both_numeric_orders_at_specified_offsets
    raw = Iso::Builder.descriptor(Iso::Builder.options({}), joliet: false)
    assert_equal [Iso::Builder::BLOCKS, Iso::Builder::BLOCKS],
                 [raw.byteslice(80, 4).unpack1("V"), raw.byteslice(84, 4).unpack1("N")]
    assert_equal "CD001".b, raw.byteslice(1, 5)
    assert_equal 34, raw.getbyte(156)
    assert_equal 1, raw.getbyte(188)
  end

  def test_prefixes_leave_absent_structures_undefined
    with_image({}) do |image|
      [0, 1, 7, 32768, 34815].each do |length|
        model = Iso::Model.decode(image.truncate(length))
        assert FsimgSpec.undefined?(model.primary), "prefix #{length} invented a root"
        assert FsimgSpec.undefined?(model.tree), "prefix #{length} invented a tree"
      end
    end
  end

  def test_decoder_follows_relocation_without_a_second_physical_tree
    with_image(relocation: 1) do |image|
      model = Iso::Model.decode(image)
      assert_equal "rockridge", model.view
      assert_equal %w[.rr_moved FILE.TXT SUB SUB/LEAF.BIN], model.tree.map { _1.path.dup.force_encoding("UTF-8") }.sort
      assert_equal 3, model.primary.directories.size
    end
  end

  def test_multi_extent_digest_covers_the_combined_file
    with_image(sections: 1) do |image|
      entry = Iso::Model.decode(image).tree.find { _1.path == "FILE.TXT" }
      assert_equal 2057, entry.size
      assert_equal Digest::SHA256.hexdigest("\0".b * 2057), entry.sha256
    end
  end

  def test_numeric_format_names_resolve_before_family_suffixes
    suites = { "iso9660" => Iso::Suite, "iso" => Object.new, "fat" => FsimgSpec::Fat::Suite }
    assert_same Iso::Suite, FsimgSpec::CasePhase.suite_for("iso9660", suites:)
    assert_same FsimgSpec::Fat::Suite, FsimgSpec::CasePhase.suite_for("fat32", suites:)
  end

  def test_descriptor_root_both_order_copies_are_checked
    with_image({}) do |image|
      model = Iso::Model.decode(image.patch(16 * 2048 + 162, [Iso::Builder::ROOT + 1].pack("N")))
      assert_includes violations(model), "iso9660.directory.copies"
    end
  end

  def test_last_legal_descriptor_slot_still_checks_terminator_tail
    with_image({}) do |image|
      primary = image.read(16 * 2048, 2048).dup
      primary[140, 4] = [85].pack("V")
      primary[148, 4] = [86].pack("N")
      primary[158, 8] = Iso::Builder.both32(87)
      root = image.read(Iso::Builder::ROOT * 2048, 2048).dup
      root[2, 8] = Iso::Builder.both32(87)
      root[36, 8] = Iso::Builder.both32(87)
      root[70, 8] = Iso::Builder.both32(88)
      path_l = Iso::Builder.path_record(identifier: "\0".b, extent: 87, parent: 1, big_endian: false)
      path_m = Iso::Builder.path_record(identifier: "\0".b, extent: 87, parent: 1, big_endian: true)
      terminator = "\xffCD001\x01".b.ljust(2048, "\0")
      legal = image.patch(16 * 2048, primary * 64).patch(80 * 2048, terminator)
        .patch(85 * 2048, path_l).patch(86 * 2048, path_m).patch(87 * 2048, root)
      assert_empty violations(Iso::Model.decode(legal))
      corrupt = legal.patch(80 * 2048 + 7, "\x01".b)
      assert_includes violations(Iso::Model.decode(corrupt)), "iso9660.descriptor.terminator"
    end
  end

  def test_duplicate_rock_ridge_display_names_are_checked
    parameters = {rockridge: 1, subdirectory: 1}
    with_image(parameters) do |image|
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      file = Iso::Builder.record(identifier: "FILE.TXT;1", extent: Iso::Builder::DATA, bytes: 9,
        flags: 0, system_use: Iso::Builder.rr(directory: false, name: "SUB"))
      changed = replace_children(image, parameters:, joliet: false, records: [file, records.last])
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.names.unique"
    end
  end

  def test_joliet_names_collide_after_version_display_normalization
    parameters = {joliet: 1, subdirectory: 1}
    with_image(parameters) do |image|
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: true)
      file = Iso::Builder.record(identifier: Iso::Builder.ucs2("SUB;1"), extent: Iso::Builder::DATA,
        bytes: 9, flags: 0)
      changed = replace_children(image, parameters:, joliet: true, records: [file, records.last])
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.names.unique"
    end
  end

  def test_dot_display_name_never_reaches_host_name_trials
    with_image({}) do |image|
      file = Iso::Builder.record(identifier: ".", extent: Iso::Builder::DATA, bytes: 9, flags: 0)
      changed = replace_children(image, parameters: {}, joliet: false, records: [file])
      model = Iso::Model.decode(changed)
      assert FsimgSpec.undefined?(model.tree)
      assert_includes violations(model), "iso9660.names.primary"
    end
  end

  def test_ce_offset_is_not_normalized_into_another_block
    parameters = {continuation: 1}
    with_image(parameters) do |image|
      offset = Iso::Witnesses.ce
      changed = image.patch(offset + 4, Iso::Builder.both32(Iso::Builder::CONTINUATION - 1))
        .patch(offset + 12, Iso::Builder.both32(2048))
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.susp.ce"
    end
  end

  def test_ce_big_endian_copy_is_checked
    with_image(continuation: 1) do |image|
      changed = image.patch(Iso::Witnesses.ce + 8, [Iso::Builder::CONTINUATION + 1].pack("N"))
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.susp.ce"
    end
  end

  def test_susp_control_versions_are_opaque_without_rock_ridge
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      changed = image.patch(Iso::Builder::ROOT * 2048 + 97, "XXXXXXXXXX".b)
      offset = Iso::Witnesses.system_use(parameters:) + 48
      changed = changed.patch(offset, "PD".b).patch(offset + 3, "\x02".b)
      assert_empty violations(Iso::Model.decode(changed))
    end
  end

  def test_child_sp_is_not_a_rock_ridge_extension_entry
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      offset = Iso::Witnesses.system_use(parameters:) + 48
      changed = image.patch(offset, "SP".b).patch(offset + 3, "\x02".b)
      assert_empty violations(Iso::Model.decode(changed))
    end
  end

  def test_payload_requires_a_complete_allocated_block
    with_image({}) do |image|
      changed = image.truncate(Iso::Builder::DATA * 2048 + 9)
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.payload.bounds"
    end
  end

  def test_logical_path_limits_are_applied_before_concatenation
    with_image(subdirectory: 1) do |image|
      ceilings = FsimgSpec.fetch_profile("quick").ceilings.with(depth: 1)
      model = Iso::Model.decode(image, ceilings:)
      assert_includes model.unclassified_scopes, "directories"
      assert FsimgSpec.undefined?(model.tree)
    end
  end

  def test_cumulative_metadata_budget_bounds_repeated_continuation_reads
    parameters = {symlink: 1, target_shape: 2}
    with_image(parameters) do |image|
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      repeated = (records.take(2) + [records.last] * 14).join.b
      assert_operator repeated.bytesize, :<=, 2048
      changed = image.patch(Iso::Builder::ROOT * 2048, repeated.ljust(2048, "\0"))
      ceilings = FsimgSpec.fetch_profile("quick").ceilings.with(entries: 4)
      model = Iso::Model.decode(changed, ceilings:)
      assert_includes model.unclassified_scopes, "directories"
      assert_operator model.susp_entries.size, :<, 256
    end
  end

  def test_file_version_order_survives_a_directory_tie
    parameters = {subdirectory: 1}
    with_image(parameters) do |image|
      file_one = Iso::Builder.record(identifier: "FILE.;1", extent: Iso::Builder::DATA, bytes: 9, flags: 0)
      directory = Iso::Builder.record(identifier: "FILE", extent: Iso::Builder::SUB, bytes: 2048, flags: 2)
      file_two = Iso::Builder.record(identifier: "FILE.;2", extent: Iso::Builder::LEAF, bytes: 9, flags: 0)
      changed = replace_children(image, parameters:, joliet: false, records: [file_one, directory, file_two])
      left = Iso::Builder.path_record(identifier: "FILE", extent: Iso::Builder::SUB, parent: 1, big_endian: false)
      right = Iso::Builder.path_record(identifier: "FILE", extent: Iso::Builder::SUB, parent: 1, big_endian: true)
      changed = changed.patch(Iso::Builder::PATH_L * 2048 + 10, left)
        .patch(Iso::Builder::PATH_M * 2048 + 10, right)
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.primary.order"
    end
  end

  def test_boot_image_cannot_claim_a_directory_block
    with_image(boot: 1) do |image|
      changed = image.patch(Iso::Builder::CATALOG * 2048 + 40, [Iso::Builder::ROOT].pack("V"))
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.ownership"
    end
  end

  def test_root_rock_ridge_extension_versions_belong_to_directory_preparation
    with_image(rockridge: 1) do |image|
      changed = image.patch(Iso::Builder::ROOT * 2048 + 34 + 7 + 3, "\x02".b)
      model = Iso::Model.decode(changed)
      classification = Iso::Rules.registry.classify(model,
        FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::TrialAnswers::HELD))
      assert_equal ["directories"], classification.violations.select { _1.rule.id == "iso9660.rr.version" }.map(&:scope)
    end
  end

  def test_dot_link_entries_still_obey_known_entry_length
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      first = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false).first.bytesize
      changed = image.patch(Iso::Builder::ROOT * 2048 + first + 34 + 36,
        "SL\x04\x01".b + Iso::Builder.entry("PD", "\0".b * 4))
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.rr.lengths"
    end
  end

  def test_an_nm_piece_without_continue_cannot_start_another_name
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      metadata = Iso::Builder.px(kind: "file") + Iso::Builder.nm("FIRST") + Iso::Builder.nm("SECOND")
      file = Iso::Builder.record(identifier: "FILE.TXT;1", extent: Iso::Builder::DATA,
        bytes: 9, flags: 0, system_use: metadata)
      changed = replace_children(image, parameters:, joliet: false, records: [file])
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.rr.names"
    end
  end

  def test_device_number_on_a_regular_file_is_corrupt
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      metadata = Iso::Builder.rr(directory: false, name: "FILE.TXT") + Iso::Builder.entry("PN", "\0".b * 16)
      file = Iso::Builder.record(identifier: "FILE.TXT;1", extent: Iso::Builder::DATA,
        bytes: 9, flags: 0, system_use: metadata)
      changed = replace_children(image, parameters:, joliet: false, records: [file])
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.rr.kind"
    end
  end

  def test_short_sl_before_another_sl_decodes_as_corruption
    parameters = {symlink: 1}
    with_image(parameters) do |image|
      original = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      metadata = Iso::Builder.px(kind: "symlink") + Iso::Builder.tf + Iso::Builder.nm("LINK") +
        Iso::Builder.entry("SL", "".b) + Iso::Builder.entry("SL", "\0\0\x01a".b)
      link = Iso::Builder.record(identifier: "LINK.;1", extent: 0, bytes: 0, flags: 0, system_use: metadata)
      changed = replace_children(image, parameters:, joliet: false, records: [original.fetch(2), link])
      model = Iso::Model.decode(changed)
      assert_includes violations(model), "iso9660.rr.lengths"
      assert_includes violations(model), "iso9660.rr.sl.framing"
    end
  end

  def test_a_sole_refused_link_component_is_unsupported_without_inventing_corruption
    [16, 32].each do |flags|
      parameters = {symlink: 1}
      with_image(parameters) do |image|
        original = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
        metadata = Iso::Builder.px(kind: "symlink") + Iso::Builder.tf + Iso::Builder.nm("LINK") +
          Iso::Builder.entry("SL", [0, flags, 0].pack("C3"))
        link = Iso::Builder.record(identifier: "LINK.;1", extent: 0, bytes: 0, flags: 0, system_use: metadata)
        changed = replace_children(image, parameters:, joliet: false, records: [original.fetch(2), link])
        broken = violations(Iso::Model.decode(changed))
        assert_includes broken, "iso9660.rr.sl.profile"
        refute_includes broken, "iso9660.rr.sl.framing"
      end
    end
  end

  def test_non_utf8_rock_ridge_name_is_omitted_but_requires_extract_refusal
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      changed = image.patch(Iso::Witnesses.system_use(parameters:) + 48 + 5, "\xff".b)
      model = Iso::Model.decode(changed)
      classification = Iso::Rules.registry.classify(model,
        FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::TrialAnswers::HELD))
      assert_equal 1, model.omitted_names.size
      assert_empty model.tree
      assert_empty classification.without_destination.violations
      assert FsimgSpec::Expectation.derive(classification, "ls").accept
      assert_equal [FsimgSpec::Status.parse("FSIMG_EUNSUPPORTED")],
        FsimgSpec::Expectation.derive(classification, "extract").statuses
    end
  end

  def test_six_byte_sp_prefix_stays_opaque
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      dot = Iso::Builder.record(identifier: "\0".b, extent: Iso::Builder::ROOT, bytes: 2048,
        flags: 2, system_use: "SP\x07\x01\xbe\xef".b)
      changed = image.patch(Iso::Builder::ROOT * 2048, ([dot] + records.drop(1)).join.b.ljust(2048, "\0"))
      model = Iso::Model.decode(changed)
      assert_equal "primary", model.view
      assert_empty violations(model)
    end
  end

  def test_opaque_child_susp_framing_is_checked_at_verification
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      original = Iso::Model.decode(image)
      extension = original.susp_entries.find { _1.scope == "open" && _1.fields.signature == "ER" }
      child_px = original.primary.records.find { _1.identifier == "FILE.TXT;1" }.entries.find { _1.fields.signature == "PX" }
      changed = image.patch(extension.offset + 8, "OTHER_1991".b).patch(child_px.offset + 2, "\x03".b)
      result = classification(Iso::Model.decode(changed))
      assert FsimgSpec::Expectation.derive(result, "ls").accept
      refute FsimgSpec::Expectation.derive(result, "verify").accept
      assert_includes result.violations.map { [_1.rule.id, _1.scope] }, ["iso9660.susp.framing", "verification"]
    end
  end

  def test_opaque_child_continuation_is_checked_at_verification
    parameters = {rockridge: 1, continuation: 1}
    with_image(parameters) do |image|
      original = Iso::Model.decode(image)
      extension = original.susp_entries.find { _1.scope == "open" && _1.fields.signature == "ER" }
      continuation = original.susp_entries.find { _1.fields.signature == "CE" }
      changed = image.patch(extension.offset + 8, "OTHER_1991".b)
        .patch(continuation.offset + 4, [Iso::Builder::BLOCKS + 1].pack("V"))
      result = classification(Iso::Model.decode(changed))
      assert FsimgSpec::Expectation.derive(result, "ls").accept
      refute FsimgSpec::Expectation.derive(result, "verify").accept
      assert_includes result.violations.map { [_1.rule.id, _1.scope] }, ["iso9660.susp.ce", "verification"]
    end
  end

  def test_unselected_joliet_file_extent_is_checked_at_verification
    parameters = {rockridge: 1, joliet: 1}
    with_image(parameters) do |image|
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: true)
      offset = Iso::Builder::JOLIET_ROOT * 2048 + records.take(2).sum(&:bytesize) + 2
      changed = image.patch(offset, Iso::Builder.both32(Iso::Builder::BLOCKS))
      result = classification(Iso::Model.decode(changed))
      assert FsimgSpec::Expectation.derive(result, "ls").accept
      refute FsimgSpec::Expectation.derive(result, "verify").accept
      assert_includes result.violations.map { [_1.rule.id, _1.scope] }, ["iso9660.payload.bounds", "verification"]
    end
  end

  def test_truncated_file_extent_is_rejected_during_directory_preparation
    with_image({}) do |image|
      model = Iso::Model.decode(image.truncate(Iso::Builder::DATA * 2048))
      expectation = FsimgSpec::Expectation.derive(classification(model), "ls")
      refute expectation.accept
      assert_equal [FsimgSpec::Status.parse("FSIMG_ECORRUPT")], expectation.statuses
    end
  end

  def test_root_er_allows_trailing_bytes_after_its_declared_fields
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      extension = Iso::Builder.entry("ER", [10, 0, 0, 1].pack("C4") + "RRIP_1991A".b + "\0\0".b)
      handshake = Iso::Builder.entry("SP", "\xbe\xef\0".b) + Iso::Builder.rr(directory: true) + extension
      dot = Iso::Builder.record(identifier: "\0".b, extent: Iso::Builder::ROOT, bytes: 2048,
        flags: 2, system_use: handshake)
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      changed = image.patch(Iso::Builder::ROOT * 2048, ([dot] + records.drop(1)).join.b.ljust(2048, "\0"))
      model = Iso::Model.decode(changed)
      assert_equal "rockridge", model.view
      assert_empty violations(model)
    end
  end

  def test_unrecognized_rock_ridge_handshake_does_not_interpret_pl
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      extension = Iso::Builder.entry("ER", [10, 0, 0, 1].pack("C4") + "OTHER_1991".b)
      handshake = Iso::Builder.entry("SP", "\xbe\xef\0".b) + Iso::Builder.rr(directory: true) + extension
      dot = Iso::Builder.record(identifier: "\0".b, extent: Iso::Builder::ROOT, bytes: 2048,
        flags: 2, system_use: handshake)
      up = Iso::Builder.record(identifier: "\x01".b, extent: Iso::Builder::ROOT, bytes: 2048,
        flags: 2, system_use: Iso::Builder.rr(directory: true) + Iso::Builder.pointer("PL", Iso::Builder::ROOT))
      records = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet: false)
      changed = image.patch(Iso::Builder::ROOT * 2048, ([dot, up] + records.drop(2)).join.b.ljust(2048, "\0"))
      model = Iso::Model.decode(changed)
      assert_equal "primary", model.view
      assert_empty violations(model)
    end
  end

  def test_root_er_lengths_are_validated_before_offering_rock_ridge
    with_image(rockridge: 1) do |image|
      changed = image.patch(Iso::Builder::ROOT * 2048 + 34 + 7 + 48 + 5, "\xff".b)
      model = Iso::Model.decode(changed)
      assert_includes violations(model), "iso9660.susp.er"
      refute_equal "rockridge", model.view
    end
  end

  def test_device_px_cannot_disagree_with_the_iso_directory_flag
    parameters = {rockridge: 1, subdirectory: 1}
    with_image(parameters) do |image|
      offset = Iso::Witnesses.file(parameters:, index: 3) + 36 + 4
      changed = image.patch(offset, Iso::Builder.both32(0o020644))
      broken = violations(Iso::Model.decode(changed))
      assert_includes broken, "iso9660.rr.kind"
      refute_includes broken, "iso9660.rr.kind_profile"
    end
  end

  def test_susp_controls_accept_noncanonical_versions_and_st_lengths
    %w[ST PD].each do |signature|
      parameters = {rockridge: 1}
      with_image(parameters) do |image|
        control = Iso::Builder.entry(signature, "\0".b * 4).dup
        control.setbyte(3, 2)
        metadata = Iso::Builder.rr(directory: false) + control + Iso::Builder.nm("FILE.TXT")
        file = Iso::Builder.record(identifier: "FILE.TXT;1", extent: Iso::Builder::DATA,
          bytes: 9, flags: 0, system_use: metadata)
        changed = replace_children(image, parameters:, joliet: false, records: [file])
        assert_empty violations(Iso::Model.decode(changed)), signature
      end
    end
  end

  def test_an_ordinary_directory_parent_cannot_carry_pl
    parameters = {rockridge: 1, subdirectory: 1}
    with_image(parameters) do |image|
      original = Iso::Model.decode(image).primary.directories.find { _1.extent == Iso::Builder::SUB }.records
      parent = Iso::Builder.record(identifier: "\x01".b, extent: Iso::Builder::ROOT, bytes: 2048, flags: 2,
        system_use: Iso::Builder.rr(directory: true, links: 3) + Iso::Builder.pointer("PL", Iso::Builder::ROOT))
      changed = image.patch(Iso::Builder::SUB * 2048, [original.first.raw, parent, original.last.raw].join.b.ljust(2048, "\0"))
      classification = classification(Iso::Model.decode(changed))
      assert_includes classification.violations.map { [_1.rule.id, _1.scope] }, ["iso9660.relocation.parents", "directories"]
    end
  end

  def test_later_section_record_copies_belong_to_directory_preparation
    parameters = {rockridge: 1, sections: 1}
    with_image(parameters) do |image|
      changed = image.patch(Iso::Witnesses.file(parameters:, index: 3) + 14, [10].pack("N"))
      result = classification(Iso::Model.decode(changed))
      assert_equal ["directories"], result.violations.select { _1.rule.id == "iso9660.directory.copies" }.map(&:scope)
    end
  end

  def test_non_block_directory_length_still_requires_zero_allocated_padding
    with_image({}) do |image|
      count = Iso::Builder.root_records(Iso::Builder.options({}), joliet: false).sum(&:bytesize)
      valid = image.patch(16 * 2048 + 166, Iso::Builder.both32(count))
        .patch(Iso::Builder::ROOT * 2048 + 10, Iso::Builder.both32(count))
        .patch(Iso::Builder::ROOT * 2048 + 34 + 10, Iso::Builder.both32(count))
      assert_empty violations(Iso::Model.decode(valid))
      changed = valid.patch(Iso::Builder::ROOT * 2048 + count, "\x01".b)
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.directory.framing"
    end
  end

  def test_a_short_rock_ridge_record_header_is_classified_as_framing_corruption
    parameters = {rockridge: 1}
    with_image(parameters) do |image|
      changed = image.patch(Iso::Witnesses.file(parameters:), "\x01".b)
      assert_includes violations(Iso::Model.decode(changed)), "iso9660.directory.framing"
    end
  end

  private

  def classification(model) = Iso::Rules.registry.classify(model,
    FsimgSpec::Environment.new(forced_format: nil, trial_answers: FsimgSpec::TrialAnswers::HELD))

  def violations(model) = classification(model).rule_ids

  def replace_children(image, parameters:, joliet:, records:)
    original = Iso::Builder.root_records(Iso::Builder.options(parameters), joliet:)
    start = (joliet ? Iso::Builder::JOLIET_ROOT : Iso::Builder::ROOT) * 2048 + original.take(2).sum(&:bytesize)
    image.patch(start, records.join.b.ljust(original.drop(2).sum(&:bytesize), "\0"))
  end

  def with_image(parameters)
    Dir.mktmpdir("iso-model-") do |directory|
      path = File.join(directory, "fixture.iso")
      Iso::Builder.write(path, parameters:, tree: Iso::Builder.tree(parameters), max_bytes: 1 << 20)
      yield FsimgSpec::ImageView.open(path)
    end
  end
end
