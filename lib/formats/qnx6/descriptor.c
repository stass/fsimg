/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "format.h"
#include "write-qnx6.h"

_Static_assert(QNX6_LINK_TARGET_MAX <= FSIMG_SYMLINK_TARGET_MAX,
    "the QNX6 link-target limit lies within the entry model's ceiling");

static fsimg_status_t qnx6_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

/* The classic profile, read and written at every classic block size. Files
 * are bounded by the 32-bit block references, a slice by nothing below the
 * host's file size.
 */
static const fsimg_format_variant_t qnx6_variants[] = {
	{
		.info = {
			.name = "qnx6",
			.profile = "classic",
			.entry_kinds = FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY |
			    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO,
			.metadata_fields = FSIMG_META_CREATED |
			    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
			    FSIMG_META_CHANGED | FSIMG_META_OWNER |
			    FSIMG_META_MODE,
			.sector_size_mask = 1u << 9,
			.allocation_unit_size_mask = (1u << 9) | (1u << 10) |
			    (1u << 11) | (1u << 12),
			.write_allocation_unit_size_mask = (1u << 9) |
			    (1u << 10) | (1u << 11) | (1u << 12),
			.max_file_bytes = (uint64_t)(QNX6_SENTINEL - 1) * 4096,
			.max_volume_bytes = INT64_MAX,
			/* Names are bounded in bytes; a UTF-8 name has no more
			 * UTF-16 units than bytes, so the byte limit bounds it.
			 */
			.max_name_utf16_units = QNX6_NAME_MAX,
			.max_label_bytes = 0,
			.max_link_target_bytes = QNX6_LINK_TARGET_MAX,
			.write_settings = FSIMG_SETTING_BYTE_ORDER |
			    FSIMG_SETTING_VOLUME_UUID |
			    FSIMG_SETTING_INODE_CAPACITY | FSIMG_SETTING_RESERVE
		},
		.id = 0		/* The classic profile is the only variant. */
	}
};

const fsimg_format_t fsimg_qnx6_format = {
	.variants = qnx6_variants,
	.variant_count = sizeof(qnx6_variants) / sizeof(qnx6_variants[0]),
	.probe_bytes = QNX6_PROBE_BYTES,
	.probe = qnx6_probe,
	.reader = &fsimg_qnx6_reader_ops,
	.writer = &fsimg_qnx6_writer_ops
};

/* The identity signature is the magic at byte 8192, in either byte order.
 * The probe then checks the header CRC, version 4 and a classic block size.
 * A magic only at byte zero is a bootless or MMI layout, and magics at both
 * positions are ambiguous; both are recognized but unsupported. open
 * validates both headers completely.
 */
static fsimg_status_t
qnx6_probe(const unsigned char *prefix, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED, const fsimg_format_variant_t **variant,
    fsimg_error_t *error)
{
	const unsigned char *header;
	uint32_t block_size;
	bool big, at_zero, at_header;

	*variant = &qnx6_variants[0];
	at_zero = length >= 4 && qnx6_magic_order(prefix, &big);
	at_header = length >= QNX6_PROBE_BYTES &&
	    qnx6_magic_order(prefix + QNX6_HEADER_OFFSET, &big);
	if (!at_zero && !at_header)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no QNX6 superblock magic"));
	if (at_zero)
		return (qnx6_unsupported(error, at_header ?
		    "QNX6 magic at byte 0 and 8192 is ambiguous" :
		    "QNX6 bootless or MMI layout is not supported"));
	header = prefix + QNX6_HEADER_OFFSET;
	if (qnx6_crc32(header + 8, QNX6_HEADER_BYTES - 8) !=
	    qnx6_u32(header + 4, big))
		return (qnx6_corrupt(error, "QNX6 header checksum mismatch"));
	block_size = qnx6_u32(header + 48, big);
	if (qnx6_u16(header + 28, big) != QNX6_VERSION ||
	    (block_size != 512 && block_size != 1024 && block_size != 2048 &&
	    block_size != 4096))
		return (qnx6_unsupported(error,
		    "QNX6 header is outside the classic profile"));
	return (FSIMG_OK);
}
