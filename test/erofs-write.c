/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "codec.h"
#include "fsimg.h"
#include "format.h"
#include "fsimg-endian.h"
#include "internal.h"
#include "layer.h"
#include "meta.h"
#include "test.h"
#include "write-erofs.h"
#include "writer.h"

/*
 * EROFS writer tests. A plan is checked against an independent model of
 * the layout rules in specs/design-erofs.md ("Layout", "Inodes",
 * "Directories", "Metadata and defaults"). The model works from the test's
 * own description of the tree, in 32-byte slots and with its own directory
 * packer, and shares no code with lib/formats/erofs/write-plan.c. It takes
 * only the catalog's ID order from the writer, since IDs are the catalog's
 * to assign.
 *
 * Directory packing is also driven directly, and every block it produces is
 * decoded with the reader's decoders.
 *
 * Emission is checked from the image it produces. Every entry's content
 * comes from a provider that serves a pattern of the entry and the offset,
 * or a link's target. The image is read back through the registered
 * reader, which enumerates it, compares each entry with its spec, verifies
 * it and extracts a small subtree. Reproducible images are compared byte
 * for byte across insertion order, memory and scratch limits and time
 * zones. A poisoned sink shows that every byte outside the superblock,
 * the inode bodies and the data blocks the plan describes is written
 * zero, and that no byte inside them is left unwritten. Failures of
 * allocation, write and flush, and a source that changed after planning,
 * fail the emission without leaking. The inode and superblock encoders
 * are checked against the reader's decoders.
 */
#define BLOCK_DEFAULT	4096u
#define BLOCK_MAX	4096u
#define SLOT		32u
#define SLOTS_PER_BLOCK	(block_size / SLOT)
#define EPOCH		INT64_C(1700000000)
#define OLD_TIME	INT64_C(1234567890)
#define MAX_SPECS	512u
#define MAX_PATH	320u
#define MAX_ROWS	(MAX_SPECS + 1)
#define MAX_NAMES	1024u
#define MAX_BLOCKS	128u

/*
 * The block size the tests plan at, which the model and the checks
 * expect: the writer's default, except while at_every_block_size() runs a
 * test at each size the writer accepts and sets sweeping.
 */
static uint32_t block_size = BLOCK_DEFAULT;
static bool sweeping;

/* The compression setting base_options() selects; the LZ4 tests set it. */
static fsimg_erofs_compression_t compression = FSIMG_EROFS_COMPRESSION_NONE;
/* The addressing base_options() selects; the 48-bit tests set it. */
static uint32_t addressing;
/* The metadata box base_options() selects; the box tests set it. */
static fsimg_erofs_metabox_t metabox = FSIMG_EROFS_METABOX_NONE;

/* Report the case before the fatal check names the failed condition. */
#define CHECK_CASE(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
	CHECK(condition); \
} while (0)

/*
 * What a file's bytes look like (spec_byte()): the default pattern,
 * repetitive text LZ4 compresses well, noise it cannot compress, or text
 * for the first half and noise after it.
 */
typedef enum content {
	CONTENT_PATTERN = 0,
	CONTENT_TEXT,
	CONTENT_NOISE,
	CONTENT_HALF
} content_t;

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
	const char	*target;	/* a link's target; its size */
	content_t	 content;
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
 * test_default_label_is_selected()), at the tests' block size. The
 * default block size is left unset, so the writer selects it.
 */
static void
base_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->label = "test";
	options->erofs_compression = compression;
	options->erofs_addressing = addressing;
	options->erofs_metabox = metabox;
	if (block_size != BLOCK_DEFAULT)
		options->allocation_unit_bytes = block_size;
}

static void
reproducible_options(fsimg_write_options_t *options)
{
	base_options(options);
	options->flags |= FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = EPOCH;
}

/* The output stack new_writer() builds, NULL for the bare volume. */
static const char *stack;

static fsimg_status_t
new_writer(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    fsimg_writer_t **writer, fsimg_error_t *error)
{
	if (stack != NULL)
		return (fsimg_writer_new(ctx, stack, options, writer, error));
	return (fsimg_writer_new_variant(ctx, &fsimg_erofs_format,
	    &fsimg_erofs_format.variants[0], options, writer, error));
}

/*
 * Open a fixture over a context with options, or the defaults when NULL.
 * meta_pages, when not zero, sizes the metadata cache, so a small value
 * with a scratch directory spills plan rows to scratch.
 */
static void
fixture_open_with(fixture_t *fixture, const fsimg_options_t *options,
    uint32_t meta_pages)
{
	fsimg_stats_t stats;
	fsimg_status_t status;

	bzero(fixture, sizeof(*fixture));
	status = fsimg_ctx_new(options, &fixture->ctx, &fixture->error);
	CHECK_CASE(status == FSIMG_OK, "context creation failed with %d: %s",
	    status, fixture->error.message);
	if (meta_pages != 0)
		CHECK(fsimg_test_meta_cache(fixture->ctx, meta_pages, NULL) ==
		    FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	fixture->memory_baseline = stats.memory_bytes;
}

static void
fixture_open(fixture_t *fixture)
{
	fixture_open_with(fixture, NULL, 0);
}

/* Free the writer and require that it released everything it held. */
static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->memory_baseline);
	CHECK(stats.scratch_bytes == 0);
	fsimg_ctx_free(fixture->ctx);
}

/*
 * Entry content comes from a provider, whose source ID is the index of the
 * entry's spec in the tree: a link serves its target and a file a pattern
 * of the index and the offset, which differs between files and drifts
 * within one. changed_source names a source whose validation reports a
 * change, UINT64_MAX for none. Planning never reads a source.
 */
static uint64_t changed_source = UINT64_MAX;

/*
 * Faults in content the source's validation cannot see: drifted_source
 * serves noise in place of its bytes, and short_source ends its reads at
 * half its size. UINT64_MAX disables each.
 */
static uint64_t drifted_source = UINT64_MAX;
static uint64_t short_source = UINT64_MAX;

/* Every provider read counts here, so a test can tell planning read none. */
static uint64_t provider_reads;

/* A byte of noise at offset, from a 64-bit mix of the index and offset. */
static unsigned char
noise_byte(uint64_t index, uint64_t offset)
{
	uint64_t x = (index << 40) ^ offset ^ UINT64_C(0x9e3779b97f4a7c15);

	x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
	return ((unsigned char)(x ^ (x >> 31)));
}

static unsigned char
spec_byte(uint64_t index, uint64_t offset)
{
	static const char text[] = "the quick brown fox jumps over the lazy dog ";
	const spec_t *spec = &tree.specs[index];
	content_t content = spec->content;

	if (spec->target != NULL)
		return ((unsigned char)spec->target[offset]);
	if (index == drifted_source)
		content = CONTENT_NOISE;
	if (content == CONTENT_HALF)
		content = offset < spec->size / 2 ? CONTENT_TEXT : CONTENT_NOISE;
	switch (content) {
	case CONTENT_TEXT:
		return ((unsigned char)text[offset % (sizeof(text) - 1)]);
	case CONTENT_NOISE:
		return (noise_byte(index, offset));
	default:
		return ((unsigned char)(index * 131 + offset * 7 + offset / 251));
	}
}

static fsimg_status_t
provider_validate(void *argument FSIMG_UNUSED, uint64_t id,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (id == changed_source ? FSIMG_ESOURCE_CHANGED : FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	unsigned char *bytes = buffer;
	uint64_t size;

	provider_reads++;
	CHECK(id < tree.count);
	size = tree.specs[id].size;
	CHECK(offset <= size);
	if (id == short_source)
		size /= 2;
	if (offset > size)
		offset = size;
	if (length > size - offset)
		length = (size_t)(size - offset);
	for (size_t i = 0; i < length; i++)
		bytes[i] = spec_byte(id, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
add_spec(fixture_t *fixture, const spec_t *spec)
{
	fsimg_entry_t entry;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER,
	    .id = (uint64_t)(spec - tree.specs) };
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
 * and names stay within a block; the size is the full blocks plus the
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

		if (used + need > block_size) {
			full++;
			used = 0;
		}
		used += need;
	}
	return (full * block_size + used);
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
		tail = (uint32_t)(row->size % block_size);
		row->layout = tail != 0 && fixed + tail <= block_size ?
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
	    block_size;

	/* Data blocks, contiguous in the same order. */
	next_block = model.metadata_end / block_size;
	for (uint64_t id = 0; id < model.count; id++) {
		erofs_row_t *row = &model.rows[id];
		uint64_t blocks = row->size / block_size;

		if (row->layout == EROFS_LAYOUT_FLAT && row->size % block_size != 0)
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
	CHECK_CASE(plan->block_bytes == block_size, "%s: block size", what);
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
	CHECK(fixture.plan->block_bytes == block_size);
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
		{ FSIMG_EROFS_COMPRESSION_NONE, 64, FSIMG_EROFS_METABOX_NONE,
		    { "64-bit addressing", FSIMG_EINVAL, "addressing" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 16, FSIMG_EROFS_METABOX_NONE,
		    { "16-bit addressing", FSIMG_EINVAL, "addressing" } },
		{ (fsimg_erofs_compression_t)7, 0, FSIMG_EROFS_METABOX_NONE,
		    { "unknown compression", FSIMG_EINVAL, "compression" } },
		{ FSIMG_EROFS_COMPRESSION_NONE, 0,
		    (fsimg_erofs_metabox_t)9,
		    { "unknown metabox", FSIMG_EINVAL, "metabox" } }
	};
	static const uint32_t widths[] = { 0, 32, 48 };
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
	options.erofs_addressing = 48;
	check_accepted(ctx, &options, "48-bit addressing");
	options.erofs_compression = FSIMG_EROFS_COMPRESSION_LZ4;
	check_accepted(ctx, &options, "48-bit addressing with LZ4");
	for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
		reproducible_options(&options);
		options.erofs_addressing = widths[i];
		options.erofs_metabox = FSIMG_EROFS_METABOX_PLAIN;
		check_accepted(ctx, &options, "plain metadata box");
		options.erofs_metabox = FSIMG_EROFS_METABOX_LZ4;
		check_accepted(ctx, &options, "LZ4 metadata box");
		options.erofs_compression = FSIMG_EROFS_COMPRESSION_LZ4;
		check_accepted(ctx, &options, "LZ4 metadata box and files");
	}
	fsimg_ctx_free(ctx);
}

/*
 * The writer emits the four block sizes EROFS defines, the ones E11 shows
 * the kernel and both other readers accept; any other size is invalid.
 */
static void
test_block_size_selects_512_to_4096(void)
{
	static const struct {
		uint32_t	 bytes;
		fsimg_status_t	 status;
	} cases[] = {
		{ 0, FSIMG_OK }, { 512, FSIMG_OK }, { 1024, FSIMG_OK },
		{ 2048, FSIMG_OK }, { 4096, FSIMG_OK },
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
	refusal_t unaligned = { "a volume of a block and a byte", FSIMG_EINVAL,
	    NULL };
	refusal_t odd = { "a volume of three blocks less a byte", FSIMG_EINVAL,
	    NULL };
	refusal_t partial = { "a volume of a block and 256 bytes",
	    FSIMG_EINVAL, NULL };
	refusal_t free_space = { "a free-space request", FSIMG_EINVAL, NULL };
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
	options.volume_bytes = 3 * block_size;
	check_accepted(ctx, &options, "a volume of three blocks");
	options.volume_bytes = 3 * block_size + 512;
	check_accepted(ctx, &options, "a volume of three blocks and a sector");
	options.volume_bytes = block_size + 1;
	check_refused(ctx, &options, &unaligned);
	options.volume_bytes = block_size + 256;
	check_refused(ctx, &options, &partial);
	options.volume_bytes = 3 * block_size - 1;
	check_refused(ctx, &options, &odd);
	options.volume_bytes = UINT64_C(0xffffffff) * block_size;
	check_accepted(ctx, &options, "a volume of 2^32 - 1 blocks");
	options.volume_bytes = (UINT64_C(1) << 32) * block_size;
	check_refused(ctx, &options, &beyond);

	/* The image is exactly its plan; there is no free space to size. */
	reproducible_options(&options);
	options.min_free_bytes = 1;
	check_refused(ctx, &options, &free_space);

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

/* Bytes rounded up to whole blocks. */
static uint64_t
whole_blocks(uint64_t bytes)
{
	return ((bytes + block_size - 1) / block_size * block_size);
}

/*
 * The root alone is a compact inode at NID 36 with "." and ".." inline;
 * the metadata area, and so the volume, ends at the next block boundary.
 */
#define ROOT_ALONE_END	(36 * SLOT + 32 + 27)

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
	CHECK(fixture.plan->metadata_end == whole_blocks(ROOT_ALONE_END));
	CHECK(fixture.plan->used_blocks ==
	    fixture.plan->metadata_end / block_size);
	CHECK(fixture.plan->block_count == fixture.plan->used_blocks);
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
	add_file(3 * block_size + 100, "/bin/big");
	add_file(0, "/bin/empty");
	add_file(2 * block_size, "/bin/exact");
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

	/*
	 * The big directory spans blocks; at 4096 bytes it is one full block
	 * and an inline tail, the case the model's flat-inline rule names.
	 */
	many = row_of_path(&fixture, "/many");
	CHECK(many.size > block_size && many.start_block != 0);
	if (block_size == 4096)
		CHECK(many.layout == EROFS_LAYOUT_FLAT_INLINE &&
		    many.size % block_size != 0);
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

	next = fixture.plan->metadata_end / block_size;
	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		erofs_row_t row = row_at(&fixture, id);
		uint64_t blocks = row.size / block_size;
		char path[MAX_PATH] = "/";

		if (row.layout == EROFS_LAYOUT_FLAT && row.size % block_size != 0)
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
		    erofs_row_tail_bytes(&row, block_size);
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
 * part and t fit one block. A compact inode's limit is t = B - 32 and an
 * extended inode's t = B - 64; a multiple of B, zero included, is flat.
 * The sizes are 4096-based, and 4064, 4065, 4032 and 4033 are B - 32,
 * B - 31, B - 64 and B - 63 modulo every B, so the cases hold at each.
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
 * final inode, so the final inode's position moves across block ends as k
 * grows; every plan is compared to the model, and the sweep requires
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
		    erofs_row_tail_bytes(&last, block_size);
		CHECK_CASE(start / block_size == (end - 1) / block_size,
		    "%s: the inode crosses a block", label);
		if (start % block_size == 0 && start != 0 &&
		    before.nid * SLOT + erofs_row_fixed_bytes(&before) +
		    erofs_row_tail_bytes(&before, block_size) < start)
			seen->moved++;
		if (end % block_size == 0)
			seen->fits++;
		if (start % block_size == block_size - SLOT &&
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

	options.volume_bytes = (used + 7) * block_size;
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->used_blocks == used);
	CHECK(fixture.plan->block_count == used + 7);
	fixture_close(&fixture);

	options.volume_bytes = used * block_size;
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->block_count == used);
	fixture_close(&fixture);

	options.volume_bytes = (used - 1) * block_size;
	fixture_open(&fixture);
	CHECK(fixture_plan(&fixture, &options, false) == FSIMG_ERANGE);
	fixture_close(&fixture);

	/*
	 * A volume of whole sectors but no whole block holds nothing; at
	 * 512-byte blocks every sector is a block, so there is none.
	 */
	if (block_size > 512) {
		options.volume_bytes = block_size - 512;
		fixture_open(&fixture);
		CHECK(fixture_plan(&fixture, &options, false) == FSIMG_ERANGE);
		fixture_close(&fixture);
	}
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
	unsigned char	 data[MAX_BLOCKS][BLOCK_MAX];
	size_t		 used[MAX_BLOCKS];
	unsigned	 blocks;
	uint64_t	 size_before_last_close;
	uint64_t	 closed_before_last_close;
} pack_result_t;

static pack_result_t packed;
static unsigned char pack_buffer[2 * BLOCK_MAX];
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
	for (size_t j = length; j < block_size; j++)
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
	CHECK(walked.blocks == (size + block_size - 1) / block_size);
	CHECK(walked.blocks > 2);
	CHECK(walked.last_valid == size - (walked.blocks - 1) * block_size);
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

/*
 * Emission.
 */
#define POISON		0xa5u
#define POISON_ALT	0x5au
#define READ_CHUNK	65536u
#define SUPER_REGION	3072u

/*
 * A volume in memory, filled with a poison byte before emission. A sparse
 * sink, one with far set, is longer than its bytes: bytes hold its first
 * dense_length, far holds far_length at far_offset, every other byte reads
 * as zero, and a write must fall inside one of the two, so a 48-bit volume
 * of 2^44 bytes costs only the blocks a test places.
 */
typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	uint64_t	 fail_write_at;	/* 1-based; 0 for none */
	bool		 fail_flush;
	unsigned char	*far;		/* sparse only */
	uint64_t	 dense_length;
	uint64_t	 far_offset;
	uint64_t	 far_length;
} memory_sink_t;

