/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "generated.h"
#include "sha256.h"

#define SEEDED_BLOCK_BYTES	32u
#define POSITIONAL_BLOCK_BYTES	16u

static bool holes_valid(const fsimg_generated_t *);
static void make_block(const fsimg_generated_t *, uint64_t, unsigned char *);
static void zero_holes(const fsimg_generated_t *, uint64_t, unsigned char *,
    size_t);

size_t
fsimg_generated_encode(const fsimg_generated_t *generated,
    unsigned char *record)
{
	unsigned char *cursor;

	fsimg_store_le64(record, generated->generator);
	fsimg_store_le64(record + 8, generated->seed);
	fsimg_store_le64(record + 16, generated->length);
	fsimg_store_le64(record + 24, generated->hole_count);
	cursor = record + FSIMG_GENERATED_HEADER;
	for (size_t i = 0; i < generated->hole_count; i++) {
		fsimg_store_le64(cursor, generated->holes[i].offset);
		fsimg_store_le64(cursor + 8, generated->holes[i].length);
		cursor += 16;
	}
	if (generated->generator == FSIMG_GENERATOR_LINK) {
		bcopy(generated->target, cursor, (size_t)generated->length);
		cursor += generated->length;
	}
	return ((size_t)(cursor - record));
}

/* The record is read back from scratch storage, so every field is checked
 * against what the encoder can write: a LINK has no seed or holes and a
 * target of 1 through FSIMG_SYMLINK_TARGET_MAX bytes without a NUL; ZERO has neither seed nor
 * holes; seeded and positional content has at most FSIMG_MTREE_HOLES_MAX
 * valid holes; the length covers exactly the fields.
 */
bool
fsimg_generated_decode(const unsigned char *record, size_t length,
    fsimg_generated_t *out)
{
	const unsigned char *cursor;
	uint64_t generator, hole_count;

	bzero(out, sizeof(*out));
	if (length < FSIMG_GENERATED_HEADER)
		return (false);
	generator = fsimg_load_le64(record);
	out->seed = fsimg_load_le64(record + 8);
	out->length = fsimg_load_le64(record + 16);
	hole_count = fsimg_load_le64(record + 24);
	cursor = record + FSIMG_GENERATED_HEADER;
	switch (generator) {
	case FSIMG_GENERATOR_LINK:
		if (out->seed != 0 || hole_count != 0 || out->length == 0 ||
		    out->length > FSIMG_SYMLINK_TARGET_MAX ||
		    length - FSIMG_GENERATED_HEADER != out->length ||
		    memchr(cursor, '\0', (size_t)out->length) != NULL)
			return (false);
		out->target = cursor;
		break;
	case FSIMG_GENERATOR_ZERO:
		if (out->seed != 0 || hole_count != 0 ||
		    length != FSIMG_GENERATED_HEADER)
			return (false);
		break;
	case FSIMG_GENERATOR_SEEDED:
	case FSIMG_GENERATOR_POSITIONAL:
		if (hole_count > FSIMG_MTREE_HOLES_MAX ||
		    length - FSIMG_GENERATED_HEADER != 16 * hole_count)
			return (false);
		out->hole_count = (size_t)hole_count;
		for (size_t i = 0; i < out->hole_count; i++) {
			out->holes[i].offset = fsimg_load_le64(cursor);
			out->holes[i].length = fsimg_load_le64(cursor + 8);
			cursor += 16;
		}
		if (!holes_valid(out))
			return (false);
		break;
	default:
		return (false);
	}
	out->generator = (fsimg_generator_t)generator;
	return (true);
}

static bool
holes_valid(const fsimg_generated_t *generated)
{
	const fsimg_mtree_hole_t *hole;
	uint64_t end;

	end = 0;
	for (size_t i = 0; i < generated->hole_count; i++) {
		hole = &generated->holes[i];
		if (hole->length == 0 || (i > 0 && hole->offset <= end) ||
		    !fsimg_add_u64(hole->offset, hole->length, &end) ||
		    end > generated->length)
			return (false);
	}
	return (true);
}

/* Block index of the content's block stream: SEEDED hashes the seed and
 * the index, POSITIONAL writes the seed and the block's byte offset.
 */
