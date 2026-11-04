/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "extract.h"
#include "fsimg-endian.h"
#include "test.h"

typedef struct fake_driver {
	fsimg_status_t status;
	fsimg_operation_t operation;
	bool root_failure;
	bool replay_failure;
	bool partial_replay;
	bool collision;
	bool allocate;
	bool seal;
	bool omitted;
	unsigned repeated;
	unsigned descents;
	bool content_failure;
	unsigned corrupt_validation;
	const char *replay_name;
} fake_driver_t;

typedef struct fixture {
	fake_driver_t fake;

	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	unsigned char *bytes;
	size_t length;
	unsigned reports;
	char loss_path[128];
	uint32_t loss_mask;
} fixture_t;

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	CHECK(offset <= f->length && length <= f->length - offset);
	bcopy(f->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	CHECK(offset <= f->length && length <= f->length - offset);
	bcopy(buffer, f->bytes + offset, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument, uint64_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	CHECK(length != 0 && length <= 128u * 1024u * 1024u);
	f->bytes = realloc(f->bytes, (size_t)length);
	CHECK(f->bytes != NULL);
	if (length > f->length)
		bzero(f->bytes + f->length, (size_t)length - f->length);
	f->length = (size_t)length;
	return (FSIMG_OK);
}

static void
record_loss(void *argument, const char *path, uint32_t mask)
{
	fixture_t *f = argument;

	if ((mask & (FSIMG_LOSS_DIRECTORY | FSIMG_LOSS_ENTRY)) == 0)
		return;
	f->reports++;

	CHECK(strlen(path) < sizeof(f->loss_path));
	strcpy(f->loss_path, path);
	f->loss_mask = mask;
}

static fsimg_status_t
source_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
source_read(void *argument FSIMG_UNUSED, uint64_t id,
    uint64_t offset FSIMG_UNUSED, void *buffer, size_t length, size_t *count,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memset(buffer, (int)('A' + id), length);
	*count = length;
	return (FSIMG_OK);
}

static const char *const paths[] = {
	"/a", "/a/inner", "/bad", "/bad/child", "/c", "/c/deep",
	"/c/deep/leaf", "/z"
};

static void
fixture_open_prefix(fixture_t *f, const char *format, size_t count)
{
	fsimg_options_t options;
	fsimg_write_options_t write_options;
	fsimg_io_ops_t io_ops;
	fsimg_source_ops_t sources;
	fsimg_writer_t *writer;

	bzero(f, sizeof(*f));
	fsimg_options_init(&options);
	options.memory_bytes = 32u * 1024u * 1024u;
	options.loss = record_loss;
	options.loss_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&io_ops);
	io_ops.read_at = memory_read;
	io_ops.write_at = memory_write;
	io_ops.get_size = memory_size;
	io_ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(f->ctx, &io_ops, f, &f->io, NULL) ==
	    FSIMG_OK);
	fsimg_write_options_init(&write_options);
	if (strcmp(format, "fat12") == 0)
		write_options.volume_bytes = 1024u * 1024u;
	CHECK(fsimg_writer_new(f->ctx, format, &write_options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_source_ops_init(&sources);
	sources.validate = source_validate;
	sources.read_at = source_read;
	CHECK(fsimg_writer_set_sources(writer, &sources, NULL, NULL) == FSIMG_OK);
	for (size_t i = 0; i < count; i++) {
		bool file = i == 1 || i == 3 || i == 6 || i == 7;
		fsimg_entry_t entry = { .path = paths[i],
		    .kind = file ? FSIMG_ENTRY_FILE : FSIMG_ENTRY_DIRECTORY,
		    .size = file ? 7 : 0 };
		fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = i };

		CHECK(fsimg_writer_add(writer, &entry, file ? &source : NULL,
		    NULL) == FSIMG_OK);
	}
	CHECK(fsimg_writer_emit(writer, f->io, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	f->reports = 0;
}

static void
fixture_open(fixture_t *f, const char *format)
{
	fixture_open_prefix(f, format, sizeof(paths) / sizeof(paths[0]));
}

static void
fixture_reader(fixture_t *f)
{
	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL) == FSIMG_OK);
}

static void
fixture_close(fixture_t *f)
{
	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	free(f->bytes);
	fsimg_ctx_free(f->ctx);
}

static unsigned char *
find_bytes(fixture_t *f, const void *needle, size_t length)
{
	for (size_t i = 0; i + length <= f->length; i++) {
		if (bcmp(f->bytes + i, needle, length) == 0)
			return (f->bytes + i);
	}
	CHECK(false);
	return (NULL);
}

