/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "decmpfs.h"
#include "test.h"

/* The decmpfs framing: the record decoders, then the relation checks of a
 * chunk table and its chunks over an in-memory resource fork.
 */

enum {
	COMPRESSION_TYPE = 4,
	COMPRESSION_SIZE = 8,
	RESOURCE_MAP = 4,
	RESOURCE_DATA_LENGTH = 8,
	RESOURCE_MAP_LENGTH = 12
};

static void
put_be(unsigned char *p, uint64_t value, size_t width)
{
	size_t i;

	for (i = 0; i < width; i++)
		p[width - i - 1] = (unsigned char)(value >> (8 * i));
}

static void
put_le(unsigned char *p, uint64_t value, size_t width)
{
	size_t i;

	for (i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static void
case_check(bool condition, const char *name, size_t length)
{
	if (!condition)
		fprintf(stderr, "decmpfs case=%s length=%zu\n", name, length);
	CHECK(condition);
}

/* Adapters preserve the decoder's typed output; no function-pointer casts. */
typedef bool (*decode_t)(const unsigned char *, size_t, void *);

static bool
decode_header(const unsigned char *p, size_t n, void *out)
{
	return (fsimg_decmpfs_header_decode(p, n, out));
}

static bool
decode_rsrc(const unsigned char *p, size_t n, void *out)
{
	return (fsimg_decmpfs_rsrc_decode(p, n, out));
}

static bool
decode_rsrc_table(const unsigned char *p, size_t n, void *out)
{
	return (fsimg_decmpfs_rsrc_table_decode(p, n, out));
}

static bool
decode_pair(const unsigned char *p, size_t n, void *out)
{
	return (fsimg_decmpfs_pair_decode(p, n, 0, out));
}

static bool
decode_word(const unsigned char *p, size_t n, void *out)
{
	return (fsimg_decmpfs_word_decode(p, n, 0, out));
}

/* Codecs reject bad framing without modifying any byte of their output. */
static void
reject(decode_t decode, const unsigned char *p, size_t n, size_t out_size,
    const char *name)
{
	unsigned char *out, *before;

	out = malloc(out_size);
	before = malloc(out_size);
	CHECK(out != NULL && before != NULL);
	memset(out, 0xa5, out_size);
	bcopy(out, before, out_size);
	case_check(!decode(p, n, out), name, n);
	case_check(bcmp(out, before, out_size) == 0, name, n);
	free(before);
	free(out);
}

/* Each short input has its own allocation, including a zero-length span. */
static void
check_span(decode_t decode, const unsigned char *p, size_t size,
    size_t out_size, bool exact, const char *name)
{
	unsigned char *short_copy, *long_copy;
	void *out;
	size_t n;

	for (n = 0; n < size; n++) {
		short_copy = malloc(n == 0 ? 1 : n);
		CHECK(short_copy != NULL);
		bcopy(p, short_copy, n);
		reject(decode, short_copy, n, out_size, name);
		free(short_copy);
	}
	out = malloc(out_size);
	long_copy = malloc(size + 1);
	CHECK(out != NULL && long_copy != NULL);
	bcopy(p, long_copy, size);
	long_copy[size] = 0xdb;
	case_check(decode(p, size, out), name, size);
	if (exact)
		reject(decode, long_copy, size + 1, out_size, name);
	else
		case_check(decode(long_copy, size + 1, out), name, size + 1);
	free(long_copy);
	free(out);
}

/* Compression framing mixes little-endian metadata with resource BE words. */
static void
test_compression_header(void)
{
	unsigned char p[16] = {0}, bad[16];
	fsimg_decmpfs_header_t header;
	size_t i;

	bcopy("fpmc", p, 4);
	put_le(p + COMPRESSION_TYPE, 0x12345678, 4);
	put_le(p + COMPRESSION_SIZE, UINT64_C(0x7fffffffffffffff), 8);
	check_span(decode_header, p, 16, sizeof(header), false, "header");
	CHECK(fsimg_decmpfs_header_decode(p, 16, &header));
	CHECK(header.type == 0x12345678);
	CHECK(header.uncompressed_size == UINT64_C(0x7fffffffffffffff));
	for (i = 0; i < 4; i++) {
		bcopy(p, bad, 16);
		bad[i] ^= 1;
		reject(decode_header, bad, 16, sizeof(header), "header magic byte");
	}
	put_le(p + COMPRESSION_SIZE, UINT64_C(0x8000000000000000), 8);
	reject(decode_header, p, 16, sizeof(header), "header signed size bound");
}

static void
test_resource_header(void)
{
	unsigned char p[16] = {0};
	fsimg_decmpfs_rsrc_t rsrc;
	fsimg_decmpfs_rsrc_table_t table;

	put_be(p, 0x12345678, 4);
	put_be(p + RESOURCE_MAP, 0x23456789, 4);
	put_be(p + RESOURCE_DATA_LENGTH, 0x3456789a, 4);
	put_be(p + RESOURCE_MAP_LENGTH, 0x456789ab, 4);
	check_span(decode_rsrc, p, 16, sizeof(rsrc), false, "resource");
	CHECK(fsimg_decmpfs_rsrc_decode(p, 16, &rsrc));
	CHECK(rsrc.data_offset == 0x12345678 && rsrc.map_offset == 0x23456789);
	CHECK(rsrc.data_length == 0x3456789a && rsrc.map_length == 0x456789ab);
	put_le(p + RESOURCE_MAP, 0x89abcdef, 4);
	check_span(decode_rsrc_table, p, 8, sizeof(table), false, "resource table");
	CHECK(fsimg_decmpfs_rsrc_table_decode(p, 8, &table));
	CHECK(table.resource_length == 0x12345678 && table.chunk_count == 0x89abcdef);
}

static void
test_chunk_index_bounds(void)
{
	unsigned char p[16] = {0};
	fsimg_decmpfs_pair_t chunk, before;
	uint32_t word, old;
	size_t i;
	static const uint32_t indices[] = {
		0, 1, 2, 0x20000000, 0x40000000, UINT32_MAX
	};

	put_le(p, 0x12345678, 4);
	put_le(p + 4, 0x23456789, 4);
	put_le(p + 8, 0x3456789a, 4);
	put_le(p + 12, 0x456789ab, 4);
	/* These spans begin at the first entry, after the table header. */
	check_span(decode_pair, p, 8, sizeof(chunk), false, "chunk pair");
	check_span(decode_word, p, 4, sizeof(word), false, "chunk word");
	for (i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
		memset(&chunk, 0xa5, sizeof(chunk));
		bcopy(&chunk, &before, sizeof(chunk));
		word = old = 0xa5a5a5a5;
		if (indices[i] < 2) {
			case_check(fsimg_decmpfs_pair_decode(p, 16,
			    indices[i], &chunk), "pair decode", indices[i]);
			case_check(chunk.offset ==
			    (indices[i] == 0 ? 0x12345678u : 0x3456789au),
			    "pair offset LE", indices[i]);
			case_check(chunk.length ==
			    (indices[i] == 0 ? 0x23456789u : 0x456789abu),
			    "pair length LE", indices[i]);
			case_check(fsimg_decmpfs_word_decode(p, 8,
			    indices[i], &word), "word decode", indices[i]);
			case_check(word ==
			    (indices[i] == 0 ? 0x12345678u : 0x23456789u),
			    "word value LE", indices[i]);
		} else {
			case_check(!fsimg_decmpfs_pair_decode(p, 16, indices[i], &chunk),
			    "pair index", indices[i]);
			case_check(bcmp(&chunk, &before, sizeof(chunk)) == 0,
			    "pair failure atomicity", indices[i]);
			case_check(!fsimg_decmpfs_word_decode(p, 8, indices[i], &word),
			    "word index", indices[i]);
			case_check(word == old, "word failure atomicity", indices[i]);
		}
	}
}

typedef struct memory_fork {
	unsigned char	*bytes;
	size_t		 length;
	size_t		 reads;
	size_t		 read_bytes;
	size_t		 fail_on_read;
} memory_fork_t;

static fsimg_status_t
read_fork(void *context, uint64_t offset, void *buffer, size_t length,
    fsimg_error_t *error)
{
	memory_fork_t *fork = context;

	CHECK(offset <= fork->length);
	CHECK(length <= fork->length - offset);
	fork->reads++;
	fork->read_bytes += length;
	if (fork->fail_on_read == fork->reads)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_READER,
		    "injected fork read failure"));
	bcopy(fork->bytes + offset, buffer, length);
	return (FSIMG_OK);
}

