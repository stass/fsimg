/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

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

/* Exclusive ownership runs (specs/design-verification-helpers.md). The
 * expected answers are written here from the design: record layout, claim
 * rules, order and overlap verdicts. No driver is involved.
 */

#define FRAME_BYTES	8u
#define RECORD_BYTES	16u
#define STORED_BYTES	(FRAME_BYTES + RECORD_BYTES)

typedef struct runs_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	 session;
	fsimg_stats_t	 baseline;
	bool		 cancel;
	char		 directory[64];
} runs_fixture_t;

static bool cancelled(void *);
static void fixture_open(runs_fixture_t *, uint32_t);
static void fixture_close(runs_fixture_t *);
static void check_resources(runs_fixture_t *);
static void expect_runs(fsimg_owner_runs_t *, const fsimg_owner_run_t *,
    size_t);
static void expect_corrupt_next(fsimg_owner_runs_t *, size_t, const char *);
static void put_record(unsigned char *, uint64_t, uint64_t);
static void test_codec_and_domains(void);
static void test_claim_rules(void);
static void test_order_and_overlap(void);
static void test_spilled_sort(void);
static void test_lifecycle(void);
static void test_scratch_corruption(void);
static void test_failures(void);

static bool
cancelled(void *argument)
{
	return (((runs_fixture_t *)argument)->cancel);
}

/* pages is the metadata cache size; one page forces every store to spill. */
static void
fixture_open(runs_fixture_t *fixture, uint32_t pages)
{
	fsimg_options_t options;
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "runs-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fd = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_bytes = 256 * 1024 * 1024;
	options.max_open_files = 8;
	options.scratch_directory_fd = fd;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_session_init(&fixture->session, fixture->ctx, "runs fixture", 0);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) ==
	    FSIMG_OK);
}

static void
fixture_close(runs_fixture_t *fixture)
{
	check_resources(fixture);
	fsimg_ctx_free(fixture->ctx);
	CHECK(rmdir(fixture->directory) == 0);
}

/* Every wrapper, stream, sorter temporary and descriptor has been released. */
static void
check_resources(runs_fixture_t *fixture)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == fixture->baseline.scratch_bytes);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
}

/* Read the expected intervals in order, then a stable end. */
static void
expect_runs(fsimg_owner_runs_t *runs, const fsimg_owner_run_t *expected,
    size_t count)
{
	fsimg_owner_run_t run;

	for (size_t i = 0; i < count; i++) {
		CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_OK);
		CHECK(run.first == expected[i].first && run.end == expected[i].end);
	}
	for (unsigned i = 0; i < 2; i++) {
		run.first = run.end = 7;
		CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_DONE);
		CHECK(run.first == 0 && run.end == 0);
	}
}

/* Publish good intervals, then require the next call to fail as corrupt,
 * expose nothing, and leave the object accepting only free.
 */
static void
expect_corrupt_next(fsimg_owner_runs_t *runs, size_t good, const char *message)
{
	fsimg_owner_run_t run;
	fsimg_error_t error;

	for (size_t i = 0; i < good; i++)
		CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_OK);
	run.first = run.end = 7;
	CHECK(fsimg_owner_runs_next(runs, &run, &error) == FSIMG_ECORRUPT);
	CHECK(run.first == 0 && run.end == 0);
	if (strstr(error.message, message) == NULL)
		fprintf(stderr, "unexpected message: %s\n", error.message);
	CHECK(strstr(error.message, message) != NULL);
	CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_ESTATE);
}

static void
put_record(unsigned char *record, uint64_t first, uint64_t count)
{
	for (unsigned i = 0; i < 8; i++) {
		record[i] = (unsigned char)(first >> (8 * i));
		record[8 + i] = (unsigned char)(count >> (8 * i));
	}
}

/* A record is the first unit and the count, each little-endian u64, in an
 * eight-byte frame. Units above 32 bits survive, domains may start at zero
 * or anywhere else, may be empty, and may end at UINT64_MAX.
 */
