# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Check fsimg's APFS reader against independent evidence. Usage:
#
#   apfs-integration.rb FSIMG
#
# Three parts, each skipped with its reason when its input or tool is absent:
#
#   manifests  test/apfs-native/*_test.rb and the native tests of
#              test/cli/apfs_info_test.rb against the captures of
#              FSIMG_APFS_NATIVE: `t --json`, `x -O` and `x` agree with
#              each native manifest, `i` with diskutil.
#   captures   For each capture directory apfs-*/image.img: the facts fsimg
#              `i --json` reports (block size and count, volume UUIDs, names,
#              object and snapshot counts) equal those fsapfsinfo and
#              apfs-dump-quick report where they are built, and the verdict
#              of fsimg `W` and `fsck_apfs -n` (macOS) or apfsck (Linux)
#              must both accept to pass; two refusals skip with both reasons,
#              as do apfsck's known defects (APFSCK_REJECTS, APFSCK_ACCEPTS).
#              A capture above MAX_BYTES is skipped, never
#              attached or copied: a sparse image of 100 GiB to 128 TiB
#              would be read or densified for little gain.
#   corpus     Each specimen of FSIMG_APFS_CORPUS against the observed
#              columns of its MATRIX.md row, on a copy of the specimen.
#
# A skip is never a pass: the run prints every skip, exits 1 when anything
# failed, 0 when something passed and 77 otherwise. Captures are attached only
# as clonefile(2) clones, read only and unmounted (tools/apfs/qualify.rb);
# specimens are never attached.

require "json"
require "rbconfig"
require "tmpdir"
require_relative "../tools/apfs/qualify"

