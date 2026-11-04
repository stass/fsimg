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
#include "selection.h"
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
static void test_attributes_tree_accepts_binary_compare_pair(void);
static void test_attributes_tree_preserves_code_unit_order(void);
static void test_attributes_tree_allows_fixed_keys_only_when_empty(void);
static void test_catalog_and_extents_keep_tree_parameters(void);
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

static void
test_attributes_tree_accepts_binary_compare_pair(void)
{
	static const struct {
		const char *name;
		uint8_t type;
		uint8_t compare;
		fsimg_status_t expected;
	} cases[] = {
		{ "attributes zero pair", 0, 0, FSIMG_OK },
		{ "attributes binary compare pair", 0xff, 0xbc, FSIMG_OK },
		{ "attributes mixed type pair", 0xff, 0, FSIMG_EUNSUPPORTED },
		{ "attributes mixed compare pair", 0, 0xbc, FSIMG_EUNSUPPORTED },
		{ "attributes unknown type with binary compare", 1, 0xbc,
		    FSIMG_EUNSUPPORTED },
		{ "attributes binary type with unknown compare", 0xff, 1,
		    FSIMG_EUNSUPPORTED },
		{ "attributes folded compare with zero type", 0, 0xcf,
		    FSIMG_EUNSUPPORTED },
		{ "attributes folded compare with binary type", 0xff, 0xcf,
		    FSIMG_EUNSUPPORTED }
	};
	gen_plan_t plan;
	gen_image_t image;
	uint64_t header;

	gen_plan_init(&plan);
	plan.attributes = true;
	gen_build(&plan, &image);
	header = gen_node_offset(&image, GEN_ATTRIBUTES, 0) + NODE_DESCRIPTOR;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		gen_store(image.bytes + header + 36, cases[i].type, 1);
		gen_store(image.bytes + header + 37, cases[i].compare, 1);
		expect_status(cases[i].name, image_open(&image, -1, false),
		    cases[i].expected, NULL);
	}
	gen_free(&image);
}

/* The stored order separates case pairs and places non-ASCII units after
 * ASCII. Folding would make A/a equal and move Z past a. Test both a single
 * leaf and index descent, with the same order on HFS Plus and HFSX.
 */
static void
test_attributes_tree_preserves_code_unit_order(void)
{
	static const gen_attribute_t attributes[] = {
		{ .file_id = 32, .name = 'A' },
		{ .file_id = 32, .name = 'Z' },
		{ .file_id = 32, .name = 'a' },
		{ .file_id = 32, .name = 'z' },
		{ .file_id = 32, .name = 0x00c4 },
		{ .file_id = 32, .name = 0x00e4 },
		{ .file_id = 32, .name = 0x8000 }
	};
	const size_t count = sizeof(attributes) / sizeof(attributes[0]);
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	hfsplus_cursor_t cursor;
	hfsplus_key_t key;
	hfsplus_record_t record;
	fsimg_status_t status;
	unsigned char name[2];
	char scenario[96];
	bool found;

	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned indexed = 0; indexed < 2; indexed++) {
			gen_plan_init(&plan);
			plan.hfsx = variant != 0;
			plan.attributes = true;
			plan.attribute_records = attributes;
			plan.attribute_count = count;
			plan.records_per_node = indexed ? 2 : 0;
			gen_build(&plan, &image);
			uint64_t header = gen_node_offset(&image,
			    GEN_ATTRIBUTES, 0) + NODE_DESCRIPTOR;
			gen_store(image.bytes + header + 36, 0xff, 1);
			gen_store(image.bytes + header + 37, 0xbc, 1);
			CHECK(image.trees[GEN_ATTRIBUTES].depth ==
			    (indexed ? 3 : 1));
			snprintf(scenario, sizeof(scenario),
			    "attributes code-unit order hfsx=%u indexed=%u opens",
			    variant, indexed);
			fixture_init(&f, &image, 0, 0);
			status = fixture_open(&f, -1, false, &error);
			expect_status(scenario, status, FSIMG_OK, &error);
			if (status != FSIMG_OK) {
				fixture_close(&f);
				gen_free(&image);
				continue;
			}
			fsimg_hfsplus_reader_t *state = f.reader.driver;
			hfsplus_cursor_init(&cursor, &state->attributes,
			    state->attributes.search_node);
			for (size_t i = 0; i < count; i++) {
				snprintf(scenario, sizeof(scenario),
				    "attribute U+%04X lookup hfsx=%u indexed=%u",
				    (unsigned)attributes[i].name, variant, indexed);
				gen_store(name, attributes[i].name, 2);
				CHECK(hfsplus_attribute_key_init(&key, 32, 0,
				    name, 1, &error) == FSIMG_OK);
				status = hfsplus_btree_find_equivalent(&cursor,
				    &key, &found, &error);
				expect_status(scenario, status, FSIMG_OK, &error);
				expect_true(scenario, status == FSIMG_OK && found);
				if (status != FSIMG_OK || !found)
					continue;
				CHECK(hfsplus_cursor_record(&cursor, &record,
				    &error) == FSIMG_OK);
				expect_true(scenario, record.key_bytes == 16 &&
				    gen_load(record.key + 14, 2) == attributes[i].name);
			}

			/* List from below the first key, through every sibling. */
			CHECK(hfsplus_attribute_key_init(&key, 32, 0, NULL, 0,
			    &error) == FSIMG_OK);
			status = hfsplus_btree_search(&cursor, &key, &error);
			size_t listed = 0;
			while (status == FSIMG_OK && cursor.valid) {
				CHECK(hfsplus_cursor_record(&cursor, &record,
				    &error) == FSIMG_OK);
				snprintf(scenario, sizeof(scenario),
				    "attribute listing entry=%zu hfsx=%u indexed=%u",
				    listed, variant, indexed);
				expect_true(scenario, listed < count &&
				    record.key_bytes == 16 &&
				    gen_load(record.key + 14, 2) ==
				    attributes[listed].name);
				listed++;
				status = hfsplus_cursor_next(&cursor, &error);
			}
			expect_status("attributes code-unit listing completes",
			    status, FSIMG_OK, &error);
			expect_true("attributes listing finds every case variant",
			    listed == count);
			fixture_close(&f);
			gen_free(&image);
		}
	}
}

static void
test_attributes_tree_allows_fixed_keys_only_when_empty(void)
{
	static const gen_attribute_t attributes[] = {
		{ .file_id = 32, .name = 'a' },
		{ .file_id = 32, .name = 'b' },
		{ .file_id = 32, .name = 'c' }
	};
	gen_plan_t plan;
	gen_image_t image;
	char scenario[96];

	for (unsigned pair = 0; pair < 2; pair++) {
		for (unsigned depth = 0; depth <= 2; depth++) {
			gen_plan_init(&plan);
			plan.attributes = true;
			plan.attribute_records = attributes;
			plan.attribute_count = depth == 0 ? 0 :
			    sizeof(attributes) / sizeof(attributes[0]);
			plan.records_per_node = depth == 2 ? 2 : 0;
			gen_build(&plan, &image);
			uint64_t header = gen_node_offset(&image,
			    GEN_ATTRIBUTES, 0) + NODE_DESCRIPTOR;
			CHECK(gen_load(image.bytes + header, 2) == depth);
			if (depth == 0) {
				CHECK(image.trees[GEN_ATTRIBUTES].record_count == 0);
				CHECK(image.trees[GEN_ATTRIBUTES].nodes == 1);
			}
			gen_store(image.bytes + header + 36, pair ? 0xff : 0, 1);
			gen_store(image.bytes + header + 37, pair ? 0xbc : 0, 1);
			gen_store(image.bytes + header + 38, 0x02, 4);
			snprintf(scenario, sizeof(scenario),
			    "attributes fixed index keys depth=%u binary pair=%u",
			    depth, pair);
			expect_status(scenario, image_open(&image, -1, false),
			    depth == 0 ? FSIMG_OK : FSIMG_EUNSUPPORTED, NULL);
			gen_free(&image);
		}
	}
}

