# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "io/nonblock"
require "timeout"

class CliTest < Minitest::Test
  class Error < StandardError
  end
end

module CliProcesses
  Result = Data.define(:stdout, :stderr, :status)

  # Own a child's capture pipes until it exits. Readers drain both pipes while
  # the caller can feed stdin or signal a child blocked on its external stdout.
  class Child
    attr_reader :pid, :status

    def initialize(
      arguments:,
      cwd: Dir.pwd,
      stdin: File::NULL,
      stdout: :capture,
      stderr: :capture,
      close_stdin: false,
      nonblocking_stdin: false,
      ignore_pipe: false,
      file_size_limit: nil
    )
      unless arguments.is_a?(Array) && !arguments.empty?
        raise ArgumentError, "command must be a nonempty argv array"
      end
      # Redirecting a standard descriptor clears O_NONBLOCK in Ruby. Require
      # the caller's intended mode; changing it here also changes their IO.
      if stdin.respond_to?(:nonblock?) && stdin.nonblock? != nonblocking_stdin
        raise ArgumentError, "stdin mode must match nonblocking_stdin"
      end
      output, output_writer = IO.pipe if stdout == :capture
      errors, error_writer = IO.pipe if stderr == :capture
      options = spawn_options(
        cwd: cwd, stdin: stdin, stdout: output_writer || stdout,
        stderr: error_writer || stderr, close_stdin: close_stdin
      )
      command = arguments.map(&:to_s)
      if nonblocking_stdin
        # Ruby clears O_NONBLOCK on redirected standard descriptors. A spare
        # descriptor survives that normalization; POSIX shell moves it to 0
        # and closes it before exec. Arguments stay separate through "$@".
        options[3] = options.delete(:in)
        options[:in] = File::NULL
        command = ["/bin/sh", "-c", "exec 0<&3; exec 3<&-; exec \"$@\"", "fsimg-cli", *command]
      end

      # An executable pair prevents Ruby's one-string shell spawn form.
      command[0] = [command.first, command.first]
      @pid = if ignore_pipe || file_size_limit
        fork do
          Process.setpgid(0, 0)
          Signal.trap("PIPE", "IGNORE") if ignore_pipe
          if file_size_limit
            Signal.trap("XFSZ", "IGNORE")
            Process.setrlimit(:FSIZE, file_size_limit, file_size_limit)
          end

          begin
            exec(*command, **options)
          rescue SystemCallError
            exit!(127)
          end
        end
      else
        Process.spawn(*command, **options, pgroup: true)
      end

      @readers = [output, errors].map do |stream|
        stream &&
          Thread.new do
            begin
              stream.read.b
            ensure
              stream.close
            end
          end
      end

    rescue SystemCallError
      output&.close
      errors&.close
      raise
    ensure
      output_writer&.close
      error_writer&.close
    end

    def finish(timeout: 30)
      finished = false
      Timeout.timeout(timeout) do
        process_status = Process.wait2(pid).last
        @status = process_status.exitstatus || -process_status.termsig
        captured = @readers.map { _1 ? _1.value : "".b }
        finished = true
        captured
      end

    ensure
      unless finished
        begin
          Process.kill("KILL", -pid)
        rescue Errno::ESRCH
          # The process group may exit between the timeout and kill.
        end

        Process.wait(pid) if @status.nil?
        @readers.each { _1&.kill }
      end

      @readers.each { _1&.join }
    end

    private

    def spawn_options(cwd:, stdin:, stdout:, stderr:, close_stdin:)
      {
        chdir: cwd.to_s,
        in: close_stdin ? :close : stdin,
        out: stdout,
        err: stderr,
        close_others: true
      }
    end

  end

  private

  def start_process(arguments, **options) = Child.new(arguments: arguments, **options)

  def run_process(arguments, timeout: 30, **options)
    child = start_process(arguments, **options)
    output, errors = child.finish(timeout: timeout)
    Result.new(stdout: output, stderr: errors, status: child.status)
  end

  def invoke(executable: @executable, root: @root, arguments:, expected: 0, **options)
    result = run_process([executable, *arguments], cwd: root, **options)
    unless expected.nil?
      assert_equal(expected, result.status, [arguments, result.status, result.stderr].inspect)
    end
    result
  end

end
