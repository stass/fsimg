/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg.h"
#include "test.h"
#include "apfs-native.h"

#define BLOCK_BYTES UINT64_C(4096)

/* Caps bound work on corrupt captures, independently of writer formulas. */
#define NATIVE_RING_CAP 65536
#define NATIVE_MAP_CAP 1024
#define NODE_ROOT 1
#define NODE_LEAF 2
#define NODE_FIXED 4

/* Object storage classes, kinds and tree subtypes from the disk format. */
#define OBJECT_NX 0x80000001u
#define OBJECT_MAPPING 0x4000000cu
#define OBJECT_SPACEMAN 0x80000005u
#define OBJECT_REAPER 0x80000011u
#define OBJECT_OMAP 0x4000000bu
#define OBJECT_PHYSICAL_ROOT 0x40000002u
#define OBJECT_PHYSICAL_NODE 0x40000003u
#define OBJECT_VIRTUAL_ROOT 2
#define OBJECT_VOLUME 13
#define OBJECT_CIB 0x40000007u
#define TREE_FREE_QUEUE 9
#define TREE_OMAP 11
#define TREE_FILESYSTEM 14
#define TREE_EXTENTS 15
#define TREE_SNAPSHOTS 16

typedef enum native_disk_offset {
	OBJECT_OID = 8,
	OBJECT_XID = 16,
	OBJECT_TYPE = 24,
	OBJECT_SUBTYPE = 28,
	NODE_FLAGS = 32,
	NODE_LEVEL = 34,
	NODE_RECORD_COUNT = 36,
	NODE_TOC_OFFSET = 40,
	NODE_TOC_LENGTH = 42,
	NODE_HEADER_END = 56,
	NODE_FOOTER = 4056,
	NX_DESC_BLOCKS = 104,
	NX_DATA_BLOCKS = 108,
	NX_DESC_BASE = 112,
	NX_DATA_BASE = 120,
	NX_DESC_NEXT = 128,
	NX_DATA_NEXT = 132,
	NX_DESC_INDEX = 136,
	NX_DESC_COUNT = 140,
	NX_DATA_INDEX = 144,
	NX_DATA_COUNT = 148,
	SM_CIB_ARRAY = 80,
	SM_BITMAP_XIDS = 324,
	SM_BITMAP_INDEXES = 328,
	SM_BITMAP_FREE_NEXT = 332,
	VOLUME_ROOT_TYPE = 116,
	VOLUME_EXTENT_TYPE = 120,
	VOLUME_SNAPSHOT_TYPE = 124,
	VOLUME_SEALED_TYPE = 1040,
	VOLUME_RESERVED_TYPE = 1044
} native_disk_offset_t;

static const char row_tree_shape[] =
	"Table-of-contents capacity, key-area start, alignment, tree flags, "
	    "and the footer's node size and fixed key and value sizes";

static const char row_inode_state[] =
	"Internal flags, protection class, write generation";

static const char row_sparse_records[] =
	"Directory statistics records and their inode field, sibling "
	    "records, document IDs, purgeable and firmlink fields";

static const char row_omap_snapshots[] =
	"Object-map snapshot count, latest-snapshot XID, snapshot tree OID and "
	    "pending revert range";

static const char row_sm_state[] =
	"Reserve counts, flags, version, structure size";

static const char row_container_absent[] =
	"Fusion fields, keylocker ranges, EFI jumpstart, test fields";

static const char row_volume_transitions[] =
	"Volume revert-to, root-to and encryption-rolling fields";

static const char row_volume_tail[] =
	"Fields past [A1]'s layout, from offset 1056";

static const char row_allocated[] =
	"Allocated block count, total blocks allocated and freed";

static const char row_document_id[] =
	"Next document ID, clone-information epoch and XID";

static const char *scenario;
static unsigned failures;
static unsigned capture_fields[3];
static unsigned construction_fields, exempt_fields;
static unsigned comparison_xid;
static uint64_t first_form_xid, second_form_xid;
static jmp_buf malformed_capture;

#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, \
		    scenario, #condition); \
		failures++; \
	} \
} while (0)

/* The capture boundary owns descriptors and cached mappings on failure. */
#define REQUIRE(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL malformed %s:%d: %s: %s\n", \
		    __FILE__, __LINE__, scenario, #condition); \
		failures++; \
		longjmp(malformed_capture, 1); \
	} \
} while (0)

/*
 * Empty-container oracle. Only the public writer fixture is shared with
 * apfs-write.c: all disk decoding below uses literal offsets from the
 * format specification, including its own Fletcher checksum and B-tree
 * traversal. Images are never mapped into memory or attached to the OS.
 * A freshly truncated, unlinked file supplies sparse zeroes for the writer.
 */
typedef struct native_image {
	int fd;
	uint64_t blocks;
	uint64_t xid;
	uint64_t checkpoint;
	uint64_t wanted_xid;
	unsigned char (*mapping_blocks)[4096];
	size_t mapping_count;
	unsigned char nx[4096];
	unsigned char sm[4096];
	unsigned char reaper[4096];
	unsigned char volume[4096];
	unsigned char maps[2][4096];
	unsigned char trees[5][4096];
} native_image_t;

typedef struct native_record {
	const unsigned char *key;
	const unsigned char *value;
	size_t key_length;
	size_t value_length;
} native_record_t;

static void native_exempt(const char *, const char *, const unsigned char *,
    const unsigned char *, size_t);
static void native_test_capture_continuation(void);
static uint64_t native_uint(const unsigned char *, size_t);
static bool native_checksum(const unsigned char *, size_t);
static bool native_read(const native_image_t *, uint64_t, unsigned char *);
static bool native_object(const native_image_t *, uint64_t, uint32_t,
    uint32_t, uint64_t, unsigned char *);
static bool native_record(const unsigned char *, uint32_t, native_record_t *);
static bool native_checkpoint_whole(const native_image_t *,
    const unsigned char *, uint64_t);
static bool native_select(native_image_t *);
static bool native_ephemeral(native_image_t *, uint64_t, uint32_t,
    uint32_t, unsigned char *);
static bool native_lookup(const native_image_t *, uint64_t, uint64_t,
    unsigned, uint64_t *);
static bool native_decode(native_image_t *);
static bool native_validate_mappings(const native_image_t *);
static bool native_spaceman_arrays(const native_image_t *);
static void native_equal(const char *, const char *, uint64_t, uint64_t);
static void expect_row(const char *, const char *, uint64_t, uint64_t,
    uint64_t);
static void native_range(const char *, const char *, const unsigned char *,
    const unsigned char *, size_t, size_t);
static void expect_writer_zero(const char *, const char *,
    const unsigned char *, const unsigned char *, size_t, size_t);

static uint64_t
native_uint(const unsigned char *bytes, size_t length)
{
	uint64_t value = 0;

	REQUIRE(length <= 8);
	for (size_t i = 0; i < length; i++)
		value |= (uint64_t)bytes[i] << (8 * i);
	return (value);
}

#define N16(p, o) native_uint((p) + (o), 2)
#define N32(p, o) native_uint((p) + (o), 4)
#define N64(p, o) native_uint((p) + (o), 8)

static bool
native_checksum(const unsigned char *bytes, size_t length)
{
	const uint64_t modulus = UINT32_MAX;
	uint64_t first = 0, second = 0, low, high;

	for (size_t i = 8; i < length; i += 4) {
		first = (first + N32(bytes, i)) % modulus;
		second = (second + first) % modulus;
	}
	low = modulus - (first + second) % modulus;
	high = modulus - (first + low) % modulus;
	return (N64(bytes, 0) == (low | high << 32));
}

