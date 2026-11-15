/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "apfs-gen.h"
#include "fsimg.h"
#include "scale.h"
#include "test.h"

#define BLOCK 4096u
#define PREFIX_BLOCKS 61u
#define FANOUT 64u
#define LEAF_RECORDS 16u
#define LEVELS 16u
#define SIBLING_BASE (UINT64_C(1) << 40)
#define SCRATCH_LIMIT (UINT64_C(4) << 30)
#define RSS_ALLOWANCE (UINT64_C(32) << 20)

/* Fixture identities and checkpoint locations. */
#define FIXTURE_XID 10u
#define FS_FIRST_OID 2000u
#define VOLUME_OID 1000u
#define SPACEMAN_OID 500u
#define REAPER_OID 501u
#define IP_FREE_QUEUE_OID 502u
#define MAIN_FREE_QUEUE_OID 503u
#define SPACEMAN_BLOCK 9u
#define REAPER_BLOCK 13u
#define IP_FREE_QUEUE_BLOCK 14u
#define MAIN_FREE_QUEUE_BLOCK 15u
#define ROOT_PARENT 1u
#define ROOT_INODE 2u
#define PRIVATE_INODE 3u
#define FIRST_FILE_INODE 16u

/* Block offsets from im->volume, before the filesystem tree. */
#define CONTAINER_OMAP_TREE 1u
#define VOLUME_HEADER 2u
#define VOLUME_OMAP_HEADER 3u
#define EXTENTREF_TREE 4u
#define SNAPMETA_TREE 5u
#define VOLUME_BLOCKS 6u

/* Encoded record sizes, including the fixed eight-byte directory name. */
#define OMAP_KEY_SIZE 16u
#define OMAP_VALUE_SIZE 16u
#define INODE_VALUE_BASE 92u
#define DREC_VALUE_BASE 18u
#define DREC_KEY_SIZE 19u
#define SIBLING_KEY_SIZE 16u
#define SIBLING_VALUE_SIZE 19u
#define EXTENT_KEY_SIZE 16u
#define EXTENT_VALUE_SIZE 24u
#define PHYS_EXTENT_VALUE_SIZE 20u
#define XFIELD_HEADER_SIZE 4u
#define XFIELD_ENTRY_SIZE 4u
#define DSTREAM_SIZE 40u
#define NAME_PADDED_SIZE 16u
/* File inode: two xfield entries, padded name, and dstream. */
#define LONGEST_FS_VALUE (INODE_VALUE_BASE + XFIELD_HEADER_SIZE + \
    2 * XFIELD_ENTRY_SIZE + NAME_PADDED_SIZE + DSTREAM_SIZE)
#define CAB_CAPACITY 507u
#define CIB_CAPACITY 126u
#define DEFAULT_FILES 32u
#define DEFAULT_AMOUNT 1000000u
/* Arbitrary test cap, below the eight-digit name and int32_t limits. */
#define MAX_AMOUNT 10000000u

/* Independent fixture encodings from the APFS on-disk layout. */
#define OBJECT_EPHEMERAL 0x80000000u
#define OBJECT_PHYSICAL 0x40000000u
#define KIND_BTREE_ROOT 2u
#define KIND_BTREE_NODE 3u
#define KIND_SPACEMAN 5u
#define KIND_SPACEMAN_CAB 6u
#define KIND_SPACEMAN_CIB 7u
#define KIND_OMAP 11u
#define KIND_FS 13u
#define KIND_REAPER 17u
#define SUBTYPE_FREE_QUEUE 9u
#define SUBTYPE_OMAP 11u
#define SUBTYPE_FSTREE 14u
#define SUBTYPE_EXTENTREF 15u
#define SUBTYPE_SNAPMETA 16u
#define TREE_UINT64_KEYS 0x01u
#define TREE_SEQUENTIAL_INSERT 0x02u
#define TREE_ALLOW_GHOSTS 0x04u
#define TREE_EPHEMERAL 0x08u
#define TREE_PHYSICAL 0x10u
#define TREE_KV_NONALIGNED 0x40u
#define RECORD_PHYS_EXTENT 2u
#define RECORD_INODE 3u
#define RECORD_DSTREAM_ID 6u
#define RECORD_SIBLING_MAP 12u
#define XFIELD_INODE_NAME 4u
#define XFIELD_INODE_DSTREAM 8u
#define XFIELD_DREC_SIBLING_ID 1u
#define XFIELD_DO_NOT_COPY 2u
#define DREC_REGULAR_FILE 8u
#define NX_MAGIC 0x4253584eu
#define FS_MAGIC 0x42535041u
#define NX_INCOMPAT_VERSION2 2u
#define NX_BLOCK_COUNT_OFFSET 40u
#define NX_OMAP_OID_OFFSET 160u
#define OBJECT_OID_OFFSET 8u

/* No image file exists. Only the checkpoint prefix and six volume blocks
 * are retained. Leaves, parents, object maps, allocation pages and payload
 * are functions of their block number. A level stores its first node, count
 * and record span; these at most 17 triples replace a catalog in memory.
 */
typedef enum shape { LARGE, WIDE, EXTENTS, DEEP, NAMES, BIG_FILE } shape_t;

typedef struct tree {
	uint64_t base;
	uint64_t records;
	uint64_t nodes;
	uint64_t first[LEVELS + 1];
	uint64_t count[LEVELS + 1];
	uint64_t span[LEVELS + 1];
	unsigned levels;
} tree_t;

typedef struct image {
	shape_t mode;
	uint64_t amount;
	uint64_t entries;
	uint64_t blocks;
	uint64_t file_size;
	uint64_t data_blocks;
	uint64_t volume;
	uint64_t payload;
	uint64_t allocated_end;
	uint64_t bitmap_base;
	uint64_t bitmap_count;
	uint64_t pool_used;
	uint64_t reads;
	uint64_t bytes;
	/* Filesystem and volume object-map source fetches; cache hits
	 * inside the reader are not observable through the stats API.
	 */
	uint64_t node_loads;
	uint64_t fs_loads;
	uint64_t cached_block;
	apfs_gen_spaceman_t sm;
	size_t sm_bytes;
	tree_t fs;
	tree_t map;
	unsigned char prefix[PREFIX_BLOCKS * BLOCK];
	unsigned char headers[VOLUME_BLOCKS * BLOCK];
	unsigned char cache[BLOCK];
} image_t;

typedef struct record {
	unsigned char key[32];
	unsigned char value[192];
	uint16_t key_size;
	uint16_t value_size;
} record_t;

/* One run owns the callback image, context and I/O until cleanup in run().
 * Phase helpers accumulate their measurements here for the final report.
 */
typedef struct scale_run {
	image_t *im;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_error_t error;
	fsimg_stats_t baseline, stats;
	uint64_t entries, replay_loads, rss_base, rss;
	double start;
	fsimg_status_t status, verify, extracted;
	bool verify_ran, extract_ran;
	int result;
} scale_run_t;

static char cleanup_directory[sizeof("apfs-scale-XXXXXX")];