static fsimg_status_t
memory_size(void *argument, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*transferred = 0;
	sink->writes++;
	if (sink->writes == sink->fail_write_at)
		return (FSIMG_EIO);
	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	return (sink->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

/*
 * The part of the request [offset, offset + length) that lies in
 * [start, start + span): its length, zero when they do not meet, and in
 * *skip its distance from offset. Sparse offsets stay below 2^46.
 */
static size_t
overlap(uint64_t offset, size_t length, uint64_t start, uint64_t span,
    size_t *skip)
{
	const uint64_t low = offset > start ? offset : start;
	const uint64_t high = offset + length < start + span ?
	    offset + length : start + span;

	if (low >= high)
		return (0);
	*skip = (size_t)(low - offset);
	return ((size_t)(high - low));
}

static fsimg_status_t
sparse_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;
	unsigned char *out = buffer;
	size_t skip = 0, count;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bzero(out, length);
	count = overlap(offset, length, 0, sink->dense_length, &skip);
	if (count != 0)
		bcopy(sink->bytes + offset + skip, out + skip, count);
	count = overlap(offset, length, sink->far_offset, sink->far_length,
	    &skip);
	if (count != 0)
		bcopy(sink->far + (offset + skip - sink->far_offset),
		    out + skip, count);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;
	size_t skip = 0;

	*transferred = 0;
	sink->writes++;
	if (sink->writes == sink->fail_write_at)
		return (FSIMG_EIO);
	if (overlap(offset, length, 0, sink->dense_length, &skip) == length)
		bcopy(buffer, sink->bytes + offset, length);
	else {
		CHECK_CASE(overlap(offset, length, sink->far_offset,
		    sink->far_length, &skip) == length, "write of %zu bytes "
		    "at %ju outside the sparse sink's bytes", length,
		    (uintmax_t)offset);
		bcopy(buffer, sink->far + (offset - sink->far_offset), length);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;
	size_t skip = 0, count;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	count = overlap(offset, (size_t)length, 0, sink->dense_length, &skip);
	if (count != 0)
		bzero(sink->bytes + offset + skip, count);
	count = overlap(offset, (size_t)length, sink->far_offset,
	    sink->far_length, &skip);
	if (count != 0)
		bzero(sink->far + (offset + skip - sink->far_offset), count);
	return (FSIMG_OK);
}

/*
 * An I/O object over a sink. The writable one has no zero_range, so the
 * common emitter must overwrite the poison with zeros itself, except over
 * a sparse sink, which zeros only the bytes it holds.
 */
static fsimg_status_t
memory_io(fixture_t *fixture, memory_sink_t *sink, bool writable,
    fsimg_io_t **out)
{
	const bool sparse = sink->far != NULL;
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = sparse ? sparse_read : memory_read;
	if (writable) {
		ops.write_at = sparse ? sparse_write : memory_write;
		ops.zero_range = sparse ? sparse_zero : NULL;
		ops.flush = memory_flush;
	}
	*out = NULL;
	return (fsimg_io_from_callbacks(fixture->ctx, &ops, sink, out, NULL));
}

/* Allocate a sink of the planned volume's size, filled with poison. */
static void
sink_open(fixture_t *fixture, memory_sink_t *sink, unsigned char poison)
{
	fsimg_plan_info_t info;

	CHECK(fixture->plan != NULL);
	CHECK(fsimg_plan_get_info(&fixture->plan->common, &info, NULL) ==
	    FSIMG_OK);
	bzero(sink, sizeof(*sink));
	sink->length = info.volume_bytes;
	sink->bytes = malloc((size_t)sink->length);
	CHECK(sink->bytes != NULL);
	memset(sink->bytes, poison, (size_t)sink->length);
}

static fsimg_status_t
emit_into(fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_io_t *output;
	fsimg_status_t status;

	status = memory_io(fixture, sink, true, &output);
	if (status == FSIMG_OK)
		status = fsimg_writer_emit(fixture->writer, output,
		    &fixture->error);
	fsimg_io_free(output);
	return (status);
}

/* Emit the planned tree into a sink poisoned with poison, which must work. */
static void
emit_tree(fixture_t *fixture, memory_sink_t *sink, unsigned char poison)
{
	fsimg_status_t status;

	sink_open(fixture, sink, poison);
	status = emit_into(fixture, sink);
	CHECK_CASE(status == FSIMG_OK, "emission failed with %d: %s (%s)",
	    status, fixture->error.message, fixture->error.path);
}

/*
 * The tree every emission test but the faults writes. It holds every kind
 * the writer stores and each layout case: an empty file, one byte, tails
 * that fit beside the inode, a 4065-byte file whose tail cannot sit beside
 * a 32-byte inode, exact multiples of the block size, multi-block files
 * with a tail and a megabyte. Names that sort before "." and ".." lead
 * the root and a subdirectory, a deep path nests ten levels, "/wide"
 * spans several directory blocks, and "/owners" holds what only an
 * extended inode stores: owners above 16 bits and times with nanoseconds
 * or before 1970. "/small" is the subtree the extraction test copies.
 */
#define WIDE_ENTRIES	300u
#define DEEP_LEVELS	10u

/* A 200-byte target and its terminator. */
static char long_target[201];

static spec_t *
add_symlink(const char *target, const char *path)
{
	spec_t *spec;

	spec = tree_add(FSIMG_ENTRY_SYMLINK, strlen(target), "%s", path);
	spec->target = target;
	return (spec);
}

static void
build_round_trip_tree(void)
{
	static const uint32_t sizes[] = { 0, 1, 100, 4064, 4065, 4096, 4097,
	    8192, 8192 + 300, 1000003 };
	char deep[MAX_PATH] = "";

	tree_reset();
	for (size_t i = 0; i < sizeof(long_target) - 1; i++)
		long_target[i] = (char)('a' + i % 26);

	add_file(3, "/-a")->mode = 0600;
	add_directory("/.-b")->mode = 0700;
	add_file(5, "/.-b/-a");
	add_file(7, "/.-b/.-b");

	add_directory("/layouts");
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		add_file(sizes[i], "/layouts/size-%u", sizes[i])->mode = 0640;

	add_directory("/links");
	add_symlink("size-1", "/links/short");
	add_symlink("../layouts/size-100", "/links/up")->mode = 0777;
	add_symlink(long_target, "/links/long");
	tree_add(FSIMG_ENTRY_FIFO, 0, "/links/pipe")->mode = 0620;

	for (unsigned i = 0; i < DEEP_LEVELS; i++) {
		size_t used = strlen(deep);

		snprintf(deep + used, sizeof(deep) - used, "/level%u", i);
		add_directory(deep);
	}
	add_file(777, "%s/bottom", deep);

	with_owner(add_directory("/owners"), 70000, 4000000000u)->mode = 0750;
	with_owner(add_file(10, "/owners/big-ids"), 70000, 4000000000u);
	with_time(add_file(20, "/owners/nanoseconds"), 1700000123, 123456789);
	with_time(add_file(30, "/owners/before-1970"), -86400 * 400 + 7,
	    999999999);
	with_time(add_file(40, "/owners/whole-seconds"), 1699999999, 0);
	with_time(add_symlink("big-ids", "/owners/link"), 1600000000, 5);

	add_directory("/small");
	add_directory("/small/sub");
	add_file(0, "/small/empty");
	add_file(1, "/small/one");
	add_file(5000, "/small/sub/five-thousand");
	add_file(9000, "/small/sub/nine-thousand");

	add_directory("/wide");
	for (unsigned i = 0; i < WIDE_ENTRIES; i++)
		add_file(i % 7, "/wide/n%03u-%s", i,
		    "0123456789012345678901234567890123456789"
		    "0123456789012345678901234567890123456789"
		    "0123456789012345678901234567890123456789");
}

/* The write options of a volume that stores times as given. */
static void
unreproducible_options(fsimg_write_options_t *options)
{
	static const unsigned char uuid[16] = { 0xe0, 0xf5, 1, 2, 3, 4, 5, 6,
	    7, 8, 9, 10, 11, 12, 13, 14 };

	base_options(options);
	bcopy(uuid, options->volume_uuid, sizeof(uuid));
}

static uint32_t
expected_mode(const spec_t *spec)
{
	if (spec->mode != 0)
		return (spec->mode & 07777);
	if (spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_FIFO)
		return (0644);
	return (0755);
}

static uint64_t
expected_size(const spec_t *spec)
{
	if (spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_SYMLINK)
		return (spec->size);
	return (0);
}

/* The stored time: the format time for a reproducible volume or no time. */
static fsimg_timestamp_t
expected_time(const spec_t *spec, bool reproducible, int64_t format_time)
{
	fsimg_timestamp_t time = { .seconds = format_time };

	if (!reproducible && spec->stamped) {
		time.seconds = spec->mtime;
		time.nanoseconds = spec->mtime_nsec;
	}
	return (time);
}

/* The index of the spec at a reader's path, which must be in the tree. */
static size_t
spec_at_path(const char *path)
{
	for (size_t i = 0; i < tree.count; i++) {
		if (strcmp(skip_slash(path), skip_slash(tree.specs[i].path)) ==
		    0)
			return (i);
	}
	fprintf(stderr, "the reader returned %s, which is in no spec\n", path);
	CHECK(false);
	return (0);
}

/* Read the current entry's content in chunks and compare it to its spec. */
static void
check_content(fsimg_reader_t *reader, const fsimg_entry_t *entry,
    size_t index)
{
	static unsigned char buffer[READ_CHUNK];
	size_t length;
	uint64_t offset;

	for (offset = 0; offset < entry->size; offset += length) {
		CHECK(fsimg_reader_read_at(reader, offset, buffer,
		    sizeof(buffer), &length, NULL) == FSIMG_OK);
		CHECK_CASE(length > 0, "%s: no bytes at %ju", entry->path,
		    (uintmax_t)offset);
		for (size_t i = 0; i < length; i++)
			CHECK_CASE(buffer[i] == spec_byte(index, offset + i),
			    "%s: byte %ju is %u, want %u", entry->path,
			    (uintmax_t)(offset + i), buffer[i],
			    spec_byte(index, offset + i));
	}
}

/* Compare an entry the reader enumerates with the spec at its path. */
static void
check_entry(fsimg_reader_t *reader, const fsimg_entry_t *entry, size_t index,
    bool reproducible, int64_t format_time)
{
	const spec_t *spec = &tree.specs[index];
	const fsimg_metadata_t *metadata = &entry->metadata;
	fsimg_timestamp_t time;

	CHECK_CASE(entry->kind == spec->kind, "%s: kind %d, want %d",
	    entry->path, entry->kind, spec->kind);
	CHECK_CASE(entry->size == expected_size(spec),
	    "%s: size %ju, want %ju", entry->path, (uintmax_t)entry->size,
	    (uintmax_t)expected_size(spec));
	CHECK_CASE((metadata->valid & (FSIMG_META_MODE | FSIMG_META_OWNER |
	    FSIMG_META_MODIFIED)) == (FSIMG_META_MODE | FSIMG_META_OWNER |
	    FSIMG_META_MODIFIED), "%s: valid %#x", entry->path,
	    metadata->valid);
	CHECK_CASE(metadata->mode == expected_mode(spec),
	    "%s: mode %o, want %o", entry->path, metadata->mode,
	    expected_mode(spec));
	CHECK_CASE(metadata->uid == (spec->owned ? spec->uid : 0) &&
	    metadata->gid == (spec->owned ? spec->gid : 0),
	    "%s: owner %u:%u", entry->path, metadata->uid, metadata->gid);
	time = expected_time(spec, reproducible, format_time);
	CHECK_CASE(metadata->modified.seconds == time.seconds &&
	    metadata->modified.nanoseconds == time.nanoseconds,
	    "%s: time %jd.%09u, want %jd.%09u", entry->path,
	    (intmax_t)metadata->modified.seconds,
	    metadata->modified.nanoseconds, (intmax_t)time.seconds,
	    time.nanoseconds);
	check_content(reader, entry, index);
}

static fsimg_reader_t *
open_reader(fixture_t *fixture, memory_sink_t *sink, fsimg_io_t **input)
{
	fsimg_reader_t *reader;
	fsimg_error_t error;
	fsimg_status_t status;

	CHECK(memory_io(fixture, sink, false, input) == FSIMG_OK);
	status = fsimg_reader_open(fixture->ctx, *input, NULL, &reader, &error);
	CHECK_CASE(status == FSIMG_OK, "the reader refused the image: %d %s",
	    status, error.message);
	return (reader);
}

/* Enumerate the image: each spec once, the implicit root not included. */
static void
check_enumeration(fixture_t *fixture, memory_sink_t *sink, bool reproducible)
{
	const int64_t format_time = fixture->plan->format_time;
	bool seen[MAX_SPECS] = { false };
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fsimg_status_t status;
	size_t count = 0, index;

	reader = open_reader(fixture, sink, &input);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) ==
	    FSIMG_OK) {
		count++;
		index = spec_at_path(entry->path);
		CHECK_CASE(!seen[index], "%s is enumerated twice", entry->path);
		seen[index] = true;
		check_entry(reader, entry, index, reproducible, format_time);
	}
	CHECK(status == FSIMG_DONE);
	CHECK_CASE(count == tree.count, "%zu entries, want %zu", count,
	    tree.count);
	for (size_t i = 0; i < tree.count; i++)
		CHECK_CASE(seen[i], "%s is not enumerated", tree.specs[i].path);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* The verifier completes cleanly and counts what the specs hold. */
static void
check_verification(fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fsimg_status_t status;
	uint64_t directories = 1, files = 0, bytes = 0;

	for (size_t i = 0; i < tree.count; i++) {
		if (tree.specs[i].kind == FSIMG_ENTRY_DIRECTORY)
			directories++;
		else
			files++;
		bytes += expected_size(&tree.specs[i]);
	}
	reader = open_reader(fixture, sink, &input);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	CHECK_CASE(status == FSIMG_OK, "verification failed with %d: %s",
	    status, error.message);
	CHECK(report.complete);
	CHECK(report.directories == directories);
	CHECK(report.files == files);
	CHECK(report.logical_bytes == bytes);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* A disposable directory under $TMPDIR. */
static void
make_temp_directory(char *path, size_t length)
{
	const char *base = getenv("TMPDIR");

	snprintf(path, length, "%s/fsimg-erofs-write-XXXXXX",
	    base != NULL && base[0] != '\0' ? base : "/tmp");
	CHECK(mkdtemp(path) != NULL);
}

/* Remove a directory tree the test created. */
static void
remove_tree(const char *path)
{
	char child[1024];
	struct dirent *entry;
	struct stat st;
	DIR *dir;

	dir = opendir(path);
	CHECK(dir != NULL);
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		CHECK(lstat(child, &st) == 0);
		if (S_ISDIR(st.st_mode))
			remove_tree(child);
		else
			CHECK(unlink(child) == 0);
	}
	CHECK(closedir(dir) == 0);
	CHECK(rmdir(path) == 0);
}

/* Compare a host file with the bytes its spec describes. */
static void
check_host_file(const char *path, size_t index)
{
	static unsigned char buffer[READ_CHUNK];
	const uint64_t size = tree.specs[index].size;
	struct stat st;
	uint64_t offset = 0;
	size_t length;
	FILE *file;

	CHECK_CASE(stat(path, &st) == 0, "%s was not extracted", path);
	CHECK_CASE((uint64_t)st.st_size == size, "%s: size %jd, want %ju",
	    path, (intmax_t)st.st_size, (uintmax_t)size);
	file = fopen(path, "rb");
	CHECK(file != NULL);
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		for (size_t i = 0; i < length; i++)
			CHECK_CASE(buffer[i] == spec_byte(index, offset + i),
			    "%s: byte %ju differs", path,
			    (uintmax_t)(offset + i));
		offset += length;
	}
	CHECK(offset == size);
	CHECK(fclose(file) == 0);
}

