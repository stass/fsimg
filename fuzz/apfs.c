/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"
#include "apfs-gen.h"

/* The input is the whole container. Each input is opened through the
 * public reader as APFS, which runs the probe, block 0, checkpoint
 * selection, the ephemeral mapping table, the container object map and the
 * selected volume, then reads the volume information and frees the reader.
 * Directory listing is not offered, so no entry may be returned.
 *
 * Almost every mutation breaks an object checksum, which stops the open at
 * the first block it reads. Each input therefore runs a second time with
 * the checksum of every nonzero block recomputed, which lets mutations
 * reach the decoders behind it. The checksum comes from the test
 * generator's independent implementation, so a fault in the library's
 * own cannot hide behind the repair.
 */
#define NX_BLOCK_SIZE	36	/* offset of nx_block_size in block 0 */
#define BLOCK_MIN	4096u
#define BLOCK_MAX	65536u
#define VOLUME_TRIES	5	/* slot indexes 0..4 after the default */

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static uint32_t repair_block_size(const uint8_t *, size_t);
static bool repair(uint8_t *, size_t);
static void open_once(fsimg_ctx_t *, fsimg_io_t *,
    const fsimg_read_options_t *);
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
	bcopy(input->data + (size_t)offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

/* The block size block 0 claims when it is a legal one, as the reader
 * reads block 0; otherwise the smallest, which every probe reads.
 */
static uint32_t
repair_block_size(const uint8_t *data, size_t size)
{
	uint32_t block_size;

	if (size < NX_BLOCK_SIZE + 4)
		return (BLOCK_MIN);
	block_size = (uint32_t)data[NX_BLOCK_SIZE] |
	    (uint32_t)data[NX_BLOCK_SIZE + 1] << 8 |
	    (uint32_t)data[NX_BLOCK_SIZE + 2] << 16 |
	    (uint32_t)data[NX_BLOCK_SIZE + 3] << 24;
	if (block_size < BLOCK_MIN || block_size > BLOCK_MAX ||
	    (block_size & (block_size - 1)) != 0)
		return (BLOCK_MIN);
	return (block_size);
}

/* Reseal every whole block with a nonzero byte after its checksum field;
 * report whether any byte changed. A zero block stays zero, as an
 * unwritten block would, so an empty checkpoint slot the input leaves
 * keeps failing its checksum instead of turning into a valid object.
 * The repair seals whole blocks only: the seeds hold no object larger
 * than one block.
 */
static bool
repair(uint8_t *data, size_t size)
{
	uint32_t block_size = repair_block_size(data, size);
	uint8_t checksum[8];
	bool changed = false;

	for (size_t at = 0; size - at >= block_size; at += block_size) {
		uint8_t *block = data + at;
		bool empty = true;

		for (size_t i = 8; i < block_size && empty; i++)
			empty = block[i] == 0;
		if (empty)
			continue;
		bcopy(block, checksum, sizeof(checksum));
		if (!apfs_gen_seal(block, block_size))
			abort();
		changed |= memcmp(checksum, block, sizeof(checksum)) != 0;
	}
	return (changed);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *copy;

	exercise(data, size);
	copy = malloc(size != 0 ? size : 1);
	if (copy == NULL)
		return (0);
	bcopy(data, copy, size);
	if (repair(copy, size))
		exercise(copy, size);
	free(copy);
	return (0);
}

/* One open, the volume information and the refused listing, then close. */
static void
open_once(fsimg_ctx_t *ctx, fsimg_io_t *io,
    const fsimg_read_options_t *read_options)
{
	fsimg_reader_t *reader;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;

	if (fsimg_reader_open(ctx, io, read_options, &reader, NULL) != FSIMG_OK)
		return;
	if (fsimg_reader_get_info(reader, &info, NULL) != FSIMG_OK ||
	    strnlen(info.label, sizeof(info.label)) == sizeof(info.label) ||
	    strcmp(info.format, "apfs") != 0)
		abort();
	/* The driver refuses directories; the walker may also run out of
	 * its budget before asking, but it never yields an entry.
	 */
	if (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK)
		abort();
	fsimg_reader_free(reader);
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

	/* The default volume and each slot, including one past the seed's
	 * volume array, after the probe; then slot 0 without the probe, so
	 * block 0 the probe rejects still reaches the open's own checks.
	 */
	fsimg_read_options_init(&read_options);
	read_options.format = "apfs";
	open_once(ctx, io, &read_options);
	read_options.volume_index_set = true;
	for (uint32_t index = 0; index < VOLUME_TRIES; index++) {
		read_options.volume_index = index;
		open_once(ctx, io, &read_options);
	}
	read_options.volume_index = 0;
	read_options.flags = FSIMG_READ_ASSUME_IDENTITY;
	open_once(ctx, io, &read_options);

	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.scratch_bytes != 0 || stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}
