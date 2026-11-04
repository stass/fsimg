/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "digest.h"
#include "hfsplus.h"
#include "test.h"

static void
case_check(bool condition, const char *name, uint32_t unit, size_t length)
{
	if (!condition)
		fprintf(stderr, "unicode case=%s unit=%04x length=%zu\n",
		    name, (unsigned int)unit, length);
	CHECK(condition);
}

static void
store_units(const uint16_t *units, size_t n, unsigned char *bytes)
{
	size_t i;

	for (i = 0; i < n; i++) {
		bytes[2 * i] = (unsigned char)(units[i] >> 8);
		bytes[2 * i + 1] = (unsigned char)units[i];
	}
}

/* The pinned serializations include singleton substitutions, but no Hangul. */
static void
test_table_digests(void)
{
	fsimg_sha256_t fold, classes, decompositions;
	uint16_t units[4], record[6], value;
	unsigned char bytes[12], cls;
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	uint32_t u;
	size_t n;

	fsimg_sha256_init(&fold);
	fsimg_sha256_init(&classes);
	fsimg_sha256_init(&decompositions);
	for (u = 0; u < 65536; u++) {
		value = hfsplus_fold_unit((uint16_t)u);
		store_units(&value, 1, bytes);
		fsimg_sha256_update(&fold, bytes, 2);
		cls = hfsplus_combining_class((uint16_t)u);
		fsimg_sha256_update(&classes, &cls, 1);
		n = hfsplus_unit_decompose((uint16_t)u, units);
		case_check(n >= 1 && n <= 4, "unit decomposition size", u, n);
		if ((u >= 0xac00 && u <= 0xd7a3) ||
		    (n == 1 && units[0] == u))
			continue;
		record[0] = (uint16_t)u;
		record[1] = (uint16_t)n;
		bcopy(units, record + 2, n * sizeof(*units));
		store_units(record, n + 2, bytes);
		fsimg_sha256_update(&decompositions, bytes, 2 * (n + 2));
	}
	/* tools/hfsplus/check-unicode-tables.rb, TABLE_DIGESTS. */
	sha256_final_hex(&fold, hex);
	CHECK(strcmp(hex,
	    "b8e22686df0e37d75ab4e0d24c83cf92a5cab407cb6ff722fa54384d6544be46") == 0);
	sha256_final_hex(&classes, hex);
	CHECK(strcmp(hex,
	    "0c753eab520b1374925dc1128228dfe56987f5284ee78837e116a89190ebc599") == 0);
	sha256_final_hex(&decompositions, hex);
	CHECK(strcmp(hex,
	    "654f209716fc62ecf5d15fff1063cc021d177bf2c0e97918b60ee93569c70496") == 0);
}

/* Hangul decomposes algorithmically to leading, vowel and optional tail. */
static void
test_hangul(void)
{
	static const uint16_t syllables[] = {
		0xac00, 0xac01, 0xac1b, 0xac1c, 0xb098, 0xc544, 0xd788, 0xd7a3
	};
	uint16_t expected[3], actual[4];
	uint32_t index;
	size_t i, n, count;

	for (i = 0; i < sizeof(syllables) / sizeof(syllables[0]); i++) {
		index = syllables[i] - 0xac00;
		expected[0] = (uint16_t)(0x1100 + index / (21 * 28));
		expected[1] = (uint16_t)(0x1161 + index / 28 % 21);
		expected[2] = (uint16_t)(0x11a7 + index % 28);
		count = index % 28 == 0 ? 2 : 3;
		n = hfsplus_unit_decompose(syllables[i], actual);
		case_check(n == count && bcmp(actual, expected,
		    count * sizeof(*actual)) == 0, "Hangul jamo", syllables[i], n);
	}
}

typedef struct name_vector {
	const char *name;
	uint16_t input[12];
	size_t input_count;
	uint16_t stored[12];
	size_t stored_count;
	const char *representative;
} name_vector_t;

/* Native E3 captures, copied at authoring time; never read at run time:
 * e3-small-names/marks-hfsplus.tsv, class0-hfsplus.tsv, class0-hfsx.tsv.
 * A representative names the created marks row behind an EEXIST result.
 */
