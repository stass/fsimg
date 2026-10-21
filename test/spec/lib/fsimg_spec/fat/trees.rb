# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "prop_check"

module FsimgSpec
  module Fat
    # The random small trees of a run (specs/design-spec-testing.md#generation-and-shrinking),
    # drawn by PropCheck as recipe trees, each a valid witness on a base of
    # the independent builder. A tree's names are 8.3, long, of the Basic
    # Multilingual Plane or beyond it, or reserved: a DOS device name, or
    # a long name beside the literal 8.3 name its short name would take
    # first. A path has at most DEPTH components, a directory at most
    # WIDTH entries and a tree at most ENTRIES, and a file is empty, one
    # byte, one cluster less a byte, one cluster or several clusters long.
    #
    # Every name is one fsimg's reader accepts (Names.problem), and no two
    # siblings share a key, by construction: the alphabets hold no
    # character a name may not hold where it stands, and a sibling whose
    # key repeats is dropped. Every alphabet character has a single
    # upper-case character, or none, so Names.key and a case-insensitive
    # host see the same siblings as fsimg. A draw the rules still refuse is
    # a generator defect, which its classification reports.
    #
    # A tree mtools populates holds only names mtools writes as they are
    # drawn (Trees.populated_tree): none beyond the BMP, and no DOS device
    # name without an extension.
    module Trees
      G = PropCheck::Generators

      # The bases drawn trees populate: the smallest valid volume of each
      # variant (Witnesses::BASES), with 512-byte clusters, so a case's
      # image stays small. On APFS the FAT12 and FAT16 images measured as
      # fully allocated, 68 KiB and 2 MiB, and the 33 MiB FAT32 image as
      # about 280 KiB.
      BASES = Witnesses::BASES
      # The bounds of every tree, kept small, so a case writes little more
      # than its image and its extracted tree. A name is at most 32 bytes
      # long, so a path stays within MAX_PATH_BYTES; 12 entries of at most
      # three clusters each, a directory's entries among them, fit the 100
      # clusters of the FAT12 base.
      DEPTH = 4
      WIDTH = 4
      ENTRIES = 12
      MAX_PATH_BYTES = DEPTH * 33
      # PropCheck's size, which no generator here reads: every count is
      # drawn from an explicit range.
      SIZE = 10

      # A drawn file: its size and the seed of its bytes.
      DrawnFile = Data.define(:size, :seed)

      ALPHANUMERIC = [*"a".."z", *"A".."Z", *"0".."9"].freeze
      LOWER = [*"a".."z"].freeze
      # The characters a short name holds besides letters and digits
      # (section 6.1).
      SHORT_SPECIAL = %w[$ % ' - _ @ ~ ` ! ( ) { } ^ # &].freeze
      SHORT = [*"A".."Z", *"0".."9", *SHORT_SPECIAL].freeze
      # The characters a long name holds besides those, inside it: space
      # and period, which may neither start nor end it, and the six that
      # section 7 adds.
      LONG_INNER = [*ALPHANUMERIC, *SHORT_SPECIAL, " ", ".", "+", ",", ";", "=", "[", "]"].freeze
      # Letters of the Basic Multilingual Plane with a one-character upper
      # case (e acute, n tilde, o slash, zhe, lambda, omega) and caseless
      # ones (two CJK ideographs, a Hangul syllable, hiragana a, Arabic
      # beh), each in its composed form.
      BMP = %W[\u00E9 \u00F1 \u00F8 \u0436 \u03BB \u03C9 \u6F22 \u5B57 \uD55C \u3042 \u0628].freeze
      # Supplementary characters, two UTF-16 units each: the Deseret
      # capital and small long I, which case-map to each other, a smiling
      # face, the G clef and a CJK Extension B ideograph.
      NON_BMP = %W[\u{10400} \u{10428} \u{1F600} \u{1D11E} \u{20000}].freeze
      DEVICES = %w[CON NUL AUX PRN COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM9
                   LPT1 LPT2 LPT3 LPT4 LPT5 LPT6 LPT7 LPT8 LPT9].freeze

      # count trees drawn from seed, each a valid Witness on the base of
      # the variant whose turn it is, so any first part of the draws
      # spreads over the variants evenly, with an id of prefix, the
      # variant, the seed and the draw's index. generator answers the
      # PropCheck generator of one tree for a cluster size; a test may pass
      # one that stands for a faulty generator.
      def self.draw(seed:, count:, generator: method(:tree), prefix: "fat.drawn")
        rng = Random.new(seed)
        Array.new(count) do |index|
          variant = BASES.keys[index % BASES.size]
          parameters = BASES.fetch(variant)
          tree = generator.call(cluster_bytes: Builder::Layout.new(parameters).cluster_bytes).call(size: SIZE, rng:)
          Witnesses::Witness.new(id: "#{prefix}.#{variant}.#{seed}.#{index}", parameters:, tree:, mutations: [],
                                 forced_format: nil, intended: nil, rule: nil)
        end
      end

      # The generator of one tree, as recipe tree entries in preorder: the
      # first ENTRIES entries of a drawn hierarchy, so every entry's
      # parent precedes it. The root holds at least one entry. names
      # generates one name group.
      def self.tree(cluster_bytes:, names: name_group) =
        children(1, cluster_bytes:, names:, groups: 1..WIDTH).map { entries(_1, prefix: "").first(ENTRIES) }

      # The generator of one tree that mtools populates as drawn. mtools
      # drops the surrogate pairs of a name beyond the BMP when it writes
      # the name as well as when it reads it back, so its read-back could
      # not catch the loss, and mcopy and mmd refuse a DOS device name
      # without an extension. Such a tree draws neither.
      def self.populated_tree(cluster_bytes:) = tree(cluster_bytes:, names: name_group(mtools: true))

      # The generator of the [name, content] pairs of a directory whose
      # entries' paths have depth components: from a count of name groups
      # in groups, at most WIDTH names with distinct keys, each a file or,
      # while depth is less than DEPTH, sometimes a directory.
      def self.children(depth, cluster_bytes:, names:, groups: 0..WIDTH) =
        list(names, groups).map { |drawn| drawn.flatten.uniq { Names.key(_1) }.first(WIDTH) }.bind do |chosen|
          G.tuple(*chosen.map { |name| content(depth, cluster_bytes:, names:).map { [name, _1] } })
        end

      # A DrawnFile, or a directory as the Array of its entries' pairs.
      def self.content(depth, cluster_bytes:, names:)
        file = G.tuple(file_size(cluster_bytes), G.choose(0..MAX_SEED)).map { |size, seed| DrawnFile.new(size:, seed:) }
        return file if depth == DEPTH

        G.frequency(3 => file, 1 => children(depth + 1, cluster_bytes:, names:))
      end

      # A file size relative to the cluster size: none, one byte, one
      # cluster less a byte, one cluster, or two or three clusters.
      def self.file_size(cluster) =
        G.one_of(G.constant(0), G.constant(1), G.constant(cluster - 1), G.constant(cluster), G.choose((cluster + 1)..(3 * cluster)))

      # One name, or the two names of a reserved pair. The weights are
      # pairs, not a Hash, since several repeat. For mtools, no name beyond
      # the BMP and no device name without an extension.
      def self.name_group(mtools: false)
        groups = [[3, short_name.map { [_1] }], [2, long_name.map { [_1] }], [2, mixed(BMP).map { [_1] }]]
        groups << [2, mixed(NON_BMP).map { [_1] }] unless mtools
        groups << [1, reserved(device_extension: mtools)]
        G.frequency(groups)
      end

      # An 8.3 name in upper case, or in lower case, which the builder
      # stores with a long name.
      def self.short_name = cased(G.tuple(word(SHORT, 1..8), extension(SHORT)).map(&:join))

      # A long ASCII name of 9 to 24 characters, which starts and ends with
      # a letter or digit.
      def self.long_name = G.tuple(word(ALPHANUMERIC, 1..1), word(LONG_INNER, 7..22), word(ALPHANUMERIC, 1..1)).map(&:join)

      # A name of up to four ASCII characters on each side of one to six
      # characters of alphabet.
      def self.mixed(alphabet) = G.tuple(word(ALPHANUMERIC, 0..4), word(alphabet, 1..6), word(ALPHANUMERIC, 0..4)).map(&:join)

      # A DOS device name in either case, with an extension or, unless
      # device_extension, without one; or a long name beside the literal 8.3
      # name that its basis name with the first numeric tail spells, which
      # the builder must leave to the literal. For two lower-case words and
      # an extension, the basis name (section 6.1) is the words in upper
      # case without the space between them, cut to six characters for the
      # tail ~1.
      def self.reserved(device_extension:)
        suffix = device_extension ? word(ALPHANUMERIC, 1..3).map { ".#{_1}" } : extension(ALPHANUMERIC)
        device = cased(G.tuple(word(DEVICES, 1..1), suffix).map(&:join)).map { [_1] }
        pair = G.tuple(word(LOWER, 3..6), word(LOWER, 3..6), word(LOWER, 3..3)).map do |first, second, suffix|
          ["#{first} #{second}.#{suffix}", "#{(first + second).upcase[0, 6]}~1.#{suffix.upcase}"]
        end
        G.one_of(device, pair)
      end

      # No extension, or a period and one to three characters of alphabet.
      def self.extension(alphabet) = G.one_of(G.constant(""), word(alphabet, 1..3).map { ".#{_1}" })

      # The names of generator, each in upper case or in lower.
      def self.cased(generator) = G.tuple(generator, G.boolean).map { |name, lower| lower ? name.downcase : name.upcase }

      # A string of a length in lengths of elements of alphabet.
      def self.word(alphabet, lengths) = list(G.choose(0...alphabet.size).map { alphabet.fetch(_1) }, lengths).map(&:join)

      # A list of a length in lengths of values of generator.
      def self.list(generator, lengths) = G.choose(lengths).bind { |count| G.tuple(*Array.new(count) { generator }) }

      # pairs as recipe tree entries in preorder, each path under prefix.
      def self.entries(pairs, prefix:) =
        pairs.flat_map do |name, content|
          path = "#{prefix}#{name}"
          case content
          in DrawnFile(size:, seed:) then [TreeEntry.new(path:, type: "file", size:, seed:)]
          in Array then [TreeEntry.new(path:, type: "directory", size: 0, seed: 0), *entries(content, prefix: "#{path}/")]
          end
        end

      private_class_method :children, :content, :file_size, :name_group, :short_name, :long_name, :mixed, :reserved,
                           :extension, :cased, :word, :list, :entries
    end
  end
end