/* Extract "/small", the tree's subtree of plain files, and compare it. */
static void
check_extraction(fixture_t *fixture, memory_sink_t *sink)
{
	static const char *const selection[] = { "/small" };
	fsimg_extract_options_t options;
	char directory[256], path[512];
	fsimg_reader_t *reader;
	fsimg_error_t error;
	fsimg_io_t *input;
	fsimg_status_t status;
	size_t extracted = 0;
	int fd;

	make_temp_directory(directory, sizeof(directory));
	fd = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(fd != -1);
	fsimg_extract_options_init(&options);
	options.selections = selection;
	options.selection_count = 1;
	reader = open_reader(fixture, sink, &input);
	status = fsimg_extract_at(reader, fd, "out", &options, &error);
	CHECK_CASE(status == FSIMG_OK, "extraction failed with %d: %s (%s)",
	    status, error.message, error.path);
	for (size_t i = 0; i < tree.count; i++) {
		if (strncmp(tree.specs[i].path, "/small/", 7) != 0 ||
		    tree.specs[i].kind != FSIMG_ENTRY_FILE)
			continue;
		snprintf(path, sizeof(path), "%s/out%s", directory,
		    tree.specs[i].path);
		check_host_file(path, i);
		extracted++;
	}
	CHECK(extracted == 4);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	CHECK(close(fd) == 0);
	remove_tree(directory);
}

static void
check_round_trip(fsimg_write_options_t *options, bool reproducible)
{
	fixture_t fixture;
	memory_sink_t sink;

	build_round_trip_tree();
	plan_tree(&fixture, options);
	emit_tree(&fixture, &sink, POISON);
	check_enumeration(&fixture, &sink, reproducible);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
}

/*
 * The root alone: its inline tail, "." and "..", is the only content
 * beside the superblock in the checksum's range.
 */
static void
test_empty_catalog_image_reads_back_as_the_root_alone(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	tree_reset();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	CHECK(row_at(&fixture, 0).layout == EROFS_LAYOUT_FLAT_INLINE);
	emit_tree(&fixture, &sink, POISON);
	CHECK(sink.length == whole_blocks(ROOT_ALONE_END));
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_reproducible_image_reads_back_as_specified(void)
{
	fsimg_write_options_t options;

	reproducible_options(&options);
	check_round_trip(&options, true);
}

static void
test_unreproducible_image_stores_given_times_and_owners(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	erofs_row_t row;

	unreproducible_options(&options);
	check_round_trip(&options, false);

	/* The round trip covers the cases it is meant to. */
	build_round_trip_tree();
	plan_tree(&fixture, &options);
	CHECK(row_of_path(&fixture, "/owners/big-ids").extended);
	CHECK(row_of_path(&fixture, "/owners/nanoseconds").extended);
	CHECK(row_of_path(&fixture, "/owners/before-1970").extended);
	CHECK(!row_of_path(&fixture, "/layouts/size-100").extended);
	row = row_of_path(&fixture, "/layouts/size-100");
	CHECK(row.layout == EROFS_LAYOUT_FLAT_INLINE);
	row = row_of_path(&fixture, "/layouts/size-4065");
	CHECK(row.layout == EROFS_LAYOUT_FLAT);
	row = row_of_path(&fixture, "/wide");
	CHECK(row.size > 4 * block_size);
	fixture_close(&fixture);
}

/*
 * Reproducible images.
 */

/*
 * One way to build the same image: how the tree is inserted, the
 * context's limits, and the time zone. A scratch size gives the context a
 * scratch directory with a one-page metadata cache, so plan rows spill.
 */
typedef struct variant {
	const char	*name;
	bool		 reverse;
	uint64_t	 memory_bytes;
	uint64_t	 scratch_bytes;
	const char	*time_zone;
} variant_t;

static void
set_time_zone(const char *zone)
{
	if (zone != NULL)
		CHECK(setenv("TZ", zone, 1) == 0);
	else
		CHECK(unsetenv("TZ") == 0);
	tzset();
}

static void
emit_variant(const variant_t *variant, memory_sink_t *sink)
{
	fsimg_write_options_t options;
	fsimg_options_t context;
	char directory[256] = "";
	fixture_t fixture;
	fsimg_status_t status;
	int fd = -1;

	fsimg_options_init(&context);
	if (variant->memory_bytes != 0)
		context.memory_bytes = variant->memory_bytes;
	if (variant->scratch_bytes != 0) {
		make_temp_directory(directory, sizeof(directory));
		fd = open(directory, O_RDONLY | O_DIRECTORY);
		CHECK(fd != -1);
		context.scratch_bytes = variant->scratch_bytes;
		context.scratch_directory_fd = fd;
	}
	reproducible_options(&options);
	set_time_zone(variant->time_zone);
	fixture_open_with(&fixture, &context,
	    variant->scratch_bytes != 0 ? 1 : 0);
	status = fixture_plan(&fixture, &options, variant->reverse);
	CHECK_CASE(status == FSIMG_OK, "%s: planning failed with %d: %s",
	    variant->name, status, fixture.error.message);
	emit_tree(&fixture, sink, POISON);
	fixture_close(&fixture);
	set_time_zone(NULL);
	if (fd != -1) {
		CHECK(close(fd) == 0);
		remove_tree(directory);
	}
}

static void
check_identity_variants(void (*build)(void))
{
	static const variant_t variants[] = {
		{ "forward", false, 0, 0, NULL },
		{ "reverse insertion", true, 0, 0, NULL },
		{ "minimum memory budget", false, 16u << 20, 0, NULL },
		{ "scratch with a one-page cache", false, 0, 8u << 20, NULL },
		{ "time zone", false, 0, 0, "America/New_York" },
		{ "time zone ahead of UTC", true, 0, 0, "Asia/Kolkata" },
	};
	const size_t count = sizeof(variants) / sizeof(variants[0]);
	memory_sink_t reference, sink;

	build();
	emit_variant(&variants[0], &reference);
	for (size_t i = 1; i < count; i++) {
		emit_variant(&variants[i], &sink);
		CHECK_CASE(sink.length == reference.length &&
		    bcmp(sink.bytes, reference.bytes,
		    (size_t)sink.length) == 0, "%s: the image differs",
		    variants[i].name);
		free(sink.bytes);
	}
	free(reference.bytes);
}

static void
test_reproducible_image_ignores_order_limits_and_time_zone(void)
{
	check_identity_variants(build_round_trip_tree);
}

/*
 * Poison.
 */

static void
mark_range(unsigned char *carries, uint64_t length, uint64_t start,
    uint64_t count)
{
	CHECK(start <= length && count <= length - start);
	memset(carries + start, 1, (size_t)count);
}

/*
 * The bytes of the image that may carry data, one byte each, from the plan
 * alone: the superblock, each inode's body, its fixed part and inline tail
 * or index, and the data blocks' content, which ends at the size under the
 * flat layout, at the last full block under the flat-inline one and at the
 * last cluster's end under the compressed one. Under a metadata box the
 * superblock has its extension slot, the bodies the box holds are inside
 * the box's blocks, and the box inode's body and blocks carry data.
 */
static unsigned char *
carrying_bytes(fixture_t *fixture, uint64_t length)
{
	unsigned char *carries = calloc(1, (size_t)length);

	const erofs_plan_t *plan = fixture->plan;
	const bool boxed = plan->metabox != FSIMG_EROFS_METABOX_NONE;

	CHECK(carries != NULL);
	mark_range(carries, length, EROFS_SUPER_OFFSET, EROFS_SUPER_BASE_BYTES +
	    (boxed ? EROFS_SUPER_SLOT_BYTES : 0));
	if (boxed) {
		mark_range(carries, length, plan->box.nid * SLOT,
		    plan->box_body);
		mark_range(carries, length, plan->box.start_block * block_size,
		    erofs_row_data_blocks(&plan->box, block_size) * block_size);
	}
	for (uint64_t id = 0; id <= fixture->writer->count; id++) {
		erofs_row_t row = row_at(fixture, id);
		uint64_t tail = erofs_row_tail_bytes(&row, block_size);
		uint64_t data = row.size - tail;

		if ((row.nid & EROFS_NID_BOX) == 0)
			mark_range(carries, length, row.nid * SLOT,
			    erofs_row_body_bytes(fixture->plan, &row));
		if (row.layout == EROFS_LAYOUT_COMPRESSED_FULL)
			data = (uint64_t)row.clusters * block_size;
		if (data != 0)
			mark_range(carries, length, row.start_block * block_size,
			    data);
	}
	return (carries);
}

/* The image with the poison given, from a reproducible plan of the tree. */
static void
emit_poisoned(memory_sink_t *sink, unsigned char poison,
    unsigned char **carries)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	reproducible_options(&options);
	plan_tree(&fixture, &options);
	emit_tree(&fixture, sink, poison);
	if (carries != NULL)
		*carries = carrying_bytes(&fixture, sink->length);
	fixture_close(&fixture);
}

/*
 * Outside the superblock, the inode bodies and the data the plan names,
 * the image is zero, the boot area included. Inside them, the same image
 * comes out under two poison bytes, so the poison cannot be a stale byte
 * the emitter left in a range it meant to write.
 */
static void
check_no_stale_byte(void (*build)(void))
{
	memory_sink_t poisoned, other;
	unsigned char *carries;

	build();
	emit_poisoned(&poisoned, POISON, &carries);
	emit_poisoned(&other, POISON_ALT, NULL);

	CHECK(poisoned.length == other.length);
	CHECK(poisoned.length > block_size);
	for (uint64_t i = 0; i < EROFS_SUPER_OFFSET; i++)
		CHECK_CASE(poisoned.bytes[i] == 0, "boot byte %ju is %u",
		    (uintmax_t)i, poisoned.bytes[i]);
	for (uint64_t i = 0; i < poisoned.length; i++) {
		if (!carries[i])
			CHECK_CASE(poisoned.bytes[i] == 0,
			    "byte %ju outside any data is %u", (uintmax_t)i,
			    poisoned.bytes[i]);
		CHECK_CASE(poisoned.bytes[i] == other.bytes[i],
		    "byte %ju is %u under poison %#x and %u under %#x",
		    (uintmax_t)i, poisoned.bytes[i], POISON, other.bytes[i],
		    POISON_ALT);
	}
	free(carries);
	free(poisoned.bytes);
	free(other.bytes);
}

static void
test_emission_leaves_no_stale_byte(void)
{
	check_no_stale_byte(build_round_trip_tree);
}

/*
 * A fixed disk fits its partition to 512-byte sectors, not to blocks. A
 * volume of whole sectors holds floor(V / B) blocks, and the partial block
 * after them is zero, under poison, and outside the block count, so the
 * reader and verifier see the same image as without it.
 */
static void
test_sector_multiple_volume_has_a_zero_partial_tail(void)
{
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fixture_t fixture;
	memory_sink_t sink;
	uint64_t blocks, used;

	build_round_trip_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	used = fixture.plan->used_blocks;
	fixture_close(&fixture);

	/* At 512-byte blocks no partial block exists; only excess blocks do. */
	blocks = used + 2;
	options.volume_bytes = blocks * block_size + block_size - 512;
	CHECK(block_size == 512 || options.volume_bytes % block_size != 0);
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->block_count == blocks);
	CHECK(fsimg_plan_get_info(&fixture.plan->common, &info, NULL) ==
	    FSIMG_OK);
	CHECK(info.volume_bytes == options.volume_bytes);
	CHECK(info.allocation_units == blocks);
	CHECK(info.free_bytes == (blocks - used) * block_size);

	emit_tree(&fixture, &sink, POISON);
	CHECK(sink.length == options.volume_bytes);
	for (uint64_t i = used * block_size; i < sink.length; i++)
		CHECK_CASE(sink.bytes[i] == 0, "byte %ju past the content is %u",
		    (uintmax_t)i, sink.bytes[i]);
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
}

/*
 * Faults.
 */

/* An emission that fails fails the writer without leaking. */
static void
test_write_failure_fails_emission_at_any_write(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	uint64_t writes;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	emit_tree(&fixture, &sink, POISON);
	writes = sink.writes;
	free(sink.bytes);
	fixture_close(&fixture);
	CHECK(writes > 2);

	for (unsigned scenario = 0; scenario < 3; scenario++) {
		const uint64_t at[] = { 1, writes / 2, writes };

		plan_tree(&fixture, &options);
		sink_open(&fixture, &sink, POISON);
		sink.fail_write_at = at[scenario];
		status = emit_into(&fixture, &sink);
		CHECK_CASE(status != FSIMG_OK,
		    "write %ju failing did not fail the emission",
		    (uintmax_t)at[scenario]);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

static void
test_flush_failure_fails_emission(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	build_mixed_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	sink_open(&fixture, &sink, POISON);
	sink.fail_flush = true;
	CHECK(emit_into(&fixture, &sink) != FSIMG_OK);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Allocation fails at every point of emission, once planning is done. */
static void
test_allocation_failure_during_emission_is_clean(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	bool succeeded = false;
	unsigned failures = 0;

	build_mixed_tree();
	reproducible_options(&options);
	for (uint64_t attempt = 1; attempt < 4096 && !succeeded; attempt++) {
		plan_tree(&fixture, &options);
		sink_open(&fixture, &sink, POISON);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = emit_into(&fixture, &sink);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		CHECK_CASE(status == FSIMG_OK || status == FSIMG_ENOMEM,
		    "allocation %ju failing gave status %d",
		    (uintmax_t)attempt, status);
		succeeded = status == FSIMG_OK;
		failures += status == FSIMG_ENOMEM;
		free(sink.bytes);
		fixture_close(&fixture);
	}
	CHECK(succeeded);
	CHECK(failures > 0);
}

/*
 * A source changed after planning fails the emission, whatever it was and
 * wherever it sits in the volume.
 */
static void
check_changed_source(const char *path)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	size_t index = spec_at_path(path);

	reproducible_options(&options);
	plan_tree(&fixture, &options);
	sink_open(&fixture, &sink, POISON);
	changed_source = index;
	status = emit_into(&fixture, &sink);
	CHECK_CASE(status == FSIMG_ESOURCE_CHANGED,
	    "%s: a changed source gave status %d: %s", path, status,
	    fixture.error.message);
	changed_source = UINT64_MAX;
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_changed_file_source_fails_emission(void)
{
	build_round_trip_tree();
	check_changed_source("/layouts/size-0");
	check_changed_source("/layouts/size-100");
	check_changed_source("/layouts/size-1000003");
}

static void
test_changed_link_source_fails_emission(void)
{
	build_round_trip_tree();
	check_changed_source("/links/short");
	check_changed_source("/links/long");
}

/*
 * LZ4 files.
 */

#define LZ4_WINDOW	65536u

/*
 * The LZ4 tree: text, which compresses, at the sizes where the cluster
 * rules change (one block, two blocks, the 64 KiB window), noise, which
 * does not, a file of text then noise, whose clusters are LZ4 and then
 * plain, the default pattern, files only an extended inode describes, and
 * the other kinds. "/small" holds the four files check_extraction()
 * copies.
 */
static void
build_lz4_tree(void)
{
	const uint64_t b = block_size;
	const uint64_t text[] = { 0, 1, b - 1, b, b + 1, 2 * b, 3 * b + 5,
	    LZ4_WINDOW - 1, LZ4_WINDOW, LZ4_WINDOW + 1, 300000, 1000003 };
	const uint64_t noise[] = { b + 1, 2 * b, 100000 };

	tree_reset();
	add_directory("/text");
	for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++)
		add_file(text[i], "/text/t%ju", (uintmax_t)text[i])->content =
		    CONTENT_TEXT;
	add_directory("/noise");
	for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); i++)
		add_file(noise[i], "/noise/n%ju", (uintmax_t)noise[i])->content =
		    CONTENT_NOISE;
	add_file(200000, "/half")->content = CONTENT_HALF;
	add_file(70000, "/pattern");
	with_owner(add_file(20000, "/owned"), 70000, 70000)->content =
	    CONTENT_TEXT;
	with_time(add_file(30000, "/timed"), 1700000123, 5)->content =
	    CONTENT_TEXT;
	add_symlink("text/t1", "/link");
	tree_add(FSIMG_ENTRY_FIFO, 0, "/pipe");
	add_directory("/small");
	add_directory("/small/sub");
	add_file(0, "/small/empty");
	add_file(1, "/small/one");
	add_file(5000, "/small/sub/five-thousand")->content = CONTENT_TEXT;
	add_file(9000, "/small/sub/nine-thousand")->content = CONTENT_TEXT;
}

