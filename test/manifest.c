/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "digest.h"
#include "fsimg-endian.h"
#include "internal.h"
#include "mtree-writer.h"
#include "socket.h"
#include "source.h"
#include "test.h"

/* fsimg_writer_add_manifest() end to end: images created from manifests,
 * read back and compared with what the manifest says. Generated content
 * is compared with a reference written here from the definitions, and its
 * digests with values the harness's Ruby generator computed; host contents
 * are walked beneath a content directory and refused wherever the walk
 * would follow a link or leave it; a manifest root merges with host trees
 * by the one merging rule; line order never changes the image; and the
 * manifests mtree -c and bsdtar write for a host tree rebuild the image
 * that importing the tree itself builds.
 */

extern char **environ;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	char		 directory[64];
	int		 parent;
	unsigned	 manifests;
} fixture_t;

/* An image in memory, sized by its plan. */
typedef struct image {
	unsigned char	*bytes;
	uint64_t	 length;
} image_t;

/* A file read back from an image. */
typedef struct found_file {
	fsimg_entry_kind_t kind;
	fsimg_metadata_t metadata;
	uint64_t	 size;
	unsigned char	 bytes[80000];
} found_file_t;

/* A manifest that must be refused, at input or by planning: status, the
 * image path named and the line, all three asserted.
 */
typedef struct refusal {
	const char	*manifest;
	fsimg_status_t	 status;
	unsigned	 line;
	const char	*path;
} refusal_t;

/* The calls a manifest output receives, by kind. */
typedef enum sink_call {
	SINK_RESIZE,
	SINK_WRITE,
	SINK_FLUSH,
	SINK_CALL_KINDS
} sink_call_t;

/*
 * A manifest output for the writer that counts each kind of call it
 * receives and fails the one numbered fail_at, counted from one, of the
 * kind fail_kind; fail_at zero fails nothing. It keeps only the length.
 */
typedef struct failing_sink {
	uint64_t	 length;
	unsigned	 calls[SINK_CALL_KINDS];
	sink_call_t	 fail_kind;
	unsigned	 fail_at;
} failing_sink_t;

/* A cancellation callback cancelling the poll numbered cancel_at, counted
 * from one, and every poll after it; cancel_at zero cancels nothing.
 */
typedef struct poll_counter {
	unsigned	 polls;
	unsigned	 cancel_at;
} poll_counter_t;

/* SHA-256 values computed by the harness's definition in Ruby
 * (FileContent.bytes, Digest::SHA256), independent of this C code.
 */
#define SEEDED_7_100 \
	"c3f1affe78a5f023f11bdb3d47c870088ee77dcb7c2b434979b8af80f969ddf5"
#define SEEDED_7_100_HOLE_10_20 \
	"7c0053017f284ec2397f5d03d1c10f67058026349190665e170d4037d82d61c9"
#define POSITIONAL_9_100 \
	"12a9e07c3a17d332bcf07c9c865922eeebd99ff2ac4d5d48750a1ccc6a3c1707"
#define ZERO_5000 \
	"7ca5bd879f393d9dd05b14f38add9c0fc6b67928f7f2d261b2e47a32ee8219e3"
#define SHA256_ABC \
	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
/* A name component longer than any host's NAME_MAX. */
#define LONG_NAME_10 "nnnnnnnnnn"
#define LONG_NAME_100 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 \
    LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 \
    LONG_NAME_10
#define LONG_NAME LONG_NAME_100 LONG_NAME_100 LONG_NAME_100
#define SHA256_XY \
	"769a4e6d0003189c7e96c5d9b7e810a0d11c3a12832527ec94b0f86d277f51ca"

static const char generated_manifest[] =
    "#mtree v2.0\n"
    "/set type=file mode=0644 time=1000000000.5\n"
    ". type=dir\n"
    "./zero fsimg.content=zero size=5000 sha256digest=" ZERO_5000 "\n"
    "./seeded fsimg.content=seeded fsimg.seed=7 size=100 \\\n"
    "    sha256digest=" SEEDED_7_100 "\n"
    "./holes fsimg.content=seeded fsimg.seed=7 size=100 fsimg.holes=10:20 \\\n"
    "    sha256digest=" SEEDED_7_100_HOLE_10_20 "\n"
    "./positional fsimg.content=positional fsimg.seed=9 size=100 \\\n"
    "    sha256digest=" POSITIONAL_9_100 "\n"
    "./big fsimg.content=seeded fsimg.seed=3 size=70000 \\\n"
    "    fsimg.holes=0:4096,8192:30000\n"
    "./dir type=dir\n"
    "./dir/empty fsimg.content=zero size=0\n";

static void fixture_open(fixture_t *, const char *, uint32_t);
static void fixture_open_options(fixture_t *, const char *, uint32_t,
    const fsimg_options_t *);
static void fixture_writer(fixture_t *, const char *, uint32_t);
static void fixture_close(fixture_t *);
static void append_text(char *, size_t, const char *);
static int write_manifest(fixture_t *, const char *);
static fsimg_status_t add_manifest(fixture_t *, const char *, int,
    fsimg_error_t *);
static fsimg_status_t emit_image(fixture_t *, image_t *, fsimg_error_t *);
static bool find_file(fixture_t *, const image_t *, const char *,
    found_file_t *);
static uint64_t count_entries(fixture_t *, const image_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t memory_flush(void *, fsimg_error_t *);
static void memory_io(fixture_t *, image_t *, bool, fsimg_io_t **);
static unsigned char seeded_byte(uint64_t, uint64_t);
static unsigned char positional_byte(uint64_t, uint64_t);
static bool in_holes(const uint64_t (*)[2], size_t, uint64_t);
static void check_seeded(const found_file_t *, uint64_t, uint64_t,
    const uint64_t (*)[2], size_t);
static void put_file(int, const char *, const char *);
static int put_directory(int, const char *);
static uint64_t line_offset(const char *, unsigned);
static void check_refusal(const refusal_t *, int);
static void check_named_line(const refusal_t *, const fsimg_error_t *);
static int run_tool(const char *const *, int);
static void remove_tree(const char *);
static void test_generated_files(void);
static void test_generated_digest_refused(void);
static uint64_t refused_plan_work(const char *, uint64_t, uint64_t,
    const char *, fsimg_status_t);
static void test_generated_digest_after_planning(void);
static void test_generated_ranges(void);
static void test_host_contents(void);
static void test_host_refusals(void);
static void test_host_digest_at_emission(void);
static void test_host_digest_needs_whole_file(void);
static void test_merge_with_tree(void);
static void test_duplicates(void);
static void test_line_order(void);
static void test_unused_content_directory(void);
static void test_qnx6_metadata(void);
static void test_external_writers(void);
static char *extract_with_manifest(fixture_t *, const image_t *,
    const char *);
static void rebuild_from_extraction(fixture_t *, const char *, uint32_t,
    const char *, image_t *);
static void test_export_round_trip(const char *, const char *);
static void test_export_changes_detected(void);
static uint64_t initialized_bytes(fixture_t *);
static void test_export_exfat_zero_file(void);
static char *long_source(void);
static void test_export_long_manifest(void);
static void test_export_lines(void);
static fsimg_status_t sink_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t sink_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t sink_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t sink_flush(void *, fsimg_error_t *);
static bool sink_fails(failing_sink_t *, sink_call_t);
static bool poll_cancelled(void *);
static fsimg_status_t write_lines(fixture_t *, failing_sink_t *);
static void test_export_writer_faults(void);
static fsimg_status_t extract_faulted(fixture_t *, const image_t *,
    const char *, uint64_t, fsimg_error_t *);
static void test_export_allocation_failures(void);
static void test_export_cancellation(void);
static void test_export_file_size_limit(void);
static void test_export_refusals(void);
static void test_export_read_by_bsdtar(void);
static void rebuild_from_tool(fixture_t *, const char *const *, int,
    const image_t *);

static void
fixture_open(fixture_t *fixture, const char *format, uint32_t flags)
{
	fixture_open_options(fixture, format, flags, NULL);
}

/* A fixture whose context takes its limits and callbacks from base, or the
 * defaults when base is NULL; the fixture sets the memory budget, the
 * descriptor limit and scratch itself.
 */
static void
fixture_open_options(fixture_t *fixture, const char *format, uint32_t flags,
    const fsimg_options_t *base)
{
	fsimg_options_t options;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "manifest-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	if (base != NULL)
		options = *base;
	else
		fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(64) * 1024 * 1024;
	options.scratch_directory_fd = fixture->parent;
	options.max_open_files = 16;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline,
	    NULL) == FSIMG_OK);
	fixture_writer(fixture, format, flags);
}

/* Replace the fixture's writer with a fresh one; flags add to the default
 * write flags.
 */
static void
fixture_writer(fixture_t *fixture, const char *format, uint32_t flags)
{
	fsimg_write_options_t write_options;

	fsimg_writer_free(fixture->writer);
	fixture->writer = NULL;
	fsimg_write_options_init(&write_options);
	write_options.flags |= flags;
	CHECK(fsimg_writer_new(fixture->ctx, format, &write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
}

/* The writer and context must have returned every byte and descriptor. */
static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->parent) == 0);
	remove_tree(fixture->directory);
}

/* Write text to a new file in the fixture directory and return it opened
 * read-only, as a caller hands the manifest to the library.
 */
