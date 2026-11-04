/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "reader.h"
#include "settings.h"

static fsimg_status_t reader_ready(const fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t current_file(fsimg_reader_t *, uint64_t, fsimg_error_t *);

void
fsimg_read_options_init(fsimg_read_options_t *options)
{
	const fsimg_setting_targets_t targets = { .read = options };

	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->fat_codepage = 437;
	fsimg_settings_init(&fsimg_filesystem_settings, &targets,
	    FSIMG_RECORD_READ);
}

static fsimg_status_t
reader_ready(const fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(reader == NULL ? NULL : reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state == FSIMG_READER_FAILED)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "reader has failed"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_abort(fsimg_reader_t *reader, fsimg_status_t status)
{
	reader->state = FSIMG_READER_FAILED;
	bzero(&reader->entry, sizeof(reader->entry));
	fsimg_walk_free(reader);
	reader->ops->close(reader);
	fsimg_dealloc(reader->link_target);
	reader->link_target = NULL;
	reader->link_length = 0;
	return (status);
}

fsimg_status_t
fsimg_reader_end_scope(fsimg_reader_t *reader)
{
	reader->state = FSIMG_READER_DONE;
	bzero(&reader->entry, sizeof(reader->entry));
	return (FSIMG_DONE);
}

fsimg_status_t
fsimg_reader_match_name(fsimg_reader_t *reader, const char *component, bool *matches,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state != FSIMG_READER_ITERATING || reader->entry.path == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "name matching requires a current entry"));
	/* Only an unrepresentable query leaves the reader usable; a failure to
	 * derive the current entry's keys is a driver failure, and terminal.
	 */
	status = fsimg_walk_match(reader, component, matches, error);
	if (status != FSIMG_OK && status != FSIMG_EINVAL)
		return (fsimg_reader_abort(reader, status));
	return (status);
}

fsimg_status_t
fsimg_reader_skip_directory(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state != FSIMG_READER_ITERATING ||
	    reader->entry.kind != FSIMG_ENTRY_DIRECTORY)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "subtree pruning requires a current directory"));
	return (fsimg_walk_skip(reader, error));
}

fsimg_status_t
fsimg_reader_stable(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_host_file_t identity;
	fsimg_status_t status;
	bool known;

	if (!reader->host_identity)
		return (FSIMG_OK);
	status = fsimg_io_identity(reader->input, &identity, &known, error);
	if (status != FSIMG_OK)
		return (status);
	if (!known || identity.kind != FSIMG_HOST_FILE ||
	    identity.device != reader->identity.device ||
	    identity.inode != reader->identity.inode ||
	    identity.length != reader->identity.length ||
	    identity.modified.seconds != reader->identity.modified.seconds ||
	    identity.modified.nanoseconds != reader->identity.modified.nanoseconds ||
	    identity.changed.seconds != reader->identity.changed.seconds ||
	    identity.changed.nanoseconds != reader->identity.changed.nanoseconds)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_READER, "image storage changed during reading"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_set_link_target(fsimg_reader_t *reader, const void *bytes,
    size_t length, fsimg_error_t *error)
{
	fsimg_error_t detail;

	if (reader->entry.kind != FSIMG_ENTRY_SYMLINK || bytes == NULL ||
	    length == 0 || length > FSIMG_SYMLINK_TARGET_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "a decoded target needs a symbolic link of 1 to 1024 bytes"));
	if (reader->link_target == NULL) {
		reader->link_target = fsimg_alloc(reader->ctx,
		    FSIMG_SYMLINK_TARGET_MAX, &detail);
		if (reader->link_target == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	bcopy(bytes, reader->link_target, length);
	reader->link_length = length;
	reader->entry.size = length;
	reader->initialized = length;
	return (FSIMG_OK);
}

/*
 * The one read path of every driver, so the one place a volume longer than its
 * slice is told apart: whatever a driver asks for past the slice is a truncated
 * image, and no driver checks its own reads against the tail. The identity
 * check comes first, so a host file that shrank after opening reports the
 * change.
 */
fsimg_status_t
fsimg_reader_bytes(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	uint64_t slice;
	fsimg_status_t status;

	*transferred = 0;
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_get_size(reader->input, &slice, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_range_fits(offset, length, slice))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
		    "image is truncated"));
	status = fsimg_io_read_session(reader->input, &reader->session, offset,
	    buffer, length, transferred, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_reader_stable(reader, error));
}

