/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "write-qnx4.h"

/*
 * Emission. The common emission object has already zeroed the sink, so
 * the loader block, every free block, free slot and free .inodes slot,
 * every unused long-name record and every byte after a file's content in
 * its last block are zero already. The driver writes, in order:
 *   1. for each entry in ID order, its content, its dot entries when it is
 *	a directory, and its records: a direct record in its parent's slot,
 *	or a link record there and its inode record in .inodes, with its
 *	long-name record when its name exceeds 48 bytes;
 *   2. the root block: the five records and the label;
 *   3. the root's fixed slots, then .inodes slot 0 and lost+found's dot
 *	entries;
 *   4. the bitmap.
 * Every byte comes from a row, the geometry or a profile constant. Record
 * positions are packed with QNX4_POSITION().
 */

#define QNX4_ROOT_POSITION QNX4_POSITION(QNX4_ROOT_BLOCK, QNX4_RECORD_ROOT)

/* .inodes slot 0: the table's signature name and the value at offset 16. */
static const unsigned char inodes_signature[QNX4_DIRECT_NAME_MAX] = {
	'I', 'a', 'm', 'T', 'H', 'E', '.', 'i', 'n', 'o', 'd', 'e', 'F', 'I',
	'L', 'E'
};
#define QNX4_INODES_SIGNATURE_VALUE UINT32_C(0x00100000)

/* The root's fixed slots after "." and "..": .bitmap's direct record, then
 * links to three root-block records.
 */
#define QNX4_BITMAP_SLOT	2u
static const struct {
	const char	*name;
	unsigned	 slot;
	unsigned	 record;
} table_links[] = {
	{ ".inodes", 3, QNX4_RECORD_INODES },
	{ ".boot", 4, QNX4_RECORD_BOOT },
	{ ".altboot", 5, QNX4_RECORD_ALTBOOT }
};

/* The formatter's root slot 6 names lost+found as a link record to
 * (2, 4) whose status 0x04 hides it from lookup, and carries 0x08 at byte
 * 0x36, inside the long-name pointer. Readers treat the slot as free; the
 * byte is reproduced so the slot is the formatter's byte for byte.
 */
#define QNX4_LOST_FOUND_SLOT	6u
#define QNX4_LOST_FOUND_MARK_OFFSET 0x36u
#define QNX4_LOST_FOUND_MARK	0x08u

/* With no catalog root entry, slot 7 is free with the status 0x02 the
 * formatter leaves there.
 */
#define QNX4_EMPTY_SLOT_STATUS	QNX4_STATUS_MODIFIED

static uint64_t block_offset(uint32_t);
static uint64_t slot_offset(uint32_t, uint32_t);
static uint64_t row_position(const qnx4_row_t *);
static void row_record(const qnx4_row_t *, qnx4_inode_t *);
static void stamp_format_time(qnx4_inode_t *, uint32_t);
static void named_record(const char *, qnx4_inode_t *);
static fsimg_status_t emit_record(fsimg_emit_t *, uint64_t,
    const qnx4_inode_t *, fsimg_error_t *);
static fsimg_status_t emit_link(fsimg_emit_t *, uint64_t, const char *,
    size_t, uint64_t, uint32_t, fsimg_error_t *);
