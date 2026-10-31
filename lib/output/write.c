/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "io.h"
#include "publish.h"
#include "writer.h"

static fsimg_status_t create_staging(fsimg_ctx_t *, int, char *, size_t,
    int *, bool *, fsimg_error_t *);
static fsimg_status_t finish_staging(fsimg_ctx_t *, int, const char *, int,
    bool, bool, fsimg_status_t, fsimg_error_t *);
static fsimg_status_t emit_writer(void *, fsimg_io_t *, uint64_t *,
    fsimg_error_t *);

/* A fresh private staging directory beside the final name, under the next
 * unused name of this process's sequence.
 */
static fsimg_status_t
create_staging(fsimg_ctx_t *ctx, int parent, char *name, size_t capacity,
    int *out, bool *created, fsimg_error_t *error)
{
	fsimg_status_t status;

	for (unsigned attempt = 0; attempt < 128; attempt++) {
		status = fsimg_poll(ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		if (ctx->scratch_sequence == UINT64_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_PUBLISH, "private output name sequence exhausted"));
		ctx->scratch_sequence++;
		snprintf(name, capacity, ".fsimg-output-%ld-%llu", (long)getpid(),
		    (unsigned long long)ctx->scratch_sequence);
		status = fsimg_private_mkdir(ctx, parent, name, FSIMG_OP_PUBLISH,
		    out, created, error);
		if (status != FSIMG_EEXISTS)
			return (status);
	}
	return (fsimg_error_set(error, FSIMG_EIO, EEXIST, FSIMG_OP_PUBLISH,
	    "private output directory names exhausted"));
}

/* Preserve the primary publication result. Cleanup can add a retained
 * staging location and OS error, but cannot turn an operation failure into
 * success or replace its diagnostic. A successful publication followed by
 * failed cleanup is a durability failure with the final image still present.
 */
static fsimg_status_t
finish_staging(fsimg_ctx_t *ctx, int parent, const char *name, int staging,
    bool created, bool retain, fsimg_status_t status, fsimg_error_t *error)
{
	char staged[NAME_MAX + sizeof("/image")];
	int cleanup_error;

	if (retain) {
		if (staging != -1)
			fsimg_fd_release(ctx, staging);
		/* The name created a directory, so it is at most NAME_MAX. */
		(void)snprintf(staged, sizeof(staged), "%s/image", name);
		fsimg_error_path(error, staged);
		return (status);
	}
	cleanup_error = 0;
	if (staging != -1) {
		if (unlinkat(staging, "image", 0) == -1 && errno != ENOENT)
			cleanup_error = errno;
		fsimg_fd_release(ctx, staging);
	}
	if (created && unlinkat(parent, name, AT_REMOVEDIR) == -1 && cleanup_error == 0)
		cleanup_error = errno;
	/* INCOMPLETE describes a raw staged sink. The final pathname has not
	 * been published; any retained private staging is reported separately.
	 */
	if (status != FSIMG_OK && error != NULL &&
	    error->output_state == FSIMG_OUTPUT_INCOMPLETE)
		error->output_state = FSIMG_OUTPUT_ABSENT;
	if (cleanup_error == 0)
		return (status);
	if (status == FSIMG_OK) {
		status = fsimg_error_set(error, FSIMG_EDURABILITY, cleanup_error,
		    FSIMG_OP_PUBLISH, "published image staging cleanup failed");
		if (error != NULL)
			error->output_state = FSIMG_OUTPUT_PUBLISHED;
	}
	if (error != NULL)
		error->cleanup_error = cleanup_error;
	fsimg_error_path(error, name);
	return (status);
}

/* An empty sink of a kind: no descriptors and no objects. */
void
fsimg_sink_init(fsimg_sink_t *sink, fsimg_sink_kind_t kind)
{
	*sink = (fsimg_sink_t){ .size = sizeof(*sink), .kind = kind,
	    .parent_fd = -1, .stream_fd = -1, .raw_fd = -1 };
}

/* Whether an output of need takes a raw intermediate into a sink of kind. */
static bool
raw_needed(fsimg_raw_need_t need, fsimg_sink_kind_t kind)
{
	return (need == FSIMG_RAW_ALWAYS ||
	    (need == FSIMG_RAW_FOR_STREAM && kind == FSIMG_SINK_STREAM));
}

/* A writer encodes its output, and so always needs the raw intermediate,
 * when its stack names a container or its options a compression.
 */
fsimg_raw_need_t
fsimg_writer_raw_need(const fsimg_stack_t *stack, const char *compression)
{
	return ((stack->slots[FSIMG_LAYER_CONTAINER].state == FSIMG_SLOT_NAMED ||
	    compression[0] != '\0') ? FSIMG_RAW_ALWAYS : FSIMG_RAW_FOR_STREAM);
}

bool
fsimg_sink_needs_raw(fsimg_sink_kind_t kind, const fsimg_stack_t *stack,
    const char *compression)
{
	return (raw_needed(fsimg_writer_raw_need(stack, compression), kind));
}

/*
 * Take a sink. Once the sink's size shows fsimg_sink_init made it, its raw
 * intermediate descriptor never outlives the call bare: when the output
 * needs it, it is adopted into *raw as a handed-over file of ctx, which
 * must then be ready; on every other outcome, a refused sink or an output
 * that needs none, it is closed here. A sink of the wrong size is refused
 * untouched, since its descriptor fields cannot be trusted. An output that
 * never needs one may pass a NULL ctx and raw. A FILE sink must name its
 * basename, an IO sink its object and a STREAM sink its descriptor; an
 * output that needs a raw intermediate and has none, or has one it does
 * not need, is refused before anything is written.
 */
