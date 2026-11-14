/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "apfs-gen.h"

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
	put_le(raw + 336, p->version, 4);
	put_le(raw + 340, p->struct_size, 4);
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
	if (!block_fits(length, 2520))
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
