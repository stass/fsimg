/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "hfsplus.h"

/*
 * Full verification (specs/design-hfsplus-driver.md, Verification): the passes
 * run in the design's order and the first contradiction or unsupported feature
 * ends the run, so a report is never complete after a skip.
 *
 * Nothing about a whole tree stays in memory. Each pass holds a node or two and
 * sends what later passes need to scratch: three streams keyed by node ID, two
 * by hard-link reference, the ownership claims and one bitset per tree. A pass
 * reads what the one before it has already checked, so only the structure pass
 * needs its own bound on a looping chain: its visited set.
 */

/* The volume attribute saying node IDs have wrapped and may be reused. */
#define VOLUME_IDS_REUSED	UINT32_C(0x00001000)

#define BITMAP_WINDOW_BYTES	4096u
#define PAYLOAD_BUFFER_BYTES	65536u
#define ALTERNATE_FROM_END	1024u
#define INDEX_MAX_KEY_BYTES	518u	/* the catalog's maxKeyLength and its own two */

/* The map record of a tree's header node. */
#define HEADER_MAP_RECORD	2u
#define HEADER_RECORDS		3u

/* The record shared by the object and thread streams, little endian: a primary
 * record's thread must carry the same bytes in everything but the flags byte.
 * RECORD_TYPE is a thread's type; a primary stores the type its thread must have.
 */
#define RECORD_ID		0
#define RECORD_PARENT		4
#define RECORD_UNITS		8
#define RECORD_TYPE		10
#define RECORD_HAS_ATTRIBUTES	11
#define RECORD_NAME		12
#define RECORD_BYTES_MAX	(RECORD_NAME + HFSPLUS_KEY_BYTES_MAX)
/* What a primary and its thread must agree on, before the name. */
#define RECORD_PAIRED_FIRST	RECORD_PARENT
#define RECORD_PAIRED_BYTES	(RECORD_TYPE + 1 - RECORD_PARENT)

/* A hard-link stream's record: a reference and either a stub or a link count. */
#define LINK_REFERENCE		0
#define LINK_VALUE		4
#define LINK_BYTES		8u

/* An attribute owner's record: its node ID. */
#define OWNER_BYTES		4u

/* "iNode" and the decimal reference, which fits ten digits. */
#define INODE_PREFIX_UNITS	5u
#define INODE_DIGITS_MAX	10u

/* A sealed stream read in order, the record it stands on in bytes. Scratch that
 * reads back shorter than its stream's fixed part is a fault, not an image's.
 */
typedef struct stream_reader {
	fsimg_stream_t	*stream;
	uint64_t	 offset;
	size_t		 minimum;
	size_t		 length;
	bool		 valid;
	unsigned char	 bytes[RECORD_BYTES_MAX];
} stream_reader_t;

/* One verification. The claims and streams are owned here, released by
 * verify_release() on every path. The counters are what later passes compare.
 */
typedef struct verify {
	fsimg_reader_t		*reader;
	fsimg_hfsplus_reader_t	*hfsplus;
	fsimg_verify_report_t	*report;
	fsimg_owner_runs_t	*claims;
	fsimg_stream_t		*objects;	/* one per primary record */
	fsimg_stream_t		*threads;	/* in key order, so by node ID */
	fsimg_stream_t		*owners;	/* one per attribute owner, ascending */
	fsimg_stream_t		*links;		/* (reference, stub ID) */
	fsimg_stream_t		*inodes;	/* (reference, link count) */
	unsigned char		*payload;	/* a read-through's buffer, or NULL */
	uint64_t		 files;
	uint64_t		 folders;	/* without the root */
	uint64_t		 primaries;	/* with the root */
	uint64_t		 private_records;
	uint64_t		 owned_records;
	uint64_t		 overflow_records;
	uint64_t		 bad_blocks;
	uint64_t		 reached;
} verify_t;

/* One tree's structure pass. nodes holds the level being checked and index the
 * level above it, read again for the merge of their records and nodes.
 */
typedef struct tree_pass {
	fsimg_reader_t	*reader;
	hfsplus_tree_t	*tree;
	fsimg_bitset_t	*visited;
	unsigned char	*nodes;
	unsigned char	*index;
	uint64_t	 leaf_records;
	uint32_t	 last_leaf;
	size_t		 previous_bytes;
	unsigned char	 previous[INDEX_MAX_KEY_BYTES];
} tree_pass_t;

/* The attribute fork whose continuation records the pass is expecting. */
typedef struct attribute_fork {
	uint64_t	 appended;
	uint32_t	 total;
	uint32_t	 file_id;
	uint16_t	 units;
	unsigned char	 name[2 * HFSPLUS_ATTR_NAME_UNITS_MAX];
} attribute_fork_t;

/* The bitmap's bytes through a window, loaded as a range reaches them. */
typedef struct bitmap_scan {
	fsimg_reader_t	*reader;
	uint64_t	 offset;	/* of the window's first byte */
	size_t		 loaded;
	uint64_t	 logical_size;
	unsigned char	 window[BITMAP_WINDOW_BYTES];
} bitmap_scan_t;