/* Ten stored bytes per chunk; the map occupies the final twelve bytes. */
static memory_fork_t
resource_fork(unsigned char *bytes, size_t data_offset, size_t count)
{
	memory_fork_t fork;
	size_t data_length, i;

	data_length = 8 + 8 * count + 10 * count;
	bzero(&fork, sizeof(fork));
	fork.bytes = bytes;
	fork.length = data_offset + data_length + 12;
	bzero(bytes, fork.length);
	put_be(bytes, data_offset, 4);
	put_be(bytes + RESOURCE_MAP, data_offset + data_length, 4);
	put_be(bytes + RESOURCE_DATA_LENGTH, data_length, 4);
	put_be(bytes + RESOURCE_MAP_LENGTH, 12, 4);
	put_be(bytes + data_offset, data_length - 4, 4);
	put_le(bytes + data_offset + 4, count, 4);
	for (i = 0; i < count; i++) {
		put_le(bytes + data_offset + 8 + 8 * i,
		    4 + 8 * count + 10 * i, 4);
		put_le(bytes + data_offset + 12 + 8 * i, 10, 4);
	}
	return (fork);
}

static memory_fork_t
word_fork(unsigned char *bytes, size_t count, size_t table_end)
{
	memory_fork_t fork;
	size_t i;

	bzero(&fork, sizeof(fork));
	fork.bytes = bytes;
	fork.length = table_end + 10 * count;
	bzero(bytes, fork.length);
	for (i = 0; i <= count; i++)
		put_le(bytes + 4 * i, table_end + 10 * i, 4);
	return (fork);
}