static void
test_codec_and_domains(void)
{
	static const uint64_t high = UINT64_C(0x0123456789abcdef);
	const fsimg_owner_run_t wide[] = {
		{ high, high + 3 }, { UINT64_MAX - 2, UINT64_MAX }
	};
	const fsimg_owner_run_t low[] = { { 0, 1 }, { 1, 9 } };
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_stream_t *stream;
	unsigned char stored[RECORD_BYTES], expected[RECORD_BYTES];
	uint64_t cursor;
	size_t length;

	fixture_open(&fixture, 16);
	CHECK(fsimg_owner_runs_new(&fixture.session, high, UINT64_MAX, &runs,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, UINT64_MAX - 2, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, high, 3, NULL) == FSIMG_OK);
	stream = fsimg_test_owner_runs_stream(runs);
	CHECK(stream->count == 2 && stream->end == 2 * STORED_BYTES);
	cursor = 0;
	CHECK(fsimg_stream_next(stream, &cursor, stored, sizeof(stored), &length,
	    NULL) == FSIMG_OK);
	put_record(expected, UINT64_MAX - 2, 2);
	CHECK(length == RECORD_BYTES && bcmp(stored, expected, length) == 0);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_runs(runs, wide, 2);
	fsimg_owner_runs_free(runs);

	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 9, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 1, 8, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 0, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_runs(runs, low, 2);
	fsimg_owner_runs_free(runs);

	/* An empty domain admits no claim but is a valid, empty inventory. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 5, 5, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_runs(runs, NULL, 0);
	fsimg_owner_runs_free(runs);
	CHECK(fsimg_owner_runs_new(&fixture.session, 5, 5, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 5, 1, NULL) == FSIMG_ECORRUPT);
	fsimg_owner_runs_free(runs);
	fixture_close(&fixture);
}

/* Claims must be nonempty and inside [first_unit, end_unit); the end is
 * proved representable before it is computed. Exactly ending and adjacent
 * claims are accepted. A rejected claim fails the object, as its inventory
 * is now incomplete.
 */
static void
test_claim_rules(void)
{
	static const struct {
		uint64_t first, count;
	} invalid[] = {
		{ 10, 0 }, { 9, 1 }, { 20, 1 }, { 21, 1 }, { 19, 2 },
		{ 10, UINT64_MAX }, { UINT64_MAX, 1 }
	};
	const fsimg_owner_run_t accepted[] = { { 10, 12 }, { 12, 20 } };
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_error_t error;

	fixture_open(&fixture, 16);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		CHECK(fsimg_owner_runs_new(&fixture.session, 10, 20, &runs,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, invalid[i].first,
		    invalid[i].count, &error) == FSIMG_ECORRUPT);
		CHECK(error.operation == FSIMG_OP_VERIFY);
		CHECK(strstr(error.message, "ownership claim") != NULL);
		CHECK(fsimg_test_owner_runs_stream(runs)->count == 0);
		CHECK(fsimg_owner_runs_append(runs, 10, 1, NULL) == FSIMG_ESTATE);
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_ESTATE);
		fsimg_owner_runs_free(runs);
	}
	CHECK(fsimg_owner_runs_new(&fixture.session, 10, 20, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 12, 8, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 10, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_runs(runs, accepted, 2);
	fsimg_owner_runs_free(runs);

	/* Reversed bounds are an invalid domain; a degenerate one is not. */
	runs = (fsimg_owner_runs_t *)&fixture;
	CHECK(fsimg_owner_runs_new(&fixture.session, 3, 2, &runs, &error) ==
	    FSIMG_EINVAL);
	CHECK(runs == NULL);
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 1, NULL, NULL) ==
	    FSIMG_EINVAL);
	fixture_close(&fixture);
}

/* Duplicate, nested, crossing and equal-start claims are all overlap; the
 * sort is stable, so equal starts keep their order and the first appended
 * is the one published before the rejection.
 */
