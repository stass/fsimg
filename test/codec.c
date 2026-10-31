/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "codec.h"
#include "fsimg.h"
#include "internal.h"
#include "test.h"

/*
 * LZO1X and NRV2B decoder tests.
 *
 * Every stream is written by hand from the format tables: either a worked
 * stream that the liblzo2 and libucl oracles decoded (hex below, with the
 * oracle's output), or a stream built by one of the two stream writers in
 * this file. A writer encodes each instruction from its definition and
 * records what the stream must decode to: literals append their bytes and a
 * match appends, one byte at a time, a copy of the bytes it names. The
 * writers share nothing with lib/codec/; each is first checked to reproduce
 * the worked streams byte for byte, and test/codec-integration.sh runs
 * every stream they write through the oracles too (--emit-cases).
 *
 * Every valid stream goes through one battery. It is decoded whole, under
 * a limit or capacity equal to its output; every proper prefix is decoded
 * and must be unterminated; and an LZO1X stream is also decoded split at
 * every input byte, through a one-byte output window and through the
 * smallest history the contract allows, with identical results. Every
 * output byte beyond what a call reports producing must still hold the
 * poison the test filled it with.
 */
#define POISON			0xa5u
#define CODE_BYTES		(256u * 1024u)
#define PLAIN_BYTES		(256u * 1024u)
#define LZO1X_HISTORY		49151u	/* the farthest LZO1X distance */
#define FULL_CHECK_BYTES	4096u	/* streams this short are cut everywhere */
#define SAMPLE_STRIDE		4093u	/* cut points of longer streams */
#define SAMPLE_TAIL		64u	/* ... which are also cut in their tail */
#define NRV2B_MAX_INPUT		65535u
#define NRV2B_MAX_CAPACITY	65536u
#define NRV2B_END_HIGH		0x1000002u
#define NRV2B_BONUS_OFFSET	0xd00u
#define TOOL_PIECE_BYTES	65536u	/* --decode: input piece and window */

/*
 * A stream under construction: its compressed bytes, the output it must
 * decode to, and the writer state each format needs.
 */
typedef struct stream {
	uint8_t		*code;
	uint8_t		*plain;
	const uint8_t	*literal_text;	/* next literals, before the pattern */
	size_t		 literal_text_length;
	size_t		 code_length;
	size_t		 plain_length;
	/*
	 * Marks of the last instruction written: the stream offset just past
	 * the byte that completed its length and the one that completed its
	 * distance, and the output length before it.
	 */
	size_t		 length_end;
	size_t		 distance_end;
	size_t		 plain_before;
	size_t		 control_index;	/* NRV2B: the control byte taking bits */
	uint32_t	 last_offset;	/* NRV2B: the offset a reuse repeats */
	unsigned	 control_free;	/* NRV2B: bits left in that byte */
	unsigned	 literal_state;	/* LZO1X: literals since the match, max 4 */
	bool		 valid;		/* every match names bytes produced */
} stream_t;

/* What a decode did, summed over all its calls. */
typedef struct codec_run {
	fsimg_error_t	 error;
	uint8_t		*output;
	size_t		 consumed;
	size_t		 produced;
	size_t		 calls;
	fsimg_status_t	 status;
	bool		 done;
} codec_run_t;

/*
 * How the test plays the caller of the resumable LZO1X decoder. Input is
 * presented up to split first (when split is nonzero), then up to the end,
 * at most piece bytes per call; each call writes at most capacity bytes,
 * and between calls the caller keeps the last keep bytes as history.
 */
typedef struct lzo_plan {
	uint64_t	 limit;
	size_t		 split;
	size_t		 piece;
	size_t		 capacity;
	size_t		 keep;
} lzo_plan_t;

#define FUZZ_STREAM_BYTES	65536u	/* fuzz input ceiling, less its header */

static const char *emit_directory;
static unsigned emit_sequence;
static bool emit_seeds;

static void *
xmalloc(size_t size)
{
	void *p;

	p = malloc(size == 0 ? 1 : size);
	CHECK(p != NULL);
	return (p);
}

/*
 * The literal pattern: a multiplicative hash of the output position, so a
 * copy from any wrong distance differs from the right one within a few
 * bytes.
 */
static uint8_t
pattern_byte(size_t position)
{
	return ((uint8_t)(((uint32_t)position * 2654435761u) >> 24));
}

static bool
poison_intact(const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (bytes[i] != POISON)
			return (false);
	return (true);
}

/*
 * Parse space-separated hex bytes into bytes[0..*length).
 */
static void
parse_hex(const char *hex, uint8_t *bytes, size_t capacity, size_t *length)
{
	unsigned value;
	int used;

	*length = 0;
	while (*hex != '\0') {
		CHECK(sscanf(hex, " %2x%n", &value, &used) == 1);
		CHECK(*length < capacity);
		bytes[(*length)++] = (uint8_t)value;
		hex += used;
		while (*hex == ' ')
			hex++;
	}
}

/*
 * Stream writing shared by both formats.
 */

static stream_t *
stream_new(void)
{
	stream_t *s;

	s = xmalloc(sizeof(*s));
	bzero(s, sizeof(*s));
	s->code = xmalloc(CODE_BYTES);
	s->plain = xmalloc(PLAIN_BYTES);
	s->last_offset = 1;
	s->valid = true;
	return (s);
}

static void
stream_free(stream_t *s)
{
	free(s->code);
	free(s->plain);
	free(s);
}

static void
put_code(stream_t *s, uint8_t byte)
{
	CHECK(s->code_length < CODE_BYTES);
	s->code[s->code_length++] = byte;
}

static void
put_code_hex(stream_t *s, const char *hex)
{
	uint8_t bytes[64];
	size_t length;

	parse_hex(hex, bytes, sizeof(bytes), &length);
	for (size_t i = 0; i < length; i++)
		put_code(s, bytes[i]);
}

/* Make the next literals the given bytes; the pattern follows them. */
static void
use_literals(stream_t *s, const void *text, size_t length)
{
	s->literal_text = text;
	s->literal_text_length = length;
}

static uint8_t
next_literal(stream_t *s)
{
	uint8_t byte;

	if (s->literal_text_length > 0) {
		byte = *s->literal_text++;
		s->literal_text_length--;
		return (byte);
	}
	return (pattern_byte(s->plain_length));
}

static void
plain_append(stream_t *s, uint8_t byte)
{
	CHECK(s->plain_length < PLAIN_BYTES);
	s->plain[s->plain_length++] = byte;
}

/*
 * What a match means: copy length bytes, one at a time, from distance
 * bytes back, so a distance shorter than the length repeats a run. A
 * distance beyond the output marks the stream invalid; it decodes no
 * further.
 */
static void
plain_copy(stream_t *s, size_t distance, size_t length)
{
	if (!s->valid || distance == 0 || distance > s->plain_length) {
		s->valid = false;
		return;
	}
	for (size_t i = 0; i < length; i++)
		plain_append(s, s->plain[s->plain_length - distance]);
}

/*
 * The LZO1X stream writer, from the instruction table: an instruction byte
 * whose top bits select the class, its length extension, its distance
 * operand, then up to three carried literals.
 */

/* Literals after an instruction; they decide the next literal state. */
static void
lzo_put_literals(stream_t *s, size_t count)
{
	uint8_t byte;

	for (size_t i = 0; i < count; i++) {
		byte = next_literal(s);
		put_code(s, byte);
		plain_append(s, byte);
	}
	s->literal_state = count < 4 ? (unsigned)count : 4;
}

/* A length extension: one zero byte per 255, then the rest, 1 to 255. */
static void
lzo_put_extension(stream_t *s, size_t value)
{
	size_t zeros;

	CHECK(value >= 1);
	zeros = (value - 1) / 255;
	for (size_t i = 0; i < zeros; i++)
		put_code(s, 0);
	put_code(s, (uint8_t)(value - 255 * zeros));
}

/* Record where an instruction starts and where its length is complete. */
static void
lzo_mark_start(stream_t *s)
{
	s->plain_before = s->plain_length;
}

static void
lzo_mark_length(stream_t *s)
{
	s->length_end = s->code_length;
}

/* The first-byte form: byte 17 + n introduces n literals, 1 to 238. */
static void
lzo_first_run(stream_t *s, size_t count)
{
	CHECK(s->code_length == 0 && count >= 1 && count <= 238);
	lzo_mark_start(s);
	put_code(s, (uint8_t)(17 + count));
	lzo_mark_length(s);
	lzo_put_literals(s, count);
}

/* 0000 LLLL after no literals: 3 + L literals, or 18 + extension. */
static void
lzo_long_run(stream_t *s, size_t count)
{
	CHECK(s->literal_state == 0 && count >= 4);
	lzo_mark_start(s);
	if (count <= 18) {
		put_code(s, (uint8_t)(count - 3));
	} else {
		put_code(s, 0);
		lzo_put_extension(s, count - 18);
	}
	lzo_mark_length(s);
	lzo_put_literals(s, count);
}

/* 0000 DDSS after 1 to 3 literals: 2 bytes from 1 + D + 4H, H next. */
static void
lzo_short_match(stream_t *s, size_t distance, unsigned carry)
{
	size_t value;

	CHECK(s->literal_state >= 1 && s->literal_state <= 3);
	CHECK(distance >= 1 && distance <= 1024 && carry <= 3);
	value = distance - 1;
	lzo_mark_start(s);
	put_code(s, (uint8_t)((value & 3) << 2 | carry));
	lzo_mark_length(s);
	put_code(s, (uint8_t)(value >> 2));
	s->distance_end = s->code_length;
	plain_copy(s, distance, 2);
	lzo_put_literals(s, carry);
}

/* 0000 DDSS after 4 or more literals: 3 bytes from 2049 + D + 4H. */
static void
lzo_mid_match(stream_t *s, size_t distance, unsigned carry)
{
	size_t value;

	CHECK(s->literal_state == 4);
	CHECK(distance >= 2049 && distance <= 3072 && carry <= 3);
	value = distance - 2049;
	lzo_mark_start(s);
	put_code(s, (uint8_t)((value & 3) << 2 | carry));
	lzo_mark_length(s);
	put_code(s, (uint8_t)(value >> 2));
	s->distance_end = s->code_length;
	plain_copy(s, distance, 3);
	lzo_put_literals(s, carry);
}

/* 01LD DDSS (3 + L) and 1LLD DDSS (5 + L): from 1 + D + 8H, H next. */
static void
lzo_near_match(stream_t *s, size_t distance, size_t length, unsigned carry)
{
	size_t value;

	CHECK(distance >= 1 && distance <= 2048);
	CHECK(length >= 3 && length <= 8 && carry <= 3);
	value = distance - 1;
	lzo_mark_start(s);
	if (length <= 4)
		put_code(s, (uint8_t)(0x40 | (length - 3) << 5 |
		    (value & 7) << 2 | carry));
	else
		put_code(s, (uint8_t)(0x80 | (length - 5) << 5 |
		    (value & 7) << 2 | carry));
	lzo_mark_length(s);
	put_code(s, (uint8_t)(value >> 3));
	s->distance_end = s->code_length;
	plain_copy(s, distance, length);
	lzo_put_literals(s, carry);
}

/* The 16-bit little-endian word 4D + S of the long and far classes. */
static void
lzo_put_word(stream_t *s, size_t d_field, unsigned carry)
{
	size_t word;

	word = 4 * d_field + carry;
	CHECK(word <= 0xffff);
	put_code(s, (uint8_t)(word & 0xff));
	put_code(s, (uint8_t)(word >> 8));
	s->distance_end = s->code_length;
}

/* 001L LLLL: 2 + L bytes, or 33 + extension, from 1 + D. */
static void
lzo_long_match(stream_t *s, size_t distance, size_t length, unsigned carry)
{
	CHECK(distance >= 1 && distance <= 16384);
	CHECK(length >= 3 && carry <= 3);
	lzo_mark_start(s);
	if (length <= 33) {
		put_code(s, (uint8_t)(0x20 | (length - 2)));
	} else {
		put_code(s, 0x20);
		lzo_put_extension(s, length - 33);
	}
	lzo_mark_length(s);
	lzo_put_word(s, distance - 1, carry);
	plain_copy(s, distance, length);
	lzo_put_literals(s, carry);
}

/* 0001 HLLL: 2 + L bytes, or 9 + extension, from 16384 + 16384H + D. */
static void
lzo_far_match(stream_t *s, size_t distance, size_t length, unsigned carry)
{
	size_t high, value;

	CHECK(distance >= 16385 && distance <= 49151);
	CHECK(length >= 3 && carry <= 3);
	value = distance - 16384;
	high = value >> 14;
	lzo_mark_start(s);
	if (length <= 9) {
		put_code(s, (uint8_t)(0x10 | high << 3 | (length - 2)));
	} else {
		put_code(s, (uint8_t)(0x10 | high << 3));
		lzo_put_extension(s, length - 9);
	}
	lzo_mark_length(s);
	lzo_put_word(s, value & 0x3fff, carry);
	plain_copy(s, distance, length);
	lzo_put_literals(s, carry);
}

/* The canonical end marker: a far match at distance 16384. */
static void
lzo_end(stream_t *s)
{
	put_code_hex(s, "11 00 00");
}

/*
 * An instruction whose meaning shows the literal state the carried
 * literals left: after none, 0000 LLLL is a literal run; after 1 to 3 it
 * is a short match.
 */
static void
lzo_probe_literal_state(stream_t *s)
{
	if (s->literal_state == 0)
		lzo_long_run(s, 4);
	else
		lzo_short_match(s, 1, 0);
}

/*
 * The NRV2B stream writer: control bits taken eight to a byte, most
 * significant first, from a control byte placed in the stream when its
 * first bit is written; data bytes go at the end of the stream at once.
 * That is the order in which a decoder meets them.
 */

static void
nrv2b_put_bit(stream_t *s, unsigned bit)
{
	if (s->control_free == 0) {
		s->control_index = s->code_length;
		put_code(s, 0);
		s->control_free = 8;
	}
	s->control_free--;
	if (bit != 0)
		s->code[s->control_index] |= (uint8_t)(1u << s->control_free);
}

/* The number code: each bit below the top one, then 1 after the last. */
static void
nrv2b_put_number(stream_t *s, uint32_t value)
{
	int top;

	CHECK(value >= 2);
	for (top = 31; (value >> top) == 0; top--)
		;
	for (int bit = top - 1; bit >= 0; bit--) {
		nrv2b_put_bit(s, (value >> bit) & 1);
		nrv2b_put_bit(s, bit == 0);
	}
}

static void
nrv2b_literals(stream_t *s, size_t count)
{
	uint8_t byte;

	for (size_t i = 0; i < count; i++) {
		byte = next_literal(s);
		nrv2b_put_bit(s, 1);
		put_code(s, byte);
		plain_append(s, byte);
	}
}

/*
 * The length part for a match copying count bytes: the field F is the
 * count less one, less one more above offset 0xd00; F of 1 to 3 is two
 * bits, larger F is 00 and the number F - 2.
 */
static void
nrv2b_put_length(stream_t *s, uint32_t offset, size_t count)
{
	size_t field;

	field = count - 1 - (offset > NRV2B_BONUS_OFFSET ? 1 : 0);
	CHECK(count >= 2 && field >= 1);
	if (field <= 3) {
		nrv2b_put_bit(s, field != 1);
		nrv2b_put_bit(s, field != 2);
	} else {
		nrv2b_put_bit(s, 0);
		nrv2b_put_bit(s, 0);
		nrv2b_put_number(s, (uint32_t)(field - 2));
	}
}

/* A match at a new offset: H = raw / 256 + 3, then the data byte raw. */
static void
nrv2b_match(stream_t *s, uint32_t offset, size_t count)
{
	uint32_t raw;

	CHECK(offset >= 1);
	raw = offset - 1;
	nrv2b_put_bit(s, 0);
	nrv2b_put_number(s, (raw >> 8) + 3);
	put_code(s, (uint8_t)(raw & 0xff));
	s->distance_end = s->code_length;
	s->last_offset = offset;
	nrv2b_put_length(s, offset, count);
	plain_copy(s, offset, count);
}

