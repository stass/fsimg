/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "codec.h"
#include "lzfse-vectors.h"
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
	VOLUME_BLOCK_SIZE = VOLUME_HEADER + 40,
	VOLUME_TOTAL_BLOCKS = VOLUME_HEADER + 44,
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
	unsigned	 reads;
	uint64_t	 watched_start[8];
	size_t		 watched_length[8];
	uint64_t	 watched_bytes[8];
	unsigned	 watched_calls[8];
	bool		 cancel_on_payload;
	bool		 cancelled;
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
static void test_compression_word_table_corruption(void);
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
	f->reads++;
	for (size_t i = 0; i < 8; i++) {
		uint64_t start = offset > f->watched_start[i] ? offset :
		    f->watched_start[i];
		uint64_t end = offset + length;

		if (end > f->watched_start[i] + f->watched_length[i])
			end = f->watched_start[i] + f->watched_length[i];
		if (start < end) {
			f->watched_bytes[i] += end - start;
			f->watched_calls[i]++;
			if (i == 0 && f->cancel_on_payload)
				f->cancelled = true;
		}
	}
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
	f->reader.slice_bytes = f->length;
	f->reader.state = FSIMG_READER_OPENING;
	return (fsimg_hfsplus_reader_ops.open(&f->reader,
	    forced < 0 ? NULL : &fsimg_hfsplus_format.variants[forced], error));
}

static void
fixture_close(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_hfsplus_reader_ops.close(&f->reader);
	/* Closing twice also covers an already cleaned partial open. */
	fsimg_hfsplus_reader_ops.close(&f->reader);
	fsimg_io_free(f->reader.narrowed);
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
	expect_status("forced hfsx wrapper", image_open(&image, 1, false),
	    FSIMG_EFORMAT, NULL);
	gen_store(image.bytes + VOLUME_HEADER + 124, 0, 2);
	expect_status("classic HFS probe", image_probe(&image), FSIMG_EFORMAT,
	    NULL);
	/* Reader identity contract: a lost signature is corruption. */
	expect_status("classic HFS open", image_open(&image, -1, false),
	    FSIMG_ECORRUPT, NULL);
	gen_free(&image);

	/* Header field limits are tested without allocating a volume that size.
	 */
	header = (hfsplus_header_t){ .signature = 0x482b, .version = 4,
	    .total_blocks = 2 };
	for (uint32_t b = 512;; b *= 2) {
		header.block_size = b;
		expect_status("header legal power of two",
		    hfsplus_header_check(&header, &error), FSIMG_OK, &error);
		if (b == UINT32_C(0x80000000))
			break;
	}
	const uint32_t bad_sizes[] = { 0, 511, 513 };
	for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++) {
		header.block_size = bad_sizes[i];
		expect_status("header invalid block size",
		    hfsplus_header_check(&header, &error), FSIMG_ECORRUPT,
		    &error);
	}
	header.block_size = 512;
	header.total_blocks = 0;
	expect_status("zero blocks", hfsplus_header_check(&header,
	    &error), FSIMG_ECORRUPT, &error);
	header.total_blocks = 9;
	expect_status("nonzero allocation block count",
	    hfsplus_header_check(&header, &error), FSIMG_OK, &error);
	/* Both operands are uint32_t: their product cannot overflow uint64_t. */
	header.total_blocks = UINT32_MAX;
	header.block_size = UINT32_C(0x80000000);
	expect_status("maximum legal block size and count",
	    hfsplus_header_check(&header, &error), FSIMG_OK, &error);
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
	return (hfsplus_fork_index(&f->reader, 32, type, &fork, index,
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
		fsimg_owner_runs_t *claims;
		uint64_t records = 0;

		CHECK(fsimg_owner_runs_new(&f.reader.session,
		    "HFS Plus allocation block", 0, plan.total_blocks,
		    &claims, &error) == FSIMG_OK);
		expect_status("fragmented fork record count", hfsplus_fork_claim(
		    &f.reader, claims, 32, 0, &fork, &records, &error),
		    FSIMG_OK, &error);
		expect_true("true overflow record count", records ==
		    (counts[n] > 8 ? (counts[n] - 1) / 8 : 0));
		fsimg_owner_runs_free(claims);
		fork.logical_size = (uint64_t)fork.total_blocks *
		    plan.block_size + 1;
		expect_status("logical size beyond allocation",
		    hfsplus_fork_index(&f.reader,
		    32, 0, &fork, &index, &error), FSIMG_ECORRUPT,
		    &error);
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
	    &fork, &index, &error), FSIMG_ECORRUPT, &error);
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
	    32, 0, &fork, &index, &error), FSIMG_ECORRUPT, &error);
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
		    fault == 8 ? 3 : 32, 0, &fork, &index, &error),
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
		gen_image_t wrapped = { 0 };
		wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
		    &wrapped.length);
		expect_status("wrapped initialized journal",
		    image_open(&wrapped, -1, false), FSIMG_EUNSUPPORTED, NULL);
		free(wrapped.bytes);
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
				fixture_close(&f);
				gen_image_t wrapped = { 0 };
				wrapped.bytes = gen_wrap(&image, 4096, 4,
				    1, 1, 1024, &wrapped.length);
				expect_status("wrapped uninitialized journal",
				    image_open(&wrapped, -1, false), FSIMG_OK, NULL);
				free(wrapped.bytes);
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

static void
test_wrapped_open_preserves_injected_failures(void)
{
	gen_plan_t plan;
	gen_image_t image, wrapped = { 0 };
	fixture_t f;
	fsimg_error_t error = { 0 };
	bool completed = false;
	const uint64_t embedded = 6144;

	gen_plan_init(&plan);
	plan.attributes = true;
	plan.journal = true;
	plan.journal_flags = 5;
	gen_build(&plan, &image);
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &wrapped.length);
	const uint64_t offsets[] = { embedded + VOLUME_HEADER,
		embedded + image.journal_info,
		embedded + gen_node_offset(&image, GEN_CATALOG, 1),
		embedded + gen_node_offset(&image, GEN_EXTENTS, 0) };
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		fixture_init(&f, &wrapped, 0, 0);
		f.fail_offset = offsets[i];
		expect_status("wrapped read failure keeps EIO",
		    fixture_open(&f, -1, false, &error), FSIMG_EIO, &error);
		fixture_close(&f);
	}
	for (uint64_t attempt = 1; attempt < 200; attempt++) {
		fixture_init(&f, &wrapped, 0, 0);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		fsimg_status_t status = fixture_open(&f, -1, false, &error);
		if (status == FSIMG_OK)
			completed = true;
		else
			expect_status("wrapped allocation failure keeps ENOMEM",
			    status, FSIMG_ENOMEM, &error);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_close(&f);
		if (completed)
			break;
	}
	expect_true("all wrapped open allocations injected", completed);
	free(wrapped.bytes);
	gen_free(&image);
}

static void
test_narrow_refuses_after_open_and_during_iteration(void)
{
	static const uint16_t name[] = { 'a' };
	gen_child_t child = { .name = name, .units = 1, .id = 32 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };

	gen_plan_init(&plan);
	plan.children = &child;
	plan.child_count = 1;
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error) ==
	    FSIMG_OK);
	const fsimg_reader_state_t states[] = { FSIMG_READER_NEW,
		FSIMG_READER_ITERATING, FSIMG_READER_DONE };
	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		fsimg_io_t *input = reader->input;
		uint64_t length = reader->slice_bytes;

		expect_true("public traversal reaches expected state",
		    reader->state == states[i]);
		expect_status("narrow outside open refuses state",
		    fsimg_reader_narrow(reader, 512, 1024, &error),
		    FSIMG_ESTATE, &error);
		expect_true("refused narrow preserves input and length",
		    reader->input == input && reader->slice_bytes == length &&
		    reader->narrowed == NULL);
		if (i < 2)
			expect_status("advance narrow state fixture",
			    fsimg_reader_next(reader, &entry, &error),
			    i == 0 ? FSIMG_OK : FSIMG_DONE, &error);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_narrow_clips_range_and_refuses_second_call(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	unsigned char bytes[16];
	size_t count;
	uint64_t length;

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	expect_status("zeroed reader refuses narrow",
	    fsimg_reader_narrow(&f.reader, 512, 1024, &error),
	    FSIMG_ESTATE, &error);
	expect_true("zeroed reader narrow preserves input and length",
	    f.reader.input == f.io && f.reader.slice_bytes == 0 &&
	    f.reader.narrowed == NULL);
	f.reader.state = FSIMG_READER_OPENING;
	f.reader.slice_bytes = image.length;
	expect_status("narrow start at end is corruption",
	    fsimg_reader_narrow(&f.reader, image.length, 1, &error),
	    FSIMG_ECORRUPT, &error);
	expect_true("out of range narrow preserves input",
	    f.reader.input == f.io && f.reader.slice_bytes == image.length &&
	    f.reader.narrowed == NULL);
	expect_true("out of range narrow names truncation",
	    strcmp(error.message,
	    "the range to read lies past the input: image is truncated") == 0);
	expect_status("narrow clips range at input end",
	    fsimg_reader_narrow(&f.reader, 512, image.length, &error),
	    FSIMG_OK, &error);
	expect_true("clipped narrow owns replacement input",
	    f.reader.input == f.reader.narrowed && f.reader.input != f.io &&
	    f.reader.slice_bytes == image.length - 512);
	CHECK(fsimg_io_get_size(f.reader.input, &length, NULL) == FSIMG_OK);
	expect_true("clipped slice has clipped size",
	    length == image.length - 512);
	CHECK(fsimg_io_read_at(f.reader.input, 512, bytes, sizeof(bytes),
	    &count, NULL) == FSIMG_OK);
	expect_true("narrow translates offsets to original input",
	    count == sizeof(bytes) &&
	    bcmp(bytes, image.bytes + VOLUME_HEADER, sizeof(bytes)) == 0);
	fsimg_io_t *input = f.reader.input;
	expect_status("second narrow during open refuses state",
	    fsimg_reader_narrow(&f.reader, 0, 1, &error), FSIMG_ESTATE, &error);
	expect_true("second narrow preserves input and length",
	    f.reader.input == input && f.reader.narrowed == input &&
	    f.reader.slice_bytes == length);
	fixture_close(&f);

	fixture_t bounded;
	fixture_init(&bounded, &image, 0, 0);
	bounded.reader.state = FSIMG_READER_OPENING;
	bounded.reader.slice_bytes = image.length;
	CHECK(image.length > 1536);
	/* Distinct from its neighbours, so a wrong offset cannot read it. */
	image.bytes[1535] = 0x5a;
	expect_status("narrow ends before parent end",
	    fsimg_reader_narrow(&bounded.reader, 512, 1024, &error),
	    FSIMG_OK, &error);
	expect_status("read last byte inside narrowed range",
	    fsimg_io_read_exact(bounded.reader.input, 1023, bytes, 1,
	    &count, &error), FSIMG_OK, &error);
	expect_true("narrowed last byte matches parent",
	    count == 1 && bytes[0] == 0x5a);
	unsigned reads = bounded.reads;
	expect_status("exact read crossing narrowed end refuses range",
	    fsimg_io_read_exact(bounded.reader.input, 1023, bytes, 2,
	    &count, &error), FSIMG_ERANGE, &error);
	expect_true("crossing narrowed end does not call backing reader",
	    bounded.reads == reads);
	fixture_close(&bounded);
	gen_free(&image);
}

static void
test_failed_wrapped_public_open_frees_slice(void)
{
	gen_plan_t plan;
	gen_image_t image, wrapped = { 0 };
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error = { 0 };
	fsimg_stats_t before, after;

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &wrapped.length);
	gen_store(wrapped.bytes + 6144 + VOLUME_HEADER + 2, 5, 2);
	fixture_init(&f, &wrapped, 0, 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	expect_status("broken embedded header fails public open",
	    fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error),
	    FSIMG_ECORRUPT, &error);
	expect_true("failed public open publishes no reader", reader == NULL);
	expect_true("failed public open reached embedded header",
	    strstr(error.message, "not HFS Plus version 4") != NULL);
	CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
	expect_true("failed public open releases slice and driver allocations",
	    after.memory_bytes == before.memory_bytes &&
	    after.descriptors == before.descriptors &&
	    after.scratch_bytes == before.scratch_bytes);
	fixture_close(&f);
	free(wrapped.bytes);
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
		bool matches, classic;

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
		classic = gen_load(prefix + 1024, 2) == 0x4244 &&
		    gen_load(prefix + 1024 + 124, 2) != 0x482b;
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
		expected = strcmp(entry->d_name,
		    "h-hfsplus-16777216-e11-mkfs-w-J-mounted") == 0 ||
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
catalog_listing_plan(gen_plan_t *plan, gen_object_t *objects)
{
	static const gen_object_t base[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2 },
		{ .parent = 32, .name = name_c, .units = 1, .id = 34,
		    .type = 2 }
	};

	bcopy(base, objects, sizeof(base));
	gen_plan_init(plan);
	plan->objects = objects;
	plan->object_count = 3;
}

static void
test_catalog_listing(void)
{
	gen_object_t objects[3];
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
	static const struct {
		const char *name;
		void (*mutate)(gen_record_t *, size_t);
		fsimg_status_t status;
		const char *message;
	} orders[] = {
		{ "ordered children", NULL, FSIMG_DONE, NULL },
		{ "equal child keys", mutate_equal_keys, FSIMG_ECORRUPT,
		    "HFS Plus catalog keys are out of order" },
		{ "descending child keys", mutate_descending_keys, FSIMG_ECORRUPT,
		    "HFS Plus catalog keys are out of order" }
	};
	gen_plan_t plan;
	listing_t listed;

	catalog_listing_plan(&plan, objects);
	listed = list_plan("nested catalog", &plan, FSIMG_DONE, NULL);
	expect_true("nested traversal order", listed.count == 3 &&
	    strcmp(listed.entries[0].path, "/a") == 0 &&
	    strcmp(listed.entries[1].path, "/a/c") == 0 &&
	    strcmp(listed.entries[2].path, "/b") == 0);
	for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		catalog_listing_plan(&plan, objects);
		plan.mutate_catalog = mutations[i].mutate;
		list_plan(mutations[i].name, &plan, FSIMG_ECORRUPT,
		    mutations[i].message);
	}
	catalog_listing_plan(&plan, objects);
	objects[0].omit_thread = true;
	list_plan("missing folder thread", &plan, FSIMG_ECORRUPT,
	    "HFS Plus thread record is missing");
	/* A second primary with the same folder ID cannot agree with the
	 * existing thread's parent and name. */
	for (unsigned descendant = 0; descendant < 2; descendant++) {
		catalog_listing_plan(&plan, objects);
		objects[1].type = 1;
		objects[1].id = 32;
		objects[1].omit_thread = true;
		objects[1].parent = descendant ? 32 : 0;
		list_plan(descendant ? "folder cycle through descendant" :
		    "reused folder ID", &plan, FSIMG_ECORRUPT,
		    "HFS Plus folder thread names another parent or name");
	}
	for (uint32_t id = 9; id <= 15; id++) {
		catalog_listing_plan(&plan, objects);
		objects[1].id = id;
		list_plan("reserved file ID", &plan, FSIMG_ECORRUPT,
		    "HFS Plus file has a reserved node ID");
		catalog_listing_plan(&plan, objects);
		objects[0].id = id;
		list_plan("reserved folder ID", &plan, FSIMG_ECORRUPT,
		    "HFS Plus folder has a reserved node ID");
	}

	/* Three root files put the second key on a new leaf when packed two
	 * records at a time; the first key remains a valid search anchor. */
	for (unsigned boundary = 0; boundary < 2; boundary++) {
		for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
			catalog_listing_plan(&plan, objects);
			objects[0].type = 2;
			objects[2].parent = 0;
			plan.records_per_node = boundary ? 2 : 0;
			plan.mutate_catalog = orders[i].mutate;
			list_plan(orders[i].name, &plan, orders[i].status, orders[i].message);
		}
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
		expect_true(name, found);
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

static const unsigned char hard_link_payload[] = "indirect payload";

static void
hard_links_plan(gen_plan_t *plan, gen_object_t *objects)
{
	static const gen_object_t base[] = {
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
		    .data = hard_link_payload, .data_size = sizeof(hard_link_payload) - 1,
		    .reserved = UINT32_MAX },
		{ .name = name_a, .units = 1, .id = 36, .parent = 33,
		    .type = 2, .special = 7, .finder = "hlnkhfs+",
		    .dates = { 42 } }
	};

	bcopy(base, objects, sizeof(base));
	gen_plan_init(plan);
	plan->objects = objects;
	plan->object_count = 5;
}

