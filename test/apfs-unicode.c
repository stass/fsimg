/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "digest.h"
#include "test.h"
#include "apfs/unicode-tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SCALAR_LIMIT 0x110000u
/* UnicodeData.txt 17.0.0 assigns 299382 points, including 2048 surrogates. */
#define ASSIGNED_SCALARS 297334
#define UCD_LINE_BYTES 4096
#define UCD_SEQUENCE_POINTS 64
#define E4_LINE_BYTES (2 * APFS_NAME_MAX_BYTES + 64)

typedef struct sequence {
	size_t		 count;
	uint32_t	 points[UCD_SEQUENCE_POINTS];
} sequence_t;

typedef struct folding {
	size_t		 count;
	uint32_t	 points[3];
} folding_t;

typedef struct native_vector {
	apfs_name_rule_t	 rule;
	uint32_t	 word;
	size_t		 count;
	uint32_t	 points[10];
} native_vector_t;

typedef struct form_vector {
	const char	*label;
	apfs_name_rule_t	 rule;
	uint32_t	 input[4];
	size_t		 input_count;
	uint32_t	 output[4];
	size_t		 output_count;
} form_vector_t;

typedef struct key_pair {
	const char	*label;
	apfs_name_rule_t	 rule;
	const char	*left;
	const char	*right;
	bool		 equal;
} key_pair_t;

typedef struct invalid_name {
	const char	*label;
	const char	*bytes;
	size_t		 length;
} invalid_name_t;

/* Large forms have static storage; the test is single-threaded. */
static apfs_name_form_t actual, expected, decomposed;
static const char *ucd_file;
static size_t ucd_line;

static void case_check(bool, const char *, ...);
static bool name_scalar(uint32_t);
static size_t encode_name(const uint32_t *, size_t, char *, size_t);
static void make_form(apfs_name_rule_t, const uint32_t *, size_t,
    apfs_name_form_t *);
static void check_form(apfs_name_rule_t, const uint32_t *, size_t,
    const uint32_t *, size_t, const char *);
static void test_native_hashes_match_stored_records(void);
static bool read_e4_line(FILE *, char *, const char *, size_t);
static unsigned long parse_e4_decimal(const char *, unsigned long,
    const char *, size_t);
static size_t check_e4_hashes(FILE *, const char *);
static void test_e4_fixture_hashes_match(void);
static void test_fresh_volume_names_hash_natively(void);
static void test_e4_full_vectors_match_when_present(void);
static void test_hash_matches_across_chunk_boundaries(void);
static void test_namespace_keys_follow_volume_rule(void);
static void test_invalid_names_leave_empty_forms(void);
static void test_expansion_reaches_key_bound_exactly(void);
static void test_short_key_capacity_stores_nothing(void);
static void test_four_byte_decomposition_stays_in_bound(void);
static void test_long_combining_run_reorders_stably(void);
static void digest_uint32(fsimg_sha256_t *, uint32_t);
static void test_every_scalar_respects_key_bound_and_matches_digest(void);
static FILE *open_ucd(const char *, const char *);
static void check_digest(const char *, const char *, const char *);
static bool read_ucd_line(FILE *, char *);
static char *trim(char *);
static size_t split_fields(char *, char **, size_t);
static sequence_t parse_points(const char *);
static bool forbidden_name_point(const sequence_t *);
static void test_ucd_normalization_matches_columns(const char *);
static folding_t *read_foldings(const char *);
static void test_ucd_caseless_matches_full_folding(const char *);
static void test_ucd_conformance(void);
static void check_assigned_point(uint32_t, bool);
static void test_unassigned_and_noncharacter_points_are_refused(void);
static void test_ascii_private_use_and_empty_names_are_assigned(void);
static void test_assigned_range_interiors_and_endpoints_are_accepted(void);
static void test_last_assigned_point_accepted_and_next_refused(void);
static void test_every_noncharacter_is_refused_alone_and_embedded(void);
static void test_assignment_refuses_non_strict_utf8(void);
static bool *read_ucd_assignment(const char *);
static void test_every_scalar_assignment_matches_ucd_and_digest(void);
static void test_assignment_matches_native_outcomes_when_native_is_set(void);

static void
case_check(bool condition, const char *format, ...)
{
	va_list args;

	if (!condition) {
		fputs("apfs unicode case=", stderr);
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		/* UCD tests set this diagnostic context and reset it after use. */
		if (ucd_file != NULL)
			fprintf(stderr, " file=%s line=%zu", ucd_file, ucd_line);
		fputc('\n', stderr);
	}
	CHECK(condition);
}

static bool
name_scalar(uint32_t point)
{
	return (point > 0 && point < SCALAR_LIMIT && point != '/' &&
	    point != '.' && !(point >= 0xd800 && point <= 0xdfff));
}

/* Independent UTF-8 fixture encoder, also used to check namespace bytes. */
static size_t
encode_name(const uint32_t *points, size_t count, char *name, size_t capacity)
{
	uint32_t point;
	size_t length = 0, width;

	for (size_t i = 0; i < count; i++) {
		point = points[i];
		CHECK(point < SCALAR_LIMIT &&
		    !(point >= 0xd800 && point <= 0xdfff));
		width = point < 0x80 ? 1 : point < 0x800 ? 2 :
		    point < 0x10000 ? 3 : 4;
		CHECK(width <= capacity - length);
		if (width == 1) {
			name[length] = (char)point;
		} else {
			name[length] = (char)((width == 2 ? 0xc0 :
			    width == 3 ? 0xe0 : 0xf0) |
			    (point >> (6 * (width - 1))));
			for (size_t j = 1; j < width; j++)
				name[length + j] = (char)(0x80 |
				    ((point >> (6 * (width - 1 - j))) & 0x3f));
		}
		length += width;
	}
	return (length);
}

static void
make_form(apfs_name_rule_t rule, const uint32_t *points, size_t count,
    apfs_name_form_t *form)
{
	char name[APFS_NAME_MAX_BYTES];
	size_t length;

	length = encode_name(points, count, name, sizeof(name));
	case_check(apfs_name_form(rule, name, length, form) == FSIMG_OK,
	    "form accepts fixture point=U+%04X length=%zu rule=%d",
	    (unsigned)points[0], length, rule);
}

static void
check_form(apfs_name_rule_t rule, const uint32_t *input, size_t input_count,
    const uint32_t *output, size_t output_count, const char *label)
{
	char key[APFS_NAME_KEY_BYTES], bytes[APFS_NAME_KEY_BYTES];
	size_t length, wanted;

	make_form(rule, input, input_count, &actual);
	case_check(actual.count == output_count && bcmp(actual.points, output,
	    output_count * sizeof(*output)) == 0,
	    "%s point=U+%04X count=%zu rule=%d",
	    label, (unsigned)input[0], actual.count, rule);
	wanted = encode_name(output, output_count, bytes, sizeof(bytes));
	CHECK(apfs_name_key(&actual, key, sizeof(key), &length) == FSIMG_OK);
	case_check(length == wanted && bcmp(key, bytes, wanted) == 0,
	    "%s point=U+%04X length=%zu rule=%d",
	    label, (unsigned)input[0], length, rule);
}

/* Captured by tools/apfs/capture.rb with newfs_apfs default and -e on
 * macOS 26.6.2 (25G83); stored records read with apfs-fuse's apfs-dump-quick.
 * The word is the stored drec name_len_and_hash, including the NUL length.
 */
