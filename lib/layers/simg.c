/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "io.h"
#include "layer.h"

/*
 * Android sparse images, as libsparse writes them: a 28-byte little-endian
 * header, then chunks in disk order, each a 12-byte header and a payload.
 * A RAW chunk carries its blocks, FILL one 32-bit value repeated across
 * them, DONT_CARE nothing (its blocks read as zeros), and CRC32 the CRC of
 * the expanded bytes before it. The chunks are walked from the file once
 * on open, each becoming a chunk record, and once more for verification.
 */
#define SIMG_MAGIC		UINT32_C(0xed26ff3a)
#define SIMG_HEADER_BYTES	28u
#define SIMG_CHUNK_HEADER_BYTES	12u
#define SIMG_VALUE_BYTES	4u
#define CHUNK_RAW		0xcac1u
#define CHUNK_FILL		0xcac2u
#define CHUNK_DONT_CARE		0xcac3u
#define CHUNK_CRC32		0xcac4u
#define VERIFY_PIECE_BYTES	(64u * 1024)

/* Header fields. */
enum {
	HEADER_MAGIC = 0,
	HEADER_MAJOR = 4,
	HEADER_MINOR = 6,
	HEADER_SIZE = 8,
	HEADER_CHUNK_HEADER_SIZE = 10,
	HEADER_BLOCK_SIZE = 12,
	HEADER_TOTAL_BLOCKS = 16,
	HEADER_TOTAL_CHUNKS = 20
};

/* Chunk header fields. */
enum {
	CHUNK_TYPE = 0,
	CHUNK_BLOCKS = 4,
	CHUNK_TOTAL_SIZE = 8
};

typedef struct simg_header {
	uint64_t	 disk_bytes;
	uint32_t	 block_bytes;
	uint32_t	 total_blocks;
	uint32_t	 total_chunks;
} simg_header_t;

/* One chunk as the walk decodes it: its disk range, where its payload
 * starts in the file, and a FILL or CRC32 chunk's value.
 */
typedef struct simg_chunk {
	uint64_t	 disk_offset;
	uint64_t	 length;
	uint64_t	 payload_offset;
	uint32_t	 value;
	uint16_t	 type;
} simg_chunk_t;

typedef fsimg_status_t (*simg_visit_t)(void *, const simg_chunk_t *,
    fsimg_error_t *);

/* The verification's running CRC32 over the expanded image. */
typedef struct simg_verify {
	fsimg_io_t	*disk;
	fsimg_session_t	*session;
	unsigned char	*piece;
	uLong		 crc;
	bool		 payload;
} simg_verify_t;

