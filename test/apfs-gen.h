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
bool	apfs_gen_container_build(unsigned char *, size_t,
    const apfs_gen_container_t *);
bool	apfs_gen_reap_list_build(unsigned char *, size_t,
    const apfs_gen_reap_list_t *);

#endif /* !FSIMG_TEST_APFS_GEN_H */
