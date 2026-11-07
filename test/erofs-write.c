/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg.h"
#include "format.h"
#include "layer.h"
#include "test.h"
#include "write-erofs.h"
#include "writer.h"

/*
 * EROFS writer planning tests. Emission is not covered: a plan is checked
 * against an independent model of the layout rules in specs/design-erofs.md
 * ("Layout", "Inodes", "Directories", "Metadata and defaults"). The model
 * works from the test's own description of the tree, in 32-byte slots and
 * with its own directory packer, and shares no code with
 * lib/formats/erofs/write-plan.c. It takes only the catalog's ID order from
 * the writer, since IDs are the catalog's to assign.
 *
 * Directory packing is also driven directly, and every block it produces is
 * decoded with the reader's decoders.
 */
#define BLOCK		4096u
#define SLOT		32u
#define SLOTS_PER_BLOCK	(BLOCK / SLOT)
#define EPOCH		INT64_C(1700000000)
#define OLD_TIME	INT64_C(1234567890)
#define MAX_SPECS	512u
#define MAX_PATH	320u
#define MAX_ROWS	(MAX_SPECS + 1)
#define MAX_NAMES	1024u
#define MAX_BLOCKS	128u

/* Report the case before the fatal check names the failed condition. */
#define CHECK_CASE(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
	CHECK(condition); \
} while (0)

/*
 * A tree under test: entries the test adds to a writer. A spec's mode is
 * the permission bits, 0 leaving them absent; its owner and time are given
 * only when owned or stamped.
 */
typedef struct spec {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint32_t	 mode;
	bool		 owned;
	uint32_t	 uid;
	uint32_t	 gid;
	bool		 stamped;
	int64_t		 mtime;
	uint32_t	 mtime_nsec;
} spec_t;

typedef struct tree {
	spec_t		 specs[MAX_SPECS];
	char		 paths[MAX_SPECS][MAX_PATH];
	size_t		 count;
} tree_t;

static tree_t tree;

static void
tree_reset(void)
{
	bzero(&tree, sizeof(tree));
}

/* Add an entry at the path the format produces, and return it to adjust. */
static spec_t *
tree_add(fsimg_entry_kind_t kind, uint64_t size, const char *format, ...)
{
	spec_t *spec;
	va_list ap;
	int length;

	CHECK(tree.count < MAX_SPECS);
	spec = &tree.specs[tree.count];
	va_start(ap, format);
	length = vsnprintf(tree.paths[tree.count], MAX_PATH, format, ap);
	va_end(ap);
	CHECK(length > 0 && (size_t)length < MAX_PATH);
	spec->path = tree.paths[tree.count];
	spec->kind = kind;
	spec->size = size;
	tree.count++;
	return (spec);
}

static spec_t *
add_file(uint64_t size, const char *format, ...)
{
	char path[MAX_PATH];
	va_list ap;

	va_start(ap, format);
	vsnprintf(path, sizeof(path), format, ap);
	va_end(ap);
	return (tree_add(FSIMG_ENTRY_FILE, size, "%s", path));
}

static spec_t *
add_directory(const char *path)
{
	return (tree_add(FSIMG_ENTRY_DIRECTORY, 0, "%s", path));
}

static spec_t *
with_owner(spec_t *spec, uint32_t uid, uint32_t gid)
{
	spec->owned = true;
	spec->uid = uid;
	spec->gid = gid;
	return (spec);
}

static spec_t *
with_time(spec_t *spec, int64_t seconds, uint32_t nanoseconds)
{
	spec->stamped = true;
	spec->mtime = seconds;
	spec->mtime_nsec = nanoseconds;
	return (spec);
}

/*
 * A context and a writer holding a tree, planned. plan is set when
 * planning succeeded, and error describes the first failure.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	erofs_plan_t	*plan;
	fsimg_error_t	 error;
	uint64_t	 memory_baseline;
} fixture_t;

/*
 * The settings every case but the label's own starts from: the defaults,
 * with the label given because the driver selects none (see
 * test_default_label_is_selected()).
 */
static void
base_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->label = "test";
}

static void
reproducible_options(fsimg_write_options_t *options)
{
	base_options(options);
	options->flags |= FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = EPOCH;
}

static fsimg_status_t
new_writer(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    fsimg_writer_t **writer, fsimg_error_t *error)
{
	return (fsimg_writer_new_variant(ctx, &fsimg_erofs_write_format,
	    &fsimg_erofs_write_format.variants[0], options, writer, error));
}

static void
fixture_open(fixture_t *fixture)
{
	fsimg_stats_t stats;

	bzero(fixture, sizeof(*fixture));
	CHECK(fsimg_ctx_new(NULL, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	fixture->memory_baseline = stats.memory_bytes;
}

/* Free the writer and require that it released everything it held. */
static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->memory_baseline);
	fsimg_ctx_free(fixture->ctx);
}

/*
 * Symbolic link targets come from a provider, which planning never reads;
 * the callbacks answer a read with the link's byte, so a stray read shows.
 */
static fsimg_status_t
provider_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    uint64_t offset FSIMG_UNUSED, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memset(buffer, 't', length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
add_spec(fixture_t *fixture, const spec_t *spec)
{
	fsimg_entry_t entry;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO, .id = 0 };
	bool has_content;

	bzero(&entry, sizeof(entry));
	entry.path = spec->path;
	entry.kind = spec->kind;
	entry.size = spec->size;
	if (spec->mode != 0) {
		entry.metadata.valid |= FSIMG_META_MODE;
		entry.metadata.mode = spec->mode;
	}
	if (spec->owned) {
		entry.metadata.valid |= FSIMG_META_OWNER;
		entry.metadata.uid = spec->uid;
		entry.metadata.gid = spec->gid;
	}
	if (spec->stamped) {
		entry.metadata.valid |= FSIMG_META_MODIFIED;
		entry.metadata.modified = (fsimg_timestamp_t){
		    .seconds = spec->mtime,
		    .nanoseconds = spec->mtime_nsec,
		    .precision_ns = 1,
		    .basis = FSIMG_TIME_UTC };
	}
	if (spec->kind == FSIMG_ENTRY_SYMLINK)
		source = (fsimg_source_t){ .kind = FSIMG_SOURCE_PROVIDER,
		    .id = 1 };
	has_content = spec->kind == FSIMG_ENTRY_FILE ||
	    spec->kind == FSIMG_ENTRY_SYMLINK;
	return (fsimg_writer_add(fixture->writer, &entry,
	    has_content ? &source : NULL, &fixture->error));
}

/*
 * Construct a writer, add the global tree (last to first when reverse) and
 * plan. Any failure is returned and described in fixture->error; the
 * fixture stays open for the caller to close.
 */
