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
 * A whole block holding a match must also end well clear of it: at least
 * five final literals, and the last match starting at least 12 bytes
 * before the output's end. The format leaves these to the encoder, but the
 * kernel and fsck.erofs refuse a block that breaks them, so full mode
 * refuses it too, in one check once the block has ended. Partial mode stops
 * before the end, or cannot tell it is there, and does not judge them.
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
#define LZ4_END_LITERALS	5	/* final literals after a match */
#define LZ4_END_MATCH_DISTANCE	12	/* last match start to output end */

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
	bool		 matched;	/* a match has been copied */
	size_t		 match_start;	/* where the last match's output began */
} lz4_block_t;

static fsimg_status_t lz4_fail(const lz4_block_t *, fsimg_status_t,
    const char *, fsimg_error_t *);
static fsimg_status_t lz4_read_length(lz4_block_t *, unsigned, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t lz4_read_offset(lz4_block_t *, size_t *,
    fsimg_error_t *);
static fsimg_status_t lz4_check_end(const lz4_block_t *, size_t,
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

/*
 * Judge a whole block's end, once its input has ended after the final
 * literal run of the given length. A literal-only block meets the end
 * conditions whatever its length; one holding a match needs at least five
 * final literals and its last match starting at least 12 bytes before the
 * output's end. The verdict's offset is the bytes consumed, all of them.
 */
static fsimg_status_t
lz4_check_end(const lz4_block_t *block, size_t literals, fsimg_error_t *error)
{
	if (!block->matched)
		return (FSIMG_OK);
	if (literals < LZ4_END_LITERALS ||
	    block->produced - block->match_start < LZ4_END_MATCH_DISTANCE)
		return (lz4_fail(block, FSIMG_ECORRUPT, "lz4: block end",
		    error));
	return (FSIMG_OK);
}

/* Decode sequences until the block ends, the bound is reached or a verdict. */
static fsimg_status_t
lz4_decode_sequences(lz4_block_t *block, fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t count, from, literals, offset;
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
		    &literals, error);
		if (status != FSIMG_OK)
			return (status);
		if (literals > block->in_len - block->consumed)
			return (lz4_fail(block, FSIMG_ECORRUPT,
			    "lz4: unterminated stream", error));
		for (size_t i = 0; i < literals; i++)
			block->out[block->produced++] =
			    block->in[block->consumed++];

		if (block->mode == LZ4_PARTIAL &&
		    block->produced == block->bound)
			return (FSIMG_OK);
		/* The input ending after literals is the end of the block. */
		if (block->consumed == block->in_len) {
			if (block->mode == LZ4_FULL)
				return (lz4_check_end(block, literals, error));
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

		/* The end conditions are judged against the last match. */
		block->matched = true;
		block->match_start = block->produced;

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

/*
 * The LZ4 block encoder.
 *
 * One call writes one complete block of a prefix of the input: the whole
 * input in whole mode, or in prefix mode as much as fits a fixed output
 * size, which is how an EROFS cluster is filled. The block is a run of
 * sequences, each the pending literals and one match, then a final run of
 * literals only.
 *
 * Matches come from a greedy scan. At each position that may still start
 * a match, the hash of its four bytes selects one table entry: the entry's
 * position is the candidate, and the current position replaces it. A
 * candidate before the current position, within the largest offset, whose
 * four bytes are equal is extended forward while the bytes agree. The scan
 * never starts a match within 12 bytes of the input's end, and never lets
 * one reach the last five bytes, so a whole block meets the end conditions
 * the decoder's full mode and the kernel enforce.
 *
 * A prefix block can end before the input does, so every sequence is
 * taken only if it fits together with the final run it then requires:
 * enough literals that the block still ends five bytes after the match
 * and 12 after its start. When a sequence does not fit, or the pending
 * literals no longer fit even as the final run, the scan stops and the
 * final run takes as many literals as fit. At a capacity of at least the
 * bound nothing ever stops the scan, so whole mode is prefix mode with the
 * capacity checked first.
 *
 * The table is zero-filled at the start of every call, so the block
 * depends only on the input, its length and the capacity, never on earlier
 * calls or the table's initial memory. Every output byte goes through
 * lz4_put, which refuses a byte past the capacity, so no write can
 * overflow even if the fit arithmetic were wrong.
 */
#define LZ4_HASH_BITS		12	/* 4096 entries: a 64 KiB window */
#define LZ4_HASH_ENTRIES	(1u << LZ4_HASH_BITS)
#define LZ4_HASH_MULTIPLIER	2654435761u	/* 2^32 over the golden ratio */
#define LZ4_GROUP_BYTES		4	/* the bytes one hash covers */
#define LZ4_MAX_DISTANCE	65535u	/* the largest two-byte offset */
#define LZ4_TOKEN_BYTES		1
#define LZ4_ENCODE_MAX_INPUT	UINT32_MAX	/* table position width */

/* The encoder's one table: table[h] is a candidate position for hash h. */
struct fsimg_lz4_encoder {
	uint32_t	 table[LZ4_HASH_ENTRIES];
};

/* One encode: the input, the block being written and how far each is. */
typedef struct lz4_encode {
	const uint8_t	*in;
	size_t		 in_len;
	uint32_t	*table;
	uint8_t		*out;
	size_t		 capacity;
	size_t		 produced;	/* block bytes written */
	size_t		 covered;	/* input the written sequences encode */
	size_t		 final_needed;	/* least final run after last match */
	bool		 overflowed;	/* lz4_put refused a byte */
} lz4_encode_t;

/* A match the scan found at its current position. */
typedef struct lz4_match {
	size_t	 distance;
	size_t	 length;
} lz4_match_t;

static size_t lz4_extension_bytes(size_t);
static size_t lz4_run_cost(size_t);
static size_t lz4_sequence_cost(size_t, size_t);
static size_t lz4_final_needed(size_t);
static size_t lz4_room(const lz4_encode_t *);
static uint32_t lz4_hash(const uint8_t *);
static bool lz4_may_start_match(const lz4_encode_t *, size_t);
static bool lz4_find_match(lz4_encode_t *, size_t, lz4_match_t *);
static void lz4_record_positions(lz4_encode_t *, size_t, size_t);
static bool lz4_sequence_fits(const lz4_encode_t *, size_t,
    const lz4_match_t *);
static size_t lz4_final_fit(const lz4_encode_t *);
static void lz4_put(lz4_encode_t *, uint8_t);
static void lz4_put_extension(lz4_encode_t *, size_t);
static void lz4_put_run(lz4_encode_t *, size_t, size_t);
static void lz4_put_sequence(lz4_encode_t *, size_t, const lz4_match_t *);
static bool lz4_encode_block(lz4_encode_t *);
static fsimg_status_t lz4_encode_check(const uint8_t *, size_t,
    const uint8_t *, size_t, fsimg_error_t *);
static fsimg_status_t lz4_encode_run(fsimg_lz4_encoder_t *, const uint8_t *,
    size_t, uint8_t *, size_t, lz4_encode_t *, fsimg_error_t *);

/* The extension bytes of a length field's value: none below 15. */
static size_t
lz4_extension_bytes(size_t value)
{
	if (value < LZ4_FIELD_EXTENDED)
		return (0);
	return (1 + (value - LZ4_FIELD_EXTENDED) / LZ4_EXTENSION_MORE);
}

/*
 * The bytes of a token, its literal extension and the literals: the whole
 * of a final run, or a sequence's start, before its offset.
 */
static size_t
lz4_run_cost(size_t literals)
{
	return (LZ4_TOKEN_BYTES + lz4_extension_bytes(literals) + literals);
}

/* The bytes of a sequence: the literals, then a match of length bytes. */
static size_t
lz4_sequence_cost(size_t literals, size_t length)
{
	return (lz4_run_cost(literals) + LZ4_OFFSET_BYTES +
	    lz4_extension_bytes(length - LZ4_MIN_MATCH));
}

/*
 * The least final run a block needs after a match of length bytes: five
 * literals, and enough that the match starts at least 12 bytes before the
 * block's end.
 */
static size_t
lz4_final_needed(size_t length)
{
	if (length + LZ4_END_LITERALS >= LZ4_END_MATCH_DISTANCE)
		return (LZ4_END_LITERALS);
	return (LZ4_END_MATCH_DISTANCE - length);
}

static size_t
lz4_room(const lz4_encode_t *encode)
{
	return (encode->capacity - encode->produced);
}

/* The four bytes, little-endian, times the multiplier, top 12 bits. */
static uint32_t
lz4_hash(const uint8_t *group)
{
	uint64_t product;
	uint32_t value;

	value = (uint32_t)group[0] | (uint32_t)group[1] << 8 |
	    (uint32_t)group[2] << 16 | (uint32_t)group[3] << 24;
	product = ((uint64_t)value * LZ4_HASH_MULTIPLIER) & UINT32_MAX;
	return ((uint32_t)(product >> (32 - LZ4_HASH_BITS)));
}

/*
 * May a match start at position? Only at least 12 bytes before the
 * input's end. Every position the scan examines or records passes this,
 * which also keeps its four hashed bytes inside the input.
 */
static bool
lz4_may_start_match(const lz4_encode_t *encode, size_t position)
{
	return (encode->in_len >= LZ4_END_MATCH_DISTANCE &&
	    position <= encode->in_len - LZ4_END_MATCH_DISTANCE);
}

/*
 * Look position up in the table and record it there. The candidate is a
 * match when it lies before position, within the largest offset, and its
 * four bytes are equal; a zero entry is position 0, judged the same way.
 * The match is extended while the bytes agree, ending no later than five
 * bytes before the input's end, which is at least 7 bytes from position.
 */
static bool
lz4_find_match(lz4_encode_t *encode, size_t position, lz4_match_t *match)
{
	const uint8_t *in = encode->in;
	uint32_t *entry;
	size_t candidate, length, longest;

	entry = &encode->table[lz4_hash(in + position)];
	candidate = *entry;
	*entry = (uint32_t)position;
	if (candidate >= position || position - candidate > LZ4_MAX_DISTANCE)
		return (false);
	if (bcmp(in + candidate, in + position, LZ4_GROUP_BYTES) != 0)
		return (false);

	longest = encode->in_len - LZ4_END_LITERALS - position;
	length = LZ4_GROUP_BYTES;
	while (length < longest &&
	    in[candidate + length] == in[position + length])
		length++;
	match->distance = position - candidate;
	match->length = length;
	return (true);
}

/* Record the positions of [from, to) that may start a match. */
static void
lz4_record_positions(lz4_encode_t *encode, size_t from, size_t to)
{
	for (size_t p = from; p < to && lz4_may_start_match(encode, p); p++)
		encode->table[lz4_hash(encode->in + p)] = (uint32_t)p;
}

/*
 * Does the sequence of the pending literals and the match at position fit,
 * together with the final run the block then needs? The sum is the cost
 * of a block of at most in_len bytes, so it is at most the bound and does
 * not overflow.
 */
static bool
lz4_sequence_fits(const lz4_encode_t *encode, size_t position,
    const lz4_match_t *match)
{
	size_t cost;

	cost = lz4_sequence_cost(position - encode->covered, match->length) +
	    lz4_run_cost(lz4_final_needed(match->length));
	return (cost <= lz4_room(encode));
}

/*
 * The longest final run that fits the room, from the input left after the
 * written sequences. Starting from the room itself, each step down sheds
 * one literal and at most one extension byte, so the loop runs at most
 * 1 + x(room) times, x being the extension bytes of the room's value.
 * The fit tests leave room for the run the last match needs, so the
 * result is at least that; lz4_encode_block checks it.
 */
static size_t
lz4_final_fit(const lz4_encode_t *encode)
{
	size_t literals, room;

	room = lz4_room(encode);
	literals = encode->in_len - encode->covered;
	if (literals > room)
		literals = room;
	while (literals > 0 && lz4_run_cost(literals) > room)
		literals--;
	return (literals);
}

/* Write one byte, or record the refusal of a byte past the capacity. */
static void
lz4_put(lz4_encode_t *encode, uint8_t byte)
{
	if (encode->produced == encode->capacity) {
		encode->overflowed = true;
		return;
	}
	encode->out[encode->produced++] = byte;
}

/* A length field's extension: 255 per full step past 15, then the rest. */
static void
lz4_put_extension(lz4_encode_t *encode, size_t value)
{
	size_t rest;

	if (value < LZ4_FIELD_EXTENDED)
		return;
	for (rest = value - LZ4_FIELD_EXTENDED; rest >= LZ4_EXTENSION_MORE;
	    rest -= LZ4_EXTENSION_MORE)
		lz4_put(encode, LZ4_EXTENSION_MORE);
	lz4_put(encode, (uint8_t)rest);
}

/*
 * Write a token with the given literal count and match field value, the
 * literal extension and the next literals of the input. A final run ends
 * here with a match field of 0; a sequence continues with its offset and
 * match extension, written by lz4_put_sequence.
 */
static void
lz4_put_run(lz4_encode_t *encode, size_t literals, size_t match_value)
{
	unsigned literal_field, match_field;

	literal_field = literals < LZ4_FIELD_EXTENDED ? (unsigned)literals :
	    LZ4_FIELD_EXTENDED;
	match_field = match_value < LZ4_FIELD_EXTENDED ?
	    (unsigned)match_value : LZ4_FIELD_EXTENDED;
	lz4_put(encode, (uint8_t)(literal_field << LZ4_FIELD_BITS |
	    match_field));
	lz4_put_extension(encode, literals);
	for (size_t i = 0; i < literals; i++)
		lz4_put(encode, encode->in[encode->covered + i]);
	encode->covered += literals;
}

/* Write the pending literals before position and the match there. */
static void
lz4_put_sequence(lz4_encode_t *encode, size_t position,
    const lz4_match_t *match)
{
	size_t match_value;

	match_value = match->length - LZ4_MIN_MATCH;
	lz4_put_run(encode, position - encode->covered, match_value);
	for (unsigned i = 0; i < LZ4_OFFSET_BYTES; i++)
		lz4_put(encode, (uint8_t)(match->distance >> (CHAR_BIT * i)));
	lz4_put_extension(encode, match_value);
	encode->covered += match->length;
	encode->final_needed = lz4_final_needed(match->length);
}

/*
 * Scan, writing each match as a sequence while it fits with the final run
 * it requires, then write the longest final run that fits. False when the
 * final run falls short of what the last match needs, which the fit tests
 * rule out: a block breaking the end conditions is never returned.
 */
static bool
lz4_encode_block(lz4_encode_t *encode)
{
	lz4_match_t match;
	size_t final, position;

	bzero(encode->table, LZ4_HASH_ENTRIES * sizeof(encode->table[0]));
	position = 0;
	while (lz4_may_start_match(encode, position)) {
		/*
		 * Pending literals that do not fit even as the final run
		 * leave no room for a sequence carrying them: stop, before
		 * scanning input the block cannot reach.
		 */
		if (lz4_run_cost(position - encode->covered) >
		    lz4_room(encode))
			break;
		if (!lz4_find_match(encode, position, &match)) {
			position++;
			continue;
		}
		if (!lz4_sequence_fits(encode, position, &match))
			break;
		lz4_put_sequence(encode, position, &match);
		lz4_record_positions(encode, position + 1,
		    position + match.length);
		position += match.length;
	}
	final = lz4_final_fit(encode);
	if (final < encode->final_needed)
		return (false);
	lz4_put_run(encode, final, 0);
	return (true);
}

static fsimg_status_t
lz4_encode_check(const uint8_t *in, size_t in_len, const uint8_t *out,
    size_t capacity, fsimg_error_t *error)
{
	if ((in == NULL && in_len != 0) || (out == NULL && capacity != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lz4: invalid buffer arguments"));
	if (fsimg_lz4_encode_bound(in_len) == SIZE_MAX)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lz4: input too long for the encoder"));
	return (FSIMG_OK);
}

/* Encode a block of a prefix of the input into out[0..capacity). */
static fsimg_status_t
lz4_encode_run(fsimg_lz4_encoder_t *encoder, const uint8_t *in,
    size_t in_len, uint8_t *out, size_t capacity, lz4_encode_t *encode,
    fsimg_error_t *error)
{
	*encode = (lz4_encode_t){ .in = in, .in_len = in_len,
	    .table = encoder->table, .out = out, .capacity = capacity };
	if (!lz4_encode_block(encode))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "lz4: encoder final run too short"));
	if (encode->overflowed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "lz4: encoder output passed its capacity"));
	return (FSIMG_OK);
}

size_t
fsimg_lz4_encode_bound(size_t in_len)
{
	uint64_t bound, length;

	length = in_len;
	if (length > LZ4_ENCODE_MAX_INPUT)
		return (SIZE_MAX);
	bound = length + length / LZ4_EXTENSION_MORE + LZ4_TOKEN_BYTES + 1;
#if SIZE_MAX < UINT64_MAX
	/* A 32-bit size_t cannot hold the bound of the longest inputs. */
	if (bound >= SIZE_MAX)
		return (SIZE_MAX);
#endif
	return ((size_t)bound);
}

fsimg_status_t
fsimg_lz4_encoder_new(fsimg_ctx_t *ctx, fsimg_lz4_encoder_t **out,
    fsimg_error_t *error)
{
	fsimg_lz4_encoder_t *encoder;

	*out = NULL;
	encoder = fsimg_alloc(ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	*out = encoder;
	return (FSIMG_OK);
}

void
fsimg_lz4_encoder_free(fsimg_lz4_encoder_t *encoder)
{
	fsimg_dealloc(encoder);
}

fsimg_status_t
fsimg_lz4_encode(fsimg_lz4_encoder_t *encoder, const uint8_t *in,
    size_t in_len, uint8_t *out, size_t capacity, size_t *out_len,
    fsimg_error_t *error)
{
	lz4_encode_t encode;
	fsimg_status_t status;

	*out_len = 0;
	status = lz4_encode_check(in, in_len, out, capacity, error);
	if (status != FSIMG_OK)
		return (status);
	if (capacity < fsimg_lz4_encode_bound(in_len))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lz4: output smaller than the bound"));

	status = lz4_encode_run(encoder, in, in_len, out, capacity, &encode,
	    error);
	if (status != FSIMG_OK)
		return (status);
	/* At the bound the scan never stops early; short is a defect. */
	if (encode.covered != in_len)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "lz4: encoder stopped before the input end"));
	*out_len = encode.produced;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_lz4_encode_prefix(fsimg_lz4_encoder_t *encoder, const uint8_t *in,
    size_t in_len, uint8_t *out, size_t capacity, fsimg_codec_step_t *step,
    fsimg_error_t *error)
{
	lz4_encode_t encode;
	fsimg_status_t status;

	bzero(step, sizeof(*step));
	status = lz4_encode_check(in, in_len, out, capacity, error);
	if (status != FSIMG_OK)
		return (status);
	/* Even the empty block is one token. */
	if (capacity < LZ4_TOKEN_BYTES)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "lz4: output smaller than one token"));

	status = lz4_encode_run(encoder, in, in_len, out, capacity, &encode,
	    error);
	if (status != FSIMG_OK)
		return (status);
	step->consumed = encode.covered;
	step->produced = encode.produced;
	step->done = encode.covered == in_len;
	return (FSIMG_OK);
}
