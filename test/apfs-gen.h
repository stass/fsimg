/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_APFS_GEN_H
#define FSIMG_TEST_APFS_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent byte layouts; this header has no dependency on the driver. */
enum {
	GEN_APFS_HEADER = 32,
	GEN_APFS_NODE_HEADER = 56,
	GEN_APFS_FOOTER = 40
};

enum {
	GEN_APFS_ROOT = 1,
	GEN_APFS_LEAF = 2,
	GEN_APFS_FIXED = 4
};

enum {
	GEN_APFS_GHOST = 65535,
	GEN_APFS_FREE_LIST_EMPTY = 65535
};

typedef struct apfs_gen_object {
	uint64_t	oid;
	uint64_t	xid;
	uint32_t	type;
	uint32_t	subtype;
} apfs_gen_object_t;

typedef struct apfs_gen_nx {
	apfs_gen_object_t object;
	uint32_t	magic;
	uint32_t	block_size;
	uint64_t	block_count;
	uint64_t	features;
	uint64_t	ro_compat_features;
	uint64_t	incompat_features;
	uint8_t		uuid[16];
	uint64_t	next_oid;
	uint64_t	next_xid;
	uint32_t	desc_blocks;
	uint32_t	data_blocks;
	uint64_t	desc_base;
	uint64_t	data_base;
	uint32_t	desc_next;
	uint32_t	data_next;
	uint32_t	desc_index;
	uint32_t	desc_len;
	uint32_t	data_index;
	uint32_t	data_len;
	uint64_t	spaceman_oid;
	uint64_t	omap_oid;
	uint64_t	reaper_oid;
	uint32_t	test_type;
	uint32_t	max_volumes;
	uint64_t	volume_oids[100];
	uint64_t	counters[32];
	uint64_t	blocked_out_start;
	uint64_t	blocked_out_count;
	uint64_t	evict_mapping_oid;
	uint64_t	flags;
	uint64_t	efi_jumpstart;
	uint8_t		fusion_uuid[16];
	uint64_t	keylocker_start;
	uint64_t	keylocker_count;
	uint64_t	ephemeral_info[4];
	uint64_t	test_oid;
	uint64_t	fusion_mt_oid;
	uint64_t	fusion_wbc_oid;
	uint64_t	fusion_wbc_start;
	uint64_t	fusion_wbc_count;
	uint64_t	newest_mounted_version;
	uint64_t	media_keylocker_start;
	uint64_t	media_keylocker_count;
} apfs_gen_nx_t;

/* Volume layout from design-apfs.md, "Volume superblock". */
typedef struct apfs_gen_fs_change {
	unsigned char id[32];
	uint64_t time;
	uint64_t last_xid;
} apfs_gen_fs_change_t;

typedef struct apfs_gen_fs {
	apfs_gen_object_t object;
	apfs_gen_fs_change_t formatted_by;
	apfs_gen_fs_change_t modified_by[8];
	unsigned char crypto[20];
	unsigned char uuid[16];
	unsigned char name[256];
	unsigned char group_uuid[16];
	uint32_t magic;
	uint32_t index;
	uint64_t features;
	uint64_t ro_compat_features;
	uint64_t incompat_features;
	uint64_t unmount_time;
	uint64_t reserve_blocks;
	uint64_t quota_blocks;
	uint64_t alloc_blocks;
	uint32_t root_tree_type;
	uint32_t extentref_tree_type;
	uint32_t snapshot_tree_type;
	uint64_t omap_oid;
	uint64_t root_tree_oid;
	uint64_t extentref_tree_oid;
	uint64_t snapshot_tree_oid;
	uint64_t revert_to_xid;
	uint64_t revert_to_oid;
	uint64_t next_obj_id;
	uint64_t files;
	uint64_t directories;
	uint64_t symlinks;
	uint64_t other_objects;
	uint64_t snapshots;
	uint64_t blocks_allocated;
	uint64_t blocks_freed;
	uint64_t modified_time;
	uint64_t flags;
	uint32_t next_doc_id;
	uint16_t role;
	uint64_t root_to_xid;
	uint64_t rolling_state_oid;
	uint64_t clone_epoch;
	uint64_t clone_xid;
	uint64_t snapshot_ext_oid;
	uint64_t integrity_oid;
	uint64_t fext_tree_oid;
	uint32_t fext_tree_type;
} apfs_gen_fs_t;

