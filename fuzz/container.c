/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"

/*
 * The input is a whole image file. Each run opens it under detection and
 * under each forced container (DMG, fixed VHD, sparse image, QNX boot
 * image, hbcifs), which drives the koly trailer, the plist, the mish
 * tables, the VHD footer, the sparse header and chunk walk, the startup
 * header and the framed chunk walk, the chunk index's sort and tiling
 * pass, and the hbcifs header and its LZO1X stream decoded into a spool
 * under a small spool limit. An opened container is verified with
 * read-through, which decodes every chunk through the cache, and its disk
 * is read in pieces from both ends. A VHD footer's checksum rarely
 * survives a mutation, so each input also runs with the footer in its
 * last 512 bytes resealed.
 */
#define PIECE_BYTES	(64u * 1024)
#define READ_SPAN	(4u * 1024 * 1024)
#define SPOOL_LIMIT	(16u * 1024 * 1024)

typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void reseal_footer(uint8_t *, size_t);
static int spool_new(void);
static void read_span(fsimg_io_t *, uint64_t, uint64_t, unsigned char *);
static void exercise_stack(fsimg_ctx_t *, fsimg_io_t *, const char *,
    unsigned char *);
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

/* Recompute the checksum of a big-endian VHD footer in the last sector:
 * the one's complement of the byte sum with the field itself zero.
 */
static void
reseal_footer(uint8_t *data, size_t size)
{
	uint8_t *footer;
	uint32_t sum = 0;

	if (size < 512 || memcmp(data + size - 512, "conectix", 8) != 0)
		return;
	footer = data + size - 512;
	memset(footer + 64, 0, 4);
	for (unsigned i = 0; i < 512; i++)
		sum += footer[i];
	sum = ~sum;
	for (unsigned i = 0; i < 4; i++)
		footer[64 + i] = (uint8_t)(sum >> (24 - 8 * i));
}

/* An empty unlinked file, as the coordinator hands one over. */
static int
spool_new(void)
{
	char path[] = "/tmp/fsimg-fuzz-spool.XXXXXX";
	int fd;

	fd = mkstemp(path);
	if (fd == -1)
		abort();
	if (unlink(path) == -1)
		abort();
	return (fd);
}

/* Read [offset, offset + length) of the disk in pieces, stopping at the
 * first failure: a damaged chunk ends the read, never the process.
 */
static void
read_span(fsimg_io_t *disk, uint64_t offset, uint64_t length,
    unsigned char *piece)
{
	size_t count, transferred;

	for (uint64_t done = 0; done < length; done += count) {
		count = length - done < PIECE_BYTES ? (size_t)(length - done) :
		    PIECE_BYTES;
		if (fsimg_io_read_exact(disk, offset + done, piece, count,
		    &transferred, NULL) != FSIMG_OK)
			return;
	}
}

/* One forced stack: open with a spool, information, verification with
 * read-through, and reads of the container's disk from both ends.
 */
static void
exercise_stack(fsimg_ctx_t *ctx, fsimg_io_t *io, const char *stack,
    unsigned char *piece)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_options_t options;
	fsimg_image_info_t info;
	fsimg_layer_report_t report;
	fsimg_image_t *image;
	fsimg_io_t *disk;
	uint64_t size, span;

	fsimg_image_options_init(&options);
	if (fsimg_stack_parse(stack, &options.stack, NULL) != FSIMG_OK)
		abort();
	options.spool_fd = spool_new();
	if (fsimg_image_open(ctx, io, &options, &image, NULL) != FSIMG_OK)
		return;
	if (fsimg_image_get_info(image, &info, NULL) == FSIMG_OK &&
	    ((info.container[0] == '\0' && (info.chunk_count != 0 ||
	    info.detail_count != 0)) ||
	    info.detail_count > FSIMG_IMAGE_DETAILS_MAX))
		abort();
	fsimg_image_verify(image, FSIMG_VERIFY_PAYLOAD, &report, NULL);
	if (fsimg_image_select(image, &selector, &disk, NULL) == FSIMG_OK) {
		if (fsimg_io_get_size(disk, &size, NULL) != FSIMG_OK)
			abort();
		span = size < READ_SPAN ? size : READ_SPAN;
		read_span(disk, 0, span, piece);
		read_span(disk, size - span, span, piece);
		fsimg_io_free(disk);
	}
	fsimg_image_free(image);
}

static void
exercise(const uint8_t *data, size_t size)
{
	/* The scheme slot is forced empty so a container's disk need hold
	 * no table; the container slot is detected or forced. A boot image
	 * and an hbcifs container carry an ImageFS, so their stacks name that
	 * filesystem: detected, which the carried rule then checks, and
	 * forced.
	 */
	static const char *const stacks[] = { "fat12/-", "fat12/-/dmg",
	    "fat12/-/vhd", "fat12/-/simg", "ifs/-", "ifs/-/ifsboot",
	    "ifs/-/hbcifs" };
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_stats_t baseline, stats;
	unsigned char *piece;

	piece = malloc(PIECE_BYTES);
	if (piece == NULL)
		return;
	fsimg_options_init(&options);
	options.memory_bytes = 32 * 1024 * 1024;
	options.max_work = 64 * 1024 * 1024;
	options.spool_bytes = SPOOL_LIMIT;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK) {
		free(piece);
		return;
	}
	fsimg_ctx_get_stats(ctx, &baseline, NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) == FSIMG_OK) {
		for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++)
			exercise_stack(ctx, io, stacks[i], piece);
		fsimg_io_free(io);
	}
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.descriptors != 0 || stats.spool_bytes != 0)
		abort();
	fsimg_ctx_free(ctx);
	free(piece);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t *sealed;

	exercise(data, size);
	if (size < 512 || (sealed = malloc(size)) == NULL)
		return (0);
	memcpy(sealed, data, size);
	reseal_footer(sealed, size);
	exercise(sealed, size);
	free(sealed);
	return (0);
}
