# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E2 puts on a mounted volume of allocation block size B. The
# profile is a flat list of steps in the order they run; each names what it
# probes and the path it creates. A step is attempted against the mounted root
# and a refusal by the system is recorded as an observation, not an error.
#
# The tree the profile builds, under the root:
#
#   size-0 size-1 size-(B-1) size-B size-(B+1)    files around the block size
#   n1/n2/.../n8/leaf                             eight nested directories
#   aaaa..., bbbb..., cccc..., dddd...            names of 1, 254, 255, 256 characters
#   colon:mid :lead trail: a::b                   names holding a colon
#   symlink-1 ... symlink-2048                    links with targets of that length
#   fifo                                          a named pipe
#   time-1904 time-1969 time-2040-limit time-2040-beyond   files with extreme mtimes
#   dir-time-1904, symlink-time-1969              the same on a directory and a link
#   mode-0600 mode-0755 mode-4755 mode-0000      files with these modes
#   dir-mode-1777, dir-mode-0500                  directories with these modes
#   size-1 again, given another owner and group   ownership changes
require "fileutils"
require "open3"

module HfsPlusPopulate
  TIME_1904 = -2_082_844_800
  TIME_1969 = -17_000_000
  TIME_2040_LIMIT = 2_212_122_495
  TIME_2040_BEYOND = 2_222_222_222
  NESTED = %w[n1 n2 n3 n4 n5 n6 n7 n8].freeze

  # `action` is called with the mounted root and may raise SystemCallError.
  Step = Data.define(:step, :path, :action)

  module_function

  # All steps of experiment E2 for a volume of block size `block_size`.
  def e2(block_size)
    size_files(block_size) + nested_dirs + long_names + colon_names + symlinks + fifo +
      file_times + dir_and_link_times + file_modes + dir_modes + ownership
  end

  # Raised when a program a step runs fails; its output is the message.
  class ToolFailed < StandardError; end

  # What a step reports besides success: text it observed, or text describing
  # a refusal it expected. A step that returns neither has nothing to report.
  Observation = Data.define(:text)
  Refusal = Data.define(:text)

  # Run `step` against `root`, appending what happened to `attempts`. An
  # Observation is kept as the attempt's `output`; a Refusal makes the attempt
  # `ok: false, refused: true` with its text as `output`.
  def attempt(attempts, step, root)
    outcome = case step.action.call(root)
              in Observation => seen then { ok: true, output: seen.text }
              in Refusal => refused then { ok: false, refused: true, output: refused.text }
              else { ok: true }
              end
    attempts << { step: step.step, path: step.path, **outcome }
  rescue SystemCallError, ToolFailed => e
    attempts << { step: step.step, path: step.path, ok: false, error: e.class.name.delete_prefix("Errno::"), message: e.message }
  end

  # ---- the steps ----

  def size_files(block_size)
    [0, 1, block_size - 1, block_size, block_size + 1].map do |size|
      step("file-size", "size-#{size}") { write_file(_1, "size-#{size}", pattern(size, size)) }
    end
  end

  def nested_dirs
    [step("nested-dirs", File.join(*NESTED)) do |root|
      FileUtils.mkdir_p(File.join(root, *NESTED))
      write_file(root, File.join(*NESTED, "leaf"), "leaf\n")
    end]
  end

  # The step's path shows only the first 20 characters of the name.
  def long_names
    { "u001" => "a", "u254" => "b" * 254, "u255" => "c" * 255, "u256" => "d" * 256 }.map do |label, name|
      step("name-#{label}", name[0, 20]) { write_file(_1, name, label) }
    end
  end

  def colon_names
    ["colon:mid", ":lead", "trail:", "a::b"].map { |name| step("name-colon", name) { write_file(_1, name, name) } }
  end

  def symlinks
    [1, 255, 1023, 1024, 1025, 2048].map do |length|
      step("symlink", "symlink-#{length}") { File.symlink("t" * length, File.join(_1, "symlink-#{length}")) }
    end
  end

  def fifo
    [step("fifo", "fifo") { system("mkfifo", File.join(_1, "fifo")) || raise(Errno::EPERM, "mkfifo") }]
  end

  def file_times
    { "time-1904" => TIME_1904, "time-1969" => TIME_1969, "time-2040-limit" => TIME_2040_LIMIT, "time-2040-beyond" => TIME_2040_BEYOND }.map do |name, time|
      step("file-time", name) do |root|
        write_file(root, name, name)
        File.utime(time, time, File.join(root, name))
        observe("lstat mtime #{File.lstat(File.join(root, name)).mtime.to_i}")
      end
    end
  end

  def dir_and_link_times
    [step("dir-time", "dir-time-1904") do |root|
       Dir.mkdir(File.join(root, "dir-time-1904"))
       File.utime(TIME_1904, TIME_1904, File.join(root, "dir-time-1904"))
       observe("lstat mtime #{File.lstat(File.join(root, 'dir-time-1904')).mtime.to_i}")
     end,
     step("symlink-time", "symlink-time-1969") do |root|
       File.symlink("size-1", File.join(root, "symlink-time-1969"))
       File.lutime(TIME_1969, TIME_1969, File.join(root, "symlink-time-1969"))
       observe("lstat mtime #{File.lstat(File.join(root, 'symlink-time-1969')).mtime.to_i}")
     end]
  end

  def file_modes
    { "mode-0600" => 0o600, "mode-0755" => 0o755, "mode-4755" => 0o4755, "mode-0000" => 0o000 }.map do |name, mode|
      step("file-mode", name) do |root|
        write_file(root, name, name)
        File.chmod(mode, File.join(root, name))
      end
    end
  end

  def dir_modes
    { "dir-mode-1777" => 0o1777, "dir-mode-0500" => 0o500 }.map do |name, mode|
      step("dir-mode", name) do |root|
        Dir.mkdir(File.join(root, name))
        File.chmod(mode, File.join(root, name))
      end
    end
  end

  # Root ownership needs privilege and is expected to be refused; the group
  # change goes to a supplementary group of the user, or to group 0.
  def ownership
    [step("chown", "size-1") { File.chown(0, nil, File.join(_1, "size-1")) },
     step("chgrp", "size-1") { File.chown(nil, other_group, File.join(_1, "size-1")) }]
  end

  # ---- helpers ----

  def step(name, path, &action) = Step.new(name, path, action)

  def observe(text) = Observation.new(text)

  def refuse(text) = Refusal.new(text)

  # 32 bytes of Finder information as hex: type, creator, flags, then zeros.
  def finder_info(type, creator, flags) = (type.b + creator.b + [flags].pack("n") + ("\0" * 22).b).unpack1("H*")

  # The last `length` characters of `text`, or all of it if it is shorter.
  def tail(text, length) = text[[text.length - length, 0].max..]

  def pattern(length, seed) = Array.new(length) { (_1 * 7 + seed) & 0xff }.pack("C*")

  def write_file(root, rel, content) = File.binwrite(File.join(root, rel), content)

  # Run a program without a shell and return its combined output; a failure
  # raises ToolFailed carrying the output.
  def tool(*argv)
    out, status = Open3.capture2e([argv.first, argv.first], *argv.drop(1))
    raise ToolFailed, "#{argv.join(' ')}: exit #{status.exitstatus}: #{out.strip}" unless status.success?

    out
  end

  # One line about what `lstat` of `path` reports, or the error it gives.
  def lstat_line(path)
    st = File.lstat(path)
    format("%s kind=%s ino=%d nlink=%d mode=%04o uid=%d size=%d mtime=%d birth=%d", File.basename(path), st.ftype, st.ino,
           st.nlink, st.mode & 0o7777, st.uid, st.size, st.mtime.to_i, st.birthtime.to_i)
  rescue SystemCallError => e
    "#{File.basename(path)} #{e.class.name.delete_prefix('Errno::')}"
  end

  def mkdir_p(root, rel) = FileUtils.mkdir_p(File.join(root, rel))

  def other_group = Process.groups.reject { _1 == Process.gid }.first || 0
end
