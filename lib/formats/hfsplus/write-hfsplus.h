/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_HFSPLUS_H
#define FSIMG_WRITE_HFSPLUS_H

#include "emit.h"
#include "hfsplus.h"

/* The writer's fixed node sizes and block size range. */
#define HFSPLUS_WRITE_NODE_BYTES	4096u	/* extents and catalog trees */
#define HFSPLUS_WRITE_ATTR_NODE_BYTES	8192u	/* attributes tree */
#define HFSPLUS_WRITE_BLOCK_DEFAULT	4096u
#define HFSPLUS_WRITE_BLOCK_MIN		512u
#define HFSPLUS_WRITE_BLOCK_MAX		65536u

/* Stored dates count from 1904. */
#define HFSPLUS_TIME_ORIGIN		INT64_C(2082844800)

/* Fixed parts of an on-disk node: the header node's three records end at
 * M - 8 with its map, and a map node's one record is M - 20 bytes.
 */
#define HFSPLUS_HEADER_MAP_OFFSET	248u
#define HFSPLUS_HEADER_MAP_BITS(M)	(8u * ((M) - 256u))
#define HFSPLUS_MAP_RECORD_BYTES(M)	((M) - 20u)
#define HFSPLUS_MAP_NODE_BITS(M)	(8u * HFSPLUS_MAP_RECORD_BYTES(M))

/* One plan row per catalog ID; row 0 is the root. A row is everything a leaf
 * record needs beyond its stream record, already selected under the metadata
 * policy, so emission never re-reads metadata:
 *   length		data fork bytes: a file's size, a link's target length
 *   first_block	first block of the data fork's one extent, 0 when empty
 *   blocks		its blocks, ceil(length / block_bytes)
 *   parent		the parent's node ID; a thread's parentID
 *   valence, folders	a folder's primary children and child folders
 *   mode		the stored BSD mode, type bits included
 *   kind		the FSIMG_ENTRY_* kind
 * The node ID is derived (hfsplus_node_id) and not stored. The wire layout
 * follows the field order below, little endian.
 */
typedef struct hfsplus_row {
	uint64_t	 length;
	uint32_t	 first_block;
	uint32_t	 blocks;
	uint32_t	 parent;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 created;
	uint32_t	 modified;
	uint32_t	 accessed;
	uint32_t	 changed;
	uint32_t	 valence;
	uint32_t	 folders;
	uint16_t	 mode;
	uint8_t		 kind;
} hfsplus_row_t;

#define HFSPLUS_ROW_BYTES		56u

/* The node ID of catalog ID id: the root is 2, the rest follow 16. */
static inline uint32_t
hfsplus_node_id(uint64_t id)
{
	return (id == 0 ? HFSPLUS_ID_ROOT :
	    (uint32_t)(HFSPLUS_ID_FIRST_USER - 1 + id));
}

/* One leaf record of the catalog stream, a borrowed view of its encoded bytes:
 *   parent	the key's parent node ID
 *   id		the catalog ID, whose row completes the record
 *   type	HFSPLUS_RECORD_*
 *   key	the namespace key, key_bytes long, empty on a thread
 *   name	the stored name, big-endian, units long. It is the key's name
 *		on a primary record and the data's name on a thread, whose
 *		on-disk key has none.
 * The sort order is (parent, key, id): the first two are the catalog's order,
 * and a namespace key is what makes two siblings the same name to macOS.
 */
typedef struct hfsplus_write_record {
	const unsigned char	*key;
	const unsigned char	*name;
	uint32_t		 id;
	uint32_t		 parent;
	uint16_t		 key_bytes;
	uint16_t		 type;
	uint16_t		 units;
} hfsplus_write_record_t;

/* Encoded: parent (4), id (4), type (2), key_bytes (2), units (2), the key and
 * the name, big endian.
 */
#define HFSPLUS_WRITE_RECORD_FIXED	14u
#define HFSPLUS_WRITE_RECORD_MAX	(HFSPLUS_WRITE_RECORD_FIXED + \
	    HFSPLUS_KEY_BYTES_MAX + 2u * HFSPLUS_NAME_UNITS_MAX)
