/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

#define MIB UINT64_C(1048576)
#define FAT_BYTES MIB
#define GPT_ENTRIES 128u
#define DISK_GUID "11111111-2222-4333-8444-555555555555"
#define ESP_GUID "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

#include "layers-fixture.h"

#define APM_LOGICAL_DATA_START 80u
#define APM_DATA_BLOCK_COUNT 84u
#define APM_PARTITION_STATUS 88u

/* A map description separates declared extents from the disk that holds it.
 * Unspecified trailing entries are zero-length slots with valid signatures.
 */
typedef struct apm_entry {
	uint32_t start;
	uint32_t blocks;
	const char *type;
	const char *name;
} apm_entry_t;

typedef struct map_case {
	const char *name;
	uint32_t block;
	uint32_t ddm_block;
	uint32_t ddm_count;
	uint32_t count;
	/* Zero encodes count entries; a nonzero value limits the encoded prefix
	 * for malformed counts and truncated maps. The builder never clips silently.
	 */
	unsigned encoded_entries;
	uint64_t disk_bytes;
	apm_entry_t entries[6];
	unsigned bad_signature;
	unsigned bad_count;
	fsimg_status_t status;
	const char *message;
} map_case_t;

typedef struct fixture {
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_image_t *image;
	uint64_t baseline;
} fixture_t;

static const char *scenario;

#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "APM case: %s\n", scenario); \
		CHECK(!"APM expectation failed: " #condition); \
	} \
} while (0)

static void
be16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)(value >> 8);
	p[1] = (unsigned char)value;
}

static void
be32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(value >> (24 - 8 * i));
}

static memory_disk_t
map_build(const map_case_t *c)
{
	memory_disk_t disk = { .stored = c->disk_bytes, .length = c->disk_bytes };
	unsigned encoded = c->encoded_entries != 0 ? c->encoded_entries : c->count;

	disk.bytes = calloc(1, (size_t)disk.length);
	CHECK(disk.bytes != NULL);
	bcopy("ER", disk.bytes, 2);
	be16(disk.bytes + 2, (uint16_t)c->ddm_block);
	be32(disk.bytes + 4, c->ddm_count);
	/* Encode the requested prefix; every encoded entry must fit the disk. */
	for (unsigned i = 1; i <= encoded; i++) {
		unsigned char *raw;

		CHECK((uint64_t)i * c->block + 512 <= disk.length);
		raw = disk.bytes + i * c->block;
		bcopy(i == c->bad_signature ? "XX" : "PM", raw, 2);
		be32(raw + 4, c->count + (i == c->bad_count));
		if (i > sizeof(c->entries) / sizeof(c->entries[0]))
			continue;
		be32(raw + 8, c->entries[i - 1].start);
		be32(raw + 12, c->entries[i - 1].blocks);
		if (c->entries[i - 1].name != NULL)
			bcopy(c->entries[i - 1].name, raw + 16,
			    strnlen(c->entries[i - 1].name, 32));
		if (c->entries[i - 1].type != NULL)
			bcopy(c->entries[i - 1].type, raw + 48,
			    strnlen(c->entries[i - 1].type, 32));
	}
	return (disk);
}

static map_case_t
ordinary_map(uint32_t block)
{
	return ((map_case_t){ .block = block, .ddm_block = block,
	    .count = 3, .disk_bytes = 4 * MIB, .entries = {
		{ .start = 1, .blocks = 3,
		    .type = "Apple_partition_map", .name = "Apple" },
		{ .start = 64, .blocks = (uint32_t)(MIB / block),
		    .type = "Apple_HFS", .name = "Macintosh" },
		{ .start = UINT32_MAX, .blocks = UINT32_MAX,
		    .type = "Apple_Free", .name = "Free" }
	    } });
}

static fsimg_status_t
disk_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_disk_t *)argument)->length;
	return (FSIMG_OK);
}

static void
fixture_init(fixture_t *f, memory_disk_t *disk)
{
	fsimg_io_ops_t ops;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	CHECK(fsimg_ctx_new(NULL, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline = stats.memory_bytes;
	fsimg_io_ops_init(&ops);
	ops.read_at = disk_read;
	ops.get_size = disk_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, disk, &f->io, NULL) == FSIMG_OK);
}