static fsimg_status_t
fixture_plan(fixture_t *fixture, const fsimg_write_options_t *options,
    bool reverse)
{
	const fsimg_plan_t *plan;
	fsimg_source_ops_t source_ops;
	fsimg_status_t status;

	status = new_writer(fixture->ctx, options, &fixture->writer,
	    &fixture->error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	status = fsimg_writer_set_sources(fixture->writer, &source_ops, NULL,
	    &fixture->error);
	if (status != FSIMG_OK)
		return (status);
	for (size_t i = 0; i < tree.count; i++) {
		status = add_spec(fixture,
		    &tree.specs[reverse ? tree.count - 1 - i : i]);
		if (status != FSIMG_OK)
			return (status);
	}
	status = fsimg_writer_plan(fixture->writer, &plan, &fixture->error);
	if (status == FSIMG_OK)
		fixture->plan = (erofs_plan_t *)plan;
	return (status);
}

/* Plan the global tree under options, which must succeed. */
static void
plan_tree(fixture_t *fixture, const fsimg_write_options_t *options)
{
	fsimg_status_t status;

	fixture_open(fixture);
	status = fixture_plan(fixture, options, false);
	CHECK_CASE(status == FSIMG_OK, "planning failed with %d: %s (%s)",
	    status, fixture->error.message, fixture->error.path);
}

static erofs_row_t
row_at(fixture_t *fixture, uint64_t id)
{
	erofs_row_t row;

	CHECK(erofs_row_get(fixture->plan, id, &row, NULL) == FSIMG_OK);
	return (row);
}

/* The path the catalog gives an ID, for naming a case. */
static void
catalog_path(fixture_t *fixture, uint64_t id, char *out, size_t length)
{
	const fsimg_catalog_entry_t *entry;

	CHECK(fsimg_catalog_get(fixture->writer, id, &entry, NULL) ==
	    FSIMG_OK);
	snprintf(out, length, "%s", entry->entry.path);
}

/*
 * The independent model.
 */

static const char *
skip_slash(const char *path)
{
	return (path[0] == '/' ? path + 1 : path);
}

/* The index of the spec the catalog holds at an ID. */
static size_t
spec_for_id(fixture_t *fixture, uint64_t id)
{
	char path[MAX_PATH];

	catalog_path(fixture, id, path, sizeof(path));
	for (size_t i = 0; i < tree.count; i++) {
		if (strcmp(skip_slash(path), skip_slash(tree.specs[i].path)) ==
		    0)
			return (i);
	}
	fprintf(stderr, "catalog path %s is in no spec\n", path);
	CHECK(false);
	return (0);
}

/* The catalog ID of the entry at a path. */
static uint64_t
id_of_path(fixture_t *fixture, const char *path)
{
	for (uint64_t id = 1; id <= fixture->writer->count; id++) {
		char found[MAX_PATH];

		catalog_path(fixture, id, found, sizeof(found));
		if (strcmp(skip_slash(found), skip_slash(path)) == 0)
			return (id);
	}
	fprintf(stderr, "no catalog entry %s\n", path);
	CHECK(false);
	return (0);
}

static erofs_row_t
row_of_path(fixture_t *fixture, const char *path)
{
	return (row_at(fixture, id_of_path(fixture, path)));
}

typedef struct name {
	const char	*bytes;
	size_t		 length;
} name_t;

static int
name_compare(const void *a, const void *b)
{
	const name_t *x = a, *y = b;
	size_t shorter = x->length < y->length ? x->length : y->length;
	int order = memcmp(x->bytes, y->bytes, shorter);

	if (order != 0)
		return (order);
	return ((x->length > y->length) - (x->length < y->length));
}

/*
 * The children of the directory at dir ("" for the root) plus "." and "..",
 * sorted bytewise with a shorter equal prefix first. Returns the count.
 */
static size_t
dir_names(const char *dir, name_t *names)
{
	size_t prefix = strlen(dir), count = 0;

	names[count++] = (name_t){ ".", 1 };
	names[count++] = (name_t){ "..", 2 };
	for (size_t i = 0; i < tree.count; i++) {
		const char *path = tree.specs[i].path;

		if (strncmp(path, dir, prefix) != 0 || path[prefix] != '/' ||
		    strchr(path + prefix + 1, '/') != NULL)
			continue;
		CHECK(count < MAX_NAMES);
		names[count++] = (name_t){ path + prefix + 1,
		    strlen(path + prefix + 1) };
	}
	qsort(names, count, sizeof(names[0]), name_compare);
	return (count);
}

/*
 * The directory's size: entries fill blocks while a block's 12-byte entries
 * and names stay within 4096 bytes; the size is the full blocks plus the
 * last block's used bytes.
 */
static uint64_t
model_dir_size(const char *dir)
{
	name_t names[MAX_NAMES];
	size_t count = dir_names(dir, names);
	uint64_t full = 0, used = 0;

	for (size_t i = 0; i < count; i++) {
		uint64_t need = 12 + names[i].length;

		if (used + need > BLOCK) {
			full++;
			used = 0;
		}
		used += need;
	}
	return (full * BLOCK + used);
}

static uint64_t
model_subdirectories(const char *dir)
{
	size_t prefix = strlen(dir);
	uint64_t count = 0;

	for (size_t i = 0; i < tree.count; i++) {
		const char *path = tree.specs[i].path;

		if (tree.specs[i].kind == FSIMG_ENTRY_DIRECTORY &&
		    strncmp(path, dir, prefix) == 0 && path[prefix] == '/' &&
		    strchr(path + prefix + 1, '/') == NULL)
			count++;
	}
	return (count);
}

typedef struct model {
	erofs_row_t	 rows[MAX_ROWS];
	const spec_t	*specs[MAX_ROWS];
	uint64_t	 count;
	uint64_t	 metadata_end;
	uint64_t	 used_blocks;
} model_t;

static model_t model;

/* One row from the rules for kind, defaults, owner, time and links. */
static erofs_row_t
model_row(const spec_t *spec, int64_t format_time, bool reproducible)
{
	erofs_row_t row;
	uint32_t type, permissions;

	bzero(&row, sizeof(row));
	row.mtime = format_time;
	if (spec == NULL) {
		row.mode = S_IFDIR | 0755;
		row.size = model_dir_size("");
		row.links = 2 + (uint32_t)model_subdirectories("");
		return (row);
	}
	switch (spec->kind) {
	case FSIMG_ENTRY_DIRECTORY:
		type = S_IFDIR;
		permissions = 0755;
		row.size = model_dir_size(spec->path);
		row.links = 2 + (uint32_t)model_subdirectories(spec->path);
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = S_IFLNK;
		permissions = 0755;
		row.size = spec->size;
		row.links = 1;
		break;
	case FSIMG_ENTRY_FIFO:
		type = S_IFIFO;
		permissions = 0644;
		row.links = 1;
		break;
	default:
		type = S_IFREG;
		permissions = 0644;
		row.size = spec->size;
		row.links = 1;
		break;
	}
	row.mode = (uint16_t)(type | (spec->mode != 0 ? spec->mode :
	    permissions));
	if (spec->owned) {
		row.uid = spec->uid;
		row.gid = spec->gid;
	}
	if (spec->stamped && !reproducible) {
		row.mtime = spec->mtime;
		row.mtime_nsec = spec->mtime_nsec;
	}
	return (row);
}

/*
 * The expected plan for the global tree at the writer's catalog order:
 * rows, then inode slots, then data blocks.
 */
static void
model_plan(fixture_t *fixture, int64_t format_time, bool reproducible)
{
	uint64_t slot, next_block;

	bzero(&model, sizeof(model));
	model.count = fixture->writer->count + 1;
	CHECK(model.count == tree.count + 1);
	for (uint64_t id = 0; id < model.count; id++) {
		model.specs[id] = id == 0 ? NULL :
		    &tree.specs[spec_for_id(fixture, id)];
		model.rows[id] = model_row(model.specs[id], format_time,
		    reproducible);
	}

	/* Metadata, in 32-byte slots from slot 36 (byte 1152). */
	slot = 36;
	for (uint64_t id = 0; id < model.count; id++) {
		erofs_row_t *row = &model.rows[id];
		uint32_t fixed, tail, body_slots, span_slots;

		row->extended = row->size > UINT32_MAX || row->uid > 65535 ||
		    row->gid > 65535 || row->links > 65535 ||
		    row->mtime != format_time || row->mtime_nsec != 0;
		fixed = row->extended ? 64 : 32;
		tail = (uint32_t)(row->size % BLOCK);
		row->layout = tail != 0 && fixed + tail <= BLOCK ?
		    EROFS_LAYOUT_FLAT_INLINE : EROFS_LAYOUT_FLAT;
		body_slots = row->layout == EROFS_LAYOUT_FLAT_INLINE ?
		    (fixed + tail + SLOT - 1) / SLOT : fixed / SLOT;
		span_slots = row->layout == EROFS_LAYOUT_FLAT_INLINE ?
		    body_slots : fixed / SLOT;
		if (slot % SLOTS_PER_BLOCK + span_slots > SLOTS_PER_BLOCK)
			slot += SLOTS_PER_BLOCK - slot % SLOTS_PER_BLOCK;
		row->nid = slot;
		slot += body_slots;
	}
	model.metadata_end = (slot + SLOTS_PER_BLOCK - 1) / SLOTS_PER_BLOCK *
	    BLOCK;

	/* Data blocks, contiguous in the same order. */
	next_block = model.metadata_end / BLOCK;
	for (uint64_t id = 0; id < model.count; id++) {
		erofs_row_t *row = &model.rows[id];
		uint64_t blocks = row->size / BLOCK;

		if (row->layout == EROFS_LAYOUT_FLAT && row->size % BLOCK != 0)
			blocks++;
		row->start_block = blocks != 0 ? next_block : 0;
		next_block += blocks;
	}
	model.used_blocks = next_block;
}

/* The first field in which two rows differ, or NULL. */
static const char *
row_difference(const erofs_row_t *got, const erofs_row_t *want)
{
	if (got->size != want->size)
		return ("size");
	if (got->nid != want->nid)
		return ("nid");
	if (got->start_block != want->start_block)
		return ("start_block");
	if (got->mtime != want->mtime)
		return ("mtime");
	if (got->mtime_nsec != want->mtime_nsec)
		return ("mtime_nsec");
	if (got->uid != want->uid)
		return ("uid");
	if (got->gid != want->gid)
		return ("gid");
	if (got->links != want->links)
		return ("links");
	if (got->mode != want->mode)
		return ("mode");
	if (got->layout != want->layout)
		return ("layout");
	if (got->extended != want->extended)
		return ("extended");
	return (NULL);
}

/* Require every row and the volume totals to equal the model. */
static void
check_plan_matches_model(fixture_t *fixture, const char *what)
{
	erofs_plan_t *plan = fixture->plan;

	CHECK_CASE(plan->inode_count == model.count, "%s: inode count",
	    what);
	CHECK_CASE(plan->block_bytes == BLOCK, "%s: block size", what);
	CHECK_CASE(plan->metadata_end == model.metadata_end,
	    "%s: metadata_end %llu, want %llu", what,
	    (unsigned long long)plan->metadata_end,
	    (unsigned long long)model.metadata_end);
	CHECK_CASE(plan->used_blocks == model.used_blocks,
	    "%s: used_blocks %llu, want %llu", what,
	    (unsigned long long)plan->used_blocks,
	    (unsigned long long)model.used_blocks);
	for (uint64_t id = 0; id < model.count; id++) {
		erofs_row_t got = row_at(fixture, id);
		const char *field = row_difference(&got, &model.rows[id]);
		char path[MAX_PATH] = "/";

		if (id != 0)
			catalog_path(fixture, id, path, sizeof(path));
		CHECK_CASE(field == NULL, "%s: id %llu (%s) field %s", what,
		    (unsigned long long)id, path, field);
	}
}

/*
 * Settings and construction.
 */

/*
 * Each of these settings is refused when the writer is constructed, before
 * any entry is added; the refusal names the offending setting in
 * error.path when it is one setting's fault.
 */
typedef struct refusal {
	const char	*what;
	fsimg_status_t	 status;
	const char	*key;
} refusal_t;

static void
check_refused(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    const refusal_t *want)
{
	fsimg_writer_t *writer = NULL;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	status = new_writer(ctx, options, &writer, &error);
	CHECK_CASE(status == want->status, "%s: status %d, want %d",
	    want->what, status, want->status);
	CHECK_CASE(writer == NULL, "%s: a refused writer exists", want->what);
	CHECK_CASE(error.status == want->status, "%s: error status",
	    want->what);
	if (want->key != NULL)
		CHECK_CASE(strcmp(error.path, want->key) == 0,
		    "%s: error.path is \"%s\", want \"%s\"", want->what,
		    error.path, want->key);
}

static void
check_accepted(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    const char *what)
{
	fsimg_writer_t *writer = NULL;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	status = new_writer(ctx, options, &writer, &error);
	CHECK_CASE(status == FSIMG_OK, "%s: refused with %d: %s", what,
	    status, error.message);
	fsimg_writer_free(writer);
}

static void
test_default_settings_plan_an_empty_volume(void)
{
	fixture_t fixture;
	fsimg_write_options_t options;

	tree_reset();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->block_bytes == BLOCK);
	fixture_close(&fixture);
}

