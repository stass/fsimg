/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_APFS_WRITE_APFS_H
#define FSIMG_APFS_WRITE_APFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apfs.h"
#include "emit.h"
#include "internal.h"

/*
 * The APFS writer's private interfaces. The writer emits 4096-byte blocks,
 * and every B-tree node is one block.
 */
#define APFS_WRITE_BLOCK_BYTES		APFS_WRITER_BLOCK_SIZE

/*
 * The bulk B-tree packer (write-btree.c), the only place that decides where
 * a node ends. Planning runs it to count a tree's nodes and emission runs it
 * again over the same records to write them, so node boundaries, node
 * counts and the order nodes complete in, and with them every OID and block
 * the plan assigns, cannot differ between the two.
 *
 * Every node, root or not, is packed against a root's capacity: the block
 * less the node header and the root's information footer. A level's only
 * node is then always a valid root, so the packer never holds a node back
 * to learn whether it is the root; every other node leaves the footer's
 * bytes unused.
 */
#define APFS_PACK_CAPACITY						\
	(APFS_WRITE_BLOCK_BYTES - APFS_NODE_HEADER_BYTES - APFS_BTREE_INFO_BYTES)

/*
 * A tree's shape as the writer builds it. storage is APFS_OBJ_PHYSICAL,
 * APFS_OBJ_VIRTUAL or APFS_OBJ_EPHEMERAL, and subtype the tree's object
 * subtype; tree_flags is the footer's flags word verbatim, and its
 * APFS_BTREE_KV_NONALIGNED bit stops keys and values being rounded up to 8
 * bytes. fixed_key and fixed_val are the leaf record sizes of a fixed-size
 * tree, both zero for variable-size entries; an index record of a
 * fixed-size tree is fixed_key bytes of key and a child OID. A node's table
 * of contents holds its key count rounded up to a multiple of 8 entries, at
 * least 8; a fixed-size tree's is sized for at least a full node of
 * records of the node's kind, leaf or index, as the native formatter sizes
 * it. The
 * footer's storage bit must match storage: APFS_BTREE_PHYSICAL for a
 * physical tree, APFS_BTREE_EPHEMERAL for an ephemeral one and neither for
 * a virtual one.
 */
typedef struct apfs_pack_profile {
	const char	*role;		/* names the tree in diagnostics */
	uint32_t	 storage;
	uint32_t	 subtype;
	uint32_t	 tree_flags;
	uint16_t	 fixed_key;
	uint16_t	 fixed_val;
} apfs_pack_profile_t;

/*
 * The profiles of the trees whose native shape is known: the filesystem
 * tree, the container object map and a free queue.
 */
extern const apfs_pack_profile_t apfs_pack_fs_tree;
extern const apfs_pack_profile_t apfs_pack_container_omap;
extern const apfs_pack_profile_t apfs_pack_free_queue;

/*
 * Where a tree's nodes go. Nodes are numbered by completion: the k'th node
 * closed is node k, children before their parent and the root last.
 *
 * node_oid(arg, k) returns node k's OID, which its header and its parent's
 * index record carry: a block number in a physical tree, a virtual OID in
 * the filesystem tree. It must be a pure function of k. When it is NULL,
 * node k's OID is k, which suits a planning run that only counts.
 *
 * write(arg, k, oid, node, error) receives node k, sealed, as
 * APFS_WRITE_BLOCK_BYTES bytes borrowed for the call. A failure stops the
 * packer and is returned as it is. When it is NULL the node is discarded.
 */
typedef struct apfs_pack_sink {
	void		*arg;
	uint64_t	(*node_oid)(void *, uint64_t);
	fsimg_status_t	(*write)(void *, uint64_t, uint64_t,
			    const unsigned char *, fsimg_error_t *);
} apfs_pack_sink_t;

/*
 * A packed tree: its nodes per level, leaves at level 0, and in all, the
 * height, the leaf record count and the root's OID. The root is node
 * node_count - 1.
 */
typedef struct apfs_pack_result {
	uint64_t	level_nodes[APFS_BTREE_LEVELS_MAX];
	uint64_t	node_count;
	uint64_t	key_count;
	uint64_t	root_oid;
	unsigned	levels;
} apfs_pack_result_t;

typedef struct apfs_pack apfs_pack_t;

/*
 * apfs_pack_open(ctx, profile, xid, sink, pack, error) starts a tree whose
 * nodes carry xid, charging its buffers to ctx; profile and sink are
 * borrowed until apfs_pack_free(). sink may be NULL. A profile the reader
 * would not accept is FSIMG_EINVAL. The packer holds one node's records
 * per level, never the tree.
 *
 * apfs_pack_add(pack, record, error) appends the next leaf record; records
 * arrive in the tree's key order, which the packer does not check, and are
 * borrowed for the call. A ghost record carries no value and is accepted
 * only when the profile's tree flags allow ghosts. A record with no key,
 * one whose lengths a fixed-size profile does not state, one that would not
 * fit an empty node, and one whose key with a child OID would not fit an
 * empty index node twice, are FSIMG_EINVAL. A tree that would grow taller
 * than APFS_BTREE_LEVELS_MAX levels is FSIMG_ERANGE; under the admission
 * rule that takes at least 2^15 leaves.
 *
 * apfs_pack_finish(pack, result, error) closes the open nodes from the
 * leaves up, the last being the root, and fills result. A tree with no
 * records is one root leaf with no keys.
 *
 * Once a call has failed, or apfs_pack_finish() has been called, adding or
 * finishing is FSIMG_ESTATE; only apfs_pack_free() remains, and it accepts
 * NULL.
 */
