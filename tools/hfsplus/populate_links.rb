# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E4 puts on a mounted volume: hard links made and unmade, a
# link to a symbolic link, an attempted directory link, and ordinary files
# whose Finder information carries the codes a link stub carries. Steps that
# observe something return `HfsPlusPopulate.observe(text)`, which `attempt` keeps.
#
#   hl2/{a,b}        one file with two names, in two folders
#   hl3/{x,y,z}      one file with three names
#   unlink-*         names removed until one is left, the original first or last
#   sl-*             a hard link to a symbolic link
#   dir-link         link(2) on a directory
#   meta-*           mode and time changes made through one name
#   code-*           ordinary files with link or alias codes in their Finder information
#   stubtest-*       ordinary files that patch_record.rb turns into look-alike stubs
#   rd/{a,b}         a link made after the root folder's creation date was set earlier
#
require_relative "populate"

module HfsPlusPopulateLinks
  P = HfsPlusPopulate
  T0 = 1_700_000_000

  module_function

  def e4 = names + unlinks + symlink_link + directory_link + metadata + codes + stub_test_files + redate

  # ---- the steps ----

  def names
    [P.step("link-2", "hl2/a/orig") do |root|
       %w[hl2/a hl2/b].each { P.mkdir_p(root, _1) }
       P.write_file(root, "hl2/a/orig", "two names\n")
       File.link(File.join(root, "hl2/a/orig"), File.join(root, "hl2/b/second"))
       P.observe(observe_all(root, %w[hl2/a/orig hl2/b/second]))
     end,
     P.step("link-3", "hl3/x/orig") do |root|
       %w[hl3/x hl3/y hl3/z].each { P.mkdir_p(root, _1) }
       P.write_file(root, "hl3/x/orig", "three names\n")
       File.link(File.join(root, "hl3/x/orig"), File.join(root, "hl3/y/second"))
       File.link(File.join(root, "hl3/x/orig"), File.join(root, "hl3/z/third"))
       P.observe(observe_all(root, %w[hl3/x/orig hl3/y/second hl3/z/third]))
     end]
  end

  # Three names unlinked back to one, keeping the original and keeping the
  # last link; two names with the original removed; and every name removed.
  def unlinks
    [group("unlink-keep-first", %w[u1/a u1/b u1/c], keep: "u1/a"),
     group("unlink-keep-last", %w[u2/a u2/b u2/c], keep: "u2/c"),
     group("unlink-two-keep-link", %w[u3/a u3/b], keep: "u3/b"),
     group("unlink-all", %w[u4/a u4/b], keep: nil)]
  end

  # Create the first name, link the others, then unlink all but `keep`.
  def group(label, names, keep:)
    P.step(label, names.first) do |root|
      names.each { P.mkdir_p(root, File.dirname(_1)) }
      P.write_file(root, names.first, "#{label}\n")
      names.drop(1).each { File.link(File.join(root, names.first), File.join(root, _1)) }
      before = observe_all(root, names)
      names.reject { _1 == keep }.each { File.unlink(File.join(root, _1)) }
      P.observe("before:\n#{before}\nafter:\n#{observe_all(root, names)}")
    end
  end

  # link(2) follows a symbolic link, so a second name for the link itself
  # takes ln -P, which links the symlink without following it.
  def symlink_link
    [P.step("symlink-link-follow", "sl/follow") do |root|
       P.mkdir_p(root, "sl")
       P.write_file(root, "sl/target", "symlink target\n")
       File.symlink("target", File.join(root, "sl/link1"))
       File.link(File.join(root, "sl/link1"), File.join(root, "sl/follow"))
       P.observe(observe_all(root, %w[sl/target sl/link1 sl/follow]))
     end,
     P.step("symlink-link-nofollow", "sl/nofollow") do |root|
       P.tool("ln", "-P", File.join(root, "sl/link1"), File.join(root, "sl/nofollow"))
       P.observe(observe_all(root, %w[sl/link1 sl/nofollow]))
     end]
  end

  def directory_link
    [P.step("directory-link", "dl/dir2") do |root|
       P.mkdir_p(root, "dl/dir1")
       File.link(File.join(root, "dl/dir1"), File.join(root, "dl/dir2"))
     end]
  end

  # Whose metadata macOS reports: change mode and mtime through one name, then
  # look at both.
  def metadata
    [P.step("link-metadata", "meta/b") do |root|
       P.mkdir_p(root, "meta")
       P.write_file(root, "meta/a", "metadata\n")
       File.link(File.join(root, "meta/a"), File.join(root, "meta/b"))
       File.chmod(0o600, File.join(root, "meta/b"))
       File.utime(T0, T0, File.join(root, "meta/b"))
       P.observe(observe_all(root, %w[meta/a meta/b]))
     end]
  end

  # Ordinary files with Finder information that is, or looks like, a link's.
  def codes
    { "code-hlnk-hfs+" => ["hlnk", "hfs+", 0], "code-fdrp-MACS-alias" => ["fdrp", "MACS", 0x8000],
      "code-alis-MACS" => %w[alis MACS].push(0x8000) }.map do |name, (type, creator, flags)|
      P.step("finder-codes", name) do |root|
        P.write_file(root, name, "ordinary #{name}\n")
        P.tool("xattr", "-wx", "com.apple.FinderInfo", P.finder_info(type, creator, flags), File.join(root, name))
        P.observe("#{P.lstat_line(File.join(root, name))}\ncontent: #{File.read(File.join(root, name)).inspect}")
      end
    end
  end

  # Files with a name and content of their own, for patch_record.rb to give a
  # link stub's codes, dates and reference on a copy of the image.
  def stub_test_files
    %w[a b c d e f g h i j].map { |letter| P.step("stub-test", "stubtest-#{letter}") { P.write_file(_1, "stubtest-#{letter}", "ordinary stubtest-#{letter}\n") } }
  end

  # Set the root folder's creation date earlier with SetFile, then read the old links and make a new one:
  # the new stub shows which date macOS gives a stub, and whether the old
  # links still read as links shows which dates its test accepts.
  def redate
    [P.step("root-redate", "rd/a") do |root|
       P.mkdir_p(root, "rd")
       P.tool("SetFile", "-d", "01/02/2020 03:04:05", root)
       P.write_file(root, "rd/a", "after the root was re-dated\n")
       File.link(File.join(root, "rd/a"), File.join(root, "rd/b"))
       P.observe(["root birth=#{File.stat(root).birthtime.to_i}", observe_all(root, %w[hl2/a/orig hl2/b/second rd/a rd/b])].join("\n"))
     end]
  end

  # ---- helpers ----

  def observe_all(root, names) = names.map { P.lstat_line(File.join(root, _1)) }.join("\n")
end
