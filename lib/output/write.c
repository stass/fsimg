/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
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

static fsimg_status_t
create_staging(fsimg_ctx_t *ctx, int parent, char *name, size_t capacity,
    int *out, bool *created, fsimg_error_t *error)
{
	fsimg_status_t status;
	int fd, saved;

	*out = -1;
	*created = false;
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
		if (mkdirat(parent, name, 0700) == 0) {
			*created = true;
			break;
		}
		if (errno != EEXIST || attempt == 127)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_PUBLISH, "private output directory creation failed"));
	}
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_PUBLISH, "private output directory open failed"));
	}
	*out = fd;
	return (FSIMG_OK);
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
	int cleanup_error;

	if (retain) {
		if (staging != -1)
			fsimg_fd_release(ctx, staging);
		if (error != NULL)
			snprintf(error->path, sizeof(error->path), "%s/image", name);
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
	if (error != NULL) {
		error->cleanup_error = cleanup_error;
		snprintf(error->path, sizeof(error->path), "%s", name);
	}
	return (status);
}

fsimg_status_t
fsimg_output_write(fsimg_writer_t *writer, int parent, const char *name,
    uint64_t length, const fsimg_publish_ops_t *hooks, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx;
	fsimg_io_t *output;
	char staging_name[96];
	fsimg_status_t status;
	int staging, fd, saved;
	bool created, retain;

	ctx = writer->ctx;
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
	status = fsimg_writer_emit(writer, output, error);
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