/*
 * Which files compress: text and the text-then-noise file once their flat
 * layout stores two blocks, noise never; the pattern may or may not. A
 * text file of at least a window fills every cluster but the last with a
 * whole window, since 64 KiB of text encodes far below 512 bytes, so its
 * clusters are ceil(size / 64 KiB); every compressed file stores fewer
 * blocks than its flat layout.
 */
static void
check_lz4_layouts(fixture_t *fixture)
{
	const spec_t *spec;
	erofs_row_t row, flat;
	bool compressed;

	for (size_t i = 0; i < tree.count; i++) {
		spec = &tree.specs[i];
		if (spec->kind != FSIMG_ENTRY_FILE)
			continue;
		row = row_of_path(fixture, spec->path);
		compressed = row.layout == EROFS_LAYOUT_COMPRESSED_FULL;
		flat = row;
		flat.layout = row.extended || spec->size % block_size == 0 ?
		    EROFS_LAYOUT_FLAT : EROFS_LAYOUT_FLAT_INLINE;
		if (flat.layout == EROFS_LAYOUT_FLAT_INLINE &&
		    erofs_row_fixed_bytes(&flat) + spec->size % block_size >
		    block_size)
			flat.layout = EROFS_LAYOUT_FLAT;
		flat.clusters = 0;
		CHECK_CASE(spec->content == CONTENT_PATTERN ||
		    compressed == ((spec->content == CONTENT_TEXT ||
		    spec->content == CONTENT_HALF) &&
		    erofs_row_data_blocks(&flat, block_size) >= 2),
		    "%s: compressed %d", spec->path, compressed);
		if (!compressed)
			continue;
		CHECK(row.clusters < erofs_row_data_blocks(&flat, block_size));
		if (spec->content == CONTENT_TEXT && spec->size >= LZ4_WINDOW)
			CHECK_CASE(row.clusters == (spec->size + LZ4_WINDOW - 1) /
			    LZ4_WINDOW, "%s: %u clusters", spec->path,
			    row.clusters);
	}
}

/*
 * A reproducible LZ4 image reads back, verifies and extracts as specified,
 * with LZ4_0PADDING and LZ4's maximum distance in its superblock.
 */
