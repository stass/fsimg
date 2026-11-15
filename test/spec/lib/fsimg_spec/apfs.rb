# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fsimg_spec"

module FsimgSpec
  # The APFS container model: object headers and Fletcher-64, the container
  # superblock, checkpoint selection over the descriptor ring, the mapped
  # ephemeral objects, the container object map and its tree, the generic
  # B-tree node, the space manager with its chunk structures and bitmaps,
  # the free queues and the reaper; and each volume's superblock, object
  # map and filesystem tree, with the inode, directory-record, extent and
  # data-stream records and their name hashing; and the writer's
  # one-volume profile, whose creation cases the case phase runs through
  # fsimg and judges by these rules and by WriterLayout. It is written from
  # specs/design-apfs.md and shares no code with lib/formats/apfs/ or the C generator.
  module Apfs
  end
end

require_relative "apfs/layout"
require_relative "apfs/model"
require_relative "apfs/checkpoint"
require_relative "apfs/btree"
require_relative "apfs/records"
require_relative "apfs/volume"
require_relative "apfs/space"
require_relative "apfs/rules"
require_relative "apfs/volume_rules"
require_relative "apfs/record_rules"
require_relative "apfs/volume_witnesses"
require_relative "apfs/witnesses"
require_relative "apfs/oracles"
require_relative "apfs/writer_layout"
require_relative "apfs/namespace"
require_relative "apfs/creation_rules"
require_relative "apfs/creations"
require_relative "apfs/cases"
require_relative "apfs/suite"