/* A size of three chunks, the last one byte long. */
#define THREE_CHUNK_SIZE	(2 * 65536 + 1)

static const fsimg_decmpfs_kind_t kinds[] = {
	FSIMG_DECMPFS_TABLE_RESOURCE, FSIMG_DECMPFS_TABLE_WORDS
};

/* A three-chunk table of either kind, ten stored bytes per chunk. */
static memory_fork_t
three_chunk_fork(unsigned char *bytes, fsimg_decmpfs_kind_t kind)
{
	if (kind == FSIMG_DECMPFS_TABLE_RESOURCE)
		return (resource_fork(bytes, 256, 3));
	return (word_fork(bytes, 3, 32));
}

static void
open_table(memory_fork_t *fork, fsimg_decmpfs_kind_t kind, uint64_t size,
    fsimg_decmpfs_table_t *table)
{
	fsimg_error_t error;

	CHECK(fsimg_decmpfs_open(kind, fork->length, size, read_fork, fork,
	    table, &error) == FSIMG_OK);
}

static void
check_chunk(const fsimg_decmpfs_table_t *table, memory_fork_t *fork,
    uint64_t index, uint64_t expected_offset, size_t expected_stored)
{
	fsimg_error_t error;
	uint64_t offset;
	size_t stored;

	CHECK(fsimg_decmpfs_chunk(table, index, read_fork, fork, &offset,
	    &stored, &error) == FSIMG_OK);
	CHECK(offset == expected_offset);
	CHECK(stored == expected_stored);
}

static void
check_open_rejection(memory_fork_t *fork, fsimg_decmpfs_kind_t kind,
    uint64_t size, fsimg_status_t status, const char *message)
{
	fsimg_decmpfs_table_t table, before;
	fsimg_error_t error;

	memset(&table, 0xa5, sizeof(table));
	bcopy(&table, &before, sizeof(table));
	CHECK(fsimg_decmpfs_open(kind, fork->length, size, read_fork, fork,
	    &table, &error) == status);
	CHECK(error.status == status);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(bcmp(&table, &before, sizeof(table)) == 0);
}

static void
check_chunk_rejection(const fsimg_decmpfs_table_t *table, memory_fork_t *fork,
    uint64_t index, fsimg_status_t status, const char *message)
{
	fsimg_error_t error;
	uint64_t offset = UINT64_C(0xa5a5a5a5a5a5a5a5);
	size_t stored = 12345;

	CHECK(fsimg_decmpfs_chunk(table, index, read_fork, fork, &offset,
	    &stored, &error) == status);
	CHECK(error.status == status);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(offset == UINT64_C(0xa5a5a5a5a5a5a5a5));
	CHECK(stored == 12345);
}