static const name_vector_t captured_names[] = {
	{ .name = "marks-hfsplus/0061+0334+0334+0334",
	  .input = {0x0061, 0x0334, 0x0334, 0x0334}, .input_count = 4,
	  .stored = {0x0061, 0x0334, 0x0334, 0x0334}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0301+0323",
	  .input = {0x0061, 0x0323, 0x0301, 0x0323}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0323, 0x0301}, .stored_count = 4,
	  .representative = "marks-hfsplus/0061+0323+0323+0301" },
	{ .name = "marks-hfsplus/0061+0301+0323+0301",
	  .input = {0x0061, 0x0301, 0x0323, 0x0301}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0301, 0x0301}, .stored_count = 4,
	  .representative = "marks-hfsplus/0061+0323+0301+0301" },
	{ .name = "marks-hfsplus/0061+0345+0345+0345",
	  .input = {0x0061, 0x0345, 0x0345, 0x0345}, .input_count = 4,
	  .stored = {0x0061, 0x0345, 0x0345, 0x0345}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0334+0334+0334",
	  .input = {0x0061, 0x0334, 0x0334, 0x0334, 0x0334}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0334, 0x0334, 0x0334}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0323+0323+0323",
	  .input = {0x0061, 0x0323, 0x0323, 0x0323, 0x0323}, .input_count = 5,
	  .stored = {0x0061, 0x0323, 0x0323, 0x0323, 0x0323}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0300+0300+0300+0300",
	  .input = {0x0061, 0x0300, 0x0300, 0x0300, 0x0300}, .input_count = 5,
	  .stored = {0x0061, 0x0300, 0x0300, 0x0300, 0x0300}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0345+0345+0345+0345",
	  .input = {0x0061, 0x0345, 0x0345, 0x0345, 0x0345}, .input_count = 5,
	  .stored = {0x0061, 0x0345, 0x0345, 0x0345, 0x0345}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0334+0334+0301",
	  .input = {0x0061, 0x0334, 0x0334, 0x0334, 0x0301}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0334, 0x0334, 0x0301}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0301+0334+0334",
	  .input = {0x0061, 0x0323, 0x0301, 0x0334, 0x0334}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0334, 0x0323, 0x0301}, .stored_count = 5,
	  .representative = "marks-hfsplus/0061+0334+0334+0323+0301" },
	{ .name = "marks-hfsplus/0061+0301+0334+0323+0323",
	  .input = {0x0061, 0x0301, 0x0334, 0x0323, 0x0323}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0323, 0x0323, 0x0301}, .stored_count = 5,
	  .representative = "marks-hfsplus/0061+0334+0323+0323+0301" },
	{ .name = "marks-hfsplus/0061+0301+0301+0301+0301",
	  .input = {0x0061, 0x0301, 0x0301, 0x0301, 0x0301}, .input_count = 5,
	  .stored = {0x0061, 0x0301, 0x0301, 0x0301, 0x0301}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+05B0+05B0+05B0",
	  .input = {0x0061, 0x05b0, 0x05b0, 0x05b0}, .input_count = 4,
	  .stored = {0x0061, 0x05b0, 0x05b0, 0x05b0}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+05B8+0F71+05B8",
	  .input = {0x0061, 0x05b8, 0x0f71, 0x05b8}, .input_count = 4,
	  .stored = {0x0061, 0x05b8, 0x05b8, 0x0f71}, .stored_count = 4,
	  .representative = "marks-hfsplus/0061+05B8+05B8+0F71" },
	{ .name = "marks-hfsplus/0061+0F71+05B8+0F71",
	  .input = {0x0061, 0x0f71, 0x05b8, 0x0f71}, .input_count = 4,
	  .stored = {0x0061, 0x05b8, 0x0f71, 0x0f71}, .stored_count = 4,
	  .representative = "marks-hfsplus/0061+05B8+0F71+0F71" },
	{ .name = "marks-hfsplus/0061+0F72+0F72+0F72",
	  .input = {0x0061, 0x0f72, 0x0f72, 0x0f72}, .input_count = 4,
	  .stored = {0x0061, 0x0f72, 0x0f72, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0344+0323",
	  .input = {0x0061, 0x0334, 0x0344, 0x0323}, .input_count = 4,
	  .stored = {0x0061, 0x0334, 0x0323, 0x0308, 0x0301}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0334+0F75",
	  .input = {0x0061, 0x0323, 0x0334, 0x0f75}, .input_count = 4,
	  .stored = {0x0061, 0x0334, 0x0f71, 0x0f74, 0x0323}, .stored_count = 5,
	  .representative = "marks-hfsplus/0061+0334+0F75+0323" },
	{ .name = "marks-hfsplus/0061+0340+0345+0323",
	  .input = {0x0061, 0x0340, 0x0345, 0x0323}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0300, 0x0345}, .stored_count = 4,
	  .representative = "marks-hfsplus/0061+0323+0300+0345" },
	{ .name = "marks-hfsplus/0061+0343",
	  .input = {0x0061, 0x0343}, .input_count = 2,
	  .stored = {0x0061, 0x0313}, .stored_count = 2,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0345+0344+0323+0062+0323+0344+0345",
	  .input = {0x0061, 0x0345, 0x0344, 0x0323,
	      0x0062, 0x0323, 0x0344, 0x0345}, .input_count = 8,
	  .stored = {0x0061, 0x0323, 0x0308, 0x0301, 0x0345,
	      0x0062, 0x0323, 0x0308, 0x0301, 0x0345}, .stored_count = 10,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0345+0F81+0323+0062+0323+0F81+0345",
	  .input = {0x0061, 0x0345, 0x0f81, 0x0323,
	      0x0062, 0x0323, 0x0f81, 0x0345}, .input_count = 8,
	  .stored = {0x0061, 0x0f71, 0x0f80, 0x0323, 0x0345,
	      0x0062, 0x0f71, 0x0f80, 0x0323, 0x0345}, .stored_count = 10,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0345+0343+0323+0062+0323+0343+0345",
	  .input = {0x0061, 0x0345, 0x0343, 0x0323,
	      0x0062, 0x0323, 0x0343, 0x0345}, .input_count = 8,
	  .stored = {0x0061, 0x0323, 0x0313, 0x0345,
	      0x0062, 0x0323, 0x0313, 0x0345}, .stored_count = 8,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0345+0062+0345+0323",
	  .input = {0x0061, 0x0323, 0x0345, 0x0062, 0x0345, 0x0323}, .input_count = 6,
	  .stored = {0x0061, 0x0323, 0x0345, 0x0062, 0x0323, 0x0345}, .stored_count = 6,
	  .representative = "marks-hfsplus/0061+0345+0323+0062+0323+0345" },
	{ .name = "marks-hfsplus/00E2+0323",
	  .input = {0x00e2, 0x0323}, .input_count = 2,
	  .stored = {0x0061, 0x0323, 0x0302}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0064+0307+0323",
	  .input = {0x0064, 0x0307, 0x0323}, .input_count = 3,
	  .stored = {0x0064, 0x0323, 0x0307}, .stored_count = 3,
	  .representative = "marks-hfsplus/1E0B+0323" },
	{ .name = "marks-hfsplus/D7A3+0334",
	  .input = {0xd7a3, 0x0334}, .input_count = 2,
	  .stored = {0x1112, 0x1175, 0x11c2, 0x0334}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/1E0D+0307",
	  .input = {0x1e0d, 0x0307}, .input_count = 2,
	  .stored = {0x0064, 0x0323, 0x0307}, .stored_count = 3,
	  .representative = "marks-hfsplus/1E0B+0323" },
	{ .name = "marks-hfsplus/05E9+05C1+05BC+05B8",
	  .input = {0x05e9, 0x05c1, 0x05bc, 0x05b8}, .input_count = 4,
	  .stored = {0x05e9, 0x05b8, 0x05bc, 0x05c1}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/05E9+05C1+05B0+05BC+05B8",
	  .input = {0x05e9, 0x05c1, 0x05b0, 0x05bc, 0x05b8}, .input_count = 5,
	  .stored = {0x05e9, 0x05b0, 0x05b8, 0x05bc, 0x05c1}, .stored_count = 5,
	  .representative = "marks-hfsplus/05E9+05C1+05BC+05B8+05B0" },
	{ .name = "marks-hfsplus/05E9+05B8+05BC+05C1+05B0",
	  .input = {0x05e9, 0x05b8, 0x05bc, 0x05c1, 0x05b0}, .input_count = 5,
	  .stored = {0x05e9, 0x05b0, 0x05b8, 0x05bc, 0x05c1}, .stored_count = 5,
	  .representative = "marks-hfsplus/05E9+05C1+05BC+05B8+05B0" },
	{ .name = "marks-hfsplus/05E9+05B0+05B8+05BC+05C1",
	  .input = {0x05e9, 0x05b0, 0x05b8, 0x05bc, 0x05c1}, .input_count = 5,
	  .stored = {0x05e9, 0x05b0, 0x05b8, 0x05bc, 0x05c1}, .stored_count = 5,
	  .representative = "marks-hfsplus/05E9+05C1+05BC+05B8+05B0" },
	{ .name = "marks-hfsplus/0F40+0F71+0F72+0F7C",
	  .input = {0x0f40, 0x0f71, 0x0f72, 0x0f7c}, .input_count = 4,
	  .stored = {0x0f40, 0x0f71, 0x0f72, 0x0f7c}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0F40+0F71+0F74+0F72+0F7C",
	  .input = {0x0f40, 0x0f71, 0x0f74, 0x0f72, 0x0f7c}, .input_count = 5,
	  .stored = {0x0f40, 0x0f71, 0x0f72, 0x0f7c, 0x0f74}, .stored_count = 5,
	  .representative = "marks-hfsplus/0F40+0F71+0F72+0F7C+0F74" },
	{ .name = "marks-hfsplus/0F40+0F7C+0F72+0F74+0F71",
	  .input = {0x0f40, 0x0f7c, 0x0f72, 0x0f74, 0x0f71}, .input_count = 5,
	  .stored = {0x0f40, 0x0f71, 0x0f7c, 0x0f72, 0x0f74}, .stored_count = 5,
	  .representative = "marks-hfsplus/0F40+0F71+0F7C+0F72+0F74" },
	{ .name = "marks-hfsplus/0F40+0F7C+0F73",
	  .input = {0x0f40, 0x0f7c, 0x0f73}, .input_count = 3,
	  .stored = {0x0f40, 0x0f71, 0x0f7c, 0x0f72}, .stored_count = 4,
	  .representative = "marks-hfsplus/0F40+0F71+0F7C+0F72" },
	{ .name = "marks-hfsplus/1E69",
	  .input = {0x1e69}, .input_count = 1,
	  .stored = {0x0073, 0x0323, 0x0307}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "marks-hfsplus/01D5",
	  .input = {0x01d5}, .input_count = 1,
	  .stored = {0x0055, 0x0308, 0x0304}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "marks-hfsplus/1FA8",
	  .input = {0x1fa8}, .input_count = 1,
	  .stored = {0x03a9, 0x0313, 0x0345}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "marks-hfsplus/03A9+0334+0345+0313",
	  .input = {0x03a9, 0x0334, 0x0345, 0x0313}, .input_count = 4,
	  .stored = {0x03a9, 0x0334, 0x0313, 0x0345}, .stored_count = 4,
	  .representative = "marks-hfsplus/1FA8+0334" },
	{ .name = "marks-hfsplus/0061+0323+0323+0301",
	  .input = {0x0061, 0x0323, 0x0323, 0x0301}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0323, 0x0301}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0301+0301",
	  .input = {0x0061, 0x0323, 0x0301, 0x0301}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0301, 0x0301}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0334+0323+0301",
	  .input = {0x0061, 0x0334, 0x0334, 0x0323, 0x0301}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0334, 0x0323, 0x0301}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0323+0323+0301",
	  .input = {0x0061, 0x0334, 0x0323, 0x0323, 0x0301}, .input_count = 5,
	  .stored = {0x0061, 0x0334, 0x0323, 0x0323, 0x0301}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+05B8+05B8+0F71",
	  .input = {0x0061, 0x05b8, 0x05b8, 0x0f71}, .input_count = 4,
	  .stored = {0x0061, 0x05b8, 0x05b8, 0x0f71}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+05B8+0F71+0F71",
	  .input = {0x0061, 0x05b8, 0x0f71, 0x0f71}, .input_count = 4,
	  .stored = {0x0061, 0x05b8, 0x0f71, 0x0f71}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0334+0F75+0323",
	  .input = {0x0061, 0x0334, 0x0f75, 0x0323}, .input_count = 4,
	  .stored = {0x0061, 0x0334, 0x0f71, 0x0f74, 0x0323}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0323+0300+0345",
	  .input = {0x0061, 0x0323, 0x0300, 0x0345}, .input_count = 4,
	  .stored = {0x0061, 0x0323, 0x0300, 0x0345}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0061+0345+0323+0062+0323+0345",
	  .input = {0x0061, 0x0345, 0x0323, 0x0062, 0x0323, 0x0345}, .input_count = 6,
	  .stored = {0x0061, 0x0323, 0x0345, 0x0062, 0x0323, 0x0345}, .stored_count = 6,
	  .representative = NULL },
	{ .name = "marks-hfsplus/1E0B+0323",
	  .input = {0x1e0b, 0x0323}, .input_count = 2,
	  .stored = {0x0064, 0x0323, 0x0307}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "marks-hfsplus/05E9+05C1+05BC+05B8+05B0",
	  .input = {0x05e9, 0x05c1, 0x05bc, 0x05b8, 0x05b0}, .input_count = 5,
	  .stored = {0x05e9, 0x05b0, 0x05b8, 0x05bc, 0x05c1}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0F40+0F71+0F72+0F7C+0F74",
	  .input = {0x0f40, 0x0f71, 0x0f72, 0x0f7c, 0x0f74}, .input_count = 5,
	  .stored = {0x0f40, 0x0f71, 0x0f72, 0x0f7c, 0x0f74}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0F40+0F71+0F7C+0F72+0F74",
	  .input = {0x0f40, 0x0f71, 0x0f7c, 0x0f72, 0x0f74}, .input_count = 5,
	  .stored = {0x0f40, 0x0f71, 0x0f7c, 0x0f72, 0x0f74}, .stored_count = 5,
	  .representative = NULL },
	{ .name = "marks-hfsplus/0F40+0F71+0F7C+0F72",
	  .input = {0x0f40, 0x0f71, 0x0f7c, 0x0f72}, .input_count = 4,
	  .stored = {0x0f40, 0x0f71, 0x0f7c, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "marks-hfsplus/1FA8+0334",
	  .input = {0x1fa8, 0x0334}, .input_count = 2,
	  .stored = {0x03a9, 0x0334, 0x0313, 0x0345}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F73",
	  .input = {0x0061, 0x0f73}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F71+0F72",
	  .input = {0x0061, 0x0f71, 0x0f72}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F72+0F71",
	  .input = {0x0061, 0x0f72, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F72+0F73",
	  .input = {0x0061, 0x0f72, 0x0f73}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F73+0F72",
	  .input = {0x0061, 0x0f73, 0x0f72}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F75",
	  .input = {0x0061, 0x0f75}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F71+0F74",
	  .input = {0x0061, 0x0f71, 0x0f74}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F74+0F71",
	  .input = {0x0061, 0x0f74, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F74+0F75",
	  .input = {0x0061, 0x0f74, 0x0f75}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74, 0x0f74}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F75+0F74",
	  .input = {0x0061, 0x0f75, 0x0f74}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74, 0x0f74}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F81",
	  .input = {0x0061, 0x0f81}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F71+0F80",
	  .input = {0x0061, 0x0f71, 0x0f80}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F80+0F71",
	  .input = {0x0061, 0x0f80, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F80+0F81",
	  .input = {0x0061, 0x0f80, 0x0f81}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80, 0x0f80}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsplus/0061+0F81+0F80",
	  .input = {0x0061, 0x0f81, 0x0f80}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80, 0x0f80}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F73",
	  .input = {0x0061, 0x0f73}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F71+0F72",
	  .input = {0x0061, 0x0f71, 0x0f72}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F72+0F71",
	  .input = {0x0061, 0x0f72, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F72+0F73",
	  .input = {0x0061, 0x0f72, 0x0f73}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F73+0F72",
	  .input = {0x0061, 0x0f73, 0x0f72}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f72, 0x0f72}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F75",
	  .input = {0x0061, 0x0f75}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F71+0F74",
	  .input = {0x0061, 0x0f71, 0x0f74}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F74+0F71",
	  .input = {0x0061, 0x0f74, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F74+0F75",
	  .input = {0x0061, 0x0f74, 0x0f75}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74, 0x0f74}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F75+0F74",
	  .input = {0x0061, 0x0f75, 0x0f74}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f74, 0x0f74}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F81",
	  .input = {0x0061, 0x0f81}, .input_count = 2,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F71+0F80",
	  .input = {0x0061, 0x0f71, 0x0f80}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F80+0F71",
	  .input = {0x0061, 0x0f80, 0x0f71}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80}, .stored_count = 3,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F80+0F81",
	  .input = {0x0061, 0x0f80, 0x0f81}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80, 0x0f80}, .stored_count = 4,
	  .representative = NULL },
	{ .name = "class0-hfsx/0061+0F81+0F80",
	  .input = {0x0061, 0x0f81, 0x0f80}, .input_count = 3,
	  .stored = {0x0061, 0x0f71, 0x0f80, 0x0f80}, .stored_count = 4,
	  .representative = NULL },
};

