/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "codec.h"
#include "internal.h"
#include "lzfse/lzfse_internal.h"
#include "lzfse/lzvn_decode_base.h"

/*
 * The adapter over the imported LZFSE and LZVN decoders: the one file that
 * includes them. It calls their state machines directly rather than the
 * reference's buffer interface, which reports a full output like a complete
 * one, and turns what the machines leave behind into the codec contract.
 *
 * Both machines are entered once per stream and never resumed, so a stop for
 * want of input or output is final. Each stays inside the [in, in + in_len) and
 * [out, out + capacity) it is given: their wide copies run only with the margin
 * to the end checked first, and the imported files' recorded adaptations close
 * the paths that did not.
 *
 * A block's header does not bind what it decodes. The LZFSE machine ignores a
 * compressed block's n_raw_bytes, and bounds an L by the literal buffer rather
 * than by the block's n_literals, so an L past them copies bytes the block did
 * not decode. Those are this stream's own earlier literals or zeroes, because
 * the state starts zeroed per stream, so no other data can reach the output. The
 * output check still holds: the machine stops at capacity whatever a header
 * declares, and a caller compares step->produced to the length it expected.
 *
 * The imported loads are native-endian, and a big-endian LZVN distance would be
 * swapped into a decode that succeeds with wrong bytes, so these decoders refuse
 * such a host rather than guess.
 */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LZFSE_HOST_SUPPORTED	true
#else
#define LZFSE_HOST_SUPPORTED	false
#endif

/* The literal copy loads eight bytes at a time and an L is bounded by the buffer's
 * end, so a load can pass that end by seven bytes; it stays inside the state.
 */
_Static_assert(offsetof(lzfse_decoder_state,
    compressed_lzfse_block_state.literals) + LZFSE_LITERALS_PER_BLOCK + 64 + 7 <=
    sizeof(lzfse_decoder_state), "literal over-read leaves the decoder state");

static fsimg_status_t lzfse_fail(const uint8_t *, const uint8_t *,
    fsimg_status_t, const char *, fsimg_error_t *);
static bool lzfse_arguments_valid(const uint8_t *, size_t, const uint8_t *,
    size_t);

/* A failure at stream position `reached`, the decoder's last good position. */
static fsimg_status_t
lzfse_fail(const uint8_t *in, const uint8_t *reached, fsimg_status_t status,
    const char *check, fsimg_error_t *error)
{
	fsimg_error_set(error, status, 0, FSIMG_OP_READER, check);
	if (error != NULL) {
		error->offset = (uint64_t)(reached - in);
		error->offset_valid = true;
	}
	return (status);
}

static bool
lzfse_arguments_valid(const uint8_t *in, size_t in_len, const uint8_t *out,
    size_t capacity)
{
	return (in != NULL && out != NULL && in_len <= FSIMG_LZFSE_INPUT_MAX &&
	    capacity <= FSIMG_LZFSE_OUTPUT_MAX);
}

size_t
fsimg_lzfse_scratch_bytes(void)
{
	return (sizeof(lzfse_decoder_state));
}

fsimg_status_t
fsimg_lzfse_decode(const uint8_t *in, size_t in_len, uint8_t *out,
    size_t capacity, void *scratch, fsimg_codec_step_t *step,
    fsimg_error_t *error)
{
	lzfse_decoder_state *state = scratch;
	int code;

	bzero(step, sizeof(*step));
	if (!lzfse_arguments_valid(in, in_len, out, capacity) || scratch == NULL)
		return (lzfse_fail(in, in, FSIMG_EINVAL,
		    "lzfse: invalid buffer arguments", error));
	if (!LZFSE_HOST_SUPPORTED)
		return (lzfse_fail(in, in, FSIMG_EUNSUPPORTED,
		    "lzfse: decoder needs a little-endian host", error));

	/* Zeroed scratch is the machine's start state, and what a block's
	 * unwritten decoder tables and literals read as, so a stream sees nothing
	 * of an earlier one.
	 */
	bzero(state, sizeof(*state));
	state->src = in;
	state->src_begin = in;
	state->src_end = in + in_len;
	state->dst = out;
	state->dst_begin = out;
	state->dst_end = out + capacity;

	code = fsimg_apple_lzfse_decode(state);
	step->consumed = (size_t)(state->src - in);
	step->produced = (size_t)(state->dst - out);
	switch (code) {
	case LZFSE_STATUS_OK:
		break;
	case LZFSE_STATUS_DST_FULL:
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: stream exceeds its output bound or an LZVN block stops early",
		    error));
	case LZFSE_STATUS_SRC_EMPTY:
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: stream is truncated", error));
	default:
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: invalid block", error));
	}
	if (!state->end_of_stream)
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: stream has no end block", error));
	if (state->src != state->src_end)
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: trailing bytes after the end block", error));
	step->done = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_lzvn_decode(const uint8_t *in, size_t in_len, uint8_t *out,
    size_t capacity, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	lzvn_decoder_state state;
	uint8_t spare;

	bzero(step, sizeof(*step));
	if (!lzfse_arguments_valid(in, in_len, out, capacity))
		return (lzfse_fail(in, in, FSIMG_EINVAL,
		    "lzvn: invalid buffer arguments", error));
	if (!LZFSE_HOST_SUPPORTED)
		return (lzfse_fail(in, in, FSIMG_EUNSUPPORTED,
		    "lzvn: decoder needs a little-endian host", error));

	/* The machine returns at once on an empty output, so an empty capacity
	 * gets one spare byte: an end marker still decodes there, and any byte
	 * written is output past the capacity.
	 */
	if (capacity == 0)
		out = &spare;
	bzero(&state, sizeof(state));
	state.src = in;
	state.src_end = in + in_len;
	state.dst = out;
	state.dst_begin = out;
	state.dst_end = out + (capacity == 0 ? sizeof(spare) : capacity);

	fsimg_apple_lzvn_decode(&state);
	step->consumed = (size_t)(state.src - in);
	step->produced = (size_t)(state.dst - out);
	if (step->produced > capacity) {
		step->produced = capacity;
		return (lzfse_fail(in, state.src, FSIMG_ECORRUPT,
		    "lzvn: stream does not end within its output bound", error));
	}
	if (!state.end_of_stream)
		return (lzfse_fail(in, state.src, FSIMG_ECORRUPT,
		    "lzvn: stream does not reach its end marker", error));
	if (state.src != state.src_end)
		return (lzfse_fail(in, state.src, FSIMG_ECORRUPT,
		    "lzvn: trailing bytes after the end marker", error));
	step->done = true;
	return (FSIMG_OK);
}
