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
 * Structural verification (specs/design-apfs-driver.md, "Verification"):
 * the container pass and the allocation merge.
 *
 * The container pass appends one claim per block range something holds to
 * the claim stream of its domain: the main domain, the container's blocks
 * [0, N), or the internal pool's own domain. Claims are records in metadata
 * scratch, so their number is bounded by the session's scratch budget, not
 * by memory, and every claim comes from a block or record read. Each
 * physical tree is read with a cursor of its own, and every node the cursor
 * loads is claimed as it first appears on the path; the cursor refuses a
 * node repeated within one tree, and a node two trees share is claimed
 * twice and found by the merge.
 *
 * The merge sorts each domain's claims by first block, folds overlapping
 * claims into spans, which only data claims of one volume may form, and
 * walks the spans beside the domain's bitmap: a block is allocated exactly
 * when a span holds it. The space-manager pager proves each chunk's free
 * count and the device's against the bitmap as it reads.
 *
 * Some blocks may be held by objects no claim describes: those of a reap in
 * progress, of a volume whose superblock names a refused feature, of
 * snapshots, whose claims are provisional (E9), and of structures this pass
 * does not read, such as an EFI jumpstart. Any of them makes the report
 * incomplete with FSIMG_EUNSUPPORTED, and the merge is then not made, since
 * it could not tell their blocks from damage; every structure is still read
 * and judged, and damage found outranks the incomplete report.
 *
 * Each volume's filesystem records are reconciled by records.c, which
 * appends every mapped file extent to a stream of its own. The merge clips
 * those extents, sorted by block, to the live physical-extent records, and
 * sweeps the boundaries of the pieces of each record: every block of a
 * record must be mapped as many times as its reference count. The common
 * walk then visits the selected volume.
 */
#define ROLE_VERIFY		"verification"
#define ROLE_EXTENTREF		"extent-reference tree"
#define ROLE_SNAPSHOTS		"snapshot metadata tree"
#define ROLE_FREE_QUEUE		"free queue"
#define ROLE_VOLUME		"volume superblock"

#define TREE_PHYSICAL		(APFS_OBJ_PHYSICAL | APFS_KIND_BTREE_ROOT)
/* A snapshot name of at most 255 bytes, stored with its NUL. */
#define SNAP_NAME_KEY_MAX	(APFS_SNAP_NAME_KEY_BYTES + 256u)
#define SNAP_META_VAL_MAX	(APFS_SNAP_META_VAL_BYTES + 256u)
/* The optional features verification knows (specs/design-apfs.md,
 * "Container superblock" and "Volume superblock"); no read-only-compatible
 * feature is known.
 */
#define NX_FEATURES_KNOWN	(APFS_NX_FEATURE_DEFRAG | APFS_NX_FEATURE_LCFD)
#define FS_FEATURES_KNOWN	0x1full
#define PEXT_KIND_NEW		1u
#define PEXT_KIND_UPDATE	2u

/* A metadata claim overlaps nothing; data claims overlap only data claims
 * of the same volume, which belong to different trees since one tree's
 * records never overlap.
 */
typedef enum {
	CLAIM_METADATA,
	CLAIM_DATA
} claim_class_t;

/* What made a claim, for diagnostics. */
typedef enum {
	SOURCE_FIXED,
	SOURCE_KEYLOCKER,
	SOURCE_OBJECT_MAP,
	SOURCE_MAPPED,
	SOURCE_NODE,
	SOURCE_SPACEMAN,
	SOURCE_FREE_QUEUE,
	SOURCE_EXTENT,
	SOURCES
} claim_source_t;

static const char *const source_names[SOURCES] = {
	[SOURCE_FIXED] = "a fixed region",
	[SOURCE_KEYLOCKER] = "a keylocker",
	[SOURCE_OBJECT_MAP] = "an object map",
	[SOURCE_MAPPED] = "a mapped object",
	[SOURCE_NODE] = "a tree node",
	[SOURCE_SPACEMAN] = "a space-manager block",
	[SOURCE_FREE_QUEUE] = "a free-queue entry",
	[SOURCE_EXTENT] = "a physical extent"
};

/* A claim: blocks [first, first + count) of its domain; a data claim names
 * its volume's slot.
 */
typedef struct claim {
	uint64_t	first;
	uint64_t	count;
	uint32_t	volume;
	uint8_t		class;
	uint8_t		source;
} claim_t;

#define CLAIM_BYTES	22u

/* A claim domain: its blocks [first, end) and its claims. */
typedef struct domain {
	const char	*name;
	fsimg_stream_t	*claims;
	uint64_t	 first;
	uint64_t	 end;
} domain_t;

/*
 * The claims of one domain folded into spans in ascending order: the
 * current span [first, end) while have_span, the claim read past it while
 * have_next, and the sorted stream's read offset.
 */
typedef struct spans {
	fsimg_stream_t	*sorted;
	claim_t		 next;
	uint64_t	 offset;
	uint64_t	 first;
	uint64_t	 end;
	bool		 have_span;
	bool		 have_next;
} spans_t;

/* One snapshot-tree walk: the volume's slot and what the records count. */
typedef struct snapshot_walk {
	uint64_t	count;
	uint64_t	newest;
	uint32_t	volume;
} snapshot_walk_t;

/*
 * The volume whose trees are read: its slot and superblock, its object
 * map's header, what its snapshot tree counts and the blocks its claims
 * cover.
 */
typedef struct volume_walk {
	const apfs_fs_t	*fs;
	apfs_omap_t	 omap;
	snapshot_walk_t	 snapshots;
	uint64_t	 blocks;
	uint32_t	 index;
} volume_walk_t;

typedef struct verify {
	fsimg_reader_t	*reader;
	apfs_reader_t	*apfs;
	apfs_container_t *container;
	unsigned char	*window;
	domain_t	 main;
	domain_t	 pool;
	/* Every volume's live physical-extent records, LIVE_BYTES each in
	 * volume and block order; the mapped file extents records.c finds;
	 * the selected volume's inodes of several names.
	 */
	fsimg_stream_t	*live_extents;
	fsimg_stream_t	*file_extents;
	fsimg_stream_t	*shared;
	/* The verification flags, and whether the selected volume holds a
	 * compressed file whose chunk tables are not read.
	 */
	unsigned	 flags;
	bool		 chunk_tables;
	/* The first reason the report is incomplete; status FSIMG_OK while
	 * there is none.
	 */
	fsimg_error_t	 incomplete;
	/* While a volume's trees are read, that volume and the blocks its
	 * claims cover.
	 */
	volume_walk_t	*volume;
	uint64_t	*tally;
	unsigned char	 uuids[APFS_NX_VOLUMES_MAX][APFS_UUID_BYTES];
	uint32_t	 uuid_count;
} verify_t;

/* A live physical-extent record, for the file-extent join. */
typedef struct live_extent {
	uint64_t	first;
	uint64_t	count;
	uint32_t	volume;
	uint32_t	refcnt;
} live_extent_t;

#define LIVE_BYTES	24u

/*
 * A boundary of the extent sweep, keyed by its volume and the first block
 * of its physical-extent record: the record opens at its first block with
 * its reference count and closes at its end, and each piece of a file
 * extent inside it enters at its first block and leaves at its end.
 */
typedef enum {
	EVENT_OPEN,
	EVENT_ENTER,
	EVENT_LEAVE,
	EVENT_CLOSE
} event_kind_t;

typedef struct event {
	uint64_t	record;
	uint64_t	position;
	uint32_t	volume;
	uint32_t	refcnt;
	uint8_t		kind;
} event_t;

#define EVENT_BYTES	25u
/* An inode of several names, then its mark once the walk has read it. */
#define SHARED_BYTES	9u
#define PAYLOAD_BYTES	65536u

/* One physical-extent tree walk: its owner and the end of its last record. */
typedef struct extent_walk {
	uint64_t	end;
	uint32_t	volume;
	bool		live;
} extent_walk_t;

/* One free-queue walk: its domain and the blocks its records queue. */
typedef struct queue_walk {
	domain_t	*domain;
	uint64_t	 blocks;
} queue_walk_t;

/* One part of the container pass. */
typedef fsimg_status_t (*part_fn_t)(verify_t *, fsimg_error_t *);

typedef fsimg_status_t (*record_fn_t)(verify_t *, const apfs_record_t *,
		    void *, fsimg_error_t *);

static int		snapshot_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);
static int		jkey_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);
static int		pair_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);
static int		claim_compare(const void *, size_t, const void *, size_t,
			    void *);
static void		claim_encode(unsigned char *, const claim_t *);
static void		claim_decode(const unsigned char *, claim_t *);
static fsimg_status_t	claim_add(verify_t *, domain_t *, const claim_t *,
			    fsimg_error_t *);
