/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "socket.h"
#include "source.h"
#include "test.h"

typedef enum provider_script {
	PROVIDER_SHORT_READS,
	PROVIDER_INTERRUPTED,
	PROVIDER_PARTIAL_INTERRUPTION,
	PROVIDER_PREMATURE_EOF,
	PROVIDER_OVERREPORT,
	PROVIDER_PARTIAL_ERROR,
	PROVIDER_CHANGED,
	PROVIDER_EMPTY_RANGE,
	PROVIDER_REPEATED_INTERRUPTION
} provider_script_t;

typedef struct source_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_source_cursor_t *active;
	fsimg_catalog_entry_t provider_entry;
	fsimg_stats_t	 baseline;
	unsigned	 calls;
	unsigned	 validations;
	provider_script_t script;
	bool		 check_reentry;
	bool		 changed;
	bool		 cancelled;
	bool		 mutate_directory;
	int		 root;
	int		 parent;
	char		 directory[64];
} source_fixture_t;

static void fixture_open(source_fixture_t *);
static void fixture_close(source_fixture_t *);
static void remove_contents(int);
static void write_file(int, const char *, const char *);
static void configure_provider(source_fixture_t *, uint64_t);
static const fsimg_catalog_entry_t *find_entry(fsimg_writer_t *, const char *);
static bool catalog_reaches(fsimg_writer_t *, const char *);
static void test_working_storage(void);
static fsimg_status_t provider_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t provider_range(void *, uint64_t, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
static bool cancellation(void *);
static void progress(void *, const fsimg_progress_t *);
static void test_active_reopen(void);
static void test_free_active(void);
static void test_failed_writer(void);
static void test_reentry(void);
static void test_provider(void);
static void test_host_tree(void);
static void test_mutation(void);
static void test_rejection(void);
static int make_directory(int, const char *);
static void set_modified(int, const char *, time_t);
static void populate_root(int, const char *, time_t);
static void read_source(fsimg_writer_t *, uint64_t, const char *);
static void test_merged_roots(void);
static void test_scanned_rank(void);
static void test_repeated_tree(void);
static void test_root_collisions(void);
static void test_file_root(void);
static void test_file_root_changes(void);
static void test_file_root_rejection(void);
static void test_symlink_import(void);
static void check_long_target(size_t, fsimg_status_t);
static void test_symlink_changes(void);
static void test_posix_import(void);
static void test_limits_and_failures(void);
static void test_corrupt_source(void);
static void test_ancestor_pivot(void);
static void test_depth_limit(void);
static void test_provider_work_limit(void);
static void test_snapshot_lifetime(void);
static void test_source_reference_eof(void);
static void test_directory_records(void);
static void test_directory_interruption(void);
static void test_unregistered_provider(void);

static void
write_file(int parent, const char *name, const char *contents)
{
	size_t length;
	int fd;

	fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	length = strlen(contents);
	CHECK(write(fd, contents, length) == (ssize_t)length);
	CHECK(close(fd) == 0);
}

static bool
cancellation(void *argument)
{
	source_fixture_t *fixture = argument;

	return (fixture->cancelled);
}

static void
progress(void *argument, const fsimg_progress_t *state)
{
	source_fixture_t *fixture = argument;
	const struct timespec changed[2] = {
		{ .tv_nsec = UTIME_OMIT }, { .tv_sec = 1 }
	};

	if (fixture->mutate_directory && state->entries != 0) {
		fixture->mutate_directory = false;
		write_file(fixture->root, "changed", "new namespace entry");
		/* Some filesystems stamp multiple operations in one clock tick. */
		CHECK(futimens(fixture->root, changed) == 0);
	}
}

static void
fixture_open(source_fixture_t *fixture)
{
	fsimg_options_t options;
	int scratch;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory), "source-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	CHECK(mkdirat(fixture->parent, "source", 0700) == 0);
	CHECK(mkdirat(fixture->parent, "scratch", 0700) == 0);
	fixture->root = openat(fixture->parent, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	scratch = openat(fixture->parent, "scratch", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->root != -1 && scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_path_bytes = 4096;
	options.scratch_directory_fd = scratch;
	options.cancelled = cancellation;
	options.cancel_argument = fixture;
	options.progress = progress;
	options.progress_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_test_meta_cache(fixture->ctx, 8, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_collect_new(fixture->ctx, &fixture->writer, NULL) == FSIMG_OK);
}

static void
remove_contents(int parent)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int fd, child;

	fd = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	directory = fdopendir(fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		CHECK(fstatat(parent, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(st.st_mode)) {
			child = openat(parent, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			CHECK(child != -1);
			remove_contents(child);
			CHECK(close(child) == 0);
			CHECK(unlinkat(parent, entry->d_name, AT_REMOVEDIR) == 0);
		} else {
			CHECK(unlinkat(parent, entry->d_name, 0) == 0);
		}
	}
	CHECK(closedir(directory) == 0);
}

static void
fixture_close(source_fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	CHECK(stats.scratch_bytes == 0 && stats.scratch_physical_bytes == 0);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->root) == 0);
	remove_contents(fixture->parent);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->directory) == 0);
}

static fsimg_status_t
provider_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	source_fixture_t *fixture = argument;
	fsimg_io_t *io;
	fsimg_stats_t stats;

	CHECK(id == 7);
	fixture->validations++;
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_ESTATE);
	if (fixture->check_reentry) {
		fixture->check_reentry = false;
		io = fixture->active->io;
		CHECK(fsimg_source_open(fixture->writer, fixture->provider_entry.id,
		    NULL) == FSIMG_ESTATE);
		CHECK(fixture->active->io == io && fixture->active->active);
	}
	if (fixture->changed)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "test provider changed"));
	return (FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	source_fixture_t *fixture = argument;
	unsigned char *bytes = buffer;
	size_t count;

	CHECK(id == 7 && length != 0);
	fixture->calls++;
	*transferred = 0;
	if (fixture->script == PROVIDER_REPEATED_INTERRUPTION ||
	    (fixture->script == PROVIDER_INTERRUPTED && fixture->calls == 1))
		return (fsimg_error_set(error, FSIMG_EIO, EINTR, FSIMG_OP_SOURCE,
		    "test interruption"));
	if (fixture->script == PROVIDER_PREMATURE_EOF)
		return (FSIMG_OK);
	if (fixture->script == PROVIDER_OVERREPORT) {
		*transferred = length + 1;
		return (FSIMG_OK);
	}
	count = length > 2 ? 2 : length;
	for (size_t i = 0; i < count; i++)
		bytes[i] = (unsigned char)(id + offset + i);
	*transferred = count;
	if (fixture->script == PROVIDER_PARTIAL_INTERRUPTION && fixture->calls == 1)
		return (fsimg_error_set(error, FSIMG_EIO, EINTR, FSIMG_OP_SOURCE,
		    "test partial interruption"));
	if (fixture->script == PROVIDER_PARTIAL_ERROR)
		return (fsimg_error_set(error, FSIMG_EIO, ENOSPC, FSIMG_OP_SOURCE,
		    "test partial error"));
	if (fixture->script == PROVIDER_CHANGED)
		fixture->changed = true;
	return (FSIMG_OK);
}

