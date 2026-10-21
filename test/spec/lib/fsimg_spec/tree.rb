# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "digest"

module FsimgSpec
  # A name the funnel refuses to create.
  class NameRefused < Error; end

  # The one way a name taken from a model or a recipe reaches the host
  # (specs/design-spec-testing.md#host-names). Names are binary strings
  # throughout: a path is built by concatenating binary strings, never with
  # File.join, because Ruby otherwise transcodes or rejects a name by its
  # encoding tag. The funnel creates one component at a time, in a
  # directory it created itself, exclusively and without following links.
  # It reads only four answers from the host; any other error propagates
  # and is a harness error.
  module NameFunnel
    ANSWERS = { Errno::EEXIST => :exists, Errno::ENAMETOOLONG => :too_long, Errno::EILSEQ => :illegal }.freeze

    # Creates the file name in directory, writing what the block writes to
    # it, and answers :created, :exists, :too_long or :illegal.
    def self.create_file(directory, name)
      flags = File::WRONLY | File::CREAT | File::EXCL | File::NOFOLLOW | File::BINARY
      File.open(path(directory, name), flags, 0o600) { |file| yield file if block_given? }
      :created
    rescue Errno::EEXIST, Errno::ENAMETOOLONG, Errno::EILSEQ => e
      ANSWERS.fetch(e.class)
    end

    # Creates the directory name in directory; mkdir never follows a link
    # in its last component.
    def self.create_directory(directory, name)
      Dir.mkdir(path(directory, name), 0o700)
      :created
    rescue Errno::EEXIST, Errno::ENAMETOOLONG, Errno::EILSEQ => e
      ANSWERS.fetch(e.class)
    end

    # The names in directory as binary strings, sorted by their bytes.
    # Dir.each_child, unlike Dir.glob, lists names that start with a dot.
    def self.children(directory) = Dir.each_child(directory.b, encoding: Encoding::BINARY).sort

    # directory/name as one binary string, once name is a single component
    # the host can be asked about.
    def self.path(directory, name)
      component = name.b
      problem =
        if component.empty? then "is empty"
        elsif [".", ".."].include?(component) then "names . or .."
        elsif component.include?("/") then "contains /"
        elsif component.include?("\0") then "contains NUL"
        end
      raise NameRefused, "name #{FsimgSpec.shown(component)} #{problem}" if problem

      directory.b + "/".b + component
    end
  end

  # One entry of a tree walked with lstat: its path relative to the root
  # as a binary string, its type, its size and, for a file, its SHA-256.
  WalkedEntry = Data.define(:path, :type, :size, :sha256)

  # Walks a tree on the host by path, with lstat, never following a link.
  # Every entry is reported, whatever its type, so a caller can refuse a
  # link or device the tree should not hold. root itself must be a
  # directory, not a link to one; the caller checks that with lstat first.
  #
  # A file is hashed through a descriptor opened without following a link
  # and without blocking, and only when the descriptor is still a regular
  # file; an entry that changed type since its lstat is reported with type
  # "changed", so a FIFO or link swapped in by a stray process can neither
  # hang the walk nor point it at a host file.
  module TreeWalk
    READ_BYTES = 1 << 16

    # Every entry under root. Files are hashed only while their total
    # size stays within hash_bytes, so a tree beyond its ceiling costs no
    # more than its listing; nil hashes every file.
    def self.entries(root, hash_bytes: nil)
      listed = walk(root.b, "".b)
      total = listed.sum { _1.type == "file" ? _1.size : 0 }
      return listed if hash_bytes && total > hash_bytes

      listed.map { _1.type == "file" ? file_entry(root.b + "/".b + _1.path, _1.path) : _1 }
    end

    private_class_method def self.walk(root, relative)
      directory = relative.empty? ? root : root + "/".b + relative
      NameFunnel.children(directory).flat_map do |name|
        path = relative.empty? ? name : relative + "/".b + name
        stat = File.lstat(root + "/".b + path)
        entry = WalkedEntry.new(path:, type: type_of(stat), size: stat.size, sha256: nil)
        stat.directory? ? [entry, *walk(root, path)] : [entry]
      end
    end

    # The entry's type as lstat or fstat reports it, except that a regular
    # file with more than one link is a "hard link": it may share its
    # content with a file outside the tree, so it is neither hashed nor
    # taken for a file of the tree.
    private_class_method def self.type_of(stat) = stat.file? && stat.nlink > 1 ? "hard link" : stat.ftype

    private_class_method def self.file_entry(full_path, path)
      File.open(full_path, File::RDONLY | File::NOFOLLOW | File::NONBLOCK | File::BINARY) do |file|
        stat = file.stat
        next WalkedEntry.new(path:, type: "changed", size: 0, sha256: nil) unless type_of(stat) == "file"

        digest = Digest::SHA256.new
        chunk = +""
        digest << chunk while file.read(READ_BYTES, chunk)
        WalkedEntry.new(path:, type: "file", size: stat.size, sha256: digest.hexdigest)
      end
    # O_NOFOLLOW on a link fails with ELOOP, or EMLINK on FreeBSD.
    rescue Errno::ELOOP, Errno::EMLINK
      WalkedEntry.new(path:, type: "changed", size: 0, sha256: nil)
    end
  end
end

