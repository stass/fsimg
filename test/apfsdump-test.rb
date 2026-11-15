# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "digest"
require "minitest/autorun"
require "stringio"
require_relative "../tools/apfs/apfsdump"

class ApfsDumpTest < Minitest::Test
  include ApfsDump

  BLOCK = 4096
  SEALED_VOLUME_FEATURE = 0x20
  NODE_HASHED_FLAG = 0x8
  NODE_NOHEADER_FLAG = 0x10
  OMAP_NOHEADER_FLAG = 0x8
  SEALED_TREE_FLAGS = 0x1C2
  FS_ROOT_OID = 1028
  ROOT_HASH_OFFSET = 128
  FREE_SPACE_OFFSET = BLOCK / 2
  SEALED_LEAF_BLOCK = 18
  SEALED_LEAF_OID = 1029
  INTEGRITY_META_OID = 1030
  INTEGRITY_META_BLOCK = 19

  # An in-memory image with the two calls Image makes of a file.
  BytesIO = Data.define(:bytes) do
    def size = bytes.bytesize
    def pread(len, off) = bytes.byteslice(off, len)
  end

  # The checkpoint-mapping block at block 1 of a 64 MiB container formatted
  # by newfs_apfs on macOS, trailing zeros left out. Its checksum was written
  # by Apple's implementation, so it is a vector independent of this one.
  NATIVE_MAP_BLOCK = [
    "96b2613f2d259e80010000000000000001000000000000000c00004000000000" \
    "0100000002000000050000800000000000100000000000000000000000000000" \
    "0004000000000000090000000000000011000080000000000010000000000000" \
    "000000000000000001040000000000000a"
  ].pack("H*").ljust(BLOCK, "\0")

  def test_fletcher64_of_a_zero_object_is_all_ones
    assert_equal 0xFFFF_FFFF_FFFF_FFFF, Fletcher64.compute("\0".b * BLOCK)
  end

  def test_fletcher64_reproduces_a_native_checksum
    obj = Obj.from(1, NATIVE_MAP_BLOCK)

    assert_equal [0x809E_252D_3F61_B296, true], [obj.computed, obj.cksum_ok?]
  end

  def test_a_changed_byte_breaks_the_checksum
    damaged = NATIVE_MAP_BLOCK.dup.tap { _1.setbyte(100, 0xFF) }

    refute_predicate Obj.from(1, damaged), :cksum_ok?
  end

  def test_native_map_block_decodes_two_mappings
    head = Layout.decode(NATIVE_MAP_BLOCK, CHECKPOINT_MAP_FIELDS)
    second = Layout.decode(NATIVE_MAP_BLOCK, CHECKPOINT_MAPPING_FIELDS, base: 40 + CHECKPOINT_MAPPING_SIZE)

    assert_equal({ flags: 1, count: 2 }, head)
    assert_equal [0x8000_0011, 1025, 10], second.values_at(:type, :oid, :paddr)
  end

  def test_superblock_fields_decode_at_their_reference_offsets
    sb = Layout.decode(superblock_block(block_count: 300, fs_oid: [1026, 1027]), NX_FIELDS)

    assert_equal [NX_MAGIC, BLOCK, 300], sb.values_at(:magic, :block_size, :block_count)
    assert_equal "00112233-4455-6677-8899-aabbccddeeff", sb[:uuid]
    assert_equal [1026, 1027] + [0] * 98, sb[:fs_oid]
    assert_equal({ start_paddr: 7, block_count: 2 }, sb[:keylocker])
    assert_equal 0xABCD, sb[:newest_mounted_version]
  end

  def test_a_field_past_the_end_of_its_bytes_is_an_error
    assert_raises(Error) { Layout.decode("\0".b * 1000, NX_FIELDS) }
  end

  def test_omap_leaf_records_are_decoded_from_a_root_node
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), [0, BLOCK, 50].pack("L<L<q<")],
                                                  [[1027, 3].pack("Q<Q<"), [1, BLOCK, 51].pack("L<L<q<")]])
    tree, problems = walk({ 9 => node })

    assert_empty problems
    assert_equal [[1026, 2, 50, ""], [1027, 3, 51, "DELETED"]], tree[:records].map { _1.values_at(:oid, :xid, :paddr, :flags_names) }
  end

  def test_a_key_count_beyond_the_table_of_contents_is_reported_and_bounded
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), [0, BLOCK, 50].pack("L<L<q<")]],
                      nkeys: 0xFFFF_FFFF)
    tree, problems = walk({ 9 => node })

    assert_match(/4294967295 keys, the table of contents holds #{TOC_ENTRIES}/, problems.first)
    assert_equal 1, tree[:records].size
  end

  def test_omap_snapshot_codec_decodes_flags_and_reserved_oid
    record = OMAP_SNAPSHOT_CODEC.record.call([4].pack("Q<"), [3, 0, 99].pack("L<L<Q<"))

    assert_equal({ xid: 4, flags: 3, flags_names: "DELETED,REVERTED", oid: 99 }, record)
  end

  def test_omap_snapshot_codec_keeps_ghost_flags_absent
    record = OMAP_SNAPSHOT_CODEC.record.call([4].pack("Q<"), nil)

    assert_equal({ xid: 4, flags: nil, flags_names: nil, oid: nil }, record)
  end

  def test_a_ghost_in_a_tree_without_ghosts_is_reported_and_dropped
    node = node_block(oid: 9, level: 0, entries: [[[1026, 2].pack("Q<Q<"), nil]])
    tree, problems = walk({ 9 => node })

    assert_equal [], tree[:records]
    assert(problems.any? { _1.include?("ghost record in a tree that does not allow ghosts") })
  end

  def test_an_index_node_naming_itself_stops_the_walk
    node = node_block(oid: 9, level: 1, entries: [[[1, 0].pack("Q<Q<"), [9].pack("Q<")]])
    tree, problems = walk({ 9 => node })

    assert_equal 1, tree[:nodes].size
    assert(problems.any? { _1.include?("reached twice") })
  end

  def test_a_headerless_hashed_tree_decodes_every_leaf_record
    nodes = hashed_tree_nodes

    tree, problems = walk_hashed(nodes, digest: Digest::SHA256.digest(nodes.fetch(100)))

    assert_empty problems
    assert_equal 3, tree[:nodes].size
    assert_equal [fs_record(INODE_RECORD), fs_record(DREC_RECORD)], tree[:records]
  end

  def test_a_hashed_child_oid_past_64_bits_is_reported
    root_oid = 100
    child_oid = 99
    child = hashed_node_block(level: 0, entries: [INODE_RECORD], root: false)
    relative_oid = 2**64 - 1
    entries = [[INODE_RECORD.first, [relative_oid].pack("Q<") + Digest::SHA256.digest(child)]]
    root = hashed_node_block(level: 1, entries:, root: true)
    nodes = { root_oid => root, child_oid => child }
    fetched = []
    fetch = lambda do |oid|
      fetched << oid
      bytes = nodes.fetch(oid) { raise Error, "unexpected virtual OID #{oid}" }
      Obj.from(oid, bytes, headerless: true)
    end

    tree, problems = walk_hashed(nodes, digest: Digest::SHA256.digest(root), fetch:)

    assert_equal 1, problems.size
    assert_match(/overflows 64 bits/, problems.first)
    assert_equal [root_oid], fetched
    assert_empty tree[:records]
  end

  def test_a_headerless_node_in_a_tree_without_hashes_is_reported
    node = fs_node_block(oid: 0, records: [INODE_RECORD], headerless: true,
                         node_flags: NODE_ROOT | NODE_LEAF | NODE_NOHEADER_FLAG)

    _tree, problems = walk_hashed({ 100 => node }, digest: nil)

    assert_equal ["tree: node 100: mapped without a header"], problems
  end

  def test_a_headerless_hashed_node_with_nonzero_header_bytes_is_reported
    node = hashed_node_block(level: 0, entries: [INODE_RECORD], root: true)
    node.setbyte(31, 1)

    _tree, problems = walk_hashed({ 100 => node }, digest: Digest::SHA256.digest(node))

    assert_equal ["tree: node 100: mapped without a header, but its first 32 bytes are not zero"], problems
  end

  def test_a_node_hashed_flag_that_disagrees_with_the_tree_is_reported
    node = fs_node_block(oid: 0, records: [INODE_RECORD], headerless: true,
                         tree_flags: SEALED_TREE_FLAGS,
                         node_flags: NODE_ROOT | NODE_LEAF | NODE_NOHEADER_FLAG)

    _tree, problems = walk_hashed({ 100 => node }, digest: Digest::SHA256.digest(node))

    assert_equal ["tree: node 100: HASHED flag disagrees with the tree"], problems
  end

  def test_a_node_noheader_flag_that_disagrees_with_its_mapping_is_reported
    node = fs_node_block(oid: 0, records: [INODE_RECORD], headerless: true,
                         tree_flags: SEALED_TREE_FLAGS,
                         node_flags: NODE_ROOT | NODE_LEAF | NODE_HASHED_FLAG)

    _tree, problems = walk_hashed({ 100 => node }, digest: Digest::SHA256.digest(node))

    assert_equal ["tree: node 100: NOHEADER flag disagrees with its mapping"], problems
  end

  def test_a_hashed_index_value_must_have_exactly_40_bytes
    [8, 39, 41].each do |size|
      entries = [[INODE_RECORD.first, "\0".b * size]]
      node = hashed_node_block(level: 1, entries:, root: true)

      tree, problems = walk_hashed({ 100 => node }, digest: Digest::SHA256.digest(node))

      assert_equal ["tree: node 100: hashed index value of #{size} bytes, expected 40"], problems
      assert_equal [], tree[:records]
    end
  end

  def test_a_bad_leaf_digest_names_the_leaf_and_preserves_the_other_leaf_records
    nodes = hashed_tree_nodes
    root_digest = Digest::SHA256.digest(nodes.fetch(100))
    stored_digest = Digest::SHA256.hexdigest(nodes.fetch(101))
    damaged = flip_free_space(nodes.fetch(101))
    nodes = nodes.merge(101 => damaged)

    tree, problems = walk_hashed(nodes, digest: root_digest)

    assert_equal ["tree: node 101: SHA-256 #{Digest::SHA256.hexdigest(damaged)} does not match the stored digest #{stored_digest}"],
                 problems
    assert_includes tree[:records], fs_record(DREC_RECORD)
  end

  def test_a_wrong_root_digest_names_the_root_oid
    nodes = hashed_tree_nodes
    wrong_digest = Digest::SHA256.digest("wrong root")

    _tree, problems = walk_hashed(nodes, digest: wrong_digest)

    assert_equal ["tree: node 100: SHA-256 #{Digest::SHA256.hexdigest(nodes.fetch(100))} does not match the stored digest #{wrong_digest.unpack1('H*')}"],
                 problems
  end

  def test_a_hashed_tree_without_a_root_digest_reports_the_missing_hash
    _tree, problems = walk_hashed(hashed_tree_nodes, digest: nil)

    assert_equal ["tree: root 100: hashed tree has no root hash to check"], problems
  end

  def test_a_damaged_container_is_reported_section_by_section
    image = superblock_block(block_count: 16, omap_oid: 5).then { seal(_1) } + ("\0".b * (BLOCK * 15))
    container = Container.new(Image.new(BytesIO.new(bytes: image)))
    doc = container.dump

    assert_equal %i[image superblock checkpoints omap volumes spaceman reaper problems], doc.keys
    assert_includes container.problems, "checkpoints: no valid superblock in the descriptor area"
    assert(container.problems.any? { _1.start_with?("container object map at block 5: checksum") })
    assert(container.problems.any? { _1.include?("ephemeral oid 1024 has no mapping") })
  end

  def test_a_whole_container_dumps_without_problems
    container = Container.new(Image.new(BytesIO.new(bytes: container_image)))
    doc = container.dump

    assert_empty container.problems
    assert_equal [1, true], [doc[:checkpoints][:selected], doc[:checkpoints][:descriptor_area][1][:latest]]
    assert_equal [1026, 12, APFS_MAGIC, "TEST"], doc[:volumes].first.values_at(:oid, :addr, :magic, :volname)
    assert_equal [39, 39], doc[:spaceman][:devices].first.values_at(:free_count, :bitmap_free)
    assert_equal({ used: 1, free: 1 }, doc[:spaceman][:internal_pool].slice(:used, :free))
    assert_equal 0x1, doc[:reaper][:flags]
  end

  def test_current_spaceman_dump_keeps_version_and_structure_size
    container = Container.new(Image.new(BytesIO.new(bytes: container_image)))
    spaceman = container.dump.fetch(:spaceman)

    assert_equal 2520, spaceman[:structure_end]
    assert_equal 1, spaceman.fetch(:version)
    assert_equal 2520, spaceman.fetch(:struct_size)
  end

  def test_older_spaceman_dump_omits_current_structure_fields
    bytes = container_image(older_layout: true)
    container = Container.new(Image.new(BytesIO.new(bytes:)))
    spaceman = container.dump.fetch(:spaceman)

    assert_equal 336, spaceman[:structure_end]
    %i[version struct_size datazone].each { refute spaceman.key?(_1), _1.to_s }
    assert_empty container.problems
  end

  # A crafted chain that names one block many times reads it once.
  def test_a_repeated_chunk_information_block_is_read_once
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(cibs: [20, 20]))))
    device = container.dump[:spaceman][:devices].first

    assert_equal [20, nil], device[:cibs].map { _1[:oid] }
    assert(container.problems.any? { _1.include?("block 20 is already read for another structure") })
  end

  def test_an_oversized_ephemeral_mapping_is_not_read
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(spaceman_size: BLOCK * 1000))))
    doc = container.dump

    assert(container.problems.any? { _1.include?("ephemeral oid 1024: size 4096000 is not 1 to 256 whole blocks") })
    assert_includes doc[:spaceman][:error], "ephemeral oid 1024 has no mapping"
  end

  def test_an_image_without_a_container_superblock_is_an_error
    image = Image.new(BytesIO.new(bytes: "\0".b * (BLOCK * 2)))

    assert_raises(Error) { Container.new(image).dump }
  end

  def test_a_volume_resolves_its_file_system_tree_through_its_object_map
    container = Container.new(Image.new(BytesIO.new(bytes: container_image)), records: true)
    volume = container.dump[:volumes].first

    assert_empty container.problems
    assert_equal ["CASE_INSENSITIVE", "hashed", 15], [volume[:incompatible_features_names], volume[:drec_keys], volume[:omap][:tree][:records].first[:paddr]]
    assert_equal %w[INODE XATTR DIR_REC FILE_EXTENT], volume[:fs_tree][:records].map { _1[:type_name] }
    assert_equal({ "DIR_REC" => 1, "FEXT" => 1, "FILE_EXTENT" => 1, "INODE" => 1, "XATTR" => 1 }, volume[:summary][:by_type])
    assert_equal [70_000, 73_728, 4096], volume[:summary].values_at(:dstream_size, :file_extent_bytes, :fext_bytes)
  end

  def test_a_sealed_volume_checks_headerless_nodes_and_reports_its_root_hash
    bytes = container_image(sealed: true)
    root = bytes.byteslice(15 * BLOCK, BLOCK)
    container = Container.new(Image.new(BytesIO.new(bytes:)), records: true)

    volume = container.dump[:volumes].first

    assert_empty container.problems
    assert_equal [2, 3], volume[:fs_tree].values_at(:node_count, :record_count)
    assert_equal %w[INODE XATTR DIR_REC], volume[:fs_tree][:records].map { _1[:type_name] }
    assert_equal Digest::SHA256.hexdigest(root), volume[:integrity_meta][:root_hash]
  end

  def test_a_sealed_volume_reports_a_leaf_changed_in_free_space
    bytes = container_image(sealed: true)
    leaf = bytes.byteslice(SEALED_LEAF_BLOCK * BLOCK, BLOCK)
    damaged = flip_free_space(leaf)
    put(bytes, SEALED_LEAF_BLOCK * BLOCK, damaged)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 file-system tree: node #{SEALED_LEAF_OID}: SHA-256 #{Digest::SHA256.hexdigest(damaged)} does not match the stored digest #{Digest::SHA256.hexdigest(leaf)}"],
                 container.problems
  end

  def test_a_sealed_file_extent_with_nonzero_flags_is_reported
    bytes = container_image(sealed: true, fext_flags: 1)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 file extent tree: extent 40/0: flags 0x1, expected 0"], container.problems
  end

  def test_a_sealed_file_extent_with_a_partial_block_length_is_reported
    bytes = container_image(sealed: true, fext_length: BLOCK + 1)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 file extent tree: extent 40/0: length 4097 is not a positive multiple of 4096"], container.problems
  end

  def test_a_sealed_file_extent_with_zero_length_is_reported
    bytes = container_image(sealed: true, fext_length: 0)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 file extent tree: extent 40/0: length 0 is not a positive multiple of 4096"], container.problems
  end

  def test_integrity_metadata_mapped_without_a_header_is_reported
    bytes = container_image(sealed: true, integrity_mapping_flags: OMAP_NOHEADER_FLAG)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 integrity metadata: mapped without a header"], container.problems
  end

  def test_an_unsupported_integrity_metadata_version_is_reported
    bytes = container_image(sealed: true, integrity_meta: { version: 1 })
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 integrity metadata: version 1, expected 2"], container.problems
  end

  def test_an_integrity_root_hash_must_fit_past_the_fixed_fields_and_inside_the_block
    [ROOT_HASH_OFFSET - 1, BLOCK - 31].each do |offset|
      bytes = container_image(sealed: true, integrity_meta: { root_hash_offset: offset })
      container = Container.new(Image.new(BytesIO.new(bytes:)))

      container.dump

      assert_equal ["volume 0 integrity metadata: root hash at #{offset}+32 is outside 128...4096",
                    "volume 0 file-system tree: root #{FS_ROOT_OID}: hashed tree has no root hash to check"],
                   container.problems
    end
  end

  def test_an_integrity_hash_type_other_than_sha256_is_reported
    bytes = container_image(sealed: true, integrity_meta: { hash_type: 2 })
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 integrity metadata: hash type 2 is not checked; only SHA-256 is",
                  "volume 0 file-system tree: root #{FS_ROOT_OID}: hashed tree has no root hash to check"],
                 container.problems
  end

  def test_either_a_broken_seal_flag_or_a_broken_xid_reports_the_seal
    [{ flags: 1, broken_xid: 0 }, { flags: 0, broken_xid: 7 }].each do |metadata|
      bytes = container_image(sealed: true, integrity_meta: metadata)
      container = Container.new(Image.new(BytesIO.new(bytes:)))

      container.dump

      assert_equal ["volume 0 integrity metadata: seal broken at xid #{metadata.fetch(:broken_xid)}"],
                   container.problems, metadata.inspect
    end
  end

  def test_a_sealed_volume_without_integrity_metadata_is_reported
    bytes = container_image(sealed: true, integrity_meta_oid: 0)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 sealed volume: no integrity metadata",
                  "volume 0 file-system tree: root #{FS_ROOT_OID}: hashed tree has no root hash to check"],
                 container.problems
  end

  def test_a_sealed_volume_without_a_file_extent_tree_is_reported
    bytes = container_image(sealed: true, fext_oid: 0)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    container.dump

    assert_equal ["volume 0 sealed volume: no file extent tree",
                  "volume 0 decmpfs stream 40 of inode 24 has no extent at offset 0"], container.problems
  end

  def test_file_extent_keys_must_increase_by_id_then_offset_without_duplicates
    [[41, 0], [40, BLOCK], [40, 0]].each do |previous|
      records = [previous, [40, 0]].map do |id, offset|
        [[id, offset].pack("Q<Q<"), [BLOCK, 16].pack("Q<Q<")]
      end
      bytes = container_image(sealed: true, fext_records: records)
      container = Container.new(Image.new(BytesIO.new(bytes:)))

      container.dump

      assert_equal ["volume 0 file extent tree: extent 40/0: key does not follow #{previous.join('/')}"],
                   container.problems
    end
  end

  # The decmpfs attribute of file 24 names data stream 40, whose extent at
  # offset 0 the volume's file extent tree places at block 16.
  def test_a_decmpfs_header_is_read_from_its_data_stream
    volume = Container.new(Image.new(BytesIO.new(bytes: container_image))).dump[:volumes].first

    assert_equal [{ obj_id: 24, stream_oid: 40, magic: DECMPFS_MAGIC, compression_type: 7, uncompressed_size: 20_000,
                    magic_ok: true }], volume[:compressed]
  end

  def test_a_decmpfs_stream_without_a_first_extent_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(fext_oid: 0))))
    volume = container.dump[:volumes].first

    assert_includes container.problems, "volume 0 decmpfs stream 40 of inode 24 has no extent at offset 0"
    assert_equal [24, 40], volume[:compressed].first.values_at(:obj_id, :stream_oid)
  end

  # A crafted volume whose file extent tree root is its file-system tree's
  # node walks that block once.
  def test_a_block_named_by_two_trees_is_read_once
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(fext_oid: 15))))
    container.dump

    assert(container.problems.any? { _1.include?("block 15 is already read for another structure") })
  end

  def test_two_slots_naming_one_volume_read_it_once
    container = Container.new(Image.new(BytesIO.new(bytes: container_image(fs_oid: [1026, 1026]))))
    volumes = container.dump[:volumes]

    assert_equal [true, false], volumes.map { _1.key?(:fs_tree) }
    assert(container.problems.any? { _1.include?("block 12 is already read for another structure") })
  end

  def test_without_records_a_volume_tree_shows_counts_only
    volume = Container.new(Image.new(BytesIO.new(bytes: container_image))).dump[:volumes].first

    assert_equal [1028, 1, 4], volume[:fs_tree].values_at(:root_oid, :node_count, :record_count)
    refute_includes volume[:fs_tree].keys, :records
  end

  def test_snapshots_dump_their_superblock_headers_and_counts
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image)), records: true)

    volume = container.dump[:volumes].first

    assert_empty container.problems
    assert_equal [4, 8], volume[:omap][:snapshot_tree][:records].map { _1[:xid] }
    assert_equal [{ addr: 26, oid: 26, xid: 4, magic: APFS_MAGIC, cksum_ok: true, num_snapshots: 1, num_files: 11 },
                  { addr: 27, oid: 27, xid: 8, magic: APFS_MAGIC, cksum_ok: true, num_snapshots: 2, num_files: 22 }],
                 volume[:snapshots].map { _1[:superblock].slice(:addr, :oid, :xid, :magic, :cksum_ok, :num_snapshots, :num_files) }
  end

  # Historical roots at XIDs 2 and 6 precede snapshots 4 and 8; the live
  # root at XID 9 must not replace either historical mapping.
  def test_snapshot_roots_are_resolved_at_the_snapshot_xid
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image)), records: true)

    volume = container.dump[:volumes].first

    assert_equal({ xid: 9, paddr: 15 }, volume[:omap][:tree][:records].last.slice(:xid, :paddr))
    assert_equal [{ oid: 1028, xid: 2, paddr: 30 }, { oid: 1028, xid: 6, paddr: 31 }],
                 volume[:snapshots].map { _1[:root_tree].slice(:oid, :xid, :paddr) }
  end

  def test_snapshots_dump_their_metadata_fields
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image)), records: true)

    snapshots = container.dump[:volumes].first[:snapshots]

    assert_equal [{ xid: 4, name: "older", create_time: 100, change_time: 200, inum: 2,
                    snap_flags: 0, extentref_tree_oid: 28, extentref_tree_type: 0x4000_0002 },
                  { xid: 8, name: "newer", create_time: 100, change_time: 200, inum: 2,
                    snap_flags: 0, extentref_tree_oid: 29, extentref_tree_type: 0x4000_0002 }],
                 snapshots.map { _1.slice(:xid, :name, :create_time, :change_time, :inum, :snap_flags,
                                         :extentref_tree_oid, :extentref_tree_type) }
  end

  def test_snapshots_dump_their_extent_reference_records
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image)), records: true)

    snapshots = container.dump[:volumes].first[:snapshots]

    assert_equal [[{ obj_id: 41, type_name: "EXTENT", length: 2 }],
                  [{ obj_id: 42, type_name: "EXTENT", length: 3 }]],
                 snapshots.map { |snap| snap[:extentref_tree][:records].map { _1.slice(:obj_id, :type_name, :length) } }
  end

  def test_a_snapshot_superblock_with_no_root_has_no_root_mapping
    bytes = snapshot_container_image(older_root_oid: 0)
    container = Container.new(Image.new(BytesIO.new(bytes:)), records: true)

    snapshot = container.dump[:volumes].first[:snapshots].first

    assert_equal 0, snapshot[:superblock][:root_tree_oid]
    assert_nil snapshot[:root_tree]
    assert_empty container.problems
  end

  def test_snapshot_extent_records_stay_out_of_the_live_summary
    volume = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image)), records: true).dump[:volumes].first

    assert_equal({ "DIR_REC" => 1, "EXTENT" => 1, "FEXT" => 1, "FILE_EXTENT" => 1,
                   "INODE" => 1, "SNAP_METADATA" => 2, "SNAP_NAME" => 2, "XATTR" => 1 }, volume[:summary][:by_type])
    assert_equal [24], volume[:compressed].map { _1[:obj_id] }
  end

  def test_without_records_snapshots_are_flat_scalar_hashes
    volume = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image))).dump[:volumes].first

    assert_equal [{ xid: 4, name: "older", superblock_xid: 4, superblock_cksum_ok: true,
                    root_tree_oid: 1028, root_tree_paddr: 30, extentref_records: 1 },
                  { xid: 8, name: "newer", superblock_xid: 8, superblock_cksum_ok: true,
                    root_tree_oid: 1028, root_tree_paddr: 31, extentref_records: 1 }],
                 volume[:snapshots].map { _1.slice(:xid, :name, :superblock_xid, :superblock_cksum_ok,
                                                 :root_tree_oid, :root_tree_paddr, :extentref_records) }
    volume[:snapshots].each do |snapshot|
      snapshot.each do |key, value|
        refute value.is_a?(Hash) || value.is_a?(Array), "snapshot #{snapshot[:xid]} field #{key} must be scalar"
      end
    end
  end

  def test_text_prints_each_snapshot_on_one_line
    volume = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image))).dump[:volumes].first
    out = StringIO.new

    Text.render({ snapshots: volume[:snapshots] }, out)

    assert_equal 2, out.string.lines.size
    assert_match(/\Asnapshots\[0\]: xid=4 name=older .*superblock_xid=4 .*root_tree_paddr=30 .*extentref_records=1\n/, out.string)
    assert_match(/\nsnapshots\[1\]: xid=8 name=newer .*superblock_xid=8 .*root_tree_paddr=31 .*extentref_records=1\n\z/, out.string)
  end

  def test_an_object_map_snapshot_without_metadata_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(snapshot_xids: [4, 8, 9]))))

    container.dump

    assert_equal ["volume 0 snapshots: 1 object-map snapshot records, first 9, have no metadata record"], container.problems
  end

  def test_an_object_map_snapshot_count_disagreement_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(snap_count: 3))))

    container.dump

    assert_equal ["volume 0 snapshots: object map counts 3, snapshot records 2"], container.problems
  end

  def test_snapshot_metadata_without_an_object_map_snapshot_record_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(snapshot_xids: [4]))))

    container.dump

    assert_equal ["volume 0 snapshots: 1 XIDs, first 8, have no object-map snapshot record"], container.problems
  end

  def test_an_object_map_most_recent_snapshot_disagreement_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(most_recent_snap: 7))))

    container.dump

    assert_equal ["volume 0 snapshots: object map's most recent snapshot 7, newest record 8"], container.problems
  end

  def test_a_superblock_snapshot_count_disagreement_is_reported
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(num_snapshots: 3))))

    container.dump

    assert_equal ["volume 0 snapshots: superblock counts 3, metadata records 2"], container.problems
  end

  # An unreadable superblock must not hide the same snapshot's extent tree.
  def test_a_snapshot_superblock_outside_the_container_preserves_its_extent_tree
    container = Container.new(Image.new(BytesIO.new(bytes: snapshot_container_image(older_sblock: 64))), records: true)

    snapshot = container.dump[:volumes].first[:snapshots].first

    assert_equal ["volume 0 snapshot 4 superblock: blocks 64+1 are outside the container (64 blocks)"],
                 container.problems
    assert_equal [41], snapshot[:extentref_tree][:records].map { _1[:obj_id] }
  end

  def test_snapshot_metadata_in_the_live_file_system_tree_is_not_a_snapshot
    live_metadata = snap_meta_record(xid: 12, name: "live", sblock: 64, extentref: 0)
    bytes = snapshot_container_image(extra_live_records: [live_metadata])
    container = Container.new(Image.new(BytesIO.new(bytes:)), records: true)

    volume = container.dump[:volumes].first

    assert_includes volume[:fs_tree][:records].map { _1.slice(:type_name, :obj_id) },
                    { type_name: "SNAP_METADATA", obj_id: 12 }
    assert_equal [4, 8], volume[:snapshots].map { _1[:xid] }
    assert_empty container.problems
  end

  def test_an_unreadable_object_map_snapshot_root_skips_dependent_cross_checks
    # Block 14 already belongs to the object map; block 64 is outside the image.
    { 14 => "block 14 is already read for another structure",
      64 => "blocks 64+1 are outside the container (64 blocks)" }.each do |oid, failure|
      bytes = snapshot_container_image(snapshot_tree_oid: oid)
      container = Container.new(Image.new(BytesIO.new(bytes:)))

      volume = container.dump[:volumes].first

      assert_equal [4, 8], volume[:snapshots].map { _1[:xid] }
      assert_equal ["volume 0 object map snapshot tree: node #{oid}: volume 0 object map snapshot tree: #{failure}"],
                   container.problems, "root OID #{oid}"
    end
  end

  def test_an_unreadable_snapshot_metadata_root_skips_dependent_cross_checks
    { 14 => "block 14 is already read for another structure",
      64 => "blocks 64+1 are outside the container (64 blocks)" }.each do |oid, failure|
      bytes = snapshot_container_image(snap_meta_oid: oid)
      container = Container.new(Image.new(BytesIO.new(bytes:)))

      volume = container.dump[:volumes].first

      assert_empty volume[:snapshots]
      assert_equal ["volume 0 snapshot metadata tree: node #{oid}: volume 0 snapshot metadata tree: #{failure}"],
                   container.problems, "root OID #{oid}"
    end
  end

  def test_a_short_snapshot_metadata_value_yields_an_error_without_reading_its_trees
    # Neither named tree can be read: the superblock is outside the image
    # and block 14 already belongs to the object map.
    key, value = snap_meta_record(xid: 4, name: "older", sblock: 64, extentref: 14)
    bytes = snapshot_container_image(older_metadata: [key, value.byteslice(0, 16)])
    container = Container.new(Image.new(BytesIO.new(bytes:)), records: true)

    snapshots = container.dump[:volumes].first[:snapshots]

    assert_equal({ xid: 4, error: "metadata record unreadable: structure ends before byte 16" }, snapshots.first)
    assert_equal [8], snapshots.drop(1).map { _1[:xid] }
    assert_equal ["volume 0 snapshot metadata tree: SNAP_METADATA record of object 4: structure ends before byte 16"],
                 container.problems
  end

  def test_without_records_an_unreadable_snapshot_superblock_has_a_scalar_error
    bytes = snapshot_container_image(older_sblock: 64)
    container = Container.new(Image.new(BytesIO.new(bytes:)))

    snapshot = container.dump[:volumes].first[:snapshots].first

    error = "volume 0 snapshot 4 superblock: blocks 64+1 are outside the container (64 blocks)"
    assert_equal error, snapshot[:superblock_error]
    assert_equal [error], container.problems
    refute_includes snapshot.keys, :superblock
  end

  def test_volume_selects_one_slot_and_reports_a_missing_one
    container = Container.new(Image.new(BytesIO.new(bytes: container_image)), volume: 5)

    assert_equal [], container.dump[:volumes]
    assert_includes container.problems, "volumes: no volume in slot 5"
  end

  def test_an_inode_decodes_its_fields_and_extended_fields
    record = fs_record(INODE_RECORD)

    assert_equal [24, "INODE", 20, "REG", 0o100644], record.values_at(:obj_id, :type_name, :parent_id, :file_type, :mode)
    assert_equal "WAS_CLONED,NO_RSRC_FORK", record[:internal_flags_names]
    assert_equal [[4, :name, "f70000"], [8, :dstream, 70_000]],
                 record[:xfields].map { [_1[:type], _1[:name], _1[:value].is_a?(Hash) ? _1[:value][:size] : _1[:value]] }
    assert_equal [2, 48], record.values_at(:xf_num_exts, :xf_used_data)
    refute_includes record.keys, :problems
  end

  def test_a_hashed_directory_record_keeps_its_stored_length_and_hash
    record = fs_record(DREC_RECORD)

    assert_equal [2, "DIR_REC", 6, 0x2B29A3, "hard1"], record.values_at(:obj_id, :type_name, :name_len, :name_hash, :name)
    assert_equal [27, "REG", [28]], [record[:file_id], record[:dir_type], record[:xfields].map { _1[:value] }]
  end

  def test_a_plain_directory_record_has_a_16_bit_name_length
    key = [(J_TYPE_DIR_REC << 60) | 2, 3].pack("Q<S<") + "ab\0"
    record = Records.decode(key, [5, 0, 4].pack("Q<Q<S<"), hashed: false)

    assert_equal [3, "ab", "DIR"], record.values_at(:name_len, :name, :dir_type)
  end

  def test_a_file_extent_splits_its_length_from_its_flags
    record = fs_record(FILE_EXTENT_RECORD)

    assert_equal [4096, 73_728, 0x12, 206, 0], record.values_at(:logical_addr, :length, :extent_flags, :phys_block_num, :crypto_id)
  end

  def test_an_extended_field_count_beyond_the_record_is_bounded_and_reported
    key, value = INODE_RECORD
    damaged = value.dup.tap { _1.bytesplice(INODE_VAL_SIZE, 2, [0xFFFF].pack("S<")) }
    record = Records.decode(key, damaged, hashed: true)

    # 152 value bytes: 92 of inode, 4 of blob, room for 14 field headers.
    assert_equal 14, record[:xfields].size
    assert_includes record[:problems], "65535 extended fields, room for 14 headers"
  end

  def test_a_name_running_past_its_key_is_an_error_not_an_exception
    key = [(J_TYPE_DIR_REC << 60) | 2, 200].pack("Q<L<") + "short\0"
    record = Records.decode(key, [5, 0, 8].pack("Q<Q<S<"), hashed: true)

    assert_equal "DIR_REC", record[:type_name]
    assert_match(/structure ends before/, record[:error])
  end

  def test_inline_attributes_show_a_symlink_target_and_a_decmpfs_header
    link = xattr_record(2, SYMLINK_EA_NAME, "a/b\0")
    decmpfs = xattr_record(3, DECMPFS_EA_NAME, [DECMPFS_MAGIC, 3, 20_000].pack("L<L<Q<") + "data")

    assert_equal "a/b", fs_record(link)[:target]
    assert_equal({ magic: DECMPFS_MAGIC, compression_type: 3, uncompressed_size: 20_000, magic_ok: true }, fs_record(decmpfs)[:decmpfs])
  end

  def test_an_unknown_record_type_is_reported_by_its_sizes
    record = Records.decode([14 << 60 | 9].pack("Q<"), "xyz", hashed: true)

    assert_equal [9, "unknown(14)", 8, 3], record.values_at(:obj_id, :type_name, :key_size, :value_size)
  end

  def test_text_prints_hex_flag_words_and_counts_trailing_zeros
    out = StringIO.new
    Text.render({ sb: { flags: 0x12, fs_oid: [1026, 0, 0], rows: [{ oid: 1, type: 0x40000002 }] } }, out)

    assert_equal "sb:\n  flags: 0x12\n  fs_oid: 1026 (+2 zero)\n  rows[0]: oid=1 type=0x40000002\n", out.string
  end

  def test_text_escapes_control_characters_in_names
    out = StringIO.new
    Text.render({ name: "a\e[2Jb" }, out)

    assert_equal "name: \"a\\e[2Jb\"\n", out.string
  end

  # Hand-packed file-system records, laid out as the reference gives them.
  # An inode (j_inode_val_t, 92 bytes) with a name and a data-stream
  # extended field: blob (count 2, 48 bytes of data), two x_field_t
  # headers, then the name padded to 8 bytes and the 40-byte j_dstream_t.
  INODE_RECORD = [
    [(3 << 60) | 24].pack("Q<"),
    [20, 24, 1, 2, 3, 4, 0x8010, 1, 0, 1, 0, 99, 99, 0o100644, 0, 0].pack("Q<7l<L<5S<S<Q<") +
      [2, 48, 4, 2, 7, 8, 0x20, 40].pack("S<S<CCS<CCS<") + "f70000\0".ljust(8, "\0") +
      [70_000, 73_728, 0, 70_000, 0].pack("Q<5")
  ].freeze
  # A hashed directory record of "hard1" (length 6 with its NUL, hash
  # 0x2b29a3) for file 27, type REG, with a sibling-ID extended field.
  DREC_RECORD = [
    [(9 << 60) | 2, (0x2B29A3 << 10) | 6].pack("Q<L<") + "hard1\0",
    [27, 0, 8].pack("Q<Q<S<") + [1, 8, 1, 2, 8].pack("S<S<CCS<") + [28].pack("Q<")
  ].freeze
  # File 24's decmpfs attribute, its data in stream 40 (j_xattr_dstream_t:
  # the stream ID and a 40-byte j_dstream_t).
  DECMPFS_STREAM_RECORD = [
    [(4 << 60) | 24, 18].pack("Q<S<") + "com.apple.decmpfs\0",
    [1, 48, 40, 197, 4096, 0, 197, 0].pack("S<S<Q<6")
  ].freeze
  # A file extent of file 24 at 4096: 73728 bytes, flags 0x12, block 206.
  FILE_EXTENT_RECORD = [[(8 << 60) | 24, 4096].pack("Q<Q<"), [(0x12 << 56) | 73_728, 206, 0].pack("Q<3")].freeze

  private

  def fs_record((key, value)) = Records.decode(key, value, hashed: true)

  def xattr_record(obj_id, name, data)
    [[(J_TYPE_XATTR << 60) | obj_id, name.bytesize + 1].pack("Q<S<") + "#{name}\0", [XATTR_DATA_EMBEDDED, data.bytesize].pack("S<S<") + data]
  end

  def walk(nodes, fetch: nil, codec: OMAP_CODEC, storage: STORAGE_PHYSICAL, subtype: KIND_OMAP, **options)
    problems = []
    fetch ||= ->(oid) { Obj.from(oid, nodes.fetch(oid)) }
    tree = BTree.new(fetch:, problem: ->(text) { problems << text },
                     codec:, what: "tree", block_size: BLOCK, storage:,
                     subtype:).walk(nodes.keys.first, **options)
    [tree, problems]
  end

  def walk_hashed(nodes, digest:, fetch: nil)
    fetch ||= ->(oid) { Obj.from(oid, nodes.fetch(oid), headerless: true) }
    name_hashed_records = Records.codec(hashed: true)
    walk(nodes, fetch:, digest:, codec: name_hashed_records,
         storage: STORAGE_VIRTUAL, subtype: KIND_FSTREE)
  end

  # Child OIDs 101 and 102 are stored as offsets 1 and 2 from root 100.
  def hashed_tree_nodes
    first = hashed_node_block(level: 0, entries: [INODE_RECORD], root: false)
    second = hashed_node_block(level: 0, entries: [DREC_RECORD], root: false)
    entries = [[INODE_RECORD.first, [1].pack("Q<") + Digest::SHA256.digest(first)],
               [DREC_RECORD.first, [2].pack("Q<") + Digest::SHA256.digest(second)]]
    { 100 => hashed_node_block(level: 1, entries:, root: true), 101 => first, 102 => second }
  end

  def hashed_node_block(level:, entries:, root:)
    fs_node_block(oid: 0, records: entries, level:, root:, headerless: true,
                  tree_flags: SEALED_TREE_FLAGS)
  end

  def flip_free_space(bytes)
    bytes.dup.tap { _1.setbyte(FREE_SPACE_OFFSET, _1.getbyte(FREE_SPACE_OFFSET) ^ 1) }
  end

  # Table-of-contents entries in a test node: 4-byte kvoff_t entries.
  TOC_ENTRIES = 16

  # A sealed root node of a fixed 16/16 physical tree. Keys fill the key
  # area from its start; values fill the value area back from the footer. A
  # nil value is a ghost.
  def node_block(oid:, level:, entries:, nkeys: entries.size, subtype: KIND_OMAP, key_size: 16, value_size: 16)
    toc_len = TOC_ENTRIES * 4
    flags = NODE_ROOT | NODE_FIXED | (level.zero? ? NODE_LEAF : 0)
    block = "\0".b * BLOCK
    put(block, 0, [0, oid, 1, KIND_BTREE | STORAGE_PHYSICAL, subtype, flags, level, nkeys, 0, toc_len].pack("Q<Q<Q<L<L<S<S<L<S<S<"))
    entries.each_with_index do |(key, value), i|
      put(block, NODE_HEADER_END + i * 4, [i * key_size, value ? (i + 1) * value_size : BTOFF_INVALID].pack("S<S<"))
      put(block, NODE_HEADER_END + toc_len + i * key_size, key)
      put(block, BLOCK - TREE_INFO_SIZE - (i + 1) * value_size, value) if value
    end
    put(block, BLOCK - TREE_INFO_SIZE, [0x12, BLOCK, key_size, value_size, key_size, value_size, entries.size, 1].pack("L<6Q<2"))
    seal(block)
  end

  def superblock_block(block_count:, fs_oid: [], omap_oid: 0)
    block = "\0".b * BLOCK
    put(block, 8, [1, 1, 0x8000_0001, 0].pack("Q<Q<L<L<"))
    put(block, 32, [NX_MAGIC, BLOCK, block_count].pack("L<L<Q<"))
    put(block, 72, ["00112233445566778899aabbccddeeff"].pack("H*"))
    put(block, 104, [2, 4, 1, 3].pack("L<L<Q<Q<"))
    put(block, 136, [0, 2, 0, 2].pack("L<4"))
    put(block, 152, [1024, omap_oid, 1025].pack("Q<3"))
    put(block, 180, [100, *fs_oid].pack("L<Q<*"))
    put(block, 1296, [7, 2].pack("q<Q<"))
    put(block, 1384, [0xABCD].pack("Q<"))
    block
  end

  # A 64-block container: block 0 and the descriptor ring (a mapping block
  # at 1, the checkpoint superblock at 2), the space manager and reaper in
  # the data area at 3 and 4, the object map at 10 with its tree at 11
  # naming the volume superblock at 12, one chunk-information block at 20
  # with its bitmap at 21, and the internal-pool bitmap ring at 22. The
  # volume's object map is at 13 with its tree at 14, which maps the
  # file-system tree's virtual root 1028 to block 15. The volume's file
  # extent tree is at 17; the decmpfs data it points to is at 16.
  def container_image(cibs: [20], spaceman_size: BLOCK, fs_oid: [1026], fext_oid: 17,
                      older_layout: false, extra_blocks: {}, sealed: false, fext_flags: 0, fext_length: BLOCK,
                      fext_records: nil, integrity_meta: {}, integrity_mapping_flags: 0,
                      integrity_meta_oid: sealed ? INTEGRITY_META_OID : 0)
    sb = seal(superblock_block(block_count: 64, fs_oid:, omap_oid: 10))
    fext_records ||= [[[40, 0].pack("Q<Q<"), [(fext_flags << 56) | fext_length, 16].pack("Q<Q<")]]
    omap_record = [[1026, 1].pack("Q<Q<"), [0, BLOCK, 12].pack("L<L<q<")] # (oid, xid) -> (flags, size, paddr)
    blocks = {
      0 => sb, 1 => checkpoint_map(spaceman_size:), 2 => sb,
      3 => spaceman(cibs:, older_layout:), 4 => object(oid: 1025, type: 0x8000_0011, fields: { 64 => [1].pack("L<") }), # reaper flags
      10 => omap(oid: 10, tree: 11), 11 => node_block(oid: 11, level: 0, entries: [omap_record]),
      12 => volume_superblock(fext_oid:, sealed:, integrity_meta_oid:),
      13 => omap(oid: 13, tree: 14), 14 => node_block(oid: 14, level: 0, entries: [fs_root_record]),
      15 => fs_node_block(oid: FS_ROOT_OID, records: [INODE_RECORD, DECMPFS_STREAM_RECORD, DREC_RECORD, FILE_EXTENT_RECORD]),
      16 => [DECMPFS_MAGIC, 7, 20_000].pack("L<L<Q<").ljust(BLOCK, "\0"),
      17 => node_block(oid: 17, level: 0, entries: fext_records, subtype: KIND_FEXT_TREE),
      20 => chunk_info, 21 => bitmap(25), 22 => bitmap(1)
    }
    if sealed
      leaf = hashed_node_block(level: 0, entries: [INODE_RECORD, DECMPFS_STREAM_RECORD, DREC_RECORD], root: false)
      root = hashed_node_block(level: 1, root: true,
                               entries: [[INODE_RECORD.first, [SEALED_LEAF_OID - FS_ROOT_OID].pack("Q<") + Digest::SHA256.digest(leaf)]])
      mappings = [fs_root_record(flags: OMAP_NOHEADER_FLAG),
                  fs_root_record(oid: SEALED_LEAF_OID, paddr: SEALED_LEAF_BLOCK, flags: OMAP_NOHEADER_FLAG),
                  fs_root_record(oid: INTEGRITY_META_OID, paddr: INTEGRITY_META_BLOCK, flags: integrity_mapping_flags)]
      blocks = blocks.merge(
        14 => node_block(oid: 14, level: 0, entries: mappings), 15 => root, SEALED_LEAF_BLOCK => leaf,
        INTEGRITY_META_BLOCK => integrity_meta_block(root_hash: Digest::SHA256.digest(root), **integrity_meta)
      )
    end
    blocks = blocks.merge(extra_blocks)
    Array.new(64) { blocks.fetch(_1) { "\0".b * BLOCK } }.join
  end

  # The hash stays at ROOT_HASH_OFFSET; overriding its declared offset
  # tests invalid pointers without overwriting fields or extending the block.
  def integrity_meta_block(root_hash:, version: 2, flags: 0, hash_type: 1,
                           root_hash_offset: ROOT_HASH_OFFSET, broken_xid: 0)
    object(oid: INTEGRITY_META_OID, type: KIND_INTEGRITY_META,
           fields: { 32 => [version, flags, hash_type, root_hash_offset, broken_xid].pack("L<4Q<"),
                     ROOT_HASH_OFFSET => root_hash })
  end

  def object(oid:, type:, fields: {}, subtype: 0, xid: 1)
    block = "\0".b * BLOCK
    put(block, 8, [oid, xid, type, subtype].pack("Q<Q<L<L<"))
    fields.each { |off, bytes| put(block, off, bytes) }
    seal(block)
  end

  # Mappings: type, subtype, size, pad, fs_oid, oid, paddr.
  def checkpoint_map(spaceman_size:)
    mappings = [[0x8000_0005, 0, spaceman_size, 0, 0, 1024, 3], [0x8000_0011, 0, BLOCK, 0, 0, 1025, 4]]
    object(oid: 1, type: 0x4000_000C, fields: { 32 => [CHECKPOINT_MAP_LAST, 2].pack("L<L<"),
                                                40 => mappings.map { _1.pack("L<4Q<3") }.join })
  end

  # The space manager of the 64-block container, in the current layout
  # (structure end 2520) or the older one (336, no version or structure
  # size). From the structure end lie the internal pool's bitmap XID, ring
  # index and free-next arrays, at +0, +8 and +16, and the main device's
  # chunk-information addresses (`cibs`) at +48.
  def spaceman(cibs:, older_layout: false)
    structure_end = older_layout ? 336 : 2520
    address_offset = structure_end + 48
    geometry = [BLOCK, 32_768, 126, 507].pack("L<4") # block size, blocks per chunk, chunks per CIB, CIBs per CAB
    # block count, chunk count, CIB count, CAB count, free count, address offset, reserved
    device = [64, 1, cibs.size, 0, 39, address_offset, 0, 0].pack("Q<Q<L<L<Q<L<L<Q<")
    # flags VERSIONED; pool: tx multiplier, block count, bitmap blocks, ring blocks, ring base, pool base
    pool = [1, 16, 2, 1, 1, 22, 23].pack("L<L<Q<L<L<Q<Q<")
    # ring free head and tail, the three array offsets, then in the current layout version and structure size
    ring = [0xFFFF, 0, structure_end, structure_end + 8, structure_end + 16].pack("S<S<L<3")
    ring += [1, 2520].pack("L<2") unless older_layout
    arrays = [1, 0, 0xFFFF].pack("Q<S<x6S<") # bitmap XID 1, ring index 0, free-next end marker
    object(oid: 1024, type: 0x8000_0005,
           fields: { 32 => geometry + device, 144 => pool, 320 => ring,
                     structure_end => arrays, address_offset => cibs.pack("Q<*") })
  end

  # Index 0, one chunk: xid 1, first block 0, 64 blocks, 39 free, bitmap at 21.
  def chunk_info = object(oid: 20, type: 0x4000_0007, fields: { 32 => [0, 1, 1, 0, 64, 39, 21].pack("L<L<Q<Q<L<L<Q<") })

  # Manually managed, physical tree types; no snapshots by default.
  def omap(oid:, tree:, snap_count: 0, snapshot_tree_oid: 0, most_recent_snap: 0,
           snapshot_tree_type: 0x4000_0002)
    object(oid:, type: 0x4000_000B,
           fields: { 32 => [1, snap_count, 0x4000_0002, snapshot_tree_type, tree,
                            snapshot_tree_oid, most_recent_snap].pack("L<4Q<3") })
  end

  # Case-insensitive, its object map at 13, a virtual file-system tree
  # rooted at 1028 by default, named TEST. The caller supplies the file
  # extent tree; extent-reference and snapshot metadata trees default to absent.
  def volume_superblock(fext_oid:, oid: 1026, xid: 1, type: KIND_FS, extentref_oid: 0,
                        snap_meta_oid: 0, num_snapshots: 0, num_files: 0, root_oid: FS_ROOT_OID,
                        sealed: false, integrity_meta_oid: 0)
    incompatible = VOL_INCOMPAT_CASE_INSENSITIVE | (sealed ? SEALED_VOLUME_FEATURE : 0)
    extentref_type = extentref_oid.zero? ? 0 : 0x4000_0002
    snap_meta_type = snap_meta_oid.zero? ? 0 : 0x4000_0002
    object(oid:, xid:, type:, fields: { 32 => "APSB", 56 => [incompatible].pack("Q<"),
                                               116 => [KIND_BTREE, extentref_type, snap_meta_type].pack("L<3"),
                                               128 => [13, root_oid, extentref_oid, snap_meta_oid].pack("Q<4"),
                                               184 => [num_files].pack("Q<"), 216 => [num_snapshots].pack("Q<"),
                                               704 => "TEST", 1024 => [integrity_meta_oid].pack("Q<"), 1032 => [fext_oid, 0x4000_0002].pack("Q<L<") })
  end

  # Virtual OID 1028 at xid 1 lives at block 15.
  def fs_root_record(oid: FS_ROOT_OID, xid: 1, paddr: 15, flags: 0) = [[oid, xid].pack("Q<Q<"), [flags, BLOCK, paddr].pack("L<L<q<")]

  def snap_meta_record(xid:, name:, sblock:, extentref:, create_time: 100, change_time: 200, inum: 2)
    [[(1 << 60) | xid].pack("Q<"),
     [extentref, sblock, create_time, change_time, inum, 0x4000_0002, 0, name.bytesize + 1].pack("Q<5L<2S<") + "#{name}\0"]
  end

  def snap_name_record(name:, xid:)
    [[(11 << 60) | 0x0FFF_FFFF_FFFF_FFFF, name.bytesize + 1].pack("Q<S<") + "#{name}\0", [xid].pack("Q<")]
  end

  def extent_record(block:, length:)
    [[(2 << 60) | block].pack("Q<"), [(1 << 60) | length, 24, 1].pack("Q<Q<L<")]
  end

  def extentref_block(oid:, block:, length:)
    fs_node_block(oid:, type: 0x4000_0002, subtype: 0x0F,
                  records: [extent_record(block:, length:)])
  end

  # Blocks 24-29 hold snapshot metadata, the omap snapshot tree, two
  # superblocks and their extent trees. Block 18 holds the live extents.
  # Historical root blocks are present but are only resolved, not walked.
  def snapshot_container_image(snapshot_xids: [4, 8], snap_count: snapshot_xids.size,
                               most_recent_snap: snapshot_xids.max, num_snapshots: 2, older_sblock: 26,
                               snap_meta_oid: 24, snapshot_tree_oid: 25, older_metadata: nil,
                               extra_live_records: [], older_root_oid: 1028)
    metadata = [older_metadata || snap_meta_record(xid: 4, name: "older", sblock: older_sblock, extentref: 28),
                snap_meta_record(xid: 8, name: "newer", sblock: 27, extentref: 29),
                snap_name_record(name: "newer", xid: 8), snap_name_record(name: "older", xid: 4)]
    roots = [fs_root_record(xid: 2, paddr: 30), fs_root_record(xid: 6, paddr: 31), fs_root_record(xid: 9)]
    snapshot_entries = snapshot_xids.map { [[_1].pack("Q<"), [0, 0, 0].pack("L<L<Q<")] }
    container_image(extra_blocks: {
      12 => volume_superblock(fext_oid: 17, xid: 10, extentref_oid: 18, snap_meta_oid:, num_snapshots:),
      13 => omap(oid: 13, tree: 14, snap_count:, snapshot_tree_oid:, most_recent_snap:),
      14 => node_block(oid: 14, level: 0, entries: roots),
      15 => fs_node_block(oid: 1028, xid: 9,
                          records: [INODE_RECORD, DECMPFS_STREAM_RECORD, DREC_RECORD, FILE_EXTENT_RECORD] + extra_live_records),
      18 => extentref_block(oid: 18, block: 40, length: 1),
      24 => fs_node_block(oid: 24, type: 0x4000_0002, subtype: 0x10, records: metadata),
      25 => node_block(oid: 25, level: 0, entries: snapshot_entries, subtype: 0x13, key_size: 8),
      26 => volume_superblock(fext_oid: 0, oid: 26, xid: 4, type: 0x4000_000D, num_snapshots: 1, num_files: 11, root_oid: older_root_oid),
      27 => volume_superblock(fext_oid: 0, oid: 27, xid: 8, type: 0x4000_000D, num_snapshots: 2, num_files: 22),
      28 => extentref_block(oid: 28, block: 41, length: 2),
      29 => extentref_block(oid: 29, block: 42, length: 3),
      30 => fs_node_block(oid: 1028, xid: 2, records: []),
      31 => fs_node_block(oid: 1028, xid: 6, records: [])
    })
  end

  # A virtual root and leaf node of a file-system tree, whose records vary
  # in size: 8-byte kvloc_t entries (key offset and length, value offset
  # and length), keys from the end of the table of contents, values back
  # from the footer in a root, or from the block end in a child. A
  # headerless hashed node keeps the first 32 bytes zero and has no checksum.
  def fs_node_block(oid:, records:, type: KIND_BTREE, subtype: KIND_FSTREE, xid: 1,
                    level: 0, root: true, headerless: false, tree_flags: 0, node_flags: nil)
    toc_len = TOC_ENTRIES * 8
    block = "\0".b * BLOCK
    flags = (root ? NODE_ROOT : 0) | (level.zero? ? NODE_LEAF : 0)
    flags |= NODE_HASHED_FLAG | NODE_NOHEADER_FLAG if headerless
    put(block, 8, [oid, xid, type, subtype].pack("Q<Q<L<L<")) unless headerless
    put(block, 32, [node_flags || flags, level, records.size, 0, toc_len].pack("S<S<L<S<S<"))
    values_end = BLOCK - (root ? TREE_INFO_SIZE : 0)
    records.each_with_index.reduce([0, 0]) do |(k_off, v_end), ((key, value), i)|
      v_end += value.bytesize
      put(block, NODE_HEADER_END + i * 8, [k_off, key.bytesize, v_end, value.bytesize].pack("S<4"))
      put(block, NODE_HEADER_END + toc_len + k_off, key)
      put(block, values_end - v_end, value)
      [k_off + key.bytesize, v_end]
    end
    put(block, BLOCK - TREE_INFO_SIZE, [tree_flags, BLOCK, 0, 0, 64, 256, records.size, 1].pack("L<6Q<2")) if root
    headerless ? block : seal(block)
  end

  # A bitmap block whose first `used` bits are set.
  def bitmap(used) = (("1" * used).ljust(BLOCK * 8, "0")).then { [_1].pack("b*") }

  def put(block, off, bytes) = block.bytesplice(off, bytes.bytesize, bytes)

  def seal(block) = block.tap { put(_1, 0, [Fletcher64.compute(_1)].pack("Q<")) }
end