static fsimg_status_t
provider_range(void *argument, uint64_t id, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error FSIMG_UNUSED)
{
	source_fixture_t *fixture = argument;

	CHECK(id == 7);
	range->offset = offset;
	range->length = fixture->script == PROVIDER_EMPTY_RANGE ? 0 : 1;
	range->kind = FSIMG_RANGE_DATA;
	return (FSIMG_OK);
}

static void
configure_provider(source_fixture_t *fixture, uint64_t length)
{
	fsimg_source_ops_t ops;
	fsimg_entry_t entry = { .path = "/provider", .kind = FSIMG_ENTRY_FILE,
	    .size = length };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 7 };
	const fsimg_catalog_entry_t *current;

	fsimg_source_ops_init(&ops);
	ops.max_transfer = 5;
	ops.validate = provider_validate;
	ops.read_at = provider_read;
	ops.next_range = provider_range;
	CHECK(fsimg_writer_set_sources(fixture->writer, &ops, fixture, NULL) == FSIMG_OK);
	bzero(&ops, sizeof(ops));
	CHECK(fsimg_writer_add(fixture->writer, &entry, &source, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture->writer, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture->writer, 1, &current, NULL) == FSIMG_OK);
	fixture->provider_entry = *current;
}

static void
test_active_reopen(void)
{
	source_fixture_t fixture;
	fsimg_io_t *io;

	fixture_open(&fixture);
	configure_provider(&fixture, 16);
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	io = fixture.writer->source.io;
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_ESTATE);
	CHECK(fixture.writer->source.active && fixture.writer->source.io == io);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	fixture_close(&fixture);
}

static void
test_free_active(void)
{
	source_fixture_t fixture;
	fsimg_stats_t stats;

	fixture_open(&fixture);
	configure_provider(&fixture, 16);
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	fsimg_writer_free(fixture.writer);
	fixture.writer = NULL;
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture.baseline.memory_bytes);
	fixture_close(&fixture);
}

static void
test_failed_writer(void)
{
	source_fixture_t fixture;
	unsigned char byte;
	size_t count;

	fixture_open(&fixture);
	configure_provider(&fixture, 16);
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_fail(fixture.writer, FSIMG_EIO) == FSIMG_EIO);
	CHECK(!fixture.writer->source.active && fixture.writer->source.io == NULL);
	CHECK(fsimg_source_read(fixture.writer, 0, &byte, 1, &count, NULL) == FSIMG_ESTATE);
	CHECK(count == 0);
	fixture_close(&fixture);
}

static void
test_reentry(void)
{
	source_fixture_t fixture;

	fixture_open(&fixture);
	configure_provider(&fixture, 16);
	fixture.active = &fixture.writer->source;
	fixture.check_reentry = true;
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	fixture_close(&fixture);
}

static void
test_provider(void)
{
	source_fixture_t fixture;
	fsimg_range_t range;
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned char bytes[16];
	size_t count;

	for (unsigned script = PROVIDER_SHORT_READS; script <= PROVIDER_EMPTY_RANGE; script++) {
		fixture_open(&fixture);
		configure_provider(&fixture, sizeof(bytes));
		fixture.script = (provider_script_t)script;
		CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
		if (script == PROVIDER_EMPTY_RANGE) {
			CHECK(fsimg_source_range(fixture.writer, 0, &range, NULL) == FSIMG_EIO);
			CHECK(range.length == 0);
			fixture_close(&fixture);
			continue;
		}
		CHECK(fsimg_source_range(fixture.writer, 16, &range, NULL) == FSIMG_DONE);
		CHECK(fsimg_source_range(fixture.writer, 0, &range, NULL) == FSIMG_OK);
		CHECK(range.offset == 0 && range.length == 1 && range.kind == FSIMG_RANGE_DATA);
		status = fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, &error);
		if (script <= PROVIDER_PARTIAL_INTERRUPTION ||
		    script == PROVIDER_CHANGED) {
			CHECK(status == FSIMG_OK && count == sizeof(bytes));
			for (size_t i = 0; i < count; i++)
				CHECK(bytes[i] == i + 7);
			CHECK(fsimg_source_read(fixture.writer, 16, bytes, 1, &count, NULL) == FSIMG_OK);
			CHECK(count == 0);
			/* Validation runs at open and at finish, not around each
			 * read, so a change during the copy surfaces at finish.
			 */
			CHECK(fsimg_source_finish(fixture.writer, NULL) ==
			    (script == PROVIDER_CHANGED ? FSIMG_ESOURCE_CHANGED : FSIMG_OK));
			CHECK(fixture.validations == 2);
		} else {
			CHECK(status == FSIMG_EIO);
			CHECK(count == (script == PROVIDER_PARTIAL_ERROR ? 2u : 0u));
			/* Open, and once more to classify the failed read. */
			CHECK(fixture.validations == 2);
			CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
			CHECK(fixture.writer->catalog_ready);
		}
		fixture_close(&fixture);
	}
	fixture_open(&fixture);
	configure_provider(&fixture, 0);
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	CHECK(fixture.validations == 2 && fixture.calls == 0);
	fixture_close(&fixture);
}

static const fsimg_catalog_entry_t *
find_entry(fsimg_writer_t *writer, const char *path)
{
	const fsimg_catalog_entry_t *entry;

	for (uint64_t id = 1; id <= writer->count; id++) {
		CHECK(fsimg_catalog_get(writer, id, &entry, NULL) == FSIMG_OK);
		if (strcmp(entry->entry.path, path) == 0)
			return (entry);
	}
	CHECK(false);
	return (NULL);
}