fsimg_status_t
fsimg_sink_adopt(fsimg_ctx_t *ctx, const fsimg_sink_t *sink,
    fsimg_raw_need_t need, fsimg_io_t **raw, fsimg_error_t *error)
{
	const char *invalid;
	fsimg_status_t status;
	bool needed;

	if (raw != NULL)
		*raw = NULL;
	if (sink == NULL || sink->size != sizeof(*sink))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_PUBLISH,
		    "missing or uninitialized output sink"));
	invalid = NULL;
	switch (sink->kind) {
	case FSIMG_SINK_FILE:
		if (sink->name == NULL)
			invalid = "a file sink needs a name";
		break;
	case FSIMG_SINK_IO:
		if (sink->io == NULL)
			invalid = "an I/O sink needs an object";
		break;
	case FSIMG_SINK_STREAM:
		if (sink->stream_fd < 0)
			invalid = "a stream sink needs a descriptor";
		break;
	default:
		invalid = "invalid output sink";
		break;
	}
	needed = raw_needed(need, sink->kind);
	if (invalid == NULL && needed && sink->raw_fd < 0)
		invalid = "this output needs a raw intermediate";
	if (invalid == NULL && !needed && sink->raw_fd != -1)
		invalid = "this output needs no raw intermediate";
	if (invalid != NULL)
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_PUBLISH,
		    invalid);
	else if (!needed)
		return (FSIMG_OK);
	else
		status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK) {
		if (sink->raw_fd >= 0)
			close(sink->raw_fd);
		return (status);
	}
	/* Adopted, and closed on failure, by the handed-over check. */
	return (fsimg_io_adopt_handed_over(ctx, sink->raw_fd, "raw intermediate",
	    raw, error));
}

/*
 * The host file an adopted sink writes, for alias checks: an IO sink's
 * backing, or a STREAM sink's descriptor, which a caller may point at a
 * regular file. A FILE sink writes a file created exclusively in private
 * staging, which no input can be, so it has none.
 */
fsimg_status_t
fsimg_sink_identity(const fsimg_sink_t *sink, fsimg_host_file_t *identity,
    bool *known, fsimg_error_t *error)
{
	*known = false;
	bzero(identity, sizeof(*identity));
	switch (sink->kind) {
	case FSIMG_SINK_IO:
		return (fsimg_io_identity(sink->io, identity, known, error));
	case FSIMG_SINK_STREAM:
		if (fsimg_host_file(sink->stream_fd, identity) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_PUBLISH, "stream output identity query failed"));
		*known = true;
		return (FSIMG_OK);
	default:
		return (FSIMG_OK);
	}
}

/* A writer's raw disk fills the staged file, which ends at the disk. */
static fsimg_status_t
emit_writer(void *argument, fsimg_io_t *output, uint64_t *length,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = argument;
	fsimg_status_t status;

	status = fsimg_writer_emit(writer, output, error);
	if (status == FSIMG_OK)
		*length = writer->plan->info.image.disk_bytes;
	return (status);
}

fsimg_status_t
fsimg_output_write(fsimg_writer_t *writer, int parent, const char *name,
    const fsimg_publish_ops_t *hooks, fsimg_error_t *error)
{
	return (fsimg_output_stage(writer->ctx, parent, name, emit_writer,
	    writer, hooks, error));
}

/*
 * The output coordinator's file case: a private staging directory beside
 * the final name, a staged file created exclusively there, filled by the
 * caller's operation, closed with its errors observed, and published
 * without replacement only after the fill succeeded. Every failure removes
 * what was staged, so nothing partial is ever published.
 */
fsimg_status_t
fsimg_output_stage(fsimg_ctx_t *ctx, int parent, const char *name,
    fsimg_output_fill_t fill, void *argument,
    const fsimg_publish_ops_t *hooks, fsimg_error_t *error)
{
	fsimg_io_t *output;
	uint64_t length;
	char staging_name[96];
	fsimg_status_t status;
	int staging, fd, saved;
	bool created, retain;

	output = NULL;
	staging = -1;
	created = retain = false;
	if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
	    strcmp(name, "..") == 0 || strchr(name, '/') != NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_PUBLISH, "output requires one nonempty basename"));
	status = create_staging(ctx, parent, staging_name, sizeof(staging_name),
	    &staging, &created, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		goto done;
	fd = openat(staging, "image", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW |
	    O_CLOEXEC, 0600);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_PUBLISH, "staged image creation failed");
		goto done;
	}
	status = fsimg_io_adopt_empty_output(ctx, fd, &output, error);
	if (status != FSIMG_OK)
		goto done;
	length = 0;
	status = fill(argument, output, &length, error);
	if (status != FSIMG_OK)
		goto done;
	/* Close errors are observed before publication. No descriptor can still
	 * write the staged image when the publication mechanism reopens it.
	 */
	status = fsimg_io_close_output(output, error);
	output = NULL;
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_publish(ctx, staging, "image", length, parent, name, hooks, error);
	/* A complete result remains available when no safe publication primitive
	 * is supported. Report its exact private location; never retry by replacing.
	 */
	retain = status == FSIMG_EUNSUPPORTED;

done:
	fsimg_io_free(output);
	return (finish_staging(ctx, parent, staging_name, staging, created,
	    retain, status, error));
}