/* Decomposition reproduces native names and their captured collisions. */
static void
test_capture_vectors(void)
{
	uint16_t actual[32];
	unsigned char a[24], b[24];
	const name_vector_t *row, *other;
	size_t i, j, n;
	bool equal, found;

	for (i = 0; i < sizeof(captured_names) / sizeof(captured_names[0]); i++) {
		row = &captured_names[i];
		case_check(hfsplus_decompose(row->input, row->input_count,
		    actual, 32, &n), row->name, row->input[0], row->input_count);
		case_check(n == row->stored_count &&
		    bcmp(actual, row->stored, n * sizeof(*actual)) == 0,
		    row->name, row->input[0], n);
		store_units(row->stored, row->stored_count, a);
		found = row->representative == NULL;
		for (j = 0; j < sizeof(captured_names) / sizeof(captured_names[0]); j++) {
			other = &captured_names[j];
			store_units(other->stored, other->stored_count, b);
			equal = row->stored_count == other->stored_count &&
			    bcmp(a, b, 2 * row->stored_count) == 0;
			case_check((hfsplus_name_compare(HFSPLUS_COMPARE_FOLDED,
			    a, row->stored_count, b, other->stored_count) == 0) == equal,
			    row->name, row->input[0], j);
			case_check((hfsplus_name_compare(HFSPLUS_COMPARE_BINARY,
			    a, row->stored_count, b, other->stored_count) == 0) == equal,
			    row->name, row->input[0], j);
			if (row->representative != NULL &&
			    strcmp(row->representative, other->name) == 0) {
				case_check(equal, row->name, row->input[0], j);
				found = true;
			}
		}
		case_check(found, row->name, row->input[0], row->input_count);
	}
}

