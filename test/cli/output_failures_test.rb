# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_output_failures_report_broken_pipes_and_cancel_blocked_writes
    exfat_image
    reader, writer = IO.pipe
    reader.close
    begin
      result = run_process(
        [
          @executable,
          "x",
          "-O",
          "-f",
          "exfat.img",
          "/nested/caf\u00e9.txt"
        ],
        cwd: @root,
        stdout: writer,
        timeout: 15
      )
      assert_equal(1, result.status)
      assert_includes(result.stderr, "output write failed: Broken pipe")
    ensure
      writer.close
    end

    # Fill the pipe before starting the producer so its first write blocks.
    reader, writer = IO.pipe
    begin
      begin
        loop { writer.write_nonblock("\0".b * 4096) }
      rescue IO::WaitWritable
        # The pipe has no remaining capacity.
      end

      writer.nonblock = false
      child = start_process(
        [@executable, "--formats", "--json"],
        cwd: @root,
        stdout: writer
      )
      sleep(0.2)
      Process.kill("INT", child.pid)
      _, diagnostic = child.finish(timeout: 10)
      assert_equal(1, child.status)
      assert_includes(diagnostic, "cancelled")
    ensure
      reader.close
      writer.close
    end
  end
end