static void
test_host_tree(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char bytes[32];
	unsigned char directory_bytes[8192];
	size_t count;
	int child;
	off_t position;

	fixture_open(&fixture);
	write_file(fixture.root, "file", "hello source");
	write_file(fixture.root, "empty", "");
	write_file(fixture.root, "fsimg-meta-v1-legitimate", "ordinary name");
	CHECK(linkat(fixture.root, "file", fixture.root, "hardlink", 0) == 0);
	CHECK(mkdirat(fixture.root, "dir", 0700) == 0);
	child = openat(fixture.root, "dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(child != -1);
	write_file(child, "child", "nested bytes");
	CHECK(close(child) == 0);
	/* A prior enumeration changes the borrowed root's position. */
	CHECK(fsimg_host_directory_read(fixture.root, directory_bytes,
	    sizeof(directory_bytes), &count) == 0);
	position = lseek(fixture.root, 0, SEEK_CUR);
	CHECK(position != -1);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/one", NULL) == FSIMG_OK);
	CHECK(lseek(fixture.root, 0, SEEK_CUR) == position);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/two", NULL) == FSIMG_OK);
	CHECK(lseek(fixture.root, 0, SEEK_CUR) == position);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fixture.writer->count == 14);
	entry = find_entry(fixture.writer, "/one/file");
	CHECK(entry->entry.metadata.valid != 0);
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, NULL) == FSIMG_OK);
	CHECK(count == 12 && bcmp(bytes, "hello source", count) == 0);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	entry = find_entry(fixture.writer, "/two/dir/child");
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, NULL) == FSIMG_OK);
	CHECK(count == 12 && bcmp(bytes, "nested bytes", count) == 0);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	fixture_close(&fixture);
}

/* A scratch directory the context adopts after it was made, as the worker's
 * is, is refused as source content like one given at creation.
 */
static void
test_adopted_scratch(void)
{
	fsimg_options_t options;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	int parent, scratch;

	parent = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(mkdirat(parent, "adopted-scratch", 0700) == 0);
	scratch = openat(parent, "adopted-scratch", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_fd_reserve(ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_adopt_scratch(ctx, scratch, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_collect_new(ctx, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add_tree(writer, scratch, "/", &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "scratch directory") != NULL);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
	CHECK(unlinkat(parent, "adopted-scratch", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
}

static void
test_mutation(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	fsimg_error_t error;
	char bytes[16];
	size_t count;
	int fd;

	for (unsigned scenario = 0; scenario < 3; scenario++) {
		fixture_open(&fixture);
		write_file(fixture.root, "file", "original");
		CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		entry = find_entry(fixture.writer, "/file");
		if (scenario == 0) {
			CHECK(unlinkat(fixture.root, "file", 0) == 0);
			write_file(fixture.root, "file", "replacement");
			CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_ESOURCE_CHANGED);
		} else {
			CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);
			fd = openat(fixture.root, "file", O_WRONLY | O_NOFOLLOW);
			CHECK(fd != -1);
			CHECK(ftruncate(fd, 1) == 0);
			CHECK(close(fd) == 0);
			if (scenario == 1) {
				/* The read stops at the new end, and the file
				 * is reported changed rather than short.
				 */
				CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, &error) == FSIMG_ESOURCE_CHANGED);
				CHECK(count == 1);
				CHECK(error.status == FSIMG_ESOURCE_CHANGED &&
				    error.operation == FSIMG_OP_SOURCE && error.offset_valid);
			} else {
				CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_ESOURCE_CHANGED);
			}
		}
		CHECK(fixture.writer->roots == NULL && fixture.writer->catalog_ready);
		fixture_close(&fixture);
	}
	fixture_open(&fixture);
	write_file(fixture.root, "file", "original");
	fixture.mutate_directory = true;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_ESOURCE_CHANGED);
	fixture_close(&fixture);
}

static void
test_rejection(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	int listener;

	fixture_open(&fixture);
	CHECK(mkfifoat(fixture.root, "fifo", 0600) == 0);
	alarm(5);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error) == FSIMG_EUNSUPPORTED);
	alarm(0);
	/* A rejected object is named by its image path. */
	CHECK(strcmp(error.path, "/fifo") == 0);
	fixture_close(&fixture);
	/* A socket, which no host opens, is rejected the same way. */
	fixture_open(&fixture);
	listener = bind_socket(fixture.root, "socket");
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/socket") == 0);
	CHECK(close(listener) == 0);
	CHECK(unlinkat(fixture.root, "socket", 0) == 0);
	fixture_close(&fixture);
	fixture_open(&fixture);
	CHECK(fsimg_writer_add_tree(fixture.writer,
	    fixture.ctx->options.scratch_directory_fd, "/", &error) == FSIMG_EINVAL);
	CHECK(fixture.writer->roots == NULL);
	fixture_close(&fixture);
	test_adopted_scratch();
	test_working_storage();
}

/* Whether the catalog holds path or anything beneath it. */
static bool
catalog_reaches(fsimg_writer_t *writer, const char *path)
{
	const fsimg_catalog_entry_t *entry;
	size_t length = strlen(path);

	for (uint64_t id = 1; id <= writer->count; id++) {
		CHECK(fsimg_catalog_get(writer, id, &entry, NULL) == FSIMG_OK);
		if (strncmp(entry->entry.path, path, length) == 0 &&
		    (entry->entry.path[length] == '\0' || entry->entry.path[length] == '/'))
			return (true);
	}
	return (false);
}

/*
 * The CLI may prepare its staging and scratch directories inside the tree
 * it imports, as `cd src && fsimg cf out.img .` does. A scan passes over
 * them, by identity, without importing or entering them; ordinary
 * directories with similar names are imported. Named as a root, either is
 * still refused.
 */
static void
test_working_storage(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	int output, child;

	fixture_open(&fixture);
	CHECK(mkdirat(fixture.parent, "output", 0700) == 0);
	output = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(output != -1);
	write_file(output, "image", "staged");
	CHECK(fsimg_host_file(output, &fixture.writer->output_root) == 0);
	fixture.writer->output_root_known = true;
	child = openat(fixture.parent, "scratch", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(child != -1);
	write_file(child, "spool", "scratch");
	CHECK(close(child) == 0);
	CHECK(mkdirat(fixture.parent, "output-like", 0700) == 0);
	CHECK(mkdirat(fixture.parent, ".fsimg-output-ordinary", 0700) == 0);
	child = openat(fixture.parent, ".fsimg-output-ordinary", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(child != -1);
	write_file(child, "kept", "ordinary");
	CHECK(close(child) == 0);
	CHECK(mkdirat(fixture.parent, ".fsimg-scratch-ordinary", 0700) == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.parent, "/", &error) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(!catalog_reaches(fixture.writer, "/output"));
	CHECK(!catalog_reaches(fixture.writer, "/scratch"));
	CHECK(catalog_reaches(fixture.writer, "/source"));
	CHECK(catalog_reaches(fixture.writer, "/output-like"));
	CHECK(catalog_reaches(fixture.writer, "/.fsimg-output-ordinary/kept"));
	CHECK(catalog_reaches(fixture.writer, "/.fsimg-scratch-ordinary"));
	CHECK(fixture.writer->count == 5);
	fixture_close(&fixture);

	fixture_open(&fixture);
	CHECK(fsimg_host_file(fixture.parent, &fixture.writer->output_root) == 0);
	fixture.writer->output_root_known = true;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.parent, "/", &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "output directory") != NULL);
	CHECK(fixture.writer->roots == NULL);
	fixture_close(&fixture);
	CHECK(close(output) == 0);
}

