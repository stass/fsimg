# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Apfs
    # The input rules of the writer's profile: the domain of the creation
    # settings and of the source tree that specs/design-apfs.md's Writer
    # section states, each evaluated over a SourceModel. A source no rule
    # flags is one the writer must accept and lay out as WriterLayout
    # states.
    module CreationRules
      BLOCK = WriterLayout::BLOCK
      MINIMUM_BLOCKS = WriterLayout::MINIMUM_BLOCKS
      MAX_LABEL_BYTES = 255
      MAX_NAME_UNITS = 255
      MAX_TARGET_BYTES = 1023

      module_function

      def all = [
        input_rule(id: "geometry", anchor: "settings-and-identities", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { source.settings.slice(:sector, :cluster).values.all? { [0, BLOCK].include?(_1) } }),
        input_rule(id: "size", anchor: "writer-values", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { size?(source.settings[:size]) }),
        input_rule(id: "label", anchor: "settings-and-identities", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { source.settings.fetch(:label, "").bytesize <= MAX_LABEL_BYTES }),
        input_rule(id: "name_code_points", anchor: "directory-records-and-names", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { names(source).all? { Names.assigned?(_1) } }),
        input_rule(id: "name_length", anchor: "directory-records-and-names", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { names(source).all? { short?(_1) } }),
        input_rule(id: "siblings", anchor: "profile", status: "FSIMG_EINVAL",
                   predicate: ->(source, _) { distinct_siblings?(source) }),
        input_rule(id: "target_length", anchor: "symbolic-links", status: "FSIMG_EUNSUPPORTED",
                   predicate: ->(source, _) { source.tree.select { _1.type == "symlink" }.all? { _1.target.bytesize <= MAX_TARGET_BYTES } })
      ]

      def input_rule(id:, anchor:, status:, predicate:)
        Rule.new(id: "apfs.create.#{id}", category: "profile", policy: "must_reject", status:, scopes: ["input"],
                 applies_to: ->(_, _) { true }, predicate:, domain: -> { Creations.for_rule("apfs.create.#{id}") },
                 source: "specs/design-apfs.md##{anchor}")
      end

      # An explicit container size is whole blocks, at least the writer's
      # minimum container (Writer values: Minimum container size).
      def size?(size) = size.nil? || (size % BLOCK).zero? && size >= MINIMUM_BLOCKS * BLOCK

      # The last component of every entry's path: every name the volume
      # stores once, since the tree lists each directory once.
      def names(source) = source.tree.map { File.basename(_1.path) }

      # At most 255 UTF-16 code units as given and in NFD (design: Name
      # length), a character above U+FFFF counting two.
      def short?(name) = [name, Names.nfd(name)].all? { _1.encode(Encoding::UTF_16LE).bytesize / 2 <= MAX_NAME_UNITS }

      # No two siblings share a comparison form under the volume's case
      # setting. Only assigned names have a form the harness's tables give,
      # so the code-point rule judges the others alone.
      def distinct_siblings?(source)
        insensitive = source.settings.fetch(:case, "insensitive") == "insensitive"
        source.tree.map { [File.dirname(_1.path), File.basename(_1.path)] }
              .select { |_, name| Names.assigned?(name) }
              .group_by(&:first)
              .all? { |_, siblings| siblings.map { Names.form(_1.last, case_insensitive: insensitive) }.uniq.size == siblings.size }
      end

      private_class_method :input_rule, :size?, :names, :short?, :distinct_siblings?
    end
  end
end
