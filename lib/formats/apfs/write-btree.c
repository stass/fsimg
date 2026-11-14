/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"
#include "write-apfs.h"

/*
 * The bulk B-tree packer. It keeps one open node per level. A record joins
 * its level's open node while the node still has room for it; otherwise
 * that node closes and the record opens the next one. Closing a node
 * encodes it, gives it the next completion index, hands it to the sink and,
 * unless it is the root, adds its index record, its first key and its OID,
 * to the level above, opening that level when there is none. When the
 * records end, the open nodes close from the leaves up and the topmost is
 * the root. A level above exists only once the level below has closed a
 * full node and opened another, so a root that is not a leaf has at least
 * two children, and an open node is never empty except the root leaf of a
 * tree with no records.
 *
 * Planning and emission differ only in the sink, so the decisions above,
 * which depend on record lengths alone, are made by the same code in both.
 *
 * A node's table of contents grows with its key count, its key area starts
 * after the table, and a root's value area ends before its footer, so no
 * record's place in the block is known until the node closes. An open node
 * therefore stages its keys and values in buffers of their own, at the
 * offsets its table of contents will store: a key's from the key area's
 * start, a value's back from the value area's end. node_encode() then lays
 * the node out through the codec's encoders, which prove every span before
 * a byte is copied into it.
 */

/*
 * A node's table holds at least one entry of at least 4 bytes per key
 * within the capacity, so no node holds more keys than this.
 */
#define ENTRIES_MAX	(APFS_PACK_CAPACITY / APFS_TOC_FIXED_BYTES)

/* A table of contents grows in steps of this many entries. */
#define TOC_INCREMENT	8u


/*
 * The open node of one level. toc[i] is entry i as the node will store it,
 * lengths included even in a fixed-size tree, whose stored entries omit
 * them. Key i starts at keys[toc[i].key.off] and value i starts
 * toc[i].val.off bytes before the end of vals, so values stack downward
 * from the end as they do in the node. key_bytes and val_bytes are
 * the lengths the staged records take, rounded up to 8 bytes each unless
 * the tree is unaligned.
 */
typedef struct pack_level {
	apfs_toc_t	toc[ENTRIES_MAX];
	unsigned char	keys[APFS_PACK_CAPACITY];
	unsigned char	vals[APFS_PACK_CAPACITY];
	uint32_t	count;
	uint32_t	key_bytes;
	uint32_t	val_bytes;
} pack_level_t;

/*
 * levels[0] to levels[result.levels - 1] are allocated; result accumulates
 * as nodes close. block is the one encode buffer: a node is written to it
 * and handed to the sink as it closes. done is set by a failure or by
 * apfs_pack_finish(), after which the open nodes no longer describe a tree
 * and the packer refuses further records.
 */
struct apfs_pack {
	unsigned char	 block[APFS_WRITE_BLOCK_BYTES];
	pack_level_t	*levels[APFS_BTREE_LEVELS_MAX];
	apfs_pack_result_t result;
	fsimg_ctx_t	*ctx;
	const apfs_pack_profile_t *profile;
	const apfs_pack_sink_t *sink;
	uint64_t	 xid;
	uint32_t	 longest_key;	/* over leaf records */
	uint32_t	 longest_val;
	bool		 done;
};

static fsimg_status_t	level_add(apfs_pack_t *, unsigned,
			    const apfs_record_t *, fsimg_error_t *);
static bool		level_fits(const apfs_pack_profile_t *, bool,
			    const pack_level_t *, uint32_t, uint32_t);
static fsimg_status_t	level_open(apfs_pack_t *, fsimg_error_t *);
static fsimg_status_t	node_close(apfs_pack_t *, unsigned, bool,
			    fsimg_error_t *);
static bool		node_encode(apfs_pack_t *, unsigned, bool, uint64_t);
static uint64_t		node_oid(const apfs_pack_t *, uint64_t);
static fsimg_status_t	pack_error(fsimg_error_t *, fsimg_status_t,
			    const char *, const char *);
