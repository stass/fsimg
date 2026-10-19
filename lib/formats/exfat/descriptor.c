/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "exfat.h"
#include "format.h"

#define EXFAT_SIGNATURE_BYTES	64

static fsimg_status_t exfat_probe(const unsigned char *, size_t, uint64_t,
    fsimg_error_t *);

static const fsimg_format_variant_t exfat_variants[] = {
	{
		.info = {
			.name = "exfat",
			.profile = "embedded-upcase/unicode-17-writer",
			.entry_kinds = FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY,
			.metadata_fields = FSIMG_META_CREATED | FSIMG_META_MODIFIED |
			    FSIMG_META_ACCESSED | FSIMG_META_ATTRIBUTES,
			.sector_size_mask = (1u << 9) | (1u << 10) | (1u << 11) |
			    (1u << 12),
			.allocation_unit_size_mask = ((1u << 26) - 1) & ~((1u << 9) - 1),
			.write_allocation_unit_size_mask = ((1u << 26) - 1) &
			    ~((1u << 9) - 1),
			.max_file_bytes = (uint64_t)EXFAT_CLUSTER_MAX * 33554432,
			.max_volume_bytes = INT64_MAX,
			.max_name_utf16_units = 255,
			/* Eleven UTF-16 units in the volume-label entry. */
			.max_label_bytes = 22
		},
		.id = 0		/* exFAT has a single variant. */
	}
};

const fsimg_format_t fsimg_exfat_format = {
	.variants = exfat_variants,
	.variant_count = sizeof(exfat_variants) / sizeof(exfat_variants[0]),
	.probe_bytes = EXFAT_SIGNATURE_BYTES,
	.probe = exfat_probe,
	.reader = &fsimg_exfat_reader_ops,
	.writer = &fsimg_exfat_writer_ops
};

/* The identity signature is the jump, the "EXFAT" name and the zeroed legacy
 * BPB through byte 63. OEM text alone is insufficient, since a FAT formatter
 * may use the same name. A signed volume below the exFAT minimum is a corrupt
 * claim. Every other boot field, including the revision and the boot
 * checksum, is validated when the volume is opened.
 */
static fsimg_status_t
exfat_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    fsimg_error_t *error)
{
	static const unsigned char legacy_zero[EXFAT_SIGNATURE_BYTES - 11] = { 0 };

	if (length < EXFAT_SIGNATURE_BYTES ||
	    bcmp(prefix, "\xeb\x76\x90" "EXFAT   ", 11) != 0 ||
	    bcmp(prefix + 11, legacy_zero, sizeof(legacy_zero)) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no exFAT boot signature"));
	if (volume_bytes < EXFAT_VOLUME_MIN)
		return (exfat_corrupt(error, "input is smaller than an exFAT volume"));
	return (FSIMG_OK);
}
