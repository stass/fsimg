/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg.h"
#include "test.h"

/* POSIX metadata against the registered writers. FAT and exFAT store none of
 * the additions, so planning treats each present field as one omission under
 * the common loss policy and rejects the additional entry kinds outright.
 * Omitted fields never change the image: an image planned with them is
 * compared byte for byte with the same tree planned without them.
 */

#define POSIX_FIELDS	(FSIMG_META_CHANGED | FSIMG_META_OWNER | FSIMG_META_MODE)
#define FILE_BYTES	1000u

static const char *const writer_formats[] = { "fat12", "fat16", "fat32", "exfat" };

typedef struct posix_fixture {
	fsimg_ctx_t	*ctx;
	uint64_t	 loss_calls;
	uint32_t	 lost_fields;
	int		 directory;
	char		 path[64];
} posix_fixture_t;

typedef enum tree_metadata {
	TREE_ORDINARY,
	TREE_POSIX_OPTIONAL,
	TREE_POSIX_REQUIRED
} tree_metadata_t;

static void fixture_open(posix_fixture_t *);
static void fixture_close(posix_fixture_t *);
static void record_loss(void *, const char *, uint32_t);
static fsimg_status_t target_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t target_read(void *, uint64_t, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_writer_t *new_writer(posix_fixture_t *, const char *, bool);
static void add_tree(fsimg_writer_t *, tree_metadata_t);
static void emit_image(posix_fixture_t *, fsimg_writer_t *, const char *);
static bool images_equal(posix_fixture_t *, const char *, const char *);
static void test_capability_records(void);
static void test_omitted_fields(const char *);
static void test_forbidden_losses(const char *);
static void test_unsupported_kinds(const char *);

/* The provider supplies a symbolic link target of repeated 't' bytes. */
static fsimg_status_t
target_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	(void)argument;
	(void)id;
	(void)error;
	return (FSIMG_OK);
}

static fsimg_status_t
target_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	(void)argument;
	(void)id;
	(void)offset;
	memset(buffer, 't', length);
	*transferred = length;
	(void)error;
	return (FSIMG_OK);
}

static void
record_loss(void *argument, const char *path, uint32_t fields)
{
	posix_fixture_t *fixture = argument;

	CHECK(path != NULL && path[0] == '/');
	fixture->loss_calls++;
	fixture->lost_fields |= fields;
}

static void
fixture_open(posix_fixture_t *fixture)
{
	fsimg_options_t options;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->path, sizeof(fixture->path), "posix-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->path) != NULL);
	fixture->directory = open(fixture->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->directory != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(32) * 1024 * 1024;
	options.scratch_directory_fd = -1;
	options.loss = record_loss;
	options.loss_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
}

static void
fixture_close(posix_fixture_t *fixture)
{
	fsimg_ctx_free(fixture->ctx);
	CHECK(unlinkat(fixture->directory, "plain.img", 0) == 0 || errno == ENOENT);
	CHECK(unlinkat(fixture->directory, "posix.img", 0) == 0 || errno == ENOENT);
	CHECK(close(fixture->directory) == 0);
	CHECK(rmdir(fixture->path) == 0);
}

static fsimg_writer_t *
new_writer(posix_fixture_t *fixture, const char *format, bool allow_loss)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	if (!allow_loss)
		options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
	CHECK(fsimg_writer_new(fixture->ctx, format, &options, &writer, NULL) == FSIMG_OK);
	return (writer);
}

/* One directory and one file. The ordinary tree carries no metadata, so a
 * reproducible plan of it records no loss. The POSIX variants add owner and
 * mode to the directory and owner, mode and change time to the file: five
 * fields.
 */