fsimg_status_t	apfs_pack_open(fsimg_ctx_t *, const apfs_pack_profile_t *,
		    uint64_t, const apfs_pack_sink_t *, apfs_pack_t **,
		    fsimg_error_t *);
fsimg_status_t	apfs_pack_add(apfs_pack_t *, const apfs_record_t *,
		    fsimg_error_t *);
fsimg_status_t	apfs_pack_finish(apfs_pack_t *, apfs_pack_result_t *,
		    fsimg_error_t *);
void		apfs_pack_free(apfs_pack_t *);

/*
 * The profiles of the volume's physical trees other than its filesystem
 * tree, as E1 and E3 show them (specs/design-apfs.md, "Writer values",
 * "Table-of-contents capacity"): the volume object map takes the container
 * map's shape, and the extent-reference and snapshot trees are
 * variable-size and unaligned physical trees.
 */
extern const apfs_pack_profile_t apfs_pack_volume_omap;
extern const apfs_pack_profile_t apfs_pack_extentref;
extern const apfs_pack_profile_t apfs_pack_snapshot;

/* The directory-record types of the kinds the writer emits. */
#define APFS_DT_FIFO			1u
#define APFS_DT_DIR			4u
#define APFS_DT_REG			8u
#define APFS_DT_LNK			10u

/*
 * Writer values (specs/design-apfs.md, "Writer values"): the checkpoint's
 * XID, the reserved inode numbers, the first inode number of a catalog
 * entry less one, the first ephemeral or virtual OID and the extent cap.
 */
#define APFS_WRITE_XID			1u
#define APFS_WRITE_INO_ROOT_PARENT	1u
#define APFS_WRITE_INO_ROOT		2u
#define APFS_WRITE_INO_PRIVATE		3u
#define APFS_WRITE_INO_BASE		15u
#define APFS_WRITE_OID_SPACEMAN		1024u
#define APFS_WRITE_OID_REAPER		1025u
#define APFS_WRITE_OID_VOLUME		1026u
#define APFS_WRITE_OID_FS_FIRST		1027u
#define APFS_WRITE_EXTENT_CAP		((UINT64_C(1) << 56) - APFS_WRITE_BLOCK_BYTES)
#define APFS_WRITE_BLOCKS_MIN		1024u

/* The inode number of catalog ID id; the root, row 0, is inode 2. */
static inline uint64_t
apfs_write_ino(uint64_t id)
{
	return (id == 0 ? APFS_WRITE_INO_ROOT : APFS_WRITE_INO_BASE + id);
}

/*
 * One plan row per catalog ID, row 0 the root, in a paged array:
 *   parent		the parent's catalog ID; zero for row 0
 *   length		a file's size or a link's target length
 *   blocks_before	the data blocks of every file before this ID
 *   first_block	the file's first data block, zero until layout and
 *			for a row without data blocks
 *   created ...	the stored times in nanoseconds, selected under the
 *			metadata policy
 *   children		the directory records a directory owns
 *   hash		the entry's 22-bit name hash
 *   uid, gid, mode	the stored owner and mode, type bits included
 *   kind		the FSIMG_ENTRY_* kind
 * A row's data blocks, ceil(length / 4096) for a file and zero otherwise,
 * are derived (apfs_row_blocks), as are its inode number and private ID.
 * blocks_before plus a row's data blocks never decreases with the ID, so
 * file runs ascend with the catalog ID.
 */
typedef struct apfs_row {
	uint64_t	 parent;
	uint64_t	 length;
	uint64_t	 blocks_before;
	uint64_t	 first_block;
	int64_t		 created;
	int64_t		 modified;
	int64_t		 changed;
	int64_t		 accessed;
	uint32_t	 children;
	uint32_t	 hash;
	uint32_t	 uid;
	uint32_t	 gid;
	uint16_t	 mode;
	uint8_t		 kind;
} apfs_row_t;

#define APFS_ROW_BYTES			88u

static inline uint64_t
apfs_row_blocks(const apfs_row_t *row)
{
	if (row->kind != FSIMG_ENTRY_FILE)
		return (0);
	return (row->length / APFS_WRITE_BLOCK_BYTES +
	    (row->length % APFS_WRITE_BLOCK_BYTES != 0));
}

/*
 * One record of the sorted filesystem stream, a borrowed view of its frame:
 * the catalog ID whose row completes it, then the key and the value,
 * encoded in full except a file extent's physical block, which emission
 * fills from the row. The frame is id (8), key_len (2), val_len (2), the
 * key and the value, little endian. The longest key is a directory record
 * of a 765-byte name and its NUL, the longest value a link's attribute
 * holding 1023 target bytes and the NUL.
 */
