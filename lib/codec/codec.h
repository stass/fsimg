/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_CODEC_H
#define FSIMG_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

/*
 * Stream decoders for LZO1X, NRV2B (the 8-bit control-buffer variant), LZ4
 * blocks and bzip2, and an LZO1X encoder, declared at the end. A codec sees one
 * compressed stream and one output; it knows nothing of the chunk, header or
 * image that framed the stream. The LZO1X and NRV2B decoders read
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
 * written is a correct prefix of a stream the caller discards; the bzip2
 * decoder's own contract, at its declaration, differs where it says so.
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

/*
 * Decode one whole raw LZ4 block, in[0..in_len), into out[0..capacity), which
 * must not overlap it; the capacity is the output bound. The block must end
 * exactly at in_len, directly after a sequence's literals. Success sets
 * step->done, step->consumed is in_len and the output is
 * out[0..step->produced), which may be shorter than the capacity; the caller
 * judges its length. An offset of zero, or an offset reaching before the
 * output start, a literal run or match longer than the room left, and an
 * input ending anywhere else are FSIMG_ECORRUPT. The block's end conditions that bind
 * only encoders (at least five final literals, the last match 12 bytes before
 * the end) are not checked. On every return out[0..step->produced) is the
 * decoded prefix and nothing past it is written. step must not be NULL.
 */
fsimg_status_t fsimg_lz4_decode(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, fsimg_codec_step_t *, fsimg_error_t *);

/*
 * Decode the first length bytes of a raw LZ4 block into out[0..length) and
 * stop there, mid-sequence if need be, without reading further or requiring
 * the block to end: the literal run or match that reaches length is copied up
 * to it, and only its copied literals must be in the input. Success sets
 * step->done with step->produced equal to length and step->consumed where the
 * decoder stopped. A block that ends before length bytes is FSIMG_ECORRUPT,
 * and so is every verdict of fsimg_lz4_decode() met before length is reached,
 * except that a literal run or match running past length is clipped instead.
 * done here means the requested length was produced, not that the block
 * ended.
 */
fsimg_status_t fsimg_lz4_decode_partial(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t length, fsimg_codec_step_t *, fsimg_error_t *);

/*
 * Decode one whole bzip2 stream, in[0..in_len), into out[0..capacity); the
 * capacity is the output bound, and output past it is corrupt. Success
 * means the stream's end mark and padding were the last thing in the input:
 * step->done is set, step->consumed is in_len and the output is
 * out[0..step->produced), which the caller compares with the length it
 * expects. One stream is read, so bytes after it are corrupt, and a
 * randomised block is FSIMG_EUNSUPPORTED.
 *
 * Working memory, at most about 3.6 MiB and less for a lower level or a
 * small capacity, is allocated through ctx after the stream header and
 * freed before the call returns, so a limit or allocation failure is the
 * context's status. On failure, out may hold bytes of a block whose checksum
 * never passed; the caller discards it. A call's running time is
 * proportional to in_len plus capacity, whatever the stream declares; its
 * caller charges that. step and error must not be NULL.
 */
fsimg_status_t fsimg_bzip2_decode(fsimg_ctx_t *, const uint8_t *in,
    size_t in_len, uint8_t *out, size_t capacity, fsimg_codec_step_t *,
    fsimg_error_t *);

/*
 * The longest stream a caller should accept for a decoded length: above any
 * real encoder's output, and low enough that a stream cannot pad itself with
 * bits that decode to nothing. decoded must be small enough not to overflow.
 */
uint64_t fsimg_bzip2_stream_bound(uint64_t decoded);

/*
 * The bounds of one LZFSE or LZVN stream: a decoded chunk, and its encoded form,
 * which holds the output of the largest raw block or marker with a margin above
 * every encoder's worst case.
 */
#define FSIMG_LZFSE_OUTPUT_MAX	65536u
#define FSIMG_LZFSE_INPUT_MAX	69632u

/* The bytes of scratch fsimg_lzfse_decode() needs. */
size_t fsimg_lzfse_scratch_bytes(void);

/*
 * Decode one whole LZFSE stream, in[0..in_len) with in_len at most
 * FSIMG_LZFSE_INPUT_MAX, into out[0..capacity) with capacity at most
 * FSIMG_LZFSE_OUTPUT_MAX; the capacity is the output bound. scratch holds
 * fsimg_lzfse_scratch_bytes() bytes with an allocation's alignment, charged to
 * the caller's context; the decoder allocates nothing. A larger bound or a NULL
 * pointer is FSIMG_EINVAL. Success means the end block was the last thing in the
 * input: step->done is set, step->consumed is in_len and the output is
 * out[0..step->produced). Output beyond the capacity, input after the end, an
 * unterminated stream and any invalid block are FSIMG_ECORRUPT. Bytes of out past
 * step->produced may have been overwritten. step must not be NULL.
 */
fsimg_status_t fsimg_lzfse_decode(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, void *scratch, fsimg_codec_step_t *,
    fsimg_error_t *);

/*
 * Decode one raw LZVN stream, without the LZFSE block framing, the same way; it
 * needs no scratch.
 */
fsimg_status_t fsimg_lzvn_decode(const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, fsimg_codec_step_t *, fsimg_error_t *);

/*
 * The LZO1X encoder: one block in, one complete stream out, end marker
 * included, in one call. Its only state is a 64 KiB table of input
 * positions, allocated once through the context and reused for every
 * block; the stream depends on the block's bytes alone, never on the
 * table's earlier contents, the host or the output capacity. The object
 * belongs to one caller at a time.
 */
typedef struct fsimg_lzo1x_encoder fsimg_lzo1x_encoder_t;

/*
 * Allocate an encoder through ctx, charged to its memory budget. On
 * failure *out is NULL and the status is the allocation's. error must not
 * be NULL.
 */
fsimg_status_t fsimg_lzo1x_encoder_new(fsimg_ctx_t *,
    fsimg_lzo1x_encoder_t **, fsimg_error_t *);

/* Free an encoder; NULL is ignored. */
void fsimg_lzo1x_encoder_free(fsimg_lzo1x_encoder_t *);

/*
 * The longest stream fsimg_lzo1x_encode writes for in_len bytes:
 * in_len + in_len / 255 + 6. For an in_len the encoder refuses, above
 * 2^32 - 1, or one whose bound does not fit a size_t, it is SIZE_MAX.
 */
size_t fsimg_lzo1x_encode_bound(size_t in_len);

/*
 * Encode in[0..in_len) into out[0..capacity), which must not overlap it,
 * and set *out_len to the stream's length. A capacity below the bound,
 * an in_len above 2^32 - 1 or a NULL buffer with a nonzero length is
 * FSIMG_EINVAL before anything is read or written. Every write is still
 * checked against the capacity; a stream that would pass it is
 * FSIMG_ESTATE, a defect in the encoder, with the buffer's contents
 * unspecified. The call's running time is
 * proportional to in_len plus *out_len, which the caller charges to its
 * session's work counter. *out_len is 0 on failure; encoder and out_len
 * must not be NULL.
 */
fsimg_status_t fsimg_lzo1x_encode(fsimg_lzo1x_encoder_t *,
    const uint8_t *in, size_t in_len, uint8_t *out, size_t capacity,
    size_t *out_len, fsimg_error_t *);

#endif
