/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "qnx6.h"

static fsimg_timestamp_t utc_seconds(uint32_t);

/* Read inode i, which a live directory slot names, from the selected inode
 * table and validate it. A committed snapshot never names a free or
 * malformed inode, so a failure is corruption unless the inode is valid but
 * outside the classic profile.
 */
fsimg_status_t
qnx6_inode_read(fsimg_reader_t *reader, uint32_t number, qnx6_inode_t *inode,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	unsigned char raw[QNX6_INODE_BYTES];
	fsimg_status_t status;

	if (number == 0 || number > g->inode_count)
		return (qnx6_corrupt(error, "QNX6 inode number is out of range"));
	status = qnx6_index_read(reader, qnx6->inodes,
	    (uint64_t)(number - 1) * QNX6_INODE_BYTES, raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	qnx6_inode_decode(raw, g->big_endian, inode);

	if ((inode->status & ~(QNX6_STATUS_ALLOCATED | QNX6_STATUS_CREATED)) != 0 ||
	    !inode->tail_zero)
		return (qnx6_unsupported(error, "QNX6 inode has unknown state or fields"));
	if ((inode->status & QNX6_STATUS_ALLOCATED) == 0)
		return (qnx6_corrupt(error, "QNX6 directory names a free inode"));
	if (inode->tree.depth > QNX6_DEPTH_MAX ||
	    qnx6_tree_leaves(inode->size, g->block_size) > g->blocks)
		return (qnx6_corrupt(error, "QNX6 inode tree exceeds the volume"));
	switch (inode->mode & QNX6_MODE_TYPE) {
	case QNX6_MODE_FILE:
		return (FSIMG_OK);
	case QNX6_MODE_DIRECTORY:
		if (inode->size == 0 || inode->size % QNX6_SLOT_BYTES != 0)
			return (qnx6_corrupt(error, "QNX6 directory length is not whole slots"));
		return (FSIMG_OK);
	case QNX6_MODE_SYMLINK:
		if (inode->size == 0)
			return (qnx6_corrupt(error, "QNX6 symbolic link is empty"));
		if (inode->size > QNX6_LINK_TARGET_MAX)
			return (qnx6_unsupported(error, "QNX6 symbolic link target is too long"));
		return (FSIMG_OK);
	case QNX6_MODE_FIFO:
		if (inode->size != 0)
			return (qnx6_corrupt(error, "QNX6 FIFO has content"));
		return (FSIMG_OK);
	default:
		return (qnx6_unsupported(error, "QNX6 inode type is not supported"));
	}
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

/* Map a validated inode to an entry snapshot without its path. Directory
 * and FIFO sizes are zero in the entry model. Times are unsigned Unix
 * seconds; the creation time is the inode's ftime.
 */
fsimg_status_t
qnx6_inode_entry(const qnx6_inode_t *inode, fsimg_entry_t *entry,
    fsimg_error_t *error)
{
	bzero(&entry->metadata, sizeof(entry->metadata));
	entry->size = 0;
	switch (inode->mode & QNX6_MODE_TYPE) {
	case QNX6_MODE_FILE:
		entry->kind = FSIMG_ENTRY_FILE;
		entry->size = inode->size;
		break;
	case QNX6_MODE_DIRECTORY:
		entry->kind = FSIMG_ENTRY_DIRECTORY;
		break;
	case QNX6_MODE_SYMLINK:
		entry->kind = FSIMG_ENTRY_SYMLINK;
		entry->size = inode->size;
		break;
	case QNX6_MODE_FIFO:
		entry->kind = FSIMG_ENTRY_FIFO;
		break;
	default:
		return (qnx6_unsupported(error, "QNX6 inode type is not supported"));
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