static void cleanup_run_directory(void);
static void store(unsigned char *, uint64_t, unsigned);
static uint64_t ceiling(uint64_t, uint64_t);
static void tree_plan(tree_t *, uint64_t, uint64_t, unsigned);
static void inode_record(const image_t *, record_t *, uint64_t);
static void dstream_record(record_t *, uint64_t);
static void fs_suffix_record(const image_t *, uint64_t, record_t *);
static void fs_record(const image_t *, uint64_t, record_t *);
static uint64_t fs_record_count(const image_t *);
static void tree_node(const image_t *, const tree_t *, uint64_t, bool,
    unsigned char *);
static void allocation_node(const image_t *, uint64_t, unsigned char *);
static void image_plan(image_t *, shape_t, uint64_t, unsigned);
static apfs_gen_node_t free_queue_node(uint64_t);
static apfs_gen_node_t volume_tree_node(uint64_t, uint32_t);
static void headers_build(image_t *);
static void fixture_check(const image_t *);
static void bitmap_prefix(unsigned char *, uint64_t, uint64_t);
static void emit_block(image_t *, uint64_t, unsigned char *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static unsigned char pattern(uint64_t);
static void content_check(fsimg_reader_t *, const image_t *);
static void require_ok(fsimg_status_t, const fsimg_error_t *);
static double seconds(void);
static fsimg_status_t extract_check(fsimg_ctx_t *, fsimg_io_t *,
    const char *, fsimg_error_t *);
static void setup_run(scale_run_t *, shape_t, uint64_t, unsigned, char *);
static void walk_image(scale_run_t *);
static void verify_image(scale_run_t *);
static void extract_image(scale_run_t *, const char *);
static void report_run(scale_run_t *);
static int run(shape_t, uint64_t, unsigned);

static void
cleanup_run_directory(void)
{
	/* Best effort: CHECK has already reported the failure. */
	if (cleanup_directory[0] != '\0')
		(void)rmdir(cleanup_directory);
}

static void
store(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t
ceiling(uint64_t value, uint64_t divisor)
{
	return (value / divisor + (value % divisor != 0));
}

static void
tree_plan(tree_t *t, uint64_t records, uint64_t base, unsigned leaf_slots)
{
	uint64_t count = ceiling(records, leaf_slots), span = leaf_slots;

	bzero(t, sizeof(*t));
	t->base = base;
	t->records = records;
	for (unsigned level = 0;; level++) {
		CHECK(level < LEVELS);
		t->first[level] = t->nodes;
		t->count[level] = count;
		t->span[level] = span;
		t->nodes += count;
		t->levels++;
		if (count == 1)
			break;
		count = ceiling(count, FANOUT);
		span *= FANOUT;
	}
}

static void
inode_record(const image_t *im, record_t *r, uint64_t oid)
{
	unsigned char fields[96], stream[DSTREAM_SIZE];
	char file_name[9];
	const char *name = "00000000";
	bool directory = oid <= PRIVATE_INODE;
	apfs_gen_dstream_t ds = { .size = im->file_size,
	    .alloced_size = im->data_blocks * BLOCK };
	apfs_gen_xfield_t xf[] = {
		{ .type = XFIELD_INODE_NAME, .flags = XFIELD_DO_NOT_COPY, },
		{ .type = XFIELD_INODE_DSTREAM, .data = stream,
		.size = sizeof(stream) }
	};
	apfs_gen_inode_val_t inode = {
	    .parent = oid <= PRIVATE_INODE ? ROOT_PARENT : ROOT_INODE,
	    .private_id = oid, .mode = directory ? 0040755 : 0100644,
	    .count = 1 };
	uint16_t count = directory ? 1 : 2;
	size_t length;

	if (oid == ROOT_INODE) {
		name = "root";
		inode.count = (int32_t)im->entries;
	} else if (oid == PRIVATE_INODE) {
		name = "private";
		inode.count = 0;
	} else {
		if (im->mode == NAMES)
			inode.count = (int32_t)im->entries;
		if (im->mode == WIDE) {
			CHECK(snprintf(file_name, sizeof(file_name),
			    "%08" PRIu64, oid - FIRST_FILE_INODE) == 8);
			name = file_name;
		}
	}
	xf[0].data = (const unsigned char *)name;
	xf[0].size = (uint16_t)(strlen(name) + 1);
	length = XFIELD_HEADER_SIZE + XFIELD_ENTRY_SIZE * count +
	    ceiling(xf[0].size, 8) * 8;

	if (!directory) {
		CHECK(apfs_gen_dstream_build(stream, sizeof(stream), &ds));
		length += sizeof(stream);
	}
	CHECK(apfs_gen_xfields_build(fields, length, xf, count));
	inode.xfields = fields;
	inode.xfields_len = length;
	r->key_size = 8;
	r->value_size = (uint16_t)(INODE_VALUE_BASE + length);
	CHECK(apfs_gen_jkey_build(r->key, 8, oid, RECORD_INODE));
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
}

static void
dstream_record(record_t *r, uint64_t oid)
{
	r->key_size = 8;
	r->value_size = 4;
	CHECK(apfs_gen_jkey_build(r->key, 8, oid, RECORD_DSTREAM_ID));
	CHECK(apfs_gen_dstream_id_val_build(r->value, 4, 1));
}

/* Decode the per-mode suffix after the root and private-directory records.
 * WIDE has inode/stream pairs. NAMES has one inode, sibling links, a stream
 * ID and sibling maps. Other modes have one inode, a stream ID and extents.
 */
static void
fs_suffix_record(const image_t *im, uint64_t ordinal, record_t *r)
{
	char name[9];
	uint64_t index, oid;

	if (im->mode == WIDE) {
		oid = FIRST_FILE_INODE + ordinal / 2;
		if (ordinal % 2 == 0)
			inode_record(im, r, oid);
		else
			dstream_record(r, oid);
		return;
	}
	if (ordinal == 0) {
		inode_record(im, r, FIRST_FILE_INODE);
		return;
	}
	if (im->mode == NAMES) {
		if (ordinal <= im->entries) {
			apfs_gen_sibling_val_t val = { .parent = ROOT_INODE,
			    .name = name, .name_len = 8 };

			index = ordinal - 1;
			CHECK(snprintf(name, sizeof(name), "%08" PRIu64,
			    index) == 8);
			r->key_size = SIBLING_KEY_SIZE;
			r->value_size = SIBLING_VALUE_SIZE;
			CHECK(apfs_gen_sibling_key_build(r->key, r->key_size,
			    FIRST_FILE_INODE, SIBLING_BASE + index));
			CHECK(apfs_gen_sibling_val_build(r->value,
			    r->value_size, &val));
		} else if (ordinal == im->entries + 1) {
			dstream_record(r, FIRST_FILE_INODE);
		} else {
			index = ordinal - im->entries - 2;
			r->key_size = 8;
			r->value_size = 8;
			CHECK(apfs_gen_jkey_build(r->key, r->key_size,
			    SIBLING_BASE + index, RECORD_SIBLING_MAP));
			CHECK(apfs_gen_sibling_map_val_build(r->value,
			    r->value_size, FIRST_FILE_INODE));
		}
		return;
	}
	if (ordinal == 1) {
		dstream_record(r, FIRST_FILE_INODE);
		return;
	}
	{
		apfs_gen_extent_val_t val = { .length = BLOCK };

		index = ordinal - 2;
		val.phys_block = im->payload + index / 2;
		if (im->mode == EXTENTS && index % 2 != 0)
			val.phys_block = 0;
		if (im->mode != EXTENTS) {
			val.length = im->file_size;
			val.phys_block = im->payload;
		}
		r->key_size = EXTENT_KEY_SIZE;
		r->value_size = EXTENT_VALUE_SIZE;
		CHECK(apfs_gen_extent_key_build(r->key, r->key_size,
		    FIRST_FILE_INODE, index * BLOCK));
		CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &val));
	}
}

