/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>

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

#include "fsimg.h"
#include "hfsplus-gen.h"
#include "scale.h"
#include "test.h"

#define NODE_BYTES 4096u
#define CATALOG_SLOTS 14u
#define EXTENT_SLOTS 50u
#define FANOUT 128u
#define GROUP_FILES 2048u
#define HEADER_BITS (8u * (NODE_BYTES - 256u))
#define MAP_BITS (8u * (NODE_BYTES - 20u))

#define MAX_DEPTH 256u

/* Cover reads that open and verification perform regardless of image size. */
#define METADATA_FIXED_READS 150u

/* Offsets and sizes: TN_ names B-tree node fields relative to the node;
 * VH_ names volume header fields relative to the header. */
enum {
	TN_FORWARD_LINK = 0, TN_BACKWARD_LINK = 4, TN_KIND = 8,
	TN_HEIGHT = 9, TN_RECORD_COUNT = 10, TN_DESCRIPTOR_BYTES = 14,
	TN_TREE_DEPTH = 14, TN_ROOT = 16, TN_LEAF_RECORDS = 20,
	TN_FIRST_LEAF = 24, TN_LAST_LEAF = 28, TN_NODE_SIZE = 32,
	TN_MAX_KEY_LENGTH = 34, TN_TOTAL_NODES = 36,
	TN_KEY_COMPARE_TYPE = 51, TN_ATTRIBUTES = 52,
	TN_USER_RECORD = 120, TN_MAP_RECORD = 248, TN_OFFSET_BYTES = 2
};

enum {
	VH_SIGNATURE = 0, VH_VERSION = 2, VH_ATTRIBUTES = 4,
	VH_FILE_COUNT = 32, VH_FOLDER_COUNT = 36, VH_BLOCK_SIZE = 40,
	VH_TOTAL_BLOCKS = 44, VH_FREE_BLOCKS = 48, VH_NEXT_CATALOG_ID = 64
};

/* Fourteen maximum-size catalog records need 14 * (24 + 248 + 2)
 * bytes; fifty overflow records need 50 * (12 + 64 + 2). Both leave
 * room for the descriptor and final offset. An index fits 128 keys,
 * each at most 24 bytes, plus child numbers and offsets. */
_Static_assert(14 + CATALOG_SLOTS * 274 + 2 <= NODE_BYTES,
    "catalog leaf packing");
_Static_assert(14 + EXTENT_SLOTS * 78 + 2 <= NODE_BYTES,
    "extents leaf packing");
_Static_assert(14 + FANOUT * 30 + 2 <= NODE_BYTES, "index packing");

/* Independent layouts: specs/design-hfsplus.md, Nodes and records, Catalog,
 * Fork data, and Allocation bitmap. No driver declarations are used here.
 * Leaves have fixed slot counts, so a record ordinal determines its node.
 * Index levels follow the leaves, bottom up; map nodes follow the root.
 */
typedef enum shape {
	CATALOG, WIDE, EXTENTS, DEEP, DEPTH_LIMIT, CAPACITY
} shape_t;

typedef struct tree {
	uint32_t records;
	uint32_t slots;
	uint32_t leaves;
	uint32_t used;
	uint32_t nodes;
	uint32_t root;
	uint32_t depth;
	uint32_t block;
} tree_t;

typedef struct image {
	shape_t shape;
	uint32_t files;
	uint32_t folders;
	uint32_t extents;
	uint32_t blocks;
	uint32_t block_size;
	uint32_t bitmap_blocks;
	uint32_t metadata_end;
	uint32_t payload;
	uint32_t free_blocks;
	tree_t catalog;
	tree_t overflow;
	uint64_t reads;
	uint64_t metadata_reads;
	/* The next unseen bitmap byte proves contiguous coverage, not merely
	 * an aggregate byte count that repeated reads could satisfy. */
	uint64_t bitmap_seen;
	uint64_t cached_offset;
	unsigned char node[NODE_BYTES];
} image_t;

/* A primary and its thread describe the same object. Only these integers
 * are needed to encode either; the catalog itself is never retained. */
typedef struct object {
	uint32_t id;
	uint32_t parent;
	uint32_t name;
	uint32_t children;
	bool folder;
	bool thread;
} object_t;

typedef struct measurement {
	uint64_t reads;
	uint64_t metadata_reads;
	uint64_t io_calls;
	double elapsed;
} measurement_t;

typedef struct pass_ceiling {
	unsigned walk;
	unsigned verify;
} pass_ceiling_t;

typedef struct walk_result {
	uint64_t entries;
	uint64_t files;
	uint64_t folders;
	uint64_t path_sum;
	fsimg_status_t status;
	fsimg_error_t error;
	measurement_t measurement;
} walk_result_t;

typedef struct verify_result {
	fsimg_status_t status;
	fsimg_error_t error;
	fsimg_verify_report_t report;
	measurement_t measurement;
} verify_result_t;

