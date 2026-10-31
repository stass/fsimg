/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "qnx4.h"

/*
 * Records, identity and links. An object's identity is the position of its
 * inode record: its directory slot for a direct record, its .inodes slot
 * for an object named by link records, or its root-block record. Links
 * point only into .inodes; the three root links into the root block are
 * checked by position at open and never resolved here. A position inside
 * an .inodes extent is a table record wherever it is found, since .inodes
 * owns its blocks exclusively.
 */

static fsimg_status_t table_record(fsimg_reader_t *, uint64_t, qnx4_inode_t *,
    fsimg_error_t *);
static fsimg_timestamp_t utc_seconds(uint32_t);

/* Read the 64 bytes at a record position. Root-block records come from the
 * root block read at open; any other position must lie in [1, N].
 */
fsimg_status_t
qnx4_record_read(fsimg_reader_t *reader, uint64_t position, unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	uint32_t block = QNX4_POSITION_BLOCK(position);
	unsigned slot = QNX4_POSITION_SLOT(position);
	size_t transferred;

	if (block == QNX4_ROOT_BLOCK) {
		if (slot >= QNX4_ROOT_RECORDS)
			return (qnx4_corrupt(error,
			    "QNX4 position names no root-block record"));
		bcopy(qnx4->root_block + slot * QNX4_RECORD_BYTES, raw,
		    QNX4_RECORD_BYTES);
		return (FSIMG_OK);
	}
	if (block == 0 || block > qnx4->geometry.blocks)
		return (qnx4_corrupt(error, "QNX4 record position is out of range"));
	return (fsimg_reader_bytes(reader, qnx4_block_offset(block) +
	    slot * QNX4_RECORD_BYTES, raw, QNX4_RECORD_BYTES, &transferred,
	    error));
}

/* Apply the closed tables to an inode record found at place: its kind, its
 * status for that kind and place, zero reserved bytes and type byte, a
 * 16-NUL name field outside directory slots, a size within the documented
 * limit and the size rules of its kind. Root-block statuses depend on the
 * record; the root block's own checks apply them.
 */
fsimg_status_t
qnx4_inode_validate(const qnx4_inode_t *inode, qnx4_place_t place,
    fsimg_error_t *error)
{
	unsigned kind = inode->mode & QNX4_MODE_TYPE;
	uint8_t base;
	bool regular;

	if ((inode->status & (QNX4_STATUS_USED | QNX4_STATUS_LINK)) ==
	    (QNX4_STATUS_USED | QNX4_STATUS_LINK))
		return (qnx4_corrupt(error, "QNX4 record is both used and a link"));
	if (kind != QNX4_MODE_FILE && kind != QNX4_MODE_DIRECTORY &&
	    kind != QNX4_MODE_SYMLINK && kind != QNX4_MODE_FIFO)
		return (qnx4_unsupported(error, "QNX4 object kind is not supported"));
	if (place != QNX4_PLACE_ROOT) {
		/* Only regular files carry the modified bit. */
		regular = kind == QNX4_MODE_FILE;
		base = place == QNX4_PLACE_TABLE ?
		    QNX4_STATUS_TABLE | QNX4_STATUS_USED : QNX4_STATUS_USED;
		if (inode->status != base && (!regular ||
		    inode->status != (base | QNX4_STATUS_MODIFIED)))
			return (qnx4_unsupported(error,
			    "QNX4 record has an unexpected status"));
	}
	if (!inode->reserved_zero || inode->type != 0)
		return (qnx4_unsupported(error,
		    "QNX4 record has nonzero reserved bytes"));
	if (place == QNX4_PLACE_TABLE &&
	    (inode->name_length != 0 || !inode->name_padding_zero))
		return (qnx4_unsupported(error,
		    "QNX4 .inodes record has a name field"));
	if (inode->size > QNX4_FILE_MAX)
		return (qnx4_unsupported(error, "QNX4 object exceeds 2 GiB"));
	switch (kind) {
	case QNX4_MODE_DIRECTORY:
		if (inode->size == 0 || inode->size % QNX4_BLOCK_BYTES != 0)
			return (qnx4_corrupt(error,
			    "QNX4 directory size is not whole blocks"));
		break;
	case QNX4_MODE_SYMLINK:
		if (inode->size == 0)
			return (qnx4_corrupt(error, "QNX4 symbolic link is empty"));
		if (inode->size > QNX4_LINK_TARGET_MAX)
			return (qnx4_unsupported(error,
			    "QNX4 symbolic link target is too long"));
		break;
	default:
		break;
	}
	if (!qnx4_extents_consistent(inode))
		return (qnx4_corrupt(error,
		    "QNX4 extent count disagrees with the record's extents"));
	return (FSIMG_OK);
}

/* Read and validate the used .inodes record at position. A free slot, a
 * link record there, or a record outside the table's closed tables fails.
 */
