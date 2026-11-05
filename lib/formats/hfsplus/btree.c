/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "hfsplus.h"

/*
 * The B-tree reader, the only code that interprets node framing. A tree is the
 * byte stream of its fork; node n is fork bytes [n * M, (n + 1) * M). Every
 * read validates the descriptor and the offset table, so a record's bounds are
 * safe to use once the node is in a buffer.
 *
 * Termination on a hostile tree holds by construction. A descent counts the
 * expected height down from the header's depth and requires each node to show
 * exactly that height, so it ends within the depth, however the child numbers
 * point. A walk along a sibling chain counts the leaves it enters and refuses
 * more than the tree's max_leaves, which the slice bounds whatever totalNodes
 * declares.
 *
 * Nothing here retains a node beyond the buffers callers name: a later search
 * rereads its path.
 */

#define NODE_SIZE_MIN_EXTENTS	512u
#define NODE_SIZE_MIN_NAMED	4096u
#define NODE_SIZE_MAX		32768u
#define MAX_KEY_EXTENTS		10u
#define TYPE_RESERVED		0xffu	/* btreeType of macOS's attributes tree */

/* The three header node records' fixed starts; the fourth entry of its offset
 * table is the map record's end.
 */
#define HEADER_RECORD_START	HFSPLUS_NODE_DESCRIPTOR_BYTES
#define HEADER_USER_START	120u
#define HEADER_MAP_START	248u
#define HEADER_RECORDS		3u

#define INDEX_DATA_BYTES	4u

static fsimg_status_t check_parameters(const hfsplus_tree_t *,
    const hfsplus_btree_t *, fsimg_error_t *);
static fsimg_status_t check_header_fields(const hfsplus_btree_t *,
    uint64_t, fsimg_error_t *);
static uint16_t node_offset(const unsigned char *, size_t, size_t);
static fsimg_status_t check_framing(const hfsplus_tree_t *,
    const unsigned char *, const hfsplus_node_t *, fsimg_error_t *);
static fsimg_status_t check_header_framing(const hfsplus_tree_t *,
    const unsigned char *, const hfsplus_node_t *, fsimg_error_t *);
static fsimg_status_t split_record(const hfsplus_tree_t *,
    const unsigned char *, size_t, hfsplus_record_t *, fsimg_error_t *);
static int ekey_compare(const hfsplus_ekey_t *, const hfsplus_ekey_t *);
static fsimg_status_t key_order(const hfsplus_tree_t *, const hfsplus_key_t *,
    const hfsplus_record_t *, int *, fsimg_error_t *);
