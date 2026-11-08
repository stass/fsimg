/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "write-erofs.h"

/*
 * Emission (specs/design-erofs-driver.md, Emission). The common writer has
 * zeroed the volume, so the boot area, the gaps between inodes, the unused
 * bytes of every block and any blocks or partial block volume_bytes adds
 * are zero already, and the driver writes only bytes that carry something.
 * In catalog ID order, root first, it writes each inode's fixed part, then
 * a directory's blocks or a file's or link's content, whose tail under the
 * flat-inline layout lands in the body just after the fixed part; a
 * compressed file's index follows its fixed part and its clusters come
 * through an encoded copy (write-lz4.c). Under a metadata box the inodes
 * the box holds are not written there: once every row's data is out, the
 * box's content, every such inode with its index in plan order, is
 * generated one block at a time and stored as the box's blocks, plain or
 * through the cluster former, and the box inode is written at NID 37. The
 * superblock comes last.
 *
 * The superblock checksum covers [1024, B) for B above 1024 and
 * [1024, 1024 + B) otherwise: the superblock and whatever inodes and inline
 * tails follow it in that range. The emission object cannot read the sink
 * back, so the driver keeps those bytes itself in covered. put(), copy()
 * and the encoded copy are the only calls into the emission object: put()
 * mirrors the part of each metadata write that falls inside the covered
 * range, and cover_copy() observes each source copy and encoded copy,
 * mirroring every byte they write there. Since the volume starts
 * zero, covered holds the range's final bytes once the last inode is
 * written, and the checksum is computed over it last.
 */

_Static_assert(2 * (1u << EROFS_BLKSZBITS_MAX) <= FSIMG_EMIT_BUFFER_BYTES,
    "the emission buffer holds a directory block and its name staging");

/* The checksum's covered range is at most the superblock region. */
#define COVERED_MAX		EROFS_SUPER_REGION_BYTES

/*
 * The emission state. super is the superblock, which also supplies the
 * compact inodes' time base; its root NID is filled from row 0 and its
 * checksum last. directory is the row of the directory whose blocks
 * erofs_dir_walk() is delivering. covered mirrors volume bytes [1024,
 * 1024 + covered_bytes), the checksum's range. former forms LZ4 clusters,
 * allocated only when the plan compresses a file.
 */
typedef struct erofs_emit_state {
	erofs_plan_t	*plan;
	fsimg_emit_t	*emit;
	erofs_lz4_former_t *former;
	erofs_super_t	 super;
	erofs_row_t	 directory;
	size_t		 covered_bytes;
	unsigned char	 covered[COVERED_MAX];
} erofs_emit_state_t;

/*
 * The metadata box's content generator (box_produce()) places the box's
 * inodes, each with its index, at their offsets in the content and hands
 * the content to consume one block at a time, in order. Invariant: block
 * holds content bytes [block_index * B, (block_index + 1) * B), every
 * earlier block has been handed to consume, and the bytes of block not yet
 * written are zero. super supplies the compact inodes' time base; index
 * stages an index's records, as the emission buffer does for an inode in
 * the primary area.
 */
typedef struct erofs_box_content {
	erofs_plan_t	*plan;
	erofs_super_t	 super;
	unsigned char	*block;
	unsigned char	*index;
	uint64_t	 block_index;
	erofs_consume_t	 consume;
	void		*consume_argument;
} erofs_box_content_t;

/* The plain box store's state: the next box block to write. */
typedef struct plain_box_run {
	erofs_emit_state_t *state;
	uint64_t	 next;
} plain_box_run_t;

static void cover(erofs_emit_state_t *, uint64_t, const unsigned char *,
    size_t);
static void cover_copy(void *, const unsigned char *, size_t, uint64_t);
static fsimg_status_t put(erofs_emit_state_t *, uint64_t, const void *,
    size_t, fsimg_error_t *);
static fsimg_status_t put_index(void *, uint64_t, const void *, size_t,
    fsimg_error_t *);
static fsimg_status_t copy(erofs_emit_state_t *, uint64_t,
    fsimg_emit_payload_t *, fsimg_error_t *);
static fsimg_status_t corrupt_plan(fsimg_error_t *, const char *);
static uint64_t inode_offset(const erofs_row_t *);
static bool row_placed(const erofs_plan_t *, uint64_t, const erofs_row_t *);
static bool blocks_placed(const erofs_plan_t *, const erofs_row_t *,
    uint64_t, uint64_t);
