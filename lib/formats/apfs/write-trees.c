/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "write-apfs.h"

/*
 * The records of the writer's generated trees (specs/design-apfs-driver.md,
 * "Planning"). Each tree has one generator, and apfs_tree_pack() is the one
 * loop that feeds it to the packer: planning runs it with no sink to count
 * nodes, emission with the writing sink to write them. Both runs therefore
 * see the same records in the same order, so the node count, the node
 * boundaries and every block and OID derived from them cannot differ
 * between the plan and the image.
 *
 * A generator reads the plan. Planning packs before layout, when the
 * addresses a generator encodes (run blocks and files' first blocks) are
 * not yet final: in planning every physical-extent key is block zero. That
 * is sound because the packer places node boundaries from the record count
 * and lengths alone and never compares keys, and every address is a
 * fixed-width field, so record lengths do not depend on those values.
 * Emission's completion of file extents rewrites a fixed-width block in
 * place too. What a record count depends on is fixed before planning packs
 * it: the sorted stream, the rows, and the filesystem tree's node count for
 * the volume object map, which is why that tree is packed first.
 */

#define PEXT_KIND_NEW		1u

/*
 * The state of one tree's generation. next is its position: the sorted
 * stream's offset for the filesystem tree, the records produced for an
 * object map, the catalog IDs passed for the extent-reference tree. key and
 * val hold a generated record; frame holds a record of the sorted stream.
 */
typedef struct apfs_tree_gen {
	const apfs_tree_run_t	*run;
	uint64_t		 next;
	unsigned char		 key[APFS_OMAP_KEY_BYTES];
	unsigned char		 val[APFS_PEXT_VAL_BYTES];
	unsigned char		 frame[APFS_WRITE_RECORD_MAX];
} apfs_tree_gen_t;

_Static_assert(APFS_OMAP_KEY_BYTES >= APFS_JKEY_BYTES &&
    APFS_PEXT_VAL_BYTES >= APFS_OMAP_VAL_BYTES,
    "the generated records fit their buffers");

/*
 * A tree's generator: each call produces the next record in key order into
 * record, borrowed until the next call, or returns FSIMG_DONE after the
 * last.
 */
typedef fsimg_status_t (*tree_next_t)(apfs_tree_gen_t *, apfs_record_t *,
    fsimg_error_t *);

static fsimg_status_t
inconsistent(const char *what, fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
	    what));
}

static fsimg_status_t
charge(const apfs_tree_run_t *run, fsimg_error_t *error)
{
	return (fsimg_session_charge(&run->writer->session, 0, 0, 1, error));
}

/* An object-map record: OID oid at the checkpoint's XID, at block. */
static fsimg_status_t
omap_record(apfs_tree_gen_t *g, uint64_t oid, uint64_t block,
    apfs_record_t *record, fsimg_error_t *error)
{
	apfs_omap_key_t key = { oid, APFS_WRITE_XID };
	apfs_omap_val_t val = { 0, 0, APFS_WRITE_BLOCK_BYTES };

	val.address = block;
	if (!apfs_omap_key_encode(g->key, APFS_OMAP_KEY_BYTES, &key) ||
	    !apfs_omap_val_encode(g->val, APFS_OMAP_VAL_BYTES, &val))
		return (inconsistent("APFS object-map record does not encode",
		    error));
	*record = (apfs_record_t){ .key = g->key, .val = g->val,
	    .key_len = APFS_OMAP_KEY_BYTES, .val_len = APFS_OMAP_VAL_BYTES };
	return (FSIMG_OK);
}

/*
 * The next record of the sorted filesystem stream, passed to the run's
 * completion first when it has one.
 */
static fsimg_status_t
fs_next(apfs_tree_gen_t *g, apfs_record_t *record, fsimg_error_t *error)
{
	const apfs_tree_run_t *run = g->run;
	apfs_write_record_t frame;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(run->plan->records, &g->next, g->frame,
	    sizeof(g->frame), &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_write_record_decode(g->frame, length, &frame))
		return (inconsistent("APFS sorted record is inconsistent",
		    error));
	if (run->complete != NULL) {
		/* The value lies in the generator's writable frame. */
		status = run->complete(run->arg, &frame,
		    g->frame + (frame.val - g->frame), error);
		if (status != FSIMG_OK)
			return (status);
	}
	*record = (apfs_record_t){ .key = frame.key, .val = frame.val,
	    .key_len = frame.key_len, .val_len = frame.val_len };
	return (FSIMG_OK);
}