static const native_vector_t native_vectors[] = {
	{ .rule = APFS_NAMES_CASELESS, .word = 0x21819804,
	  .count = 1, .points = {0xa7c0} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x2447c804,
	  .count = 1, .points = {0xa7ce} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x26a4e004,
	  .count = 1, .points = {0xa7c7} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x33694004,
	  .count = 1, .points = {0xa7ba} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x3dabf00a,
	  .count = 8, .points = {
	    0x0130, 0x0073, 0x0074, 0x0061, 0x006e,
	    0x0062, 0x0075, 0x006c} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x4751d404,
	  .count = 1, .points = {0xab70} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x49969404,
	  .count = 1, .points = {0xf900} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x4e52d404,
	  .count = 1, .points = {0x1fd3} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x51b20c08,
	  .count = 6, .points = {0x0053, 0x0074, 0x0072, 0x0061, 0x00df, 0x0065} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x51cb1805,
	  .count = 1, .points = {0x11938} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x75d97c03,
	  .count = 1, .points = {0x00e9} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x7957b002,
	  .count = 1, .points = {0x0061} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x7e01d807,
	  .count = 6, .points = {0x0052, 0x0045, 0x0041, 0x0044, 0x004d, 0x0045} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x8079ac04,
	  .count = 1, .points = {0x1e9e} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x829a8404,
	  .count = 1, .points = {0xfb05} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x94658005,
	  .count = 1, .points = {0x105c9} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x987bb404,
	  .count = 1, .points = {0x212b} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x9c83f006,
	  .count = 3, .points = {0x0061, 0x0301, 0x0316} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0x9cbe140b,
	  .count = 10, .points = {
	    0x002e, 0x0066, 0x0073, 0x0065, 0x0076,
	    0x0065, 0x006e, 0x0074, 0x0073, 0x0064} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0xab2bdc0f,
	  .count = 7, .points = {
	    0x03a3, 0x03af, 0x03c3, 0x03c5, 0x03c6,
	    0x03bf, 0x03c2} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0xbaedc005,
	  .count = 1, .points = {0x1e900} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0xdb68d404,
	  .count = 1, .points = {0x1c89} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0xecbe6c07,
	  .count = 2, .points = {0xd55c, 0xae00} },
	{ .rule = APFS_NAMES_CASELESS, .word = 0xf84f9807,
	  .count = 2, .points = {0x1c90, 0x1c91} },
	{ .rule = APFS_NAMES_NFD, .word = 0x179ce808,
	  .count = 6, .points = {0x0053, 0x0074, 0x0072, 0x0061, 0x00df, 0x0065} },
	{ .rule = APFS_NAMES_NFD, .word = 0x22bef407,
	  .count = 6, .points = {0x0052, 0x0045, 0x0041, 0x0044, 0x004d, 0x0045} },
	{ .rule = APFS_NAMES_NFD, .word = 0x25c3a004,
	  .count = 1, .points = {0xa7ba} },
	{ .rule = APFS_NAMES_NFD, .word = 0x32ed2804,
	  .count = 1, .points = {0xa7ce} },
	{ .rule = APFS_NAMES_NFD, .word = 0x35c85004,
	  .count = 1, .points = {0xa7c7} },
	{ .rule = APFS_NAMES_NFD, .word = 0x372b7804,
	  .count = 1, .points = {0xa7c0} },
	{ .rule = APFS_NAMES_NFD, .word = 0x49969404,
	  .count = 1, .points = {0xf900} },
	{ .rule = APFS_NAMES_NFD, .word = 0x4e52d404,
	  .count = 1, .points = {0x1fd3} },
	{ .rule = APFS_NAMES_NFD, .word = 0x514c3004,
	  .count = 1, .points = {0x1c89} },
	{ .rule = APFS_NAMES_NFD, .word = 0x51cb1805,
	  .count = 1, .points = {0x11938} },
	{ .rule = APFS_NAMES_NFD, .word = 0x6895b807,
	  .count = 2, .points = {0x1c90, 0x1c91} },
	{ .rule = APFS_NAMES_NFD, .word = 0x75d97c03,
	  .count = 1, .points = {0x00e9} },
	{ .rule = APFS_NAMES_NFD, .word = 0x7957b002,
	  .count = 1, .points = {0x0061} },
	{ .rule = APFS_NAMES_NFD, .word = 0x7c23d804,
	  .count = 1, .points = {0x1e9e} },
	{ .rule = APFS_NAMES_NFD, .word = 0x8bf19c0f,
	  .count = 7, .points = {
	    0x03a3, 0x03af, 0x03c3, 0x03c5, 0x03c6,
	    0x03bf, 0x03c2} },
	{ .rule = APFS_NAMES_NFD, .word = 0x8cf22004,
	  .count = 1, .points = {0xab70} },
	{ .rule = APFS_NAMES_NFD, .word = 0x92830404,
	  .count = 1, .points = {0xfb05} },
	{ .rule = APFS_NAMES_NFD, .word = 0x94658005,
	  .count = 1, .points = {0x105c9} },
	{ .rule = APFS_NAMES_NFD, .word = 0x99edd004,
	  .count = 1, .points = {0x212b} },
	{ .rule = APFS_NAMES_NFD, .word = 0x9c83f006,
	  .count = 3, .points = {0x0061, 0x0301, 0x0316} },
	{ .rule = APFS_NAMES_NFD, .word = 0x9cbe140b,
	  .count = 10, .points = {
	    0x002e, 0x0066, 0x0073, 0x0065, 0x0076,
	    0x0065, 0x006e, 0x0074, 0x0073, 0x0064} },
	{ .rule = APFS_NAMES_NFD, .word = 0xb231e805,
	  .count = 1, .points = {0x1e900} },
	{ .rule = APFS_NAMES_NFD, .word = 0xc4ad680a,
	  .count = 8, .points = {
	    0x0130, 0x0073, 0x0074, 0x0061, 0x006e,
	    0x0062, 0x0075, 0x006c} },
	{ .rule = APFS_NAMES_NFD, .word = 0xecbe6c07,
	  .count = 2, .points = {0xd55c, 0xae00} },
};

static void
test_native_hashes_match_stored_records(void)
{
	char name[APFS_NAME_MAX_BYTES];
	size_t length;

	CHECK(COUNT(native_vectors) == 48);
	for (size_t i = 0; i < COUNT(native_vectors); i++) {
		const native_vector_t *row = &native_vectors[i];

		length = encode_name(row->points, row->count, name, sizeof(name));
		case_check((row->word & 0x3ff) == length + 1,
		    "native stored length row=%zu rule=%d point=U+%04X",
		    i, row->rule, (unsigned)row->points[0]);
		make_form(row->rule, row->points, row->count, &actual);
		case_check(apfs_name_hash(&actual) == row->word >> 10,
		    "native stored hash row=%zu rule=%d point=U+%04X",
		    i, row->rule, (unsigned)row->points[0]);
	}
}

/* Read complete bounded lines, rejecting embedded NULs as malformed input. */
static bool
read_e4_line(FILE *file, char *line, const char *path, size_t line_number)
{
	size_t length = 0;
	int byte;

	while ((byte = fgetc(file)) != EOF && byte != '\n') {
		case_check(byte != 0,
		    "E4 line contains NUL file=%s line=%zu",
		    path, line_number);
		case_check(length < E4_LINE_BYTES - 1,
		    "E4 line exceeds buffer file=%s line=%zu",
		    path, line_number);
		line[length++] = (char)byte;
	}
	case_check(ferror(file) == 0, "E4 read file=%s line=%zu",
	    path, line_number);
	line[length] = '\0';
	return (byte != EOF || length != 0);
}

