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

#include "emit.h"
#include "fsimg-endian.h"
#include "unicode.h"
#include "write.h"

/* Boot fields, FSInfo and long-name slot encoding follow the author's
 * libarchive writer. Planning and allocation streaming are independent
 * implementations; no payload staging or whole FAT is used. Payloads are
 * copied by the common emission object.
 */
static uint64_t layout_offset(const fat_plan_t *, const fat_layout_t *);
static fsimg_status_t emit_boot(fat_plan_t *, fsimg_emit_t *, fsimg_error_t *);
static fsimg_status_t emit_fat(fat_plan_t *, fsimg_emit_t *, fsimg_error_t *);
static fsimg_status_t emit_directory_header(fat_plan_t *, fsimg_emit_t *,
    uint64_t, const fat_layout_t *, const fat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_child(fat_plan_t *, fsimg_emit_t *, const char *,
    const fat_layout_t *, const fat_layout_t *, fsimg_error_t *);
static fsimg_status_t emit_entries(fat_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_payload(fat_plan_t *, fsimg_emit_t *, uint64_t,
    const fat_layout_t *, fsimg_error_t *);

static uint64_t
layout_offset(const fat_plan_t *plan, const fat_layout_t *row)
{
	/* Only the fixed FAT12/16 root has contents at first == 0. Empty files
	 * also have no cluster, but perform no payload write and never use this
	 * address. All non-root directories own at least one cluster.
	 */
	if (row->first == 0)
		return (plan->root_offset);
	return (plan->data_offset +
	    (uint64_t)(row->first - 2) * plan->common.info.allocation_unit_bytes);
}

static fsimg_status_t
emit_boot(fat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char boot[512], info[512];
	uint32_t sector, total, fat_sectors;
	unsigned extension;
	fsimg_status_t status;

	bzero(boot, sizeof(boot));
	sector = plan->common.info.sector_bytes;
	total = (uint32_t)(plan->common.info.volume_bytes / sector);
	fat_sectors = (uint32_t)(plan->fat_bytes / sector);
	boot[0] = 0xeb;
	boot[1] = plan->variant == 32 ? 0x58 : 0x3c;
	boot[2] = 0x90;
	bcopy("FSIMG   ", boot + 3, 8);
	fsimg_store_le16(boot + 11, (uint16_t)sector);
	boot[13] = (unsigned char)(plan->common.info.allocation_unit_bytes / sector);
	fsimg_store_le16(boot + 14, (uint16_t)plan->reserved_sectors);
	boot[16] = 2;
	fsimg_store_le16(boot + 17, (uint16_t)plan->root_entries);
	if (plan->variant != 32 && total <= UINT16_MAX)
		fsimg_store_le16(boot + 19, (uint16_t)total);
	else
		fsimg_store_le32(boot + 32, total);
	boot[21] = 0xf8;
	fsimg_store_le16(boot + 24, 63);
	fsimg_store_le16(boot + 26, 255);
	extension = plan->variant == 32 ? 64 : 36;
	if (plan->variant == 32) {
		fsimg_store_le32(boot + 36, fat_sectors);
		fsimg_store_le32(boot + 44, 2);
		fsimg_store_le16(boot + 48, 1);
		fsimg_store_le16(boot + 50, 6);
	} else
		fsimg_store_le16(boot + 22, (uint16_t)fat_sectors);
	boot[extension] = 0x80;
	boot[extension + 2] = 0x29;
	fsimg_store_le32(boot + extension + 3, plan->common.writer->options.volume_serial);
	memset(boot + extension + 7, ' ', 11);
	bcopy(plan->common.writer->label, boot + extension + 7, strlen(plan->common.writer->label));
	bcopy(plan->variant == 12 ? "FAT12   " :
	    plan->variant == 16 ? "FAT16   " : "FAT32   ", boot + extension + 18, 8);
	/* An infinite jump is a defined non-bootable stub, rather than executing
	 * uninitialized data if firmware tries to boot a data-only volume.
	 */
	boot[extension + 26] = 0xeb;
	boot[extension + 27] = 0xfe;
	boot[510] = 0x55;
	boot[511] = 0xaa;
	status = fsimg_emit_write(emit, 0, boot, sizeof(boot), error);
	if (status != FSIMG_OK || plan->variant != 32)
		return (status);
	status = fsimg_emit_write(emit, (uint64_t)sector * 6, boot,
	    sizeof(boot), error);
	if (status != FSIMG_OK)
		return (status);

	bzero(info, sizeof(info));
	fsimg_store_le32(info, 0x41615252);
	fsimg_store_le32(info + 484, 0x61417272);
	fsimg_store_le32(info + 488, plan->clusters - plan->allocated);
	fsimg_store_le32(info + 492, plan->allocated < plan->clusters ?
	    plan->allocated + 2 : UINT32_MAX);
	fsimg_store_le32(info + 508, 0xaa550000);
	status = fsimg_emit_write(emit, sector, info, sizeof(info), error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_emit_write(emit, (uint64_t)sector * 7, info,
	    sizeof(info), error));
}

/* Allocations are contiguous and ordered by catalog ID. A single cursor over
 * plan rows supplies chain successors. The bit accumulator packs FAT12 across
 * transfer boundaries as well as the byte-aligned FAT16/FAT32 cases.
 * Free entries and table padding remain zero from output initialization.
 */
static fsimg_status_t
emit_fat(fat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
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
			if (used != FSIMG_EMIT_BUFFER_BYTES)
				continue;
			status = fsimg_emit_write(emit, plan->fat_offset + written,
			    buffer, used, error);
			if (status != FSIMG_OK)
				return (status);
			status = fsimg_emit_write(emit,
			    plan->fat_offset + plan->fat_bytes + written, buffer, used, error);
			if (status != FSIMG_OK)
				return (status);
			written += used;
			used = 0;
		}
	}
	if (bits != 0)
		buffer[used++] = (unsigned char)accumulator;
	status = fsimg_emit_write(emit, plan->fat_offset + written, buffer,
	    used, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_emit_write(emit, plan->fat_offset + plan->fat_bytes + written,
	    buffer, used, error));
}

