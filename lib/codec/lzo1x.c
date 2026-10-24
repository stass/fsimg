/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "codec.h"
#include "fsimg-endian.h"
#include "internal.h"

/*
 * The LZO1X decoder.
 *
 * An LZO1X stream is a sequence of instructions. Each is one byte whose top
 * bits select its class, then a length extension when its length field is
 * zero, then its distance operand, then up to three carried literals. A
 * literal run copies a counted run of input bytes to the output; a match
 * copies bytes already produced, from a distance behind the end of the
 * output. The stream ends with a far match at distance 16384, which no real
 * match can have.
 *
 * The decoder is a state machine that advances one byte at a time. Its
 * phase says what the next input byte is (an instruction byte, a length
 * extension byte, a distance operand byte) or where the next output byte
 * comes from (a literal from the input, a match byte from the output).
 * Every transition consumes one input byte or produces one output byte and
 * checks the one bound it touches first. A call returns when the next
 * transition needs an input byte it was not given or an output byte its
 * window has no room for, or at the end marker; the next call continues
 * from the same state wherever the caller split the stream.
 *
 * The output is bounded twice by the caller: the limit on the whole stream
 * and the capacity of each call's window. Every count read from the stream
 * is held against the limit before any byte of it is copied, so each copy
 * phase starts with its count within the output the limit still allows and
 * never needs to check the limit itself. A length is held to its bound as
 * it accumulates, with one exception: a far match is held only to a fixed
 * ceiling until its distance word shows that it is not the end marker,
 * because the marker, itself a far match of length 3 or more, must be
 * accepted when the output already equals the limit.
 */

/* Instruction bytes: the class boundaries of the instruction table. */
#define LZO1X_FAR_MATCH_MIN	16	/* 0001 HLLL */
#define LZO1X_LONG_MATCH_MIN	32	/* 001L LLLL */
#define LZO1X_NEAR_SHORT_MIN	64	/* 01LD DDSS, 3 or 4 bytes */
#define LZO1X_NEAR_LONG_MIN	128	/* 1LLD DDSS, 5 to 8 bytes */

/* A first byte of 18 or more is a run of byte - 17 literals. */
#define LZO1X_FIRST_RUN_MIN	18
#define LZO1X_FIRST_RUN_BIAS	17

/* Lengths: the base of each class's length field, and of its extension. */
#define LZO1X_RUN_BASE		3
#define LZO1X_RUN_EXTENDED	18
#define LZO1X_SHORT_LENGTH	2
#define LZO1X_MID_LENGTH	3
#define LZO1X_NEAR_SHORT_BASE	3
#define LZO1X_NEAR_LONG_BASE	5
#define LZO1X_LONG_BASE		2
#define LZO1X_LONG_EXTENDED	33
#define LZO1X_FAR_BASE		2
#define LZO1X_FAR_EXTENDED	9
#define LZO1X_EXTENSION_ZERO	255	/* a zero extension byte adds 255 */

/* Distances: each class's base and the weight of its high operand. */
#define LZO1X_SHORT_BASE	1
#define LZO1X_MID_BASE		2049
#define LZO1X_SHORT_SCALE	4
#define LZO1X_NEAR_BASE		1
#define LZO1X_NEAR_SCALE	8
#define LZO1X_LONG_DISTANCE_BASE 1
#define LZO1X_FAR_DISTANCE_BASE	16384
#define LZO1X_FAR_HIGH_WEIGHT	16384
#define LZO1X_END_DISTANCE	16384	/* the far match that ends the stream */

/* A far match's length before its distance word is known. */
#define LZO1X_FAR_LENGTH_CEILING UINT64_C(0xffffffff)

/* The literal state after a literal run: four or more literals. */
#define LZO1X_RUN_STATE		4

/* The farthest distance each match class reaches. */
#define LZO1X_NEAR_MAX_DISTANCE	2048	/* 1 + 7 + 8 x 255 */
#define LZO1X_LONG_MAX_DISTANCE	16384	/* 1 + (2^14 - 1) */
#define LZO1X_MAX_DISTANCE	49151	/* 16384 + 16384 + (2^14 - 1) */

/* The longest literal run and near match without an extension. */
#define LZO1X_FIRST_RUN_MAX	238	/* 255 - LZO1X_FIRST_RUN_BIAS */
#define LZO1X_NEAR_LONG_MAX	8	/* LZO1X_NEAR_LONG_BASE + 3 */

/* A match carries at most three literals in its S field. */
#define LZO1X_CARRIED_MAX	3

/*
 * The encoder's match finder: a table of 2^14 input positions indexed by
 * a multiplicative hash of the four bytes at each position. A candidate
 * is accepted at four bytes within near reach, where a near match costs
 * two bytes, and at five bytes beyond it, where a match costs three.
 */
#define LZO1X_HASH_BITS		14
#define LZO1X_HASH_ENTRIES	(1u << LZO1X_HASH_BITS)
#define LZO1X_HASH_MULTIPLIER	UINT64_C(2654435761)
#define LZO1X_GROUP_BYTES	4
#define LZO1X_NEAR_MIN_MATCH	4
#define LZO1X_MIN_MATCH		5

