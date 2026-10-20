# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
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