static fsimg_status_t
table_record(fsimg_reader_t *reader, uint64_t position, qnx4_inode_t *inode,
    fsimg_error_t *error)
{
	unsigned char raw[QNX4_RECORD_BYTES];
	fsimg_status_t status;

	status = qnx4_record_read(reader, position, raw, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4_inode_decode(raw, inode);
	if ((inode->status & QNX4_STATUS_USED) == 0)
		return (qnx4_corrupt(error, "QNX4 link names a free .inodes slot"));
	return (qnx4_inode_validate(inode, QNX4_PLACE_TABLE, error));
}

/* Resolve a link record's target: a slot index of at most 7 in a block of
 * an .inodes extent, not the signature in slot 0, holding a used record.
 * Every other target -- the loader, the root block, the bitmap, a
 * directory or payload block -- lies outside .inodes and is corruption.
 * *table_slot is the target's logical .inodes slot.
 */
fsimg_status_t
qnx4_link_resolve(fsimg_reader_t *reader, const qnx4_link_t *link,
    uint64_t *position, uint64_t *table_slot, qnx4_inode_t *inode,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	uint64_t logical;
	bool found;
	fsimg_status_t status;

	*position = 0;
	*table_slot = 0;
	if (link->target_slot >= QNX4_SLOTS_PER_BLOCK)
		return (qnx4_corrupt(error, "QNX4 link target slot is out of range"));
	status = qnx4_locator_find(&qnx4->inodes_locator, link->target_block,
	    &found, &logical, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (qnx4_corrupt(error, "QNX4 link target lies outside .inodes"));
	if (logical == 0 && link->target_slot == 0)
		return (qnx4_corrupt(error, "QNX4 link names the .inodes signature"));
	*position = QNX4_POSITION(link->target_block, link->target_slot);
	*table_slot = logical * QNX4_SLOTS_PER_BLOCK + link->target_slot;
	return (table_record(reader, *position, inode, error));
}

/* Read the object record at a position an admitted entry recorded: a
 * root-block record (only .boot and .altboot are admitted from the root
 * block), an .inodes record, or a direct directory slot. *table_slot is the
 * logical .inodes slot of a table record.
 */
fsimg_status_t
qnx4_object_read(fsimg_reader_t *reader, uint64_t position,
    qnx4_inode_t *inode, qnx4_place_t *place, uint64_t *table_slot,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	unsigned char raw[QNX4_RECORD_BYTES];
	uint32_t block = QNX4_POSITION_BLOCK(position);
	unsigned slot = QNX4_POSITION_SLOT(position);
	uint64_t logical;
	bool found;
	fsimg_status_t status;

	*table_slot = 0;
	if (block == QNX4_ROOT_BLOCK) {
		if (slot != QNX4_RECORD_BOOT && slot != QNX4_RECORD_ALTBOOT)
			return (qnx4_corrupt(error,
			    "invalid prepared QNX4 entry payload"));
		*place = QNX4_PLACE_ROOT;
		*inode = qnx4->root[slot];
		return (FSIMG_OK);
	}
	status = qnx4_locator_find(&qnx4->inodes_locator, block, &found,
	    &logical, error);
	if (status != FSIMG_OK)
		return (status);
	if (found) {
		*place = QNX4_PLACE_TABLE;
		*table_slot = logical * QNX4_SLOTS_PER_BLOCK + slot;
		if (*table_slot == 0)
			return (qnx4_corrupt(error,
			    "invalid prepared QNX4 entry payload"));
		return (table_record(reader, position, inode, error));
	}
	*place = QNX4_PLACE_DIRECT;
	status = qnx4_record_read(reader, position, raw, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4_inode_decode(raw, inode);
	if (qnx4_slot_free(raw) || qnx4_slot_link(raw))
		return (qnx4_corrupt(error, "invalid prepared QNX4 entry payload"));
	return (qnx4_inode_validate(inode, QNX4_PLACE_DIRECT, error));
}

static fsimg_timestamp_t
utc_seconds(uint32_t seconds)
{
	return ((fsimg_timestamp_t){
		.seconds = seconds,
		.precision_ns = 1000000000,
		.basis = FSIMG_TIME_UTC
	});
}

/* Map a validated record to an entry snapshot without its path. Directory
 * and FIFO sizes are zero in the entry model; a FIFO's recorded size and
 * blocks are never content. Times are unsigned seconds since 1970 UTC.
 */
fsimg_status_t
qnx4_inode_entry(const qnx4_inode_t *inode, fsimg_entry_t *entry,
    fsimg_error_t *error)
{
	bzero(&entry->metadata, sizeof(entry->metadata));
	entry->size = 0;
	switch (inode->mode & QNX4_MODE_TYPE) {
	case QNX4_MODE_FILE:
		entry->kind = FSIMG_ENTRY_FILE;
		entry->size = inode->size;
		break;
	case QNX4_MODE_DIRECTORY:
		entry->kind = FSIMG_ENTRY_DIRECTORY;
		break;
	case QNX4_MODE_SYMLINK:
		entry->kind = FSIMG_ENTRY_SYMLINK;
		entry->size = inode->size;
		break;
	case QNX4_MODE_FIFO:
		entry->kind = FSIMG_ENTRY_FIFO;
		break;
	default:
		return (qnx4_unsupported(error, "QNX4 object kind is not supported"));
	}
	entry->metadata.valid = FSIMG_META_CREATED | FSIMG_META_MODIFIED |
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED | FSIMG_META_OWNER |
	    FSIMG_META_MODE;
	entry->metadata.uid = inode->uid;
	entry->metadata.gid = inode->gid;
	entry->metadata.mode = inode->mode & FSIMG_MODE_BITS;
	entry->metadata.created = utc_seconds(inode->created);
	entry->metadata.modified = utc_seconds(inode->modified);
	entry->metadata.accessed = utc_seconds(inode->accessed);
	entry->metadata.changed = utc_seconds(inode->changed);
	return (FSIMG_OK);
}
