/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
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

static fsimg_status_t source_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t source_read(void *, uint64_t, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static unsigned char marker_byte(uint64_t, uint64_t);
static void check_content(fsimg_reader_t *, uint64_t);

static unsigned char
marker_byte(uint64_t id, uint64_t offset)
{
	return ((unsigned char)(id * 67 + offset * 37));
}

static fsimg_status_t
source_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	uint64_t *validations = argument;

	CHECK(id == 1 || id == 2);
	CHECK(error != NULL);
	(*validations)++;
	return (FSIMG_OK);
}

static fsimg_status_t
source_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	unsigned char *bytes = buffer;

	CHECK(argument != NULL && error != NULL);
	for (size_t i = 0; i < length; i++)
		bytes[i] = marker_byte(id, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

static void
check_content(fsimg_reader_t *reader, uint64_t zero_length)
{
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char bytes[65];
	uint64_t id, offset;
	size_t count;
	unsigned files = 0, seen = 0;
	fsimg_status_t status;

	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		CHECK(entry->kind == FSIMG_ENTRY_FILE);
		files++;
		if (strcmp(entry->path, "/B.zero") == 0) {
			CHECK((seen & 2) == 0);
			seen |= 2;
			CHECK(entry->size == zero_length);
			CHECK(fsimg_reader_next_range(reader, 0, &range, NULL) == FSIMG_OK);
			CHECK(range.kind == FSIMG_RANGE_ZERO && range.length == zero_length);
			CHECK(fsimg_reader_read_at(reader, zero_length - 1,
			    bytes, 1, &count, NULL) == FSIMG_OK);
			CHECK(count == 1 && bytes[0] == 0);
			continue;
		}
		if (strcmp(entry->path, "/A.begin") == 0) {
			CHECK((seen & 1) == 0);
			seen |= 1;
			id = 1;
		} else {
			CHECK(strcmp(entry->path, "/Z.end") == 0 && (seen & 4) == 0);
			seen |= 4;
			id = 2;
		}
		CHECK(entry->size == 65537);
		for (unsigned end = 0; end < 2; end++) {
			offset = end == 0 ? 0 : entry->size - sizeof(bytes);
			CHECK(fsimg_reader_read_at(reader, offset, bytes, sizeof(bytes),
			    &count, NULL) == FSIMG_OK);
			CHECK(count == sizeof(bytes));
			for (size_t i = 0; i < count; i++)
				CHECK(bytes[i] == marker_byte(id, offset + i));
		}
	}
	CHECK(status == FSIMG_DONE && files == 3 && seen == 7);
}

