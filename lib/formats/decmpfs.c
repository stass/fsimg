/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "decmpfs.h"
#include "fsimg-endian.h"

/*
 * The decmpfs framing: the decoders of its records and the checks that relate
 * a chunk table to its fork and a chunk to its neighbours. Every check is
 * against values in hand: the fork's length and the file's size from the
 * caller, and at most three table entries read through the callback, so the
 * table is never held whole. All arithmetic is in 64 bits on 32-bit fields
 * and on counts and indices below 2^48, so nothing wraps.
 */

static fsimg_status_t corrupt(fsimg_error_t *, const char *);
static uint64_t chunk_count(uint64_t);
static fsimg_status_t rsrc_open(uint64_t, uint64_t, fsimg_decmpfs_read_t,
    void *, fsimg_decmpfs_table_t *, fsimg_error_t *);
static fsimg_status_t words_open(uint64_t, uint64_t, fsimg_decmpfs_read_t,
    void *, fsimg_decmpfs_table_t *, fsimg_error_t *);
static fsimg_status_t pair_locate(const fsimg_decmpfs_table_t *, uint64_t,
    fsimg_decmpfs_read_t, void *, uint64_t *, size_t *, fsimg_error_t *);
static fsimg_status_t word_locate(const fsimg_decmpfs_table_t *, uint64_t,
    fsimg_decmpfs_read_t, void *, uint64_t *, size_t *, fsimg_error_t *);

static fsimg_status_t
corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

/* The bytes 66 70 6d 63, then a little-endian type and uncompressed size. */
bool
fsimg_decmpfs_header_decode(const unsigned char *raw, size_t length,
    fsimg_decmpfs_header_t *header)
{
	static const unsigned char magic[4] = { 0x66, 0x70, 0x6d, 0x63 };
	uint64_t size;

	if (length < FSIMG_DECMPFS_HEADER_BYTES ||
	    bcmp(raw, magic, sizeof(magic)) != 0)
		return (false);

	size = fsimg_load_le64(raw + 8);
	if (size > (uint64_t)INT64_MAX)
		return (false);

	header->type = fsimg_load_le32(raw + 4);
	header->uncompressed_size = size;
	return (true);
}

bool
fsimg_decmpfs_rsrc_decode(const unsigned char *raw, size_t length,
    fsimg_decmpfs_rsrc_t *rsrc)
{
	if (length < FSIMG_DECMPFS_RSRC_BYTES)
		return (false);

	rsrc->data_offset = fsimg_load_be32(raw);
	rsrc->map_offset = fsimg_load_be32(raw + 4);
	rsrc->data_length = fsimg_load_be32(raw + 8);
	rsrc->map_length = fsimg_load_be32(raw + 12);
	return (true);
}

/* A big-endian resource length, then the little-endian chunk count. */
bool
fsimg_decmpfs_rsrc_table_decode(const unsigned char *raw, size_t length,
    fsimg_decmpfs_rsrc_table_t *table)
{
	if (length < FSIMG_DECMPFS_RSRC_TABLE_BYTES)
		return (false);

	table->resource_length = fsimg_load_be32(raw);
	table->chunk_count = fsimg_load_le32(raw + 4);
	return (true);
}

bool
fsimg_decmpfs_pair_decode(const unsigned char *pairs, size_t length,
    uint32_t index, fsimg_decmpfs_pair_t *pair)
{
	uint64_t offset;

	offset = (uint64_t)index * FSIMG_DECMPFS_PAIR_BYTES;
	if (!fsimg_range_fits(offset, FSIMG_DECMPFS_PAIR_BYTES, length))
		return (false);

	pair->offset = fsimg_load_le32(pairs + offset);
	pair->length = fsimg_load_le32(pairs + offset + 4);
	return (true);
}

bool
fsimg_decmpfs_word_decode(const unsigned char *words, size_t length,
    uint32_t index, uint32_t *word)
{
	uint64_t offset;

	offset = (uint64_t)index * FSIMG_DECMPFS_WORD_BYTES;
	if (!fsimg_range_fits(offset, FSIMG_DECMPFS_WORD_BYTES, length))
		return (false);

	*word = fsimg_load_le32(words + offset);
	return (true);
}

/* The types macOS writes, the numbers from xnu's decmpfs.h and the framing and
 * markers from native captures: 0xFF for zlib, whose stream never begins with
 * it, and for LZFSE and LZBITMAP, whose streams begin with a magic; 0x06 for
 * LZVN, the code that ends its stream; 0xCC before every payload and chunk of
 * the uncompressed types 9 and 10. Type 1 is the documented unencoded payload,
 * which no native tool was seen to write.
 */
