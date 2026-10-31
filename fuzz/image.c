/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"

/*
 * The input is a whole disk. Each run opens the image under several forced
 * stacks (detected, GPT at either sector size, MBR, no scheme) and drives
 * the scheme step, partition enumeration, every selector, layer
 * verification and a reader on the selected slice, under finite limits.
 * Mutations rarely keep a GPT's CRC32 fields valid, so each input also runs
 * with both headers and their entry arrays resealed, which lets mutations
 * reach the geometry, bounds and overlap checks behind the checksums.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static uint32_t crc32_bytes(const uint8_t *, size_t);
static uint64_t le(const uint8_t *, unsigned);
static void put32(uint8_t *, uint32_t);
static void reseal_header(uint8_t *, size_t, size_t);
static void exercise_stack(fsimg_ctx_t *, fsimg_io_t *, const char *, uint32_t);
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

static uint32_t
crc32_bytes(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return (crc ^ 0xffffffffu);
}

static uint64_t
le(const uint8_t *bytes, unsigned width)
{
	uint64_t value = 0;

	for (unsigned i = width; i > 0; i--)
		value = value << 8 | bytes[i - 1];
	return (value);
}

static void
put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		bytes[i] = (uint8_t)(value >> (8 * i));
}

/* Recompute the entry-array and header CRC32 fields of a GPT header at
 * offset, when its declared sizes lie inside the buffer.
 */
static void
reseal_header(uint8_t *disk, size_t size, size_t offset)
{
	uint8_t *header;
	uint64_t header_bytes, entries, count, entry_bytes;

	if (offset > size || size - offset < 512 ||
	    memcmp(disk + offset, "EFI PART", 8) != 0)
		return;
	header = disk + offset;
	header_bytes = le(header + 12, 4);
	entries = le(header + 72, 8);
	count = le(header + 80, 4);
	entry_bytes = le(header + 84, 4);
	if (header_bytes < 92 || header_bytes > 512 || count > 1024 ||
	    entry_bytes > 1024)
		return;
	if (entries <= size / 512 && count * entry_bytes <= size - entries * 512)
		put32(header + 88, crc32_bytes(disk + entries * 512,
		    (size_t)(count * entry_bytes)));
	put32(header + 16, 0);
	put32(header + 16, crc32_bytes(header, (size_t)header_bytes));
}

/* One forced stack: the scheme step, enumeration, each selector, layer
 * verification and a reader on whatever the automatic choice selects.
 */
static void
exercise_stack(fsimg_ctx_t *ctx, fsimg_io_t *io, const char *stack,
    uint32_t sector)
{
	static const char *const texts[] = { "EFI", "esp", "0x0c", "fat32",
	    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" };
	fsimg_image_options_t options;
	fsimg_image_info_t info;
	fsimg_partition_info_t partition;
	fsimg_layer_report_t report;
	fsimg_selector_t selector;
	fsimg_image_t *image;
	fsimg_io_t *volume;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;

	fsimg_image_options_init(&options);
	if (stack != NULL && fsimg_stack_parse(stack, &options.stack, NULL) != FSIMG_OK)
		abort();
	options.layers.gpt_sector_bytes = sector;
	if (fsimg_image_open(ctx, io, &options, &image, NULL) != FSIMG_OK)
		return;
	if (fsimg_image_get_info(image, &info, NULL) == FSIMG_OK) {
		while (fsimg_image_next_partition(image, &partition, NULL) == FSIMG_OK) {
			if (partition.used && (partition.length == 0 ||
			    partition.offset > info.disk_bytes ||
			    partition.length > info.disk_bytes - partition.offset))
				abort();
		}
		fsimg_image_verify(image, 0, &report, NULL);
	}
	bzero(&selector, sizeof(selector));
	for (uint32_t index = 1; index <= 4; index++) {
		selector.kind = FSIMG_SELECT_INDEX;
		selector.index = index;
		if (fsimg_image_select(image, &selector, &volume, NULL) == FSIMG_OK)
			fsimg_io_free(volume);
	}
	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		selector.kind = i == 0 ? FSIMG_SELECT_NAME : FSIMG_SELECT_TYPE;
		selector.text = texts[i];
		if (fsimg_image_select(image, &selector, &volume, NULL) == FSIMG_OK)
			fsimg_io_free(volume);
	}
	bzero(&selector, sizeof(selector));
	if (fsimg_image_select(image, &selector, &volume, NULL) == FSIMG_OK) {
		if (fsimg_reader_open(ctx, volume, NULL, &reader, NULL) == FSIMG_OK) {
			for (unsigned count = 0; count < 64 &&
			    fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK; count++)
				;
			fsimg_reader_free(reader);
		}
		fsimg_io_free(volume);
	}
	fsimg_image_free(image);
}

static void
exercise(const uint8_t *data, size_t size)
{
	static const struct {
		const char	*stack;
		uint32_t	 sector;
	} stacks[] = {
		{ NULL, 0 }, { "gpt", 0 }, { "gpt", 4096 }, { "mbr", 0 },
		{ "fat12/-", 0 }
	};
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_stats_t baseline, stats;

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
	for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++)
		exercise_stack(ctx, io, stacks[i].stack, stacks[i].sector);
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
	uint8_t *sealed;

	exercise(data, size);
	if (size < 1024 || (sealed = malloc(size)) == NULL)
		return (0);
	memcpy(sealed, data, size);
	reseal_header(sealed, size, 512);
	reseal_header(sealed, size, size - size % 512 - 512);
	exercise(sealed, size);
	free(sealed);
	return (0);
}
