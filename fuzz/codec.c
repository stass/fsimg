/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "fsimg.h"

/*
 * The LZO1X and NRV2B decoders on arbitrary streams. The input's first
 * four bytes choose the decode and the rest is the stream:
 *
 *   byte 0     bit 0 the codec (0 LZO1X, 1 NRV2B); bit 1 adds one to the
 *              NRV2B capacity, so 65536 is reachable
 *   bytes 1-2  little-endian v: the LZO1X limit is 16v, the NRV2B
 *              capacity v
 *   byte 3     the LZO1X call plan: the low nibble n sets the input piece
 *              to 1 + 17n^2 bytes (1 to 3826, from single bytes to whole
 *              chunks), the high nibble c the window capacity to 2^(c mod
 *              13) bytes (1 to 4096)
 *
 * test-codec --seed writes seeds in this format.
 *
 * An LZO1X stream is decoded in one call into a window the size of its
 * limit, then again in pieces through a small window that keeps 49151
 * bytes of history, and the two must agree: the same verdict at the same
 * stream offset and, on success, the same bytes; on failure the shorter
 * output is a prefix of the longer. Every call must make progress or stop
 * at an edge, consume nothing after the end marker, and leave every byte
 * beyond what it reports producing as the poison it was given; under
 * AddressSanitizer every buffer is exactly its size, so no byte outside it
 * is touched. An NRV2B stream accepted under its capacity must decode to
 * the same bytes under the largest capacity.
 */
#define FUZZ_HEADER_BYTES	4
#define POISON			0xa5u
#define LZO1X_HISTORY		49151u
#define LZO1X_LIMIT_SCALE	16u
#define NRV2B_MAX_INPUT		65535u
#define NRV2B_MAX_CAPACITY	65536u

/* The outcome of one decode, summed over its calls. */
typedef struct fuzz_result {
	fsimg_error_t	 error;
	uint8_t		*output;
	size_t		 produced;
	fsimg_status_t	 status;
} fuzz_result_t;

static bool poison_intact(const uint8_t *, size_t);
static void lzo_one_call(const uint8_t *, size_t, uint64_t, fuzz_result_t *);
static void lzo_in_pieces(const uint8_t *, size_t, uint64_t, uint8_t,
    fuzz_result_t *);
static void exercise_lzo(const uint8_t *, size_t, uint64_t, uint8_t);
static void exercise_nrv2b(const uint8_t *, size_t, size_t);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static bool
poison_intact(const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (bytes[i] != POISON)
			return (false);
	return (true);
}

static void *
fuzz_alloc(size_t size)
{
	void *p;

	p = malloc(size == 0 ? 1 : size);
	if (p == NULL)
		abort();
	return (p);
}

/*
 * One call, the whole stream final, a window of the limit: it must end in
 * a verdict or the end marker with all input consumed.
 */
static void
lzo_one_call(const uint8_t *in, size_t length, uint64_t limit,
    fuzz_result_t *result)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;

	result->output = fuzz_alloc((size_t)limit);
	memset(result->output, POISON, (size_t)limit);
	fsimg_lzo1x_decoder_init(&decoder, limit);
	result->status = fsimg_lzo1x_decode(&decoder, in, length, true,
	    result->output, 0, (size_t)limit, &step, &result->error);
	result->produced = step.produced;
	if (step.consumed > length || step.produced > limit)
		abort();
	if (!poison_intact(result->output + step.produced,
	    (size_t)limit - step.produced))
		abort();
	if (result->status == FSIMG_OK &&
	    (!step.done || step.consumed != length))
		abort();
	if (result->status == FSIMG_ESTATE || result->status == FSIMG_EINVAL)
		abort();
}

/*
 * The same stream in pieces through a small window, as a container that
 * drains its window to the spool and keeps the history a match may need.
 */