static void
test_hard_links(void)
{
	static const uint16_t attr_name[] = { 'x' };
	static const uint16_t lower_inode[] = { 'i', 'n', 'o', 'd', 'e', '7' };
	gen_attr_value_t attr = { .id = 41, .name = attr_name, .units = 1,
	    .type = 0x10 };
	gen_object_t objects[6];
	gen_plan_t plan;
	listing_t listed;

	hard_links_plan(&plan, objects);
	listed = list_plan("two hard-link names in two directories", &plan,
	    FSIMG_DONE, NULL);
	expect_true("private node hidden and metadata taken from node",
	    listed.count == 3 && listed.entries[0].metadata.uid == 123 &&
	    listed.entries[2].metadata.mode == 0641 &&
	    listed.entries[0].size == sizeof(hard_link_payload) - 1 &&
	    strcmp(listed.entries[2].path, "/b/c") == 0);
	check_content("first stub content", &plan, "/a", hard_link_payload,
	    sizeof(hard_link_payload) - 1, FSIMG_OK, NULL);
	check_content("second stub content", &plan, "/b/c", hard_link_payload,
	    sizeof(hard_link_payload) - 1, FSIMG_OK, NULL);
	hard_links_plan(&plan, objects);
	plan.object_count = 6;
	listed = list_plan("three names across directories", &plan, FSIMG_DONE, NULL);
	expect_true("three names use node metadata", listed.count == 4 &&
	    strcmp(listed.entries[2].path, "/b/a") == 0 &&
	    strcmp(listed.entries[3].path, "/b/c") == 0 &&
	    listed.entries[2].metadata.uid == 123 &&
	    listed.entries[3].metadata.mode == 0641 &&
	    listed.entries[0].metadata.created.seconds == -INT64_C(2082844800));
	check_content("third stub content", &plan, "/b/a", hard_link_payload,
	    sizeof(hard_link_payload) - 1, FSIMG_OK, NULL);
	hard_links_plan(&plan, objects);
	objects[0].mode = 0100444;
	objects[0].flags = 0x20;
	list_plan("current stub mode and flag", &plan, FSIMG_DONE, NULL);
	hard_links_plan(&plan, objects);
	list_plan("zero-mode stub without chain flag", &plan, FSIMG_DONE, NULL);
	hard_links_plan(&plan, objects);
	plan.attributes = true;
	plan.values = &attr;
	plan.value_count = 1;
	objects[4].flags = 4;
	listed = list_plan("hard-link mark from indirect node", &plan, FSIMG_DONE, NULL);
	expect_true("both stubs carry node mark", listed.count == 3 &&
	    (listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) &&
	    (listed.entries[2].metadata.valid & FSIMG_META_EXTRA_DATA));
	hard_links_plan(&plan, objects);
	plan.attributes = true;
	plan.values = &attr;
	plan.value_count = 1;
	objects[0].flags = 4;
	attr.id = 32;
	listed = list_plan("stub attributes do not mark effective node", &plan,
	    FSIMG_DONE, NULL);
	expect_true("stub mark absent", !(listed.entries[0].metadata.valid &
	    FSIMG_META_EXTRA_DATA));
	hard_links_plan(&plan, objects);
	objects[0].dates[0] = 0;
	list_plan("root-date stub with different private date", &plan,
	    FSIMG_DONE, NULL);
	hard_links_plan(&plan, objects);
	objects[0].dates[0] = 43;
	listed = list_plan("Finder file codes with another date", &plan,
	    FSIMG_DONE, NULL);
	expect_true("ordinary file uses own size", listed.entries[0].size == 0);
	hard_links_plan(&plan, objects);
	objects[0].special = 0;
	list_plan("zero hard-link reference", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a zero reference");
	hard_links_plan(&plan, objects);
	objects[0].data_size = 1;
	list_plan("stub nonempty data", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a nonempty fork");
	hard_links_plan(&plan, objects);
	objects[0].resource_size = 1;
	list_plan("stub nonempty resource", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub has a nonempty fork");
	hard_links_plan(&plan, objects);
	objects[4].name = name_c;
	objects[4].units = 1;
	list_plan("missing indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub's indirect node is missing");
	hard_links_plan(&plan, objects);
	objects[4].name = lower_inode;
	list_plan("case variant indirect node is missing", &plan, FSIMG_ECORRUPT,
	    "HFS Plus hard-link stub's indirect node is missing");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0010644;
	list_plan("FIFO indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0040755;
	objects[4].type = 1;
	list_plan("folder indirect node", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	bcopy("hlnkhfs+", objects[4].finder, 8);
	objects[4].special = 7;
	objects[4].data = NULL;
	objects[4].data_size = 0;
	objects[4].resource = NULL;
	objects[4].resource_size = 0;
	list_plan("indirect node is itself stub", &plan, FSIMG_ECORRUPT,
	    "HFS Plus indirect node is a stub or of another kind");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0120777;
	listed = list_plan("hard-linked symbolic link", &plan, FSIMG_DONE, NULL);
	expect_true("indirect node decides link kind",
	    listed.entries[0].kind == FSIMG_ENTRY_SYMLINK);
	check_content("hard-linked target", &plan, "/a", hard_link_payload,
	    sizeof(hard_link_payload) - 1, FSIMG_OK, NULL);
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	objects[3].type = 2;
	list_plan("private data lookup requires folder", &plan, FSIMG_ECORRUPT,
	    "HFS Plus private data folder is not a folder");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	objects[3].type = 2;
	bzero(objects[0].finder, 32);
	bzero(objects[2].finder, 32);
	list_plan("root listing validates private folder", &plan, FSIMG_ECORRUPT,
	    "HFS Plus private folder is not a folder");
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	bcopy("fdrpMACS", objects[0].finder, 8);
	bzero(objects[2].finder, 32);
	objects[0].flags = 0x20;
	listed = list_plan("directory hard link", &plan, FSIMG_EUNSUPPORTED,
	    "HFS Plus directory hard link is not supported");
	expect_true("directory link names path and exposes no siblings",
	    strcmp(listed.error.path, "/a") == 0 && listed.count == 0);
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	bcopy("fdrpMACS", objects[0].finder, 8);
	bzero(objects[2].finder, 32);
	list_plan("Finder alias without chain flag", &plan, FSIMG_DONE, NULL);
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	bcopy("fdrpMACS", objects[0].finder, 8);
	bzero(objects[2].finder, 32);
	objects[0].flags = 0x20;
	objects[0].dates[0] = 43;
	list_plan("Finder alias with another date", &plan, FSIMG_DONE, NULL);
	hard_links_plan(&plan, objects);
	objects[4].mode = 0100644;
	bzero(objects[2].finder, 32);
	objects[0].flags = 0x20;
	objects[0].dates[0] = 0;
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
	listed = list_plan("compressed inline listing", &plan, FSIMG_DONE, NULL);
	expect_true("compressed declared size", listed.count == 2 &&
	    listed.entries[0].size == 1);
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

/* All compression integers except the resource-file framing are LE. */
static void
compression_le(unsigned char *out, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++) {
		out[i] = (unsigned char)value;
		value >>= 8;
	}
}

static void
gen_decmpfs_header(unsigned char *out, uint32_t type, uint64_t size)
{
	bcopy("fpmc", out, 4);
	compression_le(out + 4, type, 4);
	compression_le(out + 8, size, 8);
}

static unsigned char *
gen_decmpfs_resource(const unsigned char *bytes, size_t length,
    uint32_t stored_mask, size_t *size)
{
	const size_t data = 256, map = 50, chunk = 65536;
	size_t count = (length + chunk - 1) / chunk;
	size_t cursor = data + 8 + 8 * count;
	size_t capacity = cursor + count * (compressBound(chunk) + 1) + map;
	unsigned char *out = calloc(1, capacity);

	CHECK(out != NULL && count <= 64);
	compression_le(out + data + 4, count, 4);
	for (size_t i = 0; i < count; i++) {
		size_t piece = length - i * chunk;
		uLongf stored = compressBound(chunk);

		if (piece > chunk)
			piece = chunk;
		if (i < 32 && (stored_mask & (UINT32_C(1) << i)) != 0) {
			out[cursor] = 0xff;
			bcopy(bytes + i * chunk, out + cursor + 1, piece);
			stored = piece + 1;
		} else
			CHECK(compress2(out + cursor, &stored, bytes + i * chunk,
			    piece, Z_BEST_COMPRESSION) == Z_OK);
		compression_le(out + data + 8 + 8 * i,
		    cursor - data - 4, 4);
		compression_le(out + data + 12 + 8 * i, stored, 4);
		cursor += stored;
	}
	gen_store(out, data, 4);
	gen_store(out + 4, cursor, 4);
	gen_store(out + 8, cursor - data, 4);
	gen_store(out + 12, map, 4);
	gen_store(out + data, cursor - data - 4, 4);
	*size = cursor + map;
	return (out);
}

/* A compressed object has no data fork. Its sibling makes directory-wide
 * preparation failures distinguishable from errors deferred to first read.
 */
static const uint16_t compression_name[] = { 'c', 'o', 'm', '.', 'a', 'p',
    'p', 'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };

typedef struct compression_case {
	gen_plan_t plan;
	gen_object_t objects[2];
	gen_attr_value_t attributes[2];
	unsigned char header[1024];
	unsigned char *resource;
	unsigned char *plain;
	size_t size;
} compression_case_t;

static uint32_t
compression_word(const unsigned char *in)
{
	return ((uint32_t)in[0] | (uint32_t)in[1] << 8 |
	    (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24);
}

static bool
decoder_supported(void)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return (true);
#else
	return (false);
#endif
}

static void
compression_case_init(compression_case_t *c, uint32_t type, size_t size,
    uint32_t stored_mask)
{
	uLongf encoded = sizeof(c->header) - 16;

	bzero(c, sizeof(*c));
	c->size = size;
	c->plain = malloc(size + 1);
	CHECK(c->plain != NULL);
	/* Nonconstant, compressible bytes distinguish offsets and files. */
	for (size_t i = 0; i < size; i++)
		c->plain[i] = (unsigned char)('!' + (i * 7 + i / 65536) % 61);
	gen_plan_init(&c->plan);
	c->objects[0] = (gen_object_t){ .name = name_a, .units = 1,
	    .id = 32, .type = 2, .flags = 4, .owner_flags = 0x20 };
	c->objects[1] = (gen_object_t){ .name = name_b, .units = 1,
	    .id = 33, .type = 2 };
	c->attributes[0] = (gen_attr_value_t){ .id = 32, .type = 0x10,
	    .name = compression_name, .units = 17, .value = c->header, .size = 16 };
	c->plan.objects = c->objects;
	c->plan.object_count = 2;
	c->plan.values = c->attributes;
	c->plan.value_count = 1;
	c->plan.attributes = true;
	gen_decmpfs_header(c->header, type, size);
	if (size == 0)
		return;
	if (type == 4) {
		c->resource = gen_decmpfs_resource(c->plain, size, stored_mask,
		    &c->objects[0].resource_size);
		c->objects[0].resource = c->resource;
	} else if (type == 1 || stored_mask != 0) {
		size_t marker = type == 1 ? 0 : 1;

		CHECK(size + 16 + marker <= sizeof(c->header));
		if (marker != 0)
			c->header[16] = 0xff;
		bcopy(c->plain, c->header + 16 + marker, size);
		c->attributes[0].size += size + marker;
	} else if (type == 3) {
		CHECK(compress2(c->header + 16, &encoded, c->plain, size,
		    Z_BEST_COMPRESSION) == Z_OK);
		c->attributes[0].size += encoded;
	}
}

static void
compression_case_free(compression_case_t *c)
{
	free(c->plain);
	free(c->resource);
}

/* Listing admits the compressed entry before its payload is exercised. */
static fsimg_reader_t *
compression_open(const char *name, fixture_t *f, const gen_image_t *image)
{
	fsimg_reader_t *reader = public_reader(f, image);
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	if (reader == NULL)
		return (NULL);
	status = fsimg_reader_next(reader, &entry, &error);
	expect_status(name, status, FSIMG_OK, &error);
	if (status != FSIMG_OK) {
		fsimg_reader_free(reader);
		return (NULL);
	}
	expect_true(name, strcmp(entry->path, "/a") == 0);
	return (reader);
}

static bool
compression_read_pieces(const char *name, fsimg_reader_t *reader,
    const unsigned char *plain, size_t size, size_t step)
{
	unsigned char *bytes = malloc(size + 1);
	fsimg_error_t error = { 0 };
	fsimg_status_t status = FSIMG_OK;
	size_t count, piece;
	bool matches = true;

	CHECK(bytes != NULL);
	for (size_t offset = 0; offset < size; offset += piece) {
		piece = size - offset;
		if (piece > step)
			piece = step;
		if (step == 1 && size > 64 && offset % 65536 == 32 &&
		    size - offset > 64) {
			offset += 65472;
			if (offset >= size)
				break;
		}
		status = fsimg_reader_read_at(reader, offset, bytes, piece, &count, &error);
		if (status != FSIMG_OK)
			break;
		matches &= count == piece && bcmp(bytes, plain + offset, piece) == 0;
	}
	if (status != FSIMG_OK)
		expect_status(name, status, FSIMG_OK, &error);
	else
		expect_true(name, matches);
	if (size == 0) {
		bytes[0] = 0xa5;
		status = fsimg_reader_read_at(reader, 0, bytes, 1, &count, &error);
		expect_status(name, status, FSIMG_OK, &error);
		expect_true(name, count == 0 && bytes[0] == 0xa5);
	}
	free(bytes);
	return (status == FSIMG_OK);
}

static void
compression_content(const char *name, compression_case_t *c)
{
	gen_image_t image;
	fixture_t f;
	listing_t listed;
	fsimg_reader_t *reader;
	unsigned char bytes[97];
	size_t count;
	uint32_t random = 0x12345678;
	fsimg_error_t error = { 0 };

	gen_build(&c->plan, &image);
	listed = list_image(&image);
	expect_status(name, listed.status, FSIMG_DONE, &listed.error);
	if (listed.status == FSIMG_DONE) {
		/* Empty files leave nonempty resource forks and attribute payloads unconsumed. */
		bool expect_extra = c->size == 0 &&
		    (c->objects[0].resource_size != 0 || c->attributes[0].size > 16);

		expect_true(name, listed.count == 2 && listed.entries[0].size == c->size);
		expect_true(name,
		    !!(listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) == expect_extra);
	}
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		unsigned reads = f.reads;

		if (!compression_read_pieces(name, reader, c->plain, c->size, c->size + 1))
			goto close_reader;
		compression_read_pieces(name, reader, c->plain, c->size, 4096);
		compression_read_pieces(name, reader, c->plain, c->size, 1);
		if (c->size == 0)
			expect_true("test_compression_empty_reads_no_image_bytes",
			    f.reads == reads);
		for (unsigned i = 0; c->size != 0 && i < 64; i++) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			size_t offset = random % c->size;
			size_t piece = c->size - offset;
			if (piece > sizeof(bytes))
				piece = sizeof(bytes);
			fsimg_status_t status = fsimg_reader_read_at(reader, offset,
			    bytes, piece, &count, &error);
			expect_status(name, status, FSIMG_OK, &error);
			if (status != FSIMG_OK)
				break;
			expect_true(name, count == piece && bcmp(bytes, c->plain + offset, piece) == 0);
		}
close_reader:
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
}

/* A vector supplies encoded bytes and an independent content generator.
 * A two-chunk file starts with a full window, then the selected vector.
 */
static void
compression_vector_init(compression_case_t *c, unsigned type,
    const lzfse_vector_t *vector, bool two_chunks, bool stored)
{
	size_t size = vector->plain_size + (two_chunks ? 65536 : 0);
	bool lzvn = type == 7 || type == 8;
	bool fork = type == 8 || type == 12;
	unsigned char *raw = NULL;
	gen_compressed_chunk_t chunks[2];
	const unsigned char *encoded = vector->encoded;
	size_t length = vector->encoded_size;

	compression_case_init(c, type, size, 0);
	for (size_t i = 0; i < size; i++)
		c->plain[i] = lzfse_plain_byte(vector, i < 65536 ? i : i - 65536);
	if (lzvn && !stored) {
		CHECK(vector->encoded == lzfse_lzvn);
		encoded += 12;
		length -= 16;
	}
	if (stored || (two_chunks && lzvn)) {
		raw = malloc((two_chunks ? 65536 : size) + 1);
		CHECK(raw != NULL);
		raw[0] = 6;
		bcopy(c->plain, raw + 1, two_chunks ? 65536 : size);
		if (stored) {
			CHECK(lzvn && !two_chunks);
			encoded = raw;
			length = size + 1;
		}
	}
	if (fork) {
		chunks[0] = (gen_compressed_chunk_t){ encoded, length };
		if (two_chunks) {
			chunks[1] = chunks[0];
			chunks[0] = lzvn ? (gen_compressed_chunk_t){ raw, 65537 } :
			    (gen_compressed_chunk_t){ lzfse_chunk, sizeof(lzfse_chunk) };
		}
		c->resource = gen_compressed_resource(chunks, two_chunks ? 2 :
		    size == 0 ? 0 : 1, &c->objects[0].resource_size);
		c->objects[0].resource = c->resource;
	} else {
		CHECK(length + 16 <= sizeof(c->header));
		bcopy(encoded, c->header + 16, length);
		c->attributes[0].size = 16 + length;
	}
	free(raw);
}

static void
test_compression_codec_content(void)
{
	static const unsigned types[] = { 7, 8, 11, 12 };
	char name[128];
	compression_case_t c;

	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		unsigned type = types[t];
		bool lzvn = type == 7 || type == 8;

		if (!decoder_supported()) {
			compression_vector_init(&c, type, &lzfse_vectors[1], false, false);
			snprintf(name, sizeof(name), "test_compression_type%u_unsupported_host", type);
			check_content(name, &c.plan, "/a", c.plain, c.size,
			    FSIMG_EUNSUPPORTED, NULL);
			compression_case_free(&c);
			continue;
		}
		for (size_t i = 0; i < sizeof(lzfse_vectors) / sizeof(lzfse_vectors[0]); i++) {
			const lzfse_vector_t *v = &lzfse_vectors[i];

			if (lzvn && v->encoded != lzfse_lzvn && v->plain_size > 1)
				continue;
			compression_vector_init(&c, type, v, false,
			    lzvn && v->plain_size <= 1);
			snprintf(name, sizeof(name), "test_compression_type%u_%s", type, v->name);
			compression_content(name, &c);
			compression_case_free(&c);
		}
		if (type == 8 || type == 12) {
			compression_vector_init(&c, type, &lzfse_vectors[1], true, false);
			snprintf(name, sizeof(name), "test_compression_type%u_two_chunks", type);
			compression_content(name, &c);
			compression_case_free(&c);
		}
	}
}

static void
test_compression_content(void)
{
	static const size_t sizes[] = { 0, 1, 65535, 65536, 65537, 131072, 200000 };
	compression_case_t c;
	char name[96];

	compression_case_init(&c, 1, 127, 0);
	compression_content("test_compression_type1", &c);
	compression_case_free(&c);
	compression_case_init(&c, 1, 1, 0);
	compression_content("test_compression_type1_one_byte", &c);
	compression_case_free(&c);
	compression_case_init(&c, 1, 0, 0);
	compression_content("test_compression_type1_empty", &c);
	compression_case_free(&c);
	compression_case_init(&c, 3, 127, 1);
	compression_content("test_compression_type3_raw", &c);
	compression_case_free(&c);
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (sizes[i] <= 65536) {
			compression_case_init(&c, 3, sizes[i], 0);
			snprintf(name, sizeof(name), "test_compression_type3_zlib_size_%zu", sizes[i]);
			compression_content(name, &c);
			compression_case_free(&c);
		}
		for (unsigned encoding = 0; encoding < 3; encoding++) {
			compression_case_init(&c, 4, sizes[i], encoding == 0 ? 0 :
			    encoding == 1 ? UINT32_MAX : 5);
			c.objects[0].fragmented = encoding == 2;
			snprintf(name, sizeof(name), "test_compression_type4_%s_size_%zu",
			    encoding == 0 ? "zlib" : encoding == 1 ? "stored" : "fragmented_mixed", sizes[i]);
			compression_content(name, &c);
			compression_case_free(&c);
		}
	}
}

static void
compression_listing_failure(const char *name, compression_case_t *c,
    fsimg_status_t expected, const char *message)
{
	listing_t listed = list_plan(name, &c->plan, expected, message);

	expect_true(name, listed.count == 0 && strcmp(listed.error.path, "/a") == 0);
}

static void
test_compression_listing_failures(void)
{
	static const unsigned unsupported[] = { 0, 2, 13 };
	static const char *malformed[] = { "missing_attribute", "bad_magic",
	    "short_attribute", "nonempty_data_fork" };
	compression_case_t c;
	char name[96];

	for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
		compression_case_init(&c, 3, 1, 0);
		switch (i) {
		case 0:
			c.plan.value_count = 0;
			break;
		case 1:
			c.header[0] = 'x';
			break;
		case 2:
			c.attributes[0].size = 15;
			break;
		case 3:
			c.objects[0].data = (const unsigned char *)"x";
			c.objects[0].data_size = 1;
			break;
		}
		snprintf(name, sizeof(name), "test_compression_listing_%s", malformed[i]);
		compression_listing_failure(name, &c, FSIMG_ECORRUPT, NULL);
		compression_case_free(&c);
	}
	for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
		compression_case_init(&c, unsupported[i], 1, 0);
		snprintf(name, sizeof(name), "test_compression_unsupported_type_%u", unsupported[i]);
		compression_listing_failure(name, &c, FSIMG_EUNSUPPORTED,
		    "HFS Plus compression type is not supported");
		compression_case_free(&c);
	}
	for (unsigned type = 1; type <= 11; type += type == 1 ? 2 : 4) {
		compression_case_init(&c, type, 1, 0);
		gen_decmpfs_header(c.header, type, 65537);
		snprintf(name, sizeof(name), "test_compression_inline_type_%u_above_window", type);
		compression_listing_failure(name, &c, FSIMG_EUNSUPPORTED, NULL);
		compression_case_free(&c);
	}
	compression_case_init(&c, 3, 1, 0);
	gen_decmpfs_header(c.header, 3, UINT64_C(1) << 63);
	compression_listing_failure("test_compression_size_above_signed_limit", &c, FSIMG_ECORRUPT, NULL);
	compression_case_free(&c);
}

/* Payload failures must be deferred: preparation still lists both siblings. */
static void
compression_bad_content(const char *name, compression_case_t *c, size_t confirmed,
    const char *message)
{
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char *bytes = malloc(c->size + 1);
	size_t count = 0;
	listing_t listed;

	CHECK(bytes != NULL);
	gen_build(&c->plan, &image);
	listed = list_image(&image);
	expect_status(name, listed.status, FSIMG_DONE, &listed.error);
	if (listed.status == FSIMG_DONE)
		expect_true(name, listed.count == 2);
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		memset(bytes, 0xa5, c->size + 1);
		status = fsimg_reader_read_at(reader, 0, bytes, c->size, &count, &error);
		expect_status(name, status, FSIMG_ECORRUPT, &error);
		if (message != NULL)
			expect_true(name, strcmp(error.message, message) == 0);
		expect_true(name, count == confirmed && bytes[confirmed] == 0xa5);
		if (confirmed != 0)
			expect_true(name, bcmp(bytes, c->plain, confirmed) == 0);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	free(bytes);
}

/* Frame one arbitrary stored stream without disturbing any fork relation. */
static void
compression_single_stream(compression_case_t *c, const unsigned char *stream,
    size_t length)
{
	free(c->resource);
	c->size = 65536;
	gen_decmpfs_header(c->header, 4, c->size);
	c->objects[0].resource_size = 322 + length;
	c->resource = calloc(1, c->objects[0].resource_size);
	CHECK(c->resource != NULL);
	c->objects[0].resource = c->resource;
	gen_store(c->resource, 256, 4);
	gen_store(c->resource + 4, 272 + length, 4);
	gen_store(c->resource + 8, 16 + length, 4);
	gen_store(c->resource + 12, 50, 4);
	gen_store(c->resource + 256, 12 + length, 4);
	compression_le(c->resource + 260, 1, 4);
	compression_le(c->resource + 264, 12, 4);
	compression_le(c->resource + 268, length, 4);
	bcopy(stream, c->resource + 272, length);
}

static void
test_compression_codec_corruption(void)
{
	static const unsigned types[] = { 7, 8, 11, 12 };
	static const char *damage[] = { "truncated", "long", "short", "raw_wrong_length" };
	compression_case_t c;
	char name[128];

	if (!decoder_supported())
		return;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		unsigned type = types[t];
		bool lzvn = type == 7 || type == 8;
		bool fork = type == 8 || type == 12;

		for (unsigned d = 0; d < (lzvn ? 4u : 3u); d++) {
			const lzfse_vector_t *v = &lzfse_vectors[lzvn ? 1 : 2];
			size_t declared = v->plain_size;

			compression_vector_init(&c, type, v, false, d == 3);
			if (d == 0) {
				if (fork)
					compression_le(c.resource + 4,
					    compression_word(c.resource + 4) - 1, 4);
				else
					c.attributes[0].size--;
			} else {
				declared += d == 2 ? 1 : -1;
				gen_decmpfs_header(c.header, type, declared);
				c.size = declared;
			}
			snprintf(name, sizeof(name), "test_compression_type%u_%s", type, damage[d]);
			compression_bad_content(name, &c, 0, NULL);
			compression_case_free(&c);
		}
	}
}

