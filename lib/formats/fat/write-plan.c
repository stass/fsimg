/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write.h"

/* A candidate explains its own rejection. Only agreement across every
 * candidate establishes one cause for a failed automatic cluster search.
 */
typedef enum fat_geometry_result {
	FAT_GEOMETRY_NONE,
	FAT_GEOMETRY_FITS,
	FAT_GEOMETRY_TOO_SMALL,
	FAT_GEOMETRY_TOO_LARGE,
	FAT_GEOMETRY_CAPACITY,
	FAT_GEOMETRY_SPACE,
	FAT_GEOMETRY_FIELD_LIMIT,
	FAT_GEOMETRY_CONFLICT
} fat_geometry_result_t;

/* The cluster sizes a search can try: the powers of two from the sector to
 * 32 KiB, at most seven with 512-byte sectors.
 */
#define FAT_UNITS_MAX 7u
#define FAT_CLUSTER_MAX 32768u

/* One candidate cluster size's geometry; the data area follows the
 * reserved sectors, both FAT copies and the fixed root.
 */
typedef struct fat_candidate {
	uint64_t	 volume_bytes;
	uint64_t	 fat_bytes;
	uint32_t	 cluster_bytes;
	uint32_t	 clusters;
	uint32_t	 allocated;
} fat_candidate_t;

