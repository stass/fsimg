# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliHelpersTest < Minitest::Test
  include CliFixtures
  include CliImages
  include CliRecords

  def test_long_listing_rejects_extra_text_around_a_valid_row
    row = format("%s %12s %-19s %s", "-rw-r-----", "6", "-", "\"/hello\"")
    result = CliProcesses::Result.new(stdout: row + "\n", stderr: "".b, status: 0)
    assert_equal(["-", "rw-r-----", "6", "-"], long_listing(result).fetch("\"/hello\""))
    ["extra #{row}", "#{row}extra"].each do |invalid|
      result = CliProcesses::Result.new(stdout: invalid + "\n", stderr: "".b, status: 0)
      assert_raises(Minitest::Assertion) { long_listing(result) }
    end
  end

  def test_word_checksum_rejects_an_incomplete_final_word
    assert_raises(CliTest::Error) { word_sum("\0".b * 4 + "X".b) }
  end

  def test_gunzip_decodes_every_concatenated_member
    assert_equal("firstsecond".b, gunzip(gzip_bytes("first".b) + gzip_bytes("second".b)))
    assert_equal("firstsecond".b, gunzip(gzip_bytes("first".b) + "\0".b * 3 + gzip_bytes("second".b) + "\0".b))
  end

  def test_gunzip_rejects_nonzero_trailing_garbage
    assert_raises(CliTest::Error) { gunzip(gzip_bytes("first".b) + "garbage".b) }
  end

end

class CliProcessesTest < Minitest::Test
  def test_capture_deadline_applies_when_an_exited_child_leaves_a_pipe_open
    Dir.mktmpdir("cli-capture-", Dir.pwd) do |root|
      ready = File.join(root, "ready")
      program = <<~RUBY
        parent = Process.pid
        fork do
          sleep 0.01 while Process.ppid == parent
          File.write(ARGV.fetch(0), 'ready')
          sleep 3
        end
        exit!
      RUBY
      child = CliProcesses::Child.new(arguments: [RbConfig.ruby, "-e", program, ready], cwd: root)
      # Readiness is signaled only after the parent exits. The deadline must
      # therefore expire in capture drainage, independently of Ruby startup.
      Timeout.timeout(5) { sleep(0.01) until File.file?(ready) }
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      assert_raises(Timeout::Error) { child.finish(timeout: 0.1) }
      assert_operator(Process.clock_gettime(Process::CLOCK_MONOTONIC) - started, :<, 2)
    end
  end

  def test_child_preserves_nonblocking_standard_input
    reader, writer = IO.pipe
    begin
      reader.nonblock = true
      child = CliProcesses::Child.new(
        arguments: ["/bin/cat"],
        cwd: Dir.pwd,
        stdin: reader,
        nonblocking_stdin: true
      )
      # An empty nonblocking pipe reports EAGAIN immediately. A blocking
      # pipe would wait for the writer, which remains open until cleanup.
      output, errors = child.finish(timeout: 1)
      assert_equal(1, child.status, errors)
      assert_empty(output)
      refute_empty(errors)
    ensure
      reader.close
      writer.close
    end
  end

  def test_single_string_command_is_rejected
    assert_raises(ArgumentError) { CliProcesses::Child.new(arguments: "echo unsafe") }
    child = CliProcesses::Child.new(arguments: ["/no/such/program; echo unsafe"], ignore_pipe: true)
    output, = child.finish
    assert_equal(127, child.status)
    assert_empty(output)
  end

  def test_missing_binary_in_fork_exits_127
    child = CliProcesses::Child.new(arguments: ["/no/such/fsimg"], ignore_pipe: true)
    child.finish
    assert_equal(127, child.status)
  end

  def test_child_rejects_a_stdin_mode_mismatch_without_changing_the_io
    reader, writer = IO.pipe
    reader.nonblock = true
    assert_raises(ArgumentError) do
      CliProcesses::Child.new(arguments: [RbConfig.ruby, "-e", "exit"], stdin: reader).finish
    end
    assert(reader.nonblock?, "a rejected child leaves the caller's stdin nonblocking")

    reader.nonblock = false
    assert_raises(ArgumentError) do
      CliProcesses::Child.new(arguments: [RbConfig.ruby, "-e", "exit"], stdin: reader, nonblocking_stdin: true).finish
    end
    refute(reader.nonblock?, "a rejected child leaves the caller's stdin blocking")
  ensure
    reader.close
    writer.close
  end

  def test_child_preserves_blocking_standard_input
    reader, writer = IO.pipe
    reader.nonblock = false
    child = CliProcesses::Child.new(arguments: ["/bin/cat"], stdin: reader)
    refute(reader.nonblock?, "spawning a blocking child preserves the caller's stdin mode")
    reader.close
    writer.write("blocking pipe\n")
    writer.close
    output, errors = child.finish
    assert_equal(0, child.status, errors)
    assert_equal("blocking pipe\n", output)
  ensure
    reader.close unless reader.closed?
    writer.close unless writer.closed?
  end
end

class CliDiscoveryTest < Minitest::Test
  include CliProcesses

  def test_discovery_loads_tests_from_a_path_with_glob_metacharacters
    with_runner(test_source: <<~'TEST') do |result|
      class DiscoveryFixture < Minitest::Test
        def test_discovered
          assert_equal(7, 7)
        end
      end
    TEST
      assert_equal(0, result.status, result.stderr)
      assert_includes(result.stdout, "1 runs, 1 assertions")
    end
  end

  def test_discovery_rejects_a_directory_without_test_files
    with_runner(test_source: nil) do |result|
      assert_equal(1, result.status)
      assert_includes(result.stderr, "CLI suite found no test files")
    end
  end

  def test_discovery_rejects_test_files_without_test_methods
    with_runner(test_source: "# no test definitions\n") do |result|
      assert_equal(1, result.status)
      assert_includes(result.stderr, "CLI suite found no test methods")
    end
  end

  private

  def with_runner(test_source:)
    Dir.mktmpdir("cli-discovery-", Dir.pwd) do |temporary|
      directory = File.join(temporary, "fsimg[review]")
      Dir.mkdir(directory)
      %w[run.rb process.rb fixtures.rb images.rb records.rb].each do |name|
        FileUtils.cp(File.join(__dir__, name), directory)
      end
      File.write(File.join(directory, "discovery_test.rb"), test_source) if test_source
      yield run_process([RbConfig.ruby, File.join(directory, "run.rb"), "/bin/true"])
    end
  end
end