static const struct {
	uint32_t		 type;
	fsimg_decmpfs_info_t	 info;
} types[] = {
	{ FSIMG_DECMPFS_TYPE_STORED,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_NONE,
	    FSIMG_DECMPFS_UNMARKED } },
	{ FSIMG_DECMPFS_TYPE_ZLIB_INLINE,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_ZLIB, 0xff } },
	{ FSIMG_DECMPFS_TYPE_ZLIB_FORK,
	  { FSIMG_DECMPFS_TABLE_RESOURCE, FSIMG_DECMPFS_CODEC_ZLIB, 0xff } },
	{ FSIMG_DECMPFS_TYPE_LZVN_INLINE,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_LZVN, 0x06 } },
	{ FSIMG_DECMPFS_TYPE_LZVN_FORK,
	  { FSIMG_DECMPFS_TABLE_WORDS, FSIMG_DECMPFS_CODEC_LZVN, 0x06 } },
	{ FSIMG_DECMPFS_TYPE_COPY_INLINE,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_NONE, 0xcc } },
	{ FSIMG_DECMPFS_TYPE_COPY_FORK,
	  { FSIMG_DECMPFS_TABLE_WORDS, FSIMG_DECMPFS_CODEC_NONE, 0xcc } },
	{ FSIMG_DECMPFS_TYPE_LZFSE_INLINE,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_LZFSE, 0xff } },
	{ FSIMG_DECMPFS_TYPE_LZFSE_FORK,
	  { FSIMG_DECMPFS_TABLE_WORDS, FSIMG_DECMPFS_CODEC_LZFSE, 0xff } },
	{ FSIMG_DECMPFS_TYPE_LZBITMAP_INLINE,
	  { FSIMG_DECMPFS_INLINE, FSIMG_DECMPFS_CODEC_LZBITMAP, 0xff } },
	{ FSIMG_DECMPFS_TYPE_LZBITMAP_FORK,
	  { FSIMG_DECMPFS_TABLE_WORDS, FSIMG_DECMPFS_CODEC_LZBITMAP, 0xff } },
};

bool
fsimg_decmpfs_type_info(uint32_t type, fsimg_decmpfs_info_t *info)
{
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (types[i].type == type) {
			*info = types[i].info;
			return (true);
		}
	}
	return (false);
}

fsimg_status_t
fsimg_decmpfs_unencoded(const fsimg_decmpfs_info_t *info,
    const unsigned char *bytes, size_t stored, size_t expected,
    const unsigned char **content, fsimg_error_t *error)
{
	size_t skip;

	if (info->marker != FSIMG_DECMPFS_UNMARKED && stored != 0 &&
	    bytes[0] == info->marker)
		skip = 1;
	else if (info->codec != FSIMG_DECMPFS_CODEC_NONE) {
		*content = NULL;
		return (FSIMG_OK);
	} else if (info->marker != FSIMG_DECMPFS_UNMARKED)
		return (corrupt(error,
		    "decmpfs uncompressed payload or chunk lacks its marker"));
	else
		skip = 0;
	if (stored - skip != expected)
		return (corrupt(error,
		    "decmpfs uncompressed payload or chunk has the wrong length"));
	*content = bytes + skip;
	return (FSIMG_OK);
}

/* The chunks a file of size bytes is cut into: ceil(size / 65536). */
static uint64_t
chunk_count(uint64_t size)
{
	return (size / FSIMG_DECMPFS_CHUNK_BYTES +
	    (size % FSIMG_DECMPFS_CHUNK_BYTES != 0));
}

fsimg_status_t
fsimg_decmpfs_open(fsimg_decmpfs_kind_t kind, uint64_t fork_length,
    uint64_t size, fsimg_decmpfs_read_t fork_read, void *context,
    fsimg_decmpfs_table_t *table, fsimg_error_t *error)
{
	switch (kind) {
	case FSIMG_DECMPFS_TABLE_RESOURCE:
		return (rsrc_open(fork_length, size, fork_read, context, table,
		    error));
	case FSIMG_DECMPFS_TABLE_WORDS:
		return (words_open(fork_length, size, fork_read, context, table,
		    error));
	case FSIMG_DECMPFS_INLINE:
		break;
	}
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
	    "decmpfs kind is not a chunk table"));
}

