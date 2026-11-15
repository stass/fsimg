# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The creation cases of the writer's profile: a source tree and its
    # settings, which create writes from a manifest. Each input rule has a
    # valid and an invalid creation; the profile creations cover the
    # profile's shapes, each personality, the label, an explicit size, a
    # filesystem tree of several levels and names at the length limits;
    # and draw gives seeded random trees, every one of which is valid.
    module Creations
      Creation = Data.define(:id, :source, :intended, :rules)

      BLOCK = WriterLayout::BLOCK
      # A name of 255 UTF-16 units of two-byte characters that do not
      # decompose, and one of four-byte characters, each counting two, with
      # one unit to spare: both within the profile's 512-byte path ceiling.
      LONG_BMP = ("\u0434" * 255).freeze
      LONG_ASTRAL = ("\u{1F600}" * 127).freeze
      E_ACUTE = "\u00e9"
      E_DECOMPOSED = "e\u0301"

      module_function

      def file(path, size, seed: 1) = TreeEntry.file(path, size:, content: size.zero? ? "zero" : "seeded", seed: size.zero? ? nil : seed)
      def directory(path) = TreeEntry.directory(path)

      # A tree with every kind the writer stores: nested directories, an
      # empty file, files ending inside, on and after a block boundary, a
      # symbolic link and a FIFO.
      def standard = [
        directory("docs"), directory("docs/nested"),
        file("docs/readme.txt", 100), file("docs/nested/#{E_ACUTE}t#{E_ACUTE}.txt", BLOCK + 1, seed: 2),
        file("empty", 0), file("block.bin", BLOCK, seed: 3), file("blocks.bin", 3 * BLOCK + 17, seed: 4),
        TreeEntry.symlink("link", target: "docs/readme.txt"), TreeEntry.fifo("pipe")
      ]

      # Enough entries that the filesystem tree takes an index level.
      def wide = [directory("wide"), *Array.new(400) { file(format("wide/entry-%03d", _1), 0) }]

      def input_ids = CreationRules.all.map(&:id)

      def creation(id, tree, settings = {}, intended: nil, rules: input_ids) =
        Creation.new(id:, source: Source.new(tree:, settings:), intended:, rules:)

      def profile = [
        creation("apfs.create.empty", []),
        creation("apfs.create.standard", standard),
        creation("apfs.create.sensitive", [*standard, file("Readme", 1), file("README", 2, seed: 5)], { case: "sensitive" }),
        creation("apfs.create.labelled", standard, { label: "Backup #{E_ACUTE}" }),
        creation("apfs.create.empty_label", standard, { label: "" }),
        creation("apfs.create.explicit_size", standard, { size: 16 * (1 << 20), sector: BLOCK, cluster: BLOCK }),
        creation("apfs.create.wide", wide),
        creation("apfs.create.long_names", [file(LONG_BMP, 1), file(LONG_ASTRAL, 1)]),
        creation("apfs.create.large_file", [TreeEntry.file("large", size: (1 << 20) + 123, content: "positional", seed: 7)])
      ]

      ONE_FILE = [TreeEntry.file("f", size: 1, content: "seeded", seed: 1)].freeze

      # The valid and the invalid creation of each input rule, each a
      # [tree, settings] pair.
      PAIRS = {
        "apfs.create.geometry" => [[ONE_FILE, { cluster: BLOCK }], [ONE_FILE, { cluster: 512 }]],
        "apfs.create.size" => [[ONE_FILE, { size: 1024 * BLOCK }], [ONE_FILE, { size: 1023 * BLOCK }]],
        "apfs.create.label" => [[ONE_FILE, { label: "l" * 255 }], [ONE_FILE, { label: "l" * 256 }]],
        "apfs.create.name_code_points" => [[[TreeEntry.file("\u{F0000}", size: 0, content: "zero")], {}],
                                           [[TreeEntry.file("a\uffff", size: 0, content: "zero")], {}]],
        "apfs.create.name_length" => [[[TreeEntry.file(E_ACUTE * 127, size: 0, content: "zero")], {}],
                                      [[TreeEntry.file(E_ACUTE * 128, size: 0, content: "zero")], {}]],
        "apfs.create.siblings" => [[%w[Readme README].map { TreeEntry.file(_1, size: 0, content: "zero") }, { case: "sensitive" }],
                                   [%w[Readme README].map { TreeEntry.file(_1, size: 0, content: "zero") }, {}]],
        "apfs.create.target_length" => [[[TreeEntry.symlink("l", target: "t" * 1023)], {}],
                                        [[TreeEntry.symlink("l", target: "t" * 1024)], {}]],
        # The destination rules' invalid creations write a valid image that
        # extraction must refuse, or that the host's name trials decide.
        "apfs.extract.safe_kinds" => [[ONE_FILE, {}], [[*ONE_FILE, TreeEntry.symlink("l", target: "f"), TreeEntry.fifo("p")], {}]],
        "host.name.collision" => [[ONE_FILE, { case: "sensitive" }],
                                  [%w[Readme README].map { TreeEntry.file(_1, size: 0, content: "zero") }, { case: "sensitive" }]],
        "host.name.unrepresentable" => [[ONE_FILE, {}], [[TreeEntry.file(E_ACUTE * 128, size: 0, content: "zero")], {}]]
      }.freeze

      def for_rule(id)
        (valid_tree, valid_settings), (invalid_tree, invalid_settings) = PAIRS.fetch(id)
        [creation("#{id}.valid", valid_tree, valid_settings, rules: [id]),
         creation("#{id}.invalid", invalid_tree, invalid_settings, intended: id, rules: [id])]
      end

      # Every deterministic creation: each input and destination rule's
      # pair, then the profile creations.
      def deterministic = PAIRS.keys.flat_map { for_rule(_1) } + profile

      # Name parts a drawn tree combines: ASCII in both cases, a precomposed
      # and a decomposed accent, which compare equal on either
      # personality, CJK, an astral character and punctuation.
      NAME_PARTS = ["a", "B", "readme", "README", E_ACUTE, E_DECOMPOSED, "\u4e2d\u6587", "\u{1F600}", " ", "-", ".x"].freeze
      FILE_SIZES = [0, 1, BLOCK - 1, BLOCK, BLOCK + 1, 5 * BLOCK + 3].freeze
      MAX_DRAWN_ENTRIES = 24
      MAX_DRAWN_DEPTH = 3
      # File.dirname of a name at the root.
      ROOT = "."
      # The kinds a drawn entry takes, weighted by repetition.
      PLAIN_KINDS = %w[directory directory file file file].freeze
      SPECIAL_KINDS = %w[directory directory file file file symlink fifo].freeze

      # count random valid creations drawn from seed: a personality, and a
      # tree whose names never share a comparison form with a sibling,
      # since such a pair is the siblings rule's, not a valid draw's. Half
      # the trees hold symbolic links and FIFOs, which extraction refuses,
      # so the other half are extracted.
      def draw(seed:, count:)
        rng = Random.new(seed)
        Array.new(count) do |index|
          settings = rng.rand(2).zero? ? {} : { case: "sensitive" }
          kinds = rng.rand(2).zero? ? PLAIN_KINDS : SPECIAL_KINDS
          creation("apfs.create.drawn.#{seed}.#{index}", drawn_tree(rng, settings, kinds), settings, rules: [])
        end
      end

      # Entries added one at a time beneath the root or a directory already
      # drawn, each kept only when no sibling has its comparison form.
      def drawn_tree(rng, settings, kinds)
        insensitive = !settings.key?(:case)
        rng.rand(1..MAX_DRAWN_ENTRIES).times.each_with_object([]) do |ordinal, tree|
          directories = tree.select { _1.type == "directory" && _1.path.count("/") < MAX_DRAWN_DEPTH - 1 }.map(&:path)
          parent = [ROOT, *directories].sample(random: rng)
          name = Array.new(rng.rand(1..3)) { NAME_PARTS.sample(random: rng) }.join
          form = Names.form(name, case_insensitive: insensitive)
          next if tree.any? { File.dirname(_1.path) == parent && Names.form(File.basename(_1.path), case_insensitive: insensitive) == form }

          tree << drawn_entry(rng, parent == ROOT ? name : "#{parent}/#{name}", ordinal, kinds.sample(random: rng))
        end
      end

      def drawn_entry(rng, path, ordinal, kind)
        case kind
        in "directory" then directory(path)
        in "symlink" then TreeEntry.symlink(path, target: "t" * rng.rand(1..CreationRules::MAX_TARGET_BYTES))
        in "fifo" then TreeEntry.fifo(path)
        in "file" then file(path, FILE_SIZES.sample(random: rng), seed: ordinal + 1)
        end
      end
    end
  end
end
