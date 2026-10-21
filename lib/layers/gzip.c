/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "host.h"
#include "layer.h"

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

/*
 * One decode. Input bytes wait in stream.next_in and stream.avail_in,
 * where inflate leaves whatever follows a member's DEFLATE data, so the
 * header and trailer parsers read from the same place. ended is set once
 * the source has nothing more.
 */
typedef struct gzip_decoder {
	z_stream	 stream;
	fsimg_ctx_t	*ctx;
	fsimg_spool_input_t	*source;
	fsimg_spool_t	*spool;
	unsigned char	*input;
	unsigned char	*output;
	fsimg_error_t	 zlib_error;	/* Why zlib's last allocation failed */
	uint64_t	 member;	/* 1-based, for messages */
	bool		 ended;
	bool		 inflating;	/* inflateInit2 succeeded */
} gzip_decoder_t;

static fsimg_status_t gzip_probe(const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t gzip_decode(fsimg_spool_input_t *, fsimg_spool_t *,
    fsimg_error_t *);
static voidpf decoder_alloc(voidpf, uInt, uInt);
static void decoder_free(voidpf, voidpf);
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

const fsimg_compression_ops_t fsimg_gzip_compression = {
	.probe_bytes = 4,
	.probe = gzip_probe,
	.decode = gzip_decode
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

static voidpf
decoder_alloc(voidpf opaque, uInt items, uInt size)
{
	gzip_decoder_t *decoder = opaque;

	if (size != 0 && items > SIZE_MAX / size)
		return (Z_NULL);
	return (fsimg_alloc(decoder->ctx, (size_t)items * size,
	    &decoder->zlib_error));
}

static void
decoder_free(voidpf opaque FSIMG_UNUSED, voidpf address)
{
	fsimg_dealloc(address);
}

static fsimg_status_t
gzip_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/* Take the next piece of the source when no input waits. */
static fsimg_status_t
refill(gzip_decoder_t *decoder, fsimg_error_t *error)
{
	size_t count;
	fsimg_status_t status;

	if (decoder->stream.avail_in != 0 || decoder->ended)
		return (FSIMG_OK);
	status = fsimg_spool_input_read(decoder->source, decoder->input,
	    GZIP_INPUT_BYTES, &count, error);
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

	if (inflateReset(stream) != Z_OK)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "zlib refused to reset the gzip decoder"));
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
		fsimg_error_clear(&decoder->zlib_error);
		code = inflate(stream, Z_NO_FLUSH);
		if (code == Z_MEM_ERROR) {
			*error = decoder->zlib_error;
			return (error->status != FSIMG_OK ? error->status :
			    fsimg_error_set(error, FSIMG_ENOMEM, 0,
			    FSIMG_OP_IMAGE, "gzip decoder allocation failed"));
		}
		/* With input and a whole output buffer available, inflate either
		 * makes progress or fails; Z_BUF_ERROR, no progress possible, is
		 * malformed data too, so every pass consumes or produces bytes.
		 */
		if (code != Z_OK && code != Z_STREAM_END)
			return (gzip_corrupt(error,
			    "gzip member's DEFLATE data is invalid"));
		produced = GZIP_OUTPUT_BYTES - stream->avail_out;
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
	stored_crc = fsimg_layer_le32(trailer);
	stored_length = fsimg_layer_le32(trailer + 4);
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

	decoder->input = fsimg_alloc(decoder->ctx, GZIP_INPUT_BYTES, error);
	if (decoder->input == NULL)
		return (error->status);
	decoder->output = fsimg_alloc(decoder->ctx, GZIP_OUTPUT_BYTES, error);
	if (decoder->output == NULL)
		return (error->status);
	decoder->stream.zalloc = decoder_alloc;
	decoder->stream.zfree = decoder_free;
	decoder->stream.opaque = decoder;
	code = inflateInit2(&decoder->stream, -MAX_WBITS);
	if (code == Z_MEM_ERROR && decoder->zlib_error.status != FSIMG_OK) {
		*error = decoder->zlib_error;
		return (error->status);
	}
	if (code == Z_MEM_ERROR)
		return (fsimg_error_set(error, FSIMG_ENOMEM, 0, FSIMG_OP_IMAGE,
		    "gzip decoder allocation failed"));
	if (code != Z_OK)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "zlib refused to start a gzip decoder"));
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
	uint64_t length;
	uLong crc;
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
	decoder.ctx = source->ctx;
	decoder.source = source;
	decoder.spool = spool;
	status = decoder_start(&decoder, error);
	if (status == FSIMG_OK)
		status = decode_members(&decoder, error);
	decoder_end(&decoder);
	return (status);
}