/* Measure the retained first-read allocations. One byte less excludes the
 * final scratch allocation; the exact charge admits it. Closing either path
 * must return the context to the fixture's allocation baseline.
 */
static void
test_compression_lzfse_scratch_budget(void)
{
	compression_case_t c;
	gen_image_t image;
	uint64_t growth = 0;
	const char *name = "test_compression_lzfse_scratch_budget";

	if (!decoder_supported())
		return;
	compression_vector_init(&c, 12, &lzfse_vectors[2], false, false);
	gen_build(&c.plan, &image);
	for (unsigned trial = 0; trial < 3; trial++) {
		fixture_t f;
		fsimg_reader_t *reader = compression_open(name, &f, &image);
		fsimg_stats_t before, after;
		fsimg_error_t error = { 0 };
		unsigned char byte;
		size_t count;

		if (reader != NULL) {
			CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
			if (trial != 0)
				f.ctx->options.memory_bytes = before.memory_bytes + growth -
				    (trial == 1 ? 1 : 0);
			fsimg_status_t status = fsimg_reader_read_at(reader, 0,
			    &byte, 1, &count, &error);
			expect_status(name, status, trial == 1 ? FSIMG_ELIMIT : FSIMG_OK, &error);
			if (trial == 1)
				expect_true(name, count == 0);
			else
				expect_true(name, count == 1 && byte == c.plain[0]);
			CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
			if (trial == 0 && status == FSIMG_OK) {
				growth = after.memory_bytes - before.memory_bytes;
				expect_true(name, growth >= fsimg_lzfse_scratch_bytes());
			}
			fsimg_reader_free(reader);
		}
		fixture_close(&f);
		if (growth == 0)
			break;
	}
	gen_free(&image);
	compression_case_free(&c);
}

typedef enum compression_damage {
	CMP_DATA_RELATION, CMP_RESOURCE_RELATION, CMP_COUNT, CMP_TABLE_END,
	CMP_NO_BLOCKS, CMP_SHORT_HEADER, CMP_TABLE_OVERLAP, CMP_PREVIOUS_OVERLAP,
	CMP_NEXT_OVERLAP, CMP_MAP_OVERLAP, CMP_STORED_TOO_LARGE, CMP_STORED_ZERO,
	CMP_DECODE_SHORT, CMP_DECODE_LONG, CMP_LAST_SHORT, CMP_LAST_LONG,
	CMP_RAW_WRONG_SIZE, CMP_BAD_ZLIB, CMP_TRUNCATED_ZLIB, CMP_TRAILING_ZLIB,
	CMP_DICTIONARY
} compression_damage_t;

static void
test_compression_word_table_corruption(void)
{
	static const struct {
		const char *name;
		uint32_t start;
		uint32_t end;
		size_t fork_size;
		const char *message;
	} cases[] = {
		{ "table_overlaps_payload", 7, 16, 16,
		    "HFS Plus compressed file's chunk table is malformed" },
		{ "table_past_fork", 17, 16, 16,
		    "HFS Plus compressed file's chunk table is malformed" },
		{ "descending_offsets", 8, 7, 16,
		    "HFS Plus chunks overlap" },
		{ "stored_above_bound", 8, 8 + 69633, 8 + 69633,
		    "HFS Plus chunk lies outside the resource data" }
	};
	char name[112];

	if (!decoder_supported())
		return;
	for (unsigned type = 8; type <= 12; type += 4) {
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			compression_case_t c;

			/* One logical chunk needs two words. The oversized chunk
			 * stays inside its fork to isolate the stored-length bound.
			 */
			compression_case_init(&c, type, 1, 0);
			c.resource = calloc(1, cases[i].fork_size);
			CHECK(c.resource != NULL);
			compression_le(c.resource, cases[i].start, 4);
			compression_le(c.resource + 4, cases[i].end, 4);
			c.objects[0].resource = c.resource;
			c.objects[0].resource_size = cases[i].fork_size;
			snprintf(name, sizeof(name), "test_compression_type%u_%s",
			    type, cases[i].name);
			compression_bad_content(name, &c, 0, cases[i].message);
			compression_case_free(&c);
		}
	}
}

static void
test_compression_resource_map_offsets(void)
{
	const char *message = "HFS Plus resource fork's header does not match the fork";
	compression_case_t c;
	uint32_t first_chunk;

	compression_case_init(&c, 4, 131200, 0);
	/* Type 4 pairs are relative to the count word at fork offset 260. */
	first_chunk = 260 + compression_word(c.resource + 264);
	gen_store(c.resource + 4, first_chunk, 4);
	compression_bad_content("test_compression_type4_map_at_first_chunk", &c, 0, message);
	compression_case_free(&c);

	compression_case_init(&c, 4, 131200, 0);
	gen_store(c.resource + 4, c.objects[0].resource_size + 1, 4);
	compression_bad_content("test_compression_type4_map_past_fork", &c, 0, message);
	compression_case_free(&c);
}

/* A compressed file decodes to more than the volume stores. The default
 * output limit is 16 times the slice, so such a file is read at the default
 * and its bytes match.
 */
static void
test_compressed_file_larger_than_slice_reads_at_default_limit(void)
{
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	compression_case_init(&c, 4, 3 * 1024 * 1024 + 100, 0);
	gen_build(&c.plan, &image);
	expect_true("decoded file exceeds the slice", c.size > image.length);
	expect_true("decoded file is under the default output limit",
	    c.size < FSIMG_DEFAULT_OUTPUT_MULTIPLE * (uint64_t)image.length);
	reader = compression_open("compressed file larger than slice", &f, &image);
	if (reader != NULL) {
		expect_true("reader default limit is 16 times the slice",
		    reader->session.output_limit ==
		    FSIMG_DEFAULT_OUTPUT_MULTIPLE * (uint64_t)image.length);
		status = fsimg_reader_charge_file(reader, &error);
		expect_status("default limit admits the decoded size", status,
		    FSIMG_OK, &error);
		compression_read_pieces("compressed file larger than slice reads",
		    reader, c.plain, c.size, 4096);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

/* The refusal side: a decoded size beyond 16 times the slice stops at the
 * default limit.
 */
static void
test_compressed_file_beyond_default_limit_is_refused(void)
{
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	const uint64_t declared = 64 * 1024 * 1024;

	/* The generator's resource fork holds at most 64 chunks, so the header
	 * alone declares a decoded size past the limit. The charge comes before
	 * any read, so the short fork is never consulted.
	 */
	compression_case_init(&c, 4, 3 * 1024 * 1024 + 100, 0);
	gen_decmpfs_header(c.header, 4, declared);
	gen_build(&c.plan, &image);
	expect_true("decoded file exceeds the default output limit",
	    declared > FSIMG_DEFAULT_OUTPUT_MULTIPLE * (uint64_t)image.length);
	reader = compression_open("compressed file beyond default limit", &f, &image);
	if (reader != NULL) {
		status = fsimg_reader_charge_file(reader, &error);
		expect_status("default limit refuses the decoded size", status,
		    FSIMG_ELIMIT, &error);
		expect_true("refusal names the output resource",
		    error.resource == FSIMG_RESOURCE_OUTPUT);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_resource_corruption(void)
{
	static const struct {
		const char *name;
		compression_damage_t damage;
	} cases[] = {
		{ "data_relation", CMP_DATA_RELATION },
		{ "resource_relation", CMP_RESOURCE_RELATION },
		{ "count", CMP_COUNT },
		{ "truncated_table", CMP_TABLE_END },
		{ "zero_blocks", CMP_NO_BLOCKS },
		{ "short_header", CMP_SHORT_HEADER },
		{ "table_overlap", CMP_TABLE_OVERLAP },
		{ "previous_overlap", CMP_PREVIOUS_OVERLAP },
		{ "next_overlap", CMP_NEXT_OVERLAP },
		{ "map_overlap", CMP_MAP_OVERLAP },
		{ "stored_above_bound", CMP_STORED_TOO_LARGE },
		{ "zero_stored_length", CMP_STORED_ZERO },
		{ "decode_short", CMP_DECODE_SHORT },
		{ "decode_long", CMP_DECODE_LONG },
		{ "last_decode_short", CMP_LAST_SHORT },
		{ "last_decode_long", CMP_LAST_LONG },
		{ "raw_wrong_length", CMP_RAW_WRONG_SIZE },
		{ "bad_zlib", CMP_BAD_ZLIB },
		{ "truncated_zlib", CMP_TRUNCATED_ZLIB },
		{ "trailing_zlib", CMP_TRAILING_ZLIB },
		{ "preset_dictionary", CMP_DICTIONARY }
	};
	char name[112];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		compression_case_t c;
		size_t confirmed = 0;
		unsigned char *r, *pair, *chunk;
		uint32_t offset, length;

		compression_case_init(&c, 4, 131200, 0);
		r = c.resource;
		pair = r + 264;
		offset = compression_word(pair);
		length = compression_word(pair + 4);
		chunk = r + 260 + offset;
		switch (cases[i].damage) {
		case CMP_DATA_RELATION:
			gen_store(r + 12, 49, 4);
			break;
		case CMP_RESOURCE_RELATION:
			gen_store(r + 256, gen_load(r + 256, 4) - 1, 4);
			break;
		case CMP_COUNT:
			compression_le(r + 260, 2, 4);
			break;
		case CMP_TABLE_END:
			/* Valid framing, three pairs cannot fit in eight data bytes. */
			gen_store(r + 8, 8, 4);
			gen_store(r + 256, 4, 4);
			gen_store(r + 4, 264, 4);
			c.objects[0].resource_size = 314;
			break;
		case CMP_NO_BLOCKS:
			c.objects[0].resource_size = 0;
			break;
		case CMP_SHORT_HEADER:
			c.objects[0].resource_size = 15;
			break;
		case CMP_TABLE_OVERLAP:
			compression_le(pair, 4, 4);
			break;
		case CMP_PREVIOUS_OVERLAP:
			compression_le(pair + 8, offset + length - 1, 4);
			break;
		case CMP_NEXT_OVERLAP:
			compression_le(pair + 4, length + 1, 4);
			break;
		case CMP_MAP_OVERLAP:
			pair += 16;
			compression_le(pair + 4, compression_word(pair + 4) + 1, 4);
			confirmed = 131072;
			break;
		case CMP_STORED_TOO_LARGE: {
			unsigned char *oversized = calloc(1, 69633);

			CHECK(oversized != NULL);
			oversized[0] = 0xff;
			compression_single_stream(&c, oversized, 69633);
			free(oversized);
			break;
		}
		case CMP_STORED_ZERO:
			compression_le(pair + 4, 0, 4);
			break;
		case CMP_DECODE_SHORT:
		case CMP_DECODE_LONG:
		case CMP_LAST_SHORT:
		case CMP_LAST_LONG: {
			unsigned char plain[65537];
			uLongf encoded = length;
			size_t decoded = cases[i].damage == CMP_DECODE_SHORT ? 65535 : 65537;

			if (cases[i].damage == CMP_LAST_SHORT || cases[i].damage == CMP_LAST_LONG) {
				pair += 16;
				chunk = r + 260 + compression_word(pair);
				/* Replace the last stream within its allocated range. */
				encoded = compression_word(pair + 4);
				decoded = cases[i].damage == CMP_LAST_SHORT ? 127 : 129;
				confirmed = 131072;
			}
			bzero(plain, sizeof(plain));
			CHECK(compress2(chunk, &encoded, plain, decoded, Z_BEST_COMPRESSION) == Z_OK);
			compression_le(pair + 4, encoded, 4);
			break;
		}
		case CMP_RAW_WRONG_SIZE:
			chunk[0] = 0xff;
			break;
		case CMP_BAD_ZLIB:
			chunk[0] = 0;
			break;
		case CMP_TRUNCATED_ZLIB:
			compression_le(pair + 4, length - 1, 4);
			break;
		case CMP_TRAILING_ZLIB: {
			/* Append one byte before a complete, still 50-byte map. */
			pair += 16;
			length = compression_word(pair + 4);
			size_t old_size = c.objects[0].resource_size;
			unsigned char *grown = realloc(r, old_size + 1);

			CHECK(grown != NULL);
			c.resource = r = grown;
			c.objects[0].resource = r;
			c.objects[0].resource_size = old_size + 1;
			pair = r + 280;
			size_t map = (size_t)gen_load(r + 4, 4);

			memmove(r + map + 1, r + map, 50);
			r[map] = 0;
			compression_le(pair + 4, length + 1, 4);
			gen_store(r + 8, gen_load(r + 8, 4) + 1, 4);
			gen_store(r + 256, gen_load(r + 256, 4) + 1, 4);
			gen_store(r + 4, map + 1, 4);
			confirmed = 131072;
			break;
		}
		case CMP_DICTIONARY: {
			z_stream stream = { 0 };
			static const unsigned char dictionary[] = "dictionary";

			CHECK(deflateInit(&stream, Z_BEST_COMPRESSION) == Z_OK);
			CHECK(deflateSetDictionary(&stream, dictionary, sizeof(dictionary)) == Z_OK);
			bzero(c.plain, 65536);
			stream.next_in = c.plain;
			stream.avail_in = 65536;
			stream.next_out = chunk;
			stream.avail_out = length;
			CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
			compression_le(pair + 4, stream.total_out, 4);
			CHECK(deflateEnd(&stream) == Z_OK);
			break;
		}
		}
		snprintf(name, sizeof(name), "test_compression_resource_%s", cases[i].name);
		compression_bad_content(name, &c, confirmed, NULL);
		compression_case_free(&c);
	}
}

static void
test_compression_inline_corruption(void)
{
	compression_case_t c;
	char name[96];

	for (unsigned encoding = 0; encoding < 3; encoding++) {
		unsigned type = encoding == 0 ? 1 : 3;

		for (unsigned longer = 0; longer < 2; longer++) {
			compression_case_init(&c, type, 127, encoding == 2 ? 1 : 0);
			gen_decmpfs_header(c.header, type, longer ? 126 : 128);
			c.size = longer ? 126 : 128;
			snprintf(name, sizeof(name),
			    "test_compression_inline_%s_decode_%s",
			    encoding == 0 ? "type1" : encoding == 1 ? "zlib" : "raw",
			    longer ? "long" : "short");
			compression_bad_content(name, &c, 0, NULL);
			compression_case_free(&c);
		}
	}
}

/* Watch only payload/table intervals, so catalog and extent lookups cannot
 * satisfy the assertions about compressed bytes being fetched exactly once.
 */
static void
compression_watch(fixture_t *f, gen_image_t *image, compression_case_t *c)
{
	unsigned char *record = catalog_data(image, 2, 2);
	uint64_t base = gen_load(record + FILE_RESOURCE_FORK + 16, 4) * image->block_size;
	size_t count = (c->size + 65535) / 65536;
	bool flat = compression_word(c->header + 4) != 4;

	CHECK(count <= 4 && !c->objects[0].fragmented);
	f->watched_start[4] = base + (flat ? 0 : 264);
	f->watched_length[4] = flat ? 4 * (count + 1) : 8 * count;
	for (size_t i = 0; i < count; i++) {
		if (flat) {
			uint32_t start = compression_word(c->resource + 4 * i);

			f->watched_start[i] = base + start;
			f->watched_length[i] = compression_word(c->resource + 4 * (i + 1)) - start;
		} else {
			f->watched_start[i] = base + 260 + compression_word(c->resource + 264 + 8 * i);
			f->watched_length[i] = compression_word(c->resource + 268 + 8 * i);
		}

		CHECK(f->watched_length[i] > 0);
	}
}

static void
compression_counted_reads(unsigned type)
{
	char name[96];
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	unsigned char bytes[32];
	size_t count;
	fsimg_error_t error = { 0 };

	snprintf(name, sizeof(name), "test_compression_type%u_counted_reads", type);
	if (type == 4)
		compression_case_init(&c, 4, 200000, 5);
	else
		compression_vector_init(&c, type, &lzfse_vectors[1], true, false);
	size_t chunks = (c.size + 65535) / 65536;
	gen_build(&c.plan, &image);
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		compression_watch(&f, &image, &c);
		uint64_t work = reader->session.progress.work;
		uint64_t consumed = 0;

		if (!compression_read_pieces(name, reader, c.plain, c.size, 4096))
			goto close_reader;
		for (size_t i = 0; i < chunks; i++) {
			consumed += f.watched_length[i];
			expect_true("test_compression_each_chunk_read_once",
			    f.watched_bytes[i] == f.watched_length[i]);
		}
		expect_true("test_compression_charges_consumed_and_produced",
		    reader->session.progress.work - work >= c.size + consumed);
		if (type == 4)
			expect_true("test_compression_table_read_once_per_chunk",
			    f.watched_calls[4] == chunks);
		unsigned reads = f.reads;
		fsimg_status_t status = fsimg_reader_read_at(reader, c.size - 32,
		    bytes, sizeof(bytes), &count, &error);
		expect_status("test_compression_current_chunk_is_cached", status, FSIMG_OK, &error);
		expect_true("test_compression_current_chunk_is_cached", count == sizeof(bytes) &&
		    bcmp(bytes, c.plain + c.size - 32, count) == 0 && reads == f.reads);
		status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes), &count, &error);
		expect_status("test_compression_backward_seek_decodes_again", status, FSIMG_OK, &error);
		expect_true("test_compression_backward_seek_decodes_again",
		    count == sizeof(bytes) && bcmp(bytes, c.plain, count) == 0 &&
		    f.watched_bytes[0] == 2 * f.watched_length[0]);
close_reader:
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_counted_reads(void)
{
	static const unsigned types[] = { 4, 8, 12 };

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (types[i] != 4 && !decoder_supported())
			continue;
		compression_counted_reads(types[i]);
	}
}

static void
test_compression_advance_invalidates_window(void)
{
	for (unsigned inline_first = 0; inline_first < 2; inline_first++) {
		const char *name = inline_first ? "test_compression_file_after_inline" :
		    "test_compression_two_files_chunk_zero";
		compression_case_t a, b;
		gen_image_t image;
		fixture_t f;
		fsimg_reader_t *reader;
		const fsimg_entry_t *entry;
		fsimg_error_t error = { 0 };
		unsigned char bytes[128];
		size_t count;

		compression_case_init(&a, inline_first ? 3 : 4, 65536, 0);
		compression_case_init(&b, 4, 65536, 0);
		memset(b.plain, 'Z', b.size);
		free(b.resource);
		b.resource = gen_decmpfs_resource(b.plain, b.size, 0, &b.objects[0].resource_size);
		b.objects[0].resource = b.resource;
		a.objects[1] = b.objects[0];
		a.objects[1].name = name_b;
		a.objects[1].id = 33;
		a.attributes[1] = b.attributes[0];
		a.attributes[1].id = 33;
		a.plan.value_count = 2;
		gen_build(&a.plan, &image);
		reader = compression_open(name, &f, &image);
		if (reader != NULL) {
			fsimg_status_t status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes), &count, &error);
			expect_status(name, status, FSIMG_OK, &error);
			expect_true(name, count == sizeof(bytes) && bcmp(bytes, a.plain, count) == 0);
			status = fsimg_reader_next(reader, &entry, &error);
			expect_status(name, status, FSIMG_OK, &error);
			if (status == FSIMG_OK) {
				status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes), &count, &error);
				expect_status(name, status, FSIMG_OK, &error);
				expect_true(name, count == sizeof(bytes) && bcmp(bytes, b.plain, count) == 0);
			}
			fsimg_reader_free(reader);
		}
		fixture_close(&f);
		gen_free(&image);
		compression_case_free(&a);
		compression_case_free(&b);
	}
}