/* Excluded BMP ranges and all supplementary values retain their units. */
static void
test_decomposition_boundaries(void)
{
	static const uint32_t supplementary[] = {
		0x1d15e, 0x1d15f, 0x1d160, 0x1d161, 0x1d162, 0x1d163,
		0x1d164, 0x1d1bb, 0x1d1bc, 0x1d1bd, 0x1d1be, 0x1d1bf, 0x1d1c0,
		0x10000, 0x1f600, 0x10ffff
	};
	static const name_vector_t examples[] = {
		{ .name = "class zero expands before sort",
		  .input = {0x61, 0x0f72, 0x0f73}, .input_count = 3,
		  .stored = {0x61, 0x0f71, 0x0f72, 0x0f72}, .stored_count = 4 },
		{ .name = "mark then supplementary",
		  .input = {0x61, 0x0300, 0xd834, 0xdd65}, .input_count = 4,
		  .stored = {0x61, 0x0300, 0xd834, 0xdd65}, .stored_count = 4 },
		{ .name = "supplementary then mark",
		  .input = {0x61, 0xd834, 0xdd65, 0x0300}, .input_count = 4,
		  .stored = {0x61, 0xd834, 0xdd65, 0x0300}, .stored_count = 4 }
	};
	uint16_t input[255], actual[766], expected[255], unit, pair[2];
	unsigned char before[8], after[8];
	uint32_t u;
	size_t i, n, k;
	const name_vector_t *row;

	for (u = 0x2000; u <= 0xfaff; u++) {
		if (u > 0x2fff && u < 0xf900)
			continue;
		unit = (uint16_t)u;
		n = hfsplus_unit_decompose(unit, actual);
		case_check(n == 1 && actual[0] == unit, "excluded unit", u, n);
		case_check(hfsplus_decompose(&unit, 1, actual, 1, &n) &&
		    n == 1 && actual[0] == unit, "excluded name", u, n);
	}
	for (i = 0; i < sizeof(supplementary) / sizeof(supplementary[0]); i++) {
		u = supplementary[i] - 0x10000;
		pair[0] = (uint16_t)(0xd800 + (u >> 10));
		pair[1] = (uint16_t)(0xdc00 + (u & 0x3ff));
		case_check(hfsplus_decompose(pair, 2, actual, 2, &n) &&
		    n == 2 && bcmp(pair, actual, sizeof(pair)) == 0,
		    "supplementary pair", supplementary[i], n);
	}
	for (i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
		row = &examples[i];
		case_check(hfsplus_decompose(row->input, row->input_count,
		    actual, row->stored_count, &n) && n == row->stored_count &&
		    bcmp(actual, row->stored, n * sizeof(*actual)) == 0,
		    row->name, row->input[0], row->input_count);
	}
	unit = 0x01fa; /* A + ring above + acute: a three-unit table entry. */
	memset(actual, 0xa5, sizeof(actual));
	CHECK(hfsplus_decompose(&unit, 1, actual, 3, &n));
	CHECK(n == 3 && actual[0] == 0x41 && actual[1] == 0x030a &&
	    actual[2] == 0x0301 && actual[3] == 0xa5a5);
	for (i = 0; i < 3; i++) {
		memset(actual, 0xa5, sizeof(actual));
		CHECK(!hfsplus_decompose(&unit, 1, actual, i, &n));
		case_check(actual[i] == 0xa5a5, "capacity guard", unit, i);
	}
	for (i = 0; i < 255; i++)
		input[i] = 0xac01; /* Three jamo each. */
	CHECK(!hfsplus_decompose(input, 255, actual, 255, &n));
	CHECK(hfsplus_decompose(input, 255, actual, 765, &n) && n == 765);
	for (i = 0; i < 765; i += 3)
		case_check(actual[i] == 0x1100 && actual[i + 1] == 0x1161 &&
		    actual[i + 2] == 0x11a8, "long Hangul", 0xac01, i);
	/* Classes 240, 230 (two alternating marks), 220, 1, descending. */
	for (i = 0; i < 51; i++) {
		input[i] = 0x0345;
		input[51 + i] = i % 2 == 0 ? 0x0300 : 0x0301;
		input[102 + i] = i % 2 == 0 ? 0x0301 : 0x0300;
		input[153 + i] = 0x0323;
		input[204 + i] = 0x0334;
	}
	k = 0;
	for (i = 204; i < 255; i++)
		expected[k++] = input[i];
	for (i = 153; i < 204; i++)
		expected[k++] = input[i];
	for (i = 51; i < 153; i++)
		expected[k++] = input[i];
	for (i = 0; i < 51; i++)
		expected[k++] = input[i];
	CHECK(hfsplus_decompose(input, 255, actual, 255, &n) && n == 255);
	CHECK(bcmp(actual, expected, sizeof(expected)) == 0);
	/* Sorting cannot cross the supplementary pair, in either direction. */
	store_units(examples[1].stored, 4, before);
	store_units(examples[2].stored, 4, after);
	CHECK(hfsplus_name_compare(HFSPLUS_COMPARE_FOLDED,
	    before, 4, after, 4) != 0);
	CHECK(hfsplus_name_compare(HFSPLUS_COMPARE_BINARY,
	    before, 4, after, 4) != 0);
}