/* Ordinals are already in (owner, kind, suffix) order. No array of records
 * or names is allocated, including for a million-entry directory.
 * The common prefix is root inode, root directory records, private inode.
 */
static void
fs_record(const image_t *im, uint64_t ordinal, record_t *r)
{
	char name[9];
	uint64_t index;

	bzero(r, sizeof(*r));
	if (ordinal == 0) {
		inode_record(im, r, ROOT_INODE);
		return;
	}
	if (ordinal <= im->entries) {
		apfs_gen_drec_key_t key = { .parent = ROOT_INODE, .name = name,
		    .name_len = 8 };
		apfs_gen_drec_val_t val = { .flags = DREC_REGULAR_FILE };
		unsigned char fields[16], id[8];
		apfs_gen_xfield_t xf = { .type = XFIELD_DREC_SIBLING_ID,
		    .size = 8, .data = id };

		index = ordinal - 1;
		CHECK(snprintf(name, sizeof(name), "%08" PRIu64, index) == 8);
		val.child = FIRST_FILE_INODE + (im->mode == WIDE ? index : 0);
		if (im->mode == NAMES) {
			store(id, SIBLING_BASE + index, 8);
			CHECK(apfs_gen_xfields_build(fields, sizeof(fields),
			    &xf, 1));
			val.xfields = fields;
			val.xfields_len = sizeof(fields);
		}
		r->key_size = DREC_KEY_SIZE;
		r->value_size = (uint16_t)(DREC_VALUE_BASE + val.xfields_len);
		CHECK(apfs_gen_drec_key_build(r->key, r->key_size, &key));
		CHECK(apfs_gen_drec_val_build(r->value, r->value_size, &val));
		return;
	}
	if (ordinal == im->entries + 1) {
		inode_record(im, r, PRIVATE_INODE);
		return;
	}
	fs_suffix_record(im, ordinal - im->entries - 2, r);
}

/* Count the same prefix and suffix segments decoded by fs_record(). */
static uint64_t
fs_record_count(const image_t *im)
{
	uint64_t prefix = 1 + im->entries + 1;
	uint64_t suffix;

	if (im->mode == WIDE)
		suffix = 2 * im->entries; /* Inode/stream pairs. */
	else if (im->mode == NAMES)
		suffix = 1 + im->entries + 1 + im->entries;
	else
		suffix = 1 + 1 + (im->mode == EXTENTS ? im->amount :
		    (im->file_size != 0));
	return (prefix + suffix);
}

static void
tree_node(const image_t *im, const tree_t *t, uint64_t number, bool map,
    unsigned char *raw)
{
	record_t owned[FANOUT];
	apfs_gen_record_t records[FANOUT];
	uint64_t index, first, ordinal, child, available;
	unsigned level = 0, count, slots;
	bool root = number + 1 == t->nodes;
	apfs_gen_node_t node = { .object = { .xid = FIXTURE_XID,
	    .subtype = map ? SUBTYPE_OMAP : SUBTYPE_FSTREE },
	    .info = { .flags = map ? (TREE_UINT64_KEYS |
	    TREE_SEQUENTIAL_INSERT |
	    TREE_ALLOW_GHOSTS | TREE_PHYSICAL) : TREE_KV_NONALIGNED,
	    .node_size = BLOCK,
	    .key_size = map ? OMAP_KEY_SIZE : 0,
	    .val_size = map ? OMAP_VALUE_SIZE : 0,
	    .longest_key = map ? 0 : DREC_KEY_SIZE,
	    .longest_val = map ? 0 : LONGEST_FS_VALUE,
	    .key_count = t->records, .node_count = t->nodes } };

	while (level + 1 < t->levels && number >= t->first[level + 1])
		level++;
	index = number - t->first[level];
	slots = map ? FANOUT : LEAF_RECORDS;
	if (level == 0)
		first = index * slots;
	else
		first = t->first[level - 1] + index * FANOUT;
	available = level == 0 ? t->records - first :
	    t->count[level - 1] - index * FANOUT;
	/* Deep fixtures have a single child per parent and one leaf. */
	if (!map && im->mode == DEEP && level != 0) {
		first = number - 1;
		available = 1;
	}
	if (level != 0)
		slots = FANOUT;
	count = (unsigned)(available < slots ? available : slots);
	bzero(owned, sizeof(owned));
	for (unsigned i = 0; i < count; i++) {
		child = first + i;
		ordinal = level == 0 ? child :
		    (child - t->first[level - 1]) * t->span[level - 1];
		if (map) {
			apfs_gen_omap_key_t key = {
			    .oid = FS_FIRST_OID + ordinal,
			    .xid = FIXTURE_XID };
			apfs_gen_omap_value_t val = { .size = BLOCK,
			    .address = im->fs.base + ordinal };

			owned[i].key_size = OMAP_KEY_SIZE;
			owned[i].value_size = OMAP_VALUE_SIZE;
			CHECK(apfs_gen_omap_key_build(owned[i].key,
			    owned[i].key_size, &key));
			CHECK(apfs_gen_omap_value_build(owned[i].value,
			    owned[i].value_size, &val));
		} else {
			if (im->mode == DEEP && level != 0)
				ordinal = 0;
			fs_record(im, ordinal, &owned[i]);
		}
		if (level != 0) {
			store(owned[i].value, (map ? t->base : FS_FIRST_OID) +
			    child, 8);
			owned[i].value_size = 8;
		}
		records[i] = (apfs_gen_record_t){ .key = owned[i].key,
		    .value = owned[i].value, .key_size = owned[i].key_size,
		    .value_size = owned[i].value_size };
	}
	node.object.oid = (map ? t->base : FS_FIRST_OID) + number;
	node.object.type = (map ? OBJECT_PHYSICAL : 0) |
	    (root ? KIND_BTREE_ROOT : KIND_BTREE_NODE);
	node.level = (uint16_t)level;
	node.flags = (root ? GEN_APFS_ROOT : 0) |
	    (level == 0 ? GEN_APFS_LEAF : 0) | (map ? GEN_APFS_FIXED : 0);
	node.records = records;
	node.count = count;
	CHECK(apfs_gen_node_build(raw, BLOCK, &node));
}