static void
store(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[width - 1 - i] = (unsigned char)(value >> (8 * i));
}

static uint32_t
ceiling(uint64_t value, uint32_t divisor)
{
	uint64_t result = (value + divisor - 1) / divisor;

	CHECK(result <= UINT32_MAX);
	return ((uint32_t)result);
}

static double
seconds(void)
{
	struct timespec stamp;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &stamp) == 0);
	return ((double)stamp.tv_sec + (double)stamp.tv_nsec / 1e9);
}

static void
require_ok(fsimg_status_t status, const fsimg_error_t *error)
{
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: %s (resource=%u limit=%" PRIu64
		    " observed=%" PRIu64 ")\n", fsimg_status_string(status),
		    error->message, (unsigned)error->resource, error->limit,
		    error->observed);
	CHECK(status == FSIMG_OK);
}

static tree_t
plan_tree(uint32_t records, uint32_t slots)
{
	tree_t tree = { .records = records, .slots = slots, .used = 1 };
	uint32_t count, maps = 0, required;

	tree.leaves = ceiling(records, slots);
	count = tree.leaves;
	while (count != 0) {
		tree.root = tree.used;
		tree.used += count;
		tree.depth++;
		if (count == 1)
			break;
		count = ceiling(count, FANOUT);
	}
	/* Maps must cover themselves as well as the tree levels. */
	do {
		required = maps;
		if (tree.used + maps > HEADER_BITS)
			required = ceiling(tree.used + maps - HEADER_BITS, MAP_BITS);
		if (required == maps)
			break;
		maps = required;
	} while (true);
	tree.nodes = tree.used + maps;
	return (tree);
}

static void
plan_image(image_t *image, shape_t shape, uint32_t amount)
{
	uint32_t reserved, tail, bitmap, blocks;
	uint64_t minimum;

	bzero(image, sizeof(*image));
	image->shape = shape;
	image->files = shape == CATALOG || shape == WIDE ? amount : 1;
	if (shape == CATALOG)
		image->folders = ceiling(amount, GROUP_FILES);
	if (shape == DEEP || shape == DEPTH_LIMIT)
		/* The bottom file contributes one level after the folder chain. */
		image->folders = amount - 1;
	image->extents = shape == EXTENTS ? amount : (shape == CAPACITY ? 1 : 0);
	image->block_size = shape == CAPACITY ? 4096 : 512;
	reserved = ceiling(1536, image->block_size);
	tail = ceiling(1024, image->block_size);
	image->catalog = plan_tree(2 + 2 * image->files + 2 * image->folders,
	    CATALOG_SLOTS);
	image->overflow = plan_tree(image->extents > 8 ?
	    ceiling(image->extents - 8, 8) : 0, EXTENT_SLOTS);
	minimum = reserved + (uint64_t)(image->catalog.nodes +
	    image->overflow.nodes) * NODE_BYTES / image->block_size + tail + 16;
	if (image->extents != 0)
		minimum += 2 * (uint64_t)image->extents - 1;
	/* The bitmap occupies blocks whose allocation bits enlarge the bitmap;
	 * iterate until its block count covers the resulting volume. */
	bitmap = 1;
	do {
		blocks = shape == CAPACITY ? UINT32_MAX :
		    ceiling(minimum + bitmap, 1);
		image->bitmap_blocks = bitmap;
		bitmap = ceiling(ceiling(blocks, 8), image->block_size);
	} while (bitmap != image->bitmap_blocks);
	image->blocks = blocks;
	image->overflow.block = reserved + bitmap;
	image->catalog.block = image->overflow.block +
	    image->overflow.nodes * (NODE_BYTES / image->block_size);
	image->metadata_end = image->catalog.block +
	    image->catalog.nodes * (NODE_BYTES / image->block_size);
	image->payload = shape == CAPACITY ? blocks - tail - 1 : image->metadata_end;
	image->free_blocks = blocks - image->metadata_end - image->extents - tail;
	image->cached_offset = UINT64_MAX;
}

static uint32_t
folder_files(const image_t *image, uint32_t folder)
{
	uint32_t remaining = image->files - folder * GROUP_FILES;

	return (remaining < GROUP_FILES ? remaining : GROUP_FILES);
}

/* The catalog sorts by (parent CNID, name). A thread's key is its own CNID and an
 * empty name, so it precedes the primaries of that folder's children.
 */
