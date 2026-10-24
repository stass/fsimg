/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write-exfat.h"

typedef struct exfat_geometry_candidate {
	exfat_geometry_t geometry;
	uint64_t	 bitmap_length;
	uint32_t	 bitmap_clusters;
	uint32_t	 allocated;
} exfat_geometry_candidate_t;

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static fsimg_status_t prepare_upcase(exfat_plan_t *, fsimg_error_t *);
static fsimg_status_t prepare_names(exfat_plan_t *, fsimg_error_t *);
static fsimg_status_t count_allocation(exfat_plan_t *, uint32_t, uint64_t *,
    fsimg_error_t *);
static bool volume_geometry(uint64_t, uint32_t, uint32_t, exfat_geometry_t *);
static bool build_geometry_candidate(const fsimg_write_options_t *, uint64_t,
    uint32_t, uint64_t, exfat_geometry_candidate_t *);
static fsimg_status_t choose_geometry(exfat_plan_t *, fsimg_error_t *);
static fsimg_status_t allocate_layout(exfat_plan_t *, fsimg_error_t *);

const fsimg_writer_ops_t fsimg_exfat_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = exfat_write_emit,
	.free = free_plan
};

static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	uint16_t label[11];
	size_t units, length;
	uint32_t sector, cluster;
	fsimg_status_t status;

	sector = options->sector_bytes == 0 ? 512 : options->sector_bytes;
	cluster = options->allocation_unit_bytes;
	if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
	    (cluster != 0 && (cluster < sector || cluster > 33554432 ||
	    (cluster & (cluster - 1)) != 0)) || options->volume_bytes % sector != 0 ||
	    options->volume_bytes > INT64_MAX || options->fat_root_entries != 0 ||
	    ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < 315532800 || options->epoch > 4354819199LL)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid exFAT geometry or reproducible epoch"));
	options->sector_bytes = sector;
	if (options->label == NULL)
		options->label = "NO NAME";
	length = strnlen(options->label, 48);
	if (length == 48)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "exFAT label exceeds UTF-8 backing capacity"));
	status = fsimg_utf8_to_utf16(options->label, length, label, 11, &units, error);
	if (status != FSIMG_OK)
		return (status);
	for (size_t i = 0; i < units; i++) {
		if (!exfat_character_valid(label[i]))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "forbidden exFAT volume label character"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_layout_get(exfat_plan_t *plan, uint64_t id, exfat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[64];
	fsimg_status_t status;

	status = fsimg_array_get(plan->layout, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->length = fsimg_load_le64(raw);
	row->valid_length = fsimg_load_le64(raw + 8);
	row->parent_slot = fsimg_load_le64(raw + 16);
	row->first = fsimg_load_le32(raw + 24);
	row->clusters = fsimg_load_le32(raw + 28);
	bcopy(raw + 32, row->entry, 32);
	return (FSIMG_OK);
}

fsimg_status_t
exfat_layout_set(exfat_plan_t *plan, uint64_t id, const exfat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[64];

	fsimg_store_le64(raw, row->length);
	fsimg_store_le64(raw + 8, row->valid_length);
	fsimg_store_le64(raw + 16, row->parent_slot);
	fsimg_store_le32(raw + 24, row->first);
	fsimg_store_le32(raw + 28, row->clusters);
	bcopy(row->entry, raw + 32, 32);
	return (fsimg_array_set(plan->layout, id, raw, error));
}

static fsimg_status_t
prepare_names(exfat_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	fsimg_stream_t *keys;
	exfat_layout_t row, parent;
	uint16_t units[255];
	size_t count;
	uint64_t parent_id, slots;
	const char *name;
	fsimg_status_t status;

	keys = NULL;
	/* Root starts with bitmap, upcase, label and terminator slots. Each child
	 * replaces the current terminator and adds its own new terminator slot.
	 * Non-root directories start with just the one terminator slot.
	 */
	row = (exfat_layout_t){ .length = 128 };
	status = exfat_layout_set(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_new(&writer->session, 518, &keys, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			goto done;
		name = strrchr(catalog->entry.path, '/') + 1;
		status = exfat_name_units(name, units, &count, error);
		if (status != FSIMG_OK)
			goto done;
		parent_id = catalog->parent_id;
		status = exfat_key_append(keys, parent_id, units, count, error);
		if (status != FSIMG_OK)
			goto done;
		status = exfat_layout_get(plan, parent_id, &parent, error);
		if (status != FSIMG_OK)
			goto done;
		row = (exfat_layout_t){ .parent_slot = parent.length / 32 - 1 };
		slots = 2 + (count + 14) / 15;
		parent.length += slots * 32;
		if (parent.length > EXFAT_DIRECTORY_BYTES) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "exFAT directory exceeds writer size profile");
			goto done;
		}
		status = exfat_layout_set(plan, parent_id, &parent, error);
		if (status != FSIMG_OK)
			goto done;
		row.length = catalog->entry.kind == FSIMG_ENTRY_DIRECTORY ? 32 :
		    catalog->entry.size;
		row.valid_length = catalog->storage == FSIMG_STORAGE_ZERO ? 0 : row.length;
		status = exfat_metadata_encode(&plan->common, &catalog->entry, row.entry, error);
		if (status != FSIMG_OK)
			goto done;
		row.entry[1] = (unsigned char)(slots - 1);
		status = exfat_layout_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			goto done;
		if (catalog->entry.kind == FSIMG_ENTRY_FILE &&
		    catalog->storage != FSIMG_STORAGE_ZERO)
			plan->common.info.initialized_bytes += catalog->entry.size;
	}
	status = exfat_keys_unique(keys, error);
done:
	fsimg_stream_free(keys);
	return (status);
}

static fsimg_status_t
prepare_upcase(exfat_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	uint32_t code, end, output;
	uint16_t mapped;
	fsimg_error_t detail;
	fsimg_status_t status;

	status = fsimg_session_charge(&writer->session, 0, 0, 131072, error);
	if (status != FSIMG_OK)
		return (status);
	plan->upcase = fsimg_alloc(writer->ctx, EXFAT_UPCASE_BYTES, &detail);
	if (plan->upcase == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	for (code = 0; code < 65536; code++) {
		mapped = (uint16_t)fsimg_unicode_upper(code);
		fsimg_store_le16(plan->upcase + code * 2, mapped);
	}

	/* Compress in place: replacing at least three identity words by two
	 * words only moves the write cursor backward. The encoded prefix remains
	 * the single source for layout length, checksum and eventual emission.
	 */
	output = 0;
	for (code = 0; code < 65536; code = end) {
		end = code + 1;
		mapped = fsimg_load_le16(plan->upcase + code * 2);
		if (mapped == code) {
			while (end < 65536 && end - code < 65535 &&
			    fsimg_load_le16(plan->upcase + end * 2) == end)
				end++;
		}
		if (end - code >= 3) {
			fsimg_store_le16(plan->upcase + output, 0xffff);
			fsimg_store_le16(plan->upcase + output + 2, (uint16_t)(end - code));
			output += 4;
		} else {
			for (uint32_t i = code; i < end; i++) {
				fsimg_store_le16(plan->upcase + output,
				    fsimg_load_le16(plan->upcase + i * 2));
				output += 2;
			}
		}
	}
	plan->upcase_length = output;
	for (uint32_t i = 0; i < output; i++)
		plan->upcase_checksum = exfat_checksum32(plan->upcase_checksum, plan->upcase[i]);
	return (FSIMG_OK);
}

static fsimg_status_t
count_allocation(exfat_plan_t *plan, uint32_t cluster_bytes, uint64_t *out,
    fsimg_error_t *error)
{
	exfat_layout_t row;
	uint64_t total, count;
	fsimg_status_t status;

	*out = 0;
	total = (plan->upcase_length + (uint64_t)cluster_bytes - 1) / cluster_bytes;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = exfat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		count = row.length / cluster_bytes + (row.length % cluster_bytes != 0);
		if (!fsimg_add_u64(total, count, &total))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "exFAT allocation total overflow"));
	}
	*out = total;
	return (FSIMG_OK);
}

