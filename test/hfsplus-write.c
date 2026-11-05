/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "format.h"
#include "fsimg-endian.h"
#include "test.h"
#include "write-hfsplus.h"
#include "hfsplus-write-check.h"

/* Planning has no output authority. Nodes are checked as they pass through
 * a memory callback; even the map-boundary cases retain no volume image.
 */
static char scenario[192];
static unsigned failures;

typedef struct fixture fixture_t;
typedef struct tree_check tree_check_t;
typedef struct node_capture node_capture_t;

static void add_entry(fixture_t *f, const char *path, unsigned kind,
    uint64_t length, const fsimg_metadata_t *metadata);
static void add_link(fixture_t *f);
static uint16_t be16(const unsigned char *p);
static uint32_t be32(const unsigned char *p);
static uint64_t brute_blocks(uint32_t b, uint64_t free_bytes, uint64_t objects);
static fsimg_status_t capture_node(void *arg, uint32_t number,
    const unsigned char *bytes, fsimg_error_t *error);
static unsigned char *capture_tree(fixture_t *f);
static uint64_t ceil_div(uint64_t n, uint64_t d);
static void check_geometry(hfsplus_plan_t *p, uint64_t n, uint32_t b,
    uint32_t tail, uint64_t objects);
static void check_header(tree_check_t *c, const unsigned char *raw);
static void check_key_order(hfsplus_plan_t *p, unsigned entries);
static void check_map(const unsigned char *map, uint64_t first, size_t bytes,
    uint32_t used);
static void check_stored_names(hfsplus_plan_t *p);
static void collision_case(unsigned variant, const char *first,
    const char *second, fsimg_status_t expected);
static void compare_streams(fsimg_stream_t *a, fsimg_stream_t *b);
static void context_open(fixture_t *f);
static size_t data_length(const hfsplus_write_record_t *r);
static void emit_checked(fsimg_session_t *session, hfsplus_tree_shape_t *shape,
    fsimg_stream_t *records, fsimg_stream_t **levels);
static void expect_setting(fsimg_write_options_t opts);
static void fail(const char *condition, int line);
static void fixture_close(fixture_t *f);
static void fixture_open(fixture_t *f, unsigned variant,
    const fsimg_write_options_t *opts);
static fsimg_status_t inspect_node(void *argument, uint32_t number,
    const unsigned char *raw, fsimg_error_t *error);
static fsimg_status_t leaf_pattern(void *argument,
    const hfsplus_write_record_t *r, unsigned char *bytes,
    fsimg_error_t *error);
static uint32_t level_first(const hfsplus_tree_shape_t *s, unsigned level);
static void name_case(unsigned variant, const char *path,
    fsimg_status_t expected, bool label);
static void named(const char *fmt, ...);
static fsimg_status_t no_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error);
static uint32_t nodes_without_maps(uint32_t leaves);
static fsimg_write_options_t options_for(uint32_t block);
static void pack_case(uint32_t files, unsigned final_units,
    uint32_t expected_leaves, unsigned expected_depth, uint32_t expected_maps,
    bool all_blocks);
static hfsplus_plan_t *plan_ok(fixture_t *f);
static fsimg_status_t plan_status(fixture_t *f, fsimg_error_t *error);
static fsimg_status_t provider_read(void *arg, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *done, fsimg_error_t *error);
static fsimg_status_t provider_validate(void *arg, uint64_t id,
    fsimg_error_t *error);
static uint64_t small_demand(uint32_t b, uint64_t objects);
static void test_bitmap_boundaries(void);
static void test_determinism(void);
static void test_key_order_and_mapping(void);
static void test_metadata_policy(void);
static void test_minimization(void);
static void test_names(void);
static void test_packing(void);
static void test_rows_and_counts(void);
static void test_settings_and_limits(void);
static void test_tree_clump_low_32_bits(void);
static size_t utf8_unit(char *out, uint16_t unit);
static bool zero_bytes(const unsigned char *p, size_t n);

#define EXPECT(c) do { if (!(c)) fail(#c, __LINE__); } while (0)
#define REQUIRE(c) do { if (!(c)) { fail(#c, __LINE__); exit(1); } } while (0)

#define VARIANT(n, v) { .id = (v), .info = { .name = n, \
	.entry_kinds = FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY | \
	    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO, \
	.metadata_fields = FSIMG_META_CREATED | FSIMG_META_MODIFIED | \
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED | FSIMG_META_MODE | \
	    FSIMG_META_OWNER, .sector_size_mask = 1u << 9, \
	.allocation_unit_size_mask = 0x1fe00u, \
	.write_allocation_unit_size_mask = 0x1fe00u, \
	.max_file_bytes = UINT64_MAX, .max_volume_bytes = UINT64_MAX, \
	.max_name_utf16_units = 255, .max_label_bytes = 1024, \
	.max_link_target_bytes = 1024, .write_settings = FSIMG_SETTING_VOLUME_UUID } }

static const fsimg_format_variant_t variants[] = {
	VARIANT("hfsplus", HFSPLUS_VARIANT_PLUS),
	VARIANT("hfsx", HFSPLUS_VARIANT_X)
};
static const fsimg_writer_ops_t operations = {
	hfsplus_write_validate, hfsplus_write_plan, no_emit, hfsplus_write_free
};
static const fsimg_format_t format = {
	.variants = variants, .variant_count = 2, .writer = &operations
};

struct fixture {
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
};

static void
fail(const char *condition, int line)
{
	failures++;
	fprintf(stderr, "%s: line %d: %s\n", scenario, line, condition);
}

static void
named(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(scenario, sizeof(scenario), fmt, ap);
	va_end(ap);
}

static uint64_t
ceil_div(uint64_t n, uint64_t d)
{
	return (n / d + (n % d != 0));
}

static fsimg_status_t
no_emit(fsimg_writer_t *writer FSIMG_UNUSED, fsimg_emit_t *emit FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_EUNSUPPORTED);
}

static fsimg_write_options_t
options_for(uint32_t block)
{
	fsimg_write_options_t options;

	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_ALLOW_LOSS;
	options.epoch = 0;
	options.label = "untitled";
	options.allocation_unit_bytes = block;
	return (options);
}

static void
context_open(fixture_t *f)
{
	fsimg_options_t options;

	bzero(f, sizeof(*f));
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(256) * 1024 * 1024;
	options.scratch_directory_fd = -1;
	REQUIRE(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
}

static void
fixture_open(fixture_t *f, unsigned variant, const fsimg_write_options_t *opts)
{
	fsimg_error_t error;
	fsimg_status_t status;

	context_open(f);
	status = fsimg_writer_new_variant(f->ctx, &format, &variants[variant],
	    opts, &f->writer, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: construct: %d %s\n", scenario, status,
		    error.message);
	REQUIRE(status == FSIMG_OK);
}

static void
fixture_close(fixture_t *f)
{
	fsimg_writer_free(f->writer);
	fsimg_ctx_free(f->ctx);
}

static void
add_entry(fixture_t *f, const char *path, unsigned kind, uint64_t length,
    const fsimg_metadata_t *metadata)
{
	fsimg_entry_t entry = { .path = path, .kind = kind, .size = length };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };

	if (metadata != NULL)
		entry.metadata = *metadata;
	REQUIRE(fsimg_writer_add(f->writer, &entry,
	    kind == FSIMG_ENTRY_FILE || kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, NULL) == FSIMG_OK);
}

static fsimg_status_t
plan_status(fixture_t *f, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;

	return (fsimg_writer_plan(f->writer, &plan, error));
}

static hfsplus_plan_t *
plan_ok(fixture_t *f)
{
	fsimg_error_t error;
	fsimg_status_t status = plan_status(f, &error);

	if (status != FSIMG_OK)
		fprintf(stderr, "%s: plan: %d %s (%s)\n", scenario, status,
		    error.message, error.path);
	REQUIRE(status == FSIMG_OK);
	return ((hfsplus_plan_t *)f->writer->plan);
}

/* Enumerate every N, rather than using the planner's fixed-point search.
 * Empty and small catalogs have one leaf plus the header, so their three
 * trees occupy 4096, 8192 and 8192 logical bytes independently of B.
 */
static uint64_t
small_demand(uint32_t b, uint64_t objects)
{
	return (ceil_div(1536, b) + ceil_div(4096, b) +
	    ceil_div(8192, b) * 2 + objects);
}

static uint64_t
brute_blocks(uint32_t b, uint64_t free_bytes, uint64_t objects)
{
	uint64_t demand = small_demand(b, objects) + ceil_div(free_bytes, b) +
	    ceil_div(1024, b);

	for (uint64_t n = demand; ; n++)
		if (demand + ceil_div(ceil_div(n, 8), b) <= n)
			return (n);
}

static void
check_geometry(hfsplus_plan_t *p, uint64_t n, uint32_t b, uint32_t tail,
    uint64_t objects)
{
	fsimg_plan_info_t info;
	const hfsplus_tree_shape_t *trees[] = {
		&p->extents, &p->attributes, &p->catalog
	};
	uint64_t next = ceil_div(1536, b);
	uint64_t bitmap = ceil_div(ceil_div(n, 8), b);

	EXPECT(p->block_bytes == b);
	EXPECT(p->total_blocks == n);
	EXPECT(p->head_blocks == next);
	EXPECT(p->tail_blocks == tail);
	EXPECT(p->bitmap_blocks == bitmap);
	next += bitmap;
	for (unsigned i = 0; i < 3; i++) {
		const hfsplus_tree_shape_t *s = trees[i];
		uint64_t blocks = ceil_div((uint64_t)s->used_nodes *
		    s->node_bytes, b);

		EXPECT(s->first_block == next);
		EXPECT(s->blocks == blocks);
		EXPECT(s->total_nodes == blocks * b / s->node_bytes);
		next += blocks;
	}
	EXPECT(p->allocated == next + objects);
	REQUIRE(fsimg_plan_get_info(&p->common, &info, NULL) == FSIMG_OK);
	EXPECT(info.volume_bytes == p->volume_bytes);
	EXPECT(info.allocation_units == n);
	EXPECT(info.allocation_unit_bytes == b);
	EXPECT(info.sector_bytes == 512);
	EXPECT(info.allocated_bytes == (p->allocated + tail) * (uint64_t)b);
	EXPECT(info.free_bytes == (n - p->allocated - tail) * (uint64_t)b);
	EXPECT(info.allocated_bytes + info.free_bytes == n * b);
	EXPECT(info.metadata_bytes == (next + tail) * b + p->volume_bytes - n * b);
}

static void
test_bitmap_boundaries(void)
{
	fixture_t f;
	fsimg_write_options_t opts;
	hfsplus_plan_t *p;
	uint64_t target, free_blocks, n;

	for (uint32_t b = 512; b <= 65536; b *= 2) {
		for (unsigned k = 1; k <= 3; k++) {
			for (int delta = -1; delta <= 1; delta++) {
				target = (uint64_t)8 * b * k + delta;
				free_blocks = target - small_demand(b, 0) -
				    ceil_div(1024, b) - k;
				opts = options_for(b);
				opts.min_free_bytes = free_blocks * b - (delta == -1 ? b - 1 : 0);
				n = brute_blocks(b, opts.min_free_bytes, 0);
				named("bitmap B=%u k=%u delta=%d N=%" PRIu64,
				    b, k, delta, n);
				EXPECT(n == target + (delta == 1));
				if (delta == 1) {
					fsimg_write_options_t exact = opts;

					exact.min_free_bytes -= b;
					exact.volume_bytes = target * b;
					fixture_open(&f, 0, &exact);
					check_geometry(plan_ok(&f), target, b,
					    (uint32_t)ceil_div(1024, b), 0);
					fixture_close(&f);
				}
				fixture_open(&f, 0, &opts);
				p = plan_ok(&f);
				check_geometry(p, n, b, (uint32_t)ceil_div(1024, b), 0);
				fixture_close(&f);

				/* The last fitting demand before a bitmap transition
				 * grows N by two when its free-space demand grows one.
				 */
				if (delta == 0) {
					opts.min_free_bytes += b;
					EXPECT(brute_blocks(b, opts.min_free_bytes, 0) == n + 2);
					fixture_open(&f, 0, &opts);
					p = plan_ok(&f);
					check_geometry(p, n + 2, b,
					    (uint32_t)ceil_div(1024, b), 0);
					fixture_close(&f);
					opts.min_free_bytes -= b;
				}
				opts.volume_bytes = n * b;
				fixture_open(&f, 0, &opts);
				check_geometry(plan_ok(&f), n, b,
				    (uint32_t)ceil_div(1024, b), 0);
				fixture_close(&f);
				opts.volume_bytes -= b;
				fixture_open(&f, 0, &opts);
				EXPECT(plan_status(&f, NULL) == FSIMG_ERANGE);
				fixture_close(&f);
				for (uint32_t extra = 512; extra <= 1024; extra *= 2) {
					if (b <= extra)
						continue;
					opts.volume_bytes = n * b + extra;
					fixture_open(&f, 0, &opts);
					check_geometry(plan_ok(&f), n, b,
					    (uint32_t)ceil_div(1024, b), 0);
					fixture_close(&f);
				}
			}
		}
	}
}

