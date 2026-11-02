# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fsimg_spec"

module FsimgSpec
  # Independent ISO 9660, SUSP, Rock Ridge, Joliet and El Torito evidence.
  # Rule predicates are manual translations of the cited format contracts.
  module Iso9660
  end
end

require_relative "iso9660/layout"
require_relative "iso9660/builder"
require_relative "iso9660/names"
require_relative "iso9660/rock_ridge"
require_relative "iso9660/catalog"
require_relative "iso9660/model"
require_relative "iso9660/rules"
require_relative "iso9660/witnesses"
require_relative "iso9660/profiles"
require_relative "iso9660/oracles"
require_relative "iso9660/cases"
require_relative "iso9660/suite"
