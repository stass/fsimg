/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "format.h"
#include "fsimg.h"
#include "internal.h"
#include "scale.h"
#include "test.h"

#define BLOCK 2048u
#define ROOT 24u
#define MILLION 1000000u
#define SECTION_BYTES UINT32_C(0xfffff800)
#define FOUR_TIB (UINT64_C(4) << 40)

typedef enum shape {
	WIDE,
	DEEP,
	SECTIONS,
	CAPACITY
} shape_t;

/* Only a sector lives in memory. Directory sectors and all-zero payload
 * sectors are computed from their addresses, including the 8 TiB volume.
 */
typedef struct image {
	shape_t shape;
	uint32_t blocks;
	uint32_t directory_blocks;
	uint32_t entries;
	uint64_t reads;
	uint64_t last_offset;
	uint64_t metadata_reads;
} image_t;

static void both(unsigned char *, uint32_t, unsigned);
static size_t record(unsigned char *, const void *, size_t, uint32_t,
    uint32_t, unsigned);
static void sector(const image_t *, uint64_t, unsigned char *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void require_ok(fsimg_status_t, const fsimg_error_t *);
static void run(shape_t, unsigned);

static void
both(unsigned char *p, uint32_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++) {
		p[i] = value >> (8 * i);
		p[2 * width - 1 - i] = value >> (8 * i);
	}
}

static size_t
record(unsigned char *p, const void *name, size_t names,
    uint32_t extent, uint32_t bytes, unsigned flags)
{
	size_t length = 33 + names + (names % 2 == 0);

	p[0] = length;
	both(p + 2, extent, 4);
	both(p + 10, bytes, 4);
	p[25] = flags;
	both(p + 28, 1, 2);
	p[32] = names;
	bcopy(name, p + 33, names);
	return (length);
}

static void
sector(const image_t *image, uint64_t block, unsigned char *p)
{
	unsigned char self = 0, up = 1;
	uint32_t index, first, slots, extent, bytes, flags;
	uint64_t logical;
	size_t offset;
	char name[15];

	bzero(p, BLOCK);
	if (block == 16 || block == 17) {
		p[0] = block == 16 ? 1 : 255;
		bcopy("CD001", p + 1, 5);
		p[6] = 1;
		if (block == 17)
			return;
		memset(p + 40, ' ', 32);
		both(p + 80, image->blocks, 4);
		both(p + 120, 1, 2);
		both(p + 124, 1, 2);
		both(p + 128, BLOCK, 2);
		both(p + 132, image->shape == DEEP ? (image->entries + 1) * 10 : 10, 4);
		p[140] = 18;
		p[151] = 20;
		record(p + 156, &self, 1, ROOT,
		    image->shape == DEEP ? BLOCK : image->directory_blocks * BLOCK, 2);
		p[881] = 1;
		return;
	}
	if (block >= 18 && block < 22) {
		unsigned char row[10];
		unsigned rows = image->shape == DEEP ? image->entries + 1 : 1;
		unsigned table_block = (block - 18) % 2;
		bool little = block < 20;

		for (unsigned i = 0; i < rows; i++) {
			bzero(row, sizeof(row));
			row[0] = 1;
			extent = ROOT + i;
			for (unsigned j = 0; j < 4; j++)
				row[2 + (little ? j : 3 - j)] = extent >> (8 * j);
			flags = i == 0 ? 1 : i;
			row[little ? 6 : 7] = flags;
			row[little ? 7 : 6] = flags >> 8;
			row[8] = i == 0 ? 0 : 'D';
			for (unsigned j = 0; j < sizeof(row); j++) {
				unsigned at = i * sizeof(row) + j;

				if (at / BLOCK == table_block)
					p[at % BLOCK] = row[j];
			}
		}
		return;
	}
	if (image->shape == CAPACITY && block == image->blocks - 1) {
		bcopy("hello", p, 5);
		return;
	}
	if (block < ROOT || block >= ROOT + image->directory_blocks)
		return;
	index = block - ROOT;
	if (image->shape == DEEP) {
		offset = record(p, &self, 1, block, BLOCK, 2);
		if (index == 0) {
			/* SUSP handshake and RRIP 1.10 extension reference. */
			bcopy("SP\007\001\276\357\000ER\022\001\012\000\000\001RRIP_1991A",
			    p + offset, 25);
			p[0] = 60;
			offset = 60;
		}
		offset += record(p + offset, &up, 1,
		    index == 0 ? ROOT : block - 1, BLOCK, 2);
		if (index < image->entries)
			record(p + offset, "D", 1, block + 1, BLOCK, 2);
		return;
	}
	/* A 48-byte record leaves 40 slots after the dots in the first
	 * sector and 42 slots in each subsequent sector. */
	first = index == 0 ? 0 : 40 + (index - 1) * 42;
	slots = index == 0 ? 40 : 42;
	offset = 0;
	if (index == 0) {
		offset += record(p, &self, 1, ROOT,
		    image->directory_blocks * BLOCK, 2);
		offset += record(p + offset, &up, 1, ROOT,
		    image->directory_blocks * BLOCK, 2);
	}
	for (unsigned i = first; i < first + slots && i < image->entries; i++) {
		CHECK(snprintf(name, sizeof(name), "%08u.TXT;1",
		    image->shape == SECTIONS ? 0 : i) == 14);
		extent = ROOT + image->directory_blocks;
		bytes = 0;
		flags = 0;
		if (image->shape == CAPACITY) {
			extent = image->blocks - 1;
			bytes = 5;
		}
		if (image->shape == SECTIONS) {
			logical = (uint64_t)i * SECTION_BYTES;
			extent += logical / BLOCK + i;
			bytes = FOUR_TIB - logical > SECTION_BYTES ?
			    SECTION_BYTES : FOUR_TIB - logical;
			flags = i + 1 < image->entries ? 0x80 : 0;
		}
		offset += record(p + offset, name, 14, extent, bytes, flags);
	}
}