static bool row_kind(const erofs_row_t *, fsimg_entry_kind_t *, uint8_t *);
static void super_init(const erofs_plan_t *, erofs_super_t *);
static bool encode_row(const erofs_super_t *, uint64_t, const erofs_row_t *,
    unsigned char *);
static fsimg_status_t emit_inode(erofs_emit_state_t *, uint64_t,
    const erofs_row_t *, fsimg_error_t *);
static fsimg_status_t resolve_entry(void *, uint64_t, erofs_dir_entry_t *,
    fsimg_error_t *);
static fsimg_status_t write_dir_block(void *, uint64_t, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t emit_directory(erofs_emit_state_t *, uint64_t,
    const erofs_row_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t emit_content(erofs_emit_state_t *, uint64_t,
    const erofs_row_t *, fsimg_error_t *);
static fsimg_status_t emit_superblock(erofs_emit_state_t *, fsimg_error_t *);
static fsimg_status_t emit_rows(erofs_emit_state_t *, fsimg_error_t *);
static fsimg_status_t box_flush(erofs_box_content_t *, fsimg_error_t *);
static fsimg_status_t box_put(void *, uint64_t, const void *, size_t,
    fsimg_error_t *);
static fsimg_status_t box_produce(void *, erofs_consume_t, void *,
    fsimg_error_t *);
static fsimg_status_t box_content_init(erofs_box_content_t *,
    erofs_plan_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t store_plain_box(void *, const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t emit_box(erofs_emit_state_t *, fsimg_error_t *);

/*
 * Mirror the part of the volume range [offset, offset + length) that lies
 * inside the covered range into covered. The range is one emission has
 * already written, inside the volume, so its end does not overflow.
 */
static void
cover(erofs_emit_state_t *state, uint64_t offset, const unsigned char *bytes,
    size_t length)
{
	const uint64_t covered_end = EROFS_SUPER_OFFSET + state->covered_bytes;
	uint64_t start, end;

	start = offset > EROFS_SUPER_OFFSET ? offset : EROFS_SUPER_OFFSET;
	end = offset + length < covered_end ? offset + length : covered_end;
	if (start >= end)
		return;
	bcopy(bytes + (start - offset),
	    state->covered + (start - EROFS_SUPER_OFFSET),
	    (size_t)(end - start));
}

/* The source copy's observer: every byte a copy writes, at its offset. */
static void
cover_copy(void *argument, const unsigned char *bytes, size_t length,
    uint64_t offset)
{
	cover(argument, offset, bytes, length);
}

/* Write metadata bytes at a volume offset and mirror what is covered. */
static fsimg_status_t
put(erofs_emit_state_t *state, uint64_t offset, const void *bytes,
    size_t length, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_emit_write(state->emit, offset, bytes, length, error);
	if (status == FSIMG_OK)
		cover(state, offset, bytes, length);
	return (status);
}

/* put() for the LZ4 index writer. */
static fsimg_status_t
put_index(void *argument, uint64_t offset, const void *bytes, size_t length,
    fsimg_error_t *error)
{
	return (put(argument, offset, bytes, length, error));
}

/*
 * Copy catalog entry id into the regions payload names, with cover_copy()
 * observing every byte written.
 */
static fsimg_status_t
copy(erofs_emit_state_t *state, uint64_t id, fsimg_emit_payload_t *payload,
    fsimg_error_t *error)
{
	payload->observe = cover_copy;
	payload->observer_argument = state;
	return (fsimg_emit_copy(state->emit, id, payload, error));
}

static fsimg_status_t
corrupt_plan(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
	    message));
}

/*
 * The inode's byte offset in its space: meta_blkaddr is 0, so the NID
 * times 32, in the volume, or in the box's content for a NID with
 * EROFS_NID_BOX.
 */
static uint64_t
inode_offset(const erofs_row_t *row)
{
	return ((row->nid & ~EROFS_NID_BOX) * EROFS_NID_SLOT_BYTES);
}

/*
 * Whether catalog row id lies where planning put it: in the space
 * erofs_row_in_box() names, its NID carrying EROFS_NID_BOX exactly when
 * that is the box; its body inside the box's content, or inside the
 * primary metadata area after the superblock and any box inode; and its
 * data blocks inside the data area, which ends where the box's blocks
 * begin (blocks_placed()). Every write emission makes for the row is
 * derived from these fields, so a row that passes cannot reach the
 * superblock or another area. That includes an inline tail, written at a
 * volume offset after the fixed part, because erofs_row_get() refuses an
 * inline tail under a box, where the offset would be a box offset.
 */
static bool
row_placed(const erofs_plan_t *plan, uint64_t id, const erofs_row_t *row)
{
	const bool in_box = erofs_row_in_box(plan, id);
	const uint64_t data_start = plan->metadata_end / plan->block_bytes;
	uint64_t first, offset, body;

	if (((row->nid & EROFS_NID_BOX) != 0) != in_box)
		return (false);
	offset = inode_offset(row);
	body = erofs_row_body_bytes(plan, row);
	if (in_box) {
		/* An inline tail would be written at a box offset. */
		if (row->layout == EROFS_LAYOUT_FLAT_INLINE ||
		    offset > plan->box.size || body > plan->box.size - offset)
			return (false);
		return (blocks_placed(plan, row, data_start, plan->data_end));
	}
	first = plan->metabox != FSIMG_EROFS_METABOX_NONE ?
	    EROFS_WRITE_BOX_NID * EROFS_NID_SLOT_BYTES + plan->box_body :
	    EROFS_WRITE_METADATA_START;
	if (offset < first || offset > plan->metadata_end ||
	    body > plan->metadata_end - offset)
		return (false);
	return (blocks_placed(plan, row, data_start, plan->data_end));
}

/*
 * Whether the row's data blocks, if any, lie in [start, end), the area
 * planning gives them, which ends within the addressing's block limit; a
 * row that stores none keeps start block 0.
 */
static bool
blocks_placed(const erofs_plan_t *plan, const erofs_row_t *row,
    uint64_t start, uint64_t end)
{
	const uint32_t block_bytes = plan->block_bytes;
	uint64_t blocks;

	blocks = erofs_row_data_blocks(row, block_bytes);
	if (blocks == 0)
		return (row->start_block == 0);
	return (row->start_block >= start && row->start_block <= end &&
	    blocks <= end - row->start_block);
}

/*
 * The row's kind and its directory entries' file type, for the four kinds
 * the writer stores; false for any other mode.
 */
static bool
row_kind(const erofs_row_t *row, fsimg_entry_kind_t *kind,
    uint8_t *file_type)
{
	switch (row->mode & EROFS_MODE_TYPE_MASK) {
	case EROFS_MODE_REGULAR:
		*kind = FSIMG_ENTRY_FILE;
		*file_type = EROFS_FT_REGULAR;
		return (true);
	case EROFS_MODE_DIRECTORY:
		*kind = FSIMG_ENTRY_DIRECTORY;
		*file_type = EROFS_FT_DIRECTORY;
		return (true);
	case EROFS_MODE_SYMLINK:
		*kind = FSIMG_ENTRY_SYMLINK;
		*file_type = EROFS_FT_SYMLINK;
		return (true);
	case EROFS_MODE_FIFO:
		*kind = FSIMG_ENTRY_FIFO;
		*file_type = EROFS_FT_FIFO;
		return (true);
	default:
		return (false);
	}
}

/*
 * The superblock of the writer profile (specs/design-erofs.md, Volume),
 * all but the root NID and the checksum: features SB_CHKSUM and MTIME,
 * metadata from block 0, the plan's epoch with zero nanoseconds, and the
 * label raw and NUL-padded. When a file or the box is compressed,
 * LZ4_0PADDING and LZ4's maximum distance, 65535; under
 * addressing=48, 48BIT, which erofs_super_encode() writes in the wide
 * form; under a metadata box, METABOX with one extension slot holding the
 * box inode's NID. Every other field is zero. validate_options() bounds
 * the label and the block count.
 */
static void
super_init(const erofs_plan_t *plan, erofs_super_t *sb)
{
	const char *label = plan->common.writer->options.label;

	bzero(sb, sizeof(*sb));
	sb->compat = EROFS_COMPAT_SB_CHKSUM | EROFS_COMPAT_MTIME;
	sb->blkszbits = (uint8_t)(ffs((int)plan->block_bytes) - 1);
	sb->inode_count = plan->inode_count;
	sb->epoch = plan->epoch;
	sb->block_count = plan->block_count;
	bcopy(plan->uuid, sb->uuid, sizeof(sb->uuid));
	sb->name_length = (uint8_t)strlen(label);
	bcopy(label, sb->name, sb->name_length);
	if (plan->cluster_count != 0) {
		sb->incompat |= EROFS_INCOMPAT_LZ4_0PADDING;
		sb->compression_word = EROFS_LZ4_DISTANCE;
	}
	if (plan->addressing48)
		sb->incompat |= EROFS_INCOMPAT_48BIT;
	if (plan->metabox != FSIMG_EROFS_METABOX_NONE) {
		sb->incompat |= EROFS_INCOMPAT_METABOX;
		sb->extension_slots = 1;
		sb->box_nid = plan->box.nid;
	}
}

/*
 * The row's fixed part into raw, EROFS_INODE_EXTENDED_BYTES long, with
 * serial as its serial number: the row's form, layout, kind and fields.
 * The layout word is the start block, zero for an inode that stores no
 * block, or a compressed inode's physical block count, which fits 32 bits;
 * under addressing=48 the start block's bits 32 to 47 go to the word at
 * 0x06, which for a compressed inode holds the block count's high bits,
 * zero, and every compact inode, which is never a directory there, takes
 * the implicit link count of 1 that frees that word. A compact inode's
 * time is the superblock's epoch plus the offset the compact rule makes
 * exact. false means the row has no kind or does not encode.
 */
static bool
encode_row(const erofs_super_t *sb, uint64_t serial, const erofs_row_t *row,
    unsigned char *raw)
{
	erofs_inode_t inode;
	uint8_t file_type;

	bzero(&inode, sizeof(inode));
	if (!row_kind(row, &inode.kind, &file_type))
		return (false);
	inode.extended = row->extended;
	inode.layout = row->layout;
	inode.permissions = row->mode & EROFS_MODE_PERMISSIONS;
	inode.nlink = row->links;
	inode.size = row->size;
	if (row->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		inode.layout_word = row->clusters;
	} else {
		inode.layout_word = (uint32_t)row->start_block;
		inode.layout_high = (uint16_t)(row->start_block >> 32);
	}
	inode.serial = (uint32_t)serial;
	inode.uid = row->uid;
	inode.gid = row->gid;
	inode.time = row->mtime;
	inode.time_nsec = row->mtime_nsec;
	return (erofs_inode_encode(&inode, sb, raw));
}

/*
 * Check catalog entry id's row and write its inode at its NID, with its
 * 1-based position, which ID order makes id + 1, as its serial number;
 * an inode the box holds is written with the box's content instead. The
 * row is checked here, before anything of it is written, and the
 * directory and content writes that follow use the same row.
 */
static fsimg_status_t
emit_inode(erofs_emit_state_t *state, uint64_t id, const erofs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];

	if (!row_placed(state->plan, id, row) ||
	    !encode_row(&state->super, id + 1, row, raw))
		return (corrupt_plan(error,
		    "EROFS plan row lies outside its area or does not encode"));
	if (erofs_row_in_box(state->plan, id))
		return (FSIMG_OK);
	return (put(state, inode_offset(row), raw, erofs_row_fixed_bytes(row),
	    error));
}

/* A directory entry's NID and file type, from the child's plan row. */
static fsimg_status_t
resolve_entry(void *argument, uint64_t id, erofs_dir_entry_t *entry,
    fsimg_error_t *error)
{
	erofs_emit_state_t *state = argument;
	fsimg_entry_kind_t kind;
	erofs_row_t row;
	fsimg_status_t status;

	status = erofs_row_get(state->plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (!row_kind(&row, &kind, &entry->file_type))
		return (corrupt_plan(error, "EROFS plan row has no kind"));
	entry->nid = row.nid;
	return (FSIMG_OK);
}

/*
 * Write directory block index, whose first used bytes are valid. The
 * directory's size counts every block before the last as full, however
 * many of its bytes are used, so blocks 0 to full - 1 are whole data
 * blocks from the start block. Block full, the partial last block, is the
 * next data block under the flat layout and the inline tail under the
 * flat-inline one, with capacity size % B. When the size is a multiple of
 * B there is no block full: its capacity is 0, so any delivery there is
 * refused. A block outside the planned size is a plan that no longer
 * matches the catalog, refused before anything is written there.
 */
static fsimg_status_t
write_dir_block(void *argument, uint64_t index, const unsigned char *bytes,
    size_t used, fsimg_error_t *error)
{
	erofs_emit_state_t *state = argument;
	const erofs_row_t *dir = &state->directory;
	const uint32_t block_bytes = state->plan->block_bytes;
	uint64_t full, offset, capacity;

	full = dir->size / block_bytes;
	capacity = index < full ? block_bytes : dir->size % block_bytes;
	if (index > full || used > capacity)
		return (corrupt_plan(error,
		    "EROFS directory outgrew its planned size"));
	if (index == full && dir->layout == EROFS_LAYOUT_FLAT_INLINE)
		offset = inode_offset(dir) + erofs_row_fixed_bytes(dir);
	else
		offset = (dir->start_block + index) * block_bytes;
	return (put(state, offset, bytes, used, error));
}

/*
 * Directory id's blocks, packed again by the walk planning measured with,
 * so they match the planned size; a difference means the plan no longer
 * describes the catalog. The walk reuses the writer's record buffer, so
 * the parent ID is taken from the catalog before it starts. The block and
 * its name staging are assembled in the emission buffer.
 */
static fsimg_status_t
emit_directory(erofs_emit_state_t *state, uint64_t id, const erofs_row_t *row,
    uint64_t *cursor, fsimg_error_t *error)
{
	fsimg_writer_t *writer = state->plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	const erofs_dir_builder_t builder = {
		.resolve = resolve_entry,
		.block = write_dir_block,
		.argument = state,
		.pack_buffer = fsimg_emit_buffer(state->emit)
	};
	uint64_t parent_id, size;
	fsimg_status_t status;

	parent_id = 0;
	if (id != 0) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		parent_id = catalog->parent_id;
	}
	state->directory = *row;
	status = erofs_dir_walk(state->plan, cursor, id, parent_id, &builder,
	    &size, error);
	if (status != FSIMG_OK)
		return (status);
	if (size != row->size)
		return (corrupt_plan(error,
		    "EROFS directory size differs from its plan"));
	return (FSIMG_OK);
}

/*
 * A file's or link's content in one source copy: its data blocks, all of
 * them under the flat layout, a partial last block included, and the full
 * ones under the flat-inline layout, then its inline tail in the body. A
 * compressed file's map header and index come first, unless the box holds
 * them, then its clusters through an encoded copy. Either copy opens,
 * checks and closes the source, so a source changed since collection
 * fails here.
 */
static fsimg_status_t
emit_content(erofs_emit_state_t *state, uint64_t id, const erofs_row_t *row,
    fsimg_error_t *error)
{
	const uint32_t block_bytes = state->plan->block_bytes;
	fsimg_emit_region_t regions[2];
	fsimg_emit_payload_t payload = {
		.regions = regions,
		.length = row->size,
		.initialized = row->size
	};
	fsimg_status_t status;
	uint64_t blocks;
	uint32_t tail;

	if (row->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		if (state->former == NULL)
			return (corrupt_plan(error,
			    "EROFS plan compresses a file it did not count"));
		if (!erofs_row_in_box(state->plan, id)) {
			status = erofs_lz4_write_index(state->plan, row,
			    inode_offset(row) + erofs_row_fixed_bytes(row),
			    fsimg_emit_buffer(state->emit), put_index, state,
			    error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (erofs_lz4_copy(state->plan, state->former, state->emit,
		    id, row, cover_copy, state, error));
	}
	blocks = erofs_row_data_blocks(row, block_bytes);
	tail = erofs_row_tail_bytes(row, block_bytes);
	if (blocks != 0)
		regions[payload.region_count++] = (fsimg_emit_region_t){
			.image_offset = row->start_block * block_bytes,
			.capacity = blocks * block_bytes
		};
	if (tail != 0)
		regions[payload.region_count++] = (fsimg_emit_region_t){
			.image_offset = inode_offset(row) +
			    erofs_row_fixed_bytes(row),
			.capacity = tail
		};
	return (copy(state, id, &payload, error));
}

/*
 * The superblock, with its checksum computed last over the covered
 * range's final bytes, the superblock's own included with its checksum
 * field read as zero.
 */
static fsimg_status_t
emit_superblock(erofs_emit_state_t *state, fsimg_error_t *error)
{
	unsigned char raw[EROFS_SUPER_BASE_BYTES + EROFS_SUPER_SLOT_BYTES];
	erofs_super_t *sb = &state->super;
	size_t length;

	length = EROFS_SUPER_BASE_BYTES +
	    (size_t)sb->extension_slots * EROFS_SUPER_SLOT_BYTES;
	if (length > sizeof(raw) || !erofs_super_encode(sb, raw))
		return (corrupt_plan(error,
		    "EROFS superblock does not encode"));
	cover(state, EROFS_SUPER_OFFSET, raw, length);
	if (!erofs_super_checksum(state->covered, state->covered_bytes,
	    sb->blkszbits, &sb->checksum))
		return (corrupt_plan(error, "EROFS superblock checksum range"));

	/* The checksum is the only field that changes. */
	if (!erofs_super_encode(sb, raw))
		return (corrupt_plan(error,
		    "EROFS superblock does not encode"));
	return (put(state, EROFS_SUPER_OFFSET, raw, length, error));
}

/*
 * Emit the plan: every row in ID order, its inode and then its content,
 * then the metadata box, then the superblock. The cluster former exists
 * for the whole emission when the plan compresses a file or the box, and
 * is released on every outcome.
 */
fsimg_status_t
erofs_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	erofs_plan_t *plan = (erofs_plan_t *)writer->plan;
	erofs_emit_state_t state;
	fsimg_status_t status;

	bzero(&state, sizeof(state));
	state.plan = plan;
	state.emit = emit;
	super_init(plan, &state.super);
	state.covered_bytes = erofs_super_covered_bytes(state.super.blkszbits);
	if (state.covered_bytes == 0 || state.covered_bytes > COVERED_MAX)
		return (corrupt_plan(error, "EROFS block size"));
	if (plan->cluster_count != 0) {
		status = erofs_lz4_former_new(writer->ctx, &writer->session,
		    plan->block_bytes, &state.former, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = emit_rows(&state, error);
	if (status == FSIMG_OK)
		status = emit_box(&state, error);
	erofs_lz4_former_free(state.former);
	if (status != FSIMG_OK)
		return (status);
	return (emit_superblock(&state, error));
}

/*
 * Every row in ID order. Directories are walked in increasing ID order, as
 * planning walked them, so one child cursor serves them all.
 */
static fsimg_status_t
emit_rows(erofs_emit_state_t *state, fsimg_error_t *error)
{
	fsimg_writer_t *writer = state->plan->common.writer;
	erofs_row_t row;
	uint64_t child_cursor;
	fsimg_status_t status;

	child_cursor = 0;
	for (uint64_t id = 0; id <= writer->count; id++) {
		status = erofs_row_get(state->plan, id, &row, error);
		if (status == FSIMG_OK)
			status = emit_inode(state, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (id == 0)
			state->super.root_nid = row.nid;
		switch (row.mode & EROFS_MODE_TYPE_MASK) {
		case EROFS_MODE_DIRECTORY:
			status = emit_directory(state, id, &row, &child_cursor,
			    error);
			break;
		case EROFS_MODE_REGULAR:
		case EROFS_MODE_SYMLINK:
			status = emit_content(state, id, &row, error);
			break;
		case EROFS_MODE_FIFO:
			break;
		default:
			status = corrupt_plan(error,
			    "EROFS plan row has no kind");
			break;
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Hand the assembled block to the consumer and start the next one, zero.
 */
static fsimg_status_t
box_flush(erofs_box_content_t *content, fsimg_error_t *error)
{
	const uint32_t block_bytes = content->plan->block_bytes;
	fsimg_status_t status;

	status = content->consume(content->consume_argument, content->block,
	    block_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(content->block, block_bytes);
	content->block_index++;
	return (FSIMG_OK);
}

/*
 * Place bytes at offset in the box's content: the erofs_put_t of the box,
 * for inodes and the index writer. Writes come in increasing offset order,
 * since bodies are generated in plan order and each from its start, so
 * every block before the one offset falls in is finished and handed on
 * first; a write behind the open block or past the content is a plan that
 * no longer holds.
 */
static fsimg_status_t
box_put(void *argument, uint64_t offset, const void *bytes, size_t length,
    fsimg_error_t *error)
{
	erofs_box_content_t *content = argument;
	const uint32_t block_bytes = content->plan->block_bytes;
	const unsigned char *from = bytes;
	uint64_t size = content->plan->box.size;
	size_t within, take;
	fsimg_status_t status;

	if (offset / block_bytes < content->block_index || offset > size ||
	    length > size - offset)
		return (corrupt_plan(error,
		    "EROFS metadata box write lies outside its order"));
	while (length != 0) {
		while (offset / block_bytes > content->block_index) {
			status = box_flush(content, error);
			if (status != FSIMG_OK)
				return (status);
		}
		within = (size_t)(offset % block_bytes);
		take = block_bytes - within;
		if (take > length)
			take = length;
		bcopy(from, content->block + within, take);
		from += take;
		offset += take;
		length -= take;
	}
	return (FSIMG_OK);
}

/*
 * The erofs_produce_t of the box: generate its content, every inode the
 * box holds in ID order with its fixed part and, for a compressed file,
 * its map header and index or encoded extents from the cluster records,
 * hand every block of the content to consume, the last ones too, whatever
 * they hold, and end the content. The bytes between bodies and after the
 * last are zero, since each block starts zero. Each row is checked as
 * emission checks a primary row before anything of it is placed.
 */
static fsimg_status_t
box_produce(void *argument, erofs_consume_t consume, void *consume_argument,
    fsimg_error_t *error)
{
	erofs_box_content_t *content = argument;
	erofs_plan_t *plan = content->plan;
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_row_t row;
	uint64_t offset;
	fsimg_status_t status;

	content->consume = consume;
	content->consume_argument = consume_argument;
	content->block_index = 0;
	bzero(content->block, plan->block_bytes);
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		if (!erofs_row_in_box(plan, id))
			continue;
		status = erofs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (!row_placed(plan, id, &row) ||
		    !encode_row(&content->super, id + 1, &row, raw))
			return (corrupt_plan(error,
			    "EROFS plan row lies outside its area or does not "
			    "encode"));
		offset = inode_offset(&row);
		status = box_put(content, offset, raw,
		    erofs_row_fixed_bytes(&row), error);
		if (status == FSIMG_OK &&
		    row.layout == EROFS_LAYOUT_COMPRESSED_FULL)
			status = erofs_lz4_write_index(plan, &row,
			    offset + erofs_row_fixed_bytes(&row),
			    content->index, box_put, content, error);
		if (status != FSIMG_OK)
			return (status);
	}
	while (content->block_index < plan->box.size / plan->block_bytes) {
		status = box_flush(content, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (consume(consume_argument, NULL, 0, error));
}

/*
 * Prepare a box content generator over the plan, with index as its index
 * staging of FSIMG_EMIT_BUFFER_BYTES and a block of its own, allocated
 * through the writer's context; the caller releases content->block.
 */
static fsimg_status_t
box_content_init(erofs_box_content_t *content, erofs_plan_t *plan,
    unsigned char *index, fsimg_error_t *error)
{
	fsimg_error_t detail;

	bzero(content, sizeof(*content));
	content->plan = plan;
	content->index = index;
	super_init(plan, &content->super);
	content->block = fsimg_alloc(plan->common.writer->ctx,
	    plan->block_bytes, &detail);
	if (content->block == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	return (FSIMG_OK);
}

/*
 * Planning's measured encoding of an LZ4 box (pass 5): every NID and start
 * block the content holds is final, so the content is generated once and
 * formed into clusters whose records follow the files' in the plan's
 * cluster records, created here when no file is compressed. The box's
 * cluster count must fit the layout word's 32 bits.
 */
fsimg_status_t
erofs_box_measure(erofs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	erofs_box_content_t content;
	erofs_lz4_former_t *former;
	unsigned char *index;
	fsimg_error_t detail;
	uint64_t clusters;
	fsimg_status_t status;

	if (plan->cluster_records == NULL) {
		status = fsimg_store_new(&writer->session,
		    &plan->cluster_records, error);
		if (status != FSIMG_OK)
			return (status);
	}
	index = fsimg_alloc(writer->ctx, FSIMG_EMIT_BUFFER_BYTES, &detail);
	if (index == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = box_content_init(&content, plan, index, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(index);
		return (status);
	}
	status = erofs_lz4_former_new(writer->ctx, &writer->session,
	    plan->block_bytes, &former, error);
	if (status == FSIMG_OK) {
		status = erofs_lz4_box_measure(plan, former, box_produce,
		    &content, &clusters, error);
		erofs_lz4_former_free(former);
	}
	fsimg_dealloc(content.block);
	fsimg_dealloc(index);
	if (status != FSIMG_OK)
		return (status);
	/*
	 * The former forms at most one cluster per block of content, the
	 * extents an addressing=48 body reserves room for.
	 */
	if (clusters == 0 ||
	    clusters > erofs_row_index_records(&plan->box, plan->block_bytes))
		return (corrupt_plan(error,
		    "EROFS metadata box clusters differ from its blocks"));
	if (clusters > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "EROFS metadata box clusters exceed 32 bits"));
	plan->box.clusters = (uint32_t)clusters;
	plan->box.first_cluster = plan->cluster_count;
	plan->cluster_count += clusters;
	return (FSIMG_OK);
}

/*
 * The plain box store's consumer: each content block goes to the box's
 * next stored block; the end call checks that every block came.
 */
static fsimg_status_t
store_plain_box(void *argument, const unsigned char *bytes, size_t length,
    fsimg_error_t *error)
{
	plain_box_run_t *run = argument;
	erofs_plan_t *plan = run->state->plan;
	const uint64_t blocks = erofs_row_data_blocks(&plan->box,
	    plan->block_bytes);
	fsimg_status_t status;

	if (length == 0)
		return (run->next == blocks ? FSIMG_OK : corrupt_plan(error,
		    "EROFS metadata box differs from its plan"));
	if (run->next >= blocks || length != plan->block_bytes)
		return (corrupt_plan(error,
		    "EROFS metadata box differs from its plan"));
	status = put(run->state,
	    (plan->box.start_block + run->next) * plan->block_bytes, bytes,
	    length, error);
	if (status == FSIMG_OK)
		run->next++;
	return (status);
}

/*
 * Under a metadata box, store its content as its blocks, after every row's
 * data: plain, or through the cluster former under metabox=lz4, the index
 * staged in the emission buffer and the content block allocated here.
 * Then the box inode at NID 37, with its index or encoded extents under
 * metabox=lz4, and the serial number after every catalog inode's. The box
 * inode is checked first: the NID planning gives it, its body inside the
 * primary area and its blocks after the data, up to used_blocks.
 */
static fsimg_status_t
emit_box(erofs_emit_state_t *state, fsimg_error_t *error)
{
	erofs_plan_t *plan = state->plan;
	const erofs_row_t *box = &plan->box;
	const uint64_t offset = inode_offset(box);
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_box_content_t content;
	plain_box_run_t run = { .state = state };
	fsimg_status_t status;

	if (plan->metabox == FSIMG_EROFS_METABOX_NONE)
		return (FSIMG_OK);
	if (box->nid != EROFS_WRITE_BOX_NID ||
	    offset + plan->box_body > plan->metadata_end ||
	    !blocks_placed(plan, box, plan->data_end, plan->used_blocks) ||
	    !encode_row(&state->super, plan->inode_count, box, raw))
		return (corrupt_plan(error,
		    "EROFS metadata box lies outside its area"));
	if (box->layout == EROFS_LAYOUT_COMPRESSED_FULL && state->former == NULL)
		return (corrupt_plan(error,
		    "EROFS plan compresses a box it did not count"));

	if (box->size != 0) {
		status = box_content_init(&content, plan,
		    fsimg_emit_buffer(state->emit), error);
		if (status != FSIMG_OK)
			return (status);
		if (box->layout == EROFS_LAYOUT_COMPRESSED_FULL)
			status = erofs_lz4_box_store(plan, state->former,
			    box_produce, &content, put_index, state, error);
		else
			status = box_produce(&content, store_plain_box, &run,
			    error);
		fsimg_dealloc(content.block);
		if (status != FSIMG_OK)
			return (status);
	}

	status = put(state, offset, raw, erofs_row_fixed_bytes(box), error);
	if (status != FSIMG_OK || box->layout != EROFS_LAYOUT_COMPRESSED_FULL)
		return (status);
	return (erofs_lz4_write_index(plan, box,
	    offset + erofs_row_fixed_bytes(box), fsimg_emit_buffer(state->emit),
	    put_index, state, error));
}
