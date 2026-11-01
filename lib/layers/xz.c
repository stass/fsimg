/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include <lzma.h>

#include "codec-memory.h"
#include "host.h"
#include "layer.h"

/*
 * xz over the system liblzma (the .xz file format, version 1.2.1).
 * Compiled only when configure found liblzma; the registry row names
 * these operations under FSIMG_XZ.
 *
 * Read side. The probe claims on the six magic bytes. liblzma's stream
 * decoder does the rest: concatenated streams and the stream padding
 * between them, every header, index and footer CRC32, each block's check,
 * and the trailing-byte rule. It runs with no memory limit of its own:
 * every allocation, the dictionary included, goes through the codec
 * memory adapter and is charged to the context, so the memory budget is
 * the one bound. Decoded bytes go to the spool in bounded steps, each
 * reserved before it is written. A stream whose check is none, or one
 * this liblzma cannot compute, still decodes, and the layer report names
 * xz:check unverified.
 *
 * Write side. One stream of one block from lzma_easy_encoder with the
 * preset and check the settings name: no extreme flag, no filter beyond
 * LZMA2 and no threads, so the bytes depend only on the input, the
 * settings and the liblzma version. The encoder is a filter over the
 * transcode's sequential sink with its state charged to the context.
 *
 * liblzma's messages are never copied: each failure is fsimg's own text
 * with the library's numeric code and, when decoding, the compressed
 * offset where the decoder stopped.
 */
#define XZ_INPUT_BYTES		(64u * 1024)
#define XZ_OUTPUT_BYTES		(256u * 1024)
/* The encoder's unit: its output buffer and its input pieces. */
#define XZ_PIECE_BYTES		(64u * 1024)
#define XZ_MAX_LEVEL		9u

static const unsigned char xz_magic[6] = { 0xfd, '7', 'z', 'X', 'Z', 0x00 };

/*
 * One decode. Input bytes wait in stream.next_in and stream.avail_in;
 * stream.total_in is the compressed offset of the next one. ended is set
 * once the source has nothing more, and unchecked once a stream without
 * a check this liblzma computes was announced.
 */
typedef struct xz_decoder {
	lzma_stream	 stream;
	lzma_allocator	 allocator;
	fsimg_codec_memory_t memory;
	fsimg_spool_input_t *source;
	fsimg_spool_t	*spool;
	unsigned char	*input;
	unsigned char	*output;
	bool		 ended;
	bool		 unchecked;
	bool		 decoding;	/* lzma_stream_decoder succeeded */
} xz_decoder_t;

/* One encode into sink; zeros is a piece of zeros a zero run is read from. */
typedef struct xz_encoder {
	lzma_stream	 stream;
	lzma_allocator	 allocator;
	fsimg_codec_memory_t memory;
	fsimg_seq_sink_t *sink;
	unsigned char	*output;
	unsigned char	*zeros;
	bool		 encoding;	/* lzma_easy_encoder succeeded */
} xz_encoder_t;

static void *lzma_alloc_shim(void *, size_t, size_t);
static void lzma_free_shim(void *, void *);
static void lzma_bind(fsimg_codec_memory_t *, fsimg_ctx_t *,
    lzma_allocator *, lzma_stream *);
