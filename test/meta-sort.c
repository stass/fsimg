/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "host.h"
#include "meta.h"
#include "test.h"

typedef struct oracle_record {
	uint64_t	 ordinal;
	size_t		 length;
	unsigned	 key;
	unsigned char	 bytes[64];
} oracle_record_t;

typedef struct sort_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	 session;
	fsimg_stream_t	*input;
	fsimg_stats_t	 baseline;
	const char	*cancel_phase;
	uint64_t	 comparisons;
	bool		 cancel;
	bool		 check_reentry;
	char		 directory[64];
} sort_fixture_t;

static void fixture_open(sort_fixture_t *, uint32_t, bool, uint32_t);
static void fixture_close(sort_fixture_t *);
static bool cancelled(void *);
static void progress(void *, const fsimg_progress_t *);
static oracle_record_t make_record(uint64_t);
static int oracle_compare(const void *, const void *);
static int record_compare(const void *, size_t, const void *, size_t, void *);
static void add_records(sort_fixture_t *, uint64_t);
static void check_original(sort_fixture_t *, uint64_t);
static void check_resources(sort_fixture_t *, const fsimg_stats_t *);
static void test_oracle(uint64_t, uint32_t, size_t, uint32_t, bool);
static void test_failures(void);
static void test_cancellation(void);
static void test_corruption_and_limits(void);
static void test_scale(void);
static int scale_compare(const void *, size_t, const void *, size_t, void *);

static bool
cancelled(void *argument)
{
	sort_fixture_t *fixture = argument;

	return (fixture->cancel);
}

static void
progress(void *argument, const fsimg_progress_t *state)
{
	sort_fixture_t *fixture = argument;

	if (fixture->cancel_phase != NULL &&
	    strcmp(fixture->cancel_phase, state->phase) == 0)
		fixture->cancel = true;
}

static void
fixture_open(sort_fixture_t *fixture, uint32_t pages, bool scratch,
    uint32_t maximum)
{
	fsimg_options_t options;
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "sort-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fd = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_bytes = 256 * 1024 * 1024;
	options.max_open_files = 4;
	options.scratch_directory_fd = scratch ? fd : -1;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	options.progress = progress;
	options.progress_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_session_init(&fixture->session, fixture->ctx, "sort fixture", 0);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_new(&fixture->session, maximum, &fixture->input, NULL) == FSIMG_OK);
}

static void
fixture_close(sort_fixture_t *fixture)
{
	fsimg_stream_free(fixture->input);
	check_resources(fixture, &fixture->baseline);
	fsimg_ctx_free(fixture->ctx);
	CHECK(rmdir(fixture->directory) == 0);
}

static oracle_record_t
make_record(uint64_t ordinal)
{
	oracle_record_t record = { .ordinal = ordinal };

	/* Duplicate keys and varying lengths expose unstable merge tie breaking. */
	record.key = (unsigned)((ordinal * 37 + ordinal / 7) % 13);
	record.length = ordinal % 19 == 0 ? 0 : 9 + ordinal % 56;
	if (record.length == 0)
		record.key = 0;
	else {
		record.bytes[0] = (unsigned char)record.key;
		for (unsigned j = 0; j < 8; j++)
			record.bytes[j + 1] = (unsigned char)(ordinal >> (8 * j));
		for (size_t j = 9; j < record.length; j++)
			record.bytes[j] = (unsigned char)(ordinal * 17 + j);
	}
	return (record);
}

static int
oracle_compare(const void *a, const void *b)
{
	const oracle_record_t *left = a, *right = b;

	if (left->key != right->key)
		return ((left->key > right->key) - (left->key < right->key));
	return ((left->ordinal > right->ordinal) -
	    (left->ordinal < right->ordinal));
}