static unsigned long
parse_e4_decimal(const char *text, unsigned long maximum,
    const char *path, size_t line_number)
{
	unsigned long value;
	char *end;

	case_check(*text != '\0', "E4 empty decimal file=%s line=%zu",
	    path, line_number);
	for (const char *p = text; *p != '\0'; p++)
		case_check(*p >= '0' && *p <= '9',
		    "E4 decimal syntax file=%s line=%zu value=%s",
		    path, line_number, text);
	errno = 0;
	value = strtoul(text, &end, 10);
	case_check(errno == 0 && *end == '\0' && value <= maximum,
	    "E4 decimal range file=%s line=%zu value=%s maximum=%lu",
	    path, line_number, text, maximum);
	return (value);
}

static size_t
check_e4_hashes(FILE *file, const char *path)
{
	char line[E4_LINE_BYTES], name[APFS_NAME_MAX_BYTES];
	char *fields[4], *separator, pair[3];
	unsigned long stored_length, stored_hash;
	apfs_name_rule_t rule;
	size_t line_number = 0, rows = 0, length, hex_length;
	uint32_t computed_hash;
	bool hashes_match = true;

	while (read_e4_line(file, line, path, ++line_number)) {
		if (line[0] == '#' || strspn(line, " \t\r") == strlen(line))
			continue;
		fields[0] = line;
		for (size_t i = 1; i < COUNT(fields); i++) {
			separator = strchr(fields[i - 1], '\t');
			case_check(separator != NULL,
			    "E4 missing field file=%s line=%zu field=%zu",
			    path, line_number, i + 1);
			*separator = '\0';
			fields[i] = separator + 1;
		}
		case_check(strchr(fields[3], '\t') == NULL,
		    "E4 extra field file=%s line=%zu", path, line_number);
		case_check(strcmp(fields[1], "ci") == 0 ||
		    strcmp(fields[1], "cs") == 0,
		    "E4 personality file=%s line=%zu value=%s",
		    path, line_number, fields[1]);
		rule = strcmp(fields[1], "ci") == 0 ?
		    APFS_NAMES_CASELESS : APFS_NAMES_NFD;
		stored_length = parse_e4_decimal(fields[2],
		    APFS_NAME_MAX_BYTES + 1, path, line_number);
		stored_hash = parse_e4_decimal(fields[3], 0x3fffff,
		    path, line_number);

		hex_length = strlen(fields[0]);
		case_check(hex_length > 0 && hex_length % 2 == 0 &&
		    hex_length / 2 <= sizeof(name),
		    "E4 hex length file=%s line=%zu length=%zu",
		    path, line_number, hex_length);
		length = hex_length / 2;
		for (size_t i = 0; i < length; i++) {
			pair[0] = fields[0][2 * i];
			pair[1] = fields[0][2 * i + 1];
			pair[2] = '\0';
			case_check(isxdigit((unsigned char)pair[0]) &&
			    isxdigit((unsigned char)pair[1]),
			    "E4 hex syntax file=%s line=%zu hex=%s byte=%zu",
			    path, line_number, fields[0], i);
			name[i] = (char)strtoul(pair, NULL, 16);
		}
		case_check(stored_length == length + 1,
		    "E4 stored length file=%s line=%zu hex=%s personality=%s stored=%lu computed=%zu",
		    path, line_number, fields[0], fields[1],
		    stored_length, length + 1);
		case_check(apfs_name_form(rule, name, length, &actual) == FSIMG_OK,
		    "E4 form file=%s line=%zu hex=%s personality=%s",
		    path, line_number, fields[0], fields[1]);
		computed_hash = apfs_name_hash(&actual);
		if (computed_hash != stored_hash) {
			fprintf(stderr,
			    "E4 hash file=%s line=%zu hex=%s personality=%s stored=%lu computed=%u\n",
			    path, line_number, fields[0], fields[1],
			    stored_hash, (unsigned)computed_hash);
			hashes_match = false;
		}
		rows++;
	}
	CHECK(fclose(file) == 0);
	printf("E4 hashes: %zu data lines file=%s\n", rows, path);
	case_check(hashes_match, "E4 stored hashes match file=%s", path);
	return (rows);
}

static void
test_e4_fixture_hashes_match(void)
{
	FILE *file;
	size_t rows;

	file = fopen(APFS_NAME_HASHES, "rb");
	if (file == NULL)
		perror(APFS_NAME_HASHES);
	CHECK(file != NULL);
	rows = check_e4_hashes(file, APFS_NAME_HASHES);
	case_check(rows == 219,
	    "E4 fixture data lines expected=219 actual=%zu", rows);
}

static void
test_fresh_volume_names_hash_natively(void)
{
	/*
	 * The root and private-directory records of every fresh native
	 * volume (E4 research record).
	 */
	static const struct {
		const char	*name;
		uint32_t	 hash;
	} cases[] = {
		{ .name = "root", .hash = 2989177 },
		{ .name = "private-dir", .hash = 2828707 }
	};
	for (size_t i = 0; i < COUNT(cases); i++) {
		CHECK(apfs_name_form(APFS_NAMES_CASELESS, cases[i].name,
		    strlen(cases[i].name), &actual) == FSIMG_OK);
		case_check(apfs_name_hash(&actual) == cases[i].hash,
		    "named caseless hash name=%s stored=%u computed=%u",
		    cases[i].name, (unsigned)cases[i].hash,
		    (unsigned)apfs_name_hash(&actual));
	}
}

static void
test_e4_full_vectors_match_when_present(void)
{
	char path[4096];
	const char *directory;
	FILE *file;
	size_t rows;
	int length;

	directory = getenv("FSIMG_APFS_NATIVE");
	if (directory == NULL) {
		puts("E4 full vectors skipped: FSIMG_APFS_NATIVE is unset");
		return;
	}
	length = snprintf(path, sizeof(path), "%s/e4-vectors.txt", directory);
	CHECK(length >= 0 && (size_t)length < sizeof(path));
	file = fopen(path, "rb");
	if (file == NULL && errno == ENOENT) {
		printf("E4 full vectors skipped: file does not exist: %s\n", path);
		return;
	}
	if (file == NULL)
		perror(path);
	CHECK(file != NULL);
	rows = check_e4_hashes(file, path);
	case_check(rows == 33369,
	    "E4 full data lines expected=33369 actual=%zu", rows);
}

static void
test_hash_matches_across_chunk_boundaries(void)
{
	static const struct {
		size_t		 count;
		uint32_t	 hash;
	} cases[] = {
		{ .count = 63, .hash = 0x26896f },
		{ .count = 64, .hash = 0x23bed0 },
		{ .count = 65, .hash = 0x066236 },
		{ .count = 128, .hash = 0x1c25b8 },
		{ .count = 129, .hash = 0x30ea82 }
	};
	uint32_t points[129];

	/* Values computed outside the test with a bitwise Ruby CRC-32C:
	 * reflected Castagnoli polynomial 0x82F63B78, register seeded
	 * 0xffffffff, no final complement,
	 * low 22 bits. These span the hash's 64-point chunk boundaries.
	 */
	for (size_t i = 0; i < COUNT(points); i++)
		points[i] = 'a' + i % 26;
	for (size_t i = 0; i < COUNT(cases); i++) {
		make_form(APFS_NAMES_NFD, points, cases[i].count, &actual);
		case_check(apfs_name_hash(&actual) == cases[i].hash,
		    "hash chunk boundary count=%zu", cases[i].count);
	}
}

