#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Compare the keys held by the children of the catalog tree's root in two
# images of one volume, taken before and after a change, to show whether
# deleting records rewrites index keys:
#
#   index_keys.rb BEFORE_IMAGE AFTER_IMAGE
#
# It prints the number of keys in each image and how many keys of one are
# missing from the other. The catalog tree must have a root that is an index node.
require_relative "fields"

module HfsPlusIndexKeys
  module_function

  # The keys of every record of every child of the catalog root.
  def level_two_keys(path)
    File.open(path, "rb") do |io|
      fields = HfsPlusFields
      base = fields::Volume.new(fields::Image.new(io), 0, 512)
      header = fields.read_header(base, 1024)
      volume = base.with(block_size: header[:block_size])
      fork = header[:forks][:catalog]
      node_size = fields.tree_header_report(volume, fork)[:node_size]
      root_node = fields.tree_header_report(volume, fork)[:root_node]
      root = fields.read_node(volume, fork, root_node, node_size)
      fields.record_slices(root, fields.descriptor(root).num_records).flat_map do |slice|
        child = fields.read_node(volume, fork, slice.unpack1("@#{slice.bytesize - 4}N"), node_size)
        fields.record_slices(child, fields.descriptor(child).num_records).map { fields.record_key(_1) }
      end
    end
  end

  def main(argv)
    abort("usage: index_keys.rb BEFORE_IMAGE AFTER_IMAGE") unless argv.size == 2

    before, after = argv.map { level_two_keys(_1) }
    puts "before #{before.size} keys, after #{after.size}; after keys absent before: #{(after - before).size}; " \
         "before keys absent after: #{(before - after).size}"
    0
  end
end

exit(HfsPlusIndexKeys.main(ARGV)) if $PROGRAM_NAME == __FILE__