static bool		profile_valid(const apfs_pack_profile_t *);
static fsimg_status_t	record_check(const apfs_pack_profile_t *,
			    const apfs_record_t *, fsimg_error_t *);
static uint32_t		round_toc(uint32_t);
static uint32_t		stored_length(const apfs_pack_profile_t *, uint32_t);
static uint32_t		toc_bytes(const apfs_pack_profile_t *, bool, uint32_t);

/*
 * The native shapes, whose volume trees E1 and E3 show (specs/design-apfs.md,
 * "Writer values"). The filesystem, extent-reference and snapshot trees
 * are variable-size and unaligned; an object map and a free queue have
 * fixed-size records.
 */
const apfs_pack_profile_t apfs_pack_fs_tree = {
	.role = "filesystem tree",
	.storage = APFS_OBJ_VIRTUAL,
	.subtype = APFS_SUBTYPE_FSTREE,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_KV_NONALIGNED
};

const apfs_pack_profile_t apfs_pack_container_omap = {
	.role = "container object map",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_OMAP,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL,
	.fixed_key = APFS_OMAP_KEY_BYTES,
	.fixed_val = APFS_OMAP_VAL_BYTES
};

const apfs_pack_profile_t apfs_pack_volume_omap = {
	.role = "volume object map",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_OMAP,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL,
	.fixed_key = APFS_OMAP_KEY_BYTES,
	.fixed_val = APFS_OMAP_VAL_BYTES
};

const apfs_pack_profile_t apfs_pack_extentref = {
	.role = "extent-reference tree",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_EXTENTREF,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL |
	    APFS_BTREE_KV_NONALIGNED
};

const apfs_pack_profile_t apfs_pack_snapshot = {
	.role = "snapshot tree",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_SNAPMETA,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL |
	    APFS_BTREE_KV_NONALIGNED
};

const apfs_pack_profile_t apfs_pack_free_queue = {
	.role = "free queue",
	.storage = APFS_OBJ_EPHEMERAL,
	.subtype = APFS_SUBTYPE_FREE_QUEUE,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_ALLOW_GHOSTS |
	    APFS_BTREE_EPHEMERAL,
	.fixed_key = APFS_FQ_KEY_BYTES,
	.fixed_val = APFS_FQ_VAL_BYTES
};

static fsimg_status_t
pack_error(fsimg_error_t *error, fsimg_status_t status, const char *role,
    const char *what)
{
	char message[160];

	(void)snprintf(message, sizeof(message), "APFS %s %s", role, what);
	return (fsimg_error_set(error, status, 0, FSIMG_OP_WRITER, message));
}

/* The bytes a key or value of length bytes takes in a node. */
static uint32_t
stored_length(const apfs_pack_profile_t *profile, uint32_t length)
{
	if ((profile->tree_flags & APFS_BTREE_KV_NONALIGNED) != 0)
		return (length);
	return ((length + 7u) & ~7u);
}

/*
 * The length of the table of contents of a leaf or index node of entries
 * keys, as the native formatter sizes it. A variable-size table holds the
 * entries rounded up to the table's increment, and at least one
 * increment. A fixed-size table holds at least a full node: as many
 * records of the node's kind, with their entries, as APFS_PACK_CAPACITY
 * holds, rounded up the same way. An object map's leaf, with 16-byte keys
 * and values, gets 112 entries; its index nodes, with 8-byte child OIDs,
 * and a free queue's leaf, with 8-byte values, get 144. Only a leaf of
 * ghosts can hold more keys than a full node, and its table grows with
 * them. entries is at most ENTRIES_MAX + 1, so the product cannot
 * overflow.
 */
static uint32_t
toc_bytes(const apfs_pack_profile_t *profile, bool leaf, uint32_t entries)
{
	uint32_t entries_min, record_bytes, full_node;

	entries_min = entries == 0 ? 1 : entries;
	if (profile->fixed_key == 0)
		return (round_toc(entries_min) * APFS_TOC_VARIABLE_BYTES);
	record_bytes = APFS_TOC_FIXED_BYTES +
	    stored_length(profile, profile->fixed_key) + (leaf ?
	    stored_length(profile, profile->fixed_val) : APFS_CHILD_OID_BYTES);
	full_node = APFS_PACK_CAPACITY / record_bytes;
	if (entries_min < full_node)
		entries_min = full_node;
	return (round_toc(entries_min) * APFS_TOC_FIXED_BYTES);
}

