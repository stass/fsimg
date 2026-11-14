/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "apfs.h"
#include "fsimg-endian.h"
#include "reader.h"

/*
 * B-tree cursors (specs/design-apfs-driver.md, "B-trees"): node validation,
 * seeks and the in-order walk. path[0] is the root and path[depth - 1] the
 * deepest node loaded; while the cursor is on a record that node is a leaf
 * and its index is the record's.
 *
 * Every node is validated as it is loaded (node_check()), against the
 * profile, the root's footer and the separator bounds its parent gave it,
 * so a node referenced twice, a subtree repeated under two separators or a
 * cycle fails before any of its records is used; levels fall by one per
 * step, so a path is at most the height, and a walk visits each node once.
 */

#define NODE_FLAGS_KNOWN	(APFS_NODE_ROOT | APFS_NODE_LEAF | \
				    APFS_NODE_FIXED_KV | APFS_NODE_HASHED | \
				    APFS_NODE_NOHEADER)

/* Which way step() moves. */
typedef enum {
	BACKWARD,
	FORWARD
} direction_t;

/* What node_bound() finds: the first key above the one sought, or the
 * first key not below it.
 */
typedef enum {
	FIRST_ABOVE,
	FIRST_NOT_BELOW
} bound_t;

static fsimg_status_t	child_load(apfs_cursor_t *, unsigned,
			    fsimg_error_t *);
static fsimg_status_t	entry_lengths(const apfs_cursor_t *,
			    const apfs_entry_t *, bool, fsimg_error_t *);
static bool		level_contains(const apfs_cursor_t *,
			    const apfs_path_level_t *, const unsigned char *,
			    size_t);
static fsimg_status_t	node_bound(const apfs_cursor_t *,
			    const apfs_path_level_t *, const unsigned char *,
			    size_t, bound_t, uint32_t *, fsimg_error_t *);
static fsimg_status_t	node_check(apfs_cursor_t *, unsigned,
			    fsimg_error_t *);
static bool		node_entry(const apfs_cursor_t *,
			    const apfs_path_level_t *, uint32_t,
			    apfs_entry_t *);
