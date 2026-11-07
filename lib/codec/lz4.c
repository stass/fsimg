/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "codec.h"
#include "internal.h"

/*
 * The LZ4 block decoder.
 *
 * A block is a run of sequences. Each starts with a token whose high four
 * bits give the literal length and low four bits the match length less
 * the minimum match of 4; a field of 15 continues in extension bytes, each
 * adding its value, until one below 255. The literals follow, then a
 * two-byte little-endian offset and the match length's extension. The last
 * sequence stops after its literals, so the block ends exactly where the
 * input ends directly after a sequence's literals.
 *
 * One call decodes one block: the whole of it in full mode, or its first
 * bytes up to a requested length in partial mode. Every length is held
 * against the output room before each addition, so it cannot overflow, and
 * every copy count against both buffers before its first byte moves; bytes
 * are then copied one at a time, a match front to back so that an offset
 * below its length repeats the bytes just written. Nothing is read past the
 * input or written past out[produced], and every sequence consumes at least
 * its token, so the work is proportional to the bytes consumed and produced.
 */
#define LZ4_FIELD_BITS		4
#define LZ4_FIELD_MASK		0x0f
#define LZ4_FIELD_EXTENDED	15	/* the length continues in extensions */
#define LZ4_EXTENSION_MORE	255	/* another extension byte follows */
#define LZ4_MIN_MATCH		4
#define LZ4_OFFSET_BYTES	2	/* little-endian, low byte first */

/* What the output bound means. */
typedef enum lz4_mode {
	LZ4_FULL,	/* passing it is corrupt; the block ends at input end */
	LZ4_PARTIAL	/* reaching it ends the decode, wherever it falls */
} lz4_mode_t;

/* One decode: the block, the output and how far the decoder is in each. */
typedef struct lz4_block {
	const uint8_t	*in;
	size_t		 in_len;
	size_t		 consumed;
	uint8_t		*out;
	size_t		 bound;		/* capacity or requested length */
	size_t		 produced;
	lz4_mode_t	 mode;
} lz4_block_t;

static fsimg_status_t lz4_fail(const lz4_block_t *, fsimg_status_t,
    const char *, fsimg_error_t *);
