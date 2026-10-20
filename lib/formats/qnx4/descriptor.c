/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "format.h"
#include "write-qnx4.h"

static fsimg_status_t qnx4_probe(const unsigned char *, size_t, uint64_t,
    fsimg_error_t *);

/* The native profile: 512-byte blocks, names of up to 505 bytes and a
 * 48-byte label. write_units and settings are what the writer stores, zero
 * in the registered descriptor, which has no writer.
 */
#define QNX4_PROFILE(write_units, settings) {				\
	.name = "qnx4",							\
	.profile = "long names",					\
	.entry_kinds = FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY |	\
	    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO,			\
	.metadata_fields = FSIMG_META_CREATED | FSIMG_META_MODIFIED |	\
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED |			\
	    FSIMG_META_OWNER | FSIMG_META_MODE,				\
	.sector_size_mask = 1u << 9,					\
	.allocation_unit_size_mask = 1u << 9,				\
	.write_allocation_unit_size_mask = (write_units),		\
	.max_file_bytes = QNX4_FILE_MAX,				\
	.max_volume_bytes = QNX4_BLOCKS_MAX * QNX4_BLOCK_BYTES,		\
	/* A UTF-8 name has no more UTF-16 units than bytes. */		\
	.max_name_utf16_units = QNX4_NAME_MAX,				\
	.max_label_bytes = QNX4_LABEL_BYTES,				\
	.write_settings = (settings)					\
}

/* The registered descriptor reads and verifies. The registry lists a
 * writer only after native QNX has checked, mounted and modified its
 * output, so the writer has its own candidate descriptor below.
 */
static const fsimg_format_variant_t qnx4_variants[] = {
	{
		.info = QNX4_PROFILE(0, 0),
		.id = 0		/* The native profile is the only variant. */
	}
};

const fsimg_format_t fsimg_qnx4_format = {
	.variants = qnx4_variants,
	.variant_count = sizeof(qnx4_variants) / sizeof(qnx4_variants[0]),
	.probe_bytes = QNX4_PROBE_BYTES,
	.probe = qnx4_probe,
	.reader = &fsimg_qnx4_reader_ops,
	.writer = NULL
};

/* The same profile with the writer, outside the registry (write-qnx4.h). */
static const fsimg_format_variant_t qnx4_candidate_variants[] = {
	{
		.info = QNX4_PROFILE(1u << 9, FSIMG_SETTING_INODE_CAPACITY),
		.id = 0
	}
};

const fsimg_format_t fsimg_qnx4_candidate_format = {
	.variants = qnx4_candidate_variants,
	.variant_count = sizeof(qnx4_candidate_variants) /
	    sizeof(qnx4_candidate_variants[0]),
	.probe_bytes = QNX4_PROBE_BYTES,
	.probe = qnx4_probe,
	.reader = &fsimg_qnx4_reader_ops,
	.writer = &fsimg_qnx4_writer_ops
};

/* QNX4 has no magic number. The identity signature is root-block record 0,
 * the root directory, at byte 512: the name "/" and fifteen NUL bytes, a
 * status of 0x01 or 0x21 and a directory mode. No FAT, exFAT or QNX6
 * volume carries that at byte 512: a FAT volume has its reserved sectors
 * or tables there, exFAT its boot region and QNX6 its boot area.
 *
 * Once the signature matches, the probe claims the image. The root
 * directory's first extent must lie inside the slice, and records 1 to 4
 * must be the four table records the formatter writes; open validates the
 * rest, including where the root directory must start.
 */
static fsimg_status_t
qnx4_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    fsimg_error_t *error)
{
	if (length < QNX4_PROBE_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no QNX4 root block"));
	return (qnx4_root_block_identity(prefix + QNX4_BLOCK_BYTES,
	    volume_bytes, error));
}

/* The root block's identity, shared by the probe and by open, which
 * checks the bytes it reads again: record 0 is "/" with a directory mode
 * and status 0x01 or 0x21, its first extent lies in the slice, and records
 * 1 to 4 have empty names and their table statuses.
 */
fsimg_status_t
qnx4_root_block_identity(const unsigned char *block, uint64_t volume_bytes,
    fsimg_error_t *error)
{
	static const unsigned char root_name[QNX4_DIRECT_NAME_MAX] = "/";
	qnx4_inode_t record;
	uint64_t last;

	qnx4_inode_decode(block, &record);
	if (bcmp(block, root_name, sizeof(root_name)) != 0 ||
	    (record.status != QNX4_STATUS_USED &&
	    record.status != (QNX4_STATUS_USED | QNX4_STATUS_CLEAN)) ||
	    (record.mode & QNX4_MODE_TYPE) != QNX4_MODE_DIRECTORY)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no QNX4 root directory record"));

	if (record.first_extent.first == 0 || record.first_extent.blocks == 0)
		return (qnx4_corrupt(error, "QNX4 root directory has no extent"));
	last = (uint64_t)record.first_extent.first +
	    record.first_extent.blocks - 1;
	if (last > volume_bytes / QNX4_BLOCK_BYTES)
		return (qnx4_corrupt(error,
		    "QNX4 root directory lies outside the volume"));
	for (unsigned i = QNX4_RECORD_INODES; i < QNX4_ROOT_RECORDS; i++) {
		const unsigned char *raw = block + i * QNX4_RECORD_BYTES;
		bool writable = i == QNX4_RECORD_BOOT ||
		    i == QNX4_RECORD_ALTBOOT;
		uint8_t status = raw[0x3f];

		if (!qnx4_bytes_zero(raw, QNX4_DIRECT_NAME_MAX) ||
		    (status != (QNX4_STATUS_TABLE | QNX4_STATUS_USED) &&
		    (!writable || status != (QNX4_STATUS_TABLE |
		    QNX4_STATUS_USED | QNX4_STATUS_MODIFIED))))
			return (qnx4_unsupported(error,
			    "QNX4 root block has unexpected table records"));
	}
	return (FSIMG_OK);
}
