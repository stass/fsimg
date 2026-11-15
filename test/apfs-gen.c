/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "apfs-gen.h"

/* Each plan entry names a contiguous range of records or child nodes. */
typedef struct tree_plan {
	size_t	first;
	size_t	count;
	size_t	key;
	uint16_t	level;
} tree_plan_t;

static bool block_fits(size_t, size_t);
static void put_le(unsigned char *, uint64_t, unsigned);
static void write_object(unsigned char *, const apfs_gen_object_t *);
static void write_nx(unsigned char *, const apfs_gen_nx_t *);
static void write_mapping(unsigned char *, const apfs_gen_mapping_t *);
static void write_omap(unsigned char *, const apfs_gen_omap_t *);
static void write_info(unsigned char *, const apfs_gen_info_t *);
static void write_device(unsigned char *, const apfs_gen_device_t *);
static void write_queue(unsigned char *, const apfs_gen_queue_t *);
static void write_spaceman(unsigned char *, const apfs_gen_spaceman_t *);
static void write_chunk(unsigned char *, const apfs_gen_chunk_t *);
static void write_fq_key(unsigned char *, const apfs_gen_fq_key_t *);
static void write_reaper(unsigned char *, const apfs_gen_reaper_t *);

static bool
block_fits(size_t length, size_t minimum)
{
	return (length >= minimum && length % 4 == 0);
}

