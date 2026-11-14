/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"
#include "meta.h"
#include "reader.h"

/*
 * Checkpoint selection (specs/design-apfs.md, "Checkpoint selection" and
 * "Truncated containers"; specs/design-apfs-driver.md, "Open", step 2).
 *
 * One pass reads every descriptor slot once and records what the selection
 * rules need of it in a slot record, so runs are judged from records alone.
 * The candidates, superblocks that may be selected, are sorted by XID,
 * which finds ties, and judged from the newest down; the first whose
 * descriptor run is whole is selected. Its mapping blocks are then read a
 * second time to build the ephemeral mapping table, and every mapped object
 * is read once. Every array grows by blocks read, never by a declared count,
 * and a block missing from a truncated image fails its read in the funnel,
 * so truncation can never make a checkpoint look torn.
 */

#define ROLE_RING	"checkpoint descriptor"
#define ROLE_CHECKPOINT	"checkpoint"
#define ROLE_MAPPING	"checkpoint mapping"
#define ROLE_REAPER	"reaper"
#define ROLE_REAP_LIST	"reap list"

/* What one descriptor slot holds. */
typedef enum {
	SLOT_OTHER,		/* unused, damaged or not a descriptor block */
	SLOT_SUPERBLOCK,	/* a checkpoint candidate */
	SLOT_MAP		/* a valid checkpoint-mapping block */
} slot_kind_t;

/*
 * A slot record. For a candidate, desc_index and desc_len are its run and
 * fields_ok is the fourth rule of a whole run, which depends on the
 * superblock alone; for a mapping block, last is its last-map flag. The
 * block's stored checksum identifies its contents, so a block read again
 * after selection must carry the same one.
 */
typedef struct slot {
	uint64_t	checksum;
	uint64_t	xid;
	uint32_t	desc_index;
	uint32_t	desc_len;
	slot_kind_t	kind;
	bool		last;
	bool		fields_ok;
} slot_t;

/* Encoded records in metadata scratch: never native structures. */
#define SLOT_BYTES		32u	/* XID, run, kind, last, fields_ok, sum */
#define CANDIDATE_BYTES		12u	/* XID, slot */
#define MAPPING_BYTES		36u	/* OID, address, volume, type, subtype, size */

static int		candidate_compare(const void *, size_t, const void *,
			    size_t, void *);
static fsimg_status_t	candidates_select(apfs_container_t *, fsimg_store_t *,
			    fsimg_stream_t *, uint32_t, uint32_t *,
			    fsimg_error_t *);
static fsimg_status_t	checkpoint_load(apfs_container_t *, const apfs_nx_t *,
			    fsimg_store_t *, uint32_t, unsigned char *,
			    fsimg_error_t *);
static bool		data_run_holds(const apfs_nx_t *, uint64_t, uint64_t);
static fsimg_status_t	data_run_probe(apfs_container_t *, unsigned char *,
			    fsimg_error_t *);
static void		mapping_decode(const unsigned char *, apfs_cpm_t *);
static void		mapping_encode(unsigned char *, const apfs_cpm_t *);
static int		mapping_address_compare(const void *, size_t,
			    const void *, size_t, void *);
static int		mapping_oid_compare(const void *, size_t, const void *,
			    size_t, void *);
