/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_GENERATED_H
#define FSIMG_GENERATED_H

#include "internal.h"
#include "mtree.h"

/* Generated content: bytes that are a pure function of a manifest entry,
 * so no storage backs them and every host computes the same bytes.
 *
 * SEEDED is the stream of 32-byte blocks SHA-256(LE64(seed) || LE64(i))
 * for i = 0, 1, ..., cut to the length. POSITIONAL is the stream of
 * 16-byte blocks LE64(seed) || LE64(16 * j), each naming the file's
 * identity and its own offset. ZERO is all zeros; a zero file is stored as
 * the zero source and this generator only computes its digest. LINK is a
 * symbolic link's target, whose bytes the record holds. Holes are runs of
 * a SEEDED or POSITIONAL file that read as zero.
 */
typedef enum fsimg_generator {
	FSIMG_GENERATOR_ZERO,
	FSIMG_GENERATOR_SEEDED,
	FSIMG_GENERATOR_POSITIONAL,
	FSIMG_GENERATOR_LINK
} fsimg_generator_t;

/* One generated file or link. target, for LINK only, borrows length
 * bytes; holes, for SEEDED and POSITIONAL only, are positive, increasing,
 * neither touching nor overlapping, and end within length.
 */
typedef struct fsimg_generated {
	fsimg_mtree_hole_t holes[FSIMG_MTREE_HOLES_MAX];
	const unsigned char *target;
	uint64_t	 seed;
	uint64_t	 length;
	size_t		 hole_count;
	fsimg_generator_t generator;
} fsimg_generated_t;

/* A generated source record holds four little-endian u64 fields, the
 * generator, seed, length and hole count, then each hole's offset and
 * length as two u64, then a LINK's target bytes without a terminator.
 */
#define FSIMG_GENERATED_HEADER	32u
#define FSIMG_GENERATED_RECORD_MAX					\
	(FSIMG_GENERATED_HEADER + 16u * FSIMG_MTREE_HOLES_MAX)
_Static_assert(FSIMG_GENERATED_HEADER + FSIMG_SYMLINK_TARGET_MAX <=
    FSIMG_GENERATED_RECORD_MAX, "a link record fits the largest record");

/* Encode a valid SEEDED, POSITIONAL or LINK description into record, which
 * holds FSIMG_GENERATED_RECORD_MAX bytes, and return the record's length.
 */
size_t fsimg_generated_encode(const fsimg_generated_t *, unsigned char *);
/* Decode and validate a record as its writer encoded it; a LINK's target
 * borrows the record. False for any field or framing the encoder cannot
 * produce.
 */
bool fsimg_generated_decode(const unsigned char *, size_t,
    fsimg_generated_t *);
/* Fill buffer with bytes [offset, offset + length) of SEEDED, POSITIONAL or
 * ZERO content; the range lies within the content's length.
 */
void fsimg_generated_read(const fsimg_generated_t *, uint64_t,
    unsigned char *, size_t);
/* The DATA or ZERO run of SEEDED, POSITIONAL or ZERO content that starts
 * at offset, below the content's length, and ends where its kind changes.
 */
void fsimg_generated_range(const fsimg_generated_t *, uint64_t,
    fsimg_range_t *);
/* The SHA-256 of SEEDED, POSITIONAL or ZERO content, generated through
 * buffer one piece at a time; each piece is one unit of work charged to
 * session, which also polls cancellation.
 */
fsimg_status_t fsimg_generated_digest(fsimg_session_t *,
    const fsimg_generated_t *, unsigned char *, size_t,
    unsigned char [static FSIMG_SHA256_DIGEST_LENGTH], fsimg_error_t *);

#endif
