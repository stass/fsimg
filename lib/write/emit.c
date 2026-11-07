/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "emit.h"
#include "format.h"
#include "io.h"
#include "source.h"

/* The sink and buffer are borrowed from fsimg_emit_image() for one driver
 * emit call; volume_bytes is the planned volume every write must fit.
 * encoding is the running encoded copy, NULL outside one, and stored the
 * bytes its encoder has put so far.
 */
struct fsimg_emit {
	fsimg_writer_t	*writer;
	fsimg_io_t	*output;
	unsigned char	*buffer;
	uint64_t	 volume_bytes;
	const fsimg_emit_encoding_t *encoding;
	uint64_t	 stored;
};

/* Position in a file's regions. logical_start is the logical file offset of
 * the current region's first byte; image offsets come from the region itself.
 */
typedef struct region_cursor {
	const fsimg_emit_region_t *region;
	uint64_t	 logical_start;
} region_cursor_t;

static fsimg_status_t check_regions(const fsimg_emit_t *,
    const fsimg_emit_region_t *, size_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t check_payload(const fsimg_emit_t *,
    const fsimg_emit_payload_t *, fsimg_error_t *);
static fsimg_status_t stream_content(fsimg_writer_t *, uint64_t, uint64_t,
    unsigned char *, fsimg_content_consumer_t, void *, fsimg_error_t *);
static fsimg_status_t source_changed(fsimg_error_t *, const char *);
static fsimg_status_t source_begin(fsimg_writer_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t source_end(fsimg_writer_t *, uint64_t, fsimg_status_t,
    fsimg_error_t *);
static void region_seek(region_cursor_t *, uint64_t);
static fsimg_status_t copy_data(fsimg_emit_t *, const fsimg_emit_payload_t *,
    region_cursor_t *, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t copy_initialized(fsimg_emit_t *,
    const fsimg_emit_payload_t *, fsimg_error_t *);
static void name_source(fsimg_writer_t *, uint64_t, fsimg_error_t *);

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
		emit.encoding = NULL;
		emit.stored = 0;
		status = writer->format->writer->emit(writer, &emit, error);
	}
	fsimg_dealloc(buffer);
	return (status);
}

/* The buffer is lent to a running encoded copy, so its encoder gets NULL. */
unsigned char *
fsimg_emit_buffer(fsimg_emit_t *emit)
{
	return (emit->encoding != NULL ? NULL : emit->buffer);
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
	 * finish detects a changed source even when no bytes move.
	 */
	status = source_begin(writer, id, payload->length, error);
	if (status != FSIMG_OK)
		return (status);
	status = copy_initialized(emit, payload, error);
	return (source_end(writer, id, status, error));
}

/*
 * The source lifecycle the copy and the measured read share. source_begin()
 * opens entry id by stable ID and requires the source's length to equal
 * the planned length; on failure no source of its opening stays open, and
 * an open failing because another source is open leaves that one alone.
 * Only after it succeeds does the caller transfer and call source_end(),
 * which validates and closes the source after a transfer that succeeded
 * and closes it without validating after one that failed. Both name the
 * entry in a source error.
 */
static fsimg_status_t
source_begin(fsimg_writer_t *writer, uint64_t id, uint64_t length,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_source_open(writer, id, error);
	if (status == FSIMG_OK && writer->source.length != length) {
		fsimg_source_close(writer);
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned content length differs from its source");
	}
	if (status != FSIMG_OK)
		name_source(writer, id, error);
	return (status);
}

static fsimg_status_t
source_end(fsimg_writer_t *writer, uint64_t id, fsimg_status_t status,
    fsimg_error_t *error)
{
	if (status == FSIMG_OK)
		status = fsimg_source_finish(writer, error);
	else
		fsimg_source_close(writer);
	if (status != FSIMG_OK)
		name_source(writer, id, error);
	return (status);
}

/*
 * The encoded copy. The regions are checked as a payload copy's are, and
 * must hold the planned stored length, before the source is opened. While
 * the encoder runs, emit->encoding routes its fsimg_emit_put() calls into
 * those regions; it is cleared on every outcome, so a put outside the copy
 * is refused.
 */
fsimg_status_t
fsimg_emit_encoded_copy(fsimg_emit_t *emit, uint64_t id,
    const fsimg_emit_encoding_t *encoding, fsimg_error_t *error)
{
	uint64_t capacity;
	fsimg_status_t status;

	if (emit->encoding != NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "encoded copies cannot nest"));
	if (encoding->encode == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "encoded copy has no encoder"));
	status = check_regions(emit, encoding->regions,
	    encoding->region_count, &capacity, error);
	if (status != FSIMG_OK)
		return (status);
	if (encoding->stored > capacity)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned encoding exceeds its allocation"));

	emit->encoding = encoding;
	emit->stored = 0;
	status = stream_content(emit->writer, id, encoding->length,
	    emit->buffer, encoding->encode, encoding->encoder_argument, error);
	if (status == FSIMG_OK && emit->stored != encoding->stored) {
		status = source_changed(error,
		    "encoded content is shorter than planned");
		name_source(emit->writer, id, error);
	}
	emit->encoding = NULL;
	return (status);
}

