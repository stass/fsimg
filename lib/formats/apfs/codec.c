/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"

/*
 * Fletcher-64 and pure decoders and encoders of the APFS container records.
 * A public decoder first proves that every byte it reads lies inside the
 * length it was given, and that the fields framing the record (a count of
 * entries, a node's areas, a state buffer's size) agree with that length.
 * It then fills its output in full, so a rejected record leaves the output
 * untouched. An encoder proves the same of the record it is about to write
 * before it writes a byte, so it never writes past its length. Static
 * helpers take spans already proved.
 *
 * The decoders judge a record's shape, not its values: magic, kinds,
 * geometry, flags and identifiers are the validation of descriptor.c,
 * checkpoint.c, object.c, btree.c and omap.c.
 *
 * Counts and offsets are widened to uint64_t before they are multiplied or
 * added, so no product of a 32-bit count and an entry size can wrap, and a
 * sum is compared with the length only after it is formed exactly. Node
 * boundaries are the exception: a node is at most APFS_BLOCK_MAX bytes and
 * its offsets are 16-bit, so their sums fit 32 bits.
 */

#define FLETCHER_MODULUS	UINT64_C(0xffffffff)

/* The space manager's two device entries and three free-queue entries. */
#define SM_DEVICES_OFFSET	48u
#define SM_DEVICE_BYTES		48u
#define SM_FQS_OFFSET		200u
#define SM_FQ_BYTES		40u

static void	btree_info_load(const unsigned char *, apfs_btree_info_t *);
static void	btree_info_store(unsigned char *, const apfs_btree_info_t *);
static void	fs_change_decode(const unsigned char *, apfs_fs_change_t *);
static void	fs_change_encode(unsigned char *, const apfs_fs_change_t *);
static bool	cib_header_decode(const unsigned char *, size_t, uint32_t,
		    apfs_cib_t *);
static bool	cib_header_encode(unsigned char *, size_t, uint32_t,
		    const apfs_cib_t *);
static bool	entries_fit(size_t, uint32_t, uint32_t, uint32_t);
static bool	entry_offset(size_t, uint32_t, uint32_t, uint32_t, size_t *);
static void	nloc_decode(const unsigned char *, apfs_nloc_t *);
static void	nloc_encode(unsigned char *, const apfs_nloc_t *);
static bool	entry_resolve(const apfs_node_area_t *, bool,
		    const apfs_toc_t *, const apfs_kv_size_t *, apfs_entry_t *);
static bool	node_layout(size_t, const apfs_node_t *, apfs_node_area_t *);
static void	range_decode(const unsigned char *, apfs_range_t *);
static void	range_encode(unsigned char *, const apfs_range_t *);
static void	sm_device_decode(const unsigned char *, apfs_sm_device_t *);
static void	sm_device_encode(unsigned char *, const apfs_sm_device_t *);
static void	sm_fq_decode(const unsigned char *, apfs_sm_fq_t *);
static void	sm_fq_encode(unsigned char *, const apfs_sm_fq_t *);
static bool	toc_slot(size_t, const apfs_node_t *, uint32_t,
		    apfs_node_area_t *, size_t *);

/*
 * Whether a structure of header bytes followed by count entries of
 * entry_bytes each fits in length.
 */
static bool
entries_fit(size_t length, uint32_t header, uint32_t entry_bytes,
    uint32_t count)
{
	return ((uint64_t)header + (uint64_t)count * entry_bytes <= length);
}

/*
 * The offset of entry index in a structure of header bytes followed by
 * entries of entry_bytes each, when the whole entry lies inside length.
 */
static bool
entry_offset(size_t length, uint32_t header, uint32_t entry_bytes,
    uint32_t index, size_t *offset)
{
	uint64_t start;

	start = (uint64_t)header + (uint64_t)index * entry_bytes;
	if (start + entry_bytes > length)
		return (false);
	*offset = (size_t)start;
	return (true);
}

/* Fletcher-64 */

void
apfs_fletcher_init(apfs_fletcher_t *f)
{
	f->s1 = 0;
	f->s2 = 0;
}

/*
 * Both sums stay below the modulus, so adding a 32-bit word to one cannot
 * overflow 64 bits. Bytes past the last whole word of a piece belong to no
 * word; callers feed pieces whose length is a multiple of 4.
 */
void
apfs_fletcher_update(apfs_fletcher_t *f, const unsigned char *bytes,
    size_t length)
{
	for (size_t i = 0; length - i >= 4; i += 4) {
		f->s1 = (f->s1 + fsimg_load_le32(bytes + i)) % FLETCHER_MODULUS;
		f->s2 = (f->s2 + f->s1) % FLETCHER_MODULUS;
	}
}

/*
 * The stored form appends two check words chosen so that running the sums
 * over the data and then the check words ends at zero.
 */
uint64_t
apfs_fletcher_value(const apfs_fletcher_t *f)
{
	uint64_t c1, c2;

	c1 = FLETCHER_MODULUS - (f->s1 + f->s2) % FLETCHER_MODULUS;
	c2 = FLETCHER_MODULUS - (f->s1 + c1) % FLETCHER_MODULUS;
	return (c1 | c2 << 32);
}

uint64_t
apfs_object_checksum(const unsigned char *object, size_t length)
{
	apfs_fletcher_t f;

	apfs_fletcher_init(&f);
	if (length > APFS_OBJ_CHECKSUM_BYTES)
		apfs_fletcher_update(&f, object + APFS_OBJ_CHECKSUM_BYTES,
		    length - APFS_OBJ_CHECKSUM_BYTES);
	return (apfs_fletcher_value(&f));
}

bool
apfs_object_verify(const unsigned char *object, size_t length)
{
	if (length < APFS_OBJ_HEADER_BYTES || length % 4 != 0)
		return (false);
	return (apfs_object_checksum(object, length) ==
	    fsimg_load_le64(object));
}

bool
apfs_object_seal(unsigned char *object, size_t length)
{
	if (length < APFS_OBJ_HEADER_BYTES || length % 4 != 0)
		return (false);
	fsimg_store_le64(object, apfs_object_checksum(object, length));
	return (true);
}

/* Object header */

bool
apfs_obj_decode(const unsigned char *raw, size_t length, apfs_obj_t *obj)
{
	uint32_t type;

	if (length < APFS_OBJ_HEADER_BYTES)
		return (false);

	type = fsimg_load_le32(raw + 24);
	obj->checksum = fsimg_load_le64(raw);
	obj->oid = fsimg_load_le64(raw + 8);
	obj->xid = fsimg_load_le64(raw + 16);
	obj->flags = type & APFS_OBJ_FLAGS_MASK;
	obj->kind = (uint16_t)(type & APFS_OBJ_KIND_MASK);
	obj->subtype = fsimg_load_le32(raw + 28);
	return (true);
}

bool
apfs_obj_encode(unsigned char *raw, size_t length, const apfs_obj_t *obj)
{
	if (length < APFS_OBJ_HEADER_BYTES ||
	    (obj->flags & ~APFS_OBJ_FLAGS_MASK) != 0)
		return (false);

	fsimg_store_le64(raw, 0);
	fsimg_store_le64(raw + 8, obj->oid);
	fsimg_store_le64(raw + 16, obj->xid);
	fsimg_store_le32(raw + 24, obj->flags | obj->kind);
	fsimg_store_le32(raw + 28, obj->subtype);
	return (true);
}

/* Container superblock */

static void
range_decode(const unsigned char *raw, apfs_range_t *range)
{
	range->start = fsimg_load_le64(raw);
	range->count = fsimg_load_le64(raw + 8);
}

static void
range_encode(unsigned char *raw, const apfs_range_t *range)
{
	fsimg_store_le64(raw, range->start);
	fsimg_store_le64(raw + 8, range->count);
}

bool
apfs_nx_decode(const unsigned char *raw, size_t length, apfs_nx_t *nx)
{
	uint32_t desc_blocks, data_blocks;

	if (length < APFS_NX_BYTES)
		return (false);

	nx->magic = fsimg_load_le32(raw + 32);
	nx->block_size = fsimg_load_le32(raw + 36);
	nx->block_count = fsimg_load_le64(raw + 40);
	nx->features = fsimg_load_le64(raw + 48);
	nx->ro_compat_features = fsimg_load_le64(raw + 56);
	nx->incompat_features = fsimg_load_le64(raw + 64);
	bcopy(raw + 72, nx->uuid, APFS_UUID_BYTES);
	nx->next_oid = fsimg_load_le64(raw + 88);
	nx->next_xid = fsimg_load_le64(raw + 96);

	desc_blocks = fsimg_load_le32(raw + 104);
	data_blocks = fsimg_load_le32(raw + 108);
	nx->desc_blocks = desc_blocks & APFS_NX_AREA_COUNT_MASK;
	nx->desc_noncontiguous =
	    (desc_blocks & APFS_NX_AREA_NONCONTIGUOUS) != 0;
	nx->data_blocks = data_blocks & APFS_NX_AREA_COUNT_MASK;
	nx->data_noncontiguous =
	    (data_blocks & APFS_NX_AREA_NONCONTIGUOUS) != 0;
	nx->desc_base = fsimg_load_le64(raw + 112);
	nx->data_base = fsimg_load_le64(raw + 120);
	nx->desc_next = fsimg_load_le32(raw + 128);
	nx->data_next = fsimg_load_le32(raw + 132);
	nx->desc_index = fsimg_load_le32(raw + 136);
	nx->desc_len = fsimg_load_le32(raw + 140);
	nx->data_index = fsimg_load_le32(raw + 144);
	nx->data_len = fsimg_load_le32(raw + 148);

	nx->spaceman_oid = fsimg_load_le64(raw + 152);
	nx->omap_oid = fsimg_load_le64(raw + 160);
	nx->reaper_oid = fsimg_load_le64(raw + 168);
	nx->test_type = fsimg_load_le32(raw + 176);
	nx->max_volumes = fsimg_load_le32(raw + 180);
	for (unsigned i = 0; i < APFS_NX_VOLUMES_MAX; i++)
		nx->volume_oids[i] = fsimg_load_le64(raw + 184 + 8 * i);
	for (unsigned i = 0; i < APFS_NX_COUNTERS; i++)
		nx->counters[i] = fsimg_load_le64(raw + 984 + 8 * i);

	range_decode(raw + 1240, &nx->blocked_out);
	nx->evict_mapping_oid = fsimg_load_le64(raw + 1256);
	nx->flags = fsimg_load_le64(raw + 1264);
	nx->efi_jumpstart = fsimg_load_le64(raw + 1272);
	bcopy(raw + 1280, nx->fusion_uuid, APFS_UUID_BYTES);
	range_decode(raw + 1296, &nx->keylocker);
	for (unsigned i = 0; i < APFS_NX_EPHEMERAL_INFO; i++)
		nx->ephemeral_info[i] = fsimg_load_le64(raw + 1312 + 8 * i);
	nx->test_oid = fsimg_load_le64(raw + 1344);
	nx->fusion_mt_oid = fsimg_load_le64(raw + 1352);
	nx->fusion_wbc_oid = fsimg_load_le64(raw + 1360);
	range_decode(raw + 1368, &nx->fusion_wbc);
	nx->newest_mounted_version = fsimg_load_le64(raw + 1384);
	range_decode(raw + 1392, &nx->media_keylocker);
	return (true);
}

