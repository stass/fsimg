/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
	uint64_t	 logical_size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void exercise_input(fuzz_input_t *);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static fsimg_status_t
fuzz_size(void *argument, uint64_t *out, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = argument;

	*out = input->logical_size;
	return (FSIMG_OK);
}

static fsimg_status_t
fuzz_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = argument;
	size_t present;

	*transferred = 0;
	if (offset > input->logical_size || length > input->logical_size - offset)
		abort();
	present = 0;
	if (offset < input->size) {
		present = input->size - (size_t)offset;
		if (present > length)
			present = length;
		memcpy(buffer, input->data + (size_t)offset, present);
	}
	memset((unsigned char *)buffer + present, 0, length - present);
	*transferred = length;
	return (FSIMG_OK);
}

static void
exercise_input(fuzz_input_t *input)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_verify_report_t report;
	fsimg_stats_t baseline, stats;
	fsimg_range_t range;
	fsimg_status_t status;
	uint64_t file_size;
	const fsimg_entry_t *entry;
	unsigned char bytes[256];
	size_t count;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_entries = 1000;
	options.max_depth = 8;
	options.max_path_bytes = 4096;
	options.max_work = 250000;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return;
	fsimg_ctx_get_stats(ctx, &baseline, NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return;
	}
	if (fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK) {
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (entry->kind != FSIMG_ENTRY_FILE)
				continue;
			file_size = entry->size;
			if (fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes),
			    &count, NULL) != FSIMG_OK)
				break;
			if (file_size > 512 && fsimg_reader_read_at(reader, file_size - 1,
			    bytes, 1, &count, NULL) != FSIMG_OK)
				break;
			status = fsimg_reader_next_range(reader, 0, &range, NULL);
			if (status < FSIMG_OK)
				break;
			if (status == FSIMG_OK && (range.offset != 0 || range.length == 0 ||
			    range.length > file_size))
				abort();
		}
		fsimg_reader_free(reader);
	}
	if (fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK) {
		fsimg_verify(reader, 0, &report, NULL);
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes || stats.scratch_bytes != 0 ||
	    stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fuzz_input_t input = { .data = data, .size = size, .logical_size = size };

	exercise_input(&input);

	/* Exercise the same bytes as a prefix of zero-filled logical storage.
	 * This reaches valid FAT32 metadata without a 33+ MiB resident seed.
	 * The first pass retains exact-length truncation tests; both passes have
	 * independent finite sessions and allocate no logical-volume payload.
	 */
	input.logical_size = UINT64_C(128) * 1024 * 1024 * 1024;
	if (input.logical_size < size)
		input.logical_size = size;
	exercise_input(&input);
	return (0);
}
