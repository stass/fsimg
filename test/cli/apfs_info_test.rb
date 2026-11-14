# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  APFS_ROLES = {
    "System" => 1, "User" => 2, "Recovery" => 4, "VM" => 8,
    "Preboot" => 16, "Installer" => 32, "Data" => 64, "Baseband" => 128,
    "Update" => 192, "xART" => 256, "Hardware" => 320, "Backup" => 384,
    "Sidecar" => 448, "Reserved 8" => 512, "Enterprise" => 576,
    "Reserved 10" => 640, "Prelogin" => 704
  }.freeze
  APFS_BLOCK_BYTES = 4096
  APFS_SMALL_CAPTURE = "apfs-newfs-4194304-plain"

  def test_apfs_native_volume_information_matches_diskutil
    native = apfs_native_directory
    skip("diskutil plist conversion requires macOS plutil") unless RUBY_PLATFORM.include?("darwin")
    plists = native.glob("apfs-*/diskutil-list.plist").sort
    refute_empty(plists, "FSIMG_APFS_NATIVE contains no diskutil captures")
    plists.each do |plist|
      capture = plist.dirname
      conversion = run_process(["/usr/bin/plutil", "-convert", "json", "-o", "-", plist.to_s])
      assert_equal(0, conversion.status, conversion.stderr)
      container = JSON.parse(conversion.stdout).fetch("Containers").fetch(0)
      image = capture / "image.img"
      if capture.basename.to_s == "apfs-newfs-372736-plain"
        result = invoke(arguments: ["i", "-f", image.to_s], expected: 1)
        assert_includes(result.stderr, "invalid block count", capture.to_s)
        next
      end
      mismatches = []
      volumes = []
      maximum_found = false
      # The format's maximum is 100; include index 100 to observe the refusal.
      (0..100).each do |slot|
        result = invoke(arguments: ["i", "--json", "-o", "volume=#{slot}", "-f", image.to_s], expected: nil)
        if result.status != 0
          message = result.stdout + result.stderr
          if message.include?("not below the container's maximum")
            maximum_found = true
            break
          end
          next if message.include?("is empty")

          mismatches << "slot #{slot}: status #{result.status}: #{message}"
          next
        end
        info = apfs_info_record(result)
        reported_index = Integer(info.fetch("details").fetch("volume_index"))
        reported_count = Integer(info.fetch("details").fetch("volume_count"))
        mismatches << "slot #{slot}: reported index #{reported_index}" unless reported_index == slot
        unless reported_count == container.fetch("Volumes").length
          mismatches << "slot #{slot}: reported volume count #{reported_count}"
        end
        volumes << [info.fetch("uuid").downcase, info.fetch("label"), Integer(info.fetch("details").fetch("role"))]
        expected_geometry = {
          "volume_bytes" => container.fetch("CapacityCeiling"),
          "cluster_bytes" => APFS_BLOCK_BYTES,
          "allocation_units" => container.fetch("CapacityCeiling") / APFS_BLOCK_BYTES
        }
        expected_geometry.each do |field, wanted|
          actual = Integer(info.fetch(field))
          mismatches << "slot #{slot} #{field}: expected #{wanted}, got #{actual}" unless actual == wanted
        end
      end
      wanted = container.fetch("Volumes").map do |volume|
        roles = volume.fetch("Roles")
        role = roles.empty? ? 0 : roles.map { APFS_ROLES.fetch(_1) }.reduce(:|)
        [volume.fetch("APFSVolumeUUID").downcase, volume.fetch("Name"), role]
      end
      mismatches << "no maximum-index refusal" unless maximum_found
      mismatches << "duplicate volume results: #{volumes.inspect}" unless volumes.uniq.length == volumes.length
      unless volumes.to_set == wanted.to_set
        mismatches << "volume set: expected #{wanted.inspect}, got #{volumes.inspect}"
      end
      assert_empty(mismatches, "#{capture.basename}:\n#{mismatches.join("\n")}")
    end
  end

  def test_apfs_multiple_volumes_require_selection
    capture = apfs_native_directory / "apfs-diskutil-2147483648-e10-roles"
    result = invoke(arguments: ["i", "-f", (capture / "image.img").to_s], expected: 1)
    assert_match(/holds 4 volumes in slots 0 to 3; select one with volume=/, result.stderr)
  end

  def test_apfs_native_layer_stacks_report_volume_label
    directory = apfs_native_directory / "e13-layers"
    {
      "e13-gpt-udrw.dmg" => "apfs/gpt",
      "e13-gpt-udzo.dmg" => "apfs/gpt/dmg",
      "e13-gpt-udbz.dmg" => "apfs/gpt/dmg",
      "e13-apm-native.img" => "apfs/apm",
      "e13-apm-typed.img" => "apfs/apm",
      "e13-apm-udzo.dmg" => "apfs/apm/dmg",
      "e13-none-udzo.dmg" => "apfs/dmg"
    }.each do |name, stack|
      info = apfs_info_record(invoke(arguments: ["i", "--json", "-f", (directory / name).to_s]))
      assert_equal("E13", info.fetch("label"), name)
      assert_equal(stack, info.fetch("stack"), name)
    end
    result = invoke(arguments: ["i", "-f", (directory / "e13-gpt-ulfo.dmg").to_s], expected: 1)
    assert_includes(result.stderr, "LZFSE")
  end

  def test_apfs_force_waives_only_block_zero_magic
    original = apfs_small_copy
    block = original.byteslice(0, APFS_BLOCK_BYTES)
    assert_equal(block.unpack1("Q<"), apfs_fletcher(block), "independent checksum matches native block zero")

    damaged = original.dup
    damaged.setbyte(32, damaged.getbyte(32) ^ 1)
    (@root / "apfs.img").binwrite(apfs_seal_block(damaged, address: 0))
    invoke(arguments: ["i", "-F", "apfs/-", "-f", "apfs.img"], expected: 1)
    invoke(arguments: ["i", "--force", "-F", "apfs/-", "-f", "apfs.img"])

    (@root / "apfs.img").binwrite(damaged)
    result = invoke(arguments: ["i", "--force", "-F", "apfs/-", "-f", "apfs.img"], expected: 1)
    assert_includes(result.stderr, "checksum")

    wrong_oid = original.dup
    wrong_oid[8, 8] = [2].pack("Q<")
    (@root / "apfs.img").binwrite(apfs_seal_block(wrong_oid, address: 0))
    result = invoke(arguments: ["i", "--force", "-F", "apfs/-", "-f", "apfs.img"], expected: 1)
    assert_includes(result.stderr, "invalid object header")
  end

  def test_apfs_checkpoint_and_container_map_damage_fail_open
    original = apfs_small_copy
    checkpoint = apfs_selected_checkpoint(original)
    desc_base = apfs_u64(checkpoint, 112)
    desc_count = checkpoint.unpack1("L<", offset: 104)
    desc_index = checkpoint.unpack1("L<", offset: 136)
    mapping_address = desc_base + desc_index % desc_count
    map_address = apfs_u64(checkpoint, 160)
    { "checkpoint mapping" => mapping_address, "container object map" => map_address }.each do |name, address|
      damaged = original.dup
      offset = address * APFS_BLOCK_BYTES + 40
      damaged.setbyte(offset, damaged.getbyte(offset) ^ 1)
      (@root / "apfs.img").binwrite(damaged)
      result = invoke(arguments: ["i", "-f", "apfs.img"], expected: 1)
      assert_match(/APFS /, result.stderr, name)
    end
  end

  def test_apfs_truncated_native_container_reports_missing_bytes
    original = apfs_small_copy
    checkpoint = apfs_selected_checkpoint(original)
    desc_end = apfs_u64(checkpoint, 112) + checkpoint.unpack1("L<", offset: 104)
    data_base = apfs_u64(checkpoint, 120)
    data_count = checkpoint.unpack1("L<", offset: 108)
    data_index = checkpoint.unpack1("L<", offset: 144)
    data_length = checkpoint.unpack1("L<", offset: 148)
    data_last = (0...data_length).map { data_base + (data_index + _1) % data_count }.max
    { "descriptor ring" => desc_end - 1, "selected data run" => data_last }.each do |name, blocks|
      (@root / "apfs.img").binwrite(original.byteslice(0, blocks * APFS_BLOCK_BYTES))
      result = invoke(arguments: ["i", "-f", "apfs.img"], expected: 1)
      assert_includes(result.stderr, "image is truncated", name)
    end

    # Retain both object maps and their roots, and the selected APSB.
    # The native dump identifies the APSB; physical OIDs locate both maps.
    dump = JSON.parse((apfs_native_directory / APFS_SMALL_CAPTURE / "apfsdump.json").read)
    volume_address = dump.fetch("volumes").fetch(0).fetch("addr")
    volume = original.byteslice(volume_address * APFS_BLOCK_BYTES, APFS_BLOCK_BYTES)
    container_map = apfs_u64(checkpoint, 160)
    volume_map = apfs_u64(volume, 128)
    roots = [container_map, volume_map].map do |address|
      apfs_u64(original, address * APFS_BLOCK_BYTES + 48)
    end
    highest = [desc_end - 1, data_last, container_map, volume_map, volume_address, *roots].max
    kept = (highest + 1) * APFS_BLOCK_BYTES
    assert_operator(kept, :<, original.bytesize, "native fixture has unused tail")
    (@root / "apfs.img").binwrite(original.byteslice(0, kept))
    info = apfs_info_record(invoke(arguments: ["i", "--json", "-f", "apfs.img"]))
    assert_equal((4194304 - kept).to_s, info.fetch("missing_bytes"))
    human = invoke(arguments: ["i", "-f", "apfs.img"])
    assert_match(/^Missing:.*\(#{4194304 - kept} bytes\)/, human.stdout)
  end

  private

  def apfs_native_directory
    directory = ENV["FSIMG_APFS_NATIVE"]
    skip("APFS native captures require FSIMG_APFS_NATIVE") unless directory
    Pathname.new(directory)
  end

  def apfs_small_copy
    source = apfs_native_directory / APFS_SMALL_CAPTURE / "image.img"
    FileUtils.cp(source, @root / "apfs.img")
    (@root / "apfs.img").binread
  end

  def apfs_info_record(result)
    completed(result).find { _1.fetch("type") == "info" }
  end

  def apfs_u64(bytes, offset) = bytes.unpack1("Q<", offset: offset)

  def apfs_selected_checkpoint(bytes)
    base = apfs_u64(bytes, 112)
    count = bytes.unpack1("L<", offset: 104)
    blocks = (base...(base + count)).map { bytes.byteslice(_1 * APFS_BLOCK_BYTES, APFS_BLOCK_BYTES) }
    blocks.select { _1.byteslice(32, 4) == "NXSB" }.max_by { apfs_u64(_1, 16) }
  end

  # Sum little-endian words, reducing both Fletcher accumulators modulo m.
  def apfs_fletcher(block)
    modulus = 0xffffffff
    first, second = block.byteslice(8..).unpack("L<*").reduce([0, 0]) do |(sum1, sum2), word|
      next1 = (sum1 + word) % modulus
      [next1, (sum2 + next1) % modulus]
    end
    check1 = modulus - (first + second) % modulus
    check2 = modulus - (first + check1) % modulus
    check1 | (check2 << 32)
  end

  def apfs_seal_block(bytes, address:)
    result = bytes.dup
    block = result.byteslice(address * APFS_BLOCK_BYTES, APFS_BLOCK_BYTES)
    result[address * APFS_BLOCK_BYTES, 8] = [apfs_fletcher(block)].pack("Q<")
    result
  end
end