static bool
native_read(const native_image_t *image, uint64_t block, unsigned char *bytes)
{
	size_t done = 0;
	ssize_t count;

	if (block >= image->blocks)
		return (false);
	while (done < 4096) {
		count = pread(image->fd, bytes + done, 4096 - done,
		    (off_t)(block * 4096 + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return (false);
		done += (size_t)count;
	}
	return (true);
}

static bool
native_object(const native_image_t *image, uint64_t block, uint32_t type,
    uint32_t subtype, uint64_t oid, unsigned char *bytes)
{
	if (!native_read(image, block, bytes))
		return (false);
	return (native_checksum(bytes, 4096) && N32(bytes, 24) == type &&
	    N32(bytes, 28) == subtype && N64(bytes, OBJECT_OID) == oid &&
	    N64(bytes, OBJECT_XID) != 0 &&
	    N64(bytes, OBJECT_XID) <= image->xid);
}

/* These captures have small trees. The traversal also supports index
 * nodes, with the format's height bound preventing cycles from hanging it.
 * Each record's spans are checked before pointers into it are exposed.
 */
static bool
native_record(const unsigned char *node, uint32_t index,
    native_record_t *record)
{
	uint64_t flags = N16(node, NODE_FLAGS), count = N32(node,
	    NODE_RECORD_COUNT);
	size_t table = NODE_HEADER_END + N16(node, NODE_TOC_OFFSET);
	size_t table_bytes = N16(node, NODE_TOC_LENGTH);
	size_t end = (flags & NODE_ROOT) != 0 ? NODE_FOOTER : 4096;
	size_t stride = (flags & NODE_FIXED) != 0 ? 4 : 8;
	size_t key, value, key_length, value_length, slot;

	if (index >= count || table > end || table_bytes > end - table ||
	    count > table_bytes / stride)
		return (false);
	slot = table + stride * index;
	key = table + table_bytes + N16(node, slot);
	if ((flags & NODE_FIXED) != 0) {
		/* Object maps have 16-byte values; free queues have 8. */
		key_length = 16;
		value_length = (flags & NODE_LEAF) != 0 && N32(node,
		    OBJECT_SUBTYPE) != TREE_FREE_QUEUE ? 16 : 8;
		value = N16(node, slot + 2);
	} else {
		key_length = N16(node, slot + 2);
		value = N16(node, slot + 4);
		value_length = N16(node, slot + 6);
	}
	if (value == 65535 && N32(node, OBJECT_SUBTYPE) == TREE_FREE_QUEUE &&
	    (flags & NODE_LEAF) != 0) {
		value = 0;
		value_length = 0;
	}
	if (key_length < 8 || key > end ||
	    key_length > end - key || value > end)
		return (false);
	value = end - value;
	if (value_length > end - value || key + key_length > value)
		return (false);
	/* Consumers may read a filesystem key word, a directory value's
	 * first 18 bytes, or an inode's 8-byte key and fixed 92-byte body.
	 * Object maps and free queues have exact fixed-size records,
	 * including ghosts.
	 */
	if (N32(node, OBJECT_SUBTYPE) == TREE_OMAP &&
	    ((flags & NODE_FIXED) == 0 || key_length != 16 ||
	    value_length != ((flags & NODE_LEAF) != 0 ? 16u : 8u)))
		return (false);
	if (N32(node, OBJECT_SUBTYPE) == TREE_FREE_QUEUE && (key_length != 16 ||
	    (value_length != 0 && value_length != 8)))
		return (false);
	if (N32(node, OBJECT_SUBTYPE) == TREE_FILESYSTEM && (flags & NODE_LEAF)
	    != 0 &&
	    ((N64(node + key, 0) >> 60 == 3 &&
	    (key_length != 8 || value_length < 92)) ||
	    (N64(node + key, 0) >> 60 == 9 && value_length < 18)))
		return (false);
	record->key = node + key;

	record->value = node + value;
	record->key_length = key_length;
	record->value_length = value_length;
	return (true);
}

static bool
native_checkpoint_whole(const native_image_t *image,
    const unsigned char *nx, uint64_t slot)
{
	unsigned char map[4096];
	uint64_t count = N32(nx, NX_DESC_COUNT), ring = N32(nx, NX_DESC_BLOCKS);
	uint64_t first = N32(nx, NX_DESC_INDEX), xid = N64(nx, 16);
	uint64_t data = N32(nx, NX_DATA_BLOCKS), base = N64(nx, NX_DATA_BASE);

	if (ring == 0 || first >= ring || count < 2 || count > ring || count >
	    NATIVE_MAP_CAP + 1 ||
	    (first + count - 1) % ring != slot || data == 0 ||
	    base >= image->blocks || data > image->blocks - base ||
	    data > NATIVE_RING_CAP ||
	    N32(nx, NX_DATA_INDEX) >= data || N32(nx, NX_DATA_COUNT) > data ||
	    N32(nx, NX_DESC_NEXT) >= ring || N32(nx, NX_DATA_NEXT) >= data ||
	    N64(nx, 96) <= xid)
		return (false);
	for (uint64_t i = 0; i < count - 1; i++) {
		uint64_t block = N64(nx, NX_DESC_BASE) + (first + i) % ring;

		if (!native_read(image, block, map))
			return (false);
		if (!native_checksum(map, 4096) || N64(map, 8) != block ||
		    N64(map, 16) != xid || N32(map, 24) != OBJECT_MAPPING ||
		    N32(map, 28) != 0 || N32(map, 36) > 101 ||
		    N32(map, 32) != (i == count - 2 ? 1u : 0u))
			return (false);
	}
	return (true);
}

static bool
native_select(native_image_t *image)
{
	unsigned char block0[4096], candidate[4096];
	uint64_t base, count, newest = 0;
	if (!native_read(image, 0, block0) ||
	    !native_checksum(block0, 4096) || N32(block0, 32) != 0x4253584e ||
	    N32(block0, 36) != 4096 || N64(block0, 40) != image->blocks)
		return (false);
	base = N64(block0, NX_DESC_BASE);
	count = N32(block0, NX_DESC_BLOCKS);
	if (base == 0 || base >= image->blocks || count == 0 ||
	    count > image->blocks - base || count > NATIVE_RING_CAP)
		return (false);
	for (uint64_t slot = 0; slot < count; slot++) {
		if (!native_read(image, base + slot, candidate))
			return (false);
		if (!native_checksum(candidate, 4096) ||
		    N32(candidate, 32) != 0x4253584e ||
		    N32(candidate, 24) != OBJECT_NX ||
		    N32(candidate, 28) != 0 ||
		    N64(candidate, OBJECT_OID) != 1 ||
		    N32(candidate, 36) != 4096 ||
		    N64(candidate, 40) != image->blocks ||
		    N64(candidate, NX_DESC_BASE) != base || N32(candidate,
		    NX_DESC_BLOCKS) != count ||
		    bcmp(candidate + 72, block0 + 72, 16) != 0)
			continue;
		if (image->wanted_xid != 0 &&
		    N64(candidate, OBJECT_XID) != image->wanted_xid)
			continue;
		if (!native_checkpoint_whole(image, candidate, slot))
			continue;
		if (N64(candidate, OBJECT_XID) == newest)
			return (false);
		if (N64(candidate, OBJECT_XID) < newest)
			continue;
		newest = N64(candidate, OBJECT_XID);
		image->checkpoint = base + slot;
		bcopy(candidate, image->nx, 4096);
	}
	if (newest == 0)
		return (false);
	image->xid = newest;
	image->mapping_count = (size_t)N32(image->nx, NX_DESC_COUNT) - 1;
	image->mapping_blocks = calloc(image->mapping_count, 4096);
	if (image->mapping_blocks == NULL)
		return (false);
	for (size_t i = 0; i < image->mapping_count; i++) {
		uint64_t block = base + (N32(image->nx, NX_DESC_INDEX) + i) %
		    count;
		unsigned char *map = image->mapping_blocks[i];

		if (!native_read(image, block, map) ||
		    !native_checksum(map, 4096) || N64(map, 8) != block ||
		    N64(map, 16) != newest || N32(map, 24) != OBJECT_MAPPING ||
		    N32(map, 28) != 0 || N32(map, 36) > 101 ||
		    N32(map, 32) != (i + 1 == image->mapping_count ? 1u : 0u))
			return (false);
	}
	return (true);

}

static bool
native_ephemeral(native_image_t *image, uint64_t oid, uint32_t type,
    uint32_t subtype, unsigned char *bytes)
{

	uint64_t address = 0;

	for (size_t i = 0; i < image->mapping_count; i++) {
		const unsigned char *map = image->mapping_blocks[i];

		for (uint64_t j = 0; j < N32(map, 36); j++) {
			const unsigned char *entry = map + 40 + 40 * j;
			uint64_t physical = N64(entry, 32);
			uint64_t data = N64(image->nx, NX_DATA_BASE);
			uint64_t index, data_count = N32(image->nx,
			    NX_DATA_BLOCKS);

			if (N64(entry, 24) != oid)
				continue;
			/* These captures use one-block objects. Refuse a
			 * larger mapping rather than truncate its check.
			 */
			if (address != 0 || N32(entry, 0) != type ||
			    N32(entry, 4) != subtype || N32(entry, 8) != 4096 ||
			    N32(entry, 12) != 0 || N64(entry, 16) != 0 ||
			    physical < data || physical - data >= data_count)
				return (false);
			index = (physical - data + data_count -
			    N32(image->nx, NX_DATA_INDEX)) % data_count;
			if (index >= N32(image->nx, NX_DATA_COUNT))
				return (false);
			address = physical;
		}
	}
	return (address != 0 && native_object(image, address, type, subtype,
	    oid,
	    bytes));
}

static bool
native_omap_shape(const unsigned char *node, unsigned depth)
{
	uint64_t flags = N16(node, NODE_FLAGS), level = N16(node, NODE_LEVEL);

	return (level < 16 && (flags & NODE_FIXED) != 0 &&
	    (flags & ~(NODE_FIXED | NODE_ROOT | NODE_LEAF)) == 0 &&
	    ((flags & NODE_ROOT) != 0) == (depth == 0) &&
	    ((flags & NODE_LEAF) != 0) == (level == 0) &&
	    N32(node, OBJECT_TYPE) == (depth == 0 ?
	    OBJECT_PHYSICAL_ROOT : OBJECT_PHYSICAL_NODE));
}

static bool
native_lookup(const native_image_t *image, uint64_t block, uint64_t oid,
    unsigned depth, uint64_t *address)
{
	unsigned char node[4096];
	native_record_t record;
	uint64_t best_xid = 0, child = 0;

	if (depth >= 16 || !native_read(image, block, node))
		return (false);
	if (!native_checksum(node, 4096) || N64(node, OBJECT_OID) != block ||
	    (N32(node, OBJECT_TYPE) != OBJECT_PHYSICAL_ROOT &&
	    N32(node, OBJECT_TYPE) != OBJECT_PHYSICAL_NODE) || N32(node,
	    OBJECT_SUBTYPE) != TREE_OMAP ||
	    N64(node, OBJECT_XID) == 0 || N64(node, OBJECT_XID) > image->xid ||
	    N32(node, NODE_RECORD_COUNT) > 1000)
		return (false);
	if (!native_omap_shape(node, depth))
		return (false);
	for (uint32_t i = 0; i < N32(node, NODE_RECORD_COUNT); i++) {

		uint64_t key_oid, key_xid;

		if (!native_record(node, i, &record))
			return (false);
		key_oid = N64(record.key, 0);
		key_xid = N64(record.key, 8);
		if ((N16(node, NODE_FLAGS) & NODE_LEAF) == 0) {
			if (key_oid < oid || (key_oid == oid &&
			    key_xid <= image->xid))
				child = N64(record.value, 0);
			continue;
		}
		if (key_oid != oid || key_xid > image->xid || key_xid <
		    best_xid)
			continue;
		best_xid = key_xid;
		if ((N32(record.value, 0) & ~UINT64_C(2)) != 0 ||
		    N32(record.value, 4) != 4096)
			return (false);
		*address = N64(record.value, 8);
	}
	if ((N16(node, NODE_FLAGS) & NODE_LEAF) != 0)
		return (best_xid != 0 && *address < image->blocks);
	return (child != 0 && native_lookup(image, child, oid, depth + 1,
	    address));
}

/* Validate every mapped object, including nonempty native free queues.
 * The bitmap below records occupied data slots; the mapping's OID is
 * checked independently for uniqueness against earlier map entries.
 */
static bool
native_validate_mappings(const native_image_t *image)
{
	unsigned char object[4096];
	uint64_t data = N64(image->nx, NX_DATA_BASE), slots = N32(image->nx,
	    NX_DATA_BLOCKS);
	uint64_t mappings = 0;
	uint64_t *oids;
	unsigned char *occupied;
	bool valid = false;

	/* Each distinct mapping occupies at least one data block. */
	REQUIRE(slots <= SIZE_MAX / sizeof(*oids));
	oids = calloc((size_t)slots, sizeof(*oids));
	occupied = calloc((size_t)slots, 1);
	if (oids == NULL || occupied == NULL)
		goto out;
	for (size_t i = 0; i < image->mapping_count; i++) {
		const unsigned char *map = image->mapping_blocks[i];

		for (uint64_t j = 0; j < N32(map, 36); j++) {
			const unsigned char *entry = map + 40 + 40 * j;
			uint64_t address = N64(entry, 32), oid = N64(entry, 24);
			uint64_t index;

			if (N32(entry, 8) != 4096 || N32(entry, 12) != 0 ||
			    N64(entry, 16) != 0 || address < data ||
			    address - data >= slots || mappings >= slots ||
			    (N32(entry, 0) & 0xffff0000) != 0x80000000)
				goto out;
			index = address - data;
			if (occupied[index] != 0 ||
			    (index + slots - N32(image->nx, NX_DATA_INDEX)) %
			    slots >=
			    N32(image->nx, NX_DATA_COUNT))
				goto out;
			occupied[index] = 1;
			for (uint64_t k = 0; k < mappings; k++)
				if (oids[k] == oid)
					goto out;
			oids[mappings++] = oid;
			if (!native_object(image, address, (uint32_t)N32(entry,
			    0),
			    (uint32_t)N32(entry, 4), oid, object))
				goto out;
		}
	}
	valid = true;
out:
	free(occupied);
	free(oids);
	return (valid);
}

/* These fixed-size captures need no CAB or multi-block space manager.
 * Check every offset before later comparisons index an embedded array.
 */
static bool
native_spaceman_arrays(const native_image_t *image)
{
	const unsigned char *sm = image->sm;
	uint64_t cibs = N32(sm, 64), b = N32(sm, 160);
	uint64_t offsets[] = { N32(sm, 80), N32(sm, 128),
	    N32(sm, 324), N32(sm, 328), N32(sm, 332) };
	uint64_t lengths[] = { 8 * cibs, 0, 8 * b, 2 * b,
	    2 * N32(sm, 164) };

	if (cibs > 507 || b > 128 || N32(sm, 164) > 2048 ||
	    N32(sm, 68) != 0)
		return (false);
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
		if (offsets[i] > 4096 ||
		    lengths[i] > 4096 - offsets[i])
			return (false);
	return (true);
}

static bool
native_decode(native_image_t *image)
{
	uint64_t block;

	if (!native_select(image)) {
		printf("DECODE failed: XID %llu descriptor checkpoint or "
		    "mapping "
		    "blocks missing, corrupt or outside safety caps\n",
		    (unsigned long long)image->wanted_xid);
		return (false);
	}
	if (!native_validate_mappings(image)) {
		printf("DECODE failed: XID %llu mapped data objects not "
		    "intact\n",
		    (unsigned long long)image->xid);
		return (false);
	}
	if (!native_ephemeral(image, N64(image->nx, 152), OBJECT_SPACEMAN, 0,
	    image->sm))
		return (false);
	if (!native_spaceman_arrays(image))
		return (false);
	if (!native_ephemeral(image, N64(image->nx, 168), OBJECT_REAPER, 0,
	    image->reaper))
		return (false);
	block = N64(image->nx, 160);
	if (!native_object(image, block, OBJECT_OMAP, 0, block, image->maps[0]))
		return (false);
	block = N64(image->maps[0], 48);
	if (!native_object(image, block, OBJECT_PHYSICAL_ROOT, TREE_OMAP,
	    block, image->trees[0]))
		return (false);
	if (!native_omap_shape(image->trees[0], 0))
		return (false);
	if (N64(image->nx, 184) == 0)
		return (true);
	if (!native_lookup(image, block, N64(image->nx, 184), 0, &block))
		return (false);
	if (!native_object(image, block, OBJECT_VOLUME, 0, N64(image->nx, 184),
	    image->volume) || N32(image->volume, 32) != 0x42535041)
		return (false);
	block = N64(image->volume, 128);
	if (!native_object(image, block, OBJECT_OMAP, 0, block, image->maps[1]))
		return (false);
	block = N64(image->maps[1], 48);
	if (!native_object(image, block, OBJECT_PHYSICAL_ROOT, TREE_OMAP,
	    block, image->trees[1]))
		return (false);
	if (!native_lookup(image, block, N64(image->volume, 136), 0, &block))
		return (false);
	if (!native_object(image, block, OBJECT_VIRTUAL_ROOT, TREE_FILESYSTEM,
	    N64(image->volume, 136),
	    image->trees[2]))
		return (false);
	block = N64(image->volume, 144);
	if (!native_object(image, block, OBJECT_PHYSICAL_ROOT, TREE_EXTENTS,
	    block, image->trees[3]))
		return (false);
	block = N64(image->volume, 152);
	return (native_object(image, block, OBJECT_PHYSICAL_ROOT,
	    TREE_SNAPSHOTS, block,
	    image->trees[4]));
}

/* Both values are always printed. A policy exemption still asserts its
 * exact writer construction; it is never permission to ignore a mismatch.
 */
static void
native_equal(const char *field, const char *row, uint64_t writer,
    uint64_t native)
{
	capture_fields[comparison_xid]++;
	printf("%s %s: writer=%llu native=%llu [row: %s; XID %u]\n",
	    writer == native ? "COMPARE" : "FAIL unexplained", field,
	    (unsigned long long)writer, (unsigned long long)native, row,
	    comparison_xid);
	if (writer != native)
		failures++;
}

typedef enum native_source {
	MATCH_CAPTURE,
	MATCH_CONSTRUCTION
} native_source_t;

/* Catalog-dependent formulas and policy are explicit here. In particular,
 * E1's checkpoint and pool rows must never fall back to literal assertions.
 * Mixed rows have separate named entries for their policy clauses.
 */
static native_source_t
native_row_source(const char *row)
{
	static const struct {
		const char *row;
		native_source_t source;
	} rows[] = {
		{ "Object XID X", MATCH_CAPTURE },
		{ "Descriptor ring", MATCH_CAPTURE },
		{ "Data ring", MATCH_CAPTURE },
		{ "Mapping block", MATCH_CAPTURE },
		{ "Internal-pool ring fields", MATCH_CAPTURE },
		{ "Internal-pool blocks", MATCH_CAPTURE },
		{ "Newest mounted version", MATCH_CAPTURE },
		{ "Free queues", MATCH_CAPTURE },
		{ "Container superblock OID", MATCH_CAPTURE },
		{ "Ephemeral and virtual OIDs (ephemeral objects)",
		    MATCH_CAPTURE },
		{ "Volume next object identifier", MATCH_CAPTURE },
		{ "Container next XID", MATCH_CAPTURE },
		{ "Container next OID", MATCH_CONSTRUCTION },
		{ "OID of a physical object", MATCH_CONSTRUCTION },
		{ "Ephemeral and virtual OIDs (tree-node policy)",
		    MATCH_CONSTRUCTION },
		{ "Slot, index, role, name", MATCH_CONSTRUCTION },
		{ "Formatting record", MATCH_CONSTRUCTION },
		{ "Footer counts", MATCH_CONSTRUCTION },
		{ "Inode numbers", MATCH_CAPTURE },
		{ "Private ID", MATCH_CAPTURE },
		{ "Directory inode", MATCH_CAPTURE },
		{ "BSD flags", MATCH_CONSTRUCTION },
		{ row_sparse_records, MATCH_CONSTRUCTION },
		{ "Default owner and modes", MATCH_CONSTRUCTION },
		{ "Object counts", MATCH_CONSTRUCTION },
		{ row_allocated,
		    MATCH_CONSTRUCTION },
		{ "Free counts", MATCH_CONSTRUCTION },
		{ "Unused bytes", MATCH_CONSTRUCTION },
		{ "Counters", MATCH_CONSTRUCTION },
		{ "Metadata crypto state", MATCH_CONSTRUCTION },
		{ row_volume_tail,
		    MATCH_CONSTRUCTION }
	};

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		if (strcmp(row, rows[i].row) == 0)
			return (rows[i].source);
	REQUIRE(!"comparison row missing from source table");
	return (MATCH_CAPTURE);
}

static void
expect_row(const char *field, const char *row, uint64_t writer,
    uint64_t native, uint64_t expected)
{
	if (native_row_source(row) == MATCH_CAPTURE) {
		native_equal(field, row, writer, native);
		return;
	}
	construction_fields++;
	printf("%s %s: writer=%llu native=%llu construction=%llu [row: %s]\n",
	    writer != expected ? "FAIL construction" :
	    writer != native ? "EXEMPT difference" : "CONSTRUCTION", field,
	    (unsigned long long)writer, (unsigned long long)native,
	    (unsigned long long)expected, row);
	if (writer != native)
		exempt_fields++;
	if (writer != expected)
		failures++;
}

/* Writer construction checks have no native capture operand. */
static void
expect_writer_value(const char *field, const char *row, uint64_t value,
    uint64_t required)
{
	construction_fields++;
	printf("%s %s: writer=%llu required=%llu [row: %s]\n",
	    value == required ? "CONSTRUCTION" : "FAIL construction", field,
	    (unsigned long long)value, (unsigned long long)required, row);
	if (value != required)
		failures++;
}

/* XID-2 volume objects have no first-form counterpart. Assert their
 * transaction shape before comparing the writer's single-transaction XID.
 */
static void
expect_native_value(const char *field, const char *row, uint64_t value,
    uint64_t required)
{
	capture_fields[comparison_xid]++;
	printf("%s %s: native=%llu required=%llu [row: %s; XID %u]\n",
	    value == required ? "VALIDATE" : "FAIL native shape", field,
	    (unsigned long long)value, (unsigned long long)required, row,
	    comparison_xid);
	if (value != required)
		failures++;
}

static void
expect_volume_xid(const char *field, uint64_t writer, uint64_t native)
{
	comparison_xid = 1;
	native_equal(field, "Object XID X", writer, first_form_xid);
	comparison_xid = 2;
	expect_native_value(field, "Object XID X", native, second_form_xid);
	exempt_fields++;
	printf("EXEMPT difference %s: writer=%llu native=%llu; volume added "
	    "in second transaction [row: Object XID X; first and second "
	    "forms]\n",
	    field, (unsigned long long)writer, (unsigned long long)native);
}

/* Byte ranges describe reserved fields or arrays; a differing byte gets
 * its offset in the diagnostic. Matching ranges are printed only once.
 */
static void
native_range(const char *field, const char *row, const unsigned char *writer,
    const unsigned char *native, size_t offset, size_t length)
{
	capture_fields[comparison_xid]++;
	for (size_t i = offset; i < offset + length; i++) {
		if (writer[i] == native[i])
			continue;
		printf("FAIL %s[%zu]: writer=%u native=%u [row: %s; XID %u]\n",
		    field, i, writer[i], native[i], row, comparison_xid);
		failures++;
	}
	printf("COMPARE %s[%zu..%zu] [row: %s; XID %u]\n",
	    field, offset, offset + length, row, comparison_xid);
}

static void
expect_writer_zero(const char *field, const char *row,
    const unsigned char *writer, const unsigned char *native,
    size_t offset, size_t length)
{
	REQUIRE(native_row_source(row) == MATCH_CONSTRUCTION);
	construction_fields++;
	for (size_t i = offset; i < offset + length; i++) {
		if (writer[i] != 0) {
			printf("FAIL construction %s[%zu]=%u [row: %s]\n",
			    field, i, writer[i], row);
			failures++;
		}
	}
	if (bcmp(writer + offset, native + offset, length) != 0)
		native_exempt(field, row, writer + offset, native + offset,
		    length);
	else
		printf("CONSTRUCTION %s[%zu..%zu] zero [row: %s]\n",
		    field, offset, offset + length, row);
}

static void
expect_unused_bytes(const char *field, const unsigned char *writer,
    const unsigned char *native, size_t offset, size_t length)
{
	expect_writer_zero(field, "Unused bytes", writer, native, offset,
	    length);
	native_range(field, "Unused bytes", writer, native, offset, length);
}

static void native_compare_trees(native_image_t *, native_image_t *);
static void native_compare_inodes(native_image_t *, native_image_t *);
static void native_compare_chunks(native_image_t *, native_image_t *);
static void native_compare_objects(native_image_t *, native_image_t *,
    native_image_t *);
static void native_exempt(const char *, const char *, const unsigned char *,
    const unsigned char *, size_t);

static void
native_exempt(const char *field, const char *row, const unsigned char *writer,
    const unsigned char *native, size_t length)
{
	exempt_fields++;
	printf("EXEMPT %s %s: writer=", bcmp(writer, native, length) == 0 ?
	    "equal" : "difference", field);
	for (size_t i = 0; i < length; i++)
		printf("%02x", writer[i]);
	printf(" native=");
	for (size_t i = 0; i < length; i++)
		printf("%02x", native[i]);
	printf(" [row: %s]\n",
	    row);
}

static void
native_compare_objects(native_image_t *writer, native_image_t *first,
    native_image_t *second)
{
	uint64_t run = N64(writer->sm, 176) + N64(writer->sm, 152);
	/* OIDs are checked against placement in the contiguous empty catalog,
	 * independently of the writer's object references. Native physical
	 * objects were already checked against their decoded disk addresses.
	 */
	const struct {
		const char *name;
		const unsigned char *writer;
		const unsigned char *native;
		uint64_t oid;
		const char *rule;
		unsigned xid;
	} objects[] = {
		{ "container", writer->nx, first->nx, 1,
		    "Container superblock OID", 1 },
		{ "space manager", writer->sm, first->sm, 1024,
		    "Ephemeral and virtual OIDs (ephemeral objects)", 1 },
		{ "reaper", writer->reaper, first->reaper, 1025,
		    "Ephemeral and virtual OIDs (ephemeral objects)", 1 },
		{ "volume", writer->volume, second->volume, 1026,
		    "Ephemeral and virtual OIDs (ephemeral objects)", 2 },
		{ "container map", writer->maps[0], first->maps[0], run,
		    "OID of a physical object", 1 },
		{ "volume map", writer->maps[1], second->maps[1], run + 3,
		    "OID of a physical object", 2 },
		{ "container map root", writer->trees[0], first->trees[0], run
		    + 1,
		    "OID of a physical object", 1 },
		{ "volume map root", writer->trees[1], second->trees[1], run +
		    4,
		    "OID of a physical object", 2 },
		{ "filesystem root", writer->trees[2], second->trees[2], 1027,
		    "Ephemeral and virtual OIDs (tree-node policy)", 2 },
		{ "extent root", writer->trees[3], second->trees[3], run + 6,
		    "OID of a physical object", 2 },
		{ "snapshot root", writer->trees[4], second->trees[4], run + 7,
		    "OID of a physical object", 2 }
	};
	const unsigned char *wm = writer->mapping_blocks[0];
	const unsigned char *nm = first->mapping_blocks[0];
	char field[160];

	for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
		comparison_xid = objects[i].xid;
		REQUIRE(snprintf(field, sizeof(field), "%s object XID",
		    objects[i].name) < (int)sizeof(field));
		if (objects[i].xid == 1)
			native_equal(field, "Object XID X",
			    N64(objects[i].writer, 16), N64(objects[i].native,
			    16));
		else
			expect_volume_xid(field, N64(objects[i].writer, 16),
			    N64(objects[i].native, 16));
		REQUIRE(snprintf(field, sizeof(field), "%s object OID",
		    objects[i].name) < (int)sizeof(field));
		expect_row(field, objects[i].rule, N64(objects[i].writer, 8),
		    N64(objects[i].native, 8), objects[i].oid);
		native_equal("object type", "Object header",
		    N32(objects[i].writer,
		    24), N32(objects[i].native, 24));
		native_equal("object subtype", "Object header",
		    N32(objects[i].writer, 28), N32(objects[i].native, 28));
	}
	comparison_xid = 1;
	native_equal("mapping block count", "Descriptor ring",
	    writer->mapping_count, first->mapping_count);
	native_equal("checkpoint mapping count", "Mapping block",
	    N32(wm, 36), N32(nm, 36));
	native_equal("last-map flag", "Mapping block", N32(wm, 32), N32(nm,
	    32));
	native_equal("mapping-block XID", "Object XID X", N64(wm, 16),
	    N64(nm, 16));
	for (uint64_t i = 0; i < N32(wm, 36); i++) {
		const unsigned char *entry = wm + 40 + 40 * i;
		const unsigned char *other = NULL;

		for (uint64_t j = 0; j < N32(nm, 36); j++)
			if (N64(nm, 40 + 40 * j + 24) == N64(entry, 24))
				other = nm + 40 + 40 * j;
		REQUIRE(other != NULL);
		native_range("mapping fields", "Mapping block", entry, other,
		    0, 32);
		native_equal("mapped data slot", "Data ring",
		    N64(entry, 32) - N64(writer->nx, 120),
		    N64(other, 32) - N64(first->nx, 120));
	}
	comparison_xid = 2;
	for (size_t map = 0; map < 2; map++) {
		const unsigned char *tree = writer->trees[map];
		native_record_t record, other;

		REQUIRE(N32(tree, 36) == N32(second->trees[map], 36));
		for (uint32_t i = 0; i < N32(tree, 36); i++) {
			REQUIRE(native_record(tree, i, &record));
			REQUIRE(native_record(second->trees[map], i, &other));
			expect_volume_xid("object-map key XID", N64(record.key,
			    8),
			    N64(other.key, 8));
			native_range("object-map value flags and size",
			    "Mapping block",
			    record.value, other.value, 0, 8);
		}
	}
}

static void
native_compare_trees(native_image_t *writer, native_image_t *native)
{
	static const char *names[] = { "container omap", "volume omap",
	    "filesystem", "extent-reference", "snapshot" };

	static const struct {
		const char *name;
		size_t offset;
		size_t width;
	} fields[] = {
		{ "node flags", 32, 2 }, { "level", 34, 2 },
		{ "toc offset", 40, 2 }, { "toc capacity", 42, 2 },
		{ "tree flags", 4056, 4 }, { "node size", 4060, 4 },
		{ "fixed key size", 4064, 4 }, { "fixed value size", 4068, 4 }
	};
	char field[192];
	native_record_t record;

	for (size_t t = 0; t < 5; t++) {
		const unsigned char *w = writer->trees[t];
		const unsigned char *n = native->trees[t];
		uint64_t longest_key = 0, longest_value = 0;
		uint64_t count = N32(w, 36), end = 4056;

		/* Empty fixtures have single root leaves. A native tree that
		 * grew an index needs a different fixture, not a silent skip.
		 */
		REQUIRE(N16(w, 34) == 0 && N16(n, 34) == 0);
		for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++)
		    {
			REQUIRE(snprintf(field, sizeof(field), "%s %s",
			    names[t],
			    fields[f].name) < (int)sizeof(field));
			native_equal(field, row_tree_shape,
			    native_uint(w + fields[f].offset, fields[f].width),
			    native_uint(n + fields[f].offset, fields[f].width));
		}
		REQUIRE(snprintf(field, sizeof(field), "%s key-area start",
		    names[t]) <
		    (int)sizeof(field));
		native_equal(field, row_tree_shape, 56 + N16(w, 40) + N16(w,
		    42),
		    56 + N16(n, 40) + N16(n, 42));
		for (uint32_t i = 0; i < count; i++) {
			REQUIRE(native_record(w, i, &record));
			if (record.key_length > longest_key)
				longest_key = record.key_length;
			if (record.value_length > longest_value)
				longest_value = record.value_length;
		}
		REQUIRE(snprintf(field, sizeof(field), "%s leaf record count",
		    names[t]) < (int)sizeof(field));
		expect_row(field, "Footer counts", N64(w, end + 24),
		    N64(n, end + 24), count);
		REQUIRE(snprintf(field, sizeof(field), "%s node count",
		    names[t]) <
		    (int)sizeof(field));
		expect_row(field, "Footer counts", N64(w, end + 32),
		    N64(n, end + 32), 1);
		REQUIRE(snprintf(field, sizeof(field), "%s longest key",
		    names[t]) <
		    (int)sizeof(field));
		expect_row(field, "Footer counts", N32(w, end + 16),
		    N32(n, end + 16), longest_key);
		REQUIRE(snprintf(field, sizeof(field), "%s longest value",
		    names[t]) <
		    (int)sizeof(field));
		expect_row(field, "Footer counts", N32(w, end + 20),
		    N32(n, end + 20), longest_value);
		if (t >= 3) {
			native_equal(names[t], "Empty extent-reference and "
			    "snapshot trees",
			    count, N32(n, 36));
			EXPECT(count == 0);
		}
		printf("CONSTRUCTION %s: one root leaf contains every record "
		    "[rows: Node fill; Last node of a level]\n", names[t]);
	}
	for (size_t i = 0; i < 2; i++) {
		const unsigned char *w = writer->maps[i];
		const unsigned char *n = native->maps[i];

		native_equal(i == 0 ? "container map flags" :
		    "volume map flags",
		    "Object-map flags", N32(w, 32), N32(n, 32));
		native_equal("omap tree type", "Object-map tree types",
		    N32(w, 40), N32(n, 40));
		native_equal("omap snapshot tree type", "Object-map tree types",
		    N32(w, 44), N32(n, 44));
		expect_unused_bytes("omap unused bytes", w, n,
		    88, 4096 - 88);
		native_range("omap snapshots", row_omap_snapshots,
		    w, n, 36, 4);
		native_range("omap snapshot state", row_omap_snapshots,
		    w, n, 56, 32);
	}
}

