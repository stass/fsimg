#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# The native qualification cycle of the APFS writer, on macOS. Each IMAGE given
# on the command line is a raw container, a raw GPT disk holding one, or a
# UDIF file holding one (`apfs`, `apfs/gpt`, `apfs/gpt/dmg`); the cycle works
# on a copy of it under --root and leaves the original alone. Then:
#
#   1. fsck_before  attach without mounting, `fsck_apfs -n`
#   2. mount        mount read-only; compare find, stat and checksums with the
#                   expected manifest
#   3. modify       mount read-write; create, append in rounds that interleave another file,
#                   rename, mkdir, rmdir, unlink and write a large file; unmount
#   4. fsck_after   `fsck_apfs -n` again
#   5. remount      mount read-only; compare again, with the modifications
#   6. fsimg        `fsimg W` verifies the volume, `t --json` lists it and
#                   `x -O` delivers each nonempty file
#
# A step's failure does not stop the steps after it, so that one result does
# not hide another. A step with nothing to run says why, as "skipped: ...", and
# a skipped step is not a failure:
#
#   mount    without a manifest, the first mount's own view becomes the
#            baseline the remount is judged against
#   fsimg    when no --fsimg is given or its --formats does not list apfs with
#            Read and Verify
#
# The expected manifest is the `native-manifest.json` or
# `native-manifest.ndjson` beside the image (where tools/apfs/capture.rb
# populate scripts leave it), or the --manifest FILE; --no-manifest runs
# without. Both forms of docs/apfs-research.md are read: a JSON array of E3
# (kind directory, times btime_ns) and the NDJSON of E5 (kind dir, times
# birthtime_ns, mode as an octal string).
#
# A file the mounting user may not read is compared without its checksum. The
# mount ignores ownership (E3), so owners are compared nowhere; BSD flags, link counts and extended
# attributes are not compared either. Access and
# change times move with every read and modification, so only the modification
# and creation times are compared. macOS's own additions to the root are left
# out; `.fseventsd` changes at unmount.
#
# Only copies this script made are attached, all inside --root. Example:
#
#   qualify.rb --root /Volumes/Optaine/apfs-qualify --fsimg BUILD/fsimg \
#     "$FSIMG_APFS_NATIVE"/apfs-newfs-67108864-small/image.img
require "digest"
require "fileutils"
require "json"
require "optparse"
require "tmpdir"
require_relative "capture"

