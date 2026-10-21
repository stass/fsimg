/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "fat.h"
#include "format.h"
#include "write.h"

#define FAT_SECTOR_MASK		((1u << 9) | (1u << 10) | (1u << 11) | (1u << 12))
#define FAT_ENTRY_KINDS		(FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY)
#define FAT_METADATA_FIELDS	(FSIMG_META_CREATED | FSIMG_META_MODIFIED |	\
	FSIMG_META_ACCESSED | FSIMG_META_ATTRIBUTES)
/* The volume-label directory entry and boot-sector field hold 11 bytes. */
#define FAT_LABEL_BYTES		11

/* Largest FAT12/FAT16 volume: maximal reserved and two maximal FAT regions,
 * a 512-sector root directory, the largest cluster count of the variant with
 * 128-sector clusters, and a tail one sector shorter than a cluster.
 */
#define FAT_SMALL_VOLUME_BYTES(clusters)				\
	((UINT64_C(65535) * 3 + 512 + UINT64_C(clusters) * 128 - 1) * 4096)

static fsimg_status_t fat_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

static const fsimg_reader_ops_t fat_reader = {
	.open = fsimg_fat_open,
	.close = fsimg_fat_close,
	.prepare = fsimg_fat_prepare,
	.replay = fsimg_fat_replay,
	.keys = fsimg_fat_keys,
	.key = fsimg_fat_key,
	.ambiguous = "ambiguous FAT long-name or alias namespace",
	.resolve = fsimg_fat_resolve,
	.verify = fsimg_fat_verify
};

static const fsimg_writer_ops_t fat_writer = {
	.validate = fsimg_fat_write_validate,
	.plan = fsimg_fat_write_plan,
	.emit = fsimg_fat_write_emit,
	.free = fsimg_fat_write_free
};

/* File size is bounded by both its field and usable cluster identifiers.
 * Volume size additionally includes reserved/FAT/root regions and a tail
 * shorter than one cluster. These are independent supported upper bounds.
 * id is the variant's FAT entry width in bits.
 */
static const fsimg_format_variant_t fat_variants[] = {
	{
		.info = {
			.name = "fat12",
			.profile = "unicode-17-simple-upper/cp437",
			.entry_kinds = FAT_ENTRY_KINDS,
			.metadata_fields = FAT_METADATA_FIELDS,
			.sector_size_mask = FAT_SECTOR_MASK,
			.allocation_unit_size_mask = ((1u << 20) - 1) & ~((1u << 9) - 1),
			.write_allocation_unit_size_mask = ((1u << 16) - 1) & ~((1u << 9) - 1),
			.max_file_bytes = UINT64_C(4084) * 512 * 1024,
			.max_volume_bytes = FAT_SMALL_VOLUME_BYTES(4085),
			.max_name_utf16_units = 255,
			.max_label_bytes = FAT_LABEL_BYTES
		},
		.id = 12
	},
	{
		.info = {
			.name = "fat16",
			.profile = "unicode-17-simple-upper/cp437",
			.entry_kinds = FAT_ENTRY_KINDS,
			.metadata_fields = FAT_METADATA_FIELDS,
			.sector_size_mask = FAT_SECTOR_MASK,
			.allocation_unit_size_mask = ((1u << 20) - 1) & ~((1u << 9) - 1),
			.write_allocation_unit_size_mask = ((1u << 16) - 1) & ~((1u << 9) - 1),
			.max_file_bytes = UINT32_MAX,
			.max_volume_bytes = FAT_SMALL_VOLUME_BYTES(65525),
			.max_name_utf16_units = 255,
			.max_label_bytes = FAT_LABEL_BYTES
		},
		.id = 16
	},
	{
		.info = {
			.name = "fat32",
			.profile = "unicode-17-simple-upper/cp437",
			.entry_kinds = FAT_ENTRY_KINDS,
			.metadata_fields = FAT_METADATA_FIELDS,
			.sector_size_mask = FAT_SECTOR_MASK,
			.allocation_unit_size_mask = ((1u << 20) - 1) & ~((1u << 9) - 1),
			.write_allocation_unit_size_mask = ((1u << 16) - 1) & ~((1u << 9) - 1),
			.max_file_bytes = UINT32_MAX,
			.max_volume_bytes = (uint64_t)UINT32_MAX * 4096,
			.max_name_utf16_units = 255,
			.max_label_bytes = FAT_LABEL_BYTES
		},
		.id = 32
	}
};

const fsimg_format_t fsimg_fat_format = {
	.variants = fat_variants,
	.variant_count = sizeof(fat_variants) / sizeof(fat_variants[0]),
	.probe_bytes = 512,
	.probe = fat_probe,
	.reader = &fat_reader,
	.writer = &fat_writer
};

/* The variant record whose id is a decoded FAT entry width; geometry
 * decoding yields only 12, 16 or 32. The record is the one
 * fsimg_format_find() returns for the same name, so callers can compare
 * records by identity.
 */
const fsimg_format_variant_t *
fsimg_fat_variant(unsigned id)
{
	switch (id) {
	case 12:
		return (&fat_variants[0]);
	case 16:
		return (&fat_variants[1]);
	default:
		return (&fat_variants[2]);
	}
}

/* The boot record decides: its signature and legal BPB make the claim, and
 * the remaining geometry checks decide between a match, an unsupported table
 * count, FAT32 revision or field, and corruption. The variant follows from
 * the cluster count, as it does when the volume is opened.
 */
static fsimg_status_t
fat_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	fat_geometry_t geometry;
	fsimg_status_t status;

	if (length < 512)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "volume is smaller than a boot record"));
	status = fsimg_fat_geometry_decode(prefix, volume_bytes, &geometry, error);
	if (status == FSIMG_OK)
		*variant = fsimg_fat_variant(geometry.variant);
	return (status);
}