/* Damage a child's name in its containing listing, leaving /bad's own
 * parent record intact. exFAT checks the set checksum before the name.
 * ImageFS builds listings at open; change its image record after open to
 * exercise replay instead. A root-listing failure is unreachable there.
 */
static void
damage_listing(fixture_t *f, const char *format, bool root)
{
	unsigned char *name;
	unsigned char wide[16];
	const char *component = root ? "bad" : "child";
	size_t length = strlen(component);

	if (strncmp(format, "fat", 3) == 0) {
		bzero(wide, sizeof(wide));
		for (size_t i = 0; i < length; i++)
			wide[2 * i] = (unsigned char)component[i];
		name = find_bytes(f, wide, 2 * length);
		/* Claim a two-slot LFN sequence where only one slot exists. */
		name[-1] = 0x42;
	} else if (strcmp(format, "exfat") == 0) {
		bzero(wide, sizeof(wide));
		for (size_t i = 0; i < length; i++)
			wide[2 * i] = (unsigned char)component[i];
		name = find_bytes(f, wide, 2 * length);
		/* The name starts two bytes into the secondary at set + 64. */
		name[-64] ^= 1;
	} else if (strcmp(format, "ifs") == 0) {
		CHECK(!root);
		name = find_bytes(f, "bad/child", 9);
		name[8] = '/';
	} else {
		/* QNX slots and Rock Ridge NM carry these literal components. */
		name = find_bytes(f, component, length);
		name[0] = '/';
	}
}

static void
remove_tree(int parent, const char *name)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(fd != -1);
	directory = fdopendir(fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		CHECK(fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(st.st_mode))
			remove_tree(fd, entry->d_name);
		else
			CHECK(unlinkat(fd, entry->d_name, 0) == 0);
	}
	CHECK(closedir(directory) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

static void
fake_close(fsimg_reader_t *reader FSIMG_UNUSED)
{
}

static fsimg_status_t
fake_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    unsigned char *key, size_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = strlen(name);
	bcopy(name, key, *length);
	return (FSIMG_OK);
}

static fsimg_status_t
fake_keys(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	keys->count = 1;
	return (fake_key(reader, name, keys->key[0], &keys->length[0], error));
}