/* Check the classic resource header's relations to the fork: the map follows
 * the data and lies in the fork, so no chunk can be read from inside it (the
 * sizes then put it exactly at the data's end), and the table's place: the
 * table interval [D + 4, D + 8 + 8 * count), with D the data offset, must end
 * inside the declared data, D plus the data length, and the rest of the data
 * is the payload interval. Nothing here assumes the native data offset or map
 * length. The declared size must agree with the count, so a size is only as
 * large as a table the fork holds.
 */
static fsimg_status_t
rsrc_open(uint64_t fork_length, uint64_t size, fsimg_decmpfs_read_t fork_read,
    void *context, fsimg_decmpfs_table_t *table, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_DECMPFS_RSRC_BYTES];
	fsimg_decmpfs_rsrc_table_t header;
	fsimg_decmpfs_rsrc_t rsrc;
	uint64_t end, table_end;
	fsimg_status_t status;

	if (fork_length < sizeof(raw))
		return (corrupt(error, "decmpfs resource fork is too short"));
	status = fork_read(context, 0, raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	(void)fsimg_decmpfs_rsrc_decode(raw, sizeof(raw), &rsrc);
	end = (uint64_t)rsrc.data_offset + rsrc.data_length;
	if (rsrc.data_length < FSIMG_DECMPFS_RSRC_TABLE_BYTES ||
	    end + rsrc.map_length != fork_length || rsrc.map_offset < end ||
	    (uint64_t)rsrc.map_offset + rsrc.map_length > fork_length)
		return (corrupt(error,
		    "decmpfs resource header does not match the fork"));
	status = fork_read(context, rsrc.data_offset, raw,
	    FSIMG_DECMPFS_RSRC_TABLE_BYTES, error);
	if (status != FSIMG_OK)
		return (status);
	(void)fsimg_decmpfs_rsrc_table_decode(raw,
	    FSIMG_DECMPFS_RSRC_TABLE_BYTES, &header);
	table_end = (uint64_t)rsrc.data_offset + FSIMG_DECMPFS_RSRC_TABLE_BYTES +
	    (uint64_t)header.chunk_count * FSIMG_DECMPFS_PAIR_BYTES;
	if (header.resource_length != rsrc.data_length - 4 ||
	    header.chunk_count != chunk_count(size) || table_end > end)
		return (corrupt(error, "decmpfs chunk table is malformed"));
	table->table_offset = (uint64_t)rsrc.data_offset +
	    FSIMG_DECMPFS_RSRC_TABLE_BYTES;
	table->chunk_base = (uint64_t)rsrc.data_offset + 4;
	table->payload_start = table_end;
	table->payload_end = end;
	table->chunks = header.chunk_count;
	table->kind = FSIMG_DECMPFS_TABLE_RESOURCE;
	return (FSIMG_OK);
}

/* Check a word table, which begins the fork: count + 1
 * little-endian offset words from the fork's start. The first word is the
 * table's end, which must hold the words and lie in the fork. Its length is
 * the writer's choice, so it is never computed from the count, and words
 * between the last offset and the table's end are not read. All the rest of
 * the fork is the payload interval. count is at most 2^48, so the product
 * cannot wrap.
 */
static fsimg_status_t
words_open(uint64_t fork_length, uint64_t size, fsimg_decmpfs_read_t fork_read,
    void *context, fsimg_decmpfs_table_t *table, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_DECMPFS_WORD_BYTES];
	uint64_t count;
	uint32_t table_end;
	fsimg_status_t status;

	if (fork_length < sizeof(raw))
		return (corrupt(error, "decmpfs resource fork is too short"));
	status = fork_read(context, 0, raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	(void)fsimg_decmpfs_word_decode(raw, sizeof(raw), 0, &table_end);
	count = chunk_count(size);
	if (table_end < (count + 1) * FSIMG_DECMPFS_WORD_BYTES ||
	    table_end > fork_length)
		return (corrupt(error, "decmpfs chunk table is malformed"));
	table->table_offset = 0;
	table->chunk_base = 0;
	table->payload_start = table_end;
	table->payload_end = fork_length;
	table->chunks = count;
	table->kind = FSIMG_DECMPFS_TABLE_WORDS;
	return (FSIMG_OK);
}

/* The index bound is the caller's contract; it is checked here as well, so an
 * out-of-range index never makes the module read past the table.
 */
