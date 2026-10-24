/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <zlib.h>

#include "fsimg-endian.h"
#include "host.h"
#include "layer.h"
#include "zlib-memory.h"

/*
 * gzip, read side (RFC 1952; specs/design-layers.md#gzip). A file is one
 * or more members, each a header, a raw DEFLATE stream and a trailer of
 * the CRC32 and the length modulo 2^32 of the member's decoded bytes.
 *
 * The probe claims on identity: the identification bytes, method 8 and
 * zero reserved flag bits. Decoding parses every header itself, so each
 * optional field is a bounded read: FEXTRA by its 16-bit length, FNAME and
 * FCOMMENT at most 64 KiB with their terminator, all discarded, and the
 * header CRC16 checked when present. zlib inflates only the raw DEFLATE
 * data, with its state charged to the context's memory budget. The decoded
 * bytes go to the spool in bounded steps, each reserved against the spool
 * limit before it is written, so the observed length, not the trailer's
 * modular one, bounds a bomb. Members follow one another as gzip(1) reads
 * them; any byte after the last member is an error, not a warning.
 *
 * gzip, write side. One member whose header and trailer this file emits
 * around zlib's raw DEFLATE stream, so every byte outside the compressed
 * data is fixed here and not by the zlib build: method 8, no flags, the
 * modification time clamped to its 32-bit field, extra flags naming the
 * fastest or slowest level, and OS 3 (Unix). The encoder is a filter over
 * the transcode's sequential sink; its deflate state and buffers are
 * charged to the context's memory budget.
 */
#define GZIP_HEADER_BYTES	10u
#define GZIP_TRAILER_BYTES	8u
#define GZIP_METHOD_DEFLATE	8u
#define GZIP_FHCRC		0x02u
#define GZIP_FEXTRA		0x04u
#define GZIP_FNAME		0x08u
#define GZIP_FCOMMENT		0x10u
#define GZIP_RESERVED		0xe0u
/* The longest FNAME or FCOMMENT, its terminator included. */
#define GZIP_TEXT_MAX		(64u * 1024)
#define GZIP_INPUT_BYTES	(64u * 1024)
#define GZIP_OUTPUT_BYTES	(256u * 1024)
#define GZIP_OS_UNIX		3u
#define GZIP_XFL_SLOWEST	2u	/* Written for level 9 */
#define GZIP_XFL_FASTEST	4u	/* Written for level 1 */
#define GZIP_DEFAULT_LEVEL	6u
#define GZIP_MAX_LEVEL		9u
#define GZIP_MEMORY_LEVEL	8	/* zlib's default deflate memory level */
/* The encoder's unit: its deflate output buffer and its input pieces. */
#define GZIP_PIECE_BYTES	(64u * 1024)

/*
 * One decode. Input bytes wait in stream.next_in and stream.avail_in,
 * where inflate leaves whatever follows a member's DEFLATE data, so the
 * header and trailer parsers read from the same place. ended is set once
 * the source has nothing more.
 */
typedef struct gzip_decoder {
	z_stream	 stream;
	fsimg_zlib_memory_t memory;
	fsimg_spool_input_t	*source;
	fsimg_spool_t	*spool;
	unsigned char	*input;
	unsigned char	*output;
	uint64_t	 member;	/* 1-based, for messages */
	bool		 ended;
	bool		 inflating;	/* inflateInit2 succeeded */
} gzip_decoder_t;

/*
 * One encode into sink. crc and length run over the bytes the member
 * holds; zeros is a piece of zeros that a zero run is deflated from.
 */
typedef struct gzip_encoder {
	z_stream	 stream;
	fsimg_zlib_memory_t memory;
	fsimg_seq_sink_t *sink;
	unsigned char	*output;
	unsigned char	*zeros;
	uLong		 crc;
	uint64_t	 length;
	bool		 deflating;	/* deflateInit2 succeeded */
} gzip_encoder_t;