static object_t
catalog_object(const image_t *image, uint32_t ordinal)
{
	object_t object = { .id = 2, .parent = 1, .folder = true };
	uint32_t group, position, index;

	if (ordinal < 2) {
		object.thread = ordinal == 1;
		object.children = image->folders != 0 ? image->folders : image->files;
		if (image->shape == DEEP || image->shape == DEPTH_LIMIT)
			object.children = 1;
		return (object);
	}
	ordinal -= 2;
	if (image->shape == DEEP || image->shape == DEPTH_LIMIT) {
		index = ordinal / 2;
		object.id = 16 + index;
		object.parent = index == 0 ? 2 : object.id - 1;
		object.name = index;
		object.folder = index < image->folders;
		object.children = object.folder ? 1 : 0;
		object.thread = ordinal % 2 != 0;
		return (object);
	}
	if (ordinal < image->folders) {
		object.id = 16 + ordinal;
		object.parent = 2;
		object.name = ordinal;
		object.children = folder_files(image, ordinal);
		return (object);
	}
	ordinal -= image->folders;
	if (image->folders != 0 && ordinal < image->folders + image->files) {
		/* Each full region is its folder thread and 2048 file primaries.
		 * Only the final region can be short. */
		group = ordinal / (GROUP_FILES + 1);
		position = ordinal % (GROUP_FILES + 1);
		if (position == 0) {
			object.id = 16 + group;
			object.parent = 2;
			object.name = group;
			object.thread = true;
			return (object);
		}
		index = group * GROUP_FILES + position - 1;
	} else if (image->folders == 0 && ordinal < image->files) {
		index = ordinal;
	} else {
		index = ordinal - image->folders - image->files;
		object.thread = true;
	}
	object.id = 16 + image->folders + index;
	object.parent = image->folders == 0 ? 2 : 16 + index / GROUP_FILES;
	object.name = index;
	object.folder = false;
	return (object);
}

static size_t
name_bytes(const image_t *image, const object_t *object, unsigned char *p)
{
	char digits[9];
	size_t length = 8;

	CHECK(snprintf(digits, sizeof(digits), "%08" PRIu32, object->name) == 8);
	if (image->shape == CAPACITY && !object->folder)
		length = 5;
	store(p, length, 2);
	for (size_t i = 0; i < length; i++)
		store(p + 2 + 2 * i, length == 5 ? "hello"[i] : digits[i], 2);
	return (2 + 2 * length);
}

static uint64_t
path_hash(const char *path, bool folder)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (; *path != '\0'; path++) {
		hash ^= (unsigned char)*path;
		hash *= UINT64_C(1099511628211);
	}
	hash ^= (unsigned char)folder;
	hash *= UINT64_C(1099511628211);
	return (hash);
}

static uint64_t
expected_path_sum(const image_t *image)
{
	/* Walk order differs from catalog order, so sum the path/kind hashes.
	 * Use only the generator, never the reader. Folder primaries extend
	 * the deep chain; a folder thread contributes no hash and restores
	 * the parent prefix before its children in the grouped catalog. */
	/* At most MAX_DEPTH components, each a slash and eight ASCII bytes. */
	char path[MAX_DEPTH * 9 + 1] = {0};
	unsigned char name[18];
	uint32_t folder = 2;
	size_t prefix = 0, length, end;
	uint64_t sum = 0;

	for (uint32_t i = 0; i < image->catalog.records; i++) {
		object_t object = catalog_object(image, i);

		if (object.id == 2 || (object.thread && !object.folder))
			continue;
		if (object.thread && object.id == folder)
			continue;
		if (object.parent == 2)
			prefix = 0;
		else
			CHECK(object.parent == folder);
		length = (name_bytes(image, &object, name) - 2) / 2;
		end = prefix + 1 + length;
		CHECK(end < sizeof(path));
		path[prefix] = '/';
		for (size_t j = 0; j < length; j++) {
			CHECK(name[2 + 2 * j] == 0);
			path[prefix + 1 + j] = (char)name[3 + 2 * j];
		}
		path[end] = '\0';
		if (!object.thread)
			sum += path_hash(path, object.folder);
		if (object.folder) {
			folder = object.id;
			prefix = end;
		}
	}
	return (sum);
}

static size_t
catalog_key(const image_t *image, uint32_t ordinal, unsigned char *p)
{
	object_t object = catalog_object(image, ordinal);
	size_t length = 8;

	store(p + 2, object.thread ? object.id : object.parent, 4);
	if (!object.thread)
		length = 6 + name_bytes(image, &object, p + 6);
	store(p, length - 2, 2);
	return (length);
}

static void
contiguous_fork(unsigned char *p, uint64_t bytes, uint32_t first,
    uint32_t block_size)
{
	uint32_t blocks = ceiling(bytes, block_size);

	store(p, bytes, 8);
	store(p + 12, blocks, 4);
	store(p + 16, first, 4);
	store(p + 20, blocks, 4);
}

static void
file_fork(const image_t *image, unsigned char *p)
{
	store(p, image->shape == CAPACITY ? 5 : (uint64_t)image->extents * 512, 8);
	store(p + 12, image->extents, 4);
	for (uint32_t i = 0; i < image->extents && i < 8; i++) {
		store(p + 16 + 8 * i, image->payload + 2 * i, 4);
		store(p + 20 + 8 * i, 1, 4);
	}
}