/* The table holds 32-bit offsets, so a block is at most 2^32 - 1 bytes. */
#define LZO1X_MAX_BLOCK		UINT32_MAX

/* The canonical end marker: a far match of 3 at distance 16384. */
static const uint8_t lzo1x_end_marker[] = { 0x11, 0x00, 0x00 };

/*
 * What one call may read and write, and how far it has got.
 *
 * in[0..in_len) is the caller's input piece; consumed counts the bytes of
 * it this call has used. The call writes window[history..history +
 * capacity); produced counts the bytes it has written there, so the next
 * byte goes to window[history + produced], and a match may reach back over
 * those history + produced bytes.
 */
typedef struct lzo1x_call {
	const uint8_t	*in;
	uint8_t		*window;
	size_t		 in_len;
	size_t		 consumed;
	size_t		 history;
	size_t		 capacity;
	size_t		 produced;
} lzo1x_call_t;

static fsimg_status_t lzo1x_fail(fsimg_lzo1x_decoder_t *, fsimg_status_t,
    const char *, fsimg_error_t *);
static fsimg_status_t lzo1x_length_exceeded(fsimg_lzo1x_decoder_t *,
    fsimg_error_t *);
static fsimg_status_t lzo1x_grow_length(fsimg_lzo1x_decoder_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t lzo1x_start_literals(fsimg_lzo1x_decoder_t *,
    uint64_t, fsimg_error_t *);
static fsimg_status_t lzo1x_after_length(fsimg_lzo1x_decoder_t *,
    fsimg_error_t *);
static fsimg_status_t lzo1x_start_match(fsimg_lzo1x_decoder_t *, size_t,
    fsimg_error_t *);
static fsimg_status_t lzo1x_read_instruction(fsimg_lzo1x_decoder_t *,
    uint8_t, fsimg_error_t *);
static fsimg_status_t lzo1x_read_extension(fsimg_lzo1x_decoder_t *,
    uint8_t, fsimg_error_t *);
static fsimg_status_t lzo1x_read_distance_word(fsimg_lzo1x_decoder_t *,
    uint8_t, size_t, fsimg_error_t *);
static fsimg_status_t lzo1x_read_byte(fsimg_lzo1x_decoder_t *, uint8_t,
    size_t, fsimg_error_t *);
static fsimg_status_t lzo1x_run(fsimg_lzo1x_decoder_t *, lzo1x_call_t *,
    fsimg_error_t *);

/*
 * The encoder's one table. table[h] is the most recent input position
 * whose four bytes hash to h, or whatever an earlier block or the initial
 * zero fill left there; an entry is only ever a candidate, checked before
 * use.
 */
struct fsimg_lzo1x_encoder {
	uint32_t	 table[LZO1X_HASH_ENTRIES];
};

/*
 * The stream being written. Every byte goes through lzo1x_put, which
 * refuses a byte past the capacity and records that it did, so no write
 * can overflow the buffer whatever the bound argument says.
 */
typedef struct lzo1x_output {
	uint8_t		*bytes;
	size_t		 capacity;
	size_t		 length;
	bool		 overflowed;
} lzo1x_output_t;

/* A match: copy length bytes from distance bytes back. */
typedef struct lzo1x_match {
	size_t		 length;
	uint32_t	 distance;
} lzo1x_match_t;

/*
 * One block being encoded. The stream holds every input byte before
 * literal_start except those of the pending match: the last match found,
 * not yet written because its S field must say how many literals follow
 * it. in[literal_start..) are the literals since then. Until the first
 * match is held there is no pending match (has_pending false) and nothing
 * has been written, so those literals are the stream's first run. The
 * scan alternates lzo1x_write_through, which consumes the pending match,
 * with lzo1x_hold_match, and ends with one lzo1x_write_through, so a
 * block is without a pending match only before its first match and after
 * its last write.
 */
typedef struct lzo1x_block {
	const uint8_t	*in;
	uint32_t	*table;
	size_t		 in_len;
	size_t		 literal_start;
	lzo1x_output_t	 out;
	lzo1x_match_t	 pending;
	bool		 has_pending;
} lzo1x_block_t;

static bool lzo1x_block_fits(size_t);
static uint64_t lzo1x_bound(uint64_t);
static void lzo1x_put(lzo1x_output_t *, uint8_t);
static void lzo1x_put_extension(lzo1x_output_t *, size_t);
static void lzo1x_put_word(lzo1x_output_t *, uint32_t, size_t);
static void lzo1x_write_match(lzo1x_output_t *, const lzo1x_match_t *,
    size_t);
static void lzo1x_write_through(lzo1x_block_t *, size_t);
static void lzo1x_hold_match(lzo1x_block_t *, size_t, const lzo1x_match_t *);
static uint32_t lzo1x_hash(const uint8_t *);
static void lzo1x_record_positions(lzo1x_block_t *, size_t, size_t);
static bool lzo1x_find_match(const lzo1x_block_t *, size_t,
    lzo1x_match_t *);

void
fsimg_lzo1x_decoder_init(fsimg_lzo1x_decoder_t *decoder, uint64_t limit)
{
	bzero(decoder, sizeof(*decoder));
	decoder->limit = limit;
	decoder->phase = FSIMG_LZO1X_FIRST_BYTE;
}

/* Every verdict finishes the decoder and names the stream offset. */
static fsimg_status_t
lzo1x_fail(fsimg_lzo1x_decoder_t *decoder, fsimg_status_t status,
    const char *check, fsimg_error_t *error)
{
	decoder->phase = FSIMG_LZO1X_FAILED;
	fsimg_error_set(error, status, 0, FSIMG_OP_READER, check);
	if (error != NULL) {
		error->offset = decoder->stream_in;
		error->offset_valid = true;
	}
	return (status);
}

/* The verdict for a length past its bound: see length_bound. */
static fsimg_status_t
lzo1x_length_exceeded(fsimg_lzo1x_decoder_t *decoder, fsimg_error_t *error)
{
	if (decoder->instruction == FSIMG_LZO1X_FAR_MATCH)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: far-match length out of range", error));
	return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
	    "lzo1x: output longer than its limit", error));
}

