/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "apfs.h"

/*
 * File content (specs/design-apfs-driver.md, "Content" and "Salvage";
 * specs/design-apfs.md, "Data streams and file extents").
 *
 * A file's stream is tiled by the file-extent records filed under its
 * private ID, in logical order from offset zero, each starting where the
 * previous one ends; bytes past the last extent and below the size read as
 * zero. resolve answers one offset with the extent holding it. A random
 * offset costs one descent of the content cursor to the last record at or
 * below it; a read continuing where the previous extent ended advances the
 * cursor with next and checks that the record found starts exactly there,
 * so a forward read checks the whole tiling once. Every record is judged
 * before its extent is returned: its flags and crypto ID against the
 * closed tables, its length and offset against the block size and the
 * stream's bounds, and a mapped run against [1, N). A clone is read through
 * the records of its own private ID, whatever another inode sharing its
 * blocks holds.
 *
 * The stream's records are adjacent in the tree, which orders keys by
 * owner, kind and offset, so the record after the stream's last one, or
 * after the last key below the stream's first, is not the stream's.
 *
 * Only resolve moves the content cursor. While apfs->current.resolved is
 * set and current.extent came from a record ending below the initialized
 * length, the cursor rests on that record. An extent that reaches the
 * initialized length is the last one a read delivers from, so its
 * successor is checked at once, which leaves the cursor past it; no later
 * offset advances from it or from the zero tail, which ends there too.
 * A forward read therefore checks every record it could have relied on.
 */
#define ROLE_EXTENT	"file extent"
#define ROLE_COMPRESSED	"compressed file"

/* The closed table of extent crypto IDs for an unencrypted volume
 * (provisional, E16): zero alone.
 */
#define CRYPTO_ID_NONE	0u

typedef struct extent_record {
	uint64_t	 logical;
	uint64_t	 end;
	uint64_t	 image_offset;	/* FSIMG_EXTENT_ZERO for a hole */
} extent_record_t;

static fsimg_status_t	extent_after_cursor(apfs_reader_t *, bool *,
			    extent_record_t *, fsimg_error_t *);
static fsimg_status_t	extent_at_cursor(apfs_reader_t *, bool *,
			    extent_record_t *, fsimg_error_t *);
static fsimg_status_t	extent_judge(const apfs_reader_t *,
			    const apfs_extent_key_t *, const apfs_extent_val_t *,
			    extent_record_t *, fsimg_error_t *);
static fsimg_status_t	resolve_advance(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	resolve_seek(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	result_record(fsimg_reader_t *,
			    const extent_record_t *, fsimg_error_t *);
static fsimg_status_t	tiling_check(uint64_t, const extent_record_t *,
			    fsimg_error_t *);
static void		result_tail(fsimg_reader_t *, uint64_t);

fsimg_status_t
apfs_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_current_t *current = &apfs->current;
	uint64_t cached_end;
	fsimg_status_t status;

	/* Compressed content is outside what this reader decodes. */
	if (current->compressed)
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "content is not decoded"));

	cached_end = current->extent.logical_offset + current->extent.length;
	if (current->resolved && offset >= current->extent.logical_offset &&
	    offset < cached_end) {
		*extent = current->extent;
		return (FSIMG_OK);
	}
	if (current->resolved && offset == cached_end) {
		current->resolved = false;
		status = resolve_advance(reader, cached_end, error);
	} else {
		/* A failure below leaves the cursor anywhere. */
		current->resolved = false;
		status = resolve_seek(reader, offset, error);
	}
	if (status != FSIMG_OK)
		return (status);
	*extent = current->extent;
	return (FSIMG_OK);
}

/*
 * The offset end continues where the extent the cursor rests on ends: the
 * next record of the stream must start exactly there, or the stream has
 * ended and the rest of its size reads as zero.
 */
static fsimg_status_t
resolve_advance(fsimg_reader_t *reader, uint64_t end, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	extent_record_t record;
	bool found;
	fsimg_status_t status;

	status = extent_after_cursor(apfs, &found, &record, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found) {
		result_tail(reader, end);
		return (FSIMG_OK);
	}
	status = tiling_check(end, &record, error);
	if (status != FSIMG_OK)
		return (status);
	return (result_record(reader, &record, error));
}

