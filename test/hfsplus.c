/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "format.h"
#include "hfsplus-gen.h"
#include "hfsplus.h"
#include "reader.h"
#include "test.h"

/* Byte positions from the volume, B-tree and catalog layout tables. */
enum {
	VOLUME_HEADER = 1024,
	VOLUME_ATTRIBUTES = VOLUME_HEADER + 4,
	VOLUME_MOUNTED = VOLUME_HEADER + 8,
	VOLUME_JIB = VOLUME_HEADER + 12,
	VOLUME_FREE_BLOCKS = VOLUME_HEADER + 48,
	VOLUME_NEXT_ID = VOLUME_HEADER + 64,
	NODE_DESCRIPTOR = 14,
	NODE_KIND = 8,
	NODE_HEIGHT = 9,
	NODE_RECORDS = 10,
	TREE_ROOT = 2,
	TREE_FIRST_LEAF = 10,
	TREE_LAST_LEAF = 14,
	TREE_NODE_SIZE = 18,
	TREE_MAX_KEY = 20,
	TREE_TOTAL_NODES = 22,
	FORK_BLOCKS = 12,
	FORK_START = 16,
	FORK_COUNT = 20,
	FILE_DATA_FORK = 88,
	FILE_RESOURCE_FORK = 168,
	JIB_OFFSET = 36,
	JIB_SIZE = 44,
	JOURNAL_CHECKSUM = 36,
	CATALOG_NAME = 8
};

/* Tests collect failures so an engine defect does not hide later cases. */
static unsigned failures;
static unsigned assertions;

typedef struct fixture {
	fsimg_reader_t	 reader;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	const unsigned char *bytes;
	uint64_t	 length;
	uint64_t	 fail_offset;
	uint64_t	 baseline;
	int		 scratch_fd;
	char		 scratch_path[64];
} fixture_t;

typedef struct mutation {
	const char	*name;
	uint64_t	 offset;
	uint64_t	 value;
	unsigned	 width;
	fsimg_status_t	 expected;
	const char	*diagnostic;
} mutation_t;

static void expect_status(const char *, fsimg_status_t, fsimg_status_t,
    const fsimg_error_t *);
static void expect_true(const char *, bool);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static void fixture_init(fixture_t *, const gen_image_t *, uint64_t, uint64_t);
static fsimg_status_t fixture_open(fixture_t *, int, bool, fsimg_error_t *);
static void fixture_close(fixture_t *);
static fsimg_status_t image_open(const gen_image_t *, int, bool);
static fsimg_status_t image_probe(const gen_image_t *);
static void mutate_open(gen_image_t *, const mutation_t *);
static void test_identity_geometry(void);
static void test_headers(void);
static void test_tree_parameters(void);
static void test_tree_search(void);
static void test_tree_faults(void);
static void test_node_framing(void);
static void test_record_key_bounds(void);
static void test_forks(void);
static void test_journals(void);
static void test_journal_size_limits(void);
static void test_labels_info(void);
static void test_faults(void);
static void test_native(void);
static void search_fixture(gen_plan_t *, gen_child_t *, uint16_t [][2],
    size_t, unsigned);
static void check_tree_walk(fixture_t *, gen_image_t *, gen_tree_id_t);
static fsimg_status_t index_child_fork(fixture_t *, const gen_image_t *,
    uint8_t, fsimg_extent_index_t **, fsimg_error_t *);
static const char *json_scalar(const char *, const char *);
static bool json_integer(const char *, const char *, uint64_t *);
static bool native_compare(const char *, const fsimg_hfsplus_reader_t *);

static void
expect_status(const char *name, fsimg_status_t got, fsimg_status_t expected,
    const fsimg_error_t *error)
{
	assertions++;
	if (got == expected)
		return;
	fprintf(stderr, "%s: expected %s, got %s (%s)\n", name,
	    fsimg_status_string(expected), fsimg_status_string(got),
	    error == NULL ? "" : error->message);
	failures++;
}

