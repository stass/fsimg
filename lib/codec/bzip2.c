/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "internal.h"

/*
 * The bzip2 decoder.
 *
 * A stream is a header naming a level, then blocks, then an end mark, all
 * read as bits, most significant first. A block holds a text of bytes
 * transformed three times over, which the decoder undoes in reverse order:
 *
 * 1. Canonical Huffman symbols, the code changing every 50 symbols, give
 *    the transformed bytes: two symbols are digits of a repeat count for
 *    the byte at the front of a move-to-front list, the last ends the
 *    block, and each other moves a list entry to the front and emits it.
 * 2. The transformed bytes are the last column of the sorted rotations of
 *    the text. Sorting them gives the first column, which links each
 *    position to the one following it in the text, and the block's origin
 *    says where the walk along those links starts.
 * 3. In the text, four equal bytes are followed by a count of further
 *    copies. Expanding those gives the block's output, which the block's
 *    checksum covers.
 *
 * Every read is checked against the input first and every write against
 * the output, so nothing passes either. What bounds the work is the limit
 * on a block's transformed bytes: a block of L of them expands to at least
 * L - L / 5 output bytes, so one larger than free + free / 4 for the free
 * output left cannot fit whatever it holds, and is refused as it is read,
 * before any transform. All other work takes a bit of input or writes a
 * byte of output for each step, and a block's fixed setup is paid for by
 * the bits of its header, so a call takes time proportional to its input
 * plus its capacity.
 */
#define BZIP2_LEVEL_BYTES	100000u
#define BZIP2_MAX_LEVEL		9u
#define BZIP2_MAX_BLOCK_BYTES	(BZIP2_MAX_LEVEL * BZIP2_LEVEL_BYTES)

/* Stream layout, in bits; each magic is read as two 24-bit halves. */
#define BZIP2_MAGIC_HALF_BITS	24u
#define BZIP2_BLOCK_MAGIC_HIGH	UINT32_C(0x314159)
#define BZIP2_BLOCK_MAGIC_LOW	UINT32_C(0x265359)
#define BZIP2_END_MAGIC_HIGH	UINT32_C(0x177245)
#define BZIP2_END_MAGIC_LOW	UINT32_C(0x385090)
#define BZIP2_ORIGIN_BITS	24u
#define BZIP2_GROUP_COUNT_BITS	3u
#define BZIP2_SELECTOR_COUNT_BITS 15u
#define BZIP2_LENGTH_START_BITS	5u
#define BZIP2_USED_MAP_BITS	16u

#define BZIP2_MIN_GROUPS	2u
#define BZIP2_MAX_GROUPS	6u
#define BZIP2_MAX_CODE_BITS	20u
#define BZIP2_SYMBOLS_PER_SELECTOR 50u

/* The byte values, the two run digits and the end of block. */
#define BZIP2_BYTE_VALUES	256u
#define BZIP2_MAX_ALPHABET	(BZIP2_BYTE_VALUES + 2u)
#define BZIP2_RUNA		0u
#define BZIP2_RUNB		1u

/*
 * A block has at most one symbol per transformed byte plus the end of
 * block, so this many selectors reach every symbol. A stream may carry more;
 * they are validated and dropped.
 */
#define BZIP2_MAX_SELECTORS	\
	((BZIP2_MAX_BLOCK_BYTES + 1u + BZIP2_SYMBOLS_PER_SELECTOR - 1u) / \
	BZIP2_SYMBOLS_PER_SELECTOR)

/* After this many equal text bytes, the next byte counts further copies. */
#define BZIP2_RUN_BYTES		4u

/* A transformed-byte entry: the byte, and above it a link to the next. */
#define BZIP2_ENTRY_BYTE_MASK	UINT32_C(0xff)
#define BZIP2_ENTRY_LINK_SHIFT	8u

