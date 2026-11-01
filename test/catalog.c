/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "source.h"
#include "test.h"
#include "entry-log.h"

typedef struct catalog_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	char		 directory[64];
} catalog_fixture_t;

static void fixture_open(catalog_fixture_t *, uint32_t);
static void fixture_open_budget(catalog_fixture_t *, uint32_t, uint64_t,
    uint64_t);
static void fixture_open_observed(catalog_fixture_t *, entry_log_t *);
static void fixture_open_options(catalog_fixture_t *, uint32_t, uint64_t,
    uint64_t, entry_log_t *);
static void test_planning_reports_merged_catalog_once(void);
static void test_failed_planning_preserves_entry_callbacks(void);
static void test_planning_without_entry_callback(void);
static void fixture_close(catalog_fixture_t *);
static void add_entry(fsimg_writer_t *, const char *, fsimg_entry_kind_t);
static void add_dated_directory(fsimg_writer_t *, const char *, int64_t);
static void test_order(uint32_t, uint32_t);
static void test_long_directory(void);
static void test_invalid_catalogs(void);
static void test_metadata(void);
static void test_posix_metadata(void);
static void test_entry_kinds(void);
static void test_invalid_kinds(void);
static void test_failures(void);
static void test_corrupt_metadata(void);
static void test_inspection_failure(void);
static void test_scale(uint64_t, bool);
static void test_deep_catalog(void);
static void test_short_child_record(void);
static void test_child_order(void);
static void test_index_reference(void);
static void test_merged_directories(void);
static void test_path_collisions(void);
static void test_merged_metadata(void);
static void test_corrupt_rank(void);
static void use_provider(catalog_fixture_t *);
static fsimg_status_t generated_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t generated_read(void *, uint64_t, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);

static void
fixture_open(catalog_fixture_t *fixture, uint32_t pages)
{
	fixture_open_budget(fixture, pages, UINT64_C(16) * 1024 * 1024, 0);
}

static void
fixture_open_budget(catalog_fixture_t *fixture, uint32_t pages, uint64_t budget,
    uint64_t entries)
{
	fixture_open_options(fixture, pages, budget, entries, NULL);
}

static void
fixture_open_options(catalog_fixture_t *fixture, uint32_t pages, uint64_t budget,
    uint64_t entries, entry_log_t *log)
{
	fsimg_options_t options;
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory), "catalog-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fd = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = budget;
	options.entry = log == NULL ? NULL : record_entry;
	options.entry_argument = log;
	options.max_path_bytes = 512;
	options.max_open_files = 16;
	if (entries != 0)
		options.max_entries = entries;
	options.scratch_directory_fd = fd;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_collect_new(fixture->ctx, &fixture->writer, NULL) == FSIMG_OK);
}

static void
fixture_close(catalog_fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	CHECK(stats.scratch_bytes == 0 && stats.scratch_physical_bytes == 0);
	CHECK(stats.memory_peak <= fixture->ctx->options.memory_bytes);
	fsimg_ctx_free(fixture->ctx);
	CHECK(rmdir(fixture->directory) == 0);
}

/* Files read as zeros; a symbolic link takes its six-byte target from the
 * generated provider, which the caller registers first with use_provider.
 * Directories and FIFOs have no source.
 */
static void
add_entry(fsimg_writer_t *writer, const char *path, fsimg_entry_kind_t kind)
{
	fsimg_entry_t entry = { .path = path, .kind = kind };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_source_t target = { .kind = FSIMG_SOURCE_PROVIDER };
	const fsimg_source_t *source;

	source = NULL;
	if (kind == FSIMG_ENTRY_FILE)
		source = &zero;
	if (kind == FSIMG_ENTRY_SYMLINK) {
		entry.size = 6;
		source = &target;
	}
	CHECK(fsimg_writer_add(writer, &entry, source, NULL) == FSIMG_OK);
}

/* A programmatic directory whose only metadata is its modification time. */
static void
add_dated_directory(fsimg_writer_t *writer, const char *path, int64_t seconds)
{
	fsimg_entry_t entry = { .path = path, .kind = FSIMG_ENTRY_DIRECTORY };

	entry.metadata.valid = FSIMG_META_MODIFIED;
	entry.metadata.modified = (fsimg_timestamp_t){ .seconds = seconds,
	    .precision_ns = 1, .basis = FSIMG_TIME_UTC };
	CHECK(fsimg_writer_add(writer, &entry, NULL, NULL) == FSIMG_OK);
}

static void
test_order(uint32_t pages, uint32_t fan_in)
{
	const char *paths[] = { "/a/b/d", "/a-", "/z", "/a/b", "/a", "/a/c" };
	const fsimg_entry_kind_t kinds[] = { FSIMG_ENTRY_FILE, FSIMG_ENTRY_FILE,
	    FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_DIRECTORY,
	    FSIMG_ENTRY_FILE };
	const char *ordered[] = { "/", "/a", "/a/b", "/a/b/d", "/a/c", "/a-", "/z" };
	const uint64_t parents[] = { 0, 0, 1, 2, 1, 0, 0 };
	const uint64_t children[] = { 3, 2, 1, 0, 0, 0, 0 };
	catalog_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 2048, .fan_in = fan_in };
	const fsimg_catalog_entry_t *entry;
	char borrowed[64];

	fixture_open(&fixture, pages);
	for (size_t i = 0; i < 6; i++) {
		size_t source = fan_in == 2 ? (i + pages) % 6 : (5 - i + pages) % 6;

		snprintf(borrowed, sizeof(borrowed), "%s", paths[source]);
		add_entry(fixture.writer, borrowed, kinds[source]);
		memset(borrowed, 'x', strlen(borrowed));
	}
	CHECK(fsimg_writer_prepare(fixture.writer, &options, NULL) == FSIMG_OK);
	CHECK(fixture.writer->count == 6 && fixture.writer->catalog_ready);
	for (uint64_t id = 0; id <= 6; id++) {
		CHECK(fsimg_catalog_get(fixture.writer, id, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->entry.path, ordered[id]) == 0);
		CHECK(entry->id == id && entry->parent_id == parents[id]);
		CHECK(entry->children == children[id]);
	}
	CHECK(fsimg_writer_add(fixture.writer, NULL, NULL, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_catalog_get(fixture.writer, 7, &entry, NULL) == FSIMG_ERANGE);
	CHECK(entry == NULL);
	fixture_close(&fixture);
}