static fsimg_status_t simg_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t simg_probe(const unsigned char *, size_t,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static fsimg_status_t read_header(const fsimg_container_input_t *,
    simg_header_t *, fsimg_error_t *);
static fsimg_status_t read_bytes(const fsimg_container_input_t *, uint64_t,
    void *, size_t, fsimg_error_t *);
static fsimg_status_t decode_chunk(const fsimg_container_input_t *,
    const simg_header_t *, uint64_t, simg_chunk_t *, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t walk_chunks(const fsimg_container_input_t *,
    const simg_header_t *, simg_visit_t, void *, fsimg_error_t *);
static fsimg_status_t append_chunk(void *, const simg_chunk_t *,
    fsimg_error_t *);
static fsimg_status_t simg_open(const fsimg_container_input_t *,
    fsimg_container_t *, fsimg_error_t *);
static fsimg_status_t verify_chunk(void *, const simg_chunk_t *,
    fsimg_error_t *);
static fsimg_status_t simg_verify(const fsimg_container_input_t *,
    const fsimg_container_t *, bool, fsimg_error_t *);

const fsimg_container_ops_t fsimg_simg_container = {
	.probe_bytes = SIMG_HEADER_BYTES,
	.probe = simg_probe,
	.open = simg_open,
	.verify = simg_verify,
	.codecs = "raw,zero,fill",
	.payload_checks = true,
	.zlib = false
};

static fsimg_status_t
simg_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "sparse image %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/* The identity is the magic. */
static fsimg_status_t
simg_probe(const unsigned char *prefix, size_t prefix_length,
    const unsigned char *tail FSIMG_UNUSED, size_t tail_length FSIMG_UNUSED,
    uint64_t file_bytes FSIMG_UNUSED, fsimg_error_t *error)
{
	if (prefix_length < 4 || fsimg_layer_le32(prefix + HEADER_MAGIC) != SIMG_MAGIC)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no sparse image magic"));
	return (FSIMG_OK);
}

static fsimg_status_t
read_bytes(const fsimg_container_input_t *input, uint64_t offset,
    void *buffer, size_t length, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_read_session(input->file, input->session, offset,
	    buffer, length, &transferred, error));
}

/* Read and validate the header: version 1.0, the standard header sizes,
 * and a block size that is a nonzero multiple of 4.
 */
static fsimg_status_t
read_header(const fsimg_container_input_t *input, simg_header_t *header,
    fsimg_error_t *error)
{
	unsigned char raw[SIMG_HEADER_BYTES];
	fsimg_status_t status;

	if (input->file_bytes < SIMG_HEADER_BYTES)
		return (simg_corrupt(error, "header is truncated"));
	status = read_bytes(input, 0, raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	status = simg_probe(raw, sizeof(raw), NULL, 0, input->file_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (fsimg_layer_le16(raw + HEADER_MAJOR) != 1)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "sparse image major version is not supported"));
	if (fsimg_layer_le16(raw + HEADER_MINOR) != 0)
		return (simg_corrupt(error, "minor version is not 0"));
	if (fsimg_layer_le16(raw + HEADER_SIZE) != SIMG_HEADER_BYTES ||
	    fsimg_layer_le16(raw + HEADER_CHUNK_HEADER_SIZE) !=
	    SIMG_CHUNK_HEADER_BYTES)
		return (simg_corrupt(error, "header sizes are not 28 and 12"));
	header->block_bytes = fsimg_layer_le32(raw + HEADER_BLOCK_SIZE);
	header->total_blocks = fsimg_layer_le32(raw + HEADER_TOTAL_BLOCKS);
	header->total_chunks = fsimg_layer_le32(raw + HEADER_TOTAL_CHUNKS);
	if (header->block_bytes == 0 || header->block_bytes % 4 != 0)
		return (simg_corrupt(error, "block size is not a multiple of 4"));
	/* Two 32-bit factors cannot overflow 64 bits. */
	header->disk_bytes = (uint64_t)header->block_bytes * header->total_blocks;
	return (FSIMG_OK);
}

/*
 * Decode the chunk whose header starts at offset, checking that it lies in
 * the file, that its total size is the one its type implies, and that only
 * a CRC32 chunk covers no blocks. *next receives the next header's offset.
 */
static fsimg_status_t
decode_chunk(const fsimg_container_input_t *input, const simg_header_t *header,
    uint64_t offset, simg_chunk_t *chunk, uint64_t *next, fsimg_error_t *error)
{
	unsigned char raw[SIMG_CHUNK_HEADER_BYTES + SIMG_VALUE_BYTES];
	uint64_t blocks, total, expected;
	fsimg_status_t status;

	if (!fsimg_range_fits(offset, SIMG_CHUNK_HEADER_BYTES, input->file_bytes))
		return (simg_corrupt(error, "chunk header lies past the end of the file"));
	status = read_bytes(input, offset, raw, SIMG_CHUNK_HEADER_BYTES, error);
	if (status != FSIMG_OK)
		return (status);
	chunk->type = fsimg_layer_le16(raw + CHUNK_TYPE);
	blocks = fsimg_layer_le32(raw + CHUNK_BLOCKS);
	total = fsimg_layer_le32(raw + CHUNK_TOTAL_SIZE);
	chunk->length = blocks * header->block_bytes;
	chunk->payload_offset = offset + SIMG_CHUNK_HEADER_BYTES;
	chunk->value = 0;

	switch (chunk->type) {
	case CHUNK_RAW:
		expected = SIMG_CHUNK_HEADER_BYTES + chunk->length;
		break;
	case CHUNK_FILL:
	case CHUNK_CRC32:
		expected = SIMG_CHUNK_HEADER_BYTES + SIMG_VALUE_BYTES;
		break;
	case CHUNK_DONT_CARE:
		expected = SIMG_CHUNK_HEADER_BYTES;
		break;
	default:
		return (simg_corrupt(error, "chunk type is unknown"));
	}
	if (total != expected)
		return (simg_corrupt(error, "chunk size does not match its type"));
	if ((blocks == 0) != (chunk->type == CHUNK_CRC32))
		return (simg_corrupt(error,
		    "chunk covers no blocks, or a CRC32 chunk covers some"));
	if (!fsimg_range_fits(offset, total, input->file_bytes))
		return (simg_corrupt(error, "chunk lies past the end of the file"));
	if (total == SIMG_CHUNK_HEADER_BYTES + SIMG_VALUE_BYTES) {
		status = read_bytes(input, chunk->payload_offset,
		    raw + SIMG_CHUNK_HEADER_BYTES, SIMG_VALUE_BYTES, error);
		if (status != FSIMG_OK)
			return (status);
		chunk->value = fsimg_layer_le32(raw + SIMG_CHUNK_HEADER_BYTES);
	}
	*next = offset + total;
	return (FSIMG_OK);
}

/* Visit the header's count of chunks in file order, which is disk order.
 * The chunks must fill the file exactly, so the count the header declares
 * is the count the file holds, and their blocks must add up to the
 * header's total.
 */
static fsimg_status_t
walk_chunks(const fsimg_container_input_t *input, const simg_header_t *header,
    simg_visit_t visit, void *argument, fsimg_error_t *error)
{
	simg_chunk_t chunk;
	uint64_t offset, disk_offset;
	fsimg_status_t status;

	offset = SIMG_HEADER_BYTES;
	disk_offset = 0;
	for (uint32_t i = 0; i < header->total_chunks; i++) {
		if (offset == input->file_bytes)
			return (simg_corrupt(error,
			    "file holds fewer chunks than the header's total"));
		status = decode_chunk(input, header, offset, &chunk, &offset, error);
		if (status != FSIMG_OK)
			return (status);
		if (chunk.length > header->disk_bytes - disk_offset)
			return (simg_corrupt(error,
			    "chunks cover more blocks than the header's total"));
		chunk.disk_offset = disk_offset;
		status = visit(argument, &chunk, error);
		if (status != FSIMG_OK)
			return (status);
		disk_offset += chunk.length;
	}
	if (offset != input->file_bytes)
		return (simg_corrupt(error,
		    "file holds more chunks than the header's total"));
	if (disk_offset != header->disk_bytes)
		return (simg_corrupt(error,
		    "chunks cover fewer blocks than the header's total"));
	return (FSIMG_OK);
}

static fsimg_status_t
append_chunk(void *argument, const simg_chunk_t *chunk, fsimg_error_t *error)
{
	fsimg_chunk_t record;

	bzero(&record, sizeof(record));
	record.disk_offset = chunk->disk_offset;
	record.length = chunk->length;
	switch (chunk->type) {
	case CHUNK_RAW:
		record.kind = FSIMG_CHUNK_RAW;
		record.file_offset = chunk->payload_offset;
		record.encoded_length = chunk->length;
		break;
	case CHUNK_FILL:
		record.kind = FSIMG_CHUNK_FILL;
		record.argument = chunk->value;
		break;
	case CHUNK_DONT_CARE:
		record.kind = FSIMG_CHUNK_ZERO;
		break;
	default:
		/* A CRC32 chunk covers no blocks; verification checks it. */
		return (FSIMG_OK);
	}
	return (fsimg_chunk_index_append(argument, &record, error));
}

static fsimg_status_t
simg_open(const fsimg_container_input_t *input, fsimg_container_t *out,
    fsimg_error_t *error)
{
	fsimg_chunk_summary_t summary;
	fsimg_chunk_index_t *index;
	simg_header_t header;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	status = read_header(input, &header, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_chunk_index_new(input->session, "sparse image",
	    header.disk_bytes, 0, input->file_bytes, &index, error);
	if (status != FSIMG_OK)
		return (status);
	status = walk_chunks(input, &header, append_chunk, index, error);
	if (status == FSIMG_OK)
		status = fsimg_chunk_index_seal(index, error);
	if (status != FSIMG_OK) {
		fsimg_chunk_index_free(index);
		return (status);
	}
	fsimg_chunk_index_summary(index, &summary);
	status = fsimg_chunk_disk_open(input->ctx, input->file, input->session,
	    index, &out->disk, error);
	if (status != FSIMG_OK)
		return (status);
	out->disk_bytes = header.disk_bytes;
	out->chunks = summary.count;
	out->kinds = summary.kinds;
	return (FSIMG_OK);
}

/*
 * Under read-through, each chunk's expanded bytes join a running CRC32,
 * DONT_CARE blocks as zeros, and a CRC32 chunk must hold the CRC of
 * everything before it.
 */
static fsimg_status_t
verify_chunk(void *argument, const simg_chunk_t *chunk, fsimg_error_t *error)
{
	simg_verify_t *verify = argument;
	uint64_t offset, remaining;
	size_t piece, transferred;
	fsimg_status_t status;

	if (!verify->payload)
		return (FSIMG_OK);
	if (chunk->type == CHUNK_CRC32) {
		if (verify->crc != chunk->value)
			return (simg_corrupt(error,
			    "CRC32 chunk does not match the expanded image"));
		return (FSIMG_OK);
	}
	offset = chunk->disk_offset;
	remaining = chunk->length;
	while (remaining != 0) {
		piece = remaining < VERIFY_PIECE_BYTES ? (size_t)remaining :
		    VERIFY_PIECE_BYTES;
		status = fsimg_io_read_session(verify->disk, verify->session,
		    offset, verify->piece, piece, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		verify->crc = crc32(verify->crc, verify->piece, (uInt)piece);
		offset += piece;
		remaining -= piece;
	}
	return (FSIMG_OK);
}

/* Walk the header and chunk headers again, and under read-through check
 * every CRC32 chunk against the expanded bytes.
 */
static fsimg_status_t
simg_verify(const fsimg_container_input_t *input,
    const fsimg_container_t *container, bool payload, fsimg_error_t *error)
{
	simg_header_t header;
	simg_verify_t verify;
	fsimg_status_t status;

	status = read_header(input, &header, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&verify, sizeof(verify));
	verify.disk = container->disk;
	verify.session = input->session;
	verify.crc = crc32(0, Z_NULL, 0);
	verify.payload = payload;
	verify.piece = fsimg_alloc(input->ctx, VERIFY_PIECE_BYTES, error);
	if (verify.piece == NULL)
		return (error->status);
	status = walk_chunks(input, &header, verify_chunk, &verify, error);
	fsimg_dealloc(verify.piece);
	return (status);
}