static void
image_plan(image_t *im, shape_t mode, uint64_t amount, unsigned height)
{
	uint64_t records;

	CHECK(height <= LEVELS + 1);
	bzero(im, sizeof(*im));
	im->mode = mode;
	im->amount = amount;
	im->entries = mode == WIDE || mode == NAMES ? amount : 1;
	switch (mode) {
	case EXTENTS:
		im->file_size = amount * BLOCK;
		break;
	case BIG_FILE:
		im->file_size = UINT64_C(5) << 30;
		break;
	case LARGE:
		im->file_size = BLOCK;
		break;
	default:
		break;
	}
	im->data_blocks = mode == EXTENTS ? ceiling(amount, 2) :
	    im->file_size / BLOCK;
	/* Non-large geometry is deliberately roomy, yet purely virtual. */
	im->blocks = mode == LARGE ? UINT64_C(1) << 35 : UINT64_C(1) << 24;
	im->sm_bytes = apfs_gen_spaceman_geometry(BLOCK, im->blocks,
	    &im->sm);
	CHECK(im->sm_bytes != 0 && im->sm_bytes <= 4 * BLOCK);
	CHECK(im->sm.cibs_per_cab == CAB_CAPACITY);
	CHECK(im->sm.chunks_per_cib == CIB_CAPACITY);
	im->sm.ip_bm_base = PREFIX_BLOCKS;
	im->sm.ip_base = PREFIX_BLOCKS + im->sm.ip_bm_block_count;
	im->volume = im->sm.ip_base + im->sm.ip_block_count;
	records = fs_record_count(im);
	tree_plan(&im->fs, records, im->volume + VOLUME_BLOCKS, LEAF_RECORDS);
	if (mode == DEEP) {
		im->fs.levels = height;
		im->fs.nodes = height;
		for (unsigned i = 0; i < height; i++) {
			im->fs.first[i] = i;
			im->fs.count[i] = 1;
			im->fs.span[i] = records;
		}
	}
	tree_plan(&im->map, im->fs.nodes, im->fs.base + im->fs.nodes, FANOUT);
	im->payload = im->map.base + im->map.nodes;
	im->allocated_end = im->payload + im->data_blocks;
	CHECK(im->allocated_end < im->blocks);
	im->bitmap_base = im->sm.ip_base + im->sm.devices[0].cab_count +
	    im->sm.devices[0].cib_count;
	im->bitmap_count = ceiling(im->allocated_end, 8 * BLOCK);
	im->pool_used = im->bitmap_base - im->sm.ip_base + im->bitmap_count;
	CHECK(im->pool_used <= im->sm.ip_block_count);
	im->sm.devices[0].free_count = im->blocks - im->allocated_end;
	im->cached_block = UINT64_MAX;
}

static apfs_gen_node_t
free_queue_node(uint64_t oid)
{
	return ((apfs_gen_node_t){ .object = { .oid = oid, .xid = FIXTURE_XID,
	    .type = OBJECT_EPHEMERAL | KIND_BTREE_ROOT,
	    .subtype = SUBTYPE_FREE_QUEUE },
	    .flags = GEN_APFS_ROOT | GEN_APFS_LEAF | GEN_APFS_FIXED,
	    .info = { .flags = TREE_EPHEMERAL, .node_size = BLOCK,
	    .key_size = 16, .val_size = 8, .node_count = 1 } });
}

static apfs_gen_node_t
volume_tree_node(uint64_t oid, uint32_t subtype)
{
	return ((apfs_gen_node_t){ .object = { .oid = oid, .xid = FIXTURE_XID,
	    .type = OBJECT_PHYSICAL | KIND_BTREE_ROOT, .subtype = subtype },
	    .flags = GEN_APFS_ROOT | GEN_APFS_LEAF,
	    .info = { .flags = TREE_PHYSICAL | TREE_KV_NONALIGNED,
	    .node_size = BLOCK, .node_count = 1 } });
}

/* A small generator-built container supplies rings and object headers. Its
 * NX geometry is replaced in both superblocks before exposing the callback.
 * The generator never receives the apparent size as an allocation length.
 */
