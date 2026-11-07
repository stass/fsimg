/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "format.h"
#include "write-erofs.h"

_Static_assert(EROFS_SYMLINK_MAX <= FSIMG_SYMLINK_TARGET_MAX,
    "the EROFS link-target limit lies within the entry model's ceiling");

/* Blocks of 512 to 4096 bytes, every power of two. */
#define EROFS_BLOCK_SIZES	((1u << 9) | (1u << 10) | (1u << 11) |	\
	    (1u << 12))
/*
 * A 32-bit block count of the largest block. Flat content lies inside the
 * volume, so it bounds a file as well.
 */
#define EROFS_VOLUME_MAX	(UINT64_C(0xffffffff) << EROFS_BLKSZBITS_MAX)

static fsimg_status_t erofs_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

/*
 * The one public name, read and written. The reader reports every POSIX
 * kind, devices and sockets as read-only kinds, and the one time an inode
 * stores: a modification time under the volume's MTIME feature, a change
 * time without it. The writer stores files, directories, symbolic links
 * and FIFOs with the modification time, owner and mode, and always sets
 * MTIME, so the record lists the modification time and not the change
 * time, and the writer reports a source change time as an omitted field.
 * Names are bounded in bytes; a UTF-8 name has no more UTF-16 units than
 * bytes, so the byte limit bounds it. The sector and label values are the
 * writer's: the format has no sector of its own, so the writer works in
 * 512-byte sectors, and the writer keeps the name field's terminator,
 * although the reader returns a full 16-byte name. The identifier is the
 * UUID.
 */
static const fsimg_format_variant_t erofs_variants[] = {
	{
		.info = {
			.name = "erofs",
			.profile = "core",
			.entry_kinds = FSIMG_ENTRY_FILE |
			    FSIMG_ENTRY_DIRECTORY | FSIMG_ENTRY_SYMLINK |
			    FSIMG_ENTRY_FIFO | FSIMG_ENTRY_CHAR_DEVICE |
			    FSIMG_ENTRY_BLOCK_DEVICE | FSIMG_ENTRY_SOCKET,
			.metadata_fields = FSIMG_META_MODIFIED |
			    FSIMG_META_OWNER | FSIMG_META_MODE,
			.sector_size_mask = 1u << 9,
			.allocation_unit_size_mask = EROFS_BLOCK_SIZES,
			.write_allocation_unit_size_mask =
			    EROFS_WRITE_BLOCK_SIZES,
			.max_file_bytes = EROFS_VOLUME_MAX,
			.max_volume_bytes = EROFS_VOLUME_MAX,
			.max_name_utf16_units = EROFS_NAME_MAX,
			.max_label_bytes = EROFS_WRITE_LABEL_MAX,
			.max_link_target_bytes = EROFS_SYMLINK_MAX,
			.write_settings = FSIMG_SETTING_VOLUME_UUID |
			    FSIMG_SETTING_EROFS
		},
		.id = 0		/* The only variant. */
	}
};

const fsimg_format_t fsimg_erofs_format = {
	.variants = erofs_variants,
	.variant_count = sizeof(erofs_variants) / sizeof(erofs_variants[0]),
	.probe_bytes = EROFS_PROBE_BYTES,
	.probe = erofs_probe,
	.reader = &fsimg_erofs_reader_ops,
	.writer = &fsimg_erofs_writer_ops
};

/*
 * The identity signature is the magic at byte 1024; a prefix too short to
 * hold it is not EROFS. Once it matches, the probe claims the volume and
 * applies the base superblock's checks alone: block size, directory block
 * size, extension slots and the incompatible-feature table. open repeats
 * them and validates everything else.
 */
static fsimg_status_t
erofs_probe(const unsigned char *prefix, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	const unsigned char *super;
	const char *what;
	fsimg_status_t status;

	*variant = &erofs_variants[0];
	if (length <= EROFS_SUPER_OFFSET)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no EROFS superblock magic"));
	super = prefix + EROFS_SUPER_OFFSET;
	if (!erofs_magic_matches(super, length - EROFS_SUPER_OFFSET))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no EROFS superblock magic"));

	status = erofs_super_probe(super, length - EROFS_SUPER_OFFSET, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	return (FSIMG_OK);
}