/*
 * Add one extension byte's worth to the length. The length is within its
 * bound, so the room left cannot wrap, and comparing the increment with it
 * never forms a sum that could.
 */
static fsimg_status_t
lzo1x_grow_length(fsimg_lzo1x_decoder_t *decoder, uint64_t increment,
    fsimg_error_t *error)
{
	if (increment > decoder->length_bound - decoder->length)
		return (lzo1x_length_exceeded(decoder, error));
	decoder->length += increment;
	return (FSIMG_OK);
}

/*
 * Literals follow: count bytes copied from the input, held to the limit
 * before the first of them is written. Every literal copy starts here, so
 * the check covers first runs and carried literals by construction; for a
 * long run it repeats the check its length already passed. The literal
 * state becomes the count, capped at four, which is all the next
 * instruction byte needs to know about them.
 */
static fsimg_status_t
lzo1x_start_literals(fsimg_lzo1x_decoder_t *decoder, uint64_t count,
    fsimg_error_t *error)
{
	if (count > decoder->limit - decoder->stream_out)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: output longer than its limit", error));
	decoder->literal_state = count < LZO1X_RUN_STATE ? (uint8_t)count :
	    LZO1X_RUN_STATE;
	decoder->length = count;
	if (count == 0)
		decoder->phase = FSIMG_LZO1X_INSTRUCTION;
	else
		decoder->phase = FSIMG_LZO1X_COPY_LITERALS;
	return (FSIMG_OK);
}

/* The length is complete: the run's literals or the distance come next. */
static fsimg_status_t
lzo1x_after_length(fsimg_lzo1x_decoder_t *decoder, fsimg_error_t *error)
{
	switch (decoder->instruction) {
	case FSIMG_LZO1X_LITERAL_RUN:
		return (lzo1x_start_literals(decoder, decoder->length, error));
	case FSIMG_LZO1X_FAR_MATCH:
	case FSIMG_LZO1X_LONG_MATCH:
		decoder->phase = FSIMG_LZO1X_DISTANCE_WORD_LOW;
		break;
	case FSIMG_LZO1X_SHORT_MATCH:
	case FSIMG_LZO1X_MID_MATCH:
	case FSIMG_LZO1X_NEAR_MATCH:
		decoder->phase = FSIMG_LZO1X_DISTANCE_BYTE;
		break;
	}
	return (FSIMG_OK);
}

/*
 * The distance is complete. The image's faults come first: a distance
 * reaching before the stream's first byte, and a far match's length, held
 * to the limit only now, past it. Then the caller's: a distance within the
 * stream but beyond the history plus the bytes this call has written means
 * the caller kept too little.
 */
static fsimg_status_t
lzo1x_start_match(fsimg_lzo1x_decoder_t *decoder, size_t available,
    fsimg_error_t *error)
{
	if (decoder->distance > decoder->stream_out)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: distance before the output start", error));
	if (decoder->instruction == FSIMG_LZO1X_FAR_MATCH &&
	    decoder->length > decoder->limit - decoder->stream_out)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: output longer than its limit", error));
	if (decoder->distance > available)
		return (lzo1x_fail(decoder, FSIMG_ESTATE,
		    "lzo1x: distance beyond the kept history", error));
	decoder->phase = FSIMG_LZO1X_COPY_MATCH;
	return (FSIMG_OK);
}

/*
 * Decode an instruction byte into its class, its length (the base, when
 * the length field is zero and an extension follows) and the distance
 * bits it carries, and set the bound its length is held to while it is
 * read. Only a byte below 16 depends on the literal state.
 */
