/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "apfs.h"
#include "fsimg-endian.h"
#include "unicode.h"

/*
 * Volume selection, the volume superblock and snapshot selection
 * (specs/design-apfs.md, "Volume superblock", "Volume selection" and
 * "Snapshots"; specs/design-apfs-driver.md, "Open", steps 3 to 7). The
 * volume and its object map are read at the checkpoint XID. A view's
 * virtual objects are resolved at the read XID: the checkpoint's for the
 * live volume, the snapshot's for a snapshot view.
 */
#define ROLE_SPACEMAN	"space manager"
#define ROLE_VOLUME	"volume superblock"
#define ROLE_SNAPSHOT	"snapshot"
#define ROLE_SNAPSHOTS	"snapshot metadata tree"
#define ROLE_COPY	"snapshot superblock"
#define ROLE_MAP	"volume object map"
#define ROLE_MAP_TREE	"volume object-map snapshot tree"

/* The most bytes a snapshot name holds stored, its NUL included. */
#define SNAP_NAME_STORED_MAX	(APFS_SNAP_NAME_KEY_MAX - APFS_SNAP_NAME_KEY_BYTES)

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
static void		cursors_init(apfs_reader_t *, uint64_t, uint64_t);
static fsimg_status_t	fs_check(const apfs_fs_t *, uint32_t, size_t *,
			    fsimg_error_t *);
