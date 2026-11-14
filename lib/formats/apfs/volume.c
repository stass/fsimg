/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "apfs.h"
#include "unicode.h"

/*
 * Volume selection and the volume superblock (specs/design-apfs.md,
 * "Volume superblock" and "Volume selection"; specs/design-apfs-driver.md,
 * "Open", steps 3 to 6). Everything is read at the read XID, the selected
 * checkpoint's: snapshot selection is not offered, so the read options
 * never name one.
 */
#define ROLE_SPACEMAN	"space manager"
#define ROLE_VOLUME	"volume superblock"

/* The incompatible features a volume may carry. Incomplete restore and
 * sealing are known and unsupported, so they are outside the accepted set.
 */
#define FS_INCOMPAT_ACCEPTED	(APFS_FS_INCOMPAT_CASE_INSENSITIVE |	\
	    APFS_FS_INCOMPAT_DATALESS_SNAPSHOTS |			\
	    APFS_FS_INCOMPAT_ENCRYPTION_ROLLED |			\
	    APFS_FS_INCOMPAT_NORM_INSENSITIVE)
/* The volume flags the reader accepts; unencrypted is also required. */
#define FS_FLAGS_ACCEPTED	(APFS_FS_FLAG_UNENCRYPTED |		\
	    APFS_FS_FLAG_CHECK_EXTENT_REFS)

static fsimg_status_t	free_count_read(apfs_reader_t *, unsigned char *,
			    fsimg_error_t *);
static fsimg_status_t	fs_check(const apfs_fs_t *, uint32_t, size_t *,
			    fsimg_error_t *);
static fsimg_status_t	name_check(const unsigned char *, size_t *,
			    fsimg_error_t *);
static void		names_setup(apfs_reader_t *);
static fsimg_status_t	selection_invalid(fsimg_error_t *, const char *);
static fsimg_status_t	volume_choose(const apfs_nx_t *,
			    const fsimg_read_options_t *, uint32_t *, uint32_t *,
			    fsimg_error_t *);

static fsimg_status_t
selection_invalid(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
	    message));
}

/*
 * The space manager is read whole through its mapping, then its main
 * device's free count is taken. A count above the container's blocks is
 * damage, and bounding it keeps free_bytes from overflowing.
 */
static fsimg_status_t
free_count_read(apfs_reader_t *apfs, unsigned char *window,
    fsimg_error_t *error)
{
	apfs_container_t *container = &apfs->container;
	apfs_spaceman_t spaceman;
	apfs_cpm_t mapping;
	apfs_ref_t ref;
	fsimg_status_t status;
	bool found;

	status = apfs_ephemeral_find(container, container->nx.spaceman_oid,
	    &found, &mapping, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (apfs_corrupt(error, ROLE_SPACEMAN,
		    "is not in the checkpoint"));
	ref = (apfs_ref_t){ .role = ROLE_SPACEMAN,
	    .oid = container->nx.spaceman_oid, .xid = container->xid,
	    .blocks = mapping.size / container->block_size,
	    .storage = APFS_OBJ_EPHEMERAL, .subtype = APFS_SUBTYPE_NONE,
	    .kind = APFS_KIND_SPACEMAN };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_spaceman_decode(window, container->block_size, &spaceman) ||
	    spaceman.block_size != container->block_size)
		return (apfs_corrupt(error, ROLE_SPACEMAN,
		    "has an invalid header"));
	if (spaceman.devices[APFS_SM_DEVICE_MAIN].free_count >
	    container->block_count)
		return (apfs_corrupt(error, ROLE_SPACEMAN,
		    "counts more free blocks than the container holds"));
	apfs->free_blocks = spaceman.devices[APFS_SM_DEVICE_MAIN].free_count;
	return (FSIMG_OK);
}

/*
 * Choose the slot to open. volume_index names one, which must lie below the
 * maximum volume count and be occupied; without it, the container must
 * hold exactly one volume. Every refusal is the caller's selection,
 * FSIMG_EINVAL, and names slots by index alone: a volume's name is never
 * read before its superblock is validated.
 */
