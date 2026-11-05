/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "internal.h"
#include "meta.h"
#include "hfsplus-gen.h"
#include "test.h"

#define FILES 8u
#define BLOCK_BYTES 512u
#define ATTEMPT_LIMIT 10000u
#define DECODED_BYTES 65537u

typedef enum stage {
	STAGE_OPEN, STAGE_WALK, STAGE_CONTENT, STAGE_VERIFY, STAGE_COUNT
} stage_t;

typedef enum fault {
	FAULT_ALLOC, FAULT_SCRATCH_OPEN, FAULT_SCRATCH_RESIZE,
	FAULT_SCRATCH_READ, FAULT_SCRATCH_WRITE, FAULT_SHORT, FAULT_EOF,
	FAULT_READ, FAULT_CANCEL, FAULT_COUNT
} fault_t;

typedef struct result {
	uint64_t entries;
	uint64_t bytes;
	uint64_t digest;
	uint64_t extra;
	bool valid;
} result_t;

typedef struct fixture {
	gen_image_t *image;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_stats_t baseline;
	uint64_t reads;
	uint64_t polls;
	uint64_t point;
	fault_t fault;
	bool armed;
	bool fired;
	int directory;
} fixture_t;

static const char *stages[] = { "open", "walk", "content", "verify" };
static const char *faults[] = { "allocation", "scratch-open", "scratch-resize",
    "scratch-read", "scratch-write", "short", "EOF", "read-error",
    "cancellation" };
static const fsimg_meta_fault_t scratch_faults[] = { FSIMG_META_OPEN,
    FSIMG_META_RESIZE, FSIMG_META_READ, FSIMG_META_WRITE };
static const uint32_t cache_pages[STAGE_COUNT] = { 2, 2, 2, 8 };
static unsigned failures;

static double
seconds(void)
{
	struct timespec stamp;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &stamp) == 0);
	return ((double)stamp.tv_sec + (double)stamp.tv_nsec / 1000000000);
}

static void
store_le(unsigned char *bytes, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++)
		bytes[i] = (unsigned char)(value >> (8 * i));
}

/* Two zlib frames straddle a decode-window boundary in the resource fork.
 * Its 256-byte header precedes a size word, count and two offset/size pairs;
 * offsets in that table are relative to the count at byte 260.
 */
static unsigned char *
compressed_resource(const unsigned char *plain, size_t *length)
{
	size_t cursor = 256 + 8 + 2 * 8;
	size_t capacity = cursor + 2 * compressBound(65536) + 50;
	unsigned char *resource = calloc(1, capacity);

	CHECK(resource != NULL);
	store_le(resource + 260, 2, 4);
	for (size_t i = 0; i < 2; i++) {
		uLongf stored = compressBound(65536);
		size_t size = i == 0 ? 65536 : 1;

		CHECK(compress2(resource + cursor, &stored, plain + i * 65536,
		    size, Z_BEST_COMPRESSION) == Z_OK);
		store_le(resource + 264 + 8 * i, cursor - 260, 4);
		store_le(resource + 268 + 8 * i, stored, 4);
		cursor += stored;
	}
	gen_store(resource, 256, 4);
	gen_store(resource + 4, cursor, 4);
	gen_store(resource + 8, cursor - 256, 4);
	gen_store(resource + 12, 50, 4);
	gen_store(resource + 256, cursor - 260, 4);
	*length = cursor + 50;
	return (resource);
}