static void
expect_true(const char *name, bool result)
{
	assertions++;
	if (!result) {
		fprintf(stderr, "%s: assertion failed\n", name);
		failures++;
	}
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *f = argument;

	CHECK(offset <= f->length && length <= f->length - offset);
	*transferred = 0;
	if (f->fail_offset >= offset && f->fail_offset - offset < length)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected HFS Plus read failure"));
	bcopy(f->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static void
fixture_init(fixture_t *f, const gen_image_t *image, uint64_t scratch,
    uint64_t work)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	f->bytes = image->bytes;
	f->length = image->length;
	f->fail_offset = UINT64_MAX;
	f->scratch_fd = -1;
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	if (scratch != 0) {
		options.scratch_bytes = scratch;
		strcpy(f->scratch_path, "hfsplus-scratch-XXXXXX");
		CHECK(mkdtemp(f->scratch_path) != NULL);
		f->scratch_fd = open(f->scratch_path,
		    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(f->scratch_fd != -1);
		options.scratch_directory_fd = f->scratch_fd;
	}
	if (work != 0)
		options.max_work = work;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	if (scratch != 0)
		CHECK(fsimg_test_meta_cache(f->ctx, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline = stats.memory_bytes;
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io,
	    NULL) == FSIMG_OK);
	f->reader.ctx = f->ctx;
	f->reader.input = f->io;
	f->reader.ops = &fsimg_hfsplus_reader_ops;
	fsimg_read_options_init(&f->reader.options);
	fsimg_session_init(&f->reader.session, f->ctx, "test", 0);
}

static fsimg_status_t
fixture_open(fixture_t *f, int forced, bool assume, fsimg_error_t *error)
{
	if (assume)
		f->reader.options.flags |= FSIMG_READ_ASSUME_IDENTITY;
	return (fsimg_hfsplus_reader_ops.open(&f->reader, forced < 0 ? NULL :
	    &fsimg_hfsplus_format.variants[forced], error));
}

static void
fixture_close(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_hfsplus_reader_ops.close(&f->reader);
	/* Closing twice also covers an already cleaned partial open. */
	fsimg_hfsplus_reader_ops.close(&f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	expect_true("close releases all memory and scratch",
	    stats.memory_bytes == f->baseline && stats.scratch_bytes == 0 &&
	    stats.descriptors == (f->scratch_fd == -1 ? 0u : 1u));
	fsimg_ctx_free(f->ctx);
	if (f->scratch_fd != -1) {
		CHECK(close(f->scratch_fd) == 0);
		CHECK(rmdir(f->scratch_path) == 0);
	}
}

static fsimg_status_t
image_open(const gen_image_t *image, int forced, bool assume)
{
	fixture_t f;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	fixture_init(&f, image, 0, 0);
	status = fixture_open(&f, forced, assume, &error);
	fixture_close(&f);
	return (status);
}

static fsimg_status_t
image_probe(const gen_image_t *image)
{
	const fsimg_format_variant_t *variant;
	fsimg_error_t error = { 0 };

	return (fsimg_hfsplus_format.probe(image->bytes,
	    image->length < 1536 ? image->length : 1536, image->length,
	    &variant, &error));
}

static void
mutate_open(gen_image_t *image, const mutation_t *mutation)
{
	unsigned char saved[8];
	fsimg_error_t error = { 0 };
	fixture_t f;
	fsimg_status_t status;

	CHECK(mutation->offset + mutation->width <= image->length);
	CHECK(mutation->width <= sizeof(saved));
	bcopy(image->bytes + mutation->offset, saved, mutation->width);
	gen_store(image->bytes + mutation->offset, mutation->value,
	    mutation->width);
	fixture_init(&f, image, 0, 0);
	status = fixture_open(&f, -1, false, &error);
	expect_status(mutation->name, status, mutation->expected, &error);
	if (mutation->diagnostic != NULL) {
		if (strstr(error.message, mutation->diagnostic) == NULL)
			fprintf(stderr, "%s: diagnostic: %s\n",
			    mutation->name, error.message);
		expect_true(mutation->name,
		    strstr(error.message, mutation->diagnostic) != NULL);
	}
	fixture_close(&f);
	bcopy(saved, image->bytes + mutation->offset, mutation->width);
}

static void
test_identity_geometry(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_header_t header;

	gen_plan_init(&plan);
	for (unsigned x = 0; x < 2; x++) {
		plan.hfsx = x != 0;
		gen_build(&plan, &image);
		expect_status("signature/version pair probe",
		    image_probe(&image), FSIMG_OK, NULL);
		expect_status("signature/version pair open", image_open(&image,
		    -1, false), FSIMG_OK, NULL);
		expect_status("forced opposite variant", image_open(&image,
		    x == 0 ? 1 : 0, false), FSIMG_EFORMAT, NULL);
		gen_store(image.bytes + 1026, x == 0 ? 5 : 4, 2);
		expect_status("wrong signature/version probe",
		    image_probe(&image), FSIMG_EUNSUPPORTED, NULL);
		expect_status("wrong signature/version open", image_open(&image,
		    -1, false), FSIMG_EUNSUPPORTED, NULL);
		gen_free(&image);
	}
	gen_plan_init(&plan);
	gen_build(&plan, &image);
	image.bytes[0] = 'L';
	image.bytes[1] = 'K';
	expect_status("LK boot probe", image_probe(&image), FSIMG_OK, NULL);
	expect_status("LK boot open", image_open(&image, -1, false), FSIMG_OK,
	    NULL);
	image.bytes[0] = 'X';
	expect_status("bad boot probe", image_probe(&image), FSIMG_EFORMAT,
	    NULL);
	/* Reader identity contract: a lost signature is corruption. */
	expect_status("bad boot open", image_open(&image, -1, false),
	    FSIMG_ECORRUPT, NULL);
	fixture_init(&f, &image, 0, 0);
	expect_status("assume identity", fixture_open(&f, 0, true, &error),
	    FSIMG_OK, &error);
	expect_true("assumption recorded", f.reader.assumed_identity);
	fixture_close(&f);
	image.bytes[0] = image.bytes[1] = 0;
	unsigned char *extended = realloc(image.bytes, image.length + 1024);
	CHECK(extended != NULL);
	image.bytes = extended;
	bzero(image.bytes + image.length, 1024);
	image.length++;
	expect_status("partial sector probe ignores slice alignment",
	    image_probe(&image), FSIMG_OK, NULL);
	expect_status("partial sector open", image_open(&image, -1, false),
	    FSIMG_ECORRUPT, NULL);
	image.length = 1535;
	expect_status("short probe", image_probe(&image), FSIMG_EFORMAT, NULL);
	expect_status("short open", image_open(&image, -1, false),
	    FSIMG_EFORMAT, NULL);
	image.length = (size_t)plan.block_size * plan.total_blocks;
	gen_store(image.bytes + VOLUME_HEADER, 0x4244, 2);
	gen_store(image.bytes + VOLUME_HEADER + 124, 0x482b, 2);
	expect_status("wrapper probe", image_probe(&image), FSIMG_OK, NULL);
	expect_status("wrapper open", image_open(&image, -1, false),
	    FSIMG_EUNSUPPORTED, NULL);
	expect_status("forced hfsx wrapper", image_open(&image, 1, false),
	    FSIMG_EFORMAT, NULL);
	gen_store(image.bytes + VOLUME_HEADER + 124, 0, 2);
	expect_status("classic HFS probe", image_probe(&image), FSIMG_EFORMAT,
	    NULL);
	/* Reader identity contract: a lost signature is corruption. */
	expect_status("classic HFS open", image_open(&image, -1, false),
	    FSIMG_ECORRUPT, NULL);
	gen_free(&image);

	/* Header arithmetic is tested without allocating a volume of that size.
	 */
	header = (hfsplus_header_t){ .signature = 0x482b, .version = 4,
	    .total_blocks = 2 };
	for (uint32_t b = 512;; b *= 2) {
		header.block_size = b;
		expect_status("header legal power of two",
		    hfsplus_header_check(&header,
		    (uint64_t)b * 2, &error), FSIMG_OK, &error);
		if (b == UINT32_C(0x80000000))
			break;
	}
	const uint32_t bad_sizes[] = { 0, 511, 513 };
	for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++) {
		header.block_size = bad_sizes[i];
		expect_status("header invalid block size",
		    hfsplus_header_check(&header,
		    UINT64_MAX, &error), FSIMG_ECORRUPT, &error);
	}
	header.block_size = 512;
	header.total_blocks = 0;
	expect_status("zero blocks", hfsplus_header_check(&header, 4096,
	    &error), FSIMG_ECORRUPT, &error);
	header.total_blocks = 9;
	expect_status("blocks exceed slice", hfsplus_header_check(&header, 4096,
	    &error), FSIMG_ECORRUPT, &error);
	/* Both operands are uint32_t: their product cannot overflow uint64_t.
	 * The largest encoded product is still rejected against a small slice.
	 */
	header.total_blocks = UINT32_MAX;
	header.block_size = UINT32_C(0x80000000);
	expect_status("largest encoded product", hfsplus_header_check(&header,
	    4096, &error), FSIMG_ECORRUPT, &error);
	uint64_t product = (uint64_t)header.total_blocks * header.block_size;
	expect_status("maximum product fits exactly",
	    hfsplus_header_check(&header,
	    product, &error), FSIMG_OK, &error);
	expect_status("maximum product exceeds slice by one",
	    hfsplus_header_check(&header,
	    product - 1, &error), FSIMG_ECORRUPT, &error);
	for (uint32_t b = 512; b <= 4096; b *= 2) {
		gen_plan_init(&plan);
		plan.block_size = b;
		plan.total_blocks = 2048;
		gen_build(&plan, &image);
		fixture_init(&f, &image, 0, 0);
		expect_status("reserved block geometry", fixture_open(&f, -1,
		    false, &error), FSIMG_OK, &error);
		if (f.reader.driver != NULL) {
			fsimg_hfsplus_reader_t *state = f.reader.driver;

			expect_true("first reserved block",
			    state->geometry.first_block == (1536 + b - 1) / b);
			expect_true("last content block",
			    state->geometry.end_block == (image.length - 1024) /
			    b);
		}
		fixture_close(&f);
		gen_free(&image);
	}
}

static void
test_headers(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	expect_status("header baseline", image_open(&image, -1, false),
	    FSIMG_OK, NULL);
	const mutation_t cases[] = {
		{ "expanded timestamps", VOLUME_ATTRIBUTES, 1u << 29, 4,
		    FSIMG_EUNSUPPORTED, NULL },
		{ "content protection", VOLUME_ATTRIBUTES, 1u << 30, 4, FSIMG_EUNSUPPORTED, NULL },
		{ "unknown attribute", VOLUME_ATTRIBUTES, 1, 4, FSIMG_EUNSUPPORTED, NULL },
		{ "too many free blocks", VOLUME_FREE_BLOCKS, 513, 4, FSIMG_ECORRUPT, NULL },
		{ "reserved next ID", VOLUME_NEXT_ID, 15, 4, FSIMG_ECORRUPT, NULL }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		mutate_open(&image, &cases[i]);
	for (unsigned bit = 7; bit <= 31; bit++) {
		if (bit > 15 && bit != 31)
			continue;
		if (bit == 13)
			continue;
		gen_store(image.bytes + VOLUME_ATTRIBUTES, UINT32_C(1) << bit, 4);
		fixture_init(&f, &image, 0, 0);
		expect_status("single accepted attribute bit", fixture_open(&f,
		    -1, false, &error), FSIMG_OK, &error);
		expect_true("single-bit clean flag",
		    f.reader.info.clean == (bit == 8));
		fixture_close(&f);
	}
	gen_store(image.bytes + VOLUME_ATTRIBUTES, 0x100, 4);
	for (unsigned bit = 11; bit <= 14; bit += 3) {
		gen_store(image.bytes + VOLUME_ATTRIBUTES, 0x100u | (1u << bit), 4);
		fixture_init(&f, &image, 0, 0);
		expect_status("inconsistent clean-bit volume opens",
		    fixture_open(&f, -1, false, &error), FSIMG_OK, &error);
		expect_true("inconsistency overrides unmounted bit",
		    !f.reader.info.clean);
		fixture_close(&f);
	}
	gen_store(image.bytes + VOLUME_ATTRIBUTES, 0x100, 4);
	gen_store(image.bytes + VOLUME_MOUNTED, 0x46534b21, 4);
	fixture_init(&f, &image, 0, 0);
	expect_status("failed replay marker", fixture_open(&f, -1, false,
	    &error), FSIMG_OK, &error);
	expect_true("failed replay unclean", !f.reader.info.clean);
	fixture_close(&f);
	gen_store(image.bytes + VOLUME_MOUNTED, 0, 4);
	const unsigned required[] = { GEN_BITMAP_FORK, GEN_EXTENTS_FORK,
	    GEN_CATALOG_FORK };
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		unsigned char saved[80];

		bcopy(image.bytes + VOLUME_HEADER + required[i], saved, 80);
		bzero(image.bytes + VOLUME_HEADER + required[i], 80);
		fixture_init(&f, &image, 0, 0);
		expect_status("missing required fork", fixture_open(&f, -1,
		    false, &error), FSIMG_ECORRUPT, &error);
		if (required[i] == GEN_CATALOG_FORK)
			expect_true("missing catalog diagnostic",
			    strstr(error.message, "catalog is missing") != NULL);
		fixture_close(&f);
		bcopy(saved, image.bytes + VOLUME_HEADER + required[i], 80);
		uint64_t fork = 1024 + required[i];
		const mutation_t faults[] = {
			{ "special fork nonzero start zero count", fork + 20, 0,
			    4, FSIMG_ECORRUPT, NULL },
			{ "special fork reserved extent", fork + 16, 0, 4,
			    FSIMG_ECORRUPT, NULL },
			{ "special fork past volume", fork + 16,
			    plan.total_blocks, 4, FSIMG_ECORRUPT, NULL },
			{ "special fork size above allocation", fork,
			    UINT64_MAX, 8, FSIMG_ECORRUPT, NULL },
			{ "special fork huge block claim", fork + 12,
			    UINT32_MAX, 4, FSIMG_ECORRUPT, NULL }
		};
		for (size_t j = 0; j < sizeof(faults) / sizeof(faults[0]); j++)
			mutate_open(&image, &faults[j]);
	}
	gen_free(&image);
}

static void
test_tree_parameters(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	for (unsigned t = 0; t < GEN_TREE_COUNT; t++) {
		for (unsigned bound = 0; bound < 2; bound++) {
			gen_plan_init(&plan);
			plan.attributes = true;
			plan.node_size[t] = bound == 1 ? 32768 : (t ==
			    GEN_EXTENTS ? 512 : 4096);
			static const uint16_t name[] = { 'f' };
			gen_child_t child = { .name = name, .units = 1,
			    .id = 32, .data_extents = 9 };
			static const gen_attribute_t attribute = { 32, 0, 'a' };
			plan.attribute_records = &attribute;
			plan.attribute_count = 1;
			plan.children = &child;
			plan.child_count = 1;
			gen_build(&plan, &image);
			expect_status("tree node size bound", image_open(&image,
			    -1, false), FSIMG_OK, NULL);
			uint64_t h = gen_node_offset(&image, (gen_tree_id_t)t,
			    0) + 14;
			uint64_t fork = 1024 + (t == GEN_EXTENTS ?
			    GEN_EXTENTS_FORK :
			    (t == GEN_CATALOG ? GEN_CATALOG_FORK :
			    GEN_ATTRIBUTES_FORK));
			const mutation_t cases[] = {
				{ "node below minimum", h + TREE_NODE_SIZE,
				    t == GEN_EXTENTS ? 256 : 2048, 2,
				    FSIMG_EUNSUPPORTED, NULL },
				{ "node size not power two", h + TREE_NODE_SIZE, 513, 2,
				    FSIMG_ECORRUPT, NULL },
				{ "wrong max key", h + TREE_MAX_KEY, 11, 2,
				    FSIMG_EUNSUPPORTED, NULL },
				{ "wrong B-tree type", h + 36, 1, 1,
				    FSIMG_EUNSUPPORTED, NULL },
				{ "unknown compare", h + 37, 1, 1,
				    FSIMG_EUNSUPPORTED, NULL },
				{ "depth seventeen", h, 17, 2, FSIMG_ECORRUPT,
				    "tree is too deep" },
				{ "first leaf zero", h + TREE_FIRST_LEAF, 0, 4, FSIMG_ECORRUPT,
				    "root or leaf ends lie outside it" },
				{ "first leaf at totalNodes", h + TREE_FIRST_LEAF,
				    image.trees[t].nodes, 4, FSIMG_ECORRUPT,
				    "root or leaf ends lie outside it" },
				{ "last leaf zero", h + TREE_LAST_LEAF, 0, 4, FSIMG_ECORRUPT,
				    "root or leaf ends lie outside it" },
				{ "last leaf at totalNodes", h + TREE_LAST_LEAF,
				    image.trees[t].nodes, 4, FSIMG_ECORRUPT,
				    "root or leaf ends lie outside it" },
				{ "root at totalNodes", h + TREE_ROOT,
				    image.trees[t].nodes, 4, FSIMG_ECORRUPT, NULL },
				{ "wrong totalNodes", h + TREE_TOTAL_NODES,
				    image.trees[t].nodes + 1, 4,
				    FSIMG_ECORRUPT, NULL },
				{ "fork shorter than declared nodes", fork,
				    gen_load(image.bytes + fork, 8) - 1, 8,
				    FSIMG_ECORRUPT, NULL },
				{ "header kind", h - 6, 255, 1,
				    FSIMG_ECORRUPT, NULL },
				{ "header height", h - 5, 1, 1,
				    FSIMG_ECORRUPT, NULL },
				{ "header record count", h - 4, 2, 2,
				    FSIMG_ECORRUPT, NULL }
			};
			for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]);
			    i++)
				mutate_open(&image, &cases[i]);
			unsigned char saved_fork[80];
			uint32_t blocks = (uint32_t)gen_load(image.bytes +
			    fork + 12, 4);
			bcopy(image.bytes + fork, saved_fork, 80);
			gen_store(image.bytes + fork + 12, blocks - 1, 4);
			gen_store(image.bytes + fork + 20, blocks - 1, 4);
			if (blocks == 1)
				gen_store(image.bytes + fork + 16, 0, 4);
			expect_status(
			    "tree allocation shorter than logical size",
			    image_open(&image, -1, false), FSIMG_ECORRUPT,
			    NULL);
			bcopy(saved_fork, image.bytes + fork, 80);
			for (unsigned bit = 1; bit < 32; bit++) {
				uint32_t flags = t == GEN_EXTENTS ? 2 : 6;
				char name[96];
				mutation_t m = { "tree flag outside table",
				    h + 38,
				    flags ^ (UINT32_C(1) << bit), 4,
				    FSIMG_EUNSUPPORTED, NULL };

				snprintf(name, sizeof(name),
				    "tree=%u size=%u flag bit=%u",
				    t, plan.node_size[t], bit);
				m.name = name;
				mutate_open(&image, &m);
			}
			gen_store(image.bytes + h + 38,
			    t == GEN_EXTENTS ? 3 : 7, 4);
			fixture_init(&f, &image, 0, 0);
			expect_status("bad close accepted", fixture_open(&f, -1,
			    false, &error), FSIMG_OK, &error);
			expect_true("bad close unclean", !f.reader.info.clean);
			fixture_close(&f);
			gen_free(&image);
		}
	}
	for (unsigned x = 0; x < 2; x++) {
		const unsigned types[] = { 0, 0xcf, 0xbc };

		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
			gen_plan_init(&plan);
			plan.hfsx = x != 0;
			plan.compare_type = (uint8_t)types[i];
			gen_build(&plan, &image);
			fixture_init(&f, &image, 0, 0);
			bool accepted = x == 0 ? types[i] != 0xbc : types[i] !=
			    0;
			expect_status("catalog compare table", fixture_open(&f,
			    -1, false, &error),
			    accepted ? FSIMG_OK : FSIMG_EUNSUPPORTED, &error);
			if (accepted)
				expect_true("profile string",
				    strcmp(f.reader.info.profile,
				    x == 0 ? "HFS Plus" : (types[i] == 0xbc ?
				    "HFSX case-sensitive" :
				    "HFSX case-insensitive")) == 0);
			fixture_close(&f);
			gen_free(&image);
		}
	}
}