bool	apfs_gen_fs_build(unsigned char *, size_t, const apfs_gen_fs_t *);

typedef struct apfs_gen_mapping {
	uint32_t	type;
	uint32_t	subtype;
	uint32_t	size;
	uint64_t	volume_oid;
	uint64_t	oid;
	uint64_t	address;
} apfs_gen_mapping_t;

typedef struct apfs_gen_omap {
	apfs_gen_object_t object;
	uint32_t	flags;
	uint32_t	snapshot_count;
	uint32_t	tree_type;
	uint32_t	snapshot_tree_type;
	uint64_t	tree_oid;
	uint64_t	snapshot_tree_oid;
	uint64_t	most_recent_snapshot;
	uint64_t	pending_revert_min;
	uint64_t	pending_revert_max;
} apfs_gen_omap_t;

typedef struct apfs_gen_info {
	uint32_t	flags;
	uint32_t	node_size;
	uint32_t	key_size;
	uint32_t	val_size;
	uint32_t	longest_key;
	uint32_t	longest_val;
	uint64_t	key_count;
	uint64_t	node_count;
} apfs_gen_info_t;

typedef struct apfs_gen_device {
	uint64_t	block_count;
	uint64_t	chunk_count;
	uint32_t	cib_count;
	uint32_t	cab_count;
	uint64_t	free_count;
	uint32_t	addr_offset;
} apfs_gen_device_t;

typedef struct apfs_gen_queue {
	uint64_t	count;
	uint64_t	tree_oid;
	uint64_t	oldest_xid;
	uint16_t	node_limit;
} apfs_gen_queue_t;

typedef struct apfs_gen_spaceman {
	apfs_gen_object_t object;
	uint32_t	block_size;
	uint32_t	blocks_per_chunk;
	uint32_t	chunks_per_cib;
	uint32_t	cibs_per_cab;
	uint32_t	flags;
	uint32_t	ip_bm_tx_multiplier;
	uint64_t	ip_block_count;
	uint32_t	ip_bm_size_in_blocks;
	uint32_t	ip_bm_block_count;
	uint64_t	ip_bm_base;
	uint64_t	ip_base;
	uint64_t	fs_reserve_block_count;
	uint64_t	fs_reserve_alloc_count;
	uint16_t	ip_bm_free_head;
	uint16_t	ip_bm_free_tail;
	uint32_t	ip_bm_xid_offset;
	uint32_t	ip_bitmap_offset;
	uint32_t	ip_bm_free_next_offset;
	uint32_t	version;
	uint32_t	struct_size;
	apfs_gen_device_t devices[2];
	apfs_gen_queue_t free_queues[3];
} apfs_gen_spaceman_t;

typedef struct apfs_gen_chunk {
	uint64_t	xid;
	uint64_t	address;
	uint32_t	block_count;
	uint32_t	free_count;
	uint64_t	bitmap_address;
} apfs_gen_chunk_t;

typedef struct apfs_gen_omap_key {
	uint64_t	oid;
	uint64_t	xid;
} apfs_gen_omap_key_t;

typedef struct apfs_gen_omap_value {
	uint32_t	flags;
	uint32_t	size;
	uint64_t	address;
} apfs_gen_omap_value_t;

typedef struct apfs_gen_fq_key {
	uint64_t	xid;
	uint64_t	address;
} apfs_gen_fq_key_t;

typedef struct apfs_gen_reaper {
	apfs_gen_object_t object;
	uint64_t	next_reap_id;
	uint64_t	completed_id;
	uint64_t	head;
	uint64_t	tail;
	uint32_t	flags;
	uint32_t	rlcount;
	uint32_t	reaping_type;
	uint32_t	reaping_size;
	uint64_t	reaping_volume_oid;
	uint64_t	reaping_oid;
	uint64_t	reaping_xid;
	uint32_t	reaping_entry_flags;
	uint32_t	state_size;
} apfs_gen_reaper_t;

typedef struct apfs_gen_checkpoint {
	apfs_gen_object_t object;
	const apfs_gen_mapping_t *mappings;
	uint32_t	flags;
	uint32_t	count;
} apfs_gen_checkpoint_t;

