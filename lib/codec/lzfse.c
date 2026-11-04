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
 */

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
		    "lzfse: stream does not end within its output bound", error));
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
	step->done = true;
	if (state->src != state->src_end)
		return (lzfse_fail(in, state->src, FSIMG_ECORRUPT,
		    "lzfse: trailing bytes after the end block", error));
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
	step->done = true;
	if (state.src != state.src_end)
		return (lzfse_fail(in, state.src, FSIMG_ECORRUPT,
		    "lzvn: trailing bytes after the end marker", error));
	return (FSIMG_OK);
}