/*
 * Write the encoder's next bytes at logical offset emit->stored of the
 * running copy's regions, which they may cross, and show them to its
 * observer. Bytes beyond the planned stored length are a source change:
 * the plan measured this content with the same encoder.
 */
fsimg_status_t
fsimg_emit_put(fsimg_emit_t *emit, const void *bytes, size_t length,
    fsimg_error_t *error)
{
	const fsimg_emit_encoding_t *encoding = emit->encoding;
	const unsigned char *next = bytes;
	region_cursor_t cursor;
	uint64_t image_offset, batch, region_end;
	fsimg_status_t status;

	if (encoding == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "encoded output outside an encoded copy"));
	if (length > encoding->stored - emit->stored)
		return (source_changed(error,
		    "encoded content outgrew its plan"));
	cursor.region = encoding->regions;
	cursor.logical_start = 0;
	while (length != 0) {
		region_seek(&cursor, emit->stored);
		region_end = cursor.logical_start + cursor.region->capacity;
		batch = region_end - emit->stored;
		if (batch > length)
			batch = length;
		image_offset = cursor.region->image_offset +
		    (emit->stored - cursor.logical_start);
		status = fsimg_emit_write(emit, image_offset, next,
		    (size_t)batch, error);
		if (status != FSIMG_OK)
			return (status);
		if (encoding->observe != NULL)
			encoding->observe(encoding->observer_argument, next,
			    (size_t)batch, image_offset);
		emit->stored += batch;
		next += batch;
		length -= (size_t)batch;
	}
	return (FSIMG_OK);
}

/*
 * The measured read, with a batch buffer of its own: planning has no
 * emission object, and during emission the driver may hold the emission
 * buffer.
 */
fsimg_status_t
fsimg_writer_measure(fsimg_writer_t *writer, uint64_t id, uint64_t length,
    fsimg_content_consumer_t consumer, void *argument, fsimg_error_t *error)
{
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	buffer = fsimg_alloc(writer->ctx, FSIMG_EMIT_BUFFER_BYTES, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = stream_content(writer, id, length, buffer, consumer, argument,
	    error);
	fsimg_dealloc(buffer);
	return (status);
}

/*
 * Stream logical bytes [0, length) of entry id to consumer through the
 * copy's source lifecycle (source_begin(), source_end()): the ranges
 * walked in order with DATA read exactly into buffer and ZERO filled with
 * zeros, one charged work unit per zero batch since no read charges it,
 * then the end call.
 */
static fsimg_status_t
stream_content(fsimg_writer_t *writer, uint64_t id, uint64_t length,
    unsigned char *buffer, fsimg_content_consumer_t consumer, void *argument,
    fsimg_error_t *error)
{
	fsimg_range_t range;
	uint64_t offset, range_end;
	size_t batch, transferred;
	fsimg_status_t status;

	status = source_begin(writer, id, length, error);
	if (status != FSIMG_OK)
		return (status);
	offset = 0;
	while (status == FSIMG_OK && offset < length) {
		status = fsimg_source_range(writer, offset, &range, error);
		if (status != FSIMG_OK)
			break;
		range_end = offset + range.length;
		if (range_end > length)
			range_end = length;
		while (status == FSIMG_OK && offset < range_end) {
			batch = range_end - offset > FSIMG_EMIT_BUFFER_BYTES ?
			    FSIMG_EMIT_BUFFER_BYTES : (size_t)(range_end - offset);
			if (range.kind == FSIMG_RANGE_DATA)
				status = fsimg_source_read(writer, offset, buffer,
				    batch, &transferred, error);
			else {
				bzero(buffer, batch);
				status = fsimg_session_charge(&writer->session, 0,
				    0, 1, error);
			}
			if (status == FSIMG_OK)
				status = consumer(argument, buffer, batch, error);
			offset += batch;
		}
	}
	if (status == FSIMG_OK)
		status = consumer(argument, NULL, 0, error);
	return (source_end(writer, id, status, error));
}

/* A difference between encoded output and its plan, reported against the
 * source so that the copy names the changed file.
 */
static fsimg_status_t
source_changed(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
	    FSIMG_OP_SOURCE, message));
}

