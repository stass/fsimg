/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/resource.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "test.h"

static uint32_t get32(const unsigned char *);
static uint64_t get64(const unsigned char *);
static void put32(unsigned char *, uint32_t);
static void put64(unsigned char *, uint64_t);
static void exact_io(int, uint64_t, void *, size_t, bool);
static uint32_t physical_cluster(uint32_t, uint32_t, uint32_t);
static uint32_t logical_ordinal(uint32_t, uint32_t);
static void fragment_image(int, uint32_t);
static void check_bytes(const unsigned char *, size_t, uint64_t);
static void exercise(uint32_t);
static void require_ok(fsimg_status_t, const fsimg_error_t *);

static uint32_t
get32(const unsigned char *bytes)
{
	return ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	    (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24);
}

static uint64_t
get64(const unsigned char *bytes)
{
	return ((uint64_t)get32(bytes) | (uint64_t)get32(bytes + 4) << 32);
}

static void
put32(unsigned char *bytes, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		bytes[i] = (unsigned char)(value >> (i * 8));
}

static void
put64(unsigned char *bytes, uint64_t value)
{
	put32(bytes, (uint32_t)value);
	put32(bytes + 4, (uint32_t)(value >> 32));
}

static void
require_ok(fsimg_status_t status, const fsimg_error_t *error)
{
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: %s (resource=%u limit=%" PRIu64
		    " observed=%" PRIu64 ")\n", fsimg_status_string(status), error->message,
		    (unsigned)error->resource, error->limit, error->observed);
	CHECK(status == FSIMG_OK);
}

static void
exact_io(int fd, uint64_t offset, void *buffer, size_t length, bool writing)
{
	unsigned char *bytes = buffer;
	ssize_t count;
	size_t done = 0;

	CHECK(offset <= INT64_MAX && length <= INT64_MAX - offset);
	while (done < length) {
		count = writing ? pwrite(fd, bytes + done, length - done, (off_t)(offset + done)) :
		    pread(fd, bytes + done, length - done, (off_t)(offset + done));
		if (count == -1 && errno == EINTR)
			continue;
		CHECK(count > 0 && (size_t)count <= length - done);
		done += (size_t)count;
	}
}

static uint32_t
physical_cluster(uint32_t first, uint32_t ordinal, uint32_t fragments)
{
	return (first + (ordinal < fragments / 2 ? ordinal * 2 :
	    (ordinal - fragments / 2) * 2 + 1));
}

/* Invert physical_cluster for a zero-based physical offset. */
static uint32_t
logical_ordinal(uint32_t physical, uint32_t fragments)
{
	return (physical / 2 + (physical % 2 != 0 ? fragments / 2 : 0));
}

/* Preserve the writer's allocation bitmap while replacing one contiguous
 * file with the permutation [even clusters, odd clusters]. Every logical
 * cluster becomes a separate extent; its first eight bytes identify position.
 * The rest is zero. Construction uses bounded buffers and a disk-backed image.
 */