static void
expect_setting(fsimg_write_options_t opts)
{
	fixture_t f;
	fsimg_writer_t *writer = NULL;

	context_open(&f);
	EXPECT(fsimg_writer_new_variant(f.ctx, &format, &variants[0], &opts,
	    &writer, NULL) == FSIMG_EINVAL);
	fsimg_writer_free(writer);
	fsimg_ctx_free(f.ctx);
}

static void
test_settings_and_limits(void)
{
	static const uint32_t invalid_blocks[] = { 1, 256, 513, 768, 131072 };
	static const uint32_t invalid_sectors[] = { 1, 256, 1024, 4096 };
	fixture_t f;
	fsimg_write_options_t opts;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(invalid_blocks) / sizeof(*invalid_blocks); i++) {
		named("invalid allocation size %u", invalid_blocks[i]);
		opts = options_for(invalid_blocks[i]);
		expect_setting(opts);
	}
	for (size_t i = 0; i < sizeof(invalid_sectors) / sizeof(*invalid_sectors); i++) {
		named("invalid sector size %u", invalid_sectors[i]);
		opts = options_for(4096);
		opts.sector_bytes = invalid_sectors[i];
		expect_setting(opts);
	}
	named("invalid reproducible epochs");
	opts = options_for(4096);
	opts.epoch = -INT64_C(2082844801);
	expect_setting(opts);
	opts.epoch = INT64_C(2212122496);
	expect_setting(opts);
	named("unaligned exact volume");
	opts = options_for(4096);
	opts.volume_bytes = 65537;
	expect_setting(opts);
	named("exact volume over UINT32_MAX blocks");
	opts = options_for(512);
	opts.volume_bytes = (UINT64_C(1) << 32) * 512;
	expect_setting(opts);
	named("automatic volume over UINT32_MAX blocks");
	opts = options_for(512);
	opts.min_free_bytes = (uint64_t)UINT32_MAX * 512;
	fixture_open(&f, 0, &opts);
	EXPECT(plan_status(&f, NULL) == FSIMG_ERANGE);
	fixture_close(&f);
	named("file over UINT32_MAX blocks at explicit 512");
	opts = options_for(512);
	fixture_open(&f, 0, &opts);
	add_entry(&f, "/huge", FSIMG_ENTRY_FILE, (UINT64_C(1) << 32) * 512, NULL);
	EXPECT(plan_status(&f, &error) == FSIMG_EUNSUPPORTED);
	EXPECT(strcmp(error.path, "/huge") == 0);
	fixture_close(&f);
}

static void
test_minimization(void)
{
	static const uint32_t order[] = { 4096, 512, 1024, 2048, 8192,
		16384, 32768, 65536 };
	fixture_t f;
	fsimg_write_options_t opts;
	hfsplus_plan_t *p;
	uint64_t best, n, volume;
	uint32_t best_block;
	char path[32];

	for (unsigned count = 0; count <= 6; count++) {
		named("minimize %u small files", count);
		best = UINT64_MAX;
		best_block = 0;
		for (unsigned i = 0; i < 8; i++) {
			n = brute_blocks(order[i], 0, count);
			volume = n * order[i];
			if (volume < best) {
				best = volume;
				best_block = order[i];
			}
		}
		opts = options_for(0);
		opts.flags |= FSIMG_WRITE_MINIMIZE;
		fixture_open(&f, 0, &opts);
		for (unsigned i = 0; i < count; i++) {
			snprintf(path, sizeof(path), "/file%u", i);
			add_entry(&f, path, FSIMG_ENTRY_FILE, 1, NULL);
		}
		p = plan_ok(&f);
		EXPECT(p->volume_bytes == best);
		EXPECT(p->block_bytes == best_block);
		check_geometry(p, best / best_block, best_block,
		    (uint32_t)ceil_div(1024, best_block), count);
		fixture_close(&f);
	}
	for (unsigned tie = 0; tie < 2; tie++) {
		uint64_t free_bytes = tie == 0 ? 4170752 : 100633088;
		unsigned tied = 0;

		named("minimize tie %s", tie == 0 ? "increasing sizes" : "4096 first");
		best = UINT64_MAX;
		best_block = 0;
		for (unsigned i = 0; i < 8; i++) {
			volume = brute_blocks(order[i], free_bytes, 0) * order[i];
			if (volume < best) {
				best = volume;
				best_block = order[i];
				tied = 1;
			} else if (volume == best) {
				tied++;
			}
		}
		EXPECT(tied >= 2);
		EXPECT(best_block == (tie == 0 ? 512u : 4096u));
		opts = options_for(0);
		opts.flags |= FSIMG_WRITE_MINIMIZE;
		opts.min_free_bytes = free_bytes;
		fixture_open(&f, 0, &opts);
		p = plan_ok(&f);
		EXPECT(p->volume_bytes == best && p->block_bytes == best_block);
		fixture_close(&f);
	}
	named("minimize file too large for 512-byte blocks");
	opts = options_for(0);
	opts.flags |= FSIMG_WRITE_MINIMIZE;
	fixture_open(&f, 0, &opts);
	add_entry(&f, "/huge", FSIMG_ENTRY_FILE, (UINT64_C(1) << 32) * 512, NULL);
	p = plan_ok(&f);
	EXPECT(p->block_bytes > 512);
	fixture_close(&f);
}

/* A streaming checker compares leaf keys with the input and index keys
 * with first keys observed from the actual emitted children. Only header,
 * map and root nodes are retained. All other nodes are inspected and discarded.
 */
struct tree_check {
	const hfsplus_tree_shape_t *shape;
	fsimg_stream_t *records;
	fsimg_stream_t *first_keys[HFSPLUS_BTREE_DEPTH_MAX];
	uint64_t record_cursor;
	uint64_t level_cursor[HFSPLUS_BTREE_DEPTH_MAX];
	uint32_t level_seen[HFSPLUS_BTREE_DEPTH_MAX];
	uint32_t nodes_seen;
	uint32_t leaves_seen;
	unsigned char header[8192];
	unsigned char maps[2][4096];
	unsigned char root[4096];
};

static uint16_t
be16(const unsigned char *p)
{
	return ((uint16_t)((uint16_t)p[0] << 8 | p[1]));
}

static uint32_t
be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3]);
}