static size_t
catalog_record(const image_t *image, uint32_t ordinal, unsigned char *p)
{
	object_t object = catalog_object(image, ordinal);
	size_t key = catalog_key(image, ordinal, p), body;
	unsigned char *data = p + key;

	if (object.thread) {
		store(data, object.folder ? 3 : 4, 2);
		store(data + 4, object.parent, 4);
		body = 8 + name_bytes(image, &object, data + 8);
	} else {
		store(data, object.folder ? 1 : 2, 2);
		store(data + 8, object.id, 4);
		if (object.folder)
			store(data + 4, object.children, 4);
		else if (image->extents != 0)
			file_fork(image, data + 88);
		body = object.folder ? 88 : 248;
	}
	return (key + body);
}

static size_t
extent_key(const image_t *image, uint32_t ordinal, unsigned char *p)
{
	store(p, 10, 2);
	store(p + 4, 16 + image->folders, 4);
	store(p + 8, 8 + 8 * ordinal, 4);
	return (12);
}

static size_t
extent_record(const image_t *image, uint32_t ordinal, unsigned char *p)
{
	uint32_t first = 8 + 8 * ordinal;

	extent_key(image, ordinal, p);
	for (uint32_t i = 0; i < 8 && first + i < image->extents; i++) {
		store(p + 12 + 8 * i, image->payload + 2 * (first + i), 4);
		store(p + 16 + 8 * i, 1, 4);
	}
	return (76);
}

static void
map_record(unsigned char *p, uint32_t first, uint32_t length, uint32_t nodes)
{
	for (uint32_t i = 0; i < length && first + 8 * i < nodes; i++) {
		uint32_t bits = nodes - first - 8 * i;

		p[i] = bits >= 8 ? 255 : (unsigned char)(255u << (8 - bits));
	}
}

static void
emit_node(const image_t *image, const tree_t *tree, bool catalog,
    uint32_t number, unsigned char *p)
{
	uint32_t first, count, lower_first, lower_count, span, height, index;
	uint32_t child, ordinal, records;
	size_t position = TN_DESCRIPTOR_BYTES, key;

	bzero(p, NODE_BYTES);
	if (number == 0) {
		p[TN_KIND] = 1;
		store(p + TN_FORWARD_LINK, tree->nodes > tree->used ? tree->used : 0, 4);
		store(p + TN_RECORD_COUNT, 3, 2);
		store(p + NODE_BYTES - TN_OFFSET_BYTES, TN_DESCRIPTOR_BYTES, 2);
		store(p + NODE_BYTES - 2 * TN_OFFSET_BYTES, TN_USER_RECORD, 2);
		store(p + NODE_BYTES - 3 * TN_OFFSET_BYTES, TN_MAP_RECORD, 2);
		store(p + NODE_BYTES - 4 * TN_OFFSET_BYTES,
		    NODE_BYTES - 4 * TN_OFFSET_BYTES, 2);
		store(p + TN_TREE_DEPTH, tree->depth, 2);
		store(p + TN_ROOT, tree->root, 4);
		store(p + TN_LEAF_RECORDS, tree->records, 4);
		store(p + TN_FIRST_LEAF, tree->leaves != 0 ? 1 : 0, 4);
		store(p + TN_LAST_LEAF, tree->leaves, 4);
		store(p + TN_NODE_SIZE, NODE_BYTES, 2);
		store(p + TN_MAX_KEY_LENGTH, catalog ? 516 : 10, 2);
		store(p + TN_TOTAL_NODES, tree->nodes, 4);
		p[TN_KEY_COMPARE_TYPE] = catalog ? 0xcf : 0;
		store(p + TN_ATTRIBUTES, catalog ? 6 : 2, 4);
		map_record(p + TN_MAP_RECORD, 0, HEADER_BITS / 8, tree->nodes);
		return;
	}
	if (number >= tree->used) {
		p[TN_KIND] = 2;
		store(p + TN_FORWARD_LINK, number + 1 < tree->nodes ? number + 1 : 0, 4);
		store(p + TN_RECORD_COUNT, 1, 2);
		store(p + NODE_BYTES - TN_OFFSET_BYTES, TN_DESCRIPTOR_BYTES, 2);
		store(p + NODE_BYTES - 2 * TN_OFFSET_BYTES,
		    NODE_BYTES - 3 * TN_OFFSET_BYTES, 2);
		map_record(p + TN_DESCRIPTOR_BYTES,
		    HEADER_BITS + (number - tree->used) * MAP_BITS, MAP_BITS / 8,
		    tree->nodes);
		return;
	}
	first = 1;
	count = tree->leaves;
	lower_first = 0;
	lower_count = 0;
	span = tree->slots;
	height = 1;
	while (number >= first + count) {
		lower_first = first;
		lower_count = count;
		first += count;
		count = ceiling(count, FANOUT);
		height++;
		if (height > 2)
			span *= FANOUT;
	}
	index = number - first;
	store(p + TN_FORWARD_LINK, index + 1 < count ? number + 1 : 0, 4);
	store(p + TN_BACKWARD_LINK, index != 0 ? number - 1 : 0, 4);
	p[TN_KIND] = height == 1 ? 255 : 0;
	p[TN_HEIGHT] = height;
	records = height == 1 ? tree->records - index * tree->slots :
	    lower_count - index * FANOUT;
	if (records > (height == 1 ? tree->slots : FANOUT))
		records = height == 1 ? tree->slots : FANOUT;
	store(p + TN_RECORD_COUNT, records, 2);
	for (uint32_t i = 0; i < records; i++) {
		store(p + NODE_BYTES - TN_OFFSET_BYTES * (i + 1), position, 2);
		if (height == 1) {
			ordinal = index * tree->slots + i;
			position += catalog ? catalog_record(image, ordinal, p + position) :
			    extent_record(image, ordinal, p + position);
		} else {
			child = lower_first + index * FANOUT + i;
			ordinal = (index * FANOUT + i) * span;
			key = catalog ? catalog_key(image, ordinal, p + position) :
			    extent_key(image, ordinal, p + position);
			store(p + position + key, child, 4);
			position += key + 4;
		}
	}
	CHECK(position + TN_OFFSET_BYTES * (records + 1) <= NODE_BYTES);
	store(p + NODE_BYTES - TN_OFFSET_BYTES * (records + 1), position, 2);
}

