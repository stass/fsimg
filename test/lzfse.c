/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "lzfse-vectors.h"
#include "test.h"

#define GUARD 32
#define POISON 0xa5

static unsigned failures;
static void *scratch;

static bool
decoder_supported(void)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return (true);
#else
	return (false);
#endif
}

static size_t tableless_block(uint8_t *, uint32_t, uint32_t);
static size_t constant_block(uint8_t *, unsigned, uint32_t);
static void check_block_result(const char *, const uint8_t *, size_t, size_t,
    fsimg_status_t, size_t, size_t);
static void test_symbol_bound_refuses_tableless_blocks(void);
static void test_symbol_bound_limits(void);
static void test_symbol_bound_counts_matches_three_times(void);
static void test_header_symbol_limits(void);
static void test_literal_copy_buffer_boundary(void);

static void
expect(const char *name, bool condition)
{
	if (!condition) {
		fprintf(stderr, "%s failed\n", name);
		failures++;
	}
}

static fsimg_status_t
decode(bool raw, const uint8_t *in, size_t length, uint8_t *out,
    size_t capacity, fsimg_codec_step_t *step, fsimg_error_t *error)
{
	if (raw)
		return (fsimg_lzvn_decode(in, length, out, capacity, step, error));
	return (fsimg_lzfse_decode(in, length, out, capacity, scratch, step, error));
}

/* Exact input allocations expose a speculative load to a memory checker.
 * Output guards catch writes even when the caller's capacity stops mid-buffer.
 */
static void
check_stream(const char *name, bool raw, const uint8_t *bytes, size_t length,
    size_t capacity, fsimg_status_t expected, const lzfse_vector_t *vector)
{
	uint8_t *in = malloc(length == 0 ? 1 : length);
	uint8_t *out = malloc(capacity + 2 * GUARD);
	fsimg_codec_step_t step;
	fsimg_error_t error = { 0 };

	CHECK(in != NULL && out != NULL);
	bcopy(bytes, in, length);
	for (unsigned poison = 0; poison < 2; poison++) {
		memset(out, poison == 0 ? POISON : 0x5a, capacity + 2 * GUARD);
		fsimg_status_t status = decode(raw, in, length, out + GUARD,
		    capacity, &step, &error);
		expect(name, status == expected);
		for (size_t i = 0; i < GUARD; i++)
			expect(name, out[i] == (poison == 0 ? POISON : 0x5a) &&
			    out[GUARD + capacity + i] == out[i]);
		if (status == FSIMG_ECORRUPT)
			expect(name, error.offset_valid);
		expect(name, step.done == (status == FSIMG_OK));
		if (status == FSIMG_OK && vector != NULL) {
			expect(name, step.done && step.consumed == length &&
			    step.produced == vector->plain_size);
			for (size_t i = 0; i < vector->plain_size; i++)
				expect(name, out[GUARD + i] == lzfse_plain_byte(vector, i));
		}
	}
	free(out);
	free(in);
}

static void
test_vector_bounds(const lzfse_vector_t *vector, bool raw)
{
	const uint8_t *bytes = vector->encoded + (raw ? 12 : 0);
	size_t length = vector->encoded_size - (raw ? 16 : 0);
	char name[128];
	uint8_t *trailing = malloc(length + 1);

	CHECK(trailing != NULL);
	snprintf(name, sizeof(name), "test_%s_%s_exact_capacity",
	    raw ? "lzvn" : "lzfse", vector->name);
	check_stream(name, raw, bytes, length, vector->plain_size, FSIMG_OK,
	    vector);
	if (vector->plain_size != FSIMG_LZFSE_OUTPUT_MAX) {
		snprintf(name, sizeof(name), "test_%s_%s_short_decode",
		    raw ? "lzvn" : "lzfse", vector->name);
		check_stream(name, raw, bytes, length, vector->plain_size + 1,
		    FSIMG_OK, vector);
	}
	if (vector->plain_size != 0) {
		snprintf(name, sizeof(name), "test_%s_%s_capacity_one_short",
		    raw ? "lzvn" : "lzfse", vector->name);
		check_stream(name, raw, bytes, length, vector->plain_size - 1,
		    FSIMG_ECORRUPT, NULL);
	}
	for (size_t i = 0; i < length; i++) {
		snprintf(name, sizeof(name), "test_%s_%s_truncated_at_%zu",
		    raw ? "lzvn" : "lzfse", vector->name, i);
		check_stream(name, raw, bytes, i, vector->plain_size,
		    FSIMG_ECORRUPT, NULL);
	}
	bcopy(bytes, trailing, length);
	trailing[length] = 0;
	snprintf(name, sizeof(name), "test_%s_%s_trailing_byte",
	    raw ? "lzvn" : "lzfse", vector->name);
	check_stream(name, raw, trailing, length + 1, vector->plain_size,
	    FSIMG_ECORRUPT, NULL);
	free(trailing);
}