/* The volume object map: filesystem node k at its block, k from zero. */
static fsimg_status_t
vomap_next(apfs_tree_gen_t *g, apfs_record_t *record, fsimg_error_t *error)
{
	const apfs_plan_t *plan = g->run->plan;
	const apfs_tree_plan_t *fs = &plan->trees[APFS_TREE_FS];
	uint64_t k = g->next;

	if (k == fs->shape.node_count)
		return (FSIMG_DONE);
	g->next++;
	return (omap_record(g, APFS_WRITE_OID_FS_FIRST + k,
	    apfs_run_block(plan, fs->first_run + k), record, error));
}

/*
 * The extent-reference tree: one physical-extent record per file with
 * content, in catalog order, which is block order and so key order. The
 * record's key is the run's first block and its owner the inode number,
 * which is the file's private ID. Every catalog ID passed is charged.
 */
static fsimg_status_t
extentref_next(apfs_tree_gen_t *g, apfs_record_t *record,
    fsimg_error_t *error)
{
	const apfs_tree_run_t *run = g->run;
	apfs_pext_val_t pext;
	apfs_row_t row;
	uint64_t id;
	fsimg_status_t status;

	/* next counts the catalog IDs passed; the first is 1. */
	for (;;) {
		if (g->next == run->writer->count)
			return (FSIMG_DONE);
		id = ++g->next;
		status = charge(run, error);
		if (status == FSIMG_OK)
			status = apfs_row_get(run->plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (apfs_row_blocks(&row) != 0)
			break;
	}
	pext = (apfs_pext_val_t){ .length = apfs_row_blocks(&row),
	    .owner = apfs_write_ino(id), .refcnt = 1, .kind = PEXT_KIND_NEW };
	if (!apfs_bare_key_encode(g->key, APFS_JKEY_BYTES, APFS_J_PHYS_EXTENT,
	    row.first_block) ||
	    !apfs_pext_val_encode(g->val, APFS_PEXT_VAL_BYTES, &pext))
		return (inconsistent("APFS physical extent does not encode",
		    error));
	*record = (apfs_record_t){ .key = g->key, .val = g->val,
	    .key_len = APFS_JKEY_BYTES, .val_len = APFS_PEXT_VAL_BYTES };
	return (FSIMG_OK);
}

/* The snapshot tree has no records. */
static fsimg_status_t
snapshot_next(apfs_tree_gen_t *g, apfs_record_t *record,
    fsimg_error_t *error)
{
	(void)g;
	(void)record;
	(void)error;
	return (FSIMG_DONE);
}

/* The container object map's one record: the volume superblock. */
static fsimg_status_t
comap_next(apfs_tree_gen_t *g, apfs_record_t *record, fsimg_error_t *error)
{
	const apfs_plan_t *plan = g->run->plan;

	if (g->next != 0)
		return (FSIMG_DONE);
	g->next++;
	return (omap_record(g, APFS_WRITE_OID_VOLUME,
	    apfs_run_block(plan, plan->volume_run), record, error));
}

/* Each tree's profile and its generator. */
static const struct {
	const apfs_pack_profile_t	*profile;
	tree_next_t			 next;
} generators[APFS_TREE_COUNT] = {
	[APFS_TREE_FS] = { &apfs_pack_fs_tree, fs_next },
	[APFS_TREE_VOMAP] = { &apfs_pack_volume_omap, vomap_next },
	[APFS_TREE_EXTENTREF] = { &apfs_pack_extentref, extentref_next },
	[APFS_TREE_SNAPSHOT] = { &apfs_pack_snapshot, snapshot_next },
	[APFS_TREE_COMAP] = { &apfs_pack_container_omap, comap_next }
};

fsimg_status_t
apfs_tree_pack(const apfs_tree_run_t *run, apfs_tree_id_t id,
    apfs_pack_result_t *result, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx = run->writer->ctx;
	apfs_tree_gen_t *g;
	apfs_record_t record;
	apfs_pack_t *pack;
	fsimg_error_t detail;
	fsimg_status_t status;

	g = fsimg_alloc(ctx, sizeof(*g), &detail);
	if (g == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(g, sizeof(*g));
	g->run = run;
	status = apfs_pack_open(ctx, generators[id].profile, APFS_WRITE_XID,
	    run->sink, &pack, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(g);
		return (status);
	}
	for (;;) {
		status = charge(run, error);
		if (status != FSIMG_OK)
			break;
		bzero(&record, sizeof(record));
		status = generators[id].next(g, &record, error);
		if (status == FSIMG_DONE) {
			status = apfs_pack_finish(pack, result, error);
			break;
		}
		if (status == FSIMG_OK)
			status = apfs_pack_add(pack, &record, error);
		if (status != FSIMG_OK)
			break;
	}
	apfs_pack_free(pack);
	fsimg_dealloc(g);
	return (status);
}
