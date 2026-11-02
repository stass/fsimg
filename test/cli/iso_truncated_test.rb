# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_iso_unused_missing_tail_remains_readable
    iso_truncated_fixture(name: "tail.img", missing_payload: false)

    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("1", info.fetch("details").fetch("missing_blocks"))
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/A.TXT"], entries.map { _1.fetch("path") })
    assert_equal("hello".b, invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/A.TXT"]).stdout)
  end

  def test_iso_missing_payload_is_corruption_instead_of_protocol_fault
    iso_truncated_fixture(name: "payload.img", missing_payload: true)

    completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "payload.img"]))
    (@root / "restore").mkdir
    ["t", "x"].each do |mode|
      arguments = [mode, "--json", "-S", "off", "-f", "payload.img"]
      arguments += ["-C", "restore"] if mode == "x"
      result = invoke(arguments: arguments, expected: 1)
      error = failed(result)
      assert_equal(-5, error.fetch("code"))
      refute_includes(error.fetch("message"), "worker volume information")
    end
  end

  def test_iso_shared_extents_do_not_enlarge_directory_extraction_budget
    helper = @executable.dirname / "test-iso9660"
    invoke(executable: helper, arguments: ["--seed-shared", (@root / "shared.img").to_s])
    (@root / "default").mkdir
    (@root / "explicit").mkdir

    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "shared.img",
      "-C", "default"], expected: 1))
    assert_equal(-8, error.fetch("code"))
    assert_equal("output", error.fetch("resource"))
    assert_equal(File.size(@root / "shared.img").to_s, error.fetch("limit"))
    assert_equal("incomplete", error.fetch("output_state"))
    assert_equal(6, Dir.children(@root / "default/shared").size)

    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "shared.img",
      "-l", "output=320K", "-C", "explicit"]))
    files = (@root / "explicit/shared").children
    assert_equal(8, files.size)
    files.each { assert_equal(40 * 1024, _1.size) }
  end

  private

  def iso_truncated_fixture(name:, missing_payload:)
    helper = @executable.dirname / "test-iso9660"
    invoke(executable: helper, arguments: ["--seed", (@root / name).to_s])
    bytes = (@root / name).binread
    block = 2048
    # ECMA-119 offsets: PVD geometry, root record, then the first child
    # after its two 34-byte dot records. Derive positions from the seed.
    blocks = bytes[16 * block + 80, 4].unpack1("V")
    root = bytes[16 * block + 158, 4].unpack1("V")
    payload = bytes[root * block + 68 + 2, 4].unpack1("V")
    File.truncate(@root / name, (missing_payload ? payload : blocks - 1) * block)
  end
end
