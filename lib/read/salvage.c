/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "reader.h"

static fsimg_status_t chain_follow(fsimg_reader_t *,
    const fsimg_salvage_chain_t *, uint32_t, uint32_t *, fsimg_error_t *);
static fsimg_status_t chain_step(fsimg_reader_t *,
    const fsimg_salvage_chain_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t chain_distinct(fsimg_reader_t *,
    const fsimg_salvage_chain_t *, uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t chain_walk(fsimg_reader_t *,
    const fsimg_salvage_chain_t *, fsimg_extent_index_t *, uint64_t, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t readable_prefix(fsimg_reader_t *, uint64_t, uint64_t *,
    fsimg_error_t *);

/* Follow one link: FSIMG_DONE when the driver's decoder rejects it, which ends
 * the chain at this cluster; any other failure is terminal and reported.
 */
static fsimg_status_t
chain_follow(fsimg_reader_t *reader, const fsimg_salvage_chain_t *chain,
    uint32_t cluster, uint32_t *next, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_status_t status;

	*next = 0;
	fsimg_error_clear(&detail);
	status = chain->link(reader, cluster, next, &detail);
	if (status == FSIMG_OK)
		return (FSIMG_OK);
	if (fsimg_reader_decode_rejected(reader, &detail, status))
		return (FSIMG_DONE);
	if (error != NULL)
		*error = detail;
	return (status);
}

/* Move a cursor along the chain; FSIMG_DONE when the chain ends there. */
static fsimg_status_t
chain_step(fsimg_reader_t *reader, const fsimg_salvage_chain_t *chain,
    uint32_t *cluster, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = chain_follow(reader, chain, *cluster, cluster, error);
	if (status == FSIMG_OK && *cluster == 0)
		return (FSIMG_DONE);
	return (status);
}

/* Given a cycle of the proved length, count the chain's clusters before its
 * first repeat. A cursor that length ahead of another meets it at the first
 * cluster on the cycle, after as many steps as there are clusters before the
 * cycle, and the cycle's length more clusters are distinct. Only a table that
 * changed under the walk can end the search early, and then the cycle's length
 * is a bounded answer, with nothing to promise about a table that changes.
 */
static fsimg_status_t
chain_distinct(fsimg_reader_t *reader, const fsimg_salvage_chain_t *chain,
    uint64_t cycle, uint64_t *distinct, fsimg_error_t *error)
{
	uint32_t lead, trail;
	fsimg_status_t status;

	*distinct = cycle;
	lead = chain->first;
	trail = chain->first;
	status = FSIMG_OK;
	for (uint64_t step = 0; status == FSIMG_OK && step < cycle; step++)
		status = chain_step(reader, chain, &lead, error);
	for (uint64_t lead_in = 0; status == FSIMG_OK && lead_in <= chain->clusters;
	    lead_in++) {
		if (trail == lead) {
			*distinct = lead_in + cycle;
			break;
		}
		status = chain_step(reader, chain, &trail, error);
		if (status == FSIMG_OK)
			status = chain_step(reader, chain, &lead, error);
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/* Index the chain's clusters, up to limit of them. A cluster counts once its
 * own link reads and validates. The walk ends at the chain's end, at the first
 * rejected link, or at the first cluster past the slice, which stops the index.
 * It follows links past limit without indexing them, since Brent's check proves
 * a cycle some steps after its first repeat: a link back to the checkpoint sets
 * *cycle to the cycle's length and ends the walk. A repeat among the first
 * limit clusters is proved by the checkpoint's first run of at least limit
 * links, so once a run passes twice that no kept cluster repeats. The walk costs
 * in proportion to the file, whatever the chain goes on to do, and it ends over
 * a table that keeps changing.
 */
static fsimg_status_t
chain_walk(fsimg_reader_t *reader, const fsimg_salvage_chain_t *chain,
    fsimg_extent_index_t *index, uint64_t limit, uint64_t *cycle,
    fsimg_error_t *error)
{
	fsimg_extent_t extent;
	uint64_t steps, power, distance;
	uint32_t cluster, checkpoint, next;
	fsimg_status_t status;

	*cycle = 0;
	cluster = chain->first;
	checkpoint = cluster;
	steps = 0;
	distance = 0;
	power = 1;
	for (;;) {
		extent.logical_offset = steps * chain->cluster_bytes;
		extent.image_offset = chain->heap_offset +
		    (uint64_t)(cluster - 2) * chain->cluster_bytes;
		extent.length = chain->cluster_bytes;
		if (extent.image_offset >= reader->slice_bytes) {
			fsimg_extent_index_stop(index);
			return (FSIMG_OK);
		}
		status = chain_follow(reader, chain, cluster, &next, error);
		if (status != FSIMG_OK)
			return (status == FSIMG_DONE ? FSIMG_OK : status);
		if (steps < limit) {
			status = fsimg_extent_index_append(index, &extent, error);
			if (status != FSIMG_OK)
				return (status);
		}
		steps++;
		if (next == 0)
			return (FSIMG_OK);
		distance++;
		if (next == checkpoint) {
			*cycle = distance;
			return (FSIMG_OK);
		}
		if (distance == power) {
			checkpoint = next;
			power *= 2;
			distance = 0;
			if (power > 2 * limit)
				return (FSIMG_OK);
		}
		cluster = next;
	}
}

fsimg_status_t
fsimg_reader_salvage_keep(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    fsimg_status_t status, const fsimg_error_t *failure)
{
	if (status == FSIMG_OK || index == NULL ||
	    reader->entry.kind != FSIMG_ENTRY_FILE ||
	    !fsimg_reader_decode_rejected(reader, failure, status))
		return (status);
	fsimg_extent_index_stop(index);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_salvage_index(fsimg_reader_t *reader,
    const fsimg_salvage_chain_t *chain, fsimg_extent_index_t **out,
    fsimg_error_t *error)
{
	fsimg_extent_index_t *index;
	uint64_t cycle, distinct, length;
	fsimg_status_t status;

	*out = NULL;
	if (chain->wanted == 0)
		return (FSIMG_OK);
	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &index, error);
	if (status != FSIMG_OK)
		return (status);
	status = chain_walk(reader, chain, index, chain->wanted, &cycle, error);
	if (status != FSIMG_OK)
		goto fail;
	if (cycle != 0) {
		status = chain_distinct(reader, chain, cycle, &distinct, error);
		if (status != FSIMG_OK)
			goto fail;
		/* The index is append-only and holds the repeats: build it again,
		 * up to the first one.
		 */
		if (distinct < chain->wanted) {
			fsimg_extent_index_free(index);
			index = NULL;
			status = fsimg_extent_index_new(&reader->session,
			    &reader->content_domain, &index, error);
			if (status == FSIMG_OK)
				status = chain_walk(reader, chain, index, distinct,
				    &cycle, error);
			if (status != FSIMG_OK)
				goto fail;
		}
	}
	length = fsimg_extent_index_length(index);
	if (length / chain->cluster_bytes < chain->wanted &&
	    !fsimg_extent_index_stopped(index))
		reader->salvage.bound = length;
	*out = index;
	return (FSIMG_OK);
fail:
	fsimg_extent_index_free(index);
	return (status);
}

/* Walk the current file's extents from its first byte up to limit. *out is the
 * first logical byte that is not available: the rejection of its lookup, or the
 * end of the content domain or the slice, which can fall inside an extent. A
 * held or zero extent reads no image, so neither end applies to it. *out is
 * limit when every byte below limit is available. Each extent is charged, like
 * a read of it, so the walk is bounded and polled.
 *
 * A prefix past the output budget fails the file's charge whatever the rest of
 * the file holds, so the walk ends there with that charge's error and decodes
 * nothing further. The prefix never exceeds the entry's size, which is what
 * makes the charge below fail.
 */
static fsimg_status_t
readable_prefix(fsimg_reader_t *reader, uint64_t limit, uint64_t *out,
    fsimg_error_t *error)
{
	const fsimg_extent_domain_t *domain = &reader->content_domain;
	const unsigned char *held;
	fsimg_extent_t extent;
	fsimg_error_t detail;
	uint64_t offset, end, within, image, available, budget;
	fsimg_status_t status;
	bool clipped;

	budget = reader->session.output_limit > reader->session.output_bytes ?
	    reader->session.output_limit - reader->session.output_bytes : 0;
	end = reader->slice_bytes;
	if (domain->offset < end && domain->length < end - domain->offset)
		end = domain->offset + domain->length;
	offset = 0;
	while (offset < limit) {
		if (offset > budget)
			return (fsimg_session_charge(&reader->session, 0,
			    reader->entry.size, 0, error));
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		fsimg_error_clear(&detail);
		status = fsimg_reader_resolve(reader, offset, &extent, &held, &detail);
		if (status != FSIMG_OK) {
			if (fsimg_reader_decode_rejected(reader, &detail, status))
				break;
			if (error != NULL)
				*error = detail;
			return (status);
		}
		within = offset - extent.logical_offset;
		available = extent.length - within;
		clipped = false;
		if (held == NULL && extent.image_offset != FSIMG_EXTENT_ZERO) {
			if (extent.image_offset > UINT64_MAX - within)
				break;
			image = extent.image_offset + within;
			if (image < domain->offset || image >= end)
				break;
			if (available > end - image) {
				available = end - image;
				clipped = true;
			}
		}
		if (available >= limit - offset) {
			offset = limit;
			break;
		}
		offset += available;
		if (clipped)
			break;
	}
	*out = offset;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_salvage_plan(fsimg_reader_t *reader, bool *damaged,
    fsimg_error_t *error)
{
	uint64_t limit, available, prefix;
	fsimg_status_t status;

	*damaged = false;
	limit = reader->initialized;
	if (limit > reader->salvage.bound)
		limit = reader->salvage.bound;
	status = readable_prefix(reader, limit, &available, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	/* Past the initialized length the file reads as defined zeros, up to the
	 * end of its size or of an allocation that broke.
	 */
	prefix = reader->entry.size;
	if (available < limit)
		prefix = available;
	else if (prefix > reader->salvage.bound)
		prefix = reader->salvage.bound;
	if (prefix == reader->entry.size)
		return (FSIMG_OK);
	*damaged = true;
	reader->entry.size = prefix;
	if (reader->initialized > prefix)
		reader->initialized = prefix;
	return (FSIMG_OK);
}
