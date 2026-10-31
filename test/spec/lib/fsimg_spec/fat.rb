# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fsimg_spec"

module FsimgSpec
  # FAT12, FAT16 and FAT32: layouts, model and geometry, written from the
  # Microsoft FAT specification, never from fsimg's sources or C tests.
  module Fat
  end
end

require_relative "fat/layout"
require_relative "fat/geometry"
require_relative "fat/model"
require_relative "fat/builder"
require_relative "fat/names"
require_relative "fat/oracles"
require_relative "fat/bases"
require_relative "fat/witnesses"
require_relative "fat/rules"
require_relative "fat/sizing"
require_relative "fat/cases"
require_relative "fat/trees"
require_relative "fat/suite"
require_relative "fat/seeds"