/* Twelve ASCII children leave gaps between names. A fanout of two produces
 * depth three for eight records, while full packing produces depth one.
 */
static void
search_fixture(gen_plan_t *plan, gen_child_t *children, uint16_t names[][2],
    size_t count, unsigned fanout)
{
	gen_plan_init(plan);
	for (size_t i = 0; i < count; i++) {
		names[i][0] = 'b' + (uint16_t)(2 * i);
		names[i][1] = 0;
		children[i] = (gen_child_t){ .name = names[i], .units = 1,
		    .id = 32 + (uint32_t)i, .data_extents = 17 };
	}
	plan->children = children;
	plan->child_count = count;
	plan->records_per_node = (uint16_t)fanout;
	plan->node_size[GEN_EXTENTS] = 4096;
}

static void
check_tree_walk(fixture_t *f, gen_image_t *image, gen_tree_id_t id)
{
	fsimg_hfsplus_reader_t *state = f->reader.driver;
	hfsplus_tree_t *tree = id == GEN_CATALOG ? &state->catalog :
	    &state->extents;
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_key_t key = { 0 };
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	size_t count = 0;

	if (id == GEN_CATALOG)
		CHECK(hfsplus_catalog_key_init(&key, 0, NULL, 0, &error) ==
		    FSIMG_OK);
	hfsplus_cursor_init(&cursor, tree, tree->search_node);
	status = hfsplus_btree_search(&cursor, &key, &error);
	expect_status("lower bound below all records", status, FSIMG_OK,
	    &error);
	while (status == FSIMG_OK && cursor.valid) {
		status = hfsplus_cursor_record(&cursor, &record, &error);
		if (status != FSIMG_OK)
			break;
		CHECK(count < image->trees[id].record_count);
		const gen_record_t *expected = &image->trees[id].records[count];
		expect_true("walk matches independent sorted list",
		    record.key_bytes ==
		    expected->key_length && bcmp(record.key, expected->bytes,
		    expected->key_length) == 0 && record.data_bytes ==
		    expected->length - expected->key_length);
		count++;
		status = hfsplus_cursor_next(&cursor, &error);
	}
	expect_status("whole tree walk", status, FSIMG_OK, &error);
	expect_true("walk count", count == image->trees[id].record_count);
}

static void
test_tree_search(void)
{
	gen_child_t children[12];
	uint16_t names[12][2];
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_cursor_t cursor;
	hfsplus_key_t key;
	hfsplus_record_t record;
	bool found;

	const unsigned fanouts[] = { 0, 4, 2 };
	for (size_t depth = 0; depth < 3; depth++) {
		search_fixture(&plan, children, names, 4, fanouts[depth]);
		/* Four children plus two root records: fanout two gives depth
		 * three. */
		gen_build(&plan, &image);
		expect_true("catalog depth control",
		    image.trees[GEN_CATALOG].depth == depth + 1);
		expect_true("extents depth control",
		    image.trees[GEN_EXTENTS].depth == depth + 1);
		fixture_init(&f, &image, 0, 0);
		if (fixture_open(&f, -1, false, &error) != FSIMG_OK) {
			expect_true("search fixture opens", false);
			fixture_close(&f);
			gen_free(&image);
			continue;
		}
		check_tree_walk(&f, &image, GEN_CATALOG);
		check_tree_walk(&f, &image, GEN_EXTENTS);
		fsimg_hfsplus_reader_t *state = f.reader.driver;
		hfsplus_cursor_init(&cursor, &state->catalog,
		    state->catalog.search_node);
		for (unsigned i = 0; i < 4; i++) {
			unsigned char name[2];

			gen_store(name, names[i][0], 2);
			CHECK(hfsplus_catalog_key_init(&key, 2, name, 1,
			    &error) == FSIMG_OK);
			expect_status("catalog exact find",
			    hfsplus_btree_find_equivalent(&cursor, &key, &found, &error),
			    FSIMG_OK, &error);
			expect_true("catalog exact hit", found);
			gen_store(name, names[i][0] - 1, 2);
			CHECK(hfsplus_catalog_key_init(&key, 2, name, 1,
			    &error) == FSIMG_OK);
			expect_status("catalog between keys",
			    hfsplus_btree_search(&cursor, &key, &error),
			    FSIMG_OK, &error);
			expect_true("lower bound valid", cursor.valid);
			if (cursor.valid) {
				expect_status("lower bound record",
				    hfsplus_cursor_record(&cursor, &record,
				    &error), FSIMG_OK, &error);
				expect_true("lower bound name",
				    gen_load(record.key + 8, 2) == names[i][0]);
			}
			expect_status("catalog exact miss",
			    hfsplus_btree_find_equivalent(&cursor, &key, &found, &error),
			    FSIMG_OK, &error);
			expect_true("catalog exact absent", !found);
		}
		CHECK(hfsplus_catalog_key_init(&key, UINT32_MAX, NULL, 0,
		    &error) == FSIMG_OK);
		expect_status("above all catalog keys",
		    hfsplus_btree_search(&cursor, &key, &error), FSIMG_OK,
		    &error);
		expect_true("above all invalid", !cursor.valid);
		CHECK(hfsplus_catalog_key_init(&key, 2, NULL, 0,
		    &error) == FSIMG_OK);
		expect_status("empty thread key", hfsplus_btree_find_equivalent(&cursor,
		    &key, &found, &error), FSIMG_OK, &error);
		expect_true("thread exact hit", found);
		hfsplus_cursor_init(&cursor, &state->extents,
		    state->extents.search_node);
		for (unsigned i = 0; i < 4; i++) {
			key.extents = (hfsplus_ekey_t){ .file_id = 32 + i,
			    .start_block = 8 };
			expect_status("extents exact find",
			    hfsplus_btree_find_equivalent(&cursor, &key, &found, &error),
			    FSIMG_OK, &error);
			expect_true("extents hit", found);
			key.extents.start_block = 9;
			expect_status("extents lower bound between records",
			    hfsplus_btree_search(&cursor, &key, &error),
			    FSIMG_OK, &error);
			expect_true("extents lower bound valid", cursor.valid);
			if (cursor.valid) {
				expect_status("extents lower bound record",
				    hfsplus_cursor_record(&cursor, &record,
				    &error), FSIMG_OK, &error);
				expect_true("extents next logical key",
				    gen_load(record.key + 8, 4) == 16);
			}
			expect_status("extents miss",
			    hfsplus_btree_find_equivalent(&cursor, &key, &found, &error),
			    FSIMG_OK, &error);
			expect_true("extents exact absent", !found);
		}
		key.extents.file_id = UINT32_MAX;
		expect_status("extents above all", hfsplus_btree_search(&cursor,
		    &key, &error), FSIMG_OK, &error);
		expect_true("extents above all invalid", !cursor.valid);
		fixture_close(&f);
		gen_free(&image);
	}
}

