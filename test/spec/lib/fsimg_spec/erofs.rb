# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fsimg_spec"

module FsimgSpec
  # The EROFS core model: superblock and checksum, compact and extended
  # inodes, flat, flat-inline and chunk-based data, directory blocks, the
  # parent rule and the inline attribute region. It is written from
  # specs/design-erofs.md and shares no code with lib/formats/erofs/ or the
  # C generator.
  module Erofs
  end
end

require_relative "erofs/layout"
require_relative "erofs/builder"
require_relative "erofs/model"
require_relative "erofs/rules"
require_relative "erofs/witnesses"
require_relative "erofs/cases"
require_relative "erofs/suite"
