/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "write-qnx6.h"

/* Emission (specs/design-qnx6-driver.md, Emission). The common emission
 * object has already zeroed the sink, so every unused slot, inode record
 * and free block, the twin region, the boot area, the rest of both header
 * areas and the tail past E are zero already. The driver writes, in order:
 * each entry's payload, slots and long name; every object's indirect
 * blocks; the inode table; the bitmap and the system and long-name trees'
 * indirect blocks; and both headers. Every byte comes from a row, the
 * geometry or a profile constant.
 */

/* A post-order walk over one tree's indirect blocks, which produces them in
 * position order, so consecutive blocks batch in the emission buffer.
 */
typedef struct qnx6_indirect_walk {
	qnx6_plan_t	*plan;
	fsimg_emit_t	*emit;
	const qnx6_shape_t *shape;
	unsigned char	*buffer;
	uint32_t	 first;
	uint64_t	 position;
	uint64_t	 batch_start;
	size_t		 batched;
	uint32_t	 refs[QNX6_WINDOW_BYTES / 4];
} qnx6_indirect_walk_t;

static uint64_t block_offset(const qnx6_plan_t *, uint64_t);
static fsimg_status_t emit_slot(qnx6_plan_t *, fsimg_emit_t *,
    const qnx6_row_t *, const qnx6_row_t *, const char *, size_t,
    fsimg_error_t *);