/*
 * A canonical prefix code: how many symbols have each length, and the
 * symbols ordered by length and then by value. Codes of one length are
 * consecutive, and those of the next length start at twice the value after
 * the last of this one.
 */
typedef struct bzip2_code {
	uint16_t	 length_counts[BZIP2_MAX_CODE_BITS + 1u];
	uint16_t	 symbols[BZIP2_MAX_ALPHABET];
} bzip2_code_t;

/*
 * The call's working memory, one allocation. The entries hold a block's
 * transformed bytes and, once inverted, the links of the walk; their number
 * comes from the stream's level and the output capacity.
 */
typedef struct bzip2_work {
	bzip2_code_t	 codes[BZIP2_MAX_GROUPS];
	uint8_t		 selectors[BZIP2_MAX_SELECTORS];
	uint32_t	 byte_counts[BZIP2_BYTE_VALUES];
	size_t		 entry_capacity;
	uint32_t	 entries[];
} bzip2_work_t;

/* The state of one decode: input position, output position, working memory. */
typedef struct bzip2_decoder {
	const uint8_t	*in;
	size_t		 in_len;
	size_t		 position;	/* bytes consumed */
	unsigned	 current;	/* the byte being read */
	unsigned	 current_bits;	/* its bits not yet taken */
	uint8_t		*out;
	size_t		 capacity;
	size_t		 produced;
	bzip2_work_t	*work;
	fsimg_error_t	*error;
} bzip2_decoder_t;

/* The fields of a block ahead of its symbols that later stages need. */
typedef struct bzip2_block {
	uint32_t	 stored_crc;
	uint32_t	 origin;
	size_t		 entries;	/* transformed bytes read */
	uint8_t		 front[BZIP2_BYTE_VALUES];	/* move-to-front list */
	unsigned	 used;		/* byte values in the list */
	unsigned	 groups;
	unsigned	 selector_count;	/* selectors kept */
} bzip2_block_t;

/* CRC-32 with polynomial 0x04C11DB7, most significant bit first, a nibble
 * at a time.
 */
static const uint32_t bzip2_crc_nibbles[16] = {
	UINT32_C(0x00000000), UINT32_C(0x04c11db7), UINT32_C(0x09823b6e),
	UINT32_C(0x0d4326d9), UINT32_C(0x130476dc), UINT32_C(0x17c56b6b),
	UINT32_C(0x1a864db2), UINT32_C(0x1e475005), UINT32_C(0x2608edb8),
	UINT32_C(0x22c9f00f), UINT32_C(0x2f8ad6d6), UINT32_C(0x2b4bcb61),
	UINT32_C(0x350c9b64), UINT32_C(0x31cd86d3), UINT32_C(0x3c8ea00a),
	UINT32_C(0x384fbdbd)
};

static fsimg_status_t
bzip2_fail(const bzip2_decoder_t *decoder, fsimg_status_t status,
    const char *check)
{
	fsimg_error_set(decoder->error, status, 0, FSIMG_OP_READER, check);
	decoder->error->offset = decoder->position;
	decoder->error->offset_valid = true;
	return (status);
}

static inline fsimg_status_t
bzip2_read_bit(bzip2_decoder_t *decoder, unsigned *bit)
{
	if (decoder->current_bits == 0) {
		if (decoder->position == decoder->in_len)
			return (bzip2_fail(decoder, FSIMG_ECORRUPT,
			    "bzip2: stream ends early"));
		decoder->current = decoder->in[decoder->position++];
		decoder->current_bits = 8;
	}
	decoder->current_bits--;
	*bit = (decoder->current >> decoder->current_bits) & 1u;
	return (FSIMG_OK);
}

/* Read count bits, at most 32, as a number, the first bit the highest. */
static fsimg_status_t
bzip2_read_bits(bzip2_decoder_t *decoder, unsigned count, uint32_t *value)
{
	fsimg_status_t status;
	unsigned bit;

	*value = 0;
	for (unsigned i = 0; i < count; i++) {
		status = bzip2_read_bit(decoder, &bit);
		if (status != FSIMG_OK)
			return (status);
		*value = (*value << 1) | bit;
	}
	return (FSIMG_OK);
}