static void
test_tree_faults(void)
{
	gen_child_t children[12];
	uint16_t names[12][2];
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_node_t node;
	const unsigned char *bytes;
	size_t length;

	search_fixture(&plan, children, names, 4, 2);
	gen_build(&plan, &image);
	expect_status("tree fault baseline", image_open(&image, -1, false),
	    FSIMG_OK, NULL);
	uint64_t root = gen_node_offset(&image, GEN_CATALOG,
	    image.trees[GEN_CATALOG].root);
	uint64_t leaf = gen_node_offset(&image, GEN_CATALOG, 1);
	uint64_t h = gen_node_offset(&image, GEN_CATALOG, 0);
	uint32_t size = plan.node_size[GEN_CATALOG];
	uint64_t child = root + NODE_DESCRIPTOR +
	    gen_load(image.bytes + root + NODE_DESCRIPTOR, 2) + 2;
	uint64_t child_node = gen_node_offset(&image, GEN_CATALOG,
	    (uint32_t)gen_load(image.bytes + child, 4));
	const mutation_t cases[] = {
		{ "leaf offset zero not 14", leaf + size - 2, 16, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "offset odd", leaf + size - 4, 101, 2, FSIMG_ECORRUPT, NULL },
		{ "offsets overlap", leaf + size - 4, 14, 2, FSIMG_ECORRUPT, NULL },
		{ "offsets out of order", leaf + size - 4, 12, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "offset beyond node", leaf + size - 6, size + 2, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "record overlaps offset table", leaf + size - 6, size - 4, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "record count exceeds node", leaf + NODE_RECORDS, 65535, 2,
		    FSIMG_ECORRUPT, "does not fit it" },
		{ "empty leaf", leaf + NODE_RECORDS, 0, 2, FSIMG_ECORRUPT,
		    "holds no records" },
		{ "empty index", root + NODE_RECORDS, 0, 2, FSIMG_ECORRUPT,
		    "holds no records" },
		{ "key longer than record", leaf + NODE_DESCRIPTOR, 500, 2, FSIMG_ECORRUPT, NULL },
		{ "key exceeds maxKeyLength", leaf + NODE_DESCRIPTOR, 518, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "catalog key name length disagrees", leaf + 20, 255, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "child is header", child, 0, 4, FSIMG_ECORRUPT, NULL },
		{ "child is itself", child, image.trees[GEN_CATALOG].root, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "child at totalNodes", child, image.trees[GEN_CATALOG].nodes,
		    4, FSIMG_ECORRUPT, NULL },
		{ "child past totalNodes", child,
		    image.trees[GEN_CATALOG].nodes + 1, 4, FSIMG_ECORRUPT, NULL },
		{ "child wrong height", child, 1, 4, FSIMG_ECORRUPT, NULL },
		{ "child equal height", child_node + NODE_HEIGHT,
		    image.trees[GEN_CATALOG].depth, 1, FSIMG_ECORRUPT, NULL },
		{ "index data not four bytes", root + size - 4,
		    gen_load(image.bytes + root + size - 4, 2) + 2, 2,
		    FSIMG_ECORRUPT, "not a child number" },
		{ "header offsets misplaced", h + size - 4, 118, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "depth zero with root", h + 14, 0, 2, FSIMG_ECORRUPT, NULL }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		mutate_open(&image, &cases[i]);

	/* Reading a map node uses the caller-validated table, not a fixed
	 * length. */
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	unsigned char saved[4096];
	bcopy(image.bytes + leaf, saved, size);
	bzero(image.bytes + leaf, size);
	image.bytes[leaf + NODE_KIND] = 2;
	gen_store(image.bytes + leaf + NODE_RECORDS, 1, 2);
	gen_store(image.bytes + leaf + size - 2, 14, 2);
	gen_store(image.bytes + leaf + size - 4, 46, 2);
	expect_status("short map record", hfsplus_node_read(&state->catalog, 1,
	    state->catalog.search_node, 2, 0, &node, &error), FSIMG_OK, &error);
	hfsplus_node_record(&state->catalog, state->catalog.search_node, 0,
	    &bytes, &length);
	expect_true("map length from table", length == 32);
	gen_store(image.bytes + leaf + size - 4, 13, 2);
	expect_status("map malformed table", hfsplus_node_read(&state->catalog,
	    1,
	    state->catalog.search_node, 2, 0, &node, &error), FSIMG_ECORRUPT,
	    &error);
	bcopy(saved, image.bytes + leaf, size);

	/* The two-leaf cycle has consistent links, so only the visit bound
	 * stops it. Reset the search between the backlink and cycle cases.
	 */
	hfsplus_cursor_t cursor;
	hfsplus_key_t key = { 0 };
	CHECK(hfsplus_catalog_key_init(&key, 0, NULL, 0, &error) == FSIMG_OK);
	uint64_t second = gen_node_offset(&image, GEN_CATALOG, 2);
	gen_store(image.bytes + second + 4, 0, 4);
	hfsplus_cursor_init(&cursor, &state->catalog,
	    state->catalog.search_node);
	CHECK(hfsplus_btree_search(&cursor, &key, &error) == FSIMG_OK);
	CHECK(hfsplus_cursor_next(&cursor, &error) == FSIMG_OK);
	expect_status("broken backlink", hfsplus_cursor_next(&cursor, &error),
	    FSIMG_ECORRUPT, &error);
	gen_store(image.bytes + second + 4, 1, 4);
	gen_store(image.bytes + second, 1, 4);
	gen_store(image.bytes + leaf + 4, 2, 4);
	hfsplus_cursor_init(&cursor, &state->catalog,
	    state->catalog.search_node);
	CHECK(hfsplus_btree_search(&cursor, &key, &error) == FSIMG_OK);
	fsimg_status_t status = FSIMG_OK;
	for (unsigned steps = 0; status == FSIMG_OK && steps < 100; steps++)
		status = hfsplus_cursor_next(&cursor, &error);
	expect_status("consistent sibling cycle terminates", status,
	    FSIMG_ECORRUPT, &error);
	expect_true("sibling cycle reaches visit bound",
	    cursor.leaves == state->catalog.header.total_nodes);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_node_framing(void)
{
	gen_child_t children[12];
	uint16_t names[12][2];
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_node_t node;
	unsigned char saved[2];

	search_fixture(&plan, children, names, 4, 4);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	uint64_t leaf = gen_node_offset(&image, GEN_CATALOG, 1);
	uint32_t size = plan.node_size[GEN_CATALOG];
	uint16_t records = (uint16_t)gen_load(image.bytes + leaf + NODE_RECORDS, 2);
	uint64_t last = leaf + size - 2 * (records + 1);
	CHECK(records >= 3);
	/* Offset[2] descends, but the first record still frames its valid key.
	 * Only node framing runs here; no record decoder can mask the fault.
	 */
	const mutation_t cases[] = {
		{ "node read descending middle offset", leaf + size - 6,
		    gen_load(image.bytes + leaf + size - 4, 2) - 2, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "node read odd offset", leaf + size - 4,
		    gen_load(image.bytes + leaf + size - 4, 2) + 1, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "node read first offset not 14", leaf + size - 2, 16, 2,
		    FSIMG_ECORRUPT, NULL },
		{ "node read last offset into table", last,
		    size - 2 * (records + 1) + 2, 2, FSIMG_ECORRUPT, NULL },
		{ "node read empty leaf", leaf + NODE_RECORDS, 0, 2,
		    FSIMG_ECORRUPT, "holds no records" },
		{ "node read record count too large", leaf + NODE_RECORDS, 65535, 2,
		    FSIMG_ECORRUPT, "does not fit it" }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const mutation_t *m = &cases[i];

		bcopy(image.bytes + m->offset, saved, m->width);
		gen_store(image.bytes + m->offset, m->value, m->width);
		expect_status(m->name, hfsplus_node_read(&state->catalog, 1,
		    state->catalog.search_node, HFSPLUS_NODE_LEAF, 1, &node,
		    &error), m->expected, &error);
		if (m->diagnostic != NULL)
			expect_true(m->name, strstr(error.message,
			    m->diagnostic) != NULL);
		bcopy(saved, image.bytes + m->offset, m->width);
	}
	expect_status("node at totalNodes", hfsplus_node_read(
	    &state->catalog, state->catalog.header.total_nodes,
	    state->catalog.search_node, HFSPLUS_NODE_LEAF, 1, &node,
	    &error), FSIMG_ECORRUPT, &error);
	expect_true("node number bound diagnostic",
	    strstr(error.message, "lies outside its tree") != NULL);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_record_key_bounds(void)
{
	gen_child_t children[12];
	uint16_t names[12][2];
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_key_t key = { 0 };
	unsigned char saved[4096];
	const uint16_t key_lengths[] = { 500, 518 };
	const char *leaf_cases[] = {
		"leaf key beyond record with agreeing name length",
		"leaf key beyond maxKeyLength with agreeing name length"
	};
	const char *index_cases[] = {
		"index key beyond record with agreeing name length",
		"index key beyond maxKeyLength with agreeing name length"
	};
	const char *bounds_message =
	    "HFS Plus key is longer than its record or its tree allows";

	search_fixture(&plan, children, names, 4, 2);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	hfsplus_tree_t *tree = &state->catalog;
	CHECK(hfsplus_catalog_key_init(&key, 0, NULL, 0, &error) == FSIMG_OK);
	uint32_t size = tree->header.node_size;
	for (unsigned index = 0; index < 2; index++) {
		uint32_t number = index == 0 ? 1 : tree->header.root_node;
		uint64_t location = gen_node_offset(&image, GEN_CATALOG,
		    number);

		bcopy(image.bytes + location, saved, size);
		for (unsigned fault = 0; fault < 2; fault++) {
			uint16_t key_length = key_lengths[fault];

			/* Both lengths agree and the key stays inside the node.
			 * For maxKeyLength, give it room plus four data bytes
			 * so the record-length check cannot reject it.
			 */
			gen_store(image.bytes + location + NODE_DESCRIPTOR, key_length, 2);
			gen_store(image.bytes + location + 20,
			    (key_length - 6) / 2, 2);
			if (fault == 1) {
				gen_store(image.bytes + location + NODE_RECORDS, 1, 2);
				gen_store(image.bytes + location + size - 4,
				    14 + 2 + key_length + 4, 2);
			}
			hfsplus_cursor_init(&cursor, tree, tree->search_node);
			if (index == 0) {
				CHECK(hfsplus_node_read(tree, number,
				    cursor.buffer, HFSPLUS_NODE_LEAF, 1,
				    &cursor.node, &error) == FSIMG_OK);
				cursor.number = number;
				cursor.valid = true;
				expect_status(leaf_cases[fault],
				    hfsplus_cursor_record(&cursor, &record,
				    &error), FSIMG_ECORRUPT, &error);
			} else {
				expect_status(index_cases[fault],
				    hfsplus_btree_search(&cursor, &key, &error),
				    FSIMG_ECORRUPT, &error);
				/* The child-size check also rejects this record.
				 * Require rejection by the key-bound check.
				 */
				expect_true(index_cases[fault],
				    strcmp(error.message, bounds_message) == 0);
			}
		}
		bcopy(saved, image.bytes + location, size);
	}
	fixture_close(&f);
	gen_free(&image);
}

static fsimg_status_t
index_child_fork(fixture_t *f, const gen_image_t *image, uint8_t type,
    fsimg_extent_index_t **index, fsimg_error_t *error)
{
	const gen_record_t *record = &image->trees[GEN_CATALOG].records[2];
	hfsplus_fork_t fork;

	CHECK(hfsplus_fork_decode(image->bytes + record->offset +
	    record->key_length +
	    (type == 0 ? FILE_DATA_FORK : FILE_RESOURCE_FORK), 80, &fork));
	return (hfsplus_fork_index(&f->reader, 32, type, &fork, index, NULL,
	    error));
}

static void
test_forks(void)
{
	static const uint16_t name[] = { 'f' };
	const uint32_t counts[] = { 0, 1, 8, 9, 17 };
	gen_child_t child = { .name = name, .units = 1, .id = 32 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	fsimg_extent_index_t *index;
	hfsplus_fork_t fork;

	for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
		gen_plan_init(&plan);
		child.data_extents = child.resource_extents = counts[n];
		plan.children = &child;
		plan.child_count = 1;
		gen_build(&plan, &image);
		fixture_init(&f, &image, 0, 0);
		CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
		for (unsigned type = 0; type <= 255; type += 255) {
			expect_status("fragmented fork resolves",
			    index_child_fork(&f, &image,
			    (uint8_t)type, &index, &error), FSIMG_OK, &error);
			expect_true("extent count",
			    counts[n] == 0 ? index == NULL :
			    index != NULL && fsimg_test_extent_count(index) ==
			    counts[n]);
			if (index != NULL && type == 0 && counts[n] > 1) {
				unsigned char bytes[32];
				fsimg_extent_t extent;

				expect_status("read across extent boundary",
				    hfsplus_fork_read(&f.reader,
				    index, plan.block_size - 16, bytes,
				    sizeof(bytes), &error), FSIMG_OK,
				    &error);
				for (unsigned i = 0; i < sizeof(bytes); i++)
					expect_true("cross extent bytes",
					    bytes[i] ==
					    (unsigned char)(i < 16 ?
					    plan.block_size - 16 + i : 1 + i
					    - 16));
				expect_status("index lookup",
				    fsimg_extent_index_lookup(index,
				    plan.block_size, &extent, &error), FSIMG_OK,
				    &error);
				expect_true("index physical block",
				    extent.image_offset ==
				    (uint64_t)(image.payload_block + 2) *
				    plan.block_size);
			}
			fsimg_extent_index_free(index);
		}
		const gen_record_t *record =
		    &image.trees[GEN_CATALOG].records[2];
		uint64_t location = record->offset + record->key_length + FILE_DATA_FORK;
		CHECK(hfsplus_fork_decode(image.bytes + location, 80, &fork));
		uint64_t records = UINT64_MAX;
		expect_status("fragmented fork record count", hfsplus_fork_index(
		    &f.reader, 32, 0, &fork, &index, &records, &error),
		    FSIMG_OK, &error);
		expect_true("true overflow record count", records ==
		    (counts[n] > 8 ? (counts[n] - 1) / 8 : 0));
		fsimg_extent_index_free(index);
		records = UINT64_MAX;
		fork.logical_size = (uint64_t)fork.total_blocks *
		    plan.block_size + 1;
		expect_status("logical size beyond allocation",
		    hfsplus_fork_index(&f.reader,
		    32, 0, &fork, &index, &records, &error), FSIMG_ECORRUPT,
		    &error);
		expect_true("failing fork resets records", records == 0);
		CHECK(index == NULL);
		fixture_close(&f);
		gen_free(&image);
	}

	gen_plan_init(&plan);
	child.data_extents = 17;
	child.resource_extents = 0;
	plan.children = &child;
	plan.child_count = 1;
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	uint64_t overflow = image.trees[GEN_EXTENTS].records[0].offset;
	uint64_t next = image.trees[GEN_EXTENTS].records[1].offset;
	const mutation_t mutations[] = {
		{ "overflow gap", next + 8, 17, 4, FSIMG_ECORRUPT, NULL },
		{ "overflow repeat", next + 8, 8, 4, FSIMG_ECORRUPT, NULL },
		{ "overflow overlap", next + 8, 15, 4, FSIMG_ECORRUPT, NULL },
		{ "overflow nonzero start zero count", overflow + 16, 0, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "overflow used after unused", overflow + 12, 0, 8,
		    FSIMG_ECORRUPT, NULL }
	};
	for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		unsigned char saved[8];
		const mutation_t *m = &mutations[i];

		bcopy(image.bytes + m->offset, saved, m->width);
		gen_store(image.bytes + m->offset, m->value, m->width);
		expect_status(m->name, index_child_fork(&f, &image, 0, &index,
		    &error), m->expected, &error);
		fsimg_extent_index_free(index);
		bcopy(saved, image.bytes + m->offset, m->width);
	}
	/* Shorten the last record via its end offset, keeping its key intact.
	 */
	uint64_t extent_leaf = gen_node_offset(&image, GEN_EXTENTS, 1);
	uint64_t record_end = extent_leaf + plan.node_size[GEN_EXTENTS] - 6;
	uint64_t saved_end = gen_load(image.bytes + record_end, 2);
	gen_store(image.bytes + record_end, saved_end - 2, 2);
	expect_status("overflow data not 64 bytes",
	    index_child_fork(&f, &image, 0, &index, &error),
	    FSIMG_ECORRUPT, &error);
	fsimg_extent_index_free(index);
	expect_true("short overflow diagnostic", strstr(error.message,
	    "overflow record is malformed") != NULL);
	/* The final leaf record has its key plus enough trailing bytes to
	 * reach 100 bytes; a minimum-length decoder would accept it.
	 */
	gen_store(image.bytes + record_end, next - extent_leaf + 100, 2);
	expect_status("100-byte overflow leaf record",
	    index_child_fork(&f, &image, 0, &index, &error),
	    FSIMG_ECORRUPT, &error);
	expect_true("long overflow diagnostic", strstr(error.message,
	    "overflow record is malformed") != NULL);
	fsimg_extent_index_free(index);
	gen_store(image.bytes + record_end, saved_end, 2);

	unsigned char saved[64];
	bcopy(image.bytes + overflow + 12, saved, 64);
	bzero(image.bytes + overflow + 12, 64);
	expect_status("zero-block overflow record", index_child_fork(&f, &image,
	    0,
	    &index, &error), FSIMG_ECORRUPT, &error);
	expect_true("zero-block overflow diagnostic", strstr(error.message,
	    "overflow record holds no blocks") != NULL);
	fsimg_extent_index_free(index);
	bcopy(saved, image.bytes + overflow + 12, 64);
	uint64_t location = image.trees[GEN_CATALOG].records[2].offset +
	    image.trees[GEN_CATALOG].records[2].key_length + 88;
	CHECK(hfsplus_fork_decode(image.bytes + location, 80, &fork));
	fork.total_blocks = 15;
	fork.logical_size = 15u * plan.block_size;
	expect_status("overflow overshoots totalBlocks", hfsplus_fork_index(&f.reader,
	    32, 0,
	    &fork, &index, NULL, &error), FSIMG_ECORRUPT, &error);
	expect_true("overflow overshoot diagnostic", strstr(error.message,
	    "extents exceed its totalBlocks") != NULL);
	fsimg_extent_index_free(index);
	/* Eight inline blocks and the first eight-block overflow record
	 * complete this fork. The record at logical block 16 is extra.
	 */
	fork.total_blocks = 16;
	fork.logical_size = 16u * plan.block_size;
	bzero(&error, sizeof(error));
	expect_status("extra overflow after last", hfsplus_fork_index(&f.reader,
	    32, 0, &fork, &index, NULL, &error), FSIMG_ECORRUPT, &error);
	expect_true("extra overflow diagnostic", strstr(error.message,
	    "fork has an extra overflow record") != NULL);
	fsimg_extent_index_free(index);
	for (unsigned fault = 0; fault < 9; fault++) {
		const char *names[] = {
			"inline nonzero start zero count",
			    "inline used after unused",
			"inline extent in reserved head",
			    "inline extent past volume",
			"inline extent across reserved tail",
			    "zero total with used extent",
			"zero total with nonzero size", "huge claimed fork",
			"extents tree needs its own overflow"
		};

		fork = (hfsplus_fork_t){ .logical_size = 1, .total_blocks = 1,
		    .extents = { { image.payload_block, 1 } } };
		switch (fault) {
		case 0: fork.extents[0].count = 0; break;
		case 1: fork.extents[1] = fork.extents[0];
			fork.extents[0] = (hfsplus_extent_t){ 0, 0 }; break;
		case 2: fork.extents[0].start = 0; break;
		case 3: fork.extents[0].start = plan.total_blocks; break;
		case 4: fork.extents[0].start = plan.total_blocks - 2;
			fork.extents[0].count = 2; fork.total_blocks = 2; break;
		case 5: fork.total_blocks = 0; fork.logical_size = 0; break;
		case 6: fork = (hfsplus_fork_t){ .logical_size = 1 }; break;
		case 7: fork.total_blocks = UINT32_MAX; break;
		case 8: fork.total_blocks = 2; break;
		}
		uint64_t work = f.reader.session.progress.work;
		expect_status(names[fault], hfsplus_fork_index(&f.reader,
		    fault == 8 ? 3 : 32, 0, &fork, &index, NULL, &error),
		    FSIMG_ECORRUPT, &error);
		if (fault == 8)
			expect_true(names[fault], strstr(error.message,
			    "extents tree's fork needs overflow") != NULL);
		if (fault == 5)
			expect_true(names[fault], strstr(error.message,
			    "extents exceed its totalBlocks") != NULL);
		if (fault == 7)
			expect_true("huge fork rejected before extent walk",
			    f.reader.session.progress.work == work);
		fsimg_extent_index_free(index);
	}
	fixture_close(&f);
	gen_free(&image);

	gen_child_t children[12];
	uint16_t names[12][2];
	search_fixture(&plan, children, names, 12, 2);
	plan.block_size = 512;
	plan.total_blocks = 4096;
	plan.catalog_overflow = true;
	gen_build(&plan, &image);
	expect_status("catalog overflow opens", image_open(&image, -1, false),
	    FSIMG_OK, NULL);
	mutation_t missing = { "catalog overflow missing",
	    image.trees[GEN_EXTENTS].records[0].offset + 8,
	    9, 4, FSIMG_ECORRUPT, NULL };
	mutate_open(&image, &missing);
	gen_free(&image);

	/* A hundred thousand non-adjacent extents exercise the scratch-backed
	 * index. The image's calloc payload pages stay untouched. */
	gen_plan_init(&plan);
	plan.block_size = 512;
	plan.total_blocks = 240000;
	plan.node_size[GEN_EXTENTS] = 4096;
	child.data_extents = 100000;
	plan.children = &child;
	plan.child_count = 1;
	gen_build(&plan, &image);
	for (unsigned limit = 0; limit < 3; limit++) {
		fixture_init(&f, &image, limit == 1 ? 65536 : 8 * 1024 * 1024,
		    limit == 2 ? 1000 : 0);
		fsimg_status_t status = fixture_open(&f, -1, false, &error);
		expect_status("large fork volume opens before limit", status,
		    FSIMG_OK, &error);
		if (status == FSIMG_OK)
			status = index_child_fork(&f, &image, 0, &index,
			    &error);
		else
			index = NULL;
		expect_status("large fork resource bound", status,
		    limit == 0 ? FSIMG_OK : FSIMG_ELIMIT, &error);
		if (status == FSIMG_OK)
			expect_true("100000 extents retained",
			    fsimg_test_extent_count(index) == 100000);
		fsimg_stats_t stats;
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		expect_true("fragmented fork residency below one MiB",
		    stats.memory_bytes < 1024 * 1024);
		fsimg_extent_index_free(index);
		fixture_close(&f);
	}
	gen_free(&image);
}

static void
test_journals(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	for (unsigned little = 0; little < 2; little++) {
		gen_plan_init(&plan);
		plan.journal = true;
		plan.journal_little_endian = little != 0;
		gen_build(&plan, &image);
		expect_status("journal clean byte order", image_open(&image, -1,
		    false), FSIMG_OK, NULL);
		gen_store(image.bytes + VOLUME_ATTRIBUTES, 1u << 13, 4);
		expect_status("journal attribute alone", image_open(&image, -1,
		    false), FSIMG_OK, NULL);
		gen_store(image.bytes + VOLUME_ATTRIBUTES, 0x2100, 4);
		static const struct {
			const char *name;
			uint64_t start;
			uint64_t end;
			fsimg_status_t expected;
		} positions[] = {
			{ "clean interior", VOLUME_HEADER, 1024, FSIMG_OK },
			{ "start wraps", 262144, 512, FSIMG_OK },
			{ "both wrap", 262144, 262144, FSIMG_OK },
			{ "end wraps", 512, 262144, FSIMG_OK },
			{ "pending forward", 512, 1024, FSIMG_EUNSUPPORTED },
			{ "pending backward", VOLUME_HEADER, 512, FSIMG_EUNSUPPORTED },
			{ "start zero", 0, 512, FSIMG_ECORRUPT },
			{ "end zero", 512, 0, FSIMG_ECORRUPT },
			{ "start past size", 262656, 512, FSIMG_ECORRUPT },
			{ "end past size", 512, 262656, FSIMG_ECORRUPT },
			{ "start unaligned", 513, 512, FSIMG_ECORRUPT },
			{ "end unaligned", 512, 513, FSIMG_ECORRUPT }
		};
		for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]);
		    i++) {
			char name[96];

			snprintf(name, sizeof(name), "journal endian=%u %s",
			    little, positions[i].name);
			gen_journal_header(image.bytes + image.journal_header,
			    little != 0, positions[i].start, positions[i].end,
			    262144);
			expect_status(name, image_open(&image, -1, false),
			    positions[i].expected, NULL);
		}
		const uint64_t sizes[] = { 261632, UINT64_C(1073742336),
		    524288 };
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			gen_journal_header(image.bytes + image.journal_header,
			    little != 0, 512, 512, sizes[i]);
			expect_status("journal size invalid", image_open(&image,
			    -1, false), FSIMG_ECORRUPT, NULL);
		}
		gen_journal_header(image.bytes + image.journal_header,
		    little != 0, 512, 512, 262144);
		uint64_t data = image.journal_record +
		    image.trees[GEN_CATALOG].records[2].key_length;
		const mutation_t mutations[] = {
			{ "older journal big endian", image.journal_header,
			    0x4a484452, 4, FSIMG_EUNSUPPORTED,
			    "older header form" },
			{ "older journal little endian", image.journal_header,
			    0x5244484a, 4, FSIMG_EUNSUPPORTED,
			    "older header form" },
			{ "journal bad magic", image.journal_header, 0, 4,
			    FSIMG_ECORRUPT, NULL },
			{ "journal bad checksum", image.journal_header + JOURNAL_CHECKSUM, 0,
			    4, FSIMG_ECORRUPT, NULL },
			{ "journal info reserved block", VOLUME_JIB, 0, 4,
			    FSIMG_ECORRUPT,
			    "journal info block lies outside the content domain" },
			{ "journal info past content", VOLUME_JIB,
			    plan.total_blocks - 1, 4, FSIMG_ECORRUPT,
			    "journal info block lies outside the content domain" },
			{ "journal not file", data, 1, 2, FSIMG_ECORRUPT, NULL },
			{ "journal wrong offset", image.journal_info + JIB_OFFSET,
			    image.journal_header + 4096, 8, FSIMG_ECORRUPT,
			    "disagrees with the journal info block" },
			{ "journal info wrong size", image.journal_info + JIB_SIZE,
			    258048, 8, FSIMG_ECORRUPT,
			    "disagrees with the journal info block" }
		};
		for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]);
		    i++)
			mutate_open(&image, &mutations[i]);
		/* Split its one extent while preserving size and allocated
		 * blocks. */
		unsigned char saved[80];
		bcopy(image.bytes + data + FILE_DATA_FORK, saved, 80);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_COUNT, 32, 4);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_START + 8,
		    image.journal_header / 4096 + 32, 4);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_COUNT + 8, 32, 4);
		expect_status("journal two extents", image_open(&image, -1,
		    false), FSIMG_ECORRUPT, NULL);
		bcopy(saved, image.bytes + data + FILE_DATA_FORK, 80);
		fixture_init(&f, &image, 0, 0);
		CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
		((fsimg_hfsplus_reader_t *)f.reader.driver)->wrapped = true;
		expect_status("wrapped initialized journal",
		    hfsplus_journal_open(&f.reader, &error), FSIMG_EUNSUPPORTED,
		    &error);
		fixture_close(&f);
		/* A merely similar name cannot satisfy the journal requirement.
		 */
		mutation_t similar = { "journal similar stored name",
		    image.journal_record + CATALOG_NAME + 14, 'm', 2, FSIMG_ECORRUPT, NULL };
		mutate_open(&image, &similar);
		fixture_init(&f, &image, 0, 0);
		CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
		unsigned char journal_name[16];
		bcopy(image.bytes + image.journal_record + CATALOG_NAME, journal_name, 16);
		for (unsigned i = 1; i < 8; i++) {
			unsigned char *unit = image.bytes + image.journal_record
			    + 8 + 2 * i;

			gen_store(unit, gen_load(unit, 2) - ('a' - 'A'), 2);
		}
		fsimg_hfsplus_reader_t *state = f.reader.driver;
		hfsplus_cursor_t cursor;
		hfsplus_key_t key;
		CHECK(hfsplus_catalog_key_init(&key, 2, journal_name, 8,
		    &error) == FSIMG_OK);
		bool found;
		hfsplus_cursor_init(&cursor, &state->catalog,
		    state->catalog.search_node);
		expect_status("folded journal search",
		    hfsplus_btree_find_equivalent(&cursor,
		    &key, &found, &error), FSIMG_OK, &error);
		expect_true("folded journal search finds .JOURNAL", found);
		hfsplus_record_t record;
		expect_status("exact journal lookup", hfsplus_catalog_find_exact(
		    &state->catalog, &key, &record, &found, &error), FSIMG_OK,
		    &error);
		expect_true("exact lookup refuses folded spelling", !found);
		fixture_close(&f);
		/* The journal identity is exact stored units even on a folded
		 * tree. */
		expect_status("folded .JOURNAL is not exact .journal",
		    image_open(&image, -1, false), FSIMG_ECORRUPT, NULL);
		gen_free(&image);
	}
	for (unsigned present = 0; present < 2; present++) {
		static const struct {
			unsigned flags;
			fsimg_status_t without_file;
			fsimg_status_t with_file;
		} flags[] = {
			{ 0, FSIMG_ECORRUPT, FSIMG_ECORRUPT },
			{ 1, FSIMG_ECORRUPT, FSIMG_OK },
			{ 2, FSIMG_EUNSUPPORTED, FSIMG_EUNSUPPORTED },
			{ 3, FSIMG_EUNSUPPORTED, FSIMG_EUNSUPPORTED },
			{ 4, FSIMG_ECORRUPT, FSIMG_ECORRUPT },
			{ 5, FSIMG_ECORRUPT, FSIMG_OK },
			{ 6, FSIMG_EUNSUPPORTED, FSIMG_EUNSUPPORTED }
		};

		for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
			char name[96];

			snprintf(name, sizeof(name), "journal flags=%u file=%u",
			    flags[i].flags, present);
			gen_plan_init(&plan);
			plan.journal = true;
			plan.journal_file = present != 0;
			plan.journal_flags = flags[i].flags;
			gen_build(&plan, &image);
			fsimg_status_t expected = present == 0 ?
			    flags[i].without_file : flags[i].with_file;
			expect_status(name, image_open(&image, -1, false),
			    expected, NULL);
			if ((flags[i].flags & 2) != 0) {
				gen_store(image.bytes + image.journal_info + JIB_OFFSET,
				    UINT64_MAX, 8);
				gen_store(image.bytes + image.journal_info + JIB_SIZE,
				    1, 8);
				expect_status(
				    "external journal ignores location",
				    image_open(&image, -1, false),
				    FSIMG_EUNSUPPORTED, NULL);
			}
			if (flags[i].flags == 5 && present != 0) {
				fixture_init(&f, &image, 0, 0);
				f.fail_offset = image.journal_header;
				CHECK(fixture_open(&f, -1, false,
				    &error) == FSIMG_OK);
				expect_true("uninitialized journal detail",
				    f.reader.info.details[0].value == 2);
				((fsimg_hfsplus_reader_t
				    *)f.reader.driver)->wrapped = true;
				expect_status("wrapped uninitialized journal",
				    hfsplus_journal_open(&f.reader, &error),
				    FSIMG_OK, &error);
				fixture_close(&f);
			}
			/* With bit 13 clear, the same stored file is ordinary
			 * data. */
			gen_store(image.bytes + VOLUME_ATTRIBUTES, 0x100, 4);
			expect_status("unjournaled ignores journal metadata",
			    image_open(&image, -1, false), FSIMG_OK, NULL);
			gen_free(&image);
		}
	}
}