fsimg_status_t
fsimg_reader_open(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const fsimg_read_options_t *options, fsimg_reader_t **out,
    fsimg_error_t *error)
{
	fsimg_read_options_t defaults;
	fsimg_error_t detail;
	fsimg_reader_t *reader;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || input == NULL || fsimg_io_context(input) != ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "reader needs input and output in its context"));
	if (options == NULL) {
		fsimg_read_options_init(&defaults);
		options = &defaults;
	}
	if (options->size != sizeof(*options) ||
	    options->version != FSIMG_OPTIONS_VERSION ||
	    (options->flags & ~FSIMG_READ_ASSUME_IDENTITY) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "invalid read options size, version or flags"));
	/* Only a caller naming the format can vouch for its identity. */
	if ((options->flags & FSIMG_READ_ASSUME_IDENTITY) != 0 &&
	    options->format == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "assumed identity requires a named format"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "nonzero reserved read option"));
	}
	if (options->fat_codepage != 0 && options->fat_codepage != 437)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "FAT short names require CP437"));
	/* Each setting is well formed now; whether a non-default one applies
	 * waits for the format detection chooses.
	 */
	status = fsimg_read_settings_check(options, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	reader = fsimg_alloc(ctx, sizeof(*reader), &detail);
	if (reader == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	reader->ctx = ctx;
	reader->input = input;
	reader->options = *options;
	reader->options.format = NULL;
	fsimg_session_init(&reader->session, ctx, "image reading", 0);
	status = fsimg_io_identity(input, &reader->identity,
	    &reader->host_identity, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_io_get_size(input, &reader->slice_bytes, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_format_open(reader, options->format, error);
	if (status != FSIMG_OK)
		goto fail;
	/* Declared filesystem geometry can exceed the available input. The
	 * selected I/O view funds the default; an explicit context limit was
	 * already installed by session initialization. Drivers report what the
	 * volume declares, and only here is the declared size compared with
	 * the slice.
	 */
	if (ctx->options.max_output_bytes == 0)
		reader->session.output_limit = reader->slice_bytes;
	reader->info.missing_bytes =
	    reader->info.volume_bytes > reader->slice_bytes ?
	    reader->info.volume_bytes - reader->slice_bytes : 0;
	reader->path = fsimg_alloc(ctx, ctx->options.max_path_bytes, &detail);
	if (reader->path == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto fail;
	}
	*out = reader;
	return (FSIMG_OK);
fail:
	fsimg_reader_free(reader);
	return (status);
}

void
fsimg_reader_free(fsimg_reader_t *reader)
{
	if (reader == NULL || reader->ctx->in_callback)
		return;
	fsimg_walk_free(reader);
	if (reader->ops != NULL)
		reader->ops->close(reader);
	fsimg_dealloc(reader->link_target);
	fsimg_dealloc(reader->path);
	fsimg_dealloc(reader);
}

fsimg_status_t
fsimg_reader_get_info(const fsimg_reader_t *reader, fsimg_volume_info_t *out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing volume information output"));
	*out = reader->info;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_next(fsimg_reader_t *reader, const fsimg_entry_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing entry output"));
	if (reader->state == FSIMG_READER_DONE)
		return (FSIMG_DONE);
	bzero(&reader->entry, sizeof(reader->entry));
	reader->state = FSIMG_READER_ITERATING;
	status = fsimg_reader_stable(reader, error);
	/* Iteration delivers only entries whose names and ancestors are all
	 * delivered; the walker counts the omitted ones it passes.
	 */
	while (status == FSIMG_OK) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_OK && fsimg_walk_delivered(reader))
			break;
	}
	if (status == FSIMG_DONE) {
		return (fsimg_reader_end_scope(reader));
	}
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	*out = &reader->entry;
	return (FSIMG_OK);
}

static fsimg_status_t
current_file(fsimg_reader_t *reader, uint64_t offset, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	/* A symbolic link's content is its target, delivered like file bytes. */
	if (reader->entry.path == NULL || (reader->entry.kind != FSIMG_ENTRY_FILE &&
	    reader->entry.kind != FSIMG_ENTRY_SYMLINK))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "content requires a current file or symbolic link"));
	if (offset > reader->entry.size)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "content offset exceeds file length"));
	return (FSIMG_OK);
}

