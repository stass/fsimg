# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_formats_report_capabilities_and_settings

    decoded = completed(invoke(arguments: ["--formats", "--json"]))
    assert_equal("formats", decoded[-1].fetch("command"))
    formats = filesystems(decoded)
    assert_equal(
      [
        "fat12",
        "fat16",
        "fat32",
        "exfat",
        "qnx6",
        "qnx4",
        "ifs"
      ].to_set,
      formats.keys.to_set
    )
    layers = decoded
      .select { |record| (record.fetch("type") == "format" && (record.fetch("kind") != "filesystem")) }
      .map { |record| [record.fetch("name"), record] }
      .to_h
    assert_equal(
      {
        "gpt" => "scheme",
        "mbr" => "scheme",
        "dmg" => "container",
        "vhd" => "container",
        "simg" => "container",
        "ifsboot" => "container",
        "hbcifs" => "container",
        "gzip" => "compression",
        "zstd" => "compression",
        "xz" => "compression"
      },
      layers.to_a.map { |(name, record)| [name, record.fetch("kind")] }.to_h
    )
    assert_equal(["sector", "disk", "type", "name", "disk-guid", "guid"], layers.fetch("gpt").fetch("settings"))
    assert_equal(["disk", "type", "active", "id"], layers.fetch("mbr").fetch("settings"))
    assert_equal(["compress", "level", "chunk", "uuid"], layers.fetch("dmg").fetch("settings"))
    assert_equal(["block", "crc"], layers.fetch("simg").fetch("settings"))
    assert_equal(["level"], layers.fetch("gzip").fetch("settings"))
    assert_equal(7, layers.fetch("gpt").fetch("capabilities"), "layers gpt capabilities")
    assert_equal(7, layers.fetch("vhd").fetch("capabilities"), "layers vhd capabilities")
    # ifsboot is read, written and verified, lists its file settings first,
    # and needs no zlib.
    assert_equal(7, layers.fetch("ifsboot").fetch("capabilities"), "layers ifsboot capabilities")
    assert_equal([
            "startup",
            "prefix",
            "machine",
            "virtual",
            "startup_vaddr",
            "paddr_bias",
            "image_paddr",
            "ram_paddr",
            "imagefs_paddr",
            "compress"
          ], layers.fetch("ifsboot").fetch("settings"), "layers ifsboot settings")
    assert_nil(layers.fetch("ifsboot").fetch("suffix"), "layers ifsboot suffix nil")
    assert_nil(layers.fetch("ifsboot").fetch("codec_version"), "layers ifsboot codec version nil")
    assert_equal(5, layers.fetch("hbcifs").fetch("capabilities"), "layers hbcifs capabilities")
    assert_nil(layers.fetch("hbcifs").fetch("suffix"), "layers hbcifs suffix nil")
    assert_equal(7, layers.fetch("gzip").fetch("capabilities"), "layers gzip capabilities")
    assert_equal(".zst", layers.fetch("zstd").fetch("suffix"), "layers zstd suffix")
    assert_equal(layers.fetch("dmg").fetch("codec_version"), layers.fetch("gzip").fetch("codec_version"))
    assert_equal(
      {
        "gpt" => nil,
        "mbr" => nil,
        "dmg" => "raw,zero,zlib",
        "vhd" => "raw",
        "simg" => "raw,zero,fill",
        "ifsboot" => "raw,lzo1x,nrv2b",
        "hbcifs" => "lzo1x",
        "gzip" => nil,
        "zstd" => nil,
        "xz" => nil
      },
      layers.to_a.map { |(name, record)| [name, record.fetch("codecs")] }.to_h
    )
    assert_match(/\Azlib [0-9][0-9A-Za-z.-]*\z/,
      layers.fetch("dmg").fetch("codec_version"), "DMG reports its zlib codec version")
    assert_nil(layers.fetch("vhd").fetch("codec_version"), "layers vhd codec version nil")
    assert_equal(".vhd", layers.fetch("vhd").fetch("suffix"), "layers vhd suffix")
    assert_nil(layers.fetch("gpt").fetch("suffix"), "layers gpt suffix nil")
    assert(formats.values.all? { |record| record.fetch("capabilities") == 7 }, "all filesystems support read, write and verify: #{formats.transform_values { _1.fetch("capabilities") }.inspect}")
    assert_equal(((32 * 1024) * 1024), formats.fetch("exfat").fetch("write_cluster_bytes")[-1])
    assert(formats.fetch("exfat").fetch("max_file_bytes").is_a?(String), "formats exfat max file bytes is a String")
    capabilities = decoded.find { |record| record.fetch("type") == "capabilities" }
    assert_includes(["unavailable", "not-probed"].to_set, capabilities.fetch("sandbox_runtime"), "unavailable not probed to set")
    assert_equal(false, capabilities.fetch("source_device_open_exclusion"), "capabilities source device open exclusion")
    assert_includes(invoke(arguments: ["-h"]).stdout, "usage: fsimg".b, "invoke arguments h stdout")
    assert(invoke(arguments: ["-V"]).stdout.start_with?("fsimg ".b), "invoke arguments V stdout")
  end
end
