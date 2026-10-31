/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "codec.h"
#include "internal.h"

/*
 * The NRV2B decoder, for the 8-bit control-buffer variant.
 *
 * An NRV2B stream interleaves control bits, packed eight to a byte and
 * taken most significant first, with data bytes: literals and the low
 * bytes of match offsets. A control byte is read when a control bit is
 * needed and the current one is spent; a data byte is the next unread
 * input byte when it is needed. Each item starts with a control bit: 1 is
 * a literal, the next data byte; 0 is a match or the end marker, an offset
 * part and then, for a match, a length part. Both parts use one code for
 * an integer of at least 2: a value starting at 1, extended by pairs of
 * control bits, the first appended as the new low bit and the second 1 on
 * the last pair.
 *
 * The decoder runs over one whole stream in one call: the streams it reads
 * are framed chunks of at most 65535 bytes decoding to at most 65536.
 * Every bit and byte read is checked against the input first, every number
 * is held to its bound as it grows, and a match's count is held against
 * the capacity before any byte of it is written, so nothing is read past
 * the input or written past the output, and the work is proportional to
 * the bytes consumed and produced.
 */
#define NRV2B_MAX_INPUT		65535
#define NRV2B_MAX_CAPACITY	65536

/* The offset part: H = 2 reuses the last offset; more is a new one. */
#define NRV2B_HIGH_REUSE	2
#define NRV2B_HIGH_NEW_BASE	3
#define NRV2B_HIGH_MAX		UINT32_C(0x1000002)
#define NRV2B_END_MARKER_RAW	UINT32_C(0xffffffff)
#define NRV2B_INITIAL_OFFSET	1

/*
 * The length part: two bits give the field F, 1 to 3, or 00 and a number
 * G for F = G + 2. A match copies F + 1 bytes, and one more when its
 * offset is above 0xd00.
 */
#define NRV2B_LONG_FIELD_BIAS	2
#define NRV2B_COUNT_BIAS	1
#define NRV2B_OFFSET_BONUS_THRESHOLD 0xd00

/* The input and where the decoder is in it. */
typedef struct nrv2b_input {
	const uint8_t	*bytes;
	size_t		 length;
	size_t		 position;	/* bytes consumed */
	unsigned	 control;	/* the current control byte */
	unsigned	 control_bits;	/* its bits not yet taken */
} nrv2b_input_t;

/* The output and the bytes produced so far. */
typedef struct nrv2b_output {
	uint8_t		*bytes;
	size_t		 capacity;
	size_t		 produced;
} nrv2b_output_t;

/* What reading a number found. */
typedef enum nrv2b_number_result {
	NRV2B_NUMBER_READ,
	NRV2B_NUMBER_TRUNCATED,		/* the input ended inside it */
	NRV2B_NUMBER_TOO_LARGE		/* it grew past its bound */
} nrv2b_number_result_t;

static fsimg_status_t nrv2b_fail(const nrv2b_input_t *, fsimg_status_t,
    const char *, fsimg_error_t *);
static bool nrv2b_read_bit(nrv2b_input_t *, unsigned *);
static bool nrv2b_read_data(nrv2b_input_t *, uint8_t *);
static nrv2b_number_result_t nrv2b_read_number(nrv2b_input_t *, uint32_t,
    uint32_t *);
static fsimg_status_t nrv2b_read_offset(nrv2b_input_t *,
    const nrv2b_output_t *, uint32_t *, bool *, fsimg_error_t *);
static fsimg_status_t nrv2b_read_count(nrv2b_input_t *,
    const nrv2b_output_t *, uint32_t, size_t *, fsimg_error_t *);
static fsimg_status_t nrv2b_decode_items(nrv2b_input_t *, nrv2b_output_t *,
    fsimg_error_t *);