static fsimg_status_t
lzo1x_read_instruction(fsimg_lzo1x_decoder_t *decoder, uint8_t byte,
    fsimg_error_t *error)
{
	unsigned field;
	bool extended;

	extended = false;
	if (byte >= LZO1X_NEAR_LONG_MIN) {
		/* 1LLD DDSS, then the byte H: distance 1 + D + 8H. */
		decoder->instruction = FSIMG_LZO1X_NEAR_MATCH;
		decoder->length = LZO1X_NEAR_LONG_BASE + ((byte >> 5) & 3);
		decoder->distance = LZO1X_NEAR_BASE + ((byte >> 2) & 7);
		decoder->carried = byte & 3;
	} else if (byte >= LZO1X_NEAR_SHORT_MIN) {
		/* 01LD DDSS, then the byte H: distance 1 + D + 8H. */
		decoder->instruction = FSIMG_LZO1X_NEAR_MATCH;
		decoder->length = LZO1X_NEAR_SHORT_BASE + ((byte >> 5) & 1);
		decoder->distance = LZO1X_NEAR_BASE + ((byte >> 2) & 7);
		decoder->carried = byte & 3;
	} else if (byte >= LZO1X_LONG_MATCH_MIN) {
		/* 001L LLLL, then the word 4D + S: distance 1 + D. */
		decoder->instruction = FSIMG_LZO1X_LONG_MATCH;
		field = byte & 31;
		extended = field == 0;
		decoder->length = extended ? LZO1X_LONG_EXTENDED :
		    LZO1X_LONG_BASE + field;
		decoder->distance = LZO1X_LONG_DISTANCE_BASE;
	} else if (byte >= LZO1X_FAR_MATCH_MIN) {
		/* 0001 HLLL, then the word 4D + S: 16384 + 16384H + D. */
		decoder->instruction = FSIMG_LZO1X_FAR_MATCH;
		field = byte & 7;
		extended = field == 0;
		decoder->length = extended ? LZO1X_FAR_EXTENDED :
		    LZO1X_FAR_BASE + field;
		decoder->distance = LZO1X_FAR_DISTANCE_BASE +
		    LZO1X_FAR_HIGH_WEIGHT * ((byte >> 3) & 1);
	} else if (decoder->literal_state == 0) {
		/* 0000 LLLL after no literals: a run of 3 + L literals. */
		decoder->instruction = FSIMG_LZO1X_LITERAL_RUN;
		field = byte & 15;
		extended = field == 0;
		decoder->length = extended ? LZO1X_RUN_EXTENDED :
		    LZO1X_RUN_BASE + field;
	} else if (decoder->literal_state < LZO1X_RUN_STATE) {
		/* 0000 DDSS after 1 to 3 literals, then H: 1 + D + 4H. */
		decoder->instruction = FSIMG_LZO1X_SHORT_MATCH;
		decoder->length = LZO1X_SHORT_LENGTH;
		decoder->distance = LZO1X_SHORT_BASE + ((byte >> 2) & 3);
		decoder->carried = byte & 3;
	} else {
		/* 0000 DDSS after a run, then H: 2049 + D + 4H. */
		decoder->instruction = FSIMG_LZO1X_MID_MATCH;
		decoder->length = LZO1X_MID_LENGTH;
		decoder->distance = LZO1X_MID_BASE + ((byte >> 2) & 3);
		decoder->carried = byte & 3;
	}

	/*
	 * The bound the length is held to while it is read: the output the
	 * limit still allows, except for a far match, which may be the end
	 * marker and is held to the limit in start_match instead.
	 */
	if (decoder->instruction == FSIMG_LZO1X_FAR_MATCH)
		decoder->length_bound = LZO1X_FAR_LENGTH_CEILING;
	else
		decoder->length_bound = decoder->limit - decoder->stream_out;
	if (decoder->length > decoder->length_bound)
		return (lzo1x_length_exceeded(decoder, error));

	if (extended) {
		decoder->phase = FSIMG_LZO1X_LENGTH_EXTENSION;
		return (FSIMG_OK);
	}
	return (lzo1x_after_length(decoder, error));
}

/*
 * A length extension: each zero byte adds 255, and the one nonzero byte
 * that ends it adds its value.
 */
static fsimg_status_t
lzo1x_read_extension(fsimg_lzo1x_decoder_t *decoder, uint8_t byte,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = lzo1x_grow_length(decoder, byte == 0 ? LZO1X_EXTENSION_ZERO :
	    byte, error);
	if (status != FSIMG_OK)
		return (status);
	if (byte == 0)
		return (FSIMG_OK);
	return (lzo1x_after_length(decoder, error));
}

/*
 * The second byte of the little-endian word 4D + S of a long or far match:
 * D completes the distance and S counts the carried literals. A far match
 * at distance 16384 is the end marker; its length and S mean nothing.
 */