typedef struct apfs_gen_chunks {
	apfs_gen_object_t object;
	const apfs_gen_chunk_t *chunks;
	uint32_t	index;
	uint32_t	count;
} apfs_gen_chunks_t;

typedef struct apfs_gen_addresses {
	apfs_gen_object_t object;
	const uint64_t	*addresses;
	uint32_t	index;
	uint32_t	count;
} apfs_gen_addresses_t;

typedef struct apfs_gen_record {
	const unsigned char *key;
	const unsigned char *value;
	uint16_t	key_size;
	uint16_t	value_size;
	bool		ghost;
} apfs_gen_record_t;

typedef struct apfs_gen_node {
	apfs_gen_object_t object;
	apfs_gen_info_t	info;
	const apfs_gen_record_t *records;
	uint32_t	count;
	uint16_t	flags;
	uint16_t	level;
	/* Bytes; zero selects just enough for records. */
	uint16_t	toc_capacity;
} apfs_gen_node_t;

/*
 * Builders borrow zeroed buffers and nonoverlapping parameters. A false
 * return leaves the entire buffer untouched. Object builders seal the whole
 * supplied buffer, which must be at least 32 bytes and a multiple of four.
 * Record builders write only their fixed record and do not seal it. Payloads
 * and arrays outside the fixed structures remain the caller's to assemble.
 */
bool	apfs_gen_object_build(unsigned char *, size_t,
    const apfs_gen_object_t *);
bool	apfs_gen_nx_build(unsigned char *, size_t, const apfs_gen_nx_t *);
bool	apfs_gen_checkpoint_build(unsigned char *, size_t,
    const apfs_gen_checkpoint_t *);
bool	apfs_gen_omap_build(unsigned char *, size_t,
    const apfs_gen_omap_t *);
bool	apfs_gen_node_build(unsigned char *, size_t,
    const apfs_gen_node_t *);
bool	apfs_gen_spaceman_build(unsigned char *, size_t,
    const apfs_gen_spaceman_t *);
bool	apfs_gen_chunks_build(unsigned char *, size_t,
    const apfs_gen_chunks_t *);
bool	apfs_gen_addresses_build(unsigned char *, size_t,
    const apfs_gen_addresses_t *);
bool	apfs_gen_reaper_build(unsigned char *, size_t,
    const apfs_gen_reaper_t *);
bool	apfs_gen_omap_key_build(unsigned char *, size_t,
    const apfs_gen_omap_key_t *);
bool	apfs_gen_omap_value_build(unsigned char *, size_t,
    const apfs_gen_omap_value_t *);
bool	apfs_gen_fq_key_build(unsigned char *, size_t,
    const apfs_gen_fq_key_t *);
bool	apfs_gen_fq_value_build(unsigned char *, size_t, uint64_t);

/* Checksum over [8, length), reduced independently by weighted word sums. */
uint64_t apfs_gen_checksum(const unsigned char *, size_t);
bool	apfs_gen_seal(unsigned char *, size_t);

/* A checkpoint borrows its mapping array; objects are emitted from mappings. */
typedef struct apfs_gen_run {
	uint64_t	xid;
	uint64_t	next_xid;
	const apfs_gen_mapping_t *mappings;
	uint32_t	mapping_count;
	uint32_t	mapping_blocks;
	uint32_t	desc_index;
	uint32_t	data_index;
	uint32_t	data_len;
	uint32_t	desc_next;
	uint32_t	data_next;
} apfs_gen_run_t;

typedef struct apfs_gen_container {
	apfs_gen_nx_t	nx;
	const apfs_gen_run_t *runs;
	size_t	count;
	size_t	block0_run;
	uint64_t	block0_xid;
	/* Zero selects the native ring size; arrays always remain packed. */
	uint32_t	spaceman_ring_blocks;
} apfs_gen_container_t;

typedef struct apfs_gen_reap_entry {
	uint32_t	next;
	uint32_t	flags;
	uint32_t	kind;
	uint32_t	size;
	uint64_t	volume_oid;
	uint64_t	oid;
	uint64_t	xid;
} apfs_gen_reap_entry_t;

typedef struct apfs_gen_reap_list {
	apfs_gen_object_t	object;
	const apfs_gen_reap_entry_t *entries;
	uint64_t	next;
	uint32_t	flags;
	uint32_t	capacity;
	uint32_t	count;
	uint32_t	first;
	uint32_t	last;
	uint32_t	free;
} apfs_gen_reap_list_t;