static void
put_le(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

/*
 * For n data words, the second Fletcher sum equals the weighted sum
 * sum((n - i) * word[i]). Compute that directly, rather than updating a
 * running second sum. Reducing each product also bounds the accumulator.
 */
uint64_t
apfs_gen_checksum(const unsigned char *raw, size_t length)
{
	const uint64_t modulus = UINT64_C(0xffffffff);
	uint64_t first = 0, second = 0, word, check1, check2;
	size_t count;

	count = length > 8 ? (length - 8) / 4 : 0;
	for (size_t i = 0; i < count; i++) {
		word = 0;
		for (unsigned j = 0; j < 4; j++)
			word |= (uint64_t)raw[8 + 4 * i + j] << (8 * j);
		first = (first + word) % modulus;
		second = (second + word * ((count - i) % modulus) %
		    modulus) % modulus;
	}
	check1 = modulus - (first + second) % modulus;
	check2 = modulus - (first + check1) % modulus;
	return (check1 | check2 << 32);
}

bool
apfs_gen_seal(unsigned char *raw, size_t length)
{
	if (!block_fits(length, GEN_APFS_HEADER))
		return (false);
	put_le(raw, apfs_gen_checksum(raw, length), 8);
	return (true);
}

static void
write_object(unsigned char *raw, const apfs_gen_object_t *p)
{
	put_le(raw + 8, p->oid, 8);
	put_le(raw + 16, p->xid, 8);
	put_le(raw + 24, p->type, 4);
	put_le(raw + 28, p->subtype, 4);
}

static void
write_nx(unsigned char *raw, const apfs_gen_nx_t *p)
{
	write_object(raw, &p->object);
	put_le(raw + 32, p->magic, 4);
	put_le(raw + 36, p->block_size, 4);
	put_le(raw + 40, p->block_count, 8);
	put_le(raw + 48, p->features, 8);
	put_le(raw + 56, p->ro_compat_features, 8);
	put_le(raw + 64, p->incompat_features, 8);
	for (unsigned i = 0; i < 16; i++)
		put_le(raw + 72 + 1 * i, p->uuid[i], 1);
	put_le(raw + 88, p->next_oid, 8);
	put_le(raw + 96, p->next_xid, 8);
	put_le(raw + 104, p->desc_blocks, 4);
	put_le(raw + 108, p->data_blocks, 4);
	put_le(raw + 112, p->desc_base, 8);
	put_le(raw + 120, p->data_base, 8);
	put_le(raw + 128, p->desc_next, 4);
	put_le(raw + 132, p->data_next, 4);
	put_le(raw + 136, p->desc_index, 4);
	put_le(raw + 140, p->desc_len, 4);
	put_le(raw + 144, p->data_index, 4);
	put_le(raw + 148, p->data_len, 4);
	put_le(raw + 152, p->spaceman_oid, 8);
	put_le(raw + 160, p->omap_oid, 8);
	put_le(raw + 168, p->reaper_oid, 8);
	put_le(raw + 176, p->test_type, 4);
	put_le(raw + 180, p->max_volumes, 4);
	for (unsigned i = 0; i < 100; i++)
		put_le(raw + 184 + 8 * i, p->volume_oids[i], 8);
	for (unsigned i = 0; i < 32; i++)
		put_le(raw + 984 + 8 * i, p->counters[i], 8);
	put_le(raw + 1240, p->blocked_out_start, 8);
	put_le(raw + 1248, p->blocked_out_count, 8);
	put_le(raw + 1256, p->evict_mapping_oid, 8);
	put_le(raw + 1264, p->flags, 8);
	put_le(raw + 1272, p->efi_jumpstart, 8);
	for (unsigned i = 0; i < 16; i++)
		put_le(raw + 1280 + 1 * i, p->fusion_uuid[i], 1);
	put_le(raw + 1296, p->keylocker_start, 8);
	put_le(raw + 1304, p->keylocker_count, 8);
	for (unsigned i = 0; i < 4; i++)
		put_le(raw + 1312 + 8 * i, p->ephemeral_info[i], 8);
	put_le(raw + 1344, p->test_oid, 8);
	put_le(raw + 1352, p->fusion_mt_oid, 8);
	put_le(raw + 1360, p->fusion_wbc_oid, 8);
	put_le(raw + 1368, p->fusion_wbc_start, 8);
	put_le(raw + 1376, p->fusion_wbc_count, 8);
	put_le(raw + 1384, p->newest_mounted_version, 8);
	put_le(raw + 1392, p->media_keylocker_start, 8);
	put_le(raw + 1400, p->media_keylocker_count, 8);
}

static void
write_mapping(unsigned char *raw, const apfs_gen_mapping_t *p)
{
	put_le(raw + 0, p->type, 4);
	put_le(raw + 4, p->subtype, 4);
	put_le(raw + 8, p->size, 4);
	put_le(raw + 16, p->volume_oid, 8);
	put_le(raw + 24, p->oid, 8);
	put_le(raw + 32, p->address, 8);
}

static void
write_omap(unsigned char *raw, const apfs_gen_omap_t *p)
{
	write_object(raw, &p->object);
	put_le(raw + 32, p->flags, 4);
	put_le(raw + 36, p->snapshot_count, 4);
	put_le(raw + 40, p->tree_type, 4);
	put_le(raw + 44, p->snapshot_tree_type, 4);
	put_le(raw + 48, p->tree_oid, 8);
	put_le(raw + 56, p->snapshot_tree_oid, 8);
	put_le(raw + 64, p->most_recent_snapshot, 8);
	put_le(raw + 72, p->pending_revert_min, 8);
	put_le(raw + 80, p->pending_revert_max, 8);
}

static void
write_info(unsigned char *raw, const apfs_gen_info_t *p)
{
	put_le(raw + 0, p->flags, 4);
	put_le(raw + 4, p->node_size, 4);
	put_le(raw + 8, p->key_size, 4);
	put_le(raw + 12, p->val_size, 4);
	put_le(raw + 16, p->longest_key, 4);
	put_le(raw + 20, p->longest_val, 4);
	put_le(raw + 24, p->key_count, 8);
	put_le(raw + 32, p->node_count, 8);
}

static void
write_device(unsigned char *raw, const apfs_gen_device_t *p)
{
	put_le(raw + 0, p->block_count, 8);
	put_le(raw + 8, p->chunk_count, 8);
	put_le(raw + 16, p->cib_count, 4);
	put_le(raw + 20, p->cab_count, 4);
	put_le(raw + 24, p->free_count, 8);
	put_le(raw + 32, p->addr_offset, 4);
}

static void
write_queue(unsigned char *raw, const apfs_gen_queue_t *p)
{
	put_le(raw + 0, p->count, 8);
	put_le(raw + 8, p->tree_oid, 8);
	put_le(raw + 16, p->oldest_xid, 8);
	put_le(raw + 24, p->node_limit, 2);
}

static void
write_spaceman(unsigned char *raw, const apfs_gen_spaceman_t *p)
{
	write_object(raw, &p->object);
	put_le(raw + 32, p->block_size, 4);
	put_le(raw + 36, p->blocks_per_chunk, 4);
	put_le(raw + 40, p->chunks_per_cib, 4);
	put_le(raw + 44, p->cibs_per_cab, 4);
	put_le(raw + 144, p->flags, 4);
	put_le(raw + 148, p->ip_bm_tx_multiplier, 4);
	put_le(raw + 152, p->ip_block_count, 8);
	put_le(raw + 160, p->ip_bm_size_in_blocks, 4);
	put_le(raw + 164, p->ip_bm_block_count, 4);
	put_le(raw + 168, p->ip_bm_base, 8);
	put_le(raw + 176, p->ip_base, 8);
	put_le(raw + 184, p->fs_reserve_block_count, 8);
	put_le(raw + 192, p->fs_reserve_alloc_count, 8);
	put_le(raw + 320, p->ip_bm_free_head, 2);
	put_le(raw + 322, p->ip_bm_free_tail, 2);
	put_le(raw + 324, p->ip_bm_xid_offset, 4);
	put_le(raw + 328, p->ip_bitmap_offset, 4);
	put_le(raw + 332, p->ip_bm_free_next_offset, 4);
	if (p->layout == APFS_GEN_LAYOUT_CURRENT) {
		put_le(raw + 336, p->version, 4);
		put_le(raw + 340, p->struct_size, 4);
	}
	for (unsigned i = 0; i < 2; i++)
		write_device(raw + 48 + 48 * i, &p->devices[i]);
	for (unsigned i = 0; i < 3; i++)
		write_queue(raw + 200 + 40 * i, &p->free_queues[i]);
}

static void
write_chunk(unsigned char *raw, const apfs_gen_chunk_t *p)
{
	put_le(raw + 0, p->xid, 8);
	put_le(raw + 8, p->address, 8);
	put_le(raw + 16, p->block_count, 4);
	put_le(raw + 20, p->free_count, 4);
	put_le(raw + 24, p->bitmap_address, 8);
}

static void
write_fq_key(unsigned char *raw, const apfs_gen_fq_key_t *p)
{
	put_le(raw + 0, p->xid, 8);
	put_le(raw + 8, p->address, 8);
}

static void
write_reaper(unsigned char *raw, const apfs_gen_reaper_t *p)
{
	write_object(raw, &p->object);
	put_le(raw + 32, p->next_reap_id, 8);
	put_le(raw + 40, p->completed_id, 8);
	put_le(raw + 48, p->head, 8);
	put_le(raw + 56, p->tail, 8);
	put_le(raw + 64, p->flags, 4);
	put_le(raw + 68, p->rlcount, 4);
	put_le(raw + 72, p->reaping_type, 4);
	put_le(raw + 76, p->reaping_size, 4);
	put_le(raw + 80, p->reaping_volume_oid, 8);
	put_le(raw + 88, p->reaping_oid, 8);
	put_le(raw + 96, p->reaping_xid, 8);
	put_le(raw + 104, p->reaping_entry_flags, 4);
	put_le(raw + 108, p->state_size, 4);
}

bool
apfs_gen_object_build(unsigned char *raw, size_t length,
    const apfs_gen_object_t *p)
{
	if (!block_fits(length, 32))
		return (false);
	write_object(raw, p);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_nx_build(unsigned char *raw, size_t length, const apfs_gen_nx_t *p)
{
	if (!block_fits(length, 1408))
		return (false);
	write_nx(raw, p);
	return (apfs_gen_seal(raw, length));
}

/* Independent APSB encoder; reserved bytes come from the zeroed buffer. */
bool
apfs_gen_fs_build(unsigned char *raw, size_t length, const apfs_gen_fs_t *p)
{
	if (!block_fits(length, 1048))
		return (false);
	write_object(raw, &p->object);
	put_le(raw + 32, p->magic, 4);
	put_le(raw + 36, p->index, 4);
	put_le(raw + 40, p->features, 8);
	put_le(raw + 48, p->ro_compat_features, 8);
	put_le(raw + 56, p->incompat_features, 8);
	put_le(raw + 64, p->unmount_time, 8);
	put_le(raw + 72, p->reserve_blocks, 8);
	put_le(raw + 80, p->quota_blocks, 8);
	put_le(raw + 88, p->alloc_blocks, 8);
	put_le(raw + 116, p->root_tree_type, 4);
	put_le(raw + 120, p->extentref_tree_type, 4);
	put_le(raw + 124, p->snapshot_tree_type, 4);
	put_le(raw + 128, p->omap_oid, 8);
	put_le(raw + 136, p->root_tree_oid, 8);
	put_le(raw + 144, p->extentref_tree_oid, 8);
	put_le(raw + 152, p->snapshot_tree_oid, 8);
	put_le(raw + 160, p->revert_to_xid, 8);
	put_le(raw + 168, p->revert_to_oid, 8);
	put_le(raw + 176, p->next_obj_id, 8);
	put_le(raw + 184, p->files, 8);
	put_le(raw + 192, p->directories, 8);
	put_le(raw + 200, p->symlinks, 8);
	put_le(raw + 208, p->other_objects, 8);
	put_le(raw + 216, p->snapshots, 8);
	put_le(raw + 224, p->blocks_allocated, 8);
	put_le(raw + 232, p->blocks_freed, 8);
	put_le(raw + 256, p->modified_time, 8);
	put_le(raw + 264, p->flags, 8);
	put_le(raw + 960, p->next_doc_id, 4);
	put_le(raw + 964, p->role, 2);
	put_le(raw + 968, p->root_to_xid, 8);
	put_le(raw + 976, p->rolling_state_oid, 8);
	put_le(raw + 984, p->clone_epoch, 8);
	put_le(raw + 992, p->clone_xid, 8);
	put_le(raw + 1000, p->snapshot_ext_oid, 8);
	put_le(raw + 1024, p->integrity_oid, 8);
	put_le(raw + 1032, p->fext_tree_oid, 8);
	put_le(raw + 1040, p->fext_tree_type, 4);
	put_le(raw + 1044, p->reserved_tree_type, 4);
	bcopy(p->crypto, raw + 96, 20);
	bcopy(p->uuid, raw + 240, 16);
	bcopy(p->name, raw + 704, 256);
	bcopy(p->group_uuid, raw + 1008, 16);
	bcopy(p->formatted_by.id, raw + 272, 32);
	put_le(raw + 304, p->formatted_by.time, 8);
	put_le(raw + 312, p->formatted_by.last_xid, 8);
	for (unsigned i = 0; i < 8; i++) {
		bcopy(p->modified_by[i].id, raw + 320 + 48 * i, 32);
		put_le(raw + 352 + 48 * i, p->modified_by[i].time, 8);
		put_le(raw + 360 + 48 * i, p->modified_by[i].last_xid, 8);
	}
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_omap_build(unsigned char *raw, size_t length, const apfs_gen_omap_t *p)
{
	if (!block_fits(length, 88))
		return (false);
	write_omap(raw, p);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_spaceman_build(unsigned char *raw, size_t length,
    const apfs_gen_spaceman_t *p)
{
	if (!block_fits(length,
	    p->layout == APFS_GEN_LAYOUT_OLDER ? 336 : 2520))
		return (false);
	write_spaceman(raw, p);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_reaper_build(unsigned char *raw, size_t length,
    const apfs_gen_reaper_t *p)
{
	if (!block_fits(length, 112) ||
	    p->state_size > length - 112)
		return (false);
	write_reaper(raw, p);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_omap_key_build(unsigned char *raw, size_t length,
    const apfs_gen_omap_key_t *p)
{
	if (length < 16)
		return (false);
	put_le(raw, p->oid, 8);
	put_le(raw + 8, p->xid, 8);
	return (true);
}

bool
apfs_gen_omap_value_build(unsigned char *raw, size_t length,
    const apfs_gen_omap_value_t *p)
{
	if (length < 16)
		return (false);
	put_le(raw, p->flags, 4);
	put_le(raw + 4, p->size, 4);
	put_le(raw + 8, p->address, 8);
	return (true);
}

bool
apfs_gen_fq_key_build(unsigned char *raw, size_t length,
    const apfs_gen_fq_key_t *p)
{
	if (length < 16)
		return (false);
	write_fq_key(raw, p);
	return (true);
}

bool
apfs_gen_checkpoint_build(unsigned char *raw, size_t length,
    const apfs_gen_checkpoint_t *p)
{
	if (!block_fits(length, 40) || p->count > (length - 40) / 40 ||
	    (p->count != 0 && p->mappings == NULL))
		return (false);
	write_object(raw, &p->object);
	put_le(raw + 32, p->flags, 4);
	put_le(raw + 36, p->count, 4);
	for (uint32_t i = 0; i < p->count; i++)
		write_mapping(raw + 40 + 40 * (size_t)i, &p->mappings[i]);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_chunks_build(unsigned char *raw, size_t length,
    const apfs_gen_chunks_t *p)
{
	if (!block_fits(length, 40) || p->count > (length - 40) / 32 ||
	    (p->count != 0 && p->chunks == NULL))
		return (false);
	write_object(raw, &p->object);
	put_le(raw + 32, p->index, 4);
	put_le(raw + 36, p->count, 4);
	for (uint32_t i = 0; i < p->count; i++)
		write_chunk(raw + 40 + 32 * (size_t)i, &p->chunks[i]);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_addresses_build(unsigned char *raw, size_t length,
    const apfs_gen_addresses_t *p)
{
	if (!block_fits(length, 40) || p->count > (length - 40) / 8 ||
	    (p->count != 0 && p->addresses == NULL))
		return (false);
	write_object(raw, &p->object);
	put_le(raw + 32, p->index, 4);
	put_le(raw + 36, p->count, 4);
	for (uint32_t i = 0; i < p->count; i++)
		put_le(raw + 40 + 8 * (size_t)i, p->addresses[i], 8);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_fq_value_build(unsigned char *raw, size_t length, uint64_t count)
{
	if (length < 8)
		return (false);
	put_le(raw, count, 8);
	return (true);
}

/*
 * Plan the complete node before writing: keys grow up after the TOC and
 * values grow down before the footer. Ghosts consume only key bytes.
 * Subtraction checks prevent both host-size and on-disk u16 overflow.
 */
bool
apfs_gen_node_build(unsigned char *raw, size_t length, const apfs_gen_node_t *p)
{
	const apfs_gen_record_t *record;
	size_t toc_size, entry_size, key_bytes = 0, value_bytes = 0;
	size_t key_start, value_end, available, key_cursor, value_cursor;
	uint16_t fixed_value_size = 0;
	bool fixed, root, have_value_size = false;

	fixed = (p->flags & GEN_APFS_FIXED) != 0;
	root = (p->flags & GEN_APFS_ROOT) != 0;
	if (!block_fits(length, GEN_APFS_NODE_HEADER) || length > 65536 ||
	    (p->count != 0 && p->records == NULL))
		return (false);
	if (root && length < GEN_APFS_NODE_HEADER + GEN_APFS_FOOTER)
		return (false);
	value_end = length - (root ? GEN_APFS_FOOTER : 0);
	entry_size = fixed ? 4 : 8;
	if (p->count > (value_end - GEN_APFS_NODE_HEADER) / entry_size)
		return (false);
	toc_size = p->toc_capacity != 0 ? p->toc_capacity :
	    (size_t)p->count * entry_size;
	if (toc_size < (size_t)p->count * entry_size ||
	    toc_size > value_end - GEN_APFS_NODE_HEADER)
		return (false);
	key_start = GEN_APFS_NODE_HEADER + toc_size;
	available = value_end - key_start;
	for (uint32_t i = 0; i < p->count; i++) {
		record = &p->records[i];
		if ((record->key_size != 0 && record->key == NULL) ||
		    (!record->ghost && record->value_size != 0 &&
		    record->value == NULL))
			return (false);
		if (fixed && record->key_size != p->records[0].key_size)
			return (false);
		if (fixed && !record->ghost) {
			if (have_value_size && record->value_size !=
			    fixed_value_size)
				return (false);
			fixed_value_size = record->value_size;
			have_value_size = true;
		}
		if (record->key_size > available)
			return (false);
		available -= record->key_size;
		key_bytes += record->key_size;
		if (!record->ghost) {
			if (record->value_size > available)
				return (false);
			available -= record->value_size;
			value_bytes += record->value_size;
		}
	}
	if (toc_size > UINT16_MAX || key_bytes > UINT16_MAX ||
	    available > UINT16_MAX || value_bytes >= GEN_APFS_GHOST)
		return (false);

	write_object(raw, &p->object);
	put_le(raw + 32, p->flags, 2);
	put_le(raw + 34, p->level, 2);
	put_le(raw + 36, p->count, 4);
	put_le(raw + 40, 0, 2);
	put_le(raw + 42, toc_size, 2);
	put_le(raw + 44, key_bytes, 2);
	put_le(raw + 46, available, 2);
	put_le(raw + 48, GEN_APFS_FREE_LIST_EMPTY, 2);
	put_le(raw + 50, 0, 2);
	put_le(raw + 52, GEN_APFS_FREE_LIST_EMPTY, 2);
	put_le(raw + 54, 0, 2);
	key_cursor = key_start;
	value_cursor = value_end;
	for (uint32_t i = 0; i < p->count; i++) {
		record = &p->records[i];
		put_le(raw + 56 + i * entry_size, key_cursor - key_start, 2);
		if (record->key_size != 0)
			bcopy(record->key, raw + key_cursor, record->key_size);
		key_cursor += record->key_size;
		if (!record->ghost) {
			value_cursor -= record->value_size;
			if (record->value_size != 0)
				bcopy(record->value, raw + value_cursor,
				    record->value_size);
		}
		if (fixed) {
			put_le(raw + 58 + i * entry_size, record->ghost ?
			    GEN_APFS_GHOST : value_end - value_cursor, 2);
		} else {
			put_le(raw + 58 + i * entry_size, record->key_size, 2);
			put_le(raw + 60 + i * entry_size, record->ghost ?
			    GEN_APFS_GHOST : value_end - value_cursor, 2);
			put_le(raw + 62 + i * entry_size, record->ghost ? 0 :
			    record->value_size, 2);
		}
	}
	if (root)
		write_info(raw + value_end, &p->info);
	return (apfs_gen_seal(raw, length));
}

bool
apfs_gen_reap_list_build(unsigned char *raw, size_t length,
    const apfs_gen_reap_list_t *p)
{
	const apfs_gen_reap_entry_t *entry;
	unsigned char *out;

	if (!block_fits(length, 64) || p->capacity > (length - 64) / 40 ||
	    p->count > p->capacity || (p->count != 0 && p->entries == NULL))
		return (false);
	write_object(raw, &p->object);
	put_le(raw + 32, p->next, 8);
	put_le(raw + 40, p->flags, 4);
	put_le(raw + 44, p->capacity, 4);
	put_le(raw + 48, p->count, 4);
	put_le(raw + 52, p->first, 4);
	put_le(raw + 56, p->last, 4);
	put_le(raw + 60, p->free, 4);
	for (uint32_t i = 0; i < p->count; i++) {
		entry = &p->entries[i];
		out = raw + 64 + 40 * (size_t)i;
		put_le(out, entry->next, 4);
		put_le(out + 4, entry->flags, 4);
		put_le(out + 8, entry->kind, 4);
		put_le(out + 12, entry->size, 4);
		put_le(out + 16, entry->volume_oid, 8);
		put_le(out + 24, entry->oid, 8);
		put_le(out + 32, entry->xid, 8);
	}
	return (apfs_gen_seal(raw, length));
}

/* Geometry from the format relations, without using the APFS driver. */
static size_t
spaceman_geometry(uint32_t b, uint64_t n, uint32_t structure_end,
    apfs_gen_spaceman_t *sm)
{
	uint64_t chunks, cibs, cabs, pool, bitmap, end;

	if (b < 4096 || b > 65536 || (b & (b - 1)) != 0 || n == 0)
		return (0);
	chunks = n / (8 * b) + (n % (8 * b) != 0);
	cibs = (chunks + (b - 40) / 32 - 1) / ((b - 40) / 32);
	cabs = cibs <= (b - 40) / 8 ? 0 :
	    (cibs + (b - 40) / 8 - 1) / ((b - 40) / 8);
	pool = 3 * (chunks + cibs + cabs);
	bitmap = (pool + 8 * b - 1) / (8 * b);
	if (cibs > UINT32_MAX || cabs > UINT32_MAX || bitmap * 16 >= 65535)
		return (0);
	*sm = (apfs_gen_spaceman_t){ .block_size = b,
	    .blocks_per_chunk = 8 * b, .chunks_per_cib = (b - 40) / 32,
	    .cibs_per_cab = (b - 40) / 8,
	    .struct_size = structure_end == 336 ? 0 : structure_end,
	    .ip_block_count = pool, .ip_bm_size_in_blocks = (uint32_t)bitmap,
	    .ip_bm_block_count = (uint32_t)(16 * bitmap),
	    .ip_bm_tx_multiplier = 16, .ip_bm_xid_offset = structure_end,
	    .layout = structure_end == 336 ? APFS_GEN_LAYOUT_OLDER :
	    APFS_GEN_LAYOUT_CURRENT };
	sm->devices[0] = (apfs_gen_device_t){ .block_count = n,
	    .chunk_count = chunks, .cib_count = (uint32_t)cibs,
	    .cab_count = (uint32_t)cabs };
	sm->ip_bitmap_offset = (uint32_t)(structure_end + 8 * bitmap);
	end = sm->ip_bitmap_offset + 2 * bitmap;
	sm->ip_bm_free_next_offset = (uint32_t)((end + 7) / 8 * 8);
	sm->devices[0].addr_offset = sm->ip_bm_free_next_offset +
	    2 * sm->ip_bm_block_count;
	end = sm->devices[0].addr_offset + 8 * (cabs != 0 ? cabs : cibs);
	sm->devices[1].addr_offset = (uint32_t)end;
	return ((size_t)((end + b - 1) / b * b));
}

size_t
apfs_gen_spaceman_geometry(uint32_t b, uint64_t n, apfs_gen_spaceman_t *sm)
{
	return (spaceman_geometry(b, n, 2520, sm));
}

size_t
apfs_gen_spaceman_geometry_older(uint32_t b, uint64_t n,
    apfs_gen_spaceman_t *sm)
{
	return (spaceman_geometry(b, n, 336, sm));
}

/* Emit into a private image; the public builder publishes only on success. */
static bool
container_emit(unsigned char *raw, size_t length,
    const apfs_gen_container_t *p)
{
	const apfs_gen_mapping_t *mapping;
	const apfs_gen_run_t *run;
	apfs_gen_checkpoint_t map;
	apfs_gen_reaper_t reaper;
	apfs_gen_spaceman_t spaceman;
	apfs_gen_reap_list_t list;
	apfs_gen_object_t object;
	apfs_gen_nx_t nx;
	size_t b = p->nx.block_size, capacity, first, count;
	uint64_t address;

	if (b < 4096 || b > 65536 || (b & (b - 1)) != 0 ||
	    p->nx.block_count > length / b || p->count == 0 ||
	    p->runs == NULL || p->block0_run >= p->count ||
	    p->nx.desc_blocks == 0 || p->nx.data_blocks == 0 ||
	    p->nx.desc_base == 0 || p->nx.data_base == 0 ||
	    p->nx.desc_base >= length / b ||
	    p->nx.desc_blocks > length / b - p->nx.desc_base ||
	    p->nx.data_base >= length / b ||
	    p->nx.data_blocks > length / b - p->nx.data_base)
		return (false);
	capacity = (b - 40) / 40;
	for (size_t i = 0; i < p->count; i++) {
		run = &p->runs[i];
		if (run->mapping_blocks == 0 ||
		    run->mapping_blocks >= p->nx.desc_blocks ||
		    run->desc_index >= p->nx.desc_blocks ||
		    run->mapping_count > capacity * run->mapping_blocks ||
		    (run->mapping_count != 0 && run->mappings == NULL))
			return (false);
		for (uint32_t j = 0; j < run->mapping_count; j++) {
			mapping = &run->mappings[j];
			if (mapping->size == 0 || mapping->size % b != 0 ||
			    mapping->address == 0 ||
			    mapping->address >= length / b ||
			    mapping->size / b > length / b - mapping->address)
				return (false);
		}
	}
	for (size_t i = 0; i < p->count; i++) {
		run = &p->runs[i];
		nx = p->nx;
		nx.object = (apfs_gen_object_t){ .oid = 1, .xid = run->xid,
		    .type = 0x80000001 };
		nx.next_xid = run->next_xid;
		nx.desc_index = run->desc_index;
		nx.desc_len = run->mapping_blocks + 1;
		nx.data_index = run->data_index;
		nx.data_len = run->data_len;
		nx.desc_next = run->desc_next;
		nx.data_next = run->data_next;
		for (uint32_t j = 0; j < run->mapping_blocks; j++) {
			address = nx.desc_base +
			    (run->desc_index + (uint64_t)j) % nx.desc_blocks;
			first = j * capacity;
			count = first < run->mapping_count ?
			    run->mapping_count - first : 0;
			if (count > capacity)
				count = capacity;
			map = (apfs_gen_checkpoint_t){
			    .object = { .oid = address, .xid = run->xid,
			    .type = 0x4000000c },
			    .mappings = count == 0 ? NULL :
			    run->mappings + first,
			    .count = (uint32_t)count,
			    .flags = j + 1 == run->mapping_blocks ? 1 : 0 };
			if (!apfs_gen_checkpoint_build(raw + address * b, b,
			    &map))
				return (false);
		}
		address = nx.desc_base + (run->desc_index +
		    (uint64_t)run->mapping_blocks) % nx.desc_blocks;
		if (!apfs_gen_nx_build(raw + address * b, b, &nx))
			return (false);
		if (i == p->block0_run) {
			nx.object.xid = p->block0_xid;
			if (!apfs_gen_nx_build(raw, b, &nx))
				return (false);
		}
		for (uint32_t j = 0; j < run->mapping_count; j++) {
			mapping = &run->mappings[j];
			object = (apfs_gen_object_t){ .oid = mapping->oid,
			    .xid = run->xid, .type = mapping->type,
			    .subtype = mapping->subtype };
			if ((mapping->type & 0xffff) == 5) {
				if (apfs_gen_spaceman_geometry((uint32_t)b,
				    nx.block_count, &spaceman) == 0)
					return (false);
				spaceman.object = object;
				if (p->spaceman_ring_blocks != 0) {
					spaceman.ip_bm_block_count =
					    p->spaceman_ring_blocks;
					spaceman.devices[0].addr_offset =
					    spaceman.ip_bm_free_next_offset +
					    2 * spaceman.ip_bm_block_count;
					spaceman.devices[1].addr_offset =
					    spaceman.devices[0].addr_offset +
					    8 *
					    (spaceman.devices[0].cab_count !=
					    0 ? spaceman.devices[0].cab_count :
					    spaceman.devices[0].cib_count);
				}
				if (!apfs_gen_spaceman_build(raw +
				    mapping->address * b,
				    mapping->size, &spaceman))
					return (false);
			} else if ((mapping->type & 0xffff) == 18) {
				list = (apfs_gen_reap_list_t){ .object = object,
				    .capacity = (mapping->size - 64) / 40,
				    .first = UINT32_MAX, .last = UINT32_MAX };
				if (!apfs_gen_reap_list_build(raw +
				    mapping->address * b,
				    mapping->size, &list))
					return (false);
			} else if ((mapping->type & 0xffff) == 17) {
				reaper = (apfs_gen_reaper_t){ .object = object,
				    .flags = 1, .next_reap_id = 1 };
				if (!apfs_gen_reaper_build(raw +
				    mapping->address * b,
				    mapping->size, &reaper))
					return (false);
			} else {
				if (!apfs_gen_object_build(raw +
				    mapping->address * b,
				    mapping->size, &object))
					return (false);
			}
		}
	}
	return (true);
}

bool
apfs_gen_container_build(unsigned char *raw, size_t length,
    const apfs_gen_container_t *p)
{
	unsigned char *image;
	bool success;

	image = malloc(length);
	if (image == NULL)
		return (false);
	bcopy(raw, image, length);
	success = container_emit(image, length, p);
	if (success)
		bcopy(image, raw, length);
	free(image);
	return (success);
}


/* Leaves precede parents; each index groups nodes of the preceding level. */
static bool
tree_plan_build(tree_plan_t *plan, size_t capacity, size_t record_count,
    unsigned per_leaf, unsigned per_index, size_t *node_count)
{
	size_t count = 0, start, end, first, n;

	for (size_t i = 0; i < record_count;) {
		n = record_count - i;
		if (n > per_leaf)
			n = per_leaf;
		if (count == capacity)
			return (false);
		plan[count++] = (tree_plan_t){ .first = i, .count = n,
		    .key = i };
		i += n;
	}
	start = 0;
	end = count;
	while (end - start > 1) {
		first = count;
		for (size_t i = start; i < end;) {
			n = end - i;
			if (n > per_index)
				n = per_index;
			if (count == capacity || plan[i].level == 15)
				return (false);
			plan[count++] = (tree_plan_t){ .first = i, .count = n,
			    .key = plan[i].key, .level = plan[i].level + 1 };
			i += n;
		}
		start = first;
		end = count;
	}
	*node_count = count;
	return (true);
}

bool
apfs_gen_tree_build(unsigned char *raw, size_t length,
    const apfs_gen_tree_t *p, uint64_t *root, uint64_t *blocks)
{
	tree_plan_t *plan = NULL;
	apfs_gen_record_t *records = NULL;
	unsigned char *keys = NULL, *values = NULL, *image = NULL, *out;
	apfs_gen_node_t node;
	size_t b = p->block_size, capacity, count = 0, n, first;
	size_t width, key_start, value_end;
	bool success = false;

	if (b < 4096 || b > 65536 || (b & (b - 1)) != 0 ||
	    p->base == 0 || p->base >= length / b || p->count == 0 ||
	    p->records == NULL || p->records_per_leaf == 0 ||
	    p->children_per_index < 2)
		return (false);
	capacity = length / b - p->base;
	if ((p->count - 1) / p->records_per_leaf >= capacity)
		return (false);
	for (size_t i = 1; i < p->count; i++) {
		if (p->records[i - 1].key.oid > p->records[i].key.oid ||
		    (p->records[i - 1].key.oid == p->records[i].key.oid &&
		    p->records[i - 1].key.xid >= p->records[i].key.xid))
			return (false);
	}
	width = p->records_per_leaf > p->children_per_index ?
	    p->records_per_leaf : p->children_per_index;
	if (width > (b - 96) / 36)
		return (false);
	plan = calloc(capacity, sizeof(*plan));
	records = calloc(width, sizeof(*records));
	keys = calloc(width, 16);
	values = calloc(width, 16);
	image = calloc(capacity, b);
	if (plan == NULL || records == NULL || keys == NULL ||
	    values == NULL || image == NULL)
		goto done;
	if (!tree_plan_build(plan, capacity, p->count, p->records_per_leaf,
	    p->children_per_index, &count))
		goto done;
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < plan[i].count; j++) {
			first = plan[i].first + j;
			n = plan[i].level == 0 ? first : plan[first].key;
			apfs_gen_omap_key_build(keys + 16 * j, 16,
			    &p->records[n].key);
			if (plan[i].level == 0)
				apfs_gen_omap_value_build(values + 16 * j, 16,
				    &p->records[n].value);
			else
				put_le(values + 16 * j, p->base + first, 8);
			records[j] = (apfs_gen_record_t){ .key = keys + 16 * j,
			    .value = values + 16 * j, .key_size = 16,
			    .value_size = plan[i].level == 0 ? 16 : 8 };
		}
		node = (apfs_gen_node_t){
		    .object = { .oid = p->base + i, .xid = p->xid,
		    .type = i + 1 == count ? 0x40000002 : 0x40000003,
		    .subtype = 11 },
		    .info = { .flags = 0x17 | (p->nonaligned ? 0x40 : 0),
		    .node_size = (uint32_t)b, .key_size = 16, .val_size = 16,
		    .key_count = p->count, .node_count = count },
		    .records = records, .count = (uint32_t)plan[i].count,
		    .level = plan[i].level,
		    .flags = 4 | (plan[i].level == 0 ? 2 : 0) |
		    (i + 1 == count ? 1 : 0), .toc_capacity = p->toc_capacity };
		out = image + i * b;
		if (!apfs_gen_node_build(out, b, &node))
			goto done;
		if (p->nonaligned) {
			/* Shift keys into free space; all relative offsets
			 * become odd.
			 */
			key_start = 56 + (p->toc_capacity != 0 ?
			    p->toc_capacity : 4 * plan[i].count);
			value_end = b - (i + 1 == count ? 40 : 0);
			if (key_start + 16 * plan[i].count + 1 >=
			    value_end - records[0].value_size * plan[i].count)
				goto done;
			memmove(out + key_start + 1, out + key_start,
			    16 * plan[i].count);
			out[key_start] = 0;
			put_le(out + 44, 16 * plan[i].count + 1, 2);
			put_le(out + 46, value_end - key_start -
			    (16 + records[0].value_size) *
			    plan[i].count - 1, 2);
			for (size_t j = 0; j < plan[i].count; j++)
				put_le(out + 56 + 4 * j, 16 * j + 1, 2);
			apfs_gen_seal(out, b);
		}
	}
	bcopy(image, raw + p->base * b, count * b);
	*root = p->base + count - 1;
	*blocks = count;
	success = true;
 done:
	free(image);
	free(values);
	free(keys);
	free(records);
	free(plan);
	return (success);
}

bool
apfs_gen_fs_tree_build(unsigned char *raw, size_t length,
    const apfs_gen_fs_tree_t *p, apfs_gen_omap_record_t *maps,
    size_t map_capacity, uint64_t *root, uint64_t *blocks)
{
	tree_plan_t *plan = NULL;
	apfs_gen_record_t *records = NULL;
	unsigned char *values = NULL, *image = NULL;
	apfs_gen_node_t node;
	size_t b = p->block_size, count = 0, capacity, first;
	size_t width;
	uint32_t longest_key = 0, longest_val = 8;
	bool success = false;

	if (b < 4096 || b > 65536 || (b & (b - 1)) != 0 ||
	    p->base == 0 || p->base >= length / b || p->count == 0 ||
	    p->records == NULL || p->records_per_leaf == 0 ||
	    p->children_per_index < 2 || p->first_oid == 0 || maps == NULL)
		return (false);
	capacity = length / b - p->base;
	if (capacity > map_capacity)
		capacity = map_capacity;
	if (capacity == 0 || (p->count - 1) / p->records_per_leaf >= capacity ||
	    capacity - 1 > UINT64_MAX - p->first_oid)
		return (false);
	width = p->records_per_leaf > p->children_per_index ?
	    p->records_per_leaf : p->children_per_index;
	plan = calloc(capacity, sizeof(*plan));
	records = calloc(width, sizeof(*records));
	values = calloc(width, 8);
	image = calloc(capacity, b);
	if (plan == NULL || records == NULL || values == NULL || image == NULL)
		goto done;
	for (size_t i = 0; i < p->count; i++) {
		if (p->records[i].key_size > longest_key)
			longest_key = p->records[i].key_size;
		if (p->records[i].value_size > longest_val)
			longest_val = p->records[i].value_size;
	}
	if (!tree_plan_build(plan, capacity, p->count, p->records_per_leaf,
	    p->children_per_index, &count))
		goto done;
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < plan[i].count; j++) {
			first = plan[i].first + j;
			if (plan[i].level == 0) {
				records[j] = p->records[first];
				continue;
			}
			put_le(values + 8 * j, p->first_oid + first, 8);
			records[j] = (apfs_gen_record_t){
			    .key = p->records[plan[first].key].key,
			    .key_size = p->records[plan[first].key].key_size,
			    .value = values + 8 * j, .value_size = 8 };
		}
		/* Packed spans are nonaligned; virtual storage has no flag. */
		node = (apfs_gen_node_t){
		    .object = { .oid = p->first_oid + i, .xid = p->xid,
		    .type = i + 1 == count ? 2 : 3, .subtype = 14 },
		    .info = { .flags = 0x40, .node_size = (uint32_t)b,
		    .longest_key = longest_key, .longest_val = longest_val,
		    .key_count = p->count, .node_count = count },
		    .records = records, .count = (uint32_t)plan[i].count,
		    .level = plan[i].level,
		    .flags = (plan[i].level == 0 ? GEN_APFS_LEAF : 0) |
		    (i + 1 == count ? GEN_APFS_ROOT : 0) };
		if (!apfs_gen_node_build(image + i * b, b, &node))
			goto done;
	}
	bcopy(image, raw + p->base * b, count * b);
	for (size_t i = 0; i < count; i++)
		maps[i] = (apfs_gen_omap_record_t){
		    .key = { .oid = p->first_oid + i, .xid = p->xid },
		    .value = { .size = (uint32_t)b, .address = p->base + i } };
	*root = p->first_oid + count - 1;
	*blocks = count;
	success = true;
 done:
	free(image);
	free(values);
	free(records);
	free(plan);
	return (success);
}

bool
apfs_gen_dstream_build(unsigned char *raw, size_t length,
    const apfs_gen_dstream_t *p)
{
	if (length != GEN_DSTREAM_BYTES)
		return (false);
	bzero(raw, length);
	put_le(raw, p->size, 8);
	put_le(raw + 8, p->alloced_size, 8);
	put_le(raw + 16, p->default_crypto_id, 8);
	put_le(raw + 24, p->total_bytes_written, 8);
	put_le(raw + 32, p->total_bytes_read, 8);
	return (true);
}

bool
apfs_gen_dir_stats_build(unsigned char *raw, size_t length,
    const apfs_gen_dir_stats_t *p)
{
	if (length != GEN_DIR_STATS_BYTES)
		return (false);
	bzero(raw, length);
	put_le(raw, p->num_children, 8);
	put_le(raw + 8, p->total_size, 8);
	put_le(raw + 16, p->chained_key, 8);
	put_le(raw + 24, p->gen_count, 8);
	return (true);
}

bool
apfs_gen_inode_val_build(unsigned char *raw, size_t length,
    const apfs_gen_inode_val_t *p)
{
	if (length < GEN_INODE_VAL_BYTES ||
	    length - GEN_INODE_VAL_BYTES != p->xfields_len)
		return (false);
	bzero(raw, length);
	put_le(raw, p->parent, 8);
	put_le(raw + 8, p->private_id, 8);
	put_le(raw + 16, p->create_time, 8);
	put_le(raw + 24, p->mod_time, 8);
	put_le(raw + 32, p->change_time, 8);
	put_le(raw + 40, p->access_time, 8);
	put_le(raw + 48, p->internal_flags, 8);
	put_le(raw + 56, p->count, 4);
	put_le(raw + 60, p->protection_class, 4);
	put_le(raw + 64, p->write_generation, 4);
	put_le(raw + 68, p->bsd_flags, 4);
	put_le(raw + 72, p->owner, 4);
	put_le(raw + 76, p->group, 4);
	put_le(raw + 80, p->mode, 2);
	put_le(raw + 82, 0, 2);
	put_le(raw + 84, p->uncompressed_size, 8);
	if (p->xfields_len != 0)
		bcopy(p->xfields, raw + 92, p->xfields_len);
	return (true);
}

bool
apfs_gen_drec_val_build(unsigned char *raw, size_t length,
    const apfs_gen_drec_val_t *p)
{
	if (length < GEN_DREC_VAL_BYTES ||
	    length - GEN_DREC_VAL_BYTES != p->xfields_len)
		return (false);
	bzero(raw, length);
	put_le(raw, p->child, 8);
	put_le(raw + 8, p->date_added, 8);
	put_le(raw + 16, p->flags, 2);
	if (p->xfields_len != 0)
		bcopy(p->xfields, raw + 18, p->xfields_len);
	return (true);
}

bool
apfs_gen_extent_val_build(unsigned char *raw, size_t length,
    const apfs_gen_extent_val_t *p)
{
	if (length != GEN_EXTENT_VAL_BYTES ||
	    p->length > UINT64_C(0x00ffffffffffffff))
		return (false);
	bzero(raw, length);
	put_le(raw, p->length | (uint64_t)p->flags << 56, 8);
	put_le(raw + 8, p->phys_block, 8);
	put_le(raw + 16, p->crypto_id, 8);
	return (true);
}

bool
apfs_gen_pext_val_build(unsigned char *raw, size_t length,
    const apfs_gen_pext_val_t *p)
{
	if (length != GEN_PEXT_VAL_BYTES ||
	    p->length > GEN_JKEY_OID_MASK || p->kind > 15)
		return (false);
	bzero(raw, length);
	put_le(raw, p->length | (uint64_t)p->kind << 60, 8);
	put_le(raw + 8, p->owner, 8);
	put_le(raw + 16, p->refcnt, 4);
	return (true);
}

bool
apfs_gen_snap_meta_val_build(unsigned char *raw, size_t length,
    const apfs_gen_snap_meta_val_t *p)
{
	if (length < GEN_SNAP_META_VAL_BYTES ||
	    length - GEN_SNAP_META_VAL_BYTES != (size_t)p->name_len + 1)
		return (false);
	bzero(raw, length);
	put_le(raw, p->extentref_tree_oid, 8);
	put_le(raw + 8, p->sblock_oid, 8);
	put_le(raw + 16, p->create_time, 8);
	put_le(raw + 24, p->change_time, 8);
	put_le(raw + 32, p->inum, 8);
	put_le(raw + 40, p->extentref_tree_type, 4);
	put_le(raw + 44, p->flags, 4);
	put_le(raw + 48, (uint64_t)p->name_len + 1, 2);
	bcopy(p->name, raw + 50, p->name_len);
	return (true);
}

bool
apfs_gen_file_info_val_build(unsigned char *raw, size_t length,
    const apfs_gen_file_info_val_t *p)
{
	if (length < GEN_FILE_INFO_VAL_BYTES ||
	    length - GEN_FILE_INFO_VAL_BYTES != p->hash_size)
		return (false);
	bzero(raw, length);
	put_le(raw, p->hashed_len, 2);
	put_le(raw + 2, p->hash_size, 1);
	if (p->hash_size != 0)
		bcopy(p->hash, raw + 3, p->hash_size);
	return (true);
}

bool
apfs_gen_jkey_build(unsigned char *raw, size_t length, uint64_t oid,
    uint8_t kind)
{
	if (length != 8 || oid > GEN_JKEY_OID_MASK || kind > 15)
		return (false);
	put_le(raw, oid | (uint64_t)kind << 60, 8);
	return (true);
}

bool
apfs_gen_xfields_build(unsigned char *raw, size_t length,
    const apfs_gen_xfield_t *fields, uint16_t count)
{
	size_t used = 0, at;

	for (uint16_t i = 0; i < count; i++)
		used += ((size_t)fields[i].size + 7) / 8 * 8;
	if (used > UINT16_MAX || length != 4 + (size_t)count * 4 + used)
		return (false);
	bzero(raw, length);
	put_le(raw, count, 2);
	put_le(raw + 2, used, 2);
	at = 4 + (size_t)count * 4;
	for (uint16_t i = 0; i < count; i++) {
		raw[4 + i * 4] = fields[i].type;
		raw[5 + i * 4] = fields[i].flags;
		put_le(raw + 6 + i * 4, fields[i].size, 2);
		if (fields[i].size != 0)
			bcopy(fields[i].data, raw + at, fields[i].size);
		at += ((size_t)fields[i].size + 7) / 8 * 8;
	}
	return (true);
}

bool
apfs_gen_drec_key_build(unsigned char *raw, size_t length,
    const apfs_gen_drec_key_t *p)
{
	size_t fixed = p->hashed ? 12 : 10;

	if (length != fixed + p->name_len + 1 ||
	    p->parent > GEN_JKEY_OID_MASK)
		return (false);
	bzero(raw, length);
	(void)apfs_gen_jkey_build(raw, 8, p->parent, 9);
	put_le(raw + 8, p->hashed ? (uint64_t)p->hash << 10 |
	    (p->name_len + 1u) : p->name_len + 1u, p->hashed ? 4 : 2);
	bcopy(p->name, raw + fixed, p->name_len);
	return (true);
}

static bool
build_name_key(unsigned char *raw, size_t length,
    const apfs_gen_name_key_t *p, uint8_t kind)
{
	if (length != 10 + (size_t)p->name_len + 1 ||
	    p->oid > GEN_JKEY_OID_MASK)
		return (false);
	bzero(raw, length);
	(void)apfs_gen_jkey_build(raw, 8, p->oid, kind);
	put_le(raw + 8, (uint64_t)p->name_len + 1, 2);
	bcopy(p->name, raw + 10, p->name_len);
	return (true);
}

bool
apfs_gen_xattr_key_build(unsigned char *raw, size_t length,
    const apfs_gen_name_key_t *p)
{
	return (build_name_key(raw, length, p, 4));
}

bool
apfs_gen_snap_name_key_build(unsigned char *raw, size_t length,
    const apfs_gen_name_key_t *p)
{
	return (build_name_key(raw, length, p, 11));
}

bool
apfs_gen_xattr_val_build(unsigned char *raw, size_t length,
    const apfs_gen_xattr_val_t *p)
{
	bool stream = (p->flags & 1) != 0;

	if (length != 4 + (size_t)p->data_len ||
	    (stream && p->data_len != 48))
		return (false);
	bzero(raw, length);
	put_le(raw, p->flags, 2);
	put_le(raw + 2, p->data_len, 2);
	if (stream) {
		put_le(raw + 4, p->stream_oid, 8);
		(void)apfs_gen_dstream_build(raw + 12, 40, &p->dstream);
	} else if (p->data_len != 0)
		bcopy(p->data, raw + 4, p->data_len);
	return (true);
}

bool
apfs_gen_sibling_val_build(unsigned char *raw, size_t length,
    const apfs_gen_sibling_val_t *p)
{
	if (length != 10 + (size_t)p->name_len + 1)
		return (false);
	bzero(raw, length);
	put_le(raw, p->parent, 8);
	put_le(raw + 8, (uint64_t)p->name_len + 1, 2);
	bcopy(p->name, raw + 10, p->name_len);
	return (true);
}

bool
apfs_gen_sibling_key_build(unsigned char *raw, size_t length,
    uint64_t oid, uint64_t suffix)
{
	if (length != 16 || oid > GEN_JKEY_OID_MASK)
		return (false);
	(void)apfs_gen_jkey_build(raw, 8, oid, 5);
	put_le(raw + 8, suffix, 8);
	return (true);
}

bool
apfs_gen_extent_key_build(unsigned char *raw, size_t length,
    uint64_t oid, uint64_t suffix)
{
	if (length != 16 || oid > GEN_JKEY_OID_MASK)
		return (false);
	(void)apfs_gen_jkey_build(raw, 8, oid, 8);
	put_le(raw + 8, suffix, 8);
	return (true);
}

bool
apfs_gen_sibling_map_val_build(unsigned char *raw, size_t length,
    uint64_t value)
{
	if (length != 8)
		return (false);
	put_le(raw, value, 8);
	return (true);
}

bool
apfs_gen_dstream_id_val_build(unsigned char *raw, size_t length,
    uint32_t value)
{
	if (length != 4)
		return (false);
	put_le(raw, value, 4);
	return (true);
}

bool
apfs_gen_snap_name_val_build(unsigned char *raw, size_t length,
    uint64_t value)
{
	if (length != 8)
		return (false);
	put_le(raw, value, 8);
	return (true);
}

bool
apfs_gen_file_info_key_build(unsigned char *raw, size_t length,
    uint64_t oid, uint64_t lba, uint8_t type)
{
	if (length != 16 || oid > GEN_JKEY_OID_MASK ||
	    lba > UINT64_C(0x00ffffffffffffff))
		return (false);
	(void)apfs_gen_jkey_build(raw, 8, oid, 13);
	put_le(raw + 8, lba | (uint64_t)type << 56, 8);
	return (true);
}

/* Bit-at-a-time CRC-32C over little-endian Unicode scalar values. Callers
 * supply the comparison form explicitly; this does no Unicode conversion.
 */
uint32_t
apfs_gen_name_hash(const uint32_t *scalars, size_t count)
{
	uint32_t crc = UINT32_MAX;

	for (size_t i = 0; i < count; i++) {
		for (unsigned byte = 0; byte < 4; byte++) {
			crc ^= (scalars[i] >> (8 * byte)) & 255;
			for (unsigned bit = 0; bit < 8; bit++)
				crc = (crc >> 1) ^ ((crc & 1) ?
				    UINT32_C(0x82f63b78) : 0);
		}
	}
	return (crc & UINT32_C(0x3fffff));
}

/*
 * A bounded test volume: fixed metadata regions leave blocks 256..1023 for
 * payload. Leaves have a caller-selected fixed record count (eight by default).
 */
#define VOLUME_BLOCK 4096u
#define VOLUME_BLOCKS 1024u
#define VOLUME_BYTES ((size_t)VOLUME_BLOCK * VOLUME_BLOCKS)
#define VOLUME_ENTRIES 64u
#define VOLUME_RECORDS (4 * VOLUME_ENTRIES) /* Total across all record kinds. */

/*
 * Fixed block layout (all addresses are container-relative):
 * 0       container superblock
 * 1..8    checkpoint descriptors
 * 9..12   checkpoint data: spaceman, reaper, two free-queue roots
 * 13..60  remaining checkpoint data capacity
 * 61..76  internal-pool bitmap ring
 * 77..82  internal pool: chunk-info block, allocation bitmap, spare blocks
 * 83..89  reserved gap
 * 90..91  container object map and its tree
 * 92..99  reserved gap
 * 100     volume superblock
 * 101     snapshot tree
 * 102     extent-reference tree
 * 103     volume object map
 * 104..127 volume object-map tree capacity
 * 128..255 filesystem tree capacity
 * 256..1023 file payload
 */
enum {
	VOLUME_SPACEMAN = 9,
	VOLUME_REAPER = 10,
	VOLUME_FREE_QUEUE_MAIN = 11,
	VOLUME_FREE_QUEUE_POOL = 12,
	VOLUME_POOL_BITMAP = 61,
	VOLUME_CHUNK_INFO = 77,
	VOLUME_ALLOCATION_BITMAP = 78,
	VOLUME_POOL_END = 83,
	VOLUME_CONTAINER_MAP = 90,
	VOLUME_CONTAINER_MAP_TREE = 91,
	VOLUME_SUPERBLOCK = 100,
	VOLUME_SNAPSHOT_TREE = 101,
	VOLUME_EXTENTREF_TREE = 102,
	VOLUME_MAP = 103,
	VOLUME_MAP_TREE = 104,
	VOLUME_TREE_BASE = 128,
	VOLUME_DATA_BASE = 256
};

enum {
	VOLUME_KIND_PHYSICAL_EXTENT = 2,
	VOLUME_XFIELD_NAME = 4,
	VOLUME_XFIELD_DSTREAM = 8,
	VOLUME_XFIELD_DO_NOT_COPY = 2,
	VOLUME_DREC_DIRECTORY = 4,
	VOLUME_DREC_FILE = 8,
	VOLUME_XATTR_EMBEDDED = 2,
	VOLUME_OMAP_MANUALLY_MANAGED = 1
};

typedef struct volume_entry {
	uint64_t oid;
	uint64_t parent;
	uint64_t size;
	uint64_t physical;
	unsigned char *data;
	char name[128];
	bool directory;
} volume_entry_t;

typedef struct volume_record {
	unsigned char key[256];
	unsigned char value[512];
	uint16_t key_size;
	uint16_t value_size;
} volume_record_t;

struct apfs_gen_volume {
	volume_entry_t entries[VOLUME_ENTRIES];
	volume_record_t records[VOLUME_RECORDS];
	size_t entry_count;
	size_t record_count;
	uint64_t attribute_oid;
	uint16_t records_per_leaf;
	uint16_t extent_blocks;
	bool built;
};

static bool volume_add(apfs_gen_volume_t *, const volume_entry_t *);
static bool volume_records_build(apfs_gen_volume_t *);
static bool volume_entry_records(apfs_gen_volume_t *, const volume_entry_t *);
static bool volume_inode_record(apfs_gen_volume_t *, const volume_entry_t *);
static bool volume_drec_record(apfs_gen_volume_t *, const volume_entry_t *);
static bool volume_stream_records(apfs_gen_volume_t *, const volume_entry_t *);
static int volume_record_compare(const void *, const void *);
static uint64_t volume_integer(const unsigned char *, unsigned);
static bool volume_map_build(unsigned char *, uint64_t,
    const apfs_gen_omap_record_t *, size_t, uint64_t *);
static bool volume_node_build(unsigned char *, uint64_t, uint32_t,
    const apfs_gen_record_t *, uint32_t);
static bool volume_allocation_build(unsigned char *, uint64_t, uint64_t);
static bool volume_image_build(apfs_gen_volume_t *, unsigned char *);

static uint64_t
volume_integer(const unsigned char *raw, unsigned width)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < width; i++)
		value |= (uint64_t)raw[i] << (8 * i);
	return (value);
}

apfs_gen_volume_t *
apfs_gen_volume_new(void)
{
	apfs_gen_volume_t *volume;

	volume = calloc(1, sizeof(*volume));
	if (volume == NULL)
		return (NULL);
	volume->records_per_leaf = 8;
	if (!apfs_gen_volume_directory(volume, 2, 1, "root"))
		goto fail;
	if (!apfs_gen_volume_directory(volume, 3, 1, "private"))
		goto fail;
	return (volume);
 fail:
	apfs_gen_volume_free(volume);
	return (NULL);
}

void
apfs_gen_volume_free(apfs_gen_volume_t *volume)
{
	if (volume == NULL)
		return;
	for (size_t i = 0; i < volume->entry_count; i++)
		free(volume->entries[i].data);
	free(volume);
}

static bool
volume_add(apfs_gen_volume_t *volume, const volume_entry_t *entry)
{
	bool parent_found = entry->parent == 1 && entry->oid <= 3;

	if (volume->built || volume->entry_count == VOLUME_ENTRIES ||
	    entry->oid < 2 || entry->oid > GEN_JKEY_OID_MASK ||
	    entry->name[0] == '\0' || strchr(entry->name, '/') != NULL ||
	    strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
		return (false);
	for (size_t i = 0; i < volume->entry_count; i++) {
		const volume_entry_t *other = &volume->entries[i];

		if (other->oid == entry->oid ||
		    (other->parent == entry->parent &&
		    strcmp(other->name, entry->name) == 0))
			return (false);
		if (other->oid == entry->parent && other->directory)
			parent_found = true;
	}
	if (!parent_found)
		return (false);
	volume->entries[volume->entry_count] = *entry;
	volume->entry_count++;
	return (true);
}

bool
apfs_gen_volume_directory(apfs_gen_volume_t *volume, uint64_t oid,
    uint64_t parent, const char *name)
{
	volume_entry_t entry = { .oid = oid, .parent = parent,
	    .directory = true };

	if (strlen(name) >= sizeof(entry.name))
		return (false);
	bcopy(name, entry.name, strlen(name) + 1);
	return (volume_add(volume, &entry));
}

bool
apfs_gen_volume_file(apfs_gen_volume_t *volume, uint64_t oid,
    uint64_t parent, const char *name, const unsigned char *data, size_t size)
{
	volume_entry_t entry = { .oid = oid, .parent = parent, .size = size };

	if (strlen(name) >= sizeof(entry.name) || size >
	    (VOLUME_BLOCKS - VOLUME_DATA_BASE) * VOLUME_BLOCK)
		return (false);
	bcopy(name, entry.name, strlen(name) + 1);
	if (data != NULL && size != 0) {
		entry.data = malloc(size);
		if (entry.data == NULL)
			return (false);
		bcopy(data, entry.data, size);
	}
	if (!volume_add(volume, &entry)) {
		free(entry.data);
		return (false);
	}
	return (true);
}

bool
apfs_gen_volume_layout(apfs_gen_volume_t *volume, uint16_t leaf_records,
    uint16_t extent_blocks)
{
	if (volume->built || leaf_records == 0 || leaf_records > 8)
		return (false);
	volume->records_per_leaf = leaf_records;
	volume->extent_blocks = extent_blocks;
	return (true);
}

bool
apfs_gen_volume_attribute(apfs_gen_volume_t *volume, uint64_t oid)
{
	if (volume->built || volume->attribute_oid != 0)
		return (false);
	for (size_t i = 0; i < volume->entry_count; i++) {
		if (volume->entries[i].oid == oid &&
		    !volume->entries[i].directory) {
			volume->attribute_oid = oid;
			return (true);
		}
	}
	return (false);
}

static bool
volume_inode_record(apfs_gen_volume_t *volume, const volume_entry_t *entry)
{
	unsigned char fields[256], stream[GEN_DSTREAM_BYTES];
	apfs_gen_xfield_t xf[] = {
		{ .type = VOLUME_XFIELD_NAME,
		.flags = VOLUME_XFIELD_DO_NOT_COPY,
		.data = (const unsigned char *)entry->name,
		.size = (uint16_t)(strlen(entry->name) + 1) },
		{ .type = VOLUME_XFIELD_DSTREAM,
		.data = stream, .size = sizeof(stream) }
	};
	uint64_t allocated = entry->physical == 0 ? 0 :
	    (entry->size + VOLUME_BLOCK - 1) / VOLUME_BLOCK * VOLUME_BLOCK;
	apfs_gen_dstream_t ds = { .size = entry->size,
	    .alloced_size = allocated };
	apfs_gen_inode_val_t inode = { .parent = entry->parent,
	    .private_id = entry->oid, .count = entry->directory ? 0 : 1,
	    .mode = entry->directory ? 0040755 : 0100644 };
	volume_record_t *r;
	uint16_t nfields = entry->directory ? 1 : 2;
	size_t length = 4 + 4 * nfields + (xf[0].size + 7u) / 8u * 8u;

	if (entry->directory) {
		for (size_t i = 0; i < volume->entry_count; i++)
			inode.count += volume->entries[i].parent == entry->oid;
	} else {
		if (!apfs_gen_dstream_build(stream, sizeof(stream), &ds))
			return (false);
		length += sizeof(stream);
	}
	if (!apfs_gen_xfields_build(fields, length, xf, nfields))
		return (false);
	inode.xfields = fields;
	inode.xfields_len = length;
	r = &volume->records[volume->record_count++];
	r->key_size = GEN_JKEY_BYTES;
	r->value_size = (uint16_t)(GEN_INODE_VAL_BYTES + length);
	if (!apfs_gen_jkey_build(r->key, r->key_size, entry->oid,
	    APFS_GEN_KIND_INODE))
		return (false);
	if (!apfs_gen_inode_val_build(r->value, r->value_size, &inode))
		return (false);

	return (true);
}

static bool
volume_drec_record(apfs_gen_volume_t *volume, const volume_entry_t *entry)
{
	apfs_gen_drec_key_t key = { .parent = entry->parent,
	    .name = entry->name, .name_len = (uint16_t)strlen(entry->name) };
	apfs_gen_drec_val_t drec = { .child = entry->oid,
	    .flags = entry->directory ?
	    VOLUME_DREC_DIRECTORY : VOLUME_DREC_FILE };
	volume_record_t *r;

	r = &volume->records[volume->record_count++];
	r->key_size = (uint16_t)(GEN_DREC_KEY_BYTES + key.name_len + 1);
	r->value_size = GEN_DREC_VAL_BYTES;
	if (!apfs_gen_drec_key_build(r->key, r->key_size, &key))
		return (false);
	if (!apfs_gen_drec_val_build(r->value, r->value_size, &drec))
		return (false);
	return (true);
}

static bool
volume_stream_records(apfs_gen_volume_t *volume, const volume_entry_t *entry)
{
	uint64_t allocated = entry->physical == 0 ? 0 :
	    (entry->size + VOLUME_BLOCK - 1) / VOLUME_BLOCK * VOLUME_BLOCK;
	apfs_gen_extent_val_t extent = { .length = allocated,
	    .phys_block = entry->physical };
	volume_record_t *r;

	r = &volume->records[volume->record_count++];
	r->key_size = GEN_JKEY_BYTES;
	r->value_size = GEN_DSTREAM_ID_VAL_BYTES;
	if (!apfs_gen_jkey_build(r->key, r->key_size, entry->oid,
	    APFS_GEN_KIND_DSTREAM_ID))
		return (false);
	if (!apfs_gen_dstream_id_val_build(r->value, r->value_size, 1))
		return (false);
	if (volume->attribute_oid == entry->oid) {
		apfs_gen_name_key_t name = { .oid = entry->oid,
		    .name = "user.test", .name_len = 9 };
		apfs_gen_xattr_val_t value = { .flags = VOLUME_XATTR_EMBEDDED,
		    .data_len = 3,
		    .data = (const unsigned char *)"abc" };

		if (volume->record_count == VOLUME_RECORDS)
			return (false);
		r = &volume->records[volume->record_count++];
		r->key_size = GEN_NAME_KEY_BYTES + 10;
		r->value_size = GEN_XATTR_VAL_BYTES + 3;
		if (!apfs_gen_xattr_key_build(r->key, r->key_size, &name))
			return (false);
		if (!apfs_gen_xattr_val_build(r->value, r->value_size, &value))
			return (false);
	}
	for (uint64_t offset = 0; offset < allocated; offset += extent.length) {
		if (volume->record_count == VOLUME_RECORDS)
			return (false);
		extent.length = allocated - offset;
		if (volume->extent_blocks != 0 && extent.length >
		    (uint64_t)volume->extent_blocks * VOLUME_BLOCK)
			extent.length = (uint64_t)volume->extent_blocks *
			    VOLUME_BLOCK;
		extent.phys_block = entry->physical + offset / VOLUME_BLOCK;
		r = &volume->records[volume->record_count++];
		r->key_size = GEN_EXTENT_KEY_BYTES;
		r->value_size = GEN_EXTENT_VAL_BYTES;
		if (!apfs_gen_extent_key_build(r->key, r->key_size, entry->oid,
		    offset))
			return (false);
		if (!apfs_gen_extent_val_build(r->value, r->value_size,
		    &extent))
			return (false);
	}
	return (true);
}

static bool
volume_entry_records(apfs_gen_volume_t *volume, const volume_entry_t *entry)
{
	if (!volume_inode_record(volume, entry))
		return (false);
	if (!volume_drec_record(volume, entry))
		return (false);
	if (entry->directory)
		return (true);
	return (volume_stream_records(volume, entry));
}

static int
volume_record_compare(const void *left, const void *right)
{
	const volume_record_t *a = left, *b = right;
	uint64_t ak = volume_integer(a->key, 8);
	uint64_t bk = volume_integer(b->key, 8);
	uint64_t ao = ak & GEN_JKEY_OID_MASK, bo = bk & GEN_JKEY_OID_MASK;
	uint8_t kind = (uint8_t)(ak >> 60);

	if (ao != bo)
		return (ao < bo ? -1 : 1);
	if (kind != bk >> 60)
		return (kind < bk >> 60 ? -1 : 1);
	if (kind == APFS_GEN_KIND_DREC)
		return (strcmp((const char *)a->key + GEN_DREC_KEY_BYTES,
		    (const char *)b->key + GEN_DREC_KEY_BYTES));
	if (kind == APFS_GEN_KIND_FILE_EXTENT) {
		ak = volume_integer(a->key + 8, 8);
		bk = volume_integer(b->key + 8, 8);
		return (ak < bk ? -1 : ak > bk);
	}
	return (0);
}

static bool
volume_records_build(apfs_gen_volume_t *volume)
{
	uint64_t physical = VOLUME_DATA_BASE, blocks;

	volume->record_count = 0;
	for (size_t i = 0; i < volume->entry_count; i++) {
		volume_entry_t *entry = &volume->entries[i];

		blocks = entry->data == NULL ? 0 :
		    (entry->size + VOLUME_BLOCK - 1) / VOLUME_BLOCK;
		if (blocks > VOLUME_BLOCKS - physical)
			return (false);
		entry->physical = blocks == 0 ? 0 : physical;
		physical += blocks;
		if (volume->record_count > VOLUME_RECORDS - 3)
			return (false);
		if (!volume_entry_records(volume, entry))
			return (false);
	}
	qsort(volume->records, volume->record_count,
	    sizeof(volume->records[0]), volume_record_compare);
	return (true);
}

static bool
volume_map_build(unsigned char *image, uint64_t address,
    const apfs_gen_omap_record_t *records, size_t count, uint64_t *blocks)
{
	apfs_gen_tree_t tree = { .records = records, .count = count,
	    .base = address + 1, .xid = 10, .block_size = VOLUME_BLOCK,
	    .records_per_leaf = 16, .children_per_index = 8 };
	apfs_gen_omap_t map = {
		.object = { .oid = address, .xid = 10, .type = 0x4000000b },
		.flags = address == VOLUME_CONTAINER_MAP ?
		    VOLUME_OMAP_MANUALLY_MANAGED : 0,
		.tree_type = 0x40000002
	};

	if (!apfs_gen_tree_build(image, VOLUME_BYTES, &tree,
	    &map.tree_oid, blocks))
		return (false);
	return (apfs_gen_omap_build(image + address * VOLUME_BLOCK,
	    VOLUME_BLOCK, &map));
}

static bool
volume_node_build(unsigned char *image, uint64_t address, uint32_t subtype,
    const apfs_gen_record_t *records, uint32_t count)
{
	apfs_gen_node_t node = {
		.object = { .oid = address, .xid = 10,
		.type = 0x40000002, .subtype = subtype },
		.flags = GEN_APFS_ROOT | GEN_APFS_LEAF,
		.info = { .flags = 0x50, .node_size = VOLUME_BLOCK,
		.key_count = count, .node_count = 1 },
		.records = records, .count = count
	};

	return (apfs_gen_node_build(image + address * VOLUME_BLOCK,
	    VOLUME_BLOCK, &node));
}

static bool
volume_allocation_build(unsigned char *image, uint64_t tree_blocks,
    uint64_t map_blocks)
{
	apfs_gen_spaceman_t sm;
	apfs_gen_chunk_t chunk = { .xid = 10, .block_count = VOLUME_BLOCKS,
	    .bitmap_address = VOLUME_ALLOCATION_BITMAP };
	apfs_gen_chunks_t cib = {
		.object = { .oid = VOLUME_CHUNK_INFO, .xid = 10,
		.type = 0x40000007 },
		.chunks = &chunk, .count = 1
	};
	unsigned char *bitmap = image + VOLUME_ALLOCATION_BITMAP * VOLUME_BLOCK;
	unsigned char *raw = image + VOLUME_SPACEMAN * VOLUME_BLOCK;
	bool used;

	if (apfs_gen_spaceman_geometry(VOLUME_BLOCK, VOLUME_BLOCKS,
	    &sm) != VOLUME_BLOCK)
		return (false);
	sm.object = (apfs_gen_object_t){ .oid = 500, .xid = 10,
	    .type = 0x80000005 };
	sm.ip_bm_base = VOLUME_POOL_BITMAP;
	sm.ip_base = VOLUME_CHUNK_INFO;
	sm.ip_bm_free_head = 1;
	sm.ip_bm_free_tail = 15;
	for (uint64_t i = 0; i < VOLUME_DATA_BASE; i++) {
		used = i < VOLUME_POOL_END || i == VOLUME_CONTAINER_MAP ||
		    i == VOLUME_CONTAINER_MAP_TREE ||
		    (i >= VOLUME_SUPERBLOCK &&
		    i < VOLUME_MAP_TREE + map_blocks) ||
		    (i >= VOLUME_TREE_BASE &&
		    i < VOLUME_TREE_BASE + tree_blocks);
		if (used)
			bitmap[i / 8] |= (unsigned char)(1u << (i % 8));
	}
	/* The two pool blocks hold the CIB and main allocation bitmap. */
	image[VOLUME_POOL_BITMAP * VOLUME_BLOCK] = 3;
	for (uint64_t i = 0; i < VOLUME_BLOCKS; i++)
		chunk.free_count += (bitmap[i / 8] & (1u << (i % 8))) == 0;
	sm.devices[0].free_count = chunk.free_count;
	if (!apfs_gen_chunks_build(image + VOLUME_CHUNK_INFO * VOLUME_BLOCK,
	    VOLUME_BLOCK, &cib))
		return (false);
	if (!apfs_gen_spaceman_build(raw, VOLUME_BLOCK, &sm))
		return (false);
	put_le(raw + sm.ip_bm_xid_offset, 10, 8);
	put_le(raw + sm.ip_bitmap_offset, 0, 2);
	for (uint32_t i = 0; i < sm.ip_bm_block_count; i++)
		put_le(raw + sm.ip_bm_free_next_offset + 2 * i,
		    i == 0 || i + 1 == sm.ip_bm_block_count ? 65535 : i + 1, 2);
	put_le(raw + sm.devices[0].addr_offset, VOLUME_CHUNK_INFO, 8);
	return (apfs_gen_seal(raw, VOLUME_BLOCK));
}

static bool
volume_image_build(apfs_gen_volume_t *volume, unsigned char *image)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = VOLUME_BLOCK,
		.oid = 500, .address = VOLUME_SPACEMAN },
		{ .type = 0x80000011, .size = VOLUME_BLOCK,
		.oid = 501, .address = VOLUME_REAPER },
		{ .type = 0x80000002, .subtype = 9, .size = VOLUME_BLOCK,
		.oid = 502, .address = VOLUME_FREE_QUEUE_MAIN },
		{ .type = 0x80000002, .subtype = 9, .size = VOLUME_BLOCK,
		.oid = 503, .address = VOLUME_FREE_QUEUE_POOL }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
	    .mappings = mappings, .mapping_count = 4, .mapping_blocks = 1,
	    .desc_next = 2, .data_len = 4, .data_next = 4 };
	apfs_gen_container_t container = {
		.nx = { .magic = 0x4253584e, .block_size = VOLUME_BLOCK,
		.block_count = VOLUME_BLOCKS, .uuid = { 1 },
		.incompat_features = 2,
		.desc_base = 1, .desc_blocks = 8, .data_base = VOLUME_SPACEMAN,
		.data_blocks = 52, .max_volumes = 1, .volume_oids = { 1000 },
		.spaceman_oid = 500, .reaper_oid = 501,
		.omap_oid = VOLUME_CONTAINER_MAP },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	apfs_gen_omap_record_t volume_map = {
		.key = { .oid = 1000, .xid = 10 },
		.value = { .size = VOLUME_BLOCK, .address = VOLUME_SUPERBLOCK }
	};
	apfs_gen_fs_t fs = {
		.object = { .oid = 1000, .xid = 10, .type = 13 },
		.magic = 0x42535041, .uuid = { 2 }, .name = "Contracts",
		.flags = 1, .root_tree_type = 2,
		.extentref_tree_type = 0x40000002,
		.snapshot_tree_type = 0x40000002, .omap_oid = VOLUME_MAP,
		.snapshot_tree_oid = VOLUME_SNAPSHOT_TREE,
		.extentref_tree_oid = VOLUME_EXTENTREF_TREE
	};
	apfs_gen_record_t records[VOLUME_RECORDS], extents[VOLUME_ENTRIES];
	apfs_gen_omap_record_t nodes[VOLUME_TREE_BASE - 1];
	unsigned char keys[VOLUME_ENTRIES][GEN_JKEY_BYTES];
	unsigned char values[VOLUME_ENTRIES][GEN_PEXT_VAL_BYTES];
	apfs_gen_fs_tree_t tree = { .records = records,
	    .count = volume->record_count, .first_oid = 2000,
	    .base = VOLUME_TREE_BASE, .xid = 10, .block_size = VOLUME_BLOCK,
	    .records_per_leaf = volume->records_per_leaf,
	    .children_per_index = 8 };
	uint64_t tree_blocks, map_blocks, blocks;
	uint32_t extent_count = 0;

	if (!apfs_gen_container_build(image, VOLUME_BYTES, &container))
		return (false);
	if (!volume_map_build(image, VOLUME_CONTAINER_MAP, &volume_map, 1,
	    &blocks))
		return (false);
	for (size_t i = 0; i < volume->record_count; i++) {
		const volume_record_t *r = &volume->records[i];

		records[i] = (apfs_gen_record_t){ .key = r->key,
		    .value = r->value, .key_size = r->key_size,
		    .value_size = r->value_size };
	}
	if (!apfs_gen_fs_tree_build(image, VOLUME_BYTES, &tree, nodes,
	    sizeof(nodes) / sizeof(nodes[0]), &fs.root_tree_oid, &tree_blocks))
		return (false);
	if (!volume_map_build(image, VOLUME_MAP, nodes, tree_blocks,
	    &map_blocks))
		return (false);
	if (VOLUME_MAP_TREE + map_blocks > VOLUME_TREE_BASE)
		return (false);
	fs.alloc_blocks = 3 + map_blocks + tree_blocks;
	for (size_t i = 0; i < volume->entry_count; i++) {
		const volume_entry_t *entry = &volume->entries[i];
		apfs_gen_pext_val_t extent = { .owner = entry->oid,
		    .refcnt = 1, .kind = 1 };

		if (entry->directory) {
			fs.directories += entry->oid > 3;
			continue;
		}
		fs.files++;
		if (entry->physical == 0)
			continue;
		blocks = (entry->size + VOLUME_BLOCK - 1) / VOLUME_BLOCK;
		extent.length = blocks;
		if (!apfs_gen_jkey_build(keys[extent_count], GEN_JKEY_BYTES,
		    entry->physical, VOLUME_KIND_PHYSICAL_EXTENT))
			return (false);
		if (!apfs_gen_pext_val_build(values[extent_count],
		    GEN_PEXT_VAL_BYTES, &extent))
			return (false);
		extents[extent_count] = (apfs_gen_record_t){
		    .key = keys[extent_count], .key_size = GEN_JKEY_BYTES,
		    .value = values[extent_count],
		    .value_size = GEN_PEXT_VAL_BYTES };
		extent_count++;
		bcopy(entry->data, image + entry->physical * VOLUME_BLOCK,
		    (size_t)entry->size);
		for (uint64_t j = entry->physical; j < entry->physical + blocks;
		    j++)
			image[VOLUME_ALLOCATION_BITMAP * VOLUME_BLOCK +
			    j / 8] |= (unsigned char)(1u << (j % 8));
		fs.alloc_blocks += blocks;
	}
	if (!volume_node_build(image, VOLUME_SNAPSHOT_TREE, 16, NULL, 0))
		return (false);
	if (!volume_node_build(image, VOLUME_EXTENTREF_TREE, 15, extents,
	    extent_count))
		return (false);
	if (!apfs_gen_fs_build(image + VOLUME_SUPERBLOCK * VOLUME_BLOCK,
	    VOLUME_BLOCK, &fs))
		return (false);
	return (volume_allocation_build(image, tree_blocks, map_blocks));
}

bool
apfs_gen_volume_build(apfs_gen_volume_t *volume, unsigned char **bytes,
    size_t *length)
{
	unsigned char *image;

	if (!volume_records_build(volume))
		return (false);
	image = calloc(1, VOLUME_BYTES);
	if (image == NULL)
		return (false);
	if (!volume_image_build(volume, image)) {
		free(image);
		return (false);
	}
	volume->built = true;
	*bytes = image;
	*length = VOLUME_BYTES;
	return (true);
}

bool
apfs_gen_volume_record_block(const apfs_gen_volume_t *volume, uint64_t oid,
    uint8_t kind, uint64_t suffix, uint64_t *block)
{
	if (!volume->built)
		return (false);
	for (size_t i = 0; i < volume->record_count; i++) {
		const volume_record_t *r = &volume->records[i];
		uint64_t key = volume_integer(r->key, 8);

		if ((key & GEN_JKEY_OID_MASK) != oid || key >> 60 != kind)
			continue;
		if (kind == APFS_GEN_KIND_FILE_EXTENT &&
		    volume_integer(r->key + 8, 8) != suffix)
			continue;
		*block = VOLUME_TREE_BASE + i / volume->records_per_leaf;
		return (true);
	}
	return (false);
}

bool
apfs_gen_volume_extent_block(const apfs_gen_volume_t *volume, uint64_t oid,
    uint64_t offset, uint64_t *block)
{
	if (!volume->built)
		return (false);
	for (size_t i = 0; i < volume->entry_count; i++) {
		const volume_entry_t *entry = &volume->entries[i];

		if (entry->oid != oid || entry->directory ||
		    offset >= entry->size)
			continue;
		*block = entry->physical == 0 ? 0 :
		    entry->physical + offset / VOLUME_BLOCK;
		return (true);
	}
	return (false);
}

/* Test framing uses a fixed resource data offset and contiguous payloads. */
void
apfs_gen_decmpfs_header(unsigned char *raw, uint32_t type, uint64_t size)
{
	put_le(raw, UINT32_C(0x636d7066), 4);
	put_le(raw + 4, type, 4);
	put_le(raw + 8, size, 8);
}

size_t
apfs_gen_compressed_payload(unsigned char *raw, size_t capacity,
    uint32_t type, const unsigned char *source, size_t size)
{
	uLongf stored = capacity;

	if (type == 3 || type == 4) {
		if (compress2(raw, &stored, source, size, Z_BEST_COMPRESSION) != Z_OK)
			return (0);
		return ((size_t)stored);
	}
	if (capacity < size + 1)
		return (0);
	raw[0] = type == 7 || type == 8 ? 0x06 :
	    type == 9 || type == 10 ? 0xcc : 0xff;
	bcopy(source, raw + 1, size);
	return (size + 1);
}

size_t
apfs_gen_compressed_fork(unsigned char *raw, size_t capacity, uint32_t type,
    const unsigned char *source, size_t size)
{
	size_t chunks = (size + 65535) / 65536;
	size_t cursor = type == 4 ? 24 + 8 * chunks : 4 * (chunks + 1);
	size_t stored, length, entry;

	if (cursor > capacity)
		return (0);
	bzero(raw, cursor);
	for (size_t i = 0; i < chunks; i++) {
		length = size - i * 65536;
		if (length > 65536)
			length = 65536;
		stored = apfs_gen_compressed_payload(raw + cursor,
		    capacity - cursor, type, source + i * 65536, length);
		if (stored == 0)
			return (0);
		if (type == 4) {
			entry = 24 + 8 * i;
			put_le(raw + entry, cursor - 20, 4);
			put_le(raw + entry + 4, stored, 4);
		} else
			put_le(raw + 4 * i, cursor, 4);
		cursor += stored;
	}
	if (type == 4) {
		/* Big-endian classic resource header and resource length. */
		const uint32_t words[] = { 16, (uint32_t)cursor,
		    (uint32_t)cursor - 16, 0, (uint32_t)cursor - 20 };

		for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
			for (unsigned j = 0; j < 4; j++)
				raw[4 * i + j] = (unsigned char)(words[i] >> (24 - 8 * j));
		}
		put_le(raw + 20, chunks, 4);
	} else
		put_le(raw + 4 * chunks, cursor, 4);
	return (cursor);
}