static bool
zero_bytes(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

static uint32_t
level_first(const hfsplus_tree_shape_t *s, unsigned level)
{
	uint32_t first = 1;

	for (unsigned i = 0; i < level; i++)
		first += s->level_nodes[i];
	return (first);
}

static size_t
data_length(const hfsplus_write_record_t *r)
{
	switch (r->type) {
	case HFSPLUS_RECORD_FOLDER:
		return (88);
	case HFSPLUS_RECORD_FILE:
		return (248);
	default:
		return (10 + 2 * r->units);
	}
}

static fsimg_status_t
leaf_pattern(void *argument FSIMG_UNUSED, const hfsplus_write_record_t *r,
    unsigned char *bytes, fsimg_error_t *error FSIMG_UNUSED)
{
	size_t length = data_length(r);

	EXPECT(zero_bytes(bytes, length));
	/* Leave alternating bytes untouched to check the emitter's zero fill. */
	for (size_t i = 0; i < length; i += 2)
		bytes[i] = (unsigned char)(r->id % 251 + 1);
	return (FSIMG_OK);
}

static void
check_map(const unsigned char *map, uint64_t first, size_t bytes,
    uint32_t used)
{
	for (size_t i = 0; i < bytes; i++) {
		unsigned char want = 0;

		for (unsigned bit = 0; bit < 8; bit++)
			if (first + 8 * i + bit < used)
				want |= 0x80u >> bit;
		EXPECT(map[i] == want);
	}
}

static void
check_header(tree_check_t *c, const unsigned char *raw)
{
	const hfsplus_tree_shape_t *s = c->shape;
	hfsplus_btree_t h;
	uint32_t root = s->depth == 0 ? 0 : level_first(s, s->depth - 1);
	uint32_t m = s->node_bytes;

	REQUIRE(hfsplus_btree_decode(raw + 14, 106, &h));
	EXPECT(raw[8] == 1 && raw[9] == 0 && be16(raw + 10) == 3);
	EXPECT(be32(raw) == (s->map_nodes == 0 ? 0 :
	    s->used_nodes - s->map_nodes));
	EXPECT(be32(raw + 4) == 0);
	EXPECT(h.tree_depth == s->depth && h.root_node == root);
	EXPECT(h.leaf_records == s->leaf_records);
	EXPECT(h.first_leaf_node == (s->depth == 0 ? 0 : 1));
	EXPECT(h.last_leaf_node == s->level_nodes[0]);
	EXPECT(h.node_size == m && h.max_key_length == s->max_key_length);
	EXPECT(h.total_nodes == s->total_nodes);
	EXPECT(h.free_nodes == s->total_nodes - s->used_nodes);
	EXPECT(h.clump_size == hfsplus_tree_clump(s));
	EXPECT(h.btree_type == 0 && h.key_compare_type == s->compare_type);
	EXPECT(h.attributes == s->attributes);
	EXPECT(be16(raw + m - 2) == 14);
	EXPECT(be16(raw + m - 4) == 120);
	EXPECT(be16(raw + m - 6) == 248);
	EXPECT(be16(raw + m - 8) == m - 8);
	EXPECT(zero_bytes(raw + 120, 128));
	check_map(raw + 248, 0, m - 256, s->used_nodes);
	bcopy(raw, c->header, m);
}

static fsimg_status_t
inspect_node(void *argument, uint32_t number, const unsigned char *raw,
    fsimg_error_t *error FSIMG_UNUSED)
{
	tree_check_t *c = argument;
	const hfsplus_tree_shape_t *s = c->shape;
	unsigned char encoded[HFSPLUS_WRITE_RECORD_MAX];
	unsigned char level_key[HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_write_record_t record;
	hfsplus_node_t decoded;
	hfsplus_ckey_t key;
	uint32_t m = s->node_bytes, first, last;
	uint16_t count = be16(raw + 10), start, end;
	size_t length;
	unsigned level = 0;

	c->nodes_seen++;
	REQUIRE(number < s->total_nodes);
	if (number >= s->used_nodes) {
		EXPECT(zero_bytes(raw, m));
		return (FSIMG_OK);
	}
	REQUIRE(hfsplus_node_decode(raw, m, &decoded));
	EXPECT(be16(raw + 12) == 0);
	if (number == 0) {
		check_header(c, raw);
		return (FSIMG_OK);
	}
	if (number >= s->used_nodes - s->map_nodes) {
		unsigned map = number - (s->used_nodes - s->map_nodes);

		EXPECT(raw[8] == 2 && raw[9] == 0 && count == 1);
		EXPECT(be32(raw + 4) == 0);
		EXPECT(be32(raw) == (map + 1 < s->map_nodes ? number + 1 : 0));
		EXPECT(be16(raw + m - 2) == 14);
		EXPECT(be16(raw + m - 4) == m - 6);
		EXPECT(zero_bytes(raw + m - 6, 2));
		check_map(raw + 14, 8 * (m - 256) +
		    (uint64_t)map * 8 * (m - 20), m - 20, s->used_nodes);
		if (map < 2)
			bcopy(raw, c->maps[map], m);
		return (FSIMG_OK);
	}
	while (level + 1 < s->depth && number >= level_first(s, level + 1))
		level++;
	first = level_first(s, level);
	last = first + s->level_nodes[level] - 1;
	EXPECT(raw[8] == (level == 0 ? 255 : 0));
	EXPECT(raw[9] == level + 1);
	EXPECT(be32(raw + 4) == (number == first ? 0 : number - 1));
	EXPECT(be32(raw) == (number == last ? 0 : number + 1));
	EXPECT(count != 0);
	EXPECT(be16(raw + m - 2) == 14);
	for (unsigned i = 0; i < count; i++) {
		start = be16(raw + m - 2 * (i + 1));
		end = be16(raw + m - 2 * (i + 2));
		REQUIRE(start < end && end <= m - 2 * (count + 1));
		EXPECT((start & 1) == 0 && (end & 1) == 0);
		REQUIRE(hfsplus_ckey_decode(raw + start, end - start, &key));
		length = 8 + 2 * key.units;
		if (i == 0)
			REQUIRE(fsimg_stream_append(c->first_keys[level], raw + start,
			    length, NULL, NULL) == FSIMG_OK);
		EXPECT(be16(raw + start) == length - 2);
		if (level == 0) {
			REQUIRE(fsimg_stream_next(c->records, &c->record_cursor,
			    encoded, sizeof(encoded), &length, NULL) == FSIMG_OK);
			REQUIRE(hfsplus_write_record_decode(encoded, length, &record));
			EXPECT(key.parent == record.parent);
			EXPECT(key.units == (record.type == HFSPLUS_RECORD_FOLDER ||
			    record.type == HFSPLUS_RECORD_FILE ? record.units : 0));
			EXPECT(bcmp(key.name, record.name, 2 * key.units) == 0);
			length = 8 + 2 * key.units;
			EXPECT(end - start == length + data_length(&record));
			for (size_t j = 0; j < data_length(&record); j++)
				EXPECT(raw[start + length + j] == (j % 2 == 0 ?
				    record.id % 251 + 1 : 0));
			c->leaves_seen++;
		} else {
			REQUIRE(fsimg_stream_next(c->first_keys[level - 1],
			    &c->level_cursor[level - 1], level_key,
			    sizeof(level_key), &length, NULL) == FSIMG_OK);
			EXPECT(key.parent == be32(level_key + 2));
			EXPECT(key.units == be16(level_key + 6));
			EXPECT(bcmp(key.name, level_key + 8, 2 * key.units) == 0);
			length = 8 + 2 * key.units;
			EXPECT(end - start == length + 4);
			EXPECT(be32(raw + start + length) ==
			    level_first(s, level - 1) + c->level_seen[level - 1]);
			c->level_seen[level - 1]++;
		}
	}
	end = be16(raw + m - 2 * (count + 1));
	EXPECT(zero_bytes(raw + end, m - 2 * (count + 1) - end));
	if (number != last) {
		uint64_t cursor = level == 0 ? c->record_cursor :
		    c->level_cursor[level - 1];
		size_t next_bytes;

		/* A non-final node is full precisely when the next complete
		 * record and its extra offset slot cannot fit.
		 */
		if (level == 0) {
			REQUIRE(fsimg_stream_next(c->records, &cursor, encoded,
			    sizeof(encoded), &length, NULL) == FSIMG_OK);
			REQUIRE(hfsplus_write_record_decode(encoded, length, &record));
			next_bytes = 8 + data_length(&record);
			if (record.type == HFSPLUS_RECORD_FILE ||
			    record.type == HFSPLUS_RECORD_FOLDER)
				next_bytes += 2 * record.units;
		} else {
			REQUIRE(fsimg_stream_next(c->first_keys[level - 1], &cursor,
			    level_key, sizeof(level_key), &length, NULL) == FSIMG_OK);
			next_bytes = length + 4;
		}
		EXPECT(end + next_bytes > m - 2 * (count + 2));
	}
	if (number == last)
		REQUIRE(fsimg_stream_seal(c->first_keys[level], NULL) == FSIMG_OK);
	if (level + 1 == s->depth) {
		if (level != 0)
			EXPECT(count == s->level_nodes[level - 1]);
		bcopy(raw, c->root, m);
	}
	return (FSIMG_OK);
}

static void
emit_checked(fsimg_session_t *session, hfsplus_tree_shape_t *shape,
    fsimg_stream_t *records, fsimg_stream_t **levels)
{
	tree_check_t check = { .shape = shape, .records = records };
	unsigned char node[8192];
	hfsplus_tree_emit_t emit = {
		.session = session, .shape = shape, .records = records,
		.levels = levels, .node = node, .leaf_data = leaf_pattern,
		.write = inspect_node, .argument = &check
	};

	for (unsigned i = 0; i < shape->depth; i++)
		REQUIRE(fsimg_stream_new(session, HFSPLUS_WRITE_RECORD_MAX,
		    &check.first_keys[i], NULL) == FSIMG_OK);
	REQUIRE(hfsplus_tree_emit(&emit, NULL) == FSIMG_OK);
	EXPECT(check.nodes_seen == shape->used_nodes);
	EXPECT(check.leaves_seen == shape->leaf_records);
	for (unsigned i = 0; i + 1 < shape->depth; i++)
		EXPECT(check.level_seen[i] == shape->level_nodes[i]);
	for (unsigned i = 0; i < shape->depth; i++)
		fsimg_stream_free(check.first_keys[i]);
}

/* A one-unit file takes 258 bytes plus its two-byte table slot, hence
 * fifteen fit. A one-unit index record and its slot take sixteen bytes,
 * hence 255 fit. These arithmetic facts select shapes without consulting
 * the production packer's result.
 */
static uint32_t
nodes_without_maps(uint32_t leaves)
{
	uint32_t nodes = 1;

	for (;;) {
		nodes += leaves;
		if (leaves == 1)
			return (nodes);
		leaves = (uint32_t)ceil_div(leaves, 255);
	}
}

static void
pack_case(uint32_t files, unsigned final_units, uint32_t expected_leaves,
    unsigned expected_depth, uint32_t expected_maps, bool all_blocks)
{
	fixture_t f;
	fsimg_session_t session;
	fsimg_stream_t *records, *levels[HFSPLUS_BTREE_DEPTH_MAX] = { NULL };
	hfsplus_tree_shape_t shape;
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX], name[510] = { 0 };
	hfsplus_write_record_t r = { .type = HFSPLUS_RECORD_FILE,
		.units = 1, .name = name, .key = name, .key_bytes = 2 };
	size_t length;
	uint32_t used;

	context_open(&f);
	fsimg_session_init(&session, f.ctx, "hfsplus", 0);
	REQUIRE(fsimg_stream_new(&session, sizeof(raw), &records, NULL) == FSIMG_OK);
	for (uint32_t i = 0; i < files + (final_units != 0); i++) {
		r.parent = i + 2;
		r.id = i;
		r.type = i == files ? HFSPLUS_RECORD_FOLDER : HFSPLUS_RECORD_FILE;
		r.units = i == files ? final_units : 1;
		r.key_bytes = 2 * r.units;
		length = hfsplus_write_record_encode(&r, raw);
		REQUIRE(fsimg_stream_append(records, raw, length, NULL, NULL) == FSIMG_OK);
	}
	REQUIRE(fsimg_stream_seal(records, NULL) == FSIMG_OK);
	hfsplus_tree_shape_init(&shape, HFSPLUS_TREE_CATALOG, HFSPLUS_COMPARE_FOLDED);
	EXPECT(shape.node_bytes == 4096 && shape.max_key_length == 516);
	EXPECT(shape.compare_type == 0xcf && shape.attributes == 6);
	REQUIRE(hfsplus_tree_pack(&session, records, &shape, levels, NULL) == FSIMG_OK);
	used = nodes_without_maps(expected_leaves) + expected_maps;
	EXPECT(shape.level_nodes[0] == expected_leaves);
	for (uint32_t count = expected_leaves, level = 0; count != 0; level++) {
		EXPECT(shape.level_nodes[level] == count);
		count = count == 1 ? 0 : (uint32_t)ceil_div(count, 255);
	}
	EXPECT(shape.depth == expected_depth);
	EXPECT(shape.map_nodes == expected_maps);
	EXPECT(shape.used_nodes == used);
	EXPECT(shape.leaf_records == files + (final_units != 0));
	for (uint32_t b = 512; b <= 65536; b *= 2) {
		hfsplus_tree_place(&shape, 7, b);
		EXPECT(shape.first_block == 7);
		EXPECT(shape.blocks == ceil_div((uint64_t)used * 4096, b));
		EXPECT(shape.total_nodes == (uint64_t)shape.blocks * b / 4096);
		if (all_blocks || b == 4096 || b == 65536)
			emit_checked(&session, &shape, records, levels);
	}
	for (unsigned i = 0; i < HFSPLUS_BTREE_DEPTH_MAX; i++)
		fsimg_stream_free(levels[i]);
	fsimg_stream_free(records);
	fsimg_ctx_free(f.ctx);
}

static void
test_tree_clump_low_32_bits(void)
{
	hfsplus_tree_shape_t shape = { .node_bytes = 4096,
		.total_nodes = (1u << 20) - 1 };
	uint32_t clump;

	named("tree clump below 2^32 bytes");
	EXPECT(hfsplus_tree_clump(&shape) == UINT32_C(0xfffff000));

	named("tree clump at 2^32 bytes");
	shape.total_nodes = 1u << 20;
	clump = hfsplus_tree_clump(&shape);
	EXPECT(clump == 0);

	named("tree clump far above 2^32 bytes");
	shape.node_bytes = 8192;
	shape.total_nodes = (1u << 20) + 1000;
	clump = hfsplus_tree_clump(&shape);
	EXPECT(clump == UINT32_C(8192000));
}

static void
test_packing(void)
{
	static const uint32_t boundaries[] = { 30720, 30721, 63327, 63328 };
	fixture_t f;
	fsimg_session_t session;
	hfsplus_tree_shape_t shape;
	uint32_t leaves;

	named("leaf exactly full: 15 files and 41-unit folder");
	pack_case(15, 41, 1, 1, 0, true);
	named("leaf one unit over: last leaf has one record");
	pack_case(15, 42, 2, 2, 0, true);
	named("index exactly full: 255 leaves");
	pack_case(255 * 15, 0, 255, 2, 0, false);
	named("index one over: singleton non-root index, two-record root");
	pack_case(255 * 15 + 1, 0, 256, 3, 0, false);
	for (unsigned i = 0; i < 4; i++) {
		for (leaves = 1; nodes_without_maps(leaves) < boundaries[i]; leaves++)
			;
		REQUIRE(nodes_without_maps(leaves) == boundaries[i]);
		named("map boundary used without maps=%u", boundaries[i]);
		/* A map consumes a bit: 30720+32608-1 nodes leave space
		 * for one map; the next demand needs two map nodes.
		 */
		pack_case((leaves - 1) * 15 + 1, 0, leaves, 3,
		    i == 0 ? 0 : i == 3 ? 2 : 1, false);
	}
	context_open(&f);
	fsimg_session_init(&session, f.ctx, "hfsplus", 0);
	for (unsigned kind = 0; kind < 2; kind++) {
		named("empty %s header", kind == 0 ? "extents" : "attributes");
		hfsplus_tree_shape_init(&shape, kind == 0 ? HFSPLUS_TREE_EXTENTS :
		    HFSPLUS_TREE_ATTRIBUTES, HFSPLUS_COMPARE_FOLDED);
		EXPECT(shape.depth == 0 && shape.used_nodes == 1);
		EXPECT(shape.node_bytes == (kind == 0 ? 4096 : 8192));
		EXPECT(shape.max_key_length == (kind == 0 ? 10 : 266));
		EXPECT(shape.attributes == (kind == 0 ? 2 : 6));
		EXPECT(shape.compare_type == 0);
		for (uint32_t b = 512; b <= 65536; b *= 2) {
			hfsplus_tree_place(&shape, 3, b);
			emit_checked(&session, &shape, NULL, NULL);
		}
	}
	fsimg_ctx_free(f.ctx);
}

static size_t
utf8_unit(char *out, uint16_t unit)
{
	if (unit < 0x80) {
		out[0] = (char)unit;
		return (1);
	}
	if (unit < 0x800) {
		out[0] = (char)(0xc0 | unit >> 6);
		out[1] = (char)(0x80 | (unit & 63));
		return (2);
	}
	out[0] = (char)(0xe0 | unit >> 12);
	out[1] = (char)(0x80 | ((unit >> 6) & 63));
	out[2] = (char)(0x80 | (unit & 63));
	return (3);
}

static void
name_case(unsigned variant, const char *path, fsimg_status_t expected,
    bool label)
{
	fsimg_write_options_t opts = options_for(4096);
	fixture_t f;
	fsimg_error_t error;
	fsimg_status_t status;
	bool invalid_utf8 = strcmp(path, "/bad\xff") == 0 ||
	    strcmp(path, "/\xed\xa0\x80") == 0;

	if (label)
		opts.label = path + 1;
	context_open(&f);
	status = fsimg_writer_new_variant(f.ctx, &format, &variants[variant],
	    &opts, &f.writer, &error);
	if (status != FSIMG_OK) {
		EXPECT(status == expected && label);
		fixture_close(&f);
		return;
	}
	if (!label) {
		fsimg_entry_t entry = { .path = path, .kind = FSIMG_ENTRY_FILE };
		fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };

		status = fsimg_writer_add(f.writer, &entry, &source, &error);
	}
	if (status == FSIMG_OK)
		status = plan_status(&f, &error);
	if (status != expected)
		fprintf(stderr, "%s: expected status %d, actual %d: %s (%s)\n",
		    scenario, expected, status, error.message, error.path);
	EXPECT(status == expected);
	/* Common path validation rejects invalid UTF-8 without attaching a path. */
	if (expected != FSIMG_OK && !label && !invalid_utf8) {
		fsimg_error_t rendered = { 0 };

		fsimg_error_path(&rendered, path);
		if (strcmp(error.path, rendered.path) != 0)
			fprintf(stderr, "%s: expected error path [%s], actual [%s]\n",
			    scenario, rendered.path, error.path);
		EXPECT(strcmp(error.path, rendered.path) == 0);
	}
	fixture_close(&f);
}

static void
collision_case(unsigned variant, const char *first, const char *second,
    fsimg_status_t expected)
{
	fsimg_write_options_t opts = options_for(4096);
	fixture_t f;
	fsimg_error_t error;

	fixture_open(&f, variant, &opts);
	add_entry(&f, first, FSIMG_ENTRY_FILE, 0, NULL);
	add_entry(&f, second, FSIMG_ENTRY_FILE, 0, NULL);
	EXPECT(plan_status(&f, &error) == expected);
	if (expected != FSIMG_OK)
		EXPECT(strcmp(error.path, second) == 0);
	fixture_close(&f);
}