static void
headers_build(image_t *im)
{
	size_t sm_bytes = im->sm_bytes;
	apfs_gen_mapping_t mappings[] = {
		{ .type = OBJECT_EPHEMERAL | KIND_SPACEMAN, .oid = SPACEMAN_OID,
		.address = SPACEMAN_BLOCK, .size = (uint32_t)sm_bytes },
		{ .type = OBJECT_EPHEMERAL | KIND_REAPER, .oid = REAPER_OID,
		.address = REAPER_BLOCK, .size = BLOCK },
		{ .type = OBJECT_EPHEMERAL | KIND_BTREE_ROOT,
		.subtype = SUBTYPE_FREE_QUEUE, .oid = IP_FREE_QUEUE_OID,
		.address = IP_FREE_QUEUE_BLOCK, .size = BLOCK },
		{ .type = OBJECT_EPHEMERAL | KIND_BTREE_ROOT,
		.subtype = SUBTYPE_FREE_QUEUE, .oid = MAIN_FREE_QUEUE_OID,
		.address = MAIN_FREE_QUEUE_BLOCK, .size = BLOCK }
	};
	apfs_gen_run_t run = { .xid = FIXTURE_XID, .next_xid = FIXTURE_XID + 1,
	    .mappings = mappings, .mapping_count = 4, .mapping_blocks = 1,
	    .desc_next = 2,
	    .data_len = MAIN_FREE_QUEUE_BLOCK + 1 - SPACEMAN_BLOCK,
	    .data_next = MAIN_FREE_QUEUE_BLOCK + 1 - SPACEMAN_BLOCK };
	apfs_gen_container_t container = {
		.nx = { .magic = NX_MAGIC, .block_size = BLOCK,
		.block_count = PREFIX_BLOCKS, .uuid = { 1 },
		.incompat_features = NX_INCOMPAT_VERSION2,
		.desc_base = 1, .desc_blocks = SPACEMAN_BLOCK - 1,
		.data_base = SPACEMAN_BLOCK,
		.data_blocks = PREFIX_BLOCKS - SPACEMAN_BLOCK,
		.max_volumes = 1, .volume_oids = { VOLUME_OID },
		.spaceman_oid = SPACEMAN_OID, .reaper_oid = REAPER_OID },
		.runs = &run, .count = 1, .block0_xid = FIXTURE_XID
	};
	apfs_gen_omap_record_t volume = {
	    .key = { .oid = VOLUME_OID, .xid = FIXTURE_XID },
	    .value = { .size = BLOCK, .address = im->volume + VOLUME_HEADER } };
	apfs_gen_tree_t tree = { .records = &volume, .count = 1, .base = 1,
	    .xid = FIXTURE_XID, .block_size = BLOCK,
	    .records_per_leaf = LEAF_RECORDS,
	    .children_per_index = 8 };
	apfs_gen_omap_t omap = { .object = { .oid = im->volume,
	    .xid = FIXTURE_XID, .type = OBJECT_PHYSICAL | KIND_OMAP },
	    .flags = 1,
	    .tree_type = OBJECT_PHYSICAL | KIND_BTREE_ROOT,
	    .tree_oid = im->volume + CONTAINER_OMAP_TREE };
	apfs_gen_fs_t fs = {
	    .object = { .oid = VOLUME_OID, .xid = FIXTURE_XID,
	    .type = KIND_FS },
	    .magic = FS_MAGIC, .uuid = { 2 }, .name = "Scale", .flags = 1,
	    .root_tree_type = KIND_BTREE_ROOT,
	    .root_tree_oid = FS_FIRST_OID + im->fs.nodes - 1,
	    .omap_oid = im->volume + VOLUME_OMAP_HEADER,
	    .extentref_tree_type = OBJECT_PHYSICAL | KIND_BTREE_ROOT,
	    .extentref_tree_oid = im->volume + EXTENTREF_TREE,
	    .snapshot_tree_type = OBJECT_PHYSICAL | KIND_BTREE_ROOT,
	    .snapshot_tree_oid = im->volume + SNAPMETA_TREE,
	    .files = im->mode == WIDE ? im->entries : 1,
	    .alloc_blocks = 3 + im->fs.nodes + im->map.nodes +
	    im->data_blocks };
	unsigned char *sm = im->prefix + SPACEMAN_BLOCK * BLOCK;
	uint64_t root, blocks;

	CHECK(sm_bytes != 0 && sm_bytes <= 4 * BLOCK);
	CHECK(apfs_gen_container_build(im->prefix, sizeof(im->prefix),
	    &container));
	for (unsigned i = 0; i < 2; i++) {
		unsigned char *nx = im->prefix + (i == 0 ? 0 : 2 * BLOCK);

		store(nx + NX_BLOCK_COUNT_OFFSET, im->blocks, 8);
		store(nx + NX_OMAP_OID_OFFSET, im->volume, 8);
		CHECK(apfs_gen_seal(nx, BLOCK));
	}
	im->sm.free_queues[0].tree_oid = IP_FREE_QUEUE_OID;
	im->sm.free_queues[1].tree_oid = MAIN_FREE_QUEUE_OID;
	im->sm.object = (apfs_gen_object_t){ .oid = SPACEMAN_OID,
	    .xid = FIXTURE_XID,
	    .type = OBJECT_EPHEMERAL | KIND_SPACEMAN };
	im->sm.ip_bm_free_head = (uint16_t)im->sm.ip_bm_size_in_blocks;
	im->sm.ip_bm_free_tail = (uint16_t)(im->sm.ip_bm_block_count - 1);
	CHECK(apfs_gen_spaceman_build(sm, sm_bytes, &im->sm));
	for (uint32_t i = 0; i < im->sm.ip_bm_size_in_blocks; i++) {
		store(sm + im->sm.ip_bm_xid_offset + 8 * i, FIXTURE_XID, 8);
		store(sm + im->sm.ip_bitmap_offset + 2 * i, i, 2);
	}
	for (uint32_t i = 0; i < im->sm.ip_bm_block_count; i++)
		store(sm + im->sm.ip_bm_free_next_offset + 2 * i,
		    i < im->sm.ip_bm_size_in_blocks ||
		    i + 1 == im->sm.ip_bm_block_count ?
		    GEN_APFS_FREE_LIST_EMPTY : i + 1, 2);
	for (uint32_t i = 0; i < (im->sm.devices[0].cab_count != 0 ?
	    im->sm.devices[0].cab_count : im->sm.devices[0].cib_count); i++)
		store(sm + im->sm.devices[0].addr_offset + 8 * i,
		    im->sm.ip_base + i, 8);
	CHECK(apfs_gen_seal(sm, sm_bytes));
	for (unsigned i = 0; i < 2; i++) {
		uint64_t oid = i == 0 ? IP_FREE_QUEUE_OID : MAIN_FREE_QUEUE_OID;
		uint64_t block = i == 0 ?
		    IP_FREE_QUEUE_BLOCK : MAIN_FREE_QUEUE_BLOCK;
		apfs_gen_node_t node = free_queue_node(oid);

		CHECK(apfs_gen_node_build(im->prefix + block * BLOCK, BLOCK,
		    &node));
	}
	CHECK(apfs_gen_tree_build(im->headers, 2 * BLOCK, &tree, &root,
	    &blocks));
	/* Relocate and seal the locally built physical leaf. */
	store(im->headers + CONTAINER_OMAP_TREE * BLOCK +
	    OBJECT_OID_OFFSET, im->volume + CONTAINER_OMAP_TREE, 8);
	CHECK(apfs_gen_seal(im->headers + CONTAINER_OMAP_TREE * BLOCK, BLOCK));
	CHECK(apfs_gen_omap_build(im->headers, BLOCK, &omap));
	CHECK(apfs_gen_fs_build(im->headers + VOLUME_HEADER * BLOCK,
	    BLOCK, &fs));
	omap.object.oid = im->volume + VOLUME_OMAP_HEADER;
	omap.flags = 0;
	omap.tree_oid = im->map.base + im->map.nodes - 1;
	CHECK(apfs_gen_omap_build(im->headers + VOLUME_OMAP_HEADER * BLOCK,
	    BLOCK, &omap));
	for (unsigned i = EXTENTREF_TREE; i <= SNAPMETA_TREE; i++) {
		record_t owned = {0};
		apfs_gen_record_t record;
		apfs_gen_pext_val_t val = { .length = im->data_blocks,
		    .owner = FIRST_FILE_INODE, .refcnt = 1, .kind = 1 };
		apfs_gen_node_t node = volume_tree_node(im->volume + i,
		    i == EXTENTREF_TREE ? SUBTYPE_EXTENTREF : SUBTYPE_SNAPMETA);

		node.count = i == EXTENTREF_TREE && im->data_blocks != 0 ?
		    1 : 0;
		node.info.key_count = node.count;
		CHECK(apfs_gen_jkey_build(owned.key, 8, im->payload,
		    RECORD_PHYS_EXTENT));
		CHECK(apfs_gen_pext_val_build(owned.value,
		    PHYS_EXTENT_VALUE_SIZE, &val));
		record = (apfs_gen_record_t){ .key = owned.key, .key_size = 8,
		    .value = owned.value,
		    .value_size = PHYS_EXTENT_VALUE_SIZE };
		node.records = &record;
		CHECK(apfs_gen_node_build(im->headers + i * BLOCK, BLOCK,
		    &node));
	}
}

/* Compare the small default's synthesized filesystem nodes byte for byte
 * against the general fixture builder. Only physical map addresses differ:
 * the virtual nodes contain OIDs, so relocation cannot change their bytes.
 */