static void
test_resource_tables_return_fork_relative_chunk_ranges(void)
{
	static const struct {
		size_t data_offset;
		size_t count;
		uint64_t size;
	} cases[] = {
		{ .data_offset = 256, .count = 1, .size = 65536 },
		{ .data_offset = 48, .count = 3, .size = 65536 * 2 + 1 }
	};
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	fsimg_error_t error;
	memory_fork_t fork;
	size_t d, count, i, j;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		d = cases[i].data_offset;
		count = cases[i].count;
		fork = resource_fork(bytes, d, count);
		CHECK(fsimg_decmpfs_open(FSIMG_DECMPFS_TABLE_RESOURCE,
		    fork.length, cases[i].size, read_fork, &fork, &table,
		    &error) == FSIMG_OK);
		CHECK(table.table_offset == d + 8);
		CHECK(table.chunk_base == d + 4);
		CHECK(table.payload_start == d + 8 + 8 * count);
		CHECK(table.payload_end == d + 8 + 18 * count);
		CHECK(table.chunks == count);
		CHECK(table.kind == FSIMG_DECMPFS_TABLE_RESOURCE);
		CHECK(fork.reads == 2 && fork.read_bytes == 24);
		for (j = 0; j < count; j++)
			check_chunk(&table, &fork, j,
			    d + 8 + 8 * count + 10 * j, 10);
	}
}

static void
test_resource_open_rejects_inconsistent_headers_and_tables(void)
{
	static const struct {
		size_t offset;
		uint32_t value;
		bool little_endian;
		const char *message;
	} cases[] = {
		{ RESOURCE_DATA_LENGTH, 7, false,
		    "decmpfs resource header does not match the fork" },
		{ RESOURCE_MAP_LENGTH, 11, false,
		    "decmpfs resource header does not match the fork" },
		{ RESOURCE_MAP, 317, false,
		    "decmpfs resource header does not match the fork" },
		{ 256, 57, false, "decmpfs chunk table is malformed" },
		{ 260, 2, true, "decmpfs chunk table is malformed" },
		{ 260, 4, true, "decmpfs chunk table is malformed" }
	};
	unsigned char bytes[512];
	memory_fork_t fork;
	size_t i;

	fork = resource_fork(bytes, 256, 3);
	fork.length = 15;
	check_open_rejection(&fork, FSIMG_DECMPFS_TABLE_RESOURCE, THREE_CHUNK_SIZE,
	    FSIMG_ECORRUPT, "decmpfs resource fork is too short");
	CHECK(fork.reads == 0);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fork = resource_fork(bytes, 256, 3);
		if (cases[i].little_endian)
			put_le(bytes + cases[i].offset, cases[i].value, 4);
		else
			put_be(bytes + cases[i].offset, cases[i].value, 4);
		check_open_rejection(&fork, FSIMG_DECMPFS_TABLE_RESOURCE,
		    THREE_CHUNK_SIZE, FSIMG_ECORRUPT, cases[i].message);
	}
}

static void
test_resource_table_end_past_data_end_is_malformed(void)
{
	unsigned char bytes[512];
	memory_fork_t fork;

	fork = resource_fork(bytes, 256, 3);
	/* Keep the header consistent, but leave only 31 bytes for the table. */
	fork.length = 256 + 31 + 12;
	put_be(bytes + RESOURCE_DATA_LENGTH, 31, 4);
	put_be(bytes + RESOURCE_MAP, 256 + 31, 4);
	put_be(bytes + 256, 27, 4);
	check_open_rejection(&fork, FSIMG_DECMPFS_TABLE_RESOURCE, THREE_CHUNK_SIZE,
	    FSIMG_ECORRUPT, "decmpfs chunk table is malformed");
}

static void
test_word_tables_allow_slack_before_payload(void)
{
	unsigned char bytes[128];
	fsimg_decmpfs_table_t table;
	fsimg_error_t error;
	memory_fork_t fork;
	size_t count, i;

	for (count = 1; count <= 3; count += 2) {
		fork = word_fork(bytes, count, 32);
		CHECK(fsimg_decmpfs_open(FSIMG_DECMPFS_TABLE_WORDS,
		    fork.length, (count - 1) * 65536 + 1, read_fork, &fork,
		    &table, &error) == FSIMG_OK);
		CHECK(table.table_offset == 0 && table.chunk_base == 0);
		CHECK(table.payload_start == 32);
		CHECK(table.payload_end == fork.length);
		CHECK(table.chunks == count);
		CHECK(table.kind == FSIMG_DECMPFS_TABLE_WORDS);
		CHECK(fork.reads == 1 && fork.read_bytes == 4);
		for (i = 0; i < count; i++)
			check_chunk(&table, &fork, i, 32 + 10 * i, 10);
	}
}