static void xz_version(char *, size_t);
static fsimg_status_t xz_probe(const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t xz_decode(fsimg_spool_input_t *, fsimg_spool_t *,
    bool *, fsimg_error_t *);
static fsimg_status_t xz_plan(const fsimg_layer_options_t *, uint32_t,
    int64_t, fsimg_compression_settings_t *, fsimg_error_t *);
static fsimg_status_t xz_bound(uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t encoder_open(fsimg_ctx_t *,
    const fsimg_compression_settings_t *, fsimg_seq_sink_t *, void **,
    fsimg_error_t *);
static fsimg_status_t encoder_write(void *, const unsigned char *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t encoder_finish(void *, fsimg_error_t *);
static void encoder_close(void *);
static fsimg_status_t encode_out(xz_encoder_t *, lzma_action,
    fsimg_error_t *);
static fsimg_status_t memory_failed(const fsimg_codec_memory_t *,
    fsimg_error_t *);
static fsimg_status_t call_failed(const fsimg_codec_memory_t *, lzma_ret,
    const char *, fsimg_error_t *);
static fsimg_status_t decode_failed(const xz_decoder_t *, lzma_ret,
    fsimg_error_t *);
static fsimg_status_t refill(xz_decoder_t *, fsimg_error_t *);
static fsimg_status_t decoder_start(xz_decoder_t *, fsimg_error_t *);
static void decoder_end(xz_decoder_t *);
static fsimg_status_t decode_streams(xz_decoder_t *, fsimg_error_t *);

static const fsimg_compressor_ops_t xz_encoder = {
	.open = encoder_open,
	.write = encoder_write,
	.finish = encoder_finish,
	.close = encoder_close
};

const fsimg_compression_ops_t fsimg_xz_compression = {
	.probe_bytes = sizeof(xz_magic),
	.probe = xz_probe,
	.decode = xz_decode,
	.unverified = "xz:check",
	.plan = xz_plan,
	.bound = xz_bound,
	.encoder = &xz_encoder,
	.version = xz_version
};

/* liblzma's binding of the codec memory adapter. */
static void *
lzma_alloc_shim(void *opaque, size_t items, size_t size)
{
	return (fsimg_codec_memory_alloc(opaque, items, size));
}

static void
lzma_free_shim(void *opaque FSIMG_UNUSED, void *address)
{
	fsimg_codec_memory_free(address);
}

/*
 * Bind a fresh stream, before its decoder or encoder is initialized, to a
 * fresh adapter: both callbacks and the opaque pointer together.
 */
static void
lzma_bind(fsimg_codec_memory_t *memory, fsimg_ctx_t *ctx,
    lzma_allocator *allocator, lzma_stream *stream)
{
	fsimg_codec_memory_init(memory, ctx);
	allocator->alloc = lzma_alloc_shim;
	allocator->free = lzma_free_shim;
	allocator->opaque = memory;
	stream->allocator = allocator;
}

static void
xz_version(char *out, size_t capacity)
{
	snprintf(out, capacity, "liblzma %s", lzma_version_string());
}

static fsimg_status_t
xz_probe(const unsigned char *prefix, size_t length, fsimg_error_t *error)
{
	if (length < sizeof(xz_magic) || bcmp(prefix, xz_magic,
	    sizeof(xz_magic)) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no xz signature"));
	return (FSIMG_OK);
}

/*
 * liblzma could not allocate: the adapter's first error, a memory limit
 * that keeps its figures, named for xz, since an allocation failure's
 * own diagnostic does not say which layer asked.
 */
static fsimg_status_t
memory_failed(const fsimg_codec_memory_t *memory, fsimg_error_t *error)
{
	fsimg_codec_memory_error(memory, error);
	if (error->path[0] == '\0')
		snprintf(error->path, sizeof(error->path), "xz");
	return (error->status);
}

/*
 * A decoder result other than progress, classified by liblzma's closed
 * return set: memory as the adapter recorded it; a memory limit, which
 * fsimg never sets, or a programming error as internal; an option this
 * liblzma lacks as unsupported; and everything else as corruption at the
 * compressed offset, since the input is untrusted and a code new to this
 * table cannot be a parameter error of fsimg's.
 */
static fsimg_status_t
decode_failed(const xz_decoder_t *decoder, lzma_ret code,
    fsimg_error_t *error)
{
	unsigned long long offset = decoder->stream.total_in;
	char message[128];

	switch (code) {
	case LZMA_MEM_ERROR:
		return (memory_failed(&decoder->memory, error));
	case LZMA_MEMLIMIT_ERROR:
	case LZMA_PROG_ERROR:
	case LZMA_GET_CHECK:
		snprintf(message, sizeof(message),
		    "liblzma lzma_code returned %d", (int)code);
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    message));
	case LZMA_OPTIONS_ERROR:
		snprintf(message, sizeof(message),
		    "xz stream uses a filter or option this liblzma does not "
		    "decode, at compressed byte %llu", offset);
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, message));
	case LZMA_FORMAT_ERROR:
		snprintf(message, sizeof(message),
		    "xz stream header is invalid at compressed byte %llu",
		    offset);
		break;
	case LZMA_BUF_ERROR:
		/* liblzma reads what follows a stream as the next stream's
		 * header, so a short run of trailing bytes ends the input
		 * inside a header too.
		 */
		snprintf(message, sizeof(message),
		    "xz data ends inside a stream or has trailing bytes, at "
		    "compressed byte %llu", offset);
		break;
	default:
		snprintf(message, sizeof(message),
		    "xz data is invalid at compressed byte %llu (liblzma code "
		    "%d)", offset, (int)code);
		break;
	}
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * Take the next piece of the source when no input waits. Every compressed
 * byte enters here and is charged as work as it arrives, from a file or a
 * stream alike; the read polls cancellation.
 */
static fsimg_status_t
refill(xz_decoder_t *decoder, fsimg_error_t *error)
{
	size_t count;
	fsimg_status_t status;

	if (decoder->stream.avail_in != 0 || decoder->ended)
		return (FSIMG_OK);
	status = fsimg_spool_input_read(decoder->source, decoder->input,
	    XZ_INPUT_BYTES, &count, error);
	if (status == FSIMG_OK)
		status = fsimg_session_charge(decoder->source->session, 0, 0,
		    count, error);
	if (status != FSIMG_OK)
		return (status);
	decoder->stream.next_in = decoder->input;
	decoder->stream.avail_in = count;
	decoder->ended = count == 0;
	return (FSIMG_OK);
}

/*
 * Allocate the buffers and start the stream decoder: concatenated
 * streams, told of a stream without a usable check, and no memory limit
 * beyond the context's. On failure the caller's decoder_end() releases
 * whatever was set up.
 */
static fsimg_status_t
decoder_start(xz_decoder_t *decoder, fsimg_error_t *error)
{
	lzma_ret code;

	decoder->input = fsimg_alloc(decoder->source->ctx, XZ_INPUT_BYTES,
	    error);
	if (decoder->input == NULL)
		return (error->status);
	decoder->output = fsimg_alloc(decoder->source->ctx, XZ_OUTPUT_BYTES,
	    error);
	if (decoder->output == NULL)
		return (error->status);
	lzma_bind(&decoder->memory, decoder->source->ctx, &decoder->allocator,
	    &decoder->stream);
	code = lzma_stream_decoder(&decoder->stream, UINT64_MAX,
	    LZMA_CONCATENATED | LZMA_TELL_NO_CHECK |
	    LZMA_TELL_UNSUPPORTED_CHECK);
	if (code != LZMA_OK)
		return (call_failed(&decoder->memory, code,
		    "lzma_stream_decoder", error));
	decoder->decoding = true;
	return (FSIMG_OK);
}

static void
decoder_end(xz_decoder_t *decoder)
{
	if (decoder->decoding)
		lzma_end(&decoder->stream);
	fsimg_dealloc(decoder->output);
	fsimg_dealloc(decoder->input);
}

/*
 * Decode until the input ends exactly at the end of a stream or its
 * padding. Once the source has ended the decoder is told to finish, and
 * LZMA_STREAM_END is the only success; an input that ends inside a stream
 * makes liblzma report no possible progress: the first call that makes
 * none returns LZMA_OK and the next LZMA_BUF_ERROR, which is a
 * truncation, so the loop ends. Each call's output is spooled only after
 * its result is known to be progress.
 */
static fsimg_status_t
decode_streams(xz_decoder_t *decoder, fsimg_error_t *error)
{
	lzma_stream *stream = &decoder->stream;
	size_t produced;
	fsimg_status_t status;
	lzma_ret code;

	for (;;) {
		status = refill(decoder, error);
		if (status != FSIMG_OK)
			return (status);
		stream->next_out = decoder->output;
		stream->avail_out = XZ_OUTPUT_BYTES;
		code = lzma_code(stream, decoder->ended ? LZMA_FINISH :
		    LZMA_RUN);
		if (code == LZMA_NO_CHECK || code == LZMA_UNSUPPORTED_CHECK) {
			decoder->unchecked = true;
			code = LZMA_OK;
		}
		if (code != LZMA_OK && code != LZMA_STREAM_END)
			return (decode_failed(decoder, code, error));
		produced = XZ_OUTPUT_BYTES - stream->avail_out;
		/* The bytes produced are work, as the compressed bytes were
		 * when refill took them.
		 */
		status = fsimg_session_charge(decoder->source->session, 0, 0,
		    produced, error);
		if (status == FSIMG_OK)
			status = fsimg_spool_append(decoder->spool,
			    decoder->output, produced, error);
		if (status != FSIMG_OK || code == LZMA_STREAM_END)
			return (status);
	}
}

static fsimg_status_t
xz_decode(fsimg_spool_input_t *source, fsimg_spool_t *spool, bool *unchecked,
    fsimg_error_t *error)
{
	xz_decoder_t decoder;
	fsimg_status_t status;

	bzero(&decoder, sizeof(decoder));
	decoder.stream = (lzma_stream)LZMA_STREAM_INIT;
	decoder.source = source;
	decoder.spool = spool;
	status = decoder_start(&decoder, error);
	if (status == FSIMG_OK)
		status = decode_streams(&decoder, error);
	decoder_end(&decoder);
	*unchecked = decoder.unchecked;
	return (status);
}

/*
 * The preset, 0 to 9, which fsimg_layer_options_init sets to 6, xz(1)'s
 * default; and the check, CRC64 unless the options name CRC32 or SHA-256.
 * An unchecked stream is never written.
 */
static fsimg_status_t
xz_plan(const fsimg_layer_options_t *layers, uint32_t flags FSIMG_UNUSED,
    int64_t epoch FSIMG_UNUSED, fsimg_compression_settings_t *settings,
    fsimg_error_t *error)
{
	if (layers->xz_level > XZ_MAX_LEVEL)
		return (fsimg_setting_error(error, "xz:level",
		    "xz:level must be 0 to 9"));
	settings->level = layers->xz_level;
	switch (layers->xz_check) {
	case FSIMG_XZ_CRC64:
		settings->check = LZMA_CHECK_CRC64;
		break;
	case FSIMG_XZ_CRC32:
		settings->check = LZMA_CHECK_CRC32;
		break;
	case FSIMG_XZ_SHA256:
		settings->check = LZMA_CHECK_SHA256;
		break;
	default:
		return (fsimg_setting_error(error, "xz:check",
		    "xz:check must be crc32, crc64 or sha256"));
	}
	settings->timestamp = 0;
	return (FSIMG_OK);
}

/*
 * The most one stream of one block can take for length input bytes, by a
 * rule that rests on LZMA2's chunking rather than on a liblzma version; a
 * liblzma that broke it would fail as an internal error when the sink
 * refused the byte past the bound. The streaming encoder ends a chunk
 * when its compressed data nears 64 KiB or its input reaches 2 MiB, and
 * stores a chunk that did not shrink uncompressed, so no chunk is more
 * than six header bytes longer than the input it holds and each holds
 * about 64 KiB or more: length / 4096 allows sixteen bytes per 64 KiB.
 * The constant holds the stream header and footer (24), the block header
 * (at most 1024), the block padding (3), the check (at most 64), and the
 * index with its padding and CRC32 (under 64).
 */
static fsimg_status_t
xz_bound(uint64_t length, uint64_t *out, fsimg_error_t *error)
{
	if (!fsimg_add_u64(length, (length >> 12) + 2048, out))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "the image is too large for xz"));
	return (FSIMG_OK);
}

