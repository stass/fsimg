#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Print the structure of an APFS image: every field of the block-0
# container superblock, the checkpoint descriptor ring (each superblock and
# checkpoint-mapping block, the latest valid superblock marked), the
# ephemeral objects of the selected checkpoint, the container object map and
# its B-tree, each volume, the space manager with its chunk-information
# chain, bitmap free-count recounts, internal pool and free-queue trees, and
# the reaper with its reap lists.
#
# A volume is shown with every field of its superblock, its object map with
# its snapshot tree, and its file-system, extent-reference, snapshot
# metadata and (on a sealed volume) file extent trees, every record decoded,
# its snapshots, its integrity metadata, a summary of its records by type,
# and the decmpfs header of each compressed file. Each snapshot is shown
# with its metadata record, the volume superblock it names and that
# superblock's file-system root resolved at the snapshot's XID, and its
# extent-reference tree. A sealed volume's file-system tree is hashed and
# headerless: each node is checked against the SHA-256 digest its parent
# stores, the root against the integrity metadata's root hash.
#
#   apfsdump.rb [--offset BYTES] [--volume N] [--records] [--json] IMAGE
#
# IMAGE is a raw container; --offset names where the container starts in it,
# for a container inside a partitioned disk image. --volume N dumps only the
# volume in slot N of the superblock's volume array. --records lists every
# node and record of the volume trees and every field of each snapshot
# superblock; without it each live tree shows its root node and counts, and
# each snapshot one line: its superblock's header XID, checksum and counts,
# and its extent-reference tree's counts. --json prints the same document
# as JSON with stable keys, so dumps of two captures can be diffed.
#
# The dumper is written from Apple's "Apple File System Reference"
# (2020-06-22) and shares no code with lib/. Every object it reads has its
# Fletcher-64 checksum verified. A damaged image is reported, not trusted:
# each rule the image breaks is a `problem:` line at the end of the dump (a
# `problems` array in JSON) and the dump goes on with the next structure.
# Every loop is bounded by the size of the object it reads or by a fixed
# limit, never by a count read from the image alone. The exit status is 0
# for a clean dump and 1 when there is any problem or the image cannot be
# read as a container at all.
require "digest"
require "json"
require "optparse"
require "set"

