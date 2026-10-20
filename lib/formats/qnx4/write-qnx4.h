/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_WRITE_QNX4_H
#define FSIMG_WRITE_QNX4_H

#include "emit.h"
#include "format.h"
#include "qnx4.h"

/* The native formatter's default layout, which the writer reproduces: the
 * modes and sizes of the format-owned objects, the 4096-byte directory
 * rounding, the 128-slot .inodes floor and the 16-block .longfilenames
 * step, and the two root slots the formatter fills after the fixed six.
 */
#define QNX4_ROOT_MODE		(QNX4_MODE_DIRECTORY | 0775u)
#define QNX4_TABLE_FILE_MODE	(QNX4_MODE_FILE | 0444u)
#define QNX4_BOOT_PERMISSIONS	0600u
#define QNX4_LOST_FOUND_MODE	(QNX4_MODE_DIRECTORY | 0755u)
#define QNX4_LOST_FOUND_BLOCKS	16u
#define QNX4_DIRECTORY_STEP	8u	/* blocks: one 4096-byte step */
#define QNX4_SLOTS_PER_STEP	(QNX4_DIRECTORY_STEP * QNX4_SLOTS_PER_BLOCK)
#define QNX4_INODES_MIN		128u
#define QNX4_NAMES_STEP		16u
#define QNX4_ROOT_ENTRY_SLOT	7u	/* the first catalog root entry */
#define QNX4_ROOT_NAMES_SLOT	8u	/* .longfilenames */
#define QNX4_OWNER_MAX		65534u
#define QNX4_TIME_MAX		INT64_C(2147483647)
#define QNX4_LINKS_MAX		UINT16_MAX

/* .inodes holds at most this many slots, slot 0 included: its size is a
 * whole number of blocks no larger than the QNX4 object maximum, which
 * readers enforce on every record.
 */
#define QNX4_INODE_SLOTS_MAX						\
	(QNX4_FILE_MAX / QNX4_BLOCK_BYTES * QNX4_SLOTS_PER_BLOCK)

/* An absent .inodes slot or long-name record in a row. */
#define QNX4_NONE		UINT32_MAX

/* One plan row per catalog ID, row 0 for the root. A row is everything the
 * object's records and storage need, apart from its name, which stays in
 * the catalog:
 *   size	file or symbolic-link bytes, a directory's whole-block slot
 *		bytes, zero for a FIFO
 *   first	the first block of the object's one extent, 0 without one
 *   blocks	that extent's length
 *   record_block, record_slot
 *		the object's identity: the position of its inode record, a
 *		direct slot in its parent, an .inodes slot or a root-block
 *		record, the last only for /.boot and /.altboot; the layout
 *		pass fixes it
 *   slot	the entry's slot index in its parent directory
 *   next_slot	for a directory, the next slot index to assign
 *   inode_slot	the .inodes slot of an entry with a link name, or QNX4_NONE
 *   long_name	the .longfilenames record index of a name over 48 bytes, or
 *		QNX4_NONE
 * Owner, mode, links, status and the four times are already encoded under
 * the metadata policy, so emission never reads metadata again. The 64-byte
 * wire layout follows the field order below, little endian.
 */
typedef struct qnx4_row {
	uint32_t	 size;
	uint32_t	 first;
	uint32_t	 blocks;
	uint32_t	 record_block;
	uint32_t	 slot;
	uint32_t	 next_slot;
	uint32_t	 inode_slot;
	uint32_t	 long_name;
	uint32_t	 created;
	uint32_t	 modified;
	uint32_t	 accessed;
	uint32_t	 changed;
	uint16_t	 uid;
	uint16_t	 gid;
	uint16_t	 mode;
	uint16_t	 links;
	uint8_t		 record_slot;
	uint8_t		 status;
} qnx4_row_t;

#define QNX4_ROW_BYTES		64u

/* The plan: the geometry, the format-owned objects' first blocks and
 * lengths, the row array and the totals the passes derive. The root
 * directory starts at 3 + B. boot_row and altboot_row are the catalog IDs
 * of /.boot and /.altboot, or 0 (the root's ID, never one of them) when the
 * catalog has none. allocated is A: blocks 1 to A are in use and none
 * above.
 */
typedef struct qnx4_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*rows;
	qnx4_geometry_t	 geometry;
	uint64_t	 boot_row;
	uint64_t	 altboot_row;
	uint64_t	 objects;		/* catalog objects' blocks */
	uint64_t	 payload_blocks;	/* file and link content */
	uint32_t	 root_blocks;		/* R */
	uint32_t	 inodes_blocks;		/* I */
	uint32_t	 names_blocks;		/* L */
	uint32_t	 inodes_first;
	uint32_t	 lost_found_first;
	uint32_t	 names_first;
	uint32_t	 inode_slots;		/* capacity, slot 0 included */
	uint32_t	 used_inode_slots;	/* assigned from slot 1 */
	uint32_t	 long_names;
	uint32_t	 allocated;
	uint32_t	 format_time;
} qnx4_plan_t;

extern const fsimg_writer_ops_t fsimg_qnx4_writer_ops;

/* The root directory's first block, fixed at 3 + B. */
static inline uint32_t
qnx4_plan_root_first(const qnx4_plan_t *plan)
{
	return (QNX4_BITMAP_FIRST + plan->geometry.bitmap_blocks);
}

fsimg_status_t qnx4_row_get(qnx4_plan_t *, uint64_t, qnx4_row_t *,
    fsimg_error_t *);
fsimg_status_t qnx4_row_set(qnx4_plan_t *, uint64_t, const qnx4_row_t *,
    fsimg_error_t *);
fsimg_status_t qnx4_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

#endif