static void
fixture_check(const image_t *im)
{
	record_t owned[3 * DEFAULT_FILES + 2];
	apfs_gen_record_t records[3 * DEFAULT_FILES + 2];
	apfs_gen_omap_record_t mappings[8];
	unsigned char image[9 * BLOCK] = {0}, node[BLOCK];
	apfs_gen_fs_tree_t tree = { .records = records, .count = im->fs.records,
	    .first_oid = FS_FIRST_OID, .base = 1, .xid = FIXTURE_XID,
	    .block_size = BLOCK,
	    .records_per_leaf = LEAF_RECORDS, .children_per_index = FANOUT };
	uint64_t root, blocks;

	CHECK(im->mode == WIDE && im->entries == DEFAULT_FILES);
	CHECK(im->fs.records <= sizeof(owned) / sizeof(owned[0]));
	for (uint64_t i = 0; i < im->fs.records; i++) {
		fs_record(im, i, &owned[i]);
		records[i] = (apfs_gen_record_t){ .key = owned[i].key,
		    .value = owned[i].value, .key_size = owned[i].key_size,
		    .value_size = owned[i].value_size };
	}
	CHECK(apfs_gen_fs_tree_build(image, sizeof(image), &tree, mappings,
	    8, &root, &blocks));
	CHECK(blocks == im->fs.nodes && root == FS_FIRST_OID + blocks - 1);
	for (uint64_t i = 0; i < blocks; i++) {
		bzero(node, sizeof(node));
		tree_node(im, &im->fs, i, false, node);
		CHECK(bcmp(node, image + (1 + i) * BLOCK, BLOCK) == 0);
	}
}

/* Set the low count bits; bytes past the represented range remain zero. */
static void
bitmap_prefix(unsigned char *raw, uint64_t first, uint64_t count)
{
	uint64_t bits = count > first ? count - first : 0;

	if (bits > 8 * BLOCK)
		bits = 8 * BLOCK;
	memset(raw, 255, (size_t)(bits / 8));
	if (bits % 8 != 0)
		raw[bits / 8] = (unsigned char)((1u << (bits % 8)) - 1);
}

static void
allocation_node(const image_t *im, uint64_t address, unsigned char *raw)
{
	uint64_t relative = address - im->sm.ip_base;
	uint64_t cabs = im->sm.devices[0].cab_count;
	uint64_t cibs = im->sm.devices[0].cib_count;

	if (relative < cabs) {
		uint64_t addresses[CAB_CAPACITY];
		uint64_t first = relative * CAB_CAPACITY;
		uint32_t count = (uint32_t)(cibs - first);
		apfs_gen_addresses_t cab = { .object = { .oid = address,
		    .xid = FIXTURE_XID,
		    .type = OBJECT_PHYSICAL | KIND_SPACEMAN_CAB },
		    .index = (uint32_t)relative,
		    .addresses = addresses };

		if (count > CAB_CAPACITY)
			count = CAB_CAPACITY;
		cab.count = count;
		for (unsigned i = 0; i < count; i++)
			addresses[i] = im->sm.ip_base + cabs + first + i;
		CHECK(apfs_gen_addresses_build(raw, BLOCK, &cab));
	} else if (relative < cabs + cibs) {
		apfs_gen_chunk_t chunks[CIB_CAPACITY];
		uint64_t index = relative - cabs, first = index * CIB_CAPACITY;
		uint32_t count = (uint32_t)
		    (im->sm.devices[0].chunk_count - first);
		apfs_gen_chunks_t cib = { .object = { .oid = address,
		    .xid = FIXTURE_XID,
		    .type = OBJECT_PHYSICAL | KIND_SPACEMAN_CIB },
		    .index = (uint32_t)index,
		    .chunks = chunks };

		if (count > CIB_CAPACITY)
			count = CIB_CAPACITY;
		cib.count = count;
		for (unsigned i = 0; i < count; i++) {
			uint64_t start = (first + i) * 8 * BLOCK;
			uint64_t used = im->allocated_end > start ?
			    im->allocated_end - start : 0;
			uint64_t size = im->blocks - start;

			if (size > 8 * BLOCK)
				size = 8 * BLOCK;
			if (used > size)
				used = size;
			chunks[i] = (apfs_gen_chunk_t){ .xid = FIXTURE_XID,
			    .address = start, .block_count = (uint32_t)size,
			    .free_count = (uint32_t)(size - used),
			    .bitmap_address = used != 0 ?
			    im->bitmap_base + first + i : 0 };
		}
		CHECK(apfs_gen_chunks_build(raw, BLOCK, &cib));
	} else if (address < im->bitmap_base + im->bitmap_count) {
		bitmap_prefix(raw, (address - im->bitmap_base) * 8 * BLOCK,
		    im->allocated_end);
	}
}

static unsigned char
pattern(uint64_t offset)
{
	uint64_t seed = offset / BLOCK;

	/* Each block carries all eight bytes of its mixed logical number,
	 * so a block substitution cannot hide behind an 8-bit period.
	 */
	seed ^= seed >> 33;
	seed *= UINT64_C(0xff51afd7ed558ccd);
	seed ^= seed >> 33;
	seed *= UINT64_C(0xc4ceb9fe1a85ec53);
	seed ^= seed >> 33;
	return ((unsigned char)((seed >> (8 * (offset % 8))) ^
	    (offset * 13) ^ 0x5a));
}

static void
emit_block(image_t *im, uint64_t block, unsigned char *raw)
{
	bzero(raw, BLOCK);
	if (block < PREFIX_BLOCKS)
		bcopy(im->prefix + block * BLOCK, raw, BLOCK);
	else if (block < im->sm.ip_base) {
		uint64_t slot = block - im->sm.ip_bm_base;

		if (slot < im->sm.ip_bm_size_in_blocks)
			bitmap_prefix(raw, slot * 8 * BLOCK, im->pool_used);
	} else if (block < im->volume)
		allocation_node(im, block, raw);
	else if (block < im->fs.base)
		bcopy(im->headers + (block - im->volume) * BLOCK, raw, BLOCK);
	else if (block < im->map.base) {
		tree_node(im, &im->fs, block - im->fs.base, false, raw);
		im->node_loads++;
		im->fs_loads++;
	} else if (block < im->payload) {
		tree_node(im, &im->map, block - im->map.base, true, raw);
		im->node_loads++;
	} else if (block < im->allocated_end) {
		uint64_t logical = (block - im->payload) * BLOCK;

		if (im->mode == EXTENTS)
			logical *= 2;
		for (unsigned i = 0; i < BLOCK; i++)
			raw[i] = pattern(logical + i);
	}
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	image_t *im = argument;
	size_t done = 0;

	(void)error;
	CHECK(offset <= im->blocks * BLOCK &&
	    length <= im->blocks * BLOCK - offset);
	im->reads++;
	im->bytes += length;
	while (done < length) {
		uint64_t block = offset / BLOCK;
		size_t within = (size_t)(offset % BLOCK), part = BLOCK - within;

		if (part > length - done)
			part = length - done;
		/* Count source block fetches, including requests for the cached
		 * block. The cache saves fixture encoding, not reader I/O.
		 */
		if (im->cached_block != block) {
			emit_block(im, block, im->cache);
			im->cached_block = block;
		} else if (block >= im->fs.base && block < im->payload) {
			im->node_loads++;
			im->fs_loads += block < im->map.base;
		}
		bcopy(im->cache + within, (unsigned char *)buffer + done, part);
		done += part;
		offset += part;
	}
	*transferred = done;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument, uint64_t *length, fsimg_error_t *error)
{
	(void)error;
	*length = ((image_t *)argument)->blocks * BLOCK;
	return (FSIMG_OK);
}

