/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "emit.h"
#include "format.h"
#include "io.h"
#include "source.h"

/* The sink and buffer are borrowed from fsimg_emit_image() for one driver
 * emit call; volume_bytes is the planned volume every write must fit.
 */
struct fsimg_emit {
	fsimg_writer_t	*writer;
	fsimg_io_t	*output;
	unsigned char	*buffer;
	uint64_t	 volume_bytes;
};

/* Position in a file's regions. logical_start is the logical file offset of
 * the current region's first byte; image offsets come from the region itself.
 */
typedef struct region_cursor {
	const fsimg_emit_region_t *region;
	uint64_t	 logical_start;
} region_cursor_t;

static fsimg_status_t check_payload(const fsimg_emit_t *,
    const fsimg_emit_payload_t *, fsimg_error_t *);
static void region_seek(region_cursor_t *, uint64_t);
static fsimg_status_t copy_data(fsimg_emit_t *, region_cursor_t *, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t copy_initialized(fsimg_emit_t *,
    const fsimg_emit_payload_t *, fsimg_error_t *);

fsimg_status_t
fsimg_emit_image(fsimg_writer_t *writer, fsimg_io_t *output,
    fsimg_error_t *error)
{
	fsimg_emit_t emit;
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	buffer = fsimg_alloc(writer->ctx, FSIMG_EMIT_BUFFER_BYTES, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* A fresh file writes zeros only where it was written before, by its
	 * record; every other sink is zeroed in full here. The emission
	 * object is bound only once this succeeds.
	 */
	status = fsimg_io_zero_session(output, &writer->session, 0,
	    writer->plan->info.volume_bytes, error);
	if (status == FSIMG_OK) {
		emit.writer = writer;
		emit.output = output;
		emit.buffer = buffer;
		emit.volume_bytes = writer->plan->info.volume_bytes;
		status = writer->format->writer->emit(writer, &emit, error);
	}
	fsimg_dealloc(buffer);
	return (status);
}

unsigned char *
fsimg_emit_buffer(fsimg_emit_t *emit)
{
	return (emit->buffer);
}

fsimg_status_t
fsimg_emit_write(fsimg_emit_t *emit, uint64_t volume_offset, const void *bytes,
    size_t length, fsimg_error_t *error)
{
	size_t transferred;

	if (!fsimg_range_fits(volume_offset, length, emit->volume_bytes))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "emission write exceeds the planned volume"));
	return (fsimg_io_write_session(emit->output, &emit->writer->session,
	    volume_offset, bytes, length, &transferred, error));
}

fsimg_status_t
fsimg_emit_copy(fsimg_emit_t *emit, uint64_t id,
    const fsimg_emit_payload_t *payload, fsimg_error_t *error)
{
	fsimg_writer_t *writer = emit->writer;
	fsimg_status_t status;

	status = check_payload(emit, payload, error);
	if (status != FSIMG_OK)
		return (status);

	/* Empty and zero-only sources are opened too: validation at open and
	 * finish detects a changed source even when no bytes move. Every
	 * failure after this point leaves through fail, which closes the source
	 * whichever layer failed.
	 */
	status = fsimg_source_open(writer, id, error);
	if (status != FSIMG_OK)
		goto fail;
	if (writer->source.length != payload->length) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned payload length differs from its source");
		goto fail;
	}
	status = copy_initialized(emit, payload, error);
	if (status != FSIMG_OK)
		goto fail;
	return (fsimg_source_finish(writer, error));
fail:
	fsimg_source_close(writer);
	return (status);
}

/* Validate a payload placement before any source is touched: each region lies
 * within the planned volume, and initialized <= length <= total capacity.
 * length <= capacity is what keeps region_seek() inside the regions array.
 */
static fsimg_status_t
check_payload(const fsimg_emit_t *emit, const fsimg_emit_payload_t *payload,
    fsimg_error_t *error)
{
	const fsimg_emit_region_t *region;
	uint64_t capacity;

	if (payload->region_count != 0 && payload->regions == NULL)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "payload regions are missing"));
	capacity = 0;
	for (size_t i = 0; i < payload->region_count; i++) {
		region = &payload->regions[i];
		if (!fsimg_range_fits(region->image_offset, region->capacity,
		    emit->volume_bytes))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "payload region exceeds the planned volume"));
		if (!fsimg_add_u64(capacity, region->capacity, &capacity))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "payload region capacities overflow"));
	}
	if (payload->initialized > payload->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned initialized length exceeds file length"));
	if (payload->length > capacity)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned payload exceeds its allocation"));
	return (FSIMG_OK);
}

/* Advance the cursor to the region holding logical_offset, skipping
 * zero-capacity regions. The caller keeps logical_offset below the total
 * capacity, so this stops at or before the last region, and check_payload()
 * bounded the capacity sum, so logical_start + capacity cannot overflow.
 */
static void
region_seek(region_cursor_t *cursor, uint64_t logical_offset)
{
	while (logical_offset >= cursor->logical_start + cursor->region->capacity) {
		cursor->logical_start += cursor->region->capacity;
		cursor->region++;
	}
}

/* Copy the DATA bytes [logical_offset, end) of the open source. Each batch is
 * clipped to the buffer and to the region holding its first byte, so one
 * batch never spans two regions. fsimg_source_read() reads exactly or fails:
 * a source that ends before its declared length is an error, never a short
 * success.
 */
static fsimg_status_t
copy_data(fsimg_emit_t *emit, region_cursor_t *cursor, uint64_t logical_offset,
    uint64_t end, fsimg_error_t *error)
{
	uint64_t region_end, batch;
	size_t transferred;
	fsimg_status_t status;

	while (logical_offset < end) {
		region_seek(cursor, logical_offset);
		region_end = cursor->logical_start + cursor->region->capacity;
		batch = end - logical_offset;
		if (batch > FSIMG_EMIT_BUFFER_BYTES)
			batch = FSIMG_EMIT_BUFFER_BYTES;
		if (batch > region_end - logical_offset)
			batch = region_end - logical_offset;
		status = fsimg_source_read(emit->writer, logical_offset,
		    emit->buffer, (size_t)batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_emit_write(emit, cursor->region->image_offset +
		    (logical_offset - cursor->logical_start), emit->buffer,
		    (size_t)batch, error);
		if (status != FSIMG_OK)
			return (status);
		logical_offset += batch;
	}
	return (FSIMG_OK);
}

/* Copy logical bytes [0, initialized) of the open source into its regions,
 * walking the source's ranges in order. ZERO ranges are skipped because
 * fsimg_emit_image() zeroed the destination; DATA ranges are clipped to the
 * initialized length.
 */
static fsimg_status_t
copy_initialized(fsimg_emit_t *emit, const fsimg_emit_payload_t *payload,
    fsimg_error_t *error)
{
	region_cursor_t cursor;
	fsimg_range_t range;
	uint64_t logical_offset, range_end;
	fsimg_status_t status;

	cursor.region = payload->regions;
	cursor.logical_start = 0;
	logical_offset = 0;
	while (logical_offset < payload->initialized) {
		status = fsimg_source_range(emit->writer, logical_offset, &range,
		    error);
		if (status != FSIMG_OK)
			return (status);
		range_end = logical_offset + range.length;
		if (range_end > payload->initialized)
			range_end = payload->initialized;
		if (range.kind == FSIMG_RANGE_DATA) {
			status = copy_data(emit, &cursor, logical_offset, range_end,
			    error);
			if (status != FSIMG_OK)
				return (status);
		}
		logical_offset = range_end;
	}
	return (FSIMG_OK);
}