static void
test_namespace_keys_follow_volume_rule(void)
{
	static const form_vector_t forms[] = {
		{ .label = "dotted I", .rule = APFS_NAMES_CASELESS,
		  .input = {0x130}, .input_count = 1,
		  .output = {'i', 0x307}, .output_count = 2 },
		{ .label = "dotless I does not fold", .rule = APFS_NAMES_CASELESS,
		  .input = {0x131}, .input_count = 1,
		  .output = {0x131}, .output_count = 1 },
		{ .label = "Hangul with tail", .rule = APFS_NAMES_NFD,
		  .input = {0xd55c}, .input_count = 1,
		  .output = {0x1112, 0x1161, 0x11ab}, .output_count = 3 },
		{ .label = "Hangul without tail", .rule = APFS_NAMES_NFD,
		  .input = {0xac00}, .input_count = 1,
		  .output = {0x1100, 0x1161}, .output_count = 2 },
		{ .label = "CJK canonical decomposition", .rule = APFS_NAMES_NFD,
		  .input = {0xf900}, .input_count = 1,
		  .output = {0x8c48}, .output_count = 1 },
		{ .label = "bytes preserve decoded points", .rule = APFS_NAMES_BYTES,
		  .input = {0xe9, 0x301, 0x316}, .input_count = 3,
		  .output = {0xe9, 0x301, 0x316}, .output_count = 3 },
		{ .label = "canonical reordering", .rule = APFS_NAMES_NFD,
		  .input = {'a', 0x301, 0x316}, .input_count = 3,
		  .output = {'a', 0x316, 0x301}, .output_count = 3 },
		{ .label = "reorder before folding", .rule = APFS_NAMES_CASELESS,
		  .input = {'a', 0x345, 0x301}, .input_count = 3,
		  .output = {'a', 0x301, 0x3b9}, .output_count = 3 }
	};
	static const key_pair_t pairs[] = {
		{ .label = "sharp s uppercase", .rule = APFS_NAMES_CASELESS,
		  .left = "Stra\xc3\x9f" "e", .right = "STRASSE", .equal = true },
		{ .label = "sharp s lowercase", .rule = APFS_NAMES_CASELESS,
		  .left = "Stra\xc3\x9f" "e", .right = "strasse", .equal = true },
		{ .label = "capital sharp s", .rule = APFS_NAMES_CASELESS,
		  .left = "\xe1\xba\x9e", .right = "ss", .equal = true },
		{ .label = "dotted I versus i", .rule = APFS_NAMES_CASELESS,
		  .left = "\xc4\xb0", .right = "i", .equal = false },
		{ .label = "dotted versus dotless I", .rule = APFS_NAMES_CASELESS,
		  .left = "\xc4\xb0", .right = "\xc4\xb1", .equal = false },
		{ .label = "capital versus small sigma", .rule = APFS_NAMES_CASELESS,
		  .left = "\xce\xa3", .right = "\xcf\x83", .equal = true },
		{ .label = "small versus final sigma", .rule = APFS_NAMES_CASELESS,
		  .left = "\xcf\x83", .right = "\xcf\x82", .equal = true },
		{ .label = "Mtavruli folding", .rule = APFS_NAMES_CASELESS,
		  .left = "\xe1\xb2\x90", .right = "\xe1\x83\x90", .equal = true },
		{ .label = "Mtavruli case sensitive", .rule = APFS_NAMES_NFD,
		  .left = "\xe1\xb2\x90", .right = "\xe1\x83\x90", .equal = false },
		{ .label = "Kelvin folding", .rule = APFS_NAMES_CASELESS,
		  .left = "\xe2\x84\xaa", .right = "k", .equal = true },
		{ .label = "Hangul jamo spelling", .rule = APFS_NAMES_NFD,
		  .left = "\xed\x95\x9c",
		  .right = "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab", .equal = true },
		{ .label = "Hangul LV spelling", .rule = APFS_NAMES_NFD,
		  .left = "\xea\xb0\x80", .right = "\xe1\x84\x80\xe1\x85\xa1",
		  .equal = true },
		{ .label = "accent NFD", .rule = APFS_NAMES_NFD,
		  .left = "\xc3\xa9", .right = "e\xcc\x81", .equal = true },
		{ .label = "accent caseless", .rule = APFS_NAMES_CASELESS,
		  .left = "\xc3\xa9", .right = "e\xcc\x81", .equal = true },
		{ .label = "accent bytes", .rule = APFS_NAMES_BYTES,
		  .left = "\xc3\xa9", .right = "e\xcc\x81", .equal = false }
	};
	char left[APFS_NAME_KEY_BYTES], right[APFS_NAME_KEY_BYTES];
	size_t left_length, right_length;
	bool equal;

	for (size_t i = 0; i < COUNT(forms); i++) {
		const form_vector_t *row = &forms[i];

		check_form(row->rule, row->input, row->input_count,
		    row->output, row->output_count, row->label);
	}
	for (size_t i = 0; i < COUNT(pairs); i++) {
		const key_pair_t *row = &pairs[i];

		CHECK(apfs_name_form(row->rule, row->left, strlen(row->left),
		    &actual) == FSIMG_OK);
		CHECK(apfs_name_key(&actual, left, sizeof(left),
		    &left_length) == FSIMG_OK);
		CHECK(apfs_name_form(row->rule, row->right, strlen(row->right),
		    &actual) == FSIMG_OK);
		CHECK(apfs_name_key(&actual, right, sizeof(right),
		    &right_length) == FSIMG_OK);
		equal = left_length == right_length &&
		    bcmp(left, right, left_length) == 0;
		case_check(equal == row->equal, "%s equality row=%zu rule=%d",
		    row->label, i, row->rule);
	}
}

static void
test_invalid_names_leave_empty_forms(void)
{
	static const invalid_name_t invalid[] = {
		{ "empty", "", 0 },
		{ "dot", ".", 1 },
		{ "dot dot", "..", 2 },
		{ "embedded NUL", "a\0b", 3 },
		{ "slash", "a/b", 3 },
		{ "lone continuation", "a\x80", 2 },
		{ "overlong two", "a\xc0\x80", 3 },
		{ "overlong three", "a\xe0\x80\x80", 4 },
		{ "surrogate", "a\xed\xa0\x80", 4 },
		{ "above scalar range", "a\xf4\x90\x80\x80", 5 },
		{ "truncated", "a\xe2\x82", 3 }
	};
	char name[1023];

	memset(name, 'a', sizeof(name));
	for (apfs_name_rule_t rule = APFS_NAMES_BYTES;
	    rule <= APFS_NAMES_CASELESS; rule++) {
		for (size_t i = 0; i < COUNT(invalid); i++) {
			/* Seed a valid form so every failure must clear it. */
			CHECK(apfs_name_form(rule, "seed", 4, &actual) == FSIMG_OK);
			case_check(apfs_name_form(rule, invalid[i].bytes,
			    invalid[i].length, &actual) == FSIMG_ECORRUPT,
			    "%s rejected rule=%d row=%zu length=%zu",
			    invalid[i].label, rule, i, invalid[i].length);
			case_check(actual.count == 0,
			    "%s leaves empty form rule=%d row=%zu",
			    invalid[i].label, rule, i);
		}
		CHECK(apfs_name_form(rule, "...", 3, &actual) == FSIMG_OK);
		CHECK(apfs_name_form(rule, ".a", 2, &actual) == FSIMG_OK);
		CHECK(apfs_name_form(rule, name, 1022, &actual) == FSIMG_OK);
		CHECK(apfs_name_form(rule, name, 1023, &actual) ==
		    FSIMG_EUNSUPPORTED);
		CHECK(actual.count == 0);
	}
}

