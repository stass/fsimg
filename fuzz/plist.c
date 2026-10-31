/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "layer.h"

/*
 * The input is a DMG's XML property list. The bounded parser streams it to
 * a visitor that checks the contract the DMG walk relies on: opens and
 * closes pair up and name the same element, nesting stays within 32
 * elements, text arrives only inside a key, string or integer and never
 * beyond 4 KiB, and decoded data arrives only inside a data element.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

typedef struct fuzz_visit {
	fsimg_plist_element_t open[FSIMG_PLIST_MAX_DEPTH];
	unsigned	 depth;
} fuzz_visit_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t visit_open(void *, fsimg_plist_element_t,
    fsimg_error_t *);
static fsimg_status_t visit_text(void *, const char *, size_t,
    fsimg_error_t *);
static fsimg_status_t visit_data(void *, const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t visit_close(void *, fsimg_plist_element_t,
    fsimg_error_t *);
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

static fsimg_status_t
visit_open(void *argument, fsimg_plist_element_t kind,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_visit_t *visit = argument;

	if (visit->depth == FSIMG_PLIST_MAX_DEPTH || kind > FSIMG_PLIST_FALSE)
		abort();
	visit->open[visit->depth++] = kind;
	return (FSIMG_OK);
}

static fsimg_status_t
visit_text(void *argument, const char *text FSIMG_UNUSED, size_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_visit_t *visit = argument;
	fsimg_plist_element_t kind;

	if (visit->depth == 0 || length > FSIMG_PLIST_MAX_TEXT)
		abort();
	kind = visit->open[visit->depth - 1];
	if (kind != FSIMG_PLIST_KEY && kind != FSIMG_PLIST_STRING &&
	    kind != FSIMG_PLIST_INTEGER)
		abort();
	return (FSIMG_OK);
}

static fsimg_status_t
visit_data(void *argument, const unsigned char *bytes FSIMG_UNUSED,
    size_t length FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_visit_t *visit = argument;

	if (visit->depth == 0 || visit->open[visit->depth - 1] != FSIMG_PLIST_DATA)
		abort();
	return (FSIMG_OK);
}

static fsimg_status_t
visit_close(void *argument, fsimg_plist_element_t kind,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_visit_t *visit = argument;

	if (visit->depth == 0 || visit->open[visit->depth - 1] != kind)
		abort();
	visit->depth--;
	return (FSIMG_OK);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_plist_visitor_t visitor;
	fsimg_session_t session;
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fuzz_visit_t visit;
	fsimg_status_t status;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_work = 1000000;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return (0);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return (0);
	}
	memset(&visit, 0, sizeof(visit));
	visitor.open = visit_open;
	visitor.text = visit_text;
	visitor.data = visit_data;
	visitor.close = visit_close;
	visitor.argument = &visit;
	fsimg_session_init(&session, ctx, "plist fuzz", 0);
	status = fsimg_plist_parse(ctx, io, &session, 0, size, &visitor, NULL);
	/* A complete parse closes every element it opened. */
	if (status == FSIMG_OK && visit.depth != 0)
		abort();
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	return (0);
}
