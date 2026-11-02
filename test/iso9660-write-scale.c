/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "scale.h"
#include "test.h"

#define MILLION 1000000u
#define MAXIMUM_VOLUME (UINT64_C(0xffffffff) * 2048)
#define RSS_ALLOWANCE (UINT64_C(64) << 20)

static void
require_ok(fsimg_status_t status, const fsimg_error_t *error)
{
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: %s\n", fsimg_status_string(status), error->message);
	CHECK(status == FSIMG_OK);
}

static double
seconds(void)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return ((double)now.tv_sec + (double)now.tv_nsec / 1000000000.0);
}

/* Generate the catalog directly: no host source tree, payload staging or
 * resident array of entries can dominate the driver's measured resources.
 */
static void
add_files(fsimg_writer_t *writer, unsigned count)
{
	fsimg_entry_t entry;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_error_t error = { 0 };
	char path[32];

	for (unsigned i = 0; i < count; i++) {
		CHECK(snprintf(path, sizeof(path), "/F%07u.TXT", i) > 0);
		bzero(&entry, sizeof(entry));
		entry.path = path;
		entry.kind = FSIMG_ENTRY_FILE;
		require_ok(fsimg_writer_add(writer, &entry, &source, &error), &error);
	}
}

static void
run(bool capacity)
{
	fsimg_options_t limits;
	fsimg_write_options_t options;
	fsimg_read_options_t read_options;
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_reader_t *reader;
	fsimg_io_t *io;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_stats_t baseline, planned, emitted, finished;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	char directory[] = "iso-write-scale-XXXXXX";
	unsigned count = capacity ? 1 : MILLION;
	uint64_t rss = 0;
	double started, plan_seconds, emit_seconds = 0, verify_seconds = 0;
	int parent, image;

	CHECK(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(parent != -1);
	fsimg_options_init(&limits);
	limits.memory_bytes = SCALE_BUDGET;
	limits.scratch_directory_fd = parent;
	limits.max_open_files = 64;
	require_ok(fsimg_ctx_new(&limits, &ctx, &error), &error);
	require_ok(fsimg_ctx_get_stats(ctx, &baseline, &error), &error);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	if (capacity)
		options.volume_bytes = MAXIMUM_VOLUME;
	require_ok(fsimg_writer_new(ctx, "iso9660", &options, &writer, &error), &error);
	started = seconds();
	add_files(writer, count);
	require_ok(fsimg_writer_plan(writer, &plan, &error), &error);
	plan_seconds = seconds() - started;
	require_ok(fsimg_plan_get_info(plan, &info, &error), &error);
	CHECK(info.entries == count && info.payload_bytes == 0);
	CHECK(info.sector_bytes == 2048 && info.allocation_unit_bytes == 2048);
	if (capacity)
		CHECK(info.volume_bytes == MAXIMUM_VOLUME && info.allocation_units == UINT32_MAX);
	require_ok(fsimg_ctx_get_stats(ctx, &planned, &error), &error);
	scale_rss_sample(&rss);
	emitted = planned;
	if (!capacity) {
		started = seconds();
		require_ok(fsimg_writer_write_at(writer, parent, "wide.iso", &error), &error);
		emit_seconds = seconds() - started;
		require_ok(fsimg_ctx_get_stats(ctx, &emitted, &error), &error);
		scale_rss_sample(&rss);
	}
	fsimg_writer_free(writer);
	if (!capacity) {
		/* Writer and reader are independent operations. Release the plan
		 * before opening the emitted image under the same aggregate budget.
		 */
		image = openat(parent, "wide.iso", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(image != -1);
		require_ok(fsimg_io_from_fd(ctx, image, FSIMG_IO_READ, &io, &error), &error);
		CHECK(close(image) == 0);
		fsimg_read_options_init(&read_options);
		started = seconds();
		require_ok(fsimg_reader_open(ctx, io, &read_options, &reader, &error), &error);
		require_ok(fsimg_verify(reader, 0, &report, &error), &error);
		CHECK(report.complete && report.files == count);
		verify_seconds = seconds() - started;
		scale_rss_sample(&rss);
		fsimg_reader_free(reader);
		fsimg_io_free(io);
		CHECK(unlinkat(parent, "wide.iso", 0) == 0);
	}
	require_ok(fsimg_ctx_get_stats(ctx, &finished, &error), &error);
	CHECK(finished.memory_bytes == baseline.memory_bytes);
	CHECK(finished.scratch_bytes == 0 && finished.descriptors == baseline.descriptors);
	CHECK(finished.memory_peak <= SCALE_BUDGET && rss <= SCALE_BUDGET + RSS_ALLOWANCE);
	if (!capacity)
		CHECK(planned.scratch_peak > SCALE_BUDGET);
	printf("iso9660 writer profile=%s entries=%u plan_seconds=%.3f emit_seconds=%.3f"
	    " verify_seconds=%.3f volume_bytes=%" PRIu64 " plan_io_calls=%" PRIu64
	    " emit_io_calls=%" PRIu64 " writer_memory_peak=%" PRIu64
	    " scratch_peak=%" PRIu64 " descriptors_peak=%" PRIu64 " rss=%" PRIu64 "\n",
	    capacity ? "maximum-volume-plan" : "million-files", count, plan_seconds,
	    emit_seconds, verify_seconds, info.volume_bytes, planned.io_calls,
	    emitted.io_calls - planned.io_calls, emitted.memory_peak, emitted.scratch_peak,
	    emitted.descriptors_peak, rss);
	CHECK(fflush(stdout) == 0);
	fsimg_ctx_free(ctx);
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(strcmp(argv[1], "--wide") == 0 || strcmp(argv[1], "--capacity") == 0);
	run(strcmp(argv[1], "--capacity") == 0);
	return (0);
}
