/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "format.h"
#include "ifs.h"

_Static_assert(IFS_LINK_TARGET_MAX <= FSIMG_SYMLINK_TARGET_MAX,
    "the ImageFS link-target limit lies within the entry model's ceiling");

static fsimg_status_t ifs_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

/* The one variant reads either byte order. Addressing is by byte, the
 * largest image is the largest multiple of 4 that fits 32 bits, and names
 * and link targets reach the 1024-byte path limit. There is no label.
 */
static const fsimg_format_variant_t ifs_variants[] = {
	{
		.info = {
			.name = "ifs",
			.profile = "little-endian/big-endian",
			.entry_kinds = FSIMG_ENTRY_FILE |
			    FSIMG_ENTRY_DIRECTORY | FSIMG_ENTRY_SYMLINK |
			    FSIMG_ENTRY_FIFO,
			.metadata_fields = FSIMG_META_MODIFIED |
			    FSIMG_META_OWNER | FSIMG_META_MODE,
			.sector_size_mask = 1u << 0,
			.allocation_unit_size_mask = 1u << 0,
			.max_file_bytes = IFS_FILE_MAX,
			.max_volume_bytes = IFS_VOLUME_MAX,
			.max_name_utf16_units = IFS_PATH_MAX,
			.max_label_bytes = 0,
			.max_link_target_bytes = IFS_LINK_TARGET_MAX
		},
		.id = 0
	}
};

const fsimg_format_t fsimg_ifs_format = {
	.variants = ifs_variants,
	.variant_count = sizeof(ifs_variants) / sizeof(ifs_variants[0]),
	.probe_bytes = IFS_HEADER_BYTES,
	.probe = ifs_probe,
	.reader = &fsimg_ifs_reader_ops,
	.writer = NULL
};

/* The identity signature is the seven bytes "imagefs" at offset 0; no FAT
 * or exFAT boot sector, QNX6 boot area or QNX4 loader block of any capture
 * begins with it. Once it matches, the probe claims the image: the flags
 * byte must select a native byte order, and the three sizes must satisfy
 * the header bounds against the volume. Open applies the remaining header
 * rules. A prefix shorter than the fixed header is no claim.
 */
static fsimg_status_t
ifs_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	ifs_header_t header;
	fsimg_status_t status;

	*variant = &ifs_variants[0];
	if (length < IFS_HEADER_BYTES ||
	    bcmp(prefix, IFS_SIGNATURE, IFS_SIGNATURE_BYTES) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no ImageFS signature"));
	status = ifs_header_decode(prefix, &header, error);
	if (status != FSIMG_OK)
		return (status);
	return (ifs_header_bounds(&header, volume_bytes, error));
}
