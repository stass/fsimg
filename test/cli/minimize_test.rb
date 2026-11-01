# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_minimize_creates_smaller_verified_images_and_reports_geometry
    # These are the C tests' catalogs: two aligned files, with table savings
    # exceeding slack. FAT's type floors and QNX6's reserve remain constraints.
    cases = [
      {format: "fat12", file_bytes: 1 << 20, default_bytes: 2_121_216,
       volume_bytes: 2_099_200, cluster_bytes: 8192, allocation_units: 256},
      {format: "fat16", file_bytes: 16 << 20, default_bytes: 33_703_424,
       volume_bytes: 33_589_248, cluster_bytes: 4096, allocation_units: 8192},
      {format: "fat32", file_bytes: 32 << 20, default_bytes: 68_175_360,
       volume_bytes: 67_655_680, cluster_bytes: 1024, allocation_units: 65_541},
      {format: "exfat", file_bytes: 4 << 20, default_bytes: 8_474_112,
       volume_bytes: 8_425_472, cluster_bytes: 4096, allocation_units: 2051},
      {format: "qnx6", file_bytes: 4 << 20, default_bytes: 9_060_352,
       volume_bytes: 8_695_808, cluster_bytes: 2048, allocation_units: 4238}
    ]

    cases.each do |expected|
      format = expected.fetch(:format)
      source = @root / "minimize-#{format}"
      source.mkdir
      # Stream a nonzero pattern; the last chunk ends at the pinned size.
      chunk = (0...65_536).map { _1 % 251 }.pack("C*")
      names = ["F0000000", "F0000001"]
      names.each do |name|
        File.open(source / name, "wb") do |file|
          (expected.fetch(:file_bytes) / chunk.bytesize).times { file.write(chunk) }
        end
      end
      operands = ["-C", source.basename, *names]
      common = ["-R", "-e", "1700000000", "-S", "off", "-F", format]
      default_image = "default-#{format}.img"
      image = "minimized-#{format}.img"
      completed(invoke(arguments: ["c", "--json", *common, "-f", default_image, *operands]))
      assert_equal(expected.fetch(:default_bytes), (@root / default_image).size, format)

      planned = completed(invoke(arguments: ["c", "-n", "-M", "--json", *common, *operands]))
        .find { _1.fetch("type") == "plan" }
      assert_equal(format, planned.fetch("format"))
      assert_equal(expected.fetch(:cluster_bytes), planned.fetch("cluster_bytes"), format)
      assert_equal(expected.fetch(:volume_bytes).to_s, planned.fetch("volume_bytes"), format)
      assert_equal(expected.fetch(:allocation_units).to_s, planned.fetch("allocation_units"), format)
      assert_equal(format == "qnx6" ? "16" : "0", planned.fetch("inode_capacity"), format)
      assert_equal(format == "qnx6" ? "12" : "0", planned.fetch("free_inodes"), format)

      created = completed(invoke(arguments: ["c", "-M", "--json", *common, "-f", image, *operands]))
        .find { _1.fetch("type") == "plan" }
      assert_equal(planned, created, "dry run and emitted geometry: #{format}")
      assert_equal(expected.fetch(:volume_bytes), (@root / image).size, format)
      assert_operator((@root / image).size, :<, (@root / default_image).size, format)
      completed(invoke(arguments: ["W", "-d", "--json", "-f", image]))
      info = completed(invoke(arguments: ["i", "--json", "-f", image]))
        .find { _1.fetch("type") == "info" }
      assert_equal(planned.fetch("allocation_units"), info.fetch("allocation_units"), format)
      names.each do |name|
        assert_equal((source / name).binread,
          invoke(arguments: ["x", "-O", "-f", image, "/#{name}"]).stdout, format)
      end

      human = invoke(arguments: ["c", "-n", "-M", *common, *operands]).stdout
      assert_includes(human, "Clusters:     #{expected.fetch(:allocation_units)}\n".b, format)
      if format == "qnx6"
        assert_includes(human, "Inodes:       16 (12 free)\n".b)
      else
        refute_includes(human, "Inodes:".b, format)
      end

      # Reverse insertion order and use a different memory budget.
      second = "minimized-second-#{format}.img"
      completed(invoke(arguments: ["c", "-M", "--json", *common, "-m", "16M", "-f", second,
        "-C", source.basename, *names.reverse]))
      same_bytes(first: @root / image, second: @root / second)
    end
  end

  def test_minimize_rejects_fixed_disk_sizes_without_publishing
    %w[mbr gpt].each do |scheme|
      image = "minimize-#{scheme}.img"
      result = invoke(arguments: ["c", "-M", "-R", "--json", "-F", "fat16/#{scheme}",
        "-o", "#{scheme}:disk=64M", "-f", image, "-C", "source", "."], expected: 1)
      error = failed(result)
      assert_equal(-1, error.fetch("code"))
      assert_includes(error.fetch("message"), "minimization conflicts with a fixed disk size")
      refute((@root / image).exist?, "a conflicting fixed disk must publish nothing")
    end
  end
end
