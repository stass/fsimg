/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "apfs.h"
#include "fsimg-endian.h"

/*
 * Object maps (specs/design-apfs.md, "Object maps"): opening a map and
 * resolving a virtual OID at an XID over its tree's cursor.
 */

#define ROLE_CONTAINER	"container object map"
#define ROLE_VOLUME	"volume object map"

#define OMAP_FLAGS_KNOWN	(APFS_OMAP_MANUALLY_MANAGED | \
				    APFS_OMAP_ENCRYPTING | \
				    APFS_OMAP_DECRYPTING | \
				    APFS_OMAP_KEYROLLING | \
				    APFS_OMAP_CRYPTO_GENERATION)

static int	omap_compare(const unsigned char *, size_t,
		    const unsigned char *, size_t);

/* Keys order by OID, then XID, as integers. */
static int
omap_compare(const unsigned char *a, size_t alen, const unsigned char *b,
    size_t blen)
{
	uint64_t x = fsimg_load_le64(a), y = fsimg_load_le64(b);

	(void)alen;
	(void)blen;
	if (x != y)
		return (x < y ? -1 : 1);
	x = fsimg_load_le64(a + 8);
	y = fsimg_load_le64(b + 8);
	return (x < y ? -1 : x > y);
}

static const apfs_tree_profile_t container_tree = {
	.role = "container object-map tree",
	.compare = omap_compare,
	.subtype = APFS_SUBTYPE_OMAP,
	.key_min = APFS_OMAP_KEY_BYTES, .key_max = APFS_OMAP_KEY_BYTES,
	.val_min = APFS_OMAP_VAL_BYTES, .val_max = APFS_OMAP_VAL_BYTES,
	.fixed_key = APFS_OMAP_KEY_BYTES, .fixed_val = APFS_OMAP_VAL_BYTES
};

static const apfs_tree_profile_t volume_tree = {
	.role = "volume object-map tree",
	.compare = omap_compare,
	.subtype = APFS_SUBTYPE_OMAP,
	.key_min = APFS_OMAP_KEY_BYTES, .key_max = APFS_OMAP_KEY_BYTES,
	.val_min = APFS_OMAP_VAL_BYTES, .val_max = APFS_OMAP_VAL_BYTES,
	.fixed_key = APFS_OMAP_KEY_BYTES, .fixed_val = APFS_OMAP_VAL_BYTES
};

fsimg_status_t
apfs_map_open(apfs_container_t *container, apfs_map_kind_t kind,
    uint64_t oid, uint64_t xid, unsigned char *window, apfs_map_t *map,
    fsimg_error_t *error)
{
	const char *role = kind == APFS_MAP_CONTAINER ? ROLE_CONTAINER :
	    ROLE_VOLUME;
	apfs_omap_t omap;
	apfs_ref_t ref;
	fsimg_status_t status;
	bool manual;

	*map = (apfs_map_t){ .kind = kind };
	ref = (apfs_ref_t){ .role = role, .oid = oid, .xid = xid,
	    .storage = APFS_OBJ_PHYSICAL, .subtype = APFS_SUBTYPE_NONE,
	    .kind = APFS_KIND_OMAP };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_omap_decode(window, container->block_size, &omap))
		return (apfs_corrupt(error, role, "is malformed"));

	/* The closed flag table: crypto generation is accepted and ignored. */
	if ((omap.flags & ~OMAP_FLAGS_KNOWN) != 0 ||
	    (omap.flags & (APFS_OMAP_ENCRYPTING | APFS_OMAP_DECRYPTING |
	    APFS_OMAP_KEYROLLING)) != 0)
		return (apfs_unsupported(error, role,
		    "has unsupported flags"));
	manual = (omap.flags & APFS_OMAP_MANUALLY_MANAGED) != 0;
	if (manual != (kind == APFS_MAP_CONTAINER))
		return (apfs_corrupt(error, role,
		    "has the wrong management flag"));
	if (omap.pending_revert_min != 0 || omap.pending_revert_max != 0)
		return (apfs_unsupported(error, role,
		    "has a pending revert"));
	if (kind == APFS_MAP_CONTAINER && (omap.snapshot_count != 0 ||
	    omap.most_recent_snapshot != 0 || omap.snapshot_tree_oid != 0))
		return (apfs_corrupt(error, role, "has snapshots"));
	if (omap.tree_type != (APFS_OBJ_PHYSICAL | APFS_KIND_BTREE_ROOT) ||
	    omap.tree_oid == 0)
		return (apfs_corrupt(error, role, "has an invalid tree"));

	map->omap = omap;
	apfs_cursor_init(&map->cursor, container,
	    kind == APFS_MAP_CONTAINER ? &container_tree : &volume_tree,
	    APFS_OBJ_PHYSICAL, omap.tree_oid, xid, NULL);
	return (FSIMG_OK);
}

void
apfs_map_close(apfs_map_t *map)
{
	apfs_cursor_free(&map->cursor);
}

fsimg_status_t
apfs_map_lookup(apfs_map_t *map, uint64_t oid, uint64_t xid, bool *found,
    apfs_omap_key_t *key, apfs_omap_val_t *value, fsimg_error_t *error)
{
	const char *role = map->cursor.profile->role;
	apfs_container_t *container = map->cursor.container;
	unsigned char raw[APFS_OMAP_KEY_BYTES];
	apfs_record_t record;
	apfs_omap_key_t found_key;
	apfs_omap_val_t found_value;
	fsimg_status_t status;
	bool positioned;

	*found = false;
	found_key = (apfs_omap_key_t){ .oid = oid, .xid = xid };
	(void)apfs_omap_key_encode(raw, sizeof(raw), &found_key);
	status = apfs_cursor_seek(&map->cursor, raw, sizeof(raw),
	    APFS_SEEK_LE, &positioned, error);
	if (status != FSIMG_OK || !positioned)
		return (status);
	apfs_cursor_record(&map->cursor, &record);
	if (record.ghost ||
	    !apfs_omap_key_decode(record.key, record.key_len, &found_key) ||
	    !apfs_omap_val_decode(record.val, record.val_len, &found_value))
		return (apfs_corrupt(error, role, "has a malformed record"));
	/* A miss never falls through to an older entry. */
	if (found_key.oid != oid ||
	    (found_value.flags & APFS_OMAP_VAL_DELETED) != 0)
		return (FSIMG_OK);

	if ((found_value.flags & (APFS_OMAP_VAL_ENCRYPTED |
	    APFS_OMAP_VAL_NOHEADER)) != 0)
		return (apfs_unsupported(error, role,
		    "maps an encrypted or headerless object"));
	if ((found_value.flags & ~APFS_OMAP_VAL_SAVED) != 0)
		return (apfs_corrupt(error, role, "has invalid value flags"));
	if (found_value.size == 0 ||
	    found_value.size % container->block_size != 0 ||
	    found_value.address == 0 ||
	    found_value.address >= container->block_count ||
	    found_value.size / container->block_size >
	    container->block_count - found_value.address)
		return (apfs_corrupt(error, role,
		    "maps an object outside the container"));
	*key = found_key;
	*value = found_value;
	*found = true;
	return (FSIMG_OK);
}
