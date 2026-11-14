/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "apfs.h"
#include "format.h"
#include "fsimg-endian.h"

#define ROLE_BLOCK0		"container superblock"
/* The probe reads the smallest block; see apfs_probe(). */
#define APFS_PROBE_BYTES	APFS_BLOCK_MIN
/* Blocks of 4096 to 65536 bytes, every power of two. APFS addresses whole
 * blocks, so a block is also the sector.
 */
#define APFS_BLOCK_SIZES	((1u << 12) | (1u << 13) | (1u << 14) |	\
	    (1u << 15) | (1u << 16))
#define APFS_LABEL_MAX		(APFS_FS_NAME_BYTES - 1)
#define APFS_LINK_TARGET_MAX	1023u

/* The incompatible features a container may carry: version 2 alone. */
#define NX_INCOMPAT_KNOWN	APFS_NX_INCOMPAT_VERSION2

_Static_assert(APFS_LINK_TARGET_MAX <= FSIMG_SYMLINK_TARGET_MAX,
    "the APFS link-target limit lies within the entry model's ceiling");
_Static_assert(sizeof(((fsimg_volume_info_t *)NULL)->label) > APFS_LABEL_MAX,
    "the volume information holds the longest volume name");
_Static_assert(sizeof(((fsimg_volume_info_t *)NULL)->profile) >
    sizeof(APFS_PROFILE), "the volume information holds the profile");

static fsimg_status_t apfs_probe(const unsigned char *, size_t, uint64_t,
    const fsimg_format_variant_t **, fsimg_error_t *);

/*
 * The one public name. The reader reports every POSIX kind, devices and
 * sockets as read-only kinds, with owner, mode and all four timestamps.
 * A volume holds N blocks of B bytes with N * B checked to fit 64 bits, and a
 * file lies inside its container. Names and the label are bounded as the
 * format design's capability record states; volume_index selects the
 * volume of a container holding several.
 */
static const fsimg_format_variant_t apfs_variants[] = {
	{
		.info = {
			.name = "apfs",
			.profile = APFS_PROFILE,
			.entry_kinds = FSIMG_ENTRY_FILE |
			    FSIMG_ENTRY_DIRECTORY | FSIMG_ENTRY_SYMLINK |
			    FSIMG_ENTRY_FIFO | FSIMG_ENTRY_CHAR_DEVICE |
			    FSIMG_ENTRY_BLOCK_DEVICE | FSIMG_ENTRY_SOCKET,
			.metadata_fields = FSIMG_META_CREATED |
			    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
			    FSIMG_META_CHANGED | FSIMG_META_OWNER |
			    FSIMG_META_MODE,
			.sector_size_mask = APFS_BLOCK_SIZES,
			.allocation_unit_size_mask = APFS_BLOCK_SIZES,
			.max_file_bytes = UINT64_MAX,
			.max_volume_bytes = UINT64_MAX,
			.max_name_utf16_units = APFS_WRITE_NAME_MAX_UNITS,
			.max_label_bytes = APFS_LABEL_MAX,
			.max_link_target_bytes = APFS_LINK_TARGET_MAX,
			.read_settings = FSIMG_SETTING_VOLUME_INDEX
		},
		.id = 0		/* The only variant. */
	}
};

const fsimg_format_t fsimg_apfs_format = {
	.variants = apfs_variants,
	.variant_count = sizeof(apfs_variants) / sizeof(apfs_variants[0]),
	.probe_bytes = APFS_PROBE_BYTES,
	.probe = apfs_probe,
	.reader = &fsimg_apfs_reader_ops
};

/*
 * The container feature table (specs/design-apfs.md, "Container
 * superblock"): version 2 is required, and any other incompatible bit,
 * version 1 and Fusion among them, is unsupported. Low-capacity Fusion mode
 * is meaningful only with the Fusion bit, so without it the field is
 * damaged. Read-only-compatible and other optional bits are accepted. The
 * flags are a closed table too: software cryptography alone is accepted.
 */
