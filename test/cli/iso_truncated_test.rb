# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_iso_unused_missing_tail_remains_readable
    iso_truncated_fixture(name: "tail.img", missing_payload: false)

    info = completed(invoke(arguments: ["i", "--json", "-S", "off", "-f", "tail.img"]))
      .find { _1.fetch("type") == "info" }
    assert_equal("2048", info.fetch("missing_bytes"))
    entries = completed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "tail.img"]))
      .select { _1.fetch("type") == "entry" }
    assert_equal(["/A.TXT"], entries.map { _1.fetch("path") })
    assert_equal("hello".b, invoke(arguments: ["x", "-O", "-S", "off", "-f", "tail.img", "/A.TXT"]).stdout)
  end

  def test_iso_truncated_verification_refuses_missing_tail
    check_iso_truncated_verification(payload: [])
  end

  def test_iso_truncated_payload_verification_refuses_missing_tail
    check_iso_truncated_verification(payload: ["-d"])
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

  # The seeded ISO holds 44 files of 96K sharing one extent in a 256K image
  # whose descriptor declares more space: 4224K of output against a default
  # limit of 16 times the 256K slice.
  def test_iso_shared_extents_do_not_enlarge_directory_extraction_budget
    shared_iso_fixture
    (@root / "default").mkdir
    (@root / "explicit").mkdir
    limit = 16 * File.size(@root / "shared.img")

    error = failed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "shared.img",
      "-C", "default"], expected: 1))
    assert_equal(-8, error.fetch("code"))
    assert_equal("output", error.fetch("resource"))
    assert_equal(limit.to_s, error.fetch("limit"))
    assert_equal("incomplete", error.fetch("output_state"))
    assert_equal(limit / (96 * 1024), (@root / "default/shared").children.size)

    (@root / "human").mkdir
    human = invoke(arguments: ["x", "-S", "off", "-f", "shared.img", "-C", "human"], expected: 1)
    assert_includes(human.stderr, ("fsimg: output limit reached: #{limit} bytes, 16 times the " \
      "selected volume's length; rerun with -l output=BYTES to allow more, " \
      "or -l output=unlimited\n").b)

    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "shared.img",
      "-l", "output=5M", "-C", "explicit"]))
    files = (@root / "explicit/shared").children
    assert_equal(44, files.size)
    files.each { assert_equal(96 * 1024, _1.size) }
  end

  def test_explicit_output_limit_message_names_the_flag
    shared_iso_fixture
    (@root / "out").mkdir

    result = invoke(arguments: ["x", "-S", "off", "-f", "shared.img", "-l", "output=10",
      "-C", "out"], expected: 1)
    assert_includes(result.stderr, ("fsimg: output limit reached: 10 bytes, as -l output set; " \
      "rerun with a larger -l output=BYTES to allow more, or -l output=unlimited\n").b)
  end

  def test_output_unlimited_extracts_what_the_default_refuses
    shared_iso_fixture
    (@root / "out").mkdir

    completed(invoke(arguments: ["x", "--json", "-S", "off", "-f", "shared.img",
      "-l", "output=unlimited", "-C", "out"]))
    assert_equal(44, (@root / "out/shared").children.size)
  end

  def test_only_the_output_limit_accepts_unlimited
    shared_iso_fixture

    error = failed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "shared.img",
      "-l", "entries=unlimited"], expected: 2))
    assert_includes(error.fetch("message"), "resource limit requires an unsigned decimal count")
  end

  def test_output_limit_rejects_a_word_that_is_not_unlimited
    shared_iso_fixture

    error = failed(invoke(arguments: ["t", "--json", "-S", "off", "-f", "shared.img",
      "-l", "output=bogus"], expected: 2))
    assert_includes(error.fetch("message"),
      "output limit requires an unsigned byte quantity or unlimited")
  end

  private

  def shared_iso_fixture
    helper = @executable.dirname / "test-iso9660"
    invoke(executable: helper, arguments: ["--seed-shared", (@root / "shared.img").to_s])
  end

  def check_iso_truncated_verification(payload:)
    iso_truncated_fixture(name: "tail.img", missing_payload: false)
    error = failed(invoke(arguments: ["W", *payload, "--json", "-S", "off", "-f", "tail.img"], expected: 1))
    assert_equal(-5, error.fetch("code"))
    assert_includes(error.fetch("message"), "truncated")
    assert_includes(error.fetch("message"), "2048")
  end

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