static int
record_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument)
{
	sort_fixture_t *fixture = argument;
	const unsigned char *left = a, *right = b;
	fsimg_stream_t *unexpected;
	fsimg_stats_t stats;
	unsigned a_key, b_key;

	fixture->comparisons++;
	if (fixture->check_reentry) {
		CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_ESTATE);
		CHECK(fsimg_stream_new(&fixture->session, 1, &unexpected, NULL) == FSIMG_ESTATE);
		CHECK(unexpected == NULL);
		fsimg_stream_free(fixture->input);
		fixture->check_reentry = false;
	}
	a_key = a_length == 0 ? 0 : left[0];
	b_key = b_length == 0 ? 0 : right[0];
	return ((a_key > b_key) - (a_key < b_key));
}

static void
add_records(sort_fixture_t *fixture, uint64_t count)
{
	oracle_record_t record;

	for (uint64_t i = 0; i < count; i++) {
		record = make_record(i);
		CHECK(fsimg_stream_append(fixture->input, record.bytes,
		    record.length, NULL, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_stream_seal(fixture->input, NULL) == FSIMG_OK);
}

static void
check_original(sort_fixture_t *fixture, uint64_t count)
{
	oracle_record_t record;
	unsigned char data[64];
	uint64_t cursor;
	size_t length;

	cursor = 0;
	for (uint64_t i = 0; i < count; i++) {
		record = make_record(i);
		CHECK(fsimg_stream_next(fixture->input, &cursor, data,
		    sizeof(data), &length, NULL) == FSIMG_OK);
		CHECK(length == record.length);
		CHECK(bcmp(data, record.bytes, length) == 0);
	}
	CHECK(fsimg_stream_next(fixture->input, &cursor, data,
	    sizeof(data), &length, NULL) == FSIMG_DONE);
}

static void
check_resources(sort_fixture_t *fixture, const fsimg_stats_t *baseline)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == baseline->memory_bytes);
	CHECK(stats.scratch_bytes == baseline->scratch_bytes);
	CHECK(stats.descriptors == baseline->descriptors);
	CHECK(stats.memory_peak <= fixture->ctx->options.memory_bytes);
	CHECK(stats.scratch_peak <= fixture->ctx->options.scratch_bytes);
	CHECK(stats.descriptors_peak <= fixture->ctx->options.max_open_files);
}

static void
test_oracle(uint64_t count, uint32_t pages, size_t run_bytes,
    uint32_t fan_in, bool scratch)
{
	sort_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = run_bytes, .fan_in = fan_in };
	oracle_record_t *oracle;
	fsimg_stream_t *output;
	unsigned char data[64];
	uint64_t cursor;
	size_t length;

	fixture_open(&fixture, pages, scratch, sizeof(data));
	fixture.check_reentry = true;
	oracle = calloc(count == 0 ? 1 : (size_t)count, sizeof(*oracle));
	CHECK(oracle != NULL);
	for (uint64_t i = 0; i < count; i++)
		oracle[i] = make_record(i);
	/* libc qsort over integer keys/ordinals is independent of streamed heaps. */
	qsort(oracle, (size_t)count, sizeof(*oracle), oracle_compare);
	add_records(&fixture, count);
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, NULL) == FSIMG_OK);
	CHECK(output->count == count && output->sealed);
	cursor = 0;
	for (uint64_t i = 0; i < count; i++) {
		CHECK(fsimg_stream_next(output, &cursor, data, sizeof(data),
		    &length, NULL) == FSIMG_OK);
		CHECK(length == oracle[i].length);
		CHECK(bcmp(data, oracle[i].bytes, length) == 0);
	}
	CHECK(fsimg_stream_next(output, &cursor, data, sizeof(data),
	    &length, NULL) == FSIMG_DONE);
	check_original(&fixture, count);
	fsimg_stream_free(output);
	free(oracle);
	fixture_close(&fixture);
}