static fsimg_status_t emit_dots(fsimg_emit_t *, qnx4_extent_t, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t emit_payload(fsimg_emit_t *, uint64_t,
    const qnx4_row_t *, fsimg_error_t *);
static fsimg_status_t emit_long_name(qnx4_plan_t *, fsimg_emit_t *,
    const qnx4_row_t *, uint64_t, const char *, size_t, fsimg_error_t *);
static fsimg_status_t emit_names(qnx4_plan_t *, fsimg_emit_t *,
    const qnx4_row_t *, const qnx4_row_t *, const char *, size_t,
    fsimg_error_t *);
static fsimg_status_t emit_entry(qnx4_plan_t *, fsimg_emit_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t emit_entries(qnx4_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t boot_record(qnx4_plan_t *, uint64_t, qnx4_inode_t *,
    fsimg_error_t *);
static fsimg_status_t emit_root_block(qnx4_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_fixed_slots(qnx4_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_table_headers(qnx4_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static unsigned char bitmap_byte(const qnx4_plan_t *, uint64_t);
static fsimg_status_t emit_bitmap(qnx4_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);

/* The volume offset of 1-based block b. */
static uint64_t
block_offset(uint32_t block)
{
	return ((uint64_t)(block - 1) * QNX4_BLOCK_BYTES);
}

/* The volume offset of 64-byte slot `slot` of a run starting at block. */
static uint64_t
slot_offset(uint32_t block, uint32_t slot)
{
	return (block_offset(block) + (uint64_t)slot * QNX4_RECORD_BYTES);
}

/* An entry's identity: the position of its inode record. */
static uint64_t
row_position(const qnx4_row_t *row)
{
	return (QNX4_POSITION(row->record_block, row->record_slot));
}

/* An entry's inode record from its row, without a name: the name field of
 * an .inodes or root-block record is 16 NUL bytes, and a direct record's
 * name is added by the caller.
 */
static void
row_record(const qnx4_row_t *row, qnx4_inode_t *record)
{
	bzero(record, sizeof(*record));
	record->size = row->size;
	record->first_extent.first = row->first;
	record->first_extent.blocks = row->blocks;
	record->extents = row->blocks == 0 ? 0 : 1;
	record->created = row->created;
	record->modified = row->modified;
	record->accessed = row->accessed;
	record->changed = row->changed;
	record->mode = row->mode;
	record->uid = row->uid;
	record->gid = row->gid;
	record->links = row->links;
	record->status = row->status;
}

/* Set a format-owned object's four times to the format time. The caller
 * builds the record from a zeroed literal, so its owner is 0.
 */
static void
stamp_format_time(qnx4_inode_t *record, uint32_t format_time)
{
	record->created = format_time;
	record->modified = format_time;
	record->accessed = format_time;
	record->changed = format_time;
}

/* Give a record a direct name of at most 16 bytes. */
static void
named_record(const char *name, qnx4_inode_t *record)
{
	record->name_length = (uint8_t)strlen(name);
	bcopy(name, record->name, record->name_length);
}

static fsimg_status_t
emit_record(fsimg_emit_t *emit, uint64_t offset, const qnx4_inode_t *record,
    fsimg_error_t *error)
{
	unsigned char raw[QNX4_RECORD_BYTES];

	qnx4_inode_encode(record, raw);
	return (fsimg_emit_write(emit, offset, raw, sizeof(raw), error));
}

/* Write a link record named by the first `length` bytes of name, at most
 * 48, that names the record at `target` and, for a long name, its
 * .longfilenames block (0 otherwise).
 */
static fsimg_status_t
emit_link(fsimg_emit_t *emit, uint64_t offset, const char *name,
    size_t length, uint64_t target, uint32_t long_block, fsimg_error_t *error)
{
	unsigned char raw[QNX4_RECORD_BYTES];
	qnx4_link_t link;

	if (length > QNX4_LINK_NAME_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 link name exceeds its field"));
	bzero(&link, sizeof(link));
	link.name_length = (uint8_t)length;
	bcopy(name, link.name, length);
	link.target_block = QNX4_POSITION_BLOCK(target);
	link.target_slot = (uint8_t)QNX4_POSITION_SLOT(target);
	link.long_block = long_block;
	link.status = QNX4_STATUS_LINK;
	qnx4_link_encode(&link, raw);
	return (fsimg_emit_write(emit, offset, raw, sizeof(raw), error));
}

/* Write a directory's "." and "..", the first two slots of its extent:
 * link records naming the directory's own record and its parent's. "."
 * carries "I", 0x03, "QNX" after its NUL and, at byte 8, the directory's
 * first-extent length in blocks.
 */
static fsimg_status_t
emit_dots(fsimg_emit_t *emit, qnx4_extent_t directory, uint64_t self,
    uint64_t parent, fsimg_error_t *error)
{
	static const unsigned char signature[5] = { 'I', 0x03, 'Q', 'N', 'X' };
	unsigned char raw[2 * QNX4_RECORD_BYTES];
	qnx4_link_t link;

	bzero(&link, sizeof(link));
	link.name_length = 1;
	link.name[0] = '.';
	link.target_block = QNX4_POSITION_BLOCK(self);
	link.target_slot = (uint8_t)QNX4_POSITION_SLOT(self);
	link.status = QNX4_STATUS_LINK;
	qnx4_link_encode(&link, raw);
	bcopy(signature, raw + 2, sizeof(signature));
	qnx4_put32(raw + 8, directory.blocks);

	link.name_length = 2;
	link.name[1] = '.';
	link.target_block = QNX4_POSITION_BLOCK(parent);
	link.target_slot = (uint8_t)QNX4_POSITION_SLOT(parent);
	qnx4_link_encode(&link, raw + QNX4_RECORD_BYTES);
	return (fsimg_emit_write(emit, block_offset(directory.first), raw,
	    sizeof(raw), error));
}

/* Copy a file's or symbolic link's content into its extent. The rest of
 * the last block stays zero.
 */
static fsimg_status_t
emit_payload(fsimg_emit_t *emit, uint64_t id, const qnx4_row_t *row,
    fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	fsimg_emit_payload_t payload = {
		.length = row->size,
		.initialized = row->size
	};

	if (row->blocks != 0) {
		region.image_offset = block_offset(row->first);
		region.capacity = (uint64_t)row->blocks * QNX4_BLOCK_BYTES;
		payload.regions = &region;
		payload.region_count = 1;
	}
	return (fsimg_emit_copy(emit, id, &payload, error));
}

/* Write a long name's .longfilenames record: the position of the link that
 * names it, state used and the full name.
 */
static fsimg_status_t
emit_long_name(qnx4_plan_t *plan, fsimg_emit_t *emit, const qnx4_row_t *row,
    uint64_t link, const char *name, size_t length, fsimg_error_t *error)
{
	unsigned char *raw = fsimg_emit_buffer(emit);
	qnx4_longname_t record;

	if (row->long_name >= plan->long_names || length < QNX4_LONG_NAME_MIN ||
	    length > QNX4_NAME_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 long name is outside its planned records"));
	bzero(&record, sizeof(record));
	record.block = QNX4_POSITION_BLOCK(link);
	record.slot = (uint8_t)QNX4_POSITION_SLOT(link);
	record.state = QNX4_LONG_NAME_USED;
	record.name_length = (uint16_t)length;
	bcopy(name, record.name, length);
	qnx4_longname_encode(&record, raw);
	return (fsimg_emit_write(emit, block_offset(plan->names_first +
	    row->long_name), raw, QNX4_BLOCK_BYTES, error));
}

/* Write an entry's records. A name of 1 to 16 bytes is a direct record in
 * the parent's slot. A longer one is a link record there that targets the
 * entry's inode record in .inodes; above 48 bytes the link carries the
 * mangled prefix and the long-name record's block, and that record names
 * the link back.
 */
static fsimg_status_t
emit_names(qnx4_plan_t *plan, fsimg_emit_t *emit, const qnx4_row_t *parent,
    const qnx4_row_t *row, const char *name, size_t length,
    fsimg_error_t *error)
{
	unsigned char mangled[QNX4_LINK_NAME_MAX];
	qnx4_inode_t record;
	uint64_t link_offset, link;
	uint32_t long_block;
	fsimg_status_t status;

	if ((uint64_t)row->slot >=
	    (uint64_t)parent->blocks * QNX4_SLOTS_PER_BLOCK ||
	    (length > QNX4_DIRECT_NAME_MAX) != (row->inode_slot != QNX4_NONE) ||
	    (length > QNX4_LINK_NAME_MAX) != (row->long_name != QNX4_NONE))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 slot disagrees with its planned directory"));
	link_offset = slot_offset(parent->first, row->slot);
	row_record(row, &record);
	if (row->inode_slot == QNX4_NONE) {
		record.name_length = (uint8_t)length;
		bcopy(name, record.name, length);
		return (emit_record(emit, link_offset, &record, error));
	}

	status = emit_record(emit, slot_offset(plan->inodes_first,
	    row->inode_slot), &record, error);
	if (status != FSIMG_OK)
		return (status);
	if (row->long_name == QNX4_NONE)
		return (emit_link(emit, link_offset, name, length,
		    row_position(row), 0, error));
	link = QNX4_POSITION(parent->first, row->slot);
	long_block = plan->names_first + row->long_name;
	qnx4_long_prefix((const unsigned char *)name, long_block, mangled);
	status = emit_link(emit, link_offset, (const char *)mangled,
	    QNX4_LINK_NAME_MAX - 1, row_position(row), long_block, error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_long_name(plan, emit, row, link, name, length, error));
}

/* One entry: its content or its dot entries, then its records in its
 * parent. /.boot and /.altboot have content but no slot: their records
 * are root-block records, which step 2 writes.
 */
static fsimg_status_t
emit_entry(qnx4_plan_t *plan, fsimg_emit_t *emit, uint64_t id,
    fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	qnx4_row_t row, parent;
	const char *name;
	fsimg_status_t status;

	status = fsimg_catalog_get(plan->common.writer, id, &catalog, error);
	if (status == FSIMG_OK)
		status = qnx4_row_get(plan, id, &row, error);
	if (status == FSIMG_OK)
		status = qnx4_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	switch (catalog->entry.kind) {
	case FSIMG_ENTRY_DIRECTORY:
		status = emit_dots(emit,
		    (qnx4_extent_t){ .first = row.first, .blocks = row.blocks },
		    row_position(&row), row_position(&parent), error);
		break;
	case FSIMG_ENTRY_FILE:
	case FSIMG_ENTRY_SYMLINK:
		status = emit_payload(emit, id, &row, error);
		break;
	default:
		break;
	}
	if (status != FSIMG_OK || row.record_block == QNX4_ROOT_BLOCK)
		return (status);
	name = strrchr(catalog->entry.path, '/') + 1;
	return (emit_names(plan, emit, &parent, &row, name, strlen(name),
	    error));
}

/* Step 1: the root's dot entries, then every entry in ID order. */
static fsimg_status_t
emit_entries(qnx4_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	qnx4_row_t root;
	fsimg_status_t status;

	status = qnx4_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_dots(emit,
	    (qnx4_extent_t){ .first = root.first, .blocks = root.blocks },
	    QNX4_ROOT_POSITION, QNX4_ROOT_POSITION, error);
	for (uint64_t id = 1; status == FSIMG_OK &&
	    id <= plan->common.writer->count; id++)
		status = emit_entry(plan, emit, id, error);
	return (status);
}

/* Root-block record 2 or 3 from its catalog row, or the formatter's empty
 * file without one.
 */
static fsimg_status_t
boot_record(qnx4_plan_t *plan, uint64_t id, qnx4_inode_t *record,
    fsimg_error_t *error)
{
	qnx4_row_t row;
	fsimg_status_t status;

	if (id == 0) {
		*record = (qnx4_inode_t){
			.mode = QNX4_MODE_FILE | QNX4_BOOT_PERMISSIONS,
			.links = 1,
			.status = QNX4_STATUS_TABLE | QNX4_STATUS_USED
		};
		stamp_format_time(record, plan->format_time);
		return (FSIMG_OK);
	}
	status = qnx4_row_get(plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	row_record(&row, record);
	return (FSIMG_OK);
}

/* Step 2: block 2, the root, .inodes, .boot, .altboot and lost+found
 * records, the label at 0x1c0 and zeros elsewhere.
 */
static fsimg_status_t
emit_root_block(qnx4_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	const char *label = plan->common.writer->options.label;
	unsigned char raw[QNX4_BLOCK_BYTES];
	qnx4_inode_t record;
	qnx4_row_t root;
	size_t length;
	fsimg_status_t status;

	bzero(raw, sizeof(raw));
	status = qnx4_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	row_record(&root, &record);
	named_record("/", &record);
	qnx4_inode_encode(&record, raw + QNX4_RECORD_ROOT * QNX4_RECORD_BYTES);

	record = (qnx4_inode_t){
		.size = plan->inodes_blocks * QNX4_BLOCK_BYTES,
		.first_extent = {
			.first = plan->inodes_first,
			.blocks = plan->inodes_blocks
		},
		.extents = 1,
		.mode = QNX4_TABLE_FILE_MODE,
		.links = 1,
		.status = QNX4_STATUS_TABLE | QNX4_STATUS_USED
	};
	stamp_format_time(&record, plan->format_time);
	qnx4_inode_encode(&record,
	    raw + QNX4_RECORD_INODES * QNX4_RECORD_BYTES);

	status = boot_record(plan, plan->boot_row, &record, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4_inode_encode(&record, raw + QNX4_RECORD_BOOT * QNX4_RECORD_BYTES);
	status = boot_record(plan, plan->altboot_row, &record, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4_inode_encode(&record,
	    raw + QNX4_RECORD_ALTBOOT * QNX4_RECORD_BYTES);

	record = (qnx4_inode_t){
		.size = QNX4_LOST_FOUND_BLOCKS * QNX4_BLOCK_BYTES,
		.first_extent = {
			.first = plan->lost_found_first,
			.blocks = QNX4_LOST_FOUND_BLOCKS
		},
		.extents = 1,
		.mode = QNX4_LOST_FOUND_MODE,
		.links = 2,
		.status = QNX4_STATUS_TABLE | QNX4_STATUS_USED
	};
	stamp_format_time(&record, plan->format_time);
	qnx4_inode_encode(&record,
	    raw + QNX4_RECORD_LOST_FOUND * QNX4_RECORD_BYTES);

	/* Validation bounds the label to 48 bytes; NUL-padded, a 48-byte
	 * label has no terminator.
	 */
	length = strlen(label);
	if (length > QNX4_LABEL_BYTES)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 label exceeds its field"));
	bcopy(label, raw + QNX4_LABEL_OFFSET, length);
	return (fsimg_emit_write(emit, block_offset(QNX4_ROOT_BLOCK), raw,
	    sizeof(raw), error));
}

/* Step 3, the root: slots 2 to 6, slot 7 when no entry took it, and
 * slot 8.
 */
static fsimg_status_t
emit_fixed_slots(qnx4_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char raw[QNX4_RECORD_BYTES];
	qnx4_inode_t record;
	qnx4_link_t link;
	qnx4_row_t root;
	uint32_t root_first;
	fsimg_status_t status;
	bool names;

	root_first = qnx4_plan_root_first(plan);
	record = (qnx4_inode_t){
		.size = plan->geometry.bitmap_bytes,
		.first_extent = {
			.first = QNX4_BITMAP_FIRST,
			.blocks = plan->geometry.bitmap_blocks
		},
		.extents = 1,
		.mode = QNX4_TABLE_FILE_MODE,
		.links = 1,
		.status = QNX4_STATUS_USED
	};
	stamp_format_time(&record, plan->format_time);
	named_record(".bitmap", &record);
	status = emit_record(emit, slot_offset(root_first, QNX4_BITMAP_SLOT),
	    &record, error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(table_links) / sizeof(table_links[0]); i++)
		status = emit_link(emit,
		    slot_offset(root_first, table_links[i].slot),
		    table_links[i].name, strlen(table_links[i].name),
		    QNX4_POSITION(QNX4_ROOT_BLOCK, table_links[i].record), 0,
		    error);
	if (status != FSIMG_OK)
		return (status);

	bzero(&link, sizeof(link));
	link.name_length = (uint8_t)strlen("lost+found");
	bcopy("lost+found", link.name, link.name_length);
	link.target_block = QNX4_ROOT_BLOCK;
	link.target_slot = QNX4_RECORD_LOST_FOUND;
	link.status = QNX4_STATUS_HIDDEN;
	qnx4_link_encode(&link, raw);
	raw[QNX4_LOST_FOUND_MARK_OFFSET] = QNX4_LOST_FOUND_MARK;
	status = fsimg_emit_write(emit, slot_offset(root_first,
	    QNX4_LOST_FOUND_SLOT), raw, sizeof(raw), error);
	if (status == FSIMG_OK)
		status = qnx4_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	/* The root's first child takes slot 7; with none, slot 7 is free. */
	if (root.next_slot == QNX4_ROOT_ENTRY_SLOT) {
		bzero(raw, sizeof(raw));
		raw[QNX4_RECORD_BYTES - 1] = QNX4_EMPTY_SLOT_STATUS;
		status = fsimg_emit_write(emit, slot_offset(root_first,
		    QNX4_ROOT_ENTRY_SLOT), raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
	}

	/* Without long names, .longfilenames has no extent. */
	names = plan->names_blocks != 0;
	record = (qnx4_inode_t){
		.size = plan->names_blocks * QNX4_BLOCK_BYTES,
		.first_extent = {
			.first = names ? plan->names_first : 0,
			.blocks = plan->names_blocks
		},
		.extents = names ? 1 : 0,
		.mode = QNX4_TABLE_FILE_MODE,
		.links = 1,
		.status = QNX4_STATUS_USED
	};
	stamp_format_time(&record, plan->format_time);
	named_record(".longfilenames", &record);
	return (emit_record(emit,
	    slot_offset(root_first, QNX4_ROOT_NAMES_SLOT), &record, error));
}

/* Step 3, the tables: the .inodes signature in slot 0 and lost+found's dot
 * entries, whose ".." names the root's own "." slot as the formatter
 * writes it.
 */
static fsimg_status_t
emit_table_headers(qnx4_plan_t *plan, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	unsigned char raw[QNX4_RECORD_BYTES];
	fsimg_status_t status;

	bzero(raw, sizeof(raw));
	bcopy(inodes_signature, raw, sizeof(inodes_signature));
	qnx4_put32(raw + 16, QNX4_INODES_SIGNATURE_VALUE);
	status = fsimg_emit_write(emit, slot_offset(plan->inodes_first, 0), raw,
	    sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_dots(emit, (qnx4_extent_t){
	    .first = plan->lost_found_first,
	    .blocks = QNX4_LOST_FOUND_BLOCKS
	    }, QNX4_POSITION(QNX4_ROOT_BLOCK, QNX4_RECORD_LOST_FOUND),
	    QNX4_POSITION(qnx4_plan_root_first(plan), 0), error));
}

/* Byte k of the bitmap: bit i, block i + 1, is set when the block is
 * allocated, i < A, and as padding past the volume, i >= N. Bytes past
 * ceil(N / 8) hold only padding bits, so they are 0xff.
 */
static unsigned char
bitmap_byte(const qnx4_plan_t *plan, uint64_t k)
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

/* Step 4: the bitmap's B whole blocks from block 3, written in
 * buffer-sized batches.
 */
static fsimg_status_t
emit_bitmap(qnx4_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	uint64_t total, offset;
	size_t batch;
	fsimg_status_t status;

	total = (uint64_t)plan->geometry.bitmap_blocks * QNX4_BLOCK_BYTES;
	for (offset = 0; offset < total; offset += batch) {
		batch = total - offset > FSIMG_EMIT_BUFFER_BYTES ?
		    FSIMG_EMIT_BUFFER_BYTES : (size_t)(total - offset);
		for (size_t i = 0; i < batch; i++)
			buffer[i] = bitmap_byte(plan, offset + i);
		status = fsimg_emit_write(emit, QNX4_BITMAP_OFFSET + offset,
		    buffer, batch, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
qnx4_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	qnx4_plan_t *plan = (qnx4_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_entries(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_root_block(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_fixed_slots(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	status = emit_table_headers(plan, emit, error);
	if (status != FSIMG_OK)
		return (status);
	return (emit_bitmap(plan, emit, error));
}