static void
test_expansion_reaches_key_bound_exactly(void)
{
	uint32_t points[511];
	char name[1022], key[APFS_NAME_KEY_BYTES];
	size_t length, key_length;

	for (size_t i = 0; i < 511; i++)
		points[i] = 0x390;
	length = encode_name(points, 511, name, sizeof(name));
	CHECK(length == 1022);
	make_form(APFS_NAMES_CASELESS, points, 511, &actual);
	CHECK(actual.count == 1533);
	CHECK(apfs_name_key(&actual, key, sizeof(key), &key_length) == FSIMG_OK);
	CHECK(key_length == APFS_NAME_KEY_BYTES);
}

static void
test_short_key_capacity_stores_nothing(void)
{
	uint32_t points[511];
	char key[APFS_NAME_KEY_BYTES];
	size_t key_length;

	for (size_t i = 0; i < COUNT(points); i++)
		points[i] = 0x390;
	make_form(APFS_NAMES_CASELESS, points, COUNT(points), &actual);
	CHECK(apfs_name_key(&actual, key, sizeof(key), &key_length) == FSIMG_OK);
	memset(key, 0xa5, sizeof(key));
	CHECK(apfs_name_key(&actual, key, key_length - 1, &key_length) ==
	    FSIMG_ERANGE);
	CHECK(key_length == 0);
	for (size_t i = 0; i < sizeof(key); i++)
		CHECK((unsigned char)key[i] == 0xa5);
}

static void
test_four_byte_decomposition_stays_in_bound(void)
{
	uint32_t points[257];
	char name[1022], key[APFS_NAME_KEY_BYTES];
	size_t length, key_length;

	for (size_t i = 0; i < 255; i++)
		points[i] = 0x1d160;
	points[255] = 'a';
	points[256] = 'b';
	length = encode_name(points, 257, name, sizeof(name));
	CHECK(length == 1022);
	for (apfs_name_rule_t rule = APFS_NAMES_NFD;
	    rule <= APFS_NAMES_CASELESS; rule++) {
		make_form(rule, points, COUNT(points), &actual);
		case_check(actual.count == 767,
		    "four-byte decomposition count=767 rule=%d", rule);
		CHECK(apfs_name_key(&actual, key, sizeof(key), &key_length) ==
		    FSIMG_OK);
		case_check(key_length == 3062,
		    "four-byte decomposition key length=3062 rule=%d", rule);
	}
}

static void
test_long_combining_run_reorders_stably(void)
{
	uint32_t points[512], reordered[512];
	char name[1022];
	size_t length;

	/* Two ASCII starters leave room for 510 two-byte combining marks. */
	points[0] = reordered[0] = 'a';
	points[1] = reordered[1] = 'b';
	for (size_t i = 0; i < 510; i++) {
		points[2 + i] = i % 2 == 0 ? 0x301 : 0x316;
		reordered[2 + i] = i < 255 ? 0x316 : 0x301;
	}
	length = encode_name(points, COUNT(points), name, sizeof(name));
	CHECK(length == 1022);
	check_form(APFS_NAMES_NFD, points, COUNT(points), reordered,
	    COUNT(reordered), "long combining run NFD");
	check_form(APFS_NAMES_CASELESS, points, COUNT(points), reordered,
	    COUNT(reordered), "long combining run caseless");
}

static void
digest_uint32(fsimg_sha256_t *digest, uint32_t value)
{
	unsigned char bytes[4];

	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (unsigned char)(value >> (8 * i));
	fsimg_sha256_update(digest, bytes, sizeof(bytes));
}

/* Reproduce this digest with Ruby 4.0.7 (Unicode 17.0.0): for every scalar
 * name_scalar accepts, in increasing order, use unicode_normalize(:nfd)
 * for NFD and downcase(:fold).unicode_normalize(:nfd) for CASELESS.
 * Serialize NFD then CASELESS, each as a little-endian uint32 count followed
 * by little-endian uint32 points. This is exact for single scalars; an
 * independent table-driven reference built from UnicodeData.txt and
 * CaseFolding.txt 17.0.0 agrees. Ruby's normalizer must not be an oracle
 * for multi-mark strings: it misorders Tibetan U+0F73/U+0F75/U+0F81 after
 * marks such as U+0323 (see the E4 research record).
 * It changes when the pinned Unicode version does.
 */
static void
test_every_scalar_respects_key_bound_and_matches_digest(void)
{
	char name[4], key[APFS_NAME_KEY_BYTES];
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	fsimg_sha256_t digest;
	size_t length, key_length;

	fsimg_sha256_init(&digest);
	for (uint32_t point = 1; point < SCALAR_LIMIT; point++) {
		if (!name_scalar(point))
			continue;
		length = encode_name(&point, 1, name, sizeof(name));
		for (apfs_name_rule_t rule = APFS_NAMES_BYTES;
		    rule <= APFS_NAMES_CASELESS; rule++) {
			case_check(apfs_name_form(rule, name, length, &actual) ==
			    FSIMG_OK, "scalar accepted point=U+%04X rule=%d",
			    (unsigned)point, rule);
			CHECK(apfs_name_key(&actual, key, sizeof(key),
			    &key_length) == FSIMG_OK);
			case_check(key_length <= 3 * length,
			    "scalar key expansion point=U+%04X rule=%d",
			    (unsigned)point, rule);
			if (rule == APFS_NAMES_BYTES)
				case_check(key_length == length &&
				    bcmp(key, name, length) == 0,
				    "scalar byte identity point=U+%04X length=%zu",
				    (unsigned)point, key_length);
			if (rule == APFS_NAMES_NFD || rule == APFS_NAMES_CASELESS) {
				digest_uint32(&digest, (uint32_t)actual.count);
				for (size_t i = 0; i < actual.count; i++)
					digest_uint32(&digest, actual.points[i]);
			}
		}
	}
	sha256_final_hex(&digest, hex);
	case_check(strcmp(hex,
	    "171287102ac21c758a9d7fd3eae527e5b2f8e663c0496bdacc4e6ec7879fc19d") == 0,
	    "all scalar forms SHA-256");
}

static FILE *
open_ucd(const char *directory, const char *filename)
{
	char path[4096];
	FILE *file;
	int length;

	length = snprintf(path, sizeof(path), "%s/%s", directory, filename);
	CHECK(length >= 0 && (size_t)length < sizeof(path));
	file = fopen(path, "rb");
	if (file == NULL)
		perror(path);
	CHECK(file != NULL);
	ucd_file = filename;
	ucd_line = 0;
	return (file);
}

