/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write-hfsplus.h"

/* The bulk B-tree packer. A tree is built once from its complete sorted leaf
 * records, so it never inserts or splits. Records fill a node greedily: one
 * starts a new node exactly when it does not fit in the current one, and a
 * record is never divided. pack_starts_node() is that one decision, and both
 * passes take it for every record:
 *
 *   hfsplus_tree_pack()  counts nodes, records each node's first key in the
 *                        next level's stream, and repeats on that stream
 *                        until a level has one node, the root;
 *   hfsplus_tree_emit()  repeats the walk with the same decisions, encoding
 *                        each node as it closes.
 *
 * So a node boundary emission computes cannot differ from the one planning
 * sized. Records in a stream need no child pointers: nodes of a level are
 * numbered consecutively, so the index record built from entry i of a level
 * stream points at that level's first node plus i. Termination follows from
 * the fanout: a node holds at least seven of the largest index records, so
 * each level has at most a seventh of the nodes of the one below, and the
 * largest catalog the record count allows has a depth well below 16.
 */
_Static_assert((HFSPLUS_WRITE_NODE_BYTES - HFSPLUS_NODE_DESCRIPTOR_BYTES - 16u) /
    (HFSPLUS_CKEY_FIXED_BYTES + 2u * HFSPLUS_NAME_UNITS_MAX + 4u + 2u) >= 7u,
    "a node holds at least seven of the largest index records");

/* A tree's map covers totalNodes, which rounding up to whole blocks can make
 * larger than the used nodes. The coverage of the header and of each map node
 * is a whole number of the most nodes one block holds, so a count that fits a
 * coverage still fits it rounded up, and the map node count depends on the
 * catalog alone.
 */
#define HFSPLUS_NODES_PER_BLOCK_MAX \
	(HFSPLUS_WRITE_BLOCK_MAX / HFSPLUS_WRITE_NODE_BYTES)
_Static_assert(
    HFSPLUS_HEADER_MAP_BITS(HFSPLUS_WRITE_NODE_BYTES) %
    HFSPLUS_NODES_PER_BLOCK_MAX == 0 &&
    HFSPLUS_MAP_NODE_BITS(HFSPLUS_WRITE_NODE_BYTES) %
    HFSPLUS_NODES_PER_BLOCK_MAX == 0,
    "map coverage is whole blocks of nodes at the largest block size");

/* One level's greedy fill: the node open at the moment, and how many have been
 * opened. A node of n records and b record bytes occupies 14 + b + 2 * (n + 1)
 * bytes: the descriptor, the records and the offset table with its free-space
 * entry.
 */
typedef struct hfsplus_pack {
	uint32_t	 node_bytes;
	uint32_t	 used_bytes;
	uint32_t	 records;
	uint32_t	 nodes;
} hfsplus_pack_t;

/* One level being encoded into the caller's node buffer. pack decides the record
 * boundaries and counts the level's nodes so far; first and last are the level's
 * first and last node numbers, from which the open node's number and sibling
 * links derive. height and kind are the values of each node's descriptor.
 */
typedef struct hfsplus_node_encoder {
	const hfsplus_tree_emit_t *emit;
	hfsplus_pack_t	 pack;
	uint32_t	 first;
	uint32_t	 last;
	uint32_t	 height;
	int8_t		 kind;
} hfsplus_node_encoder_t;

/* The B-tree header record's fields the writer sets beyond zero. */
#define BTREE_DEPTH		0u
#define BTREE_ROOT		2u
#define BTREE_LEAF_RECORDS	6u
#define BTREE_FIRST_LEAF	10u
#define BTREE_LAST_LEAF		14u
#define BTREE_NODE_SIZE		18u
#define BTREE_MAX_KEY_LENGTH	20u
#define BTREE_TOTAL_NODES	22u
#define BTREE_FREE_NODES	26u
#define BTREE_CLUMP_SIZE	32u
#define BTREE_COMPARE_TYPE	37u
#define BTREE_ATTRIBUTES	38u

/* The one packing decision: whether a record must start a new node, because
 * none is open or it does not fit the open one.
 */
static bool
pack_starts_node(const hfsplus_pack_t *pack, uint32_t record_bytes)
{
	uint32_t occupied;

	occupied = HFSPLUS_NODE_DESCRIPTOR_BYTES + pack->used_bytes +
	    record_bytes + 2u * (pack->records + 2u);
	return (pack->nodes == 0 || occupied > pack->node_bytes);
}