static fsimg_status_t
lzo1x_read_distance_word(fsimg_lzo1x_decoder_t *decoder, uint8_t byte,
    size_t available, fsimg_error_t *error)
{
	uint32_t word;

	word = (uint32_t)decoder->word_low | (uint32_t)byte << 8;
	decoder->distance += word >> 2;
	decoder->carried = word & 3;
	if (decoder->instruction == FSIMG_LZO1X_FAR_MATCH &&
	    decoder->distance == LZO1X_END_DISTANCE) {
		decoder->phase = FSIMG_LZO1X_DONE;
		return (FSIMG_OK);
	}
	return (lzo1x_start_match(decoder, available, error));
}

/*
 * Consume one byte of an instruction. available is the history plus the
 * bytes this call has written: what a match may reach back over.
 */
static fsimg_status_t
lzo1x_read_byte(fsimg_lzo1x_decoder_t *decoder, uint8_t byte,
    size_t available, fsimg_error_t *error)
{
	uint32_t scale;

	switch (decoder->phase) {
	case FSIMG_LZO1X_FIRST_BYTE:
		if (byte >= LZO1X_FIRST_RUN_MIN)
			return (lzo1x_start_literals(decoder,
			    byte - LZO1X_FIRST_RUN_BIAS, error));
		return (lzo1x_read_instruction(decoder, byte, error));
	case FSIMG_LZO1X_INSTRUCTION:
		return (lzo1x_read_instruction(decoder, byte, error));
	case FSIMG_LZO1X_LENGTH_EXTENSION:
		return (lzo1x_read_extension(decoder, byte, error));
	case FSIMG_LZO1X_DISTANCE_BYTE:
		scale = decoder->instruction == FSIMG_LZO1X_NEAR_MATCH ?
		    LZO1X_NEAR_SCALE : LZO1X_SHORT_SCALE;
		decoder->distance += scale * byte;
		return (lzo1x_start_match(decoder, available, error));
	case FSIMG_LZO1X_DISTANCE_WORD_LOW:
		decoder->word_low = byte;
		decoder->phase = FSIMG_LZO1X_DISTANCE_WORD_HIGH;
		return (FSIMG_OK);
	case FSIMG_LZO1X_DISTANCE_WORD_HIGH:
		return (lzo1x_read_distance_word(decoder, byte, available,
		    error));
	case FSIMG_LZO1X_COPY_LITERALS:
	case FSIMG_LZO1X_COPY_MATCH:
	case FSIMG_LZO1X_DONE:
	case FSIMG_LZO1X_FAILED:
		break;
	}
	/* lzo1x_run() handles these phases without reading a byte. */
	return (lzo1x_fail(decoder, FSIMG_ESTATE,
	    "lzo1x: no instruction byte expected", error));
}

/*
 * Advance until the next transition needs an input byte the call does not
 * have or an output byte its window has no room for, the end marker, or a
 * verdict. A literal needs an input byte and room in the window; a match
 * byte needs room in the window. Neither checks the limit: the count of
 * every copy was held to it when the copy began.
 */
static fsimg_status_t
lzo1x_run(fsimg_lzo1x_decoder_t *decoder, lzo1x_call_t *call,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t write_at;
	uint8_t byte;

	for (;;) {
		write_at = call->history + call->produced;
		switch (decoder->phase) {
		case FSIMG_LZO1X_DONE:
			return (FSIMG_OK);
		case FSIMG_LZO1X_COPY_LITERALS:
			if (call->consumed == call->in_len ||
			    call->produced == call->capacity)
				return (FSIMG_OK);
			call->window[write_at] = call->in[call->consumed];
			call->consumed++;
			call->produced++;
			decoder->stream_in++;
			decoder->stream_out++;
			if (--decoder->length == 0)
				decoder->phase = FSIMG_LZO1X_INSTRUCTION;
			break;
		case FSIMG_LZO1X_COPY_MATCH:
			if (call->produced == call->capacity)
				return (FSIMG_OK);
			/*
			 * Front to back, one byte at a time, so a distance
			 * shorter than the length repeats a run. The distance
			 * is within write_at: it was checked against it when
			 * the match began or this call started, and write_at
			 * only grows within a call.
			 */
			call->window[write_at] =
			    call->window[write_at - decoder->distance];
			call->produced++;
			decoder->stream_out++;
			if (--decoder->length != 0)
				break;
			status = lzo1x_start_literals(decoder,
			    decoder->carried, error);
			if (status != FSIMG_OK)
				return (status);
			break;
		default:
			if (call->consumed == call->in_len)
				return (FSIMG_OK);
			byte = call->in[call->consumed];
			call->consumed++;
			decoder->stream_in++;
			status = lzo1x_read_byte(decoder, byte, write_at, error);
			if (status != FSIMG_OK)
				return (status);
			break;
		}
	}
}

