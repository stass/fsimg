/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "write-hfsplus.h"

/* Emission (specs/design-hfsplus-driver.md, Emission). The common emission
 * object has already zeroed the sink, so the boot area, every unused node,
 * the free blocks and the file tails past each payload are zero already. The
 * driver writes, in order: the payload of every file and symbolic link, the
 * catalog tree, the extents and attributes trees' header nodes, the bitmap,
 * and the two identical headers, the alternate last. Every byte comes from a
 * row, a stream record, the geometry or a profile constant.
 */

/* The volume attributes: clean, and unused B-tree nodes zeroed. */
#define WRITE_ATTRIBUTES		UINT32_C(0x80000100)
/* lastMountedVersion: fsimg was the last writer. */
#define WRITE_LAST_MOUNTED		UINT32_C(0x6673696d)	/* "fsim" */
#define WRITE_ENCODINGS_BITMAP		UINT64_C(1)
#define WRITE_DATA_CLUMP_MAX		65536u
/* The Finder type and creator of a symbolic link. */
#define WRITE_LINK_TYPE			UINT32_C(0x736c6e6b)	/* "slnk" */
#define WRITE_LINK_CREATOR		UINT32_C(0x72686170)	/* "rhap" */
/* The file flag stating that a thread record exists. */
#define WRITE_FLAG_THREAD		0x0002u
/* The BSD special word: on a file, the link count; one name. */
#define WRITE_SPECIAL			1u

/* The fields both primary record layouts share, at their shared offsets. */
#define RECORD_ID			8u
#define RECORD_DATES			12u
#define RECORD_BSD			32u
#define RECORD_FINDER			48u
#define FOLDER_VALENCE			4u
#define FOLDER_COUNT			84u
#define FILE_DATA_FORK			88u
/* A thread: type (2), reserved (2), parentID (4), nameLength (2), the name. */
#define THREAD_PARENT			4u
#define THREAD_UNITS			8u
#define THREAD_NAME			10u
/* The header's fields the writer sets beyond zero. */
#define HEADER_SIGNATURE		0u
#define HEADER_VERSION			2u
#define HEADER_ATTRIBUTES		4u
#define HEADER_LAST_MOUNTED		8u
#define HEADER_CREATE_DATE		16u
#define HEADER_MODIFY_DATE		20u
#define HEADER_CHECKED_DATE		28u
#define HEADER_FILE_COUNT		32u
#define HEADER_FOLDER_COUNT		36u
#define HEADER_BLOCK_SIZE		40u
#define HEADER_TOTAL_BLOCKS		44u
#define HEADER_FREE_BLOCKS		48u
#define HEADER_NEXT_ALLOCATION		52u
#define HEADER_RSRC_CLUMP		56u
#define HEADER_DATA_CLUMP		60u
#define HEADER_NEXT_CATALOG_ID		64u
#define HEADER_ENCODINGS_BITMAP		72u
#define HEADER_FINDER_IDENTIFIER	104u
#define HEADER_ALLOCATION_FILE		112u
#define HEADER_EXTENTS_FILE		192u
#define HEADER_CATALOG_FILE		272u
#define HEADER_ATTRIBUTES_FILE		352u
#define VOLUME_IDENTIFIER_BYTES		8u

/* What the packer's callbacks need: the plan for the rows and geometry, the
 * sink, and the tree being written.
 */
typedef struct hfsplus_tree_writer {
	hfsplus_plan_t			*plan;
	fsimg_emit_t			*emit;
	const hfsplus_tree_shape_t	*shape;
} hfsplus_tree_writer_t;

static void fork_encode(unsigned char *, uint64_t, uint32_t, uint32_t,
    uint32_t);
static void tree_fork_encode(unsigned char *, const hfsplus_tree_shape_t *);
static void record_common_encode(unsigned char *, uint16_t, uint32_t,
    const hfsplus_row_t *);
static void folder_encode(const hfsplus_plan_t *, const hfsplus_row_t *,
    uint32_t, unsigned char *);
static void file_encode(const hfsplus_row_t *, uint32_t, unsigned char *);
static void thread_encode(const hfsplus_write_record_t *,
    const hfsplus_row_t *, unsigned char *);