static inline uint32_t
bzip2_crc_add(uint32_t crc, uint8_t byte)
{
	crc = (crc << 4) ^
	    bzip2_crc_nibbles[((crc >> 28) ^ (byte >> 4)) & 0xfu];
	crc = (crc << 4) ^ bzip2_crc_nibbles[((crc >> 28) ^ byte) & 0xfu];
	return (crc);
}

/* The stream header gives the level, which sizes everything after it. */
static fsimg_status_t
bzip2_read_header(bzip2_decoder_t *decoder, uint32_t *level)
{
	fsimg_status_t status;
	uint32_t word;

	status = bzip2_read_bits(decoder, 32, &word);
	if (status != FSIMG_OK)
		return (status);
	if ((word >> 8) != UINT32_C(0x425a68))	/* "BZh" */
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: bad stream header"));
	word &= 0xffu;
	if (word < '1' || word > '9')
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: bad level in stream header"));
	*level = word - '0';
	return (FSIMG_OK);
}

/*
 * The used map: a 16-bit map of which ranges of 16 byte values are in use
 * and, for each such range, a 16-bit map of its values. The values, in
 * order, start the move-to-front list.
 */
static fsimg_status_t
bzip2_read_used_values(bzip2_decoder_t *decoder, bzip2_block_t *block)
{
	fsimg_status_t status;
	uint32_t ranges, values;

	status = bzip2_read_bits(decoder, BZIP2_USED_MAP_BITS, &ranges);
	if (status != FSIMG_OK)
		return (status);
	block->used = 0;
	for (unsigned range = 0; range < BZIP2_USED_MAP_BITS; range++) {
		if ((ranges & (UINT32_C(0x8000) >> range)) == 0)
			continue;
		status = bzip2_read_bits(decoder, BZIP2_USED_MAP_BITS, &values);
		if (status != FSIMG_OK)
			return (status);
		for (unsigned value = 0; value < BZIP2_USED_MAP_BITS; value++) {
			if ((values & (UINT32_C(0x8000) >> value)) != 0)
				block->front[block->used++] =
				    (uint8_t)(range * BZIP2_USED_MAP_BITS +
				    value);
		}
	}
	if (block->used == 0)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: block uses no byte values"));
	return (FSIMG_OK);
}

/*
 * The selectors name the code for each 50 symbols. Each is a unary position
 * in a move-to-front list of the group numbers. A real encoder may write
 * more than the block can use, so those past BZIP2_MAX_SELECTORS are
 * validated like the others and not kept.
 */
static fsimg_status_t
bzip2_read_selectors(bzip2_decoder_t *decoder, bzip2_block_t *block)
{
	uint8_t order[BZIP2_MAX_GROUPS];
	fsimg_status_t status;
	uint32_t count;
	unsigned bit, position;
	uint8_t group;

	status = bzip2_read_bits(decoder, BZIP2_SELECTOR_COUNT_BITS, &count);
	if (status != FSIMG_OK)
		return (status);
	if (count == 0)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: block has no selectors"));
	for (unsigned i = 0; i < BZIP2_MAX_GROUPS; i++)
		order[i] = (uint8_t)i;
	for (uint32_t i = 0; i < count; i++) {
		position = 0;
		for (;;) {
			status = bzip2_read_bit(decoder, &bit);
			if (status != FSIMG_OK)
				return (status);
			if (bit == 0)
				break;
			position++;
			if (position == block->groups)
				return (bzip2_fail(decoder, FSIMG_ECORRUPT,
				    "bzip2: selector past the group count"));
		}
		group = order[position];
		/* bcopy takes the source first. */
		bcopy(order, order + 1, position);
		order[0] = group;
		if (i < BZIP2_MAX_SELECTORS)
			decoder->work->selectors[i] = group;
	}
	block->selector_count = count < BZIP2_MAX_SELECTORS ? (unsigned)count :
	    BZIP2_MAX_SELECTORS;
	return (FSIMG_OK);
}