fsimg_status_t
fsimg_lzo1x_decode(fsimg_lzo1x_decoder_t *decoder, const uint8_t *in,
    size_t in_len, bool final, uint8_t *window, size_t history,
    size_t capacity, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	lzo1x_call_t call;
	fsimg_status_t status;

	bzero(step, sizeof(*step));
	if (decoder->phase == FSIMG_LZO1X_FAILED)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "lzo1x: decoder already failed"));
	if ((in == NULL && in_len != 0) || capacity > SIZE_MAX - history ||
	    (window == NULL && history + capacity != 0))
		return (lzo1x_fail(decoder, FSIMG_EINVAL,
		    "lzo1x: invalid buffer arguments", error));
	/* A match continued from the last call must still be in reach. */
	if (decoder->phase == FSIMG_LZO1X_COPY_MATCH &&
	    decoder->distance > history)
		return (lzo1x_fail(decoder, FSIMG_ESTATE,
		    "lzo1x: distance beyond the kept history", error));

	call = (lzo1x_call_t){ .in = in, .window = window, .in_len = in_len,
	    .history = history, .capacity = capacity };
	status = lzo1x_run(decoder, &call, error);
	step->consumed = call.consumed;
	step->produced = call.produced;
	step->done = decoder->phase == FSIMG_LZO1X_DONE;
	if (status != FSIMG_OK)
		return (status);

	/* The caller gives the stream's exact end, so nothing may follow. */
	if (step->done && call.consumed < in_len)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: trailing bytes after the end marker", error));
	if (!step->done && final && call.consumed == in_len)
		return (lzo1x_fail(decoder, FSIMG_ECORRUPT,
		    "lzo1x: unterminated stream", error));
	return (FSIMG_OK);
}

/*
 * The LZO1X encoder.
 *
 * One call turns one block into one complete stream. The block is scanned
 * from its first byte. At each position with four bytes left, the hash of
 * those four bytes selects one table entry: the entry's position is the
 * candidate, and the current position replaces it. A candidate that lies
 * before the current position, within 49151 bytes, with the same four
 * bytes, is extended forward while the bytes agree; the match is taken
 * when it is long enough for its class to save bytes. Otherwise the byte
 * becomes a literal and the scan moves on by one. Matching is greedy: a
 * match is written as found, never deferred in favour of a later one.
 *
 * The stream is a function of the block alone, whatever the table held
 * when the call began. Every position before the current one that has
 * four bytes has been recorded, those inside matches included, so the
 * entry for a group of bytes holds the latest earlier position with that
 * hash. A stale entry from an earlier block is therefore either at or past
 * the current position, or before it with different bytes (equal bytes
 * would have hashed to the same entry and overwritten it); both are
 * refused, exactly as the fresh table's zero would be. A change that skips
 * recording a position breaks this and must clear the table per block.
 *
 * Instructions are chosen so that no stream is longer than
 * lzo1x_bound(n) = n + n / 255 + 6. Every match of m bytes costs at most
 * m - 2: a near match two bytes for 4 to 8, a long or far match three
 * bytes for the lengths without an extension and more slowly growing
 * beyond. Every literal run costs its literals plus a header of at most
 * 2 + count / 255 bytes, carried literals none. The runs with a header
 * number at most one more than the matches, the first run before any
 * match, so the headers cost at most 2 (M + 1) + n / 255 against the 2M
 * the M matches save, and the end marker adds three. The output buffer
 * is still checked on every byte, so a flaw in this argument is a status,
 * never an overflow.
 */

/*
 * Is in_len a block the table's 32-bit offsets can index? The length is
 * widened first, so the comparison means the same on every host and is
 * simply always true where size_t has 32 bits.
 */
static bool
lzo1x_block_fits(size_t in_len)
{
	uint64_t length;

	length = in_len;
	return (length <= LZO1X_MAX_BLOCK);
}

/* Every block's worst-case stream length, in 64 bits on every host. */
static uint64_t
lzo1x_bound(uint64_t in_len)
{
	return (in_len + in_len / LZO1X_EXTENSION_ZERO + 6);
}

size_t
fsimg_lzo1x_encode_bound(size_t in_len)
{
	uint64_t bound;

	if (!lzo1x_block_fits(in_len))
		return (SIZE_MAX);
	bound = lzo1x_bound(in_len);
#if SIZE_MAX < UINT64_MAX
	/* A 32-bit size_t cannot hold the bound of the longest blocks. */
	if (bound > SIZE_MAX)
		return (SIZE_MAX);
#endif
	return ((size_t)bound);
}

