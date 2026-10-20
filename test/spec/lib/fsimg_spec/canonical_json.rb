# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "json"

module FsimgSpec
  # JSON text that depends only on the value: two-space indentation, empty
  # containers written [] and {}, and object keys in the order given. The
  # json gem's pretty printer lays out empty containers differently across
  # its versions, and recipe bytes key the fixture cache and live in the
  # repository, so the harness writes them itself. Scalars still go through
  # JSON.generate, whose escaping the supported versions share.
  module CanonicalJson
    def self.generate(value, indent: "")
      case value
      in {} | [] | String | Integer | true | false | nil then JSON.generate(value)
      in Hash then block("{", "}", indent:, lines: value.map { "#{JSON.generate(_1.to_s)}: #{generate(_2, indent: "#{indent}  ")}" })
      in Array then block("[", "]", indent:, lines: value.map { generate(_1, indent: "#{indent}  ") })
      end
    end

    # A container's lines between its brackets, one level deeper than
    # indent.
    private_class_method def self.block(open, close, indent:, lines:) =
      "#{open}\n#{lines.map { "#{indent}  #{_1}" }.join(",\n")}\n#{indent}#{close}"
  end
end
