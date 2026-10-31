# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_container_creation_streams_reproducibly_and_refuses_terminals
    executable = @executable
    root = @root
    # c writes each container; c -f - streams the same bytes to stdout and
    # refuses a terminal; -a accepts a container suffix naming the stack's
    # container; cW verifies the container before publishing; without -R
    # the container identifier is random and differs between runs.
    ["dmg", "vhd", "simg"].each do |container|
      image = "made.#{container}"
      invoke(
        arguments: [
          "cf",
          image,
          "-R",
          "-a",
          "-F",
          "fat12/gpt/#{container}",
          "-C",
          "source",
          "."
        ]
      )
      assert_equal(
        ["\"/HELLO.TXT\"", "\"/nested\"", "\"/nested/caf\u00e9.txt\""],
        listing(image: image).sort,
        image
      )
      streamed = invoke(
        arguments: [
          "cf",
          "-",
          "-R",
          "-F",
          "fat12/gpt/#{container}",
          "-C",
          "source",
          "."
        ]
      )
      assert_equal((root / image).binread, streamed.stdout, container)
      refute_includes(streamed.stderr, "output is incomplete".b, "streamed stderr")
      verified = invoke(arguments: ["W", "-d", "-f", image]).stdout
      assert_includes(verified, "#{container} container,".encode(Encoding::UTF_8).b, verified)
    end

    plan = records(
      invoke(
        arguments: [
          "c",
          "-n",
          "--json",
          "-R",
          "-F",
          "fat12/dmg",
          "-C",
          "source",
          "."
        ]
      )
    )
    planned = plan.find { |record| record.fetch("type") == "plan" }
    assert(planned.fetch("codec").start_with?("zlib "), "planned codec")
    assert(Integer(planned.fetch("output_bytes"), 10) > 0, "Integer planned output bytes 10")
    # A read-only container is refused as unsupported before its keys are
    # read: neither an unknown key nor a malformed value is diagnosed.
    result = invoke(
      arguments: [
        "cf",
        "refused.ifs",
        "-F",
        "ifs/hbcifs",
        "-o",
        "hbcifs:anything=garbage",
        "-C",
        "source",
        "."
      ],
      expected: 1
    )
    assert_includes(result.stderr, "the container is read only \"hbcifs\"".b, result.stderr)
    refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    invoke(arguments: ["cWf", "checked.dmg", "-F", "fat12/gpt/dmg", "-C", "source", "."])
    invoke(arguments: ["cf", "random.dmg", "-F", "fat12/dmg", "-C", "source", "."])
    first = (root / "checked.dmg").binread[(-512 + 64)...(-512 + 80)]
    refute_equal(first, (root / "random.dmg").binread[(-512 + 64)...(-512 + 80)], "separate DMG creations receive different segment IDs")
    # A terminal on stdout never receives an image, and nothing is left.
    primary, secondary = PTY.open
    begin
      result = run_process(
        [executable.to_s, "cf", "-", "-F", "fat12/dmg", "-C", "source", "."],
        cwd: root,
        stdout: secondary,
        stderr: :capture,
        timeout: 30
      )
    ensure
      secondary.close
      primary.close
    end

    assert_equal(1, result.status, "result status")
    assert_includes(result.stderr, "refusing to write an image to a terminal".b, "result stderr")
    ["made.dmg", "made.vhd", "made.simg", "checked.dmg", "random.dmg"].each do |name|
      (root / name).unlink
    end
  end
end