static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	EXPECT(stats.memory_bytes == f->baseline);
	fsimg_ctx_free(f->ctx);
}

static fsimg_status_t
image_open(fixture_t *f, const char *stack, unsigned flags,
    fsimg_error_t *error)
{
	fsimg_image_options_t options;

	fsimg_image_free(f->image);
	f->image = NULL;
	fsimg_image_options_init(&options);
	options.flags = flags;
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	return (fsimg_image_open(f->ctx, f->io, &options, &f->image, error));
}

static fsimg_status_t
select_partition(fixture_t *f, fsimg_selector_t selector, fsimg_error_t *error)
{
	fsimg_io_t *volume = NULL;
	fsimg_status_t status;

	status = fsimg_image_select(f->image, &selector, &volume, error);
	if (status != FSIMG_OK)
		EXPECT(volume == NULL);
	fsimg_io_free(volume);
	return (status);
}

static void
test_map_validation_reports_corruption(void)
{
	static const map_case_t cases[] = {
		{ .name = "zero count",
		    .count = 0, .encoded_entries = 1,
		    .status = FSIMG_ECORRUPT,
		    .message = "entry count" },
		{ .name = "1025 entries", .count = 1025, .encoded_entries = 1,
		    .status = FSIMG_ECORRUPT, .message = "entry count" },
		{ .name = "1024 entries", .count = 1024 },
		{ .name = "UINT32_MAX entries", .count = UINT32_MAX, .encoded_entries = 1,
		    .status = FSIMG_ECORRUPT, .message = "entry count" },
		{ .name = "map cut short",
		    .count = 1024, .encoded_entries = 15,
		    .disk_bytes = 8192,
		    .status = FSIMG_ECORRUPT, .message = "past the end" },
		{ .name = "later missing PM", .count = 3, .bad_signature = 2,
		    .status = FSIMG_ECORRUPT, .message = "PM signature" },
		{ .name = "later count differs", .count = 3, .bad_count = 3,
		    .status = FSIMG_ECORRUPT, .message = "another entry count" },
		{ .name = "used entry has empty type", .count = 2,
		    .entries = { { .start = 64, .blocks = 8,
		    .type = "", .name = "Data" } },
		    .status = FSIMG_ECORRUPT, .message = "no type" },
		{ .name = "used entry starts at zero", .count = 2,
		    .entries = { { .start = 0, .blocks = 8,
		    .type = "Apple_HFS", .name = "Data" } },
		    .status = FSIMG_ECORRUPT, .message = "driver descriptor" },
		{ .name = "non-map entry inside map", .count = 2,
		    .entries = { { .start = 2, .blocks = 8,
		    .type = "Apple_HFS", .name = "Data" } },
		    .status = FSIMG_ECORRUPT, .message = "partition map" },
		{ .name = "used entries overlap", .count = 2, .entries = {
		    { .start = 64, .blocks = 16,
		    .type = "Apple_HFS", .name = "A" },
		    { .start = 70, .blocks = 16,
		    .type = "Apple_Boot", .name = "B" } },
		    .status = FSIMG_ECORRUPT, .message = "overlap" },
		{ .name = "map declared extent overlaps volume", .count = 2,
		    .entries = { { .start = 1, .blocks = 100,
		    .type = "Apple_partition_map", .name = "Map" },
		    { .start = 64, .blocks = 16,
		    .type = "Apple_HFS", .name = "Data" } },
		    .status = FSIMG_ECORRUPT, .message = "overlap" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		map_case_t c = cases[i];
		memory_disk_t disk;
		fsimg_image_info_t info;
		fsimg_error_t error;
		fixture_t f;
		fsimg_status_t status;

		scenario = c.name;
		c.block = c.ddm_block = 512;
		if (c.disk_bytes == 0)
			c.disk_bytes = 4 * MIB;
		disk = map_build(&c);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		status = fsimg_image_get_info(f.image, &info, &error);
		if (status != c.status)
			fprintf(stderr, "%s: expected %d, got %d (%s)\n",
			    c.name, c.status, status, error.message);
		EXPECT(status == c.status);
		if (c.message != NULL)
			EXPECT(strstr(error.message, c.message) != NULL);
		if (status == FSIMG_OK)
			EXPECT(info.partition_count == c.count);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_block_sizes_preserve_physical_extents(void)
{
	for (unsigned block = 512; block <= 2048; block *= 4) {
		map_case_t c = ordinary_map(block);
		memory_disk_t disk;
		fixture_t f;
		fsimg_image_info_t info;
		fsimg_partition_info_t part;

		scenario = "physical extents ignore DDM size and logical data fields";
		c.ddm_count = UINT32_MAX;
		disk = map_build(&c);
		be32(disk.bytes + 2 * block + APM_LOGICAL_DATA_START, UINT32_MAX);
		be32(disk.bytes + 2 * block + APM_DATA_BLOCK_COUNT, 1);
		be32(disk.bytes + 2 * block + APM_PARTITION_STATUS, UINT32_MAX);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(strcmp(info.scheme, "apm") == 0);
		EXPECT(info.sector_bytes == block && info.partition_count == 3);
		EXPECT(info.missing_bytes == 0);
		for (unsigned index = 1; index <= 3; index++) {
			EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
			EXPECT(part.index == index && part.used == (index <= 2));
			EXPECT(part.missing_bytes == 0);
			if (index == 2) {
				EXPECT(part.offset == 64 * block && part.length == MIB);
				EXPECT(strcmp(part.apm_type, "Apple_HFS") == 0);
				EXPECT(strcmp(part.type, "hfs") == 0);
				EXPECT(strcmp(part.name, "Macintosh") == 0);
			}
			if (index == 1) {
				EXPECT(part.offset == block && part.length == 3 * block);
				EXPECT(strcmp(part.apm_type, "Apple_partition_map") == 0);
				EXPECT(strcmp(part.name, "Apple") == 0);
			}

		}
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_DONE);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_unused_slots_keep_entry_numbers(void)
{
	map_case_t c = ordinary_map(512);
	memory_disk_t disk;
	fsimg_partition_info_t part;
	fixture_t f;

	scenario = "free, void, empty and unspecified slots remain numbered";
	c.count = 6;
	c.entries[0].blocks = 6;
	c.entries[3] = (apm_entry_t){ .start = UINT32_MAX, .blocks = UINT32_MAX,
	    .type = "Apple_Void", .name = "Void" };
	c.entries[4] = (apm_entry_t){ .start = UINT32_MAX, .blocks = 0,
	    .type = "Apple_HFS", .name = "Empty" };
	disk = map_build(&c);
	fixture_init(&f, &disk);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	for (unsigned index = 1; index <= 6; index++) {
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(part.index == index && part.used == (index <= 2));
		if (index >= 3)
			EXPECT(select_partition(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_INDEX, .index = index }, NULL) == FSIMG_EINVAL);
	}
	EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_DONE);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_short_type_zeroes_residue_after_long_type(void)
{
	map_case_t c = ordinary_map(512);
	memory_disk_t disk;
	fsimg_partition_info_t part;
	fixture_t f;

	scenario = "short type after a full-width type has zero padding";
	c.entries[0].type = "12345678901234567890123456789012";
	/* Keep the first entry outside the map because its type is not map machinery. */
	c.entries[0].start = 8;
	c.entries[0].blocks = 8;
	disk = map_build(&c);
	fixture_init(&f, &disk);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
	EXPECT(strcmp(part.apm_type, c.entries[0].type) == 0);
	EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
	EXPECT(strcmp(part.apm_type, "Apple_HFS") == 0);
	/* A reused decoder buffer must not carry the long entry's suffix past
	 * the short entry's terminator, even though its visible text matches.
	 */
	for (size_t i = strlen("Apple_HFS") + 1; i < sizeof(part.apm_type); i++)
		EXPECT(part.apm_type[i] == 0);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_text_fields_replace_unprintable_bytes_and_keep_full_width(void)
{
	static const char *const fields[] = { "A\001\177\200Z",
	    "12345678901234567890123456789012" };
	static const char *const expected[] = { "A???Z",
	    "12345678901234567890123456789012" };

	for (unsigned i = 0; i < 2; i++) {
		map_case_t c = ordinary_map(512);
		fsimg_partition_info_t part;
		memory_disk_t disk;
		fixture_t f;

		scenario = "sanitized and unterminated text fields";
		c.entries[1].name = c.entries[1].type = fields[i];
		disk = map_build(&c);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(strcmp(part.name, expected[i]) == 0);
		EXPECT(strcmp(part.apm_type, expected[i]) == 0);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_entry_location_decides_map_spacing(void)
{
	static const struct {
		const char *name;
		uint32_t ddm;
		uint32_t spacing;
		fsimg_status_t status;
	} cases[] = {
		{ .name = "2048 DDM with 512 entries",
		    .ddm = 2048, .spacing = 512,
		    .status = FSIMG_OK },
		{ .name = "512 DDM does not authorize 2048 entries",
		    .ddm = 512, .spacing = 2048,
		    .status = FSIMG_EFORMAT },
		{ .name = "4096 entries are not probed",
		    .ddm = 4096, .spacing = 4096,
		    .status = FSIMG_EFORMAT }
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		map_case_t c = ordinary_map(cases[i].spacing);
		memory_disk_t disk;
		fsimg_image_info_t info;
		fixture_t f;

		scenario = cases[i].name;
		c.ddm_block = cases[i].ddm;
		disk = map_build(&c);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == cases[i].status);
		if (cases[i].status == FSIMG_OK)
			EXPECT(info.sector_bytes == 512);
		fixture_free(&f);
		free(disk.bytes);
	}

	/* Disc Recording puts matching volume extents at both spacings. Install
	 * media has Void at small-map entries 4 and 8, and a separate nine-entry
	 * map at 6144, beyond the small map's ten entries.
	 */
	for (unsigned install = 0; install < 2; install++) {
		map_case_t small = ordinary_map(512);
		map_case_t large = ordinary_map(2048);
		memory_disk_t disk, twin;
		fsimg_image_info_t info;
		fsimg_partition_info_t part;
		fixture_t f;

		scenario = install ? "install disc dual map" : "Disc Recording dual map";
		small.ddm_block = 2048;
		small.count = install ? 10 : 3;
		small.entries[0].blocks = small.count;
		large.count = install ? 9 : 3;
		large.entries[0].blocks = large.count;
		large.entries[1].start = small.entries[1].start / 4;
		disk = map_build(&small);
		twin = map_build(&large);
		if (install) {
			for (unsigned slot = 4; slot <= 8; slot += 4) {
				unsigned char *raw = disk.bytes + slot * 512;

				be32(raw + 12, 1);
				bcopy("Apple_Void", raw + 48, 11);
			}
		}
		for (unsigned slot = 1; slot <= large.count; slot++)
			bcopy(twin.bytes + slot * 2048,
			    disk.bytes + (slot + (install ? 2 : 0)) * 2048, 512);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(info.sector_bytes == 512 && info.partition_count == small.count);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(part.offset == 64 * 512 && part.length == MIB);
		fixture_free(&f);
		free(twin.bytes);
		free(disk.bytes);
	}
}

static void
test_assumed_identity_waives_only_the_driver_descriptor(void)
{
	map_case_t c = ordinary_map(512);
	memory_disk_t disk = map_build(&c);
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	scenario = "missing ER identity";
	bzero(disk.bytes, 2);
	fixture_init(&f, &disk);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	EXPECT(image_open(&f, "apm", 0, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	EXPECT(image_open(&f, "apm", FSIMG_IMAGE_ASSUME_IDENTITY, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_verify(f.image, 0, &report, &error) == FSIMG_ECORRUPT);
	EXPECT(strstr(error.message, "the disk has no driver descriptor map") != NULL);
	EXPECT(strstr(error.message, "identity was assumed") != NULL);

	scenario = "ER alone cannot locate a map even under assumed identity";
	bcopy("ER", disk.bytes, 2);
	bzero(disk.bytes + 512, 2);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	EXPECT(image_open(&f, "apm", 0, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	EXPECT(image_open(&f, "apm", FSIMG_IMAGE_ASSUME_IDENTITY, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	fixture_free(&f);
	free(disk.bytes);
}

static fsimg_status_t
refuse_write(void *argument FSIMG_UNUSED, uint64_t offset FSIMG_UNUSED,
    const void *bytes FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    size_t *transferred FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	CHECK(!"a clipped partition reached the unwrap sink");
	return (FSIMG_EIO);
}

static void
test_clipped_and_absent_extents_use_64_bit_arithmetic(void)
{
	static const struct {
		const char *name;
		uint32_t start;
		uint32_t blocks;
		uint64_t disk_bytes;
	} cases[] = {
		{ .name = "partial trailing sector",
		    .start = 64, .blocks = 512,
		    .disk_bytes = 700000 },
		{ .name = "start past end", .start = 2048, .blocks = 512, .disk_bytes = MIB },
		{ .name = "UINT32_MAX start and blocks",
		    .start = UINT32_MAX, .blocks = UINT32_MAX,
		    .disk_bytes = MIB },
		{ .name = "UINT32_MAX blocks clips without wrapping",
		    .start = 64, .blocks = UINT32_MAX,
		    .disk_bytes = MIB }
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		map_case_t c = ordinary_map(2048);
		uint64_t start = (uint64_t)cases[i].start * 2048;
		uint64_t declared = (uint64_t)cases[i].blocks * 2048;
		uint64_t kept = start < cases[i].disk_bytes ? cases[i].disk_bytes - start : 0;
		fsimg_io_ops_t ops;
		fsimg_io_t *output;
		fsimg_sink_t sink;
		memory_disk_t disk;
		fixture_t f;
		fsimg_partition_info_t part;
		fsimg_image_info_t info;
		fsimg_layer_report_t report;
		fsimg_error_t error;
		fsimg_selector_t selector = { .kind = FSIMG_SELECT_INDEX, .index = 2 };

		scenario = cases[i].name;
		c.disk_bytes = cases[i].disk_bytes;
		c.entries[1].start = cases[i].start;
		c.entries[1].blocks = cases[i].blocks;
		disk = map_build(&c);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(info.missing_bytes == start + declared - disk.length);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(part.offset == start && part.length == kept);
		EXPECT(part.missing_bytes == declared - kept);
		EXPECT(fsimg_image_verify(f.image, 0, &report, &error) == FSIMG_ECORRUPT);
		EXPECT(strstr(error.message, "truncated") != NULL);
		if (kept == 0)
			EXPECT(select_partition(&f, selector, NULL) == FSIMG_ECORRUPT);
		fsimg_io_ops_init(&ops);
		ops.write_at = refuse_write;
		ops.get_size = disk_size;
		EXPECT(fsimg_io_from_callbacks(f.ctx, &ops, &disk, &output, NULL) == FSIMG_OK);
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = output;
		EXPECT(fsimg_image_unwrap(f.image, &selector, NULL, &sink, &error) == FSIMG_ECORRUPT);
		EXPECT(strstr(error.message, "truncated") != NULL);
		fsimg_io_free(output);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_selection_keeps_order_and_matches_apm_types(void)
{
	static const struct {
		const char *type;
		const char *selector;
	} types[] = {
		{ .type = "Apple_HFS", .selector = "hfs" }, { .type = "Apple_HFS", .selector = "hfsplus" },
		{ .type = "Apple_HFSX", .selector = "hfsx" }, { .type = "Apple_HFS", .selector = "Apple_HFS" },
		{ .type = "Apple_Boot", .selector = "Apple_Boot" }
	};
	static const struct {
		const char *selector;
		const char *message;
	} invalid[] = {
		{ .selector = "0x0c",
		    .message = "an MBR type byte cannot select an APM partition" },
		{ .selector = ESP_GUID,
		    .message = "a GPT type GUID cannot select an APM partition" }
	};

	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		map_case_t c = ordinary_map(512);
		fsimg_image_info_t info;
		fsimg_error_t error;
		memory_disk_t disk;
		fixture_t f;

		scenario = types[i].selector;
		c.entries[1].type = types[i].type;
		disk = map_build(&c);
		make_fat12(disk.bytes + 64 * 512);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_AUTOMATIC }, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(info.selected == 2);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_TYPE, .text = types[i].selector }, NULL) == FSIMG_OK);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_INDEX, .index = 1 }, NULL) == FSIMG_EUNSUPPORTED);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_NAME, .text = "Macintosh" }, &error) == FSIMG_EINVAL);
		EXPECT(strcmp(error.message, "only GPT partitions can be selected by name") == 0);
		for (unsigned j = 0; j < sizeof(invalid) / sizeof(invalid[0]); j++) {
			EXPECT(select_partition(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_TYPE, .text = invalid[j].selector }, &error) == FSIMG_EINVAL);
			EXPECT(strcmp(error.message, invalid[j].message) == 0);
		}
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_automatic_selection_skips_map_driver_and_patches_claims(void)
{
	map_case_t c = { .block = 512, .ddm_block = 512, .count = 4,
	    .disk_bytes = 5 * MIB, .entries = {
		{ .start = 1, .blocks = 2048,
		    .type = "Apple_partition_map", .name = "Map" },
		{ .start = 2049, .blocks = 2048,
		    .type = "Apple_Driver43", .name = "Driver" },
		{ .start = 4097, .blocks = 2048,
		    .type = "Apple_Patches", .name = "Patches" },
		{ .start = 6145, .blocks = 2048,
		    .type = "Apple_HFS", .name = "Volume" }
	    } };
	memory_disk_t disk = map_build(&c);
	fsimg_partition_info_t part;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	scenario = "machinery with ISO claims is skipped";
	for (unsigned i = 0; i < 3; i++)
		bcopy("\000CD001\001", disk.bytes + c.entries[i].start * 512 + 32768, 7);
	make_fat12(disk.bytes + c.entries[3].start * 512);
	fixture_init(&f, &disk);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	EXPECT(select_partition(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC }, NULL) == FSIMG_OK);
	EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	EXPECT(info.selected == 4);
	for (unsigned i = 1; i <= 3; i++) {
		EXPECT(fsimg_image_next_partition(f.image, &part, NULL) == FSIMG_OK);
		EXPECT(part.used && part.claim == FSIMG_CLAIM_NONE);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_INDEX, .index = i }, NULL) == FSIMG_EUNSUPPORTED);
	}
	fixture_free(&f);
	free(disk.bytes);

	scenario = "two candidates require selector and list entry numbers";
	c = ordinary_map(512);
	c.entries[2] = (apm_entry_t){ .start = 4096, .blocks = 2048,
		    .type = "Apple_HFS", .name = "Other" };
	disk = map_build(&c);
	make_fat12(disk.bytes + 64 * 512);
	make_fat12(disk.bytes + 4096 * 512);
	fixture_init(&f, &disk);
	EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
	EXPECT(select_partition(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC }, &error) == FSIMG_EFORMAT);
	EXPECT(strstr(error.message, "2, 3") != NULL);
	EXPECT(select_partition(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_INDEX, .index = 3 }, NULL) == FSIMG_OK);
	fixture_free(&f);
	free(disk.bytes);
}

