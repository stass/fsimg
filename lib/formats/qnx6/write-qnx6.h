/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_QNX6_H
#define FSIMG_WRITE_QNX6_H

#include "emit.h"
#include "qnx6.h"

/* Inode numbers the classic profile fixes, and the modes of the two
 * formatter-created directories.
 */
#define QNX6_BOOT_INODE		2u
#define QNX6_ROOT_MODE		(QNX6_MODE_DIRECTORY | 0775u)
#define QNX6_BOOT_MODE		(QNX6_MODE_DIRECTORY | 0700u)
#define QNX6_LINKS_MAX		UINT16_MAX

/* One plan row per catalog ID; row 0 is the root and row count + 1 the
 * synthesized .boot when the catalog has none. A row is everything the
 * inode, the entry's slot and its storage need, apart from the name, which
 * stays in the catalog:
 *   length	file or symbolic-link size, or a directory's whole
 *		blocks of next_slot slots, which the layout pass sets once
 *		the block size is chosen; zero for a FIFO
 *   first	first block of the object's range: its indirect blocks in
 *		post-order, then its leaves
 *   slot	the entry's slot index in its parent directory
 *   next_slot	for a directory, the slots assigned so far, "." and ".."
 *		included
 *   long_name	the long-name record index, or QNX6_SENTINEL for an inline
 *		name
 *   mode	the type and permission bits; links and status as stored
 * Times, owner and mode are already encoded under the metadata policy, so
 * emission never re-reads metadata. The 64-byte wire layout follows the
 * field order below, little endian.
 */
typedef struct qnx6_row {
	uint64_t	 length;
	uint32_t	 first;
	uint32_t	 inode;
	uint32_t	 slot;
	uint32_t	 next_slot;
	uint32_t	 long_name;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 created;
	uint32_t	 modified;
	uint32_t	 accessed;
	uint32_t	 changed;
	uint16_t	 mode;
	uint16_t	 links;
	uint8_t		 status;
} qnx6_row_t;

#define QNX6_ROW_BYTES		64u

/* The plan: the geometry, the three system-tree shapes, the row array and
 * the totals the passes derive for the chosen block size: objects, every
 * object's blocks, and payload_blocks, those of file and link content.
 * boot_row is the catalog ID of /.boot or count + 1. allocated is A: every
 * block below it is in use, and none above it. The geometry's block size
 * is zero until the geometry pass chooses it.
 */
typedef struct qnx6_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*rows;
	qnx6_geometry_t	 geometry;
	qnx6_shape_t	 bitmap;
	qnx6_shape_t	 inodes;
	qnx6_shape_t	 names;
	uint64_t	 boot_row;
	uint64_t	 objects;
	uint64_t	 payload_blocks;
	uint32_t	 names_first;
	uint32_t	 allocated;
	uint32_t	 used_inodes;
	uint32_t	 format_time;
} qnx6_plan_t;

extern const fsimg_writer_ops_t fsimg_qnx6_writer_ops;

/* Whether .boot is synthesized: its row then follows the catalog's. */
static inline bool
qnx6_boot_synthesized(const qnx6_plan_t *plan)
{
	return (plan->boot_row == plan->common.writer->count + 1);
}

fsimg_status_t qnx6_row_get(qnx6_plan_t *, uint64_t, qnx6_row_t *,
    fsimg_error_t *);
fsimg_status_t qnx6_row_set(qnx6_plan_t *, uint64_t, const qnx6_row_t *,
    fsimg_error_t *);
bool qnx6_object_shape(const qnx6_plan_t *, uint64_t, qnx6_shape_t *);
fsimg_status_t qnx6_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

#endif