static void
test_unwritten_settings_are_refused_naming_the_setting(void)
{
	static const struct {
		fsimg_erofs_compression_t compression;
		uint32_t	 addressing;
		fsimg_erofs_metabox_t metabox;
		refusal_t	 want;
	} cases[] = {
		{ FSIMG_EROFS_COMPRESSION_LZ4, 0, FSIMG_EROFS_METABOX_NONE,
		    { "lz4 compression", FSIMG_EUNSUPPORTED, "compression" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 48, FSIMG_EROFS_METABOX_NONE,
		    { "48-bit addressing", FSIMG_EUNSUPPORTED, "addressing" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 64, FSIMG_EROFS_METABOX_NONE,
		    { "64-bit addressing", FSIMG_EINVAL, "addressing" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 16, FSIMG_EROFS_METABOX_NONE,
		    { "16-bit addressing", FSIMG_EINVAL, "addressing" } },
		{ (fsimg_erofs_compression_t)7, 0, FSIMG_EROFS_METABOX_NONE,
		    { "unknown compression", FSIMG_EINVAL, "compression" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 0,
		    (fsimg_erofs_metabox_t)9,
		    { "unknown metabox", FSIMG_EINVAL, "metabox" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 32, FSIMG_EROFS_METABOX_PLAIN,
		    { "metabox with 32-bit addressing", FSIMG_EINVAL,
		    "metabox" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 0, FSIMG_EROFS_METABOX_LZ4,
		    { "metabox with default addressing", FSIMG_EINVAL,
		    "metabox" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 48, FSIMG_EROFS_METABOX_PLAIN,
		    { "plain metabox with 48-bit addressing",
		    FSIMG_EUNSUPPORTED, NULL } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 48, FSIMG_EROFS_METABOX_LZ4,
		    { "lz4 metabox with 48-bit addressing",
		    FSIMG_EUNSUPPORTED, NULL } }
	};
	fsimg_write_options_t options;
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reproducible_options(&options);
		options.erofs_compression = cases[i].compression;
		options.erofs_addressing = cases[i].addressing;
		options.erofs_metabox = cases[i].metabox;
		check_refused(ctx, &options, &cases[i].want);
	}

	reproducible_options(&options);
	options.erofs_addressing = 32;
	check_accepted(ctx, &options, "explicit 32-bit addressing");
	fsimg_ctx_free(ctx);
}

static void
test_block_size_selects_only_4096(void)
{
	static const struct {
		uint32_t	 bytes;
		fsimg_status_t	 status;
	} cases[] = {
		{ 0, FSIMG_OK }, { 4096, FSIMG_OK },
		{ 512, FSIMG_EUNSUPPORTED }, { 1024, FSIMG_EUNSUPPORTED },
		{ 2048, FSIMG_EUNSUPPORTED },
		{ 8192, FSIMG_EINVAL }, { 3000, FSIMG_EINVAL },
		{ 256, FSIMG_EINVAL }
	};
	fsimg_write_options_t options;
	fsimg_ctx_t *ctx;
	char what[64];

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		refusal_t want = { what, cases[i].status, NULL };

		snprintf(what, sizeof(what), "allocation unit %u",
		    cases[i].bytes);
		reproducible_options(&options);
		options.allocation_unit_bytes = cases[i].bytes;
		if (cases[i].status == FSIMG_OK)
			check_accepted(ctx, &options, what);
		else
			check_refused(ctx, &options, &want);
	}
	fsimg_ctx_free(ctx);
}

static void
test_sector_volume_and_label_geometry(void)
{
	fsimg_write_options_t options;
	refusal_t sector = { "4096-byte sectors", FSIMG_EINVAL, NULL };
	refusal_t unaligned = { "volume of 4097 bytes", FSIMG_EINVAL, NULL };
	refusal_t odd = { "volume of 12288 bytes less one", FSIMG_EINVAL,
	    NULL };
	refusal_t label = { "a 16-byte label", FSIMG_EINVAL, NULL };
	refusal_t beyond = { "a volume past 2^32 blocks", FSIMG_EINVAL,
	    NULL };
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);

	reproducible_options(&options);
	options.sector_bytes = 512;
	check_accepted(ctx, &options, "512-byte sectors");
	options.sector_bytes = 4096;
	check_refused(ctx, &options, &sector);

	reproducible_options(&options);
	options.volume_bytes = 3 * BLOCK;
	check_accepted(ctx, &options, "a volume of three blocks");
	options.volume_bytes = BLOCK + 1;
	check_refused(ctx, &options, &unaligned);
	options.volume_bytes = 3 * BLOCK - 1;
	check_refused(ctx, &options, &odd);
	options.volume_bytes = (UINT64_C(1) << 32) * BLOCK;
	check_refused(ctx, &options, &beyond);

	reproducible_options(&options);
	options.label = "0123456789abcde";
	check_accepted(ctx, &options, "a 15-byte label");
	options.label = "0123456789abcdef";
	check_refused(ctx, &options, &label);
	fsimg_ctx_free(ctx);
}

static void
test_other_formats_refuse_erofs_settings(void)
{
	static const char *const formats[] = { "qnx6", "ifs", "exfat" };
	fsimg_write_options_t options;
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		fsimg_writer_t *writer = NULL;

		fsimg_write_options_init(&options);
		CHECK_CASE(fsimg_writer_new(ctx, formats[i], &options,
		    &writer, NULL) == FSIMG_OK, "%s: defaults", formats[i]);
		fsimg_writer_free(writer);

		fsimg_write_options_init(&options);
		options.erofs_compression = FSIMG_EROFS_COMPRESSION_LZ4;
		CHECK_CASE(fsimg_writer_new(ctx, formats[i], &options,
		    &writer, NULL) == FSIMG_EINVAL, "%s: compression",
		    formats[i]);

		fsimg_write_options_init(&options);
		options.erofs_addressing = 48;
		CHECK_CASE(fsimg_writer_new(ctx, formats[i], &options,
		    &writer, NULL) == FSIMG_EINVAL, "%s: addressing",
		    formats[i]);

		fsimg_write_options_init(&options);
		options.erofs_metabox = FSIMG_EROFS_METABOX_PLAIN;
		CHECK_CASE(fsimg_writer_new(ctx, formats[i], &options,
		    &writer, NULL) == FSIMG_EINVAL, "%s: metabox",
		    formats[i]);
		CHECK(writer == NULL);
	}
	fsimg_ctx_free(ctx);
}

/*
 * A writer constructed with the default settings, which include no label,
 * is accepted: the driver selects the label the common code stores.
 */
static void
test_default_label_is_selected(void)
{
	fsimg_write_options_t options;
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	check_accepted(ctx, &options, "default settings");
	fsimg_ctx_free(ctx);
}

/*
 * Identity and time.
 */

static void
test_volume_uuid_is_required_outside_reproducible_mode(void)
{
	fixture_t fixture;
	fsimg_write_options_t options;

	tree_reset();
	base_options(&options);
	fixture_open(&fixture);
	CHECK(fixture_plan(&fixture, &options, false) == FSIMG_EINVAL);
	CHECK(strcmp(fixture.error.path, "uuid") == 0);
	CHECK(fixture.plan == NULL);
	fixture_close(&fixture);
}

static void
test_given_uuid_is_stored_verbatim(void)
{
	static const unsigned char uuid[16] = { 0x10, 0x32, 0x54, 0x76,
	    0x98, 0xba, 0xdc, 0xfe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
	    0xcd, 0xef };
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	for (unsigned reproducible = 0; reproducible < 2; reproducible++) {
		if (reproducible != 0)
			reproducible_options(&options);
		else
			base_options(&options);
		bcopy(uuid, options.volume_uuid, sizeof(uuid));
		plan_tree(&fixture, &options);
		CHECK_CASE(memcmp(fixture.plan->uuid, uuid, 16) == 0,
		    "reproducible %u", reproducible);
		fixture_close(&fixture);
	}
}

static void
test_reproducible_mode_derives_uuid_and_time_from_epoch(void)
{
	static const int64_t epochs[] = { 0, -1, EPOCH, INT64_C(1) << 40 };
	fsimg_write_options_t options;
	fixture_t fixture;
	unsigned char want[16];

	tree_reset();
	for (size_t i = 0; i < sizeof(epochs) / sizeof(epochs[0]); i++) {
		reproducible_options(&options);
		options.epoch = epochs[i];
		plan_tree(&fixture, &options);
		fsimg_identifier_derive(epochs[i], FSIMG_ROLE_EROFS, 0, want);
		CHECK_CASE(memcmp(fixture.plan->uuid, want, 16) == 0,
		    "epoch %lld: uuid", (long long)epochs[i]);
		CHECK_CASE(fixture.plan->format_time == epochs[i],
		    "epoch %lld: format time", (long long)epochs[i]);
		CHECK_CASE(row_at(&fixture, 0).mtime == epochs[i],
		    "epoch %lld: root time", (long long)epochs[i]);
		fixture_close(&fixture);
	}
}

static void
test_unreproducible_format_time_is_the_host_clock(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	time_t before, after;

	tree_reset();
	base_options(&options);
	options.volume_uuid[0] = 1;
	before = time(NULL);
	plan_tree(&fixture, &options);
	after = time(NULL);
	CHECK(fixture.plan->format_time >= (int64_t)before);
	CHECK(fixture.plan->format_time <= (int64_t)after);
	CHECK(row_at(&fixture, 0).mtime == fixture.plan->format_time);
	fixture_close(&fixture);
}

/*
 * Placement and sizing.
 */

static void
test_empty_catalog_has_only_the_root_inode(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	erofs_row_t root;

	tree_reset();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	root = row_at(&fixture, 0);

	CHECK(root.nid == 36);
	CHECK(root.size == 27);
	CHECK(root.layout == EROFS_LAYOUT_FLAT_INLINE);
	CHECK(!root.extended);
	CHECK(root.start_block == 0);
	CHECK(root.links == 2);
	CHECK(root.mode == (S_IFDIR | 0755));
	CHECK(root.mtime == EPOCH && root.mtime_nsec == 0);
	CHECK(root.uid == 0 && root.gid == 0);
	CHECK(fixture.plan->inode_count == 1);
	CHECK(fixture.plan->metadata_end == BLOCK);
	CHECK(fixture.plan->used_blocks == 1);
	CHECK(fixture.plan->block_count == 1);
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "empty catalog");
	fixture_close(&fixture);
}

/* A tree of every kind, owner, size class and a directory of many names. */
static void
build_mixed_tree(void)
{
	tree_reset();
	add_directory("/bin");
	add_file(100, "/bin/tool");
	add_file(3 * BLOCK + 100, "/bin/big");
	add_file(0, "/bin/empty");
	add_file(2 * BLOCK, "/bin/exact");
	add_file(4065, "/bin/almost-full");
	add_directory("/etc")->mode = 0700;
	with_owner(add_file(70, "/etc/passwd"), 1000, 1000)->mode = 0600;
	with_owner(add_file(20, "/etc/shadow"), 70000, 5);
	tree_add(FSIMG_ENTRY_SYMLINK, 11, "/etc/link");
	tree_add(FSIMG_ENTRY_FIFO, 0, "/etc/pipe")->mode = 0620;
	add_directory("/lib");
	add_directory("/lib/a");
	add_directory("/lib/a/b");
	add_file(5000, "/lib/a/b/c");
	add_file(1, "/lib/a/d");
	add_directory("/many");
	for (unsigned i = 0; i < 150; i++)
		add_file(i * 37, "/many/entry-%03u-0123456789abcdef", i);
}

static void
test_mixed_tree_plan_matches_the_model(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	erofs_row_t many;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "mixed tree");

	/* The big directory holds a full data block and an inline tail. */
	many = row_of_path(&fixture, "/many");
	CHECK(many.size > BLOCK && many.size % BLOCK != 0);
	CHECK(many.layout == EROFS_LAYOUT_FLAT_INLINE);
	CHECK(many.start_block != 0);
	CHECK(fixture.plan->block_count == fixture.plan->used_blocks);
	fixture_close(&fixture);
}

static void
test_start_blocks_follow_catalog_order_contiguously(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	uint64_t next;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);

	next = fixture.plan->metadata_end / BLOCK;
	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);
		uint64_t blocks = row.size / BLOCK;
		char path[MAX_PATH] = "/";

		if (row.layout == EROFS_LAYOUT_FLAT && row.size % BLOCK != 0)
			blocks++;
		if (id != 0)
			catalog_path(&fixture, id, path, sizeof(path));
		if (blocks == 0) {
			CHECK_CASE(row.start_block == 0, "%s: start %llu",
			    path, (unsigned long long)row.start_block);
			continue;
		}
		CHECK_CASE(row.start_block == next, "%s: start %llu, want %llu",
		    path, (unsigned long long)row.start_block,
		    (unsigned long long)next);
		next += blocks;
	}
	CHECK(fixture.plan->used_blocks == next);
	fixture_close(&fixture);
}