/* Make parent/name a directory and return a read-only descriptor for it. */
static int
make_directory(int parent, const char *name)
{
	int fd;

	CHECK(mkdirat(parent, name, 0700) == 0);
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	return (fd);
}

/* Scanning records a host object's modification time as its metadata, so
 * distinct times tell which root's directory a merge kept.
 */
static void
set_modified(int parent, const char *name, time_t seconds)
{
	const struct timespec times[2] = {
		{ .tv_nsec = UTIME_OMIT }, { .tv_sec = seconds }
	};

	CHECK(utimensat(parent, name, times, AT_SYMLINK_NOFOLLOW) == 0);
}

/* One operand's tree: dir/<label> and only-<label>, each containing the
 * label, with dir/ modified at dir_seconds.
 */
static void
populate_root(int root, const char *label, time_t dir_seconds)
{
	char only[32];
	int directory;

	snprintf(only, sizeof(only), "only-%s", label);
	directory = make_directory(root, "dir");
	write_file(directory, label, label);
	CHECK(close(directory) == 0);
	write_file(root, only, label);
	set_modified(root, "dir", dir_seconds);
}

/* Open, read and finish one file entry, expecting exactly contents. */
static void
read_source(fsimg_writer_t *writer, uint64_t id, const char *contents)
{
	char bytes[64];
	size_t count;

	CHECK(fsimg_source_open(writer, id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_read(writer, 0, bytes, sizeof(bytes), &count,
	    NULL) == FSIMG_OK);
	CHECK(count == strlen(contents) && bcmp(bytes, contents, count) == 0);
	CHECK(fsimg_source_finish(writer, NULL) == FSIMG_OK);
}

/* Two directory roots at "/" whose trees both contain dir/ produce one /dir
 * holding the children of both. IDs, parent links and file contents are
 * the same for either registration order; only the merged directory's
 * metadata follows the order, coming from the root registered first.
 */
static void
test_merged_roots(void)
{
	static const char *const ordered[] = { "/", "/dir", "/dir/first",
	    "/dir/second", "/only-first", "/only-second" };
	static const char *const contents[] = { NULL, NULL, "first", "second",
	    "first", "second" };
	static const uint64_t parents[] = { 0, 0, 1, 1, 0, 0 };
	static const uint64_t children[] = { 3, 2, 0, 0, 0, 0 };
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	int second;

	for (unsigned order = 0; order < 2; order++) {
		fixture_open(&fixture);
		second = make_directory(fixture.parent, "second");
		populate_root(fixture.root, "first", 1000);
		populate_root(second, "second", 2000);
		if (order == 0) {
			CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
			CHECK(fsimg_writer_add_tree(fixture.writer, second, "/", NULL) == FSIMG_OK);
		} else {
			CHECK(fsimg_writer_add_tree(fixture.writer, second, "/", NULL) == FSIMG_OK);
			CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
		}
		CHECK(close(second) == 0);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);

		CHECK(fixture.writer->count == 5);
		for (uint64_t id = 0; id <= 5; id++) {
			CHECK(fsimg_catalog_get(fixture.writer, id, &entry, NULL) == FSIMG_OK);
			CHECK(strcmp(entry->entry.path, ordered[id]) == 0);
			CHECK(entry->parent_id == parents[id]);
			CHECK(entry->children == children[id]);
			if (contents[id] != NULL)
				read_source(fixture.writer, id, contents[id]);
		}
		CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
		CHECK(entry->entry.metadata.modified.seconds == (order == 0 ? 1000 : 2000));
		fixture_close(&fixture);
	}
}

/* Among records of one directory a scanned record outranks a programmatic
 * one whenever either was added, and the first-registered root outranks
 * later ones. Two programmatic records carry different attributes, which the
 * scanner never records: under a scanned record both are dropped without a
 * collision, and the capability unions planning checks show that their
 * metadata is dropped rather than merged. Both roots
 * populate /a, so /a is each root's own directory entry and /a/x is a
 * directory both trees contain.
 */
static void
test_scanned_rank(void)
{
	fsimg_entry_t synthesized = { .path = "/a", .kind = FSIMG_ENTRY_DIRECTORY,
	    .metadata = { .valid = FSIMG_META_ATTRIBUTES, .attributes = 0x01 } };
	fsimg_entry_t other = { .path = "/a", .kind = FSIMG_ENTRY_DIRECTORY,
	    .metadata = { .valid = FSIMG_META_ATTRIBUTES, .attributes = 0x02 } };
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	int second, first_root, later_root, directory;
	bool reversed, synthesized_first;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		reversed = (scenario & 1) != 0;
		synthesized_first = (scenario & 2) != 0;
		fixture_open(&fixture);
		second = make_directory(fixture.parent, "second");
		directory = make_directory(fixture.root, "x");
		CHECK(close(directory) == 0);
		directory = make_directory(second, "x");
		CHECK(close(directory) == 0);
		set_modified(fixture.root, "x", 1000);
		set_modified(second, "x", 2000);
		set_modified(fixture.parent, "source", 3000);
		set_modified(fixture.parent, "second", 4000);
		first_root = reversed ? second : fixture.root;
		later_root = reversed ? fixture.root : second;

		if (synthesized_first)
			CHECK(fsimg_writer_add(fixture.writer, &synthesized, NULL, NULL) == FSIMG_OK);
		else
			CHECK(fsimg_writer_add(fixture.writer, &other, NULL, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_add_tree(fixture.writer, first_root, "/a", NULL) == FSIMG_OK);
		CHECK(fsimg_writer_add_tree(fixture.writer, later_root, "/a", NULL) == FSIMG_OK);
		if (!synthesized_first)
			CHECK(fsimg_writer_add(fixture.writer, &synthesized, NULL, NULL) == FSIMG_OK);
		else
			CHECK(fsimg_writer_add(fixture.writer, &other, NULL, NULL) == FSIMG_OK);
		CHECK(close(second) == 0);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);

		CHECK(fixture.writer->count == 2);
		CHECK((fixture.writer->catalog_fields & FSIMG_META_ATTRIBUTES) == 0);
		CHECK(fsimg_catalog_get(fixture.writer, 1, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->entry.path, "/a") == 0 && entry->children == 1);
		CHECK((entry->entry.metadata.valid & FSIMG_META_MODIFIED) != 0);
		CHECK((entry->entry.metadata.valid & FSIMG_META_ATTRIBUTES) == 0);
		CHECK(entry->entry.metadata.modified.seconds == (reversed ? 4000 : 3000));
		CHECK(fsimg_catalog_get(fixture.writer, 2, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->entry.path, "/a/x") == 0 && entry->parent_id == 1);
		CHECK(entry->entry.metadata.modified.seconds == (reversed ? 2000 : 1000));
		fixture_close(&fixture);
	}
}

