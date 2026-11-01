/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "emit.h"
#include "format.h"
#include "test.h"
#include "selection.h"

/* Test drivers exercise content and traversal contracts independently of
 * any on-disk format. A tiny FAT volume supplies the public reader's input
 * and lifetime; its driver is closed before installing the test driver.
 */
#define VOLUME_BYTES (1024u * 1024u)
#define DECODED_TARGET "dir/target"

typedef struct memory_image {
	unsigned char	*bytes;
	uint64_t	length;
	unsigned	reads;
} memory_image_t;

typedef enum link_corruption {
	LINK_UNCHANGED,
	LINK_SIZE_CHANGED,
	LINK_INITIALIZED_CHANGED,
	LINK_KIND_CHANGED
} link_corruption_t;

typedef struct reader_fixture {
	fsimg_ctx_t		*ctx;
	fsimg_io_t		*io;
	fsimg_reader_t		*reader;
	memory_image_t		image;
	link_corruption_t	corruption;
	bool			trees;
	bool			cancel_delivery;
	bool			cancelled;
	bool			fail_link_allocation;
	int			changed_host;
	bool			duplicate_second;
	bool			deep_second;
	unsigned		tree;
	unsigned		roots[2];
	unsigned		second_descents;
	unsigned		resolves;
	uint64_t		first_work;
	uint64_t		first_entries;
	uint64_t		second_work;
} reader_fixture_t;

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_image_t *image = argument;

	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(image->bytes + offset, buffer, length);
	*count = length;
	image->reads++;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_image_t *image = argument;

	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(buffer, image->bytes + offset, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_image_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	CHECK(length <= VOLUME_BYTES);
	((memory_image_t *)argument)->length = length;
	return (FSIMG_OK);
}

static void
open_memory(fsimg_ctx_t *ctx, memory_image_t *image, fsimg_io_t **io)
{
	fsimg_io_ops_t ops;

	bzero(image, sizeof(*image));
	image->bytes = calloc(1, VOLUME_BYTES);
	CHECK(image->bytes != NULL);
	image->length = VOLUME_BYTES;
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, image, io, NULL) == FSIMG_OK);
}

static void
reader_close(fsimg_reader_t *reader FSIMG_UNUSED)
{
}

static fsimg_status_t
reader_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    unsigned char *key, size_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = strlen(name);
	bcopy(name, key, *length);
	return (FSIMG_OK);
}

static fsimg_status_t
reader_keys(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	keys->count = 1;
	return (reader_key(reader, name, keys->key[0], &keys->length[0], error));
}

/* Tree 0's omitted file is counted. Tree 1's omitted directory and its
 * children are checked without contributing to omissions or public counts.
 */