static fsimg_status_t
volume_choose(const apfs_nx_t *nx, const fsimg_read_options_t *options,
    uint32_t *index, uint32_t *count, fsimg_error_t *error)
{
	char message[160];
	uint32_t lowest = 0, highest = 0;

	*count = 0;
	for (uint32_t i = 0; i < nx->max_volumes; i++) {
		if (nx->volume_oids[i] == 0)
			continue;
		if (*count == 0)
			lowest = i;
		highest = i;
		(*count)++;
	}
	if (options->volume_index_set) {
		*index = options->volume_index;
		if (*index >= nx->max_volumes) {
			snprintf(message, sizeof(message), "APFS volume index "
			    "%u is not below the container's maximum of %u "
			    "volumes", *index, nx->max_volumes);
			return (selection_invalid(error, message));
		}
		if (nx->volume_oids[*index] == 0) {
			snprintf(message, sizeof(message),
			    "APFS volume slot %u is empty", *index);
			return (selection_invalid(error, message));
		}
		return (FSIMG_OK);
	}
	if (*count == 0)
		return (selection_invalid(error,
		    "APFS container holds no volume"));
	if (*count > 1) {
		snprintf(message, sizeof(message), "APFS container holds %u "
		    "volumes in slots %u to %u; select one with volume=",
		    *count, lowest, highest);
		return (selection_invalid(error, message));
	}
	*index = lowest;
	return (FSIMG_OK);
}

/*
 * A volume name is UTF-8 of at most 255 bytes, its terminator inside the
 * field. Its length without the terminator is stored on success.
 */
static fsimg_status_t
name_check(const unsigned char *name, size_t *length, fsimg_error_t *error)
{
	const unsigned char *end = memchr(name, '\0', APFS_FS_NAME_BYTES);
	size_t used, step;
	uint32_t point;

	if (end == NULL)
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "name has no terminator"));
	used = (size_t)(end - name);
	for (size_t at = 0; at < used; at += step) {
		if (!fsimg_utf8_decode(name + at, used - at, &step, &point))
			return (apfs_corrupt(error, ROLE_VOLUME,
			    "name is not valid UTF-8"));
	}
	*length = used;
	return (FSIMG_OK);
}

/*
 * The volume superblock's own rules: its magic, its slot, a nonzero UUID,
 * the name, then the refusal tables in the order a feature makes the rest
 * meaningless: incompatible features, flags, transitions, and last the tree
 * types, which a sealed volume, refused above, would state differently.
 */
static fsimg_status_t
fs_check(const apfs_fs_t *fs, uint32_t index, size_t *name_bytes,
    fsimg_error_t *error)
{
	static const unsigned char zero[APFS_UUID_BYTES];
	fsimg_status_t status;

	if (fs->magic != APFS_FS_MAGIC)
		return (apfs_corrupt(error, ROLE_VOLUME, "has no APSB magic"));
	if (fs->index != index)
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "names another slot of the volume array"));
	if (memcmp(fs->uuid, zero, sizeof(zero)) == 0)
		return (apfs_corrupt(error, ROLE_VOLUME, "has no UUID"));
	status = name_check(fs->name, name_bytes, error);
	if (status != FSIMG_OK)
		return (status);

	if ((fs->incompat_features & APFS_FS_INCOMPAT_SEALED) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME, "is sealed"));
	if ((fs->incompat_features & APFS_FS_INCOMPAT_INCOMPLETE_RESTORE) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has an incomplete restore"));
	if ((fs->incompat_features & ~(uint64_t)FS_INCOMPAT_ACCEPTED) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has an unsupported incompatible feature"));
	if ((fs->flags & APFS_FS_FLAG_UNENCRYPTED) == 0 ||
	    (fs->flags & APFS_FS_FLAG_ONE_KEY) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME, "is encrypted"));
	if ((fs->flags & (APFS_FS_FLAG_SPILLED_OVER |
	    APFS_FS_FLAG_SPILLOVER_CLEANER)) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has Fusion spillover"));
	if ((fs->flags & ~(uint64_t)FS_FLAGS_ACCEPTED) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has an unsupported flag"));
	if (fs->revert_to_xid != 0 || fs->revert_to_oid != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has a pending revert"));
	if (fs->rolling_state_oid != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has encryption rolling under way"));
	if (fs->root_to_xid != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has a pending root change"));
	if (fs->root_tree_type != APFS_FS_TREE_VIRTUAL ||
	    fs->extentref_tree_type != APFS_FS_TREE_PHYSICAL ||
	    fs->snapshot_tree_type != APFS_FS_TREE_PHYSICAL)
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "has invalid tree types"));
	return (FSIMG_OK);
}

fsimg_status_t
apfs_volume_read(apfs_container_t *container, apfs_map_t *container_map,
    uint32_t index, uint64_t xid, unsigned char *window, apfs_fs_t *fs,
    size_t *name_bytes, fsimg_error_t *error)
{
	apfs_ref_t ref;
	fsimg_status_t status;

	ref = (apfs_ref_t){ .map = container_map, .role = ROLE_VOLUME,
	    .oid = container->nx.volume_oids[index], .xid = xid,
	    .storage = APFS_OBJ_VIRTUAL, .subtype = APFS_SUBTYPE_NONE,
	    .kind = APFS_KIND_FS };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_fs_decode(window, container->block_size, fs))
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "is shorter than its structure"));
	return (fs_check(fs, index, name_bytes, error));
}