/* A match repeating the last offset: H = 2 and no data byte. */
static void
nrv2b_reuse(stream_t *s, size_t count)
{
	nrv2b_put_bit(s, 0);
	nrv2b_put_number(s, 2);
	s->distance_end = s->code_length;
	nrv2b_put_length(s, s->last_offset, count);
	plain_copy(s, s->last_offset, count);
}

/* The end marker: a match with H = 0x1000002 and the data byte 0xff. */
static void
nrv2b_end(stream_t *s)
{
	nrv2b_put_bit(s, 0);
	nrv2b_put_number(s, NRV2B_END_HIGH);
	put_code(s, 0xff);
}

/*
 * Decoding drivers. Each output buffer is poisoned first, and every byte a
 * call did not report producing must keep the poison.
 */

static void
run_init(codec_run_t *run)
{
	bzero(run, sizeof(*run));
	run->output = xmalloc(PLAIN_BYTES);
}

static void
run_free(codec_run_t *run)
{
	free(run->output);
	run->output = NULL;
}

static size_t
next_piece_end(const lzo_plan_t *plan, size_t position, size_t length)
{
	size_t end;

	end = position < plan->split ? plan->split : length;
	if (end - position > plan->piece)
		end = position + plan->piece;
	return (end);
}

/* Keep the last keep bytes of window[0..filled) at its front. */
static size_t
keep_history(uint8_t *window, size_t filled, size_t keep)
{
	if (filled <= keep)
		return (filled);
	bcopy(window + (filled - keep), window, keep);
	return (keep);
}

/*
 * Play the caller of the resumable decoder: present the input in pieces,
 * drain the window after each call and keep the planned history. The
 * driver ends at an error or when the stream is done and all input has
 * been presented, so bytes after the end marker reach the decoder.
 */
static void
decode_lzo(const uint8_t *code, size_t length, const lzo_plan_t *plan,
    codec_run_t *run)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	uint8_t *window;
	size_t history, piece_end, position, window_bytes;
	bool final;

	run_init(run);
	window_bytes = plan->keep + plan->capacity;
	window = xmalloc(window_bytes);
	memset(window, POISON, window_bytes);
	fsimg_lzo1x_decoder_init(&decoder, plan->limit);
	history = 0;
	position = 0;
	for (;;) {
		piece_end = next_piece_end(plan, position, length);
		final = piece_end == length;
		run->status = fsimg_lzo1x_decode(&decoder, code + position,
		    piece_end - position, final, window, history,
		    plan->capacity, &step, &run->error);
		run->calls++;
		CHECK(step.consumed <= piece_end - position);
		CHECK(step.produced <= plan->capacity);
		CHECK(poison_intact(window + history + step.produced,
		    plan->capacity - step.produced));
		CHECK(run->produced + step.produced <= PLAIN_BYTES);
		bcopy(window + history, run->output + run->produced,
		    step.produced);
		position += step.consumed;
		run->consumed += step.consumed;
		run->produced += step.produced;
		if (run->status != FSIMG_OK)
			break;
		/* Bytes left after the end marker must be refused. */
		CHECK(!run->done);
		run->done = step.done;
		if (step.done && position == length)
			break;
		CHECK(step.done || step.consumed > 0 || step.produced > 0);
		history = keep_history(window, history + step.produced,
		    plan->keep);
		memset(window + history, POISON, plan->capacity);
	}
	free(window);
}

/* One call with a window the size of the limit and no history. */
static lzo_plan_t
lzo_one_call(uint64_t limit)
{
	lzo_plan_t plan = {
		.limit = limit, .piece = SIZE_MAX, .capacity = (size_t)limit
	};

	return (plan);
}

static void
decode_nrv2b(const uint8_t *code, size_t length, size_t capacity,
    codec_run_t *run)
{
	fsimg_codec_step_t step;
	uint8_t *out;

	run_init(run);
	out = xmalloc(capacity);
	memset(out, POISON, capacity);
	run->status = fsimg_nrv2b_decode(code, length, out, capacity, &step,
	    &run->error);
	run->calls = 1;
	CHECK(step.consumed <= length && step.produced <= capacity);
	CHECK(poison_intact(out + step.produced, capacity - step.produced));
	CHECK(step.produced <= PLAIN_BYTES);
	bcopy(out, run->output, step.produced);
	run->consumed = step.consumed;
	run->produced = step.produced;
	run->done = step.done;
	free(out);
}

/*
 * Expectations. A failed one names the case and what differed.
 */

static void
expect_decoded(const char *name, const codec_run_t *run,
    const uint8_t *plain, size_t plain_length, size_t code_length)
{
	if (run->status == FSIMG_OK && run->done &&
	    run->consumed == code_length && run->produced == plain_length &&
	    memcmp(run->output, plain, plain_length) == 0)
		return;
	fprintf(stderr, "%s: status %d (%s at %" PRIu64 ") done %d consumed "
	    "%zu of %zu produced %zu of %zu\n", name, (int)run->status,
	    run->error.message, run->error.offset, (int)run->done,
	    run->consumed, code_length, run->produced, plain_length);
	if (run->status == FSIMG_OK && run->produced == plain_length)
		fprintf(stderr, "%s: the output bytes differ\n", name);
	exit(1);
}

static void
expect_failure(const char *name, const codec_run_t *run,
    fsimg_status_t status, const char *check, uint64_t offset)
{
	if (run->status == status &&
	    strstr(run->error.message, check) != NULL &&
	    run->error.offset_valid && run->error.offset == offset)
		return;
	fprintf(stderr, "%s: expected status %d \"%s\" at %" PRIu64 ", got "
	    "%d \"%s\" at %" PRIu64 "\n", name, (int)status, check, offset,
	    (int)run->status, run->error.message, run->error.offset);
	exit(1);
}

static void
expect_same_run(const char *name, const char *how, const codec_run_t *run,
    const codec_run_t *reference)
{
	if (run->status == reference->status &&
	    run->done == reference->done &&
	    run->consumed == reference->consumed &&
	    run->produced == reference->produced &&
	    memcmp(run->output, reference->output, run->produced) == 0)
		return;
	fprintf(stderr, "%s: %s differs from one call: status %d/%d consumed "
	    "%zu/%zu produced %zu/%zu\n", name, how, (int)run->status,
	    (int)reference->status, run->consumed, reference->consumed,
	    run->produced, reference->produced);
	exit(1);
}

/*
 * --emit-cases: every stream the batteries check is also written out, for
 * test/codec-integration.sh to run through the oracles. NAME.in is the
 * stream, NAME.limit the output bound in decimal, and NAME.out the output
 * an accepted stream decodes to; a stream without NAME.out is rejected.
 */
static void
write_file(const char *path, const void *bytes, size_t length)
{
	FILE *file;

	file = fopen(path, "wb");
	if (file == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		exit(2);
	}
	if (fwrite(bytes, 1, length, file) != length || fclose(file) != 0) {
		fprintf(stderr, "%s: write failed\n", path);
		exit(2);
	}
}

/*
 * --seed DIR writes the same streams as fuzz seeds instead, in the format
 * fuzz/codec.c's header comment defines: the codec byte, the bound (16 bytes of LZO1X
 * limit per unit, or the NRV2B capacity with 65536 as 65535 plus a flag),
 * a call-plan byte, then the stream. A stream longer than the fuzz input
 * ceiling is left out.
 */
static void
emit_seed(const char *codec, const uint8_t *code, size_t code_length,
    uint64_t limit)
{
	char path[4096];
	uint8_t *bytes;
	uint64_t bound;
	bool nrv2b;

	if (code_length > FUZZ_STREAM_BYTES)
		return;
	nrv2b = strcmp(codec, "nrv2b") == 0;
	bound = nrv2b ? limit : (limit + 15) / 16;
	if (bound > 0xffff + (nrv2b ? 1 : 0))
		return;
	bytes = xmalloc(4 + code_length);
	bytes[0] = nrv2b ? 1 : 0;
	if (bound > 0xffff) {
		bytes[0] |= 2;
		bound--;
	}
	bytes[1] = (uint8_t)(bound & 0xff);
	bytes[2] = (uint8_t)(bound >> 8);
	bytes[3] = (uint8_t)(0x53 + emit_sequence);
	bcopy(code, bytes + 4, code_length);
	snprintf(path, sizeof(path), "%s/%s-%05u.seed", emit_directory, codec,
	    emit_sequence);
	write_file(path, bytes, 4 + code_length);
	free(bytes);
}

static void
emit_case(const char *codec, const uint8_t *code, size_t code_length,
    uint64_t limit, const uint8_t *plain, size_t plain_length)
{
	char path[4096], text[32];

	if (emit_directory == NULL)
		return;
	emit_sequence++;
	if (emit_seeds) {
		emit_seed(codec, code, code_length, limit);
		return;
	}
	snprintf(path, sizeof(path), "%s/%s-%05u.in", emit_directory, codec,
	    emit_sequence);
	write_file(path, code, code_length);
	snprintf(path, sizeof(path), "%s/%s-%05u.limit", emit_directory,
	    codec, emit_sequence);
	snprintf(text, sizeof(text), "%" PRIu64 "\n", limit);
	write_file(path, text, strlen(text));
	if (plain == NULL)
		return;
	snprintf(path, sizeof(path), "%s/%s-%05u.out", emit_directory, codec,
	    emit_sequence);
	write_file(path, plain, plain_length);
}

/*
 * The cut points of a stream: all of them for a short stream; for a long
 * one, a stride through it and every point of its tail, where the
 * instructions under test are.
 */
static bool
is_cut_point(size_t point, size_t length)
{
	if (length <= FULL_CHECK_BYTES || point < 8)
		return (true);
	if (length - point <= SAMPLE_TAIL)
		return (true);
	return (point % SAMPLE_STRIDE == 0);
}

/*
 * The LZO1X battery for a valid stream, under a limit equal to its output.
 */
static void
check_lzo_valid(const char *name, const stream_t *s)
{
	codec_run_t reference, run;
	lzo_plan_t plan;
	char how[64];

	CHECK(s->valid);
	emit_case("lzo", s->code, s->code_length, s->plain_length, s->plain,
	    s->plain_length);
	plan = lzo_one_call(s->plain_length);
	decode_lzo(s->code, s->code_length, &plan, &reference);
	expect_decoded(name, &reference, s->plain, s->plain_length,
	    s->code_length);

	/* Every proper prefix, given as the whole stream, is unterminated. */
	for (size_t cut = 0; cut < s->code_length; cut++) {
		if (!is_cut_point(cut, s->code_length))
			continue;
		decode_lzo(s->code, cut, &plan, &run);
		snprintf(how, sizeof(how), "%s cut at %zu", name, cut);
		expect_failure(how, &run, FSIMG_ECORRUPT, "unterminated", cut);
		run_free(&run);
	}

	/* A prefix with more to come decodes on when the rest arrives. */
	plan = (lzo_plan_t){ .limit = s->plain_length, .piece = SIZE_MAX,
	    .capacity = s->plain_length == 0 ? 1 : s->plain_length,
	    .keep = LZO1X_HISTORY };
	for (size_t split = 1; split < s->code_length; split++) {
		if (!is_cut_point(split, s->code_length))
			continue;
		plan.split = split;
		decode_lzo(s->code, s->code_length, &plan, &run);
		snprintf(how, sizeof(how), "split at %zu", split);
		expect_same_run(name, how, &run, &reference);
		run_free(&run);
	}

	/* One output byte per call. */
	plan = (lzo_plan_t){ .limit = s->plain_length, .piece = SIZE_MAX,
	    .capacity = 1, .keep = LZO1X_HISTORY };
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_same_run(name, "capacity 1", &run, &reference);
	run_free(&run);

	/*
	 * The smallest history the contract allows, 49151 bytes, behind a
	 * seven-byte window fed three input bytes at a time, so that calls
	 * stop at both edges throughout the stream. For a stream shorter
	 * than 49151 bytes of output the history is all of it.
	 */
	plan = (lzo_plan_t){ .limit = s->plain_length, .piece = 3,
	    .capacity = 7, .keep = LZO1X_HISTORY };
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_same_run(name, "history 49151", &run, &reference);
	run_free(&run);
	run_free(&reference);
}

/* A stream the decoder must refuse, under the given limit, in one call. */
static void
check_lzo_rejected(const char *name, const stream_t *s, uint64_t limit,
    const char *check, uint64_t offset)
{
	codec_run_t run;
	lzo_plan_t plan;

	emit_case("lzo", s->code, s->code_length, limit, NULL, 0);
	plan = lzo_one_call(limit);
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_failure(name, &run, FSIMG_ECORRUPT, check, offset);
	run_free(&run);
}

/*
 * The NRV2B battery for a valid stream, under a capacity equal to its
 * output: decoded whole, and every proper prefix unterminated.
 */
static void
check_nrv2b_valid(const char *name, const stream_t *s)
{
	codec_run_t run;
	char how[64];

	CHECK(s->valid);
	emit_case("nrv2b", s->code, s->code_length, s->plain_length, s->plain,
	    s->plain_length);
	decode_nrv2b(s->code, s->code_length, s->plain_length, &run);
	expect_decoded(name, &run, s->plain, s->plain_length, s->code_length);
	run_free(&run);
	for (size_t cut = 0; cut < s->code_length; cut++) {
		if (!is_cut_point(cut, s->code_length))
			continue;
		decode_nrv2b(s->code, cut, s->plain_length, &run);
		snprintf(how, sizeof(how), "%s cut at %zu", name, cut);
		expect_failure(how, &run, FSIMG_ECORRUPT, "unterminated", cut);
		run_free(&run);
	}
}

static void
check_nrv2b_rejected(const char *name, const stream_t *s, size_t capacity,
    const char *check, uint64_t offset)
{
	codec_run_t run;

	emit_case("nrv2b", s->code, s->code_length, capacity, NULL, 0);
	decode_nrv2b(s->code, s->code_length, capacity, &run);
	expect_failure(name, &run, FSIMG_ECORRUPT, check, offset);
	run_free(&run);
}

/*
 * The checks themselves are live: the poison check sees one written byte,
 * and a long stream is cut at a sample of points, not at every one.
 */
static void
test_checks_are_live(void)
{
	uint8_t bytes[16];

	memset(bytes, POISON, sizeof(bytes));
	CHECK(poison_intact(bytes, sizeof(bytes)));
	bytes[11] = 0;
	CHECK(!poison_intact(bytes, sizeof(bytes)));
	CHECK(!is_cut_point(FULL_CHECK_BYTES + 1, 3 * FULL_CHECK_BYTES));
	CHECK(is_cut_point(3 * FULL_CHECK_BYTES - 1, 3 * FULL_CHECK_BYTES));
}

/*
 * The worked streams: the oracle decoded each to the output listed.
 */
typedef struct worked_stream {
	const char	*name;
	const char	*code;
	const char	*plain;
} worked_stream_t;

static const worked_stream_t lzo_worked[] = {
	{ "lzo end marker alone", "11 00 00", "" },
	{ "lzo one literal", "12 41 11 00 00", "A" },
	{ "lzo short match after one literal", "12 41 00 00 11 00 00", "AAA" },
	{ "lzo short match at 2", "13 41 42 04 00 11 00 00", "ABAB" },
	{ "lzo near match carrying a literal",
	    "15 41 42 43 44 4d 00 45 00 00 11 00 00", "ABCDABCEEE" },
	{ "lzo long match of 3", "15 41 42 43 44 21 0d 00 45 11 00 00",
	    "ABCDABCE" },
	{ "lzo overlapping near match", "15 41 42 43 44 80 00 11 00 00",
	    "ABCDDDDDD" },
};

/* Write a stream from hex and record the output it decodes to. */
static stream_t *
worked_stream(const worked_stream_t *worked)
{
	stream_t *s;

	s = stream_new();
	put_code_hex(s, worked->code);
	for (const char *p = worked->plain; *p != '\0'; p++)
		plain_append(s, (uint8_t)*p);
	return (s);
}