static void
build_image(gen_image_t *image)
{
	static const uint16_t compression[] = { 'c', 'o', 'm', '.', 'a', 'p',
	    'p', 'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };
	static const uint16_t attribute[] = { 'u', 's', 'e', 'r' };
	gen_plan_t plan;
	gen_object_t objects[FILES];
	uint16_t names[FILES][3];
	unsigned char plain[DECODED_BYTES], data[9 * BLOCK_BYTES];
	unsigned char resource_header[16] = { 'f', 'p', 'm', 'c' };
	unsigned char inline_header[128] = { 'f', 'p', 'm', 'c' };
	unsigned char *resource;
	size_t resource_size;
	uLongf inline_size = sizeof(inline_header) - 16;
	gen_attr_value_t values[] = {
		{ .id = 32, .type = 0x10, .name = compression,
		    .units = sizeof(compression) / sizeof(compression[0]),
		    .value = resource_header, .size = sizeof(resource_header) },
		{ .id = 33, .type = 0x10, .name = compression,
		    .units = sizeof(compression) / sizeof(compression[0]),
		    .value = inline_header },
		{ .id = 34, .type = 0x10, .name = attribute, .units = 4,
		    .value = (const unsigned char *)"value", .size = 5 }
	};

	for (size_t i = 0; i < sizeof(plain); i++)
		plain[i] = (unsigned char)(i * 17 + 3);
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 7 + 1);
	resource = compressed_resource(plain, &resource_size);
	store_le(resource_header + 4, 4, 4);
	store_le(resource_header + 8, sizeof(plain), 8);
	store_le(inline_header + 4, 3, 4);
	store_le(inline_header + 8, 3, 8);
	CHECK(compress2(inline_header + 16, &inline_size,
	    (const unsigned char *)"abc", 3, Z_BEST_COMPRESSION) == Z_OK);
	values[1].size = 16 + inline_size;
	for (size_t i = 0; i < FILES; i++) {
		names[i][0] = 'f';
		names[i][1] = '0' + i / 10;
		names[i][2] = '0' + i % 10;
		objects[i] = (gen_object_t){ .name = names[i], .units = 3,
		    .id = 32 + i, .type = 2,
		    .data = (const unsigned char *)"abc", .data_size = 3 };
	}
	objects[0].data_size = 0;
	objects[0].resource = resource;
	objects[0].resource_size = resource_size;
	objects[1].data_size = 0;
	for (size_t i = 0; i < 2; i++) {
		objects[i].flags = 4;
		objects[i].owner_flags = 0x20;
	}
	objects[2].flags = 4;
	/* Nine discontiguous blocks force the ninth run into the overflow tree. */
	objects[2].data = data;
	objects[2].data_size = sizeof(data);
	objects[2].fragmented = true;
	gen_plan_init(&plan);
	plan.block_size = BLOCK_BYTES;
	plan.total_blocks = 1024;
	plan.verification = true;
	plan.journal = true;
	plan.journal_info_file = true;
	plan.attributes = true;
	/* Catalog nodes are 4096 bytes. With 512-byte allocation blocks the
	 * indexed catalog exceeds its eight inline runs, making open consult
	 * the overflow tree and reread evicted extent-index pages. */
	plan.catalog_overflow = true;
	/* Each file contributes a primary and a thread. Ten records per leaf
	 * put the eight files, root and journal files beneath an index node. */
	plan.records_per_node = 10;
	plan.objects = objects;
	plan.object_count = FILES;
	plan.values = values;
	plan.value_count = sizeof(values) / sizeof(values[0]);
	gen_build(&plan, image);
	CHECK(image->trees[GEN_CATALOG].depth > 1);
	CHECK(image->trees[GEN_EXTENTS].record_count > 0);
	CHECK(image->journal_header != 0);
	free(resource);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((fixture_t *)argument)->image->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	fixture_t *f = argument;
	bool inject;

	f->reads++;
	*count = 0;
	inject = f->armed && f->reads == f->point;
	if (inject && (f->fault == FAULT_EOF || f->fault == FAULT_READ)) {
		f->fired = true;
		if (f->fault == FAULT_EOF)
			return (FSIMG_OK);
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected source read failure"));
	}
	CHECK(offset <= f->image->length);
	if (length > f->image->length - offset)
		length = f->image->length - offset;
	if (inject && f->fault == FAULT_SHORT && length > 1) {
		length /= 2;
		f->fired = true;
	}
	bcopy(f->image->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static bool
cancelled(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	if (f->armed && f->fault == FAULT_CANCEL && f->polls >= f->point) {
		f->fired = true;
		return (true);
	}
	return (false);
}

static fsimg_status_t
open_image(fixture_t *f, fsimg_error_t *error)
{
	return (fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, error));
}

static void
make_io(fixture_t *f)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

/* The digest includes names and every decoded content byte. The clean run
 * also checks payloads against their generator formulas, so the comparison
 * cannot bless a consistently truncated short-read result.
 */
static void
digest_bytes(result_t *result, const unsigned char *bytes, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		result->digest ^= bytes[i];
		result->digest *= UINT64_C(1099511628211);
	}
}