/* Each row isolates one location clause, with every other clause satisfied. */
static void
test_journal_location_clauses(void)
{
	static const struct {
		const char *name;
		uint32_t flags;
		uint64_t size;
		uint32_t count;
		bool past_end;
		bool wrong_offset;
		const char *diagnostic;
	} rows[] = {
		{ "journal count past domain", 1, 262144, 64, true, false,
		    "lies outside the content domain" },
		{ "journal fork not whole blocks", 1, 262143, 64, false, false,
		    "disagrees with the journal info block" },
		{ "uninitialized journal offset mismatch", 5, 262144, 64,
		    false, true, "disagrees with the journal info block" },
		{ "uninitialized journal size mismatch", 5, 258048, 64,
		    false, false, "disagrees with the journal info block" }
	};
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		gen_plan_init(&plan);
		plan.journal = true;
		plan.journal_flags = rows[i].flags;
		gen_build(&plan, &image);
		uint64_t data = image.journal_record +
		    image.trees[GEN_CATALOG].records[2].key_length;
		uint64_t start = rows[i].past_end ? plan.total_blocks - 2 :
		    image.journal_header / plan.block_size;
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_START, start, 4);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_BLOCKS, rows[i].count, 4);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_COUNT, rows[i].count, 4);
		/* For the fractional-block case, fork size and JIB size agree.
		 * The flags=5 size case keeps the fork's original whole size.
		 */
		if (rows[i].flags != 5)
			gen_store(image.bytes + data + FILE_DATA_FORK, rows[i].size, 8);
		gen_store(image.bytes + image.journal_info + JIB_SIZE, rows[i].size, 8);
		gen_store(image.bytes + image.journal_info + JIB_OFFSET,
		    start * plan.block_size + (rows[i].wrong_offset ? 4096 : 0),
		    8);
		fixture_init(&f, &image, 0, 0);
		expect_status(rows[i].name, fixture_open(&f, -1, false, &error),
		    FSIMG_ECORRUPT, &error);
		expect_true(rows[i].name, strstr(error.message,
		    rows[i].diagnostic) != NULL);
		fixture_close(&f);
		gen_free(&image);
	}
}