static fsimg_status_t
reader_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	typedef struct fixture_entry {
		const char *name;
		unsigned char kind;
	} fixture_entry_t;
	static const fixture_entry_t single[] = {
		{ "a", FSIMG_ENTRY_SYMLINK }, { "b", FSIMG_ENTRY_FILE }
	}, first[] = {
		{ "a", FSIMG_ENTRY_FILE }, { "\xff", FSIMG_ENTRY_FILE }
	}, second[] = {
		{ "a", FSIMG_ENTRY_FILE }, { "\xff", FSIMG_ENTRY_DIRECTORY }
	}, duplicate[] = {
		{ "a", FSIMG_ENTRY_FILE }, { "a", FSIMG_ENTRY_FILE }
	}, child[] = {
		{ "inside", FSIMG_ENTRY_FILE }
	}, deep_child[] = {
		{ "inside", FSIMG_ENTRY_DIRECTORY }
	};
	reader_fixture_t *f = reader->driver;
	const fixture_entry_t *entries;
	fsimg_status_t status;
	size_t count;
	uint32_t level = fsimg_directory_level(build);

	if (level != 0) {
		CHECK(f->trees && f->tree == 1);
		f->second_descents++;
		entries = child;
		if (f->deep_second && level == 1)
			entries = deep_child;
		count = 1;
	} else {
		f->roots[f->tree]++;
		entries = single;
		if (f->trees) {
			if (f->tree == 0)
				entries = first;
			else if (f->duplicate_second)
				entries = duplicate;
			else
				entries = second;
		}
		count = 2;
	}
	for (size_t i = 0; i < count; i++) {
		status = fsimg_directory_admit(build, entries[i].name,
		    &entries[i].kind, 1, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
reader_replay(fsimg_reader_t *reader, const char *name FSIMG_UNUSED,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	reader_fixture_t *f = reader->driver;
	fsimg_status_t status;

	CHECK(length == 1);
	reader->entry.kind = payload[0];
	if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
		return (FSIMG_OK);
	if (reader->entry.kind == FSIMG_ENTRY_SYMLINK) {
		if (f->fail_link_allocation)
			fsimg_test_fail_alloc_after(f->ctx, 1);
		status = fsimg_reader_set_link_target(reader, DECODED_TARGET,
		    sizeof(DECODED_TARGET) - 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (f->corruption == LINK_SIZE_CHANGED)
			reader->entry.size++;
		if (f->corruption == LINK_INITIALIZED_CHANGED)
			reader->initialized--;
		if (f->corruption == LINK_KIND_CHANGED)
			reader->entry.kind = FSIMG_ENTRY_FILE;
		return (FSIMG_OK);
	}
	reader->entry.size = reader->initialized = 3;
	return (FSIMG_OK);
}

static fsimg_status_t
reader_resolve(fsimg_reader_t *reader, uint64_t offset FSIMG_UNUSED,
    fsimg_extent_t *extent, fsimg_error_t *error FSIMG_UNUSED)
{
	reader_fixture_t *f = reader->driver;

	/* Resolving a decoded link is a contract violation, even if its
	 * backing bytes happened to match the declared target.
	 */
	CHECK(reader->link_length == 0);
	CHECK(reader->entry.kind == FSIMG_ENTRY_FILE);
	f->resolves++;
	*extent = (fsimg_extent_t){ .image_offset = 0, .length = 3 };
	return (FSIMG_OK);
}

static fsimg_status_t
reader_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	reader_fixture_t *f = reader->driver;
	fsimg_status_t status;
	unsigned char bytes[64];

	/* Inject only after replay has established the decoded snapshot. This
	 * bypasses the outer read_at guards and exercises delivery's own checks.
	 */
	if (f->changed_host != -1 || f->cancel_delivery) {
		CHECK((flags & FSIMG_VERIFY_PAYLOAD) != 0);
		status = fsimg_walk_next(reader, error);
		if (status != FSIMG_OK)
			return (status);
		CHECK(reader->link_target != NULL && reader->link_length == 10);
		CHECK(reader->entry.path != NULL);
		CHECK(fsimg_reader_stable(reader, NULL) == FSIMG_OK);
		if (f->changed_host != -1)
			CHECK(ftruncate(f->changed_host, VOLUME_BYTES + 1) == 0);
		f->cancelled = f->cancel_delivery;
		memset(bytes, 0xa5, sizeof(bytes));
		status = fsimg_verify_payload(reader, bytes, sizeof(bytes), report,
		    error);
		for (size_t i = 0; i < sizeof(bytes); i++)
			CHECK(bytes[i] == 0xa5);
		return (status);
	}

	/* The walker yields children; account for the root separately. */
	report->directories = 1;
	CHECK(fsimg_walk_restart(reader, NULL) == FSIMG_ESTATE);
	for (f->tree = 0; f->tree < 2; f->tree++) {
		while ((status = fsimg_walk_next(reader, error)) == FSIMG_OK) {
			status = fsimg_verify_account(reader, report, error);
			if (status != FSIMG_OK)
				return (status);
			if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
				status = fsimg_verify_payload(reader, bytes, sizeof(bytes),
				    report, error);
				if (status != FSIMG_OK)
					return (status);
			}
		}
		if (status != FSIMG_DONE)
			return (status);
		if (f->tree == 0) {
			f->first_work = reader->session.progress.work;
			f->first_entries = reader->session.progress.entries;
			status = fsimg_walk_restart(reader, error);
			if (status != FSIMG_OK)
				return (status);
			CHECK(reader->session.progress.work == f->first_work);
			CHECK(reader->session.progress.entries == f->first_entries);
		}
	}
	f->second_work = reader->session.progress.work;
	return (FSIMG_OK);
}

static const fsimg_reader_ops_t reader_ops = {
	.close = reader_close, .prepare = reader_prepare, .replay = reader_replay,
	.keys = reader_keys, .key = reader_key, .ambiguous = "duplicate test name",
	.name_profile = FSIMG_NAME_PROFILE_PORTABLE,
	.resolve = reader_resolve, .verify = reader_verify
};

static bool
reader_cancelled(void *argument)
{
	return (((reader_fixture_t *)argument)->cancelled);
}

static void
reader_fixture_open(reader_fixture_t *f)
{
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_options_t limits;

	bzero(f, sizeof(*f));
	f->changed_host = -1;
	fsimg_options_init(&limits);
	limits.cancelled = reader_cancelled;
	limits.cancel_argument = f;
	limits.memory_bytes = 32u * 1024u * 1024u;
	CHECK(fsimg_ctx_new(&limits, &f->ctx, NULL) == FSIMG_OK);
	open_memory(f->ctx, &f->image, &f->io);
	fsimg_write_options_init(&options);
	options.volume_bytes = VOLUME_BYTES;
	CHECK(fsimg_writer_new(f->ctx, "fat12", &options, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, f->io, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL) == FSIMG_OK);
	f->reader->ops->close(f->reader);
	f->reader->driver = f;
	f->reader->ops = &reader_ops;
	f->reader->content_domain = (fsimg_extent_domain_t){
		.length = VOLUME_BYTES, .unit_bytes = 1 };
}

static void
reader_fixture_close(reader_fixture_t *f)
{
	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	free(f->image.bytes);
	CHECK(f->ctx->stats.memory_bytes == sizeof(*f->ctx));
	fsimg_ctx_free(f->ctx);
}

/* Replace the callback input with a real file before opening the reader,
 * so the public open captures the identity used by source-stability checks.
 */
static int
reader_fixture_use_host(reader_fixture_t *f, const char *directory)
{
	char path[128];
	int fd;

	CHECK(snprintf(path, sizeof(path), "%s/image", directory) <
	    (int)sizeof(path));
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, f->image.bytes, VOLUME_BYTES) == VOLUME_BYTES);
	CHECK(unlink(path) == 0);
	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_io_from_fd(f->ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE,
	    &f->io, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL) == FSIMG_OK);
	CHECK(f->reader->host_identity);
	f->reader->ops->close(f->reader);
	f->reader->driver = f;
	f->reader->ops = &reader_ops;
	return (fd);
}