static fsimg_status_t	metadata_claim(verify_t *, domain_t *, claim_source_t,
			    uint64_t, uint64_t, fsimg_error_t *);
static domain_t		*block_domain(verify_t *, uint64_t);
static fsimg_status_t	block_error(fsimg_error_t *, const domain_t *,
			    uint64_t, const char *);
static void		incomplete_note(verify_t *, const fsimg_error_t *);
static void		incomplete_reason(verify_t *, const char *,
			    const char *);
static fsimg_status_t	tree_walk(verify_t *, apfs_cursor_t *, record_fn_t,
			    void *, fsimg_error_t *);
static fsimg_status_t	map_walk(verify_t *, apfs_map_kind_t, uint64_t,
			    apfs_omap_t *, fsimg_error_t *);
static fsimg_status_t	mapped_record(verify_t *, const apfs_record_t *,
			    void *, fsimg_error_t *);
static fsimg_status_t	extent_record(verify_t *, const apfs_record_t *,
			    void *, fsimg_error_t *);
static fsimg_status_t	extents_walk(verify_t *, uint32_t, bool, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	snapshot_record(verify_t *, const apfs_record_t *,
			    void *, fsimg_error_t *);
static fsimg_status_t	queue_record(verify_t *, const apfs_record_t *,
			    void *, fsimg_error_t *);
static fsimg_status_t	queue_walk(verify_t *, uint32_t, domain_t *,
			    fsimg_error_t *);
static fsimg_status_t	spaceman_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	volume_identity(verify_t *, uint32_t, apfs_fs_t *,
			    fsimg_error_t *);
static fsimg_status_t	volume_map_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	snapshot_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	live_extent_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	volume_records(verify_t *, fsimg_error_t *);
static fsimg_status_t	live_add(verify_t *, const live_extent_t *,
			    fsimg_error_t *);
static fsimg_status_t	live_next(fsimg_stream_t *, uint64_t *,
			    live_extent_t *, bool *, fsimg_error_t *);
static int		fext_compare(const void *, size_t, const void *,
			    size_t, void *);
static int		event_compare(const void *, size_t, const void *,
			    size_t, void *);
static fsimg_status_t	event_add(fsimg_stream_t *, const event_t *,
			    fsimg_error_t *);
static fsimg_status_t	event_next(fsimg_stream_t *, uint64_t *, event_t *,
			    bool *, fsimg_error_t *);
static fsimg_status_t	piece_add(fsimg_stream_t *, const live_extent_t *,
			    uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t	extents_clip(verify_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	records_bound(verify_t *, fsimg_stream_t *,
			    fsimg_error_t *);
static fsimg_status_t	events_sweep(fsimg_stream_t *, fsimg_error_t *);
static fsimg_status_t	extents_merge(verify_t *, fsimg_error_t *);
static fsimg_status_t	shared_load(verify_t *, fsimg_array_t **,
			    uint64_t *, fsimg_error_t *);
static fsimg_status_t	shared_first(fsimg_array_t *, uint64_t, uint64_t,
			    bool *, fsimg_error_t *);
static fsimg_status_t	namespace_walk(verify_t *, unsigned,
			    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t	volume_trees(verify_t *, uint32_t, const apfs_fs_t *,
			    fsimg_error_t *);
static fsimg_status_t	volume_pass(verify_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t	fixed_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	container_map_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	reaper_check(verify_t *, fsimg_error_t *);
static fsimg_status_t	main_queue_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	pool_queue_claims(verify_t *, fsimg_error_t *);
static fsimg_status_t	part_end(verify_t *, fsimg_status_t,
			    const fsimg_error_t *, fsimg_error_t *);
static fsimg_status_t	parts_run(verify_t *, const part_fn_t *, size_t,
			    bool *, fsimg_error_t *);
static fsimg_status_t	container_pass(verify_t *, fsimg_error_t *);
static fsimg_status_t	spans_open(domain_t *, spans_t *, fsimg_error_t *);
static fsimg_status_t	spans_read(spans_t *, claim_t *, bool *,
			    fsimg_error_t *);
static fsimg_status_t	spans_next(spans_t *, const domain_t *,
			    fsimg_error_t *);
static fsimg_status_t	spans_finish(spans_t *, const domain_t *, uint64_t,
			    fsimg_error_t *);
static uint64_t		bits_mismatch(const unsigned char *, uint64_t,
			    uint64_t, bool);
static fsimg_status_t	bitmap_compare(const domain_t *, spans_t *,
			    const unsigned char *, uint64_t, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	main_merge(verify_t *, apfs_sm_pager_t *,
			    fsimg_error_t *);
static fsimg_status_t	pool_merge(verify_t *, apfs_sm_pager_t *,
			    fsimg_error_t *);
static fsimg_status_t	allocation_merge(verify_t *, fsimg_error_t *);

/* The extent-reference tree: bare keys of kind 2, ordered by block. */
static const apfs_tree_profile_t extentref_tree = {
	.role = ROLE_EXTENTREF,
	.compare = jkey_compare,
	.subtype = APFS_SUBTYPE_EXTENTREF,
	.key_min = APFS_JKEY_BYTES, .key_max = APFS_JKEY_BYTES,
	.val_min = APFS_PEXT_VAL_BYTES, .val_max = APFS_PEXT_VAL_BYTES
};

/* The snapshot metadata tree: metadata records by XID, then names. */
static const apfs_tree_profile_t snapshot_tree = {
	.role = ROLE_SNAPSHOTS,
	.compare = snapshot_compare,
	.subtype = APFS_SUBTYPE_SNAPMETA,
	.key_min = APFS_JKEY_BYTES, .key_max = SNAP_NAME_KEY_MAX,
	.val_min = APFS_SNAP_NAME_VAL_BYTES, .val_max = SNAP_META_VAL_MAX
};

/* A free queue: (XID, block) keys and a block count, or a ghost for one. */
static const apfs_tree_profile_t queue_tree = {
	.role = ROLE_FREE_QUEUE,
	.compare = pair_compare,
	.subtype = APFS_SUBTYPE_FREE_QUEUE,
	.key_min = APFS_FQ_KEY_BYTES, .key_max = APFS_FQ_KEY_BYTES,
	.val_min = APFS_FQ_VAL_BYTES, .val_max = APFS_FQ_VAL_BYTES,
	.fixed_key = APFS_FQ_KEY_BYTES, .fixed_val = APFS_FQ_VAL_BYTES
};

/* Keys led by a record header order by its OID (low 60 bits), then kind. */
static int
jkey_compare(const unsigned char *a, size_t alen, const unsigned char *b,
    size_t blen)
{
	uint64_t x = fsimg_load_le64(a), y = fsimg_load_le64(b);
	uint64_t xo = x & APFS_JKEY_OID_MASK, yo = y & APFS_JKEY_OID_MASK;

	(void)alen;
	(void)blen;
	if (xo != yo)
		return (xo < yo ? -1 : 1);
	x >>= APFS_JKEY_KIND_SHIFT;
	y >>= APFS_JKEY_KIND_SHIFT;
	return (x < y ? -1 : x > y);
}

/*
 * Snapshot keys order as record headers, then name records by their stored
 * name bytes with the terminating NUL, as attribute names do; that names
 * of different lengths compare this way is provisional (E9). A name key
 * shorter than its fixed part orders before every name of its header.
 */
static int
snapshot_compare(const unsigned char *a, size_t alen, const unsigned char *b,
    size_t blen)
{
	const size_t at = APFS_SNAP_NAME_KEY_BYTES;
	size_t common;
	int order;

	order = jkey_compare(a, alen, b, blen);
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

/* Two u64 integers, the first deciding. */
static int
pair_compare(const unsigned char *a, size_t alen, const unsigned char *b,
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

/* Claims sort by first block, then count. */
static int
claim_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *arg)
{
	claim_t x, y;

	(void)alen;
	(void)blen;
	(void)arg;
	claim_decode(a, &x);
	claim_decode(b, &y);
	if (x.first != y.first)
		return (x.first < y.first ? -1 : 1);
	return (x.count < y.count ? -1 : x.count > y.count);
}

static void
claim_encode(unsigned char *raw, const claim_t *claim)
{
	fsimg_store_le64(raw, claim->first);
	fsimg_store_le64(raw + 8, claim->count);
	fsimg_store_le32(raw + 16, claim->volume);
	raw[20] = claim->class;
	raw[21] = claim->source;
}

static void
claim_decode(const unsigned char *raw, claim_t *claim)
{
	claim->first = fsimg_load_le64(raw);
	claim->count = fsimg_load_le64(raw + 8);
	claim->volume = fsimg_load_le32(raw + 16);
	claim->class = raw[20];
	claim->source = raw[21];
}

/*
 * Append a claim, which must be nonempty and lie inside its domain, and
 * only a fixed region may hold block 0; the comparison is arranged so
 * first + count is never formed. A volume's tally adds the blocks of every
 * claim made while it is read.
 */
static fsimg_status_t
claim_add(verify_t *v, domain_t *domain, const claim_t *claim,
    fsimg_error_t *error)
{
	unsigned char raw[CLAIM_BYTES];
	char message[160];

	if (claim->count == 0) {
		snprintf(message, sizeof(message), "finds %s of no blocks",
		    source_names[claim->source]);
		return (apfs_corrupt(error, ROLE_VERIFY, message));
	}
	if (claim->first < domain->first || claim->first >= domain->end ||
	    claim->count > domain->end - claim->first) {
		snprintf(message, sizeof(message), "finds %s outside the %s "
		    "at block %llu", source_names[claim->source], domain->name,
		    (unsigned long long)claim->first);
		return (apfs_corrupt(error, ROLE_VERIFY, message));
	}
	if (claim->first == 0 && claim->source != SOURCE_FIXED) {
		snprintf(message, sizeof(message), "finds %s at block 0",
		    source_names[claim->source]);
		return (apfs_corrupt(error, ROLE_VERIFY, message));
	}
	/* Snapshot trees may claim one block many times, so the tally
	 * saturates; a saturated tally never equals a count below N.
	 */
	if (v->tally != NULL)
		*v->tally = claim->count > UINT64_MAX - *v->tally ?
		    UINT64_MAX : *v->tally + claim->count;
	claim_encode(raw, claim);
	return (fsimg_stream_append(domain->claims, raw, sizeof(raw), NULL,
	    error));
}

static fsimg_status_t
metadata_claim(verify_t *v, domain_t *domain, claim_source_t source,
    uint64_t first, uint64_t count, fsimg_error_t *error)
{
	claim_t claim = { .first = first, .count = count,
	    .class = CLAIM_METADATA, .source = (uint8_t)source };

	return (claim_add(v, domain, &claim, error));
}

/* The domain a space-manager block belongs to: the pool's when inside it. */
static domain_t *
block_domain(verify_t *v, uint64_t block)
{
	if (block >= v->pool.first && block < v->pool.end)
		return (&v->pool);
	return (&v->main);
}

static fsimg_status_t
block_error(fsimg_error_t *error, const domain_t *domain, uint64_t block,
    const char *what)
{
	char message[160];

	snprintf(message, sizeof(message), "finds block %llu of the %s %s",
	    (unsigned long long)block, domain->name, what);
	return (apfs_corrupt(error, ROLE_VERIFY, message));
}

/* Keep the first reason the report is incomplete. */
static void
incomplete_note(verify_t *v, const fsimg_error_t *reason)
{
	if (v->incomplete.status == FSIMG_OK)
		v->incomplete = *reason;
}

static void
incomplete_reason(verify_t *v, const char *role, const char *what)
{
	fsimg_error_t reason;

	bzero(&reason, sizeof(reason));
	(void)apfs_unsupported(&reason, role, what);
	incomplete_note(v, &reason);
}

/*
 * Walk a tree from its first record, calling fn, when given, on each
 * record, and compare the nodes and records the walk met with the footer's
 * counts. The nodes of a physical tree are claimed: each node on the path
 * the first time it appears at its level, which an in-order walk reaches
 * once. An ephemeral tree's nodes lie in the checkpoint data area, which
 * its fixed region claims.
 */
static fsimg_status_t
tree_walk(verify_t *v, apfs_cursor_t *cursor, record_fn_t fn, void *arg,
    fsimg_error_t *error)
{
	uint64_t seen[APFS_BTREE_LEVELS_MAX] = { 0 };
	bool physical = cursor->storage == APFS_OBJ_PHYSICAL;
	apfs_record_t record;
	uint64_t keys = 0;
	fsimg_status_t status;
	bool found;

	status = apfs_cursor_first(cursor, &found, error);
	while (status == FSIMG_OK) {
		for (unsigned d = 0; physical && d < cursor->depth; d++) {
			if (cursor->path[d].oid == seen[d])
				continue;
			seen[d] = cursor->path[d].oid;
			status = metadata_claim(v, &v->main, SOURCE_NODE,
			    seen[d], 1, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (!found)
			return (apfs_cursor_counts_check(cursor, keys, error));
		keys++;
		if (fn != NULL) {
			apfs_cursor_record(cursor, &record);
			status = fn(v, &record, arg, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = apfs_cursor_next(cursor, &found, error);
	}
	return (status);
}

/*
 * Every live version an object map maps, at its size: tombstones are
 * skipped. A full scan has no lookup XID to stop at, so every record's XID,
 * a tombstone's too, must be nonzero and not above the checkpoint's. An
 * encrypted or headerless object, which a lookup refuses, is still judged
 * and claimed at its size, and makes the report incomplete.
 */
static fsimg_status_t
mapped_record(verify_t *v, const apfs_record_t *record, void *arg,
    fsimg_error_t *error)
{
	const char *role = arg;
	uint32_t block_size = v->container->block_size;
	apfs_omap_key_t key;
	apfs_omap_val_t value;

	if (record->ghost ||
	    !apfs_omap_key_decode(record->key, record->key_len, &key) ||
	    !apfs_omap_val_decode(record->val, record->val_len, &value))
		return (apfs_corrupt(error, role, "has a malformed record"));
	if (key.xid == 0)
		return (apfs_corrupt(error, role, "maps a version of XID zero"));
	if (key.xid > v->container->xid)
		return (apfs_corrupt(error, role,
		    "maps a version newer than the checkpoint"));
	if ((value.flags & APFS_OMAP_VAL_DELETED) != 0)
		return (FSIMG_OK);
	if ((value.flags & ~(APFS_OMAP_VAL_SAVED | APFS_OMAP_VAL_ENCRYPTED |
	    APFS_OMAP_VAL_NOHEADER)) != 0)
		return (apfs_corrupt(error, role, "has invalid value flags"));
	if (value.size == 0 || value.size % block_size != 0)
		return (apfs_corrupt(error, role,
		    "maps an object of an invalid size"));
	if ((value.flags & (APFS_OMAP_VAL_ENCRYPTED |
	    APFS_OMAP_VAL_NOHEADER)) != 0)
		incomplete_reason(v, role,
		    "maps an encrypted or headerless object");
	return (metadata_claim(v, &v->main, SOURCE_MAPPED, value.address,
	    value.size / block_size, error));
}

/*
 * Claim an object map, its tree's nodes and the versions it maps, reading
 * it at the checkpoint's XID through a cursor of its own. The decoded
 * header is returned in omap.
 */
static fsimg_status_t
map_walk(verify_t *v, apfs_map_kind_t kind, uint64_t oid, apfs_omap_t *omap,
    fsimg_error_t *error)
{
	apfs_container_t *container = v->container;
	apfs_map_t map;
	fsimg_status_t status;

	status = apfs_map_open(container, kind, oid, container->xid,
	    v->window, &map, error);
	if (status != FSIMG_OK)
		return (status);
	*omap = map.omap;
	status = metadata_claim(v, &v->main, SOURCE_OBJECT_MAP, oid, 1, error);
	if (status == FSIMG_OK)
		status = tree_walk(v, &map.cursor, mapped_record,
		    (void *)map.cursor.profile->role, error);
	apfs_map_close(&map);
	return (status);
}

/*
 * A physical-extent record: a data claim of its volume. Records of one
 * tree never overlap, which keeps same-tree claims out of the merge's
 * folding. A live record is of kind new with a positive count, and is kept
 * for the file-extent join; how a snapshot's tree records them is
 * provisional (E9), so only the kind's range is judged there.
 */
static fsimg_status_t
extent_record(verify_t *v, const apfs_record_t *record, void *arg,
    fsimg_error_t *error)
{
	extent_walk_t *walk = arg;
	apfs_jkey_t key;
	apfs_pext_val_t value;
	claim_t claim;
	fsimg_status_t status;

	if (record->ghost ||
	    !apfs_jkey_decode(record->key, record->key_len, &key) ||
	    key.kind != APFS_J_PHYS_EXTENT ||
	    !apfs_pext_val_decode(record->val, record->val_len, &value))
		return (apfs_corrupt(error, ROLE_EXTENTREF,
		    "has a malformed record"));
	if ((value.kind != PEXT_KIND_NEW && value.kind != PEXT_KIND_UPDATE) ||
	    (walk->live && (value.kind != PEXT_KIND_NEW ||
	    value.refcnt <= 0)))
		return (apfs_corrupt(error, ROLE_EXTENTREF,
		    "has a record of an invalid kind or count"));
	if (key.oid < walk->end)
		return (apfs_corrupt(error, ROLE_EXTENTREF,
		    "has overlapping records"));
	claim = (claim_t){ .first = key.oid, .count = value.length,
	    .volume = walk->volume, .class = CLAIM_DATA,
	    .source = SOURCE_EXTENT };
	status = claim_add(v, &v->main, &claim, error);
	if (status == FSIMG_OK && walk->live)
		status = live_add(v, &(live_extent_t){ .first = key.oid,
		    .count = value.length, .volume = walk->volume,
		    .refcnt = (uint32_t)value.refcnt }, error);
	if (status == FSIMG_OK)
		walk->end = key.oid + value.length;
	return (status);
}

/* One extent-reference tree of a volume, the live one or a snapshot's. */
static fsimg_status_t
extents_walk(verify_t *v, uint32_t volume, bool live, uint64_t root,
    fsimg_error_t *error)
{
	extent_walk_t walk = { .volume = volume, .live = live };
	apfs_cursor_t cursor;
	fsimg_status_t status;

	apfs_cursor_init(&cursor, v->container, &extentref_tree,
	    APFS_OBJ_PHYSICAL, root, v->container->xid, NULL);
	status = tree_walk(v, &cursor, extent_record, &walk, error);
	apfs_cursor_free(&cursor);
	return (status);
}

/*
 * A snapshot-tree record: a metadata record's extent-reference tree is
 * walked; a name record is checked for shape. The snapshot XID either one
 * gives is not above the checkpoint's.
 */
static fsimg_status_t
snapshot_record(verify_t *v, const apfs_record_t *record, void *arg,
    fsimg_error_t *error)
{
	snapshot_walk_t *walk = arg;
	apfs_snap_meta_val_t meta;
	apfs_name_key_t name;
	apfs_jkey_t key;
	uint64_t xid;

	if (record->ghost ||
	    !apfs_jkey_decode(record->key, record->key_len, &key))
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "has a malformed record"));
	if (key.kind == APFS_J_SNAP_NAME) {
		if (!apfs_snap_name_key_decode(record->key, record->key_len,
		    &name) ||
		    !apfs_snap_name_val_decode(record->val, record->val_len,
		    &xid))
			return (apfs_corrupt(error, ROLE_SNAPSHOTS,
			    "has a malformed name record"));
		if (xid > v->container->xid)
			return (apfs_corrupt(error, ROLE_SNAPSHOTS,
			    "names a snapshot newer than the checkpoint"));
		return (FSIMG_OK);
	}
	if (key.kind != APFS_J_SNAP_METADATA ||
	    record->key_len != APFS_JKEY_BYTES ||
	    !apfs_snap_meta_val_decode(record->val, record->val_len, &meta))
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "has a malformed metadata record"));
	if (key.oid > v->container->xid)
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "names a snapshot newer than the checkpoint"));
	if (meta.extentref_tree_type != TREE_PHYSICAL)
		return (apfs_corrupt(error, ROLE_SNAPSHOTS,
		    "names an invalid extent-reference tree"));
	walk->count++;
	walk->newest = key.oid;
	return (extents_walk(v, walk->volume, false, meta.extentref_tree_oid,
	    error));
}

/*
 * Read the superblock of the volume in slot index and apply its own rules.
 * Its refusal tables come after the rules that identify it, which include
 * a nonzero UUID, so a refused volume whose UUID was read, still zero when
 * the refusal came before decoding, takes part in the requirement that no
 * two volumes share a UUID; the refusal is returned. An unknown optional or
 * read-only-compatible feature, which reading ignores, makes the report
 * incomplete.
 */
static fsimg_status_t
volume_identity(verify_t *v, uint32_t index, apfs_fs_t *fs,
    fsimg_error_t *error)
{
	size_t name_bytes;
	fsimg_status_t status;

	static const unsigned char unread[APFS_UUID_BYTES];

	bzero(fs, sizeof(*fs));
	status = apfs_volume_read(v->container, &v->apfs->container_map,
	    index, v->container->xid, v->window, fs, &name_bytes, error);
	if (status != FSIMG_OK && status != FSIMG_EUNSUPPORTED)
		return (status);
	if (memcmp(fs->uuid, unread, APFS_UUID_BYTES) == 0)
		return (status);
	for (uint32_t i = 0; i < v->uuid_count; i++) {
		if (memcmp(v->uuids[i], fs->uuid, APFS_UUID_BYTES) == 0)
			return (apfs_corrupt(error, ROLE_VOLUME,
			    "has the UUID of another volume"));
	}
	bcopy(fs->uuid, v->uuids[v->uuid_count++], APFS_UUID_BYTES);
	if (status == FSIMG_OK && ((fs->features &
	    ~(uint64_t)FS_FEATURES_KNOWN) != 0 || fs->ro_compat_features != 0))
		incomplete_reason(v, ROLE_VOLUME, "has an unknown optional or "
		    "read-only-compatible feature");
	return (status);
}


/* The volume's object map, its nodes and the versions it maps. */
static fsimg_status_t
volume_map_claims(verify_t *v, fsimg_error_t *error)
{
	return (map_walk(v, APFS_MAP_VOLUME, v->volume->fs->omap_oid,
	    &v->volume->omap, error));
}

/* The volume's snapshot tree and each snapshot's extent-reference tree. */
static fsimg_status_t
snapshot_claims(verify_t *v, fsimg_error_t *error)
{
	volume_walk_t *volume = v->volume;
	apfs_cursor_t cursor;
	fsimg_status_t status;

	apfs_cursor_init(&cursor, v->container, &snapshot_tree,
	    APFS_OBJ_PHYSICAL, volume->fs->snapshot_tree_oid, v->container->xid,
	    NULL);
	status = tree_walk(v, &cursor, snapshot_record, &volume->snapshots,
	    error);
	apfs_cursor_free(&cursor);
	return (status);
}

static fsimg_status_t
live_extent_claims(verify_t *v, fsimg_error_t *error)
{
	return (extents_walk(v, v->volume->index, true,
	    v->volume->fs->extentref_tree_oid, error));
}

/*
 * The volume's filesystem records (records.c). A sealed volume keeps its
 * file extents in a tree this pass does not read, which already makes the
 * report incomplete, so its records are not reconciled. With payload
 * read-through the selected volume lists its inodes of several names for
 * the walk, and it notes a compressed file whose chunk tables are not
 * read; that makes the report incomplete after the allocation merge, since
 * those tables hide no blocks from it.
 */
static fsimg_status_t
volume_records(verify_t *v, fsimg_error_t *error)
{
	const volume_walk_t *volume = v->volume;
	bool selected = volume->index == v->apfs->volume_index;
	apfs_records_t records = { .extents = v->file_extents };
	fsimg_status_t status;

	if (volume->fs->fext_tree_oid != 0)
		return (FSIMG_OK);
	if (selected && (v->flags & FSIMG_VERIFY_PAYLOAD) != 0)
		records.shared = v->shared;
	status = apfs_records_check(v->container, volume->fs, volume->index,
	    v->window, &records, error);
	if (selected && records.chunk_tables)
		v->chunk_tables = true;
	return (status);
}

/*
 * A volume's trees: its object map, snapshot tree and extent-reference
 * trees, each a part of its own, so a refusal in one leaves the others
 * read. The checks that join them need all three whole. Without
 * snapshots, the allocated block count must equal the blocks its claims
 * cover, the object-map object, which E3 counts as the one block more,
 * among them; with them that count is provisional (E9), as are their
 * claims, and the report is incomplete.
 */
static fsimg_status_t
volume_trees(verify_t *v, uint32_t index, const apfs_fs_t *fs,
    fsimg_error_t *error)
{
	static const part_fn_t parts[] = { volume_map_claims,
	    snapshot_claims, live_extent_claims, volume_records };
	volume_walk_t walk = { .fs = fs, .index = index,
	    .snapshots = { .volume = index } };
	fsimg_status_t status;
	bool whole;

	if (fs->fext_tree_oid != 0 || fs->integrity_oid != 0 ||
	    fs->snapshot_ext_oid != 0)
		incomplete_reason(v, ROLE_VOLUME,
		    "holds structures verification does not read");
	v->volume = &walk;
	v->tally = &walk.blocks;
	status = parts_run(v, parts, sizeof(parts) / sizeof(parts[0]),
	    &whole, error);
	v->tally = NULL;
	v->volume = NULL;
	if (status != FSIMG_OK || !whole)
		return (status);
	if (walk.omap.snapshot_count != walk.snapshots.count ||
	    walk.omap.most_recent_snapshot != walk.snapshots.newest)
		return (apfs_corrupt(error, "volume object map",
		    "disagrees with the snapshot tree"));
	if (walk.snapshots.count != 0 || walk.omap.snapshot_tree_oid != 0) {
		incomplete_reason(v, ROLE_VOLUME, "has snapshots, whose "
		    "allocation verification does not judge");
		return (FSIMG_OK);
	}
	if (fs->alloc_blocks != walk.blocks)
		return (apfs_corrupt(error, ROLE_VOLUME,
		    "allocated block count differs from its trees"));
	return (FSIMG_OK);
}

/*
 * One volume, a part of the container pass. A refusal from its
 * superblock's feature tables leaves its trees unread, since their format
 * may depend on the refused feature.
 */
static fsimg_status_t
volume_pass(verify_t *v, uint32_t index, fsimg_error_t *error)
{
	apfs_fs_t fs;
	fsimg_status_t status;

	status = volume_identity(v, index, &fs, error);
	if (status == FSIMG_OK)
		status = volume_trees(v, index, &fs, error);
	return (status);
}

/* A free-queue record queues its run, or one block for a ghost. */
static fsimg_status_t
queue_record(verify_t *v, const apfs_record_t *record, void *arg,
    fsimg_error_t *error)
{
	queue_walk_t *walk = arg;
	apfs_fq_key_t key;
	uint64_t count = 1;
	fsimg_status_t status;

	if (!apfs_fq_key_decode(record->key, record->key_len, &key) ||
	    (!record->ghost &&
	    !apfs_fq_val_decode(record->val, record->val_len, &count)))
		return (apfs_corrupt(error, ROLE_FREE_QUEUE,
		    "has a malformed record"));
	if (key.xid > v->container->xid)
		return (apfs_corrupt(error, ROLE_FREE_QUEUE,
		    "has a record newer than the checkpoint"));
	status = metadata_claim(v, walk->domain, SOURCE_FREE_QUEUE,
	    key.address, count, error);
	if (status != FSIMG_OK)
		return (status);
	if (count > UINT64_MAX - walk->blocks)
		return (apfs_corrupt(error, ROLE_FREE_QUEUE,
		    "queues more blocks than a count holds"));
	walk->blocks += count;
	return (FSIMG_OK);
}

/*
 * A free queue's entries, claims in its domain. An empty queue is a zero
 * tree OID with a zero count, or a tree with no records; its nodes are
 * ephemeral objects the checkpoint data area's claim covers.
 */
static fsimg_status_t
queue_walk(verify_t *v, uint32_t queue, domain_t *domain,
    fsimg_error_t *error)
{
	const apfs_sm_fq_t *fq = &v->container->spaceman.free_queues[queue];
	queue_walk_t walk = { .domain = domain };
	apfs_cursor_t cursor;
	fsimg_status_t status;

	if (fq->oldest_xid > v->container->xid)
		return (apfs_corrupt(error, ROLE_FREE_QUEUE,
		    "has an oldest XID newer than the checkpoint"));
	if (fq->tree_oid == 0) {
		if (fq->count != 0)
			return (apfs_corrupt(error, ROLE_FREE_QUEUE,
			    "counts blocks but has no tree"));
		return (FSIMG_OK);
	}
	apfs_cursor_init(&cursor, v->container, &queue_tree,
	    APFS_OBJ_EPHEMERAL, fq->tree_oid, v->container->xid, NULL);
	status = tree_walk(v, &cursor, queue_record, &walk, error);
	apfs_cursor_free(&cursor);
	if (status == FSIMG_OK && walk.blocks != fq->count)
		return (apfs_corrupt(error, ROLE_FREE_QUEUE,
		    "count differs from its records"));
	return (status);
}

/*
 * The space manager's chunk-information, address and bitmap blocks, found
 * by a pager pass that reads no bitmap, each claimed in its domain.
 */
static fsimg_status_t
spaceman_claims(verify_t *v, fsimg_error_t *error)
{
	apfs_sm_pager_t *pager = NULL;
	apfs_sm_chunk_t chunk;
	uint64_t bitmap;
	fsimg_status_t status;
	bool found;

	status = apfs_sm_pager_open(v->container, &pager, error);
	while (status == FSIMG_OK) {
		status = apfs_sm_pager_next(pager, &chunk, &found, error);
		if (status != FSIMG_OK || !found)
			break;
		if (chunk.first_of_cib)
			status = metadata_claim(v,
			    block_domain(v, chunk.cib_address),
			    SOURCE_SPACEMAN, chunk.cib_address, 1, error);
		if (status == FSIMG_OK && chunk.first_of_cab &&
		    chunk.cab_address != 0)
			status = metadata_claim(v,
			    block_domain(v, chunk.cab_address),
			    SOURCE_SPACEMAN, chunk.cab_address, 1, error);
		bitmap = chunk.info.bitmap_address;
		if (status == FSIMG_OK && bitmap != 0)
			status = metadata_claim(v, block_domain(v, bitmap),
			    SOURCE_SPACEMAN, bitmap, 1, error);
	}
	apfs_sm_pager_free(pager);
	return (status);
}

/* The fixed regions and the nonzero keylocker ranges. */
static fsimg_status_t
fixed_claims(verify_t *v, fsimg_error_t *error)
{
	const apfs_nx_t *nx = &v->container->nx;
	const apfs_range_t *lockers[] = { &nx->keylocker,
	    &nx->media_keylocker };
	apfs_region_t regions[APFS_FIXED_REGIONS];
	fsimg_status_t status;

	status = apfs_fixed_regions(nx, &v->container->spaceman, regions,
	    error);
	for (size_t i = 0; status == FSIMG_OK && i < APFS_FIXED_REGIONS; i++)
		status = metadata_claim(v, &v->main, SOURCE_FIXED,
		    regions[i].first, regions[i].count, error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(lockers) / sizeof(lockers[0]); i++) {
		if (lockers[i]->count != 0)
			status = metadata_claim(v, &v->main, SOURCE_KEYLOCKER,
			    lockers[i]->start, lockers[i]->count, error);
	}
	return (status);
}

/* The container object map, its nodes and the versions it maps. */
static fsimg_status_t
container_map_claims(verify_t *v, fsimg_error_t *error)
{
	apfs_omap_t omap;

	return (map_walk(v, APFS_MAP_CONTAINER, v->container->nx.omap_oid,
	    &omap, error));
}

/* The reaper contributes no claim; one that is not idle makes the report
 * incomplete.
 */
static fsimg_status_t
reaper_check(verify_t *v, fsimg_error_t *error)
{
	fsimg_status_t status;
	bool idle;

	status = apfs_reaper_idle(v->container, v->window, &idle, error);
	if (status == FSIMG_OK && !idle)
		incomplete_reason(v, "reaper",
		    "has a pending reap, so verification is incomplete");
	return (status);
}

/* The free queues, each in its domain. */
static fsimg_status_t
main_queue_claims(verify_t *v, fsimg_error_t *error)
{
	return (queue_walk(v, APFS_SM_FQ_MAIN, &v->main, error));
}

static fsimg_status_t
pool_queue_claims(verify_t *v, fsimg_error_t *error)
{
	return (queue_walk(v, APFS_SM_FQ_INTERNAL_POOL, &v->pool, error));
}

/*
 * End one part of the container pass on its status and the detail it
 * reported. A refusal makes the report incomplete and the pass goes on to
 * the next part, so every structure outside the refused one is read and
 * damage found later still outranks the incomplete report; any other
 * failure ends the pass.
 */
static fsimg_status_t
part_end(verify_t *v, fsimg_status_t status, const fsimg_error_t *detail,
    fsimg_error_t *error)
{
	if (status == FSIMG_EUNSUPPORTED) {
		incomplete_note(v, detail);
		return (FSIMG_OK);
	}
	if (status != FSIMG_OK && error != NULL)
		*error = *detail;
	return (status);
}

/*
 * Run parts that depend on nothing an earlier one reads, each ending
 * through part_end(); whole reports whether every part completed
 * unrefused.
 */
static fsimg_status_t
parts_run(verify_t *v, const part_fn_t *parts, size_t count, bool *whole,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_status_t status = FSIMG_OK;

	*whole = true;
	for (size_t i = 0; status == FSIMG_OK && i < count; i++) {
		bzero(&detail, sizeof(detail));
		status = parts[i](v, &detail);
		if (status == FSIMG_EUNSUPPORTED)
			*whole = false;
		status = part_end(v, status, &detail, error);
	}
	return (status);
}

/*
 * Every claim of the container pass, in the design's order, made by parts
 * that depend on nothing an earlier part reads, so that each ends through
 * part_end(). The checkpoint's objects are the ones open selected and
 * checked: its superblock, mappings and space manager.
 */
static fsimg_status_t
container_pass(verify_t *v, fsimg_error_t *error)
{
	static const part_fn_t parts[] = { fixed_claims,
	    container_map_claims, reaper_check, spaceman_claims,
	    main_queue_claims, pool_queue_claims };
	const apfs_nx_t *nx = &v->container->nx;
	fsimg_error_t detail;
	fsimg_status_t status;
	bool whole;

	if ((nx->features & ~(uint64_t)NX_FEATURES_KNOWN) != 0 ||
	    nx->ro_compat_features != 0)
		incomplete_reason(v, "container superblock", "has an unknown "
		    "optional or read-only-compatible feature");
	if (nx->efi_jumpstart != 0)
		incomplete_reason(v, "container superblock",
		    "has an EFI jumpstart, which verification does not read");
	status = parts_run(v, parts, sizeof(parts) / sizeof(parts[0]), &whole,
	    error);
	for (uint32_t i = 0; status == FSIMG_OK && i < nx->max_volumes; i++) {
		if (nx->volume_oids[i] == 0)
			continue;
		bzero(&detail, sizeof(detail));
		status = volume_pass(v, i, &detail);
		status = part_end(v, status, &detail, error);
	}
	return (status);
}

/*
 * Seal and sort a domain's claims, release the unsorted copy and hold the
 * first span.
 */
static fsimg_status_t
spans_open(domain_t *domain, spans_t *spans, fsimg_error_t *error)
{
	fsimg_status_t status;

	bzero(spans, sizeof(*spans));
	status = fsimg_stream_seal(domain->claims, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(domain->claims, claim_compare, NULL,
		    NULL, &spans->sorted, error);
	fsimg_stream_free(domain->claims);
	domain->claims = NULL;
	if (status == FSIMG_OK)
		status = spans_read(spans, &spans->next, &spans->have_next,
		    error);
	if (status == FSIMG_OK)
		status = spans_next(spans, domain, error);
	return (status);
}

/* The next sorted claim; read reports whether there was one. */
static fsimg_status_t
spans_read(spans_t *spans, claim_t *claim, bool *read, fsimg_error_t *error)
{
	unsigned char raw[CLAIM_BYTES];
	size_t length;
	fsimg_status_t status;

	*read = false;
	status = fsimg_stream_next(spans->sorted, &spans->offset, raw,
	    sizeof(raw), &length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != CLAIM_BYTES)
		return (apfs_corrupt(error, ROLE_VERIFY,
		    "reads a claim record of the wrong size"));
	claim_decode(raw, claim);
	*read = true;
	return (FSIMG_OK);
}

/*
 * Make the claim read past the current span the next span, folding into it
 * every claim that starts before its end. Two claims may share a block
 * only when both are data claims of one volume; folding compares each
 * claim with the span's first, so a span holding a metadata claim holds
 * that claim alone.
 */
static fsimg_status_t
spans_next(spans_t *spans, const domain_t *domain, fsimg_error_t *error)
{
	claim_t span, claim = { 0 };
	uint64_t end;
	fsimg_status_t status = FSIMG_OK;
	char message[200];

	spans->have_span = spans->have_next;
	if (!spans->have_span)
		return (FSIMG_OK);
	span = spans->next;
	end = span.first + span.count;
	for (;;) {
		status = spans_read(spans, &claim, &spans->have_next, error);
		if (status != FSIMG_OK || !spans->have_next ||
		    claim.first >= end)
			break;
		if (span.class != CLAIM_DATA || claim.class != CLAIM_DATA ||
		    span.volume != claim.volume) {
			snprintf(message, sizeof(message), "finds block %llu "
			    "of the %s claimed by both %s and %s",
			    (unsigned long long)claim.first, domain->name,
			    source_names[span.source],
			    source_names[claim.source]);
			return (apfs_corrupt(error, ROLE_VERIFY, message));
		}
		if (claim.first + claim.count > end)
			end = claim.first + claim.count;
	}
	spans->next = claim;
	spans->first = span.first;
	spans->end = end;
	return (status);
}

/*
 * After a domain's bitmap walk, which compared its blocks below covered,
 * every span must lie below covered.
 */
static fsimg_status_t
spans_finish(spans_t *spans, const domain_t *domain, uint64_t covered,
    fsimg_error_t *error)
{
	fsimg_status_t status = FSIMG_OK;

	while (status == FSIMG_OK && spans->have_span && spans->end <= covered)
		status = spans_next(spans, domain, error);
	if (status == FSIMG_OK && spans->have_span)
		status = block_error(error, domain, spans->first,
		    "claimed past its bitmap");
	return (status);
}

/*
 * The first index in [from, to) whose bit is not set (when set) or not
 * clear, or to. Bit i is bit i % 8 of byte i / 8; whole bytes are compared
 * where aligned. A NULL bitmap is clear.
 */
static uint64_t
bits_mismatch(const unsigned char *bits, uint64_t from, uint64_t to,
    bool set)
{
	unsigned char whole = set ? 0xff : 0x00;

	if (bits == NULL)
		return (set ? from : to);
	for (uint64_t i = from; i < to; ) {
		if ((i & 7) == 0 && to - i >= 8 && bits[i >> 3] == whole) {
			i += 8;
			continue;
		}
		if ((((bits[i >> 3] >> (i & 7)) & 1) != 0) != set)
			return (i);
		i++;
	}
	return (to);
}

/*
 * Compare blocks [first, end) of a domain with a bitmap whose bit 0 stands
 * for block first: a block is set exactly when a span holds it. Each step
 * covers a run that lies wholly inside a span or wholly between spans.
 */
static fsimg_status_t
bitmap_compare(const domain_t *domain, spans_t *spans,
    const unsigned char *bits, uint64_t first, uint64_t end,
    fsimg_error_t *error)
{
	uint64_t block = first, stop, wrong;
	fsimg_status_t status;
	bool inside;

	while (block < end) {
		while (spans->have_span && spans->end <= block) {
			status = spans_next(spans, domain, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (!spans->have_span) {
			inside = false;
			stop = end;
		} else if (spans->first <= block) {
			inside = true;
			stop = spans->end;
		} else {
			inside = false;
			stop = spans->first;
		}
		if (stop > end)
			stop = end;
		wrong = bits_mismatch(bits, block - first, stop - first,
		    inside);
		if (wrong != stop - first)
			return (block_error(error, domain, first + wrong,
			    inside ? "claimed but free" :
			    "allocated but claimed by nothing"));
		block = stop;
	}
	return (FSIMG_OK);
}

/* The main domain's spans beside each chunk's bitmap; bits past N are not
 * judged.
 */
static fsimg_status_t
main_merge(verify_t *v, apfs_sm_pager_t *pager, fsimg_error_t *error)
{
	const unsigned char *bits;
	apfs_sm_chunk_t chunk;
	spans_t spans;
	uint64_t first, end, covered = v->main.first;
	fsimg_status_t status;
	bool found;

	status = spans_open(&v->main, &spans, error);
	while (status == FSIMG_OK) {
		status = apfs_sm_pager_next(pager, &chunk, &found, error);
		if (status != FSIMG_OK || !found)
			break;
		status = apfs_sm_pager_bitmap(pager, &bits, error);
		if (status != FSIMG_OK)
			break;
		first = chunk.info.address;
		end = first + chunk.info.block_count;
		if (end > v->main.end)
			end = v->main.end;
		status = bitmap_compare(&v->main, &spans, bits, first, end,
		    error);
		covered = end;
	}
	if (status == FSIMG_OK)
		status = spans_finish(&spans, &v->main, covered, error);
	fsimg_stream_free(spans.sorted);
	return (status);
}

/*
 * The pool's spans beside each block of the pool's current bitmap, block k
 * standing for the pool's blocks from k * 8B; bits past the pool's last
 * block are not judged.
 */
static fsimg_status_t
pool_merge(verify_t *v, apfs_sm_pager_t *pager, fsimg_error_t *error)
{
	uint64_t per_block = (uint64_t)v->container->block_size * 8;
	uint32_t blocks = v->container->spaceman.ip_bm_size_in_blocks;
	const unsigned char *bits;
	spans_t spans;
	uint64_t first, end, block, covered = v->pool.first;
	fsimg_status_t status;

	status = spans_open(&v->pool, &spans, error);
	for (uint32_t k = 0; status == FSIMG_OK && k < blocks; k++) {
		first = v->pool.first + k * per_block;
		if (first >= v->pool.end)
			break;
		end = first + per_block;
		if (end > v->pool.end)
			end = v->pool.end;
		status = apfs_sm_pager_pool_bitmap(pager, k, &block, &bits,
		    error);
		if (status == FSIMG_OK)
			status = bitmap_compare(&v->pool, &spans, bits, first,
			    end, error);
		covered = end;
	}
	if (status == FSIMG_OK)
		status = spans_finish(&spans, &v->pool, covered, error);
	fsimg_stream_free(spans.sorted);
	return (status);
}

static fsimg_status_t
live_add(verify_t *v, const live_extent_t *live, fsimg_error_t *error)
{
	unsigned char raw[LIVE_BYTES];

	fsimg_store_le64(raw, live->first);
	fsimg_store_le64(raw + 8, live->count);
	fsimg_store_le32(raw + 16, live->volume);
	fsimg_store_le32(raw + 20, live->refcnt);
	return (fsimg_stream_append(v->live_extents, raw, sizeof(raw), NULL,
	    error));
}

/* Read the live record at offset, or report through read the end. */
static fsimg_status_t
live_next(fsimg_stream_t *stream, uint64_t *offset, live_extent_t *live,
    bool *read, fsimg_error_t *error)
{
	unsigned char raw[LIVE_BYTES];
	size_t length;
	fsimg_status_t status;

	*read = false;
	status = fsimg_stream_next(stream, offset, raw, sizeof(raw), &length,
	    error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != LIVE_BYTES)
		return (apfs_corrupt(error, ROLE_VERIFY,
		    "finds a scratch record of the wrong size"));
	live->first = fsimg_load_le64(raw);
	live->count = fsimg_load_le64(raw + 8);
	live->volume = fsimg_load_le32(raw + 16);
	live->refcnt = fsimg_load_le32(raw + 20);
	*read = true;
	return (FSIMG_OK);
}

/* File extents sort by volume, first block, count, then stream. */
static int
fext_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	apfs_fext_claim_t x, y;

	(void)alen;
	(void)blen;
	(void)argument;
	apfs_fext_claim_decode(a, &x);
	apfs_fext_claim_decode(b, &y);
	if (x.volume != y.volume)
		return (x.volume < y.volume ? -1 : 1);
	if (x.first != y.first)
		return (x.first < y.first ? -1 : 1);
	if (x.count != y.count)
		return (x.count < y.count ? -1 : 1);
	return (x.stream < y.stream ? -1 : x.stream > y.stream);
}

/* Events sort by volume, record, position, then kind. */
static int
event_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	const unsigned char *x = a, *y = b;
	uint64_t u, w;

	(void)alen;
	(void)blen;
	(void)argument;
	u = fsimg_load_le32(x + 16);
	w = fsimg_load_le32(y + 16);
	if (u != w)
		return (u < w ? -1 : 1);
	for (size_t at = 0; at < 16; at += 8) {
		u = fsimg_load_le64(x + at);
		w = fsimg_load_le64(y + at);
		if (u != w)
			return (u < w ? -1 : 1);
	}
	return (x[24] < y[24] ? -1 : x[24] > y[24]);
}

static fsimg_status_t
event_add(fsimg_stream_t *events, const event_t *event, fsimg_error_t *error)
{
	unsigned char raw[EVENT_BYTES];

	fsimg_store_le64(raw, event->record);
	fsimg_store_le64(raw + 8, event->position);
	fsimg_store_le32(raw + 16, event->volume);
	fsimg_store_le32(raw + 20, event->refcnt);
	raw[24] = event->kind;
	return (fsimg_stream_append(events, raw, sizeof(raw), NULL, error));
}

static fsimg_status_t
event_next(fsimg_stream_t *events, uint64_t *offset, event_t *event,
    bool *read, fsimg_error_t *error)
{
	unsigned char raw[EVENT_BYTES];
	size_t length;
	fsimg_status_t status;

	*read = false;
	status = fsimg_stream_next(events, offset, raw, sizeof(raw), &length,
	    error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != EVENT_BYTES)
		return (apfs_corrupt(error, ROLE_VERIFY,
		    "finds a scratch record of the wrong size"));
	event->record = fsimg_load_le64(raw);
	event->position = fsimg_load_le64(raw + 8);
	event->volume = fsimg_load_le32(raw + 16);
	event->refcnt = fsimg_load_le32(raw + 20);
	event->kind = raw[24];
	*read = true;
	return (FSIMG_OK);
}

/* The piece [first, end) of a file extent inside record. */
static fsimg_status_t
piece_add(fsimg_stream_t *events, const live_extent_t *record, uint64_t first,
    uint64_t end, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = event_add(events, &(event_t){ .record = record->first,
	    .position = first, .volume = record->volume,
	    .kind = EVENT_ENTER }, error);
	if (status == FSIMG_OK)
		status = event_add(events, &(event_t){ .record = record->first,
		    .position = end, .volume = record->volume,
		    .kind = EVENT_LEAVE }, error);
	return (status);
}

/*
 * Clip each file extent, in block order, to the live physical-extent
 * records it spans: its first block lies in one, and it runs on only into
 * the record of its volume that begins where that one ends. Extents sorted
 * by first block never need a record before the current one, so the
 * current record only advances; a record an extent runs into is read
 * ahead from it. Each piece adds its two boundaries to events.
 */
static fsimg_status_t
extents_clip(verify_t *v, fsimg_stream_t *events, fsimg_error_t *error)
{
	unsigned char raw[APFS_FEXT_CLAIM_BYTES];
	fsimg_stream_t *extents = NULL;
	apfs_fext_claim_t extent;
	live_extent_t live, record;
	uint64_t extents_at = 0, live_at = 0, ahead_at, at, end, piece_end;
	size_t length;
	bool have_live, read;
	fsimg_status_t status;

	status = fsimg_stream_seal(v->file_extents, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(v->file_extents, fext_compare, NULL,
		    NULL, &extents, error);
	if (status == FSIMG_OK)
		status = live_next(v->live_extents, &live_at, &live,
		    &have_live, error);
	while (status == FSIMG_OK) {
		status = fsimg_stream_next(extents, &extents_at, raw,
		    sizeof(raw), &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status != FSIMG_OK)
			break;
		if (length != APFS_FEXT_CLAIM_BYTES) {
			status = apfs_corrupt(error, ROLE_VERIFY,
			    "finds a scratch record of the wrong size");
			break;
		}
		apfs_fext_claim_decode(raw, &extent);
		/* Both lie inside the container, so no end overflows. */
		while (status == FSIMG_OK && have_live &&
		    (live.volume < extent.volume ||
		    (live.volume == extent.volume &&
		    live.first + live.count <= extent.first)))
			status = live_next(v->live_extents, &live_at, &live,
			    &have_live, error);
		if (status != FSIMG_OK)
			break;
		if (!have_live || live.volume != extent.volume ||
		    live.first > extent.first) {
			status = apfs_corrupt(error, ROLE_VERIFY, "finds a file "
			    "extent outside every physical-extent record");
			break;
		}
		record = live;
		ahead_at = live_at;
		at = extent.first;
		end = extent.first + extent.count;
		for (;;) {
			piece_end = record.first + record.count < end ?
			    record.first + record.count : end;
			status = piece_add(events, &record, at, piece_end,
			    error);
			if (status != FSIMG_OK || piece_end == end)
				break;
			status = live_next(v->live_extents, &ahead_at, &record,
			    &read, error);
			if (status != FSIMG_OK)
				break;
			if (!read || record.volume != extent.volume ||
			    record.first != piece_end) {
				status = apfs_corrupt(error, ROLE_VERIFY,
				    "finds a file extent running past its "
				    "physical-extent records");
				break;
			}
			at = piece_end;
		}
	}
	fsimg_stream_free(extents);
	return (status);
}

/* Each live record opens and closes its own boundaries. */
static fsimg_status_t
records_bound(verify_t *v, fsimg_stream_t *events, fsimg_error_t *error)
{
	live_extent_t live;
	uint64_t at = 0;
	bool read;
	fsimg_status_t status;

	for (;;) {
		status = live_next(v->live_extents, &at, &live, &read, error);
		if (status != FSIMG_OK || !read)
			return (status);
		status = event_add(events, &(event_t){ .record = live.first,
		    .position = live.first, .volume = live.volume,
		    .refcnt = live.refcnt, .kind = EVENT_OPEN }, error);
		if (status == FSIMG_OK)
			status = event_add(events, &(event_t){
			    .record = live.first,
			    .position = live.first + live.count,
			    .volume = live.volume, .kind = EVENT_CLOSE },
			    error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/*
 * Sweep the sorted boundaries record by record. A record's opening sorts
 * first among its boundaries and its closing last, and every piece lies
 * between them, so the spans between successive positions cover the
 * record exactly; over each, the pieces entered and not yet left must
 * number the record's reference count. A block no stream maps has none.
 */
static fsimg_status_t
events_sweep(fsimg_stream_t *events, fsimg_error_t *error)
{
	event_t event, record;
	uint64_t at = 0, position = 0, active = 0;
	bool read, have_record = false;
	fsimg_status_t status;

	for (;;) {
		status = event_next(events, &at, &event, &read, error);
		if (status != FSIMG_OK || !read)
			return (status);
		if (!have_record || event.volume != record.volume ||
		    event.record != record.record) {
			if (event.kind != EVENT_OPEN)
				return (apfs_corrupt(error, ROLE_VERIFY,
				    "finds a file-extent piece outside its "
				    "record"));
			record = event;
			have_record = true;
			position = event.position;
			active = 0;
		} else if (event.position != position) {
			if (active != record.refcnt)
				return (apfs_corrupt(error, ROLE_EXTENTREF,
				    "reference count differs from the streams "
				    "mapping its blocks"));
			position = event.position;
		}
		if (event.kind == EVENT_ENTER)
			active++;
		else if (event.kind == EVENT_LEAVE)
			active--;
	}
}

/*
 * The file extents of every volume against the live physical-extent
 * records: clipped to them, then swept for the reference counts.
 */
static fsimg_status_t
extents_merge(verify_t *v, fsimg_error_t *error)
{
	fsimg_stream_t *events = NULL, *sorted = NULL;
	fsimg_status_t status;

	status = fsimg_stream_seal(v->live_extents, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&v->reader->session, EVENT_BYTES,
		    &events, error);
	if (status == FSIMG_OK)
		status = extents_clip(v, events, error);
	if (status == FSIMG_OK)
		status = records_bound(v, events, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(events, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(events, event_compare, NULL, NULL,
		    &sorted, error);
	fsimg_stream_free(events);
	if (status == FSIMG_OK)
		status = events_sweep(sorted, error);
	fsimg_stream_free(sorted);
	return (status);
}

/*
 * The selected volume's inodes of several names, which the scan appended
 * in ascending order, copied into a paged array of SHARED_BYTES records
 * whose last byte marks an inode the walk has read.
 */
static fsimg_status_t
shared_load(verify_t *v, fsimg_array_t **array, uint64_t *count,
    fsimg_error_t *error)
{
	unsigned char raw[SHARED_BYTES];
	uint64_t at = 0, previous = 0;
	size_t length;
	fsimg_status_t status;

	*array = NULL;
	*count = v->shared->count;
	if (*count == 0)
		return (FSIMG_OK);
	status = fsimg_stream_seal(v->shared, error);
	if (status == FSIMG_OK)
		status = fsimg_array_new(&v->reader->session, SHARED_BYTES,
		    *count, array, error);
	for (uint64_t i = 0; status == FSIMG_OK && i < *count; i++) {
		status = fsimg_stream_next(v->shared, &at, raw, 8, &length,
		    error);
		if (status == FSIMG_DONE || (status == FSIMG_OK &&
		    (length != 8 || (i > 0 && fsimg_load_le64(raw) <=
		    previous))))
			return (apfs_corrupt(error, ROLE_VERIFY,
			    "finds its list of linked inodes out of order"));
		previous = fsimg_load_le64(raw);
		raw[8] = 0;
		if (status == FSIMG_OK)
			status = fsimg_array_set(*array, i, raw, error);
	}
	return (status);
}

/*
 * Whether the walk reads inode now: true unless it is one of several names
 * already read; the first time an inode of several names is met, it is
 * marked. The array is searched by halving.
 */
static fsimg_status_t
shared_first(fsimg_array_t *array, uint64_t count, uint64_t inode,
    bool *first, fsimg_error_t *error)
{
	unsigned char raw[SHARED_BYTES];
	uint64_t low = 0, high = count, middle, number;
	fsimg_status_t status;

	*first = true;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = fsimg_array_get(array, middle, raw, error);
		if (status != FSIMG_OK)
			return (status);
		number = fsimg_load_le64(raw);
		if (number < inode) {
			low = middle + 1;
			continue;
		}
		if (number > inode) {
			high = middle;
			continue;
		}
		if (raw[8] != 0) {
			*first = false;
			return (FSIMG_OK);
		}
		raw[8] = 1;
		return (fsimg_array_set(array, middle, raw, error));
	}
	return (FSIMG_OK);
}

/*
 * The common walk of the selected volume: every delivered entry is counted
 * through fsimg_verify_account(), the root among the directories, and with
 * payload read-through each file and symbolic link is read, an inode of
 * several names once, at the first name the walk reaches.
 */
static fsimg_status_t
namespace_walk(verify_t *v, unsigned flags, fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = v->reader;
	fsimg_array_t *shared = NULL;
	unsigned char *payload = NULL;
	fsimg_error_t detail;
	uint64_t count;
	bool first;
	fsimg_status_t status;

	status = shared_load(v, &shared, &count, error);
	if (status == FSIMG_OK && (flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		payload = fsimg_alloc(reader->ctx, PAYLOAD_BYTES, &detail);
		if (payload == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
		}
	}
	report->directories = 1;
	while (status == FSIMG_OK) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status == FSIMG_OK)
			status = fsimg_verify_account(reader, report, error);
		if (status != FSIMG_OK || payload == NULL ||
		    !fsimg_walk_delivered(reader) ||
		    (reader->entry.kind != FSIMG_ENTRY_FILE &&
		    reader->entry.kind != FSIMG_ENTRY_SYMLINK))
			continue;
		first = true;
		if (shared != NULL)
			status = shared_first(shared, count,
			    v->apfs->current.inode, &first, error);
		if (status == FSIMG_OK && first)
			status = fsimg_verify_payload(reader, payload,
			    PAYLOAD_BYTES, report, error);
	}
	fsimg_dealloc(payload);
	fsimg_array_free(shared);
	return (status);
}

/* Both domains' merges over one pager pass, then the file extents'. */
static fsimg_status_t
allocation_merge(verify_t *v, fsimg_error_t *error)
{
	apfs_sm_pager_t *pager = NULL;
	fsimg_status_t status;

	status = apfs_sm_pager_open(v->container, &pager, error);
	if (status == FSIMG_OK)
		status = main_merge(v, pager, error);
	if (status == FSIMG_OK)
		status = pool_merge(v, pager, error);
	apfs_sm_pager_free(pager);
	if (status == FSIMG_OK)
		status = extents_merge(v, error);
	return (status);
}

fsimg_status_t
apfs_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_container_t *container = &apfs->container;
	const apfs_spaceman_t *sm = &container->spaceman;
	verify_t *v;
	fsimg_error_t detail;
	fsimg_status_t status;

	reader->session.progress.phase = "APFS verification";
	v = fsimg_alloc(reader->ctx, sizeof(*v), &detail);
	if (v == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	*v = (verify_t){ .reader = reader, .apfs = apfs,
	    .container = container, .flags = flags,
	    .main = { .name = "container", .first = 0,
	    .end = container->block_count },
	    .pool = { .name = "internal pool", .first = sm->ip_base,
	    .end = sm->ip_base + sm->ip_block_count } };
	v->window = fsimg_alloc(reader->ctx, container->block_size, &detail);
	status = v->window == NULL ? detail.status : FSIMG_OK;
	if (status != FSIMG_OK && error != NULL)
		*error = detail;
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, CLAIM_BYTES,
		    &v->main.claims, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, CLAIM_BYTES,
		    &v->pool.claims, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, LIVE_BYTES,
		    &v->live_extents, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session,
		    APFS_FEXT_CLAIM_BYTES, &v->file_extents, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, 8, &v->shared,
		    error);
	if (status == FSIMG_OK)
		status = container_pass(v, error);
	if (status == FSIMG_OK && v->incomplete.status == FSIMG_OK) {
		status = allocation_merge(v, error);
		report->free_clusters = apfs->free_blocks;
		report->allocated_clusters = container->block_count -
		    apfs->free_blocks;
	}
	if (status == FSIMG_OK && v->chunk_tables)
		incomplete_reason(v, "compressed file", "keeps its content in "
		    "chunks whose tables verification does not read");
	if (status == FSIMG_OK)
		status = namespace_walk(v, flags, report, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status == FSIMG_OK && v->incomplete.status != FSIMG_OK) {
		if (error != NULL)
			*error = v->incomplete;
		status = v->incomplete.status;
	}
	if (status == FSIMG_OK) {
		reader->info.free_bytes = report->free_clusters *
		    container->block_size;
		reader->info.valid |= FSIMG_VOLUME_FREE;
	}
	fsimg_stream_free(v->shared);
	fsimg_stream_free(v->file_extents);
	fsimg_stream_free(v->live_extents);
	fsimg_stream_free(v->pool.claims);
	fsimg_stream_free(v->main.claims);
	fsimg_dealloc(v->window);
	fsimg_dealloc(v);
	return (status);
}
