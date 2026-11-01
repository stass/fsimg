/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* The custom allocator and the frame header parser are in this section. */
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

#include "codec-memory.h"
#include "host.h"
#include "layer.h"

/*
 * zstd over the system libzstd (RFC 8878). Compiled only when configure
 * found a libzstd with the custom-allocator interface; the registry row
 * names these operations under FSIMG_ZSTD.
 *
 * Read side. The probe claims on a frame's magic number or a skippable
 * frame's. Frames are decoded in sequence, as zstd(1) reads them, with
 * skippable frames producing nothing. At each frame boundary this file
 * parses the frame header from its own input buffer: a frame that names a
 * dictionary is unsupported, one without a content checksum makes the
 * layer report name zstd:checksum unverified, and a declared content size
 * is the length the frame's decoded bytes must reach exactly. The window
 * is limited to 128 MiB before the library allocates anything, and every
 * allocation the library makes for a current-format frame goes through
 * the codec memory adapter, so the memory budget bounds the rest. Decoded
 * bytes go to the spool in bounded steps, each reserved before it is
 * written.
 *
 * The header parse is also what keeps the budget whole: libzstd's stream
 * decoder would decode a legacy (v0.5 to v0.7) frame too, with decoders
 * that allocate through malloc, outside the adapter. The header parser
 * accepts only current and skippable frames, and libzstd stops at the end
 * of every frame, so no byte reaches the library before its frame's
 * header passed here.
 *
 * Write side. One frame from ZSTD_compressStream2 on a context whose
 * parameters are all set here: the level, a content checksum, the content
 * size when the plan knows the input's exact length, and no worker
 * threads, so the bytes depend only on the input, the settings and the
 * libzstd version.
 *
 * libzstd's error names are never copied: each failure is fsimg's own
 * text with the library's numeric code and, when decoding, the compressed
 * offset where the failing call's input began, at or before the point
 * where the library detected the failure (a checksum is detected only at
 * the frame's end).
 */
#define ZSTD_INPUT_BYTES	(64u * 1024)
#define ZSTD_OUTPUT_BYTES	(256u * 1024)
/* The encoder's unit: its output buffer and its input pieces. */
#define ZSTD_PIECE_BYTES	(64u * 1024)
#define ZSTD_DEFAULT_LEVEL	3u
#define ZSTD_MAX_LEVEL		19u	/* zstd(1)'s regular levels */
/* The largest window a frame may declare: zstd(1)'s own default limit. */
#define ZSTD_WINDOW_LOG_MAX	27
/* A frame header is at most 18 bytes; a skippable one's is 8. */
#define ZSTD_HEADER_MAX		18u

/*
 * One decode. The input buffer holds the compressed bytes from offset
 * base on, of which in.pos have been consumed and in.size are present.
 * ended is set once the source has nothing more. At a frame boundary
 * (boundary) the next frame's header has yet to be parsed; inside a frame
 * expected is its declared content size, ZSTD_CONTENTSIZE_UNKNOWN when it
 * declares none, and decoded what it has produced so far.
 */
typedef struct zstd_decoder {
	ZSTD_DStream	*stream;
	fsimg_codec_memory_t memory;
	fsimg_spool_input_t *source;
	fsimg_spool_t	*spool;
	unsigned char	*input;
	unsigned char	*output;
	ZSTD_inBuffer	 in;
	uint64_t	 base;
	uint64_t	 expected;
	uint64_t	 decoded;
	bool		 ended;
	bool		 boundary;
	bool		 unchecked;
} zstd_decoder_t;

/* One encode into sink; zeros is a piece of zeros a zero run is read from. */
typedef struct zstd_encoder {
	ZSTD_CStream	*stream;
	fsimg_codec_memory_t memory;
	fsimg_seq_sink_t *sink;
	unsigned char	*output;
	unsigned char	*zeros;
} zstd_encoder_t;