/* Deliver up to length bytes of the current file at offset, clipped to the
 * file length; an offset at or past it delivers nothing. Bytes below the
 * initialized length are read from the extents the driver resolves, one
 * exact physical read per piece; the rest are synthesized zeros and never
 * touch storage. A decoded link target is served as [0, size) of the
 * reader's target buffer instead, after the stability check and one unit
 * of work that a stored read would cost. transferred counts only confirmed
 * bytes, so a failure in a later extent still reports the pieces before
 * it; stored counts those among them read from the image, which excludes
 * synthesized zeros and decoded targets.
 */
fsimg_status_t
fsimg_reader_content(fsimg_reader_t *reader, uint64_t offset, void *out,
    size_t length, size_t *transferred, size_t *stored, fsimg_error_t *error)
{
	const fsimg_extent_domain_t *domain = &reader->content_domain;
	unsigned char *buffer = out;
	fsimg_extent_t extent;
	uint64_t initialized, within, image;
	size_t piece, count;
	fsimg_status_t status;

	initialized = reader->initialized;
	*transferred = 0;
	*stored = 0;
	/* Decoded content belongs to a symbolic link whose size the driver
	 * left at the decoded length; anything else is a driver defect.
	 */
	if (reader->link_length != 0 &&
	    (reader->entry.kind != FSIMG_ENTRY_SYMLINK ||
	    reader->entry.size != reader->link_length ||
	    initialized != reader->link_length))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "decoded link target does not match the current entry"));
	if (offset >= reader->entry.size)
		return (FSIMG_OK);
	if (length > reader->entry.size - offset)
		length = (size_t)(reader->entry.size - offset);
	if (reader->link_length != 0) {
		status = fsimg_reader_stable(reader, error);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&reader->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (length != 0)
			bcopy(reader->link_target + offset, buffer, length);
		*transferred = length;
		return (FSIMG_OK);
	}
	while (*transferred < length) {
		piece = length - *transferred;
		if (offset >= initialized) {
			bzero(buffer + *transferred, piece);
			*transferred = length;
			break;
		}
		bzero(&extent, sizeof(extent));
		status = reader->ops->resolve(reader, offset, &extent, error);
		if (status != FSIMG_OK)
			return (status);
		/* The extent must contain offset; clip it to the request and to
		 * the initialized length, so slack after either is never read.
		 * The piece must lie in the content domain whatever the driver
		 * resolved, so no driver can deliver metadata as content.
		 */
		if (offset < extent.logical_offset ||
		    offset - extent.logical_offset >= extent.length)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_READER, "resolved extent does not contain the offset"));
		within = offset - extent.logical_offset;
		if (piece > extent.length - within)
			piece = (size_t)(extent.length - within);
		if (piece > initialized - offset)
			piece = (size_t)(initialized - offset);
		if (extent.image_offset > UINT64_MAX - within ||
		    extent.image_offset + within < domain->offset ||
		    !fsimg_range_fits(extent.image_offset + within - domain->offset,
		    piece, domain->length))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_READER, "content extent outside the file storage domain"));
		image = extent.image_offset + within;
		status = fsimg_reader_bytes(reader, image,
		    buffer + *transferred, piece, &count, error);
		*transferred += count;
		*stored += count;
		offset += count;
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Charge a file's declared size to the output budget before it is copied. The
 * default budget is the slice, so on a truncated volume a lost file commonly
 * shows first as an overrun. One read of its last stored byte goes through the
 * funnel and tells the two apart: a file past the end is corrupt, while one whose
 * last stored byte is there keeps the limit error, since its extents are shared
 * or the limit is the caller's. The bytes after the initialized length are zeros
 * no read of the image supplies, so they prove nothing and a file storing none
 * keeps the limit. Any other failure of that read outranks the limit. Either way
 * the copy does not start.
 */
