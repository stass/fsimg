/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "qnx4.h"

/* The allocation bitmap pager. The bitmap's position follows from the
 * geometry alone: B blocks from block 3, so its bytes start at volume
 * offset 1024. Bit k of those bytes, least significant first within each
 * byte, is block k + 1, and a set bit means allocated. The pager keeps one
 * 4 KiB window of the bitmap bytes, aligned to 4 KiB from their start, and
 * answers whether a run of blocks is allocated a window at a time, so a
 * long extent costs one read per 32768 blocks.
 */

static fsimg_status_t load_window(fsimg_reader_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t bits_set(fsimg_reader_t *, uint64_t, uint64_t, bool *,
    fsimg_error_t *);

/* Make the window holding bitmap byte `byte` current. */
static fsimg_status_t
load_window(fsimg_reader_t *reader, uint64_t byte, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	uint64_t window, region;
	size_t length, transferred;
	fsimg_status_t status;

	window = byte / QNX4_WINDOW_BYTES * QNX4_WINDOW_BYTES;
	if (qnx4->bitmap_window_length != 0 &&
	    qnx4->bitmap_window_offset == window)
		return (FSIMG_OK);
	/* The region is whole bitmap blocks, at least ceil(N / 8) bytes, so
	 * the window holds every byte of a block in [1, N].
	 */
	region = (uint64_t)qnx4->geometry.bitmap_blocks * QNX4_BLOCK_BYTES;
	length = region - window > QNX4_WINDOW_BYTES ? QNX4_WINDOW_BYTES :
	    (size_t)(region - window);
	qnx4->bitmap_window_length = 0;
	status = fsimg_reader_bytes(reader, QNX4_BITMAP_OFFSET + window,
	    qnx4->bitmap_window, length, &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4->bitmap_window_offset = window;
	qnx4->bitmap_window_length = length;
	return (FSIMG_OK);
}

/* Whether bits [first, first + count) of the bitmap bytes are all set. The
 * caller keeps the range inside the bitmap region.
 */
static fsimg_status_t
bits_set(fsimg_reader_t *reader, uint64_t first, uint64_t count, bool *all,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	uint64_t bit, end, byte, low, high;
	unsigned mask;
	fsimg_status_t status;

	*all = false;
	end = first + count;
	/* One bitmap byte per pass: bits [low, high) of byte `byte`. */
	for (bit = first; bit < end; bit = (byte + 1) * 8) {
		byte = bit / 8;
		status = load_window(reader, byte, error);
		if (status != FSIMG_OK)
			return (status);
		low = bit % 8;
		high = end - byte * 8 < 8 ? end - byte * 8 : 8;
		mask = (0xffu >> (8 - (high - low))) << low;
		if ((qnx4->bitmap_window[byte - qnx4->bitmap_window_offset] &
		    mask) != mask)
			return (FSIMG_OK);
	}
	*all = true;
	return (FSIMG_OK);
}

/* Check the bitmap against the geometry before any record is trusted, in
 * two bounded windows: the first window must mark the loader, the root
 * block and the bitmap blocks it describes allocated, and the last bitmap
 * block must mark every bit from N to the end of its last used byte
 * allocated and hold 0xff in every byte after that. Those bits describe no
 * block, so they never count as free. The rest of the bitmap's own blocks
 * are checked by full verification.
 */
fsimg_status_t
qnx4_bitmap_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_geometry_t *g = &qnx4->geometry;
	unsigned char last[QNX4_BLOCK_BYTES];
	uint64_t system, tail, region;
	size_t transferred;
	bool all;
	fsimg_status_t status;

	system = 2 + (uint64_t)g->bitmap_blocks;
	if (system > QNX4_WINDOW_BYTES * 8)
		system = QNX4_WINDOW_BYTES * 8;
	status = bits_set(reader, 0, system, &all, error);
	if (status != FSIMG_OK)
		return (status);
	if (!all)
		return (qnx4_corrupt(error, "QNX4 bitmap marks its own blocks free"));

	/* The padding bits N .. 8 * ceil(N / 8) - 1 end the used bytes. */
	tail = (uint64_t)g->bitmap_bytes * 8 - g->blocks;
	status = bits_set(reader, g->blocks, tail, &all, error);
	if (status != FSIMG_OK)
		return (status);
	if (!all)
		return (qnx4_corrupt(error, "QNX4 bitmap marks blocks past N free"));
	region = (uint64_t)g->bitmap_blocks * QNX4_BLOCK_BYTES;
	status = fsimg_reader_bytes(reader, QNX4_BITMAP_OFFSET + region -
	    QNX4_BLOCK_BYTES, last, sizeof(last), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	/* ceil(N / 8) lies inside the last bitmap block, since
	 * B = ceil(N / 4096).
	 */
	for (uint64_t i = g->bitmap_bytes - (region - QNX4_BLOCK_BYTES);
	    i < QNX4_BLOCK_BYTES; i++)
		if (last[i] != 0xff)
			return (qnx4_corrupt(error,
			    "QNX4 bitmap padding is not 0xff"));
	return (FSIMG_OK);
}

/* Fail unless blocks [first, first + count) are all allocated. The caller
 * has checked that the run lies in [1, N].
 */
fsimg_status_t
qnx4_blocks_allocated(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    fsimg_error_t *error)
{
	bool all;
	fsimg_status_t status;

	status = bits_set(reader, (uint64_t)first - 1, count, &all, error);
	if (status != FSIMG_OK)
		return (status);
	if (!all)
		return (qnx4_corrupt(error, "QNX4 referenced block is free"));
	return (FSIMG_OK);
}