fsimg_status_t
fsimg_lzo1x_encoder_new(fsimg_ctx_t *ctx, fsimg_lzo1x_encoder_t **out,
    fsimg_error_t *error)
{
	fsimg_lzo1x_encoder_t *encoder;

	*out = NULL;
	/* fsimg_alloc zero-fills, so the table starts in a defined state. */
	encoder = fsimg_alloc(ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	*out = encoder;
	return (FSIMG_OK);
}

void
fsimg_lzo1x_encoder_free(fsimg_lzo1x_encoder_t *encoder)
{
	fsimg_dealloc(encoder);
}

static void
lzo1x_put(lzo1x_output_t *out, uint8_t byte)
{
	if (out->length == out->capacity) {
		out->overflowed = true;
		return;
	}
	out->bytes[out->length] = byte;
	out->length++;
}

/*
 * A length extension of value 1 or more: each zero byte stands for 255,
 * and the final byte, 1 to 255, for the rest.
 */
static void
lzo1x_put_extension(lzo1x_output_t *out, size_t value)
{
	size_t zero_bytes;

	zero_bytes = (value - 1) / LZO1X_EXTENSION_ZERO;
	for (size_t i = 0; i < zero_bytes; i++)
		lzo1x_put(out, 0);
	lzo1x_put(out, (uint8_t)(value - zero_bytes * LZO1X_EXTENSION_ZERO));
}

/*
 * The 16-bit little-endian word of a long or far match: a 14-bit distance
 * field above the two bits of carried literals.
 */
static void
lzo1x_put_word(lzo1x_output_t *out, uint32_t distance_field, size_t carried)
{
	uint32_t word;

	word = (distance_field << 2) | (uint32_t)carried;
	lzo1x_put(out, (uint8_t)(word & 0xff));
	lzo1x_put(out, (uint8_t)(word >> 8));
}

/*
 * Write a match carrying 0 to 3 literals, in the cheapest class that
 * reaches its distance: near (two bytes, lengths 4 to 8 within 2048),
 * long (within 16384) or far (to 49151). A long or far length past its
 * field is the field's zero and an extension.
 */
static void
lzo1x_write_match(lzo1x_output_t *out, const lzo1x_match_t *match,
    size_t carried)
{
	uint32_t field;
	uint8_t high;

	if (match->distance <= LZO1X_NEAR_MAX_DISTANCE &&
	    match->length <= LZO1X_NEAR_LONG_MAX) {
		/* 01LD DDSS or 1LLD DDSS, then the byte H: d = 1 + D + 8H. */
		field = match->distance - LZO1X_NEAR_BASE;
		if (match->length < LZO1X_NEAR_LONG_BASE)
			lzo1x_put(out, (uint8_t)(LZO1X_NEAR_SHORT_MIN |
			    (match->length - LZO1X_NEAR_SHORT_BASE) << 5 |
			    (field % LZO1X_NEAR_SCALE) << 2 | carried));
		else
			lzo1x_put(out, (uint8_t)(LZO1X_NEAR_LONG_MIN |
			    (match->length - LZO1X_NEAR_LONG_BASE) << 5 |
			    (field % LZO1X_NEAR_SCALE) << 2 | carried));
		lzo1x_put(out, (uint8_t)(field / LZO1X_NEAR_SCALE));
		return;
	}

	if (match->distance <= LZO1X_LONG_MAX_DISTANCE) {
		/* 001L LLLL, extension, word 4D + S: d = 1 + D. */
		if (match->length <= LZO1X_LONG_EXTENDED) {
			lzo1x_put(out, (uint8_t)(LZO1X_LONG_MATCH_MIN |
			    (match->length - LZO1X_LONG_BASE)));
		} else {
			lzo1x_put(out, LZO1X_LONG_MATCH_MIN);
			lzo1x_put_extension(out,
			    match->length - LZO1X_LONG_EXTENDED);
		}
		lzo1x_put_word(out, match->distance - LZO1X_LONG_DISTANCE_BASE,
		    carried);
		return;
	}

	/* 0001 HLLL, extension, word 4D + S: d = 16384 + 16384H + D. */
	field = match->distance - LZO1X_FAR_DISTANCE_BASE;
	high = (uint8_t)((field / LZO1X_FAR_HIGH_WEIGHT) << 3);
	if (match->length <= LZO1X_FAR_EXTENDED) {
		lzo1x_put(out, (uint8_t)(LZO1X_FAR_MATCH_MIN | high |
		    (match->length - LZO1X_FAR_BASE)));
	} else {
		lzo1x_put(out, (uint8_t)(LZO1X_FAR_MATCH_MIN | high));
		lzo1x_put_extension(out, match->length - LZO1X_FAR_EXTENDED);
	}
	lzo1x_put_word(out, field % LZO1X_FAR_HIGH_WEIGHT, carried);
}

/*
 * Write everything before end that is not yet in the stream: the pending
 * match, now that the count of literals after it is known, then those
 * literals, in[literal_start..end). Up to three ride in the match's S
 * field; four or more follow it, with S = 0, as a long literal run.
 * Without a pending match the literals are the stream's first run. The
 * pending match is consumed, so it is written exactly once.
 */
static void
lzo1x_write_through(lzo1x_block_t *block, size_t end)
{
	lzo1x_output_t *out;
	size_t count;

	out = &block->out;
	count = end - block->literal_start;
	if (!block->has_pending) {
		/* The first run: 17 + n, or 0000 0000 and an extension. */
		if (count == 0) {
			/* Nothing precedes the end marker of an empty block. */
		} else if (count <= LZO1X_FIRST_RUN_MAX) {
			lzo1x_put(out, (uint8_t)(LZO1X_FIRST_RUN_BIAS + count));
		} else {
			lzo1x_put(out, 0);
			lzo1x_put_extension(out, count - LZO1X_RUN_EXTENDED);
		}
	} else if (count <= LZO1X_CARRIED_MAX) {
		lzo1x_write_match(out, &block->pending, count);
	} else {
		/* A long run after a match: 0000 LLLL, L = 0 extended. */
		lzo1x_write_match(out, &block->pending, 0);
		if (count <= LZO1X_RUN_EXTENDED) {
			lzo1x_put(out, (uint8_t)(count - LZO1X_RUN_BASE));
		} else {
			lzo1x_put(out, 0);
			lzo1x_put_extension(out, count - LZO1X_RUN_EXTENDED);
		}
	}

	for (size_t i = block->literal_start; i < end; i++)
		lzo1x_put(out, block->in[i]);
	block->has_pending = false;
	block->literal_start = end;
}

/*
 * The match at position becomes the pending one, written by the next
 * lzo1x_write_through; the literals after it start where it ends.
 */
static void
lzo1x_hold_match(lzo1x_block_t *block, size_t position,
    const lzo1x_match_t *match)
{
	block->pending = *match;
	block->has_pending = true;
	block->literal_start = position + match->length;
}

/*
 * The table index of the four bytes at in: their little-endian value
 * times 2654435761, modulo 2^32, top 14 bits. The product is formed in
 * 64 bits and reduced explicitly, so no host's integer promotion changes
 * it.
 */
static uint32_t
lzo1x_hash(const uint8_t *in)
{
	uint32_t group, product;

	group = fsimg_load_le32(in);
	product = (uint32_t)((group * LZO1X_HASH_MULTIPLIER) & UINT32_MAX);
	return (product >> (32 - LZO1X_HASH_BITS));
}

/* Record each position in [from, to) that has four bytes of input. */
static void
lzo1x_record_positions(lzo1x_block_t *block, size_t from, size_t to)
{
	for (size_t position = from; position < to; position++) {
		if (block->in_len - position < LZO1X_GROUP_BYTES)
			return;
		block->table[lzo1x_hash(block->in + position)] =
		    (uint32_t)position;
	}
}

/*
 * Look up the candidate for the four bytes at position, which must have
 * four bytes of input. It becomes a match when it is a real earlier
 * occurrence in reach and, extended, long enough to be worth writing.
 * The lookup changes nothing; the caller records position afterwards.
 */
static bool
lzo1x_find_match(const lzo1x_block_t *block, size_t position,
    lzo1x_match_t *match)
{
	const uint8_t *in;
	size_t candidate, distance, length, shortest;

	in = block->in;
	candidate = block->table[lzo1x_hash(in + position)];

	/* The entry may be any earlier block's; check before reading. */
	if (candidate >= position || position - candidate > LZO1X_MAX_DISTANCE)
		return (false);
	if (bcmp(in + candidate, in + position, LZO1X_GROUP_BYTES) != 0)
		return (false);

	length = LZO1X_GROUP_BYTES;
	while (position + length < block->in_len &&
	    in[candidate + length] == in[position + length])
		length++;
	distance = position - candidate;
	shortest = distance <= LZO1X_NEAR_MAX_DISTANCE ?
	    LZO1X_NEAR_MIN_MATCH : LZO1X_MIN_MATCH;
	if (length < shortest)
		return (false);

	match->length = length;
	match->distance = (uint32_t)distance;
	return (true);
}

fsimg_status_t
fsimg_lzo1x_encode(fsimg_lzo1x_encoder_t *encoder, const uint8_t *in,
    size_t in_len, uint8_t *out, size_t capacity, size_t *out_len,
    fsimg_error_t *error)
{
	lzo1x_block_t block;
	lzo1x_match_t match;
	size_t next, position;
	bool found;

	*out_len = 0;
	if ((in == NULL && in_len != 0) || (out == NULL && capacity != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lzo1x: invalid buffer arguments"));
	if (!lzo1x_block_fits(in_len))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lzo1x: block longer than 2^32 - 1 bytes"));
	if (capacity < lzo1x_bound(in_len))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lzo1x: output smaller than the bound"));

	block = (lzo1x_block_t){ .in = in, .table = encoder->table,
	    .in_len = in_len,
	    .out = { .bytes = out, .capacity = capacity } };
	/*
	 * Each step looks up position, then records every position it
	 * moves past, so every earlier position with four bytes is in the
	 * table before a later one is looked up.
	 */
	position = 0;
	while (in_len - position >= LZO1X_GROUP_BYTES) {
		found = lzo1x_find_match(&block, position, &match);
		next = found ? position + match.length : position + 1;
		lzo1x_record_positions(&block, position, next);
		if (found) {
			lzo1x_write_through(&block, position);
			lzo1x_hold_match(&block, position, &match);
		}
		position = next;
	}
	/* The tail, too short to hash, joins the last literals. */
	lzo1x_write_through(&block, in_len);
	for (size_t i = 0; i < sizeof(lzo1x_end_marker); i++)
		lzo1x_put(&block.out, lzo1x_end_marker[i]);

	if (block.out.overflowed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "lzo1x: encoder output exceeded its bound"));
	*out_len = block.out.length;
	return (FSIMG_OK);
}
