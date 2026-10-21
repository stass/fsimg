# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # What one stage did to one case: its outcome, the execution behind it,
  # the safety problems the checks after it found, for extract the
  # destination tree as walked, and the case directory's Footprint after
  # it.
  StageResult = Data.define(:stage, :outcome, :execution, :problems, :tree, :footprint) do
    def safe? = problems.empty?
  end

  # The disk a case directory takes (specs/design-spec-testing.md#runner):
  # the entries it holds that are not directories and their logical
  # bytes, as a walk with lstat finds them; a sparse image counts its
  # logical size. The walk is total: a directory it cannot list, such as
  # one fsimg left unreadable, adds nothing, so the checks after a stage,
  # not the footprint, report what the stage left behind.
  Footprint = Data.define(:files, :bytes) do
    def self.of(directory)
      children = begin
        NameFunnel.children(directory)
      rescue SystemCallError
        []
      end
      children.map { child(directory.b + "/".b + _1) }.reduce(new(files: 0, bytes: 0), :+)
    end

    def self.child(path)
      stat = File.lstat(path)
      stat.directory? ? of(path) : new(files: 1, bytes: stat.size)
    rescue SystemCallError
      new(files: 0, bytes: 0)
    end
    private_class_method :child

    def +(other) = with(files: files + other.files, bytes: bytes + other.bytes)
  end

  # The only code that knows fsimg's command syntax
  # (specs/design-spec-testing.md#runner). It runs one stage of one case
  # in that case's directory, with relative operands, a fixed environment
  # and the profile's limits, classifies the outcome and checks the case
  # directory afterwards.
  class Runner
    IMAGE = "image.img"
    CREATED = "out/created.img"
    # Longer than the coordinator's two two-second diagnostic attempts, so
    # fsimg's own deadline fires first and reports itself.
    GRACE_SECONDS = 10
    # Text the coordinator does not count, such as a sanitizer report.
    CAPTURE_ALLOWANCE = 1 << 20
    ADDRESS_SPACE_BYTES = 1 << 30
    SANITIZER_SUFFIX = "abort_on_error=0:log_path=stderr:exitcode=87"
    SANITIZER_VARIABLES = %w[ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS MSAN_OPTIONS].freeze
    # The creation settings and the create option each becomes.
    CREATE_SETTINGS = { size: ["-s"], sector: ["-o", "sector="], cluster: ["-o", "cluster="] }.freeze

    # fsimg is the binary's absolute path, and environment the harness's
    # own (ENV), from which PATH, the sanitizer options and the sandbox
    # policy are taken. text_bytes is the coordinator's text ceiling for
    # the limits passed, and address_space whether RLIMIT_AS applies; the
    # preflight establishes both. observer sees each stage's group start
    # and end.
    def initialize(fsimg:, profile:, environment:, text_bytes:, address_space:, observer: GroupObserver.new)
      @fsimg = fsimg
      @ceilings = profile.ceilings
      @environment = environment
      @observer = observer
      @text_bytes = text_bytes
      @address_space = address_space
    end

    # Runs stage of recipe in case_directory, an absolute path. sandbox is
    # the status the preflight established, or nil while it runs.
    def run(stage, recipe:, case_directory:, sandbox:)
      raise Error, "#{recipe.id}: the case directory #{case_directory} is not absolute" unless case_directory.start_with?("/")
      raise Error, "#{recipe.id} does not run #{FsimgSpec.shown(stage)}" unless recipe.stages.include?(stage)

      image_before = image_identity(case_directory, stage, recipe)
      create_destination(case_directory, recipe) if stage == "extract"
      execution = Invocation.run(command(stage, recipe), chdir: case_directory, env: fsimg_environment(case_directory),
                                                          limits:, observer: @observer)
      capture_problems = capture(case_directory, stage, execution)
      outcome = Outcome.classify(execution, stage:, sandbox:)
      tree = stage == "extract" ? walk_destination(case_directory) : []
      problems = capture_problems + problems(case_directory, stage:, recipe:, execution:, outcome:, image_before:, tree:)
      StageResult.new(stage:, outcome:, execution:, tree:, problems:, footprint: Footprint.of(case_directory))
    end

    # fsimg's argv for stage of recipe, with relative operands. A listing
    # recurses by default, extract fills the dest/ the runner created (-E),
    # and create stores the contents of source/ at the image root.
    def command(stage, recipe)
      case stage
      in "info" then [@fsimg, "i", *reading_options(recipe), "-f", image(recipe)]
      in "ls" then [@fsimg, "t", *reading_options(recipe), "-f", image(recipe)]
      in "extract" then [@fsimg, "x", *reading_options(recipe), "-E", "-f", image(recipe), "-C", "dest"]
      in "verify" then [@fsimg, "W", *reading_options(recipe), "-f", image(recipe)]
      in "verify-deep" then [@fsimg, "W", *reading_options(recipe), "-d", "-f", image(recipe)]
      in "create"
        [@fsimg, "c", *common_options, "-R", "-F", recipe.format, *settings(recipe), "-f", CREATED, "-C", "source", "."]
      else raise Error, "#{recipe.id}: unknown stage #{FsimgSpec.shown(stage)}"
      end
    end

    def limits
      seconds = @ceilings.seconds + GRACE_SECONDS
      Invocation::Limits.new(
        wall_seconds: seconds, cpu_seconds: seconds + 5,
        file_bytes: [@ceilings.image, @ceilings.output, @ceilings.scratch].max,
        address_space_bytes: @address_space ? ADDRESS_SPACE_BYTES : nil,
        capture_bytes: @text_bytes + CAPTURE_ALLOWANCE
      )
    end

    # The environment fsimg sees, and nothing else.
    def fsimg_environment(case_directory)
      sanitizers = SANITIZER_VARIABLES.to_h do |name|
        [name, [@environment[name], SANITIZER_SUFFIX].compact.reject(&:empty?).join(":")]
      end
      { "PATH" => @environment.fetch("PATH"), "LC_ALL" => "C", "TZ" => "UTC", "HOME" => case_directory,
        "TMPDIR" => File.join(case_directory, "scratch"), **sanitizers }
    end

    private

    def image(recipe) = recipe.reading? ? IMAGE : CREATED

    def reading_options(recipe) = [*common_options, *(recipe.forced_format ? ["-F", recipe.forced_format] : [])]

    # The profile's ceilings, the scratch directory and the sandbox policy,
    # which stays at fsimg's default unless the run requires a sandbox.
    def common_options
      limits = %i[seconds entries depth path output scratch fds].flat_map { ["-l", "#{_1}=#{@ceilings.limit(_1)}"] }
      sandbox = @environment["FSIMG_SPEC_SANDBOX"] == "required" ? ["-S", "required"] : []
      ["--json", "-m", @ceilings.memory.to_s, *limits, "-T", "scratch", *sandbox]
    end

    def settings(recipe)
      recipe.source.settings.sort.flat_map do |name, value|
        option, prefix = CREATE_SETTINGS.fetch(name) { raise Error, "#{recipe.id}: unknown creation setting #{name}" }
        [option, "#{prefix}#{value}"]
      end
    end

    # Writes the stage's stdout and stderr beside the image, creating each
    # file exclusively and without following a link, and answers the
    # problems: a name fsimg left in a capture's place is one.
    def capture(case_directory, stage, execution)
      { "stdout" => execution.stdout, "stderr" => execution.stderr }.filter_map do |stream, bytes|
        name = "#{stage}.#{stream}"
        answer = NameFunnel.create_file(case_directory, name) { _1.write(bytes) }
        "#{name} existed before its capture" unless answer == :created
      end
    end

    # Creates dest/ empty and private for extract to fill: fsimg fills the
    # directory -E names only when the caller owns it and no one else may
    # write it. The case directory check forbids dest/ before extract, so a
    # dest/ already there is a fault of the harness.
    def create_destination(case_directory, recipe)
      answer = NameFunnel.create_directory(case_directory, "dest")
      raise Error, "#{recipe.id}: dest could not be created before extract (#{answer})" unless answer == :created
    end

    # The image's identity, or nil before create: its inode, size and
    # times, so a rewrite that restores the modification time still shows
    # in the change time. The image must be a regular file with one link,
    # never a hard link to a host file.
    def image_identity(case_directory, stage, recipe)
      return nil if stage == "create"

      found = identity(File.lstat(File.join(case_directory, image(recipe))))
      raise Error, "#{recipe.id}: #{image(recipe)} is not a regular file with one link" unless found

      found
    end

    def identity(stat) = stat.file? && stat.nlink == 1 ? [stat.ino, stat.size, stat.mtime, stat.ctime] : nil

    # The extracted tree, or nothing when dest is absent or no directory;
    # the case directory check reports a dest of the wrong type. Files are
    # hashed only while the tree stays within the output ceiling.
    def walk_destination(case_directory)
      destination = File.join(case_directory, "dest")
      real_directory?(destination) ? TreeWalk.entries(destination, hash_bytes: @ceilings.output) : []
    end

    def real_directory?(path)
      File.lstat(path).directory?
    rescue Errno::ENOENT
      false
    end

    # The checks after every stage. Any problem fails the case under every
    # policy and ends it.
    def problems(case_directory, stage:, recipe:, execution:, outcome:, image_before:, tree:)
      [
        *image_problems(case_directory, recipe, image_before),
        *directory_problems(case_directory, recipe:, stage:),
        *tree_problems(tree),
        *residue_problems(case_directory, recipe, outcome),
        *created_problems(case_directory, stage, outcome),
        ("a process of the group outlived fsimg" if execution.leftover)
      ].compact
    end

    def image_problems(case_directory, recipe, image_before)
      return [] unless image_before

      identity(File.lstat(File.join(case_directory, image(recipe)))) == image_before ? [] : ["#{image(recipe)} changed"]
    rescue Errno::ENOENT
      ["#{image(recipe)} disappeared"]
    end

    # The names the case directory may hold once stage has run, each with
    # the type lstat must report: the image, or the source tree of a
    # creation case, scratch/ and out/, dest/ once extract has run, and
    # the captures of the stages run so far. Recipes list their stages in
    # run order. A link in place of any could send a capture or a walk to a
    # host path, and any other name is a write fsimg had no business
    # making.
    def case_entries(recipe, stage)
      ran = recipe.stages.take(recipe.stages.index(stage) + 1)
      {
        **(recipe.reading? ? { IMAGE => "file" } : { "source" => "directory" }),
        "scratch" => "directory", "out" => "directory",
        **(ran.include?("extract") ? { "dest" => "directory" } : {}),
        **ran.flat_map { ["#{_1}.stdout", "#{_1}.stderr"] }.to_h { [_1, "file"] }
      }.transform_keys(&:b)
    end

    # Every name in the case directory must be one the case may hold at
    # this point, of the type it must have.
    def directory_problems(case_directory, recipe:, stage:)
      entries = case_entries(recipe, stage)
      NameFunnel.children(case_directory).filter_map do |name|
        expected = entries[name]
        next "unexpected #{FsimgSpec.shown(name)} in the case directory" unless expected

        actual = File.lstat(case_directory.b + "/".b + name).ftype
        "#{FsimgSpec.shown(name)} is a #{actual}, not a #{expected}" unless actual == expected
      end
    end

    # An accepted create must leave its image as a regular file, which the
    # reading stages and the model then open.
    def created_problems(case_directory, stage, outcome)
      return [] unless stage == "create" && outcome.kind == "accepted"

      stat = File.lstat(File.join(case_directory, CREATED))
      if !stat.file? then ["#{CREATED} is a #{stat.ftype}, not a file"]
      elsif stat.nlink != 1 then ["#{CREATED} has #{stat.nlink} links"]
      else []
      end
    rescue Errno::ENOENT
      ["create accepted but wrote no #{CREATED}"]
    end

    def tree_problems(tree)
      foreign = tree.reject { %w[file directory].include?(_1.type) }
      total = tree.sum { _1.type == "file" ? _1.size : 0 }
      [
        *foreign.map { "dest holds the #{_1.type} #{FsimgSpec.shown(_1.path)}" },
        ("dest holds #{total} bytes, beyond the output ceiling #{@ceilings.output}" if total > @ceilings.output)
      ].compact
    end

    # scratch/ must be empty and out/ hold nothing beyond a creation
    # case's created.img, except what an error record names as retained.
    def residue_problems(case_directory, recipe, outcome)
      error = outcome.records.find { _1["type"] == "error" } || {}
      created = recipe.reading? ? nil : File.basename(CREATED)
      [
        *leftovers(case_directory, "scratch", allowed: [retained_entry(error["retained_scratch"], "scratch")]),
        *leftovers(case_directory, "out", allowed: [created, retained_entry(error["retained_output"], "out")])
      ]
    end

    # The entry of directory that a retained path reported by fsimg lies
    # in, or nil. fsimg retains a whole entry, a scratch directory such as
    # scratch/NAME or a staging directory such as out/STAGING/image, so the
    # first component under directory is what may remain.
    def retained_entry(path, directory)
      return nil unless path.is_a?(String) && path.start_with?("#{directory}/")

      path.delete_prefix("#{directory}/").split("/").first
    end

    def leftovers(case_directory, directory, allowed:)
      path = File.join(case_directory, directory)
      return [] unless real_directory?(path)

      (NameFunnel.children(path) - allowed.compact.map(&:b)).map { "#{directory}/#{FsimgSpec.shown(_1)} was left behind" }
    end
  end
end
