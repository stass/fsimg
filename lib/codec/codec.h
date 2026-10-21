/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_CODEC_H
#define FSIMG_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

/*
 * Stream decoders for LZO1X and NRV2B (the 8-bit control-buffer variant).
 * A decoder sees one compressed stream and one output; it knows nothing of
 * the chunk, header or image that framed the stream. Both decoders read
 * one input byte at a time with its bound checked first and write one
 * output byte at a time with its bound checked first, so no call reads
 * past its input or writes past its output, and a call's running time is
 * proportional to the bytes it consumes plus the bytes it produces. The
 * caller charges that sum, consumed plus produced, to its session's work
 * counter after each call.
 *
 * A corrupt stream is FSIMG_ECORRUPT with the codec's name and the failed
 * check in the message and the stream offset in error->offset: the stream
 * bytes consumed when the check failed, counting a byte whose value failed
 * it but not a byte that was refused before it was read. Output already
 * written is a correct prefix of a stream the caller discards.
 */

/* What one decoder call did. */
typedef struct fsimg_codec_step {
	size_t	 consumed;	/* input bytes used by this call */
	size_t	 produced;	/* output bytes written by this call */
	bool	 done;		/* the end marker has been decoded */
} fsimg_codec_step_t;

/*
 * Which part of the LZO1X stream the decoder expects next: one of the
 * input bytes of an instruction (its first byte, its length extension, its
 * one-byte or two-byte distance operand), or one of the output bytes of a
 * literal copy or a match copy.
 */
typedef enum fsimg_lzo1x_phase {
	FSIMG_LZO1X_FIRST_BYTE,
	FSIMG_LZO1X_INSTRUCTION,
	FSIMG_LZO1X_LENGTH_EXTENSION,
	FSIMG_LZO1X_DISTANCE_BYTE,
	FSIMG_LZO1X_DISTANCE_WORD_LOW,
	FSIMG_LZO1X_DISTANCE_WORD_HIGH,
	FSIMG_LZO1X_COPY_LITERALS,
	FSIMG_LZO1X_COPY_MATCH,
	FSIMG_LZO1X_DONE,
	FSIMG_LZO1X_FAILED
} fsimg_lzo1x_phase_t;

/* The instruction classes of an LZO1X stream. */
typedef enum fsimg_lzo1x_class {
	FSIMG_LZO1X_LITERAL_RUN,	/* 0000 LLLL after no literals */
	FSIMG_LZO1X_SHORT_MATCH,	/* 0000 DDSS after 1 to 3 literals */
	FSIMG_LZO1X_MID_MATCH,		/* 0000 DDSS after 4 or more literals */
	FSIMG_LZO1X_FAR_MATCH,		/* 0001 HLLL */
	FSIMG_LZO1X_LONG_MATCH,		/* 001L LLLL */
	FSIMG_LZO1X_NEAR_MATCH		/* 01LD DDSS and 1LLD DDSS */
} fsimg_lzo1x_class_t;

/*
 * A resumable LZO1X decoder. The caller embeds it or keeps it on its
 * stack; it owns no memory. Its fields are private to lib/codec/lzo1x.c.
 *
 * length is the instruction's run or match length while its operands are
 * read, held to length_bound, and the bytes still to copy once copying
 * begins. literal_state is the number of literals copied since the last
 * match, capped at 4: it alone decides what an instruction byte below 16
 * means.
 */
typedef struct fsimg_lzo1x_decoder {
	uint64_t	 limit;		/* most bytes the whole stream may produce */
	uint64_t	 stream_in;	/* stream bytes consumed by all calls */
	uint64_t	 stream_out;	/* stream bytes produced by all calls */
	uint64_t	 length;
	uint64_t	 length_bound;
	uint32_t	 distance;	/* match distance as assembled so far */
	fsimg_lzo1x_phase_t phase;
	fsimg_lzo1x_class_t instruction; /* class of the instruction in hand */
	uint8_t		 carried;	/* literals the match in hand carries */
	uint8_t		 literal_state;
	uint8_t		 word_low;	/* low byte of a distance word */
} fsimg_lzo1x_decoder_t;

/*
 * Prepare a decoder for one stream that may produce at most limit bytes.
 */
void fsimg_lzo1x_decoder_init(fsimg_lzo1x_decoder_t *, uint64_t limit);

/*
 * Decode the next piece of an LZO1X stream. in[0..in_len) is the next
 * piece; final says no byte follows it. window[0..history) holds the most
 * recently produced bytes in order, at least min(bytes produced, 49151) of
 * them, and the call writes into window[history..history + capacity).
 *
 * The call returns FSIMG_OK at an edge: when its next step needs an input
 * byte beyond in_len or an output byte beyond its capacity, or when it has
 * decoded the end marker (step->done). Instruction bytes that need no
 * output are still consumed once the window is full, so a stream that
 * fills the window exactly can end in the same call. The step records
 * what the call did on every return. A match reaching before the
 * first byte of the stream is corrupt; one within the stream but beyond the
 * kept history is the caller's error, FSIMG_ESTATE. After any error the
 * decoder is finished and every later call is FSIMG_ESTATE. decoder and
 * step must not be NULL.
 */
fsimg_status_t fsimg_lzo1x_decode(fsimg_lzo1x_decoder_t *,
    const uint8_t *in, size_t in_len, bool final,
    uint8_t *window, size_t history, size_t capacity,
    fsimg_codec_step_t *, fsimg_error_t *);

/*
 * Decode one whole NRV2B stream, in[0..in_len) with in_len at most 65535,
 * into out[0..capacity) with capacity at most 65536; the capacity is the
 * output bound. Success means the end marker was the last thing in the
 * input: step->done is set, step->consumed is in_len and the output is
 * out[0..step->produced). step must not be NULL.
 */
fsimg_status_t fsimg_nrv2b_decode(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, fsimg_codec_step_t *, fsimg_error_t *);

#endif