/*
 * One group's code. The lengths come as a start and then, for each symbol,
 * steps ending in a 0 bit: 10 is one longer, 11 is one shorter. Each length
 * is held to the format's range at every step, so a walk stays bounded in
 * value however long the input lets it run. The code itself must be possible,
 * no more symbols of a length than that length has codes left; an unused
 * pattern is allowed here and refused only if the stream ever uses it.
 */
static fsimg_status_t
bzip2_read_code(bzip2_decoder_t *decoder, unsigned alphabet,
    bzip2_code_t *code)
{
	uint8_t lengths[BZIP2_MAX_ALPHABET];
	uint16_t next[BZIP2_MAX_CODE_BITS + 1u];
	fsimg_status_t status;
	uint32_t length, free_codes;
	unsigned bit;

	status = bzip2_read_bits(decoder, BZIP2_LENGTH_START_BITS, &length);
	if (status != FSIMG_OK)
		return (status);
	bzero(code->length_counts, sizeof(code->length_counts));
	for (unsigned symbol = 0; symbol < alphabet; symbol++) {
		for (;;) {
			if (length < 1 || length > BZIP2_MAX_CODE_BITS)
				return (bzip2_fail(decoder, FSIMG_ECORRUPT,
				    "bzip2: code length out of range"));
			status = bzip2_read_bit(decoder, &bit);
			if (status != FSIMG_OK)
				return (status);
			if (bit == 0)
				break;
			status = bzip2_read_bit(decoder, &bit);
			if (status != FSIMG_OK)
				return (status);
			length = bit == 0 ? length + 1 : length - 1;
		}
		lengths[symbol] = (uint8_t)length;
		code->length_counts[length]++;
	}

	free_codes = 1;
	next[0] = 0;
	for (unsigned bits = 1; bits <= BZIP2_MAX_CODE_BITS; bits++) {
		free_codes *= 2;
		if (code->length_counts[bits] > free_codes)
			return (bzip2_fail(decoder, FSIMG_ECORRUPT,
			    "bzip2: code lengths are oversubscribed"));
		free_codes -= code->length_counts[bits];
		next[bits] = (uint16_t)(next[bits - 1] +
		    code->length_counts[bits - 1]);
	}
	for (unsigned symbol = 0; symbol < alphabet; symbol++)
		code->symbols[next[lengths[symbol]]++] = (uint16_t)symbol;
	return (FSIMG_OK);
}

/*
 * Decode one symbol a bit at a time. At each length, value is the bits so
 * far and first the first code of that length; value is that length's code
 * when it lies within the length's count of first.
 */
static inline fsimg_status_t
bzip2_read_symbol(bzip2_decoder_t *decoder, const bzip2_code_t *code,
    unsigned *symbol)
{
	fsimg_status_t status;
	uint32_t value, first, count;
	unsigned bit, index;

	value = 0;
	first = 0;
	index = 0;
	for (unsigned bits = 1; bits <= BZIP2_MAX_CODE_BITS; bits++) {
		status = bzip2_read_bit(decoder, &bit);
		if (status != FSIMG_OK)
			return (status);
		value = (value << 1) | bit;
		count = code->length_counts[bits];
		if (value - first < count) {
			*symbol = code->symbols[index + (value - first)];
			return (FSIMG_OK);
		}
		index += count;
		first = (first + count) << 1;
	}
	return (bzip2_fail(decoder, FSIMG_ECORRUPT,
	    "bzip2: bit pattern matches no symbol"));
}

/*
 * The most transformed bytes a block can hold and still fit free output
 * bytes: L of them produce at least L - L / 5, the fewest coming from
 * groups of four equal bytes and a zero count, which holds just up to
 * free + free / 4. A block never exceeds the level's size either.
 */
