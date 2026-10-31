/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "qnx6.h"

/* A tree has sixteen root slots and depth L. At depth zero they reference
 * content blocks ("leaves"); otherwise each references an indirect block of
 * P = B / 4 references, and every branch descends exactly L levels. Leaf k
 * lies below root slot floor(k / P^L), then the base-P digits of k mod P^L
 * select one slot per level.
 *
 * The walker visits only the slots that cover leaves below the object's
 * leaf count n, so its work is bounded by n plus the indirect blocks above
 * them; every other slot must hold the unused sentinel.
 */
typedef struct qnx6_walk {
	fsimg_reader_t	*reader;
	const qnx6_object_t *object;
	fsimg_extent_index_t *index;
	uint64_t	 leaves;
	unsigned	 depth;
	/* The run of blocks waiting to be recorded as one ownership run. */
	uint32_t	 run_first;
	uint32_t	 run_count;
	/* The indirect blocks on the path from the root to the current node,
	 * for cycle detection, and each one's decoded references: level l
	 * holds the indirect block at height depth - l.
	 */
	uint32_t	 path[QNX6_DEPTH_MAX];
	uint32_t	 refs[QNX6_DEPTH_MAX][QNX6_WINDOW_BYTES / 4];
	unsigned char	 block[QNX6_WINDOW_BYTES];
} qnx6_walk_t;