static fsimg_status_t gzip_probe(const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t gzip_decode(fsimg_spool_input_t *, fsimg_spool_t *,
    fsimg_error_t *);
static fsimg_status_t gzip_plan(const fsimg_layer_options_t *, uint32_t,
    int64_t, fsimg_compression_settings_t *, fsimg_error_t *);
static fsimg_status_t gzip_bound(uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t encoder_open(fsimg_ctx_t *,
    const fsimg_compression_settings_t *, fsimg_seq_sink_t *, void **,
    fsimg_error_t *);
static fsimg_status_t encoder_write(void *, const unsigned char *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t encoder_finish(void *, fsimg_error_t *);
static void encoder_close(void *);
static fsimg_status_t deflate_out(gzip_encoder_t *, int, fsimg_error_t *);
static unsigned char header_xfl(uint32_t);
static fsimg_status_t zlib_failed(const fsimg_zlib_memory_t *, int,
    const char *, fsimg_error_t *);
static fsimg_status_t gzip_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t refill(gzip_decoder_t *, fsimg_error_t *);
static fsimg_status_t next_byte(gzip_decoder_t *, unsigned char *, uLong *,
    fsimg_error_t *);
static fsimg_status_t skip_text(gzip_decoder_t *, uLong *, const char *,
    fsimg_error_t *);
static fsimg_status_t read_header(gzip_decoder_t *, fsimg_error_t *);
static fsimg_status_t inflate_member(gzip_decoder_t *, uLong *, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t read_trailer(gzip_decoder_t *, uLong, uint64_t,
    fsimg_error_t *);
static fsimg_status_t decoder_start(gzip_decoder_t *, fsimg_error_t *);
static void decoder_end(gzip_decoder_t *);
static fsimg_status_t decode_members(gzip_decoder_t *, fsimg_error_t *);

static const fsimg_compressor_ops_t gzip_encoder = {
	.open = encoder_open,
	.write = encoder_write,
	.finish = encoder_finish,
	.close = encoder_close
};

const fsimg_compression_ops_t fsimg_gzip_compression = {
	.probe_bytes = 4,
	.probe = gzip_probe,
	.decode = gzip_decode,
	.plan = gzip_plan,
	.bound = gzip_bound,
	.encoder = &gzip_encoder,
	.zlib = true
};

static fsimg_status_t
gzip_probe(const unsigned char *prefix, size_t length, fsimg_error_t *error)
{
	if (length < 4 || prefix[0] != 0x1f || prefix[1] != 0x8b ||
	    prefix[2] != GZIP_METHOD_DEFLATE || (prefix[3] & GZIP_RESERVED) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no gzip signature"));
	return (FSIMG_OK);
}

/*
 * A zlib result that says nothing about the gzip data: an allocation
 * failure as the adapter recorded it, or a fault of fsimg or of the zlib
 * library in the named call.
 */
static fsimg_status_t
zlib_failed(const fsimg_zlib_memory_t *memory, int code, const char *call,
    fsimg_error_t *error)
{
	char message[64];

	if (code == Z_MEM_ERROR)
		return (fsimg_zlib_memory_error(memory, error));
	if (code == Z_VERSION_ERROR)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the zlib library does not match its header"));
	snprintf(message, sizeof(message), "zlib %s returned %d", call, code);
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    message));
}

static fsimg_status_t
gzip_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * Take the next piece of the source when no input waits. Every compressed
 * byte enters here, headers and trailers included, and is charged as work
 * as it arrives, from a file or a stream alike.
 */
static fsimg_status_t
refill(gzip_decoder_t *decoder, fsimg_error_t *error)
{
	size_t count;
	fsimg_status_t status;

	if (decoder->stream.avail_in != 0 || decoder->ended)
		return (FSIMG_OK);
	status = fsimg_spool_input_read(decoder->source, decoder->input,
	    GZIP_INPUT_BYTES, &count, error);
	if (status == FSIMG_OK)
		status = fsimg_session_charge(decoder->source->session, 0, 0,
		    count, error);
	if (status != FSIMG_OK)
		return (status);
	decoder->stream.next_in = decoder->input;
	decoder->stream.avail_in = (uInt)count;
	decoder->ended = count == 0;
	return (FSIMG_OK);
}

/* One header or trailer byte, added to the header CRC when crc is given;
 * the input ending first is a truncated member.
 */
static fsimg_status_t
next_byte(gzip_decoder_t *decoder, unsigned char *byte, uLong *crc,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = refill(decoder, error);
	if (status != FSIMG_OK)
		return (status);
	if (decoder->stream.avail_in == 0)
		return (gzip_corrupt(error, "gzip member is truncated"));
	*byte = *decoder->stream.next_in++;
	decoder->stream.avail_in--;
	if (crc != NULL)
		*crc = crc32(*crc, byte, 1);
	return (FSIMG_OK);
}

/* Skip a zero-terminated FNAME or FCOMMENT of at most GZIP_TEXT_MAX bytes. */
static fsimg_status_t
skip_text(gzip_decoder_t *decoder, uLong *crc, const char *too_long,
    fsimg_error_t *error)
{
	unsigned char byte;
	fsimg_status_t status;

	for (size_t length = 1;; length++) {
		if (length > GZIP_TEXT_MAX)
			return (gzip_corrupt(error, too_long));
		status = next_byte(decoder, &byte, crc, error);
		if (status != FSIMG_OK || byte == 0)
			return (status);
	}
}

/*
 * A member header. The first member's identification, method and reserved
 * flags are what the probe claimed; a later member failing them is bytes
 * after the last member.
 */
static fsimg_status_t
read_header(gzip_decoder_t *decoder, fsimg_error_t *error)
{
	unsigned char header[GZIP_HEADER_BYTES], byte;
	unsigned extra;
	uLong crc;
	fsimg_status_t status;

	/* The identity first, so short trailing bytes are named as such. */
	crc = crc32(0, Z_NULL, 0);
	for (unsigned i = 0; i < GZIP_HEADER_BYTES; i++) {
		status = next_byte(decoder, &header[i], &crc, error);
		if (status == FSIMG_ECORRUPT && i < 4 && decoder->member > 1)
			return (gzip_corrupt(error,
			    "bytes follow the last gzip member"));
		if (status != FSIMG_OK)
			return (status);
		if (i == 3 && gzip_probe(header, 4, error) != FSIMG_OK)
			return (gzip_corrupt(error, decoder->member == 1 ?
			    "gzip header is invalid" :
			    "bytes follow the last gzip member"));
	}
	if ((header[3] & GZIP_FEXTRA) != 0) {
		extra = 0;
		for (unsigned i = 0; i < 2; i++) {
			status = next_byte(decoder, &byte, &crc, error);
			if (status != FSIMG_OK)
				return (status);
			extra |= (unsigned)byte << (8 * i);
		}
		for (unsigned i = 0; i < extra; i++) {
			status = next_byte(decoder, &byte, &crc, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	if ((header[3] & GZIP_FNAME) != 0) {
		status = skip_text(decoder, &crc, "gzip file name exceeds 64 KiB",
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	if ((header[3] & GZIP_FCOMMENT) != 0) {
		status = skip_text(decoder, &crc, "gzip comment exceeds 64 KiB",
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	if ((header[3] & GZIP_FHCRC) != 0) {
		unsigned char stored[2];

		for (unsigned i = 0; i < 2; i++) {
			status = next_byte(decoder, &stored[i], NULL, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if ((unsigned)(stored[0] | stored[1] << 8) != (crc & 0xffff))
			return (gzip_corrupt(error, "gzip header CRC16 mismatch"));
	}
	return (FSIMG_OK);
}

/*
 * Inflate one member's DEFLATE data into the spool, one output buffer at
 * a time, accumulating the CRC32 and length of what it decodes.
 */
static fsimg_status_t
inflate_member(gzip_decoder_t *decoder, uLong *crc, uint64_t *length,
    fsimg_error_t *error)
{
	z_stream *stream = &decoder->stream;
	size_t produced;
	fsimg_status_t status;
	int code;

	code = inflateReset(stream);
	if (code != Z_OK)
		return (zlib_failed(&decoder->memory, code, "inflateReset", error));
	*crc = crc32(0, Z_NULL, 0);
	*length = 0;
	do {
		status = refill(decoder, error);
		if (status != FSIMG_OK)
			return (status);
		if (stream->avail_in == 0)
			return (gzip_corrupt(error, "gzip member is truncated"));
		stream->next_out = decoder->output;
		stream->avail_out = GZIP_OUTPUT_BYTES;
		code = inflate(stream, Z_NO_FLUSH);
		/* With input and a whole output buffer available, inflate either
		 * makes progress or fails; Z_BUF_ERROR, no progress possible, is
		 * malformed data too, so every pass consumes or produces bytes.
		 * Raw DEFLATE has no dictionary, so a request for one is too.
		 */
		if (code == Z_DATA_ERROR || code == Z_BUF_ERROR ||
		    code == Z_NEED_DICT)
			return (gzip_corrupt(error,
			    "gzip member's DEFLATE data is invalid"));
		if (code != Z_OK && code != Z_STREAM_END)
			return (zlib_failed(&decoder->memory, code, "inflate",
			    error));
		produced = GZIP_OUTPUT_BYTES - stream->avail_out;
		/* The bytes produced are work, as the compressed bytes were
		 * when refill took them, so a decode costs what a container's
		 * chunk decoding costs.
		 */
		status = fsimg_session_charge(decoder->source->session, 0, 0,
		    produced, error);
		if (status != FSIMG_OK)
			return (status);
		*crc = crc32(*crc, decoder->output, (uInt)produced);
		*length += produced;
		status = fsimg_spool_append(decoder->spool, decoder->output,
		    produced, error);
		if (status != FSIMG_OK)
			return (status);
	} while (code != Z_STREAM_END);
	return (FSIMG_OK);
}

/* The trailer: the member's CRC32 and its length modulo 2^32. */
static fsimg_status_t
read_trailer(gzip_decoder_t *decoder, uLong crc, uint64_t length,
    fsimg_error_t *error)
{
	unsigned char trailer[GZIP_TRAILER_BYTES];
	uint32_t stored_crc, stored_length;
	fsimg_status_t status;

	for (unsigned i = 0; i < GZIP_TRAILER_BYTES; i++) {
		status = next_byte(decoder, &trailer[i], NULL, error);
		if (status != FSIMG_OK)
			return (status);
	}
	stored_crc = fsimg_load_le32(trailer);
	stored_length = fsimg_load_le32(trailer + 4);
	if (stored_crc != (uint32_t)crc)
		return (gzip_corrupt(error, "gzip member CRC32 mismatch"));
	if (stored_length != (uint32_t)length)
		return (gzip_corrupt(error, "gzip member length mismatch"));
	return (FSIMG_OK);
}

/*
 * Allocate the decoder's buffers and start raw inflate (negative window
 * bits: the member headers are parsed here, not by zlib). On failure the
 * caller's decoder_end() releases whatever was set up.
 */
static fsimg_status_t
decoder_start(gzip_decoder_t *decoder, fsimg_error_t *error)
{
	int code;

	decoder->input = fsimg_alloc(decoder->source->ctx, GZIP_INPUT_BYTES,
	    error);
	if (decoder->input == NULL)
		return (error->status);
	decoder->output = fsimg_alloc(decoder->source->ctx, GZIP_OUTPUT_BYTES,
	    error);
	if (decoder->output == NULL)
		return (error->status);
	fsimg_zlib_memory_bind(&decoder->memory, decoder->source->ctx,
	    &decoder->stream);
	code = inflateInit2(&decoder->stream, -MAX_WBITS);
	if (code != Z_OK)
		return (zlib_failed(&decoder->memory, code, "inflateInit2", error));
	decoder->inflating = true;
	return (FSIMG_OK);
}

static void
decoder_end(gzip_decoder_t *decoder)
{
	if (decoder->inflating)
		inflateEnd(&decoder->stream);
	fsimg_dealloc(decoder->output);
	fsimg_dealloc(decoder->input);
}

/* Members in sequence, until the input ends exactly after a trailer. */
static fsimg_status_t
decode_members(gzip_decoder_t *decoder, fsimg_error_t *error)
{
	uint64_t length = 0;
	uLong crc = 0;
	fsimg_status_t status;

	for (;;) {
		decoder->member++;
		status = read_header(decoder, error);
		if (status != FSIMG_OK)
			return (status);
		status = inflate_member(decoder, &crc, &length, error);
		if (status != FSIMG_OK)
			return (status);
		status = read_trailer(decoder, crc, length, error);
		if (status != FSIMG_OK)
			return (status);
		status = refill(decoder, error);
		if (status != FSIMG_OK)
			return (status);
		if (decoder->stream.avail_in == 0)
			return (FSIMG_OK);
	}
}

static fsimg_status_t
gzip_decode(fsimg_spool_input_t *source, fsimg_spool_t *spool,
    fsimg_error_t *error)
{
	gzip_decoder_t decoder;
	fsimg_status_t status;

	bzero(&decoder, sizeof(decoder));
	decoder.source = source;
	decoder.spool = spool;
	status = decoder_start(&decoder, error);
	if (status == FSIMG_OK)
		status = decode_members(&decoder, error);
	decoder_end(&decoder);
	return (status);
}

/*
 * The level, 1 to 9 with zero selecting 6, zlib's and gzip(1)'s default;
 * and the modification time, the epoch under FSIMG_WRITE_REPRODUCIBLE and
 * the host clock otherwise.
 */
static fsimg_status_t
gzip_plan(const fsimg_layer_options_t *layers, uint32_t flags, int64_t epoch,
    fsimg_compression_settings_t *settings, fsimg_error_t *error)
{
	settings->level = layers->gzip_level == 0 ? GZIP_DEFAULT_LEVEL :
	    layers->gzip_level;
	if (settings->level > GZIP_MAX_LEVEL)
		return (fsimg_setting_error(error, "gzip:level",
		    "gzip:level must be 1 to 9"));
	settings->timestamp = (flags & FSIMG_WRITE_REPRODUCIBLE) != 0 ? epoch :
	    (int64_t)time(NULL);
	return (FSIMG_OK);
}

/*
 * The member's header and trailer around the most DEFLATE can take for
 * length input bytes: the larger of zlib's two conservative bounds, one
 * for fixed Huffman codes (an eighth more, which also covers the nine-bit
 * literals of zlib-ng's fastest level) and one for stored blocks. Neither
 * depends on the window or memory level, so the bound holds for whichever
 * zlib the library runs against.
 */
static fsimg_status_t
gzip_bound(uint64_t length, uint64_t *out, fsimg_error_t *error)
{
	uint64_t fixed, stored;

	if (!fsimg_add_u64(length, (length >> 3) + (length >> 8) +
	    (length >> 9) + 4, &fixed) ||
	    !fsimg_add_u64(length, (length >> 5) + (length >> 7) +
	    (length >> 11) + 7, &stored) ||
	    !fsimg_add_u64(fixed > stored ? fixed : stored,
	    GZIP_HEADER_BYTES + GZIP_TRAILER_BYTES, out))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "the image is too large for gzip"));
	return (FSIMG_OK);
}

/*
 * Start raw deflate (negative window bits: the header and trailer are
 * written here, not by zlib) and write the member's header. The time
 * field holds seconds since 1970 in 32 bits; a time it cannot hold, and
 * zero itself, is zero, which RFC 1952 reads as no time stamp.
 */
static fsimg_status_t
encoder_open(fsimg_ctx_t *ctx, const fsimg_compression_settings_t *settings,
    fsimg_seq_sink_t *sink, void **state, fsimg_error_t *error)
{
	unsigned char header[GZIP_HEADER_BYTES];
	gzip_encoder_t *encoder;
	uint32_t mtime;
	int code;

	encoder = fsimg_alloc(ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	bzero(encoder, sizeof(*encoder));
	*state = encoder;
	encoder->sink = sink;
	encoder->output = fsimg_alloc(ctx, GZIP_PIECE_BYTES, error);
	if (encoder->output == NULL)
		return (error->status);
	encoder->zeros = fsimg_alloc(ctx, GZIP_PIECE_BYTES, error);
	if (encoder->zeros == NULL)
		return (error->status);
	bzero(encoder->zeros, GZIP_PIECE_BYTES);
	fsimg_zlib_memory_bind(&encoder->memory, ctx, &encoder->stream);
	code = deflateInit2(&encoder->stream, (int)settings->level, Z_DEFLATED,
	    -MAX_WBITS, GZIP_MEMORY_LEVEL, Z_DEFAULT_STRATEGY);
	if (code != Z_OK)
		return (zlib_failed(&encoder->memory, code, "deflateInit2", error));
	encoder->deflating = true;
	encoder->crc = crc32(0, Z_NULL, 0);

	mtime = settings->timestamp < 1 || settings->timestamp > UINT32_MAX ?
	    0 : (uint32_t)settings->timestamp;
	header[0] = 0x1f;
	header[1] = 0x8b;
	header[2] = GZIP_METHOD_DEFLATE;
	header[3] = 0;
	fsimg_store_le32(header + 4, mtime);
	header[8] = header_xfl(settings->level);
	header[9] = GZIP_OS_UNIX;
	return (fsimg_seq_write(sink, header, sizeof(header), error));
}

/* The extra flags RFC 1952 defines: the slowest or the fastest level. */
static unsigned char
header_xfl(uint32_t level)
{
	if (level == GZIP_MAX_LEVEL)
		return (GZIP_XFL_SLOWEST);
	if (level == 1)
		return (GZIP_XFL_FASTEST);
	return (0);
}

/*
 * Run deflate over its pending input with flush, writing each full or
 * final output buffer to the sink. Without Z_FINISH the input is consumed
 * once deflate leaves output room; with it the stream ends. Anything else
 * is zlib breaking its contract, an internal error.
 */
static fsimg_status_t
deflate_out(gzip_encoder_t *encoder, int flush, fsimg_error_t *error)
{
	z_stream *stream = &encoder->stream;
	fsimg_status_t status;
	int code;

	do {
		stream->next_out = encoder->output;
		stream->avail_out = GZIP_PIECE_BYTES;
		code = deflate(stream, flush);
		if (code != Z_OK && code != Z_STREAM_END && code != Z_BUF_ERROR)
			return (zlib_failed(&encoder->memory, code, "deflate",
			    error));
		status = fsimg_seq_write(encoder->sink, encoder->output,
		    GZIP_PIECE_BYTES - stream->avail_out, error);
		if (status != FSIMG_OK)
			return (status);
	} while (stream->avail_out == 0 && code != Z_STREAM_END);
	if (flush == Z_FINISH && code != Z_STREAM_END)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "zlib did not end the DEFLATE stream"));
	if (stream->avail_in != 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "zlib left its input unconsumed"));
	return (FSIMG_OK);
}

/* Deflate length bytes, or length zeros, in pieces zlib's 32-bit counts
 * hold, with the member's CRC32 and length over them.
 */
static fsimg_status_t
encoder_write(void *state, const unsigned char *bytes, uint64_t length,
    fsimg_error_t *error)
{
	gzip_encoder_t *encoder = state;
	const unsigned char *input;
	fsimg_status_t status;
	uInt piece;

	status = FSIMG_OK;
	while (status == FSIMG_OK && length > 0) {
		piece = length > GZIP_PIECE_BYTES ? GZIP_PIECE_BYTES :
		    (uInt)length;
		input = bytes != NULL ? bytes : encoder->zeros;
		encoder->crc = crc32(encoder->crc, input, piece);
		encoder->length += piece;
		encoder->stream.next_in = (Bytef *)(uintptr_t)input;
		encoder->stream.avail_in = piece;
		status = deflate_out(encoder, Z_NO_FLUSH, error);
		if (bytes != NULL)
			bytes += piece;
		length -= piece;
	}
	return (status);
}

/* End the DEFLATE stream, then the trailer: the CRC32 and the length
 * modulo 2^32 of the bytes written, little-endian.
 */
static fsimg_status_t
encoder_finish(void *state, fsimg_error_t *error)
{
	gzip_encoder_t *encoder = state;
	unsigned char trailer[GZIP_TRAILER_BYTES];
	fsimg_status_t status;

	encoder->stream.next_in = Z_NULL;
	encoder->stream.avail_in = 0;
	status = deflate_out(encoder, Z_FINISH, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_le32(trailer, (uint32_t)encoder->crc);
	fsimg_store_le32(trailer + 4, (uint32_t)encoder->length);
	return (fsimg_seq_write(encoder->sink, trailer, sizeof(trailer), error));
}

static void
encoder_close(void *state)
{
	gzip_encoder_t *encoder = state;

	if (encoder->deflating)
		deflateEnd(&encoder->stream);
	fsimg_dealloc(encoder->zeros);
	fsimg_dealloc(encoder->output);
	fsimg_dealloc(encoder);
}