static void
pack_open_node(hfsplus_pack_t *pack)
{
	pack->nodes++;
	pack->used_bytes = 0;
	pack->records = 0;
}

static void
pack_add(hfsplus_pack_t *pack, uint32_t record_bytes)
{
	pack->used_bytes += record_bytes;
	pack->records++;
}

/* Place a record in a counting pass. True when it starts a new node. */
static bool
pack_place(hfsplus_pack_t *pack, uint32_t record_bytes)
{
	bool starts_node;

	starts_node = pack_starts_node(pack, record_bytes);
	if (starts_node)
		pack_open_node(pack);
	pack_add(pack, record_bytes);
	return (starts_node);
}

/* The on-disk length of a leaf record: its key and its data, every part even. */
static uint32_t
leaf_record_bytes(const hfsplus_write_record_t *record)
{
	uint32_t units = record->units;

	switch (record->type) {
	case HFSPLUS_RECORD_FOLDER:
		return (HFSPLUS_CKEY_FIXED_BYTES + 2u * units +
		    HFSPLUS_FOLDER_BYTES);
	case HFSPLUS_RECORD_FILE:
		return (HFSPLUS_CKEY_FIXED_BYTES + 2u * units +
		    HFSPLUS_FILE_BYTES);
	default:
		return (HFSPLUS_CKEY_FIXED_BYTES + HFSPLUS_THREAD_FIXED_BYTES +
		    2u * units);
	}
}

/* A level record's index record: its key at actual length, then a child. */
static uint32_t
index_record_bytes(uint32_t units)
{
	return (HFSPLUS_CKEY_FIXED_BYTES + 2u * units + 4u);
}

/* Whether a stream record is a thread, whose on-disk key has an empty name. */
static bool
record_is_thread(const hfsplus_write_record_t *record)
{
	return (record->type == HFSPLUS_RECORD_FOLDER_THREAD ||
	    record->type == HFSPLUS_RECORD_FILE_THREAD);
}

size_t
hfsplus_write_record_encode(const hfsplus_write_record_t *record,
    unsigned char *raw)
{
	size_t name_bytes = 2u * (size_t)record->units;

	fsimg_store_be32(raw, record->parent);
	fsimg_store_be32(raw + 4, record->id);
	fsimg_store_be16(raw + 8, record->type);
	fsimg_store_be16(raw + 10, record->key_bytes);
	fsimg_store_be16(raw + 12, record->units);
	if (record->key_bytes != 0)
		bcopy(record->key, raw + HFSPLUS_WRITE_RECORD_FIXED,
		    record->key_bytes);
	if (name_bytes != 0)
		bcopy(record->name, raw + HFSPLUS_WRITE_RECORD_FIXED +
		    record->key_bytes, name_bytes);
	return (HFSPLUS_WRITE_RECORD_FIXED + record->key_bytes + name_bytes);
}

bool
hfsplus_write_record_decode(const unsigned char *raw, size_t length,
    hfsplus_write_record_t *record)
{
	if (length < HFSPLUS_WRITE_RECORD_FIXED)
		return (false);
	record->parent = fsimg_load_be32(raw);
	record->id = fsimg_load_be32(raw + 4);
	record->type = fsimg_load_be16(raw + 8);
	record->key_bytes = fsimg_load_be16(raw + 10);
	record->units = fsimg_load_be16(raw + 12);
	record->key = raw + HFSPLUS_WRITE_RECORD_FIXED;
	record->name = record->key + record->key_bytes;
	return (record->units <= HFSPLUS_NAME_UNITS_MAX &&
	    length == HFSPLUS_WRITE_RECORD_FIXED +
	    (size_t)record->key_bytes + 2u * (size_t)record->units);
}

/* Big-endian bytes order as the fields they encode, and a namespace key's
 * bytes as its units do, with a shorter equal prefix first. That is the
 * catalog comparators' order, folded or binary, and it needs no tables once
 * the key is built. An empty key, a thread's, comes first in its parent.
 */
