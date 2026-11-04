#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Experiment E8: how often an interrupted session leaves transactions in a
# journal. Each trial formats a fresh journaled volume, mounts it writable,
# starts a writer that creates files without syncing, interrupts the session
# after a delay, and decodes the journal of what is left:
#
#   force      hdiutil detach -force MOUNTPOINT while the writer runs
#   snapshot   a copy of the backing image taken while the writer runs, as a
#              crash would leave the file
#
# Then it mounts the interrupted image again and decodes the journal once more.
# One JSON line per trial goes to stdout. --keep DIR writes the image of the
# first trial that left transactions, before and after the second mount, to
# DIR/interrupted.img and DIR/remounted.img. Everything attached is detached
# again, on failure too.
#
#   e8_interrupt.rb --mode force --trials 10 --delay 0.6 --keep OUT_DIR
require "fileutils"
require "json"
require "open3"
require "optparse"
require "tmpdir"
require_relative "fields"

module HfsPlusInterrupt
  class Error < StandardError; end

  IMAGE_BYTES = 32 << 20
  FILE_BYTES = 4096

  Trial = Data.define(:number, :mode, :delay)

  module_function

  def run(*argv)
    out, status = Open3.capture2e(*argv)
    raise Error, "#{argv.join(' ')} failed: #{out.strip}" unless status.success?

    out
  end

  def attach(image, mountpoint: nil)
    argv = %w[hdiutil attach -imagekey diskimage-class=CRawDiskImage]
    argv += mountpoint ? ["-mountpoint", mountpoint] : ["-nomount"]
    run(*argv, image)[%r{^/dev/disk\d+}] or raise Error, "no device attached for #{image}"
  end

  def detach(target, force: false)
    run(*%w[hdiutil detach], *(force ? ["-force"] : []), target)
  end

  # Attach `image` (unmounted, or at `mountpoint`), yield its device, and always
  # detach it again, forced and best effort: a detach failure here must not hide
  # the error that brought us here, and the block may already have detached it.
  def with_attached(image, mountpoint: nil)
    device = attach(image, mountpoint: mountpoint)
    yield device
  ensure
    begin
      detach(device, force: true) if device
    rescue Error
      nil
    end
  end

  # A fresh journaled volume in `image`: formatted through an unmounted attachment.
  def format_journaled(image)
    FileUtils.rm_f(image)
    run("mkfile", "-n", IMAGE_BYTES.to_s, image)
    with_attached(image) { |device| run("newfs_hfs", "-J", "-v", "E8", "-b", "4096", device.sub("/dev/", "/dev/r")) }
  end

  # What the decoder says about the journal and the volume header.
  def journal_state(image)
    report = File.open(image, "rb") { HfsPlusFields.decode(_1, limit: 0) }
    header = report.fetch(:volume_header)
    report.fetch(:journal, {}).slice(:info_flags, :byte_order, :start, :end, :header)
          .merge(file_count: header[:file_count], folder_count: header[:folder_count], write_count: header[:write_count],
                 attributes: header[:attributes])
  end

  # The writer: files of FILE_BYTES until it is killed. It never syncs.
  def spawn_writer(root)
    spawn("ruby", "-e", 'i = 0; loop { File.binwrite(File.join(ARGV[0], format("w%07d", i)), "x" * 4096); i += 1 }', root,
          out: File::NULL, err: File::NULL)
  end

  def stop(pid)
    Process.kill("KILL", pid)
    Process.wait(pid)
  rescue Errno::ESRCH, Errno::ECHILD
    nil
  end

  # Interrupt one session; the image at `image` is what the session left.
  def interrupt(trial, image, workdir)
    mount = File.join(workdir, "mnt")
    Dir.mkdir(mount)
    with_attached(image, mountpoint: mount) do
      pid = spawn_writer(mount)
      begin
        sleep(trial.delay)
        end_session(trial.mode, image, mount, workdir)
      ensure
        stop(pid)
      end
    end
  end

  def end_session(mode, image, mount, workdir)
    case mode
    when "force" then detach(mount, force: true)
    when "snapshot"
      copy = File.join(workdir, "snapshot.img")
      FileUtils.cp(image, copy)
      detach(mount, force: true)
      FileUtils.mv(copy, image)
    else raise Error, "unknown mode #{mode}"
    end
  end

  # Mount the interrupted image again, as a user would, and unmount it.
  def remount(image, workdir)
    mount = File.join(workdir, "mnt2")
    Dir.mkdir(mount)
    with_attached(image, mountpoint: mount) do
      seen = Dir.children(mount).count { _1.start_with?("w") }
      detach(mount)
      seen
    end
  end

  # Only the first trial that left transactions is kept.
  def keep?(options, interrupted)
    keep = options[:keep]
    keep && interrupted[:start] != interrupted[:end] && !File.exist?(File.join(keep, "interrupted.img"))
  end

  def trial_result(trial, interrupted, seen, remounted)
    { trial: trial.number, mode: trial.mode, delay: trial.delay, interrupted: interrupted,
      files_seen_after_remount: seen, remounted: remounted }
  end

  # A directory on another device than its parent has a volume mounted on it.
  def mountpoint?(path) = File.stat(path).dev != File.stat(File.dirname(path)).dev

  # Empty and remove the work directory. A mount point that is still mounted is
  # never removed, since that would delete the volume's files: it is reported.
  # (The writer can leave files in an unmounted mount point after a forced detach.)
  def remove_workdir(dir)
    Dir.children(dir).each do |name|
      path = File.join(dir, name)
      raise Error, "#{path} is still a mount point" if File.directory?(path) && mountpoint?(path)

      FileUtils.rm_rf(path)
    end
    Dir.rmdir(dir)
  rescue Error, SystemCallError => e
    warn "e8_interrupt.rb: #{dir} not removed: #{e.message}"
  end

  def trial(trial, options)
    dir = Dir.mktmpdir("e8-", options.fetch(:work))
    image = File.join(dir, "v.img")
    format_journaled(image)
    interrupt(trial, image, dir)
    interrupted = journal_state(image)
    kept = keep?(options, interrupted)
    FileUtils.cp(image, File.join(options[:keep], "interrupted.img")) if kept
    seen = remount(image, dir)
    FileUtils.cp(image, File.join(options[:keep], "remounted.img")) if kept
    trial_result(trial, interrupted, seen, journal_state(image))
  ensure
    remove_workdir(dir) if dir
  end

  def main(argv)
    options = { mode: "force", trials: 10, delay: 0.6, work: File.join(Dir.home, ".cache/fsimg-hfs/1d") }
    OptionParser.new do |o|
      o.on("--mode MODE", %w[force snapshot]) { options[:mode] = _1 }
      o.on("--trials N", Integer) { options[:trials] = _1 }
      o.on("--delay SECONDS", Float) { options[:delay] = _1 }
      o.on("--keep DIR") { options[:keep] = _1 }
      o.on("--work DIR") { options[:work] = _1 }
    end.parse!(argv)
    FileUtils.mkdir_p(options[:keep]) if options[:keep]
    FileUtils.mkdir_p(options[:work])
    options[:work] = File.realpath(options[:work])
    (1..options[:trials]).each do |number|
      puts JSON.generate(trial(Trial.new(number, options[:mode], options[:delay]), options))
      $stdout.flush
    end
    0
  rescue Error, SystemCallError => e
    warn "e8_interrupt.rb: #{e.message}"
    1
  end
end

exit(HfsPlusInterrupt.main(ARGV)) if $PROGRAM_NAME == __FILE__
