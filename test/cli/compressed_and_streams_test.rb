# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_compressed_streams_handle_relays_terminals_and_limits
    partition_fixtures
    disk = (@root / "gpt.img").binread
    # Unallocated sectors carry an incompressible chunk to exercise raw runs
    # alongside the zero and zlib runs in the three-region stdin image.
    disk[(6144 * 512)...(6400 * 512)] = Random.new(7).bytes(256 * 512)
    (@root / "gpt.dmg").binwrite(
      dmg_image(disk: disk, regions: [[0, 2048], [2048, 2048], [4096, 4096]])
    )
    executable = @executable
    root = @root
    # t, x, i and W read standard input through the coordinator's relay,
    # raw, DMG and gzip alike, and gzip files in place, the relayed stream
    # counting against fds; a terminal on stdin is refused; -z on a raw file
    # is an absent signature; a raw stream that also passes for gzip is
    # ambiguous until -z or -F resolves it; -T - leaves no spool for a gzip
    # file; and a spool limit stops the decode.
    listed = error = stack = result = nil
    names = listing(image: "gpt.img")
    disk = (root / "gpt.img").binread
    (root / "gpt.img.gz").binwrite(gzip_bytes(disk, mtime: 0))
    ["gpt.img", "gpt.dmg", "gpt.img.gz"].each do |image|
      (root / image).open("rb") do |stream|
        listed = invoke(arguments: ["tf", "-"], stdin: stream)
      end

      assert_equal(
        names,
        listed.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true),
        image
      )
    end
    # The worker charges the relayed stream like any descriptor it holds.
    (root / "gpt.img").open("rb") do |stream|
      error = failed(
        invoke(
          arguments: ["tf", "-", "--json", "-l", "fds=2"],
          expected: 1,
          stdin: stream
        )
      )
    end

    assert_equal("fds", error.fetch("resource"), error)
    assert_equal("2", error.fetch("limit"), error)
    (root / "gpt.img").open("rb") do |stream|
      invoke(arguments: ["tf", "-", "-l", "fds=3"], stdin: stream)
    end

    assert_equal(names, listing(image: "gpt.img.gz"))
    info = invoke(arguments: ["i", "-f", "gpt.img.gz"]).stdout.dup.force_encoding(Encoding::UTF_8)
    assert(info.start_with?("Stack:        fat12/gpt\nCompression:  gzip\n"), info)
    (root / "gpt.img.gz").open("rb") do |stream|
      stack = completed(
        invoke(
          arguments: [
            "i",
            "--json",
            "-f",
            "-"
          ],
          stdin: stream
        )
      )
        .find { |record| record.fetch("type") == "stack" }
    end

    assert_equal("gzip", stack.fetch("compression"), "stack compression")
    assert_equal("fat12/gpt", stack.fetch("stack"), "stack stack")
    invoke(arguments: ["Wf", "gpt.img.gz"])
    private_directory(root: root, name: "from-stdin")
    (root / "gpt.img.gz").open("rb") do |stream|
      invoke(arguments: ["xf", "-", "-E", "-C", "from-stdin"], stdin: stream)
    end

    assert_equal("hello\n".b, (root / "from-stdin/HELLO.TXT").binread)
    leader, follower = PTY.open
    begin
      result = invoke(arguments: ["tf", "-"], expected: 1, stdin: follower)
    ensure
      leader.close
      follower.close
    end

    assert_includes(result.stderr, "refusing to read an image from a terminal".b, result.stderr)
    # A standard input the relay cannot read fails as that, not as the
    # verdict the worker reaches on the bytes it got before the early end:
    # a closed descriptor, which the coordinator holds with an unreadable
    # placeholder so no descriptor of its own takes its place, and a
    # directory.

    directory = File.open(root, "r")
    begin
      result = invoke(arguments: ["tf", "-"], expected: 1, stdin: directory)
    ensure
      directory.close
    end

    assert_includes(result.stderr, "standard input could not be read".b, result.stderr)
    # A non-blocking standard input with no data yet is waited for.
    reader, writer = IO.pipe
    reader.nonblock = true
    process = start_process(
      [executable.to_s, "tf", "-"],
      cwd: root,
      stdin: reader,
      nonblocking_stdin: true,
      stdout: :capture,
      stderr: :capture
    )
    reader.close
    sleep(0.5)
    writer.write((root / "gpt.img.gz").binread)

    writer.close
    output, errors = process.finish(timeout: 30)
    assert_equal(0, process.status, errors)
    assert_equal(names, output.dup.force_encoding(Encoding::UTF_8).lines(chomp: true))
    result = invoke(arguments: ["t", "-z", "-f", "gpt.img"], expected: 1)
    assert_includes(result.stderr, "no gzip signature".b, result.stderr)
    # gzip's identity in the protective MBR's boot code.
    (root / "ambiguous.img").binwrite("\x1f\x8b\b\x00".b + disk[4..])
    [
      [[], 1, "passes for both a compressed and an uncompressed image".b],
      [["-F", "fat12"], 0, "".b],
      [["-z"], 1, "DEFLATE data is invalid".b]
    ].each do |arguments, expected, detail|
      (root / "ambiguous.img").open("rb") do |stream|
        result = invoke(arguments: ["t", *arguments, "-f", "-"], expected: expected, stdin: stream)
      end

      assert_includes(result.stderr, detail, [arguments, result.stderr])
      if expected == 0
        assert_equal(names, result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true))
      end
    end

    result = invoke(arguments: ["tf", "gpt.img.gz", "-T", "-"], expected: 1)
    assert_includes(result.stderr, "-T - disables".b, result.stderr)
    result = invoke(arguments: ["tf", "gpt.img.gz", "-l", "spool=1M"], expected: 1)
    assert_includes(result.stderr, "spool limit 1048576".b, result.stderr)
  end
  # The AddressSanitizer runtime opens a socket before main(), which takes
  # descriptor 0 when it is closed, so the closed case is unobservable
  # there: the coordinator correctly relays that socket as the input.
  def test_closed_standard_input_reports_a_relay_read_failure
    skip("sanitizer runtime takes descriptor 0 before main") if ENV.include?("ASAN_OPTIONS")
    human_image
    result = invoke(arguments: ["tf", "-"], expected: 1, close_stdin: true)
    assert_includes(result.stderr, "standard input could not be read".b)
  end
end