static int		map_snapshot_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);
static fsimg_status_t	map_snapshot_check(apfs_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	name_check(const unsigned char *, size_t *,
			    fsimg_error_t *);
static void		names_setup(apfs_reader_t *);
static fsimg_status_t	selection_invalid(fsimg_error_t *, const char *);
static int		snapshot_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);
static fsimg_status_t	snapshot_name_find(apfs_reader_t *, apfs_cursor_t *,
			    const char *, size_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t	snapshot_select(apfs_reader_t *, const char *,
			    unsigned char *, fsimg_error_t *);
static fsimg_status_t	volume_choose(const apfs_nx_t *,
			    const fsimg_read_options_t *, uint32_t *, uint32_t *,
			    fsimg_error_t *);

/* The snapshot metadata tree: metadata records by XID, then names. */
static const apfs_tree_profile_t snapshot_tree = {
	.role = ROLE_SNAPSHOTS,
	.compare = snapshot_compare,
	.subtype = APFS_SUBTYPE_SNAPMETA,
	.key_min = APFS_JKEY_BYTES, .key_max = APFS_SNAP_NAME_KEY_MAX,
	.val_min = APFS_SNAP_NAME_VAL_BYTES, .val_max = APFS_SNAP_META_VAL_MAX
};

/* The volume object map's snapshot tree: one fixed record per snapshot. */
static const apfs_tree_profile_t map_snapshot_tree = {
	.role = ROLE_MAP_TREE,
	.compare = map_snapshot_compare,
	.subtype = APFS_SUBTYPE_OMAP_SNAPSHOT,
	.key_min = APFS_OMAP_SNAP_KEY_BYTES, .key_max = APFS_OMAP_SNAP_KEY_BYTES,
	.val_min = APFS_OMAP_SNAP_VAL_BYTES, .val_max = APFS_OMAP_SNAP_VAL_BYTES,
	.fixed_key = APFS_OMAP_SNAP_KEY_BYTES,
	.fixed_val = APFS_OMAP_SNAP_VAL_BYTES
};

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
	    apfs->volume_index, container->xid, window, &apfs->fs,
	    &apfs->label_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	status = apfs_map_open(container, APFS_MAP_VOLUME, apfs->fs.omap_oid,
	    container->xid, window, &apfs->volume_map, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->options.snapshot != NULL) {
		status = snapshot_select(apfs, reader->options.snapshot, window,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	names_setup(apfs);
	cursors_init(apfs, apfs->snapshot ? apfs->snapshot_fs.root_tree_oid :
	    apfs->fs.root_tree_oid, apfs->read_xid);
	return (FSIMG_OK);
}

/*
 * The three filesystem-tree cursors of a view: its root, resolved with
 * every node it reaches through the live volume object map at xid.
 */
static void
cursors_init(apfs_reader_t *apfs, uint64_t root, uint64_t xid)
{
	const apfs_tree_profile_t *profile = apfs_fs_tree_profile(apfs->hashed);

	apfs_cursor_init(&apfs->lookup, &apfs->container, profile,
	    APFS_OBJ_VIRTUAL, root, xid, &apfs->volume_map);
	apfs_cursor_init(&apfs->scan, &apfs->container, profile,
	    APFS_OBJ_VIRTUAL, root, xid, &apfs->volume_map);
	apfs_cursor_init(&apfs->content, &apfs->container, profile,
	    APFS_OBJ_VIRTUAL, root, xid, &apfs->volume_map);
}

void
apfs_volume_live(apfs_reader_t *apfs)
{
	if (!apfs->snapshot)
		return;
	apfs_cursor_free(&apfs->content);
	apfs_cursor_free(&apfs->scan);
	apfs_cursor_free(&apfs->lookup);
	apfs->snapshot = false;
	apfs->read_xid = apfs->container.xid;
	cursors_init(apfs, apfs->fs.root_tree_oid, apfs->read_xid);
}

/*
 * Snapshot selection, step 7 of open. The name record gives the snapshot's
 * XID and the metadata record its flags and superblock copy; the object
 * map's own record of the snapshot must exist and carry no flag. Only once
 * every check passes does the copy become the view and its XID the read
 * XID. A snapshot of a volume that keeps dataless snapshots may lack data
 * the live object map no longer retains, so it is refused before the
 * search.
 */
static fsimg_status_t
snapshot_select(apfs_reader_t *apfs, const char *name, unsigned char *window,
    fsimg_error_t *error)
{
	apfs_container_t *container = &apfs->container;
	apfs_snap_meta_val_t meta;
	apfs_cursor_t cursor;
	uint64_t xid, copy_oid = 0;
	size_t length;
	fsimg_status_t status;

	if ((apfs->fs.incompat_features &
	    APFS_FS_INCOMPAT_DATALESS_SNAPSHOTS) != 0)
		return (apfs_unsupported(error, ROLE_VOLUME,
		    "has dataless snapshots"));
	/* The reader core admits 1 to 255 bytes; the bound keeps the key
	 * buffer's size a local fact.
	 */
	length = strnlen(name, SNAP_NAME_STORED_MAX);
	if (length == 0 || length >= SNAP_NAME_STORED_MAX)
		return (selection_invalid(error,
		    "APFS snapshot name is not 1 to 255 bytes"));

	/* The snapshot metadata tree is physical, read at the checkpoint. */
	apfs_cursor_init(&cursor, container, &snapshot_tree, APFS_OBJ_PHYSICAL,
	    apfs->fs.snapshot_tree_oid, container->xid, NULL);
	status = snapshot_name_find(apfs, &cursor, name, length, &xid, error);
	if (status == FSIMG_OK)
		status = apfs_snapshot_meta_find(&cursor, name, length, xid,
		    &meta, error);
	if (status == FSIMG_OK && meta.flags != 0)
		status = apfs_unsupported(error, ROLE_SNAPSHOT,
		    "has unsupported flags");
	if (status == FSIMG_OK)
		copy_oid = meta.sblock_oid;
	apfs_cursor_free(&cursor);
	if (status == FSIMG_OK)
		status = map_snapshot_check(apfs, xid, error);
	if (status == FSIMG_OK)
		status = apfs_snapshot_copy_read(container, &apfs->fs, copy_oid,
		    xid, window, &apfs->snapshot_fs, error);
	if (status != FSIMG_OK)
		return (status);
	apfs->snapshot = true;
	apfs->read_xid = xid;
	return (FSIMG_OK);
}

/*
 * The name record of the name's length bytes, found by its exact key: the
 * all-ones OID, the name kind, the stored length with the NUL and the
 * bytes with the NUL, which snapshot_compare orders. Its value is the
 * snapshot's XID, nonzero and not above the checkpoint's. A record whose
 * key does not equal the one sought means the volume holds no such name,
 * the caller's selection, FSIMG_EINVAL; a record that equals it but is a
 * ghost or does not decode is damage.
 */
static fsimg_status_t
snapshot_name_find(apfs_reader_t *apfs, apfs_cursor_t *cursor,
    const char *name, size_t length, uint64_t *xid, fsimg_error_t *error)
{
	unsigned char key[APFS_SNAP_NAME_KEY_MAX];
	const apfs_name_key_t sought = { .name = name,
	    .oid = APFS_JKEY_OID_MASK, .name_len = (uint16_t)length };
	size_t key_len = APFS_SNAP_NAME_KEY_BYTES + length + 1;
	apfs_name_key_t found;
	apfs_record_t record;
	fsimg_status_t status;
	bool positioned;

	if (!apfs_snap_name_key_encode(key, key_len, &sought))
		return (selection_invalid(error,
		    "APFS snapshot name is not 1 to 255 bytes"));
	status = apfs_cursor_seek(cursor, key, key_len, APFS_SEEK_LE,
	    &positioned, error);
	if (status != FSIMG_OK)
		return (status);
	if (positioned)
		apfs_cursor_record(cursor, &record);
	if (!positioned ||
	    snapshot_compare(record.key, record.key_len, key, key_len) != 0)
		return (selection_invalid(error,
		    "APFS volume has no snapshot of that name"));
	if (record.ghost ||
	    !apfs_snap_name_key_decode(record.key, record.key_len, &found) ||
	    !apfs_snap_name_val_decode(record.val, record.val_len, xid))
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "has a malformed name record"));
	if (*xid == 0)
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "names a snapshot of XID zero"));
	if (*xid > apfs->container.xid)
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "names a snapshot newer than the checkpoint"));
	return (FSIMG_OK);
}