bool
apfs_nx_encode(unsigned char *raw, size_t length, const apfs_nx_t *nx)
{
	uint32_t desc_blocks, data_blocks;

	if (length < APFS_NX_BYTES ||
	    nx->desc_blocks > APFS_NX_AREA_COUNT_MASK ||
	    nx->data_blocks > APFS_NX_AREA_COUNT_MASK)
		return (false);

	fsimg_store_le32(raw + 32, nx->magic);
	fsimg_store_le32(raw + 36, nx->block_size);
	fsimg_store_le64(raw + 40, nx->block_count);
	fsimg_store_le64(raw + 48, nx->features);
	fsimg_store_le64(raw + 56, nx->ro_compat_features);
	fsimg_store_le64(raw + 64, nx->incompat_features);
	bcopy(nx->uuid, raw + 72, APFS_UUID_BYTES);
	fsimg_store_le64(raw + 88, nx->next_oid);
	fsimg_store_le64(raw + 96, nx->next_xid);

	desc_blocks = nx->desc_blocks;
	if (nx->desc_noncontiguous)
		desc_blocks |= APFS_NX_AREA_NONCONTIGUOUS;
	data_blocks = nx->data_blocks;
	if (nx->data_noncontiguous)
		data_blocks |= APFS_NX_AREA_NONCONTIGUOUS;
	fsimg_store_le32(raw + 104, desc_blocks);
	fsimg_store_le32(raw + 108, data_blocks);
	fsimg_store_le64(raw + 112, nx->desc_base);
	fsimg_store_le64(raw + 120, nx->data_base);
	fsimg_store_le32(raw + 128, nx->desc_next);
	fsimg_store_le32(raw + 132, nx->data_next);
	fsimg_store_le32(raw + 136, nx->desc_index);
	fsimg_store_le32(raw + 140, nx->desc_len);
	fsimg_store_le32(raw + 144, nx->data_index);
	fsimg_store_le32(raw + 148, nx->data_len);

	fsimg_store_le64(raw + 152, nx->spaceman_oid);
	fsimg_store_le64(raw + 160, nx->omap_oid);
	fsimg_store_le64(raw + 168, nx->reaper_oid);
	fsimg_store_le32(raw + 176, nx->test_type);
	fsimg_store_le32(raw + 180, nx->max_volumes);
	for (unsigned i = 0; i < APFS_NX_VOLUMES_MAX; i++)
		fsimg_store_le64(raw + 184 + 8 * i, nx->volume_oids[i]);
	for (unsigned i = 0; i < APFS_NX_COUNTERS; i++)
		fsimg_store_le64(raw + 984 + 8 * i, nx->counters[i]);

	range_encode(raw + 1240, &nx->blocked_out);
	fsimg_store_le64(raw + 1256, nx->evict_mapping_oid);
	fsimg_store_le64(raw + 1264, nx->flags);
	fsimg_store_le64(raw + 1272, nx->efi_jumpstart);
	bcopy(nx->fusion_uuid, raw + 1280, APFS_UUID_BYTES);
	range_encode(raw + 1296, &nx->keylocker);
	for (unsigned i = 0; i < APFS_NX_EPHEMERAL_INFO; i++)
		fsimg_store_le64(raw + 1312 + 8 * i, nx->ephemeral_info[i]);
	fsimg_store_le64(raw + 1344, nx->test_oid);
	fsimg_store_le64(raw + 1352, nx->fusion_mt_oid);
	fsimg_store_le64(raw + 1360, nx->fusion_wbc_oid);
	range_encode(raw + 1368, &nx->fusion_wbc);
	fsimg_store_le64(raw + 1384, nx->newest_mounted_version);
	range_encode(raw + 1392, &nx->media_keylocker);
	return (true);
}

/* Checkpoint-mapping blocks */

bool
apfs_cpm_block_decode(const unsigned char *raw, size_t length,
    apfs_cpm_block_t *block)
{
	uint32_t count;

	if (length < APFS_CPM_HEADER_BYTES)
		return (false);
	count = fsimg_load_le32(raw + 36);
	if (!entries_fit(length, APFS_CPM_HEADER_BYTES, APFS_CPM_ENTRY_BYTES,
	    count))
		return (false);

	block->flags = fsimg_load_le32(raw + 32);
	block->count = count;
	return (true);
}

bool
apfs_cpm_block_encode(unsigned char *raw, size_t length,
    const apfs_cpm_block_t *block)
{
	if (length < APFS_CPM_HEADER_BYTES ||
	    !entries_fit(length, APFS_CPM_HEADER_BYTES, APFS_CPM_ENTRY_BYTES,
	    block->count))
		return (false);

	fsimg_store_le32(raw + 32, block->flags);
	fsimg_store_le32(raw + 36, block->count);
	return (true);
}

bool
apfs_cpm_decode(const unsigned char *raw, size_t length,
    const apfs_cpm_block_t *block, uint32_t index, apfs_cpm_t *cpm)
{
	const unsigned char *p;
	size_t offset;
	uint32_t type;

	if (index >= block->count || !entry_offset(length, APFS_CPM_HEADER_BYTES, APFS_CPM_ENTRY_BYTES,
	    index, &offset))
		return (false);

	p = raw + offset;
	type = fsimg_load_le32(p);
	cpm->flags = type & APFS_OBJ_FLAGS_MASK;
	cpm->kind = (uint16_t)(type & APFS_OBJ_KIND_MASK);
	cpm->subtype = fsimg_load_le32(p + 4);
	cpm->size = fsimg_load_le32(p + 8);
	cpm->volume_oid = fsimg_load_le64(p + 16);
	cpm->oid = fsimg_load_le64(p + 24);
	cpm->address = fsimg_load_le64(p + 32);
	return (true);
}

bool
apfs_cpm_encode(unsigned char *raw, size_t length,
    const apfs_cpm_block_t *block, uint32_t index, const apfs_cpm_t *cpm)
{
	unsigned char *p;
	size_t offset;

	if (index >= block->count || (cpm->flags & ~APFS_OBJ_FLAGS_MASK) != 0 ||
	    !entry_offset(length, APFS_CPM_HEADER_BYTES, APFS_CPM_ENTRY_BYTES,
	    index, &offset))
		return (false);

	p = raw + offset;
	fsimg_store_le32(p, cpm->flags | cpm->kind);
	fsimg_store_le32(p + 4, cpm->subtype);
	fsimg_store_le32(p + 8, cpm->size);
	fsimg_store_le32(p + 12, 0);
	fsimg_store_le64(p + 16, cpm->volume_oid);
	fsimg_store_le64(p + 24, cpm->oid);
	fsimg_store_le64(p + 32, cpm->address);
	return (true);
}

/* Object maps */

bool
apfs_omap_decode(const unsigned char *raw, size_t length, apfs_omap_t *omap)
{
	if (length < APFS_OMAP_BYTES)
		return (false);

	omap->flags = fsimg_load_le32(raw + 32);
	omap->snapshot_count = fsimg_load_le32(raw + 36);
	omap->tree_type = fsimg_load_le32(raw + 40);
	omap->snapshot_tree_type = fsimg_load_le32(raw + 44);
	omap->tree_oid = fsimg_load_le64(raw + 48);
	omap->snapshot_tree_oid = fsimg_load_le64(raw + 56);
	omap->most_recent_snapshot = fsimg_load_le64(raw + 64);
	omap->pending_revert_min = fsimg_load_le64(raw + 72);
	omap->pending_revert_max = fsimg_load_le64(raw + 80);
	return (true);
}

bool
apfs_omap_encode(unsigned char *raw, size_t length, const apfs_omap_t *omap)
{
	if (length < APFS_OMAP_BYTES)
		return (false);

	fsimg_store_le32(raw + 32, omap->flags);
	fsimg_store_le32(raw + 36, omap->snapshot_count);
	fsimg_store_le32(raw + 40, omap->tree_type);
	fsimg_store_le32(raw + 44, omap->snapshot_tree_type);
	fsimg_store_le64(raw + 48, omap->tree_oid);
	fsimg_store_le64(raw + 56, omap->snapshot_tree_oid);
	fsimg_store_le64(raw + 64, omap->most_recent_snapshot);
	fsimg_store_le64(raw + 72, omap->pending_revert_min);
	fsimg_store_le64(raw + 80, omap->pending_revert_max);
	return (true);
}

bool
apfs_omap_key_decode(const unsigned char *raw, size_t length,
    apfs_omap_key_t *key)
{
	if (length != APFS_OMAP_KEY_BYTES)
		return (false);

	key->oid = fsimg_load_le64(raw);
	key->xid = fsimg_load_le64(raw + 8);
	return (true);
}

bool
apfs_omap_key_encode(unsigned char *raw, size_t length,
    const apfs_omap_key_t *key)
{
	if (length != APFS_OMAP_KEY_BYTES)
		return (false);

	fsimg_store_le64(raw, key->oid);
	fsimg_store_le64(raw + 8, key->xid);
	return (true);
}

bool
apfs_omap_val_decode(const unsigned char *raw, size_t length,
    apfs_omap_val_t *val)
{
	if (length != APFS_OMAP_VAL_BYTES)
		return (false);

	val->flags = fsimg_load_le32(raw);
	val->size = fsimg_load_le32(raw + 4);
	val->address = fsimg_load_le64(raw + 8);
	return (true);
}

bool
apfs_omap_val_encode(unsigned char *raw, size_t length,
    const apfs_omap_val_t *val)
{
	if (length != APFS_OMAP_VAL_BYTES)
		return (false);

	fsimg_store_le32(raw, val->flags);
	fsimg_store_le32(raw + 4, val->size);
	fsimg_store_le64(raw + 8, val->address);
	return (true);
}

/* B-tree nodes */

static void
nloc_decode(const unsigned char *raw, apfs_nloc_t *nloc)
{
	nloc->off = fsimg_load_le16(raw);
	nloc->len = fsimg_load_le16(raw + 2);
}

static void
nloc_encode(unsigned char *raw, const apfs_nloc_t *nloc)
{
	fsimg_store_le16(raw, nloc->off);
	fsimg_store_le16(raw + 2, nloc->len);
}

/*
 * Derive a node's areas from its stored header fields and prove that they
 * nest in order inside a node of length bytes:
 *
 *	56  toc_start  key_start  free_start  free_end  value_end  [footer]
 *
 * and that the table of contents holds an entry for every key. A node is
 * one block, so length is at most APFS_BLOCK_MAX and every boundary fits
 * 32 bits.
 */
static bool
node_layout(size_t length, const apfs_node_t *node, apfs_node_area_t *area)
{
	uint32_t entry_bytes, footer, free_end, free_start, key_start;
	uint32_t toc_start, value_end;

	footer = (node->flags & APFS_NODE_ROOT) != 0 ?
	    APFS_BTREE_INFO_BYTES : 0;
	if (length > APFS_BLOCK_MAX ||
	    length < (size_t)APFS_NODE_HEADER_BYTES + footer)
		return (false);
	value_end = (uint32_t)length - footer;

	toc_start = APFS_NODE_HEADER_BYTES + node->toc.off;
	key_start = toc_start + node->toc.len;
	if (key_start > value_end)
		return (false);
	entry_bytes = (node->flags & APFS_NODE_FIXED_KV) != 0 ?
	    APFS_TOC_FIXED_BYTES : APFS_TOC_VARIABLE_BYTES;
	if ((uint64_t)node->key_count * entry_bytes > node->toc.len)
		return (false);

	free_start = key_start + node->free.off;
	free_end = free_start + node->free.len;
	if (free_end > value_end)
		return (false);

	area->toc_start = toc_start;
	area->key_start = key_start;
	area->free_start = free_start;
	area->free_end = free_end;
	area->value_end = value_end;
	return (true);
}