static void
test_names(void)
{
	/* The 23 forbidden adjacent stored pairs, transcribed from the
	 * Unicode evidence rather than obtained from the writer's table.
	 */
	static const uint16_t pairs[][2] = {
		{ 0x0306, 0x0307 }, { 0x0308, 0x030d }, { 0x00a8, 0x030d },
		{ 0x0391, 0x030d }, { 0x0395, 0x030d }, { 0x0397, 0x030d },
		{ 0x0399, 0x030d }, { 0x039f, 0x030d }, { 0x03a5, 0x030d },
		{ 0x03a9, 0x030d }, { 0x03b1, 0x030d }, { 0x03b5, 0x030d },
		{ 0x03b7, 0x030d }, { 0x03b9, 0x030d }, { 0x03bf, 0x030d },
		{ 0x03c5, 0x030d }, { 0x03c9, 0x030d }, { 0x03d2, 0x030d },
		{ 0x09ac, 0x09bc }, { 0x0a21, 0x0a3c }, { 0x0b2f, 0x0b3c },
		{ 0x0e4d, 0x0e32 }, { 0x0ecd, 0x0eb2 }
	};
	static const char *invalid[] = {
		"/bad\xff", "/\xef\xbf\xbe", "/\xef\xbf\xbf", "/\xed\xa0\x80"
	};
	static const char *owned[] = {
		"\xe2\x90\x80\xe2\x90\x80\xe2\x90\x80\xe2\x90\x80HFS+ Private Data",
		".HFS+ Private Directory Data\r", ".journal", ".journal_info_block"
	};
	char path[1100], alternate[1100];
	size_t length;
	uint16_t ignorable;

	for (ignorable = 1; ignorable != 0 && hfsplus_fold_unit(ignorable) != 0;
	    ignorable++)
		;
	REQUIRE(ignorable != 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned i = 0; i < 4; i++) {
			named("variant %u invalid UTF-8/name scalar %u", variant, i);
			name_case(variant, invalid[i], FSIMG_EINVAL, false);
			name_case(variant, invalid[i], FSIMG_EINVAL, true);
		}
		for (unsigned count = 127; count <= 128; count++) {
			named("variant %u decomposed length from %u e-acute", variant, count);
			path[0] = '/';
			length = 1;
			for (unsigned i = 0; i < count; i++)
				length += utf8_unit(path + length, 0x00e9);
			if (count == 127)
				path[length++] = 'x';
			path[length] = '\0';
			name_case(variant, path, count == 127 ? FSIMG_OK : FSIMG_EINVAL, false);
			name_case(variant, path, count == 127 ? FSIMG_OK : FSIMG_EINVAL, true);
		}
		memset(path, 'a', 257);
		path[0] = '/';
		path[257] = '\0';
		named("variant %u 256-unit ASCII name", variant);
		name_case(variant, path, FSIMG_EINVAL, false);
		for (unsigned i = 0; i < 23; i++) {
			for (unsigned prefix = 0; prefix < 2; prefix++) {
				named("variant %u forbidden pair %04x %04x prefix=%u",
				    variant, pairs[i][0], pairs[i][1], prefix);
				strcpy(path, prefix == 0 ? "/" : "/before");
				length = strlen(path);
				length += utf8_unit(path + length, pairs[i][0]);
				length += utf8_unit(path + length, pairs[i][1]);
				strcpy(path + length, "after");
				name_case(variant, path, FSIMG_EINVAL, false);
				name_case(variant, path, FSIMG_EINVAL, true);
			}
			for (unsigned near = 0; near < 2; near++) {
				named("variant %u pair %u near miss %u", variant, i, near);
				path[0] = '/';
				length = 1;
				if (near != 0) {
					length += utf8_unit(path + length, pairs[i][0]);
					path[length++] = 'x';
				}
				length += utf8_unit(path + length, pairs[i][1]);
				path[length] = '\0';
				name_case(variant, path, FSIMG_OK, false);
			}
		}
		named("variant %u all-ignorable U+%04x", variant, ignorable);
		path[0] = '/';
		length = 1 + utf8_unit(path + 1, ignorable);
		path[length] = '\0';
		name_case(variant, path, variant == 0 ? FSIMG_EINVAL : FSIMG_OK, false);
		named("variant %u case collision", variant);
		collision_case(variant, "/A", "/a", variant == 0 ? FSIMG_EINVAL : FSIMG_OK);
		named("variant %u canonical collision", variant);
		collision_case(variant, "/e\xcc\x81", "/\xc3\xa9", FSIMG_EINVAL);
		for (unsigned i = 0; i < 4; i++) {
			fixture_t f;
			fsimg_write_options_t opts = options_for(4096);

			named("variant %u owned root name %u", variant, i);
			snprintf(path, sizeof(path), "/%s", owned[i]);
			name_case(variant, path, FSIMG_EINVAL, false);
			fixture_open(&f, variant, &opts);
			add_entry(&f, "/sub", FSIMG_ENTRY_DIRECTORY, 0, NULL);
			snprintf(path, sizeof(path), "/sub/%s", owned[i]);
			add_entry(&f, path, FSIMG_ENTRY_FILE, 0, NULL);
			plan_ok(&f);
			fixture_close(&f);
			snprintf(alternate, sizeof(alternate), "/%s", owned[i]);
			for (char *p = alternate; *p != '\0'; p++)
				if (*p >= 'a' && *p <= 'z') {
					*p -= 'a' - 'A';
					break;
				}
			named("variant %u owned root name %u case variant", variant, i);
			name_case(variant, alternate, variant == 0 ? FSIMG_EINVAL : FSIMG_OK, false);
		}
	}
}

static void
check_stored_names(hfsplus_plan_t *p)
{
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_write_record_t r;
	uint64_t cursor = 0;
	size_t length;
	bool mapped_null = false, mapped_slash = false, decomposed = false;

	for (uint64_t i = 0; i < p->records->count; i++) {
		REQUIRE(fsimg_stream_next(p->records, &cursor, raw, sizeof(raw),
		    &length, NULL) == FSIMG_OK);
		REQUIRE(hfsplus_write_record_decode(raw, length, &r));
		if (r.type != HFSPLUS_RECORD_FILE)
			continue;
		if (r.units == 2 && be16(r.name) == 'n' && be16(r.name + 2) == 0)
			mapped_null = true;
		if (r.units == 2 && be16(r.name) == 's' && be16(r.name + 2) == '/')
			mapped_slash = true;
		if (r.units == 2 && be16(r.name) == 'e' && be16(r.name + 2) == 0x301)
			decomposed = true;
	}
	EXPECT(mapped_null && mapped_slash && decomposed);
}

static void
check_key_order(hfsplus_plan_t *p, unsigned entries)
{
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX], previous[510];
	unsigned primary[64] = { 0 }, thread[64] = { 0 };
	bool seen_parent[128] = { false };
	hfsplus_write_record_t r;
	hfsplus_ckey_t key, prev = { .name = previous };
	uint64_t cursor = 0;
	size_t length;

	REQUIRE(entries < 64);
	EXPECT(p->records->count == (entries + 1) * 2);
	for (uint64_t i = 0; i < p->records->count; i++) {
		REQUIRE(fsimg_stream_next(p->records, &cursor, raw, sizeof(raw),
		    &length, NULL) == FSIMG_OK);
		REQUIRE(hfsplus_write_record_decode(raw, length, &r));
		key = (hfsplus_ckey_t){ .parent = r.parent, .name = r.name,
			.units = r.type == HFSPLUS_RECORD_FILE ||
			    r.type == HFSPLUS_RECORD_FOLDER ? r.units : 0 };
		if (i != 0)
			EXPECT(hfsplus_ckey_compare(p->compare, &prev, &key) < 0);
		REQUIRE(r.id <= entries && r.parent < 128);
		if (key.units == 0) {
			thread[r.id]++;
			EXPECT(r.parent == hfsplus_node_id(r.id));
			EXPECT(!seen_parent[r.parent]);
		} else {
			primary[r.id]++;
		}
		seen_parent[r.parent] = true;
		if (r.id == 0) {
			if (key.units != 0)
				EXPECT(r.parent == 1 && key.units == 8);
			else
				EXPECT(r.parent == 2 && r.units == 8);
			EXPECT(bcmp(r.name, "\0u\0n\0t\0i\0t\0l\0e\0d", 16) == 0);
		}
		prev.parent = key.parent;
		prev.units = key.units;
		bcopy(key.name, previous, 2 * key.units);
	}
	for (unsigned i = 0; i <= entries; i++) {
		EXPECT(primary[i] == 1);
		EXPECT(thread[i] == 1);
	}
}

static void
test_key_order_and_mapping(void)
{
	static const char *paths[] = {
		"/ASCII", "/ascii2", "/e\xcc\x80", "/\xc3\xa9", "/\xce\x91",
		"/\xce\xb1x", "/\xea\xb0\x80", "/\xf0\x9f\x98\x80", "/n\xe2\x90\x80",
		"/s:", "/ignore\xe2\x80\x8c", "/folder", "/folder/Child", "/folder/child2"
	};
	fixture_t f;
	fsimg_write_options_t opts = options_for(4096);
	hfsplus_plan_t *p;

	for (unsigned variant = 0; variant < 2; variant++) {
		named("variant %u real comparator order and stored names", variant);
		fixture_open(&f, variant, &opts);
		for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); i++)
			add_entry(&f, paths[i], i == 11 ? FSIMG_ENTRY_DIRECTORY :
			    FSIMG_ENTRY_FILE, 0, NULL);
		p = plan_ok(&f);
		check_key_order(p, sizeof(paths) / sizeof(*paths));
		check_stored_names(p);
		emit_checked(&f.writer->session, &p->catalog, p->records, p->levels);
		fixture_close(&f);
	}
	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned i = 0; i < 3; i++) {
			fsimg_status_t status;
			fsimg_error_t error;

			named("variant %u default label form %u", variant, i);
			opts.label = i == 0 ? NULL : i == 1 ? "" : "untitled";
			context_open(&f);
			status = fsimg_writer_new_variant(f.ctx, &format,
			    &variants[variant], &opts, &f.writer, &error);

			if (status != FSIMG_OK)
				fprintf(stderr, "%s: expected OK, actual %d: %s\n",
				    scenario, status, error.message);
			EXPECT(status == FSIMG_OK);
			if (status == FSIMG_OK)
				check_key_order(plan_ok(&f), 0);
			fixture_close(&f);
		}
	}
}

static fsimg_status_t
provider_validate(void *arg FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
provider_read(void *arg FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    uint64_t offset, void *buffer, size_t length, size_t *done,
    fsimg_error_t *error FSIMG_UNUSED)
{
	static const char target[] = "target";

	REQUIRE(offset <= 6 && length <= 6 - offset);
	bcopy(target + offset, buffer, length);
	*done = length;
	return (FSIMG_OK);
}

static void
add_link(fixture_t *f)
{
	fsimg_source_ops_t ops;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
	fsimg_entry_t entry = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
		.size = 6 };

	fsimg_source_ops_init(&ops);
	ops.validate = provider_validate;
	ops.read_at = provider_read;
	REQUIRE(fsimg_writer_set_sources(f->writer, &ops, NULL, NULL) == FSIMG_OK);
	REQUIRE(fsimg_writer_add(f->writer, &entry, &source, NULL) == FSIMG_OK);
}

static void
test_rows_and_counts(void)
{
	static const char *paths[] = { "/dir", "/dir/child", "/empty", "/fifo",
		"/link", "/payload" };
	static const uint16_t modes[] = { 0040755, 0040755, 0040755, 0100644,
		0010644, 0120755, 0100644 };
	fixture_t f;
	fsimg_write_options_t opts = options_for(4096);
	hfsplus_plan_t *p;
	hfsplus_row_t row;
	const fsimg_catalog_entry_t *entry;
	uint32_t next;

	for (unsigned variant = 0; variant < 2; variant++) {
		named("variant %u rows, counts, contiguous forks", variant);
		fixture_open(&f, variant, &opts);
		add_link(&f);
		add_entry(&f, "/payload", FSIMG_ENTRY_FILE, 4097, NULL);
		add_entry(&f, "/dir", FSIMG_ENTRY_DIRECTORY, 0, NULL);
		add_entry(&f, "/dir/child", FSIMG_ENTRY_DIRECTORY, 0, NULL);
		add_entry(&f, "/empty", FSIMG_ENTRY_FILE, 0, NULL);
		add_entry(&f, "/fifo", FSIMG_ENTRY_FIFO, 0, NULL);
		p = plan_ok(&f);
		EXPECT(hfsplus_node_id(0) == 2);
		EXPECT(p->next_catalog_id == 22);
		EXPECT(p->file_count == 4 && p->folder_count == 2);
		next = p->catalog.first_block + p->catalog.blocks;
		for (unsigned i = 0; i <= 6; i++) {
			REQUIRE(hfsplus_row_get(p, i, &row, NULL) == FSIMG_OK);
			EXPECT(row.uid == 0 && row.gid == 0);
			EXPECT(row.mode == modes[i]);
			EXPECT(row.created == 2082844800u);
			EXPECT(row.modified == 2082844800u);
			EXPECT(row.accessed == 2082844800u);
			EXPECT(row.changed == 2082844800u);
			if (i == 0) {
				EXPECT(row.parent == 1 && row.valence == 5 && row.folders == 1);
				continue;
			}
			EXPECT(hfsplus_node_id(i) == 15 + i);
			REQUIRE(fsimg_catalog_get(f.writer, i, &entry, NULL) == FSIMG_OK);
			EXPECT(strcmp(entry->entry.path, paths[i - 1]) == 0);
			EXPECT(row.parent == (i == 2 ? 16 : 2));
			EXPECT(row.valence == (i == 1 ? 1u : 0u));
			EXPECT(row.folders == (i == 1 ? 1u : 0u));
			EXPECT(row.length == (i == 5 ? 6u : i == 6 ? 4097u : 0u));
			EXPECT(row.blocks == (i == 5 ? 1u : i == 6 ? 2u : 0u));
			if (row.blocks == 0)
				EXPECT(row.first_block == 0);
			else {
				EXPECT(row.first_block == next);
				next += row.blocks;
			}
		}
		EXPECT(p->allocated == next);
		check_geometry(p, brute_blocks(4096, 0, 3), 4096, 1, 3);
		fixture_close(&f);
		named("variant %u empty next catalog ID", variant);
		fixture_open(&f, variant, &opts);
		p = plan_ok(&f);
		EXPECT(p->next_catalog_id == 16 && p->file_count == 0 && p->folder_count == 0);
		fixture_close(&f);
	}
}

