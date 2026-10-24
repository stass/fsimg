/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "fsimg-endian.h"
#include "host.h"

static fsimg_status_t next_owner(fsimg_owner_runs_t *, fsimg_owner_run_t *,
    bool *, fsimg_error_t *);
static fsimg_status_t verify_allocations(fsimg_reader_t *,
    fsimg_verify_report_t *, fsimg_error_t *);

/* Advance to the next sorted ownership run; *have_run is false after the
 * last. The run utility rejects overlapping and damaged runs as it returns
 * them.
 */
static fsimg_status_t
next_owner(fsimg_owner_runs_t *owners, fsimg_owner_run_t *run, bool *have_run,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_owner_runs_next(owners, run, error);
	*have_run = status == FSIMG_OK;
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	return (status);
}

static fsimg_status_t
verify_allocations(fsimg_reader_t *reader, fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	const exfat_geometry_t *g = &exfat_state(reader)->geometry;
	fsimg_owner_runs_t *owners = exfat_state(reader)->owners;
	fsimg_owner_run_t run;
	unsigned char bitmap[4096];
	uint64_t bitmap_offset, window;
	uint32_t value;
	size_t batch;
	bool have_run, claimed, allocated;
	fsimg_status_t status;

	bitmap_offset = UINT64_MAX;
	status = next_owner(owners, &run, &have_run, error);
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
			status = exfat_index_read(reader, exfat_state(reader)->bitmap,
			    window, bitmap, batch, error);
			if (status != FSIMG_OK)
				return (status);
			bitmap_offset = window;
		}
		while (have_run && cluster >= run.end) {
			status = next_owner(owners, &run, &have_run, error);
			if (status != FSIMG_OK)
				return (status);
		}
		claimed = have_run && cluster >= run.first;
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

	/* Drain the runs: a run ending at the last cluster is never advanced
	 * past by the scan, and a duplicate or damaged record after it must
	 * still be read and rejected. The utility fails any later run, so the
	 * check after it is only defensive.
	 */
	status = next_owner(owners, &run, &have_run, error);
	if (status == FSIMG_OK && have_run)
		status = exfat_corrupt(error, "exFAT ownership exceeds heap");
	return (status);
}

fsimg_status_t
exfat_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	buffer = NULL;
	reader->session.progress.phase = "exFAT verification";
	/* One exclusive domain: the heap's cluster numbers. */
	status = fsimg_owner_runs_new(&reader->session, 2,
	    (uint64_t)exfat->geometry.clusters + 2, &exfat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	/* Indexes built while opening predate the owner runs; every index
	 * built during the traversal below claims its own runs.
	 */
	status = exfat_index_claim(reader, exfat->bitmap, error);
	if (status != FSIMG_OK)
		goto done;
	status = exfat_index_claim(reader, exfat->upcase_index, error);
	if (status != FSIMG_OK)
		goto done;
	status = exfat_index_claim(reader, exfat->root, error);
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
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		status = fsimg_verify_account(reader, report, error);
		if (status != FSIMG_OK)
			goto done;
		if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		if (buffer != NULL) {
			status = fsimg_verify_payload(reader, buffer,
			    EXFAT_BUFFER_BYTES, report, error);
			if (status != FSIMG_OK)
				goto done;
		}
	}
	status = fsimg_owner_runs_seal(exfat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	status = verify_allocations(reader, report, error);
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
	fsimg_owner_runs_free(exfat->owners);
	exfat->owners = NULL;
	fsimg_dealloc(buffer);
	return (status);
}
