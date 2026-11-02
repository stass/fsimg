# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module FsimgSpec
  module Iso9660
    class BuilderError < FixtureError; end

    # Small independent images with deliberately separated metadata regions.
    # Addresses are fixture facts, never offsets read from the C test builder.
    # The model follows descriptor and record pointers rather than this map.
    module Builder
      NAME = "iso9660-independent"
      BLOCK = 2048
      BLOCKS = 96
      PRIMARY = 16
      PATH_L = 20
      PATH_M = 21
      ROOT = 22
      SUB = 23
      DATA = 24
      CONTINUATION = 25
      CATALOG = 26
      LEAF = 27
      JOLIET_L = 28
      JOLIET_M = 29
      JOLIET_ROOT = 30
      JOLIET_SUB = 31
      RELOCATED = 32
      DATE = [124, 1, 2, 3, 4, 5, 0].pack("C7").freeze
      BOOLEAN_PARAMETERS = %i[rockridge joliet boot boot_sections sections subdirectory continuation relocation symlink fifo].freeze
      PARAMETERS = (BOOLEAN_PARAMETERS + %i[target_length target_shape]).freeze
      SL_BLOCK = 60

      module_function

      def options(parameters)
        unknown = parameters.keys - PARAMETERS
        raise BuilderError, "unknown ISO builder parameters #{unknown.inspect}" unless unknown.empty?
        raise BuilderError, "ISO feature values must be 0 or 1" unless parameters.slice(*BOOLEAN_PARAMETERS).values.all? { [0, 1].include?(_1) }
        raise BuilderError, "ISO target length is outside 0..1024" unless parameters.fetch(:target_length, 0).between?(0, 1024)
        raise BuilderError, "ISO target shape is outside 0..2" unless parameters.fetch(:target_shape, 0).between?(0, 2)

        defaults = BOOLEAN_PARAMETERS.to_h { [_1, false] }.merge(parameters.slice(*BOOLEAN_PARAMETERS).transform_values { _1 == 1 })
        defaults.merge(rockridge: defaults[:rockridge] || defaults[:continuation] || defaults[:relocation] || defaults[:symlink] || defaults[:fifo],
                       boot: defaults[:boot] || defaults[:boot_sections],
                       subdirectory: defaults[:subdirectory] || defaults[:relocation],
                       target_length: parameters.fetch(:target_length, 0), target_shape: parameters.fetch(:target_shape, 0)).freeze
      end

      def tree(parameters)
        selected = options(parameters)
        [TreeEntry.file("FILE.TXT", size: selected[:sections] ? BLOCK + 9 : 9, content: "zero"),
         *(selected[:symlink] ? [TreeEntry.symlink("LINK", target: target(selected))] : []),
         *(selected[:fifo] ? [TreeEntry.fifo("PIPE")] : []),
         *(selected[:subdirectory] ? [TreeEntry.directory("SUB"), TreeEntry.file("SUB/LEAF.BIN", size: 7, content: "zero")] : []),
         *(selected[:relocation] ? [TreeEntry.directory(".rr_moved")] : [])].freeze
      end

      def write(path, parameters:, tree:, max_bytes:)
        raise BuilderError, "ISO fixture exceeds the image ceiling" if BLOCKS * BLOCK > max_bytes
        raise BuilderError, "ISO fixture tree disagrees with its feature profile" unless tree == Builder.tree(parameters)

        selected = options(parameters)
        File.open(path, "wbx", 0o600) do |file|
          file.truncate(BLOCKS * BLOCK)
          descriptors(selected).each { |block, bytes| file.pwrite(bytes, block * BLOCK) }
          write_hierarchy(file, selected:, joliet: false)
          write_hierarchy(file, selected:, joliet: true) if selected[:joliet]
          file.pwrite(nm("FILE.TXT"), CONTINUATION * BLOCK) if selected[:continuation]
          file.pwrite(boot_catalog(sections: selected[:boot_sections]), CATALOG * BLOCK) if selected[:boot]
          sl_areas(selected).each { |block, bytes| file.pwrite(bytes, block * BLOCK) } if selected[:symlink]
        end
      end

      def both16(value) = [value].pack("v") + [value].pack("n")
      def both32(value) = [value].pack("V") + [value].pack("N")

      def entry(signature, bytes) = signature.b + [bytes.bytesize + 4, 1].pack("CC") + bytes
      def pointer(signature, block) = entry(signature, both32(block))
      def px(kind:, links: 1) = entry("PX", [{"directory" => 0o40755, "file" => 0o100644, "symlink" => 0o120777, "fifo" => 0o10644}.fetch(kind), links, 0, 0].map { both32(_1) }.join.b)
      def tf = entry("TF", "\x02".b + DATE)
      def nm(name) = entry("NM", "\0".b + name.b)
      def rr(directory:, name: nil, links: 1) = px(kind: directory ? "directory" : "file", links:) + tf + (name ? nm(name) : "".b)
      def handshake(links:) = entry("SP", "\xbe\xef\0".b) + rr(directory: true, links:) +
        entry("ER", [10, 0, 0, 1].pack("C4") + "RRIP_1991A".b)

      def record(identifier:, extent:, bytes:, flags:, system_use: "".b)
        identifier = identifier.b
        header = Layout::DirectoryHeader.new(record_bytes: 0, attribute_blocks: 0,
          extent_le: extent, extent_be: extent, data_bytes_le: bytes, data_bytes_be: bytes,
          date: DATE, flags:, unit_bytes: 0, interleave_gap: 0, sequence_le: 1, sequence_be: 1,
          identifier_bytes: identifier.bytesize).to_binary_s
        raw = header + identifier + (identifier.bytesize.even? ? "\0".b : "".b) + system_use
        raw += "\0".b if raw.bytesize.odd?
        raw.setbyte(0, raw.bytesize)
        raw
      end

      def path_record(identifier:, extent:, parent:, big_endian:)
        [identifier.bytesize, 0].pack("CC") + [extent].pack(big_endian ? "N" : "V") +
          [parent].pack(big_endian ? "n" : "v") + identifier.b + (identifier.bytesize.odd? ? "\0".b : "".b)
      end

      def path_rows(selected, joliet:)
        return [["\0".b, JOLIET_ROOT, 1], *(selected[:subdirectory] ? [[ucs2("SUB"), JOLIET_SUB, 1]] : [])] if joliet

        children = if selected[:relocation]
          [["RR_MOVED".b, SUB, 1], ["SUB_0000001".b, RELOCATED, 2]]
        elsif selected[:subdirectory]
          [["SUB".b, SUB, 1]]
        else
          []
        end
        [["\0".b, ROOT, 1], *children]
      end

      def descriptor(selected, joliet:)
        rows = path_rows(selected, joliet:)
        path_bytes = rows.sum { 8 + _1.first.bytesize + _1.first.bytesize % 2 }
        label = joliet ? ucs2("SPEC".ljust(16)) : "SPEC".ljust(32).b
        Layout::Descriptor.new(descriptor_type: joliet ? 2 : 1, identifier: "CD001", descriptor_version: 1,
          volume_flags: 0, system_identifier: "\0" * 32, volume_identifier: label, reserved_a: "\0" * 8,
          space_le: BLOCKS, space_be: BLOCKS, escapes: joliet ? "%/E".b.ljust(32, "\0") : "\0" * 32,
          set_size_le: 1, set_size_be: 1, sequence_le: 1, sequence_be: 1, block_bytes_le: BLOCK,
          block_bytes_be: BLOCK, path_bytes_le: path_bytes, path_bytes_be: path_bytes,
          path_l: joliet ? JOLIET_L : PATH_L, path_l_copy: 0, path_m: joliet ? JOLIET_M : PATH_M,
          path_m_copy: 0, root_record: record(identifier: "\0", extent: joliet ? JOLIET_ROOT : ROOT,
                                            bytes: BLOCK, flags: 2),
          identifiers: "\0" * 623, dates: "0" * 16 + "\0" + ("0" * 16 + "\0") * 3,
          structure_version: 1, tail: "\0" * 1166).to_binary_s
      end

      def descriptors(selected)
        records = [descriptor(selected, joliet: false)]
        if selected[:boot]
          boot = "\0CD001\x01".b.ljust(BLOCK, "\0")
          boot[7, 32] = "EL TORITO SPECIFICATION".b.ljust(32, "\0")
          boot[71, 4] = [CATALOG].pack("V")
          records << boot
        end
        records << descriptor(selected, joliet: true) if selected[:joliet]
        records << "\xffCD001\x01".b.ljust(BLOCK, "\0")
        records.each_with_index.to_h { |bytes, index| [PRIMARY + index, bytes] }
      end

      def write_hierarchy(file, selected:, joliet:)
        rows = path_rows(selected, joliet:)
        [false, true].zip(joliet ? [JOLIET_L, JOLIET_M] : [PATH_L, PATH_M]).each do |big_endian, block|
          bytes = rows.map { |identifier, extent, parent| path_record(identifier:, extent:, parent:, big_endian:) }.join.b
          file.pwrite(bytes, block * BLOCK)
        end
        root = joliet ? JOLIET_ROOT : ROOT
        file.pwrite(root_records(selected, joliet:).join.b, root * BLOCK)
        if selected[:subdirectory]
          sub = if joliet then JOLIET_SUB
                elsif selected[:relocation] then RELOCATED
                else SUB
                end
          parent = !joliet && selected[:relocation] ? SUB : root
          use = selected[:rockridge] && !joliet
          self_use = use ? rr(directory: true, links: 2) : "".b
          parent_use = use ? rr(directory: true, links: 3) : "".b
          parent_use += pointer("PL", ROOT) if use && selected[:relocation]
          child_use = use ? rr(directory: false, name: "LEAF.BIN") : "".b
          records = [record(identifier: "\0", extent: sub, bytes: BLOCK, flags: 2, system_use: self_use),
                     record(identifier: "\x01", extent: parent, bytes: BLOCK, flags: 2, system_use: parent_use),
                     record(identifier: joliet ? ucs2("LEAF.BIN;1") : "LEAF.BIN;1", extent: LEAF,
                            bytes: 7, flags: 0, system_use: child_use)]
          file.pwrite(records.join.b, sub * BLOCK)
        end
        write_container(file) if !joliet && selected[:relocation]
      end

      def root_records(selected, joliet:)
        root = joliet ? JOLIET_ROOT : ROOT
        use = selected[:rockridge] && !joliet
        links = 2 + (selected[:subdirectory] ? 1 : 0) + (selected[:relocation] && !joliet ? 1 : 0)
        root_use = use ? handshake(links:) : "".b
        parent_use = use ? rr(directory: true, links:) : "".b
        records = [record(identifier: "\0", extent: root, bytes: BLOCK, flags: 2, system_use: root_use),
                   record(identifier: "\x01", extent: root, bytes: BLOCK, flags: 2, system_use: parent_use)]
        file_use = use ? rr(directory: false, name: "FILE.TXT") : "".b
        if use && selected[:continuation]
          file_use = rr(directory: false) + entry("CE", both32(CONTINUATION) + both32(0) + both32(nm("FILE.TXT").bytesize))
        end
        identifier = joliet ? ucs2("FILE.TXT;1") : "FILE.TXT;1"
        records << record(identifier:, extent: DATA, bytes: selected[:sections] ? BLOCK : 9,
                          flags: selected[:sections] ? 0x80 : 0, system_use: file_use)
        records << record(identifier:, extent: LEAF, bytes: 9, flags: 0) if selected[:sections]
        if selected[:symlink] && !joliet
          metadata = px(kind: "symlink") + tf + nm("LINK")
          stream = sl_entries(target(selected)).join.b
          areas = sl_areas(selected)
          metadata += if areas.empty? then stream
                      else entry("CE", both32(SL_BLOCK) + both32(0) + both32(areas.fetch(SL_BLOCK).bytesize))
                      end
          records << record(identifier: "LINK.;1", extent: 0, bytes: 0, flags: 0, system_use: metadata)
        end
        if selected[:fifo] && !joliet
          records << record(identifier: "PIPE.;1", extent: 0, bytes: 0, flags: 0,
                            system_use: px(kind: "fifo") + tf + nm("PIPE"))
        end
        if selected[:relocation] && !joliet
          records << record(identifier: "RR_MOVED", extent: SUB, bytes: BLOCK, flags: 3,
                            system_use: rr(directory: true, name: ".rr_moved", links: 3))
          records << record(identifier: "SUB", extent: 0, bytes: 0, flags: 0,
                            system_use: rr(directory: true, name: "SUB", links: 2) + pointer("CL", RELOCATED))
        elsif selected[:subdirectory]
          records << record(identifier: joliet ? ucs2("SUB") : "SUB", extent: joliet ? JOLIET_SUB : SUB,
                            bytes: BLOCK, flags: 2, system_use: use ? rr(directory: true, name: "SUB", links: 2) : "".b)
        end
        records
      end

      def write_container(file)
        records = [record(identifier: "\0", extent: SUB, bytes: BLOCK, flags: 2, system_use: rr(directory: true, links: 3)),
                   record(identifier: "\x01", extent: ROOT, bytes: BLOCK, flags: 2, system_use: rr(directory: true, links: 4)),
                   record(identifier: "SUB_0000001", extent: RELOCATED, bytes: BLOCK, flags: 2,
                          system_use: rr(directory: true, name: "SUB", links: 2) + entry("RE", "".b))]
        file.pwrite(records.join.b, SUB * BLOCK)
      end

      def boot_catalog(sections: false)
        raw = "\0".b * BLOCK
        raw.setbyte(0, 1)
        raw[30, 2] = "\x55\xaa".b
        raw[28, 2] = [(-raw.byteslice(0, 32).unpack("v16").sum) & 0xffff].pack("v")
        raw.setbyte(32, 0x88)
        raw[38, 2] = [1].pack("v")
        raw[40, 4] = [DATA].pack("V")
        if sections
          raw.setbyte(64, 0x91)
          raw.setbyte(65, 0xef)
          raw[66, 2] = [1].pack("v")
          raw.setbyte(96, 0x88)
          raw[102, 2] = [1].pack("v")
          raw[104, 4] = [LEAF].pack("V")
        end
        raw
      end

      def ucs2(text) = text.encode(Encoding::UTF_16BE).b

      def target(selected)
        length = selected.fetch(:target_length)
        return [".././FILE.TXT".b, "/a//b/./../c/".b, "/".b * 1024].fetch(selected.fetch(:target_shape)) if length.zero?

        ("t".b * length).bytes.each_with_index.map { |byte, index| index % 256 == 255 ? 47 : byte }.pack("C*")
      end

      def sl_entries(target)
        roots = target.bytes.take_while { _1 == 47 }.size
        pieces = Array.new(roots) { [8, 0].pack("CC") }
        rest = target.byteslice(roots, target.bytesize - roots)
        unless rest.empty?
          pieces += rest.split("/".b, -1).flat_map do |component|
            if component == ".".b then [[2, 0].pack("CC")]
            elsif component == "..".b then [[4, 0].pack("CC")]
            else
              chunks = component.empty? ? ["".b] : component.bytes.each_slice(248).map { _1.pack("C*") }
              chunks.each_with_index.map { |chunk, index| [index + 1 < chunks.size ? 1 : 0, chunk.bytesize].pack("CC") + chunk }
            end
          end
        end
        groups = pieces.each_with_object([+"".b]) do |piece, packed|
          packed << +"".b if packed.last.bytesize + piece.bytesize > 250
          packed.last << piece
        end
        groups.each_with_index.map { |bytes, index| entry("SL", [index + 1 < groups.size ? 1 : 0].pack("C") + bytes) }
      end

      def sl_areas(selected)
        stream = sl_entries(target(selected))
        return {} if (px(kind: "symlink") + tf + nm("LINK") + stream.join.b).bytesize <= 214

        groups = stream.each_with_object([+"".b]) do |entry, packed|
          packed << +"".b if packed.last.bytesize + entry.bytesize > BLOCK - 28
          packed.last << entry
        end
        groups.each_with_index.to_h do |bytes, index|
          following = if index + 1 < groups.size
            count = groups[index + 1].bytesize + (index + 2 < groups.size ? 28 : 0)
            entry("CE", both32(SL_BLOCK + index + 1) + both32(0) + both32(count))
          else
            "".b
          end
          [SL_BLOCK + index, bytes + following]
        end
      end
    end
  end
end
