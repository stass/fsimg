/*-
 * Copyright (c) 2024-2026 Stanislav Sedov <stas@deglitch.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <string.h>
#include <strings.h>

#include "io.h"
#include "source.h"
#include "unicode.h"
#include "write.h"

/* Boot fields, FSInfo and long-name slot encoding follow the author's
 * libarchive writer. Planning, allocation streaming and direct source copying
 * are independent implementations; no payload staging or whole FAT is used.
 */
static fsimg_status_t write_bytes(fsimg_plan_t *, fsimg_io_t *, uint64_t,
    const void *, size_t, fsimg_error_t *);
static uint64_t layout_offset(const fsimg_plan_t *, const fat_layout_t *);
static fsimg_status_t emit_boot(fsimg_plan_t *, fsimg_io_t *, fsimg_error_t *);
static fsimg_status_t emit_fat(fsimg_plan_t *, fsimg_io_t *, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t emit_directory_header(fsimg_plan_t *, fsimg_io_t *,
    uint64_t, const fat_layout_t *, const fat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_child(fsimg_plan_t *, fsimg_io_t *, const char *,
    const fat_layout_t *, const fat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_entries(fsimg_plan_t *, fsimg_io_t *,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t emit_payload(fsimg_plan_t *, fsimg_io_t *, uint64_t,
    const fat_layout_t *, unsigned char *, fsimg_error_t *);

static fsimg_status_t
write_bytes(fsimg_plan_t *plan, fsimg_io_t *output, uint64_t offset,
    const void *buffer, size_t length, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_write_session(output, &plan->writer->session,
	    offset, buffer, length, &transferred, error));
}

static uint64_t
layout_offset(const fsimg_plan_t *plan, const fat_layout_t *row)
{
	/* Only the fixed FAT12/16 root has contents at first == 0. Empty files
	 * also have no cluster, but perform no payload write and never use this
	 * address. All non-root directories own at least one cluster.
	 */
	if (row->first == 0)
		return (plan->root_offset);
	return (plan->data_offset +
	    (uint64_t)(row->first - 2) * plan->info.allocation_unit_bytes);
}

static fsimg_status_t
emit_boot(fsimg_plan_t *plan, fsimg_io_t *output, fsimg_error_t *error)
{
	unsigned char boot[512], info[512];
	uint32_t sector, total, fat_sectors;
	unsigned extension;
	fsimg_status_t status;

	bzero(boot, sizeof(boot));
	sector = plan->info.sector_bytes;
	total = (uint32_t)(plan->info.volume_bytes / sector);
	fat_sectors = (uint32_t)(plan->fat_bytes / sector);
	boot[0] = 0xeb;
	boot[1] = plan->variant == 32 ? 0x58 : 0x3c;
	boot[2] = 0x90;
	bcopy("FSIMG   ", boot + 3, 8);
	fsimg_fat_put16(boot + 11, (uint16_t)sector);
	boot[13] = (unsigned char)(plan->info.allocation_unit_bytes / sector);
	fsimg_fat_put16(boot + 14, (uint16_t)plan->reserved_sectors);
	boot[16] = 2;
	fsimg_fat_put16(boot + 17, (uint16_t)plan->root_entries);
	if (plan->variant != 32 && total <= UINT16_MAX)
		fsimg_fat_put16(boot + 19, (uint16_t)total);
	else
		fsimg_fat_put32(boot + 32, total);
	boot[21] = 0xf8;
	fsimg_fat_put16(boot + 24, 63);
	fsimg_fat_put16(boot + 26, 255);
	extension = plan->variant == 32 ? 64 : 36;
	if (plan->variant == 32) {
		fsimg_fat_put32(boot + 36, fat_sectors);
		fsimg_fat_put32(boot + 44, 2);
		fsimg_fat_put16(boot + 48, 1);
		fsimg_fat_put16(boot + 50, 6);
	} else
		fsimg_fat_put16(boot + 22, (uint16_t)fat_sectors);
	boot[extension] = 0x80;
	boot[extension + 2] = 0x29;
	fsimg_fat_put32(boot + extension + 3, plan->writer->options.volume_serial);
	memset(boot + extension + 7, ' ', 11);
	bcopy(plan->writer->label, boot + extension + 7, strlen(plan->writer->label));
	bcopy(plan->variant == 12 ? "FAT12   " :
	    plan->variant == 16 ? "FAT16   " : "FAT32   ", boot + extension + 18, 8);
	/* An infinite jump is a defined non-bootable stub, rather than executing
	 * uninitialized data if firmware tries to boot a data-only volume.
	 */
	boot[extension + 26] = 0xeb;
	boot[extension + 27] = 0xfe;
	boot[510] = 0x55;
	boot[511] = 0xaa;
	status = write_bytes(plan, output, 0, boot, sizeof(boot), error);
	if (status != FSIMG_OK || plan->variant != 32)
		return (status);
	status = write_bytes(plan, output, (uint64_t)sector * 6, boot,
	    sizeof(boot), error);
	if (status != FSIMG_OK)
		return (status);

	bzero(info, sizeof(info));
	fsimg_fat_put32(info, 0x41615252);
	fsimg_fat_put32(info + 484, 0x61417272);
	fsimg_fat_put32(info + 488, plan->clusters - plan->allocated);
	fsimg_fat_put32(info + 492, plan->allocated < plan->clusters ?
	    plan->allocated + 2 : UINT32_MAX);
	fsimg_fat_put32(info + 508, 0xaa550000);
	status = write_bytes(plan, output, sector, info, sizeof(info), error);
	if (status != FSIMG_OK)
		return (status);
	return (write_bytes(plan, output, (uint64_t)sector * 7, info,
	    sizeof(info), error));
}

/* Allocations are contiguous and ordered by catalog ID. A single cursor over
 * plan rows supplies chain successors. The bit accumulator packs FAT12 across
 * transfer boundaries as well as the byte-aligned FAT16/FAT32 cases.
 * Free entries and table padding remain zero from output initialization.
 */
static fsimg_status_t
emit_fat(fsimg_plan_t *plan, fsimg_io_t *output, unsigned char *buffer,
    fsimg_error_t *error)
{
	fat_layout_t row;
	uint64_t row_id, written, accumulator;
	uint32_t value, eoc;
	size_t used;
	unsigned bits;
	fsimg_status_t status;

	bzero(&row, sizeof(row));
	row_id = written = accumulator = 0;
	used = bits = 0;
	eoc = plan->variant == 12 ? 0xfff :
	    plan->variant == 16 ? 0xffff : 0x0fffffff;
	for (uint32_t cluster = 0; cluster < plan->allocated + 2; cluster++) {
		if (cluster < 2)
			value = cluster == 0 ? eoc & ~7u : eoc;
		else {
			while (row.clusters == 0 || cluster >= row.first + row.clusters) {
				status = fsimg_fat_layout_get(plan, row_id, &row, error);
				if (status != FSIMG_OK)
					return (status);
				row_id++;
			}
			value = cluster + 1 == row.first + row.clusters ? eoc : cluster + 1;
		}
		accumulator |= (uint64_t)value << bits;
		bits += plan->variant;
		while (bits >= 8) {
			buffer[used++] = (unsigned char)accumulator;
			accumulator >>= 8;
			bits -= 8;
			if (used != FAT_WRITE_BUFFER)
				continue;
			status = write_bytes(plan, output, plan->fat_offset + written,
			    buffer, used, error);
			if (status != FSIMG_OK)
				return (status);
			status = write_bytes(plan, output,
			    plan->fat_offset + plan->fat_bytes + written, buffer, used, error);
			if (status != FSIMG_OK)
				return (status);
			written += used;
			used = 0;
		}
	}
	if (bits != 0)
		buffer[used++] = (unsigned char)accumulator;
	status = write_bytes(plan, output, plan->fat_offset + written, buffer,
	    used, error);
	if (status != FSIMG_OK)
		return (status);
	return (write_bytes(plan, output, plan->fat_offset + plan->fat_bytes + written,
	    buffer, used, error));
}

static fsimg_status_t
emit_directory_header(fsimg_plan_t *plan, fsimg_io_t *output, uint64_t id,
    const fat_layout_t *row, const fat_layout_t *parent, fsimg_error_t *error)
{
	unsigned char raw[64];
	uint32_t parent_cluster;

	bzero(raw, sizeof(raw));
	if (id == 0) {
		memset(raw, ' ', 11);
		bcopy(plan->writer->label, raw, strlen(plan->writer->label));
		raw[11] = 8;
		return (write_bytes(plan, output, layout_offset(plan, row), raw, 32, error));
	}
	bcopy(row->entry, raw, 32);
	bcopy(row->entry, raw + 32, 32);
	memset(raw, ' ', 11);
	memset(raw + 32, ' ', 11);
	raw[0] = '.';
	raw[32] = raw[33] = '.';
	raw[11] = raw[43] = 0x10;
	parent_cluster = parent->first;
	/* The FAT32 root always starts at cluster two; '..' still encodes zero
	 * when it refers to the root, just as on FAT12/16.
	 */
	if (parent->first == 2 && plan->variant == 32)
		parent_cluster = 0;
	fsimg_fat_put16(raw + 32 + 26, (uint16_t)parent_cluster);
	fsimg_fat_put16(raw + 32 + 20, (uint16_t)(parent_cluster >> 16));
	return (write_bytes(plan, output, layout_offset(plan, row), raw,
	    sizeof(raw), error));
}

static fsimg_status_t
emit_child(fsimg_plan_t *plan, fsimg_io_t *output, const char *name,
    const fat_layout_t *row, const fat_layout_t *parent, fsimg_error_t *error)
{
	static const unsigned positions[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
	    24, 28, 30 };
	uint16_t units[255], value;
	unsigned char raw[21 * 32], short_name[11], *slot, checksum;
	size_t count, long_slots, index;
	fsimg_status_t status;

	long_slots = 0;
	bzero(raw, sizeof(raw));
	if (!fsimg_fat_write_short_name(name, short_name)) {
		status = fsimg_utf8_to_utf16(name, strlen(name), units, 255, &count, error);
		if (status != FSIMG_OK)
			return (status);
		long_slots = (count + 12) / 13;
		checksum = 0;
		for (unsigned i = 0; i < 11; i++)
			checksum = (unsigned char)((checksum >> 1) +
			    ((checksum & 1) << 7) + row->entry[i]);
		for (size_t ordinal = long_slots; ordinal != 0; ordinal--) {
			slot = raw + (long_slots - ordinal) * 32;
			slot[0] = (unsigned char)(ordinal | (ordinal == long_slots ? 0x40 : 0));
			slot[11] = 0x0f;
			slot[13] = checksum;
			for (unsigned i = 0; i < 13; i++) {
				index = (ordinal - 1) * 13 + i;
				value = index < count ? units[index] :
				    index == count ? 0 : 0xffff;
				fsimg_fat_put16(slot + positions[i], value);
			}
		}
	}
	bcopy(row->entry, raw + long_slots * 32, 32);
	return (write_bytes(plan, output, layout_offset(plan, parent) +
	    row->parent_slot * 32, raw, (long_slots + 1) * 32, error));
}

static fsimg_status_t
emit_payload(fsimg_plan_t *plan, fsimg_io_t *output, uint64_t id,
    const fat_layout_t *row, unsigned char *buffer, fsimg_error_t *error)
{
	fsimg_range_t range;
	uint64_t offset, remaining;
	size_t batch, transferred;
	fsimg_status_t status;

	status = fsimg_source_open(plan->writer, id, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->writer->source.length != row->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned payload length differs from its source"));
	offset = 0;
	while (offset < row->length) {
		status = fsimg_source_range(plan->writer, offset, &range, error);
		if (status != FSIMG_OK)
			return (status);
		if (range.kind == FSIMG_RANGE_ZERO) {
			/* The complete destination was initialized before data emission. */
			offset += range.length;
			continue;
		}
		remaining = range.length;
		while (remaining != 0) {
			batch = remaining > FAT_WRITE_BUFFER ? FAT_WRITE_BUFFER :
			    (size_t)remaining;
			status = fsimg_source_read(plan->writer, offset, buffer, batch,
			    &transferred, error);
			if (status != FSIMG_OK)
				return (status);
			if (transferred != batch)
				return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
				    FSIMG_OP_WRITER, "source ended before its planned range"));
			status = write_bytes(plan, output, layout_offset(plan, row) + offset,
			    buffer, batch, error);
			if (status != FSIMG_OK)
				return (status);
			offset += batch;
			remaining -= batch;
		}
	}
	return (fsimg_source_finish(plan->writer, error));
}

static fsimg_status_t
emit_entries(fsimg_plan_t *plan, fsimg_io_t *output, unsigned char *buffer,
    fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	fat_layout_t row, parent;
	const char *name;
	fsimg_status_t status;

	status = fsimg_fat_layout_get(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_directory_header(plan, output, 0, &row, &row, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= plan->writer->count; id++) {
		status = fsimg_catalog_get(plan->writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_get(plan, catalog->parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		name = strrchr(catalog->entry.path, '/') + 1;
		status = emit_child(plan, output, name, &row, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.slots != 0)
			status = emit_directory_header(plan, output, id, &row, &parent, error);
		else
			status = emit_payload(plan, output, id, &row, buffer, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_write_emit(fsimg_writer_t *writer, fsimg_io_t *output,
    fsimg_error_t *error)
{
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	buffer = fsimg_alloc(writer->ctx, FAT_WRITE_BUFFER, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_io_zero_session(output, &writer->session, 0,
	    writer->plan->info.volume_bytes, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_boot(writer->plan, output, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_fat(writer->plan, output, buffer, error);
	if (status != FSIMG_OK)
		goto done;
	status = emit_entries(writer->plan, output, buffer, error);
done:
	fsimg_dealloc(buffer);
	return (status);
}