static fsimg_status_t
fake_failure(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fixture_t *f = reader->driver;
	fsimg_error_t detail;

	if (f->fake.allocate) {
		fsimg_test_fail_alloc_after(f->ctx, 1);
		CHECK(fsimg_alloc(f->ctx, 1, &detail) == NULL);
		fsimg_test_fail_alloc_after(f->ctx, 0);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	return (fsimg_error_set(error, f->fake.status, 0, f->fake.operation,
	    "injected driver failure"));
}

static fsimg_status_t
fake_admit(fsimg_directory_build_t *build, const char *name,
    fsimg_entry_kind_t kind, fsimg_error_t *error)
{
	unsigned char payload = (unsigned char)kind;

	return (fsimg_directory_admit(build, name, &payload, 1, error));
}

static fsimg_status_t
fake_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fixture_t *f = reader->driver;
	fsimg_status_t status;
	bool root = fsimg_directory_level(build) == 0;
	char name[32];
	fsimg_stream_t *records;
	fsimg_stream_mark_t mark;
	unsigned char invalid = 0xff;

	if (!root || f->fake.root_failure) {
		if (!root)
			f->fake.descents++;
		status = fake_admit(build, "inside", FSIMG_ENTRY_FILE, error);
		if (status != FSIMG_OK)
			return (status);
		if (f->fake.seal) {
			CHECK(fsimg_stream_seal(fsimg_test_walk_records(reader),
			    NULL) == FSIMG_OK);
		}
		if (f->fake.collision) {
			if (f->fake.corrupt_validation == 0)
				return (fake_admit(build, "inside", FSIMG_ENTRY_FILE, error));
			records = fsimg_test_walk_records(reader);
			CHECK(records != NULL);
			mark = fsimg_stream_mark(records);
			status = fake_admit(build, "inside", FSIMG_ENTRY_FILE, error);
			if (status != FSIMG_OK)
				return (status);
			return (fsimg_store_write(records->store, mark.offset +
			    (f->fake.corrupt_validation == 2 ? 8 : 0), &invalid, 1,
			    error));
		}

		if (!f->fake.replay_failure && f->fake.status != FSIMG_OK)
			return (fake_failure(reader, error));
		return (FSIMG_OK);
	}
	if (f->fake.repeated != 0) {
		for (unsigned i = 0; i < f->fake.repeated; i++) {
			CHECK(snprintf(name, sizeof(name), "d%03u", i) < (int)sizeof(name));
			status = fake_admit(build, name, FSIMG_ENTRY_DIRECTORY, error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (FSIMG_OK);
	}
	status = fake_admit(build, "a", FSIMG_ENTRY_FILE, error);
	if (status == FSIMG_OK)
		status = fake_admit(build, f->fake.omitted ? "\xff" : "bad",
		    FSIMG_ENTRY_DIRECTORY, error);
	if (status == FSIMG_OK)
		status = fake_admit(build, "c", FSIMG_ENTRY_FILE, error);
	return (status);
}

static fsimg_status_t
fake_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fixture_t *f = reader->driver;
	fsimg_status_t status;

	CHECK(length == 1);
	if (f->fake.replay_failure && strcmp(name, f->fake.replay_name) == 0) {
		if (f->fake.partial_replay) {
			reader->entry.kind = FSIMG_ENTRY_SYMLINK;
			reader->entry.size = 99;
			status = fsimg_reader_set_link_target(reader, "stale-target",
			    12, error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (fake_failure(reader, error));
	}
	reader->entry.kind = payload[0];
	if (reader->entry.kind == FSIMG_ENTRY_FILE)
		reader->entry.size = reader->initialized = 3;
	return (FSIMG_OK);
}

static fsimg_status_t
fake_resolve(fsimg_reader_t *reader, uint64_t offset FSIMG_UNUSED,
    fsimg_extent_t *extent, fsimg_error_t *error)
{
	fixture_t *f = reader->driver;

	if (f->fake.content_failure && strcmp(reader->entry.path, "/c") == 0)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IO,
		    "injected content I/O failure"));
	*extent = (fsimg_extent_t){ .image_offset = 0, .length = 3 };
	return (FSIMG_OK);
}

static const fsimg_reader_ops_t fake_ops = {
	.close = fake_close, .prepare = fake_prepare, .replay = fake_replay,
	.key = fake_key, .keys = fake_keys, .ambiguous = "duplicate test name",
	.name_profile = FSIMG_NAME_PROFILE_PORTABLE, .resolve = fake_resolve
};

static void
fake_open(fixture_t *f)
{
	fixture_open(f, "fat12");
	fixture_reader(f);
	f->reader->ops->close(f->reader);
	f->reader->ops = &fake_ops;
	f->reader->driver = f;
	f->reader->content_domain = (fsimg_extent_domain_t){
	    .length = f->length, .unit_bytes = 1 };
	bcopy("yes", f->bytes, 3);
	f->fake.operation = FSIMG_OP_READER;
	f->fake.replay_name = "bad";
}

static void
expect_entry(fixture_t *f, const char *path)
{
	const fsimg_entry_t *entry;

	CHECK(fsimg_reader_next(f->reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry != NULL && strcmp(entry->path, path) == 0);
	CHECK(f->reader->state == FSIMG_READER_ITERATING);
}

static void
corrupt_next_record(fixture_t *f, bool class_byte)
{
	fsimg_store_t *store;
	uint64_t cursor;
	unsigned char invalid = 0xff;

	fsimg_test_walk_cursor(f->reader, &store, &cursor);
	CHECK(store != NULL);
	CHECK(fsimg_store_write(store, cursor + (class_byte ? 8 : 0),
	    &invalid, 1, NULL) == FSIMG_OK);
}

/* The one-slot LFN for z is followed by its short entry. Inflating that
 * entry's length needs a second cluster that the writer never allocated.
 */
static void
damage_file(fixture_t *f)
{
	static const unsigned char z[] = { 'z', 0, 0, 0, 0xff, 0xff };
	unsigned char *name = find_bytes(f, z, sizeof(z));

	fsimg_store_le32(name + 31 + 28, 65537);
}

static void
expect_failure(fixture_t *f, fsimg_status_t status,
    fsimg_operation_t operation, const char *message, const char *path,
    bool without_error)
{
	const fsimg_entry_t *entry = NULL;
	fsimg_error_t error;

	CHECK(fsimg_reader_next(f->reader, &entry,
	    without_error ? NULL : &error) == status);
	CHECK(entry == NULL);
	if (!without_error) {
		CHECK(error.status == status);
		CHECK(error.operation == operation);
		CHECK(strcmp(error.message, message) == 0);
		CHECK(strcmp(error.path, path) == 0);
	}
	CHECK(f->reader->state == FSIMG_READER_FAILED);
	CHECK(f->reader->salvage.losses == 0);
	CHECK(f->reports == 0);
	CHECK(f->ctx->stats.salvage_losses == 0);
}

static void
expect_losses(fixture_t *f, uint64_t total, unsigned reports,
    const char *path, uint32_t mask)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(f->reader->salvage.losses == total);
	CHECK(stats.salvage_losses == total);
	CHECK(f->reports == reports);
	if (reports != 0) {
		CHECK(strcmp(f->loss_path, path) == 0);
		CHECK(f->loss_mask == mask);
	}
}

static void
expect_done(fixture_t *f)
{
	const fsimg_entry_t *entry;

	for (unsigned i = 0; i < 2; i++) {
		CHECK(fsimg_reader_next(f->reader, &entry, NULL) == FSIMG_DONE);
		CHECK(entry == NULL);
		CHECK(f->reader->state == FSIMG_READER_DONE);
	}
}

static void
expect_content(fixture_t *f)
{
	char bytes[4] = { 0 };
	size_t count;

	CHECK(fsimg_reader_read_at(f->reader, 0, bytes, 3, &count, NULL) == FSIMG_OK);
	CHECK(count == 3 && strcmp(bytes, "yes") == 0);
}

/* A failed child appended records before rejecting the image. Its parent's
 * complete listing must survive, and the child's records must be discarded.
 */
static void
test_bad_child_listing_preserves_parent_and_rewinds_records(void)
{
	static const fsimg_status_t failures[] = {
		FSIMG_ECORRUPT, FSIMG_EUNSUPPORTED, FSIMG_OK
	};
	fixture_t f;
	fsimg_stream_t *records;
	fsimg_stream_mark_t mark;
	uint64_t metadata_losses;

	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		fake_open(&f);
		f.reader->salvage.enabled = true;
		f.fake.status = failures[i];
		f.fake.collision = failures[i] == FSIMG_OK;
		metadata_losses = f.ctx->stats.metadata_losses;
		expect_entry(&f, "/a");
		expect_entry(&f, "/bad");
		records = fsimg_test_walk_records(f.reader);
		CHECK(records != NULL);
		mark = fsimg_stream_mark(records);

		expect_entry(&f, "/c");
		CHECK(records->end == mark.offset && records->count == mark.count);
		expect_losses(&f, 1, 1, "/bad", FSIMG_LOSS_DIRECTORY);
		CHECK(f.ctx->stats.metadata_losses == metadata_losses);
		expect_content(&f);
		expect_done(&f);
		fixture_close(&f);
	}
}