typedef struct apfs_write_record {
	const unsigned char	*key;
	const unsigned char	*val;
	uint64_t		 id;
	uint16_t		 key_len;
	uint16_t		 val_len;
} apfs_write_record_t;

#define APFS_WRITE_NAME_MAX_BYTES	(3u * APFS_WRITE_NAME_MAX_UNITS)
#define APFS_WRITE_FRAME_FIXED		12u
#define APFS_WRITE_KEY_MAX		(APFS_DREC_HASHED_KEY_BYTES + \
	    APFS_WRITE_NAME_MAX_BYTES + 1u)
#define APFS_WRITE_VAL_MAX		(APFS_XATTR_VAL_BYTES + \
	    FSIMG_SYMLINK_TARGET_MAX)
#define APFS_WRITE_RECORD_MAX		(APFS_WRITE_FRAME_FIXED + \
	    APFS_WRITE_KEY_MAX + APFS_WRITE_VAL_MAX)

bool	apfs_write_record_decode(const unsigned char *, size_t,
	    apfs_write_record_t *);

/*
 * A packed tree and the run index of its node 0: node k sits at
 * run_block(first_run + k).
 */
typedef struct apfs_tree_plan {
	apfs_pack_result_t	shape;
	uint64_t		first_run;
} apfs_tree_plan_t;

/* An ephemeral object of the checkpoint: the space manager or the reaper. */
typedef struct apfs_ephemeral {
	uint64_t	oid;
	uint32_t	kind;
	uint32_t	bytes;
	uint32_t	first_slot;
} apfs_ephemeral_t;

#define APFS_EPHEMERAL_COUNT		2u

/*
 * The plan. It owns rows and records, and apfs_write_free() releases them;
 * emission only borrows them.
 *   nx, sm		the native geometry for N, the fields planned so far
 *   regions		the fixed regions, ascending, one prefix from block 0
 *   run_first		the run's first block, past the fixed regions
 *   allocated_blocks	the allocation, blocks [0, allocated_blocks): the
 *			fixed regions and the run, contiguous
 *   comap_run ...	run indexes of the single-block objects; the trees
 *			follow the format design's order
 *   ephemeral		the space manager and the reaper in OID order
 *   pool_cibs ...	the internal pool's blocks: chunk-information and
 *			address blocks, and the bitmap blocks of the chunks the
 *			allocation touches
 *   files ...		the volume superblock's per-kind totals
 *   data_blocks	every file's data blocks
 *   meta_blocks	the run's metadata blocks, before the files
 *   format_time	nanoseconds, fixed before the first pass
 *   next_ino		the volume's next object identifier
 *   next_oid		the container's next OID
 *   volume_allocated	the volume superblock's allocated block count
 */
typedef struct apfs_plan {
	fsimg_plan_t		 common;
	fsimg_array_t		*rows;
	fsimg_stream_t		*records;
	apfs_nx_t		 nx;
	apfs_spaceman_t		 sm;
	apfs_region_t		 regions[APFS_FIXED_REGIONS];
	uint64_t		 run_first;
	uint64_t		 allocated_blocks;
	apfs_tree_plan_t	 comap_tree;
	apfs_tree_plan_t	 vomap_tree;
	apfs_tree_plan_t	 fs_tree;
	apfs_tree_plan_t	 extentref_tree;
	apfs_tree_plan_t	 snapshot_tree;
	apfs_ephemeral_t	 ephemeral[APFS_EPHEMERAL_COUNT];
	unsigned char		 volume_uuid[16];
	unsigned char		 container_uuid[16];
	uint64_t		 comap_run;
	uint64_t		 volume_run;
	uint64_t		 vomap_run;
	uint64_t		 block_count;
	uint64_t		 sm_blocks;
	uint64_t		 pool_cibs;
	uint64_t		 pool_cabs;
	uint64_t		 pool_bitmaps;
	uint64_t		 files;
	uint64_t		 directories;
	uint64_t		 symlinks;
	uint64_t		 others;
	uint64_t		 data_files;
	uint64_t		 data_blocks;
	uint64_t		 meta_blocks;
	int64_t			 format_time;
	uint64_t		 next_ino;
	uint64_t		 next_oid;
	uint64_t		 volume_allocated;
	bool			 case_sensitive;
} apfs_plan_t;

/* The writer operations: those of write-plan.c and write-emit.c. */
extern const fsimg_writer_ops_t fsimg_apfs_writer_ops;

fsimg_status_t	apfs_write_validate(fsimg_write_options_t *, unsigned,
		    fsimg_error_t *);
fsimg_status_t	apfs_write_plan(fsimg_writer_t *, fsimg_error_t *);
void		apfs_write_free(fsimg_plan_t *);
fsimg_status_t	apfs_write_emit(fsimg_writer_t *, fsimg_emit_t *,
		    fsimg_error_t *);
fsimg_status_t	apfs_row_get(apfs_plan_t *, uint64_t, apfs_row_t *,
		    fsimg_error_t *);
/* The block of run index i: run_first + i. */
uint64_t	apfs_run_block(const apfs_plan_t *, uint64_t);

#endif