static void
test_deep_catalog(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char path[256];

	fixture_open(&fixture, 2);
	for (unsigned i = 40; i != 0; i--) {
		for (unsigned j = 0; j < i; j++) {
			path[2 * j] = '/';
			path[2 * j + 1] = 'd';
		}
		path[2 * i] = '\0';
		add_entry(fixture.writer, path, FSIMG_ENTRY_DIRECTORY);
	}
	add_entry(fixture.writer, "/d-/f", FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, "/d-", FSIMG_ENTRY_DIRECTORY);
	add_entry(fixture.writer, "/z", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	for (uint64_t id = 1; id <= 40; id++) {
		CHECK(fsimg_catalog_get(fixture.writer, id, &entry, NULL) == FSIMG_OK);
		CHECK(entry->parent_id == id - 1);
		CHECK(entry->children == (id == 40 ? 0u : 1u));
	}
	CHECK(fsimg_catalog_get(fixture.writer, 41, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, "/d-") == 0 && entry->parent_id == 0);
	CHECK(fsimg_catalog_get(fixture.writer, 42, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, "/d-/f") == 0 && entry->parent_id == 41);
	CHECK(fsimg_catalog_get(fixture.writer, 43, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, "/z") == 0 && entry->parent_id == 0);
	fixture_close(&fixture);
}

static void
test_long_directory(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char directory[202], child[208];

	fixture_open(&fixture, 2);
	directory[0] = '/';
	memset(directory + 1, 'd', 200);
	directory[201] = '\0';
	snprintf(child, sizeof(child), "%s/file", directory);
	add_entry(fixture.writer, child, FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, directory, FSIMG_ENTRY_DIRECTORY);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, directory) == 0);
	CHECK(fsimg_catalog_get(fixture.writer, 2, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, child) == 0 && entry->parent_id == 1);
	fixture_close(&fixture);
}

static void
test_invalid_catalogs(void)
{
	catalog_fixture_t fixture;
	fsimg_entry_t entry = { .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_error_t error;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		fixture_open(&fixture, 2);
		if (scenario == 0) {
			add_entry(fixture.writer, "/missing/file", FSIMG_ENTRY_FILE);
		} else if (scenario == 1) {
			add_entry(fixture.writer, "/a", FSIMG_ENTRY_FILE);
			add_entry(fixture.writer, "/a/b", FSIMG_ENTRY_FILE);
		} else {
			add_entry(fixture.writer, "/a", scenario == 2 ?
			    FSIMG_ENTRY_FILE : FSIMG_ENTRY_DIRECTORY);
			add_entry(fixture.writer, "/a", FSIMG_ENTRY_FILE);
		}
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) == FSIMG_EINVAL);
		if (scenario >= 2)
			CHECK(strcmp(error.path, "/a") == 0 && !error.path_truncated);
		CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
		CHECK(!fixture.writer->catalog_ready && fixture.writer->index == NULL);
		CHECK(fsimg_writer_add(fixture.writer, NULL, NULL, NULL) == FSIMG_ESTATE);
		fixture_close(&fixture);
	}
	fixture_open(&fixture, 2);
	fixture.ctx->options.max_entries = 1;
	add_entry(fixture.writer, "/a", FSIMG_ENTRY_FILE);
	entry.path = "/b";
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_ENTRIES);
	CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
	fixture_close(&fixture);
}

static void
test_metadata(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *current;
	fsimg_entry_t entry = { .path = "/file", .kind = FSIMG_ENTRY_FILE,
	    .size = UINT64_C(1) << 45 };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	unsigned char buffer[17];
	size_t transferred;

	fixture_open(&fixture, 2);
	/* Garbage in absent fields must neither influence order nor be emitted. */
	memset(&entry.metadata, 0x97, sizeof(entry.metadata));
	entry.metadata.valid = FSIMG_META_MODIFIED;
	entry.metadata.required = FSIMG_META_MODIFIED;
	entry.metadata.modified = (fsimg_timestamp_t){ .seconds = INT64_MIN,
	    .precision_ns = 100, .nanoseconds = 123400, .basis = FSIMG_TIME_OFFSET,
	    .offset_minutes = -60 };
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_OK);
	CHECK(current->entry.metadata.modified.seconds == INT64_MIN);
	CHECK(current->entry.metadata.modified.offset_minutes == -60);
	CHECK(current->entry.metadata.attributes == 0);
	CHECK(current->entry.metadata.created.precision_ns == 0);
	CHECK(fsimg_source_open(fixture.writer, current->id, NULL) == FSIMG_OK);
	memset(buffer, 0x42, sizeof(buffer));
	CHECK(fsimg_source_read(fixture.writer, entry.size - sizeof(buffer), buffer,
	    sizeof(buffer), &transferred, NULL) == FSIMG_OK);
	CHECK(transferred == sizeof(buffer));
	for (size_t i = 0; i < sizeof(buffer); i++)
		CHECK(buffer[i] == 0);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	fixture_close(&fixture);
}