static void
test_inodes_are_aligned_in_catalog_order(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	uint64_t previous_end = 36 * SLOT;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);
		uint64_t start = row.nid * SLOT;

		CHECK_CASE(start >= previous_end, "id %llu overlaps its "
		    "predecessor", (unsigned long long)id);
		previous_end = start + erofs_row_fixed_bytes(&row) +
		    erofs_row_tail_bytes(&row, BLOCK);
		CHECK_CASE(previous_end <= fixture.plan->metadata_end,
		    "id %llu ends past the metadata area",
		    (unsigned long long)id);
	}
	fixture_close(&fixture);
}

static void
test_link_count_is_two_plus_subdirectories(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	add_directory("/a");
	add_directory("/a/b");
	add_directory("/a/c");
	add_directory("/a/c/d");
	add_file(1, "/a/f");
	add_directory("/e");
	add_file(1, "/e/g");
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);
		char path[MAX_PATH] = "/";
		uint32_t want = 1;

		if (id != 0)
			catalog_path(&fixture, id, path, sizeof(path));
		if (strcmp(path, "/") == 0)
			want = 4;
		else if (strcmp(path, "/a") == 0)
			want = 4;
		else if (strcmp(path, "/a/c") == 0)
			want = 3;
		else if (strcmp(path, "/a/b") == 0 ||
		    strcmp(path, "/a/c/d") == 0 || strcmp(path, "/e") == 0)
			want = 2;
		CHECK_CASE(row.links == want, "%s: links %u, want %u", path,
		    row.links, want);
	}
	fixture_close(&fixture);
}