/* The same tree named twice merges its directories and then fails on its
 * first file in catalog order, whatever the host's directory order.
 */
static void
test_repeated_tree(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	int directory;

	fixture_open(&fixture);
	write_file(fixture.root, "b", "later file");
	directory = make_directory(fixture.root, "a");
	write_file(directory, "z", "first file");
	CHECK(close(directory) == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "/a/z") == 0);
	CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
	CHECK(fixture.writer->roots == NULL);
	fixture_close(&fixture);
}

/* Host roots collide like programmatic entries. The root at "/" holds f as
 * a file, directory or link; a rival then supplies /f as a regular-file
 * root, as a directory in a second root at "/", or as a programmatic
 * directory that requires an owner, which merging cannot keep.
 */
static void
test_root_collisions(void)
{
	typedef enum { HOLDS_FILE, HOLDS_DIRECTORY, HOLDS_LINK } holding_t;
	typedef enum {
		RIVAL_FILE_ROOT, RIVAL_SECOND_ROOT, RIVAL_REQUIRED_DIRECTORY
	} rival_t;
	static const struct {
		holding_t	 holding;
		rival_t		 rival;
	} cases[] = {
		{ HOLDS_FILE, RIVAL_FILE_ROOT },
		{ HOLDS_DIRECTORY, RIVAL_FILE_ROOT },
		{ HOLDS_LINK, RIVAL_SECOND_ROOT },
		{ HOLDS_DIRECTORY, RIVAL_REQUIRED_DIRECTORY }
	};
	fsimg_entry_t required = { .path = "/f", .kind = FSIMG_ENTRY_DIRECTORY };
	source_fixture_t fixture;
	fsimg_error_t error;
	int second, file, directory;

	required.metadata.valid = FSIMG_META_OWNER;
	required.metadata.required = FSIMG_META_OWNER;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture);
		if (cases[i].holding == HOLDS_FILE) {
			write_file(fixture.root, "f", "scanned file");
		} else if (cases[i].holding == HOLDS_DIRECTORY) {
			directory = make_directory(fixture.root, "f");
			CHECK(close(directory) == 0);
		} else {
			CHECK(symlinkat("target", fixture.root, "f") == 0);
		}
		CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);

		if (cases[i].rival == RIVAL_FILE_ROOT) {
			write_file(fixture.parent, "single", "file root");
			file = openat(fixture.parent, "single", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			CHECK(file != -1);
			CHECK(fsimg_writer_add_tree(fixture.writer, file, "/f", NULL) == FSIMG_OK);
			CHECK(close(file) == 0);
		} else if (cases[i].rival == RIVAL_SECOND_ROOT) {
			second = make_directory(fixture.parent, "second");
			directory = make_directory(second, "f");
			CHECK(close(directory) == 0);
			CHECK(fsimg_writer_add_tree(fixture.writer, second, "/", NULL) == FSIMG_OK);
			CHECK(close(second) == 0);
		} else {
			CHECK(fsimg_writer_add(fixture.writer, &required, NULL, NULL) == FSIMG_OK);
		}
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "/f") == 0);
		fixture_close(&fixture);
	}
}

/* A regular-file root contributes one file entry at its image path. Its
 * source borrows the root descriptor, which the registry keeps open, so
 * the file opens again after its source closes. Reads are positioned: the
 * descriptor's shared offset does not affect them.
 */
static void
test_file_root(void)
{
	fsimg_entry_t parent = { .path = "/d", .kind = FSIMG_ENTRY_DIRECTORY };
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	int file;

	fixture_open(&fixture);
	write_file(fixture.parent, "single", "file root bytes");
	file = openat(fixture.parent, "single",
	    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	CHECK(file != -1);
	CHECK(lseek(file, 5, SEEK_SET) == 5);
	CHECK(fsimg_writer_add_tree(fixture.writer, file, "/d/f", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(fixture.writer, &parent, NULL, NULL) == FSIMG_OK);
	CHECK(close(file) == 0);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);

	CHECK(fixture.writer->count == 2);
	entry = find_entry(fixture.writer, "/d/f");
	CHECK(entry->entry.kind == FSIMG_ENTRY_FILE && entry->entry.size == 15);
	CHECK(entry->storage == FSIMG_STORAGE_HOST && entry->parent_id == 1);
	CHECK((entry->entry.metadata.valid & FSIMG_META_MODIFIED) != 0);
	read_source(fixture.writer, entry->id, "file root bytes");
	CHECK(fixture.writer->roots != NULL && fixture.writer->roots->fd != -1);
	read_source(fixture.writer, entry->id, "file root bytes");
	fixture_close(&fixture);
}

/* A regular-file root is held to the recorded identity like any host
 * source: growth or a new modification time after adoption is a change,
 * whether it happens before the source opens or while it is open.
 */
static void
test_file_root_changes(void)
{
	static const struct {
		bool		 grows;		/* Otherwise only its mtime changes. */
		bool		 while_open;
	} cases[] = {
		{ .grows = true, .while_open = false },
		{ .grows = false, .while_open = false },
		{ .grows = true, .while_open = true },
		{ .grows = false, .while_open = true }
	};
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char bytes[32];
	size_t count;
	int file, writable;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture);
		write_file(fixture.parent, "single", "original");
		set_modified(fixture.parent, "single", 1000);
		file = openat(fixture.parent, "single", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(file != -1);
		CHECK(fsimg_writer_add_tree(fixture.writer, file, "/f", NULL) == FSIMG_OK);
		CHECK(close(file) == 0);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		entry = find_entry(fixture.writer, "/f");
		if (cases[i].while_open)
			CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);

		if (cases[i].grows) {
			writable = openat(fixture.parent, "single",
			    O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
			CHECK(writable != -1);
			CHECK(write(writable, "+", 1) == 1);
			CHECK(close(writable) == 0);
		} else {
			set_modified(fixture.parent, "single", 2000);
		}

		/* While open, the declared bytes still read; finish sees the
		 * change.
		 */
		if (cases[i].while_open) {
			CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes),
			    &count, NULL) == FSIMG_OK);
			CHECK(count == strlen("original"));
			CHECK(fsimg_source_finish(fixture.writer, NULL) ==
			    FSIMG_ESOURCE_CHANGED);
		} else
			CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) ==
			    FSIMG_ESOURCE_CHANGED);
		CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
		CHECK(fixture.writer->roots == NULL);
		fixture_close(&fixture);
	}
}