static fsimg_status_t preflight(verify_t *, fsimg_error_t *);
static fsimg_status_t visit_node(tree_pass_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t map_chain_mark(tree_pass_t *, fsimg_error_t *);
static fsimg_status_t map_bits_compare(tree_pass_t *, const unsigned char *,
    size_t, uint64_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t map_compare(tree_pass_t *, fsimg_error_t *);
static fsimg_status_t index_pair(hfsplus_level_t *, uint16_t *, uint32_t,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t node_records_check(tree_pass_t *, const hfsplus_level_t *,
    hfsplus_level_t *, uint16_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t level_check(tree_pass_t *, unsigned, uint32_t, uint32_t,
    uint32_t *, fsimg_error_t *);
static fsimg_status_t tree_levels_check(tree_pass_t *, fsimg_error_t *);
static fsimg_status_t tree_structure(fsimg_reader_t *, hfsplus_tree_t *,
    fsimg_error_t *);
static fsimg_status_t name_record_append(fsimg_stream_t *, uint32_t, uint32_t,
    uint16_t, const unsigned char *, uint16_t, bool, fsimg_error_t *);
static fsimg_status_t link_append(fsimg_stream_t *, uint32_t, uint32_t,
    fsimg_error_t *);
static bool inode_reference(const hfsplus_ckey_t *, uint32_t *);
static fsimg_status_t thread_take(verify_t *, const hfsplus_ckey_t *,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t root_take(verify_t *, const hfsplus_ckey_t *,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t primary_take(verify_t *, const hfsplus_ckey_t *,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t catalog_pass(verify_t *, fsimg_error_t *);
static fsimg_status_t attribute_fork_finish(const attribute_fork_t *,
    fsimg_error_t *);
static fsimg_status_t attribute_take(verify_t *, const hfsplus_akey_t *,
    const hfsplus_attr_t *, attribute_fork_t *, fsimg_error_t *);
static fsimg_status_t attributes_pass(verify_t *, fsimg_error_t *);
static fsimg_status_t special_storage(verify_t *, fsimg_error_t *);
static fsimg_status_t stream_sorted(fsimg_stream_t **, fsimg_compare_fn_t,
    fsimg_error_t *);
static int id_compare(const void *, size_t, const void *, size_t, void *);
static int reference_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t reader_open(verify_t *, stream_reader_t *,
    fsimg_stream_t *, size_t, fsimg_error_t *);
static fsimg_status_t reader_advance(verify_t *, stream_reader_t *,
    fsimg_error_t *);
static fsimg_status_t object_merge(verify_t *, const stream_reader_t *,
    stream_reader_t *, stream_reader_t *, fsimg_error_t *);
static fsimg_status_t objects_merge(verify_t *, fsimg_error_t *);
static fsimg_status_t links_merge(verify_t *, fsimg_error_t *);
static fsimg_status_t merges(verify_t *, fsimg_error_t *);
static fsimg_status_t content_check(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t reachability(verify_t *, fsimg_error_t *);
static fsimg_status_t bitmap_byte(bitmap_scan_t *, uint64_t, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t bitmap_range(bitmap_scan_t *, uint64_t, uint64_t, bool,
    const char *, fsimg_error_t *);
static fsimg_status_t bitmap_gap(bitmap_scan_t *, uint64_t, uint64_t, uint64_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t bitmap_compare(verify_t *, fsimg_error_t *);
static bool fork_equal(const hfsplus_fork_t *, const hfsplus_fork_t *);
static fsimg_status_t alternate_compare(verify_t *, fsimg_error_t *);
static void verify_release(verify_t *);

/* What this driver cannot verify is refused before any pass: a label the display
 * mapping refused, read at open, and a directory hard link's target, since the
 * driver walks none. The keyed searches here also give the catalog pass the
 * private data folder's ID. A file under the private directory name is no folder;
 * a folder whose valence understates its children fails reachability later.
 */
static fsimg_status_t
preflight(verify_t *verify, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = verify->hfsplus;
	const hfsplus_owned_name_t *name =
	    &hfsplus_owned_names[HFSPLUS_OWNED_PRIVATE_DIRECTORY];
	hfsplus_folder_t folder;
	hfsplus_record_t record;
	hfsplus_key_t key;
	bool found;
	fsimg_status_t status;

	if (!hfsplus->label_valid)
		return (hfsplus_unsupported(error,
		    "HFS Plus volume's label cannot be displayed"));
	status = hfsplus_catalog_key_init(&key, HFSPLUS_ID_ROOT, name->units,
	    name->count, error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_find_exact(&hfsplus->catalog, &key,
		    &record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (found) {
		if (!hfsplus_folder_decode(record.data, record.data_bytes,
		    &folder))
			return (hfsplus_corrupt(error,
			    "HFS Plus private folder is not a folder"));
		if (folder.valence != 0)
			return (hfsplus_unsupported(error,
			    "HFS Plus directory hard links are not supported"));
	}
	if (hfsplus->private_data_known)
		return (FSIMG_OK);
	return (hfsplus_private_data_lookup(verify->reader, error));
}

/* Mark a node visited. Every node of the tree is marked once, so this is also
 * what ends a looping chain: its first repeat fails here.
 */
static fsimg_status_t
visit_node(tree_pass_t *pass, uint32_t number, fsimg_error_t *error)
{
	bool visited;
	fsimg_status_t status;

	status = fsimg_bitset_test_and_set(pass->visited, number, &visited, error);
	if (status != FSIMG_OK)
		return (status);
	if (visited)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree reaches a node twice"));
	return (FSIMG_OK);
}

/* Node 0 and the chain of map nodes from its fLink are in use without being in
 * any level. Each map node holds one record, whose length its table gives.
 */
static fsimg_status_t
map_chain_mark(tree_pass_t *pass, fsimg_error_t *error)
{
	hfsplus_node_t node;
	fsimg_status_t status;

	status = visit_node(pass, 0, error);
	if (status == FSIMG_OK)
		status = hfsplus_node_read(pass->tree, 0, pass->nodes,
		    HFSPLUS_NODE_HEADER, 0, &node, error);
	while (status == FSIMG_OK && node.forward_link != 0) {
		status = visit_node(pass, node.forward_link, error);
		if (status != FSIMG_OK)
			break;
		status = hfsplus_node_read(pass->tree, node.forward_link,
		    pass->nodes, HFSPLUS_NODE_MAP, 0, &node, error);
		if (status != FSIMG_OK)
			break;
		if (node.backward_link != 0 || node.records != 1)
			return (hfsplus_corrupt(error,
			    "HFS Plus map node is malformed"));
	}
	return (status);
}

/* Compare one map record, most significant bit first, with the visited set from
 * node *position on. Bits at or past totalNodes must be clear.
 */
static fsimg_status_t
map_bits_compare(tree_pass_t *pass, const unsigned char *bytes, size_t length,
    uint64_t *position, uint64_t *free_nodes, fsimg_error_t *error)
{
	uint64_t total = pass->tree->header.total_nodes;
	bool visited, marked;
	fsimg_status_t status;

	for (size_t i = 0; i < length; i++) {
		for (unsigned bit = 0; bit < 8; bit++) {
			marked = (bytes[i] & (0x80u >> bit)) != 0;
			if (*position + bit >= total) {
				if (marked)
					return (hfsplus_corrupt(error,
					    "HFS Plus node map marks a node past the tree's end"));
				continue;
			}
			status = fsimg_bitset_test(pass->visited, *position + bit,
			    &visited, error);
			if (status != FSIMG_OK)
				return (status);
			if (marked && !visited)
				return (hfsplus_corrupt(error,
				    "HFS Plus node map marks a node the tree does not use"));
			if (!marked && visited)
				return (hfsplus_corrupt(error,
				    "HFS Plus tree uses a node its map leaves free"));
			if (!marked)
				(*free_nodes)++;
		}
		*position += 8;
	}
	return (FSIMG_OK);
}

/* The visited set now holds every node the tree uses: the map must say exactly
 * that, and its clear bits are freeNodes. The chain must cover totalNodes.
 */
static fsimg_status_t
map_compare(tree_pass_t *pass, fsimg_error_t *error)
{
	const unsigned char *bytes;
	hfsplus_node_t node;
	uint64_t position = 0, free_nodes = 0;
	size_t length;
	uint16_t record = HEADER_MAP_RECORD;
	fsimg_status_t status;

	status = hfsplus_node_read(pass->tree, 0, pass->nodes,
	    HFSPLUS_NODE_HEADER, 0, &node, error);
	if (status != FSIMG_OK)
		return (status);
	if (node.records != HEADER_RECORDS)
		return (hfsplus_corrupt(error,
		    "HFS Plus header node's records are misplaced"));
	for (;;) {
		hfsplus_node_record(pass->tree, pass->nodes, record, &bytes,
		    &length);
		status = map_bits_compare(pass, bytes, length, &position,
		    &free_nodes, error);
		if (status != FSIMG_OK)
			return (status);
		if (node.forward_link == 0)
			break;
		status = hfsplus_node_read(pass->tree, node.forward_link,
		    pass->nodes, HFSPLUS_NODE_MAP, 0, &node, error);
		if (status != FSIMG_OK)
			return (status);
		if (node.records != 1)
			return (hfsplus_corrupt(error,
			    "HFS Plus map node is malformed"));
		record = 0;
	}
	if (position < pass->tree->header.total_nodes)
		return (hfsplus_corrupt(error,
		    "HFS Plus node map's chain does not cover the tree"));
	if (free_nodes != pass->tree->header.free_nodes)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's freeNodes disagrees with its map"));
	return (FSIMG_OK);
}

/* The next index record across the level above must name this node and carry its
 * first key: the separator and child checks, as a merge of two sequential streams.
 */
static fsimg_status_t
index_pair(hfsplus_level_t *index, uint16_t *position, uint32_t number,
    const hfsplus_record_t *first, fsimg_error_t *error)
{
	hfsplus_record_t record;
	uint32_t child;
	fsimg_status_t status;

	if (*position == index->node.records) {
		status = hfsplus_level_next(index, error);
		if (status != FSIMG_OK)
			return (status);
		if (!index->valid)
			return (hfsplus_corrupt(error,
			    "HFS Plus level holds a node no index record names"));
		*position = 0;
	}
	status = hfsplus_level_record(index, *position, &record, error);
	if (status != FSIMG_OK)
		return (status);
	(*position)++;
	if (!hfsplus_index_child(&record, &child))
		return (hfsplus_corrupt(error,
		    "HFS Plus index record is not a child number"));
	if (child != number)
		return (hfsplus_corrupt(error,
		    "HFS Plus index record does not name the next node of its level"));
	if (record.key_bytes != first->key_bytes ||
	    bcmp(record.key, first->key, first->key_bytes) != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus index key differs from its child's first key"));
	return (FSIMG_OK);
}

/* Every record of the level's current node: keys strictly ascending across the
 * level, an index node's records naming children, the first record paired with
 * the level above when there is one. *first_child takes a first record's child.
 */
static fsimg_status_t
node_records_check(tree_pass_t *pass, const hfsplus_level_t *nodes,
    hfsplus_level_t *index, uint16_t *index_position, uint32_t *first_child,
    fsimg_error_t *error)
{
	hfsplus_record_t record, previous;
	uint32_t child;
	int order;
	fsimg_status_t status;

	for (uint16_t i = 0; i < nodes->node.records; i++) {
		status = hfsplus_level_record(nodes, i, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (pass->previous_bytes != 0) {
			previous = (hfsplus_record_t){ .key = pass->previous,
			    .key_bytes = pass->previous_bytes };
			status = hfsplus_record_compare(pass->tree, &previous,
			    &record, &order, error);
			if (status != FSIMG_OK)
				return (status);
			if (order >= 0)
				return (hfsplus_corrupt(error,
				    "HFS Plus tree's keys do not ascend"));
		}
		if (record.key_bytes > sizeof(pass->previous))
			return (hfsplus_corrupt(error, "HFS Plus key is malformed"));
		bcopy(record.key, pass->previous, record.key_bytes);
		pass->previous_bytes = record.key_bytes;
		if (nodes->height > 1) {
			if (!hfsplus_index_child(&record, &child))
				return (hfsplus_corrupt(error,
				    "HFS Plus index record is not a child number"));
			if (i == 0)
				*first_child = child;
		}
		if (i == 0 && index->valid) {
			status = index_pair(index, index_position, nodes->number,
			    &record, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

/* Check one level, leftmost node first, and give the leftmost child of its first
 * node for the level below. A level other than the root's merges with the index
 * records above it, which must end when it does; the root's level is the root
 * alone.
 */
static fsimg_status_t
level_check(tree_pass_t *pass, unsigned height, uint32_t first,
    uint32_t upper_first, uint32_t *next_first, fsimg_error_t *error)
{
	const hfsplus_btree_t *header = &pass->tree->header;
	bool top = height == header->tree_depth;
	hfsplus_level_t nodes;
	hfsplus_level_t index = { .valid = false };
	uint16_t index_position = 0;
	uint32_t first_child = 0;
	fsimg_status_t status;

	pass->previous_bytes = 0;
	status = hfsplus_level_open(&nodes, pass->tree, pass->nodes, first, height,
	    error);
	if (status == FSIMG_OK && !top)
		status = hfsplus_level_open(&index, pass->tree, pass->index,
		    upper_first, height + 1, error);
	while (status == FSIMG_OK && nodes.valid) {
		status = visit_node(pass, nodes.number, error);
		if (status != FSIMG_OK)
			break;
		if (top && nodes.node.forward_link != 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus tree's root has a sibling"));
		status = node_records_check(pass, &nodes, &index, &index_position,
		    &first_child, error);
		if (status != FSIMG_OK)
			break;
		if (nodes.number == first)
			*next_first = first_child;
		if (height == 1) {
			pass->leaf_records += nodes.node.records;
			pass->last_leaf = nodes.number;
		}
		status = fsimg_session_charge(&pass->reader->session, 0, 0, 1,
		    error);
		if (status == FSIMG_OK)
			status = hfsplus_level_next(&nodes, error);
	}
	if (status != FSIMG_OK)
		return (status);
	if (!top && (index_position != index.node.records ||
	    index.node.forward_link != 0))
		return (hfsplus_corrupt(error,
		    "HFS Plus index record names no node of the level below"));
	return (FSIMG_OK);
}

/* The levels from the root down, then what the iteration counted against the
 * header. An empty tree has no level and no leaf record.
 */
static fsimg_status_t
tree_levels_check(tree_pass_t *pass, fsimg_error_t *error)
{
	const hfsplus_btree_t *header = &pass->tree->header;
	uint32_t first = header->root_node, upper_first = 0, next_first = 0;
	fsimg_status_t status;

	for (unsigned height = header->tree_depth; height >= 1; height--) {
		if (height == 1 && first != header->first_leaf_node)
			return (hfsplus_corrupt(error,
			    "HFS Plus tree's firstLeafNode is not its first leaf"));
		status = level_check(pass, height, first, upper_first,
		    &next_first, error);
		if (status != FSIMG_OK)
			return (status);
		upper_first = first;
		first = next_first;
	}
	if (pass->leaf_records != header->leaf_records)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's leafRecords disagrees with its leaves"));
	if (pass->last_leaf != header->last_leaf_node)
		return (hfsplus_corrupt(error,
		    "HFS Plus tree's lastLeafNode is not its last leaf"));
	return (FSIMG_OK);
}

/* Step 1 for one tree: every node of every level once, and the map. An absent
 * tree is valid and holds nothing.
 */
static fsimg_status_t
tree_structure(fsimg_reader_t *reader, hfsplus_tree_t *tree,
    fsimg_error_t *error)
{
	tree_pass_t pass = { .reader = reader, .tree = tree };
	fsimg_error_t detail;
	fsimg_status_t status;

	if (!tree->present)
		return (FSIMG_OK);
	pass.nodes = fsimg_alloc(reader->ctx, tree->header.node_size, &detail);
	if (pass.nodes != NULL)
		pass.index = fsimg_alloc(reader->ctx, tree->header.node_size,
		    &detail);
	if (pass.index == NULL) {
		fsimg_dealloc(pass.nodes);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_bitset_new(&reader->session, tree->header.total_nodes,
	    &pass.visited, error);
	if (status == FSIMG_OK)
		status = map_chain_mark(&pass, error);
	if (status == FSIMG_OK)
		status = tree_levels_check(&pass, error);
	if (status == FSIMG_OK)
		status = map_compare(&pass, error);
	fsimg_bitset_free(pass.visited);
	fsimg_dealloc(pass.nodes);
	fsimg_dealloc(pass.index);
	return (status);
}

static fsimg_status_t
name_record_append(fsimg_stream_t *stream, uint32_t id, uint32_t parent,
    uint16_t type, const unsigned char *name, uint16_t units, bool has_attributes,
    fsimg_error_t *error)
{
	unsigned char record[RECORD_BYTES_MAX];

	fsimg_store_le32(record + RECORD_ID, id);
	fsimg_store_le32(record + RECORD_PARENT, parent);
	fsimg_store_le16(record + RECORD_UNITS, units);
	record[RECORD_TYPE] = (unsigned char)type;
	record[RECORD_HAS_ATTRIBUTES] = has_attributes ? 1 : 0;
	bcopy(name, record + RECORD_NAME, 2 * (size_t)units);
	return (fsimg_stream_append(stream, record,
	    RECORD_NAME + 2 * (size_t)units, NULL, error));
}

static fsimg_status_t
link_append(fsimg_stream_t *stream, uint32_t reference, uint32_t value,
    fsimg_error_t *error)
{
	unsigned char record[LINK_BYTES];

	fsimg_store_le32(record + LINK_REFERENCE, reference);
	fsimg_store_le32(record + LINK_VALUE, value);
	return (fsimg_stream_append(stream, record, sizeof(record), NULL, error));
}

/* The reference of a name spelled exactly as a stub's reference would find it:
 * "iNode" and a decimal value without a leading zero. Any other name in the private
 * data folder, a temp file's among them, is no indirect node.
 */
static bool
inode_reference(const hfsplus_ckey_t *key, uint32_t *reference)
{
	static const char prefix[] = "iNode";
	uint64_t value = 0;

	if (key->units <= INODE_PREFIX_UNITS ||
	    key->units > INODE_PREFIX_UNITS + INODE_DIGITS_MAX)
		return (false);
	for (unsigned i = 0; i < key->units; i++) {
		uint16_t unit = fsimg_load_be16(key->name + 2 * (size_t)i);

		if (i < INODE_PREFIX_UNITS) {
			if (unit != (unsigned char)prefix[i])
				return (false);
			continue;
		}
		if (unit < '0' || unit > '9' ||
		    (i == INODE_PREFIX_UNITS && unit == '0'))
			return (false);
		value = value * 10 + (unit - '0');
	}
	if (value > UINT32_MAX)
		return (false);
	*reference = (uint32_t)value;
	return (true);
}

/* A thread's key is (id, ""). Its key ordering puts threads in node-ID order, so
 * they need no sort.
 */
static fsimg_status_t
thread_take(verify_t *verify, const hfsplus_ckey_t *key,
    const hfsplus_record_t *record, fsimg_error_t *error)
{
	hfsplus_thread_t thread;

	if (key->units != 0 ||
	    !hfsplus_thread_decode(record->data, record->data_bytes, &thread))
		return (hfsplus_corrupt(error,
		    "HFS Plus thread record is malformed"));
	return (name_record_append(verify->threads, key->parent, thread.parent,
	    thread.type, thread.name, thread.units, false, error));
}

/* Open validated the root; the one record under its parent must be it. Its
 * flags and name still go through the merge as any primary's do.
 */
static fsimg_status_t
root_take(verify_t *verify, const hfsplus_ckey_t *key,
    const hfsplus_record_t *record, fsimg_error_t *error)
{
	hfsplus_folder_t folder;

	if (!hfsplus_folder_decode(record->data, record->data_bytes, &folder) ||
	    folder.id != HFSPLUS_ID_ROOT)
		return (hfsplus_corrupt(error,
		    "HFS Plus root record is not folder 2"));
	verify->primaries++;
	return (name_record_append(verify->objects, folder.id, key->parent,
	    HFSPLUS_RECORD_FOLDER_THREAD, key->name, key->units,
	    (folder.flags & HFSPLUS_FLAG_ATTRIBUTES) != 0, error));
}

/* A folder or file record. Every record of the catalog comes through here once,
 * so every file's both forks are resolved and claimed once, reachable or not.
 * Whatever holds no thread or parent is found by the merge and reachability.
 */
static fsimg_status_t
primary_take(verify_t *verify, const hfsplus_ckey_t *key,
    const hfsplus_record_t *record, fsimg_error_t *error)
{
	fsimg_reader_t *reader = verify->reader;
	const fsimg_hfsplus_reader_t *hfsplus = verify->hfsplus;
	hfsplus_primary_t primary;
	hfsplus_owned_t owned;
	bool folder, in_private_data;
	uint32_t id, reference;
	uint16_t flags;
	fsimg_status_t status;

	if (key->units == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder or file record has an empty name"));
	if (key->parent == HFSPLUS_ID_ROOT_PARENT)
		return (root_take(verify, key, record, error));
	status = hfsplus_record_validate(reader, record, &primary, error);
	if (status != FSIMG_OK)
		return (status);
	if (primary.link == HFSPLUS_LINK_DIRECTORY)
		return (hfsplus_unsupported(error,
		    "HFS Plus directory hard links are not supported"));
	folder = primary.kind == FSIMG_ENTRY_DIRECTORY;
	id = folder ? primary.folder.id : primary.file.id;
	flags = folder ? primary.folder.flags : primary.file.flags;
	in_private_data = hfsplus->private_data_present &&
	    key->parent == hfsplus->private_data_id;

	if (in_private_data) {
		if (folder)
			return (hfsplus_corrupt(error,
			    "HFS Plus private data folder holds a folder"));
		verify->private_records++;
		if (primary.link == HFSPLUS_LINK_NONE &&
		    inode_reference(key, &reference)) {
			status = link_append(verify->inodes, reference,
			    primary.file.bsd.special, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	if (hfsplus_owned_find(hfsplus, key, &owned))
		verify->owned_records++;
	verify->primaries++;
	if (folder)
		verify->folders++;
	else {
		verify->files++;
		status = hfsplus_fork_claim(reader, verify->claims, id,
		    HFSPLUS_FORK_DATA, &primary.file.data_fork,
		    &verify->overflow_records, error);
		if (status == FSIMG_OK)
			status = hfsplus_fork_claim(reader, verify->claims, id,
			    HFSPLUS_FORK_RESOURCE, &primary.file.resource_fork,
			    &verify->overflow_records, error);
		if (status == FSIMG_OK && primary.link == HFSPLUS_LINK_FILE)
			status = link_append(verify->links,
			    primary.file.bsd.special, id, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (name_record_append(verify->objects, id, key->parent,
	    folder ? HFSPLUS_RECORD_FOLDER_THREAD : HFSPLUS_RECORD_FILE_THREAD,
	    key->name, key->units, (flags & HFSPLUS_FLAG_ATTRIBUTES) != 0, error));
}

/* Step 2: the catalog's leaves, which step 1 has checked, are the inventory. */
static fsimg_status_t
catalog_pass(verify_t *verify, fsimg_error_t *error)
{
	hfsplus_tree_t *tree = &verify->hfsplus->catalog;
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_ckey_t key;
	uint16_t type;
	fsimg_status_t status;

	hfsplus_cursor_init(&cursor, tree, verify->hfsplus->catalog_scan_node);
	status = hfsplus_cursor_seek(&cursor, tree->header.first_leaf_node, 0,
	    error);
	while (status == FSIMG_OK && cursor.valid) {
		status = hfsplus_cursor_record(&cursor, &record, error);
		if (status != FSIMG_OK)
			break;
		if (!hfsplus_ckey_decode(record.key, record.key_bytes, &key) ||
		    !hfsplus_catalog_type(record.data, record.data_bytes, &type))
			return (hfsplus_corrupt(error,
			    "HFS Plus catalog record is malformed"));
		if (type == HFSPLUS_RECORD_FOLDER_THREAD ||
		    type == HFSPLUS_RECORD_FILE_THREAD)
			status = thread_take(verify, &key, &record, error);
		else
			status = primary_take(verify, &key, &record, error);
		if (status == FSIMG_OK)
			status = hfsplus_cursor_next(&cursor, error);
	}
	return (status);
}

static fsimg_status_t
attribute_fork_finish(const attribute_fork_t *fork, fsimg_error_t *error)
{
	if (fork->appended != fork->total)
		return (hfsplus_corrupt(error,
		    "HFS Plus attribute fork's extents fall short of its totalBlocks"));
	return (FSIMG_OK);
}

/* One attribute record, in key order. A fork record starts a fork at logical
 * block 0, and only its own continuation records may follow it until the fork is
 * whole; any record ending it early is refused.
 */
static fsimg_status_t
attribute_take(verify_t *verify, const hfsplus_akey_t *key,
    const hfsplus_attr_t *attr, attribute_fork_t *fork, fsimg_error_t *error)
{
	uint64_t blocks;
	fsimg_status_t status;

	switch (attr->type) {
	case HFSPLUS_ATTR_INLINE:
	case HFSPLUS_ATTR_FORK:
		status = attribute_fork_finish(fork, error);
		if (status != FSIMG_OK)
			return (status);
		if (key->start_block != 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute's first record does not start at block 0"));
		fork->appended = 0;
		fork->total = 0;
		if (attr->type == HFSPLUS_ATTR_INLINE)
			return (FSIMG_OK);
		status = hfsplus_attr_fork_claim(verify->reader, verify->claims,
		    &attr->fork, &fork->appended, error);
		if (status != FSIMG_OK)
			return (status);
		fork->total = attr->fork.total_blocks;
		fork->file_id = key->file_id;
		fork->units = key->units;
		bcopy(key->name, fork->name, 2 * (size_t)key->units);
		return (FSIMG_OK);
	case HFSPLUS_ATTR_EXTENTS:
		if (fork->appended == fork->total || fork->file_id != key->file_id ||
		    fork->units != key->units ||
		    bcmp(fork->name, key->name, 2 * (size_t)key->units) != 0 ||
		    key->start_block != fork->appended)
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute extents record does not continue a fork"));
		status = hfsplus_attr_extents_claim(verify->reader, verify->claims,
		    attr->extents, &blocks, error);
		if (status != FSIMG_OK)
			return (status);
		if (blocks > fork->total - fork->appended)
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute fork's extents exceed its totalBlocks"));
		fork->appended += blocks;
		return (FSIMG_OK);
	default:
		return (hfsplus_unsupported(error,
		    "HFS Plus attribute record has an unknown type"));
	}
}

/* Step 3: the attributes tree's leaves, in node-ID order, yield each owner once. */
static fsimg_status_t
attributes_pass(verify_t *verify, fsimg_error_t *error)
{
	hfsplus_tree_t *tree = &verify->hfsplus->attributes;
	unsigned char owner[OWNER_BYTES];
	attribute_fork_t fork = { .total = 0 };
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_akey_t key;
	hfsplus_attr_t attr;
	uint32_t previous = 0;
	fsimg_status_t status;

	if (!tree->present || tree->header.tree_depth == 0)
		return (FSIMG_OK);
	hfsplus_cursor_init(&cursor, tree, tree->search_node);
	status = hfsplus_cursor_seek(&cursor, tree->header.first_leaf_node, 0,
	    error);
	while (status == FSIMG_OK && cursor.valid) {
		status = hfsplus_cursor_record(&cursor, &record, error);
		if (status != FSIMG_OK)
			break;
		if (!hfsplus_akey_decode(record.key, record.key_bytes, &key) ||
		    key.units == 0 ||
		    !hfsplus_attr_decode(record.data, record.data_bytes, &attr))
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute record is malformed"));
		status = attribute_take(verify, &key, &attr, &fork, error);
		if (status == FSIMG_OK && key.file_id != previous) {
			fsimg_store_le32(owner, key.file_id);
			status = fsimg_stream_append(verify->owners, owner,
			    sizeof(owner), NULL, error);
			previous = key.file_id;
		}
		if (status == FSIMG_OK)
			status = hfsplus_cursor_next(&cursor, error);
	}
	if (status != FSIMG_OK)
		return (status);
	return (attribute_fork_finish(&fork, error));
}

/* Step 4: what no catalog record owns. The journal is not here: its two files'
 * records were claimed by the catalog pass.
 */
static fsimg_status_t
special_storage(verify_t *verify, fsimg_error_t *error)
{
	const hfsplus_geometry_t *g = &verify->hfsplus->geometry;
	const hfsplus_header_t *header = &verify->hfsplus->header;
	const struct {
		uint32_t		 id;
		const hfsplus_fork_t	*fork;
	} specials[] = {
		{ HFSPLUS_ID_ALLOCATION, &header->allocation_file },
		{ HFSPLUS_ID_EXTENTS, &header->extents_file },
		{ HFSPLUS_ID_CATALOG, &header->catalog_file },
		{ HFSPLUS_ID_ATTRIBUTES, &header->attributes_file },
		{ HFSPLUS_ID_STARTUP, &header->startup_file }
	};
	fsimg_status_t status;

	status = fsimg_owner_runs_append(verify->claims, 0, g->first_block, error);
	if (status == FSIMG_OK && g->end_block < g->blocks)
		status = fsimg_owner_runs_append(verify->claims, g->end_block,
		    g->blocks - g->end_block, error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(specials) / sizeof(specials[0]); i++)
		status = hfsplus_fork_claim(verify->reader, verify->claims,
		    specials[i].id, HFSPLUS_FORK_DATA, specials[i].fork,
		    &verify->overflow_records, error);
	if (status == FSIMG_OK)
		status = hfsplus_bad_blocks_claim(verify->reader, verify->claims,
		    &verify->overflow_records, &verify->bad_blocks, error);
	return (status);
}

/* Replace a stream by its sorted copy. */
static fsimg_status_t
stream_sorted(fsimg_stream_t **stream, fsimg_compare_fn_t compare,
    fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	fsimg_status_t status;

	status = fsimg_stream_seal(*stream, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(*stream, compare, NULL, NULL, &sorted,
		    error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_stream_free(*stream);
	*stream = sorted;
	return (FSIMG_OK);
}

/* Comparators cannot report errors: a record too short to load compares by length,
 * and the merge rejects its length on reading it.
 */
static int
id_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint32_t ida, idb;

	if (alen < RECORD_NAME || blen < RECORD_NAME)
		return ((alen > blen) - (alen < blen));
	ida = fsimg_load_le32((const unsigned char *)a + RECORD_ID);
	idb = fsimg_load_le32((const unsigned char *)b + RECORD_ID);
	return ((ida > idb) - (ida < idb));
}

static int
reference_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint32_t refa, refb;

	if (alen != LINK_BYTES || blen != LINK_BYTES)
		return ((alen > blen) - (alen < blen));
	refa = fsimg_load_le32((const unsigned char *)a + LINK_REFERENCE);
	refb = fsimg_load_le32((const unsigned char *)b + LINK_REFERENCE);
	return ((refa > refb) - (refa < refb));
}

static fsimg_status_t
reader_open(verify_t *verify, stream_reader_t *reader, fsimg_stream_t *stream,
    size_t minimum, fsimg_error_t *error)
{
	reader->stream = stream;
	reader->offset = 0;
	reader->minimum = minimum;
	return (reader_advance(verify, reader, error));
}

/* Each record read is one work unit, so a merge polls for cancellation. */
static fsimg_status_t
reader_advance(verify_t *verify, stream_reader_t *reader, fsimg_error_t *error)
{
	fsimg_status_t status;

	reader->valid = false;
	status = fsimg_session_charge(&verify->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_next(reader->stream, &reader->offset, reader->bytes,
	    sizeof(reader->bytes), &reader->length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (reader->length < reader->minimum)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_VERIFY,
		    "HFS Plus verification's scratch record is malformed"));
	reader->valid = true;
	return (FSIMG_OK);
}

/* One primary, in node-ID order, against the next thread and attribute owner. A
 * thread or owner below it has no primary, as nothing below it remains.
 */
static fsimg_status_t
object_merge(verify_t *verify, const stream_reader_t *object,
    stream_reader_t *thread, stream_reader_t *owner, fsimg_error_t *error)
{
	uint32_t id = fsimg_load_le32(object->bytes + RECORD_ID);
	bool owns_attributes = false;
	fsimg_status_t status;

	if (thread->valid && fsimg_load_le32(thread->bytes + RECORD_ID) < id)
		return (hfsplus_corrupt(error, "HFS Plus thread has no primary record"));
	if (!thread->valid || fsimg_load_le32(thread->bytes + RECORD_ID) != id)
		return (hfsplus_corrupt(error, "HFS Plus thread record is missing"));
	if (thread->length != object->length ||
	    bcmp(thread->bytes + RECORD_PAIRED_FIRST,
	    object->bytes + RECORD_PAIRED_FIRST, RECORD_PAIRED_BYTES) != 0 ||
	    bcmp(thread->bytes + RECORD_NAME, object->bytes + RECORD_NAME,
	    object->length - RECORD_NAME) != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus thread does not match its record"));
	status = reader_advance(verify, thread, error);
	if (status != FSIMG_OK)
		return (status);

	if (owner->valid) {
		uint32_t owner_id = fsimg_load_le32(owner->bytes);

		if (owner_id < id)
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute names no primary record"));
		owns_attributes = owner_id == id;
	}
	if (owns_attributes != (object->bytes[RECORD_HAS_ATTRIBUTES] != 0))
		return (hfsplus_corrupt(error,
		    "HFS Plus record's attribute flag disagrees with the attributes tree"));
	return (owns_attributes ? reader_advance(verify, owner, error) : FSIMG_OK);
}

/* Step 5's first merge. Sorted by ID, a repeated ID is two adjacent records, and
 * the thread of the first leaves the second none.
 */
static fsimg_status_t
objects_merge(verify_t *verify, fsimg_error_t *error)
{
	stream_reader_t object, thread, owner;
	uint32_t largest = 0;
	fsimg_status_t status;

	status = stream_sorted(&verify->objects, id_compare, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(verify->threads, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(verify->owners, error);
	if (status == FSIMG_OK)
		status = reader_open(verify, &object, verify->objects,
		    RECORD_NAME, error);
	if (status == FSIMG_OK)
		status = reader_open(verify, &thread, verify->threads,
		    RECORD_NAME, error);
	if (status == FSIMG_OK)
		status = reader_open(verify, &owner, verify->owners,
		    OWNER_BYTES, error);
	while (status == FSIMG_OK && object.valid) {
		status = object_merge(verify, &object, &thread, &owner, error);
		if (status != FSIMG_OK)
			return (status);
		largest = fsimg_load_le32(object.bytes + RECORD_ID);
		status = reader_advance(verify, &object, error);
	}
	if (status != FSIMG_OK)
		return (status);
	if (thread.valid)
		return (hfsplus_corrupt(error, "HFS Plus thread has no primary record"));
	if (owner.valid)
		return (hfsplus_corrupt(error,
		    "HFS Plus attribute names no primary record"));
	if ((verify->hfsplus->header.attributes & VOLUME_IDS_REUSED) == 0 &&
	    verify->hfsplus->header.next_catalog_id <= largest)
		return (hfsplus_corrupt(error,
		    "HFS Plus nextCatalogID does not exceed the largest node ID"));
	return (FSIMG_OK);
}

/* Step 5's second merge: every stub names an indirect node and every indirect
 * node's link count equals its stubs. An indirect node's name fixes its reference,
 * so no two share one.
 */
static fsimg_status_t
links_merge(verify_t *verify, fsimg_error_t *error)
{
	stream_reader_t link, inode;
	fsimg_status_t status;

	status = stream_sorted(&verify->links, reference_compare, error);
	if (status == FSIMG_OK)
		status = stream_sorted(&verify->inodes, reference_compare, error);
	if (status == FSIMG_OK)
		status = reader_open(verify, &link, verify->links, LINK_BYTES,
		    error);
	if (status == FSIMG_OK)
		status = reader_open(verify, &inode, verify->inodes,
		    LINK_BYTES, error);
	while (status == FSIMG_OK && inode.valid) {
		uint32_t reference = fsimg_load_le32(inode.bytes + LINK_REFERENCE);
		uint32_t count = fsimg_load_le32(inode.bytes + LINK_VALUE);
		uint64_t stubs = 0;

		while (status == FSIMG_OK && link.valid &&
		    fsimg_load_le32(link.bytes + LINK_REFERENCE) <= reference) {
			if (fsimg_load_le32(link.bytes + LINK_REFERENCE) != reference)
				return (hfsplus_corrupt(error,
				    "HFS Plus hard-link stub's indirect node is missing"));
			stubs++;
			status = reader_advance(verify, &link, error);
		}
		if (status != FSIMG_OK)
			return (status);
		if (stubs != count)
			return (hfsplus_corrupt(error,
			    "HFS Plus indirect node's link count disagrees with its stubs"));
		status = reader_advance(verify, &inode, error);
	}
	if (status != FSIMG_OK)
		return (status);
	if (link.valid)
		return (hfsplus_corrupt(error,
		    "HFS Plus hard-link stub's indirect node is missing"));
	return (FSIMG_OK);
}

/* Step 5: the merges, then the totals the header and the extents tree declare. */
static fsimg_status_t
merges(verify_t *verify, fsimg_error_t *error)
{
	const fsimg_hfsplus_reader_t *hfsplus = verify->hfsplus;
	fsimg_status_t status;

	status = objects_merge(verify, error);
	if (status == FSIMG_OK)
		status = links_merge(verify, error);
	if (status != FSIMG_OK)
		return (status);
	if (verify->overflow_records != hfsplus->extents.header.leaf_records)
		return (hfsplus_corrupt(error,
		    "HFS Plus extents tree holds an overflow record no fork consumes"));
	if (verify->files != hfsplus->header.file_count)
		return (hfsplus_corrupt(error,
		    "HFS Plus fileCount disagrees with the catalog"));
	if (verify->folders != hfsplus->header.folder_count)
		return (hfsplus_corrupt(error,
		    "HFS Plus folderCount disagrees with the catalog"));
	return (FSIMG_OK);
}

/* What a structural pass reads of an entry's content. A compressed file opens as a
 * read would, checking its attribute, its fork's headers and an inline payload's
 * decode, then has its whole chunk table checked; no chunk in a fork is decoded. A
 * symbolic link's target is read, so a target no extraction could create fails
 * here. An ordinary file's forks were resolved by the catalog pass.
 */
static fsimg_status_t
content_check(fsimg_reader_t *reader, fsimg_error_t *error)
{
	const hfsplus_child_t *current = &hfsplus_state(reader)->current;
	fsimg_status_t status;

	if (current->size == 0 || (current->compression == 0 &&
	    current->kind != FSIMG_ENTRY_SYMLINK))
		return (FSIMG_OK);
	status = hfsplus_content_open(reader, error);
	if (status == FSIMG_OK && current->compression != 0)
		status = hfsplus_compress_table_check(reader, error);
	return (status);
}

/* Step 6: the common walk's preparation checks each reachable folder. What it
 * reaches, the root and the records it never enters must then be all the
 * primaries, or some of them hang off a disconnected cycle or a missing parent.
 */
static fsimg_status_t
reachability(verify_t *verify, fsimg_error_t *error)
{
	fsimg_reader_t *reader = verify->reader;
	fsimg_status_t status;

	verify->report->directories = 1;
	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		verify->reached++;
		status = fsimg_verify_account(reader, verify->report, error);
		if (status != FSIMG_OK)
			return (status);
		if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		status = verify->payload != NULL ?
		    fsimg_verify_payload(reader, verify->payload,
		    PAYLOAD_BUFFER_BYTES, verify->report, error) :
		    content_check(reader, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (1 + verify->reached + verify->owned_records +
	    verify->private_records != verify->primaries)
		return (hfsplus_corrupt(error,
		    "HFS Plus catalog holds records the root does not reach"));
	return (FSIMG_OK);
}

/* One byte of the bitmap, through the window. Each window loaded polls and charges
 * through its read.
 */
static fsimg_status_t
bitmap_byte(bitmap_scan_t *scan, uint64_t byte, unsigned char *value,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (scan->loaded == 0 || byte < scan->offset ||
	    byte - scan->offset >= scan->loaded) {
		uint64_t left = scan->logical_size - byte;

		scan->offset = byte;
		scan->loaded = left < BITMAP_WINDOW_BYTES ? (size_t)left :
		    BITMAP_WINDOW_BYTES;
		status = hfsplus_fork_read(scan->reader,
		    hfsplus_state(scan->reader)->bitmap, byte, scan->window,
		    scan->loaded, error);
		if (status != FSIMG_OK)
			return (status);
	}
	*value = scan->window[byte - scan->offset];
	return (FSIMG_OK);
}

/* Require bits [first, end) of the bitmap, most significant first, all set or all
 * clear.
 */
static fsimg_status_t
bitmap_range(bitmap_scan_t *scan, uint64_t first, uint64_t end, bool set,
    const char *message, fsimg_error_t *error)
{
	unsigned offset, count;
	unsigned char byte, mask;
	fsimg_status_t status;

	while (first < end) {
		status = bitmap_byte(scan, first / 8, &byte, error);
		if (status != FSIMG_OK)
			return (status);
		offset = (unsigned)(first % 8);
		count = 8 - offset;
		if (count > end - first)
			count = (unsigned)(end - first);
		mask = (unsigned char)((0xffu >> offset) &
		    (0xffu << (8 - offset - count)));
		if ((byte & mask) != (set ? mask : 0))
			return (hfsplus_corrupt(error, message));
		first += count;
	}
	return (FSIMG_OK);
}

/* Bits between claims must be clear, except from tail_first on, where a set bit
 * is the reserved tail of a volume whose alternate lies past allocation
 * (bitmap_compare()): it adds to *tail_set instead.
 */
static fsimg_status_t
bitmap_gap(bitmap_scan_t *scan, uint64_t first, uint64_t end, uint64_t tail_first,
    uint64_t *tail_set, fsimg_error_t *error)
{
	unsigned char byte;
	fsimg_status_t status;

	status = bitmap_range(scan, first, end < tail_first ? end : tail_first,
	    false, "HFS Plus bitmap marks a block nothing owns", error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t bit = first > tail_first ? first : tail_first; bit < end;
	    bit++) {
		status = bitmap_byte(scan, bit / 8, &byte, error);
		if (status != FSIMG_OK)
			return (status);
		if ((byte & (0x80u >> (bit % 8))) != 0)
			(*tail_set)++;
	}
	return (FSIMG_OK);
}

/* Step 7: the sealed claims, in block order, against the bitmap's first N bits;
 * the gaps between them must be clear, and so must every bit past N. The utility
 * refuses overlapping claims as it returns them, whichever objects made them.
 *
 * A formatter marks the last blocks below N for the alternate header wherever it
 * lies. When it lies past allocation, in the bytes from N * B to V, no reserved
 * tail was claimed and those blocks are a content block like any other, so
 * they may be set with no owner, and then count as allocated.
 */
static fsimg_status_t
bitmap_compare(verify_t *verify, fsimg_error_t *error)
{
	const fsimg_hfsplus_reader_t *hfsplus = verify->hfsplus;
	const hfsplus_geometry_t *g = &hfsplus->geometry;
	uint64_t blocks = g->blocks;
	uint64_t logical = hfsplus->header.allocation_file.logical_size;
	uint64_t tail_blocks = (ALTERNATE_FROM_END + g->block_size - 1) /
	    g->block_size;
	uint64_t tail_first = g->end_block == g->blocks &&
	    blocks > tail_blocks ? blocks - tail_blocks : blocks;
	const char *set_message = "HFS Plus bitmap leaves an owned block clear";
	const char *padding_message = "HFS Plus bitmap's padding bits are not zero";
	bitmap_scan_t *scan;
	fsimg_owner_run_t run;
	fsimg_error_t detail;
	uint64_t position = 0, claimed = 0, tail_set = 0, bits;
	fsimg_status_t status;

	if (logical < (blocks + 7) / 8)
		return (hfsplus_corrupt(error, "HFS Plus bitmap is too short"));
	if (!fsimg_mul_u64(logical, 8, &bits))
		return (hfsplus_corrupt(error, "HFS Plus bitmap is too long"));
	status = fsimg_owner_runs_seal(verify->claims, error);
	if (status != FSIMG_OK)
		return (status);
	scan = fsimg_alloc(verify->reader->ctx, sizeof(*scan), &detail);
	if (scan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	scan->reader = verify->reader;
	scan->offset = 0;
	scan->loaded = 0;
	scan->logical_size = logical;
	for (;;) {
		status = fsimg_owner_runs_next(verify->claims, &run, error);
		if (status != FSIMG_OK)
			break;
		status = bitmap_gap(scan, position, run.first, tail_first,
		    &tail_set, error);
		if (status == FSIMG_OK)
			status = bitmap_range(scan, run.first, run.end, true,
			    set_message, error);
		if (status != FSIMG_OK)
			break;
		claimed += run.end - run.first;
		position = run.end;
	}
	if (status == FSIMG_DONE)
		status = bitmap_gap(scan, position, blocks, tail_first, &tail_set,
		    error);
	if (status == FSIMG_OK)
		status = bitmap_range(scan, blocks, bits, false, padding_message,
		    error);
	fsimg_dealloc(scan);
	if (status != FSIMG_OK)
		return (status);
	claimed += tail_set;
	if (blocks - claimed != hfsplus->header.free_blocks)
		return (hfsplus_corrupt(error,
		    "HFS Plus freeBlocks disagrees with the bitmap"));
	verify->report->free_clusters = blocks - claimed;
	verify->report->bad_clusters = verify->bad_blocks;
	verify->report->allocated_clusters = claimed - verify->bad_blocks;
	return (FSIMG_OK);
}

static bool
fork_equal(const hfsplus_fork_t *a, const hfsplus_fork_t *b)
{
	if (a->logical_size != b->logical_size ||
	    a->total_blocks != b->total_blocks)
		return (false);
	for (unsigned i = 0; i < HFSPLUS_EXTENTS_PER_RECORD; i++)
		if (a->extents[i].start != b->extents[i].start ||
		    a->extents[i].count != b->extents[i].count)
			return (false);
	return (true);
}

/* Step 8, the last: the alternate header agrees with the primary in what macOS
 * keeps in step between them (specs/design-hfsplus.md, Alternate header). A read
 * failure keeps its class. Verification never runs on a truncated volume, so these
 * bytes lie in the slice.
 */
static fsimg_status_t
alternate_compare(verify_t *verify, fsimg_error_t *error)
{
	const fsimg_hfsplus_reader_t *hfsplus = verify->hfsplus;
	const hfsplus_header_t *primary = &hfsplus->header;
	unsigned char raw[HFSPLUS_HEADER_BYTES];
	hfsplus_header_t alternate;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(verify->reader,
	    hfsplus->geometry.volume_bytes - ALTERNATE_FROM_END, raw,
	    sizeof(raw), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_header_decode(raw, sizeof(raw), &alternate))
		return (hfsplus_corrupt(error,
		    "HFS Plus alternate header is malformed"));
	if (alternate.signature != primary->signature ||
	    alternate.version != primary->version)
		return (hfsplus_corrupt(error,
		    "HFS Plus alternate header holds no matching volume header"));
	if (alternate.block_size != primary->block_size ||
	    alternate.total_blocks != primary->total_blocks ||
	    !fork_equal(&alternate.allocation_file, &primary->allocation_file) ||
	    !fork_equal(&alternate.extents_file, &primary->extents_file) ||
	    !fork_equal(&alternate.catalog_file, &primary->catalog_file) ||
	    !fork_equal(&alternate.attributes_file, &primary->attributes_file) ||
	    !fork_equal(&alternate.startup_file, &primary->startup_file))
		return (hfsplus_corrupt(error,
		    "HFS Plus alternate header disagrees with the primary"));
	return (FSIMG_OK);
}

static void
verify_release(verify_t *verify)
{
	fsimg_owner_runs_free(verify->claims);
	fsimg_stream_free(verify->objects);
	fsimg_stream_free(verify->threads);
	fsimg_stream_free(verify->owners);
	fsimg_stream_free(verify->links);
	fsimg_stream_free(verify->inodes);
	fsimg_dealloc(verify->payload);
}

fsimg_status_t
hfsplus_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	fsimg_session_t *session = &reader->session;
	verify_t verify = { .reader = reader, .hfsplus = hfsplus,
	    .report = report };
	fsimg_error_t detail;
	fsimg_status_t status;

	session->progress.phase = "HFS Plus verification";
	status = preflight(&verify, error);
	if (status != FSIMG_OK)
		return (status);
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		verify.payload = fsimg_alloc(reader->ctx, PAYLOAD_BUFFER_BYTES,
		    &detail);
		if (verify.payload == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	status = fsimg_owner_runs_new(session, "HFS Plus allocation block", 0,
	    hfsplus->geometry.blocks, &verify.claims, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, RECORD_BYTES_MAX, &verify.objects,
		    error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, RECORD_BYTES_MAX, &verify.threads,
		    error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, OWNER_BYTES, &verify.owners,
		    error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, LINK_BYTES, &verify.links, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, LINK_BYTES, &verify.inodes, error);

	if (status == FSIMG_OK)
		status = tree_structure(reader, &hfsplus->extents, error);
	if (status == FSIMG_OK)
		status = tree_structure(reader, &hfsplus->catalog, error);
	if (status == FSIMG_OK)
		status = tree_structure(reader, &hfsplus->attributes, error);
	if (status == FSIMG_OK)
		status = catalog_pass(&verify, error);
	if (status == FSIMG_OK)
		status = attributes_pass(&verify, error);
	if (status == FSIMG_OK)
		status = special_storage(&verify, error);
	if (status == FSIMG_OK)
		status = merges(&verify, error);
	if (status == FSIMG_OK)
		status = reachability(&verify, error);
	if (status == FSIMG_OK)
		status = bitmap_compare(&verify, error);
	if (status == FSIMG_OK)
		status = alternate_compare(&verify, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status == FSIMG_OK) {
		reader->info.free_bytes = report->free_clusters *
		    hfsplus->geometry.block_size;
		reader->info.valid |= FSIMG_VOLUME_FREE;
	}
	verify_release(&verify);
	return (status);
}