static void
expect_same_code(const char *name, const stream_t *written,
    const stream_t *worked)
{
	if (written->code_length == worked->code_length &&
	    memcmp(written->code, worked->code, worked->code_length) == 0 &&
	    written->plain_length == worked->plain_length &&
	    memcmp(written->plain, worked->plain, worked->plain_length) == 0)
		return;
	fprintf(stderr, "%s: the writer's stream differs from the worked "
	    "stream\n", name);
	exit(1);
}

/*
 * The worked streams decode to the oracle's output, and the LZO1X writer
 * reproduces every one of them, the last two built rather than listed: a
 * long run of 21 at the start, and the oracle's encoding of 300 zeros.
 */
static void
test_lzo_worked_streams(void)
{
	static const uint8_t zeros[32];
	stream_t *s, *w;
	size_t count;

	count = sizeof(lzo_worked) / sizeof(lzo_worked[0]);
	for (size_t i = 0; i < count; i++) {
		s = worked_stream(&lzo_worked[i]);
		check_lzo_valid(lzo_worked[i].name, s);
		stream_free(s);
	}

	/* The writer, instruction by instruction, gives the same bytes. */
	w = stream_new();
	lzo_end(w);
	s = worked_stream(&lzo_worked[0]);
	expect_same_code(lzo_worked[0].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "A", 1);
	lzo_first_run(w, 1);
	lzo_end(w);
	s = worked_stream(&lzo_worked[1]);
	expect_same_code(lzo_worked[1].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "A", 1);
	lzo_first_run(w, 1);
	lzo_short_match(w, 1, 0);
	lzo_end(w);
	s = worked_stream(&lzo_worked[2]);
	expect_same_code(lzo_worked[2].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "AB", 2);
	lzo_first_run(w, 2);
	lzo_short_match(w, 2, 0);
	lzo_end(w);
	s = worked_stream(&lzo_worked[3]);
	expect_same_code(lzo_worked[3].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "ABCDE", 5);
	lzo_first_run(w, 4);
	lzo_near_match(w, 4, 3, 1);
	lzo_short_match(w, 1, 0);
	lzo_end(w);
	s = worked_stream(&lzo_worked[4]);
	expect_same_code(lzo_worked[4].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "ABCDE", 5);
	lzo_first_run(w, 4);
	lzo_long_match(w, 4, 3, 1);
	lzo_end(w);
	s = worked_stream(&lzo_worked[5]);
	expect_same_code(lzo_worked[5].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "ABCD", 4);
	lzo_first_run(w, 4);
	lzo_near_match(w, 1, 5, 0);
	lzo_end(w);
	s = worked_stream(&lzo_worked[6]);
	expect_same_code(lzo_worked[6].name, w, s);
	stream_free(s);
	stream_free(w);

	/* 00 03 and 21 literals: a long run of 18 + 3 at the start. */
	w = stream_new();
	use_literals(w, "abcdefghijklmnopqrstu", 21);
	lzo_long_run(w, 21);
	lzo_end(w);
	CHECK(w->code_length == 2 + 21 + 3);
	CHECK(w->code[0] == 0x00 && w->code[1] == 0x03);
	check_lzo_valid("lzo long run of 21 at the start", w);
	stream_free(w);

	/* 02, five zeros, 20 f3 10 00, 00 01, nineteen zeros, 11 00 00. */
	w = stream_new();
	use_literals(w, zeros, sizeof(zeros));
	lzo_long_run(w, 5);
	lzo_long_match(w, 5, 276, 0);
	lzo_long_run(w, 19);
	lzo_end(w);
	s = stream_new();
	put_code_hex(s, "02 00 00 00 00 00 20 f3 10 00 00 01");
	for (size_t i = 0; i < 19; i++)
		put_code(s, 0);
	put_code_hex(s, "11 00 00");
	for (size_t i = 0; i < 300; i++)
		plain_append(s, 0);
	expect_same_code("lzo 300 zeros", w, s);
	check_lzo_valid("lzo 300 zeros", s);
	stream_free(s);
	stream_free(w);
}

/*
 * Streams the oracle rejects, each paired with the valid stream it was
 * mutated from.
 */
static void
test_lzo_rejected_worked_streams(void)
{
	static const worked_stream_t near = {
		"lzo near match at 4", "15 41 42 43 44 4c 00 11 00 00",
		"ABCDABC"
	};
	stream_t *s;

	/* A mid-distance short match at 2049 after four bytes. */
	s = worked_stream(&near);
	check_lzo_valid(near.name, s);
	stream_free(s);
	s = stream_new();
	put_code_hex(s, "15 41 42 43 44 00 00 11 00 00");
	check_lzo_rejected("lzo mid match at 2049 after 4", s, 100,
	    "distance before the output start", 7);
	stream_free(s);

	/* Distance 3 after two bytes, from the valid distance 2. */
	s = stream_new();
	put_code_hex(s, "13 41 42 08 00 11 00 00");
	check_lzo_rejected("lzo short match at 3 after 2", s, 100,
	    "distance before the output start", 5);
	stream_free(s);

	/* A far match with H = 1 where the end marker was expected. */
	s = stream_new();
	put_code_hex(s, "12 41 19 00 00");
	check_lzo_rejected("lzo far match at 32768", s, 100,
	    "distance before the output start", 5);
	stream_free(s);

	/* A byte after the end marker. */
	s = stream_new();
	put_code_hex(s, "12 41 11 00 00 00");
	check_lzo_rejected("lzo byte after the marker", s, 100,
	    "trailing bytes", 5);
	stream_free(s);

	/*
	 * A first byte of 17 is an ordinary far match, not a version byte:
	 * 11 04 00 names distance 16385 before anything is produced.
	 */
	s = stream_new();
	put_code_hex(s, "11 04 00 11 00 00");
	check_lzo_rejected("lzo first byte 17 as a version", s, 100,
	    "distance before the output start", 3);
	stream_free(s);
}

/*
 * First runs of 1 to 238 literals in the first-byte form, 239 as a long
 * run at the start; after 1 to 3 the literal state makes 0000 DDSS a
 * short match.
 */
static void
test_lzo_first_runs(void)
{
	static const size_t counts[] = { 1, 2, 3, 4, 238 };
	char name[64];
	stream_t *s;

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		s = stream_new();
		lzo_first_run(s, counts[i]);
		if (counts[i] < 4)
			lzo_probe_literal_state(s);
		lzo_end(s);
		snprintf(name, sizeof(name), "lzo first run of %zu", counts[i]);
		check_lzo_valid(name, s);
		stream_free(s);
	}
	s = stream_new();
	lzo_long_run(s, 239);
	lzo_end(s);
	CHECK(s->code[0] == 0x00 && s->code[1] == 239 - 18);
	check_lzo_valid("lzo first run of 239", s);
	stream_free(s);
}

/*
 * Long runs after a match carrying nothing: 4 to 18 in the instruction
 * byte, then with no, one and two zero bytes of extension (19 to 273, 274
 * to 528, 529 to 783).
 */
static void
test_lzo_long_runs(void)
{
	static const size_t counts[] = { 4, 18, 19, 273, 274, 528, 529, 783 };
	char name[64];
	stream_t *s;

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		s = stream_new();
		lzo_first_run(s, 4);
		lzo_near_match(s, 4, 3, 0);
		lzo_long_run(s, counts[i]);
		lzo_end(s);
		snprintf(name, sizeof(name), "lzo long run of %zu", counts[i]);
		check_lzo_valid(name, s);
		stream_free(s);
	}
}

/*
 * Short matches after one, two and three literals, at distances 1 and
 * 1024, carrying 0 to 3 literals; the literal state the carried literals
 * leave decides the next instruction.
 */
static void
test_lzo_short_matches(void)
{
	static const size_t distances[] = { 1, 1024 };
	char name[96];
	stream_t *s;

	for (unsigned state = 1; state <= 3; state++) {
		for (size_t d = 0; d < 2; d++) {
			for (unsigned carry = 0; carry <= 3; carry++) {
				s = stream_new();
				lzo_long_run(s, 1100);
				lzo_near_match(s, 1, 3, state);
				lzo_short_match(s, distances[d], carry);
				lzo_probe_literal_state(s);
				lzo_end(s);
				snprintf(name, sizeof(name), "lzo short match "
				    "after %u at %zu carrying %u", state,
				    distances[d], carry);
				check_lzo_valid(name, s);
				stream_free(s);
			}
		}
	}
}

/*
 * After four or more literals 0000 DDSS is a three-byte match at 2049 to
 * 3072.
 */
static void
test_lzo_mid_matches(void)
{
	static const size_t distances[] = { 2049, 3072 };
	char name[96];
	stream_t *s;

	for (size_t d = 0; d < 2; d++) {
		for (unsigned carry = 0; carry <= 3; carry++) {
			s = stream_new();
			lzo_long_run(s, 3100);
			lzo_mid_match(s, distances[d], carry);
			lzo_probe_literal_state(s);
			lzo_end(s);
			snprintf(name, sizeof(name), "lzo mid match at %zu "
			    "carrying %u", distances[d], carry);
			check_lzo_valid(name, s);
			stream_free(s);
		}
	}
	/* A long run after a match leaves the same state. */
	s = stream_new();
	lzo_first_run(s, 4);
	lzo_long_match(s, 4, 2100, 0);
	lzo_long_run(s, 4);
	lzo_mid_match(s, 2049, 0);
	lzo_end(s);
	check_lzo_valid("lzo mid match after a long run", s);
	stream_free(s);
}

/* Near matches of 3, 4, 5 and 8 at distances 1 and 2048. */
static void
test_lzo_near_matches(void)
{
	static const size_t lengths[] = { 3, 4, 5, 8 };
	static const size_t distances[] = { 1, 2048 };
	char name[96];
	stream_t *s;

	for (size_t n = 0; n < 4; n++) {
		for (size_t d = 0; d < 2; d++) {
			for (unsigned carry = 0; carry <= 3; carry++) {
				s = stream_new();
				lzo_long_run(s, 2100);
				lzo_near_match(s, distances[d], lengths[n],
				    carry);
				lzo_probe_literal_state(s);
				lzo_end(s);
				snprintf(name, sizeof(name), "lzo near match of "
				    "%zu at %zu carrying %u", lengths[n],
				    distances[d], carry);
				check_lzo_valid(name, s);
				stream_free(s);
			}
		}
	}
}

/*
 * Long matches of 3, 33, 34 and 289 at distances 1 and 16384; the
 * carried literals are varied at the farthest distance.
 */
static void
test_lzo_long_matches(void)
{
	static const size_t lengths[] = { 3, 33, 34, 289 };
	static const size_t distances[] = { 1, 16384 };
	char name[96];
	stream_t *s;

	for (size_t n = 0; n < 4; n++) {
		for (size_t d = 0; d < 2; d++) {
			for (unsigned carry = 0; carry <= 3; carry++) {
				if (carry != 0 && (n != 3 || d != 1))
					continue;
				s = stream_new();
				lzo_long_run(s, 16400);
				lzo_long_match(s, distances[d], lengths[n],
				    carry);
				lzo_probe_literal_state(s);
				lzo_end(s);
				snprintf(name, sizeof(name), "lzo long match of "
				    "%zu at %zu carrying %u", lengths[n],
				    distances[d], carry);
				check_lzo_valid(name, s);
				stream_free(s);
			}
		}
	}
}

/*
 * Far matches of 3, 9, 10 and 265 at distances 16385 and 49151, the
 * farthest any LZO1X stream can name; the carried literals are varied at
 * the farthest distance.
 */
static void
test_lzo_far_matches(void)
{
	static const size_t lengths[] = { 3, 9, 10, 265 };
	static const size_t distances[] = { 16385, 49151 };
	char name[96];
	stream_t *s;

	for (size_t n = 0; n < 4; n++) {
		for (size_t d = 0; d < 2; d++) {
			for (unsigned carry = 0; carry <= 3; carry++) {
				if (carry != 0 && (n != 3 || d != 1))
					continue;
				s = stream_new();
				lzo_long_run(s, 49200);
				lzo_far_match(s, distances[d], lengths[n],
				    carry);
				lzo_probe_literal_state(s);
				lzo_end(s);
				snprintf(name, sizeof(name), "lzo far match of "
				    "%zu at %zu carrying %u", lengths[n],
				    distances[d], carry);
				check_lzo_valid(name, s);
				stream_free(s);
			}
		}
	}
	/* 32768, the first distance with H = 1. */
	s = stream_new();
	lzo_long_run(s, 33000);
	lzo_far_match(s, 32768, 4, 0);
	lzo_end(s);
	CHECK(s->code[s->code_length - 6] == 0x1a);
	check_lzo_valid("lzo far match at 32768", s);
	stream_free(s);
}

/*
 * The end marker is any far match at distance 16384: its length and S
 * bits are read and ignored. It is accepted when the output already
 * equals the limit, as every battery's limit makes it.
 */
static void
test_lzo_end_markers(void)
{
	static const char *const markers[] = {
		"11 00 00", "11 01 00", "10 01 00 00", "10 00 05 00 00"
	};
	char name[64];
	stream_t *s;

	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
		s = stream_new();
		lzo_first_run(s, 1);
		put_code_hex(s, markers[i]);
		snprintf(name, sizeof(name), "lzo end marker %s", markers[i]);
		check_lzo_valid(name, s);
		stream_free(s);

		s = stream_new();
		put_code_hex(s, markers[i]);
		snprintf(name, sizeof(name), "lzo empty stream %s", markers[i]);
		check_lzo_valid(name, s);
		stream_free(s);
	}
}

/*
 * One match of each class at the farthest distance the output allows,
 * then the same match one byte farther: corrupt, named at the operand
 * byte that completed the distance.
 */
typedef void (*lzo_prefix_fn_t)(stream_t *);
typedef void (*lzo_match_fn_t)(stream_t *, size_t);

static void prefix_two(stream_t *s) { lzo_first_run(s, 2); }
static void prefix_four(stream_t *s) { lzo_first_run(s, 4); }
static void prefix_2048(stream_t *s) { lzo_long_run(s, 2048); }
static void prefix_16385(stream_t *s) { lzo_long_run(s, 16385); }
static void short_at(stream_t *s, size_t d) { lzo_short_match(s, d, 0); }
static void mid_at(stream_t *s, size_t d) { lzo_mid_match(s, d, 0); }
static void near_at(stream_t *s, size_t d) { lzo_near_match(s, d, 3, 0); }
static void long_at(stream_t *s, size_t d) { lzo_long_match(s, d, 3, 0); }
static void far_at(stream_t *s, size_t d) { lzo_far_match(s, d, 3, 0); }

static void
test_lzo_distance_past_start(void)
{
	static const struct {
		const char	*name;
		lzo_prefix_fn_t	 prefix;
		lzo_match_fn_t	 match;
		size_t		 farthest;
	} cases[] = {
		{ "short", prefix_two, short_at, 2 },
		{ "mid", prefix_2048, mid_at, 0 },
		{ "near", prefix_four, near_at, 4 },
		{ "long", prefix_four, long_at, 4 },
		{ "far", prefix_16385, far_at, 16385 },
	};
	char name[96];
	stream_t *s;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* No valid mid match exists before 2049 bytes. */
		if (cases[i].farthest != 0) {
			s = stream_new();
			cases[i].prefix(s);
			cases[i].match(s, cases[i].farthest);
			lzo_end(s);
			snprintf(name, sizeof(name), "lzo %s match from the "
			    "first byte", cases[i].name);
			check_lzo_valid(name, s);
			stream_free(s);
		}
		s = stream_new();
		cases[i].prefix(s);
		cases[i].match(s, s->plain_length + 1);
		CHECK(!s->valid);
		lzo_end(s);
		snprintf(name, sizeof(name), "lzo %s match before the first "
		    "byte", cases[i].name);
		check_lzo_rejected(name, s, 100000,
		    "distance before the output start", s->distance_end);
		stream_free(s);
	}
}

