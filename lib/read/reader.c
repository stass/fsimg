/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"
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
	fsimg_fat_close(reader);
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
	return (fsimg_fat_match_name(reader, component, matches, error));
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
	return (fsimg_fat_skip_directory(reader, error));
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
	if (options->format != NULL && strcmp(options->format, "fat12") != 0 &&
	    strcmp(options->format, "fat16") != 0 &&
	    strcmp(options->format, "fat32") != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported reader format"));
	reader = fsimg_alloc(ctx, sizeof(*reader), &detail);
	if (reader == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	reader->ctx = ctx;
	reader->input = input;
	fsimg_session_init(&reader->session, ctx, "FAT reading", 0);
	status = fsimg_io_identity(input, &reader->identity,
	    &reader->host_identity, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_fat_open(reader, options->format, error);
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
	fsimg_fat_close(reader);
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
		status = fsimg_fat_next(reader, error);
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
	status = fsimg_fat_read(reader, offset, buffer, length, transferred, error);
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
	if (offset == reader->entry.size)
		return (FSIMG_DONE);
	status = fsimg_fat_range(reader, offset, out, error);
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
	status = fsimg_fat_verify(reader, flags, out, error);
	bzero(&reader->entry, sizeof(reader->entry));
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	reader->state = FSIMG_READER_DONE;
	out->complete = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_format_next(size_t *cursor, fsimg_format_info_t *out, fsimg_error_t *error)
{
	static const char *const names[] = { "fat12", "fat16", "fat32" };
	static const uint64_t max_clusters[] = { 4084, 65524, 0x0ffffff5 };
	uint64_t sectors;

	fsimg_error_clear(error);
	if (cursor == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "missing format cursor or output"));
	bzero(out, sizeof(*out));
	if (*cursor >= sizeof(names) / sizeof(names[0]))
		return (FSIMG_DONE);
	strcpy(out->name, names[*cursor]);
	strcpy(out->profile, "unicode-17-simple-upper/cp437");
	out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY;
	out->entry_kinds = (1u << FSIMG_ENTRY_FILE) | (1u << FSIMG_ENTRY_DIRECTORY);
	out->metadata_fields = 0x0f;
	out->sector_size_mask = (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12);
	out->allocation_unit_size_mask = ((1u << 20) - 1) & ~((1u << 9) - 1);
	out->write_allocation_unit_size_mask = ((1u << 16) - 1) & ~((1u << 9) - 1);
	/* File size is bounded by both its field and usable cluster identifiers.
	 * Volume size additionally includes reserved/FAT/root regions and a tail
	 * shorter than one cluster. These are independent supported upper bounds.
	 */
	out->max_file_bytes = *cursor == 0 ? UINT64_C(4084) * 512 * 1024 : UINT32_MAX;
	if (*cursor < 2) {
		sectors = UINT64_C(65535) * 3 + 512 +
		    (max_clusters[*cursor] + 1) * 128 - 1;
		out->max_volume_bytes = sectors * 4096;
	} else {
		out->max_volume_bytes = (uint64_t)UINT32_MAX * 4096;
	}
	out->max_name_utf16_units = 255;
	(*cursor)++;
	return (FSIMG_OK);
}
