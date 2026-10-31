/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "exfat.h"
#include "fsimg-endian.h"

typedef struct cluster_visit {
	fsimg_reader_t	*reader;
	exfat_run_fn_t	 visit;
} cluster_visit_t;

static fsimg_status_t record_run(fsimg_reader_t *, fsimg_extent_index_t *,
    uint32_t, uint32_t, fsimg_error_t *);
static fsimg_status_t index_chain(fsimg_reader_t *, uint32_t,
    fsimg_extent_index_t *, fsimg_error_t *);
static fsimg_status_t bitmap_range(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);
static fsimg_status_t visit_clusters(void *, const fsimg_extent_t *,
    fsimg_error_t *);
static fsimg_status_t own_run(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);

fsimg_status_t
exfat_table_get(fsimg_reader_t *reader, uint32_t cluster, uint32_t *value,
    fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	const exfat_geometry_t *g = &exfat->geometry;
	uint64_t offset, window;
	size_t transferred, length;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	offset = (uint64_t)cluster * 4;
	if (!fsimg_range_fits(offset, 4, g->fat_bytes))
		return (exfat_corrupt(error, "exFAT cluster exceeds FAT"));
	window = offset / sizeof(exfat->table_window) * sizeof(exfat->table_window);
	if (exfat->table_window_length == 0 || exfat->table_window_offset != window) {
		length = g->fat_bytes - window > sizeof(exfat->table_window) ?
		    sizeof(exfat->table_window) : (size_t)(g->fat_bytes - window);
		status = fsimg_reader_bytes(reader, g->fat_offset + window,
		    exfat->table_window, length, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		exfat->table_window_offset = window;
		exfat->table_window_length = length;
	}
	*value = fsimg_load_le32(exfat->table_window + (size_t)(offset - window));
	return (FSIMG_OK);
}

/* Record one validated cluster run as the index's next extent. Once the
 * bitmap is available every run must be marked allocated in it. During full
 * verification building an index is the allocation's ownership claim, so
 * each allocation is built once and no caller has to remember either step.
 */
static fsimg_status_t
record_run(fsimg_reader_t *reader, fsimg_extent_index_t *index, uint32_t first,
    uint32_t count, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	const exfat_geometry_t *g = &exfat->geometry;
	fsimg_extent_t extent;
	fsimg_status_t status;

	extent.logical_offset = fsimg_extent_index_length(index);
	extent.image_offset = exfat_cluster_offset(g, first);
	extent.length = (uint64_t)count * g->cluster_bytes;
	status = fsimg_extent_index_append(index, &extent, error);
	if (status != FSIMG_OK)
		return (status);
	if (exfat->bitmap_ready) {
		status = bitmap_range(reader, first, count, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (exfat->owners != NULL)
		return (own_run(reader, first, count, error));
	return (FSIMG_OK);
}

/* Coalesce adjacent physical clusters while Brent's checkpoint detects
 * cycles. The heap count independently caps all walks, even malformed ones.
 * The index owns the entire chain, including preallocation beyond payload.
 */
static fsimg_status_t
index_chain(fsimg_reader_t *reader, uint32_t start, fsimg_extent_index_t *index,
    fsimg_error_t *error)
{
	const exfat_geometry_t *g = &exfat_state(reader)->geometry;
	uint64_t steps, power, distance;
	uint32_t cluster, next, checkpoint, first, count;
	fsimg_status_t status;

	cluster = start;
	checkpoint = cluster;
	first = cluster;
	count = 0;
	steps = 0;
	distance = 0;
	power = 1;
	for (;;) {
		if (!exfat_cluster_valid(g, cluster) || ++steps > g->clusters)
			return (exfat_corrupt(error, "invalid or cyclic exFAT allocation chain"));
		count++;
		status = exfat_table_get(reader, cluster, &next, error);
		if (status != FSIMG_OK)
			return (status);
		if (next != cluster + 1 || next == UINT32_MAX) {
			status = record_run(reader, index, first, count, error);
			if (status != FSIMG_OK)
				return (status);
			first = next;
			count = 0;
		}
		if (next == UINT32_MAX)
			return (FSIMG_OK);
		distance++;
		if (next == checkpoint)
			return (exfat_corrupt(error, "cyclic exFAT allocation chain"));
		if (distance == power) {
			checkpoint = next;
			power *= 2;
			distance = 0;
		}
		cluster = next;
	}
}

/* Build the extent index of a stream's allocation. A stream without
 * allocation has the empty (NULL) index. Contiguous allocation follows the
 * rounded-up DataLength; a chain must cover DataLength, and exactly so under
 * EXFAT_LENGTH_EXACT_ALLOCATION. Only a complete index is returned.
 */
fsimg_status_t
exfat_index_build(fsimg_reader_t *reader, const exfat_stream_t *stream,
    exfat_length_policy_t policy, fsimg_extent_index_t **out,
    fsimg_error_t *error)
{
	const exfat_geometry_t *g = &exfat_state(reader)->geometry;
	fsimg_extent_index_t *index;
	uint64_t allocated;
	uint32_t count;
	fsimg_status_t status;

	*out = NULL;
	status = exfat_stream_validate(g, stream, false, error);
	if (status != FSIMG_OK)
		return (status);
	if (stream->first == 0)
		return (FSIMG_OK);
	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &index, error);
	if (status != FSIMG_OK)
		return (status);
	if (stream->contiguous) {
		count = (uint32_t)(stream->length / g->cluster_bytes +
		    (stream->length % g->cluster_bytes != 0));
		status = record_run(reader, index, stream->first, count, error);
	} else {
		status = index_chain(reader, stream->first, index, error);
		if (status == FSIMG_OK) {
			allocated = fsimg_extent_index_length(index);
			if (allocated < stream->length ||
			    (policy == EXFAT_LENGTH_EXACT_ALLOCATION &&
			    allocated != stream->length))
				status = exfat_corrupt(error,
				    "exFAT chain does not cover its declared allocation");
		}
	}
	if (status != FSIMG_OK) {
		fsimg_extent_index_free(index);
		return (status);
	}
	*out = index;
	return (FSIMG_OK);
}

/* Read complete metadata bytes through an index. Unlike content reads there
 * is no partial result: any failure fails the whole record.
 */
fsimg_status_t
exfat_index_read(fsimg_reader_t *reader, fsimg_extent_index_t *index,
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
		if (batch > EXFAT_BUFFER_BYTES)
			batch = EXFAT_BUFFER_BYTES;
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

fsimg_status_t
exfat_bitmap_bit(fsimg_reader_t *reader, uint32_t cluster, bool *allocated,
    fsimg_error_t *error)
{
	unsigned char byte;
	fsimg_status_t status;

	status = exfat_index_read(reader, exfat_state(reader)->bitmap,
	    (cluster - 2) / 8, &byte, 1, error);
	if (status == FSIMG_OK)
		*allocated = (byte & (1u << ((cluster - 2) % 8))) != 0;
	return (status);
}

static fsimg_status_t
bitmap_range(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    fsimg_error_t *error)
{
	unsigned char bytes[4096];
	uint64_t bit, end, byte_offset;
	size_t batch;
	unsigned begin_bit, end_bit, mask;
	fsimg_status_t status;

	bit = (uint64_t)first - 2;
	end = bit + count;
	while (bit < end) {
		byte_offset = bit / 8;
		batch = (size_t)((end + 7) / 8 - byte_offset);
		if (batch > sizeof(bytes))
			batch = sizeof(bytes);
		status = exfat_index_read(reader, exfat_state(reader)->bitmap,
		    byte_offset, bytes, batch, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < batch; i++) {
			begin_bit = i == 0 ? (unsigned)(bit % 8) : 0;
			end_bit = (byte_offset + i + 1) * 8 > end ? (unsigned)(end % 8) : 8;
			mask = ((1u << end_bit) - 1) & ~((1u << begin_bit) - 1);
			if ((bytes[i] & mask) != mask)
				return (exfat_corrupt(error, "owned exFAT cluster is marked free"));
		}
		status = fsimg_session_charge(&reader->session, 0, 0, batch, error);
		if (status != FSIMG_OK)
			return (status);
		bit = (byte_offset + batch) * 8;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
visit_clusters(void *arg, const fsimg_extent_t *extent, fsimg_error_t *error)
{
	cluster_visit_t *clusters = arg;
	const exfat_geometry_t *g = &exfat_state(clusters->reader)->geometry;
	uint32_t first, count;

	/* The index confines the extent to whole clusters inside the heap, whose
	 * cluster count fits uint32_t with room for the two reserved identifiers
	 * below it.
	 */
	first = (uint32_t)((extent->image_offset - g->heap_offset) /
	    g->cluster_bytes + 2);
	count = (uint32_t)(extent->length / g->cluster_bytes);
	return (clusters->visit(clusters->reader, first, count, error));
}

/* Visit an index's extents in order as cluster runs. The walk requires the
 * extents to tile the allocation, so each cluster is visited exactly once.
 */
fsimg_status_t
exfat_index_runs(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    exfat_run_fn_t visit, fsimg_error_t *error)
{
	cluster_visit_t clusters = { .reader = reader, .visit = visit };

	return (fsimg_extent_index_walk(index, visit_clusters, &clusters, error));
}

/* Require every cluster of an index built before the bitmap was available
 * to be marked allocated in it.
 */
fsimg_status_t
exfat_index_check(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    fsimg_error_t *error)
{
	return (exfat_index_runs(reader, index, bitmap_range, error));
}

static fsimg_status_t
own_run(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    fsimg_error_t *error)
{
	return (fsimg_owner_runs_append(exfat_state(reader)->owners, first, count,
	    error));
}

/* Claim an index built while opening, before full verification created the
 * owner runs; the caller must have created them. Every later index claims its
 * runs as it is built.
 */
fsimg_status_t
exfat_index_claim(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    fsimg_error_t *error)
{
	return (exfat_index_runs(reader, index, own_run, error));
}