static void
test_default_modes_and_given_modes(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	add_directory("/d");
	add_directory("/d0700")->mode = 0700;
	add_file(1, "/f");
	add_file(1, "/f4755")->mode = 04755;
	tree_add(FSIMG_ENTRY_SYMLINK, 3, "/l");
	tree_add(FSIMG_ENTRY_SYMLINK, 3, "/l0600")->mode = 0600;
	tree_add(FSIMG_ENTRY_FIFO, 0, "/p");
	tree_add(FSIMG_ENTRY_FIFO, 0, "/p0660")->mode = 0660;
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "modes");
	for (uint64_t id = 1; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);
		char path[MAX_PATH];
		uint16_t want;

		catalog_path(&fixture, id, path, sizeof(path));
		if (strcmp(path, "/d") == 0 || strcmp(path, "/l") == 0)
			want = 0755;
		else if (strcmp(path, "/f") == 0 || strcmp(path, "/p") == 0)
			want = 0644;
		else if (strcmp(path, "/d0700") == 0)
			want = 0700;
		else if (strcmp(path, "/f4755") == 0)
			want = 04755;
		else if (strcmp(path, "/l0600") == 0)
			want = 0600;
		else
			want = 0660;
		CHECK_CASE((row.mode & 07777) == want, "%s: mode %o, want %o",
		    path, row.mode & 07777, want);
	}
	fixture_close(&fixture);
}

/*
 * The flat-inline rule: a nonzero tail t stays in the body when the fixed
 * part and t fit one block. A compact inode's limit is t = 4064 and an
 * extended inode's t = 4032; a multiple of 4096, zero included, is flat.
 */
static void
test_tail_layout_follows_the_block_fit(void)
{
	static const struct {
		const char	*path;
		uint64_t	 size;
		bool		 extended;
		erofs_layout_t	 layout;
	} cases[] = {
		{ "/empty", 0, false, EROFS_LAYOUT_FLAT },
		{ "/one-block", 4096, false, EROFS_LAYOUT_FLAT },
		{ "/two-blocks", 8192, false, EROFS_LAYOUT_FLAT },
		{ "/tail-1", 1, false, EROFS_LAYOUT_FLAT_INLINE },
		{ "/tail-4064", 4064, false, EROFS_LAYOUT_FLAT_INLINE },
		{ "/tail-4065", 4065, false, EROFS_LAYOUT_FLAT },
		{ "/tail-4095", 4095, false, EROFS_LAYOUT_FLAT },
		{ "/block-and-tail-4064", 4096 + 4064, false,
		    EROFS_LAYOUT_FLAT_INLINE },
		{ "/block-and-tail-4065", 4096 + 4065, false,
		    EROFS_LAYOUT_FLAT },
		{ "/wide-empty", 0, true, EROFS_LAYOUT_FLAT },
		{ "/wide-tail-4032", 4032, true, EROFS_LAYOUT_FLAT_INLINE },
		{ "/wide-tail-4033", 4033, true, EROFS_LAYOUT_FLAT },
		{ "/wide-tail-100", 100, true, EROFS_LAYOUT_FLAT_INLINE }
	};
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		spec_t *spec = add_file(cases[i].size, "%s", cases[i].path);

		if (cases[i].extended)
			with_owner(spec, 70000, 0);
	}
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		erofs_row_t row = row_of_path(&fixture, cases[i].path);

		CHECK_CASE(row.extended == cases[i].extended,
		    "%s: extended %d", cases[i].path, row.extended);
		CHECK_CASE(row.layout == cases[i].layout, "%s: layout %d",
		    cases[i].path, row.layout);
	}
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "tail layouts");
	fixture_close(&fixture);
}

/*
 * An inode with an inline tail lies inside one block; a tail-free inode
 * keeps only its fixed part inside one. The sweep adds k FIFOs and then one
 * final inode, so the final inode's position moves across the end of block
 * 0 as k grows; every plan is compared to the model, and the sweep requires
 * that it saw the cases the rule names:
 *   moved	the inode would have crossed, so it starts the next block
 *   fits	the inode ends exactly at the block's end and stays
 */
typedef struct sweep_seen {
	unsigned	 moved;
	unsigned	 fits;
	unsigned	 compact_in_last_slot;
} sweep_seen_t;

static void
sweep_final_inode(const char *what, uint64_t size, bool extended,
    fsimg_entry_kind_t kind, bool pad, sweep_seen_t *seen)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	erofs_row_t last, before;
	uint64_t last_id, start, end;
	char label[96];

	reproducible_options(&options);
	for (unsigned k = 0; k <= 110; k++) {
		tree_reset();
		for (unsigned i = 0; i < k; i++)
			tree_add(FSIMG_ENTRY_FIFO, 0, "/f%03u", i);
		if (pad)
			add_file(1, "/g");
		if (kind == FSIMG_ENTRY_FIFO)
			tree_add(kind, 0, "/zz");
		else
			add_file(size, "/zz");
		if (extended)
			with_owner(&tree.specs[tree.count - 1], 70000, 0);
		snprintf(label, sizeof(label), "%s, %u FIFOs", what, k);

		plan_tree(&fixture, &options);
		model_plan(&fixture, EPOCH, true);
		check_plan_matches_model(&fixture, label);

		last_id = fixture.writer->count;
		last = row_at(&fixture, last_id);
		before = row_at(&fixture, last_id - 1);
		start = last.nid * SLOT;
		end = start + erofs_row_fixed_bytes(&last) +
		    erofs_row_tail_bytes(&last, BLOCK);
		CHECK_CASE(start / BLOCK == (end - 1) / BLOCK,
		    "%s: the inode crosses a block", label);
		if (start % BLOCK == 0 && start != 0 &&
		    before.nid * SLOT + erofs_row_fixed_bytes(&before) +
		    erofs_row_tail_bytes(&before, BLOCK) < start)
			seen->moved++;
		if (end % BLOCK == 0)
			seen->fits++;
		if (start % BLOCK == BLOCK - SLOT &&
		    erofs_row_fixed_bytes(&last) == SLOT)
			seen->compact_in_last_slot++;
		fixture_close(&fixture);
	}
}

static void
test_inline_tail_never_crosses_a_block(void)
{
	static const uint64_t sizes[] = { 32, 64, 96, 100, 200, 1000 };
	sweep_seen_t seen = { 0, 0, 0 };
	char what[48];

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(what, sizeof(what), "file of %llu bytes",
		    (unsigned long long)sizes[i]);
		sweep_final_inode(what, sizes[i], false, FSIMG_ENTRY_FILE,
		    false, &seen);
	}
	CHECK(seen.moved > 0);
	CHECK(seen.fits > 0);
}

static void
test_extended_inode_moves_when_its_fixed_part_would_cross(void)
{
	sweep_seen_t seen = { 0, 0, 0 };

	sweep_final_inode("extended FIFO", 0, true, FSIMG_ENTRY_FIFO, false,
	    &seen);
	sweep_final_inode("extended FIFO after a file", 0, true,
	    FSIMG_ENTRY_FIFO, true, &seen);
	CHECK(seen.moved > 0);
}