static void
check_digest(const char *directory, const char *filename, const char *wanted)
{
	unsigned char bytes[8192];
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	fsimg_sha256_t digest;
	FILE *file;
	size_t length;

	file = open_ucd(directory, filename);
	fsimg_sha256_init(&digest);
	while ((length = fread(bytes, 1, sizeof(bytes), file)) != 0)
		fsimg_sha256_update(&digest, bytes, length);
	CHECK(ferror(file) == 0);
	CHECK(fclose(file) == 0);
	sha256_final_hex(&digest, hex);
	case_check(strcmp(hex, wanted) == 0, "pinned UCD SHA-256");
}

/* Reject a truncated line rather than interpreting its prefix as a row. */
static bool
read_ucd_line(FILE *file, char *line)
{
	if (fgets(line, UCD_LINE_BYTES, file) == NULL) {
		CHECK(ferror(file) == 0);
		return (false);
	}
	ucd_line++;
	case_check(strchr(line, '\n') != NULL || feof(file),
	    "bounded UCD line length=%zu", strlen(line));
	return (true);
}

static char *
trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return (text);
}

/* Keep empty fields so malformed rows cannot silently change shape. */
static size_t
split_fields(char *line, char **fields, size_t capacity)
{
	char *separator;
	size_t count = 0;

	for (;;) {
		case_check(count < capacity, "UCD field count=%zu", count);
		fields[count++] = line;
		separator = strchr(line, ';');
		if (separator == NULL)
			break;
		*separator = '\0';
		line = separator + 1;
	}
	for (size_t i = 0; i < count; i++)
		fields[i] = trim(fields[i]);
	return (count);
}

static sequence_t
parse_points(const char *text)
{
	sequence_t sequence = {0};
	unsigned long point;
	char *end;

	while (*text != '\0') {
		case_check(isxdigit((unsigned char)*text),
		    "UCD hexadecimal point index=%zu", sequence.count);
		errno = 0;
		point = strtoul(text, &end, 16);
		case_check(errno == 0 && end != text && point < SCALAR_LIMIT &&
		    !(point >= 0xd800 && point <= 0xdfff) &&
		    (*end == '\0' || isspace((unsigned char)*end)),
		    "UCD scalar syntax point=U+%04lX index=%zu",
		    point, sequence.count);
		case_check(sequence.count < COUNT(sequence.points),
		    "UCD sequence capacity point=U+%04lX count=%zu",
		    point, sequence.count);
		sequence.points[sequence.count++] = (uint32_t)point;
		text = end;
		while (isspace((unsigned char)*text))
			text++;
	}
	case_check(sequence.count > 0, "nonempty UCD sequence");
	return (sequence);
}

static bool
forbidden_name_point(const sequence_t *sequence)
{
	for (size_t i = 0; i < sequence->count; i++) {
		if (sequence->points[i] == 0 || sequence->points[i] == '/')
			return (true);
	}
	return (false);
}

static void
test_ucd_normalization_matches_columns(const char *directory)
{
	sequence_t columns[5];
	char line[UCD_LINE_BYTES], *fields[6], *text, *comment;
	bool *part1, skip;
	FILE *file;
	size_t field_count, rows = 0, skipped = 0, dot_columns = 0;
	unsigned part = 0;
	bool seen_part[6] = {false};

	part1 = calloc(SCALAR_LIMIT, sizeof(*part1));
	CHECK(part1 != NULL);
	file = open_ucd(directory, "NormalizationTest.txt");
	while (read_ucd_line(file, line)) {
		comment = strchr(line, '#');
		if (comment != NULL)
			*comment = '\0';
		text = trim(line);
		if (*text == '\0')
			continue;
		if (*text == '@') {
			case_check(strlen(text) == 6 &&
			    strncmp(text, "@Part", 5) == 0 &&
			    text[5] >= '0' && text[5] <= '5',
			    "normalization part marker");
			part = (unsigned)(text[5] - '0');
			CHECK(!seen_part[part]);
			seen_part[part] = true;
			continue;
		}
		CHECK(seen_part[part]);
		field_count = split_fields(text, fields, COUNT(fields));
		case_check(field_count == 6 && fields[5][0] == '\0',
		    "normalization five columns field_count=%zu", field_count);
		skip = false;
		for (size_t i = 0; i < COUNT(columns); i++) {
			columns[i] = parse_points(fields[i]);
			if (forbidden_name_point(&columns[i]))
				skip = true;
		}
		if (part == 1) {
			CHECK(columns[0].count == 1);
			CHECK(!part1[columns[0].points[0]]);
			part1[columns[0].points[0]] = true;
		}
		if (skip) {
			printf("UCD normalization line %zu (U+%04X) skipped: "
			    "a column contains NUL or slash\n", ucd_line,
			    (unsigned)columns[0].points[0]);
			skipped++;
			continue;
		}
		for (size_t i = 0; i < COUNT(columns); i++) {
			sequence_t input = columns[i];
			sequence_t output = columns[i < 3 ? 2 : 4];

			/* Bare dot names are forbidden. An ASCII suffix makes
			 * these columns valid without changing normalization.
			 */
			if (input.count <= 2 && input.points[0] == '.' &&
			    (input.count == 1 || input.points[1] == '.')) {
				CHECK(output.count < COUNT(output.points));
				input.points[input.count++] = 'a';
				output.points[output.count++] = 'a';
				dot_columns++;
			}
			check_form(APFS_NAMES_NFD, input.points, input.count,
			    output.points, output.count, "UCD normalization column");
		}
		rows++;
	}
	CHECK(fclose(file) == 0);
	CHECK(rows > 0 && seen_part[1]);
	printf("UCD %s normalization: %zu rows, %zu skipped (NUL or slash)\n",
	    APFS_UNICODE_VERSION, rows, skipped);
	printf("UCD normalization: %zu dot-name columns checked with suffix\n",
	    dot_columns);
	ucd_line = 0;
	for (uint32_t point = 1; point < SCALAR_LIMIT; point++) {
		if (name_scalar(point) && !part1[point])
			check_form(APFS_NAMES_NFD, &point, 1, &point, 1,
			    "scalar absent from normalization Part 1 is NFD");
	}
	free(part1);
	ucd_file = NULL;
}

static folding_t *
read_foldings(const char *directory)
{
	sequence_t source, mapping;
	folding_t *foldings, *entry;
	char line[UCD_LINE_BYTES], *fields[4], *text, *comment;
	FILE *file;
	size_t count, rows = 0;

	foldings = calloc(SCALAR_LIMIT, sizeof(*foldings));
	CHECK(foldings != NULL);
	file = open_ucd(directory, "CaseFolding.txt");
	while (read_ucd_line(file, line)) {
		comment = strchr(line, '#');
		if (comment != NULL)
			*comment = '\0';
		text = trim(line);
		if (*text == '\0')
			continue;
		count = split_fields(text, fields, COUNT(fields));
		case_check(count == 4 && fields[3][0] == '\0',
		    "case folding three fields count=%zu", count);
		source = parse_points(fields[0]);
		mapping = parse_points(fields[2]);
		CHECK(source.count == 1);
		case_check(strlen(fields[1]) == 1 &&
		    strchr("CFST", fields[1][0]) != NULL,
		    "case folding status point=U+%04X",
		    (unsigned)source.points[0]);
		if (fields[1][0] == 'S' || fields[1][0] == 'T')
			continue;
		entry = &foldings[source.points[0]];
		CHECK(entry->count == 0);
		CHECK(mapping.count <= COUNT(entry->points));
		entry->count = mapping.count;
		bcopy(mapping.points, entry->points,
		    mapping.count * sizeof(*entry->points));
		rows++;
	}
	CHECK(fclose(file) == 0);
	CHECK(rows > 0);
	printf("UCD case folding: %zu C/F mappings\n", rows);
	ucd_file = NULL;
	ucd_line = 0;
	return (foldings);
}