/*
 * A failed liblzma call whose parameters are fsimg's own: a decoder's or
 * encoder's initialization, or an encoding step over fsimg's transcode
 * with settings the plan validated. Every code but an allocation failure
 * is therefore an internal error, named with the call.
 */
static fsimg_status_t
call_failed(const fsimg_codec_memory_t *memory, lzma_ret code,
    const char *call, fsimg_error_t *error)
{
	char message[64];

	if (code == LZMA_MEM_ERROR)
		return (memory_failed(memory, error));
	snprintf(message, sizeof(message), "liblzma %s returned %d", call,
	    (int)code);
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * Start the stream encoder. Its whole state, the preset's match finder
 * and dictionary included, is allocated here through the adapter, so a
 * preset the memory budget cannot hold fails now, before any byte is
 * written.
 */
static fsimg_status_t
encoder_open(fsimg_ctx_t *ctx, const fsimg_compression_settings_t *settings,
    fsimg_seq_sink_t *sink, void **state, fsimg_error_t *error)
{
	xz_encoder_t *encoder;
	lzma_ret code;

	encoder = fsimg_alloc(ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	bzero(encoder, sizeof(*encoder));
	encoder->stream = (lzma_stream)LZMA_STREAM_INIT;
	*state = encoder;
	encoder->sink = sink;
	encoder->output = fsimg_alloc(ctx, XZ_PIECE_BYTES, error);
	if (encoder->output == NULL)
		return (error->status);
	encoder->zeros = fsimg_alloc(ctx, XZ_PIECE_BYTES, error);
	if (encoder->zeros == NULL)
		return (error->status);
	bzero(encoder->zeros, XZ_PIECE_BYTES);
	lzma_bind(&encoder->memory, ctx, &encoder->allocator, &encoder->stream);
	code = lzma_easy_encoder(&encoder->stream, settings->level,
	    (lzma_check)settings->check);
	if (code != LZMA_OK)
		return (call_failed(&encoder->memory, code,
		    "lzma_easy_encoder", error));
	encoder->encoding = true;
	return (FSIMG_OK);
}

/*
 * Run the encoder over its pending input with action, writing each full
 * or final output buffer to the sink. Under LZMA_RUN the input is
 * consumed once the encoder leaves output room; under LZMA_FINISH the
 * stream ends.
 */
static fsimg_status_t
encode_out(xz_encoder_t *encoder, lzma_action action, fsimg_error_t *error)
{
	lzma_stream *stream = &encoder->stream;
	fsimg_status_t status;
	lzma_ret code;

	do {
		stream->next_out = encoder->output;
		stream->avail_out = XZ_PIECE_BYTES;
		code = lzma_code(stream, action);
		if (code != LZMA_OK && code != LZMA_STREAM_END)
			return (call_failed(&encoder->memory, code,
			    "lzma_code", error));
		status = fsimg_seq_write(encoder->sink, encoder->output,
		    XZ_PIECE_BYTES - stream->avail_out, error);
		if (status != FSIMG_OK)
			return (status);
	} while (stream->avail_out == 0 && code != LZMA_STREAM_END);
	if (action == LZMA_FINISH && code != LZMA_STREAM_END)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "liblzma did not end the xz stream"));
	if (stream->avail_in != 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "liblzma left its input unconsumed"));
	return (FSIMG_OK);
}