static void
test_word_open_rejects_short_forks_and_invalid_table_ends(void)
{
	static const uint32_t ends[] = { 15, 63 };
	unsigned char bytes[128];
	memory_fork_t fork;
	size_t i;

	fork = word_fork(bytes, 3, 32);
	fork.length = 3;
	check_open_rejection(&fork, FSIMG_DECMPFS_TABLE_WORDS, THREE_CHUNK_SIZE,
	    FSIMG_ECORRUPT, "decmpfs resource fork is too short");
	CHECK(fork.reads == 0);
	for (i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
		fork = word_fork(bytes, 3, 32);
		put_le(bytes, ends[i], 4);
		check_open_rejection(&fork, FSIMG_DECMPFS_TABLE_WORDS,
		    THREE_CHUNK_SIZE, FSIMG_ECORRUPT, "decmpfs chunk table is malformed");
	}
}

static void
test_chunk_ranges_must_stay_inside_payload(void)
{
	static const struct {
		uint32_t start;
		uint32_t length;
	} cases[] = {
		{ .start = 31, .length = 1 },
		{ .start = 70031, .length = 2 },
		{ .start = 32, .length = 69633 }
	};
	unsigned char bytes[70064];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;
	size_t i;

	/* Each table's payload interval is [32, 70032). */
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fork = resource_fork(bytes, 16, 1);
		fork.length = 70044;
		put_be(bytes + RESOURCE_DATA_LENGTH, 70016, 4);
		put_be(bytes + RESOURCE_MAP, 70032, 4);
		put_be(bytes + 16, 70012, 4);
		put_le(bytes + 24, cases[i].start - 20, 4);
		put_le(bytes + 28, cases[i].length, 4);
		open_table(&fork, FSIMG_DECMPFS_TABLE_RESOURCE, 1, &table);
		check_chunk_rejection(&table, &fork, 0, FSIMG_ECORRUPT,
		    "decmpfs chunk lies outside the resource data");

		fork = word_fork(bytes, 3, 32);
		fork.length = 70032;
		put_le(bytes + 4, cases[i].start, 4);
		put_le(bytes + 8, cases[i].start + cases[i].length, 4);
		open_table(&fork, FSIMG_DECMPFS_TABLE_WORDS, THREE_CHUNK_SIZE,
		    &table);
		check_chunk_rejection(&table, &fork, 1, FSIMG_ECORRUPT,
		    "decmpfs chunk lies outside the resource data");
	}
}

static void
test_stored_length_limit_is_inclusive(void)
{
	unsigned char bytes[70064];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;

	fork = resource_fork(bytes, 16, 1);
	fork.length = 32 + 69632 + 12;
	put_be(bytes + RESOURCE_DATA_LENGTH, 16 + 69632, 4);
	put_be(bytes + RESOURCE_MAP, 32 + 69632, 4);
	put_be(bytes + 16, 12 + 69632, 4);
	put_le(bytes + 28, 69632, 4);
	open_table(&fork, FSIMG_DECMPFS_TABLE_RESOURCE, 1, &table);
	check_chunk(&table, &fork, 0, 32, 69632);

	fork = word_fork(bytes, 1, 32);
	fork.length = 32 + 69632;
	put_le(bytes + 4, fork.length, 4);
	open_table(&fork, FSIMG_DECMPFS_TABLE_WORDS, 1, &table);
	check_chunk(&table, &fork, 0, 32, 69632);
}

static void
test_resource_chunks_reject_overlap_with_either_neighbour(void)
{
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	fsimg_error_t error;
	memory_fork_t fork;
	size_t i;

	for (i = 0; i < 2; i++) {
		fork = resource_fork(bytes, 256, 3);
		/* Extend the previous pair, or move the next pair back a byte. */
		if (i == 0)
			put_le(bytes + 268, 11, 4);
		else
			put_le(bytes + 280, 47, 4);
		CHECK(fsimg_decmpfs_open(FSIMG_DECMPFS_TABLE_RESOURCE,
		    fork.length, THREE_CHUNK_SIZE, read_fork, &fork, &table,
		    &error) == FSIMG_OK);
		check_chunk_rejection(&table, &fork, 1, FSIMG_ECORRUPT,
		    "decmpfs chunks overlap");
	}
}