static void
test_order_and_overlap(void)
{
	static const struct {
		uint64_t first[3], count[3];
		size_t claims, good;
	} cases[] = {
		{ { 4, 4 }, { 2, 2 }, 2, 1 },		/* identical */
		{ { 2, 4 }, { 8, 1 }, 2, 1 },		/* nested */
		{ { 6, 2 }, { 4, 5 }, 2, 1 },		/* crossing */
		{ { 9, 2, 5 }, { 2, 2, 1 }, 3, 3 },	/* disjoint */
		{ { 8, 2, 9 }, { 2, 2, 1 }, 3, 2 }	/* trailing inside */
	};
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_owner_run_t run;

	fixture_open(&fixture, 16);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fsimg_owner_runs_new(&fixture.session, 0, 16, &runs,
		    NULL) == FSIMG_OK);
		for (size_t j = 0; j < cases[i].claims; j++)
			CHECK(fsimg_owner_runs_append(runs, cases[i].first[j],
			    cases[i].count[j], NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
		if (cases[i].good == cases[i].claims) {
			for (size_t j = 0; j < cases[i].good; j++)
				CHECK(fsimg_owner_runs_next(runs, &run, NULL) ==
				    FSIMG_OK);
			CHECK(fsimg_owner_runs_next(runs, &run, NULL) ==
			    FSIMG_DONE);
		} else
			expect_corrupt_next(runs, cases[i].good,
			    "overlapping ownership claims");
		fsimg_owner_runs_free(runs);
	}

	/* Stability: of two claims starting at unit 5, the first appended is
	 * published, in either order of their lengths.
	 */
	for (unsigned order = 0; order < 2; order++) {
		CHECK(fsimg_owner_runs_new(&fixture.session, 0, 16, &runs,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 5, order == 0 ? 1 : 3,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 5, order == 0 ? 3 : 1,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_OK);
		CHECK(run.first == 5 && run.end == (order == 0 ? 6u : 8u));
		expect_corrupt_next(runs, 0, "overlapping ownership claims");
		fsimg_owner_runs_free(runs);
	}
	fixture_close(&fixture);
}

/* Enough claims to exceed one in-memory sort run, through a one-page cache:
 * the external merge orders them exactly as a resident sort does.
 */
static void
test_spilled_sort(void)
{
	const uint64_t claims = 200000;
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_owner_run_t run;
	fsimg_stats_t stats;
	uint64_t ordinal;

	for (uint32_t pages = 1; pages <= 64; pages *= 64) {
		fixture_open(&fixture, pages);
		CHECK(fsimg_owner_runs_new(&fixture.session, 0, claims * 2, &runs,
		    NULL) == FSIMG_OK);
		/* Claim i is [2i, 2i + 1) or, every third, [2i, 2i + 2); a
		 * multiplicative permutation scatters the append order.
		 */
		for (uint64_t i = 0; i < claims; i++) {
			ordinal = i * 7919 % claims;
			CHECK(fsimg_owner_runs_append(runs, ordinal * 2,
			    ordinal % 3 == 0 ? 2 : 1, NULL) == FSIMG_OK);
		}
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
		CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.scratch_peak > (uint64_t)claims * STORED_BYTES);
		for (uint64_t i = 0; i < claims; i++) {
			CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_OK);
			CHECK(run.first == i * 2);
			CHECK(run.end == i * 2 + (i % 3 == 0 ? 2 : 1));
		}
		CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_DONE);
		fsimg_owner_runs_free(runs);
		fixture_close(&fixture);
	}
}

/* Collecting, reading and exhausted are one forward lifecycle; calls out of
 * order fail with FSIMG_ESTATE and change nothing, and free accepts every
 * state, including NULL.
 */