static fsimg_status_t	node_load(apfs_cursor_t *, unsigned, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	root_load(apfs_cursor_t *, fsimg_error_t *);
static fsimg_status_t	step(apfs_cursor_t *, direction_t, bool *,
			    fsimg_error_t *);

void
apfs_cursor_init(apfs_cursor_t *cursor, apfs_container_t *container,
    const apfs_tree_profile_t *profile, uint32_t storage, uint64_t root,
    uint64_t xid, apfs_map_t *map)
{
	*cursor = (apfs_cursor_t){ .container = container, .profile = profile,
	    .map = map, .root = root, .xid = xid, .storage = storage };
}

void
apfs_cursor_free(apfs_cursor_t *cursor)
{
	for (unsigned i = 0; i < APFS_BTREE_LEVELS_MAX; i++) {
		fsimg_dealloc(cursor->path[i].window);
		cursor->path[i].window = NULL;
	}
	cursor->depth = 0;
	cursor->positioned = false;
}

/*
 * Resolve entry index of a level's node. A fixed-size entry's lengths are
 * the tree's fixed key size and, for a leaf, its fixed value size; an index
 * node's value is a child OID.
 */
static bool
node_entry(const apfs_cursor_t *cursor, const apfs_path_level_t *level,
    uint32_t index, apfs_entry_t *entry)
{
	const apfs_tree_profile_t *profile = cursor->profile;
	apfs_kv_size_t sizes = { .key = profile->fixed_key,
	    .val = level->node.level == 0 ? profile->fixed_val :
	    APFS_CHILD_OID_BYTES };

	return (apfs_entry_decode(level->window,
	    cursor->container->block_size, &level->node, index, &sizes, entry));
}

/*
 * An entry's key and value lengths against the profile: a ghost, which has
 * no value, only in a leaf of a tree that allows ghosts; a leaf value within
 * the profile's bounds; an index value exactly a child OID.
 */
static fsimg_status_t
entry_lengths(const apfs_cursor_t *cursor, const apfs_entry_t *entry,
    bool leaf, fsimg_error_t *error)
{
	const apfs_tree_profile_t *profile = cursor->profile;

	if (entry->key_len < profile->key_min ||
	    entry->key_len > profile->key_max)
		return (apfs_corrupt(error, profile->role,
		    "node has a key of the wrong length"));
	if (entry->ghost) {
		if (!leaf ||
		    (cursor->info.flags & APFS_BTREE_ALLOW_GHOSTS) == 0)
			return (apfs_corrupt(error, profile->role,
			    "node has a ghost entry"));
		return (FSIMG_OK);
	}
	if (leaf && (entry->val_len < profile->val_min ||
	    entry->val_len > profile->val_max))
		return (apfs_corrupt(error, profile->role,
		    "node has a value of the wrong length"));
	if (!leaf && entry->val_len != APFS_CHILD_OID_BYTES)
		return (apfs_corrupt(error, profile->role,
		    "node has a value of the wrong length"));
	return (FSIMG_OK);
}

/*
 * The node checks of specs/design-apfs.md, "Validation", for path[depth],
 * whose window and decoded header are loaded and whose bounds are set.
 */
static fsimg_status_t
node_check(apfs_cursor_t *cursor, unsigned depth, fsimg_error_t *error)
{
	const apfs_tree_profile_t *profile = cursor->profile;
	const apfs_path_level_t *level = &cursor->path[depth];
	const apfs_node_t *node = &level->node;
	const char *role = profile->role;
	const unsigned char *window = level->window, *previous = NULL;
	const unsigned char *key;
	apfs_entry_t entry;
	apfs_record_t record;
	size_t previous_len = 0;
	uint32_t storage_flags;
	fsimg_status_t status;
	bool leaf = node->level == 0, fixed = profile->fixed_key != 0;

	/* Hashed and headerless nodes belong to sealed trees. */
	if ((node->flags & (APFS_NODE_HASHED | APFS_NODE_NOHEADER)) != 0)
		return (apfs_unsupported(error, role,
		    "node is hashed or headerless"));
	if ((node->flags & ~NODE_FLAGS_KNOWN) != 0 ||
	    ((node->flags & APFS_NODE_ROOT) != 0) != (depth == 0) ||
	    ((node->flags & APFS_NODE_LEAF) != 0) != leaf ||
	    ((node->flags & APFS_NODE_FIXED_KV) != 0) != fixed)
		return (apfs_corrupt(error, role, "node has invalid flags"));

	if (depth == 0) {
		if (node->level >= APFS_BTREE_LEVELS_MAX)
			return (apfs_unsupported(error, role,
			    "is taller than 16 levels"));
		if ((node->info.flags & ~APFS_BTREE_FLAGS_KNOWN) != 0)
			return (apfs_corrupt(error, role,
			    "has unknown tree flags"));
		if ((node->info.flags & APFS_BTREE_FLAGS_REFUSED) != 0)
			return (apfs_unsupported(error, role,
			    "is a hashed, headerless or nonpersistent tree"));
		storage_flags = cursor->storage == APFS_OBJ_PHYSICAL ?
		    APFS_BTREE_PHYSICAL : cursor->storage == APFS_OBJ_EPHEMERAL ?
		    APFS_BTREE_EPHEMERAL : 0;
		if ((node->info.flags & (APFS_BTREE_PHYSICAL |
		    APFS_BTREE_EPHEMERAL)) != storage_flags ||
		    node->info.node_size != cursor->container->block_size ||
		    node->info.key_size != profile->fixed_key ||
		    node->info.val_size != profile->fixed_val)
			return (apfs_corrupt(error, role,
			    "root does not match its tree"));
		cursor->info = node->info;
	} else if (node->level + 1u != cursor->path[depth - 1].node.level)
		return (apfs_corrupt(error, role, "node has the wrong level"));

	/* Only an empty tree's root leaf may hold no key: an index node
	 * without one leads nowhere.
	 */
	if (node->key_count == 0 && !(depth == 0 && leaf))
		return (apfs_corrupt(error, role, "node is empty"));

	for (uint32_t i = 0; i < node->key_count; i++) {
		if (!node_entry(cursor, level, i, &entry))
			return (apfs_corrupt(error, role,
			    "node has an entry outside its areas"));
		if ((cursor->info.flags & APFS_BTREE_KV_NONALIGNED) == 0 &&
		    ((entry.key_off - node->area.key_start) % 8 != 0 ||
		    (!entry.ghost &&
		    (node->area.value_end - entry.val_off) % 8 != 0)))
			return (apfs_corrupt(error, role,
			    "node has a misaligned entry"));
		status = entry_lengths(cursor, &entry, leaf, error);
		if (status != FSIMG_OK)
			return (status);
		key = window + entry.key_off;
		if (profile->entry_check != NULL) {
			record = (apfs_record_t){ .key = key,
			    .key_len = entry.key_len, .ghost = entry.ghost,
			    .val = entry.ghost ? NULL : window + entry.val_off,
			    .val_len = entry.val_len };
			status = profile->entry_check(&record, leaf, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (previous != NULL && profile->compare(previous,
		    previous_len, key, entry.key_len) >= 0)
			return (apfs_corrupt(error, role,
			    "node has keys out of order"));
		previous = key;
		previous_len = entry.key_len;
	}

	/* Keys increase, so the first and the last decide the bounds. */
	if (node->key_count != 0) {
		(void)node_entry(cursor, level, 0, &entry);
		if (level->low != NULL && profile->compare(window +
		    entry.key_off, entry.key_len, level->low,
		    level->low_len) < 0)
			return (apfs_corrupt(error, role,
			    "node lies outside its parent's separators"));
		if (level->high != NULL && profile->compare(previous,
		    previous_len, level->high, level->high_len) >= 0)
			return (apfs_corrupt(error, role,
			    "node lies outside its parent's separators"));
	}
	return (FSIMG_OK);
}

/*
 * Load node oid as path[depth], whose bounds the caller has set. The path
 * is cut to depth first, so a failure leaves the cursor on the shallower
 * path it can still use, and not on any record.
 */
static fsimg_status_t
node_load(apfs_cursor_t *cursor, unsigned depth, uint64_t oid,
    fsimg_error_t *error)
{
	apfs_path_level_t *level = &cursor->path[depth];
	apfs_ref_t ref;
	fsimg_error_t detail;
	fsimg_status_t status;

	cursor->depth = depth;
	cursor->positioned = false;
	/* Defensive: levels fall by one per step from a root below 16. */
	if (depth >= APFS_BTREE_LEVELS_MAX)
		return (apfs_corrupt(error, cursor->profile->role,
		    "is deeper than its root's level"));
	for (unsigned i = 0; i < depth; i++)
		if (cursor->path[i].oid == oid)
			return (apfs_corrupt(error, cursor->profile->role,
			    "repeats a node on its path"));
	if (level->window == NULL) {
		level->window = fsimg_alloc(cursor->container->reader->ctx,
		    cursor->container->block_size, &detail);
		if (level->window == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	ref = (apfs_ref_t){ .map = cursor->map, .role = cursor->profile->role,
	    .oid = oid, .xid = cursor->xid, .storage = cursor->storage,
	    .subtype = cursor->profile->subtype,
	    .kind = depth == 0 ? APFS_KIND_BTREE_ROOT : APFS_KIND_BTREE_NODE };
	status = apfs_object_read(cursor->container, &ref, level->window, NULL,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_node_decode(level->window, cursor->container->block_size,
	    &level->node))
		return (apfs_corrupt(error, cursor->profile->role,
		    "node is malformed"));
	level->oid = oid;
	level->index = 0;
	status = node_check(cursor, depth, error);
	if (status == FSIMG_OK)
		cursor->depth = depth + 1;
	return (status);
}

static fsimg_status_t
root_load(apfs_cursor_t *cursor, fsimg_error_t *error)
{
	cursor->path[0].low = cursor->path[0].high = NULL;
	cursor->path[0].low_len = cursor->path[0].high_len = 0;
	return (node_load(cursor, 0, cursor->root, error));
}

/*
 * Load the child of path[depth] at its index as path[depth + 1]. The
 * child's bounds are the separator naming it and the next one, or past the
 * last separator the parent's own upper bound.
 */
static fsimg_status_t
child_load(apfs_cursor_t *cursor, unsigned depth, fsimg_error_t *error)
{
	apfs_path_level_t *parent = &cursor->path[depth];
	apfs_path_level_t *child = &cursor->path[depth + 1];
	apfs_entry_t entry;

	/* Defensive, as in node_load(). */
	if (depth + 1 >= APFS_BTREE_LEVELS_MAX)
		return (apfs_corrupt(error, cursor->profile->role,
		    "is deeper than its root's level"));
	if (!node_entry(cursor, parent, parent->index, &entry))
		return (apfs_corrupt(error, cursor->profile->role,
		    "node has an entry outside its areas"));
	child->low = parent->window + entry.key_off;
	child->low_len = entry.key_len;
	child->high = parent->high;
	child->high_len = parent->high_len;
	if (parent->index + 1 < parent->node.key_count) {
		apfs_entry_t next;

		if (!node_entry(cursor, parent, parent->index + 1, &next))
			return (apfs_corrupt(error, cursor->profile->role,
			    "node has an entry outside its areas"));
		child->high = parent->window + next.key_off;
		child->high_len = next.key_len;
	}
	return (node_load(cursor, depth + 1,
	    fsimg_load_le64(parent->window + entry.val_off), error));
}

/*
 * The position in a level's node of the first key above key, or of the
 * first key not below it, by binary search over keys node_check() proved
 * increasing; the key count when there is none.
 */
static fsimg_status_t
node_bound(const apfs_cursor_t *cursor, const apfs_path_level_t *level,
    const unsigned char *key, size_t key_len, bound_t bound,
    uint32_t *position, fsimg_error_t *error)
{
	apfs_entry_t entry;
	uint32_t low = 0, high = level->node.key_count, middle;
	int order;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (!node_entry(cursor, level, middle, &entry))
			return (apfs_corrupt(error, cursor->profile->role,
			    "node has an entry outside its areas"));
		order = cursor->profile->compare(level->window + entry.key_off,
		    entry.key_len, key, key_len);
		if (bound == FIRST_ABOVE ? order <= 0 : order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	*position = low;
	return (FSIMG_OK);
}

static bool
level_contains(const apfs_cursor_t *cursor, const apfs_path_level_t *level,
    const unsigned char *key, size_t key_len)
{
	apfs_compare_fn_t compare = cursor->profile->compare;

	return ((level->low == NULL ||
	    compare(key, key_len, level->low, level->low_len) >= 0) &&
	    (level->high == NULL ||
	    compare(key, key_len, level->high, level->high_len) < 0));
}

/*
 * Move from the current leaf record to the following (forward) or the
 * preceding one: climb while the position is at the node's edge, move one
 * entry, and descend to the nearest edge of the subtree reached. Reaching
 * past the root's edge leaves the cursor on no record.
 */
static fsimg_status_t
step(apfs_cursor_t *cursor, direction_t direction, bool *found,
    fsimg_error_t *error)
{
	apfs_path_level_t *level;
	unsigned depth = cursor->depth - 1;
	fsimg_status_t status;
	bool forward = direction == FORWARD;

	*found = false;
	cursor->positioned = false;
	for (;;) {
		level = &cursor->path[depth];
		if (forward ? level->index + 1 < level->node.key_count :
		    level->index > 0) {
			level->index = forward ? level->index + 1 :
			    level->index - 1;
			break;
		}
		if (depth == 0)
			return (FSIMG_OK);
		depth--;
	}
	while (cursor->path[depth].node.level != 0) {
		status = child_load(cursor, depth, error);
		if (status != FSIMG_OK)
			return (status);
		depth++;
		level = &cursor->path[depth];
		level->index = forward ? 0 : level->node.key_count - 1;
	}
	cursor->depth = depth + 1;
	cursor->positioned = true;
	*found = true;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_cursor_seek(apfs_cursor_t *cursor, const unsigned char *key,
    size_t key_len, apfs_seek_t mode, bool *found, fsimg_error_t *error)
{
	apfs_path_level_t *level;
	unsigned depth;
	uint32_t position;
	fsimg_status_t status;

	*found = false;
	cursor->positioned = false;
	status = fsimg_session_charge(&cursor->container->reader->session, 0,
	    0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (key_len < cursor->profile->key_min ||
	    key_len > cursor->profile->key_max)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "APFS seek key has the wrong length"));
	if (cursor->depth == 0) {
		status = root_load(cursor, error);
		if (status != FSIMG_OK)
			return (status);
	}
	/* Start from the deepest node whose bounds contain the key. */
	depth = cursor->depth - 1;
	while (depth > 0 &&
	    !level_contains(cursor, &cursor->path[depth], key, key_len))
		depth--;
	cursor->depth = depth + 1;

	/* In an index node, take the last child whose separator is not above
	 * the key, or the first child when every separator is.
	 */
	while (cursor->path[depth].node.level != 0) {
		level = &cursor->path[depth];
		status = node_bound(cursor, level, key, key_len, FIRST_ABOVE,
		    &position, error);
		if (status != FSIMG_OK)
			return (status);
		level->index = position == 0 ? 0 : position - 1;
		status = child_load(cursor, depth, error);
		if (status != FSIMG_OK)
			return (status);
		depth++;
	}

	level = &cursor->path[depth];
	if (level->node.key_count == 0)
		return (FSIMG_OK);
	/* LE wants the key before the first one above the key sought; GE
	 * wants the first one not below it. The record can lie in a
	 * neighbouring leaf, when every key of this one lies above the key
	 * (LE) or below it (GE).
	 */
	status = node_bound(cursor, level, key, key_len,
	    mode == APFS_SEEK_LE ? FIRST_ABOVE : FIRST_NOT_BELOW, &position,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (mode == APFS_SEEK_LE) {
		if (position == 0) {
			level->index = 0;
			return (step(cursor, BACKWARD, found, error));
		}
		level->index = position - 1;
	} else {
		if (position == level->node.key_count) {
			level->index = position - 1;
			return (step(cursor, FORWARD, found, error));
		}
		level->index = position;
	}
	cursor->positioned = true;
	*found = true;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_cursor_first(apfs_cursor_t *cursor, bool *found, fsimg_error_t *error)
{
	unsigned depth = 0;
	fsimg_status_t status;

	*found = false;
	cursor->positioned = false;
	status = fsimg_session_charge(&cursor->container->reader->session, 0,
	    0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (cursor->depth == 0) {
		status = root_load(cursor, error);
		if (status != FSIMG_OK)
			return (status);
	}
	cursor->depth = 1;
	cursor->path[0].index = 0;
	while (cursor->path[depth].node.level != 0) {
		status = child_load(cursor, depth, error);
		if (status != FSIMG_OK)
			return (status);
		depth++;
	}
	if (cursor->path[depth].node.key_count == 0)
		return (FSIMG_OK);
	cursor->positioned = true;
	*found = true;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_cursor_next(apfs_cursor_t *cursor, bool *found, fsimg_error_t *error)
{
	fsimg_status_t status;

	*found = false;
	if (!cursor->positioned)
		return (FSIMG_OK);
	/* Each operation charges, so one served from loaded windows still
	 * counts against the work budget and observes cancellation.
	 */
	status = fsimg_session_charge(&cursor->container->reader->session, 0,
	    0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	return (step(cursor, FORWARD, found, error));
}

void
apfs_cursor_record(const apfs_cursor_t *cursor, apfs_record_t *record)
{
	const apfs_path_level_t *level;
	apfs_entry_t entry;

	*record = (apfs_record_t){ .key = NULL };
	if (!cursor->positioned)
		return;
	level = &cursor->path[cursor->depth - 1];
	/* node_check() resolved every entry of a loaded node. */
	if (!node_entry(cursor, level, level->index, &entry))
		return;
	record->key = level->window + entry.key_off;
	record->key_len = entry.key_len;
	record->ghost = entry.ghost;
	if (!entry.ghost) {
		record->val = level->window + entry.val_off;
		record->val_len = entry.val_len;
	}
}
