# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_compressed_create_verification_spool_limit_leaves_nothing_published
    coordinator_source
    scratch = private_directory(root: @root, name: "scratch")
    original_children = @root.children.sort
    arguments = [
      "cWf", "out.img.gz", "-T", scratch.to_s,
      "-F", "fat16", "-z", "-s", "4M"
    ]

    refused = invoke(
      arguments: [*arguments, "-l", "spool=1M", "-C", "src", "."],
      expected: 1
    )

    assert_includes(
      refused.stderr, "spool limit 1048576, observed 4194304".b
    )
    refute((@root / "out.img.gz").exist?, "failed cW must not publish")
    assert_empty(scratch.children, "failed cW must remove scratch")
    assert_equal(
      original_children, @root.children.sort,
      "failed cW must remove staging beside the destination"
    )

    invoke(arguments: [*arguments, "-C", "src", "."])

    assert_empty(scratch.children, "successful cW must remove scratch")
    assert((@root / "out.img.gz").file?, "successful cW must publish")
    verified = invoke(arguments: ["Wf", "out.img.gz", "-T", scratch.to_s])
    assert_includes(verified.stdout, "filesystem structure verified".b)
    assert_empty(scratch.children, "verification must remove scratch")
    listed = invoke(arguments: ["tf", "out.img.gz", "-T", scratch.to_s])
    assert_equal("\"/hello.txt\"\n".b, listed.stdout)
    assert_empty(scratch.children, "listing must remove scratch")
  end

  def test_fixed_vhd_lists_with_forced_container_or_forced_bare_volume
    coordinator_source
    scratch = private_directory(root: @root, name: "scratch")
    invoke(arguments: [
      "cf", "disk.vhd", "-T", scratch.to_s,
      "-F", "fat16/-/vhd", "-s", "4M",
      "-C", "src", "."
    ])
    assert_empty(scratch.children, "creation must remove scratch")

    # Emptying the container slot reads FAT directly; the fixed VHD's footer
    # lies after the volume and does not prevent reading it as a bare volume.
    ["fat16/-/vhd", "fat16/-/-"].each do |stack|
      result = invoke(arguments: ["tf", "disk.vhd", "-T", scratch.to_s, "-F", stack])

      assert_equal("\"/hello.txt\"\n".b, result.stdout, stack)
      assert_empty(scratch.children, "listing must remove scratch: #{stack}")
    end
  end

  def test_gzip_lists_with_forced_compression_or_forced_bare_volume
    coordinator_source
    scratch = private_directory(root: @root, name: "scratch")
    invoke(arguments: [
      "cf", "img.gz", "-T", scratch.to_s,
      "-F", "fat16", "-z", "-s", "4M",
      "-C", "src", "."
    ])
    assert_empty(scratch.children, "creation must remove scratch")

    [["-z"], ["-F", "fat16/-/-"]].each do |forcing|
      result = invoke(arguments: ["tf", "img.gz", "-T", scratch.to_s, *forcing])

      assert_equal("\"/hello.txt\"\n".b, result.stdout, forcing.inspect)
      assert_empty(scratch.children, "listing must remove scratch: #{forcing.inspect}")
    end
  end

  def test_silent_fifo_reports_deadline_without_waiting_for_stdin_eof
    scratch = private_directory(root: @root, name: "scratch")
    fifo = @root / "silent-input"
    File.mkfifo(fifo)

    # Open the reader without waiting for a writer, then restore blocking
    # reads. Keep the writer open until fsimg exits so it never receives EOF.
    File.open(fifo, File::RDONLY | File::NONBLOCK) do |reader|
      File.open(fifo, File::WRONLY | File::NONBLOCK) do |_writer|
        reader.nonblock = false
        result = invoke(
          arguments: ["tf", "-", "-T", scratch.to_s, "-l", "seconds=1"],
          stdin: reader, expected: 1, timeout: 10
        )

        assert_empty(result.stdout)
        assert_includes(result.stderr, "worker cancelled or deadline exceeded".b)
        refute_includes(
          result.stderr, "child exit and channel closure did not complete".b
        )
        assert_empty(scratch.children, "deadline cancellation must remove scratch")
      end
    end
  end

  private

  def coordinator_source
    (@root / "src").mkdir
    (@root / "src/hello.txt").binwrite("hello\n".b)
  end
end