static size_t
bzip2_block_limit(size_t level_bytes, size_t free_bytes)
{
	if (free_bytes >= level_bytes)
		return (level_bytes);
	if (free_bytes + free_bytes / 4 >= level_bytes)
		return (level_bytes);
	return (free_bytes + free_bytes / 4);
}

/*
 * Read the symbols into the entries, counting each byte value for the
 * inversion. Run digits are bijective base 2, lowest first: RUNA adds the
 * current weight and RUNB twice it. The pending count is held against the
 * block limit after every digit, so neither it nor the weight, which never
 * exceeds the count plus one, can pass 32 bits.
 */
static fsimg_status_t
bzip2_read_symbols(bzip2_decoder_t *decoder, bzip2_block_t *block,
    size_t limit)
{
	bzip2_work_t *work = decoder->work;
	const bzip2_code_t *code;
	fsimg_status_t status;
	uint32_t run_bytes, run_weight;
	unsigned symbol, symbols_left, selector, position, end_of_block;
	size_t entries;
	uint8_t value;

	bzero(work->byte_counts, sizeof(work->byte_counts));
	entries = 0;
	run_bytes = 0;
	run_weight = 1;
	selector = 0;
	symbols_left = 0;
	code = NULL;
	end_of_block = block->used + 1;
	for (;;) {
		if (symbols_left == 0) {
			if (selector == block->selector_count)
				return (bzip2_fail(decoder, FSIMG_ECORRUPT,
				    "bzip2: symbols past the last selector"));
			code = &work->codes[work->selectors[selector++]];
			symbols_left = BZIP2_SYMBOLS_PER_SELECTOR;
		}
		symbols_left--;
		status = bzip2_read_symbol(decoder, code, &symbol);
		if (status != FSIMG_OK)
			return (status);

		if (symbol == BZIP2_RUNA || symbol == BZIP2_RUNB) {
			run_bytes += symbol == BZIP2_RUNA ? run_weight :
			    2 * run_weight;
			run_weight *= 2;
			if (run_bytes > limit - entries)
				return (bzip2_fail(decoder, FSIMG_ECORRUPT,
				    "bzip2: block passes its limit"));
			continue;
		}

		value = block->front[0];
		work->byte_counts[value] += run_bytes;
		for (uint32_t i = 0; i < run_bytes; i++)
			work->entries[entries++] = value;
		run_bytes = 0;
		run_weight = 1;
		if (symbol == end_of_block)
			break;

		if (entries == limit)
			return (bzip2_fail(decoder, FSIMG_ECORRUPT,
			    "bzip2: block passes its limit"));
		position = symbol - 1;
		value = block->front[position];
		/* bcopy takes the source first. */
		bcopy(block->front, block->front + 1, position);
		block->front[0] = value;
		work->byte_counts[value]++;
		work->entries[entries++] = value;
	}
	block->entries = entries;
	return (FSIMG_OK);
}

/*
 * Build the walk: the byte value counts give where each value starts in
 * the sorted first column, and the k-th of a value in the first column is
 * the k-th of that value in the last column, the entry's own position. Then
 * walk from the origin, expanding runs and folding every output byte into
 * the block's checksum. Any permutation of a malformed block, even of
 * several cycles, walks over initialized entries for exactly entries steps
 * and fails the checksum.
 */
