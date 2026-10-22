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
    REVERSED = "out/reversed.img"
    # The second create of a manifest case, which reads the manifest's
    # lines reversed and must write the bytes create wrote
    # (specs/design-spec-testing.md#cases). It runs as a stage of its own
    # name, with captures of that name, but no recipe lists it: the case
    # runs it once create has accepted, and judges it by those bytes.
    REPRODUCTION = "create-reversed"
    # The image each create stage writes.
    WRITES = { "create" => CREATED, REPRODUCTION => REVERSED }.freeze
    MANIFESTS = [Manifest::NAME, Manifest::REVERSED].freeze
    # How the two created images are opened and compared.
    READ_FLAGS = File::RDONLY | File::NOFOLLOW | File::NONBLOCK | File::BINARY
    COMPARE_BYTES = 1 << 20
    # Longer than the coordinator's two two-second diagnostic attempts, so
    # fsimg's own deadline fires first and reports itself.
    GRACE_SECONDS = 10
    # The most bytes kept of each of a stage's stdout and stderr. A listing
    # takes about 550 bytes and the path per entry, so this holds every
    # case's text while trees stay below some 60,000 entries; longer output
    # is reported as output_exceeded.
    CAPTURE_BYTES = 64 << 20
    ADDRESS_SPACE_BYTES = 1 << 30
    SANITIZER_SUFFIX = "abort_on_error=0:log_path=stderr:exitcode=87"
    SANITIZER_VARIABLES = %w[ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS MSAN_OPTIONS].freeze
    # The creation settings and the create option each becomes.
    CREATE_SETTINGS = { size: ["-s"], sector: ["-o", "sector="], cluster: ["-o", "cluster="] }.freeze

    # fsimg is the binary's absolute path, and environment the harness's
    # own (ENV), from which PATH, the sanitizer options and the sandbox
    # policy are taken. address_space is whether RLIMIT_AS applies, which
    # the preflight establishes. observer sees each stage's group start and
    # end.
    def initialize(fsimg:, profile:, environment:, address_space:, observer: GroupObserver.new)
      @fsimg = fsimg
      @ceilings = profile.ceilings
      @environment = environment
      @observer = observer
      @address_space = address_space
    end

    # Runs stage of recipe in case_directory, an absolute path. sandbox is
    # the status the preflight established, or nil while it runs. stage is
    # one the recipe lists, or REPRODUCTION for a manifest case.
    def run(stage, recipe:, case_directory:, sandbox:)
      raise Error, "#{recipe.id}: the case directory #{case_directory} is not absolute" unless case_directory.start_with?("/")
      raise Error, "#{recipe.id} does not run #{FsimgSpec.shown(stage)}" unless runs?(recipe, stage)

      inputs_before = input_identities(case_directory, stage, recipe)
      create_destination(case_directory, recipe) if stage == "extract"
      execution = Invocation.run(command(stage, recipe), chdir: case_directory, env: fsimg_environment(case_directory),
                                                          limits:, observer: @observer)
      capture_problems = capture(case_directory, stage, execution)
      outcome = Outcome.classify(execution, stage: WRITES.key?(stage) ? "create" : stage, sandbox:)
      tree = stage == "extract" ? walk_destination(case_directory) : []
      problems = capture_problems + problems(case_directory, stage:, recipe:, execution:, outcome:, inputs_before:, tree:)
      StageResult.new(stage:, outcome:, execution:, tree:, problems:, footprint: Footprint.of(case_directory))
    end

    # fsimg's argv for stage of recipe, with relative operands. A listing
    # recurses by default, and extract fills the dest/ the runner created
    # (-E). create reads the manifest as an @ operand, with no -C: the case
    # directory is the working directory, and a manifest of generated
    # content opens nothing beneath it (specs/design-mtree.md#command-line).
    # A host-tree case stores the contents of source/ at the image root.
    def command(stage, recipe)
      case stage
      in "info" then [@fsimg, "i", *reading_options(recipe), "-f", image(recipe)]
      in "ls" then [@fsimg, "t", *reading_options(recipe), "-f", image(recipe)]
      in "extract" then [@fsimg, "x", *reading_options(recipe), "-E", "-f", image(recipe), "-C", "dest"]
      in "verify" then [@fsimg, "W", *reading_options(recipe), "-f", image(recipe)]
      in "verify-deep" then [@fsimg, "W", *reading_options(recipe), "-d", "-f", image(recipe)]
      in "create" if recipe.source.manifest? then [*create_options(recipe), "-f", CREATED, "@#{Manifest::NAME}"]
      in "create" then [*create_options(recipe), "-f", CREATED, "-C", "source", "."]
      in REPRODUCTION then [*create_options(recipe), "-f", REVERSED, "@#{Manifest::REVERSED}"]
      else raise Error, "#{recipe.id}: unknown stage #{FsimgSpec.shown(stage)}"
      end
    end

    # Compares the image the second create wrote with the one create wrote
    # and answers why they differ, or nil
    # (specs/design-spec-testing.md#cases). A matching image is consumed:
    # it is removed, so the case holds one image through the reading
    # stages, and a second call finds nothing to compare. The runner's
    # checks left both regular files with one link; each is read through a
    # descriptor that follows no link and does not block.
    def settle_reproduction(case_directory)
      reversed_path = File.join(case_directory, REVERSED)
      problem = File.open(File.join(case_directory, CREATED), READ_FLAGS) do |created|
        File.open(reversed_path, READ_FLAGS) do |reversed|
          sizes = [created.size, reversed.size]
          next "#{REVERSED} has #{sizes.last} bytes, #{CREATED} #{sizes.first}" unless sizes.uniq.one?

          offset = first_difference(created, reversed)
          "#{REVERSED} differs from #{CREATED} in the #{COMPARE_BYTES}-byte block at #{offset}" if offset
        end
      end
      File.unlink(reversed_path) unless problem
      problem
    end

    def limits
      seconds = @ceilings.seconds + GRACE_SECONDS
      Invocation::Limits.new(
        wall_seconds: seconds, cpu_seconds: seconds + 5,
        file_bytes: [@ceilings.image, @ceilings.output, @ceilings.scratch].max,
        address_space_bytes: @address_space ? ADDRESS_SPACE_BYTES : nil,
        capture_bytes: CAPTURE_BYTES
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

    def runs?(recipe, stage) = run_order(recipe).include?(stage)

    # The stages of recipe in the order a case runs them: its own, with a
    # manifest case's second create right after create.
    def run_order(recipe) =
      recipe.stages.flat_map { _1 == "create" && manifest_case?(recipe) ? ["create", REPRODUCTION] : [_1] }

    # The stages a case has run once stage has, stage last.
    def stages_run(recipe, stage) = run_order(recipe).then { _1.take(_1.index(stage) + 1) }

    # The offset of the first block in which two files of one size differ,
    # or nil.
    def first_difference(one, other)
      offset = 0
      while (block = one.read(COMPARE_BYTES))
        return offset unless block == other.read(COMPARE_BYTES)

        offset += block.bytesize
      end
      nil
    end

    def create_options(recipe) = [@fsimg, "c", *common_options, "-R", "-F", recipe.format, *settings(recipe)]

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

    # The identities of the files stage reads, which no stage may change:
    # the image, which create has written by any stage after it, and a
    # manifest case's manifests. An identity is the inode, size and times,
    # so a rewrite that restores the modification time still shows in the
    # change time. Each must be a regular file with one link, never a hard
    # link to a host file.
    def input_identities(case_directory, stage, recipe)
      inputs = [*(stage == "create" ? [] : [image(recipe)]), *(manifest_case?(recipe) ? MANIFESTS : [])]
      inputs.to_h do |name|
        found = identity(File.lstat(File.join(case_directory, name)))
        raise Error, "#{recipe.id}: #{name} is not a regular file with one link" unless found

        [name, found]
      end
    end

    def manifest_case?(recipe) = !recipe.reading? && recipe.source.manifest?

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
    def problems(case_directory, stage:, recipe:, execution:, outcome:, inputs_before:, tree:)
      [
        *input_problems(case_directory, inputs_before),
        *directory_problems(case_directory, recipe:, stage:),
        *tree_problems(tree),
        *residue_problems(case_directory, recipe, stage, outcome),
        *created_problems(case_directory, stage, outcome),
        ("a process of the group outlived fsimg" if execution.leftover)
      ].compact
    end

    def input_problems(case_directory, inputs_before) =
      inputs_before.filter_map do |name, before|
        identity(File.lstat(File.join(case_directory, name))) == before ? nil : "#{name} changed"
      rescue Errno::ENOENT
        "#{name} disappeared"
      end

    # The names the case directory may hold once stage has run, each with
    # the type lstat must report: the image, or a creation case's manifests
    # or source tree, scratch/ and out/, dest/ once extract has run, and
    # the captures of the stages run so far. A link in place of any could
    # send a capture or a walk to a host path, and any other name is a
    # write fsimg had no business making.
    def case_entries(recipe, stage)
      ran = stages_run(recipe, stage)
      {
        **inputs(recipe),
        "scratch" => "directory", "out" => "directory",
        **(ran.include?("extract") ? { "dest" => "directory" } : {}),
        **ran.flat_map { ["#{_1}.stdout", "#{_1}.stderr"] }.to_h { [_1, "file"] }
      }.transform_keys(&:b)
    end

    def inputs(recipe)
      if recipe.reading? then { IMAGE => "file" }
      elsif recipe.source.manifest? then MANIFESTS.to_h { [_1, "file"] }
      else { "source" => "directory" }
      end
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
    # reading stages and the model then open, and so must the second
    # create, whose image is compared with it.
    def created_problems(case_directory, stage, outcome)
      written = WRITES[stage]
      return [] unless written && outcome.kind == "accepted"

      stat = File.lstat(File.join(case_directory, written))
      if !stat.file? then ["#{written} is a #{stat.ftype}, not a file"]
      elsif stat.nlink != 1 then ["#{written} has #{stat.nlink} links"]
      else []
      end
    rescue Errno::ENOENT
      ["#{stage} accepted but wrote no #{written}"]
    end

    def tree_problems(tree)
      foreign = tree.reject { %w[file directory].include?(_1.type) }
      total = tree.sum { _1.type == "file" ? _1.size : 0 }
      [
        *foreign.map { "dest holds the #{_1.type} #{FsimgSpec.shown(_1.path)}" },
        ("dest holds #{total} bytes, beyond the output ceiling #{@ceilings.output}" if total > @ceilings.output)
      ].compact
    end

    # scratch/ must be empty and out/ hold nothing beyond the created
    # images, except what an error record names as retained.
    def residue_problems(case_directory, recipe, stage, outcome)
      error = outcome.records.find { _1["type"] == "error" } || {}
      written = created_images(recipe, stage, outcome).map { File.basename(_1) }
      [
        *leftovers(case_directory, "scratch", allowed: [retained_entry(error["retained_scratch"], "scratch")]),
        *leftovers(case_directory, "out", allowed: [*written, retained_entry(error["retained_output"], "out")])
      ]
    end

    # The images out/ may hold once stage has ended with outcome: the
    # image of each create stage that has accepted, the second create's
    # only until settle_reproduction removes it. A case runs
    # on past a create stage only when it accepts, so only stage itself
    # may not have.
    def created_images(recipe, stage, outcome)
      ran = stages_run(recipe, stage)
      ran.pop unless outcome.kind == "accepted"
      ran.filter_map { WRITES[_1] unless _1 == REPRODUCTION && _1 != stage }
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