static bool
volume_geometry(uint64_t volume_bytes, uint32_t sector, uint32_t cluster,
    exfat_geometry_t *g)
{
	uint64_t sectors, spc, low, high, middle, heap, clusters, capacity;

	if (volume_bytes < 1048576 || volume_bytes > INT64_MAX ||
	    volume_bytes % sector != 0)
		return (false);
	sectors = volume_bytes / sector;
	spc = cluster / sector;
	low = 1;
	high = (((uint64_t)EXFAT_CLUSTER_MAX + 2) * 4 + sector - 1) / sector;
	/* Increasing FAT length only decreases the heap count. Binary search
	 * finds its smallest sufficient size without a circular sizing iteration.
	 */
	while (low < high) {
		middle = low + (high - low) / 2;
		heap = (24 + middle + spc - 1) / spc * spc;
		clusters = heap < sectors ? (sectors - heap) / spc : 0;
		if (clusters > EXFAT_CLUSTER_MAX)
			clusters = EXFAT_CLUSTER_MAX;
		capacity = middle * sector / 4;
		if (capacity >= clusters + 2)
			high = middle;
		else
			low = middle + 1;
	}
	heap = (24 + low + spc - 1) / spc * spc;
	if (heap > UINT32_MAX || heap >= sectors)
		return (false);
	clusters = (sectors - heap) / spc;
	if (clusters > EXFAT_CLUSTER_MAX)
		clusters = EXFAT_CLUSTER_MAX;
	if (clusters < 3 || low * sector / 4 < clusters + 2)
		return (false);
	*g = (exfat_geometry_t){ .volume_bytes = volume_bytes,
	    .fat_offset = (uint64_t)24 * sector, .fat_bytes = low * sector,
	    .heap_offset = heap * sector, .sector_bytes = sector,
	    .cluster_bytes = cluster, .clusters = (uint32_t)clusters };
	return (true);
}