static void
test_bad_root_listing_finishes_with_one_loss(void)
{
	fixture_t f;

	fake_open(&f);
	f.reader->salvage.enabled = true;
	f.fake.root_failure = true;
	f.fake.status = FSIMG_ECORRUPT;
	expect_done(&f);
	expect_losses(&f, 1, 1, "/", FSIMG_LOSS_DIRECTORY);
	fixture_close(&f);
}

static void
test_failed_replay_skips_entry_without_descending_and_renders_name(void)
{
	static const char *const names[] = { "a", "bad", "\xff" };
	static const char *const rendered[] = { "/a", "/bad", "/\\xff" };
	fixture_t f;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		fake_open(&f);
		f.reader->salvage.enabled = true;
		f.reader->omission.descend = false;
		f.fake.omitted = i == 2;
		f.fake.replay_failure = true;
		f.fake.replay_name = names[i];
		f.fake.status = FSIMG_ECORRUPT;
		if (i != 0)
			expect_entry(&f, "/a");
		else {
			expect_entry(&f, "/bad");
			CHECK(fsimg_reader_skip_directory(f.reader, NULL) == FSIMG_OK);
		}
		expect_entry(&f, "/c");
		CHECK(f.fake.descents == 0);
		expect_losses(&f, 1, 1, rendered[i], FSIMG_LOSS_ENTRY);
		expect_content(&f);
		expect_done(&f);
		fixture_close(&f);
	}
}

static void
test_failed_replay_discards_partial_entry_and_link_content(void)
{
	fixture_t f;

	fake_open(&f);
	f.reader->salvage.enabled = true;
	f.fake.replay_failure = true;
	f.fake.partial_replay = true;
	f.fake.status = FSIMG_ECORRUPT;
	expect_entry(&f, "/a");
	expect_entry(&f, "/c");
	CHECK(f.reader->entry.kind == FSIMG_ENTRY_FILE);
	CHECK(f.reader->entry.size == 3);
	CHECK(f.reader->initialized == 3);
	CHECK(f.reader->link_length == 0);
	CHECK(f.fake.descents == 0);
	expect_content(&f);
	expect_losses(&f, 1, 1, "/bad", FSIMG_LOSS_ENTRY);
	expect_done(&f);
	fixture_close(&f);
}