static void
volume_header(const image_t *image, unsigned char *p)
{
	bzero(p, NODE_BYTES);
	store(p + VH_SIGNATURE, 0x482b, 2);
	store(p + VH_VERSION, 4, 2);
	store(p + VH_ATTRIBUTES, 0x100, 4);
	store(p + VH_FILE_COUNT, image->files, 4);
	store(p + VH_FOLDER_COUNT, image->folders, 4);
	store(p + VH_BLOCK_SIZE, image->block_size, 4);
	store(p + VH_TOTAL_BLOCKS, image->blocks, 4);
	store(p + VH_FREE_BLOCKS, image->free_blocks, 4);
	store(p + VH_NEXT_CATALOG_ID, 16 + image->folders + image->files, 4);
	contiguous_fork(p + GEN_BITMAP_FORK, ceiling(image->blocks, 8),
	    ceiling(1536, image->block_size), image->block_size);
	contiguous_fork(p + GEN_EXTENTS_FORK, (uint64_t)image->overflow.nodes * NODE_BYTES,
	    image->overflow.block, image->block_size);
	contiguous_fork(p + GEN_CATALOG_FORK, (uint64_t)image->catalog.nodes * NODE_BYTES,
	    image->catalog.block, image->block_size);
}

static unsigned char
bitmap_byte(const image_t *image, uint64_t offset)
{
	uint64_t first = offset * 8;
	uint32_t tail = image->blocks - ceiling(1024, image->block_size);
	unsigned char byte = 0;

	if (first + 8 <= image->metadata_end)
		return (255);
	if (first >= image->metadata_end && first + 8 <= image->payload)
		return (0);
	if (image->shape == EXTENTS && first >= image->payload &&
	    first + 8 <= image->payload + 2 * (uint64_t)image->extents - 1)
		return ((first - image->payload) % 2 == 0 ? 0xaa : 0x55);
	if (first >= image->metadata_end && first + 8 <= tail &&
	    first >= image->payload + 2 * (uint64_t)image->extents)
		return (0);
	for (unsigned i = 0; i < 8 && first + i < image->blocks; i++) {
		uint64_t block = first + i;
		bool payload = block >= image->payload &&
		    block - image->payload < 2 * (uint64_t)image->extents &&
		    (block - image->payload) % 2 == 0;

		if (block < image->metadata_end || block >= tail || payload)
			byte |= (unsigned char)(0x80u >> i);
	}
	return (byte);
}

static unsigned char
payload_byte(uint64_t offset)
{
	return ((unsigned char)(offset ^ (offset >> 9) ^ (offset >> 17)));
}