/* A level stream record: the first key of one node of the level below:
 * parent (4), units (2), the name, big endian.
 */
#define HFSPLUS_WRITE_LEVEL_FIXED	6u
#define HFSPLUS_WRITE_LEVEL_MAX		(HFSPLUS_WRITE_LEVEL_FIXED + \
	    2u * HFSPLUS_NAME_UNITS_MAX)

/* One tree's shape. Packing fixes the counts, from the records alone; placing
 * the tree at a block size fixes the fork:
 *   level_nodes[i]	nodes at height i + 1, level_nodes[0] the leaves
 *   depth		the root's height, 0 for a tree of only its header
 *   used_nodes		the header, every level and the map nodes
 *   total_nodes	used_nodes rounded up to whole allocation blocks
 *   first_block, blocks	the fork's one extent
 * Nodes are numbered: 0 the header, then each level from the leaves up, then
 * the map nodes. Every node at or past used_nodes is unused and zero.
 */
typedef struct hfsplus_tree_shape {
	uint32_t	 level_nodes[HFSPLUS_BTREE_DEPTH_MAX];
	uint32_t	 node_bytes;
	uint32_t	 depth;
	uint32_t	 leaf_records;
	uint32_t	 map_nodes;
	uint32_t	 used_nodes;
	uint32_t	 total_nodes;
	uint32_t	 first_block;
	uint32_t	 blocks;
	uint32_t	 attributes;
	uint16_t	 max_key_length;
	uint8_t		 compare_type;
} hfsplus_tree_shape_t;

/* Writes one finished node. node holds node_bytes bytes, valid for the call. */
typedef fsimg_status_t (*hfsplus_node_write_fn_t)(void *, uint32_t,
    const unsigned char *, fsimg_error_t *);
/* Encodes a catalog record's data into zeroed bytes of exactly the record's
 * data length; the packer has already written the key.
 */
typedef fsimg_status_t (*hfsplus_leaf_data_fn_t)(void *,
    const hfsplus_write_record_t *, unsigned char *, fsimg_error_t *);

/* What hfsplus_tree_emit() borrows for the call:
 *   shape	a packed and placed shape
 *   records	the sorted records it was packed from, NULL for an empty tree
 *   levels	the level streams packing filled, NULL for an empty tree
 *   node	node_bytes bytes of the caller's storage, overwritten
 * The caller's leaf_data and write see argument unchanged.
 */
typedef struct hfsplus_tree_emit {
	fsimg_session_t			*session;
	const hfsplus_tree_shape_t	*shape;
	fsimg_stream_t			*records;
	fsimg_stream_t			*const *levels;
	unsigned char			*node;
	hfsplus_leaf_data_fn_t		 leaf_data;
	hfsplus_node_write_fn_t		 write;
	void				*argument;
} hfsplus_tree_emit_t;

/* The plan. It owns rows, records and every non-NULL level stream, and
 * hfsplus_write_free() releases them; emission only borrows them, and reads
 * them in any order, any number of times, because a stream read takes the
 * caller's cursor.
 *   records		the sorted catalog leaf records
 *   levels		levels[i] holds the first key of each node at height
 *			i + 1, in order; the index nodes at height i + 2 hold
 *			one record per entry. The root's level is present too,
 *			with its one key, which no node holds.
 *   allocated		A: blocks [0, A) are in use, the reserved head included
 *   tail_blocks	t: the reserved blocks at the end of the N
 *   bitmap_blocks	the bitmap's blocks, from block head_blocks
 *   format_time	the format time as a stored date
 *   next_catalog_id	nextCatalogID
 *   file_count		catalog file records
 *   folder_count	catalog folder records except the root
 * Block counts come from the placed geometry and never change after planning.
 */