static int
write_manifest(fixture_t *fixture, const char *text)
{
	char name[32];
	size_t length;
	int fd;

	snprintf(name, sizeof(name), "manifest-%u", fixture->manifests++);
	fd = openat(fixture->parent, name,
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	length = strlen(text);
	CHECK(write(fd, text, length) == (ssize_t)length);
	CHECK(close(fd) == 0);
	fd = openat(fixture->parent, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	return (fd);
}

static fsimg_status_t
add_manifest(fixture_t *fixture, const char *text, int contents,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	int fd;

	fd = write_manifest(fixture, text);
	status = fsimg_writer_add_manifest(fixture->writer, fd, contents, error);
	CHECK(close(fd) == 0);
	return (status);
}

static fsimg_status_t
emit_image(fixture_t *fixture, image_t *image, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_status_t status;

	bzero(image, sizeof(*image));
	status = fsimg_writer_plan(fixture->writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	image->length = info.volume_bytes;
	image->bytes = malloc((size_t)image->length);
	CHECK(image->bytes != NULL);
	memory_io(fixture, image, true, &output);
	status = fsimg_writer_emit(fixture->writer, output, error);
	fsimg_io_free(output);
	return (status);
}

/* Find path in image and read its whole content into file. */
static bool
find_file(fixture_t *fixture, const image_t *image, const char *path,
    found_file_t *file)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	uint64_t offset;
	size_t length;
	fsimg_status_t status;
	bool found;

	memory_io(fixture, (image_t *)image, false, &input);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader,
	    NULL) == FSIMG_OK);
	found = false;
	while (!found &&
	    (status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		if (strcmp(entry->path, path) != 0)
			continue;
		found = true;
		file->kind = entry->kind;
		file->size = entry->size;
		file->metadata = entry->metadata;
		CHECK(entry->size <= sizeof(file->bytes));
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset,
			    file->bytes + offset, (size_t)(entry->size - offset),
			    &length, NULL) == FSIMG_OK);
			CHECK(length != 0);
		}
	}
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	return (found);
}

static uint64_t
count_entries(fixture_t *fixture, const image_t *image)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	uint64_t count;
	fsimg_status_t status;

	memory_io(fixture, (image_t *)image, false, &input);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader,
	    NULL) == FSIMG_OK);
	count = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK)
		count++;
	CHECK(status == FSIMG_DONE);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	return (count);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	*length = image->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	CHECK(fsimg_range_fits(offset, length, image->length));
	bcopy(image->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	CHECK(fsimg_range_fits(offset, length, image->length));
	bcopy(buffer, image->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	CHECK(fsimg_range_fits(offset, length, image->length));
	bzero(image->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static void
memory_io(fixture_t *fixture, image_t *image, bool writable, fsimg_io_t **out)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	if (writable) {
		ops.write_at = memory_write;
		ops.zero_range = memory_zero;
		ops.flush = memory_flush;
	}
	CHECK(fsimg_io_from_callbacks(fixture->ctx, &ops, image, out,
	    NULL) == FSIMG_OK);
}

/* The reference generators, one byte at a time from the definitions: a
 * seeded byte at offset o is byte o mod 32 of SHA-256(LE64(seed) ||
 * LE64(o / 32)); a positional byte is byte o mod 16 of LE64(seed) ||
 * LE64(16 * (o / 16)).
 */
static unsigned char
seeded_byte(uint64_t seed, uint64_t offset)
{
	fsimg_sha256_t hash;
	unsigned char message[16], block[32];

	for (unsigned i = 0; i < 8; i++) {
		message[i] = (unsigned char)(seed >> (8 * i));
		message[8 + i] = (unsigned char)((offset / 32) >> (8 * i));
	}
	fsimg_sha256_init(&hash);
	fsimg_sha256_update(&hash, message, sizeof(message));
	fsimg_sha256_final(block, &hash);
	return (block[offset % 32]);
}

static unsigned char
positional_byte(uint64_t seed, uint64_t offset)
{
	uint64_t field;
	unsigned index;

	index = (unsigned)(offset % 16);
	field = index < 8 ? seed : offset - offset % 16;
	return ((unsigned char)(field >> (8 * (index % 8))));
}

static bool
in_holes(const uint64_t (*holes)[2], size_t count, uint64_t offset)
{
	for (size_t i = 0; i < count; i++) {
		if (offset >= holes[i][0] && offset - holes[i][0] < holes[i][1])
			return (true);
	}
	return (false);
}

static void
check_seeded(const found_file_t *file, uint64_t seed, uint64_t size,
    const uint64_t (*holes)[2], size_t hole_count)
{
	CHECK(file->kind == FSIMG_ENTRY_FILE && file->size == size);
	for (uint64_t offset = 0; offset < size; offset++) {
		if (in_holes(holes, hole_count, offset))
			CHECK(file->bytes[offset] == 0);
		else
			CHECK(file->bytes[offset] == seeded_byte(seed, offset));
	}
}

static void
put_file(int parent, const char *name, const char *contents)
{
	size_t length;
	int fd;

	fd = openat(parent, name,
	    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	length = strlen(contents);
	CHECK(write(fd, contents, length) == (ssize_t)length);
	CHECK(close(fd) == 0);
}

static int
put_directory(int parent, const char *name)
{
	int fd;

	CHECK(mkdirat(parent, name, 0700) == 0);
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	return (fd);
}

static void
append_text(char *buffer, size_t capacity, const char *text)
{
	size_t used, length;

	used = strlen(buffer);
	length = strlen(text);
	CHECK(length < capacity - used);
	bcopy(text, buffer + used, length + 1);
}

/* The byte offset at which line number line (from one) begins. */
static uint64_t
line_offset(const char *text, unsigned line)
{
	uint64_t offset;

	offset = 0;
	for (unsigned current = 1; current < line; current++)
		offset = (uint64_t)(strchr(text + offset, '\n') - text) + 1;
	return (offset);
}

/* Refusals happen at input, name the line by offset and number and the
 * entry by image path, never quote a host path, and fail the writer.
 */
static void
check_refusal(const refusal_t *refusal, int contents)
{
	fixture_t fixture;
	fsimg_error_t error;

	fixture_open(&fixture, "fat12", 0);
	CHECK(add_manifest(&fixture, refusal->manifest, contents,
	    &error) == refusal->status);
	check_named_line(refusal, &error);
	CHECK(fsimg_writer_add_manifest(fixture.writer, -1, -1,
	    NULL) == FSIMG_ESTATE);
	fixture_close(&fixture);
}

static void
check_named_line(const refusal_t *refusal, const fsimg_error_t *error)
{
	char prefix[32];

	CHECK(error->status == refusal->status);
	CHECK(error->offset_valid &&
	    error->offset == line_offset(refusal->manifest, refusal->line));
	snprintf(prefix, sizeof(prefix), "manifest line %u: ", refusal->line);
	CHECK(strncmp(error->message, prefix, strlen(prefix)) == 0);
	CHECK(strcmp(error->path, refusal->path) == 0);
	CHECK(strstr(error->message, "manifest-") == NULL);
}

/* Run a tool with stdout to output, or -1 when it is not installed. */
static int
run_tool(const char *const *argv, int output)
{
	posix_spawn_file_actions_t actions;
	pid_t pid;
	int result, status;

	CHECK(posix_spawn_file_actions_init(&actions) == 0);
	if (output != -1)
		CHECK(posix_spawn_file_actions_adddup2(&actions, output, 1) == 0);
	result = posix_spawnp(&pid, argv[0], &actions, NULL,
	    (char *const *)argv, environ);
	CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
	if (result == ENOENT)
		return (-1);
	CHECK(result == 0);
	CHECK(waitpid(pid, &status, 0) == pid);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : 128);
}

static void
remove_tree(const char *path)
{
	const char *argv[] = { "rm", "-rf", path, NULL };

	CHECK(run_tool(argv, -1) == 0);
}

/* Every generator and hole form, in a FAT image, read back byte for byte
 * against the reference generators; the manifest's digests come from the
 * harness's own generator, so a disagreement between the two would refuse
 * the manifest before any plan.
 */
static void
test_generated_files(void)
{
	static const uint64_t hole_10_20[][2] = { { 10, 20 } };
	static const uint64_t big_holes[][2] = { { 0, 4096 }, { 8192, 30000 } };
	fixture_t fixture;
	image_t image;
	found_file_t *file;

	file = malloc(sizeof(*file));
	CHECK(file != NULL);
	fixture_open(&fixture, "fat12", 0);
	CHECK(add_manifest(&fixture, generated_manifest, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);

	CHECK(find_file(&fixture, &image, "/zero", file));
	CHECK(file->kind == FSIMG_ENTRY_FILE && file->size == 5000);
	for (size_t i = 0; i < 5000; i++)
		CHECK(file->bytes[i] == 0);
	CHECK(find_file(&fixture, &image, "/seeded", file));
	check_seeded(file, 7, 100, NULL, 0);
	CHECK(find_file(&fixture, &image, "/holes", file));
	check_seeded(file, 7, 100, hole_10_20, 1);
	CHECK(find_file(&fixture, &image, "/big", file));
	check_seeded(file, 3, 70000, big_holes, 2);
	CHECK(find_file(&fixture, &image, "/positional", file));
	CHECK(file->size == 100);
	for (uint64_t offset = 0; offset < 100; offset++)
		CHECK(file->bytes[offset] == positional_byte(9, offset));
	CHECK(find_file(&fixture, &image, "/dir/empty", file));
	CHECK(file->kind == FSIMG_ENTRY_FILE && file->size == 0);
	/* The manifest's time reached the entry. */
	CHECK((file->metadata.valid & FSIMG_META_MODIFIED) != 0 &&
	    file->metadata.modified.seconds == 1000000000);
	CHECK(count_entries(&fixture, &image) == 7);

	free(image.bytes);
	fixture_close(&fixture);
	free(file);
}

/* A digest that disagrees with the generator is refused by planning, at
 * the line that claims it, for each generator: the manifest's import
 * succeeds, planning fails the writer, and no plan is kept.
 */
static void
test_generated_digest_refused(void)
{
	fixture_t fixture;
	fsimg_error_t error;
	const fsimg_plan_t *plan;
	static const refusal_t refusals[] = {
		{ ". type=dir\n"
		  "./s type=file fsimg.content=seeded fsimg.seed=8 size=100 "
		  "sha256digest=" SEEDED_7_100 "\n",
		  FSIMG_EINVAL, 2, "/s" },
		{ ". type=dir\n./d type=dir\n"
		  "./d/h type=file fsimg.content=seeded fsimg.seed=7 size=100 "
		  "fsimg.holes=10:21 sha256digest=" SEEDED_7_100_HOLE_10_20 "\n",
		  FSIMG_EINVAL, 3, "/d/h" },
		{ "./p type=file fsimg.content=positional fsimg.seed=9 size=99 "
		  "sha256digest=" POSITIONAL_9_100 "\n",
		  FSIMG_EINVAL, 1, "/p" },
		{ "/set type=file\n./z fsimg.content=zero size=5001 "
		  "sha256digest=" ZERO_5000 "\n",
		  FSIMG_EINVAL, 2, "/z" }
	};

	for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		fixture_open(&fixture, "fat12", 0);
		CHECK(add_manifest(&fixture, refusals[i].manifest, -1,
		    &error) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &plan,
		    &error) == refusals[i].status);
		check_named_line(&refusals[i], &error);
		CHECK(fsimg_writer_plan(fixture.writer, &plan,
		    NULL) == FSIMG_ESTATE);
		fixture_close(&fixture);
	}
}

/* Plan manifest in a fresh fixture, with an exact volume size and an
 * output limit (zero for neither), expect the planning refusal status, and
 * return the work the writer was charged.
 */
static uint64_t
refused_plan_work(const char *format, uint64_t volume_bytes,
    uint64_t max_output_bytes, const char *manifest, fsimg_status_t status)
{
	fsimg_write_options_t write_options;
	fsimg_options_t options;
	fixture_t fixture;
	fsimg_error_t error;
	const fsimg_plan_t *plan;
	uint64_t work;

	fsimg_options_init(&options);
	options.max_output_bytes = max_output_bytes;
	fixture_open_options(&fixture, format, 0, &options);
	fsimg_writer_free(fixture.writer);
	fsimg_write_options_init(&write_options);
	write_options.volume_bytes = volume_bytes;
	CHECK(fsimg_writer_new(fixture.ctx, format, &write_options,
	    &fixture.writer, NULL) == FSIMG_OK);
	CHECK(add_manifest(&fixture, manifest, -1, &error) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == status);
	/* The one limit a caller here sets is the output limit. */
	CHECK(status != FSIMG_ELIMIT || error.resource == FSIMG_RESOURCE_OUTPUT);
	work =fixture.writer->session.progress.work;
	fixture_close(&fixture);
	return (work);
}

/* A generated file's digest is hashed only after planning has accepted its
 * size: a 1 GiB seeded file claiming a wrong digest is refused as too large
 * for a 1 MiB FAT12 volume, and for a 1 MiB output limit on an
 * automatically sized exFAT volume, not as a mismatch. Hashing it would
 * charge the writer 16384 units of work, one per 64 KiB; keeping the claim
 * may add only a few units over planning the same file without a digest.
 */
static void
test_generated_digest_after_planning(void)
{
	static const struct {
		const char	*format;
		uint64_t	 volume_bytes;
		uint64_t	 max_output_bytes;
		fsimg_status_t	 status;
	} images[] = {
		{ "fat12", 1024 * 1024, 0, FSIMG_ERANGE },
		{ "exfat", 0, 1024 * 1024, FSIMG_ELIMIT }
	};
	static const char claimed[] =
	    ". type=dir\n"
	    "./small type=file fsimg.content=seeded fsimg.seed=7 size=100 "
	    "sha256digest=" SEEDED_7_100 "\n"
	    "./huge type=file fsimg.content=seeded fsimg.seed=1 "
	    "size=1073741824 sha256digest=" SEEDED_7_100 "\n";
	static const char unclaimed[] =
	    ". type=dir\n"
	    "./small type=file fsimg.content=seeded fsimg.seed=7 size=100 "
	    "sha256digest=" SEEDED_7_100 "\n"
	    "./huge type=file fsimg.content=seeded fsimg.seed=1 "
	    "size=1073741824\n";

	uint64_t with_claim, without_claim;

	for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
		with_claim = refused_plan_work(images[i].format,
		    images[i].volume_bytes, images[i].max_output_bytes, claimed,
		    images[i].status);
		without_claim = refused_plan_work(images[i].format,
		    images[i].volume_bytes, images[i].max_output_bytes, unclaimed,
		    images[i].status);
		CHECK(with_claim >= without_claim &&
		    with_claim - without_claim < 64);
	}
}