static fsimg_status_t
image_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	*size = (uint64_t)image->blocks * BLOCK;
	return (FSIMG_OK);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;
	unsigned char bytes[BLOCK], *out = buffer;
	size_t copied = 0, part;
	uint64_t block;

	CHECK(offset <= (uint64_t)image->blocks * BLOCK);
	CHECK(length <= (uint64_t)image->blocks * BLOCK - offset);
	image->reads++;
	image->last_offset = offset;
	while (copied < length) {
		block = offset / BLOCK;
		part = BLOCK - offset % BLOCK;
		if (part > length - copied)
			part = length - copied;
		if (block >= ROOT && block < ROOT + image->directory_blocks)
			image->metadata_reads++;
		sector(image, block, bytes);
		bcopy(bytes + offset % BLOCK, out + copied, part);
		copied += part;
		offset += part;
	}
	*count = copied;
	return (FSIMG_OK);
}

static void
require_ok(fsimg_status_t status, const fsimg_error_t *error)
{
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: %s\n", fsimg_status_string(status), error->message);
	CHECK(status == FSIMG_OK);
}

static void
run(shape_t shape, unsigned tree_depth)
{
	const char *labels[] = { "million-directory", "rockridge-tree",
	    "multi-extent-4TiB", "maximum-volume" };
	image_t image = { .shape = shape, .blocks = UINT32_MAX };
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader = NULL;
	fsimg_options_t limits;
	fsimg_io_ops_t ops;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_stats_t baseline, stats;
	fsimg_error_t error = {0};
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	struct rusage usage;
	uint64_t entries = 0, rss, expected_offset;
	unsigned char byte, content[5];
	size_t count;
	char directory[] = "iso-scale-XXXXXX";
	int scratch;

	image.entries = shape == WIDE ? MILLION : 1;
	if (shape == DEEP)
		image.entries = tree_depth;
	if (shape == SECTIONS)
		image.entries = (FOUR_TIB + SECTION_BYTES - 1) / SECTION_BYTES;
	image.directory_blocks = shape == DEEP ? image.entries + 1 :
	    1 + (image.entries > 40 ? (image.entries - 40 + 41) / 42 : 0);
	if (shape == WIDE || shape == DEEP)
		image.blocks = ROOT + image.directory_blocks + 1;
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = SCALE_BUDGET;
	limits.max_entries = MILLION + 1;
	limits.max_work = UINT64_C(10000000000);
	limits.max_depth = 256;
	limits.scratch_directory_fd = scratch;
	require_ok(fsimg_ctx_new(&limits, &ctx, &error), &error);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_ctx_get_stats(ctx, &baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	require_ok(fsimg_io_from_callbacks(ctx, &ops, &image, &io, &error), &error);
	fsimg_read_options_init(&options);
	options.iso_view = shape == DEEP ? FSIMG_ISO_VIEW_ROCKRIDGE : FSIMG_ISO_VIEW_PRIMARY;
	require_ok(fsimg_reader_open(ctx, io, &options, &reader, &error), &error);
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		entries++;
		if (shape == CAPACITY) {
			CHECK(entry->size == sizeof(content));
			require_ok(fsimg_reader_read_at(reader, 0, content,
			    sizeof(content), &count, &error), &error);
			CHECK(count == sizeof(content));
			CHECK(bcmp(content, "hello", sizeof(content)) == 0);
			CHECK(image.last_offset == (uint64_t)(UINT32_MAX - 1) * BLOCK);
		}
		if (shape == SECTIONS) {
			CHECK(entry->size == FOUR_TIB);
			expected_offset = (uint64_t)(ROOT + image.directory_blocks) * BLOCK;
			/* Sample both sides of every section boundary and the final byte.
			 * W below checks ownership without reading 4 TiB of zero payload.
			 */
			for (uint64_t at = SECTION_BYTES; at < FOUR_TIB; at += SECTION_BYTES) {
				require_ok(fsimg_reader_read_at(reader, at - 1, &byte, 1,
				    &count, &error), &error);
				CHECK(count == 1 && byte == 0);
				CHECK(image.last_offset == expected_offset + at - 1 +
				    (at / SECTION_BYTES - 1) * BLOCK);
				require_ok(fsimg_reader_read_at(reader, at, &byte, 1,
				    &count, &error), &error);
				CHECK(count == 1 && byte == 0);
				CHECK(image.last_offset == expected_offset + at +
				    (at / SECTION_BYTES) * BLOCK);
			}
			require_ok(fsimg_reader_read_at(reader, FOUR_TIB - 1, &byte, 1,
			    &count, &error), &error);
			CHECK(count == 1 && byte == 0);
			CHECK(image.last_offset == expected_offset + FOUR_TIB - 1 +
			    ((FOUR_TIB - 1) / SECTION_BYTES) * BLOCK);
		}
	}
	if (shape == DEEP && tree_depth == 257) {
		CHECK(status == FSIMG_ELIMIT);
		CHECK(error.resource == FSIMG_RESOURCE_DEPTH);
		CHECK(error.limit == 256 && error.observed == 257);
		CHECK(entries == 256);
	} else {
		if (status != FSIMG_DONE)
			require_ok(status, &error);
		CHECK(entries == (shape == SECTIONS ? 1 : image.entries));
	}
	fsimg_reader_free(reader);
	reader = NULL;
	if (shape != DEEP) {
		require_ok(fsimg_reader_open(ctx, io, &options, &reader, &error), &error);
		require_ok(fsimg_verify(reader, 0, &report, &error), &error);
		CHECK(report.complete && report.files == (shape == SECTIONS ? 1 : image.entries));
	}
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == baseline.descriptors);
	CHECK(stats.memory_peak <= SCALE_BUDGET);
	if (shape == WIDE)
		CHECK(stats.scratch_peak > SCALE_BUDGET);
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
#if defined(__APPLE__)
	rss = usage.ru_maxrss;