static bool
compression_cancelled(void *argument)
{
	return (((fixture_t *)argument)->cancelled);
}

static void
test_compression_cancel_during_decode(void)
{
	const char *name = "test_compression_cancel_during_decode";
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	unsigned char bytes[65536];
	size_t count;

	compression_case_init(&c, 4, 65536, 0);
	gen_build(&c.plan, &image);
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		compression_watch(&f, &image, &c);
		f.ctx->options.cancelled = compression_cancelled;
		f.ctx->options.cancel_argument = &f;
		f.cancel_on_payload = true;
		expect_status(name, fsimg_reader_read_at(reader, 0, bytes,
		    sizeof(bytes), &count, &error), FSIMG_ECANCELLED, &error);
		expect_true(name, count == 0);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_work_consumed_plus_produced(void)
{
	const char *name = "test_compression_work_consumed_plus_produced";
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	unsigned char bytes[65536];
	size_t count;

	compression_case_init(&c, 4, 65536, 0);
	gen_build(&c.plan, &image);
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		compression_watch(&f, &image, &c);
		f.ctx->options.max_work = reader->session.progress.work +
		    65536 + compression_word(c.resource + 268) - 1;
		expect_status(name, fsimg_reader_read_at(reader, 0, bytes,
		    sizeof(bytes), &count, &error), FSIMG_ELIMIT, &error);
		expect_true(name, count == 0);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_charge_before_decode(void)
{
	const char *name = "test_compression_charge_before_decode";
	compression_case_t c;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };

	compression_case_init(&c, 4, 65536, 0);
	gen_build(&c.plan, &image);

	fixture_init(&f, &image, 0, 0);
	f.ctx->options.max_output_bytes = 65535;
	reader = NULL;
	expect_status(name, fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error),
	    FSIMG_OK, &error);
	if (reader != NULL) {
		compression_watch(&f, &image, &c);
		expect_status(name, fsimg_reader_next(reader, &entry, &error), FSIMG_OK, &error);
		expect_status(name, fsimg_reader_charge_file(reader, &error),
		    FSIMG_ELIMIT, &error);
		expect_true(name, f.watched_bytes[0] == 0);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
compression_allocation_failure(unsigned type)
{
	char name[80];
	compression_case_t c;
	gen_image_t image;
	bool completed = false, failed = false;

	snprintf(name, sizeof(name), "test_compression_type%u_allocation_failure", type);
	compression_case_init(&c, type, 65536, 0);
	gen_build(&c.plan, &image);
	for (uint64_t attempt = 1; attempt <= 64; attempt++) {
		fixture_t f;
		fsimg_reader_t *reader = compression_open(name, &f, &image);
		fsimg_error_t error = { 0 };
		unsigned char byte;
		size_t count;

		if (reader == NULL) {
			fixture_close(&f);
			break;
		}
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		fsimg_status_t status = fsimg_reader_read_at(reader, 0, &byte, 1, &count, &error);
		if (status == FSIMG_OK) {
			completed = true;
			expect_true(name, count == 1 && byte == c.plain[0]);
		} else {
			failed = true;
			expect_status(name, status, FSIMG_ENOMEM, &error);
			expect_true(name, count == 0);
		}
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fsimg_reader_free(reader);
		fixture_close(&f);
		if (completed)
			break;
	}
	expect_true(name, completed && failed);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_allocation_failure(void)
{
	compression_allocation_failure(3);
	compression_allocation_failure(4);
}

/* A small encoded stream expands far beyond the declared one-window size.
 * Compare context peaks with a raw compressed file, including zlib's charged
 * state in a fixed allowance, rather than requiring one allocator layout.
 */
static uint64_t
compression_first_read_peak(const char *name, compression_case_t *c,
    fsimg_status_t expected, bool no_payload)
{
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_error_t error = { 0 };
	fsimg_stats_t stats;
	unsigned char byte;
	size_t count;

	gen_build(&c->plan, &image);
	reader = compression_open(name, &f, &image);
	if (reader != NULL) {
		if (c->resource != NULL)
			compression_watch(&f, &image, c);
		fsimg_status_t status = fsimg_reader_read_at(reader, 0, &byte, 1, &count, &error);
		expect_status(name, status, expected, &error);
		expect_true(name, count == (expected == FSIMG_OK ? 1u : 0u));
		if (no_payload)
			expect_true(name, f.watched_bytes[0] == 0);
		fsimg_reader_free(reader);
	}
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	fixture_close(&f);
	gen_free(&image);
	return (stats.memory_peak);
}

static void
test_compression_bombs(void)
{
	compression_case_t c;
	unsigned char *bomb = calloc(1, 1024 * 1024);
	unsigned char encoded[2048];
	uLongf length = sizeof(encoded);
	/* Measured charged inflate state and window total 39960 bytes. */
	const uint64_t inflate_memory_allowance = 40 * 1024;
	uint64_t baseline, peak;

	CHECK(bomb != NULL);
	CHECK(compress2(encoded, &length, bomb, 1024 * 1024, Z_BEST_COMPRESSION) == Z_OK);
	compression_case_init(&c, 4, 65536, 1);
	baseline = compression_first_read_peak("test_compression_stored_peak", &c, FSIMG_OK, false);
	compression_case_free(&c);
	for (unsigned inline_payload = 0; inline_payload < 2; inline_payload++) {
		const char *name = inline_payload ? "test_compression_inline_bomb" : "test_compression_chunk_bomb";

		compression_case_init(&c, inline_payload ? 3 : 4, 65536, 0);
		if (inline_payload) {
			/* A maximally compressible megabyte fits an ordinary node. */
			CHECK(length + 16 <= 3802);
			c.attributes[0].value = malloc(length + 16);
			CHECK(c.attributes[0].value != NULL);
			gen_decmpfs_header((unsigned char *)c.attributes[0].value, 3, 65536);
			bcopy(encoded, (unsigned char *)c.attributes[0].value + 16, length);
			c.attributes[0].size = length + 16;
		} else {
			compression_single_stream(&c, encoded, length);
		}
		peak = compression_first_read_peak(name, &c, FSIMG_ECORRUPT, false);
		expect_true(name, peak <= baseline + inflate_memory_allowance);
		if (inline_payload)
			free((void *)c.attributes[0].value);
		compression_case_free(&c);
	}
	compression_case_init(&c, 4, 1, 0);
	gen_decmpfs_header(c.header, 4, UINT64_C(1) << 40);
	(void)compression_first_read_peak("test_compression_huge_size_one_chunk", &c, FSIMG_ECORRUPT, true);
	compression_case_free(&c);
	free(bomb);
}

static void
test_compression_empty_extra_data(void)
{
	static const struct {
		const char *name;
		unsigned type;
		bool payload;
	} cases[] = {
		{ "empty_type4_nonempty_fork_is_extra", 4, true },
		{ "empty_type1_nonempty_payload_is_extra", 1, true },
		{ "empty_type3_nonempty_payload_is_extra", 3, true },
		{ "empty_type8_nonempty_fork_is_extra", 8, true },
		{ "empty_type4_empty_fork_is_not_extra", 4, false },
		{ "empty_type1_empty_payload_is_not_extra", 1, false },
		{ "empty_type3_empty_payload_is_not_extra", 3, false }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		compression_case_t c;
		listing_t listed;
		const char *name = cases[i].name;

		compression_case_init(&c, cases[i].type, 0, 0);
		c.attributes[0].size = 16;
		c.objects[0].resource_size = 0;
		if (cases[i].payload) {
			if (cases[i].type == 4 || cases[i].type == 8) {
				c.objects[0].resource = (const unsigned char *)"fork";
				c.objects[0].resource_size = 4;
			} else {
				c.header[16] = 0xff;
				c.attributes[0].size = 17;
			}
		}
		listed = list_plan(name, &c.plan, FSIMG_DONE, NULL);
		if (listed.status == FSIMG_DONE)
			expect_true(name, !!(listed.entries[0].metadata.valid &
			    FSIMG_META_EXTRA_DATA) == cases[i].payload);
		if (!cases[i].payload)
			check_content(name, &c.plan, "/a", c.plain, 0, FSIMG_OK, NULL);
		compression_case_free(&c);
	}
}

static void
test_compression_releases_held_span(void)
{
	for (unsigned corrupt = 0; corrupt < 2; corrupt++) {
		const char *name = corrupt ? "compressed_failed_read_releases_held_span" :
		    "compressed_successful_read_releases_held_span";
		compression_case_t c;
		gen_image_t image;
		fixture_t f;
		fsimg_reader_t *reader;
		fsimg_error_t error = { 0 };
		unsigned char byte;
		size_t count;

		compression_case_init(&c, 3, 127, 0);
		if (corrupt)
			c.header[16] = 0;
		gen_build(&c.plan, &image);
		reader = compression_open(name, &f, &image);
		if (reader != NULL) {
			expect_status(name, fsimg_reader_read_at(reader, 0, &byte, 1,
			    &count, &error), corrupt ? FSIMG_ECORRUPT : FSIMG_OK, &error);
			expect_true(name, reader->held == NULL);
			fsimg_reader_free(reader);
		}
		fixture_close(&f);
		gen_free(&image);
		compression_case_free(&c);
	}
}

static void
test_compression_changed_since_listing(void)
{
	static const struct {
		const char *name;
		bool owner;
		size_t offset;
		uint64_t value;
		size_t width;
		const char *message;
	} cases[] = {
		{ "compression_type_changed_since_listing", false, 4, 1, 4,
		    "HFS Plus compression attribute changed since it was listed" },
		{ "compression_size_changed_since_listing", false, 8, 126, 8,
		    "HFS Plus compression attribute changed since it was listed" },
		{ "compression_owner_flag_cleared_since_listing", true, 41, 0, 1,
		    "HFS Plus file record changed since it was listed" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		compression_case_t c;
		gen_image_t image;
		fixture_t f;
		fsimg_reader_t *reader;
		fsimg_error_t error = { 0 };
		unsigned char byte, *field = NULL;
		size_t count;
		const char *name = cases[i].name;

		compression_case_init(&c, 3, 127, 0);
		gen_build(&c.plan, &image);
		reader = compression_open(name, &f, &image);
		if (reader != NULL) {
			gen_tree_t *tree = &image.trees[cases[i].owner ? GEN_CATALOG : GEN_ATTRIBUTES];

			for (size_t j = 0; j < tree->record_count; j++) {
				gen_record_t *r = &tree->records[j];
				unsigned char *data = image.bytes + r->offset + r->key_length;

				if (cases[i].owner) {
					if (gen_load(data, 2) == 2 && gen_load(data + 8, 4) == 32)
						field = data + cases[i].offset;
				} else if (gen_load(data, 4) == 0x10)
					field = data + 16 + cases[i].offset;
			}
			CHECK(field != NULL);
			if (cases[i].owner)
				gen_store(field, cases[i].value, cases[i].width);
			else
				compression_le(field, cases[i].value, cases[i].width);
			expect_status(name, fsimg_reader_read_at(reader, 0, &byte, 1,
			    &count, &error), FSIMG_ECORRUPT, &error);
			expect_true(name, count == 0 && strcmp(error.message, cases[i].message) == 0);
			fsimg_reader_free(reader);
		}
		fixture_close(&f);
		gen_free(&image);
		compression_case_free(&c);
	}
}

static void
test_compression_truncated_charge(void)
{
	const char *name = "compressed_truncated_volume_charge_probes_content";
	compression_case_t c;
	gen_image_t image, slice;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };

	compression_case_init(&c, 4, 3 * 65536, 0);
	gen_build(&c.plan, &image);
	slice = image;
	slice.length -= 512;
	fixture_init(&f, &slice, 0, 0);
	f.ctx->options.max_output_bytes = 65535;
	reader = NULL;
	expect_status(name, fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error),
	    FSIMG_OK, &error);
	if (reader != NULL) {
		expect_status(name, fsimg_reader_next(reader, &entry, &error), FSIMG_OK, &error);
		compression_watch(&f, &image, &c);
		expect_status(name, fsimg_reader_get_info(reader, &info, &error), FSIMG_OK, &error);
		expect_true(name, info.missing_bytes == 512);
		expect_status(name, fsimg_reader_charge_file(reader, &error), FSIMG_ELIMIT, &error);
		expect_true(name, f.watched_bytes[2] != 0);
		fsimg_reader_free(reader);
	}
	fixture_close(&f);
	gen_free(&image);
	compression_case_free(&c);
}

static void
test_compression_extra_data_and_hard_link(void)
{
	static const uint16_t other[] = { 'x' };
	compression_case_t c;
	listing_t listed;

	for (unsigned type = 1; type <= 4; type += type == 1 ? 2 : 1) {
		compression_case_init(&c, type, 1, 0);
		if (type != 4) {
			c.objects[0].resource = (const unsigned char *)"fork";
			c.objects[0].resource_size = 4;
		}
		listed = list_plan("test_compression_resource_extra_data", &c.plan, FSIMG_DONE, NULL);
		if (listed.status == FSIMG_DONE)
			expect_true("test_compression_resource_extra_data",
			    !!(listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA) == (type != 4));
		c.attributes[1] = (gen_attr_value_t){ .id = 32, .type = 0x10,
		    .name = other, .units = 1, .value = (const unsigned char *)"x", .size = 1 };
		c.plan.value_count = 2;
		listed = list_plan("test_compression_other_attribute_extra_data", &c.plan, FSIMG_DONE, NULL);
		if (listed.status == FSIMG_DONE)
			expect_true("test_compression_other_attribute_extra_data",
			    listed.entries[0].metadata.valid & FSIMG_META_EXTRA_DATA);
		compression_case_free(&c);
	}
	compression_case_init(&c, 3, 127, 0);
	gen_object_t objects[6];
	gen_plan_t plan;

	hard_links_plan(&plan, objects);
	objects[4].data = NULL;
	objects[4].data_size = 0;
	objects[4].flags |= 4;
	objects[4].owner_flags = 0x20;
	c.attributes[0].id = 41;
	plan.values = c.attributes;
	plan.value_count = 1;
	plan.attributes = true;
	check_content("test_compression_hard_link_indirect_content", &plan, "/a",
	    c.plain, c.size, FSIMG_OK, NULL);
	compression_case_free(&c);
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

/* Inspect serialized fork descriptors, without the reader's fork decoder or
 * validator. Zero-count slots must be zero pairs and terminate the used prefix;
 * raw byte checks catch invalid fixtures even when no content read opens them.
 */
static void
check_generated_fork(const char *name, const unsigned char *fork)
{
	bool unused = false;

	for (size_t i = 0; i < 8; i++) {
		uint64_t start = gen_load(fork + 16 + 8 * i, 4);
		uint64_t count = gen_load(fork + 20 + 8 * i, 4);

		expect_true(name, count == 0 ? start == 0 : !unused);
		unused |= count == 0;
	}
}

static void
test_generator_fork_descriptors(void)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2,
		    .data_size = 1, .resource_size = 1 },
		{ .name = name_c, .units = 1, .id = 34, .type = 2,
		    .data_size = 9 * 4096, .resource_size = 9 * 4096,
		    .fragmented = true }
	};
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	plan.attributes = true;
	plan.journal = true;
	plan.journal_file = true;
	plan.journal_info_file = true;
	gen_build(&plan, &image);
	for (size_t offset = GEN_BITMAP_FORK; offset <= GEN_STARTUP_FORK;
	    offset += 80) {
		check_generated_fork("generated volume special fork descriptors",
		    image.bytes + image.header + offset);
		check_generated_fork("generated alternate header fork descriptors",
		    image.bytes + image.length - 1024 + offset);
	}
	const gen_tree_t *tree = &image.trees[GEN_CATALOG];

	for (size_t i = 0; i < tree->record_count; i++) {
		const unsigned char *record = image.bytes + tree->records[i].offset;
		const unsigned char *data = record + 2 + gen_load(record, 2);
		char name[96];

		if (gen_load(data, 2) != 2)
			continue;
		snprintf(name, sizeof(name), "generated file %llu data fork descriptors",
		    (unsigned long long)gen_load(data + 8, 4));
		check_generated_fork(name, data + FILE_DATA_FORK);
		snprintf(name, sizeof(name), "generated file %llu resource fork descriptors",
		    (unsigned long long)gen_load(data + 8, 4));
		check_generated_fork(name, data + FILE_RESOURCE_FORK);
	}
	gen_free(&image);
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
	uint64_t compression_counts[256] = { 0 };

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
	/* Count the whole listing before delivery, so a corrupt payload does not
	 * hide compression types in later files. Reopen for the byte stream.
	 */
	status = fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error);
	while (status == FSIMG_OK &&
	    (status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK)
		if (entry->kind == FSIMG_ENTRY_FILE || entry->kind == FSIMG_ENTRY_SYMLINK)
			compression_counts[hfsplus_state(reader)->current.compression]++;
	fsimg_reader_free(reader);
	reader = NULL;
	if (status == FSIMG_DONE)
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
	for (size_t i = 0; i < 256; i++)
		if (compression_counts[i] != 0)
			fprintf(stderr, "compression type %zu: %" PRIu64 "\n",
			    i, compression_counts[i]);
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
			store_payload(payload, PREPARED_LEAF, leaf, 4);
			store_payload(payload, PREPARED_RECORD, j, 2);
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
	typedef enum prepared_mutation {
		PREPARED_FIELD,
		PREPARED_NODE_COUNT,
		PREPARED_SYMLINK_FIELD,
		PREPARED_LOCATION,
		PREPARED_BAD_NAME,
		PREPARED_UNCHANGED
	} prepared_mutation_t;
	typedef enum prepared_followup {
		PREPARED_ADVANCE,
		PREPARED_ENTER_FOLDER,
		PREPARED_READ_FILE
	} prepared_followup_t;
	typedef enum prepared_probe {
		PREPARED_NO_PROBE,
		PREPARED_LENGTH_PROBE,
		PREPARED_NAME_PROBE
	} prepared_probe_t;
	/* Each row names its fixture, mutation, observing operation and result. */
	static const struct {
		const char *name;
		uint16_t fixture_type;
		prepared_mutation_t mutation;
		size_t offset;
		uint64_t value;
		size_t width;
		uint32_t target_id;
		uint16_t target_type;
		prepared_followup_t followup;
		prepared_probe_t probe;
		fsimg_status_t expected;
		const char *diagnostic;
	} cases[] = {
		{ .name = "prepared unknown kind refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_KIND,
		    .value = 255,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared leaf at node count refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_NODE_COUNT,
		    .offset = PREPARED_LEAF,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared leaf above node count refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_LEAF,
		    .value = UINT32_MAX,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared reserved id refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_ID,
		    .value = 15,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared mode outside permission bits refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_MODE,
		    .value = 010000,
		    .width = 2,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared unknown flags refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_FLAGS,
		    .value = 0x80,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared unknown compression on file refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_COMPRESSION,
		    .value = 2,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared known compression on folder refused",
		    .fixture_type = 1,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_COMPRESSION,
		    .value = 3,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared file valence refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_VALENCE,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared file special field refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_SPECIAL,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared empty symlink refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_SYMLINK_FIELD,
		    .offset = PREPARED_SIZE,
		    .width = 8,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared oversized symlink refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_SYMLINK_FIELD,
		    .offset = PREPARED_SIZE,
		    .value = 1024,
		    .width = 8,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared FIFO with bytes refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_KIND,
		    .value = FSIMG_ENTRY_FIFO,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared extra data outside boolean refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_EXTRA,
		    .value = 2,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared owner without owner mode flag refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_OWNER,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared group without owner mode flag refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_GROUP,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared mode without owner mode flag refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_MODE,
		    .value = 0644,
		    .width = 2,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared folder count without flag refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_FOLDER_COUNT,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared folder count flag on file refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_FLAGS,
		    .value = HFSPLUS_CHILD_FOLDER_COUNT,
		    .width = 1,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared folder count on folder without flag refused",
		    .fixture_type = 1,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_FOLDER_COUNT,
		    .value = 1,
		    .width = 4,
		    .followup = PREPARED_ADVANCE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared HFS Plus entry payload" },
		{ .name = "prepared entry baseline",
		    .fixture_type = 2,
		    .mutation = PREPARED_UNCHANGED,
		    .followup = PREPARED_ADVANCE,
		    .probe = PREPARED_LENGTH_PROBE,
		    .expected = FSIMG_OK },
		{ .name = "prepared folder location names another folder",
		    .fixture_type = 1,
		    .mutation = PREPARED_LOCATION,
		    .target_id = 35,
		    .target_type = 1,
		    .followup = PREPARED_ENTER_FOLDER,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "HFS Plus folder record changed since it was listed" },
		{ .name = "prepared record past leaf end refused",
		    .fixture_type = 1,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_RECORD,
		    .value = UINT16_MAX,
		    .width = 2,
		    .followup = PREPARED_ENTER_FOLDER,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "HFS Plus record location lies outside its leaf" },
		{ .name = "prepared file location names another file",
		    .fixture_type = 2,
		    .mutation = PREPARED_LOCATION,
		    .target_id = 34,
		    .target_type = 2,
		    .followup = PREPARED_READ_FILE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "HFS Plus file record changed since it was listed" },
		{ .name = "prepared file size disagrees with record",
		    .fixture_type = 2,
		    .mutation = PREPARED_FIELD,
		    .offset = PREPARED_SIZE,
		    .value = 2,
		    .width = 8,
		    .followup = PREPARED_READ_FILE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "HFS Plus file record changed since it was listed" },
		{ .name = "prepared file location names a folder",
		    .fixture_type = 2,
		    .mutation = PREPARED_LOCATION,
		    .target_id = 35,
		    .target_type = 1,
		    .followup = PREPARED_READ_FILE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "HFS Plus file record changed since it was listed" },
		{ .name = "prepared invalid UTF-8 name refused",
		    .fixture_type = 2,
		    .mutation = PREPARED_BAD_NAME,
		    .followup = PREPARED_ADVANCE,
		    .probe = PREPARED_NAME_PROBE,
		    .expected = FSIMG_ECORRUPT,
		    .diagnostic = "invalid prepared directory record" }
	};
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	for (size_t scenario = 0; scenario < sizeof(cases) / sizeof(cases[0]);
	    scenario++) {
		unsigned before_failures = failures;
		fixture_t f;
		fsimg_reader_t *reader;
		const fsimg_entry_t *entry;
		fsimg_store_t *store;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;
		unsigned char payload[PREPARED_BYTES], byte;
		uint64_t cursor;
		size_t count;
		const char *name = cases[scenario].name;
		const char *message = cases[scenario].diagnostic;
		bool folder = cases[scenario].fixture_type == 1;

		objects[1].type = cases[scenario].fixture_type;
		objects[1].data_size = folder ? 0 : 1;
		gen_build(&plan, &image);
		expect_listing(name, &image, FSIMG_DONE, NULL);
		reader = public_reader(&f, &image);
		CHECK(reader != NULL);
		CHECK(fsimg_reader_next(reader, &entry, &error) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/a") == 0);
		fsimg_test_walk_cursor(reader, &store, &cursor);
		CHECK(store != NULL);
		/* 8-byte frame, class, one-byte name and NUL precede payload. */
		CHECK(fsimg_store_read(store, cursor + 11, payload,
		    sizeof(payload), &error) == FSIMG_OK);
		switch (cases[scenario].mutation) {
		case PREPARED_SYMLINK_FIELD:
			payload[PREPARED_KIND] = FSIMG_ENTRY_SYMLINK;
			/* FALLTHROUGH */
		case PREPARED_FIELD:
			store_payload(payload, cases[scenario].offset,
			    cases[scenario].value, cases[scenario].width);
			break;
		case PREPARED_NODE_COUNT:
			store_payload(payload, PREPARED_LEAF,
			    image.trees[GEN_CATALOG].nodes, 4);
			break;
		case PREPARED_LOCATION:
			payload_location(&image, payload, cases[scenario].target_id,
			    cases[scenario].target_type);
			break;
		case PREPARED_BAD_NAME:
			/* The shared walker validates UTF-8 before driver replay. */
			byte = 0xff;
			CHECK(fsimg_store_write(store, cursor + 9, &byte, 1, &error) == FSIMG_OK);
			break;
		case PREPARED_UNCHANGED:
			break;
		}
		CHECK(fsimg_store_write(store, cursor + 11, payload,
		    sizeof(payload), &error) == FSIMG_OK);
		if (cases[scenario].probe == PREPARED_LENGTH_PROBE) {
			static const struct {
				const char *name;
				size_t length;
			} lengths[] = {
				{ "driver replay rejects 59-byte payload", PREPARED_BYTES - 1 },
				{ "driver replay rejects empty payload", 0 },
				{ "driver replay rejects 61-byte payload", PREPARED_BYTES + 1 }
			};
			unsigned char extended[PREPARED_BYTES + 1] = { 0 };

			bcopy(payload, extended, sizeof(payload));
			for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
				status = reader->ops->replay(reader, "b", extended,
				    lengths[i].length, &error);
				expect_status(lengths[i].name, status, FSIMG_ECORRUPT, &error);
				expect_diagnostic(lengths[i].name, &error,
				    "invalid prepared HFS Plus entry payload");
			}
			status = reader->ops->replay(reader, "b", payload,
			    sizeof(payload), &error);
			expect_status("driver replay accepts 60-byte payload", status,
			    FSIMG_OK, &error);
		}
		if (cases[scenario].probe == PREPARED_NAME_PROBE) {
			fsimg_directory_keys_t keys;
			char scratch_name[2] = { 0 };

			CHECK(fsimg_store_read(store, cursor + 9, scratch_name, 1,
			    &error) == FSIMG_OK);
			status = reader->ops->keys(reader, scratch_name, payload,
			    sizeof(payload), &keys, &error);
			expect_status("scratch name cannot produce HFS Plus keys", status,
			    FSIMG_ECORRUPT, &error);
			expect_diagnostic("scratch name cannot produce HFS Plus keys", &error,
			    "invalid HFS Plus file name");
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
		if (cases[scenario].followup != PREPARED_ADVANCE) {
			expect_status(name, status, FSIMG_OK, &error);
			if (status == FSIMG_OK) {
				if (cases[scenario].followup == PREPARED_ENTER_FOLDER)
					status = fsimg_reader_next(reader, &entry, &error);
				else
					status = fsimg_reader_read_at(reader, 0, &byte, 1, &count, &error);
			}
		}
		expect_status(name, status, cases[scenario].expected, &error);
		if (message != NULL)
			expect_diagnostic(name, &error, message);
		else {
			expect_true(name, strcmp(entry->path, "/b") == 0);
			expect_status(name, fsimg_reader_read_at(
			    reader, 0, &byte, 1, &count, &error), FSIMG_OK, &error);
			expect_true(name, count == 1 && byte == 0);
		}
		fsimg_reader_free(reader);
		fixture_close(&f);
		if (failures != before_failures)
			fprintf(stderr, "prepared case failed: %s\n", name);
		gen_free(&image);
	}
}

static void
malform_file_record(gen_record_t *records, size_t count)
{
	gen_record_t *r = find_record(records, count, 2, 2);

	r->length -= 2;
}

static void
malform_folder_record(gen_record_t *records, size_t count)
{
	gen_record_t *r = find_record(records, count, 2, 1);

	r->length -= 2;
}

static void
empty_catalog_record(gen_record_t *records, size_t count)
{
	gen_record_t *r = find_record(records, count, 2, 2);

	r->length = r->key_length;
}

static void
malform_child_key(gen_record_t *records, size_t count)
{
	gen_record_t *r = find_record(records, count, 2, 2);

	/* Keep framing intact; the embedded unit count disagrees with keyLength. */
	gen_store(r->bytes + 6, 2, 2);
}

static void
test_corrupt_catalog_records_pin_diagnostics(void)
{
	static const struct {
		const char *name;
		uint16_t type;
		void (*mutate)(gen_record_t *, size_t);
		const char *message;
	} rows[] = {
		{ "malformed file record", 2, malform_file_record,
		    "HFS Plus file record is malformed" },
		{ "malformed folder record", 1, malform_folder_record,
		    "HFS Plus folder record is malformed" },
		{ "catalog record without a type", 2, empty_catalog_record,
		    "HFS Plus catalog record is too short for its type" },
		{ "malformed child key in directory scan", 2, malform_child_key,
		    "HFS Plus key is malformed" }
	};

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		gen_object_t object = { .name = name_a, .units = 1, .id = 32,
		    .type = rows[i].type };
		gen_plan_t plan;

		gen_plan_init(&plan);
		plan.objects = &object;
		plan.object_count = 1;
		list_plan("catalog corruption baseline", &plan, FSIMG_DONE, NULL);
		plan.mutate_catalog = rows[i].mutate;
		list_plan(rows[i].name, &plan, FSIMG_ECORRUPT, rows[i].message);
	}
}

static void
test_attribute_scan_refuses_a_malformed_key(void)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .flags = 4 };
	gen_attr_value_t values[] = {
		{ .id = 32, .name = name_a, .units = 1, .type = 0x10 },
		{ .id = 32, .name = name_b, .units = 1, .type = 0x10 }
	};
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	plan.attributes = true;
	plan.values = values;
	plan.value_count = 2;
	gen_build(&plan, &image);
	expect_listing("attribute key baseline", &image, FSIMG_DONE, NULL);
	gen_store(image.bytes + image.trees[GEN_ATTRIBUTES].records[1].offset + 12,
	    2, 2);
	expect_listing("malformed attribute key during scan", &image,
	    FSIMG_ECORRUPT, "HFS Plus attribute key is malformed");
	gen_free(&image);
}

