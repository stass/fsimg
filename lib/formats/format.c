/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "fat.h"
#include "format.h"
#include "write.h"

static const fsimg_reader_ops_t fat_reader = {
	.open = fsimg_fat_open,
	.close = fsimg_fat_close,
	.next = fsimg_fat_next,
	.match_name = fsimg_fat_match_name,
	.skip_directory = fsimg_fat_skip_directory,
	.read = fsimg_fat_read,
	.range = fsimg_fat_range,
	.verify = fsimg_fat_verify
};

static const fsimg_writer_ops_t fat_writer = {
	.validate = fsimg_fat_write_validate,
	.plan = fsimg_fat_write_plan,
	.emit = fsimg_fat_write_emit,
	.free = fsimg_fat_write_free
};

static const fsimg_format_t formats[] = {
	{ .name = "fat12", .reader = &fat_reader, .writer = &fat_writer, .variant = 12 },
	{ .name = "fat16", .reader = &fat_reader, .writer = &fat_writer, .variant = 16 },
	{ .name = "fat32", .reader = &fat_reader, .writer = &fat_writer, .variant = 32 },
	{ .name = "exfat", .reader = &fsimg_exfat_reader_ops,
	    .writer = &fsimg_exfat_writer_ops }
};

const fsimg_format_t *
fsimg_format_find(const char *name)
{
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) == 0)
			return (&formats[i]);
	}
	return (NULL);
}

fsimg_status_t
fsimg_format_open(fsimg_reader_t *reader, const char *forced,
    fsimg_error_t *error)
{
	const fsimg_format_t *format;
	static const unsigned char exfat_legacy_zero[53] = { 0 };
	unsigned char prefix[64];
	uint64_t length;
	size_t transferred;
	fsimg_status_t status;

	format = forced == NULL ? NULL : fsimg_format_find(forced);
	if (forced != NULL && (format == NULL || format->reader == NULL))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported reader format"));
	status = fsimg_io_get_size(reader->input, &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length < 512)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "volume is smaller than a boot record"));
	if (forced == NULL) {
		status = fsimg_reader_bytes(reader, 0, prefix, sizeof(prefix),
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		/* FAT OEM text is arbitrary. exFAT identity also needs its jump and
		 * zero legacy BPB; FAT selects its actual variant from geometry.
		 */
		format = fsimg_format_find(
		    bcmp(prefix, "\xeb\x76\x90" "EXFAT   ", 11) == 0 &&
		    bcmp(prefix + 11, exfat_legacy_zero, sizeof(exfat_legacy_zero)) == 0 ?
		    "exfat" : "fat12");
	}
	if (format == NULL || format->reader == NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported reader format"));
	reader->ops = format->reader;
	return (reader->ops->open(reader, forced, error));
}

fsimg_status_t
fsimg_format_next(size_t *cursor, fsimg_format_info_t *out, fsimg_error_t *error)
{
	const fsimg_format_t *format;
	uint64_t sectors;
	unsigned variant;

	fsimg_error_clear(error);
	if (cursor == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing format cursor or output"));
	bzero(out, sizeof(*out));
	if (*cursor >= sizeof(formats) / sizeof(formats[0]))
		return (FSIMG_DONE);
	format = &formats[*cursor];
	variant = format->variant;
	strcpy(out->name, format->name);
	if (format->reader != NULL) {
		out->capabilities |= FSIMG_CAP_READ;
		if (format->reader->verify != NULL)
			out->capabilities |= FSIMG_CAP_VERIFY;
	}
	if (format->writer != NULL)
		out->capabilities |= FSIMG_CAP_WRITE;
	if (variant == 0) {
		strcpy(out->profile, "embedded-upcase/unicode-17-writer");
		out->entry_kinds = (1u << FSIMG_ENTRY_FILE) |
		    (1u << FSIMG_ENTRY_DIRECTORY);
		out->metadata_fields = 0x0f;
		out->sector_size_mask = (1u << 9) | (1u << 10) |
		    (1u << 11) | (1u << 12);
		out->allocation_unit_size_mask = ((1u << 26) - 1) & ~((1u << 9) - 1);
		out->write_allocation_unit_size_mask = out->allocation_unit_size_mask;
		out->max_file_bytes = (uint64_t)EXFAT_CLUSTER_MAX * 33554432;
		out->max_volume_bytes = INT64_MAX;
		out->max_name_utf16_units = 255;
		(*cursor)++;
		return (FSIMG_OK);
	}
	strcpy(out->profile, "unicode-17-simple-upper/cp437");
	out->entry_kinds = (1u << FSIMG_ENTRY_FILE) | (1u << FSIMG_ENTRY_DIRECTORY);
	out->metadata_fields = 0x0f;
	out->sector_size_mask = (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12);
	out->allocation_unit_size_mask = ((1u << 20) - 1) & ~((1u << 9) - 1);
	out->write_allocation_unit_size_mask = ((1u << 16) - 1) & ~((1u << 9) - 1);
	/* File size is bounded by both its field and usable cluster identifiers.
	 * Volume size additionally includes reserved/FAT/root regions and a tail
	 * shorter than one cluster. These are independent supported upper bounds.
	 */
	out->max_file_bytes = variant == 12 ? UINT64_C(4084) * 512 * 1024 : UINT32_MAX;
	if (variant != 32) {
		sectors = UINT64_C(65535) * 3 + 512 +
		    (variant == 12 ? UINT64_C(4085) : UINT64_C(65525)) * 128 - 1;
		out->max_volume_bytes = sectors * 4096;
	} else {
		out->max_volume_bytes = (uint64_t)UINT32_MAX * 4096;
	}
	out->max_name_utf16_units = 255;
	(*cursor)++;
	return (FSIMG_OK);
}
