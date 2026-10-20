/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"
#include "qnx6.h"

/* QNX6 finds its second header from the slice length, so the input is the
 * whole slice: no zero-extended logical volume as in the FAT harness. The
 * decoder is forced, so every input reaches the QNX6 probe, open,
 * traversal, content and verification paths under finite limits. A
 * mutation almost always breaks a header CRC, so each input is also run
 * with both headers resealed, which lets mutations reach the trees.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void reseal(uint8_t *, size_t, size_t);
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

/* Store the CRC of the header at offset, in the byte order of its magic. */
static void
reseal(uint8_t *data, size_t size, size_t offset)
{
	uint32_t crc;
	bool big;

	if (offset > size || size - offset < QNX6_HEADER_BYTES ||
	    !qnx6_magic_order(data + offset, &big))
		return;
	crc = qnx6_crc32(data + offset + 8, QNX6_HEADER_BYTES - 8);
	for (unsigned i = 0; i < 4; i++)
		data[offset + 4 + (big ? 3 - i : i)] = (uint8_t)(crc >> (8 * i));
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *copy;

	exercise(data, size);
	if (size / QNX6_HEADER_AREA < 2)
		return (0);
	copy = malloc(size);
	if (copy == NULL)
		return (0);
	memcpy(copy, data, size);
	/* The second header occupies the last whole 4 KiB area but one. */
	reseal(copy, size, QNX6_HEADER_OFFSET);
	reseal(copy, size, (size / QNX6_HEADER_AREA - 1) * QNX6_HEADER_AREA);
	if (memcmp(copy, data, size) != 0)
		exercise(copy, size);
	free(copy);
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
	read_options.format = "qnx6";
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
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes || stats.scratch_bytes != 0 ||
	    stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}
