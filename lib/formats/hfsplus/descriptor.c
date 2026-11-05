/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "format.h"
#include "fsimg-endian.h"
#include "write-hfsplus.h"

#define HFSPLUS_LABEL_BYTES		510u	/* 255 UTF-16 units */
#define HFSPLUS_BLOCK_MIN		512u

/* An allocation block of 512 bytes to 2^31 bytes, every power of two. */
#define HFSPLUS_BLOCK_SIZES		UINT32_C(0xfffffe00)
/* N * B with N of 2^32 - 1 blocks of 2^31 bytes. */
#define HFSPLUS_VOLUME_MAX		((UINT64_C(0xffffffff)) << 31)

_Static_assert(HFSPLUS_LINK_TARGET_MAX <= FSIMG_SYMLINK_TARGET_MAX,
    "the HFS Plus link-target limit lies within the entry model's ceiling");
_Static_assert(sizeof(((fsimg_volume_info_t *)NULL)->label) >
    HFSPLUS_NAME_UTF8_MAX, "the volume information holds the longest label");

static fsimg_status_t hfsplus_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

/* The kinds the writer stores; the reader also delivers the device and socket
 * kinds a native volume can hold.
 */
#define HFSPLUS_WRITE_KINDS	(FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY |	\
	    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO)
#define HFSPLUS_READ_KINDS	(HFSPLUS_WRITE_KINDS |				\
	    FSIMG_ENTRY_CHAR_DEVICE | FSIMG_ENTRY_BLOCK_DEVICE |	\
	    FSIMG_ENTRY_SOCKET)
/* 512 to 65536 bytes, every power of two. */
#define HFSPLUS_WRITE_BLOCK_SIZES	UINT32_C(0x0001fe00)
_Static_assert(HFSPLUS_WRITE_BLOCK_MIN == 1u << 9 &&
    HFSPLUS_WRITE_BLOCK_MAX == 1u << 16,
    "the writer's capability record states its block size range");

#define HFSPLUS_VARIANT_INFO(variant_name, kinds, bytes_max)		\
	.name = (variant_name),						\
	.entry_kinds = (kinds),						\
	.metadata_fields = FSIMG_META_CREATED | FSIMG_META_MODIFIED |	\
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED |			\
	    FSIMG_META_OWNER | FSIMG_META_MODE,				\
	.sector_size_mask = 1u << 9,					\
	.allocation_unit_size_mask = HFSPLUS_BLOCK_SIZES,		\
	.max_file_bytes = (bytes_max),					\
	.max_volume_bytes = (bytes_max),				\
	.max_name_utf16_units = HFSPLUS_NAME_UNITS_MAX,			\
	.max_label_bytes = HFSPLUS_LABEL_BYTES,				\
	.max_link_target_bytes = HFSPLUS_LINK_TARGET_MAX

static const fsimg_format_variant_t hfsplus_variants[] = {
	[HFSPLUS_VARIANT_PLUS] = {
		.info = {
			HFSPLUS_VARIANT_INFO("hfsplus", HFSPLUS_READ_KINDS,
			    HFSPLUS_VOLUME_MAX),
			.profile = "HFS Plus"
		},
		.id = HFSPLUS_VARIANT_PLUS
	},
	[HFSPLUS_VARIANT_X] = {
		.info = {
			HFSPLUS_VARIANT_INFO("hfsx", HFSPLUS_READ_KINDS,
			    HFSPLUS_VOLUME_MAX),
			.profile = "HFSX case-sensitive"
		},
		.id = HFSPLUS_VARIANT_X
	}
};

const fsimg_format_t fsimg_hfsplus_format = {
	.variants = hfsplus_variants,
	.variant_count = sizeof(hfsplus_variants) / sizeof(hfsplus_variants[0]),
	.probe_bytes = HFSPLUS_PROBE_BYTES,
	.probe = hfsplus_probe,
	.reader = &fsimg_hfsplus_reader_ops
};

/* The registered descriptor reads and verifies. The registry lists a writer
 * only after the native qualification cycle has accepted its output, so the
 * writer has its own descriptor, reachable only by code that names it.
 */
#define HFSPLUS_WRITE_VARIANT_INFO(variant_name)			\
	HFSPLUS_VARIANT_INFO(variant_name, HFSPLUS_WRITE_KINDS,		\
	    HFSPLUS_WRITE_VOLUME_MAX),					\
	.write_allocation_unit_size_mask = HFSPLUS_WRITE_BLOCK_SIZES,	\
	.write_settings = FSIMG_SETTING_VOLUME_UUID