/*
 * Framing verdicts: a stream whose end marker is replaced by a near match
 * ends unterminated, a byte after the marker is trailing, and a far match
 * with H = 1 in its place reaches before the start.
 */
static void
test_lzo_framing_failures(void)
{
	stream_t *s;
	size_t length;

	s = stream_new();
	lzo_first_run(s, 4);
	lzo_near_match(s, 1, 3, 0);
	length = s->code_length;
	lzo_end(s);
	check_lzo_valid("lzo near match then marker", s);
	s->code_length = length;
	check_lzo_rejected("lzo near match for the marker", s, 100,
	    "unterminated", length);
	lzo_end(s);
	put_code(s, 0x11);
	check_lzo_rejected("lzo byte after the marker", s, 100,
	    "trailing bytes", s->code_length - 1);
	s->code_length = length;
	put_code_hex(s, "19 00 00");
	check_lzo_rejected("lzo far match for the marker", s, 100,
	    "distance before the output start", s->code_length);
	stream_free(s);
}

/*
 * A distance within the stream but beyond the history the caller kept is
 * the caller's error, FSIMG_ESTATE, never a corrupt image. The input is
 * split before the far match, so the call that reads it starts with
 * exactly the kept history and nothing produced.
 */
static void
test_lzo_history_contract(void)
{
	codec_run_t run;
	lzo_plan_t plan;
	stream_t *s;
	size_t match_at;

	s = stream_new();
	lzo_long_run(s, 49200);
	match_at = s->code_length;
	lzo_far_match(s, 49151, 3, 0);
	lzo_end(s);
	plan = (lzo_plan_t){ .limit = s->plain_length, .split = match_at,
	    .piece = SIZE_MAX, .capacity = 4096, .keep = LZO1X_HISTORY };
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_decoded("lzo history 49151", &run, s->plain, s->plain_length,
	    s->code_length);
	run_free(&run);

	plan.keep = LZO1X_HISTORY - 1;
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_failure("lzo history 49150", &run, FSIMG_ESTATE,
	    "kept history", s->distance_end);
	run_free(&run);
	stream_free(s);
}

/*
 * The history is checked again when a call continues a match: a match at
 * distance 4 stops in a full four-byte window, and the next call keeps
 * only two bytes of history. The window sits inside a larger buffer, so a
 * copy from before it would read the guard bytes instead of failing.
 */
static void
test_lzo_history_mid_match(void)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t buffer[32];
	uint8_t *window;
	stream_t *s;

	s = stream_new();
	use_literals(s, "ABCD", 4);
	lzo_first_run(s, 4);
	lzo_near_match(s, 4, 5, 0);
	lzo_end(s);
	memset(buffer, POISON, sizeof(buffer));
	window = buffer + 16;
	fsimg_lzo1x_decoder_init(&decoder, s->plain_length);
	CHECK(fsimg_lzo1x_decode(&decoder, s->code, s->distance_end, false,
	    window, 0, 4, &step, &error) == FSIMG_OK);
	CHECK(step.consumed == s->distance_end && step.produced == 4);
	CHECK(!step.done);

	/* Keep "CD" only; the match still needs "ABCD". */
	bcopy(window + 2, window, 2);
	memset(window + 2, POISON, 8);
	CHECK(fsimg_lzo1x_decode(&decoder, s->code + s->distance_end,
	    s->code_length - s->distance_end, true, window, 2, 8, &step,
	    &error) == FSIMG_ESTATE);
	CHECK(strstr(error.message, "kept history") != NULL);
	CHECK(step.consumed == 0 && step.produced == 0);
	CHECK(poison_intact(window + 2, 8));
	stream_free(s);
}

/*
 * The limit. A stream whose output exactly reaches it decodes (every
 * battery shows that). Under one byte less, the instruction that would
 * pass it is refused the moment its count is known, before any byte of it
 * is written: at the byte completing its length, at a far match's
 * distance word, and for carried literals once their match is copied.
 */
static void
expect_limit_refusal(const char *name, const stream_t *s, uint64_t offset,
    size_t produced)
{
	codec_run_t run;
	lzo_plan_t plan;

	check_lzo_valid(name, s);
	plan = lzo_one_call(s->plain_length - 1);
	decode_lzo(s->code, s->code_length, &plan, &run);
	expect_failure(name, &run, FSIMG_ECORRUPT, "longer than its limit",
	    offset);
	if (run.produced != produced) {
		fprintf(stderr, "%s: %zu bytes written before the refusal, "
		    "expected %zu\n", name, run.produced, produced);
		exit(1);
	}
	run_free(&run);
}

/* Write the last instruction under test, then the end marker. */
static void
end_after_instruction(stream_t *s, size_t *length_end, size_t *before)
{
	*length_end = s->length_end;
	*before = s->plain_before;
	lzo_end(s);
}

static void
test_lzo_limits(void)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t window[16];
	size_t before, carried_before, length_end;
	stream_t *s;

	s = stream_new();
	lzo_first_run(s, 4);
	lzo_near_match(s, 4, 5, 0);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo near match past the limit", s, length_end,
	    before);

	/* The window one byte short of the output stops, not done. */
	memset(window, POISON, sizeof(window));
	fsimg_lzo1x_decoder_init(&decoder, s->plain_length);
	CHECK(fsimg_lzo1x_decode(&decoder, s->code, s->code_length, true,
	    window, 0, s->plain_length - 1, &step, &error) == FSIMG_OK);
	CHECK(step.produced == s->plain_length - 1 && !step.done);
	CHECK(memcmp(window, s->plain, s->plain_length - 1) == 0);
	CHECK(poison_intact(window + step.produced,
	    sizeof(window) - step.produced));
	stream_free(s);

	/* Refused at its extension byte, the 20 known only there. */
	s = stream_new();
	lzo_long_run(s, 20);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo long run past the limit", s, length_end,
	    before);
	stream_free(s);

	s = stream_new();
	lzo_first_run(s, 2);
	lzo_short_match(s, 2, 0);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo short match past the limit", s, length_end,
	    before);
	stream_free(s);

	s = stream_new();
	lzo_long_run(s, 2100);
	lzo_mid_match(s, 2049, 0);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo mid match past the limit", s, length_end,
	    before);
	stream_free(s);

	s = stream_new();
	lzo_first_run(s, 4);
	lzo_long_match(s, 4, 40, 0);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo long match past the limit", s, length_end,
	    before);
	stream_free(s);

	/* A far match is held to the limit only after its distance word. */
	s = stream_new();
	lzo_long_run(s, 16400);
	lzo_far_match(s, 16385, 5, 0);
	lzo_end(s);
	expect_limit_refusal("lzo far match past the limit", s,
	    s->distance_end, 16400);
	stream_free(s);

	/* A first run's count is known at the first byte. */
	s = stream_new();
	lzo_first_run(s, 5);
	end_after_instruction(s, &length_end, &before);
	expect_limit_refusal("lzo first run past the limit", s, length_end,
	    before);
	stream_free(s);

	/* A carried literal, after the near match that just fits. */
	s = stream_new();
	lzo_first_run(s, 4);
	lzo_near_match(s, 4, 3, 1);
	/* Everything but the one carried literal is written first. */
	carried_before = s->plain_length - 1;
	lzo_end(s);
	expect_limit_refusal("lzo carried literal past the limit", s,
	    s->distance_end, carried_before);
	stream_free(s);
}

/*
 * Length claims far beyond the limit, each the beginning of an
 * instruction whose extension would run for megabytes, are refused while
 * the extension accumulates, after a few hundred bytes of it; a far
 * match's length, which waits for its distance word, is held to 2^32 - 1
 * instead. Raising the limit to cover each claim lets the same input
 * decode on until it runs out, which shows that the limit was the check
 * that refused it.
 */

/* A buffer holding prefix and then zeros zero extension bytes. */
static uint8_t *
claim_stream(const char *prefix, size_t zeros, size_t *length)
{
	uint8_t *code;
	size_t prefix_length;

	code = xmalloc(16 + zeros);
	parse_hex(prefix, code, 16, &prefix_length);
	bzero(code + prefix_length, zeros);
	*length = prefix_length + zeros;
	return (code);
}

/* The zero extension bytes after which base + 255 per byte passes bound. */
static size_t
zeros_to_pass(uint64_t base, uint64_t bound)
{
	return ((size_t)((bound - base) / 255 + 1));
}

static void
expect_unfinished(const char *name, const uint8_t *code, size_t length,
    uint64_t limit)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t window[8];

	fsimg_lzo1x_decoder_init(&decoder, limit);
	if (fsimg_lzo1x_decode(&decoder, code, length, false, window, 0,
	    sizeof(window), &step, &error) == FSIMG_OK &&
	    step.consumed == length && !step.done)
		return;
	fprintf(stderr, "%s: the raised limit did not decode on\n", name);
	exit(1);
}

static void
test_lzo_length_claims(void)
{
	const uint64_t ceiling = UINT64_C(0xffffffff);
	codec_run_t run;
	lzo_plan_t plan;
	uint8_t *code;
	size_t length, zeros;

	/*
	 * One literal, then 001 00000: a long match of 33 plus its
	 * extension, under 65536 with 65535 bytes left.
	 */
	zeros = zeros_to_pass(33, 65535);
	code = claim_stream("12 41 20", 70000, &length);
	plan = lzo_one_call(65536);
	decode_lzo(code, length, &plan, &run);
	expect_failure("lzo long match claim", &run, FSIMG_ECORRUPT,
	    "longer than its limit", 3 + zeros);
	CHECK(run.produced == 1 && run.consumed + run.produced < 300);
	run_free(&run);
	expect_unfinished("lzo long match claim", code, length, UINT64_MAX);
	free(code);

	/* 0000 0000 at the start and 1000 zeros: a run of 255018. */
	zeros = zeros_to_pass(18, 65536);
	code = claim_stream("00", 1000, &length);
	plan = lzo_one_call(65536);
	decode_lzo(code, length, &plan, &run);
	expect_failure("lzo run claim", &run, FSIMG_ECORRUPT,
	    "longer than its limit", 1 + zeros);
	CHECK(run.produced == 0);
	run_free(&run);
	expect_unfinished("lzo run claim", code, length, 300000);
	free(code);

	/*
	 * One literal, then 0001 0000: a far match of 9 plus its extension,
	 * under no limit, refused at the ceiling before any distance word.
	 */
	zeros = zeros_to_pass(9, ceiling);
	code = claim_stream("12 41 10", zeros + 2, &length);
	plan = (lzo_plan_t){ .limit = UINT64_MAX, .piece = SIZE_MAX,
	    .capacity = 16 };
	decode_lzo(code, length, &plan, &run);
	expect_failure("lzo far match claim past 2^32 - 1", &run,
	    FSIMG_ECORRUPT, "length out of range", 3 + zeros);
	run_free(&run);
	expect_unfinished("lzo far match claim below 2^32 - 1", code,
	    3 + zeros - 1, UINT64_MAX);
	free(code);
}

/*
 * A decompression bomb is paid for byte by byte: one literal and a long
 * match of 65535 at distance 1, 265 bytes claiming 64 KiB, decode under a
 * 64 KiB limit through 4 KiB windows, and the work charged after each
 * call is exactly consumed plus produced. The work counter refuses the
 * same decode one byte of work short. Under a 32 KiB limit the claim is
 * refused at its length field, for the few bytes of work of its header.
 */
static fsimg_status_t
decode_bomb_charged(const stream_t *s, uint64_t limit, uint64_t max_work,
    codec_run_t *run, uint64_t *work, fsimg_error_t *charge_error)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_options_t options;
	fsimg_session_t session;
	fsimg_status_t status;
	fsimg_ctx_t *ctx;
	uint8_t *window;
	size_t history, position;

	fsimg_options_init(&options);
	options.max_work = max_work;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_session_init(&session, ctx, "codec", UINT64_MAX);
	run_init(run);
	window = xmalloc(LZO1X_HISTORY + 4096);
	fsimg_lzo1x_decoder_init(&decoder, limit);
	history = 0;
	position = 0;
	for (;;) {
		run->status = fsimg_lzo1x_decode(&decoder, s->code + position,
		    s->code_length - position, true, window, history, 4096,
		    &step, &run->error);
		run->calls++;
		status = fsimg_session_charge(&session, 0, 0,
		    step.consumed + step.produced, charge_error);
		position += step.consumed;
		run->consumed += step.consumed;
		bcopy(window + history, run->output + run->produced,
		    step.produced);
		run->produced += step.produced;
		run->done = step.done;
		if (status != FSIMG_OK || run->status != FSIMG_OK || step.done)
			break;
		history = keep_history(window, history + step.produced,
		    LZO1X_HISTORY);
	}
	*work = session.progress.work;
	free(window);
	fsimg_ctx_free(ctx);
	return (status);
}

static void
test_lzo_bomb(void)
{
	fsimg_error_t charge_error;
	codec_run_t run;
	uint64_t work;
	stream_t *s;

	s = stream_new();
	lzo_first_run(s, 1);
	lzo_long_match(s, 1, 65535, 0);
	lzo_end(s);
	CHECK(s->code_length == 265 && s->plain_length == 65536);

	CHECK(decode_bomb_charged(s, 65536, 0, &run, &work, &charge_error) ==
	    FSIMG_OK);
	expect_decoded("lzo bomb", &run, s->plain, s->plain_length,
	    s->code_length);
	CHECK(work == 265 + 65536);
	CHECK(run.calls <= 65536 / 4096 + 2);
	run_free(&run);

	CHECK(decode_bomb_charged(s, 65536, 265 + 65536 - 1, &run, &work,
	    &charge_error) == FSIMG_ELIMIT);
	CHECK(charge_error.resource == FSIMG_RESOURCE_WORK);
	run_free(&run);

	CHECK(decode_bomb_charged(s, 32768, 0, &run, &work, &charge_error) ==
	    FSIMG_OK);
	CHECK(run.status == FSIMG_ECORRUPT && run.produced == 1);
	CHECK(strstr(run.error.message, "longer than its limit") != NULL);
	CHECK(work < 140 && run.calls == 1);
	run_free(&run);
	stream_free(s);
}

/*
 * The interface: invalid buffers, a finished decoder, and calls after the
 * end marker.
 */
static void
test_lzo_interface(void)
{
	static const uint8_t marker[] = { 0x12, 0x41, 0x11, 0x00, 0x00, 0x00 };
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t window[8];

	fsimg_lzo1x_decoder_init(&decoder, 8);
	CHECK(fsimg_lzo1x_decode(&decoder, NULL, 1, false, window, 0, 8,
	    &step, &error) == FSIMG_EINVAL);
	fsimg_lzo1x_decoder_init(&decoder, 8);
	CHECK(fsimg_lzo1x_decode(&decoder, marker, 1, false, NULL, 0, 8,
	    &step, &error) == FSIMG_EINVAL);
	fsimg_lzo1x_decoder_init(&decoder, 8);
	CHECK(fsimg_lzo1x_decode(&decoder, marker, 1, false, window, SIZE_MAX,
	    2, &step, &error) == FSIMG_EINVAL);
	CHECK(fsimg_lzo1x_decode(&decoder, marker, 1, false, window, 0, 8,
	    &step, &error) == FSIMG_ESTATE);
	CHECK(step.consumed == 0 && step.produced == 0);

	/* No input and none promised is an edge, not an error. */
	fsimg_lzo1x_decoder_init(&decoder, 8);
	CHECK(fsimg_lzo1x_decode(&decoder, NULL, 0, false, NULL, 0, 0,
	    &step, &error) == FSIMG_OK);
	CHECK(step.consumed == 0 && step.produced == 0 && !step.done);

	/* After the end marker: no input is done again; any byte trails. */
	CHECK(fsimg_lzo1x_decode(&decoder, marker, 5, false, window, 0, 8,
	    &step, &error) == FSIMG_OK);
	CHECK(step.done && step.consumed == 5 && step.produced == 1);
	CHECK(fsimg_lzo1x_decode(&decoder, NULL, 0, true, window, 1, 7,
	    &step, &error) == FSIMG_OK);
	CHECK(step.done && step.consumed == 0);
	CHECK(fsimg_lzo1x_decode(&decoder, marker + 5, 1, true, window, 1, 7,
	    &step, &error) == FSIMG_ECORRUPT);
	CHECK(strstr(error.message, "trailing") != NULL && error.offset == 5);
	CHECK(step.consumed == 0);
	CHECK(fsimg_lzo1x_decode(&decoder, NULL, 0, true, window, 1, 7,
	    &step, &error) == FSIMG_ESTATE);
}

