/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_EXAMPLE_H
#define FSIMG_EXAMPLE_H

/* Standalone examples need POSIX host operations and BSD memory primitives;
 * they use no private project header or generated configuration header.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <fsimg.h>

static int
example_failure(const fsimg_error_t *error)
{
	const unsigned char *byte;

	/* Error paths may contain image names. This example reports structured
	 * fields without sending those names unescaped to a terminal.
	 */
	fprintf(stderr, "fsimg: %s; operation=%d resource=%d output=%d errno=%d cleanup=%d\n",
	    fsimg_status_string(error->status), error->operation, error->resource,
	    error->output_state, error->os_error, error->cleanup_error);
	if (error->path[0] != '\0') {
		fprintf(stderr, "image or retained-output path=\"");
		for (byte = (const unsigned char *)error->path; *byte != '\0'; byte++) {
			if (*byte < 32 || *byte >= 127 || *byte == '\\' || *byte == '"')
				fprintf(stderr, "\\x%02x", *byte);
			else
				fputc(*byte, stderr);
		}
		fprintf(stderr, "\"%s\n", error->path_truncated ? " [truncated]" : "");
	}
	return (1);
}

static fsimg_status_t
example_context(const char *private_scratch, fsimg_ctx_t **out, fsimg_error_t *error)
{
	fsimg_options_t options;
	fsimg_status_t status;
	int fd;

	*out = NULL;
	fd = open(private_scratch, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1) {
		bzero(error, sizeof(*error));
		error->status = FSIMG_EIO;
		error->operation = FSIMG_OP_CONTEXT;
		error->os_error = errno;
		return (FSIMG_EIO);
	}
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(16) * 1024 * 1024;
	options.scratch_directory_fd = fd;
	status = fsimg_ctx_new(&options, out, error);
	/* The context duplicates the caller's descriptor on success. */
	if (close(fd) == -1 && status == FSIMG_OK) {
		bzero(error, sizeof(*error));
		error->status = FSIMG_EIO;
		error->operation = FSIMG_OP_CONTEXT;
		error->os_error = errno;
		fsimg_ctx_free(*out);
		*out = NULL;
		return (FSIMG_EIO);
	}
	return (status);
}

static int
example_host_failure(void)
{
	fprintf(stderr, "host operation failed: errno=%d\n", errno);
	return (1);
}

#endif