/* The whole target of link id, read through the one source lifecycle the
 * copy uses: open by stable ID, which reads and checks the target and, for
 * a host link, its recorded identity; one exact read of the declared
 * length, charged to the session; and finish, which validates again. The
 * source is closed on every outcome, so nothing is held between planning
 * and emission, and a host link changed in between fails as a changed
 * source when it is opened again.
 */
fsimg_status_t
fsimg_writer_link_target(fsimg_writer_t *writer, uint64_t id,
    unsigned char *target, size_t *length, fsimg_error_t *error)
{
	size_t declared, transferred;
	fsimg_status_t status;

	*length = 0;
	status = fsimg_source_open(writer, id, error);
	if (status != FSIMG_OK)
		goto fail;
	/* Only a link's source holds its target. */
	if (!writer->source.link) {
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "link-target read requires a symbolic link");
		goto fail;
	}
	if (writer->source.length > FSIMG_SYMLINK_TARGET_MAX) {
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "link target exceeds its buffer");
		goto fail;
	}
	declared = (size_t)writer->source.length;
	status = fsimg_source_read(writer, 0, target, declared, &transferred,
	    error);
	if (status == FSIMG_OK && transferred != declared)
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "link target read ended early");
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_source_finish(writer, error);
	if (status != FSIMG_OK) {
		name_source(writer, id, error);
		return (status);
	}
	*length = declared;
	return (FSIMG_OK);
fail:
	fsimg_source_close(writer);
	name_source(writer, id, error);
	return (status);
}

/* Name the entry whose source failed, so that a changed or mismatching
 * file is reported by its image path. Only source failures are named: an
 * output write failing during the copy concerns the image, not the file.
 * The lookup replaces the catalog's current snapshot, which the failing
 * driver no longer reads, and a lookup failure leaves the error as it is.
 */
static void
name_source(fsimg_writer_t *writer, uint64_t id, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *entry;
	fsimg_error_t ignored;

	if (error == NULL || error->operation != FSIMG_OP_SOURCE)
		return;
	if (fsimg_catalog_get(writer, id, &entry, &ignored) == FSIMG_OK)
		fsimg_error_path(error, entry->entry.path);
}

/* Check that every region lies within the planned volume and total their
 * capacities, which therefore cannot overflow.
 */
static fsimg_status_t
check_regions(const fsimg_emit_t *emit, const fsimg_emit_region_t *regions,
    size_t count, uint64_t *capacity, fsimg_error_t *error)
{
	*capacity = 0;
	if (count != 0 && regions == NULL)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "planned regions are missing"));
	for (size_t i = 0; i < count; i++) {
		if (!fsimg_range_fits(regions[i].image_offset,
		    regions[i].capacity, emit->volume_bytes))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "planned region exceeds the volume"));
		if (!fsimg_add_u64(*capacity, regions[i].capacity, capacity))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "planned region capacities overflow"));
	}
	return (FSIMG_OK);
}

/* Validate a payload placement before any source is touched: each region lies
 * within the planned volume, and initialized <= length <= total capacity.
 * length <= capacity is what keeps region_seek() inside the regions array.
 */
static fsimg_status_t
check_payload(const fsimg_emit_t *emit, const fsimg_emit_payload_t *payload,
    fsimg_error_t *error)
{
	uint64_t capacity;
	fsimg_status_t status;

	status = check_regions(emit, payload->regions, payload->region_count,
	    &capacity, error);
	if (status != FSIMG_OK)
		return (status);
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
 * success. The payload's observer sees each batch once it is written.
 */
static fsimg_status_t
copy_data(fsimg_emit_t *emit, const fsimg_emit_payload_t *payload,
    region_cursor_t *cursor, uint64_t logical_offset, uint64_t end,
    fsimg_error_t *error)
{
	uint64_t region_end, batch, image_offset;
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
		image_offset = cursor->region->image_offset +
		    (logical_offset - cursor->logical_start);
		status = fsimg_emit_write(emit, image_offset, emit->buffer,
		    (size_t)batch, error);
		if (status != FSIMG_OK)
			return (status);
		if (payload->observe != NULL)
			payload->observe(payload->observer_argument, emit->buffer,
			    (size_t)batch, image_offset);
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
			status = copy_data(emit, payload, &cursor, logical_offset,
			    range_end, error);
			if (status != FSIMG_OK)
				return (status);
		}
		logical_offset = range_end;
	}
	return (FSIMG_OK);
}