/*
 * The metadata record at the snapshot's XID, a bare record header as its
 * key: it must exist and carry the name the name record does. Its decoded
 * value, whose name points into the cursor's node, is returned in meta.
 */
fsimg_status_t
apfs_snapshot_meta_find(apfs_cursor_t *cursor, const char *name,
    size_t length, uint64_t xid, apfs_snap_meta_val_t *meta,
    fsimg_error_t *error)
{
	unsigned char key[APFS_JKEY_BYTES];
	apfs_record_t record;
	fsimg_status_t status;
	bool positioned;

	fsimg_store_le64(key, xid |
	    ((uint64_t)APFS_J_SNAP_METADATA << APFS_JKEY_KIND_SHIFT));
	status = apfs_cursor_seek(cursor, key, sizeof(key), APFS_SEEK_LE,
	    &positioned, error);
	if (status != FSIMG_OK)
		return (status);
	if (positioned)
		apfs_cursor_record(cursor, &record);
	if (!positioned || apfs_jkey_compare(record.key, record.key_len, key,
	    sizeof(key)) != 0)
		return (apfs_corrupt(error, ROLE_SNAPSHOT,
		    "has no metadata record"));
	if (record.ghost || record.key_len != APFS_JKEY_BYTES ||
	    !apfs_snap_meta_val_decode(record.val, record.val_len, meta))
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "has a malformed metadata record"));
	if (meta->name_len != length || memcmp(meta->name, name, length) != 0)
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "metadata names another snapshot"));
	return (FSIMG_OK);
}

/*
 * The volume object map's snapshot tree, read at the checkpoint XID, holds
 * a record for every snapshot; this one's must exist and carry no flag.
 * The record's OID is reserved and not judged.
 */
static fsimg_status_t
map_snapshot_check(apfs_reader_t *apfs, uint64_t xid, fsimg_error_t *error)
{
	apfs_container_t *container = &apfs->container;
	const apfs_omap_t *omap = &apfs->volume_map.omap;
	unsigned char key[APFS_OMAP_SNAP_KEY_BYTES];
	apfs_record_t record;
	apfs_cursor_t cursor;
	fsimg_status_t status;
	bool positioned;

	if (omap->snapshot_tree_oid == 0)
		return (apfs_corrupt(error, ROLE_MAP,
		    "has no record of the snapshot"));
	if (omap->snapshot_tree_type != (APFS_OBJ_PHYSICAL |
	    APFS_KIND_BTREE_ROOT))
		return (apfs_corrupt(error, ROLE_MAP,
		    "has an invalid snapshot tree"));
	fsimg_store_le64(key, xid);
	apfs_cursor_init(&cursor, container, &map_snapshot_tree,
	    APFS_OBJ_PHYSICAL, omap->snapshot_tree_oid, container->xid, NULL);
	status = apfs_cursor_seek(&cursor, key, sizeof(key), APFS_SEEK_LE,
	    &positioned, error);
	if (status == FSIMG_OK) {
		if (positioned)
			apfs_cursor_record(&cursor, &record);
		if (!positioned || record.ghost ||
		    fsimg_load_le64(record.key) != xid)
			status = apfs_corrupt(error, ROLE_MAP,
			    "has no record of the snapshot");
		else if (fsimg_load_le32(record.val) != 0)
			status = apfs_unsupported(error, ROLE_MAP,
			    "snapshot record has unsupported flags");
	}
	apfs_cursor_free(&cursor);
	return (status);
}