/* A regular file cannot populate the image root, and a root that is neither
 * a directory nor a regular file fails before anything is collected.
 */
static void
test_file_root_rejection(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	int fd;

	fixture_open(&fixture);
	write_file(fixture.parent, "single", "file root");
	fd = openat(fixture.parent, "single", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_writer_add_tree(fixture.writer, fd, "/", &error) == FSIMG_EINVAL);
	CHECK(fixture.writer->roots == NULL && fixture.writer->collected == 0);
	CHECK(close(fd) == 0);
	fixture_close(&fixture);

	fixture_open(&fixture);
	CHECK(mkfifoat(fixture.parent, "fifo", 0600) == 0);
	fd = openat(fixture.parent, "fifo", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_writer_add_tree(fixture.writer, fd, "/fifo", &error) == FSIMG_EINVAL);
	CHECK(fixture.writer->roots == NULL && fixture.writer->collected == 0);
	CHECK(close(fd) == 0);
	fixture_close(&fixture);

	/* A descriptor for the link itself needs a host-specific open flag. */
#if defined(O_SYMLINK) || defined(O_PATH)
	fixture_open(&fixture);
	write_file(fixture.parent, "single", "file root");
	CHECK(symlinkat("single", fixture.parent, "link") == 0);
#if defined(O_SYMLINK)
	fd = openat(fixture.parent, "link", O_RDONLY | O_SYMLINK | O_CLOEXEC);
#else
	fd = openat(fixture.parent, "link", O_PATH | O_NOFOLLOW | O_CLOEXEC);
#endif
	CHECK(fd != -1);
	CHECK(fsimg_writer_add_tree(fixture.writer, fd, "/link", &error) == FSIMG_EINVAL);
	CHECK(fixture.writer->roots == NULL && fixture.writer->collected == 0);
	CHECK(close(fd) == 0);
	fixture_close(&fixture);
#endif
}

/* A host link becomes a SYMLINK entry whose content is its target. The
 * scanner neither follows the link nor requires the target to exist.
 */
static void
test_symlink_import(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char bytes[64];
	size_t count;

	fixture_open(&fixture);
	CHECK(symlinkat("../outside/target", fixture.root, "link") == 0);
	CHECK(mkdirat(fixture.root, "dir", 0700) == 0);
	CHECK(symlinkat("dir", fixture.root, "directory-link") == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fixture.writer->count == 3);

	entry = find_entry(fixture.writer, "/link");
	CHECK(entry->entry.kind == FSIMG_ENTRY_SYMLINK);
	CHECK(entry->entry.size == strlen("../outside/target"));
	CHECK(entry->storage == FSIMG_STORAGE_HOST);
	CHECK((entry->entry.metadata.valid & (FSIMG_META_OWNER | FSIMG_META_MODE)) == 0);
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);
	CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count,
	    NULL) == FSIMG_OK);
	CHECK(count == 17 && bcmp(bytes, "../outside/target", count) == 0);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);

	/* A link to a directory is still a link; the scan does not descend. */
	entry = find_entry(fixture.writer, "/directory-link");
	CHECK(entry->entry.kind == FSIMG_ENTRY_SYMLINK && entry->entry.size == 3);
	fixture_close(&fixture);

	/* The entry model holds targets of up to 1024 bytes: one at the
	 * ceiling is imported and one byte more fails the scan explicitly.
	 * Every host stores a 1023-byte target; only Linux stores the two
	 * longer ones, since macOS and FreeBSD cap a target at 1023 bytes.
	 */
	check_long_target(1023, FSIMG_OK);
#if defined(__linux__)
	check_long_target(1024, FSIMG_OK);
	check_long_target(1025, FSIMG_EUNSUPPORTED);
#endif
}

/* Import a link whose target is `length` bytes and expect the scan's
 * status. An imported link's size is its length, and reopening reads the
 * whole target back into the source cursor's ceiling-sized buffer. The
 * target is split into 64-byte components, since a host can refuse a
 * single component longer than its name limit.
 */
static void
check_long_target(size_t length, fsimg_status_t expected)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	fsimg_error_t error;
	char target[FSIMG_SYMLINK_TARGET_MAX + 2], bytes[sizeof(target)];
	size_t count;

	CHECK(length < sizeof(target));
	memset(target, 'a', length);
	target[length] = '\0';
	for (size_t i = 64; i < length; i += 64)
		target[i] = '/';
	fixture_open(&fixture);
	CHECK(symlinkat(target, fixture.root, "long") == 0);
	bzero(&error, sizeof(error));
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/",
	    &error) == expected);
	if (expected == FSIMG_OK) {
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		entry = find_entry(fixture.writer, "/long");
		CHECK(entry->entry.kind == FSIMG_ENTRY_SYMLINK &&
		    entry->entry.size == length);
		CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes),
		    &count, NULL) == FSIMG_OK);
		CHECK(count == length && bcmp(bytes, target, length) == 0);
		CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	} else
		CHECK(strcmp(error.path, "/long") == 0);
	fixture_close(&fixture);
}

/* Reopening reads the link again and compares it with the scan: a different
 * length, a replaced link, a link replaced by a file, and a missing link all
 * fail as changed sources. A replacement is created before the rename, so it
 * cannot reuse the original inode.
 */
static void
test_symlink_changes(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		fixture_open(&fixture);
		CHECK(symlinkat("first-target", fixture.root, "link") == 0);
		CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
		CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
		entry = find_entry(fixture.writer, "/link");
		switch (scenario) {
		case 0:
			CHECK(symlinkat("longer-target", fixture.root, "replacement") == 0);
			break;
		case 1:
			CHECK(symlinkat("other-target", fixture.root, "replacement") == 0);
			break;
		case 2:
			write_file(fixture.root, "replacement", "first-target");
			break;
		case 3:
			break;
		}
		if (scenario == 3)
			CHECK(unlinkat(fixture.root, "link", 0) == 0);
		else
			CHECK(renameat(fixture.root, "replacement", fixture.root, "link") == 0);
		CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) ==
		    FSIMG_ESOURCE_CHANGED);
		CHECK(fixture.writer->state == FSIMG_WRITER_FAILED);
		fixture_close(&fixture);
	}
}

/* Owner and permission bits enter the catalog only on request, as valid
 * optional fields taken from the object the scanner opened.
 */