/* The four POSIX additions survive the catalog exactly, garbage in absent
 * fields is not carried, and each new kind keeps its declared size and source.
 */
static void
test_posix_metadata(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *current;
	fsimg_entry_t directory = { .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	fsimg_entry_t link = { .path = "/dir/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 1024 };
	fsimg_entry_t fifo = { .path = "/dir/pipe", .kind = FSIMG_ENTRY_FIFO };
	fsimg_source_t target = { .kind = FSIMG_SOURCE_PROVIDER, .id = 9 };

	fixture_open(&fixture, 2);
	use_provider(&fixture);
	directory.metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODE |
	    FSIMG_META_CHANGED;
	directory.metadata.required = FSIMG_META_MODE;
	directory.metadata.uid = UINT32_MAX;
	directory.metadata.gid = 7;
	directory.metadata.mode = 07777;
	directory.metadata.changed = (fsimg_timestamp_t){ .seconds = -5,
	    .precision_ns = 1, .nanoseconds = 999999999, .basis = FSIMG_TIME_OFFSET,
	    .offset_minutes = 1439 };
	memset(&link.metadata, 0x97, sizeof(link.metadata));
	link.metadata.valid = FSIMG_META_MODE;
	link.metadata.required = 0;
	link.metadata.mode = 0777;
	CHECK(fsimg_writer_add(fixture.writer, &directory, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(fixture.writer, &link, &target, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(fixture.writer, &fifo, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);

	CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_OK);
	CHECK(current->entry.kind == FSIMG_ENTRY_DIRECTORY && current->children == 2);
	CHECK(current->entry.metadata.valid == directory.metadata.valid);
	CHECK(current->entry.metadata.required == FSIMG_META_MODE);
	CHECK(current->entry.metadata.uid == UINT32_MAX);
	CHECK(current->entry.metadata.gid == 7);
	CHECK(current->entry.metadata.mode == 07777);
	CHECK(current->entry.metadata.changed.seconds == -5);
	CHECK(current->entry.metadata.changed.nanoseconds == 999999999);
	CHECK(current->entry.metadata.changed.offset_minutes == 1439);

	CHECK(fsimg_catalog_get(fixture.writer, 2, &current, NULL) == FSIMG_OK);
	CHECK(strcmp(current->entry.path, "/dir/link") == 0);
	CHECK(current->entry.kind == FSIMG_ENTRY_SYMLINK);
	CHECK(current->entry.size == 1024 &&
	    current->storage == FSIMG_STORAGE_PROVIDER && current->source_id == 9);
	CHECK(current->entry.metadata.mode == 0777);
	CHECK(current->entry.metadata.uid == 0 && current->entry.metadata.gid == 0);
	CHECK(current->entry.metadata.changed.precision_ns == 0);
	CHECK(current->entry.metadata.attributes == 0);

	CHECK(fsimg_catalog_get(fixture.writer, 3, &current, NULL) == FSIMG_OK);
	CHECK(current->entry.kind == FSIMG_ENTRY_FIFO);
	CHECK(current->entry.size == 0 && current->storage == FSIMG_STORAGE_NONE);
	CHECK(current->children == 0);
	fixture_close(&fixture);
}

/* A symbolic link is content: like a file it needs a source and cannot be a
 * parent. A FIFO is like a directory without children: no size, no source.
 */
static void
test_entry_kinds(void)
{
	catalog_fixture_t fixture;
	fsimg_entry_t link = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 6 };
	fsimg_entry_t child = { .path = "/link/child", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_source_t target = { .kind = FSIMG_SOURCE_PROVIDER };

	fixture_open(&fixture, 2);
	use_provider(&fixture);
	CHECK(fsimg_writer_add(fixture.writer, &link, &target, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(fixture.writer, &child, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_EINVAL);
	fixture_close(&fixture);
}

static void
test_invalid_kinds(void)
{
	static const struct {
		fsimg_entry_kind_t kind;
		uint64_t size;
		fsimg_source_kind_t source;
		uint32_t valid;
		uint32_t mode;
	} cases[] = {
		{ .kind = FSIMG_ENTRY_SYMLINK, .size = 6 },
		{ .kind = FSIMG_ENTRY_SYMLINK, .size = 6, .source = FSIMG_SOURCE_ZERO },
		{ .kind = FSIMG_ENTRY_SYMLINK, .size = 0, .source = FSIMG_SOURCE_PROVIDER },
		{ .kind = FSIMG_ENTRY_SYMLINK, .size = 1025, .source = FSIMG_SOURCE_PROVIDER },
		{ .kind = FSIMG_ENTRY_FIFO, .source = FSIMG_SOURCE_ZERO },
		{ .kind = FSIMG_ENTRY_FIFO, .size = 1 },
		{ .kind = FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY },
		{ .kind = (fsimg_entry_kind_t)16 },
		{ .kind = (fsimg_entry_kind_t)0 },
		{ .kind = FSIMG_ENTRY_FIFO, .valid = FSIMG_META_MODE, .mode = 010000 },
		{ .kind = FSIMG_ENTRY_FIFO, .valid = 0x80 }
	};
	catalog_fixture_t fixture;
	fsimg_entry_t entry;
	fsimg_source_t source;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture, 2);
		use_provider(&fixture);
		entry = (fsimg_entry_t){ .path = "/entry", .kind = cases[i].kind,
		    .size = cases[i].size };
		entry.metadata.valid = cases[i].valid;
		entry.metadata.mode = cases[i].mode;
		source = (fsimg_source_t){ .kind = cases[i].source };
		CHECK(fsimg_writer_add(fixture.writer, &entry,
		    cases[i].source != 0 ? &source : NULL, &error) == FSIMG_EINVAL);
		CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
		fixture_close(&fixture);
	}
}

static void
test_failures(void)
{
	catalog_fixture_t fixture;
	fsimg_writer_t *writer;
	fsimg_sort_options_t options = { .run_bytes = 2048, .fan_in = 2 };
	fsimg_status_t status;
	char path[64];
	bool passed;

	for (uint64_t attempt = 1; attempt < 12; attempt++) {
		fixture_open(&fixture, 2);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_writer_collect_new(fixture.ctx, &writer, NULL);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK)
			fsimg_writer_free(writer);
		else
			CHECK(status == FSIMG_ENOMEM && writer == NULL);
		fixture_close(&fixture);
		if (status == FSIMG_OK)
			break;
	}
	passed = false;
	for (uint64_t attempt = 1; attempt <= 128; attempt++) {
		fixture_open(&fixture, 2);
		for (unsigned i = 0; i < 100; i++) {
			snprintf(path, sizeof(path), "/f%03u", 99 - i);
			add_entry(fixture.writer, path, FSIMG_ENTRY_FILE);
		}
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_writer_prepare(fixture.writer, &options, NULL);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK)
			passed = true;
		else {
			CHECK(status == FSIMG_ENOMEM);
			CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
			CHECK(fixture.writer->index == NULL && fixture.writer->ordered == NULL);
		}
		fixture_close(&fixture);
		if (passed)
			break;
	}
	CHECK(passed);
}

static void
test_corrupt_metadata(void)
{
	catalog_fixture_t fixture;
	fsimg_entry_t entry = { .path = "/file", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_catalog_entry_t *current;
	unsigned char bytes[8];

	fixture_open(&fixture, 2);
	entry.metadata.valid = FSIMG_META_MODIFIED;
	entry.metadata.modified = (fsimg_timestamp_t){ .seconds = 123,
	    .precision_ns = 1, .nanoseconds = 5, .basis = FSIMG_TIME_UTC };
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	/* An out-of-range encoded nanosecond value is rejected, not narrowed. */
	fsimg_store_le64(bytes, 1000000005);
	CHECK(fsimg_store_write(fixture.writer->ordered->store,
	    8 + FSIMG_CATALOG_MODIFIED + 16, bytes, 4, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_ECORRUPT);
	CHECK(current == NULL);
	fixture_close(&fixture);

	/* An encoded kind outside the closed set is corruption, not a new kind. */
	fixture_open(&fixture, 2);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	fsimg_store_le64(bytes, FSIMG_ENTRY_FILE | FSIMG_ENTRY_SYMLINK);
	CHECK(fsimg_store_write(fixture.writer->ordered->store,
	    8 + FSIMG_CATALOG_KIND, bytes, 4, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);

	/* An encoded mode with file-type bits is corruption. */
	fixture_open(&fixture, 2);
	entry.metadata.valid |= FSIMG_META_MODE;
	entry.metadata.mode = 0644;
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	fsimg_store_le64(bytes, 0100644);
	CHECK(fsimg_store_write(fixture.writer->ordered->store,
	    8 + FSIMG_CATALOG_MODE, bytes, 4, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);

	/* An absent field is written as zero bytes; anything else there is
	 * corruption, for scalar fields and timestamps alike.
	 */
	static const unsigned absent[] = { FSIMG_CATALOG_ATTRIBUTES,
	    FSIMG_CATALOG_UID, FSIMG_CATALOG_GID, FSIMG_CATALOG_MODE,
	    FSIMG_CATALOG_CREATED,
	    FSIMG_CATALOG_ACCESSED + FSIMG_CATALOG_TIME_BYTES - 1,
	    FSIMG_CATALOG_CHANGED };
	entry.metadata.valid = FSIMG_META_MODIFIED;
	for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
		fixture_open(&fixture, 2);
		CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) == FSIMG_OK);
		bytes[0] = 1;
		CHECK(fsimg_store_write(fixture.writer->ordered->store,
		    8 + absent[i], bytes, 1, NULL) == FSIMG_OK);
		CHECK(fsimg_catalog_get(fixture.writer, 1, &current, NULL) ==
		    FSIMG_ECORRUPT);
		fixture_close(&fixture);
	}
}

static void
test_inspection_failure(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;

	fixture_open(&fixture, 2);
	add_entry(fixture.writer, "/file", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	fixture.ctx->options.max_work = fixture.writer->session.progress.work;
	CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_ELIMIT);
	CHECK(entry == NULL && fixture.writer->state == FSIMG_WRITER_FAILED);
	CHECK(fixture.writer->catalog_ready);
	fixture.ctx->options.max_work = 0;
	CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, "/file") == 0);
	fixture_close(&fixture);
}

static void
test_short_child_record(void)
{
	catalog_fixture_t fixture;
	unsigned char payload[12] = { 0 };

	fixture_open(&fixture, 2);
	fixture.writer->count = 1;
	fixture.writer->state = FSIMG_WRITER_PREPARING;
	CHECK(fsimg_array_new(&fixture.writer->session, FSIMG_CATALOG_INDEX_SIZE,
	    2, &fixture.writer->index, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_new(&fixture.writer->session, 512,
	    &fixture.writer->children, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_append(fixture.writer->children, payload,
	    sizeof(payload), NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_seal(fixture.writer->children, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_count_children(fixture.writer, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

static void
test_child_order(void)
{
	catalog_fixture_t fixture;
	unsigned char payload[18] = { 0 };

	for (unsigned scenario = 0; scenario < 2; scenario++) {
		fixture_open(&fixture, 2);
		fixture.writer->count = 4;
		fixture.writer->state = FSIMG_WRITER_PREPARING;
		CHECK(fsimg_array_new(&fixture.writer->session, FSIMG_CATALOG_INDEX_SIZE,
		    5, &fixture.writer->index, NULL) == FSIMG_OK);
		CHECK(fsimg_stream_new(&fixture.writer->session, 512,
		    &fixture.writer->children, NULL) == FSIMG_OK);
		fsimg_store_le64(payload, scenario == 0 ? 0 : 2);
		fsimg_store_le64(payload + 8, scenario == 0 ? 2 : 3);
		payload[16] = scenario == 0 ? 'b' : 'a';
		CHECK(fsimg_stream_append(fixture.writer->children, payload,
		    sizeof(payload), NULL, NULL) == FSIMG_OK);
		fsimg_store_le64(payload, 0);
		fsimg_store_le64(payload + 8, 1);
		payload[16] = 'a';
		CHECK(fsimg_stream_append(fixture.writer->children, payload,
		    sizeof(payload), NULL, NULL) == FSIMG_OK);
		for (uint64_t i = 0; i < 2; i++) {
			fsimg_store_le64(payload, scenario == 0 ? 0 : (i == 0 ? 0 : 2));
			fsimg_store_le64(payload + 8, scenario == 0 ? 3 + i : (i == 0 ? 2 : 4));
			payload[16] = scenario == 0 ? (unsigned char)('c' + i) : 'b';
			CHECK(fsimg_stream_append(fixture.writer->children, payload,
			    sizeof(payload), NULL, NULL) == FSIMG_OK);
		}
		CHECK(fsimg_stream_seal(fixture.writer->children, NULL) == FSIMG_OK);
		CHECK(fsimg_catalog_count_children(fixture.writer, NULL) == FSIMG_ECORRUPT);
		fixture_close(&fixture);
	}
}

static void
test_index_reference(void)
{
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	unsigned char index[FSIMG_CATALOG_INDEX_SIZE];

	fixture_open(&fixture, 2);
	add_entry(fixture.writer, "/file", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_array_get(fixture.writer->index, 1, index, NULL) == FSIMG_OK);
	fsimg_store_le64(index + 8, 1);
	CHECK(fsimg_array_set(fixture.writer->index, 1, index, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_ECORRUPT);
	CHECK(entry == NULL);
	fixture_close(&fixture);
}

/* Programmatic records of one directory merge into one entry whose
 * children are the union of the records' children when they encode equally,
 * as the parents the CLI synthesizes for several operands do. The merged
 * catalog is the same wherever the repeated records sit among the other
 * entries and with sort runs small enough to need merging.
 */
static void
test_merged_directories(void)
{
	static const char *const ordered[] = { "/", "/d", "/d/x", "/d/y", "/e" };
	static const uint64_t parents[] = { 0, 0, 1, 1, 0 };
	static const uint64_t children[] = { 2, 2, 0, 0, 0 };
	catalog_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 2048, .fan_in = 2 };
	const fsimg_catalog_entry_t *entry;

	for (unsigned order = 0; order < 2; order++) {
		fixture_open(&fixture, 1);
		if (order == 0)
			add_dated_directory(fixture.writer, "/d", 100);
		add_entry(fixture.writer, "/d/x", FSIMG_ENTRY_FILE);
		add_entry(fixture.writer, "/e", FSIMG_ENTRY_DIRECTORY);
		add_entry(fixture.writer, "/e", FSIMG_ENTRY_DIRECTORY);
		add_dated_directory(fixture.writer, "/d", 100);
		if (order == 1)
			add_dated_directory(fixture.writer, "/d", 100);
		add_entry(fixture.writer, "/d/y", FSIMG_ENTRY_FILE);
		CHECK(fsimg_writer_prepare(fixture.writer, &options, NULL) == FSIMG_OK);

		/* Six records collapse to four entries with dense IDs. */
		CHECK(fixture.writer->count == 4);
		for (uint64_t id = 0; id <= 4; id++) {
			CHECK(fsimg_catalog_get(fixture.writer, id, &entry, NULL) == FSIMG_OK);
			CHECK(strcmp(entry->entry.path, ordered[id]) == 0);
			CHECK(entry->parent_id == parents[id]);
			CHECK(entry->children == children[id]);
		}
		CHECK(fsimg_catalog_get(fixture.writer, 5, &entry, NULL) == FSIMG_ERANGE);
		CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
		CHECK(entry->entry.metadata.valid == FSIMG_META_MODIFIED);
		CHECK(entry->entry.metadata.modified.seconds == 100);
		fixture_close(&fixture);
	}
}

/* Equal paths merge only when every record is a directory. Any other run
 * fails preparation naming the path, also beneath a merged directory.
 */
static void
test_path_collisions(void)
{
	static const struct {
		size_t			 records;
		fsimg_entry_kind_t	 kinds[3];
	} cases[] = {
		{ 2, { FSIMG_ENTRY_FILE, FSIMG_ENTRY_FILE } },
		{ 2, { FSIMG_ENTRY_FILE, FSIMG_ENTRY_DIRECTORY } },
		{ 2, { FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_FILE } },
		{ 2, { FSIMG_ENTRY_SYMLINK, FSIMG_ENTRY_DIRECTORY } },
		{ 2, { FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_SYMLINK } },
		{ 2, { FSIMG_ENTRY_SYMLINK, FSIMG_ENTRY_SYMLINK } },
		{ 2, { FSIMG_ENTRY_SYMLINK, FSIMG_ENTRY_FILE } },
		{ 2, { FSIMG_ENTRY_FIFO, FSIMG_ENTRY_DIRECTORY } },
		{ 3, { FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_FILE } },
		{ 3, { FSIMG_ENTRY_FILE, FSIMG_ENTRY_DIRECTORY, FSIMG_ENTRY_DIRECTORY } }
	};
	catalog_fixture_t fixture;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture, 2);
		use_provider(&fixture);
		add_entry(fixture.writer, "/d", FSIMG_ENTRY_DIRECTORY);
		add_entry(fixture.writer, "/d", FSIMG_ENTRY_DIRECTORY);
		for (size_t record = 0; record < cases[i].records; record++)
			add_entry(fixture.writer, "/d/a", cases[i].kinds[record]);
		add_entry(fixture.writer, "/d/b", FSIMG_ENTRY_FILE);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) == FSIMG_EINVAL);
		if (strcmp(error.path, "/d/a") != 0)
			fprintf(stderr, "collision case %zu named '%s': %s\n", i,
			    error.path, error.message);
		CHECK(strcmp(error.path, "/d/a") == 0 && !error.path_truncated);
		CHECK(strstr(error.message, "more than one source") != NULL);
		CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
		CHECK(!fixture.writer->catalog_ready && fixture.writer->index == NULL);
		fixture_close(&fixture);
	}
}

/* Programmatic records of one directory that encode differently collide:
 * nothing ranks one above the other, so keeping either would discard the
 * other's metadata on an arbitrary basis. The collision names the path in
 * either insertion order, for optional and required metadata alike.
 */
static void
test_merged_metadata(void)
{
	fsimg_entry_t bare = { .path = "/d", .kind = FSIMG_ENTRY_DIRECTORY };
	fsimg_entry_t owned = { .path = "/d", .kind = FSIMG_ENTRY_DIRECTORY };
	catalog_fixture_t fixture;
	fsimg_error_t error;

	owned.metadata.valid = FSIMG_META_OWNER;
	owned.metadata.uid = 1000;
	owned.metadata.gid = 1000;
	for (unsigned with_required = 0; with_required < 2; with_required++) {
		owned.metadata.required = with_required == 0 ? 0 : FSIMG_META_OWNER;
		for (unsigned order = 0; order < 2; order++) {
			fixture_open(&fixture, 2);
			CHECK(fsimg_writer_add(fixture.writer,
			    order == 0 ? &owned : &bare, NULL, NULL) == FSIMG_OK);
			CHECK(fsimg_writer_add(fixture.writer,
			    order == 0 ? &bare : &owned, NULL, NULL) == FSIMG_OK);
			CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) ==
			    FSIMG_EINVAL);
			CHECK(strcmp(error.path, "/d") == 0);
			CHECK(strstr(error.message, "more than one source") != NULL);
			fixture_close(&fixture);
		}
	}

	/* Two dated records differing only in their time collide as well, and
	 * so does a differing record after two identical ones: each record is
	 * compared with the kept one, not with its predecessor.
	 */
	for (unsigned identical = 1; identical <= 2; identical++) {
		fixture_open(&fixture, 2);
		for (unsigned i = 0; i < identical; i++)
			add_dated_directory(fixture.writer, "/d", 100);
		add_dated_directory(fixture.writer, "/d", 200);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "/d") == 0);
		fixture_close(&fixture);
	}
}