static void
test_lz4_image_reads_back_as_specified(void)
{
	fsimg_write_options_t options;
	erofs_config_t config;
	erofs_super_t sb;
	fixture_t fixture;
	memory_sink_t sink;
	const char *what;

	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	build_lz4_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	check_lz4_layouts(&fixture);
	emit_tree(&fixture, &sink, POISON);
	CHECK(sink.length > 4096);
	CHECK(erofs_region_decode(sink.bytes + EROFS_SUPER_OFFSET,
	    4096 - EROFS_SUPER_OFFSET, &sb, &config, &what) == FSIMG_OK);
	CHECK(sb.incompat == EROFS_INCOMPAT_LZ4_0PADDING);
	CHECK(sb.compression_word == 65535);
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

static void
test_lz4_image_ignores_order_limits_and_time_zone(void)
{
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	check_identity_variants(build_lz4_tree);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

static void
test_lz4_emission_leaves_no_stale_byte(void)
{
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	check_no_stale_byte(build_lz4_tree);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

/* The image of the current tree under the compression setting given. */
static void
emit_compressed(fsimg_erofs_compression_t setting, memory_sink_t *sink)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	compression = setting;
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	emit_tree(&fixture, sink, POISON);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

/*
 * When no file would store fewer blocks compressed, compression=lz4 writes
 * the uncompressed image byte for byte, superblock included.
 */
static void
test_lz4_without_a_compressible_file_is_the_flat_image(void)
{
	const uint64_t b = block_size;
	memory_sink_t flat, lz4;

	tree_reset();
	add_file(b + 1, "/text")->content = CONTENT_TEXT;
	add_file(b, "/block")->content = CONTENT_TEXT;
	add_file(3 * b, "/noise")->content = CONTENT_NOISE;
	add_file(0, "/empty");
	emit_compressed(FSIMG_EROFS_COMPRESSION_NONE, &flat);
	emit_compressed(FSIMG_EROFS_COMPRESSION_LZ4, &lz4);
	CHECK(flat.length == lz4.length &&
	    bcmp(flat.bytes, lz4.bytes, (size_t)flat.length) == 0);
	free(flat.bytes);
	free(lz4.bytes);
}

/*
 * A fault in a compressed file's source, raised before planning or between
 * planning and emission, fails the step that reads it with want and leaves
 * no source open; a changed source is reported by the file's path.
 */
static void
check_lz4_fault(const char *path, uint64_t *fault, bool before_planning,
    fsimg_status_t want)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	size_t index = spec_at_path(path);

	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	reproducible_options(&options);
	fixture_open(&fixture);
	if (before_planning)
		*fault = index;
	status = fixture_plan(&fixture, &options, false);
	if (!before_planning) {
		CHECK(status == FSIMG_OK);
		CHECK(row_of_path(&fixture, path).layout ==
		    EROFS_LAYOUT_COMPRESSED_FULL);
		sink_open(&fixture, &sink, POISON);
		*fault = index;
		status = emit_into(&fixture, &sink);
		free(sink.bytes);
	}
	*fault = UINT64_MAX;
	CHECK_CASE(status == want, "%s: status %d, want %d: %s", path, status,
	    want, fixture.error.message);
	CHECK_CASE(want != FSIMG_ESOURCE_CHANGED ||
	    strcmp(skip_slash(fixture.error.path), skip_slash(path)) == 0,
	    "%s: the error names %s", path, fixture.error.path);
	CHECK(!fixture.writer->source.active);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

static void
test_lz4_source_faults_fail_planning_and_emission(void)
{
	build_lz4_tree();
	/* Content that changes after measurement changes the clusters. */
	check_lz4_fault("/text/t300000", &drifted_source, false,
	    FSIMG_ESOURCE_CHANGED);
	check_lz4_fault("/half", &drifted_source, false,
	    FSIMG_ESOURCE_CHANGED);
	check_lz4_fault("/text/t300000", &changed_source, true,
	    FSIMG_ESOURCE_CHANGED);
	check_lz4_fault("/text/t300000", &changed_source, false,
	    FSIMG_ESOURCE_CHANGED);
	check_lz4_fault("/text/t1000003", &short_source, true, FSIMG_EIO);
	check_lz4_fault("/text/t1000003", &short_source, false, FSIMG_EIO);
}

/*
 * Allocation fails at every point of planning and then of emission, and a
 * write fails midway, under compression=lz4.
 */
static void
test_lz4_allocation_and_write_failures_are_clean(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	bool emitting = false, done = false;
	unsigned failures = 0;
	uint64_t writes;

	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	build_lz4_tree();
	reproducible_options(&options);
	for (uint64_t attempt = 1; attempt < 8192 && !done; attempt++) {
		fixture_open(&fixture);
		if (!emitting)
			fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fixture_plan(&fixture, &options, false);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK && !emitting) {
			emitting = true;
			attempt = 0;
		} else if (status == FSIMG_OK) {
			sink_open(&fixture, &sink, POISON);
			fsimg_test_fail_alloc_after(fixture.ctx, attempt);
			status = emit_into(&fixture, &sink);
			fsimg_test_fail_alloc_after(fixture.ctx, 0);
			done = status == FSIMG_OK;
			free(sink.bytes);
		}
		CHECK_CASE(status == FSIMG_OK || status == FSIMG_ENOMEM,
		    "allocation %ju failing gave status %d",
		    (uintmax_t)attempt, status);
		failures += status == FSIMG_ENOMEM;
		CHECK(fixture.writer == NULL || !fixture.writer->source.active);
		fixture_close(&fixture);
	}
	CHECK(done && failures > 0);

	plan_tree(&fixture, &options);
	emit_tree(&fixture, &sink, POISON);
	writes = sink.writes;
	free(sink.bytes);
	fixture_close(&fixture);
	plan_tree(&fixture, &options);
	sink_open(&fixture, &sink, POISON);
	sink.fail_write_at = writes / 2;
	CHECK(emit_into(&fixture, &sink) != FSIMG_OK);
	free(sink.bytes);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

/*
 * Plan a 1 MiB text file under compression=lz4, an output limit and a
 * requested volume, 0 for none. Its least image is the metadata's first
 * blocks and one cluster per 64 KiB window, 16 clusters. The text repeats
 * a 44-byte sentence, so a window of it encodes in far less than 512
 * bytes and the file stores about those 16 clusters at every block size,
 * while the flat image stores 1 MiB.
 */
static fsimg_status_t
plan_lz4_under_output_limit(fixture_t *fixture, uint64_t limit,
    uint64_t volume_bytes)
{
	fsimg_write_options_t options;
	fsimg_options_t context;

	tree_reset();
	add_file(1u << 20, "/text")->content = CONTENT_TEXT;
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	reproducible_options(&options);
	options.volume_bytes = volume_bytes;
	compression = FSIMG_EROFS_COMPRESSION_NONE;
	fsimg_options_init(&context);
	context.max_output_bytes = limit;
	fixture_open_with(fixture, &context, 0);
	provider_reads = 0;
	return (fixture_plan(fixture, &options, false));
}

/*
 * An output limit below the least image the content could compress to
 * refuses the plan before measurement reads a byte, and so does one below
 * a requested volume or, behind a DMG container, below twice the least
 * image, since the raw intermediate holds the disk too. A limit the flat
 * image exceeds but the compressed image fits is accepted, down to exactly
 * the planned output.
 */
static void
test_lz4_output_limit_below_the_least_image_refuses_unread(void)
{
	const uint64_t least = 16 * (uint64_t)block_size;
	fixture_t fixture;
	fsimg_status_t status;
	uint64_t output;

	status = plan_lz4_under_output_limit(&fixture, least, 0);
	CHECK_CASE(status == FSIMG_ELIMIT, "status %d: %s", status,
	    fixture.error.message);
	CHECK(fixture.error.resource == FSIMG_RESOURCE_OUTPUT &&
	    fixture.error.limit == least && fixture.error.observed > least);
	CHECK_CASE(provider_reads == 0, "planning read the source %llu times",
	    (unsigned long long)provider_reads);
	CHECK(!fixture.writer->source.active);
	fixture_close(&fixture);

	status = plan_lz4_under_output_limit(&fixture, 64 * (uint64_t)block_size,
	    65 * (uint64_t)block_size);
	CHECK(status == FSIMG_ELIMIT &&
	    fixture.error.resource == FSIMG_RESOURCE_OUTPUT);
	CHECK(fixture.error.observed == 65 * (uint64_t)block_size);
	CHECK(provider_reads == 0);
	fixture_close(&fixture);

	stack = "erofs/dmg";
	status = plan_lz4_under_output_limit(&fixture, 2 * least, 0);
	stack = NULL;
	CHECK_CASE(status == FSIMG_ELIMIT, "status %d: %s", status,
	    fixture.error.message);
	CHECK(fixture.error.resource == FSIMG_RESOURCE_OUTPUT &&
	    fixture.error.observed > 2 * least &&
	    fixture.error.observed % 2 == 0 &&
	    strstr(fixture.error.message, "raw intermediate") != NULL);
	CHECK(provider_reads == 0);
	fixture_close(&fixture);
	stack = "erofs/dmg";
	status = plan_lz4_under_output_limit(&fixture, 16 * least, 0);
	stack = NULL;
	CHECK_CASE(status == FSIMG_OK, "status %d: %s", status,
	    fixture.error.message);
	fixture_close(&fixture);

	status = plan_lz4_under_output_limit(&fixture, 64 * (uint64_t)block_size,
	    0);
	CHECK_CASE(status == FSIMG_OK, "status %d: %s", status,
	    fixture.error.message);
	CHECK(provider_reads != 0);
	CHECK(row_of_path(&fixture, "/text").layout ==
	    EROFS_LAYOUT_COMPRESSED_FULL);
	output = fixture.plan->common.info.output_bytes;
	fixture_close(&fixture);

	status = plan_lz4_under_output_limit(&fixture, output, 0);
	CHECK_CASE(status == FSIMG_OK, "status %d: %s", status,
	    fixture.error.message);
	fixture_close(&fixture);
	status = plan_lz4_under_output_limit(&fixture, output - 1, 0);
	/* Refused by the floor where it is tight, by the full check else. */
	CHECK(status == FSIMG_ELIMIT &&
	    fixture.error.resource == FSIMG_RESOURCE_OUTPUT);
	fixture_close(&fixture);
}

/*
 * Cluster records and compressed rows come back from scratch storage, so
 * a plan damaged after planning is refused, not emitted: a reserved byte
 * set, a plain flag above one, a stored length beyond the block, a cluster
 * that does not start where the previous one ended, and a row whose
 * records lie beyond the adopted ones.
 */
static void
test_lz4_damaged_cluster_plan_is_refused(void)
{
	static const struct {
		const char	*name;
		size_t		 at;		/* byte of the second record */
		int		 value;		/* -1: add one to the byte */
		fsimg_status_t	 want;
	} cases[] = {
		{ "reserved byte", 13, 1, FSIMG_ESTATE },
		{ "plain flag", 12, 2, FSIMG_ESTATE },
		{ "stored length", 11, 0xff, FSIMG_ESTATE },
		{ "logical start", 0, -1, FSIMG_ESTATE },
		{ "row beyond the records", SIZE_MAX, 0, FSIMG_ECORRUPT },
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	erofs_row_t row;
	unsigned char byte;
	uint64_t id, offset;
	fsimg_status_t status;

	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	build_lz4_tree();
	reproducible_options(&options);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		plan_tree(&fixture, &options);
		id = id_of_path(&fixture, "/text/t300000");
		row = row_at(&fixture, id);
		CHECK(row.layout == EROFS_LAYOUT_COMPRESSED_FULL &&
		    row.clusters >= 2);
		if (cases[i].at == SIZE_MAX) {
			row.first_cluster = fixture.plan->cluster_count;
			CHECK(erofs_row_set(fixture.plan, id, &row, NULL) ==
			    FSIMG_OK);
		} else {
			offset = (row.first_cluster + 1) * 16 + cases[i].at;
			CHECK(fsimg_store_read(fixture.plan->cluster_records,
			    offset, &byte, 1, NULL) == FSIMG_OK);
			byte = cases[i].value < 0 ? (unsigned char)(byte + 1) :
			    (unsigned char)cases[i].value;
			CHECK(fsimg_store_write(fixture.plan->cluster_records,
			    offset, &byte, 1, NULL) == FSIMG_OK);
		}
		sink_open(&fixture, &sink, POISON);
		status = emit_into(&fixture, &sink);
		CHECK_CASE(status == cases[i].want, "%s: status %d: %s",
		    cases[i].name, status, fixture.error.message);
		CHECK_CASE(strcmp(fixture.error.message, cases[i].want ==
		    FSIMG_ESTATE ? "invalid EROFS cluster plan" :
		    "invalid EROFS plan row") == 0, "%s: %s", cases[i].name,
		    fixture.error.message);
		free(sink.bytes);
		fixture_close(&fixture);
	}
	compression = FSIMG_EROFS_COMPRESSION_NONE;
}

/*
 * The map header and full-index records the LZ4 writer encodes decode to
 * the same fields; what a record cannot hold, or the writer never emits,
 * is refused.
 */
static void
test_index_encoding_decodes_to_the_same_records(void)
{
	const erofs_zrecord_t records[] = {
		{ .type = EROFS_RECORD_HEAD1, .clusterofs = 511,
		    .block = UINT32_MAX },
		{ .type = EROFS_RECORD_PLAIN, .clusterofs = 7, .block = 12 },
		{ .type = EROFS_RECORD_NONHEAD, .backward = 129 },
	};
	const erofs_zheader_t header = { .advice = 0 };
	erofs_zrecord_t decoded, bad;
	erofs_zheader_t zh;
	erofs_super_t sb;
	unsigned char raw[16];
	const char *what;

	bzero(&sb, sizeof(sb));
	sb.incompat = EROFS_INCOMPAT_LZ4_0PADDING;
	memset(raw, 0xff, sizeof(raw));
	CHECK(erofs_zheader_encode(&header, raw));
	for (size_t i = 0; i < EROFS_ZHEADER_BYTES; i++)
		CHECK(raw[i] == 0);
	CHECK(erofs_zheader_decode(raw, &sb, EROFS_LAYOUT_COMPRESSED_FULL,
	    &zh, &what) == FSIMG_OK);
	CHECK(zh.advice == 0 && zh.head1_algorithm == EROFS_ALGORITHM_LZ4 &&
	    !zh.whole_fragment);
	zh = (erofs_zheader_t){ .whole_fragment = true };
	CHECK(!erofs_zheader_encode(&zh, raw));
	zh = (erofs_zheader_t){ .advice = EROFS_ADVICE_INLINE_TAIL,
	    .tail_bytes = 5 };
	CHECK(!erofs_zheader_encode(&zh, raw));

	for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
		memset(raw, 0xff, sizeof(raw));
		CHECK(erofs_full_record_encode(&records[i], 3, raw));
		CHECK(erofs_full_record_decode(raw, false, &decoded, &what) ==
		    FSIMG_OK);
		CHECK(decoded.type == records[i].type &&
		    decoded.clusterofs == records[i].clusterofs &&
		    decoded.block == records[i].block &&
		    decoded.backward == records[i].backward && !decoded.marker);
		if (records[i].type == EROFS_RECORD_NONHEAD)
			CHECK(fsimg_load_le16(raw + 6) == 3 &&
			    fsimg_load_le16(raw + 2) == 0);
	}
	bad = (erofs_zrecord_t){ .type = EROFS_RECORD_HEAD1,
	    .block = UINT64_C(1) << 32 };
	CHECK(!erofs_full_record_encode(&bad, 0, raw));
	bad = (erofs_zrecord_t){ .type = EROFS_RECORD_NONHEAD };
	CHECK(!erofs_full_record_encode(&bad, 1, raw));
	bad = (erofs_zrecord_t){ .type = EROFS_RECORD_NONHEAD,
	    .backward = 1, .marker = true, .marker_blocks = 2 };
	CHECK(!erofs_full_record_encode(&bad, 1, raw));
}

/*
 * The map header and 32-byte encoded extents the 48-bit LZ4 writer encodes
 * decode to the same fields, a count and offsets past 32 bits included,
 * and so do length words of each format; a count whose bit 47 the decoder
 * would read as the whole-file mark, another width or advice, and a length
 * word the decoder refuses or would not reproduce are refused.
 */
static void
test_extent_encoding_decodes_to_the_same_records(void)
{
	const erofs_xlength_t lengths[] = {
		{ .stored_bytes = 4096, .format = EROFS_ALGORITHM_LZ4 + 1 },
		{ .stored_bytes = 512, .format = EROFS_ALGORITHM_LZ4 + 1,
		    .partial = true },
		{ .stored_bytes = EROFS_CLUSTER_STORED_MAX },
	};
	const erofs_xrecord_t record = {
		.physical = (UINT64_C(1) << 40) + 4096,
		.logical = (UINT64_C(1) << 33) + 7,
		.length_word = 0x10001000u
	};
	erofs_zheader_t header = {
		.extents_form = EROFS_EXTENTS_SEARCHED,
		.extent_bytes = EROFS_EXTENT32_BYTES,
		.advice = EROFS_EXTENT32_ADVICE,
		.extent_count = (UINT64_C(1) << 32) + 5
	}, zh;
	erofs_xrecord_t xrecord;
	erofs_xlength_t xlength, bad;
	erofs_super_t sb;
	unsigned char raw[EROFS_EXTENT32_BYTES];
	const char *what;
	uint32_t word;

	bzero(&sb, sizeof(sb));
	sb.blkszbits = 12;
	sb.incompat = EROFS_INCOMPAT_48BIT | EROFS_INCOMPAT_LZ4_0PADDING;
	memset(raw, 0xff, sizeof(raw));
	CHECK(erofs_zheader_encode(&header, raw));
	CHECK(fsimg_load_le32(raw) == 5 &&
	    fsimg_load_le16(raw + 4) == EROFS_EXTENT32_ADVICE &&
	    fsimg_load_le16(raw + 6) == 1);
	CHECK(erofs_zheader_decode(raw, &sb, EROFS_LAYOUT_COMPRESSED_FULL,
	    &zh, &what) == FSIMG_OK);
	CHECK(zh.extents_form == EROFS_EXTENTS_SEARCHED &&
	    zh.extent_bytes == EROFS_EXTENT32_BYTES &&
	    zh.advice == EROFS_EXTENT32_ADVICE &&
	    zh.extent_count == header.extent_count && !zh.whole_fragment);
	header.extent_count = (UINT64_C(1) << 47) - 1;
	CHECK(erofs_zheader_encode(&header, raw));
	CHECK(erofs_zheader_decode(raw, &sb, EROFS_LAYOUT_COMPRESSED_FULL,
	    &zh, &what) == FSIMG_OK);
	CHECK(!zh.whole_fragment && zh.extent_count == header.extent_count);
	header.extent_count = UINT64_C(1) << 47;
	CHECK(!erofs_zheader_encode(&header, raw));
	header.extent_count = 1;
	header.extent_bytes = 16;
	CHECK(!erofs_zheader_encode(&header, raw));
	header.extent_bytes = EROFS_EXTENT32_BYTES;
	header.advice |= EROFS_ADVICE_FRAGMENT;
	CHECK(!erofs_zheader_encode(&header, raw));
	header.advice = EROFS_EXTENT32_ADVICE;
	header.head2_algorithm = 1;
	CHECK(!erofs_zheader_encode(&header, raw));
	header = (erofs_zheader_t){ .extent_count = 1 };
	CHECK(!erofs_zheader_encode(&header, raw));
	header = (erofs_zheader_t){ .advice = EROFS_ADVICE_EXTENTS };
	CHECK(!erofs_zheader_encode(&header, raw));
	header = (erofs_zheader_t){ .lclusterbits = 3 };
	CHECK(!erofs_zheader_encode(&header, raw));

	memset(raw, 0xff, sizeof(raw));
	CHECK(erofs_xrecord_encode(&record, EROFS_EXTENT32_BYTES, raw));
	for (size_t i = 20; i < sizeof(raw); i++)
		CHECK(raw[i] == 0);
	CHECK(erofs_xrecord_decode(raw, EROFS_EXTENT32_BYTES, &xrecord,
	    &what) == FSIMG_OK);
	CHECK(xrecord.physical == record.physical &&
	    xrecord.logical == record.logical &&
	    xrecord.length_word == record.length_word);
	CHECK(!erofs_xrecord_encode(&record, 16, raw));

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		CHECK(erofs_xlength_encode(&lengths[i], &word));
		CHECK(erofs_xlength_decode(word, &xlength, &what) == FSIMG_OK);
		CHECK(xlength.stored_bytes == lengths[i].stored_bytes &&
		    xlength.format == lengths[i].format &&
		    xlength.partial == lengths[i].partial);
	}
	bad = (erofs_xlength_t){ .format = EROFS_ALGORITHM_LZ4 + 1 };
	CHECK(!erofs_xlength_encode(&bad, &word));
	bad.stored_bytes = EROFS_CLUSTER_STORED_MAX + 1;
	CHECK(!erofs_xlength_encode(&bad, &word));
	bad.stored_bytes = 4096;
	bad.format = 0x10;
	CHECK(!erofs_xlength_encode(&bad, &word));
	bad = (erofs_xlength_t){ .stored_bytes = 4096, .partial = true };
	CHECK(!erofs_xlength_encode(&bad, &word));
}

/*
 * The encoders.
 */

static erofs_super_t
sample_super(void)
{
	erofs_super_t sb;

	bzero(&sb, sizeof(sb));
	sb.compat = EROFS_COMPAT_SB_CHKSUM | EROFS_COMPAT_MTIME;
	sb.blkszbits = 12;
	sb.root_nid = 36;
	sb.inode_count = 9;
	sb.block_count = 17;
	sb.epoch = EPOCH;
	for (unsigned i = 0; i < sizeof(sb.uuid); i++)
		sb.uuid[i] = (uint8_t)(0x40 + i);
	bcopy("label", sb.name, 5);
	sb.name_length = 5;
	return (sb);
}

static void
test_super_encoding_decodes_to_the_same_superblock(void)
{
	unsigned char region[SUPER_REGION];
	erofs_super_t sb = sample_super(), decoded;
	erofs_config_t config;
	fsimg_status_t status;
	const char *what = NULL;

	bzero(region, sizeof(region));
	CHECK(erofs_super_encode(&sb, region));
	CHECK(erofs_super_checksum(region, sizeof(region), sb.blkszbits,
	    &sb.checksum));
	CHECK(erofs_super_encode(&sb, region));
	status = erofs_region_decode(region, sizeof(region), &decoded, &config,
	    &what);
	CHECK_CASE(status == FSIMG_OK, "decoding refused: %s", what);

	CHECK(bcmp(decoded.uuid, sb.uuid, sizeof(sb.uuid)) == 0);
	CHECK(decoded.name_length == 5 && bcmp(decoded.name, "label", 5) == 0);
	CHECK(decoded.root_nid == sb.root_nid);
	CHECK(decoded.inode_count == sb.inode_count);
	CHECK(decoded.block_count == sb.block_count);
	CHECK(decoded.epoch == sb.epoch && decoded.epoch_nsec == 0);
	CHECK(decoded.compat == sb.compat && decoded.incompat == 0);
	CHECK(decoded.checksum == sb.checksum);
	CHECK(decoded.blkszbits == sb.blkszbits);
	CHECK(decoded.extension_slots == 0 && decoded.box_nid == 0);
}

static void
test_super_encoding_refuses_what_it_cannot_store(void)
{
	unsigned char raw[EROFS_SUPER_BASE_BYTES];
	erofs_super_t sb = sample_super();

	CHECK(erofs_super_encode(&sb, raw));

	sb.incompat = EROFS_INCOMPAT_METABOX;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.incompat = EROFS_INCOMPAT_COMPR_CFGS;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.compat |= EROFS_COMPAT_XATTR_FILTER;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.extension_slots = 1;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.root_nid = UINT16_MAX + 1;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.block_count = UINT64_C(1) << 32;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.blkszbits = EROFS_BLKSZBITS_MAX + 1;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.epoch_nsec = 1000000000;
	CHECK(!erofs_super_encode(&sb, raw));

	sb = sample_super();
	sb.name[1] = '\0';
	sb.name_length = 3;
	CHECK(!erofs_super_encode(&sb, raw));
}

/* A compact inode fits the sample superblock's epoch and every field. */
static erofs_inode_t
sample_inode(bool extended)
{
	erofs_inode_t inode;

	bzero(&inode, sizeof(inode));
	inode.kind = FSIMG_ENTRY_FILE;
	inode.layout = EROFS_LAYOUT_FLAT_INLINE;
	inode.extended = extended;
	inode.permissions = 0640;
	inode.nlink = 1;
	inode.size = 5000;
	inode.layout_word = 77;
	inode.serial = 9;
	inode.uid = 1000;
	inode.gid = 100;
	inode.time = EPOCH + 3600;
	return (inode);
}

static void
check_inode_round_trip(const erofs_inode_t *inode, size_t fixed_bytes)
{
	const erofs_super_t sb = sample_super();
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_inode_t decoded;
	fsimg_status_t status;
	const char *what = NULL;

	bzero(raw, sizeof(raw));
	CHECK(erofs_inode_encode(inode, &sb, raw));
	status = erofs_inode_decode(raw, fixed_bytes, &sb, &decoded, &what);
	CHECK_CASE(status == FSIMG_OK, "decoding refused: %s", what);
	CHECK(decoded.fixed_bytes == fixed_bytes);
	CHECK(decoded.extended == inode->extended);
	CHECK(decoded.kind == inode->kind && decoded.layout == inode->layout);
	CHECK(decoded.permissions == inode->permissions);
	CHECK(decoded.nlink == inode->nlink && decoded.size == inode->size);
	CHECK(decoded.layout_word == inode->layout_word);
	CHECK(decoded.serial == inode->serial);
	CHECK(decoded.uid == inode->uid && decoded.gid == inode->gid);
	CHECK(decoded.time == inode->time);
	CHECK(decoded.time_nsec == inode->time_nsec);
}

static void
test_compact_inode_encoding_decodes_to_the_same_inode(void)
{
	erofs_inode_t inode = sample_inode(false);

	check_inode_round_trip(&inode, EROFS_INODE_COMPACT_BYTES);
	inode.kind = FSIMG_ENTRY_DIRECTORY;
	inode.layout = EROFS_LAYOUT_FLAT;
	inode.nlink = 3;
	inode.time = EPOCH;
	check_inode_round_trip(&inode, EROFS_INODE_COMPACT_BYTES);
}

static void
test_extended_inode_encoding_decodes_to_the_same_inode(void)
{
	erofs_inode_t inode = sample_inode(true);

	inode.uid = 70000;
	inode.gid = 4000000000u;
	inode.size = (UINT64_C(1) << 33) + 100;
	inode.time = -5;
	inode.time_nsec = 999999999;
	inode.nlink = 70000;
	check_inode_round_trip(&inode, EROFS_INODE_EXTENDED_BYTES);
}

static void
test_compact_inode_encoding_refuses_what_it_cannot_store(void)
{
	const erofs_super_t sb = sample_super();
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_inode_t inode = sample_inode(false);

	CHECK(erofs_inode_encode(&inode, &sb, raw));

	inode.uid = 70000;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode = sample_inode(false);
	inode.gid = 70000;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode = sample_inode(false);
	inode.size = UINT64_C(1) << 32;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode = sample_inode(false);
	inode.time = sb.epoch - 1;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode = sample_inode(false);
	inode.time_nsec = sb.epoch_nsec + 1;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));

	/* The same record fits the extended form. */
	inode.extended = true;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
}

/*
 * 48-bit addressing (specs/design-erofs.md, 48-bit profile). The
 * structural tests decode the emitted bytes themselves, so they need no
 * reader; the round trips read the image back once the reader opens a
 * 48BIT volume.
 */

#define SB_BLOCKS_HIGH		0x0e	/* the 16-bit root NID field */
#define SB_BLOCKS_LOW		0x24
#define SB_WIDE_ROOT		0x70
#define INODE_FORMAT_BIT4	0x0010u
#define INODE_HIGH_WORD		0x06
#define INODE_COMPACT_TIME	0x0c
#define EXTENT_LZ4		1u

/* The superblock region of an image, decoded and its checksum checked. */
static erofs_super_t
decode_image_super(const memory_sink_t *sink)
{
	const size_t length = sink->length - EROFS_SUPER_OFFSET <
	    SUPER_REGION ? (size_t)(sink->length - EROFS_SUPER_OFFSET) :
	    SUPER_REGION;
	erofs_super_t sb;
	erofs_config_t config;
	const char *what = NULL;
	fsimg_status_t status;

	status = erofs_region_decode(sink->bytes + EROFS_SUPER_OFFSET, length,
	    &sb, &config, &what);
	CHECK_CASE(status == FSIMG_OK, "superblock refused: %s", what);
	return (sb);
}

/*
 * The emitted superblock is the wide form on a small volume: 48BIT, the
 * root NID in the 64-bit field and the block count's high bits, zero, in
 * the 16-bit one. The epoch is the smallest modification second, the time
 * before 1970 in the round-trip tree. Every directory is extended without
 * format bit 4, so it keeps its "." entry, and has the size the 32-bit
 * plan gives it; every compact inode, never a directory, carries the
 * implicit link count with zero start-block bits and stores its time as
 * the offset from the epoch, nonzero for most.
 */
