# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module CliFixtures
  private

  def compression_format(name:)
    @compression_formats ||= {}
    @compression_formats[name] ||= begin
      record = completed(invoke(arguments: ["--formats", "--json"]))
        .find { _1["type"] == "format" && _1["name"] == name }
      refute_nil(record, "#{name} must have a format catalogue row")
      record
    end
  end

  def xz_format = compression_format(name: "xz")
  def zstd_format = compression_format(name: "zstd")
  def compression_supported?(name:) = compression_format(name: name).fetch("capabilities") != 0
  def xz_supported? = compression_supported?(name: "xz")
  def zstd_supported? = compression_supported?(name: "zstd")

  def same_bytes(first:, second:)
    assert(FileUtils.compare_file(first, second), "bytes differ: #{first} and #{second}")
  end

  def listing(executable: @executable, root: @root, image:)
    result = invoke(executable: executable, root: root, arguments: ["tf", image])
    result.stdout.dup.force_encoding(Encoding::UTF_8).lines(chomp: true)
  end

  def private_directory(root:, name:, mode: 0o700)
    path = (root / name)
    path.mkdir
    path.chmod(mode)
    path
  end

  def accepted_letters(executable: @executable, root: @root, mode:)
    # The option letters mode accepts: each is given with an argument, and
    # one the parser does not refuse as unknown or foreign to the mode is
    # accepted. Mode letters (except -W, which verifies in c), -V and -M are
    # left out.
    skipped = "cxtiM".chars.to_set | (mode.start_with?("c") ? Set.new : Set["W"])
    return (("a".."z").to_a + ("A".."Z").to_a)
      .reject { skipped.include?(_1) }
      .select { accepted_letter?(executable: executable, root: root, mode: mode, letter: _1) }
  end

  def seeded_bytes(seed:, size:)
    # Seeded generated content as specs/design-mtree.md defines it.
    blocks = (0...((size + 31) / 32)).map { |index| [seed, index].pack("Q<Q<").then { Digest::SHA256.digest(_1) } }
    return blocks.join("".b)[0...size]
  end

  def generated_manifest(directory:, name:)
    # Write a manifest of generated files only into a new directory that
    # also holds an unrelated file, and return the seeded file's bytes.
    directory.mkdir
    (directory / "unrelated.txt").binwrite("not a source\n".b)
    seeded = seeded_bytes(seed: 7, size: 100)
    lines = [
      ". type=dir",
      "./sub type=dir mode=0755",
      "./sub/gen type=file fsimg.content=seeded fsimg.seed=7 size=100 sha256digest=#{seeded.then { Digest::SHA256.hexdigest(_1) }}",
      "./zero type=file fsimg.content=zero size=5000"
    ]
    (directory / name).write(lines.join("\n") + "\n")
    (directory / "reversed-#{name}").write(lines.reverse.join("\n") + "\n")
    seeded
  end

  def image_fixture(name:, format:, size: nil)
    arguments = ["cf", name, "-R", "-F", format]
    arguments += ["-s", size] if size
    invoke(
      executable: @executable,
      root: @root,
      arguments: [*arguments, "-C", "source", "."]
    )
  end

  def human_image = image_fixture(name: "human.img", format: "fat12")
  def exfat_image = image_fixture(name: "exfat.img", format: "exfat")

  def merge_fixtures
    %w[merge-a merge-b].zip(%w[x y]).each do |directory, member|
      FileUtils.mkdir_p(@root / directory / "sub")
      (@root / directory / "sub" / member).binwrite(member)
    end
  end

  def find_program(name)
    ENV
      .fetch("PATH", "")
      .split(File::PATH_SEPARATOR)
      .map { File.join(_1, name) }
      .find { File.file?(_1) && File.executable?(_1) }
  end

  def relative_path(path) = path.relative_path_from(Pathname.pwd).to_s

  def accepted_letter?(executable:, root:, mode:, letter:)
    stderr = invoke(executable: executable, root: root,
      arguments: [mode, "-#{letter}", "1"], expected: nil).stderr
    ["unknown option".b, "does not apply to mode".b, "-V takes no other".b]
      .none? { stderr.include?(_1) }
  end
end
