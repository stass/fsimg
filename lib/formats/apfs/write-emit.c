/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write-apfs.h"

/*
 * Emission writes the container the plan describes
 * (specs/design-apfs-driver.md, "Emission"), in the first form of the
 * format design's writer values: one checkpoint at XID 1 whose free queues
 * are empty. The order of work is
 *
 *   payload        every file's content, copied into its run;
 *   trees          the filesystem tree, replayed from the sorted records
 *                  through the planning packer, then the volume object
 *                  map's, extent-reference, snapshot and container object
 *                  map's trees, the two object maps and the volume
 *                  superblock;
 *   space manager  the chunk-information, address and bitmap blocks in the
 *                  internal pool, the pool's bitmap and the space manager;
 *   checkpoint     the reaper, the mapping block, the superblock and its
 *                  copy in block 0.
 *
 * Every block number was fixed by layout, so the bytes do not depend on
 * this order. The common emission object has zeroed the volume, so blocks
 * the writer never writes, the tail of every file's last block among them,
 * hold zero; every block written is encoded over a zeroed buffer, so its
 * unused fields and free space hold zero too. A tree's node boundaries come
 * from the packer that planning ran over the same records, and each tree's
 * shape is compared with the planned one before its blocks are named
 * anywhere else, so a divergence fails instead of writing an inconsistent
 * container.
 */

#define BLOCK			APFS_WRITE_BLOCK_BYTES
#define NX_INCOMPAT		APFS_NX_INCOMPAT_VERSION2
#define FS_FEATURES		0x2u	/* hard-link map records */
#define OMAP_TREE_TYPE		(APFS_OBJ_PHYSICAL | APFS_KIND_BTREE_ROOT)
#define SM_FLAGS		0x1u
#define SM_VERSION		1u
#define REAPER_NEXT_ID		1u
#define NEXT_DOC_ID		3u
#define PEXT_KIND_NEW		1u
#define FORMATTED_BY		"fsimg " PACKAGE_VERSION

_Static_assert(sizeof(FORMATTED_BY) <= APFS_FS_CHANGE_ID_BYTES,
    "the formatting identifier fits its field with a NUL");

/*
 * The state of one emission. key and val hold a generated record; frame
 * holds one record of the sorted stream, whose file-extent values are
 * completed in place; target holds a link target read again. cursor and
 * next are the position of the tree being packed: the sorted stream's
 * offset for the filesystem tree, the records produced, or the catalog IDs
 * passed, for the others. pack_tree() resets both before each tree.
 */
typedef struct apfs_emitter {
	apfs_plan_t	*plan;
	fsimg_writer_t	*writer;
	fsimg_emit_t	*emit;
	uint64_t	 cursor;
	uint64_t	 next;
	unsigned char	 key[APFS_OMAP_KEY_BYTES];
	unsigned char	 val[APFS_PEXT_VAL_BYTES];
	unsigned char	 frame[APFS_WRITE_RECORD_MAX];
	unsigned char	 target[FSIMG_SYMLINK_TARGET_MAX];
} apfs_emitter_t;

_Static_assert(APFS_OMAP_KEY_BYTES >= APFS_JKEY_BYTES &&
    APFS_PEXT_VAL_BYTES >= APFS_OMAP_VAL_BYTES,
    "the generated records fit their buffers");

/*
 * A tree's records as a sequential cursor over the emitter's position:
 * each call produces the next record into record, borrowed until the next
 * call, or returns FSIMG_DONE after the last. A source runs once per tree,
 * in key order, from the position pack_tree() reset.
 */
typedef fsimg_status_t (*record_source_t)(apfs_emitter_t *, apfs_record_t *,
    fsimg_error_t *);

/* The plan and the bytes being emitted disagree: an internal error. */
static fsimg_status_t
diverged(const char *what, fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
	    what));
}

static fsimg_status_t
charge(apfs_emitter_t *e, fsimg_error_t *error)
{
	return (fsimg_session_charge(&e->writer->session, 0, 0, 1, error));
}

/* Write count whole blocks from bytes at block. */
static fsimg_status_t
write_blocks(apfs_emitter_t *e, uint64_t block, const unsigned char *bytes,
    uint64_t count, fsimg_error_t *error)
{
	return (fsimg_emit_write(e->emit, block * BLOCK, bytes, (size_t)(count * BLOCK),
	    error));
}

/*
 * Encode an object header at the start of object, a buffer of length
 * bytes whose body is already encoded, and store the checksum over all of
 * it.
 */
static fsimg_status_t
seal(unsigned char *object, size_t length, uint64_t oid, uint32_t storage,
    uint16_t kind, uint32_t subtype, fsimg_error_t *error)
{
	apfs_obj_t header = { .oid = oid, .xid = APFS_WRITE_XID,
	    .flags = storage, .subtype = subtype, .kind = kind };

	if (!apfs_obj_encode(object, length, &header) ||
	    !apfs_object_seal(object, length))
		return (diverged("APFS object does not encode", error));
	return (FSIMG_OK);
}