/* entries rounded up to the table's increment. */
static uint32_t
round_toc(uint32_t entries)
{
	return ((entries + TOC_INCREMENT - 1) / TOC_INCREMENT * TOC_INCREMENT);
}

/*
 * Whether level's open node has room for one more record whose key and
 * value take key_stored and val_stored bytes. Every term is at most
 * APFS_PACK_CAPACITY, so the sum cannot overflow.
 */
static bool
level_fits(const apfs_pack_profile_t *profile, bool leaf,
    const pack_level_t *level, uint32_t key_stored, uint32_t val_stored)
{
	return (toc_bytes(profile, leaf, level->count + 1) + level->key_bytes +
	    key_stored + level->val_bytes + val_stored <= APFS_PACK_CAPACITY);
}

static uint64_t
node_oid(const apfs_pack_t *pack, uint64_t index)
{
	if (pack->sink == NULL || pack->sink->node_oid == NULL)
		return (index);
	return (pack->sink->node_oid(pack->sink->arg, index));
}

/* Allocate the level above the current top, its open node empty. */
static fsimg_status_t
level_open(apfs_pack_t *pack, fsimg_error_t *error)
{
	pack_level_t *level;
	fsimg_error_t detail;

	level = fsimg_alloc(pack->ctx, sizeof(*level), &detail);
	if (level == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* The staging buffers need no clearing: only staged spans are read. */
	level->count = level->key_bytes = level->val_bytes = 0;
	pack->levels[pack->result.levels++] = level;
	return (FSIMG_OK);
}

/*
 * Add record to the open node of level depth, first closing that node when
 * the record does not fit it, and opening the level when the record is the
 * first to reach it. apfs_pack_add() admits only a record whose leaf form
 * fits an empty node and two of whose index forms do, and an index key is a
 * copy of a leaf key, so a record always fits the node opened for it and an
 * index node that closes because it is full holds at least two records.
 * Only a level's last node may hold one.
 */
static fsimg_status_t
level_add(apfs_pack_t *pack, unsigned depth, const apfs_record_t *record,
    fsimg_error_t *error)
{
	const apfs_pack_profile_t *profile = pack->profile;
	pack_level_t *level;
	apfs_toc_t *entry;
	fsimg_status_t status;
	uint32_t key_stored, val_stored;

	if (depth == APFS_BTREE_LEVELS_MAX)
		return (pack_error(error, FSIMG_ERANGE, profile->role,
		    "would be taller than 16 levels"));
	if (depth == pack->result.levels) {
		status = level_open(pack, error);
		if (status != FSIMG_OK)
			return (status);
	}
	level = pack->levels[depth];

	key_stored = stored_length(profile, (uint32_t)record->key_len);
	val_stored = record->ghost ? 0 :
	    stored_length(profile, (uint32_t)record->val_len);
	if (level->count != 0 &&
	    !level_fits(profile, depth == 0, level, key_stored, val_stored)) {
		status = node_close(pack, depth, false, error);
		if (status != FSIMG_OK)
			return (status);
	}

	entry = &level->toc[level->count];
	entry->key.off = (uint16_t)level->key_bytes;
	entry->key.len = (uint16_t)record->key_len;
	bcopy(record->key, level->keys + level->key_bytes, record->key_len);
	level->key_bytes += key_stored;
	if (record->ghost) {
		entry->val.off = APFS_TOC_GHOST;
		entry->val.len = 0;
	} else {
		level->val_bytes += val_stored;
		entry->val.off = (uint16_t)level->val_bytes;
		entry->val.len = (uint16_t)record->val_len;
		if (record->val_len != 0)
			bcopy(record->val, level->vals + sizeof(level->vals) -
			    level->val_bytes, record->val_len);
	}
	level->count++;
	return (FSIMG_OK);
}

/*
 * Encode the open node of level depth into pack->block as node oid: the
 * object header, the node header with the root's footer, then each staged
 * entry through apfs_entry_encode(), whose proven spans are the only places
 * a key or value is copied. The block is zeroed first, so alignment padding,
 * the free space and a non-root node's unused footer bytes hold zero.
 * False means the codec refused a span, which the fit arithmetic rules out.
 */
static bool
node_encode(apfs_pack_t *pack, unsigned depth, bool root, uint64_t oid)
{
	const apfs_pack_profile_t *profile = pack->profile;
	const pack_level_t *level = pack->levels[depth];
	const apfs_toc_t *staged;
	unsigned char *block = pack->block;
	apfs_kv_size_t sizes;
	apfs_entry_t entry;
	apfs_node_t node;
	apfs_obj_t obj;
	uint32_t free_len, toc_len, value_end;

	bzero(block, sizeof(pack->block));
	obj = (apfs_obj_t){ .oid = oid, .xid = pack->xid,
	    .flags = profile->storage, .subtype = profile->subtype,
	    .kind = root ? APFS_KIND_BTREE_ROOT : APFS_KIND_BTREE_NODE };
	if (!apfs_obj_encode(block, sizeof(pack->block), &obj))
		return (false);

	/* The free space lies between the keys and the values. */
	toc_len = toc_bytes(profile, depth == 0, level->count);
	value_end = APFS_WRITE_BLOCK_BYTES -
	    (root ? APFS_BTREE_INFO_BYTES : 0);
	free_len = value_end - APFS_NODE_HEADER_BYTES - toc_len -
	    level->key_bytes - level->val_bytes;
	node = (apfs_node_t){ .level = (uint16_t)depth,
	    .key_count = level->count,
	    .toc = { .off = 0, .len = (uint16_t)toc_len },
	    .free = { .off = (uint16_t)level->key_bytes,
	    .len = (uint16_t)free_len },
	    .key_free = { .off = APFS_FREE_LIST_EMPTY, .len = 0 },
	    .val_free = { .off = APFS_FREE_LIST_EMPTY, .len = 0 } };
	if (root)
		node.flags |= APFS_NODE_ROOT;
	if (depth == 0)
		node.flags |= APFS_NODE_LEAF;
	if (profile->fixed_key != 0)
		node.flags |= APFS_NODE_FIXED_KV;
	if (root)
		node.info = (apfs_btree_info_t){
		    .key_count = pack->result.key_count,
		    .node_count = pack->result.node_count + 1,
		    .flags = profile->tree_flags,
		    .node_size = APFS_WRITE_BLOCK_BYTES,
		    .key_size = profile->fixed_key,
		    .val_size = profile->fixed_val,
		    .longest_key = pack->longest_key,
		    .longest_val = pack->longest_val };
	if (!apfs_node_encode(block, sizeof(pack->block), &node))
		return (false);

	sizes = (apfs_kv_size_t){ .key = profile->fixed_key,
	    .val = depth == 0 ? profile->fixed_val : APFS_CHILD_OID_BYTES };
	for (uint32_t i = 0; i < level->count; i++) {
		staged = &level->toc[i];
		if (!apfs_entry_encode(block, sizeof(pack->block), &node, i,
		    staged, &sizes, &entry))
			return (false);
		bcopy(level->keys + staged->key.off, block + entry.key_off,
		    entry.key_len);
		if (!entry.ghost && entry.val_len != 0)
			bcopy(level->vals + sizeof(level->vals) -
			    staged->val.off, block + entry.val_off,
			    entry.val_len);
	}
	return (apfs_object_seal(block, sizeof(pack->block)));
}

/*
 * Close the open node of level depth: encode it as the next node in
 * completion order, hand it to the sink, and unless it is the root add its
 * index record to the level above. The level is left with an empty open
 * node. The index record is added last, so a parent that closes because of
 * it completes after this child.
 */
static fsimg_status_t
node_close(apfs_pack_t *pack, unsigned depth, bool root, fsimg_error_t *error)
{
	const apfs_pack_sink_t *sink = pack->sink;
	pack_level_t *level = pack->levels[depth];
	apfs_record_t index_record;
	fsimg_status_t status;
	unsigned char child_oid[APFS_CHILD_OID_BYTES];
	uint64_t index, oid;

	index = pack->result.node_count;
	oid = node_oid(pack, index);
	if (!node_encode(pack, depth, root, oid))
		return (pack_error(error, FSIMG_EINVAL, pack->profile->role,
		    "node does not encode"));
	if (sink != NULL && sink->write != NULL) {
		status = sink->write(sink->arg, index, oid, pack->block, error);
		if (status != FSIMG_OK)
			return (status);
	}
	pack->result.node_count++;
	pack->result.level_nodes[depth]++;
	if (root) {
		pack->result.root_oid = oid;
		level->count = level->key_bytes = level->val_bytes = 0;
		return (FSIMG_OK);
	}

	/*
	 * The index record borrows the staged first key, so the level is
	 * emptied only once the level above has copied it.
	 */
	fsimg_store_le64(child_oid, oid);
	index_record = (apfs_record_t){ .key = level->keys,
	    .key_len = level->toc[0].key.len, .val = child_oid,
	    .val_len = sizeof(child_oid) };
	status = level_add(pack, depth + 1, &index_record, error);
	level->count = level->key_bytes = level->val_bytes = 0;
	return (status);
}

/*
 * Whether profile describes a tree the reader accepts: a role, a storage
 * class whose footer bit the flags carry and no other class's, only known
 * flags and none of a sealed or nonpersistent tree, fixed sizes both set
 * or both zero, and a table minimum that leaves room for records.
 */
static bool
profile_valid(const apfs_pack_profile_t *profile)
{
	uint32_t storage_flags;

	if (profile->role == NULL)
		return (false);
	switch (profile->storage) {
	case APFS_OBJ_PHYSICAL:
		storage_flags = APFS_BTREE_PHYSICAL;
		break;
	case APFS_OBJ_EPHEMERAL:
		storage_flags = APFS_BTREE_EPHEMERAL;
		break;
	case APFS_OBJ_VIRTUAL:
		storage_flags = 0;
		break;
	default:
		return (false);
	}
	if ((profile->tree_flags & ~APFS_BTREE_FLAGS_KNOWN) != 0 ||
	    (profile->tree_flags & APFS_BTREE_FLAGS_REFUSED) != 0 ||
	    (profile->tree_flags & (APFS_BTREE_PHYSICAL |
	    APFS_BTREE_EPHEMERAL)) != storage_flags)
		return (false);
	if ((profile->fixed_key == 0) != (profile->fixed_val == 0))
		return (false);
	return (toc_bytes(profile, true, 2) < APFS_PACK_CAPACITY &&
	    toc_bytes(profile, false, 2) < APFS_PACK_CAPACITY);
}

fsimg_status_t
apfs_pack_open(fsimg_ctx_t *ctx, const apfs_pack_profile_t *profile,
    uint64_t xid, const apfs_pack_sink_t *sink, apfs_pack_t **out,
    fsimg_error_t *error)
{
	apfs_pack_t *pack;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	if (!profile_valid(profile))
		return (pack_error(error, FSIMG_EINVAL,
		    profile->role != NULL ? profile->role : "B-tree",
		    "tree profile is malformed"));

	pack = fsimg_alloc(ctx, sizeof(*pack), &detail);
	if (pack == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* Every field is set here; the block is cleared per node anyway. */
	*pack = (apfs_pack_t){ .ctx = ctx, .profile = profile, .sink = sink,
	    .xid = xid };

	/* Level 0 exists from the start: an empty tree is a root leaf. */
	status = level_open(pack, error);
	if (status != FSIMG_OK) {
		apfs_pack_free(pack);
		return (status);
	}
	*out = pack;
	return (FSIMG_OK);
}

/*
 * Whether record may join the tree, under the rules apfs_pack_add()
 * states. The record must fit an empty node as a leaf record, and two index
 * records naming nodes it starts, its key with a child OID, must fit one,
 * so every index node closed full has at least two children, only a
 * level's last node may have one, and the height grows with the logarithm
 * of the record count. Bounding the lengths first keeps the
 * arithmetic in 32 bits.
 */
static fsimg_status_t
record_check(const apfs_pack_profile_t *profile, const apfs_record_t *record,
    fsimg_error_t *error)
{
	uint32_t key_stored, val_stored;

	if (record->key_len == 0)
		return (pack_error(error, FSIMG_EINVAL, profile->role,
		    "record has no key"));
	if (record->ghost &&
	    (profile->tree_flags & APFS_BTREE_ALLOW_GHOSTS) == 0)
		return (pack_error(error, FSIMG_EINVAL, profile->role,
		    "allows no ghost record"));
	if (profile->fixed_key != 0 && (record->key_len != profile->fixed_key ||
	    (!record->ghost && record->val_len != profile->fixed_val)))
		return (pack_error(error, FSIMG_EINVAL, profile->role,
		    "record does not have the tree's fixed sizes"));

	if (record->key_len > APFS_PACK_CAPACITY ||
	    (!record->ghost && record->val_len > APFS_PACK_CAPACITY))
		return (pack_error(error, FSIMG_EINVAL, profile->role,
		    "record does not fit a node"));
	key_stored = stored_length(profile, (uint32_t)record->key_len);
	val_stored = record->ghost ? 0 :
	    stored_length(profile, (uint32_t)record->val_len);
	if (toc_bytes(profile, true, 1) + key_stored + val_stored >
	    APFS_PACK_CAPACITY ||
	    toc_bytes(profile, false, 2) + 2 * (key_stored +
	    APFS_CHILD_OID_BYTES) > APFS_PACK_CAPACITY)
		return (pack_error(error, FSIMG_EINVAL, profile->role,
		    "record does not fit a node"));
	return (FSIMG_OK);
}

/*
 * Any failure ends the tree: a refused record would otherwise leave a hole
 * in the stream that planning and emission might not share.
 */
fsimg_status_t
apfs_pack_add(apfs_pack_t *pack, const apfs_record_t *record,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (pack->done)
		return (pack_error(error, FSIMG_ESTATE, pack->profile->role,
		    "packer has failed or finished"));
	status = record_check(pack->profile, record, error);
	if (status == FSIMG_OK)
		status = level_add(pack, 0, record, error);
	if (status != FSIMG_OK) {
		pack->done = true;
		return (status);
	}

	pack->result.key_count++;
	if (record->key_len > pack->longest_key)
		pack->longest_key = (uint32_t)record->key_len;
	if (!record->ghost && record->val_len > pack->longest_val)
		pack->longest_val = (uint32_t)record->val_len;
	return (FSIMG_OK);
}

/*
 * Close the open nodes from the leaves up. Closing a level below the top
 * adds an index record to the level above, which may close that level's
 * node and even open a new top, so the loop rereads the height each time;
 * the node that closes with no level above it is the root.
 */
fsimg_status_t
apfs_pack_finish(apfs_pack_t *pack, apfs_pack_result_t *result,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	bool root;

	if (pack->done)
		return (pack_error(error, FSIMG_ESTATE, pack->profile->role,
		    "packer has failed or finished"));
	pack->done = true;
	for (unsigned depth = 0; depth < pack->result.levels; depth++) {
		root = depth + 1 == pack->result.levels;
		status = node_close(pack, depth, root, error);
		if (status != FSIMG_OK)
			return (status);
	}
	*result = pack->result;
	return (FSIMG_OK);
}

void
apfs_pack_free(apfs_pack_t *pack)
{
	if (pack == NULL)
		return;
	for (unsigned i = 0; i < pack->result.levels; i++)
		fsimg_dealloc(pack->levels[i]);
	fsimg_dealloc(pack);
}
