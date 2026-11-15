# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The independent checkers that read an APFS container beside fsimg
    # (specs/design-spec-testing.md#independence-and-oracles): Apple's
    # fsck_apfs -n on macOS, run on the raw disk hdiutil attaches without
    # mounting, and apfsck from apfsprogs, which reads the image file. Each
    # gives one Observation: agrees when it exits 0, disagrees with the
    # first lines it wrote otherwise, and unavailable on a host without
    # it. Neither mounts or writes the image: hdiutil attaches it read
    # only, and both checkers run in their read-only modes.
    #
    # Attaching lets the kernel probe the image, which no process sandbox
    # confines, so the checkers read only an Unmutated image
    # (specs/design-spec-testing.md#process-boundary-and-host-safety).
    module Oracles
      TOOLS = %w[fsck_apfs apfsck].freeze
      LIMITS = Invocation::Limits.new(wall_seconds: 300, cpu_seconds: 300, file_bytes: 1 << 20, address_space_bytes: nil,
                                      capture_bytes: 1 << 20)
      QUOTED_LINES = 8
      DEVICE = %r{\A/dev/disk\d+\z}

      # An image no harness mutation touched, the only input the checkers
      # take. It takes no image path or ImageView: its two constructors
      # each name the fixed file only a native capture or fsimg's create
      # stage writes, and the harness writes a mutated view to neither.
      # A plain class, not a Data, so no #with can swap the path.
      class Unmutated
        attr_reader :path

        private_class_method :new

        # The capture name under the native capture directory root
        # ($FSIMG_APFS_NATIVE).
        def self.native(name, root:) = new(File.join(root, name, "image.img"))

        # The image create wrote in a case directory.
        def self.created(directory) = new(File.join(directory, Runner::CREATED))

        def initialize(path)
          @path = File.expand_path(path).freeze
          freeze
        end
      end

      module_function

      # One Observation per tool of an Unmutated image. environment names
      # the programs: FSCK_APFS and HDIUTIL override the system paths, and
      # APFSCK, or else PATH, finds apfsck.
      def observe(image, environment: ENV)
        raise HarnessError, "the APFS checkers read only an Unmutated image" unless image.is_a?(Unmutated)

        { "fsck_apfs" => fsck_apfs(image.path, environment), "apfsck" => apfsck(image.path, environment) }
      end

      def fsck_apfs(path, environment)
        return Observation.unavailable("fsck_apfs runs on macOS only") unless RUBY_PLATFORM.include?("darwin")

        fsck, hdiutil = [["FSCK_APFS", "/sbin/fsck_apfs"], ["HDIUTIL", "/usr/bin/hdiutil"]].map { environment.fetch(_1, _2) }
        missing = [fsck, hdiutil].reject { File.executable?(_1) }
        return Observation.unavailable("no #{missing.join(" or ")}") unless missing.empty?

        attached(hdiutil, path) do |device|
          verdict(run([fsck, "-n", device.sub("/dev/disk", "/dev/rdisk")]), "fsck_apfs")
        end
      end

      def apfsck(path, environment)
        program = environment["APFSCK"] || find_on_path("apfsck", environment.fetch("PATH", ""))
        return Observation.unavailable("no apfsck") unless program && File.executable?(program)

        verdict(run([program, path]), "apfsck")
      end

      # Attaches the image read only as a raw disk without mounting it,
      # yields its whole-disk device and detaches it again, whatever the
      # block does. An attachment whose output names no device, or a disk
      # that stays attached, disagrees, so no observation stands without
      # the checker having run and the disk having gone.
      def attached(hdiutil, path)
        attach = run([hdiutil, "attach", "-readonly", "-nomount", "-imagekey", "diskimage-class=CRawDiskImage", path])
        device = attach.stdout.to_s.lines.filter_map { _1.split.first }.find { DEVICE.match?(_1) }
        begin
          return verdict(attach, "hdiutil attach") unless attach.exit_status&.zero?
          return Observation.disagrees("hdiutil attach: no whole-disk device in its output") unless device

          observation = yield device
        ensure
          detached = device.nil? || detach?(hdiutil, device)
        end
        detached ? observation : Observation.disagrees("hdiutil detach: #{device} stays attached")
      end

      def detach?(hdiutil, device) = [[], ["-force"]].any? { run([hdiutil, "detach", *_1, device]).exit_status&.zero? }

      def run(argv) = Invocation.run(argv, chdir: Dir.pwd, env: { "LC_ALL" => "C" }, limits: LIMITS)

      # A checker that reports damage while exiting 0 disagrees: fsck_apfs
      # passes over a newest checkpoint that fails its checks, says so, and
      # judges an older one (design: Checkpoint selection, E8).
      DAMAGE = /^(error|warning):|failed consistency check/

      def verdict(execution, program)
        clean = execution.exit_status&.zero? && execution.ended_by.nil? && !DAMAGE.match?("#{execution.stderr}\n#{execution.stdout}")
        return Observation.agrees if clean

        reason = execution.launch_error || execution.ended_by || "exit #{execution.exit_status.inspect}"
        lines = [execution.stderr, execution.stdout].join("\n").lines.map(&:strip).reject(&:empty?).first(QUOTED_LINES)
        Observation.disagrees("#{program}: #{reason}: #{FsimgSpec.shown(lines.join(" | "), limit: Observation::DETAIL_LENGTH)}")
      end

      def find_on_path(name, path) = path.split(File::PATH_SEPARATOR).map { File.join(_1, name) }.find { File.executable?(_1) }
    end
  end
end
