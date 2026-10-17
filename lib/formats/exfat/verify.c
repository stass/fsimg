/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "host.h"

typedef struct exfat_owner_cursor {
	fsimg_stream_t	*stream;
	uint64_t	 cursor;
	uint64_t	 first;
	uint64_t	 end;
	bool		 exhausted;
} exfat_owner_cursor_t;

static int owner_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t owner_next(fsimg_reader_t *, exfat_owner_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t verify_allocations(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t read_payload(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);

static int
owner_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint64_t first, second;

	if (alen != 16 || blen != 16)
		return ((alen > blen) - (alen < blen));
	first = fsimg_meta_get_u64(a);
	second = fsimg_meta_get_u64(b);
	return ((first > second) - (first < second));
}

static fsimg_status_t
owner_next(fsimg_reader_t *reader, exfat_owner_cursor_t *owners,
    fsimg_error_t *error)
{
	unsigned char record[16];
	uint64_t first, count, capacity;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(owners->stream, &owners->cursor, record,
	    sizeof(record), &length, error);
	if (status == FSIMG_DONE) {
		owners->exhausted = true;
		return (FSIMG_OK);
	}
	if (status != FSIMG_OK)
		return (status);
	if (length != sizeof(record))
		return (exfat_corrupt(error, "invalid exFAT ownership record length"));
	first = fsimg_meta_get_u64(record);
	count = fsimg_meta_get_u64(record + 8);
	capacity = (uint64_t)reader->format.exfat->geometry.clusters + 2;
	if (first < 2 || first >= capacity || count == 0 ||
	    count > capacity - first)
		return (exfat_corrupt(error, "invalid exFAT ownership interval"));
	if (first < owners->end)
		return (exfat_corrupt(error, "cross-linked exFAT allocation ownership"));
	owners->first = first;
	owners->end = first + count;
	return (FSIMG_OK);
}

static fsimg_status_t
verify_allocations(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	const exfat_geometry_t *g = &reader->format.exfat->geometry;
	exfat_owner_cursor_t owners;
	unsigned char bitmap[4096];
	uint64_t bitmap_offset, window;
	uint32_t value;
	size_t batch;
	bool claimed, allocated;
	fsimg_status_t status;

	bzero(&owners, sizeof(owners));
	owners.stream = sorted;
	bitmap_offset = UINT64_MAX;
	status = owner_next(reader, &owners, error);
	if (status != FSIMG_OK)
		return (status);

	/* Every owner has already validated its allocation representation. Merge
	 * their sorted intervals with bitmap bits; FAT values are deliberately
	 * irrelevant for contiguous owners and for free clusters. Only allocated
	 * unowned clusters need a FAT lookup, to distinguish bad storage from leaks.
	 */
	for (uint32_t cluster = 2; cluster - 2 < g->clusters; cluster++) {
		window = (uint64_t)(cluster - 2) / (sizeof(bitmap) * 8) * sizeof(bitmap);
		if (window != bitmap_offset) {
			batch = (size_t)(((uint64_t)g->clusters + 7) / 8 - window);
			if (batch > sizeof(bitmap))
				batch = sizeof(bitmap);
			status = exfat_map_read(reader, &reader->format.exfat->bitmap,
			    window, bitmap, batch, error);
			if (status != FSIMG_OK)
				return (status);
			bitmap_offset = window;
		}
		while (!owners.exhausted && cluster >= owners.end) {
			status = owner_next(reader, &owners, error);
			if (status != FSIMG_OK)
				return (status);
		}
		claimed = !owners.exhausted && cluster >= owners.first;
		allocated = (bitmap[(cluster - 2) / 8 - bitmap_offset] &
		    (1u << ((cluster - 2) % 8))) != 0;
		if (!allocated) {
			if (claimed)
				return (exfat_corrupt(error, "owned exFAT cluster is marked free"));
			report->free_clusters++;
		} else if (claimed) {
			report->allocated_clusters++;
		} else {
			status = exfat_table_get(reader, cluster, &value, error);
			if (status != FSIMG_OK)
				return (status);
			if (value != UINT32_C(0xfffffff7))
				return (exfat_corrupt(error, "unowned exFAT allocation"));
			report->bad_clusters++;
		}
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (!owners.exhausted) {
		status = owner_next(reader, &owners, error);
		if (status != FSIMG_OK)
			return (status);
		if (!owners.exhausted)
			return (exfat_corrupt(error, "exFAT ownership exceeds heap"));
	}
	return (FSIMG_OK);
}

static fsimg_status_t
read_payload(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	uint64_t offset, initialized;
	size_t batch;
	fsimg_status_t status;

	/* Uninitialized tail storage is not payload. Its zero semantics are covered
	 * by the content API; read-through never reads those physical bytes.
	 */
	initialized = reader->format.exfat->current.valid_length;
	for (offset = 0; offset < initialized; offset += batch) {
		batch = initialized - offset > EXFAT_BUFFER_BYTES ?
		    EXFAT_BUFFER_BYTES : (size_t)(initialized - offset);
		status = exfat_map_read(reader, &reader->format.exfat->file,
		    offset, buffer, batch, error);
		if (status != FSIMG_OK)
			return (status);
		report->payload_bytes_read += batch;
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = reader->format.exfat;
	fsimg_stream_t *sorted;
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	sorted = NULL;
	buffer = NULL;
	reader->session.progress.phase = "exFAT verification";
	status = fsimg_stream_new(&reader->session, 16, &exfat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	status = exfat_map_own(reader, &exfat->bitmap, error);
	if (status != FSIMG_OK)
		goto done;
	status = exfat_map_own(reader, &exfat->upcase_map, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, EXFAT_BUFFER_BYTES, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}
	report->directories = 1;
	for (;;) {
		status = exfat_next(reader, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY) {
			report->directories++;
			continue;
		}
		report->files++;
		if (!fsimg_add_u64(report->logical_bytes, reader->entry.size,
		    &report->logical_bytes)) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_VERIFY, "exFAT logical-byte total overflow");
			goto done;
		}
		if (buffer != NULL) {
			status = read_payload(reader, buffer, report, error);
			if (status != FSIMG_OK)
				goto done;
		}
	}
	status = fsimg_stream_seal(exfat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_sort(exfat->owners, owner_compare, NULL, NULL, &sorted, error);
	if (status != FSIMG_OK)
		goto done;
	status = verify_allocations(reader, sorted, report, error);
	if (status != FSIMG_OK)
		goto done;
	if (!reader->info.clean || reader->info.io_error) {
		status = exfat_corrupt(error, "exFAT volume is dirty or records a media failure");
		goto done;
	}
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		goto done;
	reader->info.free_bytes = report->free_clusters * exfat->geometry.cluster_bytes;
	reader->info.valid |= FSIMG_VOLUME_FREE;
done:
	fsimg_stream_free(sorted);
	fsimg_stream_free(exfat->owners);
	exfat->owners = NULL;
	fsimg_dealloc(buffer);
	return (status);
}