static void
test_addressing48_image_structure(void)
{
	fsimg_write_options_t options;
	fixture_t fixture, narrow;
	memory_sink_t sink;
	erofs_super_t sb;
	const unsigned char *raw;
	uint64_t offset_times = 0;

	build_round_trip_tree();
	unreproducible_options(&options);
	plan_tree(&narrow, &options);
	addressing = 48;
	unreproducible_options(&options);
	plan_tree(&fixture, &options);
	emit_tree(&fixture, &sink, POISON);

	sb = decode_image_super(&sink);
	raw = sink.bytes + EROFS_SUPER_OFFSET;
	CHECK(sb.incompat == EROFS_INCOMPAT_48BIT);
	CHECK(sb.root_nid == row_at(&fixture, 0).nid && sb.root_nid == 36);
	CHECK(fsimg_load_le64(raw + SB_WIDE_ROOT) == sb.root_nid);
	CHECK(fsimg_load_le16(raw + SB_BLOCKS_HIGH) == 0);
	CHECK(fsimg_load_le32(raw + SB_BLOCKS_LOW) ==
	    fixture.plan->block_count);
	CHECK(sb.block_count == fixture.plan->block_count);
	CHECK(sb.epoch == row_of_path(&fixture, "/owners/before-1970").mtime);
	CHECK(sb.epoch < 0 && sb.epoch_nsec == 0);

	for (uint64_t id = 0; id <= fixture.writer->count; id++) {
		const erofs_row_t row = row_at(&fixture, id);
		const unsigned char *inode = sink.bytes + row.nid * SLOT;
		const uint16_t format = fsimg_load_le16(inode);
		const bool directory = (row.mode & EROFS_MODE_TYPE_MASK) ==
		    EROFS_MODE_DIRECTORY;
		uint32_t offset;

		CHECK_CASE((format & 1) == row.extended &&
		    (row.extended || !directory), "id %ju: form",
		    (uintmax_t)id);
		CHECK_CASE(((format & INODE_FORMAT_BIT4) != 0) == !row.extended,
		    "id %ju: format bit 4", (uintmax_t)id);
		CHECK(fsimg_load_le16(inode + INODE_HIGH_WORD) == 0);
		if (directory)
			CHECK_CASE(row.size == row_at(&narrow, id).size,
			    "id %ju: directory size", (uintmax_t)id);
		if (row.extended)
			continue;
		offset = fsimg_load_le32(inode + INODE_COMPACT_TIME);
		CHECK_CASE(sb.epoch + offset == row.mtime &&
		    row.mtime_nsec == 0, "id %ju: compact time", (uintmax_t)id);
		offset_times += offset != 0;
	}
	CHECK(offset_times > 0);
	check_enumeration(&fixture, &sink, false);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
	fixture_close(&narrow);
	addressing = 0;
}

/*
 * The compact form takes a whole second from the epoch to 2^32 - 1
 * seconds after it; a second later, or any nanoseconds, is extended. The
 * epoch is the smallest second, here before the host clock's format time,
 * and the last compact inode stores the largest offset.
 */
static void
test_addressing48_compact_time_range(void)
{
	const int64_t far = OLD_TIME + (INT64_C(1) << 32);
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	tree_reset();
	with_time(add_file(10, "/first"), OLD_TIME, 0);
	with_time(add_file(10, "/last-compact"), far - 1, 0);
	with_time(add_file(10, "/too-far"), far, 0);
	with_time(add_file(10, "/nanoseconds"), OLD_TIME + 7, 1);
	add_file(10, "/absent");
	add_directory("/dir");
	addressing = 48;
	unreproducible_options(&options);
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->epoch == OLD_TIME);
	CHECK(fixture.plan->format_time > OLD_TIME);
	CHECK(!row_of_path(&fixture, "/first").extended);
	CHECK(!row_of_path(&fixture, "/last-compact").extended);
	CHECK(!row_of_path(&fixture, "/absent").extended);
	CHECK(row_of_path(&fixture, "/too-far").extended);
	CHECK(row_of_path(&fixture, "/nanoseconds").extended);
	CHECK(row_of_path(&fixture, "/dir").extended);
	CHECK(row_at(&fixture, 0).extended);
	emit_tree(&fixture, &sink, POISON);
	CHECK(fsimg_load_le32(sink.bytes + row_of_path(&fixture,
	    "/last-compact").nid * SLOT + INODE_COMPACT_TIME) == UINT32_MAX);
	free(sink.bytes);
	fixture_close(&fixture);
	addressing = 0;
}

/*
 * Check one compressed row's map header and 32-byte encoded extents in the
 * image and decode the file through them: the count is the row's clusters,
 * at most ceil(size / B); each record stores B bytes, LZ4 or plain, at
 * the row's next block, from a logical offset that starts at 0 and
 * increases strictly below size, and its reserved bytes are zero.
 */
static void
check_encoded_extents(fixture_t *fixture, const memory_sink_t *sink,
    uint64_t id, unsigned char *decoded)
{
	const erofs_row_t row = row_at(fixture, id);
	const size_t index = spec_for_id(fixture, id);
	const unsigned char *header = sink->bytes + row.nid * SLOT +
	    erofs_row_fixed_bytes(&row);
	const unsigned char *extent, *block;
	uint64_t logical, end, physical;
	fsimg_codec_step_t step;
	uint32_t word;
	size_t start;

	CHECK(fsimg_load_le32(header) == row.clusters);
	CHECK(fsimg_load_le16(header + 4) == EROFS_EXTENT32_ADVICE);
	CHECK(fsimg_load_le16(header + 6) == 0);
	for (size_t i = 8; i < EROFS_EXTENT32_OFFSET; i++)
		CHECK(header[i] == 0);
	CHECK(row.clusters <= (row.size + block_size - 1) / block_size);

	for (uint32_t i = 0; i < row.clusters; i++) {
		extent = header + EROFS_EXTENT32_OFFSET + i * 32u;
		word = fsimg_load_le32(extent);
		physical = fsimg_load_le32(extent + 4) |
		    (uint64_t)fsimg_load_le32(extent + 8) << 32;
		logical = fsimg_load_le32(extent + 12) |
		    (uint64_t)fsimg_load_le32(extent + 16) << 32;
		end = i + 1 == row.clusters ? row.size :
		    fsimg_load_le32(extent + 32 + 12) |
		    (uint64_t)fsimg_load_le32(extent + 32 + 16) << 32;
		CHECK_CASE((word & 0x0fffffffu) == block_size &&
		    word >> 28 <= EXTENT_LZ4, "id %ju extent %u: length word "
		    "%#x", (uintmax_t)id, i, word);
		CHECK(physical == (row.start_block + i) * block_size);
		CHECK((i != 0 || logical == 0) && logical < end &&
		    end <= row.size && end - logical <= LZ4_WINDOW);
		for (size_t k = 20; k < 32; k++)
			CHECK(extent[k] == 0);

		block = sink->bytes + physical;
		if (word >> 28 == EXTENT_LZ4) {
			for (start = 0; start < block_size && block[start] == 0;
			    start++)
				continue;
			CHECK(fsimg_lz4_decode(block + start, block_size - start,
			    decoded, (size_t)(end - logical), &step, NULL) ==
			    FSIMG_OK && step.produced == end - logical);
		} else
			bcopy(block, decoded, (size_t)(end - logical));
		for (uint64_t k = 0; k < end - logical; k++)
			CHECK_CASE(decoded[k] == spec_byte(index, logical + k),
			    "id %ju byte %ju", (uintmax_t)id,
			    (uintmax_t)(logical + k));
	}
}

/*
 * Under addressing=48 with compression=lz4 every compressed file is mapped
 * by 32-byte encoded extents that decode to its content, and the
 * superblock adds LZ4_0PADDING and LZ4's maximum distance to 48BIT.
 */
static void
test_addressing48_lz4_files_are_encoded_extents(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	erofs_super_t sb;
	unsigned char *decoded;
	uint64_t compressed = 0;

	decoded = malloc(LZ4_WINDOW);
	CHECK(decoded != NULL);
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	addressing = 48;
	build_lz4_tree();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	check_lz4_layouts(&fixture);
	emit_tree(&fixture, &sink, POISON);
	sb = decode_image_super(&sink);
	CHECK(sb.incompat ==
	    (EROFS_INCOMPAT_48BIT | EROFS_INCOMPAT_LZ4_0PADDING));
	CHECK(sb.compression_word == 65535);
	for (uint64_t id = 1; id <= fixture.writer->count; id++) {
		if (row_at(&fixture, id).layout != EROFS_LAYOUT_COMPRESSED_FULL)
			continue;
		check_encoded_extents(&fixture, &sink, id, decoded);
		compressed++;
	}
	CHECK(compressed > 5);
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	free(decoded);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
	addressing = 0;
}

/*
 * A file of more encoded extents than the emission buffer holds, 2047
 * after the map header, is written across buffer flushes: 2100 windows of
 * text at 512-byte blocks, one cluster each.
 */
static void
test_addressing48_extents_cross_the_staging_buffer(void)
{
	const uint64_t size = 2100 * (uint64_t)LZ4_WINDOW;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	unsigned char *decoded;
	uint64_t id;

	decoded = malloc(LZ4_WINDOW);
	CHECK(decoded != NULL);
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	addressing = 48;
	block_size = 512;
	tree_reset();
	add_file(size, "/text")->content = CONTENT_TEXT;
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	id = id_of_path(&fixture, "/text");
	CHECK(row_at(&fixture, id).clusters == 2100);
	CHECK(2100 * EROFS_EXTENT32_BYTES > FSIMG_EMIT_BUFFER_BYTES);
	emit_tree(&fixture, &sink, POISON);
	check_encoded_extents(&fixture, &sink, id, decoded);
	free(sink.bytes);
	free(decoded);
	fixture_close(&fixture);
	block_size = BLOCK_DEFAULT;
	compression = FSIMG_EROFS_COMPRESSION_NONE;
	addressing = 0;
}

/*
 * The 48-bit image is the same bytes whatever the insertion order, budget,
 * scratch or time zone, and leaves no stale byte, with and without LZ4.
 */
static void
test_addressing48_image_is_reproducible_and_clean(void)
{
	addressing = 48;
	check_identity_variants(build_round_trip_tree);
	check_no_stale_byte(build_round_trip_tree);
	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	check_identity_variants(build_lz4_tree);
	check_no_stale_byte(build_lz4_tree);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
	addressing = 0;
}

/*
 * A volume size whose block count needs more than 32 bits is accepted
 * under addressing=48 and gives the plan that block count; one past 48
 * bits, or past 32 without the setting, is invalid geometry.
 */
static void
test_addressing48_volume_size_passes_32_bits(void)
{
	const uint64_t blocks = (UINT64_C(1) << 32) + 1000;
	fsimg_write_options_t options;
	fixture_t fixture;
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer = NULL;

	addressing = 48;
	tree_reset();
	add_file(5000, "/file");
	reproducible_options(&options);
	options.volume_bytes = blocks * block_size;
	plan_tree(&fixture, &options);
	CHECK(fixture.plan->block_count == blocks);
	CHECK(fixture.plan->volume_bytes == blocks * block_size);
	fixture_close(&fixture);

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	options.volume_bytes = (UINT64_C(1) << 48) * block_size;
	CHECK(new_writer(ctx, &options, &writer, NULL) == FSIMG_EINVAL);
	options.erofs_addressing = 32;
	options.volume_bytes = blocks * block_size;
	CHECK(new_writer(ctx, &options, &writer, NULL) == FSIMG_EINVAL);
	CHECK(writer == NULL);
	fsimg_ctx_free(ctx);
	addressing = 0;
}

#define FAR_START	((UINT64_C(1) << 32) + 8)

/*
 * Plan a flat file of noise and an LZ4 file of text under addressing=48 on
 * a volume of 2^32 + 64 blocks, move both to blocks from FAR_START, and
 * emit into a sparse sink poisoned with poison whose far window holds
 * them.
 */
static void
emit_far_files(fixture_t *fixture, memory_sink_t *sink, unsigned char poison)
{
	const char *const paths[] = { "/flat", "/packed" };
	fsimg_write_options_t options;
	erofs_row_t row;
	uint64_t next = FAR_START;
	fsimg_status_t status;

	tree_reset();
	add_file(3 * block_size + 100, "/flat")->content = CONTENT_NOISE;
	add_file(4 * block_size, "/packed")->content = CONTENT_TEXT;
	reproducible_options(&options);
	options.volume_bytes = (FAR_START + 56) * block_size;
	plan_tree(fixture, &options);
	CHECK(row_of_path(fixture, "/flat").layout !=
	    EROFS_LAYOUT_COMPRESSED_FULL);
	CHECK(row_of_path(fixture, "/packed").layout ==
	    EROFS_LAYOUT_COMPRESSED_FULL);

	/*
	 * The dense part is the volume as planned, taken before the rows
	 * move; used_blocks and data_end then grow to the far window's end,
	 * so the emitter's row_placed() accepts the moved rows.
	 */
	bzero(sink, sizeof(*sink));
	sink->length = options.volume_bytes;
	sink->dense_length = fixture->plan->used_blocks * block_size;
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		row = row_of_path(fixture, paths[i]);
		row.start_block = next;
		next += erofs_row_data_blocks(&row, block_size);
		CHECK(erofs_row_set(fixture->plan, id_of_path(fixture,
		    paths[i]), &row, &fixture->error) == FSIMG_OK);
	}
	fixture->plan->used_blocks = next;
	fixture->plan->data_end = next;
	sink->far_offset = FAR_START * block_size;
	sink->far_length = (next - FAR_START) * block_size;
	sink->bytes = malloc((size_t)sink->dense_length);
	sink->far = malloc((size_t)sink->far_length);
	CHECK(sink->bytes != NULL && sink->far != NULL);
	memset(sink->bytes, poison, (size_t)sink->dense_length);
	memset(sink->far, poison, (size_t)sink->far_length);
	status = emit_into(fixture, sink);
	CHECK_CASE(status == FSIMG_OK, "emission failed with %d: %s", status,
	    fixture->error.message);
}

/*
 * Start blocks past 2^32, which only more than 2 TiB of stored content
 * reaches, planted by emit_far_files(). The flat inode carries the high
 * bits in its word at 0x06 and the compressed file's first encoded extent
 * a physical offset past 2^32 blocks; two emissions over different poison
 * bytes are identical, so the sink's zero_range cleared the blocks the
 * files left and every slack byte; and the reader enumerates and verifies
 * both files' content from there.
 */
static void
test_addressing48_start_blocks_pass_32_bits(void)
{
	fixture_t fixture, again;
	memory_sink_t sink, other;
	erofs_super_t sb;
	erofs_inode_t inode;
	erofs_xrecord_t record;
	erofs_row_t row;
	const char *what = NULL;

	compression = FSIMG_EROFS_COMPRESSION_LZ4;
	addressing = 48;
	emit_far_files(&fixture, &sink, POISON);
	emit_far_files(&again, &other, POISON_ALT);
	CHECK(memcmp(sink.bytes, other.bytes, (size_t)sink.dense_length) == 0);
	CHECK(memcmp(sink.far, other.far, (size_t)sink.far_length) == 0);
	free(other.bytes);
	free(other.far);
	fixture_close(&again);

	sb = decode_image_super(&sink);
	row = row_of_path(&fixture, "/flat");
	CHECK(erofs_inode_decode(sink.bytes + row.nid * SLOT,
	    erofs_row_fixed_bytes(&row), &sb, &inode, &what) == FSIMG_OK);
	CHECK(inode.layout_high == 1 &&
	    inode.layout_word == (uint32_t)FAR_START);
	row = row_of_path(&fixture, "/packed");
	CHECK(erofs_xrecord_decode(sink.bytes + row.nid * SLOT +
	    erofs_row_fixed_bytes(&row) + EROFS_EXTENT32_OFFSET,
	    EROFS_EXTENT32_BYTES, &record, &what) == FSIMG_OK);
	CHECK(record.physical == row.start_block * block_size &&
	    row.start_block > UINT32_MAX);
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	free(sink.bytes);
	free(sink.far);
	fixture_close(&fixture);
	compression = FSIMG_EROFS_COMPRESSION_NONE;
	addressing = 0;
}

/* Source, allocation, write and cluster-plan faults fail the 48-bit image. */
static void
test_addressing48_faults_are_clean(void)
{
	addressing = 48;
	test_write_failure_fails_emission_at_any_write();
	test_lz4_source_faults_fail_planning_and_emission();
	test_lz4_allocation_and_write_failures_are_clean();
	test_lz4_damaged_cluster_plan_is_refused();
	addressing = 0;
}

/*
 * Under 48BIT the superblock encodes as the wide form, the block count's
 * bits 32 to 47 in the 16-bit field, and decodes to the same values; a
 * root NID of zero, which selects the narrow form, or a block count past
 * 48 bits is refused.
 */
