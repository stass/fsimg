/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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

static uint64_t divide_up(uint64_t, uint64_t);
static fsimg_status_t count_clusters(fat_plan_t *, uint32_t, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t choose_geometry(fat_plan_t *, fsimg_error_t *);
static fat_geometry_result_t try_geometry(fat_plan_t *, uint32_t, uint64_t);
static fsimg_status_t geometry_error(const fat_plan_t *, fat_geometry_result_t,
    fsimg_error_t *);
static fsimg_status_t allocate_layout(fat_plan_t *, fsimg_error_t *);

static uint64_t
divide_up(uint64_t value, uint64_t unit)
{
	return (value / unit + (value % unit != 0 ? 1 : 0));
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
	row->length = fsimg_meta_get_u64(raw);
	row->slots = fsimg_meta_get_u64(raw + 8);
	row->parent_slot = fsimg_meta_get_u64(raw + 16);
	row->first = (uint32_t)fsimg_meta_get_u64(raw + 24);
	row->clusters = (uint32_t)(fsimg_meta_get_u64(raw + 24) >> 32);
	bcopy(raw + 32, row->entry, 32);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_layout_set(fat_plan_t *plan, uint64_t id, const fat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[FAT_LAYOUT_BYTES];

	fsimg_meta_put_u64(raw, row->length);
	fsimg_meta_put_u64(raw + 8, row->slots);
	fsimg_meta_put_u64(raw + 16, row->parent_slot);
	fsimg_meta_put_u64(raw + 24, row->first | (uint64_t)row->clusters << 32);
	bcopy(row->entry, raw + 32, 32);
	return (fsimg_array_set(plan->layout, id, raw, error));
}

static fsimg_status_t
count_clusters(fat_plan_t *plan, uint32_t cluster_bytes, uint64_t *out,
    fsimg_error_t *error)
{
	fat_layout_t row;
	uint64_t total, length, count;
	fsimg_status_t status;

	total = 0;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (id == 0 && plan->variant != 32)
			continue;
		length = row.slots == 0 ? row.length : row.slots * 32;
		count = divide_up(length, cluster_bytes);
		if (!fsimg_add_u64(total, count, &total))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "FAT allocation total overflows"));
	}
	*out = total;
	return (FSIMG_OK);
}

/* For exact size, increasing FAT length decreases the number of data clusters
 * and increases table capacity. Binary search finds the first sufficient FAT
 * in at most 32 steps. Oversized metadata cannot be a valid candidate.
 */
static fat_geometry_result_t
try_geometry(fat_plan_t *plan, uint32_t cluster_bytes, uint64_t allocated)
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
	plan->clusters = (uint32_t)clusters;
	plan->allocated = (uint32_t)allocated;
	plan->fat_offset = (uint64_t)plan->reserved_sectors * sector;
	plan->fat_bytes = fat_sectors * sector;
	plan->root_offset = plan->fat_offset + 2 * plan->fat_bytes;
	plan->data_offset = plan->root_offset + root_sectors * sector;
	plan->common.info.volume_bytes = sectors * sector;
	plan->common.info.allocation_unit_bytes = cluster_bytes;
	plan->common.info.free_bytes = (clusters - allocated) * cluster_bytes;
	plan->common.info.allocated_bytes = allocated * cluster_bytes;
	return (FAT_GEOMETRY_FITS);
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

static fsimg_status_t
choose_geometry(fat_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options;
	fat_layout_t root;
	uint64_t root_entries, allocated;
	uint32_t cluster_bytes, maximum, sector;
	fsimg_status_t status;
	fat_geometry_result_t reason, rejected;

	options = &plan->common.writer->options;
	sector = options->sector_bytes;
	plan->reserved_sectors = plan->variant == 32 ? 32 : 1;
	status = fsimg_fat_layout_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->variant != 32) {
		root_entries = options->fat_root_entries;
		if (root_entries == 0) {
			root_entries = divide_up(root.slots, sector / 32) * (sector / 32);
			if (root_entries < 512)
				root_entries = 512;
		}
		if (root_entries < root.slots || root_entries > UINT16_MAX ||
		    root_entries % (sector / 32) != 0)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "FAT fixed root capacity is insufficient or invalid"));
		plan->root_entries = (uint32_t)root_entries;
	}
	cluster_bytes = options->allocation_unit_bytes;
	maximum = cluster_bytes == 0 ? 32768 : cluster_bytes;
	if (cluster_bytes == 0)
		cluster_bytes = sector;
	rejected = FAT_GEOMETRY_NONE;
	for (; cluster_bytes <= maximum; cluster_bytes *= 2) {
		status = count_clusters(plan, cluster_bytes, &allocated, error);
		if (status != FSIMG_OK)
			return (status);
		reason = try_geometry(plan, cluster_bytes, allocated);
		if (reason == FAT_GEOMETRY_FITS)
			return (FSIMG_OK);
		if (rejected == FAT_GEOMETRY_NONE)
			rejected = reason;
		else if (rejected != reason)
			rejected = FAT_GEOMETRY_CONFLICT;
	}
	return (geometry_error(plan, rejected, error));
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
		fsimg_fat_put16(row.entry + 26, (uint16_t)row.first);
		if (plan->variant == 32)
			fsimg_fat_put16(row.entry + 20, (uint16_t)(row.first >> 16));
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
fsimg_fat_write_validate(fsimg_write_options_t *selected, unsigned variant,
    fsimg_error_t *error)
{
	uint32_t sector, cluster;
	size_t length;

	sector = selected->sector_bytes == 0 ? 512 : selected->sector_bytes;
	cluster = selected->allocation_unit_bytes;
	if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
	    (cluster != 0 && (cluster < sector || cluster > 32768 ||
	    (cluster & (cluster - 1)) != 0)) ||
	    selected->volume_bytes % sector != 0 ||
	    selected->volume_bytes / sector > UINT32_MAX ||
	    selected->fat_root_entries > UINT16_MAX ||
	    (variant == 32 && selected->fat_root_entries != 0) ||
	    ((selected->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (selected->epoch < 315532800 || selected->epoch > 4354819199LL)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT geometry or reproducible epoch"));
	selected->sector_bytes = sector;
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