/* The rank fields are closed: an unknown origin, nonzero padding, an order
 * on a programmatic record, a scanned order naming no registered root, or a
 * scanned record with provider storage, or a programmatic record with
 * generated or host storage, is corruption. Each case breaks
 * one rule: a pretended registered root keeps the order rule satisfied
 * where another rule is under test. A scanned zero-source file is valid,
 * as a manifest's zero file is one.
 */
static void
test_corrupt_rank(void)
{
	static const struct {
		unsigned		 offset;
		uint32_t		 value;
		fsimg_entry_kind_t	 kind;
		uint64_t		 roots;
	} corruptions[] = {
		{ FSIMG_CATALOG_ORIGIN, 2, FSIMG_ENTRY_DIRECTORY, 0 },
		{ FSIMG_CATALOG_ORIGIN + 4, 1, FSIMG_ENTRY_DIRECTORY, 0 },
		{ FSIMG_CATALOG_ORDER, 1, FSIMG_ENTRY_DIRECTORY, 0 },
		{ FSIMG_CATALOG_ORIGIN, FSIMG_ORIGIN_SCANNED, FSIMG_ENTRY_DIRECTORY, 0 },
		{ FSIMG_CATALOG_ORIGIN, FSIMG_ORIGIN_SCANNED, FSIMG_ENTRY_SYMLINK, 1 },
		{ FSIMG_CATALOG_STORAGE, FSIMG_STORAGE_GENERATED, FSIMG_ENTRY_FILE, 1 },
		{ FSIMG_CATALOG_STORAGE, FSIMG_STORAGE_HOST, FSIMG_ENTRY_FILE, 1 }
	};
	catalog_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	unsigned char bytes[4];

	for (size_t i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); i++) {
		fixture_open(&fixture, 2);
		if (corruptions[i].kind == FSIMG_ENTRY_SYMLINK)
			use_provider(&fixture);
		add_entry(fixture.writer, "/entry", corruptions[i].kind);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		fixture.writer->root_count = corruptions[i].roots;
		CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
		/* The first record's header starts eight bytes into its stream. */
		fsimg_store_le32(bytes, corruptions[i].value);
		CHECK(fsimg_store_write(fixture.writer->ordered->store,
		    8 + corruptions[i].offset, bytes, sizeof(bytes), NULL) == FSIMG_OK);
		CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) ==
		    FSIMG_ECORRUPT);
		fixture_close(&fixture);
	}
}

