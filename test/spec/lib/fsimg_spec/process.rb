# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # A process the harness started outlived SIGKILL. Nothing the run does
  # after it can be trusted to be isolated, so it ends the case phase
  # instead of failing one case. It derives from Exception, not Error, so
  # that no rescue of StandardError, such as the case phase's per-case
  # funnel, can take it for one case's fault; only the boundaries of a run
  # and of a replay name it.
  class ContainmentError < Exception; end

  # Told when an invocation's process group starts and when the
  # invocation is done with it; a test subclasses it to watch or disturb a
  # run. It does nothing by default, and nothing kills through it. Both
  # calls run inside the invocation's cleanup, so one that raises still
  # leaves the child stopped.
  class GroupObserver
    def started(_pgid) = nil

    def ended(_pgid) = nil
  end

  # One child process run under the runner's process lifetime
  # (specs/design-spec-testing.md#runner):
  #
  # 1. spawn it in a new process group, under resource limits, with a fixed
  #    environment and the case directory as its working directory;
  # 2. read stdout and stderr, each up to the capture cap, and write any
  #    input to stdin, while polling for the child by pid; never wait(-1),
  #    Open3 or Process.detach, which reap in helper threads;
  # 3. kill the child and its group when the wall clock or a cap is
  #    reached, then reap; a child alive GROUP_EXIT_SECONDS after the kill
  #    is a harness error;
  # 4. once the child is reaped, probe its group: a member that outlived the
  #    child is a leftover, recorded and killed;
  # 5. read both pipes to their end.
  #
  # A group with members keeps its ID allocated, so the step 4 kill cannot
  # reach a reused ID while a member lives. The residual race spans the
  # reap to the kill: if the group empties in that window, a new process
  # can take the ID as its group leader and receive the kill. Invocations
  # run one at a time, so the CPU time of the harness's reaped children
  # before and after a run measures that run, and any helper a test runs
  # beside it.
  #
  # SIGINT and SIGTERM reach the harness as Ruby's own Interrupt and
  # SignalException, which Ruby raises in the main thread, where stages
  # run. They wait while a child starts and arrive only while supervise
  # runs, which they unwind, stopping the child. The harness installs no
  # Signal.trap: a handler runs even while a child starts, where no cleanup
  # can yet reach it.
  module Invocation
    POLL_SECONDS = 0.05
    READ_BYTES = 1 << 16
    # How long reading the pipes to their end may take once the group is
    # gone; only a process that escaped the group with setsid can hold them.
    DRAIN_SECONDS = 5
    # How long a killed child or group may take to disappear.
    GROUP_EXIT_SECONDS = 5

    # wall_seconds bounds the run and cpu_seconds its CPU time
    # (RLIMIT_CPU). file_bytes is RLIMIT_FSIZE. address_space_bytes is
    # RLIMIT_AS, or nil where the host does not enforce it or the binary
    # cannot start under it. capture_bytes caps each of stdout and stderr.
    Limits = Data.define(:wall_seconds, :cpu_seconds, :file_bytes, :address_space_bytes, :capture_bytes)

    # What one run did. exit_status and signal come from the reaped child;
    # ended_by is nil, "deadline" or "capture_cap" when the runner killed
    # the group, so a signal it sent is never read as a crash. leftover says
    # a group member outlived the child. launch_error holds the message of a
    # spawn that failed, and every other field is then empty.
    Execution = Data.define(:exit_status, :signal, :stdout, :stderr, :wall_seconds, :cpu_seconds,
                            :ended_by, :leftover, :launch_error, :address_space_applied)

    # input is nil, which gives the child an empty stdin, or an Enumerator
    # of the binary chunks its stdin reads, written as the pipe takes them.
    def self.run(argv, chdir:, env:, limits:, observer: GroupObserver.new, input: nil)
      started = now
      cpu_before = children_cpu
      IO.pipe(binmode: true) do |out_reader, out_writer|
        IO.pipe(binmode: true) do |err_reader, err_writer|
          IO.pipe(binmode: true) do |in_reader, in_writer|
            # Interrupts wait while the child starts and arrive only during
            # supervise, inside stopped_on_unwind, so no started child
            # escapes its cleanup.
            Thread.handle_interrupt(Object => :never) do
              pid = begin
                spawn(argv, chdir:, env:, limits:, stdin: input ? in_reader : File::NULL, out: out_writer, err: err_writer)
              rescue SystemCallError => e
                return launch_failure(e.message, started:)
              end
              stopped_on_unwind(pid, observer:) do
                # The child holds its own ends now. Without input, the
                # stdin pipe is unused, so its writer closes too.
                [out_writer, err_writer, in_reader, *(input ? [] : [in_writer])].each(&:close)
                feed = input && Feed.new(in_writer, input)
                Thread.handle_interrupt(Object => :immediate) do
                  supervise(pid, readers: [out_reader, err_reader], feed:, limits:, started:, cpu_before:)
                end
              end
            end
          end
        end
      end
    end

    # A child's stdin, written from an Enumerator of binary chunks as the
    # pipe takes them, so that writing never blocks supervision. The pipe
    # closes once the chunks run out, which the child reads as the end of
    # its input, or once the child stops reading, as a tool that failed
    # may; its exit status reports that. The pending chunk and the pipe are
    # the feed's own state.
    class Feed
      def initialize(writer, chunks)
        @writer = writer
        @chunks = chunks
        @pending = "".b
      end

      # The pipe while input remains to be written, for IO.select, or nil.
      def io = @writer.closed? ? nil : @writer

      # Writes what the pipe takes of the pending chunk, taking the next
      # chunk once one is written.
      def write
        @pending = @chunks.next.b if @pending.empty?
        written = @writer.write_nonblock(@pending, exception: false)
        @pending = @pending.byteslice(written..) if written.is_a?(Integer)
      rescue StopIteration, Errno::EPIPE
        close
      end

      def close = @writer.closed? ? nil : @writer.close
    end

    # Tells observer the child pid started, then runs the block, which
    # supervises the child to its end. An exception from either, such as
    # an interrupt, stops the child, so no stage outlives the harness's
    # interest in it.
    private_class_method def self.stopped_on_unwind(pid, observer:)
      finished = false
      # A thread the observer starts takes on its mask; under :never it
      # could be neither interrupted nor killed.
      Thread.handle_interrupt(Object => :immediate) { observer.started(pid) }
      result = yield
      finished = true
      result
    ensure
      stop(pid) unless finished
      observer.ended(pid)
    end

    # Stops a child whose supervision ended early: kills it and its group,
    # then waits up to GROUP_EXIT_SECONDS until the child is reaped and the
    # group is gone. Past the bound it gives up, and the exception that
    # ended supervision goes on; a child that supervise itself found
    # surviving SIGKILL so costs the bound twice. An unreaped child keeps
    # its pid, so it is killed by pid as well, in case it left its group.
    # A child supervision already reaped is never signalled by pid, which
    # may name another process by now; its group is killed under the reuse
    # race described above.
    private_class_method def self.stop(pid)
      reap(pid) ? kill_group(pid) : kill_child(pid)
      deadline = now + GROUP_EXIT_SECONDS
      sleep(POLL_SECONDS / 10) until (reap(pid) && !group_alive?(pid)) || now >= deadline
    end

    # Reaps the child pid if it has exited, and answers whether it is
    # reaped, now or earlier.
    private_class_method def self.reap(pid)
      !Process.wait(pid, Process::WNOHANG).nil?
    rescue Errno::ECHILD
      true
    end

    # Kills the unreaped child pid and its group.
    private_class_method def self.kill_child(pid)
      kill_group(pid)
      Process.kill(:KILL, pid)
    rescue Errno::ESRCH, Errno::EPERM
      nil
    end

    # Kills every process of the group. ESRCH means the group is gone; on
    # macOS a group whose only member is an unreaped zombie answers EPERM,
    # and that member is already dead.
    private_class_method def self.kill_group(pgid)
      Process.kill(:KILL, -pgid)
    rescue Errno::ESRCH, Errno::EPERM
      nil
    end

    # Kills the group and waits until no member is left, so nothing of it
    # runs while the checks after the stage inspect the case directory.
    # SIGKILL cannot be caught; a member still present at the bound is a
    # harness error.
    private_class_method def self.kill_and_await_group(pgid)
      deadline = now + GROUP_EXIT_SECONDS
      kill_group(pgid)
      sleep(POLL_SECONDS / 10) while group_alive?(pgid) && now < deadline
      raise ContainmentError, "process group #{pgid} survived SIGKILL" if group_alive?(pgid)
    end

    # Whether any process is left in the group. EPERM means a member
    # exists that the harness may not signal, which still counts.
    def self.group_alive?(pgid)
      Process.kill(0, -pgid)
      true
    rescue Errno::ESRCH
      false
    rescue Errno::EPERM
      true
    end

    private_class_method def self.launch_failure(message, started:) =
      Execution.new(exit_status: nil, signal: nil, stdout: "".b, stderr: "".b, wall_seconds: now - started,
                    cpu_seconds: 0.0, ended_by: nil, leftover: false, launch_error: message,
                    address_space_applied: false)

    # The argv[0] pair keeps Process.spawn from handing a lone argument to
    # a shell.
    private_class_method def self.spawn(argv, chdir:, env:, limits:, stdin:, out:, err:)
      Process.spawn(env, [argv.first, argv.first], *argv.drop(1),
                    chdir:, unsetenv_others: true, pgroup: true, close_others: true,
                    in: stdin, out:, err:, **rlimits(limits))
    end

    private_class_method def self.rlimits(limits)
      fixed = { rlimit_core: 0, rlimit_cpu: limits.cpu_seconds, rlimit_fsize: limits.file_bytes }
      limits.address_space_bytes ? fixed.merge(rlimit_as: limits.address_space_bytes) : fixed
    end

    # feed is the child's Feed, or nil when its stdin is empty.
    private_class_method def self.supervise(pid, readers:, feed:, limits:, started:, cpu_before:)
      captures = readers.to_h { [_1, +"".b] }
      open_readers = readers.dup
      ended_by = nil
      killed_at = nil
      status = nil
      loop do
        read_available(captures, open_readers, cap: limits.capture_bytes, feed:)
        _, status = Process.wait2(pid, Process::WNOHANG)
        break if status
        raise ContainmentError, "process #{pid} survived SIGKILL" if killed_at && now - killed_at >= GROUP_EXIT_SECONDS
        next if ended_by

        # Checked only while the child runs, so a child that has already
        # exited is never reported as stopped.
        ended_by = stop_reason(captures, started:, limits:)
        next unless ended_by

        killed_at = now
        kill_child(pid)
      end
      feed&.close
      leftover = group_alive?(pid)
      kill_and_await_group(pid) if leftover
      drain(captures, open_readers, cap: limits.capture_bytes)
      # The last bytes written before the exit are read only now; a capture
      # they push past its cap is cut, and says so.
      ended_by ||= "capture_cap" if over_cap?(captures, limits.capture_bytes)

      Execution.new(exit_status: status.exitstatus, signal: status.termsig,
                    stdout: captures.fetch(readers.first), stderr: captures.fetch(readers.last),
                    wall_seconds: now - started, cpu_seconds: children_cpu - cpu_before,
                    ended_by:, leftover:, launch_error: nil,
                    address_space_applied: !limits.address_space_bytes.nil?)
    end

    # "deadline" or "capture_cap" once the run must be stopped, else nil.
    # A capture holds at most one byte past the cap, which marks the cap
    # as reached.
    private_class_method def self.stop_reason(captures, started:, limits:)
      if now - started >= limits.wall_seconds then "deadline"
      elsif over_cap?(captures, limits.capture_bytes) then "capture_cap"
      end
    end

    private_class_method def self.over_cap?(captures, cap) = captures.values.any? { _1.bytesize > cap }

    # Appends what the ready pipes hold to their captures and drops a pipe
    # from open_readers at its end. Both are the supervisor's own buffers,
    # updated in place as the pipes are read. A feed whose pipe has room is
    # written to.
    private_class_method def self.read_available(captures, open_readers, cap:, feed: nil, timeout: POLL_SECONDS)
      writers = [feed&.io].compact
      return sleep(timeout) if open_readers.empty? && writers.empty?

      ready, writable = IO.select(open_readers, writers, nil, timeout)
      feed.write unless Array(writable).empty?
      Array(ready).each do |reader|
        case reader.read_nonblock(READ_BYTES, exception: false)
        in nil then open_readers.delete(reader)
        in :wait_readable then nil
        in String => chunk then append_capped(captures.fetch(reader), chunk, cap:)
        end
      end
    end

    # Keeps at most cap + 1 bytes in capture: the byte past the cap marks
    # the cap as reached, and nothing beyond it is held in memory.
    private_class_method def self.append_capped(capture, chunk, cap:)
      room = cap + 1 - capture.bytesize
      capture << chunk.byteslice(0, room) if room.positive?
    end

    private_class_method def self.drain(captures, open_readers, cap:)
      deadline = now + DRAIN_SECONDS
      read_available(captures, open_readers, cap:) until open_readers.empty? || now >= deadline
    end

    private_class_method def self.now = Process.clock_gettime(Process::CLOCK_MONOTONIC)

    private_class_method def self.children_cpu = Process.times.then { _1.cutime + _1.cstime }
  end
end