int
main(int argc, char **argv)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	fsimg_options_t limits;
	fsimg_write_options_t options;
	fsimg_source_ops_t provider;
	fsimg_source_t source;
	fsimg_entry_t entry;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_verify_report_t report;
	fsimg_stats_t stats;
	fsimg_error_t error;
	struct stat storage;
	struct rusage usage;
	struct timespec before, after;
	uint64_t zero_length, validations, rss, volume_bytes;
	char directory[] = "exfat-scale-XXXXXX";
	char *number_end;
	int parent, scratch, fd, result;
	bool huge;
	fsimg_status_t status;

	huge = argc == 2 && strcmp(argv[1], "--huge") == 0;
	CHECK(argc == 1 || huge || (argc == 3 && strcmp(argv[1], "--volume") == 0));
	volume_bytes = huge ? UINT64_C(128) << 40 : UINT64_C(32) << 20;
	if (argc == 3) {
		errno = 0;
		volume_bytes = strtoull(argv[2], &number_end, 10);
		CHECK(errno == 0 && *number_end == '\0');
		CHECK(volume_bytes >= (UINT64_C(32) << 20) && volume_bytes <= (UINT64_C(128) << 40));
		CHECK(volume_bytes % 512 == 0);
	}
	zero_length = huge ? (UINT64_C(8) << 40) + 17 : 4 * 1024 * 1024 + 17;
	CHECK(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(mkdirat(parent, "scratch", 0700) == 0);
	scratch = openat(parent, "scratch", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = UINT64_C(64) << 20;
	limits.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&limits, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	options.volume_bytes = volume_bytes;
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_writer_new(ctx, "exfat", &options, &writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&provider);
	provider.validate = source_validate;
	provider.read_at = source_read;
	validations = 0;
	CHECK(fsimg_writer_set_sources(writer, &provider, &validations, NULL) == FSIMG_OK);
	entry = (fsimg_entry_t){ .path = "/Z.end", .kind = FSIMG_ENTRY_FILE, .size = 65537 };
	source = (fsimg_source_t){ .kind = FSIMG_SOURCE_PROVIDER, .id = 2 };
	CHECK(fsimg_writer_add(writer, &entry, &source, NULL) == FSIMG_OK);
	entry.path = "/A.begin";
	source.id = 1;
	CHECK(fsimg_writer_add(writer, &entry, &source, NULL) == FSIMG_OK);
	entry.path = "/B.zero";
	entry.size = zero_length;
	source = (fsimg_source_t){ .kind = FSIMG_SOURCE_ZERO };
	CHECK(fsimg_writer_add(writer, &entry, &source, NULL) == FSIMG_OK);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.volume_bytes == options.volume_bytes);
	CHECK(info.payload_bytes == zero_length + 131074 && info.initialized_bytes == 131074);
	status = fsimg_writer_write_at(writer, parent, "volume.img", &error);
	result = 0;
	if (status == FSIMG_EIO && error.os_error == EFBIG) {
		CHECK(printf("SKIP: host rejects a %" PRIu64 "-byte sparse regular file: %s\n",
		    options.volume_bytes, error.message) >= 0);
		result = 77;
	}
	if (status != FSIMG_OK && result != 77)
		fprintf(stderr, "sparse exFAT emission: %d errno=%d %s\n",
		    status, error.os_error, error.message);
	CHECK(status == FSIMG_OK || result == 77);
	fsimg_writer_free(writer);
	if (result == 0) {
		CHECK(validations >= 4);
		fd = openat(parent, "volume.img", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(fd != -1 && fstat(fd, &storage) == 0);
		CHECK((uint64_t)storage.st_size == options.volume_bytes);
		CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(ctx, io, NULL, &reader, &error) == FSIMG_OK);
		check_content(reader, zero_length);
		fsimg_reader_free(reader);
		CHECK(fsimg_reader_open(ctx, io, NULL, &reader, &error) == FSIMG_OK);
		status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
		if (status != FSIMG_OK)
			fprintf(stderr, "sparse exFAT verify: %d %s\n", status, error.message);
		CHECK(status == FSIMG_OK && report.complete && report.files == 3);
		CHECK(report.payload_bytes_read == 131074);
		fsimg_reader_free(reader);
		fsimg_io_free(io);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(parent, "volume.img", 0) == 0);
		CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
		CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.memory_peak <= limits.memory_bytes);
		/* These three-file profiles need only bounded metadata and initialized
		 * marker reads. A capacity increase must not scan unreferenced payload.
		 */
		CHECK(stats.read_bytes <= (UINT64_C(64) << 20));
		/* A zero peak means the host has not recorded a usable RSS sample. */
		CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0);
#if defined(__APPLE__)
		rss = (uint64_t)usage.ru_maxrss;
#else
		CHECK((uint64_t)usage.ru_maxrss <= UINT64_MAX / 1024);
		rss = (uint64_t)usage.ru_maxrss * 1024;
#endif
		/* st_blocks is a filesystem allocation sample, not settled device
		 * writes: compression and delayed allocation can affect this value.
		 */
		CHECK(printf("exfat sparse volume=%" PRIu64 " logical_payload=%" PRIu64
		    " initialized_payload=%" PRIu64 " cluster=%u allocated_blocks_sample_bytes=%" PRIu64
		    " written=%" PRIu64 " read=%" PRIu64 " memory_peak=%" PRIu64
		    " scratch_peak=%" PRIu64 " fds_peak=%" PRIu64 " rss=%" PRIu64
		    " rss_available=%s elapsed=%.3f\n", info.volume_bytes, info.payload_bytes,
		    info.initialized_bytes, info.allocation_unit_bytes,
		    (uint64_t)storage.st_blocks * 512, stats.written_bytes, stats.read_bytes,
		    stats.memory_peak, stats.scratch_peak, stats.descriptors_peak, rss,
		    rss != 0 ? "true" : "false",
		    (double)(after.tv_sec - before.tv_sec) +
		    (double)(after.tv_nsec - before.tv_nsec) / 1000000000.0) >= 0);
	}
	CHECK(fflush(stdout) == 0);
	fsimg_ctx_free(ctx);
	CHECK(close(scratch) == 0 && unlinkat(parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0 && rmdir(directory) == 0);
	return (result);
}