module ApfsIntegration
  MAX_BYTES = 16 << 30
  COPY_BYTES = 2 << 30
  TIMEOUT = 10 * 60
  TOP = File.expand_path("..", __dir__)
  # The native comparisons, which only this script runs. The CLI suite's
  # test/cli/apfs_info_test.rb compares `i` with diskutil on the same captures.
  NATIVE_TESTS = %w[test/apfs-native/listing_test.rb test/apfs-native/content_test.rb].freeze

  # One outcome. verdict is :pass, :fail, :skip or :unrecorded (a corpus
  # observation MATRIX.md does not hold yet, which is not a pass).
  Result = Data.define(:part, :subject, :verdict, :detail)

  # What one tool says about a container. A field the tool does not report
  # is nil and is not compared. counts are the volume superblock's file,
  # directory, symlink and other-object counters.
  Volume = Data.define(:uuid, :name, :counts, :snapshots)
  Facts = Data.define(:block_size, :blocks, :volumes)

  # A refusal may be a failure to run, so only two acceptances can agree.
  # An interrupted command or incomplete output has no verdict.
  class Verdict < Data.define(:outcome, :reason)
    OUTCOMES = %i[accepted refused no_verdict].freeze

    def initialize(outcome:, reason:)
      raise ArgumentError, "unknown verdict outcome: #{outcome.inspect}" unless OUTCOMES.include?(outcome)

      super
    end

    def accepted? = outcome == :accepted
    def refused? = outcome == :refused
    def no_verdict? = outcome == :no_verdict

    def agrees?(other) = accepted? && other.accepted?

    def to_s
      case outcome
      in :accepted then "ok"
      in :refused then "refused (#{reason})"
      in :no_verdict then "did not run (#{reason})"
      end
    end
  end

  # An empty listing needs no extraction, but still participates in the
  # corpus's uniform no-verdict guard and text comparison.
  NoFile = Data.define do
    def no_verdict? = false
    def to_s = "no file"
  end
  NO_FILE = NoFile.new

  module_function

  def result(part, subject, verdict, detail = "") = Result.new(part:, subject:, verdict:, detail:)

  def log = (@log ||= ApfsCapture::Log.new)

  def run(*argv, timeout: TIMEOUT, stderr: nil) = log.run_status(*argv, timeout: timeout, stderr: stderr)

  def oracle(path) = File.join(ENV.fetch("FSIMG_ORACLES", File.expand_path("~/.local/share/fsimg-oracles")), path)

  def find_oracle(path) = File.executable?(oracle(path)) ? oracle(path) : nil

  def bare_container?(image) = File.open(image, "rb") { _1.pread(4, 32) } == "NXSB"

  def darwin? = RUBY_PLATFORM.include?("darwin")

  def on_path?(name) = ENV.fetch("PATH", "").split(File::PATH_SEPARATOR).any? { File.executable?(File.join(_1, name)) }

  # ---- fsimg -----------------------------------------------------------------

  # fsimg's JSON records for argv and its exit status, standard error
  # discarded. A line that is not a record, such as the runner's timeout
  # note, is left out.
  def fsimg_run(fsimg, *argv)
    out, status = run(fsimg, *argv, stderr: File::NULL)
    [ApfsQualify.text(out).lines.select { _1.start_with?("{") }.map { JSON.parse(_1) }, status]
  end

  def fsimg_records(fsimg, *argv)
    records, status = fsimg_run(fsimg, *argv)
    verdict = records_verdict(records, status)
    raise ApfsCapture::Error, "fsimg #{verdict}" if verdict.no_verdict?

    records
  end

  def info_records(fsimg, image, selection) = fsimg_records(fsimg, "i", "--json", *selection, "-f", image)

  def volume_record(records)
    records.find { _1.fetch("type") == "error" } || records.find { _1.fetch("type") == "info" }
  end

  SLOTS = /holds \d+ volumes in slots (?<low>\d+) to (?<high>\d+)/
  EMPTY_SLOT = /\AAPFS volume slot \d+ is empty\z/

  # [selection, record] for each volume: selection is the arguments that
  # select it (the APFS partition of a partitioned disk, the slot of a
  # container holding several volumes) and record its `i` info or error.
  def volume_records(fsimg, image, partition = [])
    records = info_records(fsimg, image, partition)
    first = volume_record(records)
    if first.nil?
      apfs = records.select { _1.fetch("type") == "partition" && _1.fetch("format") == "apfs" }
      raise ApfsCapture::Error, "#{image}: i names no volume and #{apfs.length} APFS partitions" unless apfs.length == 1 && partition.empty?

      return volume_records(fsimg, image, ["--partition=#{apfs.first.fetch("index")}"])
    end
    slots = SLOTS.match(first.fetch("message", ""))
    return [[partition, first]] unless first.fetch("type") == "error" && slots

    range = Integer(slots[:low], 10)..Integer(slots[:high], 10)

    range.map { [*partition, "-o", "volume=#{_1}"] }.map { [_1, volume_record(info_records(fsimg, image, _1))] }
         .reject { |_, record| record.fetch("message", "").match?(EMPTY_SLOT) }
  end

  # volume_records supplies info only after exit 0 and a complete summary.
  # Error records remain refusals even when they describe a failure to run.
  def fsimg_facts(records)
    return Verdict.new(outcome: :no_verdict, reason: "no volume records") if records.empty?

    infos = records.map(&:last)
    refusal = infos.find { _1.fetch("type") == "error" }
    return Verdict.new(outcome: :refused, reason: refusal.fetch("message")) if refusal

    volumes = infos.map do |info|
      details = info.fetch("details")
      counts = %w[files directories symlinks other_objects].map { Integer(details.fetch(_1), 10) }
      Volume.new(uuid: info.fetch("uuid"), name: info.fetch("label"), counts: counts,
                 snapshots: Integer(details.fetch("snapshots"), 10))
    end
    first = infos.first
    Facts.new(block_size: first.fetch("cluster_bytes"), blocks: Integer(first.fetch("allocation_units"), 10), volumes: volumes)
  end

  # `W` over every volume: all must accept, and a missing verdict takes
  # precedence over a refusal so that an incomplete check cannot agree.
  def fsimg_verify(fsimg, image, records)
    raise ApfsCapture::Error, "no volumes to verify" if records.empty?

    verdicts = records.map(&:first).map do |selection|
      verdict = command_verdict(fsimg, "W", "--json", *selection, "-f", image)
      next verdict if verdict.accepted?

      reason = [selection.join(" "), verdict.reason].reject(&:empty?).join(": ")
      Verdict.new(outcome: verdict.outcome, reason: reason)
    end
    reason = verdicts.map(&:reason).reject(&:empty?).join("; ")
    outcome = if verdicts.any?(&:no_verdict?)
                :no_verdict
              elsif verdicts.all?(&:accepted?)
                :accepted
              else
                :refused
              end
    Verdict.new(outcome: outcome, reason: reason)
  end

  # ---- Oracles ---------------------------------------------------------------

  def fsapfsinfo_facts(text)
    volumes = text.split(/^Volume: \d+ information:$/).drop(1).map do |section|
      Volume.new(uuid: section[/^\s+Identifier\s+: (\S+)/, 1], name: section[/^\s+Name\s+: (.*)$/, 1],
                 counts: nil, snapshots: Integer(section[/^\s+Number of snapshots\s+: (\d+)/, 1], 10))
    end
    Facts.new(block_size: nil, blocks: nil, volumes: volumes)
  end

  DUMP_FIELDS = /^(num_files|num_directories|num_symlinks|num_other_fsobjs|num_snapshots|vol_uuid|volname)\s+: (.*)$/

  # apfs-dump-quick prints the container superblock first and then each
  # volume superblock, its counters before its UUID and its name last.
  def dump_facts(text)
    hex = ->(field) { text[/^\h{4} u\d+ #{field}\s+: (\h+)$/, 1]&.to_i(16) }
    volumes = text.scan(DUMP_FIELDS).slice_after { _1.first == "volname" }.select { _1.last.first == "volname" }.map do |group|
      fields = group.to_h
      counts = %w[num_files num_directories num_symlinks num_other_fsobjs].map { fields.fetch(_1).to_i(16) }
      Volume.new(uuid: fields.fetch("vol_uuid").downcase, name: fields.fetch("volname"), counts: counts,
                 snapshots: fields.fetch("num_snapshots").to_i(16))
    end
    Facts.new(block_size: hex.("block_size"), blocks: hex.("block_count"), volumes: volumes)
  end

  # Each fact the oracle reports that fsimg reports otherwise.
  def differences(fsimg, other)
    container = %i[block_size blocks].filter_map do |field|
      wanted = other.public_send(field)
      got = fsimg.public_send(field)
      "#{field}: oracle #{wanted}, fsimg #{got}" unless wanted.nil? || wanted == got
    end
    ours = fsimg.volumes.to_h { [_1.uuid, _1] }
    theirs = other.volumes.to_h { [_1.uuid, _1] }
    return container + ["volumes: oracle #{theirs.keys.sort}, fsimg #{ours.keys.sort}"] unless ours.keys.sort == theirs.keys.sort

    container + theirs.flat_map do |uuid, volume|
      %i[name counts snapshots].filter_map do |field|
        wanted = volume.public_send(field)
        got = ours.fetch(uuid).public_send(field)
        "#{uuid} #{field}: oracle #{wanted.inspect}, fsimg #{got.inspect}" unless wanted.nil? || wanted == got
      end
    end
  end

  # Captures fsimg refuses by design, with the refusal it must give. The
  # 91-block container is below the format's minimum, which fsck_apfs also
  # refuses; fsapfsinfo and apfs-dump-quick read it regardless.
  REFUSED = { "apfs-newfs-372736-plain" => "invalid block count" }.freeze

  def fact_result(part, name, facts, text)
    if facts.is_a?(Verdict)
      expected = facts.refused? && REFUSED[name]
      return result(part, name, :skip, "fsimg refuses it by design: #{facts.reason}") if expected && facts.reason.include?(expected)

      return result(part, name, :fail, "fsimg #{facts}")
    end

    found = differences(facts, yield(text))
    found.empty? ? result(part, name, :pass) : result(part, name, :fail, found.join("; "))
  end

  # Only two acceptances pass. Two refusals may conceal failures to run.
  def verdict_result(part, name, checker, verify)
    return result(part, name, :pass, "both #{checker}") if checker.agrees?(verify)
    if checker.refused? && verify.refused?
      return result(part, name, :skip, "both refused: fsimg #{verify.reason}; #{part} #{checker.reason}")
    end

    result(part, name, :fail, "#{part} #{checker}, fsimg W #{verify}")
  end

  # Native captures apfsck rejects with these findings although fsck_apfs
  # accepts them and fsimg verifies them: apfsck limits on name length and
  # its name hashes of newer Unicode marks, a role it counts as reserved, and
  # decmpfs resource-fork layouts macOS writes.
  APFSCK_REJECTS = {
    "apfs-diskutil-8589934592-e10-allroles" => "reserved role in use",
    "apfs-newfs-268435456-e3-ci" => "name is too long",
    "apfs-newfs-268435456-e3-cs" => "name is too long",
    "apfs-newfs-67108864-e14-ci" => "name is too long",
    "apfs-newfs-67108864-e14-cs" => "name is too long",
    "apfs-newfs-268435456-e4-names-ci" => "filename hash is corrupted",
    "apfs-newfs-268435456-e4-names-cs" => "filename hash is corrupted",
    "apfs-newfs-33554432-e4-u17marks-ci" => "filename hash is corrupted",
    "apfs-newfs-33554432-e4-u17marks-cs" => "filename hash is corrupted",
    "apfs-newfs-67108864-e6-xclone" => "block metadata is too big",
    "apfs-newfs-67108864-e7-compression" => "incompatible resource fork flags"
  }.freeze

  # Native captures apfsck accepts although fsck_apfs refuses them, which
  # fsimg refuses by design with this message (the REFUSED table).
  APFSCK_ACCEPTS = { "apfs-newfs-372736-plain" => "invalid block count" }.freeze

  # apfsck beside fsimg `W`. The two named tables of known apfsck defects
  # are skips with their reasons; every other pair is judged as any checker.
  def apfsck_result(name, checker, verify)
    rejects = APFSCK_REJECTS[name]
    if rejects && checker.refused? && verify.accepted? && checker.reason.include?(rejects)
      return result("apfsck", name, :skip, "apfsck rejects what fsck_apfs accepts: #{checker.reason}")
    end

    accepts = APFSCK_ACCEPTS[name]
    if accepts && checker.accepted? && verify.refused? && verify.reason.include?(accepts)
      return result("apfsck", name, :skip, "apfsck accepts what fsck_apfs refuses: #{verify.reason}")
    end

    verdict_result("apfsck", name, checker, verify)
  end

  def fsapfsinfo(name, image, facts)
    program = find_oracle("libfsapfs/bin/fsapfsinfo")
    return result("fsapfsinfo", name, :skip, "not built under #{oracle("libfsapfs")}") unless program

    out, status = run(program, image)
    return result("fsapfsinfo", name, :fail, "exit #{status.inspect}") unless status&.zero?

    fact_result("fsapfsinfo", name, facts, ApfsQualify.text(out)) { fsapfsinfo_facts(_1) }
  end

  def dump_quick(name, image, facts, work)
    program = find_oracle("apfs-fuse/bin/apfs-dump-quick")
    return result("apfs-dump-quick", name, :skip, "not built under #{oracle("apfs-fuse")}") unless program

    output = File.join(work, "dump.txt")
    _out, status = run(program, image, output)
    return result("apfs-dump-quick", name, :fail, "exit #{status.inspect}") unless status&.zero?

    fact_result("apfs-dump-quick", name, facts, ApfsQualify.text(File.binread(output))) { dump_facts(_1) }
  ensure
    FileUtils.rm_f(output) if output
  end

  FSCK_OK = /^\*\* The container .* appears to be OK\./
  # The closing "** " status line of a container fsck_apfs could not verify;
  # only status lines are matched, so a volume name cannot trigger it.
  FSCK_UNVERIFIED = /could not be verified|is corrupt/i

  # fsck_apfs -n on the capture's store, attached read only and unmounted
  # from a clone in work, which shares the capture's blocks.
  def fsck_apfs(name, image, verify, work)
    return result("fsck_apfs", name, :skip, "fsck_apfs runs on macOS only") unless darwin?

    clone = File.join(work, "clone.img")
    begin
      ApfsQualify.clone_image(image, clone)
    rescue ApfsCapture::Error => e
      return result("fsck_apfs", name, :skip, e.message)
    end
    attachments = ApfsCapture::Attachments.new(log)
    out, status = attachments.with(clone, raw: true, readonly: true) do |attached|
      run(ApfsCapture.tool("fsck_apfs"), "-n", ApfsCapture.raw_device(attached.store), timeout: ApfsCapture::FSCK_TIMEOUT)
    end
    text = ApfsQualify.text(out)
    findings = ApfsQualify.fsck_findings(text)
    outcome = case
              when status.nil? then :no_verdict
              when !findings.empty? || text.lines.grep(/\A\*\* /).last.to_s.match?(FSCK_UNVERIFIED) then :refused
              when status.zero? && text.match?(FSCK_OK) then :accepted
              else :no_verdict
              end
    reason = findings.first(3).join(" | ").then { _1.empty? ? "exit #{status.inspect}" : _1 }
    verdict_result("fsck_apfs", name, Verdict.new(outcome: outcome, reason: reason), verify)
  ensure
    FileUtils.rm_f(clone) if clone
  end

  # apfsck's verdict on each bare container: a built Linux apfsck run on the
  # image, or, with FSIMG_APFS_DOCKER=1, tools/apfs/apfsck-linux.sh, which
  # builds apfsprogs in Docker and so needs the network. name => Verdict, or
  # the reason it did not run.
  def apfsck_verdicts(images)
    program = ENV["APFSCK"] || find_oracle("apfsprogs/bin/apfsck")
    return images.to_h { |name, image| [name, apfsck_direct(program, image)] } if program
    return "apfsck builds on Linux only; set FSIMG_APFS_DOCKER=1 to run it through Docker, which fetches apfsprogs" \
      unless ENV["FSIMG_APFS_DOCKER"] == "1"
    return "FSIMG_APFS_DOCKER=1 but no docker in PATH" unless on_path?("docker")

    out, status = run("sh", File.join(TOP, "tools/apfs/apfsck-linux.sh"), *images.values, timeout: 60 * 60)
    if status.nil?
      return images.to_h { |name, _| [name, Verdict.new(outcome: :no_verdict, reason: "apfsck-linux.sh did not run to a verdict")] }
    end

    lines = ApfsQualify.text(out).lines.map(&:chomp)
    images.to_h do |name, image|
      at = lines.index { _1.start_with?("#{image}: ") }
      verdict = at && lines[at].delete_prefix("#{image}: ")
      outcome = { "pass" => :accepted, "FAIL" => :refused, "error" => :no_verdict }.fetch(verdict, :no_verdict)
      reason = if verdict == "pass"
                 ""
               elsif verdict&.start_with?("skipped:")
                 verdict
               else
                 apfsck_reason(lines, at)
               end
      [name, Verdict.new(outcome: outcome, reason: reason)]
    end
  end

  def apfsck_reason(lines, at)
    return "no verdict in apfsck-linux.sh output: #{lines.last(3).join(" | ")}" unless at

    lines.drop(at + 1).take_while { !_1.match?(/: (pass|FAIL|error)\z|: skipped:/) }.first(3).join(" | ")
  end

  def apfsck_direct(program, image)
    out, status = run(program, image)
    text = ApfsQualify.text(out).strip
    # A shell wrapper can report a signal as exit status 128 + signal,
    # rather than the runner observing a signaled process directly.
    outcome = case
              when status.nil? || status > 128 || [126, 127].include?(status) then :no_verdict
              when !text.empty? then :refused
              when status.zero? then :accepted
              else :no_verdict
              end
    reason = text.lines.first(3).map(&:strip).join(" | ")
    Verdict.new(outcome: outcome, reason: reason.empty? ? "exit #{status.inspect}" : reason)
  end

  # ---- Parts -----------------------------------------------------------------

  def manifests(fsimg, native)
    return [result("manifests", "native", :skip, "FSIMG_APFS_NATIVE names no directory")] unless native

    environment = { "FSIMG_CLI_EXECUTABLE" => fsimg, "FSIMG_APFS_NATIVE" => native }
    NATIVE_TESTS.map do |file, *options|
      out, status = log.run_status("env", *environment.map { "#{_1}=#{_2}" }, RbConfig.ruby,
                                   File.join(TOP, file), *options, timeout: 60 * 60)
      text = ApfsQualify.text(out)
      summary = text.lines.grep(/runs, .* assertions/).last.to_s.strip
      next result("manifests", file, :fail, text.lines.last(20).join) unless status&.zero?

      # Minitest exits 0 when every test skipped; that is not a pass.
      counts = summary.match(/(?<runs>\d+) runs, .* (?<skips>\d+) skips/)
      all_skipped = counts.nil? || counts[:runs] == counts[:skips]
      result("manifests", file, all_skipped ? :skip : :pass, summary)
    end
  end

  def captures(fsimg, native, work)
    return [result("captures", "native", :skip, "FSIMG_APFS_NATIVE names no directory")] unless native

    images = Dir[File.join(native, "apfs-*/image.img")].sort.to_h { [File.basename(File.dirname(_1)), _1] }
    large, checked = images.partition { |_, image| File.size(image) > MAX_BYTES }
    skips = large.map { |name, image| result("captures", name, :skip, "#{File.size(image)} bytes exceeds #{MAX_BYTES}") }
    bare = checked.select { |_, image| bare_container?(image) }.to_h
    apfsck = apfsck_verdicts(bare)
    skips + checked.flat_map do |name, image|
      capture_results(fsimg: fsimg, name: name, image: image, work: work, bare: bare.key?(name), apfsck: apfsck)
    rescue ApfsCapture::Error, JSON::ParserError, KeyError, TypeError, ArgumentError, SystemCallError => e
      [result("captures", name, :fail, "#{e.class}: #{e.message}")]
    end
  end

  def capture_results(fsimg:, name:, image:, work:, bare:, apfsck:)
    records = volume_records(fsimg, image)
    facts = fsimg_facts(records)
    verify = facts.is_a?(Verdict) ? facts : fsimg_verify(fsimg, image, records)
    oracles = if bare
                checker = apfsck.is_a?(String) ? result("apfsck", name, :skip, apfsck) : apfsck_result(name, apfsck.fetch(name), verify)
                [fsapfsinfo(name, image, facts), dump_quick(name, image, facts, work), checker]
              else
                %w[fsapfsinfo apfs-dump-quick apfsck].map { result(_1, name, :skip, "not a bare container") }
              end
    [fsck_apfs(name, image, verify, work), *oracles]
  end

  # ---- Corpus ----------------------------------------------------------------

  COMMANDS = %w[i t x W].freeze

  # The rows of MATRIX.md's results table: the specimen, its four expected
  # cells and its four observed cells.
  def matrix_rows(text)
    text.lines.grep(/\A\|/).map { _1.strip.delete_prefix("|").delete_suffix("|").split("|", -1).map(&:strip) }
        .select { _1.length == 9 }.reject { _1.first == "Specimen" || _1.first.start_with?("---") }
        .to_h { [_1.first, COMMANDS.zip(_1.last(4)).to_h] }
  end

  # The specimen file a row names: the file itself, or a directory's one file.
  def specimen_file(corpus, specimen)
    path = File.join(corpus, specimen)
    return path if File.file?(path)

    files = Dir.children(path).map { File.join(path, _1) }.select { File.file?(_1) }
    raise ApfsCapture::Error, "#{specimen}: #{files.length} files, not one" unless files.length == 1

    files.first
  end

  # A private copy of a specimen, which is untrusted: a clone where the work
  # directory shares its volume, else a byte copy of a file small enough.
  def specimen_copy(source, work)
    copy = File.join(work, "specimen#{File.extname(source)}")
    ApfsQualify.clone_image(source, copy)
    copy
  rescue ApfsCapture::Error
    raise ApfsCapture::Error, "#{source}: #{File.size(source)} bytes, too large to copy" if File.size(source) > COPY_BYTES

    FileUtils.cp(source, copy)
    copy
  end

  # `x -O` of file with its output discarded. -O takes no --json, so the
  # reason is fsimg's last diagnostic, the copy's path shown as IMAGE.
  # x cannot tell a refusal from a crash reported by fsimg. The corpus
  # compares the text, so a crash's text does not match a recorded refusal.
  def extract_verdict(fsimg, selection, file, work)
    errors = File.join(work, "x.err")
    child = Process.spawn(fsimg, "x", "-O", *selection, file, out: File::NULL, err: errors, pgroup: true)
    status = Timeout.timeout(TIMEOUT) { Process.wait2(child).last }
    return Verdict.new(outcome: :no_verdict, reason: "killed by signal") if status.signaled?

    line = File.read(errors).lines.map(&:strip).reject { _1.include?("sandbox unavailable") }.last.to_s
    image = selection.last
    return Verdict.new(outcome: :accepted, reason: "") if status.success?
    return Verdict.new(outcome: :no_verdict, reason: "exit #{status.exitstatus} without a diagnostic") if line.empty?

    Verdict.new(outcome: :refused, reason: line.delete_prefix("fsimg: ").gsub(image, "IMAGE"))
  rescue Timeout::Error
    begin
      Process.kill(:KILL, -child)
    rescue Errno::ESRCH, Errno::EPERM
      nil
    end
    Process.wait(child)
    Verdict.new(outcome: :no_verdict, reason: "timeout after #{TIMEOUT} seconds")
  rescue SystemCallError => e
    Verdict.new(outcome: :no_verdict, reason: e.message)
  ensure
    FileUtils.rm_f(errors)
  end

  # Accept only exit 0 with a complete summary record last. An error record
  # is a refusal, which may be a failure to run; refusals never pass a
  # checker comparison. The same rule gates the records used for facts.
  def command_verdict(fsimg, *argv)
    records, status = fsimg_run(fsimg, *argv)
    records_verdict(records, status)
  end

  def records_verdict(records, status)
    return Verdict.new(outcome: :no_verdict, reason: "could not start, killed or timed out") if status.nil?

    error = records.find { _1.fetch("type") == "error" }
    return Verdict.new(outcome: :refused, reason: error.fetch("status")) if error

    last = records.last
    complete = status.zero? && last && last.fetch("type") == "summary" && last.fetch("complete")
    return Verdict.new(outcome: :accepted, reason: "") if complete

    Verdict.new(outcome: :no_verdict, reason: "exit #{status} without a complete summary")
  end

  # What `i`, `t`, `x -O` of the first listed file and `W` give, on the first
  # volume of a container that holds several.
  def observe(fsimg, image, work)
    selection = [*volume_records(fsimg, image).first.first, "-f", image]
    listing = fsimg_records(fsimg, "t", "--json", *selection)
    file = listing.find { _1.fetch("type") == "entry" && _1.fetch("kind") == "file" }&.fetch("path")
    {
      "i" => command_verdict(fsimg, "i", "--json", *selection),
      "t" => command_verdict(fsimg, "t", "--json", *selection),
      "x" => file ? extract_verdict(fsimg, selection, file, work) : NO_FILE,
      "W" => command_verdict(fsimg, "W", "--json", *selection)
    }
  end

  def corpus(fsimg, directory, work)
    return [result("corpus", "corpus", :skip, "FSIMG_APFS_CORPUS names no directory")] unless directory

    matrix = File.join(directory, "MATRIX.md")
    return [result("corpus", "corpus", :fail, "#{matrix} is missing")] unless File.file?(matrix)

    matrix_rows(File.read(matrix)).map do |specimen, observed|
      copy = specimen_copy(specimen_file(directory, specimen), work)
      seen = observe(fsimg, copy, work)
      shown = COMMANDS.map { "#{_1}=#{seen.fetch(_1)}" }.join(", ")
      next result("corpus", specimen, :fail, shown) if seen.values.any?(&:no_verdict?)
      next result("corpus", specimen, :unrecorded, shown) if observed.values.any?(&:empty?)

      wrong = COMMANDS.reject { |command| seen.fetch(command).to_s == observed.fetch(command) }
      wrong.empty? ? result("corpus", specimen, :pass) : result("corpus", specimen, :fail, "#{shown}; MATRIX.md: #{observed}")
    rescue ApfsCapture::Error, JSON::ParserError, KeyError, SystemCallError => e
      result("corpus", specimen, :fail, "#{e.class}: #{e.message}")
    ensure
      FileUtils.rm_f(copy) if copy
    end
  end

  # ---- Main ------------------------------------------------------------------

  def directory(name) = ENV[name].to_s.empty? || !File.directory?(ENV[name]) ? nil : File.expand_path(ENV[name])

  def main(argv)
    fsimg = File.expand_path(argv.fetch(0))
    native = directory("FSIMG_APFS_NATIVE")
    specimens = directory("FSIMG_APFS_CORPUS")
    results = Dir.mktmpdir("apfs-integration-", Dir.pwd) do |work|
      manifests(fsimg, native) + captures(fsimg, native, work) + corpus(fsimg, specimens, work)
    end
    results.each { puts format("%-11s %-16s %s %s", _1.verdict.upcase, _1.part, _1.subject, _1.detail).rstrip }
    tally = results.map(&:verdict).tally
    puts "summary: #{%i[pass fail skip unrecorded].map { "#{tally.fetch(_1, 0)} #{_1}" }.join(", ")}"
    return 1 if tally.key?(:fail)

    return 0 if tally.key?(:pass)

    77
  end
end

exit(ApfsIntegration.main(ARGV)) if $PROGRAM_NAME == __FILE__
