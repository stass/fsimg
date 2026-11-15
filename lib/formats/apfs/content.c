/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "apfs.h"

/*
 * File content (specs/design-apfs-driver.md, "Content" and "Salvage";
 * specs/design-apfs.md, "Data streams and file extents"): the extents of
 * the current stream, which resolve returns for an uncompressed file and
 * through which compress.c reads a compressed file's stored bytes.
 *
 * A stream is tiled by the file-extent records filed under its
 * private ID, in logical order from offset zero, each starting where the
 * previous one ends; bytes past the last extent and below the stream's size
 * read as zero. stream_extent() answers one offset with the extent holding
 * it. Delivery clips an extent to the file's initialized length, which
 * replay sets to the data stream's size and salvage may lower. A random
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
 * after the last key below the stream's first, is not the stream's. A
 * sealed volume keeps its file extents in the sealed file-extent tree
 * instead (specs/design-apfs.md, "Sealed volumes"), whose keys order by
 * private ID and offset alone; the content cursor is over that tree, and
 * extent_key_encode() and extent_at_cursor() are the two places that tell
 * the trees' records apart.
 *
 * Only stream_extent() moves the content cursor. While
 * apfs->current.resolved is set and current.extent came from a record
 * ending below the stream's size, the cursor rests on that record. An
 * extent that reaches the size is the last one a read uses, so its successor is
 * checked at once, which leaves the cursor past it; no later offset
 * advances from it or from the zero tail, which ends there too. A forward
 * read therefore checks every record it could have relied on. The size is
 * the stream's own, not the reader's initialized length, which salvage
 * lowers to a damaged file's prefix: planning and copying the prefix meet
 * the same records.
 */
#define ROLE_EXTENT	"file extent"

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
static bool		extent_key_encode(const apfs_reader_t *,
			    unsigned char *, const apfs_extent_key_t *);
static fsimg_status_t	extent_judge(const apfs_reader_t *,
			    const apfs_extent_key_t *, const apfs_extent_val_t *,
			    extent_record_t *, fsimg_error_t *);
