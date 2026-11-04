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
# missing from the other. The catalog tree must be at least three levels deep,
# so that the root's children are index nodes; on a shallower tree they are
# leaves, whose keys any insertion or deletion changes, and the tool refuses.
require_relative "fields"

module HfsPlusIndexKeys
  module_function

  # The keys of every record of every child of the catalog root, all of which
  # must be index nodes.
  def level_two_keys(path)
    File.open(path, "rb") do |io|
      fields = HfsPlusFields
      catalog = fields.open_catalog(io)
      tree = catalog.tree
      raise fields::Error, "#{path}: the catalog tree is #{tree[:depth]} levels deep; its root's children are not index nodes" if tree[:depth] < 3

      root = fields.read_node(catalog.volume, catalog.fork, tree[:root_node], tree[:node_size])
      fields.record_slices(root, fields.descriptor(root).num_records).flat_map do |slice|
        child_number = fields.index_child(slice, fields.record_key(slice))
        child = fields.read_node(catalog.volume, catalog.fork, child_number, tree[:node_size])
        raise fields::Malformed, "#{path}: node #{child_number} under the root is not an index node" unless fields.descriptor(child).kind.zero?

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
  rescue HfsPlusFields::Error, SystemCallError => e
    warn "index_keys.rb: #{e.message}"
    1
  end
end

exit(HfsPlusIndexKeys.main(ARGV)) if $PROGRAM_NAME == __FILE__