/* Keep the location rule satisfied while changing the header size, so these
 * reach the journal range check rather than an earlier mismatch. The large
 * fixture has sparse calloc backing: only metadata pages are touched.
 */
static void
test_journal_size_limits(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	static const struct {
		const char *name;
		uint64_t size;
		uint32_t blocks;
		fsimg_status_t expected;
	} rows[] = {
		{ "journal below 256 KiB", 258048, 512, FSIMG_ECORRUPT },
		{ "journal exactly 1 GiB", UINT64_C(1073741824), 300000,
		    FSIMG_OK },
		{ "journal above 1 GiB", UINT64_C(1073745920), 300000,
		    FSIMG_ECORRUPT }
	};

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		gen_plan_init(&plan);
		plan.journal = true;
		plan.total_blocks = rows[i].blocks;
		gen_build(&plan, &image);
		expect_status("journal size baseline", image_open(&image, -1,
		    false), FSIMG_OK, NULL);
		uint64_t data = image.journal_record +
		    image.trees[GEN_CATALOG].records[2].key_length;
		uint64_t size = rows[i].size;
		gen_store(image.bytes + image.journal_info + JIB_SIZE, size, 8);
		gen_store(image.bytes + data + FILE_DATA_FORK, size, 8);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_BLOCKS,
		    size / plan.block_size, 4);
		gen_store(image.bytes + data + FILE_DATA_FORK + FORK_COUNT,
		    size / plan.block_size, 4);
		gen_journal_header(image.bytes + image.journal_header, false,
		    512, 512, size);
		fixture_init(&f, &image, 0, 0);
		expect_status(rows[i].name,
		    fixture_open(&f, -1, false, &error), rows[i].expected,
		    &error);
		fixture_close(&f);
		gen_free(&image);
	}
}

