/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "io.h"
#include "layer.h"

/*
 * The Harman-Becker IFS container, hbcifs (specs/design-layers.md, the
 * hbcifs profile), read only. A 64-byte little-endian header precedes one
 * unframed LZO1X stream that runs to the end of the file and decodes to
 * an ImageFS:
 *
 *	0x00	 8	"hbcifs" and two NUL bytes: the identity
 *	0x08	 4	decoded length: the disk length, the decoder's bound
 *	0x0c	 4	payload length: the file length minus 64
 *	0x10	 4	unidentified; 32
 *	0x14	 4	unidentified; reported, never checked
 *	0x18	 4	unidentified; the bytes 02 01 01 00
 *	0x1c	 4	unidentified; reported, never checked
 *	0x20	32	zero
 *
 * One producer's output is known, so the values the closed table names
 * at 0x10, 0x18 and 0x20 are required and any other is unsupported rather
 * than guessed at. The stream has no framing to index, so open decodes it
 * once into the spool: the decoder reads the payload in 64 KiB pieces and
 * writes through a 64 KiB window behind 64 KiB of history, more than the
 * 49151 bytes a match may reach back, and each window's output is
 * appended to the spool, which reserves it against the spool limit first.
 * The disk is then the spool. Nothing is left for verification to repeat;
 * the image reports the two fields no evidence explains as unverified.
 */
#define HBCIFS_HEADER_BYTES	64u
#define HBCIFS_PIECE_BYTES	(64u * 1024)
#define HBCIFS_HISTORY_BYTES	(64u * 1024)
#define HBCIFS_FIELD_10		32u

/* Header fields. */
enum {
	HEADER_IDENTITY = 0x00,
	HEADER_DECODED = 0x08,
	HEADER_PAYLOAD = 0x0c,
	HEADER_FIELD_10 = 0x10,
	HEADER_FIELD_14 = 0x14,
	HEADER_FIELD_18 = 0x18,
	HEADER_FIELD_1C = 0x1c,
	HEADER_RESERVED = 0x20
};

static const unsigned char hbcifs_identity[8] = "hbcifs\0";
static const unsigned char hbcifs_field_18[4] = { 0x02, 0x01, 0x01, 0x00 };

static fsimg_status_t hbcifs_failure(fsimg_error_t *, fsimg_status_t,
    const char *);
