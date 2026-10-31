/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"

/* QNX4 derives its block count from the slice length, so the input is the
 * whole slice. The decoder is forced, so every input reaches the QNX4
 * probe, open, traversal and content paths under finite limits. A
 * mutation that changes the length by less than a block makes it a
 * partial block, which open refuses at once; each input is therefore also
 * run cut to whole blocks, which lets length-changing mutations reach the
 * structures.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void exercise(const uint8_t *, size_t);
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

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	exercise(data, size);
	if (size % 512 != 0)
		exercise(data, size - size % 512);
	return (0);
}

static void
exercise(const uint8_t *data, size_t size)
{
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_read_options_t read_options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_verify_report_t report;
	fsimg_stats_t baseline, stats;
	const fsimg_entry_t *entry;
	unsigned char bytes[256];
	size_t count;
	fsimg_status_t status;

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
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return;
	}
	fsimg_read_options_init(&read_options);
	read_options.format = "qnx4";
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (entry->kind != FSIMG_ENTRY_FILE &&
			    entry->kind != FSIMG_ENTRY_SYMLINK)
				continue;
			if (fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes),
			    &count, NULL) != FSIMG_OK)
				break;
			if (entry->size > sizeof(bytes) &&
			    fsimg_reader_read_at(reader, entry->size - 1, bytes, 1,
			    &count, NULL) != FSIMG_OK)
				break;
		}
		fsimg_reader_free(reader);
	}
	/* Full verification: a passing report accounts for every block of
	 * the slice, which is the whole input, as allocated or free.
	 */
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		if (status == FSIMG_OK && report.allocated_clusters +
		    report.free_clusters != size / 512)
			abort();
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes || stats.scratch_bytes != 0 ||
	    stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}