/* The xattr name length includes its terminating NUL. */
static bool
native_purgeable_key_valid(const native_record_t *record)
{
	return (record->key_length == 32 &&
	    N64(record->key, 0) == (UINT64_C(4) << 60 | 2) &&
	    N16(record->key, 8) == 22 &&
	    bcmp(record->key + 10, "purgeable-drecs-fixed", 21) == 0 &&
	    record->key[31] == 0);
}

/* An empty namespace has two reserved inodes and their names. Other
 * record kinds in the native root (for example its purgeable attribute)
 * are shown explicitly under the writer's no-extra-records policy.
 */
static void
native_compare_inodes(native_image_t *writer, native_image_t *native)
{
	native_record_t wrec, nrec;
	char field[128];
	uint64_t wkind_count[16] = { 0 }, nkind_count[16] = { 0 };

	for (size_t pass = 0; pass < 2; pass++) {
		const unsigned char *tree = pass == 0 ? writer->trees[2] :
		    native->trees[2];
		uint64_t *counts = pass == 0 ? wkind_count : nkind_count;

		for (uint32_t i = 0; i < N32(tree, 36); i++) {
			REQUIRE(native_record(tree, i, &wrec));
			REQUIRE(wrec.key_length >= 8);
			counts[N64(wrec.key, 0) >> 60]++;
		}
	}
	for (size_t kind = 0; kind < 16; kind++) {
		REQUIRE(snprintf(field, sizeof(field), "filesystem record "
		    "kind %zu",
		    kind) < (int)sizeof(field));
		if (kind == 4) {
			expect_native_value("native purgeable attribute excess",
			    row_sparse_records,
			    nkind_count[kind] > 1 ? nkind_count[kind] : 0, 0);
			for (uint32_t i = 0; i < N32(native->trees[2], 36);
			    i++) {
				REQUIRE(native_record(native->trees[2], i,
				    &nrec));
				if (N64(nrec.key, 0) >> 60 != 4)
					continue;
				EXPECT(native_purgeable_key_valid(&nrec));
			}
			expect_row(field, row_sparse_records,
			    wkind_count[kind],
			    nkind_count[kind], 0);
		} else {
			native_equal(field, row_sparse_records,
			    wkind_count[kind],
			    nkind_count[kind]);
		}
	}
	for (uint64_t inode = 2; inode <= 3; inode++) {
		bool found_writer = false, found_native = false;

		for (uint32_t i = 0; i < N32(writer->trees[2], 36); i++) {
			REQUIRE(native_record(writer->trees[2], i, &wrec));
			if (N64(wrec.key, 0) >> 60 != 9)
				continue;
			REQUIRE(wrec.value_length >= 18);
			if (N64(wrec.value, 0) == inode) {
				found_writer = true;
				break;
			}
		}
		for (uint32_t i = 0; i < N32(native->trees[2], 36); i++) {
			REQUIRE(native_record(native->trees[2], i, &nrec));
			if (N64(nrec.key, 0) >> 60 != 9)
				continue;
			REQUIRE(nrec.value_length >= 18);
			if (N64(nrec.value, 0) == inode) {
				found_native = true;
				break;
			}
		}
		REQUIRE(found_writer && found_native);
		native_equal("reserved directory name key length", "Name field",
		    wrec.key_length, nrec.key_length);
		if (wrec.key_length == nrec.key_length)
			native_range("reserved directory name key",
			    "Name field",
			    wrec.key, nrec.key, 0, wrec.key_length);
		native_equal("reserved directory entry type", "Directory inode",
		    N16(wrec.value, 16), N16(nrec.value, 16));
		native_exempt("directory record time", "Format time",
		    wrec.value + 8, nrec.value + 8, 8);
		found_writer = false;
		found_native = false;

		for (uint32_t i = 0; i < N32(writer->trees[2], 36); i++) {
			REQUIRE(native_record(writer->trees[2], i, &wrec));
			if (N64(wrec.key, 0) == (UINT64_C(3) << 60 | inode)) {
				found_writer = true;
				break;
			}
		}
		for (uint32_t i = 0; i < N32(native->trees[2], 36); i++) {
			REQUIRE(native_record(native->trees[2], i, &nrec));
			if (N64(nrec.key, 0) == (UINT64_C(3) << 60 | inode)) {
				found_native = true;
				break;
			}
		}
		REQUIRE(found_writer && found_native);
		REQUIRE(wrec.key_length == 8);
		REQUIRE(nrec.key_length == 8);
		REQUIRE(wrec.value_length >= 92 && nrec.value_length >= 92);
		REQUIRE(snprintf(field, sizeof(field), "inode %llu parent",
		    (unsigned long long)inode) < (int)sizeof(field));
		expect_row(field, "Inode numbers", N64(wrec.value, 0),
		    N64(nrec.value, 0), 1);
		expect_row("private ID", "Private ID", N64(wrec.value, 8),
		    N64(nrec.value, 8), inode);
		expect_row("directory children", "Directory inode",
		    N32(wrec.value, 56), N32(nrec.value, 56), 0);
		expect_row("uncompressed size", "Directory inode",
		    N64(wrec.value, 84), N64(nrec.value, 84), 0);
		native_equal("inode internal flags", row_inode_state,
		    N64(wrec.value, 48), N64(nrec.value, 48));
		native_equal("inode protection class", row_inode_state,
		    N32(wrec.value, 60), N32(nrec.value, 60));
		native_equal("inode write generation", row_inode_state,
		    N32(wrec.value, 64), N32(nrec.value, 64));
		expect_row("inode BSD flags", "BSD flags", N32(wrec.value, 68),
		    N32(nrec.value, 68), 0);
		expect_row("inode owner", "Default owner and modes",
		    N32(wrec.value, 72), N32(nrec.value, 72), 0);
		expect_row("inode group", "Default owner and modes",
		    N32(wrec.value, 76), N32(nrec.value, 76), 0);
		expect_row("inode mode", "Default owner and modes",
		    N16(wrec.value, 80), N16(nrec.value, 80), 040755);
		expect_unused_bytes("inode pad 82",
		    wrec.value, nrec.value, 82, 2);
		native_exempt("inode four timestamps",
		    "Format time; Time range",
		    wrec.value + 16, nrec.value + 16, 32);
		/* Root/private name xfields have the native representation.
		 * Validate their blob spans before comparing bytes.
		 */
		REQUIRE(wrec.value_length >= 104 && nrec.value_length >= 104);
		native_equal("inode name blob length", "Name field",
		    wrec.value_length - 92, nrec.value_length - 92);
		if (wrec.value_length == nrec.value_length)
			native_range("inode name blob", "Name field",
			    wrec.value,
			    nrec.value, 92, wrec.value_length - 92);
	}
	printf("NOT APPLICABLE empty catalog [rows: Non-directory inode; "
	    "File extent; Extent cap; Data-stream field; Reference records; "
	    "Name length; Link target]\n");
}