static fsimg_status_t	resolve_advance(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	resolve_seek(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	result_record(fsimg_reader_t *,
			    const extent_record_t *, fsimg_error_t *);
static fsimg_status_t	stream_extent(fsimg_reader_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	tiling_check(uint64_t, const extent_record_t *,
			    fsimg_error_t *);
static void		result_tail(apfs_reader_t *, uint64_t);
static int		sealed_extent_compare(const unsigned char *, size_t,
			    const unsigned char *, size_t);

/* Both trees' extent keys fit the one search buffer. */
_Static_assert(APFS_SEALED_EXTENT_KEY_BYTES == APFS_EXTENT_KEY_BYTES,
    "sealed and file-extent keys differ in size");

/* The sealed file-extent tree: fixed keys of private ID then offset. */
static const apfs_tree_profile_t sealed_extent_tree = {
	.role = "sealed file-extent tree",
	.compare = sealed_extent_compare,
	.subtype = APFS_SUBTYPE_FEXT,
	.key_min = APFS_SEALED_EXTENT_KEY_BYTES,
	.key_max = APFS_SEALED_EXTENT_KEY_BYTES,
	.val_min = APFS_SEALED_EXTENT_VAL_BYTES,
	.val_max = APFS_SEALED_EXTENT_VAL_BYTES,
	.fixed_key = APFS_SEALED_EXTENT_KEY_BYTES,
	.fixed_val = APFS_SEALED_EXTENT_VAL_BYTES,
};

const apfs_tree_profile_t *
apfs_sealed_extent_tree_profile(void)
{
	return (&sealed_extent_tree);
}

/* Keys of the profile's fixed size, as the cursor guarantees. */
static int
sealed_extent_compare(const unsigned char *a, size_t a_len,
    const unsigned char *b, size_t b_len)
{
	apfs_extent_key_t x = { 0 }, y = { 0 };

	(void)apfs_sealed_extent_key_decode(a, a_len, &x);
	(void)apfs_sealed_extent_key_decode(b, b_len, &y);
	if (x.private_id != y.private_id)
		return (x.private_id < y.private_id ? -1 : 1);
	if (x.logical != y.logical)
		return (x.logical < y.logical ? -1 : 1);
	return (0);
}

/* The content cursor's key for a stream offset, in its tree's encoding. */
static bool
extent_key_encode(const apfs_reader_t *apfs, unsigned char *raw,
    const apfs_extent_key_t *key)
{
	if (apfs->sealed)
		return (apfs_sealed_extent_key_encode(raw,
		    APFS_SEALED_EXTENT_KEY_BYTES, key));
	return (apfs_extent_key_encode(raw, APFS_EXTENT_KEY_BYTES, key));
}

fsimg_status_t
apfs_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	fsimg_status_t status;

	if (apfs->current.compressed)
		return (apfs_compress_resolve(reader, offset, extent, error));
	status = stream_extent(reader, offset, error);
	if (status != FSIMG_OK)
		return (status);
	*extent = apfs->current.extent;
	return (FSIMG_OK);
}

void
apfs_stream_select(apfs_reader_t *apfs, const apfs_stream_t *stream)
{
	apfs->current.stream = *stream;
	apfs->current.resolved = false;
}

fsimg_status_t
apfs_stream_read(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	const fsimg_extent_t *extent = &apfs->current.extent;
	unsigned char *out = buffer;
	uint64_t size = apfs->current.stream.size, position, within;
	size_t batch, transferred;
	fsimg_status_t status;

	if (offset > size || length > size - offset)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "APFS stream read runs past the stream"));
	for (size_t done = 0; done < length; done += batch) {
		position = offset + done;
		status = stream_extent(reader, position, error);
		if (status != FSIMG_OK)
			return (status);
		within = position - extent->logical_offset;
		batch = length - done;
		if (batch > extent->length - within)
			batch = (size_t)(extent->length - within);
		if (extent->image_offset == FSIMG_EXTENT_ZERO) {
			bzero(out + done, batch);
			continue;
		}
		status = fsimg_reader_bytes(reader, extent->image_offset + within,
		    out + done, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Leave the extent of the current stream holding offset, which is below
 * its size, in current.extent: the one already there, its successor, or
 * one a descent finds.
 */
static fsimg_status_t
stream_extent(fsimg_reader_t *reader, uint64_t offset, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_current_t *current = &apfs->current;
	uint64_t cached_end;
	bool resolved = current->resolved;

	cached_end = current->extent.logical_offset + current->extent.length;
	if (resolved && offset >= current->extent.logical_offset &&
	    offset < cached_end)
		return (FSIMG_OK);
	/* A failure below leaves the cursor anywhere. */
	current->resolved = false;
	if (resolved && offset == cached_end)
		return (resolve_advance(reader, cached_end, error));
	return (resolve_seek(reader, offset, error));
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
		result_tail(apfs, end);
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
		.private_id = apfs->current.stream.oid, .logical = offset };
	extent_record_t record;
	uint64_t end = 0;
	bool positioned, found = false;
	fsimg_status_t status;

	if (!extent_key_encode(apfs, key, &search))
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
	result_tail(apfs, end);
	return (FSIMG_OK);
}

/*
 * The stream's records end at end: the bytes from there to its size read
 * as zero. Every caller has end <= offset < size, so the length is
 * positive.
 */
static void
result_tail(apfs_reader_t *apfs, uint64_t end)
{
	apfs_current_t *current = &apfs->current;

	current->extent = (fsimg_extent_t){ .logical_offset = end,
	    .image_offset = FSIMG_EXTENT_ZERO,
	    .length = current->stream.size - end };
	current->resolved = true;
}

/*
 * The record the cursor is on holds the offset. When it reaches the
 * stream's size, the record after it must continue the tiling or not be
 * the stream's, as resolve_advance() would require. A successor of the
 * stream starting below the size contradicts bytes this record delivers
 * and is always rejected. Any other rejection there, a damaged next node,
 * a malformed next record or a successor past the size, judges no
 * delivered byte, so under salvage it leaves the record readable and the
 * walk or verification reports the damage where it meets it
 * (specs/design-apfs-driver.md, "Salvage"). No later offset of the stream
 * advances from the cursor, and a failed node load cuts the cursor's path
 * above the node before reading it, so the next seek reuses only nodes
 * that loaded: where such a failure left the cursor does not matter.
 */
static fsimg_status_t
result_record(fsimg_reader_t *reader, const extent_record_t *record,
    fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_current_t *current = &apfs->current;
	extent_record_t next;
	fsimg_error_t detail;
	bool found, delivered;
	fsimg_status_t status;

	if (record->end >= current->stream.size) {
		fsimg_error_clear(&detail);
		status = extent_after_cursor(apfs, &found, &next, &detail);
		if (status == FSIMG_OK && found)
			status = tiling_check(record->end, &next, &detail);
		delivered = found && next.logical < current->stream.size;
		if (status != FSIMG_OK && (delivered ||
		    !fsimg_reader_decode_rejected(reader, &detail, status))) {
			if (error != NULL)
				*error = detail;
			return (status);
		}
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
	if (apfs->sealed) {
		/* Every record of the sealed tree is a file extent. */
		if (!apfs_sealed_extent_key_decode(raw.key, raw.key_len,
		    &key) || raw.ghost ||
		    !apfs_sealed_extent_val_decode(raw.val, raw.val_len, &val))
			return (apfs_corrupt(error, ROLE_EXTENT,
			    "is malformed"));
		if (key.private_id != apfs->current.stream.oid)
			return (FSIMG_OK);
	} else {
		if (!apfs_jkey_decode(raw.key, raw.key_len, &header))
			return (apfs_corrupt(error, ROLE_EXTENT,
			    "key is malformed"));
		if (header.kind != APFS_J_FILE_EXTENT ||
		    header.oid != apfs->current.stream.oid)
			return (FSIMG_OK);
		if (!apfs_extent_key_decode(raw.key, raw.key_len, &key) ||
		    raw.ghost ||
		    !apfs_extent_val_decode(raw.val, raw.val_len, &val))
			return (apfs_corrupt(error, ROLE_EXTENT,
			    "is malformed"));
	}
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
	/* stream.size is at most INT64_MAX, as replay checks, so rounding
	 * cannot overflow.
	 */
	rounded = (current->stream.size + block - 1) / block * block;
	limit = rounded > current->stream.alloced ? rounded :
	    current->stream.alloced;
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