static void
check_reader_failed_without_snapshot(reader_fixture_t *f)
{
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	unsigned char byte;
	size_t count;

	CHECK(f->reader->state == FSIMG_READER_FAILED);
	CHECK(f->reader->entry.path == NULL && f->reader->entry.kind == 0);
	CHECK(f->reader->entry.size == 0);
	CHECK(f->reader->link_target == NULL && f->reader->link_length == 0);
	CHECK(fsimg_reader_next(f->reader, &entry, &error) == FSIMG_ESTATE);
	CHECK(strcmp(error.message, "reader has failed") == 0);
	CHECK(fsimg_reader_read_at(f->reader, 0, &byte, 1, &count, &error) ==
	    FSIMG_ESTATE);
	CHECK(count == 0 && strcmp(error.message, "reader has failed") == 0);
}

static void
test_decoded_link_host_change_during_verification_is_terminal(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	char directory[] = "common-reader-XXXXXX";
	int fd;

	CHECK(mkdtemp(directory) != NULL);
	reader_fixture_open(&f);
	fd = reader_fixture_use_host(&f, directory);
	f.changed_host = fd;
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, &error) ==
	    FSIMG_ESOURCE_CHANGED);
	CHECK(error.operation == FSIMG_OP_READER);
	CHECK(!report.complete && report.payload_bytes_read == 0);
	check_reader_failed_without_snapshot(&f);
	reader_fixture_close(&f);
	CHECK(close(fd) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_decoded_link_cancellation_during_verification_is_terminal(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;

	reader_fixture_open(&f);
	f.cancel_delivery = true;
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_ECANCELLED);
	CHECK(!report.complete && report.payload_bytes_read == 0);
	f.cancelled = false;
	check_reader_failed_without_snapshot(&f);
	reader_fixture_close(&f);
}

static void
test_decoded_link_allocation_failure_during_advance_is_terminal(void)
{
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error;

	reader_fixture_open(&f);
	f.fail_link_allocation = true;
	CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ENOMEM);
	CHECK(entry == NULL);
	CHECK(f.ctx->allocation_attempts == 1);
	CHECK(error.status == FSIMG_ENOMEM);
	fsimg_test_fail_alloc_after(f.ctx, 0);
	check_reader_failed_without_snapshot(&f);
	reader_fixture_close(&f);
}

static void
test_decoded_link_reads_ranges_and_verification(void)
{
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	fsimg_verify_report_t report = { .payload_bytes_read = 17 };
	unsigned char bytes[64];
	size_t count;
	unsigned reads;

	reader_fixture_open(&f);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_SYMLINK && entry->size == 10);
	reads = f.image.reads;
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes), &count,
	    NULL) == FSIMG_OK);
	CHECK(count == 10 && bcmp(bytes, DECODED_TARGET, 10) == 0);
	CHECK(fsimg_reader_read_at(f.reader, 4, bytes, 3, &count, NULL) == FSIMG_OK);
	CHECK(count == 3 && bcmp(bytes, "tar", 3) == 0);
	CHECK(fsimg_reader_read_at(f.reader, 9, bytes, 4, &count, NULL) == FSIMG_OK);
	CHECK(count == 1 && bytes[0] == 't');
	CHECK(fsimg_reader_read_at(f.reader, 10, bytes, 4, &count, NULL) == FSIMG_OK);
	CHECK(count == 0);
	CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_OK);
	CHECK(range.kind == FSIMG_RANGE_DATA && range.offset == 0 &&
	    range.length == 10);
	CHECK(fsimg_reader_next_range(f.reader, 10, &range, NULL) == FSIMG_DONE);
	memset(bytes, 0xa5, sizeof(bytes));
	CHECK(fsimg_verify_payload(f.reader, bytes, sizeof(bytes), &report,
	    NULL) == FSIMG_OK);
	CHECK(bcmp(bytes, DECODED_TARGET, 10) == 0);
	CHECK(report.payload_bytes_read == 17);
	CHECK(f.resolves == 0 && f.image.reads == reads);

	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_FILE && f.reader->link_length == 0);
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes), &count,
	    NULL) == FSIMG_OK);
	CHECK(count == 3 && bcmp(bytes, f.image.bytes, 3) == 0);
	CHECK(f.resolves == 1);
	reader_fixture_close(&f);
}

/* Stored counts image bytes only, even when one delivery spans both the
 * initialized prefix and the synthesized zero tail.
 */
static void
test_content_reports_stored_bytes_separately(void)
{
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char bytes[16];
	size_t count, stored;

	reader_fixture_open(&f);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_content(f.reader, 0, bytes, 10, &count, &stored,
	    NULL) == FSIMG_OK);
	CHECK(count == 10 && stored == 0);
	CHECK(bcmp(bytes, DECODED_TARGET, 10) == 0);

	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	f.reader->entry.size = 7;
	CHECK(f.reader->initialized == 3);
	CHECK(fsimg_reader_content(f.reader, 1, bytes, 2, &count, &stored,
	    NULL) == FSIMG_OK);
	CHECK(count == 2 && stored == count);
	CHECK(bcmp(bytes, f.image.bytes + 1, 2) == 0);
	memset(bytes, 0xa5, sizeof(bytes));
	CHECK(fsimg_reader_content(f.reader, 3, bytes, 4, &count, &stored,
	    NULL) == FSIMG_OK);
	CHECK(count == 4 && stored == 0);
	for (size_t i = 0; i < count; i++)
		CHECK(bytes[i] == 0);
	CHECK(fsimg_reader_content(f.reader, 1, bytes, 6, &count, &stored,
	    NULL) == FSIMG_OK);
	CHECK(count == 6 && stored == 2);
	CHECK(bcmp(bytes, f.image.bytes + 1, 2) == 0);
	for (size_t i = 2; i < count; i++)
		CHECK(bytes[i] == 0);
	reader_fixture_close(&f);
}