static unsigned char
expected_byte(unsigned file, uint64_t offset)
{
	switch (file) {
	case 0:
		return ((unsigned char)(offset * 17 + 3));
	case 2:
		return ((unsigned char)(offset * 7 + 1));
	default:
		return ((unsigned char)"abc"[offset]);
	}
}

static uint64_t
expected_size(unsigned file)
{
	switch (file) {
	case 0:
		return (DECODED_BYTES);
	case 2:
		return (9 * BLOCK_BYTES);
	default:
		return (3);
	}
}

static fsimg_status_t
walk(fixture_t *f, bool content, result_t *result, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	unsigned char bytes[8192];
	fsimg_status_t status;
	size_t count, length;
	uint64_t offset;
	unsigned file;

	while ((status = fsimg_reader_next(f->reader, &entry, error)) == FSIMG_OK) {
		result->entries++;
		digest_bytes(result, (const unsigned char *)entry->path,
		    strlen(entry->path));
		if (entry->kind != FSIMG_ENTRY_FILE ||
		    sscanf(entry->path, "/f%u", &file) != 1 || file >= FILES) {
			result->valid = false;
			return (FSIMG_OK);
		}
		if (entry->size != expected_size(file)) {
			result->valid = false;
			return (FSIMG_OK);
		}
		if ((entry->metadata.valid & FSIMG_META_EXTRA_DATA) != 0)
			result->extra++;
		if (!content)
			continue;
		for (offset = 0; offset < entry->size; offset += count) {
			length = entry->size - offset < sizeof(bytes) ?
			    (size_t)(entry->size - offset) : sizeof(bytes);
			status = fsimg_reader_read_at(f->reader, offset, bytes,
			    length, &count, error);
			if (status != FSIMG_OK)
				return (status);
			if (count != length) {
				result->valid = false;
				return (FSIMG_OK);
			}
			for (size_t i = 0; i < count; i++) {
				if (bytes[i] != expected_byte(file, offset + i))
					result->valid = false;
			}
			digest_bytes(result, bytes, count);
			result->bytes += count;
		}
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/* Diagnostic records are optional, and failures must report the same status
 * with either form.
 */
static fsimg_status_t
perform(fixture_t *f, stage_t stage, result_t *result, fsimg_error_t *error)
{
	fsimg_verify_report_t report = {0};
	fsimg_status_t status;

	*result = (result_t){ .digest = UINT64_C(14695981039346656037),
	    .valid = true };
	switch (stage) {
	case STAGE_OPEN:
		return (open_image(f, error));
	case STAGE_WALK:
		return (walk(f, false, result, error));
	case STAGE_CONTENT:
		return (walk(f, true, result, error));
	case STAGE_VERIFY:
		status = fsimg_verify(f->reader, FSIMG_VERIFY_PAYLOAD, &report, error);
		result->valid = report.complete == (status == FSIMG_OK);
		if (status == FSIMG_OK) {
			result->valid = result->valid && report.files == FILES &&
			    report.directories == 1;
			result->entries = report.files;
			result->bytes = report.logical_bytes;
		}
		return (status);
	case STAGE_COUNT:
		break;
	}
	CHECK(false);
	return (FSIMG_EINVAL);
}

static void
prepare(fixture_t *f, gen_image_t *image, int directory, stage_t stage)
{
	fsimg_options_t options;

	*f = (fixture_t){ .image = image, .directory = directory };
	fsimg_options_init(&options);
	/* The context requires at least 16 MiB. The cache bounds keep open and
	 * directory preparation spilling with this small fixture. Verification
	 * has more concurrent streams and needs less aggressive eviction to
	 * reach scratch without dominating the exhaustive sweep's runtime. */
	options.memory_bytes = UINT64_C(16) << 20;
	options.scratch_directory_fd = directory;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(f->ctx, cache_pages[stage], NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &f->baseline, NULL) == FSIMG_OK);
	make_io(f);
	if (stage != STAGE_OPEN)
		CHECK(open_image(f, NULL) == FSIMG_OK);
	f->reads = f->polls = 0;
}

static void
directory_empty(int directory)
{
	struct dirent *entry;
	struct stat st;
	DIR *stream;
	int fd;

	/* Open a separate directory description so repeated scans start at zero. */
	fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	stream = fdopendir(fd);
	CHECK(stream != NULL);
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		CHECK(fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0);
		errno = 0;
	}
	CHECK(errno == 0);
	CHECK(closedir(stream) == 0);
}