/*
 * The NRV2B worked streams A to E, and A with the unused low bits of its
 * last control byte set; the writer reproduces each of A to E.
 */
static const worked_stream_t nrv2b_worked[] = {
	{ "nrv2b example A", "c0 41 42 00 00 00 00 01 20 ff", "AB" },
	{ "nrv2b example B", "d9 61 62 01 80 00 00 00 00 02 40 ff",
	    "abababab" },
	{ "nrv2b example C", "ba 61 00 62 50 00 00 00 00 00 48 ff",
	    "aaaabbb" },
	{ "nrv2b example D", "94 78 00 00 00 00 00 12 ff", "xxx" },
	{ "nrv2b example E", "00 00 00 00 00 04 80 ff", "" },
	{ "nrv2b example A with unused bits set",
	    "c0 41 42 00 00 00 00 01 3f ff", "AB" },
};

static void
test_nrv2b_worked_streams(void)
{
	stream_t *s, *w;
	size_t count;

	count = sizeof(nrv2b_worked) / sizeof(nrv2b_worked[0]);
	for (size_t i = 0; i < count; i++) {
		s = worked_stream(&nrv2b_worked[i]);
		check_nrv2b_valid(nrv2b_worked[i].name, s);
		stream_free(s);
	}

	w = stream_new();
	use_literals(w, "AB", 2);
	nrv2b_literals(w, 2);
	nrv2b_end(w);
	s = worked_stream(&nrv2b_worked[0]);
	expect_same_code(nrv2b_worked[0].name, w, s);
	stream_free(s);
	stream_free(w);

	/* Offset 2, F = 5 as 00 then G = 3, six bytes. */
	w = stream_new();
	use_literals(w, "ab", 2);
	nrv2b_literals(w, 2);
	nrv2b_match(w, 2, 6);
	nrv2b_end(w);
	s = worked_stream(&nrv2b_worked[1]);
	expect_same_code(nrv2b_worked[1].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	use_literals(w, "ab", 2);
	nrv2b_literals(w, 1);
	nrv2b_match(w, 1, 3);
	nrv2b_literals(w, 1);
	nrv2b_reuse(w, 2);
	nrv2b_end(w);
	s = worked_stream(&nrv2b_worked[2]);
	expect_same_code(nrv2b_worked[2].name, w, s);
	stream_free(s);
	stream_free(w);

	/* A reuse before any new offset repeats the initial offset 1. */
	w = stream_new();
	use_literals(w, "x", 1);
	nrv2b_literals(w, 1);
	nrv2b_reuse(w, 2);
	nrv2b_end(w);
	s = worked_stream(&nrv2b_worked[3]);
	expect_same_code(nrv2b_worked[3].name, w, s);
	stream_free(s);
	stream_free(w);

	w = stream_new();
	nrv2b_end(w);
	s = worked_stream(&nrv2b_worked[4]);
	expect_same_code(nrv2b_worked[4].name, w, s);
	stream_free(s);
	stream_free(w);
}

/*
 * The NRV2B forms: the length fields, the bonus byte above offset 0xd00
 * (example F), an offset equal to the bytes produced, a match whose
 * offset number ends on the last bit of a control byte, and far offsets.
 */
static void
test_nrv2b_forms(void)
{
	static const size_t near_counts[] = { 2, 3, 4, 5, 6, 66 };
	static const size_t far_counts[] = { 3, 4, 5, 6, 7, 67 };
	char name[96];
	stream_t *s;

	/* F = 1, 2, 3 and G = 2, 3, 63, below and above the bonus. */
	for (size_t i = 0; i < 6; i++) {
		s = stream_new();
		nrv2b_literals(s, 4);
		nrv2b_match(s, 3, near_counts[i]);
		nrv2b_reuse(s, near_counts[i]);
		nrv2b_end(s);
		snprintf(name, sizeof(name), "nrv2b match of %zu at 3",
		    near_counts[i]);
		check_nrv2b_valid(name, s);
		stream_free(s);

		s = stream_new();
		nrv2b_literals(s, NRV2B_BONUS_OFFSET + 1);
		nrv2b_match(s, NRV2B_BONUS_OFFSET + 1, far_counts[i]);
		nrv2b_reuse(s, far_counts[i]);
		nrv2b_end(s);
		snprintf(name, sizeof(name), "nrv2b match of %zu at 0xd01",
		    far_counts[i]);
		check_nrv2b_valid(name, s);
		stream_free(s);
	}

	/* Example F, and at 0xd00 the same field copies one byte fewer. */
	s = stream_new();
	nrv2b_literals(s, 0xd01);
	nrv2b_match(s, 0xd01, 3);
	nrv2b_reuse(s, 3);
	nrv2b_end(s);
	check_nrv2b_valid("nrv2b example F", s);
	stream_free(s);
	s = stream_new();
	nrv2b_literals(s, 0xd00);
	nrv2b_match(s, 0xd00, 2);
	nrv2b_reuse(s, 2);
	nrv2b_end(s);
	check_nrv2b_valid("nrv2b example F at 0xd00", s);
	stream_free(s);

	/* An offset equal to the bytes produced copies from the first. */
	s = stream_new();
	nrv2b_literals(s, 7);
	nrv2b_match(s, 7, 4);
	nrv2b_end(s);
	check_nrv2b_valid("nrv2b match from the first byte", s);
	stream_free(s);

	/*
	 * Five literals, the match bit and H = 3 fill the first control
	 * byte; L follows at once, before the next control byte.
	 */
	s = stream_new();
	nrv2b_literals(s, 5);
	nrv2b_match(s, 5, 2);
	nrv2b_end(s);
	CHECK(s->code[0] == 0xfb && s->code[6] == 4 && s->distance_end == 7);
	check_nrv2b_valid("nrv2b L at a control byte boundary", s);
	stream_free(s);

	/* Long offset numbers: H = 159 and 100. */
	s = stream_new();
	nrv2b_literals(s, 40000);
	nrv2b_match(s, 40000, 5);
	nrv2b_match(s, 25000, 9);
	nrv2b_end(s);
	check_nrv2b_valid("nrv2b far offsets", s);
	stream_free(s);
}

/*
 * NRV2B verdicts, each named with its check and the bytes consumed when
 * it failed.
 */
static void
test_nrv2b_failures(void)
{
	static const worked_stream_t high_past_end = {
		"nrv2b H = 0x1000003", "00 00 00 00 00 05 80 ff", ""
	};
	static const worked_stream_t marker_low = {
		"nrv2b H = 0x1000002 with L = 0xfe", "00 00 00 00 00 04 80 fe", ""
	};
	stream_t *s;
	size_t length;

	/* An offset one past the bytes produced. */
	s = stream_new();
	nrv2b_literals(s, 4);
	nrv2b_match(s, 4, 3);
	nrv2b_end(s);
	check_nrv2b_valid("nrv2b offset 4 after 4", s);
	stream_free(s);
	s = stream_new();
	nrv2b_literals(s, 4);
	nrv2b_match(s, 5, 3);
	nrv2b_end(s);
	check_nrv2b_rejected("nrv2b offset 5 after 4", s, 100,
	    "distance before the output start", s->distance_end);
	stream_free(s);

	/* A reuse at the very start. */
	s = stream_new();
	nrv2b_reuse(s, 2);
	nrv2b_end(s);
	check_nrv2b_rejected("nrv2b reuse at the start", s, 100,
	    "distance before the output start", s->distance_end);
	stream_free(s);

	/*
	 * H grows past 0x1000002 at its last data bit, bit 47, in the sixth
	 * byte; L = 0xfe under the marker's H is offset 0xffffffff.
	 */
	s = worked_stream(&high_past_end);
	check_nrv2b_rejected(high_past_end.name, s, 100,
	    "offset out of range", 6);
	stream_free(s);
	s = worked_stream(&marker_low);
	check_nrv2b_rejected(marker_low.name, s, 100,
	    "distance before the output start", 8);
	stream_free(s);

	/* The marker's 0xff dropped, and a byte after it. */
	s = worked_stream(&nrv2b_worked[1]);
	length = s->code_length;
	s->code_length = length - 1;
	check_nrv2b_rejected("nrv2b marker byte dropped", s, 100,
	    "unterminated", length - 1);
	s->code_length = length;
	put_code(s, 0xff);
	check_nrv2b_rejected("nrv2b byte after the marker", s, 100,
	    "trailing bytes", length);
	stream_free(s);
}

/*
 * The capacity is the NRV2B bound: an output exactly filling it decodes
 * (every battery shows that); a literal when it is full, or a match whose
 * count would pass it, is refused before any byte of it is written. These
 * are not emitted for the oracle, whose decoder writes one byte past its
 * capacity.
 */
static void
expect_capacity_refusal(const char *name, const stream_t *s,
    uint64_t offset, size_t produced)
{
	codec_run_t run;

	check_nrv2b_valid(name, s);
	decode_nrv2b(s->code, s->code_length, s->plain_length - 1, &run);
	expect_failure(name, &run, FSIMG_ECORRUPT, "longer than its capacity",
	    offset);
	CHECK(run.produced == produced);
	run_free(&run);
}

static void
test_nrv2b_capacity(void)
{
	stream_t *s;

	/* The second literal, when one byte fills the capacity. */
	s = stream_new();
	nrv2b_literals(s, 2);
	nrv2b_end(s);
	expect_capacity_refusal("nrv2b literal past the capacity", s, 2, 1);
	stream_free(s);

	/* A two-bit length field, refused when it is read. */
	s = stream_new();
	nrv2b_literals(s, 3);
	nrv2b_match(s, 2, 3);
	nrv2b_end(s);
	expect_capacity_refusal("nrv2b short match past the capacity", s,
	    s->distance_end, 3);
	stream_free(s);

	/*
	 * G = 63, refused at its fifth data bit, when it reaches 63 with 62
	 * allowed. The length bits fill the first control byte, so G's first
	 * four pairs fill the byte after L and the fifth starts another.
	 */
	s = stream_new();
	nrv2b_literals(s, 3);
	nrv2b_match(s, 2, 66);
	nrv2b_end(s);
	expect_capacity_refusal("nrv2b long match past the capacity", s,
	    s->distance_end + 2, 3);
	stream_free(s);
}

/*
 * A 64 KiB NRV2B claim in a stream of a few bytes decodes under a 64 KiB
 * capacity, for work of consumed plus produced, and is refused at its
 * length field under 32 KiB with one byte produced.
 */
static void
test_nrv2b_bomb(void)
{
	fsimg_options_t options;
	fsimg_session_t session;
	fsimg_ctx_t *ctx;
	codec_run_t run;
	stream_t *s;

	s = stream_new();
	nrv2b_literals(s, 1);
	nrv2b_match(s, 1, 65535);
	nrv2b_end(s);
	CHECK(s->code_length <= 20 && s->plain_length == 65536);
	check_nrv2b_valid("nrv2b bomb", s);

	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_session_init(&session, ctx, "codec", UINT64_MAX);
	decode_nrv2b(s->code, s->code_length, 65536, &run);
	CHECK(run.status == FSIMG_OK);
	CHECK(fsimg_session_charge(&session, 0, 0, run.consumed + run.produced,
	    NULL) == FSIMG_OK);
	CHECK(session.progress.work == s->code_length + 65536);
	run_free(&run);
	fsimg_ctx_free(ctx);

	decode_nrv2b(s->code, s->code_length, 32768, &run);
	CHECK(run.status == FSIMG_ECORRUPT && run.produced == 1);
	CHECK(strstr(run.error.message, "longer than its capacity") != NULL);
	CHECK(run.consumed < s->code_length);
	run_free(&run);
	stream_free(s);
}

static void
test_nrv2b_interface(void)
{
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t *in, *out;

	in = xmalloc(NRV2B_MAX_INPUT + 1);
	out = xmalloc(NRV2B_MAX_CAPACITY + 1);
	bzero(in, NRV2B_MAX_INPUT + 1);
	CHECK(fsimg_nrv2b_decode(in, NRV2B_MAX_INPUT + 1, out, 16, &step,
	    &error) == FSIMG_EINVAL);
	CHECK(fsimg_nrv2b_decode(in, 8, out, NRV2B_MAX_CAPACITY + 1, &step,
	    &error) == FSIMG_EINVAL);
	CHECK(fsimg_nrv2b_decode(NULL, 8, out, 16, &step, &error) ==
	    FSIMG_EINVAL);
	CHECK(fsimg_nrv2b_decode(in, 8, NULL, 16, &step, &error) ==
	    FSIMG_EINVAL);
	CHECK(step.consumed == 0 && step.produced == 0 && !step.done);

	/* The largest input and capacity are accepted. */
	CHECK(fsimg_nrv2b_decode(in, NRV2B_MAX_INPUT, out, NRV2B_MAX_CAPACITY,
	    &step, &error) == FSIMG_ECORRUPT);
	free(in);
	free(out);
}

/*
 * Command modes for test/codec-integration.sh and the fuzz seeds.
 */

static uint8_t *
read_file(const char *path, size_t *length)
{
	uint8_t *bytes;
	FILE *file;
	long size;

	file = fopen(path, "rb");
	if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
	    (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "%s: cannot read\n", path);
		exit(2);
	}
	bytes = xmalloc((size_t)size);
	if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
		fprintf(stderr, "%s: short read\n", path);
		exit(2);
	}
	fclose(file);
	*length = (size_t)size;
	return (bytes);
}

static uint64_t
parse_count(const char *text)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || *text < '0' || *text > '9' || *end != '\0') {
		fprintf(stderr, "%s: not a decimal count\n", text);
		exit(2);
	}
	return ((uint64_t)value);
}

static int
report_decode_error(const char *path, const fsimg_error_t *error)
{
	fprintf(stderr, "%s: %s at %" PRIu64 "\n", path, error->message,
	    error->offset);
	return (1);
}

/*
 * --decode lzo IN OUT LIMIT: decode as the hbcifs container will, the
 * input in 64 KiB pieces and the output through a 64 KiB window that
 * keeps the 64 KiB before it as history.
 */
static int
decode_lzo_file(const char *in_path, const char *out_path, uint64_t limit)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	fsimg_status_t status;
	uint8_t *code, *window;
	size_t history, length, piece, position;
	FILE *out;
	int result;
	bool was_done;

	out = fopen(out_path, "wb");
	if (out == NULL) {
		fprintf(stderr, "%s: %s\n", out_path, strerror(errno));
		return (2);
	}
	code = read_file(in_path, &length);
	window = xmalloc(2 * TOOL_PIECE_BYTES);
	fsimg_lzo1x_decoder_init(&decoder, limit);
	was_done = false;
	history = 0;
	position = 0;
	for (;;) {
		piece = length - position;
		if (piece > TOOL_PIECE_BYTES)
			piece = TOOL_PIECE_BYTES;
		status = fsimg_lzo1x_decode(&decoder, code + position, piece,
		    position + piece == length, window, history,
		    TOOL_PIECE_BYTES, &step, &error);
		if (fwrite(window + history, 1, step.produced, out) !=
		    step.produced) {
			fprintf(stderr, "%s: write failed\n", out_path);
			result = 2;
			break;
		}
		if (status != FSIMG_OK) {
			result = report_decode_error(in_path, &error);
			break;
		}
		/* Bytes left after the end marker must have been refused. */
		if (was_done) {
			fprintf(stderr, "%s: bytes after the end marker were "
			    "accepted\n", in_path);
			result = 2;
			break;
		}
		was_done = step.done;
		position += step.consumed;
		if (step.done && position == length) {
			result = 0;
			break;
		}
		history = keep_history(window, history + step.produced,
		    TOOL_PIECE_BYTES);
	}
	free(code);
	free(window);
	if (fclose(out) != 0 && result == 0)
		result = 2;
	return (result);
}