module FsimgSpec
  # A file's bytes as a recipe tree entry gives them: the manifest's
  # generated content (specs/design-mtree.md#generated-bytes), which
  # follows at any offset from the entry alone, never from PropCheck's
  # random source, so it stays the same across upgrades. zero is zero
  # bytes. seeded is SHA-256 in counter mode, 32-byte blocks
  # SHA-256(LE64(seed) || LE64(i)). positional is 16-byte blocks
  # LE64(seed) || LE64(16 * j), each naming the file and its own offset,
  # so a misplaced cluster shows where it came from. Holes read as zeros
  # within either.
  module FileContent
    BLOCK_BYTES = { "seeded" => 32, "positional" => 16 }.freeze
    CHUNK_BYTES = 1 << 16

    def self.write(io, entry) = each_chunk(entry) { io.write(_1) }

    # The SHA-256 of the bytes, computed without holding them whole.
    def self.sha256(entry)
      digest = Digest::SHA256.new
      each_chunk(entry) { digest << _1 }
      digest.hexdigest
    end

    # Yields the bytes in chunks of CHUNK_BYTES, or answers an Enumerator
    # of them without a block, such as a tool's stdin reads.
    def self.each_chunk(entry)
      return enum_for(:each_chunk, entry) unless block_given?

      (0...entry.size).step(CHUNK_BYTES) { yield bytes(entry, offset: _1, length: [CHUNK_BYTES, entry.size - _1].min) }
    end

    # The length bytes at offset, with every hole that overlaps them
    # zeroed.
    def self.bytes(entry, offset:, length:)
      stream = generated(entry, offset:, length:)
      entry.holes.each do |start, hole|
        from = [start, offset].max
        to = [start + hole, offset + length].min
        stream[from - offset, to - from] = "\0".b * (to - from) if from < to
      end
      stream
    end

    private_class_method def self.generated(entry, offset:, length:)
      return "\0".b * length if entry.content == "zero"

      size = BLOCK_BYTES.fetch(entry.content)
      first = offset / size
      last = (offset + length - 1) / size
      stream = (first..last).map { block(entry, _1) }.join.b
      stream.byteslice(offset - (first * size), length)
    end

    private_class_method def self.block(entry, index) =
      entry.content == "seeded" ? Digest::SHA256.digest([entry.seed, index].pack("Q<Q<")) : [entry.seed, 16 * index].pack("Q<Q<")
  end

  # The tree a model knows an image to hold, until it decodes directories
  # itself (specs/design-spec-testing.md#cases): the recipe tree of an
  # unmutated base, or the source tree of an image fsimg created. Its
  # entries are the WalkedEntry values a walk of the extracted tree gives,
  # so the two compare as values: binary paths in byte order, and each file
  # with its size and SHA-256. A directory has no size and no digest, since
  # the size a host reports for a directory says nothing about the image.
  # A known tree is an Array of WalkedEntry in path order, so differences
  # are reported in that order.
  module KnownTree
    # The known tree of entries, a recipe tree.
    def self.of(entries) = from_walk(entries.map { walked_entry(_1) })

    # walked, a TreeWalk listing, in the known tree's form.
    def self.from_walk(walked) = walked.map { _1.type == "directory" ? _1.with(size: nil) : _1 }.sort_by(&:path).freeze

    # The most differences of each kind a comparison names; it counts the
    # rest.
    SHOWN_DIFFERENCES = 3

    # How actual, a listing in the known tree's form, differs from
    # expected, each difference a sentence that starts with label: the
    # entries actual lacks, then those it adds beyond them. An entry that
    # differs in type, size or digest is both, and one listed twice is
    # unexpected the second time. Paths are quoted shown, since a listing
    # may come from fsimg or a tool reading an untrusted image.
    def self.differences(label, actual:, expected:)
      { "lacks" => surplus(expected, actual), "has unexpected" => surplus(actual, expected) }.flat_map do |verb, entries|
        shown = entries.first(SHOWN_DIFFERENCES).map { "#{label} #{verb} #{described(_1)}" }
        rest = entries.size - shown.size
        rest.positive? ? [*shown, "#{label} #{verb} #{rest} more"] : shown
      end
    end

    # entries beyond other, counting repeats, as multisets differ.
    private_class_method def self.surplus(entries, other)
      others = other.tally
      entries.tally.flat_map { |entry, count| [entry] * [count - others.fetch(entry, 0), 0].max }
    end

    private_class_method def self.described(entry) =
      ["#{entry.type} #{FsimgSpec.shown(entry.path)}", ("of #{entry.size} bytes" if entry.size),
       ("SHA-256 #{entry.sha256}" if entry.sha256)].compact.join(" ")

    private_class_method def self.walked_entry(entry)
      return WalkedEntry.new(path: entry.path.b, type: "directory", size: nil, sha256: nil) if entry.type == "directory"

      WalkedEntry.new(path: entry.path.b, type: "file", size: entry.size, sha256: FileContent.sha256(entry))
    end
  end

  # A recipe tree written to the host through the name funnel and walked
  # back. The tree fits the host only when every name was created and the
  # walk lists exactly the names written; HFS+ normalization or a refused
  # name makes the case a host skip.
  module SourceTree
    # Writes entries under root, which must exist and be empty, and answers
    # nil when the host holds the tree, or why it does not.
    def self.write(root, entries)
      ordered = entries.sort_by { [_1.path.count("/"), _1.path] }
      refusal = ordered.lazy.filter_map { create(root.b, _1) }.first
      return refusal if refusal

      walked = TreeWalk.entries(root).map { [_1.path, _1.type] }.sort
      written = entries.map { [_1.path.b, _1.type] }.sort
      walked == written ? nil : "the host stored other names than the tree's"
    end

    private_class_method def self.create(root, entry)
      parent, _, name = entry.path.b.rpartition("/".b)
      directory = parent.empty? ? root : root + "/".b + parent
      answer =
        if entry.type == "directory" then NameFunnel.create_directory(directory, name)
        else NameFunnel.create_file(directory, name) { FileContent.write(_1, entry) }
        end
      "#{FsimgSpec.shown(entry.path)}: #{answer}" unless answer == :created
    end
  end
end