static void
fragment_image(int fd, uint32_t fragments)
{
	unsigned char boot[512], root[512], buffer[65536];
	unsigned char *primary, *stream;
	uint64_t fat, heap, root_offset, payload;
	uint32_t first, physical, ordinal, next, batch;
	size_t position, set_length;
	uint16_t checksum;

	exact_io(fd, 0, boot, sizeof(boot), false);
	CHECK(boot[108] == 9 && boot[109] == 0 && boot[110] == 1);
	fat = (uint64_t)get32(boot + 80) * 512;
	heap = (uint64_t)get32(boot + 88) * 512;
	CHECK(get32(boot + 96) >= 2);
	root_offset = heap + (uint64_t)(get32(boot + 96) - 2) * 512;
	exact_io(fd, root_offset, root, sizeof(root), false);
	for (position = 0; position < sizeof(root) && root[position] != 0x85; position += 32)
		continue;
	CHECK(position < sizeof(root));
	primary = root + position;
	set_length = ((size_t)primary[1] + 1) * 32;
	CHECK(set_length >= 96 && set_length <= sizeof(root) - position);
	stream = primary + 32;
	CHECK(stream[0] == 0xc0 && (stream[1] & 3) == 3);
	first = get32(stream + 20);
	payload = (uint64_t)fragments * 512;
	CHECK(get64(stream + 24) == payload && first >= 2);
	CHECK((uint64_t)first + fragments <= (uint64_t)get32(boot + 92) + 2);
	stream[1] &= (unsigned char)~2u;
	put64(stream + 8, payload);
	checksum = 0;
	for (size_t i = 0; i < set_length; i++) {
		if (i != 2 && i != 3)
			checksum = (uint16_t)((checksum >> 1 | (uint32_t)checksum << 15) + primary[i]);
	}
	primary[2] = (unsigned char)checksum;
	primary[3] = (unsigned char)(checksum >> 8);
	exact_io(fd, root_offset, root, sizeof(root), true);

	/* FAT entries are written in physical order, independently of the logical
	 * chain order. No resident allocation table or fragment list is retained.
	 */
	for (physical = 0; physical < fragments; physical += batch) {
		batch = fragments - physical;
		if (batch > sizeof(buffer) / 4)
			batch = sizeof(buffer) / 4;
		for (uint32_t i = 0; i < batch; i++) {
			ordinal = logical_ordinal(physical + i, fragments);
			next = ordinal + 1 == fragments ? UINT32_MAX :
			    physical_cluster(first, ordinal + 1, fragments);
			put32(buffer + i * 4, next);
		}
		exact_io(fd, fat + ((uint64_t)first + physical) * 4, buffer, (size_t)batch * 4, true);
	}
	for (physical = 0; physical < fragments; physical += batch) {
		batch = fragments - physical;
		if (batch > sizeof(buffer) / 512)
			batch = sizeof(buffer) / 512;
		bzero(buffer, (size_t)batch * 512);
		for (uint32_t i = 0; i < batch; i++) {
			ordinal = logical_ordinal(physical + i, fragments);
			put64(buffer + i * 512, ordinal);
		}
		exact_io(fd, heap + ((uint64_t)first - 2 + physical) * 512,
		    buffer, (size_t)batch * 512, true);
	}
	CHECK(fsync(fd) == 0);
}

static void
check_bytes(const unsigned char *bytes, size_t length, uint64_t offset)
{
	static const unsigned char zero[512];

	CHECK(offset % 512 == 0 && length % 512 == 0);
	for (size_t i = 0; i < length; i += 512) {
		CHECK(get64(bytes + i) == (offset + i) / 512);
		CHECK(bcmp(bytes + i + 8, zero, 504) == 0);
	}
}