bool
apfs_node_decode(const unsigned char *raw, size_t length, apfs_node_t *node)
{
	apfs_node_t n;

	if (length < APFS_NODE_HEADER_BYTES)
		return (false);

	n.flags = fsimg_load_le16(raw + 32);
	n.level = fsimg_load_le16(raw + 34);
	n.key_count = fsimg_load_le32(raw + 36);
	nloc_decode(raw + 40, &n.toc);
	nloc_decode(raw + 44, &n.free);
	nloc_decode(raw + 48, &n.key_free);
	nloc_decode(raw + 52, &n.val_free);
	if (!node_layout(length, &n, &n.area))
		return (false);

	if ((n.flags & APFS_NODE_ROOT) != 0)
		btree_info_load(raw + n.area.value_end, &n.info);
	else
		bzero(&n.info, sizeof(n.info));
	*node = n;
	return (true);
}

bool
apfs_node_encode(unsigned char *raw, size_t length, const apfs_node_t *node)
{
	apfs_node_area_t area;

	if (!node_layout(length, node, &area))
		return (false);

	fsimg_store_le16(raw + 32, node->flags);
	fsimg_store_le16(raw + 34, node->level);
	fsimg_store_le32(raw + 36, node->key_count);
	nloc_encode(raw + 40, &node->toc);
	nloc_encode(raw + 44, &node->free);
	nloc_encode(raw + 48, &node->key_free);
	nloc_encode(raw + 52, &node->val_free);
	if ((node->flags & APFS_NODE_ROOT) != 0)
		btree_info_store(raw + area.value_end, &node->info);
	return (true);
}

/*
 * The offset of table-of-contents entry index and the area it indexes, when
 * the node's stored fields describe areas that fit length and index is below
 * the key count, which the layout proved the table of contents holds.
 */
static bool
toc_slot(size_t length, const apfs_node_t *node, uint32_t index,
    apfs_node_area_t *area, size_t *offset)
{
	size_t entry_bytes;

	if (index >= node->key_count || !node_layout(length, node, area))
		return (false);
	entry_bytes = (node->flags & APFS_NODE_FIXED_KV) != 0 ?
	    APFS_TOC_FIXED_BYTES : APFS_TOC_VARIABLE_BYTES;
	*offset = area->toc_start + (size_t)index * entry_bytes;
	return (true);
}

/*
 * Resolve a table-of-contents entry to spans of its node. A key offset
 * counts from the start of the key area and the key must end before the
 * free space. A value offset counts back from the end of the value area, so
 * the value occupies [value_end - off, value_end - off + len), which must
 * start at or after the free space's end. A fixed-size entry takes its
 * lengths from sizes; a ghost has no value span.
 */
static bool
entry_resolve(const apfs_node_area_t *area, bool fixed,
    const apfs_toc_t *toc, const apfs_kv_size_t *sizes, apfs_entry_t *entry)
{
	uint32_t key_len, val_len;

	if (fixed && sizes == NULL)
		return (false);
	key_len = fixed ? sizes->key : toc->key.len;
	val_len = fixed ? sizes->val : toc->val.len;

	if ((uint64_t)area->key_start + toc->key.off + key_len >
	    area->free_start)
		return (false);
	if (toc->val.off != APFS_TOC_GHOST &&
	    (val_len > toc->val.off ||
	    toc->val.off > area->value_end - area->free_end))
		return (false);

	entry->key_off = area->key_start + toc->key.off;
	entry->key_len = key_len;
	entry->ghost = toc->val.off == APFS_TOC_GHOST;
	if (entry->ghost) {
		entry->val_off = 0;
		entry->val_len = 0;
	} else {
		entry->val_off = area->value_end - toc->val.off;
		entry->val_len = val_len;
	}
	return (true);
}

bool
apfs_entry_decode(const unsigned char *raw, size_t length,
    const apfs_node_t *node, uint32_t index, const apfs_kv_size_t *sizes,
    apfs_entry_t *entry)
{
	apfs_node_area_t area;
	apfs_toc_t toc;
	size_t offset;
	bool fixed;

	if (!toc_slot(length, node, index, &area, &offset))
		return (false);

	fixed = (node->flags & APFS_NODE_FIXED_KV) != 0;
	if (fixed) {
		toc.key.off = fsimg_load_le16(raw + offset);
		toc.key.len = 0;
		toc.val.off = fsimg_load_le16(raw + offset + 2);
		toc.val.len = 0;
	} else {
		nloc_decode(raw + offset, &toc.key);
		nloc_decode(raw + offset + 4, &toc.val);
	}
	return (entry_resolve(&area, fixed, &toc, sizes, entry));
}

bool
apfs_entry_encode(unsigned char *raw, size_t length, const apfs_node_t *node,
    uint32_t index, const apfs_toc_t *toc, const apfs_kv_size_t *sizes,
    apfs_entry_t *entry)
{
	apfs_node_area_t area;
	apfs_entry_t resolved;
	size_t offset;
	bool fixed;

	fixed = (node->flags & APFS_NODE_FIXED_KV) != 0;
	if (!toc_slot(length, node, index, &area, &offset) ||
	    !entry_resolve(&area, fixed, toc, sizes, &resolved))
		return (false);

	if (fixed) {
		fsimg_store_le16(raw + offset, toc->key.off);
		fsimg_store_le16(raw + offset + 2, toc->val.off);
	} else {
		nloc_encode(raw + offset, &toc->key);
		nloc_encode(raw + offset + 4, &toc->val);
	}
	*entry = resolved;
	return (true);
}

static void
btree_info_load(const unsigned char *raw, apfs_btree_info_t *info)
{
	info->flags = fsimg_load_le32(raw);
	info->node_size = fsimg_load_le32(raw + 4);
	info->key_size = fsimg_load_le32(raw + 8);
	info->val_size = fsimg_load_le32(raw + 12);
	info->longest_key = fsimg_load_le32(raw + 16);
	info->longest_val = fsimg_load_le32(raw + 20);
	info->key_count = fsimg_load_le64(raw + 24);
	info->node_count = fsimg_load_le64(raw + 32);
}

static void
btree_info_store(unsigned char *raw, const apfs_btree_info_t *info)
{
	fsimg_store_le32(raw, info->flags);
	fsimg_store_le32(raw + 4, info->node_size);
	fsimg_store_le32(raw + 8, info->key_size);
	fsimg_store_le32(raw + 12, info->val_size);
	fsimg_store_le32(raw + 16, info->longest_key);
	fsimg_store_le32(raw + 20, info->longest_val);
	fsimg_store_le64(raw + 24, info->key_count);
	fsimg_store_le64(raw + 32, info->node_count);
}

bool
apfs_btree_info_decode(const unsigned char *raw, size_t length,
    apfs_btree_info_t *info)
{
	if (length != APFS_BTREE_INFO_BYTES)
		return (false);
	btree_info_load(raw, info);
	return (true);
}

bool
apfs_btree_info_encode(unsigned char *raw, size_t length,
    const apfs_btree_info_t *info)
{
	if (length != APFS_BTREE_INFO_BYTES)
		return (false);
	btree_info_store(raw, info);
	return (true);
}

/* Space manager */

/* A 48-byte device entry; its last 12 bytes are reserved. */
static void
sm_device_decode(const unsigned char *raw, apfs_sm_device_t *device)
{
	device->block_count = fsimg_load_le64(raw);
	device->chunk_count = fsimg_load_le64(raw + 8);
	device->cib_count = fsimg_load_le32(raw + 16);
	device->cab_count = fsimg_load_le32(raw + 20);
	device->free_count = fsimg_load_le64(raw + 24);
	device->addr_offset = fsimg_load_le32(raw + 32);
}

static void
sm_device_encode(unsigned char *raw, const apfs_sm_device_t *device)
{
	fsimg_store_le64(raw, device->block_count);
	fsimg_store_le64(raw + 8, device->chunk_count);
	fsimg_store_le32(raw + 16, device->cib_count);
	fsimg_store_le32(raw + 20, device->cab_count);
	fsimg_store_le64(raw + 24, device->free_count);
	fsimg_store_le32(raw + 32, device->addr_offset);
	fsimg_store_le32(raw + 36, 0);
	fsimg_store_le64(raw + 40, 0);
}

/* A 40-byte free-queue entry; its last 14 bytes are reserved. */
static void
sm_fq_decode(const unsigned char *raw, apfs_sm_fq_t *fq)
{
	fq->count = fsimg_load_le64(raw);
	fq->tree_oid = fsimg_load_le64(raw + 8);
	fq->oldest_xid = fsimg_load_le64(raw + 16);
	fq->node_limit = fsimg_load_le16(raw + 24);
}

static void
sm_fq_encode(unsigned char *raw, const apfs_sm_fq_t *fq)
{
	fsimg_store_le64(raw, fq->count);
	fsimg_store_le64(raw + 8, fq->tree_oid);
	fsimg_store_le64(raw + 16, fq->oldest_xid);
	fsimg_store_le16(raw + 24, fq->node_limit);
	fsimg_store_le16(raw + 26, 0);
	fsimg_store_le32(raw + 28, 0);
	fsimg_store_le64(raw + 32, 0);
}

bool
apfs_spaceman_decode(const unsigned char *raw, size_t length,
    apfs_spaceman_t *sm)
{
	if (length < APFS_SM_BYTES)
		return (false);

	sm->block_size = fsimg_load_le32(raw + 32);
	sm->blocks_per_chunk = fsimg_load_le32(raw + 36);
	sm->chunks_per_cib = fsimg_load_le32(raw + 40);
	sm->cibs_per_cab = fsimg_load_le32(raw + 44);
	for (unsigned i = 0; i < APFS_SM_DEVICES; i++)
		sm_device_decode(raw + SM_DEVICES_OFFSET + SM_DEVICE_BYTES * i,
		    &sm->devices[i]);
	sm->flags = fsimg_load_le32(raw + 144);

	sm->ip_bm_tx_multiplier = fsimg_load_le32(raw + 148);
	sm->ip_block_count = fsimg_load_le64(raw + 152);
	sm->ip_bm_size_in_blocks = fsimg_load_le32(raw + 160);
	sm->ip_bm_block_count = fsimg_load_le32(raw + 164);
	sm->ip_bm_base = fsimg_load_le64(raw + 168);
	sm->ip_base = fsimg_load_le64(raw + 176);
	sm->fs_reserve_block_count = fsimg_load_le64(raw + 184);
	sm->fs_reserve_alloc_count = fsimg_load_le64(raw + 192);
	for (unsigned i = 0; i < APFS_SM_FREE_QUEUES; i++)
		sm_fq_decode(raw + SM_FQS_OFFSET + SM_FQ_BYTES * i,
		    &sm->free_queues[i]);

	sm->ip_bm_free_head = fsimg_load_le16(raw + 320);
	sm->ip_bm_free_tail = fsimg_load_le16(raw + 322);
	sm->ip_bm_xid_offset = fsimg_load_le32(raw + 324);
	sm->ip_bitmap_offset = fsimg_load_le32(raw + 328);
	sm->ip_bm_free_next_offset = fsimg_load_le32(raw + 332);
	sm->version = fsimg_load_le32(raw + 336);
	sm->struct_size = fsimg_load_le32(raw + 340);
	return (true);
}

