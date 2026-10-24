/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"

/* The payload buffer: the largest physical read of one batch. */
#define PAYLOAD_BYTES 65536u

static fsimg_status_t next_owner(fsimg_owner_runs_t *, fsimg_owner_run_t *,
    bool *, fsimg_error_t *);
static fsimg_status_t verify_file(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t verify_allocations(fsimg_reader_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t verify_reserved(fsimg_reader_t *,
    const fsimg_verify_report_t *, fsimg_error_t *);

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
verify_file(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	fat_chain_t chain;
	fsimg_extent_t run;
	uint64_t position, length;
	size_t batch, transferred;
	fsimg_status_t status;

	fsimg_fat_chain_init(&chain, fsimg_fat_state(reader)->current_cluster,
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
		if (run.logical_offset >= reader->entry.size)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_VERIFY, "FAT chain run starts past the file"));
		length = run.length;
		if (length > reader->entry.size - run.logical_offset)
			length = reader->entry.size - run.logical_offset;
		for (position = 0; position < length; position += batch) {
			batch = length - position > PAYLOAD_BYTES ? PAYLOAD_BYTES :
			    (size_t)(length - position);
			status = fsimg_reader_bytes(reader, run.image_offset + position,
			    buffer, batch, &transferred, error);
			fsimg_stat_add(&report->payload_bytes_read, transferred);
			if (status != FSIMG_OK)
				return (status);
		}
	}
}

static fsimg_status_t
verify_allocations(fsimg_reader_t *reader, fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	fsimg_owner_runs_t *owners = fsimg_fat_state(reader)->owners;
	fsimg_owner_run_t run;
	uint32_t value;
	fsimg_status_t status;
	bool have_run, claimed;

	status = next_owner(owners, &run, &have_run, error);
	if (status != FSIMG_OK)
		return (status);

	/* Merge the sorted ownership runs with a single sequential FAT scan.
	 * Every allocated cluster has exactly one owner; gaps must be free or
	 * bad. The run utility rejects overlapping runs as it returns them.
	 */
	for (uint32_t cluster = 2; cluster - 2 < g->clusters; cluster++) {
		while (have_run && cluster >= run.end) {
			status = next_owner(owners, &run, &have_run, error);
			if (status != FSIMG_OK)
				return (status);
		}
		claimed = have_run && cluster >= run.first;
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

	/* Drain the runs: a run ending at the last cluster is never advanced
	 * past by the scan, and a duplicate or damaged record after it must
	 * still be read and rejected. The utility fails any later run, so the
	 * check after it is only defensive.
	 */
	status = next_owner(owners, &run, &have_run, error);
	if (status == FSIMG_OK && have_run)
		status = fsimg_fat_corrupt(error, "FAT ownership exceeds allocation table");
	return (status);
}

static fsimg_status_t
verify_reserved(fsimg_reader_t *reader, const fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	unsigned char primary[512], backup[512], info[512];
	uint32_t free_count, next_free;
	size_t transferred;
	fsimg_status_t status;

	if (!reader->info.clean || reader->info.io_error)
		return (fsimg_fat_corrupt(error, "FAT volume is dirty or records an I/O error"));
	if (fsimg_fat_state(reader)->root_label_seen &&
	    (reader->info.valid & FSIMG_VOLUME_LABEL) != 0 &&
	    strcmp(fsimg_fat_state(reader)->root_label, reader->info.label) != 0)
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
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	fsimg_error_t detail;
	unsigned char *buffer;
	fsimg_status_t status;

	buffer = NULL;
	reader->session.progress.phase = "FAT verification";
	/* One exclusive domain: the heap's cluster numbers. */
	status = fsimg_owner_runs_new(&reader->session, 2,
	    (uint64_t)fat->geometry.clusters + 2, &fat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, PAYLOAD_BYTES, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}

	/* Directory preparation records directory allocations; each returned file
	 * contributes its own complete chain. No resident ownership map is needed.
	 */
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
		status = verify_file(reader, buffer, report, error);
		if (status != FSIMG_OK)
			goto done;
	}

	status = fsimg_owner_runs_seal(fat->owners, error);
	if (status != FSIMG_OK)
		goto done;
	status = verify_allocations(reader, report, error);
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
	fsimg_owner_runs_free(fat->owners);
	fat->owners = NULL;
	return (status);
}
