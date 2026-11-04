/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_FORCE_SHORT_H
#define FSIMG_TEST_FORCE_SHORT_H

#include <string.h>

#include "host.h"
#include "fsimg.h"
#include "test.h"

static fsimg_status_t force_short_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t force_short_size(void *, uint64_t *, fsimg_error_t *);
static void check_force_short_volumes(const char *, uint64_t);
static void check_force_truncated_volume(fsimg_ctx_t *, fsimg_io_t *,
    const char *);

static fsimg_status_t
force_short_read(void *cookie, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	uint64_t size = *(const uint64_t *)cookie;

	*count = offset >= size ? 0 : (size_t)(size - offset);
	if (*count > length)
		*count = length;
	memset(buffer, 0, *count);
	return (FSIMG_OK);
}

static fsimg_status_t
force_short_size(void *cookie, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = *(const uint64_t *)cookie;
	return (FSIMG_OK);
}

/* Lengths are independent of driver declarations: the last case ends one
 * byte before the format's documented identification prefix is complete.
 */
static void
check_force_short_volumes(const char *format, uint64_t prefix)
{
	const uint64_t lengths[] = { 0, 512, prefix - 1 };
	fsimg_io_ops_t operations;
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	uint64_t length;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&operations);
	operations.read_at = force_short_read;
	operations.get_size = force_short_size;
	fsimg_read_options_init(&options);
	options.format = format;
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		length = lengths[i];
		reader = NULL;
		CHECK(fsimg_io_from_callbacks(ctx, &operations, &length,
		    &io, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(ctx, io, &options, &reader, NULL) != FSIMG_OK);
		CHECK(reader == NULL);
		fsimg_io_free(io);
	}
	fsimg_ctx_free(ctx);
}

/* Identity remains intact. Drivers may reject the shortened geometry at
 * open or discover missing metadata while walking the complete tree.
 */
static void
check_force_truncated_volume(fsimg_ctx_t *ctx, fsimg_io_t *io,
    const char *format)
{
	fsimg_read_options_t options;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	fsimg_read_options_init(&options);
	options.format = format;
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	status = fsimg_reader_open(ctx, io, &options, &reader, NULL);
	if (status == FSIMG_OK) {
		do {
			status = fsimg_reader_next(reader, &entry, NULL);
		} while (status == FSIMG_OK);
		CHECK(status == FSIMG_DONE || status < FSIMG_OK);
	} else {
		CHECK(status < FSIMG_OK);
		CHECK(reader == NULL);
	}
	fsimg_reader_free(reader);
}

#endif