/* Relocate GPT's primary array beyond the APM system area. A used GPT
 * entry ensures type selection reaches the scheme's type guard.
 */
static void
add_gpt_beside_apm(memory_disk_t *disk, const map_case_t *c)
{
	static const uint16_t name[] = { 'A', 0 };
	const gpt_partition_t part = { .type_guid = ESP_GUID, .name = name,
	    .first_lba = 2048, .last_lba = 4095 };
	memory_disk_t gpt = { .stored = disk->length, .length = disk->length };
	uint64_t last = disk->length / 512 - 1;

	gpt.bytes = calloc(1, (size_t)gpt.length);
	CHECK(gpt.bytes != NULL);
	gpt_build(&gpt, 512, &part, 1);
	bcopy(gpt.bytes + 1024, gpt.bytes + 128 * 512, 16384);
	bzero(gpt.bytes + 1024, 16384);
	put64(gpt.bytes + 512 + 72, 128);
	put64(gpt.bytes + 512 + 40, 160);
	put64(gpt.bytes + last * 512 + 40, 160);
	gpt_reseal(&gpt, 512);
	bcopy(disk->bytes, gpt.bytes, 8);
	for (unsigned slot = 1; slot <= c->count; slot++)
		bcopy(disk->bytes + slot * 2048, gpt.bytes + slot * 2048, 512);
	free(disk->bytes);
	*disk = gpt;
}

