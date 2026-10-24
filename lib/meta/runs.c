/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <strings.h>

#include "host.h"
#include "meta.h"

/* Exclusive ownership runs (specs/design-verification-helpers.md). A record
 * is a claim on the allocation units [first, first + count): the first unit
 * and the positive count, each a little-endian u64, in one stream frame.
 * Units are allocation-unit numbers, never byte offsets.
 */
#define RUN_RECORD_BYTES 16u

/* One forward lifecycle. Collection appends claims; sealing sorts them and
 * starts reading; the cursor ends exhausted. Any operational failure leaves
 * the object failed, which only free accepts.
 */
typedef enum owner_runs_state {
	OWNER_RUNS_COLLECTING,
	OWNER_RUNS_READING,
	OWNER_RUNS_EXHAUSTED,
	OWNER_RUNS_FAILED
} owner_runs_state_t;

/* The stream is the unsorted claims while collecting and the sorted ones
 * afterwards; it is the only store the object owns. stream_offset is the
 * byte offset of the next sorted frame. previous_end is the end unit of the
 * last interval next published, meaningful once have_previous is set.
 */
struct fsimg_owner_runs {
	fsimg_session_t	*session;
	fsimg_stream_t	*stream;
	uint64_t	 first_unit;
	uint64_t	 end_unit;
	uint64_t	 stream_offset;
	uint64_t	 previous_end;
	owner_runs_state_t state;
	bool		 have_previous;
};

static fsimg_status_t runs_state_error(fsimg_error_t *, const char *);
static fsimg_status_t claim_check(const fsimg_owner_runs_t *, uint64_t,
    uint64_t, fsimg_error_t *);
static int run_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t runs_fail(fsimg_owner_runs_t *, fsimg_status_t);

static fsimg_status_t
runs_state_error(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_VERIFY,
	    message));
}

/* A claim must name at least one unit and lie inside the domain. first is
 * tested first, so the subtraction cannot wrap and the end fits.
 */
static fsimg_status_t
claim_check(const fsimg_owner_runs_t *runs, uint64_t first, uint64_t count,
    fsimg_error_t *error)
{
	char message[128];

	if (count == 0) {
		snprintf(message, sizeof(message),
		    "empty ownership claim at unit %" PRIu64, first);
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_VERIFY, message));
	}
	if (first < runs->first_unit || first >= runs->end_unit ||
	    count > runs->end_unit - first) {
		snprintf(message, sizeof(message),
		    "ownership claim of %" PRIu64 " units at unit %" PRIu64
		    " is outside its allocation domain", count, first);
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_VERIFY, message));
	}
	return (FSIMG_OK);
}

/* Order by first unit. The comparator cannot report an error, so it tests
 * both lengths before loading a field: malformed records order by length
 * alone, deterministically, and the cursor rejects them later. Equal starts
 * compare equal; the stable sort keeps them, never merging duplicates.
 */
static int
run_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint64_t first, second;

	if (alen != RUN_RECORD_BYTES || blen != RUN_RECORD_BYTES)
		return ((alen > blen) - (alen < blen));
	first = fsimg_meta_get_u64(a);
	second = fsimg_meta_get_u64(b);
	return ((first > second) - (first < second));
}

static fsimg_status_t
runs_fail(fsimg_owner_runs_t *runs, fsimg_status_t status)
{
	runs->state = OWNER_RUNS_FAILED;
	return (status);
}

fsimg_status_t
fsimg_owner_runs_new(fsimg_session_t *session, uint64_t first_unit,
    uint64_t end_unit, fsimg_owner_runs_t **out, fsimg_error_t *error)
{
	fsimg_owner_runs_t *runs;
	fsimg_error_t detail;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	if (session == NULL || out == NULL || first_unit > end_unit)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_VERIFY,
		    "invalid ownership-run domain"));
	runs = fsimg_alloc(session->ctx, sizeof(*runs), &detail);
	if (runs == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(runs, sizeof(*runs));
	runs->session = session;
	runs->first_unit = first_unit;
	runs->end_unit = end_unit;
	runs->state = OWNER_RUNS_COLLECTING;
	status = fsimg_stream_new(session, RUN_RECORD_BYTES, &runs->stream,
	    error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(runs);
		return (status);
	}
	*out = runs;
	return (FSIMG_OK);
}

void
fsimg_owner_runs_free(fsimg_owner_runs_t *runs)
{
	if (runs == NULL)
		return;
	fsimg_stream_free(runs->stream);
	fsimg_dealloc(runs);
}

/* Record one claim. Claims arrive in any order and are never coalesced: a
 * merged pair could hide a repeated claim. An invalid claim or a failed
 * append leaves the inventory incomplete, so the object fails.
 */
