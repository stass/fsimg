/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_IFS_H
#define FSIMG_WRITE_IFS_H

#include "emit.h"
#include "format.h"
#include "ifs.h"

/* The ImageFS writer (specs/design-ifs-driver.md, Writer settings,
 * Planning and Emission). It lays an image out in the writer profile's one
 * deterministic order: the header and the mountpoint padded to 4, the root
 * record and then one record per catalog entry in ID order, each padded to
 * 4, the terminator, each regular file's data in ID order on a 4-byte
 * boundary, and the trailer.
 */

/* The last offset a planned image may reach: its trailer starts below the
 * largest image_size.
 */
#define IFS_TRAILER_MAX		(IFS_VOLUME_MAX - 4)

/* The longest record, a link with the longest path and target, fits the
 * emission buffer it is encoded in.
 */
_Static_assert(IFS_REC_LINK_PATH + IFS_PATH_MAX + 1 + IFS_LINK_TARGET_MAX + 1
    + 3 <= FSIMG_EMIT_BUFFER_BYTES, "an ImageFS record fits the emit buffer");

/* Records and file data start on 4-byte boundaries. */
static inline uint64_t
ifs_round4(uint64_t value)
{
	return ((value + 3) & ~UINT64_C(3));
}

/* One plan row per catalog ID; row 0 is the root. A row is everything the
 * entry's record and data need, apart from its path, which stays in the
 * catalog:
 *   kind		the record's kind
 *   path_length	the stored path's bytes, the image path without its
 *			leading slash
 *   length		a regular file's bytes or a symbolic link's target
 *			bytes; zero for a directory or a FIFO
 *   table_offset	where the record starts, and record_length its bytes,
 *			the kind's minimum rounded up to 4
 *   data_offset	where a regular file's data starts
 *   uid, gid, permissions, mtime
 *			already encoded under the metadata policy
 * The inode word is the catalog ID plus 1 with no flags, a pure function of
 * the row's key, so it is not stored. The 40-byte wire layout follows the
 * field order, little endian.
 */
typedef struct ifs_row {
	ifs_kind_t	 kind;
	uint32_t	 path_length;
	uint32_t	 length;
	uint32_t	 table_offset;
	uint32_t	 record_length;
	uint32_t	 data_offset;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 permissions;
	uint32_t	 mtime;
} ifs_row_t;

#define IFS_ROW_BYTES		40u

/* The plan: the byte order and the mountpoint the settings select, the
 * offsets the passes derive, the fallback time of the root and of every
 * entry without a time, and the row array.
 */
typedef struct ifs_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*rows;
	ifs_byte_order_t order;
	uint32_t	 dir_offset;
	uint32_t	 hdr_dir_size;
	uint32_t	 image_size;
	uint32_t	 fallback_time;
	uint32_t	 mountpoint_length;
	char		 mountpoint[IFS_PATH_MAX + 1];
} ifs_plan_t;

extern const fsimg_writer_ops_t fsimg_ifs_writer_ops;

fsimg_status_t ifs_row_get(ifs_plan_t *, uint64_t, ifs_row_t *,
    fsimg_error_t *);
fsimg_status_t ifs_row_set(ifs_plan_t *, uint64_t, const ifs_row_t *,
    fsimg_error_t *);
fsimg_status_t ifs_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

#endif