static fsimg_status_t lz4_read_length(lz4_block_t *, unsigned, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t lz4_read_offset(lz4_block_t *, size_t *,
    fsimg_error_t *);
static fsimg_status_t lz4_decode_sequences(lz4_block_t *, fsimg_error_t *);
static fsimg_status_t lz4_decode(lz4_mode_t, const uint8_t *, size_t,
    uint8_t *, size_t, fsimg_codec_step_t *, fsimg_error_t *);

static fsimg_status_t
lz4_fail(const lz4_block_t *block, fsimg_status_t status, const char *check,
    fsimg_error_t *error)
{
	fsimg_error_set(error, status, 0, FSIMG_OP_READER, check);
	if (error != NULL) {
		error->offset = block->consumed;
		error->offset_valid = true;
	}
	return (status);
}

/*
 * Read a length: the token's field plus base (0 for literals, the minimum
 * match for a match), and its extension bytes when the field is 15. The
 * result is the number of bytes to copy. It is held against the output
 * room before every addition: in full mode passing the room is corrupt at
 * once; in partial mode the length is clipped to the room, and the rest of
 * the extension is still read, since a literal run's literals follow it.
 * Once clipped, length equals room, so every later byte above 0 keeps it
 * clipped.
 */
static fsimg_status_t
lz4_read_length(lz4_block_t *block, unsigned field, size_t base,
    size_t *count, fsimg_error_t *error)
{
	size_t length, room;
	uint8_t byte;

	room = block->bound - block->produced;
	length = base + field;
	if (length > room) {
		if (block->mode == LZ4_FULL)
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: output longer than its capacity", error));
		length = room;
	}
	if (field == LZ4_FIELD_EXTENDED) {
		do {
			if (block->consumed == block->in_len)
				return (lz4_fail(block, FSIMG_ECORRUPT,
				    "lz4: unterminated stream", error));
			byte = block->in[block->consumed++];
			if (byte > room - length) {
				if (block->mode == LZ4_FULL)
					return (lz4_fail(block, FSIMG_ECORRUPT,
					    "lz4: output longer than its capacity",
					    error));
				length = room;
			} else {
				length += byte;
			}
		} while (byte == LZ4_EXTENSION_MORE);
	}
	*count = length;
	return (FSIMG_OK);
}

/*
 * Read a match offset, low byte first, and check it: zero is invalid, and
 * one greater than the bytes produced reaches before the output start.
 */
static fsimg_status_t
lz4_read_offset(lz4_block_t *block, size_t *offset, fsimg_error_t *error)
{
	size_t value;

	value = 0;
	for (unsigned i = 0; i < LZ4_OFFSET_BYTES; i++) {
		if (block->consumed == block->in_len)
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: unterminated stream", error));
		value |= (size_t)block->in[block->consumed++] << (CHAR_BIT * i);
	}
	if (value == 0)
		return (lz4_fail(block, FSIMG_ECORRUPT,
		    "lz4: zero match offset", error));
	if (value > block->produced)
		return (lz4_fail(block, FSIMG_ECORRUPT,
		    "lz4: distance before the output start", error));
	*offset = value;
	return (FSIMG_OK);
}

/* Decode sequences until the block ends, the bound is reached or a verdict. */
static fsimg_status_t
lz4_decode_sequences(lz4_block_t *block, fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t count, from, offset;
	uint8_t token;

	for (;;) {
		if (block->mode == LZ4_PARTIAL &&
		    block->produced == block->bound)
			return (FSIMG_OK);
		if (block->consumed == block->in_len)
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: unterminated stream", error));
		token = block->in[block->consumed++];

		status = lz4_read_length(block, token >> LZ4_FIELD_BITS, 0,
		    &count, error);
		if (status != FSIMG_OK)
			return (status);
		if (count > block->in_len - block->consumed)
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: unterminated stream", error));
		for (size_t i = 0; i < count; i++)
			block->out[block->produced++] =
			    block->in[block->consumed++];

		if (block->mode == LZ4_PARTIAL &&
		    block->produced == block->bound)
			return (FSIMG_OK);
		/* The input ending after literals is the end of the block. */
		if (block->consumed == block->in_len) {
			if (block->mode == LZ4_FULL)
				return (FSIMG_OK);
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: stream ends before the requested length",
			    error));
		}

		status = lz4_read_offset(block, &offset, error);
		if (status != FSIMG_OK)
			return (status);
		status = lz4_read_length(block, token & LZ4_FIELD_MASK,
		    LZ4_MIN_MATCH, &count, error);
		if (status != FSIMG_OK)
			return (status);

		/* Front to back, so an offset below the count repeats. */
		from = block->produced - offset;
		for (size_t i = 0; i < count; i++)
			block->out[block->produced++] = block->out[from + i];
	}
}

static fsimg_status_t
lz4_decode(lz4_mode_t mode, const uint8_t *in, size_t in_len, uint8_t *out,
    size_t bound, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	lz4_block_t block = {
		.in = in, .in_len = in_len, .out = out, .bound = bound,
		.mode = mode
	};
	fsimg_status_t status;

	bzero(step, sizeof(*step));
	if ((in == NULL && in_len != 0) || (out == NULL && bound != 0))
		return (lz4_fail(&block, FSIMG_EINVAL,
		    "lz4: invalid buffer arguments", error));

	status = lz4_decode_sequences(&block, error);
	step->consumed = block.consumed;
	step->produced = block.produced;
	if (status == FSIMG_OK)
		step->done = true;
	return (status);
}

fsimg_status_t
fsimg_lz4_decode(const uint8_t *in, size_t in_len, uint8_t *out,
    size_t capacity, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	return (lz4_decode(LZ4_FULL, in, in_len, out, capacity, step, error));
}

fsimg_status_t
fsimg_lz4_decode_partial(const uint8_t *in, size_t in_len, uint8_t *out,
    size_t length, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	return (lz4_decode(LZ4_PARTIAL, in, in_len, out, length, step,
	    error));
}