static void *zstd_alloc_shim(void *, size_t);
static void zstd_free_shim(void *, void *);
static ZSTD_customMem zstd_bind(fsimg_codec_memory_t *, fsimg_ctx_t *);
static void zstd_version(char *, size_t);
static fsimg_status_t zstd_probe(const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t zstd_decode(fsimg_spool_input_t *, fsimg_spool_t *,
    bool *, fsimg_error_t *);
static fsimg_status_t zstd_plan(const fsimg_layer_options_t *, uint32_t,
    int64_t, fsimg_compression_settings_t *, fsimg_error_t *);
static fsimg_status_t zstd_bound(uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t encoder_open(fsimg_ctx_t *,
    const fsimg_compression_settings_t *, fsimg_seq_sink_t *, void **,
    fsimg_error_t *);
static fsimg_status_t encoder_write(void *, const unsigned char *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t encoder_finish(void *, fsimg_error_t *);
static void encoder_close(void *);
static fsimg_status_t encode_step(zstd_encoder_t *, ZSTD_inBuffer *,
    ZSTD_EndDirective, size_t *, fsimg_error_t *);
static fsimg_status_t memory_failed(const fsimg_codec_memory_t *,
    fsimg_error_t *);
static fsimg_status_t call_failed(const fsimg_codec_memory_t *, size_t,
    const char *, fsimg_error_t *);
static fsimg_status_t decode_failed(const zstd_decoder_t *, size_t,
    fsimg_error_t *);
static fsimg_status_t zstd_corrupt(const zstd_decoder_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t refill(zstd_decoder_t *, size_t, fsimg_error_t *);
static fsimg_status_t frame_start(zstd_decoder_t *, fsimg_error_t *);
static fsimg_status_t decoder_start(zstd_decoder_t *, fsimg_error_t *);
static void decoder_end(zstd_decoder_t *);
static fsimg_status_t decode_frames(zstd_decoder_t *, fsimg_error_t *);

static const fsimg_compressor_ops_t zstd_encoder = {
	.open = encoder_open,
	.write = encoder_write,
	.finish = encoder_finish,
	.close = encoder_close
};

const fsimg_compression_ops_t fsimg_zstd_compression = {
	.probe_bytes = 4,
	.probe = zstd_probe,
	.decode = zstd_decode,
	.unverified = "zstd:checksum",
	.plan = zstd_plan,
	.bound = zstd_bound,
	.encoder = &zstd_encoder,
	.version = zstd_version
};

/* libzstd's binding of the codec memory adapter: one item of size bytes. */
static void *
zstd_alloc_shim(void *opaque, size_t size)
{
	return (fsimg_codec_memory_alloc(opaque, 1, size));
}

static void
zstd_free_shim(void *opaque FSIMG_UNUSED, void *address)
{
	fsimg_codec_memory_free(address);
}

/*
 * A fresh adapter and the allocator record that installs both callbacks
 * with it; libzstd copies the record into the context it creates.
 */
static ZSTD_customMem
zstd_bind(fsimg_codec_memory_t *memory, fsimg_ctx_t *ctx)
{
	fsimg_codec_memory_init(memory, ctx);
	return ((ZSTD_customMem){ .customAlloc = zstd_alloc_shim,
	    .customFree = zstd_free_shim, .opaque = memory });
}

static void
zstd_version(char *out, size_t capacity)
{
	snprintf(out, capacity, "zstd %s", ZSTD_versionString());
}

/* A frame's magic number, or a skippable frame's, 0x184D2A50 to 5F. */
static fsimg_status_t
zstd_probe(const unsigned char *prefix, size_t length, fsimg_error_t *error)
{
	if (length >= 4 && prefix[1] == 0x2a && prefix[2] == 0x4d &&
	    prefix[3] == 0x18 && (prefix[0] & 0xf0) == 0x50)
		return (FSIMG_OK);
	if (length >= 4 && prefix[0] == 0x28 && prefix[1] == 0xb5 &&
	    prefix[2] == 0x2f && prefix[3] == 0xfd)
		return (FSIMG_OK);
	return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
	    "no zstd signature"));
}

/*
 * libzstd could not allocate: the adapter's first error, a memory limit
 * that keeps its figures, named for zstd, since an allocation failure's
 * own diagnostic does not say which layer asked.
 */
static fsimg_status_t
memory_failed(const fsimg_codec_memory_t *memory, fsimg_error_t *error)
{
	fsimg_codec_memory_error(memory, error);
	if (error->path[0] == '\0')
		snprintf(error->path, sizeof(error->path), "zstd");
	return (error->status);
}

/* The longest detail zstd_corrupt reports, with its NUL, and its message:
 * the detail between the prefix and the suffix, and a 64-bit offset.
 */
#define ZSTD_DETAIL_BYTES	48
#define ZSTD_CORRUPT_BYTES	(ZSTD_DETAIL_BYTES + \
	    sizeof("zstd  at compressed byte ") + 20)

static fsimg_status_t
zstd_corrupt(const zstd_decoder_t *decoder, const char *what,
    fsimg_error_t *error)
{
	char message[ZSTD_CORRUPT_BYTES];

	snprintf(message, sizeof(message), "zstd %s at compressed byte %llu",
	    what, (unsigned long long)(decoder->base + decoder->in.pos));
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * A decoder error, classified by libzstd's error codes: memory as the
 * adapter recorded it; a window above the limit, a dictionary or a frame
 * parameter this libzstd does not decode as unsupported; and everything
 * else as corruption at the compressed offset, since the input is
 * untrusted and a code new to this table cannot be a parameter error of
 * fsimg's.
 */
static fsimg_status_t
decode_failed(const zstd_decoder_t *decoder, size_t result,
    fsimg_error_t *error)
{
	char message[ZSTD_DETAIL_BYTES];

	switch (ZSTD_getErrorCode(result)) {
	case ZSTD_error_memory_allocation:
		return (memory_failed(&decoder->memory, error));
	case ZSTD_error_frameParameter_windowTooLarge:
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "zstd frame's window exceeds 128 MiB"));
	case ZSTD_error_dictionary_wrong:
	case ZSTD_error_dictionary_corrupted:
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "zstd frame needs a dictionary"));
	case ZSTD_error_frameParameter_unsupported:
	case ZSTD_error_version_unsupported:
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE,
		    "zstd frame uses a parameter this libzstd does not decode"));
	case ZSTD_error_checksum_wrong:
		return (zstd_corrupt(decoder,
		    "frame's content checksum does not match", error));
	case ZSTD_error_corruption_detected:
		return (zstd_corrupt(decoder, "data is corrupt", error));
	default:
		snprintf(message, sizeof(message),
		    "data is invalid (libzstd code %d)",
		    (int)ZSTD_getErrorCode(result));
		return (zstd_corrupt(decoder, message, error));
	}
}