/* --decode nrv2b IN OUT CAPACITY, the capacity at most 65536. */
static int
decode_nrv2b_file(const char *in_path, const char *out_path,
    uint64_t capacity)
{
	fsimg_codec_step_t step;
	fsimg_error_t error;
	uint8_t *code, *out;
	size_t length;
	int result;

	if (capacity > NRV2B_MAX_CAPACITY) {
		fprintf(stderr, "%" PRIu64 ": above the NRV2B capacity\n",
		    capacity);
		return (2);
	}
	code = read_file(in_path, &length);
	out = xmalloc((size_t)capacity);
	result = 0;
	if (fsimg_nrv2b_decode(code, length, out, (size_t)capacity, &step,
	    &error) == FSIMG_OK)
		write_file(out_path, out, step.produced);
	else
		result = report_decode_error(in_path, &error);
	free(code);
	free(out);
	return (result);
}

/* A splitmix64 sequence: deterministic for a seed on every host. */
static uint64_t
next_random(uint64_t *state)
{
	uint64_t z;

	*state += 0x9e3779b97f4a7c15u;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return (z ^ (z >> 31));
}

/*
 * One to three edits of a stream: a bit flipped, a byte replaced, removed,
 * inserted or duplicated, or the stream cut short. The result stays a
 * possible NRV2B input, at most 65535 bytes.
 */
static size_t
mutate(uint8_t *bytes, size_t length, size_t capacity, uint64_t *state)
{
	size_t at, edits;

	edits = 1 + next_random(state) % 3;
	for (size_t i = 0; i < edits && length > 0; i++) {
		at = next_random(state) % length;
		switch (next_random(state) % 6) {
		case 0:
			bytes[at] ^= (uint8_t)(1u << next_random(state) % 8);
			break;
		case 1:
			bytes[at] = (uint8_t)next_random(state);
			break;
		case 2:
			memmove(bytes + at, bytes + at + 1, length - at - 1);
			length--;
			break;
		case 3:
		case 4:
			if (length >= capacity)
				break;
			memmove(bytes + at + 1, bytes + at, length - at);
			if (next_random(state) % 2 == 0 && at > 0)
				bytes[at] = bytes[at - 1];
			else
				bytes[at] = (uint8_t)next_random(state);
			length++;
			break;
		default:
			length = at;
			break;
		}
	}
	return (length);
}

/*
 * --mutate CODEC SEED COUNT DIR FILE...: COUNT mutated copies of each
 * FILE, and --random CODEC SEED COUNT DIR: COUNT random strings of 1 to
 * 256 bytes, written as DIR/CODEC-*.in.
 */
static int
write_mutations(char **argv, int argc)
{
	char path[4096];
	const char *base, *codec, *directory;
	uint64_t count, state;
	uint8_t *bytes, *source;
	size_t length, source_length;

	codec = argv[2];
	state = parse_count(argv[3]);
	count = parse_count(argv[4]);
	directory = argv[5];
	bytes = xmalloc(NRV2B_MAX_INPUT);
	for (int f = 6; f < argc; f++) {
		source = read_file(argv[f], &source_length);
		base = strrchr(argv[f], '/');
		base = base == NULL ? argv[f] : base + 1;
		for (uint64_t i = 0; i < count; i++) {
			length = source_length < NRV2B_MAX_INPUT ?
			    source_length : NRV2B_MAX_INPUT;
			bcopy(source, bytes, length);
			length = mutate(bytes, length, NRV2B_MAX_INPUT, &state);
			snprintf(path, sizeof(path), "%s/%s-%s-m%" PRIu64 ".in",
			    directory, codec, base, i);
			write_file(path, bytes, length);
		}
		free(source);
	}
	free(bytes);
	return (0);
}

static int
write_random(char **argv)
{
	char path[4096];
	const char *codec, *directory;
	uint64_t count, state;
	uint8_t bytes[256];
	size_t length;

	codec = argv[2];
	state = parse_count(argv[3]);
	count = parse_count(argv[4]);
	directory = argv[5];
	for (uint64_t i = 0; i < count; i++) {
		length = 1 + next_random(&state) % sizeof(bytes);
		for (size_t j = 0; j < length; j++)
			bytes[j] = (uint8_t)next_random(&state);
		snprintf(path, sizeof(path), "%s/%s-random-%" PRIu64 ".in",
		    directory, codec, i);
		write_file(path, bytes, length);
	}
	return (0);
}

/*
 * LZO1X encoder tests.
 *
 * The encoder is judged by its streams. Every stream the tests make goes
 * through encode_and_check: it must fit a buffer of exactly the design's
 * bound, n + n / 255 + 6, leave the rest of that buffer untouched, and
 * decode under fsimg_lzo1x_decode, in one call under a limit equal to the
 * block, to exactly the block. With --emit-cases each stream is also
 * written out, so test/codec-integration.sh decodes it under the liblzo2
 * oracle.
 *
 * Most blocks are built with the stream writer above, which records the
 * stream and the block it decodes to at once. The writer's instructions
 * are chosen by hand from the encoder's rules (greedy matching, the
 * acceptance lengths, the class for each distance, carried literals), so
 * the encoder must reproduce the writer's stream byte for byte: that pins
 * the choice of every instruction as well as its validity. Other blocks
 * are judged by length: an incompressible block is one first run, and the
 * bound must hold for every shape.
 *
 * One encoder serves every case unless a test says otherwise, so each
 * expected stream is also met with a table that earlier blocks filled.
 *
 * Generated bytes come from the splitmix sequence, the same on every
 * host. Where a test expects random bytes to be incompressible, or to
 * hold no match beyond the ones it builds, its seed was chosen and checked
 * to give no accidental repeat of four bytes the encoder would take; a
 * new seed must be checked the same way.
 */
#define ENCODE_MAX_BLOCK	UINT32_MAX
#define ENCODE_CHUNK_BYTES	65536u
#define ENCODE_STORED_MAX	65535u	/* largest 16-bit stored length */
#define ENCODE_HASH_BITS	14
#define ENCODE_SEED_LIMIT	4096u	/* longest block kept as a seed */

/* The design's bound, written here independently of the header. */
static size_t
design_bound(size_t n)
{
	return (n + n / 255 + 6);
}

/* Bytes a length extension of value takes: a zero per 255, then one. */
static size_t
extension_bytes(size_t value)
{
	return ((value - 1) / 255 + 1);
}

/* The stream of a block written as one first run, then the end marker. */
static size_t
first_run_stream_length(size_t n)
{
	if (n == 0)
		return (3);
	if (n <= 238)
		return (1 + n + 3);
	return (1 + extension_bytes(n - 18) + n + 3);
}

/*
 * The design's hash, written from its text: the four bytes as a
 * little-endian value, times 2654435761 modulo 2^32, top 14 bits.
 */
static uint32_t
design_hash(const uint8_t *group)
{
	uint32_t value;

	value = (uint32_t)group[0] | (uint32_t)group[1] << 8 |
	    (uint32_t)group[2] << 16 | (uint32_t)group[3] << 24;
	return ((uint32_t)(((uint64_t)value * 2654435761u) & 0xffffffffu) >>
	    (32 - ENCODE_HASH_BITS));
}

static fsimg_ctx_t *
new_context(void)
{
	fsimg_options_t options;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &ctx, &error) == FSIMG_OK);
	return (ctx);
}

static fsimg_lzo1x_encoder_t *
new_encoder(fsimg_ctx_t *ctx)
{
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_error_t error;

	CHECK(fsimg_lzo1x_encoder_new(ctx, &encoder, &error) == FSIMG_OK);
	CHECK(encoder != NULL);
	return (encoder);
}

/* Does the stream decode, in one call, to exactly the block? */
static bool
stream_decodes_to(const uint8_t *code, size_t code_length,
    const uint8_t *block, size_t n)
{
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	fsimg_error_t error;
	fsimg_status_t status;
	uint8_t *window;
	bool same;

	window = xmalloc(n);
	fsimg_lzo1x_decoder_init(&decoder, n);
	status = fsimg_lzo1x_decode(&decoder, code, code_length, true, window,
	    0, n, &step, &error);
	same = status == FSIMG_OK && step.done &&
	    step.consumed == code_length && step.produced == n &&
	    bcmp(window, block, n) == 0;
	free(window);
	return (same);
}

/*
 * Write an encoder block as a fuzz seed for fuzz/codec.c's encoder mode:
 * header byte 0 with bit 2 set, three unused header bytes, the block.
 */
static void
emit_encoder_seed(const uint8_t *block, size_t n)
{
	char path[4096];
	uint8_t *bytes;

	if (n > ENCODE_SEED_LIMIT)
		return;
	bytes = xmalloc(4 + n);
	bzero(bytes, 4);
	bytes[0] = 4;
	bcopy(block, bytes + 4, n);
	emit_sequence++;
	snprintf(path, sizeof(path), "%s/lzoenc-%05u.seed", emit_directory,
	    emit_sequence);
	write_file(path, bytes, 4 + n);
	free(bytes);
}

/*
 * Encode a block into a buffer of exactly the bound, filled with poison,
 * and check the stream: within the bound, nothing written after it, and
 * decoding to the block. The caller frees the stream; its length is
 * *code_length.
 */
static uint8_t *
encode_and_check(fsimg_lzo1x_encoder_t *encoder, const uint8_t *block,
    size_t n, size_t *code_length)
{
	fsimg_error_t error;
	uint8_t *code;
	size_t bound;

	bound = design_bound(n);
	CHECK(fsimg_lzo1x_encode_bound(n) == bound);
	code = xmalloc(bound);
	memset(code, POISON, bound);
	CHECK(fsimg_lzo1x_encode(encoder, block, n, code, bound, code_length,
	    &error) == FSIMG_OK);
	CHECK(*code_length <= bound);
	CHECK(poison_intact(code + *code_length, bound - *code_length));
	CHECK(stream_decodes_to(code, *code_length, block, n));

	if (emit_directory != NULL && emit_seeds)
		emit_encoder_seed(block, n);
	else
		emit_case("lzo", code, *code_length, n, block, n);
	return (code);
}

/* Encode the block and check its stream with encode_and_check. */
static void
round_trip(fsimg_lzo1x_encoder_t *encoder, const uint8_t *block, size_t n)
{
	uint8_t *code;
	size_t code_length;

	code = encode_and_check(encoder, block, n, &code_length);
	free(code);
}

/* The length of the block's stream, checked by encode_and_check. */
static size_t
encoded_length(fsimg_lzo1x_encoder_t *encoder, const uint8_t *block,
    size_t n)
{
	uint8_t *code;
	size_t code_length;

	code = encode_and_check(encoder, block, n, &code_length);
	free(code);
	return (code_length);
}

/*
 * The encoder must write exactly the stream the writer built for the
 * block the writer recorded.
 */
static void
expect_encoding(const char *name, fsimg_lzo1x_encoder_t *encoder,
    stream_t *s)
{
	uint8_t *code;
	size_t code_length;

	CHECK(s->valid);
	code = encode_and_check(encoder, s->plain, s->plain_length,
	    &code_length);
	if (code_length != s->code_length ||
	    bcmp(code, s->code, code_length) != 0) {
		fprintf(stderr, "%s: the encoder wrote %zu bytes, the "
		    "expected stream has %zu\n", name, code_length,
		    s->code_length);
		CHECK(!"the encoder's stream is the expected one");
	}
	free(code);
	stream_free(s);
}

static void
fill_random(uint8_t *bytes, size_t n, uint64_t seed)
{
	for (size_t i = 0; i < n; i++)
		bytes[i] = (uint8_t)next_random(&seed);
}

/* Words from a small vocabulary, separated by spaces: text-like bytes. */
static void
fill_text(uint8_t *bytes, size_t n, uint64_t seed)
{
	static const char *const words[] = { "the", "image", "volume",
	    "of", "boot", "chunk", "a", "stream", "file", "directory",
	    "startup", "header", "QNX", "and", "to", "decoded", "\n" };
	const char *word;
	size_t at;

	at = 0;
	while (at < n) {
		word = words[next_random(&seed) %
		    (sizeof(words) / sizeof(words[0]))];
		for (size_t k = 0; word[k] != '\0' && at < n; k++) {
			bytes[at] = (uint8_t)word[k];
			at++;
		}
		if (at < n) {
			bytes[at] = ' ';
			at++;
		}
	}
}

/* A block, as text, and its stream, as hex. */
typedef struct hand_stream {
	const char	*block;
	const char	*stream;
} hand_stream_t;

/* Small blocks whose streams follow from the tables alone. */
static void
test_lzo_encoder_hand_streams(void)
{
	static const hand_stream_t cases[] = {
		/* The empty block is the end marker alone. */
		{ "", "11 00 00" },
		/* No four bytes repeat: one first run, byte 17 + n. */
		{ "A", "12 41 11 00 00" },
		{ "ABC", "14 41 42 43 11 00 00" },
		{ "ABCD", "15 41 42 43 44 11 00 00" },
		/* A near match of 4 at distance 4: 01LD DDSS, L 1, D 3. */
		{ "ABCDABCD", "15 41 42 43 44 6c 00 11 00 00" },
		/* ... carrying two and three literals in S. */
		{ "ABCDABCDxy", "15 41 42 43 44 6e 00 78 79 11 00 00" },
		{ "ABCDABCDxyz", "15 41 42 43 44 6f 00 78 79 7a 11 00 00" },
		/* Four literals after it: S 0, then a long run with L 1. */
		{ "ABCDABCDwxyz",
		    "15 41 42 43 44 6c 00 01 77 78 79 7a 11 00 00" },
		/* Near matches of 5 and 8: 1LLD DDSS, L 0 and 3. */
		{ "ABCDABCDA", "15 41 42 43 44 8c 00 11 00 00" },
		{ "ABCDEFGHABCDEFGH",
		    "19 41 42 43 44 45 46 47 48 fc 00 11 00 00" },
		/* A match of 9 within 2048 is long: 001L LLLL, word 4 x 8. */
		{ "ABCDEFGHIABCDEFGHI",
		    "1a 41 42 43 44 45 46 47 48 49 27 20 00 11 00 00" },
	};
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t expected[64], zeros[300], *code;
	size_t code_length, expected_length;

	ctx = new_context();
	encoder = new_encoder(ctx);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		parse_hex(cases[i].stream, expected, sizeof(expected),
		    &expected_length);
		code = encode_and_check(encoder,
		    (const uint8_t *)cases[i].block, strlen(cases[i].block),
		    &code_length);
		if (code_length != expected_length ||
		    bcmp(code, expected, code_length) != 0) {
			fprintf(stderr, "\"%s\": unexpected stream\n",
			    cases[i].block);
			CHECK(!"the encoder's stream is the hand-built one");
		}
		free(code);
	}

	/*
	 * 300 zeros: one literal, then a long match of 299 at distance 1,
	 * L 0 and extension 266 (a zero byte and 11), word 0.
	 */
	bzero(zeros, sizeof(zeros));
	parse_hex("12 00 20 00 0b 00 00 11 00 00", expected, sizeof(expected),
	    &expected_length);
	code = encode_and_check(encoder, zeros, sizeof(zeros), &code_length);
	CHECK(code_length == expected_length &&
	    bcmp(code, expected, code_length) == 0);
	free(code);

	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/*
 * A block of a few marker bytes, a run of zeros, and the markers again at
 * exactly distance back: the zeros are one literal and a long match at
 * distance 1, and the markers' second copy is a match when its distance
 * and length allow one, else literals. The run is written as the
 * encoder must write it, so the cases pin every class boundary.
 */