/* Stored slash and NUL exchange with colon and U+2400, without a terminator. */
static void
test_path_exchanges(void)
{
	static const struct {
		const char *name;
		uint16_t stored[2];
		size_t units;
		const char *path;
		size_t bytes;
	} rows[] = {
		{ .name = "slash", .stored = {0x2f}, .units = 1,
		  .path = ":", .bytes = 1 },
		{ .name = "NUL", .stored = {0}, .units = 1,
		  .path = "\xe2\x90\x80", .bytes = 3 },
		{ .name = "both", .stored = {0x2f, 0}, .units = 2,
		  .path = ":\xe2\x90\x80", .bytes = 4 }
	};
	unsigned char stored[4], key[510], query[510];
	char path[766];
	fsimg_error_t error;
	size_t i, n, kn, qn;
	hfsplus_compare_t kind;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		store_units(rows[i].stored, rows[i].units, stored);
		memset(path, 0xa5, sizeof(path));
		case_check(hfsplus_name_to_path(stored, rows[i].units,
		    path, &n, &error) == FSIMG_OK, rows[i].name, 0, rows[i].units);
		case_check(n == rows[i].bytes && bcmp(path, rows[i].path, n) == 0 &&
		    (unsigned char)path[n] == 0xa5, rows[i].name, 0, n);
		for (kind = HFSPLUS_COMPARE_FOLDED; kind <= HFSPLUS_COMPARE_BINARY;
		    kind++) {
			case_check(hfsplus_stored_key(kind, stored, rows[i].units,
			    key, &kn, &error) == FSIMG_OK, rows[i].name, kind, rows[i].units);
			case_check(hfsplus_query_key(kind, path, n, query, &qn,
			    &error) == FSIMG_OK, rows[i].name, kind, n);
			case_check(kn == qn && bcmp(key, query, kn) == 0,
			    rows[i].name, kind, kn);
		}
	}
}

