/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "qnx4.h"

/*
 * The extent walker. An object's storage is an ordered list of extents:
 * the first lives in its record, the rest in a chain of 512-byte extent
 * blocks that the record's chain pointer starts. The record's extent count
 * counts them all, so the chain holds exactly count - 1 descriptors, every
 * block of it but the last holding 60. A walk therefore reads at most
 * ceil((count - 1) / 60) extent blocks and appends at most count extents,
 * whatever the chain's pointers say: a loop or a repeated block either
 * fails the claim on its second visit or leaves the count unmet when the
 * chain ends, and both are corruption.
 *
 * Each extent and extent block is range-checked against [1, N], then put
 * through the walk policy (allocation, and for an exclusive walk a claim)
 * before the walk reads or appends it. The extents become one extent index
 * over the content domain; physically adjacent extents coalesce there.
 */

#define LOCATOR_RECORD_BYTES 12u

typedef struct qnx4_walk {
	fsimg_reader_t	*reader;
	fsimg_extent_index_t *index;
	qnx4_walk_policy_t policy;
	uint64_t	 allocated;	/* blocks appended so far */
	uint64_t	 limit;		/* the most blocks the object may own */
	unsigned char	 block[QNX4_BLOCK_BYTES];
} qnx4_walk_t;

typedef struct locator_fill {
	fsimg_stream_t	*stream;
	uint64_t	 count;
} locator_fill_t;

static fsimg_status_t check_run(qnx4_walk_t *, uint32_t, uint32_t,
    fsimg_error_t *);
static fsimg_status_t append_extent(qnx4_walk_t *, const qnx4_extent_t *,
    fsimg_error_t *);
static fsimg_status_t walk_chain(qnx4_walk_t *, const qnx4_inode_t *,
    fsimg_error_t *);
static uint64_t allocation_limit(const qnx4_inode_t *, uint32_t);
static fsimg_status_t locator_visit(void *, const fsimg_extent_t *,
    fsimg_error_t *);
static int locator_compare(const void *, size_t, const void *, size_t,
    void *);

/* Range-check a run of blocks, then apply the walk policy to it. A run can
 * span almost the volume (a FIFO's allocation is unbounded), but its cost
 * stays charged: each 4 KiB bitmap window is one bounded read and each
 * claimed-bitset page one store access, and both charge a work unit and
 * observe cancellation.
 */
static fsimg_status_t
check_run(qnx4_walk_t *walk, uint32_t first, uint32_t blocks,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(walk->reader);
	bool claimed;
	fsimg_status_t status;

	if (first == 0 || blocks == 0 ||
	    (uint64_t)first + blocks - 1 > qnx4->geometry.blocks)
		return (qnx4_corrupt(error, "QNX4 extent lies outside the volume"));
	status = qnx4_blocks_allocated(walk->reader, first, blocks, error);
	if (status != FSIMG_OK || walk->policy != QNX4_WALK_EXCLUSIVE)
		return (status);
	status = fsimg_bitset_test_and_set_range(qnx4->claimed, first, blocks,
	    &claimed, error);
	if (status != FSIMG_OK)
		return (status);
	if (claimed)
		return (qnx4_corrupt(error, "QNX4 block is already in use"));
	return (FSIMG_OK);
}

/* Check one extent and append it to the index after those before it. An
 * extent that takes the allocation past the object's limit fails before
 * the policy scans its blocks, so no record can make a walk visit more
 * blocks than its size needs.
 */