static void
test_compact_inode_may_use_a_blocks_last_slot(void)
{
	sweep_seen_t seen = { 0, 0, 0 };

	sweep_final_inode("compact FIFO", 0, false, FSIMG_ENTRY_FIFO, false,
	    &seen);
	sweep_final_inode("compact FIFO after a file", 0, false,
	    FSIMG_ENTRY_FIFO, true, &seen);
	CHECK(seen.compact_in_last_slot > 0);
}

/*
 * An inode is extended exactly when a compact one cannot hold it: a size
 * over 32 bits, an owner over 16 bits, a link count over 16 bits, or a
 * time that is not the format time to the nanosecond.
 */
static void
test_extended_form_triggers(void)
{
	static const struct {
		const char	*path;
		uint64_t	 size;
		uint32_t	 uid;
		uint32_t	 gid;
		bool		 extended;
	} cases[] = {
		{ "/size-32-bit-max", UINT32_MAX, 0, 0, false },
		{ "/size-over-32-bits", UINT64_C(1) << 32, 0, 0, true },
		{ "/size-5-gib", UINT64_C(5) << 30, 0, 0, true },
		{ "/uid-65535", 10, 65535, 0, false },
		{ "/uid-65536", 10, 65536, 0, true },
		{ "/gid-65535", 10, 0, 65535, false },
		{ "/gid-65536", 10, 0, 65536, true },
		{ "/uid-and-gid-max", 10, UINT32_MAX, UINT32_MAX, true }
	};
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		spec_t *spec = add_file(cases[i].size, "%s", cases[i].path);

		with_owner(spec, cases[i].uid, cases[i].gid);
	}
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		erofs_row_t row = row_of_path(&fixture, cases[i].path);

		CHECK_CASE(row.extended == cases[i].extended,
		    "%s: extended %d", cases[i].path, row.extended);
		CHECK_CASE(row.uid == cases[i].uid &&
		    row.gid == cases[i].gid, "%s: owner", cases[i].path);
		CHECK_CASE(row.size == cases[i].size, "%s: size",
		    cases[i].path);
	}
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "extended triggers");
	fixture_close(&fixture);
}

static void
test_modification_time_is_compared_to_the_nanosecond(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	base_options(&options);
	options.volume_uuid[0] = 1;
	for (unsigned attempt = 0; attempt < 5; attempt++) {
		int64_t now = (int64_t)time(NULL);
		erofs_row_t row;

		tree_reset();
		with_time(add_file(10, "/at-format-time"), now, 0);
		with_time(add_file(10, "/a-nanosecond-later"), now, 1);
		with_time(add_file(10, "/long-ago"), OLD_TIME, 0);
		add_file(10, "/unstamped");
		plan_tree(&fixture, &options);
		if (fixture.plan->format_time != now) {
			/* The clock ticked between sampling and planning. */
			fixture_close(&fixture);
			continue;
		}

		row = row_of_path(&fixture, "/at-format-time");
		CHECK(!row.extended && row.mtime == now && row.mtime_nsec == 0);
		row = row_of_path(&fixture, "/a-nanosecond-later");
		CHECK(row.extended && row.mtime == now && row.mtime_nsec == 1);
		row = row_of_path(&fixture, "/long-ago");
		CHECK(row.extended && row.mtime == OLD_TIME &&
		    row.mtime_nsec == 0);
		row = row_of_path(&fixture, "/unstamped");
		CHECK(!row.extended && row.mtime == now && row.mtime_nsec == 0);
		model_plan(&fixture, now, false);
		check_plan_matches_model(&fixture, "stamped times");
		fixture_close(&fixture);
		return;
	}
	CHECK_CASE(false, "the clock ticked during every attempt");
}

static void
test_reproducible_mode_stores_the_epoch_in_every_inode(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	tree_reset();
	with_time(add_file(10, "/stamped"), OLD_TIME, 5);
	with_time(add_directory("/stamped-dir"), OLD_TIME, 0);
	with_owner(add_file(10, "/owned"), 1000, 100);
	add_file(10, "/plain");
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);

		CHECK_CASE(!row.extended && row.mtime == EPOCH &&
		    row.mtime_nsec == 0, "id %llu", (unsigned long long)id);
	}
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "reproducible times");
	fixture_close(&fixture);
}

/*
 * A volume larger than the plan keeps the plan's rows and adds zero blocks;
 * a smaller one is refused.
 */
static void
test_volume_size_sets_the_block_count(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	uint64_t used;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	used = fixture.plan->used_blocks;
	CHECK(used > 1);
	fixture_close(&fixture);

	options.volume_bytes = (used + 7) * BLOCK;
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->used_blocks == used);
	CHECK(fixture.plan->block_count == used + 7);
	fixture_close(&fixture);

	options.volume_bytes = used * BLOCK;
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->block_count == used);
	fixture_close(&fixture);

	options.volume_bytes = (used - 1) * BLOCK;
	fixture_open(&fixture);
	CHECK(fixture_plan(&fixture, &options, false) == FSIMG_ERANGE);
	fixture_close(&fixture);
}

static void
test_names_are_bounded_in_bytes(void)
{
	static const char snowman[] = "\xe2\x98\x83";
	static const char eacute[] = "\xc3\xa9";
	fsimg_write_options_t options;
	fixture_t fixture;
	char name[1024];
	size_t length;
	fsimg_status_t status;

	reproducible_options(&options);

	/* Accepted: 255 ASCII bytes, 85 three-byte characters. */
	tree_reset();
	memset(name, 'a', 255);
	name[255] = '\0';
	add_file(1, "/%s", name);
	for (length = 0; length < 85; length++)
		memcpy(name + 3 * length, snowman, 3);
	name[255] = '\0';
	add_file(1, "/%s", name);
	plan_tree(&fixture, &options);
	model_plan(&fixture, EPOCH, true);
	check_plan_matches_model(&fixture, "255-byte names");
	fixture_close(&fixture);

	/*
	 * Refused: 256 ASCII bytes; 128 two-byte characters and 86 three-byte
	 * ones, which are within 255 UTF-16 units but over 255 bytes.
	 */
	for (unsigned which = 0; which < 3; which++) {
		tree_reset();
		length = 0;
		if (which == 0) {
			memset(name, 'a', 256);
			length = 256;
		} else {
			for (unsigned i = 0; i < (which == 1 ? 128u : 86u); i++) {
				memcpy(name + length, which == 1 ? eacute :
				    snowman, which == 1 ? 2 : 3);
				length += which == 1 ? 2 : 3;
			}
		}
		name[length] = '\0';
		add_file(1, "/%s", name);
		fixture_open(&fixture);
		status = fixture_plan(&fixture, &options, false);
		CHECK_CASE(status == FSIMG_EINVAL, "%zu-byte name (%u): "
		    "status %d", length, which, status);
		if (which != 0)
			CHECK_CASE(strstr(fixture.error.message,
			    "255 bytes") != NULL, "%zu-byte name: refused "
			    "by the catalog, not the byte limit: %s", length,
			    fixture.error.message);
		fixture_close(&fixture);
	}
}

static void
test_insertion_order_does_not_change_the_plan(void)
{
	fsimg_write_options_t options;
	fixture_t forward, backward;

	build_mixed_tree();
	reproducible_options(&options);
	fixture_open(&forward);
	CHECK(fixture_plan(&forward, &options, false) == FSIMG_OK);
	fixture_open(&backward);
	CHECK(fixture_plan(&backward, &options, true) == FSIMG_OK);

	CHECK(forward.writer->count == backward.writer->count);
	CHECK(forward.plan->metadata_end == backward.plan->metadata_end);
	CHECK(forward.plan->used_blocks == backward.plan->used_blocks);
	CHECK(memcmp(forward.plan->uuid, backward.plan->uuid, 16) == 0);
	for (uint64_t id = 0; id <= forward.writer->count; id++) {
		erofs_row_t a = row_at(&forward, id);
		erofs_row_t b = row_at(&backward, id);
		const char *field = row_difference(&a, &b);

		CHECK_CASE(field == NULL, "id %llu: field %s",
		    (unsigned long long)id, field);
	}
	fixture_close(&forward);
	fixture_close(&backward);
}

/*
 * Directory packing.
 */

typedef struct pack_result {
	unsigned char	 data[MAX_BLOCKS][BLOCK];
	size_t		 used[MAX_BLOCKS];
	unsigned	 blocks;
	uint64_t	 size_before_last_close;
	uint64_t	 closed_before_last_close;
} pack_result_t;