static void
test_thread_lookup_refuses_a_malformed_key_after_following_a_leaf(void)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2 },
		{ .name = name_c, .units = 1, .id = 34, .type = 2 }
	};
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	hfsplus_cursor_t cursor;
	hfsplus_thread_t thread;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = 3;
	plan.records_per_node = 2;
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 0);
	CHECK(fixture_open(&f, -1, false, &error) == FSIMG_OK);
	fsimg_hfsplus_reader_t *state = f.reader.driver;
	hfsplus_tree_t *tree = &state->catalog;
	gen_record_t *r = find_record(image.trees[GEN_CATALOG].records,
	    image.trees[GEN_CATALOG].record_count, 33, 4);

	/* Start the search in the preceding leaf, so its lower bound follows
	 * fLink. The next leaf's first key is decoded by thread lookup itself. */
	tree->header.tree_depth = 1;
	tree->header.root_node = (uint32_t)((r->offset -
	    image.trees[GEN_CATALOG].offset) / tree->header.node_size) - 1;
	hfsplus_cursor_init(&cursor, tree, tree->search_node);
	status = hfsplus_thread_lookup(&cursor, 33, 4, &thread, &error);
	expect_status("thread lookup across leaf baseline", status, FSIMG_OK, &error);
	gen_store(image.bytes + r->offset + 6, 1, 2);
	hfsplus_cursor_init(&cursor, tree, tree->search_node);
	status = hfsplus_thread_lookup(&cursor, 33, 4, &thread, &error);
	expect_status("malformed thread key after leaf transition", status,
	    FSIMG_ECORRUPT, &error);
	expect_diagnostic("malformed thread key after leaf transition", &error,
	    "HFS Plus key is malformed");
	fixture_close(&f);
	gen_free(&image);
}

/* Run each refusal at root and beneath /b, rebuilding its ordinary child. */
static void
test_child_refusals_name_the_whole_path(void)
{
	enum {
		CHILD_COMPRESSION_FORK,
		CHILD_UNKNOWN_ATTRIBUTE,
		CHILD_EMPTY_SYMLINK,
		CHILD_LONG_SYMLINK,
		CHILD_EXPANDED_TIMES,
		CHILD_UNKNOWN_FLAG,
		CHILD_DIRECTORY_LINK,
		CHILD_COMPRESSED,
		CHILD_ZERO_STUB,
		CHILD_UNCONVERTIBLE
	};
	static const uint16_t cmp_name[] = { 'c', 'o', 'm', '.', 'a', 'p', 'p',
	    'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };
	static const uint16_t surrogate[] = { 0xd800 };
	static const unsigned char cmp[] = { 'f', 'p', 'm', 'c', 13, 0, 0, 0,
	    1, 0, 0, 0, 0, 0, 0, 0 };
	static const struct {
		const char *name;
		fsimg_status_t status;
		const char *message;
	} rows[] = {
		[CHILD_COMPRESSION_FORK] = { "compression attribute in fork", FSIMG_EUNSUPPORTED,
		    "HFS Plus compression attribute is not stored inline" },
		[CHILD_UNKNOWN_ATTRIBUTE] = { "unknown attribute type names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus attribute record has an unknown type" },
		[CHILD_EMPTY_SYMLINK] = { "empty symlink names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus symbolic link's target length is not 1 to 1023 bytes" },
		[CHILD_LONG_SYMLINK] = { "oversized symlink names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus symbolic link's target length is not 1 to 1023 bytes" },
		[CHILD_EXPANDED_TIMES] = { "expanded timestamps name child", FSIMG_EUNSUPPORTED,
		    "HFS Plus catalog record has expanded timestamps" },
		[CHILD_UNKNOWN_FLAG] = { "unknown flag names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus catalog record has an unknown flag" },
		[CHILD_DIRECTORY_LINK] = { "directory hard link names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus directory hard link is not supported" },
		[CHILD_COMPRESSED] = { "compressed file names child", FSIMG_EUNSUPPORTED,
		    "HFS Plus compression type is not supported" },
		[CHILD_ZERO_STUB] = { "zero stub reference names child", FSIMG_ECORRUPT,
		    "HFS Plus hard-link stub has a zero reference" },
		[CHILD_UNCONVERTIBLE] = { "unconvertible child stays unnamed", FSIMG_ECORRUPT,
		    "HFS Plus name has an unpaired surrogate" }
	};

	for (unsigned nested = 0; nested < 2; nested++) {
		for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
			gen_object_t objects[] = {
				{ .name = name_a, .units = 1, .id = 32, .type = 2 },
				{ .name = name_b, .units = 1, .id = 33, .type = 1 }
			};
			gen_attr_value_t attr = { .id = 32, .name = cmp_name,
			    .units = sizeof(cmp_name) / sizeof(cmp_name[0]),
			    .type = 0x10, .value = cmp, .size = sizeof(cmp) };
			gen_plan_t plan;
			listing_t listed;
			char scenario[100];
			const char *path = nested ? "/b/a" : "/a";

			gen_plan_init(&plan);
			plan.objects = objects;
			plan.object_count = nested ? 2 : 1;
			objects[0].parent = nested ? 33 : 0;
			switch (i) {
			case CHILD_COMPRESSION_FORK:
			case CHILD_UNKNOWN_ATTRIBUTE:
			case CHILD_COMPRESSED:
				plan.attributes = true;
				plan.values = &attr;
				plan.value_count = 1;
				objects[0].flags = 4;
				objects[0].owner_flags = i == CHILD_UNKNOWN_ATTRIBUTE ? 0 : 0x20;
				if (i == CHILD_COMPRESSION_FORK)
					attr.type = 0x20;
				else if (i == CHILD_UNKNOWN_ATTRIBUTE)
					attr.type = 0x40;
				break;
			case CHILD_EMPTY_SYMLINK:
			case CHILD_LONG_SYMLINK:
				objects[0].mode = 0120777;
				objects[0].data_size = i == CHILD_EMPTY_SYMLINK ? 0 : 1024;
				break;
			case CHILD_EXPANDED_TIMES:
				objects[0].flags = 0x1000;
				break;
			case CHILD_UNKNOWN_FLAG:
				objects[0].flags = 0x8000;
				break;
			case CHILD_DIRECTORY_LINK:
				bcopy("fdrpMACS", objects[0].finder, 8);
				objects[0].flags = 0x20;
				break;
			case CHILD_ZERO_STUB:
				bcopy("hlnkhfs+", objects[0].finder, 8);
				break;
			case CHILD_UNCONVERTIBLE:
				objects[0].name = surrogate;
				break;
			}
			snprintf(scenario, sizeof(scenario), "%s: %s", path, rows[i].name);
			listed = list_plan(scenario, &plan, rows[i].status, rows[i].message);
			if (i == CHILD_UNCONVERTIBLE)
				path = "";
			expect_true(scenario, strcmp(listed.error.path, path) == 0);
			expect_true("refused directory delivers no children",
			    listed.count == nested);
		}
	}
}