static void
test_apm_yields_to_other_schemes_and_preserves_iso_hybrid(void)
{
	static const struct {
		const char *name;
		const char *scheme;
		const char *hybrid;
		bool write_mbr;
		bool gpt;
		bool iso;
		const char *selector;
		const char *selector_error;
	} cases[] = {
		{ .name = "APM beside MBR", .scheme = "mbr", .hybrid = "",
		    .write_mbr = true,
		    .selector = "Apple_HFS",
		    .selector_error = "an APM type cannot select an MBR partition" },
		{ .name = "APM beside GPT", .scheme = "gpt", .hybrid = "", .gpt = true,
		    .selector = "Apple_HFS",
		    .selector_error = "an APM type cannot select a GPT partition" },
		{ .name = "APM beside ISO", .scheme = "", .hybrid = "apm", .iso = true,
		    .selector = "esp",
		    .selector_error = "the partition type has no APM type" },
		{ .name = "APM GPT MBR ISO", .scheme = "", .hybrid = "gpt",
		    .gpt = true, .iso = true,
		    .selector = "Apple_HFS",
		    .selector_error = "an APM type cannot select a GPT partition" }
	};

	CHECK(fsimg_apm_scheme.yields);
	for (size_t i = 0; fsimg_layer_at(i) != NULL; i++) {
		const fsimg_layer_t *layer = fsimg_layer_at(i);

		if (layer->scheme != NULL && layer->scheme->yields)
			CHECK(strcmp(layer->name, "apm") == 0);
	}

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		map_case_t c = ordinary_map(2048);
		memory_disk_t disk = map_build(&c);
		fsimg_image_info_t info;
		fsimg_error_t error;
		fixture_t f;

		scenario = cases[i].name;
		if (cases[i].gpt)
			add_gpt_beside_apm(&disk, &c);
		else if (cases[i].write_mbr)
			mbr_entry(disk.bytes, 0, 0, 0x83, 2048, 2048);
		if (cases[i].iso)
			bcopy("\000CD001\001", disk.bytes + 32768, 7);
		fixture_init(&f, &disk);
		EXPECT(image_open(&f, NULL, 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(strcmp(info.scheme, cases[i].scheme) == 0);
		EXPECT(strcmp(info.hybrid_scheme, cases[i].hybrid) == 0);
		/* ISO's automatic view is bare; force its surveyed table to test guards. */
		if (cases[i].iso)
			EXPECT(image_open(&f, cases[i].hybrid, 0, NULL) == FSIMG_OK);
		EXPECT(select_partition(&f, (fsimg_selector_t){
		    .kind = FSIMG_SELECT_TYPE, .text = cases[i].selector }, &error) == FSIMG_EINVAL);
		EXPECT(strcmp(error.message, cases[i].selector_error) == 0);
		EXPECT(image_open(&f, "apm", 0, NULL) == FSIMG_OK);
		EXPECT(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		EXPECT(strcmp(info.scheme, "apm") == 0 && info.sector_bytes == 2048);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_apm_write_stack_parses_but_planning_is_read_only(void)
{
	map_case_t c = ordinary_map(512);
	memory_disk_t disk = map_build(&c);
	fsimg_stack_t stack;
	fsimg_write_options_t options;
	fsimg_writer_t *writer = NULL;
	const fsimg_plan_t *plan;
	fsimg_status_t status;
	fsimg_error_t error;
	fixture_t f;

	scenario = "APM descriptor and writer planning are read only";
	EXPECT(fsimg_stack_parse("fat32/apm", &stack, NULL) == FSIMG_OK);
	EXPECT(fsimg_stack_parse("hfsplus/apm/dmg", &stack, NULL) == FSIMG_OK);
	EXPECT(!fsimg_scheme_writable(&fsimg_apm_scheme));
	EXPECT(fsimg_scheme_write_set_valid(&fsimg_apm_scheme));
	fixture_init(&f, &disk);
	fsimg_write_options_init(&options);
	options.volume_bytes = 64 * MIB;
	status = fsimg_writer_new(f.ctx, "fat32/apm", &options, &writer, &error);
	if (status == FSIMG_OK)
		status = fsimg_writer_plan(writer, &plan, &error);
	EXPECT(status == FSIMG_EUNSUPPORTED);
	EXPECT(strstr(error.message, "read only") != NULL);
	fsimg_writer_free(writer);
	fixture_free(&f);
	free(disk.bytes);
}

/* Drive deferred open as well as listing: failures in table allocation,
 * map reads and per-volume probes must clean up every charged object.
 */
static fsimg_status_t
open_and_list(fixture_t *f, fsimg_error_t *error)
{
	fsimg_partition_info_t part;
	fsimg_status_t status;

	status = image_open(f, NULL, 0, error);
	if (status != FSIMG_OK)
		return (status);
	while ((status = fsimg_image_next_partition(f->image, &part, error)) == FSIMG_OK)
		continue;
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

static void
test_allocation_and_read_failures_release_open_and_listing_state(void)
{
	map_case_t c = ordinary_map(512);
	memory_disk_t disk = map_build(&c);
	unsigned reads;
	fixture_t f;
	bool reached_success = false;

	scenario = "allocation failure sweep";
	make_fat12(disk.bytes + 64 * 512);
	for (unsigned allocation = 1; allocation < 256; allocation++) {
		fsimg_status_t status;
		fsimg_error_t error;

		fixture_init(&f, &disk);
		fsimg_test_fail_alloc_after(f.ctx, allocation);
		status = open_and_list(&f, &error);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		EXPECT(status == FSIMG_OK || status == FSIMG_ENOMEM);
		if (status != FSIMG_OK)
			EXPECT(error.status == status);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			reached_success = true;
			break;
		}
	}
	EXPECT(reached_success);
	disk.reads = 0;
	fixture_init(&f, &disk);
	EXPECT(open_and_list(&f, NULL) == FSIMG_OK);
	reads = disk.reads;
	fixture_free(&f);
	EXPECT(reads > 3);
	for (unsigned short_read = 0; short_read <= 2; short_read++) {
		static const char *const names[] = {
			"read error sweep", "short read sweep", "premature EOF sweep"
		};

		scenario = names[short_read];
		for (unsigned read = 1; read <= reads; read++) {
			fsimg_status_t status;
			fsimg_error_t error;

			disk.reads = 0;
			disk.fail_read = read;
			disk.short_read = short_read;
			fixture_init(&f, &disk);
			status = open_and_list(&f, &error);
			/* Exact I/O retries a positive partial transfer. Zero progress
			 * before the advertised disk end is an I/O failure.
			 */
			EXPECT(status == (short_read == 1 ? FSIMG_OK : FSIMG_EIO));
			if (status != FSIMG_OK)
				EXPECT(error.status == status);
			else
				EXPECT(disk.reads > read);
			fixture_free(&f);
		}
	}
	free(disk.bytes);
}

int
main(void)
{
	test_map_validation_reports_corruption();
	test_block_sizes_preserve_physical_extents();
	test_unused_slots_keep_entry_numbers();
	test_short_type_zeroes_residue_after_long_type();
	test_text_fields_replace_unprintable_bytes_and_keep_full_width();
	test_entry_location_decides_map_spacing();
	test_assumed_identity_waives_only_the_driver_descriptor();
	test_clipped_and_absent_extents_use_64_bit_arithmetic();
	test_selection_keeps_order_and_matches_apm_types();
	test_automatic_selection_skips_map_driver_and_patches_claims();
	test_apm_yields_to_other_schemes_and_preserves_iso_hybrid();
	test_apm_write_stack_parses_but_planning_is_read_only();
	test_allocation_and_read_failures_release_open_and_listing_state();
	return (0);
}