static bool
build_geometry_candidate(const fsimg_write_options_t *options, uint64_t bytes,
    uint32_t cluster, uint64_t base, exfat_geometry_candidate_t *candidate)
{
	exfat_geometry_t geometry;
	uint64_t bitmap_bytes, bitmap_clusters, allocated;

	if (!volume_geometry(bytes, options->sector_bytes, cluster, &geometry))
		return (false);
	bitmap_bytes = ((uint64_t)geometry.clusters + 7) / 8;
	bitmap_clusters = (bitmap_bytes + cluster - 1) / cluster;
	if (!fsimg_add_u64(base, bitmap_clusters, &allocated) ||
	    allocated > geometry.clusters ||
	    (geometry.clusters - allocated) * cluster < options->min_free_bytes)
		return (false);
	candidate->geometry = geometry;
	candidate->bitmap_length = bitmap_bytes;
	candidate->bitmap_clusters = (uint32_t)bitmap_clusters;
	candidate->allocated = (uint32_t)allocated;
	return (true);
}

static fsimg_status_t
choose_geometry(exfat_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	exfat_geometry_candidate_t candidate;
	uint64_t base, low, high, middle, max_fat, spc, sector;
	uint32_t cluster, last;
	fsimg_status_t status;

	sector = writer->options.sector_bytes;
	cluster = writer->options.allocation_unit_bytes;
	last = cluster == 0 ? 33554432 : cluster;
	if (cluster == 0)
		cluster = (uint32_t)sector;
	for (; cluster <= last; cluster *= 2) {
		status = count_allocation(plan, cluster, &base, error);
		if (status != FSIMG_OK)
			return (status);
		if (writer->options.volume_bytes != 0) {
			if (!build_geometry_candidate(&writer->options, writer->options.volume_bytes,
			    cluster, base, &candidate))
				continue;
		} else {
			spc = cluster / sector;
			max_fat = (((uint64_t)EXFAT_CLUSTER_MAX + 2) * 4 + sector - 1) / sector;
			high = (24 + max_fat + spc - 1) / spc * spc +
			    (uint64_t)EXFAT_CLUSTER_MAX * spc;
			if (high > INT64_MAX / sector)
				high = INT64_MAX / sector;
			if (!build_geometry_candidate(&writer->options, high * sector,
			    cluster, base, &candidate))
				continue;
			low = 1048576 / sector;
			/* Bitmap allocation is included in each candidate's fit test.
			 * Free capacity is monotone, so no allocation/bitmap fixed point
			 * or per-cluster capacity search is needed.
			 */
			while (low < high) {
				middle = low + (high - low) / 2;
				if (build_geometry_candidate(&writer->options, middle * sector,
				    cluster, base, &candidate))
					high = middle;
				else
					low = middle + 1;
			}
			if (!build_geometry_candidate(&writer->options, low * sector,
			    cluster, base, &candidate))
				continue;
		}
		if (writer->options.allocation_unit_bytes == 0 && cluster != last &&
		    candidate.geometry.clusters > EXFAT_CLUSTER_RECOMMENDED)
			continue;
		plan->geometry = candidate.geometry;
		plan->bitmap_length = candidate.bitmap_length;
		plan->bitmap_clusters = candidate.bitmap_clusters;
		plan->upcase_clusters = (plan->upcase_length + cluster - 1) / cluster;
		plan->allocated = candidate.allocated;
		return (FSIMG_OK);
	}
	return (fsimg_error_set(error, FSIMG_ERANGE, 0,
	    FSIMG_OP_WRITER, "no exFAT geometry fits content and free-space reserve"));
}

