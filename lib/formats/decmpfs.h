/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_DECMPFS_H
#define FSIMG_DECMPFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"

/* The framing of macOS transparent compression, which HFS Plus and APFS both
 * store: the header of a file's com.apple.decmpfs value and the chunk tables of
 * its resource fork. The module is pure. It holds no state, allocates nothing
 * and knows no fork: a driver hands it a callback that reads the fork's bytes.
 * The header is little-endian; a type 4 resource fork wraps a little-endian
 * table in a big-endian classic resource file. The module states each
 * compression type's facts: where its bytes are kept, its codec and its raw
 * marker. Which types a format admits, the decoding of a chunk and the work
 * charged for it belong to the driver.
 */

#define FSIMG_DECMPFS_HEADER_BYTES	16u
#define FSIMG_DECMPFS_RSRC_BYTES	16u
#define FSIMG_DECMPFS_RSRC_TABLE_BYTES	8u
#define FSIMG_DECMPFS_PAIR_BYTES	8u
#define FSIMG_DECMPFS_WORD_BYTES	4u
#define FSIMG_DECMPFS_CHUNK_BYTES	65536u	/* a chunk's decoded length */
#define FSIMG_DECMPFS_STORED_MAX	69632u	/* its stored bound */

/* The header of a com.apple.decmpfs value. */
typedef struct fsimg_decmpfs_header {
	uint64_t	 uncompressed_size;
	uint32_t	 type;
} fsimg_decmpfs_header_t;

/* The classic resource file header at the start of a type 4 resource fork. */
typedef struct fsimg_decmpfs_rsrc {
	uint32_t	 data_offset;
	uint32_t	 map_offset;
	uint32_t	 data_length;
	uint32_t	 map_length;
} fsimg_decmpfs_rsrc_t;

/* The eight bytes at a type 4 resource fork's data offset. */
typedef struct fsimg_decmpfs_rsrc_table {
	uint32_t	 resource_length;
	uint32_t	 chunk_count;
} fsimg_decmpfs_rsrc_table_t;

/* One entry of a type 4 chunk table; offset counts from the chunk count. */
typedef struct fsimg_decmpfs_pair {
	uint32_t	 offset;
	uint32_t	 length;
} fsimg_decmpfs_pair_t;

/* Where a type keeps its stored bytes: after the header in the attribute, or
 * in the resource fork behind one of the two forms of chunk table.
 */
typedef enum fsimg_decmpfs_kind {
	FSIMG_DECMPFS_INLINE,		/* the bytes after the header */
	FSIMG_DECMPFS_TABLE_RESOURCE,	/* (offset, length) pairs */
	FSIMG_DECMPFS_TABLE_WORDS	/* successor-offset words */
} fsimg_decmpfs_kind_t;

/* The codec a type's payload or chunks are encoded with. NONE stores them
 * unencoded.
 */
typedef enum fsimg_decmpfs_codec {
	FSIMG_DECMPFS_CODEC_NONE,
	FSIMG_DECMPFS_CODEC_ZLIB,
	FSIMG_DECMPFS_CODEC_LZVN,
	FSIMG_DECMPFS_CODEC_LZFSE,
	FSIMG_DECMPFS_CODEC_LZBITMAP
} fsimg_decmpfs_codec_t;

/* The compression types, the header's type field. */
#define FSIMG_DECMPFS_TYPE_STORED		1u
#define FSIMG_DECMPFS_TYPE_ZLIB_INLINE		3u
#define FSIMG_DECMPFS_TYPE_ZLIB_FORK		4u
#define FSIMG_DECMPFS_TYPE_LZVN_INLINE		7u
#define FSIMG_DECMPFS_TYPE_LZVN_FORK		8u
#define FSIMG_DECMPFS_TYPE_COPY_INLINE		9u
#define FSIMG_DECMPFS_TYPE_COPY_FORK		10u
#define FSIMG_DECMPFS_TYPE_LZFSE_INLINE		11u
#define FSIMG_DECMPFS_TYPE_LZFSE_FORK		12u
#define FSIMG_DECMPFS_TYPE_LZBITMAP_INLINE	13u
#define FSIMG_DECMPFS_TYPE_LZBITMAP_FORK	14u

#define FSIMG_DECMPFS_UNMARKED	(-1)	/* a type with no raw marker */

/* The facts of one compression type. marker is the raw marker byte, or
 * FSIMG_DECMPFS_UNMARKED. Under a codec, a payload or chunk whose first byte
 * is the marker is that byte followed by the content, unencoded; any other is
 * the codec's stream, none of which begins with its marker. Under
 * FSIMG_DECMPFS_CODEC_NONE every payload and chunk of a marked type is the
 * marker and the content, and an unmarked type's payload is the content.
 */
typedef struct fsimg_decmpfs_info {
	fsimg_decmpfs_kind_t	 kind;
	fsimg_decmpfs_codec_t	 codec;
	int			 marker;
} fsimg_decmpfs_info_t;

