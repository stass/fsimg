# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "tmpdir"
require "fsimg_spec"

# The name funnel creates exactly the bytes it is given, refuses names it
# must never create, and reports the host's own answer for every other
# name (specs/design-spec-testing.md#host-names).
class TestTree < Minitest::Test
  E_ACUTE = "\u00e9"
  NFD_E_ACUTE = "e\u0301"

  def test_funnel_refuses_names_that_are_no_single_component
    in_directory do |directory|
      ["", ".", "..", "a/b", "a\0b"].each do |name|
        assert_raises(FsimgSpec::NameRefused, name.inspect) { FsimgSpec::NameFunnel.create_file(directory, name) }
        assert_raises(FsimgSpec::NameRefused, name.inspect) { FsimgSpec::NameFunnel.create_directory(directory, name) }
      end
      assert_empty FsimgSpec::NameFunnel.children(directory)
    end
  end

  # The bytes C3 A9 tagged ISO-8859-1 read as two characters; File.join
  # would transcode them to C3 83 C2 A9. The funnel keeps the bytes, which
  # every host accepts as UTF-8.
  def test_funnel_creates_the_exact_bytes_of_a_name_whatever_its_encoding_tag
    in_directory do |directory|
      names = ["\xC3\xA9".dup.force_encoding(Encoding::ISO_8859_1), "\xC3\xA8".dup.force_encoding(Encoding::IBM437)]

      assert_equal %i[created created], names.map { FsimgSpec::NameFunnel.create_file(directory, _1) }
      assert_equal ["\xC3\xA8".b, "\xC3\xA9".b], FsimgSpec::NameFunnel.children(directory)
    end
  end

  # The single byte E9 tagged ISO-8859-1 is e-acute, which File.join would
  # transcode to C3 A9. The funnel asks the host for the byte itself: APFS
  # refuses it as no UTF-8, and ext4 creates it.
  def test_funnel_never_transcodes_a_latin1_byte
    in_directory do |directory|
      answer = FsimgSpec::NameFunnel.create_file(directory, "\xE9".dup.force_encoding(Encoding::ISO_8859_1))

      assert_includes %i[created illegal], answer
      assert_equal(answer == :created ? ["\xE9".b] : [], FsimgSpec::NameFunnel.children(directory))
    end
  end

  # A host that folds case or normalization answers EEXIST for the second
  # name of a pair; one that does not creates both. The funnel reports what
  # the host did, whichever kind it is.
  def test_funnel_reports_the_hosts_answer_for_colliding_pairs
    [%w[README readme], [E_ACUTE, NFD_E_ACUTE]].each do |first, second|
      in_directory do |directory|
        assert_equal :created, FsimgSpec::NameFunnel.create_file(directory, first)
        expected = host_folds?(first, second) ? :exists : :created

        assert_equal expected, FsimgSpec::NameFunnel.create_file(directory, second), [first, second].inspect
      end
    end
  end

  # 255 U+00E9 take 510 bytes of UTF-8: APFS counts characters and creates
  # the name, ext4 counts bytes and answers ENAMETOOLONG.
  def test_funnel_reports_a_long_component_as_created_or_too_long
    in_directory do |directory|
      name = E_ACUTE * 255
      answer = FsimgSpec::NameFunnel.create_file(directory, name)

      assert_includes %i[created too_long], answer
      assert_equal [name.b], FsimgSpec::NameFunnel.children(directory) if answer == :created
    end
  end

  def test_funnel_never_follows_a_link
    in_directory do |directory|
      File.symlink(File.join(directory, "elsewhere"), File.join(directory, "link"))

      assert_equal :exists, FsimgSpec::NameFunnel.create_file(directory, "link")
      refute File.exist?(File.join(directory, "elsewhere"))
    end
  end

  def test_source_tree_is_written_and_walked_back
    in_directory do |directory|
      file = FsimgSpec::TreeEntry.file("d/f", size: 100, seed: 3, holes: [[10, 5]])
      tree = [FsimgSpec::TreeEntry.directory("d"), file, entry("e")]

      assert_nil FsimgSpec::SourceTree.write(directory, tree)
      walked = FsimgSpec::TreeWalk.entries(directory)
      assert_equal [["d", "directory"], ["d/f", "file"], ["e", "file"]], walked.map { [_1.path, _1.type] }
      assert_equal FsimgSpec::FileContent.bytes(file, offset: 0, length: 100), File.binread(File.join(directory, "d", "f"))
    end
  end

  # A tree whose names collide on this host cannot be held there; the case
  # becomes a host skip.
  def test_source_tree_that_collides_on_the_host_is_refused
    in_directory do |directory|
      refusal = FsimgSpec::SourceTree.write(directory, [entry("README"), entry("readme")])

      if host_folds?("README", "readme")
        assert_match(/readme.*exists/, refusal)
      else
        assert_nil refusal
      end
    end
  end

  # File content follows from the entry and offset alone, so any slice can
  # be produced without the bytes before it, holes included.
  def test_file_content_slices_agree_with_the_whole
    %w[seeded positional].each do |content|
      file = FsimgSpec::TreeEntry.file("f", size: 200, content:, seed: 9, holes: [[20, 30], [120, 1]])
      whole = FsimgSpec::FileContent.bytes(file, offset: 0, length: 200)

      assert_equal whole.byteslice(33, 100), FsimgSpec::FileContent.bytes(file, offset: 33, length: 100), content
      assert_equal whole, FsimgSpec::FileContent.each_chunk(file).to_a.join, content
      refute_equal whole, FsimgSpec::FileContent.bytes(file.with(seed: 10), offset: 0, length: 200), content
    end
  end

  # The generators as specs/design-mtree.md#generated-bytes defines them,
  # the bytes fsimg's own generators must give: seeded blocks are
  # SHA-256(LE64(seed) || LE64(i)), positional ones LE64(seed) ||
  # LE64(offset), zero is zeros, and holes read as zeros within either.
  def test_file_content_follows_the_manifest_generators
    seeded = FsimgSpec::TreeEntry.file("s", size: 40, seed: 5)
    positional = FsimgSpec::TreeEntry.file("p", size: 40, content: "positional", seed: 5, holes: [[18, 4]])
    zero = FsimgSpec::TreeEntry.file("z", size: 40, content: "zero")
    blocks = [0, 1].map { Digest::SHA256.digest([5, _1].pack("Q<Q<")) }.join
    second = [5, 16].pack("Q<Q<")

    assert_equal blocks.byteslice(0, 40), FsimgSpec::FileContent.bytes(seeded, offset: 0, length: 40)
    assert_equal [5, 0].pack("Q<Q<") + second.byteslice(0, 2) + ("\0".b * 4) + second.byteslice(6, 10) + [5, 32].pack("Q<Q<").byteslice(0, 8),
                 FsimgSpec::FileContent.bytes(positional, offset: 0, length: 40)
    assert_equal "\0".b * 40, FsimgSpec::FileContent.bytes(zero, offset: 0, length: 40)
  end

  # A tree beyond its bound is listed without hashing its files.
  def test_files_are_hashed_only_within_the_bound
    Dir.mktmpdir do |directory|
      File.write(File.join(directory, "a"), "x" * 20)

      assert_nil FsimgSpec::TreeWalk.entries(directory, hash_bytes: 19).first.sha256
      assert_equal Digest::SHA256.hexdigest("x" * 20), FsimgSpec::TreeWalk.entries(directory, hash_bytes: 20).first.sha256
    end
  end

  private

  def in_directory(&) = Dir.mktmpdir { yield _1.b }

  def entry(path) = FsimgSpec::TreeEntry.file(path, size: 0, content: "zero")

  # Whether this host stores first and second as one name, probed with the
  # plain file API in a directory of its own.
  def host_folds?(first, second)
    Dir.mktmpdir do |probe|
      File.write(File.join(probe, first), "")
      File.exist?(File.join(probe, second))
    end
  end
end