static void
journal_file_nonregular(gen_record_t *records, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		gen_record_t *r = &records[i];

		if (gen_load(r->bytes + 2, 4) == 2 &&
		    gen_load(r->bytes + 6, 2) == 8) {
			gen_store(r->bytes + r->key_length + 42, 0010644, 2);
			return;
		}
	}
	CHECK(false);
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
	expect_true("format-owned journal info remains unnamed", listed.error.path[0] == '\0');
	gen_free(&image);
	gen_plan_init(&plan);
	plan.journal = true;
	plan.journal_file = true;
	plan.mutate_catalog = journal_file_nonregular;
	listed = list_plan("journal must be regular file", &plan, FSIMG_ECORRUPT,
	    "HFS Plus journal file is not a regular file");
	expect_true("format-owned journal remains unnamed", listed.error.path[0] == '\0');
}

/*
 * This layout is what HFS_WRAPPER_VOLUME_BYTES, HFS_WRAPPER_OFFSET,
 * HFS_WRAPPER_PAYLOAD_OFFSET and HFS_WRAPPER_PREFIX_BYTES in
 * test/cli/hfsplus_truncated_test.rb depend on.
 */
static void
emit_wrapped_seed(const char *path)
{
	gen_image_t image, envelope = { 0 };
	gen_plan_t plan;
	unsigned char content[65537];
	gen_object_t object;
	static const uint16_t name[] = { 'a' };
	FILE *file;
	uint64_t start;
	unsigned char *fork;

	gen_plan_init(&plan);
	plan.verification = true;
	for (size_t i = 0; i < sizeof(content); i++)
		content[i] = (unsigned char)(i % 251);
	object = (gen_object_t){ .name = name, .units = 1,
	    .id = 32, .type = 2, .mode = 0100644, .data = content,
	    .data_size = sizeof(content) };
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	/* Keep metadata before the cut and place /a at a known tail
	 * offset for the CLI's truncated extraction fixture.
	 */
	start = (uint64_t)(plan.total_blocks - 32) * plan.block_size;
	fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork + FORK_START, start / plan.block_size, 4);
	bcopy(content, image.bytes + start, sizeof(content));
	gen_rebuild_bitmap(&image);
	bcopy(image.bytes + VOLUME_HEADER,
	    image.bytes + image.length - 1024, 512);
	envelope.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &envelope.length);
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(envelope.bytes, 1, envelope.length, file) ==
	    envelope.length);
	CHECK(fclose(file) == 0);
	free(envelope.bytes);
	gen_free(&image);
}

/* Runtime corpus fixtures include an indexed directory, overflow extents,
 * empty content and, for HFSX, two case-distinct root names.
 */
static void
emit_seed(const char *path, bool hfsx)
{
	static const uint16_t directory[] = { 'd', 'i', 'r' };
	static const uint16_t empty[] = { 'e', 'm', 'p', 't', 'y' };
	static const uint16_t upper[] = { 'A' }, lower[] = { 'a' };
	unsigned char content[65537];
	gen_object_t objects[36];
	compression_case_t compressed[4];
	gen_attr_value_t attributes[4];
	uint16_t names[32][3];
	gen_plan_t plan;
	gen_image_t image;
	FILE *file;

	bzero(objects, sizeof(objects));
	for (size_t i = 0; i < sizeof(content); i++)
		content[i] = (unsigned char)(i * 7 + i / 251);
	objects[0] = (gen_object_t){ .name = directory, .units = 3,
	    .id = 16, .type = 1, .mode = 0040755 };
	objects[1] = (gen_object_t){ .name = empty, .units = 5,
	    .id = 17, .type = 2, .mode = 0100644 };
	for (size_t i = 0; i < 32; i++) {
		names[i][0] = 'f';
		names[i][1] = '0' + i / 10;
		names[i][2] = '0' + i % 10;
		objects[i + 2] = (gen_object_t){ .name = names[i], .units = 3,
		    .parent = 16, .id = 18 + i, .type = 2, .mode = 0100644,
		    .data = content, .data_size = i == 0 ? sizeof(content) : 33,
		    .fragmented = i == 0 };
	}
	objects[34] = (gen_object_t){ .name = upper, .units = 1,
	    .id = 50, .type = 2, .mode = 0100644 };
	objects[35] = (gen_object_t){ .name = lower, .units = 1,
	    .id = 51, .type = 2, .mode = 0100644 };
	gen_plan_init(&plan);
	plan.hfsx = hfsx;
	plan.compare_type = hfsx ? 0xbc : 0xcf;
	plan.records_per_node = 4;
	plan.objects = objects;
	plan.object_count = hfsx ? 36 : 34;
	for (size_t i = 0; i < 4; i++) {
		if (i < 2)
			compression_vector_init(&compressed[i], i == 0 ? 8 : 12,
			    &lzfse_vectors[i == 0 ? 1 : 2], false, false);
		else
			compression_case_init(&compressed[i], i == 2 ? 3 : 4,
			    i == 2 ? 65536 : 3 * 65536, i == 2 ? 0 : 4);
		objects[i + 2] = compressed[i].objects[0];
		objects[i + 2].name = names[i];
		objects[i + 2].units = 3;
		objects[i + 2].parent = 16;
		objects[i + 2].id = 18 + i;
		attributes[i] = compressed[i].attributes[0];
		attributes[i].id = 18 + i;
	}
	plan.values = attributes;
	plan.value_count = 4;
	plan.attributes = true;
	gen_build(&plan, &image);
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(image.bytes, 1, image.length, file) == image.length);
	CHECK(fclose(file) == 0);
	gen_free(&image);
	for (size_t i = 0; i < 4; i++)
		compression_case_free(&compressed[i]);
}

static void
check_truncated_volume(const gen_image_t *image, size_t length,
    const unsigned char *payload, size_t size)
{
	gen_image_t slice = *image;
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_stats_t before, after;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char bytes[64];
	size_t count = 0;
	char missing[32], scenario[96];

	CHECK(size <= sizeof(bytes) && length < image->length);
	slice.length = length;
	snprintf(scenario, sizeof(scenario), "truncated tail at %zu bytes", length);
	expect_status(scenario, image_probe(&slice), FSIMG_OK, NULL);
	fixture_init(&f, &slice, 0, 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error);
	expect_status(scenario, status, FSIMG_OK, &error);
	if (status != FSIMG_OK)
		goto done;
	expect_status("truncated tail info", fsimg_reader_get_info(reader,
	    &info, &error), FSIMG_OK, &error);
	expect_true(scenario, info.volume_bytes == image->length &&
	    info.missing_bytes == image->length - length);

	/* Verification must refuse the missing tail before driver work, leaving
	 * this reader untouched and usable for listing and content.
	 */
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	status = fsimg_verify(reader, 0, &report, &error);
	expect_status(scenario, status, FSIMG_ECORRUPT,
	    &error);
	expect_true("verification names truncation",
	    strstr(error.message, "truncated") != NULL);
	snprintf(missing, sizeof(missing), "%" PRIu64,
	    (uint64_t)image->length - length);
	expect_true("verification names missing byte count",
	    strstr(error.message, missing) != NULL);
	CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
	expect_true("truncation verification performs no I/O",
	    after.io_calls == before.io_calls && !report.complete);

	status = fsimg_reader_next(reader, &entry, &error);
	expect_status("truncated tail lists surviving file", status,
	    FSIMG_OK, &error);
	if (status == FSIMG_OK) {
		expect_true("surviving file name kind and size",
		    strcmp(entry->path, "/a") == 0 &&
		    entry->kind == FSIMG_ENTRY_FILE && entry->size == size);
		status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes),
		    &count, &error);
		expect_status("truncated tail reads surviving file", status,
		    FSIMG_OK, &error);
		expect_true("surviving file exact bytes", count == size &&
		    bcmp(bytes, payload, size) == 0);
		expect_status("truncated tail listing complete",
		    fsimg_reader_next(reader, &entry, &error), FSIMG_DONE, &error);
	}
done:
	fsimg_reader_free(reader);
	fixture_close(&f);
}

static void
test_truncated_volume(void)
{
	static const unsigned char payload[] = "surviving file bytes";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };
	unsigned char *extended;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	check_truncated_volume(&image, image.length / 2, payload,
	    sizeof(payload) - 1);
	check_truncated_volume(&image, image.length / 2 + 100, payload,
	    sizeof(payload) - 1);

	/* An unaligned slice reaching all allocated blocks has no missing tail
	 * to explain its partial sector.
	 */
	extended = realloc(image.bytes, image.length + 1);
	CHECK(extended != NULL);
	image.bytes = extended;
	image.bytes[image.length++] = 0;
	fixture_init(&f, &image, 0, 0);
	expect_status("unaligned complete allocation", fixture_open(&f, -1,
	    false, &error), FSIMG_ECORRUPT, &error);
	expect_true("unaligned complete allocation names partial sector",
	    strstr(error.message, "not whole sectors") != NULL);
	fixture_close(&f);
	gen_free(&image);
}

