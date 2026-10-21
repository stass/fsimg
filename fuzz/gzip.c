/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <sys/types.h>

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"

/*
 * The input is a whole gzip file. Each run opens it with gzip forced, as
 * a file and as a stream through a pipe, each into a fresh unlinked spool
 * under a 4 MiB limit: the member header parser (FEXTRA, FNAME, FCOMMENT,
 * the header CRC16), inflate, the trailer checks, the trailing-byte rule
 * and the spool's reservation all see the mutated bytes. Every outcome
 * must release what it charged.
 */
#define SPOOL_LIMIT	(4u * 1024 * 1024)

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static int spool_new(void);
static void open_once(fsimg_ctx_t *, fsimg_io_t *, int);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static fsimg_status_t
fuzz_size(void *argument, uint64_t *out, fsimg_error_t *error FSIMG_UNUSED)
{
	*out = ((fuzz_input_t *)argument)->size;
	return (FSIMG_OK);
}

static fsimg_status_t
fuzz_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = argument;

	if (offset > input->size || length > input->size - offset)
		abort();
	memcpy(buffer, input->data + (size_t)offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

/* An empty unlinked file, as the coordinator hands one over. */
static int
spool_new(void)
{
	char path[] = "/tmp/fsimg-fuzz-spool.XXXXXX";
	int fd;

	fd = mkstemp(path);
	if (fd == -1)
		abort();
	if (unlink(path) == -1)
		abort();
	return (fd);
}

/* One open of the file, or of the stream on stream_fd when io is NULL. */
static void
open_once(fsimg_ctx_t *ctx, fsimg_io_t *io, int stream_fd)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_options_t options;
	fsimg_image_t *image;
	fsimg_io_t *disk;

	fsimg_image_options_init(&options);
	options.flags = FSIMG_IMAGE_GZIP;
	options.spool_fd = spool_new();
	if ((io != NULL ? fsimg_image_open(ctx, io, &options, &image, NULL) :
	    fsimg_image_open_stream(ctx, stream_fd, &options, &image, NULL)) !=
	    FSIMG_OK)
		return;
	if (fsimg_image_select(image, &selector, &disk, NULL) == FSIMG_OK)
		fsimg_io_free(disk);
	fsimg_image_free(image);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_stats_t baseline, stats;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	ssize_t written;
	int ends[2];

	signal(SIGPIPE, SIG_IGN);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.spool_bytes = SPOOL_LIMIT;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return (0);
	fsimg_ctx_get_stats(ctx, &baseline, NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) == FSIMG_OK) {
		open_once(ctx, io, -1);
		fsimg_io_free(io);
	}
	/* A pipe holds the whole input only when it is small; larger inputs
	 * exercise the file path alone.
	 */
	if (size <= 16 * 1024 && pipe(ends) == 0) {
		written = size == 0 ? 0 : write(ends[1], data, size);
		close(ends[1]);
		if (written == (ssize_t)size)
			open_once(ctx, NULL, ends[0]);
		close(ends[0]);
	}
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.descriptors != 0 || stats.spool_bytes != 0)
		abort();
	fsimg_ctx_free(ctx);
	return (0);
}