static void
store_le(uint8_t *out, uint32_t value)
{
	for (size_t i = 0; i < 4; i++) {
		out[i] = (uint8_t)value;
		value >>= 8;
	}
}

static void
test_invalid_arguments(void)
{
	uint8_t byte = 0;
	fsimg_codec_step_t step;
	fsimg_error_t error;

	for (unsigned raw = 0; raw < 2; raw++) {
		expect("test_input_limit", decode(raw, &byte,
		    FSIMG_LZFSE_INPUT_MAX + 1, &byte, 1, &step, &error) == FSIMG_EINVAL);
		expect("test_output_limit", decode(raw, &byte, 1, &byte,
		    FSIMG_LZFSE_OUTPUT_MAX + 1, &step, &error) == FSIMG_EINVAL);
		expect("test_null_input", decode(raw, NULL, 0, &byte, 0,
		    &step, &error) == FSIMG_EINVAL);
		expect("test_null_output", decode(raw, &byte, 0, NULL, 0,
		    &step, &error) == FSIMG_EINVAL);
		check_stream("test_empty_input", raw, &byte, 0, 0,
		    FSIMG_ECORRUPT, NULL);
	}
	expect("test_null_scratch", fsimg_lzfse_decode(&byte, 1, &byte, 1,
	    NULL, &step, &error) == FSIMG_EINVAL);
}

/* Omitted tables retain zero-bit decoder entries. Both bit-count fields are
 * seven (zero pending bits); seven LMD bytes suffice to initialize the stream.
 */
static size_t
tableless_block(uint8_t *out, uint32_t literals, uint32_t matches)
{

	bzero(out, 39);
	bcopy("bvx2", out, 4);
	store_le(out + 8, literals);
	store_le(out + 13, matches);
	out[21] = 7;
	store_le(out + 24, 32);
	out[15] = 0x70;
	out[23] = 0x70;
	return (39);
}

/* One-symbol L/M/D tables consume no state bits. D=1 permits no-op matches
 * after a stored byte, and literal copies when L is nonzero.
 */
static size_t
constant_block(uint8_t *out, unsigned l_symbol, uint32_t matches)
{

	bzero(out, 780);
	bcopy("bvx1", out, 4);
	store_le(out + 16, matches);
	store_le(out + 24, 8);
	out[50 + 2 * l_symbol] = 64;
	out[90] = 64;
	out[133] = 1;
	return (780);
}

/* These fixtures emit an optional stored 'x' followed by zero-valued literals.
 * Header and symbol-budget failures stop before advancing past that header.
 */
static void
check_block_result(const char *name, const uint8_t *in, size_t length,
    size_t capacity, fsimg_status_t expected, size_t produced, size_t stopped)
{
	uint8_t *out = malloc(capacity + 1);
	fsimg_codec_step_t step;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	CHECK(out != NULL);
	status = decode(false, in, length, out, capacity, &step, &error);
	if (status != expected)
		fprintf(stderr, "%s: status %d, expected %d: %s\n", name,
		    status, expected, error.message);
	expect(name, status == expected);
	expect(name, step.done == (status == FSIMG_OK));
	if (status == FSIMG_OK && expected == FSIMG_OK) {
		expect(name, step.produced == produced && step.consumed == length);
		for (size_t i = 0; i < produced; i++) {
			uint8_t byte = i == 0 && in[3] == '-' ? 'x' : 0;

			expect(name, out[i] == byte);
		}
	} else if (expected != FSIMG_OK) {
		expect(name, strcmp(error.message, "lzfse: invalid block") == 0);
		expect(name, error.offset_valid && error.offset == stopped &&
		    step.consumed == stopped);
	}
	free(out);
}

static void
test_symbol_bound_refuses_tableless_blocks(void)
{
	uint8_t *in = calloc(1, 69632);
	size_t length = 0;

	CHECK(in != NULL);
	while (length + 39 + 4 <= 69632)
		length += tableless_block(in + length, 40000, 0);
	bcopy("bvx$", in + length, 4);
	/* Ten blocks declare 400000 symbols; the eleventh crosses 422144.
	 * The offset is its header, far before the end of the full-size input.
	 */
	check_block_result("test_symbol_bound_refuses_tableless_blocks", in,
	    69632, 65536, FSIMG_ECORRUPT, 0, 10 * 39);
	free(in);
}

