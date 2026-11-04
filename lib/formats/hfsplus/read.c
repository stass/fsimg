/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hfsplus.h"

/* The volume is unclean when its header says so: bit 8 clear, bit 11 or 14 set
 * or a failed journal replay's mark, or when a tree records a bad close.
 */
#define ATTRIBUTE_UNMOUNTED		(UINT32_C(1) << 8)
#define ATTRIBUTE_BOOT_INCONSISTENT	(UINT32_C(1) << 11)
#define ATTRIBUTE_INCONSISTENT		(UINT32_C(1) << 14)
#define LAST_MOUNTED_FAILED_REPLAY	UINT32_C(0x46534b21)	/* "FSK!" */

static fsimg_status_t hfsplus_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void hfsplus_close(fsimg_reader_t *);
static bool volume_clean(const fsimg_hfsplus_reader_t *);
static void detail_add(fsimg_volume_info_t *, const char *, uint64_t);
static void report_info(fsimg_reader_t *);

const fsimg_reader_ops_t fsimg_hfsplus_reader_ops = {
	.open = hfsplus_open,
	.close = hfsplus_close
};

fsimg_hfsplus_reader_t *
hfsplus_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
hfsplus_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

fsimg_status_t
hfsplus_unsupported(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_READER,
	    message));
}

static bool
volume_clean(const fsimg_hfsplus_reader_t *hfsplus)
{
	const hfsplus_header_t *header = &hfsplus->header;
	uint32_t closes;

	closes = hfsplus->extents.header.attributes |
	    hfsplus->catalog.header.attributes;
	if (hfsplus->attributes.present)
		closes |= hfsplus->attributes.header.attributes;
	return ((header->attributes & ATTRIBUTE_UNMOUNTED) != 0 &&
	    (header->attributes & (ATTRIBUTE_BOOT_INCONSISTENT |
	    ATTRIBUTE_INCONSISTENT)) == 0 &&
	    header->last_mounted_version != LAST_MOUNTED_FAILED_REPLAY &&
	    (closes & HFSPLUS_BTREE_BAD_CLOSE) == 0);
}

static void
detail_add(fsimg_volume_info_t *info, const char *name, uint64_t value)
{
	fsimg_volume_detail_t *detail = &info->details[info->detail_count++];

	snprintf(detail->name, sizeof(detail->name), "%s", name);
	detail->value = value;
}

/* The profile says the variant and its name order; whether the volume is
 * journaled is the journal detail's to say. The details' order is the driver
 * design's, and create_date stays the stored 1904-based local value.
 */
static void
report_info(fsimg_reader_t *reader)
{
	const fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_header_t *header = &hfsplus->header;
	fsimg_volume_info_t *info = &reader->info;
	bool variant_x = header->signature == HFSPLUS_SIGNATURE_X;

	strcpy(info->format, variant_x ? "hfsx" : "hfsplus");
	if (!variant_x)
		strcpy(info->profile, "HFS Plus");
	else if (hfsplus->catalog.compare == HFSPLUS_COMPARE_BINARY)
		strcpy(info->profile, "HFSX case-sensitive");
	else
		strcpy(info->profile, "HFSX case-insensitive");
	info->valid |= FSIMG_VOLUME_GEOMETRY;
	if (hfsplus->label_valid) {
		info->valid |= FSIMG_VOLUME_LABEL;
		bcopy(hfsplus->label, info->label, hfsplus->label_bytes);
	}
	info->volume_bytes = hfsplus->geometry.volume_bytes;
	info->sector_bytes = 512;
	info->allocation_unit_bytes = hfsplus->geometry.block_size;
	info->allocation_units = hfsplus->geometry.blocks;
	info->time_basis = FSIMG_TIME_UTC;
	info->scope = FSIMG_VALIDATION_HEADER;
	info->clean = volume_clean(hfsplus);
	info->io_error = false;

	detail_add(info, "journal", hfsplus->journal);
	detail_add(info, "catalog_node_bytes",
    hfsplus->catalog.header.node_size);
	detail_add(info, "extents_node_bytes",
    hfsplus->extents.header.node_size);
	detail_add(info, "attributes_node_bytes", hfsplus->attributes.present ?
	    hfsplus->attributes.header.node_size : 0);
	detail_add(info, "files", header->file_count);
	detail_add(info, "folders", header->folder_count);
	detail_add(info, "next_catalog_id", header->next_catalog_id);
	detail_add(info, "volume_id",
	    (uint64_t)header->finder_info[6] << 32 | header->finder_info[7]);
	detail_add(info, "last_mounted", header->last_mounted_version);
	detail_add(info, "create_date", header->create_date);
}

static fsimg_status_t
hfsplus_open(fsimg_reader_t *reader, const fsimg_format_variant_t *forced,
    fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus;
	fsimg_error_t detail;
	fsimg_status_t status;

	hfsplus = fsimg_alloc(reader->ctx, sizeof(*hfsplus), &detail);
	if (hfsplus == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(hfsplus, sizeof(*hfsplus));
	reader->driver = hfsplus;
	status = hfsplus_volume_open(reader, forced, error);
	if (status != FSIMG_OK)
		return (status);
	report_info(reader);
	return (FSIMG_OK);
}

static void
hfsplus_close(fsimg_reader_t *reader)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);

	if (hfsplus == NULL)
		return;
	hfsplus_tree_free(&hfsplus->extents);
	hfsplus_tree_free(&hfsplus->catalog);
	hfsplus_tree_free(&hfsplus->attributes);
	fsimg_dealloc(hfsplus->catalog_scan_node);
	fsimg_extent_index_free(hfsplus->bitmap);
	fsimg_dealloc(hfsplus);
	reader->driver = NULL;
}
