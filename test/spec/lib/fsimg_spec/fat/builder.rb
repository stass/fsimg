# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "set"

module FsimgSpec
  module Fat
    # Parameters the builder cannot render: a field value wider than its
    # field, a zero divisor, a tail that adds a cluster, a tree larger than
    # the volume, a name no directory entry set can hold, or an image
    # larger than the caller allows. A base the builder refuses is one the
    # harness cannot materialize.
    class BuilderError < FixtureError; end

    # The independent FAT builder (specs/design-spec-testing.md#fixtures),
    # written from the Microsoft FAT specification. It renders an image from
    # explicit parameters: the boot sector and BPB (section 3), every FAT
    # with its reserved entries (section 4), the FAT12/16 root region or
    # FAT32 root cluster (section 6.6), FSInfo and the backup boot sector
    # (sections 3.6 and 5), and a file tree with long names (sections 6
    # and 7). It derives every dependent field from its parameters and never
    # checks them against the specification, so an out-of-domain value is
    # rendered coherently. It refuses a value its field cannot hold, a
    # layout whose structures would overwrite one another, such as a FAT
    # written over the boot sector, and one whose decoded cluster count
    # would differ from clusters; a mutation of a rendered image sets such
    # a value instead.
    #
    # The builder runs in the harness's own process, where no file-size
    # limit confines it, and a recipe's geometry can describe an image of
    # up to 2^32 sectors of 65535 bytes, near 256 TiB. write therefore
    # refuses an image larger than its max_bytes before creating the file.
    # Every byte it writes, file data included, lies inside the image, so
    # that bound covers all of them.
    #
    # clusters fixes the data-cluster count, and with it the variant; the
    # FAT size is the smallest that holds clusters + 2 entries unless
    # fat_size is given, and the volume ends after the last cluster plus
    # tail_sectors.
    module Builder
      NAME = "independent"
      DEFAULTS = {
        bytes_per_sector: 512, sectors_per_cluster: 1, fat_count: 2, media: 0xF8, volume_id: 0x1234_5678,
        hidden_sectors: 0, tail_sectors: 0, fs_info: 1, backup_boot: 6, root_cluster: 2
      }.freeze
      # The width in bits of the field each parameter fills, which bounds
      # it. fat_size fills BPB_FATSz16 on FAT12/16, and tail_sectors, like
      # the total it extends, BPB_TotSec32.
      PARAMETER_BITS = {
        clusters: 32, bytes_per_sector: 16, sectors_per_cluster: 8, reserved_sectors: 16, fat_count: 8, root_entries: 16,
        media: 8, volume_id: 32, hidden_sectors: 32, fs_info: 16, backup_boot: 16, root_cluster: 32, fat_size: 32,
        tail_sectors: 32
      }.freeze
      # The FAT32 boot record and its backup copy are three sectors long
      # (section 3.6).
      BOOT_RECORD_SECTORS = 3
      # The variant a data-cluster count selects (section 3.5): fewer than
      # 4085 clusters is FAT12, fewer than 65525 FAT16, and any more FAT32.
      # Each FAT entry is 12, 16 or 32 bits wide (section 4). The builder
      # keeps its own copy of these rather than share geometry.rb's, so an
      # error in the decoder it is checked against cannot hide in both.
      FAT16_MINIMUM_CLUSTERS = 4085
      FAT32_MINIMUM_CLUSTERS = 65_525
      ENTRY_BITS = { "fat12" => 12, "fat16" => 16, "fat32" => 32 }.freeze
      # The variant's own defaults (section 3.4): one reserved sector and a
      # 512-entry root directory for FAT12/16, 32 reserved sectors and no
      # root region for FAT32.
      VARIANT_DEFAULTS = {
        "fat12" => { reserved_sectors: 1, root_entries: 512 },
        "fat16" => { reserved_sectors: 1, root_entries: 512 },
        "fat32" => { reserved_sectors: 32, root_entries: 0 }
      }.freeze

      # Writes the image to a new file at path and answers its Layout. The
      # layout and every name are checked before the file is created, so a
      # refusal leaves nothing behind.
      def self.write(path, parameters:, tree:, max_bytes:)
        layout = Layout.new(parameters)
        raise BuilderError, "the #{layout.image_bytes}-byte image exceeds the #{max_bytes}-byte limit" if
          layout.image_bytes > max_bytes

        allocation = Allocation.new(layout, tree)
        File.open(path, File::WRONLY | File::CREAT | File::EXCL | File::BINARY, 0o600) do |out|
          out.truncate(layout.image_bytes)
          Renderer.new(layout, allocation, out).call
        end
        layout
      end

      # The Allocation the image of parameters and tree would have,
      # computed in memory, or a BuilderError where the builder cannot lay
      # them out.
      def self.plan(parameters:, tree:) = Allocation.new(Layout.new(parameters), tree)

      # An on-disk record given as its fields in order, each
      # [specification field name, pack directive, value], so every row
      # can be checked against the specification's table for the record.
      # The names document the table; only directives and values are
      # packed. A value is an Array where its directive repeats, as v5.
      module Record
        def self.pack(fields) =
          fields.flat_map { |_name, _directive, value| Array(value) }
                .pack(fields.map { |_name, directive, _value| directive }.join)
      end

      # The volume's geometry, every field derived from the parameters.
      class Layout
        attr_reader :variant, :bytes_per_sector, :sectors_per_cluster, :reserved_sectors, :fat_count, :root_entries,
                    :clusters, :media, :volume_id, :hidden_sectors, :fat_size, :root_dir_sectors, :first_data_sector,
                    :total_sectors, :fs_info, :backup_boot, :root_cluster

        # Every parameter must be an Integer its field can hold, so every
        # structure the builder writes lies within total_sectors, which
        # itself fits BPB_TotSec32.
        def initialize(parameters)
          unknown = parameters.keys - PARAMETER_BITS.keys
          raise BuilderError, "unknown builder parameters #{unknown.join(", ")}" unless unknown.empty?

          @clusters = parameters.fetch(:clusters) { raise BuilderError, "the builder needs a cluster count" }
          check_field(:clusters, @clusters)
          @variant = variant_for(@clusters)
          values = DEFAULTS.merge(VARIANT_DEFAULTS.fetch(@variant)).merge(parameters)
          values.except(:fat_size).each { |name, value| check_field(name, value) }

          @bytes_per_sector = values.fetch(:bytes_per_sector)
          @sectors_per_cluster = values.fetch(:sectors_per_cluster)
          @reserved_sectors = values.fetch(:reserved_sectors)
          @fat_count = values.fetch(:fat_count)
          @root_entries = values.fetch(:root_entries)
          @media = values.fetch(:media)
          @volume_id = values.fetch(:volume_id)
          @hidden_sectors = values.fetch(:hidden_sectors)
          @fs_info = values.fetch(:fs_info)
          @backup_boot = values.fetch(:backup_boot)
          @root_cluster = values.fetch(:root_cluster)
          tail_sectors = values.fetch(:tail_sectors)
          raise BuilderError, "zero clusters, bytes per sector or sectors per cluster" if
            [@clusters, @bytes_per_sector, @sectors_per_cluster].any?(&:zero?)

          check_tail(tail_sectors)
          check_reserved_region

          @fat_size = values.fetch(:fat_size) { minimal_fat_size }
          check_field(:fat_size, @fat_size, bits: fat32? ? 32 : 16)
          @root_dir_sectors = ((@root_entries * 32) + @bytes_per_sector - 1) / @bytes_per_sector
          @first_data_sector = @reserved_sectors + (@fat_count * @fat_size) + @root_dir_sectors
          @total_sectors = @first_data_sector + (@clusters * @sectors_per_cluster) + tail_sectors
          check_field(:total_sectors, @total_sectors, bits: 32)
        end

        def fat32? = variant == "fat32"

        def entry_bits = ENTRY_BITS.fetch(variant)

        def image_bytes = total_sectors * bytes_per_sector

        def cluster_bytes = sectors_per_cluster * bytes_per_sector

        def sector_offset(sector) = sector * bytes_per_sector

        def cluster_offset(cluster) = sector_offset(first_data_sector + ((cluster - 2) * sectors_per_cluster))

        def fat_offset(copy) = sector_offset(reserved_sectors + (copy * fat_size))

        def root_offset = sector_offset(reserved_sectors + (fat_count * fat_size))

        private

        def variant_for(clusters)
          if clusters < FAT16_MINIMUM_CLUSTERS then "fat12"
          elsif clusters < FAT32_MINIMUM_CLUSTERS then "fat16"
          else "fat32"
          end
        end

        def check_field(name, value, bits: PARAMETER_BITS.fetch(name))
          return if value.is_a?(Integer) && value.between?(0, (1 << bits) - 1)

          raise BuilderError, "#{name} #{FsimgSpec.shown(value)} does not fit a #{bits}-bit field"
        end

        # A reader counts clusters as the data sectors divided by
        # sectors_per_cluster, rounding down (section 3.5), so a tail of a
        # whole cluster or more would decode as more clusters, and perhaps
        # another variant, than the parameters name.
        def check_tail(tail_sectors)
          return if tail_sectors < @sectors_per_cluster

          raise BuilderError, "#{tail_sectors} tail sectors make up a whole #{@sectors_per_cluster}-sector cluster"
        end

        # The byte ranges of the 512-byte records the builder writes into
        # the reserved region: the boot sector and, for FAT32, FSInfo and
        # the backup boot sector with its FSInfo copy, unless backup_boot is
        # zero.
        def record_extents
          at = ->(sector) { (sector * @bytes_per_sector)...((sector * @bytes_per_sector) + 512) }
          sectors =
            if !fat32? then [0]
            elsif @backup_boot.zero? then [0, @fs_info]
            else [0, @fs_info, @backup_boot, @backup_boot + @fs_info]
            end
          sectors.map(&at)
        end

        # The records must not overlap, and they and the three-sector FAT32
        # boot record at backup_boot, or at sector 0 without a backup, must
        # lie inside the reserved region, or one record, or a FAT written
        # after them, would overwrite another.
        def check_reserved_region
          extents = record_extents.sort_by(&:begin)
          backup_end = fat32? ? (@backup_boot + BOOT_RECORD_SECTORS) * @bytes_per_sector : 0
          return if extents.each_cons(2).all? { |extent, following| extent.end <= following.begin } &&
                    [*extents.map(&:end), backup_end].max <= @reserved_sectors * @bytes_per_sector

          raise BuilderError, "the boot records overlap or exceed the #{@reserved_sectors}-sector reserved region"
        end

        # The fewest sectors that hold entries 0 through clusters + 1.
        def minimal_fat_size
          bytes = (((@clusters + 2) * entry_bits) + 7) / 8
          (bytes + @bytes_per_sector - 1) / @bytes_per_sector
        end
      end

      # Where every directory and file of the tree lives: directories get
      # their entries' bytes, and every object a contiguous cluster run,
      # a Range, allocated in path order from cluster 2, or on FAT32 from
      # the root cluster, so the used clusters are one run ending before
      # @next.
      class Allocation
        attr_reader :chains, :directories, :files

        # The steps run in this order because each needs the one before.
        # A directory's size, and so its chain, depends on its children's
        # names, so every directory's names are assigned first. The FAT32
        # root takes the first run, starting at root_cluster; each entry
        # then takes the next run in path order. Only once every chain
        # exists can directory_bytes write each child's first cluster.
        def initialize(layout, tree)
          @layout = layout
          @tree = tree.sort_by { _1.path.b }
          @chains = {}
          @next = 2
          @names = names_by_directory
          allocate_root
          @tree.each { allocate(_1) }
          @directories = ([nil] + @tree.select { _1.type == "directory" }.map(&:path)).to_h { [_1, directory_bytes(_1)] }
          @files = @tree.select { _1.type == "file" && _1.size.positive? }
        end

        # The first cluster of path, where nil names the root: the FAT32
        # root's is root_cluster, and the FAT12/16 root, which lies outside
        # the data region, and an empty file have 0.
        def first_cluster(path)
          return @layout.fat32? ? @layout.root_cluster : 0 if path.nil?

          @chains.fetch(path, [0]).first
        end

        def used_clusters = @chains.values.sum(&:size)

        # The first cluster no chain uses, or nil when every one is used.
        # The chains form one run from start, so when a FAT32 root cluster
        # above 2 starts it, clusters 2 up to it are free and 2 is the
        # first; otherwise the run starts at 2 and @next follows it.
        def first_free
          start = @layout.fat32? ? @layout.root_cluster : 2
          if start > 2 then 2
          elsif @next <= @layout.clusters + 1 then @next
          end
        end

        private

        def children(parent) = @tree.select { _1.parent == parent }

        def names_by_directory
          ([nil] + @tree.select { _1.type == "directory" }.map(&:path)).to_h do |directory|
            [directory, ShortNames.assign(children(directory).map { _1.path.rpartition("/").last })]
          end
        end

        def entry_count(directory) =
          (directory ? 2 : 0) + @names.fetch(directory).sum { 1 + LongName.entry_count(_1.long) }

        def allocate_root
          if @layout.fat32?
            raise BuilderError, "root cluster #{@layout.root_cluster} is no data cluster" unless
              (2..@layout.clusters + 1).cover?(@layout.root_cluster)

            @next = @layout.root_cluster
            @chains[nil] = take(clusters_for(entry_count(nil) * 32))
          elsif entry_count(nil) > @layout.root_entries
            raise BuilderError, "#{entry_count(nil)} root entries exceed the #{@layout.root_entries}-entry root region"
          end
        end

        def allocate(entry)
          bytes = entry.type == "directory" ? entry_count(entry.path) * 32 : entry.size
          @chains[entry.path] = take(clusters_for(bytes)) if entry.type == "directory" || bytes.positive?
        end

        def clusters_for(bytes) = [(bytes + @layout.cluster_bytes - 1) / @layout.cluster_bytes, 1].max

        def take(count)
          raise BuilderError, "the tree needs more than #{@layout.clusters} clusters" if @next + count - 1 > @layout.clusters + 1

          (@next..(@next + count - 1)).tap { @next += count }
        end

        # The bytes of directory: dot entries for a subdirectory, then each
        # child's long-name entries and short entry.
        def directory_bytes(directory)
          dots = directory ? [Entries.short(".".ljust(11).b, cluster: first_cluster(directory), directory: true),
                              Entries.short("..".ljust(11).b, cluster: dotdot_cluster(directory), directory: true)] : []
          entries = children(directory).zip(@names.fetch(directory)).flat_map do |child, names|
            [*LongName.entries(names.long, checksum: Entries.checksum(names.short)),
             Entries.short(names.short, cluster: first_cluster(child.path), directory: child.type == "directory",
                                        size: child.type == "file" ? child.size : 0)]
          end
          (dots + entries).join
        end

        # The first cluster a dotdot entry names: 0 when the parent is the
        # root, on every variant (section 6.5).
        def dotdot_cluster(directory)
          parent = directory.rpartition("/").first
          parent.empty? ? 0 : first_cluster(parent)
        end
      end

      # Short names (section 6.1): a name its own 8.3 short name spells
      # exactly is stored as itself; any other takes a long name and a
      # short name the basis-name and numeric-tail algorithms generate.
      #
      # Every name of an entry is a key of its directory, compared without
      # case, and a reader takes a key that two entries share for an
      # ambiguous directory (specs/design-fat-reader.md#directory-preparation).
      # A generated short name therefore never spells the name of another
      # entry, whichever entry comes first: a long abc~1 keeps ABC~1 from
      # a sibling, and a literal NAMEFI~1.TXT keeps its own short name from
      # the long name file.txt. The keys are the builder's own upper case,
      # not the model's, so an error in either cannot hide in both. The
      # builder's OEM character set is ASCII: every other character of a
      # basis name is lossy and becomes "_", which the specification
      # allows for a character the OEM code page lacks.
      module ShortNames
        Names = Data.define(:short, :long)
        CHARACTERS = /\A[A-Z0-9$%'\-_@~`!(){}^#&]+\z/

        # The names of one directory's entries, in order. taken holds the
        # short names given so far, and tried the next number to try for
        # each generated basis, so every name costs constant work.
        def self.assign(names)
          keys = names.to_set { upper(_1) }
          taken = Set.new
          tried = Hash.new(1)
          names.map do |name|
            # A short name is free when no entry before this one took it
            # and it spells no other entry's name: a key it spells is the
            # entry's own only when it is the entry's name in upper case.
            spells_another = ->(short) { spelled(short) != upper(name) && keys.include?(spelled(short)) }
            free = ->(short) { !taken.include?(short) && !spells_another.(short) }
            assigned = untailed(name, free) || numbered_name(name, free, tried)
            taken << assigned.short
            assigned
          end
        end

        # name's short name without a numeric tail when name in upper case
        # is a valid 8.3 name and that short name is free, or nil. Only a
        # name that is its own short name needs no long name.
        def self.untailed(name, free)
          short = plain(upper(name))
          return nil unless short && free.(short)

          Names.new(short:, long: name == spelled(short) ? nil : name)
        end

        # name in upper case, character by character, keeping a character
        # whose upper case is several characters, such as U+00DF, as the
        # simple case mapping a reader compares with keeps it.
        def self.upper(name) = name.each_char.map { |char| char.upcase.then { _1.length == 1 ? _1 : char } }.join

        # The 11-byte DIR_Name of name when it is a valid upper-case 8.3
        # name that DIR_Name spells back exactly, or nil. HELLO. fits as
        # HELLO but would read back as HELLO, so it takes a long name.
        def self.plain(name)
          base, dot, extension = name.rpartition(".")
          base, extension = extension, "" if dot.empty?
          return nil unless fits_8_3?(base, extension)

          short = (base.ljust(8) + extension.ljust(3)).b
          short if spelled(short) == name
        end

        # Whether base and extension are valid upper-case short-name parts:
        # a base of one to eight characters and an extension of up to three.
        private_class_method def self.fits_8_3?(base, extension) =
          CHARACTERS.match?(base) && base.length <= 8 &&
          (extension.empty? || (CHARACTERS.match?(extension) && extension.length <= 3))

        # The name a DIR_Name reads as: its base and extension without
        # their padding, joined by a dot when there is an extension.
        def self.spelled(short)
          base = short.byteslice(0, 8).rstrip
          extension = short.byteslice(8, 3).rstrip
          extension.empty? ? base : "#{base}.#{extension}"
        end

        # name with the short name basis~N, N the first number whose short
        # name is free.
        def self.numbered_name(name, free, tried)
          key = basis(name)
          number = (tried[key]..).find { free.(numbered(*key, _1)) }
          tried[key] = number + 1
          Names.new(short: numbered(*key, number), long: name)
        end

        # The basis name of name, as its primary part and extension: in
        # upper case, every space and the leading periods stripped, the
        # primary part up to the first period and the extension after the
        # last, of at most 8 and 3 characters, each character no short
        # name holds made "_". A name of nothing but periods and spaces,
        # which no directory holds validly, takes "_".
        def self.basis(name)
          stripped = upper(name).delete(" ").sub(/\A\.+/, "")
          extension = stripped.include?(".") ? stripped.rpartition(".").last : ""
          [clean(stripped.partition(".").first)[0, 8].then { _1.empty? ? "_" : _1 }, clean(extension)[0, 3]]
        end

        # The DIR_Name of basis~number with suffix, the basis cut so the
        # name keeps eight characters. ~ and seven digits fill all eight,
        # so a larger number has no short name.
        def self.numbered(basis, suffix, number)
          tail = "~#{number}"
          raise BuilderError, "no eight-character short name holds #{tail}" if tail.length > 8

          ("#{basis[0, 8 - tail.length]}#{tail}".ljust(8) + suffix.ljust(3)).b
        end

        def self.clean(text) = text.chars.map { CHARACTERS.match?(_1) ? _1 : "_" }.join
      end

      # Long-name entries (section 7): the name in UTF-16, 13 units per
      # entry, ended by 0x0000 and padded with 0xFFFF, stored last entry
      # first with LAST_LONG_ENTRY on its ordinal.
      module LongName
        UNITS_PER_ENTRY = 13
        LAST_LONG_ENTRY = 0x40
        ATTR_LONG_NAME = 0x0F
        # LDIR_Ord numbers the entries from 1 (section 7.1) below
        # LAST_LONG_ENTRY, so a name fills at most 63 entries. That is more
        # than the 255 characters section 7 allows, which the builder
        # renders so a reader's limit can be tested.
        MAX_ENTRIES = LAST_LONG_ENTRY - 1

        def self.entry_count(name)
          return 0 if name.nil?

          count = (units(name).size + UNITS_PER_ENTRY - 1) / UNITS_PER_ENTRY
          raise BuilderError, "long name #{FsimgSpec.shown(name)} needs #{count} entries, over #{MAX_ENTRIES}" if
            count > MAX_ENTRIES

          count
        end

        def self.entries(name, checksum:)
          return [] if name.nil?

          count = entry_count(name)
          padded = units(name) + [0x0000] + ([0xFFFF] * UNITS_PER_ENTRY)
          (1..count).map do |ordinal|
            part = padded[(ordinal - 1) * UNITS_PER_ENTRY, UNITS_PER_ENTRY]
            fields = [
              ["LDIR_Ord", "C", ordinal == count ? ordinal | LAST_LONG_ENTRY : ordinal],
              ["LDIR_Name1", "v5", part[0, 5]],
              ["LDIR_Attr", "C", ATTR_LONG_NAME],
              ["LDIR_Type", "C", 0],
              ["LDIR_Chksum", "C", checksum],
              ["LDIR_Name2", "v6", part[5, 6]],
              ["LDIR_FstClusLO", "v", 0],
              ["LDIR_Name3", "v2", part[11, 2]]
            ]
            Record.pack(fields)
          end.reverse
        end

        def self.units(name) = name.encode(Encoding::UTF_16LE).unpack("v*")
      end

      # Short directory entries (section 6) with a fixed timestamp,
      # 2000-01-01 00:00:00, so a build is reproducible.
      module Entries
        DATE = ((2000 - 1980) << 9) | (1 << 5) | 1
        TIME = 0
        ATTR_DIRECTORY = 0x10
        ATTR_ARCHIVE = 0x20

        def self.short(name, cluster:, directory:, size: 0)
          fields = [
            ["DIR_Name", "a11", name],
            ["DIR_Attr", "C", directory ? ATTR_DIRECTORY : ATTR_ARCHIVE],
            ["DIR_NTRes", "C", 0],
            ["DIR_CrtTimeTenth", "C", 0],
            ["DIR_CrtTime", "v", TIME],
            ["DIR_CrtDate", "v", DATE],
            ["DIR_LstAccDate", "v", DATE],
            ["DIR_FstClusHI", "v", cluster >> 16],
            ["DIR_WrtTime", "v", TIME],
            ["DIR_WrtDate", "v", DATE],
            ["DIR_FstClusLO", "v", cluster & 0xFFFF],
            ["DIR_FileSize", "V", size]
          ]
          Record.pack(fields)
        end

        # The checksum of a short name (section 7.2).
        def self.checksum(name) = name.bytes.reduce(0) { |sum, byte| ((((sum & 1) << 7) | (sum >> 1)) + byte) & 0xFF }
      end

      # Writes every region of the image into out, which already has the
      # image's length; everything else stays a hole.
      class Renderer
        END_OF_CHAIN = { 12 => 0xFFF, 16 => 0xFFFF, 32 => 0x0FFF_FFFF }.freeze
        RUN_ENTRIES = 4096

        # BS_jmpBoot (section 3.1): a short jump, EB xx 90, to the byte
        # after the BPB, offset 90 on FAT32 and 62 otherwise.
        FAT32_JUMP = "\xEB\x58\x90".b
        FAT16_JUMP = "\xEB\x3C\x90".b
        # BS_OEMName, the name section 3.1 recommends.
        OEM_NAME = "MSWIN4.1"
        # BPB_SecPerTrk and BPB_NumHeads, the interrupt 0x13 geometry FAT
        # itself never uses: that of a translated hard disk.
        SECTORS_PER_TRACK = 63
        HEADS = 255
        # BS_DrvNum 0x80, a hard disk; BS_BootSig 0x29, which says
        # BS_VolID, BS_VolLab and BS_FilSysType follow (sections 3.2, 3.3).
        DRIVE_NUMBER = 0x80
        EXTENDED_BOOT_SIGNATURE = 0x29
        VOLUME_LABEL = "NO NAME    "
        # Signature_word: 0x55 at byte 510 and 0xAA at byte 511.
        SIGNATURE_OFFSET = 510
        SIGNATURE_WORD = "\x55\xAA".b
        # FSInfo's three signatures (section 5), and FSI_Nxt_Free's value
        # for no hint, given when no cluster is free.
        FSI_LEAD_SIGNATURE = 0x4161_5252
        FSI_STRUCTURE_SIGNATURE = 0x6141_7272
        FSI_TRAIL_SIGNATURE = 0xAA55_0000
        NO_FREE_CLUSTER_HINT = 0xFFFF_FFFF

        def initialize(layout, allocation, out)
          @layout = layout
          @allocation = allocation
          @out = out
        end

        def call
          boot = boot_sector
          @out.pwrite(boot, 0)
          write_fats
          write_directories
          write_files
          write_fat32_records(boot) if @layout.fat32?
        end

        private

        # The BPB fields every variant shares (section 3.1), the variant's
        # extended BPB (sections 3.2 and 3.3), zeros, and the signature.
        def boot_sector
          extended = @layout.fat32? ? fat32_extended_bpb : fat16_extended_bpb
          Record.pack(common_bpb + extended).ljust(SIGNATURE_OFFSET, "\0") + SIGNATURE_WORD
        end

        # The total sector count fills BPB_TotSec16 on a FAT12/16 volume
        # small enough, and BPB_TotSec32 otherwise.
        def common_bpb
          layout = @layout
          total16 = !layout.fat32? && layout.total_sectors < 0x10000 ? layout.total_sectors : 0
          [
            ["BS_jmpBoot", "a3", layout.fat32? ? FAT32_JUMP : FAT16_JUMP],
            ["BS_OEMName", "a8", OEM_NAME],
            ["BPB_BytsPerSec", "v", layout.bytes_per_sector],
            ["BPB_SecPerClus", "C", layout.sectors_per_cluster],
            ["BPB_RsvdSecCnt", "v", layout.reserved_sectors],
            ["BPB_NumFATs", "C", layout.fat_count],
            ["BPB_RootEntCnt", "v", layout.root_entries],
            ["BPB_TotSec16", "v", total16],
            ["BPB_Media", "C", layout.media],
            ["BPB_FATSz16", "v", layout.fat32? ? 0 : layout.fat_size],
            ["BPB_SecPerTrk", "v", SECTORS_PER_TRACK],
            ["BPB_NumHeads", "v", HEADS],
            ["BPB_HiddSec", "V", layout.hidden_sectors],
            ["BPB_TotSec32", "V", total16.zero? ? layout.total_sectors : 0]
          ]
        end

        def fat16_extended_bpb
          [
            ["BS_DrvNum", "C", DRIVE_NUMBER],
            ["BS_Reserved1", "C", 0],
            ["BS_BootSig", "C", EXTENDED_BOOT_SIGNATURE],
            ["BS_VolID", "V", @layout.volume_id],
            ["BS_VolLab", "a11", VOLUME_LABEL],
            ["BS_FilSysType", "a8", @layout.variant.upcase.ljust(8)]
          ]
        end

        def fat32_extended_bpb
          [
            ["BPB_FATSz32", "V", @layout.fat_size],
            ["BPB_ExtFlags", "v", 0],
            ["BPB_FSVer", "v", 0],
            ["BPB_RootClus", "V", @layout.root_cluster],
            ["BPB_FSInfo", "v", @layout.fs_info],
            ["BPB_BkBootSec", "v", @layout.backup_boot],
            ["BPB_Reserved", "a12", ""],
            ["BS_DrvNum", "C", DRIVE_NUMBER],
            ["BS_Reserved1", "C", 0],
            ["BS_BootSig", "C", EXTENDED_BOOT_SIGNATURE],
            ["BS_VolID", "V", @layout.volume_id],
            ["BS_VolLab", "a11", VOLUME_LABEL],
            ["BS_FilSysType", "a8", "FAT32   "]
          ]
        end

        # Writes every FAT copy; bytes no entry sets stay zero holes.
        def write_fats
          fat_runs.each do |offset, bytes|
            @layout.fat_count.times { @out.pwrite(bytes, @layout.fat_offset(_1) + offset) }
          end
        end

        # The FAT's entries as runs of [first cluster, values], lazily, so a
        # long chain costs at most RUN_ENTRIES entries of memory; the sum
        # of a Lazy and the chains' Lazy is itself lazy. FAT[0] holds the
        # media byte with every other bit set and FAT[1] the end-of-chain
        # mark (section 4.2); each chain links its clusters and ends with
        # the mark.
        def entry_runs
          eoc = END_OF_CHAIN.fetch(@layout.entry_bits)
          reserved = [0, [(eoc & ~0xFF) | @layout.media, eoc]]
          chains = @allocation.chains.values.lazy.flat_map do |chain|
            chain.each_slice(RUN_ENTRIES).lazy.map { |slice| [slice.first, slice.map { _1 == chain.last ? eoc : _1 + 1 }] }
          end
          [reserved].lazy + chains
        end

        # The entries as runs of [byte offset, bytes] within one FAT copy,
        # leaving out every entry beyond a FAT too small for it, as a
        # fat_size parameter can make. A FAT12 volume has fewer than
        # FAT16_MINIMUM_CLUSTERS clusters, so its table is packed whole.
        def fat_runs
          return [[0, packed_fat12]] if @layout.entry_bits == 12

          width = @layout.entry_bits / 8
          capacity = @layout.fat_size * @layout.bytes_per_sector / width
          entry_runs.filter_map do |first, values|
            kept = values.first([capacity - first, 0].max)
            [first * width, kept.pack(width == 4 ? "V*" : "v*")] unless kept.empty?
          end
        end

        # The FAT12 table up to its last entry. Two entries share the middle
        # byte of three: an even entry fills its first byte and the low
        # nibble of the second, an odd one the high nibble and the third
        # byte (section 4.1).
        def packed_fat12
          limit = @layout.fat_size * @layout.bytes_per_sector
          entries = entry_runs.flat_map { |first, values| values.each_with_index.map { |value, index| [first + index, value] } }
                              .select { |cluster, _| fat12_pair_end(cluster) <= limit }.to_a
          bytes = "\0".b * (entries.map { |cluster, _| fat12_pair_end(cluster) }.max || 0)
          entries.each do |cluster, value|
            offset = fat12_offset(cluster)
            pair = bytes.byteslice(offset, 2).unpack1("v")
            pair = cluster.even? ? (pair & 0xF000) | value : (pair & 0x000F) | (value << 4)
            bytes[offset, 2] = [pair].pack("v")
          end
          bytes
        end

        # FAT12 entry cluster lies in the two bytes from cluster * 3 / 2, and
        # fits the table when both do.
        def fat12_offset(cluster) = cluster * 3 / 2

        def fat12_pair_end(cluster) = fat12_offset(cluster) + 2

        def write_directories
          @allocation.directories.each do |path, bytes|
            offset = path.nil? && !@layout.fat32? ? @layout.root_offset : @layout.cluster_offset(@allocation.first_cluster(path))
            @out.pwrite(bytes, offset)
          end
        end

        def write_files
          @allocation.files.each do |file|
            @out.seek(@layout.cluster_offset(@allocation.first_cluster(file.path)))
            FileContent.write(@out, file)
          end
        end

        # FSInfo after the boot sector, and unless backup_boot is zero the
        # backup boot record with its own FSInfo copy (sections 3.6 and 5).
        def write_fat32_records(boot)
          info = fs_info_sector
          @out.pwrite(info, @layout.sector_offset(@layout.fs_info))
          return if @layout.backup_boot.zero?

          @out.pwrite(boot, @layout.sector_offset(@layout.backup_boot))
          @out.pwrite(info, @layout.sector_offset(@layout.backup_boot + @layout.fs_info))
        end

        def fs_info_sector
          fields = [
            ["FSI_LeadSig", "V", FSI_LEAD_SIGNATURE],
            ["FSI_Reserved1", "a480", ""],
            ["FSI_StrucSig", "V", FSI_STRUCTURE_SIGNATURE],
            ["FSI_Free_Count", "V", @layout.clusters - @allocation.used_clusters],
            ["FSI_Nxt_Free", "V", @allocation.first_free || NO_FREE_CLUSTER_HINT],
            ["FSI_Reserved2", "a12", ""],
            ["FSI_TrailSig", "V", FSI_TRAIL_SIGNATURE]
          ]
          Record.pack(fields)
        end
      end
    end
  end
end