int
hfsplus_write_record_compare(const void *a, size_t a_length FSIMG_UNUSED,
    const void *b, size_t b_length FSIMG_UNUSED, void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a;
	const unsigned char *right = b;
	size_t left_key, right_key;
	int order;

	order = memcmp(left, right, 4);
	if (order != 0)
		return (order);
	left_key = fsimg_load_be16(left + 10);
	right_key = fsimg_load_be16(right + 10);
	order = memcmp(left + HFSPLUS_WRITE_RECORD_FIXED,
	    right + HFSPLUS_WRITE_RECORD_FIXED,
	    left_key < right_key ? left_key : right_key);
	if (order != 0)
		return (order);
	if (left_key != right_key)
		return (left_key < right_key ? -1 : 1);
	return (memcmp(left + 4, right + 4, 4));
}

void
hfsplus_tree_shape_init(hfsplus_tree_shape_t *shape, hfsplus_tree_kind_t kind,
    hfsplus_compare_t compare)
{
	bzero(shape, sizeof(*shape));
	shape->used_nodes = 1;
	switch (kind) {
	case HFSPLUS_TREE_EXTENTS:
		shape->node_bytes = HFSPLUS_WRITE_NODE_BYTES;
		shape->max_key_length = HFSPLUS_EKEY_LENGTH;
		shape->attributes = HFSPLUS_BTREE_BIG_KEYS;
		break;
	case HFSPLUS_TREE_CATALOG:
		shape->node_bytes = HFSPLUS_WRITE_NODE_BYTES;
		shape->max_key_length = HFSPLUS_MAX_KEY_CATALOG;
		shape->attributes = HFSPLUS_BTREE_BIG_KEYS |
		    HFSPLUS_BTREE_VARIABLE_KEYS;
		shape->compare_type = compare == HFSPLUS_COMPARE_BINARY ?
		    HFSPLUS_COMPARE_TYPE_BINARY : HFSPLUS_COMPARE_TYPE_FOLDED;
		break;
	case HFSPLUS_TREE_ATTRIBUTES:
		shape->node_bytes = HFSPLUS_WRITE_ATTR_NODE_BYTES;
		shape->max_key_length = HFSPLUS_MAX_KEY_ATTRIBUTES;
		shape->attributes = HFSPLUS_BTREE_BIG_KEYS |
		    HFSPLUS_BTREE_VARIABLE_KEYS;
		break;
	}
}

/* Append one node's first key to the next level's stream. */
static fsimg_status_t
level_append(fsimg_stream_t *level, uint32_t parent, uint32_t units,
    const unsigned char *name, fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_WRITE_LEVEL_MAX];
	size_t name_bytes = 2u * (size_t)units;

	fsimg_store_be32(raw, parent);
	fsimg_store_be16(raw + 4, (uint16_t)units);
	if (name_bytes != 0)
		bcopy(name, raw + HFSPLUS_WRITE_LEVEL_FIXED, name_bytes);
	return (fsimg_stream_append(level, raw,
	    HFSPLUS_WRITE_LEVEL_FIXED + name_bytes, NULL, error));
}

static fsimg_status_t
level_stream_new(fsimg_session_t *session, fsimg_stream_t **level,
    fsimg_error_t *error)
{
	return (fsimg_stream_new(session, HFSPLUS_WRITE_LEVEL_MAX, level, error));
}

static fsimg_status_t
stale_stream(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
	    "HFS Plus packer record stream is inconsistent"));
}

/* Decode a level stream record read back from scratch; its name is at raw +
 * HFSPLUS_WRITE_LEVEL_FIXED. Only a record level_append did not write fails, and
 * passing keeps the name within the caller's HFSPLUS_WRITE_LEVEL_MAX bytes.
 */
static bool
level_record_decode(const unsigned char *raw, size_t length, uint32_t *parent,
    uint32_t *units)
{
	if (length < HFSPLUS_WRITE_LEVEL_FIXED)
		return (false);
	*parent = fsimg_load_be32(raw);
	*units = fsimg_load_be16(raw + 4);
	return (*units <= HFSPLUS_NAME_UNITS_MAX &&
	    length == HFSPLUS_WRITE_LEVEL_FIXED + 2u * (size_t)*units);
}