/*
 * Make at least want bytes available at in.pos, or all the source has
 * left: the unconsumed bytes move to the buffer's start and the source
 * fills the rest. Every compressed byte enters here and is charged as
 * work as it arrives; the read polls cancellation.
 */
static fsimg_status_t
refill(zstd_decoder_t *decoder, size_t want, fsimg_error_t *error)
{
	ZSTD_inBuffer *in = &decoder->in;
	size_t count;
	fsimg_status_t status;

	while (in->size - in->pos < want && !decoder->ended) {
		if (in->pos != 0) {
			memmove(decoder->input, decoder->input + in->pos,
			    in->size - in->pos);
			decoder->base += in->pos;
			in->size -= in->pos;
			in->pos = 0;
		}
		status = fsimg_spool_input_read(decoder->source,
		    decoder->input + in->size, ZSTD_INPUT_BYTES - in->size,
		    &count, error);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(decoder->source->session,
			    0, 0, count, error);
		if (status != FSIMG_OK)
			return (status);
		in->size += count;
		decoder->ended = count == 0;
	}
	return (FSIMG_OK);
}

/*
 * Parse the next frame's header where the previous frame ended, or at the
 * start, where the probe has seen a magic number. Bytes after a frame that
 * are not a current or skippable frame, a legacy frame among them, are
 * trailing bytes; too few to parse are a truncated header; a header
 * libzstd refuses otherwise, such as a window beyond the format's or a
 * reserved bit, is classified as a decoder error is.
 */
