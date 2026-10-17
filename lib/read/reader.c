/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "format.h"
#include "reader.h"

static fsimg_status_t reader_ready(const fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t current_file(fsimg_reader_t *, uint64_t, fsimg_error_t *);

void
fsimg_read_options_init(fsimg_read_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->fat_codepage = 437;
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
	reader->ops->close(reader);
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
	return (reader->ops->match_name(reader, component, matches, error));
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
	return (reader->ops->skip_directory(reader, error));
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
fsimg_reader_bytes(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	fsimg_status_t status;

	*transferred = 0;
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (status);
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
	    options->version != FSIMG_OPTIONS_VERSION || options->flags != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "invalid read options size, version or flags"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "nonzero reserved read option"));
	}
	if (options->fat_codepage != 0 && options->fat_codepage != 437)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "FAT short names require CP437"));
	reader = fsimg_alloc(ctx, sizeof(*reader), &detail);
	if (reader == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	reader->ctx = ctx;
	reader->input = input;
	fsimg_session_init(&reader->session, ctx, "image reading", 0);
	status = fsimg_io_identity(input, &reader->identity,
	    &reader->host_identity, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_format_open(reader, options->format, error);
	if (status != FSIMG_OK)
		goto fail;
	reader->session.output_limit = ctx->options.max_output_bytes == 0 ?
	    reader->info.volume_bytes : ctx->options.max_output_bytes;
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
	if (reader->ops != NULL)
		reader->ops->close(reader);
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
	if (status == FSIMG_OK)
		status = reader->ops->next(reader, error);
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
	if (reader->entry.path == NULL || reader->entry.kind != FSIMG_ENTRY_FILE)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "content requires a current regular file"));
	if (offset > reader->entry.size)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "content offset exceeds file length"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_read_at(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	size_t batch, count;
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
	/* Even synthesized zero bytes depend on stable input. Common content
	 * dispatch owns these checks so cached and zero-producing drivers cannot
	 * bypass source consistency, cancellation or the session work budget.
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
		status = reader->ops->read(reader, offset,
		    buffer == NULL ? NULL : (unsigned char *)buffer + *transferred,
		    batch, &count, error);
		*transferred += count;
		offset += count;
		if (status != FSIMG_OK)
			return (fsimg_reader_abort(reader, status));
		if (count != batch) {
			status = fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_READER, "driver returned incomplete successful content");
			return (fsimg_reader_abort(reader, status));
		}
	} while (*transferred < length);
	status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_reader_next_range(fsimg_reader_t *reader, uint64_t offset,
    fsimg_range_t *out, fsimg_error_t *error)
{
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
	status = reader->ops->range(reader, offset, out, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	return (FSIMG_OK);
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
	out->scope = (flags & FSIMG_VERIFY_PAYLOAD) != 0 ?
	    FSIMG_VALIDATION_PAYLOAD : FSIMG_VALIDATION_STRUCTURAL;
	reader->state = FSIMG_READER_ITERATING;
	status = reader->ops->verify(reader, flags, out, error);
	bzero(&reader->entry, sizeof(reader->entry));
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	reader->state = FSIMG_READER_DONE;
	out->complete = true;
	return (FSIMG_OK);
}