static pack_result_t packed;
static unsigned char pack_buffer[2 * BLOCK];
static char name_bytes[MAX_NAMES][256];

/* A name of length bytes, distinct by index in its first four bytes. */
static void
fill_name(char *name, size_t length, unsigned index)
{
	for (size_t j = 0; j < length; j++)
		name[j] = j < 4 ? (char)('0' + (index / (j == 0 ? 1000 :
		    j == 1 ? 100 : j == 2 ? 10 : 1)) % 10) :
		    (char)('a' + j % 26);
}

static void
store_block(unsigned index, size_t used, unsigned block_bytes)
{
	CHECK(index < MAX_BLOCKS);
	bcopy(pack_buffer, packed.data[index], block_bytes);
	packed.used[index] = used;
}

/*
 * Pack entries of the given name lengths into blocks of block_bytes the way
 * the walker does: close the open block when an entry does not fit. Entry i
 * has NID 0x123456789 + i and file type 1 + i % 7.
 */
static void
pack_lengths(uint32_t block_bytes, const size_t *lengths, size_t count,
    erofs_dir_entry_t *entries)
{
	erofs_dir_pack_t pack;

	CHECK(count <= MAX_NAMES);
	bzero(&packed, sizeof(packed));
	memset(pack_buffer, 0xa5, sizeof(pack_buffer));
	erofs_dir_pack_init(&pack, block_bytes, pack_buffer);
	for (size_t i = 0; i < count; i++) {
		fill_name(name_bytes[i], lengths[i], (unsigned)i);
		entries[i] = (erofs_dir_entry_t){ .name = name_bytes[i],
		    .name_length = lengths[i],
		    .nid = UINT64_C(0x123456789) + i,
		    .file_type = (uint8_t)(1 + i % 7) };
		if (!erofs_dir_pack_fits(&pack, lengths[i])) {
			store_block(packed.blocks, erofs_dir_pack_close(&pack),
			    block_bytes);
			packed.blocks++;
		}
		CHECK(erofs_dir_pack_fits(&pack, lengths[i]));
		erofs_dir_pack_add(&pack, &entries[i]);
	}
	packed.size_before_last_close = erofs_dir_pack_size(&pack);
	packed.closed_before_last_close = pack.closed;
	store_block(packed.blocks, erofs_dir_pack_close(&pack), block_bytes);
	packed.blocks++;
}

/*
 * Every block decodes under the reader's rules (B bytes valid, the last
 * block its used length), the entries come back in order with their NIDs,
 * file types and names, and the bytes past each block's used length are
 * zero.
 */
static void
check_packed_entries(const char *what, uint32_t block_bytes,
    const erofs_dir_entry_t *entries, size_t count)
{
	size_t next = 0;

	for (unsigned b = 0; b < packed.blocks; b++) {
		size_t valid = b + 1 == packed.blocks ? packed.used[b] :
		    block_bytes;
		erofs_dirblock_t block;
		const char *why;

		CHECK_CASE(erofs_dirblock_decode(packed.data[b], valid,
		    &block, &why) == FSIMG_OK, "%s: block %u: %s", what, b,
		    why);
		for (size_t i = 0; i < block.count; i++) {
			erofs_dirent_t dirent;

			CHECK_CASE(next < count, "%s: extra entry", what);
			CHECK_CASE(erofs_dirent_decode(&block, i, &dirent,
			    &why) == FSIMG_OK, "%s: block %u entry %zu: %s",
			    what, b, i, why);
			CHECK_CASE(dirent.nid == entries[next].nid &&
			    dirent.file_type == entries[next].file_type,
			    "%s: entry %zu nid or type", what, next);
			CHECK_CASE(dirent.name_length ==
			    entries[next].name_length &&
			    memcmp(packed.data[b] + dirent.name_offset,
			    entries[next].name, dirent.name_length) == 0,
			    "%s: entry %zu name", what, next);
			next++;
		}
		for (size_t j = packed.used[b]; j < block_bytes; j++)
			CHECK_CASE(packed.data[b][j] == 0, "%s: block %u "
			    "byte %zu past the used length is not zero", what,
			    b, j);
	}
	CHECK_CASE(next == count, "%s: %zu entries decoded, want %zu", what,
	    next, count);
}

static void
check_pack_shape(const char *what, uint32_t block_bytes,
    const size_t *want_used, unsigned want_blocks)
{
	uint64_t total = 0;

	CHECK_CASE(packed.blocks == want_blocks, "%s: %u blocks, want %u",
	    what, packed.blocks, want_blocks);
	for (unsigned b = 0; b < want_blocks; b++)
		CHECK_CASE(packed.used[b] == want_used[b], "%s: block %u "
		    "used %zu, want %zu", what, b, packed.used[b],
		    want_used[b]);
	CHECK_CASE(packed.size_before_last_close ==
	    packed.closed_before_last_close * block_bytes +
	    want_used[want_blocks - 1], "%s: size", what);
	for (unsigned b = 0; b + 1 < want_blocks; b++)
		total += block_bytes;
	CHECK_CASE(packed.size_before_last_close ==
	    total + want_used[want_blocks - 1], "%s: size from shape", what);
}

/*
 * Entries of 244-byte names take 256 bytes each, so B / 256 of them fill a
 * block exactly. One more byte, in a longer name or in a following entry,
 * opens the next block; one byte less leaves the block short of the entry
 * that follows.
 */
static void
test_directory_blocks_fill_exactly_and_overflow_by_one_byte(void)
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };
	erofs_dir_entry_t entries[MAX_NAMES];
	size_t lengths[MAX_NAMES];
	size_t used[4];
	char what[64];

	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		uint32_t b = sizes[s];
		size_t n = b / 256;

		for (size_t i = 0; i < n; i++)
			lengths[i] = 244;
		snprintf(what, sizeof(what), "B=%u exact", b);
		pack_lengths(b, lengths, n, entries);
		used[0] = b;
		check_pack_shape(what, b, used, 1);
		check_packed_entries(what, b, entries, n);

		lengths[n] = 1;
		snprintf(what, sizeof(what), "B=%u exact then 1 byte", b);
		pack_lengths(b, lengths, n + 1, entries);
		used[0] = b;
		used[1] = 13;
		check_pack_shape(what, b, used, 2);
		check_packed_entries(what, b, entries, n + 1);

		lengths[n - 1] = 245;
		snprintf(what, sizeof(what), "B=%u last name one longer", b);
		pack_lengths(b, lengths, n, entries);
		used[0] = b - 256;
		used[1] = 12 + 245;
		check_pack_shape(what, b, used, 2);
		check_packed_entries(what, b, entries, n);

		lengths[n - 1] = 243;
		lengths[n] = 1;
		snprintf(what, sizeof(what), "B=%u one byte short", b);
		pack_lengths(b, lengths, n + 1, entries);
		used[0] = b - 1;
		used[1] = 13;
		check_pack_shape(what, b, used, 2);
		check_packed_entries(what, b, entries, n + 1);
	}
}

static void
test_directory_blocks_hold_255_byte_names(void)
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };
	erofs_dir_entry_t entries[MAX_NAMES];
	size_t lengths[MAX_NAMES];
	char what[64];

	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		uint32_t b = sizes[s];
		unsigned per_block = b / (12 + 255);
		unsigned want_blocks = (40 + per_block - 1) / per_block;
		size_t used[MAX_BLOCKS];

		for (size_t i = 0; i < 40; i++)
			lengths[i] = 255;
		snprintf(what, sizeof(what), "B=%u 255-byte names", b);
		pack_lengths(b, lengths, 40, entries);
		for (unsigned i = 0; i < want_blocks; i++)
			used[i] = per_block * (12 + 255);
		used[want_blocks - 1] = (40 - (want_blocks - 1) * per_block) *
		    (12 + 255);
		check_pack_shape(what, b, used, want_blocks);
		check_packed_entries(what, b, entries, 40);
	}
}

static void
test_directory_blocks_mix_name_lengths(void)
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };
	erofs_dir_entry_t entries[MAX_NAMES];
	size_t lengths[MAX_NAMES];
	char what[64];

	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		uint32_t b = sizes[s];

		for (size_t i = 0; i < 300; i++)
			lengths[i] = 1 + (i * 37) % 255;
		snprintf(what, sizeof(what), "B=%u mixed lengths", b);
		pack_lengths(b, lengths, 300, entries);
		check_packed_entries(what, b, entries, 300);
		CHECK_CASE(packed.size_before_last_close ==
		    (uint64_t)(packed.blocks - 1) * b +
		    packed.used[packed.blocks - 1], "%s: size", what);
	}
}

