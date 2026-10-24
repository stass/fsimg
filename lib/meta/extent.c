/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "fsimg-endian.h"
#include "meta.h"

#define EXTENT_RECORD_BYTES 24u

/* Records live in scratch; everything else is trusted private memory. The
 * covered length is the sum of all appended extents, so every valid record
 * lies inside [0, length). The cache is the last extent a lookup returned,
 * already decoded and checked; walks never read or change it.
 */
struct fsimg_extent_index {
	fsimg_store_t	*records;
	fsimg_extent_domain_t domain;
	uint64_t	 count;
	uint64_t	 length;
	fsimg_extent_t	 last;
	fsimg_extent_t	 cached;
	uint64_t	 cached_record;
	bool		 cache_valid;
};

static fsimg_status_t extent_corrupt(fsimg_error_t *, const char *);
static bool extent_valid(const fsimg_extent_domain_t *, uint64_t,
    const fsimg_extent_t *);
static fsimg_status_t record_write(fsimg_extent_index_t *, uint64_t,
    const fsimg_extent_t *, fsimg_error_t *);
static fsimg_status_t record_read(fsimg_extent_index_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);
static bool extent_contains(const fsimg_extent_t *, uint64_t);
static fsimg_status_t record_search(fsimg_extent_index_t *, uint64_t,
    uint64_t *, fsimg_error_t *);

static fsimg_status_t
extent_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_METADATA,
	    message));
}

/* An extent is valid when it names whole units of the logical range
 * [0, covered) stored as whole units inside the physical domain. This is the
 * only check between a scratch record and an image read, so a damaged record
 * cannot redirect content outside the domain. It can still name another
 * object's units inside it; only ownership verification detects that
 * cross-link.
 */
static bool
extent_valid(const fsimg_extent_domain_t *domain, uint64_t covered,
    const fsimg_extent_t *extent)
{
	if (extent->length == 0 || extent->logical_offset >= covered ||
	    extent->length > covered - extent->logical_offset)
		return (false);
	if (extent->logical_offset % domain->unit_bytes != 0 ||
	    extent->length % domain->unit_bytes != 0)
		return (false);
	return (extent->image_offset >= domain->offset &&
	    (extent->image_offset - domain->offset) % domain->unit_bytes == 0 &&
	    fsimg_range_fits(extent->image_offset - domain->offset,
	    extent->length, domain->length));
}

static bool
extent_contains(const fsimg_extent_t *extent, uint64_t offset)
{
	return (offset >= extent->logical_offset &&
	    offset - extent->logical_offset < extent->length);
}

static fsimg_status_t
record_write(fsimg_extent_index_t *index, uint64_t record,
    const fsimg_extent_t *extent, fsimg_error_t *error)
{
	unsigned char bytes[EXTENT_RECORD_BYTES];

	fsimg_store_le64(bytes, extent->logical_offset);
	fsimg_store_le64(bytes + 8, extent->image_offset);
	fsimg_store_le64(bytes + 16, extent->length);
	return (fsimg_store_write(index->records, record * EXTENT_RECORD_BYTES,
	    bytes, sizeof(bytes), error));
}

/* Every record read back from scratch is decoded and validated here. The
 * output is defined, as an empty extent, even when the read fails.
 */
