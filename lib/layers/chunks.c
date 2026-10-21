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
#include "meta.h"

/*
 * A container's disk as a chunk index, and the read-only adapter that
 * serves the disk from it.
 *
 * The index holds one 40-byte record per chunk: a disk range and how to
 * obtain its bytes (raw bytes at a file offset, zeros, a repeated fill
 * value, or compressed bytes). A container's open appends the records in
 * whatever order its tables list them; sealing sorts them by disk offset
 * with the external sort and copies them, in that order, into a store of
 * fixed-size records while proving that they tile the disk: the first
 * starts at zero, each starts where the previous one ends, and the last
 * ends at the disk length. A lookup is then a binary search over the
 * store, or the cached last record and its successor for sequential
 * access. Records live in scratch, which is not trusted, so every record
 * read back is decoded and checked against the disk and the file range
 * the container's data occupies; that check is the only thing between a
 * record and a read of the file.
 *
 * The adapter resolves each read through the index. Raw records read the
 * file at the translated offset, zero and fill records synthesize bytes,
 * and compressed records are inflated whole into a least-recently-used
 * cache of four chunk buffers, each the size of the largest compressed
 * chunk the index holds. Sequential reads and any access confined to four
 * chunks therefore decode each chunk once; a pattern alternating across
 * more chunks pays one decode per read until the work limit, or without
 * one the decode bound of 64 times the compressed chunks' decoded length
 * plus 1 GiB, stops it. The bound counts only bytes a compressed record
 * declares, so a disk declared vast with zero runs does not raise it.
 * The cache changes no byte a read returns.
 */
#define CHUNK_RECORD_BYTES	40u
#define CHUNK_CACHE_SLOTS	4u
#define CHUNK_INPUT_BYTES	(64u * 1024)
#define DECODE_BOUND_FACTOR	64u
#define DECODE_BOUND_EXTRA	(UINT64_C(1) << 30)

_Static_assert(FSIMG_CHUNK_MAX_DECODED <= UINT32_MAX,
    "a chunk's decoded length fits zlib's output counter");

/* Record fields, little-endian. */
enum {
	RECORD_DISK_OFFSET = 0,
	RECORD_LENGTH = 8,
	RECORD_FILE_OFFSET = 16,
	RECORD_ENCODED = 24,
	RECORD_KIND = 32,
	RECORD_ARGUMENT = 36
};

struct fsimg_chunk_index {
	fsimg_session_t	*session;
	const char	*owner;		/* Names the container in messages */
	fsimg_stream_t	*appended;	/* Records as appended, until sealed */
	fsimg_store_t	*records;	/* Sealed records in disk order */
	uint64_t	 disk_bytes;
	uint64_t	 data_offset;
	uint64_t	 data_bytes;
	uint64_t	 count;
	uint64_t	 max_decoded;
	uint64_t	 compressed_decoded;	/* Compressed records' lengths, summed */
	unsigned	 kinds;
	bool		 sealed;
	/* The last record a lookup returned, already decoded and checked. */
	fsimg_chunk_t	 last;
	uint64_t	 last_record;
	bool		 last_valid;
};

/* One decompressed chunk; the slot is empty while length is zero. */
typedef struct cache_slot {
	unsigned char	*bytes;
	fsimg_chunk_t	 chunk;
	uint64_t	 last_use;
} cache_slot_t;

/*
 * The adapter's state. The file and the session are borrowed from the
 * image, which outlives the adapter; the index, the cache buffers and the
 * inflate state are owned and released with the adapter.
 */
typedef struct chunk_disk {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*file;
	fsimg_session_t	*session;
	fsimg_chunk_index_t *index;
	unsigned char	*cache;		/* CHUNK_CACHE_SLOTS buffers */
	unsigned char	*input;		/* Compressed bytes in pieces */
	cache_slot_t	 slots[CHUNK_CACHE_SLOTS];
	uint64_t	 slot_bytes;	/* Each slot's size, fixed at open */
	z_stream	 stream;
	fsimg_error_t	 zlib_error;	/* Why zlib's last allocation failed */
	uint64_t	 decoded_bytes;
	uint64_t	 decode_bound;
	uint64_t	 use_clock;
	bool		 stream_ready;
} chunk_disk_t;