static void
test_labels_info(void)
{
	static const struct {
		const char *name;
		uint64_t value;
	} details[] = {
		{ "journal", 0 }, { "catalog_node_bytes", 4096 },
		{ "extents_node_bytes", 512 }, { "attributes_node_bytes", 0 },
		{ "files", 0 }, { "folders", 0 }, { "next_catalog_id", 1000000 },
		{ "volume_id", UINT64_C(0x123456789abcdef0) },
		{ "last_mounted", 0x31302e30 }, { "create_date", 123456 }
	};
	static const struct {
		const char *name;
		uint16_t unit;
		size_t units;
		bool valid;
		const char *display;
	} labels[] = {
		{ "ASCII label", 'A', 1, true, "A" },
		{ "NUL label", 0, 1, true, "\xe2\x90\x80" },
		{ "slash label", '/', 1, true, ":" },
		{ "NUL symbol label", 0x2400, 1, false, "" },
		{ "colon label", ':', 1, false, "" },
		{ "surrogate label", 0xd800, 1, false, "" },
		{ "precomposed label", 0xe9, 1, false, "" },
		{ "dot label", '.', 1, false, "" },
		{ "255 BMP units", 0x4e00, 255, true, NULL },
		{ "empty label", 0, 0, false, "" }
	};
	uint16_t wide[255];
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		gen_plan_init(&plan);
		for (size_t j = 0; j < labels[i].units; j++)
			wide[j] = labels[i].unit;
		plan.label = wide;
		plan.label_units = labels[i].units;
		gen_build(&plan, &image);
		fixture_init(&f, &image, 0, 0);
		fsimg_status_t status = fixture_open(&f, -1, false, &error);
		expect_status(labels[i].name, status, FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			const fsimg_volume_info_t *info = &f.reader.info;
			bool valid = labels[i].valid;

			expect_true(labels[i].name,
			    ((info->valid & FSIMG_VOLUME_LABEL) != 0) == valid);
			if (labels[i].display != NULL)
				expect_true(labels[i].name,
				    strcmp(info->label, labels[i].display) == 0);
			else {
				expect_true(labels[i].name,
				    strlen(info->label) == 765 &&
				    info->label[765] == '\0');
				for (unsigned j = 0; j < 255; j++)
					expect_true(labels[i].name,
					    bcmp(info->label + 3 * j,
					    "\xe4\xb8\x80", 3) == 0);
			}
			expect_true(labels[i].name,
			    info->volume_bytes == image.length &&
			    info->sector_bytes == 512 &&
			    info->allocation_unit_bytes == plan.block_size
			    &&
			    info->allocation_units == plan.total_blocks);
			expect_true(labels[i].name, strcmp(info->format,
			    "hfsplus") == 0);
			expect_true(labels[i].name,
			    info->valid == (FSIMG_VOLUME_GEOMETRY | (valid ?
			    FSIMG_VOLUME_LABEL : 0)));
			expect_true(labels[i].name, info->detail_count == 10);
			for (size_t j = 0; j < sizeof(details) / sizeof(details[0]);
			    j++)
				expect_true(labels[i].name,
				    strcmp(info->details[j].name,
				    details[j].name) == 0 &&
				    info->details[j].value == details[j].value);
		}
		fixture_close(&f);
		/* Every label fixture is paired with a root identity failure.
		 */
		uint64_t data = image.root_record +
		    image.trees[GEN_CATALOG].records[0].key_length;
		mutation_t wrong_id = { "root primary not folder 2", data + 8,
		    3, 4, FSIMG_ECORRUPT, NULL };
		mutate_open(&image, &wrong_id);
		gen_free(&image);
	}
	gen_plan_init(&plan);
	gen_build(&plan, &image);
	uint64_t primary = image.root_record +
	    image.trees[GEN_CATALOG].records[0].key_length;
	uint64_t thread = image.root_thread + 8;
	const mutation_t roots[] = {
		{ "root thread missing", image.root_thread + 2, 3, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "root thread wrong type", thread, 4, 2, FSIMG_ECORRUPT, NULL },
		{ "root thread wrong parent", thread + 4, 2, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "root thread fold-equal spelling", thread + 10, 't', 2,
		    FSIMG_ECORRUPT, NULL },
		{ "root primary missing", image.root_record + 2, 0, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "root primary not folder", primary, 2, 2, FSIMG_ECORRUPT, NULL }
	};
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
		mutate_open(&image, &roots[i]);
	gen_free(&image);
}

static void
test_faults(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	gen_plan_init(&plan);
	plan.journal = true;
	plan.attributes = true;
	gen_build(&plan, &image);
	expect_status("fault baseline", image_open(&image, -1, false), FSIMG_OK,
	    NULL);
	const uint64_t offsets[] = { image.journal_header,
		gen_node_offset(&image, GEN_CATALOG, 1),
		gen_node_offset(&image, GEN_EXTENTS, 0) };
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		fixture_init(&f, &image, 0, 0);
		f.fail_offset = offsets[i];
		expect_status("read failure keeps EIO", fixture_open(&f, -1,
		    false, &error), FSIMG_EIO, &error);
		fixture_close(&f);
	}
	bool completed = false;
	for (uint64_t attempt = 1; attempt < 200; attempt++) {
		fixture_init(&f, &image, 0, 0);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		fsimg_status_t status = fixture_open(&f, -1, false, &error);
		if (status == FSIMG_OK)
			completed = true;
		else
			expect_status("injected allocation failure", status,
			    FSIMG_ENOMEM, &error);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_close(&f);
		if (completed)
			break;
	}
	expect_true("all open allocations injected", completed);
	gen_free(&image);
}

/* A purpose-written scanner for fields.json's flat volume-header scalars.
 * Searching begins at the volume_header object and ends before its nested
 * dates/forks; duplicate fields elsewhere cannot supply the comparison.
 */
static const char *
json_scalar(const char *header, const char *field)
{
	const char *p = header, *start, *end, *value;
	unsigned depth = 0;

	if (p == NULL || *p != '{')
		return (NULL);
	for (; *p != '\0'; p++) {
		if (*p == '{' || *p == '[') {
			depth++;
			continue;
		}
		if (*p == '}' || *p == ']') {
			if (--depth == 0)
				break;
			continue;
		}
		if (*p != '"')
			continue;
		start = ++p;
		while (*p != '\0' && *p != '"') {
			if (*p == '\\' && p[1] != '\0')
				p++;
			p++;
		}
		if (*p == '\0')
			return (NULL);
		end = p;
		value = p + 1;
		while (*value == ' ' || *value == '\t' || *value == '\n')
			value++;
		if (depth != 1 || *value != ':' ||
		    (size_t)(end - start) != strlen(field) ||
		    strncmp(start, field, (size_t)(end - start)) != 0)
			continue;
		value++;
		while (*value == ' ' || *value == '\t' || *value == '\n')
			value++;
		return (value);
	}
	return (NULL);
}

static bool
json_integer(const char *header, const char *field, uint64_t *value)
{
	const char *p = json_scalar(header, field);
	char *end;

	if (p == NULL || *p < '0' || *p > '9')
		return (false);
	errno = 0;
	*value = strtoull(p, &end, 10);
	return (errno == 0 && (*end == ',' || *end == '\n' || *end == ' ' ||
	    *end == '}'));
}

static bool
native_compare(const char *header, const fsimg_hfsplus_reader_t *state)
{
	const char *fields[] = { "block_size", "total_blocks", "file_count",
		"folder_count", "next_catalog_id" };
	const uint64_t values[] = { state->header.block_size,
	    state->header.total_blocks,
		state->header.file_count, state->header.folder_count,
		    state->header.next_catalog_id };
	uint64_t expected;
	bool matches = true;

	for (unsigned i = 0; i < 5; i++) {
		if (!json_integer(header, fields[i],
		    &expected) || expected != values[i]) {
			fprintf(stderr, "native field mismatch: %s\n",
			    fields[i]);
			matches = false;
		}
	}
	const char *signature = json_scalar(header, "signature");
	const char *wanted = state->header.signature == 0x482b ? "\"H+\"" :
	    "\"HX\"";
	return (matches && signature != NULL && strncmp(signature, wanted,
	    4) == 0);
}

/* Inflate one capture into the memory input fixture; no temporary image. */
static bool
native_gzip(const char *path, gen_image_t *image)
{
	gzFile stream;
	unsigned char *bytes;
	size_t capacity = 1024 * 1024;
	int count;

	stream = gzopen(path, "rb");
	if (stream == NULL)
		return (false);
	image->bytes = malloc(capacity);
	CHECK(image->bytes != NULL);
	for (;;) {
		if (capacity - image->length < 1024 * 1024) {
			CHECK(capacity <= SIZE_MAX / 2);
			capacity *= 2;
			bytes = realloc(image->bytes, capacity);
			CHECK(bytes != NULL);
			image->bytes = bytes;
		}
		count = gzread(stream, image->bytes + image->length,
		    1024 * 1024);
		CHECK(count >= 0);
		if (count == 0)
			break;
		image->length += (size_t)count;
	}
	CHECK(gzclose(stream) == Z_OK);
	return (true);
}