static void
check_truncated_file_listed(const gen_image_t *slice, size_t size)
{
	fixture_t f;
	fsimg_reader_t *reader = public_reader(&f, slice);
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("file past cut is listed", status, FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			expect_true("file past cut name and size",
			    strcmp(entry->path, "/a") == 0 && entry->size == size);
			expect_status("missing payload does not stop listing",
			    fsimg_reader_next(reader, &entry, &error),
			    FSIMG_DONE, &error);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
}

static void
check_truncated_file_read(const gen_image_t *slice)
{
	fixture_t f;
	fsimg_reader_t *reader = public_reader(&f, slice);
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char byte;
	size_t count;

	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("select file past cut for reading", status,
		    FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			status = fsimg_reader_read_at(reader, 0, &byte, 1,
			    &count, &error);
			expect_status("read file past cut", status,
			    FSIMG_ECORRUPT, &error);
			expect_true("read file past cut names truncation",
			    strstr(error.message, "truncated") != NULL);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
}

static void
check_truncated_file_extract(const gen_image_t *slice)
{
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_extract_options_t options;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	char temporary[] = "hfsplus-truncated-XXXXXX";
	int parent, root;

	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	reader = public_reader(&f, slice);
	if (reader != NULL) {
		fsimg_extract_options_init(&options);
		status = fsimg_extract_at(reader, parent, "out", &options, &error);
		expect_status("extract file past cut", status, FSIMG_ECORRUPT,
		    &error);
		expect_true("extract file past cut names truncation",
		    strstr(error.message, "truncated") != NULL);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	root = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root != -1) {
		CHECK(unlinkat(root, "a", 0) == 0 || errno == ENOENT);
		CHECK(close(root) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
	} else
		CHECK(errno == ENOENT);
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
}

static void
test_truncated_unused_tail_extract(void)
{
	static const unsigned char payload[] = "surviving extracted bytes";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image, slice;
	fixture_t f;
	fsimg_reader_t *reader;
	fsimg_extract_options_t options;
	fsimg_error_t error = { 0 };
	unsigned char bytes[sizeof(payload)];
	char temporary[] = "hfsplus-surviving-XXXXXX";
	size_t count = 0;
	int parent, root, fd;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	/* The generator places payload and metadata before this unused tail. */
	slice = image;
	slice.length /= 2;
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	reader = public_reader(&f, &slice);
	if (reader != NULL) {
		fsimg_extract_options_init(&options);
		expect_status("extract with only unused tail missing",
		    fsimg_extract_at(reader, parent, "out", &options, &error),
		    FSIMG_OK, &error);
		root = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(root != -1);
		fd = openat(root, "a", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(fd != -1);
		while (count < sizeof(bytes)) {
			ssize_t got = read(fd, bytes + count, sizeof(bytes) - count);

			CHECK(got >= 0);
			if (got == 0)
				break;
			count += (size_t)got;
		}
		expect_true("truncated volume extracts exact surviving bytes",
		    count == sizeof(payload) - 1 &&
		    bcmp(bytes, payload, sizeof(payload) - 1) == 0);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(root, "a", 0) == 0);
		CHECK(close(root) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	gen_free(&image);
}

static void
test_truncated_file(void)
{
	static const unsigned char payload[] = "file beyond the cut";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image, slice;
	unsigned char *fork;
	uint32_t block;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	/* Move the file into the unused tail, beyond all tree nodes. */
	block = plan.total_blocks - 4;
	fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork + FORK_START, block, 4);
	bcopy(payload, image.bytes + (uint64_t)block * plan.block_size,
	    sizeof(payload) - 1);
	slice = image;
	slice.length = (size_t)(block - 1) * plan.block_size;
	check_truncated_file_listed(&slice, sizeof(payload) - 1);
	check_truncated_file_read(&slice);
	check_truncated_file_extract(&slice);
	gen_free(&image);
}

static void
test_truncated_metadata(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_error_t error = { 0 };

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	image.length = gen_node_offset(&image, GEN_CATALOG, 0) +
	    image.trees[GEN_CATALOG].node_size / 2;
	fixture_init(&f, &image, 0, 0);
	expect_status("cut through catalog header node", fixture_open(&f, -1,
	    false, &error), FSIMG_ECORRUPT, &error);
	expect_true("catalog header node cut names truncation",
	    strstr(error.message, "truncated") != NULL);
	fixture_close(&f);
	gen_free(&image);
}

/* Run the same public operations for both resource measurements. */
static fsimg_stats_t
check_surviving_file(const char *scenario, const gen_image_t *slice,
    uint64_t volume_bytes, const unsigned char *payload, size_t size)
{
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_volume_info_t info;
	fsimg_stats_t stats;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char bytes[64];
	size_t count = 0;

	CHECK(size <= sizeof(bytes));
	fixture_init(&f, slice, 0, 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &reader, &error);
	expect_status(scenario, status, FSIMG_OK, &error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_reader_get_info(reader, &info, &error);
	expect_status(scenario, status, FSIMG_OK, &error);
	if (status == FSIMG_OK)
		expect_true(scenario, info.volume_bytes == volume_bytes &&
		    info.missing_bytes == volume_bytes - slice->length);
	status = fsimg_reader_next(reader, &entry, &error);
	expect_status(scenario, status, FSIMG_OK, &error);
	if (status != FSIMG_OK)
		goto done;
	expect_true(scenario, strcmp(entry->path, "/a") == 0 &&
	    entry->kind == FSIMG_ENTRY_FILE && entry->size == size);
	status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes), &count,
	    &error);
	expect_status(scenario, status, FSIMG_OK, &error);
	if (status != FSIMG_OK)
		goto done;
	expect_true(scenario, count == size && bcmp(bytes, payload, size) == 0);
	expect_status(scenario, fsimg_reader_next(reader, &entry, &error),
	    FSIMG_DONE, &error);
done:
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fixture_close(&f);
	return (stats);
}

static void
test_truncated_last_block(void)
{
	static const unsigned char payload[] = "last allocation block";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image, slice;
	fixture_t f;
	fsimg_error_t error = { 0 };
	unsigned char *fork;
	uint32_t block;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	CHECK(plan.block_size == 4096);
	block = plan.total_blocks - 1;
	fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork + FORK_START, block, 4);
	bcopy(payload, image.bytes + (uint64_t)block * plan.block_size,
	    sizeof(payload) - 1);
	slice = image;
	slice.length = (size_t)block * plan.block_size + 2048;
	(void)check_surviving_file("truncated last block holds surviving file",
	    &slice, (uint64_t)plan.total_blocks * plan.block_size, payload,
	    sizeof(payload) - 1);

	/* The startup fork is validated at open without reading its payload. */
	fork = image.bytes + VOLUME_HEADER + GEN_STARTUP_FORK;
	gen_store(fork, sizeof(payload) - 1, 8);
	gen_store(fork + FORK_BLOCKS, 1, 4);
	gen_store(fork + FORK_START, block, 4);
	gen_store(fork + FORK_COUNT, 1, 4);
	fixture_init(&f, &slice, 0, 0);
	expect_status("truncated last block holds special fork",
	    fixture_open(&f, -1, false, &error), FSIMG_OK, &error);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_truncated_largest_volume(void)
{
	static const unsigned char payload[] = "small stored file";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image, slice;
	fsimg_stats_t intact, truncated;
	uint64_t product;
	uint32_t used;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	intact = check_surviving_file("intact resource baseline", &image,
	    image.length, payload, sizeof(payload) - 1);
	used = plan.total_blocks -
	    (uint32_t)gen_load(image.bytes + VOLUME_FREE_BLOCKS, 4);
	gen_store(image.bytes + VOLUME_TOTAL_BLOCKS, UINT32_MAX, 4);
	gen_store(image.bytes + VOLUME_FREE_BLOCKS, UINT32_MAX - used, 4);
	slice = image;
	slice.length /= 2;
	product = (uint64_t)UINT32_MAX * plan.block_size;
	truncated = check_surviving_file("largest block count surviving file",
	    &slice, product, payload, sizeof(payload) - 1);
	expect_true("declared volume size does not increase memory or scratch",
	    truncated.memory_peak == intact.memory_peak &&
	    truncated.scratch_peak == intact.scratch_peak);

	gen_free(&image);
}

static void
test_truncated_largest_block_size(void)
{
	gen_plan_t plan;
	gen_image_t image, slice;
	fixture_t f;
	fsimg_stats_t intact, truncated;
	fsimg_error_t error = { 0 };

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 10000);
	expect_status("intact block size resource baseline",
	    fixture_open(&f, -1, false, &error), FSIMG_OK, &error);
	CHECK(fsimg_ctx_get_stats(f.ctx, &intact, NULL) == FSIMG_OK);
	fixture_close(&f);

	/* Keep this fixture's own block count and free count. */
	gen_store(image.bytes + VOLUME_BLOCK_SIZE, UINT32_C(0x80000000), 4);
	slice = image;
	slice.length = 4096;
	expect_status("largest block size probe", image_probe(&slice),
	    FSIMG_OK, NULL);
	fixture_init(&f, &slice, 0, 10000);
	expect_status("largest block size tree read exceeds slice",
	    fixture_open(&f, -1, false, &error), FSIMG_ECORRUPT, &error);
	expect_true("largest block size tree read names truncation",
	    strstr(error.message, "truncated") != NULL);
	CHECK(fsimg_ctx_get_stats(f.ctx, &truncated, NULL) == FSIMG_OK);
	expect_true("largest block size stays within intact memory bound",
	    truncated.memory_peak <= intact.memory_peak);
	expect_true("largest block size charges no scratch",
	    truncated.scratch_peak == 0);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_truncated_sibling_loop(void)
{
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	hfsplus_cursor_t cursor;
	fsimg_stats_t intact, truncated;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char *fork, *header;
	uint64_t leaf;
	uint32_t nodes = 1000000;

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	fixture_init(&f, &image, 0, 100000);
	status = fixture_open(&f, -1, false, &error);
	expect_status("intact sibling resource baseline", status, FSIMG_OK,
	    &error);
	if (status == FSIMG_OK) {
		fsimg_hfsplus_reader_t *state = f.reader.driver;
		hfsplus_tree_t *tree = &state->catalog;

		expect_true("intact tree caps leaves at total nodes",
		    tree->max_leaves == tree->header.total_nodes);
	}
	CHECK(fsimg_ctx_get_stats(f.ctx, &intact, NULL) == FSIMG_OK);
	fixture_close(&f);
	image.length /= 2;

	gen_store(image.bytes + VOLUME_TOTAL_BLOCKS, 2 * nodes, 4);
	gen_store(image.bytes + VOLUME_FREE_BLOCKS, 0, 4);
	fork = image.bytes + VOLUME_HEADER + GEN_CATALOG_FORK;
	gen_store(fork, (uint64_t)nodes * plan.block_size, 8);
	gen_store(fork + FORK_BLOCKS, nodes, 4);
	gen_store(fork + FORK_COUNT, nodes, 4);
	header = image.bytes + gen_node_offset(&image, GEN_CATALOG, 0);
	gen_store(header + NODE_DESCRIPTOR + TREE_TOTAL_NODES, nodes, 4);
	leaf = gen_node_offset(&image, GEN_CATALOG,
	    image.trees[GEN_CATALOG].first_leaf);
	gen_store(image.bytes + leaf, image.trees[GEN_CATALOG].first_leaf, 4);
	gen_store(image.bytes + leaf + 4,
	    image.trees[GEN_CATALOG].first_leaf, 4);

	/* More than enough work for one visit per slice node, but not for the
	 * hostile declared node count. No key-order checks hide the visit cap.
	 */
	fixture_init(&f, &image, 0, 100000);
	status = fixture_open(&f, -1, false, &error);
	expect_status("truncated sibling loop opens", status, FSIMG_OK, &error);
	if (status == FSIMG_OK) {
		fsimg_hfsplus_reader_t *state = f.reader.driver;
		hfsplus_tree_t *tree = &state->catalog;

		expect_true("truncated tree caps leaves at slice node count",
		    tree->max_leaves == image.length / tree->header.node_size);
		hfsplus_cursor_init(&cursor, tree, tree->search_node);
		status = hfsplus_cursor_seek(&cursor,
		    image.trees[GEN_CATALOG].first_leaf, 0, &error);
		expect_status("truncated sibling loop seek", status, FSIMG_OK,
		    &error);
		while (status == FSIMG_OK)
			status = hfsplus_cursor_next(&cursor, &error);
		expect_status("truncated sibling loop stops at physical cap", status,
		    FSIMG_ECORRUPT, &error);
		expect_true("truncated sibling loop names looping chain",
		    strstr(error.message, "HFS Plus sibling chain loops") != NULL);
		expect_true("truncated sibling loop bounds entered leaves",
		    cursor.leaves == tree->max_leaves);
	}
	CHECK(fsimg_ctx_get_stats(f.ctx, &truncated, NULL) == FSIMG_OK);
	expect_true("inflated total nodes stays within intact memory bound",
	    truncated.memory_peak <= intact.memory_peak);
	fixture_close(&f);
	gen_free(&image);
}

/* Move one block-sized leaf while keeping the header and index readable. */
static void
relocate_tree_leaf(gen_image_t *image, gen_tree_id_t kind,
    unsigned fork_offset, uint32_t leaf, uint32_t block)
{
	gen_tree_t *tree = &image->trees[kind];
	unsigned char *fork = image->bytes + VOLUME_HEADER + fork_offset;
	uint32_t first = (uint32_t)gen_load(fork + FORK_START, 4);

	CHECK(tree->node_size == image->block_size && leaf > 0 &&
	    leaf < tree->nodes);
	bcopy(image->bytes + gen_node_offset(image, kind, leaf),
	    image->bytes + (uint64_t)block * image->block_size,
	    image->block_size);
	bzero(fork + FORK_START, 64);
	gen_store(fork + FORK_START, first, 4);
	gen_store(fork + FORK_COUNT, leaf, 4);
	gen_store(fork + FORK_START + 8, block, 4);
	gen_store(fork + FORK_COUNT + 8, 1, 4);
	if (leaf + 1 < tree->nodes) {
		gen_store(fork + FORK_START + 16, first + leaf + 1, 4);
		gen_store(fork + FORK_COUNT + 16, tree->nodes - leaf - 1, 4);
	}
}

static void
test_truncated_catalog_leaf(void)
{
	gen_child_t child = { .name = name_a, .units = 1, .id = 32 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	gen_plan_init(&plan);
	plan.children = &child;
	plan.child_count = 1;
	plan.records_per_node = 2;
	gen_build(&plan, &image);
	/* Leaf one holds the root and its thread; leaf two holds the child. */
	relocate_tree_leaf(&image, GEN_CATALOG, GEN_CATALOG_FORK, 2,
	    plan.total_blocks - 4);
	image.length /= 2;
	reader = public_reader(&f, &image);
	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("catalog child leaf past cut fails listing", status,
		    FSIMG_ECORRUPT, &error);
		expect_true("catalog child leaf past cut names truncation",
		    strstr(error.message, "truncated") != NULL);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
}

static void
check_truncated_open(const char *scenario, const gen_image_t *image)
{
	fixture_t f;
	fsimg_error_t error = { 0 };

	fixture_init(&f, image, 0, 0);
	expect_status(scenario, fixture_open(&f, -1, false, &error),
	    FSIMG_ECORRUPT, &error);
	expect_true(scenario, strstr(error.message, "truncated") != NULL);
	fixture_close(&f);
}

static void
test_truncated_attributes_header(void)
{
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.attributes = true;
	gen_build(&plan, &image);
	image.length = gen_node_offset(&image, GEN_ATTRIBUTES, 0);
	check_truncated_open("attributes header past cut fails open", &image);
	gen_free(&image);
}

static void
test_truncated_journal_info(void)
{
	gen_plan_t plan;
	gen_image_t image;

	gen_plan_init(&plan);
	plan.journal = true;
	gen_build(&plan, &image);
	bcopy(image.bytes + image.journal_info,
	    image.bytes + (uint64_t)(plan.total_blocks - 4) * plan.block_size,
	    plan.block_size);
	gen_store(image.bytes + VOLUME_JIB, plan.total_blocks - 4, 4);
	image.length /= 2;
	check_truncated_open("journal info block past cut fails open", &image);
	gen_free(&image);
}

static void
test_truncated_journal_header(void)
{
	gen_plan_t plan;
	gen_image_t image;
	unsigned char *fork;
	uint32_t block;

	gen_plan_init(&plan);
	plan.journal = true;
	gen_build(&plan, &image);
	block = plan.total_blocks - (uint32_t)(plan.journal_size /
	    plan.block_size) - 2;
	bcopy(image.bytes + image.journal_header,
	    image.bytes + (uint64_t)block * plan.block_size,
	    (size_t)plan.journal_size);
	fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork + FORK_START, block, 4);
	gen_store(image.bytes + image.journal_info + JIB_OFFSET,
	    (uint64_t)block * plan.block_size, 8);
	image.length /= 2;
	check_truncated_open("journal header past cut fails open", &image);
	gen_free(&image);
}

static void
test_truncated_overflow_leaf(void)
{
	gen_child_t child = { .name = name_a, .units = 1, .id = 32,
	    .data_extents = 9 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char byte;
	size_t count;

	gen_plan_init(&plan);
	plan.children = &child;
	plan.child_count = 1;
	plan.node_size[GEN_EXTENTS] = plan.block_size;
	gen_build(&plan, &image);
	relocate_tree_leaf(&image, GEN_EXTENTS, GEN_EXTENTS_FORK,
	    image.trees[GEN_EXTENTS].first_leaf, plan.total_blocks - 4);
	image.length /= 2;
	reader = public_reader(&f, &image);
	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("file with missing overflow leaf is listed", status,
		    FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			/* Fork resolution is deferred until content is requested. */
			status = fsimg_reader_read_at(reader,
			    (uint64_t)8 * plan.block_size, &byte, 1, &count, &error);
			expect_status("overflow extent leaf past cut fails read",
			    status, FSIMG_ECORRUPT, &error);
			expect_true("overflow extent leaf past cut names truncation",
			    strstr(error.message, "truncated") != NULL);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_truncated_file_straddles_cut(void)
{
	unsigned char payload[4096], bytes[32];
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	size_t count;
	uint32_t block;

	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char)i;
	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	block = plan.total_blocks - 4;
	gen_store(catalog_data(&image, 2, 2) + FILE_DATA_FORK + FORK_START,
	    block, 4);
	bcopy(payload, image.bytes + (uint64_t)block * plan.block_size,
	    sizeof(payload));
	image.length = (size_t)block * plan.block_size + 2048;
	reader = public_reader(&f, &image);
	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("straddling file is listed", status, FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			status = fsimg_reader_read_at(reader, 2016, bytes,
			    sizeof(bytes), &count, &error);
			expect_status("straddling file before cut reads", status,
			    FSIMG_OK, &error);
			expect_true("straddling file before cut exact bytes",
			    count == sizeof(bytes) &&
			    bcmp(bytes, payload + 2016, sizeof(bytes)) == 0);
			status = fsimg_reader_read_at(reader, 2048, bytes, 1,
			    &count, &error);
			expect_status("straddling file after cut fails", status,
			    FSIMG_ECORRUPT, &error);
			expect_true("straddling file after cut names truncation",
			    strstr(error.message, "truncated") != NULL);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_truncated_file_output_limit(void)
{
	static const unsigned char payload[] = "stored prefix";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image;
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char *fork;
	uint64_t size;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	/* The file can fill the volume, so the stored slice must be under
	 * 1/16 of it for the file to exceed the default output limit. */
	image.length /= 2 * FSIMG_DEFAULT_OUTPUT_MULTIPLE;
	size = FSIMG_DEFAULT_OUTPUT_MULTIPLE * (uint64_t)image.length +
	    plan.block_size;
	fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork, size, 8);
	gen_store(fork + FORK_BLOCKS, size / plan.block_size, 4);
	gen_store(fork + FORK_COUNT, size / plan.block_size, 4);
	reader = public_reader(&f, &image);
	expect_true("truncated image opens", reader != NULL);
	if (reader != NULL) {
		status = fsimg_reader_next(reader, &entry, &error);
		expect_status("file larger than slice is listed", status,
		    FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			expect_true("file exceeds slice output ceiling",
			    entry->size > reader->session.output_limit &&
			    reader->session.output_limit ==
			    FSIMG_DEFAULT_OUTPUT_MULTIPLE * (uint64_t)image.length);
			status = fsimg_reader_charge_file(reader, &error);
			expect_status("missing last stored byte outranks output limit",
			    status, FSIMG_ECORRUPT, &error);
			expect_true("output charge last byte names truncation",
			    strstr(error.message, "truncated") != NULL);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	gen_free(&image);
}

static void
test_truncated_longer_slice(void)
{
	static const unsigned char payload[] = "complete file";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image;
	unsigned char *extended;

	gen_plan_init(&plan);
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	extended = realloc(image.bytes, image.length + 4096);
	CHECK(extended != NULL);
	image.bytes = extended;
	bzero(image.bytes + image.length, 4096);
	image.length += 4096;
	(void)check_surviving_file("slice longer than allocation geometry",
	    &image, image.length, payload, sizeof(payload) - 1);
	gen_free(&image);
}

/* Every row starts from a real envelope, so only the named field is hostile. */
static void
test_wrapper_refusals(void)
{
	gen_plan_t plan;
	gen_image_t image, wrapped = { 0 };
	const uint64_t embedded = 6144;
	mutation_t cases[] = {
		{ "wrapper zero allocation size", 1044, 0, 4, FSIMG_ECORRUPT,
		    "HFS wrapper's allocation block size is not a multiple of 512" },
		{ "wrapper unaligned allocation size", 1044, 513, 4,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's allocation block size is not a multiple of 512" },
		{ "wrapper empty extent", 1152, 0, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded extent is empty" },
		{ "wrapper extent past outer volume", 1150, 3, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded extent lies past its outer volume" },
		{ "wrapper start at input end", 1052, 4110, 2, FSIMG_ECORRUPT,
		    "the range to read lies past the input: image is truncated" },
		{ "wrapper start past input end", 1052, 4111, 2, FSIMG_ECORRUPT,
		    "the range to read lies past the input: image is truncated" },
		{ "wrapper embedded HFSX", embedded + 1024, 0x48580005, 4,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume is not HFS Plus version 4" },
		{ "wrapper embedded classic HFS", embedded + 1024, 0x4244, 2,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume is not HFS Plus version 4" },
		{ "wrapper embedded wrong version", embedded + 1026, 5, 2,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume is not HFS Plus version 4" },
		{ "wrapper doubled embedded block size", embedded + 1064, 8192, 4,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded range is shorter than the volume's "
		    "block size and count" },
		{ "wrapper embedded count one long", embedded + 1068, 513, 4,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded range is shorter than the volume's "
		    "block size and count" },
		{ "wrapper extent one block short", 1152, 511, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded range is shorter than the volume's "
		    "block size and count" }
	};

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &wrapped.length);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		mutate_open(&wrapped, &cases[i]);

	/* These rows require several hostile fields to reach their guard. */
	typedef struct wrapper_fields {
		mutation_t mutation;
		uint32_t size;
		uint16_t blocks;
		uint16_t sector;
		uint16_t start;
	} wrapper_fields_t;
	const wrapper_fields_t compound[] = {
		{ .mutation = { "wrapper overlaps MDB", 1150, 0, 2,
		    FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume overlaps its master directory block" },
		    .size = 4096, .blocks = 514, .sector = 0, .start = 1 },
		{ .mutation = { "wrapper offset 512 overlaps MDB", 1150,
		    0, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume overlaps its master directory block" },
		    .size = 4096, .blocks = 514, .sector = 1, .start = 0 },
		{ .mutation = { "wrapper offset 1024 overlaps MDB", 1150,
		    0, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded volume overlaps its master directory block" },
		    .size = 4096, .blocks = 514, .sector = 2, .start = 0 },
		{ .mutation = { "wrapper 64-bit offset and length products",
		    1152, 2, 2, FSIMG_ECORRUPT,
		    "the range to read lies past the input: image is truncated" },
		    .size = UINT32_C(1) << 31, .blocks = 4,
		    .sector = 4, .start = 2 },
		{ .mutation = { "wrapper maximal extent fields", 1152,
		    UINT16_MAX, 2, FSIMG_ECORRUPT,
		    "HFS wrapper's embedded extent lies past its outer volume" },
		    .size = UINT32_MAX - 511, .blocks = UINT16_MAX - 1,
		    .sector = UINT16_MAX, .start = UINT16_MAX }
	};

	for (size_t i = 0; i < sizeof(compound) / sizeof(compound[0]); i++) {
		gen_store(wrapped.bytes + 1044, compound[i].size, 4);
		gen_store(wrapped.bytes + 1042, compound[i].blocks, 2);
		gen_store(wrapped.bytes + 1052, compound[i].sector, 2);
		gen_store(wrapped.bytes + 1150, compound[i].start, 2);
		mutate_open(&wrapped, &compound[i].mutation);
	}
	free(wrapped.bytes);
	gen_free(&image);
}

static void
test_wrapper_journals(void)
{
	typedef struct journal_case {
		const char *name;
		uint32_t flags;
		bool pending;
		bool image_base;
		bool mismatch;
		fsimg_status_t status;
		const char *message;
	} journal_case_t;
	static const journal_case_t cases[] = {
		{ "wrapper initialized clean embedded base", 1, false, false,
		    false, FSIMG_EUNSUPPORTED,
		    "HFS Plus journal inside a wrapper has no established location" },
		{ "wrapper initialized pending embedded base", 1, true, false,
		    false, FSIMG_EUNSUPPORTED,
		    "HFS Plus journal inside a wrapper has no established location" },
		{ "wrapper initialized clean image base", 1, false, true,
		    false, FSIMG_EUNSUPPORTED,
		    "HFS Plus journal inside a wrapper has no established location" },
		{ "wrapper initialized pending image base", 1, true, true,
		    false, FSIMG_EUNSUPPORTED,
		    "HFS Plus journal inside a wrapper has no established location" },
		{ "wrapper uninitialized embedded base", 5, false, false,
		    false, FSIMG_OK, NULL },
		{ "wrapper uninitialized wrong location", 5, false, false,
		    true, FSIMG_ECORRUPT, "disagrees with the journal info block" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		gen_plan_t plan;
		gen_image_t image, wrapped = { 0 };
		fixture_t f;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;

		gen_plan_init(&plan);
		plan.journal = true;
		plan.journal_flags = cases[i].flags;
		plan.journal_end = cases[i].pending ? 1024 : 512;
		gen_build(&plan, &image);
		if (cases[i].image_base || cases[i].mismatch)
			gen_store(image.bytes + image.journal_info + JIB_OFFSET,
			    image.journal_header + (cases[i].image_base ? 6144 :
			    4096), 8);
		wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
		    &wrapped.length);
		fixture_init(&f, &wrapped, 0, 0);
		status = fixture_open(&f, -1, false, &error);
		expect_status(cases[i].name, status, cases[i].status, &error);
		if (cases[i].message != NULL)
			expect_true(cases[i].name,
			    strstr(error.message, cases[i].message) != NULL);
		fixture_close(&f);
		free(wrapped.bytes);
		gen_free(&image);
	}
}

/* Public listing and extraction must see the embedded file, even when only
 * unused embedded bytes or the outer tail are missing.
 */
static void
check_wrapper_contents(const char *name, const gen_image_t *wrapped,
    uint64_t embedded, uint64_t reported_volume, uint64_t missing)
{
	static const unsigned char payload[] = "wrapper file bytes";
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	const fsimg_entry_t *entry;
	fsimg_volume_info_t info;
	fsimg_error_t error = { 0 };
	fsimg_verify_report_t report;
	unsigned char bytes[64];
	size_t count;
	char number[96];

	fixture_init(&f, wrapped, 0, 0);
	expect_status(name, fsimg_reader_open(f.ctx, f.io, NULL, &reader,
	    &error), FSIMG_OK, &error);
	if (reader != NULL) {
		expect_status(name,
		    fsimg_reader_get_info(reader, &info, &error),
		    FSIMG_OK, &error);
		expect_true("wrapper profile", strcmp(info.profile,
		    "HFS Plus in HFS wrapper") == 0);
		if (info.volume_bytes != reported_volume ||
		    info.missing_bytes != missing)
			fprintf(stderr, "%s: volume_bytes=%" PRIu64
			    " expected=%" PRIu64 ", missing_bytes=%" PRIu64
			    " expected=%" PRIu64 "\n", name, info.volume_bytes,
			    reported_volume, info.missing_bytes, missing);
		expect_true(name, info.volume_bytes == reported_volume &&
		    info.missing_bytes == missing);
		expect_true(name, info.detail_count > 0);
		if (info.detail_count > 0) {
			const fsimg_volume_detail_t *last =
			    &info.details[info.detail_count - 1];

			expect_true("wrapper offset is the last detail",
			    strcmp(last->name, "wrapper_offset") == 0 &&
			    last->value == embedded);
		}
		/* The outer volume is unchecked; only embedded loss fails. */
		expect_status(name, fsimg_verify(reader, 0, &report, &error),
		    missing == 0 ? FSIMG_OK : FSIMG_ECORRUPT, &error);
		if (missing != 0) {
			snprintf(number, sizeof(number),
			    "image is truncated: %" PRIu64 " declared bytes are missing",
			    missing);
			expect_true("wrapper truncation diagnostic and count",
			    strstr(error.message, number) != NULL);
		}
		/* Verification consumes iteration. Reopen for listing. */
		fsimg_reader_free(reader);
		reader = NULL;
		fixture_close(&f);
		fixture_init(&f, wrapped, 0, 0);
		expect_status(name, fsimg_reader_open(f.ctx, f.io, NULL,
		    &reader, &error), FSIMG_OK, &error);
	}
	if (reader != NULL) {
		fsimg_status_t status;

		status = fsimg_reader_next(reader, &entry, &error);
		expect_status(name, status, FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			expect_true(name, strcmp(entry->path, "/a") == 0 &&
			    entry->kind == FSIMG_ENTRY_FILE &&
			    entry->size == sizeof(payload) - 1);
			expect_status(name, fsimg_reader_read_at(reader, 0,
			    bytes, sizeof(bytes), &count, &error),
			    FSIMG_OK, &error);
			expect_true(name, count == sizeof(payload) - 1 &&
			    bcmp(bytes, payload, count) == 0);
			expect_status(name,
			    fsimg_reader_next(reader, &entry, &error),
			    FSIMG_DONE, &error);
		}
	}
	fsimg_reader_free(reader);
	fixture_close(&f);

	char temporary[] = "hfsplus-wrapper-XXXXXX";
	fsimg_extract_options_t options;
	int parent, root, fd;

	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	fixture_init(&f, wrapped, 0, 0);
	expect_status(name, fsimg_reader_open(f.ctx, f.io, NULL, &reader,
	    &error), FSIMG_OK, &error);
	if (reader != NULL) {
		fsimg_extract_options_init(&options);
		expect_status(name, fsimg_extract_at(reader, parent, "out",
		    &options, &error), FSIMG_OK, &error);
		root = openat(parent, "out",
		    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(root != -1);
		fd = openat(root, "a", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(fd != -1);
		ssize_t got = read(fd, bytes, sizeof(bytes));
		expect_true(name, got == sizeof(payload) - 1 &&
		    bcmp(bytes, payload, sizeof(payload) - 1) == 0);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(root, "a", 0) == 0);
		CHECK(close(root) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
}

/* The range, not N * B, locates the alternate header. gen_build emits it
 * there and accounts only for reserved tail blocks inside the allocation.
 */
static void
test_wrapper_longer_ranges(void)
{
	typedef struct longer_range {
		const char *name;
		uint32_t blocks;
		uint32_t outer_block_size;
		size_t extra_bytes;
		uint64_t kept;
		uint64_t missing;
	} longer_range_t;
	static const longer_range_t cases[] = {
		{ .name = "wrapper embedded count one short", .blocks = 511,
		    .outer_block_size = 4096, .extra_bytes = 4096 },
		{ .name = "wrapper extent one block long", .blocks = 512,
		    .outer_block_size = 4096, .extra_bytes = 4096 },
		{ .name = "wrapper range exceeds volume by 512 bytes",
		    .blocks = 512, .outer_block_size = 512,
		    .extra_bytes = 512 },
		{ .name = "wrapper range exceeds volume by four blocks",
		    .blocks = 512, .outer_block_size = 4096,
		    .extra_bytes = 4 * 4096 },
		{ .name = "wrapper cut between allocation and range ends",
		    .blocks = 512, .outer_block_size = 4096,
		    /* N * B + 512 of a range of N * B + 4096. */
		    .extra_bytes = 4096, .kept = 2097664, .missing = 3584 },
		{ .name = "wrapper cut at an unaligned length",
		    .blocks = 512, .outer_block_size = 4096,
		    /* N * B + 1 of a range of N * B + 4096. */
		    .extra_bytes = 4096, .kept = 2097153, .missing = 4095 }
	};
	static const unsigned char payload[] = "wrapper file bytes";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		gen_plan_t plan;
		gen_image_t image, wrapped = { 0 };
		uint64_t embedded = 2048 + cases[i].outer_block_size;

		gen_plan_init(&plan);
		plan.total_blocks = cases[i].blocks;
		plan.extra_bytes = cases[i].extra_bytes;
		plan.verification = true;
		plan.objects = &object;
		plan.object_count = 1;
		gen_build(&plan, &image);
		fixture_t baseline;
		fsimg_error_t error = { 0 };
		fsimg_verify_report_t report;
		fsimg_reader_t *reader = public_reader(&baseline, &image);

		if (reader != NULL)
			expect_status(cases[i].name,
			    fsimg_verify(reader, 0, &report, &error), FSIMG_OK,
			    &error);
		fsimg_reader_free(reader);
		fixture_close(&baseline);
		expect_true(cases[i].name,
		    bcmp(image.bytes + 1024,
		    image.bytes + image.length - 1024, 512) == 0);
		wrapped.bytes = gen_wrap(&image, cases[i].outer_block_size,
		    4, 1, 1, 1024, &wrapped.length);
		expect_status(cases[i].name, image_open(&wrapped, -1, false),
		    FSIMG_OK, NULL);
		uint64_t range_bytes = (uint64_t)plan.total_blocks *
		    plan.block_size + plan.extra_bytes;
		/* A row with no kept value keeps the whole image. */
		if (cases[i].kept != 0)
			wrapped.length = embedded + cases[i].kept;
		check_wrapper_contents(cases[i].name, &wrapped, embedded,
		    range_bytes, cases[i].missing);
		free(wrapped.bytes);
		gen_free(&image);
	}
}

static void
test_wrapper_cut_keeps_reserved_tail(void)
{
	gen_plan_t plan;
	gen_image_t image, wrapped = { 0 };
	fixture_t fixture;
	fsimg_error_t error = { 0 };
	const char *names[] = {
		"wrapper intact fork reaches reserved tail",
		"wrapper cut fork reaches reserved tail"
	};

	gen_plan_init(&plan);
	gen_build(&plan, &image);
	/* The startup fork is checked at open without reading its bytes. */
	unsigned char *fork = image.bytes + VOLUME_HEADER + GEN_STARTUP_FORK;
	gen_store(fork, plan.block_size, 8);
	gen_store(fork + FORK_BLOCKS, 1, 4);
	gen_store(fork + FORK_START, plan.total_blocks - 1, 4);
	gen_store(fork + FORK_COUNT, 1, 4);
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &wrapped.length);
	for (size_t i = 0; i < 2; i++) {
		if (i == 1)
			wrapped.length = 6144 + image.length / 2;
		fixture_init(&fixture, &wrapped, 0, 0);
		fsimg_status_t status = fixture_open(&fixture, -1, false,
		    &error);
		expect_status(names[i], status, FSIMG_ECORRUPT, &error);
		expect_true(names[i], strstr(error.message,
		    "extent lies outside the content domain") != NULL);
		fixture_close(&fixture);
	}
	free(wrapped.bytes);
	gen_free(&image);
}

static void
test_wrappers(void)
{
	typedef struct wrapper_position {
		const char *name;
		uint32_t size;
		uint16_t before;
		uint16_t after;
		size_t slack;
		uint16_t first_sector;
	} wrapper_position_t;
	static const wrapper_position_t positions[] = {
		{ "wrapper at offset 1536 after MDB", 4096, 0, 2, 1024, 3 },
		{ "wrapper at allocation start", 4096, 0, 2, 1024, 4 },
		{ "wrapper in allocation middle", 4096, 1, 1, 1024, 4 },
		{ "wrapper at allocation end", 4096, 2, 0, 1024, 4 },
		{ "wrapper allocation size 73728", 73728, 1, 1, 1024, 4 },
		{ "wrapper allocation size 512", 512, 1, 1, 1024, 4 },
		{ "wrapper mkfs slack 4096", 4096, 1, 1, 4096, 4 },
		{ "wrapper Blender slack 2048", 4096, 1, 1, 2048, 4 }
	};
	static const unsigned char payload[] = "wrapper file bytes";
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data = payload, .data_size = sizeof(payload) - 1 };
	gen_plan_t plan;
	gen_image_t image, wrapped = { 0 };

	gen_plan_init(&plan);
	/* 540 * 4096 is also exactly 30 * 73728. */
	plan.total_blocks = 540;
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, &image);
	wrapped.bytes = gen_wrap(&image, 512, 4, 1, 1, 1024,
	    &wrapped.length);
	/* The master directory block's embedded extent count: one 512-byte block. */
	gen_store(wrapped.bytes + 1024 + 0x80, 1, 2);
	fixture_t short_range;
	fsimg_error_t short_error = { 0 };
	fixture_init(&short_range, &wrapped, 0, 0);
	expect_status("uncut wrapper with 512-byte embedded range",
	    fixture_open(&short_range, -1, false, &short_error),
	    FSIMG_ECORRUPT, &short_error);
	expect_true("512-byte embedded range names short range, not truncation",
	    strcmp(short_error.message,
	    "HFS wrapper's embedded range is shorter than an HFS Plus header") == 0);
	fixture_close(&short_range);
	free(wrapped.bytes);
	check_surviving_file("unwrapped wrapper baseline", &image,
	    image.length, payload, sizeof(payload) - 1);
	for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
		uint64_t embedded = (uint64_t)positions[i].first_sector * 512 +
		    (uint64_t)positions[i].before * positions[i].size;

		wrapped.bytes = gen_wrap(&image, positions[i].size,
		    positions[i].first_sector,
		    positions[i].before, positions[i].after, positions[i].slack,
		    &wrapped.length);
		expect_true(positions[i].name, bcmp(wrapped.bytes + 1024,
		    wrapped.bytes + wrapped.length - 1024, 512) == 0);
		expect_status(positions[i].name, image_probe(&wrapped), FSIMG_OK, NULL);
		expect_status(positions[i].name, image_open(&wrapped, -1, false),
		    FSIMG_OK, NULL);
		check_wrapper_contents(positions[i].name, &wrapped, embedded,
		    (uint64_t)plan.total_blocks * plan.block_size, 0);
		expect_status("real wrapper forced hfsx", image_open(&wrapped,
		    1, false), FSIMG_EFORMAT, NULL);
		free(wrapped.bytes);
	}
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 4096,
	    &wrapped.length);
	const uint64_t embedded = 6144;
	const uint64_t reported_volume =
	    (uint64_t)plan.total_blocks * plan.block_size;
	typedef struct wrapper_cut {
		const char *name;
		uint64_t kept;
		uint64_t missing;
	} wrapper_cut_t;
	const wrapper_cut_t cuts[] = {
		{ "wrapper truncated embedded tail", reported_volume / 2,
		    reported_volume / 2 },
		{ "wrapper cut one byte before embedded end",
		    reported_volume - 1, 1 },
		{ "wrapper cut at embedded end", reported_volume, 0 },
		{ "wrapper truncated outer tail", reported_volume + 512, 0 },
		{ "wrapper cut at outer allocation end",
		    reported_volume + 4096, 0 },
		{ "wrapper cut in mkfs slack", reported_volume + 4096 + 1536,
		    0 },
		{ "wrapper cut in alternate MDB",
		    reported_volume + 4096 + 3072 + 256,
		    0 }
	};
	for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
		wrapped.length = embedded + cuts[i].kept;
		check_wrapper_contents(cuts[i].name, &wrapped, embedded,
		    reported_volume, cuts[i].missing);
	}
	static const char *open_cuts[] = {
		"wrapper embedded header lost", "wrapper header survives missing trees"
	};
	for (size_t i = 0; i < 2; i++) {
		fixture_t f;
		fsimg_error_t error = { 0 };

		wrapped.length = embedded + (i == 0 ? 1024 : 1536);
		fixture_init(&f, &wrapped, 0, 0);
		expect_status(open_cuts[i], fixture_open(&f, -1, false, &error),
		    FSIMG_ECORRUPT, &error);
		expect_true(open_cuts[i],
		    strstr(error.message, "image is truncated") != NULL);
		fixture_close(&f);
	}
	fixture_t before_embed;
	fsimg_error_t before_error = { 0 };
	wrapped.length = embedded - 512;
	fixture_init(&before_embed, &wrapped, 0, 0);
	expect_status("wrapper slice ends before embedded start",
	    fixture_open(&before_embed, -1, false, &before_error),
	    FSIMG_ECORRUPT, &before_error);
	expect_true("wrapper slice ends before embedded start diagnostic",
	    strstr(before_error.message, "lies past the input") != NULL);
	fixture_close(&before_embed);
	free(wrapped.bytes);

	/* A referenced file beyond the cut must fail through the slice funnel. */
	unsigned char *fork = catalog_data(&image, 2, 2) + FILE_DATA_FORK;
	gen_store(fork + FORK_START, plan.total_blocks - 4, 4);
	wrapped.bytes = gen_wrap(&image, 4096, 4, 1, 1, 1024,
	    &wrapped.length);
	wrapped.length = 6144 + image.length / 2;
	check_truncated_file_listed(&wrapped, sizeof(payload) - 1);
	check_truncated_file_read(&wrapped);
	check_truncated_file_extract(&wrapped);
	free(wrapped.bytes);
	gen_free(&image);
}

/*
 * Calling through a table keeps the compiler from inlining every test into
 * main and summing their stack frames.
 */
static void (*const tests[])(void) = {
	test_narrow_refuses_after_open_and_during_iteration,
	test_narrow_clips_range_and_refuses_second_call,
	test_failed_wrapped_public_open_frees_slice,
	test_wrapped_open_preserves_injected_failures,
	test_wrappers,
	test_wrapper_longer_ranges,
	test_wrapper_cut_keeps_reserved_tail,
	test_wrapper_refusals,
	test_wrapper_journals,
	test_root_fields,
	test_attribute_record_lengths,
	test_distinct_dates,
	test_corrupt_catalog_records_pin_diagnostics,
	test_attribute_scan_refuses_a_malformed_key,
	test_thread_lookup_refuses_a_malformed_key_after_following_a_leaf,
	test_child_refusals_name_the_whole_path,
	test_prepared_entries,
	test_generator_fork_descriptors,
	test_journal_info_identity,
	test_probe_exclusivity,
	test_folder_counts_and_parents,
	test_catalog_exact_lengths,
	test_names_selection_owned,
	test_attribute_key_order,
	test_mixed_round_trip,
	test_catalog_listing,
	test_catalog_flags_kinds,
	test_symbolic_links,
	test_hard_links,
	test_attributes_compression,
	test_compression_content,
	test_compression_codec_content,
	test_compression_codec_corruption,
	test_compression_lzfse_scratch_budget,
	test_compression_listing_failures,
	test_compression_resource_corruption,
	test_compression_resource_map_offsets,
	test_compression_word_table_corruption,
	test_compression_inline_corruption,
	test_compression_counted_reads,
	test_compression_advance_invalidates_window,
	test_compression_cancel_during_decode,
	test_compression_work_consumed_plus_produced,
	test_compression_charge_before_decode,
	test_compression_allocation_failure,
	test_compression_bombs,
	test_compression_empty_extra_data,
	test_compression_releases_held_span,
	test_compression_changed_since_listing,
	test_compression_truncated_charge,
	test_compression_extra_data_and_hard_link,
	test_names_listing,
	test_identity_geometry,
	test_truncated_volume,
	test_truncated_unused_tail_extract,
	test_truncated_file,
	test_truncated_metadata,
	test_truncated_largest_volume,
	test_truncated_largest_block_size,
	test_truncated_last_block,
	test_truncated_sibling_loop,
	test_truncated_catalog_leaf,
	test_truncated_attributes_header,
	test_truncated_journal_info,
	test_truncated_journal_header,
	test_truncated_overflow_leaf,
	test_truncated_file_straddles_cut,
	test_truncated_file_output_limit,
	test_compressed_file_larger_than_slice_reads_at_default_limit,
	test_compressed_file_beyond_default_limit_is_refused,
	test_truncated_longer_slice,
	test_headers,
	test_tree_parameters,
	test_attributes_tree_accepts_binary_compare_pair,
	test_attributes_tree_preserves_code_unit_order,
	test_attributes_tree_allows_fixed_keys_only_when_empty,
	test_catalog_and_extents_keep_tree_parameters,
	test_tree_search,
	test_attribute_search,
	test_empty_tree_search,
	test_catalog_key_owns_thread_name,
	test_content_domain_empty,
	test_tree_faults,
	test_node_framing,
	test_record_key_bounds,
	test_forks,
	test_journals,
	test_journal_size_limits,
	test_journal_location_clauses,
	test_labels_info,
	test_faults,
	test_native,
};

int
main(int argc, char **argv)
{
	if (argc == 3 && (strcmp(argv[1], "--seed") == 0 ||
	    strcmp(argv[1], "--seed-hfsx") == 0 ||
	    strcmp(argv[1], "--seed-wrap") == 0)) {
		if (strcmp(argv[1], "--seed-wrap") == 0)
			emit_wrapped_seed(argv[2]);
		else
			emit_seed(argv[2], strcmp(argv[1], "--seed-hfsx") == 0);
		return (failures == 0 ? 0 : 1);
	}
	if (argc == 3 && strcmp(argv[1], "--native-content") == 0)
		return (native_content(argv[2]));
	CHECK(argc == 1);
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		tests[i]();
	printf("HFS Plus: %u assertions, %u failures\n", assertions, failures);
	return (failures == 0 ? 0 : 1);
}
