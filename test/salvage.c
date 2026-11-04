/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
	bool host_collision;
	int collision_parent;
	bool allocate;
	bool seal;
	bool omitted;
	bool symlink;
	unsigned repeated;
	unsigned descents;
	bool content_failure;
	uint64_t content_offset;
	fsimg_status_t resolve_failure;
	bool scratch_failure;
	bool output_failure;
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
	char loss_path[1024];
	char first_loss_path[1024];
	uint32_t first_loss_mask;
	char shortened_path[1024];
	uint32_t loss_mask;
	bool patterned;
	uint64_t fail_start;
	uint64_t fail_end;
	fsimg_status_t read_failure;
	unsigned shortened;
	unsigned skipped;
	unsigned collisions;
	bool capture_entries;
	unsigned entry_reports;
	char entry_paths[16][272];
	uint64_t entry_sizes[16];
	bool limit_output;
} fixture_t;

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	fixture_t *f = argument;

	if (f->read_failure != FSIMG_OK && offset < f->fail_end &&
	    (offset >= f->fail_start || length > f->fail_start - offset)) {
		*count = 0;
		return (fsimg_error_set(error, f->read_failure, 0, FSIMG_OP_IO,
		    "injected input failure"));
	}
	CHECK(offset <= f->length && length <= f->length - offset);
	bcopy(f->bytes + offset, buffer, length);
	if (f->limit_output) {
		struct rlimit limit;

		CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0);
		limit.rlim_cur = 0;
		CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
	}
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

	if ((mask & (FSIMG_LOSS_DIRECTORY | FSIMG_LOSS_ENTRY |
	    FSIMG_LOSS_SHORTENED | FSIMG_LOSS_SKIPPED | FSIMG_LOSS_COLLISION |
	    FSIMG_LOSS_UNSUPPORTED)) == 0)
		return;
	CHECK(strlen(path) < sizeof(f->loss_path));
	if (f->reports == 0) {
		strcpy(f->first_loss_path, path);
		f->first_loss_mask = mask;
	}
	f->reports++;
	f->shortened += mask == FSIMG_LOSS_SHORTENED;
	f->skipped += mask == FSIMG_LOSS_SKIPPED;
	f->collisions += mask == FSIMG_LOSS_COLLISION;

	strcpy(f->loss_path, path);
	if (mask == FSIMG_LOSS_SHORTENED)
		strcpy(f->shortened_path, path);
	f->loss_mask = mask;
}

static void
record_extracted_entry(void *argument, const fsimg_entry_t *entry)
{
	fixture_t *f = argument;
	unsigned index;

	if (!f->capture_entries)
		return;
	index = f->entry_reports++;

	CHECK(index < 16);
	CHECK(strlen(entry->path) < sizeof(f->entry_paths[index]));
	strcpy(f->entry_paths[index], entry->path);
	f->entry_sizes[index] = entry->size;
}

static fsimg_status_t
source_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* Provider IDs are indices into the entries[] passed to fixture_build. */
static fsimg_status_t
source_read(void *argument, uint64_t entry_index,
    uint64_t offset, void *buffer, size_t length, size_t *count,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;
	unsigned char *bytes = buffer;

	for (size_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)('A' + entry_index +
		    (f->patterned ? (offset + i) / 512 : 0));
	*count = length;
	return (FSIMG_OK);
}

static const char *const paths[] = {
	"/a", "/a/inner", "/bad", "/bad/child", "/c", "/c/deep",
	"/c/deep/leaf", "/z"
};