fsimg_status_t
fsimg_owner_runs_append(fsimg_owner_runs_t *runs, uint64_t first,
    uint64_t count, fsimg_error_t *error)
{
	unsigned char record[RUN_RECORD_BYTES];
	fsimg_status_t status;

	if (runs == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_VERIFY,
		    "missing ownership runs"));
	if (runs->state != OWNER_RUNS_COLLECTING)
		return (runs_state_error(error,
		    "ownership claim after collection ended"));
	status = claim_check(runs, first, count, error);
	if (status != FSIMG_OK)
		return (runs_fail(runs, status));
	fsimg_meta_put_u64(record, first);
	fsimg_meta_put_u64(record + 8, count);
	status = fsimg_stream_append(runs->stream, record, sizeof(record), NULL,
	    error);
	if (status != FSIMG_OK)
		return (runs_fail(runs, status));
	return (FSIMG_OK);
}

/* End collection and sort the claims by first unit with the bounded
 * external sort. The sorted stream replaces the unsorted one only once
 * complete; on failure the object keeps the original, only to free it.
 * Success proves that sorting finished, not that any record is valid.
 */
fsimg_status_t
fsimg_owner_runs_seal(fsimg_owner_runs_t *runs, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	fsimg_status_t status;

	if (runs == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_VERIFY,
		    "missing ownership runs"));
	if (runs->state != OWNER_RUNS_COLLECTING)
		return (runs_state_error(error, "ownership runs are not collecting"));
	status = fsimg_stream_seal(runs->stream, error);
	if (status != FSIMG_OK)
		return (runs_fail(runs, status));
	status = fsimg_stream_sort(runs->stream, run_compare, NULL, NULL,
	    &sorted, error);
	if (status != FSIMG_OK)
		return (runs_fail(runs, status));
	fsimg_stream_free(runs->stream);
	runs->stream = sorted;
	runs->stream_offset = 0;
	runs->state = OWNER_RUNS_READING;
	return (FSIMG_OK);
}

/* Publish the next sorted interval, or FSIMG_DONE after the last. Scratch is
 * untrusted, so each record is checked again: its length before any field
 * is loaded, then the claim rules. A record starting before the previous
 * interval's end overlaps it; with nonempty intervals in sorted order that
 * one comparison rejects duplicates, nesting and crossing alike. Adjacent
 * intervals are legal. The cursor and previous end advance only after every
 * check passes.
 */
fsimg_status_t
fsimg_owner_runs_next(fsimg_owner_runs_t *runs, fsimg_owner_run_t *out,
    fsimg_error_t *error)
{
	unsigned char record[RUN_RECORD_BYTES];
	char message[128];
	uint64_t stream_offset, first, count;
	size_t length;
	fsimg_status_t status;

	if (out != NULL)
		bzero(out, sizeof(*out));
	if (runs == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_VERIFY,
		    "missing ownership-run argument"));
	if (runs->state == OWNER_RUNS_EXHAUSTED)
		return (FSIMG_DONE);
	if (runs->state != OWNER_RUNS_READING)
		return (runs_state_error(error,
		    "ownership runs are not ready for reading"));

	stream_offset = runs->stream_offset;
	status = fsimg_stream_next(runs->stream, &stream_offset, record,
	    sizeof(record), &length, error);
	if (status == FSIMG_DONE) {
		runs->state = OWNER_RUNS_EXHAUSTED;
		return (FSIMG_DONE);
	}
	if (status != FSIMG_OK)
		return (runs_fail(runs, status));
	if (length != RUN_RECORD_BYTES)
		return (runs_fail(runs, fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "malformed ownership scratch record")));
	/* Append admitted only valid claims, so an invalid one here is
	 * damaged scratch. An overlap may come from the image or scratch.
	 */
	first = fsimg_meta_get_u64(record);
	count = fsimg_meta_get_u64(record + 8);
	if (claim_check(runs, first, count, NULL) != FSIMG_OK)
		return (runs_fail(runs, fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "invalid ownership scratch record")));
	if (runs->have_previous && first < runs->previous_end) {
		snprintf(message, sizeof(message),
		    "overlapping ownership claims: unit %" PRIu64
		    " is inside a claim ending before unit %" PRIu64,
		    first, runs->previous_end);
		return (runs_fail(runs, fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_VERIFY, message)));
	}

	out->first = first;
	out->end = first + count;
	runs->stream_offset = stream_offset;
	runs->previous_end = out->end;
	runs->have_previous = true;
	return (FSIMG_OK);
}

fsimg_stream_t *
fsimg_test_owner_runs_stream(const fsimg_owner_runs_t *runs)
{
	return (runs->stream);
}