typedef struct apfs_gen_omap_record {
	apfs_gen_omap_key_t	key;
	apfs_gen_omap_value_t	value;
} apfs_gen_omap_record_t;

typedef struct apfs_gen_tree {
	const apfs_gen_omap_record_t *records;
	size_t	count;
	uint64_t	base;
	uint64_t	xid;
	uint32_t	block_size;
	uint16_t	records_per_leaf;
	uint16_t	children_per_index;
	uint16_t	toc_capacity;
	bool	nonaligned;
} apfs_gen_tree_t;

/* The tree occupies consecutive blocks, leaves first and root last. */
bool	apfs_gen_tree_build(unsigned char *, size_t, const apfs_gen_tree_t *,
    uint64_t *, uint64_t *);
/* Independent geometry; returns the packed object's byte size, or zero. */
size_t	apfs_gen_spaceman_geometry(uint32_t, uint64_t,
    apfs_gen_spaceman_t *);
bool	apfs_gen_container_build(unsigned char *, size_t,
    const apfs_gen_container_t *);
bool	apfs_gen_reap_list_build(unsigned char *, size_t,
    const apfs_gen_reap_list_t *);

/* Filesystem records use independent offsets and packed-word layouts. */
enum {
	GEN_DSTREAM_BYTES = 40,
	GEN_DIR_STATS_BYTES = 32,
	GEN_INODE_VAL_BYTES = 92,
	GEN_DREC_VAL_BYTES = 18,
	GEN_EXTENT_VAL_BYTES = 24,
	GEN_PEXT_VAL_BYTES = 20,
	GEN_SNAP_META_VAL_BYTES = 50,
	GEN_FILE_INFO_VAL_BYTES = 3,
	GEN_JKEY_BYTES = 8,
	GEN_DREC_KEY_BYTES = 10,
	GEN_DREC_HASHED_KEY_BYTES = 12,
	GEN_NAME_KEY_BYTES = 10,
	GEN_SIBLING_KEY_BYTES = 16,
	GEN_SIBLING_VAL_BYTES = 10,
	GEN_SIBLING_MAP_VAL_BYTES = 8,
	GEN_DSTREAM_ID_VAL_BYTES = 4,
	GEN_EXTENT_KEY_BYTES = 16,
	GEN_SNAP_NAME_VAL_BYTES = 8,
	GEN_FILE_INFO_KEY_BYTES = 16,
	GEN_XATTR_VAL_BYTES = 4,
	GEN_XATTR_STREAM_BYTES = 48,
	GEN_XF_BLOB_BYTES = 4,
	GEN_XF_HEADER_BYTES = 4,
	GEN_XF_ALIGN = 8
};

typedef struct apfs_gen_dstream {
	uint64_t	size;
	uint64_t	alloced_size;
	uint64_t	default_crypto_id;
	uint64_t	total_bytes_written;
	uint64_t	total_bytes_read;
} apfs_gen_dstream_t;

bool	apfs_gen_dstream_build(unsigned char *, size_t,
    const apfs_gen_dstream_t *);

typedef struct apfs_gen_dir_stats {
	uint64_t	num_children;
	uint64_t	total_size;
	uint64_t	chained_key;
	uint64_t	gen_count;
} apfs_gen_dir_stats_t;

bool	apfs_gen_dir_stats_build(unsigned char *, size_t,
    const apfs_gen_dir_stats_t *);

typedef struct apfs_gen_inode_val {
	uint64_t	parent;
	uint64_t	private_id;
	int64_t	create_time;
	int64_t	mod_time;
	int64_t	change_time;
	int64_t	access_time;
	uint64_t	internal_flags;
	int32_t	count;
	uint32_t	protection_class;
	uint32_t	write_generation;
	uint32_t	bsd_flags;
	uint32_t	owner;
	uint32_t	group;
	uint16_t	mode;
	uint64_t	uncompressed_size;
	const unsigned char *xfields;
	size_t	xfields_len;
} apfs_gen_inode_val_t;

bool	apfs_gen_inode_val_build(unsigned char *, size_t,
    const apfs_gen_inode_val_t *);

typedef struct apfs_gen_drec_val {
	uint64_t	child;
	int64_t	date_added;
	uint16_t	flags;
	const unsigned char *xfields;
	size_t	xfields_len;
} apfs_gen_drec_val_t;

