/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"

/*
 * The APFS reader (specs/design-apfs-driver.md, "Open"): block 0, the
 * checkpoint, the space manager, the container object map, the selected
 * volume and its object map, then the volume information. Directories are
 * listed by directory.c and inode.c, and file content resolved by content.c.
 */
#define ROLE_CHECKPOINT	"checkpoint superblock"

static fsimg_status_t	apfs_open(fsimg_reader_t *,
			    const fsimg_format_variant_t *, fsimg_error_t *);
static void		apfs_close(fsimg_reader_t *);
static fsimg_status_t	block0_read(fsimg_reader_t *, unsigned char *,
			    apfs_obj_t *, apfs_nx_t *, fsimg_error_t *);
static void		detail_add(fsimg_volume_info_t *, const char *,
			    uint64_t);
static void		report_info(fsimg_reader_t *);

const fsimg_reader_ops_t fsimg_apfs_reader_ops = {
	.open = apfs_open,
	.close = apfs_close,
	.prepare = apfs_prepare,
	.replay = apfs_replay,
	.keys = apfs_keys,
	.key = apfs_key,
	.ambiguous = "APFS directory holds names the volume treats as equal",
	.name_profile = FSIMG_NAME_PROFILE_NONE,
	.resolve = apfs_resolve,
	.verify = apfs_verify
};

/*
 * Read block 0 into window, which holds APFS_BLOCK_MAX bytes: its first
 * 4096 bytes, the identity check on the magic alone, then the rest of the
 * block its valid block size names and the validation the probe makes.
 * Every byte comes through the read funnel, so a slice shorter than the
 * block fails as truncated.
 */
static fsimg_status_t
block0_read(fsimg_reader_t *reader, unsigned char *window, apfs_obj_t *header,
    apfs_nx_t *nx, fsimg_error_t *error)
{
	size_t transferred;
	uint32_t block_size;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader, 0, window, APFS_BLOCK_MIN,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_reader_identity(reader,
	    fsimg_load_le32(window + APFS_OBJ_HEADER_BYTES) == APFS_NX_MAGIC,
	    "no APFS container superblock magic",
	    "NXSB magic at offset 32 of block 0", error);
	if (status != FSIMG_OK)
		return (status);
	block_size = fsimg_load_le32(window + APFS_OBJ_HEADER_BYTES + 4);
	if (apfs_block_size_valid(block_size) && block_size > APFS_BLOCK_MIN) {
		status = fsimg_reader_bytes(reader, APFS_BLOCK_MIN,
		    window + APFS_BLOCK_MIN, block_size - APFS_BLOCK_MIN,
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
	} else
		block_size = APFS_BLOCK_MIN;
	return (apfs_block0_check(window, block_size, header, nx, error));
}

static fsimg_status_t
apfs_open(fsimg_reader_t *reader, const fsimg_format_variant_t *forced
    FSIMG_UNUSED, fsimg_error_t *error)
{
	apfs_reader_t *apfs;
	unsigned char *window;
	apfs_obj_t header;
	apfs_nx_t nx;
	fsimg_error_t detail;
	fsimg_status_t status;

	apfs = fsimg_alloc(reader->ctx, sizeof(*apfs), &detail);
	if (apfs == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(apfs, sizeof(*apfs));
	/* The reader core closes the driver after a failed open, and close
	 * accepts a zeroed or partly opened state.
	 */
	reader->driver = apfs;
	window = fsimg_alloc(reader->ctx, APFS_BLOCK_MAX, &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}

	status = block0_read(reader, window, &header, &nx, error);
	if (status == FSIMG_OK)
		status = apfs_checkpoint_open(reader, &header, &nx,
		    &apfs->container, error);
	if (status == FSIMG_OK)
		status = apfs_nx_features(&apfs->container.nx, ROLE_CHECKPOINT,
		    error);
	if (status == FSIMG_OK)
		status = apfs_volume_open(reader, apfs, window, error);
	fsimg_dealloc(window);
	if (status != FSIMG_OK)
		return (status);

	reader->content_domain = (fsimg_extent_domain_t){
		.offset = apfs->container.block_size,
		.length = (apfs->container.block_count - 1) *
		    apfs->container.block_size,
		.unit_bytes = apfs->container.block_size
	};
	report_info(reader);
	return (FSIMG_OK);
}

static void
apfs_close(fsimg_reader_t *reader)
{
	apfs_reader_t *apfs = reader->driver;

	if (apfs == NULL)
		return;
	apfs_compress_free(apfs);
	apfs_volume_close(apfs);
	fsimg_dealloc(apfs);
	reader->driver = NULL;
}

static void
detail_add(fsimg_volume_info_t *info, const char *name, uint64_t value)
{
	fsimg_volume_detail_t *detail;

	if (info->detail_count >= FSIMG_VOLUME_DETAILS_MAX)
		return;
	detail = &info->details[info->detail_count++];
	snprintf(detail->name, sizeof(detail->name), "%s", name);
	detail->value = value;
}

/*
 * The volume information and its fourteen details, in the format design's
 * order ("Information and capabilities"). The container's length is
 * N * B of the selected checkpoint, whatever the slice holds, so the reader
 * core derives the missing bytes of a truncated container. The label is
 * the validated volume name, the only image text reported. A sealed volume
 * is refused while opening, so sealed is zero.
 */
static void
report_info(fsimg_reader_t *reader)
{
	const apfs_reader_t *apfs = reader->driver;
	const apfs_container_t *container = &apfs->container;
	const apfs_fs_t *fs = &apfs->fs;
	fsimg_volume_info_t *info = &reader->info;

	strcpy(info->format, "apfs");
	strcpy(info->profile, APFS_PROFILE);
	info->valid |= FSIMG_VOLUME_GEOMETRY | FSIMG_VOLUME_LABEL |
	    FSIMG_VOLUME_UUID | FSIMG_VOLUME_FREE;
	bcopy(fs->name, info->label, apfs->label_bytes);
	info->label[apfs->label_bytes] = '\0';
	bcopy(fs->uuid, info->uuid, APFS_UUID_BYTES);
	info->volume_bytes = container->block_count * container->block_size;
	info->allocation_units = container->block_count;
	info->allocation_unit_bytes = container->block_size;
	info->sector_bytes = container->block_size;
	info->free_bytes = apfs->free_blocks * container->block_size;
	info->time_basis = FSIMG_TIME_UTC;
	info->scope = FSIMG_VALIDATION_HEADER;
	info->clean = container->clean;
	info->io_error = false;

	detail_add(info, "volume_index", apfs->volume_index);
	detail_add(info, "volume_count", apfs->volume_count);
	detail_add(info, "role", fs->role);
	detail_add(info, "case_insensitive", (fs->incompat_features &
	    APFS_FS_INCOMPAT_CASE_INSENSITIVE) != 0);
	detail_add(info, "norm_insensitive", (fs->incompat_features &
	    APFS_FS_INCOMPAT_NORM_INSENSITIVE) != 0);
	detail_add(info, "checkpoint_xid", container->xid);
	detail_add(info, "read_xid", apfs->read_xid);
	detail_add(info, "files", fs->files);
	detail_add(info, "directories", fs->directories);
	detail_add(info, "symlinks", fs->symlinks);
	detail_add(info, "other_objects", fs->other_objects);
	detail_add(info, "snapshots", fs->snapshots);
	detail_add(info, "volume_blocks", fs->alloc_blocks);
	detail_add(info, "sealed", 0);
}
