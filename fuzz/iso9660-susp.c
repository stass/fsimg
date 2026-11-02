/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"

/* Input consists of four 128-byte system use slots (root dot, file,
 * symlink, subdirectory dot), then four 2048-byte continuation blocks.
 * Geometry and directory framing are fixed, so mutations reach SUSP.
 * Short inputs leave the rest zero. test-iso9660 writes the seed layout.
 */
#define BLOCK 2048u
#define IMAGE_BYTES (128u * BLOCK)
#define INPUT_BYTES (512u + 4u * BLOCK)

typedef struct fuzz_input {
	unsigned char *bytes;
} fuzz_input_t;

static void both16(unsigned char *, unsigned);
static void both32(unsigned char *, unsigned);
static size_t record(unsigned char *, unsigned, unsigned, unsigned,
    const unsigned char *, size_t, const unsigned char *);
static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void exercise(fuzz_input_t *, fsimg_iso_view_t);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static void
both16(unsigned char *p, unsigned value)
{
	p[0] = p[3] = value;
	p[1] = p[2] = value >> 8;
}

static void
both32(unsigned char *p, unsigned value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = value >> (8 * i);
		p[7 - i] = value >> (8 * i);
	}
}

static size_t
record(unsigned char *p, unsigned block, unsigned length, unsigned flags,
    const unsigned char *name, size_t namesize, const unsigned char *susp)
{
	size_t size = 33 + namesize + (namesize % 2 == 0);

	both32(p + 2, block);
	both32(p + 10, length);
	p[25] = flags;
	both16(p + 28, 1);
	p[32] = namesize;
	bcopy(name, p + 33, namesize);
	if (susp != NULL) {
		bcopy(susp, p + size, 128);
		size += 128;
	}
	p[0] = size;
	return (size);
}

static fsimg_status_t
fuzz_size(void *arg FSIMG_UNUSED, uint64_t *out,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*out = IMAGE_BYTES;
	return (FSIMG_OK);
}

static fsimg_status_t
fuzz_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = arg;

	if (offset > IMAGE_BYTES || length > IMAGE_BYTES - offset)
		abort();
	bcopy(input->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static void
exercise(fuzz_input_t *input, fsimg_iso_view_t view)
{
	fsimg_options_t options;
	fsimg_read_options_t read_options;
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
	options.max_entries = 100;
	options.max_depth = 8;
	options.max_path_bytes = 4096;
	options.max_work = 25000;
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
	fsimg_read_options_init(&read_options);
	read_options.format = "iso9660";
	read_options.iso_view = view;
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		for (unsigned entries = 0; entries < 100 &&
		    fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK; entries++) {
			if (entry->kind != FSIMG_ENTRY_FILE &&
			    entry->kind != FSIMG_ENTRY_SYMLINK)
				continue;
			/* Independent finite bounds even if content size is corrupt. */
			for (uint64_t offset = 0; offset < entry->size && offset < 4096;) {
				if (fsimg_reader_read_at(reader, offset, bytes,
				    sizeof(bytes), &count, NULL) != FSIMG_OK || count == 0)
					break;
				offset += count;
			}
		}
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.scratch_bytes != 0 || stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	unsigned char input_bytes[INPUT_BYTES] = {0};
	fuzz_input_t input;
	unsigned char *p, self = 0, up = 1;
	size_t offset;

	if (size > INPUT_BYTES)
		return (0);
	bcopy(data, input_bytes, size);
	input.bytes = calloc(1, IMAGE_BYTES);
	if (input.bytes == NULL)
		return (0);
	p = input.bytes + 16 * BLOCK;
	p[0] = 1;
	bcopy("CD001", p + 1, 5);
	p[6] = 1;
	both32(p + 80, 128);
	both16(p + 120, 1);
	both16(p + 124, 1);
	both16(p + 128, BLOCK);
	both32(p + 132, 10);
	p[140] = 82;
	p[151] = 83;
	record(p + 156, 84, BLOCK, 2, &self, 1, NULL);
	p[881] = 1;
	p = input.bytes + 17 * BLOCK;
	p[0] = 255;
	bcopy("CD001", p + 1, 5);
	p[6] = 1;
	p = input.bytes + 82 * BLOCK;
	p[0] = p[6] = 1;
	p[2] = 84;
	p = input.bytes + 83 * BLOCK;
	p[0] = p[7] = 1;
	p[5] = 84;
	p = input.bytes + 84 * BLOCK;
	offset = record(p, 84, BLOCK, 2, &self, 1, input_bytes);
	offset += record(p + offset, 84, BLOCK, 2, &up, 1, NULL);
	offset += record(p + offset, 104, 5, 0,
	    (const unsigned char *)"FILE;1", 6, input_bytes + 128);
	offset += record(p + offset, 0, 0, 0,
	    (const unsigned char *)"LINK;1", 6, input_bytes + 256);
	record(p + offset, 85, BLOCK, 2,
	    (const unsigned char *)"SUB", 3, NULL);
	p = input.bytes + 85 * BLOCK;
	offset = record(p, 85, BLOCK, 2, &self, 1, input_bytes + 384);
	record(p + offset, 84, BLOCK, 2, &up, 1, NULL);
	bcopy("hello", input.bytes + 104 * BLOCK, 5);
	bcopy(input_bytes + 512, input.bytes + 110 * BLOCK, 4 * BLOCK);
	exercise(&input, FSIMG_ISO_VIEW_AUTO);
	exercise(&input, FSIMG_ISO_VIEW_ROCKRIDGE);
	free(input.bytes);
	return (0);
}