static void
fixture_build(fixture_t *f, const char *format, const fsimg_entry_t *entries,
    size_t count, bool patterned)
{
	fsimg_options_t options;
	fsimg_write_options_t write_options;
	fsimg_io_ops_t io_ops;
	fsimg_source_ops_t sources;
	fsimg_writer_t *writer;

	bzero(f, sizeof(*f));
	f->patterned = patterned;
	fsimg_options_init(&options);
	options.memory_bytes = 32u * 1024u * 1024u;
	options.entry = record_extracted_entry;
	options.entry_argument = f;
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
	if (patterned && strcmp(format, "ifs") != 0) {
		write_options.volume_bytes = 8u * 1024u * 1024u;
		if (strcmp(format, "iso9660") != 0)
			write_options.allocation_unit_bytes = 512;
	}
	CHECK(fsimg_writer_new(f->ctx, format, &write_options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_source_ops_init(&sources);
	sources.validate = source_validate;
	sources.read_at = source_read;
	CHECK(fsimg_writer_set_sources(writer, &sources, f, NULL) == FSIMG_OK);
	for (size_t i = 0; i < count; i++) {
		fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = i };

		CHECK(fsimg_writer_add(writer, &entries[i],
		    entries[i].kind == FSIMG_ENTRY_FILE ? &source : NULL,
		    NULL) == FSIMG_OK);
	}

	CHECK(fsimg_writer_emit(writer, f->io, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	f->reports = 0;
	f->entry_reports = 0;
}

static void
fixture_open_prefix(fixture_t *f, const char *format, size_t count)
{
	fsimg_entry_t entries[sizeof(paths) / sizeof(paths[0])];

	for (size_t i = 0; i < count; i++) {
		bool file = i == 1 || i == 3 || i == 6 || i == 7;

		entries[i] = (fsimg_entry_t){ .path = paths[i],
		    .kind = file ? FSIMG_ENTRY_FILE : FSIMG_ENTRY_DIRECTORY,
		    .size = file ? 7 : 0 };
	}
	fixture_build(f, format, entries, count, false);
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
	if (f->fake.host_collision && strcmp(name, "c") == 0) {
		int fd;

		/* Occupy the next host name after private-root validation, so
		 * the extractor's exclusive creation observes the collision.
		 */
		fd = openat(f->fake.collision_parent, "out/c",
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		CHECK(fd != -1);
		CHECK(write(fd, "occupied", 8) == 8);
		CHECK(close(fd) == 0);
	}
	reader->entry.kind = f->fake.symlink && strcmp(name, "bad") == 0 ?
	    FSIMG_ENTRY_SYMLINK : payload[0];
	if (reader->entry.kind == FSIMG_ENTRY_SYMLINK) {
		reader->entry.size = 6;
		return (fsimg_reader_set_link_target(reader, "target", 6, error));
	}
	if (reader->entry.kind == FSIMG_ENTRY_FILE)
		reader->entry.size = reader->initialized = 3;
	return (FSIMG_OK);
}

static fsimg_status_t
fake_resolve(fsimg_reader_t *reader, uint64_t offset FSIMG_UNUSED,
    fsimg_extent_t *extent, fsimg_error_t *error)
{
	fixture_t *f = reader->driver;

	if (strcmp(reader->entry.path, "/c") == 0) {
		if (f->fake.scratch_failure) {
			fsimg_extent_index_t *index;
			fsimg_status_t status;

			fsimg_test_fail_alloc_after(f->ctx, 1);
			status = fsimg_extent_index_new(&reader->session,
			    &reader->content_domain, &index, error);
			fsimg_test_fail_alloc_after(f->ctx, 0);
			CHECK(status == FSIMG_ENOMEM && index == NULL);
			return (status);
		}
		if (f->fake.resolve_failure != FSIMG_OK)
			return (fsimg_error_set(error, f->fake.resolve_failure, 0,
			    FSIMG_OP_READER, "injected resolve failure"));
		f->limit_output = f->fake.output_failure;
		if (f->fake.content_failure) {
			f->fail_end = 3;
			if (f->read_failure == FSIMG_OK)
				f->read_failure = FSIMG_EIO;
		}
	}
	*extent = (fsimg_extent_t){
	    .image_offset = strcmp(reader->entry.path, "/c") == 0 ?
	    f->fake.content_offset : 0, .length = 3 };
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
		if (f->loss_mask != mask)
			fprintf(stderr, "loss %s: expected mask %#x, got %#x\n",
			    path, mask, f->loss_mask);
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
test_skipped_unsupported_entries_are_losses_only_under_salvage(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_extract_result_t result;
	fsimg_error_t error;
	char directory[] = "salvage-unsupported-XXXXXX";
	int parent, root;

	parent = open_destination_parent(directory);
	for (unsigned omitted = 0; omitted < 2; omitted++) {
		for (unsigned salvage = 0; salvage < 2; salvage++) {
			fake_open(&f);
			f.fake.omitted = omitted != 0;
			f.fake.symlink = omitted == 0;
			CHECK(mkdirat(parent, "out", 0700) == 0);
			root = openat(parent, "out", O_RDONLY | O_DIRECTORY);
			CHECK(root != -1);
			fsimg_extract_options_init(&options);
			options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
			if (salvage)
				options.flags |= FSIMG_EXTRACT_SALVAGE;
			CHECK(fsimg_extract_into(f.reader, root, &options, &result, &error) ==
			    (salvage ? FSIMG_EPARTIAL : FSIMG_OK));
			CHECK(result.salvage_losses == salvage);
			CHECK(result.skipped_entries == (omitted ? 0 : 1));
			CHECK(result.omitted_entries == omitted);
			expect_losses(&f, salvage, salvage,
			    omitted ? "/\\xff" : "/bad", FSIMG_LOSS_UNSUPPORTED);
			CHECK(faccessat(root, "bad", F_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
			CHECK(close(root) == 0);
			remove_tree(parent, "out");
			fixture_close(&f);
		}
	}
	CHECK(close(parent) == 0 && rmdir(directory) == 0);
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
test_content_io_failure_is_terminal_with_salvage_enabled(void)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_extract_options_t options;
	char directory[] = "salvage-content-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	for (unsigned salvage = 0; salvage < 2; salvage++) {
		fake_open(&f);
		f.fake.content_failure = true;
		fsimg_extract_options_init(&options);
		if (salvage)
			options.flags |= FSIMG_EXTRACT_SALVAGE;
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    FSIMG_EIO);
		CHECK(error.status == FSIMG_EIO && error.operation == FSIMG_OP_IO);
		CHECK(strcmp(error.message, "injected input failure") == 0);
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
	CHECK(strcmp(error.message, "injected input failure") == 0);
	CHECK(strcmp(error.path, "out") == 0);
	CHECK(f.reader->state == FSIMG_READER_FAILED);
	expect_losses(&f, 1, 1, "/bad", FSIMG_LOSS_DIRECTORY);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

/* All file fixtures use 512-byte clusters. Each logical cluster has its own
 * byte, independent of the table walk and of physical allocation order.
 */
typedef struct file_image {
	bool exfat;
	uint64_t table;
	uint64_t table_bytes;
	uint64_t heap;
	uint32_t clusters;
	uint32_t first;
	unsigned copies;
	unsigned char *record;
} file_image_t;

typedef enum file_damage {
	FILE_CLEAN,
	FILE_FREE,
	FILE_BAD,
	FILE_OUTSIDE,
	FILE_SHORT,
	FILE_CYCLE,
	FILE_BITMAP,
	FILE_TRUNCATED,
	FILE_ZERO_TAIL,
	FILE_TAIL_TRUNCATED,
	FILE_TAIL_SHORT,
	FILE_TAIL_BAD,
	FILE_HUGE,
	FILE_BUDGET,
	FILE_COLLISION,
	FILE_DIRECTORY_COLLISION,
	FILE_HOST_COLLISION,
	FILE_LATER_COLLISION,
	FILE_LATER_DIRECTORY_COLLISION,
	FILE_LONG_NAME
} file_damage_t;

typedef struct file_case {
	const char *name;
	file_damage_t damage;
	unsigned size_clusters;
	/* Indices in the logical chain, not on-disk cluster numbers. */
	unsigned damaged_link_index;
	unsigned link_target_index;
	size_t prefix;
	bool contiguous;
	bool exfat_only;
	fsimg_status_t strict_fat;
	fsimg_status_t strict_exfat;
	fsimg_status_t salvage_status;
	uint32_t loss_mask;
	unsigned loss_count;
} file_case_t;

static const file_case_t file_cases[] = {
	{ .name = "clean", .size_clusters = 9, .prefix = 4608,
	    .strict_fat = FSIMG_OK, .strict_exfat = FSIMG_OK,
	    .salvage_status = FSIMG_OK,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "free-third", .damage = FILE_FREE, .size_clusters = 5,
	    .damaged_link_index = 2, .prefix = 1024,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "bad-third", .damage = FILE_BAD, .size_clusters = 5,
	    .damaged_link_index = 2, .prefix = 1024,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "outside-third", .damage = FILE_OUTSIDE, .size_clusters = 5,
	    .damaged_link_index = 2, .prefix = 1024,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "free-first", .damage = FILE_FREE, .size_clusters = 5,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1 },
	{ .name = "bad-first", .damage = FILE_BAD, .size_clusters = 5,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1 },
	{ .name = "outside-first", .damage = FILE_OUTSIDE, .size_clusters = 5,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1 },
	{ .name = "short-chain", .damage = FILE_SHORT, .size_clusters = 5,
	    .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "cycle-size-five", .damage = FILE_CYCLE, .size_clusters = 5,
	    .damaged_link_index = 3, .link_target_index = 1, .prefix = 2048,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "cycle-size-seven", .damage = FILE_CYCLE, .size_clusters = 7,
	    .damaged_link_index = 3, .link_target_index = 1, .prefix = 2048,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "cycle-size-nine", .damage = FILE_CYCLE, .size_clusters = 9,
	    .damaged_link_index = 3, .link_target_index = 1, .prefix = 2048,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "self-cycle", .damage = FILE_CYCLE, .size_clusters = 2,
	    .prefix = 512,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "cycle-to-first", .damage = FILE_CYCLE, .size_clusters = 6,
	    .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "whole-before-cycle", .damage = FILE_CYCLE, .size_clusters = 3,
	    .damaged_link_index = 3, .link_target_index = 1, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_OK,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "whole-before-end", .size_clusters = 3, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_OK,
	    .salvage_status = FSIMG_OK,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "bitmap-chained", .damage = FILE_BITMAP, .size_clusters = 5,
	    .damaged_link_index = 2, .prefix = 1024, .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "bitmap-contiguous", .damage = FILE_BITMAP,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1024,
	    .contiguous = true, .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "bitmap-first", .damage = FILE_BITMAP,
	    .size_clusters = 5, .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1 },
	{ .name = "zero-tail", .damage = FILE_ZERO_TAIL, .size_clusters = 5,
	    .prefix = 2560, .contiguous = true, .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_OK,
	    .salvage_status = FSIMG_OK,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "truncated-zero-tail", .damage = FILE_TAIL_TRUNCATED,
	    .size_clusters = 5, .prefix = 2560, .contiguous = true,
	    .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_OK,
	    .salvage_status = FSIMG_OK,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "short-chain-zero-tail", .damage = FILE_TAIL_SHORT,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "bad-chain-zero-tail", .damage = FILE_TAIL_BAD,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1024,
	    .exfat_only = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "truncated-run", .damage = FILE_TRUNCATED,
	    .size_clusters = 9, .prefix = 2 * 512 + 137, .contiguous = true,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 2 },
	{ .name = "truncated-fragments", .damage = FILE_TRUNCATED,
	    .size_clusters = 9, .prefix = 2 * 512 + 137,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 2 },
	{ .name = "huge-short-chain", .damage = FILE_HUGE,
	    .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ELIMIT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1 },
	{ .name = "clean-over-budget", .damage = FILE_BUDGET,
	    .size_clusters = 9, .prefix = 4608,
	    .strict_fat = FSIMG_ELIMIT, .strict_exfat = FSIMG_ELIMIT,
	    .salvage_status = FSIMG_ELIMIT,
	    .loss_mask = 0, .loss_count = 0 },
	{ .name = "occupied-partial", .damage = FILE_COLLISION,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_COLLISION, .loss_count = 1 },
	{ .name = "directory-partial", .damage = FILE_DIRECTORY_COLLISION,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_COLLISION, .loss_count = 1 },
	{ .name = "host-equivalent-partial", .damage = FILE_HOST_COLLISION,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_COLLISION, .loss_count = 1 },
	{ .name = "later-clean-partial", .damage = FILE_LATER_COLLISION,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_COLLISION, .loss_count = 2 },
	{ .name = "later-directory-partial", .damage = FILE_LATER_DIRECTORY_COLLISION,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_COLLISION, .loss_count = 2 },
	{ .name = "partial-name-too-long", .damage = FILE_LONG_NAME,
	    .size_clusters = 5, .damaged_link_index = 2, .prefix = 1536,
	    .strict_fat = FSIMG_ECORRUPT, .strict_exfat = FSIMG_ECORRUPT,
	    .salvage_status = FSIMG_EPARTIAL,
	    .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1 }
};

static uint64_t
file_cluster_offset(const file_image_t *image, uint32_t cluster)
{
	return (image->heap + (uint64_t)(cluster - 2) * 512);
}

static void
file_table_set(fixture_t *f, const file_image_t *image, uint32_t cluster,
    uint32_t value)
{
	for (unsigned copy = 0; copy < image->copies; copy++) {
		unsigned char *slot = f->bytes + image->table +
		    copy * image->table_bytes + cluster * (image->exfat ? 4 : 2);

		if (image->exfat)
			fsimg_store_le32(slot, value);
		else
			fsimg_store_le16(slot, (uint16_t)value);
	}
}

static void
file_set_checksum(unsigned char *set)
{
	uint16_t checksum = 0;
	size_t length = ((size_t)set[1] + 1) * 32;

	for (size_t i = 0; i < length; i++) {
		if (i != 2 && i != 3)
			checksum = (uint16_t)((checksum >> 1) |
			    (checksum << 15)) + set[i];
	}
	fsimg_store_le16(set + 2, checksum);
}

/* Locate the writer's unique nine-cluster file, scanning directory slots.
 * No other fixture entry has this size; payload clusters contain one byte.
 */
static void
file_image_layout(fixture_t *f, const char *format, file_image_t *image)
{
	uint32_t sector, root_sectors;
	unsigned char *boot = f->bytes;

	bzero(image, sizeof(*image));
	image->exfat = strcmp(format, "exfat") == 0;
	if (image->exfat) {
		sector = 1u << boot[108];
		CHECK(sector == 512 && boot[109] == 0);
		image->table = (uint64_t)fsimg_load_le32(boot + 80) * sector;
		image->table_bytes = (uint64_t)fsimg_load_le32(boot + 84) * sector;
		image->heap = (uint64_t)fsimg_load_le32(boot + 88) * sector;
		image->clusters = fsimg_load_le32(boot + 92);
		image->copies = boot[110];
	} else {
		sector = fsimg_load_le16(boot + 11);
		CHECK(sector == 512 && boot[13] == 1);
		image->table = (uint64_t)fsimg_load_le16(boot + 14) * sector;
		image->table_bytes = (uint64_t)fsimg_load_le16(boot + 22) * sector;
		image->copies = boot[16];
		root_sectors = (fsimg_load_le16(boot + 17) * 32 + sector - 1) /
		    sector;
		image->heap = image->table + image->copies * image->table_bytes +
		    root_sectors * sector;
		image->clusters = (uint32_t)((f->length - image->heap) / 512);
	}
	for (size_t i = (size_t)image->heap; i + 96 <= f->length; i += 32) {
		unsigned char *slot = f->bytes + i;

		if (image->exfat) {
			if (slot[0] != 0x85 || slot[32] != 0xc0 ||
			    fsimg_load_le64(slot + 56) != 4608)
				continue;
			image->first = fsimg_load_le32(slot + 52);
		} else {
			if ((slot[11] & 0x18) != 0 || fsimg_load_le32(slot + 28) != 4608)
				continue;
			image->first = fsimg_load_le16(slot + 26);
		}
		image->record = slot;
		return;
	}
	CHECK(false);
}

static void
file_bitmap_set(fixture_t *f, const file_image_t *image, uint32_t cluster,
    bool allocated)
{
	uint32_t root = fsimg_load_le32(f->bytes + 96);
	unsigned char *directory = f->bytes + file_cluster_offset(image, root);
	unsigned char *bitmap;

	for (unsigned i = 0; i < 512; i += 32) {
		if (directory[i] != 0x81)
			continue;
		bitmap = f->bytes + file_cluster_offset(image,
		    fsimg_load_le32(directory + i + 20));
		if (allocated)
			bitmap[(cluster - 2) / 8] |=
			    (unsigned char)(1u << ((cluster - 2) % 8));
		else
			bitmap[(cluster - 2) / 8] &=
			    (unsigned char)~(1u << ((cluster - 2) % 8));
		return;
	}
	CHECK(false);
}

static bool
file_has_zero_tail(const file_case_t *row)
{
	return (row->damage == FILE_ZERO_TAIL ||
	    row->damage == FILE_TAIL_TRUNCATED ||
	    row->damage == FILE_TAIL_SHORT || row->damage == FILE_TAIL_BAD);
}

static bool
file_has_collision(const file_case_t *row)
{
	return (row->damage == FILE_COLLISION ||
	    row->damage == FILE_DIRECTORY_COLLISION ||
	    row->damage == FILE_HOST_COLLISION ||
	    row->damage == FILE_LATER_COLLISION ||
	    row->damage == FILE_LATER_DIRECTORY_COLLISION);
}

/* Make physical adjacency independent of logical order. The table is also
 * populated for NoFatChain streams so chained and contiguous cases share data.
 */
static void
file_image_damage(fixture_t *f, file_image_t *image, const file_case_t *row)
{
	static const unsigned fragments[] = { 0, 2, 4, 6, 8, 1, 3, 5, 7 };
	uint32_t clusters[9], value, end = image->exfat ? UINT32_MAX : 0xffff;
	uint64_t size = (uint64_t)row->size_clusters * 512;
	unsigned char *record = image->record;

	for (unsigned i = 0; i < 9; i++)
		clusters[i] = image->first + (row->contiguous ? i : fragments[i]);
	for (unsigned i = 0; i < 9; i++) {
		file_table_set(f, image, clusters[i], i == 8 ? end : clusters[i + 1]);
		/* entries[2] supplies this file: one byte per logical cluster. */
		memset(f->bytes + file_cluster_offset(image, clusters[i]),
		    'A' + 2 + i, 512);
	}
	if (row->damage == FILE_HUGE)
		size = (uint64_t)image->clusters * 512;
	switch (row->damage) {
	case FILE_FREE:
	case FILE_TAIL_BAD:
		file_table_set(f, image, clusters[row->damaged_link_index], 0);
		break;
	case FILE_BAD:
		value = image->exfat ? UINT32_C(0xfffffff7) : 0xfff7;
		file_table_set(f, image, clusters[row->damaged_link_index], value);
		break;
	case FILE_OUTSIDE:
		file_table_set(f, image, clusters[row->damaged_link_index], image->clusters + 2);
		break;
	case FILE_SHORT:
	case FILE_TAIL_SHORT:
	case FILE_HUGE:
	case FILE_COLLISION:
	case FILE_DIRECTORY_COLLISION:
	case FILE_HOST_COLLISION:
	case FILE_LATER_COLLISION:
	case FILE_LATER_DIRECTORY_COLLISION:
	case FILE_LONG_NAME:
		file_table_set(f, image, clusters[row->damaged_link_index], end);
		break;
	case FILE_CYCLE:
		file_table_set(f, image, clusters[row->damaged_link_index], clusters[row->link_target_index]);
		break;
	case FILE_BITMAP:
		file_bitmap_set(f, image, clusters[row->damaged_link_index], false);
		break;
	case FILE_TRUNCATED:
	case FILE_TAIL_TRUNCATED:
		f->length = (size_t)file_cluster_offset(image, clusters[2]) + 137;
		break;
	default:
		break;
	}
	if (image->exfat) {
		record[33] = row->contiguous ? 3 : 1;
		fsimg_store_le64(record + 40, file_has_zero_tail(row) ? 700 : size);
		fsimg_store_le64(record + 56, size);
		file_set_checksum(record);
	} else
		fsimg_store_le32(record + 28, (uint32_t)size);
	if (row->damage == FILE_TRUNCATED || row->damage == FILE_TAIL_TRUNCATED)
		CHECK(fsimg_io_resize(f->io, f->length, NULL) == FSIMG_OK);
}

static void
expect_absent(int root, const char *name)
{
	struct stat st;

	CHECK(fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(errno == ENOENT);
}

static void
expect_pattern_file(int root, const char *name, size_t length, unsigned entry_index,
    size_t initialized)
{
	struct stat st;
	unsigned char bytes[512];
	size_t offset = 0, piece;
	int fd;

	fd = openat(root, name, O_RDONLY | O_NOFOLLOW);
	if (fd == -1)
		fprintf(stderr, "expected file %s (%zu bytes): %s\n",
		    name, length, strerror(errno));
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
	CHECK(st.st_size == (off_t)length);
	while (offset < length) {
		piece = length - offset < sizeof(bytes) ? length - offset : sizeof(bytes);
		CHECK(read(fd, bytes, piece) == (ssize_t)piece);
		for (size_t i = 0; i < piece; i++) {
			unsigned char expected = offset + i >= initialized ? 0 :
			    (unsigned char)('A' + entry_index + (offset + i) / 512);

			CHECK(bytes[i] == expected);
		}
		offset += piece;
	}
	CHECK(read(fd, bytes, 1) == 0);
	CHECK(close(fd) == 0);
}

/* Both records have one name slot. Swap entire sets, including their checksum
 * or short-name checksum, to put the occupied name before the damaged file.
 */
static void
file_collision_order(fixture_t *f, const file_image_t *image,
    const file_case_t *row)
{
	enum {
		DIRECTORY_SLOT_BYTES = 32,
		EXFAT_SET_BYTES = 3 * DIRECTORY_SLOT_BYTES,
		FAT_SET_BYTES = 2 * DIRECTORY_SLOT_BYTES,
		EXFAT_NAME_OFFSET = 2 * DIRECTORY_SLOT_BYTES + 2,
		FAT_LONG_NAME_OFFSET = 1
	};
	unsigned char saved[EXFAT_SET_BYTES], wide[24] = { 0 };
	unsigned char *other, *file;
	const char *name = row->damage == FILE_HOST_COLLISION ?
	    "FOO.PARTIAL" : "foo.partial";
	size_t length = image->exfat ? EXFAT_SET_BYTES : FAT_SET_BYTES;

	for (size_t i = 0; i < strlen(name); i++)
		wide[i * 2] = (unsigned char)name[i];
	other = find_bytes(f, wide, image->exfat ? strlen(name) * 2 : 10);
	other -= image->exfat ? EXFAT_NAME_OFFSET : FAT_LONG_NAME_OFFSET;
	file = image->record - (image->exfat ? 0 : DIRECTORY_SLOT_BYTES);
	if ((other < file) == (row->damage != FILE_LATER_COLLISION &&
	    row->damage != FILE_LATER_DIRECTORY_COLLISION))
		return;
	bcopy(file, saved, length);
	bcopy(other, file, length);
	bcopy(saved, other, length);
}

static bool
host_ignores_case(int parent)
{
	struct stat st;
	bool equivalent;
	int fd;

	fd = openat(parent, "case-probe", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	equivalent = fstatat(parent, "CASE-PROBE", &st, AT_SYMLINK_NOFOLLOW) == 0;
	CHECK(equivalent || errno == ENOENT);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(parent, "case-probe", 0) == 0);
	return (equivalent);
}

static void
build_file_case(fixture_t *f, file_image_t *image, const char *format,
    const file_case_t *row, const char *path, const char *occupied)
{
	bool directory = row->damage == FILE_DIRECTORY_COLLISION ||
	    row->damage == FILE_LATER_DIRECTORY_COLLISION;
	size_t count = file_has_collision(row) ? 6 : 5;

	/* /b is allocated before foo, below its truncated zero tail.
	 * /z lies beyond a truncated initialized run and is also skipped.
	 */
	fsimg_entry_t entries[] = {
		{ .path = "/a", .kind = FSIMG_ENTRY_FILE, .size = 512 },
		{ .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = path, .kind = FSIMG_ENTRY_FILE, .size = 4608 },
		{ .path = "/empty", .kind = FSIMG_ENTRY_FILE },
		{ .path = row->damage == FILE_TAIL_TRUNCATED ? "/b" :
		    "/z", .kind = FSIMG_ENTRY_FILE, .size = 512 },
		{ .path = occupied, .kind = directory ? FSIMG_ENTRY_DIRECTORY :
		    FSIMG_ENTRY_FILE, .size = directory ? 0 : 512 },
		{ .path = "/dir/foo.partial/child", .kind = FSIMG_ENTRY_FILE,
		    .size = 512 }
	};

	if (row->damage == FILE_LATER_DIRECTORY_COLLISION)
		count++;
	fixture_build(f, format, entries, count, true);
	file_image_layout(f, format, image);
	file_image_damage(f, image, row);
	if (file_has_collision(row))
		file_collision_order(f, image, row);
	fixture_reader(f);
}

static void
check_completed_file_case(fixture_t *f, const file_case_t *row, int output,
    const char *path, const char *partial, const char *occupied,
    fsimg_status_t expected, const fsimg_error_t *error)
{
	bool truncated = row->damage == FILE_TRUNCATED;
	bool damaged = row->prefix < (size_t)row->size_clusters * 512 ||
	    row->damage == FILE_HUGE;
	bool collision = file_has_collision(row);
	bool later = row->damage == FILE_LATER_COLLISION ||
	    row->damage == FILE_LATER_DIRECTORY_COLLISION;
	bool skipped = row->prefix == 0 || (collision && !later) ||
	    row->damage == FILE_LONG_NAME;

	expect_pattern_file(output, "a", 512, 0, 512);
	expect_pattern_file(output, "empty", 0, 3, 0);
	if (truncated)
		expect_absent(output, "z");
	else
		expect_pattern_file(output, row->damage == FILE_TAIL_TRUNCATED ?
		    "b" : "z", 512, 4, 512);
	if (!damaged) {
		expect_pattern_file(output, path + 1, row->prefix, 2,
		    file_has_zero_tail(row) ? 700 : row->prefix);
		expect_absent(output, partial);
	} else {
		expect_absent(output, path + 1);
		if (!skipped)
			expect_pattern_file(output, partial, row->prefix, 2,
			    file_has_zero_tail(row) ? 700 : row->prefix);
		else if (!collision && row->damage != FILE_LONG_NAME)
			expect_absent(output, partial);
	}
	if (later) {
		expect_absent(output, "child");
		CHECK(strcmp(f->first_loss_path, path) == 0);
		CHECK(f->first_loss_mask == FSIMG_LOSS_SHORTENED);
	} else if (collision) {
		if (row->damage == FILE_DIRECTORY_COLLISION)
			CHECK(directory_children(output, occupied + 1) == 0);
		else
			expect_pattern_file(output, occupied + 1, 512, 5, 512);
	}
	CHECK(directory_children(output, "dir") ==
	    (collision || !skipped ? 1u : 0u));
	expect_losses(f, row->loss_count, row->loss_count,
	    truncated ? "/z" : later ? occupied : path, row->loss_mask);
	CHECK(f->shortened == (unsigned)(damaged && !skipped));
	if (f->shortened != 0)
		CHECK(strcmp(f->shortened_path, path) == 0);
	CHECK(f->skipped == (unsigned)truncated +
	    (unsigned)(damaged && skipped && !collision));
	CHECK(f->collisions == (unsigned)collision);
	CHECK(f->reader->state == FSIMG_READER_DONE);
	if (expected == FSIMG_EPARTIAL)
		CHECK(error->output_state == FSIMG_OUTPUT_INCOMPLETE);
	if (row->damage == FILE_HUGE)
		CHECK(f->reader->session.output_bytes == row->prefix + 1024);
}

static void
check_failed_file_case(fixture_t *f, const file_image_t *image,
    const file_case_t *row, int output, const char *path, const char *partial,
    const char *occupied, bool salvage, fsimg_status_t expected,
    const fsimg_error_t *error)
{
	bool collision = file_has_collision(row);

	CHECK(error->status == expected);
	if (!salvage) {
		struct stat st;
		int found;

		/* Strict copying may retain a pre-sized incomplete file. */
		found = fstatat(output, path + 1, &st, AT_SYMLINK_NOFOLLOW);
		CHECK(found == 0 || errno == ENOENT);
		if (found == 0) {
			CHECK(S_ISREG(st.st_mode));
			CHECK(st.st_size == (off_t)(row->damage == FILE_HUGE ?
			    (uint64_t)image->clusters * 512 :
			    (uint64_t)row->size_clusters * 512));
		}
	}
	CHECK(f->reader->state == FSIMG_READER_FAILED);
	if (!salvage || row->damage == FILE_BUDGET) {
		if (collision && row->damage != FILE_LATER_COLLISION &&
		    row->damage != FILE_LATER_DIRECTORY_COLLISION) {
			if (row->damage == FILE_DIRECTORY_COLLISION)
				CHECK(directory_children(output, occupied + 1) == 0);
			else
				expect_pattern_file(output, occupied + 1, 512, 5, 512);
		} else if (row->damage != FILE_LONG_NAME)
			expect_absent(output, partial);
		expect_losses(f, 0, 0, NULL, 0);
	} else {
		expect_pattern_file(output, partial, row->prefix, 2, row->prefix);
		expect_losses(f, 1, 1, path, FSIMG_LOSS_SHORTENED);
	}
}

static void
check_file_mode(const char *format, const file_case_t *row, int parent,
    const char *path, const char *occupied, bool salvage,
    fsimg_status_t expected)
{
	fixture_t f;
	file_image_t image;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	char partial[272];
	int output;

	CHECK(snprintf(partial, sizeof(partial), "%s.partial", path + 1) <
	    (int)sizeof(partial));
	build_file_case(&f, &image, format, row, path, occupied);
	fsimg_extract_options_init(&options);
	if (salvage)
		options.flags |= FSIMG_EXTRACT_SALVAGE;
	if (row->damage == FILE_HUGE)
		options.max_output_bytes = row->prefix + 1024;
	if (row->damage == FILE_BUDGET)
		options.max_output_bytes = 1000;
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	if (status != expected)
		fprintf(stderr, "%s/%s %s: expected %s, got %s: %s\n",
		    format, row->name, salvage ? "salvage" : "strict",
		    fsimg_status_string(expected), fsimg_status_string(status),
		    error.message);
	CHECK(status == expected);
	output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(output != -1);
	if (expected == FSIMG_OK || expected == FSIMG_EPARTIAL)
		check_completed_file_case(&f, row, output, path, partial, occupied,
		    expected, &error);
	else
		check_failed_file_case(&f, &image, row, output, path, partial,
		    occupied, salvage, expected, &error);
	CHECK(close(output) == 0);
	remove_tree(parent, "out");
	fixture_close(&f);
}

static void
check_file_strict(const char *format, const file_case_t *row, int parent,
    const char *path, const char *occupied)
{
	fsimg_status_t expected = strcmp(format, "exfat") == 0 ?
	    row->strict_exfat : row->strict_fat;

	check_file_mode(format, row, parent, path, occupied, false, expected);
}

static void
check_file_salvage(const char *format, const file_case_t *row, int parent,
    const char *path, const char *occupied)
{
	check_file_mode(format, row, parent, path, occupied, true,
	    row->salvage_status);
}

static void
check_file_extraction(const char *format, const file_case_t *row)
{
	char directory[] = "salvage-file-XXXXXX";
	char long_path[256];
	const char *path = "/dir/foo";
	const char *occupied = row->damage == FILE_HOST_COLLISION ?
	    "/dir/FOO.PARTIAL" : "/dir/foo.partial";
	int parent;

	if (row->exfat_only && strcmp(format, "exfat") != 0)
		return;
	fprintf(stderr, "file salvage: %s/%s\n", format, row->name);
	parent = open_destination_parent(directory);
	if (row->damage == FILE_HOST_COLLISION && !host_ignores_case(parent)) {
		fprintf(stderr, "SKIP host-equivalent collision: case-sensitive host\n");
		CHECK(close(parent) == 0);
		CHECK(rmdir(directory) == 0);
		return;
	}
	if (row->damage == FILE_LONG_NAME) {
		bcopy("/dir/", long_path, 5);
		memset(long_path + 5, 'n', 250);
		long_path[255] = '\0';
		path = long_path;
	}
	check_file_strict(format, row, parent, path, occupied);
	check_file_salvage(format, row, parent, path, occupied);
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_file_salvage_prefix_bytes_partial_names_collisions_and_output_limits(void)
{
	static const char *const formats[] = { "fat16", "exfat" };

	for (size_t i = 0; i < sizeof(file_cases) / sizeof(file_cases[0]); i++) {
		if (file_cases[i].damage == FILE_LATER_COLLISION ||
		    file_cases[i].damage == FILE_LATER_DIRECTORY_COLLISION)
			continue;
		for (size_t j = 0; j < sizeof(formats) / sizeof(formats[0]); j++)
			check_file_extraction(formats[j], &file_cases[i]);
	}
}

static void
test_resolve_corruption_skips_file_before_creation(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	char directory[] = "salvage-resolve-XXXXXX";
	int parent, output;

	parent = open_destination_parent(directory);
	fake_open(&f);
	f.fake.resolve_failure = FSIMG_ECORRUPT;
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SALVAGE;
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	if (status != FSIMG_EPARTIAL)
		fprintf(stderr, "resolve corruption: expected incomplete extraction, "
		    "got %s: %s\n", fsimg_status_string(status), error.message);
	CHECK(status == FSIMG_EPARTIAL);
	expect_losses(&f, 1, 1, "/c", FSIMG_LOSS_SKIPPED);
	output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(output != -1);
	expect_absent(output, "c");
	expect_absent(output, "c.partial");
	CHECK(directory_children(output, ".") == 2);
	CHECK(close(output) == 0);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

static void
test_file_pass_resource_and_state_failures_are_terminal(void)
{
	static const fsimg_status_t failures[] = {
		FSIMG_ENOMEM, FSIMG_ESTATE
	};
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	char directory[] = "salvage-pass-failure-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		fake_open(&f);
		f.fake.scratch_failure = failures[i] == FSIMG_ENOMEM;
		if (!f.fake.scratch_failure)
			f.fake.resolve_failure = failures[i];
		fsimg_extract_options_init(&options);
		options.flags |= FSIMG_EXTRACT_SALVAGE;
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    failures[i]);
		CHECK(error.status == failures[i]);
		CHECK(error.operation == (f.fake.scratch_failure ? FSIMG_OP_ALLOCATE :
		    FSIMG_OP_READER));
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		expect_losses(&f, 0, 0, NULL, 0);
		remove_tree(parent, "out");
		fixture_close(&f);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_output_write_failure_after_content_read_is_terminal(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	struct rlimit limit;
	void (*handler)(int);
	char directory[] = "salvage-output-failure-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	fake_open(&f);
	f.fake.output_failure = true;
	CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0);
	handler = signal(SIGXFSZ, SIG_IGN);
	CHECK(handler != SIG_ERR);
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SALVAGE;
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
	CHECK(signal(SIGXFSZ, handler) != SIG_ERR);
	CHECK(status == FSIMG_EIO && error.status == FSIMG_EIO);
	CHECK(error.operation == FSIMG_OP_IO);
	CHECK(error.os_error == EFBIG);
	CHECK(f.reader->state == FSIMG_READER_FAILED);
	expect_losses(&f, 0, 0, NULL, 0);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

static void
test_table_walk_changed_input_and_provider_io_failures_are_terminal(void)
{
	static const char *const formats[] = { "fat16", "exfat" };
	static const fsimg_entry_t entries[] = {
		{ .path = "/a", .kind = FSIMG_ENTRY_FILE, .size = 512 },
		{ .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/dir/foo", .kind = FSIMG_ENTRY_FILE, .size = 4608 },
		{ .path = "/z", .kind = FSIMG_ENTRY_FILE, .size = 512 }
	};
	fixture_t f;
	file_image_t image;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	char directory[] = "salvage-provider-failure-XXXXXX";
	int parent;

	parent = open_destination_parent(directory);
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		/* The changed-input case fails in the table walk, before copying. */
		for (unsigned failure_case = 0; failure_case < 3; failure_case++) {
			fsimg_status_t expected = failure_case == 2 ?
			    FSIMG_ESOURCE_CHANGED : FSIMG_EIO;

			fixture_build(&f, formats[i], entries,
			    sizeof(entries) / sizeof(entries[0]), true);
			file_image_layout(&f, formats[i], &image);
			/* A separate table page leaves directory walks readable and
			 * requires file planning to call the input provider.
			 */
			image.first = 3000;
			CHECK(image.first + 9 < image.clusters);
			if (image.exfat) {
				fsimg_store_le32(image.record + 52, image.first);
				for (unsigned j = 0; j < 9; j++)
					file_bitmap_set(&f, &image, image.first + j, true);
			} else
				fsimg_store_le16(image.record + 26, (uint16_t)image.first);
			file_image_damage(&f, &image, &file_cases[0]);
			fixture_reader(&f);
			f.read_failure = expected;
			if (failure_case == 1) {
				f.fail_start = file_cluster_offset(&image, image.first);
				f.fail_end = f.fail_start + 9 * 512;
			} else {
				f.fail_start = image.table +
				    (image.first * (image.exfat ? 4 : 2) / 4096) * 4096;
				f.fail_end = f.fail_start + 4096;
			}
			fsimg_extract_options_init(&options);
			options.flags |= FSIMG_EXTRACT_SALVAGE;
			CHECK(fsimg_extract_at(f.reader, parent, "out", &options,
			    &error) == expected);
			CHECK(error.status == expected && error.operation == FSIMG_OP_IO);
			CHECK(strcmp(error.message, "injected input failure") == 0);
			CHECK(f.reader->state == FSIMG_READER_FAILED);
			expect_losses(&f, 0, 0, NULL, 0);
			remove_tree(parent, "out");
			fixture_close(&f);
		}
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_resolved_extents_outside_or_straddling_content_domain(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	char directory[] = "salvage-domain-XXXXXX";
	unsigned char bytes[3];
	int parent, output, fd;

	parent = open_destination_parent(directory);
	for (unsigned outside = 0; outside < 2; outside++) {
		fake_open(&f);
		/* a and bad/inside still resolve to the complete "yes" extent.
		 * Only c starts later: at the end, or one byte before it.
		 */
		f.reader->content_domain.length = 3;
		f.fake.content_offset = outside ? 3 : 2;
		fsimg_extract_options_init(&options);
		options.flags |= FSIMG_EXTRACT_SALVAGE;
		CHECK(fsimg_extract_at(f.reader, parent, "out", &options, &error) ==
		    FSIMG_EPARTIAL);
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		expect_losses(&f, 1, 1, "/c", outside ?
		    FSIMG_LOSS_SKIPPED : FSIMG_LOSS_SHORTENED);
		CHECK(f.reader->state == FSIMG_READER_DONE);
		output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(output != -1);
		expect_absent(output, "c");
		if (outside)
			expect_absent(output, "c.partial");
		else {
			fd = openat(output, "c.partial", O_RDONLY | O_NOFOLLOW);
			CHECK(fd != -1);
			CHECK(read(fd, bytes, sizeof(bytes)) == 1);
			CHECK(bytes[0] == 's');
			CHECK(read(fd, bytes, sizeof(bytes)) == 0);
			CHECK(close(fd) == 0);
		}
		CHECK(close(output) == 0);
		remove_tree(parent, "out");
		fixture_close(&f);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

/* Only the first cluster belongs to the file. The remaining links must not
 * consume the reader session's work budget, even when they end in a cycle.
 * A one-link control establishes that this budget fits ordinary extraction.
 */
static bool
check_one_cluster_file_work(const char *format, unsigned chain_length,
    bool cycle)
{
	static const fsimg_entry_t entries[] = {
		{ .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/dir/tiny", .kind = FSIMG_ENTRY_FILE, .size = 4608 }
	};
	fixture_t f;
	file_image_t image;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	char directory[] = "salvage-file-work-XXXXXX";
	uint32_t last_target;
	int parent, output;

	parent = open_destination_parent(directory);
	fixture_build(&f, format, entries, 2, true);
	file_image_layout(&f, format, &image);
	image.first = 3000;
	CHECK(image.first + chain_length < image.clusters);
	last_target = cycle ? image.first + chain_length / 2 :
	    image.exfat ? UINT32_MAX : 0xffff;
	for (unsigned i = 0; i < chain_length; i++) {
		file_table_set(&f, &image, image.first + i,
		    i + 1 < chain_length ? image.first + i + 1 : last_target);
		if (image.exfat)
			file_bitmap_set(&f, &image, image.first + i, true);
	}
	memset(f.bytes + file_cluster_offset(&image, image.first), 'B', 512);
	if (image.exfat) {
		image.record[33] = 1; /* Chained allocation, not NoFatChain. */
		fsimg_store_le32(image.record + 52, image.first);
		fsimg_store_le64(image.record + 40, 512);
		fsimg_store_le64(image.record + 56, 512);
		file_set_checksum(image.record);
	} else {
		fsimg_store_le16(image.record + 26, (uint16_t)image.first);
		fsimg_store_le32(image.record + 28, 512);
	}
	fixture_reader(&f);
	/* Opening exFAT already charges its metadata validation. Bound
	 * the additional extraction work equally for control and long chains.
	 */
	f.ctx->options.max_work = f.reader->session.progress.work + 256;
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SALVAGE;
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s one-cluster file, %u-link %s: expected success "
		    "within 256 additional work units, got %s: %s\n", format, chain_length,
		    cycle ? "cycle" : "chain", fsimg_status_string(status),
		    error.message);
	if (status == FSIMG_OK) {
		output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(output != -1);
		expect_pattern_file(output, "dir/tiny", 512, 1, 512);
		expect_absent(output, "dir/tiny.partial");
		CHECK(close(output) == 0);
		expect_losses(&f, 0, 0, NULL, 0);
		CHECK(f.reader->state == FSIMG_READER_DONE);
	}
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
	return (status == FSIMG_OK);
}

static void
test_one_cluster_file_work_ignores_long_chain_and_late_cycle(void)
{
	static const char *const formats[] = { "fat16", "exfat" };
	unsigned failures = 0;

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		CHECK(check_one_cluster_file_work(formats[i], 1, false));
		failures += !check_one_cluster_file_work(formats[i], 8192, false);
		failures += !check_one_cluster_file_work(formats[i], 8192, true);
	}
	CHECK(failures == 0);
}

/* Wire mutations are independent of the format drivers. The writers supply
 * clean trees; only foo's allocation is changed. a, empty and z precede its
 * payload, so a truncation leaves both the namespace and siblings readable.
 */
typedef enum prefix_damage {
	PREFIX_CLEAN,
	PREFIX_PAST_END,
	PREFIX_SYMLINK,
	PREFIX_FIRST_OUTSIDE,
	PREFIX_FIRST_FREE,
	PREFIX_LATER_OUTSIDE,
	PREFIX_SIGNATURE,
	PREFIX_CHAIN_SHORT,
	PREFIX_SIZE,
	PREFIX_CUT,
	PREFIX_CUT_BEFORE,
	PREFIX_INDEX_CUT,
	PREFIX_INDIRECT_BAD,
	PREFIX_LINKS,
	PREFIX_LINK_LOADER,
	PREFIX_LINK_ROOT,
	PREFIX_LINK_BITMAP,
	PREFIX_DIRECTORY,
	PREFIX_ISO_MULTI,
	PREFIX_ISO_SYSTEM,
	PREFIX_ISO_DESCRIPTOR
} prefix_damage_t;

typedef struct prefix_case {
	const char *format;
	const char *name;
	prefix_damage_t damage;
	fsimg_status_t strict_status;
	const char *strict_message;
	fsimg_status_t salvage_status;
	const char *output_name;
	size_t prefix;
	uint32_t loss_mask;
	unsigned loss_count;
	const char *loss_path;
	unsigned shortened;
	unsigned skipped;
	const char *linked_output_name;
	const char *empty_directory;
} prefix_case_t;

static const prefix_case_t prefix_cases[] = {
	{ .format = "qnx4", .name = "clean",
	    .damage = PREFIX_CLEAN, .strict_status = FSIMG_OK,
	    .strict_message = "",
	    .salvage_status = FSIMG_OK, .output_name = "foo",
	    .prefix = 1536, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "qnx4", .name = "first-outside",
	    .damage = PREFIX_FIRST_OUTSIDE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent lies outside the volume",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx4", .name = "first-free",
	    .damage = PREFIX_FIRST_FREE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 referenced block is free",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx4", .name = "later-outside",
	    .damage = PREFIX_LATER_OUTSIDE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent lies outside the volume",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "extent-signature",
	    .damage = PREFIX_SIGNATURE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent block is malformed",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "extent-past-slice",
	    .damage = PREFIX_INDEX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "chain-short",
	    .damage = PREFIX_CHAIN_SHORT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent chain ends before its extent count",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 1024, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "size-over-allocation",
	    .damage = PREFIX_SIZE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 allocation is short of the object's size",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 1536, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "cut",
	    .damage = PREFIX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512 + 137, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx4", .name = "cut-before",
	    .damage = PREFIX_CUT_BEFORE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx4", .name = "two-names",
	    .damage = PREFIX_LINKS, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent lies outside the volume",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 2,
	    .loss_path = "/link", .shortened = 2, .skipped = 0,
	    .linked_output_name = "link.partial" },
	{ .format = "qnx4", .name = "two-names-loader",
	    .damage = PREFIX_LINK_LOADER, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 block is already in use",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 2,
	    .loss_path = "/link", .shortened = 2, .skipped = 0,
	    .linked_output_name = "link.partial" },
	{ .format = "qnx4", .name = "two-names-root",
	    .damage = PREFIX_LINK_ROOT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 block is already in use",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 2,
	    .loss_path = "/link", .shortened = 2, .skipped = 0,
	    .linked_output_name = "link.partial" },
	{ .format = "qnx4", .name = "two-names-bitmap",
	    .damage = PREFIX_LINK_BITMAP, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 block is already in use",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 2,
	    .loss_path = "/link", .shortened = 2, .skipped = 0,
	    .linked_output_name = "link.partial" },
	{ .format = "qnx4", .name = "allocation-past-size",
	    .damage = PREFIX_PAST_END, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 allocation exceeds the object's size",
	    .salvage_status = FSIMG_OK, .output_name = "foo",
	    .prefix = 1024, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "qnx4", .name = "directory-bad-extent",
	    .damage = PREFIX_DIRECTORY, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX4 extent lies outside the volume",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo",
	    .prefix = 1536, .loss_mask = FSIMG_LOSS_DIRECTORY, .loss_count = 1,
	    .loss_path = "/bad", .shortened = 0, .skipped = 0,
	    .empty_directory = "bad" },
	{ .format = "qnx6", .name = "clean",
	    .damage = PREFIX_CLEAN, .strict_status = FSIMG_OK,
	    .strict_message = "",
	    .salvage_status = FSIMG_OK, .output_name = "foo",
	    .prefix = 1536, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "qnx6", .name = "first-outside",
	    .damage = PREFIX_FIRST_OUTSIDE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 required block reference is unused or out of range",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx6", .name = "first-free",
	    .damage = PREFIX_FIRST_FREE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 referenced block is free",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx6", .name = "later-outside",
	    .damage = PREFIX_LATER_OUTSIDE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 required block reference is unused or out of range",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx6", .name = "indirect-past-slice",
	    .damage = PREFIX_INDEX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 128 * 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx6", .name = "indirect-invalid",
	    .damage = PREFIX_INDIRECT_BAD, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 required block reference is unused or out of range",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 128 * 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx6", .name = "cut",
	    .damage = PREFIX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512 + 137, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "qnx6", .name = "cut-before",
	    .damage = PREFIX_CUT_BEFORE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "qnx6", .name = "two-names",
	    .damage = PREFIX_LINKS, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 required block reference is unused or out of range",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 2,
	    .loss_path = "/link", .shortened = 2, .skipped = 0,
	    .linked_output_name = "link.partial" },
	{ .format = "qnx6", .name = "directory-past-slice",
	    .damage = PREFIX_DIRECTORY, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo",
	    .prefix = 1536, .loss_mask = FSIMG_LOSS_DIRECTORY, .loss_count = 1,
	    .loss_path = "/bad", .shortened = 0, .skipped = 0,
	    .empty_directory = "bad" },
	{ .format = "qnx6", .name = "allocation-past-size",
	    .damage = PREFIX_PAST_END, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 unused block reference is out of range",
	    .salvage_status = FSIMG_OK, .output_name = "foo",
	    .prefix = 1536, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "qnx6", .name = "symlink-bad-storage",
	    .damage = PREFIX_SYMLINK, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "QNX6 required block reference is unused or out of range",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_ENTRY, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 0 },
	{ .format = "ifs", .name = "clean",
	    .damage = PREFIX_CLEAN, .strict_status = FSIMG_OK,
	    .strict_message = "",
	    .salvage_status = FSIMG_OK, .output_name = "foo",
	    .prefix = 1536, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "ifs", .name = "cut",
	    .damage = PREFIX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "foo.partial",
	    .prefix = 512 + 137, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 1, .skipped = 0 },
	{ .format = "ifs", .name = "cut-before",
	    .damage = PREFIX_CUT_BEFORE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "image is truncated",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/foo", .shortened = 0, .skipped = 1 },
	{ .format = "iso9660", .name = "clean",
	    .damage = PREFIX_CLEAN, .strict_status = FSIMG_OK,
	    .strict_message = "",
	    .salvage_status = FSIMG_OK, .output_name = "FOO",
	    .prefix = 3 * 2048, .loss_mask = 0, .loss_count = 0,
	    .loss_path = NULL, .shortened = 0, .skipped = 0 },
	{ .format = "iso9660", .name = "multi-section-cut",
	    .damage = PREFIX_ISO_MULTI, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "extent is empty, misaligned or outside its allocation domain",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "FOO.partial",
	    .prefix = 3 * 2048, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/FOO", .shortened = 1, .skipped = 0 },
	{ .format = "iso9660", .name = "single-section-cut",
	    .damage = PREFIX_CUT, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "extent is empty, misaligned or outside its allocation domain",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = "FOO.partial",
	    .prefix = 2048, .loss_mask = FSIMG_LOSS_SHORTENED, .loss_count = 1,
	    .loss_path = "/FOO", .shortened = 1, .skipped = 0 },
	{ .format = "iso9660", .name = "first-past-blocks",
	    .damage = PREFIX_CUT_BEFORE, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "extent is empty, misaligned or outside its allocation domain",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/FOO", .shortened = 0, .skipped = 1 },
	{ .format = "iso9660", .name = "first-system-area",
	    .damage = PREFIX_ISO_SYSTEM, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "extent is empty, misaligned or outside its allocation domain",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/FOO", .shortened = 0, .skipped = 1 },
	{ .format = "iso9660", .name = "first-descriptor",
	    .damage = PREFIX_ISO_DESCRIPTOR, .strict_status = FSIMG_ECORRUPT,
	    .strict_message = "extent is empty, misaligned or outside its allocation domain",
	    .salvage_status = FSIMG_EPARTIAL, .output_name = NULL,
	    .prefix = 0, .loss_mask = FSIMG_LOSS_SKIPPED, .loss_count = 1,
	    .loss_path = "/FOO", .shortened = 0, .skipped = 1 },
};

static bool
prefix_linked(const prefix_case_t *row)
{
	return (row->damage == PREFIX_LINKS ||
	    row->damage == PREFIX_LINK_LOADER ||
	    row->damage == PREFIX_LINK_ROOT ||
	    row->damage == PREFIX_LINK_BITMAP);
}

static void
prefix_bitmap(unsigned char *bitmap, uint32_t bit, bool allocated)
{
	unsigned char mask = (unsigned char)(1u << (bit % 8));

	if (allocated)
		bitmap[bit / 8] |= mask;
	else
		bitmap[bit / 8] &= (unsigned char)~mask;
}

static void
prefix_qnx4(fixture_t *f, const prefix_case_t *row)
{
	enum {
		BLOCK_BYTES = 512, BITMAP_OFFSET = 1024, RECORD_BYTES = 64,
		NAME_BYTES = 16, SIZE_OFFSET = 16, FIRST_OFFSET = 20,
		FIRST_BLOCKS_OFFSET = 24, CHAIN_OFFSET = 28, COUNT_OFFSET = 48,
		LINK_SLOT_OFFSET = 52, LINKS_OFFSET = 56, STATUS_OFFSET = 63,
		X_USED_OFFSET = 8, X_COVERED_OFFSET = 12, X_FIRST_OFFSET = 16,
		X_BLOCKS_OFFSET = 20, X_SECOND_OFFSET = 24,
		X_SECOND_BLOCKS_OFFSET = 28, X_SIGNATURE_OFFSET = 496,
		X_COPY_FIRST_OFFSET = 504, X_COPY_BLOCKS_OFFSET = 508
	};
	unsigned char *record = find_bytes(f, "foo\0", 4), *xblock;
	uint32_t first = fsimg_load_le32(record + FIRST_OFFSET);
	uint32_t blocks = (uint32_t)(f->length / BLOCK_BYTES);
	uint32_t chain = blocks - 16, relocated = blocks / 2, next_first;

	/* Put foo above every sibling, so cutting it leaves siblings readable. */
	bcopy(f->bytes + (size_t)(first - 1) * BLOCK_BYTES,
	    f->bytes + (size_t)(relocated - 1) * BLOCK_BYTES, 1536);
	first = relocated;
	fsimg_store_le32(record + FIRST_OFFSET, first);
	for (unsigned i = 0; i < 3; i++)
		prefix_bitmap(f->bytes + BITMAP_OFFSET, first + i - 1, true);

	if (prefix_linked(row)) {
		unsigned char *other = find_bytes(f, "link\0", 5);
		unsigned char *table = find_bytes(f, "IamTHE.inodeFILE", 16);
		uint64_t position = (uint64_t)(table - f->bytes) + RECORD_BYTES;
		uint32_t table_block = (uint32_t)(position / BLOCK_BYTES + 1);
		unsigned char slot = (unsigned char)(position % BLOCK_BYTES / RECORD_BYTES);

		bcopy(record, table + RECORD_BYTES, RECORD_BYTES);
		bzero(table + RECORD_BYTES, NAME_BYTES);
		table[RECORD_BYTES + STATUS_OFFSET] = 0x13;
		fsimg_store_le16(table + RECORD_BYTES + LINKS_OFFSET, 2);
		bzero(record, RECORD_BYTES);
		bcopy("foo", record, 3);
		bzero(other, RECORD_BYTES);
		bcopy("link", other, 4);
		fsimg_store_le32(record + COUNT_OFFSET, table_block);
		fsimg_store_le32(other + COUNT_OFFSET, table_block);
		record[LINK_SLOT_OFFSET] = other[LINK_SLOT_OFFSET] = slot;
		record[STATUS_OFFSET] = other[STATUS_OFFSET] = 8;
		record = table + RECORD_BYTES;
	}
	if (row->damage == PREFIX_DIRECTORY) {
		unsigned char *directory = find_bytes(f, "bad\0", 4);

		fsimg_store_le32(directory + FIRST_OFFSET, blocks + 1);
	}
	if (row->damage == PREFIX_FIRST_OUTSIDE)
		fsimg_store_le32(record + FIRST_OFFSET, blocks + 1);
	if (row->damage == PREFIX_FIRST_FREE)
		prefix_bitmap(f->bytes + BITMAP_OFFSET, first - 1, false);
	if (row->damage == PREFIX_SIZE)
		fsimg_store_le32(record + SIZE_OFFSET, 2048);
	if (row->damage == PREFIX_CUT || row->damage == PREFIX_CUT_BEFORE) {
		f->length = (size_t)(first - 1) * BLOCK_BYTES + row->prefix;
		return;
	}
	if (row->damage != PREFIX_LATER_OUTSIDE &&
	    row->damage != PREFIX_SIGNATURE && row->damage != PREFIX_CHAIN_SHORT &&
	    row->damage != PREFIX_INDEX_CUT && row->damage != PREFIX_PAST_END &&
	    !prefix_linked(row))
		return;

	/* Split the original run after its first block, using a free block
	 * for the extent descriptors and an independent wire signature.
	 */
	xblock = f->bytes + (size_t)(chain - 1) * BLOCK_BYTES;
	bzero(xblock, BLOCK_BYTES);
	prefix_bitmap(f->bytes + BITMAP_OFFSET, chain - 1, true);
	fsimg_store_le32(record + FIRST_BLOCKS_OFFSET, 1);
	fsimg_store_le32(record + CHAIN_OFFSET, chain);
	fsimg_store_le16(record + COUNT_OFFSET, row->damage == PREFIX_CHAIN_SHORT ? 3 : 2);
	xblock[X_USED_OFFSET] = 1;
	fsimg_store_le32(xblock + X_COVERED_OFFSET, row->damage == PREFIX_CHAIN_SHORT ? 1 : 2);
	switch (row->damage) {
	case PREFIX_LINK_LOADER:
		next_first = 1;
		break;
	case PREFIX_LINK_ROOT:
		next_first = 2;
		break;
	case PREFIX_LINK_BITMAP:
		next_first = 3;
		break;
	case PREFIX_SIGNATURE:
	case PREFIX_INDEX_CUT:
	case PREFIX_CHAIN_SHORT:
		next_first = first + 1;
		break;
	default:
		next_first = blocks + 1;
		break;
	}
	fsimg_store_le32(xblock + X_FIRST_OFFSET, next_first);
	fsimg_store_le32(xblock + X_BLOCKS_OFFSET, row->damage == PREFIX_CHAIN_SHORT ? 1 : 2);
	bcopy("IamXblk", xblock + X_SIGNATURE_OFFSET, 7);
	fsimg_store_le32(xblock + X_COPY_FIRST_OFFSET, first);
	fsimg_store_le32(xblock + X_COPY_BLOCKS_OFFSET, 1);
	if (row->damage == PREFIX_PAST_END) {
		/* Both logical blocks are in the first extent. The record claims
		 * three extents, with the next descriptor outside the volume.
		 */
		fsimg_store_le32(record + SIZE_OFFSET, 1024);
		fsimg_store_le32(record + FIRST_BLOCKS_OFFSET, 2);
		fsimg_store_le16(record + COUNT_OFFSET, 3);
		xblock[X_USED_OFFSET] = 2;
		fsimg_store_le32(xblock + X_BLOCKS_OFFSET, 1);
		fsimg_store_le32(xblock + X_SECOND_OFFSET, first + 2);
		fsimg_store_le32(xblock + X_SECOND_BLOCKS_OFFSET, 1);
		fsimg_store_le32(xblock + X_COPY_BLOCKS_OFFSET, 2);
	}
	if (row->damage == PREFIX_SIGNATURE)
		xblock[X_SIGNATURE_OFFSET] = '!';
	if (row->damage == PREFIX_INDEX_CUT)
		f->length = (size_t)(chain - 1) * BLOCK_BYTES;
}

/* Decode only the writer's small, little-endian inode-table descriptor.
 * The table can be indirect even though this fixture uses few live inodes.
 */
static unsigned char *
prefix_qnx6_inode(fixture_t *f, uint32_t number)
{
	unsigned char *header = f->bytes + 8192;
	unsigned char *refs = header + 80;
	uint64_t leaf = (uint64_t)(number - 1) * 128 / 512;
	uint64_t span = 1;
	uint32_t block;

	for (unsigned i = 0; i < header[144]; i++)
		span *= 128;
	for (unsigned depth = header[144]; ; depth--) {
		block = fsimg_load_le32(refs + leaf / span * 4);
		CHECK(12288u + (uint64_t)block * 512 + 512 <= f->length);
		refs = f->bytes + 12288u + (size_t)block * 512;
		if (depth == 0)
			return (refs + (number - 1) % 4 * 128);
		leaf %= span;
		span /= 128;
	}
}

static void
prefix_qnx6(fixture_t *f, const prefix_case_t *row)
{
	unsigned char *name = find_bytes(f, "foo\0", 4);
	uint32_t number = fsimg_load_le32(name - 5);
	unsigned char *inode = prefix_qnx6_inode(f, number);
	unsigned char *refs = inode + 36;
	unsigned char *header = f->bytes + 8192;
	uint32_t blocks = fsimg_load_le32(header + 60);
	uint32_t first = fsimg_load_le32(refs);
	uint32_t bitmap = fsimg_load_le32(header + 160);

	CHECK(header[224] == 0);
	if (inode[100] == 0) {
		uint32_t relocated = blocks / 2;

		for (unsigned i = 0; i < 3; i++) {
			uint32_t old = fsimg_load_le32(refs + i * 4);

			bcopy(f->bytes + 12288u + (size_t)old * 512,
			    f->bytes + 12288u + (size_t)(relocated + i) * 512, 512);
			fsimg_store_le32(refs + i * 4, relocated + i);
			prefix_bitmap(f->bytes + 12288u + (size_t)bitmap * 512,
			    relocated + i, true);
		}
		first = relocated;
	}
	if (prefix_linked(row)) {
		name = find_bytes(f, "link\0", 5);
		fsimg_store_le32(name - 5, number);
		fsimg_store_le16(inode + 34, 2);
	}
	if (row->damage == PREFIX_PAST_END)
		fsimg_store_le32(refs + 3 * 4, blocks);
	if (row->damage == PREFIX_SYMLINK) {
		/* Keep one block of the file as link storage, then invalidate it. */
		fsimg_store_le64(inode, 512);
		fsimg_store_le16(inode + 32, 0120777);
		fsimg_store_le32(refs, blocks);
		fsimg_store_le32(refs + 4, UINT32_MAX);
		fsimg_store_le32(refs + 8, UINT32_MAX);
	}
	if (row->damage == PREFIX_FIRST_FREE)
		prefix_bitmap(f->bytes + 12288u + (size_t)bitmap * 512, first, false);
	if (row->damage == PREFIX_FIRST_OUTSIDE)
		fsimg_store_le32(refs, blocks);
	if (row->damage == PREFIX_LATER_OUTSIDE || prefix_linked(row))
		fsimg_store_le32(refs + 4, blocks);
	if (row->damage == PREFIX_CUT || row->damage == PREFIX_CUT_BEFORE)
		f->length = 12288u + (size_t)first * 512 + row->prefix;
	if (row->damage == PREFIX_INDIRECT_BAD)
		fsimg_store_le32(refs + 4, blocks);
	if (row->damage == PREFIX_INDEX_CUT) {
		/* Relocate the second indirect block past all namespace bytes.
		 * Its allocated reference is valid in-volume, but not in-slice.
		 */
		fsimg_store_le32(refs + 4, blocks - 1);
		prefix_bitmap(f->bytes + 12288u + (size_t)bitmap * 512,
		    blocks - 1, true);
		f->length = 12288u + (size_t)(blocks - 1) * 512;
	}
	if (row->damage == PREFIX_DIRECTORY) {
		name = find_bytes(f, "bad\0", 4);
		inode = prefix_qnx6_inode(f, fsimg_load_le32(name - 5));
		fsimg_store_le32(inode + 36, blocks - 1);
		prefix_bitmap(f->bytes + 12288u + (size_t)bitmap * 512,
		    blocks - 1, true);
		f->length = 12288u + (size_t)(blocks - 1) * 512;
	}
}

/* ECMA-119 both-endian fields and section records, matching the independent
 * encoder in iso9660-image.c. No production layout or decoder is used.
 */
static void
prefix_iso32(unsigned char *p, uint32_t value)
{
	fsimg_store_le32(p, value);
	fsimg_store_be32(p + 4, value);
}

static size_t
prefix_iso_record(unsigned char *p, const char *name, uint32_t first,
    uint32_t size, unsigned flags)
{
	size_t length = strlen(name), bytes = 33 + length + (length % 2 == 0);

	bzero(p, bytes);
	p[0] = (unsigned char)bytes;
	prefix_iso32(p + 2, first);
	prefix_iso32(p + 10, size);
	p[25] = (unsigned char)flags;
	p[28] = p[31] = 1;
	p[32] = (unsigned char)length;
	bcopy(name, p + 33, length);
	return (bytes);
}

static void
prefix_iso(fixture_t *f, const prefix_case_t *row)
{
	unsigned char *root, *p;
	size_t used = 68;
	uint32_t first = row->damage == PREFIX_ISO_SYSTEM ? 1 :
	    row->damage == PREFIX_ISO_DESCRIPTOR ? 16 : 104;

	/* Keep the writer's descriptors/path tables; replace its root records
	 * with a small plain ISO hierarchy at an independently chosen block.
	 */
	p = f->bytes + 16 * 2048;
	prefix_iso32(p + 156 + 2, 84);
	prefix_iso32(p + 156 + 10, 2048);
	fsimg_store_le32(f->bytes +
	    (size_t)fsimg_load_le32(p + 140) * 2048 + 2, 84);
	fsimg_store_be32(f->bytes +
	    (size_t)fsimg_load_be32(p + 148) * 2048 + 2, 84);
	root = f->bytes + 84 * 2048;
	bzero(root, 2048);
	prefix_iso_record(root, "x", 84, 2048, 2);
	root[33] = 0;
	prefix_iso_record(root + 34, "x", 84, 2048, 2);
	root[34 + 33] = 1;
	used += prefix_iso_record(root + used, "A;1", 100, 512, 0);
	used += prefix_iso_record(root + used, "EMPTY;1", 101, 0, 0);
	used += prefix_iso_record(root + used, "Z;1", 102, 512, 0);
	if (row->damage == PREFIX_ISO_MULTI) {
		used += prefix_iso_record(root + used, "FOO;1", 104, 2048, 0x80);
		used += prefix_iso_record(root + used, "FOO;1", 106, 3 * 2048, 0x80);
		prefix_iso_record(root + used, "FOO;1", 110, 2048, 0);
		f->length = 108 * 2048 + 137;
	} else {
		prefix_iso_record(root + used, "FOO;1", first, 3 * 2048, 0);
		if (row->damage == PREFIX_CUT || row->damage == PREFIX_CUT_BEFORE)
			f->length = 104 * 2048 + row->prefix +
			    (row->damage == PREFIX_CUT ? 137 : 0);
	}
	/* Distinct logical-block bytes detect gaps, zeros and partial blocks. */
	for (unsigned i = 0; i < 10; i++)
		memset(f->bytes + (104 + i) * 2048, 'D' + i, 2048);
	if (row->damage == PREFIX_ISO_MULTI) {
		for (unsigned i = 0; i < 3; i++)
			memset(f->bytes + (106 + i) * 2048, 'H' + i, 2048);
	}
	memset(f->bytes + 100 * 2048, 'A', 512);
	memset(f->bytes + 102 * 2048, 'C', 512);
}

static void
prefix_ifs(fixture_t *f, const prefix_case_t *row)
{
	unsigned char bytes[1536];
	unsigned char *name = find_bytes(f, "foo\0", 4);
	size_t record = (size_t)(name - f->bytes) - 32;
	uint32_t old = fsimg_load_le32(f->bytes + record + 24);
	uint32_t first = (uint32_t)f->length - 4, sum = 0;

	bcopy(f->bytes + old, bytes, sizeof(bytes));
	CHECK(memory_resize(f, f->length + sizeof(bytes), NULL) == FSIMG_OK);
	bcopy(bytes, f->bytes + first, sizeof(bytes));
	fsimg_store_le32(f->bytes + record + 24, first);
	fsimg_store_le32(f->bytes + 8, (uint32_t)f->length);
	for (size_t i = 0; i + 4 < f->length; i += 4)
		sum += fsimg_load_le32(f->bytes + i);
	fsimg_store_le32(f->bytes + f->length - 4, 0u - sum);
	if (row->damage != PREFIX_CLEAN)
		f->length = first + row->prefix;
}

static void
build_prefix_case(fixture_t *f, const prefix_case_t *row)
{
	fsimg_entry_t entries[] = {
		{ .path = "/a", .kind = FSIMG_ENTRY_FILE, .size = 512 },
		{ .path = "/empty", .kind = FSIMG_ENTRY_FILE },
		{ .path = "/z", .kind = FSIMG_ENTRY_FILE, .size = 512 },
		{ .path = "/foo", .kind = FSIMG_ENTRY_FILE, .size = 1536 },
		{ .path = row->damage == PREFIX_DIRECTORY ? "/bad" : "/link",
		    .kind = row->damage == PREFIX_DIRECTORY ? FSIMG_ENTRY_DIRECTORY :
		    FSIMG_ENTRY_FILE, .size = row->damage == PREFIX_DIRECTORY ? 0 : 512 },
		{ .path = "/bad/child", .kind = FSIMG_ENTRY_FILE, .size = 512 }
	};

	if (strcmp(row->format, "qnx6") == 0 &&
	    (row->damage == PREFIX_INDEX_CUT || row->damage == PREFIX_INDIRECT_BAD))
		entries[3].size = 129 * 512;
	size_t count = 4;

	if (row->damage == PREFIX_DIRECTORY)
		count = 6;
	else if (prefix_linked(row))
		count = 5;
	fixture_build(f, row->format, entries, count, true);
	if (strcmp(row->format, "qnx4") == 0)
		prefix_qnx4(f, row);
	else if (strcmp(row->format, "qnx6") == 0)
		prefix_qnx6(f, row);
	else if (strcmp(row->format, "iso9660") == 0)
		prefix_iso(f, row);
	else
		prefix_ifs(f, row);
	/* Resize the callback adapter's cached slice before opening. */
	CHECK(fsimg_io_resize(f->io, f->length, NULL) == FSIMG_OK);
	if (strcmp(row->format, "iso9660") == 0) {
		fsimg_read_options_t options;

		fsimg_read_options_init(&options);
		options.iso_view = FSIMG_ISO_VIEW_PRIMARY;
		CHECK(fsimg_reader_open(f->ctx, f->io, &options, &f->reader,
		    NULL) == FSIMG_OK);
	} else
		fixture_reader(f);
}

static void
expect_iso_prefix(int root, const char *name, const prefix_case_t *row)
{
	unsigned char bytes[2048];
	struct stat st;
	int fd;

	fd = openat(root, name, O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t)row->prefix);
	for (size_t i = 0; i < row->prefix / 2048; i++) {
		unsigned char expected = (unsigned char)('D' + i);

		/* The first section has one D block; the second starts at H. */
		if (row->damage == PREFIX_ISO_MULTI && i != 0)
			expected = (unsigned char)('H' + (i - 1));

		CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
		for (size_t j = 0; j < sizeof(bytes); j++)
			CHECK(bytes[j] == expected);
	}
	CHECK(read(fd, bytes, 1) == 0);
	CHECK(close(fd) == 0);
}

/* The row supplies the oracle; format selection only chooses its byte codec. */
static void
expect_prefix_output(int output, const prefix_case_t *row)
{
	bool iso = strcmp(row->format, "iso9660") == 0;
	const char *plain = iso ? "FOO" : "foo";
	const char *partial = iso ? "FOO.partial" : "foo.partial";

	expect_pattern_file(output, iso ? "A" : "a", 512, 0, 512);
	expect_pattern_file(output, iso ? "EMPTY" : "empty", 0, 1, 0);
	expect_pattern_file(output, iso ? "Z" : "z", 512, 2, 512);
	if (row->output_name == NULL || strcmp(row->output_name, plain) != 0)
		expect_absent(output, plain);
	if (row->output_name == NULL || strcmp(row->output_name, partial) != 0)
		expect_absent(output, partial);
	if (row->output_name != NULL) {
		if (iso)
			expect_iso_prefix(output, row->output_name, row);
		else
			expect_pattern_file(output, row->output_name,
			    row->prefix, 3, row->prefix);
	}
	if (row->empty_directory != NULL) {
		CHECK(directory_children(output, row->empty_directory) == 0);
		expect_absent(output, "bad.partial");
		expect_absent(output, "bad/child");
		expect_absent(output, "child");
	}
	if (row->linked_output_name != NULL) {
		expect_absent(output, "link");
		expect_pattern_file(output, row->linked_output_name,
		    row->prefix, 3, row->prefix);
	}
}

static void
expect_prefix_entry(const fixture_t *f, const prefix_case_t *row,
    const char *name, uint64_t size)
{
	unsigned matches = 0;

	for (unsigned i = 0; i < f->entry_reports; i++) {
		if (strcmp(f->entry_paths[i] + 1, name) != 0)
			continue;
		if (f->entry_sizes[i] != size)
			fprintf(stderr, "prefix %s/%s: /%s expected %llu bytes, got %llu\n",
			    row->format, row->name, name, (unsigned long long)size,
			    (unsigned long long)f->entry_sizes[i]);
		CHECK(f->entry_sizes[i] == size);
		matches++;
	}
	if (matches != 1)
		fprintf(stderr, "prefix %s/%s: /%s expected one entry report, got %u\n",
		    row->format, row->name, name, matches);
	CHECK(matches == 1);
}

static void
check_prefix_case(const prefix_case_t *row, bool salvage)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status, expected;
	char directory[] = "salvage-prefix-XXXXXX";
	int parent, output;

	parent = open_destination_parent(directory);
	build_prefix_case(&f, row);
	f.capture_entries = true;
	fsimg_extract_options_init(&options);
	if (salvage)
		options.flags |= FSIMG_EXTRACT_SALVAGE;
	expected = salvage ? row->salvage_status : row->strict_status;
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	if (status != expected || (!salvage && expected != FSIMG_OK &&
	    strcmp(error.message, row->strict_message) != 0))
		fprintf(stderr, "prefix %s/%s %s: expected %s, got %s: %s\n",
		    row->format, row->name, salvage ? "salvage" : "strict",
		    fsimg_status_string(expected), fsimg_status_string(status),
		    error.message);
	CHECK(status == expected);
	if (!salvage && expected != FSIMG_OK) {
		CHECK(error.status == row->strict_status);
		CHECK(strcmp(error.message, row->strict_message) == 0);
		CHECK(f.reader->state == FSIMG_READER_FAILED);
		expect_losses(&f, 0, 0, NULL, 0);
	} else {
		output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(output != -1);
		expect_prefix_output(output, row);
		if (row->shortened == 1 && row->loss_count == 1) {
			bool iso = strcmp(row->format, "iso9660") == 0;

			/* QNX writers also supply boot entries. Check each intended
			 * sibling and shortened file once, independently of those extras.
			 */
			expect_prefix_entry(&f, row, row->output_name, row->prefix);
			expect_prefix_entry(&f, row, iso ? "A" : "a", 512);
			expect_prefix_entry(&f, row, iso ? "EMPTY" : "empty", 0);
			expect_prefix_entry(&f, row, iso ? "Z" : "z", 512);
		}
		expect_losses(&f, row->loss_count, row->loss_count,
		    row->loss_path, row->loss_mask);
		CHECK(f.shortened == row->shortened);
		CHECK(f.skipped == row->skipped);
		if (row->linked_output_name != NULL) {
			CHECK(strcmp(f.first_loss_path, "/foo") == 0);
			CHECK(f.first_loss_mask == row->loss_mask);
		}
		CHECK(f.reader->state == FSIMG_READER_DONE);
		if (expected == FSIMG_EPARTIAL)
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		CHECK(close(output) == 0);
	}
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

static void
test_strict_exclusive_creation_collision_preserves_existing_file(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	char directory[] = "salvage-strict-collision-XXXXXX";
	fsimg_status_t status;
	char bytes[9];
	int parent, fd;

	parent = open_destination_parent(directory);
	fake_open(&f);
	f.fake.host_collision = true;
	f.fake.collision_parent = parent;
	fsimg_extract_options_init(&options);
	status = fsimg_extract_at(f.reader, parent, "out", &options, &error);
	if (status != FSIMG_EEXISTS)
		fprintf(stderr, "strict collision: %s: %s\n",
		    fsimg_status_string(status), error.message);
	CHECK(status == FSIMG_EEXISTS);
	CHECK(error.status == FSIMG_EEXISTS);
	CHECK(strcmp(error.message, "exclusive output file creation failed") == 0);
	CHECK(f.reader->state == FSIMG_READER_FAILED);
	expect_losses(&f, 0, 0, NULL, 0);
	fd = openat(parent, "out/c", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(read(fd, bytes, sizeof(bytes)) == 8);
	CHECK(bcmp(bytes, "occupied", 8) == 0);
	CHECK(read(fd, bytes, 1) == 0);
	CHECK(close(fd) == 0);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	fixture_close(&f);
}

/* Each row owns a disposable working directory. A failed assertion in a
 * child cannot hide another row or leave its output tree behind.
 */
static unsigned
run_file_prefix_or_collision_row(const prefix_case_t *prefix, const file_case_t *file,
    const char *format, bool salvage)
{
	char directory[] = "salvage-row-XXXXXX";
	const char *name = prefix != NULL ? prefix->name : file->name;
	pid_t child;
	int status;
	bool passed;

	CHECK(mkdtemp(directory) != NULL);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(chdir(directory) == 0);
		if (prefix != NULL)
			check_prefix_case(prefix, salvage);
		else {
			char destination[] = "destination-XXXXXX";
			int parent = open_destination_parent(destination);

			if (salvage)
				check_file_salvage(format, file, parent,
				    "/dir/foo", "/dir/foo.partial");
			else
				check_file_strict(format, file, parent,
				    "/dir/foo", "/dir/foo.partial");
			CHECK(close(parent) == 0);
			CHECK(rmdir(destination) == 0);
		}
		exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child);
	remove_tree(AT_FDCWD, directory);
	passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	fprintf(stderr, "regression %s/%s %s: %s\n", format, name,
	    salvage ? "salvage" : "strict", passed ? "PASS" : "FAIL");
	return (!passed);
}

static unsigned
test_file_prefixes_and_exclusive_creation_collisions(void)
{
	static const char *const formats[] = { "fat16", "exfat" };
	unsigned failures = 0;

	for (size_t i = 0; i < sizeof(prefix_cases) / sizeof(prefix_cases[0]); i++) {
		for (unsigned salvage = 0; salvage < 2; salvage++)
			failures += run_file_prefix_or_collision_row(&prefix_cases[i], NULL,
			    prefix_cases[i].format, salvage != 0);
	}
	for (size_t i = 0; i < sizeof(file_cases) / sizeof(file_cases[0]); i++) {
		if (file_cases[i].damage != FILE_LATER_COLLISION &&
		    file_cases[i].damage != FILE_LATER_DIRECTORY_COLLISION)
			continue;
		for (size_t j = 0; j < sizeof(formats) / sizeof(formats[0]); j++) {
			for (unsigned salvage = 0; salvage < 2; salvage++)
				failures += run_file_prefix_or_collision_row(NULL, &file_cases[i],
				    formats[j], salvage != 0);
		}
	}
	return (failures);
}

int
main(void)
{
	unsigned regression_failures;

	regression_failures = test_file_prefixes_and_exclusive_creation_collisions();
	test_strict_exclusive_creation_collision_preserves_existing_file();
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
	test_content_io_failure_is_terminal_with_salvage_enabled();
	test_resolve_corruption_skips_file_before_creation();
	test_resolved_extents_outside_or_straddling_content_domain();
	test_file_pass_resource_and_state_failures_are_terminal();
	test_output_write_failure_after_content_read_is_terminal();
	test_table_walk_changed_input_and_provider_io_failures_are_terminal();
	test_file_salvage_prefix_bytes_partial_names_collisions_and_output_limits();
	test_terminal_content_failure_takes_precedence_over_directory_loss();
	test_one_cluster_file_work_ignores_long_chain_and_late_cycle();
	test_skipped_unsupported_entries_are_losses_only_under_salvage();
	CHECK(regression_failures == 0);
	return (0);
}