static void
test_metadata_policy(void)
{
	fixture_t f;
	fsimg_write_options_t opts;
	fsimg_metadata_t meta = {
		.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_CREATED |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED | FSIMG_META_CHANGED,
		.uid = 123, .gid = 456, .mode = 0610,
		.created = { .seconds = -2082844800, .basis = FSIMG_TIME_UTC,
		    .precision_ns = 1000000000 },
		.modified = { .seconds = 0, .basis = FSIMG_TIME_UTC,
		    .precision_ns = 1000000000 },
		.accessed = { .seconds = 2212122495, .basis = FSIMG_TIME_UTC,
		    .precision_ns = 1000000000 },
		.changed = { .seconds = 2212122496, .basis = FSIMG_TIME_UTC,
		    .precision_ns = 1000000000 }
	};
	hfsplus_plan_t *p;
	hfsplus_row_t row;
	fsimg_plan_info_t info;
	fsimg_error_t error;

	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned mode = 0; mode < 6; mode++) {
			unsigned policy = mode % 3;

			named("variant %u metadata policy %u bound %s", variant,
			    policy, mode < 3 ? "upper" : "lower");
			meta.changed.seconds = mode < 3 ? INT64_C(2212122496) :
			    -INT64_C(2082844801);
			opts = options_for(4096);
			if (policy != 2)
				opts.flags &= ~FSIMG_WRITE_REPRODUCIBLE;
			if (policy == 1)
				opts.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
			fixture_open(&f, variant, &opts);
			add_entry(&f, "/dates", FSIMG_ENTRY_FILE, 0, &meta);
			add_entry(&f, "/absent", FSIMG_ENTRY_FILE, 0, NULL);
			if (policy == 1) {
				EXPECT(plan_status(&f, &error) == FSIMG_EUNSUPPORTED);
				EXPECT(strcmp(error.path, "/dates") == 0);
				fixture_close(&f);
				continue;
			}
			p = plan_ok(&f);
			REQUIRE(hfsplus_row_get(p, 2, &row, NULL) == FSIMG_OK);
			EXPECT(row.uid == 123 && row.gid == 456 && row.mode == 0100610);
			EXPECT(row.created == (policy == 2 ? p->format_time : 0));
			EXPECT(row.modified == (policy == 2 ? p->format_time : 2082844800u));
			EXPECT(row.accessed == (policy == 2 ? p->format_time : UINT32_MAX));
			EXPECT(row.changed == p->format_time);
			REQUIRE(hfsplus_row_get(p, 1, &row, NULL) == FSIMG_OK);
			EXPECT(row.created == p->format_time && row.modified == p->format_time);
			EXPECT(row.accessed == p->format_time && row.changed == p->format_time);
			REQUIRE(fsimg_plan_get_info(&p->common, &info, NULL) == FSIMG_OK);
			EXPECT(info.metadata_losses == (policy == 2 ? 3u : 1u));
			fixture_close(&f);
		}
	}
}

static void
compare_streams(fsimg_stream_t *a, fsimg_stream_t *b)
{
	unsigned char one[HFSPLUS_WRITE_RECORD_MAX], two[HFSPLUS_WRITE_RECORD_MAX];
	uint64_t ac = 0, bc = 0;
	size_t an, bn;

	if (a == NULL || b == NULL) {
		EXPECT(a == b);
		return;
	}
	EXPECT(a->count == b->count && a->end == b->end);
	for (uint64_t i = 0; i < a->count; i++) {
		REQUIRE(fsimg_stream_next(a, &ac, one, sizeof(one), &an, NULL) == FSIMG_OK);
		REQUIRE(fsimg_stream_next(b, &bc, two, sizeof(two), &bn, NULL) == FSIMG_OK);
		EXPECT(an == bn && bcmp(one, two, an) == 0);
	}
}

struct node_capture {
	unsigned char *bytes;
	uint32_t node_bytes;
};

static fsimg_status_t
capture_node(void *arg, uint32_t number, const unsigned char *bytes,
    fsimg_error_t *error FSIMG_UNUSED)
{
	node_capture_t *capture = arg;

	bcopy(bytes, capture->bytes + (size_t)number * capture->node_bytes,
	    capture->node_bytes);
	return (FSIMG_OK);
}

static unsigned char *
capture_tree(fixture_t *f)
{
	hfsplus_plan_t *p = (hfsplus_plan_t *)f->writer->plan;
	node_capture_t capture = { .node_bytes = p->catalog.node_bytes };
	unsigned char node[4096];
	hfsplus_tree_emit_t emit = {
		.session = &f->writer->session, .shape = &p->catalog,
		.records = p->records, .levels = p->levels, .node = node,
		.leaf_data = leaf_pattern, .write = capture_node, .argument = &capture
	};

	capture.bytes = calloc(p->catalog.total_nodes, p->catalog.node_bytes);
	REQUIRE(capture.bytes != NULL);
	REQUIRE(hfsplus_tree_emit(&emit, NULL) == FSIMG_OK);
	return (capture.bytes);
}

static void
test_determinism(void)
{
	fixture_t baseline, other;
	fsimg_write_options_t opts = options_for(4096);
	fsimg_sort_options_t sort = { .run_bytes = 4096, .fan_in = 2 };
	hfsplus_plan_t *a, *b;
	unsigned char ar[HFSPLUS_ROW_BYTES], br[HFSPLUS_ROW_BYTES];
	unsigned char *anodes, *bnodes;
	char path[64];
	char *saved_tz = getenv("TZ") == NULL ? NULL : strdup(getenv("TZ"));

	for (unsigned variant = 0; variant < 2; variant++) {
		named("variant %u deterministic setup", variant);
		fixture_open(&baseline, variant, &opts);
		for (unsigned i = 0; i < 320; i++) {
			snprintf(path, sizeof(path), "/file%03u", i);
			add_entry(&baseline, path, FSIMG_ENTRY_FILE, i, NULL);
		}
		a = plan_ok(&baseline);
		anodes = capture_tree(&baseline);
		for (unsigned pass = 0; pass < 3; pass++) {
			named("variant %u determinism reverse/sort/TZ pass %u", variant, pass);
			REQUIRE(setenv("TZ", pass == 2 ? "Pacific/Honolulu" : "UTC0", 1) == 0);
			tzset();
			fixture_open(&other, variant, &opts);
			for (unsigned j = 0; j < 320; j++) {
				unsigned i = pass == 0 ? 319 - j : j;

				snprintf(path, sizeof(path), "/file%03u", i);
				add_entry(&other, path, FSIMG_ENTRY_FILE, i, NULL);
			}
			if (pass != 0) {
				/* Custom preparation is an internal entry point. The
				 * public planner owns preparation with default settings;
				 * a prepared catalog goes straight to the driver here.
				 */
				REQUIRE(fsimg_writer_prepare(other.writer, &sort, NULL) == FSIMG_OK);
				REQUIRE(hfsplus_write_plan(other.writer, NULL) == FSIMG_OK);
				b = (hfsplus_plan_t *)other.writer->plan;
			} else {
				b = plan_ok(&other);
			}
			EXPECT(a->volume_bytes == b->volume_bytes);
			EXPECT(a->block_bytes == b->block_bytes);
			EXPECT(a->allocated == b->allocated);
			EXPECT(a->total_blocks == b->total_blocks);
			EXPECT(a->head_blocks == b->head_blocks && a->tail_blocks == b->tail_blocks);
			EXPECT(a->bitmap_blocks == b->bitmap_blocks);
			EXPECT(bcmp(&a->catalog, &b->catalog, sizeof(a->catalog)) == 0);
			EXPECT(bcmp(&a->extents, &b->extents, sizeof(a->extents)) == 0);
			EXPECT(bcmp(&a->attributes, &b->attributes, sizeof(a->attributes)) == 0);
			for (unsigned i = 0; i <= 320; i++) {
				REQUIRE(fsimg_array_get(a->rows, i, ar, NULL) == FSIMG_OK);
				REQUIRE(fsimg_array_get(b->rows, i, br, NULL) == FSIMG_OK);
				EXPECT(bcmp(ar, br, sizeof(ar)) == 0);
			}
			compare_streams(a->records, b->records);
			for (unsigned i = 0; i < HFSPLUS_BTREE_DEPTH_MAX; i++)
				compare_streams(a->levels[i], b->levels[i]);
			bnodes = capture_tree(&other);
			EXPECT(bcmp(anodes, bnodes, (size_t)a->catalog.total_nodes * 4096) == 0);
			free(bnodes);
			fixture_close(&other);
		}
		free(anodes);
		fixture_close(&baseline);
	}
	if (saved_tz != NULL) {
		REQUIRE(setenv("TZ", saved_tz, 1) == 0);
		free(saved_tz);
	} else {
		REQUIRE(unsetenv("TZ") == 0);
	}
	tzset();
}


/* Emission tests use the registered writer descriptor.
 * The earlier tests keep their planning-only descriptor.
 */
static void
check_emitted_reader(hwf_fixture_t *f, hwf_image_t *image, unsigned variant,
    bool empty, bool reproducible)
{
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_volume_info_t volume;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned count = 0;
	uint64_t logical = 0;
	bool seen[HWF_COUNT] = { false };
	unsigned char bytes[4096];

	fsimg_read_options_init(&options);
	options.format = variant ? "hfsx" : "hfsplus";
	REQUIRE(hwf_io(f, image, &io) == FSIMG_OK);
	REQUIRE(fsimg_reader_open(f->ctx, io, &options, &reader, NULL) == FSIMG_OK);
	REQUIRE(fsimg_reader_get_info(reader, &volume, NULL) == FSIMG_OK);
	EXPECT(strcmp(volume.label, HWF_LABEL) == 0);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		unsigned which = HWF_COUNT;
		size_t done;

		if (strcmp(entry->path, "/") == 0) {
			EXPECT(entry->kind == FSIMG_ENTRY_DIRECTORY);
			continue;
		}
		for (unsigned i = 0; i < HWF_COUNT && !empty; i++)
			if (strcmp(f->entries[i].listed, entry->path) == 0)
				which = i;
		REQUIRE(which < HWF_COUNT);
		EXPECT(!seen[which]);
		seen[which] = true;
		count++;
		EXPECT(entry->kind == f->entries[which].kind);
		EXPECT(entry->size == f->entries[which].length);
		EXPECT(entry->metadata.uid == 123 && entry->metadata.gid == 456);
		EXPECT(entry->metadata.mode == 0751);
		EXPECT((entry->metadata.valid & hwf_metadata().valid) == hwf_metadata().valid);
		EXPECT(entry->metadata.created.seconds == HWF_EPOCH - (reproducible ? 0 : 4));
		EXPECT(entry->metadata.modified.seconds == HWF_EPOCH - (reproducible ? 0 : 3));
		EXPECT(entry->metadata.changed.seconds == HWF_EPOCH - (reproducible ? 0 : 2));
		EXPECT(entry->metadata.accessed.seconds == HWF_EPOCH - (reproducible ? 0 : 1));
		logical += entry->size;
		for (uint64_t offset = 0; offset < entry->size; offset += done) {
			REQUIRE(fsimg_reader_read_at(reader, offset, bytes,
			    sizeof(bytes), &done, NULL) == FSIMG_OK);
			REQUIRE(done > 0 && done <= entry->size - offset);
			for (size_t i = 0; i < done; i++)
				EXPECT(bytes[i] == hwf_byte(which + 1, offset + i));
		}
	}
	EXPECT(status == FSIMG_DONE);
	EXPECT(count == (empty ? 0 : HWF_COUNT));
	fsimg_reader_free(reader);
	REQUIRE(fsimg_reader_open(f->ctx, io, &options, &reader, NULL) == FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
	EXPECT(status == FSIMG_OK);
	EXPECT(report.complete);
	EXPECT(report.files == (empty ? 0 : HWF_COUNT - 3));
	EXPECT(report.directories == (empty ? 1 : 4));
	EXPECT(report.logical_bytes == logical);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
}