static fsimg_status_t
bzip2_write_text(bzip2_decoder_t *decoder, const bzip2_block_t *block,
    uint32_t *crc)
{
	bzip2_work_t *work = decoder->work;
	uint32_t total, count, entry, link, block_crc;
	unsigned equal_bytes, previous;
	uint8_t byte;

	total = 0;
	for (unsigned value = 0; value < BZIP2_BYTE_VALUES; value++) {
		count = work->byte_counts[value];
		work->byte_counts[value] = total;
		total += count;
	}
	for (size_t i = 0; i < block->entries; i++) {
		byte = (uint8_t)(work->entries[i] & BZIP2_ENTRY_BYTE_MASK);
		work->entries[work->byte_counts[byte]++] |=
		    (uint32_t)i << BZIP2_ENTRY_LINK_SHIFT;
	}

	block_crc = UINT32_MAX;
	equal_bytes = 0;
	previous = 0;
	link = work->entries[block->origin] >> BZIP2_ENTRY_LINK_SHIFT;
	for (size_t i = 0; i < block->entries; i++) {
		entry = work->entries[link];
		link = entry >> BZIP2_ENTRY_LINK_SHIFT;
		byte = (uint8_t)(entry & BZIP2_ENTRY_BYTE_MASK);
		if (equal_bytes == BZIP2_RUN_BYTES) {
			/* This byte is a count; the next byte starts afresh. */
			if (byte > decoder->capacity - decoder->produced)
				return (bzip2_fail(decoder, FSIMG_ECORRUPT,
				    "bzip2: output longer than its capacity"));
			for (unsigned copy = 0; copy < byte; copy++)
				block_crc = bzip2_crc_add(block_crc,
				    (uint8_t)previous);
			memset(decoder->out + decoder->produced, (int)previous,
			    byte);
			decoder->produced += byte;
			equal_bytes = 0;
			continue;
		}
		if (decoder->produced == decoder->capacity)
			return (bzip2_fail(decoder, FSIMG_ECORRUPT,
			    "bzip2: output longer than its capacity"));
		equal_bytes = equal_bytes > 0 && byte == previous ?
		    equal_bytes + 1 : 1;
		previous = byte;
		decoder->out[decoder->produced++] = byte;
		block_crc = bzip2_crc_add(block_crc, byte);
	}
	*crc = ~block_crc;
	return (FSIMG_OK);
}

/*
 * One block, its magic already read. The block limit is taken from the
 * output still free, so a block that cannot fit stops while it is read.
 */
static fsimg_status_t
bzip2_read_block(bzip2_decoder_t *decoder, uint32_t *crc)
{
	bzip2_block_t block;
	fsimg_status_t status;
	uint32_t groups, randomised, crc_computed;
	size_t limit;

	status = bzip2_read_bits(decoder, 32, &block.stored_crc);
	if (status != FSIMG_OK)
		return (status);
	status = bzip2_read_bits(decoder, 1, &randomised);
	if (status != FSIMG_OK)
		return (status);
	if (randomised != 0)
		return (bzip2_fail(decoder, FSIMG_EUNSUPPORTED,
		    "bzip2: randomised blocks are not supported"));
	status = bzip2_read_bits(decoder, BZIP2_ORIGIN_BITS, &block.origin);
	if (status != FSIMG_OK)
		return (status);
	status = bzip2_read_used_values(decoder, &block);
	if (status != FSIMG_OK)
		return (status);
	status = bzip2_read_bits(decoder, BZIP2_GROUP_COUNT_BITS, &groups);
	if (status != FSIMG_OK)
		return (status);
	if (groups < BZIP2_MIN_GROUPS || groups > BZIP2_MAX_GROUPS)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: group count out of range"));
	block.groups = groups;
	status = bzip2_read_selectors(decoder, &block);
	if (status != FSIMG_OK)
		return (status);
	for (unsigned group = 0; group < block.groups; group++) {
		status = bzip2_read_code(decoder, block.used + 2,
		    &decoder->work->codes[group]);
		if (status != FSIMG_OK)
			return (status);
	}

	limit = bzip2_block_limit(decoder->work->entry_capacity,
	    decoder->capacity - decoder->produced);
	status = bzip2_read_symbols(decoder, &block, limit);
	if (status != FSIMG_OK)
		return (status);
	if (block.origin >= block.entries)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: origin outside the block"));
	status = bzip2_write_text(decoder, &block, &crc_computed);
	if (status != FSIMG_OK)
		return (status);
	if (crc_computed != block.stored_crc)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: block checksum mismatch"));
	*crc = crc_computed;
	return (FSIMG_OK);
}

