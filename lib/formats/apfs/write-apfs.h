/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_APFS_WRITE_APFS_H
#define FSIMG_APFS_WRITE_APFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apfs.h"
#include "internal.h"

/*
 * The APFS writer's private interfaces. The writer emits 4096-byte blocks,
 * and every B-tree node is one block.
 */
#define APFS_WRITE_BLOCK_BYTES		4096u

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
 * of contents holds its key count rounded up to a multiple of 8 entries,
 * and never fewer than toc_min. The footer's storage bit must match
 * storage: APFS_BTREE_PHYSICAL for a physical tree, APFS_BTREE_EPHEMERAL
 * for an ephemeral one and neither for a virtual one.
 */
typedef struct apfs_pack_profile {
	const char	*role;		/* names the tree in diagnostics */
	uint32_t	 storage;
	uint32_t	 subtype;
	uint32_t	 tree_flags;
	uint16_t	 fixed_key;
	uint16_t	 fixed_val;
	uint16_t	 toc_min;
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

#endif