/* Admission requires a bounded UTF-16 name that survives display and query. */
static void
test_name_admission(void)
{
	static const struct {
		const char *name;
		uint16_t units[12];
		size_t count;
		fsimg_status_t status;
	} rows[] = {
		{ .name = "empty", .count = 0, .status = FSIMG_ECORRUPT },
		{ .name = "high surrogate", .units = {0xd800}, .count = 1,
		  .status = FSIMG_ECORRUPT },
		{ .name = "low surrogate", .units = {0xdc00}, .count = 1,
		  .status = FSIMG_ECORRUPT },
		{ .name = "reversed surrogates", .units = {0xdc00, 0xd800},
		  .count = 2, .status = FSIMG_ECORRUPT },
		{ .name = "dot", .units = {0x2e}, .count = 1,
		  .status = FSIMG_EUNSUPPORTED },
		{ .name = "dot dot", .units = {0x2e, 0x2e}, .count = 2,
		  .status = FSIMG_EUNSUPPORTED },
		{ .name = "three dots", .units = {0x2e, 0x2e, 0x2e}, .count = 3,
		  .status = FSIMG_OK },
		{ .name = "dot space", .units = {0x2e, 0x20}, .count = 2,
		  .status = FSIMG_OK },
		{ .name = "stored colon", .units = {0x3a}, .count = 1,
		  .status = FSIMG_EUNSUPPORTED },
		{ .name = "stored NUL symbol", .units = {0x2400}, .count = 1,
		  .status = FSIMG_EUNSUPPORTED },
		{ .name = "stored slash", .units = {0x2f}, .count = 1,
		  .status = FSIMG_OK },
		{ .name = "stored NUL", .units = {0}, .count = 1,
		  .status = FSIMG_OK },
		{ .name = "precomposed", .units = {0xe9}, .count = 1,
		  .status = FSIMG_EUNSUPPORTED },
		{ .name = "decomposed", .units = {0x65, 0x301}, .count = 2,
		  .status = FSIMG_OK },
		{ .name = "misordered marks", .units = {0x61, 0x301, 0x323},
		  .count = 3, .status = FSIMG_EUNSUPPORTED },
		{ .name = "Icon CR", .units = {0x49, 0x63, 0x6f, 0x6e, 0xd},
		  .count = 5, .status = FSIMG_OK },
		{ .name = "percent text",
		  .units = {0x25, 0x45, 0x46, 0x25, 0x42, 0x46, 0x25, 0x42, 0x46},
		  .count = 9, .status = FSIMG_OK },
		{ .name = "ignorable", .units = {0x200c}, .count = 1,
		  .status = FSIMG_OK },
		{ .name = "supplementary", .units = {0xd834, 0xdd5e}, .count = 2,
		  .status = FSIMG_OK }
	};
	unsigned char bytes[512];
	uint16_t units[256];
	char path[766];
	fsimg_error_t error;
	size_t i, n;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		store_units(rows[i].units, rows[i].count, bytes);
		case_check(hfsplus_name_admit(bytes, rows[i].count, &error) ==
		    rows[i].status, rows[i].name, rows[i].units[0], rows[i].count);
		if (rows[i].status == FSIMG_ECORRUPT)
			case_check(hfsplus_name_to_path(bytes, rows[i].count,
			    path, &n, &error) == FSIMG_ECORRUPT,
			    rows[i].name, rows[i].units[0], rows[i].count);
	}
	for (i = 0; i < 256; i++)
		units[i] = 0x61;
	store_units(units, 256, bytes);
	CHECK(hfsplus_name_admit(bytes, 256, &error) == FSIMG_ECORRUPT);
	CHECK(hfsplus_name_to_path(bytes, 256, path, &n, &error) == FSIMG_ECORRUPT);
	for (i = 0; i < 255; i++)
		units[i] = 0xac01;
	store_units(units, 255, bytes);
	CHECK(hfsplus_name_admit(bytes, 255, &error) == FSIMG_EUNSUPPORTED);
	for (i = 0; i < 255; i++)
		units[i] = 0x4e00;
	store_units(units, 255, bytes);
	CHECK(hfsplus_name_admit(bytes, 255, &error) == FSIMG_OK);
	memset(path, 0xa5, sizeof(path));
	CHECK(hfsplus_name_to_path(bytes, 255, path, &n, &error) == FSIMG_OK);
	CHECK(n == 765 && (unsigned char)path[n] == 0xa5);
	for (i = 0; i < 765; i += 3)
		case_check(bcmp(path + i, "\xe4\xb8\x80", 3) == 0,
		    "three-byte maximum", 0x4e00, i);
	for (i = 0; i < 254; i += 2) {
		units[i] = 0xd834;
		units[i + 1] = 0xdd5e;
	}
	units[254] = 0x61;
	store_units(units, 255, bytes);
	CHECK(hfsplus_name_admit(bytes, 255, &error) == FSIMG_OK);
	CHECK(hfsplus_name_to_path(bytes, 255, path, &n, &error) == FSIMG_OK);
	CHECK(n == 509);
}

typedef struct compare_pair {
	const char *name;
	uint16_t a[6];
	uint16_t b[6];
	size_t an;
	size_t bn;
	int folded;
	int binary;
} compare_pair_t;

static int
signum(int value)
{
	return ((value > 0) - (value < 0));
}