#else
	rss = (uint64_t)usage.ru_maxrss * 1024;
#endif
	printf("iso9660 %s tree_depth=%u max_depth=256 entries=%" PRIu64
	    " reads=%" PRIu64 " io_calls=%" PRIu64
	    " metadata_block_reads=%" PRIu64 " metadata_pass_equivalents=%.3f"
	    " scratch_peak=%" PRIu64 " memory_peak=%" PRIu64 " rss=%" PRIu64 "\n",
	    labels[shape], shape == DEEP ? tree_depth : 0, entries, image.reads, stats.io_calls,
	    image.metadata_reads, (double)image.metadata_reads / image.directory_blocks,
	    stats.scratch_peak, stats.memory_peak, rss);
	CHECK(fflush(stdout) == 0);
	fsimg_ctx_free(ctx);
	CHECK(rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (strcmp(argv[1], "--wide") == 0)
		run(WIDE, 0);
	else if (strcmp(argv[1], "--deep") == 0)
		run(DEEP, 256);
	else if (strcmp(argv[1], "--depth-limit") == 0)
		run(DEEP, 257);
	else if (strcmp(argv[1], "--sections") == 0)
		run(SECTIONS, 0);
	else {
		CHECK(strcmp(argv[1], "--capacity") == 0);
		run(CAPACITY, 0);
	}
	return (0);
}