fsimg_status_t
fsimg_reader_charge_file(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_error_t detail;
	unsigned char last;
	size_t count;
	fsimg_status_t probe, status;

	status = fsimg_session_charge(&reader->session, 0, reader->entry.size, 1,
	    error);
	if (status != FSIMG_ELIMIT || reader->info.missing_bytes == 0 ||
	    reader->initialized == 0)
		return (status);
	probe = fsimg_reader_read_at(reader, reader->initialized - 1, &last, 1,
	    &count, &detail);
	if (probe == FSIMG_OK || probe == FSIMG_ELIMIT)
		return (status);
	if (error != NULL)
		*error = detail;
	return (probe);
}

fsimg_status_t
fsimg_reader_read_at(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	size_t batch, count, stored;
	fsimg_status_t status;

	if (transferred != NULL)
		*transferred = 0;
	status = current_file(reader, offset, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred == NULL || (length != 0 && buffer == NULL))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing content buffer or count output"));
	if (length > reader->entry.size - offset)
		length = (size_t)(reader->entry.size - offset);
	/* Even synthesized zero bytes depend on stable input, and every batch,
	 * whether resolved from a cached extent or synthesized, is charged and
	 * polled for cancellation by the session.
	 */
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	do {
		batch = length - *transferred;
		if (batch > 65536)
			batch = 65536;
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (fsimg_reader_abort(reader, status));
		count = 0;
		status = fsimg_reader_content(reader, offset, buffer == NULL ? NULL :
		    (unsigned char *)buffer + *transferred, batch, &count, &stored,
		    error);
		*transferred += count;
		offset += count;
		if (status != FSIMG_OK)
			return (fsimg_reader_abort(reader, status));
	} while (*transferred < length);
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	return (FSIMG_OK);
}

/* A range answers from lengths alone: DATA up to the initialized length,
 * ZERO after it. Physical fragmentation never divides a range, and a query
 * resolves no storage, so a range followed by reads of it walks the
 * allocation once, during the reads.
 */
