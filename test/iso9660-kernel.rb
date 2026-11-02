# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require_relative "iso9660-writer"

# Run explicitly as root in a disposable Linux container or on the FreeBSD
# qualification host. Ordinary tests never request mounts or privileges.
# Each call owns exactly one temporary mount and, on FreeBSD, one md device.
module IsoKernel
  module_function

  def mount_arguments(device:, root:, view:, platform:)
    if platform == "Linux"
      namespace = case view
      when "rockridge" then []
      when "joliet" then %w[norock utf8 check=strict]
      when "primary" then %w[norock nojoliet map=off]
      end
      options = %w[loop ro nosuid nodev] + namespace
      ["mount", "-t", "iso9660", "-o", options.join(","), device, root]
    else
      namespace = case view
      when "rockridge" then []
      when "joliet" then ["-r", "-g", "-C", "UTF-8"]
      when "primary" then ["-r", "-j", "-g"]
      end
      ["mount_cd9660", "-o", "ro,nosuid,noexec", *namespace, device, root]
    end
  end

  def with_mount(image:, view:, platform:)
    # Three fixture resources, in acquisition order: a directory, an
    # optional md device, and the mount. Register unmount before attempting
    # mount: the OS mount may succeed even when the command reports failure.
    cleanups = []
    begin
      root = Dir.mktmpdir("iso-kernel-")
      cleanups << -> { Dir.rmdir(root) } # Never recurse through a retained mount.
      device = image
      if platform == "FreeBSD"
        device = IsoWriter.command("mdconfig", "-a", "-t", "vnode", "-f", image, "-o", "readonly").strip
        raise IsoWriter::Error, "unexpected md device name" unless device.match?(/\Amd\d+\z/)

        unit = device.delete_prefix("md")
        cleanups << -> { IsoWriter.command("mdconfig", "-d", "-u", unit) }
        device = "/dev/#{device}"
      end
      cleanups << -> { IsoWriter.command("umount", root) }
      arguments = mount_arguments(device: device, root: root, view: view, platform: platform)
      IsoWriter.command(*arguments)
      yield root
    ensure
      # This is the finalization boundary: record every cleanup failure,
      # retain the primary diagnostic, and still attempt the remaining owners.
      primary = $!
      failures = cleanups.reverse_each.filter_map do |cleanup|
        begin
          cleanup.call
          nil
        rescue StandardError => error
          error
        end
      end
      unless failures.empty?
        details = ([primary].compact + failures).map(&:message)
        raise IsoWriter::Error, details.join("\ncleanup: ")
      end
    end
  end

  def call(image:, manifest:)
    raise IsoWriter::Error, "kernel qualification requires root in a disposable test environment" unless Process.euid.zero?
    raise IsoWriter::Error, "image must be a regular non-symlink file" unless File.lstat(image).file?

    platform = IsoWriter.command("uname", "-s").strip
    raise IsoWriter::Error, "kernel qualification supports Linux and FreeBSD" unless %w[Linux FreeBSD].include?(platform)

    expected = JSON.parse(File.read(manifest))
    multi_extent = expected.values.any? do |rows|
      rows.values.any? { _1.fetch("kind") == "file" && _1.fetch("size") > 0xffffffff }
    end
    if platform == "FreeBSD" && multi_extent
      puts "UNSUPPORTED: native FreeBSD cd9660 does not combine multi-extent file sections; this case is not a pass"
      return :unsupported
    end
    IsoWriter::VIEWS.each do |view|
      with_mount(image: File.realpath(image), view: view, platform: platform) do |root|
        actual = IsoWriter.host_manifest(root: root, view: view)
        IsoWriter.compare(expected: expected.fetch(view), actual: actual, context: "#{platform} #{view}")
        puts "#{platform} #{view}: #{actual.size} entries, complete metadata and content matched"
      end
    end
    :passed
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    result = IsoKernel.call(image: File.expand_path(ARGV.fetch(0)), manifest: File.expand_path(ARGV.fetch(1)))
    exit(result == :unsupported ? 77 : 0)
  rescue IsoWriter::Error, KeyError, JSON::ParserError => error
    warn error.message
    exit 1
  end
end
