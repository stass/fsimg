# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "date"
require "digest"
require "set"

module FsimgSpec
  module Iso9660
    Descriptor = Data.define(:offset, :raw, :fields)
    SuspEntry = Data.define(:offset, :raw, :fields, :scope, :area)
    Continuation = Data.define(:offset, :length, :previous, :scope, :block_le, :block_be,
                              :within_le, :within_be, :length_be)
    DirectoryRecord = Data.define(:offset, :raw, :fields, :identifier, :system_use, :entries, :scope)
    Directory = Data.define(:extent, :bytes, :parent, :identifier, :records, :tails, :joliet, :available)
    Hierarchy = Data.define(:descriptor, :root, :directories, :records, :paths_l, :paths_m, :susp, :rockridge)
    PathRecord = Data.define(:raw, :identifier, :extent, :parent, :attribute_blocks)

    # The independent model holds decoded bytes, not fsimg's interpretation.
    # Bad framing remains a fact for rules to judge. Only reaching a work or
    # digest bound makes its dependent scopes unclassified. Fixed layouts
    # are read only from complete spans; absent values are explicit.
    class Model < Data.define(:image_length, :descriptors, :primary, :joliet, :continuations, :susp_entries,
                              :catalogs, :tree, :prepared_records, :rr_records, :omitted_names, :cut_off)
      BLOCK = 2048
      UNKNOWN = Undefined.new(reason: "structure is absent or truncated")
      VALIDATION_SCOPES = { "verify" => "structural", "verify-deep" => "initialized-payload" }.freeze

      def self.decode(image, ceilings: nil) = Decoder.new(image, ceilings:).call

      def volume = descriptors.find { _1.fields.descriptor_type == 1 }
      def hierarchies = [primary, joliet].reject { FsimgSpec.undefined?(_1) }
      def records = hierarchies.flat_map { [_1.root, *_1.records] }
      def rr_entries = rr_records.flat_map(&:entries)
      def view
        return "rockridge" if !FsimgSpec.undefined?(primary) && primary.rockridge

        FsimgSpec.undefined?(joliet) ? "primary" : "joliet"
      end
      def unclassified_scopes = cut_off

      def record_problems(result)
        records = result.outcome.records
        case result.stage
        in "info"
          info = records.select { _1["type"] == "info" }
          return ["info does not contain exactly one volume"] unless info.one?

          expected = { "format" => "iso9660", "profile" => view }
          expected.merge!("volume_bytes" => volume.fields.space_le * BLOCK, "sector_bytes" => BLOCK,
                          "cluster_bytes" => BLOCK) if volume
          problems = expected.filter_map { |field, value| "info #{field} differs" unless info.first[field].to_s == value.to_s }
          entries = catalogs.flat_map { Catalog.decode(_1.last).entries }
          details = {"boot_catalog_block" => catalogs.first&.fetch(1) || 0, "boot_entries" => entries.size,
                     "boot_x86" => entries.count { _1.platform == 0 }, "boot_efi" => entries.count { _1.platform == 0xef }}
          problems + details.filter_map { |field, value| "info #{field} differs" unless info.first.fetch("details").fetch(field).to_s == value.to_s }
        in _ if FsimgSpec.undefined?(tree) then []
        in "ls"
          actual = records.select { _1["type"] == "entry" }.map do |row|
            WalkedEntry.new(path: row.fetch("path").delete_prefix("/").b, type: row.fetch("kind"),
                            size: %w[file symlink].include?(row.fetch("kind")) ? Integer(row.fetch("size_bytes"), 10) : nil, sha256: nil)
          end
          KnownTree.differences("ls", actual:, expected: tree.map { _1.with(sha256: nil) })
        in "extract" then KnownTree.differences("extract", actual: KnownTree.from_walk(result.tree), expected: tree)
        in "verify" | "verify-deep"
          verify = records.select { _1["type"] == "verify" }
          return ["verify does not contain exactly one report"] unless verify.one?

          files = tree.reject { _1.type == "directory" }
          expected = { "complete" => true, "validation_scope" => VALIDATION_SCOPES.fetch(result.stage),
                       "files" => files.size, "directories" => tree.count { _1.type == "directory" } + 1,
                       "logical_bytes" => files.sum { _1.size || 0 } }
          expected.filter_map { |field, value| "verify #{field} differs" unless verify.first[field].to_s == value.to_s }
        else []
        end
      end
    end

    class Decoder
      BLOCK = Model::BLOCK
      MAX_DESCRIPTORS = 65
      MAX_AREAS = 17
      CHUNK = 1 << 16

      def initialize(image, ceilings: nil)
        @image = image
        @remaining = ceilings ? [8 * ceilings.entries + 8192, 1_000_000].min : 100_000
        @digest_limit = ceilings ? ceilings.output : 16 << 20
        @depth_limit = ceilings ? ceilings.depth : 256
        @path_limit = ceilings ? ceilings.path - 2 : 65534
        @digest_bytes = 0
        @cut_off = []
        @continuations = []
        @susp_entries = []
        @prepared_records = []
        @rr_records = []
        @omitted_names = []
      end

      def call
        descriptors = scan_descriptors
        primary_descriptor = descriptors.find { _1.fields.descriptor_type == 1 }
        joliet_descriptor = descriptors.find { joliet?(_1.fields) }
        primary = primary_descriptor ? hierarchy(primary_descriptor, joliet: false) : Model::UNKNOWN
        joliet = joliet_descriptor ? hierarchy(joliet_descriptor, joliet: true) : Model::UNKNOWN
        selected = if !FsimgSpec.undefined?(primary) && primary.rockridge then primary
                   elsif !FsimgSpec.undefined?(joliet) then joliet
                   else primary
                   end
        tree = FsimgSpec.undefined?(selected) ? Model::UNKNOWN : logical_tree(selected)
        Model.new(image_length: @image.length, descriptors: descriptors.freeze, primary:, joliet:,
                  continuations: @continuations.freeze, susp_entries: @susp_entries.freeze,
                  catalogs: catalogs(descriptors).freeze, tree:, prepared_records: @prepared_records.freeze,
                  rr_records: @rr_records.freeze,
                  omitted_names: @omitted_names.freeze, cut_off: @cut_off.freeze)
      end

      private

      # Charge every retained metadata read before allocating its bytes.
      # Four input bytes can hold at most one SUSP entry, the densest
      # record, so this also bounds cumulative decoded objects. Derived
      # paths use the same budget before concatenation. Payload digests
      # retain only one chunk and have their separate byte ceiling.
      def read(offset, count, scope: "directories")
        available = [[@image.length - offset, 0].max, count].min
        return "".b unless reserve(available, scope:)

        @image.read(offset, available)
      end

      def reserve(bytes, scope:)
        units = 1 + (bytes + 3) / 4
        if units > @remaining
          incomplete(scope)
          return false
        end
        @remaining -= units
        true
      end

      def incomplete(scope)
        dependent = case scope
                    in "open" then %w[open directories files verification destination]
                    in "verification" then %w[verification]
                    in "files" then %w[files verification destination]
                    else %w[directories files verification destination]
                    end
        @cut_off |= dependent
      end

      def complete(record, bytes)
        bytes.bytesize >= record.new.num_bytes ? Layout.decode(record, bytes) : Model::UNKNOWN
      end

      def scan_descriptors
        descriptors = []
        MAX_DESCRIPTORS.times do |index|
          raw = read((16 + index) * BLOCK, BLOCK, scope: "open")
          break if raw.bytesize < BLOCK

          descriptor = Descriptor.new(offset: (16 + index) * BLOCK, raw: raw.freeze,
                                      fields: Layout.decode(Layout::Descriptor, raw))
          descriptors << descriptor
          break if descriptor.fields.descriptor_type == 255
        end
        descriptors.freeze
      end

      def joliet?(descriptor) = descriptor.descriptor_type == 2 && descriptor.descriptor_version == 1 &&
        ["%/@".b, "%/C".b, "%/E".b].include?(descriptor.escapes.byteslice(0, 3)) &&
        descriptor.escapes.byteslice(3, 29).bytes.all?(&:zero?)

      def hierarchy(descriptor, joliet:)
        root = decode_record(descriptor.offset + 156, descriptor.fields.root_record, scope: "open", susp: false)
        if FsimgSpec.undefined?(root.fields)
          return Hierarchy.new(descriptor:, root:, directories: [].freeze, records: [].freeze,
            paths_l: [].freeze, paths_m: [].freeze, susp: false, rockridge: false)
        end

        root_fields = root.fields
        root_raw = read(root_fields.extent_le * BLOCK, [root_fields.data_bytes_le, BLOCK].min, scope: joliet ? "directories" : "open")
        first = decode_record(root_fields.extent_le * BLOCK, root_raw.byteslice(0, root_raw.getbyte(0) || 0) || "".b,
                              scope: "open", susp: false)
        susp = !joliet && !FsimgSpec.undefined?(first.system_use) && first.system_use.bytesize >= 7 &&
          first.system_use.byteslice(0, 7).match?(/\ASP\x07\x01\xbe\xef/n)
        handshake = susp ? entries(first.system_use, first.offset + first.raw.bytesize - first.system_use.bytesize, "open") : []
        skip = susp ? first.system_use.getbyte(6) : 0
        rockridge = susp && skip.zero? && handshake.any? { _1.fields.signature == "ER" && extension?(_1.raw) }
        directories = []
        records = []
        queue = [[root_fields.extent_le, root_fields.data_bytes_le, root_fields.extent_le, "\0".b]]
        seen = Set.new
        until queue.empty?
          break if @remaining.zero?
          extent, bytes, parent, identifier = queue.shift
          next unless seen.add?(extent)

          directory = directory(extent:, bytes:, parent:, identifier:, joliet:, susp: susp && skip.zero?)
          directories << directory
          records.concat(directory.records)
          directory.records.drop(2).each do |record|
            next if FsimgSpec.undefined?(record.fields) || FsimgSpec.undefined?(record.identifier)
            next unless record.fields.flags & 2 != 0

            queue << [record.fields.extent_le, record.fields.data_bytes_le, extent, record.identifier]
          end
        end
        Hierarchy.new(descriptor:, root:, directories: directories.freeze, records: records.freeze,
                      paths_l: paths(descriptor.fields, big_endian: false),
                      paths_m: paths(descriptor.fields, big_endian: true), susp:, rockridge:)
      end

      def directory(extent:, bytes:, parent:, identifier:, joliet:, susp:)
        records = []
        tails = []
        offset = 0
        while offset < bytes
          available = [BLOCK - offset % BLOCK, bytes - offset].min
          raw = read(extent * BLOCK + offset, available)
          if raw.empty? || raw.getbyte(0).zero?
            tails << raw.freeze
            break if raw.bytesize < available

            offset += available
            next
          end
          length = raw.getbyte(0)
          record = decode_record(extent * BLOCK + offset, raw.byteslice(0, length), scope: "directories", susp:)
          records << record
          break if length < 34 || length > available || raw.bytesize < length

          offset += length
        end
        padding = (BLOCK - bytes % BLOCK) % BLOCK
        tails << read(extent * BLOCK + bytes, padding).freeze if padding.positive?
        available = [[@image.length / BLOCK - extent, 0].max * BLOCK, bytes].min
        Directory.new(extent:, bytes:, parent:, identifier:, records: records.freeze, tails: tails.freeze, joliet:, available:)
      end

      def decode_record(offset, raw, scope:, susp:)
        fields = complete(Layout::DirectoryHeader, raw)
        if FsimgSpec.undefined?(fields)
          return DirectoryRecord.new(offset:, raw: raw.freeze, fields:, identifier: Model::UNKNOWN,
                                     system_use: Model::UNKNOWN, entries: [].freeze, scope:)
        end
        identifier = raw.byteslice(33, fields.identifier_bytes)
        identifier = Model::UNKNOWN unless identifier && identifier.bytesize == fields.identifier_bytes
        start = 33 + fields.identifier_bytes + (fields.identifier_bytes.even? ? 1 : 0)
        system_use = start <= raw.bytesize ? raw.byteslice(start, raw.bytesize - start).freeze : Model::UNKNOWN
        decoded = susp && !FsimgSpec.undefined?(system_use) ? entries(system_use, offset + start, scope) : []
        DirectoryRecord.new(offset:, raw: raw.freeze, fields:, identifier:, system_use:,
                            entries: decoded.freeze, scope:)
      end

      def entries(initial, initial_offset, scope)
        areas = [[initial_offset, initial.bytesize]]
        raw = initial
        offset = initial_offset
        found = []
        MAX_AREAS.times do
          at = 0
          pending = nil
          first_entry = found.size
          while at + 4 <= raw.bytesize && raw.getbyte(at) != 0
            header = Layout.decode(Layout::SuspHeader, raw.byteslice(at, 4))
            length = header.entry_bytes
            bytes = raw.byteslice(at, [length, 4].max) || "".b
            found << SuspEntry.new(offset: offset + at, raw: bytes.freeze, fields: header, scope:, area: offset)
            break if length < 4 || at + length > raw.bytesize
            if header.signature == "ST"
              @susp_entries.concat(found)
              return found
            end

            if header.signature == "CE" && length == 28
              block, within, count = [4, 12, 20].map { bytes.byteslice(_1, 4).unpack1("V") }
              block_be, within_be, length_be = [8, 16, 24].map { bytes.byteslice(_1, 4).unpack1("N") }
              next_offset = block * BLOCK + within
              @continuations << Continuation.new(offset: next_offset, length: count, previous: areas.map { _1.dup.freeze }.freeze, scope:,
                                                 block_le: block, block_be:, within_le: within, within_be:, length_be:)
              if block == block_be && within == within_be && count == length_be && within < BLOCK
                pending = [next_offset, count] if pending.nil?
              end
            end
            at += length
          end
          break unless pending && found.drop(first_entry).count { _1.fields.signature == "CE" } == 1
          offset, count = pending
          break if count.zero? || count > BLOCK || offset / BLOCK != (offset + count - 1) / BLOCK
          break if areas.any? { |start, size| offset < start + size && start < offset + count }

          areas << [offset, count]
          raw = read(offset, count, scope:)
        end
        @susp_entries.concat(found)
        found
      end

      def extension?(raw)
        RockRidge.extension_shape?(raw) && raw.getbyte(7) == 1 &&
          %w[RRIP_1991A IEEE_P1282 IEEE_1282].include?(raw.byteslice(8, raw.getbyte(4)))
      end

      def paths(descriptor, big_endian:)
        block = big_endian ? descriptor.path_m : descriptor.path_l
        raw = read(block * BLOCK, descriptor.path_bytes_le, scope: "verification")
        found = []
        offset = 0
        while offset < raw.bytesize
          length = raw.getbyte(offset)
          bytes = 8 + length + length % 2
          piece = raw.byteslice(offset, bytes)
          break if piece.bytesize < 8

          found << PathRecord.new(raw: piece.freeze, identifier: piece.byteslice(8, length).freeze,
                                  extent: piece.byteslice(2, 4).unpack1(big_endian ? "N" : "V"),
                                  parent: piece.byteslice(6, 2).unpack1(big_endian ? "n" : "v"),
                                  attribute_blocks: piece.getbyte(1))
          break if length.zero? || piece.bytesize < bytes

          offset += bytes
        end
        found.freeze
      end

      def catalogs(descriptors)
        descriptors.filter_map do |descriptor|
          next unless descriptor.fields.descriptor_type.zero? &&
            descriptor.raw.byteslice(7, 32) == "EL TORITO SPECIFICATION".b.ljust(32, "\0")

          block = descriptor.raw.byteslice(71, 4).unpack1("V")
          [descriptor, block, read(block * BLOCK, BLOCK, scope: "open").freeze].freeze
        end
      end

      def logical_tree(hierarchy)
        directories = hierarchy.directories.to_h { [_1.extent, _1] }
        root = directories[hierarchy.descriptor.fields.root_record.byteslice(2, 4).unpack1("V")]
        return Model::UNKNOWN unless root

        tree = []
        queue = [[root, "".b, 0]]
        seen = Set.new
        until queue.empty?
          directory, path, depth = queue.shift
          return Model::UNKNOWN unless seen.add?(directory.extent)
          @prepared_records.concat(directory.records)
          if hierarchy.rockridge
            @rr_records.concat(directory.records.take(2) + runs(directory.records.drop(2)).map(&:first))
          end

          runs(directory.records.drop(2)).each do |run|
            return Model::UNKNOWN if run.any? { FsimgSpec.undefined?(_1.fields) }
            record = run.first
            return Model::UNKNOWN if FsimgSpec.undefined?(record.identifier) || FsimgSpec.undefined?(record.fields)
            next if hierarchy.rockridge && signature(record, "RE")

            child = hierarchy.rockridge && signature(record, "CL")
            target = child ? child.raw.byteslice(4, 4)&.unpack1("V") : record.fields.extent_le
            directory_kind = record.fields.flags & 2 != 0 || child
            view = if hierarchy.rockridge then "rockridge"
                   elsif directory.joliet then "joliet"
                   else "primary"
                   end
            name = Names.decode(record, view:)
            return Model::UNKNOWN unless Names.native?(name, view:)
            return Model::UNKNOWN if hierarchy.rockridge && name.bytesize > 1024
            unless Names.portable?(name)
              @omitted_names << name.freeze
              next
            end

            bytes = path.bytesize + (path.empty? ? 0 : 1) + name.bytesize
            if depth + 1 > @depth_limit || bytes > @path_limit || !reserve(bytes, scope: "directories")
              incomplete("directories")
              return Model::UNKNOWN
            end
            member = path.empty? ? name : path + "/".b + name
            if directory_kind
              tree << WalkedEntry.new(path: member.freeze, type: "directory", size: nil, sha256: nil)
              following = directories[target]
              return Model::UNKNOWN unless following

              queue << [following, member, depth + 1]
            elsif hierarchy.rockridge && RockRidge.kind(record) == "symlink"
              target = RockRidge.target(record)
              tree << WalkedEntry.new(path: member.freeze, type: "symlink", size: target.length,
                                     sha256: Digest::SHA256.hexdigest(target.bytes))
            elsif hierarchy.rockridge && RockRidge.kind(record) == "fifo"
              tree << WalkedEntry.new(path: member.freeze, type: "fifo", size: nil, sha256: nil)
            else
              size = run.sum { _1.fields.data_bytes_le }
              digest = digest(run)
              tree << WalkedEntry.new(path: member.freeze, type: "file", size:, sha256: digest)
            end
          end
        end
        tree.freeze
      end

      def runs(records)
        records.each_with_object([]) do |record, groups|
          if groups.any? && !FsimgSpec.undefined?(groups.last.last.fields) && groups.last.last.fields.flags & 0x80 != 0
            groups.last << record
          else
            groups << [record]
          end
        end
      end

      def signature(record, name) = record.entries.find { _1.fields.signature == name }

      def digest(records)
        size = records.sum { FsimgSpec.undefined?(_1.fields) ? 0 : _1.fields.data_bytes_le }
        if @digest_bytes + size > @digest_limit
          incomplete("files")
          return nil
        end
        @digest_bytes += size
        digest = Digest::SHA256.new
        records.each do |record|
          return nil if FsimgSpec.undefined?(record.fields)

          offset = (record.fields.extent_le + record.fields.attribute_blocks) * BLOCK
          remaining = record.fields.data_bytes_le
          while remaining.positive?
            bytes = @image.read(offset, [remaining, CHUNK].min)
            return nil if bytes.empty?

            digest.update(bytes)
            offset += bytes.bytesize
            remaining -= bytes.bytesize
          end
        end
        digest.hexdigest
      end
    end
  end
end