static fsimg_status_t
nrv2b_fail(const nrv2b_input_t *input, fsimg_status_t status,
    const char *check, fsimg_error_t *error)
{
	fsimg_error_set(error, status, 0, FSIMG_OP_READER, check);
	if (error != NULL) {
		error->offset = input->position;
		error->offset_valid = true;
	}
	return (status);
}

/* The next control bit, reading a control byte when the last is spent. */
static bool
nrv2b_read_bit(nrv2b_input_t *input, unsigned *bit)
{
	if (input->control_bits == 0) {
		if (input->position == input->length)
			return (false);
		input->control = input->bytes[input->position++];
		input->control_bits = 8;
	}
	input->control_bits--;
	*bit = (input->control >> input->control_bits) & 1;
	return (true);
}

static bool
nrv2b_read_data(nrv2b_input_t *input, uint8_t *byte)
{
	if (input->position == input->length)
		return (false);
	*byte = input->bytes[input->position++];
	return (true);
}

/*
 * Read a number of the pair code. It is held to max after every bit
 * appended: a value only grows, so once past max it stays past.
 */
static nrv2b_number_result_t
nrv2b_read_number(nrv2b_input_t *input, uint32_t max, uint32_t *value)
{
	unsigned bit, stop;

	*value = 1;
	do {
		if (!nrv2b_read_bit(input, &bit))
			return (NRV2B_NUMBER_TRUNCATED);
		*value = *value << 1 | bit;
		if (*value > max)
			return (NRV2B_NUMBER_TOO_LARGE);
		if (!nrv2b_read_bit(input, &stop))
			return (NRV2B_NUMBER_TRUNCATED);
	} while (stop == 0);
	return (NRV2B_NUMBER_READ);
}

/*
 * The offset part of a match: H, then for a new offset the data byte L.
 * The raw value (H - 3) x 256 + L of 0xffffffff is the end marker; any
 * other offset, new or reused, must lie within the bytes produced.
 */
static fsimg_status_t
nrv2b_read_offset(nrv2b_input_t *input, const nrv2b_output_t *output,
    uint32_t *last_offset, bool *end, fsimg_error_t *error)
{
	uint32_t high, raw;
	uint8_t low;

	*end = false;
	switch (nrv2b_read_number(input, NRV2B_HIGH_MAX, &high)) {
	case NRV2B_NUMBER_READ:
		break;
	case NRV2B_NUMBER_TRUNCATED:
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: unterminated stream", error));
	case NRV2B_NUMBER_TOO_LARGE:
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: offset out of range", error));
	}
	if (high != NRV2B_HIGH_REUSE) {
		if (!nrv2b_read_data(input, &low))
			return (nrv2b_fail(input, FSIMG_ECORRUPT,
			    "nrv2b: unterminated stream", error));
		/* H is at most 0x1000002, so raw fits without wrapping. */
		raw = (high - NRV2B_HIGH_NEW_BASE) << 8 | low;
		if (raw == NRV2B_END_MARKER_RAW) {
			*end = true;
			return (FSIMG_OK);
		}
		*last_offset = raw + 1;
	}
	if (*last_offset > output->produced)
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: distance before the output start", error));
	return (FSIMG_OK);
}

/*
 * The length part of a match at offset: the bytes it copies, held against
 * the room left in the output before any of them is written. For the long
 * form the bound is applied to G as it grows.
 */
