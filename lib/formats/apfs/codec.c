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