fsimg_status_t
apfs_nx_features(const apfs_nx_t *nx, const char *role, fsimg_error_t *error)
{
	if ((nx->incompat_features & APFS_NX_INCOMPAT_FUSION) != 0)
		return (apfs_unsupported(error, role, "is a Fusion container"));
	if ((nx->incompat_features & ~(uint64_t)NX_INCOMPAT_KNOWN) != 0)
		return (apfs_unsupported(error, role,
		    "has an unsupported incompatible feature"));
	if ((nx->incompat_features & APFS_NX_INCOMPAT_VERSION2) == 0)
		return (apfs_unsupported(error, role, "is not APFS version 2"));
	if ((nx->features & APFS_NX_FEATURE_LCFD) != 0)
		return (apfs_corrupt(error, role,
		    "sets low-capacity Fusion mode without Fusion"));
	if ((nx->flags & ~(uint64_t)APFS_NX_FLAG_CRYPTO_SW) != 0)
		return (apfs_unsupported(error, role,
		    "has an unsupported flag"));
	return (FSIMG_OK);
}

/*
 * Block 0 after its magic, in the probe's order. raw holds the first length
 * bytes of block 0 and volume_bytes is the volume's length. The checks are
 * the decoded header and superblock, a valid block size B within the
 * volume, the header of the container superblock (kind 1, OID 1, a nonzero
 * XID, ephemeral, no subtype; the encrypted and nonpersistent flags are
 * unsupported), the checksum over B bytes when raw holds them, the geometry
 * and the features. The block size is checked before it bounds anything.
 */
static fsimg_status_t
block0_check(const unsigned char *raw, size_t length, uint64_t volume_bytes,
    apfs_obj_t *header, apfs_nx_t *nx, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (!apfs_obj_decode(raw, length, header) ||
	    !apfs_nx_decode(raw, length, nx))
		return (apfs_corrupt(error, ROLE_BLOCK0,
		    "is shorter than one block"));
	if (!apfs_block_size_valid(nx->block_size))
		return (apfs_corrupt(error, ROLE_BLOCK0,
		    "has an invalid block size"));
	if (volume_bytes < nx->block_size)
		return (apfs_corrupt(error, ROLE_BLOCK0,
		    "is shorter than one block"));
	if ((header->flags & (APFS_OBJ_ENCRYPTED | APFS_OBJ_NONPERSISTENT)) != 0)
		return (apfs_unsupported(error, ROLE_BLOCK0,
		    "is encrypted or nonpersistent"));
	if (header->kind != APFS_KIND_NX_SUPERBLOCK || header->oid != 1 ||
	    header->xid == 0 || header->subtype != APFS_SUBTYPE_NONE ||
	    header->flags != APFS_OBJ_EPHEMERAL)
		return (apfs_corrupt(error, ROLE_BLOCK0,
		    "has an invalid object header"));
	if (nx->block_size <= length && !apfs_object_verify(raw, nx->block_size))
		return (apfs_corrupt(error, ROLE_BLOCK0, "has a bad checksum"));
	status = apfs_nx_geometry(nx, ROLE_BLOCK0, error);
	if (status != FSIMG_OK)
		return (status);
	return (apfs_nx_features(nx, ROLE_BLOCK0, error));
}

/*
 * Open's check of the whole block it read. The bytes held are the volume
 * as far as this check can tell, so a block longer than length fails as
 * shorter than one block and the checksum always runs.
 */
fsimg_status_t
apfs_block0_check(const unsigned char *raw, size_t length, apfs_obj_t *header,
    apfs_nx_t *nx, fsimg_error_t *error)
{
	return (block0_check(raw, length, length, header, nx, error));
}

/*
 * The identity check is the NXSB magic at offset 32 of block 0 and nothing
 * else; without it the probe makes no claim. With it, the probe claims the
 * image and judges block 0 with the function open uses. The prefix is the
 * first 4096 bytes, the smallest block, cut to the volume's length, so the
 * shared detection read does not grow to the largest block for every image.
 * The prefix holds the whole block only when B is 4096; a larger block's
 * checksum is verified by open, which reads the whole block.
 */
static fsimg_status_t
apfs_probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	apfs_obj_t header;
	apfs_nx_t nx;

	*variant = &apfs_variants[0];
	if (prefix == NULL || length < APFS_OBJ_HEADER_BYTES + 4 ||
	    fsimg_load_le32(prefix + APFS_OBJ_HEADER_BYTES) != APFS_NX_MAGIC)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no APFS container superblock magic"));
	return (block0_check(prefix, length, volume_bytes, &header, &nx, error));
}