static void
make_block(const fsimg_generated_t *generated, uint64_t index,
    unsigned char *block)
{
	fsimg_sha256_t hash;
	unsigned char message[16];

	fsimg_store_le64(message, generated->seed);
	if (generated->generator == FSIMG_GENERATOR_POSITIONAL) {
		bcopy(message, block, 8);
		fsimg_store_le64(block + 8, index * POSITIONAL_BLOCK_BYTES);
		return;
	}
	fsimg_store_le64(message + 8, index);
	fsimg_sha256_init(&hash);
	fsimg_sha256_update(&hash, message, sizeof(message));
	fsimg_sha256_final(block, &hash);
}

/* Zero the part of buffer, which holds bytes [offset, offset + length),
 * that the holes cover. Holes are ordered, so the scan stops at the first
 * one starting past the buffer.
 */
static void
zero_holes(const fsimg_generated_t *generated, uint64_t offset,
    unsigned char *buffer, size_t length)
{
	const fsimg_mtree_hole_t *hole;
	uint64_t end, from, hole_end, to;

	end = offset + length;
	for (size_t i = 0; i < generated->hole_count; i++) {
		hole = &generated->holes[i];
		hole_end = hole->offset + hole->length;
		if (hole_end <= offset)
			continue;
		if (hole->offset >= end)
			break;
		from = hole->offset > offset ? hole->offset : offset;
		to = hole_end < end ? hole_end : end;
		bzero(buffer + (from - offset), (size_t)(to - from));
	}
}

/* The block stream is cut at the first byte wanted and the bytes taken
 * block by block; offset + length is within the content's length, so no
 * position overflows.
 */
void
fsimg_generated_read(const fsimg_generated_t *generated, uint64_t offset,
    unsigned char *buffer, size_t length)
{
	unsigned char block[SEEDED_BLOCK_BYTES];
	uint64_t position;
	size_t block_bytes, count, done, skip;

	if (generated->generator == FSIMG_GENERATOR_ZERO) {
		bzero(buffer, length);
		return;
	}
	block_bytes = generated->generator == FSIMG_GENERATOR_SEEDED ?
	    SEEDED_BLOCK_BYTES : POSITIONAL_BLOCK_BYTES;
	for (done = 0; done < length; done += count) {
		position = offset + done;
		skip = (size_t)(position % block_bytes);
		make_block(generated, position / block_bytes, block);
		count = block_bytes - skip;
		if (count > length - done)
			count = length - done;
		bcopy(block + skip, buffer + done, count);
	}
	zero_holes(generated, offset, buffer, length);
}

void
fsimg_generated_range(const fsimg_generated_t *generated, uint64_t offset,
    fsimg_range_t *range)
{
	const fsimg_mtree_hole_t *hole;
	uint64_t data_end, hole_end;

	range->offset = offset;
	if (generated->generator == FSIMG_GENERATOR_ZERO) {
		range->kind = FSIMG_RANGE_ZERO;
		range->length = generated->length - offset;
		return;
	}
	data_end = generated->length;
	for (size_t i = 0; i < generated->hole_count; i++) {
		hole = &generated->holes[i];
		hole_end = hole->offset + hole->length;
		if (offset >= hole->offset && offset < hole_end) {
			range->kind = FSIMG_RANGE_ZERO;
			range->length = hole_end - offset;
			return;
		}
		if (hole->offset > offset) {
			data_end = hole->offset;
			break;
		}
	}
	range->kind = FSIMG_RANGE_DATA;
	range->length = data_end - offset;
}

fsimg_status_t
fsimg_generated_digest(fsimg_session_t *session,
    const fsimg_generated_t *generated, unsigned char *buffer, size_t capacity,
    unsigned char digest[static FSIMG_SHA256_DIGEST_LENGTH],
    fsimg_error_t *error)
{
	fsimg_sha256_t hash;
	uint64_t offset;
	size_t piece;
	fsimg_status_t status;

	fsimg_sha256_init(&hash);
	for (offset = 0; offset < generated->length; offset += piece) {
		status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		piece = capacity;
		if (piece > generated->length - offset)
			piece = (size_t)(generated->length - offset);
		fsimg_generated_read(generated, offset, buffer, piece);
		fsimg_sha256_update(&hash, buffer, piece);
	}
	fsimg_sha256_final(digest, &hash);
	return (FSIMG_OK);
}