static void
test_rejecting_last_record_finishes_level_and_resumes_parent(void)
{
	fixture_t f;

	for (unsigned child = 0; child < 2; child++) {
		fake_open(&f);
		f.reader->salvage.enabled = true;
		f.fake.replay_failure = true;
		f.fake.replay_name = child ? "inside" : "c";
		f.fake.status = FSIMG_ECORRUPT;
		expect_entry(&f, "/a");
		expect_entry(&f, "/bad");
		if (child) {
			expect_entry(&f, "/c");
			expect_content(&f);
		} else
			expect_entry(&f, "/bad/inside");
		expect_done(&f);
		CHECK(f.fake.descents == 1);
		expect_losses(&f, 1, 1, child ? "/bad/inside" : "/c",
		    FSIMG_LOSS_ENTRY);
		fixture_close(&f);
	}
}

static void
test_loss_examples_are_capped_but_totals_are_exact(void)
{
	fixture_t f;
	char path[32];

	fake_open(&f);
	f.reader->salvage.enabled = true;
	f.fake.repeated = FSIMG_LOSS_EXAMPLES + 7;
	f.fake.status = FSIMG_ECORRUPT;
	for (unsigned i = 0; i < f.fake.repeated; i++) {
		CHECK(snprintf(path, sizeof(path), "/d%03u", i) < (int)sizeof(path));
		expect_entry(&f, path);
	}
	expect_done(&f);
	CHECK(snprintf(path, sizeof(path), "/d%03u", FSIMG_LOSS_EXAMPLES - 1) <
	    (int)sizeof(path));
	expect_losses(&f, f.fake.repeated, FSIMG_LOSS_EXAMPLES, path,
	    FSIMG_LOSS_DIRECTORY);
	fixture_close(&f);
}

static void
test_terminal_prepare_and_replay_errors_preserve_status_and_detail(void)
{
	static const fsimg_status_t statuses[] = {
		FSIMG_EIO, FSIMG_ENOMEM, FSIMG_ELIMIT, FSIMG_ECANCELLED,
		FSIMG_ESOURCE_CHANGED, FSIMG_ESTATE, FSIMG_ECORRUPT
	};
	fixture_t f;
	fsimg_operation_t operation;

	for (unsigned replay = 0; replay < 2; replay++) {
		for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
			for (unsigned no_error = 0; no_error < 2; no_error++) {
				fake_open(&f);
				f.reader->salvage.enabled = true;
				f.fake.status = statuses[i];
				f.fake.replay_failure = replay != 0;
				f.fake.allocate = statuses[i] == FSIMG_ENOMEM;
				f.fake.operation = statuses[i] == FSIMG_ECORRUPT ?
				    FSIMG_OP_METADATA : FSIMG_OP_READER;
				operation = f.fake.allocate ? FSIMG_OP_ALLOCATE :
				    f.fake.operation;
				expect_entry(&f, "/a");
				if (!replay)
					expect_entry(&f, "/bad");

				expect_failure(&f, statuses[i], operation,
				    f.fake.allocate ? "allocation failed" :
				    "injected driver failure", "", no_error != 0);
				fixture_close(&f);
			}
		}
	}
}

static void
test_failed_record_rewind_is_terminal(void)
{
	fixture_t f;

	fake_open(&f);
	f.reader->salvage.enabled = true;
	f.fake.status = FSIMG_ECORRUPT;
	f.fake.seal = true;
	expect_entry(&f, "/a");
	expect_entry(&f, "/bad");
	expect_failure(&f, FSIMG_ESTATE, FSIMG_OP_METADATA,
	    "sealed stream cannot rewind", "", false);
	fixture_close(&f);
}

static void
test_scratch_frame_and_prepared_class_corruption_are_terminal(void)
{
	fixture_t f;

	for (unsigned salvage = 0; salvage < 2; salvage++) {
		for (unsigned class_byte = 0; class_byte < 2; class_byte++) {
			fake_open(&f);
			f.reader->salvage.enabled = salvage != 0;
			expect_entry(&f, "/a");
			corrupt_next_record(&f, class_byte != 0);
			expect_failure(&f, FSIMG_ECORRUPT,
			    class_byte ? FSIMG_OP_READER : FSIMG_OP_METADATA,
			    class_byte ? "invalid prepared directory record" :
			    "invalid metadata frame version or length", "", false);
			fixture_close(&f);
		}
	}
}

/* A namespace collision makes validation read the second prepared record.
 * Its scratch framing and class are not driver decode failures, even though
 * a class error has the same status/operation pair as image corruption.
 */
static void
test_corrupted_records_during_namespace_validation_are_terminal(void)
{
	fixture_t f;

	for (unsigned corruption = 1; corruption <= 2; corruption++) {
		fake_open(&f);
		f.reader->salvage.enabled = true;
		f.fake.collision = true;
		f.fake.corrupt_validation = corruption;
		expect_entry(&f, "/a");
		expect_entry(&f, "/bad");
		expect_failure(&f, FSIMG_ECORRUPT,
		    corruption == 2 ? FSIMG_OP_READER : FSIMG_OP_METADATA,
		    corruption == 2 ? "invalid prepared directory record" :
		    "invalid metadata frame version or length", "", false);
		fixture_close(&f);
	}
}

