/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "qnx6.h"

/* The allocation bitmap: bit p, least significant first within byte
 * floor(p / 8), is set when block p is allocated. The bitmap is itself a
 * block tree whose blocks must be allocated, so it is bootstrapped: index it
 * while recording every block the walk visits, then check each recorded
 * block's bit through the finished index. The blocks whose bits the check
 * reads are exactly the blocks the index covers, so one pass closes the
 * self-reference.
 */

static fsimg_status_t check_recorded(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t check_tail(fsimg_reader_t *, fsimg_error_t *);

static fsimg_status_t
check_recorded(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	unsigned char record[4];
	uint64_t cursor;
	uint32_t block;
	size_t length;
	bool allocated;
	fsimg_status_t status;

	status = fsimg_stream_seal(qnx6->record, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	for (;;) {
		status = fsimg_stream_next(qnx6->record, &cursor, record,
		    sizeof(record), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		block = fsimg_meta_get_u32(record);
		if (length != sizeof(record) || block >= qnx6->geometry.blocks)
			return (qnx6_corrupt(error, "invalid QNX6 bitmap record"));
		status = qnx6_block_allocated(reader, block, &allocated, error);
		if (status != FSIMG_OK)
			return (status);
		if (!allocated)
			return (qnx6_corrupt(error, "QNX6 bitmap block is marked free"));
	}
}

/* Bits at and past N in the bitmap's last block describe no block; the
 * classic profile sets them, so they are never counted as free.
 */
static fsimg_status_t
check_tail(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	unsigned char bytes[QNX6_WINDOW_BYTES];
	uint64_t offset, end;
	unsigned mask;
	size_t length;
	fsimg_status_t status;

	offset = g->blocks / 8;
	end = fsimg_extent_index_length(qnx6->bitmap);
	/* The index covers whole leaves: ceil(N / 8) bytes rounded up to a
	 * block. The tail from byte floor(N / 8) to the end of the last leaf
	 * is therefore at most one block, which fits the 4 KiB buffer.
	 */
	length = (size_t)(end - offset);
	if (length == 0)
		return (FSIMG_OK);
	status = qnx6_index_read(reader, qnx6->bitmap, offset, bytes, length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	mask = 0xffu & ~((1u << (g->blocks % 8)) - 1);
	if ((bytes[0] & mask) != mask)
		return (qnx6_corrupt(error, "QNX6 bitmap marks blocks past N free"));
	for (size_t i = 1; i < length; i++)
		if (bytes[i] != 0xff)
			return (qnx6_corrupt(error, "QNX6 bitmap marks blocks past N free"));
	return (FSIMG_OK);
}

fsimg_status_t
qnx6_bitmap_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	qnx6_object_t bitmap = {
		.tree = &qnx6->header.bitmap,
		.owner = QNX6_OWNER_BITMAP,
		.domain = QNX6_DOMAIN_SYSTEM,
		.role = QNX6_ROLE_BITMAP,
		.policy = QNX6_WALK_BOOTSTRAP
	};
	fsimg_status_t status;

	status = fsimg_stream_new(&reader->session, 4, &qnx6->record, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx6_index_build(reader, &bitmap, &qnx6->bitmap, error);
	if (status == FSIMG_OK) {
		qnx6->bitmap_ready = true;
		status = check_recorded(reader, error);
	}
	fsimg_stream_free(qnx6->record);
	qnx6->record = NULL;
	if (status != FSIMG_OK)
		return (status);
	return (check_tail(reader, error));
}

/* Whether block p < N is allocated, through a 4 KiB window of the bitmap
 * retained between calls.
 */
fsimg_status_t
qnx6_block_allocated(fsimg_reader_t *reader, uint32_t block, bool *allocated,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	uint64_t byte, window, remaining;
	size_t length;
	fsimg_status_t status;

	*allocated = false;
	if (!qnx6->bitmap_ready || block >= qnx6->geometry.blocks)
		return (qnx6_corrupt(error, "QNX6 allocation query out of range"));
	byte = block / 8;
	window = byte / QNX6_WINDOW_BYTES * QNX6_WINDOW_BYTES;
	if (qnx6->bitmap_window_length == 0 ||
	    qnx6->bitmap_window_offset != window) {
		/* The index covers at least ceil(N / 8) bytes, so the window
		 * holds the queried byte.
		 */
		remaining = fsimg_extent_index_length(qnx6->bitmap) - window;
		length = remaining > QNX6_WINDOW_BYTES ? QNX6_WINDOW_BYTES :
		    (size_t)remaining;
		qnx6->bitmap_window_length = 0;
		status = qnx6_index_read(reader, qnx6->bitmap, window,
		    qnx6->bitmap_window, length, error);
		if (status != FSIMG_OK)
			return (status);
		qnx6->bitmap_window_offset = window;
		qnx6->bitmap_window_length = length;
	}
	*allocated = (qnx6->bitmap_window[byte - window] >> (block % 8) & 1) != 0;
	return (FSIMG_OK);
}
