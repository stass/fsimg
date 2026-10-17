/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"

typedef struct owner_cursor {
	fsimg_stream_t	*stream;
	uint64_t	 cursor;
	uint64_t	 first;
	uint64_t	 end;
	bool		 exhausted;
} owner_cursor_t;

static int owner_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t owner_next(fsimg_reader_t *, owner_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t verify_file(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t verify_allocations(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t verify_reserved(fsimg_reader_t *,
    const fsimg_verify_report_t *, fsimg_error_t *);

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
owner_next(fsimg_reader_t *reader, owner_cursor_t *owners, fsimg_error_t *error)
{
	unsigned char record[16];
	uint64_t first, count;
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
		return (fsimg_fat_corrupt(error, "invalid FAT ownership record length"));
	first = fsimg_meta_get_u64(record);
	count = fsimg_meta_get_u64(record + 8);
	if (first < 2 || count == 0 ||
	    first >= (uint64_t)reader->format.fat->geometry.clusters + 2 ||
	    count > (uint64_t)reader->format.fat->geometry.clusters + 2 - first)
		return (fsimg_fat_corrupt(error, "invalid FAT ownership run"));
	if (first < owners->end)
		return (fsimg_fat_corrupt(error, "cross-linked FAT allocation ownership"));
	owners->first = first;
	owners->end = first + count;
	return (FSIMG_OK);
}

static fsimg_status_t
verify_file(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	const fat_geometry_t *g = &reader->format.fat->geometry;
	fat_chain_t chain;
	fat_extent_t run;
	uint64_t position, length;
	size_t batch, transferred;
	fsimg_status_t status;

	fsimg_fat_chain_init(&chain, reader->format.fat->current_cluster,
	    (reader->entry.size + g->cluster_bytes - 1) / g->cluster_bytes);
	for (;;) {
		status = fsimg_fat_chain_run(reader, &chain, &run, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_owner_append(reader, &run, error);
		if (status != FSIMG_OK)
			return (status);

		/* Structural verification needs allocation metadata only. Read-through
		 * visits logical file bytes, excluding the last cluster's slack.
		 */
		if (buffer == NULL)
			continue;
		length = run.length;
		if (length > reader->entry.size - run.logical_offset)
			length = reader->entry.size - run.logical_offset;
		position = 0;
		while (position < length) {
			batch = length - position > 65536 ? 65536 :
			    (size_t)(length - position);
			status = fsimg_reader_bytes(reader, run.image_offset + position,
			    buffer, batch, &transferred, error);
			report->payload_bytes_read += transferred;
			if (status != FSIMG_OK)
				return (status);
			position += batch;
		}
	}
}

static fsimg_status_t
verify_allocations(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	const fat_geometry_t *g = &reader->format.fat->geometry;
	owner_cursor_t owners;
	uint32_t value;
	fsimg_status_t status;
	bool claimed;

	bzero(&owners, sizeof(owners));
	owners.stream = sorted;
	status = owner_next(reader, &owners, error);
	if (status != FSIMG_OK)
		return (status);

	/* Merge the sorted ownership intervals with a single sequential FAT scan.
	 * Every allocated cluster has exactly one owner. Gaps must be free or bad;
	 * overlaps fail as soon as the next interval is admitted by owner_next.
	 */
	for (uint32_t cluster = 2; cluster - 2 < g->clusters; cluster++) {
		while (!owners.exhausted && cluster >= owners.end) {
			status = owner_next(reader, &owners, error);
			if (status != FSIMG_OK)
				return (status);
		}
		claimed = !owners.exhausted && cluster >= owners.first;
		status = fsimg_fat_table_get(reader, cluster, &value, error);
		if (status != FSIMG_OK)
			return (status);
		if (value == 0) {
			report->free_clusters++;
			if (claimed)
				return (fsimg_fat_corrupt(error, "owned FAT cluster is marked free"));
		} else if (value == g->bad_value) {
			report->bad_clusters++;
			if (claimed)
				return (fsimg_fat_corrupt(error, "owned FAT cluster is marked bad"));
		} else {
			if (value < g->eoc_value && !fsimg_fat_cluster_valid(g, value))
				return (fsimg_fat_corrupt(error, "invalid FAT allocation-table value"));
			if (!claimed)
				return (fsimg_fat_corrupt(error, "unowned FAT allocation"));
			report->allocated_clusters++;
		}
	}
	if (!owners.exhausted) {
		status = owner_next(reader, &owners, error);
		if (status != FSIMG_OK)
			return (status);
		if (!owners.exhausted)
			return (fsimg_fat_corrupt(error, "FAT ownership exceeds allocation table"));
	}
	return (FSIMG_OK);
}

static fsimg_status_t
verify_reserved(fsimg_reader_t *reader, const fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &reader->format.fat->geometry;
	unsigned char primary[512], backup[512], info[512];
	uint32_t free_count, next_free;
	size_t transferred;
	fsimg_status_t status;

	if (!reader->info.clean || reader->info.io_error)
		return (fsimg_fat_corrupt(error, "FAT volume is dirty or records an I/O error"));
	if (reader->format.fat->root_label_seen &&
	    (reader->info.valid & FSIMG_VOLUME_LABEL) != 0 &&
	    strcmp(reader->format.fat->root_label, reader->info.label) != 0)
		return (fsimg_fat_corrupt(error, "FAT boot and root volume labels disagree"));
	if (g->variant != 32)
		return (FSIMG_OK);
	if (g->fsinfo_sector != 0xffff) {
		status = fsimg_reader_bytes(reader,
		    (uint64_t)g->fsinfo_sector * g->sector_bytes, info, sizeof(info),
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (fsimg_fat_u32(info) != 0x41615252 || fsimg_fat_u32(info + 484) != 0x61417272 ||
		    fsimg_fat_u32(info + 508) != 0xaa550000)
			return (fsimg_fat_corrupt(error, "invalid FAT32 FSInfo signatures"));
		free_count = fsimg_fat_u32(info + 488);
		next_free = fsimg_fat_u32(info + 492);
		if ((free_count != UINT32_MAX && free_count != report->free_clusters) ||
		    (next_free != UINT32_MAX && !fsimg_fat_cluster_valid(g, next_free)))
			return (fsimg_fat_corrupt(error, "inconsistent FAT32 FSInfo hints"));
	}
	if (g->backup_sector != 0 && g->backup_sector != 0xffff) {
		status = fsimg_reader_bytes(reader, 0, primary, sizeof(primary),
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_reader_bytes(reader,
		    (uint64_t)g->backup_sector * g->sector_bytes, backup, sizeof(backup),
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (backup[510] != 0x55 || backup[511] != 0xaa ||
		    bcmp(primary + 11, backup + 11, 79) != 0)
			return (fsimg_fat_corrupt(error, "FAT32 backup BPB copy differs"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = reader->format.fat;
	fsimg_stream_t *sorted;
	fsimg_error_t detail;
	unsigned char *buffer;
	fsimg_status_t status;

	sorted = NULL;
	buffer = NULL;
	reader->session.progress.phase = "FAT verification";
	status = fsimg_stream_new(&reader->session, 16, &fat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, 65536, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}

	/* Traversal prepasses record directory allocations; each returned file
	 * contributes its own complete chain. No resident ownership map is needed.
	 */
	report->directories = 1;
	for (;;) {
		status = fsimg_fat_next(reader, error);
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
			    FSIMG_OP_VERIFY, "verification logical-byte total overflow");
			goto done;
		}
		status = verify_file(reader, buffer, report, error);
		if (status != FSIMG_OK)
			goto done;
	}

	status = fsimg_stream_seal(fat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_sort(fat->owners, owner_compare, NULL, NULL,
	    &sorted, error);
	if (status != FSIMG_OK)
		goto done;
	status = verify_allocations(reader, sorted, report, error);
	if (status != FSIMG_OK)
		goto done;
	status = verify_reserved(reader, report, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		goto done;
	reader->info.free_bytes = report->free_clusters * fat->geometry.cluster_bytes;
	reader->info.valid |= FSIMG_VOLUME_FREE;
done:
	fsimg_dealloc(buffer);
	fsimg_stream_free(sorted);
	fsimg_stream_free(fat->owners);
	fat->owners = NULL;
	return (status);
}