/*
 * A random offset. The last record of the stream at or below the offset
 * holds it, or ends at or before it; with no such record the stream must
 * have none at all, since its first starts at zero. Either way, when no
 * record holds the offset, the stream ends at end and no record of it may
 * follow; the rest of the size reads as zero.
 */
static fsimg_status_t
resolve_seek(fsimg_reader_t *reader, uint64_t offset, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	unsigned char key[APFS_EXTENT_KEY_BYTES];
	const apfs_extent_key_t search = {
		.private_id = apfs->current.private_id, .logical = offset };
	extent_record_t record;
	uint64_t end = 0;
	bool positioned, found = false;
	fsimg_status_t status;

	if (!apfs_extent_key_encode(key, sizeof(key), &search))
		return (apfs_corrupt(error, ROLE_EXTENT,
		    "private ID is invalid"));

	/* The record at or below the offset. */
	status = apfs_cursor_seek(&apfs->content, key, sizeof(key),
	    APFS_SEEK_LE, &positioned, error);
	if (status == FSIMG_OK && positioned)
		status = extent_at_cursor(apfs, &found, &record, error);
	if (status != FSIMG_OK)
		return (status);
	if (found) {
		if (offset < record.end)
			return (result_record(reader, &record, error));
		end = record.end;
	}

	/* The record after it, or the tree's first when none is below. */
	if (positioned)
		status = extent_after_cursor(apfs, &found, &record, error);
	else {
		found = false;
		status = apfs_cursor_seek(&apfs->content, key, sizeof(key),
		    APFS_SEEK_GE, &positioned, error);
		if (status == FSIMG_OK && positioned)
			status = extent_at_cursor(apfs, &found, &record,
			    error);
	}
	if (status != FSIMG_OK)
		return (status);
	if (found)
		return (apfs_corrupt(error, ROLE_EXTENT, end == 0 ?
		    "does not start the stream" :
		    "leaves a gap after the extent before it"));
	result_tail(reader, end);
	return (FSIMG_OK);
}

/*
 * The stream's records end at end: the bytes from there to the initialized
 * length read as zero. Every caller has end <= offset < reader->initialized
 * by the resolve contract, so the length is positive.
 */
static void
result_tail(fsimg_reader_t *reader, uint64_t end)
{
	apfs_current_t *current = &((apfs_reader_t *)reader->driver)->current;

	current->extent = (fsimg_extent_t){ .logical_offset = end,
	    .image_offset = FSIMG_EXTENT_ZERO,
	    .length = reader->initialized - end };
	current->resolved = true;
}

/*
 * The record the cursor is on holds the offset. When it reaches the
 * initialized length, the record after it must continue the tiling or not
 * be the stream's, as resolve_advance() would require.
 */
static fsimg_status_t
result_record(fsimg_reader_t *reader, const extent_record_t *record,
    fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_current_t *current = &apfs->current;
	extent_record_t next;
	bool found;
	fsimg_status_t status;

	if (record->end >= reader->initialized) {
		status = extent_after_cursor(apfs, &found, &next, error);
		if (status == FSIMG_OK && found)
			status = tiling_check(record->end, &next, error);
		if (status != FSIMG_OK)
			return (status);
	}
	current->extent = (fsimg_extent_t){ .logical_offset = record->logical,
	    .image_offset = record->image_offset,
	    .length = record->end - record->logical };
	current->resolved = true;
	return (FSIMG_OK);
}

/* A record of the stream following one that ends at end starts there. */
static fsimg_status_t
tiling_check(uint64_t end, const extent_record_t *next, fsimg_error_t *error)
{
	if (next->logical == end)
		return (FSIMG_OK);
	return (apfs_corrupt(error, ROLE_EXTENT, next->logical < end ?
	    "overlaps the extent before it" :
	    "leaves a gap after the extent before it"));
}