static void
test_posix_import(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	struct stat st;
	int fd;

	fixture_open(&fixture);
	write_file(fixture.root, "file", "mode");
	fd = openat(fixture.root, "file", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fchmod(fd, 0640) == 0);
	CHECK(fstat(fd, &st) == 0);
	CHECK(close(fd) == 0);
	CHECK(symlinkat("file", fixture.root, "link") == 0);
	fixture.writer->options.flags |= FSIMG_WRITE_IMPORT_POSIX;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);

	entry = find_entry(fixture.writer, "/file");
	CHECK((entry->entry.metadata.valid & FSIMG_META_OWNER) != 0);
	CHECK((entry->entry.metadata.valid & FSIMG_META_MODE) != 0);
	CHECK(entry->entry.metadata.required == 0);
	CHECK(entry->entry.metadata.mode == 0640);
	CHECK(entry->entry.metadata.uid == (uint32_t)st.st_uid);
	CHECK(entry->entry.metadata.gid == (uint32_t)st.st_gid);
	CHECK((entry->entry.metadata.valid & FSIMG_META_CHANGED) == 0);

	entry = find_entry(fixture.writer, "/link");
	CHECK(entry->entry.kind == FSIMG_ENTRY_SYMLINK);
	CHECK((entry->entry.metadata.valid & FSIMG_META_MODE) != 0);
	CHECK((entry->entry.metadata.mode & ~07777u) == 0);
	CHECK(entry->entry.metadata.uid == (uint32_t)st.st_uid);
	fixture_close(&fixture);
}

static void
test_limits_and_failures(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	fsimg_stats_t stats;
	fsimg_status_t status;
	bool passed;

	fixture_open(&fixture);
	fixture.ctx->options.max_open_files = 1;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DESCRIPTORS);
	fixture_close(&fixture);
	fixture_open(&fixture);
	fixture.cancelled = true;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error) == FSIMG_ECANCELLED);
	fixture.cancelled = false;
	fixture_close(&fixture);
	passed = false;
	for (uint64_t attempt = 1; attempt < 32; attempt++) {
		fixture_open(&fixture);
		write_file(fixture.root, "file", "content");
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK)
			passed = true;
		else {
			CHECK(status == FSIMG_ENOMEM);
			CHECK(fixture.writer->roots == NULL);
			CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
			CHECK(stats.descriptors == fixture.baseline.descriptors);
		}
		fixture_close(&fixture);
		if (passed)
			break;
	}
	CHECK(passed);
}

static void
test_corrupt_source(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	unsigned char bytes[8];
	uint64_t cursor, value;
	size_t length;

	fixture_open(&fixture);
	write_file(fixture.root, "file", "content");
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	entry = find_entry(fixture.writer, "/file");
	cursor = entry->source_id;
	CHECK(fsimg_stream_next(fixture.writer->host_sources, &cursor,
	    fixture.writer->record, fixture.writer->record_capacity, &length, NULL) == FSIMG_OK);
	value = fsimg_load_le64(fixture.writer->record + 40);
	fsimg_store_le64(bytes, (UINT64_C(1) << 32) | value);
	CHECK(fsimg_store_write(fixture.writer->host_sources->store, 8 + 40,
	    bytes, sizeof(bytes), NULL) == FSIMG_OK);
	entry = find_entry(fixture.writer, "/file");
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

static void
test_ancestor_pivot(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char bytes[16];
	int child, outside;

	fixture_open(&fixture);
	CHECK(mkdirat(fixture.root, "dir", 0700) == 0);
	child = openat(fixture.root, "dir", O_RDONLY | O_DIRECTORY);
	CHECK(child != -1);
	write_file(child, "file", "source");
	CHECK(close(child) == 0);
	CHECK(mkdirat(fixture.parent, "outside", 0700) == 0);
	outside = openat(fixture.parent, "outside", O_RDONLY | O_DIRECTORY);
	CHECK(outside != -1);
	write_file(outside, "file", "sentinel");
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(renameat(fixture.root, "dir", fixture.root, "saved") == 0);
	CHECK(symlinkat("../outside", fixture.root, "dir") == 0);
	entry = find_entry(fixture.writer, "/dir/file");
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_ESOURCE_CHANGED);
	child = openat(outside, "file", O_RDONLY | O_NOFOLLOW);
	CHECK(child != -1);
	CHECK(read(child, bytes, sizeof(bytes)) == 8);
	CHECK(bcmp(bytes, "sentinel", 8) == 0);
	CHECK(close(child) == 0);
	CHECK(close(outside) == 0);
	fixture_close(&fixture);
}

static void
test_depth_limit(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	int parent, child;

	fixture_open(&fixture);
	fixture.ctx->options.max_depth = 3;
	parent = dup(fixture.root);
	CHECK(parent != -1);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(mkdirat(parent, "d", 0700) == 0);
		child = openat(parent, "d", O_RDONLY | O_DIRECTORY);
		CHECK(child != -1);
		CHECK(close(parent) == 0);
		parent = child;
	}
	CHECK(close(parent) == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DEPTH && fixture.writer->roots == NULL);
	fixture_close(&fixture);
}

static void
test_provider_work_limit(void)
{
	source_fixture_t fixture;
	fsimg_error_t error;
	unsigned char bytes[16];
	size_t count;

	fixture_open(&fixture);
	configure_provider(&fixture, sizeof(bytes));
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	/* One short callback exhausts the cumulative allowance; reads are not
	 * validated, so they charge nothing else.
	 */
	fixture.ctx->options.max_work = fixture.writer->session.progress.work + 1;
	CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK && count == 2 && fixture.calls == 1);
	fixture.ctx->options.max_work = 0;
	CHECK(fixture.writer->roots == NULL && !fixture.writer->source.active);
	fixture_close(&fixture);
	fixture_open(&fixture);
	configure_provider(&fixture, sizeof(bytes));
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	fixture.script = PROVIDER_REPEATED_INTERRUPTION;
	fixture.ctx->options.max_work = fixture.writer->session.progress.work + 3;
	CHECK(fsimg_source_read(fixture.writer, 0, bytes, sizeof(bytes), &count, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK && count == 0 && fixture.calls == 3);
	fixture.ctx->options.max_work = 0;
	fixture_close(&fixture);
	fixture_open(&fixture);
	configure_provider(&fixture, 0);
	CHECK(fsimg_source_open(fixture.writer, fixture.provider_entry.id, NULL) == FSIMG_OK);
	fixture.changed = true;
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_ESOURCE_CHANGED);
	CHECK(fixture.calls == 0 && fixture.validations == 2);
	fixture_close(&fixture);
}