static void
lzo_in_pieces(const uint8_t *in, size_t length, uint64_t limit, uint8_t plan,
    fuzz_result_t *result)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	uint8_t *window;
	size_t capacity, history, piece, piece_length, position;
	bool done;

	piece = 1 + (size_t)(plan & 15) * (plan & 15) * 17;
	capacity = (size_t)1 << ((plan >> 4) % 13);
	window = fuzz_alloc(LZO1X_HISTORY + capacity);
	result->output = fuzz_alloc((size_t)limit);
	result->produced = 0;
	fsimg_lzo1x_decoder_init(&decoder, limit);
	history = 0;
	position = 0;
	done = false;
	for (;;) {
		piece_length = length - position;
		if (piece_length > piece)
			piece_length = piece;
		memset(window + history, POISON, capacity);
		result->status = fsimg_lzo1x_decode(&decoder, in + position,
		    piece_length, position + piece_length == length, window,
		    history, capacity, &step, &result->error);
		if (step.consumed > piece_length || step.produced > capacity)
			abort();
		if (!poison_intact(window + history + step.produced,
		    capacity - step.produced))
			abort();
		/* After the end marker nothing more is accepted. */
		if (done && (step.consumed != 0 || step.produced != 0 ||
		    result->status == FSIMG_OK))
			abort();
		if (result->produced + step.produced > limit)
			abort();
		memcpy(result->output + result->produced, window + history,
		    step.produced);
		result->produced += step.produced;
		position += step.consumed;
		done = step.done;
		if (result->status != FSIMG_OK)
			break;
		if (done && position == length)
			break;
		if (!done && step.consumed == 0 && step.produced == 0)
			abort();
		history += step.produced;
		if (history > LZO1X_HISTORY) {
			memmove(window, window + (history - LZO1X_HISTORY),
			    LZO1X_HISTORY);
			history = LZO1X_HISTORY;
		}
	}
	free(window);
}

static void
exercise_lzo(const uint8_t *in, size_t length, uint64_t limit, uint8_t plan)
{
	fuzz_result_t whole, pieces;
	size_t shorter;

	lzo_one_call(in, length, limit, &whole);
	lzo_in_pieces(in, length, limit, plan, &pieces);
	if (whole.status != pieces.status)
		abort();
	if (whole.status != FSIMG_OK &&
	    whole.error.offset != pieces.error.offset)
		abort();
	if (whole.status == FSIMG_OK && whole.produced != pieces.produced)
		abort();
	shorter = whole.produced < pieces.produced ? whole.produced :
	    pieces.produced;
	if (memcmp(whole.output, pieces.output, shorter) != 0)
		abort();
	free(whole.output);
	free(pieces.output);
}

static void
exercise_nrv2b(const uint8_t *in, size_t length, size_t capacity)
{
	fsimg_codec_step_t step, largest_step;
	fsimg_error_t error;
	fsimg_status_t status;
	uint8_t *out, *largest;

	if (length > NRV2B_MAX_INPUT)
		return;
	out = fuzz_alloc(capacity);
	memset(out, POISON, capacity);
	status = fsimg_nrv2b_decode(in, length, out, capacity, &step, &error);
	if (step.consumed > length || step.produced > capacity)
		abort();
	if (!poison_intact(out + step.produced, capacity - step.produced))
		abort();
	if (status == FSIMG_OK && (!step.done || step.consumed != length))
		abort();
	if (status == FSIMG_ESTATE || status == FSIMG_EINVAL)
		abort();

	/* More room never changes an accepted stream's bytes. */
	if (status == FSIMG_OK && capacity < NRV2B_MAX_CAPACITY) {
		largest = fuzz_alloc(NRV2B_MAX_CAPACITY);
		if (fsimg_nrv2b_decode(in, length, largest, NRV2B_MAX_CAPACITY,
		    &largest_step, &error) != FSIMG_OK)
			abort();
		if (largest_step.produced != step.produced ||
		    memcmp(largest, out, step.produced) != 0)
			abort();
		free(largest);
	}
	free(out);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint32_t bound;

	if (size < FUZZ_HEADER_BYTES)
		return (0);
	bound = (uint32_t)data[1] | (uint32_t)data[2] << 8;
	if ((data[0] & 1) == 0)
		exercise_lzo(data + FUZZ_HEADER_BYTES, size - FUZZ_HEADER_BYTES,
		    (uint64_t)bound * LZO1X_LIMIT_SCALE, data[3]);
	else
		exercise_nrv2b(data + FUZZ_HEADER_BYTES,
		    size - FUZZ_HEADER_BYTES, bound + ((data[0] >> 1) & 1));
	return (0);
}