/* Comparison is unsigned unit order after optional folding, never NFD. */
static void
test_comparators(void)
{
	static const compare_pair_t pairs[] = {
		{ .name = "empty and ignorable", .an = 0, .b = {0x200c}, .bn = 1,
		  .folded = -1, .binary = -1 },
		{ .name = "empty and NUL", .an = 0, .b = {0}, .bn = 1,
		  .folded = -1, .binary = -1 },
		{ .name = "empty and letter", .an = 0, .b = {0x61}, .bn = 1,
		  .folded = -1, .binary = -1 },
		{ .name = "ignorable first", .a = {0x200c}, .an = 1,
		  .b = {0x61}, .bn = 1, .folded = -1, .binary = 1 },
		{ .name = "two all-ignorable names", .a = {0x200c}, .an = 1,
		  .b = {0x200d, 0xfeff}, .bn = 2, .folded = 0, .binary = -1 },
		{ .name = "middle ignorable", .a = {0x61, 0x200d, 0x62}, .an = 3,
		  .b = {0x61, 0x62}, .bn = 2, .folded = 0, .binary = 1 },
		{ .name = "edge ignorables", .a = {0xfeff, 0x61, 0x200c}, .an = 3,
		  .b = {0x61}, .bn = 1, .folded = 0, .binary = 1 },
		{ .name = "Latin", .a = {0x41}, .an = 1, .b = {0x61}, .bn = 1,
		  .folded = 0, .binary = -1 },
		{ .name = "Latin-1", .a = {0xc6}, .an = 1, .b = {0xe6}, .bn = 1,
		  .folded = 0, .binary = -1 },
		{ .name = "decomposable unit stays itself", .a = {0xc0}, .an = 1,
		  .b = {0xe0}, .bn = 1, .folded = -1, .binary = -1 },
		{ .name = "Latin extended", .a = {0x141}, .an = 1,
		  .b = {0x142}, .bn = 1, .folded = 0, .binary = -1 },
		{ .name = "Greek", .a = {0x391}, .an = 1, .b = {0x3b1}, .bn = 1,
		  .folded = 0, .binary = -1 },
		{ .name = "Cyrillic", .a = {0x410}, .an = 1, .b = {0x430}, .bn = 1,
		  .folded = 0, .binary = -1 },
		{ .name = "Armenian", .a = {0x531}, .an = 1, .b = {0x561}, .bn = 1,
		  .folded = 0, .binary = -1 },
		{ .name = "Roman numeral", .a = {0x2160}, .an = 1,
		  .b = {0x2170}, .bn = 1, .folded = 0, .binary = -1 },
		{ .name = "fullwidth", .a = {0xff21}, .an = 1,
		  .b = {0xff41}, .bn = 1, .folded = 0, .binary = -1 },
		{ .name = "Georgian", .a = {0x10a0}, .an = 1,
		  .b = {0x10d0}, .bn = 1, .folded = 0, .binary = -1 },
		{ .name = "Latin unfurled page", .a = {0x21c}, .an = 1,
		  .b = {0x21d}, .bn = 1, .folded = -1, .binary = -1 },
		{ .name = "circled letters", .a = {0x24b6}, .an = 1,
		  .b = {0x24d0}, .bn = 1, .folded = -1, .binary = -1 },
		{ .name = "Deseret", .a = {0xd801, 0xdc00}, .an = 2,
		  .b = {0xd801, 0xdc28}, .bn = 2, .folded = -1, .binary = -1 },
		{ .name = "NUL after replacement", .a = {0}, .an = 1,
		  .b = {0xfffd}, .bn = 1, .folded = 1, .binary = -1 },
		{ .name = "NUL equals FFFF", .a = {0}, .an = 1,
		  .b = {0xffff}, .bn = 1, .folded = 0, .binary = -1 },
		{ .name = "before surrogate", .a = {0xd7ff}, .an = 1,
		  .b = {0xd800, 0xdc00}, .bn = 2, .folded = -1, .binary = -1 },
		{ .name = "after surrogate", .a = {0xdbff, 0xdfff}, .an = 2,
		  .b = {0xe000}, .bn = 1, .folded = -1, .binary = -1 },
		{ .name = "prefix", .a = {0x61}, .an = 1,
		  .b = {0x61, 0x62}, .bn = 2, .folded = -1, .binary = -1 },
		{ .name = "no decomposition", .a = {0xe9}, .an = 1,
		  .b = {0x65, 0x301}, .bn = 2, .folded = 1, .binary = 1 }
	};
	unsigned char a[12], b[12], empty[1] = {0};
	hfsplus_ckey_t ka, kb;
	hfsplus_compare_t kind;
	const compare_pair_t *row;
	size_t i;
	int expected;

	for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		row = &pairs[i];
		store_units(row->a, row->an, a);
		store_units(row->b, row->bn, b);
		ka = (hfsplus_ckey_t){ .name = a, .parent = 0x81234567,
		    .units = (uint16_t)row->an };
		kb = (hfsplus_ckey_t){ .name = b, .parent = 0x81234567,
		    .units = (uint16_t)row->bn };
		for (kind = HFSPLUS_COMPARE_FOLDED; kind <= HFSPLUS_COMPARE_BINARY;
		    kind++) {
			expected = kind == HFSPLUS_COMPARE_FOLDED ? row->folded : row->binary;
			case_check(signum(hfsplus_name_compare(kind, a, row->an,
			    b, row->bn)) == expected, row->name, kind, i);
			case_check(signum(hfsplus_name_compare(kind, b, row->bn,
			    a, row->an)) == -expected, row->name, kind, i);
			case_check(signum(hfsplus_ckey_compare(kind, &ka, &kb)) == expected,
			    row->name, kind, i);
			case_check(signum(hfsplus_ckey_compare(kind, &kb, &ka)) == -expected,
			    row->name, kind, i);
			if (row->bn != 0)
				case_check(hfsplus_name_compare(kind, empty, 0, b, row->bn) < 0,
				    "empty before nonempty", kind, i);
		}
	}
	ka = (hfsplus_ckey_t){ .name = empty, .parent = 0x80000000, .units = 0 };
	kb = (hfsplus_ckey_t){ .name = b, .parent = 1, .units = 2 };
	for (kind = HFSPLUS_COMPARE_FOLDED; kind <= HFSPLUS_COMPARE_BINARY; kind++) {
		CHECK(hfsplus_ckey_compare(kind, &ka, &kb) > 0);
		CHECK(hfsplus_ckey_compare(kind, &kb, &ka) < 0);
	}
}