bool
apfs_spaceman_encode(unsigned char *raw, size_t length,
    const apfs_spaceman_t *sm)
{
	if (length < APFS_SM_BYTES)
		return (false);

	fsimg_store_le32(raw + 32, sm->block_size);
	fsimg_store_le32(raw + 36, sm->blocks_per_chunk);
	fsimg_store_le32(raw + 40, sm->chunks_per_cib);
	fsimg_store_le32(raw + 44, sm->cibs_per_cab);
	for (unsigned i = 0; i < APFS_SM_DEVICES; i++)
		sm_device_encode(raw + SM_DEVICES_OFFSET + SM_DEVICE_BYTES * i,
		    &sm->devices[i]);
	fsimg_store_le32(raw + 144, sm->flags);

	fsimg_store_le32(raw + 148, sm->ip_bm_tx_multiplier);
	fsimg_store_le64(raw + 152, sm->ip_block_count);
	fsimg_store_le32(raw + 160, sm->ip_bm_size_in_blocks);
	fsimg_store_le32(raw + 164, sm->ip_bm_block_count);
	fsimg_store_le64(raw + 168, sm->ip_bm_base);
	fsimg_store_le64(raw + 176, sm->ip_base);
	fsimg_store_le64(raw + 184, sm->fs_reserve_block_count);
	fsimg_store_le64(raw + 192, sm->fs_reserve_alloc_count);
	for (unsigned i = 0; i < APFS_SM_FREE_QUEUES; i++)
		sm_fq_encode(raw + SM_FQS_OFFSET + SM_FQ_BYTES * i,
		    &sm->free_queues[i]);

	fsimg_store_le16(raw + 320, sm->ip_bm_free_head);
	fsimg_store_le16(raw + 322, sm->ip_bm_free_tail);
	fsimg_store_le32(raw + 324, sm->ip_bm_xid_offset);
	fsimg_store_le32(raw + 328, sm->ip_bitmap_offset);
	fsimg_store_le32(raw + 332, sm->ip_bm_free_next_offset);
	fsimg_store_le32(raw + 336, sm->version);
	fsimg_store_le32(raw + 340, sm->struct_size);
	bzero(raw + APFS_SM_DATAZONE_OFFSET,
	    APFS_SM_BYTES - APFS_SM_DATAZONE_OFFSET);
	return (true);
}

/*
 * Chunk-information blocks and their address blocks share a header, an
 * index and a count at offsets 32 and 36, and differ in their records.
 */

static bool
cib_header_decode(const unsigned char *raw, size_t length,
    uint32_t entry_bytes, apfs_cib_t *cib)
{
	uint32_t count;

	if (length < APFS_CIB_HEADER_BYTES)
		return (false);
	count = fsimg_load_le32(raw + 36);
	if (!entries_fit(length, APFS_CIB_HEADER_BYTES, entry_bytes, count))
		return (false);

	cib->index = fsimg_load_le32(raw + 32);
	cib->count = count;
	return (true);
}

static bool
cib_header_encode(unsigned char *raw, size_t length, uint32_t entry_bytes,
    const apfs_cib_t *cib)
{
	if (length < APFS_CIB_HEADER_BYTES ||
	    !entries_fit(length, APFS_CIB_HEADER_BYTES, entry_bytes,
	    cib->count))
		return (false);

	fsimg_store_le32(raw + 32, cib->index);
	fsimg_store_le32(raw + 36, cib->count);
	return (true);
}

bool
apfs_cib_decode(const unsigned char *raw, size_t length, apfs_cib_t *cib)
{
	return (cib_header_decode(raw, length, APFS_CHUNK_INFO_BYTES, cib));
}

bool
apfs_cib_encode(unsigned char *raw, size_t length, const apfs_cib_t *cib)
{
	return (cib_header_encode(raw, length, APFS_CHUNK_INFO_BYTES, cib));
}

bool
apfs_cab_decode(const unsigned char *raw, size_t length, apfs_cib_t *cab)
{
	return (cib_header_decode(raw, length, APFS_CAB_ENTRY_BYTES, cab));
}

bool
apfs_cab_encode(unsigned char *raw, size_t length, const apfs_cib_t *cab)
{
	return (cib_header_encode(raw, length, APFS_CAB_ENTRY_BYTES, cab));
}

bool
apfs_chunk_info_decode(const unsigned char *raw, size_t length,
    const apfs_cib_t *cib, uint32_t index, apfs_chunk_info_t *chunk)
{
	const unsigned char *p;
	size_t offset;

	if (index >= cib->count || !entry_offset(length, APFS_CIB_HEADER_BYTES, APFS_CHUNK_INFO_BYTES,
	    index, &offset))
		return (false);

	p = raw + offset;
	chunk->xid = fsimg_load_le64(p);
	chunk->address = fsimg_load_le64(p + 8);
	chunk->block_count = fsimg_load_le32(p + 16);
	chunk->free_count = fsimg_load_le32(p + 20);
	chunk->bitmap_address = fsimg_load_le64(p + 24);
	return (true);
}

bool
apfs_chunk_info_encode(unsigned char *raw, size_t length,
    const apfs_cib_t *cib, uint32_t index, const apfs_chunk_info_t *chunk)
{
	unsigned char *p;
	size_t offset;

	if (index >= cib->count || !entry_offset(length, APFS_CIB_HEADER_BYTES, APFS_CHUNK_INFO_BYTES,
	    index, &offset))
		return (false);

	p = raw + offset;
	fsimg_store_le64(p, chunk->xid);
	fsimg_store_le64(p + 8, chunk->address);
	fsimg_store_le32(p + 16, chunk->block_count);
	fsimg_store_le32(p + 20, chunk->free_count);
	fsimg_store_le64(p + 24, chunk->bitmap_address);
	return (true);
}

bool
apfs_cab_address_decode(const unsigned char *raw, size_t length,
    const apfs_cib_t *cab, uint32_t index, uint64_t *address)
{
	size_t offset;

	if (index >= cab->count || !entry_offset(length, APFS_CAB_HEADER_BYTES, APFS_CAB_ENTRY_BYTES,
	    index, &offset))
		return (false);
	*address = fsimg_load_le64(raw + offset);
	return (true);
}

bool
apfs_cab_address_encode(unsigned char *raw, size_t length,
    const apfs_cib_t *cab, uint32_t index, uint64_t address)
{
	size_t offset;

	if (index >= cab->count || !entry_offset(length, APFS_CAB_HEADER_BYTES, APFS_CAB_ENTRY_BYTES,
	    index, &offset))
		return (false);
	fsimg_store_le64(raw + offset, address);
	return (true);
}

/* Free-queue records */

bool
apfs_fq_key_decode(const unsigned char *raw, size_t length,
    apfs_fq_key_t *key)
{
	if (length != APFS_FQ_KEY_BYTES)
		return (false);

	key->xid = fsimg_load_le64(raw);
	key->address = fsimg_load_le64(raw + 8);
	return (true);
}

bool
apfs_fq_key_encode(unsigned char *raw, size_t length, const apfs_fq_key_t *key)
{
	if (length != APFS_FQ_KEY_BYTES)
		return (false);

	fsimg_store_le64(raw, key->xid);
	fsimg_store_le64(raw + 8, key->address);
	return (true);
}

bool
apfs_fq_val_decode(const unsigned char *raw, size_t length, uint64_t *count)
{
	if (length != APFS_FQ_VAL_BYTES)
		return (false);
	*count = fsimg_load_le64(raw);
	return (true);
}

bool
apfs_fq_val_encode(unsigned char *raw, size_t length, uint64_t count)
{
	if (length != APFS_FQ_VAL_BYTES)
		return (false);
	fsimg_store_le64(raw, count);
	return (true);
}

/* Reaper */

bool
apfs_reaper_decode(const unsigned char *raw, size_t length,
    apfs_reaper_t *reaper)
{
	uint32_t state_size;

	if (length < APFS_REAPER_BYTES)
		return (false);
	state_size = fsimg_load_le32(raw + 108);
	if (state_size > length - APFS_REAPER_BYTES)
		return (false);

	reaper->next_reap_id = fsimg_load_le64(raw + 32);
	reaper->completed_id = fsimg_load_le64(raw + 40);
	reaper->head = fsimg_load_le64(raw + 48);
	reaper->tail = fsimg_load_le64(raw + 56);
	reaper->flags = fsimg_load_le32(raw + 64);
	reaper->rlcount = fsimg_load_le32(raw + 68);
	reaper->reaping_type = fsimg_load_le32(raw + 72);
	reaper->reaping_size = fsimg_load_le32(raw + 76);
	reaper->reaping_volume_oid = fsimg_load_le64(raw + 80);
	reaper->reaping_oid = fsimg_load_le64(raw + 88);
	reaper->reaping_xid = fsimg_load_le64(raw + 96);
	reaper->reaping_entry_flags = fsimg_load_le32(raw + 104);
	reaper->state_size = state_size;
	return (true);
}

bool
apfs_reaper_encode(unsigned char *raw, size_t length,
    const apfs_reaper_t *reaper)
{
	if (length < APFS_REAPER_BYTES ||
	    reaper->state_size > length - APFS_REAPER_BYTES)
		return (false);

	fsimg_store_le64(raw + 32, reaper->next_reap_id);
	fsimg_store_le64(raw + 40, reaper->completed_id);
	fsimg_store_le64(raw + 48, reaper->head);
	fsimg_store_le64(raw + 56, reaper->tail);
	fsimg_store_le32(raw + 64, reaper->flags);
	fsimg_store_le32(raw + 68, reaper->rlcount);
	fsimg_store_le32(raw + 72, reaper->reaping_type);
	fsimg_store_le32(raw + 76, reaper->reaping_size);
	fsimg_store_le64(raw + 80, reaper->reaping_volume_oid);
	fsimg_store_le64(raw + 88, reaper->reaping_oid);
	fsimg_store_le64(raw + 96, reaper->reaping_xid);
	fsimg_store_le32(raw + 104, reaper->reaping_entry_flags);
	fsimg_store_le32(raw + 108, reaper->state_size);
	return (true);
}

/* Reap list */

bool
apfs_reap_list_decode(const unsigned char *raw, size_t length,
    apfs_reap_list_t *list)
{
	uint32_t capacity, count;

	if (length < APFS_REAP_LIST_BYTES)
		return (false);
	capacity = fsimg_load_le32(raw + 44);
	count = fsimg_load_le32(raw + 48);
	if (capacity > (length - APFS_REAP_LIST_BYTES) /
	    APFS_REAP_LIST_ENTRY_BYTES || count > capacity)
		return (false);

	list->next = fsimg_load_le64(raw + 32);
	list->flags = fsimg_load_le32(raw + 40);
	list->capacity = capacity;
	list->count = count;
	list->first = fsimg_load_le32(raw + 52);
	list->last = fsimg_load_le32(raw + 56);
	list->free = fsimg_load_le32(raw + 60);
	return (true);
}