static fsimg_status_t leaf_data(void *, const hfsplus_write_record_t *,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t node_write(void *, uint32_t, const unsigned char *,
    fsimg_error_t *);
static fsimg_status_t emit_payloads(hfsplus_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_tree(hfsplus_plan_t *, fsimg_emit_t *,
    const hfsplus_tree_shape_t *, fsimg_stream_t *, fsimg_stream_t *const *,
    fsimg_error_t *);
static unsigned char bitmap_byte(const hfsplus_plan_t *, uint64_t);
static fsimg_status_t emit_bitmap(hfsplus_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_headers(hfsplus_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t hfsplus_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

const fsimg_writer_ops_t fsimg_hfsplus_writer_ops = {
	.validate = hfsplus_write_validate,
	.plan = hfsplus_write_plan,
	.emit = hfsplus_write_emit,
	.free = hfsplus_write_free
};

/* A fork of at most one extent, stored into 80 zeroed bytes. */
static void
fork_encode(unsigned char *raw, uint64_t logical, uint32_t clump, uint32_t first,
    uint32_t blocks)
{
	fsimg_store_be64(raw, logical);
	fsimg_store_be32(raw + 8, clump);
	fsimg_store_be32(raw + 12, blocks);
	fsimg_store_be32(raw + 16, first);
	fsimg_store_be32(raw + 20, blocks);
}

static void
tree_fork_encode(unsigned char *raw, const hfsplus_tree_shape_t *shape)
{
	fork_encode(raw, hfsplus_tree_logical_bytes(shape),
	    hfsplus_tree_clump(shape), shape->first_block, shape->blocks);
}

/* What a folder and a file record have in common: the type, the flags, the
 * ID, the dates in their stored order with backupDate left zero, and the BSD
 * information with its two flag bytes left zero.
 */
static void
record_common_encode(unsigned char *raw, uint16_t flags, uint32_t node_id,
    const hfsplus_row_t *row)
{
	fsimg_store_be16(raw + 2, flags);
	fsimg_store_be32(raw + RECORD_ID, node_id);
	fsimg_store_be32(raw + RECORD_DATES, row->created);
	fsimg_store_be32(raw + RECORD_DATES + 4, row->modified);
	fsimg_store_be32(raw + RECORD_DATES + 8, row->changed);
	fsimg_store_be32(raw + RECORD_DATES + 12, row->accessed);
	fsimg_store_be32(raw + RECORD_BSD, row->uid);
	fsimg_store_be32(raw + RECORD_BSD + 4, row->gid);
	fsimg_store_be16(raw + RECORD_BSD + 10, row->mode);
	fsimg_store_be32(raw + RECORD_BSD + 12, WRITE_SPECIAL);
}

/* The folder count is HFSX's alone: the flag and the field go together. */
static void
folder_encode(const hfsplus_plan_t *plan, const hfsplus_row_t *row,
    uint32_t node_id, unsigned char *raw)
{
	bool counted = plan->variant == HFSPLUS_VARIANT_X;

	fsimg_store_be16(raw, HFSPLUS_RECORD_FOLDER);
	record_common_encode(raw, counted ? HFSPLUS_FLAG_FOLDER_COUNT : 0,
	    node_id, row);
	fsimg_store_be32(raw + FOLDER_VALENCE, row->valence);
	if (counted)
		fsimg_store_be32(raw + FOLDER_COUNT, row->folders);
}

/* A FIFO's row has no fork, so its data fork encodes as zero. The resource
 * fork is always empty.
 */
static void
file_encode(const hfsplus_row_t *row, uint32_t node_id, unsigned char *raw)
{
	fsimg_store_be16(raw, HFSPLUS_RECORD_FILE);
	record_common_encode(raw, WRITE_FLAG_THREAD, node_id, row);
	if (row->kind == FSIMG_ENTRY_SYMLINK) {
		fsimg_store_be32(raw + RECORD_FINDER, WRITE_LINK_TYPE);
		fsimg_store_be32(raw + RECORD_FINDER + 4, WRITE_LINK_CREATOR);
	}
	fork_encode(raw + FILE_DATA_FORK, row->length, 0, row->first_block,
	    row->blocks);
}

static void
thread_encode(const hfsplus_write_record_t *record, const hfsplus_row_t *row,
    unsigned char *raw)
{
	fsimg_store_be16(raw, record->type);
	fsimg_store_be32(raw + THREAD_PARENT, row->parent);
	fsimg_store_be16(raw + THREAD_UNITS, record->units);
	bcopy(record->name, raw + THREAD_NAME, 2u * (size_t)record->units);
}

/* The packer's leaf callback: raw is zeroed and exactly the record's length. */
static fsimg_status_t
leaf_data(void *argument, const hfsplus_write_record_t *record,
    unsigned char *raw, fsimg_error_t *error)
{
	hfsplus_tree_writer_t *tree = argument;
	hfsplus_row_t row;
	fsimg_status_t status;

	status = hfsplus_row_get(tree->plan, record->id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	switch (record->type) {
	case HFSPLUS_RECORD_FOLDER:
		folder_encode(tree->plan, &row, hfsplus_node_id(record->id), raw);
		break;
	case HFSPLUS_RECORD_FILE:
		file_encode(&row, hfsplus_node_id(record->id), raw);
		break;
	default:
		thread_encode(record, &row, raw);
		break;
	}
	return (FSIMG_OK);
}

/* A node's place in the volume follows from the tree's one extent. */
static fsimg_status_t
node_write(void *argument, uint32_t number, const unsigned char *node,
    fsimg_error_t *error)
{
	hfsplus_tree_writer_t *tree = argument;
	const hfsplus_tree_shape_t *shape = tree->shape;

	return (fsimg_emit_write(tree->emit,
	    (uint64_t)shape->first_block * tree->plan->block_bytes +
	    (uint64_t)number * shape->node_bytes, node, shape->node_bytes,
	    error));
}

/* Step 1: every file's and link's content in its one extent. An empty file
 * has no region, and is still opened, so a changed source is detected.
 */
static fsimg_status_t
emit_payloads(hfsplus_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	hfsplus_row_t row;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= writer->count; id++) {
		fsimg_emit_region_t region;
		fsimg_emit_payload_t payload;

		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = hfsplus_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_FILE && row.kind != FSIMG_ENTRY_SYMLINK)
			continue;
		bzero(&payload, sizeof(payload));
		payload.length = row.length;
		payload.initialized = row.length;
		if (row.blocks != 0) {
			region.image_offset = (uint64_t)row.first_block *
			    plan->block_bytes;
			region.capacity = (uint64_t)row.blocks * plan->block_bytes;
			payload.regions = &region;
			payload.region_count = 1;
		}
		status = fsimg_emit_copy(emit, id, &payload, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* One tree through the packer, encoding in the transfer buffer, which holds
 * any node: nothing else touches it until the tree is written.
 */
static fsimg_status_t
emit_tree(hfsplus_plan_t *plan, fsimg_emit_t *emit,
    const hfsplus_tree_shape_t *shape, fsimg_stream_t *records,
    fsimg_stream_t *const *levels, fsimg_error_t *error)
{
	hfsplus_tree_writer_t tree = {
		.plan = plan, .emit = emit, .shape = shape
	};
	hfsplus_tree_emit_t request = {
		.session = &plan->common.writer->session,
		.shape = shape,
		.records = records,
		.levels = levels,
		.node = fsimg_emit_buffer(emit),
		.leaf_data = leaf_data,
		.write = node_write,
		.argument = &tree
	};

	return (hfsplus_tree_emit(&request, error));
}

_Static_assert(HFSPLUS_WRITE_NODE_BYTES <= FSIMG_EMIT_BUFFER_BYTES &&
    HFSPLUS_WRITE_ATTR_NODE_BYTES <= FSIMG_EMIT_BUFFER_BYTES,
    "the transfer buffer holds a node of every writer tree");

/* One byte of the bitmap, most significant bit first: ones for [0, A) and the
 * tail, zeros elsewhere and in the padding past N.
 */
static unsigned char
bitmap_byte(const hfsplus_plan_t *plan, uint64_t index)
{
	uint64_t tail = (uint64_t)plan->total_blocks - plan->tail_blocks;
	unsigned char value = 0;

	for (unsigned bit = 0; bit < 8; bit++) {
		uint64_t block = index * 8 + bit;

		if (block < plan->allocated ||
		    (block >= tail && block < plan->total_blocks))
			value |= (unsigned char)(0x80u >> bit);
	}
	return (value);
}

/* Step 4: the whole bitmap, in transfer-buffer batches. */
static fsimg_status_t
emit_bitmap(hfsplus_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	uint64_t base, offset, total;
	size_t batch;
	fsimg_status_t status;

	base = (uint64_t)plan->head_blocks * plan->block_bytes;
	total = (uint64_t)plan->bitmap_blocks * plan->block_bytes;
	for (offset = 0; offset < total; offset += batch) {
		batch = total - offset > FSIMG_EMIT_BUFFER_BYTES ?
		    FSIMG_EMIT_BUFFER_BYTES : (size_t)(total - offset);
		for (size_t i = 0; i < batch; i++)
			buffer[i] = bitmap_byte(plan, offset + i);
		status = fsimg_emit_write(emit, base + offset, buffer, batch,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Step 5: the primary header, then the alternate at V - 1024. The startup
 * file's fork stays zero, as does backupDate and writeCount.
 */
static fsimg_status_t
emit_headers(hfsplus_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	unsigned char raw[HFSPLUS_HEADER_BYTES];
	uint32_t block = plan->block_bytes;
	uint32_t data_clump, bitmap_bytes;
	bool x = plan->variant == HFSPLUS_VARIANT_X;
	fsimg_status_t status;

	bzero(raw, sizeof(raw));
	fsimg_store_be16(raw + HEADER_SIGNATURE,
	    x ? HFSPLUS_SIGNATURE_X : HFSPLUS_SIGNATURE_PLUS);
	fsimg_store_be16(raw + HEADER_VERSION,
	    x ? HFSPLUS_VERSION_X : HFSPLUS_VERSION_PLUS);
	fsimg_store_be32(raw + HEADER_ATTRIBUTES, WRITE_ATTRIBUTES);
	fsimg_store_be32(raw + HEADER_LAST_MOUNTED, WRITE_LAST_MOUNTED);
	fsimg_store_be32(raw + HEADER_CREATE_DATE, plan->format_time);
	fsimg_store_be32(raw + HEADER_MODIFY_DATE, plan->format_time);
	fsimg_store_be32(raw + HEADER_CHECKED_DATE, plan->format_time);
	fsimg_store_be32(raw + HEADER_FILE_COUNT, plan->file_count);
	fsimg_store_be32(raw + HEADER_FOLDER_COUNT, plan->folder_count);
	fsimg_store_be32(raw + HEADER_BLOCK_SIZE, block);
	fsimg_store_be32(raw + HEADER_TOTAL_BLOCKS, plan->total_blocks);
	fsimg_store_be32(raw + HEADER_FREE_BLOCKS,
	    plan->total_blocks - plan->allocated - plan->tail_blocks);
	fsimg_store_be32(raw + HEADER_NEXT_ALLOCATION, plan->allocated);
	data_clump = block < WRITE_DATA_CLUMP_MAX / 16 ? 16 * block :
	    WRITE_DATA_CLUMP_MAX;
	fsimg_store_be32(raw + HEADER_RSRC_CLUMP, data_clump);
	fsimg_store_be32(raw + HEADER_DATA_CLUMP, data_clump);
	fsimg_store_be32(raw + HEADER_NEXT_CATALOG_ID, plan->next_catalog_id);
	fsimg_store_be64(raw + HEADER_ENCODINGS_BITMAP, WRITE_ENCODINGS_BITMAP);
	bcopy(options->volume_uuid, raw + HEADER_FINDER_IDENTIFIER,
	    VOLUME_IDENTIFIER_BYTES);
	bitmap_bytes = plan->bitmap_blocks * block;
	fork_encode(raw + HEADER_ALLOCATION_FILE, bitmap_bytes, bitmap_bytes,
	    plan->head_blocks, plan->bitmap_blocks);
	tree_fork_encode(raw + HEADER_EXTENTS_FILE, &plan->extents);
	tree_fork_encode(raw + HEADER_CATALOG_FILE, &plan->catalog);
	tree_fork_encode(raw + HEADER_ATTRIBUTES_FILE, &plan->attributes);

	status = fsimg_emit_write(emit, HFSPLUS_BOOT_BYTES, raw, sizeof(raw),
	    error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_emit_write(emit, plan->volume_bytes - HFSPLUS_BOOT_BYTES,
	    raw, sizeof(raw), error));
}

static fsimg_status_t
hfsplus_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	hfsplus_plan_t *plan = (hfsplus_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_payloads(plan, emit, error);
	if (status == FSIMG_OK)
		status = emit_tree(plan, emit, &plan->catalog, plan->records,
		    plan->levels, error);
	if (status == FSIMG_OK)
		status = emit_tree(plan, emit, &plan->extents, NULL, NULL, error);
	if (status == FSIMG_OK)
		status = emit_tree(plan, emit, &plan->attributes, NULL, NULL,
		    error);
	if (status == FSIMG_OK)
		status = emit_bitmap(plan, emit, error);
	if (status == FSIMG_OK)
		status = emit_headers(plan, emit, error);
	return (status);
}