static void
add_tree(fsimg_writer_t *writer, tree_metadata_t metadata)
{
	fsimg_entry_t directory = { .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	fsimg_entry_t file = { .path = "/dir/file", .kind = FSIMG_ENTRY_FILE,
	    .size = FILE_BYTES };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_timestamp_t changed = { .seconds = 1000000000,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };

	if (metadata != TREE_ORDINARY) {
		directory.metadata.valid |= FSIMG_META_OWNER | FSIMG_META_MODE;
		directory.metadata.uid = 1000;
		directory.metadata.gid = 1000;
		directory.metadata.mode = 0755;
		file.metadata.valid |= POSIX_FIELDS;
		file.metadata.uid = 1000;
		file.metadata.gid = 100;
		file.metadata.mode = 04644;
		file.metadata.changed = changed;
	}
	if (metadata == TREE_POSIX_REQUIRED)
		file.metadata.required = FSIMG_META_OWNER;
	CHECK(fsimg_writer_add(writer, &directory, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &file, &zero, NULL) == FSIMG_OK);
}

static void
emit_image(posix_fixture_t *fixture, fsimg_writer_t *writer, const char *name)
{
	fsimg_io_t *io;
	int fd;

	fd = openat(fixture->directory, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
	    0600);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(fixture->ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE,
	    &io, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
	fsimg_io_free(io);
}

static bool
images_equal(posix_fixture_t *fixture, const char *left_name,
    const char *right_name)
{
	unsigned char left_bytes[65536], right_bytes[65536];
	ssize_t left_count, right_count;
	bool equal;
	int left, right;

	left = openat(fixture->directory, left_name, O_RDONLY | O_CLOEXEC);
	right = openat(fixture->directory, right_name, O_RDONLY | O_CLOEXEC);
	CHECK(left != -1 && right != -1);
	equal = true;
	for (;;) {
		left_count = read(left, left_bytes, sizeof(left_bytes));
		right_count = read(right, right_bytes, sizeof(right_bytes));
		CHECK(left_count >= 0 && right_count >= 0);
		if (left_count != right_count ||
		    bcmp(left_bytes, right_bytes, (size_t)left_count) != 0) {
			equal = false;
			break;
		}
		if (left_count == 0)
			break;
	}
	CHECK(close(left) == 0 && close(right) == 0);
	return (equal);
}

/* FAT and exFAT advertise exactly the original kinds and fields, and a label
 * capacity in encoded bytes: eleven OEM bytes, or eleven UTF-16 units. QNX6
 * stores every kind and the POSIX fields, and has no label.
 */
static void
test_capability_records(void)
{
	fsimg_format_info_t info;
	size_t cursor;
	unsigned count;

	cursor = 0;
	count = 0;
	while (fsimg_format_next(&cursor, &info, NULL) == FSIMG_OK) {
		count++;
		if (strcmp(info.name, "qnx6") == 0 ||
		    strcmp(info.name, "qnx4") == 0) {
			CHECK(info.entry_kinds == (FSIMG_ENTRY_FILE |
			    FSIMG_ENTRY_DIRECTORY | FSIMG_ENTRY_SYMLINK |
			    FSIMG_ENTRY_FIFO));
			CHECK(info.metadata_fields == (FSIMG_META_CREATED |
			    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
			    FSIMG_META_CHANGED | FSIMG_META_OWNER |
			    FSIMG_META_MODE));
			CHECK(info.max_label_bytes ==
			    (strcmp(info.name, "qnx4") == 0 ? 48 : 0));
			continue;
		}
		CHECK(info.entry_kinds == (FSIMG_ENTRY_FILE | FSIMG_ENTRY_DIRECTORY));
		CHECK(info.metadata_fields == (FSIMG_META_CREATED |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED | FSIMG_META_ATTRIBUTES));
		if (strcmp(info.name, "exfat") == 0)
			CHECK(info.max_label_bytes == 22);
		else
			CHECK(info.max_label_bytes == 11);
	}
	CHECK(count == 6);
}

/* Optional owner, mode and change time are counted once per field and
 * reported, and the image is the one planned without them.
 */
static void
test_omitted_fields(const char *format)
{
	posix_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_stats_t stats;

	fixture_open(&fixture);
	writer = new_writer(&fixture, format, true);
	add_tree(writer, TREE_ORDINARY);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 0 && fixture.loss_calls == 0);
	emit_image(&fixture, writer, "plain.img");
	fsimg_writer_free(writer);

	writer = new_writer(&fixture, format, true);
	add_tree(writer, TREE_POSIX_OPTIONAL);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 5);
	CHECK(fixture.loss_calls == 2 && fixture.lost_fields == POSIX_FIELDS);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.metadata_losses == 5);
	emit_image(&fixture, writer, "posix.img");
	fsimg_writer_free(writer);

	CHECK(images_equal(&fixture, "plain.img", "posix.img"));
	fixture_close(&fixture);
}

/* A caller that clears ALLOW_LOSS, or requires a field, gets a failed plan. */
static void
test_forbidden_losses(const char *format)
{
	posix_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;

	fixture_open(&fixture);
	writer = new_writer(&fixture, format, false);
	add_tree(writer, TREE_POSIX_OPTIONAL);
	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(error.operation == FSIMG_OP_WRITER && plan == NULL);
	fsimg_writer_free(writer);

	writer = new_writer(&fixture, format, true);
	add_tree(writer, TREE_POSIX_REQUIRED);
	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	fsimg_writer_free(writer);

	/* Without the new fields the stricter policy changes nothing. */
	writer = new_writer(&fixture, format, false);
	add_tree(writer, TREE_ORDINARY);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	fixture_close(&fixture);
}

/* A kind is never optional: a format without links or FIFOs fails planning
 * whatever the loss policy.
 */
static void
test_unsupported_kinds(const char *format)
{
	posix_fixture_t fixture;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fsimg_entry_t link = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 4 };
	fsimg_entry_t fifo = { .path = "/fifo", .kind = FSIMG_ENTRY_FIFO };
	fsimg_source_t target = { .kind = FSIMG_SOURCE_PROVIDER };
	fsimg_source_ops_t provider;

	fsimg_source_ops_init(&provider);
	provider.validate = target_validate;
	provider.read_at = target_read;
	fixture_open(&fixture);
	writer = new_writer(&fixture, format, true);
	CHECK(fsimg_writer_set_sources(writer, &provider, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &link, &target, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strstr(error.path, "/link") != NULL);
	fsimg_writer_free(writer);

	/* A plan that fails reports none of the losses its other entries
	 * would have counted.
	 */
	writer = new_writer(&fixture, format, true);
	CHECK(fsimg_writer_set_sources(writer, &provider, NULL, NULL) == FSIMG_OK);
	add_tree(writer, TREE_POSIX_OPTIONAL);
	CHECK(fsimg_writer_add(writer, &link, &target, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/link") == 0);
	CHECK(fixture.loss_calls == 0);
	fsimg_writer_free(writer);

	writer = new_writer(&fixture, format, true);
	add_tree(writer, TREE_ORDINARY);
	CHECK(fsimg_writer_add(writer, &fifo, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/fifo") == 0);
	CHECK(fixture.loss_calls == 0);
	fsimg_writer_free(writer);
	fixture_close(&fixture);
}

int
main(void)
{
	test_capability_records();
	for (size_t i = 0; i < sizeof(writer_formats) / sizeof(writer_formats[0]); i++) {
		test_omitted_fields(writer_formats[i]);
		test_forbidden_losses(writer_formats[i]);
		test_unsupported_kinds(writer_formats[i]);
	}
	return (0);
}