/* A checked chunk table, in fork offsets: entries start at table_offset, a
 * chunk's offset counts from chunk_base, and every chunk lies in
 * [payload_start, payload_end). chunks is ceil(size / 65536), which the
 * table holds entries for.
 */
typedef struct fsimg_decmpfs_table {
	uint64_t		 table_offset;
	uint64_t		 chunk_base;
	uint64_t		 payload_start;
	uint64_t		 payload_end;
	uint64_t		 chunks;
	fsimg_decmpfs_kind_t	 kind;
} fsimg_decmpfs_table_t;

/* A driver's read of fork bytes, given (context, offset, buffer, length,
 * error): [offset, offset + length) into buffer. The module asks only for
 * ranges inside the fork's length as given to fsimg_decmpfs_open().
 */
typedef fsimg_status_t (*fsimg_decmpfs_read_t)(void *, uint64_t, void *, size_t,
    fsimg_error_t *);

/* Record decoders. Each takes bytes and their length, returns false when the
 * length or, for the header, its magic or its size is wrong, and then leaves
 * its output untouched. The header, resource header and resource table
 * decoders read a fixed prefix of what may be a longer buffer; a value may
 * continue past the header with its inline payload. The header's size must
 * fit a signed 64-bit content length.
 *
 * fsimg_decmpfs_pair_decode() and fsimg_decmpfs_word_decode(), given (table,
 * table_bytes, index, out), decode entry `index` of a chunk table, false when
 * it lies outside table_bytes. table is the first entry: for a pair, the byte
 * after fsimg_decmpfs_rsrc_table_t; for a word, the fork's start.
 */
bool fsimg_decmpfs_header_decode(const unsigned char *, size_t,
    fsimg_decmpfs_header_t *);
bool fsimg_decmpfs_rsrc_decode(const unsigned char *, size_t,
    fsimg_decmpfs_rsrc_t *);
bool fsimg_decmpfs_rsrc_table_decode(const unsigned char *, size_t,
    fsimg_decmpfs_rsrc_table_t *);
bool fsimg_decmpfs_pair_decode(const unsigned char *, size_t, uint32_t,
    fsimg_decmpfs_pair_t *);
bool fsimg_decmpfs_word_decode(const unsigned char *, size_t, uint32_t,
    uint32_t *);

/* fsimg_decmpfs_type_info(type, info) fills info with a known type's facts
 * and returns true, or returns false for a type the module does not know and
 * leaves info untouched. Knowing a type is not admitting it: each driver
 * decides which known types it decodes.
 */
bool fsimg_decmpfs_type_info(uint32_t, fsimg_decmpfs_info_t *);

/* fsimg_decmpfs_unencoded(info, bytes, stored, expected, content, error)
 * applies a type's raw marker to one payload or chunk: stored bytes that
 * decode to expected bytes. It returns FSIMG_OK with *content at the expected
 * bytes of content inside bytes when they are stored unencoded, and FSIMG_OK
 * with *content NULL when bytes are the codec's stream. A marked type with no
 * codec whose bytes lack the marker, and unencoded content of the wrong
 * length, are FSIMG_ECORRUPT, leaving *content untouched.
 */
fsimg_status_t fsimg_decmpfs_unencoded(const fsimg_decmpfs_info_t *,
    const unsigned char *, size_t, size_t, const unsigned char **,
    fsimg_error_t *);

/* fsimg_decmpfs_open(kind, fork_length, size, fork_read, context, table,
 * error) checks a resource fork's chunk table of the given kind against the
 * fork's length and the file's uncompressed size, and describes it in table.
 * It reads the resource header and the table's count, or a word table's first
 * word, and no other entry. kind is a table form; FSIMG_DECMPFS_INLINE is
 * FSIMG_ESTATE.
 *
 * fsimg_decmpfs_chunk(table, index, fork_read, context, offset, stored,
 * error) reads entry `index` with its neighbours, at most three entries,
 * checks the chunk against them, the payload interval and the stored bound,
 * and returns its fork offset and stored length. table is what
 * fsimg_decmpfs_open() returned for the same fork and fork_read. index is
 * below table->chunks; any other index is FSIMG_ESTATE.
 *
 * A rejection is FSIMG_ECORRUPT with a message naming the framing; a failure
 * of fork_read passes through unchanged. Either leaves the outputs untouched.
 */
fsimg_status_t fsimg_decmpfs_open(fsimg_decmpfs_kind_t, uint64_t, uint64_t,
    fsimg_decmpfs_read_t, void *, fsimg_decmpfs_table_t *, fsimg_error_t *);
fsimg_status_t fsimg_decmpfs_chunk(const fsimg_decmpfs_table_t *, uint64_t,
    fsimg_decmpfs_read_t, void *, uint64_t *, size_t *, fsimg_error_t *);

#endif