static void
test_decoded_link_declared_inconsistently_during_replay_fails_at_advance(void)
{
	static const struct {
		const char *name;
		link_corruption_t corruption;
	} cases[] = {
		{ "size changed", LINK_SIZE_CHANGED },
		{ "initialized length changed", LINK_INITIALIZED_CHANGED },
		{ "kind changed", LINK_KIND_CHANGED }
	};
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned reads;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reader_fixture_open(&f);
		f.corruption = cases[i].corruption;
		reads = f.image.reads;
		status = fsimg_reader_next(f.reader, &entry, &error);
		if (status != FSIMG_ESTATE)
			fprintf(stderr, "%s: expected advance ESTATE, got %d\n",
			    cases[i].name, status);
		CHECK(status == FSIMG_ESTATE);
		CHECK(error.operation == FSIMG_OP_READER);
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		CHECK(f.reader->link_target == NULL && f.reader->link_length == 0);
		CHECK(f.resolves == 0 && f.image.reads == reads);
		CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ESTATE);
		CHECK(strcmp(error.message, "reader has failed") == 0);
		reader_fixture_close(&f);
	}
}

static void
test_decoded_link_payload_verification_charges_work(void)
{
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report = { 0 };
	fsimg_error_t error;
	unsigned char bytes[64];

	reader_fixture_open(&f);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	f.ctx->options.max_work = f.reader->session.progress.work;
	CHECK(fsimg_verify_payload(f.reader, bytes, sizeof(bytes), &report,
	    &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	CHECK(report.payload_bytes_read == 0);
	reader_fixture_close(&f);
}

static void
test_decoded_link_declaration_bounds_and_single_allocation(void)
{
	reader_fixture_t f;
	unsigned char bytes[1025], *allocation;
	uint64_t attempts, memory;

	reader_fixture_open(&f);
	memset(bytes, 'x', sizeof(bytes));
	f.reader->entry.kind = FSIMG_ENTRY_SYMLINK;
	CHECK(fsimg_reader_set_link_target(f.reader, bytes, 0, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_reader_set_link_target(f.reader, bytes, 1025, NULL) ==
	    FSIMG_ESTATE);
	CHECK(f.reader->link_target == NULL);
	f.reader->entry.kind = FSIMG_ENTRY_FILE;
	CHECK(fsimg_reader_set_link_target(f.reader, bytes, 1, NULL) == FSIMG_ESTATE);
	f.reader->entry.kind = FSIMG_ENTRY_SYMLINK;
	attempts = f.ctx->allocation_attempts;
	memory = f.ctx->stats.memory_bytes;
	CHECK(fsimg_reader_set_link_target(f.reader, bytes, 1024, NULL) == FSIMG_OK);
	allocation = f.reader->link_target;
	CHECK(f.ctx->allocation_attempts == attempts + 1);
	CHECK(f.ctx->stats.memory_bytes >= memory + 1024);
	memory = f.ctx->stats.memory_bytes;
	CHECK(f.reader->entry.size == 1024 && f.reader->initialized == 1024);
	bytes[0] = 'y';
	CHECK(allocation[0] == 'x');
	CHECK(fsimg_reader_set_link_target(f.reader, bytes, 1, NULL) == FSIMG_OK);
	CHECK(f.reader->link_target == allocation && allocation[0] == 'y');
	CHECK(f.ctx->allocation_attempts == attempts + 1);
	CHECK(f.ctx->stats.memory_bytes == memory);
	reader_fixture_close(&f);
}

static void
test_decoded_link_buffer_obeys_memory_budget(void)
{
	reader_fixture_t f;
	fsimg_error_t error;
	unsigned char target[1024];
	uint64_t budget;

	reader_fixture_open(&f);
	memset(target, 'x', sizeof(target));
	f.reader->entry.kind = FSIMG_ENTRY_SYMLINK;
	budget = f.ctx->options.memory_bytes;
	f.ctx->options.memory_bytes = f.ctx->stats.memory_bytes + 1023;
	CHECK(fsimg_reader_set_link_target(f.reader, target, sizeof(target),
	    &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(f.reader->link_target == NULL && f.reader->link_length == 0);
	f.ctx->options.memory_bytes = budget;
	CHECK(fsimg_reader_set_link_target(f.reader, target, sizeof(target),
	    NULL) == FSIMG_OK);
	reader_fixture_close(&f);
}

static void
test_decoded_link_mutated_after_replay_fails_at_delivery(void)
{
	static const struct {
		const char *name;
		link_corruption_t corruption;
	} cases[] = {
		{ "size changed", LINK_SIZE_CHANGED },
		{ "kind changed", LINK_KIND_CHANGED }
	};
	reader_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	unsigned char bytes[16];
	size_t count;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reader_fixture_open(&f);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		/* Change the declared snapshot after replay, so this checks the
		 * delivery guard independently of any walker replay guard.
		 */
		switch (cases[i].corruption) {
		case LINK_SIZE_CHANGED:
			f.reader->entry.size++;
			break;
		case LINK_INITIALIZED_CHANGED:
			f.reader->initialized--;
			break;
		case LINK_KIND_CHANGED:
			f.reader->entry.kind = FSIMG_ENTRY_FILE;
			break;
		case LINK_UNCHANGED:
			CHECK(!"a corruption case is required");
		}
		status = fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes),
		    &count, &error);
		if (status != FSIMG_ESTATE)
			fprintf(stderr, "%s: expected terminal ESTATE, got %d\n",
			    cases[i].name, status);
		CHECK(status == FSIMG_ESTATE);
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		CHECK(f.reader->link_target == NULL && f.reader->link_length == 0);
		CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ESTATE);
		CHECK(strcmp(error.message, "reader has failed") == 0);
		CHECK(fsimg_reader_read_at(f.reader, 0, bytes, 1, &count,
		    &error) == FSIMG_ESTATE);
		CHECK(strcmp(error.message, "reader has failed") == 0);
		reader_fixture_close(&f);
	}
}

/* Model the FILE selection plus read_at path used by the cat job. */
static void
test_decoded_link_cat_selection_delivers_target(void)
{
	reader_fixture_t f;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	const char *const members[] = { "/a" };
	unsigned char bytes[64];
	size_t length;

	reader_fixture_open(&f);
	CHECK(fsimg_selection_new(f.reader, members, 1, FSIMG_SELECTION_FILE,
	    &selection, NULL) == FSIMG_OK);
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_SYMLINK && entry->size == 10);
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes), &length,
	    NULL) == FSIMG_OK);
	CHECK(length == 10 && bcmp(bytes, DECODED_TARGET, 10) == 0);
	CHECK(f.resolves == 0);
	fsimg_selection_free(selection);
	reader_fixture_close(&f);
}