/* The blocks and the end mark, whose checksum folds in every block's. */
static fsimg_status_t
bzip2_read_blocks(bzip2_decoder_t *decoder)
{
	fsimg_status_t status;
	uint32_t high, low, stream_crc, stored_crc, block_crc;

	stream_crc = 0;
	for (;;) {
		status = bzip2_read_bits(decoder, BZIP2_MAGIC_HALF_BITS, &high);
		if (status != FSIMG_OK)
			return (status);
		status = bzip2_read_bits(decoder, BZIP2_MAGIC_HALF_BITS, &low);
		if (status != FSIMG_OK)
			return (status);
		if (high == BZIP2_END_MAGIC_HIGH && low == BZIP2_END_MAGIC_LOW)
			break;
		if (high != BZIP2_BLOCK_MAGIC_HIGH ||
		    low != BZIP2_BLOCK_MAGIC_LOW)
			return (bzip2_fail(decoder, FSIMG_ECORRUPT,
			    "bzip2: bad block magic"));
		status = bzip2_read_block(decoder, &block_crc);
		if (status != FSIMG_OK)
			return (status);
		stream_crc = ((stream_crc << 1) | (stream_crc >> 31)) ^
		    block_crc;
	}
	status = bzip2_read_bits(decoder, 32, &stored_crc);
	if (status != FSIMG_OK)
		return (status);
	if (stored_crc != stream_crc)
		return (bzip2_fail(decoder, FSIMG_ECORRUPT,
		    "bzip2: stream checksum mismatch"));
	return (FSIMG_OK);
}

uint64_t
fsimg_bzip2_stream_bound(uint64_t decoded)
{
	return (decoded + decoded / 4 + 4096);
}

fsimg_status_t
fsimg_bzip2_decode(fsimg_ctx_t *ctx, const uint8_t *in, size_t in_len,
    uint8_t *out, size_t capacity, fsimg_codec_step_t *step,
    fsimg_error_t *error)
{
	bzip2_decoder_t decoder;
	fsimg_status_t status;
	uint32_t level;
	size_t entry_capacity;

	bzero(step, sizeof(*step));
	bzero(&decoder, sizeof(decoder));
	decoder.in = in;
	decoder.in_len = in_len;
	decoder.out = out;
	decoder.capacity = capacity;
	decoder.error = error;
	if ((in == NULL && in_len != 0) || (out == NULL && capacity != 0))
		return (bzip2_fail(&decoder, FSIMG_EINVAL,
		    "bzip2: invalid buffer arguments"));

	status = bzip2_read_header(&decoder, &level);
	if (status == FSIMG_OK) {
		entry_capacity = bzip2_block_limit(level * BZIP2_LEVEL_BYTES,
		    capacity);
		decoder.work = fsimg_alloc(ctx, sizeof(*decoder.work) +
		    entry_capacity * sizeof(decoder.work->entries[0]), error);
		if (decoder.work == NULL)
			status = error->status;
	}
	if (status == FSIMG_OK) {
		decoder.work->entry_capacity = entry_capacity;
		status = bzip2_read_blocks(&decoder);
		fsimg_dealloc(decoder.work);
	}
	step->consumed = decoder.position;
	step->produced = decoder.produced;
	if (status != FSIMG_OK)
		return (status);
	step->done = true;

	/*
	 * The framing gives the stream's exact end: a chunk holds one stream,
	 * so bytes after the padding are not a further stream.
	 */
	if (decoder.position != in_len)
		return (bzip2_fail(&decoder, FSIMG_ECORRUPT,
		    "bzip2: trailing bytes after the stream"));
	return (FSIMG_OK);
}
