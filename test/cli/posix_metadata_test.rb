# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_posix_metadata_reports_losses_and_rejects_unsupported_links
    image_fixture(name: "fat16.img", format: "fat16")
    root = @root
    # FAT and exFAT store no POSIX fields or links: -p imports become counted
    # losses that leave image bytes unchanged, and a link fails planning. The
    # QNX6 and QNX4 formats store every kind and the POSIX fields; ImageFS
    # stores every kind with modification time, owner and mode, and takes the
    # byte order and mountpoint settings.
    decoded = completed(invoke(arguments: ["--formats", "--json"]))
    formats = filesystems(decoded)
    qnx6 = formats.delete("qnx6")
    assert_equal(15, qnx6.fetch("entry_kinds"), "qnx6 entry kinds")
    assert_equal(0x77, qnx6.fetch("metadata_fields"), "qnx6 metadata fields")
    assert_equal(0, qnx6.fetch("max_label_bytes"), "qnx6 max label bytes")
    assert_equal(15, qnx6.fetch("write_settings"), "qnx6 write settings")
    assert_equal(1023, qnx6.fetch("max_link_target_bytes"))
    qnx4 = formats.delete("qnx4")
    assert_equal(15, qnx4.fetch("entry_kinds"), "qnx4 entry kinds")
    assert_equal(0x77, qnx4.fetch("metadata_fields"), "qnx4 metadata fields")
    assert_equal(48, qnx4.fetch("max_label_bytes"), "qnx4 max label bytes")
    assert_equal(4, qnx4.fetch("write_settings"), "qnx4 write settings")
    assert_equal(1023, qnx4.fetch("max_link_target_bytes"))
    ifs = formats.delete("ifs")
    assert_equal(15, ifs.fetch("entry_kinds"), "ifs entry kinds")
    assert_equal(0x62, ifs.fetch("metadata_fields"), "ifs metadata fields")
    assert_equal(0, ifs.fetch("max_label_bytes"), "ifs max label bytes")
    assert_equal(0x11, ifs.fetch("write_settings"), "ifs write settings")
    assert_equal(1024, ifs.fetch("max_link_target_bytes"))
    formats.each do |name, record|
      assert_equal(0, record.fetch("write_settings"), "#{name} exposes no format-specific write settings")
      assert_equal(3, record.fetch("entry_kinds"), "#{name} supports only files and directories")
      assert_equal(15, record.fetch("metadata_fields"), "#{name} supports only DOS attributes and timestamps")
      assert_equal(0, record.fetch("max_link_target_bytes"), "#{name} does not support symbolic link targets")
    end
    assert_equal(11, formats.fetch("fat16").fetch("max_label_bytes"))
    assert_equal(22, formats.fetch("exfat").fetch("max_label_bytes"))
    plans = []
    [[], ["-p"]].each do |flags|
      decoded = completed(
        invoke(
          arguments: [
            "c",
            "-n",
            "--json",
            "-R",
            *flags,
            "-F",
            "fat16",
            "-C",
            "source",
            "."
          ]
        )
      )
      plans.push(decoded.find { |record| record.fetch("type") == "plan" })
    end
    # Three entries each lose an owner and a mode.
    assert_equal((Integer(plans[0].fetch("metadata_losses"), 10) + 6), Integer(plans[1].fetch("metadata_losses"), 10))
    completed(
      invoke(
        arguments: [
          "c",
          "--json",
          "-R",
          "-p",
          "-F",
          "fat16",
          "-f",
          "posix.img",
          "-C",
          "source",
          "."
        ]
      )
    )
    same_bytes(first: (root / "fat16.img"), second: (root / "posix.img"))
    listed = completed(invoke(arguments: ["t", "--json", "-f", "posix.img"]))
    entries = listed
      .select { |record| record.fetch("type") == "entry" }
      .map { |record| [record.fetch("path"), record] }
      .to_h
    assert_equal("directory", entries.fetch("/nested").fetch("kind"))
    assert_equal("file", entries.fetch("/HELLO.TXT").fetch("kind"))
    entries.each do |path, record|
      %w[uid mode changed].each do |field|
        assert_nil(record.fetch(field), "FAT entry #{path} has no #{field} metadata")
      end
    end
    info = completed(invoke(arguments: ["i", "--json", "-f", "posix.img"]))[1]
    assert_nil(info.fetch("uuid"), "info uuid nil")
    refute_includes(invoke(arguments: ["i", "-f", "posix.img"]).stdout, "UUID".b, "invoke arguments i f posix img stdout")
    linked = (root / "linked-source")
    linked.mkdir
    (linked / "target").binwrite("data".b)
    File.symlink("target", (linked / "link"))
    # Unconfined, the scan imports the link and planning rejects its kind.
    error = failed(
      invoke(
        arguments: [
          "c",
          "-n",
          "--json",
          "-S",
          "off",
          "-F",
          "fat16",
          "-C",
          "linked-source",
          "."
        ],
        expected: 1
      )
    )
    assert_equal(-6, error.fetch("code"), "error code")
    assert_equal("/link", error.fetch("path"), "error path")
    assert_includes(error.fetch("message"), "entry kind", "error message")
    human = invoke(
      arguments: [
        "c",
        "-n",
        "-S",
        "off",
        "-F",
        "fat16",
        "-C",
        "linked-source",
        "."
      ],
      expected: 1
    )
    assert_includes(human.stderr, "cannot store this entry kind".b, "human stderr")
    # A confined worker may be denied link inspection; either way the
    # failure is unsupported and names the link.
    error = failed(
      invoke(
        arguments: [
          "c",
          "-n",
          "--json",
          "-F",
          "fat16",
          "-C",
          "linked-source",
          "."
        ],
        expected: 1
      )
    )
    assert_equal(-6, error.fetch("code"), "error code")
    assert_equal("/link", error.fetch("path"), "error path")
    assert_match(/entry kind|link inspection is not permitted/, error.fetch("message"))
    skipped = completed(invoke(arguments: ["x", "--json", "-k", "-f", "posix.img"]))
    assert_equal("0", skipped[-1].fetch("skipped_entries"), "the final extraction record reports no skipped entries")
    assert_equal("x", skipped[-1].fetch("command"), "the final extraction record identifies the extract command")
    assert_equal("hello\n".b, (root / "posix/HELLO.TXT").binread)
    invoke(arguments: ["t", "-k", "-f", "posix.img"], expected: 2)
  end
end
