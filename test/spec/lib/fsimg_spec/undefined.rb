# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

module FsimgSpec
  # A value a total decoder could not determine: a field beyond the bytes
  # it was given, or a quantity derived from such a field, from a zero
  # divisor or from contradictory fields. reason says which. A rule applies
  # only when every input its predicate reads is defined.
  Undefined = Data.define(:reason)

  def self.undefined?(value) = value.is_a?(Undefined)

  # The block's result over inputs, or the first Undefined among them, so
  # an undefined input makes every quantity derived from it undefined.
  def self.derive(*inputs)
    inputs.find { undefined?(_1) } || yield(*inputs)
  end
end
