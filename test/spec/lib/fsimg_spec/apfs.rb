# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fsimg_spec"

module FsimgSpec
  # The APFS container model: object headers and Fletcher-64, the container
  # superblock, checkpoint selection over the descriptor ring, the mapped
  # ephemeral objects, the container object map and its tree, the generic
  # B-tree node, the space manager with its chunk structures and bitmaps,
  # the free queues and the reaper. It is written from specs/design-apfs.md
  # and shares no code with lib/formats/apfs/ or the C generator.
  module Apfs
  end
end

require_relative "apfs/layout"
require_relative "apfs/model"
require_relative "apfs/checkpoint"
require_relative "apfs/btree"
require_relative "apfs/space"
require_relative "apfs/rules"
require_relative "apfs/witnesses"
require_relative "apfs/oracles"