static void
release_handles(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	f->reader = NULL;
	fsimg_io_free(f->io);
	f->io = NULL;
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == f->baseline.descriptors);
	directory_empty(f->directory);
}

/* Every attempt has this single exit: turn off injection, release all handles,
 * check accounting and names, then prove the same context remains reusable.
 * The recovery walk goes through the same teardown checks as the attempt.
 */
static void
cleanup(fixture_t *f)
{
	result_t result = { .valid = true };
	fsimg_status_t status;

	f->armed = false;
	fsimg_test_fail_alloc_after(f->ctx, 0);
	fsimg_test_meta_fail_after(f->ctx, FSIMG_META_OPEN, 0, 0);
	release_handles(f);
	make_io(f);
	status = open_image(f, NULL);
	if (status == FSIMG_OK)
		status = walk(f, false, &result, NULL);
	release_handles(f);
	CHECK(status == FSIMG_OK);
	CHECK(result.valid && result.entries == FILES);
	fsimg_ctx_free(f->ctx);
}

static bool
same_result(const result_t *a, const result_t *b)
{
	return (a->entries == b->entries && a->bytes == b->bytes &&
	    a->digest == b->digest && a->extra == b->extra && a->valid);
}

static bool
fault_reached(const fixture_t *f, fault_t fault, unsigned point)
{
	switch (fault) {
	case FAULT_ALLOC:
		return (f->ctx->allocation_attempts >= point);
	case FAULT_SCRATCH_OPEN:
	case FAULT_SCRATCH_RESIZE:
	case FAULT_SCRATCH_READ:
	case FAULT_SCRATCH_WRITE:
		return (f->ctx->meta_fault_after == 0);
	case FAULT_SHORT:
		return (f->reads >= point);
	default:
		return (f->fired);
	}
}

static fsimg_status_t
expected_status(fault_t fault, bool hit)
{
	if (!hit || fault == FAULT_SHORT)
		return (FSIMG_OK);
	switch (fault) {
	case FAULT_ALLOC:
		return (FSIMG_ENOMEM);
	case FAULT_CANCEL:
		return (FSIMG_ECANCELLED);
	default:
		return (FSIMG_EIO);
	}
}

static fsimg_status_t
attempt(fixture_t *f, stage_t stage, fault_t fault, unsigned point,
    const result_t *expected, fsimg_error_t *error, bool *hit)
{
	result_t result;
	fsimg_status_t status, wanted;

	f->armed = true;
	f->fault = fault;
	f->point = point;
	if (fault == FAULT_ALLOC)
		fsimg_test_fail_alloc_after(f->ctx, point);
	else if (fault >= FAULT_SCRATCH_OPEN && fault <= FAULT_SCRATCH_WRITE)
		fsimg_test_meta_fail_after(f->ctx,
		    scratch_faults[fault - FAULT_SCRATCH_OPEN], point, EIO);
	if (error != NULL)
		fsimg_error_clear(error);
	status = perform(f, stage, &result, error);
	*hit = fault_reached(f, fault, point);
	wanted = expected_status(fault, *hit);
	if (error != NULL && status != FSIMG_OK && error->status != status) {
		fprintf(stderr, "stage=%s fault=%s attempt=%u status=%d error.status=%d\n",
		    stages[stage], faults[fault], point, status, error->status);
		failures++;
	}
	if (status != wanted || !result.valid || (status == FSIMG_OK &&
	    !same_result(&result, expected))) {
		fprintf(stderr, "stage=%s fault=%s attempt=%u record=%u status=%d expected=%d result_matches=%u\n",
		    stages[stage], faults[fault], point, (unsigned)(error != NULL),
		    status, wanted,
		    (unsigned)same_result(&result, expected));
		failures++;
	}
	return (status);
}