static void
test_failures(void)
{
	sort_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 256, .fan_in = 2 };
	fsimg_stream_t *output;
	fsimg_stats_t baseline;
	fsimg_error_t error;
	fsimg_status_t status;
	bool reached_success;
	const uint64_t failure_points[] = { 1, 3, 15, 100 };

	fixture_open(&fixture, 2, true, 64);
	add_records(&fixture, 160);
	CHECK(fsimg_store_flush(fixture.input->store, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &baseline, NULL) == FSIMG_OK);
	reached_success = false;
	for (uint64_t attempt = 1; attempt <= 64; attempt++) {
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_stream_sort(fixture.input, record_compare, &fixture,
		    &options, &output, &error);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK) {
			fsimg_stream_free(output);
			reached_success = true;
		} else {
			CHECK(status == FSIMG_ENOMEM && output == NULL);
			CHECK(error.status == status);
		}
		check_resources(&fixture, &baseline);
		check_original(&fixture, 160);
		if (reached_success)
			break;
	}
	CHECK(reached_success);
	for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE; point++) {
		for (size_t i = 0; i < sizeof(failure_points) / sizeof(failure_points[0]); i++) {
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)point, failure_points[i], ENOSPC);
			status = fsimg_stream_sort(fixture.input, record_compare,
			    &fixture, &options, &output, &error);
			fsimg_test_meta_fail_after(fixture.ctx, (fsimg_meta_fault_t)point, 0, 0);
			if (status == FSIMG_OK)
				fsimg_stream_free(output);
			else {
				CHECK(status == FSIMG_EIO && output == NULL);
				CHECK(error.os_error == ENOSPC);
			}
			check_resources(&fixture, &baseline);
			check_original(&fixture, 160);
		}
	}
	fixture.ctx->options.scratch_bytes = baseline.scratch_bytes + 1000;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, &error) == FSIMG_ELIMIT);
	CHECK(output == NULL && error.resource == FSIMG_RESOURCE_SCRATCH);
	fixture.ctx->options.scratch_bytes = 256 * 1024 * 1024;
	check_resources(&fixture, &baseline);
	check_original(&fixture, 160);
	fixture_close(&fixture);
}

static void
test_cancellation(void)
{
	const char *phases[] = { "metadata runs", "metadata merge", "read", "write" };
	sort_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 256, .fan_in = 3 };
	fsimg_stream_t *output;
	fsimg_stats_t baseline;

	fixture_open(&fixture, 2, true, 64);
	add_records(&fixture, 200);
	CHECK(fsimg_store_flush(fixture.input->store, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &baseline, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
		fixture.cancel_phase = phases[i];
		CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
		    &options, &output, NULL) == FSIMG_ECANCELLED);
		CHECK(output == NULL);
		fixture.cancel_phase = NULL;
		fixture.cancel = false;
		check_resources(&fixture, &baseline);
		check_original(&fixture, 200);
	}
	fixture_close(&fixture);
}

static void
test_corruption_and_limits(void)
{
	sort_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 1, .fan_in = 2 };
	fsimg_stream_t *output;
	fsimg_error_t error;
	uint64_t work;

	fixture_open(&fixture, 32, false, 64);
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    NULL, &output, NULL) == FSIMG_ESTATE);
	add_records(&fixture, 20);
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, NULL) == FSIMG_ELIMIT);
	options.run_bytes = 4096;
	options.fan_in = 1;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, NULL) == FSIMG_EINVAL);
	options.fan_in = 2;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, NULL) == FSIMG_OK);
	fsimg_stream_free(output);
	work = fixture.session.progress.work;
	fixture.ctx->options.max_work = work + 1;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, &error) == FSIMG_ELIMIT);
	CHECK(output == NULL && error.resource == FSIMG_RESOURCE_WORK);
	CHECK(fixture.session.progress.work == work + 1);
	fixture.ctx->options.max_work = 0;
	/* These 20 records fit one page/run: 20 frame reads and 18 payload
	 * reads consume the allowance before the first comparison is called.
	 */
	fixture.comparisons = 0;
	fixture.ctx->options.max_work = fixture.session.progress.work + 38;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, &error) == FSIMG_ELIMIT);
	CHECK(output == NULL && error.resource == FSIMG_RESOURCE_WORK);
	CHECK(fixture.comparisons == 0);
	fixture.ctx->options.max_work = 0;
	fixture.input->count++;
	CHECK(fsimg_stream_sort(fixture.input, record_compare, &fixture,
	    &options, &output, NULL) == FSIMG_ECORRUPT);
	CHECK(output == NULL);
	fixture.input->count--;
	check_original(&fixture, 20);
	fixture_close(&fixture);
}