/* The independent C/F map folds NFD(c), then the public NFD operation
 * supplies the final normalization. No production folding table is read.
 */
static void
test_ucd_caseless_matches_full_folding(const char *directory)
{
	folding_t *foldings;
	sequence_t folded;
	uint32_t point;

	foldings = read_foldings(directory);
	for (point = 1; point < SCALAR_LIMIT; point++) {
		if (!name_scalar(point))
			continue;
		make_form(APFS_NAMES_NFD, &point, 1, &decomposed);
		folded.count = 0;
		for (size_t i = 0; i < decomposed.count; i++) {
			uint32_t unit = decomposed.points[i];
			const folding_t *entry = &foldings[unit];
			size_t count = entry->count == 0 ? 1 : entry->count;
			const uint32_t *points = entry->count == 0 ?
			    &unit : entry->points;

			CHECK(count <= COUNT(folded.points) - folded.count);
			bcopy(points, folded.points + folded.count,
			    count * sizeof(*points));
			folded.count += count;
		}
		make_form(APFS_NAMES_NFD, folded.points, folded.count, &expected);
		make_form(APFS_NAMES_CASELESS, &point, 1, &actual);
		case_check(actual.count == expected.count &&
		    bcmp(actual.points, expected.points,
		    expected.count * sizeof(*expected.points)) == 0,
		    "UCD NFD(fold(NFD(c))) point=U+%04X count=%zu",
		    (unsigned)point, actual.count);
	}
	free(foldings);
}

static void
test_ucd_conformance(void)
{
	const char *directory;

	directory = getenv("FSIMG_APFS_UCD");
	if (directory == NULL) {
		puts("UCD conformance skipped: FSIMG_APFS_UCD is unset");
		return;
	}
	check_digest(directory, "UnicodeData.txt", APFS_UCD_UNICODEDATA_SHA256);
	check_digest(directory, "CaseFolding.txt", APFS_UCD_CASEFOLDING_SHA256);
	check_digest(directory, "NormalizationTest.txt",
	    "5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db");
	test_ucd_normalization_matches_columns(directory);
	test_ucd_caseless_matches_full_folding(directory);
	ucd_file = NULL;
	ucd_line = 0;
}

/* Check both the scalar and its occurrence between assigned ASCII points. */
static void
check_assigned_point(uint32_t point, bool wanted)
{
	uint32_t points[] = {'a', point, 'z'};
	char name[6];
	size_t length;

	length = encode_name(&point, 1, name, sizeof(name));
	case_check(apfs_name_assigned(name, length) == wanted,
	    "assignment alone point=U+%04X expected=%d", (unsigned)point,
	    wanted);
	length = encode_name(points, COUNT(points), name, sizeof(name));
	case_check(apfs_name_assigned(name, length) == wanted,
	    "assignment embedded point=U+%04X expected=%d", (unsigned)point,
	    wanted);
}

static void
test_unassigned_and_noncharacter_points_are_refused(void)
{
	CHECK(!apfs_name_assigned("\xcd\xb8", 2));
	CHECK(!apfs_name_assigned("\xef\xbf\xbf", 3));
	CHECK(!apfs_name_assigned("a\xcd\xb8", 3));
}

static void
test_ascii_private_use_and_empty_names_are_assigned(void)
{
	CHECK(apfs_name_assigned("plain ASCII", 11));
	/* Assignment is vacuous for empty names; callers reject them elsewhere. */
	CHECK(apfs_name_assigned("", 0));
	check_assigned_point(0xe000, true);
	check_assigned_point(0xf0000, true);
	check_assigned_point(0x10fffd, true);
}

static void
test_assigned_range_interiors_and_endpoints_are_accepted(void)
{
	/* First, interior, Last for five UnicodeData.txt 17.0.0 ranges. */
	static const uint32_t points[] = {
		0x3400, 0x3500, 0x4dbf,
		0x4e00, 0x4e01, 0x9fff,
		0xac00, 0xac01, 0xd7a3,
		0x17000, 0x17001, 0x187ff,
		0x20000, 0x20001, 0x2a6df
	};

	for (size_t i = 0; i < COUNT(points); i++)
		check_assigned_point(points[i], true);
}

static void
test_last_assigned_point_accepted_and_next_refused(void)
{
	/* Adjacent assigned/unassigned pairs verified in UnicodeData.txt 17.0.0. */
	static const uint32_t last_assigned[] = {
		0x377, 0x85b, 0xd7a3, 0x1faf8, 0x2a6df
	};

	for (size_t i = 0; i < COUNT(last_assigned); i++) {
		check_assigned_point(last_assigned[i], true);
		check_assigned_point(last_assigned[i] + 1, false);
	}
}

static void
test_every_noncharacter_is_refused_alone_and_embedded(void)
{
	for (uint32_t point = 0xfdd0; point <= 0xfdef; point++)
		check_assigned_point(point, false);
	for (uint32_t plane = 0; plane <= 16; plane++) {
		check_assigned_point((plane << 16) | 0xfffe, false);
		check_assigned_point((plane << 16) | 0xffff, false);
	}
}

static void
test_assignment_refuses_non_strict_utf8(void)
{
	static const invalid_name_t invalid[] = {
		{ "lone continuation", "\x80", 1 },
		{ "truncated sequence", "\xe2\x82", 2 },
		{ "encoded surrogate", "\xed\xa0\x80", 3 },
		{ "overlong two", "\xc0\x80", 2 },
		{ "overlong three", "\xe0\x80\x80", 3 },
		{ "above scalar range", "\xf4\x90\x80\x80", 4 }
	};

	for (size_t i = 0; i < COUNT(invalid); i++)
		case_check(!apfs_name_assigned(invalid[i].bytes, invalid[i].length),
		    "assignment rejects %s", invalid[i].label);
}

/* Build a flat oracle from UCD rows, independent of the generated page map.
 * First/Last pairs include every point between their endpoints. Surrogate
 * rows remain in the map but are excluded from the scalar traversal below.
 */
static bool *
read_ucd_assignment(const char *directory)
{
	char line[UCD_LINE_BYTES], range_name[UCD_LINE_BYTES];
	char *fields[15], *end, *first_suffix, *last_suffix;
	bool *assigned, in_range = false;
	FILE *file;
	unsigned long point;
	uint32_t range_start = 0, previous = 0;
	size_t rows = 0;

	check_digest(directory, "UnicodeData.txt", APFS_UCD_UNICODEDATA_SHA256);
	assigned = calloc(SCALAR_LIMIT, sizeof(*assigned));
	CHECK(assigned != NULL);
	file = open_ucd(directory, "UnicodeData.txt");
	while (read_ucd_line(file, line)) {
		CHECK(split_fields(line, fields, COUNT(fields)) == COUNT(fields));
		CHECK(isxdigit((unsigned char)fields[0][0]));
		errno = 0;
		point = strtoul(fields[0], &end, 16);
		case_check(errno == 0 && *end == '\0' && point < SCALAR_LIMIT &&
		    (rows == 0 || point > previous), "ordered UCD assignment point");
		previous = (uint32_t)point;
		rows++;
		first_suffix = strstr(fields[1], ", First>");
		last_suffix = strstr(fields[1], ", Last>");
		if (first_suffix != NULL) {
			CHECK(!in_range && strcmp(first_suffix, ", First>") == 0);
			*first_suffix = '\0';
			CHECK(fields[1][0] == '<');
			bcopy(fields[1], range_name, strlen(fields[1]) + 1);
			range_start = (uint32_t)point;
			in_range = true;
		} else if (last_suffix != NULL) {
			CHECK(in_range && strcmp(last_suffix, ", Last>") == 0);
			*last_suffix = '\0';
			CHECK(strcmp(fields[1], range_name) == 0);
			for (uint32_t member = range_start; member <= point; member++)
				assigned[member] = true;
			in_range = false;
		} else {
			CHECK(!in_range);
			assigned[point] = true;
		}
	}
	CHECK(!in_range && rows > 0);
	CHECK(fclose(file) == 0);
	ucd_file = NULL;
	ucd_line = 0;
	return (assigned);
}