static void
test_second_walk_checks_omitted_subtrees_without_accounting(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;

	reader_fixture_open(&f);
	f.trees = true;
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.files == 1 && report.directories == 1);
	CHECK(report.logical_bytes == 3 && report.omitted_entries == 1);
	CHECK(f.ctx->stats.omitted_entries == 1);
	CHECK(f.roots[0] == 1 && f.roots[1] == 1 && f.second_descents == 1);
	CHECK(f.second_work > f.first_work);
	CHECK(f.reader->session.progress.entries > f.first_entries);
	reader_fixture_close(&f);
}

static void
test_second_walk_payload_reads_count_stored_bytes(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;
	unsigned reads;

	reader_fixture_open(&f);
	f.trees = true;
	reads = f.image.reads;
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK);
	/* Each tree reads two stored files, including omitted entries. */
	CHECK(report.complete && report.files == 1 && report.directories == 1);
	CHECK(report.logical_bytes == 3 && report.omitted_entries == 1);
	CHECK(report.payload_bytes_read == 12);
	CHECK(f.resolves == 4 && f.image.reads == reads + 4);
	CHECK(f.roots[0] == 1 && f.roots[1] == 1 && f.second_descents == 1);
	reader_fixture_close(&f);
}

static void
test_second_walk_rejects_duplicate_namespace(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;

	reader_fixture_open(&f);
	f.trees = f.duplicate_second = true;
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(!report.complete && f.roots[0] == 1 && f.roots[1] == 1);
	CHECK(strstr(error.message, "duplicate test name") != NULL);
	reader_fixture_close(&f);
}

static void
test_second_walk_obeys_depth_and_work_limits(void)
{
	reader_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	uint64_t first_work;

	reader_fixture_open(&f);
	f.trees = f.deep_second = true;
	f.ctx->options.max_depth = 1;
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DEPTH && f.roots[1] == 1);
	reader_fixture_close(&f);

	reader_fixture_open(&f);
	f.trees = true;
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	first_work = f.first_work;
	reader_fixture_close(&f);
	reader_fixture_open(&f);
	f.trees = true;
	f.ctx->options.max_work = first_work;
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK && f.roots[0] == 1);
	CHECK(f.first_work == first_work);
	reader_fixture_close(&f);
}

static void
test_restart_requires_full_verification_and_finished_walk(void)
{
	reader_fixture_t f;
	const fsimg_entry_t *entry;

	reader_fixture_open(&f);
	CHECK(fsimg_walk_restart(f.reader, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_walk_restart(f.reader, NULL) == FSIMG_ESTATE);
	f.reader->omission.descend = true;
	CHECK(fsimg_walk_restart(f.reader, NULL) == FSIMG_ESTATE);
	f.reader->omission.descend = false;
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	CHECK(fsimg_walk_restart(f.reader, NULL) == FSIMG_ESTATE);
	reader_fixture_close(&f);
}

static void
test_reader_view_rejected_before_driver_open_or_image_reads(void)
{
	reader_fixture_t f;
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	fsimg_error_t error;
	unsigned reads;

	reader_fixture_open(&f);
	for (unsigned forced = 0; forced < 2; forced++) {
		fsimg_read_options_init(&options);
		options.format = forced ? "fat12" : NULL;
		options.iso_view = FSIMG_ISO_VIEW_JOLIET;
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &reader, &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "view") == 0);
		CHECK(strstr(error.message, "read setting does not apply") != NULL);
	}
	fsimg_read_options_init(&options);
	options.format = "fat12";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &reader, NULL) == FSIMG_OK);
	options.iso_view = FSIMG_ISO_VIEW_PRIMARY;
	CHECK(reader->options.iso_view == FSIMG_ISO_VIEW_AUTO);
	CHECK(reader->options.format == NULL);
	fsimg_reader_free(reader);
	f.image.length = 0;
	options.format = NULL;
	reads = f.image.reads;
	options.iso_view = (fsimg_iso_view_t)9;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &reader, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "view") == 0 && f.image.reads == reads);
	reader_fixture_close(&f);
}