static void
test_lifecycle(void)
{
	const fsimg_owner_run_t last[] = { { 3, 8 } };
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_owner_run_t run;

	fixture_open(&fixture, 16);
	fsimg_owner_runs_free(NULL);

	/* Free while collecting, with claims appended. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 8, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 3, 5, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_owner_runs_append(runs, 0, 1, NULL) == FSIMG_OK);
	fsimg_owner_runs_free(runs);
	check_resources(&fixture);

	/* One interval ending at the domain end; free while reading. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 8, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 3, 5, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_owner_runs_append(runs, 0, 1, NULL) == FSIMG_ESTATE);
	expect_runs(runs, last, 1);
	CHECK(fsimg_owner_runs_append(runs, 0, 1, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_ESTATE);
	fsimg_owner_runs_free(runs);

	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 8, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 3, 5, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	fsimg_owner_runs_free(runs);

	/* Empty input ends at once and stays ended. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 8, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_runs(runs, NULL, 0);
	fsimg_owner_runs_free(runs);

	/* Missing arguments. */
	CHECK(fsimg_owner_runs_append(NULL, 0, 1, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_owner_runs_seal(NULL, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_owner_runs_next(NULL, &run, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_owner_runs_new(NULL, 0, 1, &runs, NULL) == FSIMG_EINVAL);
	fixture_close(&fixture);
}

/* Scratch is untrusted. Short records reach the comparator without being
 * decoded and fail at the cursor as scratch damage; valid-length records
 * whose fields changed after sorting fail the claim or order checks before
 * anything is published; damaged framing fails in the stream layer.
 */
static void
test_scratch_corruption(void)
{
	unsigned char record[RECORD_BYTES], frame[FRAME_BYTES];
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_stream_t *stream;
	fsimg_error_t error;
	fsimg_owner_run_t run;

	fixture_open(&fixture, 16);
	for (size_t length = 0; length < RECORD_BYTES; length++) {
		CHECK(fsimg_owner_runs_new(&fixture.session, 0, 64, &runs,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 8, 2, NULL) == FSIMG_OK);
		memset(record, 0xff, sizeof(record));
		CHECK(fsimg_stream_append(fsimg_test_owner_runs_stream(runs),
		    record, length, NULL, NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 2, 2, NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
		/* Shorter records order first: the cursor meets one at once. */
		CHECK(fsimg_owner_runs_next(runs, &run, &error) == FSIMG_ECORRUPT);
		CHECK(error.operation == FSIMG_OP_METADATA);
		CHECK(strstr(error.message, "scratch") != NULL);
		CHECK(run.first == 0 && run.end == 0);
		fsimg_owner_runs_free(runs);
	}

	/* Damage written into the sorted records: { 2, 2 } then { 8, 2 }. */
	for (unsigned damage = 0; damage < 4; damage++) {
		CHECK(fsimg_owner_runs_new(&fixture.session, 0, 64, &runs,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 8, 2, NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_append(runs, 2, 2, NULL) == FSIMG_OK);
		CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
		stream = fsimg_test_owner_runs_stream(runs);
		switch (damage) {
		case 0:	/* second record now starts inside the first */
			put_record(record, 3, 2);
			break;
		case 1:	/* second record now starts before the first */
			put_record(record, 1, 2);
			break;
		case 2:	/* second record now ends past the domain */
			put_record(record, 8, 57);
			break;
		default: /* second record is now empty */
			put_record(record, 8, 0);
			break;
		}
		CHECK(fsimg_store_write(stream->store, STORED_BYTES + FRAME_BYTES,
		    record, sizeof(record), NULL) == FSIMG_OK);
		expect_corrupt_next(runs, 1, damage < 2 ?
		    "overlapping ownership claims" : "scratch");
		fsimg_owner_runs_free(runs);
	}

	/* Damage written before sorting: { 8, 2 } becomes empty. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 64, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 8, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 2, 2, NULL) == FSIMG_OK);
	put_record(record, 8, 0);
	CHECK(fsimg_store_write(fsimg_test_owner_runs_stream(runs)->store,
	    FRAME_BYTES, record, sizeof(record), NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	expect_corrupt_next(runs, 1, "scratch");
	fsimg_owner_runs_free(runs);

	/* A frame claiming more than the record maximum. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, 64, &runs, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_owner_runs_append(runs, 2, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_OK);
	fsimg_meta_put_u64(frame, (uint64_t)(RECORD_BYTES + 1) << 32 | 1);
	CHECK(fsimg_store_write(fsimg_test_owner_runs_stream(runs)->store, 0,
	    frame, sizeof(frame), NULL) == FSIMG_OK);
	expect_corrupt_next(runs, 0, "frame");
	fsimg_owner_runs_free(runs);
	fixture_close(&fixture);
}

/* Allocation, scratch, cancellation and work failures during construction,
 * collection, sorting and reading leave nothing published and no resource
 * held once the object is freed.
 */
static void
test_failures(void)
{
	const uint64_t claims = 3000;
	runs_fixture_t fixture;
	fsimg_owner_runs_t *runs;
	fsimg_owner_run_t run;
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned failures;
	bool reached_success;

	fixture_open(&fixture, 1);

	/* Allocation failure at every point of a complete lifecycle. */
	reached_success = false;
	for (uint64_t attempt = 1; !reached_success && attempt <= 256; attempt++) {
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_owner_runs_new(&fixture.session, 0, claims * 2,
		    &runs, &error);
		for (uint64_t i = 0; status == FSIMG_OK && i < claims; i++)
			status = fsimg_owner_runs_append(runs, (claims - 1 - i) * 2,
			    1, &error);
		if (status == FSIMG_OK)
			status = fsimg_owner_runs_seal(runs, &error);
		while (status == FSIMG_OK)
			status = fsimg_owner_runs_next(runs, &run, &error);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_DONE)
			reached_success = true;
		else {
			CHECK(status == FSIMG_ENOMEM && error.status == status);
			if (runs != NULL)
				CHECK(fsimg_owner_runs_next(runs, &run, NULL) ==
				    FSIMG_ESTATE);
		}
		fsimg_owner_runs_free(runs);
		check_resources(&fixture);
	}
	CHECK(reached_success);

	/* Scratch faults at a range of operations, in every phase. */
	for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE; point++) {
		failures = 0;
		for (uint64_t nth = 1; nth <= 40; nth += 13) {
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)point, nth, ENOSPC);
			status = fsimg_owner_runs_new(&fixture.session, 0,
			    claims * 2, &runs, &error);
			for (uint64_t i = 0; status == FSIMG_OK && i < claims; i++)
				status = fsimg_owner_runs_append(runs,
				    (claims - 1 - i) * 2, 1, &error);
			if (status == FSIMG_OK)
				status = fsimg_owner_runs_seal(runs, &error);
			while (status == FSIMG_OK)
				status = fsimg_owner_runs_next(runs, &run, &error);
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)point, 0, 0);
			if (status != FSIMG_DONE) {
				failures++;
				CHECK(status == FSIMG_EIO && error.os_error == ENOSPC);
				if (runs != NULL)
					CHECK(fsimg_owner_runs_next(runs, &run,
					    NULL) == FSIMG_ESTATE);
			}
			fsimg_owner_runs_free(runs);
			check_resources(&fixture);
		}
		CHECK(failures > 0);
	}

	/* Cancellation during the sort fails the seal. */
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, claims * 2, &runs,
	    NULL) == FSIMG_OK);
	for (uint64_t i = 0; i < claims; i++)
		CHECK(fsimg_owner_runs_append(runs, (claims - 1 - i) * 2, 1,
		    NULL) == FSIMG_OK);
	fixture.cancel = true;
	CHECK(fsimg_owner_runs_seal(runs, &error) == FSIMG_ECANCELLED);
	fixture.cancel = false;
	CHECK(fsimg_owner_runs_next(runs, &run, NULL) == FSIMG_ESTATE);
	fsimg_owner_runs_free(runs);
	check_resources(&fixture);

	/* A work limit reached while collecting fails the append. */
	fixture.ctx->options.max_work = fixture.session.progress.work + 4;
	CHECK(fsimg_owner_runs_new(&fixture.session, 0, claims * 2, &runs,
	    NULL) == FSIMG_OK);
	status = FSIMG_OK;
	for (uint64_t i = 0; status == FSIMG_OK && i < claims; i++)
		status = fsimg_owner_runs_append(runs, (claims - 1 - i) * 2, 1,
		    &error);
	CHECK(status == FSIMG_ELIMIT && error.resource == FSIMG_RESOURCE_WORK);
	CHECK(fsimg_owner_runs_seal(runs, NULL) == FSIMG_ESTATE);
	fsimg_owner_runs_free(runs);
	fixture.ctx->options.max_work = 0;
	fixture_close(&fixture);
}

int
main(int argc, char **argv FSIMG_UNUSED)
{
	CHECK(argc == 1);
	test_codec_and_domains();
	test_claim_rules();
	test_order_and_overlap();
	test_spilled_sort();
	test_lifecycle();
	test_scratch_corruption();
	test_failures();
	return (0);
}