static void
test_emission_matrix(void)
{
	hwf_fixture_t f;
	hwf_image_t image;
	fsimg_write_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;

	for (unsigned variant = 0; variant < 2; variant++) {
		for (uint32_t block = 512; block <= 65536; block *= 2) {
			for (unsigned shape = 0; shape < 5; shape++) {
				named("emission variant=%u block=%u shape=%u", variant, block, shape);
				fprintf(stderr, "%s\n", scenario);
				hwf_options(&options, block);
				if (shape >= 2)
					options.volume_bytes = 1024 * 1024 +
					    (shape == 3 ? 512 : shape == 4 ? 1536 : 0);
				hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
				if (shape != 1)
					hwf_add_tree(&f, block, false);
				image = (hwf_image_t){ 0 };
				status = hwf_emit(&f, &image, &error);
				if (status != FSIMG_OK)
					fprintf(stderr, "%s: emit %d %s\n", scenario, status, error.message);
				REQUIRE(status == FSIMG_OK);
				hwc_check(&image, &f, &options, variant, shape == 1);
				check_emitted_reader(&f, &image, variant, shape == 1, true);
				free(image.bytes);
				hwf_close(&f);
			}
		}
		hwf_options(&options, 0);
		options.flags |= FSIMG_WRITE_MINIMIZE;
		named("emission minimize variant=%u", variant);
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		hwf_add_tree(&f, 4096, false);
		image = (hwf_image_t){ 0 };
		REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
		hwc_check(&image, &f, &options, variant, false);
		check_emitted_reader(&f, &image, variant, false, true);
		free(image.bytes);
		hwf_close(&f);
	}
}

static void
test_reserves_tail_blocks_when_alternate_is_past_allocation_domain(void)
{
	hwf_fixture_t f;
	fsimg_write_options_t options;
	const unsigned char *header, *bitmap;
	uint32_t blocks, used;

	for (unsigned variant = 0; variant < 2; variant++) {
		for (uint32_t block = 2048; block <= 4096; block *= 2) {
			for (uint32_t remainder = 1024; remainder <= 2048; remainder += 512) {
				hwf_image_t image = { 0 };

				if (remainder >= block)
					continue;
				named("reserved tail beyond allocation domain variant=%u B=%u remainder=%u",
				    variant, block, remainder);
				fprintf(stderr, "%s\n", scenario);
				hwf_options(&options, block);
				options.volume_bytes = 4 * 1024 * 1024 + remainder;
				hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
				hwf_add_tree(&f, block, false);
				REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
				REQUIRE(image.length == options.volume_bytes);
				header = image.bytes + 1024;
				EXPECT(be32(header + 40) == block);
				blocks = be32(header + 44);
				REQUIRE(blocks == image.length / block);
				EXPECT(image.length - (uint64_t)blocks * block == remainder);
				REQUIRE(be32(header + 128) < blocks);
				bitmap = image.bytes + (uint64_t)be32(header + 128) * block;
				EXPECT((bitmap[(blocks - 1) / 8] &
				    (1u << (7 - (blocks - 1) % 8))) != 0);
				used = 0;
				for (uint32_t bit = 0; bit < blocks; bit++)
					used += (bitmap[bit / 8] >> (7 - bit % 8)) & 1;
				EXPECT(be32(header + 48) == blocks - used);
				EXPECT(bcmp(header, image.bytes + image.length - 1024, 512) == 0);
				hwc_check(&image, &f, &options, variant, false);
				check_emitted_reader(&f, &image, variant, false, true);
				free(image.bytes);
				hwf_close(&f);
			}
		}
	}
}

static void
test_emitted_metadata(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		hwf_fixture_t f;
		hwf_image_t image = { 0 };
		fsimg_write_options_t options;
		time_t before, after;
		uint32_t created;

		named("emitted distinct timestamps variant=%u", variant);
		hwf_options(&options, 4096);
		options.flags &= ~FSIMG_WRITE_REPRODUCIBLE;
		before = time(NULL);
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		hwf_add_tree(&f, 4096, false);
		REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
		after = time(NULL);
		created = be32(image.bytes + 1024 + 16);
		EXPECT(created >= (uint64_t)before + 2082844800u);
		EXPECT(created <= (uint64_t)after + 2082844800u);
		hwc_check(&image, &f, &options, variant, false);
		check_emitted_reader(&f, &image, variant, false, false);
		free(image.bytes);
		hwf_close(&f);
	}
}

static void
test_emitted_identity(void)
{
	hwf_fixture_t f;
	hwf_image_t baseline, image;
	fsimg_write_options_t options;
	char *saved = getenv("TZ") == NULL ? NULL : strdup(getenv("TZ"));
	char directory[] = "/tmp/fsimg-hfs-identity.XXXXXX";
	char other_directory[] = "/tmp/fsimg-hfs-identity.XXXXXX";
	int scratch, other_scratch;

	REQUIRE(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY);
	REQUIRE(scratch >= 0);
	REQUIRE(mkdtemp(other_directory) != NULL);
	other_scratch = open(other_directory, O_RDONLY | O_DIRECTORY);
	REQUIRE(other_scratch >= 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		named("emitted identity variant=%u", variant);
		hwf_options(&options, 4096);
		baseline = (hwf_image_t){ 0 };
		for (unsigned pass = 0; pass < 3; pass++) {
			REQUIRE(setenv("TZ", pass == 0 ? "UTC0" : "PST8PDT", 1) == 0);
			tzset();
			hwf_open(&f, variant, &options,
			    pass == 1 ? 16 * 1024 * 1024 : 64 * 1024 * 1024,
			    pass == 0 ? -1 : pass == 1 ? scratch : other_scratch, false);
			hwf_add_tree(&f, 4096, pass != 0);
			image = (hwf_image_t){ 0 };
			REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
			if (pass == 0)
				baseline = image;
			else {
				EXPECT(image.length == baseline.length);
				EXPECT(bcmp(image.bytes, baseline.bytes, (size_t)image.length) == 0);
				free(image.bytes);
			}
			hwf_close(&f);
		}
		free(baseline.bytes);
	}
	REQUIRE(close(scratch) == 0);
	REQUIRE(rmdir(directory) == 0);
	REQUIRE(close(other_scratch) == 0);
	REQUIRE(rmdir(other_directory) == 0);
	if (saved == NULL)
		REQUIRE(unsetenv("TZ") == 0);
	else {
		REQUIRE(setenv("TZ", saved, 1) == 0);
		free(saved);
	}
	tzset();
}

typedef enum emit_fault {
	EMIT_ALLOC, EMIT_CANCEL, EMIT_SCRATCH_OPEN, EMIT_SCRATCH_RESIZE,
	EMIT_SCRATCH_READ, EMIT_SCRATCH_WRITE, EMIT_SHORT, EMIT_WRITE,
	EMIT_FLUSH, EMIT_SOURCE_SHORT, EMIT_EOF, EMIT_CHANGED
} emit_fault_t;

/* Each failed attempt owns a fresh context. Closing it must restore every
 * resource counter; scratch entries are checked by removing the directory.
 */
static void
test_emission_faults(void)
{
	const fsimg_meta_fault_t scratch_faults[] = { FSIMG_META_OPEN,
	    FSIMG_META_RESIZE, FSIMG_META_READ, FSIMG_META_WRITE };
	char directory[] = "/tmp/fsimg-hfs-faults.XXXXXX";
	hwf_fixture_t f;
	hwf_image_t image;
	fsimg_write_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	bool hit;
	int scratch;

	REQUIRE(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY);
	REQUIRE(scratch >= 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		for (uint32_t block = 512; block <= 8192; block *= 16) {
			for (emit_fault_t fault = EMIT_ALLOC; fault <= EMIT_CHANGED; fault++) {
				for (uint64_t point = 1;; point++) {
					named("emit fault variant=%u B=%u fault=%u point=%" PRIu64,
					    variant, block, fault, point);
					REQUIRE(point < 10000);
					hwf_options(&options, block);
					hwf_open(&f, variant, &options, 16 * 1024 * 1024, scratch, false);
					/* One payload, rather than the full boundary matrix. */
					{
						fsimg_entry_t entry = { .path = "/payload",
						    .kind = FSIMG_ENTRY_FILE, .size = 31 };
						fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
						REQUIRE(fsimg_writer_add(f.writer, &entry, &source, NULL) == FSIMG_OK);
					}
					image = (hwf_image_t){ 0 };
					if (fault == EMIT_ALLOC)
						fsimg_test_fail_alloc_after(f.ctx, point);
					else if (fault == EMIT_CANCEL) {
						f.polls = 0;
						f.cancel_at = point;
					} else if (fault >= EMIT_SCRATCH_OPEN && fault <= EMIT_SCRATCH_WRITE)
						fsimg_test_meta_fail_after(f.ctx,
						    scratch_faults[fault - EMIT_SCRATCH_OPEN], point, EIO);
					else if (fault == EMIT_SHORT)
						image.short_write = point;
					else if (fault == EMIT_WRITE)
						image.fail_write = point;
					else if (fault == EMIT_FLUSH)
						image.fail_flush = point == 1;
					else if (fault == EMIT_SOURCE_SHORT)
						f.short_read = point;
					else if (fault == EMIT_EOF)
						f.fail_read = point;
					else
						f.fail_validate = point;
					status = hwf_emit(&f, &image, &error);
					if (fault == EMIT_ALLOC)
						hit = f.ctx->allocation_attempts >= point;
					else if (fault == EMIT_CANCEL)
						hit = f.polls >= point;
					else if (fault >= EMIT_SCRATCH_OPEN && fault <= EMIT_SCRATCH_WRITE)
						hit = f.ctx->meta_fault_after == 0;
					else if (fault == EMIT_SHORT || fault == EMIT_WRITE)
						hit = image.writes >= point;
					else if (fault == EMIT_FLUSH)
						hit = point == 1;
					else if (fault == EMIT_EOF || fault == EMIT_SOURCE_SHORT)
						hit = f.reads >= point;
					else
						hit = f.validates >= point;
					if (!hit || fault == EMIT_SHORT || fault == EMIT_SOURCE_SHORT)
						EXPECT(status == FSIMG_OK);
					else {
						if (fault == EMIT_ALLOC)
							EXPECT(status == FSIMG_ENOMEM || status == FSIMG_ELIMIT);
						else if (fault == EMIT_CANCEL)
							EXPECT(status == FSIMG_ECANCELLED);
						else if (fault == EMIT_CHANGED)
							EXPECT(status == FSIMG_ESOURCE_CHANGED);
						else
							EXPECT(status == FSIMG_EIO);
						EXPECT(error.output_state != FSIMG_OUTPUT_PUBLISHED);
					}
					free(image.bytes);
					fsimg_test_fail_alloc_after(f.ctx, 0);
					fsimg_test_meta_fail_after(f.ctx, FSIMG_META_OPEN, 0, 0);
					f.cancel_at = 0;
					hwf_close(&f);
					/* No named scratch survives even between attempts. */
					{
						DIR *dir = opendir(directory);
						struct dirent *entry;
						REQUIRE(dir != NULL);
						while ((entry = readdir(dir)) != NULL)
							EXPECT(strcmp(entry->d_name, ".") == 0 ||
							    strcmp(entry->d_name, "..") == 0);
						REQUIRE(closedir(dir) == 0);
					}
					if (!hit) {
						EXPECT(point > 1);
						fprintf(stderr, "%s: swept %" PRIu64 " checkpoints\n",
						    scenario, point - 1);
						break;
					}
				}
			}
		}
	}
	REQUIRE(close(scratch) == 0);
	REQUIRE(rmdir(directory) == 0);
}

static void
test_real_capabilities(void)
{
	fsimg_write_options_t options;
	hwf_fixture_t f;
	fsimg_entry_t device = { .path = "/device", .kind = FSIMG_ENTRY_CHAR_DEVICE };
	fsimg_type_selector_t selector;
	const fsimg_partition_type_t *type;

	named("real descriptor capabilities");
	fprintf(stderr, "%s\n", scenario);
	REQUIRE(fsimg_hfsplus_format.writer != NULL);
	for (unsigned variant = 0; variant < 2; variant++) {
		const fsimg_format_info_t *info = &fsimg_hfsplus_format.variants[variant].info;

		EXPECT(info->max_file_bytes == UINT64_C(0xffffffff0000));
		EXPECT(info->max_volume_bytes == UINT64_C(0xffffffff0000));
		EXPECT(info->max_label_bytes == 510);
		EXPECT(info->max_name_utf16_units == 255);
		EXPECT(info->max_link_target_bytes == 1023);
		EXPECT(info->write_settings == FSIMG_SETTING_VOLUME_UUID);
		EXPECT(info->write_allocation_unit_size_mask == 0x1fe00u);
		EXPECT(info->entry_kinds == (FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY |
		    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_FIFO | FSIMG_ENTRY_CHAR_DEVICE |
		    FSIMG_ENTRY_BLOCK_DEVICE | FSIMG_ENTRY_SOCKET));
		hwf_options(&options, 4096);
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		EXPECT(fsimg_writer_add(f.writer, &device, NULL, NULL) == FSIMG_EINVAL);
		hwf_close(&f);
		REQUIRE(fsimg_partition_type_select(NULL, info->name, &selector, NULL) == FSIMG_OK);
		type = selector.type;
		REQUIRE(type != NULL);
		EXPECT(strcmp(type->gpt_guid, "48465300-0000-11AA-AA11-00306543ECAC") == 0);
		EXPECT(type->mbr_types[0] == 0xaf);
	}
}