static void
test_catalog_and_extents_keep_tree_parameters(void)
{
	static const uint16_t name[] = { 'f' };
	gen_child_t child = { .name = name, .units = 1, .id = 32,
	    .data_extents = 9 };
	gen_plan_t plan;
	gen_image_t image;

	for (unsigned populated = 0; populated < 2; populated++) {
		gen_plan_init(&plan);
		plan.children = &child;
		plan.child_count = populated;
		gen_build(&plan, &image);
		for (unsigned t = GEN_EXTENTS; t <= GEN_CATALOG; t++) {
			uint64_t header = gen_node_offset(&image,
			    (gen_tree_id_t)t, 0) + NODE_DESCRIPTOR;
			const mutation_t cases[] = {
				{ t == GEN_EXTENTS ? "extents reject binary type" :
				    "catalog rejects binary type", header + 36,
				    0xff, 1, FSIMG_EUNSUPPORTED, NULL },
				{ t == GEN_EXTENTS ? "extents keep fixed index keys" :
				    "catalog requires variable index keys", header + 38,
				    0x02, 4, t == GEN_EXTENTS ? FSIMG_OK :
				    FSIMG_EUNSUPPORTED, NULL }
			};
			for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
				mutate_open(&image, &cases[i]);
		}
		gen_free(&image);
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

/* Library traversal checks use the independent generator's disk records. */
typedef struct listed_entry {
	char path[1100];
	fsimg_entry_kind_t kind;
	uint64_t size;
	fsimg_metadata_t metadata;
} listed_entry_t;

typedef struct listing {
	listed_entry_t entries[64];
	size_t count;
	fsimg_status_t status;
	fsimg_error_t error;
} listing_t;

static fsimg_reader_t *public_reader(fixture_t *f, const gen_image_t *image);
static listing_t list_image(const gen_image_t *image);
static listing_t list_plan(const char *name, const gen_plan_t *plan, fsimg_status_t expected,
    const char *message);
static gen_record_t *find_record(gen_record_t *records, size_t count, uint32_t parent,
    uint16_t type);
static void mutate_thread_kind(gen_record_t *r, size_t n);
static void mutate_thread_parent(gen_record_t *r, size_t n);
static void mutate_thread_name(gen_record_t *r, size_t n);
static void mutate_child_thread(gen_record_t *r, size_t n);
static void mutate_equal_keys(gen_record_t *r, size_t n);
static void mutate_descending_keys(gen_record_t *r, size_t n);
static void test_catalog_listing(void);
static void test_catalog_flags_kinds(void);
static void check_content(const char *name, const gen_plan_t *plan, const char *path,
    const unsigned char *wanted, size_t length, fsimg_status_t expected,
    const char *message);
static void test_symbolic_links(void);
static void test_hard_links(void);
static void test_attributes_compression(void);
static void test_names_listing(void);
static unsigned char *catalog_data(gen_image_t *image, uint32_t parent, uint16_t type);
static void expect_listing(const char *name, const gen_image_t *image,
    fsimg_status_t expected, const char *message);
static void test_folder_counts_and_parents(void);
static void test_catalog_exact_lengths(void);
static void check_selection(const gen_plan_t *plan, const char *query,
    const char *display, fsimg_status_t expected);
static void test_names_selection_owned(void);
static void test_attribute_key_order(void);
static void test_mixed_round_trip(void);
static void test_probe_exclusivity(void);
static int native_content(const char *path);

static const uint16_t name_a[] = { 'a' };
static const uint16_t name_b[] = { 'b' };
static const uint16_t name_c[] = { 'c' };
static const uint16_t private_data[] = { 0, 0, 0, 0, 'H', 'F', 'S', '+',
    ' ', 'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'a', 't', 'a' };
static const uint16_t inode7[] = { 'i', 'N', 'o', 'd', 'e', '7' };

static fsimg_reader_t *
public_reader(fixture_t *f, const gen_image_t *image)
{
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	fixture_init(f, image, 0, 0);
	status = fsimg_reader_open(f->ctx, f->io, NULL, &reader, &error);
	expect_status("public autodetection open", status, FSIMG_OK, &error);
	return (reader);
}

static listing_t
list_image(const gen_image_t *image)
{
	listing_t result = { 0 };
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;

	fixture_init(&f, image, 0, 0);
	result.status = fsimg_reader_open(f.ctx, f.io, NULL, &reader,
	    &result.error);
	if (result.status != FSIMG_OK) {
		fixture_close(&f);
		return (result);
	}
	while ((result.status = fsimg_reader_next(reader, &entry,
	    &result.error)) == FSIMG_OK) {
		CHECK(result.count < 64);
		listed_entry_t *out = &result.entries[result.count++];
		CHECK(strlen(entry->path) < sizeof(out->path));
		strcpy(out->path, entry->path);
		out->kind = entry->kind;
		out->size = entry->size;
		out->metadata = entry->metadata;
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	return (result);
}

static listing_t
list_plan(const char *name, const gen_plan_t *plan, fsimg_status_t expected,
    const char *message)
{
	gen_image_t image;
	listing_t result;

	gen_build(plan, &image);
	result = list_image(&image);
	expect_status(name, result.status, expected, &result.error);
	if (message != NULL) {
		if (strcmp(result.error.message, message) != 0)
			fprintf(stderr, "%s: expected diagnostic '%s', got '%s'\n",
			    name, message, result.error.message);
		expect_true(name, strcmp(result.error.message, message) == 0);
	}
	gen_free(&image);
	return (result);
}

/* Find a generated record by its independent on-disk key and type. */
static gen_record_t *
find_record(gen_record_t *records, size_t count, uint32_t parent,
    uint16_t type)
{
	for (size_t i = 0; i < count; i++) {
		gen_record_t *r = &records[i];

		if (gen_load(r->bytes + 2, 4) == parent &&
		    gen_load(r->bytes + r->key_length, 2) == type)
			return (r);
	}
	CHECK(false);
	return (NULL);
}

static void
mutate_thread_kind(gen_record_t *r, size_t n)
{
	gen_record_t *thread = find_record(r, n, 32, 3);

	gen_store(thread->bytes + thread->key_length, 4, 2);
}

static void
mutate_thread_parent(gen_record_t *r, size_t n)
{
	gen_record_t *thread = find_record(r, n, 32, 3);

	gen_store(thread->bytes + thread->key_length + 4, 99, 4);
}

static void
mutate_thread_name(gen_record_t *r, size_t n)
{
	gen_record_t *thread = find_record(r, n, 32, 3);

	gen_store(thread->bytes + thread->key_length + 10, 'z', 2);
}

static void
mutate_child_thread(gen_record_t *r, size_t n)
{
	gen_record_t *file = find_record(r, n, 2, 2);

	gen_store(file->bytes + file->key_length, 4, 2);
}

static void
mutate_equal_keys(gen_record_t *r, size_t n)
{
	gen_record_t *file = find_record(r, n, 2, 2);
	size_t i = (size_t)(file - r);

	CHECK(i + 1 < n && r[i + 1].key_length == file->key_length);
	bcopy(file->bytes, r[i + 1].bytes, file->key_length);
}

static void
mutate_descending_keys(gen_record_t *r, size_t n)
{
	gen_record_t *file = find_record(r, n, 2, 2);

	gen_store(file->bytes + 8, 'z', 2);
}

static void
test_catalog_listing(void)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2 },
		{ .parent = 32, .name = name_c, .units = 1, .id = 34,
		    .type = 2 }
	};
	static const struct {
		const char *name;
		void (*mutate)(gen_record_t *, size_t);
		const char *message;
	} mutations[] = {
		{ "folder with file thread", mutate_thread_kind,
		    "HFS Plus thread record is malformed or of another kind" },
		{ "folder thread wrong parent", mutate_thread_parent,
		    "HFS Plus folder thread names another parent or name" },
		{ "folder thread wrong name", mutate_thread_name,
		    "HFS Plus folder thread names another parent or name" },
		{ "thread among children", mutate_child_thread,
		    "HFS Plus catalog record is not a folder or file" }
	};
	gen_plan_t plan;
	listing_t listed;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 3;
	listed = list_plan("nested catalog", &plan, FSIMG_DONE, NULL);
	expect_true("nested traversal order", listed.count == 3 &&
	    strcmp(listed.entries[0].path, "/a") == 0 &&
	    strcmp(listed.entries[1].path, "/a/c") == 0 &&
	    strcmp(listed.entries[2].path, "/b") == 0);
	for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		plan.mutate_catalog = mutations[i].mutate;
		list_plan(mutations[i].name, &plan, FSIMG_ECORRUPT,
		    mutations[i].message);
	}
	plan.mutate_catalog = NULL;
	objects[0].omit_thread = true;
	list_plan("missing folder thread", &plan, FSIMG_ECORRUPT,
	    "HFS Plus thread record is missing");
	objects[0].omit_thread = false;
	/* A second primary with the same folder ID cannot agree with the
	 * existing thread's parent and name. */
	objects[1].type = 1;
	objects[1].id = 32;
	objects[1].omit_thread = true;
	list_plan("reused folder ID", &plan, FSIMG_ECORRUPT,
	    "HFS Plus folder thread names another parent or name");
	objects[1].parent = 32;
	list_plan("folder cycle through descendant", &plan, FSIMG_ECORRUPT,
	    "HFS Plus folder thread names another parent or name");
	objects[1].parent = 0;
	objects[1].type = 2;
	objects[1].id = 33;
	objects[1].omit_thread = false;
	for (uint32_t id = 9; id <= 15; id++) {
		objects[1].id = id;
		list_plan("reserved file ID", &plan, FSIMG_ECORRUPT,
		    "HFS Plus file has a reserved node ID");
		objects[1].id = 33;
		objects[0].id = id;
		list_plan("reserved folder ID", &plan, FSIMG_ECORRUPT,
		    "HFS Plus folder has a reserved node ID");
		objects[0].id = 32;
	}

	/* Three root files put the second key on a new leaf when packed two
	 * records at a time; the first key remains a valid search anchor. */
	objects[0].type = 2;
	objects[2].parent = 0;
	for (unsigned boundary = 0; boundary < 2; boundary++) {
		plan.records_per_node = boundary ? 2 : 0;
		list_plan("ordered children", &plan, FSIMG_DONE, NULL);
		plan.mutate_catalog = mutate_equal_keys;
		list_plan("equal child keys", &plan, FSIMG_ECORRUPT,
		    "HFS Plus catalog keys are out of order");
		plan.mutate_catalog = mutate_descending_keys;
		list_plan("descending child keys", &plan, FSIMG_ECORRUPT,
		    "HFS Plus catalog keys are out of order");
		plan.mutate_catalog = NULL;
	}
}