fsimg_status_t
fsimg_reader_next_range(fsimg_reader_t *reader, uint64_t offset,
    fsimg_range_t *out, fsimg_error_t *error)
{
	uint64_t initialized;
	fsimg_status_t status;

	status = current_file(reader, offset, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing content range output"));
	bzero(out, sizeof(*out));
	status = fsimg_reader_stable(reader, error);
	if (status == FSIMG_OK)
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	if (offset == reader->entry.size)
		return (FSIMG_DONE);
	initialized = reader->initialized;
	out->offset = offset;
	if (offset < initialized) {
		out->kind = FSIMG_RANGE_DATA;
		out->length = initialized - offset;
	} else {
		out->kind = FSIMG_RANGE_ZERO;
		out->length = reader->entry.size - offset;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_verify_account(fsimg_reader_t *reader, fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	if (reader->omission.uncounted || !fsimg_walk_delivered(reader))
		return (FSIMG_OK);
	if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY) {
		report->directories++;
		return (FSIMG_OK);
	}
	report->files++;
	if (!fsimg_add_u64(report->logical_bytes, reader->entry.size,
	    &report->logical_bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_VERIFY,
		    "verification logical-byte total overflow"));
	return (FSIMG_OK);
}

/* The batch loop of payload verification. Each batch is a bounded delivery
 * request, which resolves and checks extents and polls and charges each I/O
 * call; the loop adds no charge of its own. Delivery completes a batch below
 * entry.size or fails, so a short success is a broken contract, not a retry.
 * The report counter saturates and counts the bytes delivery read from
 * the image; progress uses the batch's transferred count, which includes
 * a decoded link target's bytes.
 */
fsimg_status_t
fsimg_verify_payload(fsimg_reader_t *reader, unsigned char *buffer,
    size_t capacity, fsimg_verify_report_t *report, fsimg_error_t *error)
{
	uint64_t offset, remaining;
	size_t batch, count, stored;
	fsimg_status_t status;

	if (reader == NULL || buffer == NULL || capacity == 0 || report == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_VERIFY,
		    "invalid payload verification argument"));
	if (reader->initialized > reader->entry.size)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_VERIFY,
		    "initialized length exceeds the entry size"));

	for (offset = 0; offset < reader->initialized; offset += count) {
		remaining = reader->initialized - offset;
		batch = remaining > capacity ? capacity : (size_t)remaining;
		count = 0;
		status = fsimg_reader_content(reader, offset, buffer, batch,
		    &count, &stored, error);
		fsimg_stat_add(&report->payload_bytes_read, stored);
		if (status != FSIMG_OK)
			return (status);
		if (count != batch)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_VERIFY, "content delivery ended a batch early"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_identity(fsimg_reader_t *reader, bool holds, const char *refusal,
    const char *phrase, fsimg_error_t *error)
{
	if (holds)
		return (FSIMG_OK);
	if ((reader->options.flags & FSIMG_READ_ASSUME_IDENTITY) == 0)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, refusal));
	if (reader->assumed_identity == NULL)
		reader->assumed_identity = phrase;
	return (FSIMG_OK);
}

/* Full verification certifies the volume as its format, which an assumed
 * identity is not: the volume fails before any work, the reader untouched,
 * naming the identity check the open skipped.
 */
static fsimg_status_t
assumed_identity_failed(const fsimg_reader_t *reader, fsimg_error_t *error)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "%s identity was assumed: %s",
	    reader->info.format, reader->assumed_identity);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_VERIFY,
	    message));
}

/* A volume missing part of itself is not a verified volume, whatever
 * references the tail: it fails before any work, the reader untouched, and
 * no driver needs a truncation check of its own.
 */
static fsimg_status_t
truncated_failed(const fsimg_reader_t *reader, fsimg_error_t *error)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message),
	    "image is truncated: %llu declared bytes are missing",
	    (unsigned long long)reader->info.missing_bytes);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_VERIFY,
	    message));
}

fsimg_status_t
fsimg_verify(fsimg_reader_t *reader, unsigned flags, fsimg_verify_report_t *out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		bzero(out, sizeof(*out));
	status = reader_ready(reader, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || (flags & ~FSIMG_VERIFY_PAYLOAD) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_VERIFY, "invalid verification output or flags"));
	if (reader->state != FSIMG_READER_NEW)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_VERIFY, "verification requires an untouched reader"));
	/* A driver without a verifier advertises no verify capability; the
	 * request fails before any work, so the reader stays untouched and the
	 * report stays incomplete.
	 */
	if (reader->ops->verify == NULL)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_VERIFY, "format has no full verification"));
	if (reader->info.missing_bytes != 0)
		return (truncated_failed(reader, error));
	if (reader->assumed_identity != NULL)
		return (assumed_identity_failed(reader, error));
	out->scope = (flags & FSIMG_VERIFY_PAYLOAD) != 0 ?
	    FSIMG_VALIDATION_PAYLOAD : FSIMG_VALIDATION_STRUCTURAL;
	/* Full verification checks what delivery omits, so the walker
	 * descends into omitted directories too.
	 */
	reader->omission.descend = true;
	reader->state = FSIMG_READER_ITERATING;
	status = reader->ops->verify(reader, flags, out, error);
	bzero(&reader->entry, sizeof(reader->entry));
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	reader->state = FSIMG_READER_DONE;
	out->omitted_entries = reader->omission.count;
	out->complete = true;
	return (FSIMG_OK);
}
