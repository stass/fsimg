# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

module CliImages
  private

  GptPartition = Data.define(:first_lba, :last_lba, :type, :name, :payload)
  MbrPartition = Data.define(:first_lba, :sector_count, :type, :payload)

  ESP_TYPE = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
  LINUX_TYPE = "0fc63daf-8483-4772-8e79-3d69d8477de4"
  DISK_GUID = "12345678-9abc-4def-8123-456789abcdef"
  SEGMENT_ID = "0123abcd-4567-4890-8bcd-ef0123456789"
  BASIC_TYPE = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
  BOOT_FIELDS = [
    "signature",
    "version",
    "flags1",
    "flags2",
    "header_size",
    "machine",
    "startup_vaddr",
    "paddr_bias",
    "image_paddr",
    "ram_paddr",
    "ram_size",
    "startup_size",
    "stored_size",
    "imagefs_paddr",
    "imagefs_size",
    "preboot_size"
  ].freeze
  def gpt_disk(sectors:, partitions:)
    # A GPT disk laid out as sgdisk does: protective MBR, primary header at
    # LBA 1 with 128 entries from LBA 2, backup entries and header at the end.
    # Partition values name the bounds, type, name, and volume bytes.
    disk = "\0".b * (sectors * 512)
    disk[446...462] = [
      0,
      "\x00\x02\x00".b,
      0xEE,
      "\xff\xff\xff".b,
      1,
      [(sectors - 1), 0xFFFFFFFF].min
    ].pack("Ca3Ca3L<L<")
    disk[510...512] = "U\xaa".b
    entries = "\0".b * (128 * 128)
    partitions.each_with_index do |partition, index|
      first = partition.first_lba
      last = partition.last_lba
      kind = partition.type
      name = partition.name
      payload = partition.payload
      entries[(index * 128)...((index + 1) * 128)] = (((uuid_bytes(value: kind, little: true) +
        uuid_bytes(value: (index + 1), little: true)) +
        [
          first,
          last,
          0
        ].pack("Q<Q<Q<")) +
        name.encode(Encoding::UTF_16LE).b.ljust(72, "\x00".b))
      disk[(first * 512)...((first * 512) + payload.size)] = payload
    end

    entries_crc = Zlib.crc32(entries)
    header = -> (current:, alternate:, entries_lba:) do
      fields = {
        signature: "EFI PART".b,
        revision: 0x10000,
        header_size: 92,
        header_crc: 0,
        reserved: 0,
        current_lba: current,
        alternate_lba: alternate,
        first_usable_lba: 34,
        last_usable_lba: (sectors - 34),
        disk_guid: uuid_bytes(value: DISK_GUID, little: true),
        entries_lba: entries_lba,
        entry_count: 128,
        entry_size: 128,
        entries_crc: entries_crc
      }.values.pack("a8L<L<L<L<Q<Q<Q<Q<a16Q<L<L<L<")
      (fields[0...16] + [Zlib.crc32(fields)].pack("L<")) + fields[20..]
    end

    disk[512...604] = header.call(current: 1, alternate: (sectors - 1), entries_lba: 2)
    disk[1024...(1024 + entries.size)] = entries
    disk[((sectors - 33) * 512)...((sectors - 1) * 512)] = entries
    disk[((sectors - 1) * 512)...(((sectors - 1) * 512) + 92)] = header.call(
      current: (sectors - 1),
      alternate: 1,
      entries_lba: (sectors - 33)
    )
    disk
  end

  def mbr_disk(sectors:, partitions:)
    # An MBR disk with named partition bounds, type, and volume bytes;
    # and the first is active.
    disk = "\0".b * (sectors * 512)
    disk[440...444] = [0xA1B2C3D4].pack("L<")
    partitions.each_with_index do |partition, index|
      first = partition.first_lba
      count = partition.sector_count
      kind = partition.type
      payload = partition.payload
      disk[(446 + (16 * index))...(462 + (16 * index))] = [
        ((index == 0) ? 0x80 : 0),
        "\x00\x00\x00".b,
        kind,
        "\x00\x00\x00".b,
        first,
        count
      ].pack("Ca3Ca3L<L<")
      disk[(first * 512)...((first * 512) + payload.size)] = payload
    end

    disk[510...512] = "U\xaa".b
    disk
  end

  def dmg_image(disk:, regions:)
    # A DMG as hdiutil lays it out: the data fork, the XML plist, the koly
    # trailer. Each (first, count) region in sectors gets a block table whose
    # 256-sector chunks are zero runs when blank, zlib runs when compression
    # pays and raw runs otherwise; the master checksum covers the tables'.
    fork = String.new(encoding: Encoding::BINARY)
    entries = []
    master = 0
    regions.each_with_index do |(first, count), index|
      runs = []
      crc = 0
      (0...count).step(256).each do |sector|
        sectors = [256, (count - sector)].min
        data = disk[((first + sector) * 512)...(((first + sector) + sectors) * 512)]
        crc = Zlib.crc32(data, crc)
        packed = Zlib::Deflate.deflate(data, 9)
        if data.bytes.all?(&:zero?)
          runs.push([0, sector, sectors, fork.size, 0])
        elsif packed.size < data.size
          runs.push([0x80000005, sector, sectors, fork.size, packed.size])
          fork += packed
        else
          runs.push([1, sector, sectors, fork.size, data.size])
          fork += data
        end
      end

      runs.push([0xFFFFFFFF, count, 0, fork.size, 0])
      mish_fields = {
        signature: "mish".b,
        version: 1,
        first_sector: first,
        sector_count: count,
        data_offset: 0,
        buffers_needed: 0x208,
        descriptor: index,
        reserved: "".b,
        checksum_type: 2,
        checksum_bits: 32,
        checksum: [crc].pack("L>"),
        run_count: runs.size
      }
      mish = mish_fields.values.pack("a4L>Q>Q>Q>L>L>a24L>L>a128L>")
      mish += runs
        .map { |
            (kind, sector, sectors, offset, length)
          |
          [kind, 0, sector, sectors, offset, length].pack("L>L>Q>Q>Q>Q>")
        }
        .join("".b)
      master = Zlib.crc32([crc].pack("L>"), master)
      entries.push(<<~XML)
        \t\t\t<dict>
        \t\t\t\t<key>Attributes</key>
        \t\t\t\t<string>0x0050</string>
        \t\t\t\t<key>Data</key>
        \t\t\t\t<data>#{[mish].pack("m0")}</data>
        \t\t\t\t<key>ID</key>
        \t\t\t\t<string>#{index}</string>
        \t\t\t\t<key>Name</key>
        \t\t\t\t<string>region &amp; more</string>
        \t\t\t</dict>
      XML
    end

    xml = (<<~XML).encode(Encoding::UTF_8).b
      <?xml version="1.0" encoding="UTF-8"?>
      <plist version="1.0">
      <dict>
      \t<key>resource-fork</key>
      \t<dict>
      \t\t<key>blkx</key>
      \t\t<array>
      #{entries.join("")}\t\t</array>
      \t</dict>
      </dict>
      </plist>
    XML
    koly_fields = {
      signature: "koly".b,
      version: 4,
      header_size: 512,
      flags: 1,
      running_data_offset: 0,
      data_fork_offset: 0,
      data_fork_length: fork.size,
      resource_fork_offset: 0,
      resource_fork_length: 0,
      segment_number: 1,
      segment_count: 1,
      segment_id: uuid_bytes(value: SEGMENT_ID, little: false),
      data_checksum_type: 2,
      data_checksum_bits: 32,
      data_checksum: [Zlib.crc32(fork)].pack("L>"),
      xml_offset: fork.size,
      xml_length: xml.size,
      reserved: "".b,
      master_checksum_type: 2,
      master_checksum_bits: 32,
      master_checksum: [master].pack("L>"),
      image_variant: 1,
      sector_count: (disk.size / 512),
      reserved_tail: "".b
    }
    koly = koly_fields.values.pack("a4L>L>L>Q>Q>Q>Q>Q>L>L>a16L>L>a128Q>Q>a120L>L>a128L>Q>a12")
    fork + xml + koly
  end

  def vhd_image(disk:)
    # A fixed VHD: the disk and a footer whose checksum is the one's
    # complement of its byte sum.
    footer = {
      cookie: "conectix".b,
      features: 2,
      format_version: 0x10000,
      data_offset: 0xFFFFFFFFFFFFFFFF,
      timestamp: 0,
      creator_application: "test".b,
      creator_version: 0x10000,
      creator_host_os: "Wi2k".b,
      original_size: disk.size,
      current_size: disk.size,
      cylinders: 120,
      heads: 16,
      sectors_per_track: 63,
      disk_type: 2,
      checksum: 0,
      unique_id: uuid_bytes(value: DISK_GUID, little: false),
      saved_state: 0,
      reserved: "".b
    }.values.pack("a8L>L>Q>L>a4L>a4Q>Q>S>CCL>L>a16Ca427").dup
    footer[64...68] = [(~footer.bytes.sum & 0xFFFFFFFF)].pack("L>")
    disk + footer
  end

  def simg_image(disk:)
    # An Android sparse image of 4 KiB blocks: a DONT_CARE chunk for each
    # blank 64 KiB unit, a RAW chunk otherwise, and a closing CRC32 chunk.
    chunks = []
    (0...disk.size).step(65536).each do |offset|
      unit = disk[offset...(offset + 65536)]
      if unit.bytes.any? { _1 != 0 }
        chunks.push([0xCAC1, 0, (unit.size / 4096), (12 + unit.size)].pack("S<S<L<L<") + unit)
      else
        chunks.push([0xCAC3, 0, (unit.size / 4096), 12].pack("S<S<L<L<"))
      end
    end

    chunks.push([0xCAC4, 0, 0, 16, Zlib.crc32(disk)].pack("S<S<L<L<L<"))
    header = {
      magic: 0xED26FF3A,
      major_version: 1,
      minor_version: 0,
      file_header_size: 28,
      chunk_header_size: 12,
      block_size: 4096,
      total_blocks: (disk.size / 4096),
      total_chunks: chunks.size,
      checksum: 0
    }.values.pack("L<S<S<S<S<L<L<L<L<")
    return (header + chunks.join("".b))
  end

  # A big-endian ImageFS from the published layout, independent of the writer:
  # etc/motd is the boot file, bin/sh is a link with an implied parent, and
  # shadowed directory records lie below that link. A twin shares the boot
  # file's inode; motd can carry a name outside the profile.
  def ifs_image(payload:, shadowed: 0, motd: "motd".b, script: 0, twin: false)
    mountpoint = "/proc/boot\0".b
    dir_offset = (0x58 + mountpoint.bytesize + 3) / 4 * 4
    table_arguments = {payload_size: payload.bytesize, shadowed: shadowed, motd: motd, twin: twin}
    # The directory's length does not depend on the encoded data offset.
    data_offset = dir_offset + ifs_table(**table_arguments, data_offset: 0).bytesize
    table = ifs_table(**table_arguments, data_offset: data_offset)
    image_size = (data_offset + payload.bytesize + 3) / 4 * 4 + 4
    header = {
      signature_and_flags: "imagefs\x05".b,
      image_size: image_size,
      header_directory_size: data_offset,
      directory_offset: dir_offset,
      boot_inode_0: 2,
      other_boot_inodes: "\0".b * 12,
      script_inode: script,
      chain_address: "\0".b * 4,
      spare: "\0".b * 40,
      mount_flags: 3
    }.values.pack("a8L>L>L>L>a12L>a4a40L>")
    assert_equal(0x58, header.bytesize)
    image = ((header + mountpoint).ljust(dir_offset, "\0".b) + table + payload)
      .ljust(image_size - 4, "\0".b)
    # The trailer makes the big-endian word sum of the entire image zero.
    checksum = -image.unpack("L>*").sum & 0xffffffff
    image + [checksum].pack("L>")
  end

  def ifs_table(payload_size:, data_offset:, shadowed:, motd:, twin:)
    target = "/proc/boot/ksh".b
    records = [
      ifs_record(mode: 0o40755, inode: 1, path: "etc".b),
      ifs_record(
        mode: 0o100640,
        inode: 2,
        path: "etc/".b + motd,
        tail: [data_offset, payload_size].pack("L>L>")
      ),
      ifs_record(
        mode: 0o120777,
        inode: 3,
        path: "bin/sh".b,
        tail: [7, target.bytesize].pack("S>S>"),
        extra: target + "\0".b
      )
    ]
    hidden = (0...shadowed).map do |ordinal|
      inode = twin && ordinal.zero? ? 2 : 4 + ordinal
      ifs_record(mode: 0o40755, inode: inode, path: format("bin/sh/x%d", ordinal).b)
    end

    (records + hidden).join.b + "\0".b * 4
  end

  def ifs_record(mode:, inode:, path:, tail: "".b, extra: "".b)
    body = tail + path + "\0".b + extra
    length = (0x18 + body.bytesize + 3) / 4 * 4
    prefix = [length, 0, inode, mode, 7, 9, 86400].pack("S>S>L>L>L>L>L>")
    (prefix + body).ljust(length, "\0".b)
  end

  def boot_header(image)
    # The QNX startup header at offset 0, decoded from the profile's table.
    return BOOT_FIELDS.zip(image[0..].unpack("L<S<CCS<S<L<L<L<L<L<L<L<L<L<S<")).to_h
  end

  def word_sum(data)
    raise CliTest::Error, "word checksum needs complete 32-bit words" unless (data.bytesize % 4).zero?
    return (data.unpack("L<*").sum & 0xffffffff)
  end
  # A gzip byte stream can contain several members and zero padding. Reading
  # just its first member would let extra emitted payload escape the oracle.
  def gunzip(bytes)
    output = String.new(encoding: Encoding::BINARY)
    remaining = bytes
    until remaining.empty?
      stream = StringIO.new(remaining)
      Zlib::GzipReader.wrap(stream) do |reader|
        output << reader.read.b
        remaining = (reader.unused || "".b) + (stream.read || "".b)
      end

      remaining = remaining.sub(/\A\0+/n, "".b)
    end

    output
  rescue Zlib::Error
    raise CliTest::Error, "invalid gzip fixture stream"
  end

  def gzip_bytes(bytes, mtime: 0)
    stream = StringIO.new("".b)
    writer = Zlib::GzipWriter.new(stream)
    writer.mtime = mtime
    writer.write(bytes)
    writer.close
    stream.string
  end

  def uuid_bytes(value:, little: false)
    text = value.is_a?(Integer) ? format("%032x", value) : value.delete("-")
    raw = [text].pack("H*")
    little ? raw[0, 4].reverse + raw[4, 2].reverse + raw[6, 2].reverse + raw[8..] : raw
  end

  def partition_fixtures
    image_fixture(name: "part.img", format: "fat12", size: "1M")
    volume = (@root / "part.img").binread
    disk = gpt_disk(
      sectors: 8192,
      partitions: [
        GptPartition.new(first_lba: 2048, last_lba: 4095, type: ESP_TYPE, name: "EFI", payload: volume),
        GptPartition.new(first_lba: 4096, last_lba: 6143, type: LINUX_TYPE, name: "data", payload: "".b)
      ]
    )
    (@root / "gpt.img").binwrite(disk)
  end

end