/* Poison covers the emission transfer-buffer allocation. The output's zero
 * callback turns poison off immediately afterward, before source I/O objects
 * that require zeroed allocations are created. The poisoned buffer holds
 * every tree node and bitmap batch. Construction, collection, planning and
 * cleanup run with poison disabled.
 */
static void
test_poisoned_images(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		for (uint32_t block = 512; block <= 65536; block *= 2) {
			pid_t child;
			int status;

			named("poisoned image variant=%u B=%u", variant, block);
			fflush(NULL);
			child = fork();
			REQUIRE(child >= 0);
			if (child == 0) {
				hwf_fixture_t f;
				hwf_image_t image = { 0 };
				fsimg_write_options_t options;
				const fsimg_plan_t *plan;
				fsimg_plan_info_t info;
				fsimg_io_t *io;
				struct itimerval deadline = { .it_value.tv_sec = 10 };

				REQUIRE(setitimer(ITIMER_REAL, &deadline, NULL) == 0);
				hwf_options(&options, block);
				hwf_open(&f, variant, &options,
				    64 * 1024 * 1024, -1, false);
				hwf_add_tree(&f, block, false);
				REQUIRE(fsimg_writer_plan(f.writer, &plan, NULL) == FSIMG_OK);
				REQUIRE(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
				image.length = info.volume_bytes;
				image.bytes = malloc((size_t)image.length);
				REQUIRE(image.bytes != NULL);
				memset(image.bytes, 0xa5, (size_t)image.length);
				image.poison_context = f.ctx;
				REQUIRE(hwf_io(&f, &image, &io) == FSIMG_OK);
				fsimg_test_poison_allocations(f.ctx, true);
				REQUIRE(fsimg_writer_emit(f.writer, io, NULL) == FSIMG_OK);
				fsimg_test_poison_allocations(f.ctx, false);
				fsimg_io_free(io);
				hwc_check(&image, &f, &options, variant, false);
				free(image.bytes);
				hwf_close(&f);
				_exit(0);
			}
			REQUIRE(waitpid(child, &status, 0) == child);
			if (!WIFEXITED(status))
				fprintf(stderr, "%s: signal %d before completing image check\n",
				    scenario, WTERMSIG(status));
			EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		}
	}
}

/* Each child changes one otherwise zero byte. Requiring the checker's
 * explicit failure exit proves that every class reaches a rejecting check.
 */
static void
test_raw_scan_rejects_stray_bytes(void)
{
	static const char *classes[] = { "boot area", "node free space",
	    "unused node", "payload tail", "free block", "reserved record field" };
	hwf_fixture_t f;
	hwf_image_t image = { 0 };
	fsimg_write_options_t options;
	unsigned char *header, *cat, *leaf, *extents, *record;
	uint64_t offsets[6] = { 0 };
	uint32_t node, used;
	unsigned records, end;

	hwf_options(&options, 8192);
	options.volume_bytes = 1024 * 1024;
	hwf_open(&f, 0, &options, 64 * 1024 * 1024, -1, false);
	hwf_add_tree(&f, 8192, false);
	REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
	hwc_check(&image, &f, &options, 0, false);
	header = image.bytes + 1024;
	cat = image.bytes + (uint64_t)be32(header + 272 + 16) * 8192;
	extents = image.bytes + (uint64_t)be32(header + 192 + 16) * 8192;
	leaf = cat + (uint64_t)be32(cat + 14 + 10) * 4096;
	records = be16(leaf + 10);
	end = be16(leaf + 4096 - 2 * (records + 1));
	REQUIRE(end < 4096 - 2 * (records + 1));
	used = be32(extents + 14 + 22) - be32(extents + 14 + 26);
	REQUIRE(be32(extents + 14 + 26) > 0);
	record = leaf + be16(leaf + 4094);
	record += be16(record) + 2;
	REQUIRE(be16(record) == 1 && be32(record + 8) == 2);
	offsets[0] = 0;
	offsets[1] = (uint64_t)(leaf - image.bytes) + end;
	offsets[2] = (uint64_t)(extents - image.bytes) + (uint64_t)used * 4096;
	offsets[4] = (uint64_t)be32(header + 52) * 8192;
	REQUIRE(be32(header + 48) > 0);
	/* Locate a nonempty file whose final allocation block has slack. */
	node = be32(cat + 14 + 10);
	while (node != 0 && offsets[3] == 0) {
		leaf = cat + (uint64_t)node * 4096;
		for (unsigned r = 0; r < be16(leaf + 10); r++) {
			const unsigned char *key = leaf + be16(leaf + 4096 - 2 * (r + 1));
			const unsigned char *data = key + be16(key) + 2;
			uint64_t length;

			/* The root thread's reserved word follows its record type. */
			if (be16(data) == 3 && be32(key + 2) == 2)
				offsets[5] = (uint64_t)(data - image.bytes) + 2;
			if (be16(data) != 2)
				continue;
			length = (uint64_t)be32(data + 88) << 32 | be32(data + 92);
			if (length != 0 && length % 8192 != 0)
				offsets[3] = (uint64_t)be32(data + 104) * 8192 + length;
		}
		node = be32(leaf);
	}
	REQUIRE(offsets[3] != 0 && offsets[5] != 0);
	for (unsigned i = 0; i < 6; i++) {
		pid_t child;
		int status;

		named("raw scan rejects %s", classes[i]);
		REQUIRE(offsets[i] < image.length && image.bytes[offsets[i]] == 0);
		fprintf(stderr, "%s (expected checker failure)\n", scenario);
		fflush(NULL);
		child = fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			image.bytes[offsets[i]] = 1;
			hwc_check(&image, &f, &options, 0, false);
			_exit(0);
		}
		REQUIRE(waitpid(child, &status, 0) == child);
		EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	}
	free(image.bytes);
	hwf_close(&f);
}

static void
test_unpublished_failures(void)
{
	char directory[] = "/tmp/fsimg-hfs-output.XXXXXX";
	int parent;

	REQUIRE(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY);
	REQUIRE(parent >= 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		for (uint32_t block = 512; block <= 8192; block *= 16) {
			for (unsigned fault = 0; fault < 3; fault++) {
				hwf_fixture_t f;
				fsimg_write_options_t options;
				const fsimg_plan_t *plan;
				fsimg_error_t error;
				fsimg_status_t status;
				fsimg_entry_t entry = { .path = "/payload",
				    .kind = FSIMG_ENTRY_FILE, .size = 31 };
				fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };

				named("publication variant=%u B=%u fault=%u", variant, block, fault);
				hwf_options(&options, block);
				hwf_open(&f, variant, &options,
				    16 * 1024 * 1024, parent, false);
				REQUIRE(fsimg_writer_add(f.writer, &entry, &source, NULL) == FSIMG_OK);
				REQUIRE(fsimg_writer_plan(f.writer, &plan, NULL) == FSIMG_OK);
				if (fault == 0)
					f.fail_read = f.reads + 1;
				else if (fault == 1)
					f.fail_validate = f.validates + 1;
				else
					f.cancel_at = f.polls + 1;
				status = fsimg_writer_write_at(f.writer, parent, "image", &error);
				EXPECT(status == (fault == 0 ? FSIMG_EIO :
				    fault == 1 ? FSIMG_ESOURCE_CHANGED : FSIMG_ECANCELLED));
				EXPECT(error.output_state == FSIMG_OUTPUT_ABSENT);
				EXPECT(faccessat(parent, "image", F_OK, 0) == -1 && errno == ENOENT);
				f.cancel_at = 0;
				hwf_close(&f);
			}
		}
	}
	REQUIRE(close(parent) == 0);
	REQUIRE(rmdir(directory) == 0);
}

/* A malformed level record must be rejected before an index node is written.
 * The valid leaf stream has sixteen files: fifteen per leaf, then one, the
 * smallest two-leaf shape for one-unit file keys.
 */
typedef struct level_writes {
	uint32_t numbers[4];
	unsigned count;
} level_writes_t;

static fsimg_status_t
record_level_write(void *argument, uint32_t number,
    const unsigned char *bytes FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	level_writes_t *writes = argument;

	REQUIRE(writes->count < 4);
	writes->numbers[writes->count++] = number;
	return (FSIMG_OK);
}

/* Replace a completed level's scratch stream at a poll boundary, before the
 * index packer reads it. Both stores are real streams with their own frames;
 * swapping their values transfers ownership without calling into the library
 * from a callback. The replacement keeps the well-formed stream for cleanup.
 */
typedef struct level_corruption {
	hfsplus_tree_shape_t *shape;
	fsimg_stream_t **levels;
	fsimg_stream_t *replacement;
	bool injected;
} level_corruption_t;

static bool
replace_packed_level(void *argument)
{
	level_corruption_t *corruption = argument;
	fsimg_stream_t saved;

	if (!corruption->injected && corruption->shape->level_nodes[0] == 2) {
		corruption->injected = true;
		saved = *corruption->levels[0];
		*corruption->levels[0] = *corruption->replacement;
		*corruption->replacement = saved;
	}
	return (false);
}

static void
test_pack_and_emit_reject_malformed_level_records(void)
{
	typedef struct malformed_level_case {
		unsigned units;
		size_t length;
	} malformed_level_case_t;
	static const malformed_level_case_t cases[] = {
		{ .units = 0x0fff, .length = 8 },
		{ .units = 256, .length = 516 },
		{ .units = 2, .length = 8 },
		{ .units = 1, .length = 10 },
		{ .units = 0, .length = 4 }
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		fixture_t f;
		fsimg_session_t session;
		fsimg_stream_t *records, *levels[HFSPLUS_BTREE_DEPTH_MAX] = { NULL };
		hfsplus_tree_shape_t shape;
		unsigned char raw[HFSPLUS_WRITE_RECORD_MAX] = { 0 };
		unsigned char name[] = { 0, 'a' }, guarded[4096 + 64], guard[64];
		hfsplus_write_record_t record = { .type = HFSPLUS_RECORD_FILE,
		    .units = 1, .key_bytes = 2, .key = name, .name = name };
		level_writes_t writes = { 0 };
		hfsplus_tree_emit_t emit = { .session = &session, .shape = &shape,
		    .levels = levels, .node = guarded, .leaf_data = leaf_pattern,
		    .write = record_level_write, .argument = &writes };
		size_t length;

		named("malformed level units=%u length=%zu", cases[i].units, cases[i].length);
		context_open(&f);
		fsimg_session_init(&session, f.ctx, "hfsplus", 0);
		REQUIRE(fsimg_stream_new(&session, sizeof(raw), &records, NULL) == FSIMG_OK);
		for (unsigned n = 0; n < 16; n++) {
			record.parent = n + 2;
			length = hfsplus_write_record_encode(&record, raw);
			REQUIRE(fsimg_stream_append(records, raw, length, NULL, NULL) == FSIMG_OK);
		}
		REQUIRE(fsimg_stream_seal(records, NULL) == FSIMG_OK);
		hfsplus_tree_shape_init(&shape, HFSPLUS_TREE_CATALOG, HFSPLUS_COMPARE_FOLDED);
		REQUIRE(hfsplus_tree_pack(&session, records, &shape, levels, NULL) == FSIMG_OK);
		REQUIRE(shape.depth == 2 && shape.level_nodes[0] == 2);
		hfsplus_tree_place(&shape, 0, 4096);

		fsimg_stream_free(levels[0]);
		REQUIRE(fsimg_stream_new(&session, HFSPLUS_WRITE_LEVEL_MAX,
		    &levels[0], NULL) == FSIMG_OK);
		bzero(raw, sizeof(raw));
		fsimg_store_be32(raw, 2);
		fsimg_store_be16(raw + 4, (uint16_t)cases[i].units);
		REQUIRE(fsimg_stream_append(levels[0], raw, cases[i].length,
		    NULL, NULL) == FSIMG_OK);
		REQUIRE(fsimg_stream_seal(levels[0], NULL) == FSIMG_OK);
		memset(guarded, 0xa5, sizeof(guarded));
		bcopy(guarded + 4096, guard, sizeof(guard));
		emit.records = records;

		EXPECT(hfsplus_tree_emit(&emit, NULL) == FSIMG_ESTATE);
		EXPECT(bcmp(guarded + 4096, guard, sizeof(guard)) == 0);
		EXPECT(writes.count == 2);
		EXPECT(writes.numbers[0] == 1 && writes.numbers[1] == 2);
		for (unsigned level = 0; level < HFSPLUS_BTREE_DEPTH_MAX; level++)
			fsimg_stream_free(levels[level]);
		/* Exercise the sizing pass with the same malformed payload, not
		 * just the emitter's independently supplied stream.
		 */
		{
			fsimg_stream_t *replacement;
			const unsigned char first_key[] = { 0, 0, 0, 2, 0, 1, 0, 97 };
			level_corruption_t corruption = { .shape = &shape,
			    .levels = levels };

			bzero(levels, sizeof(levels));
			REQUIRE(fsimg_stream_new(&session, HFSPLUS_WRITE_LEVEL_MAX,
			    &replacement, NULL) == FSIMG_OK);
			/* The poll may occur during the first frame header read.
			 * Keep that frame identical; corrupt the following one.
			 */
			REQUIRE(fsimg_stream_append(replacement, first_key, sizeof(first_key),
			    NULL, NULL) == FSIMG_OK);
			REQUIRE(fsimg_stream_append(replacement, raw, cases[i].length,
			    NULL, NULL) == FSIMG_OK);
			REQUIRE(fsimg_stream_seal(replacement, NULL) == FSIMG_OK);
			corruption.replacement = replacement;
			hfsplus_tree_shape_init(&shape, HFSPLUS_TREE_CATALOG, HFSPLUS_COMPARE_FOLDED);
			f.ctx->options.cancelled = replace_packed_level;
			f.ctx->options.cancel_argument = &corruption;
			EXPECT(hfsplus_tree_pack(&session, records, &shape, levels, NULL) == FSIMG_ESTATE);
			EXPECT(corruption.injected);
			f.ctx->options.cancelled = NULL;
			f.ctx->options.cancel_argument = NULL;
			fsimg_stream_free(replacement);
			for (unsigned level = 0; level < HFSPLUS_BTREE_DEPTH_MAX; level++)
				fsimg_stream_free(levels[level]);
		}
		fsimg_stream_free(records);
		fsimg_ctx_free(f.ctx);
	}
}