static uint64_t divide_up(uint64_t, uint64_t);
static uint64_t root_sectors_entries(uint64_t, uint32_t);
static fsimg_status_t count_clusters(fat_plan_t *, uint32_t, unsigned,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t choose_root(fat_plan_t *, fsimg_error_t *);
static fsimg_status_t choose_geometry(fat_plan_t *, fsimg_error_t *);
static fat_geometry_result_t try_geometry(const fat_plan_t *, uint32_t,
    uint64_t, fat_candidate_t *);
static void apply_geometry(fat_plan_t *, const fat_candidate_t *);
static fsimg_status_t geometry_error(const fat_plan_t *, fat_geometry_result_t,
    fsimg_error_t *);
static fsimg_status_t allocate_layout(fat_plan_t *, fsimg_error_t *);

static uint64_t
divide_up(uint64_t value, uint64_t unit)
{
	return (value / unit + (value % unit != 0 ? 1 : 0));
}

/* entries rounded up to whole sectors of 32-byte directory entries: the
 * only capacities a FAT12/16 fixed root region can have.
 */
static uint64_t
root_sectors_entries(uint64_t entries, uint32_t sector_bytes)
{
	uint64_t per_sector = sector_bytes / 32;

	return (divide_up(entries, per_sector) * per_sector);
}

fsimg_status_t
fsimg_fat_layout_get(fat_plan_t *plan, uint64_t id, fat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[FAT_LAYOUT_BYTES];
	fsimg_status_t status;

	bzero(row, sizeof(*row));
	status = fsimg_array_get(plan->layout, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->length = fsimg_load_le64(raw);
	row->slots = fsimg_load_le64(raw + 8);
	row->parent_slot = fsimg_load_le64(raw + 16);
	row->first = (uint32_t)fsimg_load_le64(raw + 24);
	row->clusters = (uint32_t)(fsimg_load_le64(raw + 24) >> 32);
	bcopy(raw + 32, row->entry, 32);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_layout_set(fat_plan_t *plan, uint64_t id, const fat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[FAT_LAYOUT_BYTES];

	fsimg_store_le64(raw, row->length);
	fsimg_store_le64(raw + 8, row->slots);
	fsimg_store_le64(raw + 16, row->parent_slot);
	fsimg_store_le64(raw + 24, row->first | (uint64_t)row->clusters << 32);
	bcopy(row->entry, raw + 32, 32);
	return (fsimg_array_set(plan->layout, id, raw, error));
}

/* The clusters every candidate size needs, in one pass over the layout
 * rows: totals[i] is the allocation with clusters of first << i bytes. The
 * FAT12/16 root lives outside the data area and needs none.
 */
static fsimg_status_t
count_clusters(fat_plan_t *plan, uint32_t first, unsigned count,
    uint64_t *totals, fsimg_error_t *error)
{
	fat_layout_t row;
	uint64_t length;
	fsimg_status_t status;

	for (unsigned i = 0; i < count; i++)
		totals[i] = 0;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (id == 0 && plan->variant != 32)
			continue;
		length = row.slots == 0 ? row.length : row.slots * 32;
		for (unsigned i = 0; i < count; i++) {
			if (!fsimg_add_u64(totals[i],
			    divide_up(length, (uint64_t)first << i), &totals[i]))
				return (fsimg_error_set(error, FSIMG_ERANGE, 0,
				    FSIMG_OP_WRITER, "FAT allocation total overflows"));
		}
	}
	return (FSIMG_OK);
}

/* The geometry with clusters of cluster_bytes holding allocated of them and
 * the free reserve, or why there is none; the plan is unchanged. For exact
 * size, increasing FAT length decreases the number of data clusters
 * and increases table capacity. Binary search finds the first sufficient FAT
 * in at most 32 steps. Oversized metadata cannot be a valid candidate.
 */
static fat_geometry_result_t
try_geometry(const fat_plan_t *plan, uint32_t cluster_bytes, uint64_t allocated,
    fat_candidate_t *out)
{
	const fsimg_write_options_t *options;
	uint64_t minimum, maximum, needed, clusters, sectors, fat_sectors;
	uint64_t root_sectors, overhead, low, high, middle, capacity, data_sectors;
	uint32_t sector, spc;

	options = &plan->common.writer->options;
	sector = options->sector_bytes;
	spc = cluster_bytes / sector;
	minimum = plan->variant == 12 ? 1 : plan->variant == 16 ? 4085 : 65525;
	maximum = plan->variant == 12 ? 4084 :
	    plan->variant == 16 ? 65524 : 0x0ffffff5;
	/* Microsoft's formatting recommendation keeps automatic volumes away
	 * from type cutovers that other implementations can misclassify. Exact
	 * requests retain every legal count; upper margins can advance the
	 * automatic search to its next permitted cluster size.
	 */
	if (options->volume_bytes == 0) {
		if (plan->variant != 12)
			minimum += FAT_AUTO_TYPE_MARGIN;
		if (plan->variant != 32)
			maximum -= FAT_AUTO_TYPE_MARGIN - 1;
	}
	needed = divide_up(options->min_free_bytes, cluster_bytes);
	if (!fsimg_add_u64(allocated, needed, &needed))
		return (FAT_GEOMETRY_CAPACITY);
	if (needed < minimum)
		needed = minimum;
	if (needed > maximum)
		return (FAT_GEOMETRY_CAPACITY);
	root_sectors = (uint64_t)plan->root_entries * 32 / sector;
	overhead = plan->reserved_sectors + root_sectors;
	if (options->volume_bytes == 0) {
		clusters = needed;
		fat_sectors = divide_up(divide_up((clusters + 2) * plan->variant, 8),
		    sector);
		sectors = overhead + 2 * fat_sectors + clusters * spc;
	} else {
		sectors = options->volume_bytes / sector;
		if (sectors <= overhead + 2)
			return (FAT_GEOMETRY_TOO_SMALL);
		low = 1;
		high = (sectors - overhead) / 2;
		while (low < high) {
			middle = low + (high - low) / 2;
			data_sectors = sectors - overhead - 2 * middle;
			clusters = data_sectors / spc;
			capacity = middle * sector * 8 / plan->variant;
			if (capacity >= clusters + 2)
				high = middle;
			else
				low = middle + 1;
		}
		fat_sectors = low;
		clusters = (sectors - overhead - 2 * fat_sectors) / spc;
	}
	if (clusters < minimum)
		return (FAT_GEOMETRY_TOO_SMALL);
	if (clusters > maximum)
		return (FAT_GEOMETRY_TOO_LARGE);
	if (clusters < needed)
		return (FAT_GEOMETRY_SPACE);
	if (sectors > UINT32_MAX || fat_sectors == 0 ||
	    (plan->variant != 32 && fat_sectors > UINT16_MAX))
		return (FAT_GEOMETRY_FIELD_LIMIT);
	*out = (fat_candidate_t){ .volume_bytes = sectors * sector,
	    .fat_bytes = fat_sectors * sector, .cluster_bytes = cluster_bytes,
	    .clusters = (uint32_t)clusters, .allocated = (uint32_t)allocated };
	return (FAT_GEOMETRY_FITS);
}

static void
apply_geometry(fat_plan_t *plan, const fat_candidate_t *g)
{
	fsimg_plan_info_t *info = &plan->common.info;
	uint32_t sector = plan->common.writer->options.sector_bytes;

	plan->clusters = g->clusters;
	plan->allocated = g->allocated;
	plan->fat_offset = (uint64_t)plan->reserved_sectors * sector;
	plan->fat_bytes = g->fat_bytes;
	plan->root_offset = plan->fat_offset + 2 * plan->fat_bytes;
	plan->data_offset = plan->root_offset + (uint64_t)plan->root_entries * 32;
	info->volume_bytes = g->volume_bytes;
	info->allocation_unit_bytes = g->cluster_bytes;
	info->allocation_units = g->clusters;
	info->root_entries = plan->root_entries;
	info->free_bytes = (uint64_t)(g->clusters - g->allocated) * g->cluster_bytes;
	info->allocated_bytes = (uint64_t)g->allocated * g->cluster_bytes;
}

static fsimg_status_t
geometry_error(const fat_plan_t *plan, fat_geometry_result_t reason,
    fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	char cluster_context[48], message[256];

	/* An explicit cluster size narrows the search. Its capacity failure does
	 * not establish that the source exceeds every supported cluster size.
	 */
	cluster_context[0] = '\0';
	if (options->allocation_unit_bytes != 0)
		snprintf(cluster_context, sizeof(cluster_context), " with %u-byte clusters",
		    options->allocation_unit_bytes);

	switch (reason) {
	case FAT_GEOMETRY_TOO_SMALL:
	case FAT_GEOMETRY_TOO_LARGE:
		/* Automatic sizing derives a legal cluster count or rejects capacity
		 * or field limits; only explicit sizes reach these range failures.
		 */
		snprintf(message, sizeof(message), "image size %llu bytes is too %s for FAT%u%s (%u-byte sectors)",
		    (unsigned long long)options->volume_bytes,
		    reason == FAT_GEOMETRY_TOO_SMALL ? "small" : "large", plan->variant,
		    cluster_context, options->sector_bytes);
		break;
	case FAT_GEOMETRY_CAPACITY:
		snprintf(message, sizeof(message), "%s too large for FAT%u%s",
		    options->min_free_bytes == 0 ? "source tree is" :
		    "source tree and requested free space are", plan->variant, cluster_context);
		break;
	case FAT_GEOMETRY_SPACE:
		snprintf(message, sizeof(message), "image size %llu bytes cannot fit the source tree%s as FAT%u%s",
		    (unsigned long long)options->volume_bytes,
		    options->min_free_bytes == 0 ? "" : " and requested free space",
		    plan->variant, cluster_context);
		break;
	case FAT_GEOMETRY_FIELD_LIMIT:
		snprintf(message, sizeof(message), "required image exceeds FAT%u size limits with %u-byte sectors",
		    plan->variant, options->sector_bytes);
		break;
	default:
		snprintf(message, sizeof(message), "cannot create FAT%u: requested size and allocation settings conflict",
		    plan->variant);
		break;
	}
	return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER, message));
}

