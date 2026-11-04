/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "hfsplus.h"

/*
 * The fork resolver. A fork's blocks are its eight inline extents followed by
 * the overflow records of its (file ID, fork type) in the extents tree, each
 * keyed by the logical block it continues from. Everything that reads a fork,
 * the catalog's and the bitmap's included, goes through here, so the domain,
 * continuity and sum checks exist once.
 *
 * The walk is bounded by construction whatever the records claim. Every
 * overflow record must add at least one block and may not pass totalBlocks,
 * and totalBlocks cannot exceed the content domain, so a walk reads at most
 * that many records. Its extents go to the common extent index, which keeps
 * them in charged scratch, so a million-extent fork costs scratch and not RAM.
 */

typedef struct fork_walk {
	fsimg_reader_t		*reader;
	fsimg_extent_index_t	*index;
	uint64_t		 appended;	/* blocks so far */
	uint32_t		 total;		/* the fork's totalBlocks */
} fork_walk_t;

static fsimg_status_t check_descriptors(const hfsplus_geometry_t *,
    const hfsplus_extent_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t append_extents(fork_walk_t *, const hfsplus_extent_t *,
    fsimg_error_t *);
static fsimg_status_t walk_overflow(fork_walk_t *, uint32_t, uint8_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t check_fork(const hfsplus_geometry_t *,
    const hfsplus_fork_t *, uint64_t *, fsimg_error_t *);

/* Check one record's eight descriptors against the extent rules and give the
 * blocks of its used ones: used precede unused, an unused one is (0, 0), and
 * each used one lies in the content domain, tested before any sum is formed.
 */
static fsimg_status_t
check_descriptors(const hfsplus_geometry_t *geometry,
    const hfsplus_extent_t *extents, uint64_t *blocks, fsimg_error_t *error)
{
	bool ended = false;

	*blocks = 0;
	for (unsigned i = 0; i < HFSPLUS_EXTENTS_PER_RECORD; i++) {
		uint32_t start = extents[i].start, count = extents[i].count;

		if (count == 0) {
			if (start != 0)
				return (hfsplus_corrupt(error,
				    "HFS Plus extent has a start and no blocks"));
			ended = true;
			continue;
		}
		if (ended)
			return (hfsplus_corrupt(error,
			    "HFS Plus extent follows an unused one"));
		if (start < geometry->first_block ||
		    start >= geometry->end_block ||
		    count > geometry->end_block - start)
			return (hfsplus_corrupt(error,
			    "HFS Plus extent lies outside the content domain"));
		*blocks += count;
	}
	return (FSIMG_OK);
}

/* Append a checked record's used extents after those before it, one work unit
 * per descriptor. The caller has checked they do not pass totalBlocks.
 */
static fsimg_status_t
append_extents(fork_walk_t *walk, const hfsplus_extent_t *extents,
    fsimg_error_t *error)
{
	uint32_t block_size = hfsplus_state(walk->reader)->geometry.block_size;
	fsimg_extent_t run;
	fsimg_status_t status;

	for (unsigned i = 0; i < HFSPLUS_EXTENTS_PER_RECORD; i++) {
		if (extents[i].count == 0)
			break;
		status = fsimg_session_charge(&walk->reader->session, 0, 0, 1,
		    error);
		if (status != FSIMG_OK)
			return (status);
		run.logical_offset = walk->appended * block_size;
		run.image_offset = (uint64_t)extents[i].start * block_size;
		run.length = (uint64_t)extents[i].count * block_size;
		status = fsimg_extent_index_append(walk->index, &run, error);
		if (status != FSIMG_OK)
			return (status);
		walk->appended += extents[i].count;
	}
	return (FSIMG_OK);
}

/* Continue a fork short of totalBlocks from the extents tree: its next record
 * must carry exactly the key (file ID, fork type, appended), and the records
 * after it must continue the count until it reaches totalBlocks. The record
 * after the last must belong to another fork, so an extra one is caught where
 * it lies.
 */
static fsimg_status_t
walk_overflow(fork_walk_t *walk, uint32_t file_id, uint8_t fork_type,
    uint64_t *records, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(walk->reader);
	hfsplus_extent_t extents[HFSPLUS_EXTENTS_PER_RECORD];
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_ekey_t stored;
	hfsplus_key_t key;
	uint64_t blocks;
	fsimg_status_t status;

	hfsplus_cursor_init(&cursor, &hfsplus->extents,
	    hfsplus->extents.search_node);
	key.extents = (hfsplus_ekey_t){
		.file_id = file_id,
		.fork_type = fork_type,
		.start_block = (uint32_t)walk->appended
	};
	status = hfsplus_btree_search(&cursor, &key, error);
	if (status != FSIMG_OK)
		return (status);

	/* The caller has a block to find, so a missing first record fails here
	 * as a later one would.
	 */
	while (walk->appended < walk->total) {
		if (!cursor.valid)
			return (hfsplus_corrupt(error,
			    "HFS Plus fork's overflow records end early"));
		status = hfsplus_cursor_record(&cursor, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_ekey_decode(record.key, record.key_bytes,
		    &stored) ||
		    !hfsplus_extents_decode(record.data, record.data_bytes,
		    extents))
			return (hfsplus_corrupt(error,
			    "HFS Plus overflow record is malformed"));
		if (stored.file_id != file_id ||
		    stored.fork_type != fork_type ||
		    stored.start_block != walk->appended)
			return (hfsplus_corrupt(error,
			    "HFS Plus fork's overflow records do not continue it"));
		status = check_descriptors(&hfsplus->geometry, extents, &blocks,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (blocks == 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus overflow record holds no blocks"));
		if (blocks > walk->total - walk->appended)
			return (hfsplus_corrupt(error,
			    "HFS Plus fork's extents exceed its totalBlocks"));
		status = append_extents(walk, extents, error);
		if (status == FSIMG_OK)
			status = hfsplus_cursor_next(&cursor, error);
		if (status != FSIMG_OK)
			return (status);
		(*records)++;
	}

	if (!cursor.valid)
		return (FSIMG_OK);
	status = hfsplus_cursor_record(&cursor, &record, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_ekey_decode(record.key, record.key_bytes, &stored))
		return (hfsplus_corrupt(error,
		    "HFS Plus overflow record is malformed"));
	if (stored.file_id == file_id && stored.fork_type == fork_type)
		return (hfsplus_corrupt(error,
		    "HFS Plus fork has an extra overflow record"));
	return (FSIMG_OK);
}

/* Check fork data and give the blocks of its inline extents: the descriptors,
 * their sum within totalBlocks, totalBlocks within the content domain, so a
 * huge claim fails before any walk, and logicalSize within its allocation.
 */
static fsimg_status_t
check_fork(const hfsplus_geometry_t *geometry, const hfsplus_fork_t *fork,
    uint64_t *inline_blocks, fsimg_error_t *error)
{
	uint64_t allocation;
	fsimg_status_t status;

	status = check_descriptors(geometry, fork->extents, inline_blocks,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (*inline_blocks > fork->total_blocks)
		return (hfsplus_corrupt(error,
		    "HFS Plus fork's extents exceed its totalBlocks"));
	if (fork->total_blocks > geometry->end_block - geometry->first_block)
		return (hfsplus_corrupt(error,
		    "HFS Plus fork claims more blocks than the volume holds"));
	if (!fsimg_mul_u64(fork->total_blocks, geometry->block_size,
	    &allocation) ||
	    fork->logical_size > allocation)
		return (hfsplus_corrupt(error,
		    "HFS Plus fork's size exceeds its allocation"));
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_fork_index(fsimg_reader_t *reader, uint32_t file_id, uint8_t fork_type,
    const hfsplus_fork_t *fork, fsimg_extent_index_t **index, uint64_t *records,
    fsimg_error_t *error)
{
	fork_walk_t walk = { .reader = reader, .total = fork->total_blocks };
	uint64_t inline_blocks, consumed;
	fsimg_status_t status;

	*index = NULL;
	if (records != NULL)
		*records = 0;
	consumed = 0;
	status = check_fork(&hfsplus_state(reader)->geometry, fork,
	    &inline_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	/* The extents tree cannot hold its own continuation: this is what keeps
	 * resolving it from reading it.
	 */
	if (file_id == HFSPLUS_ID_EXTENTS &&
	    inline_blocks != fork->total_blocks)
		return (hfsplus_corrupt(error,
		    "HFS Plus extents tree's fork needs overflow"));
	if (fork->total_blocks == 0)
		return (FSIMG_OK);

	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &walk.index, error);
	if (status != FSIMG_OK)
		return (status);
	status = append_extents(&walk, fork->extents, error);
	if (status == FSIMG_OK && walk.appended < walk.total)
		status = walk_overflow(&walk, file_id, fork_type, &consumed,
		    error);
	if (status != FSIMG_OK) {
		fsimg_extent_index_free(walk.index);
		return (status);
	}
	if (records != NULL)
		*records = consumed;
	*index = walk.index;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_fork_one_extent(fsimg_reader_t *reader, const hfsplus_fork_t *fork,
    hfsplus_extent_t *extent, fsimg_error_t *error)
{
	uint64_t inline_blocks;
	fsimg_status_t status;

	status = check_fork(&hfsplus_state(reader)->geometry, fork,
	    &inline_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	if (fork->total_blocks == 0 ||
	    fork->extents[0].count != fork->total_blocks)
		return (hfsplus_corrupt(error,
		    "HFS Plus fork is not exactly one extent"));
	*extent = fork->extents[0];
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_fork_read(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    uint64_t offset, void *buffer, size_t length, fsimg_error_t *error)
{
	fsimg_extent_t extent;
	uint64_t position, within;
	size_t batch, done, transferred;
	fsimg_status_t status;

	for (done = 0; done < length; done += batch) {
		position = offset + done;
		status = fsimg_extent_index_lookup(index, position, &extent,
		    error);
		if (status != FSIMG_OK)
			return (status);
		within = position - extent.logical_offset;
		batch = length - done;
		if (batch > extent.length - within)
			batch = (size_t)(extent.length - within);
		status = fsimg_reader_bytes(reader,
		    extent.image_offset + within,
		    (unsigned char *)buffer + done, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}