static void
require_ok(fsimg_status_t status, const fsimg_error_t *error)
{
	if (status != FSIMG_OK) {
		fprintf(stderr, "%s: %s (resource=%u limit=%" PRIu64
		    " observed=%" PRIu64 ")\n", fsimg_status_string(status),
		    error->message, (unsigned)error->resource, error->limit,
		    error->observed);
	}
	CHECK(status == FSIMG_OK);
}

/* Compare reader bytes with an independent logical-offset pattern. Hole records
 * are checked through read_at too; each EXTENTS range must reach EOF.
 * The big file probes both sides of 2^32 and its tail without streaming GiB.
 */
static void
content_check(fsimg_reader_t *reader, const image_t *im)
{
	unsigned char bytes[65536], expected;
	fsimg_error_t error = {0};
	fsimg_range_t range;
	uint64_t offset = 0;
	size_t got;

	while (offset < im->file_size) {
		size_t batch;

		require_ok(fsimg_reader_next_range(reader, offset, &range,
		    &error), &error);
		CHECK(range.offset == offset && range.length != 0 &&
		    range.length <= im->file_size - offset);
		/* Deliberately pin the common next_range contract: interior
		 * holes report DATA through EOF; read_at returns their zeros.
		 */
		if (im->mode == EXTENTS) {
			CHECK(range.kind == FSIMG_RANGE_DATA);
			CHECK(range.length == im->file_size - offset);
		}
		batch = range.length > sizeof(bytes) ?
		    sizeof(bytes) : (size_t)range.length;
		/* Query next_range at every hole boundary. */
		if (im->mode == EXTENTS && batch > BLOCK)
			batch = BLOCK;
		require_ok(fsimg_reader_read_at(reader, offset, bytes, batch,
		    &got, &error), &error);
		CHECK(got == batch);
		for (size_t i = 0; i < batch; i++) {
			bool hole = im->mode == EXTENTS &&
			    (offset + i) / BLOCK % 2 != 0;

			expected = hole ? 0 : pattern(offset + i);
			if (range.kind == FSIMG_RANGE_ZERO)
				CHECK(expected == 0);
			CHECK(bytes[i] == expected);
		}
		CHECK(range.kind == FSIMG_RANGE_ZERO ||
		    range.kind == FSIMG_RANGE_DATA);
		if (im->mode == BIG_FILE) {
			if (offset == 0)
				offset = (UINT64_C(1) << 32) - 17;
			else if (offset == (UINT64_C(1) << 32) - 17)
				offset = (UINT64_C(1) << 32) + 123;
			else if (offset == (UINT64_C(1) << 32) + 123)
				offset = im->file_size - sizeof(bytes);
			else
				offset = im->file_size;
		} else {
			offset += batch;
		}
	}
}

/* Only the small file in --large is extracted. The container itself remains
 * a callback source; the sole output file is exactly one block long.
 */