static fsimg_status_t
image_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	image_t *image = argument;

	(void)error;
	*size = (uint64_t)image->blocks * image->block_size;
	return (FSIMG_OK);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	image_t *image = argument;
	unsigned char *out = buffer;
	uint64_t size, block, base, bitmap, logical, node_offset;
	const tree_t *tree;
	size_t part, copied = 0;

	(void)error;
	size = (uint64_t)image->blocks * image->block_size;
	CHECK(offset <= size && length <= size - offset);
	image->reads++;
	bitmap = (uint64_t)ceiling(1536, image->block_size) * image->block_size;
	while (copied < length) {
		/* Work in sectors so neither header can straddle this piece.
		 * Tree generation caches just one node across callback calls. */
		part = 512 - offset % 512;
		if (part > length - copied)
			part = length - copied;
		block = offset / image->block_size;
		base = offset - offset % 512;
		bzero(out + copied, part);
		if (base == 1024 || base == size - 1024) {
			volume_header(image, image->node);
			image->cached_offset = UINT64_MAX;
			bcopy(image->node + offset - base, out + copied, part);
		} else if (offset >= bitmap && offset < bitmap + ceiling(image->blocks, 8)) {
			logical = offset - bitmap;
			for (size_t i = 0; i < part; i++) {
				if (logical + i < ceiling(image->blocks, 8))
					out[copied + i] = bitmap_byte(image, logical + i);
			}
			if (logical <= image->bitmap_seen && logical + part > image->bitmap_seen)
				image->bitmap_seen = logical + part;
			image->metadata_reads++;
		} else if (block >= image->overflow.block && block < image->metadata_end) {
			tree = block >= image->catalog.block ? &image->catalog : &image->overflow;
			logical = offset - (uint64_t)tree->block * image->block_size;
			node_offset = (uint64_t)tree->block * image->block_size +
			    logical / NODE_BYTES * NODE_BYTES;
			if (node_offset != image->cached_offset) {
				emit_node(image, tree, tree == &image->catalog,
				    logical / NODE_BYTES, image->node);
				image->cached_offset = node_offset;
			}
			bcopy(image->node + logical % NODE_BYTES, out + copied, part);
			image->metadata_reads++;
		} else if (image->extents != 0 && block >= image->payload &&
		    block - image->payload < 2 * (uint64_t)image->extents &&
		    (block - image->payload) % 2 == 0) {
			logical = (block - image->payload) / 2 * image->block_size +
			    offset % image->block_size;
			for (size_t i = 0; i < part; i++)
				out[copied + i] = image->shape == CAPACITY ?
				    (logical + i < 5 ? "hello"[logical + i] : 0) : payload_byte(logical + i);
		}
		copied += part;
		offset += part;
	}
	*count = copied;
	return (FSIMG_OK);
}

static fsimg_status_t
check_content(fsimg_reader_t *reader, const image_t *image, fsimg_error_t *error)
{
	unsigned char bytes[65536];
	uint64_t size = image->shape == CAPACITY ? 5 : (uint64_t)image->extents * 512;
	uint64_t offset;
	size_t count, length;
	fsimg_status_t status;

	for (offset = 0; offset < size; offset += count) {
		length = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
		status = fsimg_reader_read_at(reader, offset, bytes, length, &count, error);
		if (status != FSIMG_OK)
			return (status);
		CHECK(count == length);
		for (size_t i = 0; i < count; i++)
			CHECK(bytes[i] == (image->shape == CAPACITY ?
			    (unsigned char)"hello"[offset + i] : payload_byte(offset + i)));
	}
	if (image->shape != EXTENTS)
		return (FSIMG_OK);
	/* Reverse order probes touch both sides of the inline/overflow boundary,
	 * B-tree leaf boundaries, and the final extent, independently of replay. */
	for (uint32_t i = 0; i < 67; i++) {
		uint32_t extent = i >= 64 ? 8 + (i - 64) * EXTENT_SLOTS * 8 :
		    (uint64_t)(64 - i) * (image->extents - 1) / 64;

		offset = (uint64_t)extent * 512;
		if (offset == 0 || offset >= size)
			continue;
		status = fsimg_reader_read_at(reader, offset - 1, bytes, 2, &count, error);
		if (status != FSIMG_OK)
			return (status);
		CHECK(count == 2 && bytes[0] == payload_byte(offset - 1) &&
		    bytes[1] == payload_byte(offset));
	}
	return (FSIMG_OK);
}

static measurement_t
measurement(const image_t *image, fsimg_ctx_t *ctx, double start)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return ((measurement_t){ .reads = image->reads,
	    .metadata_reads = image->metadata_reads, .io_calls = stats.io_calls,
	    .elapsed = seconds() - start });
}

