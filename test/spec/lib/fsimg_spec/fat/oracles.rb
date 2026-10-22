# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"
require "json"

module FsimgSpec
  module Fat
    # A volume the read-only checker program, known by its file name,
    # cannot read (specs/design-spec-testing.md#checker-blind-spots): on a
    # volume whose cluster count is one of clusters it does not apply and
    # never runs, so nothing it would print is read. reason says why,
    # citing the program's source and the specification section it
    # departs from. clusters are exact counts, never a range: a blind spot
    # is traced to the counts the program's source names.
    CheckerBlindSpot = Data.define(:program, :clusters, :reason) do
      include Checked

      def initialize(clusters:, **fields)
        raise HarnessError, "a checker blind spot names exact cluster counts" unless
          clusters.is_a?(Array) && !clusters.empty? && clusters.all?(Integer)

        super(clusters: clusters.dup.freeze, **fields)
      end

      # Whether program, the checker's file name, cannot read a volume of
      # cluster_count clusters, which is Undefined where the BPB is.
      def covers?(program:, cluster_count:) = program == self.program && clusters.include?(cluster_count)
    end

    # The independent tools that read a FAT image beside fsimg
    # (specs/design-spec-testing.md#independence-and-oracles). Each gives one
    # Observation of an image whose tree is known: the read-only checker
    # (fsck -n), the mtools read-back (mdir -b and mtype) and fatcat.
    #
    # Every tool runs through the OracleShell, in the image's directory with
    # the image named relative to it. Tool output is untrusted text that
    # carries names from the image: each stream is read within a cap the
    # known tree sets, parsed as bytes, compared with the tree as values,
    # and quoted only through FsimgSpec.shown. Every name a tool is given is
    # one argv element behind the fixed prefix ::/ or /, so no name passes
    # through a shell or reads as an option, and the tools read files only
    # to stdout, so an observation writes nothing.
    module Oracles
      # A step that found the tool disagreeing with the tree; Oracles.observe
      # turns it into that tool's Observation.
      class Disagreement < Error; end

      # The read-only checkers read one or two FATs.
      CHECKER_FAT_COUNTS = 1..2
      # A listing may take this much per entry beyond its path, and this
      # much beyond its entries, so a tree with a few entries more still
      # lists in full, and the difference names them.
      LINE_BYTES = 256
      SLACK_BYTES = 64 << 10
      # A file's read-back may run this far past the file's size, so a
      # longer file shows as a size that differs, and a warning on stderr
      # fits beside an empty file.
      READ_SLACK_BYTES = 64 << 10
      # What the checker may write: a report, never the image's contents.
      CHECKER_BYTES = 1 << 20
      # The characters mtools reads as a pattern in a name it looks up; a
      # backslash before one takes it literally.
      PATTERN_CHARACTERS = /[\\*?\[\]]/

      # What the detail of a tool that failed quotes, for reports: this
      # many of the lines it wrote that are not blank, within
      # Observation::DETAIL_LENGTH, since a checker's diagnostic can follow
      # its progress lines.
      QUOTED_LINES = 8

      TOOLS = %w[fsck mtools fatcat].freeze
      # The tools whose disagreement counts against the image they read:
      # a base serves no case, and a created image fails its case
      # (specs/design-spec-testing.md#independence-and-oracles). fatcat is
      # recorded only.
      VERDICT_TOOLS = %w[fsck mtools].freeze

      FREEBSD_BOOT_C = "https://cgit.freebsd.org/src/tree/sbin/fsck_msdosfs/boot.c"

      # The checker's blind spots (specs/design-spec-testing.md#checker-blind-spots).
      # FreeBSD's fsck_msdosfs picks the FAT type in readboot by comparing
      # the cluster count with the largest cluster number of each FAT
      # width, one more than the count section 3.5 compares, so it misreads
      # the volume at each threshold: at 4085 clusters it walks a FAT16
      # volume's chains as 12-bit entries and prints whatever it then
      # finds, and at 65525 it refuses a FAT32 volume as too small.
      BLIND_SPOTS = [
        CheckerBlindSpot.new(
          program: "fsck_msdosfs", clusters: [4085],
          reason: "FreeBSD's fsck_msdosfs (#{FREEBSD_BOOT_C}, readboot) takes a 12-bit FAT while the cluster count is " \
                  "below CLUST_RSRVD & CLUST12_MASK, 4086, where section 3.5 makes 4085 clusters FAT16.".freeze
        ),
        CheckerBlindSpot.new(
          program: "fsck_msdosfs", clusters: [65_525],
          reason: "FreeBSD's fsck_msdosfs (#{FREEBSD_BOOT_C}, readboot) refuses a FAT32 volume whose cluster count is " \
                  "below CLUST_RSRVD & CLUST16_MASK, 65526, where section 3.5 makes 65525 clusters FAT32.".freeze
        )
      ].freeze

      # One Observation per tool of the image at image, a path relative to
      # chdir, whose model knows its tree. Each tool is observed on its
      # own: the rescue inside the block turns one tool's Disagreement into
      # its observation and leaves the other tools to run. On a host the
      # shell cannot run oracles on, every tool is unavailable.
      def self.observe(shell, image:, chdir:, model:)
        raise HarnessError, "the oracles read only an image whose tree is known" if FsimgSpec.undefined?(model.tree)

        reason = shell.unavailable_reason
        return TOOLS.to_h { [_1, Observation.unavailable(reason)] } if reason

        geometry = model.geometry
        # A FAT32 image's root cluster, nil for FAT12 or FAT16, whose root
        # directory is a fixed region, or Undefined with the BPB.
        root_cluster = FsimgSpec.derive(model.fat32_extension, model.extension) { _1 ? _2.bpb_root_clus : nil }
        {
          "fsck" => -> { checker(shell, image:, chdir:, fat_count: geometry.fat_count, cluster_count: geometry.cluster_count) },
          "mtools" => -> { mtools(shell, image:, chdir:, tree: model.tree) },
          "fatcat" => lambda {
            fatcat(shell, image:, chdir:, tree: model.tree, geometry:, root_cluster:)
          }
        }.to_h do |tool, run|
          [tool, run.call]
        rescue Disagreement => e
          [tool, Observation.disagrees(e.message)]
        end
      end

      # Each verdict tool's disagreement in observations, as "tool: detail".
      # One that does not apply or is unavailable gives none.
      def self.disagreements(observations) =
        observations.slice(*VERDICT_TOOLS).filter_map do |tool, observation|
          "#{tool}: #{observation.detail}" if observation.state == "disagrees"
        end

      # fsck.fat -n or fsck_msdosfs -n, the first configured, agrees when it
      # exits 0. It does not run on a volume one of its blind spots covers.
      # Its label names the program, its file name, which a blind spot
      # names, since the two checkers differ in their defects.
      private_class_method def self.checker(shell, image:, chdir:, fat_count:, cluster_count:)
        tool = shell.tools.values_at(:fsck_fat, :fsck_msdosfs).compact.first
        return Observation.unavailable("no read-only checker is configured") unless tool
        return Observation.not_applicable("the checkers read one or two FATs, not #{fat_count}") unless
          fat_count.is_a?(Integer) && CHECKER_FAT_COUNTS.cover?(fat_count)

        program = File.basename(tool)
        blind = BLIND_SPOTS.find { _1.covers?(program:, cluster_count:) }
        return Observation.not_applicable(blind.reason) if blind

        output(shell, [tool, "-n", image], chdir:, cap: CHECKER_BYTES, label: "#{program} -n")
        Observation.agrees
      end

      # The mtools read-back: mdir -b lists every path, a directory with a
      # trailing /, and mtype streams each file the tree expects, whose
      # size and SHA-256 the harness takes from the stream. mtools drops the
      # surrogate pairs of a long name, so a name beyond the BMP is not
      # one it reads back.
      private_class_method def self.mtools(shell, image:, chdir:, tree:)
        mdir, mtype = shell.tools.values_at(:mdir, :mtype)
        return Observation.unavailable("mdir or mtype is not configured") unless mdir && mtype
        return Observation.not_applicable("mtools drops long names beyond the BMP") if tree.any? { beyond_bmp?(_1.path) }

        listing = output(shell, [mdir, "-b", "-/", "-a", "-i", image, "::/"], chdir:, cap: listing_bytes(tree), label: "mdir")
        listed = listing.split("\n".b).map { mdir_entry(_1) }
        read = read_files(listed, tree) do |path, cap|
          output(shell, [mtype, "-i", image, "::/".b + literal(path)], chdir:, cap:, label: "mtype #{FsimgSpec.shown(path)}")
        end
        agreement("mtools", actual: read, expected: tree)
      end

      # fatcat lists each directory of the tree as JSON and reads each file
      # the tree expects to stdout. It reads 512-byte sectors only, finds a
      # FAT32 root directory only at cluster 2, the usual one, and decodes
      # no Unicode, so it applies only where every name is ASCII. It reads
      # a FAT12 or FAT16 root region in whole clusters, so a region that is
      # no multiple of the cluster size is read on into the data region,
      # whose first directory it lists as the root's (FatSystem::getEntries
      # and rootClusters in the pinned fatcat). root_cluster is nil for
      # FAT12 or FAT16. Its exit status is 0 even when a path is missing,
      # so only what it writes counts.
      private_class_method def self.fatcat(shell, image:, chdir:, tree:, geometry:, root_cluster:)
        tool = shell.tools[:fatcat]
        sector = geometry.bytes_per_sector
        return Observation.unavailable("fatcat is not configured") unless tool
        return Observation.not_applicable("fatcat reads 512-byte sectors, not #{sector}") unless sector == 512

        # Whether the root region ends inside a cluster, or Undefined.
        root_overread = FsimgSpec.derive(geometry.root_entries, geometry.sectors_per_cluster) do |entries, spc|
          spc.zero? || !((entries * Geometry::DIRECTORY_ENTRY_BYTES) % (spc * sector)).zero?
        end
        return Observation.not_applicable("fatcat finds a FAT32 root directory at cluster 2 only") unless
          root_cluster.nil? || root_cluster == 2
        return Observation.not_applicable("fatcat reads a FAT12 or FAT16 root region in whole clusters") if
          root_cluster.nil? && root_overread != false # an Undefined region is not known to be whole
        return Observation.not_applicable("fatcat decodes ASCII names only") unless tree.all? { _1.path.ascii_only? }

        directories = ["".b, *tree.select { _1.type == "directory" }.map(&:path)]
        listed = directories.flat_map do |directory|
          path = "/".b + directory
          json = output(shell, [tool, "-F", "json", "-l", path, image], chdir:, cap: listing_bytes(tree),
                                                                       label: "fatcat -l #{FsimgSpec.shown(path)}")
          fatcat_entries(json, directory)
        end
        read = read_files(listed, tree) do |path, cap|
          output(shell, [tool, "-r", "/".b + path, image], chdir:, cap:, label: "fatcat -r #{FsimgSpec.shown(path)}")
        end
        agreement("fatcat", actual: read, expected: tree)
      end

      # Runs argv through shell and answers its stdout once it ran to its
      # end and exited 0. A tool that ran past its deadline, wrote past cap,
      # left a process behind or exited otherwise disagrees, quoting the
      # first lines it wrote. One that could not start is the harness's
      # fault.
      private_class_method def self.output(shell, argv, chdir:, cap:, label:)
        execution = shell.run(argv, chdir:, capture_bytes: cap)
        raise HarnessError, "#{label} could not start: #{execution.launch_error}" if execution.launch_error

        case execution.ended_by
        in "capture_cap" then raise Disagreement, "#{label} wrote more than #{cap} bytes"
        in "deadline" then raise Disagreement, "#{label} ran past its deadline"
        in nil then nil
        end
        raise Disagreement, "#{label} left a process behind" if execution.leftover
        return execution.stdout if execution.exit_status == 0

        ending = execution.signal ? "died by signal #{execution.signal}" : "exited #{execution.exit_status}"
        raise Disagreement, "#{label} #{ending}: #{quoted(execution)}"
      end

      # The first QUOTED_LINES lines that are not blank of what a tool
      # wrote, stderr first, as a detail quotes them.
      private_class_method def self.quoted(execution)
        lines = [execution.stderr, execution.stdout].lazy.flat_map { _1.each_line(chomp: true).lazy }
                                                   .reject { _1.strip.empty? }.first(QUOTED_LINES)
        FsimgSpec.shown(lines.join(" | ".b), limit: Observation::DETAIL_LENGTH)
      end

      # A cap for a listing of tree's entries and a few more.
      private_class_method def self.listing_bytes(tree) = tree.sum { _1.path.bytesize + LINE_BYTES } + SLACK_BYTES

      # A line of mdir -b -/ as a WalkedEntry: ::/ and the path, with a
      # trailing / for a directory. Any other line disagrees.
      private_class_method def self.mdir_entry(line)
        raise Disagreement, "mdir lists #{FsimgSpec.shown(line)}, which is no ::/ path" unless line.start_with?("::/".b)

        path = line.delete_prefix("::/".b)
        directory = path.end_with?("/".b)
        WalkedEntry.new(path: directory ? path.chomp("/".b) : path, type: directory ? "directory" : "file", size: nil, sha256: nil)
      end

      # The entries of fatcat's JSON listing of directory, without . and ..
      # and without deleted entries, which a listing without -d omits.
      private_class_method def self.fatcat_entries(json, directory)
        listing = begin
          JSON.parse(json.dup.force_encoding(Encoding::UTF_8))
        rescue JSON::ParserError, EncodingError
          raise Disagreement, "fatcat -l #{FsimgSpec.shown(directory)} wrote no JSON listing: #{FsimgSpec.shown(json)}"
        end
        entries = listing.is_a?(Hash) ? listing["Entries"] : nil
        raise Disagreement, "fatcat -l #{FsimgSpec.shown(directory)} wrote no entry list" unless entries.is_a?(Array) && entries.all?(Hash)

        entries.reject { [".", ".."].include?(_1["Name"]) || _1["IsDeleted"] == true }.map do |entry|
          name = entry["Name"].to_s.b
          type = { "Directory" => "directory", "File" => "file" }.fetch(entry["EntryType"]) { FsimgSpec.shown(entry["EntryType"]) }
          WalkedEntry.new(path: directory.empty? ? name : directory + "/".b + name, type:, size: nil, sha256: nil)
        end
      end

      # The listed entries with every file the tree expects read through
      # the block, which answers its bytes within a cap; a file the tree
      # does not expect is not read, since it differs anyway.
      private_class_method def self.read_files(listed, tree)
        sizes = tree.select { _1.type == "file" }.to_h { [_1.path, _1.size] }
        listed.map do |entry|
          next entry unless entry.type == "file" && sizes.key?(entry.path)

          bytes = yield entry.path, sizes.fetch(entry.path) + READ_SLACK_BYTES
          entry.with(size: bytes.bytesize, sha256: Digest::SHA256.hexdigest(bytes))
        end
      end

      # Agrees when actual, a listing as WalkedEntry values, is the known
      # tree; otherwise disagrees, naming the first differences.
      private_class_method def self.agreement(tool, actual:, expected:)
        differences = KnownTree.differences(tool, actual: KnownTree.from_walk(actual), expected:)
        differences.empty? ? Observation.agrees : Observation.disagrees(differences.join("; "))
      end

      # path with every character mtools reads as a pattern escaped.
      # The pattern is ASCII, so it matches the bytes of a binary path.
      def self.literal(path) = path.b.gsub(PATTERN_CHARACTERS) { "\\".b + _1 }

      private_class_method def self.beyond_bmp?(path) =
        path.dup.force_encoding(Encoding::UTF_8).then { _1.valid_encoding? && _1.each_codepoint.any? { |point| point > 0xFFFF } }
    end
  end
end