/* The FAT12/16 fixed root: the requested capacity, which validation has
 * rounded up to whole sectors, or the smallest whole number of sectors
 * holding the root's slots, at least 512 entries under the default policy
 * and exactly that under minimization. Either must hold the root's slots
 * and fit the 16-bit entry count. FAT32 has none.
 */
static fsimg_status_t
choose_root(fat_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	fat_layout_t root;
	uint64_t root_entries;
	fsimg_status_t status;
	char message[128];

	if (plan->variant == 32)
		return (FSIMG_OK);
	status = fsimg_fat_layout_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	root_entries = options->fat_root_entries;
	if (root_entries == 0) {
		root_entries = root_sectors_entries(root.slots, options->sector_bytes);
		if ((options->flags & FSIMG_WRITE_MINIMIZE) == 0 && root_entries < 512)
			root_entries = 512;
	}
	if (root_entries < root.slots || root_entries > UINT16_MAX) {
		if (root_entries < root.slots)
			snprintf(message, sizeof(message), "FAT root directory "
			    "capacity %llu is below the %llu entries the root needs",
			    (unsigned long long)root_entries,
			    (unsigned long long)root.slots);
		else
			snprintf(message, sizeof(message), "FAT root directory "
			    "needs %llu entries, above the 65535 a fixed root holds",
			    (unsigned long long)root_entries);
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    message));
	}
	plan->root_entries = (uint32_t)root_entries;
	return (FSIMG_OK);
}

