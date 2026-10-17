/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "source.h"
#include "unicode.h"
#include "write-exfat.h"

static fsimg_status_t write_bytes(exfat_plan_t *, fsimg_io_t *, uint64_t,
    const void *, size_t, fsimg_error_t *);
static unsigned size_shift(uint32_t);
static fsimg_status_t emit_boot(exfat_plan_t *, fsimg_io_t *, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t emit_chain(exfat_plan_t *, fsimg_io_t *, uint32_t,
    uint32_t, unsigned char *, fsimg_error_t *);
static fsimg_status_t emit_metadata(exfat_plan_t *, fsimg_io_t *, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t emit_payload(exfat_plan_t *, fsimg_io_t *, uint64_t,
    const exfat_layout_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t emit_child(exfat_plan_t *, fsimg_io_t *, const char *,
    const exfat_layout_t *, const exfat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_catalog(exfat_plan_t *, fsimg_io_t *, unsigned char *,
    fsimg_error_t *);

static fsimg_status_t
write_bytes(exfat_plan_t *plan, fsimg_io_t *output, uint64_t offset,
    const void *bytes, size_t length, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_write_session(output, &plan->common.writer->session,
	    offset, bytes, length, &transferred, error));
}

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

static fsimg_status_t
emit_boot(exfat_plan_t *plan, fsimg_io_t *output, unsigned char *buffer,
    fsimg_error_t *error)
{
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
	status = write_bytes(plan, output, 0, buffer, length, error);
	if (status != FSIMG_OK)
		return (status);
	return (write_bytes(plan, output, length, buffer, length, error));
}

static fsimg_status_t
emit_chain(exfat_plan_t *plan, fsimg_io_t *output, uint32_t first,
    uint32_t count, unsigned char *buffer, fsimg_error_t *error)
{
	uint32_t emitted, batch, cluster;
	fsimg_status_t status;

	emitted = 0;
	while (emitted < count) {
		batch = count - emitted;
		if (batch > EXFAT_BUFFER_BYTES / 4)
			batch = EXFAT_BUFFER_BYTES / 4;
		for (uint32_t i = 0; i < batch; i++) {
			cluster = first + emitted + i;
			exfat_put32(buffer + i * 4, emitted + i + 1 == count ?
			    UINT32_MAX : cluster + 1);
		}
		status = write_bytes(plan, output,
		    plan->geometry.fat_offset + (uint64_t)(first + emitted) * 4,
		    buffer, (size_t)batch * 4, error);
		if (status != FSIMG_OK)
			return (status);
		emitted += batch;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
emit_metadata(exfat_plan_t *plan, fsimg_io_t *output, unsigned char *buffer,
    fsimg_error_t *error)
{
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
	status = write_bytes(plan, output, g->fat_offset, buffer, 8, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, output, 2, plan->bitmap_clusters, buffer, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, output, upcase_first, plan->upcase_clusters, buffer, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_chain(plan, output, root.first, root.clusters, buffer, error);
	if (status != FSIMG_OK)
		return (status);

	/* Allocation is a deterministic prefix. The common zero-initialization
	 * already defines all free bits, unused FAT entries and cluster slack.
	 */
	remaining = ((uint64_t)plan->allocated + 7) / 8;
	offset = 0;
	while (remaining != 0) {
		batch = remaining > EXFAT_BUFFER_BYTES ? EXFAT_BUFFER_BYTES : (size_t)remaining;
		memset(buffer, 0xff, batch);
		if (remaining == batch && plan->allocated % 8 != 0)
			buffer[batch - 1] = (unsigned char)((1u << (plan->allocated % 8)) - 1);
		status = write_bytes(plan, output, exfat_cluster_offset(g, 2) + offset,
		    buffer, batch, error);
		if (status != FSIMG_OK)
			return (status);
		offset += batch;
		remaining -= batch;
	}
	for (offset = 0; offset < plan->upcase_length; offset += batch) {
		batch = plan->upcase_length - offset > EXFAT_BUFFER_BYTES ?
		    EXFAT_BUFFER_BYTES : (size_t)(plan->upcase_length - offset);
		status = write_bytes(plan, output,
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
	return (write_bytes(plan, output, exfat_cluster_offset(g, root.first),
	    header, sizeof(header), error));
}

static fsimg_status_t
emit_payload(exfat_plan_t *plan, fsimg_io_t *output, uint64_t id,
    const exfat_layout_t *row, unsigned char *buffer, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	fsimg_range_t range;
	uint64_t offset, end;
	size_t batch, transferred;
	fsimg_status_t status;

	status = fsimg_source_open(writer, id, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->source.length != row->length)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source length differs from exFAT plan"));
	offset = 0;
	while (offset < row->valid_length) {
		status = fsimg_source_range(writer, offset, &range, error);
		if (status != FSIMG_OK)
			return (status);
		end = offset + range.length;
		if (end > row->valid_length)
			end = row->valid_length;
		if (range.kind == FSIMG_RANGE_ZERO) {
			offset = end;
			continue;
		}
		while (offset < end) {
			batch = end - offset > EXFAT_BUFFER_BYTES ?
			    EXFAT_BUFFER_BYTES : (size_t)(end - offset);
			status = fsimg_source_read(writer, offset, buffer, batch, &transferred, error);
			if (status != FSIMG_OK)
				return (status);
			status = write_bytes(plan, output,
			    exfat_cluster_offset(&plan->geometry, row->first) + offset,
			    buffer, batch, error);
			if (status != FSIMG_OK)
				return (status);
			offset += batch;
		}
	}
	return (fsimg_source_finish(writer, error));
}

static fsimg_status_t
emit_child(exfat_plan_t *plan, fsimg_io_t *output, const char *name,
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
	return (write_bytes(plan, output,
	    exfat_cluster_offset(&plan->geometry, parent->first) + row->parent_slot * 32,
	    set, length, error));
}

static fsimg_status_t
emit_catalog(exfat_plan_t *plan, fsimg_io_t *output, unsigned char *buffer,
    fsimg_error_t *error)
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
		status = emit_child(plan, output, strrchr(catalog->entry.path, '/') + 1,
		    &row, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		if (catalog->entry.kind == FSIMG_ENTRY_FILE) {
			status = emit_payload(plan, output, id, &row, buffer, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_write_emit(fsimg_writer_t *writer, fsimg_io_t *output, fsimg_error_t *error)
{
	exfat_plan_t *plan = (exfat_plan_t *)writer->plan;
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	buffer = fsimg_alloc(writer->ctx, EXFAT_BUFFER_BYTES, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_io_zero_session(output, &writer->session, 0,
	    plan->geometry.volume_bytes, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_boot(plan, output, buffer, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_metadata(plan, output, buffer, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_catalog(plan, output, buffer, error);
done:
	fsimg_dealloc(buffer);
	return (status);
}