static void
test_strict_prepare_root_and_replay_errors_remain_terminal(void)
{
	fixture_t f;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		fake_open(&f);
		f.reader->salvage.enabled = false;
		f.fake.status = FSIMG_ECORRUPT;
		f.fake.root_failure = scenario == 1;
		f.fake.replay_failure = scenario == 2;
		f.fake.collision = scenario == 3;
		if (!f.fake.root_failure) {
			expect_entry(&f, "/a");
			if (!f.fake.replay_failure)
				expect_entry(&f, "/bad");
		}
		expect_failure(&f, FSIMG_ECORRUPT, FSIMG_OP_READER,
		    f.fake.collision ?
		    "duplicate test name: collides with entry 1 of its directory" :
		    "injected driver failure", f.fake.collision ? "/bad/inside" : "",
		    false);
		fixture_close(&f);
	}
}

/* All messages in this table were captured by extracting these mutations
 * with the strict reader. They are independent of salvage classification.
 */
typedef struct format_case {
	const char *format;
	const char *message;
	bool root_rejected_at_open;
	bool replay_loss;
} format_case_t;

static const format_case_t format_cases[] = {
	{ .format = "fat12", .message = "FAT long-name checksum or count mismatch" },
	{ .format = "fat16", .message = "FAT long-name checksum or count mismatch" },
	{ .format = "fat32", .message = "FAT long-name checksum or count mismatch" },
	{ .format = "exfat", .message = "invalid exFAT directory entry-set checksum",
	    .root_rejected_at_open = true },
	{ .format = "iso9660", .message = "Rock Ridge name holds a NUL or a slash" },
	{ .format = "qnx4", .message = "invalid QNX4 file name" },
	{ .format = "qnx6", .message = "invalid QNX6 file name" },
	{ .format = "ifs", .message = "invalid prepared ImageFS entry",
	    .replay_loss = true }
};

static unsigned
directory_children(int root, const char *name)
{
	struct dirent *entry;
	DIR *directory;
	unsigned children = 0;
	int fd;

	fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(fd != -1);
	directory = fdopendir(fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 &&
		    strcmp(entry->d_name, "..") != 0)
			children++;
	}
	CHECK(closedir(directory) == 0);
	return (children);
}

static void
expect_extracted_file(int root, const char *path, unsigned id)
{
	struct stat st;
	unsigned char bytes[8];
	int fd;

	fd = openat(root, path, O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 7);
	CHECK(read(fd, bytes, sizeof(bytes)) == 7);
	for (unsigned i = 0; i < 7; i++)
		CHECK(bytes[i] == 'A' + id);
	CHECK(close(fd) == 0);
}

static int
open_destination_parent(char *path)
{
	int parent;

	CHECK(mkdtemp(path) != NULL);
	parent = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent != -1);
	return (parent);
}

static void
check_format_extraction(const format_case_t *row, bool root, bool salvage)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_extract_options_t options;
	char directory[] = "salvage-format-XXXXXX";
	int parent, output;

	fixture_open(&f, row->format);
	if (row->replay_loss)
		fixture_reader(&f);
	damage_listing(&f, row->format, root);
	/* exFAT scans the root for its mandatory system entries at open.
	 * Opening stays strict, so this root mutation cannot reach salvage.
	 */
	if (root && row->root_rejected_at_open) {
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(error.status == FSIMG_ECORRUPT);
		CHECK(error.operation == FSIMG_OP_READER);
		CHECK(strcmp(error.message, row->message) == 0);
		CHECK(strcmp(error.path, "") == 0);
		CHECK(f.reader == NULL);
		fixture_close(&f);
		return;
	}
	if (f.reader == NULL)
		fixture_reader(&f);
	parent = open_destination_parent(directory);
	fsimg_extract_options_init(&options);
	if (salvage)
		options.flags |= FSIMG_EXTRACT_SALVAGE;

	if (!salvage) {
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(error.status == FSIMG_ECORRUPT);
		CHECK(error.operation == FSIMG_OP_READER);
		CHECK(strcmp(error.message, row->message) == 0);
		CHECK(strcmp(error.path, "out") == 0);
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		expect_losses(&f, 0, 0, NULL, 0);
	} else {
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    FSIMG_EPARTIAL);
		CHECK(error.status == FSIMG_EPARTIAL);
		CHECK(error.operation == FSIMG_OP_EXTRACT);
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		CHECK(f.reader->state == FSIMG_READER_DONE);
		output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(output != -1);
		if (root)
			CHECK(directory_children(output, ".") == 0);
		else {
			expect_extracted_file(output, "a/inner", 1);
			expect_extracted_file(output, "c/deep/leaf", 6);
			expect_extracted_file(output, "z", 7);
			CHECK(directory_children(output, "bad") == 0);
		}
		CHECK(close(output) == 0);
		expect_losses(&f, 1, 1, root ? "/" :
		    row->replay_loss ? "/bad/child" : "/bad",
		    row->replay_loss ? FSIMG_LOSS_ENTRY : FSIMG_LOSS_DIRECTORY);
		expect_done(&f);
	}
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