static void
test_symbol_bound_limits(void)
{
	static const struct {
		const char *name;
		size_t capacity;
		bool extra;
	} cases[] = {
		{ "test_symbol_bound_at_limit_decodes", 65536, false },
		{ "test_symbol_bound_one_past_limit", 65536, true },
		{ "test_symbol_bound_zero_capacity_at_limit", 0, false },
		{ "test_symbol_bound_zero_capacity_one_past_limit", 0, true }
	};
	uint8_t in[512];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t remaining = 4 * (cases[i].capacity + 40000);
		size_t length = 0, stopped;

		while (remaining != 0) {
			uint32_t literals = remaining > 40000 ? 40000 : remaining;

			length += tableless_block(in + length, literals, 0);
			remaining -= literals;
		}
		stopped = length;
		if (cases[i].extra)
			length += tableless_block(in + length, 4, 0);
		bcopy("bvx$", in + length, 4);
		check_block_result(cases[i].name, in, length + 4,
		    cases[i].capacity, cases[i].extra ? FSIMG_ECORRUPT :
		    FSIMG_OK, 0, stopped);
	}
}

static void
test_symbol_bound_counts_matches_three_times(void)
{
	uint8_t in[2048];
	size_t length;

	bcopy("bvx-", in, 4);
	store_le(in + 4, 1);
	in[8] = 'x';
	length = 9 + constant_block(in + 9, 0, 1);
	/* The table block declares three symbols. Fourteen no-op blocks add
	 * 420000; 713 matches reach 422142, two below the limit.
	 */
	for (unsigned i = 0; i < 14; i++)
		length += tableless_block(in + length, 0, 10000);
	length += tableless_block(in + length, 0, 713);
	bcopy("bvx$", in + length, 4);
	check_block_result("test_match_symbols_below_limit", in, length + 4,
	    65536, FSIMG_OK, 1, 0);
	tableless_block(in + length, 0, 1);
	bcopy("bvx$", in + length + 39, 4);
	check_block_result("test_symbol_bound_counts_matches_three_times", in,
	    length + 43, 65536, FSIMG_ECORRUPT, 0, length);
}

static void
test_header_symbol_limits(void)
{
	static const struct {
		const char *name;
		uint32_t literals;
		uint32_t matches;
		fsimg_status_t status;
	} cases[] = {
		{ "test_literal_header_at_limit", 40000, 0, FSIMG_OK },
		{ "test_too_many_literals", 40004, 0, FSIMG_ECORRUPT },
		{ "test_match_header_at_limit", 0, 10000, FSIMG_OK },
		{ "test_too_many_matches", 0, 10001, FSIMG_ECORRUPT }
	};
	uint8_t in[1024];
	size_t length;
	char name[96];

	for (unsigned version = 1; version <= 2; version++) {
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			size_t stopped;

			bcopy("bvx-", in, 4);
			store_le(in + 4, 1);
			in[8] = 'x';
			if (version == 1) {
				stopped = 9;
				length = 9 + constant_block(in + 9, 0,
				    cases[i].matches);
				store_le(in + 9 + 12, cases[i].literals);
			} else {
				/* V2 omits tables and reuses the preceding block's. */
				stopped = 9 + constant_block(in + 9, 0, 0);
				length = stopped + tableless_block(in + stopped,
				    cases[i].literals, cases[i].matches);
			}
			bcopy("bvx$", in + length, 4);
			snprintf(name, sizeof(name), "%s_v%u", cases[i].name,
			    version);
			check_block_result(name, in, length + 4, 65536,
			    cases[i].status, 1, stopped);
		}
	}
}

static void
test_literal_copy_buffer_boundary(void)
{
	uint8_t in[1409];
	const size_t payload = 633, length = 772 + payload;

	/* L symbol 16 supplies base 16 plus two payload bits per match.
	 * 2503 matches give 40048 bytes; the first five add 3 each, ending
	 * one byte inside the 40064-byte literal buffer. The sixth can add 1
	 * to reach its excluded end. Bits are consumed from the payload's end.
	 * No literals are declared: the stream's zeroed storage supplies them.
	 */
	constant_block(in, 16, 2503);
	store_le(in + 24, payload);
	bzero(in + 772, payload);
	in[length - 1] = 0xff;
	in[length - 2] = 0xc0;
	bcopy("bvx$", in + length, 4);
	check_block_result("test_literal_copy_inside_buffer", in, length + 4,
	    65536, FSIMG_OK, 40063, 0);
	in[length - 2] = 0xd0;
	check_block_result("test_literal_copy_reaches_buffer_end", in,
	    length + 4, 65536, FSIMG_ECORRUPT, 0, 772);
}