/* The leaf level: one pass over the sorted records. */
static fsimg_status_t
pack_leaves(fsimg_session_t *session, fsimg_stream_t *records,
    hfsplus_tree_shape_t *shape, fsimg_stream_t *first_keys, fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_write_record_t record;
	hfsplus_pack_t pack = { .node_bytes = shape->node_bytes };
	uint64_t cursor = 0;
	size_t length;
	fsimg_status_t status;

	for (;;) {
		status = fsimg_stream_next(records, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_write_record_decode(raw, length, &record))
			return (stale_stream(error));
		shape->leaf_records++;
		if (!pack_place(&pack, leaf_record_bytes(&record)))
			continue;
		status = level_append(first_keys, record.parent,
		    record_is_thread(&record) ? 0 : record.units, record.name,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	shape->level_nodes[0] = pack.nodes;
	return (FSIMG_OK);
}

/* One index level: its records are the first keys of the level below, and it
 * records its own nodes' first keys for the level above.
 */
static fsimg_status_t
pack_index_level(fsimg_session_t *session, fsimg_stream_t *below,
    uint32_t *nodes, fsimg_stream_t *first_keys, uint32_t node_bytes,
    fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_WRITE_LEVEL_MAX];
	hfsplus_pack_t pack = { .node_bytes = node_bytes };
	uint64_t cursor = 0;
	size_t length;
	uint32_t parent, units;
	fsimg_status_t status;

	for (;;) {
		status = fsimg_stream_next(below, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (!level_record_decode(raw, length, &parent, &units))
			return (stale_stream(error));
		if (!pack_place(&pack, index_record_bytes(units)))
			continue;
		status = level_append(first_keys, parent, units,
		    raw + HFSPLUS_WRITE_LEVEL_FIXED, error);
		if (status != FSIMG_OK)
			return (status);
	}
	*nodes = pack.nodes;
	return (FSIMG_OK);
}

/* The map nodes a tree of used nodes without them needs: the smallest k with
 * used + k <= H + k * P, where H and P are the header's and a map node's bits,
 * since each map node also occupies a bit. That is ceil((used - H) / (P - 1)).
 */
static uint32_t
map_nodes_needed(uint32_t used, uint32_t node_bytes)
{
	uint32_t header_bits = HFSPLUS_HEADER_MAP_BITS(node_bytes);
	uint32_t per_node = HFSPLUS_MAP_NODE_BITS(node_bytes) - 1u;

	if (used <= header_bits)
		return (0);
	return ((used - header_bits + per_node - 1u) / per_node);
}

fsimg_status_t
hfsplus_tree_pack(fsimg_session_t *session, fsimg_stream_t *records,
    hfsplus_tree_shape_t *shape, fsimg_stream_t **levels, fsimg_error_t *error)
{
	uint32_t height, used;
	fsimg_status_t status;

	status = level_stream_new(session, &levels[0], error);
	if (status == FSIMG_OK)
		status = pack_leaves(session, records, shape, levels[0], error);
	if (status != FSIMG_OK)
		return (status);
	if (shape->level_nodes[0] == 0)
		return (stale_stream(error));

	/* A level of one node is the root; every level above is built from
	 * the first keys of the one below.
	 */
	height = 1;
	while (shape->level_nodes[height - 1] > 1) {
		if (height == HFSPLUS_BTREE_DEPTH_MAX)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER,
			    "HFS Plus catalog exceeds the tree depth"));
		status = level_stream_new(session, &levels[height], error);
		if (status == FSIMG_OK)
			status = pack_index_level(session, levels[height - 1],
			    &shape->level_nodes[height], levels[height],
			    shape->node_bytes, error);
		if (status != FSIMG_OK)
			return (status);
		height++;
	}
	shape->depth = height;

	/* Planning bounds the records below 2^32. A leaf holds at least five
	 * of the largest records, so these sums and the rounding up to whole
	 * blocks stay far inside 32 bits.
	 */
	used = 1;
	for (uint32_t i = 0; i < height; i++)
		used += shape->level_nodes[i];
	shape->map_nodes = map_nodes_needed(used, shape->node_bytes);
	shape->used_nodes = used + shape->map_nodes;
	return (FSIMG_OK);
}

uint64_t
hfsplus_tree_blocks(const hfsplus_tree_shape_t *shape, uint32_t block_bytes)
{
	uint64_t bytes;

	bytes = (uint64_t)shape->used_nodes * shape->node_bytes;
	return (bytes / block_bytes + (bytes % block_bytes != 0));
}

void
hfsplus_tree_place(hfsplus_tree_shape_t *shape, uint32_t first_block,
    uint32_t block_bytes)
{
	uint64_t blocks;

	blocks = hfsplus_tree_blocks(shape, block_bytes);
	shape->first_block = first_block;
	shape->blocks = (uint32_t)blocks;
	shape->total_nodes = (uint32_t)(blocks * block_bytes /
	    shape->node_bytes);
}

