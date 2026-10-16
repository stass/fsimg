/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "meta.h"
#include "test.h"

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	 session;
	fsimg_stats_t	 baseline;
	char		 directory[64];
} fixture_t;

static void fixture_open(fixture_t *, uint32_t, uint64_t, uint32_t);
static void fixture_close(fixture_t *);
static void test_arrays(void);
static void test_resident(void);
static void test_streams(void);
static void test_sparse_and_quotas(void);
static void test_failures(void);
static void test_retained_dirty_page(void);
static void test_exclusive_scratch(void);
static void test_aggregate_resident_lengths(void);
static void test_input_survives_spill_failure(void);
static int compare_bytes(const void *, size_t, const void *, size_t, void *);
static void test_scratch_overflow(void);
static void test_sort_work_limit(void);
static void test_transactional_eviction(void);

static void
fixture_open(fixture_t *fixture, uint32_t pages, uint64_t scratch,
    uint32_t descriptors)
{
	fsimg_options_t options;
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "meta-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fd = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_bytes = scratch;
	options.max_open_files = descriptors;
	options.scratch_directory_fd = fd;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_session_init(&fixture->session, fixture->ctx, "metadata test", 0);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
}

static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	CHECK(stats.scratch_bytes == 0 && stats.scratch_physical_bytes == 0);
	CHECK(stats.memory_peak <= fixture->ctx->options.memory_bytes);
	fsimg_ctx_free(fixture->ctx);
	/* A surviving scratch name makes rmdir fail even if all FDs were closed. */
	CHECK(rmdir(fixture->directory) == 0);
}