static const fsimg_format_variant_t hfsplus_write_variants[] = {
	[HFSPLUS_VARIANT_PLUS] = {
		.info = {
			HFSPLUS_WRITE_VARIANT_INFO("hfsplus"),
			.profile = "HFS Plus"
		},
		.id = HFSPLUS_VARIANT_PLUS
	},
	[HFSPLUS_VARIANT_X] = {
		.info = {
			HFSPLUS_WRITE_VARIANT_INFO("hfsx"),
			.profile = "HFSX case-sensitive"
		},
		.id = HFSPLUS_VARIANT_X
	}
};

const fsimg_format_t fsimg_hfsplus_write_format = {
	.variants = hfsplus_write_variants,
	.variant_count = sizeof(hfsplus_write_variants) /
	    sizeof(hfsplus_write_variants[0]),
	.probe_bytes = HFSPLUS_PROBE_BYTES,
	.probe = hfsplus_probe,
	.reader = &fsimg_hfsplus_reader_ops,
	.writer = &fsimg_hfsplus_writer_ops
};

/* The boot area is all zero or begins with the classic signature "LK", the
 * two forms native formatters write. No other registered format's volume
 * begins that way and also has a header signature at 1024.
 */
bool
hfsplus_boot_area_valid(const unsigned char *prefix)
{
	if (prefix[0] == 'L' && prefix[1] == 'K')
		return (true);
	for (size_t i = 0; i < HFSPLUS_BOOT_BYTES; i++) {
		if (prefix[i] != 0)
			return (false);
	}
	return (true);
}

/* The header's fields the probe and open both check. N * B is not compared
 * with the slice: a shorter slice is a truncated volume, not a corrupt one.
 */
fsimg_status_t
hfsplus_header_check(const hfsplus_header_t *header, fsimg_error_t *error)
{
	if (!(header->signature == HFSPLUS_SIGNATURE_PLUS &&
	    header->version == HFSPLUS_VERSION_PLUS) &&
	    !(header->signature == HFSPLUS_SIGNATURE_X &&
	    header->version == HFSPLUS_VERSION_X))
		return (hfsplus_unsupported(error,
		    "HFS Plus signature and version do not pair"));
	if (header->block_size < HFSPLUS_BLOCK_MIN ||
	    (header->block_size & (header->block_size - 1)) != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus block size is not a power of two from 512"));
	if (header->total_blocks == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus volume has no blocks"));
	return (FSIMG_OK);
}

/* The identity signature has two parts: at 1024 a volume header signature or
 * a wrapper's master directory block embedding "H+", and a boot area that is
 * zero or begins "LK". Once they match the probe claims the image. A header is
 * then checked for its pairing and geometry, a wrapper's block on its
 * signatures alone; open checks its fields.
 */
static fsimg_status_t
hfsplus_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	const unsigned char *block;
	hfsplus_header_t header;
	hfsplus_mdb_t mdb;
	uint16_t signature;

	(void)volume_bytes;
	*variant = &hfsplus_variants[HFSPLUS_VARIANT_PLUS];
	if (prefix == NULL || length < HFSPLUS_PROBE_BYTES ||
	    !hfsplus_boot_area_valid(prefix))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER,
		    "no HFS Plus identity signature"));
	block = prefix + HFSPLUS_BOOT_BYTES;
	signature = fsimg_load_be16(block);
	if (signature == HFSPLUS_SIGNATURE_WRAPPER &&
	    hfsplus_mdb_decode(block, HFSPLUS_HEADER_BYTES, &mdb) &&
	    mdb.embedded_signature == HFSPLUS_SIGNATURE_PLUS)
		return (FSIMG_OK);
	if ((signature != HFSPLUS_SIGNATURE_PLUS &&
	    signature != HFSPLUS_SIGNATURE_X) ||
	    !hfsplus_header_decode(block, HFSPLUS_HEADER_BYTES, &header))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER,
		    "no HFS Plus identity signature"));
	if (signature == HFSPLUS_SIGNATURE_X)
		*variant = &hfsplus_variants[HFSPLUS_VARIANT_X];
	return (hfsplus_header_check(&header, error));
}