static fsimg_status_t	mappings_append(apfs_container_t *, const apfs_nx_t *,
			    const unsigned char *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	mappings_check(apfs_container_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	mappings_index(apfs_container_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	mapped_objects_read(apfs_container_t *,
			    unsigned char *, fsimg_error_t *);
static bool		range_inside(uint64_t, uint64_t, uint64_t);
static fsimg_status_t	ring_scan(apfs_container_t *, const apfs_nx_t *,
			    unsigned char *, fsimg_store_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	run_whole(apfs_container_t *, fsimg_store_t *,
			    uint32_t, uint32_t, const slot_t *, bool *,
			    fsimg_error_t *);
static fsimg_status_t	slot_classify(const apfs_nx_t *, uint64_t,
			    const unsigned char *, slot_t *, apfs_nx_t *,
			    fsimg_error_t *);
static fsimg_status_t	slot_get(fsimg_store_t *, uint32_t, slot_t *,
			    fsimg_error_t *);
static bool		slot_same(const slot_t *, const slot_t *);
static fsimg_status_t	slot_put(fsimg_store_t *, uint32_t, const slot_t *,
			    fsimg_error_t *);

/* Whether [start, start + count) is a nonempty range inside [1, n). */
static bool
range_inside(uint64_t start, uint64_t count, uint64_t n)
{
	return (count != 0 && start >= 1 && start < n && count <= n - start);
}

bool
apfs_block_size_valid(uint32_t b)
{
	return (b >= APFS_BLOCK_MIN && b <= APFS_BLOCK_MAX && (b & (b - 1)) == 0);
}

fsimg_status_t
apfs_nx_geometry(const apfs_nx_t *nx, const char *role, fsimg_error_t *error)
{
	uint64_t bytes, n = nx->block_count;
	uint32_t b = nx->block_size;

	if (nx->desc_noncontiguous || nx->data_noncontiguous)
		return (apfs_unsupported(error, role,
		    "has a noncontiguous checkpoint area"));
	if (!apfs_block_size_valid(b))
		return (apfs_corrupt(error, role, "has an invalid block size"));
	if (!fsimg_mul_u64(n, b, &bytes) || bytes < (UINT64_C(1) << 20))
		return (apfs_corrupt(error, role, "has an invalid block count"));
	if (nx->max_volumes == 0 || nx->max_volumes > APFS_NX_VOLUMES_MAX)
		return (apfs_corrupt(error, role,
		    "has an invalid maximum volume count"));
	for (uint32_t i = nx->max_volumes; i < APFS_NX_VOLUMES_MAX; i++)
		if (nx->volume_oids[i] != 0)
			return (apfs_corrupt(error, role,
			    "names a volume past its maximum count"));
	if (!range_inside(nx->desc_base, nx->desc_blocks, n) ||
	    !range_inside(nx->data_base, nx->data_blocks, n) ||
	    (nx->desc_base < nx->data_base + nx->data_blocks &&
	    nx->data_base < nx->desc_base + nx->desc_blocks))
		return (apfs_corrupt(error, role,
		    "has invalid checkpoint areas"));
	if ((nx->keylocker.count != 0 &&
	    !range_inside(nx->keylocker.start, nx->keylocker.count, n)) ||
	    (nx->media_keylocker.count != 0 &&
	    !range_inside(nx->media_keylocker.start,
	    nx->media_keylocker.count, n)))
		return (apfs_corrupt(error, role,
		    "has a keylocker range outside the container"));
	return (FSIMG_OK);
}

static fsimg_status_t
slot_put(fsimg_store_t *slots, uint32_t index, const slot_t *slot,
    fsimg_error_t *error)
{
	unsigned char raw[SLOT_BYTES] = { 0 };

	fsimg_store_le64(raw, slot->xid);
	fsimg_store_le32(raw + 8, slot->desc_index);
	fsimg_store_le32(raw + 12, slot->desc_len);
	raw[16] = (unsigned char)slot->kind;
	raw[17] = slot->last;
	raw[18] = slot->fields_ok;
	fsimg_store_le64(raw + 24, slot->checksum);
	return (fsimg_store_write(slots, (uint64_t)index * SLOT_BYTES, raw,
	    sizeof(raw), error));
}

static fsimg_status_t
slot_get(fsimg_store_t *slots, uint32_t index, slot_t *slot,
    fsimg_error_t *error)
{
	unsigned char raw[SLOT_BYTES];
	fsimg_status_t status;

	status = fsimg_store_read(slots, (uint64_t)index * SLOT_BYTES, raw,
	    sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	slot->xid = fsimg_load_le64(raw);
	slot->desc_index = fsimg_load_le32(raw + 8);
	slot->desc_len = fsimg_load_le32(raw + 12);
	slot->kind = (slot_kind_t)raw[16];
	slot->last = raw[17] != 0;
	slot->fields_ok = raw[18] != 0;
	slot->checksum = fsimg_load_le64(raw + 24);
	return (FSIMG_OK);
}

/*
 * Whether a block read again classifies exactly as its slot record: every
 * field the selection judged, and the stored checksum, which ties the
 * fields used afterwards to the bytes judged.
 */
static bool
slot_same(const slot_t *a, const slot_t *b)
{
	return (a->kind == b->kind && a->checksum == b->checksum &&
	    a->xid == b->xid && a->desc_index == b->desc_index &&
	    a->desc_len == b->desc_len && a->last == b->last &&
	    a->fields_ok == b->fields_ok);
}

/*
 * Classify the descriptor block at block, read into window, against block
 * 0's superblock. A block whose checksum fails, such as a never-written
 * slot of zeros, is unused, as is any block that is neither a candidate nor
 * a valid mapping block. The one failure is a candidate with a
 * noncontiguous area, which is FSIMG_EUNSUPPORTED. A candidate's superblock
 * is decoded into *nx when nx is not NULL.
 */
static fsimg_status_t
slot_classify(const apfs_nx_t *block0, uint64_t block,
    const unsigned char *window, slot_t *slot, apfs_nx_t *nx,
    fsimg_error_t *error)
{
	apfs_cpm_block_t header;
	apfs_cpm_t mapping;
	apfs_obj_t obj;
	apfs_nx_t candidate;
	size_t length = block0->block_size;
	fsimg_status_t status;

	*slot = (slot_t){ .kind = SLOT_OTHER };
	if (!apfs_object_verify(window, length) ||
	    !apfs_obj_decode(window, length, &obj) || obj.xid == 0 ||
	    obj.subtype != APFS_SUBTYPE_NONE ||
	    (obj.flags & (APFS_OBJ_NOHEADER | APFS_OBJ_ENCRYPTED |
	    APFS_OBJ_NONPERSISTENT)) != 0)
		return (FSIMG_OK);
	slot->checksum = obj.checksum;

	if (obj.kind == APFS_KIND_NX_SUPERBLOCK) {
		if ((obj.flags & APFS_OBJ_STORAGE_MASK) != APFS_OBJ_EPHEMERAL ||
		    obj.oid != 1 || !apfs_nx_decode(window, length, &candidate) ||
		    candidate.magic != APFS_NX_MAGIC ||
		    candidate.block_size != block0->block_size ||
		    memcmp(candidate.uuid, block0->uuid, APFS_UUID_BYTES) != 0 ||
		    candidate.desc_base != block0->desc_base ||
		    candidate.desc_blocks != block0->desc_blocks ||
		    candidate.desc_noncontiguous != block0->desc_noncontiguous)
			return (FSIMG_OK);
		/* A candidate of this container with a noncontiguous data area
		 * is unsupported; any other bad geometry makes it no candidate.
		 */
		status = apfs_nx_geometry(&candidate, ROLE_CHECKPOINT, error);
		if (status == FSIMG_EUNSUPPORTED)
			return (status);
		if (status != FSIMG_OK)
			return (FSIMG_OK);
		slot->kind = SLOT_SUPERBLOCK;
		slot->xid = obj.xid;
		slot->desc_index = candidate.desc_index;
		slot->desc_len = candidate.desc_len;
		slot->fields_ok = candidate.desc_next < candidate.desc_blocks &&
		    candidate.data_next < candidate.data_blocks &&
		    candidate.data_index < candidate.data_blocks &&
		    candidate.data_len <= candidate.data_blocks &&
		    candidate.next_xid > obj.xid;
		if (nx != NULL)
			*nx = candidate;
		return (FSIMG_OK);
	}

	if (obj.kind != APFS_KIND_CHECKPOINT_MAP ||
	    (obj.flags & APFS_OBJ_STORAGE_MASK) != APFS_OBJ_PHYSICAL ||
	    obj.oid != block || !apfs_cpm_block_decode(window, length, &header) ||
	    (header.flags & ~APFS_CPM_LAST) != 0)
		return (FSIMG_OK);
	for (uint32_t i = 0; i < header.count; i++) {
		if (!apfs_cpm_decode(window, length, &header, i, &mapping) ||
		    mapping.kind == 0 || mapping.size == 0 ||
		    mapping.size % block0->block_size != 0)
			return (FSIMG_OK);
	}
	slot->kind = SLOT_MAP;
	slot->xid = obj.xid;
	slot->last = (header.flags & APFS_CPM_LAST) != 0;
	return (FSIMG_OK);
}

/* Read every descriptor slot once, recording each and listing candidates. */
static fsimg_status_t
ring_scan(apfs_container_t *container, const apfs_nx_t *block0,
    unsigned char *window, fsimg_store_t *slots, fsimg_stream_t *candidates,
    fsimg_error_t *error)
{
	unsigned char raw[CANDIDATE_BYTES];
	slot_t slot;
	fsimg_status_t status;

	for (uint32_t i = 0; i < block0->desc_blocks; i++) {
		status = apfs_block_read(container, block0->desc_base + i,
		    window, ROLE_RING, error);
		if (status == FSIMG_OK)
			status = slot_classify(block0, block0->desc_base + i,
			    window, &slot, NULL, error);
		if (status == FSIMG_OK)
			status = fsimg_store_grow(slots,
			    ((uint64_t)i + 1) * SLOT_BYTES, error);
		if (status == FSIMG_OK)
			status = slot_put(slots, i, &slot, error);
		if (status == FSIMG_OK && slot.kind == SLOT_SUPERBLOCK) {
			fsimg_store_le64(raw, slot.xid);
			fsimg_store_le32(raw + 8, i);
			status = fsimg_stream_append(candidates, raw,
			    sizeof(raw), NULL, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Whether the candidate in slot index has a whole descriptor run: at least
 * two blocks, inside the ring, ending at its own slot, every other block a
 * valid mapping block of its XID, and the last-map flag on exactly the
 * run's last mapping block (rules 1 and 2; rule 3 is part of a valid mapping
 * block and rule 4 is fields_ok).
 */
static fsimg_status_t
run_whole(apfs_container_t *container, fsimg_store_t *slots, uint32_t ring,
    uint32_t index, const slot_t *candidate, bool *whole, fsimg_error_t *error)
{
	slot_t slot;
	uint32_t len = candidate->desc_len;
	fsimg_status_t status;
	bool last_map;

	*whole = false;
	if (!candidate->fields_ok || len < 2 || len > ring ||
	    candidate->desc_index >= ring ||
	    ((uint64_t)candidate->desc_index + len - 1) % ring != index)
		return (FSIMG_OK);
	for (uint32_t j = 0; j + 1 < len; j++) {
		status = fsimg_session_charge(&container->reader->session, 0, 0,
		    1, error);
		if (status == FSIMG_OK)
			status = slot_get(slots,
			    (uint32_t)(((uint64_t)candidate->desc_index + j) %
			    ring), &slot, error);
		if (status != FSIMG_OK)
			return (status);
		/* The run's last block is the superblock, so its last
		 * mapping block is the one before it.
		 */
		last_map = j + 2 == len;
		if (slot.kind != SLOT_MAP || slot.xid != candidate->xid ||
		    slot.last != last_map)
			return (FSIMG_OK);
	}
	*whole = true;
	return (FSIMG_OK);
}

/* Candidates sort newest first. */
static int
candidate_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *arg)
{
	uint64_t x = fsimg_load_le64(a), y = fsimg_load_le64(b);

	(void)alen;
	(void)blen;
	(void)arg;
	return (x > y ? -1 : x < y);
}

/*
 * Sort the candidates newest first, refusing two with one XID, and select
 * the newest whose run is whole. A newer candidate whose run is not whole
 * is a checkpoint a crash interrupted, so the container is not clean.
 */
static fsimg_status_t
candidates_select(apfs_container_t *container, fsimg_store_t *slots,
    fsimg_stream_t *candidates, uint32_t ring, uint32_t *selected,
    fsimg_error_t *error)
{
	unsigned char raw[CANDIDATE_BYTES];
	fsimg_stream_t *sorted = NULL;
	slot_t slot;
	uint64_t cursor = 0, previous = 0;
	uint32_t index;
	size_t length;
	fsimg_status_t status;
	bool found = false, whole;

	container->clean = true;
	status = fsimg_stream_seal(candidates, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(candidates, candidate_compare, NULL,
		    NULL, &sorted, error);
	while (status == FSIMG_OK) {
		status = fsimg_stream_next(sorted, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status == FSIMG_OK && length != sizeof(raw))
			status = apfs_corrupt(error, ROLE_RING,
			    "candidate record is malformed");
		if (status != FSIMG_OK)
			break;
		index = fsimg_load_le32(raw + 8);
		if (previous != 0 && fsimg_load_le64(raw) == previous) {
			status = apfs_corrupt(error, ROLE_RING,
			    "holds two checkpoints with one XID");
			break;
		}
		previous = fsimg_load_le64(raw);
		/* Older candidates are still visited to find ties. */
		if (found)
			continue;
		status = slot_get(slots, index, &slot, error);
		if (status == FSIMG_OK)
			status = run_whole(container, slots, ring, index, &slot,
			    &whole, error);
		if (status != FSIMG_OK)
			break;
		if (whole) {
			found = true;
			*selected = index;
		} else
			container->clean = false;
	}
	fsimg_stream_free(sorted);
	if (status == FSIMG_OK && !found)
		status = apfs_corrupt(error, ROLE_RING,
		    "holds no whole checkpoint");
	return (status);
}

static void
mapping_encode(unsigned char *raw, const apfs_cpm_t *mapping)
{
	fsimg_store_le64(raw, mapping->oid);
	fsimg_store_le64(raw + 8, mapping->address);
	fsimg_store_le64(raw + 16, mapping->volume_oid);
	fsimg_store_le32(raw + 24, mapping->flags | mapping->kind);
	fsimg_store_le32(raw + 28, mapping->subtype);
	fsimg_store_le32(raw + 32, mapping->size);
}

static void
mapping_decode(const unsigned char *raw, apfs_cpm_t *mapping)
{
	uint32_t type = fsimg_load_le32(raw + 24);

	mapping->oid = fsimg_load_le64(raw);
	mapping->address = fsimg_load_le64(raw + 8);
	mapping->volume_oid = fsimg_load_le64(raw + 16);
	mapping->flags = type & APFS_OBJ_FLAGS_MASK;
	mapping->kind = (uint16_t)(type & APFS_OBJ_KIND_MASK);
	mapping->subtype = fsimg_load_le32(raw + 28);
	mapping->size = fsimg_load_le32(raw + 32);
}

static int
mapping_address_compare(const void *a, size_t alen, const void *b,
    size_t blen, void *arg)
{
	uint64_t x = fsimg_load_le64((const unsigned char *)a + 8);
	uint64_t y = fsimg_load_le64((const unsigned char *)b + 8);

	(void)alen;
	(void)blen;
	(void)arg;
	return (x < y ? -1 : x > y);
}

static int
mapping_oid_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *arg)
{
	uint64_t x = fsimg_load_le64(a), y = fsimg_load_le64(b);

	(void)alen;
	(void)blen;
	(void)arg;
	return (x < y ? -1 : x > y);
}

/*
 * Whether blocks [address, address + blocks) lie inside the checkpoint's
 * data run without straddling the data ring's wrap. The run starts at index
 * data_index of the data area and wraps modulo its size, so it is one piece
 * [data_index, data_index + data_len) or two, the second [0, the excess).
 */
static bool
data_run_holds(const apfs_nx_t *nx, uint64_t address, uint64_t blocks)
{
	uint64_t area = nx->data_blocks, first = nx->data_index;
	uint64_t end = first + nx->data_len, index;

	if (address < nx->data_base)
		return (false);
	index = address - nx->data_base;
	if (index >= area || blocks > area - index)
		return (false);
	if (index >= first && index + blocks <= (end < area ? end : area))
		return (true);
	return (end > area && index + blocks <= end - area);
}

/*
 * A truncated image is cut at its tail, so the data run lies in the slice
 * exactly when its highest block does: the last block of its first piece.
 * Reading that block makes a cut through the run fail as truncation even
 * where no mapped object reaches it.
 */
static fsimg_status_t
data_run_probe(apfs_container_t *container, unsigned char *window,
    fsimg_error_t *error)
{
	const apfs_nx_t *nx = &container->nx;
	uint64_t end = (uint64_t)nx->data_index + nx->data_len;

	if (nx->data_len == 0)
		return (FSIMG_OK);
	if (end > nx->data_blocks)
		end = nx->data_blocks;
	return (apfs_block_read(container, nx->data_base + end - 1, window,
	    ROLE_CHECKPOINT, error));
}

/* Append the mappings of one reread mapping block, checking each run. */
static fsimg_status_t
mappings_append(apfs_container_t *container, const apfs_nx_t *nx,
    const unsigned char *window, fsimg_stream_t *stream, fsimg_error_t *error)
{
	unsigned char raw[MAPPING_BYTES];
	apfs_cpm_block_t header;
	apfs_cpm_t mapping;
	fsimg_status_t status;

	if (!apfs_cpm_block_decode(window, container->block_size, &header))
		return (apfs_corrupt(error, ROLE_MAPPING, "is malformed"));
	for (uint32_t i = 0; i < header.count; i++) {
		if (!apfs_cpm_decode(window, container->block_size, &header, i,
		    &mapping))
			return (apfs_corrupt(error, ROLE_MAPPING,
			    "is malformed"));
		if (!data_run_holds(nx, mapping.address,
		    mapping.size / container->block_size))
			return (apfs_corrupt(error, ROLE_MAPPING,
			    "lies outside the checkpoint's data run"));
		mapping_encode(raw, &mapping);
		status = fsimg_stream_append(stream, raw, sizeof(raw), NULL,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* No two mapped block runs overlap: sorted by address, each ends before
 * the next starts.
 */
static fsimg_status_t
mappings_check(apfs_container_t *container, fsimg_stream_t *stream,
    fsimg_error_t *error)
{
	unsigned char raw[MAPPING_BYTES];
	fsimg_stream_t *sorted = NULL;
	apfs_cpm_t mapping;
	uint64_t cursor = 0, end = 0;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_sort(stream, mapping_address_compare, NULL, NULL,
	    &sorted, error);
	while (status == FSIMG_OK) {
		status = fsimg_stream_next(sorted, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status == FSIMG_OK)
			status = fsimg_session_charge(
			    &container->reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			break;
		mapping_decode(raw, &mapping);
		if (mapping.address < end) {
			status = apfs_corrupt(error, ROLE_MAPPING,
			    "overlaps another mapping");
			break;
		}
		/* Inside the data run, so the sum cannot overflow. */
		end = mapping.address + mapping.size / container->block_size;
	}
	fsimg_stream_free(sorted);
	return (status);
}

/* Copy the mappings, sorted by OID, into the table, refusing a repeat. */
static fsimg_status_t
mappings_index(apfs_container_t *container, fsimg_stream_t *stream,
    fsimg_error_t *error)
{
	unsigned char raw[MAPPING_BYTES];
	fsimg_stream_t *sorted = NULL;
	uint64_t cursor = 0, previous = 0, count = 0;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_sort(stream, mapping_oid_compare, NULL, NULL,
	    &sorted, error);
	if (status == FSIMG_OK)
		status = fsimg_store_new(&container->reader->session,
		    &container->mappings, error);
	while (status == FSIMG_OK) {
		status = fsimg_stream_next(sorted, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status != FSIMG_OK)
			break;
		if (count != 0 && fsimg_load_le64(raw) == previous) {
			status = apfs_corrupt(error, ROLE_MAPPING,
			    "maps one ephemeral OID twice");
			break;
		}
		previous = fsimg_load_le64(raw);
		status = fsimg_store_grow(container->mappings,
		    (count + 1) * MAPPING_BYTES, error);
		if (status == FSIMG_OK)
			status = fsimg_store_write(container->mappings,
			    count * MAPPING_BYTES, raw, sizeof(raw), error);
		if (status == FSIMG_OK)
			count++;
	}
	fsimg_stream_free(sorted);
	container->mapping_count = count;
	return (status);
}

fsimg_status_t
apfs_ephemeral_find(apfs_container_t *container, uint64_t oid, bool *found,
    apfs_cpm_t *mapping, fsimg_error_t *error)
{
	unsigned char raw[MAPPING_BYTES];
	uint64_t low = 0, high = container->mapping_count, middle, key;
	fsimg_status_t status;

	*found = false;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = fsimg_store_read(container->mappings,
		    middle * MAPPING_BYTES, raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
		key = fsimg_load_le64(raw);
		if (key == oid) {
			mapping_decode(raw, mapping);
			*found = true;
			return (FSIMG_OK);
		}
		if (key < oid)
			low = middle + 1;
		else
			high = middle;
	}
	return (FSIMG_OK);
}

/*
 * Read every mapped object once; apfs_object_read() checks it. Only the
 * space manager spans several blocks; the size its geometry implies is
 * checked by its own reader, and the data run bounds it here.
 */
static fsimg_status_t
mapped_objects_read(apfs_container_t *container, unsigned char *window,
    fsimg_error_t *error)
{
	unsigned char raw[MAPPING_BYTES];
	apfs_cpm_t mapping;
	apfs_ref_t ref;
	fsimg_status_t status;

	for (uint64_t i = 0; i < container->mapping_count; i++) {
		status = fsimg_store_read(container->mappings,
		    i * MAPPING_BYTES, raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
		mapping_decode(raw, &mapping);
		ref = (apfs_ref_t){ .role = "checkpoint object",
		    .oid = mapping.oid, .xid = container->xid,
		    .blocks = mapping.kind == APFS_KIND_SPACEMAN ?
		    mapping.size / container->block_size : 1,
		    .storage = APFS_OBJ_EPHEMERAL, .subtype = mapping.subtype,
		    .kind = mapping.kind };
		status = apfs_object_read(container, &ref, window, NULL, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Load the selected checkpoint: read its superblock and mapping blocks
 * again, which must classify as they did, build and check the mapping
 * table, and read every mapped object.
 */
static fsimg_status_t
checkpoint_load(apfs_container_t *container, const apfs_nx_t *block0,
    fsimg_store_t *slots, uint32_t selected, unsigned char *window,
    fsimg_error_t *error)
{
	fsimg_stream_t *stream = NULL;
	apfs_cpm_t mapping;
	slot_t recorded, again, map;
	apfs_ref_t ref;
	apfs_nx_t nx;
	uint32_t ring = block0->desc_blocks, slot;
	fsimg_status_t status;
	bool found;

	status = slot_get(slots, selected, &recorded, error);
	if (status == FSIMG_OK)
		status = apfs_block_read(container,
		    block0->desc_base + selected, window, ROLE_RING, error);
	if (status == FSIMG_OK)
		status = slot_classify(block0, block0->desc_base + selected,
		    window, &again, &nx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!slot_same(&again, &recorded))
		return (apfs_corrupt(error, ROLE_CHECKPOINT,
		    "changed while read"));

	container->nx = nx;
	container->xid = recorded.xid;
	container->block_count = nx.block_count;

	status = fsimg_stream_new(&container->reader->session, MAPPING_BYTES,
	    &stream, error);
	for (uint32_t j = 0; status == FSIMG_OK && j + 1 < nx.desc_len; j++) {
		slot = (uint32_t)(((uint64_t)nx.desc_index + j) % ring);
		status = slot_get(slots, slot, &map, error);
		if (status == FSIMG_OK)
			status = apfs_block_read(container,
			    block0->desc_base + slot, window, ROLE_RING, error);
		if (status == FSIMG_OK)
			status = slot_classify(block0, block0->desc_base + slot,
			    window, &again, NULL, error);
		if (status != FSIMG_OK)
			break;
		if (!slot_same(&again, &map)) {
			status = apfs_corrupt(error, ROLE_MAPPING,
			    "changed while read");
			break;
		}
		status = mappings_append(container, &nx, window, stream, error);
	}
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(stream, error);
	if (status == FSIMG_OK)
		status = mappings_check(container, stream, error);
	if (status == FSIMG_OK)
		status = mappings_index(container, stream, error);
	fsimg_stream_free(stream);
	if (status != FSIMG_OK)
		return (status);

	status = apfs_ephemeral_find(container, nx.spaceman_oid, &found,
	    &mapping, error);
	if (status == FSIMG_OK && (!found ||
	    mapping.kind != APFS_KIND_SPACEMAN))
		status = apfs_corrupt(error, ROLE_CHECKPOINT,
		    "does not map its space manager");
	if (status == FSIMG_OK)
		status = apfs_ephemeral_find(container, nx.reaper_oid, &found,
		    &mapping, error);
	if (status == FSIMG_OK && (!found ||
	    mapping.kind != APFS_KIND_NX_REAPER))
		status = apfs_corrupt(error, ROLE_CHECKPOINT,
		    "does not map its reaper");
	if (status == FSIMG_OK)
		status = mapped_objects_read(container, window, error);
	if (status == FSIMG_OK)
		status = data_run_probe(container, window, error);
	if (status == FSIMG_OK) {
		ref = (apfs_ref_t){ .role = "container object map",
		    .oid = nx.omap_oid, .xid = container->xid,
		    .storage = APFS_OBJ_PHYSICAL, .subtype = APFS_SUBTYPE_NONE,
		    .kind = APFS_KIND_OMAP };
		status = apfs_object_read(container, &ref, window, NULL, error);
	}
	return (status);
}

fsimg_status_t
apfs_checkpoint_open(fsimg_reader_t *reader, const apfs_obj_t *header,
    const apfs_nx_t *block0, apfs_container_t *container, fsimg_error_t *error)
{
	fsimg_store_t *slots = NULL;
	fsimg_stream_t *candidates = NULL;
	unsigned char *window;
	fsimg_error_t detail;
	uint32_t selected = 0;
	fsimg_status_t status;

	*container = (apfs_container_t){ .reader = reader,
	    .block_size = block0->block_size,
	    .block_count = block0->block_count };
	status = apfs_nx_geometry(block0, "container superblock", error);
	if (status != FSIMG_OK)
		return (status);
	window = fsimg_alloc(reader->ctx, block0->block_size, &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}

	status = fsimg_store_new(&reader->session, &slots, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, CANDIDATE_BYTES,
		    &candidates, error);
	if (status == FSIMG_OK)
		status = ring_scan(container, block0, window, slots, candidates,
		    error);
	if (status == FSIMG_OK)
		status = candidates_select(container, slots, candidates,
		    block0->desc_blocks, &selected, error);
	if (status == FSIMG_OK)
		status = checkpoint_load(container, block0, slots, selected,
		    window, error);
	if (status == FSIMG_OK && header->xid > container->xid)
		status = apfs_corrupt(error, "container superblock",
		    "is newer than every whole checkpoint");

	fsimg_stream_free(candidates);
	fsimg_store_free(slots);
	fsimg_dealloc(window);
	if (status != FSIMG_OK)
		apfs_container_close(container);
	return (status);
}

void
apfs_container_close(apfs_container_t *container)
{
	fsimg_store_free(container->mappings);
	container->mappings = NULL;
	container->mapping_count = 0;
}

/*
 * The reaper's list is well formed in one of two shapes: head, tail and
 * object count all zero, or exactly count reap-list objects of the
 * checkpoint from head, the last with next zero and OID equal to tail.
 * The walk reads at most count objects, and every list object is a
 * distinct mapped object, so a count above the mapping count is refused
 * before any is read. A list that cycles cannot end with a next of zero,
 * so it fails at its last object.
 */
fsimg_status_t
apfs_reaper_idle(apfs_container_t *container, unsigned char *window,
    bool *idle, fsimg_error_t *error)
{
	apfs_reaper_t reaper;
	apfs_reap_list_t list;
	apfs_ref_t ref;
	uint64_t oid;
	fsimg_status_t status;
	bool empty = true;

	*idle = false;
	ref = (apfs_ref_t){ .role = ROLE_REAPER, .oid = container->nx.reaper_oid,
	    .xid = container->xid, .storage = APFS_OBJ_EPHEMERAL,
	    .subtype = APFS_SUBTYPE_NONE, .kind = APFS_KIND_NX_REAPER };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_reaper_decode(window, container->block_size, &reaper))
		return (apfs_corrupt(error, ROLE_REAPER, "is malformed"));

	if (reaper.head == 0 && reaper.tail == 0 && reaper.rlcount == 0) {
		*idle = (reaper.flags & APFS_REAPER_FLAG_CONTINUE) == 0;
		return (FSIMG_OK);
	}
	if (reaper.head == 0 || reaper.rlcount == 0 ||
	    reaper.rlcount > container->mapping_count)
		return (apfs_corrupt(error, ROLE_REAP_LIST, "is malformed"));

	oid = reaper.head;
	for (uint32_t i = 0; i < reaper.rlcount; i++) {
		if (oid == 0)
			return (apfs_corrupt(error, ROLE_REAP_LIST,
			    "ends before its count"));
		ref = (apfs_ref_t){ .role = ROLE_REAP_LIST, .oid = oid,
		    .xid = container->xid, .storage = APFS_OBJ_EPHEMERAL,
		    .subtype = APFS_SUBTYPE_NONE,
		    .kind = APFS_KIND_NX_REAP_LIST };
		status = apfs_object_read(container, &ref, window, NULL,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (!apfs_reap_list_decode(window, container->block_size,
		    &list))
			return (apfs_corrupt(error, ROLE_REAP_LIST,
			    "is malformed"));
		if (list.count != 0)
			empty = false;
		if (i + 1 == reaper.rlcount &&
		    (list.next != 0 || oid != reaper.tail))
			return (apfs_corrupt(error, ROLE_REAP_LIST,
			    "does not end at its tail"));
		oid = list.next;
	}
	*idle = empty && (reaper.flags & APFS_REAPER_FLAG_CONTINUE) == 0;
	return (FSIMG_OK);
}