static fsimg_status_t emit_dots(qnx6_plan_t *, fsimg_emit_t *,
    const qnx6_row_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t emit_long_name(qnx6_plan_t *, fsimg_emit_t *, uint32_t,
    const char *, size_t, fsimg_error_t *);
static fsimg_status_t emit_payload(qnx6_plan_t *, fsimg_emit_t *, uint64_t,
    const qnx6_row_t *, fsimg_error_t *);
static fsimg_status_t emit_entries(qnx6_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t flush_indirect(qnx6_indirect_walk_t *, fsimg_error_t *);
static fsimg_status_t visit_indirect(qnx6_indirect_walk_t *, unsigned,
    uint64_t, fsimg_error_t *);
static fsimg_status_t emit_indirect(qnx6_plan_t *, fsimg_emit_t *,
    const qnx6_shape_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t emit_object_indirect(qnx6_plan_t *, fsimg_emit_t *,
    uint64_t, fsimg_error_t *);
static void tree_of(const qnx6_shape_t *, uint32_t, uint64_t, qnx6_tree_t *);
static fsimg_status_t emit_inodes(qnx6_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static unsigned char bitmap_byte(const qnx6_plan_t *, uint64_t);
static fsimg_status_t emit_bitmap(qnx6_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_headers(qnx6_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);

/* The volume offset of block p. */
static uint64_t
block_offset(const qnx6_plan_t *plan, uint64_t block)
{
	return (QNX6_DATA_ORIGIN + block * plan->geometry.block_size);
}

/* Write an entry's slot in its directory: an inline name, or the long-name
 * marker with the record index and checksum, which the flags word 0x100
 * makes mandatory.
 */
static fsimg_status_t
emit_slot(qnx6_plan_t *plan, fsimg_emit_t *emit, const qnx6_row_t *directory,
    const qnx6_row_t *row, const char *name, size_t length,
    fsimg_error_t *error)
{
	unsigned char raw[QNX6_SLOT_BYTES];
	qnx6_shape_t shape;
	qnx6_slot_t slot;

	if (!qnx6_object_shape(plan, directory->length, &shape) ||
	    ((uint64_t)row->slot + 1) * QNX6_SLOT_BYTES > directory->length ||
	    (length > QNX6_INLINE_NAME_MAX) != (row->long_name != QNX6_SENTINEL))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 slot disagrees with its planned directory"));
	bzero(&slot, sizeof(slot));
	slot.inode = row->inode;
	if (row->long_name != QNX6_SENTINEL) {
		slot.long_name = true;
		slot.long_index = row->long_name;
		slot.checksum = qnx6_name_checksum((const unsigned char *)name,
		    length);
	} else {
		slot.length = (uint8_t)length;
		bcopy(name, slot.name, length);
	}
	qnx6_slot_encode(&slot, plan->geometry.big_endian, raw);
	return (fsimg_emit_write(emit, block_offset(plan, (uint64_t)directory->first +
	    shape.indirect) + (uint64_t)row->slot * QNX6_SLOT_BYTES, raw,
	    sizeof(raw), error));
}

/* Write a directory's first two slots: "." naming itself and ".." naming
 * its parent; the root is its own parent.
 */
static fsimg_status_t
emit_dots(qnx6_plan_t *plan, fsimg_emit_t *emit, const qnx6_row_t *directory,
    uint32_t parent, fsimg_error_t *error)
{
	unsigned char raw[2 * QNX6_SLOT_BYTES];
	qnx6_shape_t shape;
	qnx6_slot_t slot;

	if (!qnx6_object_shape(plan, directory->length, &shape) ||
	    directory->length < sizeof(raw))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 directory has no room for its dot entries"));
	slot = (qnx6_slot_t){ .inode = directory->inode, .length = 1,
	    .name = "." };
	qnx6_slot_encode(&slot, plan->geometry.big_endian, raw);
	slot = (qnx6_slot_t){ .inode = parent, .length = 2, .name = ".." };
	qnx6_slot_encode(&slot, plan->geometry.big_endian, raw + QNX6_SLOT_BYTES);
	return (fsimg_emit_write(emit, block_offset(plan, (uint64_t)directory->first +
	    shape.indirect), raw, sizeof(raw), error));
}

/* Write long-name record index: the two-byte length and the name at the
 * start of its own block of the long-name stream; the rest stays zero.
 */
static fsimg_status_t
emit_long_name(qnx6_plan_t *plan, fsimg_emit_t *emit, uint32_t index,
    const char *name, size_t length, fsimg_error_t *error)
{
	unsigned char raw[2 + QNX6_NAME_MAX];

	if (index >= plan->names.leaves || length > QNX6_NAME_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 long name is outside its planned stream"));
	qnx6_put16(raw, (uint16_t)length, plan->geometry.big_endian);
	bcopy(name, raw + 2, length);
	return (fsimg_emit_write(emit, block_offset(plan, (uint64_t)plan->names_first +
	    plan->names.indirect + index), raw, 2 + length, error));
}

/* Copy a file's or link's content into its leaves, one contiguous region
 * after its indirect blocks. A partial last block stays zero.
 */
static fsimg_status_t
emit_payload(qnx6_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    const qnx6_row_t *row, fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	qnx6_shape_t shape;
	fsimg_emit_payload_t payload = {
		.length = row->length,
		.initialized = row->length
	};

	if (!qnx6_object_shape(plan, row->length, &shape))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 payload lost its shape"));
	if (shape.leaves != 0) {
		region.image_offset = block_offset(plan, (uint64_t)row->first +
		    shape.indirect);
		region.capacity = shape.leaves * plan->geometry.block_size;
		payload.regions = &region;
		payload.region_count = 1;
	}
	return (fsimg_emit_copy(emit, id, &payload, error));
}

/* Step 1: the root's and a synthesized .boot's slots, then for every entry
 * in ID order its slot and long name, its own dot entries when it is a
 * directory, and its content when it is a file or symbolic link.
 */
static fsimg_status_t
emit_entries(qnx6_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	qnx6_row_t root, row, parent;
	const char *name;
	size_t length;
	fsimg_status_t status;

	status = qnx6_row_get(plan, 0, &root, error);
	if (status == FSIMG_OK)
		status = emit_dots(plan, emit, &root, QNX6_ROOT_INODE, error);
	if (status == FSIMG_OK && qnx6_boot_synthesized(plan)) {
		status = qnx6_row_get(plan, plan->boot_row, &row, error);
		if (status == FSIMG_OK)
			status = emit_dots(plan, emit, &row, QNX6_ROOT_INODE, error);
		if (status == FSIMG_OK)
			status = emit_slot(plan, emit, &root, &row, ".boot", 5,
			    error);
	}
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = qnx6_row_get(plan, id, &row, error);
		if (status == FSIMG_OK)
			status = qnx6_row_get(plan, catalog->parent_id, &parent,
			    error);
		if (status != FSIMG_OK)
			break;
		name = strrchr(catalog->entry.path, '/') + 1;
		length = strlen(name);
		status = emit_slot(plan, emit, &parent, &row, name, length, error);
		if (status == FSIMG_OK && row.long_name != QNX6_SENTINEL)
			status = emit_long_name(plan, emit, row.long_name, name,
			    length, error);
		if (status != FSIMG_OK)
			break;
		switch (catalog->entry.kind) {
		case FSIMG_ENTRY_DIRECTORY:
			status = emit_dots(plan, emit, &row, parent.inode, error);
			break;
		case FSIMG_ENTRY_FILE:
		case FSIMG_ENTRY_SYMLINK:
			status = emit_payload(plan, emit, id, &row, error);
			break;
		default:
			break;
		}
	}
	return (status);
}

static fsimg_status_t
flush_indirect(qnx6_indirect_walk_t *walk, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (walk->batched == 0)
		return (FSIMG_OK);
	status = fsimg_emit_write(walk->emit, block_offset(walk->plan,
	    (uint64_t)walk->first + walk->batch_start), walk->buffer,
	    walk->batched * walk->plan->geometry.block_size, error);
	walk->batch_start = walk->position;
	walk->batched = 0;
	return (status);
}

/* Visit indirect node (level, index): its child indirect blocks first, then
 * the node itself, whose post-order position must be the next one.
 */
static fsimg_status_t
visit_indirect(qnx6_indirect_walk_t *walk, unsigned level, uint64_t index,
    fsimg_error_t *error)
{
	const qnx6_shape_t *shape = walk->shape;
	uint32_t block = walk->plan->geometry.block_size;
	unsigned char *raw;
	uint64_t child;
	fsimg_status_t status;

	if (level < shape->depth) {
		for (uint64_t c = 0; c < shape->pointers; c++) {
			child = index * shape->pointers + c;
			if (child >= shape->count[level + 1])
				break;
			status = visit_indirect(walk, level + 1, child, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	if (qnx6_shape_position(shape, level, index) != walk->position)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 indirect block is out of post-order"));
	qnx6_shape_refs(shape, walk->first, level, index, walk->refs);
	raw = walk->buffer + walk->batched * block;
	for (uint32_t k = 0; k < shape->pointers; k++)
		qnx6_put32(raw + (size_t)k * 4, walk->refs[k],
		    walk->plan->geometry.big_endian);
	walk->batched++;
	walk->position++;
	if (walk->batched * block == FSIMG_EMIT_BUFFER_BYTES)
		return (flush_indirect(walk, error));
	return (FSIMG_OK);
}

/* Emit the indirect blocks of a tree laid out from block first. Each is
 * generated from the shape alone; no pointer vector is stored.
 */
static fsimg_status_t
emit_indirect(qnx6_plan_t *plan, fsimg_emit_t *emit, const qnx6_shape_t *shape,
    uint32_t first, fsimg_error_t *error)
{
	qnx6_indirect_walk_t *walk;
	fsimg_error_t detail;
	fsimg_status_t status;

	if (shape->depth == 0)
		return (FSIMG_OK);
	walk = fsimg_alloc(plan->common.writer->ctx, sizeof(*walk), &detail);
	if (walk == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	*walk = (qnx6_indirect_walk_t){ .plan = plan, .emit = emit,
	    .shape = shape, .buffer = fsimg_emit_buffer(emit), .first = first };
	status = FSIMG_OK;
	for (uint64_t i = 0; status == FSIMG_OK && i < shape->count[1]; i++)
		status = visit_indirect(walk, 1, i, error);
	if (status == FSIMG_OK)
		status = flush_indirect(walk, error);
	if (status == FSIMG_OK && walk->position != shape->indirect)
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 indirect walk missed blocks");
	fsimg_dealloc(walk);
	return (status);
}

static fsimg_status_t
emit_object_indirect(qnx6_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    fsimg_error_t *error)
{
	qnx6_shape_t shape;
	qnx6_row_t row;
	fsimg_status_t status;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = qnx6_row_get(plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (!qnx6_object_shape(plan, row.length, &shape))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 object lost its shape"));
	return (emit_indirect(plan, emit, &shape, row.first, error));
}

/* The descriptor of a tree laid out from block first. */
static void
tree_of(const qnx6_shape_t *shape, uint32_t first, uint64_t length,
    qnx6_tree_t *tree)
{
	tree->length = length;
	tree->depth = (uint8_t)shape->depth;
	qnx6_shape_refs(shape, first, 0, 0, tree->ref);
}

/* Step 3: the used inode records in inode order, which is the root, .boot
 * and then the catalog without /.boot. Records batch in the emission
 * buffer; unused records stay zero. Catalog IDs below boot_row take inodes
 * 3, 4, ... in order, so inode n is row n - 2 until the IDs reach boot_row;
 * from there one ID (boot_row itself) has been skipped, so it is row n - 1.
 * A synthesized .boot's row, count + 1, is never reached that way.
 */
static fsimg_status_t
emit_inodes(qnx6_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	const size_t per_batch = FSIMG_EMIT_BUFFER_BYTES / QNX6_INODE_BYTES;
	qnx6_shape_t shape;
	qnx6_inode_t inode;
	qnx6_row_t row;
	uint64_t table, id, batch_start;
	uint32_t number;
	size_t batched;
	fsimg_status_t status;

	table = block_offset(plan, plan->bitmap.indirect + plan->bitmap.leaves +
	    plan->inodes.indirect);
	batch_start = 0;
	batched = 0;
	for (number = 1; number <= plan->used_inodes; number++) {
		if (number == QNX6_ROOT_INODE)
			id = 0;
		else if (number == QNX6_BOOT_INODE)
			id = plan->boot_row;
		else
			id = number - 2 + (number - 2 >= plan->boot_row ? 1 : 0);
		status = fsimg_session_charge(&plan->common.writer->session, 0,
		    0, 1, error);
		if (status == FSIMG_OK)
			status = qnx6_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.inode != number || !qnx6_object_shape(plan, row.length, &shape))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "QNX6 inode order disagrees with its rows"));
		inode = (qnx6_inode_t){
			.size = row.length, .uid = row.uid, .gid = row.gid,
			.created = row.created, .modified = row.modified,
			.accessed = row.accessed, .changed = row.changed,
			.mode = row.mode, .links = row.links, .status = row.status
		};
		tree_of(&shape, row.first, row.length, &inode.tree);
		qnx6_inode_encode(&inode, plan->geometry.big_endian,
		    buffer + batched * QNX6_INODE_BYTES);
		if (++batched == per_batch || number == plan->used_inodes) {
			status = fsimg_emit_write(emit, table + batch_start *
			    QNX6_INODE_BYTES, buffer, batched * QNX6_INODE_BYTES,
			    error);
			if (status != FSIMG_OK)
				return (status);
			batch_start += batched;
			batched = 0;
		}
	}
	return (FSIMG_OK);
}

/* Byte k of the bitmap: block p is allocated exactly when p < A, and the
 * bits for p >= N in the last bitmap block are set.
 */
static unsigned char
bitmap_byte(const qnx6_plan_t *plan, uint64_t k)
{
	uint64_t first = k * 8;
	unsigned char value;

	if (first + 8 <= plan->allocated || first >= plan->geometry.blocks)
		return (0xff);
	if (first >= plan->allocated && first + 8 <= plan->geometry.blocks)
		return (0);
	value = 0;
	for (unsigned bit = 0; bit < 8; bit++) {
		if (first + bit < plan->allocated ||
		    first + bit >= plan->geometry.blocks)
			value |= (unsigned char)(1u << bit);
	}
	return (value);
}

/* Step 4: the bitmap leaves, which start after the bitmap tree's indirect
 * blocks at block 0, written in buffer-sized batches.
 */
static fsimg_status_t
emit_bitmap(qnx6_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	uint64_t total, offset, base;
	size_t batch;
	fsimg_status_t status;

	total = plan->bitmap.leaves * plan->geometry.block_size;
	base = block_offset(plan, plan->bitmap.indirect);
	for (offset = 0; offset < total; offset += batch) {
		batch = total - offset > FSIMG_EMIT_BUFFER_BYTES ?
		    FSIMG_EMIT_BUFFER_BYTES : (size_t)(total - offset);
		for (size_t i = 0; i < batch; i++)
			buffer[i] = bitmap_byte(plan, offset + i);
		status = fsimg_emit_write(emit, base + offset, buffer, batch, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Step 5: two byte-identical serial-1 headers at 8192 and S2. */
static fsimg_status_t
emit_headers(qnx6_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	const qnx6_geometry_t *g = &plan->geometry;
	unsigned char raw[QNX6_HEADER_BYTES];
	qnx6_header_t header;
	fsimg_status_t status;

	bzero(&header, sizeof(header));
	header.serial = 1;
	header.format_time = plan->format_time;
	header.sync_time = plan->format_time;
	header.flags = QNX6_FLAG_CHECKSUM;
	header.version = QNX6_VERSION;
	header.reserve = (uint16_t)options->reserve_percent;
	bcopy(options->volume_uuid, header.uuid, sizeof(header.uuid));
	header.block_size = g->block_size;
	header.inode_count = g->inode_count;
	header.free_inodes = g->inode_count - plan->used_inodes;
	header.block_count = g->blocks;
	header.free_blocks = g->blocks - plan->allocated;
	header.groups = g->groups;
	tree_of(&plan->bitmap, 0, (uint64_t)g->blocks / 8 + (g->blocks % 8 != 0),
	    &header.bitmap);
	tree_of(&plan->inodes, (uint32_t)(plan->bitmap.indirect +
	    plan->bitmap.leaves), (uint64_t)g->inode_count * QNX6_INODE_BYTES,
	    &header.inodes);
	tree_of(&plan->names, plan->names_first,
	    plan->names.leaves * g->block_size, &header.names);
	qnx6_header_encode(&header, g->big_endian, raw);
	status = fsimg_emit_write(emit, QNX6_HEADER_OFFSET, raw, sizeof(raw),
	    error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_emit_write(emit, g->second_header, raw, sizeof(raw), error));
}

fsimg_status_t
qnx6_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit, fsimg_error_t *error)
{
	qnx6_plan_t *plan = (qnx6_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_entries(plan, emit, error);
	for (uint64_t id = 0; status == FSIMG_OK && id <= writer->count; id++)
		status = emit_object_indirect(plan, emit, id, error);
	if (status == FSIMG_OK && qnx6_boot_synthesized(plan))
		status = emit_object_indirect(plan, emit, plan->boot_row, error);
	if (status == FSIMG_OK)
		status = emit_inodes(plan, emit, error);
	if (status == FSIMG_OK)
		status = emit_bitmap(plan, emit, error);
	if (status == FSIMG_OK)
		status = emit_indirect(plan, emit, &plan->bitmap, 0, error);
	if (status == FSIMG_OK)
		status = emit_indirect(plan, emit, &plan->inodes,
		    (uint32_t)(plan->bitmap.indirect + plan->bitmap.leaves), error);
	if (status == FSIMG_OK)
		status = emit_indirect(plan, emit, &plan->names, plan->names_first,
		    error);
	if (status == FSIMG_OK)
		status = emit_headers(plan, emit, error);
	return (status);
}