/* Allocation differs because XID 1 has only the container object map.
 * Remove exactly those two physical objects and the writer's eight-block
 * catalog from the comparison, asserting each excluded bit was allocated.
 * All remaining bits are compared, including padding past the volume end.
 */
static uint64_t
native_chunk_bitmap(native_image_t *image, const unsigned char *entry,
    unsigned char *bitmap, uint64_t catalog_first, uint64_t catalog_last)
{
	uint64_t free_blocks = 0, first = N64(entry, 8);
	uint64_t count = N32(entry, 16), address = N64(entry, 24);

	REQUIRE(count <= 32768 && first < image->blocks &&
	    count <= image->blocks - first);
	if (address != 0)
		REQUIRE(native_read(image, address, bitmap));
	else
		bzero(bitmap, 4096);
	for (uint64_t bit = 0; bit < 32768; bit++) {
		bool set = (bitmap[bit / 8] & (1u << (bit % 8))) != 0;
		uint64_t block = first + bit;

		if (bit < count && !set)
			free_blocks++;
		if (block >= catalog_first && block < catalog_last) {
			EXPECT(set);
			bitmap[bit / 8] &= (unsigned char)~(1u << (bit % 8));
		}
	}
	construction_fields++;
	printf("%s %s chunk free count: stored=%llu clear=%llu "
	    "[row: Free counts]\n", N32(entry, 20) == free_blocks ?
	    "CONSTRUCTION" : "FAIL", image->wanted_xid == 1 ? "native" :
	    "writer", (unsigned long long)N32(entry, 20),
	    (unsigned long long)free_blocks);
	EXPECT(N32(entry, 20) == free_blocks);
	return (free_blocks);
}