bool	apfs_gen_drec_val_build(unsigned char *, size_t,
    const apfs_gen_drec_val_t *);

typedef struct apfs_gen_extent_val {
	uint64_t	length;
	uint64_t	phys_block;
	uint64_t	crypto_id;
	uint8_t		flags;
} apfs_gen_extent_val_t;

bool	apfs_gen_extent_val_build(unsigned char *, size_t,
    const apfs_gen_extent_val_t *);

typedef struct apfs_gen_pext_val {
	uint64_t	length;
	uint64_t	owner;
	int32_t	refcnt;
	uint8_t		kind;
} apfs_gen_pext_val_t;

bool	apfs_gen_pext_val_build(unsigned char *, size_t,
    const apfs_gen_pext_val_t *);

typedef struct apfs_gen_snap_meta_val {
	uint64_t	extentref_tree_oid;
	uint64_t	sblock_oid;
	int64_t	create_time;
	int64_t	change_time;
	uint64_t	inum;
	uint32_t	extentref_tree_type;
	uint32_t	flags;
	const char *name;
	uint16_t	name_len;
} apfs_gen_snap_meta_val_t;

bool	apfs_gen_snap_meta_val_build(unsigned char *, size_t,
    const apfs_gen_snap_meta_val_t *);

typedef struct apfs_gen_file_info_val {
	uint16_t	hashed_len;
	uint8_t	hash_size;
	const unsigned char *hash;
} apfs_gen_file_info_val_t;

bool	apfs_gen_file_info_val_build(unsigned char *, size_t,
    const apfs_gen_file_info_val_t *);

#define GEN_JKEY_OID_MASK UINT64_C(0x0fffffffffffffff)

typedef struct apfs_gen_xfield {
	const unsigned char *data;
	uint16_t	size;
	uint8_t		type;
	uint8_t		flags;
} apfs_gen_xfield_t;

typedef struct apfs_gen_drec_key {
	const char	*name;
	uint64_t	parent;
	uint32_t	hash;
	uint16_t	name_len;
	bool		hashed;
} apfs_gen_drec_key_t;

typedef struct apfs_gen_name_key {
	const char	*name;
	uint64_t	oid;
	uint16_t	name_len;
} apfs_gen_name_key_t;

typedef struct apfs_gen_sibling_val {
	const char	*name;
	uint64_t	parent;
	uint16_t	name_len;
} apfs_gen_sibling_val_t;

typedef struct apfs_gen_xattr_val {
	const unsigned char *data;
	apfs_gen_dstream_t dstream;
	uint64_t	stream_oid;
	uint16_t	flags;
	uint16_t	data_len;
} apfs_gen_xattr_val_t;

bool	apfs_gen_jkey_build(unsigned char *, size_t, uint64_t, uint8_t);
bool	apfs_gen_xfields_build(unsigned char *, size_t,
    const apfs_gen_xfield_t *, uint16_t);
bool	apfs_gen_drec_key_build(unsigned char *, size_t,
    const apfs_gen_drec_key_t *);
bool	apfs_gen_xattr_key_build(unsigned char *, size_t,
    const apfs_gen_name_key_t *);
bool	apfs_gen_snap_name_key_build(unsigned char *, size_t,
    const apfs_gen_name_key_t *);
bool	apfs_gen_xattr_val_build(unsigned char *, size_t,
    const apfs_gen_xattr_val_t *);
bool	apfs_gen_sibling_key_build(unsigned char *, size_t, uint64_t,
    uint64_t);
bool	apfs_gen_sibling_val_build(unsigned char *, size_t,
    const apfs_gen_sibling_val_t *);
bool	apfs_gen_sibling_map_val_build(unsigned char *, size_t, uint64_t);
bool	apfs_gen_dstream_id_val_build(unsigned char *, size_t, uint32_t);
bool	apfs_gen_snap_name_val_build(unsigned char *, size_t, uint64_t);
bool	apfs_gen_extent_key_build(unsigned char *, size_t, uint64_t, uint64_t);
bool	apfs_gen_file_info_key_build(unsigned char *, size_t, uint64_t,
    uint64_t, uint8_t);

#endif /* !FSIMG_TEST_APFS_GEN_H */
