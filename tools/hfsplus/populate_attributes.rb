# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E5 puts on a mounted volume: extended attributes of many
# sizes and names, an ACL, Finder information, resource forks, and files that
# lose their last attribute. Attributes are set with xattr(1); steps that read
# something back return it as a String for `HfsPlusPopulate.attempt` to keep.
#
#   odd                 attributes of 0, 1, 2, 3, 5, 7 and 17 bytes
#   sizes               attributes around the inline limit and beyond it
#   names-*             names of 127 and 128 units, non-ASCII, differing only in case
#   acl-*               an ACL on a file and on a folder
#   finder-info-*       Finder information set through xattr
#   rsrc-*              resource forks through ..namedfork/rsrc
#   last-*              attributes removed until none is left
#   plain-*             objects given no attribute, to show what macOS adds itself
require "tmpdir"
require_relative "populate"

module HfsPlusPopulateAttributes
  P = HfsPlusPopulate
  ODD_SIZES = [0, 1, 2, 3, 5, 7, 17].freeze
  # The grid is read off the decoded tree: the largest "inline" row and the
  # smallest "fork" row bracket the limit.
  SIZES = [100, 1000, 2000, 3000, 3500, 3700, 3790, 3800, 3801, 3802, 3803, 3804, 3810, 3900, 4000, 5000, 8000, 70_000].freeze
  RSRC_SIZES = [1, 100, 5000].freeze
  PREFIX = "com.fsimg."

  module_function

  def e5 = odd + sizes + names + acls + finder_info + resource_forks + removals + plain

  def odd
    [P.step("attr-odd", "attr-odd") do |root|
       path = file(root, "attr-odd")
       ODD_SIZES.each { |size| set(path, "#{PREFIX}odd#{size}", "o" * size) }
       listing(path)
     end]
  end

  def sizes
    [P.step("attr-sizes", "attr-sizes") do |root|
       path = file(root, "attr-sizes")
       SIZES.each { |size| set(path, "#{PREFIX}sz#{size}", "s" * size) }
       listing(path)
     end]
  end

  # A 127-unit name is the longest the attribute tree holds; a name the system
  # refuses is reported, not fatal.
  def names
    [P.step("attr-names", "attr-names") do |root|
       path = file(root, "attr-names")
       cases = %w[Case case CASE].map { "#{PREFIX}#{_1}" }
       others = ["a" * 127, "b" * 128, "caf\u00e9", "\u{1F600}", "a:b/c"]
       refused = (cases + others).filter_map do |name|
         set(path, name, name[0, 4])
         nil
       rescue P::ToolFailed => e
         "#{name[0, 12]}...(#{name.length}): #{e.message.lines.first.strip[-60..]}"
       end
       "#{listing(path)}\nrefused: #{refused.inspect}"
     end]
  end

  def acls
    [P.step("acl-file", "acl-file") do |root|
       path = file(root, "acl-file")
       P.tool("chmod", "+a", "everyone deny delete", path)
       P.tool("chmod", "+a", "everyone allow read", path)
       P.tool("ls", "-lde", path) + listing(path)
     end,
     P.step("acl-dir", "acl-dir") do |root|
       path = File.join(root, "acl-dir")
       Dir.mkdir(path)
       P.tool("chmod", "+a", "everyone deny delete", path)
       P.tool("ls", "-lde", path) + listing(path)
     end]
  end

  def finder_info
    [P.step("finder-info-file", "finder-info-file") do |root|
       path = file(root, "finder-info-file")
       P.tool("xattr", "-wx", "com.apple.FinderInfo", info("TEXT", "ttxt", 0x4000), path)
       P.tool("xattr", "-px", "com.apple.FinderInfo", path) + listing(path)
     end,
     P.step("finder-info-dir", "finder-info-dir") do |root|
       path = File.join(root, "finder-info-dir")
       Dir.mkdir(path)
       P.tool("xattr", "-wx", "com.apple.FinderInfo", info("\0\0\0\0", "\0\0\0\0", 0x0400), path)
       P.tool("xattr", "-px", "com.apple.FinderInfo", path) + listing(path)
     end]
  end

  def resource_forks
    RSRC_SIZES.map do |size|
      P.step("rsrc", "rsrc-#{size}") do |root|
        path = file(root, "rsrc-#{size}")
        File.binwrite("#{path}/..namedfork/rsrc", P.pattern(size, size))
        "rsrc size #{File.size("#{path}/..namedfork/rsrc")}\n#{listing(path)}"
      end
    end
  end

  # One attribute removed from a file that had one, two (leaving one) and two
  # (leaving none), against a file that never had any.
  def removals
    [removal("last-one-to-none", 1, 1), removal("last-two-to-one", 2, 1), removal("last-two-to-none", 2, 2),
     P.step("last-never", "last-never") { |root| file(root, "last-never") && "never had an attribute" }]
  end

  def removal(name, set_count, remove_count)
    P.step("attr-remove", name) do |root|
      path = file(root, name)
      set_count.times { set(path, "#{PREFIX}r#{_1}", "value#{_1}") }
      before = listing(path)
      remove_count.times { P.tool("xattr", "-d", "#{PREFIX}r#{_1}", path) }
      "before: #{before}after: #{listing(path)}"
    end
  end

  # Objects macOS touches only by creating them, to see which attributes it adds.
  def plain
    [P.step("plain-file", "plain-file") { |root| file(root, "plain-file") && listing(File.join(root, "plain-file")) },
     P.step("plain-dir", "plain-dir") { |root| Dir.mkdir(File.join(root, "plain-dir")) },
     P.step("plain-symlink", "plain-symlink") { |root| File.symlink("plain-file", File.join(root, "plain-symlink")) },
     P.step("attr-symlink", "attr-symlink") do |root|
       path = File.join(root, "attr-symlink")
       File.symlink("plain-file", path)
       P.tool("xattr", "-sw", "#{PREFIX}link", "on the link", path)
       P.tool("xattr", "-sl", path)
     end,
     P.step("plain-ditto", "plain-ditto") do |root|
       Dir.mktmpdir("e5-src") do |src|
         File.write(File.join(src, "s"), "ditto source\n")
         P.tool("ditto", File.join(src, "s"), File.join(root, "plain-ditto"))
       end
       listing(File.join(root, "plain-ditto"))
     end]
  end

  # ---- helpers ----

  def file(root, name)
    path = File.join(root, name)
    File.write(path, "#{name}\n")
    path
  end

  def set(path, name, value) = P.tool("xattr", "-w", name, value, path)

  # The attribute names of `path`, one per line.
  def listing(path) = P.tool("xattr", path)

  def info(type, creator, flags) = (type.b + creator.b + [flags].pack("n") + ("\0" * 22).b).unpack1("H*")
end