static void
test_arrays(void)
{
	fixture_t fixture;
	fsimg_array_t *array, *wide, *bad;
	fsimg_bitset_t *bitset;
	unsigned char value[31], expected[31], large[7001], readback[7001];
	uint64_t index;
	bool was_set;

	fixture_open(&fixture, 2, 1024 * 1024, 4);
	CHECK(fsimg_array_new(&fixture.session, sizeof(value), 5000, &array, NULL) == FSIMG_OK);
	for (uint64_t i = 0; i < 5000; i++) {
		index = i * 37 % 5000;
		for (size_t j = 0; j < sizeof(value); j++)
			value[j] = (unsigned char)(index * 13 + j);
		CHECK(fsimg_array_set(array, index, value, NULL) == FSIMG_OK);
	}
	for (uint64_t i = 0; i < 5000; i++) {
		for (size_t j = 0; j < sizeof(expected); j++)
			expected[j] = (unsigned char)(i * 13 + j);
		CHECK(fsimg_array_get(array, i, value, NULL) == FSIMG_OK);
		CHECK(bcmp(value, expected, sizeof(value)) == 0);
	}
	CHECK(fsimg_array_get(array, 5000, value, NULL) == FSIMG_ERANGE);
	CHECK(fsimg_array_set(array, UINT64_MAX, value, NULL) == FSIMG_ERANGE);
	fsimg_array_free(array);

	CHECK(fsimg_array_new(&fixture.session, sizeof(large), 3, &wide, NULL) == FSIMG_OK);
	memset(large, 0x97, sizeof(large));
	CHECK(fsimg_array_set(wide, 1, large, NULL) == FSIMG_OK);
	CHECK(fsimg_array_get(wide, 1, readback, NULL) == FSIMG_OK);
	CHECK(bcmp(large, readback, sizeof(large)) == 0);
	CHECK(fsimg_array_get(wide, 0, readback, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(readback); i++)
		CHECK(readback[i] == 0);
	fsimg_array_free(wide);

	CHECK(fsimg_bitset_new(&fixture.session, 200003, &bitset, NULL) == FSIMG_OK);
	for (uint64_t i = 0; i < 200003; i += 47) {
		CHECK(fsimg_bitset_test_and_set(bitset, i, &was_set, NULL) == FSIMG_OK);
		CHECK(!was_set);
	}
	for (uint64_t i = 0; i < 200003; i += 47) {
		CHECK(fsimg_bitset_test_and_set(bitset, i, &was_set, NULL) == FSIMG_OK);
		CHECK(was_set);
	}
	CHECK(fsimg_bitset_test_and_set(bitset, 200003, &was_set, NULL) == FSIMG_ERANGE);
	CHECK(!was_set);
	fsimg_bitset_free(bitset);
	CHECK(fsimg_array_new(&fixture.session, 2, UINT64_MAX, &bad, NULL) == FSIMG_ERANGE);
	CHECK(bad == NULL);
	CHECK(fsimg_array_new(&fixture.session, 1, UINT64_MAX, &bad, NULL) == FSIMG_ERANGE);
	CHECK(bad == NULL);
	CHECK(fsimg_array_new(&fixture.session, 1, 0, &bad, NULL) == FSIMG_OK);
	CHECK(fsimg_array_get(bad, 0, value, NULL) == FSIMG_ERANGE);
	fsimg_array_free(bad);
	fixture_close(&fixture);
}

static void
test_resident(void)
{
	fsimg_ctx_t *ctx;
	fsimg_session_t session;
	fsimg_store_t *a, *b;
	fsimg_stats_t baseline, stats;
	fsimg_error_t error;
	unsigned char value;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_session_init(&session, ctx, "resident test", 0);
	CHECK(fsimg_test_meta_cache(ctx, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_store_new(&session, &a, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(ctx, 1, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_store_new(&session, &b, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(a, 4096, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(b, 4096, NULL) == FSIMG_OK);
	value = 0x9a;
	CHECK(fsimg_store_write(a, 4095, &value, 1, NULL) == FSIMG_OK);
	value = 0x2e;
	CHECK(fsimg_store_write(b, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(b, 4097, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(fsimg_store_size(b) == 4096);
	CHECK(fsimg_store_read(a, 4095, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0x9a);
	CHECK(fsimg_store_flush(a, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.io_calls == 0 && stats.scratch_bytes == 0);
	fsimg_store_free(b);
	CHECK(fsimg_store_grow(a, 8192, NULL) == FSIMG_OK);
	CHECK(fsimg_store_read(a, 8191, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0);
	fsimg_store_free(a);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == baseline.memory_bytes);
	fsimg_ctx_free(ctx);
}

static void
test_streams(void)
{
	fixture_t fixture;
	fsimg_stream_t *stream;
	unsigned char data[7001], buffer[7001], header[8];
	uint64_t cursor, reference;
	size_t length;

	fixture_open(&fixture, 1, 1024 * 1024, 4);
	CHECK(fsimg_stream_new(&fixture.session, sizeof(data), &stream, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_append(stream, NULL, 0, &reference, NULL) == FSIMG_OK);
	CHECK(reference == 0);
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 7);
	CHECK(fsimg_stream_append(stream, data, sizeof(data), &reference, NULL) == FSIMG_OK);
	CHECK(reference == 8 && stream->count == 2);
	CHECK(fsimg_stream_append(stream, data, sizeof(data) + 1, NULL, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_stream_seal(stream, NULL) == FSIMG_OK);
	CHECK(fsimg_stream_append(stream, NULL, 0, NULL, NULL) == FSIMG_ESTATE);
	cursor = 0;
	CHECK(fsimg_stream_next(stream, &cursor, NULL, 0, &length, NULL) == FSIMG_OK);
	CHECK(cursor == 8 && length == 0);
	CHECK(fsimg_stream_next(stream, &cursor, buffer, 7, &length, NULL) == FSIMG_ERANGE);
	CHECK(cursor == 8 && length == 0);
	CHECK(fsimg_stream_next(stream, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_OK);
	CHECK(length == sizeof(data) && bcmp(buffer, data, length) == 0);
	CHECK(fsimg_stream_next(stream, &cursor, NULL, 0, &length, NULL) == FSIMG_DONE);

	/* A valid frame cannot direct a copy beyond the declared record bound. */
	fsimg_meta_put_u64(header, ((uint64_t)UINT32_MAX << 32) | 1);
	CHECK(fsimg_store_write(stream->store, 8, header, sizeof(header), NULL) == FSIMG_OK);
	cursor = 8;
	CHECK(fsimg_stream_next(stream, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_ECORRUPT);
	CHECK(cursor == 8 && length == 0);
	fsimg_meta_put_u64(header, ((uint64_t)sizeof(data) << 32) | 2);
	CHECK(fsimg_store_write(stream->store, 8, header, sizeof(header), NULL) == FSIMG_OK);
	CHECK(fsimg_stream_next(stream, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_ECORRUPT);
	cursor = fsimg_store_size(stream->store) - 3;
	CHECK(fsimg_stream_next(stream, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_ECORRUPT);
	cursor = UINT64_MAX;
	CHECK(fsimg_stream_next(stream, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_ECORRUPT);
	fsimg_stream_free(stream);
	fixture_close(&fixture);
}

static void
test_sparse_and_quotas(void)
{
	fixture_t fixture;
	fsimg_store_t *store, *other;
	fsimg_error_t error;
	fsimg_stats_t stats;
	unsigned char value;
	uint64_t capacity;

	/* The large logical file must be charged even when it is almost all holes. */
	capacity = UINT64_C(1) << 30;
	fixture_open(&fixture, 1, capacity + 16, 3);
	CHECK(fsimg_store_new(&fixture.session, &store, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(store, capacity, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_bytes == capacity + 16);
	CHECK(stats.memory_bytes < 100000);
	CHECK(fsimg_store_read(store, capacity - 1, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0);
	value = 0xb7;
	CHECK(fsimg_store_write(store, capacity - 1, &value, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_store_flush(store, NULL) == FSIMG_OK);
	CHECK(fsimg_store_new(&fixture.session, &other, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(other, 8192, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SCRATCH);
	fsimg_store_free(other);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_physical_bytes != 0);
	CHECK(stats.scratch_physical_bytes <= stats.scratch_physical_peak);
	CHECK(fsimg_store_read(store, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0);
	CHECK(fsimg_store_read(store, capacity - 1, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0xb7);
	fsimg_store_free(store);
	fixture_close(&fixture);

	fixture_open(&fixture, 1, 1024 * 1024, 1);
	CHECK(fsimg_store_new(&fixture.session, &store, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(store, 8192, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DESCRIPTORS);
	fsimg_store_free(store);
	fixture_close(&fixture);
}

static void
test_failures(void)
{
	fixture_t fixture;
	fsimg_array_t *array;
	fsimg_status_t status;
	fsimg_error_t error;

	for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE; point++) {
		if (point == FSIMG_META_READ)
			continue;
		fixture_open(&fixture, 1, 1024 * 1024, 4);
		fsimg_test_meta_fail_after(fixture.ctx, (fsimg_meta_fault_t)point, 1, ENOSPC);
		CHECK(fsimg_array_new(&fixture.session, 8, 8192, &array, &error) == FSIMG_EIO);
		CHECK(array == NULL && error.os_error == ENOSPC);
		fixture_close(&fixture);
	}
	for (uint64_t attempt = 1; attempt < 10; attempt++) {
		fixture_open(&fixture, 1, 1024 * 1024, 4);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_array_new(&fixture.session, 8, 8192, &array, &error);
		if (status == FSIMG_OK)
			fsimg_array_free(array);
		else {
			CHECK(status == FSIMG_ENOMEM && array == NULL);
			CHECK(error.status == FSIMG_ENOMEM);
		}
		fixture_close(&fixture);
		if (status == FSIMG_OK)
			break;
	}
}

static void
test_retained_dirty_page(void)
{
	fixture_t fixture;
	fsimg_store_t *a, *b;
	fsimg_error_t error;
	unsigned char value;

	fixture_open(&fixture, 1, 1024 * 1024, 4);
	CHECK(fsimg_store_new(&fixture.session, &a, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(a, 8192, NULL) == FSIMG_OK);
	value = 0x6e;
	CHECK(fsimg_store_write(a, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_store_new(&fixture.session, &b, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(b, 1, NULL) == FSIMG_OK);
	fsimg_test_meta_fail_after(fixture.ctx, FSIMG_META_WRITE, 1, ENOSPC);
	CHECK(fsimg_store_read(b, 0, &value, 1, &error) == FSIMG_EIO);
	CHECK(error.os_error == ENOSPC);
	CHECK(fsimg_store_read(a, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0x6e);
	fsimg_test_meta_fail_after(fixture.ctx, FSIMG_META_READ, 1, EIO);
	CHECK(fsimg_store_read(a, 4096, &value, 1, &error) == FSIMG_EIO);
	CHECK(fsimg_store_read(a, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0x6e);
	CHECK(fsimg_store_read(b, 0, &value, 1, NULL) == FSIMG_OK);
	CHECK(value == 0);
	fsimg_store_free(a);
	fsimg_store_free(b);
	fixture_close(&fixture);
}

static void
test_exclusive_scratch(void)
{
	fixture_t fixture;
	fsimg_store_t *store;
	struct stat st;
	int directory;
	char name[64];

	fixture_open(&fixture, 1, 1024 * 1024, 4);
	directory = fixture.ctx->options.scratch_directory_fd;
	/* Preexisting symlinks and regular files must never be followed/replaced. */
	CHECK(symlinkat("outside", directory, "fsimg-meta-v1-0000000000000001") == 0);
	CHECK(fsimg_store_new(&fixture.session, &store, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(store, 8192, NULL) == FSIMG_OK);
	CHECK(fstatat(directory, "fsimg-meta-v1-0000000000000001", &st,
	    AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISLNK(st.st_mode));
	CHECK(fstatat(directory, "fsimg-meta-v1-0000000000000002", &st, 0) == -1);
	CHECK(errno == ENOENT);
	fsimg_store_free(store);
	CHECK(unlinkat(directory, "fsimg-meta-v1-0000000000000001", 0) == 0);
	for (uint64_t i = 3; i < 131; i++) {
		snprintf(name, sizeof(name), "fsimg-meta-v1-%016" PRIx64, i);
		CHECK(mkdirat(directory, name, 0700) == 0);
	}
	CHECK(fsimg_store_new(&fixture.session, &store, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(store, 8192, NULL) == FSIMG_EIO);
	fsimg_store_free(store);
	for (uint64_t i = 3; i < 131; i++) {
		snprintf(name, sizeof(name), "fsimg-meta-v1-%016" PRIx64, i);
		CHECK(unlinkat(directory, name, AT_REMOVEDIR) == 0);
	}
	fixture_close(&fixture);
}

static int
compare_bytes(const void *a, size_t a_size, const void *b, size_t b_size,
    void *argument FSIMG_UNUSED)
{
	int result;
	size_t common;

	common = a_size < b_size ? a_size : b_size;
	result = memcmp(a, b, common);
	if (result != 0)
		return (result);
	return ((a_size > b_size) - (a_size < b_size));
}

static void
test_input_survives_spill_failure(void)
{
	fixture_t fixture;
	fsimg_stream_t *input, *output;
	unsigned char buffer[100];
	uint64_t cursor;
	size_t length;

	for (unsigned point = FSIMG_META_RESIZE; point <= FSIMG_META_WRITE; point++) {
		if (point == FSIMG_META_READ)
			continue;
		fixture_open(&fixture, 1, 1024 * 1024, 4);
		CHECK(fsimg_stream_new(&fixture.session, sizeof(buffer), &input, NULL) == FSIMG_OK);
		memset(buffer, 0x42, sizeof(buffer));
		CHECK(fsimg_stream_append(input, buffer, sizeof(buffer), NULL, NULL) == FSIMG_OK);
		CHECK(fsimg_stream_seal(input, NULL) == FSIMG_OK);
		fsimg_test_meta_fail_after(fixture.ctx, (fsimg_meta_fault_t)point, 1, ENOSPC);
		CHECK(fsimg_stream_sort(input, compare_bytes, NULL, NULL, &output, NULL) == FSIMG_EIO);
		CHECK(output == NULL);
		cursor = 0;
		CHECK(fsimg_stream_next(input, &cursor, buffer, sizeof(buffer), &length, NULL) == FSIMG_OK);
		CHECK(length == sizeof(buffer) && buffer[0] == 0x42);
		fsimg_stream_free(input);
		fixture_close(&fixture);
	}
}

static void
test_aggregate_resident_lengths(void)
{
	fixture_t fixture;
	fsimg_store_t *stores[4];
	fsimg_error_t error;

	fixture_open(&fixture, 1, 4096 + 16, 5);
	for (size_t i = 0; i < 4; i++)
		CHECK(fsimg_store_new(&fixture.session, &stores[i], NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(stores[0], 4096, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(stores[1], 4096, NULL) == FSIMG_OK);
	CHECK(fsimg_store_grow(stores[2], 4096, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SCRATCH);
	for (size_t i = 0; i < 4; i++)
		fsimg_store_free(stores[i]);
	fixture_close(&fixture);
}

static void
test_transactional_eviction(void)
{
	fixture_t fixture;
	fsimg_store_t *resident, *backed;
	fsimg_stats_t before, after;
	unsigned char value;

	for (unsigned point = FSIMG_META_RESIZE; point <= FSIMG_META_WRITE; point++) {
		if (point == FSIMG_META_READ)
			continue;
		fixture_open(&fixture, 1, 1024 * 1024, 4);
		CHECK(fsimg_store_new(&fixture.session, &backed, NULL) == FSIMG_OK);
		CHECK(fsimg_store_grow(backed, 8192, NULL) == FSIMG_OK);
		CHECK(fsimg_store_new(&fixture.session, &resident, NULL) == FSIMG_OK);
		CHECK(fsimg_store_grow(resident, 1, NULL) == FSIMG_OK);
		value = 0x71;
		CHECK(fsimg_store_write(resident, 0, &value, 1, NULL) == FSIMG_OK);
		CHECK(fsimg_ctx_get_stats(fixture.ctx, &before, NULL) == FSIMG_OK);
		fsimg_test_meta_fail_after(fixture.ctx, (fsimg_meta_fault_t)point, 1, ENOSPC);
		CHECK(fsimg_store_read(backed, 0, &value, 1, NULL) == FSIMG_EIO);
		CHECK(fsimg_store_read(resident, 0, &value, 1, NULL) == FSIMG_OK);
		CHECK(value == 0x71);
		CHECK(fsimg_ctx_get_stats(fixture.ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.descriptors == before.descriptors);
		CHECK(after.scratch_bytes == before.scratch_bytes);
		CHECK(after.scratch_physical_bytes == before.scratch_physical_bytes);
		CHECK(fsimg_store_read(backed, 0, &value, 1, NULL) == FSIMG_OK);
		CHECK(value == 0);
		CHECK(fsimg_store_read(resident, 0, &value, 1, NULL) == FSIMG_OK);
		CHECK(value == 0x71);
		fsimg_store_free(backed);
		fsimg_store_free(resident);
		fixture_close(&fixture);
	}
}

static void
test_scratch_overflow(void)
{
	fixture_t fixture;
	fsimg_store_t *store;
	fsimg_error_t error;

	fixture_open(&fixture, 1, UINT64_MAX, 4);
	CHECK(fsimg_store_new(&fixture.session, &store, NULL) == FSIMG_OK);
	/* Model a reservation total near the integer limit without huge files. */
	fixture.ctx->stats.scratch_bytes = UINT64_MAX - 8191;
	CHECK(fsimg_store_grow(store, 8192, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SCRATCH);
	fixture.ctx->stats.scratch_bytes = 0;
	fsimg_store_free(store);
	fixture_close(&fixture);
}

static void
test_sort_work_limit(void)
{
	fixture_t fixture;
	fsimg_stream_t *input, *output;
	fsimg_error_t error;
	unsigned char value;

	fixture_open(&fixture, 8, 1024 * 1024, 4);
	CHECK(fsimg_stream_new(&fixture.session, 1, &input, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 100; i++) {
		value = (unsigned char)(100 - i);
		CHECK(fsimg_stream_append(input, &value, 1, NULL, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_stream_seal(input, NULL) == FSIMG_OK);
	fixture.ctx->options.max_work = fixture.session.progress.work + 1;
	CHECK(fsimg_stream_sort(input, compare_bytes, NULL, NULL, &output, &error) == FSIMG_ELIMIT);
	CHECK(output == NULL && error.resource == FSIMG_RESOURCE_WORK);
	fsimg_stream_free(input);
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--aggregate-resident") == 0) {
		test_aggregate_resident_lengths();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--scratch-overflow") == 0) {
		test_scratch_overflow();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--sort-work-limit") == 0) {
		test_sort_work_limit();
		return (0);
	}
	CHECK(argc == 1);
	test_arrays();
	test_resident();
	test_streams();
	test_sparse_and_quotas();
	test_failures();
	test_retained_dirty_page();
	test_exclusive_scratch();
	test_input_survives_spill_failure();
	test_aggregate_resident_lengths();
	test_transactional_eviction();
	test_scratch_overflow();
	test_sort_work_limit();
	return (0);
}