static fsimg_status_t
record_read(fsimg_extent_index_t *index, uint64_t record, fsimg_extent_t *out,
    fsimg_error_t *error)
{
	unsigned char bytes[EXTENT_RECORD_BYTES];
	fsimg_status_t status;

	*out = (fsimg_extent_t){ 0 };
	if (record >= index->count)
		return (extent_corrupt(error, "extent record is outside its index"));
	status = fsimg_store_read(index->records, record * EXTENT_RECORD_BYTES,
	    bytes, sizeof(bytes), error);
	if (status != FSIMG_OK)
		return (status);
	out->logical_offset = fsimg_load_le64(bytes);
	out->image_offset = fsimg_load_le64(bytes + 8);
	out->length = fsimg_load_le64(bytes + 16);
	if (!extent_valid(&index->domain, index->length, out))
		return (extent_corrupt(error,
		    "extent index record is outside its allocation or domain"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_extent_index_new(fsimg_session_t *session,
    const fsimg_extent_domain_t *domain, fsimg_extent_index_t **out,
    fsimg_error_t *error)
{
	fsimg_extent_index_t *index;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	if (domain->unit_bytes == 0 || domain->length % domain->unit_bytes != 0 ||
	    !fsimg_range_fits(domain->offset, domain->length, UINT64_MAX))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_METADATA,
		    "invalid extent domain"));
	index = fsimg_alloc(session->ctx, sizeof(*index), &detail);
	if (index == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	index->domain = *domain;
	status = fsimg_store_new(session, &index->records, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(index);
		return (status);
	}
	*out = index;
	return (FSIMG_OK);
}

void
fsimg_extent_index_free(fsimg_extent_index_t *index)
{
	if (index == NULL)
		return;
	fsimg_store_free(index->records);
	fsimg_dealloc(index);
}

uint64_t
fsimg_extent_index_length(const fsimg_extent_index_t *index)
{
	return (index == NULL ? 0 : index->length);
}

/* Extend the index by the extent starting at its covered length. An extent
 * physically adjacent to the last one extends that record instead of adding
 * one, so records stay proportional to actual fragments.
 */
fsimg_status_t
fsimg_extent_index_append(fsimg_extent_index_t *index,
    const fsimg_extent_t *extent, fsimg_error_t *error)
{
	fsimg_extent_t merged;
	uint64_t end, capacity;
	fsimg_status_t status;

	if (extent->logical_offset != index->length ||
	    !fsimg_add_u64(index->length, extent->length, &end))
		return (extent_corrupt(error, "extent is not contiguous with its index"));
	if (!extent_valid(&index->domain, end, extent))
		return (extent_corrupt(error,
		    "extent is empty, misaligned or outside its allocation domain"));
	index->cache_valid = false;

	if (index->count != 0 &&
	    index->last.image_offset + index->last.length == extent->image_offset) {
		merged = index->last;
		merged.length += extent->length;
		status = record_write(index, index->count - 1, &merged, error);
		if (status != FSIMG_OK)
			return (status);
		index->last = merged;
		index->length = end;
		return (FSIMG_OK);
	}
	capacity = fsimg_store_size(index->records);
	if (capacity < (index->count + 1) * EXTENT_RECORD_BYTES) {
		/* Grow in pages so a fragmented allocation does not resize scratch
		 * once per record.
		 */
		capacity += FSIMG_META_PAGE_SIZE;
		status = fsimg_store_grow(index->records, capacity, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = record_write(index, index->count, extent, error);
	if (status != FSIMG_OK)
		return (status);
	index->count++;
	index->last = *extent;
	index->length = end;
	return (FSIMG_OK);
}

/* Find the last record that starts at or before offset by binary search.
 * Every probe is decoded and validated. The output is defined, as record
 * zero, even when the search fails.
 */
static fsimg_status_t
record_search(fsimg_extent_index_t *index, uint64_t offset, uint64_t *record,
    fsimg_error_t *error)
{
	fsimg_extent_t probe;
	uint64_t low, high, middle;
	fsimg_status_t status;

	*record = 0;
	low = 0;
	high = index->count;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = record_read(index, middle, &probe, error);
		if (status != FSIMG_OK)
			return (status);
		if (probe.logical_offset <= offset)
			low = middle + 1;
		else
			high = middle;
	}
	if (low == 0)
		return (extent_corrupt(error, "extent index has no covering record"));
	*record = low - 1;
	return (FSIMG_OK);
}

/* Return the extent containing offset. Sequential access crosses each record
 * once: the cached extent answers repeated requests and its successor, which
 * must start where the cached extent ends, answers the next one. Any other
 * offset uses binary search. A lookup does not prove that the records tile
 * the allocation; only a walk does.
 */
fsimg_status_t
fsimg_extent_index_lookup(fsimg_extent_index_t *index, uint64_t offset,
    fsimg_extent_t *out, fsimg_error_t *error)
{
	fsimg_extent_t found;
	uint64_t record;
	bool successor;
	fsimg_status_t status;

	if (offset >= fsimg_extent_index_length(index))
		return (extent_corrupt(error, "offset exceeds its indexed allocation"));
	if (index->cache_valid && extent_contains(&index->cached, offset)) {
		*out = index->cached;
		return (FSIMG_OK);
	}
	successor = index->cache_valid &&
	    offset >= index->cached.logical_offset &&
	    offset - index->cached.logical_offset == index->cached.length &&
	    index->cached_record + 1 < index->count;
	if (successor)
		record = index->cached_record + 1;
	else {
		status = record_search(index, offset, &record, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = record_read(index, record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (successor && found.logical_offset != offset)
		return (extent_corrupt(error, "discontinuous extent index"));
	if (!extent_contains(&found, offset))
		return (extent_corrupt(error, "extent index has no covering record"));
	index->cached = found;
	index->cached_record = record;
	index->cache_valid = true;
	*out = found;
	return (FSIMG_OK);
}

/* Visit every record in logical order. Together the records must tile the
 * covered length exactly, without gaps, overlap or reordering, so each
 * logical unit is visited once. The walk leaves the lookup cache alone, so a
 * visitor may look up through the same index.
 */
fsimg_status_t
fsimg_extent_index_walk(fsimg_extent_index_t *index,
    fsimg_extent_visit_fn_t visit, void *arg, fsimg_error_t *error)
{
	fsimg_extent_t extent;
	uint64_t expected;
	fsimg_status_t status;

	if (index == NULL)
		return (FSIMG_OK);
	expected = 0;
	for (uint64_t record = 0; record < index->count; record++) {
		status = record_read(index, record, &extent, error);
		if (status != FSIMG_OK)
			return (status);
		if (extent.logical_offset != expected)
			return (extent_corrupt(error, "discontinuous extent index"));
		/* record_read bounds the extent's end by the covered length. */
		expected = extent.logical_offset + extent.length;
		status = visit(arg, &extent, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (expected != index->length)
		return (extent_corrupt(error, "extent index does not cover its allocation"));
	return (FSIMG_OK);
}

uint64_t
fsimg_test_extent_count(const fsimg_extent_index_t *index)
{
	return (index == NULL ? 0 : index->count);
}

fsimg_store_t *
fsimg_test_extent_records(const fsimg_extent_index_t *index)
{
	return (index == NULL ? NULL : index->records);
}
