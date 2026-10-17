/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "host.h"
#include "test.h"

static fsimg_status_t empty_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t empty_size(void *, uint64_t *, fsimg_error_t *);

static fsimg_status_t
empty_read(void *argument FSIMG_UNUSED, uint64_t offset FSIMG_UNUSED,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	bzero(buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
empty_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = *(uint64_t *)argument;
	return (FSIMG_OK);
}

int
main(void)
{
	fsimg_ctx_t *ctx;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	fsimg_io_ops_t ops;
	fsimg_read_options_t options;
	fsimg_error_t error;
	uint64_t length;
	fsimg_status_t status;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = empty_read;
	ops.get_size = empty_size;
	for (length = 0; length < 512; length++) {
		CHECK(fsimg_io_from_callbacks(ctx, &ops, &length, &io, NULL) == FSIMG_OK);
		for (unsigned forced = 0; forced < 3; forced++) {
			fsimg_read_options_init(&options);
			options.format = forced == 2 ? "exfat" : forced == 1 ? "fat16" : NULL;
			status = fsimg_reader_open(ctx, io, &options, &reader, &error);
			if (status != FSIMG_EFORMAT)
				fprintf(stderr, "short prefix length=%llu forced=%u status=%d: %s\n",
				    (unsigned long long)length, forced, status, error.message);
			CHECK(status == FSIMG_EFORMAT && reader == NULL);
		}
		fsimg_io_free(io);
	}
	fsimg_ctx_free(ctx);
	return (0);
}