static fsimg_status_t
generated_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error)
{
	fsimg_error_clear(error);
	return (FSIMG_OK);
}

static fsimg_status_t
generated_read(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred, fsimg_error_t *error)
{
	unsigned char *bytes = buffer;

	for (size_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)(id + offset + i);
	*transferred = length;
	fsimg_error_clear(error);
	return (FSIMG_OK);
}

/* Register the generated-content provider as the writer's only source. */
static void
use_provider(catalog_fixture_t *fixture)
{
	fsimg_source_ops_t provider;

	fsimg_source_ops_init(&provider);
	provider.validate = generated_validate;
	provider.read_at = generated_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &provider, NULL, NULL) ==
	    FSIMG_OK);
}

static void
test_scale(uint64_t files, bool wide)
{
	catalog_fixture_t fixture;
	fsimg_source_ops_t provider;
	fsimg_entry_t entry = { .kind = FSIMG_ENTRY_FILE, .size = 7 };
	fsimg_source_t reference = { .kind = FSIMG_SOURCE_PROVIDER };
	const fsimg_catalog_entry_t *current;
	fsimg_stats_t stats;
	struct timespec start, end;
	char path[80];
	unsigned char data[7];
	uint64_t groups, id, file, parent;
	size_t transferred;
	double seconds;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	groups = wide ? 0 : files / 1000 + (files % 1000 != 0);
	fixture_open_budget(&fixture, 256,
	    (wide ? UINT64_C(16) : UINT64_C(256)) * 1024 * 1024, files + groups);
	fsimg_source_ops_init(&provider);
	provider.validate = generated_validate;
	provider.read_at = generated_read;
	CHECK(fsimg_writer_set_sources(fixture.writer, &provider, NULL, NULL) == FSIMG_OK);
	for (uint64_t i = files; i != 0; i--) {
		file = i - 1;
		if (wide)
			snprintf(path, sizeof(path), "/f%08" PRIu64, file);
		else
			snprintf(path, sizeof(path), "/d%08" PRIu64 "/f%08" PRIu64,
			    file / 1000, file);
		entry.path = path;
		reference.id = file;
		CHECK(fsimg_writer_add(fixture.writer, &entry, &reference, NULL) == FSIMG_OK);
	}
	for (uint64_t i = groups; i != 0; i--) {
		snprintf(path, sizeof(path), "/d%08" PRIu64, i - 1);
		add_entry(fixture.writer, path, FSIMG_ENTRY_DIRECTORY);
	}
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	id = 1;
	if (wide) {
		for (file = 0; file < files; file++) {
			CHECK(fsimg_catalog_get(fixture.writer, id++, &current, NULL) == FSIMG_OK);
			snprintf(path, sizeof(path), "/f%08" PRIu64, file);
			CHECK(strcmp(current->entry.path, path) == 0 && current->parent_id == 0);
			CHECK(current->source_id == file && current->entry.size == 7);
		}
		CHECK(fsimg_catalog_get(fixture.writer, 0, &current, NULL) == FSIMG_OK);
		CHECK(current->children == files);
	}
	for (uint64_t group = 0; group < groups; group++) {
		CHECK(fsimg_catalog_get(fixture.writer, id, &current, NULL) == FSIMG_OK);
		snprintf(path, sizeof(path), "/d%08" PRIu64, group);
		CHECK(strcmp(current->entry.path, path) == 0 && current->parent_id == 0);
		parent = id++;
		for (file = group * 1000; file < files && file < (group + 1) * 1000; file++) {
			CHECK(fsimg_catalog_get(fixture.writer, id++, &current, NULL) == FSIMG_OK);
			snprintf(path, sizeof(path), "/d%08" PRIu64 "/f%08" PRIu64, group, file);
			CHECK(strcmp(current->entry.path, path) == 0 && current->parent_id == parent);
			CHECK(current->source_id == file && current->entry.size == 7);
			if (file % 1000 != 0)
				continue;
			CHECK(fsimg_source_open(fixture.writer, current->id, NULL) == FSIMG_OK);
			CHECK(fsimg_source_read(fixture.writer, 0, data, sizeof(data), &transferred, NULL) == FSIMG_OK);
			CHECK(transferred == sizeof(data));
			for (size_t j = 0; j < sizeof(data); j++)
				CHECK(data[j] == (unsigned char)(file + j));
			CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
		}
	}
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_peak <= fixture.ctx->options.memory_bytes);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	seconds = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
	printf("catalog scale: files=%" PRIu64 ", directories=%" PRIu64
	    ", memory peak=%" PRIu64 ", scratch peak=%" PRIu64
	    ", descriptors peak=%" PRIu64 ", IO calls=%" PRIu64 ", seconds=%.3f\n",
	    files, groups, stats.memory_peak, stats.scratch_peak,
	    stats.descriptors_peak, stats.io_calls, seconds);
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--long-directory") == 0) {
		test_long_directory();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--corrupt-metadata") == 0) {
		test_corrupt_metadata();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--inspection-failure") == 0) {
		test_inspection_failure();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--short-child") == 0) {
		test_short_child_record();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--child-order") == 0) {
		test_child_order();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--index-reference") == 0) {
		test_index_reference();
		return (0);
	}
	if (argc == 3 && (strcmp(argv[1], "--scale") == 0 ||
	    strcmp(argv[1], "--wide") == 0)) {
		char *end;
		uint64_t files;

		errno = 0;
		files = strtoull(argv[2], &end, 10);
		CHECK(errno == 0 && *end == '\0' && files > 0 && files <= 10000000);
		test_scale(files, strcmp(argv[1], "--wide") == 0);
		return (0);
	}
	CHECK(argc == 1);
	for (uint32_t pages = 1; pages <= 16; pages *= 4) {
		test_order(pages, 2);
		test_order(pages, 8);
	}
	test_planning_reports_merged_catalog_once();
	test_failed_planning_preserves_entry_callbacks();
	test_planning_without_entry_callback();
	test_long_directory();
	test_deep_catalog();
	test_invalid_catalogs();
	test_metadata();
	test_posix_metadata();
	test_entry_kinds();
	test_invalid_kinds();
	test_failures();
	test_corrupt_metadata();
	test_inspection_failure();
	test_short_child_record();
	test_child_order();
	test_index_reference();
	test_merged_directories();
	test_path_collisions();
	test_merged_metadata();
	test_corrupt_rank();
	return (0);
}