static fsimg_status_t
extract_check(fsimg_ctx_t *ctx, fsimg_io_t *io, const char *directory,
    fsimg_error_t *error)
{
	unsigned char bytes[BLOCK];
	fsimg_extract_options_t options;
	fsimg_reader_t *reader = NULL;
	fsimg_status_t status;
	struct stat st;
	int parent, output, file;

	parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	status = fsimg_reader_open(ctx, io, NULL, &reader, error);
	if (status == FSIMG_OK) {
		fsimg_extract_options_init(&options);
		status = fsimg_extract_at(reader, parent, "out", &options,
		    error);
	}
	fsimg_reader_free(reader);
	if (status == FSIMG_OK) {
		output = openat(parent, "out",
		    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(output != -1);
		file = openat(output, "00000000",
		    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(file != -1);
		CHECK(fstat(file, &st) == 0 && st.st_size == BLOCK);
		CHECK(read(file, bytes, sizeof(bytes)) == sizeof(bytes));
		for (unsigned i = 0; i < BLOCK; i++)
			CHECK(bytes[i] == pattern(i));
		CHECK(close(file) == 0);
		CHECK(unlinkat(output, "00000000", 0) == 0);
		CHECK(close(output) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
	}
	CHECK(close(parent) == 0);
	return (status);
}

static double
seconds(void)
{
	struct timespec stamp;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &stamp) == 0);
	return ((double)stamp.tv_sec + (double)stamp.tv_nsec / 1e9);
}

static void
setup_run(scale_run_t *r, shape_t mode, uint64_t amount, unsigned height,
    char *directory)
{
	image_t *im = r->im;
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	int scratch;

	image_plan(im, mode, amount, height);
	headers_build(im);
	if (mode == WIDE && amount == DEFAULT_FILES)
		fixture_check(im);
	CHECK(mkdtemp(directory) != NULL);
	bcopy(directory, cleanup_directory, sizeof(cleanup_directory));
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = SCALE_BUDGET;
	options.scratch_bytes = SCRATCH_LIMIT;
	options.max_entries = im->entries;
	options.max_work = UINT64_C(1000000000000);
	options.scratch_directory_fd = scratch;
	require_ok(fsimg_ctx_new(&options, &r->ctx, &r->error), &r->error);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_ctx_get_stats(r->ctx, &r->baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	require_ok(fsimg_io_from_callbacks(r->ctx, &ops, im, &r->io,
	    &r->error), &r->error);
}

static void
walk_image(scale_run_t *r)
{
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader = NULL;
	uint64_t before;

	r->status = fsimg_reader_open(r->ctx, r->io, NULL, &reader, &r->error);
	if (r->status == FSIMG_OK) {
		while (true) {
			before = r->im->node_loads;
			r->status = fsimg_reader_next(reader, &entry,
			    &r->error);
			if (r->status != FSIMG_OK)
				break;
			/* The first next() prepares the directory. Later calls
			 * measure replay lookups without that scan.
			 */
			if (r->entries != 0)
				r->replay_loads += r->im->node_loads - before;
			{
				char expected_path[10];

				CHECK(snprintf(expected_path,
				    sizeof(expected_path), "/%08" PRIu64,
				    r->entries) == 9);
				CHECK(strcmp(entry->path, expected_path) == 0);
			}
			r->entries++;
			CHECK(entry->kind == FSIMG_ENTRY_FILE &&
			    entry->size == r->im->file_size);
			if (r->im->file_size != 0)
				content_check(reader, r->im);
		}
	}
	r->rss = scale_peak_rss();
	fsimg_reader_free(reader);
}

static void
verify_image(scale_run_t *r)
{
	fsimg_reader_t *reader = NULL;
	fsimg_verify_report_t report = {0};
	shape_t mode = r->im->mode;

	r->verify_ran = true;
	r->verify = fsimg_reader_open(r->ctx, r->io, NULL, &reader, &r->error);
	if (r->verify == FSIMG_OK)
		r->verify = fsimg_verify(reader,
		    mode == EXTENTS || mode == LARGE ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, &r->error);
	if (r->verify != FSIMG_OK || !report.complete)
		r->result = 1;
	if (r->verify == FSIMG_OK) {
		CHECK(report.files == r->im->entries &&
		    report.directories == 1);
		CHECK(report.logical_bytes ==
		    r->im->entries * r->im->file_size);
		CHECK(report.free_clusters ==
		    r->im->blocks - r->im->allocated_end);
	}
	scale_rss_sample(&r->rss);
	fsimg_reader_free(reader);
}

static void
extract_image(scale_run_t *r, const char *directory)
{
	r->extract_ran = true;
	r->extracted = extract_check(r->ctx, r->io, directory, &r->error);
	if (r->extracted != FSIMG_OK)
		r->result = 1;
}

static void
report_run(scale_run_t *r)
{
	static const char *const labels[] = {
		[LARGE] = "large",
		[WIDE] = "wide",
		[EXTENTS] = "extents",
		[DEEP] = "deep",
		[NAMES] = "names",
		[BIG_FILE] = "big-file"
	};
	shape_t mode = r->im->mode;

	scale_rss_sample(&r->rss);
	CHECK(fsimg_ctx_get_stats(r->ctx, &r->stats, NULL) == FSIMG_OK);
	if (r->rss - r->rss_base > SCALE_BUDGET + RSS_ALLOWANCE) {
		fprintf(stderr, "%s: RSS growth exceeds allowance\n",
		    labels[mode]);
		r->result = 1;
	}
	if (r->stats.memory_peak > SCALE_BUDGET) {
		fprintf(stderr, "%s: memory peak exceeds budget\n",
		    labels[mode]);
		r->result = 1;
	}
	if (r->stats.scratch_peak > SCRATCH_LIMIT) {
		fprintf(stderr, "%s: scratch peak exceeds limit\n",
		    labels[mode]);
		r->result = 1;
	}
	if (mode == LARGE || mode == DEEP || mode == BIG_FILE)
		CHECK(r->stats.scratch_peak == 0);
	printf("apfs %s parameter=%" PRIu64 " height=%u entries=%" PRIu64
	    " reads=%" PRIu64 " read_bytes=%" PRIu64 " io_calls=%" PRIu64
	    " source_node_loads=%" PRIu64 " fs_node_loads=%" PRIu64
	    " lookup_source_loads_per_entry=%.3f memory_peak=%" PRIu64
	    " memory_limit=%" PRIu64 " entry_limit=%" PRIu64
	    " scratch_peak=%" PRIu64 " scratch_limit=%" PRIu64
	    " rss=%" PRIu64 " rss_growth=%" PRIu64 " seconds=%.3f"
	    " walk=%s verify=%s extract=%s result=%s\n",
	    labels[mode], r->im->amount, r->im->fs.levels, r->entries,
	    r->im->reads, r->im->bytes, r->stats.io_calls,
	    r->im->node_loads, r->im->fs_loads,
	    r->entries > 1 ?
	    (double)r->replay_loads / (double)(r->entries - 1) : 0,
	    r->stats.memory_peak, SCALE_BUDGET, r->im->entries,
	    r->stats.scratch_peak, SCRATCH_LIMIT, r->rss, r->rss - r->rss_base,
	    seconds() - r->start, fsimg_status_string(r->status),
	    r->verify_ran ? fsimg_status_string(r->verify) : "skipped",
	    r->extract_ran ? fsimg_status_string(r->extracted) : "skipped",
	    r->result == 0 ? "PASS" : "FAIL");
	CHECK(fflush(stdout) == 0);
	if (r->result != 0 && r->error.status != FSIMG_OK)
		fprintf(stderr, "%s: %s (resource=%u limit=%" PRIu64
		    " observed=%" PRIu64 ")\n", labels[mode], r->error.message,
		    (unsigned)r->error.resource, r->error.limit,
		    r->error.observed);
}

static int
run(shape_t mode, uint64_t amount, unsigned height)
{
	scale_run_t r = {0};
	fsimg_stats_t after;
	char directory[] = "apfs-scale-XXXXXX";
	int open_descriptors = fsimg_test_open_descriptors();

	r.im = calloc(1, sizeof(*r.im));
	CHECK(r.im != NULL);
	setup_run(&r, mode, amount, height, directory);
	r.rss_base = scale_peak_rss();
	r.start = seconds();
	walk_image(&r);
	if (mode == DEEP && height == LEVELS + 1) {
		if (r.status != FSIMG_EUNSUPPORTED ||
		    strstr(r.error.message, "taller than 16") == NULL)
			r.result = 1;
	} else if (r.status != FSIMG_DONE || r.entries != r.im->entries) {
		r.result = 1;
	} else {
		verify_image(&r);
	}
	if (mode == LARGE && r.result == 0)
		extract_image(&r, directory);
	report_run(&r);
	fsimg_io_free(r.io);
	CHECK(fsimg_ctx_get_stats(r.ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == r.baseline.memory_bytes &&
	    after.scratch_bytes == 0 &&
	    after.descriptors == r.baseline.descriptors);
	fsimg_ctx_free(r.ctx);
	CHECK(rmdir(directory) == 0);
	cleanup_directory[0] = '\0';
	free(r.im);
	CHECK(fsimg_test_open_descriptors() == open_descriptors);
	return (r.result);
}

int
main(int argc, char **argv)
{
	shape_t mode = WIDE;
	uint64_t amount = argc == 1 ? DEFAULT_FILES : DEFAULT_AMOUNT;
	char *end;

	CHECK(atexit(cleanup_run_directory) == 0);
	CHECK(argc <= 3);
	if (argc >= 2) {
		if (strcmp(argv[1], "--large") == 0)
			mode = LARGE;
		else if (strcmp(argv[1], "--wide") == 0)
			mode = WIDE;
		else if (strcmp(argv[1], "--extents") == 0)
			mode = EXTENTS;
		else if (strcmp(argv[1], "--deep") == 0)
			mode = DEEP;
		else if (strcmp(argv[1], "--names") == 0)
			mode = NAMES;
		else {
			CHECK(strcmp(argv[1], "--big-file") == 0);
			mode = BIG_FILE;
		}
	}
	if (argc == 3) {
		CHECK(mode == WIDE || mode == EXTENTS || mode == NAMES);
		errno = 0;
		amount = strtoull(argv[2], &end, 10);
		CHECK(errno == 0 && *argv[2] != '\0' && *end == '\0' &&
		    argv[2][0] != '-' && amount > 0 && amount <= MAX_AMOUNT);
	}
	if (mode == LARGE)
		amount = UINT64_C(1) << 35;
	if (mode == BIG_FILE)
		amount = UINT64_C(5) << 30;
	if (mode == DEEP) {
		int accepted = run(mode, LEVELS, LEVELS);
		int refused = run(mode, LEVELS + 1, LEVELS + 1);

		return (accepted != 0 || refused != 0);
	}
	return (run(mode, amount, 0));
}