/*
 * The snapshot's superblock copy: a physical volume superblock at oid
 * whose header XID is the snapshot's. Its own rules, not the live
 * superblock's: it names no trees of its own, so the live object map is
 * the one searched, and its incompatible features are the live ones, so
 * the live name rule reads it. Its name, UUID and counts are not checked.
 */
fsimg_status_t
apfs_snapshot_copy_read(apfs_container_t *container, const apfs_fs_t *live,
    uint64_t oid, uint64_t xid, unsigned char *window, apfs_fs_t *copy,
    fsimg_error_t *error)
{
	apfs_obj_t header;
	apfs_ref_t ref;
	fsimg_status_t status;

	ref = (apfs_ref_t){ .role = ROLE_COPY, .oid = oid, .xid = xid,
	    .storage = APFS_OBJ_PHYSICAL, .subtype = APFS_SUBTYPE_NONE,
	    .kind = APFS_KIND_FS };
	status = apfs_object_read(container, &ref, window, &header, error);
	if (status != FSIMG_OK)
		return (status);
	if (header.xid != xid)
		return (apfs_corrupt(error, ROLE_COPY, "has the wrong XID"));
	if (!apfs_fs_decode(window, container->block_size, copy))
		return (apfs_corrupt(error, ROLE_COPY,
		    "is shorter than its structure"));
	if (copy->magic != APFS_FS_MAGIC)
		return (apfs_corrupt(error, ROLE_COPY, "has no APSB magic"));
	if (copy->omap_oid != 0 || copy->extentref_tree_oid != 0 ||
	    copy->snapshot_tree_oid != 0)
		return (apfs_unsupported(error, ROLE_COPY,
		    "names trees of its own"));
	if (copy->incompat_features != live->incompat_features)
		return (apfs_unsupported(error, ROLE_COPY,
		    "has other incompatible features"));
	return (FSIMG_OK);
}

/*
 * Snapshot keys order as record headers, by OID (low 60 bits) then kind,
 * then name records by their stored name bytes with the terminating NUL,
 * so a proper prefix sorts first (E9). A name key shorter than its fixed
 * part orders before every name of its header.
 */
static int
snapshot_compare(const unsigned char *a, size_t alen, const unsigned char *b,
    size_t blen)
{
	const size_t at = APFS_SNAP_NAME_KEY_BYTES;
	size_t common;
	int order;

	order = apfs_jkey_compare(a, alen, b, blen);
	if (order != 0 || (alen <= at && blen <= at))
		return (order);
	if (alen <= at || blen <= at)
		return (alen <= at ? -1 : 1);
	common = (alen < blen ? alen : blen) - at;
	order = memcmp(a + at, b + at, common);
	if (order != 0)
		return (order < 0 ? -1 : 1);
	return (alen < blen ? -1 : alen > blen);
}

/* Object-map snapshot keys are XIDs. */
static int
map_snapshot_compare(const unsigned char *a, size_t alen,
    const unsigned char *b, size_t blen)
{
	uint64_t x = fsimg_load_le64(a), y = fsimg_load_le64(b);

	(void)alen;
	(void)blen;
	return (x < y ? -1 : x > y);
}

const apfs_tree_profile_t *
apfs_snapshot_tree_profile(void)
{
	return (&snapshot_tree);
}

const apfs_tree_profile_t *
apfs_map_snapshot_tree_profile(void)
{
	return (&map_snapshot_tree);
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