/* The BPB still probes as FAT12; corrupt only a FAT reserved entry,
 * which the driver's open checks. AUTO therefore reaches the corrupt
 * table, while JOLIET must be refused before that read, forced or detected.
 */
static void
test_view_applicability_precedes_driver_open(void)
{
	reader_fixture_t f;
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	fsimg_error_t error;
	size_t table;

	reader_fixture_open(&f);
	/* FAT BPB: bytes per sector at 11, reserved sector count at 14. */
	table = (f.image.bytes[11] + 256u * f.image.bytes[12]) *
	    (f.image.bytes[14] + 256u * f.image.bytes[15]);
	CHECK(table < f.image.length && f.image.bytes[table] != 0);
	f.image.bytes[table] = 0;
	for (unsigned forced = 0; forced < 2; forced++) {
		fsimg_read_options_init(&options);
		options.format = forced ? "fat12" : NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &reader, &error) ==
		    FSIMG_ECORRUPT);
		options.iso_view = FSIMG_ISO_VIEW_JOLIET;
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &reader, &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "view") == 0);
		CHECK(strstr(error.message, "read setting does not apply") != NULL);
	}
	reader_fixture_close(&f);
}

/* An unregistered writer reads links in both callbacks. Its image has no
 * filesystem encoding: only the common source lifecycle is under test.
 */
typedef struct link_run {
	char		target[7];
	bool		fail_validation;
	bool		short_read;
	unsigned	planned;
	unsigned	emitted;
	unsigned	validations;
	unsigned	reads;
	unsigned	fail_validation_at;
	unsigned char	emitted_target[1024];
	size_t		emitted_length;
} link_run_t;

typedef struct link_plan {
	fsimg_plan_t	common;
	uint64_t	ids[2];
	unsigned char	targets[2][1024];
	size_t		lengths[2];
	unsigned	count;
} link_plan_t;

static fsimg_status_t
link_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	link_run_t *run = argument;

	CHECK(id == 7);
	run->validations++;
	if (run->fail_validation || run->validations == run->fail_validation_at)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "test link changed"));
	return (FSIMG_OK);
}

static fsimg_status_t
link_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	link_run_t *run = argument;

	CHECK(id == 7 && offset <= 6 && length <= 6 - offset);
	run->reads++;
	*count = run->short_read ? 0 : length;
	bcopy(run->target + offset, buffer, *count);
	return (FSIMG_OK);
}

static const char *validated_boot_bios;
static unsigned writer_validations;

static fsimg_status_t
link_writer_validate(fsimg_write_options_t *options,
    unsigned variant FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	writer_validations++;
	validated_boot_bios = options->boot_bios;
	options->sector_bytes = 512;
	return (FSIMG_OK);
}

static fsimg_status_t
link_writer_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	link_run_t *run = writer->provider_argument;
	link_plan_t *plan;
	const fsimg_catalog_entry_t *entry;
	unsigned char target[1024];
	size_t length;
	uint64_t work;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), error);
	CHECK(plan != NULL);
	writer->plan = &plan->common;
	plan->common.writer = writer;
	plan->common.info.volume_bytes = 512;
	plan->common.info.sector_bytes = 512;
	for (uint64_t id = 1; id <= writer->count; id++) {
		CHECK(fsimg_catalog_get(writer, id, &entry, NULL) == FSIMG_OK);
		if (entry->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		if (entry->entry.kind != FSIMG_ENTRY_SYMLINK) {
			CHECK(fsimg_writer_link_target(writer, id, target, &length,
			    NULL) == FSIMG_ESTATE);
			CHECK(!writer->source.active && length == 0);
			continue;
		}
		CHECK(plan->count < 2);
		work = writer->session.progress.work;
		status = fsimg_writer_link_target(writer, id,
		    plan->targets[plan->count], &plan->lengths[plan->count], error);
		CHECK(!writer->source.active);
		if (status != FSIMG_OK)
			return (status);
		CHECK(writer->session.progress.work > work);
		CHECK(plan->lengths[plan->count] == 6);
		CHECK(bcmp(plan->targets[plan->count], "target", 6) == 0);
		plan->ids[plan->count] = id;
		plan->count++;
		run->planned++;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
link_writer_emit(fsimg_writer_t *writer, fsimg_emit_t *emit FSIMG_UNUSED,
    fsimg_error_t *error)
{
	link_run_t *run = writer->provider_argument;
	link_plan_t *plan = (link_plan_t *)writer->plan;
	uint64_t work;
	fsimg_status_t status;

	for (unsigned i = 0; i < plan->count; i++) {
		work = writer->session.progress.work;
		status = fsimg_writer_link_target(writer, plan->ids[i],
		    run->emitted_target, &run->emitted_length, error);
		CHECK(!writer->source.active);
		if (status != FSIMG_OK)
			return (status);
		CHECK(writer->session.progress.work > work);
		CHECK(run->emitted_length == 6);
		CHECK(bcmp(run->emitted_target, run->target, 6) == 0);
		run->emitted++;
	}
	return (FSIMG_OK);
}

static void
link_writer_free(fsimg_plan_t *plan)
{
	fsimg_dealloc(plan);
}

static const fsimg_writer_ops_t link_writer_ops = {
	.validate = link_writer_validate, .plan = link_writer_plan,
	.emit = link_writer_emit, .free = link_writer_free
};
static const fsimg_format_variant_t link_variant = {
	.info = { .name = "test-links", .entry_kinds = FSIMG_ENTRY_FILE |
	    FSIMG_ENTRY_SYMLINK | FSIMG_ENTRY_DIRECTORY,
	    .max_link_target_bytes = 1024, .max_file_bytes = UINT64_MAX,
	    .max_volume_bytes = UINT64_MAX, .write_settings = FSIMG_SETTING_ELTORITO }
};
static const fsimg_format_t link_format = {
	.variants = &link_variant, .variant_count = 1, .writer = &link_writer_ops
};

static fsimg_writer_t *
new_link_writer(fsimg_ctx_t *ctx, link_run_t *run)
{
	fsimg_writer_t *writer;
	fsimg_source_ops_t ops;
	fsimg_entry_t entry = { .path = "/provider", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 6 };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 7 };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };

	bzero(run, sizeof(*run));
	strcpy(run->target, "target");
	CHECK(fsimg_writer_new_variant(ctx, &link_format, &link_variant, NULL,
	    &writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&ops);
	ops.validate = link_validate;
	ops.read_at = link_read;
	CHECK(fsimg_writer_set_sources(writer, &ops, run, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &entry, &source, NULL) == FSIMG_OK);
	entry = (fsimg_entry_t){ .path = "/file", .kind = FSIMG_ENTRY_FILE };
	CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
	return (writer);
}

static void
test_provider_link_reads_at_plan_and_emit_and_observes_changed_bytes(void)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_io_t *io;
	memory_image_t image;
	link_run_t run;
	const fsimg_plan_t *plan;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (unsigned changed = 0; changed < 2; changed++) {
		writer = new_link_writer(ctx, &run);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(run.planned == 1 && run.emitted == 0 && !writer->source.active);
		if (changed)
			strcpy(run.target, "newest");
		open_memory(ctx, &image, &io);
		CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
		CHECK(run.emitted == 1 && run.emitted_length == 6);
		CHECK(bcmp(run.emitted_target, changed ? "newest" : "target", 6) == 0);
		CHECK(!writer->source.active);
		fsimg_writer_free(writer);
		fsimg_io_free(io);
		free(image.bytes);
	}
	fsimg_ctx_free(ctx);
}