bool
apfs_reap_list_encode(unsigned char *raw, size_t length,
    const apfs_reap_list_t *list)
{
	if (length < APFS_REAP_LIST_BYTES ||
	    list->capacity > (length - APFS_REAP_LIST_BYTES) /
	    APFS_REAP_LIST_ENTRY_BYTES || list->count > list->capacity)
		return (false);

	fsimg_store_le64(raw + 32, list->next);
	fsimg_store_le32(raw + 40, list->flags);
	fsimg_store_le32(raw + 44, list->capacity);
	fsimg_store_le32(raw + 48, list->count);
	fsimg_store_le32(raw + 52, list->first);
	fsimg_store_le32(raw + 56, list->last);
	fsimg_store_le32(raw + 60, list->free);
	return (true);
}

/*
 * Filesystem-tree records. The key and value decoders follow the container
 * rules above: a fixed part is proved inside the record before a field is
 * read, and a variable part, a name or an extended-field blob, is proved to
 * end the record exactly. A key decoder also proves its key's kind, which
 * selects the layout it reads. Kinds of inodes, flags policy, field types,
 * name validity, hashes and order are the checks of inode.c, directory.c
 * and content.c (specs/design-apfs-driver.md, "Codecs").
 */

static bool	jkey_load(const unsigned char *, size_t, uint32_t, size_t,
		    uint64_t *);
static void	jkey_store(unsigned char *, uint64_t, uint32_t);
static int32_t	load_le32_signed(const unsigned char *);
static int64_t	load_le64_signed(const unsigned char *);
static bool	name_fits(size_t, size_t, uint16_t, uint32_t);
static bool	name_key_decode(const unsigned char *, size_t, uint32_t,
		    size_t, apfs_name_key_t *);
static bool	name_key_encode(unsigned char *, size_t, uint32_t, size_t,
		    const apfs_name_key_t *);
static bool	name_span(const unsigned char *, size_t, size_t, uint32_t,
		    const char **, uint16_t *);
static void	name_store(unsigned char *, size_t, const char *, uint16_t);
static bool	padding_zero(const apfs_xfield_t *);
static bool	type_repeated(unsigned char *, uint8_t);
static uint32_t	xfield_padded(uint32_t);
static bool	xfield_step(const apfs_xfields_t *, uint16_t, size_t *,
		    apfs_xfield_t *);
static bool	xfields_copyable(const apfs_xfields_t *, size_t, size_t);

/*
 * Signed fields, read as two's complement without the implementation-defined
 * conversion of an unsigned value above the signed maximum.
 */
static int64_t
load_le64_signed(const unsigned char *raw)
{
	uint64_t value;

	value = fsimg_load_le64(raw);
	if (value <= INT64_MAX)
		return ((int64_t)value);
	return (-(int64_t)~value - 1);
}

static int32_t
load_le32_signed(const unsigned char *raw)
{
	uint32_t value;

	value = fsimg_load_le32(raw);
	if (value <= INT32_MAX)
		return ((int32_t)value);
	return (-(int32_t)~value - 1);
}

/*
 * Reads the OID of a key whose fixed part, the header included, is fixed
 * bytes: refuses a key shorter than that or of a kind other than kind.
 */
static bool
jkey_load(const unsigned char *raw, size_t length, uint32_t kind,
    size_t fixed, uint64_t *oid)
{
	uint64_t word;

	if (length < fixed)
		return (false);
	word = fsimg_load_le64(raw);
	if (word >> APFS_JKEY_KIND_SHIFT != kind)
		return (false);
	*oid = word & APFS_JKEY_OID_MASK;
	return (true);
}

/* The caller has proved oid within APFS_JKEY_OID_MASK. */
static void
jkey_store(unsigned char *raw, uint64_t oid, uint32_t kind)
{
	fsimg_store_le64(raw, oid | (uint64_t)kind << APFS_JKEY_KIND_SHIFT);
}

/*
 * A name stored at offset at with stored bytes, its NUL included: it must
 * end the record exactly and end in its NUL. The caller has proved at
 * within length.
 */
static bool
name_span(const unsigned char *raw, size_t length, size_t at,
    uint32_t stored, const char **name, uint16_t *name_len)
{
	if (stored == 0 || length - at != stored || raw[length - 1] != '\0')
		return (false);
	*name = (const char *)(raw + at);
	*name_len = (uint16_t)(stored - 1);
	return (true);
}

/*
 * Whether a name of name_len bytes fits a record of length bytes: stored
 * with its NUL from offset at, it must end the record, and its length with
 * the NUL must fit a length field holding at most max.
 */
static bool
name_fits(size_t length, size_t at, uint16_t name_len, uint32_t max)
{
	return ((uint32_t)name_len + 1 <= max && length >= at &&
	    length - at == (size_t)name_len + 1);
}

static void
name_store(unsigned char *raw, size_t at, const char *name,
    uint16_t name_len)
{
	if (name_len > 0)
		bcopy(name, raw + at, name_len);
	raw[at + name_len] = '\0';
}

/* Key header */

bool
apfs_jkey_decode(const unsigned char *raw, size_t length, apfs_jkey_t *key)
{
	uint64_t word;

	if (length < APFS_JKEY_BYTES)
		return (false);

	word = fsimg_load_le64(raw);
	key->oid = word & APFS_JKEY_OID_MASK;
	key->kind = (uint8_t)(word >> APFS_JKEY_KIND_SHIFT);
	return (true);
}

bool
apfs_jkey_encode(unsigned char *raw, size_t length, const apfs_jkey_t *key)
{
	if (length < APFS_JKEY_BYTES || key->oid > APFS_JKEY_OID_MASK ||
	    key->kind > APFS_JKEY_KIND_MASK)
		return (false);

	jkey_store(raw, key->oid, key->kind);
	return (true);
}

bool
apfs_bare_key_decode(const unsigned char *raw, size_t length, uint8_t kind,
    uint64_t *oid)
{
	if (length != APFS_JKEY_BYTES)
		return (false);
	return (jkey_load(raw, length, kind, APFS_JKEY_BYTES, oid));
}

bool
apfs_bare_key_encode(unsigned char *raw, size_t length, uint8_t kind,
    uint64_t oid)
{
	if (length != APFS_JKEY_BYTES || oid > APFS_JKEY_OID_MASK ||
	    kind > APFS_JKEY_KIND_MASK)
		return (false);

	jkey_store(raw, oid, kind);
	return (true);
}

/* Extended fields */

static uint32_t
xfield_padded(uint32_t size)
{
	return ((size + APFS_XF_ALIGN - 1) & ~(APFS_XF_ALIGN - 1));
}

/*
 * Field index of a blob whose fields before it take *offset bytes of the
 * data area: proves its header and its padded data inside the blob, then
 * fills field and advances *offset past the padding. It trusts xf's span
 * and count only, and every read stays inside the span, so an edited blob
 * description cannot widen a field past it.
 */
static bool
xfield_step(const apfs_xfields_t *xf, uint16_t index, size_t *offset,
    apfs_xfield_t *field)
{
	const unsigned char *head;
	uint64_t data_start;
	uint32_t padded;
	uint16_t size;

	data_start = APFS_XF_BLOB_BYTES +
	    (uint64_t)xf->count * APFS_XF_HEADER_BYTES;
	if (index >= xf->count || data_start > xf->length)
		return (false);
	head = xf->raw + APFS_XF_BLOB_BYTES +
	    (size_t)index * APFS_XF_HEADER_BYTES;
	size = fsimg_load_le16(head + 2);
	padded = xfield_padded(size);
	if (data_start + *offset + padded > xf->length)
		return (false);

	field->type = head[0];
	field->flags = head[1];
	field->size = size;
	field->data = xf->raw + data_start + *offset;
	*offset += padded;
	return (true);
}

/*
 * Whether type is already marked in seen, a set of the 256 field types; it
 * is marked on return either way.
 */
static bool
type_repeated(unsigned char *seen, uint8_t type)
{
	unsigned char bit;
	bool repeated;

	bit = (unsigned char)(1u << (type % 8));
	repeated = (seen[type / 8] & bit) != 0;
	seen[type / 8] |= bit;
	return (repeated);
}

/* Whether the bytes padding a proved field to its alignment are zero. */
static bool
padding_zero(const apfs_xfield_t *field)
{
	uint32_t i;

	for (i = field->size; i < xfield_padded(field->size); i++)
		if (field->data[i] != 0)
			return (false);
	return (true);
}

bool
apfs_xfields_decode(const unsigned char *raw, size_t length,
    apfs_xfields_t *xf)
{
	unsigned char seen[256 / 8];
	apfs_xfields_t blob;
	apfs_xfield_t field;
	size_t offset;
	uint16_t i;

	if (length == 0) {
		xf->raw = NULL;
		xf->length = 0;
		xf->count = 0;
		xf->used = 0;
		return (true);
	}
	if (length < APFS_XF_BLOB_BYTES)
		return (false);

	/* The headers and the used data fill the blob exactly. */
	blob.raw = raw;
	blob.length = length;
	blob.count = fsimg_load_le16(raw);
	blob.used = fsimg_load_le16(raw + 2);
	if (APFS_XF_BLOB_BYTES + (uint64_t)blob.count * APFS_XF_HEADER_BYTES +
	    blob.used != length)
		return (false);

	/* Each field's padded data follows the last, and the used count is
	 * their sum.
	 */
	bzero(seen, sizeof(seen));
	offset = 0;
	for (i = 0; i < blob.count; i++) {
		if (!xfield_step(&blob, i, &offset, &field))
			return (false);
		if (!padding_zero(&field))
			return (false);
		if (type_repeated(seen, field.type))
			return (false);
	}
	if (offset != blob.used)
		return (false);

	*xf = blob;
	return (true);
}

bool
apfs_xfield_get(const apfs_xfields_t *xf, uint16_t index,
    apfs_xfield_t *field)
{
	apfs_xfield_t found;
	size_t offset;
	uint32_t i;

	offset = 0;
	for (i = 0; i <= index; i++)
		if (!xfield_step(xf, (uint16_t)i, &offset, &found))
			return (false);
	*field = found;
	return (true);
}

bool
apfs_xfield_find(const apfs_xfields_t *xf, uint8_t type,
    apfs_xfield_t *field)
{
	apfs_xfield_t found;
	size_t offset;
	uint16_t i;

	offset = 0;
	for (i = 0; i < xf->count; i++) {
		if (!xfield_step(xf, i, &offset, &found))
			return (false);
		if (found.type == type) {
			*field = found;
			return (true);
		}
	}
	return (false);
}

bool
apfs_xfields_size(const apfs_xfield_t *fields, uint16_t count,
    size_t *size)
{
	uint64_t used;
	uint16_t i;

	used = 0;
	for (i = 0; i < count; i++)
		used += xfield_padded(fields[i].size);
	if (used > UINT16_MAX)
		return (false);

	*size = APFS_XF_BLOB_BYTES + (size_t)count * APFS_XF_HEADER_BYTES +
	    (size_t)used;
	return (true);
}