module ApfsDump
  class Error < StandardError; end

  NX_MAGIC = 0x4253584E # "NXSB"
  MIN_BLOCK_SIZE = 4096
  MAX_BLOCK_SIZE = 65_536
  OBJ_HEADER_SIZE = 32

  # Bounds of this dumper, not of the format: no loop runs longer than these.
  MAX_RING_BLOCKS = 1 << 12
  MAX_TREE_NODES = 1 << 17
  MAX_REAP_LISTS = 1 << 12
  # Native ephemeral objects are a few blocks; this bounds one read.
  MAX_OBJECT_BLOCKS = 256
  # All ephemeral objects of a checkpoint together; native ones hold a few
  # blocks.
  MAX_EPHEMERAL_BYTES = 64 << 20
  # Leaf records one tree walk decodes.
  MAX_TREE_RECORDS = 1 << 20
  # Problems kept and printed; any further ones are only counted.
  MAX_PROBLEMS = 10_000
  # Chunk-information and address blocks of one device.
  MAX_CHAIN_BLOCKS = 1 << 16
  # A B-tree is at most 16 levels (root level 15).
  MAX_TREE_LEVEL = 15

  KIND_NX_SUPERBLOCK = 0x01
  KIND_BTREE = 0x02
  KIND_BTREE_NODE = 0x03
  KIND_SPACEMAN = 0x05
  KIND_SPACEMAN_CAB = 0x06
  KIND_SPACEMAN_CIB = 0x07
  KIND_SPACEMAN_FREE_QUEUE = 0x09
  KIND_OMAP = 0x0B
  KIND_CHECKPOINT_MAP = 0x0C
  KIND_FS = 0x0D
  KIND_NX_REAPER = 0x11
  KIND_NX_REAP_LIST = 0x12
  KIND_OMAP_SNAPSHOT = 0x13

  OBJECT_KINDS = {
    0x00 => "INVALID", 0x01 => "NX_SUPERBLOCK", 0x02 => "BTREE", 0x03 => "BTREE_NODE",
    0x05 => "SPACEMAN", 0x06 => "SPACEMAN_CAB", 0x07 => "SPACEMAN_CIB", 0x08 => "SPACEMAN_BITMAP",
    0x09 => "SPACEMAN_FREE_QUEUE", 0x0A => "EXTENT_LIST_TREE", 0x0B => "OMAP", 0x0C => "CHECKPOINT_MAP",
    0x0D => "FS", 0x0E => "FSTREE", 0x0F => "BLOCKREFTREE", 0x10 => "SNAPMETATREE", 0x11 => "NX_REAPER",
    0x12 => "NX_REAP_LIST", 0x13 => "OMAP_SNAPSHOT", 0x14 => "EFI_JUMPSTART", 0x15 => "FUSION_MIDDLE_TREE",
    0x16 => "NX_FUSION_WBC", 0x17 => "NX_FUSION_WBC_LIST", 0x18 => "ER_STATE", 0x19 => "GBITMAP",
    0x1A => "GBITMAP_TREE", 0x1B => "GBITMAP_BLOCK", 0x1C => "ER_RECOVERY_BLOCK", 0x1D => "SNAP_META_EXT",
    0x1E => "INTEGRITY_META", 0x1F => "FEXT_TREE", 0x20 => "RESERVED_20", 0xFF => "TEST"
  }.freeze

  STORAGE_MASK = 0xC000_0000
  STORAGE_VIRTUAL = 0x0000_0000
  STORAGE_EPHEMERAL = 0x8000_0000
  STORAGE_PHYSICAL = 0x4000_0000
  STORAGE = { STORAGE_VIRTUAL => "virtual", STORAGE_EPHEMERAL => "ephemeral", STORAGE_PHYSICAL => "physical" }.freeze
  TYPE_FLAGS = { 0x2000_0000 => "NOHEADER", 0x1000_0000 => "ENCRYPTED", 0x0800_0000 => "NONPERSISTENT" }.freeze

  NX_FEATURES = { 0x1 => "DEFRAG", 0x2 => "LCFD" }.freeze
  NX_INCOMPAT = { 0x1 => "VERSION1", 0x2 => "VERSION2", 0x100 => "FUSION" }.freeze
  NX_FLAGS = { 0x1 => "RESERVED_1", 0x2 => "RESERVED_2", 0x4 => "CRYPTO_SW" }.freeze
  CHECKPOINT_FLAGS = { 0x1 => "LAST" }.freeze
  CHECKPOINT_MAP_LAST = 0x1
  OMAP_FLAGS = {
    0x1 => "MANUALLY_MANAGED", 0x2 => "ENCRYPTING", 0x4 => "DECRYPTING", 0x8 => "KEYROLLING",
    0x10 => "CRYPTO_GENERATION"
  }.freeze
  OMAP_VAL_FLAGS = { 0x1 => "DELETED", 0x2 => "SAVED", 0x4 => "ENCRYPTED", 0x8 => "NOHEADER", 0x10 => "CRYPTO_GENERATION" }.freeze
  OMAP_VAL_DELETED = 0x1
  OMAP_VAL_NOHEADER = 0x8
  OMAP_SNAPSHOT_FLAGS = { 0x1 => "DELETED", 0x2 => "REVERTED" }.freeze
  NODE_FLAGS = {
    0x1 => "ROOT", 0x2 => "LEAF", 0x4 => "FIXED_KV_SIZE", 0x8 => "HASHED", 0x10 => "NOHEADER",
    0x8000 => "CHECK_KOFF_INVAL"
  }.freeze
  NODE_ROOT = 0x1
  NODE_LEAF = 0x2
  NODE_FIXED = 0x4
  NODE_HASHED = 0x8
  NODE_NOHEADER = 0x10
  TREE_FLAGS = {
    0x1 => "UINT64_KEYS", 0x2 => "SEQUENTIAL_INSERT", 0x4 => "ALLOW_GHOSTS", 0x8 => "EPHEMERAL",
    0x10 => "PHYSICAL", 0x20 => "NONPERSISTENT", 0x40 => "KV_NONALIGNED", 0x80 => "HASHED", 0x100 => "NOHEADER"
  }.freeze
  TREE_ALLOW_GHOSTS = 0x4
  TREE_HASHED = 0x80
  # A hashed index value: the child OID less the root OID, then the
  # SHA-256 of the child node, the only hash this dumper checks.
  SHA256_SIZE = 32
  HASHED_INDEX_VALUE_SIZE = 8 + SHA256_SIZE
  U64_MASK = (1 << 64) - 1
  BTOFF_INVALID = 0xFFFF
  SM_FLAGS = { 0x1 => "VERSIONED" }.freeze
  REAPER_FLAGS = { 0x1 => "BHM_FLAG", 0x2 => "CONTINUE" }.freeze
  NRLE_FLAGS = { 0x1 => "VALID", 0x2 => "REAP_ID_RECORD", 0x4 => "CALL", 0x8 => "COMPLETION", 0x10 => "CLEANUP" }.freeze
  FREE_QUEUE_NAMES = %w[internal-pool main tier2].freeze
  DEVICE_NAMES = %w[main tier2].freeze

  # Field tables: [name, byte offset, codec]. A codec is an unpack directive
  # (one value, or an array when it ends in a count), a Symbol naming an
  # entry of Layout::CODECS, a table for one nested structure, or
  # [table, stride, count] for an array of them.
  OBJ_FIELDS = [
    [:cksum, 0, "Q<"], [:oid, 8, "Q<"], [:xid, 16, "Q<"], [:type, 24, "L<"], [:subtype, 28, "L<"]
  ].freeze

  NX_FIELDS = [
    [:magic, 32, "L<"], [:block_size, 36, "L<"], [:block_count, 40, "Q<"],
    [:features, 48, "Q<"], [:readonly_compatible_features, 56, "Q<"], [:incompatible_features, 64, "Q<"],
    [:uuid, 72, :uuid], [:next_oid, 88, "Q<"], [:next_xid, 96, "Q<"],
    [:xp_desc_blocks, 104, "L<"], [:xp_data_blocks, 108, "L<"], [:xp_desc_base, 112, "Q<"],
    [:xp_data_base, 120, "Q<"], [:xp_desc_next, 128, "L<"], [:xp_data_next, 132, "L<"],
    [:xp_desc_index, 136, "L<"], [:xp_desc_len, 140, "L<"], [:xp_data_index, 144, "L<"],
    [:xp_data_len, 148, "L<"], [:spaceman_oid, 152, "Q<"], [:omap_oid, 160, "Q<"], [:reaper_oid, 168, "Q<"],
    [:test_type, 176, "L<"], [:max_file_systems, 180, "L<"], [:fs_oid, 184, "Q<100"],
    [:counters, 984, "Q<32"], [:blocked_out_prange, 1240, :prange], [:evict_mapping_tree_oid, 1256, "Q<"],
    [:flags, 1264, "Q<"], [:efi_jumpstart, 1272, "Q<"], [:fusion_uuid, 1280, :uuid],
    [:keylocker, 1296, :prange], [:ephemeral_info, 1312, "Q<4"], [:test_oid, 1344, "Q<"],
    [:fusion_mt_oid, 1352, "Q<"], [:fusion_wbc_oid, 1360, "Q<"], [:fusion_wbc, 1368, :prange],
    [:newest_mounted_version, 1384, "Q<"], [:mkb_locker, 1392, :prange]
  ].freeze

  CHECKPOINT_MAP_FIELDS = [[:flags, 32, "L<"], [:count, 36, "L<"]].freeze
  CHECKPOINT_MAPPING_SIZE = 40
  CHECKPOINT_MAPPING_FIELDS = [
    [:type, 0, "L<"], [:subtype, 4, "L<"], [:size, 8, "L<"], [:pad, 12, "L<"],
    [:fs_oid, 16, "Q<"], [:oid, 24, "Q<"], [:paddr, 32, "Q<"]
  ].freeze

  OMAP_FIELDS = [
    [:flags, 32, "L<"], [:snap_count, 36, "L<"], [:tree_type, 40, "L<"], [:snapshot_tree_type, 44, "L<"],
    [:tree_oid, 48, "Q<"], [:snapshot_tree_oid, 56, "Q<"], [:most_recent_snap, 64, "Q<"],
    [:pending_revert_min, 72, "Q<"], [:pending_revert_max, 80, "Q<"]
  ].freeze

  NODE_HEADER_END = 56
  TREE_INFO_SIZE = 40
  NODE_FIELDS = [
    [:flags, 32, "S<"], [:level, 34, "S<"], [:nkeys, 36, "L<"], [:table_space, 40, :nloc],
    [:free_space, 44, :nloc], [:key_free_list, 48, :nloc], [:val_free_list, 52, :nloc]
  ].freeze
  # The btree_info_t footer, at the end of a root node.
  TREE_INFO_FIELDS = [
    [:flags, 0, "L<"], [:node_size, 4, "L<"], [:key_size, 8, "L<"], [:val_size, 12, "L<"],
    [:longest_key, 16, "L<"], [:longest_val, 20, "L<"], [:key_count, 24, "Q<"], [:node_count, 32, "Q<"]
  ].freeze

  DEVICE_FIELDS = [
    [:block_count, 0, "Q<"], [:chunk_count, 8, "Q<"], [:cib_count, 16, "L<"], [:cab_count, 20, "L<"],
    [:free_count, 24, "Q<"], [:addr_offset, 32, "L<"], [:reserved, 36, "L<"], [:reserved2, 40, "Q<"]
  ].freeze
  FREE_QUEUE_FIELDS = [
    [:count, 0, "Q<"], [:tree_oid, 8, "Q<"], [:oldest_xid, 16, "Q<"], [:tree_node_limit, 24, "S<"],
    [:pad16, 26, "S<"], [:pad32, 28, "L<"], [:reserved, 32, "Q<"]
  ].freeze
  ALLOCATION_ZONE_FIELDS = [
    [:zone_start, 0, "Q<"], [:zone_end, 8, "Q<"], [:previous_boundaries, 16, "Q<14"],
    [:zone_id, 128, "S<"], [:previous_boundary_index, 130, "S<"], [:reserved, 132, "L<"]
  ].freeze
  ALLOCATION_ZONES_PER_DEVICE = 8
  SPACEMAN_FIELDS = [
    [:block_size, 32, "L<"], [:blocks_per_chunk, 36, "L<"], [:chunks_per_cib, 40, "L<"],
    [:cibs_per_cab, 44, "L<"], [:dev, 48, [DEVICE_FIELDS, 48, 2]], [:flags, 144, "L<"],
    [:ip_bm_tx_multiplier, 148, "L<"], [:ip_block_count, 152, "Q<"], [:ip_bm_size_in_blocks, 160, "L<"],
    [:ip_bm_block_count, 164, "L<"], [:ip_bm_base, 168, "Q<"], [:ip_base, 176, "Q<"],
    [:fs_reserve_block_count, 184, "Q<"], [:fs_reserve_alloc_count, 192, "Q<"],
    [:fq, 200, [FREE_QUEUE_FIELDS, 40, 3]], [:ip_bm_free_head, 320, "S<"], [:ip_bm_free_tail, 322, "S<"],
    [:ip_bm_xid_offset, 324, "L<"], [:ip_bitmap_offset, 328, "L<"], [:ip_bm_free_next_offset, 332, "L<"]
  ].freeze
  # The fixed structure has two layouts (specs/design-apfs.md, "Ephemeral
  # objects and the space manager"): the current one adds these fields and
  # ends at SPACEMAN_CURRENT_END; the older one ends at SPACEMAN_OLDER_END,
  # where its arrays begin.
  SPACEMAN_CURRENT_FIELDS = [
    [:version, 336, "L<"], [:struct_size, 340, "L<"],
    [:datazone, 344, [ALLOCATION_ZONE_FIELDS, 136, 2 * ALLOCATION_ZONES_PER_DEVICE]]
  ].freeze
  SPACEMAN_CURRENT_END = 2520
  SPACEMAN_OLDER_END = 336
  # Internal-pool ring blocks are indexed by u16; 0xffff is the end marker.
  IP_BM_INDEX_INVALID = 0xFFFF
  IP_BM_BLOCK_COUNT_MAX = 0xFFFE

  ADDRESS_BLOCK_FIELDS = [[:index, 32, "L<"], [:count, 36, "L<"]].freeze
  CHUNK_INFO_SIZE = 32
  CHUNK_INFO_FIELDS = [
    [:xid, 0, "Q<"], [:addr, 8, "Q<"], [:block_count, 16, "L<"], [:free_count, 20, "L<"], [:bitmap_addr, 24, "Q<"]
  ].freeze

  REAPER_FIELDS = [
    [:next_reap_id, 32, "Q<"], [:completed_id, 40, "Q<"], [:head, 48, "Q<"], [:tail, 56, "Q<"],
    [:flags, 64, "L<"], [:rlcount, 68, "L<"], [:reap_type, 72, "L<"], [:reap_size, 76, "L<"],
    [:reap_fs_oid, 80, "Q<"], [:reap_oid, 88, "Q<"], [:reap_xid, 96, "Q<"], [:nrle_flags, 104, "L<"],
    [:state_buffer_size, 108, "L<"]
  ].freeze
  REAPER_STATE_OFFSET = 112
  REAP_LIST_FIELDS = [
    [:next, 32, "Q<"], [:flags, 40, "L<"], [:max, 44, "L<"], [:count, 48, "L<"], [:first, 52, "L<"],
    [:last, 56, "L<"], [:free, 60, "L<"]
  ].freeze
  REAP_LIST_ENTRIES_OFFSET = 64
  REAP_LIST_ENTRY_SIZE = 40
  REAP_LIST_ENTRY_FIELDS = [
    [:next, 0, "L<"], [:flags, 4, "L<"], [:type, 8, "L<"], [:size, 12, "L<"], [:fs_oid, 16, "Q<"],
    [:oid, 24, "Q<"], [:xid, 32, "Q<"]
  ].freeze

  # ---- volumes ------------------------------------------------------------

  APFS_MAGIC = 0x42535041 # "APSB"
  KIND_FSTREE = 0x0E
  KIND_BLOCKREFTREE = 0x0F
  KIND_SNAPMETATREE = 0x10
  KIND_INTEGRITY_META = 0x1E
  KIND_FEXT_TREE = 0x1F

  VOL_FEATURES = {
    0x1 => "DEFRAG_PRERELEASE", 0x2 => "HARDLINK_MAP_RECORDS", 0x4 => "DEFRAG", 0x8 => "STRICTATIME",
    0x10 => "VOLGRP_SYSTEM_INO_SPACE"
  }.freeze
  VOL_ROCOMPAT = {}.freeze
  VOL_INCOMPAT = {
    0x1 => "CASE_INSENSITIVE", 0x2 => "DATALESS_SNAPS", 0x4 => "ENC_ROLLED", 0x8 => "NORMALIZATION_INSENSITIVE",
    0x10 => "INCOMPLETE_RESTORE", 0x20 => "SEALED_VOLUME", 0x40 => "RESERVED_40"
  }.freeze
  VOL_INCOMPAT_CASE_INSENSITIVE = 0x1
  VOL_INCOMPAT_NORMALIZATION_INSENSITIVE = 0x8
  VOL_INCOMPAT_SEALED = 0x20
  VOL_FS_FLAGS = {
    0x1 => "UNENCRYPTED", 0x2 => "RESERVED_2", 0x4 => "RESERVED_4", 0x8 => "ONEKEY", 0x10 => "SPILLEDOVER",
    0x20 => "RUN_SPILLOVER_CLEANER", 0x40 => "ALWAYS_CHECK_EXTENTREF", 0x80 => "RESERVED_80", 0x100 => "RESERVED_100"
  }.freeze
  # A volume has at most one role: one of the low six bits, or an
  # enumerated value shifted left by six.
  VOL_ROLES = {
    0x0 => "NONE", 0x1 => "SYSTEM", 0x2 => "USER", 0x4 => "RECOVERY", 0x8 => "VM", 0x10 => "PREBOOT",
    0x20 => "INSTALLER", 0x40 => "DATA", 0x80 => "BASEBAND", 0xC0 => "UPDATE", 0x100 => "XART",
    0x140 => "HARDWARE", 0x180 => "BACKUP", 0x1C0 => "SIDECAR", 0x200 => "RESERVED_8", 0x240 => "ENTERPRISE",
    0x280 => "RESERVED_10", 0x2C0 => "PRELOGIN"
  }.freeze

  META_CRYPTO_FIELDS = [
    [:major_version, 0, "S<"], [:minor_version, 2, "S<"], [:cpflags, 4, "L<"], [:persistent_class, 8, "L<"],
    [:key_os_version, 12, "L<"], [:key_revision, 16, "S<"], [:unused, 18, "S<"]
  ].freeze
  MODIFIED_BY_FIELDS = [[:id, 0, :name32], [:timestamp, 32, "Q<"], [:last_xid, 40, "Q<"]].freeze
  APFS_MAX_HIST = 8
  APFS_FIELDS = [
    [:magic, 32, "L<"], [:fs_index, 36, "L<"], [:features, 40, "Q<"], [:readonly_compatible_features, 48, "Q<"],
    [:incompatible_features, 56, "Q<"], [:unmount_time, 64, "Q<"], [:fs_reserve_block_count, 72, "Q<"],
    [:fs_quota_block_count, 80, "Q<"], [:fs_alloc_count, 88, "Q<"], [:meta_crypto, 96, META_CRYPTO_FIELDS],
    [:root_tree_type, 116, "L<"], [:extentref_tree_type, 120, "L<"], [:snap_meta_tree_type, 124, "L<"],
    [:omap_oid, 128, "Q<"], [:root_tree_oid, 136, "Q<"], [:extentref_tree_oid, 144, "Q<"],
    [:snap_meta_tree_oid, 152, "Q<"], [:revert_to_xid, 160, "Q<"], [:revert_to_sblock_oid, 168, "Q<"],
    [:next_obj_id, 176, "Q<"], [:num_files, 184, "Q<"], [:num_directories, 192, "Q<"], [:num_symlinks, 200, "Q<"],
    [:num_other_fsobjects, 208, "Q<"], [:num_snapshots, 216, "Q<"], [:total_blocks_alloced, 224, "Q<"],
    [:total_blocks_freed, 232, "Q<"], [:vol_uuid, 240, :uuid], [:last_mod_time, 256, "Q<"], [:fs_flags, 264, "Q<"],
    [:formatted_by, 272, MODIFIED_BY_FIELDS], [:modified_by, 320, [MODIFIED_BY_FIELDS, 48, APFS_MAX_HIST]],
    [:volname, 704, :volname], [:next_doc_id, 960, "L<"], [:role, 964, "S<"], [:reserved, 966, "S<"],
    [:root_to_xid, 968, "Q<"], [:er_state_oid, 976, "Q<"], [:cloneinfo_id_epoch, 984, "Q<"],
    [:cloneinfo_xid, 992, "Q<"], [:snap_meta_ext_oid, 1000, "Q<"], [:volume_group_id, 1008, :uuid],
    [:integrity_meta_oid, 1024, "Q<"], [:fext_tree_oid, 1032, "Q<"], [:fext_tree_type, 1040, "L<"],
    [:reserved_type, 1044, "L<"], [:reserved_oid, 1048, "Q<"]
  ].freeze

  INTEGRITY_META_FIELDS = [
    [:version, 32, "L<"], [:flags, 36, "L<"], [:hash_type, 40, "L<"], [:root_hash_offset, 44, "L<"],
    [:broken_xid, 48, "Q<"]
  ].freeze
  INTEGRITY_FLAGS = { 0x1 => "SEAL_BROKEN" }.freeze
  INTEGRITY_SEAL_BROKEN = 0x1
  HASH_TYPES = { 0 => "INVALID", 1 => "SHA256", 2 => "SHA512_256", 3 => "SHA384", 4 => "SHA512" }.freeze
  HASH_SIZES = { 1 => 32, 2 => 32, 3 => 48, 4 => 64 }.freeze
  HASH_SHA256 = 1
  # Version 2 is the only one captured; its root hash starts past the
  # header and the version's fixed and reserved fields.
  INTEGRITY_VERSION = 2
  INTEGRITY_ROOT_HASH_MIN = 128

  # ---- file-system records --------------------------------------------------

  OBJ_ID_MASK = 0x0FFF_FFFF_FFFF_FFFF
  OBJ_TYPE_SHIFT = 60
  J_TYPE_SNAP_METADATA = 1
  J_TYPE_EXTENT = 2
  J_TYPE_INODE = 3
  J_TYPE_XATTR = 4
  J_TYPE_SIBLING_LINK = 5
  J_TYPE_DSTREAM_ID = 6
  J_TYPE_CRYPTO_STATE = 7
  J_TYPE_FILE_EXTENT = 8
  J_TYPE_DIR_REC = 9
  J_TYPE_DIR_STATS = 10
  J_TYPE_SNAP_NAME = 11
  J_TYPE_SIBLING_MAP = 12
  J_TYPE_FILE_INFO = 13
  J_TYPES = {
    0 => "ANY", 1 => "SNAP_METADATA", 2 => "EXTENT", 3 => "INODE", 4 => "XATTR", 5 => "SIBLING_LINK",
    6 => "DSTREAM_ID", 7 => "CRYPTO_STATE", 8 => "FILE_EXTENT", 9 => "DIR_REC", 10 => "DIR_STATS",
    11 => "SNAP_NAME", 12 => "SIBLING_MAP", 13 => "FILE_INFO", 15 => "INVALID"
  }.freeze
  J_KINDS = { 0 => "ANY", 1 => "NEW", 2 => "UPDATE", 3 => "DEAD", 4 => "UPDATE_REFCNT", 255 => "INVALID" }.freeze

  INODE_FIELDS = [
    [:parent_id, 0, "Q<"], [:private_id, 8, "Q<"], [:create_time, 16, "Q<"], [:mod_time, 24, "Q<"],
    [:change_time, 32, "Q<"], [:access_time, 40, "Q<"], [:internal_flags, 48, "Q<"],
    [:nchildren_or_nlink, 56, "l<"], [:default_protection_class, 60, "L<"], [:write_generation_counter, 64, "L<"],
    [:bsd_flags, 68, "L<"], [:owner, 72, "L<"], [:group, 76, "L<"], [:mode, 80, "S<"], [:pad1, 82, "S<"],
    [:uncompressed_size, 84, "Q<"]
  ].freeze
  INODE_VAL_SIZE = 92
  INODE_FLAGS = {
    0x1 => "IS_APFS_PRIVATE", 0x2 => "MAINTAIN_DIR_STATS", 0x4 => "DIR_STATS_ORIGIN", 0x8 => "PROT_CLASS_EXPLICIT",
    0x10 => "WAS_CLONED", 0x20 => "FLAG_UNUSED", 0x40 => "HAS_SECURITY_EA", 0x80 => "BEING_TRUNCATED",
    0x100 => "HAS_FINDER_INFO", 0x200 => "IS_SPARSE", 0x400 => "WAS_EVER_CLONED", 0x800 => "ACTIVE_FILE_TRIMMED",
    0x1000 => "PINNED_TO_MAIN", 0x2000 => "PINNED_TO_TIER2", 0x4000 => "HAS_RSRC_FORK", 0x8000 => "NO_RSRC_FORK",
    0x10000 => "ALLOCATION_SPILLEDOVER", 0x20000 => "FAST_PROMOTE", 0x40000 => "HAS_UNCOMPRESSED_SIZE",
    0x80000 => "IS_PURGEABLE", 0x100000 => "WANTS_TO_BE_PURGEABLE", 0x200000 => "IS_SYNC_ROOT",
    0x400000 => "SNAPSHOT_COW_EXEMPTION"
  }.freeze
  # File types of an inode's mode (S_IFMT) and of a directory record's
  # flags (DT_*), by the name both share.
  MODE_TYPES = {
    0o010000 => "FIFO", 0o020000 => "CHR", 0o040000 => "DIR", 0o060000 => "BLK", 0o100000 => "REG",
    0o120000 => "LNK", 0o140000 => "SOCK", 0o160000 => "WHT"
  }.freeze
  S_IFMT = 0o170000
  DT_TYPES = {
    0 => "UNKNOWN", 1 => "FIFO", 2 => "CHR", 4 => "DIR", 6 => "BLK", 8 => "REG", 10 => "LNK", 12 => "SOCK", 14 => "WHT"
  }.freeze
  DREC_TYPE_MASK = 0x000F
  DREC_VAL_SIZE = 18
  J_DREC_LEN_MASK = 0x0000_03FF
  J_DREC_HASH_SHIFT = 10

  XATTR_FLAGS = { 0x1 => "DATA_STREAM", 0x2 => "DATA_EMBEDDED", 0x4 => "FILE_SYSTEM_OWNED", 0x8 => "RESERVED_8" }.freeze
  XATTR_DATA_STREAM = 0x1
  XATTR_DATA_EMBEDDED = 0x2
  SYMLINK_EA_NAME = "com.apple.fs.symlink"
  DECMPFS_EA_NAME = "com.apple.decmpfs"
  # The decmpfs header (magic "fpmc", compression type, uncompressed size)
  # is macOS's, not the APFS reference's; only it is decoded.
  DECMPFS_MAGIC = 0x636D_7066
  DECMPFS_FIELDS = [[:magic, 0, "L<"], [:compression_type, 4, "L<"], [:uncompressed_size, 8, "Q<"]].freeze
  # Leading bytes of inline extended-attribute data shown in a record.
  XATTR_DATA_SHOWN = 32

  DSTREAM_FIELDS = [
    [:size, 0, "Q<"], [:alloced_size, 8, "Q<"], [:default_crypto_id, 16, "Q<"], [:total_bytes_written, 24, "Q<"],
    [:total_bytes_read, 32, "Q<"]
  ].freeze
  DIR_STATS_FIELDS = [[:num_children, 0, "Q<"], [:total_size, 8, "Q<"], [:chained_key, 16, "Q<"], [:gen_count, 24, "Q<"]].freeze
  SNAP_METADATA_FIELDS = [
    [:extentref_tree_oid, 0, "Q<"], [:sblock_oid, 8, "Q<"], [:create_time, 16, "Q<"], [:change_time, 24, "Q<"],
    [:inum, 32, "Q<"], [:extentref_tree_type, 40, "L<"], [:snap_flags, 44, "L<"], [:name_len, 48, "S<"]
  ].freeze
  SNAP_META_FLAGS = { 0x1 => "PENDING_DATALESS", 0x2 => "MERGE_IN_PROGRESS" }.freeze
  PEXT_LEN_MASK = 0x0FFF_FFFF_FFFF_FFFF
  PEXT_KIND_SHIFT = 60
  FILE_EXTENT_LEN_MASK = 0x00FF_FFFF_FFFF_FFFF
  FILE_EXTENT_FLAG_SHIFT = 56
  FILE_INFO_TYPES = { 1 => "DATA_HASH" }.freeze

  XF_FLAGS = {
    0x1 => "DATA_DEPENDENT", 0x2 => "DO_NOT_COPY", 0x4 => "RESERVED_4", 0x8 => "CHILDREN_INHERIT",
    0x10 => "USER_FIELD", 0x20 => "SYSTEM_FIELD", 0x40 => "RESERVED_40", 0x80 => "RESERVED_80"
  }.freeze
  # Extended-field types: name and how the data decodes (an unpack
  # directive, a Symbol of Layout::CODECS, a field table, :text for a
  # NUL-terminated string, or :hex for opaque bytes).
  INODE_XFIELDS = {
    1 => [:snap_xid, "Q<"], 2 => [:delta_tree_oid, "Q<"], 3 => [:document_id, "L<"], 4 => [:name, :text],
    5 => [:prev_fsize, "Q<"], 6 => [:reserved_6, :hex], 7 => [:finder_info, :hex], 8 => [:dstream, DSTREAM_FIELDS],
    9 => [:reserved_9, :hex], 10 => [:dir_stats, DIR_STATS_FIELDS], 11 => [:fs_uuid, :uuid],
    12 => [:reserved_12, :hex], 13 => [:sparse_bytes, "Q<"], 14 => [:rdev, "L<"], 15 => [:purgeable_flags, :hex],
    16 => [:orig_sync_root_id, "Q<"]
  }.freeze
  INO_EXT_TYPE_DSTREAM = 8
  DREC_XFIELDS = { 1 => [:sibling_id, "Q<"] }.freeze

  # Fletcher-64 as APFS uses it: over the object's bytes from offset 8 to its
  # end, read as little-endian 32-bit words, both sums modulo 2^32 - 1.
  module Fletcher64
    MOD = 0xFFFF_FFFF

    module_function

    def compute(object)
      s1, s2 = object.byteslice(8..).unpack("L<*").reduce([0, 0]) do |(a, b), word|
        a = (a + word) % MOD
        [a, (b + a) % MOD]
      end
      c1 = MOD - ((s1 + s2) % MOD)
      c2 = MOD - ((s1 + c1) % MOD)
      (c2 << 32) | c1
    end
  end

  # Decoding of field tables and naming of flag words.
  module Layout
    CODECS = {
      uuid: [16, ->(b) { b.unpack("H8H4H4H4H12").join("-") }],
      prange: [16, ->(b) { b.unpack("q<Q<").then { |start, count| { start_paddr: start, block_count: count } } }],
      nloc: [4, ->(b) { b.unpack("S<S<").then { |off, len| { off:, len: } } }],
      name32: [32, ->(b) { Layout.text(b) }],
      volname: [256, ->(b) { Layout.text(b) }]
    }.freeze

    module_function

    # Decodes `table` from `bytes` at `base`. Raises Error when the bytes end
    # before a field does.
    def decode(bytes, table, base: 0) = table.to_h { |name, off, codec| [name, field(bytes, base + off, codec)] }

    def field(bytes, off, codec)
      value =
        case codec
        in Symbol
          CODECS.fetch(codec).then { |len, fn| fn.(slice(bytes, off, len)) }
        in [Array => table, Integer => stride, Integer => count]
          Array.new(count) { decode(bytes, table, base: off + _1 * stride) }
        in [Array, *] => table
          decode(bytes, table, base: off)
        in /\d\z/ # a repeat count: "Q<4" is an array of four values
          bytes.unpack("@#{off}#{codec}")
        else
          bytes.unpack1("@#{off}#{codec}")
        end
      raise Error, "structure ends before byte #{off}" if value.nil? || (value.is_a?(Array) && value.include?(nil))

      value
    rescue ArgumentError
      raise Error, "structure ends before byte #{off}"
    end

    def slice(bytes, off, len)
      raise Error, "structure ends before byte #{off + len}" if off + len > bytes.bytesize

      bytes.byteslice(off, len)
    end

    # The bytes before the first NUL as a UTF-8 string, or, when they are
    # not valid UTF-8, their escaped form, so every name prints and
    # serialises as JSON.
    def text(bytes)
      str = bytes.byteslice(0, bytes.index("\0") || bytes.bytesize).dup.force_encoding(Encoding::UTF_8)
      str.valid_encoding? ? str : str.b.dump
    end

    ARRAY_WIDTHS = { "Q<" => 8, "S<" => 2 }.freeze

    # `count` values of `directive` at `off` in `bytes`, or Error when the
    # bytes end first.
    def array(bytes, off:, count:, directive:)
      slice(bytes, off, count * ARRAY_WIDTHS.fetch(directive)).unpack("#{directive}#{count}")
    end

    def names(table, word)
      known = table.filter_map { |bit, name| name if word.anybits?(bit) }
      unknown = word & ~table.keys.reduce(0, :|)
      (known + (unknown.zero? ? [] : [format("unknown(0x%x)", unknown)])).join(",")
    end

    # `fields` with a `<name>_names` entry after each flag word in `tables`.
    def with_names(fields, tables)
      fields.each_with_object({}) do |(key, value), out|
        out[key] = value
        out[:"#{key}_names"] = names(tables.fetch(key), value) if tables.key?(key)
      end
    end

    def type_name(type)
      kind = OBJECT_KINDS.fetch(type & 0xFFFF) { format("unknown(0x%x)", type & 0xFFFF) }
      storage = STORAGE.fetch(type & STORAGE_MASK, "invalid-storage")
      flags = names(TYPE_FLAGS, type & ~STORAGE_MASK & 0xFFFF_0000)
      [kind, storage, flags].reject(&:empty?).join(" ")
    end

    def kind_name(kind) = OBJECT_KINDS.fetch(kind & 0xFFFF) { format("unknown(0x%x)", kind & 0xFFFF) }

    # Zero bits among the first `bits` bits of a bitmap, least significant bit
    # of each byte first: the free blocks it records.
    def zero_bits(bitmap, bits) = bitmap.unpack1("b*")[0, bits].count("0")
  end

  # One object as read from the image: its block address, bytes, decoded
  # header, and whether its stored checksum matches. A `headerless` object
  # was read through an object-map mapping with the no-header flag: its
  # first 32 bytes are not a header and hold no checksum, so the summary
  # leaves them out and no header rule applies to it.
  Obj = Data.define(:addr, :bytes, :header, :computed, :headerless) do
    def self.from(addr, bytes, headerless: false)
      new(addr:, bytes:, header: Layout.decode(bytes, OBJ_FIELDS), computed: Fletcher64.compute(bytes), headerless:)
    end

    def cksum_ok? = header[:cksum] == computed
    def oid = header[:oid]
    def xid = header[:xid]
    def kind = header[:type] & 0xFFFF
    def storage = header[:type] & STORAGE_MASK
    def subtype = header[:subtype]

    def summary
      return { addr:, headerless: true } if headerless

      { addr:, **header, cksum_ok: cksum_ok?, type_name: Layout.type_name(header[:type]),
        subtype_name: Layout.kind_name(subtype) }
    end

    # The summary followed by `fields`, which may not reuse a header key.
    def document(fields)
      clash = summary.keys & fields.keys
      raise ArgumentError, "fields #{clash.join(', ')} shadow the object header" unless clash.empty?

      summary.merge(fields)
    end

    # The rules of the reference this object breaks, as messages. A
    # headerless object has no header to keep them, which is itself a
    # problem wherever a header is expected.
    def header_problems(kind:, storage:, subtype: nil, oid: nil)
      return ["mapped without a header"] if headerless

      [
        ("kind #{Layout.kind_name(self.kind)}, expected #{Layout.kind_name(kind)}" if self.kind != kind),
        ("storage #{STORAGE.fetch(self.storage, 'invalid')}, expected #{STORAGE.fetch(storage)}" if self.storage != storage),
        ("subtype #{Layout.kind_name(self.subtype)}, expected #{Layout.kind_name(subtype)}" if subtype && self.subtype != subtype),
        ("oid #{self.oid}, expected #{oid}" if oid && self.oid != oid)
      ].compact
    end
  end

  # Bounds-checked access to a container that starts `offset` bytes into
  # the image file.
  class Image
    attr_reader :size

    def initialize(io, offset: 0)
      raise Error, "offset #{offset} is outside the image (#{io.size} bytes)" if offset.negative? || offset > io.size

      @io = io
      @offset = offset
      @size = io.size - offset
    end

    def read(off, len)
      raise Error, "#{len} bytes at #{off} are outside the image (#{@size} bytes)" if off.negative? || len.negative? || off + len > @size

      data = @io.pread(len, @offset + off)
      raise Error, "short read of #{len} bytes at #{off}" if data.bytesize != len

      data.b
    end
  end

  # How a tree's records decode: fixed key and value sizes (nil in a tree of
  # variable-size records) and `record`, called with the key bytes and the
  # value bytes (nil for a ghost) and returning the record's fields.
  Codec = Data.define(:name, :key_size, :value_size, :record)

  OMAP_CODEC = Codec.new(
    name: "omap", key_size: 16, value_size: 16,
    record: lambda do |key, value|
      oid, xid = key.unpack("Q<Q<")
      flags, size, paddr = value ? value.unpack("L<L<q<") : [nil, nil, nil]
      { oid:, xid:, flags:, flags_names: flags && Layout.names(OMAP_VAL_FLAGS, flags), size:, paddr: }
    end
  )

  # An object map's snapshot record: a snapshot's XID, its flags and an
  # OID the reference reserves (zero). A ghost record has no value.
  OMAP_SNAPSHOT_CODEC = Codec.new(
    name: "omap snapshot", key_size: 8, value_size: 16,
    record: lambda do |key, value|
      flags, _pad, oid = value ? value.unpack("L<L<Q<") : [nil, nil, nil]
      { xid: key.unpack1("Q<"), flags:, flags_names: flags && Layout.names(OMAP_SNAPSHOT_FLAGS, flags), oid: }
    end
  )

  # A free-queue record: a run of `count` blocks freed at `xid`. A ghost
  # record has no value and stands for one block.
  FREE_QUEUE_CODEC = Codec.new(
    name: "free queue", key_size: 16, value_size: 8,
    record: lambda do |key, value|
      xid, paddr = key.unpack("Q<q<")
      { xid:, paddr:, count: value ? value.unpack1("Q<") : 1, ghost: value.nil? }
    end
  )

  # An object map's records indexed by OID, resolving virtual OIDs. A record
  # without a value (a ghost) maps nothing.
  class ObjectMap
    def initialize(records)
      @by_oid = records.reject { _1[:paddr].nil? }.group_by { _1[:oid] }
    end

    # The newest record of `oid` not above `xid`, or nil when there is none.
    def find(oid, xid) = @by_oid.fetch(oid, []).select { _1[:xid] <= xid }.max_by { _1[:xid] }
  end

  # Walks a B-tree from its root and returns its nodes and leaf records;
  # given a block, it yields each record instead of keeping it, so a
  # caller that only counts holds none of them. `fetch` maps a child OID
  # to its Obj (raising Error when it cannot);
  # `problem` receives a message for each rule a node breaks. The walk is
  # bounded: levels fall by one per step from at most MAX_TREE_LEVEL, a node
  # is visited once, and at most MAX_TREE_NODES nodes are read.
  #
  # In a hashed tree (a sealed volume's file-system tree) an index value is
  # the child's OID less the root's OID, then the child's SHA-256; each
  # child's digest is checked against it, and the root's against the digest
  # #walk is given. Its nodes may be headerless, which only a digest binds
  # to their OIDs, so a headerless node in a tree without hashes is a
  # problem. A node whose digest does not match is reported and still walked.
  class BTree
    Entry = Data.define(:key, :value)

    def initialize(fetch:, problem:, codec:, what:, block_size:, storage:, subtype:)
      @fetch = fetch
      @problem = problem
      @codec = codec
      @what = what
      @block_size = block_size
      @storage = storage
      @subtype = subtype
    end

    # `digest` is the SHA-256 the root must have, nil when nothing stores one.
    def walk(root_oid, digest: nil, &each)
      @root_oid = root_oid
      # Set from the root's footer by #tree_info.
      @ghosts = @hashed = false
      @nodes = []
      @records = []
      @each = each || @records.method(:<<)
      @decoded = 0
      @seen = Set.new
      @full = false
      visit(root_oid, level: nil, info: nil, digest:)
      { nodes: @nodes, records: @records }
    end

    private

    # Reports `text` and returns nil, whatever the callback returns, so a
    # rejected entry or child is `nil` to the code that skips it.
    def problem(text)
      @problem.("#{@what}: #{text}")
      nil
    end

    # `level` is the level the parent implies, nil for the root; `info` is
    # the root's footer, which fixes the record sizes of every node;
    # `digest` is the SHA-256 stored for the node, nil when nothing stores
    # one.
    def visit(oid, level:, info:, digest:)
      return problem("node limit #{MAX_TREE_NODES} reached at oid #{oid}") if @seen.size >= MAX_TREE_NODES
      return problem("node #{oid} is reached twice") unless @seen.add?(oid)

      obj = @fetch.(oid)
      root = info.nil?
      node = Layout.decode(obj.bytes, NODE_FIELDS)
      info = tree_info(oid, obj) if root
      # In a hashed tree the digest stands in for a header, whose place
      # holds zeros; anywhere else a headerless node breaks the header rules.
      if obj.headerless && @hashed
        problem("node #{oid}: mapped without a header, but its first #{OBJ_HEADER_SIZE} bytes are not zero") unless
          obj.bytes.byteslice(0, OBJ_HEADER_SIZE).delete("\0").empty?
      else
        obj.header_problems(kind: root ? KIND_BTREE : KIND_BTREE_NODE, storage: @storage, subtype: @subtype)
           .each { problem("node #{oid}: #{_1}") }
      end
      digest_problems(oid, obj, digest:, root:)
      return unless node_shape_ok?(oid, node, headerless: obj.headerless, level:, root:)

      entries = entries(oid, obj.bytes, node, info:, root:)
      @nodes << node_summary(obj, node, info:, root:, entries:)
      node[:level].zero? ? leaf(entries) : children(oid, entries, level: node[:level], info:)
    rescue Error => e
      problem("node #{oid}: #{e.message}")
    end

    # Compares the node's SHA-256 with the `digest` its parent, or for the
    # root the integrity metadata, stores. A hashed tree's root must have one.
    def digest_problems(oid, obj, digest:, root:)
      if digest
        actual = Digest::SHA256.digest(obj.bytes)
        problem("node #{oid}: SHA-256 #{actual.unpack1('H*')} does not match the stored digest #{digest.unpack1('H*')}") if actual != digest
      elsif @hashed && root
        problem("root #{oid}: hashed tree has no root hash to check")
      end
    end

    # Decodes the root's footer and keeps the two flags that change how
    # every node of the tree is read: ghosts allowed, and hashed.
    def tree_info(oid, obj)
      info = Layout.decode(obj.bytes, TREE_INFO_FIELDS, base: @block_size - TREE_INFO_SIZE)
      @ghosts = info[:flags].anybits?(TREE_ALLOW_GHOSTS)
      @hashed = info[:flags].anybits?(TREE_HASHED)
      problem("root #{oid}: footer node size #{info[:node_size]}, expected #{@block_size}") if info[:node_size] != @block_size
      if @codec.key_size && [info[:key_size], info[:val_size]] != [@codec.key_size, @codec.value_size]
        problem("root #{oid}: footer key and value sizes #{info[:key_size]}/#{info[:val_size]}, " \
                "expected #{@codec.key_size}/#{@codec.value_size}")
      end
      info
    end

    # Reports every flag and level rule the node breaks. Returns whether its
    # level is usable: a wrong level would let the walk recurse without bound.
    # The HASHED flag follows the tree, NOHEADER the node's mapping.
    def node_shape_ok?(oid, node, headerless:, level:, root:)
      as_implied = level.nil? || node[:level] == level
      in_bound = node[:level] <= MAX_TREE_LEVEL
      problems = [
        ("ROOT flag #{node[:flags].anybits?(NODE_ROOT)}, expected #{root}" if node[:flags].anybits?(NODE_ROOT) != root),
        ("LEAF flag disagrees with level #{node[:level]}" if node[:flags].anybits?(NODE_LEAF) != node[:level].zero?),
        ("HASHED flag disagrees with the tree" if node[:flags].anybits?(NODE_HASHED) != @hashed),
        ("NOHEADER flag disagrees with its mapping" if node[:flags].anybits?(NODE_NOHEADER) != headerless),
        ("level #{node[:level]}, expected #{level}" unless as_implied),
        ("level #{node[:level]} is above #{MAX_TREE_LEVEL}" unless in_bound)
      ].compact
      problems.each { problem("node #{oid}: #{_1}") }
      as_implied && in_bound
    end

    # The node's entries, each a key slice and a value slice (nil for a
    # ghost). An entry whose ranges leave the key or value area is reported
    # and skipped. Offsets are those of the reference: table of contents at
    # 56 + table_space.off, keys from its end, values back from the end of
    # the value area (the node end, or the footer start in a root).
    def entries(oid, bytes, node, info:, root:)
      fixed = node[:flags].anybits?(NODE_FIXED)
      width = fixed ? 4 : 8
      toc = NODE_HEADER_END + node[:table_space][:off]
      keys = toc + node[:table_space][:len]
      values_end = @block_size - (root ? TREE_INFO_SIZE : 0)
      if keys > values_end
        problem("node #{oid}: key area starts at #{keys}, past the value area end #{values_end}")
        return []
      end

      capacity = node[:table_space][:len] / width
      if node[:nkeys] > capacity
        problem("node #{oid}: #{node[:nkeys]} keys, the table of contents holds #{capacity}")
      end
      Array.new([node[:nkeys], capacity].min) do |i|
        locate(oid, bytes, toc + i * width, fixed:, leaf: node[:level].zero?, info:, area: keys...values_end)
      end.compact
    end

    def locate(oid, bytes, at, fixed:, leaf:, info:, area:)
      # A fixed-size entry (kvoff_t) holds two offsets and takes its lengths
      # from the footer; an index value is a child OID. A variable entry
      # (kvloc_t) holds offset and length of both.
      if fixed
        k_off, v_off = bytes.unpack("@#{at}S<S<")
        k_len = info[:key_size]
        v_len = leaf ? info[:val_size] : 8
      else
        k_off, k_len, v_off, v_len = bytes.unpack("@#{at}S<S<S<S<")
      end
      key_at = area.begin + k_off
      return problem("node #{oid}: key at #{key_at}+#{k_len} leaves the key area") unless inside?(area, key_at, k_len)
      if v_off == BTOFF_INVALID
        return problem("node #{oid}: ghost record in a tree that does not allow ghosts") unless @ghosts

        return Entry.new(key: bytes.byteslice(key_at, k_len), value: nil)
      end

      value_at = area.end - v_off
      return problem("node #{oid}: value at #{value_at}+#{v_len} leaves the value area") unless inside?(area, value_at, v_len)

      Entry.new(key: bytes.byteslice(key_at, k_len), value: bytes.byteslice(value_at, v_len))
    end

    def inside?(area, at, len) = at >= area.begin && at + len <= area.end

    def node_summary(obj, node, info:, root:, entries:)
      summary = obj.document({ **Layout.with_names(node, flags: NODE_FLAGS), entries: entries.size })
      summary[:info] = Layout.with_names(info, flags: TREE_FLAGS) if root
      summary
    end

    def leaf(entries)
      entries.each do |entry|
        next problem("a #{entry.key.bytesize}/#{entry.value&.bytesize}-byte record is not a #{@codec.name} record") unless fits?(entry)
        break full if @decoded >= MAX_TREE_RECORDS

        @decoded += 1
        @each.(@codec.record.(entry.key, entry.value))
      end
    end

    # Reports, once per walk, that the record limit stopped keeping records.
    def full
      problem("record limit #{MAX_TREE_RECORDS} reached; further records are not decoded") unless @full
      @full = true
      nil
    end

    # Whether an entry has the codec's fixed sizes; a ghost has no value.
    def fits?(entry)
      return true unless @codec.key_size

      entry.key.bytesize == @codec.key_size && (entry.value.nil? || entry.value.bytesize == @codec.value_size)
    end

    # Visits the child each index entry of node `oid` names. A hashed
    # index value holds the child's OID less the root's and the child's
    # digest, and a sum past 64 bits is a problem, never a wrapped OID;
    # any other holds the child's OID.
    def children(oid, entries, level:, info:)
      entries.each do |entry|
        if @hashed
          size = entry.value.to_s.bytesize
          next problem("node #{oid}: hashed index value of #{size} bytes, expected #{HASHED_INDEX_VALUE_SIZE}") if
            size != HASHED_INDEX_VALUE_SIZE

          relative, digest = entry.value.unpack("Q<a#{SHA256_SIZE}")
          child = @root_oid + relative
          next problem("node #{oid}: child OID #{@root_oid} + #{relative} overflows 64 bits") if child > U64_MASK

          visit(child, level: level - 1, info:, digest:)
        else
          next problem("index entry without a child") if entry.value.nil? || entry.value.bytesize < 8

          visit(entry.value.unpack1("Q<"), level: level - 1, info:, digest: nil)
        end
      end
    end
  end

  # A section boundary for a reader with a #problem method: a failure inside
  # one structure is recorded and the dump goes on with the next.
  module Sections
    private

    def section(name)
      yield
    rescue StandardError => e
      problem("#{name}: #{e.is_a?(Error) ? '' : "#{e.class}: "}#{e.message}")
      { error: e.message }
    end
  end

  # Reads the container layer of an image into one document (nested hashes
  # with stable keys) and collects every problem met on the way.
  class Container
    include Sections

    attr_reader :problems

    # `volume` selects one volume by its slot in the superblock's array of
    # volumes; `records` keeps every volume tree record in the document.
    def initialize(image, volume: nil, records: false)
      @image = image
      @volume = volume
      @records = records
      @problems = []
      @unrecorded = 0
      @ephemeral_bytes = 0
      @ephemeral = {}
      @claimed = Set.new
    end

    def dump
      doc = { image: { size: @image.size } }
      doc[:superblock] = block_zero
      doc[:checkpoints] = section(:checkpoints) { checkpoints }
      doc[:omap] = section(:omap) { container_omap }
      doc[:volumes] = section(:volumes) { volumes }
      doc[:spaceman] = section(:spaceman) { spaceman }
      doc[:reaper] = section(:reaper) { reaper }
      @problems << "#{@unrecorded} further problems are not recorded" if @unrecorded.positive?
      doc[:problems] = @problems
      doc
    end

    private

    # Records a problem; past MAX_PROBLEMS it is only counted, and #dump
    # ends the list with that count.
    def problem(text)
      if @problems.size < MAX_PROBLEMS
        @problems << text
      else
        @unrecorded += 1
      end
      nil
    end

    # Records a problem and returns the section value that stands for it.
    def failure(text, **fields)
      problem(text)
      { **fields, error: text }
    end

    # ---- objects --------------------------------------------------------

    def in_container?(addr, blocks) = addr.positive? && blocks.positive? && addr + blocks <= @block_count

    # Reads the object at `addr`, `blocks` long, and reports a checksum
    # mismatch unless it is `headerless`. Block 0 is read through #block_zero.
    def read_object(addr, what, blocks: 1, headerless: false)
      raise Error, "#{what}: blocks #{addr}+#{blocks} are outside the container (#{@block_count} blocks)" unless in_container?(addr, blocks)

      checked(Obj.from(addr, @image.read(addr * @block_size, blocks * @block_size), headerless:), what)
    end

    def checked(obj, what)
      return obj if obj.headerless || obj.cksum_ok?

      problem(format("%s at block %d: checksum 0x%016x, computed 0x%016x", what, obj.addr, obj.header[:cksum], obj.computed))
      obj
    end

    # Marks the blocks of `span` as read for one structure. Returns false,
    # claiming nothing, when any of them already was. The ephemeral objects
    # and the space manager's chain claim their blocks, and so does every
    # object of a volume (#read_once), so naming one block many times cannot
    # multiply their work; every other read is bounded by a fixed limit of
    # this dumper.
    def claim(span)
      return false if span.any? { @claimed.include?(_1) }

      @claimed.merge(span)
      true
    end

    # Reads an object no other structure has read. A volume reads all its
    # objects so: a block that many trees, volumes or object-map entries
    # name is read and walked once.
    def read_once(addr, what, headerless: false)
      raise Error, "#{what}: block #{addr} is already read for another structure" unless claim(addr..addr)

      read_object(addr, what, headerless:)
    end

    def raw_block(addr, what)
      raise Error, "#{what}: block #{addr} is outside the container (#{@block_count} blocks)" unless in_container?(addr, 1)

      @image.read(addr * @block_size, @block_size)
    end

    def check_header(obj, what, **expected)
      obj.header_problems(**expected).each { problem("#{what} at block #{obj.addr}: #{_1}") }
    end

    def ephemeral(oid, what)
      @ephemeral.fetch(oid) { raise Error, "#{what}: ephemeral oid #{oid} has no mapping in the selected checkpoint" }
    end

    def superblock_fields(obj)
      fields = Layout.decode(obj.bytes, NX_FIELDS)
      Layout.with_names(fields, features: NX_FEATURES, incompatible_features: NX_INCOMPAT, flags: NX_FLAGS)
    end

    # ---- block 0 ---------------------------------------------------------

    # Block 0 fixes the block size and count every later read is judged by;
    # an image without a usable one is not dumped further.
    def block_zero
      @block_size = @image.read(0, MIN_BLOCK_SIZE).unpack1("@36L<")
      unless @block_size.between?(MIN_BLOCK_SIZE, MAX_BLOCK_SIZE) && (@block_size & (@block_size - 1)).zero?
        raise Error, "block 0: block size #{@block_size} is not a power of two from #{MIN_BLOCK_SIZE} to #{MAX_BLOCK_SIZE}"
      end

      obj = Obj.from(0, @image.read(0, @block_size))
      sb = superblock_fields(obj)
      raise Error, format("block 0: magic 0x%08x is not NXSB", sb[:magic]) if sb[:magic] != NX_MAGIC

      problem(format("block 0: checksum 0x%016x, computed 0x%016x", obj.header[:cksum], obj.computed)) unless obj.cksum_ok?
      check_header(obj, "block 0", kind: KIND_NX_SUPERBLOCK, storage: STORAGE_EPHEMERAL, oid: 1)
      @block_count = sb[:block_count]
      if @block_count * @block_size > @image.size
        problem("block 0: the container is #{@block_count * @block_size} bytes, the image holds #{@image.size}")
      end
      @sb = obj.document(sb)
    end

    # ---- checkpoints -----------------------------------------------------

    def checkpoints
      desc_blocks = @sb[:xp_desc_blocks]
      return failure("checkpoints: the descriptor area is noncontiguous; not decoded") if desc_blocks.anybits?(0x8000_0000)

      ring = Array.new([desc_blocks, MAX_RING_BLOCKS].min) { descriptor(_1, @sb[:xp_desc_base] + _1) }
      problem("checkpoints: descriptor area of #{desc_blocks} blocks; only #{MAX_RING_BLOCKS} read") if desc_blocks > MAX_RING_BLOCKS
      latest = select_latest(ring)
      area = ring.map { |slot| slot[:role] == "superblock" ? { **slot.except(:superblock), latest: slot[:index] == latest&.dig(:index) } : slot }
      return { descriptor_area: area, selected: nil } unless latest

      # From here on the sections read the selected checkpoint's superblock.
      @sb = latest[:superblock]
      maps = checkpoint_maps(ring, latest)
      { descriptor_area: area, selected: latest[:index], superblock: @sb,
        objects: ephemeral_objects(maps.flat_map { _1[:mappings] }) }
    end

    # One slot of the descriptor ring: a container superblock, a checkpoint
    # mapping block, a block never written (all zero, not an object), or
    # anything else.
    def descriptor(index, addr)
      bytes = raw_block(addr, "descriptor #{index}")
      return { index:, addr:, role: "empty" } if bytes.count("\0") == bytes.bytesize

      obj = checked(Obj.from(addr, bytes), "descriptor #{index}")
      case obj.kind
      when KIND_NX_SUPERBLOCK then obj.document({ index:, **superblock_slot(obj, "descriptor #{index}") })
      when KIND_CHECKPOINT_MAP then obj.document({ index:, role: "map", **checkpoint_map(obj, "descriptor #{index}") })
      else obj.document({ index:, role: "other" })
      end
    rescue Error => e
      failure("checkpoints: #{e.message}", index:, addr:, role: "unreadable")
    end

    # A checkpoint superblock is a candidate only with block 0's block size,
    # which every read depends on. Block 0 is only typically a copy of the
    # latest superblock, so a different block count (a resize) is shown but
    # does not disqualify it; reads stay bounded by block 0's count.
    def superblock_slot(obj, what)
      sb = superblock_fields(obj)
      same_size = sb[:block_size] == @block_size
      problem("#{what}: block size #{sb[:block_size]} differs from block 0's #{@block_size}") unless same_size
      valid = obj.cksum_ok? && sb[:magic] == NX_MAGIC && same_size
      fields = sb.slice(:magic, :next_xid, :xp_desc_index, :xp_desc_len, :xp_data_index, :xp_data_len)
      { role: "superblock", valid:, **fields, superblock: obj.document(sb) }
    end

    def checkpoint_map(obj, what)
      head = Layout.with_names(Layout.decode(obj.bytes, CHECKPOINT_MAP_FIELDS), flags: CHECKPOINT_FLAGS)
      capacity = (@block_size - 40) / CHECKPOINT_MAPPING_SIZE
      problem("#{what}: #{head[:count]} mappings, the block holds #{capacity}") if head[:count] > capacity
      mappings = Array.new([head[:count], capacity].min) do |i|
        fields = Layout.decode(obj.bytes, CHECKPOINT_MAPPING_FIELDS, base: 40 + i * CHECKPOINT_MAPPING_SIZE)
        { **fields, type_name: Layout.type_name(fields[:type]) }
      end
      { **head, mappings: }
    end

    # The ring slot of the valid superblock with the greatest XID, or nil.
    def select_latest(ring)
      valid = ring.select { _1[:role] == "superblock" && _1[:valid] }
      return problem("checkpoints: no valid superblock in the descriptor area") if valid.empty?

      latest = valid.max_by { _1[:xid] }
      ties = valid.count { _1[:xid] == latest[:xid] }
      problem("checkpoints: #{ties} superblocks share the latest xid #{latest[:xid]}") if ties > 1
      if @sb[:xid] > latest[:xid]
        problem("checkpoints: block 0 xid #{@sb[:xid]} is newer than the latest checkpoint #{latest[:xid]}")
      end
      latest
    end

    # The mapping blocks of the latest checkpoint's descriptor run, which
    # ends at the superblock's own slot, judged from the ring already read.
    def checkpoint_maps(ring, latest)
      sb = latest[:superblock]
      len = sb[:xp_desc_len]
      unless len.between?(1, ring.size)
        problem("checkpoints: descriptor run of #{len} blocks does not fit the ring of #{ring.size}")
        return []
      end

      run = Array.new(len) { ring[(sb[:xp_desc_index] + _1) % ring.size] }
      if run.last[:index] != latest[:index]
        problem("checkpoints: the run ends at slot #{run.last[:index]}, not the superblock's slot #{latest[:index]}")
      end
      maps = run[0...-1]
      maps.each_with_index do |slot, i|
        next problem("checkpoints: run slot #{slot[:index]} is not a mapping block") if slot[:role] != "map"

        problem("checkpoints: mapping block #{slot[:index]} has xid #{slot[:xid]}, the checkpoint #{sb[:xid]}") if slot[:xid] != sb[:xid]
        last = i == maps.size - 1
        problem("checkpoints: mapping block #{slot[:index]} LAST flag is #{!last}") if slot[:flags].anybits?(CHECKPOINT_MAP_LAST) != last
      end
      maps.select { _1[:role] == "map" }
    end

    # Reads each ephemeral object of the selected checkpoint once, keeps it
    # for the sections that resolve ephemeral OIDs, and lists it. Each block
    # of the data area is read for at most one object, so the work is bounded
    # by the data area whatever the mappings say.
    def ephemeral_objects(mappings)
      if @sb[:xp_data_blocks].anybits?(0x8000_0000)
        return [failure("checkpoints: the data area is noncontiguous; its objects are not read")]
      end

      data = @sb[:xp_data_base]...(@sb[:xp_data_base] + @sb[:xp_data_blocks])
      mappings.map { ephemeral_object(_1, data) }
    end

    def ephemeral_object(mapping, data)
      what = "ephemeral oid #{mapping[:oid]}"
      blocks = mapping[:size] / @block_size
      span = mapping[:paddr]...(mapping[:paddr] + blocks)
      if blocks.zero? || (mapping[:size] % @block_size).nonzero? || blocks > MAX_OBJECT_BLOCKS
        return failure("#{what}: size #{mapping[:size]} is not 1 to #{MAX_OBJECT_BLOCKS} whole blocks", **mapping)
      end
      return failure("#{what}: blocks #{span} are outside the checkpoint data area", **mapping) unless data.cover?(span)
      return failure("#{what}: mapped twice", **mapping) if @ephemeral.key?(mapping[:oid])
      if @ephemeral_bytes + mapping[:size] > MAX_EPHEMERAL_BYTES
        return failure("#{what}: ephemeral objects exceed this dumper's #{MAX_EPHEMERAL_BYTES} bytes", **mapping)
      end
      return failure("#{what}: blocks #{span} overlap another object", **mapping) unless claim(span)

      obj = read_object(mapping[:paddr], what, blocks:)
      check_header(obj, what, kind: mapping[:type] & 0xFFFF, storage: STORAGE_EPHEMERAL, subtype: mapping[:subtype],
                              oid: mapping[:oid])
      @ephemeral[mapping[:oid]] = obj
      @ephemeral_bytes += mapping[:size]
      obj.document(mapping.slice(:size, :fs_oid))
    rescue Error => e
      failure("checkpoints: #{e.message}", **mapping)
    end

    # ---- object map and volumes -----------------------------------------

    def container_omap
      obj = read_object(@sb[:omap_oid], "container object map")
      check_header(obj, "container object map", kind: KIND_OMAP, storage: STORAGE_PHYSICAL, oid: obj.addr)
      fields = Layout.with_names(Layout.decode(obj.bytes, OMAP_FIELDS), flags: OMAP_FLAGS)
      tree = physical_tree("container omap tree", OMAP_CODEC, subtype: KIND_OMAP).walk(fields[:tree_oid])
      @omap = ObjectMap.new(tree[:records])
      obj.document({ **fields, tree: })
    end

    def physical_tree(what, codec, subtype:)
      BTree.new(fetch: ->(oid) { read_object(oid, what) }, problem: method(:problem), codec:, what:,
                block_size: @block_size, storage: STORAGE_PHYSICAL, subtype:)
    end

    def ephemeral_tree(what, codec, subtype:)
      BTree.new(fetch: ->(oid) { ephemeral(oid, what) }, problem: method(:problem), codec:, what:,
                block_size: @block_size, storage: STORAGE_EPHEMERAL, subtype:)
    end

    # Each volume superblock OID resolved through the container object map,
    # or only the selected one's, and the volume read from it.
    def volumes
      raise Error, "the container object map is unreadable" unless @omap

      slots = @sb[:fs_oid].each_with_index.reject { |oid, _| oid.zero? }
      slots = slots.select { |_, slot| slot == @volume } if @volume
      problem("volumes: no volume in slot #{@volume}") if slots.empty? && @volume
      slots.map { |oid, slot| section("volume #{slot}") { volume(oid, slot) } }
    end

    def volume(oid, slot)
      what = "volume #{slot} (oid #{oid})"
      found = @omap.find(oid, @sb[:xid])
      return failure("#{what}: not in the container object map", slot:, oid:) unless found
      return failure("#{what}: deleted in the object map", slot:, oid:) if found[:flags].anybits?(OMAP_VAL_DELETED)

      # A volume superblock is one block; a mapping says otherwise is reported
      # and its first block read.
      problem("#{what}: mapped size #{found[:size]}, expected #{@block_size}") if found[:size] != @block_size
      # Slots naming one superblock would multiply the work of its trees.
      return failure("#{what}: block #{found[:paddr]} is already read for another structure", slot:, oid:) unless claim(found[:paddr]..found[:paddr])
      obj = read_object(found[:paddr], what)
      check_header(obj, what, kind: KIND_FS, storage: STORAGE_VIRTUAL, oid:)
      fields = Volume.new(obj, slot:, read: method(:read_once), read_block: method(:raw_block), problem: method(:problem),
                               block_size: @block_size, records: @records).dump
      obj.document({ slot:, omap_xid: found[:xid], **fields })
    rescue Error => e
      failure("volumes: #{e.message}", slot:, oid:)
    end

    # ---- space manager ---------------------------------------------------

    def spaceman
      obj = ephemeral(@sb[:spaceman_oid], "space manager")
      check_header(obj, "space manager", kind: KIND_SPACEMAN, storage: STORAGE_EPHEMERAL, oid: @sb[:spaceman_oid])
      sm = Layout.decode(obj.bytes, SPACEMAN_FIELDS)
      problem("space manager: block size #{sm[:block_size]}, the container's #{@block_size}") if sm[:block_size] != @block_size
      layout = spaceman_layout(obj.bytes, sm)
      obj.document({
        **Layout.with_names(sm.except(:dev, :fq), flags: SM_FLAGS),
        **layout.except(:datazone),
        **layout.slice(:datazone).transform_values { allocation_zones(_1) },
        devices: sm[:dev].each_with_index.map { |dev, i| device(obj, dev:, index: i) },
        internal_pool: section(:internal_pool) { internal_pool(obj, sm) },
        free_queues: sm[:fq].each_with_index.map { |fq, i| free_queue(fq, i) }
      })
    end

    # The reader's layout rule: a structure size of SPACEMAN_CURRENT_END
    # selects the current layout; otherwise arrays from SPACEMAN_OLDER_END
    # select the older one, whose bytes at 336 are its XID array. Anything
    # else is shown as the current layout, structure size and all.
    def spaceman_layout(bytes, sm)
      current = Layout.decode(bytes, SPACEMAN_CURRENT_FIELDS)
      lowest = sm.values_at(:ip_bm_xid_offset, :ip_bitmap_offset, :ip_bm_free_next_offset)
                 .push(sm[:dev].first[:addr_offset]).min
      return { structure_end: SPACEMAN_OLDER_END } if current[:struct_size] != SPACEMAN_CURRENT_END && lowest == SPACEMAN_OLDER_END

      { structure_end: SPACEMAN_CURRENT_END, **current }
    end

    def allocation_zones(zones)
      zones.each_with_index.filter_map do |zone, i|
        next if zone.values.flatten.all?(&:zero?)

        { device: DEVICE_NAMES[i / ALLOCATION_ZONES_PER_DEVICE], index: i % ALLOCATION_ZONES_PER_DEVICE, **zone }
      end
    end

    # A device's chunk-information chain: the address array in the space
    # manager names chunk-information blocks directly when there are no
    # address blocks, else address blocks that name them. Every chunk's
    # bitmap is recounted against its stored free count, and the sum against
    # the device's. Each block of the chain is read once (#claim), so the
    # work is bounded by the image, not by the counts it states.
    def device(obj, dev:, index:)
      name = DEVICE_NAMES[index]
      what = "space manager #{name} device"
      return { name:, **dev, cibs: [] } if dev[:block_count].zero? && dev[:cib_count].zero?
      if [dev[:cib_count], dev[:cab_count]].max > MAX_CHAIN_BLOCKS
        return failure("#{what}: #{dev[:cib_count]} chunk-information and #{dev[:cab_count]} address blocks, " \
                       "above this dumper's #{MAX_CHAIN_BLOCKS}", name:, **dev)
      end

      direct = dev[:cab_count].zero?
      addrs = Layout.array(obj.bytes, off: dev[:addr_offset], count: direct ? dev[:cib_count] : dev[:cab_count], directive: "Q<")
      named = direct ? addrs : addrs.each_with_index.lazy.flat_map { |addr, i| address_block(addr, label: "#{what} address block #{i}") }
      # One address past the stated count is taken to tell "more" from "equal".
      cib_addrs = named.first(dev[:cib_count] + 1)
      problem("#{what}: the chain names more than the device's #{dev[:cib_count]} chunk-information blocks") if cib_addrs.size > dev[:cib_count]
      problem("#{what}: the chain names #{cib_addrs.size} chunk-information blocks, the device says #{dev[:cib_count]}") if cib_addrs.size < dev[:cib_count]
      cibs = cib_addrs.first(dev[:cib_count]).each_with_index.map do |addr, i|
        chunk_info_block(addr, label: "#{what} chunk-information block #{i}")
      end
      recount = cibs.sum { |cib| cib.fetch(:chunks, []).sum { _1[:bitmap_free] || _1[:free_count] } }
      problem("#{what}: bitmaps record #{recount} free blocks, the device says #{dev[:free_count]}") if recount != dev[:free_count]
      { name:, **dev, bitmap_free: recount, cabs: direct ? [] : addrs, cibs: }
    rescue Error => e
      failure("#{what}: #{e.message}", name:, **dev)
    end

    def address_block(addr, label:)
      unless claim(addr..addr)
        problem("#{label}: block #{addr} is already read for another structure")
        return []
      end

      obj = read_object(addr, label)
      check_header(obj, label, kind: KIND_SPACEMAN_CAB, storage: STORAGE_PHYSICAL, oid: addr)
      head = Layout.decode(obj.bytes, ADDRESS_BLOCK_FIELDS)
      capacity = (@block_size - 40) / 8
      problem("#{label}: #{head[:count]} entries, the block holds #{capacity}") if head[:count] > capacity
      Layout.array(obj.bytes, off: 40, count: [head[:count], capacity].min, directive: "Q<")
    rescue Error => e
      problem(e.message)
      []
    end

    def chunk_info_block(addr, label:)
      return failure("#{label}: block #{addr} is already read for another structure", addr:) unless claim(addr..addr)

      obj = read_object(addr, label)
      check_header(obj, label, kind: KIND_SPACEMAN_CIB, storage: STORAGE_PHYSICAL, oid: addr)
      head = Layout.decode(obj.bytes, ADDRESS_BLOCK_FIELDS)
      capacity = (@block_size - 40) / CHUNK_INFO_SIZE
      problem("#{label}: #{head[:count]} chunks, the block holds #{capacity}") if head[:count] > capacity
      chunks = Array.new([head[:count], capacity].min) do |i|
        chunk(Layout.decode(obj.bytes, CHUNK_INFO_FIELDS, base: 40 + i * CHUNK_INFO_SIZE), label: "#{label} chunk #{i}")
      end
      obj.document({ **head, chunks: })
    rescue Error => e
      failure(e.message, addr:)
    end

    # A chunk with no bitmap block is entirely free.
    def chunk(info, label:)
      if info[:bitmap_addr].zero?
        problem("#{label}: no bitmap, but #{info[:free_count]} of #{info[:block_count]} blocks free") if info[:free_count] != info[:block_count]
        return { **info, bitmap_free: nil }
      end
      unless claim(info[:bitmap_addr]..info[:bitmap_addr])
        return failure("#{label}: bitmap block #{info[:bitmap_addr]} is already read for another structure", **info, bitmap_free: nil)
      end

      bits = [info[:block_count], @block_size * 8].min
      problem("#{label}: #{info[:block_count]} blocks, more than a bitmap block covers") if info[:block_count] > bits
      free = Layout.zero_bits(raw_block(info[:bitmap_addr], label), bits)
      problem("#{label}: bitmap records #{free} free blocks, the chunk says #{info[:free_count]}") if free != info[:free_count]
      { **info, bitmap_free: free }
    rescue Error => e
      failure(e.message, **info, bitmap_free: nil)
    end

    # The internal pool's bitmap ring. The reference names these fields but
    # does not describe them; this reading is that the space manager holds,
    # at the three offsets, one XID and one u16 ring index per block of the
    # current pool bitmap (ip_bm_size_in_blocks of each) and a u16 free-list
    # successor per ring block (ip_bm_block_count), ended by 0xffff.
    def internal_pool(obj, sm)
      size = sm[:ip_bm_size_in_blocks]
      ring = sm[:ip_bm_block_count]
      raise Error, "internal pool: ring of #{ring} blocks is above #{IP_BM_BLOCK_COUNT_MAX}" if ring > IP_BM_BLOCK_COUNT_MAX
      raise Error, "internal pool: bitmap of #{size} blocks is larger than its ring of #{ring}" if size > ring

      xids = Layout.array(obj.bytes, off: sm[:ip_bm_xid_offset], count: size, directive: "Q<")
      indexes = Layout.array(obj.bytes, off: sm[:ip_bitmap_offset], count: size, directive: "S<")
      free_next = Layout.array(obj.bytes, off: sm[:ip_bm_free_next_offset], count: ring, directive: "S<")
      per_block = @block_size * 8
      covered = [sm[:ip_block_count], size * per_block].min
      problem("internal pool: #{sm[:ip_block_count]} blocks, its bitmap covers #{covered}") if covered < sm[:ip_block_count]
      # Block j of the bitmap holds bits j * per_block onward; it is read only
      # when some of those bits cover the pool.
      free = indexes.each_with_index.sum do |ring_index, j|
        raise Error, "internal pool: bitmap index #{ring_index} is outside the ring of #{ring}" if ring_index >= ring

        bits = (covered - j * per_block).clamp(0, per_block)
        bits.zero? ? 0 : Layout.zero_bits(raw_block(sm[:ip_bm_base] + ring_index, "internal-pool bitmap"), bits)
      end
      { bitmap_xids: xids, bitmap_indexes: indexes, free_next:, used: covered - free, free: }
    end

    def free_queue(fq, index)
      name = FREE_QUEUE_NAMES[index]
      return { name:, **fq, tree: nil } if fq[:tree_oid].zero?

      what = "#{name} free queue"
      tree = ephemeral_tree("#{what} tree", FREE_QUEUE_CODEC, subtype: KIND_SPACEMAN_FREE_QUEUE).walk(fq[:tree_oid])
      queued = tree[:records].sum { _1[:count] }
      problem("#{what}: records hold #{queued} blocks, the queue says #{fq[:count]}") if queued != fq[:count]
      { name:, **fq, queued:, tree: }
    end

    # ---- reaper ----------------------------------------------------------

    def reaper
      obj = ephemeral(@sb[:reaper_oid], "reaper")
      check_header(obj, "reaper", kind: KIND_NX_REAPER, storage: STORAGE_EPHEMERAL, oid: @sb[:reaper_oid])
      fields = Layout.with_names(Layout.decode(obj.bytes, REAPER_FIELDS), flags: REAPER_FLAGS, nrle_flags: NRLE_FLAGS)
      room = obj.bytes.bytesize - REAPER_STATE_OFFSET
      problem("reaper: state buffer of #{fields[:state_buffer_size]} bytes, the object holds #{room}") if fields[:state_buffer_size] > room
      # The state buffer in hexadecimal, its trailing zero bytes left out.
      state = obj.bytes.byteslice(REAPER_STATE_OFFSET, [fields[:state_buffer_size], room].min)
      state = state.byteslice(0, (state.rindex(/[^\0]/n) || -1) + 1)
      obj.document({ **fields, state_buffer: state.unpack1("H*"), reap_lists: reap_lists(fields[:head]) })
    end

    # The reap lists from the head, each an ephemeral object naming the next.
    def reap_lists(head)
      seen = Set.new
      oid = head
      lists = []
      until oid.zero?
        break problem("reaper: reap list #{oid} is reached twice") unless seen.add?(oid)
        break problem("reaper: more than #{MAX_REAP_LISTS} reap lists") if lists.size >= MAX_REAP_LISTS

        list = reap_list(oid)
        lists << list
        oid = list.fetch(:next, 0)
      end
      lists
    end

    def reap_list(oid)
      what = "reap list #{oid}"
      obj = ephemeral(oid, what)
      check_header(obj, what, kind: KIND_NX_REAP_LIST, storage: STORAGE_EPHEMERAL, oid:)
      fields = Layout.decode(obj.bytes, REAP_LIST_FIELDS)
      capacity = (obj.bytes.bytesize - REAP_LIST_ENTRIES_OFFSET) / REAP_LIST_ENTRY_SIZE
      problem("#{what}: room for #{fields[:max]} entries, the object holds #{capacity}") if fields[:max] > capacity
      entries = Array.new([fields[:max], capacity].min) do |i|
        entry = Layout.decode(obj.bytes, REAP_LIST_ENTRY_FIELDS, base: REAP_LIST_ENTRIES_OFFSET + i * REAP_LIST_ENTRY_SIZE)
        { index: i, **Layout.with_names(entry, flags: NRLE_FLAGS) } unless entry.values.all?(&:zero?)
      end
      obj.document({ **fields, entries: entries.compact })
    rescue Error => e
      failure(e.message, oid:)
    end
  end

  # The records of a sealed volume's file extent tree: a file's private ID
  # and logical address, mapped to a length (flags in the top eight bits)
  # and a physical block. A ghost record has no value. The type name, which
  # file-system records carry too, is what the volume summary counts.
  FEXT_CODEC = Codec.new(
    name: "file extent tree", key_size: 16, value_size: 16,
    record: lambda do |key, value|
      private_id, logical_addr = key.unpack("Q<Q<")
      len_and_flags, phys_block_num = value ? value.unpack("Q<Q<") : [nil, nil]
      { type_name: "FEXT", private_id:, logical_addr:, length: len_and_flags && len_and_flags & FILE_EXTENT_LEN_MASK,
        extent_flags: len_and_flags && len_and_flags >> FILE_EXTENT_FLAG_SHIFT, phys_block_num: }
    end
  )

  # Decoding of file-system records, which the file-system tree, the
  # extent-reference tree and the snapshot metadata tree all hold: a j_key_t
  # header, whose top four bits are the record type and the rest an object
  # ID, then the rest of the key and the value as that type lays them out.
  # A record that breaks a rule carries a `problems` list; one whose bytes
  # end before a field carries an `error` instead of its fields. The name
  # hash of a directory record is shown as stored, not verified.
  module Records
    XF_BLOB_FIELDS = [[:num_exts, 0, "S<"], [:used_data, 2, "S<"]].freeze
    X_FIELD_FIELDS = [[:type, 0, "C"], [:flags, 1, "C"], [:size, 2, "S<"]].freeze
    XF_BLOB_SIZE = 4
    X_FIELD_SIZE = 4
    DREC_VAL_FIELDS = [[:file_id, 0, "Q<"], [:date_added, 8, "Q<"], [:flags, 16, "S<"]].freeze
    XATTR_VAL_FIELDS = [[:flags, 0, "S<"], [:xdata_len, 2, "S<"]].freeze
    XATTR_VAL_SIZE = 4
    PHYS_EXT_FIELDS = [[:len_and_kind, 0, "Q<"], [:owning_obj_id, 8, "Q<"], [:refcnt, 16, "l<"]].freeze
    FILE_EXTENT_FIELDS = [[:len_and_flags, 0, "Q<"], [:phys_block_num, 8, "Q<"], [:crypto_id, 16, "Q<"]].freeze
    SNAP_METADATA_NAME_AT = 50
    FILE_DATA_HASH_FIELDS = [[:hashed_len, 0, "S<"], [:hash_size, 2, "C"]].freeze
    FILE_DATA_HASH_AT = 3
    # Key and value sizes of the record types whose layout fixes them; nil
    # where a name or extended fields make the size vary.
    SIZES = {
      J_TYPE_SNAP_METADATA => [8, nil], J_TYPE_EXTENT => [8, 20], J_TYPE_INODE => [8, nil],
      J_TYPE_SIBLING_LINK => [16, nil], J_TYPE_DSTREAM_ID => [8, 4], J_TYPE_CRYPTO_STATE => [8, nil],
      J_TYPE_FILE_EXTENT => [16, 24], J_TYPE_DIR_STATS => [8, 32], J_TYPE_SNAP_NAME => [nil, 8],
      J_TYPE_SIBLING_MAP => [8, 8], J_TYPE_FILE_INFO => [16, nil]
    }.freeze

    module_function

    # The codec of a volume's file-system trees; `hashed` selects the
    # directory-record key with a name hash.
    def codec(hashed:)
      Codec.new(name: "file-system", key_size: nil, value_size: nil,
                record: ->(key, value) { decode(key, value || "".b, hashed:) })
    end

    def decode(key, value, hashed:)
      head = Layout.field(key, 0, "Q<")
      type = head >> OBJ_TYPE_SHIFT
      base = { obj_id: head & OBJ_ID_MASK, type:, type_name: J_TYPES.fetch(type) { "unknown(#{type})" } }
      fields = body(type, key, value, hashed:)
      problems = size_problems(type, key:, value:) + fields.fetch(:problems, [])
      { **base, **fields.except(:problems), **(problems.empty? ? {} : { problems: }) }
    rescue Error => e
      { **(base || { obj_id: nil, type: nil, type_name: "unreadable" }), error: e.message }
    end

    def body(type, key, value, hashed:)
      case type
      when J_TYPE_SNAP_METADATA then snap_metadata(value)
      when J_TYPE_EXTENT then phys_extent(value)
      when J_TYPE_INODE then inode(value)
      when J_TYPE_XATTR then xattr(key, value)
      when J_TYPE_SIBLING_LINK then sibling_link(key, value)
      when J_TYPE_DSTREAM_ID then { refcnt: Layout.field(value, 0, "L<") }
      when J_TYPE_CRYPTO_STATE then { value_size: value.bytesize } # identified, not decoded
      when J_TYPE_FILE_EXTENT then file_extent(key, value)
      when J_TYPE_DIR_REC then drec(key, value, hashed:)
      when J_TYPE_DIR_STATS then Layout.decode(value, DIR_STATS_FIELDS)
      when J_TYPE_SNAP_NAME then snap_name(key, value)
      when J_TYPE_SIBLING_MAP then { file_id: Layout.field(value, 0, "Q<") }
      when J_TYPE_FILE_INFO then file_info(key, value)
      else { key_size: key.bytesize, value_size: value.bytesize }
      end
    end

    def size_problems(type, key:, value:)
      key_size, value_size = SIZES.fetch(type, [nil, nil])
      [("key of #{key.bytesize} bytes, expected #{key_size}" if key_size && key.bytesize != key_size),
       ("value of #{value.bytesize} bytes, expected #{value_size}" if value_size && value.bytesize != value_size)].compact
    end

    # A name of `len` bytes, its NUL included, that ends the structure in
    # `bytes` at `at`: the name and the rules it breaks.
    def name(bytes, at:, len:)
      raw = Layout.slice(bytes, at, len)
      problems = [("name of #{len} bytes does not end in NUL" unless raw.end_with?("\0")),
                  ("#{bytes.bytesize - at - len} bytes follow the name" if bytes.bytesize != at + len)]
      [Layout.text(raw), problems.compact]
    end

    def inode(value)
      fields = Layout.decode(value, INODE_FIELDS)
      { **Layout.with_names(fields, internal_flags: INODE_FLAGS),
        file_type: MODE_TYPES.fetch(fields[:mode] & S_IFMT, "unknown"),
        **xfields(value, at: INODE_VAL_SIZE, types: INODE_XFIELDS) }
    end

    # A directory entry. Its key holds the name length and, in a volume
    # with hashed keys, a 22-bit name hash above it; its value the target's
    # file ID, the date added, a flags word whose low four bits are the
    # entry's type, and extended fields.
    def drec(key, value, hashed:)
      if hashed
        word = Layout.field(key, 8, "L<")
        len = word & J_DREC_LEN_MASK
        name, problems = name(key, at: 12, len:)
        key_fields = { name_len: len, name_hash: word >> J_DREC_HASH_SHIFT, name: }
      else
        len = Layout.field(key, 8, "S<")
        name, problems = name(key, at: 10, len:)
        key_fields = { name_len: len, name: }
      end
      val = Layout.decode(value, DREC_VAL_FIELDS)
      xf = xfields(value, at: DREC_VAL_SIZE, types: DREC_XFIELDS)
      { **key_fields, **val, dir_type: DT_TYPES.fetch(val[:flags] & DREC_TYPE_MASK, "unknown"), **xf,
        problems: problems + xf.fetch(:problems, []) }
    end

    # The extended fields from `at` to the end of `bytes`: an xf_blob_t
    # (field count and bytes of data), one x_field_t per field, then each
    # field's data in the same order, each padded to a multiple of eight
    # bytes. The count is trusted only as far as the bytes hold headers.
    # xf_used_data is compared with the padded data alone: the reference
    # says it also counts the x_field_t headers, but every native record
    # seen stores the data size (a root inode's one 5-byte name field: 8).
    def xfields(bytes, at:, types:)
      return { xfields: [] } if bytes.bytesize == at

      blob = Layout.decode(bytes, XF_BLOB_FIELDS, base: at)
      room = (bytes.bytesize - at - XF_BLOB_SIZE) / X_FIELD_SIZE
      heads = Array.new([blob[:num_exts], room].min) { Layout.decode(bytes, X_FIELD_FIELDS, base: at + XF_BLOB_SIZE + _1 * X_FIELD_SIZE) }
      data_at = at + XF_BLOB_SIZE + heads.size * X_FIELD_SIZE
      # Where each field's data starts, then where the last one ends.
      offsets = heads.each_with_object([data_at]) { |head, offs| offs << offs.last + pad8(head[:size]) }
      data_end = offsets.last
      fields = heads.zip(offsets).map { |head, off| xfield(bytes, head:, at: off, types:) }
      used = data_end - data_at
      repeated = heads.map { _1[:type] }.tally.select { |_, n| n > 1 }.keys
      problems = [
        ("#{blob[:num_exts]} extended fields, room for #{room} headers" if blob[:num_exts] > room),
        ("extended-field data takes #{used} bytes, the blob says #{blob[:used_data]}" if used != blob[:used_data]),
        ("extended-field data ends at #{data_end}, the record at #{bytes.bytesize}" if data_end != bytes.bytesize),
        ("extended-field types #{repeated.join(', ')} repeat" unless repeated.empty?)
      ].compact + fields.filter_map { "extended field #{_1[:type]}: #{_1[:error]}" if _1[:error] }
      { xf_num_exts: blob[:num_exts], xf_used_data: blob[:used_data], xfields: fields, problems: }
    end

    def pad8(size) = (size + 7) & ~7

    def xfield(bytes, head:, at:, types:)
      name, codec = types.fetch(head[:type]) { [:"unknown_#{head[:type]}", :hex] }
      field = { type: head[:type], name:, flags: head[:flags], flags_names: Layout.names(XF_FLAGS, head[:flags]), size: head[:size] }
      data = Layout.slice(bytes, at, head[:size])
      { **field, value: xfield_value(data, codec) }
    rescue Error => e
      { **field, error: e.message }
    end

    def xfield_value(data, codec)
      case codec
      when :text then Layout.text(data)
      when :hex then data.unpack1("H*")
      else Layout.field(data, 0, codec)
      end
    end

    # An extended attribute: its data inline or in a data stream, as its
    # flags say. Inline data is shown as a symbolic link's target, as a
    # decmpfs header, or as its leading bytes.
    def xattr(key, value)
      len = Layout.field(key, 8, "S<")
      name, problems = name(key, at: 10, len:)
      head = Layout.decode(value, XATTR_VAL_FIELDS)
      stream, embedded = [XATTR_DATA_STREAM, XATTR_DATA_EMBEDDED].map { head[:flags].anybits?(_1) }
      data = Layout.slice(value, XATTR_VAL_SIZE, head[:xdata_len])
      problems += [("flags name #{stream ? 'both' : 'neither'} a data stream and inline data" if stream == embedded),
                   ("#{value.bytesize - XATTR_VAL_SIZE - data.bytesize} bytes follow the data" if value.bytesize != XATTR_VAL_SIZE + data.bytesize)].compact
      { name_len: len, name:, **Layout.with_names(head, flags: XATTR_FLAGS), **xattr_data(name, data, stream:), problems: }
    end

    def xattr_data(name, data, stream:)
      if stream then { stream_oid: Layout.field(data, 0, "Q<"), dstream: Layout.decode(data, DSTREAM_FIELDS, base: 8) }
      elsif name == SYMLINK_EA_NAME then { target: Layout.text(data) }
      elsif name == DECMPFS_EA_NAME
        header = Layout.decode(data, DECMPFS_FIELDS)
        { decmpfs: { **header, magic_ok: header[:magic] == DECMPFS_MAGIC } }
      else { data_hex: data.byteslice(0, XATTR_DATA_SHOWN).unpack1("H*") }
      end
    end

    def sibling_link(key, value)
      parent_id, len = Layout.field(value, 0, "Q<"), Layout.field(value, 8, "S<")
      name, problems = name(value, at: 10, len:)
      { sibling_id: Layout.field(key, 8, "Q<"), parent_id:, name_len: len, name:, problems: }
    end

    # A file extent: the key's logical address, the value's length (flags in
    # the top eight bits), physical block and crypto ID.
    def file_extent(key, value)
      val = Layout.decode(value, FILE_EXTENT_FIELDS)
      { logical_addr: Layout.field(key, 8, "Q<"), length: val[:len_and_flags] & FILE_EXTENT_LEN_MASK,
        extent_flags: val[:len_and_flags] >> FILE_EXTENT_FLAG_SHIFT, **val.except(:len_and_flags) }
    end

    # A physical extent of the extent-reference tree; its object ID is the
    # extent's first block.
    def phys_extent(value)
      val = Layout.decode(value, PHYS_EXT_FIELDS)
      kind = val[:len_and_kind] >> PEXT_KIND_SHIFT
      { length: val[:len_and_kind] & PEXT_LEN_MASK, kind:, kind_name: J_KINDS.fetch(kind) { "unknown(#{kind})" },
        **val.except(:len_and_kind) }
    end

    def snap_metadata(value)
      fields = Layout.decode(value, SNAP_METADATA_FIELDS)
      name, problems = name(value, at: SNAP_METADATA_NAME_AT, len: fields[:name_len])
      { **Layout.with_names(fields, snap_flags: SNAP_META_FLAGS), name:, problems: }
    end

    def snap_name(key, value)
      len = Layout.field(key, 8, "S<")
      name, problems = name(key, at: 10, len:)
      { name_len: len, name:, snap_xid: Layout.field(value, 0, "Q<"), problems: }
    end

    # A file-info record of a sealed volume: the key's info type (top eight
    # bits) and logical block address, the value's data hash.
    def file_info(key, value)
      word = Layout.field(key, 8, "Q<")
      info_type = word >> FILE_EXTENT_FLAG_SHIFT
      hash = Layout.decode(value, FILE_DATA_HASH_FIELDS)
      { info_type:, info_type_name: FILE_INFO_TYPES.fetch(info_type) { "unknown(#{info_type})" },
        lba: word & FILE_EXTENT_LEN_MASK, **hash, hash: Layout.slice(value, FILE_DATA_HASH_AT, hash[:hash_size]).unpack1("H*") }
    end
  end

  # Counts of a volume's records as its trees are walked: records by type
  # across all its trees (each type lives in one tree), inodes by file
  # type, and the bytes their data streams and file extents hold.
  class Summary
    def initialize
      @records = Hash.new(0)
      @inodes = Hash.new(0)
      @totals = Hash.new(0)
    end

    def add(record)
      @records[record[:type_name]] += 1
      case record
      in { type: J_TYPE_INODE, file_type:, xfields: }
        @inodes[file_type] += 1
        xfields.each { add_dstream(_1[:value]) if _1[:type] == INO_EXT_TYPE_DSTREAM && _1[:value].is_a?(Hash) }
      in { type: J_TYPE_FILE_EXTENT, length: } then @totals[:file_extent_bytes] += length
      in { type_name: "FEXT", length: Integer => length } then @totals[:fext_bytes] += length
      in { type: J_TYPE_XATTR, flags: } then @totals[flags.anybits?(XATTR_DATA_STREAM) ? :xattrs_in_streams : :xattrs_inline] += 1
      else nil
      end
    end

    def to_h = { records: @records.values.sum, by_type: @records.sort.to_h, inodes: @inodes.sort.to_h, **@totals.sort.to_h }

    private

    def add_dstream(dstream)
      @totals[:dstream_size] += dstream[:size]
      @totals[:dstream_alloced_size] += dstream[:alloced_size]
    end
  end

  # Reads one volume: every field of its superblock, its object map, and the
  # trees it names, each record decoded. Virtual OIDs resolve through the
  # volume object map at the superblock's XID. `read` reads a physical
  # object, once, and checks its checksum, `read_block` reads a block as it
  # is, and
  # `problem` records a problem; all three are the container's. With
  # `records` every tree node and record is in the document; without, each
  # tree shows its root node and counts, and the summary counts the
  # records. The decmpfs header of each compressed file is listed either
  # way, read from the first block of its data stream when it is not
  # inline: the walks note which streams decmpfs attributes name and where
  # every stream's extent at offset 0 starts, and the headers are read
  # once all trees are walked, whatever order the records came in.
  #
  # Each snapshot metadata record is kept, with or without `records`, and
  # once the live trees are walked each snapshot is shown with the volume
  # superblock it names and its own extent-reference tree. A snapshot's
  # trees are views of the past, so their records are checked but stay out
  # of the summary and the decmpfs headers, which describe the live volume.
  class Volume
    include Sections

    # The fields of a snapshot metadata record a snapshot shows.
    SNAPSHOT_META = %i[
      name snap_flags snap_flags_names create_time change_time inum sblock_oid extentref_tree_oid extentref_tree_type
    ].freeze
    # The fields of a snapshot metadata record and of its superblock that
    # its line shows without `records`: what they point at and count.
    SNAPSHOT_META_BRIEF = %i[name snap_flags sblock_oid extentref_tree_oid].freeze
    SNAPSHOT_SUPERBLOCK_BRIEF = %i[omap_oid root_tree_oid snap_meta_ext_oid num_files num_directories num_snapshots fs_alloc_count].freeze

    def initialize(obj, slot:, read:, read_block:, problem:, block_size:, records:)
      @obj = obj
      @label = "volume #{slot}"
      @read = read
      @read_block = read_block
      @problem = problem
      @block_size = block_size
      @records = records
      @summary = Summary.new
      @compressed = []
      # The snapshot metadata records, in tree order.
      @snapshots = []
      # The volume object map's fields and the XIDs of its snapshot records,
      # nil when either is unreadable.
      @omap_fields = nil
      @omap_snapshot_xids = nil
      # The SHA-256 root hash of a sealed volume's integrity metadata, nil
      # when there is none to check the file-system tree's root against.
      @root_hash = nil
      # Data-stream IDs that decmpfs attributes name, each to its inode.
      @decmpfs_streams = {}
      # The first physical block of each data stream's extent at offset 0.
      @first_blocks = {}
    end

    def dump
      sb = Layout.with_names(Layout.decode(@obj.bytes, APFS_FIELDS),
                             features: VOL_FEATURES, readonly_compatible_features: VOL_ROCOMPAT,
                             incompatible_features: VOL_INCOMPAT, fs_flags: VOL_FS_FLAGS)
      problem(format("magic 0x%08x is not APSB", sb[:magic])) if sb[:magic] != APFS_MAGIC
      # Directory-record keys carry a name hash in a case- or
      # normalization-insensitive volume.
      name_hashed = sb[:incompatible_features].anybits?(VOL_INCOMPAT_CASE_INSENSITIVE | VOL_INCOMPAT_NORMALIZATION_INSENSITIVE)
      fs = Records.codec(hashed: name_hashed)
      sealed_problems(sb)
      omap = section(:omap) { object_map(sb[:omap_oid]) }
      # Before the file-system tree, whose root hash it holds.
      integrity_meta = section(:integrity_meta) { integrity_meta(sb[:integrity_meta_oid]) }
      fs_tree = section(:fs_tree) do
        tree("file-system tree", oid: sb[:root_tree_oid], type: sb[:root_tree_type], subtype: KIND_FSTREE, codec: fs,
                                 digest: @root_hash)
      end
      extentref_tree = section(:extentref_tree) do
        tree("extent-reference tree", oid: sb[:extentref_tree_oid], type: sb[:extentref_tree_type],
                                      subtype: KIND_BLOCKREFTREE, codec: fs)
      end
      # Only this tree's metadata records are snapshots.
      meta_tree = section(:snap_meta_tree) do
        tree("snapshot metadata tree", oid: sb[:snap_meta_tree_oid], type: sb[:snap_meta_tree_type],
                                       subtype: KIND_SNAPMETATREE, codec: fs,
                                       each: ->(record) { @snapshots << record if record[:type] == J_TYPE_SNAP_METADATA })
      end
      {
        **sb, role_name: VOL_ROLES.fetch(sb[:role]) { format("unknown(0x%x)", _1) }, drec_keys: name_hashed ? "hashed" : "plain",
        omap:, fs_tree:, extentref_tree:,
        snap_meta_tree: meta_tree,
        # After the live trees and the object map, which it reads.
        snapshots: section(:snapshots) { snapshots(sb, fs, meta_read: meta_tree.nil? || meta_tree[:node_count].to_i.positive?) },
        fext_tree: section(:fext_tree) do
          tree("file extent tree", oid: sb[:fext_tree_oid], type: sb[:fext_tree_type], subtype: KIND_FEXT_TREE, codec: FEXT_CODEC,
                                   each: fext_rules)
        end,
        integrity_meta:,
        summary: section(:summary) { @summary.to_h },
        compressed: section(:compressed) { compressed }
      }
    end

    private

    # Records `text` as a problem of this volume and returns it.
    def problem(text) = "#{@label} #{text}".tap { @problem.(_1) }

    def object_map(oid)
      what = "#{@label} object map"
      obj = @read.(oid, what)
      obj.header_problems(kind: KIND_OMAP, storage: STORAGE_PHYSICAL, oid: obj.addr).each { problem("object map: #{_1}") }
      fields = Layout.with_names(Layout.decode(obj.bytes, OMAP_FIELDS), flags: OMAP_FLAGS)
      walk = btree("object map tree", OMAP_CODEC, storage: STORAGE_PHYSICAL, subtype: KIND_OMAP).walk(fields[:tree_oid])
      @omap = ObjectMap.new(walk[:records])
      @omap_fields = fields
      obj.document({ **fields, tree: tree_doc(fields[:tree_oid], nodes: walk[:nodes], kept: walk[:records], count: walk[:records].size),
                     snapshot_tree: section(:omap_snapshot_tree) { omap_snapshot_tree(fields) } })
    end

    # The object map's snapshot tree: one record per snapshot, keyed by its
    # XID, which #snapshots compares with the metadata records.
    def omap_snapshot_tree(fields)
      oid = fields[:snapshot_tree_oid]
      if oid.zero?
        @omap_snapshot_xids = []
        return nil
      end

      what = "object map snapshot tree"
      problem(format("%s: type 0x%x is not a physical B-tree", what, fields[:snapshot_tree_type])) if
        fields[:snapshot_tree_type] != (STORAGE_PHYSICAL | KIND_BTREE)
      walk = btree(what, OMAP_SNAPSHOT_CODEC, storage: STORAGE_PHYSICAL, subtype: KIND_OMAP_SNAPSHOT).walk(oid)
      # A root that could not be read leaves the XIDs unknown.
      @omap_snapshot_xids = walk[:records].map { _1[:xid] } unless walk[:nodes].empty?
      tree_doc(oid, nodes: walk[:nodes], kept: walk[:records], count: walk[:records].size)
    end

    # Walks the tree whose root is `oid` (none when zero); `type` is the
    # superblock's object type for it, which says whether `oid` is virtual
    # or physical. Each record has its problems reported, goes to `each`
    # and, in a `live` tree, to the summary; it is kept only with `records`.
    # `digest` is the SHA-256 the root must have.
    def tree(what, oid:, type:, subtype:, codec:, live: true, each: nil, digest: nil)
      return nil if oid.zero?

      problem(format("%s: type 0x%x is not a B-tree", what, type)) if (type & 0xFFFF) != KIND_BTREE
      kept = []
      count = 0
      walk = btree(what, codec, storage: type & STORAGE_MASK, subtype:).walk(oid, digest:) do |record|
        count += 1
        note(what, record, live:)
        each&.(record)
        kept << record if @records
      end
      tree_doc(oid, nodes: walk[:nodes], kept:, count:)
    end

    # Every snapshot the metadata records name, in XID order, with its
    # superblock and extent-reference tree. `meta_read` is false when the
    # snapshot metadata tree has a root that could not be read, and then
    # nothing is compared with its records, which would all seem missing.
    def snapshots(sb, codec, meta_read:)
      snapshot_counts(sb, @snapshots.map { _1[:obj_id] }) if meta_read
      @snapshots.map { |meta| section("snapshot #{meta[:obj_id]}") { snapshot(meta, codec) } }
    end

    # Compares the metadata records' XIDs with the superblock's snapshot
    # count and, when the object map and its snapshot tree were read, with
    # the map's snapshot records, count and most recent snapshot. A record
    # flagged deleted is compared like any other, and an empty map's most
    # recent snapshot is taken to be 0: no capture shows a snapshot being
    # deleted, so a volume caught in one may report a difference here.
    def snapshot_counts(sb, xids)
      problem("snapshots: superblock counts #{sb[:num_snapshots]}, metadata records #{xids.size}") if sb[:num_snapshots] != xids.size
      return unless @omap_fields && @omap_snapshot_xids

      omap_xids = @omap_snapshot_xids
      unmapped = xids - omap_xids
      unnamed = omap_xids - xids
      problem("snapshots: #{unmapped.size} XIDs, first #{unmapped.first}, have no object-map snapshot record") unless unmapped.empty?
      problem("snapshots: #{unnamed.size} object-map snapshot records, first #{unnamed.first}, have no metadata record") unless unnamed.empty?
      count, recent = @omap_fields.values_at(:snap_count, :most_recent_snap)
      problem("snapshots: object map counts #{count}, snapshot records #{omap_xids.size}") if count != omap_xids.size
      newest = omap_xids.max || 0
      problem("snapshots: object map's most recent snapshot #{recent}, newest record #{newest}") if recent != newest
    end

    # A snapshot with `records`: its metadata, every field of its superblock,
    # the mapping of its file-system root and its extent-reference tree.
    # Without, one line of scalars: the superblock's header XID, checksum
    # and counts, the root's block and the extent-reference tree's counts.
    # A metadata record too short to decode names nothing to read.
    def snapshot(meta, codec)
      xid = meta[:obj_id]
      return { xid:, error: "metadata record unreadable: #{meta[:error]}" } if meta[:error]

      sb = snapshot_superblock(meta[:sblock_oid], xid)
      root = sb[:root_tree_oid]&.positive? ? root_mapping(sb[:root_tree_oid], xid) : nil
      refs = tree("snapshot #{xid} extent-reference tree", oid: meta[:extentref_tree_oid], type: meta[:extentref_tree_type],
                                                           subtype: KIND_BLOCKREFTREE, codec:, live: false)
      return { xid:, **meta.slice(*SNAPSHOT_META), superblock: sb, root_tree: root, extentref_tree: refs } if @records

      { xid:, **meta.slice(*SNAPSHOT_META_BRIEF), superblock_xid: sb[:xid], superblock_cksum_ok: sb[:cksum_ok],
        **(sb[:error] ? { superblock_error: sb[:error] } : {}), **sb.slice(*SNAPSHOT_SUPERBLOCK_BRIEF),
        root_tree_paddr: root&.dig(:paddr), extentref_nodes: refs&.dig(:node_count), extentref_records: refs&.dig(:record_count) }
    end

    # The volume superblock a snapshot names: a physical object at `oid`,
    # or the reason it cannot be read.
    def snapshot_superblock(oid, xid)
      what = "snapshot #{xid} superblock"
      obj = @read.(oid, what)
      obj.header_problems(kind: KIND_FS, storage: STORAGE_PHYSICAL, oid: obj.addr).each { problem("#{what}: #{_1}") }
      fields = Layout.with_names(Layout.decode(obj.bytes, APFS_FIELDS),
                                 features: VOL_FEATURES, readonly_compatible_features: VOL_ROCOMPAT,
                                 incompatible_features: VOL_INCOMPAT, fs_flags: VOL_FS_FLAGS)
      problem(format("%s: magic 0x%08x is not APSB", what, fields[:magic])) if fields[:magic] != APFS_MAGIC
      obj.document(fields)
    rescue Error => e
      { error: problem(e.message) }
    end

    # Where the volume object map puts a snapshot's file-system root at the
    # snapshot's XID; the block is not read, since it may be a node the live
    # tree shares.
    def root_mapping(oid, xid)
      found = @omap&.find(oid, xid)
      return { oid:, error: problem("snapshot #{xid}: root tree oid #{oid} has no object-map record at its XID") } unless found

      { oid:, **found.slice(:xid, :flags, :flags_names, :size, :paddr) }
    end

    def btree(what, codec, storage:, subtype:)
      fetch = storage == STORAGE_VIRTUAL ? ->(oid) { virtual(oid, what) } : ->(oid) { @read.(oid, "#{@label} #{what}") }
      BTree.new(fetch:, problem: @problem, codec:, what: "#{@label} #{what}", block_size: @block_size, storage:, subtype:)
    end

    def note(what, record, live:)
      [*record[:error], *record[:problems]].each do
        problem("#{what}: #{record[:type_name]} record of object #{record[:obj_id]}: #{_1}")
      end
      return unless live

      @summary.add(record)
      compression(record)
    end

    # Notes what the decmpfs headers need: an inline header, a stream a
    # decmpfs attribute names, or where a stream's first extent starts (a
    # file extent, or a sealed volume's file extent tree record; block 0
    # marks a hole).
    def compression(record)
      case record
      in { type: J_TYPE_XATTR, name: DECMPFS_EA_NAME, decmpfs: }
        @compressed << { obj_id: record[:obj_id], stream_oid: nil, **decmpfs }
      in { type: J_TYPE_XATTR, name: DECMPFS_EA_NAME, stream_oid: }
        @decmpfs_streams[stream_oid] = record[:obj_id]
      in { type: J_TYPE_FILE_EXTENT, obj_id: stream, logical_addr: 0, phys_block_num: Integer => block } if block.positive?
        @first_blocks[stream] = block
      in { type_name: "FEXT", private_id: stream, logical_addr: 0, phys_block_num: Integer => block } if block.positive?
        @first_blocks[stream] = block
      else nil
      end
    end

    # The decmpfs header of every compressed file: the inline ones, then
    # each one read from the first block of its stream.
    def compressed
      @compressed + @decmpfs_streams.map do |stream, inode|
        block = @first_blocks[stream]
        header = block ? decmpfs_block(block) : { error: problem("decmpfs stream #{stream} of inode #{inode} has no extent at offset 0") }
        { obj_id: inode, stream_oid: stream, **header }
      end
    end

    def decmpfs_block(addr)
      header = Layout.decode(@read_block.(addr, "#{@label} decmpfs data"), DECMPFS_FIELDS)
      { **header, magic_ok: header[:magic] == DECMPFS_MAGIC }
    rescue Error => e
      { error: problem("decmpfs data at block #{addr}: #{e.message}") }
    end

    # A tree's document: its counts, and its nodes and the records `kept`
    # with `records`, else its root node. `count` is every record decoded,
    # which `kept` holds only with `records`.
    def tree_doc(oid, nodes:, kept:, count:)
      counts = { root_oid: oid, node_count: nodes.size, record_count: count }
      @records ? { **counts, nodes:, records: kept } : { **counts, root: nodes.first }
    end

    # The object a virtual OID names in the volume object map at the
    # superblock's XID, headerless when its mapping has the no-header flag.
    def virtual(oid, what)
      raise Error, "the volume object map is unreadable" unless @omap

      found = @omap.find(oid, @obj.xid)
      raise Error, "virtual oid #{oid} is not in the volume object map at xid #{@obj.xid}" unless found
      raise Error, "virtual oid #{oid} is deleted in the volume object map" if found[:flags].anybits?(OMAP_VAL_DELETED)

      @read.(found[:paddr], "#{@label} #{what} (virtual oid #{oid})", headerless: found[:flags].anybits?(OMAP_VAL_NOHEADER))
    end

    # The sealed feature names a sealed volume, which has integrity metadata
    # and a file extent tree. The converse is not checked.
    def sealed_problems(sb)
      return unless sb[:incompatible_features].anybits?(VOL_INCOMPAT_SEALED)

      problem("sealed volume: no integrity metadata") if sb[:integrity_meta_oid].zero?
      problem("sealed volume: no file extent tree") if sb[:fext_tree_oid].zero?
    end

    # The integrity metadata of a sealed volume. Its root hash, when it is a
    # SHA-256 inside the object, becomes @root_hash, which the file-system
    # tree's root must match. A broken seal is reported, since its digests
    # are not expected to match.
    def integrity_meta(oid)
      return nil if oid.zero?

      obj = virtual(oid, "integrity metadata")
      obj.header_problems(kind: KIND_INTEGRITY_META, storage: STORAGE_VIRTUAL, oid:).each { problem("integrity metadata: #{_1}") }
      fields = Layout.with_names(Layout.decode(obj.bytes, INTEGRITY_META_FIELDS), flags: INTEGRITY_FLAGS)
      problem("integrity metadata: version #{fields[:version]}, expected #{INTEGRITY_VERSION}") if fields[:version] != INTEGRITY_VERSION
      if fields[:flags].anybits?(INTEGRITY_SEAL_BROKEN) || fields[:broken_xid].positive?
        problem("integrity metadata: seal broken at xid #{fields[:broken_xid]}")
      end
      root_hash = stored_root_hash(obj.bytes, fields)
      if fields[:hash_type] == HASH_SHA256
        @root_hash = root_hash
      else
        problem("integrity metadata: hash type #{fields[:hash_type]} is not checked; only SHA-256 is")
      end
      obj.document({ **fields, hash_type_name: HASH_TYPES.fetch(fields[:hash_type]) { "unknown(#{fields[:hash_type]})" },
                     root_hash: root_hash&.unpack1("H*") })
    end

    # The root hash at the offset the integrity metadata names, the hash
    # type's length; nil for an unknown hash type, and nil and a problem
    # when it is not wholly past the fixed fields and inside the object.
    def stored_root_hash(bytes, fields)
      size = HASH_SIZES[fields[:hash_type]]
      return nil unless size

      offset = fields[:root_hash_offset]
      unless offset >= INTEGRITY_ROOT_HASH_MIN && offset + size <= bytes.bytesize
        problem("integrity metadata: root hash at #{offset}+#{size} is outside #{INTEGRITY_ROOT_HASH_MIN}...#{bytes.bytesize}")
        return nil
      end
      bytes.byteslice(offset, size)
    end

    # The rules of a sealed file extent tree's records, as a callback for
    # #tree: keys ascend by private ID then logical offset, and each extent
    # has no flags and a length that is a positive multiple of the block
    # size. A ghost record has a key only.
    def fext_rules
      last = nil
      lambda do |record|
        key = record.values_at(:private_id, :logical_addr)
        at = "file extent tree: extent #{key.join('/')}"
        problem("#{at}: key does not follow #{last.join('/')}") if last && (key <=> last) <= 0
        last = key
        next unless record[:length]

        problem(format("%s: flags 0x%x, expected 0", at, record[:extent_flags])) unless record[:extent_flags].zero?
        problem("#{at}: length #{record[:length]} is not a positive multiple of #{@block_size}") unless
          record[:length].positive? && (record[:length] % @block_size).zero?
      end
    end
  end

  # Prints a document as indented `key: value` lines. Integers whose key
  # names a checksum, type or flag word print in hexadecimal, a file mode
  # in octal. A hash of
  # scalars inside an array prints on one line; trailing zeros of an integer
  # array are counted instead of printed.
  module Text
    HEX_KEY = /cksum|type\z|flags\z|features\z|magic\z|subtype\z/
    OCTAL_KEY = /mode\z/
    # Printable ASCII; any other string, such as a name from the image with
    # control characters, prints escaped.
    PLAIN_TEXT = /\A[ -~]*\z/

    module_function

    def render(doc, out) = lines(doc, 0).each { out.puts(_1) }

    def lines(hash, depth)
      pad = "  " * depth
      hash.flat_map do |key, value|
        case value
        in Hash then ["#{pad}#{key}:", *lines(value, depth + 1)]
        in [] then ["#{pad}#{key}: none"]
        in [Hash, *] then value.each_with_index.flat_map { |item, i| element("#{pad}#{key}[#{i}]", item, depth) }
        in Array then ["#{pad}#{key}: #{scalars(key, value)}"]
        else ["#{pad}#{key}: #{scalar(key, value)}"]
        end
      end
    end

    def element(label, item, depth)
      return ["#{label}: #{item.map { |k, v| "#{k}=#{scalar(k, v)}" }.join(' ')}"] if item.values.none? { _1.is_a?(Hash) || _1.is_a?(Array) }

      ["#{label}:", *lines(item, depth + 1)]
    end

    def scalars(key, values)
      shown = values.reverse.drop_while { _1 == 0 }.reverse
      omitted = values.size - shown.size
      parts = shown.map { scalar(key, _1) }
      parts << "(+#{omitted} zero)" unless omitted.zero?
      parts.join(" ")
    end

    def scalar(key, value)
      case value
      when nil then "-"
      when Integer
        if key.to_s.match?(HEX_KEY) then format("0x%x", value)
        elsif key.to_s.match?(OCTAL_KEY) then format("0%o", value)
        else value.to_s
        end
      when String then value.match?(PLAIN_TEXT) ? value : value.dump
      else value.to_s
      end
    end
  end

  def self.main(argv)
    offset = 0
    json = false
    volume = nil
    records = false
    parser = OptionParser.new("usage: apfsdump.rb [--offset BYTES] [--volume N] [--records] [--json] IMAGE") do |o|
      o.on("--offset BYTES", Integer) { offset = _1 }
      o.on("--volume N", Integer) { volume = _1 }
      o.on("--records") { records = true }
      o.on("--json") { json = true }
    end
    paths = parser.parse(argv)
    raise Error, parser.banner unless paths.size == 1

    File.open(paths.first, "rb") do |io|
      container = Container.new(Image.new(io, offset:), volume:, records:)
      doc = container.dump
      json ? $stdout.puts(JSON.pretty_generate(doc)) : Text.render(doc.except(:problems), $stdout)
      container.problems.each { $stdout.puts("problem: #{_1}") } unless json
      container.problems.empty? ? 0 : 1
    end
  rescue Error, OptionParser::ParseError, SystemCallError => e
    warn "apfsdump: #{e.message}"
    1
  end
end

exit ApfsDump.main(ARGV) if $PROGRAM_NAME == __FILE__