fsimg_status_t
apfs_volume_open(fsimg_reader_t *reader, apfs_reader_t *apfs,
    unsigned char *window, fsimg_error_t *error)
{
	apfs_container_t *container = &apfs->container;
	fsimg_status_t status;

	apfs->read_xid = container->xid;
	status = free_count_read(apfs, window, error);
	if (status != FSIMG_OK)
		return (status);
	status = apfs_map_open(container, APFS_MAP_CONTAINER,
	    container->nx.omap_oid, container->xid, window,
	    &apfs->container_map, error);
	if (status != FSIMG_OK)
		return (status);

	status = volume_choose(&container->nx, &reader->options,
	    &apfs->volume_index, &apfs->volume_count, error);
	if (status != FSIMG_OK)
		return (status);
	status = apfs_volume_read(container, &apfs->container_map,
	    apfs->volume_index, apfs->read_xid, window, &apfs->fs,
	    &apfs->label_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	status = apfs_map_open(container, APFS_MAP_VOLUME, apfs->fs.omap_oid,
	    apfs->read_xid, window, &apfs->volume_map, error);
	if (status != FSIMG_OK)
		return (status);
	names_setup(apfs);
	apfs_cursor_init(&apfs->lookup, container,
	    apfs_fs_tree_profile(apfs->hashed), APFS_OBJ_VIRTUAL,
	    apfs->fs.root_tree_oid, apfs->read_xid, &apfs->volume_map);
	apfs_cursor_init(&apfs->scan, container,
	    apfs_fs_tree_profile(apfs->hashed), APFS_OBJ_VIRTUAL,
	    apfs->fs.root_tree_oid, apfs->read_xid, &apfs->volume_map);
	apfs_cursor_init(&apfs->content, container,
	    apfs_fs_tree_profile(apfs->hashed), APFS_OBJ_VIRTUAL,
	    apfs->fs.root_tree_oid, apfs->read_xid, &apfs->volume_map);
	return (FSIMG_OK);
}

/*
 * The volume's name rule ("Directory records and names"): a
 * case-insensitive or normalization-insensitive volume hashes its
 * directory-record keys.
 */
apfs_name_rule_t
apfs_volume_rule(const apfs_fs_t *fs)
{
	if ((fs->incompat_features & APFS_FS_INCOMPAT_CASE_INSENSITIVE) != 0)
		return (APFS_NAMES_CASELESS);
	if ((fs->incompat_features & APFS_FS_INCOMPAT_NORM_INSENSITIVE) != 0)
		return (APFS_NAMES_NFD);
	return (APFS_NAMES_BYTES);
}

uint64_t
apfs_inode_min(const apfs_fs_t *fs)
{
	return ((fs->features & APFS_FS_FEATURE_VOLGRP_INODES) != 0 &&
	    fs->role == APFS_FS_ROLE_SYSTEM ? APFS_INO_VOLGRP_FIRST :
	    APFS_INO_FIRST);
}

/* The selected volume's name rule, key variant and least ordinary inode. */
static void
names_setup(apfs_reader_t *apfs)
{
	const apfs_fs_t *fs = &apfs->fs;

	apfs->rule = apfs_volume_rule(fs);
	apfs->hashed = apfs->rule != APFS_NAMES_BYTES;
	apfs->inode_min = apfs_inode_min(fs);
}

void
apfs_volume_close(apfs_reader_t *apfs)
{
	apfs_cursor_free(&apfs->content);
	apfs_cursor_free(&apfs->scan);
	apfs_cursor_free(&apfs->lookup);
	apfs_map_close(&apfs->volume_map);
	apfs_map_close(&apfs->container_map);
	apfs_container_close(&apfs->container);
}