/* A bitmap present on only one side must describe catalog blocks in a chunk
 * the other image leaves entirely free, with no other allocation in it.
 */
static bool
catalog_only_chunk(const unsigned char *entry, const unsigned char *other,
    uint64_t catalog_first, uint64_t catalog_blocks)
{
	uint64_t first = N64(entry, 8), count = N32(entry, 16);
	uint64_t catalog_last = catalog_first + catalog_blocks;
	uint64_t overlap_first = first > catalog_first ? first : catalog_first;
	uint64_t overlap_last = first + count < catalog_last ?
	    first + count : catalog_last;

	if (overlap_first >= overlap_last || N32(other, 20) != N32(other, 16))
		return (false);
	EXPECT(count - N32(entry, 20) == overlap_last - overlap_first);
	return (true);
}

static void
native_compare_chunks(native_image_t *writer, native_image_t *native)
{
	unsigned char wcib[4096], ncib[4096], wb[4096], nb[4096];
	uint64_t cibs = N32(writer->sm, 64);
	uint64_t wpool = N64(writer->sm, 176), npool = N64(native->sm, 176);
	uint64_t wfirst = wpool + N64(writer->sm, 152);
	uint64_t nfirst = N64(native->nx, 160);
	uint64_t wused = cibs, nused = N32(native->sm, 64);
	uint64_t wfree = 0, nfree = 0;
	const uint64_t wcatalog_blocks = 8, ncatalog_blocks = 2;
	/* Bitmap objects needed only because this side's catalog uses a chunk
	 * that the other image leaves entirely free.
	 */
	uint64_t wcatalog_bitmaps = 0, ncatalog_bitmaps = 0;
	char field[160];

	REQUIRE(cibs == N32(native->sm, 64));
	for (uint64_t c = 0; c < cibs; c++) {
		uint64_t wblock = N64(writer->sm, N32(writer->sm, 80) + 8 * c);
		uint64_t nblock = N64(native->sm, N32(native->sm, 80) + 8 * c);

		REQUIRE(native_object(writer, wblock, OBJECT_CIB, 0,
		    wblock, wcib));
		REQUIRE(native_object(native, nblock, OBJECT_CIB, 0,
		    nblock, ncib));
		native_equal("CIB XID", "Object XID X", N64(wcib, 16),
		    N64(ncib, 16));
		native_equal("CIB relative placement", "Internal-pool blocks",
		    wblock - wpool, nblock - npool);
		native_range("CIB index and count", "Chunk-information entry",
		    wcib, ncib, 32, 8);
		REQUIRE(N32(wcib, 36) <= 126 && N32(ncib, 36) <= 126 &&
		    N32(wcib, 36) == N32(ncib, 36));
		for (uint64_t i = 0; i < N32(wcib, 36); i++) {
			const unsigned char *w = wcib + 40 + 32 * i;
			const unsigned char *n = ncib + 40 + 32 * i;
			uint64_t wa = N64(w, 24), na = N64(n, 24);

			REQUIRE(snprintf(field, sizeof(field), "chunk %llu "
			    "entry",
			    (unsigned long long)(126 * c + i)) <
			    (int)sizeof(field));
			native_range(field, "Chunk-information entry", w, n, 0,
			    20);
			wfree += native_chunk_bitmap(writer, w, wb, wfirst,
			    wfirst + wcatalog_blocks);
			nfree += native_chunk_bitmap(native, n, nb, nfirst,
			    nfirst + ncatalog_blocks);
			expect_writer_value("writer chunk bitmap presence",
			    "Chunk-information entry", wa != 0,
			    N32(w, 20) < N32(w, 16));
			expect_native_value("native chunk bitmap presence",
			    "Chunk-information entry", na != 0,
			    N32(n, 20) < N32(n, 16));
			wcatalog_bitmaps += catalog_only_chunk(w, n, wfirst,
			    wcatalog_blocks);
			ncatalog_bitmaps += catalog_only_chunk(n, w, nfirst,
			    ncatalog_blocks);
			if (na != 0) {
				expect_native_value("native bitmap ordering",
				    "Internal-pool blocks", na - npool, nused);
				nused++;
			}
			if (wa != 0) {
				expect_writer_value("writer bitmap ordering",
				    "Internal-pool blocks", wa - wpool, wused);
				wused++;
			}
			native_range("chunk bitmap except catalog allocation",
			    "Chunk-information entry; Bitmap bits past N", wb,
			    nb,
			    0, 4096);
		}
	}
	native_equal("writer device free blocks", "Free counts",
	    N64(writer->sm, 72), wfree);
	native_equal("native device free blocks", "Free counts",
	    N64(native->sm, 72), nfree);
	printf("EXPLAINS allocation: writer eight catalog blocks, XID 1 two "
	    "container-map blocks; writer pool objects=%llu native=%llu "
	    "[rows: Free counts; Allocated block count, total blocks allocated "
	    "and freed; Internal-pool blocks]\n",
	    (unsigned long long)wused, (unsigned long long)nused);
	native_equal("pool usage from catalog-only chunks",
	    "Internal-pool blocks", wused + ncatalog_bitmaps,
	    nused + wcatalog_bitmaps);
	REQUIRE(native_read(writer, N64(writer->sm, 168), wb));
	REQUIRE(native_read(native, N64(native->sm, 168), nb));
	for (uint64_t bit = 0; bit < 32768; bit++) {
		bool wset = (wb[bit / 8] & (1u << (bit % 8))) != 0;
		bool nset = (nb[bit / 8] & (1u << (bit % 8))) != 0;

		EXPECT(wset == (bit < wused));
		EXPECT(nset == (bit < nused));
	}
}