static void
sweep(gen_image_t *image, int directory, stage_t stage, fault_t fault,
    const result_t *expected)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t hits = 0;
	bool hit, any_hit, complete = false;
	double start = seconds();
	unsigned point, forms = fault == FAULT_ALLOC ? 2 : 1;

	/* Flushed so that a crash or hang names the sweep it happened in. */
	printf("HFS faults stage=%s fault=%s\n", stages[stage], faults[fault]);
	CHECK(fflush(stdout) == 0);
	for (point = 1; point < ATTEMPT_LIMIT; point++) {
		any_hit = false;
		complete = true;
		for (unsigned i = 0; i < forms; i++) {
			fsimg_error_t *detail;

			/* Allocation faults use both record forms at every point;
			 * other faults alternate NULL on odd points and a cleared
			 * record on even points. */
			detail = (point + i) % 2 == 0 ? &error : NULL;
			prepare(&f, image, directory, stage);
			status = attempt(&f, stage, fault, point, expected,
			    detail, &hit);
			any_hit |= hit;
			complete = complete && status == FSIMG_OK;
			if (hit && (fault != FAULT_SHORT || f.fired))
				hits++;
			/* Cleanup also runs for an unexpected status or result. */
			cleanup(&f);
		}
		if (!any_hit)
			break;
		complete = false;
	}
	printf("HFS faults stage=%s fault=%s attempts=%u hits=%" PRIu64
	    " seconds=%.3f complete=%u\n", stages[stage], faults[fault], point,
	    hits, seconds() - start, (unsigned)complete);
	CHECK(fflush(stdout) == 0);
	CHECK(complete);
	CHECK(hits != 0);
}

int
main(int argc, char **argv)
{
	gen_image_t image;
	fixture_t f;
	result_t expected[STAGE_COUNT];
	fsimg_stats_t stats, before;
	char path[] = "hfsplus-faults-XXXXXX";
	int directory, open_descriptors;
	double start = seconds();
	bool selected = false;

	CHECK(argc >= 1 && argc <= 3);
	CHECK(mkdtemp(path) != NULL);
	directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(directory != -1);
	open_descriptors = fsimg_test_open_descriptors();
	build_image(&image);
	for (stage_t stage = STAGE_OPEN; stage < STAGE_COUNT; stage++) {
		prepare(&f, &image, directory, stage);
		CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
		CHECK(perform(&f, stage, &expected[stage], NULL) == FSIMG_OK);
		CHECK(expected[stage].valid);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		printf("baseline stage=%s reads=%" PRIu64 " polls=%" PRIu64
		    " scratch_peak=%" PRIu64 "\n", stages[stage], f.reads,
		    f.polls, stats.scratch_peak);
		/* Directory preparation and verification must exercise scratch,
		 * rather than silently reducing these sweeps to in-memory tests. */
		CHECK(stats.scratch_peak > 0);
		CHECK(stats.io_calls - before.io_calls > f.reads);
		if (stage != STAGE_OPEN)
			CHECK(stats.scratch_peak > before.scratch_peak);
		if (stage == STAGE_WALK || stage == STAGE_CONTENT) {
			CHECK(expected[stage].entries == FILES);
			CHECK(expected[stage].extra == 1);
		}
		cleanup(&f);
	}
	for (stage_t stage = STAGE_OPEN; stage < STAGE_COUNT; stage++) {
		if (argc > 1 && strcmp(argv[1], stages[stage]) != 0)
			continue;
		for (fault_t fault = FAULT_ALLOC; fault < FAULT_COUNT; fault++) {
			if (argc > 2 && strcmp(argv[2], faults[fault]) != 0)
				continue;
			selected = true;
			sweep(&image, directory, stage, fault, &expected[stage]);
		}
	}
	CHECK(selected);
	gen_free(&image);
	directory_empty(directory);
	CHECK(fsimg_test_open_descriptors() == open_descriptors);
	CHECK(close(directory) == 0);
	CHECK(rmdir(path) == 0);
	printf("HFS faults seconds=%.3f failures=%u\n", seconds() - start, failures);
	return (failures == 0 ? 0 : 1);
}
