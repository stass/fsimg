/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_EMIT_H
#define FSIMG_EMIT_H

#include "writer.h"

#define FSIMG_EMIT_BUFFER_BYTES 65536u

/* One contiguous part of a file's planned allocation. A file's regions are
 * listed in logical order; their capacities sum to its allocated bytes.
 */
typedef struct fsimg_emit_region {
	uint64_t	 image_offset;
	uint64_t	 capacity;
} fsimg_emit_region_t;

/* Where one file's payload goes. length is the file length the plan records
 * and must equal the catalog's. initialized is the prefix the format stores,
 * with initialized <= length <= total region capacity; everything after it
 * stays zero. An empty file has no regions.
 */
typedef struct fsimg_emit_payload {
	const fsimg_emit_region_t *regions;
	size_t		 region_count;
	uint64_t	 length;
	uint64_t	 initialized;
} fsimg_emit_payload_t;

/* Emission is the only way a format driver writes image bytes. The emission
 * object is opaque: drivers cannot reach the sink, and the copy operation is
 * their only path to source content.
 *
 * fsimg_emit_image() runs after the writer's pre-mutation checks and resize.
 * It zeroes every byte of the planned volume, then constructs the emission
 * object and passes it to the driver's emit operation. The object therefore
 * exists only once the destination is known to be zero, which is what lets
 * the copy operation skip zero ranges. It borrows the writer and its session
 * and lives only for the duration of the driver's emit call.
 */
fsimg_status_t fsimg_emit_image(fsimg_writer_t *, fsimg_io_t *,
    fsimg_error_t *);

/* The emission transfer buffer holds FSIMG_EMIT_BUFFER_BYTES. A driver may
 * encode metadata in it between operations. Its contents are unspecified
 * whenever the driver takes it, and fsimg_emit_copy() overwrites it, so a
 * driver initializes every byte it then writes.
 */
unsigned char *fsimg_emit_buffer(fsimg_emit_t *);

/* Write encoded bytes at a volume offset. The range must lie within the
 * planned volume; whether the bytes belong there is the driver's concern.
 */
fsimg_status_t fsimg_emit_write(fsimg_emit_t *, uint64_t, const void *,
    size_t, fsimg_error_t *);

/* Copy catalog entry id into its planned payload placement. A placement that
 * is inconsistent or leaves the volume fails before the source is opened.
 * The operation opens, copies, validates and closes the entry's source; the
 * source is closed on every outcome.
 */
fsimg_status_t fsimg_emit_copy(fsimg_emit_t *, uint64_t,
    const fsimg_emit_payload_t *, fsimg_error_t *);

#endif