static fsimg_status_t index_child(const hfsplus_tree_t *, const unsigned char *,
    const hfsplus_node_t *, const hfsplus_key_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t enter_leaf(hfsplus_cursor_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t follow_chain(hfsplus_cursor_t *, fsimg_error_t *);

/* The tree parameter table of specs/design-hfsplus.md. A value outside it is
 * unobserved rather than contradictory, so unsupported; a node size that is
 * not a power of two has no meaning at all and is corruption. The catalog's
 * keyCompareType depends on the volume's signature and is volume.c's choice.
 *
 * An attributes tree's btreeType and keyCompareType come as one of two pairs,
 * (0, 0) or (TYPE_RESERVED, HFSPLUS_COMPARE_TYPE_BINARY), and the header's pair
 * never changes how its keys compare: attribute names are always compared as
 * unsigned code units. A tree with no index node, which is only an empty
 * one, may omit the variable index keys flag, since no index key exists for
 * the flag to describe.
 */
static fsimg_status_t
check_parameters(const hfsplus_tree_t *tree, const hfsplus_btree_t *header,
    fsimg_error_t *error)
{
	uint32_t node_min, max_key, flags, found;
	uint8_t type = 0, compare = 0;

	switch (tree->kind) {
	case HFSPLUS_TREE_EXTENTS:
		node_min = NODE_SIZE_MIN_EXTENTS;
		max_key = MAX_KEY_EXTENTS;
		flags = HFSPLUS_BTREE_BIG_KEYS;
		break;
	case HFSPLUS_TREE_CATALOG:
		node_min = NODE_SIZE_MIN_NAMED;
		max_key = HFSPLUS_MAX_KEY_CATALOG;
		flags = HFSPLUS_BTREE_BIG_KEYS | HFSPLUS_BTREE_VARIABLE_KEYS;
		compare = header->key_compare_type;
		break;
	case HFSPLUS_TREE_ATTRIBUTES:
		node_min = NODE_SIZE_MIN_NAMED;
		max_key = HFSPLUS_MAX_KEY_ATTRIBUTES;
		flags = HFSPLUS_BTREE_BIG_KEYS | HFSPLUS_BTREE_VARIABLE_KEYS;
		if (header->btree_type == TYPE_RESERVED) {
			type = TYPE_RESERVED;
			compare = HFSPLUS_COMPARE_TYPE_BINARY;
		}
		break;
	default:
		return (hfsplus_corrupt(error,
		    "HFS Plus tree kind is unknown"));
	}
	if (header->node_size == 0 ||
	    (header->node_size & (header->node_size - 1)) != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's node size is not a power of two"));
	if (header->node_size < node_min || header->node_size > NODE_SIZE_MAX)
		return (hfsplus_unsupported(error,
		    "HFS Plus tree's node size is outside the supported range"));
	found = header->attributes & ~(uint32_t)HFSPLUS_BTREE_BAD_CLOSE;
	if (tree->kind == HFSPLUS_TREE_ATTRIBUTES && header->tree_depth == 0 &&
	    found == HFSPLUS_BTREE_BIG_KEYS)
		flags = HFSPLUS_BTREE_BIG_KEYS;
	if (header->max_key_length != max_key || header->btree_type != type ||
	    header->key_compare_type != compare || found != flags)
		return (hfsplus_unsupported(error,
		    "HFS Plus tree's parameters are outside the supported table"));
	return (FSIMG_OK);
}

/* The header record's structure: the fork holds exactly its nodes, and an
 * empty tree, of depth zero, names no nodes while any other names real ones.
 * A tree of no nodes at all fails when node 0 is read. Verification owns the
 * fields no read depends on.
 */
static fsimg_status_t
check_header_fields(const hfsplus_btree_t *header, uint64_t logical_size,
    fsimg_error_t *error)
{
	uint64_t fork_bytes;

	if (!fsimg_mul_u64(header->total_nodes, header->node_size,
	    &fork_bytes) || fork_bytes != logical_size)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's node count disagrees with its fork"));
	if (header->tree_depth > HFSPLUS_BTREE_DEPTH_MAX)
		return (hfsplus_corrupt(error, "HFS Plus tree is too deep"));
	if (header->tree_depth == 0) {
		if (header->root_node != 0 || header->first_leaf_node != 0 ||
		    header->last_leaf_node != 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus empty tree names nodes"));
		return (FSIMG_OK);
	}
	if (header->root_node == 0 ||
	    header->root_node >= header->total_nodes ||
	    header->first_leaf_node == 0 ||
	    header->first_leaf_node >= header->total_nodes ||
	    header->last_leaf_node == 0 ||
	    header->last_leaf_node >= header->total_nodes)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's root or leaf ends lie outside it"));
	return (FSIMG_OK);
}

/* Offset table entry i, stored at M - 2 * (i + 1). */
static uint16_t
node_offset(const unsigned char *buffer, size_t node_size, size_t i)
{
	return (fsimg_load_be16(buffer + node_size - 2 * (i + 1)));
}

/* A node is well formed when 14 = offset[0] < ... < offset[records] <=
 * M - 2 * (records + 1) and every offset is even. Leaf and index nodes also
 * hold a record: the tree's search and chain walks count on it.
 */
static fsimg_status_t
check_framing(const hfsplus_tree_t *tree, const unsigned char *buffer,
    const hfsplus_node_t *node, fsimg_error_t *error)
{
	size_t node_size = tree->header.node_size;
	size_t entries = (size_t)node->records + 1;
	uint32_t previous, offset;

	if (entries > (node_size - HFSPLUS_NODE_DESCRIPTOR_BYTES) / 2)
		return (hfsplus_corrupt(error,
		    "HFS Plus node's offset table does not fit it"));
	if ((node->kind == HFSPLUS_NODE_LEAF ||
	    node->kind == HFSPLUS_NODE_INDEX) && node->records == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus node holds no records"));
	previous = 0;
	for (size_t i = 0; i < entries; i++) {
		offset = node_offset(buffer, node_size, i);
		if (offset % 2 != 0 ||
		    (i == 0 && offset != HFSPLUS_NODE_DESCRIPTOR_BYTES) ||
		    (i != 0 && offset <= previous))
			return (hfsplus_corrupt(error,
			    "HFS Plus node's offset table is out of order"));
		previous = offset;
	}
	if (previous > node_size - 2 * entries)
		return (hfsplus_corrupt(error,
		    "HFS Plus node's records overlap its offset table"));
	return (FSIMG_OK);
}

/* Node 0 has the fixed layout of three records: header, user data and map. */
static fsimg_status_t
check_header_framing(const hfsplus_tree_t *tree, const unsigned char *buffer,
    const hfsplus_node_t *node, fsimg_error_t *error)
{
	size_t node_size = tree->header.node_size;

	if (node->records != HEADER_RECORDS ||
	    node_offset(buffer, node_size, 0) != HEADER_RECORD_START ||
	    node_offset(buffer, node_size, 1) != HEADER_USER_START ||
	    node_offset(buffer, node_size, 2) != HEADER_MAP_START ||
	    node_offset(buffer, node_size, 3) != node_size - 8)
		return (hfsplus_corrupt(error,
		    "HFS Plus header node's records are misplaced"));
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_node_read(hfsplus_tree_t *tree, uint32_t number, unsigned char *buffer,
    int kind, unsigned height, hfsplus_node_t *node, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (number >= tree->header.total_nodes)
		return (hfsplus_corrupt(error,
		    "HFS Plus node lies outside its tree"));
	status = fsimg_session_charge(&tree->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = hfsplus_fork_read(tree->reader, tree->fork,
	    (uint64_t)number * tree->header.node_size, buffer,
	    tree->header.node_size, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_node_decode(buffer, tree->header.node_size, node))
		return (hfsplus_corrupt(error, "HFS Plus node is malformed"));
	if (node->kind != kind || node->height != height)
		return (hfsplus_corrupt(error,
		    "HFS Plus node has the wrong kind or height"));
	return (check_framing(tree, buffer, node, error));
}

void
hfsplus_node_record(const hfsplus_tree_t *tree, const unsigned char *buffer,
    uint16_t index, const unsigned char **record, size_t *length)
{
	size_t node_size = tree->header.node_size;
	size_t start = node_offset(buffer, node_size, index);

	*record = buffer + start;
	*length = node_offset(buffer, node_size, (size_t)index + 1) - start;
}

fsimg_status_t
hfsplus_tree_open(fsimg_reader_t *reader, hfsplus_tree_t *tree,
    hfsplus_tree_kind_t kind, uint32_t id, const hfsplus_fork_t *fork,
    fsimg_error_t *error)
{
	unsigned char start[HFSPLUS_NODE_DESCRIPTOR_BYTES +
	    HFSPLUS_BTREE_HEADER_BYTES];
	hfsplus_node_t node;
	hfsplus_btree_t header;
	fsimg_error_t detail;
	fsimg_status_t status;
	uint64_t slice_nodes;

	*tree = (hfsplus_tree_t){ .reader = reader, .kind = kind };
	status = hfsplus_fork_index(reader, id, HFSPLUS_FORK_DATA, fork,
	    &tree->fork, error);
	if (status != FSIMG_OK || tree->fork == NULL)
		return (status);

	/* The node size is in the node; read just enough to learn it. */
	status = hfsplus_fork_read(reader, tree->fork, 0, start, sizeof(start),
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_btree_decode(start + HFSPLUS_NODE_DESCRIPTOR_BYTES,
	    HFSPLUS_BTREE_HEADER_BYTES, &header))
		return (hfsplus_corrupt(error,
		    "HFS Plus tree header is malformed"));
	status = check_parameters(tree, &header, error);
	if (status == FSIMG_OK)
		status = check_header_fields(&header, fork->logical_size,
		    error);
	if (status != FSIMG_OK)
		return (status);
	tree->header = header;
	slice_nodes = hfsplus_state(reader)->geometry.slice_bytes /
	    header.node_size;
	tree->max_leaves = slice_nodes < header.total_nodes ?
	    (uint32_t)slice_nodes : header.total_nodes;

	tree->search_node = fsimg_alloc(reader->ctx, header.node_size, &detail);
	if (tree->search_node == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = hfsplus_node_read(tree, 0, tree->search_node,
	    HFSPLUS_NODE_HEADER, 0, &node, error);
	if (status != FSIMG_OK)
		return (status);
	status = check_header_framing(tree, tree->search_node, &node, error);
	if (status != FSIMG_OK)
		return (status);
	tree->present = true;
	return (FSIMG_OK);
}

void
hfsplus_tree_free(hfsplus_tree_t *tree)
{
	fsimg_dealloc(tree->search_node);
	fsimg_extent_index_free(tree->fork);
	tree->search_node = NULL;
	tree->fork = NULL;
	tree->present = false;
}

/* Frame a record: a two-byte keyLength that excludes itself, the key, then the
 * data at the next even offset. The node's offset table has given length.
 */
static fsimg_status_t
split_record(const hfsplus_tree_t *tree, const unsigned char *bytes,
    size_t length, hfsplus_record_t *record, fsimg_error_t *error)
{
	size_t key_length, key_bytes, data_start;

	if (length < 2)
		return (hfsplus_corrupt(error, "HFS Plus record holds no key"));
	key_length = fsimg_load_be16(bytes);
	key_bytes = 2 + key_length;
	data_start = key_bytes + key_bytes % 2;
	if (key_length > tree->header.max_key_length || data_start > length)
		return (hfsplus_corrupt(error,
		    "HFS Plus key is longer than its record or its tree allows"));
	record->key = bytes;
	record->key_bytes = key_bytes;
	record->data = bytes + data_start;
	record->data_bytes = length - data_start;
	return (FSIMG_OK);
}

/* Extents keys compare as the tuple (fileID, forkType, startBlock). */
static int
ekey_compare(const hfsplus_ekey_t *a, const hfsplus_ekey_t *b)
{
	if (a->file_id != b->file_id)
		return (a->file_id < b->file_id ? -1 : 1);
	if (a->fork_type != b->fork_type)
		return (a->fork_type < b->fork_type ? -1 : 1);
	if (a->start_block != b->start_block)
		return (a->start_block < b->start_block ? -1 : 1);
	return (0);
}

/* Attribute keys compare by fileID, then name as unsigned units on either
 * variant, then startBlock, although the name follows it in the bytes.
 */
int
hfsplus_akey_compare(const hfsplus_akey_t *a, const hfsplus_akey_t *b)
{
	int order;

	if (a->file_id != b->file_id)
		return (a->file_id < b->file_id ? -1 : 1);
	order = hfsplus_name_compare(HFSPLUS_COMPARE_BINARY, a->name, a->units,
	    b->name, b->units);
	if (order != 0)
		return (order);
	if (a->start_block != b->start_block)
		return (a->start_block < b->start_block ? -1 : 1);
	return (0);
}

/* Compare a record's key with a search key under the tree's order: *order is
 * negative, zero or positive as the stored key sorts before, equal to or after
 * the search key. The decoders refuse a key their tree cannot hold.
 */
static fsimg_status_t
key_order(const hfsplus_tree_t *tree, const hfsplus_key_t *search,
    const hfsplus_record_t *record, int *order, fsimg_error_t *error)
{
	hfsplus_ekey_t stored_extents;
	hfsplus_ckey_t stored_catalog, wanted_catalog;
	hfsplus_akey_t stored_attribute, wanted_attribute;
	bool decoded;

	switch (tree->kind) {
	case HFSPLUS_TREE_EXTENTS:
		decoded = hfsplus_ekey_decode(record->key, record->key_bytes,
		    &stored_extents);
		if (decoded)
			*order = ekey_compare(&stored_extents,
			    &search->extents);
		break;
	case HFSPLUS_TREE_CATALOG:
		decoded = hfsplus_ckey_decode(record->key, record->key_bytes,
		    &stored_catalog);
		wanted_catalog = (hfsplus_ckey_t){
			.name = search->catalog.name,
			.parent = search->catalog.parent,
			.units = search->catalog.units
		};
		if (decoded)
			*order = hfsplus_ckey_compare(tree->compare,
			    &stored_catalog, &wanted_catalog);
		break;
	case HFSPLUS_TREE_ATTRIBUTES:
		decoded = hfsplus_akey_decode(record->key, record->key_bytes,
		    &stored_attribute);
		wanted_attribute = (hfsplus_akey_t){
			.name = search->attribute.name,
			.file_id = search->attribute.file_id,
			.start_block = search->attribute.start_block,
			.units = search->attribute.units
		};
		if (decoded)
			*order = hfsplus_akey_compare(&stored_attribute,
			    &wanted_attribute);
		break;
	default:
		decoded = false;
		break;
	}
	if (!decoded)
		return (hfsplus_corrupt(error, "HFS Plus key is malformed"));
	return (FSIMG_OK);
}

/* The child to follow from an index node: the greatest index key not above the
 * search key, or the first when the search key precedes them all. The first
 * record is that fallback, and hfsplus_node_read() has required an index node
 * to hold one, so *child is always set on success.
 */
static fsimg_status_t
index_child(const hfsplus_tree_t *tree, const unsigned char *buffer,
    const hfsplus_node_t *node, const hfsplus_key_t *key, uint32_t *child,
    fsimg_error_t *error)
{
	hfsplus_record_t record;
	const unsigned char *bytes;
	uint32_t candidate;
	size_t length;
	int order;
	fsimg_status_t status;

	for (uint16_t i = 0; i < node->records; i++) {
		hfsplus_node_record(tree, buffer, i, &bytes, &length);
		status = split_record(tree, bytes, length, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_index_child(&record, &candidate))
			return (hfsplus_corrupt(error,
			    "HFS Plus index record is not a child number"));
		status = key_order(tree, key, &record, &order, error);
		if (status != FSIMG_OK)
			return (status);
		if (i == 0 || order <= 0)
			*child = candidate;
		if (order >= 0)
			break;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
enter_leaf(hfsplus_cursor_t *cursor, uint32_t number, fsimg_error_t *error)
{
	fsimg_status_t status;

	/* A chain entering more leaves than the tree declares (totalNodes) or
	 * the slice holds, whichever is smaller, is a loop.
	 */
	if (cursor->leaves >= cursor->tree->max_leaves)
		return (hfsplus_corrupt(error, "HFS Plus sibling chain loops"));
	status = hfsplus_node_read(cursor->tree, number, cursor->buffer,
	    HFSPLUS_NODE_LEAF, 1, &cursor->node, error);
	if (status != FSIMG_OK)
		return (status);
	cursor->leaves++;
	cursor->number = number;
	cursor->record = 0;
	return (FSIMG_OK);
}

/* Leave the current leaf for the next along fLink, or end the position. */
static fsimg_status_t
follow_chain(hfsplus_cursor_t *cursor, fsimg_error_t *error)
{
	uint32_t previous = cursor->number;
	uint32_t next = cursor->node.forward_link;
	fsimg_status_t status;

	cursor->valid = false;
	if (next == 0)
		return (FSIMG_OK);
	status = enter_leaf(cursor, next, error);
	if (status != FSIMG_OK)
		return (status);
	if (cursor->node.backward_link != previous)
		return (hfsplus_corrupt(error,
		    "HFS Plus sibling's back link does not name its neighbour"));
	cursor->valid = true;
	return (FSIMG_OK);
}

void
hfsplus_cursor_init(hfsplus_cursor_t *cursor, hfsplus_tree_t *tree,
    unsigned char *buffer)
{
	bzero(cursor, sizeof(*cursor));
	cursor->tree = tree;
	cursor->buffer = buffer;
}

fsimg_status_t
hfsplus_btree_search(hfsplus_cursor_t *cursor, const hfsplus_key_t *key,
    fsimg_error_t *error)
{
	hfsplus_tree_t *tree = cursor->tree;
	hfsplus_node_t node;
	hfsplus_record_t record;
	uint32_t number;
	int order;
	fsimg_status_t status;

	cursor->valid = false;
	cursor->leaves = 0;
	if (tree->header.tree_depth == 0)
		return (FSIMG_OK);

	/* Descend: each level must have exactly the height one below its
	 * parent, which rules out a child of equal height and a cycle.
	 */
	number = tree->header.root_node;
	for (unsigned height = tree->header.tree_depth; height > 1; height--) {
		status = hfsplus_node_read(tree, number, cursor->buffer,
		    HFSPLUS_NODE_INDEX, height, &node, error);
		if (status == FSIMG_OK)
			status = index_child(tree, cursor->buffer, &node, key,
			    &number, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = enter_leaf(cursor, number, error);
	if (status != FSIMG_OK)
		return (status);

	/* Step past records below the key within this leaf. */
	cursor->valid = true;
	for (; cursor->record < cursor->node.records; cursor->record++) {
		status = hfsplus_cursor_record(cursor, &record, error);
		if (status == FSIMG_OK)
			status = key_order(tree, key, &record, &order, error);
		if (status != FSIMG_OK)
			return (status);
		if (order >= 0)
			return (FSIMG_OK);
	}
	return (follow_chain(cursor, error));
}

fsimg_status_t
hfsplus_catalog_key_init(hfsplus_key_t *key, uint32_t parent,
    const unsigned char *name, size_t units, fsimg_error_t *error)
{
	if (units > HFSPLUS_NAME_UNITS_MAX)
		return (hfsplus_corrupt(error,
		    "HFS Plus catalog name is longer than a key holds"));
	key->catalog.parent = parent;
	key->catalog.units = (uint16_t)units;
	if (units != 0)
		bcopy(name, key->catalog.name, 2 * units);
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_attribute_key_init(hfsplus_key_t *key, uint32_t file_id,
    uint32_t start_block, const unsigned char *name, size_t units,
    fsimg_error_t *error)
{
	if (units > HFSPLUS_ATTR_NAME_UNITS_MAX)
		return (hfsplus_corrupt(error,
		    "HFS Plus attribute name is longer than a key holds"));
	key->attribute.file_id = file_id;
	key->attribute.start_block = start_block;
	key->attribute.units = (uint16_t)units;
	if (units != 0)
		bcopy(name, key->attribute.name, 2 * units);
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_btree_find_equivalent(hfsplus_cursor_t *cursor,
    const hfsplus_key_t *key, bool *found, fsimg_error_t *error)
{
	hfsplus_record_t record;
	int order;
	fsimg_status_t status;

	*found = false;
	status = hfsplus_btree_search(cursor, key, error);
	if (status != FSIMG_OK || !cursor->valid)
		return (status);
	status = hfsplus_cursor_record(cursor, &record, error);
	if (status == FSIMG_OK)
		status = key_order(cursor->tree, key, &record, &order, error);
	if (status != FSIMG_OK)
		return (status);
	*found = order == 0;
	return (FSIMG_OK);
}

/* A valid folded catalog cannot hold two names folding equal under one parent,
 * so the one record the search reaches decides.
 */
fsimg_status_t
hfsplus_catalog_locate(hfsplus_cursor_t *cursor, const hfsplus_key_t *key,
    hfsplus_record_t *record, bool *found, fsimg_error_t *error)
{
	hfsplus_ckey_t stored;
	fsimg_status_t status;

	status = hfsplus_btree_find_equivalent(cursor, key, found, error);
	if (status != FSIMG_OK || !*found)
		return (status);
	status = hfsplus_cursor_record(cursor, record, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_ckey_decode(record->key, record->key_bytes, &stored))
		return (hfsplus_corrupt(error, "HFS Plus key is malformed"));
	*found = stored.units == key->catalog.units &&
	    bcmp(stored.name, key->catalog.name,
	    2 * (size_t)stored.units) == 0;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_catalog_find_exact(hfsplus_tree_t *catalog, const hfsplus_key_t *key,
    hfsplus_record_t *record, bool *found, fsimg_error_t *error)
{
	hfsplus_cursor_t cursor;

	hfsplus_cursor_init(&cursor, catalog, catalog->search_node);
	return (hfsplus_catalog_locate(&cursor, key, record, found, error));
}

fsimg_status_t
hfsplus_cursor_seek(hfsplus_cursor_t *cursor, uint32_t number, uint16_t record,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	cursor->valid = false;
	cursor->leaves = 0;
	status = enter_leaf(cursor, number, error);
	if (status != FSIMG_OK)
		return (status);
	if (record >= cursor->node.records)
		return (hfsplus_corrupt(error,
		    "HFS Plus record location lies outside its leaf"));
	cursor->record = record;
	cursor->valid = true;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_cursor_next(hfsplus_cursor_t *cursor, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (!cursor->valid)
		return (FSIMG_OK);
	status = fsimg_session_charge(&cursor->tree->reader->session, 0, 0, 1,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (cursor->record + 1u < cursor->node.records) {
		cursor->record++;
		return (FSIMG_OK);
	}
	return (follow_chain(cursor, error));
}

fsimg_status_t
hfsplus_cursor_record(const hfsplus_cursor_t *cursor, hfsplus_record_t *record,
    fsimg_error_t *error)
{
	const unsigned char *bytes;
	size_t length;

	hfsplus_node_record(cursor->tree, cursor->buffer, cursor->record,
	    &bytes, &length);
	return (split_record(cursor->tree, bytes, length, record, error));
}

bool
hfsplus_index_child(const hfsplus_record_t *record, uint32_t *child)
{
	if (record->data_bytes != INDEX_DATA_BYTES)
		return (false);
	*child = fsimg_load_be32(record->data);
	return (true);
}

fsimg_status_t
hfsplus_record_compare(const hfsplus_tree_t *tree, const hfsplus_record_t *a,
    const hfsplus_record_t *b, int *order, fsimg_error_t *error)
{
	hfsplus_ekey_t extents_a, extents_b;
	hfsplus_ckey_t catalog_a, catalog_b;
	hfsplus_akey_t attribute_a, attribute_b;
	bool decoded = false;

	switch (tree->kind) {
	case HFSPLUS_TREE_EXTENTS:
		decoded = hfsplus_ekey_decode(a->key, a->key_bytes, &extents_a) &&
		    hfsplus_ekey_decode(b->key, b->key_bytes, &extents_b);
		if (decoded)
			*order = ekey_compare(&extents_a, &extents_b);
		break;
	case HFSPLUS_TREE_CATALOG:
		decoded = hfsplus_ckey_decode(a->key, a->key_bytes, &catalog_a) &&
		    hfsplus_ckey_decode(b->key, b->key_bytes, &catalog_b);
		if (decoded)
			*order = hfsplus_ckey_compare(tree->compare, &catalog_a,
			    &catalog_b);
		break;
	case HFSPLUS_TREE_ATTRIBUTES:
		decoded = hfsplus_akey_decode(a->key, a->key_bytes, &attribute_a) &&
		    hfsplus_akey_decode(b->key, b->key_bytes, &attribute_b);
		if (decoded)
			*order = hfsplus_akey_compare(&attribute_a, &attribute_b);
		break;
	}
	if (!decoded)
		return (hfsplus_corrupt(error, "HFS Plus key is malformed"));
	return (FSIMG_OK);
}

/* A level's kind follows from its height, and a child of another kind or height
 * fails the read, so the header, a map node and the node itself cannot stand in.
 */
static fsimg_status_t
level_read(hfsplus_level_t *level, uint32_t number, fsimg_error_t *error)
{
	level->number = number;
	return (hfsplus_node_read(level->tree, number, level->buffer,
	    level->height == 1 ? HFSPLUS_NODE_LEAF : HFSPLUS_NODE_INDEX,
	    level->height, &level->node, error));
}

fsimg_status_t
hfsplus_level_open(hfsplus_level_t *level, hfsplus_tree_t *tree,
    unsigned char *buffer, uint32_t first, unsigned height, fsimg_error_t *error)
{
	fsimg_status_t status;

	*level = (hfsplus_level_t){ .tree = tree, .buffer = buffer,
	    .height = height };
	status = level_read(level, first, error);
	if (status != FSIMG_OK)
		return (status);
	if (level->node.backward_link != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus level's first node has a back link"));
	level->valid = true;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_level_next(hfsplus_level_t *level, fsimg_error_t *error)
{
	uint32_t previous = level->number;
	uint32_t next = level->node.forward_link;
	fsimg_status_t status;

	level->valid = false;
	if (next == 0)
		return (FSIMG_OK);
	status = level_read(level, next, error);
	if (status != FSIMG_OK)
		return (status);
	if (level->node.backward_link != previous)
		return (hfsplus_corrupt(error,
		    "HFS Plus sibling's back link does not name its neighbour"));
	level->valid = true;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_level_record(const hfsplus_level_t *level, uint16_t index,
    hfsplus_record_t *record, fsimg_error_t *error)
{
	const unsigned char *bytes;
	size_t length;

	hfsplus_node_record(level->tree, level->buffer, index, &bytes, &length);
	return (split_record(level->tree, bytes, length, record, error));
}