/*
 * Walking a planned directory.
 */

typedef struct walk_entry {
	char		 name[256];
	size_t		 length;
	uint64_t	 nid;
	uint8_t		 file_type;
} walk_entry_t;

typedef struct walk {
	walk_entry_t	 entries[MAX_NAMES];
	size_t		 count;
	uint64_t	 blocks;
	size_t		 last_valid;
} walk_t;

static walk_t walked;

/* The recognisable NID and type a resolved catalog ID receives. */
#define WALK_NID(id)	((id) + 1000)
#define WALK_TYPE(id)	((uint8_t)(1 + (id) % 7))

static fsimg_status_t
walk_resolve(void *argument FSIMG_UNUSED, uint64_t id,
    erofs_dir_entry_t *entry, fsimg_error_t *error FSIMG_UNUSED)
{
	entry->nid = WALK_NID(id);
	entry->file_type = WALK_TYPE(id);
	return (FSIMG_OK);
}

static fsimg_status_t
walk_block(void *argument, uint64_t index, const unsigned char *bytes,
    size_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	walk_t *walk = argument;
	erofs_dirblock_t block;
	const char *why;

	CHECK(index == walk->blocks);
	walk->blocks++;
	walk->last_valid = length;
	CHECK_CASE(erofs_dirblock_decode(bytes, length, &block, &why) ==
	    FSIMG_OK, "block %llu: %s", (unsigned long long)index, why);
	for (size_t j = length; j < BLOCK; j++)
		CHECK_CASE(bytes[j] == 0, "block %llu byte %zu past the "
		    "valid length is not zero", (unsigned long long)index, j);
	for (size_t i = 0; i < block.count; i++) {
		erofs_dirent_t dirent;
		walk_entry_t *entry;

		CHECK(erofs_dirent_decode(&block, i, &dirent, &why) ==
		    FSIMG_OK);
		CHECK(walk->count < MAX_NAMES);
		entry = &walk->entries[walk->count++];
		entry->length = dirent.name_length;
		memcpy(entry->name, bytes + dirent.name_offset,
		    dirent.name_length);
		entry->nid = dirent.nid;
		entry->file_type = dirent.file_type;
	}
	return (FSIMG_OK);
}

/* Walk a directory, delivering its blocks into walked; return its size. */
static uint64_t
walk_directory(fixture_t *fixture, uint64_t *cursor, uint64_t id,
    uint64_t parent_id)
{
	const erofs_dir_builder_t builder = {
		.resolve = walk_resolve,
		.block = walk_block,
		.argument = &walked,
		.pack_buffer = pack_buffer
	};
	fsimg_error_t error;
	uint64_t size;

	bzero(&walked, sizeof(walked));
	CHECK(erofs_dir_walk(fixture->plan, cursor, id, parent_id, &builder,
	    &size, &error) == FSIMG_OK);
	return (size);
}

/*
 * The walk's entries must be the model's sorted names, strictly increasing
 * bytewise across blocks, with "." and ".." resolved to the directory and
 * its parent.
 */
static void
check_walked_names(const char *what, const char *dir, uint64_t id,
    uint64_t parent_id)
{
	name_t names[MAX_NAMES];
	size_t count = dir_names(dir, names);

	CHECK_CASE(walked.count == count, "%s: %zu entries, want %zu", what,
	    walked.count, count);
	for (size_t i = 0; i < count; i++) {
		const walk_entry_t *entry = &walked.entries[i];
		name_t got = { entry->name, entry->length };

		CHECK_CASE(name_compare(&got, &names[i]) == 0,
		    "%s: entry %zu is %.*s, want %.*s", what, i,
		    (int)entry->length, entry->name, (int)names[i].length,
		    names[i].bytes);
		if (i > 0) {
			name_t previous = { walked.entries[i - 1].name,
			    walked.entries[i - 1].length };

			CHECK_CASE(name_compare(&previous, &got) < 0,
			    "%s: entry %zu does not increase", what, i);
		}
		if (names[i].length == 1 && names[i].bytes[0] == '.')
			CHECK_CASE(entry->nid == WALK_NID(id),
			    "%s: \".\" nid", what);
		else if (names[i].length == 2 &&
		    memcmp(names[i].bytes, "..", 2) == 0)
			CHECK_CASE(entry->nid == WALK_NID(parent_id),
			    "%s: \"..\" nid", what);
		CHECK_CASE(entry->file_type == WALK_TYPE(entry->nid - 1000),
		    "%s: entry %zu type", what, i);
	}
}

static void
test_directory_walk_merges_dots_across_blocks(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	uint64_t cursor = 0, size, zz;
	erofs_row_t root;
	char long_name[256];

	/*
	 * ID 1 sorts first, so it is the directory the root's walk leaves the
	 * cursor in front of. Names around "." and "..": "!x" and "-a" sort
	 * before ".", ".-" between "." and "..", ".a" after "..".
	 */
	tree_reset();
	add_directory("/!x");
	add_file(1, "/!x/p");
	add_file(1, "/!x/.b");
	add_file(1, "/!x/-c");
	add_file(1, "/-a");
	add_file(1, "/.-");
	add_file(1, "/.a");
	add_directory("/zz");
	add_file(1, "/zz/inner");
	memset(long_name, 'q', 200);
	for (unsigned i = 0; i < 60; i++) {
		long_name[200] = '\0';
		add_file(1, "/n%03u%s", i, long_name);
	}
	reproducible_options(&options);
	plan_tree(&fixture, &options);

	CHECK(spec_for_id(&fixture, 1) == 0);
	root = row_at(&fixture, 0);
	size = walk_directory(&fixture, &cursor, 0, 0);
	CHECK(root.size == size);
	CHECK(size == model_dir_size(""));
	CHECK(walked.blocks == (size + BLOCK - 1) / BLOCK);
	CHECK(walked.blocks > 2);
	CHECK(walked.last_valid == size - (walked.blocks - 1) * BLOCK);
	check_walked_names("root", "", 0, 0);

	/* The next walk starts at directory 1's first child. */
	size = walk_directory(&fixture, &cursor, 1, 0);
	CHECK(row_at(&fixture, 1).size == size);
	check_walked_names("/!x", "/!x", 1, 0);

	/* The one after that starts at directory "/zz"'s. */
	zz = id_of_path(&fixture, "/zz");
	size = walk_directory(&fixture, &cursor, zz, 0);
	CHECK(row_at(&fixture, zz).size == size);
	check_walked_names("/zz", "/zz", zz, 0);
	fixture_close(&fixture);
}

int
main(void)
{
	test_default_settings_plan_an_empty_volume();
	test_unwritten_settings_are_refused_naming_the_setting();
	test_block_size_selects_only_4096();
	test_sector_volume_and_label_geometry();
	test_other_formats_refuse_erofs_settings();
	test_default_label_is_selected();

	test_volume_uuid_is_required_outside_reproducible_mode();
	test_given_uuid_is_stored_verbatim();
	test_reproducible_mode_derives_uuid_and_time_from_epoch();
	test_unreproducible_format_time_is_the_host_clock();

	test_empty_catalog_has_only_the_root_inode();
	test_mixed_tree_plan_matches_the_model();
	test_start_blocks_follow_catalog_order_contiguously();
	test_inodes_are_aligned_in_catalog_order();
	test_link_count_is_two_plus_subdirectories();
	test_default_modes_and_given_modes();
	test_tail_layout_follows_the_block_fit();
	test_inline_tail_never_crosses_a_block();
	test_extended_inode_moves_when_its_fixed_part_would_cross();
	test_compact_inode_may_use_a_blocks_last_slot();
	test_extended_form_triggers();
	test_modification_time_is_compared_to_the_nanosecond();
	test_reproducible_mode_stores_the_epoch_in_every_inode();
	test_volume_size_sets_the_block_count();
	test_names_are_bounded_in_bytes();
	test_insertion_order_does_not_change_the_plan();

	test_directory_blocks_fill_exactly_and_overflow_by_one_byte();
	test_directory_blocks_hold_255_byte_names();
	test_directory_blocks_mix_name_lengths();
	test_directory_walk_merges_dots_across_blocks();
	return (0);
}