static fsimg_status_t hbcifs_probe(const unsigned char *, size_t,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static size_t keep_history(unsigned char *, size_t);
static fsimg_status_t stream_failure(const fsimg_lzo1x_decoder_t *,
    fsimg_error_t *, const char *);
static fsimg_status_t stream_end(const fsimg_lzo1x_decoder_t *, bool,
    uint32_t, fsimg_error_t *);
static fsimg_status_t decode_payload(const fsimg_container_input_t *,
    uint32_t, fsimg_error_t *);
static fsimg_status_t disk_resolve(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t disk_next_zero(void *, uint64_t, uint64_t *, uint64_t *,
    fsimg_error_t *);
static void disk_release(void *);

/* The decoded disk is the sealed spool, which answers for itself. */
static const fsimg_io_resolver_ops_t disk_ops = {
	.resolve = disk_resolve,
	.next_zero = disk_next_zero,
	.release = disk_release
};
static void report_header(const unsigned char *, fsimg_container_t *);
static fsimg_status_t hbcifs_open(const fsimg_container_input_t *,
    fsimg_container_t *, fsimg_error_t *);
static fsimg_status_t hbcifs_verify(const fsimg_container_input_t *,
    const fsimg_container_t *, bool, fsimg_error_t *);

const fsimg_container_ops_t fsimg_hbcifs_container = {
	.probe_bytes = HBCIFS_HEADER_BYTES,
	.probe = hbcifs_probe,
	.open = hbcifs_open,
	.verify = hbcifs_verify,
	.codecs = "lzo1x",
	.carries = "ifs",
	.unverified = "0x14,0x1c",
	.payload_checks = false,
	.zlib = false,
	.spooled = true
};

static fsimg_status_t
hbcifs_failure(fsimg_error_t *error, fsimg_status_t status,
    const char *message)
{
	return (fsimg_error_set(error, status, 0, FSIMG_OP_IMAGE, message));
}

/*
 * The identity claims; then the tie between the payload length and the
 * file, or a decoded length of zero, which no ImageFS has, is corrupt, and
 * a value outside the closed table is unsupported, naming its field. The
 * decoded length bounds the decoder and is checked by the decode itself.
 */
static fsimg_status_t
hbcifs_probe(const unsigned char *prefix, size_t length,
    const unsigned char *tail FSIMG_UNUSED, size_t tail_length FSIMG_UNUSED,
    uint64_t file_bytes, fsimg_error_t *error)
{
	if (length < sizeof(hbcifs_identity) ||
	    bcmp(prefix + HEADER_IDENTITY, hbcifs_identity,
	    sizeof(hbcifs_identity)) != 0)
		return (hbcifs_failure(error, FSIMG_EFORMAT,
		    "no hbcifs identity"));
	if (length < HBCIFS_HEADER_BYTES || file_bytes < HBCIFS_HEADER_BYTES)
		return (hbcifs_failure(error, FSIMG_ECORRUPT,
		    "hbcifs header is truncated"));
	if (fsimg_layer_le32(prefix + HEADER_PAYLOAD) !=
	    file_bytes - HBCIFS_HEADER_BYTES)
		return (hbcifs_failure(error, FSIMG_ECORRUPT,
		    "hbcifs payload length is not the file length minus 64"));
	if (fsimg_layer_le32(prefix + HEADER_DECODED) == 0)
		return (hbcifs_failure(error, FSIMG_ECORRUPT,
		    "hbcifs decoded length is zero"));
	if (fsimg_layer_le32(prefix + HEADER_FIELD_10) != HBCIFS_FIELD_10)
		return (hbcifs_failure(error, FSIMG_EUNSUPPORTED,
		    "hbcifs header field 0x10 holds an unobserved value"));
	if (bcmp(prefix + HEADER_FIELD_18, hbcifs_field_18,
	    sizeof(hbcifs_field_18)) != 0)
		return (hbcifs_failure(error, FSIMG_EUNSUPPORTED,
		    "hbcifs header field 0x18 holds an unobserved value"));
	for (size_t i = HEADER_RESERVED; i < HBCIFS_HEADER_BYTES; i++) {
		if (prefix[i] != 0)
			return (hbcifs_failure(error, FSIMG_EUNSUPPORTED,
			    "hbcifs reserved header bytes are not zero"));
	}
	return (FSIMG_OK);
}

/* The most recent HBCIFS_HISTORY_BYTES of filled window bytes, moved to
 * the window's start; returns how many it kept.
 */
static size_t
keep_history(unsigned char *window, size_t filled)
{
	if (filled <= HBCIFS_HISTORY_BYTES)
		return (filled);
	bcopy(window + (filled - HBCIFS_HISTORY_BYTES), window,
	    HBCIFS_HISTORY_BYTES);
	return (HBCIFS_HISTORY_BYTES);
}

/* A failure at the decoder's input position, as a file offset. */
static fsimg_status_t
stream_failure(const fsimg_lzo1x_decoder_t *decoder, fsimg_error_t *error,
    const char *message)
{
	hbcifs_failure(error, FSIMG_ECORRUPT, message);
	if (error != NULL) {
		error->offset = HBCIFS_HEADER_BYTES + decoder->stream_in;
		error->offset_valid = true;
	}
	return (FSIMG_ECORRUPT);
}

/*
 * The verdict once the decoder reaches the end marker: the marker must
 * end the payload, final being true only when no payload is left to
 * read, and the stream must have produced exactly the decoded length.
 * Longer output never gets here; the decoder refuses it at its limit.
 */
static fsimg_status_t
stream_end(const fsimg_lzo1x_decoder_t *decoder, bool final, uint32_t decoded,
    fsimg_error_t *error)
{
	if (!final)
		return (stream_failure(decoder, error,
		    "hbcifs payload continues after the end marker"));
	if (decoder->stream_out != decoded)
		return (stream_failure(decoder, error,
		    "hbcifs stream ends short of its decoded length"));
	return (FSIMG_OK);
}

/*
 * Decode the payload, the file after the header, into the spool. Each
 * decoder call stops at one of three edges: it consumed what remains of
 * the current piece, it filled the window, or it reached the end marker.
 * The loop therefore always advances: an exhausted piece is refilled, a
 * full window is trimmed to its history, and the end marker, or an
 * exhausted final piece the decoder calls unterminated, ends it. Each
 * call's consumed and produced bytes are charged to the work counter,
 * which polls cancellation, and its output is appended to the spool,
 * which reserves the spool limit before the file grows. Corruption the
 * codec finds is the image's: it keeps its message and its stream offset
 * becomes a file offset. Any other codec status is a broken caller
 * contract and stays the codec's.
 */
static fsimg_status_t
decode_payload(const fsimg_container_input_t *input, uint32_t decoded,
    fsimg_error_t *error)
{
	fsimg_spool_input_t source = { .ctx = input->ctx, .file = input->file,
	    .session = input->session, .offset = HBCIFS_HEADER_BYTES,
	    .file_bytes = input->file_bytes, .fd = -1 };
	fsimg_lzo1x_decoder_t decoder;
	fsimg_codec_step_t step;
	unsigned char *piece, *window;
	size_t have, used, history;
	bool final;
	fsimg_status_t status;

	piece = fsimg_alloc(input->ctx, HBCIFS_PIECE_BYTES, error);
	if (piece == NULL)
		return (error->status);
	window = fsimg_alloc(input->ctx, HBCIFS_HISTORY_BYTES +
	    HBCIFS_PIECE_BYTES, error);
	if (window == NULL) {
		fsimg_dealloc(piece);
		return (error->status);
	}
	fsimg_lzo1x_decoder_init(&decoder, decoded);
	have = used = history = 0;
	for (;;) {
		if (used == have && source.offset < source.file_bytes) {
			status = fsimg_spool_input_read(&source, piece,
			    HBCIFS_PIECE_BYTES, &have, error);
			if (status != FSIMG_OK)
				break;
			used = 0;
		}
		final = source.offset == source.file_bytes;
		status = fsimg_lzo1x_decode(&decoder, piece + used, have - used,
		    final, window, history, HBCIFS_PIECE_BYTES, &step, error);
		if (status != FSIMG_OK) {
			if (status == FSIMG_ECORRUPT) {
				error->operation = FSIMG_OP_IMAGE;
				error->offset += HBCIFS_HEADER_BYTES;
			}
			break;
		}
		status = fsimg_session_charge(input->session, 0, 0,
		    (uint64_t)step.consumed + step.produced, error);
		if (status == FSIMG_OK)
			status = fsimg_spool_append(input->spool, window + history,
			    step.produced, error);
		if (status != FSIMG_OK)
			break;
		used += step.consumed;
		if (step.done) {
			status = stream_end(&decoder, final, decoded, error);
			break;
		}
		history = keep_history(window, history + step.produced);
	}
	fsimg_dealloc(window);
	fsimg_dealloc(piece);
	return (status);
}

/* The disk reads the sealed spool, its argument. */
static fsimg_status_t
disk_resolve(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	return (fsimg_io_read_exact(argument, offset, buffer, length,
	    transferred, error));
}

static fsimg_status_t
disk_next_zero(void *argument, uint64_t offset, uint64_t *start, uint64_t *end,
    fsimg_error_t *error)
{
	return (fsimg_io_next_zero_range(argument, offset, start, end, error));
}

static void
disk_release(void *argument)
{
	fsimg_io_free(argument);
}

/* Info's details: the two lengths and the four unidentified words. */
static void
report_header(const unsigned char *header, fsimg_container_t *out)
{
	static const struct {
		const char	*name;
		size_t		 offset;
	} fields[] = {
		{ "decoded_bytes", HEADER_DECODED },
		{ "payload_bytes", HEADER_PAYLOAD },
		{ "field_0x10", HEADER_FIELD_10 },
		{ "field_0x14", HEADER_FIELD_14 },
		{ "field_0x18", HEADER_FIELD_18 },
		{ "field_0x1c", HEADER_FIELD_1C }
	};

	_Static_assert(sizeof(fields) / sizeof(fields[0]) <=
	    FSIMG_IMAGE_DETAILS_MAX, "the header's fields fit the details");
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		snprintf(out->details[i].name, sizeof(out->details[i].name),
		    "%s", fields[i].name);
		out->details[i].value = fsimg_layer_le32(header +
		    fields[i].offset);
	}
	out->detail_count = sizeof(fields) / sizeof(fields[0]);
}

/*
 * Validate the header as the probe does, decode the payload into the
 * spool and yield the sealed spool as the disk. The disk carries the
 * input's identity, so alias checks see the file the container came from,
 * not the unlinked spool. Without a spool the open fails as a spool limit
 * of zero, as a compressed input does.
 */
static fsimg_status_t
hbcifs_open(const fsimg_container_input_t *input, fsimg_container_t *out,
    fsimg_error_t *error)
{
	unsigned char header[HBCIFS_HEADER_BYTES];
	fsimg_io_t *spooled;
	size_t transferred;
	uint32_t decoded;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	if (input->file_bytes < HBCIFS_HEADER_BYTES)
		return (hbcifs_failure(error, FSIMG_ECORRUPT,
		    "hbcifs header is truncated"));
	status = fsimg_io_read_session(input->file, input->session, 0, header,
	    sizeof(header), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	status = hbcifs_probe(header, sizeof(header), NULL, 0,
	    input->file_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (input->spool == NULL)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SPOOL, 0, 0));
	decoded = fsimg_layer_le32(header + HEADER_DECODED);
	status = decode_payload(input, decoded, error);
	if (status == FSIMG_OK)
		status = fsimg_spool_seal(input->spool, &spooled, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_resolver(input->ctx, input->file, decoded,
	    &disk_ops, spooled, &out->disk, error);
	if (status != FSIMG_OK)
		return (status);
	out->disk_bytes = decoded;
	out->chunks = 1;
	report_header(header, out);
	return (FSIMG_OK);
}

/*
 * The identity, the length tie, the closed table and the exact decode all
 * held at open, so there is nothing to repeat. The fields at 0x14 and
 * 0x1c have no known rule; the image reports them from the profile's
 * unverified list and calls the verification incomplete.
 */
static fsimg_status_t
hbcifs_verify(const fsimg_container_input_t *input FSIMG_UNUSED,
    const fsimg_container_t *container FSIMG_UNUSED, bool payload FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}