static void
test_wide_super_encoding_decodes_to_the_same_superblock(void)
{
	unsigned char region[SUPER_REGION];
	erofs_super_t sb = sample_super(), decoded;
	erofs_config_t config;
	const char *what = NULL;

	sb.incompat = EROFS_INCOMPAT_48BIT;
	sb.block_count = (UINT64_C(1) << 32) + 3;
	bzero(region, sizeof(region));
	CHECK(erofs_super_encode(&sb, region));
	CHECK(fsimg_load_le16(region + SB_BLOCKS_HIGH) == 1);
	CHECK(fsimg_load_le32(region + SB_BLOCKS_LOW) == 3);
	CHECK(fsimg_load_le64(region + SB_WIDE_ROOT) == 36);
	CHECK(erofs_super_checksum(region, sizeof(region), sb.blkszbits,
	    &sb.checksum));
	CHECK(erofs_super_encode(&sb, region));
	CHECK(erofs_region_decode(region, sizeof(region), &decoded, &config,
	    &what) == FSIMG_OK);
	CHECK(decoded.root_nid == 36 && decoded.block_count == sb.block_count);
	CHECK(decoded.incompat == EROFS_INCOMPAT_48BIT);

	sb.root_nid = 0;
	CHECK(!erofs_super_encode(&sb, region));
	sb.root_nid = UINT64_C(1) << 40;
	CHECK(erofs_super_encode(&sb, region));
	sb.block_count = UINT64_C(1) << 48;
	CHECK(!erofs_super_encode(&sb, region));
}

/*
 * The implicit link count: a compact non-directory of a 48BIT volume with
 * one link always sets format bit 4 and stores its start block's high bits
 * in the link-count word, and decodes to the same inode; a FIFO takes it
 * with zero bits, and the same inode stored without the bit re-encodes
 * with it. A directory, an extended inode, another link count and a volume
 * without 48BIT keep the bit clear, and compact start-block bits without
 * it are refused.
 */
static void
test_implicit_link_count_encoding(void)
{
	erofs_super_t sb = sample_super();
	unsigned char raw[EROFS_INODE_EXTENDED_BYTES];
	erofs_inode_t inode = sample_inode(false), decoded;
	const char *what = NULL;

	sb.incompat = EROFS_INCOMPAT_48BIT;
	inode.time = EPOCH;
	inode.layout_high = 0x1234;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) != 0);
	CHECK(fsimg_load_le16(raw + INODE_HIGH_WORD) == 0x1234);
	CHECK(erofs_inode_decode(raw, EROFS_INODE_COMPACT_BYTES, &sb,
	    &decoded, &what) == FSIMG_OK);
	CHECK(decoded.nlink == 1 && decoded.layout_high == 0x1234 &&
	    decoded.layout_word == 77);

	/* Stored with the bit clear, the same inode decodes and re-encodes
	 * with it. */
	inode.layout_high = 0;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	fsimg_store_le16(raw, fsimg_load_le16(raw) & ~INODE_FORMAT_BIT4);
	fsimg_store_le16(raw + INODE_HIGH_WORD, 1);
	CHECK(erofs_inode_decode(raw, EROFS_INODE_COMPACT_BYTES, &sb,
	    &decoded, &what) == FSIMG_OK);
	CHECK(erofs_inode_encode(&decoded, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) != 0);
	CHECK(fsimg_load_le16(raw + INODE_HIGH_WORD) == 0);

	inode.kind = FSIMG_ENTRY_FIFO;
	inode.layout = EROFS_LAYOUT_FLAT;
	inode.size = 0;
	inode.layout_word = 0;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) != 0);

	inode = sample_inode(false);
	inode.time = EPOCH;
	inode.nlink = 2;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) == 0);
	CHECK(fsimg_load_le16(raw + INODE_HIGH_WORD) == 2);
	inode.layout_high = 1;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode.nlink = 1;
	inode.layout_high = 0;
	inode.extended = true;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) == 0);
	inode.extended = false;
	inode.kind = FSIMG_ENTRY_DIRECTORY;
	inode.size = UINT32_C(1) << sb.blkszbits;
	inode.layout = EROFS_LAYOUT_FLAT;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) == 0);
	inode.layout_high = 1;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
	inode = sample_inode(false);
	inode.time = EPOCH;
	sb.incompat = 0;
	CHECK(erofs_inode_encode(&inode, &sb, raw));
	CHECK((fsimg_load_le16(raw) & INODE_FORMAT_BIT4) == 0);
	inode.layout_high = 1;
	CHECK(!erofs_inode_encode(&inode, &sb, raw));
}

/*
 * The metadata box.
 */

#define SB_BOX_NID		0x80
#define BOX_NID			37u

/* Whether row id is one the box holds: all but a 32-bit root. */
static bool
expect_in_box(uint64_t id)
{
	return (addressing == 48 || id != 0);
}

/*
 * The heads of the box inode's clusters, read from the image: each
 * cluster's logical start, its block and whether it is plain, from the
 * full index at addressing=32 or the encoded extents at addressing=48.
 * The box's content is a whole number of blocks, so the full index holds
 * one record per block and no end marker. The reserved extents after the
 * header's count are zero. Returns the number of heads.
 */
static size_t
box_heads(const memory_sink_t *sink, const erofs_inode_t *box,
    uint64_t *logical, uint64_t *block, bool *plain, size_t capacity)
{
	const unsigned char *body = sink->bytes + BOX_NID * SLOT +
	    (box->extended ? EROFS_INODE_EXTENDED_BYTES :
	    EROFS_INODE_COMPACT_BYTES);
	const uint64_t records = box->size / block_size;
	erofs_zrecord_t record;
	erofs_xrecord_t extent;
	const char *what = NULL;
	size_t heads = 0;
	uint32_t count;

	CHECK(box->size % block_size == 0);
	if (addressing == 48) {
		count = fsimg_load_le32(body);
		CHECK(count == box->layout_word && count <= records);
		CHECK(fsimg_load_le16(body + 4) == EROFS_EXTENT32_ADVICE);
		for (uint64_t i = 0; i < count; i++) {
			CHECK(erofs_xrecord_decode(body + EROFS_EXTENT32_OFFSET +
			    i * EROFS_EXTENT32_BYTES, EROFS_EXTENT32_BYTES,
			    &extent, &what) == FSIMG_OK);
			CHECK(heads < capacity);
			logical[heads] = extent.logical;
			block[heads] = extent.physical / block_size;
			plain[heads++] = extent.length_word >> 28 != EXTENT_LZ4;
		}
		for (uint64_t i = EROFS_EXTENT32_OFFSET + count *
		    EROFS_EXTENT32_BYTES; i < EROFS_EXTENT32_OFFSET + records *
		    EROFS_EXTENT32_BYTES; i++)
			CHECK_CASE(body[i] == 0, "reserved extent byte %ju",
			    (uintmax_t)i);
		return (heads);
	}
	for (size_t i = 0; i < EROFS_FULL_INDEX_OFFSET; i++)
		CHECK(body[i] == 0);
	for (uint64_t k = 0; k < records; k++) {
		CHECK(erofs_full_record_decode(body + EROFS_FULL_INDEX_OFFSET +
		    k * EROFS_FULL_RECORD_BYTES, false, &record, &what) ==
		    FSIMG_OK);
		if (record.type == EROFS_RECORD_NONHEAD)
			continue;
		CHECK(heads < capacity);
		logical[heads] = k * block_size + record.clusterofs;
		block[heads] = record.block;
		plain[heads++] = record.type == EROFS_RECORD_PLAIN;
	}
	CHECK(heads == box->layout_word);
	return (heads);
}

/*
 * The box's content, read from the image as an independent reader would:
 * the box inode's flat blocks, or its clusters decoded in order, each
 * running to the next head or the content's end; the clusters are the
 * box's blocks in order, after every inode's data.
 */
static unsigned char *
box_content(fixture_t *fixture, const memory_sink_t *sink,
    const erofs_inode_t *box)
{
	enum { HEADS_MAX = 4096 };
	static uint64_t logical[HEADS_MAX], block[HEADS_MAX];
	static bool plain[HEADS_MAX];
	const erofs_row_t *planned = &fixture->plan->box;
	unsigned char *content;
	const unsigned char *bytes;
	fsimg_codec_step_t step;
	uint64_t end;
	size_t heads, start;

	content = malloc((size_t)box->size + 1);
	CHECK(content != NULL);
	if (box->layout == EROFS_LAYOUT_FLAT) {
		CHECK(box->layout_word == planned->start_block);
		bcopy(sink->bytes + planned->start_block * block_size, content,
		    (size_t)box->size);
		return (content);
	}
	CHECK(box->layout == EROFS_LAYOUT_COMPRESSED_FULL);
	heads = box_heads(sink, box, logical, block, plain, HEADS_MAX);
	CHECK(heads == planned->clusters && heads > 0);
	for (size_t i = 0; i < heads; i++) {
		end = i + 1 < heads ? logical[i + 1] : box->size;
		CHECK((i != 0 || logical[0] == 0) && logical[i] < end &&
		    end - logical[i] <= LZ4_WINDOW);
		CHECK(block[i] == planned->start_block + i);
		bytes = sink->bytes + block[i] * block_size;
		if (plain[i]) {
			CHECK(end - logical[i] <= block_size);
			bcopy(bytes, content + logical[i],
			    (size_t)(end - logical[i]));
			continue;
		}
		for (start = 0; start < block_size && bytes[start] == 0; start++)
			continue;
		CHECK(fsimg_lz4_decode(bytes + start, block_size - start,
		    content + logical[i], (size_t)(end - logical[i]), &step,
		    NULL) == FSIMG_OK && step.produced == end - logical[i]);
	}
	return (content);
}

/*
 * Every directory entry of directory row dir names a child in the box with
 * bit 63, except those naming a 32-bit root in the primary area: "." and
 * ".." of the root and ".." of each of its subdirectories. Returns how
 * many entries name the root that way.
 */
static uint64_t
count_unboxed_entries(const memory_sink_t *sink, const erofs_row_t *dir,
    uint64_t root_nid)
{
	const unsigned char *block;
	uint64_t nid, unboxed = 0;
	uint16_t count;

	CHECK(dir->layout == EROFS_LAYOUT_FLAT);
	for (uint64_t b = 0; b * block_size < dir->size; b++) {
		block = sink->bytes + (dir->start_block + b) * block_size;
		count = fsimg_load_le16(block + 8) / 12;
		for (uint16_t e = 0; e < count; e++) {
			nid = fsimg_load_le64(block + e * 12u);
			if ((nid & EROFS_NID_BOX) != 0)
				continue;
			CHECK(addressing != 48 && nid == root_nid);
			unboxed++;
		}
	}
	return (unboxed);
}

/*
 * The box image's structure: METABOX with one extension slot naming the
 * box inode at NID 37, beside 48BIT under addressing=48, and LZ4_0PADDING
 * exactly when a cluster is written, the box's included. The root is the
 * box's first inode at addressing=48 and follows the box inode in the
 * primary area at addressing=32; every other inode is in the box. The box
 * inode is a regular file of the box's whole blocks, flat or compressed
 * as the setting says, and its content holds each row's inode at the
 * offset the row's NID names and zero everywhere else. At addressing=32
 * the entries naming the root without bit 63 are its own two and one in
 * each subdirectory, as many as its links.
 */
static void
check_box_structure(fixture_t *fixture, const memory_sink_t *sink)
{
	const erofs_plan_t *plan = fixture->plan;
	const unsigned char *raw = sink->bytes + EROFS_SUPER_OFFSET;
	const erofs_row_t root = row_at(fixture, 0);
	erofs_super_t sb;
	erofs_inode_t box, inode;
	unsigned char *content;
	const char *what = NULL;
	uint64_t previous = 0, data_end = 0, unboxed = 0;
	uint32_t want;

	sb = decode_image_super(sink);
	want = EROFS_INCOMPAT_METABOX |
	    (addressing == 48 ? EROFS_INCOMPAT_48BIT : 0) |
	    (plan->cluster_count != 0 ? EROFS_INCOMPAT_LZ4_0PADDING : 0);
	CHECK_CASE(sb.incompat == want, "incompat %#x, want %#x", sb.incompat,
	    want);
	CHECK(sb.extension_slots == 1 && sb.box_nid == BOX_NID);
	CHECK(fsimg_load_le64(raw + SB_BOX_NID) == BOX_NID);
	for (size_t i = SB_BOX_NID + 8; i < EROFS_SUPER_BASE_BYTES +
	    EROFS_SUPER_SLOT_BYTES; i++)
		CHECK(raw[i] == 0);
	CHECK(sb.inode_count == fixture->writer->count + 2);
	CHECK(sb.root_nid == root.nid);
	if (addressing == 48) {
		CHECK(root.nid == EROFS_NID_BOX);
		CHECK(fsimg_load_le64(raw + SB_WIDE_ROOT) == EROFS_NID_BOX);
	} else {
		CHECK(root.nid * SLOT >= BOX_NID * SLOT + plan->box_body &&
		    root.nid <= UINT16_MAX);
		CHECK(fsimg_load_le16(raw + SB_BLOCKS_HIGH) == root.nid);
	}

	CHECK(erofs_inode_decode(sink->bytes + BOX_NID * SLOT,
	    EROFS_INODE_EXTENDED_BYTES, &sb, &box, &what) == FSIMG_OK);
	CHECK(box.kind == FSIMG_ENTRY_FILE && box.size == plan->box.size &&
	    box.size % block_size == 0 && box.nlink == 1);
	CHECK(box.layout == (metabox == FSIMG_EROFS_METABOX_LZ4 &&
	    box.size != 0 ? EROFS_LAYOUT_COMPRESSED_FULL : EROFS_LAYOUT_FLAT));
	CHECK(BOX_NID * SLOT + plan->box_body <= plan->metadata_end);

	content = box_content(fixture, sink, &box);
	for (uint64_t id = 0; id <= fixture->writer->count; id++) {
		const erofs_row_t row = row_at(fixture, id);
		const uint64_t offset = (row.nid & ~EROFS_NID_BOX) * SLOT;
		const uint64_t blocks = erofs_row_data_blocks(&row, block_size);

		CHECK_CASE(((row.nid & EROFS_NID_BOX) != 0) == expect_in_box(id),
		    "id %ju: NID %#jx", (uintmax_t)id, (uintmax_t)row.nid);
		CHECK(row.layout != EROFS_LAYOUT_FLAT_INLINE);
		if (blocks != 0 && row.start_block + blocks > data_end)
			data_end = row.start_block + blocks;
		if ((row.mode & EROFS_MODE_TYPE_MASK) == EROFS_MODE_DIRECTORY)
			unboxed += count_unboxed_entries(sink, &row, root.nid);
		if (!expect_in_box(id))
			continue;
		CHECK_CASE(offset >= previous && offset +
		    erofs_row_body_bytes(plan, &row) <= box.size,
		    "id %ju lies outside the box's order", (uintmax_t)id);
		for (uint64_t k = previous; k < offset; k++)
			CHECK_CASE(content[k] == 0, "box byte %ju is %u",
			    (uintmax_t)k, content[k]);
		previous = offset + erofs_row_body_bytes(plan, &row);
		CHECK(erofs_inode_decode(content + offset,
		    erofs_row_fixed_bytes(&row), &sb, &inode, &what) ==
		    FSIMG_OK);
		CHECK_CASE(inode.size == row.size && inode.layout == row.layout &&
		    inode.extended == row.extended && inode.serial == id + 1,
		    "id %ju: box inode", (uintmax_t)id);
	}
	for (uint64_t k = previous; k < box.size; k++)
		CHECK_CASE(content[k] == 0, "box byte %ju is %u", (uintmax_t)k,
		    content[k]);
	CHECK(unboxed == (addressing == 48 ? 0 : root.links));
	CHECK(plan->box.start_block == 0 ? box.size == 0 :
	    plan->box.start_block >= data_end);
	CHECK(plan->box.start_block + erofs_row_data_blocks(&plan->box,
	    block_size) == plan->used_blocks || box.size == 0);
	free(content);
}

/* One box image of the tree build makes: plan, emit and check it whole. */
static void
check_box_round_trip(void (*build)(void))
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	build();
	reproducible_options(&options);
	plan_tree(&fixture, &options);
	emit_tree(&fixture, &sink, POISON);
	check_box_structure(&fixture, &sink);
	check_enumeration(&fixture, &sink, true);
	check_verification(&fixture, &sink);
	check_extraction(&fixture, &sink);
	free(sink.bytes);
	fixture_close(&fixture);
}