static void
setup_context_io(image_t *image, const char *directory, fsimg_ctx_t **ctx,
    fsimg_io_t **io, fsimg_stats_t *baseline)
{
	fsimg_options_t limits;
	fsimg_io_ops_t ops;
	fsimg_error_t error = {0};
	int scratch;

	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = SCALE_BUDGET;
	limits.max_entries = 4 * (uint64_t)(image->files + image->folders) + 1024;
	limits.max_work = UINT64_C(1000000000000);
	limits.max_depth = MAX_DEPTH;
	limits.scratch_directory_fd = scratch;
	require_ok(fsimg_ctx_new(&limits, ctx, &error), &error);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_ctx_get_stats(*ctx, baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	require_ok(fsimg_io_from_callbacks(*ctx, &ops, image, io, &error), &error);
}

static walk_result_t
walk_image(image_t *image, fsimg_ctx_t *ctx, fsimg_io_t *io, uint64_t *rss)
{
	walk_result_t result = {0};
	fsimg_reader_t *reader = NULL;
	fsimg_read_options_t options;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	double start = seconds();

	fsimg_read_options_init(&options);
	status = fsimg_reader_open(ctx, io, &options, &reader, &result.error);
	if (status == FSIMG_OK) {
		while ((status = fsimg_reader_next(reader, &entry, &result.error)) == FSIMG_OK) {
			result.entries++;
			result.path_sum += path_hash(entry->path,
			    entry->kind == FSIMG_ENTRY_DIRECTORY);
			if (entry->kind == FSIMG_ENTRY_DIRECTORY)
				result.folders++;
			else {
				CHECK(entry->kind == FSIMG_ENTRY_FILE);
				CHECK(entry->size == (image->shape == CAPACITY ? 5 :
				    (uint64_t)image->extents * 512));
				result.files++;
				if (image->extents != 0) {
					status = check_content(reader, image, &result.error);
					if (status != FSIMG_OK)
						break;
				}
			}
		}
	}
	result.status = status;
	result.measurement = measurement(image, ctx, start);
	*rss = scale_peak_rss();
	fsimg_reader_free(reader);
	return (result);
}

static verify_result_t
verify_image(image_t *image, fsimg_ctx_t *ctx, fsimg_io_t *io,
    uint64_t walk_io_calls, uint64_t *rss)
{
	verify_result_t result = {0};
	fsimg_reader_t *reader = NULL;
	fsimg_read_options_t options;
	/* Extents exercise payload traversal; other shapes stress metadata. */
	unsigned flags = image->shape == EXTENTS ? FSIMG_VERIFY_PAYLOAD : 0;
	double start;

	image->reads = 0;
	image->metadata_reads = 0;
	image->bitmap_seen = 0;
	image->cached_offset = UINT64_MAX;
	fsimg_read_options_init(&options);
	start = seconds();
	result.status = fsimg_reader_open(ctx, io, &options, &reader, &result.error);
	if (result.status == FSIMG_OK)
		result.status = fsimg_verify(reader, flags, &result.report, &result.error);
	result.measurement = measurement(image, ctx, start);
	result.measurement.io_calls -= walk_io_calls;
	scale_rss_sample(rss);
	fsimg_reader_free(reader);
	return (result);
}

static void
assert_depth_limit(const walk_result_t *walk, const verify_result_t *verify)
{
	CHECK(walk->status == FSIMG_ELIMIT && walk->error.resource == FSIMG_RESOURCE_DEPTH);
	CHECK(walk->error.limit == MAX_DEPTH &&
	    walk->error.observed == MAX_DEPTH + 1 && walk->entries == MAX_DEPTH);
	CHECK(verify->status == FSIMG_ELIMIT && verify->error.resource == FSIMG_RESOURCE_DEPTH);
	CHECK(verify->error.limit == MAX_DEPTH &&
	    verify->error.observed == MAX_DEPTH + 1);
	CHECK(!verify->report.complete);
}

static void
assert_complete(const image_t *image, const walk_result_t *walk,
    const verify_result_t *verify, const fsimg_stats_t *stats)
{
	if (walk->status != FSIMG_DONE)
		require_ok(walk->status, &walk->error);
	require_ok(verify->status, &verify->error);
	CHECK(walk->entries == (uint64_t)image->files + image->folders);
	CHECK(walk->path_sum == expected_path_sum(image));
	CHECK(walk->files == image->files && walk->folders == image->folders);
	CHECK(verify->report.complete && verify->report.files == image->files &&
	    verify->report.directories == (uint64_t)image->folders + 1);
	CHECK(verify->report.logical_bytes == (image->shape == CAPACITY ? 5 :
	    (uint64_t)image->extents * 512));
	CHECK(verify->report.free_clusters == image->free_blocks);
	if (image->shape == EXTENTS) {
		CHECK(verify->report.scope == FSIMG_VALIDATION_PAYLOAD);
		CHECK(verify->report.payload_bytes_read ==
		    (uint64_t)image->extents * 512);
	} else {
		CHECK(verify->report.scope == FSIMG_VALIDATION_STRUCTURAL);
		CHECK(verify->report.payload_bytes_read == 0);
	}
	if (image->shape == WIDE)
		CHECK(stats->scratch_peak > SCALE_BUDGET);
	if (image->shape == CAPACITY)
		CHECK(image->bitmap_seen == ceiling(image->blocks, 8));
}

static void
run(shape_t shape, uint32_t amount)
{
	const char *labels[] = { "catalog", "wide", "extents", "deep", "depth-limit", "capacity" };
	/* Metadata block reads per block of metadata, which a repeated whole-tree
	 * scan per object would exceed. Each is the integer above the ratio measured
	 * from a single object (with METADATA_FIXED_READS) to 10,000,000 catalog
	 * objects. DEEP repeats a root-to-leaf search for each level.
	 */
	static const pass_ceiling_t pass_ceilings[] = {
		[CATALOG] = { 1, 3 },
		[WIDE] = { 1, 3 },
		[EXTENTS] = { 2, 4 },
		[DEEP] = { 21, 23 },
		[DEPTH_LIMIT] = { 21, 23 },
		[CAPACITY] = { 1, 2 }
	};
	image_t image;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_stats_t baseline, stats;
	walk_result_t walked;
	verify_result_t verified;
	uint64_t rss, rss_base, metadata_blocks;
	char directory[] = "hfsplus-scale-XXXXXX";
	int open_descriptors = fsimg_test_open_descriptors();

	plan_image(&image, shape, amount);
	CHECK(mkdtemp(directory) != NULL);
	setup_context_io(&image, directory, &ctx, &io, &baseline);
	rss_base = scale_peak_rss();
	walked = walk_image(&image, ctx, io, &rss);
	verified = verify_image(&image, ctx, io, walked.measurement.io_calls,
	    &rss);
	fsimg_io_free(io);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	fsimg_ctx_free(ctx);
	CHECK(fsimg_test_open_descriptors() == open_descriptors);
	CHECK(rmdir(directory) == 0);
	metadata_blocks = (uint64_t)(image.catalog.nodes + image.overflow.nodes) *
	    (NODE_BYTES / 512) + ceiling(ceiling(image.blocks, 8), 512);
	printf("hfsplus %s amount=%" PRIu32 " entries=%" PRIu64
	    " walk_reads=%" PRIu64 " walk_io_calls=%" PRIu64
	    " walk_metadata_block_reads=%" PRIu64 " walk_metadata_pass_equivalents=%.3f"
	    " walk_seconds=%.3f verify_reads=%" PRIu64 " verify_io_calls=%" PRIu64
	    " verify_metadata_block_reads=%" PRIu64 " verify_metadata_pass_equivalents=%.3f"
	    " verify_seconds=%.3f scratch_peak=%" PRIu64 " memory_peak=%" PRIu64
	    " rss=%" PRIu64 " bitmap_seen=%" PRIu64
	    " walk_status=%d verify_status=%d verify_complete=%u\n",
	    labels[shape], amount, walked.entries,
	    walked.measurement.reads, walked.measurement.io_calls,
	    walked.measurement.metadata_reads,
	    (double)walked.measurement.metadata_reads / metadata_blocks,
	    walked.measurement.elapsed, verified.measurement.reads,
	    verified.measurement.io_calls, verified.measurement.metadata_reads,
	    (double)verified.measurement.metadata_reads / metadata_blocks,
	    verified.measurement.elapsed,
	    stats.scratch_peak, stats.memory_peak, rss, image.bitmap_seen,
	    (int)walked.status, (int)verified.status, (unsigned)verified.report.complete);
	CHECK(fflush(stdout) == 0);
	/* The live context owns its baseline allocation and scratch descriptor;
	 * all resources acquired by the reader and I/O must have returned. */
	CHECK(stats.memory_bytes - baseline.memory_bytes == 0);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == baseline.descriptors);
	CHECK(stats.memory_peak <= SCALE_BUDGET);
	/* An uncharged resident catalog or extent array must not grow with the
	 * fixture. Allow runtime/allocator overhead above the charged budget. */
	CHECK(rss <= rss_base + SCALE_BUDGET + (UINT64_C(16) << 20));
	CHECK(walked.measurement.metadata_reads <=
	    metadata_blocks * pass_ceilings[shape].walk + METADATA_FIXED_READS);
	CHECK(verified.measurement.metadata_reads <=
	    metadata_blocks * pass_ceilings[shape].verify + METADATA_FIXED_READS);
	if (shape == DEPTH_LIMIT)
		assert_depth_limit(&walked, &verified);
	else
		assert_complete(&image, &walked, &verified, &stats);
}