static fsimg_status_t
frame_start(zstd_decoder_t *decoder, fsimg_error_t *error)
{
	ZSTD_frameHeader header;
	size_t result;
	fsimg_status_t status;

	status = refill(decoder, ZSTD_HEADER_MAX, error);
	if (status != FSIMG_OK)
		return (status);
	result = ZSTD_getFrameHeader(&header,
	    decoder->input + decoder->in.pos,
	    decoder->in.size - decoder->in.pos);
	if (ZSTD_isError(result) &&
	    ZSTD_getErrorCode(result) == ZSTD_error_prefix_unknown)
		return (zstd_corrupt(decoder,
		    "data has trailing bytes that are not a frame", error));
	if (ZSTD_isError(result))
		return (decode_failed(decoder, result, error));
	if (result != 0)
		return (zstd_corrupt(decoder, "frame header is truncated",
		    error));
	if (header.frameType == ZSTD_skippableFrame) {
		decoder->expected = 0;
	} else {
		if (header.dictID != 0)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, "zstd frame needs a dictionary"));
		if (header.windowSize > (UINT64_C(1) << ZSTD_WINDOW_LOG_MAX))
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE,
			    "zstd frame's window exceeds 128 MiB"));
		if (!header.checksumFlag)
			decoder->unchecked = true;
		decoder->expected = header.frameContentSize;
	}
	decoder->decoded = 0;
	decoder->boundary = false;
	return (FSIMG_OK);
}

/*
 * Allocate the buffers and create the stream decoder through the adapter,
 * with the window limit set before any frame is seen. On failure the
 * caller's decoder_end() releases whatever was set up.
 */
static fsimg_status_t
decoder_start(zstd_decoder_t *decoder, fsimg_error_t *error)
{
	size_t result;

	decoder->input = fsimg_alloc(decoder->source->ctx, ZSTD_INPUT_BYTES,
	    error);
	if (decoder->input == NULL)
		return (error->status);
	decoder->output = fsimg_alloc(decoder->source->ctx, ZSTD_OUTPUT_BYTES,
	    error);
	if (decoder->output == NULL)
		return (error->status);
	decoder->stream = ZSTD_createDStream_advanced(zstd_bind(
	    &decoder->memory, decoder->source->ctx));
	if (decoder->stream == NULL)
		return (memory_failed(&decoder->memory, error));
	result = ZSTD_DCtx_setParameter(decoder->stream, ZSTD_d_windowLogMax,
	    ZSTD_WINDOW_LOG_MAX);
	if (ZSTD_isError(result))
		return (call_failed(&decoder->memory, result,
		    "ZSTD_DCtx_setParameter", error));
	decoder->in = (ZSTD_inBuffer){ .src = decoder->input };
	decoder->boundary = true;
	return (FSIMG_OK);
}

static void
decoder_end(zstd_decoder_t *decoder)
{
	ZSTD_freeDStream(decoder->stream);
	fsimg_dealloc(decoder->output);
	fsimg_dealloc(decoder->input);
}

/*
 * Frames in sequence until the input ends exactly at the end of one.
 * ZSTD_decompressStream returns zero once a frame is decoded and flushed;
 * otherwise it stops when the output buffer is full, which only another
 * call drains, or when it has consumed all the input present, which the
 * source then tops up. Those three are the only ways a call returns, so
 * a call that leaves room in the output with the source ended and its
 * input consumed has stopped inside a frame: a truncation. The library
 * stops at the end of each frame, so every frame start passes frame_start
 * first. Each call's output is spooled only after its result is known not
 * to be an error.
 */
