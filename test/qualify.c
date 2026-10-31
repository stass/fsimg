/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "test.h"

static void require_ok(fsimg_status_t, const fsimg_error_t *);
static void plan_catalog(uint64_t, bool);

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
plan_catalog(uint64_t files, bool wide)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_options_t limits;
	fsimg_write_options_t options;
	fsimg_entry_t entry = { .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_stats_t baseline, stats;
	fsimg_error_t error;
	struct timespec start, end;
	struct rusage usage;
	char directory[] = "qualification-catalog-XXXXXX";
	char path[64];
	uint64_t groups, rss;
	int scratch;

	groups = wide ? 0 : (files + 999) / 1000;
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = (wide ? UINT64_C(16) : UINT64_C(256)) << 20;
	limits.max_entries = files + groups;
	limits.max_path_bytes = 512;
	limits.max_open_files = 16;
	limits.scratch_directory_fd = scratch;
	require_ok(fsimg_ctx_new(&limits, &ctx, &error), &error);
	CHECK(close(scratch) == 0);
	require_ok(fsimg_ctx_get_stats(ctx, &baseline, &error), &error);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	require_ok(fsimg_writer_new(ctx, "exfat", &options, &writer, &error), &error);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);

	/* Reverse order forces catalog sorting. Every path is generated in a
	 * reusable buffer; there is no application-owned per-entry catalog.
	 */
	for (uint64_t remaining = files; remaining != 0; remaining--) {
		uint64_t file = remaining - 1;

		if (wide)
			snprintf(path, sizeof(path), "/f%08" PRIu64, file);
		else
			snprintf(path, sizeof(path), "/d%08" PRIu64 "/f%08" PRIu64,
			    file / 1000, file);
		entry.path = path;
		require_ok(fsimg_writer_add(writer, &entry, &zero, &error), &error);
	}
	entry.kind = FSIMG_ENTRY_DIRECTORY;
	for (uint64_t remaining = groups; remaining != 0; remaining--) {
		snprintf(path, sizeof(path), "/d%08" PRIu64, remaining - 1);
		entry.path = path;
		require_ok(fsimg_writer_add(writer, &entry, NULL, &error), &error);
	}
	require_ok(fsimg_writer_plan(writer, &plan, &error), &error);
	require_ok(fsimg_plan_get_info(plan, &info, &error), &error);
	CHECK(strcmp(info.format, "exfat") == 0 && info.entries == files + groups);
	CHECK(info.payload_bytes == 0 && info.initialized_bytes == 0);
	CHECK(info.metadata_bytes != 0 && info.allocation_unit_bytes != 0);
	CHECK(info.allocated_bytes <= info.metadata_bytes);
	CHECK(info.metadata_bytes <= info.volume_bytes);
	CHECK(info.free_bytes <= info.volume_bytes - info.metadata_bytes);
	CHECK(info.volume_bytes - info.metadata_bytes - info.free_bytes <
	    info.allocation_unit_bytes);
	require_ok(fsimg_ctx_get_stats(ctx, &stats, &error), &error);
	CHECK(stats.memory_peak <= limits.memory_bytes);
	CHECK(stats.scratch_peak <= limits.scratch_bytes);
	CHECK(stats.descriptors_peak <= limits.max_open_files);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	/* Short processes may finish before the host records an RSS sample. */
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0);
#if defined(__APPLE__)
	rss = (uint64_t)usage.ru_maxrss;
#else
	CHECK((uint64_t)usage.ru_maxrss <= UINT64_MAX / 1024);
	rss = (uint64_t)usage.ru_maxrss * 1024;
#endif
	CHECK(printf("exfat plan files=%" PRIu64 " directories=%" PRIu64
	    " entries=%" PRIu64 " volume=%" PRIu64 " metadata=%" PRIu64
	    " allocated=%" PRIu64 " free=%" PRIu64 " cluster=%u"
	    " budget=%" PRIu64 " memory_peak=%" PRIu64 " scratch_peak=%" PRIu64
	    " physical_scratch_peak=%" PRIu64 " fds_peak=%" PRIu64
	    " io_calls=%" PRIu64 " read=%" PRIu64 " written=%" PRIu64
	    " rss=%" PRIu64 " rss_available=%s collection_plan_seconds=%.3f\n",
	    files, groups, info.entries,
	    info.volume_bytes, info.metadata_bytes, info.allocated_bytes, info.free_bytes,
	    info.allocation_unit_bytes, limits.memory_bytes,
	    stats.memory_peak, stats.scratch_peak, stats.scratch_physical_peak,
	    stats.descriptors_peak, stats.io_calls, stats.read_bytes,
	    stats.written_bytes, rss, rss != 0 ? "true" : "false", (double)(end.tv_sec - start.tv_sec) +
	    (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0) >= 0);
	CHECK(fflush(stdout) == 0);

	fsimg_writer_free(writer);
	require_ok(fsimg_ctx_get_stats(ctx, &stats, &error), &error);
	CHECK(stats.memory_bytes == baseline.memory_bytes);
	CHECK(stats.descriptors == baseline.descriptors && stats.scratch_bytes == 0);
	fsimg_ctx_free(ctx);
	CHECK(rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
	uint64_t files;
	char *end;

	if (argc == 1) {
		plan_catalog(1001, false);
		plan_catalog(1001, true);
		return (0);
	}
	CHECK(argc == 3 && (strcmp(argv[1], "--catalog") == 0 ||
	    strcmp(argv[1], "--wide") == 0));
	errno = 0;
	files = strtoull(argv[2], &end, 10);
	CHECK(errno == 0 && *end == '\0' && files > 0 && files <= 10000000);
	plan_catalog(files, strcmp(argv[1], "--wide") == 0);
	return (0);
}