static stream_t *
repeat_after_zeros(const char *markers, size_t distance)
{
	stream_t *s;
	size_t marker_count;

	marker_count = strlen(markers);
	s = stream_new();
	/*
	 * The markers and the first zero, the string's terminating NUL, as
	 * one first run: the zero's own group has no earlier copy.
	 */
	use_literals(s, markers, marker_count + 1);
	lzo_first_run(s, marker_count + 1);
	/* The zeros up to the markers' second copy, from distance 1. */
	lzo_long_match(s, 1, distance - marker_count - 1, 0);
	return (s);
}

static void
test_lzo_encoder_distances(void)
{
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	stream_t *s;

	ctx = new_context();
	encoder = new_encoder(ctx);

	/* Four bytes at 2048 are a near match; at 2049 they are not. */
	s = repeat_after_zeros("\x01\x02\x03\x04", 2048);
	lzo_near_match(s, 2048, 4, 0);
	lzo_end(s);
	expect_encoding("4 bytes at 2048", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04", 2049);
	use_literals(s, "\x01\x02\x03\x04", 4);
	lzo_long_run(s, 4);
	lzo_end(s);
	expect_encoding("4 bytes at 2049", encoder, s);

	/* Five bytes: long to 16384, far from 16385 to 49151, none past. */
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 2049);
	lzo_long_match(s, 2049, 5, 0);
	lzo_end(s);
	expect_encoding("5 bytes at 2049", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 16384);
	lzo_long_match(s, 16384, 5, 0);
	lzo_end(s);
	expect_encoding("5 bytes at 16384", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 16385);
	lzo_far_match(s, 16385, 5, 0);
	lzo_end(s);
	expect_encoding("5 bytes at 16385", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 32768);
	lzo_far_match(s, 32768, 5, 0);
	lzo_end(s);
	expect_encoding("5 bytes at 32768", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 49151);
	lzo_far_match(s, 49151, 5, 0);
	lzo_end(s);
	expect_encoding("5 bytes at 49151", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 49152);
	use_literals(s, "\x01\x02\x03\x04\x05", 5);
	lzo_long_run(s, 5);
	lzo_end(s);
	expect_encoding("5 bytes at 49152", encoder, s);

	/* Carried literals on a long and a far match: the tail of 2 or 3. */
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 9000);
	use_literals(s, "xyz", 3);
	lzo_long_match(s, 9000, 5, 3);
	lzo_end(s);
	expect_encoding("long match carrying 3", encoder, s);
	s = repeat_after_zeros("\x01\x02\x03\x04\x05", 40000);
	use_literals(s, "xy", 2);
	lzo_far_match(s, 40000, 5, 2);
	lzo_end(s);
	expect_encoding("far match carrying 2", encoder, s);

	/* A far match past its length field: L 0 and an extension. */
	s = repeat_after_zeros("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a"
	    "\x0b\x0c", 20000);
	lzo_far_match(s, 20000, 12, 0);
	lzo_end(s);
	expect_encoding("far match of 12", encoder, s);

	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/*
 * Literal runs at the edges of their forms: first runs of 238 and 239
 * (the byte 17 + n, then 0000 0000 with an extension), and runs after a
 * match of 4, 18, 19, 274 and 300 (0000 LLLL, then L 0 with an
 * extension of one byte, then of two). The literals are random bytes, in
 * which no four bytes repeat.
 */
static void
test_lzo_encoder_literal_runs(void)
{
	static const size_t first_runs[] = { 1, 238, 239, 5000 };
	static const size_t after_match[] = { 4, 18, 19, 274, 300 };
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t literals[5000];
	stream_t *s;

	ctx = new_context();
	encoder = new_encoder(ctx);
	fill_random(literals, sizeof(literals), 17);

	for (size_t i = 0; i < sizeof(first_runs) / sizeof(first_runs[0]);
	    i++) {
		s = stream_new();
		use_literals(s, literals, first_runs[i]);
		/* The writer's long run at the start is the extended form. */
		if (first_runs[i] <= 238)
			lzo_first_run(s, first_runs[i]);
		else
			lzo_long_run(s, first_runs[i]);
		lzo_end(s);
		expect_encoding("first run", encoder, s);
	}

	for (size_t i = 0; i < sizeof(after_match) / sizeof(after_match[0]);
	    i++) {
		s = stream_new();
		use_literals(s, "ABCD", 4);
		lzo_first_run(s, 4);
		lzo_near_match(s, 4, 4, 0);
		use_literals(s, literals, after_match[i]);
		lzo_long_run(s, after_match[i]);
		lzo_end(s);
		expect_encoding("run after a match", encoder, s);
	}

	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/* Each shape of block, at each length, round-trips within the bound. */
static void
test_lzo_encoder_round_trips(void)
{
	static const size_t lengths[] = { 4095, 65536, 65537, 200000 };
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t *block;
	size_t n;

	ctx = new_context();
	encoder = new_encoder(ctx);
	block = xmalloc(200000);
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		n = lengths[i];
		bzero(block, n);
		round_trip(encoder, block, n);
		fill_text(block, n, n);
		round_trip(encoder, block, n);
		for (size_t k = 0; k < n; k++)
			block[k] = k % 2 == 0 ? 'a' : 'b';
		round_trip(encoder, block, n);
	}

	/*
	 * Random bytes repeated with a period of 49152 are one byte beyond
	 * reach and do not compress; with a period of 49151 the second copy
	 * is a far match from the first position whose table entry no other
	 * group has displaced, to the end of the block.
	 */
	fill_random(block, 49152, 7);
	bcopy(block, block + 49152, 49152);
	bcopy(block, block + 2 * 49152, 49152);
	CHECK(encoded_length(encoder, block, 3 * 49152) >= 3 * 49152);
	fill_random(block, 49151, 11);
	bcopy(block, block + 49151, 49151);
	CHECK(encoded_length(encoder, block, 2 * 49151) <
	    encoded_length(encoder, block, 49151) + 1000);

	free(block);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/*
 * The bound n + n / 255 + 6 holds for every block: encode_and_check
 * asserts it. Incompressible blocks come within three bytes of it as one
 * first run; blocks of every length to 16, and blocks of literal runs
 * between the shortest matches, the shapes whose headers cost most
 * against their matches' savings, stay within it too.
 */
static void
test_lzo_encoder_bound(void)
{
	static const size_t random_lengths[] = { 1, 100, 238, 239, 4096,
	    65535, 65536, 65537, 300000 };
	static const size_t run_lengths[] = { 4, 18, 19, 238, 239, 273, 274 };
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t *block;
	size_t n, unit;

	ctx = new_context();
	encoder = new_encoder(ctx);
	block = xmalloc(300000);

	for (n = 0; n <= 16; n++) {
		bzero(block, n);
		round_trip(encoder, block, n);
		for (size_t i = 0; i < n; i++)
			block[i] = (uint8_t)(i + 1);
		CHECK(encoded_length(encoder, block, n) ==
		    first_run_stream_length(n));
		fill_random(block, n, n);
		round_trip(encoder, block, n);
	}

	for (size_t i = 0;
	    i < sizeof(random_lengths) / sizeof(random_lengths[0]); i++) {
		n = random_lengths[i];
		fill_random(block, n, 100 + n);
		CHECK(encoded_length(encoder, block, n) ==
		    first_run_stream_length(n));
		CHECK(design_bound(n) - first_run_stream_length(n) <= 3);
	}

	/*
	 * Random runs of each length, each followed by a copy of its last
	 * four bytes: a near match of 4 at distance 4 after every run.
	 */
	for (size_t i = 0; i < sizeof(run_lengths) / sizeof(run_lengths[0]);
	    i++) {
		unit = run_lengths[i] + 4;
		n = 0;
		while (n + unit <= 65536) {
			fill_random(block + n, run_lengths[i], n);
			bcopy(block + n + run_lengths[i] - 4,
			    block + n + run_lengths[i], 4);
			n += unit;
		}
		round_trip(encoder, block, n);
	}

	free(block);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/*
 * Arguments the encoder refuses before it reads or writes a byte, and the
 * proof that the round-trip check would catch a wrong stream.
 */
static void
test_lzo_encoder_refusals(void)
{
	static const size_t lengths[] = { 0, 1, 100, 65536 };
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uint8_t *block, *code;
	size_t code_length, n;

	ctx = new_context();
	encoder = new_encoder(ctx);
	block = xmalloc(65536);
	fill_text(block, 65536, 3);

	/* One byte below the bound is refused, with nothing written. */
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		n = lengths[i];
		code = xmalloc(design_bound(n) - 1);
		memset(code, POISON, design_bound(n) - 1);
		code_length = 1;
		CHECK(fsimg_lzo1x_encode(encoder, block, n, code,
		    design_bound(n) - 1, &code_length, &error) == FSIMG_EINVAL);
		CHECK(error.status == FSIMG_EINVAL && code_length == 0);
		CHECK(poison_intact(code, design_bound(n) - 1));
		free(code);
	}

	code = xmalloc(design_bound(65536));
	CHECK(fsimg_lzo1x_encode(encoder, NULL, 1, code, design_bound(65536),
	    &code_length, &error) == FSIMG_EINVAL);
	CHECK(fsimg_lzo1x_encode(encoder, block, 0, NULL, 6, &code_length,
	    &error) == FSIMG_EINVAL);
	CHECK(fsimg_lzo1x_encode_bound(0) == 6);
	CHECK(fsimg_lzo1x_encode_bound(65536) == 65799);
#if SIZE_MAX > UINT32_MAX
	/* A block past 2^32 - 1 is refused on its length; none is read. */
	CHECK(fsimg_lzo1x_encode_bound((size_t)ENCODE_MAX_BLOCK + 1) ==
	    SIZE_MAX);
	CHECK(fsimg_lzo1x_encode(encoder, block, (size_t)ENCODE_MAX_BLOCK + 1,
	    code, design_bound(65536), &code_length, &error) == FSIMG_EINVAL);
#endif

	free(code);

	/* A byte flipped in a stream fails the round-trip check. */
	code = encode_and_check(encoder, block, 4096, &code_length);
	code[code_length / 2] ^= 0x40;
	CHECK(!stream_decodes_to(code, code_length, block, 4096));
	free(code);

	free(block);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
}

/* The table is charged to the context, and a failed allocation is clean. */
static void
test_lzo_encoder_allocation(void)
{
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_stats_t before, during, after;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;

	ctx = new_context();
	CHECK(fsimg_ctx_get_stats(ctx, &before, &error) == FSIMG_OK);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_lzo1x_encoder_new(ctx, &encoder, &error) == FSIMG_ENOMEM);
	CHECK(encoder == NULL && error.status == FSIMG_ENOMEM);
	CHECK(fsimg_ctx_get_stats(ctx, &during, &error) == FSIMG_OK);
	CHECK(during.memory_bytes == before.memory_bytes);

	fsimg_test_fail_alloc_after(ctx, 0);
	encoder = new_encoder(ctx);
	CHECK(fsimg_ctx_get_stats(ctx, &during, &error) == FSIMG_OK);
	CHECK(during.memory_bytes >= before.memory_bytes + 65536);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_lzo1x_encoder_free(NULL);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);
	fsimg_ctx_free(ctx);
}

/* FNV-1a over a stream: a digest of the bytes for cross-run comparison. */
static uint64_t
digest_bytes(uint64_t digest, const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		digest ^= bytes[i];
		digest *= 0x100000001b3u;
	}
	return (digest);
}

/*
 * The digest of the streams of a fixed corpus, on the given encoder: a
 * text block, a random block, a run of zeros and a period-3 pattern.
 */
static uint64_t
corpus_digest(fsimg_lzo1x_encoder_t *encoder)
{
	uint8_t *block, *code;
	uint64_t digest;
	size_t code_length;

	block = xmalloc(200000);
	digest = 0xcbf29ce484222325u;
	fill_text(block, 200000, 5);
	code = encode_and_check(encoder, block, 200000, &code_length);
	digest = digest_bytes(digest, code, code_length);
	free(code);
	fill_random(block, 65536, 5);
	code = encode_and_check(encoder, block, 65536, &code_length);
	digest = digest_bytes(digest, code, code_length);
	free(code);
	bzero(block, 70000);
	code = encode_and_check(encoder, block, 70000, &code_length);
	digest = digest_bytes(digest, code, code_length);
	free(code);
	for (size_t i = 0; i < 100000; i++)
		block[i] = (uint8_t)("xyz"[i % 3]);
	code = encode_and_check(encoder, block, 100000, &code_length);
	digest = digest_bytes(digest, code, code_length);
	free(code);
	free(block);
	return (digest);
}

/*
 * Find a four-byte group that differs from group but hashes, under the
 * design's hash, to the same table entry.
 */
static void
find_colliding_group(const uint8_t *group, uint8_t *colliding)
{
	uint32_t target;

	target = design_hash(group);
	for (uint32_t value = 1;; value++) {
		colliding[0] = (uint8_t)value;
		colliding[1] = (uint8_t)(value >> 8);
		colliding[2] = (uint8_t)(value >> 16);
		colliding[3] = 0x7f;
		if (bcmp(colliding, group, 4) != 0 &&
		    design_hash(colliding) == target)
			return;
	}
}

/* Does any group of block[0..end) hash to the entry of group? */
static bool
block_hashes_to(const uint8_t *block, size_t end, const uint8_t *group)
{
	for (size_t p = 0; p + 4 <= end; p++)
		if (design_hash(block + p) == design_hash(group))
			return (true);
	return (false);
}

/* The corpus digest computed by a second process, on its own encoder. */
static uint64_t
second_process_digest(fsimg_ctx_t *ctx)
{
	uint64_t digest;
	int pipe_ends[2], status;
	pid_t child;

	CHECK(pipe(pipe_ends) == 0);
	/* The child must not write the parent's buffered output again. */
	fflush(NULL);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		/*
		 * The parent alone writes the cases --emit-cases asks for.
		 * A failed CHECK here exits without writing, which the
		 * parent's read sees as end of file once both write ends
		 * are closed.
		 */
		close(pipe_ends[0]);
		emit_directory = NULL;
		digest = corpus_digest(new_encoder(ctx));
		_exit(write(pipe_ends[1], &digest, sizeof(digest)) ==
		    (ssize_t)sizeof(digest) ? 0 : 1);
	}
	close(pipe_ends[1]);
	CHECK(read(pipe_ends[0], &digest, sizeof(digest)) ==
	    (ssize_t)sizeof(digest));
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0);
	close(pipe_ends[0]);
	return (digest);
}

/*
 * Encode plant on a fresh encoder, then block on the same encoder: the
 * stream must be expected, the block's stream on a fresh table.
 */
static void
expect_plant_harmless(const char *name, fsimg_ctx_t *ctx,
    const uint8_t *plant, size_t plant_length, const uint8_t *block,
    size_t n, const uint8_t *expected, size_t expected_length)
{
	fsimg_lzo1x_encoder_t *encoder;
	uint8_t *code;
	size_t code_length;

	encoder = new_encoder(ctx);
	round_trip(encoder, plant, plant_length);
	code = encode_and_check(encoder, block, n, &code_length);
	if (code_length != expected_length ||
	    bcmp(code, expected, code_length) != 0) {
		fprintf(stderr, "%s: the planted entry changed the stream\n",
		    name);
		CHECK(!"a planted table entry is harmless");
	}
	free(code);
	fsimg_lzo1x_encoder_free(encoder);
}

/*
 * The stream depends on the block alone: a fresh encoder, a used one and
 * a second process give the same bytes, and the corpus's digest is
 * pinned, so another host must give them too.
 */
