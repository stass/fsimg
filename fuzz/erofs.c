/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsimg.h"
#include "host.h"
#include "erofs.h"

/* The input is the whole volume: EROFS addresses everything from the start
 * of the image, and the decoder is forced so every input reaches the probe,
 * open, mapping, directory, content and verification paths under finite
 * limits. A mutation almost always breaks the superblock checksum, so each
 * input is also run with the checksum recomputed, when the volume claims
 * to carry one, which lets mutations reach the trees.
 */
enum {
	SB_CHECKSUM = 0x04,	/* offsets from volume byte 1024 */
	SB_COMPAT = 0x08,
	SB_BLKSZBITS = 0x0c
};

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static bool reseal(uint8_t *, size_t);
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

/* Store the checksum of the superblock region if SB_CHKSUM is set and the
 * checked range lies inside the input; report whether the bytes changed.
 */
static bool
reseal(uint8_t *data, size_t size)
{
	uint8_t *sb = data + EROFS_SUPER_OFFSET;
	size_t length;
	uint32_t crc;

	if (size < EROFS_PROBE_BYTES ||
	    (sb[SB_COMPAT] & EROFS_COMPAT_SB_CHKSUM) == 0)
		return (false);
	length = size - EROFS_SUPER_OFFSET;
	if (length > EROFS_SUPER_REGION_BYTES)
		length = EROFS_SUPER_REGION_BYTES;
	if (!erofs_super_checksum(sb, length, sb[SB_BLKSZBITS], &crc))
		return (false);
	for (unsigned i = 0; i < 4; i++)
		sb[SB_CHECKSUM + i] = (uint8_t)(crc >> (8 * i));
	return (true);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *copy;

	exercise(data, size);
	copy = malloc(size != 0 ? size : 1);
	if (copy == NULL)
		return (0);
	memcpy(copy, data, size);
	if (reseal(copy, size) && memcmp(copy, data, size) != 0)
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
	read_options.format = "erofs";
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