static void
test_match_without_distance(void)
{
	static const uint8_t small[] = { 0xf1, 6, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t large[] = { 0xf0, 0, 6, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t before_start[] = { 0, 1, 6, 0, 0, 0, 0, 0, 0, 0 };
	static const struct {
		const char *name;
		const uint8_t *bytes;
		size_t length;
	} cases[] = {
		{ "test_sml_m_without_distance", small, sizeof(small) },
		{ "test_lrg_m_without_distance", large, sizeof(large) },
		{ "test_match_before_output", before_start, sizeof(before_start) }
	};
	uint8_t block[32];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		check_stream(cases[i].name, true, cases[i].bytes, cases[i].length,
		    256, FSIMG_ECORRUPT, NULL);
		bcopy("bvxn", block, 4);
		store_le(block + 4, 16);
		store_le(block + 8, (uint32_t)cases[i].length);
		bcopy(cases[i].bytes, block + 12, cases[i].length);
		bcopy("bvx$", block + 12 + cases[i].length, 4);
		check_stream(cases[i].name, false, block, cases[i].length + 16,
		    256, FSIMG_ECORRUPT, NULL);
	}
}

static void
test_compressed_headers(void)
{
	/* Header fields use disk offsets, independent of the imported C types. */
	static const struct {
		const char *name;
		size_t word;
		unsigned shift;
		uint32_t value;
	} packed_cases[] = {
		{ "test_v2_literal_payload_past_input", 8, 20, 0xfffff },
		{ "test_v2_lmd_payload_past_input", 16, 40, 0xfffff },
	};
	static const struct {
		const char *name;
		size_t offset;
		uint32_t value;
	} cases[] = {
		/* Only a sanitizer distinguishes these from unsafe bit shifts;
		 * the failure status alone is the same with the defect.
		 */
		{ "test_literal_bits_int32_max", 28, INT32_MAX },
		{ "test_lmd_bits_int32_max", 40, INT32_MAX },
		{ "test_literal_payload_past_input", 20, UINT32_MAX },
		{ "test_lmd_payload_past_input", 24, UINT32_MAX },
		{ "test_invalid_magic", 0, 0 }
	};
	uint8_t block[sizeof(lzfse_v1)];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bcopy(lzfse_v1, block, sizeof(block));
		store_le(block + cases[i].offset, cases[i].value);
		check_stream(cases[i].name, false, block, sizeof(block), 8192,
		    FSIMG_ECORRUPT, NULL);
	}
	bcopy(lzfse_fse, block, sizeof(lzfse_fse));
	store_le(block + 24, 31);
	check_stream("test_v2_header_smaller_than_fixed_part", false, block,
	    sizeof(lzfse_fse), 8192, FSIMG_ECORRUPT, NULL);
	for (size_t i = 0; i < sizeof(packed_cases) / sizeof(packed_cases[0]); i++) {
		uint64_t word = 0;

		bcopy(lzfse_fse, block, sizeof(lzfse_fse));
		for (size_t j = 0; j < 8; j++)
			word |= (uint64_t)block[packed_cases[i].word + j] << (8 * j);
		word &= ~(UINT64_C(0xfffff) << packed_cases[i].shift);
		word |= (uint64_t)packed_cases[i].value << packed_cases[i].shift;
		for (size_t j = 0; j < 8; j++)
			block[packed_cases[i].word + j] = (uint8_t)(word >> (8 * j));
		check_stream(packed_cases[i].name, false, block, sizeof(lzfse_fse),
		    8192, FSIMG_ECORRUPT, NULL);
	}

	/* An empty table must not give match symbols a usable distance. */
	bzero(block, 784);
	bcopy("bvx1", block, 4);
	store_le(block + 16, 1);
	store_le(block + 24, 8);
	check_stream("test_zero_tables_with_match", false, block, 780, 64,
	    FSIMG_ECORRUPT, NULL);
	/* Constant tables select L=0, M=1, D=1 with no preceding output.
	 * Frequencies are LE uint16_t at v1 offsets 50, 90 and 130.
	 */
	block[50] = 64;
	block[92] = 64;
	block[133] = 1;
	check_stream("test_fse_match_before_output", false, block, 780, 64,
	    FSIMG_ECORRUPT, NULL);

	/* Eight zero-valued payload bytes initialize a stream with bits == 0;
	 * its first flush loads nothing. The payload ends at the allocation end.
	 * Only a sanitizer distinguishes these two cases from an out-of-bounds
	 * load: their failure status alone also passes with the defect.
	 */
	bcopy(lzfse_v1, block, 772);
	store_le(block + 12, 0);
	store_le(block + 16, 1);
	store_le(block + 20, 0);
	store_le(block + 24, 8);
	store_le(block + 40, 0);
	bzero(block + 772, 8);
	check_stream("test_lmd_zero_bits_at_input_end", false, block, 780, 8192,
	    FSIMG_ECORRUPT, NULL);
	store_le(block + 12, 4);
	store_le(block + 20, 8);
	store_le(block + 24, 0);
	store_le(block + 28, 0);
	check_stream("test_literal_zero_bits_no_lmd_at_input_end", false,
	    block, 780, 8192, FSIMG_ECORRUPT, NULL);
}