static fsimg_status_t
allocate_layout(exfat_plan_t *plan, fsimg_error_t *error)
{
	exfat_layout_t row;
	fsimg_plan_info_t *info = &plan->common.info;
	const exfat_geometry_t *g = &plan->geometry;
	uint64_t next, count, directory_bytes;
	fsimg_status_t status;
	bool directory;

	next = (uint64_t)2 + plan->bitmap_clusters + plan->upcase_clusters;
	directory_bytes = 0;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = exfat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		count = row.length / g->cluster_bytes + (row.length % g->cluster_bytes != 0);
		if (count > (uint64_t)g->clusters + 2 - next)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "exFAT layout exceeds planned allocation"));
		row.first = count == 0 ? 0 : (uint32_t)next;
		row.clusters = (uint32_t)count;
		directory = id == 0 || (fsimg_load_le16(row.entry + 4) & 16) != 0;
		if (directory) {
			row.length = count * g->cluster_bytes;
			row.valid_length = row.length;
			directory_bytes += row.length;
		}
		status = exfat_layout_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		next += count;
	}
	if (next != (uint64_t)plan->allocated + 2)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "exFAT allocation accounting mismatch"));
	plan->root_cluster = 2 + plan->bitmap_clusters + plan->upcase_clusters;
	plan->geometry.root_cluster = plan->root_cluster;
	info->volume_bytes = g->volume_bytes;
	info->sector_bytes = g->sector_bytes;
	info->allocation_unit_bytes = g->cluster_bytes;
	info->allocated_bytes = (uint64_t)plan->allocated * g->cluster_bytes;
	info->free_bytes = (uint64_t)(g->clusters - plan->allocated) * g->cluster_bytes;
	info->metadata_bytes = g->heap_offset + directory_bytes +
	    (uint64_t)(plan->bitmap_clusters + plan->upcase_clusters) * g->cluster_bytes;
	return (FSIMG_OK);
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	exfat_plan_t *plan;
	fsimg_error_t detail;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	plan->common.writer = writer;
	strcpy(plan->common.info.format, "exfat");
	plan->common.info.entries = writer->count;
	plan->common.info.payload_bytes = writer->payload_bytes;
	plan->common.info.required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		plan->common.info.required_io_flags |= FSIMG_IO_DURABLE;
	status = fsimg_array_new(&writer->session, 64, writer->count + 1,
	    &plan->layout, error);
	if (status != FSIMG_OK)
		goto fail;
	status = prepare_upcase(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = prepare_names(plan, error);
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
	free_plan(&plan->common);
	return (status);
}

static void
free_plan(fsimg_plan_t *base)
{
	exfat_plan_t *plan = (exfat_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->layout);
	fsimg_dealloc(plan->upcase);
	fsimg_dealloc(plan);
}