/* The source cursor serves generated content: next_range() tiles the file
 * into its DATA and ZERO runs, reads at scattered offsets match the
 * reference and read zero inside holes, a positional block decodes to its
 * seed and offset, and a link's target is served from its record.
 */
static void
test_generated_ranges(void)
{
	static const char manifest[] =
	    "/set type=file\n"
	    "./h fsimg.content=seeded fsimg.seed=5 size=100 "
	    "fsimg.holes=10:20,50:5\n"
	    "./p fsimg.content=positional fsimg.seed=77 size=64\n"
	    "./l type=link link=t\\040arget\n";
	static const uint64_t holes[][2] = { { 10, 20 }, { 50, 5 } };
	static const struct {
		uint64_t	 offset;
		uint64_t	 length;
		fsimg_range_kind_t kind;
	} tiles[] = {
		{ 0, 10, FSIMG_RANGE_DATA }, { 10, 20, FSIMG_RANGE_ZERO },
		{ 30, 20, FSIMG_RANGE_DATA }, { 50, 5, FSIMG_RANGE_ZERO },
		{ 55, 45, FSIMG_RANGE_DATA }
	};
	const fsimg_catalog_entry_t *entry;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;
	fsimg_range_t range;
	unsigned char bytes[100];
	uint64_t offset, state;
	size_t length, transferred;
	int fd;
	fixture_t fixture;

	fixture_open(&fixture, "fat12", 0);
	ctx = fixture.ctx;
	CHECK(fsimg_writer_collect_new(ctx, &writer, NULL) == FSIMG_OK);
	fd = write_manifest(&fixture, manifest);
	CHECK(fsimg_writer_add_manifest(writer, fd, -1, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	CHECK(fsimg_writer_prepare(writer, NULL, NULL) == FSIMG_OK);
	CHECK(writer->count == 3);

	/* Catalog order is path order: /h, /l, /p. */
	CHECK(fsimg_catalog_get(writer, 1, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->entry.path, "/h") == 0 &&
	    entry->storage == FSIMG_STORAGE_GENERATED);
	CHECK(fsimg_source_open(writer, 1, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
		CHECK(fsimg_source_range(writer, tiles[i].offset, &range,
		    NULL) == FSIMG_OK);
		CHECK(range.offset == tiles[i].offset &&
		    range.length == tiles[i].length && range.kind == tiles[i].kind);
	}
	CHECK(fsimg_source_range(writer, 100, &range, NULL) == FSIMG_DONE);
	/* A range asked for inside a run starts there. */
	CHECK(fsimg_source_range(writer, 12, &range, NULL) == FSIMG_OK);
	CHECK(range.offset == 12 && range.length == 18 &&
	    range.kind == FSIMG_RANGE_ZERO);
	state = 12345;
	for (unsigned trial = 0; trial < 200; trial++) {
		state = state * 6364136223846793005u + 1442695040888963407u;
		offset = (state >> 33) % 100;
		length = (size_t)((state >> 17) % 40) + 1;
		CHECK(fsimg_source_read(writer, offset, bytes, length,
		    &transferred, NULL) == FSIMG_OK);
		CHECK(transferred == (length < 100 - offset ? length :
		    100 - offset));
		for (size_t i = 0; i < transferred; i++)
			CHECK(bytes[i] == (in_holes(holes, 2, offset + i) ? 0 :
			    seeded_byte(5, offset + i)));
	}
	CHECK(fsimg_source_finish(writer, NULL) == FSIMG_OK);

	CHECK(fsimg_source_open(writer, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_source_range(writer, 0, &range, NULL) == FSIMG_OK);
	CHECK(range.length == 7 && range.kind == FSIMG_RANGE_DATA);
	CHECK(fsimg_source_read(writer, 0, bytes, 7, &transferred,
	    NULL) == FSIMG_OK);
	CHECK(transferred == 7 && memcmp(bytes, "t arget", 7) == 0);
	CHECK(fsimg_source_finish(writer, NULL) == FSIMG_OK);

	CHECK(fsimg_source_open(writer, 3, NULL) == FSIMG_OK);
	CHECK(fsimg_source_read(writer, 48, bytes, 16, &transferred,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_load_le64(bytes) == 77 &&
	    fsimg_load_le64(bytes + 8) == 48);
	CHECK(fsimg_source_finish(writer, NULL) == FSIMG_OK);

	fsimg_writer_free(writer);
	fixture_close(&fixture);
}

/* Host bytes beneath the content directory, by the entry's own path and
 * by contents=, with a size taken from the file when the manifest has
 * none and digests that the copy confirms. The content files' own owners,
 * modes and times never reach the image.
 */
static void
test_host_contents(void)
{
	static const char manifest[] =
	    "/set type=file\n"
	    ". type=dir\n"
	    "./a size=3 sha256digest=" SHA256_ABC "\n"
	    "./sub type=dir\n"
	    "./sub/b sha256digest=" SHA256_XY "\n"
	    "./renamed contents=other/c time=1000000000\n"
	    "./gen fsimg.content=seeded fsimg.seed=1 size=10\n";
	fixture_t fixture;
	image_t image;
	found_file_t *file;
	int contents, directory;

	file = malloc(sizeof(*file));
	CHECK(file != NULL);
	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	put_file(contents, "a", "abc");
	directory = put_directory(contents, "sub");
	put_file(directory, "b", "xy");
	CHECK(close(directory) == 0);
	directory = put_directory(contents, "other");
	put_file(directory, "c", "payload c");
	CHECK(close(directory) == 0);

	CHECK(add_manifest(&fixture, manifest, contents, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	CHECK(find_file(&fixture, &image, "/a", file));
	CHECK(file->size == 3 && memcmp(file->bytes, "abc", 3) == 0);
	CHECK((file->metadata.valid & FSIMG_META_MODIFIED) != 0 &&
	    file->metadata.modified.seconds == 315532800);
	CHECK(find_file(&fixture, &image, "/sub/b", file));
	CHECK(file->size == 2 && memcmp(file->bytes, "xy", 2) == 0);
	CHECK(find_file(&fixture, &image, "/renamed", file));
	CHECK(file->size == 9 && memcmp(file->bytes, "payload c", 9) == 0);
	CHECK(file->metadata.modified.seconds == 1000000000);
	CHECK(find_file(&fixture, &image, "/gen", file));
	check_seeded(file, 1, 10, NULL, 0);
	CHECK(count_entries(&fixture, &image) == 5);

	free(image.bytes);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);
	free(file);
}

/* Each contents path that would follow a link, leave the directory or
 * name something other than a regular file is refused at its line, and so
 * is any entry needing host bytes when there is no content directory.
 */
static void
test_host_refusals(void)
{
	static const refusal_t refusals[] = {
		{ ". type=dir\n./x type=file contents=ln/inner\n",
		  FSIMG_EUNSUPPORTED, 2, "/x" },
		{ ". type=dir\n./x type=file contents=lf\n",
		  FSIMG_EUNSUPPORTED, 2, "/x" },
		{ "./lf type=file\n", FSIMG_EUNSUPPORTED, 1, "/lf" },
		{ ". type=dir\n./x type=file contents=d\n",
		  FSIMG_EUNSUPPORTED, 2, "/x" },
		{ ". type=dir\n./x type=file contents=p\n",
		  FSIMG_EUNSUPPORTED, 2, "/x" },
		{ ". type=dir\n./x type=file contents=s\n",
		  FSIMG_EUNSUPPORTED, 2, "/x" },
		{ ". type=dir\n./x type=file contents=missing\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ ". type=dir\n./x type=file contents=f/inner\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ ". type=dir\n./x type=file contents=d/" LONG_NAME "\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ ". type=dir\n./x type=file contents=d/../f\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ ". type=dir\n./x type=file contents=/f\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ ". type=dir\n./x type=file size=2 contents=f\n",
		  FSIMG_EINVAL, 2, "/x" },
		{ "./d type=dir\n./d/inner type=file size=4\n",
		  FSIMG_EINVAL, 2, "/d/inner" }
	};
	static const refusal_t descriptorless = {
		"./ok type=file fsimg.content=zero size=1\n./f type=file\n",
		FSIMG_EINVAL, 2, "/f"
	};
	fixture_t fixture;
	int contents, directory, listener;

	/* The content tree lives in its own fixture; each refusal opens a
	 * writer of its own beside it.
	 */
	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	put_file(contents, "f", "abc");
	directory = put_directory(contents, "d");
	put_file(directory, "inner", "in");
	CHECK(close(directory) == 0);
	CHECK(symlinkat("d", contents, "ln") == 0);
	CHECK(symlinkat("f", contents, "lf") == 0);
	CHECK(mkfifoat(contents, "p", 0600) == 0);
	listener = bind_socket(contents, "s");

	for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++)
		check_refusal(&refusals[i], contents);
	check_refusal(&descriptorless, -1);

	CHECK(close(listener) == 0);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);
}

/* A host file's digest is checked by the copy, not at input: planning
 * succeeds and reads no content byte, and emission fails as a changed
 * source naming the entry. A file replaced after collection fails the same
 * way even without a digest.
 */
static void
test_host_digest_at_emission(void)
{
	fixture_t fixture;
	image_t image;
	fsimg_stats_t collected, planned;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	int contents;

	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	put_file(contents, "f", "abd");
	CHECK(add_manifest(&fixture, "./f type=file sha256digest=" SHA256_ABC
	    "\n", contents, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &collected, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &planned, NULL) == FSIMG_OK);
	CHECK(planned.read_bytes == collected.read_bytes);
	CHECK(emit_image(&fixture, &image, &error) == FSIMG_ESOURCE_CHANGED);
	CHECK(error.operation == FSIMG_OP_SOURCE);
	CHECK(strcmp(error.path, "/f") == 0);
	free(image.bytes);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	put_file(contents, "f", "abc");
	CHECK(add_manifest(&fixture, "./f type=file\n", contents,
	    NULL) == FSIMG_OK);
	CHECK(unlinkat(contents, "f", 0) == 0);
	put_file(contents, "f", "abc");
	CHECK(emit_image(&fixture, &image, &error) == FSIMG_ESOURCE_CHANGED);
	CHECK(strcmp(error.path, "/f") == 0);
	free(image.bytes);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);
}

/* Finishing a cursor whose file carries a digest asserts that every byte
 * was served in order: a cursor that served nothing, or a prefix that
 * skipped the start, fails as an internal fault instead of passing an
 * unchecked claim; serving the whole file in order checks it.
 */
static void
test_host_digest_needs_whole_file(void)
{
	fixture_t fixture;
	fsimg_writer_t *writer;
	unsigned char bytes[3];
	size_t transferred;
	int contents, fd;

	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	put_file(contents, "f", "abc");
	fd = write_manifest(&fixture, "./f type=file sha256digest=" SHA256_ABC
	    "\n");
	for (unsigned served = 0; served < 3; served++) {
		CHECK(fsimg_writer_collect_new(fixture.ctx, &writer,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_add_manifest(writer, fd, contents,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_prepare(writer, NULL, NULL) == FSIMG_OK);
		CHECK(fsimg_source_open(writer, 1, NULL) == FSIMG_OK);
		if (served == 1)
			CHECK(fsimg_source_read(writer, 1, bytes, 2, &transferred,
			    NULL) == FSIMG_OK && transferred == 2);
		if (served == 2)
			CHECK(fsimg_source_read(writer, 0, bytes, 3, &transferred,
			    NULL) == FSIMG_OK && transferred == 3);
		CHECK(fsimg_source_finish(writer, NULL) ==
		    (served == 2 ? FSIMG_OK : FSIMG_ECORRUPT));
		fsimg_writer_free(writer);
	}
	CHECK(close(fd) == 0);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);
}

/* A manifest root merges with a host-tree root by the one merging rule: a
 * directory both supply holds the union of their children, and a file
 * both supply is a collision naming its path.
 */
static void
test_merge_with_tree(void)
{
	static const char manifest[] =
	    "./EFI type=dir\n"
	    "./EFI/B.TXT type=file fsimg.content=seeded fsimg.seed=2 size=40\n"
	    "./M.TXT type=file fsimg.content=zero size=3\n";
	fixture_t fixture;
	image_t image;
	found_file_t *file;
	fsimg_error_t error;
	int efi, tree;

	file = malloc(sizeof(*file));
	CHECK(file != NULL);
	fixture_open(&fixture, "fat12", 0);
	tree = put_directory(fixture.parent, "tree");
	efi = put_directory(tree, "EFI");
	put_file(efi, "A.TXT", "alpha");
	CHECK(close(efi) == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, tree, "/", NULL) == FSIMG_OK);
	CHECK(add_manifest(&fixture, manifest, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	CHECK(find_file(&fixture, &image, "/EFI/A.TXT", file));
	CHECK(file->size == 5 && memcmp(file->bytes, "alpha", 5) == 0);
	CHECK(find_file(&fixture, &image, "/EFI/B.TXT", file));
	check_seeded(file, 2, 40, NULL, 0);
	CHECK(find_file(&fixture, &image, "/M.TXT", file));
	CHECK(file->size == 3);
	CHECK(count_entries(&fixture, &image) == 4);
	free(image.bytes);

	fixture_writer(&fixture, "fat12", 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, tree, "/", NULL) == FSIMG_OK);
	CHECK(add_manifest(&fixture, "./EFI type=dir\n"
	    "./EFI/A.TXT type=file fsimg.content=zero size=1\n", -1,
	    NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "/EFI/A.TXT") == 0);
	free(image.bytes);
	CHECK(close(tree) == 0);
	fixture_close(&fixture);
	free(file);
}

/* Within one manifest a directory repeated with the same effective
 * keywords merges with itself, in either line form; any other repeat is a
 * collision at planning naming the path, as is a missing parent.
 */
static void
test_duplicates(void)
{
	static const struct {
		const char	*manifest;
		fsimg_status_t	 status;
		const char	*path;
	} cases[] = {
		{ ". type=dir\nd type=dir mode=0755\n..\n./d type=dir mode=0755\n"
		  "./d/f type=file fsimg.content=zero size=1\n", FSIMG_OK, NULL },
		{ "./d type=dir mode=0755\n./d type=dir mode=0700\n",
		  FSIMG_EINVAL, "/d" },
		{ "./f type=file fsimg.content=zero size=1\n"
		  "./f type=file fsimg.content=zero size=1\n", FSIMG_EINVAL, "/f" },
		{ "./d type=dir\n./d type=file fsimg.content=zero size=1\n",
		  FSIMG_EINVAL, "/d" },
		{ "./a/b type=dir\n", FSIMG_EINVAL, "/a/b" }
	};
	fixture_t fixture;
	image_t image;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture, "fat12", 0);
		CHECK(add_manifest(&fixture, cases[i].manifest, -1,
		    NULL) == FSIMG_OK);
		CHECK(emit_image(&fixture, &image, &error) == cases[i].status);
		if (cases[i].path != NULL)
			CHECK(strcmp(error.path, cases[i].path) == 0);
		else
			CHECK(count_entries(&fixture, &image) == 2);
		free(image.bytes);
		fixture_close(&fixture);
	}
}

/* The catalog sorts, so the order of a manifest's lines never changes the
 * image: the same full-form lines forward and reversed give the same
 * bytes, and a changed keyword gives different ones.
 */
static void
test_line_order(void)
{
	static const char *const lines[] = {
		"./a type=dir time=1200000000\n",
		"./a/one type=file fsimg.content=seeded fsimg.seed=1 size=3000 "
		"time=1200000002\n",
		"./a/two type=file fsimg.content=positional fsimg.seed=2 size=700\n",
		"./b type=dir\n",
		"./b/three type=file fsimg.content=zero size=9000\n",
		"./b/four type=file fsimg.content=seeded fsimg.seed=4 size=64 "
		"time=1300000000\n"
	};
	static const char changed_line[] =
	    "./b/four type=file fsimg.content=seeded fsimg.seed=5 size=64 "
	    "time=1300000000\n";
	const size_t count = sizeof(lines) / sizeof(lines[0]);
	char forward[1024], reversed[1024], changed[1024];
	fixture_t fixture;
	image_t images[3];

	forward[0] = reversed[0] = changed[0] = '\0';
	for (size_t i = 0; i < count; i++) {
		append_text(forward, sizeof(forward), lines[i]);
		append_text(reversed, sizeof(reversed), lines[count - 1 - i]);
		append_text(changed, sizeof(changed),
		    i == count - 1 ? changed_line : lines[i]);
	}

	fixture_open(&fixture, "fat16", 0);
	CHECK(add_manifest(&fixture, forward, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &images[0], NULL) == FSIMG_OK);
	fixture_writer(&fixture, "fat16", 0);
	CHECK(add_manifest(&fixture, reversed, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &images[1], NULL) == FSIMG_OK);
	fixture_writer(&fixture, "fat16", 0);
	CHECK(add_manifest(&fixture, changed, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &images[2], NULL) == FSIMG_OK);
	fixture_close(&fixture);

	CHECK(images[0].length == images[1].length &&
	    bcmp(images[0].bytes, images[1].bytes, (size_t)images[0].length) == 0);
	CHECK(images[0].length != images[2].length ||
	    bcmp(images[0].bytes, images[2].bytes, (size_t)images[0].length) != 0);
	for (size_t i = 0; i < 3; i++)
		free(images[i].bytes);
}

/* A manifest that needs no host bytes leaves an offered content directory
 * untouched: its duplicate is closed when the call returns, and the image
 * is the one built without a directory.
 */
static void
test_unused_content_directory(void)
{
	fixture_t fixture;
	image_t with, without;
	fsimg_stats_t stats;
	int contents;

	fixture_open(&fixture, "fat12", 0);
	contents = put_directory(fixture.parent, "contents");
	CHECK(add_manifest(&fixture, generated_manifest, contents,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == fixture.baseline.descriptors);
	CHECK(emit_image(&fixture, &with, NULL) == FSIMG_OK);
	CHECK(close(contents) == 0);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", 0);
	CHECK(add_manifest(&fixture, generated_manifest, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &without, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(with.length == without.length &&
	    bcmp(with.bytes, without.bytes, (size_t)with.length) == 0);
	free(with.bytes);
	free(without.bytes);
}

/* The adopting form the worker uses consumes both reserved descriptors on
 * every outcome: an offered content directory that is not a directory, a
 * manifest that is not a regular file and a writer no longer collecting
 * are refused with both descriptors closed and both reservations
 * returned, and an accepted pair imports, its unused directory released.
 */
static void
test_adopted_descriptors(void)
{
	const fsimg_plan_t *plan;
	fixture_t fixture;
	fsimg_error_t error;
	fsimg_stats_t stats;
	image_t image;
	int contents, manifest;

	fixture_open(&fixture, "fat12", 0);
	put_file(fixture.parent, "plain", "x");
	manifest = write_manifest(&fixture, generated_manifest);
	contents = openat(fixture.parent, "plain", O_RDONLY | O_CLOEXEC);
	CHECK(contents != -1);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_adopt_manifest(fixture.writer, manifest, contents,
	    &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "readable directory") != NULL);
	CHECK(fcntl(manifest, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(contents, F_GETFD) == -1 && errno == EBADF);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == fixture.baseline.descriptors);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", 0);
	manifest = put_directory(fixture.parent, "not-a-manifest");
	contents = put_directory(fixture.parent, "contents");
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_adopt_manifest(fixture.writer, manifest, contents,
	    &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "not a regular file") != NULL);
	CHECK(fcntl(manifest, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(contents, F_GETFD) == -1 && errno == EBADF);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == fixture.baseline.descriptors);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", 0);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	manifest = write_manifest(&fixture, generated_manifest);
	contents = openat(fixture.parent, ".", O_RDONLY | O_DIRECTORY |
	    O_CLOEXEC);
	CHECK(contents != -1);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_adopt_manifest(fixture.writer, manifest, contents,
	    NULL) != FSIMG_OK);
	CHECK(fcntl(manifest, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(contents, F_GETFD) == -1 && errno == EBADF);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == fixture.baseline.descriptors);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", 0);
	manifest = write_manifest(&fixture, generated_manifest);
	contents = put_directory(fixture.parent, "contents");
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_fd_reserve(fixture.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_adopt_manifest(fixture.writer, manifest, contents,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == fixture.baseline.descriptors);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	free(image.bytes);
	fixture_close(&fixture);
}

/* A format that stores owner, mode, links and FIFOs carries the
 * manifest's keywords exactly, with no host object behind them.
 */
static void
test_qnx6_metadata(void)
{
	static const char manifest[] =
	    "/set uid=0 gid=0 time=1500000000\n"
	    "./etc type=dir mode=0700 uid=5 gid=6\n"
	    "./etc/su type=file mode=04755 fsimg.content=seeded fsimg.seed=6 "
	    "size=2000\n"
	    "./etc/sh type=link link=../bin/k\\040sh\n"
	    "./etc/log type=fifo mode=0620\n";
	fixture_t fixture;
	image_t image;
	found_file_t *file;

	file = malloc(sizeof(*file));
	CHECK(file != NULL);
	fixture_open(&fixture, "qnx6", 0);
	CHECK(add_manifest(&fixture, manifest, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	CHECK(find_file(&fixture, &image, "/etc", file));
	CHECK(file->kind == FSIMG_ENTRY_DIRECTORY &&
	    file->metadata.mode == 0700 && file->metadata.uid == 5 &&
	    file->metadata.gid == 6);
	CHECK(find_file(&fixture, &image, "/etc/su", file));
	check_seeded(file, 6, 2000, NULL, 0);
	CHECK(file->metadata.mode == 04755 && file->metadata.uid == 0 &&
	    file->metadata.modified.seconds == 1500000000);
	CHECK(find_file(&fixture, &image, "/etc/sh", file));
	CHECK(file->kind == FSIMG_ENTRY_SYMLINK && file->size == 11 &&
	    memcmp(file->bytes, "../bin/k sh", 11) == 0);
	CHECK(find_file(&fixture, &image, "/etc/log", file));
	CHECK(file->kind == FSIMG_ENTRY_FIFO && file->metadata.mode == 0620);
	free(image.bytes);
	fixture_close(&fixture);
	free(file);
}

/* Round trips through two independent writers of the format. For a host
 * tree, mtree -c and bsdtar --format=mtree each write a manifest; the
 * manifest plus the tree as its content directory must build the image
 * that importing the tree itself builds, byte for byte under reproducible
 * mode, and extracting it must give back the tree's bytes. A writer that
 * is not installed is skipped with a note.
 */
static void
test_external_writers(void)
{
	char tree_path[128];
	/*
	 * mtree(8)'s default keywords less flags, plus sha256digest: FreeBSD's
	 * tool can write flags=uarch, a value the parser refuses.
	 */
	const char *const mtree_argv[] = {
		"mtree", "-c", "-k",
		"type,mode,uid,gid,nlink,size,link,time,sha256digest",
		"-p", tree_path, NULL
	};
	const char *const bsdtar_argv[] = {
		"bsdtar", "--format=mtree", "--options=mtree:sha256", "-cf", "-",
		"-C", tree_path, ".", NULL
	};
	fixture_t fixture;
	image_t from_tree;
	int big, deep, space, sub, tree;

	fixture_open(&fixture, "fat16", FSIMG_WRITE_REPRODUCIBLE);
	tree = put_directory(fixture.parent, "tree");
	put_file(tree, "a", "abc");
	space = put_directory(tree, "sp ace");
	put_file(space, "f#1", "xy");
	sub = put_directory(tree, "sub");
	put_file(sub, "empty", "");
	deep = put_directory(sub, "deep");
	put_file(deep, "caf\303\251", "q");
	big = openat(deep, "big", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(big != -1);
	for (unsigned i = 0; i < 3000; i++)
		CHECK(write(big, "0123456789abcdefghijklmnopqrstuv", 32) == 32);
	CHECK(close(big) == 0);
	CHECK(close(deep) == 0);
	CHECK(close(sub) == 0);
	CHECK(close(space) == 0);
	CHECK(fsimg_writer_add_tree(fixture.writer, tree, "/", NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &from_tree, NULL) == FSIMG_OK);
	snprintf(tree_path, sizeof(tree_path), "%s/tree", fixture.directory);

	rebuild_from_tool(&fixture, mtree_argv, tree, &from_tree);
	rebuild_from_tool(&fixture, bsdtar_argv, tree, &from_tree);
	free(from_tree.bytes);
	CHECK(close(tree) == 0);
	fixture_close(&fixture);
}

/* Write the tool's manifest of the tree, build an image from it with the
 * tree as the content directory, require it equal to the image importing
 * the tree built, and extract it to check the files' bytes.
 */
static void
rebuild_from_tool(fixture_t *fixture, const char *const *argv, int tree,
    const image_t *from_tree)
{
	static const struct {
		const char	*path;
		const char	*bytes;
	} tree_files[] = {
		{ "a", "abc" }, { "sp ace/f#1", "xy" }, { "sub/empty", "" },
		{ "sub/deep/caf\303\251", "q" }
	};
	char name[64], buffer[64];
	fsimg_extract_options_t extract;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	image_t image;
	ssize_t length;
	int extracted, fd, result;

	snprintf(name, sizeof(name), "%s.mtree", argv[0]);
	fd = openat(fixture->parent, name,
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	result = run_tool(argv, fd);
	CHECK(close(fd) == 0);
	if (result == -1) {
		printf("skip: %s is not installed\n", argv[0]);
		return;
	}
	CHECK(result == 0);

	fixture_writer(fixture, "fat16", FSIMG_WRITE_REPRODUCIBLE);
	fd = openat(fixture->parent, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_writer_add_manifest(fixture->writer, fd, tree,
	    NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	CHECK(emit_image(fixture, &image, NULL) == FSIMG_OK);
	CHECK(image.length == from_tree->length &&
	    bcmp(image.bytes, from_tree->bytes, (size_t)image.length) == 0);

	memory_io(fixture, &image, false, &input);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader,
	    NULL) == FSIMG_OK);
	fsimg_extract_options_init(&extract);
	snprintf(name, sizeof(name), "%s.out", argv[0]);
	CHECK(fsimg_extract_at(reader, fixture->parent, name, &extract,
	    NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	extracted = openat(fixture->parent, name,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(extracted != -1);
	for (size_t i = 0; i < sizeof(tree_files) / sizeof(tree_files[0]); i++) {
		fd = openat(extracted, tree_files[i].path,
		    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(fd != -1);
		length = read(fd, buffer, sizeof(buffer));
		CHECK(length == (ssize_t)strlen(tree_files[i].bytes) &&
		    memcmp(buffer, tree_files[i].bytes, (size_t)length) == 0);
		CHECK(close(fd) == 0);
	}
	CHECK(close(extracted) == 0);
	free(image.bytes);
}

/* A manifest x --mtree must carry back into an image: names that need
 * escapes, every generator and a hole, a deep directory, and on QNX the
 * owners, modes, links and FIFO the safe extractor cannot create.
 *
 * The FAT and exFAT manifest has no nonempty zero-content file, which
 * exFAT cannot carry back byte for byte (test_export_exfat_zero_file).
 */
static const char export_manifest_fat[] =
    "/set uid=0 gid=0 mode=0644 time=1500000000\n"
    "./sp\\040ace type=dir time=1400000000\n"
    "./sp\\040ace/hash\\0431 type=file fsimg.content=seeded fsimg.seed=4 "
    "size=70000 fsimg.holes=4096:40000\n"
    "./caf\\303\\251 type=file fsimg.content=positional fsimg.seed=8 "
    "size=300\n"
    "./empty type=file fsimg.content=zero size=0\n"
    "./a type=dir\n"
    "./a/b type=dir\n"
    "./a/b/c type=file fsimg.content=seeded fsimg.seed=1 size=10\n";

static const char export_manifest_qnx[] =
    "/set uid=0 gid=0 mode=0644 time=1500000000\n"
    "./sp\\040ace type=dir mode=0750 uid=7 gid=8 time=1400000000\n"
    "./sp\\040ace/hash\\0431 type=file fsimg.content=seeded fsimg.seed=4 "
    "size=70000 fsimg.holes=4096:40000\n"
    "./back\\\\slash type=file mode=04755 "
    "fsimg.content=positional fsimg.seed=8 size=300\n"
    "./caf\\303\\251 type=link link=../t\\040a\\\\r\\043get\n"
    "./log type=fifo mode=0620 uid=3 gid=4\n"
    "./zero type=file fsimg.content=zero size=5000\n"
    "./a type=dir\n"
    "./a/b type=dir\n"
    "./a/b/c type=file fsimg.content=seeded fsimg.seed=1 size=10\n";

/* Extract image into a new directory name beside a new manifest
 * name.mtree, skipping the kinds the safe extractor cannot create, and
 * return the manifest's text, which the caller frees.
 */
static char *
extract_with_manifest(fixture_t *fixture, const image_t *image,
    const char *name)
{
	char path[64];
	fsimg_extract_options_t options;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	struct stat st;
	char *text;
	int fd;

	snprintf(path, sizeof(path), "%s.mtree", name);
	fd = openat(fixture->parent, path,
	    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	memory_io(fixture, (image_t *)image, false, &input);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader,
	    NULL) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	CHECK(options.manifest_fd == -1);
	options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	options.manifest_fd = fd;
	CHECK(fsimg_extract_at(reader, fixture->parent, name, &options,
	    NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fsimg_io_free(input);

	CHECK(fstat(fd, &st) == 0);
	text = malloc((size_t)st.st_size + 1);
	CHECK(text != NULL);
	CHECK(pread(fd, text, (size_t)st.st_size, 0) == st.st_size);
	text[st.st_size] = '\0';
	CHECK(close(fd) == 0);
	return (text);
}

/* Build an image from the manifest name.mtree, with the extracted tree
 * name as its content directory.
 */
static void
rebuild_from_extraction(fixture_t *fixture, const char *format, uint32_t flags,
    const char *name, image_t *image)
{
	char path[64];
	int manifest, tree;

	fixture_writer(fixture, format, flags);
	snprintf(path, sizeof(path), "%s.mtree", name);
	manifest = openat(fixture->parent, path, O_RDONLY | O_CLOEXEC);
	CHECK(manifest != -1);
	tree = openat(fixture->parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(tree != -1);
	CHECK(fsimg_writer_add_manifest(fixture->writer, manifest, tree,
	    NULL) == FSIMG_OK);
	CHECK(close(manifest) == 0);
	CHECK(close(tree) == 0);
	CHECK(emit_image(fixture, image, NULL) == FSIMG_OK);
}

/* The initialized payload bytes the fixture's writer plans. */
static uint64_t
initialized_bytes(fixture_t *fixture)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	CHECK(fsimg_writer_plan(fixture->writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	return (info.initialized_bytes);
}

/*
 * What exFAT cannot carry back. Its planning gives a generated zero file
 * an initialized length of zero but a host file its full length, holes or
 * not, since host contents are copied rather than known to be zero. The
 * extracted copy of a nonempty zero file is a host file, so the image
 * rebuilt from it initializes the file, and the images differ, while the
 * manifests, which carry no initialized length, still agree.
 */
static void
test_export_exfat_zero_file(void)
{
	static const char source[] =
	    "/set uid=0 gid=0 mode=0644 time=1500000000\n"
	    "./zero type=file fsimg.content=zero size=5000\n";
	fixture_t fixture;
	image_t original, rebuilt;
	char *first, *second;

	fixture_open(&fixture, "exfat", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(initialized_bytes(&fixture) == 0);
	CHECK(emit_image(&fixture, &original, NULL) == FSIMG_OK);
	first = extract_with_manifest(&fixture, &original, "zero");

	rebuild_from_extraction(&fixture, "exfat", FSIMG_WRITE_REPRODUCIBLE,
	    "zero", &rebuilt);
	CHECK(initialized_bytes(&fixture) == 5000);
	CHECK(rebuilt.length == original.length &&
	    bcmp(rebuilt.bytes, original.bytes, (size_t)original.length) != 0);
	second = extract_with_manifest(&fixture, &rebuilt, "again");
	CHECK(strcmp(first, second) == 0);
	free(first);
	free(second);
	free(original.bytes);
	free(rebuilt.bytes);
	fixture_close(&fixture);
}

/* The round trip x --mtree promises. Under reproducible mode the image
 * built from an extracted tree and its manifest equals the original byte
 * for byte, and extracting it again writes the same manifest; without it,
 * times survive too, so the second extraction's manifest equals the
 * first. Two extractions of one image write identical manifests.
 */
static void
test_export_round_trip(const char *format, const char *source)
{
	fixture_t fixture;
	image_t original, rebuilt;
	char *first, *again, *second;

	fixture_open(&fixture, format, FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &original, NULL) == FSIMG_OK);
	first = extract_with_manifest(&fixture, &original, "first");
	again = extract_with_manifest(&fixture, &original, "again");
	CHECK(strcmp(first, again) == 0);
	rebuild_from_extraction(&fixture, format, FSIMG_WRITE_REPRODUCIBLE,
	    "first", &rebuilt);
	CHECK(rebuilt.length == original.length &&
	    bcmp(rebuilt.bytes, original.bytes, (size_t)original.length) == 0);
	second = extract_with_manifest(&fixture, &rebuilt, "second");
	CHECK(strcmp(first, second) == 0);
	free(first);
	free(again);
	free(second);
	free(original.bytes);
	free(rebuilt.bytes);

	fixture_writer(&fixture, format, 0);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &original, NULL) == FSIMG_OK);
	first = extract_with_manifest(&fixture, &original, "timed");
	CHECK(strstr(first, "./sp\\040ace type=dir") != NULL &&
	    strstr(first, " time=1400000000.000000000\n") != NULL);
	rebuild_from_extraction(&fixture, format, 0, "timed", &rebuilt);
	second = extract_with_manifest(&fixture, &rebuilt, "retimed");
	CHECK(strcmp(first, second) == 0);
	free(first);
	free(second);
	free(original.bytes);
	free(rebuilt.bytes);
	fixture_close(&fixture);
}

/* The seeded failures of the round trip: a byte changed in the extracted
 * tree fails its recorded digest during the copy, and a keyword changed in
 * the written manifest changes the image.
 */
static void
test_export_changes_detected(void)
{
	fixture_t fixture;
	image_t original, rebuilt;
	char *text, *mode;
	unsigned char byte;
	int fd, tree;

	fixture_open(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, export_manifest_qnx, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &original, NULL) == FSIMG_OK);
	text = extract_with_manifest(&fixture, &original, "changed");
	tree = openat(fixture.parent, "changed",
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(tree != -1);

	fd = openat(tree, "a/b/c", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(pread(fd, &byte, 1, 0) == 1);
	byte ^= 1;
	CHECK(pwrite(fd, &byte, 1, 0) == 1);
	fixture_writer(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, text, tree, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &rebuilt, NULL) == FSIMG_ESOURCE_CHANGED);
	free(rebuilt.bytes);
	byte ^= 1;
	CHECK(pwrite(fd, &byte, 1, 0) == 1);
	CHECK(close(fd) == 0);

	mode = strstr(text, "mode=0750");
	CHECK(mode != NULL);
	mode[strlen("mode=075")] = '1';
	fixture_writer(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, text, tree, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &rebuilt, NULL) == FSIMG_OK);
	CHECK(rebuilt.length == original.length &&
	    bcmp(rebuilt.bytes, original.bytes, (size_t)original.length) != 0);
	free(rebuilt.bytes);

	/* Restored, the tree and manifest build the original again. */
	mode[strlen("mode=075")] = '0';
	fixture_writer(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, text, tree, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &rebuilt, NULL) == FSIMG_OK);
	CHECK(rebuilt.length == original.length &&
	    bcmp(rebuilt.bytes, original.bytes, (size_t)original.length) == 0);

	CHECK(close(tree) == 0);
	free(text);
	free(original.bytes);
	free(rebuilt.bytes);
	fixture_close(&fixture);
}

/* A manifest longer than the writer's 64 KiB buffer is written in several
 * pieces, each growing the file before it is written, and reads back as
 * the image it describes: 800 files with 100-byte names make about
 * 150 KiB of lines.
 */
static void
test_export_long_manifest(void)
{
	fixture_t fixture;
	image_t original, rebuilt;
	char *source, *text;

	source = long_source();
	fixture_open(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &original, NULL) == FSIMG_OK);
	text = extract_with_manifest(&fixture, &original, "long");
	CHECK(strlen(text) > 2 * 65536);
	rebuild_from_extraction(&fixture, "qnx6", FSIMG_WRITE_REPRODUCIBLE,
	    "long", &rebuilt);
	CHECK(rebuilt.length == original.length &&
	    bcmp(rebuilt.bytes, original.bytes, (size_t)original.length) == 0);
	free(text);
	free(source);
	free(original.bytes);
	free(rebuilt.bytes);
	fixture_close(&fixture);
}

/* A source manifest of 800 files with 103-byte names, whose exported
 * manifest outgrows two of the writer's 64 KiB buffers.
 */
static char *
long_source(void)
{
	char line[192];
	char *source;
	size_t capacity;

	capacity = 800 * sizeof(line) + 64;
	source = malloc(capacity);
	CHECK(source != NULL);
	snprintf(source, capacity, "./d type=dir\n");
	for (unsigned i = 0; i < 800; i++) {
		snprintf(line, sizeof(line), "./d/" LONG_NAME_100 "%03u type=file "
		    "fsimg.content=seeded fsimg.seed=%u size=%u\n", i, i, 1 + i % 7);
		append_text(source, capacity, line);
	}
	return (source);
}

/* Count a call of kind and say whether it is the one to fail. */
static bool
sink_fails(failing_sink_t *sink, sink_call_t kind)
{
	return (++sink->calls[kind] == sink->fail_at && kind == sink->fail_kind);
}

static fsimg_status_t
sink_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((failing_sink_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_resize(void *argument, uint64_t length, fsimg_error_t *error)
{
	failing_sink_t *sink = argument;

	if (sink_fails(sink, SINK_RESIZE))
		return (fsimg_error_set(error, FSIMG_EIO, ENOSPC, FSIMG_OP_IO,
		    "injected resize failure"));
	sink->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_write(void *argument, uint64_t offset, const void *buffer FSIMG_UNUSED,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	failing_sink_t *sink = argument;

	*transferred = 0;
	if (sink_fails(sink, SINK_WRITE))
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected write failure"));
	CHECK(offset + length <= sink->length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_flush(void *argument, fsimg_error_t *error)
{
	failing_sink_t *sink = argument;

	if (sink_fails(sink, SINK_FLUSH))
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected flush failure"));
	return (FSIMG_OK);
}

static bool
poll_cancelled(void *argument)
{
	poll_counter_t *counter = argument;

	return (counter->cancel_at != 0 && ++counter->polls >= counter->cancel_at);
}

/* Write a manifest of 2000 directory lines with 200-byte paths, more than
 * six of the writer's 64 KiB buffers, to sink; the writer is freed
 * whatever happens, so the fixture's accounting shows any leak.
 */
static fsimg_status_t
write_lines(fixture_t *fixture, failing_sink_t *sink)
{
	char path[256];
	fsimg_mtree_writer_t *writer;
	fsimg_mtree_record_t record;
	fsimg_session_t session;
	fsimg_entry_t entry;
	fsimg_io_ops_t ops;
	fsimg_io_t *output;
	fsimg_status_t status;

	fsimg_io_ops_init(&ops);
	ops.flags = FSIMG_IO_DURABLE;
	ops.get_size = sink_size;
	ops.resize = sink_resize;
	ops.write_at = sink_write;
	ops.flush = sink_flush;
	status = fsimg_io_from_callbacks(fixture->ctx, &ops, sink, &output, NULL);
	if (status != FSIMG_OK)
		return (status);
	fsimg_session_init(&session, fixture->ctx, "manifest", 0);
	status = fsimg_mtree_writer_start(fixture->ctx, &session, output, &writer,
	    NULL);
	if (status != FSIMG_OK)
		return (status);
	bzero(&entry, sizeof(entry));
	entry.kind = FSIMG_ENTRY_DIRECTORY;
	entry.path = path;
	bzero(&record, sizeof(record));
	record.entry = &entry;
	for (unsigned i = 0; i < 2000; i++) {
		snprintf(path, sizeof(path), "/%04u" LONG_NAME_100 LONG_NAME_10
		    LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10
		    LONG_NAME_10 LONG_NAME_10 LONG_NAME_10 LONG_NAME_10, i);
		status = fsimg_mtree_writer_add(writer, &record, NULL);
		if (status != FSIMG_OK) {
			fsimg_mtree_writer_free(writer);
			return (status);
		}
	}
	return (fsimg_mtree_writer_finish(writer, NULL));
}

/*
 * Every way the manifest's output can fail reaches the caller as that
 * failure, at once, and leaves nothing allocated: each resize, each write
 * and each flush a clean run makes failing in turn, the context cancelled
 * at each poll, and each allocation the writer makes failing.
 */
static void
test_export_writer_faults(void)
{
	failing_sink_t clean, sink;
	poll_counter_t counter;
	fsimg_options_t options;
	fixture_t fixture;
	fsimg_status_t status;
	unsigned polls;

	bzero(&counter, sizeof(counter));
	fsimg_options_init(&options);
	options.cancelled = poll_cancelled;
	options.cancel_argument = &counter;
	fixture_open_options(&fixture, "qnx6", 0, &options);
	bzero(&clean, sizeof(clean));
	CHECK(write_lines(&fixture, &clean) == FSIMG_OK);
	CHECK(clean.length > 6 * 65536);
	CHECK(clean.calls[SINK_RESIZE] > 6 && clean.calls[SINK_FLUSH] == 1);
	for (sink_call_t kind = 0; kind < SINK_CALL_KINDS; kind++) {
		for (unsigned at = 1; at <= clean.calls[kind]; at++) {
			bzero(&sink, sizeof(sink));
			sink.fail_kind = kind;
			sink.fail_at = at;
			CHECK(write_lines(&fixture, &sink) == FSIMG_EIO);
		}
	}

	/* Writing polls cancellation; each poll can be the one cancelled. */
	counter.cancel_at = UINT_MAX;
	bzero(&sink, sizeof(sink));
	CHECK(write_lines(&fixture, &sink) == FSIMG_OK);
	polls = counter.polls;
	CHECK(polls > clean.calls[SINK_WRITE]);
	for (unsigned at = 1; at <= polls; at++) {
		counter.polls = 0;
		counter.cancel_at = at;
		bzero(&sink, sizeof(sink));
		CHECK(write_lines(&fixture, &sink) == FSIMG_ECANCELLED);
	}
	counter.cancel_at = 0;

	for (uint64_t attempt = 1;; attempt++) {
		bzero(&sink, sizeof(sink));
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = write_lines(&fixture, &sink);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK) {
			CHECK(attempt > 3);
			break;
		}
		CHECK(status == FSIMG_ENOMEM);
	}
	fixture_close(&fixture);
}

/* Extract image into name with a manifest written to name.mtree, under
 * whatever faults the fixture's context injects, with allocation
 * fail_after of the extraction failing when it is not zero; the manifest
 * file is removed again.
 */
static fsimg_status_t
extract_faulted(fixture_t *fixture, const image_t *image, const char *name,
    uint64_t fail_after, fsimg_error_t *error)
{
	char path[64];
	fsimg_extract_options_t options;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fsimg_status_t status;
	int fd;

	snprintf(path, sizeof(path), "%s.mtree", name);
	fd = openat(fixture->parent, path,
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	memory_io(fixture, (image_t *)image, false, &input);
	fsimg_test_fail_alloc_after(fixture->ctx, fail_after);
	status = fsimg_reader_open(fixture->ctx, input, NULL, &reader, error);
	if (status == FSIMG_OK) {
		fsimg_extract_options_init(&options);
		options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
		options.manifest_fd = fd;
		status = fsimg_extract_at(reader, fixture->parent, name, &options,
		    error);
		fsimg_reader_free(reader);
	}
	fsimg_test_fail_alloc_after(fixture->ctx, 0);
	fsimg_io_free(input);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture->parent, path, 0) == 0);
	return (status);
}

/* An extraction with a manifest fails cleanly at each of its allocations,
 * the manifest writer's among them.
 */
static void
test_export_allocation_failures(void)
{
	fixture_t fixture;
	image_t image;
	char name[32];
	fsimg_status_t status;

	fixture_open(&fixture, "qnx6", 0);
	CHECK(add_manifest(&fixture, export_manifest_qnx, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	for (uint64_t attempt = 1;; attempt++) {
		snprintf(name, sizeof(name), "allocation-%llu",
		    (unsigned long long)attempt);
		status = extract_faulted(&fixture, &image, name, attempt, NULL);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(attempt < 512);
	}
	free(image.bytes);
	fixture_close(&fixture);
}

/* An extraction with a manifest stops at whichever poll is cancelled,
 * including those of the manifest's writes and its flush.
 */
static void
test_export_cancellation(void)
{
	poll_counter_t cancel;
	fsimg_options_t options;
	fixture_t fixture;
	image_t image;
	char name[32];
	fsimg_status_t status;

	bzero(&cancel, sizeof(cancel));
	fsimg_options_init(&options);
	options.cancelled = poll_cancelled;
	options.cancel_argument = &cancel;
	fixture_open_options(&fixture, "qnx6", 0, &options);
	CHECK(add_manifest(&fixture, export_manifest_qnx, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	for (unsigned at = 1;; at++) {
		snprintf(name, sizeof(name), "cancelled-%u", at);
		cancel.polls = 0;
		cancel.cancel_at = at;
		status = extract_faulted(&fixture, &image, name, 0, NULL);
		cancel.cancel_at = 0;
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ECANCELLED);
		CHECK(at < 100000);
	}
	free(image.bytes);
	fixture_close(&fixture);
}

/*
 * A host write that fails reaches the extraction's caller: under a file
 * size limit that the extracted files fit and the manifest outgrows, the
 * manifest's growth fails with EFBIG rather than the process being
 * signalled. It runs in a child, since the limit is the process's.
 */
static void
test_export_file_size_limit(void)
{
	struct rlimit limit = { 32768, 32768 };
	fsimg_error_t error;
	fixture_t fixture;
	image_t image;
	char *source;
	pid_t child;
	int status;

	source = long_source();
	fixture_open(&fixture, "qnx6", 0);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
		    setrlimit(RLIMIT_FSIZE, &limit) == -1)
			_exit(2);
		if (extract_faulted(&fixture, &image, "file-size", 0, &error) !=
		    FSIMG_EIO || error.os_error != EFBIG)
			_exit(1);
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	free(image.bytes);
	free(source);
	fixture_close(&fixture);
}

/* The exact lines for a QNX6 image: the signature and root, then each
 * entry in the reader's order with its escapes, the digest of the bytes
 * written and a skipped link and FIFO with everything but a digest. The
 * line of the /.boot directory every QNX6 image holds, timed by its
 * creation, comes between the root and the entries.
 */
static void
test_export_lines(void)
{
	static const char source[] =
	    "./d\\040\\043x type=dir mode=0700 uid=1 gid=2 time=6\n"
	    "./d\\040\\043x/f type=file mode=0644 uid=0 gid=0 time=7 "
	    "fsimg.content=zero size=3\n"
	    "./d\\040\\043x/l type=link mode=0777 uid=0 gid=0 time=8 "
	    "link=\\\\\\001\n"
	    "./d\\040\\043x/p type=fifo mode=0600 uid=0 gid=0 time=9\n";
	static const char header[] = "#mtree v2.0\n. type=dir\n./.boot type=dir ";
	static const char expected[] =
	    "./d\\040\\043x type=dir mode=0700 uid=1 gid=2 "
	    "time=6.000000000\n"
	    "./d\\040\\043x/f type=file mode=0644 uid=0 gid=0 "
	    "time=7.000000000 size=3 sha256digest="
	    "709e80c88487a2411e1ee4dfb9f22a861492d20c4765150c0c794abd70f8147c\n"
	    "./d\\040\\043x/l type=link mode=0777 uid=0 gid=0 "
	    "time=8.000000000 link=\\134\\001\n"
	    "./d\\040\\043x/p type=fifo mode=0600 uid=0 gid=0 "
	    "time=9.000000000\n";
	fixture_t fixture;
	image_t image;
	char *text;

	fixture_open(&fixture, "qnx6", 0);
	CHECK(add_manifest(&fixture, source, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	text = extract_with_manifest(&fixture, &image, "lines");
	if (strncmp(text, header, strlen(header)) != 0 ||
	    strcmp(strchr(text + strlen(header), '\n') + 1, expected) != 0)
		printf("manifest written:\n%s", text);
	CHECK(strncmp(text, header, strlen(header)) == 0);
	CHECK(strcmp(strchr(text + strlen(header), '\n') + 1, expected) == 0);
	free(text);
	free(image.bytes);
	fixture_close(&fixture);
}

/* A manifest descriptor that is not an empty regular file open for
 * writing without O_APPEND is refused before the destination exists, and
 * an extraction that fails leaves the manifest unfinished, for its
 * caller to discard.
 */
static void
test_export_refusals(void)
{
	const struct {
		const char	*name;
		int		 flags;
		const char	*contents;
	} cases[] = {
		{ "read-only", O_RDONLY, "" },
		{ "append", O_WRONLY | O_APPEND, "" },
		{ "nonempty", O_WRONLY, "x" }
	};
	fsimg_extract_options_t options;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fixture_t fixture;
	image_t image;
	struct stat st;
	int fd;

	fixture_open(&fixture, "qnx6", 0);
	CHECK(add_manifest(&fixture, export_manifest_qnx, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		put_file(fixture.parent, cases[i].name, cases[i].contents);
		fd = openat(fixture.parent, cases[i].name,
		    cases[i].flags | O_CLOEXEC);
		CHECK(fd != -1);
		memory_io(&fixture, &image, false, &input);
		CHECK(fsimg_reader_open(fixture.ctx, input, NULL, &reader,
		    NULL) == FSIMG_OK);
		fsimg_extract_options_init(&options);
		options.manifest_fd = fd;
		CHECK(fsimg_extract_at(reader, fixture.parent, "refused", &options,
		    NULL) == FSIMG_EINVAL);
		CHECK(fstatat(fixture.parent, "refused", &st,
		    AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
		fsimg_reader_free(reader);
		fsimg_io_free(input);
		CHECK(close(fd) == 0);
	}

	/* Without SKIP_UNSUPPORTED the link fails the extraction. */
	fd = openat(fixture.parent, "failed.mtree",
	    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	memory_io(&fixture, &image, false, &input);
	CHECK(fsimg_reader_open(fixture.ctx, input, NULL, &reader,
	    NULL) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.manifest_fd = fd;
	CHECK(fsimg_extract_at(reader, fixture.parent, "failed", &options,
	    NULL) == FSIMG_EUNSUPPORTED);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
	CHECK(close(fd) == 0);
	free(image.bytes);
	fixture_close(&fixture);
}

/* bsdtar reads the manifest x --mtree writes, decoding every escaped name
 * to the bytes the image holds; it is skipped with a note when it is not
 * installed.
 */
static void
test_export_read_by_bsdtar(void)
{
	static const char expected[] =
	    ".\n"
	    "./a\n"
	    "./a/b\n"
	    "./a/b/c\n"
	    "./caf\303\251\n"
	    "./empty\n"
	    "./sp ace\n"
	    "./sp ace/hash#1\n";
	char manifest_path[128];
	const char *const argv[] = {
		"bsdtar", "-tf", manifest_path, NULL
	};
	fixture_t fixture;
	image_t image;
	struct stat st;
	char listing[512];
	char *text;
	ssize_t length;
	int fd, result;

	fixture_open(&fixture, "fat16", 0);
	CHECK(add_manifest(&fixture, export_manifest_fat, -1, NULL) == FSIMG_OK);
	CHECK(emit_image(&fixture, &image, NULL) == FSIMG_OK);
	text = extract_with_manifest(&fixture, &image, "listed");
	snprintf(manifest_path, sizeof(manifest_path), "%s/listed.mtree",
	    fixture.directory);
	fd = openat(fixture.parent, "listing",
	    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	result = run_tool(argv, fd);
	if (result == -1) {
		printf("skip: bsdtar is not installed\n");
	} else {
		CHECK(result == 0);
		CHECK(fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(listing));
		length = pread(fd, listing, sizeof(listing) - 1, 0);
		CHECK(length == st.st_size);
		listing[length] = '\0';
		if (strcmp(listing, expected) != 0)
			printf("bsdtar listed:\n%s", listing);
		CHECK(strcmp(listing, expected) == 0);
	}
	CHECK(close(fd) == 0);
	free(text);
	free(image.bytes);
	fixture_close(&fixture);
}

int
main(void)
{
	test_generated_files();
	test_generated_digest_refused();
	test_generated_digest_after_planning();
	test_generated_ranges();
	test_host_contents();
	test_host_refusals();
	test_host_digest_at_emission();
	test_host_digest_needs_whole_file();
	test_merge_with_tree();
	test_duplicates();
	test_line_order();
	test_unused_content_directory();
	test_adopted_descriptors();
	test_qnx6_metadata();
	test_external_writers();
	test_export_round_trip("fat16", export_manifest_fat);
	test_export_round_trip("exfat", export_manifest_fat);
	test_export_round_trip("qnx6", export_manifest_qnx);
	test_export_round_trip("qnx4", export_manifest_qnx);
	test_export_changes_detected();
	test_export_exfat_zero_file();
	test_export_long_manifest();
	test_export_writer_faults();
	test_export_allocation_failures();
	test_export_cancellation();
	test_export_file_size_limit();
	test_export_lines();
	test_export_refusals();
	test_export_read_by_bsdtar();
	return (0);
}