static fsimg_status_t chunk_corrupt(const fsimg_chunk_index_t *,
    fsimg_error_t *, const char *);
static bool chunk_valid(const fsimg_chunk_index_t *, const fsimg_chunk_t *);
static void record_encode(const fsimg_chunk_t *, unsigned char *);
static void record_decode(const unsigned char *, fsimg_chunk_t *);
static int record_compare(const void *, size_t, const void *, size_t,
    void *);
static fsimg_status_t record_read(fsimg_chunk_index_t *, uint64_t,
    fsimg_chunk_t *, fsimg_error_t *);
static fsimg_status_t record_search(fsimg_chunk_index_t *, uint64_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t tile_records(fsimg_chunk_index_t *, fsimg_stream_t *,
    fsimg_error_t *);
static voidpf zlib_alloc(voidpf, uInt, uInt);
static void zlib_free(voidpf, voidpf);
static void disk_release(void *);
static fsimg_status_t charge_decode(chunk_disk_t *, const fsimg_chunk_t *,
    fsimg_error_t *);
static fsimg_status_t inflate_chunk(chunk_disk_t *, const fsimg_chunk_t *,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t cache_fetch(chunk_disk_t *, const fsimg_chunk_t *,
    const unsigned char **, fsimg_error_t *);
static void fill_pattern(unsigned char *, size_t, uint32_t, uint64_t);
static fsimg_status_t disk_resolve(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);

static fsimg_status_t
chunk_corrupt(const fsimg_chunk_index_t *index, fsimg_error_t *error,
    const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "%s chunk %s", index->owner, check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * A record is valid when it names a nonempty range of the disk and its
 * encoded bytes, if any, lie in the container's data range: raw bytes as
 * many as the range, compressed bytes at least one and at most zlib's
 * bound for the decoded length, which is at most the 16 MiB a cache slot
 * holds. Zero and fill records name no file bytes.
 */
static bool
chunk_valid(const fsimg_chunk_index_t *index, const fsimg_chunk_t *chunk)
{
	if (chunk->length == 0 || !fsimg_range_fits(chunk->disk_offset,
	    chunk->length, index->disk_bytes))
		return (false);
	switch (chunk->kind) {
	case FSIMG_CHUNK_ZERO:
		if (chunk->argument != 0)
			return (false);
		/* FALLTHROUGH */
	case FSIMG_CHUNK_FILL:
		return (chunk->file_offset == 0 && chunk->encoded_length == 0);
	case FSIMG_CHUNK_RAW:
		if (chunk->argument != 0 || chunk->encoded_length != chunk->length)
			return (false);
		break;
	case FSIMG_CHUNK_COMPRESSED:
		if (chunk->argument != FSIMG_CODEC_ZLIB ||
		    chunk->length > FSIMG_CHUNK_MAX_DECODED ||
		    chunk->encoded_length == 0 ||
		    chunk->encoded_length > compressBound((uLong)chunk->length))
			return (false);
		break;
	default:
		return (false);
	}
	return (chunk->file_offset >= index->data_offset &&
	    fsimg_range_fits(chunk->file_offset - index->data_offset,
	    chunk->encoded_length, index->data_bytes));
}

static void
record_encode(const fsimg_chunk_t *chunk, unsigned char *raw)
{
	fsimg_layer_put_le64(raw + RECORD_DISK_OFFSET, chunk->disk_offset);
	fsimg_layer_put_le64(raw + RECORD_LENGTH, chunk->length);
	fsimg_layer_put_le64(raw + RECORD_FILE_OFFSET, chunk->file_offset);
	fsimg_layer_put_le64(raw + RECORD_ENCODED, chunk->encoded_length);
	fsimg_layer_put_le32(raw + RECORD_KIND, (uint32_t)chunk->kind);
	fsimg_layer_put_le32(raw + RECORD_ARGUMENT, chunk->argument);
}

static void
record_decode(const unsigned char *raw, fsimg_chunk_t *chunk)
{
	chunk->disk_offset = fsimg_layer_le64(raw + RECORD_DISK_OFFSET);
	chunk->length = fsimg_layer_le64(raw + RECORD_LENGTH);
	chunk->file_offset = fsimg_layer_le64(raw + RECORD_FILE_OFFSET);
	chunk->encoded_length = fsimg_layer_le64(raw + RECORD_ENCODED);
	chunk->kind = (fsimg_chunk_kind_t)fsimg_layer_le32(raw + RECORD_KIND);
	chunk->argument = fsimg_layer_le32(raw + RECORD_ARGUMENT);
}

/* Records order by disk offset; equal offsets are overlap the tiling
 * pass rejects, so their relative order does not matter.
 */
static int
record_compare(const void *left, size_t left_length, const void *right,
    size_t right_length, void *argument FSIMG_UNUSED)
{
	uint64_t a, b;

	if (left_length != CHUNK_RECORD_BYTES || right_length != CHUNK_RECORD_BYTES)
		return (left_length < right_length ? -1 :
		    left_length > right_length);
	a = fsimg_layer_le64((const unsigned char *)left + RECORD_DISK_OFFSET);
	b = fsimg_layer_le64((const unsigned char *)right + RECORD_DISK_OFFSET);
	return (a < b ? -1 : a > b);
}

fsimg_status_t
fsimg_chunk_index_new(fsimg_session_t *session, const char *owner,
    uint64_t disk_bytes, uint64_t data_offset, uint64_t data_bytes,
    fsimg_chunk_index_t **out, fsimg_error_t *error)
{
	fsimg_chunk_index_t *index;
	fsimg_status_t status;

	*out = NULL;
	index = fsimg_alloc(session->ctx, sizeof(*index), error);
	if (index == NULL)
		return (error->status);
	bzero(index, sizeof(*index));
	index->session = session;
	index->owner = owner;
	index->disk_bytes = disk_bytes;
	index->data_offset = data_offset;
	index->data_bytes = data_bytes;
	status = fsimg_stream_new(session, CHUNK_RECORD_BYTES, &index->appended,
	    error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(index);
		return (status);
	}
	*out = index;
	return (FSIMG_OK);
}

void
fsimg_chunk_index_free(fsimg_chunk_index_t *index)
{
	if (index == NULL)
		return;
	fsimg_stream_free(index->appended);
	fsimg_store_free(index->records);
	fsimg_dealloc(index);
}

/* Append one record; a record that is not valid for the container is
 * corrupt. Records may arrive in any disk order.
 */
fsimg_status_t
fsimg_chunk_index_append(fsimg_chunk_index_t *index,
    const fsimg_chunk_t *chunk, fsimg_error_t *error)
{
	unsigned char raw[CHUNK_RECORD_BYTES];
	fsimg_status_t status;

	if (index->sealed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "chunk index is sealed"));
	if (!chunk_valid(index, chunk))
		return (chunk_corrupt(index, error,
		    "lies outside the disk or the data, or its lengths disagree"));
	record_encode(chunk, raw);
	status = fsimg_stream_append(index->appended, raw, sizeof(raw), NULL,
	    error);
	if (status != FSIMG_OK)
		return (status);
	index->count++;
	index->kinds |= 1u << chunk->kind;
	if (chunk->kind == FSIMG_CHUNK_COMPRESSED) {
		if (chunk->length > index->max_decoded)
			index->max_decoded = chunk->length;
		/* Overlapping records fail at seal; until then saturate. */
		if (!fsimg_add_u64(index->compressed_decoded, chunk->length,
		    &index->compressed_decoded))
			index->compressed_decoded = UINT64_MAX;
	}
	return (FSIMG_OK);
}

/*
 * Copy the sorted records into the fixed-record store, proving as they go
 * that they tile the disk: each starts where the previous one ended, from
 * zero to the disk length, so there is neither a gap nor an overlap.
 */
static fsimg_status_t
tile_records(fsimg_chunk_index_t *index, fsimg_stream_t *sorted,
    fsimg_error_t *error)
{
	unsigned char raw[CHUNK_RECORD_BYTES];
	fsimg_chunk_t chunk;
	uint64_t cursor, expected, record, capacity;
	size_t length;
	fsimg_status_t status;

	status = fsimg_store_new(index->session, &index->records, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	expected = 0;
	capacity = 0;
	for (record = 0;; record++) {
		status = fsimg_stream_next(sorted, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		record_decode(raw, &chunk);
		if (length != sizeof(raw) || !chunk_valid(index, &chunk))
			return (chunk_corrupt(index, error,
			    "record changed in scratch"));
		if (chunk.disk_offset < expected)
			return (chunk_corrupt(index, error, "ranges overlap"));
		if (chunk.disk_offset > expected)
			return (chunk_corrupt(index, error,
			    "ranges leave part of the disk uncovered"));
		expected = chunk.disk_offset + chunk.length;
		if (capacity < (record + 1) * CHUNK_RECORD_BYTES) {
			/* Grow a page's count of records at a time. */
			capacity += FSIMG_META_PAGE_SIZE * CHUNK_RECORD_BYTES;
			status = fsimg_store_grow(index->records, capacity, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = fsimg_store_write(index->records,
		    record * CHUNK_RECORD_BYTES, raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (record != index->count)
		return (chunk_corrupt(index, error, "record count changed in scratch"));
	if (expected != index->disk_bytes)
		return (chunk_corrupt(index, error,
		    "ranges leave part of the disk uncovered"));
	return (FSIMG_OK);
}

/* Sort the appended records by disk offset and keep them only if they tile
 * the disk exactly. The appended stream is released either way.
 */
fsimg_status_t
fsimg_chunk_index_seal(fsimg_chunk_index_t *index, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	fsimg_status_t status;

	if (index->sealed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "chunk index is sealed"));
	status = fsimg_stream_seal(index->appended, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_sort(index->appended, record_compare, NULL, NULL,
	    &sorted, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_stream_free(index->appended);
	index->appended = NULL;
	status = tile_records(index, sorted, error);
	fsimg_stream_free(sorted);
	if (status != FSIMG_OK)
		return (status);
	index->sealed = true;
	return (FSIMG_OK);
}

void
fsimg_chunk_index_summary(const fsimg_chunk_index_t *index,
    fsimg_chunk_summary_t *out)
{
	out->count = index->count;
	out->max_decoded = index->max_decoded;
	out->kinds = index->kinds;
}

/* Every record read back from scratch is decoded and checked here. */
static fsimg_status_t
record_read(fsimg_chunk_index_t *index, uint64_t record, fsimg_chunk_t *out,
    fsimg_error_t *error)
{
	unsigned char raw[CHUNK_RECORD_BYTES];
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	if (record >= index->count)
		return (chunk_corrupt(index, error, "record is outside its index"));
	status = fsimg_store_read(index->records, record * CHUNK_RECORD_BYTES,
	    raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	record_decode(raw, out);
	if (!chunk_valid(index, out))
		return (chunk_corrupt(index, error, "record changed in scratch"));
	return (FSIMG_OK);
}

/* The last record that starts at or before offset, by binary search. */
static fsimg_status_t
record_search(fsimg_chunk_index_t *index, uint64_t offset, uint64_t *record,
    fsimg_error_t *error)
{
	fsimg_chunk_t probe;
	uint64_t low, high, middle;
	fsimg_status_t status;

	*record = 0;
	low = 0;
	high = index->count;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = record_read(index, middle, &probe, error);
		if (status != FSIMG_OK)
			return (status);
		if (probe.disk_offset <= offset)
			low = middle + 1;
		else
			high = middle;
	}
	if (low == 0)
		return (chunk_corrupt(index, error, "index has no covering record"));
	*record = low - 1;
	return (FSIMG_OK);
}

/* The record containing a disk offset. Sequential access crosses each
 * record once: the last record found answers repeated requests and its
 * successor the offset where it ends; any other offset uses binary search.
 */
fsimg_status_t
fsimg_chunk_index_lookup(fsimg_chunk_index_t *index, uint64_t offset,
    fsimg_chunk_t *out, fsimg_error_t *error)
{
	fsimg_chunk_t found;
	uint64_t record;
	fsimg_status_t status;

	if (!index->sealed || offset >= index->disk_bytes)
		return (chunk_corrupt(index, error, "lookup outside the disk"));
	if (index->last_valid && offset >= index->last.disk_offset &&
	    offset - index->last.disk_offset < index->last.length) {
		*out = index->last;
		return (FSIMG_OK);
	}
	/* A record lies within the disk, so its end cannot overflow. */
	if (index->last_valid &&
	    offset == index->last.disk_offset + index->last.length &&
	    index->last_record + 1 < index->count)
		record = index->last_record + 1;
	else {
		status = record_search(index, offset, &record, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = record_read(index, record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (offset < found.disk_offset || offset - found.disk_offset >= found.length)
		return (chunk_corrupt(index, error, "index has no covering record"));
	index->last = found;
	index->last_record = record;
	index->last_valid = true;
	*out = found;
	return (FSIMG_OK);
}

fsimg_store_t *
fsimg_test_chunk_records(const fsimg_chunk_index_t *index)
{
	return (index == NULL ? NULL : index->records);
}

void
fsimg_chunk_kinds_text(unsigned kinds, char *text, size_t capacity)
{
	static const struct {
		fsimg_chunk_kind_t kind;
		const char *name;
	} names[] = {
		{ FSIMG_CHUNK_RAW, "raw" }, { FSIMG_CHUNK_ZERO, "zero" },
		{ FSIMG_CHUNK_FILL, "fill" }, { FSIMG_CHUNK_COMPRESSED, "zlib" }
	};
	size_t used;
	int count;

	used = 0;
	text[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((kinds & (1u << names[i].kind)) == 0)
			continue;
		count = snprintf(text + used, capacity - used, "%s%s",
		    used == 0 ? "" : ",", names[i].name);
		if (count < 0 || (size_t)count >= capacity - used)
			return;
		used += (size_t)count;
	}
}

/* zlib's state is charged to the context like every other allocation. */
static voidpf
zlib_alloc(voidpf opaque, uInt items, uInt size)
{
	chunk_disk_t *disk = opaque;

	if (size != 0 && items > SIZE_MAX / size)
		return (Z_NULL);
	return (fsimg_alloc(disk->ctx, (size_t)items * size, &disk->zlib_error));
}

static void
zlib_free(voidpf opaque FSIMG_UNUSED, voidpf address)
{
	fsimg_dealloc(address);
}

static void
disk_release(void *argument)
{
	chunk_disk_t *disk = argument;

	if (disk->stream_ready)
		inflateEnd(&disk->stream);
	fsimg_chunk_index_free(disk->index);
	fsimg_dealloc(disk->cache);
	fsimg_dealloc(disk->input);
	fsimg_dealloc(disk);
}

/*
 * Open the adapter over a sealed index. The cache is allocated here, four
 * buffers of the largest compressed chunk, so a budget that cannot hold it
 * fails the open as a memory limit naming the chunk cache rather than a
 * later read. The adapter adopts the index on every outcome.
 */
fsimg_status_t
fsimg_chunk_disk_open(fsimg_ctx_t *ctx, fsimg_io_t *file,
    fsimg_session_t *session, fsimg_chunk_index_t *index, fsimg_io_t **out,
    fsimg_error_t *error)
{
	chunk_disk_t *disk;
	uint64_t bound;
	fsimg_status_t status;

	*out = NULL;
	disk = fsimg_alloc(ctx, sizeof(*disk), error);
	if (disk == NULL) {
		fsimg_chunk_index_free(index);
		return (error->status);
	}
	bzero(disk, sizeof(*disk));
	disk->ctx = ctx;
	disk->file = file;
	disk->session = session;
	disk->index = index;
	if (!fsimg_mul_u64(index->compressed_decoded, DECODE_BOUND_FACTOR,
	    &bound) ||
	    !fsimg_add_u64(bound, DECODE_BOUND_EXTRA, &bound))
		bound = UINT64_MAX;
	disk->decode_bound = bound;
	if (index->max_decoded != 0) {
		/* At most 16 MiB per slot, so four fit a size_t. */
		disk->cache = fsimg_alloc(ctx,
		    (size_t)index->max_decoded * CHUNK_CACHE_SLOTS, error);
		if (disk->cache == NULL) {
			if (error->status == FSIMG_ELIMIT)
				snprintf(error->message, sizeof(error->message),
				    "the chunk cache exceeds the memory limit");
			status = error->status;
			goto fail;
		}
		disk->slot_bytes = index->max_decoded;
		for (unsigned i = 0; i < CHUNK_CACHE_SLOTS; i++)
			disk->slots[i].bytes = disk->cache +
			    (size_t)index->max_decoded * i;
		disk->input = fsimg_alloc(ctx, CHUNK_INPUT_BYTES, error);
		if (disk->input == NULL) {
			status = error->status;
			goto fail;
		}
		disk->stream.zalloc = zlib_alloc;
		disk->stream.zfree = zlib_free;
		disk->stream.opaque = disk;
		if (inflateInit(&disk->stream) != Z_OK) {
			*error = disk->zlib_error;
			if (error->status == FSIMG_OK)
				fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IMAGE,
				    "zlib initialization failed");
			status = error->status;
			goto fail;
		}
		disk->stream_ready = true;
	}
	return (fsimg_io_resolver(ctx, file, index->disk_bytes, disk_resolve,
	    disk_release, disk, out, error));

fail:
	disk_release(disk);
	return (status);
}

/*
 * Charge a decode before doing it: the compressed bytes read and the bytes
 * produced both count as work, and without a work limit the adapter still
 * stops at its decode bound.
 */
static fsimg_status_t
charge_decode(chunk_disk_t *disk, const fsimg_chunk_t *chunk,
    fsimg_error_t *error)
{
	uint64_t total;
	fsimg_status_t status;

	if (!fsimg_add_u64(disk->decoded_bytes, chunk->length, &total))
		total = UINT64_MAX;
	if (total > disk->decode_bound) {
		fsimg_limit_error(error, FSIMG_RESOURCE_WORK, disk->decode_bound,
		    total);
		snprintf(error->message, sizeof(error->message),
		    "the chunk cache reached its decode bound");
		return (FSIMG_ELIMIT);
	}
	status = fsimg_session_charge(disk->session, 0, 0,
	    chunk->encoded_length + chunk->length, error);
	if (status != FSIMG_OK)
		return (status);
	disk->decoded_bytes = total;
	return (FSIMG_OK);
}

/*
 * Inflate one zlib chunk into out, which holds exactly its declared
 * length. The compressed bytes are read from the file in pieces and must
 * end exactly where the stream ends; the stream must produce exactly the
 * declared length. Excess output is detected by giving inflate one spare
 * byte once out is full: a stream that writes it is longer than declared.
 */
static fsimg_status_t
inflate_chunk(chunk_disk_t *disk, const fsimg_chunk_t *chunk,
    unsigned char *out, fsimg_error_t *error)
{
	z_stream *stream = &disk->stream;
	unsigned char spare;
	uint64_t consumed;
	size_t piece, transferred;
	fsimg_status_t status;
	int result;

	if (inflateReset(stream) != Z_OK)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IMAGE,
		    "zlib reset failed"));
	stream->next_out = out;
	stream->avail_out = (uInt)chunk->length;
	stream->next_in = disk->input;
	stream->avail_in = 0;
	consumed = 0;
	for (;;) {
		if (stream->avail_in == 0 && consumed < chunk->encoded_length) {
			piece = chunk->encoded_length - consumed < CHUNK_INPUT_BYTES ?
			    (size_t)(chunk->encoded_length - consumed) :
			    CHUNK_INPUT_BYTES;
			status = fsimg_io_read_session(disk->file, disk->session,
			    chunk->file_offset + consumed, disk->input, piece,
			    &transferred, error);
			if (status != FSIMG_OK)
				return (status);
			stream->next_in = disk->input;
			stream->avail_in = (uInt)piece;
			consumed += piece;
		}
		if (stream->avail_out == 0 && stream->next_out != &spare) {
			stream->next_out = &spare;
			stream->avail_out = 1;
		}
		fsimg_error_clear(&disk->zlib_error);
		result = inflate(stream, Z_NO_FLUSH);
		if (stream->next_out == &spare + 1)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_IMAGE,
			    "compressed chunk decodes to more bytes than declared"));
		if (result == Z_STREAM_END)
			break;
		if (result == Z_MEM_ERROR) {
			*error = disk->zlib_error;
			if (error->status == FSIMG_OK)
				fsimg_error_set(error, FSIMG_ELIMIT, 0,
				    FSIMG_OP_IMAGE, "zlib could not allocate its state");
			return (error->status);
		}
		if (result == Z_BUF_ERROR)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_IMAGE,
			    "compressed chunk ends before its stream does"));
		if (result != Z_OK)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_IMAGE, "compressed chunk is not a valid zlib stream"));
	}
	/* The stream may end in the very call that fills out, before the
	 * spare byte is offered, so the count of bytes produced decides. */
	if (stream->total_out != chunk->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
		    "compressed chunk decodes to fewer bytes than declared"));
	if (stream->avail_in != 0 || consumed != chunk->encoded_length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
		    "compressed chunk continues past the end of its stream"));
	return (FSIMG_OK);
}

/* The decoded bytes of a compressed chunk, from the cache or decoded into
 * its least recently used slot. A slot matches only the identical record,
 * so a record cannot be served another record's bytes. The record comes
 * from scratch, so its length is checked against the slot here, where the
 * slot is written, whatever the index held when the cache was sized.
 */
static fsimg_status_t
cache_fetch(chunk_disk_t *disk, const fsimg_chunk_t *chunk,
    const unsigned char **bytes, fsimg_error_t *error)
{
	cache_slot_t *slot, *victim;
	fsimg_status_t status;

	if (chunk->length > disk->slot_bytes)
		return (chunk_corrupt(disk->index, error,
		    "record changed in scratch"));
	victim = &disk->slots[0];
	for (unsigned i = 0; i < CHUNK_CACHE_SLOTS; i++) {
		slot = &disk->slots[i];
		if (slot->chunk.length != 0 &&
		    slot->chunk.disk_offset == chunk->disk_offset &&
		    slot->chunk.length == chunk->length &&
		    slot->chunk.file_offset == chunk->file_offset &&
		    slot->chunk.encoded_length == chunk->encoded_length) {
			slot->last_use = ++disk->use_clock;
			*bytes = slot->bytes;
			return (FSIMG_OK);
		}
		if (slot->last_use < victim->last_use)
			victim = slot;
	}

	/* The victim holds nothing valid until its decode succeeds. */
	victim->chunk.length = 0;
	status = charge_decode(disk, chunk, error);
	if (status != FSIMG_OK)
		return (status);
	status = inflate_chunk(disk, chunk, victim->bytes, error);
	if (status != FSIMG_OK)
		return (status);
	victim->chunk = *chunk;
	victim->last_use = ++disk->use_clock;
	*bytes = victim->bytes;
	return (FSIMG_OK);
}

/* A fill value repeats every four bytes, little-endian, from the start of
 * its chunk; delta is the first byte's offset within the chunk.
 */
static void
fill_pattern(unsigned char *out, size_t length, uint32_t value, uint64_t delta)
{
	unsigned char pattern[4];

	fsimg_layer_put_le32(pattern, value);
	for (size_t i = 0; i < length; i++)
		out[i] = pattern[(delta + i) % 4];
}

/* Serve a disk range chunk by chunk. */
static fsimg_status_t
disk_resolve(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	chunk_disk_t *disk = argument;
	const unsigned char *decoded;
	unsigned char *out;
	fsimg_chunk_t chunk;
	uint64_t position, delta;
	size_t piece, count;
	fsimg_status_t status;

	*transferred = 0;
	while (*transferred < length) {
		position = offset + *transferred;
		status = fsimg_chunk_index_lookup(disk->index, position, &chunk,
		    error);
		if (status != FSIMG_OK)
			return (status);
		delta = position - chunk.disk_offset;
		piece = length - *transferred;
		if (chunk.length - delta < piece)
			piece = (size_t)(chunk.length - delta);
		out = (unsigned char *)buffer + *transferred;
		switch (chunk.kind) {
		case FSIMG_CHUNK_RAW:
			status = fsimg_io_read_session(disk->file, disk->session,
			    chunk.file_offset + delta, out, piece, &count, error);
			break;
		case FSIMG_CHUNK_ZERO:
			bzero(out, piece);
			break;
		case FSIMG_CHUNK_FILL:
			fill_pattern(out, piece, chunk.argument, delta);
			break;
		case FSIMG_CHUNK_COMPRESSED:
			status = cache_fetch(disk, &chunk, &decoded, error);
			if (status == FSIMG_OK)
				bcopy(decoded + delta, out, piece);
			break;
		default:
			/* lookup returns only valid records */
			return (chunk_corrupt(disk->index, error, "has an unknown kind"));
		}
		if (status != FSIMG_OK)
			return (status);
		*transferred += piece;
	}
	return (FSIMG_OK);
}