/* The first node of level index level, where index 0 is the leaves: the header
 * is node 0 and the levels follow it from the leaves up. Index depth is where
 * the map nodes start.
 */
static uint32_t
level_first_node(const hfsplus_tree_shape_t *shape, uint32_t level)
{
	uint32_t first = 1;

	for (uint32_t i = 0; i < level; i++)
		first += shape->level_nodes[i];
	return (first);
}

/* Prepare the buffer for the next node of the encoder's level. */
static void
node_begin(hfsplus_node_encoder_t *encoder)
{
	unsigned char *node = encoder->emit->node;
	uint32_t node_bytes = encoder->pack.node_bytes;

	bzero(node, node_bytes);
	node[8] = (unsigned char)encoder->kind;
	node[9] = (unsigned char)encoder->height;
	fsimg_store_be16(node + node_bytes - 2, HFSPLUS_NODE_DESCRIPTOR_BYTES);
}

/* The bytes of a record the pack just placed at the end of the node. Its
 * offset table entry, its end, follows the entry of its start.
 */
static unsigned char *
node_record(const hfsplus_node_encoder_t *encoder, uint32_t record_bytes)
{
	const hfsplus_pack_t *pack = &encoder->pack;
	unsigned char *node = encoder->emit->node;
	uint32_t end;

	end = HFSPLUS_NODE_DESCRIPTOR_BYTES + pack->used_bytes;
	fsimg_store_be16(node + pack->node_bytes - 2u * (pack->records + 1u),
	    (uint16_t)end);
	return (node + end - record_bytes);
}

/* Link the finished node, the pack's open one, to its siblings by the level's
 * numbering and write it.
 */
static fsimg_status_t
node_end(const hfsplus_node_encoder_t *encoder, fsimg_error_t *error)
{
	const hfsplus_pack_t *pack = &encoder->pack;
	unsigned char *node = encoder->emit->node;
	uint32_t number;

	number = encoder->first + pack->nodes - 1u;
	fsimg_store_be32(node, number == encoder->last ? 0 : number + 1u);
	fsimg_store_be32(node + 4, number == encoder->first ? 0 : number - 1u);
	fsimg_store_be16(node + 10, (uint16_t)pack->records);
	return (encoder->emit->write(encoder->emit->argument, number, node,
	    error));
}

/* A catalog key: keyLength, which excludes itself, the parent and the name. */
static size_t
key_encode(unsigned char *out, uint32_t parent, uint32_t units,
    const unsigned char *name)
{
	size_t name_bytes = 2u * (size_t)units;

	fsimg_store_be16(out, (uint16_t)(HFSPLUS_CKEY_FIXED_BYTES - 2u +
	    name_bytes));
	fsimg_store_be32(out + 2, parent);
	fsimg_store_be16(out + 6, (uint16_t)units);
	if (name_bytes != 0)
		bcopy(name, out + HFSPLUS_CKEY_FIXED_BYTES, name_bytes);
	return (HFSPLUS_CKEY_FIXED_BYTES + name_bytes);
}

/* Place one record, closing the previous node when it starts a new one, and
 * return where its bytes go. A record starting a node must fit the empty node,
 * so every write the caller then makes through the result stays inside it.
 */
static fsimg_status_t
encoder_place(hfsplus_node_encoder_t *encoder, uint32_t record_bytes,
    unsigned char **out, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (pack_starts_node(&encoder->pack, record_bytes)) {
		if (encoder->pack.nodes != 0) {
			status = node_end(encoder, error);
			if (status != FSIMG_OK)
				return (status);
		}
		pack_open_node(&encoder->pack);
		if (encoder->first + encoder->pack.nodes - 1u > encoder->last ||
		    pack_starts_node(&encoder->pack, record_bytes))
			return (stale_stream(error));
		node_begin(encoder);
	}
	pack_add(&encoder->pack, record_bytes);
	*out = node_record(encoder, record_bytes);
	return (FSIMG_OK);
}