bool
apfs_xfields_encode(unsigned char *raw, size_t length,
    const apfs_xfield_t *fields, uint16_t count)
{
	unsigned char seen[256 / 8];
	unsigned char *head;
	size_t at, size;
	uint32_t padded;
	uint16_t i;

	if (!apfs_xfields_size(fields, count, &size) || size != length)
		return (false);
	bzero(seen, sizeof(seen));
	for (i = 0; i < count; i++)
		if (type_repeated(seen, fields[i].type))
			return (false);

	at = APFS_XF_BLOB_BYTES + (size_t)count * APFS_XF_HEADER_BYTES;
	fsimg_store_le16(raw, count);
	fsimg_store_le16(raw + 2, (uint16_t)(size - at));
	for (i = 0; i < count; i++) {
		head = raw + APFS_XF_BLOB_BYTES +
		    (size_t)i * APFS_XF_HEADER_BYTES;
		head[0] = fields[i].type;
		head[1] = fields[i].flags;
		fsimg_store_le16(head + 2, fields[i].size);
		padded = xfield_padded(fields[i].size);
		if (fields[i].size > 0)
			bcopy(fields[i].data, raw + at, fields[i].size);
		bzero(raw + at + fields[i].size, padded - fields[i].size);
		at += padded;
	}
	return (true);
}

bool
apfs_xfield_u32(const apfs_xfield_t *field, uint32_t *value)
{
	if (field->size != sizeof(uint32_t))
		return (false);
	*value = fsimg_load_le32(field->data);
	return (true);
}

bool
apfs_xfield_u64(const apfs_xfield_t *field, uint64_t *value)
{
	if (field->size != sizeof(uint64_t))
		return (false);
	*value = fsimg_load_le64(field->data);
	return (true);
}

bool
apfs_xfield_name(const apfs_xfield_t *field, const char **name,
    uint16_t *name_len)
{
	if (field->size == 0 || field->data[field->size - 1] != '\0')
		return (false);
	*name = (const char *)field->data;
	*name_len = (uint16_t)(field->size - 1);
	return (true);
}

/*
 * Whether a record encoder may copy blob xf after its fixed part of fixed
 * bytes: the record's length is exactly both, and the blob decodes.
 */
static bool
xfields_copyable(const apfs_xfields_t *xf, size_t length, size_t fixed)
{
	apfs_xfields_t check;

	return (length >= fixed && length - fixed == xf->length &&
	    apfs_xfields_decode(xf->raw, xf->length, &check));
}

/* Data streams and directory statistics */

bool
apfs_dstream_decode(const unsigned char *raw, size_t length,
    apfs_dstream_t *dstream)
{
	if (length != APFS_DSTREAM_BYTES)
		return (false);

	dstream->size = fsimg_load_le64(raw);
	dstream->alloced_size = fsimg_load_le64(raw + 8);
	dstream->default_crypto_id = fsimg_load_le64(raw + 16);
	dstream->total_bytes_written = fsimg_load_le64(raw + 24);
	dstream->total_bytes_read = fsimg_load_le64(raw + 32);
	return (true);
}

bool
apfs_dstream_encode(unsigned char *raw, size_t length,
    const apfs_dstream_t *dstream)
{
	if (length != APFS_DSTREAM_BYTES)
		return (false);

	fsimg_store_le64(raw, dstream->size);
	fsimg_store_le64(raw + 8, dstream->alloced_size);
	fsimg_store_le64(raw + 16, dstream->default_crypto_id);
	fsimg_store_le64(raw + 24, dstream->total_bytes_written);
	fsimg_store_le64(raw + 32, dstream->total_bytes_read);
	return (true);
}

bool
apfs_dir_stats_decode(const unsigned char *raw, size_t length,
    apfs_dir_stats_t *stats)
{
	if (length != APFS_DIR_STATS_BYTES)
		return (false);

	stats->num_children = fsimg_load_le64(raw);
	stats->total_size = fsimg_load_le64(raw + 8);
	stats->chained_key = fsimg_load_le64(raw + 16);
	stats->gen_count = fsimg_load_le64(raw + 24);
	return (true);
}

bool
apfs_dir_stats_encode(unsigned char *raw, size_t length,
    const apfs_dir_stats_t *stats)
{
	if (length != APFS_DIR_STATS_BYTES)
		return (false);

	fsimg_store_le64(raw, stats->num_children);
	fsimg_store_le64(raw + 8, stats->total_size);
	fsimg_store_le64(raw + 16, stats->chained_key);
	fsimg_store_le64(raw + 24, stats->gen_count);
	return (true);
}

/* Inodes */

bool
apfs_inode_val_decode(const unsigned char *raw, size_t length,
    apfs_inode_val_t *inode)
{
	apfs_xfields_t xfields;

	if (length < APFS_INODE_VAL_BYTES ||
	    !apfs_xfields_decode(raw + APFS_INODE_VAL_BYTES,
	    length - APFS_INODE_VAL_BYTES, &xfields))
		return (false);

	inode->xfields = xfields;
	inode->parent = fsimg_load_le64(raw);
	inode->private_id = fsimg_load_le64(raw + 8);
	inode->create_time = load_le64_signed(raw + 16);
	inode->mod_time = load_le64_signed(raw + 24);
	inode->change_time = load_le64_signed(raw + 32);
	inode->access_time = load_le64_signed(raw + 40);
	inode->internal_flags = fsimg_load_le64(raw + 48);
	inode->count = load_le32_signed(raw + 56);
	inode->protection_class = fsimg_load_le32(raw + 60);
	inode->write_generation = fsimg_load_le32(raw + 64);
	inode->bsd_flags = fsimg_load_le32(raw + 68);
	inode->owner = fsimg_load_le32(raw + 72);
	inode->group = fsimg_load_le32(raw + 76);
	inode->mode = fsimg_load_le16(raw + 80);
	inode->uncompressed_size = fsimg_load_le64(raw + 84);
	return (true);
}

bool
apfs_inode_val_encode(unsigned char *raw, size_t length,
    const apfs_inode_val_t *inode)
{
	if (!xfields_copyable(&inode->xfields, length, APFS_INODE_VAL_BYTES))
		return (false);

	fsimg_store_le64(raw, inode->parent);
	fsimg_store_le64(raw + 8, inode->private_id);
	fsimg_store_le64(raw + 16, (uint64_t)inode->create_time);
	fsimg_store_le64(raw + 24, (uint64_t)inode->mod_time);
	fsimg_store_le64(raw + 32, (uint64_t)inode->change_time);
	fsimg_store_le64(raw + 40, (uint64_t)inode->access_time);
	fsimg_store_le64(raw + 48, inode->internal_flags);
	fsimg_store_le32(raw + 56, (uint32_t)inode->count);
	fsimg_store_le32(raw + 60, inode->protection_class);
	fsimg_store_le32(raw + 64, inode->write_generation);
	fsimg_store_le32(raw + 68, inode->bsd_flags);
	fsimg_store_le32(raw + 72, inode->owner);
	fsimg_store_le32(raw + 76, inode->group);
	fsimg_store_le16(raw + 80, inode->mode);
	fsimg_store_le16(raw + 82, 0);
	fsimg_store_le64(raw + 84, inode->uncompressed_size);
	if (inode->xfields.length > 0)
		bcopy(inode->xfields.raw, raw + APFS_INODE_VAL_BYTES,
		    inode->xfields.length);
	return (true);
}

/* Directory records */

bool
apfs_drec_key_decode(const unsigned char *raw, size_t length, bool hashed,
    apfs_drec_key_t *key)
{
	const char *name;
	uint64_t parent;
	size_t at;
	uint32_t hash, stored, word;
	uint16_t name_len;

	at = hashed ? APFS_DREC_HASHED_KEY_BYTES : APFS_DREC_KEY_BYTES;
	if (!jkey_load(raw, length, APFS_J_DREC, at, &parent))
		return (false);
	if (hashed) {
		word = fsimg_load_le32(raw + 8);
		stored = word & APFS_DREC_LEN_MASK;
		hash = (word & APFS_DREC_HASH_MASK) >> APFS_DREC_HASH_SHIFT;
	} else {
		stored = fsimg_load_le16(raw + 8);
		hash = 0;
	}
	if (!name_span(raw, length, at, stored, &name, &name_len))
		return (false);

	key->name = name;
	key->parent = parent;
	key->hash = hash;
	key->name_len = name_len;
	key->hashed = hashed;
	return (true);
}

bool
apfs_drec_key_encode(unsigned char *raw, size_t length,
    const apfs_drec_key_t *key)
{
	size_t at;
	uint32_t max, stored;
	bool hash_fits;

	if (key->hashed) {
		at = APFS_DREC_HASHED_KEY_BYTES;
		max = APFS_DREC_LEN_MASK;
		hash_fits = key->hash <=
		    APFS_DREC_HASH_MASK >> APFS_DREC_HASH_SHIFT;
	} else {
		at = APFS_DREC_KEY_BYTES;
		max = UINT16_MAX;
		hash_fits = key->hash == 0;
	}
	if (key->parent > APFS_JKEY_OID_MASK || !hash_fits ||
	    !name_fits(length, at, key->name_len, max))
		return (false);

	stored = (uint32_t)key->name_len + 1;
	jkey_store(raw, key->parent, APFS_J_DREC);
	if (key->hashed)
		fsimg_store_le32(raw + 8, key->hash << APFS_DREC_HASH_SHIFT |
		    stored);
	else
		fsimg_store_le16(raw + 8, (uint16_t)stored);
	name_store(raw, at, key->name, key->name_len);
	return (true);
}

bool
apfs_drec_val_decode(const unsigned char *raw, size_t length,
    apfs_drec_val_t *drec)
{
	apfs_xfields_t xfields;

	if (length < APFS_DREC_VAL_BYTES ||
	    !apfs_xfields_decode(raw + APFS_DREC_VAL_BYTES,
	    length - APFS_DREC_VAL_BYTES, &xfields))
		return (false);

	drec->xfields = xfields;
	drec->child = fsimg_load_le64(raw);
	drec->date_added = load_le64_signed(raw + 8);
	drec->flags = fsimg_load_le16(raw + 16);
	return (true);
}

bool
apfs_drec_val_encode(unsigned char *raw, size_t length,
    const apfs_drec_val_t *drec)
{
	if (!xfields_copyable(&drec->xfields, length, APFS_DREC_VAL_BYTES))
		return (false);

	fsimg_store_le64(raw, drec->child);
	fsimg_store_le64(raw + 8, (uint64_t)drec->date_added);
	fsimg_store_le16(raw + 16, drec->flags);
	if (drec->xfields.length > 0)
		bcopy(drec->xfields.raw, raw + APFS_DREC_VAL_BYTES,
		    drec->xfields.length);
	return (true);
}

/*
 * Keys holding a u16 name length after the header and the name after their
 * fixed part of fixed bytes: extended attributes and snapshot names.
 */
static bool
name_key_decode(const unsigned char *raw, size_t length, uint32_t kind,
    size_t fixed, apfs_name_key_t *key)
{
	const char *name;
	uint64_t oid;
	uint16_t name_len, stored;

	if (!jkey_load(raw, length, kind, fixed, &oid))
		return (false);
	stored = fsimg_load_le16(raw + 8);
	if (!name_span(raw, length, fixed, stored, &name, &name_len))
		return (false);

	key->name = name;
	key->oid = oid;
	key->name_len = name_len;
	return (true);
}

static bool
name_key_encode(unsigned char *raw, size_t length, uint32_t kind,
    size_t fixed, const apfs_name_key_t *key)
{
	if (key->oid > APFS_JKEY_OID_MASK ||
	    !name_fits(length, fixed, key->name_len, UINT16_MAX))
		return (false);

	jkey_store(raw, key->oid, kind);
	fsimg_store_le16(raw + 8, (uint16_t)(key->name_len + 1));
	name_store(raw, fixed, key->name, key->name_len);
	return (true);
}