static fsimg_status_t
emit_directory_header(fat_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    const fat_layout_t *row, const fat_layout_t *parent, fsimg_error_t *error)
{
	unsigned char raw[64];
	uint32_t parent_cluster;

	bzero(raw, sizeof(raw));
	if (id == 0) {
		memset(raw, ' ', 11);
		bcopy(plan->common.writer->label, raw, strlen(plan->common.writer->label));
		raw[11] = 8;
		return (fsimg_emit_write(emit, layout_offset(plan, row), raw, 32, error));
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
	fsimg_store_le16(raw + 32 + 26, (uint16_t)parent_cluster);
	fsimg_store_le16(raw + 32 + 20, (uint16_t)(parent_cluster >> 16));
	return (fsimg_emit_write(emit, layout_offset(plan, row), raw,
	    sizeof(raw), error));
}

static fsimg_status_t
emit_child(fat_plan_t *plan, fsimg_emit_t *emit, const char *name,
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
				fsimg_store_le16(slot + positions[i], value);
			}
		}
	}
	bcopy(row->entry, raw + long_slots * 32, 32);
	return (fsimg_emit_write(emit, layout_offset(plan, parent) +
	    row->parent_slot * 32, raw, (long_slots + 1) * 32, error));
}

/* A FAT file occupies one contiguous run of clusters; an empty file has none.
 * FAT records no separate initialized length, so the whole file is copied.
 */
static fsimg_status_t
emit_payload(fat_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    const fat_layout_t *row, fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	fsimg_emit_payload_t payload = {
		.length = row->length,
		.initialized = row->length
	};

	if (row->clusters != 0) {
		region.image_offset = layout_offset(plan, row);
		region.capacity = (uint64_t)row->clusters *
		    plan->common.info.allocation_unit_bytes;
		payload.regions = &region;
		payload.region_count = 1;
	}
	return (fsimg_emit_copy(emit, id, &payload, error));
}

static fsimg_status_t
emit_entries(fat_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	fat_layout_t row, parent;
	const char *name;
	fsimg_status_t status;

	status = fsimg_fat_layout_get(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_directory_header(plan, emit, 0, &row, &row, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= plan->common.writer->count; id++) {
		status = fsimg_catalog_get(plan->common.writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_get(plan, catalog->parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		name = strrchr(catalog->entry.path, '/') + 1;
		status = emit_child(plan, emit, name, &row, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.slots != 0)
			status = emit_directory_header(plan, emit, id, &row, &parent, error);
		else
			status = emit_payload(plan, emit, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* The sink is already zero: emit only the boot region, both FAT copies,
 * and directory contents with their files' payloads. Free clusters, unused
 * FAT entries and slack are never written.
 */
fsimg_status_t
fsimg_fat_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	fat_plan_t *plan = (fat_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_boot(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_fat(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_entries(plan, emit, error));
}