static void
fixture_open_observed(catalog_fixture_t *fixture, entry_log_t *log)
{
	fixture_open_options(fixture, 2, UINT64_C(16) * 1024 * 1024, 0, log);
	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_writer_new(fixture->ctx, "fat16", NULL, &fixture->writer,
	    NULL) == FSIMG_OK);
}

/* Component tuples put /a/child before /a-. Two adds contribute the same
 * parent; the implicit root and the merged-away directory contribute no event.
 */
static void
test_planning_reports_merged_catalog_once(void)
{
	catalog_fixture_t fixture;
	entry_log_t log = { 0 };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_open_observed(&fixture, &log);
	add_entry(fixture.writer, "/a-", FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, "/a", FSIMG_ENTRY_DIRECTORY);
	add_entry(fixture.writer, "/a/z", FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, "/a", FSIMG_ENTRY_DIRECTORY);
	add_entry(fixture.writer, "/a/b", FSIMG_ENTRY_FILE);
	CHECK(log.count == 0);

	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.entries == 4 && log.count == info.entries);
	expect_entry(&log, 0, "/a", FSIMG_ENTRY_DIRECTORY);
	expect_entry(&log, 1, "/a/b", FSIMG_ENTRY_FILE);
	expect_entry(&log, 2, "/a/z", FSIMG_ENTRY_FILE);
	expect_entry(&log, 3, "/a-", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(log.count == 4);
	fixture_close(&fixture);
	/* The callback owns these copies even after the context is gone. */
	expect_entry(&log, 2, "/a/z", FSIMG_ENTRY_FILE);
}