/* The cluster size: the requested one, or every legal size in increasing
 * order (specs/design-sizing.md). The default policy takes the first that
 * fits; minimization the one with the smallest volume, the earliest on a
 * tie, so it agrees with the default whenever that is already minimal.
 * Each candidate evaluated charges one work unit.
 */
static fsimg_status_t
choose_geometry(fat_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint64_t totals[FAT_UNITS_MAX];
	fat_candidate_t candidate, best;
	uint32_t first, last;
	unsigned count;
	fsimg_status_t status;
	fat_geometry_result_t reason, rejected;
	bool minimize, found;

	plan->reserved_sectors = plan->variant == 32 ? 32 : 1;
	status = choose_root(plan, error);
	if (status != FSIMG_OK)
		return (status);
	minimize = (options->flags & FSIMG_WRITE_MINIMIZE) != 0;
	first = options->allocation_unit_bytes;
	last = first == 0 ? FAT_CLUSTER_MAX : first;
	if (first == 0)
		first = options->sector_bytes;
	count = 0;
	while (count < FAT_UNITS_MAX && (first << count) <= last)
		count++;
	status = count_clusters(plan, first, count, totals, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&best, sizeof(best));
	found = false;
	rejected = FAT_GEOMETRY_NONE;
	for (unsigned i = 0; i < count; i++) {
		status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
		    error);
		if (status != FSIMG_OK)
			return (status);
		reason = try_geometry(plan, first << i, totals[i], &candidate);
		if (reason != FAT_GEOMETRY_FITS) {
			rejected = rejected == FAT_GEOMETRY_NONE || rejected == reason ?
			    reason : FAT_GEOMETRY_CONFLICT;
			continue;
		}
		if (!found || candidate.volume_bytes < best.volume_bytes)
			best = candidate;
		found = true;
		if (!minimize)
			break;
	}
	if (!found)
		return (geometry_error(plan, rejected, error));
	apply_geometry(plan, &best);
	return (FSIMG_OK);
}

