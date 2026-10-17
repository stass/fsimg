/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "test.h"
#include "unicode.h"

static void test_scalars(void);
static void test_invalid(void);
static void test_paths_and_metadata(void);

static void
test_scalars(void)
{
	uint16_t input[2], decoded[2];
	char bytes[6];
	size_t units, written, count;

	/* Enumerate the scalar domain independently in UTF-16, including both
	 * sides of every UTF-8 width boundary and the surrogate exclusion.
	 */
	for (uint32_t scalar = 0; scalar <= 0x10ffff; scalar++) {
		if (scalar >= 0xd800 && scalar <= 0xdfff)
			continue;
		units = scalar < 0x10000 ? 1 : 2;
		if (units == 1) {
			input[0] = (uint16_t)scalar;
		} else {
			input[0] = (uint16_t)(0xd800 + (scalar - 0x10000) / 1024);
			input[1] = (uint16_t)(0xdc00 + (scalar - 0x10000) % 1024);
		}
		memset(bytes, 0x7e, sizeof(bytes));
		CHECK(fsimg_utf16_to_utf8(input, units, bytes, 4, &written, NULL) == FSIMG_OK);
		CHECK(written == (scalar < 0x80 ? 1u : scalar < 0x800 ? 2u :
		    scalar < 0x10000 ? 3u : 4u));
		CHECK(bytes[4] == 0x7e && bytes[5] == 0x7e);
		CHECK(fsimg_utf8_to_utf16(bytes, written, decoded, 2, &count, NULL) == FSIMG_OK);
		CHECK(count == units && bcmp(input, decoded, count * sizeof(*input)) == 0);
		CHECK(fsimg_utf8_to_utf16(bytes, written, NULL, 0, &count, NULL) == FSIMG_OK);
		CHECK(count == units);
	}
	CHECK(fsimg_utf16_to_utf8((const uint16_t[]){ 0xd83d, 0xde00 }, 2,
	    bytes, sizeof(bytes), &written, NULL) == FSIMG_OK);
	CHECK(written == 4 && bcmp(bytes, "\xf0\x9f\x98\x80", 4) == 0);
}

static void
test_invalid(void)
{
	const char *invalid[] = { "\x80", "\xc0\x80", "\xc1\xbf", "\xc2",
	    "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80",
	    "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff",
	    "\xe2\x28\xa1", "\xf0\x90\x80" };
	const uint16_t bad[][2] = { { 0xd800, 0 }, { 0xdc00, 0 },
	    { 0xd800, 0xd800 }, { 0xdbff, 0xffff } };
	uint16_t units[4];
	char bytes[4];
	size_t count;

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		CHECK(fsimg_utf8_to_utf16(invalid[i], strlen(invalid[i]), units,
		    4, &count, NULL) == FSIMG_EINVAL);
		CHECK(count == 0);
	}
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(fsimg_utf16_to_utf8(bad[i], 2, bytes, sizeof(bytes),
		    &count, NULL) == FSIMG_EINVAL);
		CHECK(count == 0);
	}
	CHECK(fsimg_utf8_to_utf16("\xf0\x9f\x98\x80", 4, units, 1,
	    &count, NULL) == FSIMG_ERANGE);
	CHECK(count == 0);
	CHECK(fsimg_utf16_to_utf8((const uint16_t[]){ 0xd83d, 0xde00 },
	    2, bytes, 3, &count, NULL) == FSIMG_ERANGE);
	CHECK(count == 0);
}

static void
test_paths_and_metadata(void)
{
	const char *invalid[] = { "", "a", "/", "//a", "/a/", "/a//b",
	    "/./a", "/a/..", "/\xff" };
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_metadata_t metadata = { 0 };
	fsimg_error_t error;
	size_t length;
	uint32_t depth;

	fsimg_options_init(&options);
	options.max_depth = 2;
	options.max_path_bytes = 16;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK(fsimg_path_validate(ctx, invalid[i], false, &length,
		    &depth, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_path_validate(ctx, "/a/b", false, &length, &depth, NULL) == FSIMG_OK);
	CHECK(length == 4 && depth == 2);
	CHECK(fsimg_path_validate(ctx, "/", true, &length, &depth, NULL) == FSIMG_OK);
	CHECK(length == 1 && depth == 0);
	CHECK(fsimg_path_validate(ctx, "/a/b/c", false, &length, &depth, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DEPTH);
	CHECK(fsimg_path_validate(ctx, "/123456789012345", false, &length,
	    &depth, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_PATH);
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_OK);
	metadata.required = FSIMG_META_CREATED;
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_EINVAL);
	metadata.valid = FSIMG_META_CREATED;
	metadata.created = (fsimg_timestamp_t){ .seconds = INT64_MIN,
	    .precision_ns = 1, .basis = FSIMG_TIME_UTC };
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_OK);
	metadata.created.nanoseconds = 1000000000;
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_EINVAL);
	metadata.created.nanoseconds = 0;
	metadata.created.basis = FSIMG_TIME_OFFSET;
	metadata.created.offset_minutes = -1440;
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_EINVAL);
	metadata.created.offset_minutes = 60;
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_OK);
	metadata.created.basis = FSIMG_TIME_WALL;
	CHECK(fsimg_metadata_validate(&metadata, NULL) == FSIMG_EINVAL);
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_scalars();
	test_invalid();
	test_paths_and_metadata();
	return (0);
}