static void
test_leaf_decoder_rejects_256_units(void)
{
	unsigned char raw[14 + 512] = { 0 };
	hfsplus_write_record_t record;

	named("leaf decoder refuses 256 units with matching length");
	fsimg_store_be16(raw + 12, 256);
	EXPECT(!hfsplus_write_record_decode(raw, sizeof(raw), &record));
}

static void
test_volume_validate_limit(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		fsimg_write_options_t options = options_for(65536);

		named("volume validation limit variant=%u", variant);
		options.volume_bytes = UINT64_C(0xffffffff0000);
		EXPECT(hfsplus_write_validate(&options, variant, NULL) == FSIMG_OK);
		options.volume_bytes = UINT64_C(0xffffffff0200);
		EXPECT(hfsplus_write_validate(&options, variant, NULL) == FSIMG_EINVAL);
	}
}

static void
test_plan_accepts_uint32_max_blocks(void)
{
	fixture_t f;
	fsimg_write_options_t options = options_for(512);
	hfsplus_plan_t *plan;
	fsimg_stats_t stats;

	named("plan UINT32_MAX blocks without retaining a bitmap");
	options.volume_bytes = UINT64_C(0xffffffff) * 512;
	fixture_open(&f, 0, &options);
	plan = plan_ok(&f);
	EXPECT(plan->total_blocks == UINT32_MAX);
	EXPECT(plan->volume_bytes == UINT64_C(0x1fffffffe00));
	EXPECT(plan->bitmap_blocks == 1048576);
	REQUIRE(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	/* A 512-MiB bitmap cannot fit in this context's 256-MiB budget.
	 * Its backing store must not exist either: only the tiny catalog does.
	 */
	EXPECT(stats.scratch_bytes < 1024 * 1024);
	fixture_close(&f);
}

static void
test_plan_refuses_catalog_record_count_overflow(void)
{
	fixture_t f;
	fsimg_write_options_t options = options_for(4096);

	named("catalog count exceeds two records per entry");
	fixture_open(&f, 0, &options);
	REQUIRE(fsimg_writer_prepare(f.writer, NULL, NULL) == FSIMG_OK);
	/* Enter the driver after preparation, which otherwise derives count
	 * from real catalog entries. Refusal precedes any catalog access.
	 */
	f.writer->count = UINT64_C(2147483647);
	EXPECT(hfsplus_write_plan(f.writer, NULL) == FSIMG_ERANGE);
	f.writer->count = 0;
	fixture_close(&f);
}

static void
test_real_label_mapping(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		fixture_t context;
		fsimg_write_options_t options;
		fsimg_writer_t *writer = NULL;
		hwf_fixture_t f;
		hwf_image_t image = { 0 };
		fsimg_io_t *io;
		fsimg_reader_t *reader;
		fsimg_read_options_t read_options;
		fsimg_volume_info_t volume;

		named("real label slash refusal variant=%u", variant);
		context_open(&context);
		hwf_options(&options, 4096);
		options.label = "a/b";
		EXPECT(fsimg_writer_new_variant(context.ctx, &fsimg_hfsplus_format,
		    &fsimg_hfsplus_format.variants[variant], &options,
		    &writer, NULL) == FSIMG_EINVAL);
		fsimg_writer_free(writer);

		fsimg_ctx_free(context.ctx);

		named("real label colon round trip variant=%u", variant);
		options.label = "a:b";
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		REQUIRE(hwf_emit(&f, &image, NULL) == FSIMG_OK);
		REQUIRE(hwf_io(&f, &image, &io) == FSIMG_OK);
		fsimg_read_options_init(&read_options);
		read_options.format = variant ? "hfsx" : "hfsplus";
		REQUIRE(fsimg_reader_open(f.ctx, io, &read_options, &reader, NULL) == FSIMG_OK);
		REQUIRE(fsimg_reader_get_info(reader, &volume, NULL) == FSIMG_OK);
		EXPECT(strcmp(volume.label, "a:b") == 0);
		fsimg_reader_free(reader);
		fsimg_io_free(io);
		free(image.bytes);
		hwf_close(&f);
	}
}

static void
test_real_volume_serial_accepted_and_ignored(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		fsimg_write_options_t options;
		hwf_fixture_t f;
		hwf_image_t baseline = { 0 }, with_serial = { 0 };

		named("real volume serial accepted and ignored variant=%u", variant);
		hwf_options(&options, 4096);
		options.volume_serial = 0;
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		REQUIRE(hwf_emit(&f, &baseline, NULL) == FSIMG_OK);
		hwf_close(&f);

		options.volume_serial = 1;
		hwf_open(&f, variant, &options, 64 * 1024 * 1024, -1, false);
		REQUIRE(hwf_emit(&f, &with_serial, NULL) == FSIMG_OK);
		REQUIRE(with_serial.length == baseline.length);
		EXPECT(memcmp(with_serial.bytes, baseline.bytes,
		    (size_t)baseline.length) == 0);
		free(with_serial.bytes);
		free(baseline.bytes);
		hwf_close(&f);
	}
}

static fsimg_status_t
long_link_read(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    uint64_t offset FSIMG_UNUSED, void *buffer, size_t length, size_t *done,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memset(buffer, 't', length);
	*done = length;
	return (FSIMG_OK);
}

static void
test_real_link_target_boundary(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned length = 1023; length <= 1024; length++) {
			fixture_t f;
			fsimg_write_options_t options;
			fsimg_source_ops_t ops;
			fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER };
			fsimg_entry_t entry = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
			    .size = length };
			const fsimg_plan_t *plan;

			named("real link target variant=%u bytes=%u", variant, length);
			hwf_options(&options, 4096);
			context_open(&f);
			REQUIRE(fsimg_writer_new_variant(f.ctx, &fsimg_hfsplus_format,
			    &fsimg_hfsplus_format.variants[variant], &options,
			    &f.writer, NULL) == FSIMG_OK);
			fsimg_source_ops_init(&ops);
			ops.read_at = long_link_read;
			ops.validate = provider_validate;
			REQUIRE(fsimg_writer_set_sources(f.writer, &ops, NULL, NULL) == FSIMG_OK);
			REQUIRE(fsimg_writer_add(f.writer, &entry, &source, NULL) == FSIMG_OK);
			/* The design accepts 1..1023 bytes; the reader must have room
			 * for a terminating NUL in its 1024-byte target buffer.
			 */
			EXPECT(fsimg_writer_plan(f.writer, &plan, NULL) ==
			    (length == 1023 ? FSIMG_OK : FSIMG_EUNSUPPORTED));
			fixture_close(&f);
		}
	}
}

static void
test_raw_check_rejects_missing_root_child(void)
{
	fixture_t f;
	fsimg_session_t session;
	fsimg_stream_t *records, *levels[HFSPLUS_BTREE_DEPTH_MAX] = { NULL };
	hfsplus_tree_shape_t shape;
	node_capture_t capture = { .node_bytes = 4096 };
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX], node[4096], name[] = { 0, 'a' };
	hfsplus_write_record_t record = { .type = HFSPLUS_RECORD_FILE,
	    .units = 1, .key_bytes = 2, .key = name, .name = name };
	hfsplus_tree_emit_t emit = { .session = &session, .shape = &shape,
	    .levels = levels, .node = node, .leaf_data = leaf_pattern,
	    .write = capture_node, .argument = &capture };
	unsigned char *root;
	unsigned count, start, end;
	uint64_t length;
	pid_t child;
	int status;

	named("raw checker rejects omitted final root child");
	context_open(&f);
	fsimg_session_init(&session, f.ctx, "hfsx", 0);
	REQUIRE(fsimg_stream_new(&session, sizeof(raw), &records, NULL) == FSIMG_OK);
	/* Fifteen one-unit files fit in a leaf; the sixteenth requires a
	 * second leaf and a two-record index root.
	 */
	for (unsigned i = 0; i < 16; i++) {
		size_t bytes;

		record.parent = i + 2;
		bytes = hfsplus_write_record_encode(&record, raw);
		REQUIRE(fsimg_stream_append(records, raw, bytes, NULL, NULL) == FSIMG_OK);
	}
	REQUIRE(fsimg_stream_seal(records, NULL) == FSIMG_OK);
	hfsplus_tree_shape_init(&shape, HFSPLUS_TREE_CATALOG, HFSPLUS_COMPARE_BINARY);
	REQUIRE(hfsplus_tree_pack(&session, records, &shape, levels, NULL) == FSIMG_OK);
	hfsplus_tree_place(&shape, 0, 4096);
	REQUIRE(shape.depth == 2 && shape.level_nodes[0] == 2);
	length = (uint64_t)shape.total_nodes * 4096;
	capture.bytes = calloc((size_t)length, 1);
	REQUIRE(capture.bytes != NULL);
	emit.records = records;
	REQUIRE(hfsplus_tree_emit(&emit, NULL) == FSIMG_OK);
	hwc_check_tree(capture.bytes, length);
	root = capture.bytes + 3 * 4096;
	count = be16(root + 10);
	REQUIRE(count == 2);
	start = be16(root + 4096 - 2 * count);
	end = be16(root + 4096 - 2 * (count + 1));

	fflush(NULL);
	child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		fsimg_store_be16(root + 10, (uint16_t)(count - 1));
		bzero(root + start, end - start);
		/* Keep the removed record's start as the free-space offset;
		 * erase the obsolete offset in the newly freed slack.
		 */
		bzero(root + 4096 - 2 * (count + 1), 2);
		hwc_check_tree(capture.bytes, length);
		_exit(0);
	}
	REQUIRE(waitpid(child, &status, 0) == child);
	EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	free(capture.bytes);
	for (unsigned level = 0; level < HFSPLUS_BTREE_DEPTH_MAX; level++)
		fsimg_stream_free(levels[level]);
	fsimg_stream_free(records);
	fsimg_ctx_free(f.ctx);
}

int
main(void)
{
	test_pack_and_emit_reject_malformed_level_records();
	test_leaf_decoder_rejects_256_units();
	test_volume_validate_limit();
	test_plan_accepts_uint32_max_blocks();
	test_plan_refuses_catalog_record_count_overflow();
	test_real_label_mapping();
	test_real_volume_serial_accepted_and_ignored();
	test_real_link_target_boundary();
	test_raw_check_rejects_missing_root_child();
	test_real_capabilities();
	test_emission_matrix();
	test_reserves_tail_blocks_when_alternate_is_past_allocation_domain();
	test_emitted_metadata();
	test_emitted_identity();
	test_emission_faults();
	test_unpublished_failures();
	test_poisoned_images();
	test_raw_scan_rejects_stray_bytes();
	test_bitmap_boundaries();
	test_settings_and_limits();
	test_minimization();
	test_tree_clump_low_32_bits();
	test_packing();
	test_names();
	test_key_order_and_mapping();
	test_rows_and_counts();
	test_metadata_policy();
	test_determinism();
	if (failures != 0)
		fprintf(stderr, "hfsplus writer: %u failed checks\n", failures);
	return (failures == 0 ? 0 : 1);
}