static void
test_failed_planning_preserves_entry_callbacks(void)
{
	catalog_fixture_t fixture;
	entry_log_t log = { 0 };
	fsimg_entry_t collision = { .path = "/collision", .kind = FSIMG_ENTRY_FILE,
	    .size = 1 };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	size_t collisions;

	fixture_open_observed(&fixture, &log);
	use_provider(&fixture);
	add_entry(fixture.writer, "/a", FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, "/link", FSIMG_ENTRY_SYMLINK);
	CHECK(log.count == 0);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(plan == NULL);
	CHECK(strcmp(error.path, "/link") == 0);
	CHECK(log.count == 2);
	expect_entry(&log, 0, "/a", FSIMG_ENTRY_FILE);
	expect_entry(&log, 1, "/link", FSIMG_ENTRY_SYMLINK);
	fixture_close(&fixture);

	bzero(&log, sizeof(log));
	fixture_open_observed(&fixture, &log);
	add_entry(fixture.writer, "/a", FSIMG_ENTRY_FILE);
	add_entry(fixture.writer, "/collision", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_add(fixture.writer, &collision, &zero, NULL) == FSIMG_OK);
	CHECK(log.count == 0);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == FSIMG_EINVAL);
	CHECK(plan == NULL && strcmp(error.path, "/collision") == 0);
	CHECK(log.count > 0);
	expect_entry(&log, 0, "/a", FSIMG_ENTRY_FILE);
	collisions = 0;
	for (size_t i = 0; i < log.count; i++) {
		CHECK(strcmp(log.entries[i].path, "/") != 0);
		if (strcmp(log.entries[i].path, "/collision") == 0)
			collisions++;
	}
	CHECK(collisions == 1);
	fixture_close(&fixture);
}

static void
test_planning_without_entry_callback(void)
{
	catalog_fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_open_observed(&fixture, NULL);
	add_entry(fixture.writer, "/dir", FSIMG_ENTRY_DIRECTORY);
	add_entry(fixture.writer, "/dir/file", FSIMG_ENTRY_FILE);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.entries == 2);
	fixture_close(&fixture);
}