static void
test_lzo_encoder_determinism(void)
{
	fsimg_lzo1x_encoder_t *fresh, *used;
	const char *saved_directory;
	fsimg_ctx_t *ctx;
	uint8_t *block;
	uint64_t digest;

	ctx = new_context();
	fresh = new_encoder(ctx);
	used = new_encoder(ctx);
	block = xmalloc(65536);
	fill_text(block, 65536, 9);
	round_trip(used, block, 65536);

	/* The first pass alone writes its streams for --emit-cases. */
	digest = corpus_digest(fresh);
	saved_directory = emit_directory;
	emit_directory = NULL;
	CHECK(corpus_digest(used) == digest);
	CHECK(corpus_digest(used) == digest);
	CHECK(second_process_digest(ctx) == digest);
	emit_directory = saved_directory;
	if (digest != UINT64_C(0x1d122f6ed0e77d07)) {
		fprintf(stderr, "corpus digest 0x%016" PRIx64 "\n", digest);
		CHECK(!"the corpus digest is the pinned one");
	}

	free(block);
	fsimg_lzo1x_encoder_free(fresh);
	fsimg_lzo1x_encoder_free(used);
	fsimg_ctx_free(ctx);
}

/*
 * Table entries an earlier block leaves cannot reach the stream. The
 * block under test holds the group G at positions 100 and 396, its last
 * four bytes, and no other group before 100 that hashes to G's entry, so
 * a fresh table has no candidate for G at 100 and finds the match at
 * 396, which makes the stream shorter than one first run. Plants put
 * other candidates in G's entry:
 *
 *  - position 100 itself, from a block with G there: only the check that
 *    a candidate lies before the current position refuses it; without
 *    that check it would be a match of distance 0;
 *  - position 396, after the current one, from a block with G there: the
 *    same check refuses it, and so would the distance check, since the
 *    unsigned distance wraps, so this plant guards the pair rather than
 *    either check alone;
 *  - position 50, from a block whose group there differs from G but
 *    collides with it under the design's hash: the candidate lies before
 *    100 within reach, the block under test has other bytes there, and
 *    only the byte comparison refuses it.
 *
 * Each plant must leave the stream the fresh table gives. The collision
 * plant lands in G's entry only if the test's hash is the encoder's,
 * which a first case shows: a colliding group between two copies of G
 * displaces the first, so the second is no match, while zeros in its
 * place leave the match, which ends the block and so saves two bytes.
 */
static void
test_lzo_encoder_stale_entries(void)
{
	static const uint8_t group[4] = { 0x47, 0x31, 0x9e, 0x05 };
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t block[400], colliding[4], plant[400], *expected;
	size_t expected_length;

	ctx = new_context();
	encoder = new_encoder(ctx);
	find_colliding_group(group, colliding);

	fill_random(block, 64, 21);
	bcopy(group, block, 4);
	bcopy(colliding, block + 20, 4);
	bcopy(group, block + 60, 4);
	CHECK(encoded_length(encoder, block, 64) ==
	    first_run_stream_length(64));
	bzero(block + 20, 4);
	CHECK(encoded_length(encoder, block, 64) <
	    first_run_stream_length(64));
	fsimg_lzo1x_encoder_free(encoder);

	fill_random(block, sizeof(block), 33);
	bcopy(group, block + 100, 4);
	bcopy(group, block + 396, 4);
	CHECK(!block_hashes_to(block, 100 + 3, group));
	CHECK(bcmp(block + 50, group, 4) != 0);
	encoder = new_encoder(ctx);
	expected = encode_and_check(encoder, block, sizeof(block),
	    &expected_length);
	CHECK(expected_length < first_run_stream_length(sizeof(block)));
	fsimg_lzo1x_encoder_free(encoder);

	fill_random(plant, sizeof(plant), 44);
	bcopy(group, plant + 100, 4);
	CHECK(!block_hashes_to(plant + 101, sizeof(plant) - 101, group));
	expect_plant_harmless("candidate at the position", ctx, plant,
	    sizeof(plant), block, sizeof(block), expected, expected_length);

	fill_random(plant, sizeof(plant), 45);
	bcopy(group, plant + 396, 4);
	expect_plant_harmless("candidate after the position", ctx, plant,
	    sizeof(plant), block, sizeof(block), expected, expected_length);

	fill_random(plant, sizeof(plant), 55);
	bcopy(colliding, plant + 50, 4);
	CHECK(!block_hashes_to(plant + 51, sizeof(plant) - 51, group));
	expect_plant_harmless("candidate with other bytes", ctx, plant,
	    sizeof(plant), block, sizeof(block), expected, expected_length);

	free(expected);
	fsimg_ctx_free(ctx);
}

/*
 * --encode IN OUT: IN as one block, its stream to OUT, for the oracle in
 * test/codec-integration.sh.
 */
static int
encode_file(const char *in_path, const char *out_path)
{
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uint8_t *block, *code;
	size_t bound, code_length, n;

	block = read_file(in_path, &n);
	bound = fsimg_lzo1x_encode_bound(n);
	if (bound == SIZE_MAX) {
		fprintf(stderr, "%s: too long for one block\n", in_path);
		exit(2);
	}
	ctx = new_context();
	encoder = new_encoder(ctx);
	code = xmalloc(bound);
	if (fsimg_lzo1x_encode(encoder, block, n, code, bound, &code_length,
	    &error) != FSIMG_OK) {
		fprintf(stderr, "%s: %s\n", in_path, error.message);
		exit(1);
	}
	write_file(out_path, code, code_length);
	free(code);
	free(block);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
	return (0);
}

/*
 * A body framed as the ifsboot writer frames it, with what the framing
 * produced. framed, when not NULL, receives the framed bytes: each
 * chunk's 16-bit big-endian length and stream, then the zero length.
 */
typedef struct framed_body {
	FILE		*framed;
	uint64_t	 body_bytes;
	uint64_t	 framed_bytes;	/* length words and streams */
	uint64_t	 chunks;
	uint64_t	 split_chunks;
} framed_body_t;

static void
put_stored_length(framed_body_t *body, size_t length)
{
	uint8_t word[2];

	body->framed_bytes += 2;
	if (body->framed == NULL)
		return;
	word[0] = (uint8_t)(length >> 8);
	word[1] = (uint8_t)(length & 0xff);
	CHECK(fwrite(word, 1, 2, body->framed) == 2);
}

/*
 * Frame one chunk: a stream of at most 65535 bytes behind its 16-bit
 * length, or, when the stream is longer, the chunk's two halves,
 * ceil(n / 2) and floor(n / 2) bytes. Every stream must decode to its
 * bytes.
 */
static void
frame_chunk(fsimg_lzo1x_encoder_t *encoder, const uint8_t *chunk, size_t n,
    uint8_t *code, framed_body_t *body, bool may_split)
{
	fsimg_error_t error;
	size_t code_length;

	CHECK(fsimg_lzo1x_encode(encoder, chunk, n, code, design_bound(n),
	    &code_length, &error) == FSIMG_OK);
	CHECK(code_length <= design_bound(n));
	if (code_length > ENCODE_STORED_MAX) {
		CHECK(may_split);
		body->split_chunks++;
		frame_chunk(encoder, chunk, n - n / 2, code, body, false);
		frame_chunk(encoder, chunk + (n - n / 2), n / 2, code, body,
		    false);
		return;
	}
	CHECK(stream_decodes_to(code, code_length, chunk, n));
	put_stored_length(body, code_length);
	if (body->framed != NULL)
		CHECK(fwrite(code, 1, code_length, body->framed) ==
		    code_length);
	body->framed_bytes += code_length;
	body->chunks++;
}

/* Frame a whole body in chunks of 65536 bytes, the last one shorter. */
static void
frame_body(fsimg_lzo1x_encoder_t *encoder, const uint8_t *block,
    size_t length, framed_body_t *body)
{
	uint8_t *code;
	size_t n;

	code = xmalloc(design_bound(ENCODE_CHUNK_BYTES));
	body->body_bytes = length;
	for (size_t at = 0; at < length; at += n) {
		n = length - at < ENCODE_CHUNK_BYTES ? length - at :
		    ENCODE_CHUNK_BYTES;
		frame_chunk(encoder, block + at, n, code, body, true);
	}
	put_stored_length(body, 0);
	free(code);
}

/*
 * --encode-framed IN OUT: IN framed as the ifsboot writer frames it,
 * behind a minimal 256-byte startup header, the fields the oracle's boot
 * mode reads: the signature, flags1 naming LZO chunks, a startup_size of
 * 256 that puts the body right after the header, and the imagefs_size.
 * The oracle then decodes every chunk, split halves included, exactly as
 * a boot image's.
 */
static int
encode_framed_file(const char *in_path, const char *out_path)
{
	framed_body_t body;
	fsimg_lzo1x_encoder_t *encoder;
	fsimg_ctx_t *ctx;
	uint8_t header[256], *block;
	size_t length;

	block = read_file(in_path, &length);
	CHECK(fsimg_lzo1x_encode_bound(length) != SIZE_MAX);
	bzero(header, sizeof(header));
	header[0] = 0xeb;	/* the signature 0x00ff7eeb, little-endian */
	header[1] = 0x7e;
	header[2] = 0xff;
	header[6] = 0x08;	/* flags1: LZO1X chunks */
	header[0x21] = 0x01;	/* startup_size 256 */
	header[0x2c] = (uint8_t)length;
	header[0x2d] = (uint8_t)(length >> 8);
	header[0x2e] = (uint8_t)(length >> 16);
	header[0x2f] = (uint8_t)(length >> 24);

	bzero(&body, sizeof(body));
	body.framed = fopen(out_path, "wb");
	if (body.framed == NULL) {
		fprintf(stderr, "%s: %s\n", out_path, strerror(errno));
		return (2);
	}
	CHECK(fwrite(header, 1, sizeof(header), body.framed) ==
	    sizeof(header));
	ctx = new_context();
	encoder = new_encoder(ctx);
	frame_body(encoder, block, length, &body);
	CHECK(fclose(body.framed) == 0);
	free(block);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
	return (0);
}

static int
compare_names(const void *a, const void *b)
{
	return (strcmp(*(char *const *)a, *(char *const *)b));
}

/*
 * --native: every 64 KiB chunk of every decoded ImageFS body in
 * $FSIMG_IFS_NATIVE/decoded is encoded, framed as the ifsboot writer
 * frames it, and decoded again; the sizes are reported per body, so the
 * written body can be compared with the native one. Exits 77 when the
 * variable is unset or the directory holds no body.
 */
static int
encode_native_bodies(void)
{
	char directory[4096], path[8192], *names[256];
	framed_body_t body, total;
	fsimg_lzo1x_encoder_t *encoder;
	const char *native;
	struct dirent *entry;
	fsimg_ctx_t *ctx;
	uint8_t *block;
	size_t count, length;
	DIR *dir;

	native = getenv("FSIMG_IFS_NATIVE");
	if (native == NULL || *native == '\0') {
		printf("SKIP: FSIMG_IFS_NATIVE is not set\n");
		return (77);
	}
	snprintf(directory, sizeof(directory), "%s/decoded", native);
	dir = opendir(directory);
	if (dir == NULL) {
		printf("SKIP: %s is missing\n", directory);
		return (77);
	}
	count = 0;
	for (;;) {
		/* readdir returns NULL with errno set on an error. */
		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			CHECK(errno == 0);
			break;
		}
		length = strlen(entry->d_name);
		if (length <= 8 ||
		    strcmp(entry->d_name + length - 8, ".imagefs") != 0)
			continue;
		CHECK(count < sizeof(names) / sizeof(names[0]));
		names[count] = strdup(entry->d_name);
		CHECK(names[count] != NULL);
		count++;
	}
	closedir(dir);
	if (count == 0) {
		printf("SKIP: no decoded bodies in %s\n", directory);
		return (77);
	}
	qsort(names, count, sizeof(names[0]), compare_names);

	ctx = new_context();
	encoder = new_encoder(ctx);
	bzero(&total, sizeof(total));
	for (size_t i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
		block = read_file(path, &length);
		bzero(&body, sizeof(body));
		frame_body(encoder, block, length, &body);
		printf("native: %s: %" PRIu64 " bytes in %" PRIu64
		    " chunks (%" PRIu64 " split), framed body %" PRIu64
		    " bytes\n", names[i], body.body_bytes, body.chunks,
		    body.split_chunks, body.framed_bytes);
		total.body_bytes += body.body_bytes;
		total.framed_bytes += body.framed_bytes;
		total.chunks += body.chunks;
		total.split_chunks += body.split_chunks;
		free(block);
		free(names[i]);
	}
	printf("PASS: %zu native bodies, %" PRIu64 " bytes in %" PRIu64
	    " chunks round-trip within the bound; framed %" PRIu64 " bytes\n",
	    count, total.body_bytes, total.chunks, total.framed_bytes);
	fsimg_lzo1x_encoder_free(encoder);
	fsimg_ctx_free(ctx);
	return (0);
}

#define RUN(test) do { \
	if (getenv("CODEC_DEBUG") != NULL) \
		fprintf(stderr, "%s\n", #test); \
	test(); \
} while (0)

static void
run_tests(void)
{
	RUN(test_checks_are_live);
	RUN(test_lzo_worked_streams);
	RUN(test_lzo_rejected_worked_streams);
	RUN(test_lzo_first_runs);
	RUN(test_lzo_long_runs);
	RUN(test_lzo_short_matches);
	RUN(test_lzo_mid_matches);
	RUN(test_lzo_near_matches);
	RUN(test_lzo_long_matches);
	RUN(test_lzo_far_matches);
	RUN(test_lzo_end_markers);
	RUN(test_lzo_distance_past_start);
	RUN(test_lzo_framing_failures);
	RUN(test_lzo_history_contract);
	RUN(test_lzo_history_mid_match);
	RUN(test_lzo_limits);
	RUN(test_lzo_length_claims);
	RUN(test_lzo_bomb);
	RUN(test_lzo_interface);
	RUN(test_nrv2b_worked_streams);
	RUN(test_nrv2b_forms);
	RUN(test_nrv2b_failures);
	RUN(test_nrv2b_capacity);
	RUN(test_nrv2b_bomb);
	RUN(test_nrv2b_interface);
	RUN(test_lzo_encoder_hand_streams);
	RUN(test_lzo_encoder_distances);
	RUN(test_lzo_encoder_literal_runs);
	RUN(test_lzo_encoder_round_trips);
	RUN(test_lzo_encoder_bound);
	RUN(test_lzo_encoder_refusals);
	RUN(test_lzo_encoder_allocation);
	RUN(test_lzo_encoder_determinism);
	RUN(test_lzo_encoder_stale_entries);
}

int
main(int argc, char **argv)
{
	if (argc == 6 && strcmp(argv[1], "--decode") == 0 &&
	    strcmp(argv[2], "lzo") == 0)
		return (decode_lzo_file(argv[3], argv[4],
		    parse_count(argv[5])));
	if (argc == 6 && strcmp(argv[1], "--decode") == 0 &&
	    strcmp(argv[2], "nrv2b") == 0)
		return (decode_nrv2b_file(argv[3], argv[4],
		    parse_count(argv[5])));
	if (argc == 3 && strcmp(argv[1], "--emit-cases") == 0) {
		emit_directory = argv[2];
		run_tests();
		return (0);
	}
	if (argc == 4 && strcmp(argv[1], "--encode") == 0)
		return (encode_file(argv[2], argv[3]));
	if (argc == 4 && strcmp(argv[1], "--encode-framed") == 0)
		return (encode_framed_file(argv[2], argv[3]));
	if (argc == 2 && strcmp(argv[1], "--native") == 0)
		return (encode_native_bodies());
	if (argc >= 7 && strcmp(argv[1], "--mutate") == 0)
		return (write_mutations(argv, argc));
	if (argc == 6 && strcmp(argv[1], "--random") == 0)
		return (write_random(argv));
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_directory = argv[2];
		emit_seeds = true;
		run_tests();
		return (0);
	}
	if (argc != 1) {
		fprintf(stderr, "usage: test-codec [--decode lzo|nrv2b IN OUT "
		    "BOUND | --emit-cases DIR | --encode IN OUT | --encode-framed "
		    "IN OUT | --mutate CODEC SEED COUNT DIR FILE... | --native | "
		    "--random CODEC SEED COUNT DIR | --seed DIR]\n");
		return (2);
	}
	run_tests();
	return (0);
}