/* Extended attributes */

bool
apfs_xattr_key_decode(const unsigned char *raw, size_t length,
    apfs_name_key_t *key)
{
	return (name_key_decode(raw, length, APFS_J_XATTR,
	    APFS_XATTR_KEY_BYTES, key));
}

bool
apfs_xattr_key_encode(unsigned char *raw, size_t length,
    const apfs_name_key_t *key)
{
	return (name_key_encode(raw, length, APFS_J_XATTR,
	    APFS_XATTR_KEY_BYTES, key));
}

bool
apfs_xattr_val_decode(const unsigned char *raw, size_t length,
    apfs_xattr_val_t *xattr)
{
	apfs_dstream_t dstream;
	uint64_t stream_oid;
	uint16_t data_len, flags;

	if (length < APFS_XATTR_VAL_BYTES)
		return (false);
	flags = fsimg_load_le16(raw);
	data_len = fsimg_load_le16(raw + 2);

	/* The layout flag in force selects inline data or a stream. */
	switch (flags & (APFS_XATTR_DATA_STREAM | APFS_XATTR_DATA_EMBEDDED)) {
	case APFS_XATTR_DATA_EMBEDDED:
		if (length - APFS_XATTR_VAL_BYTES != data_len)
			return (false);
		bzero(&dstream, sizeof(dstream));
		stream_oid = 0;
		xattr->data = raw + APFS_XATTR_VAL_BYTES;
		break;
	case APFS_XATTR_DATA_STREAM:
		if (data_len != APFS_XATTR_STREAM_BYTES ||
		    length - APFS_XATTR_VAL_BYTES != data_len)
			return (false);
		stream_oid = fsimg_load_le64(raw + APFS_XATTR_VAL_BYTES);
		(void)apfs_dstream_decode(raw + APFS_XATTR_VAL_BYTES + 8,
		    APFS_DSTREAM_BYTES, &dstream);
		xattr->data = NULL;
		break;
	default:
		return (false);
	}

	xattr->dstream = dstream;
	xattr->stream_oid = stream_oid;
	xattr->flags = flags;
	xattr->data_len = data_len;
	return (true);
}

bool
apfs_xattr_val_encode(unsigned char *raw, size_t length,
    const apfs_xattr_val_t *xattr)
{
	bool stream;

	switch (xattr->flags &
	    (APFS_XATTR_DATA_STREAM | APFS_XATTR_DATA_EMBEDDED)) {
	case APFS_XATTR_DATA_EMBEDDED:
		stream = false;
		break;
	case APFS_XATTR_DATA_STREAM:
		if (xattr->data_len != APFS_XATTR_STREAM_BYTES)
			return (false);
		stream = true;
		break;
	default:
		return (false);
	}
	if (length < APFS_XATTR_VAL_BYTES ||
	    length - APFS_XATTR_VAL_BYTES != xattr->data_len)
		return (false);

	fsimg_store_le16(raw, xattr->flags);
	fsimg_store_le16(raw + 2, xattr->data_len);
	if (stream) {
		fsimg_store_le64(raw + APFS_XATTR_VAL_BYTES, xattr->stream_oid);
		(void)apfs_dstream_encode(raw + APFS_XATTR_VAL_BYTES + 8,
		    APFS_DSTREAM_BYTES, &xattr->dstream);
	} else if (xattr->data_len > 0)
		bcopy(xattr->data, raw + APFS_XATTR_VAL_BYTES, xattr->data_len);
	return (true);
}

/* Sibling links, sibling maps and data-stream IDs */

bool
apfs_sibling_key_decode(const unsigned char *raw, size_t length,
    apfs_sibling_key_t *key)
{
	uint64_t inode;

	if (length != APFS_SIBLING_KEY_BYTES ||
	    !jkey_load(raw, length, APFS_J_SIBLING_LINK,
	    APFS_SIBLING_KEY_BYTES, &inode))
		return (false);

	key->inode = inode;
	key->sibling_id = fsimg_load_le64(raw + 8);
	return (true);
}

bool
apfs_sibling_key_encode(unsigned char *raw, size_t length,
    const apfs_sibling_key_t *key)
{
	if (length != APFS_SIBLING_KEY_BYTES ||
	    key->inode > APFS_JKEY_OID_MASK)
		return (false);

	jkey_store(raw, key->inode, APFS_J_SIBLING_LINK);
	fsimg_store_le64(raw + 8, key->sibling_id);
	return (true);
}

bool
apfs_sibling_val_decode(const unsigned char *raw, size_t length,
    apfs_sibling_val_t *sibling)
{
	const char *name;
	uint16_t name_len, stored;

	if (length < APFS_SIBLING_VAL_BYTES)
		return (false);
	stored = fsimg_load_le16(raw + 8);
	if (!name_span(raw, length, APFS_SIBLING_VAL_BYTES, stored, &name,
	    &name_len))
		return (false);

	sibling->name = name;
	sibling->parent = fsimg_load_le64(raw);
	sibling->name_len = name_len;
	return (true);
}

bool
apfs_sibling_val_encode(unsigned char *raw, size_t length,
    const apfs_sibling_val_t *sibling)
{
	if (!name_fits(length, APFS_SIBLING_VAL_BYTES, sibling->name_len,
	    UINT16_MAX))
		return (false);

	fsimg_store_le64(raw, sibling->parent);
	fsimg_store_le16(raw + 8, (uint16_t)(sibling->name_len + 1));
	name_store(raw, APFS_SIBLING_VAL_BYTES, sibling->name,
	    sibling->name_len);
	return (true);
}

bool
apfs_sibling_map_val_decode(const unsigned char *raw, size_t length,
    uint64_t *inode)
{
	if (length != APFS_SIBLING_MAP_VAL_BYTES)
		return (false);

	*inode = fsimg_load_le64(raw);
	return (true);
}

bool
apfs_sibling_map_val_encode(unsigned char *raw, size_t length,
    uint64_t inode)
{
	if (length != APFS_SIBLING_MAP_VAL_BYTES)
		return (false);

	fsimg_store_le64(raw, inode);
	return (true);
}

bool
apfs_dstream_id_val_decode(const unsigned char *raw, size_t length,
    uint32_t *refcnt)
{
	if (length != APFS_DSTREAM_ID_VAL_BYTES)
		return (false);

	*refcnt = fsimg_load_le32(raw);
	return (true);
}

bool
apfs_dstream_id_val_encode(unsigned char *raw, size_t length,
    uint32_t refcnt)
{
	if (length != APFS_DSTREAM_ID_VAL_BYTES)
		return (false);

	fsimg_store_le32(raw, refcnt);
	return (true);
}

/* File and physical extents */

bool
apfs_extent_key_decode(const unsigned char *raw, size_t length,
    apfs_extent_key_t *key)
{
	uint64_t private_id;

	if (length != APFS_EXTENT_KEY_BYTES ||
	    !jkey_load(raw, length, APFS_J_FILE_EXTENT, APFS_EXTENT_KEY_BYTES,
	    &private_id))
		return (false);

	key->private_id = private_id;
	key->logical = fsimg_load_le64(raw + 8);
	return (true);
}

bool
apfs_extent_key_encode(unsigned char *raw, size_t length,
    const apfs_extent_key_t *key)
{
	if (length != APFS_EXTENT_KEY_BYTES ||
	    key->private_id > APFS_JKEY_OID_MASK)
		return (false);

	jkey_store(raw, key->private_id, APFS_J_FILE_EXTENT);
	fsimg_store_le64(raw + 8, key->logical);
	return (true);
}

bool
apfs_extent_val_decode(const unsigned char *raw, size_t length,
    apfs_extent_val_t *extent)
{
	uint64_t len_and_flags;

	if (length != APFS_EXTENT_VAL_BYTES)
		return (false);

	len_and_flags = fsimg_load_le64(raw);
	extent->length = len_and_flags & APFS_EXTENT_LEN_MASK;
	extent->flags = (uint8_t)(len_and_flags >> APFS_EXTENT_FLAGS_SHIFT);
	extent->phys_block = fsimg_load_le64(raw + 8);
	extent->crypto_id = fsimg_load_le64(raw + 16);
	return (true);
}

bool
apfs_extent_val_encode(unsigned char *raw, size_t length,
    const apfs_extent_val_t *extent)
{
	if (length != APFS_EXTENT_VAL_BYTES ||
	    extent->length > APFS_EXTENT_LEN_MASK)
		return (false);

	fsimg_store_le64(raw, extent->length |
	    (uint64_t)extent->flags << APFS_EXTENT_FLAGS_SHIFT);
	fsimg_store_le64(raw + 8, extent->phys_block);
	fsimg_store_le64(raw + 16, extent->crypto_id);
	return (true);
}

bool
apfs_pext_val_decode(const unsigned char *raw, size_t length,
    apfs_pext_val_t *pext)
{
	uint64_t len_and_kind;

	if (length != APFS_PEXT_VAL_BYTES)
		return (false);

	len_and_kind = fsimg_load_le64(raw);
	pext->length = len_and_kind & APFS_PEXT_LEN_MASK;
	pext->kind = (uint8_t)(len_and_kind >> APFS_PEXT_KIND_SHIFT);
	pext->owner = fsimg_load_le64(raw + 8);
	pext->refcnt = load_le32_signed(raw + 16);
	return (true);
}

bool
apfs_pext_val_encode(unsigned char *raw, size_t length,
    const apfs_pext_val_t *pext)
{
	if (length != APFS_PEXT_VAL_BYTES ||
	    pext->length > APFS_PEXT_LEN_MASK ||
	    pext->kind > APFS_PEXT_KIND_MASK)
		return (false);

	fsimg_store_le64(raw, pext->length |
	    (uint64_t)pext->kind << APFS_PEXT_KIND_SHIFT);
	fsimg_store_le64(raw + 8, pext->owner);
	fsimg_store_le32(raw + 16, (uint32_t)pext->refcnt);
	return (true);
}

/* Snapshot metadata and names */

bool
apfs_snap_meta_val_decode(const unsigned char *raw, size_t length,
    apfs_snap_meta_val_t *snap)
{
	const char *name;
	uint16_t name_len, stored;

	if (length < APFS_SNAP_META_VAL_BYTES)
		return (false);
	stored = fsimg_load_le16(raw + 48);
	if (!name_span(raw, length, APFS_SNAP_META_VAL_BYTES, stored, &name,
	    &name_len))
		return (false);

	snap->name = name;
	snap->extentref_tree_oid = fsimg_load_le64(raw);
	snap->sblock_oid = fsimg_load_le64(raw + 8);
	snap->create_time = load_le64_signed(raw + 16);
	snap->change_time = load_le64_signed(raw + 24);
	snap->inum = fsimg_load_le64(raw + 32);
	snap->extentref_tree_type = fsimg_load_le32(raw + 40);
	snap->flags = fsimg_load_le32(raw + 44);
	snap->name_len = name_len;
	return (true);
}