/*
 * Move the content cursor to the next record and judge it as
 * extent_at_cursor() does; a cursor that moves past the tree's end finds
 * none.
 */
static fsimg_status_t
extent_after_cursor(apfs_reader_t *apfs, bool *found, extent_record_t *record,
    fsimg_error_t *error)
{
	bool positioned;
	fsimg_status_t status;

	*found = false;
	status = apfs_cursor_next(&apfs->content, &positioned, error);
	if (status != FSIMG_OK || !positioned)
		return (status);
	return (extent_at_cursor(apfs, found, record, error));
}

/*
 * The record the content cursor is on: found reports whether it is a file
 * extent of the current stream, and record its judged extent when it is.
 */
static fsimg_status_t
extent_at_cursor(apfs_reader_t *apfs, bool *found, extent_record_t *record,
    fsimg_error_t *error)
{
	apfs_record_t raw;
	apfs_jkey_t header;
	apfs_extent_key_t key;
	apfs_extent_val_t val;
	fsimg_status_t status;

	*found = false;
	apfs_cursor_record(&apfs->content, &raw);
	if (!apfs_jkey_decode(raw.key, raw.key_len, &header))
		return (apfs_corrupt(error, ROLE_EXTENT, "key is malformed"));
	if (header.kind != APFS_J_FILE_EXTENT ||
	    header.oid != apfs->current.private_id)
		return (FSIMG_OK);
	if (!apfs_extent_key_decode(raw.key, raw.key_len, &key) ||
	    raw.ghost || !apfs_extent_val_decode(raw.val, raw.val_len, &val))
		return (apfs_corrupt(error, ROLE_EXTENT, "is malformed"));
	status = extent_judge(apfs, &key, &val, record, error);
	if (status != FSIMG_OK)
		return (status);
	*found = true;
	return (FSIMG_OK);
}

/*
 * One record's rules: flags and crypto ID from the closed tables, a
 * nonzero length and an offset in whole blocks, an end within the larger
 * of the size rounded up to whole blocks and the allocated bytes, and for
 * a mapped extent blocks inside [1, N). The offset is a key's u64, so its
 * end is checked against overflow; N * B was checked when the geometry was
 * accepted, so a block below N has an image offset.
 */
static fsimg_status_t
extent_judge(const apfs_reader_t *apfs, const apfs_extent_key_t *key,
    const apfs_extent_val_t *val, extent_record_t *record,
    fsimg_error_t *error)
{
	const apfs_container_t *container = &apfs->container;
	const apfs_current_t *current = &apfs->current;
	uint64_t block = container->block_size;
	uint64_t rounded, limit, blocks;

	if (val->flags != 0)
		return (apfs_unsupported(error, ROLE_EXTENT,
		    "has unsupported flags"));
	if (val->crypto_id != CRYPTO_ID_NONE)
		return (apfs_unsupported(error, ROLE_EXTENT,
		    "is encrypted"));
	if (val->length == 0 || val->length % block != 0 ||
	    key->logical % block != 0)
		return (apfs_corrupt(error, ROLE_EXTENT,
		    "is not in whole blocks"));
	/* stream_size is at most INT64_MAX, so rounding cannot overflow. */
	rounded = (current->stream_size + block - 1) / block * block;
	limit = rounded > current->stream_alloced ? rounded :
	    current->stream_alloced;
	if (key->logical > limit || val->length > limit - key->logical)
		return (apfs_corrupt(error, ROLE_EXTENT,
		    "runs past its data stream"));
	record->logical = key->logical;
	record->end = key->logical + val->length;
	if (val->phys_block == 0) {
		record->image_offset = FSIMG_EXTENT_ZERO;
		return (FSIMG_OK);
	}
	blocks = val->length / block;
	if (val->phys_block >= container->block_count ||
	    blocks > container->block_count - val->phys_block)
		return (apfs_corrupt(error, ROLE_EXTENT,
		    "lies outside the container"));
	record->image_offset = val->phys_block * block;
	return (FSIMG_OK);
}