int
main(int argc, char **argv)
{
	shape_t shape;
	uint32_t amount;
	unsigned long long parsed;
	char *end;

	CHECK(argc == 2 || argc == 3);
	if (strcmp(argv[1], "--catalog") == 0) {
		shape = CATALOG;
		amount = 10000000;
	} else if (strcmp(argv[1], "--wide") == 0) {
		shape = WIDE;
		amount = 100000;
	} else if (strcmp(argv[1], "--extents") == 0) {
		shape = EXTENTS;
		amount = 1000000;
	} else if (strcmp(argv[1], "--deep") == 0) {
		shape = DEEP;
		amount = MAX_DEPTH;
	} else if (strcmp(argv[1], "--depth-limit") == 0) {
		shape = DEPTH_LIMIT;
		amount = MAX_DEPTH + 1;
	} else {
		CHECK(strcmp(argv[1], "--capacity") == 0);
		shape = CAPACITY;
		amount = UINT32_MAX;
	}
	if (argc == 3) {
		CHECK(shape == CATALOG || shape == WIDE || shape == EXTENTS || shape == DEEP);
		errno = 0;
		parsed = strtoull(argv[2], &end, 10);
		CHECK(errno == 0 && argv[2][0] >= '0' && argv[2][0] <= '9' && *end == '\0');
		CHECK(parsed > 0 && parsed <= 10000000);
		amount = (uint32_t)parsed;
	}
	run(shape, amount);
	return (0);
}