static void
test_directory_records(void)
{
	unsigned char bytes[64];
	const char *name;
	size_t length, cursor, name_length;
#if defined(__APPLE__)
	uint32_t word;
#elif defined(__linux__)
	uint64_t inode;
	uint16_t reclen;
#else
	struct dirent entry;
#endif

	bzero(bytes, sizeof(bytes));
#if defined(__APPLE__)
	/* getattrlistbulk: length, returned attributes, then name reference.
	 * The name offset is relative to the reference at byte 24.
	 */
	length = 36;
	word = (uint32_t)length;
	bcopy(&word, bytes, sizeof(word));
	word = 1;
	bcopy(&word, bytes + 4, sizeof(word));
	word = 8;
	bcopy(&word, bytes + 24, sizeof(word));
	word = 2;
	bcopy(&word, bytes + 28, sizeof(word));
	bytes[32] = 'a';
#elif defined(__linux__)
	length = 24;
	inode = 1;
	reclen = (uint16_t)length;
	bcopy(&inode, bytes, sizeof(inode));
	bcopy(&reclen, bytes + 16, sizeof(reclen));
	bytes[19] = 'a';
#else
	bzero(&entry, sizeof(entry));
	length = offsetof(struct dirent, d_name) + 2;
	entry.d_fileno = 1;
	entry.d_reclen = (uint16_t)length;
	entry.d_namlen = 1;
	bcopy(&entry, bytes, offsetof(struct dirent, d_name));
	bytes[offsetof(struct dirent, d_name)] = 'a';
#endif
	cursor = 0;
	CHECK(fsimg_host_directory_name(bytes, length, &cursor, &name, &name_length) == 1);
	CHECK(name_length == 1 && name[0] == 'a' && cursor == length);
	CHECK(fsimg_host_directory_name(bytes, length, &cursor, &name, &name_length) == 0);
#if defined(__linux__) || defined(__FreeBSD__)
	/* A native unused slot has valid framing but no live name. It must be
	 * skipped before applying live-entry name validation.
	 */
	bcopy(bytes, bytes + length, length);
#if defined(__linux__)
	inode = 0;
	bcopy(&inode, bytes, sizeof(inode));
	bytes[19] = '\0';
#else
	entry.d_fileno = 0;
	entry.d_namlen = 0;
	bcopy(&entry, bytes, offsetof(struct dirent, d_name));
	bytes[offsetof(struct dirent, d_name)] = '\0';
#endif
	cursor = 0;
	CHECK(fsimg_host_directory_name(bytes, length * 2, &cursor, &name, &name_length) == 1);
	CHECK(name_length == 1 && name[0] == 'a' && cursor == length * 2);
	cursor = 0;
	CHECK(fsimg_host_directory_name(bytes, length, &cursor, &name, &name_length) == 0);
	CHECK(cursor == length);
	bcopy(bytes + length, bytes, length);
#endif
	cursor = 0;
	CHECK(fsimg_host_directory_name(bytes, length - 1, &cursor, &name, &name_length) == -1);
	CHECK(cursor == 0);
	bzero(bytes, sizeof(bytes));
	CHECK(fsimg_host_directory_name(bytes, sizeof(bytes), &cursor, &name, &name_length) == -1);
	CHECK(cursor == 0);
}

static void
test_snapshot_lifetime(void)
{
	source_fixture_t fixture;
	const fsimg_catalog_entry_t *entry;
	char name[201], path[202];

	fixture_open(&fixture);
	memset(name, 'x', 200);
	name[200] = '\0';
	snprintf(path, sizeof(path), "/%s", name);
	write_file(fixture.root, name, "content");
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	entry = find_entry(fixture.writer, path);
	CHECK(fsimg_source_open(fixture.writer, entry->id, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, path) == 0);
	CHECK(fsimg_source_finish(fixture.writer, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, path) == 0);
	fixture_close(&fixture);
}

static void
test_source_reference_eof(void)
{
	source_fixture_t fixture;
	unsigned char bytes[8];

	fixture_open(&fixture);
	write_file(fixture.root, "file", "content");
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	fsimg_store_le64(bytes, fsimg_store_size(fixture.writer->host_sources->store));
	/* The only entry is the first frame; its source reference is at byte 16. */
	CHECK(fsimg_store_write(fixture.writer->ordered->store, 8 + 16,
	    bytes, sizeof(bytes), NULL) == FSIMG_OK);
	CHECK(fsimg_source_open(fixture.writer, 1, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

static void
test_directory_interruption(void)
{
	source_fixture_t fixture;

	fixture_open(&fixture);
	write_file(fixture.root, "file", "content");
	/* Interrupt the refill after an entire nonempty batch was consumed. */
	fixture.writer->interrupt_directory_read_after = 2;
	CHECK(fsimg_writer_add_tree(fixture.writer, fixture.root, "/", NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	CHECK(fixture.writer->count == 1);
	fixture_close(&fixture);
}

static void
test_unregistered_provider(void)
{
	source_fixture_t fixture;
	fsimg_entry_t entry = { .path = "/zero", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	unsigned char bytes[8];

	fixture_open(&fixture);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_prepare(fixture.writer, NULL, NULL) == FSIMG_OK);
	fsimg_store_le64(bytes, FSIMG_STORAGE_PROVIDER);
	/* Corrupt a ZERO record into an unregistered provider reference. */
	CHECK(fsimg_store_write(fixture.writer->ordered->store, 8 + 8,
	    bytes, sizeof(bytes), NULL) == FSIMG_OK);
	CHECK(fsimg_source_open(fixture.writer, 1, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2) {
		if (strcmp(argv[1], "--active-reopen") == 0)
			test_active_reopen();
		else if (strcmp(argv[1], "--free-active") == 0)
			test_free_active();
		else if (strcmp(argv[1], "--failed-writer") == 0)
			test_failed_writer();
		else if (strcmp(argv[1], "--reentry") == 0)
			test_reentry();
		else if (strcmp(argv[1], "--corrupt-source") == 0)
			test_corrupt_source();
		else if (strcmp(argv[1], "--snapshot-lifetime") == 0)
			test_snapshot_lifetime();
		else if (strcmp(argv[1], "--reference-eof") == 0)
			test_source_reference_eof();
		else if (strcmp(argv[1], "--directory-interruption") == 0)
			test_directory_interruption();
		else if (strcmp(argv[1], "--unregistered-provider") == 0)
			test_unregistered_provider();
		else if (strcmp(argv[1], "--directory-records") == 0)
			test_directory_records();
		else
			CHECK(false);
		return (0);
	}
	CHECK(argc == 1);
	test_active_reopen();
	test_free_active();
	test_failed_writer();
	test_reentry();
	test_provider();
	test_host_tree();
	test_mutation();
	test_rejection();
	test_merged_roots();
	test_scanned_rank();
	test_repeated_tree();
	test_root_collisions();
	test_file_root();
	test_file_root_changes();
	test_file_root_rejection();
	test_symlink_import();
	test_symlink_changes();
	test_posix_import();
	test_limits_and_failures();
	test_corrupt_source();
	test_ancestor_pivot();
	test_depth_limit();
	test_provider_work_limit();
	test_snapshot_lifetime();
	test_source_reference_eof();
	test_directory_records();
	test_directory_interruption();
	test_unregistered_provider();
	return (0);
}