typedef struct hfsplus_plan {
	fsimg_plan_t		 common;
	fsimg_array_t		*rows;
	fsimg_stream_t		*records;
	fsimg_stream_t		*levels[HFSPLUS_BTREE_DEPTH_MAX];
	hfsplus_tree_shape_t	 extents;
	hfsplus_tree_shape_t	 catalog;
	hfsplus_tree_shape_t	 attributes;
	uint64_t		 volume_bytes;
	uint32_t		 block_bytes;
	uint32_t		 total_blocks;
	uint32_t		 head_blocks;
	uint32_t		 tail_blocks;
	uint32_t		 bitmap_blocks;
	uint32_t		 allocated;
	uint32_t		 format_time;
	uint32_t		 next_catalog_id;
	uint32_t		 file_count;
	uint32_t		 folder_count;
	unsigned		 variant;
	hfsplus_compare_t	 compare;
} hfsplus_plan_t;

/* The writer operations of write-plan.c. */
fsimg_status_t hfsplus_write_validate(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
fsimg_status_t hfsplus_write_plan(fsimg_writer_t *, fsimg_error_t *);
void hfsplus_write_free(fsimg_plan_t *);

fsimg_status_t hfsplus_row_get(hfsplus_plan_t *, uint64_t, hfsplus_row_t *,
    fsimg_error_t *);
fsimg_status_t hfsplus_row_set(hfsplus_plan_t *, uint64_t,
    const hfsplus_row_t *, fsimg_error_t *);

/* The catalog stream's record codec (write-btree.c). Encoding returns the
 * bytes written, at most HFSPLUS_WRITE_RECORD_MAX. Decoding borrows raw and
 * fails on a frame whose lengths disagree. The comparator is the sort order
 * above, as an fsimg_compare_fn_t.
 */
size_t hfsplus_write_record_encode(const hfsplus_write_record_t *,
    unsigned char *);
bool hfsplus_write_record_decode(const unsigned char *, size_t,
    hfsplus_write_record_t *);
int hfsplus_write_record_compare(const void *, size_t, const void *, size_t,
    void *);

/* The packer (write-btree.c).
 *
 * hfsplus_tree_shape_init(shape, kind, compare) gives kind's parameters and a
 * shape of the header node alone: what the extents and attributes trees are.
 *
 * hfsplus_tree_pack(session, records, shape, levels, error) runs the packer
 * over at least one sorted record, filling the shape's counts and creating
 * levels[0] up to the root's level. levels has HFSPLUS_BTREE_DEPTH_MAX NULL
 * pointers on entry; the caller owns what it holds afterwards, on failure too.
 *
 * hfsplus_tree_blocks(shape, block_bytes) is the fork's blocks at a block
 * size, the constant-time step of the sizing search: packed counts do not
 * depend on the block size. hfsplus_tree_place(shape, first_block,
 * block_bytes) fixes the fork there.
 *
 * hfsplus_tree_emit() runs the same packer again, writing each node through
 * the caller: the leaves in order, each index level, the header and the map
 * nodes. It fails with FSIMG_ESTATE when the streams no longer produce
 * the packed counts.
 */
void hfsplus_tree_shape_init(hfsplus_tree_shape_t *, hfsplus_tree_kind_t,
    hfsplus_compare_t);
fsimg_status_t hfsplus_tree_pack(fsimg_session_t *, fsimg_stream_t *,
    hfsplus_tree_shape_t *, fsimg_stream_t **, fsimg_error_t *);
uint64_t hfsplus_tree_blocks(const hfsplus_tree_shape_t *, uint32_t);
void hfsplus_tree_place(hfsplus_tree_shape_t *, uint32_t, uint32_t);
fsimg_status_t hfsplus_tree_emit(const hfsplus_tree_emit_t *, fsimg_error_t *);

/* The fork's logical size in bytes. A tree fork's clump size is this value,
 * or the largest a 32-bit clump can state.
 */
static inline uint64_t
hfsplus_tree_logical_bytes(const hfsplus_tree_shape_t *shape)
{
	return ((uint64_t)shape->total_nodes * shape->node_bytes);
}

static inline uint32_t
hfsplus_tree_clump(const hfsplus_tree_shape_t *shape)
{
	uint64_t bytes = hfsplus_tree_logical_bytes(shape);

	return (bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes);
}

#endif