static void
test_catalog_flags_kinds(void)
{
	static const struct {
		uint16_t type;
		uint16_t mode;
		fsimg_entry_kind_t kind;
	} kinds[] = {
		{ 1, 0040751, FSIMG_ENTRY_DIRECTORY },
		{ 1, 0, FSIMG_ENTRY_DIRECTORY },
		{ 2, 0106751, FSIMG_ENTRY_FILE },
		{ 2, 0, FSIMG_ENTRY_FILE },
		{ 2, 0010640, FSIMG_ENTRY_FIFO },
		{ 2, 0140640, FSIMG_ENTRY_SOCKET },
		{ 2, 0020640, FSIMG_ENTRY_CHAR_DEVICE },
		{ 2, 0060640, FSIMG_ENTRY_BLOCK_DEVICE }
	};
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2 };
	gen_plan_t plan;
	listing_t listed;
	char scenario[80];

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	for (uint16_t type = 1; type <= 2; type++) {
		object.type = type;
		for (unsigned bit = 0; bit < 16; bit++) {
			object.flags = (uint16_t)(1u << bit);
			bool unsupported = bit >= 12 || (type == 2 && bit == 4);
			const char *message = bit == 12 ?
			    "HFS Plus catalog record has expanded timestamps" :
			    "HFS Plus catalog record has an unknown flag";
			snprintf(scenario, sizeof(scenario), "record %u flag 0x%04x",
			    type, object.flags);
			listed = list_plan(scenario, &plan, unsupported ?
			    FSIMG_EUNSUPPORTED : FSIMG_DONE,
			    unsupported ? message : NULL);
			if (unsupported)
				expect_true("refused directory exposes no child",
				    listed.count == 0);
		}
	}
	object.flags = 0;
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		object.type = kinds[i].type;
		object.mode = kinds[i].mode;
		object.owner = 123;
		object.group = 456;
		object.special = 0xab123456;
		object.dates[0] = 0;
		object.dates[1] = 2082844801;
		object.dates[2] = 2082844802;
		object.dates[3] = 2082844803;
		listed = list_plan("kind table row", &plan, FSIMG_DONE, NULL);
		if (listed.count != 1) {
			expect_true("kind table delivers one entry", false);
			continue;
		}
		const listed_entry_t *e = &listed.entries[0];
		const fsimg_metadata_t *m = &e->metadata;
		expect_true("kind and size", e->kind == kinds[i].kind &&
		    e->size == 0);
		expect_true("four stored UTC times", m->created.seconds ==
		    -INT64_C(2082844800) && m->modified.seconds == 1 &&
		    m->changed.seconds == 2 && m->accessed.seconds == 3);
		if (object.mode == 0)
			expect_true("zero BSD omits owner and mode",
			    (m->valid & (FSIMG_META_OWNER | FSIMG_META_MODE)) == 0);
		else
			expect_true("owner and permission bits",
			    (m->valid & (FSIMG_META_OWNER | FSIMG_META_MODE)) ==
			    (FSIMG_META_OWNER | FSIMG_META_MODE) && m->uid == 123 &&
			    m->gid == 456 && m->mode == (object.mode & 07777));
		if (e->kind == FSIMG_ENTRY_CHAR_DEVICE ||
		    e->kind == FSIMG_ENTRY_BLOCK_DEVICE)
			expect_true("device split", (m->valid & FSIMG_META_DEVICE) &&
			    m->dev_major == 0xab && m->dev_minor == 0x123456);
	}
	for (unsigned type = 1; type <= 2; type++) {
		object.type = (uint16_t)type;
		object.mode = 0644;
		list_plan("permissions without type", &plan, FSIMG_ECORRUPT,
		    type == 1 ? "HFS Plus folder's mode is not a directory's" :
		    "HFS Plus file's mode is not in the kind table");
		object.mode = 0030644;
		list_plan("unlisted mode type", &plan, FSIMG_ECORRUPT,
		    type == 1 ? "HFS Plus folder's mode is not a directory's" :
		    "HFS Plus file's mode is not in the kind table");
	}
	object.type = 2;
	object.data_size = 1;
	for (size_t i = 5; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		object.mode = kinds[i].mode;
		list_plan("nonempty socket/device fork", &plan, FSIMG_ECORRUPT,
		    "HFS Plus socket or device has a nonempty fork");
	}
}

/* Public read_at intentionally accepts regular files only. The common content
 * delivery primitive also serves symlinks during extraction. */