static void
native_compare_container(native_image_t *writer, native_image_t *native)
{
	const unsigned char *w = writer->nx, *n = native->nx;
	unsigned char block0[4096];
	char field[160];
	static const struct {
		const char *field;
		const char *row;
		size_t offset;
		size_t width;
	} nx_fields[] = {
		{ "block size", "Container size step", 36, 4 },
		{ "block count", "Container size step", 40, 8 },
		{ "optional features", "Features and flags", 48, 8 },
		{ "readonly features", "Features and flags", 56, 8 },
		{ "incompatible features", "Features and flags", 64, 8 },
		{ "descriptor blocks", "Checkpoint area sizes and bases", 104,
		    4 },
		{ "data blocks", "Checkpoint area sizes and bases", 108, 4 },
		{ "descriptor base", "Checkpoint area sizes and bases",
		    112, 8 },
		{ "data base", "Checkpoint area sizes and bases", 120, 8 },
		{ "maximum volumes", "Maximum volume count", 180, 4 },
		{ "ephemeral word 0", "Ephemeral-information words", 1312, 8 }
	};

	for (size_t i = 0; i < sizeof(nx_fields) / sizeof(nx_fields[0]); i++) {
		REQUIRE(snprintf(field, sizeof(field), "container %s",
		    nx_fields[i].field) < (int)sizeof(field));
		native_equal(field, nx_fields[i].row,
		    native_uint(w + nx_fields[i].offset, nx_fields[i].width),
		    native_uint(n + nx_fields[i].offset, nx_fields[i].width));
	}
	expect_row("checkpoint XID", "Object XID X", writer->xid,
	    native->xid, 1);
	expect_row("next XID", "Container next XID", N64(w, 96),
	    N64(n, 96), 2);
	expect_row("next OID", "Container next OID", N64(w, 88),
	    N64(n, 88), 1028);
	expect_row("container OID", "Container superblock OID", N64(w, 8),
	    N64(n, 8), 1);
	expect_row("space manager OID", "Ephemeral and virtual OIDs "
	    "(ephemeral objects)",
	    N64(w, 152), N64(n, 152), 1024);
	expect_row("reaper OID", "Ephemeral and virtual OIDs (ephemeral "
	    "objects)", N64(w, 168),
	    N64(n, 168), 1025);
	expect_row("volume OID slot 0", "Slot, index, role, name", N64(w, 184),
	    N64(n, 184), 1026);
	expect_writer_zero("unused volume slots", "Slot, index, role, name", w,
	    n,
	    192, 792);
	expect_row("descriptor next", "Descriptor ring", N32(w, 128),
	    N32(n, 128), 2);
	expect_row("descriptor index", "Descriptor ring", N32(w, 136),
	    N32(n, 136), 0);
	expect_row("descriptor count", "Descriptor ring", N32(w, 140),
	    N32(n, 140), 2);
	expect_row("data next", "Data ring", N32(w, 132), N32(n, 132), 2);
	expect_row("data index", "Data ring", N32(w, 144), N32(n, 144), 0);
	expect_row("data count", "Data ring", N32(w, 148), N32(n, 148), 2);
	REQUIRE(native_read(writer, 0, block0));
	native_range("block 0 vs checkpoint", "Block 0", block0, w, 0, 4096);
	expect_writer_zero("container counter 0", "Counters", w, n, 984, 8);
	EXPECT(N64(n, 984) != 0);
	for (size_t i = 1; i < 32; i++) {
		REQUIRE(snprintf(field, sizeof(field), "container counter %zu",
		    i) < (int)sizeof(field));
		expect_native_value(field, "Counters", N64(n, 984 + 8 * i), 0);
		native_equal(field, "Counters", N64(w, 984 + 8 * i),
		    N64(n, 984 + 8 * i));
	}
	expect_unused_bytes("container reserved tail", w, n,
	    1408, 4096 - 1408);
	native_range("container test type", row_container_absent,
	    w, n, 176, 4);
	native_range("container absent features", row_container_absent,
	    w, n, 1240, 72);
	native_range("ephemeral words 1-3", "Ephemeral-information words", w, n,
	    1320, 24);
	native_range("container test and Fusion fields", row_container_absent,
	    w, n, 1344, 40);
	expect_row("newest mounted version", "Newest mounted version",
	    N64(w, 1384), N64(n, 1384), 0);
	native_range("media keylocker", row_container_absent,
	    w, n, 1392, 16);
}

static void
native_compare_spaceman(native_image_t *writer, native_image_t *native)
{
	const unsigned char *ws = writer->sm, *ns = native->sm;
	uint64_t bitmap_blocks = N32(ws, 160);
	char field[160];
	static const struct {
		const char *field;
		const char *row;
		size_t offset;
		size_t width;
	} sm_fields[] = {
		{ "block size", "Geometry", 32, 4 },
		{ "blocks per chunk", "Geometry", 36, 4 },
		{ "chunks per CIB", "Geometry", 40, 4 },
		{ "CIBs per CAB", "Geometry", 44, 4 },
		{ "device blocks", "Geometry", 48, 8 },
		{ "chunk count", "Geometry", 56, 8 },
		{ "CIB count", "Geometry", 64, 4 },
		{ "CAB count", "Geometry", 68, 4 },
		{ "main array offset", "Internal pool", 80, 4 },
		{ "tier2 array offset", "Tier-2 device and queue", 128, 4 },
		{ "flags", row_sm_state, 144, 4 },
		{ "pool multiplier", "Internal pool", 148, 4 },
		{ "pool blocks", "Internal pool", 152, 8 },
		{ "bitmap size", "Internal pool", 160, 4 },
		{ "bitmap ring blocks", "Internal pool", 164, 4 },
		{ "bitmap ring base", "Internal pool", 168, 8 },
		{ "pool base", "Internal pool", 176, 8 },
		{ "pool node limit", "Free queues", 224, 2 },
		{ "main node limit", "Free queues", 264, 2 },
		{ "XID array offset", "Internal pool", 324, 4 },
		{ "bitmap index offset", "Internal pool", 328, 4 },
		{ "free-next offset", "Internal pool", 332, 4 },
		{ "version", row_sm_state, 336, 4 },
		{ "structure size", row_sm_state, 340, 4 }
	};
	for (size_t i = 0; i < sizeof(sm_fields) / sizeof(sm_fields[0]); i++) {
		REQUIRE(snprintf(field, sizeof(field), "space manager %s",
		    sm_fields[i].field) < (int)sizeof(field));
		native_equal(field, sm_fields[i].row,
		    native_uint(ws + sm_fields[i].offset, sm_fields[i].width),
		    native_uint(ns + sm_fields[i].offset, sm_fields[i].width));
	}
	native_range("tier2 device counts", "Tier-2 device and queue", ws, ns,
	    96, 32);
	native_range("reserve counts", row_sm_state,
	    ws, ns, 184, 16);
	native_range("tier2 queue", "Tier-2 device and queue", ws, ns, 280, 40);
	native_range("data zones", row_sm_state,
	    ws, ns, 344, 2176);
	for (size_t i = 0; i < 2; i++)
		native_range(i == 0 ? "pool free queue" : "main free queue",
		    "Free queues", ws, ns, 200 + 40 * i, 24);
	expect_row("pool bitmap free head", "Internal-pool ring fields",
	    N16(ws, 320), N16(ns, 320), bitmap_blocks);
	expect_row("pool bitmap free tail", "Internal-pool ring fields",
	    N16(ws, 322), N16(ns, 322), 16 * bitmap_blocks - 1);
	REQUIRE(bitmap_blocks <= N32(ns, 160));
	REQUIRE(16 * bitmap_blocks <= N32(ns, 164));
	REQUIRE(16 * bitmap_blocks <= N32(ws, 164));
	for (uint64_t i = 0; i < bitmap_blocks; i++) {
		expect_row("pool bitmap XID", "Internal-pool ring fields",
		    N64(ws, N32(ws, 324) + 8 * i),
		    N64(ns, N32(ns, 324) + 8 * i), 1);
		expect_row("pool bitmap index", "Internal-pool ring fields",
		    N16(ws, N32(ws, 328) + 2 * i),
		    N16(ns, N32(ns, 328) + 2 * i), i);
	}
	for (uint64_t i = 0; i < 16 * bitmap_blocks; i++)
		expect_row("pool bitmap free-next", "Internal-pool ring fields",
		    N16(ws, N32(ws, 332) + 2 * i),
		    N16(ns, N32(ns, 332) + 2 * i),
		    i < bitmap_blocks || i == 16 * bitmap_blocks - 1 ?
		    65535 : i + 1);
	{
		const size_t reserved[][2] = {
			{ 84, 12 }, { 132, 12 }, { 226, 14 }, { 266, 14 }
		};
		unsigned char wtail[4096], ntail[4096];

		for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]);
		    i++)
			expect_unused_bytes("space manager reserved words",
			    ws, ns, reserved[i][0], reserved[i][1]);
		bcopy(ws, wtail, 4096);
		bcopy(ns, ntail, 4096);
		for (size_t pass = 0; pass < 2; pass++) {
			unsigned char *tail = pass == 0 ? wtail : ntail;
			const unsigned char *sm = pass == 0 ? ws : ns;
			const size_t offsets[] = { 80, 324, 328, 332 };
			const size_t lengths[] = { 8 * N32(sm, 64),
			    8 * N32(sm, 160), 2 * N32(sm, 160),
			    2 * N32(sm, 164) };

			for (size_t i = 0; i < 4; i++)
				bzero(tail + N32(sm, offsets[i]), lengths[i]);
		}
		expect_unused_bytes("space manager outside arrays",
		    wtail, ntail, 2520, 4096 - 2520);
	}
	native_range("reaper idle state", "Reaper", writer->reaper,
	    native->reaper, 32, 4096 - 32);
}