module ApfsQualify
  class Error < ApfsCapture::Error; end

  COMMAND_TIMEOUT = 30 * 60
  OUTPUT_EXCERPT_BYTES = 4096
  # What macOS adds to a volume's root.
  NATIVE_ROOT = %w[.fseventsd .Spotlight-V100 .Trashes .TemporaryItems .DS_Store].freeze
  MOUNT_OPTIONS = %w[-nobrowse -owners off].freeze
  COMPARED = %i[kind size mode created modified target sha256].freeze
  LISTED = %i[kind size mode created modified].freeze
  # The kinds the manifests and `find` name, as one set.
  KINDS = { "dir" => "directory", "link" => "symlink", "chr" => "char", "char_device" => "char",
            "characterSpecial" => "char", "blk" => "block", "block_device" => "block",
            "blockSpecial" => "block" }.freeze
  # Where a UDIF file keeps its signature, and a GPT disk its header.
  UDIF_SIGNATURE = "koly"
  GPT_SIGNATURE = "EFI PART"
  GPT_OFFSET = 512
  LARGE_BYTES = 16 << 20
  CHUNK_BYTES = 1 << 20
  # Each round appends to one file and writes a block of another after it, so
  # that the next append cannot extend in place.
  FRAGMENT_ROUNDS = 16
  APPEND_SIZES = [1, 4095, 4096, 4097, 100_000].freeze

  # What a path should show, or what one shows; nil means it is not compared.
  View = Data.define(:kind, :size, :mode, :created, :modified, :target, :sha256) do
    def self.of(kind:, size: nil, mode: nil, created: nil, modified: nil, target: nil, sha256: nil)
      new(kind: kind, size: size, mode: mode, created: created, modified: modified, target: target, sha256: sha256)
    end

    def regular? = kind == "file"
  end

  # One image to qualify: where it is, how it is wrapped (raw, gpt or dmg), and
  # the manifest that says what it holds and the tree it parses to (both nil for
  # none).
  Image = Data.define(:name, :source, :layout, :manifest, :tree)

  Tools = Data.define(:fsimg)

  module_function

  # ---- Manifests -----------------------------------------------------------

  # The expected tree of a manifest in either form: path (as bytes, with a
  # leading slash) to View. The root and macOS's additions to it are left out.
  def parse_manifest(text)
    raise Error, "manifest is not UTF-8" unless text.dup.force_encoding("UTF-8").valid_encoding?

    records = text.lstrip.start_with?("[") ? JSON.parse(text) : text.lines.reject { _1.strip.empty? }.map { JSON.parse(_1) }
    records.filter_map { manifest_entry(_1) }.then { unique_paths(_1) }
  rescue JSON::ParserError => e
    raise Error, "manifest is not JSON: #{e.message.lines.first}"
  end

  def unique_paths(entries)
    dupes = entries.map(&:first).tally.select { _2 > 1 }.keys
    raise Error, "manifest paths repeat: #{dupes.first(4).join(', ')}" unless dupes.empty?

    entries.to_h
  end

  def manifest_entry(record)
    raise Error, "manifest entry #{record.inspect[0, 80]} is not an object with a path" unless record.is_a?(Hash) && record["path"].is_a?(String)

    path = "/#{record.fetch('path').delete_prefix('/').delete_prefix('./')}"
    return if path == "/" || path == "/." || native_addition?(path)

    # A path is applied to the mount by the modifications, so it may not leave it.
    raise Error, "manifest path #{path.inspect} is not a plain relative path" unless plain_path?(path)

    kind = record.fetch("kind")
    kind = KINDS.fetch(kind, kind)
    [path.b, View.of(kind: kind, size: kind == "file" ? record.fetch("size") : nil, mode: manifest_mode(record["mode"]),
                     created: record["btime_ns"] || record["birthtime_ns"], modified: record["mtime_ns"],
                     target: record["target"]&.b, sha256: %w[file symlink].include?(kind) ? record["sha256"] : nil)]
  rescue KeyError => e
    raise Error, "manifest entry #{record.inspect[0, 80]} lacks #{e.key}"
  end

  def plain_path?(path) = path.split("/").drop(1).none? { _1.empty? || _1 == "." || _1 == ".." || _1.include?("\0") }

  # The permission bits: an integer, or the octal st_mode string of the NDJSON
  # form, which carries the file type bits too.
  def manifest_mode(mode) = mode.is_a?(String) ? mode.to_i(8) & 0o7777 : mode

  # Whether the path is, or lies beneath, one of macOS's additions to the root.
  def native_addition?(path) = NATIVE_ROOT.include?(path.delete_prefix("/").split("/", 2).first)

  # The manifest that goes with an image: the one asked for, else the one
  # beside the image; nil when `skip` or none is there.
  def find_manifest(source, given:, skip:)
    return if skip

    given || %w[native-manifest.json native-manifest.ndjson].map { File.join(File.dirname(source), _1) }.find { File.file?(_1) }
  end

  # ---- What a mounted volume or a listing shows ------------------------------

  def nanoseconds(time) = time.to_i * 1_000_000_000 + time.nsec

  def view_of(full, digests:)
    st = File.lstat(full)
    kind = KINDS.fetch(st.ftype, st.ftype)
    View.of(kind: kind, size: kind == "file" ? st.size : nil, mode: st.mode & 0o7777, created: nanoseconds(st.birthtime),
            modified: nanoseconds(st.mtime), target: kind == "symlink" ? File.readlink(full).b : nil,
            sha256: digests ? checksum(full, kind) : nil)
  end

  # The checksum of a file's bytes or, as E5 records it, of a symbolic link's
  # target; nil for other kinds.
  def checksum(full, kind)
    case kind
    when "file" then file_digest(full)
    when "symlink" then Digest::SHA256.hexdigest(File.readlink(full))
    end
  end

  # The checksum of a file; nil, so not compared, when the mounting user may not
  # read it (the manifest hashes such a file after making it readable).
  def file_digest(full)
    Digest::SHA256.file(full).hexdigest
  rescue Errno::EACCES
    nil
  end

  # A `t --json` entry record; its symlink target is not listed, so nil there
  # leaves the expected target uncompared by `differences`.
  def view_of_record(record)
    kind = KINDS.fetch(record.fetch("kind"), record.fetch("kind"))
    View.of(kind: kind, size: kind == "file" ? record.fetch("size_bytes").to_i : nil, mode: record.fetch("mode")&.&(0o7777),
            created: record_nanoseconds(record, "created"), modified: record_nanoseconds(record, "modified"))
  end

  def record_nanoseconds(record, field)
    time = record.fetch(field)
    return unless time

    time.fetch("seconds").to_i * 1_000_000_000 + time.fetch("nanoseconds").to_i
  end

  # Fields an observed view may lack: a listing does not show a symbolic link's
  # target, and an unreadable file has no checksum.
  UNLISTED = %i[target sha256].freeze

  # Each difference between the expected tree and an observed one, in the given
  # fields. A field expected as nil is not compared, and a field of UNLISTED only
  # where the observed view has it.
  def differences(expected, observed, fields)
    missing = (expected.keys - observed.keys).map { "#{_1.inspect} missing" }
    extra = (observed.keys - expected.keys).map { "#{_1.inspect} unexpected" }
    changed = expected.flat_map do |path, want|
      got = observed[path]
      next [] unless got

      fields.filter_map do |field|
        wanted = want.public_send(field)
        seen = got.public_send(field)
        next if wanted.nil? || (UNLISTED.include?(field) && seen.nil?)

        "#{path.inspect} #{field} #{seen.inspect}, not #{wanted.inspect}" unless wanted == seen
      end
    end
    missing + extra + changed
  end

  # Tool output as text for the record, whatever bytes it held.
  def text(bytes) = bytes.dup.force_encoding("UTF-8").scrub

  # ---- fsck_apfs and fsimg output --------------------------------------------

  # fsck_apfs -n narrates each check with these lines; any other line is a
  # finding.
  FSCK_PROGRESS = [/\A\*\* (Checking|Verifying) /, /\A\*\* The (volume|container) \S+ .*appears to be OK\.\z/,
                   /\AChecking the checkpoint /, /\AThe volume .* was formatted by .* and last modified by /].freeze

  def fsck_findings(out)
    out.lines.map(&:strip).reject { |line| line.empty? || FSCK_PROGRESS.any? { _1.match?(line) } }
  end

  # Whether `fsimg --formats` lists apfs readable and verifiable.
  def supports_apfs?(formats)
    row = formats.lines.map(&:split).find { _1.first == "apfs" }
    !row.nil? && row[1] == "yes" && row[3] == "yes"
  end

  # ---- Images ----------------------------------------------------------------

  # How an image is wrapped, from its bytes: dmg (a UDIF file, which wraps
  # either of the others), raw (a container) or gpt (a GUID partition table).
  def layout_of(path)
    File.open(path, "rb") do |file|
      return "dmg" if magic?(file, file.size - 512, UDIF_SIGNATURE)
      return "raw" if magic?(file, 32, ApfsCapture::CONTAINER_MAGIC)
      return "gpt" if magic?(file, GPT_OFFSET, GPT_SIGNATURE)
    end
    raise Error, "#{path} is neither a raw container, a GPT disk nor a UDIF file"
  end

  def magic?(file, offset, text) = offset >= 0 && offset + text.bytesize <= file.size && file.pread(text.bytesize, offset) == text

  # The image's name is that of its capture directory when it is the capture's
  # image.img.
  def image_name(source)
    base = File.basename(source)
    name = base.start_with?("image.") ? File.basename(File.dirname(File.expand_path(source))) : File.basename(base, ".*")
    name.gsub(/[^\w.+-]/, "_")
  end

  def images(sources, manifest:, skip_manifest:)
    raise Error, "--manifest names one manifest, so one image" if manifest && sources.size != 1

    made = sources.map do |source|
      raise Error, "#{source} is not a file" unless File.file?(source)

      manifest_path = find_manifest(source, given: manifest, skip: skip_manifest)
      Image.new(name: image_name(source), source: source, layout: layout_of(source), manifest: manifest_path,
                tree: manifest_path && parse_manifest(File.read(manifest_path, mode: "rb")))
    end
    raise Error, "an image may not be named results.json" if made.any? { _1.name == "results.json" }
    dupes = made.map(&:name).tally.select { _2 > 1 }.keys
    raise Error, "images with one name: #{dupes.join(', ')}" unless dupes.empty?

    made
  end

  # ---- The plan ----------------------------------------------------------------

  STEPS = %i[fsck_before mount modify fsck_after remount fsimg].freeze

  BASELINE = "no manifest, the first mount's view is the baseline"

  # Which steps skip their work and why; a step absent from the result does it
  # all. `fsimg` is the reason the fsimg steps cannot run, or nil.
  def plan(image, fsimg:)
    { mount: (BASELINE unless image.manifest), fsimg: fsimg }.compact
  end

  # Why the fsimg steps cannot run, or nil when they can.
  def fsimg_skip(tools, formats)
    return "no --fsimg given" unless tools.fsimg
    return "#{tools.fsimg} does not list apfs with Read and Verify" unless supports_apfs?(formats)

    nil
  end

  # ---- One image's trip through the cycle ------------------------------------

  # Which modifications a read-write mount completed and which it refused, as
  # "step: reason", the failure that ended them (nil if none) and the tree that
  # leaves.
  Outcome = Data.define(:tree, :completed, :refused, :failure)

  class Cycle
    # An attempt that did nothing, with the reason.
    Skipped = Data.define(:reason)

    # The volume mounts at a directory of its own under `mounts`, apart from
    # the image's directory.
    attr_reader :path

    def initialize(image, tools, directory, mounts:, options:, skip_fsimg:)
      @image = image
      @tools = tools
      @directory = directory
      @path = File.join(directory, image.layout == "dmg" ? "image.dmg" : "image.img")
      @mountpoint = File.join(mounts, image.name)
      @options = options
      @skip_fsimg = skip_fsimg
      @log = ApfsCapture::Log.new
      @attachments = ApfsCapture::Attachments.new(@log)
      @tree = image.tree
      @refusals = []
      @completed = []
    end

    # Run every step and return the image's row of the results. An interrupt
    # also detaches what was attached. An attach it cut short may leave a device
    # no one knows of, which the next run's check of `leftovers` refuses.
    def run
      FileUtils.mkdir_p(@mountpoint)
      begin
        FileUtils.cp(@image.source, @path)
        steps = STEPS.to_h { [_1, attempt(_1)] }
      ensure
        released = release
        ApfsQualify.remove_empty(@mountpoint)
      end
      { image: @image.name, layout: @image.layout, **steps, completed: @completed, refused: @refusals, detach: released }
    ensure
      File.write(File.join(@directory, "commands.txt"), @log.to_s)
    end

    private

    # The boundary of one step: any failure of the step, whatever its class,
    # becomes the step's result so that the steps after it still run. Interrupts
    # are not StandardError and stop the run.
    def attempt(step)
      return "skipped: #{@skip_fsimg}" if step == :fsimg && @skip_fsimg

      result = send(step)
      result.is_a?(Skipped) ? "skipped: #{result.reason}" : "pass"
    rescue StandardError => e
      "FAIL: #{ApfsQualify.text(ApfsQualify.text(e.message).lines.first(12).join.strip.byteslice(0, OUTPUT_EXCERPT_BYTES))}"
    end

    def release
      @attachments.release
      forced = @log.to_s.lines.grep(/\A\$ \S*hdiutil detach .* -force/)
      forced.empty? ? "pass" : "FAIL: forced #{forced.size} detach"
    rescue ApfsCapture::Error => e
      "FAIL: #{ApfsQualify.text(e.message)}"
    end

    def raw? = @image.layout != "dmg"

    # What the volume should show; none until the mount step has taken one.
    def baseline = @tree || raise(Error, "no baseline: the mount step did not complete")

    def fsck_before = fsck
    def fsck_after = fsck

    def fsck
      out, status = @attachments.with(@path, raw: raw?) do |attached|
        @log.run_status(ApfsCapture.tool("fsck_apfs"), "-n", ApfsCapture.raw_device(attached.store),
                        timeout: ApfsCapture::FSCK_TIMEOUT)
      end
      text = ApfsQualify.text(out)
      findings = ApfsQualify.fsck_findings(text)
      raise Error, "fsck_apfs findings: #{findings.first(8).join('; ')}" unless findings.empty?
      raise Error, "fsck_apfs exit #{status.inspect}: #{text}" unless status == 0
      raise Error, "fsck_apfs did not report the container OK: #{text}" unless text.match?(/^\*\* The container .* appears to be OK\./)
    end

    # With a manifest the mount is judged against it. Without, it takes the
    # tree the volume shows as the baseline for the remount.
    def mount
      observed = read_only { observe(digests: true) }
      return report_differences("mount", observed, COMPARED) if @tree

      @tree = observed
      Skipped.new(ApfsQualify::BASELINE)
    end

    def remount
      baseline
      report_differences("remount", read_only { observe(digests: true) }, COMPARED)
    end

    def read_only(&) = mounted(readonly: true, &)

    # Attach with the volume mounted at the mount point, which must then be a
    # mount: a failed mount that leaves the empty host directory would otherwise
    # be read, or written, as the volume.
    def mounted(readonly:)
      @attachments.with(@path, mountpoint: @mountpoint, raw: raw?, readonly: readonly, options: MOUNT_OPTIONS) do
        raise Error, "#{@mountpoint} is not a mount point" if File.stat(@mountpoint).dev == File.stat(File.dirname(@mountpoint)).dev

        yield
      end
    end

    def report_differences(what, observed, fields)
      differences = ApfsQualify.differences(baseline, observed, fields)
      raise Error, "#{what}: #{differences.first(8).join('; ')} (#{differences.size} differences)" unless differences.empty?
    end

    # The volume, by `find` and `stat`, with macOS's own additions to the root
    # left out unread.
    def observe(digests:)
      errors = File.join(@directory, "find-stderr")
      out, code = @log.run_status(ApfsCapture.tool("find"), @mountpoint, "-mindepth", "1", *native_prune, "-print0",
                                  timeout: COMMAND_TIMEOUT, stderr: errors)
      raise Error, "find exit #{code.inspect}: #{ApfsQualify.text(File.binread(errors, OUTPUT_EXCERPT_BYTES))}" unless code == 0

      out.split("\0".b).to_h { [_1.byteslice(@mountpoint.bytesize..).b, ApfsQualify.view_of(_1, digests: digests)] }
    end

    def native_prune
      paths = NATIVE_ROOT.flat_map { ["-o", "-path", File.join(@mountpoint, _1)] }.drop(1)
      ["(", *paths, ")", "-prune", "-o"]
    end

    def modify
      tree = baseline
      outcome = mounted(readonly: false) do
        Modification.new(root: @mountpoint, tree: tree, tolerate_full: @options.fetch(:tolerate_full),
                         large_bytes: @options.fetch(:large_bytes)).run
      end
      @tree = outcome.tree
      @refusals = outcome.refused
      @completed = outcome.completed
      raise Error, "modification failed: #{outcome.failure}" if outcome.failure
    rescue Errno::EROFS
      # Only a UDIF file can be read-only by design; a container or GPT disk
      # that mounts read-only is the writer's fault.
      raise unless @image.layout == "dmg"

      Skipped.new("the image is read-only (a compressed UDIF file, for one)")
    end

    def fsimg
      verify, code = @log.run_status(@tools.fsimg, "W", "-f", @path, timeout: COMMAND_TIMEOUT)
      raise Error, "fsimg W exit #{code}: #{ApfsQualify.text(verify)}" unless code == 0

      report_differences("fsimg t --json", fsimg_entries, LISTED)
      check_delivery
    end

    def fsimg_entries
      fsimg_output("t", "--json", "-f", @path).lines.filter_map do |line|
        record = JSON.parse(line)
        next unless record["type"] == "entry"

        path = record.fetch("path")
        [path.b, ApfsQualify.view_of_record(record)] unless ApfsQualify.native_addition?(path)
      end.to_h
    end

    # `x -O` delivers each stored file's bytes.
    def check_delivery
      baseline.each do |path, view|
        next unless view.regular? && view.sha256 && view.size.to_i.positive?

        digest = Digest::SHA256.hexdigest(fsimg_output("x", "-O", "-f", @path, path.dup.force_encoding("UTF-8")))
        raise Error, "x -O #{path.inspect} delivers #{digest}, not #{view.sha256}" unless digest == view.sha256
      end
    end

    # What the command writes on stdout, stderr apart so that it cannot mix in.
    def fsimg_output(*args)
      errors = File.join(@directory, "fsimg-stderr")
      out, code = @log.run_status(@tools.fsimg, *args, timeout: COMMAND_TIMEOUT, stderr: errors)
      raise Error, "fsimg #{args.first} exit #{code}: #{ApfsQualify.text(File.binread(errors, OUTPUT_EXCERPT_BYTES))}" unless code == 0

      out.force_encoding("UTF-8")
    end
  end

  # The read-write mount's work: each step changes the volume and returns the
  # tree it now shows. A volume that may be `tolerate_full` can refuse a step
  # with ENOSPC, whose own folder then goes away so that the tree stays as it
  # was; any other failure ends the modifications.
  class Modification
    PATTERN = "fsimg qualification\n"
    Step = Data.define(:name, :folder, :run)

    def initialize(root:, tree:, tolerate_full:, large_bytes:)
      @root = root
      @tree = tree
      @tolerate_full = tolerate_full
      @large_bytes = large_bytes
      @originals = tree.select { |path, view| view.regular? && path.count("/") == 1 }.keys.sort
    end

    # Run every step and return the Outcome. A step that fails, or that a
    # volume without room refuses, leaves its folder removed so that the tree
    # stays what the volume shows; a failure ends the run, a refusal does not.
    def run
      tree = @tree
      completed = []
      refused = []
      failure = nil
      steps.each do |step|
        tree = step.run.call(tree)
        completed << step.name
      rescue Errno::EROFS
        raise
      rescue SystemCallError => e
        FileUtils.rm_rf(File.join(@root, step.folder)) if step.folder
        reason = ApfsQualify.text("#{step.name}: #{e.message}")
        if e.is_a?(Errno::ENOSPC) && @tolerate_full
          refused << reason
        else
          failure = reason
          break
        end
      end
      Outcome.new(tree: tree, completed: completed, refused: refused, failure: failure)
    end

    private

    def steps
      [Step.new(name: "create", folder: "q-create", run: method(:create)),
       Step.new(name: "append", folder: "q-append", run: method(:append)),
       Step.new(name: "rename", folder: "q-rename", run: method(:rename)),
       Step.new(name: "mkdir-rmdir", folder: "q-folders", run: method(:folders)),
       Step.new(name: "unlink", folder: "q-unlink", run: method(:unlink_new)),
       Step.new(name: "large-file", folder: "q-large", run: method(:large_file)),
       *original_steps]
    end

    # The same operations on what the writer stored, each on a file of its own,
    # while enough files exist.
    def original_steps
      renamed, unlinked = @originals
      [original_step("rename-original", renamed) { rename_original(_1, renamed) },
       original_step("unlink-original", unlinked) { unlink_original(_1, unlinked) }].compact
    end

    def original_step(name, path, &run) = path && Step.new(name: name, folder: nil, run: run)

    def full(path) = File.join(@root.b, path)

    # A folder this run makes. Dir.mkdir fails on anything already there, a
    # symbolic link from the image included, which mkdir_p would follow out of
    # the volume; the folders beneath are then ones this run made.
    def folder(name) = Dir.mkdir(File.join(@root, name)).then { ["/#{name}".b, View.of(kind: "directory")] }
    def file_view(bytes) = View.of(kind: "file", size: bytes.bytesize, sha256: Digest::SHA256.hexdigest(bytes))

    def write(path, bytes)
      File.binwrite(File.join(@root, path), bytes)
      ["/#{path}".b, file_view(bytes)]
    end

    # A file and a symbolic link to it.
    def create(tree)
      made = [folder("q-create"), write("q-create/new", PATTERN * 500)]
      File.symlink("new", File.join(@root, "q-create/link"))
      tree.merge((made + [["/q-create/link".b, View.of(kind: "symlink", target: "new".b)]]).to_h)
    end

    # Each round fsyncs more bytes onto `data` and a block of a new file right
    # after it, so that the next round cannot extend the data in place.
    def append(tree)
      made = [folder("q-append")]
      chunks = APPEND_SIZES.cycle.take(FRAGMENT_ROUNDS).each_with_index.map { |size, round| append_bytes(size, round) }
      chunks.each_with_index do |chunk, round|
        File.open(File.join(@root, "q-append/data"), "ab") { sync(_1, chunk) }
        spacer = format("q-append/spacer-%02d", round)
        File.open(File.join(@root, spacer), "wb") { sync(_1, PATTERN) }
        made << ["/#{spacer}".b, file_view(PATTERN)]
      end
      tree.merge((made + [["/q-append/data".b, file_view(chunks.join.b)]]).to_h)
    end

    def append_bytes(size, round) = ((PATTERN * (size / PATTERN.bytesize + 1)).byteslice(0, size - 1) + "#{round % 10}").b

    def sync(file, bytes)
      file.write(bytes)
      file.fsync
    end

    # Within a folder, and from one folder to another.
    def rename(tree)
      made = [folder("q-rename"), folder("q-rename/from"), folder("q-rename/to")]
      _, view = write("q-rename/from/old", PATTERN * 3)
      File.rename(File.join(@root, "q-rename/from/old"), File.join(@root, "q-rename/from/new"))
      File.rename(File.join(@root, "q-rename/from/new"), File.join(@root, "q-rename/to/moved"))
      tree.merge((made + [["/q-rename/to/moved".b, view]]).to_h)
    end

    def folders(tree)
      kept = folder("q-folders")
      Dir.mkdir(File.join(@root, "q-folders/kept"))
      Dir.mkdir(File.join(@root, "q-folders/gone"))
      Dir.rmdir(File.join(@root, "q-folders/gone"))
      tree.merge([kept, ["/q-folders/kept".b, View.of(kind: "directory")]].to_h)
    end

    def unlink_new(tree)
      kept = folder("q-unlink")
      made = write("q-unlink/before", PATTERN * 2)
      write("q-unlink/gone", PATTERN)
      File.unlink(File.join(@root, "q-unlink/gone"))
      tree.merge([kept, made].to_h)
    end

    # One file of `large_bytes`, in chunks that each differ, flushed at the end.
    def large_file(tree)
      kept = folder("q-large")
      digest = Digest::SHA256.new
      File.open(File.join(@root, "q-large/big"), "wb") do |file|
        (0...@large_bytes).step(CHUNK_BYTES).each_with_index do |offset, index|
          chunk = Random.new(index).bytes([CHUNK_BYTES, @large_bytes - offset].min)
          file.write(chunk)
          digest << chunk
        end
        file.fsync
      end
      tree.merge([kept, ["/q-large/big".b, View.of(kind: "file", size: @large_bytes, sha256: digest.hexdigest)]].to_h)
    end

    def rename_original(tree, path)
      File.rename(full(path), File.join(@root, "q-renamed"))
      tree.except(path).merge("/q-renamed".b => tree.fetch(path))
    end

    def unlink_original(tree, path)
      File.unlink(full(path))
      tree.except(path)
    end
  end

  # ---- Running ---------------------------------------------------------------

  # The disk images attached from under `root`, by `hdiutil info`, which names
  # them by their real paths. A failed `hdiutil info` says nothing about them.
  def leftovers(root)
    out, ok = ApfsCapture::Log.new.run(ApfsCapture.tool("hdiutil"), "info")
    raise Error, "hdiutil info failed: #{text(out)}" unless ok

    real = "#{File.realpath(root)}/".b
    out.lines.filter_map { _1[/\Aimage-path\s*:\s*(.*)$/, 1] }.select { _1.start_with?(real) }
  end

  def result_table(rows)
    columns = STEPS + [:detach]
    header = "| Image | Layout | #{columns.join(' | ')} | Refused |"
    rule = "|#{' --- |' * (columns.size + 3)}"
    lines = rows.map do |row|
      cells = columns.map { row.fetch(_1)[0, 60].gsub(/[\n|]/, " ") }
      refused = row.fetch(:refused).map { _1.split(":").first }.join(", ")
      "| #{row.fetch(:image)} | #{row.fetch(:layout)} | #{cells.join(' | ')} | #{refused} |"
    end
    [header, rule, *lines].join("\n")
  end

  def parse(argv)
    options = { large_bytes: LARGE_BYTES, tolerate_full: false, skip_manifest: false }
    OptionParser.new do |o|
      o.banner = "usage: qualify.rb --root DIR [--fsimg PATH] [--manifest FILE | --no-manifest] [options] IMAGE..."
      o.on("--root DIR", "a disposable directory, private to this user, for the copies") { options[:root] = File.expand_path(_1) }
      o.on("--fsimg PATH", "the built fsimg; its W, t and x run only if it lists apfs") { options[:fsimg] = File.expand_path(_1) }
      o.on("--manifest FILE", "the expected manifest of the one image") { options[:manifest] = File.expand_path(_1) }
      o.on("--no-manifest", "do not look for a manifest beside the images") { options[:skip_manifest] = true }
      o.on("--large-bytes N", Integer, "size of the large file written natively (default #{LARGE_BYTES})") do
        raise OptionParser::InvalidArgument, "--large-bytes must be positive" unless _1.positive?

        options[:large_bytes] = _1
      end
      o.on("--tolerate-full", "a modification refused for lack of space is recorded, not a failure") { options[:tolerate_full] = true }
      o.on("--keep", "keep each passing copy too; a failed one is always kept") { options[:keep] = true }
      o.on("--list", "print each image's layout, manifest and skipped steps, and stop") { options[:list] = true }
    end.parse!(argv).then { |sources| [options, sources] }
  end

  # Each run writes, renames and removes beneath root, so it may not be a
  # symbolic link, open to another user's changes or inside a git work tree.
  def private_directory(path)
    raise Error, "#{path} lies inside a git work tree" if ApfsCapture.inside_work_tree?(path)

    FileUtils.mkdir_p(path)
    stat = File.lstat(path)
    return if stat.directory? && stat.uid == Process.euid && (stat.mode & 0o022).zero?

    raise Error, "#{path} is not a directory private to this user"
  end

  # Remove a mount point or the directory of them only when it is empty. A
  # recursive removal would reach into a volume left mounted, so one still in
  # use or holding files stays, with a warning; a missing one is already gone.
  # The removal runs in ensure blocks, so it never raises over the real error.
  def remove_empty(directory)
    Dir.rmdir(directory)
  rescue Errno::ENOENT
    nil
  rescue SystemCallError => e
    warn "qualify.rb: left #{directory}: #{e.message}"
  end

  def write_results(root, rows)
    path = File.join(root, "results.json")
    File.write("#{path}.new", JSON.pretty_generate(rows))
    File.rename("#{path}.new", path)
  end

  def passed?(row) = (STEPS + [:detach]).all? { row.fetch(_1).start_with?("pass", "skipped") }

  # The fsimg skip reason of this run, from `fsimg --formats`.
  def fsimg_reason(tools)
    formats = tools.fsimg ? ApfsCapture::Log.new.run(tools.fsimg, "--formats").first : ""
    fsimg_skip(tools, formats)
  end

  # Run the images in turn. The results file holds every finished row, so that a
  # crash loses only the image it stopped.
  def run_images(chosen, tools, root, mounts:, options:)
    reason = fsimg_reason(tools)
    chosen.each_with_object([]) do |image, rows|
      directory = File.join(root, image.name)
      FileUtils.rm_rf(directory)
      FileUtils.mkdir_p(directory)
      cycle = Cycle.new(image, tools, directory, mounts: mounts, options: options, skip_fsimg: reason)
      row = cycle.run
      puts row.slice(:image, :layout, *STEPS, :refused).to_json
      FileUtils.rm_f(cycle.path) if passed?(row) && !options[:keep]
      rows << row
      write_results(root, rows)
    end
  end

  def main(argv)
    options, sources = parse(argv)
    raise Error, "no IMAGE given" if sources.empty?

    chosen = images(sources, manifest: options[:manifest], skip_manifest: options[:skip_manifest])
    tools = Tools.new(fsimg: options[:fsimg])
    if options[:list]
      reason = fsimg_reason(tools)
      chosen.each { puts "#{_1.name}\t#{_1.layout}\t#{_1.manifest || '-'}\t#{plan(_1, fsimg: reason).map { |step, why| "#{step}: #{why}" }.join('; ')}" }
      return 0
    end
    run_all(chosen, tools, options)
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "qualify.rb: #{e.message}"
    1
  end

  def run_all(chosen, tools, options)
    root = options.fetch(:root) { raise Error, "missing --root" }
    private_directory(root)
    # Each run removes the image directories beneath root, so nothing it reads may lie there.
    inputs = chosen.flat_map { [_1.source, _1.manifest] } + [tools.fsimg]
    inside = inputs.compact.find { File.realpath(_1).start_with?("#{File.realpath(root)}/") }
    raise Error, "#{inside} lies inside --root, whose image directories each run removes" if inside

    # An image attached by an earlier run lies under root, whose image
    # directories each run removes, so it must be detached first.
    stale = leftovers(root)
    raise Error, "already attached: #{stale.join(', ')}" unless stale.empty?

    FileUtils.rm_f(File.join(root, "results.json"))
    # The mount points live in a fresh private directory on the system disk.
    # It goes with rmdir, never a recursive removal, which would reach into a
    # volume left mounted.
    mounts = Dir.mktmpdir("fsimg-qualify-mounts-", "/tmp")
    rows = begin
      run_images(chosen, tools, root, mounts: mounts, options: options)
    ensure
      remove_empty(mounts)
    end
    puts result_table(rows)
    remaining = leftovers(root)
    warn "still attached: #{remaining.join(', ')}" unless remaining.empty?
    rows.all? { passed?(_1) } && remaining.empty? ? 0 : 1
  end
end

exit ApfsQualify.main(ARGV) if $PROGRAM_NAME == __FILE__
