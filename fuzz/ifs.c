/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"

/* An ImageFS is read as the exact slice: the decoder is forced, so every
 * input reaches the probe, open, the table pass, the derived hierarchy,
 * traversal, content and full verification under finite limits. Bytes
 * past image_size are never interpreted, so each input is also run cut to
 * the image_size its header declares, when that is shorter, so
 * length-changing mutations keep reaching the table; on that run every
 * read must lie inside the declared image. An opened image must fit the
 * slice, and no resource may outlive the reader.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static uint32_t declared_size(const uint8_t *, size_t);
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

/* The header's image_size in the byte order its flags byte selects, or 0
 * when the input holds no header.
 */
static uint32_t
declared_size(const uint8_t *data, size_t size)
{
	if (size < 0x58)
		return (0);
	if (data[7] == 0x05)
		return ((uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 |
		    (uint32_t)data[10] << 8 | data[11]);
	return ((uint32_t)data[11] << 24 | (uint32_t)data[10] << 16 |
	    (uint32_t)data[9] << 8 | data[8]);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint32_t image_size = declared_size(data, size);

	exercise(data, size);
	if (image_size != 0 && image_size < size)
		exercise(data, image_size);
	return (0);
}

static void
exercise(const uint8_t *data, size_t size)
{
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_read_options_t read_options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_status_t status;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_stats_t baseline, stats;
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
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return;
	}
	fsimg_read_options_init(&read_options);
	read_options.format = "ifs";
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		/* An opened image declares a size within the slice. */
		if (fsimg_reader_get_info(reader, &info, NULL) != FSIMG_OK ||
		    info.volume_bytes > size || info.detail_count > 16)
			abort();
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (entry->kind != FSIMG_ENTRY_FILE &&
			    entry->kind != FSIMG_ENTRY_SYMLINK)
				continue;
			if (entry->size > info.volume_bytes)
				abort();
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
	/* Full verification: a passing report holds a volume exactly the
	 * slice's length, every byte of it in use and complete.
	 */
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		if (status == FSIMG_OK && (!report.complete ||
		    report.allocated_clusters != size || report.free_clusters != 0))
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