bool
apfs_snap_meta_val_encode(unsigned char *raw, size_t length,
    const apfs_snap_meta_val_t *snap)
{
	if (!name_fits(length, APFS_SNAP_META_VAL_BYTES, snap->name_len,
	    UINT16_MAX))
		return (false);

	fsimg_store_le64(raw, snap->extentref_tree_oid);
	fsimg_store_le64(raw + 8, snap->sblock_oid);
	fsimg_store_le64(raw + 16, (uint64_t)snap->create_time);
	fsimg_store_le64(raw + 24, (uint64_t)snap->change_time);
	fsimg_store_le64(raw + 32, snap->inum);
	fsimg_store_le32(raw + 40, snap->extentref_tree_type);
	fsimg_store_le32(raw + 44, snap->flags);
	fsimg_store_le16(raw + 48, (uint16_t)(snap->name_len + 1));
	name_store(raw, APFS_SNAP_META_VAL_BYTES, snap->name, snap->name_len);
	return (true);
}

bool
apfs_snap_name_key_decode(const unsigned char *raw, size_t length,
    apfs_name_key_t *key)
{
	return (name_key_decode(raw, length, APFS_J_SNAP_NAME,
	    APFS_SNAP_NAME_KEY_BYTES, key));
}

bool
apfs_snap_name_key_encode(unsigned char *raw, size_t length,
    const apfs_name_key_t *key)
{
	return (name_key_encode(raw, length, APFS_J_SNAP_NAME,
	    APFS_SNAP_NAME_KEY_BYTES, key));
}

bool
apfs_snap_name_val_decode(const unsigned char *raw, size_t length,
    uint64_t *xid)
{
	if (length != APFS_SNAP_NAME_VAL_BYTES)
		return (false);

	*xid = fsimg_load_le64(raw);
	return (true);
}

bool
apfs_snap_name_val_encode(unsigned char *raw, size_t length, uint64_t xid)
{
	if (length != APFS_SNAP_NAME_VAL_BYTES)
		return (false);

	fsimg_store_le64(raw, xid);
	return (true);
}

/* File information */

bool
apfs_file_info_key_decode(const unsigned char *raw, size_t length,
    apfs_file_info_key_t *key)
{
	uint64_t private_id, word;

	if (length != APFS_FILE_INFO_KEY_BYTES ||
	    !jkey_load(raw, length, APFS_J_FILE_INFO, APFS_FILE_INFO_KEY_BYTES,
	    &private_id))
		return (false);

	word = fsimg_load_le64(raw + 8);
	key->private_id = private_id;
	key->lba = word & APFS_FILE_INFO_LBA_MASK;
	key->type = (uint8_t)(word >> APFS_FILE_INFO_TYPE_SHIFT);
	return (true);
}

bool
apfs_file_info_key_encode(unsigned char *raw, size_t length,
    const apfs_file_info_key_t *key)
{
	if (length != APFS_FILE_INFO_KEY_BYTES ||
	    key->private_id > APFS_JKEY_OID_MASK ||
	    key->lba > APFS_FILE_INFO_LBA_MASK)
		return (false);

	jkey_store(raw, key->private_id, APFS_J_FILE_INFO);
	fsimg_store_le64(raw + 8, key->lba |
	    (uint64_t)key->type << APFS_FILE_INFO_TYPE_SHIFT);
	return (true);
}

bool
apfs_file_info_val_decode(const unsigned char *raw, size_t length,
    apfs_file_info_val_t *info)
{
	if (length < APFS_FILE_INFO_VAL_BYTES ||
	    length - APFS_FILE_INFO_VAL_BYTES != raw[2])
		return (false);

	info->hash = raw + APFS_FILE_INFO_VAL_BYTES;
	info->hashed_len = fsimg_load_le16(raw);
	info->hash_size = raw[2];
	return (true);
}

bool
apfs_file_info_val_encode(unsigned char *raw, size_t length,
    const apfs_file_info_val_t *info)
{
	if (length < APFS_FILE_INFO_VAL_BYTES ||
	    length - APFS_FILE_INFO_VAL_BYTES != info->hash_size)
		return (false);

	fsimg_store_le16(raw, info->hashed_len);
	raw[2] = info->hash_size;
	if (info->hash_size > 0)
		bcopy(info->hash, raw + APFS_FILE_INFO_VAL_BYTES,
		    info->hash_size);
	return (true);
}

/* A formatting or modification record: a 32-byte identifier, then its time
 * and the last XID it wrote.
 */
static void
fs_change_decode(const unsigned char *raw, apfs_fs_change_t *change)
{
	bcopy(raw, change->id, APFS_FS_CHANGE_ID_BYTES);
	change->time = fsimg_load_le64(raw + 32);
	change->last_xid = fsimg_load_le64(raw + 40);
}

static void
fs_change_encode(unsigned char *raw, const apfs_fs_change_t *change)
{
	bcopy(change->id, raw, APFS_FS_CHANGE_ID_BYTES);
	fsimg_store_le64(raw + 32, change->time);
	fsimg_store_le64(raw + 40, change->last_xid);
}

bool
apfs_fs_decode(const unsigned char *raw, size_t length, apfs_fs_t *fs)
{
	if (length < APFS_FS_BYTES)
		return (false);

	fs->magic = fsimg_load_le32(raw + 32);
	fs->index = fsimg_load_le32(raw + 36);
	fs->features = fsimg_load_le64(raw + 40);
	fs->ro_compat_features = fsimg_load_le64(raw + 48);
	fs->incompat_features = fsimg_load_le64(raw + 56);
	fs->unmount_time = fsimg_load_le64(raw + 64);
	fs->reserve_blocks = fsimg_load_le64(raw + 72);
	fs->quota_blocks = fsimg_load_le64(raw + 80);
	fs->alloc_blocks = fsimg_load_le64(raw + 88);
	bcopy(raw + 96, fs->crypto, APFS_FS_CRYPTO_BYTES);
	fs->root_tree_type = fsimg_load_le32(raw + 116);
	fs->extentref_tree_type = fsimg_load_le32(raw + 120);
	fs->snapshot_tree_type = fsimg_load_le32(raw + 124);
	fs->omap_oid = fsimg_load_le64(raw + 128);
	fs->root_tree_oid = fsimg_load_le64(raw + 136);
	fs->extentref_tree_oid = fsimg_load_le64(raw + 144);
	fs->snapshot_tree_oid = fsimg_load_le64(raw + 152);
	fs->revert_to_xid = fsimg_load_le64(raw + 160);
	fs->revert_to_oid = fsimg_load_le64(raw + 168);
	fs->next_obj_id = fsimg_load_le64(raw + 176);
	fs->files = fsimg_load_le64(raw + 184);
	fs->directories = fsimg_load_le64(raw + 192);
	fs->symlinks = fsimg_load_le64(raw + 200);
	fs->other_objects = fsimg_load_le64(raw + 208);
	fs->snapshots = fsimg_load_le64(raw + 216);
	fs->blocks_allocated = fsimg_load_le64(raw + 224);
	fs->blocks_freed = fsimg_load_le64(raw + 232);
	bcopy(raw + 240, fs->uuid, APFS_UUID_BYTES);
	fs->modified_time = fsimg_load_le64(raw + 256);
	fs->flags = fsimg_load_le64(raw + 264);
	fs_change_decode(raw + 272, &fs->formatted_by);
	for (unsigned i = 0; i < APFS_FS_CHANGES; i++)
		fs_change_decode(raw + 320 + 48 * i, &fs->modified_by[i]);
	bcopy(raw + 704, fs->name, APFS_FS_NAME_BYTES);
	fs->next_doc_id = fsimg_load_le32(raw + 960);
	fs->role = fsimg_load_le16(raw + 964);
	fs->root_to_xid = fsimg_load_le64(raw + 968);
	fs->rolling_state_oid = fsimg_load_le64(raw + 976);
	fs->clone_epoch = fsimg_load_le64(raw + 984);
	fs->clone_xid = fsimg_load_le64(raw + 992);
	fs->snapshot_ext_oid = fsimg_load_le64(raw + 1000);
	bcopy(raw + 1008, fs->group_uuid, APFS_UUID_BYTES);
	fs->integrity_oid = fsimg_load_le64(raw + 1024);
	fs->fext_tree_oid = fsimg_load_le64(raw + 1032);
	fs->fext_tree_type = fsimg_load_le32(raw + 1040);
	return (true);
}

/* The reserved field at 966 is written zero. */
bool
apfs_fs_encode(unsigned char *raw, size_t length, const apfs_fs_t *fs)
{
	if (length < APFS_FS_BYTES)
		return (false);

	fsimg_store_le32(raw + 32, fs->magic);
	fsimg_store_le32(raw + 36, fs->index);
	fsimg_store_le64(raw + 40, fs->features);
	fsimg_store_le64(raw + 48, fs->ro_compat_features);
	fsimg_store_le64(raw + 56, fs->incompat_features);
	fsimg_store_le64(raw + 64, fs->unmount_time);
	fsimg_store_le64(raw + 72, fs->reserve_blocks);
	fsimg_store_le64(raw + 80, fs->quota_blocks);
	fsimg_store_le64(raw + 88, fs->alloc_blocks);
	bcopy(fs->crypto, raw + 96, APFS_FS_CRYPTO_BYTES);
	fsimg_store_le32(raw + 116, fs->root_tree_type);
	fsimg_store_le32(raw + 120, fs->extentref_tree_type);
	fsimg_store_le32(raw + 124, fs->snapshot_tree_type);
	fsimg_store_le64(raw + 128, fs->omap_oid);
	fsimg_store_le64(raw + 136, fs->root_tree_oid);
	fsimg_store_le64(raw + 144, fs->extentref_tree_oid);
	fsimg_store_le64(raw + 152, fs->snapshot_tree_oid);
	fsimg_store_le64(raw + 160, fs->revert_to_xid);
	fsimg_store_le64(raw + 168, fs->revert_to_oid);
	fsimg_store_le64(raw + 176, fs->next_obj_id);
	fsimg_store_le64(raw + 184, fs->files);
	fsimg_store_le64(raw + 192, fs->directories);
	fsimg_store_le64(raw + 200, fs->symlinks);
	fsimg_store_le64(raw + 208, fs->other_objects);
	fsimg_store_le64(raw + 216, fs->snapshots);
	fsimg_store_le64(raw + 224, fs->blocks_allocated);
	fsimg_store_le64(raw + 232, fs->blocks_freed);
	bcopy(fs->uuid, raw + 240, APFS_UUID_BYTES);
	fsimg_store_le64(raw + 256, fs->modified_time);
	fsimg_store_le64(raw + 264, fs->flags);
	fs_change_encode(raw + 272, &fs->formatted_by);
	for (unsigned i = 0; i < APFS_FS_CHANGES; i++)
		fs_change_encode(raw + 320 + 48 * i, &fs->modified_by[i]);
	bcopy(fs->name, raw + 704, APFS_FS_NAME_BYTES);
	fsimg_store_le32(raw + 960, fs->next_doc_id);
	fsimg_store_le16(raw + 964, fs->role);
	fsimg_store_le16(raw + 966, 0);
	fsimg_store_le64(raw + 968, fs->root_to_xid);
	fsimg_store_le64(raw + 976, fs->rolling_state_oid);
	fsimg_store_le64(raw + 984, fs->clone_epoch);
	fsimg_store_le64(raw + 992, fs->clone_xid);
	fsimg_store_le64(raw + 1000, fs->snapshot_ext_oid);
	bcopy(fs->group_uuid, raw + 1008, APFS_UUID_BYTES);
	fsimg_store_le64(raw + 1024, fs->integrity_oid);
	fsimg_store_le64(raw + 1032, fs->fext_tree_oid);
	fsimg_store_le32(raw + 1040, fs->fext_tree_type);
	return (true);
}