static void
native_compare_volume(native_image_t *writer, native_image_t *native)
{
	const unsigned char *wv = writer->volume, *nv = native->volume;
	char field[160];
	static const struct {
		const char *field;
		const char *row;
		size_t offset;
		size_t width;
	} volume_fields[] = {
		{ "optional features", "Features and flags", 40, 8 },
		{ "readonly features", "Features and flags", 48, 8 },
		{ "incompatible features", "Features and flags", 56, 8 },
		{ "reserve", "Reserve and quota", 72, 8 },
		{ "quota", "Reserve and quota", 80, 8 },
		{ "flags", "Features and flags", 264, 8 },
		{ "next document ID", row_document_id, 960, 4 },
		{ "clone epoch", row_document_id, 984, 8 },
		{ "clone XID", row_document_id, 992, 8 }
	};
	for (size_t i = 0; i < sizeof(volume_fields) /
	    sizeof(volume_fields[0]); i++) {
		REQUIRE(snprintf(field, sizeof(field), "volume %s",
		    volume_fields[i].field) < (int)sizeof(field));
		native_equal(field, volume_fields[i].row,
		    native_uint(wv + volume_fields[i].offset,
		    volume_fields[i].width),
		    native_uint(nv + volume_fields[i].offset,
		    volume_fields[i].width));
	}
	expect_row("volume index", "Slot, index, role, name", N32(wv, 36),
	    N32(nv, 36), 0);
	expect_row("volume role", "Slot, index, role, name", N16(wv, 964),
	    N16(nv, 964), 0);
	native_range("volume name", "Slot, index, role, name", wv, nv, 704,
	    256);
	native_range("volume transitions", row_volume_transitions,
	    wv, nv, 160, 16);
	native_range("volume root-to and rolling", row_volume_transitions,
	    wv, nv, 968, 16);
	expect_native_value("native crypto major version", "Metadata crypto "
	    "state",
	    N16(nv, 96), 5);
	expect_native_value("native crypto minor version", "Metadata crypto "
	    "state",
	    N16(nv, 98), 0);
	expect_native_value("native crypto flags", "Metadata crypto state",
	    N32(nv, 100), 0);
	expect_native_value("native crypto persistent class", "Metadata "
	    "crypto state",
	    N32(nv, 104), 6);
	EXPECT(N32(nv, 108) != 0);
	expect_native_value("native crypto revision", "Metadata crypto state",
	    N16(nv, 112), 1);
	expect_native_value("native crypto unused", "Metadata crypto state",
	    N16(nv, 114), 0);
	expect_writer_zero("volume metadata crypto state", "Metadata crypto "
	    "state",
	    wv, nv, 96, 20);
	native_range("modification records", "Modification records", wv, nv,
	    320, 384);
	{
		const size_t offsets[] = { 1056, 1064, 1068, 1104, 1120, 1128 };
		unsigned char normalized[4096];

		bcopy(nv, normalized, sizeof(normalized));
		for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]);
		    i++) {
			size_t offset = offsets[i];
			size_t width = offset == 1064 || offset == 1068 ? 4 : 8;

			EXPECT(native_uint(nv + offset, width) != 0);
			if (offset == 1056)
				expect_native_value("native document index XID",
				    row_volume_tail, N64(nv, offset),
				    N64(nv, 312));
			expect_writer_zero("volume tail word", row_volume_tail,
			    wv, nv, offset, width);
			bzero(normalized + offset, width);
		}
		expect_unused_bytes("volume tail outside named words",
		    wv, normalized, 1056, 4096 - 1056);
	}

	native_range("volume snapshot/group/integrity/sealing", "Snapshot, "
	    "sealing and integrity fields; UUIDs",
	    wv, nv, 1000, 40);
	for (size_t offset = VOLUME_ROOT_TYPE;
	    offset <= VOLUME_SNAPSHOT_TYPE; offset += 4) {
		REQUIRE(snprintf(field, sizeof(field), "volume tree type %zu",
		    offset) < (int)sizeof(field));
		native_equal(field, "Volume tree types",
		    N32(wv, offset), N32(nv, offset));
	}
	EXPECT(N64(wv, 1032) == 0);
	native_equal("sealed file-extent tree type (offset 1040)",
	    "Volume tree types", N32(wv, VOLUME_SEALED_TYPE),
	    N32(nv, VOLUME_SEALED_TYPE));
	native_equal("reserved tree type (offset 1044)", "Volume tree types",
	    N32(wv, VOLUME_RESERVED_TYPE), N32(nv, VOLUME_RESERVED_TYPE));
	native_equal("reserved tree OID", "Volume tree types",
	    N64(wv, 1048), N64(nv, 1048));
	EXPECT(N64(wv, 1048) == 0);
	expect_unused_bytes("volume reserved 966", wv, nv, 966, 2);
	expect_row("volume next inode", "Volume next object identifier",
	    N64(wv, 176), N64(nv, 176), 16);
	for (size_t i = 0; i < 7; i++) {
		REQUIRE(snprintf(field, sizeof(field), "volume counter %zu", i)
		    <
		    (int)sizeof(field));
		expect_row(field, i < 5 ? "Object counts" : row_allocated,
		    N64(wv, 184 + i * 8), N64(nv, 184 + i * 8), 0);
	}
	expect_row("volume allocated blocks", row_allocated,
	    N64(wv, 88), N64(nv, 88), 5);
	expect_row("filesystem root OID", "Ephemeral and virtual OIDs "
	    "(tree-node policy)",
	    N64(wv, 136), N64(nv, 136), 1027);
	expect_volume_xid("formatting XID", N64(wv, 312), N64(nv, 312));
	native_exempt("container UUID", "UUIDs", writer->nx + 72,
	    native->nx + 72, 16);
	native_exempt("volume UUID", "UUIDs", wv + 240, nv + 240, 16);
	native_exempt("volume modification time", "Modification and unmount "
	    "times",
	    wv + 256, nv + 256, 8);
	native_equal("volume unmount time", "Modification and unmount times",
	    N64(wv, 64), N64(nv, 64));
	native_exempt("formatting time", "Formatting record", wv + 304,
	    nv + 304, 8);
	native_exempt("formatting identifier", "Formatting record", wv + 272,
	    nv + 272, 32);
}

typedef struct native_sink {
	int fd;
	uint64_t length;
	uint64_t initialized;
	bool wrote;
} native_sink_t;

static fsimg_status_t native_sink_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t native_sink_write(void *, uint64_t, const void *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t native_sink_zero(void *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t native_sink_flush(void *, fsimg_error_t *);

static fsimg_status_t
native_sink_size(void *argument, uint64_t *length,
    fsimg_error_t *error)
{
	const native_sink_t *sink = argument;

	(void)error;
	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
native_sink_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	native_sink_t *sink = argument;
	ssize_t count;

	(void)error;
	REQUIRE(offset <= sink->length && length <= sink->length - offset);
	REQUIRE(sink->initialized == sink->length);
	sink->wrote = true;
	do {
		count = pwrite(sink->fd, buffer, length, (off_t)offset);
	} while (count < 0 && errno == EINTR);
	*transferred = count < 0 ? 0 : (size_t)count;
	return (count < 0 ? FSIMG_EIO : FSIMG_OK);
}

/* Only initialization may use sparse zeroes. After a write, a zero request
 * must not pretend to erase existing bytes, so reject it loudly.
 */
static fsimg_status_t
native_sink_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	native_sink_t *sink = argument;

	(void)error;
	REQUIRE(!sink->wrote && offset == sink->initialized);
	REQUIRE(offset <= sink->length && length <= sink->length - offset);
	sink->initialized += length;
	return (FSIMG_OK);
}

static fsimg_status_t
native_sink_flush(void *argument, fsimg_error_t *error)
{
	const native_sink_t *sink = argument;

	(void)error;
	return (fsync(sink->fd) == 0 ? FSIMG_OK : FSIMG_EIO);
}

static void
native_put(unsigned char *bytes, size_t offset, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++)
		bytes[offset + i] = (unsigned char)(value >> (8 * i));
}

unsigned
apfs_native_test_decoder(void)
{
	unsigned char node[4096] = { 0 };
	native_record_t record;
	unsigned before = failures;

	scenario = "native decoder rejects a zero-length key at block end";
	native_put(node, OBJECT_SUBTYPE, TREE_FILESYSTEM, 4);
	native_put(node, NODE_FLAGS, NODE_LEAF, 2);
	native_put(node, NODE_RECORD_COUNT, 1, 4);
	native_put(node, NODE_TOC_LENGTH, 8, 2);
	native_put(node, 56, 4096 - 64, 2);
	EXPECT(!native_record(node, 0, &record));

	scenario = "native object maps require fixed-size records";
	native_put(node, OBJECT_SUBTYPE, TREE_OMAP, 4);
	native_put(node, 56, 0, 2);
	native_put(node, 58, 16, 2);
	native_put(node, 60, 16, 2);
	native_put(node, 62, 16, 2);
	EXPECT(!native_record(node, 0, &record));

	scenario = "native inode and directory values require their fixed body";
	native_put(node, OBJECT_SUBTYPE, TREE_FILESYSTEM, 4);
	native_put(node, 64, UINT64_C(3) << 60 | 2, 8);
	EXPECT(!native_record(node, 0, &record));
	native_put(node, 64, UINT64_C(9) << 60 | 2, 8);
	EXPECT(!native_record(node, 0, &record));

	scenario = "native inode key is exactly eight bytes";
	native_put(node, 58, 8, 2);
	native_put(node, 60, 92, 2);
	native_put(node, 62, 92, 2);
	native_put(node, 64, UINT64_C(3) << 60 | 2, 8);
	EXPECT(native_record(node, 0, &record));

	scenario = "native decoder rejects a sixteen-byte inode key";
	native_put(node, 58, 16, 2);
	EXPECT(!native_record(node, 0, &record));

	scenario = "native purgeable attribute key includes its NUL";
	bzero(node, sizeof(node));
	native_put(node, OBJECT_SUBTYPE, TREE_FILESYSTEM, 4);
	native_put(node, NODE_FLAGS, NODE_LEAF, 2);
	native_put(node, NODE_RECORD_COUNT, 1, 4);
	native_put(node, NODE_TOC_LENGTH, 8, 2);
	native_put(node, 58, 32, 2);
	native_put(node, 60, 4, 2);
	native_put(node, 62, 4, 2);
	native_put(node, 64, UINT64_C(4) << 60 | 2, 8);
	native_put(node, 72, 22, 2);
	bcopy("purgeable-drecs-fixed", node + 74, 22);
	if (native_record(node, 0, &record))
		EXPECT(native_purgeable_key_valid(&record));
	else
		EXPECT(!"purgeable attribute record rejected");

	scenario = "native purgeable attribute key rejects an omitted NUL";
	native_put(node, 58, 31, 2);
	native_put(node, 72, 21, 2);
	if (native_record(node, 0, &record))
		EXPECT(!native_purgeable_key_valid(&record));
	else
		EXPECT(!"short purgeable attribute record not decoded");

	scenario = "native map root, index and leaf flags match their types";
	bzero(node, sizeof(node));
	native_put(node, OBJECT_TYPE, OBJECT_PHYSICAL_ROOT, 4);
	native_put(node, NODE_FLAGS, NODE_ROOT | NODE_LEAF | NODE_FIXED, 2);
	EXPECT(native_omap_shape(node, 0));
	EXPECT(!native_omap_shape(node, 1));
	native_put(node, NODE_LEVEL, 1, 2);
	EXPECT(!native_omap_shape(node, 0));
	native_put(node, NODE_FLAGS, NODE_ROOT | NODE_FIXED, 2);
	EXPECT(native_omap_shape(node, 0));
	native_put(node, OBJECT_TYPE, OBJECT_PHYSICAL_NODE, 4);
	native_put(node, NODE_FLAGS, NODE_FIXED, 2);
	EXPECT(native_omap_shape(node, 1));
	native_put(node, NODE_FLAGS, 0, 2);
	EXPECT(!native_omap_shape(node, 1));

	scenario = "native map fixed-size leaf and index record spans";
	bzero(node, sizeof(node));
	native_put(node, OBJECT_SUBTYPE, TREE_OMAP, 4);
	native_put(node, NODE_FLAGS, NODE_LEAF | NODE_FIXED, 2);
	native_put(node, NODE_RECORD_COUNT, 1, 4);
	native_put(node, NODE_TOC_LENGTH, 4, 2);
	native_put(node, 58, 16, 2);
	EXPECT(native_record(node, 0, &record));
	EXPECT(record.key_length == 16 && record.value_length == 16);
	native_put(node, NODE_FLAGS, NODE_FIXED, 2);
	native_put(node, 58, 8, 2);
	EXPECT(native_record(node, 0, &record));
	EXPECT(record.key_length == 16 && record.value_length == 8);
	native_put(node, 56, 4096 - 60 - 8, 2);
	EXPECT(!native_record(node, 0, &record));
	{
		native_image_t image = { .blocks = 1024 };

		scenario = "native space-manager arrays use bounds, not "
		    "layout fits";
		native_put(image.sm, 64, 2, 4);
		native_put(image.sm, 160, 2, 4);
		native_put(image.sm, 164, 32, 4);
		native_put(image.sm, SM_CIB_ARRAY, 1800, 4);
		native_put(image.sm, 128, 1900, 4);
		native_put(image.sm, SM_BITMAP_XIDS, 2000, 4);
		native_put(image.sm, SM_BITMAP_INDEXES, 2100, 4);
		native_put(image.sm, SM_BITMAP_FREE_NEXT, 2200, 4);
		EXPECT(native_spaceman_arrays(&image));
		native_put(image.sm, SM_BITMAP_XIDS, 4090, 4);
		EXPECT(!native_spaceman_arrays(&image));
	}
	native_test_capture_continuation();
	return (failures - before);
}