/* SHA-256 of one byte (0 or 1) per scalar in increasing order, including
 * U+0000 and excluding U+D800..U+DFFF. Reproduce with:
 * FSIMG_APFS_UCD=$HOME/.local/share/fsimg-oracles/ucd/17.0.0 \
 *     make check TESTS=test-apfs-unicode
 * The "Scalar assignment: ... SHA-256 <hex>" line in test-apfs-unicode.log
 * gives the value after the UCD comparison has confirmed every scalar.
 */
static void
test_every_scalar_assignment_matches_ucd_and_digest(void)
{
	static const char wanted[] =
	    "f4d679f0ca488249e63ff5666bd1119f7ee19c723ec838b19736490e53349192";
	char name[4], hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
	fsimg_sha256_t digest;
	const char *directory;
	bool *assigned = NULL;
	unsigned char answer, expected_answer;
	size_t length, accepted = 0;

	directory = getenv("FSIMG_APFS_UCD");
	if (directory != NULL)
		assigned = read_ucd_assignment(directory);
	else
		puts("Assignment UCD comparison skipped: FSIMG_APFS_UCD is unset");
	fsimg_sha256_init(&digest);
	for (uint32_t point = 0; point < SCALAR_LIMIT; point++) {
		if (point >= 0xd800 && point <= 0xdfff)
			continue;
		length = encode_name(&point, 1, name, sizeof(name));
		answer = apfs_name_assigned(name, length) ? 1 : 0;
		fsimg_sha256_update(&digest, &answer, 1);
		accepted += answer;
		if (assigned == NULL)
			continue;
		expected_answer = assigned[point] ? 1 : 0;
		case_check(answer == expected_answer,
		    "UCD assignment point=U+%04X expected=%u actual=%u",
		    (unsigned)point, (unsigned)expected_answer, (unsigned)answer);
	}
	sha256_final_hex(&digest, hex);
	printf("Scalar assignment: %zu accepted, SHA-256 %s\n", accepted, hex);
	CHECK(accepted == ASSIGNED_SCALARS);
	case_check(strcmp(hex, wanted) == 0, "scalar assignment SHA-256");
	free(assigned);
}

static void
test_assignment_matches_native_outcomes_when_native_is_set(void)
{
	static const char *files[] = {
		"results-ci.tsv", "results-cs.tsv",
		"results-cn-ci.tsv", "results-cn-cs.tsv",
		"results-u17m-ci.tsv", "results-u17m-cs.tsv",
		"results-u18-ci.tsv", "results-u18-cs.tsv"
	};
	char path[4096], line[E4_LINE_BYTES], name[APFS_NAME_MAX_BYTES];
	char *outcome, pair[3];
	const char *directory;
	FILE *file;
	size_t rows, length, hex_length;
	int path_length;
	bool wanted;

	directory = getenv("FSIMG_APFS_NATIVE");
	if (directory == NULL) {
		puts("E4 assignment outcomes skipped: FSIMG_APFS_NATIVE is unset");
		return;
	}
	/* When FSIMG_APFS_NATIVE is set, every listed file must exist. */
	for (size_t i = 0; i < COUNT(files); i++) {
		path_length = snprintf(path, sizeof(path), "%s/e4-inputs/%s",
		    directory, files[i]);
		CHECK(path_length >= 0 && (size_t)path_length < sizeof(path));
		file = fopen(path, "rb");
		if (file == NULL)
			perror(path);
		CHECK(file != NULL);
		rows = 0;
		while (read_e4_line(file, line, path, rows + 1)) {
			outcome = strchr(line, '\t');
			case_check(outcome != NULL,
			    "E4 outcome missing file=%s line=%zu", path, rows + 1);
			*outcome++ = '\0';
			case_check(strcmp(outcome, "ok") == 0 ||
			    strcmp(outcome, "EEXIST") == 0 ||
			    strcmp(outcome, "EILSEQ") == 0,
			    "E4 outcome file=%s hex=%s value=%s", path, line, outcome);
			hex_length = strlen(line);
			case_check(hex_length > 0 && hex_length % 2 == 0 &&
			    hex_length / 2 <= sizeof(name),
			    "E4 outcome hex length file=%s hex=%s", path, line);
			length = hex_length / 2;
			for (size_t j = 0; j < length; j++) {
				pair[0] = line[2 * j];
				pair[1] = line[2 * j + 1];
				pair[2] = '\0';
				case_check(isxdigit((unsigned char)pair[0]) &&
				    isxdigit((unsigned char)pair[1]),
				    "E4 outcome hex syntax file=%s hex=%s", path, line);
				name[j] = (char)strtoul(pair, NULL, 16);
			}
			wanted = strcmp(outcome, "EILSEQ") != 0;
			case_check(apfs_name_assigned(name, length) == wanted,
			    "E4 assignment file=%s hex=%s outcome=%s expected=%d",
			    path, line, outcome, wanted);
			rows++;
		}
		CHECK(fclose(file) == 0);
		case_check(rows > 0, "E4 outcomes nonempty file=%s", path);
		printf("E4 assignment: %zu rows checked file=%s\n", rows, path);
	}
}

int
main(void)
{
	CHECK(strcmp(APFS_PROFILE, "apfs-unicode-" APFS_UNICODE_VERSION) == 0);
	test_native_hashes_match_stored_records();
	test_e4_fixture_hashes_match();
	test_fresh_volume_names_hash_natively();
	test_e4_full_vectors_match_when_present();
	test_hash_matches_across_chunk_boundaries();
	test_namespace_keys_follow_volume_rule();
	test_invalid_names_leave_empty_forms();
	test_expansion_reaches_key_bound_exactly();
	test_short_key_capacity_stores_nothing();
	test_four_byte_decomposition_stays_in_bound();
	test_long_combining_run_reorders_stably();
	test_every_scalar_respects_key_bound_and_matches_digest();
	test_ucd_conformance();
	test_unassigned_and_noncharacter_points_are_refused();
	test_ascii_private_use_and_empty_names_are_assigned();
	test_assigned_range_interiors_and_endpoints_are_accepted();
	test_last_assigned_point_accepted_and_next_refused();
	test_every_noncharacter_is_refused_alone_and_embedded();
	test_assignment_refuses_non_strict_utf8();
	test_every_scalar_assignment_matches_ucd_and_digest();
	test_assignment_matches_native_outcomes_when_native_is_set();
	return (0);
}