static void
check_content(const char *name, const gen_plan_t *plan, const char *path,
    const unsigned char *wanted, size_t length, fsimg_status_t expected,
    const char *message)
{
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char *bytes = malloc(length + 1);
	size_t count = 0, stored;
	bool found = false;

	CHECK(bytes != NULL);
	gen_build(plan, &image);
	reader = public_reader(&f, &image);
	if (reader != NULL) {
		while ((status = fsimg_reader_next(reader, &entry, &error)) ==
		    FSIMG_OK) {
			if (strcmp(entry->path, path) != 0)
				continue;
			found = true;
			if (entry->kind == FSIMG_ENTRY_SYMLINK)
				status = fsimg_reader_content(reader, 0, bytes,
				    length, &count, &stored, &error);
			else
				status = fsimg_reader_read_at(reader, 0, bytes,
				    length, &count, &error);
			expect_status(name, status, expected, &error);
			if (message != NULL)
				expect_true(name, strcmp(error.message, message) == 0);
			if (status == FSIMG_OK)
				expect_true(name, count == length &&
				    bcmp(bytes, wanted, length) == 0);
			break;
		}
		expect_true("requested content entry exists", found);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	free(bytes);
}

static void
test_symbolic_links(void)
{
	unsigned char target[1024];
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .mode = 0120777, .data = target };
	gen_plan_t plan;

	memset(target, 'x', sizeof(target));
	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	for (size_t size = 1; size <= 1023; size += 1022) {
		object.data_size = size;
		listing_t listed = list_plan("symlink listed", &plan, FSIMG_DONE, NULL);
		expect_true("symbolic-link kind and size", listed.count == 1 &&
		    listed.entries[0].kind == FSIMG_ENTRY_SYMLINK &&
		    listed.entries[0].size == size);
		check_content("symlink target delivered", &plan, "/a", target,
		    size, FSIMG_OK, NULL);
	}
	object.data_size = 1024;
	list_plan("symlink 1024 refused", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus symbolic link's target length is not 1 to 1023 bytes");
	object.data_size = 0;
	list_plan("symlink empty refused", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus symbolic link's target length is not 1 to 1023 bytes");
	object.data_size = 1;
	target[0] = 0;
	list_plan("NUL target listing reads no bytes", &plan, FSIMG_DONE, NULL);
	check_content("NUL target refused on delivery", &plan, "/a", target,
	    1, FSIMG_EUNSUPPORTED,
	    "HFS Plus symbolic link's target holds a NUL byte");
	target[0] = 'x';
	object.resource_size = 1;
	list_plan("symlink resource fork refused", &plan, FSIMG_ECORRUPT,
	    "HFS Plus symbolic link has a resource fork");
}

static void
test_hard_links(void)
{
	static const unsigned char payload[] = "indirect payload";
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2,
		    .special = 7, .finder = "hlnkhfs+", .dates = { 42 },
		    .owner = UINT32_MAX, .group = UINT32_MAX },
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_c, .units = 1, .id = 34, .type = 2,
		    .special = 7, .finder = "hlnkhfs+", .parent = 33,
		    .dates = { 42 } },
		{ .name = private_data, .units = 21, .id = 40, .type = 1,
		    .dates = { 42 } },
		{ .name = inode7, .units = 6, .id = 41, .parent = 40,
		    .type = 2, .mode = 0100641, .owner = 123, .group = 456,
		    .data = payload, .data_size = sizeof(payload) - 1,
		    .reserved = UINT32_MAX },
		{ .name = name_a, .units = 1, .id = 36, .parent = 33,
		    .type = 2, .special = 7, .finder = "hlnkhfs+",
		    .dates = { 42 } }
	};
	gen_plan_t plan;
	listing_t listed;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 5;
	listed = list_plan("two hard-link names in two directories", &plan,
	    FSIMG_DONE, NULL);
	expect_true("private node hidden and metadata taken from node",
	    listed.count == 3 && listed.entries[0].metadata.uid == 123 &&
	    listed.entries[2].metadata.mode == 0641 &&
	    listed.entries[0].size == sizeof(payload) - 1 &&
	    strcmp(listed.entries[2].path, "/b/c") == 0);
	check_content("first stub content", &plan, "/a", payload,
	    sizeof(payload) - 1, FSIMG_OK, NULL);
	check_content("second stub content", &plan, "/b/c", payload,
	    sizeof(payload) - 1, FSIMG_OK, NULL);
	plan.object_count = 6;
	listed = list_plan("three names across directories", &plan, FSIMG_DONE, NULL);
	expect_true("three names use node metadata", listed.count == 4 &&
	    strcmp(listed.entries[2].path, "/b/a") == 0 &&
	    strcmp(listed.entries[3].path, "/b/c") == 0 &&
	    listed.entries[2].metadata.uid == 123 &&
	    listed.entries[3].metadata.mode == 0641 &&
	    listed.entries[0].metadata.created.seconds == -INT64_C(2082844800));
	check_content("third stub content", &plan, "/b/a", payload,
	    sizeof(payload) - 1, FSIMG_OK, NULL);
	plan.object_count = 5;
	objects[0].mode = 0100444;
	objects[0].flags = 0x20;
	list_plan("current stub mode and flag", &plan, FSIMG_DONE, NULL);
	objects[0].mode = 0;
	objects[0].flags = 0;
	list_plan("zero-mode stub without chain flag", &plan, FSIMG_DONE, NULL);
	static const uint16_t attr_name[] = { 'x' };
	gen_attr_value_t attr = { .id = 41, .name = attr_name, .units = 1,
	    .type = 0x10 };
	plan.attributes = true;
	plan.values = &attr;
	plan.value_count = 1;
	objects[4].flags = 4;
	listed = list_plan("hard-link mark from indirect node", &plan, FSIMG_DONE, NULL);
	expect_true("both stubs carry node mark", listed.count == 3 &&
	    (listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) &&
	    (listed.entries[2].metadata.valid & FSIMG_META_EXTRA_DATA));
	objects[4].flags = 0;
	objects[0].flags = 4;
	attr.id = 32;
	listed = list_plan("stub attributes do not mark effective node", &plan,
	    FSIMG_DONE, NULL);
	expect_true("stub mark absent", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	objects[0].flags = 0;
	plan.attributes = false;
	plan.value_count = 0;
	objects[0].dates[0] = 0;
	list_plan("root-date stub with different private date", &plan,
	    FSIMG_DONE, NULL);
	objects[0].dates[0] = 43;
	listed = list_plan("Finder file codes with another date", &plan,
	    FSIMG_DONE, NULL);
	expect_true("ordinary file uses own size", listed.entries[0].size == 0);
	objects[0].dates[0] = 42;
	objects[0].special = 0;
	list_plan("zero hard-link reference", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a zero reference");
	objects[0].special = 7;
	objects[0].data_size = 1;
	list_plan("stub nonempty data", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a nonempty fork");
	objects[0].data_size = 0;
	objects[0].resource_size = 1;
	list_plan("stub nonempty resource", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a nonempty fork");
	objects[0].resource_size = 0;
	objects[4].name = name_c;
	objects[4].units = 1;
	list_plan("missing indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub's indirect node is missing");
	static const uint16_t lower_inode[] = { 'i', 'n', 'o', 'd', 'e', '7' };
	objects[4].name = lower_inode;
	objects[4].units = 6;
	list_plan("case variant indirect node is missing", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub's indirect node is missing");
	objects[4].name = inode7;
	objects[4].mode = 0010644;
	list_plan("FIFO indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	objects[4].type = 1;
	objects[4].mode = 0040755;
	list_plan("folder indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	objects[4].type = 2;
	objects[4].mode = 0100644;
	bcopy("hlnkhfs+", objects[4].finder, 8);
	list_plan("indirect node is itself stub", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	bzero(objects[4].finder, 32);
	objects[4].mode = 0120777;
	listed = list_plan("hard-linked symbolic link", &plan, FSIMG_DONE, NULL);
	expect_true("indirect node decides link kind",
	    listed.entries[0].kind == FSIMG_ENTRY_SYMLINK);
	check_content("hard-linked target", &plan, "/a", payload,
	    sizeof(payload) - 1, FSIMG_OK, NULL);
	objects[4].mode = 0100644;
	objects[3].type = 2;
	list_plan("private data lookup requires folder", &plan, FSIMG_ECORRUPT,
	    "HFS Plus private data folder is not a folder");
	bzero(objects[0].finder, 32);
	bzero(objects[2].finder, 32);
	list_plan("root listing validates private folder", &plan, FSIMG_ECORRUPT,
	    "HFS Plus private folder is not a folder");
	objects[3].type = 1;
	bcopy("fdrpMACS", objects[0].finder, 8);
	objects[0].flags = 0x20;
	listed = list_plan("directory hard link", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus directory hard link is not supported");
	expect_true("directory link names path and exposes no siblings",
	    strcmp(listed.error.path, "/a") == 0 && listed.count == 0);
	objects[0].flags = 0;
	list_plan("Finder alias without chain flag", &plan, FSIMG_DONE, NULL);
	objects[0].flags = 0x20;
	objects[0].dates[0] = 43;
	list_plan("Finder alias with another date", &plan, FSIMG_DONE, NULL);
	objects[0].dates[0] = 0;
	bcopy("hlnkhfs+", objects[0].finder, 8);
	plan.object_count = 1;
	list_plan("root-date stub without data folder", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub on a volume with no private data folder");
}

static void
test_attributes_compression(void)
{
	static const uint16_t attr_name[] = { 'x' };
	static const uint16_t cmp_name[] = { 'c', 'o', 'm', '.', 'a', 'p', 'p',
	    'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };
	static const uint16_t acl_name[] = { 'c', 'o', 'm', '.', 'a', 'p', 'p',
	    'l', 'e', '.', 's', 'y', 's', 't', 'e', 'm', '.', 'S', 'e', 'c',
	    'u', 'r', 'i', 't', 'y' };
	unsigned char cmp[16] = { 'f', 'p', 'm', 'c', 3, 0, 0, 0,
	    1, 0, 0, 0, 0, 0, 0, 0 };
	gen_attr_value_t attr = { .id = 32, .name = attr_name,
	    .units = 1, .type = 0x10 };
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2 }
	};
	gen_plan_t plan;
	listing_t listed;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 2;
	plan.values = &attr;
	plan.value_count = 1;
	plan.attributes = true;
	listed = list_plan("unmarked file has no extra data", &plan,
	    FSIMG_DONE, NULL);
	expect_true("clear flag skips attributes", listed.count == 2 &&
	    (listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) == 0);
	objects[0].flags = 4;
	for (unsigned type = 0x10; type <= 0x30; type += 0x10) {
		attr.type = type;
		attr.value = (const unsigned char *)"hi";
		attr.size = 1;
		listed = list_plan("stored attribute mark", &plan, FSIMG_DONE, NULL);
		expect_true("extra data valid but not required", listed.count == 2 &&
		    (listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) &&
		    !(listed.entries[0].metadata.required & FSIMG_META_EXTRA_DATA) &&
		    !(listed.entries[1].metadata.valid & FSIMG_META_EXTRA_DATA));
	}
	attr.type = 0x10;
	attr.name = acl_name;
	attr.units = sizeof(acl_name) / sizeof(acl_name[0]);
	listed = list_plan("ACL is stored extra data", &plan, FSIMG_DONE, NULL);
	expect_true("ACL mark", listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA);
	objects[0].type = 1;
	listed = list_plan("folder attribute", &plan, FSIMG_DONE, NULL);
	expect_true("folder extra-data mark", listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA);
	objects[0].type = 2;
	attr.id = 31;
	listed = list_plan("neighbour attribute does not leak", &plan,
	    FSIMG_DONE, NULL);
	expect_true("neighbour mark absent", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	attr.id = 34;
	listed = list_plan("following neighbour does not leak", &plan, FSIMG_DONE, NULL);
	expect_true("following mark absent", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	attr.id = 32;
	plan.value_count = 0;
	listed = list_plan("set flag without record", &plan, FSIMG_DONE, NULL);
	expect_true("no record no mark", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	plan.attributes = false;
	listed = list_plan("absent attribute tree", &plan, FSIMG_DONE, NULL);
	expect_true("absent tree no mark", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	objects[0].resource_size = 1;
	listed = list_plan("resource fork extra data", &plan, FSIMG_DONE, NULL);
	expect_true("resource mark", listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA);
	objects[0].resource_size = 0;
	plan.attributes = true;
	plan.value_count = 1;
	attr.type = 0x40;
	list_plan("unknown attribute type", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus attribute record has an unknown type");
	attr.type = 0x10;
	attr.name = cmp_name;
	attr.units = sizeof(cmp_name) / sizeof(cmp_name[0]);
	attr.value = cmp;
	attr.size = sizeof(cmp);
	listed = list_plan("decmpfs without compression flag", &plan, FSIMG_DONE, NULL);
	expect_true("ordinary decmpfs has mark", listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA);
	objects[0].owner_flags = 0x20;
	listed = list_plan("compressed inline refusal", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus compressed file is not supported");
	expect_true("compression refusal path and no children",
	    strcmp(listed.error.path, "/a") == 0 && listed.count == 0);
	plan.value_count = 0;
	list_plan("compressed missing attribute", &plan, FSIMG_ECORRUPT,
	    "HFS Plus compressed file has no compression attribute");
	plan.value_count = 1;
	cmp[0] = 'x';
	list_plan("bad compression signature", &plan, FSIMG_ECORRUPT,
	    "HFS Plus compression attribute's header is malformed");
	cmp[0] = 'f';
	attr.size = 14;
	list_plan("short compression header", &plan, FSIMG_ECORRUPT,
	    "HFS Plus compression attribute's header is malformed");
	attr.size = 16;
	objects[0].data_size = 1;
	list_plan("compressed nonempty data fork", &plan, FSIMG_ECORRUPT,
	    "HFS Plus compressed file has a nonempty data fork");
	objects[0].data_size = 0;
	attr.type = 0x20;
	list_plan("fork compression attribute", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus compression attribute is not stored inline");
	objects[0].mode = 0120777;
	objects[0].data_size = 1;
	objects[0].data = (const unsigned char *)"x";
	list_plan("symlink owner flag is not compression", &plan, FSIMG_DONE, NULL);
}

static void
test_names_listing(void)
{
	static const struct {
		const char *name;
		uint16_t stored[12];
		size_t units;
		const char *display;
		fsimg_status_t status;
	} rows[] = {
		{ "slash", { '/' }, 1, "/:", FSIMG_DONE },
		{ "NUL unit", { 0 }, 1, "/\xe2\x90\x80", FSIMG_DONE },
		{ "stored colon", { ':' }, 1, NULL, FSIMG_EUNSUPPORTED },
		{ "stored NUL symbol", { 0x2400 }, 1, NULL, FSIMG_EUNSUPPORTED },
		{ "dot", { '.' }, 1, NULL, FSIMG_EUNSUPPORTED },
		{ "dot dot", { '.', '.' }, 2, NULL, FSIMG_EUNSUPPORTED },
		{ "Icon CR", { 'I', 'c', 'o', 'n', 13 }, 5, "/Icon\r", FSIMG_DONE },
		{ "percent text", { '%','E','F','%','B','F','%','B','F' }, 9,
		    "/%EF%BF%BF", FSIMG_DONE },
		{ "all ignorable", { 0x200c }, 1, "/\xe2\x80\x8c", FSIMG_DONE },
		{ "precomposed", { 0xe9 }, 1, NULL, FSIMG_EUNSUPPORTED },
		{ "unpaired surrogate", { 0xd800 }, 1, NULL, FSIMG_ECORRUPT }
	};
	gen_object_t objects[] = {
		{ .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2 }
	};
	gen_plan_t plan;
	listing_t listed;
	uint16_t long_name[255];

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 1;
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		objects[0].name = rows[i].stored;
		objects[0].units = rows[i].units;
		const char *diagnostic = NULL;

		if (rows[i].status == FSIMG_ECORRUPT)
			diagnostic = "HFS Plus name has an unpaired surrogate";
		else if (rows[i].status == FSIMG_EUNSUPPORTED) {
			if (i == 4 || i == 5)
				diagnostic = "HFS Plus name is a dot name";
			else
				diagnostic = "HFS Plus name does not map back from its image path";
		}
		listed = list_plan(rows[i].name, &plan, rows[i].status, diagnostic);
		if (rows[i].status == FSIMG_DONE)
			expect_true(rows[i].name, listed.count == 1 &&
			    strcmp(listed.entries[0].path, rows[i].display) == 0);
		else {
			expect_true("invalid name makes directory unlistable",
			    listed.count == 0);
		}
	}
	for (size_t i = 0; i < 255; i++)
		long_name[i] = 'x';
	objects[0].name = long_name;
	for (size_t units = 1; units <= 255; units += units == 1 ? 253 : 1) {
		objects[0].units = units;
		listed = list_plan("name length limit", &plan, FSIMG_DONE, NULL);
		expect_true("name units preserved", listed.count == 1 &&
		    strlen(listed.entries[0].path) == units + 1);
	}
	long_name[253] = 0xd83d;
	long_name[254] = 0xde00;
	list_plan("surrogate pair at 255 units", &plan, FSIMG_DONE, NULL);
	objects[0].name = rows[8].stored;
	objects[0].units = 1;
	plan.object_count = 2;
	listed = list_plan("ignorable child before sibling", &plan, FSIMG_DONE, NULL);
	expect_true("thread then ignorable then sibling", listed.count == 2 &&
	    strcmp(listed.entries[0].path, "/\xe2\x80\x8c") == 0);
	static const uint16_t ignorable[] = { 0x200d };
	objects[1].name = ignorable;
	list_plan("two all-ignorable siblings", &plan, FSIMG_ECORRUPT,
	    "HFS Plus catalog keys are out of order");
	static const uint16_t upper_a[] = { 'A' };
	objects[0].name = upper_a;
	objects[1].name = name_a;
	list_plan("folded case siblings collide", &plan, FSIMG_ECORRUPT,
	    "HFS Plus catalog keys are out of order");
	plan.hfsx = true;
	plan.compare_type = 0xbc;
	list_plan("binary case siblings coexist", &plan, FSIMG_DONE, NULL);
}

static unsigned char *
catalog_data(gen_image_t *image, uint32_t parent, uint16_t type)
{
	gen_tree_t *tree = &image->trees[GEN_CATALOG];
	gen_record_t *record = find_record(tree->records, tree->record_count,
	    parent, type);

	return (image->bytes + record->offset + record->key_length);
}

static void
expect_listing(const char *name, const gen_image_t *image,
    fsimg_status_t expected, const char *message)
{
	listing_t listed = list_image(image);

	expect_status(name, listed.status, expected, &listed.error);
	if (message != NULL) {
		if (strcmp(listed.error.message, message) != 0)
			fprintf(stderr, "%s: diagnostic: %s\n", name,
			    listed.error.message);
		expect_true(name, strcmp(listed.error.message, message) == 0);
	}
}

static void
test_folder_counts_and_parents(void)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1 },
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_c, .units = 1, .id = 34, .type = 1, .parent = 32 },
		{ .name = name_a, .units = 1, .id = 35, .type = 1, .parent = 33 }
	};
	gen_plan_t plan;
	gen_image_t image;
	unsigned char *folder;
	uint32_t saved;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 4;
	gen_build(&plan, &image);
	expect_listing("folder count baseline", &image, FSIMG_DONE, NULL);
	for (unsigned which = 0; which < 2; which++) {
		folder = catalog_data(&image, which ? 2 : 1, 1);
		saved = (uint32_t)gen_load(folder + 4, 4);
		for (int delta = -1; delta <= 1; delta += 2) {
			gen_store(folder + 4, saved + delta, 4);
			expect_listing("valence off by one", &image, FSIMG_ECORRUPT,
			    "HFS Plus folder's valence disagrees with its children");
		}
		gen_store(folder + 4, saved, 4);
		saved = (uint32_t)gen_load(folder + 84, 4);
		gen_store(folder + 84, saved + 1, 4);
		expect_listing("folder count ignored without flag", &image,
		    FSIMG_DONE, NULL);
		gen_store(folder + 2, 0x10, 2);
		expect_listing("folder count checked with flag", &image,
		    FSIMG_ECORRUPT,
		    "HFS Plus folder count disagrees with its child folders");
		gen_store(folder + 84, saved, 4);
		expect_listing("correct flagged folder count", &image,
		    FSIMG_DONE, NULL);
		gen_store(folder + 2, 0, 2);
	}
	folder = catalog_data(&image, 33, 1);
	gen_store(folder + 8, 34, 4);
	expect_listing("folder reachable from second parent", &image,
	    FSIMG_ECORRUPT,
	    "HFS Plus folder thread names another parent or name");
	gen_free(&image);
	objects[3].parent = 34;
	gen_build(&plan, &image);
	expect_listing("acyclic folder baseline", &image, FSIMG_DONE, NULL);
	folder = catalog_data(&image, 34, 1);
	gen_store(folder + 8, 32, 4);
	expect_listing("folder inside own descendant", &image, FSIMG_ECORRUPT,
	    "HFS Plus folder thread names another parent or name");
	gen_free(&image);
}

/* Odd record lengths necessarily make an odd node offset too. Test the exact
 * decoder directly as well, so the listing's framing rejection cannot stand in
 * for evidence that the record decoder checks its own length. */
static void
test_catalog_exact_lengths(void)
{
	gen_plan_t plan;
	gen_image_t image;
	gen_object_t object = { .name = name_a, .units = 1, .id = 32 };
	hfsplus_folder_t folder;
	hfsplus_file_t file;
	hfsplus_thread_t thread;
	unsigned char raw[249];

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	for (uint16_t type = 1; type <= 4; type++) {
		object.type = type == 1 || type == 3 ? 1 : 2;
		gen_build(&plan, &image);
		expect_listing("exact catalog record baseline", &image, FSIMG_DONE, NULL);
		gen_tree_t *tree = &image.trees[GEN_CATALOG];
		gen_record_t *r = find_record(tree->records, tree->record_count,
		    type <= 2 ? 2 : 32, type);
		size_t length = r->length - r->key_length;
		bzero(raw, sizeof(raw));
		bcopy(image.bytes + r->offset + r->key_length, raw, length);
		for (int delta = -1; delta <= 1; delta++) {
			size_t size = (size_t)((int)length + delta);
			bool decoded;

			switch (type) {
			case 1:
				decoded = hfsplus_folder_decode(raw, size, &folder);
				break;
			case 2:
				decoded = hfsplus_file_decode(raw, size, &file);
				break;
			default:
				decoded = hfsplus_thread_decode(raw, size, &thread);
				break;
			}
			expect_true("catalog exact length decoder", decoded == (delta == 0));
			if (delta == 0)
				continue;
			uint64_t node_start = gen_node_offset(&image, GEN_CATALOG,
			    tree->first_leaf);
			unsigned char *node = image.bytes + node_start;
			size_t records = (size_t)gen_load(node + 10, 2);
			bool changed = false;
			/* Move the offset immediately after this record by one. */
			for (size_t i = 1; i <= records; i++) {
				unsigned char *slot = node + tree->node_size - 2 * (i + 1);
				uint64_t value = gen_load(slot, 2);

				if (value == r->offset - node_start + r->length) {
					gen_store(slot, value + delta, 2);
					expect_listing("odd catalog record framing",
					    &image, FSIMG_ECORRUPT,
					    "HFS Plus node's offset table is out of order");
					gen_store(slot, value, 2);
					changed = true;
					break;
				}
			}
			expect_true("length mutation reached record", changed);
		}
		gen_free(&image);
	}
	object.type = 2;
	gen_build(&plan, &image);
	gen_store(catalog_data(&image, 2, 2), 99, 2);
	expect_listing("unknown catalog record", &image, FSIMG_ECORRUPT,
	    "HFS Plus catalog record is not a folder or file");
	gen_free(&image);
}

static void
check_selection(const gen_plan_t *plan, const char *query,
    const char *display, fsimg_status_t expected)
{
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	gen_build(plan, &image);
	reader = public_reader(&f, &image);
	if (reader != NULL) {
		CHECK(fsimg_selection_new(reader, &query, 1, FSIMG_SELECTION_FILE,
		    &selection, &error) == FSIMG_OK);
		status = fsimg_selection_next(selection, &entry, &error);
		expect_status(query, status, expected, &error);
		if (status == FSIMG_EINVAL) {
			expect_true("missing selection names its query",
			    strcmp(error.path, query) == 0 && strcmp(error.message,
			    "selected image path was not found") == 0);
		}
		if (status == FSIMG_OK) {
			expect_true("selection returns stored display", strcmp(entry->path,
			    display) == 0);
			expect_status("selected scope ends", fsimg_selection_next(
			    selection, &entry, &error), FSIMG_DONE, &error);
		}
		fsimg_selection_free(selection);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
}

static void
test_names_selection_owned(void)
{
	static const uint16_t slash[] = { '/' };
	static const uint16_t nul[] = { 0 };
	static const uint16_t ignored[] = { 0x200c };
	static const uint16_t decomposed[] = { 'e', 0x301 };
	static const uint16_t upper[] = { 'A' };
	static const uint16_t journal[] = { '.', 'j', 'o', 'u', 'r', 'n', 'a', 'l' };
	static const uint16_t private_dir[] = { '.', 'H', 'F', 'S', '+', ' ',
	    'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'i', 'r', 'e', 'c',
	    't', 'o', 'r', 'y', ' ', 'D', 'a', 't', 'a', 13 };
	gen_object_t object = { .name = slash, .units = 1, .id = 32, .type = 2 };
	gen_plan_t plan;
	listing_t listed;
	uint16_t variant[29];

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	check_selection(&plan, "/:", "/:", FSIMG_OK);
	object.name = nul;
	check_selection(&plan, "/\xe2\x90\x80", "/\xe2\x90\x80", FSIMG_OK);
	object.name = ignored;
	check_selection(&plan, "/\xe2\x80\x8c", "/\xe2\x80\x8c", FSIMG_OK);
	object.name = decomposed;
	object.units = 2;
	check_selection(&plan, "/\xc3\xa9", "/e\xcc\x81", FSIMG_OK);
	object.name = upper;
	object.units = 1;
	check_selection(&plan, "/a", "/A", FSIMG_OK);
	plan.hfsx = true;
	plan.compare_type = 0xbc;
	check_selection(&plan, "/A", "/A", FSIMG_OK);
	check_selection(&plan, "/a", NULL, FSIMG_EINVAL);
	plan.hfsx = false;
	plan.compare_type = 0xcf;
	object.type = 1;
	for (unsigned which = 0; which < 2; which++) {
		object.name = which ? private_dir : private_data;
		object.units = which ? 29 : 21;
		listed = list_plan("private folder exact hidden", &plan, FSIMG_DONE, NULL);
		expect_true("private folder valence counted", listed.count == 0);
		bcopy(object.name, variant, object.units * sizeof(variant[0]));
		variant[which ? 1 : 4] = 'h';
		object.name = variant;
		listed = list_plan("case variant private name delivered", &plan, FSIMG_DONE, NULL);
		expect_true("private-name variant ordinary entry", listed.count == 1);
	}
	object.name = journal;
	object.units = 8;
	object.type = 2;
	listed = list_plan("unjournaled .journal delivered", &plan, FSIMG_DONE, NULL);
	expect_true("ordinary journal name", listed.count == 1 &&
	    strcmp(listed.entries[0].path, "/.journal") == 0);
	plan.objects = NULL;
	plan.object_count = 0;
	listed = list_plan("empty volume", &plan, FSIMG_DONE, NULL);
	expect_true("empty volume has no entries", listed.count == 0);
	plan.journal = true;
	plan.journal_info_file = true;
	listed = list_plan("journal-owned files hidden", &plan, FSIMG_DONE, NULL);
	expect_true("journal files not delivered", listed.count == 0);
	gen_image_t image;
	gen_build(&plan, &image);
	gen_tree_t *tree = &image.trees[GEN_CATALOG];
	gen_record_t *info = &tree->records[3];
	CHECK(gen_load(info->bytes + 6, 2) == 19);
	gen_store(image.bytes + info->offset + info->key_length + 88 + 16,
	    image.journal_info / image.block_size + 1, 4);
	expect_listing("journal info fork disagreement", &image, FSIMG_ECORRUPT,
	    "HFS Plus .journal_info_block disagrees with the header");
	gen_free(&image);
	plan.journal_info_file = false;
	list_plan("journal info file missing", &plan, FSIMG_ECORRUPT,
	    "HFS Plus journaled volume is missing a journal file");
}

static void
test_attribute_key_order(void)
{
	static const uint16_t x[] = { 'x' }, y[] = { 'y' };
	gen_attr_value_t values[] = {
		{ .id = 32, .name = x, .units = 1, .type = 0x10 },
		{ .id = 32, .name = y, .units = 1, .type = 0x10 }
	};
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .flags = 4 };
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	plan.attributes = true;
	plan.values = values;
	plan.value_count = 2;
	for (unsigned boundary = 0; boundary < 2; boundary++) {
		plan.records_per_node = boundary ? 2 : 0;
		gen_build(&plan, &image);
		expect_listing("attribute order baseline", &image, FSIMG_DONE, NULL);
		gen_record_t *r = &image.trees[GEN_ATTRIBUTES].records[1];
		gen_store(image.bytes + r->offset + 14, 'a', 2);
		expect_listing("descending attribute key", &image, FSIMG_ECORRUPT,
		    "HFS Plus attribute keys are out of order");
		gen_free(&image);
	}
}

static void
test_mixed_round_trip(void)
{
	static const uint16_t d[] = { 'd' }, e[] = { 'e' }, f_name[] = { 'f' };
	static const unsigned char target[] = "a/a";
	static const unsigned char linked[] = "shared bytes";
	unsigned char fragmented[9 * 4096 + 13], zeros[65537] = { 0 };
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1, .mode = 0040750 },
		{ .name = name_a, .units = 1, .id = 33, .type = 2, .parent = 32,
		    .mode = 0100640, .data = fragmented,
		    .data_size = sizeof(fragmented), .fragmented = true },
		{ .name = name_b, .units = 1, .id = 34, .type = 2,
		    .mode = 0100644, .data_size = sizeof(zeros) },
		{ .name = name_c, .units = 1, .id = 35, .type = 2,
		    .mode = 0120777, .data = target, .data_size = sizeof(target) - 1 },
		{ .name = d, .units = 1, .id = 36, .type = 2, .mode = 0010600 },
		{ .name = e, .units = 1, .id = 37, .type = 2, .special = 7,
		    .finder = "hlnkhfs+" },
		{ .name = f_name, .units = 1, .id = 38, .type = 2, .special = 7,
		    .finder = "hlnkhfs+" },
		{ .name = private_data, .units = 21, .id = 40, .type = 1 },
		{ .name = inode7, .units = 6, .id = 41, .type = 2, .parent = 40,
		    .mode = 0100600, .data = linked, .data_size = sizeof(linked) - 1 }
	};
	const struct {
		const char *path;
		fsimg_entry_kind_t kind;
		const unsigned char *data;
		size_t size;
		unsigned mode;
	} expected[] = {
		{ "/a", FSIMG_ENTRY_DIRECTORY, NULL, 0, 0750 },
		{ "/a/a", FSIMG_ENTRY_FILE, fragmented, sizeof(fragmented), 0640 },
		{ "/b", FSIMG_ENTRY_FILE, zeros, sizeof(zeros), 0644 },
		{ "/c", FSIMG_ENTRY_SYMLINK, target, sizeof(target) - 1, 0777 },
		{ "/d", FSIMG_ENTRY_FIFO, NULL, 0, 0600 },
		{ "/e", FSIMG_ENTRY_FILE, linked, sizeof(linked) - 1, 0600 },
		{ "/f", FSIMG_ENTRY_FILE, linked, sizeof(linked) - 1, 0600 }
	};
	gen_plan_t plan;
	gen_image_t image;
	fixture_t fixture;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	fsimg_extract_options_t options;
	fsimg_stats_t stats;
	unsigned char bytes[sizeof(zeros)];
	size_t count, stored;
	char temporary[] = "hfsplus-roundtrip-XXXXXX";
	int parent, root, fd;
	struct stat st;

	for (size_t i = 0; i < sizeof(fragmented); i++)
		fragmented[i] = (unsigned char)(i % 251);
	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	gen_build(&plan, &image);
	for (unsigned content = 0; content < 2; content++) {
		reader = public_reader(&fixture, &image);
		if (reader != NULL) {
			for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
				status = fsimg_reader_next(reader, &entry, &error);
				expect_status(expected[i].path, status, FSIMG_OK, &error);
				if (status != FSIMG_OK)
					break;
				expect_true("mixed entry name kind size and mode",
				    strcmp(entry->path, expected[i].path) == 0 &&
				    entry->kind == expected[i].kind &&
				    entry->size == expected[i].size &&
				    entry->metadata.mode == expected[i].mode);
				expect_true("advance releases per-file extent index",
				    hfsplus_state(reader)->file == NULL);
				if (!content || expected[i].size == 0)
					continue;
				if (entry->kind == FSIMG_ENTRY_SYMLINK)
					status = fsimg_reader_content(reader, 0, bytes,
					    expected[i].size, &count, &stored, &error);
				else
					status = fsimg_reader_read_at(reader, 0, bytes,
					    expected[i].size, &count, &error);
				expect_status("mixed content delivery", status, FSIMG_OK, &error);
				expect_true("mixed content exact bytes", count == expected[i].size &&
				    bcmp(bytes, expected[i].data, count) == 0);
			}
			expect_status("mixed traversal complete", fsimg_reader_next(reader,
			    &entry, &error), FSIMG_DONE, &error);
			fsimg_reader_free(reader);
		}
		fixture_close(&fixture);
	}
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	/* Extraction's safe profile deliberately refuses links and FIFOs and
	 * creates host modes 0600/0700 instead of restoring image modes. */
	for (unsigned skip = 0; skip < 2; skip++) {
		reader = public_reader(&fixture, &image);
		fsimg_extract_options_init(&options);
		if (skip)
			options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
		status = fsimg_extract_at(reader, parent, "out", &options, &error);
		expect_status("mixed extraction", status, skip ? FSIMG_OK :
		    FSIMG_EUNSUPPORTED, &error);
		if (!skip)
			expect_true("extraction names first unsupported kind",
			    strcmp(error.path, "/c") == 0);
		else {
			CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
			expect_true("extraction counts two unsupported entries",
			    stats.skipped_entries == 2);
		}
		root = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(root != -1);
		for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
			const char *path = expected[i].path + 1;

			if (expected[i].kind != FSIMG_ENTRY_FILE || (!skip && i >= 3))
				continue;
			fd = openat(root, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			CHECK(fd != -1);
			CHECK(fstat(fd, &st) == 0);
			expect_true("extracted size and safe host mode", st.st_size ==
			    (off_t)expected[i].size && (st.st_mode & 07777) == 0600);
			count = 0;
			while (count < expected[i].size) {
				ssize_t got = read(fd, bytes + count, expected[i].size - count);

				CHECK(got > 0);
				count += (size_t)got;
			}
			expect_true("extracted bytes", bcmp(bytes, expected[i].data, count) == 0);
			CHECK(close(fd) == 0);
			CHECK(unlinkat(root, path, 0) == 0);
		}
		CHECK(fstatat(root, "a", &st, AT_SYMLINK_NOFOLLOW) == 0);
		expect_true("extracted directory safe mode", (st.st_mode & 07777) == 0700);
		CHECK(unlinkat(root, "a", AT_REMOVEDIR) == 0);
		CHECK(close(root) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
		fsimg_reader_free(reader);
		fixture_close(&fixture);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	gen_free(&image);
}

static void
test_probe_exclusivity(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fsimg_format_info_t info;
	fsimg_error_t error = { 0 };
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	fsimg_survey_t survey;
	size_t cursor = 0;

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	while (fsimg_format_next(&cursor, &info, &error) == FSIMG_OK) {
		format = fsimg_format_find(info.name, &variant);
		CHECK(format != NULL);
		if (strcmp(info.name, "hfsplus") == 0 || strcmp(info.name, "hfsx") == 0)
			continue;
		expect_status(info.name, format->probe(image.bytes,
		    format->probe_bytes, image.length, &variant, &error),
		    FSIMG_EFORMAT, &error);
	}
	expect_status("HFS Plus registry detection", fsimg_format_identify(NULL,
	    image.bytes, fsimg_format_probe_bytes(NULL), image.length,
	    &survey, &error), FSIMG_OK, &error);
	expect_true("exactly one HFS Plus claimant", survey.claims == 1 &&
	    survey.format == &fsimg_hfsplus_format &&
	    strcmp(survey.variant->info.name, "hfsplus") == 0);
	gen_free(&image);
}

/* A binary stream for the manifest comparator: regular files and link targets,
 * in the same depth-first order as the public listing. No host image writes. */
static int
native_content(const char *path)
{
	gen_image_t image = { 0 };
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char buffer[65536];
	size_t count, stored, piece;
	uint64_t offset;
	int fd = -1;
	size_t length = strlen(path);

	if (length >= 3 && strcmp(path + length - 3, ".gz") == 0)
		CHECK(native_gzip(path, &image));
	else {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		CHECK(fd != -1);
	}
	fixture_init(&f, &image, 0, 0);
	if (fd != -1) {
		fsimg_io_free(f.io);
		CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &f.io,
		    &error) == FSIMG_OK);
	}
	status = fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error);
	while (status == FSIMG_OK &&
	    (status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE &&
		    entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		for (offset = 0; offset < entry->size; offset += count) {
			piece = entry->size - offset > sizeof(buffer) ?
			    sizeof(buffer) : (size_t)(entry->size - offset);
			if (entry->kind == FSIMG_ENTRY_FILE)
				status = fsimg_reader_read_at(reader, offset, buffer,
				    piece, &count, &error);
			else
				status = fsimg_reader_content(reader, offset, buffer,
				    piece, &count, &stored, &error);
			if (status != FSIMG_OK)
				break;
			CHECK(count == piece);
			CHECK(fwrite(buffer, 1, count, stdout) == count);
		}
	}
	if (status != FSIMG_DONE)
		fprintf(stderr, "native content: %s: %s\n", error.path,
		    error.message);
	fsimg_reader_free(reader);
	fixture_close(&f);
	if (fd != -1)
		CHECK(close(fd) == 0);
	gen_free(&image);
	return (status == FSIMG_DONE ? 0 : 1);
}

/* Report the exact diagnostic as evidence that the intended guard ran. */
static void
expect_diagnostic(const char *name, const fsimg_error_t *error,
    const char *message)
{
	expect_true(name, strcmp(error->message, message) == 0);
	printf("%s: %s\n", name, error->message);
}

static void
test_root_fields(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	static const struct {
		const char *name;
		unsigned offset;
		uint16_t value;
		fsimg_status_t status;
		const char *message;
	} cases[] = {
		{ "root unknown flag refused at open", 2, 0x8000,
		    FSIMG_EUNSUPPORTED, "HFS Plus catalog record has an unknown flag" },
		{ "root expanded times refused at open", 2, 0x1000,
		    FSIMG_EUNSUPPORTED, "HFS Plus catalog record has expanded timestamps" },
		{ "root file mode refused at open", 42, 0100644,
		    FSIMG_ECORRUPT, "HFS Plus folder's mode is not a directory's" }
	};

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	expect_status("root zero mode accepted at open",
	    fixture_open(&f, -1, false, &error), FSIMG_OK, &error);
	fixture_close(&f);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		unsigned char *field = catalog_data(&image, 1, 1) + cases[i].offset;

		gen_store(field, cases[i].value, 2);
		fixture_init(&f, &image, 0, 0);
		expect_status(cases[i].name, fixture_open(&f, -1, false, &error),
		    cases[i].status, &error);
		expect_diagnostic(cases[i].name, &error, cases[i].message);
		fixture_close(&f);
		gen_store(field, 0, 2);
	}
	gen_free(&image);
}

static void
test_attribute_record_lengths(void)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .flags = 4 };
	gen_attr_value_t value = { .id = 32, .name = name_b, .units = 1 };
	gen_plan_t plan;
	gen_image_t image;
	hfsplus_attr_t decoded;
	static const uint32_t types[] = { 0x20, 0x30, 0x10 };
	static const char *names[] = { "short fork attribute refused",
	    "long extents attribute refused", "inline attribute overrun refused" };

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	plan.attributes = true;
	plan.values = &value;
	plan.value_count = 1;
	for (size_t i = 0; i < 3; i++) {
		value.type = types[i];
		gen_build(&plan, &image);
		expect_listing("attribute length baseline", &image, FSIMG_DONE, NULL);
		gen_tree_t *tree = &image.trees[GEN_ATTRIBUTES];
		gen_record_t *record = &tree->records[0];
		unsigned char *data = image.bytes + record->offset + record->key_length;
		unsigned char *node = image.bytes + gen_node_offset(&image,
		    GEN_ATTRIBUTES, tree->first_leaf);
		unsigned char *end = node + tree->node_size - 4;
		uint64_t saved = gen_load(end, 2);

		if (i < 2) {
			size_t length = record->length - record->key_length;
			size_t odd_length = i == 0 ? length - 1 : length + 1;

			expect_true(names[i], !hfsplus_attr_decode(data, odd_length, &decoded));
			/* Odd boundaries fail node framing. Even malformed lengths
			 * reach the attribute scanner and its own diagnostic. */
			gen_store(end, i == 0 ? saved - 1 : saved + 1, 2);
			expect_listing("odd attribute boundary refused", &image,
			    FSIMG_ECORRUPT, "HFS Plus node's offset table is out of order");
			gen_store(end, i == 0 ? saved - 2 : saved + 2, 2);
		} else
			gen_store(data + 12, 1, 4);
		listing_t listed = list_image(&image);
		expect_status(names[i], listed.status, FSIMG_ECORRUPT, &listed.error);
		expect_diagnostic(names[i], &listed.error,
		    "HFS Plus attribute record is malformed");
		gen_free(&image);
	}
}

static void
test_distinct_dates(void)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2,
		    .dates = { 2082844811, 2082844812, 2082844813, 2082844814, 2082844815 } },
		{ .name = name_b, .units = 1, .id = 33, .type = 1,
		    .dates = { 2082844821, 2082844822, 2082844823, 2082844824, 2082844825 } },
		{ .name = name_c, .units = 1, .id = 34, .type = 2,
		    .finder = "hlnkhfs+", .special = 7,
		    .dates = { 42, 52, 62, 72, 82 } },
		{ .name = private_data, .units = 21, .id = 40, .type = 1,
		    .dates = { 42 } },
		{ .name = inode7, .units = 6, .id = 41, .parent = 40, .type = 2,
		    .dates = { 2082844831, 2082844832, 2082844833, 2082844834, 2082844835 } }
	};
	gen_plan_t plan;
	listing_t listed;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	listed = list_plan("file folder and indirect node dates", &plan, FSIMG_DONE, NULL);
	expect_true("three visible dated entries", listed.count == 3);
	for (size_t i = 0; i < listed.count; i++) {
		const fsimg_metadata_t *m = &listed.entries[i].metadata;
		int64_t base = (int64_t)(i + 1) * 10;

		expect_true("createDate supplies created", m->created.seconds == base + 1);
		expect_true("contentModDate supplies modified", m->modified.seconds == base + 2);
		expect_true("attributeModDate supplies changed", m->changed.seconds == base + 3);
		expect_true("accessDate supplies accessed", m->accessed.seconds == base + 4);
		expect_true("only four date fields delivered", m->valid ==
		    (FSIMG_META_CREATED | FSIMG_META_MODIFIED | FSIMG_META_CHANGED |
		    FSIMG_META_ACCESSED));
	}
}

/* Scratch encodes integers little-endian, independently of the disk format. */
static void
store_payload(unsigned char *payload, size_t offset, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++)
		payload[offset + i] = (unsigned char)(value >> (8 * i));
}

