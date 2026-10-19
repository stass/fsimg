/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "emit.h"
#include "unicode.h"
#include "write-exfat.h"

static unsigned size_shift(uint32_t);
static fsimg_status_t emit_boot(exfat_plan_t *, fsimg_emit_t *, fsimg_error_t *);
static fsimg_status_t emit_chain(exfat_plan_t *, fsimg_emit_t *, uint32_t,
    uint32_t, fsimg_error_t *);
static fsimg_status_t emit_metadata(exfat_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_payload(exfat_plan_t *, fsimg_emit_t *, uint64_t,
    const exfat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_child(exfat_plan_t *, fsimg_emit_t *, const char *,
    const exfat_layout_t *, const exfat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_catalog(exfat_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);

static unsigned
size_shift(uint32_t bytes)
{
	unsigned shift = 0;

	while (bytes > 1) {
		bytes >>= 1;
		shift++;
	}
	return (shift);
}

/* The boot region is twelve sectors: the boot sector, eight extended boot
 * sectors, OEM parameters, a reserved sector, and a sector repeating the
 * region checksum. The checksum skips VolumeFlags (bytes 106-107) and
 * PercentInUse (112). The backup region follows immediately. Twelve sectors
 * of at most 4096 bytes fit the emission buffer.
 */
static fsimg_status_t
emit_boot(exfat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	const exfat_geometry_t *g = &plan->geometry;
	uint32_t checksum;
	size_t length;
	fsimg_status_t status;

	length = (size_t)12 * g->sector_bytes;
	bzero(buffer, length);
	bcopy("\xeb\x76\x90" "EXFAT   ", buffer, 11);
	fsimg_meta_put_u64(buffer + 72, g->volume_bytes / g->sector_bytes);
	exfat_put32(buffer + 80, (uint32_t)(g->fat_offset / g->sector_bytes));
	exfat_put32(buffer + 84, (uint32_t)(g->fat_bytes / g->sector_bytes));
	exfat_put32(buffer + 88, (uint32_t)(g->heap_offset / g->sector_bytes));
	exfat_put32(buffer + 92, g->clusters);
	exfat_put32(buffer + 96, plan->root_cluster);
	exfat_put32(buffer + 100, plan->common.writer->options.volume_serial);
	exfat_put16(buffer + 104, 0x0100);
	buffer[108] = (unsigned char)size_shift(g->sector_bytes);
	buffer[109] = (unsigned char)size_shift(g->cluster_bytes / g->sector_bytes);
	buffer[110] = 1;
	buffer[111] = 0x80;
	buffer[112] = (unsigned char)((uint64_t)plan->allocated * 100 / g->clusters);
	memset(buffer + 120, 0xf4, 390);
	buffer[510] = 0x55;
	buffer[511] = 0xaa;
	for (unsigned i = 1; i <= 8; i++)
		exfat_put32(buffer + (size_t)(i + 1) * g->sector_bytes - 4,
		    UINT32_C(0xaa550000));
	checksum = 0;
	for (size_t i = 0; i < (size_t)11 * g->sector_bytes; i++) {
		if (i != 106 && i != 107 && i != 112)
			checksum = exfat_checksum32(checksum, buffer[i]);
	}
	for (size_t i = (size_t)11 * g->sector_bytes; i < length; i += 4)
		exfat_put32(buffer + i, checksum);
	status = fsimg_emit_write(emit, 0, buffer, length, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_emit_write(emit, length, buffer, length, error));
}

/* Write the FAT entries of a contiguous run of count clusters starting at
 * first: each entry links to the next cluster and the last ends the chain.
 */
static fsimg_status_t
emit_chain(exfat_plan_t *plan, fsimg_emit_t *emit, uint32_t first,
    uint32_t count, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	uint32_t emitted, batch, cluster;
	fsimg_status_t status;

	emitted = 0;
	while (emitted < count) {
		batch = count - emitted;
		if (batch > FSIMG_EMIT_BUFFER_BYTES / 4)
			batch = FSIMG_EMIT_BUFFER_BYTES / 4;
		for (uint32_t i = 0; i < batch; i++) {
			cluster = first + emitted + i;
			exfat_put32(buffer + i * 4, emitted + i + 1 == count ?
			    UINT32_MAX : cluster + 1);
		}
		status = fsimg_emit_write(emit,
		    plan->geometry.fat_offset + ((uint64_t)first + emitted) * 4,
		    buffer, (size_t)batch * 4, error);
		if (status != FSIMG_OK)
			return (status);
		emitted += batch;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
emit_metadata(exfat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	const exfat_geometry_t *g = &plan->geometry;
	exfat_layout_t root;
	unsigned char header[96];
	uint16_t label[11];
	uint64_t remaining, offset;
	size_t batch, label_count;
	uint32_t upcase_first;
	fsimg_status_t status;

	status = exfat_layout_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	upcase_first = 2 + plan->bitmap_clusters;
	exfat_put32(buffer, UINT32_C(0xfffffff8));
	exfat_put32(buffer + 4, UINT32_MAX);
	status = fsimg_emit_write(emit, g->fat_offset, buffer, 8, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, emit, 2, plan->bitmap_clusters, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, emit, upcase_first, plan->upcase_clusters, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, emit, root.first, root.clusters, error);
	if (status != FSIMG_OK)
		return (status);

	/* Allocation is a deterministic prefix. The common zero-initialization
	 * already defines all free bits, unused FAT entries and cluster slack.
	 */
	remaining = ((uint64_t)plan->allocated + 7) / 8;
	offset = 0;
	while (remaining != 0) {
		batch = remaining > FSIMG_EMIT_BUFFER_BYTES ? FSIMG_EMIT_BUFFER_BYTES : (size_t)remaining;
		memset(buffer, 0xff, batch);
		if (remaining == batch && plan->allocated % 8 != 0)
			buffer[batch - 1] = (unsigned char)((1u << (plan->allocated % 8)) - 1);
		status = fsimg_emit_write(emit, exfat_cluster_offset(g, 2) + offset,
		    buffer, batch, error);
		if (status != FSIMG_OK)
			return (status);
		offset += batch;
		remaining -= batch;
	}
	for (offset = 0; offset < plan->upcase_length; offset += batch) {
		batch = plan->upcase_length - offset > FSIMG_EMIT_BUFFER_BYTES ?
		    FSIMG_EMIT_BUFFER_BYTES : (size_t)(plan->upcase_length - offset);
		status = fsimg_emit_write(emit,
		    exfat_cluster_offset(g, upcase_first) + offset,
		    plan->upcase + offset, batch, error);
		if (status != FSIMG_OK)
			return (status);
	}
	bzero(header, sizeof(header));
	header[0] = 0x81;
	exfat_put32(header + 20, 2);
	fsimg_meta_put_u64(header + 24, plan->bitmap_length);
	header[32] = 0x82;
	exfat_put32(header + 36, plan->upcase_checksum);
	exfat_put32(header + 52, upcase_first);
	fsimg_meta_put_u64(header + 56, plan->upcase_length);
	header[64] = 0x83;
	status = fsimg_utf8_to_utf16(plan->common.writer->label,
	    strlen(plan->common.writer->label), label, 11, &label_count, error);
	if (status != FSIMG_OK)
		return (status);
	header[65] = (unsigned char)label_count;
	for (size_t i = 0; i < label_count; i++)
		exfat_put16(header + 66 + i * 2, label[i]);
	return (fsimg_emit_write(emit, exfat_cluster_offset(g, root.first),
	    header, sizeof(header), error));
}

/* An exFAT file occupies one contiguous cluster run; an empty file has none.
 * Bytes from the valid data length to the end of the allocation stay zero.
 */
static fsimg_status_t
emit_payload(exfat_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    const exfat_layout_t *row, fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	fsimg_emit_payload_t payload = {
		.length = row->length,
		.initialized = row->valid_length
	};

	if (row->clusters != 0) {
		region.image_offset = exfat_cluster_offset(&plan->geometry,
		    row->first);
		region.capacity = (uint64_t)row->clusters *
		    plan->geometry.cluster_bytes;
		payload.regions = &region;
		payload.region_count = 1;
	}
	return (fsimg_emit_copy(emit, id, &payload, error));
}

static fsimg_status_t
emit_child(exfat_plan_t *plan, fsimg_emit_t *emit, const char *name,
    const exfat_layout_t *row, const exfat_layout_t *parent, fsimg_error_t *error)
{
	unsigned char set[608];
	uint16_t units[255];
	size_t count, names, length;
	fsimg_status_t status;

	status = exfat_name_units(name, units, &count, error);
	if (status != FSIMG_OK)
		return (status);
	names = (count + 14) / 15;
	length = 64 + names * 32;
	if (row->parent_slot >= parent->length / 32 ||
	    length > parent->length - row->parent_slot * 32 ||
	    row->entry[1] != names + 1)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "exFAT child exceeds its planned directory slots"));
	bzero(set, length);
	bcopy(row->entry, set, 32);
	set[32] = 0xc0;
	set[33] = row->clusters == 0 ? 1 : 3;
	set[35] = (unsigned char)count;
	exfat_put16(set + 36, exfat_name_hash(units, count, NULL));
	fsimg_meta_put_u64(set + 40, row->valid_length);
	exfat_put32(set + 52, row->first);
	fsimg_meta_put_u64(set + 56, row->length);
	for (size_t i = 0; i < names; i++) {
		set[64 + i * 32] = 0xc1;
		for (size_t j = 0; j < 15 && i * 15 + j < count; j++)
			exfat_put16(set + 66 + i * 32 + j * 2, units[i * 15 + j]);
	}
	exfat_put16(set + 2, exfat_set_checksum(set, length));
	return (fsimg_emit_write(emit,
	    exfat_cluster_offset(&plan->geometry, parent->first) + row->parent_slot * 32,
	    set, length, error));
}

static fsimg_status_t
emit_catalog(exfat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	exfat_layout_t row, parent;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = exfat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		status = exfat_layout_get(plan, catalog->parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		status = emit_child(plan, emit, strrchr(catalog->entry.path, '/') + 1,
		    &row, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		if (catalog->entry.kind == FSIMG_ENTRY_FILE) {
			status = emit_payload(plan, emit, id, &row, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

/* The sink is already zero: emit the main and backup boot regions, the FAT
 * chains and system files, then each entry set with its file's payload.
 */
fsimg_status_t
exfat_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit, fsimg_error_t *error)
{
	exfat_plan_t *plan = (exfat_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_boot(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_metadata(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_catalog(plan, emit, error));
}