static fsimg_status_t
decode_frames(zstd_decoder_t *decoder, fsimg_error_t *error)
{
	ZSTD_outBuffer out;
	size_t result;
	fsimg_status_t status;

	for (;;) {
		if (decoder->boundary) {
			status = refill(decoder, 1, error);
			if (status != FSIMG_OK)
				return (status);
			if (decoder->in.pos == decoder->in.size)
				return (FSIMG_OK);
			status = frame_start(decoder, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = refill(decoder, 1, error);
		if (status != FSIMG_OK)
			return (status);
		out = (ZSTD_outBuffer){ .dst = decoder->output,
		    .size = ZSTD_OUTPUT_BYTES };
		result = ZSTD_decompressStream(decoder->stream, &out,
		    &decoder->in);
		if (ZSTD_isError(result))
			return (decode_failed(decoder, result, error));
		/* The bytes produced are work, as the compressed bytes were
		 * when refill took them.
		 */
		status = fsimg_session_charge(decoder->source->session, 0, 0,
		    out.pos, error);
		if (status == FSIMG_OK)
			status = fsimg_spool_append(decoder->spool,
			    decoder->output, out.pos, error);
		if (status != FSIMG_OK)
			return (status);
		decoder->decoded += out.pos;
		if (decoder->expected != ZSTD_CONTENTSIZE_UNKNOWN &&
		    decoder->decoded > decoder->expected)
			return (zstd_corrupt(decoder,
			    "frame exceeds its content size", error));
		if (result == 0) {
			if (decoder->expected != ZSTD_CONTENTSIZE_UNKNOWN &&
			    decoder->decoded != decoder->expected)
				return (zstd_corrupt(decoder,
				    "frame is shorter than its content size",
				    error));
			decoder->boundary = true;
		} else if (out.pos < out.size && decoder->ended &&
		    decoder->in.pos == decoder->in.size) {
			return (zstd_corrupt(decoder, "frame is truncated",
			    error));
		}
	}
}

static fsimg_status_t
zstd_decode(fsimg_spool_input_t *source, fsimg_spool_t *spool,
    bool *unchecked, fsimg_error_t *error)
{
	zstd_decoder_t decoder;
	fsimg_status_t status;

	bzero(&decoder, sizeof(decoder));
	decoder.source = source;
	decoder.spool = spool;
	status = decoder_start(&decoder, error);
	if (status == FSIMG_OK)
		status = decode_frames(&decoder, error);
	decoder_end(&decoder);
	*unchecked = decoder.unchecked;
	return (status);
}

/* The level, 1 to 19 with zero selecting 3, zstd(1)'s default. */
static fsimg_status_t
zstd_plan(const fsimg_layer_options_t *layers, uint32_t flags FSIMG_UNUSED,
    int64_t epoch FSIMG_UNUSED, fsimg_compression_settings_t *settings,
    fsimg_error_t *error)
{
	settings->level = layers->zstd_level == 0 ? ZSTD_DEFAULT_LEVEL :
	    layers->zstd_level;
	if (settings->level > ZSTD_MAX_LEVEL)
		return (fsimg_setting_error(error, "zstd:level",
		    "zstd:level must be 1 to 19"));
	settings->timestamp = 0;
	return (FSIMG_OK);
}

/*
 * The most one frame can take for length input bytes: the frame header
 * (at most 18 bytes), the checksum (4) and length + length / 256 + 512,
 * which exceeds ZSTD_compressBound() for every length. It rests on
 * libzstd's block policy, which stores incompressible input as raw blocks
 * of 128 KiB, each behind a 3-byte header; a library that broke it would
 * fail as an internal error when the sink refuses the byte past the bound.
 */
static fsimg_status_t
zstd_bound(uint64_t length, uint64_t *out, fsimg_error_t *error)
{
	if (!fsimg_add_u64(length, (length >> 8) + 534, out))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "the image is too large for zstd"));
	return (FSIMG_OK);
}

/*
 * A failed call that untrusted input cannot cause: setting a parameter,
 * or encoding fsimg's own transcode with the level the plan validated.
 * Every code but an allocation failure is an internal error, named with
 * the call; a pledged-size mismatch is internal too.
 */