/* Locate a generated catalog record within its leaf's offset table. */
static void
payload_location(const gen_image_t *image, unsigned char *payload,
    uint32_t id, uint16_t type)
{
	const gen_tree_t *tree = &image->trees[GEN_CATALOG];

	for (size_t i = 0; i < tree->record_count; i++) {
		const gen_record_t *r = &tree->records[i];

		if (gen_load(r->bytes + r->key_length, 2) != type ||
		    gen_load(r->bytes + r->key_length + 8, 4) != id)
			continue;
		uint32_t leaf = (uint32_t)((r->offset - tree->offset) / tree->node_size);
		uint64_t start = gen_node_offset(image, GEN_CATALOG, leaf);
		const unsigned char *node = image->bytes + start;
		size_t count = (size_t)gen_load(node + 10, 2);

		for (size_t j = 0; j < count; j++) {
			if (gen_load(node + tree->node_size - 2 * (j + 1), 2) != r->offset - start)
				continue;
			store_payload(payload, 4, leaf, 4);
			store_payload(payload, 8, j, 2);
			return;
		}
	}
	CHECK(false);
}

static void
test_prepared_entries(void)
{
	/* /a is delivered first; every case changes only queued /b. /c is
	 * an alternate file and /d an alternate folder for location checks. */
	static const uint16_t name_d[] = { 'd' };
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2, .data_size = 1 },
		{ .name = name_c, .units = 1, .id = 34, .type = 2, .data_size = 1 },
		{ .name = name_d, .units = 1, .id = 35, .type = 1 }
	};
	static const struct {
		const char *name;
		size_t offset;
		uint64_t value;
		size_t width;
	} invalid[] = {
		{ "prepared unknown kind refused", 14, 255, 1 },
		{ "prepared leaf at node count refused", 4, 0, 4 },
		{ "prepared leaf above node count refused", 4, UINT32_MAX, 4 },
		{ "prepared reserved id refused", 0, 15, 4 },
		{ "prepared mode outside permission bits refused", 33, 010000, 2 },
		{ "prepared unknown flags refused", 35, 0x80, 1 },
		{ "prepared compression refused", 16, 1, 1 },
		{ "prepared file valence refused", 56, 1, 4 },
		{ "prepared file special field refused", 52, 1, 4 },
		{ "prepared empty symlink refused", 17, 0, 8 },
		{ "prepared oversized symlink refused", 17, 1024, 8 },
		{ "prepared FIFO with bytes refused", 14, FSIMG_ENTRY_FIFO, 1 },
		{ "prepared extra data outside boolean refused", 15, 2, 1 }
	};
	size_t invalid_count = sizeof(invalid) / sizeof(invalid[0]);
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	for (size_t scenario = 0; scenario < invalid_count + 7; scenario++) {
		fixture_t f;
		fsimg_reader_t *reader;
		const fsimg_entry_t *entry;
		fsimg_store_t *store;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;
		unsigned char payload[64], byte;
		uint64_t cursor;
		size_t count;
		bool folder = scenario == invalid_count + 1 || scenario == invalid_count + 2;
		const char *name = "prepared entry baseline";
		const char *message = NULL;

		objects[1].type = folder ? 1 : 2;
		objects[1].data_size = folder ? 0 : 1;
		gen_build(&plan, &image);
		expect_listing("prepared entry source baseline", &image, FSIMG_DONE, NULL);
		reader = public_reader(&f, &image);
		CHECK(reader != NULL);
		CHECK(fsimg_reader_next(reader, &entry, &error) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/a") == 0);
		fsimg_test_walk_cursor(reader, &store, &cursor);
		CHECK(store != NULL);
		/* 8-byte frame, class, one-byte name and NUL precede payload. */
		CHECK(fsimg_store_read(store, cursor + 11, payload,
		    sizeof(payload), &error) == FSIMG_OK);
		if (scenario < invalid_count) {
			name = invalid[scenario].name;
			message = "invalid prepared HFS Plus entry payload";
			store_payload(payload, invalid[scenario].offset,
			    scenario == 1 ? image.trees[GEN_CATALOG].nodes : invalid[scenario].value,
			    invalid[scenario].width);
			if (scenario == 9 || scenario == 10)
				payload[14] = FSIMG_ENTRY_SYMLINK;
		} else switch (scenario - invalid_count) {
		case 0:
			break;
		case 1:
			name = "prepared folder location names another folder";
			payload_location(&image, payload, 35, 1);
			message = "HFS Plus folder record changed since it was listed";
			break;
		case 2:
			name = "prepared record past leaf end refused";
			store_payload(payload, 8, UINT16_MAX, 2);
			message = "HFS Plus record location lies outside its leaf";
			break;
		case 3:
			name = "prepared file location names another file";
			payload_location(&image, payload, 34, 2);
			message = "HFS Plus file record changed since it was listed";
			break;
		case 4:
			name = "prepared file size disagrees with record";
			store_payload(payload, 17, 2, 8);
			message = "HFS Plus file record changed since it was listed";
			break;
		case 5:
			name = "prepared file location names a folder";
			payload_location(&image, payload, 35, 1);
			message = "HFS Plus file record changed since it was listed";
			break;
		case 6:
			name = "prepared invalid UTF-8 name refused";
			/* The shared walker validates UTF-8 before driver replay. */
			byte = 0xff;
			CHECK(fsimg_store_write(store, cursor + 9, &byte, 1, &error) == FSIMG_OK);
			message = "invalid prepared directory record";
			break;
		}
		CHECK(fsimg_store_write(store, cursor + 11, payload,
		    sizeof(payload), &error) == FSIMG_OK);
		if (scenario == invalid_count + 6) {
			/* Exercise the driver guard while its reader is live; the
			 * public advance closes it on the walker's earlier refusal. */
			status = reader->ops->replay(reader, "\xff", payload,
			    sizeof(payload), &error);
			expect_status("driver replay rejects invalid UTF-8", status,
			    FSIMG_ECORRUPT, &error);
			expect_diagnostic("driver replay rejects invalid UTF-8", &error,
			    "invalid prepared HFS Plus file name");
		}
		status = fsimg_reader_next(reader, &entry, &error);
		if (folder || (scenario >= invalid_count + 3 && scenario <= invalid_count + 5)) {
			expect_status(name, status, FSIMG_OK, &error);
			if (status == FSIMG_OK) {
				if (folder)
					status = fsimg_reader_next(reader, &entry, &error);
				else
					status = fsimg_reader_read_at(reader, 0, &byte, 1, &count, &error);
			}
		}
		expect_status(name, status, message == NULL ? FSIMG_OK : FSIMG_ECORRUPT, &error);
		if (message != NULL)
			expect_diagnostic(name, &error, message);
		else {
			expect_true("valid prepared file retains its name", strcmp(entry->path, "/b") == 0);
			expect_status("valid prepared file can be read", fsimg_reader_read_at(
			    reader, 0, &byte, 1, &count, &error), FSIMG_OK, &error);
			expect_true("valid prepared file returns its byte", count == 1 && byte == 0);
		}
		fsimg_reader_free(reader);
		fixture_close(&f);
		gen_free(&image);
	}
}