static fsimg_status_t
nrv2b_read_count(nrv2b_input_t *input, const nrv2b_output_t *output,
    uint32_t offset, size_t *count, fsimg_error_t *error)
{
	size_t field_to_count, g_bias, g_max, room;
	uint32_t g;
	unsigned a, b;

	/* The count is the field F plus one, plus one more above 0xd00. */
	room = output->capacity - output->produced;
	field_to_count = NRV2B_COUNT_BIAS;
	if (offset > NRV2B_OFFSET_BONUS_THRESHOLD)
		field_to_count++;

	if (!nrv2b_read_bit(input, &a))
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: unterminated stream", error));
	if (!nrv2b_read_bit(input, &b))
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: unterminated stream", error));
	if (a != 0 || b != 0) {
		/* 01, 10, 11: F = 1, 2, 3. */
		*count = (a << 1 | b) + field_to_count;
	} else {
		/*
		 * 00: F = G + 2, so the count is G + 2 + field_to_count; call
		 * that sum of biases g_bias. Then the count fits exactly when
		 * G is at most the room less g_bias, and with no more room
		 * than g_bias every G, which is at least 2, is too large.
		 */
		g_bias = NRV2B_LONG_FIELD_BIAS + field_to_count;
		g_max = room > g_bias ? room - g_bias : 0;
		switch (nrv2b_read_number(input, (uint32_t)g_max, &g)) {
		case NRV2B_NUMBER_READ:
			break;
		case NRV2B_NUMBER_TRUNCATED:
			return (nrv2b_fail(input, FSIMG_ECORRUPT,
			    "nrv2b: unterminated stream", error));
		case NRV2B_NUMBER_TOO_LARGE:
			return (nrv2b_fail(input, FSIMG_ECORRUPT,
			    "nrv2b: output longer than its capacity", error));
		}
		*count = g + g_bias;
	}
	if (*count > room)
		return (nrv2b_fail(input, FSIMG_ECORRUPT,
		    "nrv2b: output longer than its capacity", error));
	return (FSIMG_OK);
}

/* Decode items until the end marker, or a verdict. */
static fsimg_status_t
nrv2b_decode_items(nrv2b_input_t *input, nrv2b_output_t *output,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	uint32_t last_offset;
	size_t count, from;
	unsigned bit;
	uint8_t byte;
	bool end;

	last_offset = NRV2B_INITIAL_OFFSET;
	for (;;) {
		if (!nrv2b_read_bit(input, &bit))
			return (nrv2b_fail(input, FSIMG_ECORRUPT,
			    "nrv2b: unterminated stream", error));

		if (bit == 1) {
			if (output->produced == output->capacity)
				return (nrv2b_fail(input, FSIMG_ECORRUPT,
				    "nrv2b: output longer than its capacity",
				    error));
			if (!nrv2b_read_data(input, &byte))
				return (nrv2b_fail(input, FSIMG_ECORRUPT,
				    "nrv2b: unterminated stream", error));
			output->bytes[output->produced++] = byte;
			continue;
		}

		status = nrv2b_read_offset(input, output, &last_offset, &end,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (end)
			return (FSIMG_OK);
		status = nrv2b_read_count(input, output, last_offset, &count,
		    error);
		if (status != FSIMG_OK)
			return (status);

		/* Front to back, so an offset below the count repeats. */
		from = output->produced - last_offset;
		for (size_t i = 0; i < count; i++)
			output->bytes[output->produced++] =
			    output->bytes[from + i];
	}
}

fsimg_status_t
fsimg_nrv2b_decode(const uint8_t *in, size_t in_len, uint8_t *out,
    size_t capacity, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	nrv2b_input_t input = { .bytes = in, .length = in_len };
	nrv2b_output_t output = { .bytes = out, .capacity = capacity };
	fsimg_status_t status;

	bzero(step, sizeof(*step));
	if ((in == NULL && in_len != 0) || (out == NULL && capacity != 0) ||
	    in_len > NRV2B_MAX_INPUT || capacity > NRV2B_MAX_CAPACITY)
		return (nrv2b_fail(&input, FSIMG_EINVAL,
		    "nrv2b: invalid buffer arguments", error));

	status = nrv2b_decode_items(&input, &output, error);
	step->consumed = input.position;
	step->produced = output.produced;
	if (status != FSIMG_OK)
		return (status);
	step->done = true;

	/* The framing gives the stream's exact end, so nothing may follow. */
	if (input.position != in_len)
		return (nrv2b_fail(&input, FSIMG_ECORRUPT,
		    "nrv2b: trailing bytes after the end marker", error));
	return (FSIMG_OK);
}