static void
test_link_read_failures_close_source(void)
{
	enum { FAIL_OPEN, SHORT_READ, FAIL_FINISH };
	static const char *const names[] = {
		"open validation", "short read", "finish validation"
	};
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_io_t *io;
	memory_image_t image;
	link_run_t run;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fsimg_status_t status;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (unsigned fault = 0; fault < 3; fault++) {
		writer = new_link_writer(ctx, &run);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		run.fail_validation = fault == FAIL_OPEN;
		run.short_read = fault == SHORT_READ;
		/* A provider is checked at open and finish. Fail only the
		 * latter to exercise cleanup after the target was read.
		 */
		if (fault == FAIL_FINISH)
			run.fail_validation_at = run.validations + 2;
		open_memory(ctx, &image, &io);
		status = fsimg_writer_emit(writer, io, &error);
		if (status != (fault == SHORT_READ ? FSIMG_EIO : FSIMG_ESOURCE_CHANGED))
			fprintf(stderr, "%s: unexpected status %d\n", names[fault], status);
		CHECK(status == (fault == SHORT_READ ? FSIMG_EIO : FSIMG_ESOURCE_CHANGED));
		if (fault != SHORT_READ)
			CHECK(strcmp(error.path, "/provider") == 0);
		CHECK(!writer->source.active && run.emitted == 0);
		fsimg_writer_free(writer);
		fsimg_io_free(io);
		free(image.bytes);
	}
	fsimg_ctx_free(ctx);
}

static void
count_link_losses(void *argument, const char *path FSIMG_UNUSED,
    uint32_t fields FSIMG_UNUSED)
{
	unsigned *count = argument;

	(*count)++;
}

static void
test_link_read_failures_during_planning_publish_nothing(void)
{
	enum { FAIL_OPEN, SHORT_READ, FAIL_FINISH };
	static const char *const names[] = {
		"planning open validation", "planning short read",
		"planning finish validation"
	};
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_options_t options;
	link_run_t run;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fsimg_status_t status, expected;
	unsigned losses = 0;
	fsimg_plan_t unpublished = { 0 };

	fsimg_options_init(&options);
	options.loss = count_link_losses;
	options.loss_argument = &losses;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	for (unsigned fault = 0; fault < 3; fault++) {
		writer = new_link_writer(ctx, &run);
		run.fail_validation = fault == FAIL_OPEN;
		run.short_read = fault == SHORT_READ;
		if (fault == FAIL_FINISH)
			run.fail_validation_at = 2;
		/* Seed with a valid non-NULL pointer: failure must clear the
		 * output rather than expose the driver's incomplete summary.
		 */
		plan = &unpublished;
		expected = fault == SHORT_READ ? FSIMG_EIO : FSIMG_ESOURCE_CHANGED;
		status = fsimg_writer_plan(writer, &plan, &error);
		if (status != expected)
			fprintf(stderr, "%s: expected %d, got %d\n",
			    names[fault], expected, status);
		CHECK(status == expected);
		if (fault != SHORT_READ)
			CHECK(strcmp(error.path, "/provider") == 0);
		CHECK(run.validations == (fault == SHORT_READ ? 0u :
		    (fault == FAIL_FINISH ? 2u : 1u)));
		CHECK(run.reads == 1);
		CHECK(!writer->source.active);
		CHECK(writer->source.io == NULL && writer->source.fd == -1);
		CHECK(writer->state == FSIMG_WRITER_FAILED);
		CHECK(plan == NULL && writer->plan == NULL);
		CHECK(run.planned == 0 && run.emitted == 0);
		CHECK(losses == 0 && ctx->stats.metadata_losses == 0);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_ESTATE);
		CHECK(plan == NULL && losses == 0);
		fsimg_writer_free(writer);
	}
	fsimg_ctx_free(ctx);
}