static void
native_explain_change(const char *field, const char *row, uint64_t first,
    uint64_t second)
{
	if (first != second)
		printf("FORMS %s: XID 1=%llu XID 2=%llu [row: %s]\n", field,
		    (unsigned long long)first, (unsigned long long)second, row);
}

static void
native_explain_forms(const native_image_t *first,
    const native_image_t *second)
{
	static const struct {
		const char *field;
		const char *row;
		size_t offset;
		size_t width;
	} fields[] = {
		{ "object XID", "Object XID X", 16, 8 },
		{ "next XID", "Container next XID", 96, 8 },
		{ "next OID", "Container next OID", 88, 8 },
		{ "descriptor next", "Descriptor ring", NX_DESC_NEXT, 4 },
		{ "descriptor index", "Descriptor ring", NX_DESC_INDEX, 4 },
		{ "descriptor count", "Descriptor ring", NX_DESC_COUNT, 4 },
		{ "data next", "Data ring", NX_DATA_NEXT, 4 },
		{ "data index", "Data ring", NX_DATA_INDEX, 4 },
		{ "data count", "Data ring", NX_DATA_COUNT, 4 },
		{ "volume slot 0", "Slot, index, role, name", 184, 8 },
		{ "newest mounted version", "Newest mounted version", 1384, 8 }
	};
	const unsigned char *a = first->sm, *b = second->sm;

	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		native_explain_change(fields[i].field, fields[i].row,
		    native_uint(first->nx + fields[i].offset, fields[i].width),
		    native_uint(second->nx + fields[i].offset,
		    fields[i].width));
	native_explain_change("mapping count", "Mapping block",
	    N32(first->mapping_blocks[0], 36), N32(second->mapping_blocks[0],
	    36));
	for (size_t offset = 200; offset <= 256; offset += 8) {
		if (offset == 224 || offset == 232)
			continue;
		native_explain_change("free queue", "Free queues",
		    N64(a, offset), N64(b, offset));
	}
	native_explain_change("pool ring free head",
	    "Internal-pool ring fields",
	    N16(a, 320), N16(b, 320));
	native_explain_change("pool ring free tail",
	    "Internal-pool ring fields",
	    N16(a, 322), N16(b, 322));
	for (size_t i = 0; i < N32(a, 160) && i < N32(b, 160); i++) {
		native_explain_change("pool slot XID", "Internal-pool ring "
		    "fields",
		    N64(a, N32(a, SM_BITMAP_XIDS) + 8 * i),
		    N64(b, N32(b, SM_BITMAP_XIDS) + 8 * i));
		native_explain_change("pool slot index", "Internal-pool ring "
		    "fields",
		    N16(a, N32(a, SM_BITMAP_INDEXES) + 2 * i),
		    N16(b, N32(b, SM_BITMAP_INDEXES) + 2 * i));
	}
	for (size_t i = 0; i < N32(a, 164) && i < N32(b, 164); i++)
		native_explain_change("pool free-next", "Internal-pool ring "
		    "fields",
		    N16(a, N32(a, SM_BITMAP_FREE_NEXT) + 2 * i),
		    N16(b, N32(b, SM_BITMAP_FREE_NEXT) + 2 * i));
	for (size_t i = 0; i < N32(a, 64) && i < N32(b, 64); i++)
		native_explain_change("CIB address", "Internal-pool blocks",
		    N64(a, N32(a, SM_CIB_ARRAY) + 8 * i),
		    N64(b, N32(b, SM_CIB_ARRAY) + 8 * i));
}

/* Heap state survives the malformed-capture jump without indeterminate
 * automatic objects. All files and caches have this single cleanup owner.
 */
typedef struct native_capture {
	native_image_t first;
	native_image_t second;
	native_image_t writer;
	native_sink_t sink;
} native_capture_t;

static void
native_run_capture(const char *directory, const char *name, uint64_t size)
{
	native_capture_t *capture = calloc(1, sizeof(*capture));
	native_image_t *first, *second, *writer;
	fsimg_write_options_t options;
	fsimg_io_ops_t ops;
	struct stat st;
	char path[4096], temporary[4096], label[256];
	const char *scratch = getenv("TMPDIR");
	unsigned before = failures;

	CHECK(capture != NULL);
	first = &capture->first;
	second = &capture->second;
	writer = &capture->writer;
	first->fd = second->fd = writer->fd = capture->sink.fd = -1;
	bzero(capture_fields, sizeof(capture_fields));
	construction_fields = exempt_fields = 0;
	CHECK(snprintf(path, sizeof(path), "%s/%s/image.img", directory,
	    name) < (int)sizeof(path));
	scenario = path;
	printf("\nCAPTURE %s (%llu bytes)\n", name, (unsigned long long)size);
	if (setjmp(malformed_capture) != 0)
		goto out;
	first->fd = open(path, O_RDONLY | O_NOFOLLOW);
	REQUIRE(first->fd != -1 && fstat(first->fd, &st) == 0 &&
	    S_ISREG(st.st_mode) && st.st_size == (off_t)size);
	first->blocks = size / BLOCK_BYTES;
	first->wanted_xid = 1;
	printf("DECODE XID 1: descriptor slots and retained data objects; "
	    "checksums, OIDs, XIDs and mapping spans required intact\n");
	REQUIRE(native_decode(first));
	second->fd = first->fd;
	second->blocks = first->blocks;
	second->wanted_xid = 2;
	REQUIRE(native_decode(second));
	first_form_xid = first->xid;
	second_form_xid = second->xid;
	native_explain_forms(first, second);
	printf("DECODE intact: XID 1 at block %llu; XID 2 at block %llu\n",
	    (unsigned long long)first->checkpoint,
	    (unsigned long long)second->checkpoint);
	REQUIRE(memchr(second->volume + 704, '\0', 256) != NULL);
	bcopy(second->volume + 704, label, sizeof(label));
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = INT64_C(1700000000);
	options.label = label;
	options.volume_bytes = size;
	options.case_sensitive = (N64(second->volume, 56) & 1) == 0;
	if (scratch == NULL || scratch[0] == '\0')
		scratch = "/tmp";
	REQUIRE(snprintf(temporary, sizeof(temporary),
	    "%s/apfs-native.XXXXXX", scratch) < (int)sizeof(temporary));
	capture->sink.fd = mkstemp(temporary);
	REQUIRE(capture->sink.fd != -1);
	REQUIRE(unlink(temporary) == 0);
	capture->sink.length = size;
	REQUIRE(ftruncate(capture->sink.fd, (off_t)size) == 0);
	fsimg_io_ops_init(&ops);
	ops.get_size = native_sink_size;
	ops.write_at = native_sink_write;
	ops.zero_range = native_sink_zero;
	ops.flush = native_sink_flush;
	REQUIRE(apfs_native_emit(&options, &ops, &capture->sink));
	writer->fd = capture->sink.fd;
	writer->blocks = first->blocks;
	REQUIRE(native_decode(writer));

	comparison_xid = 1;
	native_compare_container(writer, first);
	native_compare_spaceman(writer, first);
	native_compare_chunks(writer, first);
	comparison_xid = 2;
	native_compare_volume(writer, second);
	native_compare_objects(writer, first, second);
	native_compare_trees(writer, second);
	native_compare_inodes(writer, second);
out:
	printf("SUMMARY %s: XID 1 fields=%u; XID 2 fields=%u; "
	    "constructions=%u; exemptions=%u; failures=%u\n", name,
	    capture_fields[1], capture_fields[2], construction_fields,
	    exempt_fields, failures - before);
	if (failures != before)
		printf("FAIL %s: %u failures\n", name, failures - before);
	free(first->mapping_blocks);
	free(second->mapping_blocks);
	free(writer->mapping_blocks);
	if (first->fd != -1)
		CHECK(close(first->fd) == 0);
	if (capture->sink.fd != -1)
		CHECK(close(capture->sink.fd) == 0);
	free(capture);
}

/* Two malformed files exercise the public comparator's capture boundary.
 * Each expected rejection must return so the following capture is checked.
 */
static void
native_test_capture_continuation(void)
{
	static const char *names[] = { "bad-first-checkpoint", "next-capture" };
	const char *scratch = getenv("TMPDIR");
	char root[4096], directory[4096], path[4096];

	if (scratch == NULL || scratch[0] == '\0')
		scratch = "/tmp";
	CHECK(snprintf(root, sizeof(root), "%s/apfs-native-test.XXXXXX",
	    scratch) < (int)sizeof(root));
	CHECK(mkdtemp(root) != NULL);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		unsigned before = failures, rejected;
		int fd;

		CHECK(snprintf(directory, sizeof(directory), "%s/%s", root,
		    names[i]) < (int)sizeof(directory));
		CHECK(mkdir(directory, 0700) == 0);
		CHECK(snprintf(path, sizeof(path), "%s/image.img", directory) <
		    (int)sizeof(path));
		fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, 4194304) == 0);
		CHECK(close(fd) == 0);
		printf("REGRESSION expected malformed capture rejection: %s\n",
		    names[i]);
		native_run_capture(root, names[i], 4194304);
		rejected = failures - before;
		failures = before;
		scenario = "malformed native capture returns to the "
		    "next capture";
		EXPECT(rejected == 1);
		CHECK(unlink(path) == 0);
		CHECK(rmdir(directory) == 0);
	}
	CHECK(rmdir(root) == 0);
}

int
apfs_compare_native(const char *directory)
{
	static const struct {
		const char *name;
		uint64_t size;
	} captures[] = {
		{ "apfs-newfs-4194304-plain", UINT64_C(4194304) },
		{ "apfs-newfs-33554432-plain", UINT64_C(33554432) },
		{ "apfs-hdiutil-33554432-plain", UINT64_C(33554432) },
		{ "apfs-newfs-536870912-plain", UINT64_C(536870912) },
		{ "apfs-newfs-1073741824-plain", UINT64_C(1073741824) },
		{ "apfs-newfs-8589934592-plain", UINT64_C(8589934592) },
		{ "apfs-newfs-107374182400-plain", UINT64_C(107374182400) }
	};

	for (size_t i = 0; i < sizeof(captures) / sizeof(captures[0]); i++)
		native_run_capture(directory, captures[i].name,
		    captures[i].size);
	printf("native APFS: %zu captures, %u failures\n",
	    sizeof(captures) / sizeof(captures[0]), failures);
	return (failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