static fsimg_status_t
call_failed(const fsimg_codec_memory_t *memory, size_t result,
    const char *call, fsimg_error_t *error)
{
	char message[80];

	if (ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation)
		return (memory_failed(memory, error));
	snprintf(message, sizeof(message), "libzstd %s failed with code %d",
	    call, (int)ZSTD_getErrorCode(result));
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * Create the stream encoder through the adapter and fix every parameter
 * the frame's shape depends on. An empty first step makes libzstd
 * allocate its whole workspace now, so a level the memory budget cannot
 * hold fails here, before any byte is written.
 */
static fsimg_status_t
encoder_open(fsimg_ctx_t *ctx, const fsimg_compression_settings_t *settings,
    fsimg_seq_sink_t *sink, void **state, fsimg_error_t *error)
{
	static const ZSTD_cParameter parameters[] = { ZSTD_c_compressionLevel,
	    ZSTD_c_checksumFlag, ZSTD_c_contentSizeFlag, ZSTD_c_nbWorkers };
	ZSTD_inBuffer empty = { .src = NULL };
	zstd_encoder_t *encoder;
	size_t result, pending;
	int values[4];

	encoder = fsimg_alloc(ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	bzero(encoder, sizeof(*encoder));
	*state = encoder;
	encoder->sink = sink;
	encoder->output = fsimg_alloc(ctx, ZSTD_PIECE_BYTES, error);
	if (encoder->output == NULL)
		return (error->status);
	encoder->zeros = fsimg_alloc(ctx, ZSTD_PIECE_BYTES, error);
	if (encoder->zeros == NULL)
		return (error->status);
	bzero(encoder->zeros, ZSTD_PIECE_BYTES);
	encoder->stream = ZSTD_createCStream_advanced(zstd_bind(
	    &encoder->memory, ctx));
	if (encoder->stream == NULL)
		return (memory_failed(&encoder->memory, error));
	values[0] = (int)settings->level;
	values[1] = 1;
	values[2] = 1;
	values[3] = 0;
	for (size_t i = 0; i < sizeof(parameters) / sizeof(parameters[0]);
	    i++) {
		result = ZSTD_CCtx_setParameter(encoder->stream, parameters[i],
		    values[i]);
		if (ZSTD_isError(result))
			return (call_failed(&encoder->memory, result,
			    "ZSTD_CCtx_setParameter", error));
	}
	if (settings->length != FSIMG_COMPRESSION_LENGTH_UNKNOWN) {
		result = ZSTD_CCtx_setPledgedSrcSize(encoder->stream,
		    settings->length);
		if (ZSTD_isError(result))
			return (call_failed(&encoder->memory, result,
			    "ZSTD_CCtx_setPledgedSrcSize", error));
	}
	return (encode_step(encoder, &empty, ZSTD_e_continue, &pending,
	    error));
}

/*
 * One ZSTD_compressStream2 call over in with directive, its output
 * written to the sink; pending is what libzstd still holds to flush.
 */
static fsimg_status_t
encode_step(zstd_encoder_t *encoder, ZSTD_inBuffer *in,
    ZSTD_EndDirective directive, size_t *pending, fsimg_error_t *error)
{
	ZSTD_outBuffer out = { .dst = encoder->output,
	    .size = ZSTD_PIECE_BYTES };
	size_t result;

	result = ZSTD_compressStream2(encoder->stream, &out, in, directive);
	if (ZSTD_isError(result))
		return (call_failed(&encoder->memory, result,
		    "ZSTD_compressStream2", error));
	*pending = result;
	return (fsimg_seq_write(encoder->sink, encoder->output, out.pos,
	    error));
}

/* Encode length bytes, or length zeros, in pieces of ZSTD_PIECE_BYTES. */
static fsimg_status_t
encoder_write(void *state, const unsigned char *bytes, uint64_t length,
    fsimg_error_t *error)
{
	zstd_encoder_t *encoder = state;
	ZSTD_inBuffer in;
	fsimg_status_t status;
	size_t piece, pending;

	status = FSIMG_OK;
	while (status == FSIMG_OK && length > 0) {
		piece = length > ZSTD_PIECE_BYTES ? ZSTD_PIECE_BYTES :
		    (size_t)length;
		in = (ZSTD_inBuffer){ .src = bytes != NULL ? bytes :
		    encoder->zeros, .size = piece };
		while (status == FSIMG_OK && in.pos < in.size)
			status = encode_step(encoder, &in, ZSTD_e_continue,
			    &pending, error);
		if (bytes != NULL)
			bytes += piece;
		length -= piece;
	}
	return (status);
}

/*
 * End the frame: the last block and the checksum, flushed until libzstd
 * holds nothing more. A pledged length the input did not match fails
 * here.
 */
static fsimg_status_t
encoder_finish(void *state, fsimg_error_t *error)
{
	zstd_encoder_t *encoder = state;
	ZSTD_inBuffer empty = { .src = NULL };
	fsimg_status_t status;
	size_t pending;

	do {
		status = encode_step(encoder, &empty, ZSTD_e_end, &pending,
		    error);
	} while (status == FSIMG_OK && pending != 0);
	return (status);
}

static void
encoder_close(void *state)
{
	zstd_encoder_t *encoder = state;

	ZSTD_freeCStream(encoder->stream);
	fsimg_dealloc(encoder->zeros);
	fsimg_dealloc(encoder->output);
	fsimg_dealloc(encoder);
}