static void
test_format_extraction_retains_good_siblings_and_an_empty_bad_directory(void)
{
	for (size_t i = 0; i < sizeof(format_cases) / sizeof(format_cases[0]); i++) {
		const format_case_t *row = &format_cases[i];

		for (unsigned salvage = 0; salvage < 2; salvage++) {
			check_format_extraction(row, false, salvage != 0);
			/* ImageFS has no root decode at iteration: open built the
			 * complete tree, including the implicit root, from its table.
			 */
			if (!row->replay_loss)
				check_format_extraction(row, true, salvage != 0);
		}
	}
}

/* Both root siblings are directories whose only child's LFN is damaged.
 * Salvage must retain the directories even though neither yields a child.
 */
static void
test_all_sibling_listings_rejected_retains_directories_and_returns_partial(void)
{
	static const unsigned char inner[] = { 'i', 0, 'n', 0, 'n', 0, 'e', 0, 'r', 0 };
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	unsigned char *name;
	char directory[] = "salvage-all-rejected-XXXXXX";
	int parent, output;

	/* The first four fixture paths contain /a and /bad and their files. */
	fixture_open_prefix(&f, "fat12", 4);
	name = find_bytes(&f, inner, sizeof(inner));
	name[-1] = 0x42;
	damage_listing(&f, "fat12", false);
	fixture_reader(&f);
	parent = open_destination_parent(directory);
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SALVAGE;

	CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
	    FSIMG_EPARTIAL);
	CHECK(error.status == FSIMG_EPARTIAL);
	CHECK(error.operation == FSIMG_OP_EXTRACT);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(f.reader->state == FSIMG_READER_DONE);
	output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(output != -1);
	CHECK(directory_children(output, ".") == 2);
	CHECK(directory_children(output, "a") == 0);
	CHECK(directory_children(output, "bad") == 0);
	expect_losses(&f, 2, 2, "/bad", FSIMG_LOSS_DIRECTORY);
	expect_done(&f);

	CHECK(close(output) == 0);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