/* Encode length bytes, or length zeros, in pieces of XZ_PIECE_BYTES. */
static fsimg_status_t
encoder_write(void *state, const unsigned char *bytes, uint64_t length,
    fsimg_error_t *error)
{
	xz_encoder_t *encoder = state;
	fsimg_status_t status;
	size_t piece;

	status = FSIMG_OK;
	while (status == FSIMG_OK && length > 0) {
		piece = length > XZ_PIECE_BYTES ? XZ_PIECE_BYTES :
		    (size_t)length;
		encoder->stream.next_in = bytes != NULL ? bytes :
		    encoder->zeros;
		encoder->stream.avail_in = piece;
		status = encode_out(encoder, LZMA_RUN, error);
		if (bytes != NULL)
			bytes += piece;
		length -= piece;
	}
	return (status);
}

/* End the block and the stream: the check, the index and the footer. */
static fsimg_status_t
encoder_finish(void *state, fsimg_error_t *error)
{
	xz_encoder_t *encoder = state;

	encoder->stream.next_in = NULL;
	encoder->stream.avail_in = 0;
	return (encode_out(encoder, LZMA_FINISH, error));
}

static void
encoder_close(void *state)
{
	xz_encoder_t *encoder = state;

	if (encoder->encoding)
		lzma_end(&encoder->stream);
	fsimg_dealloc(encoder->zeros);
	fsimg_dealloc(encoder->output);
	fsimg_dealloc(encoder);
}