static void
test_descending_words_are_overlapping_chunks(void)
{
	unsigned char bytes[128];
	fsimg_decmpfs_table_t table;
	fsimg_error_t error;
	memory_fork_t fork;

	fork = word_fork(bytes, 3, 32);
	put_le(bytes + 8, 41, 4);
	CHECK(fsimg_decmpfs_open(FSIMG_DECMPFS_TABLE_WORDS, fork.length,
	    THREE_CHUNK_SIZE, read_fork, &fork, &table, &error) == FSIMG_OK);
	check_chunk_rejection(&table, &fork, 1, FSIMG_ECORRUPT,
	    "decmpfs chunks overlap");
}

static void
test_chunk_indices_past_table_do_not_read(void)
{
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;
	size_t k;

	for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		fork = three_chunk_fork(bytes, kinds[k]);
		open_table(&fork, kinds[k], THREE_CHUNK_SIZE, &table);
		fork.reads = 0;
		check_chunk_rejection(&table, &fork, table.chunks, FSIMG_ESTATE,
		    "decmpfs chunk index is past the table");
		check_chunk_rejection(&table, &fork, UINT64_MAX, FSIMG_ESTATE,
		    "decmpfs chunk index is past the table");
		CHECK(fork.reads == 0);
	}
}

static void
test_callback_errors_pass_through_open_and_chunk(void)
{
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;
	size_t failing, k, open_reads;

	for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		/* A resource table's open reads twice, a word table's once. */
		open_reads = kinds[k] == FSIMG_DECMPFS_TABLE_RESOURCE ? 2 : 1;
		for (failing = 1; failing <= open_reads; failing++) {
			fork = three_chunk_fork(bytes, kinds[k]);
			fork.fail_on_read = failing;
			check_open_rejection(&fork, kinds[k], THREE_CHUNK_SIZE,
			    FSIMG_EIO, "injected fork read failure");
			CHECK(fork.reads == failing);
		}
		fork = three_chunk_fork(bytes, kinds[k]);
		open_table(&fork, kinds[k], THREE_CHUNK_SIZE, &table);
		fork.fail_on_read = fork.reads + 1;
		check_chunk_rejection(&table, &fork, 1, FSIMG_EIO,
		    "injected fork read failure");
	}
}

static void
test_damaged_last_pair_preserves_earlier_chunks(void)
{
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;

	/* The last pair's length runs one byte past the resource data. */
	fork = resource_fork(bytes, 256, 3);
	put_le(bytes + 284, 11, 4);
	open_table(&fork, FSIMG_DECMPFS_TABLE_RESOURCE, THREE_CHUNK_SIZE,
	    &table);
	check_chunk(&table, &fork, 0, 288, 10);
	check_chunk(&table, &fork, 1, 298, 10);
	check_chunk_rejection(&table, &fork, 2, FSIMG_ECORRUPT,
	    "decmpfs chunk lies outside the resource data");
}

static void
test_middle_chunk_reads_one_bounded_table_window(void)
{
	static const struct {
		uint64_t offset;
		size_t window;
	} expected[] = {
		{ .offset = 298, .window = 3 * FSIMG_DECMPFS_PAIR_BYTES },
		{ .offset = 42, .window = 2 * FSIMG_DECMPFS_WORD_BYTES }
	};
	unsigned char bytes[512];
	fsimg_decmpfs_table_t table;
	memory_fork_t fork;
	size_t k;

	for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		fork = three_chunk_fork(bytes, kinds[k]);
		open_table(&fork, kinds[k], THREE_CHUNK_SIZE, &table);
		fork.reads = 0;
		fork.read_bytes = 0;
		check_chunk(&table, &fork, 1, expected[k].offset, 10);
		CHECK(fork.reads == 1);
		CHECK(fork.read_bytes == expected[k].window);
	}
}

int
main(void)
{
	test_compression_header();
	test_resource_header();
	test_chunk_index_bounds();
	test_resource_tables_return_fork_relative_chunk_ranges();
	test_resource_open_rejects_inconsistent_headers_and_tables();
	test_resource_table_end_past_data_end_is_malformed();
	test_word_tables_allow_slack_before_payload();
	test_word_open_rejects_short_forks_and_invalid_table_ends();
	test_chunk_ranges_must_stay_inside_payload();
	test_stored_length_limit_is_inclusive();
	test_resource_chunks_reject_overlap_with_either_neighbour();
	test_descending_words_are_overlapping_chunks();
	test_chunk_indices_past_table_do_not_read();
	test_callback_errors_pass_through_open_and_chunk();
	test_damaged_last_pair_preserves_earlier_chunks();
	test_middle_chunk_reads_one_bounded_table_window();
	return (0);
}