static void
test_salvage_rejects_selection_and_manifest_before_creating_destination(void)
{
	static const char *const selection[] = { "/a" };
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_extract_result_t result;
	fsimg_error_t error;
	struct stat st;
	char directory[] = "salvage-options-XXXXXX";
	int parent, root, manifest;

	parent = open_destination_parent(directory);
	manifest = openat(parent, "manifest", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(manifest != -1);
	CHECK(unlinkat(parent, "manifest", 0) == 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned into = 0; into < 2; into++) {
			fixture_open(&f, "fat12");
			fixture_reader(&f);
			fsimg_extract_options_init(&options);
			options.flags |= FSIMG_EXTRACT_SALVAGE;
			if (variant == 0) {
				options.selections = selection;
				options.selection_count = 1;
			} else
				options.manifest_fd = manifest;
			if (into) {
				/* The coordinator normally supplies this private root. */
				CHECK(mkdirat(parent, "root", 0700) == 0);
				root = openat(parent, "root", O_RDONLY | O_DIRECTORY);
				CHECK(root != -1);
				CHECK(fsimg_extract_into(f.reader, root, &options, &result,
				    &error) == FSIMG_EINVAL);
				CHECK(directory_children(root, ".") == 0);
				CHECK(close(root) == 0);
				CHECK(unlinkat(parent, "root", AT_REMOVEDIR) == 0);
			} else {
				CHECK(fsimg_extract_at(f.reader, parent, "out", &options,
				    &error) == FSIMG_EINVAL);
				CHECK(fstatat(parent, "out", &st, AT_SYMLINK_NOFOLLOW) == -1);
				CHECK(errno == ENOENT);
			}
			CHECK(error.status == FSIMG_EINVAL);
			expect_losses(&f, 0, 0, NULL, 0);
			fixture_close(&f);
		}
	}
	CHECK(fstat(manifest, &st) == 0 && st.st_size == 0);
	CHECK(close(manifest) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_clean_salvage_succeeds_and_into_reports_per_operation_losses(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_extract_result_t result;
	fsimg_error_t error;
	char directory[] = "salvage-into-XXXXXX";
	int parent, root;

	parent = open_destination_parent(directory);
	for (unsigned damaged = 0; damaged < 2; damaged++) {
		fixture_open(&f, "fat12");
		if (damaged)
			damage_listing(&f, "fat12", false);
		fixture_reader(&f);
		CHECK(mkdirat(parent, "root", 0700) == 0);
		root = openat(parent, "root", O_RDONLY | O_DIRECTORY);
		CHECK(root != -1);
		fsimg_extract_options_init(&options);
		options.flags |= FSIMG_EXTRACT_SALVAGE;
		CHECK(fsimg_extract_into(f.reader, root, &options, &result, &error) ==
		    (damaged ? FSIMG_EPARTIAL : FSIMG_OK));
		CHECK(result.salvage_losses == damaged);
		CHECK(f.reader->state == FSIMG_READER_DONE);
		expect_losses(&f, damaged, damaged, "/bad", FSIMG_LOSS_DIRECTORY);
		expect_extracted_file(root, "a/inner", 1);
		expect_extracted_file(root, "c/deep/leaf", 6);
		expect_extracted_file(root, "z", 7);
		if (damaged) {
			CHECK(error.status == FSIMG_EPARTIAL);
			CHECK(error.operation == FSIMG_OP_EXTRACT);
			CHECK(directory_children(root, "bad") == 0);
		} else
			expect_extracted_file(root, "bad/child", 3);
		CHECK(close(root) == 0);
		remove_tree(parent, "root");
		fixture_close(&f);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_damaged_file_content_is_terminal_with_salvage_enabled(void)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_extract_options_t options;
	char directory[] = "salvage-content-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	for (unsigned salvage = 0; salvage < 2; salvage++) {
		fixture_open(&f, "fat12");
		damage_file(&f);
		fixture_reader(&f);
		fsimg_extract_options_init(&options);
		if (salvage)
			options.flags |= FSIMG_EXTRACT_SALVAGE;
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(error.status == FSIMG_ECORRUPT);
		CHECK(error.operation == FSIMG_OP_READER);
		CHECK(strcmp(error.message, "FAT file chain length differs from its size") == 0);
		CHECK(strcmp(error.path, "out") == 0);
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		expect_losses(&f, 0, 0, NULL, 0);
		remove_tree(parent, "out");
		fixture_close(&f);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_terminal_content_failure_takes_precedence_over_directory_loss(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	char directory[] = "salvage-terminal-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	fake_open(&f);
	f.fake.status = FSIMG_ECORRUPT;
	f.fake.content_failure = true;
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SALVAGE;
	CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) == FSIMG_EIO);
	CHECK(error.status == FSIMG_EIO && error.operation == FSIMG_OP_IO);
	CHECK(strcmp(error.message, "injected content I/O failure") == 0);
	CHECK(strcmp(error.path, "out") == 0);
	CHECK(f.reader->state == FSIMG_READER_FAILED);
	expect_losses(&f, 1, 1, "/bad", FSIMG_LOSS_DIRECTORY);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

int
main(void)
{
	CHECK(FSIMG_EPARTIAL == -14);
	CHECK(FSIMG_LOSS_EXAMPLES == 32u);
	CHECK(strcmp(fsimg_status_string(FSIMG_EPARTIAL), "incomplete extraction") == 0);
	test_bad_child_listing_preserves_parent_and_rewinds_records();
	test_bad_root_listing_finishes_with_one_loss();
	test_failed_replay_skips_entry_without_descending_and_renders_name();
	test_failed_replay_discards_partial_entry_and_link_content();
	test_rejecting_last_record_finishes_level_and_resumes_parent();
	test_loss_examples_are_capped_but_totals_are_exact();
	test_terminal_prepare_and_replay_errors_preserve_status_and_detail();
	test_failed_record_rewind_is_terminal();
	test_scratch_frame_and_prepared_class_corruption_are_terminal();
	test_corrupted_records_during_namespace_validation_are_terminal();
	test_strict_prepare_root_and_replay_errors_remain_terminal();
	test_format_extraction_retains_good_siblings_and_an_empty_bad_directory();
	test_all_sibling_listings_rejected_retains_directories_and_returns_partial();
	test_salvage_rejects_selection_and_manifest_before_creating_destination();
	test_clean_salvage_succeeds_and_into_reports_per_operation_losses();
	test_damaged_file_content_is_terminal_with_salvage_enabled();
	test_terminal_content_failure_takes_precedence_over_directory_loss();
	return (0);
}