/*
 * The emission buffer, for one object of blocks blocks, zeroed. Nothing
 * else holds it between the encoding and the write that follows.
 */
static fsimg_status_t
block_buffer(apfs_emitter_t *e, uint64_t blocks, unsigned char **buffer,
    fsimg_error_t *error)
{
	*buffer = fsimg_emit_buffer(e->emit);
	if (*buffer == NULL || blocks * BLOCK > FSIMG_EMIT_BUFFER_BYTES)
		return (diverged("APFS object does not fit the emission buffer",
		    error));
	bzero(*buffer, (size_t)(blocks * BLOCK));
	return (FSIMG_OK);
}

/* Step 1: payload */

/*
 * Copy every regular file into its run. An empty file has no run and is
 * copied with no regions, so its source is still opened and validated.
 * The copy writes the file's length; the rest of its last block keeps the
 * zero the emission object wrote.
 */
static fsimg_status_t
emit_payload(apfs_emitter_t *e, fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	fsimg_emit_payload_t payload;
	apfs_row_t row;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= e->writer->count; id++) {
		status = charge(e, error);
		if (status == FSIMG_OK)
			status = apfs_row_get(e->plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_FILE)
			continue;
		bzero(&payload, sizeof(payload));
		payload.length = row.length;
		payload.initialized = row.length;
		if (apfs_row_blocks(&row) != 0) {
			region.image_offset = row.first_block * BLOCK;
			region.capacity = apfs_row_blocks(&row) * BLOCK;
			payload.regions = &region;
			payload.region_count = 1;
		}
		status = fsimg_emit_copy(e->emit, id, &payload, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* The trees */

/*
 * Where a tree's nodes go: node k of tree sits at the run block of the
 * tree's first index plus k. Its OID is that block in a physical tree and
 * the first filesystem node OID plus k in the filesystem tree.
 */
typedef struct apfs_tree_sink {
	apfs_emitter_t		*emitter;
	const apfs_tree_plan_t	*tree;
	bool			 virtual_oids;
} apfs_tree_sink_t;

static uint64_t
tree_node_block(const apfs_plan_t *plan, const apfs_tree_plan_t *tree,
    uint64_t k)
{
	return (apfs_run_block(plan, tree->first_run + k));
}

static uint64_t
tree_node_oid(void *arg, uint64_t k)
{
	const apfs_tree_sink_t *sink = arg;

	if (sink->virtual_oids)
		return (APFS_WRITE_OID_FS_FIRST + k);
	return (tree_node_block(sink->emitter->plan, sink->tree, k));
}

/* A node past the planned count would land on another object's block. */
static fsimg_status_t
tree_node_write(void *arg, uint64_t k, uint64_t oid,
    const unsigned char *node, fsimg_error_t *error)
{
	const apfs_tree_sink_t *sink = arg;
	apfs_emitter_t *e = sink->emitter;

	(void)oid;
	if (k >= sink->tree->shape.node_count)
		return (diverged("APFS tree outgrew its plan", error));
	return (write_blocks(e, tree_node_block(e->plan, sink->tree, k), node,
	    1, error));
}

/* The shape a tree packed to equals the one planning counted. */
static bool
shape_equal(const apfs_pack_result_t *a, const apfs_pack_result_t *b)
{
	if (a->node_count != b->node_count || a->key_count != b->key_count ||
	    a->levels != b->levels)
		return (false);
	for (unsigned level = 0; level < a->levels; level++)
		if (a->level_nodes[level] != b->level_nodes[level])
			return (false);
	return (true);
}

/*
 * Pack one tree from source with the writing sink. Nodes are written as
 * they close, the root last.
 */
static fsimg_status_t
pack_tree(apfs_emitter_t *e, const apfs_pack_profile_t *profile,
    const apfs_tree_plan_t *tree, bool virtual_oids, record_source_t source,
    fsimg_error_t *error)
{
	apfs_tree_sink_t sink = { e, tree, virtual_oids };
	apfs_pack_sink_t pack_sink = { &sink, tree_node_oid, tree_node_write };
	apfs_pack_result_t result;
	apfs_record_t record;
	apfs_pack_t *pack;
	fsimg_status_t status;

	status = apfs_pack_open(e->writer->ctx, profile, APFS_WRITE_XID,
	    &pack_sink, &pack, error);
	if (status != FSIMG_OK)
		return (status);
	e->cursor = 0;
	e->next = 0;
	for (;;) {
		status = charge(e, error);
		if (status != FSIMG_OK)
			break;
		bzero(&record, sizeof(record));
		status = source(e, &record, error);
		if (status == FSIMG_DONE) {
			status = apfs_pack_finish(pack, &result, error);
			break;
		}
		if (status == FSIMG_OK)
			status = apfs_pack_add(pack, &record, error);
		if (status != FSIMG_OK)
			break;
	}
	apfs_pack_free(pack);
	if (status == FSIMG_OK && !shape_equal(&result, &tree->shape))
		status = diverged("APFS tree differs from its plan", error);
	return (status);
}

/* The root of a planned tree: its last node. */
static uint64_t
tree_root_block(const apfs_plan_t *plan, const apfs_tree_plan_t *tree)
{
	return (tree_node_block(plan, tree, tree->shape.node_count - 1));
}

/*
 * Complete a file extent of the sorted stream in place: its physical block
 * is the file's first block plus the extent's offset in the run. The
 * extent must lie inside the run planning gave the file.
 */
static fsimg_status_t
complete_extent(apfs_emitter_t *e, const apfs_write_record_t *frame,
    const apfs_row_t *row, fsimg_error_t *error)
{
	/* The value lies in the emitter's frame, which is writable. */
	unsigned char *val = e->frame + (frame->val - e->frame);
	apfs_extent_key_t key;
	apfs_extent_val_t extent;
	uint64_t blocks = apfs_row_blocks(row);

	if (!apfs_extent_key_decode(frame->key, frame->key_len, &key) ||
	    !apfs_extent_val_decode(frame->val, frame->val_len, &extent) ||
	    row->first_block == 0 || key.logical % BLOCK != 0 ||
	    extent.length % BLOCK != 0 || key.logical / BLOCK > blocks ||
	    extent.length / BLOCK > blocks - key.logical / BLOCK)
		return (diverged("APFS file extent is outside its run", error));
	extent.phys_block = row->first_block + key.logical / BLOCK;
	if (!apfs_extent_val_encode(val, frame->val_len, &extent))
		return (diverged("APFS file extent does not encode", error));
	return (FSIMG_OK);
}

/*
 * Read a link's target again through the common link-target operation and
 * compare it with the attribute planning encoded, so that no target the
 * emission has not just read reaches the output.
 */
static fsimg_status_t
check_link(apfs_emitter_t *e, const apfs_write_record_t *frame,
    const apfs_row_t *row, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	apfs_xattr_val_t xattr;
	size_t length;
	fsimg_status_t status;

	status = fsimg_writer_link_target(e->writer, frame->id, e->target,
	    &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_xattr_val_decode(frame->val, frame->val_len, &xattr))
		return (diverged("APFS link attribute does not decode", error));
	if (length == row->length && xattr.data_len == length + 1 &&
	    bcmp(xattr.data, e->target, length) == 0)
		return (FSIMG_OK);
	status = fsimg_catalog_get(e->writer, frame->id, &catalog, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0, FSIMG_OP_WRITER,
	    "link target changed since planning");
	fsimg_error_path(error, catalog->entry.path);
	return (FSIMG_ESOURCE_CHANGED);
}

/*
 * The next record of the sorted filesystem stream, with file extents
 * completed and link targets checked. The writer emits extended
 * attributes only for symbolic links, so every attribute of a link's row
 * is its target.
 */
static fsimg_status_t
fs_record(apfs_emitter_t *e, apfs_record_t *record,
    fsimg_error_t *error)
{
	apfs_write_record_t frame;
	apfs_jkey_t jkey;
	apfs_row_t row;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(e->plan->records, &e->cursor, e->frame,
	    sizeof(e->frame), &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_write_record_decode(e->frame, length, &frame) ||
	    !apfs_jkey_decode(frame.key, frame.key_len, &jkey))
		return (diverged("APFS sorted record is inconsistent", error));
	if (jkey.kind == APFS_J_FILE_EXTENT || jkey.kind == APFS_J_XATTR) {
		status = apfs_row_get(e->plan, frame.id, &row, error);
		if (status == FSIMG_OK && jkey.kind == APFS_J_FILE_EXTENT)
			status = complete_extent(e, &frame, &row, error);
		if (status == FSIMG_OK && jkey.kind == APFS_J_XATTR) {
			if (row.kind != FSIMG_ENTRY_SYMLINK)
				status = diverged("APFS attribute is not a link "
				    "target", error);
			else
				status = check_link(e, &frame, &row, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	record->key = frame.key;
	record->key_len = frame.key_len;
	record->val = frame.val;
	record->val_len = frame.val_len;
	return (FSIMG_OK);
}

/* The next record of the volume object map: filesystem node k at its
 * block, for k counting up from zero.
 */
static fsimg_status_t
volume_omap_record(apfs_emitter_t *e, apfs_record_t *record,
    fsimg_error_t *error)
{
	const apfs_tree_plan_t *fs = &e->plan->fs_tree;
	uint64_t k = e->next;
	apfs_omap_key_t key = { APFS_WRITE_OID_FS_FIRST + k, APFS_WRITE_XID };
	apfs_omap_val_t val = { 0, 0, BLOCK };

	if (k == fs->shape.node_count)
		return (FSIMG_DONE);
	e->next++;
	val.address = tree_node_block(e->plan, fs, k);
	if (!apfs_omap_key_encode(e->key, APFS_OMAP_KEY_BYTES, &key) ||
	    !apfs_omap_val_encode(e->val, APFS_OMAP_VAL_BYTES, &val))
		return (diverged("APFS object-map record does not encode",
		    error));
	*record = (apfs_record_t){ .key = e->key, .val = e->val,
	    .key_len = APFS_OMAP_KEY_BYTES, .val_len = APFS_OMAP_VAL_BYTES };
	return (FSIMG_OK);
}

/*
 * The next physical-extent record: one per file with content, in catalog
 * order, which is block order and so key order. The record's key is the
 * run's first block and its owner the inode number, which is the file's
 * private ID.
 */
static fsimg_status_t
extentref_record(apfs_emitter_t *e, apfs_record_t *record,
    fsimg_error_t *error)
{
	apfs_pext_val_t pext;
	apfs_row_t row;
	uint64_t id;
	fsimg_status_t status;

	/* next counts the catalog IDs passed; the first is 1. */
	for (;;) {
		if (e->next == e->writer->count)
			return (FSIMG_DONE);
		id = ++e->next;
		status = charge(e, error);
		if (status == FSIMG_OK)
			status = apfs_row_get(e->plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (apfs_row_blocks(&row) != 0)
			break;
	}
	pext = (apfs_pext_val_t){ .length = apfs_row_blocks(&row),
	    .owner = apfs_write_ino(id), .refcnt = 1, .kind = PEXT_KIND_NEW };
	if (!apfs_bare_key_encode(e->key, APFS_JKEY_BYTES, APFS_J_PHYS_EXTENT,
	    row.first_block) ||
	    !apfs_pext_val_encode(e->val, APFS_PEXT_VAL_BYTES, &pext))
		return (diverged("APFS physical extent does not encode",
		    error));
	*record = (apfs_record_t){ .key = e->key, .val = e->val,
	    .key_len = APFS_JKEY_BYTES, .val_len = APFS_PEXT_VAL_BYTES };
	return (FSIMG_OK);
}

/* The snapshot tree has no records. */
static fsimg_status_t
no_record(apfs_emitter_t *e, apfs_record_t *record, fsimg_error_t *error)
{
	(void)e;
	(void)record;
	(void)error;
	return (FSIMG_DONE);
}

/* The container object map's one record: the volume superblock. */
static fsimg_status_t
container_omap_record(apfs_emitter_t *e, apfs_record_t *record,
    fsimg_error_t *error)
{
	apfs_omap_key_t key = { APFS_WRITE_OID_VOLUME, APFS_WRITE_XID };
	apfs_omap_val_t val = { 0, 0, BLOCK };

	if (e->next++ != 0)
		return (FSIMG_DONE);
	val.address = apfs_run_block(e->plan, e->plan->volume_run);
	if (!apfs_omap_key_encode(e->key, APFS_OMAP_KEY_BYTES, &key) ||
	    !apfs_omap_val_encode(e->val, APFS_OMAP_VAL_BYTES, &val))
		return (diverged("APFS object-map record does not encode",
		    error));
	*record = (apfs_record_t){ .key = e->key, .val = e->val,
	    .key_len = APFS_OMAP_KEY_BYTES, .val_len = APFS_OMAP_VAL_BYTES };
	return (FSIMG_OK);
}

/*
 * An object map at run index run over the tree whose root is at
 * tree_block: the container's manually managed, the volume's with no
 * flags, neither with snapshots.
 */
static fsimg_status_t
emit_omap(apfs_emitter_t *e, uint64_t run, uint64_t tree_block,
    uint32_t flags, fsimg_error_t *error)
{
	uint64_t block = apfs_run_block(e->plan, run);
	apfs_omap_t omap = { .tree_oid = tree_block, .flags = flags,
	    .tree_type = OMAP_TREE_TYPE, .snapshot_tree_type = OMAP_TREE_TYPE };
	unsigned char *buffer;
	fsimg_status_t status;

	status = block_buffer(e, 1, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_omap_encode(buffer, BLOCK, &omap))
		return (diverged("APFS object map does not encode", error));
	status = seal(buffer, BLOCK, block, APFS_OBJ_PHYSICAL, APFS_KIND_OMAP,
	    APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, block, buffer, 1, error);
	return (status);
}

/*
 * The volume superblock: slot 0, role none, unencrypted, the label as its
 * name, its trees' roots, the per-kind totals and the format time.
 */
static fsimg_status_t
emit_volume(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	const char *label = e->writer->options.label;
	uint64_t block = apfs_run_block(plan, plan->volume_run);
	unsigned char *buffer;
	apfs_fs_t fs;
	fsimg_status_t status;

	if (label == NULL || strlen(label) >= APFS_FS_NAME_BYTES)
		return (diverged("APFS label was not validated", error));
	bzero(&fs, sizeof(fs));
	fs.magic = APFS_FS_MAGIC;
	fs.features = FS_FEATURES;
	fs.incompat_features = plan->case_sensitive ?
	    APFS_FS_INCOMPAT_NORM_INSENSITIVE :
	    APFS_FS_INCOMPAT_CASE_INSENSITIVE;
	fs.alloc_blocks = plan->volume_allocated;
	fs.root_tree_type = APFS_FS_TREE_VIRTUAL;
	fs.extentref_tree_type = APFS_FS_TREE_PHYSICAL;
	fs.snapshot_tree_type = APFS_FS_TREE_PHYSICAL;
	fs.omap_oid = apfs_run_block(plan, plan->vomap_run);
	fs.root_tree_oid = APFS_WRITE_OID_FS_FIRST +
	    plan->fs_tree.shape.node_count - 1;
	fs.extentref_tree_oid = tree_root_block(plan, &plan->extentref_tree);
	fs.snapshot_tree_oid = tree_root_block(plan, &plan->snapshot_tree);
	fs.next_obj_id = plan->next_ino;
	fs.files = plan->files;
	fs.directories = plan->directories;
	fs.symlinks = plan->symlinks;
	fs.other_objects = plan->others;
	fs.blocks_allocated = plan->data_blocks;
	fs.modified_time = (uint64_t)plan->format_time;
	fs.flags = APFS_FS_FLAG_UNENCRYPTED;
	fs.next_doc_id = NEXT_DOC_ID;
	bcopy(plan->volume_uuid, fs.uuid, sizeof(fs.uuid));
	bcopy(FORMATTED_BY, fs.formatted_by.id, sizeof(FORMATTED_BY));
	fs.formatted_by.time = (uint64_t)plan->format_time;
	fs.formatted_by.last_xid = APFS_WRITE_XID;
	bcopy(label, fs.name, strlen(label));

	status = block_buffer(e, 1, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_fs_encode(buffer, BLOCK, &fs))
		return (diverged("APFS volume superblock does not encode",
		    error));
	status = seal(buffer, BLOCK, APFS_WRITE_OID_VOLUME, APFS_OBJ_VIRTUAL,
	    APFS_KIND_FS, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, block, buffer, 1, error);
	return (status);
}

/*
 * Steps 2 and 3: the filesystem tree, the volume object map's tree and
 * object, the extent-reference and snapshot trees, the volume superblock,
 * and the container object map's tree and object.
 */
static fsimg_status_t
emit_trees(apfs_emitter_t *e, fsimg_error_t *error)
{
	apfs_plan_t *plan = e->plan;
	fsimg_status_t status;

	status = pack_tree(e, &apfs_pack_fs_tree, &plan->fs_tree, true,
	    fs_record, error);
	if (status == FSIMG_OK)
		status = pack_tree(e, &apfs_pack_volume_omap, &plan->vomap_tree,
		    false, volume_omap_record, error);
	if (status == FSIMG_OK)
		status = pack_tree(e, &apfs_pack_extentref,
		    &plan->extentref_tree, false, extentref_record, error);
	if (status == FSIMG_OK)
		status = pack_tree(e, &apfs_pack_snapshot, &plan->snapshot_tree,
		    false, no_record, error);
	if (status == FSIMG_OK)
		status = emit_omap(e, plan->vomap_run,
		    tree_root_block(plan, &plan->vomap_tree), 0, error);
	if (status == FSIMG_OK)
		status = emit_volume(e, error);
	if (status == FSIMG_OK)
		status = pack_tree(e, &apfs_pack_container_omap,
		    &plan->comap_tree, false, container_omap_record, error);
	if (status == FSIMG_OK)
		status = emit_omap(e, plan->comap_run,
		    tree_root_block(plan, &plan->comap_tree),
		    APFS_OMAP_MANUALLY_MANAGED, error);
	return (status);
}

/* Step 4: the space manager */

/*
 * The internal pool's blocks, ascending from its base: with no address
 * blocks, the chunk-information blocks; with them, address block j at
 * j * (per_cab + 1) followed by the chunk-information blocks it names.
 * The bitmap block of chunk c follows them all, at c past their end; the
 * allocation touches chunks 0 to pool_bitmaps - 1, a prefix.
 */
static uint64_t
pool_cib(const apfs_plan_t *plan, uint64_t i)
{
	uint64_t per_cab = plan->sm.cibs_per_cab;

	if (plan->pool_cabs == 0)
		return (plan->sm.ip_base + i);
	return (plan->sm.ip_base + i + i / per_cab + 1);
}

static uint64_t
pool_cab(const apfs_plan_t *plan, uint64_t j)
{
	return (plan->sm.ip_base + j * (plan->sm.cibs_per_cab + 1));
}

static uint64_t
pool_bitmap(const apfs_plan_t *plan, uint64_t c)
{
	return (plan->sm.ip_base + plan->pool_cabs + plan->pool_cibs + c);
}

/* Set the first count bits of zeroed bits, least significant first. */
static void
bits_fill(unsigned char *bits, uint64_t count)
{
	memset(bits, 0xff, (size_t)(count / 8));
	if (count % 8 != 0)
		bits[count / 8] = (unsigned char)((1u << (count % 8)) - 1);
}

/* The blocks of [first, first + count) below end, the allocation's end. */
static uint64_t
allocated_in(uint64_t first, uint64_t count, uint64_t end)
{
	if (end <= first)
		return (0);
	return (end - first < count ? end - first : count);
}

/* Seal and write a chunk-information block holding count entries. */
static fsimg_status_t
write_cib(apfs_emitter_t *e, unsigned char *cib, uint64_t index,
    uint32_t count, fsimg_error_t *error)
{
	uint64_t block = pool_cib(e->plan, index);
	apfs_cib_t header = { .index = (uint32_t)index, .count = count };
	fsimg_status_t status;

	if (!apfs_cib_encode(cib, BLOCK, &header))
		return (diverged("APFS chunk-information block does not "
		    "encode", error));
	status = seal(cib, BLOCK, block, APFS_OBJ_PHYSICAL,
	    APFS_KIND_SPACEMAN_CIB, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, block, cib, 1, error);
	return (status);
}

/*
 * One cursor over the chunks in ascending order. A chunk's entry counts
 * the allocated blocks inside it, which gives its free count; a chunk with
 * any gets its bitmap block, ones up to the allocation's end and zeros
 * after, and the bits past N stay zero. The entries fill the
 * chunk-information blocks in chunk order, each written when it is full
 * and the last as far as it is filled. The emission buffer holds the open
 * chunk-information block and, after it, the bitmap being written.
 * free_total receives the sum of the free counts.
 */
static fsimg_status_t
emit_chunks(apfs_emitter_t *e, uint64_t *free_total, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	const apfs_sm_device_t *main = &plan->sm.devices[APFS_SM_DEVICE_MAIN];
	uint64_t per_chunk = plan->sm.blocks_per_chunk;
	uint64_t per_cib = plan->sm.chunks_per_cib;
	uint64_t end = plan->allocated_blocks, n = plan->block_count;
	unsigned char *buffer, *cib, *bitmap;
	apfs_chunk_info_t info;
	apfs_cib_t header;
	fsimg_status_t status;

	status = block_buffer(e, 2, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	cib = buffer;
	bitmap = buffer + BLOCK;
	*free_total = 0;
	for (uint64_t c = 0; c < main->chunk_count; c++) {
		uint64_t first = c * per_chunk;
		uint64_t count = n - first < per_chunk ? n - first : per_chunk;
		uint64_t used = allocated_in(first, count, end);
		uint32_t slot = (uint32_t)(c % per_cib);

		status = charge(e, error);
		if (status != FSIMG_OK)
			return (status);
		if (used != 0) {
			if (c >= plan->pool_bitmaps)
				return (diverged("APFS allocation passes its "
				    "bitmaps", error));
			bzero(bitmap, BLOCK);
			bits_fill(bitmap, used);
			status = write_blocks(e, pool_bitmap(plan, c), bitmap,
			    1, error);
			if (status != FSIMG_OK)
				return (status);
		}
		info = (apfs_chunk_info_t){ .xid = APFS_WRITE_XID,
		    .address = first,
		    .bitmap_address = used != 0 ? pool_bitmap(plan, c) : 0,
		    .block_count = (uint32_t)count,
		    .free_count = (uint32_t)(count - used) };
		*free_total += count - used;
		/* The header's count admits the entry being encoded. */
		header = (apfs_cib_t){ .index = (uint32_t)(c / per_cib),
		    .count = slot + 1 };
		if (!apfs_chunk_info_encode(cib, BLOCK, &header, slot, &info))
			return (diverged("APFS chunk entry does not encode",
			    error));
		if (slot + 1 == per_cib || c + 1 == main->chunk_count) {
			status = write_cib(e, cib, c / per_cib, slot + 1,
			    error);
			if (status != FSIMG_OK)
				return (status);
			bzero(cib, BLOCK);
		}
	}
	return (FSIMG_OK);
}

/* The address blocks, when the geometry has any: each lists its share of
 * the chunk-information blocks in order.
 */
static fsimg_status_t
emit_cabs(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	uint64_t per_cab = plan->sm.cibs_per_cab;
	unsigned char *buffer;
	apfs_cib_t header;
	fsimg_status_t status;

	for (uint64_t j = 0; j < plan->pool_cabs; j++) {
		uint64_t first = j * per_cab;
		uint64_t count = plan->pool_cibs - first < per_cab ?
		    plan->pool_cibs - first : per_cab;

		status = charge(e, error);
		if (status == FSIMG_OK)
			status = block_buffer(e, 1, &buffer, error);
		if (status != FSIMG_OK)
			return (status);
		header = (apfs_cib_t){ .index = (uint32_t)j,
		    .count = (uint32_t)count };
		if (!apfs_cab_encode(buffer, BLOCK, &header))
			return (diverged("APFS address block does not encode",
			    error));
		for (uint64_t i = 0; i < count; i++)
			if (!apfs_cab_address_encode(buffer, BLOCK, &header,
			    (uint32_t)i, pool_cib(plan, first + i)))
				return (diverged("APFS address block does not "
				    "encode", error));
		status = seal(buffer, BLOCK, pool_cab(plan, j), APFS_OBJ_PHYSICAL,
		    APFS_KIND_SPACEMAN_CAB, APFS_SUBTYPE_NONE, error);
		if (status == FSIMG_OK)
			status = write_blocks(e, pool_cab(plan, j), buffer, 1,
			    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * The pool's bitmap, b blocks in ring slots 0 to b - 1: bit k is pool
 * block k, set exactly for the address, chunk-information and bitmap
 * blocks taken, which are the pool's first blocks.
 */
static fsimg_status_t
emit_pool_bitmap(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	uint64_t per_block = (uint64_t)BLOCK * 8;
	uint64_t taken = plan->pool_cabs + plan->pool_cibs + plan->pool_bitmaps;
	unsigned char *buffer;
	fsimg_status_t status;

	for (uint64_t k = 0; k < plan->sm.ip_bm_size_in_blocks; k++) {
		status = charge(e, error);
		if (status == FSIMG_OK)
			status = block_buffer(e, 1, &buffer, error);
		if (status != FSIMG_OK)
			return (status);
		bits_fill(buffer, allocated_in(k * per_block, per_block,
		    taken));
		status = write_blocks(e, plan->sm.ip_bm_base + k, buffer, 1,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * The space manager's arrays, at the offsets the geometry packed: every
 * bitmap block current at XID 1 in the ring slot of its own index; the
 * ring's free-next list chaining the free slots b to 16 * b - 1, the
 * current slots and the last ending it; the address array naming the
 * address blocks, or the chunk-information blocks when there are none.
 */
static void
encode_sm_arrays(const apfs_plan_t *plan, unsigned char *object)
{
	const apfs_spaceman_t *sm = &plan->sm;
	const apfs_sm_device_t *main = &sm->devices[APFS_SM_DEVICE_MAIN];
	uint64_t b = sm->ip_bm_size_in_blocks, slots = sm->ip_bm_block_count;

	for (uint64_t k = 0; k < b; k++) {
		fsimg_store_le64(object + sm->ip_bm_xid_offset + 8 * k,
		    APFS_WRITE_XID);
		fsimg_store_le16(object + sm->ip_bitmap_offset + 2 * k,
		    (uint16_t)k);
	}
	for (uint64_t s = 0; s < slots; s++)
		fsimg_store_le16(object + sm->ip_bm_free_next_offset + 2 * s,
		    s < b || s + 1 == slots ? APFS_SM_RING_FREE_END :
		    (uint16_t)(s + 1));
	if (plan->pool_cabs != 0)
		for (uint64_t j = 0; j < plan->pool_cabs; j++)
			fsimg_store_le64(object + main->addr_offset + 8 * j,
			    pool_cab(plan, j));
	else
		for (uint64_t i = 0; i < plan->pool_cibs; i++)
			fsimg_store_le64(object + main->addr_offset + 8 * i,
			    pool_cib(plan, i));
}

/*
 * The space manager object, S blocks in data slots 0 to S - 1: the
 * geometry's fields, the device's free count, empty free queues in the
 * zero-OID form, and the pool ring's head and tail.
 */
static fsimg_status_t
emit_spaceman(apfs_emitter_t *e, uint64_t free_total, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	const apfs_ephemeral_t *object = &plan->ephemeral[0];
	uint64_t blocks = plan->sm_blocks, b = plan->sm.ip_bm_size_in_blocks;
	apfs_spaceman_t sm = plan->sm;
	unsigned char *buffer;
	fsimg_status_t status;

	if (free_total != plan->block_count - plan->allocated_blocks)
		return (diverged("APFS free count differs from the "
		    "allocation", error));
	sm.devices[APFS_SM_DEVICE_MAIN].free_count = free_total;
	sm.flags = SM_FLAGS;
	sm.version = SM_VERSION;
	sm.struct_size = APFS_SM_BYTES;
	sm.ip_bm_free_head = (uint16_t)b;
	sm.ip_bm_free_tail = (uint16_t)(sm.ip_bm_block_count - 1);
	status = block_buffer(e, blocks, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_spaceman_encode(buffer, (size_t)(blocks * BLOCK), &sm))
		return (diverged("APFS space manager does not encode", error));
	encode_sm_arrays(plan, buffer);
	status = seal(buffer, (size_t)(blocks * BLOCK), object->oid,
	    APFS_OBJ_EPHEMERAL, APFS_KIND_SPACEMAN, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, plan->nx.data_base + object->first_slot,
		    buffer, blocks, error);
	return (status);
}

/*
 * Step 4, after the checkpoint's runs are proved inside their areas: the
 * descriptor run is two slots and the data run the space manager's S and
 * the reaper's one, so nothing step 5 writes leaves its area.
 */
static fsimg_status_t
emit_space(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	uint64_t free_total;
	fsimg_status_t status;

	if (plan->nx.desc_blocks < 2 ||
	    plan->nx.data_blocks < plan->sm_blocks + 1)
		return (diverged("APFS checkpoint areas are too small",
		    error));
	status = emit_chunks(e, &free_total, error);
	if (status == FSIMG_OK)
		status = emit_cabs(e, error);
	if (status == FSIMG_OK)
		status = emit_pool_bitmap(e, error);
	if (status == FSIMG_OK)
		status = emit_spaceman(e, free_total, error);
	return (status);
}

/* Step 5: the checkpoint */

/* The idle reaper in its data slot. */
static fsimg_status_t
emit_reaper(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_ephemeral_t *object = &e->plan->ephemeral[1];
	apfs_reaper_t reaper = { .next_reap_id = REAPER_NEXT_ID,
	    .flags = APFS_REAPER_FLAG_RESERVED,
	    .state_size = BLOCK - APFS_REAPER_BYTES };
	unsigned char *buffer;
	fsimg_status_t status;

	status = block_buffer(e, 1, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_reaper_encode(buffer, BLOCK, &reaper))
		return (diverged("APFS reaper does not encode", error));
	status = seal(buffer, BLOCK, object->oid, APFS_OBJ_EPHEMERAL,
	    APFS_KIND_NX_REAPER, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, e->plan->nx.data_base +
		    object->first_slot, buffer, 1, error);
	return (status);
}

/*
 * The mapping block in descriptor slot 0: one mapping per ephemeral object,
 * in the table's order, so the mappings and the objects cannot disagree.
 */
static fsimg_status_t
emit_mappings(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	uint64_t block = plan->nx.desc_base;
	apfs_cpm_block_t header = { .flags = APFS_CPM_LAST,
	    .count = APFS_EPHEMERAL_COUNT };
	unsigned char *buffer;
	apfs_cpm_t map;
	fsimg_status_t status;

	status = block_buffer(e, 1, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_cpm_block_encode(buffer, BLOCK, &header))
		return (diverged("APFS mapping block does not encode", error));
	for (uint32_t i = 0; i < APFS_EPHEMERAL_COUNT; i++) {
		const apfs_ephemeral_t *object = &plan->ephemeral[i];

		map = (apfs_cpm_t){ .oid = object->oid,
		    .address = plan->nx.data_base + object->first_slot,
		    .flags = APFS_OBJ_EPHEMERAL, .subtype = APFS_SUBTYPE_NONE,
		    .size = object->bytes, .kind = (uint16_t)object->kind };
		if (!apfs_cpm_encode(buffer, BLOCK, &header, i, &map))
			return (diverged("APFS mapping does not encode",
			    error));
	}
	status = seal(buffer, BLOCK, block, APFS_OBJ_PHYSICAL,
	    APFS_KIND_CHECKPOINT_MAP, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, block, buffer, 1, error);
	return (status);
}

/*
 * The container superblock in descriptor slot 1, after its mapping block,
 * and its copy in block 0. The descriptor run is slots 0 and 1, the data
 * run the space manager's S slots and the reaper's.
 */
static fsimg_status_t
emit_superblock(apfs_emitter_t *e, fsimg_error_t *error)
{
	const apfs_plan_t *plan = e->plan;
	uint32_t data_len = (uint32_t)plan->sm_blocks + 1;
	apfs_nx_t nx = plan->nx;
	unsigned char *buffer;
	fsimg_status_t status;

	nx.magic = APFS_NX_MAGIC;
	nx.incompat_features = NX_INCOMPAT;
	bcopy(plan->container_uuid, nx.uuid, sizeof(nx.uuid));
	nx.next_oid = plan->next_oid;
	nx.next_xid = APFS_WRITE_XID + 1;
	nx.spaceman_oid = APFS_WRITE_OID_SPACEMAN;
	nx.reaper_oid = APFS_WRITE_OID_REAPER;
	nx.omap_oid = apfs_run_block(plan, plan->comap_run);
	nx.volume_oids[0] = APFS_WRITE_OID_VOLUME;
	nx.desc_index = 0;
	nx.desc_len = 2;
	nx.desc_next = 2;
	nx.data_index = 0;
	nx.data_len = data_len;
	nx.data_next = data_len;

	status = block_buffer(e, 1, &buffer, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_nx_encode(buffer, BLOCK, &nx))
		return (diverged("APFS container superblock does not encode",
		    error));
	status = seal(buffer, BLOCK, 1, APFS_OBJ_EPHEMERAL,
	    APFS_KIND_NX_SUPERBLOCK, APFS_SUBTYPE_NONE, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, nx.desc_base + 1, buffer, 1, error);
	if (status == FSIMG_OK)
		status = write_blocks(e, 0, buffer, 1, error);
	return (status);
}

static fsimg_status_t
emit_checkpoint(apfs_emitter_t *e, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = emit_reaper(e, error);
	if (status == FSIMG_OK)
		status = emit_mappings(e, error);
	if (status == FSIMG_OK)
		status = emit_superblock(e, error);
	return (status);
}

fsimg_status_t
apfs_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	apfs_emitter_t *e;
	fsimg_error_t detail;
	fsimg_status_t status;

	e = fsimg_alloc(writer->ctx, sizeof(*e), &detail);
	if (e == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(e, sizeof(*e));
	e->plan = (apfs_plan_t *)writer->plan;
	e->writer = writer;
	e->emit = emit;

	status = emit_payload(e, error);
	if (status == FSIMG_OK)
		status = emit_trees(e, error);
	if (status == FSIMG_OK)
		status = emit_space(e, error);
	if (status == FSIMG_OK)
		status = emit_checkpoint(e, error);
	fsimg_dealloc(e);
	return (status);
}