static fsimg_status_t
allocate_layout(fat_plan_t *plan, fsimg_error_t *error)
{
	fat_layout_t row;
	uint64_t directory_bytes;
	uint32_t next, cluster_bytes;
	fsimg_status_t status;

	next = 2;
	directory_bytes = 0;
	cluster_bytes = plan->common.info.allocation_unit_bytes;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.slots != 0)
			row.length = row.slots * 32;
		if (id != 0 || plan->variant == 32) {
			row.clusters = (uint32_t)divide_up(row.length, cluster_bytes);
			if (row.clusters != 0) {
				row.first = next;
				next += row.clusters;
			}
			if (row.slots != 0)
				directory_bytes += (uint64_t)row.clusters * cluster_bytes;
		}
		fsimg_store_le16(row.entry + 26, (uint16_t)row.first);
		if (plan->variant == 32)
			fsimg_store_le16(row.entry + 20, (uint16_t)(row.first >> 16));
		status = fsimg_fat_layout_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->common.info.metadata_bytes = plan->data_offset + directory_bytes;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_write_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fat_plan_t *plan;
	fsimg_error_t detail;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	plan->common.writer = writer;
	plan->variant = writer->variant;
	snprintf(plan->common.info.format, sizeof(plan->common.info.format), "fat%u", plan->variant);
	plan->common.info.payload_bytes = writer->payload_bytes;
	plan->common.info.initialized_bytes = writer->payload_bytes;
	plan->common.info.entries = writer->count;
	plan->common.info.sector_bytes = writer->options.sector_bytes;
	plan->common.info.required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		plan->common.info.required_io_flags |= FSIMG_IO_DURABLE;
	status = fsimg_array_new(&writer->session, FAT_LAYOUT_BYTES,
	    writer->count + 1, &plan->layout, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_fat_write_names(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = choose_geometry(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = allocate_layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	plan->common.info.scratch_bytes = writer->ctx->stats.scratch_bytes;
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	fsimg_fat_write_free(&plan->common);
	return (status);
}

void
fsimg_fat_write_free(fsimg_plan_t *base)
{
	fat_plan_t *plan = (fat_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->layout);
	fsimg_dealloc(plan);
}

fsimg_status_t
fsimg_fat_write_validate(fsimg_write_options_t *selected,
    unsigned variant FSIMG_UNUSED, fsimg_error_t *error)
{
	uint64_t root_entries;
	uint32_t sector, cluster;
	size_t length;

	sector = selected->sector_bytes == 0 ? 512 : selected->sector_bytes;
	cluster = selected->allocation_unit_bytes;
	if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
	    (cluster != 0 && (cluster < sector || cluster > FAT_CLUSTER_MAX ||
	    (cluster & (cluster - 1)) != 0)) ||
	    selected->volume_bytes % sector != 0 ||
	    selected->volume_bytes / sector > UINT32_MAX ||
	    ((selected->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (selected->epoch < 315532800 || selected->epoch > 4354819199LL)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT geometry or reproducible epoch"));
	selected->sector_bytes = sector;
	/* A requested root capacity rounds up to whole sectors of 32-byte
	 * entries; the common settings check has already refused one on FAT32,
	 * which has no fixed root. Rounding is idempotent, so a second
	 * validation of the selected options keeps the same capacity.
	 */
	if (selected->fat_root_entries != 0) {
		root_entries = root_sectors_entries(selected->fat_root_entries,
		    sector);
		if (root_entries > UINT16_MAX)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "FAT root directory capacity exceeds "
			    "65535 entries once rounded up to whole sectors"));
		selected->fat_root_entries = (uint32_t)root_entries;
	}
	if (selected->label == NULL)
		selected->label = "NO NAME";
	length = strnlen(selected->label, 12);
	if (length == 0 || length > 11 || selected->label[0] == ' ' ||
	    selected->label[length - 1] == ' ')
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT volume label length or spacing"));
	for (size_t i = 0; i < length; i++) {
		if (!((selected->label[i] >= 'A' && selected->label[i] <= 'Z') ||
		    (selected->label[i] >= '0' && selected->label[i] <= '9') ||
		    strchr(" $%'-_@~`!(){}^#&", selected->label[i]) != NULL))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "FAT label requires uppercase ASCII label characters"));
	}
	return (FSIMG_OK);
}