/*
 * Each box setting at each addressing, around flat files and around LZ4
 * files, reads back through fsimg's reader, verifier and extractor, and
 * has the box layout the design gives it.
 */
static void
test_metabox_images_read_back_as_specified(void)
{
	static const uint32_t widths[] = { 32, 48 };
	static const fsimg_erofs_metabox_t boxes[] = {
		FSIMG_EROFS_METABOX_PLAIN, FSIMG_EROFS_METABOX_LZ4
	};

	for (size_t w = 0; w < 2; w++) {
		for (size_t b = 0; b < 2; b++) {
			addressing = widths[w];
			metabox = boxes[b];
			check_box_round_trip(build_round_trip_tree);
			compression = FSIMG_EROFS_COMPRESSION_LZ4;
			check_box_round_trip(build_lz4_tree);
			compression = FSIMG_EROFS_COMPRESSION_NONE;
		}
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
}

/*
 * The root alone: at addressing=32 the box is empty, a flat inode of no
 * blocks under either setting, and the root follows the box inode; at
 * addressing=48 the root is the box's only inode.
 */
static void
test_metabox_with_the_root_alone(void)
{
	static const uint32_t widths[] = { 32, 48 };
	static const fsimg_erofs_metabox_t boxes[] = {
		FSIMG_EROFS_METABOX_PLAIN, FSIMG_EROFS_METABOX_LZ4
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	for (size_t w = 0; w < 2; w++) {
		for (size_t b = 0; b < 2; b++) {
			addressing = widths[w];
			metabox = boxes[b];
			tree_reset();
			reproducible_options(&options);
			plan_tree(&fixture, &options);
			CHECK((fixture.plan->box.size == 0) == (addressing == 32));
			emit_tree(&fixture, &sink, POISON);
			check_box_structure(&fixture, &sink);
			check_enumeration(&fixture, &sink, true);
			check_verification(&fixture, &sink);
			free(sink.bytes);
			fixture_close(&fixture);
		}
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
}

/*
 * A box of more blocks than one LZ4 window, at 512-byte blocks, so the
 * LZ4 box has several clusters and its full index or extents span the
 * box inode's body across blocks: 500 files in one directory, with times
 * that make most inodes extended.
 */
static void
test_metabox_of_several_clusters(void)
{
	static const uint32_t widths[] = { 32, 48 };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	block_size = 512;
	metabox = FSIMG_EROFS_METABOX_LZ4;
	for (size_t w = 0; w < 2; w++) {
		addressing = widths[w];
		tree_reset();
		add_directory("/many");
		for (unsigned i = 0; i < 500; i++)
			with_time(add_file(i % 7, "/many/f%u", i),
			    OLD_TIME + i * 7919, i % 3 == 0 ? 0 : i);
		unreproducible_options(&options);
		plan_tree(&fixture, &options);
		CHECK(fixture.plan->box.clusters >= 2);
		emit_tree(&fixture, &sink, POISON);
		check_box_structure(&fixture, &sink);
		check_enumeration(&fixture, &sink, false);
		check_verification(&fixture, &sink);
		free(sink.bytes);
		fixture_close(&fixture);
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
	block_size = BLOCK_DEFAULT;
}

/*
 * The box image is the same bytes whatever the insertion order, budget,
 * scratch or time zone, and leaves no stale byte in the box's blocks, its
 * inode or anywhere else, under each setting at each addressing; at small
 * blocks the box spans several blocks and, under LZ4, several clusters.
 */
static void
test_metabox_image_is_reproducible_and_clean(void)
{
	static const uint32_t widths[] = { 32, 48 };
	static const fsimg_erofs_metabox_t boxes[] = {
		FSIMG_EROFS_METABOX_PLAIN, FSIMG_EROFS_METABOX_LZ4
	};

	for (size_t w = 0; w < 2; w++) {
		for (size_t b = 0; b < 2; b++) {
			addressing = widths[w];
			metabox = boxes[b];
			check_identity_variants(build_round_trip_tree);
			check_no_stale_byte(build_round_trip_tree);
			compression = FSIMG_EROFS_COMPRESSION_LZ4;
			check_identity_variants(build_lz4_tree);
			check_no_stale_byte(build_lz4_tree);
			compression = FSIMG_EROFS_COMPRESSION_NONE;
		}
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
}

/*
 * The box's own cluster records come back from scratch storage like a
 * file's, so a damaged one is refused, not stored: a reserved byte set or
 * a logical start moved.
 */
static void
check_damaged_box_records(void)
{
	static const struct {
		const char	*name;
		size_t		 at;
		int		 value;		/* -1: add one to the byte */
	} cases[] = {
		{ "reserved byte", 13, 1 },
		{ "logical start", 0, -1 },
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	unsigned char byte;
	uint64_t offset;
	fsimg_status_t status;

	build_lz4_tree();
	reproducible_options(&options);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		plan_tree(&fixture, &options);
		offset = fixture.plan->box.first_cluster * 16 + cases[i].at;
		CHECK(fsimg_store_read(fixture.plan->cluster_records, offset,
		    &byte, 1, NULL) == FSIMG_OK);
		byte = cases[i].value < 0 ? (unsigned char)(byte + 1) :
		    (unsigned char)cases[i].value;
		CHECK(fsimg_store_write(fixture.plan->cluster_records, offset,
		    &byte, 1, NULL) == FSIMG_OK);
		sink_open(&fixture, &sink, POISON);
		status = emit_into(&fixture, &sink);
		CHECK_CASE(status == FSIMG_ESTATE &&
		    strcmp(fixture.error.message,
		    "invalid EROFS cluster plan") == 0, "%s: status %d: %s",
		    cases[i].name, status, fixture.error.message);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/*
 * Rows come back from scratch storage, so a box row damaged after planning
 * is refused before anything of it is written: one given an inline tail,
 * which no inode has under a box and which would land at a volume offset
 * taken from its box offset; one whose NID lost bit 63; one whose offset
 * lies past the box's content; and one whose data lies in the box's
 * blocks.
 */
static void
test_metabox_damaged_rows_are_refused(void)
{
	static const char *const cases[] = {
		"inline tail", "NID without bit 63", "offset past the box",
		"data in the box's blocks"
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	erofs_row_t row;
	uint64_t id;
	fsimg_status_t status;

	metabox = FSIMG_EROFS_METABOX_PLAIN;
	for (size_t w = 0; w < 2; w++) {
		addressing = w == 0 ? 32 : 48;
		build_round_trip_tree();
		reproducible_options(&options);
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			plan_tree(&fixture, &options);
			id = id_of_path(&fixture, "/small/one");
			row = row_at(&fixture, id);
			CHECK(row.layout == EROFS_LAYOUT_FLAT && row.size == 1);
			if (i == 0) {
				row.layout = EROFS_LAYOUT_FLAT_INLINE;
				row.start_block = 0;
			} else if (i == 1)
				row.nid &= ~EROFS_NID_BOX;
			else if (i == 2)
				row.nid = EROFS_NID_BOX |
				    fixture.plan->box.size / SLOT;
			else
				row.start_block = fixture.plan->data_end;
			CHECK(erofs_row_set(fixture.plan, id, &row, NULL) ==
			    FSIMG_OK);
			sink_open(&fixture, &sink, POISON);
			status = emit_into(&fixture, &sink);
			CHECK_CASE(status == (i == 0 ? FSIMG_ECORRUPT :
			    FSIMG_ESTATE), "%s at %u: status %d: %s",
			    cases[i], addressing, status, fixture.error.message);
			free(sink.bytes);
			fixture_close(&fixture);
		}
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
}

/*
 * Source, allocation, write and cluster-plan faults fail the box image
 * cleanly at each addressing; the LZ4 runs also measure the box during
 * planning, so an allocation failure there is covered.
 */
static void
test_metabox_faults_are_clean(void)
{
	static const uint32_t widths[] = { 32, 48 };

	for (size_t w = 0; w < 2; w++) {
		addressing = widths[w];
		metabox = FSIMG_EROFS_METABOX_PLAIN;
		test_write_failure_fails_emission_at_any_write();
		test_allocation_failure_during_emission_is_clean();
		metabox = FSIMG_EROFS_METABOX_LZ4;
		test_write_failure_fails_emission_at_any_write();
		test_allocation_failure_during_emission_is_clean();
		test_changed_file_source_fails_emission();
		test_lz4_source_faults_fail_planning_and_emission();
		test_lz4_allocation_and_write_failures_are_clean();
		test_lz4_damaged_cluster_plan_is_refused();
		compression = FSIMG_EROFS_COMPRESSION_LZ4;
		check_damaged_box_records();
		compression = FSIMG_EROFS_COMPRESSION_NONE;
	}
	metabox = FSIMG_EROFS_METABOX_NONE;
	addressing = 0;
}

/*
 * The superblock with a metadata box encodes its one extension slot, the
 * box NID at 0x80, and decodes to the same values; a box NID of zero or
 * with bit 63, a second slot, or a box NID without METABOX is refused.
 */
static void
test_box_super_encoding_decodes_to_the_same_superblock(void)
{
	unsigned char region[SUPER_REGION];
	erofs_super_t sb = sample_super(), decoded;
	erofs_config_t config;
	const char *what = NULL;

	sb.incompat = EROFS_INCOMPAT_METABOX | EROFS_INCOMPAT_48BIT;
	sb.extension_slots = 1;
	sb.box_nid = BOX_NID;
	sb.root_nid = EROFS_NID_BOX;
	memset(region, 0xff, sizeof(region));
	bzero(region + EROFS_SUPER_BASE_BYTES + EROFS_SUPER_SLOT_BYTES,
	    sizeof(region) - EROFS_SUPER_BASE_BYTES - EROFS_SUPER_SLOT_BYTES);
	CHECK(erofs_super_encode(&sb, region));
	CHECK(erofs_super_checksum(region, sizeof(region), sb.blkszbits,
	    &sb.checksum));
	CHECK(erofs_super_encode(&sb, region));
	CHECK(erofs_region_decode(region, sizeof(region), &decoded, &config,
	    &what) == FSIMG_OK);
	CHECK(decoded.incompat == sb.incompat &&
	    decoded.extension_slots == 1 && decoded.box_nid == BOX_NID &&
	    decoded.root_nid == EROFS_NID_BOX && !decoded.reserved_nonzero);
	for (size_t i = SB_BOX_NID + 8; i < EROFS_SUPER_BASE_BYTES +
	    EROFS_SUPER_SLOT_BYTES; i++)
		CHECK(region[i] == 0);

	sb.incompat = EROFS_INCOMPAT_METABOX;
	sb.root_nid = 40;
	CHECK(erofs_super_encode(&sb, region));
	sb.box_nid = 0;
	CHECK(!erofs_super_encode(&sb, region));
	sb.box_nid = BOX_NID | EROFS_NID_BOX;
	CHECK(!erofs_super_encode(&sb, region));
	sb.box_nid = BOX_NID;
	sb.extension_slots = 2;
	CHECK(!erofs_super_encode(&sb, region));
	sb.extension_slots = 1;
	sb.incompat = 0;
	CHECK(!erofs_super_encode(&sb, region));
}

/*
 * Run a test at each block size the writer accepts, then return to the
 * default. The model, the sweeps and the image checks follow block_size.
 * A failed check exits, and report_block_size() then names the size.
 */
static void
at_every_block_size(void (*test)(void))
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };

	sweeping = true;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		block_size = sizes[i];
		test();
	}
	sweeping = false;
	block_size = BLOCK_DEFAULT;
}

/* At exit, name the block size of the test that failed during a sweep. */
static void
report_block_size(void)
{
	if (sweeping)
		fprintf(stderr, "the failed check ran at %u-byte blocks\n",
		    block_size);
}

int
main(void)
{
	CHECK(atexit(report_block_size) == 0);
	test_default_settings_plan_an_empty_volume();
	test_unwritten_settings_are_refused_naming_the_setting();
	test_block_size_selects_512_to_4096();
	at_every_block_size(test_sector_volume_and_label_geometry);
	test_other_formats_refuse_erofs_settings();
	test_default_label_is_selected();

	test_volume_uuid_is_required_outside_reproducible_mode();
	test_given_uuid_is_stored_verbatim();
	test_reproducible_mode_derives_uuid_and_time_from_epoch();
	test_unreproducible_format_time_is_the_host_clock();

	at_every_block_size(test_empty_catalog_has_only_the_root_inode);
	at_every_block_size(test_mixed_tree_plan_matches_the_model);
	at_every_block_size(test_start_blocks_follow_catalog_order_contiguously);
	at_every_block_size(test_inodes_are_aligned_in_catalog_order);
	test_link_count_is_two_plus_subdirectories();
	test_default_modes_and_given_modes();
	at_every_block_size(test_tail_layout_follows_the_block_fit);
	at_every_block_size(test_inline_tail_never_crosses_a_block);
	at_every_block_size(test_extended_inode_moves_when_its_fixed_part_would_cross);
	at_every_block_size(test_compact_inode_may_use_a_blocks_last_slot);
	test_extended_form_triggers();
	test_modification_time_is_compared_to_the_nanosecond();
	test_reproducible_mode_stores_the_epoch_in_every_inode();
	at_every_block_size(test_volume_size_sets_the_block_count);
	test_names_are_bounded_in_bytes();
	test_insertion_order_does_not_change_the_plan();

	test_directory_blocks_fill_exactly_and_overflow_by_one_byte();
	test_directory_blocks_hold_255_byte_names();
	test_directory_blocks_mix_name_lengths();
	at_every_block_size(test_directory_walk_merges_dots_across_blocks);

	at_every_block_size(test_empty_catalog_image_reads_back_as_the_root_alone);
	at_every_block_size(test_reproducible_image_reads_back_as_specified);
	at_every_block_size(test_unreproducible_image_stores_given_times_and_owners);
	at_every_block_size(test_reproducible_image_ignores_order_limits_and_time_zone);
	at_every_block_size(test_emission_leaves_no_stale_byte);
	at_every_block_size(test_sector_multiple_volume_has_a_zero_partial_tail);
	test_write_failure_fails_emission_at_any_write();
	test_flush_failure_fails_emission();
	test_allocation_failure_during_emission_is_clean();
	test_changed_file_source_fails_emission();
	test_changed_link_source_fails_emission();

	at_every_block_size(test_lz4_image_reads_back_as_specified);
	at_every_block_size(test_lz4_image_ignores_order_limits_and_time_zone);
	at_every_block_size(test_lz4_emission_leaves_no_stale_byte);
	at_every_block_size(test_lz4_without_a_compressible_file_is_the_flat_image);
	test_lz4_source_faults_fail_planning_and_emission();
	test_lz4_allocation_and_write_failures_are_clean();
	test_lz4_damaged_cluster_plan_is_refused();
	at_every_block_size(test_lz4_output_limit_below_the_least_image_refuses_unread);
	test_index_encoding_decodes_to_the_same_records();

	test_super_encoding_decodes_to_the_same_superblock();
	test_super_encoding_refuses_what_it_cannot_store();
	test_compact_inode_encoding_decodes_to_the_same_inode();
	test_extended_inode_encoding_decodes_to_the_same_inode();
	test_compact_inode_encoding_refuses_what_it_cannot_store();

	at_every_block_size(test_addressing48_image_structure);
	test_addressing48_compact_time_range();
	at_every_block_size(test_addressing48_lz4_files_are_encoded_extents);
	test_addressing48_extents_cross_the_staging_buffer();
	at_every_block_size(test_addressing48_image_is_reproducible_and_clean);
	at_every_block_size(test_addressing48_volume_size_passes_32_bits);
	at_every_block_size(test_addressing48_start_blocks_pass_32_bits);
	test_addressing48_faults_are_clean();

	test_box_super_encoding_decodes_to_the_same_superblock();
	at_every_block_size(test_metabox_images_read_back_as_specified);
	at_every_block_size(test_metabox_with_the_root_alone);
	test_metabox_of_several_clusters();
	at_every_block_size(test_metabox_image_is_reproducible_and_clean);
	test_metabox_damaged_rows_are_refused();
	test_metabox_faults_are_clean();
	test_wide_super_encoding_decodes_to_the_same_superblock();
	test_implicit_link_count_encoding();
	test_extent_encoding_decodes_to_the_same_records();
	return (0);
}