static void
test_host_link_reads_and_retargeted_link_names_image_path(void)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_io_t *io;
	memory_image_t image;
	link_run_t run;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	char directory[] = "common-source-XXXXXX";
	int root;

	CHECK(mkdtemp(directory) != NULL);
	root = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (unsigned changed = 0; changed < 2; changed++) {
		CHECK(symlinkat("target", root, "host") == 0);
		writer = new_link_writer(ctx, &run);
		CHECK(fsimg_writer_add_tree(writer, root, "/", NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(run.planned == 2);
		if (changed) {
			CHECK(unlinkat(root, "host", 0) == 0);
			CHECK(symlinkat("newest", root, "host") == 0);
		}
		open_memory(ctx, &image, &io);
		if (changed) {
			CHECK(fsimg_writer_emit(writer, io, &error) == FSIMG_ESOURCE_CHANGED);
			CHECK(strcmp(error.path, "/host") == 0);
		} else {
			CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
			CHECK(run.emitted == 2 && run.emitted_length == 6);
			CHECK(bcmp(run.emitted_target, "target", 6) == 0);
		}
		CHECK(!writer->source.active);
		fsimg_writer_free(writer);
		fsimg_io_free(io);
		free(image.bytes);
		CHECK(unlinkat(root, "host", 0) == 0);
	}
	fsimg_ctx_free(ctx);
	CHECK(close(root) == 0 && rmdir(directory) == 0);
}

static void
test_writer_owns_validated_boot_paths(void)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_error_t error;
	char bios[] = "/boot/image", efi[] = "/efi/image";
	static const char *const invalid[] = { "relative", "" };

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	options.boot_bios = bios;
	options.boot_efi = efi;
	writer_validations = 0;
	CHECK(fsimg_writer_new_variant(ctx, &link_format, &link_variant,
	    &options, &writer, NULL) == FSIMG_OK);
	CHECK(writer_validations == 1);
	CHECK(validated_boot_bios != bios);
	CHECK(validated_boot_bios == writer->options.boot_bios);
	CHECK(writer->options.boot_bios != bios && writer->options.boot_efi != efi);
	bios[1] = efi[1] = 'X';
	CHECK(strcmp(writer->options.boot_bios, "/boot/image") == 0);
	CHECK(strcmp(writer->options.boot_efi, "/efi/image") == 0);
	CHECK(writer->options.boot_bios == writer->boot_bios);
	CHECK(writer->options.boot_efi == writer->boot_efi);
	fsimg_writer_free(writer);
	for (unsigned field = 0; field < 2; field++) {
		for (size_t i = 0; i < 2; i++) {
			fsimg_write_options_init(&options);
			if (field == 0)
				options.boot_bios = invalid[i];
			else
				options.boot_efi = invalid[i];
			writer_validations = 0;
			CHECK(fsimg_writer_new_variant(ctx, &link_format, &link_variant,
			    &options, &writer, &error) == FSIMG_EINVAL);
			CHECK(writer_validations == 0);
			CHECK(error.operation == FSIMG_OP_WRITER);
			CHECK(strcmp(error.path, field == 0 ? "boot" : "efiboot") == 0);
		}
	}
	CHECK(fsimg_writer_new_variant(ctx, &link_format, &link_variant, NULL,
	    &writer, NULL) == FSIMG_OK);
	CHECK(writer->options.boot_bios == NULL && writer->options.boot_efi == NULL);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_provider_link_reads_at_plan_and_emit_and_observes_changed_bytes();
	test_link_read_failures_close_source();
	test_link_read_failures_during_planning_publish_nothing();
	test_host_link_reads_and_retargeted_link_names_image_path();
	test_writer_owns_validated_boot_paths();
	test_decoded_link_host_change_during_verification_is_terminal();
	test_decoded_link_cancellation_during_verification_is_terminal();
	test_decoded_link_allocation_failure_during_advance_is_terminal();
	test_content_reports_stored_bytes_separately();
	test_decoded_link_declared_inconsistently_during_replay_fails_at_advance();
	test_decoded_link_reads_ranges_and_verification();
	test_decoded_link_declaration_bounds_and_single_allocation();
	test_decoded_link_buffer_obeys_memory_budget();
	test_decoded_link_mutated_after_replay_fails_at_delivery();
	test_decoded_link_cat_selection_delivers_target();
	test_second_walk_checks_omitted_subtrees_without_accounting();
	test_second_walk_payload_reads_count_stored_bytes();
	test_second_walk_rejects_duplicate_namespace();
	test_second_walk_obeys_depth_and_work_limits();
	test_restart_requires_full_verification_and_finished_walk();
	test_reader_view_rejected_before_driver_open_or_image_reads();
	test_view_applicability_precedes_driver_open();
	test_decoded_link_payload_verification_charges_work();
	return (0);
}