static int
scale_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	uint64_t left, right;

	CHECK(a_length == 2048 && b_length == 2048);
	left = fsimg_load_le64(a);
	right = fsimg_load_le64(b);
	return ((left > right) - (left < right));
}

static void
test_scale(void)
{
	sort_fixture_t fixture;
	fsimg_sort_options_t options = { .run_bytes = 256 * 1024, .fan_in = 8 };
	fsimg_stream_t *output;
	fsimg_array_t *array;
	fsimg_stats_t stats;
	struct rusage usage;
	struct timespec start, end;
	unsigned char data[2048], value[8];
	uint64_t count, cursor, index;
	size_t length;
	double seconds;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	fixture_open(&fixture, 16, true, sizeof(data));
	count = 12 * 1024;
	for (uint64_t i = count; i != 0; i--) {
		memset(data, (unsigned char)(i - 1), sizeof(data));
		fsimg_store_le64(data, i - 1);
		CHECK(fsimg_stream_append(fixture.input, data, sizeof(data), NULL, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_stream_seal(fixture.input, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_sort(fixture.input, scale_compare, NULL,
	    &options, &output, NULL) == FSIMG_OK);
	cursor = 0;
	for (uint64_t i = 0; i < count; i++) {
		CHECK(fsimg_stream_next(output, &cursor, data, sizeof(data), &length, NULL) == FSIMG_OK);
		CHECK(length == sizeof(data) && fsimg_load_le64(data) == i);
		for (size_t j = 8; j < sizeof(data); j++)
			CHECK(data[j] == (unsigned char)i);
	}
	CHECK(fsimg_stream_next(output, &cursor, data, sizeof(data), &length, NULL) == FSIMG_DONE);
	fsimg_stream_free(output);
	fsimg_stream_free(fixture.input);
	fixture.input = NULL;

	/* Fixed-record lookup also exceeds the complete resident memory budget. */
	count = 3 * 1024 * 1024;
	CHECK(fsimg_array_new(&fixture.session, sizeof(value), count, &array, NULL) == FSIMG_OK);
	for (uint64_t i = 0; i < count; i++) {
		fsimg_store_le64(value, i * 37 + 1);
		CHECK(fsimg_array_set(array, i, value, NULL) == FSIMG_OK);
	}
	for (uint64_t i = 0; i < 10000; i++) {
		index = i * UINT64_C(104729) % count;
		CHECK(fsimg_array_get(array, index, value, NULL) == FSIMG_OK);
		CHECK(fsimg_load_le64(value) == index * 37 + 1);
	}
	fsimg_array_free(array);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_peak > fixture.ctx->options.memory_bytes);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
	seconds = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
	printf("metadata scale: 24 MiB sort and 24 MiB array, 16 MiB budget\n"
	    "memory peak=%" PRIu64 ", scratch logical peak=%" PRIu64
	    ", sampled physical peak=%" PRIu64 ", IO calls=%" PRIu64
	    ", read=%" PRIu64 ", written=%" PRIu64 ", seconds=%.3f\n",
	    stats.memory_peak, stats.scratch_peak, stats.scratch_physical_peak,
	    stats.io_calls, stats.read_bytes, stats.written_bytes, seconds);
	/* getrusage units differ by host; preserve the host value in evidence. */
	printf("getrusage ru_maxrss=%ld (host units), merge fan-in=8, cache pages=16\n",
	    usage.ru_maxrss);
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_scale();
		return (0);
	}
	CHECK(argc == 1);
	test_oracle(0, 32, 4096, 2, false);
	test_oracle(1, 32, 4096, 2, false);
	test_oracle(3, 32, 4096, 2, false);
	test_oracle(30, 32, 4096, 2, false);
	for (uint32_t pages = 1; pages <= 16; pages *= 4) {
		test_oracle(400, pages, 256, 2, true);
		test_oracle(400, pages, 1024, 3, true);
		test_oracle(400, pages, 65536, 16, true);
	}
	test_failures();
	test_cancellation();
	test_corruption_and_limits();
	return (0);
}