static uint64_t span_of(uint32_t, unsigned);
static fsimg_status_t flush_run(qnx6_walk_t *, fsimg_error_t *);
static fsimg_status_t own_block(qnx6_walk_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t claim_block(fsimg_qnx6_reader_t *, uint32_t,
    qnx6_role_t, fsimg_error_t *);
static fsimg_status_t check_allocated(fsimg_reader_t *, uint32_t,
    fsimg_error_t *);
static fsimg_status_t visit_block(qnx6_walk_t *, uint32_t, qnx6_role_t,
    fsimg_error_t *);
static fsimg_status_t visit_node(qnx6_walk_t *, uint32_t, unsigned, uint64_t,
    fsimg_error_t *);
static fsimg_status_t check_slots(qnx6_walk_t *, const uint32_t *, unsigned,
    unsigned, uint64_t, fsimg_error_t *);

/* n = ceil(length / B), without overflowing length + B. */
uint64_t
qnx6_tree_leaves(uint64_t length, uint32_t block_size)
{
	return (length / block_size + (length % block_size != 0));
}

/* The smallest depth L with 16 * P^L >= n, or false when even depth 5 is
 * too shallow. Capacities beyond 64 bits exceed every n.
 */
bool
qnx6_tree_min_depth(uint64_t leaves, uint32_t pointers, unsigned *depth)
{
	uint64_t capacity;

	capacity = QNX6_ROOT_SLOTS;
	for (unsigned l = 0; l <= QNX6_DEPTH_MAX; l++) {
		if (capacity >= leaves) {
			*depth = l;
			return (true);
		}
		if (!fsimg_mul_u64(capacity, pointers, &capacity)) {
			*depth = l + 1;
			return (l + 1 <= QNX6_DEPTH_MAX);
		}
	}
	return (false);
}

/* The total block count of a tree with n leaves at depth L: the leaves plus
 * ceil(n / P^m) indirect blocks at each of the L levels above them, where m
 * counts levels up from the leaves.
 */
bool
qnx6_tree_blocks(uint64_t leaves, uint32_t pointers, unsigned depth,
    uint64_t *blocks)
{
	uint64_t total, span;

	total = leaves;
	span = 1;
	for (unsigned m = 1; m <= depth; m++) {
		if (!fsimg_mul_u64(span, pointers, &span))
			span = UINT64_MAX;
		if (!fsimg_add_u64(total, leaves / span + (leaves % span != 0),
		    &total))
			return (false);
	}
	*blocks = total;
	return (true);
}

/* P^power, saturating: any span beyond 64 bits exceeds every leaf count. */
static uint64_t
span_of(uint32_t pointers, unsigned power)
{
	uint64_t span;

	span = 1;
	for (unsigned i = 0; i < power; i++)
		if (!fsimg_mul_u64(span, pointers, &span))
			return (UINT64_MAX);
	return (span);
}

static fsimg_status_t
flush_run(qnx6_walk_t *walk, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (walk->run_count == 0)
		return (FSIMG_OK);
	status = qnx6_own(walk->reader, walk->run_first, walk->run_count,
	    walk->object->owner, error);
	walk->run_count = 0;
	return (status);
}

/* During verification, record that the walked object owns block. Blocks
 * visited consecutively extend the pending run.
 */
static fsimg_status_t
own_block(qnx6_walk_t *walk, uint32_t block, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (!walk->object->records ||
	    qnx6_state(walk->reader)->owners == NULL)
		return (FSIMG_OK);
	if (walk->run_count != 0 &&
	    walk->run_first + walk->run_count == block) {
		walk->run_count++;
		return (FSIMG_OK);
	}
	status = flush_run(walk, error);
	walk->run_first = block;
	walk->run_count = 1;
	return (status);
}

/* Claim a block for the walked object; a block claimed twice is shared by
 * two objects, or used twice by one. The diagnostic names the role of the
 * second claim.
 */
static fsimg_status_t
claim_block(fsimg_qnx6_reader_t *qnx6, uint32_t block, qnx6_role_t role,
    fsimg_error_t *error)
{
	bool claimed;
	fsimg_status_t status;

	status = fsimg_bitset_test_and_set(qnx6->claimed, block, &claimed,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (!claimed)
		return (FSIMG_OK);
	switch (role) {
	case QNX6_ROLE_INDIRECT:
		return (qnx6_corrupt(error, "QNX6 indirect block is already in use"));
	case QNX6_ROLE_NAMES:
		return (qnx6_corrupt(error, "QNX6 long-name block is already in use"));
	case QNX6_ROLE_DIRECTORY:
		return (qnx6_corrupt(error, "QNX6 directory block is already in use"));
	case QNX6_ROLE_SYMLINK:
		return (qnx6_corrupt(error, "QNX6 symbolic link block is already in use"));
	case QNX6_ROLE_PAYLOAD:
		break;
	case QNX6_ROLE_BITMAP:
	case QNX6_ROLE_INODES:
		/* The system trees are never claimed. */
		return (qnx6_corrupt(error, "QNX6 system block is already in use"));
	}
	return (qnx6_corrupt(error, "QNX6 file data block is already in use"));
}

/* Check that a block is allocated; every policy but the bootstrap does. */
static fsimg_status_t
check_allocated(fsimg_reader_t *reader, uint32_t block, fsimg_error_t *error)
{
	bool allocated;
	fsimg_status_t status;

	status = qnx6_block_allocated(reader, block, &allocated, error);
	if (status != FSIMG_OK)
		return (status);
	if (!allocated)
		return (qnx6_corrupt(error, "QNX6 referenced block is free"));
	return (FSIMG_OK);
}

/* Apply the object's policy to one referenced block, which the caller has
 * checked is a valid reference below N, then record its ownership. role is
 * the block's, for diagnostics.
 */
static fsimg_status_t
visit_block(qnx6_walk_t *walk, uint32_t block, qnx6_role_t role,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = walk->reader;
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	unsigned char record[4];
	bool system;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	system = walk->object->domain == QNX6_DOMAIN_SYSTEM;
	if (system != (block < (uint64_t)qnx6->geometry.reservation * 2))
		return (qnx6_corrupt(error, system ?
		    "QNX6 system tree references a block outside the reservation" :
		    "QNX6 object references a system block"));

	switch (walk->object->policy) {
	case QNX6_WALK_BOOTSTRAP:
		/* The bitmap does not exist yet: record the block for the
		 * allocation check that follows the walk.
		 */
		fsimg_store_le32(record, block);
		status = fsimg_stream_append(qnx6->record, record,
		    sizeof(record), NULL, error);
		break;
	case QNX6_WALK_ALLOCATED:
		status = check_allocated(reader, block, error);
		break;
	case QNX6_WALK_EXCLUSIVE:
		status = check_allocated(reader, block, error);
		if (status == FSIMG_OK)
			status = claim_block(qnx6, block, role, error);
		break;
	}
	if (status != FSIMG_OK)
		return (status);
	return (own_block(walk, block, error));
}

/* Check an array of count references, each the root of a subtree of the
 * given height that covers P^height leaves from first on. The slots that
 * cover leaves below n are followed; the rest must be unused.
 */
static fsimg_status_t
check_slots(qnx6_walk_t *walk, const uint32_t *refs, unsigned count,
    unsigned height, uint64_t first, fsimg_error_t *error)
{
	const qnx6_geometry_t *g = &qnx6_state(walk->reader)->geometry;
	uint64_t span, remaining, needed;
	fsimg_status_t status;

	span = span_of(g->pointers, height);
	remaining = walk->leaves - first;
	needed = remaining / span + (remaining % span != 0);
	for (unsigned k = 0; k < count; k++) {
		if (k >= needed) {
			if (refs[k] == QNX6_SENTINEL)
				continue;
			if (refs[k] >= g->blocks)
				return (qnx6_corrupt(error,
				    "QNX6 unused block reference is out of range"));
			return (qnx6_unsupported(error,
			    "QNX6 block reference beyond the logical end"));
		}
		if (refs[k] == QNX6_SENTINEL || refs[k] >= g->blocks)
			return (qnx6_corrupt(error,
			    "QNX6 required block reference is unused or out of range"));
		/* k < needed keeps first + k * span below n. */
		status = visit_node(walk, refs[k], height, first + k * span,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Visit the subtree rooted at block: a leaf at height 0, otherwise an
 * indirect block of P references to subtrees of height - 1, covering
 * leaves from first on.
 */
static fsimg_status_t
visit_node(qnx6_walk_t *walk, uint32_t block, unsigned height, uint64_t first,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = walk->reader;
	const qnx6_geometry_t *g = &qnx6_state(reader)->geometry;
	fsimg_extent_t extent;
	size_t transferred;
	unsigned level;
	fsimg_status_t status;

	if (height == 0) {
		status = visit_block(walk, block, walk->object->role, error);
		if (status != FSIMG_OK)
			return (status);
		extent.logical_offset = first * g->block_size;
		extent.image_offset = QNX6_DATA_ORIGIN +
		    (uint64_t)block * g->block_size;
		extent.length = g->block_size;
		return (fsimg_extent_index_append(walk->index, &extent, error));
	}

	/* An indirect block at this height has depth - height ancestors. */
	level = walk->depth - height;
	for (unsigned a = 0; a < level; a++)
		if (walk->path[a] == block)
			return (qnx6_corrupt(error, "QNX6 block tree has a cycle"));
	status = visit_block(walk, block, QNX6_ROLE_INDIRECT, error);
	if (status != FSIMG_OK)
		return (status);
	walk->path[level] = block;
	status = fsimg_reader_bytes(reader, QNX6_DATA_ORIGIN +
	    (uint64_t)block * g->block_size, walk->block, g->block_size,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint32_t k = 0; k < g->pointers; k++)
		walk->refs[level][k] = qnx6_u32(walk->block + (size_t)k * 4,
		    g->big_endian);
	return (check_slots(walk, walk->refs[level], g->pointers, height - 1,
	    first, error));
}

/* Build the extent index of an object's leaves in logical order, applying
 * its policy to every block the walk visits, indirect blocks included. An
 * object without leaves has the empty (NULL) index. The descriptor's depth
 * must reach n; leaves beyond the declared length are refused, since the
 * classic profile has no qualified preallocation.
 */
fsimg_status_t
qnx6_index_build(fsimg_reader_t *reader, const qnx6_object_t *object,
    fsimg_extent_index_t **out, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	const qnx6_tree_t *tree = object->tree;
	const fsimg_extent_domain_t *domain;
	qnx6_walk_t *walk;
	uint64_t capacity;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	if (tree->depth > QNX6_DEPTH_MAX)
		return (qnx6_corrupt(error, "QNX6 block tree is too deep"));
	walk = fsimg_alloc(reader->ctx, sizeof(*walk), &detail);
	if (walk == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	walk->reader = reader;
	walk->object = object;
	walk->index = NULL;
	walk->depth = tree->depth;
	walk->run_count = 0;
	walk->leaves = qnx6_tree_leaves(tree->length, g->block_size);
	if (!fsimg_mul_u64(span_of(g->pointers, tree->depth), QNX6_ROOT_SLOTS,
	    &capacity))
		capacity = UINT64_MAX;
	if (walk->leaves > capacity || walk->leaves > g->blocks) {
		status = qnx6_corrupt(error,
		    "QNX6 object length exceeds its tree or the volume");
		goto done;
	}
	domain = object->domain == QNX6_DOMAIN_SYSTEM ? &qnx6->system_domain :
	    &reader->content_domain;
	if (walk->leaves != 0) {
		status = fsimg_extent_index_new(&reader->session, domain,
		    &walk->index, error);
		if (status != FSIMG_OK)
			goto done;
	}
	status = check_slots(walk, tree->ref, QNX6_ROOT_SLOTS, tree->depth, 0,
	    error);
	if (status == FSIMG_OK)
		status = flush_run(walk, error);
done:
	if (status == FSIMG_OK)
		*out = walk->index;
	else
		fsimg_extent_index_free(walk->index);
	fsimg_dealloc(walk);
	return (status);
}

/* Read complete metadata bytes through an index. Any failure fails the
 * whole read; there is no partial metadata record.
 */
fsimg_status_t
qnx6_index_read(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    uint64_t offset, void *buffer, size_t length, fsimg_error_t *error)
{
	fsimg_extent_t extent;
	uint64_t within;
	size_t batch, transferred, done;
	fsimg_status_t status;

	done = 0;
	while (done < length) {
		status = fsimg_extent_index_lookup(index, offset + done, &extent,
		    error);
		if (status != FSIMG_OK)
			return (status);
		within = offset + done - extent.logical_offset;
		batch = length - done;
		if (batch > QNX6_BUFFER_BYTES)
			batch = QNX6_BUFFER_BYTES;
		if (batch > extent.length - within)
			batch = (size_t)(extent.length - within);
		status = fsimg_reader_bytes(reader, extent.image_offset + within,
		    (unsigned char *)buffer + done, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		done += batch;
	}
	return (FSIMG_OK);
}

/* The shape of a tree the writer emits: n leaves at the minimal depth L,
 * with count[j] = ceil(n / P^(L + 1 - j)) indirect blocks at level j, from
 * 1 at the root's children to L at the leaves' parents. count[0] is the
 * root descriptor and count[L + 1] the leaves, so node (j, i) has children
 * at level j + 1 for every j. False when no depth up to 5 holds n.
 */
bool
qnx6_shape(uint64_t leaves, uint32_t pointers, qnx6_shape_t *shape)
{
	uint64_t span;

	bzero(shape, sizeof(*shape));
	if (!qnx6_tree_min_depth(leaves, pointers, &shape->depth))
		return (false);
	shape->leaves = leaves;
	shape->pointers = pointers;
	shape->count[0] = 1;
	for (unsigned j = 1; j <= shape->depth; j++) {
		span = span_of(pointers, shape->depth + 1 - j);
		shape->count[j] = leaves / span + (leaves % span != 0);
		shape->indirect += shape->count[j];
	}
	shape->count[shape->depth + 1] = leaves;
	return (true);
}

/* The post-order position of indirect node (level, index) within the
 * tree's run of indirect blocks: every node of its own and earlier subtrees
 * at its level and below, plus the completed nodes above it.
 *
 *   pos(j, i) = sum(m = j..L: min((i + 1) * P^(m - j), count_m)) - 1
 *             + sum(m = 1..j-1: floor(i / P^(j - m)))
 *
 * With index < count_j = ceil(n / P^(L + 1 - j)) and m <= L, each product
 * (i + 1) * P^(m - j) is at most n / P + P^5, far below 2^64 for any n the
 * writer lays out.
 */
uint64_t
qnx6_shape_position(const qnx6_shape_t *shape, unsigned level, uint64_t index)
{
	uint64_t position, covered;

	position = 0;
	for (unsigned m = level; m <= shape->depth; m++) {
		covered = (index + 1) * span_of(shape->pointers, m - level);
		position += covered < shape->count[m] ? covered : shape->count[m];
	}
	position--;
	for (unsigned m = 1; m < level; m++)
		position += index / span_of(shape->pointers, level - m);
	return (position);
}

/* Fill the references of node (level, index) of a tree laid out from block
 * first: its indirect blocks in post-order, then its leaves in logical order.
 * Level 0 is the root descriptor's sixteen slots, any other level an
 * indirect block's P slots. Slots past the last child hold the sentinel.
 * The caller has checked that first plus the tree's blocks fits the volume.
 */
void
qnx6_shape_refs(const qnx6_shape_t *shape, uint32_t first, unsigned level,
    uint64_t index, uint32_t *refs)
{
	uint64_t child, slots, position;
	bool leaf;

	slots = level == 0 ? QNX6_ROOT_SLOTS : shape->pointers;
	leaf = level == shape->depth;
	for (uint64_t c = 0; c < slots; c++) {
		child = level == 0 ? c : index * shape->pointers + c;
		if (child >= shape->count[level + 1]) {
			refs[c] = QNX6_SENTINEL;
			continue;
		}
		position = leaf ? shape->indirect + child :
		    qnx6_shape_position(shape, level + 1, child);
		refs[c] = (uint32_t)(first + position);
	}
}
