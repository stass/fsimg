/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void exercise(const uint8_t *, size_t, const char *, fsimg_iso_view_t);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static fsimg_status_t
fuzz_size(void *arg, uint64_t *out, fsimg_error_t *error FSIMG_UNUSED)
{
	*out = ((fuzz_input_t *)arg)->size;
	return (FSIMG_OK);
}

static fsimg_status_t
fuzz_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = arg;

	if (offset > input->size || length > input->size - offset)
		abort();
	bcopy(input->data + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static void
exercise(const uint8_t *data, size_t size, const char *format,
    fsimg_iso_view_t view)
{
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_read_options_t read_options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_stats_t baseline, stats;
	fsimg_verify_report_t report;
	fsimg_status_t status;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char bytes[4096];
	uint64_t offset;
	size_t count;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_entries = 1000;
	options.max_depth = 16;
	options.max_path_bytes = 4096;
	options.max_work = 250000;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return;
	fsimg_ctx_get_stats(ctx, &baseline, NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return;
	}
	fsimg_read_options_init(&read_options);
	read_options.format = format;
	read_options.iso_view = view;
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) ==
	    FSIMG_OK) {
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (entry->kind != FSIMG_ENTRY_FILE)
				continue;
			/* The work ceiling also bounds content on heavily
			 * shared images. */
			for (offset = 0; offset < entry->size;
			    offset += count) {
				if (fsimg_reader_next_range(reader, offset,
				    &range, NULL) != FSIMG_OK)
					break;
				if (range.offset != offset ||
				    range.length == 0 ||
				    range.length > entry->size - offset)
					abort();
				if (fsimg_reader_read_at(reader, offset, bytes,
				    sizeof(bytes), &count, NULL) != FSIMG_OK ||
				    count == 0)
					break;
			}
		}
		fsimg_reader_free(reader);
	}
	/* Each verification starts before traversal on a fresh reader. */
	for (unsigned payload = 0; payload < 2; payload++) {
		if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) !=
		    FSIMG_OK)
			continue;
		bzero(&report, sizeof(report));
		status = fsimg_verify(reader, payload ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, NULL);
		if (report.complete != (status == FSIMG_OK))
			abort();
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.scratch_bytes != 0 ||
	    stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	for (unsigned forced = 0; forced < 2; forced++) {
		for (unsigned view = 0; view <= 3; view++)
			exercise(data, size, forced ? "iso9660" : NULL,
			    (fsimg_iso_view_t)view);
	}
	return (0);
}