static void
test_native(void)
{
	const char *directory = getenv("FSIMG_HFSPLUS_NATIVE");
	struct dirent *entry;
	DIR *dir;
	char image_path[4096], fields_path[4096];
	unsigned captures = 0, missing = 0;

	if (directory == NULL) {
		puts("SKIP: FSIMG_HFSPLUS_NATIVE unset; native captures not read "
		    "(a skip is not a pass)");
		return;
	}
	dir = opendir(directory);
	CHECK(dir != NULL);
	while ((entry = readdir(dir)) != NULL) {
		struct stat st;
		FILE *fields;
		char *json;
		const char *header;
		fixture_t f;
		gen_image_t empty = { 0 };
		fsimg_error_t error = { 0 };
		fsimg_status_t status, probe, expected;
		unsigned char prefix[1536];
		const fsimg_format_variant_t *variant;
		size_t transferred;
		int fd;
		bool matches, wrapped, classic;

		CHECK(snprintf(image_path, sizeof(image_path),
		    "%s/%s/image.img", directory,
		    entry->d_name) < (int)sizeof(image_path));
		CHECK(snprintf(fields_path, sizeof(fields_path),
		    "%s/%s/fields.json", directory,
		    entry->d_name) < (int)sizeof(fields_path));
		fields = fopen(fields_path, "rb");
		if (fields == NULL)
			continue;
		fd = open(image_path, O_RDONLY | O_CLOEXEC);
		if (fd == -1) {
			CHECK(snprintf(image_path, sizeof(image_path),
			    "%s/%s/image.img.gz", directory,
			    entry->d_name) < (int)sizeof(image_path));
			if (!native_gzip(image_path, &empty)) {
				missing++;
				CHECK(fclose(fields) == 0);
				continue;
			}
		}

		CHECK(fstat(fileno(fields),
		    &st) == 0 && st.st_size >= 0 && st.st_size < 16 * 1024 *
		    1024);
		json = calloc(1, (size_t)st.st_size + 1);
		CHECK(json != NULL);
		CHECK(fread(json, 1, (size_t)st.st_size,
		    fields) == (size_t)st.st_size);
		CHECK(fclose(fields) == 0);
		header = json_scalar(json, "volume_header");
		fixture_init(&f, &empty, 0, 0);
		if (fd != -1) {
			fsimg_io_free(f.io);
			CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &f.io,
			    &error) == FSIMG_OK);
			f.reader.input = f.io;
			CHECK(fsimg_io_get_size(f.io, &f.length, &error) == FSIMG_OK);
		}

		bzero(prefix, sizeof(prefix));
		CHECK(fsimg_io_read_at(f.io, 0, prefix, sizeof(prefix),
		    &transferred, &error) == FSIMG_OK);
		probe = fsimg_hfsplus_format.probe(prefix, transferred,
		    f.length, &variant, &error);
		wrapped = gen_load(prefix + 1024, 2) == 0x4244 &&
		    gen_load(prefix + 1024 + 124, 2) == 0x482b;
		classic = gen_load(prefix + 1024, 2) == 0x4244 && !wrapped;
		expect_status(entry->d_name, probe,
		    classic ? FSIMG_EFORMAT : FSIMG_OK, &error);
		if (classic) {
			printf("%s: not claimed, classic HFS\n", entry->d_name);
			fixture_close(&f);
			if (fd != -1)
				CHECK(close(fd) == 0);
			free(empty.bytes);
			free(json);
			captures++;
			continue;
		}
		expected = wrapped ||
		    strcmp(entry->d_name,
		    "h-hfsplus-33554432-e8-interrupted") == 0 ?
		    FSIMG_EUNSUPPORTED : FSIMG_OK;
		status = fixture_open(&f, -1, false, &error);
		matches = status == expected;
		if (status == FSIMG_OK)
			matches = header != NULL && native_compare(header,
			    f.reader.driver) && matches;
		printf("%s: %s; reason=%s; comparison=%s; probe=%s\n",
		    entry->d_name,
		    fsimg_status_string(status),
		    status == FSIMG_OK ? "opened" : error.message,
		    matches ? "OK" : "FAIL", fsimg_status_string(probe));
		expect_status(entry->d_name, status, expected, &error);
		expect_true("native volume header comparison", matches);
		fixture_close(&f);
		if (fd != -1)
			CHECK(close(fd) == 0);
		free(empty.bytes);
		free(json);
		captures++;
	}
	CHECK(closedir(dir) == 0);
	printf("Native captures: %u read, %u without an image\n", captures,
	    missing);
	expect_true("native captures found", captures != 0);
}

/* Attribute keys sort by ID, name, then logical start, on both variants. */
static void
test_attribute_search(void)
{
	static const gen_attribute_t attributes[] = {
		{ .file_id = 31, .name = 'z', .start_block = 9 },
		{ .file_id = 32, .name = 'A', .start_block = 0 },
		{ .file_id = 32, .name = 'A', .start_block = 2 },
		{ .file_id = 32, .name = 'a', .start_block = 0 },
		{ .file_id = 33, .name = 'A', .start_block = 0 }
	};
	static const struct {
		const char *name;
		gen_attribute_t key;
		size_t next;
		bool found;
	} rows[] = {
		{ "file ID order", { 30, 9, 'z' }, 0, false },
		{ "name order before start", { 32, 99, '@' }, 1, false },
		{ "start block order", { 32, 1, 'A' }, 2, false },
		{ "binary name order", { 32, 99, 'Z' }, 3, false },
		{ "exact attribute", { 32, 2, 'A' }, 2, true }
	};
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_cursor_t cursor;
	hfsplus_key_t key;
	hfsplus_record_t record;
	unsigned char name[2];
	bool found;

	for (unsigned variant = 0; variant < 2; variant++) {
		gen_plan_init(&plan);
		plan.hfsx = variant != 0;
		plan.attributes = true;
		plan.attribute_records = attributes;
		plan.attribute_count = sizeof(attributes) / sizeof(attributes[0]);
		plan.records_per_node = 2;
		gen_build(&plan, &image);
		fixture_init(&f, &image, 0, 0);
		CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
		fsimg_hfsplus_reader_t *state = f.reader.driver;
		hfsplus_cursor_init(&cursor, &state->attributes,
		    state->attributes.search_node);
		for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
			gen_store(name, rows[i].key.name, 2);
			CHECK(hfsplus_attribute_key_init(&key, rows[i].key.file_id,
			    rows[i].key.start_block, name, 1, &error) == FSIMG_OK);
			expect_status(rows[i].name, hfsplus_btree_search(&cursor,
			    &key, &error), FSIMG_OK, &error);
			expect_true(rows[i].name, cursor.valid);
			if (cursor.valid) {
				CHECK(hfsplus_cursor_record(&cursor, &record,
				    &error) == FSIMG_OK);
				const gen_record_t *wanted =
				    &image.trees[GEN_ATTRIBUTES].records[rows[i].next];
				expect_true(rows[i].name, record.key_bytes ==
				    wanted->key_length && bcmp(record.key,
				    wanted->bytes, record.key_bytes) == 0);
			}
			expect_status(rows[i].name, hfsplus_btree_find_equivalent(
			    &cursor, &key, &found, &error), FSIMG_OK, &error);
			expect_true(rows[i].name, found == rows[i].found);
		}
		fixture_close(&f);
		gen_free(&image);
	}
}

static void
test_empty_tree_search(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_cursor_t cursor;
	hfsplus_key_t key = { 0 };

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	uint64_t header = gen_node_offset(&image, GEN_EXTENTS, 0) +
	    NODE_DESCRIPTOR;
	const mutation_t nodes[] = {
		{ "empty tree with root", header + TREE_ROOT, 1, 4,
		    FSIMG_ECORRUPT, "empty tree names nodes" },
		{ "empty tree with first leaf", header + TREE_FIRST_LEAF, 1, 4,
		    FSIMG_ECORRUPT, "empty tree names nodes" },
		{ "empty tree with last leaf", header + TREE_LAST_LEAF, 1, 4,
		    FSIMG_ECORRUPT, "empty tree names nodes" }
	};
	CHECK(gen_load(image.bytes + header, 2) == 0);
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		mutate_open(&image, &nodes[i]);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	CHECK(state->extents.header.tree_depth == 0);
	hfsplus_cursor_init(&cursor, &state->extents,
	    state->extents.search_node);
	expect_status("empty tree lower bound", hfsplus_btree_search(&cursor,
	    &key, &error), FSIMG_OK, &error);
	expect_true("empty tree has no lower bound", !cursor.valid);
	fixture_close(&f);
	gen_free(&image);
}

/* The thread's borrowed name lives in the very buffer the lookup reuses. */
static void
test_catalog_key_owns_thread_name(void)
{
	static const uint16_t child_name[] = { 'a' };
	gen_child_t child = { .name = child_name, .units = 1, .id = 32 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_key_t key;
	hfsplus_record_t record;
	hfsplus_thread_t thread;
	hfsplus_folder_t folder;
	unsigned char saved[510];
	bool found;

	gen_plan_init(&plan);
	plan.children = &child;
	plan.child_count = 1;
	plan.records_per_node = 2;
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	/* An index read replaces the thread's leaf bytes during lookup. */
	CHECK(state->catalog.header.tree_depth > 1);
	CHECK(hfsplus_catalog_key_init(&key, 2, NULL, 0, &error) == FSIMG_OK);
	CHECK(hfsplus_catalog_find_exact(&state->catalog, &key, &record,
	    &found, &error) == FSIMG_OK && found);
	CHECK(hfsplus_thread_decode(record.data, record.data_bytes, &thread));
	bcopy(thread.name, saved, 2 * thread.units);
	CHECK(hfsplus_catalog_key_init(&key, thread.parent, thread.name,
	    thread.units, &error) == FSIMG_OK);
	CHECK(hfsplus_catalog_find_exact(&state->catalog, &key, &record,
	    &found, &error) == FSIMG_OK);
	expect_true("thread name survives exact search", bcmp(key.catalog.name,
	    saved, 2 * key.catalog.units) == 0 && key.catalog.units ==
	    plan.label_units);
	expect_true("thread finds root primary", found &&
	    hfsplus_folder_decode(record.data, record.data_bytes, &folder) &&
	    folder.id == 2);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_content_domain_empty(void)
{
	static const struct {
		const char *name;
		uint32_t block_size;
		uint32_t blocks;
	} rows[] = {
		{ "N of one", 4096, 1 },
		{ "first equals end", 512, 3 },
		{ "first exceeds end", 512, 2 }
	};
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		gen_plan_init(&plan);
		gen_build(&plan, &image);
		gen_store(image.bytes + image.header + 40, rows[i].block_size, 4);
		gen_store(image.bytes + image.header + 44, rows[i].blocks, 4);
		gen_store(image.bytes + image.header + 48, 0, 4);
		fixture_init(&f, &image, 0, 0);
		expect_status(rows[i].name, fixture_open(&f, -1, false, &error),
		    FSIMG_ECORRUPT, &error);
		expect_true(rows[i].name, strstr(error.message,
		    "leaves no blocks for content") != NULL);
		fixture_close(&f);
		gen_free(&image);
	}
}

int
main(void)
{
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	test_identity_geometry();
	test_headers();
	test_tree_parameters();
	test_tree_search();
	test_attribute_search();
	test_empty_tree_search();
	test_catalog_key_owns_thread_name();
	test_content_domain_empty();
	test_tree_faults();
	test_node_framing();
	test_record_key_bounds();
	test_forks();
	test_journals();
	test_journal_size_limits();
	test_journal_location_clauses();
	test_labels_info();
	test_faults();
	test_native();
	printf("HFS Plus: %u assertions, %u failures\n", assertions, failures);
	return (failures == 0 ? 0 : 1);
}