fsimg_status_t
fsimg_decmpfs_chunk(const fsimg_decmpfs_table_t *table, uint64_t index,
    fsimg_decmpfs_read_t fork_read, void *context, uint64_t *offset,
    size_t *stored, fsimg_error_t *error)
{
	if (index >= table->chunks)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "decmpfs chunk index is past the table"));
	switch (table->kind) {
	case FSIMG_DECMPFS_TABLE_RESOURCE:
		return (pair_locate(table, index, fork_read, context, offset,
		    stored, error));
	case FSIMG_DECMPFS_TABLE_WORDS:
		return (word_locate(table, index, fork_read, context, offset,
		    stored, error));
	case FSIMG_DECMPFS_INLINE:
		break;
	}
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
	    "decmpfs table has no table kind"));
}

/* Read chunk `index`'s pair through a small window of it and its neighbours,
 * and check it against them, the payload interval and the stored bound: the
 * chunk's offset counts from chunk_base. raw has room for three pairs and only
 * last - first + 1 of them are read, so it may be partly filled. Every index
 * decoded here stays at or below last - first, which is at most 2, so each
 * decode lies in the filled part and cannot fail.
 */
static fsimg_status_t
pair_locate(const fsimg_decmpfs_table_t *table, uint64_t index,
    fsimg_decmpfs_read_t fork_read, void *context, uint64_t *offset,
    size_t *stored, fsimg_error_t *error)
{
	unsigned char raw[3 * FSIMG_DECMPFS_PAIR_BYTES];
	fsimg_decmpfs_pair_t entry, neighbour;
	uint64_t first, last, start;
	fsimg_status_t status;

	first = index == 0 ? 0 : index - 1;
	last = index + 1 == table->chunks ? index : index + 1;
	status = fork_read(context,
	    table->table_offset + first * FSIMG_DECMPFS_PAIR_BYTES, raw,
	    (size_t)(last - first + 1) * FSIMG_DECMPFS_PAIR_BYTES, error);
	if (status != FSIMG_OK)
		return (status);
	(void)fsimg_decmpfs_pair_decode(raw, sizeof(raw),
	    (uint32_t)(index - first), &entry);
	start = table->chunk_base + entry.offset;
	if (entry.length > FSIMG_DECMPFS_STORED_MAX ||
	    start < table->payload_start ||
	    start + entry.length > table->payload_end)
		return (corrupt(error,
		    "decmpfs chunk lies outside the resource data"));
	if (first != index) {
		(void)fsimg_decmpfs_pair_decode(raw, sizeof(raw), 0, &neighbour);
		if ((uint64_t)neighbour.offset + neighbour.length > entry.offset)
			return (corrupt(error, "decmpfs chunks overlap"));
	}
	if (last != index) {
		(void)fsimg_decmpfs_pair_decode(raw, sizeof(raw),
		    (uint32_t)(last - first), &neighbour);
		if ((uint64_t)entry.offset + entry.length > neighbour.offset)
			return (corrupt(error, "decmpfs chunks overlap"));
	}
	*offset = start;
	*stored = entry.length;
	return (FSIMG_OK);
}

/* Chunk `index`'s span in a word table: its offset word to its successor, so
 * a chunk's neighbours are the two words that define it. The table holds both
 * words, since its end was checked against the count and the fork. raw is
 * exactly those two words, so neither decode can fail.
 */
static fsimg_status_t
word_locate(const fsimg_decmpfs_table_t *table, uint64_t index,
    fsimg_decmpfs_read_t fork_read, void *context, uint64_t *offset,
    size_t *stored, fsimg_error_t *error)
{
	unsigned char raw[2 * FSIMG_DECMPFS_WORD_BYTES];
	uint64_t end, start;
	uint32_t end_word, start_word;
	fsimg_status_t status;

	status = fork_read(context,
	    table->table_offset + index * FSIMG_DECMPFS_WORD_BYTES, raw,
	    sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	(void)fsimg_decmpfs_word_decode(raw, sizeof(raw), 0, &start_word);
	(void)fsimg_decmpfs_word_decode(raw, sizeof(raw), 1, &end_word);
	if (end_word < start_word)
		return (corrupt(error, "decmpfs chunks overlap"));
	start = table->chunk_base + start_word;
	end = table->chunk_base + end_word;
	if (start < table->payload_start || end > table->payload_end ||
	    end - start > FSIMG_DECMPFS_STORED_MAX)
		return (corrupt(error,
		    "decmpfs chunk lies outside the resource data"));
	*offset = start;
	*stored = (size_t)(end - start);
	return (FSIMG_OK);
}