static void
exercise(uint32_t fragments)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	fsimg_options_t limits;
	fsimg_write_options_t options;
	fsimg_extract_options_t extraction;
	fsimg_verify_report_t verified;
	fsimg_entry_t file = { .path = "/FRAG.BIN", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_entry_t *entry;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	struct timespec start, end;
	struct rusage usage;
	struct stat storage;
	unsigned char buffer[65536];
	char directory[] = "fragment-fixture-XXXXXX";
	uint64_t offset, length, calls, rss;
	uint32_t samples[] = { fragments - 1, 0, fragments / 2, 7, fragments / 3 };
	size_t count;
	int parent, scratch, fd, output, content;

	CHECK(mkdtemp(directory) != NULL);
	fprintf(stderr, "fragment qualification fixture: %s\n", directory);
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(parent != -1 && mkdirat(parent, "scratch", 0700) == 0);
	scratch = openat(parent, "scratch", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = UINT64_C(16) << 20;
	limits.max_work = (uint64_t)fragments * 128 + 1048576;
	limits.scratch_directory_fd = scratch;
	require_ok(fsimg_ctx_new(&limits, &ctx, &error), &error);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.allocation_unit_bytes = 512;
	require_ok(fsimg_writer_new(ctx, "exfat", &options, &writer, &error), &error);
	file.size = length = (uint64_t)fragments * 512;
	require_ok(fsimg_writer_add(writer, &file, &zero, &error), &error);
	require_ok(fsimg_writer_write_at(writer, parent, "image", &error), &error);
	fsimg_writer_free(writer);
	fd = openat(parent, "image", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	fragment_image(fd, fragments);
	CHECK(close(fd) == 0);
	fd = openat(parent, "image", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	require_ok(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, &error), &error);
	require_ok(fsimg_ctx_get_stats(ctx, &before, &error), &error);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	require_ok(fsimg_reader_open(ctx, io, NULL, &reader, &error), &error);
	require_ok(fsimg_reader_next(reader, &entry, &error), &error);
	CHECK(strcmp(entry->path, file.path) == 0 && entry->size == length);
	for (offset = 0; offset < length; offset += count) {
		require_ok(fsimg_reader_read_at(reader, offset, buffer, sizeof(buffer), &count, &error), &error);
		CHECK(count != 0 && count <= length - offset);
		check_bytes(buffer, count, offset);
	}
	require_ok(fsimg_ctx_get_stats(ctx, &after, &error), &error);
	calls = after.io_calls - before.io_calls;
	CHECK(calls <= (uint64_t)fragments * 16 + 10000);
	for (unsigned sample = 0; sample < 5; sample++) {
		offset = (uint64_t)samples[sample] * 512;
		require_ok(fsimg_reader_read_at(reader, offset, buffer, 512, &count, &error), &error);
		CHECK(count == 512);
		check_bytes(buffer, count, offset);
	}
	fsimg_reader_free(reader);
	require_ok(fsimg_reader_open(ctx, io, NULL, &reader, &error), &error);
	require_ok(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &verified, &error), &error);
	CHECK(verified.complete && verified.files == 1 && verified.payload_bytes_read == length);
	fsimg_reader_free(reader);
	require_ok(fsimg_reader_open(ctx, io, NULL, &reader, &error), &error);
	fsimg_extract_options_init(&extraction);
	require_ok(fsimg_extract_at(reader, parent, "extracted", &extraction, &error), &error);
	fsimg_reader_free(reader);
	output = openat(parent, "extracted", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(output != -1);
	content = openat(output, "FRAG.BIN", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(content != -1 && fstat(content, &storage) == 0 && (uint64_t)storage.st_size == length);
	for (offset = 0; offset < length; offset += count) {
		count = length - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(length - offset);
		exact_io(content, offset, buffer, count, false);
		check_bytes(buffer, count, offset);
	}
	CHECK(close(content) == 0 && unlinkat(output, "FRAG.BIN", 0) == 0);
	CHECK(close(output) == 0 && unlinkat(parent, "extracted", AT_REMOVEDIR) == 0);
	require_ok(fsimg_ctx_get_stats(ctx, &after, &error), &error);
	CHECK(after.memory_peak <= limits.memory_bytes && after.descriptors_peak <= limits.max_open_files);
	if (fragments >= 1048576)
		CHECK(after.scratch_peak > limits.memory_bytes);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	/* A statistics-clock sample may be absent for a short process. Zero is
	 * unavailable telemetry, not evidence of zero resident memory.
	 */
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0);
#if defined(__APPLE__)
	rss = (uint64_t)usage.ru_maxrss;
#else
	CHECK((uint64_t)usage.ru_maxrss <= UINT64_MAX / 1024);
	rss = (uint64_t)usage.ru_maxrss * 1024;
#endif
	CHECK(printf("exfat fragments=%u payload=%" PRIu64 " sequential_io_calls=%" PRIu64
	    " total_io_calls=%" PRIu64 " memory_peak=%" PRIu64 " scratch_peak=%" PRIu64
	    " adapter_read=%" PRIu64 " adapter_written=%" PRIu64 " rss=%" PRIu64
	    " rss_available=%s seconds=%.3f\n", fragments, length,
	    calls, after.io_calls - before.io_calls, after.memory_peak, after.scratch_peak,
	    after.read_bytes - before.read_bytes, after.written_bytes - before.written_bytes, rss,
	    rss != 0 ? "true" : "false", (double)(end.tv_sec - start.tv_sec) +
	    (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0) >= 0);
	CHECK(fflush(stdout) == 0);
	fsimg_io_free(io);
	CHECK(close(fd) == 0 && unlinkat(parent, "image", 0) == 0);
	fsimg_ctx_free(ctx);
	CHECK(close(scratch) == 0 && unlinkat(parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0 && rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
	uint64_t fragments = 512;
	char *end;

	if (argc != 1) {
		CHECK(argc == 3 && strcmp(argv[1], "--fragments") == 0);
		errno = 0;
		fragments = strtoull(argv[2], &end, 10);
		CHECK(errno == 0 && *end == '\0' && fragments >= 512 && fragments <= 1048576);
		CHECK((fragments & 1) == 0);
	}
	exercise((uint32_t)fragments);
	return (0);
}
