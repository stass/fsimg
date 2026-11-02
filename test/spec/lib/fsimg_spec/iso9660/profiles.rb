# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "prop_check"

module FsimgSpec
  module Iso9660
    # Seeded combinations of independent fixture features. The fixed map
    # keeps their metadata disjoint; boolean implications belong to the
    # builder (for example, an SL stream also needs the RR handshake).
    module Profiles
      G = PropCheck::Generators
      module_function

      def generator
        flags = G.tuple(*Builder::BOOLEAN_PARAMETERS.map { G.choose(0..1) })
        target = G.one_of(*[0, 1, 254, 255, 256, 1023, 1024].map { G.constant(_1) })
        G.tuple(flags, target, G.choose(0..2)).map do |booleans, length, shape|
          Builder::BOOLEAN_PARAMETERS.zip(booleans).to_h.merge(target_length: length, target_shape: shape).freeze
        end
      end

      def draw(seed:, count:)
        rng = Random.new(seed)
        chosen = generator
        Array.new(count) do |index|
          parameters = chosen.call(size: 10, rng:)
          Witnesses::Witness.new(id: "iso9660.drawn.#{seed}.#{index}", parameters:, mutations: [],
                                intended: nil, rules: [])
        end
      end
    end
  end
end