static void
journal_info_folder(gen_record_t *records, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		gen_record_t *r = &records[i];

		if (gen_load(r->bytes + 2, 4) != 2 || gen_load(r->bytes + 6, 2) != 19)
			continue;
		gen_store(r->bytes + r->key_length, 1, 2);
		r->length = r->key_length + 88;
		return;
	}
	CHECK(false);
}

static void
test_journal_info_identity(void)
{
	gen_plan_t plan;
	gen_image_t image;
	listing_t listed;

	gen_plan_init(&plan);
	plan.journal = true;
	plan.journal_info_file = true;
	gen_build(&plan, &image);
	expect_listing("journal info identity baseline", &image, FSIMG_DONE, NULL);
	gen_tree_t *tree = &image.trees[GEN_CATALOG];
	gen_record_t *info = &tree->records[3];
	CHECK(gen_load(info->bytes + 6, 2) == 19);
	gen_store(image.bytes + info->offset + info->key_length + 104,
	    image.journal_info / image.block_size + 1, 4);
	listed = list_image(&image);
	expect_status("journal info start must match header", listed.status,
	    FSIMG_ECORRUPT, &listed.error);
	expect_diagnostic("journal info start must match header", &listed.error,
	    "HFS Plus .journal_info_block disagrees with the header");
	gen_free(&image);
	plan.mutate_catalog = journal_info_folder;
	gen_build(&plan, &image);
	listed = list_image(&image);
	expect_status("journal info must be regular file", listed.status,
	    FSIMG_ECORRUPT, &listed.error);
	expect_diagnostic("journal info must be regular file", &listed.error,
	    "HFS Plus journal info file is not a regular file");
	gen_free(&image);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--native-content") == 0)
		return (native_content(argv[2]));
	CHECK(argc == 1);
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	test_root_fields();
	test_attribute_record_lengths();
	test_distinct_dates();
	test_prepared_entries();
	test_journal_info_identity();
	test_probe_exclusivity();
	test_folder_counts_and_parents();
	test_catalog_exact_lengths();
	test_names_selection_owned();
	test_attribute_key_order();
	test_mixed_round_trip();
	test_catalog_listing();
	test_catalog_flags_kinds();
	test_symbolic_links();
	test_hard_links();
	test_attributes_compression();
	test_names_listing();
	test_identity_geometry();
	test_headers();
	test_tree_parameters();
	test_attributes_tree_accepts_binary_compare_pair();
	test_attributes_tree_preserves_code_unit_order();
	test_attributes_tree_allows_fixed_keys_only_when_empty();
	test_catalog_and_extents_keep_tree_parameters();
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