/* Namespace keys decompose first, then fold/drop or preserve units. */
static void
test_namespace_keys(void)
{
	static const struct {
		const char *name;
		uint16_t stored[6];
		size_t units;
		const char *query;
		size_t bytes;
		uint16_t folded[6];
		size_t folded_units;
		uint16_t binary[6];
		size_t binary_units;
	} rows[] = {
		{ .name = "precomposed query", .stored = {0x65, 0x301}, .units = 2,
		  .query = "\xc3\xa9", .bytes = 2,
		  .folded = {0x65, 0x301}, .folded_units = 2,
		  .binary = {0x65, 0x301}, .binary_units = 2 },
		{ .name = "decomposed query", .stored = {0x65, 0x301}, .units = 2,
		  .query = "e\xcc\x81", .bytes = 3,
		  .folded = {0x65, 0x301}, .folded_units = 2,
		  .binary = {0x65, 0x301}, .binary_units = 2 },
		{ .name = "precomposed stored", .stored = {0xe9}, .units = 1,
		  .query = "e\xcc\x81", .bytes = 3,
		  .folded = {0x65, 0x301}, .folded_units = 2,
		  .binary = {0x65, 0x301}, .binary_units = 2 },
		{ .name = "fold case and drop", .stored = {0x41, 0x200d, 0x42},
		  .units = 3, .query = "A\xe2\x80\x8d" "B", .bytes = 5,
		  .folded = {0x61, 0x62}, .folded_units = 2,
		  .binary = {0x41, 0x200d, 0x42}, .binary_units = 3 },
		{ .name = "all ignorable", .stored = {0x200c, 0xfeff}, .units = 2,
		  .query = "\xe2\x80\x8c\xef\xbb\xbf", .bytes = 6,
		  .folded_units = 0, .binary = {0x200c, 0xfeff}, .binary_units = 2 },
		{ .name = "NUL key", .stored = {0}, .units = 1,
		  .query = "\xe2\x90\x80", .bytes = 3,
		  .folded = {0xffff}, .folded_units = 1,
		  .binary = {0}, .binary_units = 1 },
		{ .name = "capital", .stored = {0x41}, .units = 1,
		  .query = "A", .bytes = 1, .folded = {0x61}, .folded_units = 1,
		  .binary = {0x41}, .binary_units = 1 },
		{ .name = "lowercase", .stored = {0x61}, .units = 1,
		  .query = "a", .bytes = 1, .folded = {0x61}, .folded_units = 1,
		  .binary = {0x61}, .binary_units = 1 }
	};
	unsigned char stored[12], expected[12], key[510], query[510];
	hfsplus_compare_t kind;
	fsimg_error_t error;
	size_t i, n, qn, en;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		store_units(rows[i].stored, rows[i].units, stored);
		for (kind = HFSPLUS_COMPARE_FOLDED; kind <= HFSPLUS_COMPARE_BINARY;
		    kind++) {
			if (kind == HFSPLUS_COMPARE_FOLDED) {
				en = 2 * rows[i].folded_units;
				store_units(rows[i].folded, rows[i].folded_units, expected);
			} else {
				en = 2 * rows[i].binary_units;
				store_units(rows[i].binary, rows[i].binary_units, expected);
			}
			case_check(hfsplus_stored_key(kind, stored, rows[i].units,
			    key, &n, &error) == FSIMG_OK, rows[i].name, kind, rows[i].units);
			case_check(n == en && bcmp(key, expected, en) == 0,
			    rows[i].name, kind, n);
			case_check(hfsplus_query_key(kind, rows[i].query, rows[i].bytes,
			    query, &qn, &error) == FSIMG_OK, rows[i].name, kind, rows[i].bytes);
			case_check(qn == en && bcmp(query, expected, en) == 0,
			    rows[i].name, kind, qn);
		}
	}
}

/* Query framing rejects invalid UTF-8 and decompositions beyond 255 units. */
static void
test_key_bounds(void)
{
	static const struct {
		const char *name;
		const char *bytes;
		size_t length;
	} invalid[] = {
		{ .name = "overlong two", .bytes = "\xc0\x80", .length = 2 },
		{ .name = "overlong three", .bytes = "\xe0\x80\x80", .length = 3 },
		{ .name = "UTF-8 surrogate", .bytes = "\xed\xa0\x80", .length = 3 },
		{ .name = "above scalar range", .bytes = "\xf5\x80\x80\x80", .length = 4 },
		{ .name = "truncated", .bytes = "\xe2\x82", .length = 2 },
		{ .name = "stray continuation", .bytes = "\x80", .length = 1 }
	};
	/* One contiguous buffer makes the first byte after the key observable. */
	unsigned char key[HFSPLUS_KEY_BYTES_MAX + 1], stored[512];
	char ascii[766], hangul[765];
	uint16_t units[256];
	fsimg_error_t error;
	hfsplus_compare_t kind;
	size_t i, n;

	memset(ascii, 'a', sizeof(ascii));
	for (i = 0; i < 256; i++)
		units[i] = 0x61;
	store_units(units, 256, stored);
	for (i = 0; i < 255; i++)
		bcopy("\xea\xb0\x81", hangul + 3 * i, 3); /* U+AC01. */
	for (kind = HFSPLUS_COMPARE_FOLDED; kind <= HFSPLUS_COMPARE_BINARY; kind++) {
		for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
			case_check(hfsplus_query_key(kind, invalid[i].bytes,
			    invalid[i].length, key, &n, &error) == FSIMG_EINVAL,
			    invalid[i].name, kind, invalid[i].length);
		memset(key, 0xa5, sizeof(key));
		CHECK(hfsplus_query_key(kind, ascii, 255, key, &n, &error) == FSIMG_OK);
		CHECK(n == 510 && key[510] == 0xa5);
		CHECK(bcmp(key, stored, 510) == 0);
		memset(key, 0xa5, sizeof(key));
		CHECK(hfsplus_stored_key(kind, stored, 255, key, &n, &error) == FSIMG_OK);
		CHECK(n == 510 && key[510] == 0xa5);
		CHECK(bcmp(key, stored, 510) == 0);
		CHECK(hfsplus_query_key(kind, ascii, 256, key, &n, &error) == FSIMG_EINVAL);
		CHECK(hfsplus_query_key(kind, ascii, 766, key, &n, &error) == FSIMG_EINVAL);
		CHECK(hfsplus_query_key(kind, hangul, 765, key, &n, &error) == FSIMG_EINVAL);
		CHECK(hfsplus_stored_key(kind, stored, 256, key, &n, &error) == FSIMG_ECORRUPT);
	}
}

int
main(void)
{
	test_table_digests();
	test_hangul();
	test_capture_vectors();
	test_decomposition_boundaries();
	test_path_exchanges();
	test_name_admission();
	test_comparators();
	test_namespace_keys();
	test_key_bounds();
	return (0);
}