static void
write_seed(const char *directory, const char *name, bool raw,
    const uint8_t *bytes, size_t length)
{
	char path[4096];
	FILE *file;

	CHECK(snprintf(path, sizeof(path), "%s/%s-%s.seed", directory,
	    raw ? "lzvn" : "lzfse", name) < (int)sizeof(path));
	file = fopen(path, "wb");
	CHECK(file != NULL);
	/* Bit 0 selects raw LZVN; capacity code 127 selects the full window. */
	CHECK(fputc(254 | raw, file) != EOF);
	CHECK(fwrite(bytes, 1, length, file) == length);
	CHECK(fclose(file) == 0);
}

static void
test_decoders_refuse_unsupported_host(void)
{
	uint8_t out[1];
	fsimg_codec_step_t step;
	fsimg_error_t error = { 0 };

	expect("test_lzfse_refuses_unsupported_host",
	    fsimg_lzfse_decode(lzfse_lzvn, sizeof(lzfse_lzvn), out,
	    sizeof(out), scratch, &step, &error) == FSIMG_EUNSUPPORTED);
	expect("test_lzvn_refuses_unsupported_host",
	    fsimg_lzvn_decode(lzfse_lzvn + 12, sizeof(lzfse_lzvn) - 16,
	    out, sizeof(out), &step, &error) == FSIMG_EUNSUPPORTED);
}

int
main(int argc, char **argv)
{
	static const uint8_t end[] = { 6, 0, 0, 0, 0, 0, 0, 0 };
	lzfse_vector_t empty = { "empty", end, sizeof(end), 0, 61 };

	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		for (size_t i = 0; i < sizeof(lzfse_vectors) / sizeof(lzfse_vectors[0]); i++) {
			const lzfse_vector_t *v = &lzfse_vectors[i];
			write_seed(argv[2], v->name, false, v->encoded, v->encoded_size);
		}
		write_seed(argv[2], "encoded", true, lzfse_lzvn + 12,
		    sizeof(lzfse_lzvn) - 16);
		write_seed(argv[2], "empty", true, end, sizeof(end));
		return (0);
	}
	CHECK(argc == 1);
	expect("test_scratch_nonzero", fsimg_lzfse_scratch_bytes() != 0);
	scratch = malloc(fsimg_lzfse_scratch_bytes());
	CHECK(scratch != NULL);
	memset(scratch, POISON, fsimg_lzfse_scratch_bytes());
	if (!decoder_supported()) {
		test_decoders_refuse_unsupported_host();
		free(scratch);
		return (failures != 0);
	}
	/* Different streams reuse the same scratch, including an empty stream. */
	for (size_t i = 0; i < sizeof(lzfse_vectors) / sizeof(lzfse_vectors[0]); i++)
		test_vector_bounds(&lzfse_vectors[i], false);
	test_vector_bounds(&lzfse_vectors[1], true);
	check_stream("test_lzvn_end_only_zero_capacity", true, end, sizeof(end),
	    0, FSIMG_OK, &empty);
	test_invalid_arguments();
	test_match_without_distance();
	test_compressed_headers();
	test_symbol_bound_refuses_tableless_blocks();
	test_symbol_bound_limits();
	test_symbol_bound_counts_matches_three_times();
	test_header_symbol_limits();
	test_literal_copy_buffer_boundary();
	free(scratch);
	return (failures != 0);
}