static fsimg_status_t
append_extent(qnx4_walk_t *walk, const qnx4_extent_t *extent,
    fsimg_error_t *error)
{
	fsimg_extent_t run;
	fsimg_status_t status;

	status = fsimg_session_charge(&walk->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (extent->blocks > walk->limit - walk->allocated)
		return (qnx4_corrupt(error,
		    "QNX4 allocation exceeds the object's size"));
	status = check_run(walk, extent->first, extent->blocks, error);
	if (status != FSIMG_OK)
		return (status);
	run.logical_offset = walk->allocated * QNX4_BLOCK_BYTES;
	run.image_offset = qnx4_block_offset(extent->first);
	run.length = (uint64_t)extent->blocks * QNX4_BLOCK_BYTES;
	status = fsimg_extent_index_append(walk->index, &run, error);
	if (status != FSIMG_OK)
		return (status);
	walk->allocated += extent->blocks;
	return (FSIMG_OK);
}

/* Follow the chain of extent blocks for the record's count - 1 further
 * descriptors. Each block must point back at the block before it (0 for
 * the first), hold 1 to 60 descriptors, 60 when another block follows,
 * zero spare bytes and unused descriptors, a covered sum equal to its
 * descriptors' lengths and the signature; the first block also carries a
 * copy of the record's first extent, later blocks zero there.
 */
static fsimg_status_t
walk_chain(qnx4_walk_t *walk, const qnx4_inode_t *record, fsimg_error_t *error)
{
	qnx4_xblock_t xblock;
	qnx4_extent_t zero = { 0, 0 };
	const qnx4_extent_t *copy;
	uint64_t covered;
	size_t transferred;
	uint32_t block, previous, remaining;
	fsimg_status_t status;

	remaining = (uint32_t)record->extents - 1;
	previous = 0;
	block = record->chain;
	while (remaining != 0) {
		if (block == 0)
			return (qnx4_corrupt(error,
			    "QNX4 extent chain ends before its extent count"));
		status = check_run(walk, block, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_reader_bytes(walk->reader, qnx4_block_offset(block),
		    walk->block, sizeof(walk->block), &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		qnx4_xblock_decode(walk->block, &xblock);

		copy = previous == 0 ? &record->first_extent : &zero;
		if (xblock.previous != previous || !xblock.spare_zero ||
		    !xblock.signature_ok || xblock.first_copy.first != copy->first ||
		    xblock.first_copy.blocks != copy->blocks)
			return (qnx4_corrupt(error, "QNX4 extent block is malformed"));
		if (xblock.used == 0 || xblock.used > QNX4_XBLOCK_EXTENTS ||
		    xblock.used > remaining)
			return (qnx4_corrupt(error,
			    "QNX4 extent block disagrees with its extent count"));
		covered = 0;
		for (unsigned i = 0; i < QNX4_XBLOCK_EXTENTS; i++) {
			if (i < xblock.used) {
				covered += xblock.extent[i].blocks;
				continue;
			}
			if (xblock.extent[i].first != 0 ||
			    xblock.extent[i].blocks != 0)
				return (qnx4_corrupt(error,
				    "QNX4 extent block has a stray descriptor"));
		}
		if (covered != xblock.covered)
			return (qnx4_corrupt(error,
			    "QNX4 extent block's covered count disagrees"));

		for (unsigned i = 0; i < xblock.used; i++) {
			status = append_extent(walk, &xblock.extent[i], error);
			if (status != FSIMG_OK)
				return (status);
		}
		remaining -= xblock.used;
		if (remaining == 0 && xblock.next != 0)
			return (qnx4_corrupt(error,
			    "QNX4 extent chain continues past its extent count"));
		if (remaining != 0 && xblock.next != 0 &&
		    xblock.used != QNX4_XBLOCK_EXTENTS)
			return (qnx4_unsupported(error,
			    "QNX4 extent block is partly used before another"));
		previous = block;
		block = xblock.next;
	}
	return (FSIMG_OK);
}

/* The allocation the kind binds to the size: exactly the blocks a regular
 * file, symbolic link or directory needs. A FIFO may own any blocks, never
 * read; claims bound those by the volume.
 */
static uint64_t
allocation_limit(const qnx4_inode_t *record, uint32_t volume_blocks)
{
	if ((record->mode & QNX4_MODE_TYPE) == QNX4_MODE_FIFO)
		return (volume_blocks);
	return ((uint64_t)record->size / QNX4_BLOCK_BYTES +
	    (record->size % QNX4_BLOCK_BYTES != 0));
}

/* A record's own extent fields agree with its count: the count is 0
 * exactly when the first extent is empty, an extent's block and length are
 * zero together, and a chain exists exactly when the count exceeds 1.
 */
bool
qnx4_extents_consistent(const qnx4_inode_t *record)
{
	const qnx4_extent_t *first = &record->first_extent;

	return ((record->extents == 0) == (first->first == 0) &&
	    (first->first == 0) == (first->blocks == 0) &&
	    (record->chain != 0) == (record->extents > 1));
}

/* Build the extent index of a validated record's storage under policy. An
 * object without storage has the empty (NULL) index.
 */
fsimg_status_t
qnx4_index_build(fsimg_reader_t *reader, const qnx4_inode_t *record,
    qnx4_walk_policy_t policy, fsimg_extent_index_t **out,
    fsimg_error_t *error)
{
	const qnx4_extent_t *first = &record->first_extent;
	qnx4_walk_t *walk;
	uint64_t limit;
	bool fifo;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	if (!qnx4_extents_consistent(record))
		return (qnx4_corrupt(error,
		    "QNX4 extent count disagrees with the record's extents"));
	fifo = (record->mode & QNX4_MODE_TYPE) == QNX4_MODE_FIFO;
	limit = allocation_limit(record, qnx4_state(reader)->geometry.blocks);
	if (record->extents == 0) {
		if (!fifo && limit != 0)
			return (qnx4_corrupt(error,
			    "QNX4 object has a size but no storage"));
		return (FSIMG_OK);
	}

	walk = fsimg_alloc(reader->ctx, sizeof(*walk), &detail);
	if (walk == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	walk->reader = reader;
	walk->policy = policy;
	walk->allocated = 0;
	walk->limit = limit;
	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &walk->index, error);
	if (status == FSIMG_OK)
		status = append_extent(walk, first, error);
	if (status == FSIMG_OK)
		status = walk_chain(walk, record, error);
	if (status == FSIMG_OK && !fifo && walk->allocated != limit)
		status = qnx4_corrupt(error,
		    "QNX4 allocation is short of the object's size");
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
qnx4_index_read(fsimg_reader_t *reader, fsimg_extent_index_t *index,
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

/*
 * Locators. A link names its target by physical position, so finding the
 * target in .inodes (or a long-name record in .longfilenames) needs the
 * inverse of the object's index: physical block to logical block. A
 * locator holds one 12-byte record per index extent -- first block, block
 * count, logical first block -- sorted by first block in a paged array and
 * searched by bisection. It is built once at open from an index of at most
 * 65535 extents.
 */

static fsimg_status_t
locator_visit(void *argument, const fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	locator_fill_t *fill = argument;
	unsigned char record[LOCATOR_RECORD_BYTES];

	fsimg_meta_put_u32(record,
	    (uint32_t)(extent->image_offset / QNX4_BLOCK_BYTES + 1));
	fsimg_meta_put_u32(record + 4,
	    (uint32_t)(extent->length / QNX4_BLOCK_BYTES));
	fsimg_meta_put_u32(record + 8,
	    (uint32_t)(extent->logical_offset / QNX4_BLOCK_BYTES));
	fill->count++;
	return (fsimg_stream_append(fill->stream, record, sizeof(record), NULL,
	    error));
}

static int
locator_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint32_t left, right;

	if (alen != LOCATOR_RECORD_BYTES || blen != LOCATOR_RECORD_BYTES)
		return ((alen > blen) - (alen < blen));
	left = fsimg_meta_get_u32(a);
	right = fsimg_meta_get_u32(b);
	return ((left > right) - (left < right));
}

fsimg_status_t
qnx4_locator_build(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    qnx4_locator_t *locator, fsimg_error_t *error)
{
	locator_fill_t fill = { NULL, 0 };
	fsimg_stream_t *sorted;
	unsigned char record[LOCATOR_RECORD_BYTES];
	uint64_t cursor;
	size_t length;
	fsimg_status_t status;

	locator->runs = NULL;
	locator->count = 0;
	sorted = NULL;
	status = fsimg_stream_new(&reader->session, LOCATOR_RECORD_BYTES,
	    &fill.stream, error);
	if (status == FSIMG_OK)
		status = fsimg_extent_index_walk(index, locator_visit, &fill,
		    error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(fill.stream, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(fill.stream, locator_compare, NULL,
		    NULL, &sorted, error);
	if (status == FSIMG_OK)
		status = fsimg_array_new(&reader->session, LOCATOR_RECORD_BYTES,
		    fill.count, &locator->runs, error);
	cursor = 0;
	while (status == FSIMG_OK && locator->count < fill.count) {
		status = fsimg_stream_next(sorted, &cursor, record,
		    sizeof(record), &length, error);
		if (status == FSIMG_OK && length != sizeof(record))
			status = qnx4_corrupt(error, "invalid QNX4 locator record");
		if (status == FSIMG_OK)
			status = fsimg_array_set(locator->runs, locator->count,
			    record, error);
		if (status == FSIMG_OK)
			locator->count++;
	}
	if (status == FSIMG_DONE)
		status = qnx4_corrupt(error, "QNX4 locator lost a record");
	fsimg_stream_free(sorted);
	fsimg_stream_free(fill.stream);
	if (status != FSIMG_OK)
		qnx4_locator_free(locator);
	return (status);
}

void
qnx4_locator_free(qnx4_locator_t *locator)
{
	fsimg_array_free(locator->runs);
	locator->runs = NULL;
	locator->count = 0;
}

/* Find the logical block of physical block `block`: *found is false when no
 * extent of the object holds it.
 */
fsimg_status_t
qnx4_locator_find(const qnx4_locator_t *locator, uint32_t block, bool *found,
    uint64_t *logical, fsimg_error_t *error)
{
	unsigned char record[LOCATOR_RECORD_BYTES];
	uint64_t low, high, middle;
	uint32_t first, blocks;
	fsimg_status_t status;

	*found = false;
	*logical = 0;
	/* Find the last run starting at or below block. */
	low = 0;
	high = locator->count;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = fsimg_array_get(locator->runs, middle, record, error);
		if (status != FSIMG_OK)
			return (status);
		if (fsimg_meta_get_u32(record) <= block)
			low = middle + 1;
		else
			high = middle;
	}
	if (low == 0)
		return (FSIMG_OK);
	status = fsimg_array_get(locator->runs, low - 1, record, error);
	if (status != FSIMG_OK)
		return (status);
	first = fsimg_meta_get_u32(record);
	blocks = fsimg_meta_get_u32(record + 4);
	if (block - first >= blocks)
		return (FSIMG_OK);
	*found = true;
	*logical = fsimg_meta_get_u32(record + 8) + (uint64_t)(block - first);
	return (FSIMG_OK);
}