static fsimg_status_t
emit_leaves(const hfsplus_tree_emit_t *emit, fsimg_error_t *error)
{
	const hfsplus_tree_shape_t *shape = emit->shape;
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_node_encoder_t encoder;
	hfsplus_write_record_t record;
	unsigned char *out;
	uint64_t cursor = 0;
	size_t key_bytes, length;
	fsimg_status_t status;

	bzero(&encoder, sizeof(encoder));
	encoder.emit = emit;
	encoder.pack.node_bytes = shape->node_bytes;
	encoder.first = level_first_node(shape, 0);
	encoder.last = encoder.first + shape->level_nodes[0] - 1u;
	encoder.height = 1;
	encoder.kind = HFSPLUS_NODE_LEAF;
	for (;;) {
		status = fsimg_stream_next(emit->records, &cursor, raw,
		    sizeof(raw), &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(emit->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_write_record_decode(raw, length, &record))
			return (stale_stream(error));
		status = encoder_place(&encoder, leaf_record_bytes(&record),
		    &out, error);
		if (status != FSIMG_OK)
			return (status);
		key_bytes = key_encode(out, record.parent,
		    record_is_thread(&record) ? 0 : record.units, record.name);
		status = emit->leaf_data(emit->argument, &record,
		    out + key_bytes, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (encoder.pack.nodes != shape->level_nodes[0])
		return (stale_stream(error));
	return (node_end(&encoder, error));
}

/* The index level at the given level index, 1 being the level over the leaves. */
static fsimg_status_t
emit_index_level(const hfsplus_tree_emit_t *emit, uint32_t level,
    fsimg_error_t *error)
{
	const hfsplus_tree_shape_t *shape = emit->shape;
	unsigned char raw[HFSPLUS_WRITE_LEVEL_MAX];
	hfsplus_node_encoder_t encoder;
	unsigned char *out;
	uint64_t cursor = 0;
	size_t key_bytes, length;
	uint32_t child, parent, units;
	fsimg_status_t status;

	bzero(&encoder, sizeof(encoder));
	encoder.emit = emit;
	encoder.pack.node_bytes = shape->node_bytes;
	encoder.first = level_first_node(shape, level);
	encoder.last = encoder.first + shape->level_nodes[level] - 1u;
	encoder.height = level + 1u;
	encoder.kind = HFSPLUS_NODE_INDEX;
	child = level_first_node(shape, level - 1u);
	for (;;) {
		status = fsimg_stream_next(emit->levels[level - 1u], &cursor,
		    raw, sizeof(raw), &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(emit->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (!level_record_decode(raw, length, &parent, &units))
			return (stale_stream(error));
		status = encoder_place(&encoder, index_record_bytes(units), &out,
		    error);
		if (status != FSIMG_OK)
			return (status);
		key_bytes = key_encode(out, parent, units,
		    raw + HFSPLUS_WRITE_LEVEL_FIXED);
		fsimg_store_be32(out + key_bytes, child);
		child++;
	}
	if (encoder.pack.nodes != shape->level_nodes[level])
		return (stale_stream(error));
	return (node_end(&encoder, error));
}

/* Mark the next in-use nodes in a map record, most significant bit first, and
 * return how many it took. Bits past the used nodes stay zero.
 */
static uint32_t
map_mark(unsigned char *map, uint32_t map_bytes, uint32_t remaining)
{
	uint32_t bits, whole;

	bits = remaining < 8u * map_bytes ? remaining : 8u * map_bytes;
	whole = bits / 8u;
	memset(map, 0xff, whole);
	if (bits % 8u != 0)
		map[whole] = (unsigned char)(0xffu << (8u - bits % 8u));
	return (bits);
}

/* The chain of map nodes: bLink 0, height 0, one record of fixed length. */
static fsimg_status_t
emit_map_nodes(const hfsplus_tree_emit_t *emit, uint32_t remaining,
    fsimg_error_t *error)
{
	const hfsplus_tree_shape_t *shape = emit->shape;
	unsigned char *node = emit->node;
	uint32_t first, node_bytes, record_bytes;
	fsimg_status_t status;

	node_bytes = shape->node_bytes;
	record_bytes = HFSPLUS_MAP_RECORD_BYTES(node_bytes);
	first = level_first_node(shape, shape->depth);
	for (uint32_t i = 0; i < shape->map_nodes; i++) {
		bzero(node, node_bytes);
		if (i + 1u < shape->map_nodes)
			fsimg_store_be32(node, first + i + 1u);
		node[8] = HFSPLUS_NODE_MAP;
		fsimg_store_be16(node + 10, 1);
		remaining -= map_mark(node + HFSPLUS_NODE_DESCRIPTOR_BYTES,
		    record_bytes, remaining);
		fsimg_store_be16(node + node_bytes - 2,
		    HFSPLUS_NODE_DESCRIPTOR_BYTES);
		fsimg_store_be16(node + node_bytes - 4,
		    (uint16_t)(HFSPLUS_NODE_DESCRIPTOR_BYTES + record_bytes));
		status = emit->write(emit->argument, first + i, node, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* The header record, at its fixed offsets; reserved fields stay zero. */
static void
header_record_encode(const hfsplus_tree_shape_t *shape, unsigned char *record)
{
	uint32_t first_leaf, last_leaf, root;

	first_leaf = 0;
	last_leaf = 0;
	root = 0;
	if (shape->depth != 0) {
		first_leaf = 1;
		last_leaf = shape->level_nodes[0];
		root = level_first_node(shape, shape->depth - 1u);
	}
	fsimg_store_be16(record + BTREE_DEPTH, (uint16_t)shape->depth);
	fsimg_store_be32(record + BTREE_ROOT, root);
	fsimg_store_be32(record + BTREE_LEAF_RECORDS, shape->leaf_records);
	fsimg_store_be32(record + BTREE_FIRST_LEAF, first_leaf);
	fsimg_store_be32(record + BTREE_LAST_LEAF, last_leaf);
	fsimg_store_be16(record + BTREE_NODE_SIZE, (uint16_t)shape->node_bytes);
	fsimg_store_be16(record + BTREE_MAX_KEY_LENGTH, shape->max_key_length);
	fsimg_store_be32(record + BTREE_TOTAL_NODES, shape->total_nodes);
	fsimg_store_be32(record + BTREE_FREE_NODES,
	    shape->total_nodes - shape->used_nodes);
	fsimg_store_be32(record + BTREE_CLUMP_SIZE, hfsplus_tree_clump(shape));
	record[BTREE_COMPARE_TYPE] = shape->compare_type;
	fsimg_store_be32(record + BTREE_ATTRIBUTES, shape->attributes);
}

/* Node 0: the header record, zero user data and the map's first bits. Returns
 * through remaining the in-use nodes the map nodes still have to mark.
 */
static fsimg_status_t
emit_header(const hfsplus_tree_emit_t *emit, uint32_t *remaining,
    fsimg_error_t *error)
{
	const hfsplus_tree_shape_t *shape = emit->shape;
	unsigned char *node = emit->node;
	uint32_t map_bytes, node_bytes;

	node_bytes = shape->node_bytes;
	map_bytes = node_bytes - HFSPLUS_HEADER_MAP_OFFSET - 8u;
	bzero(node, node_bytes);
	if (shape->map_nodes != 0)
		fsimg_store_be32(node, level_first_node(shape, shape->depth));
	node[8] = HFSPLUS_NODE_HEADER;
	fsimg_store_be16(node + 10, 3);
	header_record_encode(shape, node + HFSPLUS_NODE_DESCRIPTOR_BYTES);
	*remaining = shape->used_nodes - map_mark(node +
	    HFSPLUS_HEADER_MAP_OFFSET, map_bytes, shape->used_nodes);
	fsimg_store_be16(node + node_bytes - 2, HFSPLUS_NODE_DESCRIPTOR_BYTES);
	fsimg_store_be16(node + node_bytes - 4,
	    HFSPLUS_NODE_DESCRIPTOR_BYTES + HFSPLUS_BTREE_HEADER_BYTES);
	fsimg_store_be16(node + node_bytes - 6, HFSPLUS_HEADER_MAP_OFFSET);
	fsimg_store_be16(node + node_bytes - 8,
	    (uint16_t)(node_bytes - 8u));
	return (emit->write(emit->argument, 0, node, error));
}

fsimg_status_t
hfsplus_tree_emit(const hfsplus_tree_emit_t *emit, fsimg_error_t *error)
{
	const hfsplus_tree_shape_t *shape = emit->shape;
	uint32_t remaining;
	fsimg_status_t status;

	if (shape->depth != 0) {
		status = emit_leaves(emit, error);
		if (status != FSIMG_OK)
			return (status);
		for (uint32_t level = 1; level < shape->depth; level++) {
			status = emit_index_level(emit, level, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}

	/* The header's map marks the first in-use nodes, and the map nodes
	 * take over where it stops.
	 */
	status = emit_header(emit, &remaining, error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_map_nodes(emit, remaining, error));
}
