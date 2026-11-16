/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <ctype.h>
#include <glob.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apfs.h"
#include "apfs-gen.h"
#include "fsimg-endian.h"
#include "reader.h"
#include "test.h"

#define BLOCK 4096u
#define SMALL_BLOCKS (2 * 32768ull + 17)
#define LARGE_BLOCKS 2147483648ull
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static unsigned failures;
static const char *scenario;

static void expect(bool, const char *);
static bool status_is(fsimg_status_t, fsimg_status_t,
    const fsimg_error_t *, const char *);
static void geometry_equal(const apfs_nx_t *, const apfs_spaceman_t *,
    const apfs_nx_t *, const apfs_spaceman_t *);
static void fixed_regions_match(const apfs_nx_t *, const apfs_spaceman_t *);
static void test_native_geometry_matches_embedded_captures(void);
static void test_json_integer_extractor_is_bounded(void);
static void test_native_dumps_match_geometry_and_regions(void);
static void test_spaceman_rejects_inconsistent_geometry_and_arrays(void);
static void test_older_spaceman_accepts_supported_block_sizes(void);
static void test_older_spaceman_rejects_invalid_arrays(void);
static void test_older_xid_high_half_selects_current_structure(void);
static void test_current_spaceman_distinguishes_invalid_and_unknown_sizes(void);
static void test_wrong_cab_count_depends_on_proven_block_size(void);
static void test_array_end_determines_implied_size(void);
static void test_checkpoint_keeps_verified_spaceman(void);
static void test_mapped_size_mismatch_rejected_before_extra_block_read(void);
static void test_pager_walks_small_and_large_sparse_containers(void);
static void
    test_checkpoint_rejects_hostile_first_block_before_object_read(void);
static void test_checkpoint_rejects_fusion(void);
static void test_pager_caches_second_spaceman_block(void);
static void test_pager_rejects_damaged_chunk_structures(void);
static void test_bitmap_free_count_mismatch_fails_when_bitmap_read(void);
static void test_device_free_count_mismatch_fails_at_end(void);
static void test_pool_bitmap_rejects_invalid_slot_and_future_xid(void);
static void test_chunk_bitmap_outside_pool_is_passed_through(void);

static void
expect(bool condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", scenario, what);
		failures++;
	}
}

static bool
status_is(fsimg_status_t status, fsimg_status_t wanted,
    const fsimg_error_t *error, const char *role)
{
	if (status != wanted) {
		fprintf(stderr, "%s: expected %d, got %d (%s)\n", scenario,
		    wanted, status, error->message);
		failures++;
		return (false);
	}
	if (wanted != FSIMG_OK && strstr(error->message, role) == NULL) {
		fprintf(stderr,
		    "%s: expected message containing '%s', got '%s'\n",
		    scenario, role, error->message);
		failures++;
		return (false);
	}
	return (true);
}

/* Only the formatter-derived fields are compared, not live allocator state. */
static void
geometry_equal(const apfs_nx_t *nx, const apfs_spaceman_t *sm,
    const apfs_nx_t *expected_nx, const apfs_spaceman_t *expected_sm)
{
#define NX_EQ(field) expect(nx->field == expected_nx->field, #field)
#define SM_EQ(field) expect(sm->field == expected_sm->field, #field)
	NX_EQ(block_size);
	NX_EQ(block_count);
	NX_EQ(desc_base);
	NX_EQ(desc_blocks);
	NX_EQ(data_base);
	NX_EQ(data_blocks);
	NX_EQ(max_volumes);
	for (unsigned i = 0; i < 4; i++)
		NX_EQ(ephemeral_info[i]);
	SM_EQ(block_size);
	SM_EQ(blocks_per_chunk);
	SM_EQ(chunks_per_cib);
	SM_EQ(cibs_per_cab);
	SM_EQ(devices[0].block_count);
	SM_EQ(devices[0].chunk_count);
	SM_EQ(devices[0].cib_count);
	SM_EQ(devices[0].cab_count);
	SM_EQ(devices[0].addr_offset);
	SM_EQ(devices[1].addr_offset);
	SM_EQ(ip_block_count);
	SM_EQ(ip_bm_size_in_blocks);
	SM_EQ(ip_bm_block_count);
	SM_EQ(ip_bm_tx_multiplier);
	SM_EQ(ip_bm_base);
	SM_EQ(ip_base);
	SM_EQ(ip_bm_xid_offset);
	SM_EQ(ip_bitmap_offset);
	SM_EQ(ip_bm_free_next_offset);
	SM_EQ(free_queues[0].node_limit);
	SM_EQ(free_queues[1].node_limit);
#undef NX_EQ
#undef SM_EQ
}

static void
fixed_regions_match(const apfs_nx_t *nx, const apfs_spaceman_t *sm)
{
	const apfs_region_t expected[] = {
		{ .first = 0, .count = 1, .kind = APFS_REGION_BLOCK0 },
		{ .first = nx->desc_base, .count = nx->desc_blocks,
		    .kind = APFS_REGION_DESCRIPTORS },
		{ .first = nx->data_base, .count = nx->data_blocks,
		    .kind = APFS_REGION_DATA },
		{ .first = sm->ip_bm_base, .count = sm->ip_bm_block_count,
		    .kind = APFS_REGION_POOL_RING },
		{ .first = sm->ip_base, .count = sm->ip_block_count,
		    .kind = APFS_REGION_POOL }
	};
	apfs_region_t regions[APFS_FIXED_REGIONS];
	fsimg_error_t error = { 0 };

	if (!status_is(apfs_fixed_regions(nx, sm, regions, &error), FSIMG_OK,
	    &error, "region"))
		return;
	for (unsigned i = 0; i < APFS_FIXED_REGIONS; i++) {
		expect(regions[i].first == expected[i].first &&
		    regions[i].count == expected[i].count &&
		    regions[i].kind == expected[i].kind,
		    "fixed region range and role");
		expect(strlen(apfs_region_role(regions[i].kind)) != 0,
		    "region has a diagnostic role");
		if (i != 0)
			expect(regions[i - 1].first + regions[i - 1].count <=
			    regions[i].first, "regions ascend without overlap");
	}
}

/* Embedded observations, not values calculated by the geometry under test. */
typedef struct native_case {
	const char *path;
	apfs_nx_t nx;
	apfs_spaceman_t sm;
	uint64_t mapped_bytes;
} native_case_t;

static const native_case_t native_cases[] = {
	/* $FSIMG_APFS_NATIVE/e1-sweep/n92/apfsdump.json */
	{ .path = "e1-sweep/n92",
	  .nx = { .block_size = 4096,
	    .block_count = 92ull,
	    .desc_base = 1ull,
	    .desc_blocks = 8ull,
	    .data_base = 9ull,
	    .data_blocks = 52ull,
	    .max_volumes = 1ull,
	    .ephemeral_info = { 4295229441ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 6ull,
	    .ip_bm_size_in_blocks = 1ull,
	    .ip_bm_block_count = 16ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 61ull,
	    .ip_base = 77ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2528ull,
	    .ip_bm_free_next_offset = 2536ull,
	    .devices = { { .block_count = 92ull,
	      .chunk_count = 1ull,
	      .cib_count = 1ull,
	      .cab_count = 0ull,
	      .addr_offset = 2568ull,
	    }, { .addr_offset = 2576 } },
	    .free_queues = { { .node_limit = 1 },
	      { .node_limit = 1 } } },
	  .mapped_bytes = 4096 },
	/* $FSIMG_APFS_NATIVE/apfs-newfs-372736-plain/apfsdump.json */
	{ .path = "apfs-newfs-372736-plain",
	  .nx = { .block_size = 4096,
	    .block_count = 91ull,
	    .desc_base = 1ull,
	    .desc_blocks = 8ull,
	    .data_base = 9ull,
	    .data_blocks = 52ull,
	    .max_volumes = 1ull,
	    .ephemeral_info = { 4295229441ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 6ull,
	    .ip_bm_size_in_blocks = 1ull,
	    .ip_bm_block_count = 16ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 61ull,
	    .ip_base = 77ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2528ull,
	    .ip_bm_free_next_offset = 2536ull,
	    .devices = { { .block_count = 91ull,
	      .chunk_count = 1ull,
	      .cib_count = 1ull,
	      .cab_count = 0ull,
	      .addr_offset = 2568ull,
	    }, { .addr_offset = 2576 } },
	    .free_queues = { { .node_limit = 1 },
	      { .node_limit = 1 } } },
	  .mapped_bytes = 4096 },
	/* $FSIMG_APFS_NATIVE/apfs-newfs-4194304-plain/apfsdump.json */
	{ .path = "apfs-newfs-4194304-plain",
	  .nx = { .block_size = 4096,
	    .block_count = 1024ull,
	    .desc_base = 1ull,
	    .desc_blocks = 8ull,
	    .data_base = 9ull,
	    .data_blocks = 52ull,
	    .max_volumes = 1ull,
	    .ephemeral_info = { 4295229441ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 6ull,
	    .ip_bm_size_in_blocks = 1ull,
	    .ip_bm_block_count = 16ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 61ull,
	    .ip_base = 77ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2528ull,
	    .ip_bm_free_next_offset = 2536ull,
	    .devices = { { .block_count = 1024ull,
	      .chunk_count = 1ull,
	      .cib_count = 1ull,
	      .cab_count = 0ull,
	      .addr_offset = 2568ull,
	    }, { .addr_offset = 2576 } },
	    .free_queues = { { .node_limit = 1 },
	      { .node_limit = 1 } } },
	  .mapped_bytes = 4096 },
	/* $FSIMG_APFS_NATIVE/apfs-newfs-1073741824-plain/apfsdump.json */
	{ .path = "apfs-newfs-1073741824-plain",
	  .nx = { .block_size = 4096,
	    .block_count = 262144ull,
	    .desc_base = 1ull,
	    .desc_blocks = 16ull,
	    .data_base = 17ull,
	    .data_blocks = 992ull,
	    .max_volumes = 2ull,
	    .ephemeral_info = { 34360000513ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 27ull,
	    .ip_bm_size_in_blocks = 1ull,
	    .ip_bm_block_count = 16ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 1009ull,
	    .ip_base = 1025ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2528ull,
	    .ip_bm_free_next_offset = 2536ull,
	    .devices = { { .block_count = 262144ull,
	      .chunk_count = 8ull,
	      .cib_count = 1ull,
	      .cab_count = 0ull,
	      .addr_offset = 2568ull,
	    }, { .addr_offset = 2576 } },
	    .free_queues = { { .node_limit = 1 },
	      { .node_limit = 116 } } },
	  .mapped_bytes = 4096 },
	/* $FSIMG_APFS_NATIVE/apfs-newfs-8589934592-plain/apfsdump.json */
	{ .path = "apfs-newfs-8589934592-plain",
	  .nx = { .block_size = 4096,
	    .block_count = 2097152ull,
	    .desc_base = 1ull,
	    .desc_blocks = 68ull,
	    .data_base = 69ull,
	    .data_blocks = 6160ull,
	    .max_volumes = 16ull,
	    .ephemeral_info = { 34360000513ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 195ull,
	    .ip_bm_size_in_blocks = 1ull,
	    .ip_bm_block_count = 16ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 6229ull,
	    .ip_base = 6245ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2528ull,
	    .ip_bm_free_next_offset = 2536ull,
	    .devices = { { .block_count = 2097152ull,
	      .chunk_count = 64ull,
	      .cib_count = 1ull,
	      .cab_count = 0ull,
	      .addr_offset = 2568ull,
	    }, { .addr_offset = 2576 } },
	    .free_queues = { { .node_limit = 1 },
	      { .node_limit = 512 } } },
	  .mapped_bytes = 4096 },
	/* $FSIMG_APFS_NATIVE/e12-sweep/n2147483648/apfsdump.json */
	{ .path = "e12-sweep/n2147483648",
	  .nx = { .block_size = 4096,
	    .block_count = 2147483648ull,
	    .desc_base = 1ull,
	    .desc_blocks = 288ull,
	    .data_base = 289ull,
	    .data_blocks = 28360ull,
	    .max_volumes = 100ull,
	    .ephemeral_info = { 34360000513ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 198177ull,
	    .ip_bm_size_in_blocks = 7ull,
	    .ip_bm_block_count = 112ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 28649ull,
	    .ip_base = 28761ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 2576ull,
	    .ip_bm_free_next_offset = 2592ull,
	    .devices = { { .block_count = 2147483648ull,
	      .chunk_count = 65536ull,
	      .cib_count = 521ull,
	      .cab_count = 2ull,
	      .addr_offset = 2816ull,
	    }, { .addr_offset = 2832 } },
	    .free_queues = { { .node_limit = 175 },
	      { .node_limit = 512 } } },
	  .mapped_bytes = 4096 },
	/*
	 * $FSIMG_APFS_NATIVE/apfs-newfs-140737488355328-e12-128t/apfsdump.json
	 */
	{ .path = "apfs-newfs-140737488355328-e12-128t",
	  .nx = { .block_size = 4096,
	    .block_count = 34359738368ull,
	    .desc_base = 1ull,
	    .desc_blocks = 300ull,
	    .data_base = 301ull,
	    .data_blocks = 29716ull,
	    .max_volumes = 100ull,
	    .ephemeral_info = { 34360000513ull } },
	  .sm = {
	    .block_size = 4096ull,
	    .blocks_per_chunk = 32768ull,
	    .chunks_per_cib = 126ull,
	    .cibs_per_cab = 507ull,
	    .ip_block_count = 3170748ull,
	    .ip_bm_size_in_blocks = 97ull,
	    .ip_bm_block_count = 1552ull,
	    .ip_bm_tx_multiplier = 16ull,
	    .ip_bm_base = 30017ull,
	    .ip_base = 31569ull,
	    .ip_bm_xid_offset = 2520ull,
	    .ip_bitmap_offset = 3296ull,
	    .ip_bm_free_next_offset = 3496ull,
	    .devices = { { .block_count = 34359738368ull,
	      .chunk_count = 1048576ull,
	      .cib_count = 8323ull,
	      .cab_count = 17ull,
	      .addr_offset = 6600ull,
	    }, { .addr_offset = 6736 } },
	    .free_queues = { { .node_limit = 512 },
	      { .node_limit = 512 } } },
	  .mapped_bytes = 8192 },
};

static void
test_native_geometry_matches_embedded_captures(void)
{
	apfs_nx_t nx;
	apfs_spaceman_t sm;
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	for (size_t i = 0; i < ARRAY_COUNT(native_cases); i++) {
		scenario = native_cases[i].path;
		CHECK(apfs_native_geometry(native_cases[i].nx.block_count, &nx,
		    &sm));
		geometry_equal(&nx, &sm, &native_cases[i].nx,
		    &native_cases[i].sm);
		if (status_is(apfs_spaceman_check(&sm, nx.block_size,
		    nx.block_count, &blocks, &error), FSIMG_OK, &error,
		    "space manager"))
			expect(blocks * nx.block_size ==
			    native_cases[i].mapped_bytes,
			    "implied size equals captured mapping size");
		fixed_regions_match(&nx, &sm);
	}
	scenario = "native geometry rejects block counts outside writer range";
	expect(!apfs_native_geometry(0, &nx, &sm), "zero blocks rejected");
	expect(!apfs_native_geometry(APFS_WRITER_BLOCKS_MAX + 1, &nx, &sm),
	    "above writer maximum rejected");
}

/*
 * A bounded JSON field extractor. Spans borrow a read-only mapping, never
 * terminate strings in place, and skip nested values without allocating a
 * token tree. Only requested integer fields are decoded. Missing fields,
 * excessive nesting, malformed delimiters and integer overflow fail the test.
 */
typedef struct json_span {
	const char *first;
	const char *end;
} json_span_t;

static const char *json_space(const char *, const char *);
static const char *json_string_end(const char *, const char *);
static const char *json_value_end(const char *, const char *);
static json_span_t json_member(json_span_t, const char *);
static json_span_t json_item(json_span_t, unsigned);
static bool json_integer(json_span_t, uint64_t, uint64_t *);
static bool native_fields(json_span_t, apfs_nx_t *, apfs_spaceman_t *,
    uint64_t *);

static const char *
json_space(const char *p, const char *end)
{
	while (p != end && isspace((unsigned char)*p))
		p++;
	return (p);
}

static const char *
json_string_end(const char *p, const char *end)
{
	if (p == end || *p++ != '"')
		return (NULL);
	while (p < end) {
		if (*p == '"')
			return (p + 1);
		if ((unsigned char)*p < 32)
			return (NULL);
		if (*p++ == '\\') {
			if (p == end)
				return (NULL);
			p++;
		}
	}
	return (NULL);
}

static const char *
json_value_end(const char *p, const char *end)
{
	char closers[64];
	unsigned depth = 0;

	p = json_space(p, end);
	if (p == end)
		return (NULL);
	if (*p == '"')
		return (json_string_end(p, end));
	if (*p != '{' && *p != '[') {
		while (p < end && !isspace((unsigned char)*p) &&
		    *p != ',' && *p != '}' && *p != ']')
			p++;
		return (p);
	}
	do {
		if (*p == '"') {
			p = json_string_end(p, end);
			if (p == NULL)
				return (NULL);
			continue;
		}
		if (*p == '{' || *p == '[') {
			if (depth == sizeof(closers))
				return (NULL);
			closers[depth++] = *p == '{' ? '}' : ']';
		} else if (*p == '}' || *p == ']') {
			if (depth == 0 || *p != closers[--depth])
				return (NULL);
		}
		p++;
	} while (p < end && depth != 0);
	return (depth == 0 ? p : NULL);
}

/* Member lookup only visits direct children; nested names cannot shadow it. */
static json_span_t
json_member(json_span_t object, const char *key)
{
	const char *p = json_space(object.first, object.end), *name, *value,
	    *end;
	size_t length = strlen(key);

	if (p == object.end || *p++ != '{')
		return ((json_span_t){ 0 });
	while ((p = json_space(p, object.end)) < object.end && *p != '}') {
		name = p;
		p = json_string_end(p, object.end);
		if (p == NULL)
			break;
		end = p;
		p = json_space(p, object.end);
		if (p == object.end || *p++ != ':')
			break;
		value = json_space(p, object.end);
		p = json_value_end(value, object.end);
		if (p == NULL || p == value)
			break;
		if ((size_t)(end - name) == length + 2 &&
		    memcmp(name + 1, key, length) == 0)
			return ((json_span_t){ value, p });
		p = json_space(p, object.end);
		if (p == object.end || *p++ != ',')
			break;
	}
	return ((json_span_t){ 0 });
}

static json_span_t
json_item(json_span_t array, unsigned index)
{
	const char *p = json_space(array.first, array.end), *end;

	if (p == array.end || *p++ != '[')
		return ((json_span_t){ 0 });
	for (unsigned i = 0; ; i++) {
		p = json_space(p, array.end);
		if (p == array.end || *p == ']')
			break;
		end = json_value_end(p, array.end);
		if (end == NULL || end == p)
			break;
		if (i == index)
			return ((json_span_t){ p, end });
		p = json_space(end, array.end);
		if (p == array.end || *p++ != ',')
			break;
	}
	return ((json_span_t){ 0 });
}

static bool
json_integer(json_span_t value, uint64_t maximum, uint64_t *out)
{
	uint64_t number = 0, digit;

	if (value.first == value.end)
		return (false);
	for (const char *p = value.first; p < value.end; p++) {
		if (*p < '0' || *p > '9')
			return (false);
		digit = (unsigned)(*p - '0');
		if (digit > maximum || number > (maximum - digit) / 10)
			return (false);
		number = number * 10 + digit;
	}
	*out = number;
	return (true);
}

static void
test_json_integer_extractor_is_bounded(void)
{
	static const char object[] =
	    "{\"nested\":{\"n\":2},\"n\":18446744073709551615}";
	static const char overflow[] = "18446744073709551616";
	static const char truncated[] = "\"unterminated\\";
	char nested[130];
	json_span_t root = { object, object + sizeof(object) - 1 };
	uint64_t number;

	scenario = "JSON extractor bounds fields, integers, strings and "
	    "nesting";
	expect(json_integer(json_member(root, "n"), UINT64_MAX, &number) &&
	    number == UINT64_MAX, "direct member overrides nested name");
	expect(!json_integer(json_member(root, "missing"), UINT64_MAX, &number),
	    "absent member returns failure");
	expect(!json_integer((json_span_t){ overflow,
	    overflow + sizeof(overflow) - 1 }, UINT64_MAX, &number),
	    "integer overflow returns failure");
	expect(!json_integer(json_member(root, "n"), UINT32_MAX, &number),
	    "narrow field overflow returns failure");
	expect(json_string_end(truncated,
	    truncated + sizeof(truncated) - 1) == NULL,
	    "escape at end of span returns failure");
	memset(nested, '[', 65);
	memset(nested + 65, ']', 65);
	expect(json_value_end(nested, nested + sizeof(nested)) == NULL,
	    "excessive nesting returns failure");
}

/* Every destination width is checked before narrowing the extracted integer. */
#define JSON_NUMBER(span, target) do { \
	uint64_t number, maximum = UINT64_MAX >> (64 - 8 * sizeof(target)); \
	if (!json_integer((span), maximum, &number)) { \
		expect(false, "missing or invalid integer: " #target); \
		return (false); \
	} \
	(target) = number; \
} while (0)
#define JSON_FIELD(object, key, target) \
	JSON_NUMBER(json_member((object), (key)), target)

static bool
native_fields(json_span_t root, apfs_nx_t *nx, apfs_spaceman_t *sm,
    uint64_t *mapped_bytes)
{
	json_span_t checkpoints, superblock, space, device, queues, ephemeral;
	json_span_t descriptors, descriptor, mappings, mapping;
	uint64_t oid, mapping_oid, size;
	bool found = false;

	bzero(nx, sizeof(*nx));
	bzero(sm, sizeof(*sm));
	checkpoints = json_member(root, "checkpoints");
	superblock = json_member(checkpoints, "superblock");
	space = json_member(root, "spaceman");
	JSON_FIELD(superblock, "block_size", nx->block_size);
	JSON_FIELD(superblock, "block_count", nx->block_count);
	JSON_FIELD(superblock, "xp_desc_base", nx->desc_base);
	JSON_FIELD(superblock, "xp_desc_blocks", nx->desc_blocks);
	JSON_FIELD(superblock, "xp_data_base", nx->data_base);
	JSON_FIELD(superblock, "xp_data_blocks", nx->data_blocks);
	JSON_FIELD(superblock, "max_file_systems", nx->max_volumes);
	ephemeral = json_member(superblock, "ephemeral_info");
	for (unsigned i = 0; i < 4; i++)
		JSON_NUMBER(json_item(ephemeral, i), nx->ephemeral_info[i]);
#define SM_FIELD(field) JSON_FIELD(space, #field, sm->field)
	SM_FIELD(block_size);
	SM_FIELD(blocks_per_chunk);
	SM_FIELD(chunks_per_cib);
	SM_FIELD(cibs_per_cab);
	SM_FIELD(ip_block_count);
	SM_FIELD(ip_bm_size_in_blocks);
	SM_FIELD(ip_bm_block_count);
	SM_FIELD(ip_bm_tx_multiplier);
	SM_FIELD(ip_bm_base);
	SM_FIELD(ip_base);
	SM_FIELD(ip_bm_xid_offset);
	SM_FIELD(ip_bitmap_offset);
	SM_FIELD(ip_bm_free_next_offset);
	SM_FIELD(struct_size);
#undef SM_FIELD
	/*
	 * Every native capture has the current layout; the decoder derives
	 * the end of the fixed structure from it, so the dump must state it.
	 */
	if (sm->struct_size != APFS_SM_BYTES) {
		expect(false,
		    "native capture has the current space-manager layout");
		return (false);
	}
	sm->structure_end = APFS_SM_BYTES;
	for (unsigned i = 0; i < 2; i++) {
		device = json_item(json_member(space, "devices"), i);
#define DEVICE_FIELD(field) JSON_FIELD(device, #field, sm->devices[i].field)
		DEVICE_FIELD(block_count);
		DEVICE_FIELD(chunk_count);
		DEVICE_FIELD(cib_count);
		DEVICE_FIELD(cab_count);
		DEVICE_FIELD(free_count);
		DEVICE_FIELD(addr_offset);
#undef DEVICE_FIELD
	}
	queues = json_member(space, "free_queues");
	for (unsigned i = 0; i < 3; i++)
		JSON_FIELD(json_item(queues, i), "tree_node_limit",
		    sm->free_queues[i].node_limit);
	JSON_FIELD(space, "oid", oid);
	descriptors = json_member(checkpoints, "descriptor_area");
	for (unsigned i = 0; ; i++) {
		descriptor = json_item(descriptors, i);
		if (descriptor.first == NULL)
			break;
		mappings = json_member(descriptor, "mappings");
		for (unsigned j = 0; ; j++) {
			mapping = json_item(mappings, j);
			if (mapping.first == NULL)
				break;
			JSON_FIELD(mapping, "oid", mapping_oid);
			if (mapping_oid != oid)
				continue;
			JSON_FIELD(mapping, "size", size);
			if (found && size != *mapped_bytes) {
				expect(false,
				    "space-manager mapping sizes disagree");
				return (false);
			}
			*mapped_bytes = size;
			found = true;
		}
	}
	expect(found, "checkpoint maps space manager");
	return (found);
}

static void
test_native_dumps_match_geometry_and_regions(void)
{
	const char *root = getenv("FSIMG_APFS_NATIVE");
	const char *patterns[] = { "e1-sweep/*/apfsdump.json",
	    "e12-sweep/*/apfsdump.json", "apfs-*/apfsdump.json" };
	apfs_nx_t nx, generated_nx;
	apfs_spaceman_t sm, generated_sm;
	fsimg_error_t error = { 0 };
	struct stat st;
	glob_t paths;
	char pattern[PATH_MAX];
	char *bytes;
	uint64_t mapped_bytes = 0, blocks;
	unsigned checked = 0;
	int fd, result;

	if (root == NULL) {
		puts("SKIP: native dumps (FSIMG_APFS_NATIVE is unset)");
		return;
	}
	for (unsigned i = 0; i < ARRAY_COUNT(patterns); i++) {
		CHECK(snprintf(pattern, sizeof(pattern), "%s/%s", root,
		    patterns[i]) < (int)sizeof(pattern));
		bzero(&paths, sizeof(paths));
		result = glob(pattern, 0, NULL, &paths);
		CHECK(result == 0 || result == GLOB_NOMATCH);
		for (size_t j = 0; j < paths.gl_pathc; j++) {
			scenario = paths.gl_pathv[j];
			fd = open(scenario, O_RDONLY);
			CHECK(fd >= 0);
			CHECK(fstat(fd, &st) == 0);
			/*
			 * The largest capture is 244 MiB; reject unbounded
			 * inputs.
			 */
			CHECK(st.st_size > 0 &&
			    st.st_size <= 512 * 1024 * 1024);
			bytes = mmap(NULL, (size_t)st.st_size, PROT_READ,
			    MAP_PRIVATE,
			    fd, 0);
			CHECK(bytes != MAP_FAILED);
			if (native_fields((json_span_t){ bytes,
			    bytes + st.st_size },
			    &nx, &sm, &mapped_bytes)) {
				if (status_is(apfs_spaceman_check(&sm,
				    nx.block_size,
				    nx.block_count, &blocks, &error), FSIMG_OK,
				    &error,
				    "space manager"))
					expect(blocks * nx.block_size ==
					    mapped_bytes,
					    "mapped size equals implied size");
				fixed_regions_match(&nx, &sm);
				if (nx.block_size == BLOCK) {
					CHECK(apfs_native_geometry(
					    nx.block_count,
					    &generated_nx, &generated_sm));
					geometry_equal(&generated_nx,
					    &generated_sm, &nx, &sm);
				}
				checked++;
			}
			CHECK(munmap(bytes, (size_t)st.st_size) == 0);
			CHECK(close(fd) == 0);
		}
		globfree(&paths);
	}
	printf("native dumps checked: %u\n", checked);
	expect(checked != 0, "at least one native dump checked");
}

/*
 * Each field mutation names the rule expected to reject its value and the
 * status it fails with.
 */
typedef struct field_mutation {
	const char *name;
	size_t offset;
	unsigned width;
	uint64_t value;
	const char *message;
	fsimg_status_t status;
} field_mutation_t;

#define SM_MUTATION_STATUS(field, value_, message_, status_) \
	{ #field " = " #value_, offsetof(apfs_spaceman_t, field), \
	    sizeof(((apfs_spaceman_t *)0)->field), value_, message_, status_ }
#define SM_MUTATION(field, value_, message_) \
	SM_MUTATION_STATUS(field, value_, message_, FSIMG_ECORRUPT)

static void
test_spaceman_rejects_inconsistent_geometry_and_arrays(void)
{
	static const field_mutation_t mutations[] = {
		SM_MUTATION(block_size, 8192, "wrong chunk geometry"),
		SM_MUTATION(blocks_per_chunk, 32767, "wrong chunk geometry"),
		SM_MUTATION(chunks_per_cib, 125, "wrong chunk geometry"),
		SM_MUTATION(cibs_per_cab, 506, "wrong chunk geometry"),
		SM_MUTATION(devices[0].block_count, 1, "device counts"),
		SM_MUTATION(devices[0].chunk_count, 2, "device counts"),
		SM_MUTATION(devices[0].cib_count, 2, "device counts"),
		SM_MUTATION(devices[0].free_count, 2097153, "device counts"),
		SM_MUTATION(devices[1].block_count, 1, "tier-2 device"),
		SM_MUTATION(devices[1].free_count, 1, "tier-2 device"),
		SM_MUTATION(ip_block_count, 0, "does not fit its pool"),
		SM_MUTATION(ip_bm_size_in_blocks, 2, "does not fit its pool"),
		SM_MUTATION(ip_bm_block_count, 0, "does not fit its pool"),
		SM_MUTATION(ip_bm_block_count, 65535, "does not fit its pool"),
		SM_MUTATION(ip_bm_xid_offset, 2512,
		    "inside its structure or unaligned"),
		SM_MUTATION(ip_bm_xid_offset, 2521,
		    "inside its structure or unaligned"),
		SM_MUTATION(ip_bitmap_offset, 2520, "overlapping arrays"),
		SM_MUTATION(ip_bm_free_next_offset, 2528, "overlapping arrays"),
		/* Address and XID arrays are non-adjacent in the array list. */
		SM_MUTATION(devices[0].addr_offset, 2520, "overlapping arrays"),
		SM_MUTATION(devices[0].addr_offset, 2561, "unaligned"),
		SM_MUTATION_STATUS(devices[0].addr_offset, BLOCK,
		    "packed extent", FSIMG_EUNSUPPORTED)
	};
	apfs_nx_t nx;
	apfs_spaceman_t sm;
	fsimg_error_t error = { 0 };
	uint64_t blocks;
	uint32_t value32;

	for (size_t i = 0; i < ARRAY_COUNT(mutations); i++) {
		scenario = mutations[i].name;
		CHECK(apfs_native_geometry(2097152, &nx, &sm));
		if (mutations[i].width == 8)
			bcopy(&mutations[i].value,
			    (char *)&sm + mutations[i].offset, 8);
		else {
			CHECK(mutations[i].width == 4);
			value32 = (uint32_t)mutations[i].value;
			bcopy(&value32, (char *)&sm + mutations[i].offset, 4);
		}
		status_is(apfs_spaceman_check(&sm, BLOCK, nx.block_count,
		    &blocks, &error), mutations[i].status, &error,
		    mutations[i].message);
	}
}

static void
test_older_spaceman_accepts_supported_block_sizes(void)
{
	const uint32_t sizes[] = { 4096, 16384 };
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t sm;
	unsigned char raw[16384];
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	for (unsigned i = 0; i < ARRAY_COUNT(sizes); i++) {
		scenario = sizes[i] == 4096 ? "older layout at 4096" :
		    "older layout at 16384";
		CHECK(apfs_gen_spaceman_geometry_older(sizes[i], 1024,
		    &generated) == sizes[i]);
		expect(generated.ip_bm_xid_offset == 336 &&
		    generated.ip_bitmap_offset == 344 &&
		    generated.ip_bm_free_next_offset == 352 &&
		    generated.devices[0].addr_offset == 384 &&
		    generated.devices[1].addr_offset == 392,
		    "older arrays have the reference packing");
		bzero(raw, sizeof(raw));
		/* The builder must preserve the caller's XID array at 336. */
		fsimg_store_le64(raw + 336, 10);
		CHECK(apfs_gen_spaceman_build(raw, sizes[i], &generated));
		expect(fsimg_load_le64(raw + 336) == 10,
		    "older builder leaves the XID array intact");
		CHECK(apfs_spaceman_decode(raw, sizes[i], &sm));
		if (status_is(apfs_spaceman_check(&sm, sizes[i], 1024,
		    &blocks, &error), FSIMG_OK, &error, "space manager"))
			expect(blocks == 1,
			    "older packed extent implies one block");
	}
}

static void
test_older_spaceman_rejects_invalid_arrays(void)
{
	static const struct {
		const char *name;
		uint32_t offset;
		uint32_t value;
		fsimg_status_t status;
		const char *message;
	} cases[] = {
		{ "older XID below structure", 324, 328, FSIMG_ECORRUPT,
		    "invalid structure size" },
		{ "older XID unaligned", 324, 337, FSIMG_ECORRUPT,
		    "invalid structure size" },
		{ "older bitmap index overlaps XID", 328, 336, FSIMG_ECORRUPT,
		    "overlapping arrays" },
		{ "older bitmap index unaligned", 328, 345, FSIMG_ECORRUPT,
		    "inside its structure or unaligned" },
		{ "older address array past packing", 80, 392,
		    FSIMG_EUNSUPPORTED, "packed extent" }
	};
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t sm;
	unsigned char raw[BLOCK];
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	CHECK(apfs_gen_spaceman_geometry_older(BLOCK, 1024,
	    &generated) == BLOCK);
	for (size_t i = 0; i < ARRAY_COUNT(cases); i++) {
		scenario = cases[i].name;
		bzero(raw, sizeof(raw));
		CHECK(apfs_gen_spaceman_build(raw, sizeof(raw), &generated));
		fsimg_store_le32(raw + cases[i].offset, cases[i].value);
		CHECK(apfs_spaceman_decode(raw, sizeof(raw), &sm));
		status_is(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks,
		    &error), cases[i].status, &error, cases[i].message);
	}
}

static void
test_older_xid_high_half_selects_current_structure(void)
{
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t sm;
	unsigned char raw[BLOCK];
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	scenario = "older XID high half 2520 selects current structure";
	CHECK(apfs_gen_spaceman_geometry_older(BLOCK, 1024,
	    &generated) == BLOCK);
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + 336, (UINT64_C(2520) << 32) | 10);
	CHECK(apfs_gen_spaceman_build(raw, sizeof(raw), &generated));
	CHECK(apfs_spaceman_decode(raw, sizeof(raw), &sm));
	status_is(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks, &error),
	    FSIMG_ECORRUPT, &error, "inside its structure or unaligned");
}

static void
test_current_spaceman_distinguishes_invalid_and_unknown_sizes(void)
{
	const uint32_t sizes[] = { 0, 2528, 4096 };
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t sm;
	unsigned char raw[BLOCK];
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	for (unsigned i = 0; i < ARRAY_COUNT(sizes); i++) {
		const char *names[] = { "current structure size 0",
		    "current structure size 2528",
		    "unknown layout with arrays past structure size 4096" };

		scenario = names[i];
		CHECK(apfs_gen_spaceman_geometry(BLOCK, 1024,
		    &generated) == BLOCK);
		generated.struct_size = sizes[i];
		if (sizes[i] == 4096) {
			generated.ip_bm_xid_offset += 4096 - 2520;
			generated.ip_bitmap_offset += 4096 - 2520;
			generated.ip_bm_free_next_offset += 4096 - 2520;
			for (unsigned d = 0; d < 2; d++)
				generated.devices[d].addr_offset += 4096 - 2520;
		}
		bzero(raw, sizeof(raw));
		CHECK(apfs_gen_spaceman_build(raw, sizeof(raw), &generated));
		CHECK(apfs_spaceman_decode(raw, sizeof(raw), &sm));
		status_is(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks,
		    &error), sizes[i] == 4096 ? FSIMG_EUNSUPPORTED :
		    FSIMG_ECORRUPT, &error, sizes[i] == 4096 ?
		    "structure size of an unknown layout" :
		    "invalid structure size");
		if (sizes[i] == 4096) {
			scenario = "unknown layout with wrong chunk geometry";
			fsimg_store_le32(raw + 36, generated.blocks_per_chunk - 1);
			CHECK(apfs_spaceman_decode(raw, sizeof(raw), &sm));
			status_is(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks,
			    &error), FSIMG_ECORRUPT, &error,
			    "wrong chunk geometry");
		}
	}
}

static void
test_wrong_cab_count_depends_on_proven_block_size(void)
{
	const uint32_t sizes[] = { 4096, 8192, 16384 };
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t sm;
	unsigned char raw[16384];
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	for (unsigned i = 0; i < ARRAY_COUNT(sizes); i++) {
		scenario = sizes[i] == 8192 ? "unproven CAB threshold at 8192" :
		    sizes[i] == 4096 ? "wrong CAB count at 4096" :
		    "wrong CAB count at 16384";
		CHECK(apfs_gen_spaceman_geometry(sizes[i], 1024,
		    &generated) != 0);
		bzero(raw, sizeof(raw));
		CHECK(apfs_gen_spaceman_build(raw, sizes[i], &generated));
		CHECK(apfs_spaceman_decode(raw, sizes[i], &sm));
		CHECK(apfs_spaceman_check(&sm, sizes[i], 1024, &blocks,
		    &error) == FSIMG_OK);
		sm.devices[0].cab_count = 1;
		status_is(apfs_spaceman_check(&sm, sizes[i], 1024, &blocks,
		    &error),
		    sizes[i] == 8192 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT,
		    &error, "address-block count");
	}
}

static void
test_array_end_determines_implied_size(void)
{
	apfs_nx_t nx;
	apfs_spaceman_t sm;
	fsimg_error_t error = { 0 };
	uint64_t blocks;

	scenario = "packed bitmap of one block implies one space-manager block";
	CHECK(apfs_native_geometry(1024, &nx, &sm));
	CHECK(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks,
	    &error) == FSIMG_OK);
	expect(blocks == 1, "packed size is one block");
	scenario = "larger ring packs the address array into the second block";
	sm.ip_bm_block_count = 1000;
	sm.devices[0].addr_offset = sm.ip_bm_free_next_offset + 2000;
	CHECK(apfs_spaceman_check(&sm, BLOCK, 1024, &blocks,
	    &error) == FSIMG_OK);
	expect(blocks == 2, "packed larger ring implies two blocks");
}

/*
 * Sparse fixture: only the checkpoint prefix is resident. The callback emits
 * physical CIBs and CABs on demand from their indexes, using the independent
 * generator. Addresses are explicit and disjoint; the image's advertised N
 * never becomes an allocation size. Chunks 0 and last each allocate one bit;
 * every other chunk is fully free and has no bitmap.
 */
#define CHUNKS_PER_CIB 126u
#define CIBS_PER_CAB 507u
#define BLOCKS_PER_CHUNK 32768ull
#define PHYSICAL_CIB 0x40000007u
#define PHYSICAL_CAB 0x40000006u
#define EPHEMERAL_SM 0x80000005u
#define EPHEMERAL_REAPER 0x80000011u
#define PHYSICAL_CHECKPOINT 0x4000000cu
#define PHYSICAL_OMAP 0x4000000bu
#define EPHEMERAL_NX 0x80000001u
#define SM_OID 100u
#define REAPER_OID 102u
#define OMAP_BLOCK 16u
#define REAPER_BLOCK 40u
#define EXTERNAL_BITMAP_BLOCK 900u
#define FIXTURE_XID 10u
#define PREFIX_BLOCKS 64u
#define SM_BLOCK 32u
#define CIB_BASE 9002u
#define CAB_BASE 9000u
#define RING_BASE 8000u

typedef enum pager_damage {
	DAMAGE_NONE,
	DAMAGE_CIB_INDEX,
	DAMAGE_CIB_REPEAT,
	DAMAGE_CIB_COUNT,
	DAMAGE_CAB_REPEAT,
	DAMAGE_CAB_INDEX,
	DAMAGE_CAB_COUNT,
	DAMAGE_CIB_ZERO,
	DAMAGE_CIB_OUTSIDE,
	DAMAGE_CHUNK_FIRST,
	DAMAGE_CHUNK_BLOCKS,
	DAMAGE_CHUNK_FREE,
	DAMAGE_CHUNK_NO_BITMAP,
	DAMAGE_CHUNK_BITMAP_OUTSIDE,
	DAMAGE_CHUNK_XID,
	DAMAGE_BITMAP_COUNT,
	DAMAGE_DEVICE_FREE,
	DAMAGE_POOL_SLOT,
	DAMAGE_POOL_XID,
	BITMAP_OUTSIDE_POOL
} pager_damage_t;

typedef struct fixture {
	unsigned char prefix[PREFIX_BLOCKS * BLOCK];
	apfs_gen_spaceman_t sm;
	apfs_gen_nx_t nx;
	pager_damage_t damage;
	uint64_t reads;
	uint64_t read_log[64];
	uint64_t reaper_address;
	uint64_t sm_reads[2];
	uint64_t extra_sm_reads;
	bool different_peek;
	size_t sm_bytes;
	uint64_t baseline;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t reader;
	apfs_container_t container;
	fsimg_error_t error;
} fixture_t;

static uint64_t cib_address(const fixture_t *, uint64_t);
static void emit_cib(fixture_t *, uint64_t, unsigned char *);
static void emit_cab(fixture_t *, uint64_t, unsigned char *);
static fsimg_status_t sparse_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t sparse_size(void *, uint64_t *, fsimg_error_t *);
static void emit_spaceman(fixture_t *);
static void fixture_init(fixture_t *, uint64_t, pager_damage_t);
static fsimg_status_t checkpoint_open(fixture_t *);
static void fixture_close(fixture_t *);
static bool walk_chunks(fixture_t *, apfs_sm_pager_t *);

/*
 * Only the emitter of a value supplies its target; all damage is stated here.
 */
typedef struct damage_targets {
	uint64_t *address;
	uint64_t *cab_address;
	apfs_gen_chunks_t *cib;
	apfs_gen_addresses_t *cab;
	apfs_gen_chunk_t *chunk;
	apfs_gen_spaceman_t *sm;
	uint64_t *pool_xid;
	uint16_t *pool_slot;
} damage_targets_t;

static void
apply_damage(const fixture_t *f, uint64_t index, damage_targets_t t)
{
	switch (f->damage) {
	case DAMAGE_CIB_ZERO:
		if (t.address != NULL && index == 0)
			*t.address = 0;
		break;
	case DAMAGE_CIB_OUTSIDE:
		if (t.address != NULL && index == 0)
			*t.address = f->nx.block_count;
		break;
	case DAMAGE_CIB_REPEAT:
		if (t.address != NULL && index == 1)
			*t.address = CIB_BASE;
		break;
	case DAMAGE_CAB_REPEAT:
		if (t.cab_address != NULL && index == 1)
			*t.cab_address = CAB_BASE;
		break;
	case DAMAGE_CIB_INDEX:
		if (t.cib != NULL && index == 0)
			t.cib->index = 1;
		break;
	case DAMAGE_CIB_COUNT:
		if (t.cib != NULL && index == 0)
			t.cib->count--;
		break;
	case DAMAGE_CAB_INDEX:
		if (t.cab != NULL && index == 0)
			t.cab->index = 1;
		break;
	case DAMAGE_CAB_COUNT:
		if (t.cab != NULL && index == 0)
			t.cab->count--;
		break;
	case DAMAGE_CHUNK_FIRST:
		if (t.chunk != NULL && index == 0)
			t.chunk->address++;
		break;
	case DAMAGE_CHUNK_BLOCKS:
		if (t.chunk != NULL && index == 0)
			t.chunk->block_count--;
		break;
	case DAMAGE_CHUNK_FREE:
		if (t.chunk != NULL && index == 0)
			t.chunk->free_count = BLOCKS_PER_CHUNK + 1;
		break;
	case DAMAGE_CHUNK_NO_BITMAP:
		if (t.chunk != NULL && index == 0)
			t.chunk->bitmap_address = 0;
		break;
	case DAMAGE_CHUNK_BITMAP_OUTSIDE:
		if (t.chunk != NULL && index == 0)
			t.chunk->bitmap_address = f->nx.block_count;
		break;
	case DAMAGE_CHUNK_XID:
		if (t.chunk != NULL && index == 0)
			t.chunk->xid = FIXTURE_XID + 1;
		break;
	case DAMAGE_BITMAP_COUNT:
		if (t.chunk != NULL && index == 0)
			t.chunk->free_count--;
		break;
	case BITMAP_OUTSIDE_POOL:
		if (t.chunk != NULL && index == 0)
			t.chunk->bitmap_address = EXTERNAL_BITMAP_BLOCK;
		break;
	case DAMAGE_DEVICE_FREE:
		if (t.sm != NULL)
			t.sm->devices[0].free_count++;
		break;
	case DAMAGE_POOL_SLOT:
		if (t.pool_slot != NULL)
			*t.pool_slot = (uint16_t)f->sm.ip_bm_block_count;
		break;
	case DAMAGE_POOL_XID:
		if (t.pool_xid != NULL)
			*t.pool_xid = FIXTURE_XID + 1;
		break;
	case DAMAGE_NONE:
		break;
	}
}

static uint64_t
cib_address(const fixture_t *f, uint64_t index)
{
	uint64_t address = CIB_BASE + index;

	apply_damage(f, index,
	    (damage_targets_t){ .address = &address });
	return (address);
}

static void
emit_cib(fixture_t *f, uint64_t index, unsigned char *raw)
{
	apfs_gen_chunk_t chunks[CHUNKS_PER_CIB];
	apfs_gen_chunks_t cib;
	uint64_t k, left, chunk_count = f->sm.devices[0].chunk_count;
	uint32_t count = (uint32_t)(chunk_count - index * CHUNKS_PER_CIB);

	if (count > CHUNKS_PER_CIB)
		count = CHUNKS_PER_CIB;
	for (uint32_t i = 0; i < count; i++) {
		k = index * CHUNKS_PER_CIB + i;
		left = f->nx.block_count - k * BLOCKS_PER_CHUNK;
		chunks[i] = (apfs_gen_chunk_t){ .xid = FIXTURE_XID,
		    .address = k * BLOCKS_PER_CHUNK,
		    .block_count = (uint32_t)(left < BLOCKS_PER_CHUNK ?
		    left : BLOCKS_PER_CHUNK) };
		chunks[i].free_count = chunks[i].block_count;
		if (k == 0 || k + 1 == chunk_count) {
			chunks[i].free_count--;
			chunks[i].bitmap_address = CIB_BASE +
			    f->sm.devices[0].cib_count + (k != 0);
		}
	}
	cib = (apfs_gen_chunks_t){ .object = { .oid = CIB_BASE + index,
	    .xid = FIXTURE_XID, .type = PHYSICAL_CIB },
	    .index = (uint32_t)index,
	    .count = count, .chunks = chunks };
	apply_damage(f, index, (damage_targets_t){ .cib = &cib,
	    .chunk = &chunks[0] });
	CHECK(apfs_gen_chunks_build(raw, BLOCK, &cib));
}

static void
emit_cab(fixture_t *f, uint64_t index, unsigned char *raw)
{
	uint64_t addresses[CIBS_PER_CAB], first = index * CIBS_PER_CAB;
	uint32_t count = f->sm.devices[0].cib_count - (uint32_t)first;
	apfs_gen_addresses_t cab;

	if (count > CIBS_PER_CAB)
		count = CIBS_PER_CAB;
	for (uint32_t i = 0; i < count; i++)
		addresses[i] = cib_address(f, first + i);
	cab = (apfs_gen_addresses_t){ .object = { .oid = CAB_BASE + index,
	    .xid = FIXTURE_XID, .type = PHYSICAL_CAB },
	    .index = (uint32_t)index,
	    .count = count, .addresses = addresses };
	apply_damage(f, index, (damage_targets_t){ .cab = &cab });
	CHECK(apfs_gen_addresses_build(raw, BLOCK, &cab));
}

static fsimg_status_t
sparse_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;
	unsigned char raw[BLOCK];
	uint64_t block = offset / BLOCK;
	uint64_t bitmap_base = CIB_BASE + f->sm.devices[0].cib_count;

	CHECK(offset % BLOCK + length <= BLOCK);
	CHECK(offset < f->nx.block_count * BLOCK);
	CHECK(length <= f->nx.block_count * BLOCK - offset);
	if (f->reads < ARRAY_COUNT(f->read_log))
		f->read_log[f->reads] = block;
	f->reads++;
	if (block >= SM_BLOCK && block < SM_BLOCK + 2)
		f->sm_reads[block - SM_BLOCK]++;
	if (block == SM_BLOCK + 1)
		f->extra_sm_reads++;
	bzero(raw, sizeof(raw));
	if (block == f->reaper_address)
		bcopy(f->prefix + REAPER_BLOCK * BLOCK, raw, BLOCK);
	else if (block < PREFIX_BLOCKS)
		bcopy(f->prefix + block * BLOCK, raw, BLOCK);
	else if (block >= CIB_BASE &&
	    block - CIB_BASE < f->sm.devices[0].cib_count)
		emit_cib(f, block - CIB_BASE, raw);
	else if (block >= CAB_BASE &&
	    block - CAB_BASE < f->sm.devices[0].cab_count)
		emit_cab(f, block - CAB_BASE, raw);
	else if (block == bitmap_base || block == bitmap_base + 1 ||
	    block == EXTERNAL_BITMAP_BLOCK) {
		raw[0] = 1;
		/* Bits past N are outside the last chunk's free-count check. */
		if (block == bitmap_base + 1 &&
		    f->nx.block_count == SMALL_BLOCKS)
			raw[BLOCK - 1] = 0xff;
	} else if (block >= RING_BASE && block - RING_BASE <
	    f->sm.ip_bm_block_count)
		memset(raw, (int)(0x50 + block - RING_BASE), BLOCK);
	else
		/* An unexpected read must not become silent zero I/O. */
		CHECK(false);
	/*
	 * The unverified peek is valid but differs from the checksummed copy.
	 */
	if (block == SM_BLOCK && f->different_peek && f->sm_reads[0] == 1) {
		fsimg_store_le32(raw + 148, 17);
		CHECK(apfs_gen_seal(raw, BLOCK));
	}
	bcopy(raw + offset % BLOCK, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_size(void *argument, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	*length = f->nx.block_count * BLOCK;
	return (FSIMG_OK);
}

static void
emit_spaceman(fixture_t *f)
{
	unsigned char *raw = f->prefix + SM_BLOCK * BLOCK;
	uint32_t b = f->sm.ip_bm_size_in_blocks;
	uint32_t ring = f->sm.ip_bm_block_count;
	uint32_t addresses = f->sm.devices[0].cab_count != 0 ?
	    f->sm.devices[0].cab_count : f->sm.devices[0].cib_count;
	uint64_t address;
	uint16_t next, slot;
	uint64_t xid;

	bzero(raw, f->sm_bytes);
	CHECK(apfs_gen_spaceman_build(raw, f->sm_bytes, &f->sm));
	for (uint32_t i = 0; i < b; i++) {
		xid = FIXTURE_XID;
		slot = (uint16_t)(b + i);
		apply_damage(f, i, (damage_targets_t){ .pool_xid = &xid,
		    .pool_slot = &slot });
		fsimg_store_le64(raw + f->sm.ip_bm_xid_offset + 8 * i, xid);
		fsimg_store_le16(raw + f->sm.ip_bitmap_offset + 2 * i, slot);
	}
	/* Current slots b..2b-1; free chain 0..b-1, 2b..ring-1. */
	for (uint32_t i = 0; i < ring; i++) {
		next = (uint16_t)(i + 1);
		if (i == b - 1)
			next = (uint16_t)(2 * b);
		if ((i >= b && i < 2 * b) || i + 1 == ring)
			next = 65535;
		fsimg_store_le16(raw + f->sm.ip_bm_free_next_offset + 2 * i,
		    next);
	}
	for (uint32_t i = 0; i < addresses; i++) {
		address = f->sm.devices[0].cab_count == 0 ? cib_address(f, i) :
		    CAB_BASE + i;
		if (f->sm.devices[0].cab_count != 0)
			apply_damage(f, i, (damage_targets_t){
			    .cab_address = &address });
		fsimg_store_le64(raw + f->sm.devices[0].addr_offset + 8 * i,
		    address);
	}
	CHECK(apfs_gen_seal(raw, f->sm_bytes));
}

static void
fixture_init(fixture_t *f, uint64_t n, pager_damage_t damage)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = EPHEMERAL_SM, .size = BLOCK, .oid = SM_OID,
		    .address = SM_BLOCK },
		{ .type = EPHEMERAL_REAPER, .size = BLOCK, .oid = REAPER_OID,
		    .address = REAPER_BLOCK }
	};
	apfs_gen_checkpoint_t checkpoint = {
		.object = { .oid = 1, .xid = FIXTURE_XID,
		    .type = PHYSICAL_CHECKPOINT },
		.mappings = mappings, .count = 2, .flags = 1
	};
	apfs_gen_object_t omap = { .oid = OMAP_BLOCK, .xid = FIXTURE_XID,
	    .type = PHYSICAL_OMAP };
	apfs_gen_reaper_t reaper = { .object = { .oid = REAPER_OID,
	    .xid = FIXTURE_XID,
	    .type = EPHEMERAL_REAPER }, .flags = 1, .next_reap_id = 1 };
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	f->damage = damage;
	f->reaper_address = REAPER_BLOCK;
	f->nx = (apfs_gen_nx_t){ .object = { .oid = 1, .xid = FIXTURE_XID,
	    .type = EPHEMERAL_NX }, .magic = 0x4253584e, .block_size = BLOCK,
	    .block_count = n, .uuid = { 1 }, .next_xid = FIXTURE_XID + 1,
	    .desc_base = 1, .desc_blocks = 8, .data_base = SM_BLOCK,
	    .data_blocks = 32, .desc_index = 0, .desc_len = 2, .desc_next = 2,
	    .data_len = 9, .data_next = 9, .max_volumes = 100,
	    .spaceman_oid = SM_OID, .reaper_oid = REAPER_OID,
	    .omap_oid = OMAP_BLOCK };
	CHECK(apfs_gen_spaceman_geometry(BLOCK, n, &f->sm) == BLOCK);
	f->sm.object = (apfs_gen_object_t){ .oid = SM_OID, .xid = FIXTURE_XID,
	    .type = EPHEMERAL_SM };
	f->sm.devices[0].free_count = n - 2;
	apply_damage(f, 0, (damage_targets_t){ .sm = &f->sm });
	f->sm_bytes = BLOCK;
	f->sm.ip_bm_base = RING_BASE;
	f->sm.ip_base = CAB_BASE;
	f->sm.ip_bm_free_head = 0;
	f->sm.ip_bm_free_tail = (uint16_t)(f->sm.ip_bm_block_count - 1);
	CHECK(apfs_gen_nx_build(f->prefix, BLOCK, &f->nx));
	CHECK(apfs_gen_nx_build(f->prefix + 2 * BLOCK, BLOCK, &f->nx));
	CHECK(apfs_gen_checkpoint_build(f->prefix + BLOCK, BLOCK, &checkpoint));
	CHECK(apfs_gen_reaper_build(f->prefix + REAPER_BLOCK * BLOCK, BLOCK,
	    &reaper));
	CHECK(apfs_gen_object_build(f->prefix + OMAP_BLOCK * BLOCK, BLOCK,
	    &omap));
	emit_spaceman(f);

	fsimg_options_init(&options);
	/* The context's smallest supported memory limit is 16 MiB. */
	options.memory_bytes = 16 * 1024 * 1024;
	/*
	 * Each metadata read charges both the APFS operation and bounded I/O.
	 */
	options.max_work = f->sm.devices[0].chunk_count +
	    2 * (f->sm.devices[0].cib_count + f->sm.devices[0].cab_count +
	    f->sm.ip_bm_size_in_blocks + 2) + 256;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline = stats.memory_bytes;
	f->reader.ctx = f->ctx;
	fsimg_read_options_init(&f->reader.options);
	fsimg_session_init(&f->reader.session, f->ctx, "spaceman test", 0);
	f->reader.state = FSIMG_READER_OPENING;
	fsimg_io_ops_init(&ops);
	ops.read_at = sparse_read;
	ops.get_size = sparse_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io,
	    NULL) == FSIMG_OK);
	f->reader.input = f->io;
	f->reader.slice_bytes = n * BLOCK;
}

static fsimg_status_t
checkpoint_open(fixture_t *f)
{
	apfs_obj_t header;
	apfs_nx_t nx;

	CHECK(apfs_obj_decode(f->prefix, BLOCK, &header));
	CHECK(apfs_nx_decode(f->prefix, BLOCK, &nx));
	return (apfs_checkpoint_open(&f->reader, &header, &nx, &f->container,
	    &f->error));
}

static void
fixture_close(fixture_t *f)
{
	fsimg_stats_t stats;

	apfs_container_close(&f->container);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	expect(stats.memory_bytes == f->baseline, "fixture releases memory");
	expect(stats.scratch_bytes == 0, "fixture releases scratch");
	fsimg_ctx_free(f->ctx);
}

static void
test_checkpoint_keeps_verified_spaceman(void)
{
	fixture_t f;
	unsigned char encoded[BLOCK] = { 0 };

	scenario = "checkpoint retains checked bytes rather than the valid "
	    "peek";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_NONE);
	f.different_peek = true;
	if (status_is(checkpoint_open(&f), FSIMG_OK, &f.error,
	    "space manager")) {
		CHECK(apfs_spaceman_encode(encoded, BLOCK,
		    &f.container.spaceman));
		expect(memcmp(encoded + 32, f.prefix + SM_BLOCK * BLOCK + 32,
		    2520 - 32) == 0, "container holds every generated field");
	}
	fixture_close(&f);
}

static void
test_mapped_size_mismatch_rejected_before_extra_block_read(void)
{
	fixture_t f;
	unsigned char *map;

	scenario = "space-manager mapping exceeds implied size";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_NONE);
	map = f.prefix + BLOCK;
	fsimg_store_le32(map + 48, 2 * BLOCK);
	CHECK(apfs_gen_seal(map, BLOCK));
	status_is(checkpoint_open(&f), FSIMG_ECORRUPT, &f.error,
	    "has the wrong size");
	expect(f.extra_sm_reads == 0, "extra mapped block never read");
	fixture_close(&f);
}

/*
 * Grow the data run with the mapping; keep its tail probe outside the object.
 */
static void
fixture_map_spaceman(fixture_t *f, uint32_t bytes)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = EPHEMERAL_SM, .size = bytes, .oid = SM_OID,
		    .address = SM_BLOCK },
		{ .type = EPHEMERAL_REAPER, .size = BLOCK, .oid = REAPER_OID,
		    .address = SM_BLOCK + bytes / BLOCK }
	};
	apfs_gen_checkpoint_t checkpoint = {
		.object = { .oid = 1, .xid = FIXTURE_XID,
		    .type = PHYSICAL_CHECKPOINT },
		.mappings = mappings, .count = ARRAY_COUNT(mappings), .flags = 1
	};

	f->reaper_address = mappings[1].address;
	f->nx.data_blocks = bytes / BLOCK + 1;
	f->nx.data_len = f->nx.data_blocks;
	f->nx.data_next = 0;
	CHECK(apfs_gen_nx_build(f->prefix, BLOCK, &f->nx));
	CHECK(apfs_gen_nx_build(f->prefix + 2 * BLOCK, BLOCK, &f->nx));
	CHECK(apfs_gen_checkpoint_build(f->prefix + BLOCK, BLOCK, &checkpoint));
}

static void
test_checkpoint_rejects_hostile_first_block_before_object_read(void)
{
	fixture_t f;
	uint64_t end;
	uint32_t mapped_bytes;

	for (unsigned i = 0; i < 2; i++) {
		scenario = i == 0 ? "wrong chunk geometry before inflated "
		    "object read" :
		    "inflated CIB count before large matching mapping read";
		fixture_init(&f, 1048576, DAMAGE_NONE);
		/* Matching size for B=4096, cab_count=0, cib_count=2^28. */
		f.sm.devices[0].cab_count = 0;
		f.sm.devices[0].cib_count = 268435456;
		f.sm.devices[0].addr_offset = 2568;
		if (i == 0)
			f.sm.blocks_per_chunk--;
		end = 2568ull + 8ull * f.sm.devices[0].cib_count;
		mapped_bytes = (uint32_t)((end + BLOCK - 1) / BLOCK * BLOCK);
		fixture_map_spaceman(&f, mapped_bytes);
		/*
		 * Emit only fixed fields; the hostile array is never
		 * materialized.
		 */
		CHECK(apfs_gen_spaceman_build(f.prefix + SM_BLOCK * BLOCK,
		    BLOCK, &f.sm));
		status_is(checkpoint_open(&f), FSIMG_ECORRUPT, &f.error,
		    i == 0 ? "wrong chunk geometry" : "device counts");
		expect(f.sm_reads[0] == 1,
		    "only the unverified first block is read");
		CHECK(f.reads <= ARRAY_COUNT(f.read_log));
		for (uint64_t k = 0; k < f.reads; k++)
			expect(f.read_log[k] <= SM_BLOCK ||
			    f.read_log[k] >= SM_BLOCK + mapped_bytes / BLOCK,
			    "read log contains no block past the object's "
			    "first block");
		fixture_close(&f);
	}

	scenario = "current structure size bit flip with stale checksum";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_NONE);
	fsimg_store_le32(f.prefix + SM_BLOCK * BLOCK + 340, 2520 ^ 0x1000);
	status_is(checkpoint_open(&f), FSIMG_ECORRUPT, &f.error,
	    "invalid structure size");
	fixture_close(&f);
}

static void
test_checkpoint_rejects_fusion(void)
{
	fixture_t f;

	scenario = "Fusion space manager is unsupported";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_NONE);
	f.nx.incompat_features |= 0x100;
	CHECK(apfs_gen_nx_build(f.prefix, BLOCK, &f.nx));
	CHECK(apfs_gen_nx_build(f.prefix + 2 * BLOCK, BLOCK, &f.nx));
	status_is(checkpoint_open(&f), FSIMG_EUNSUPPORTED, &f.error, "Fusion");
	fixture_close(&f);
}

static bool
walk_chunks(fixture_t *f, apfs_sm_pager_t *pager)
{
	apfs_sm_chunk_t chunk;
	const unsigned char *bits;
	uint64_t chunks = f->sm.devices[0].chunk_count, free_sum = 0, clear;
	bool found;

	for (uint64_t k = 0; k < chunks; k++) {
		if (!status_is(apfs_sm_pager_next(pager, &chunk, &found,
		    &f->error),
		    FSIMG_OK, &f->error, "chunk"))
			return (false);
		expect(found, "next chunk found");
		if (!found)
			return (false);
		expect(chunk.index == k, "chunk indexes ascend");
		expect(chunk.info.address == k * BLOCKS_PER_CHUNK,
		    "chunk starts at its position");
		expect(chunk.first_of_cib == (k % CHUNKS_PER_CIB == 0),
		    "first chunk of CIB");
		expect(chunk.first_of_cab == (f->sm.devices[0].cab_count != 0 &&
		    k % (CHUNKS_PER_CIB * CIBS_PER_CAB) == 0),
		    "first chunk of CAB");
		expect(chunk.cib_address == CIB_BASE + k / CHUNKS_PER_CIB,
		    "CIB physical address");
		expect(chunk.cab_address ==
		    (f->sm.devices[0].cab_count == 0 ? 0 :
		    CAB_BASE + k / (CHUNKS_PER_CIB * CIBS_PER_CAB)),
		    "CAB physical address");
		if (!status_is(apfs_sm_pager_bitmap(pager, &bits, &f->error),
		    FSIMG_OK, &f->error, "chunk"))
			return (false);
		clear = chunk.info.block_count;
		if (bits != NULL)
			for (uint32_t i = 0; i < chunk.info.block_count; i++)
				clear -= (bits[i / 8] >> (i % 8)) & 1;
		expect(clear == chunk.info.free_count,
		    "bitmap clear bits match free count");
		expect((bits == NULL) == (k != 0 && k + 1 != chunks),
		    "fully free chunks omit bitmap");
		free_sum += clear;
	}
	if (!status_is(apfs_sm_pager_next(pager, &chunk, &found, &f->error),
	    FSIMG_OK, &f->error, "space manager"))
		return (false);
	expect(!found, "iteration ends with found false");
	expect(free_sum == f->sm.devices[0].free_count,
	    "device free count recounted");
	return (true);
}

static void
test_pager_caches_second_spaceman_block(void)
{
	fixture_t f;
	apfs_sm_pager_t *pager;
	const unsigned char *bits;
	uint64_t block, first_reads, second_reads;

	scenario = "two-block space manager caches its second-block address "
	    "array";
	fixture_init(&f, 127 * BLOCKS_PER_CHUNK, DAMAGE_NONE);
	f.sm.ip_bm_block_count = 1000;
	f.sm.ip_bm_free_tail = 999;
	f.sm.devices[0].addr_offset = f.sm.ip_bm_free_next_offset + 2000;
	f.sm.devices[1].addr_offset = f.sm.devices[0].addr_offset +
	    8 * f.sm.devices[0].cib_count;
	f.sm_bytes = 2 * BLOCK;
	fixture_map_spaceman(&f, (uint32_t)f.sm_bytes);
	emit_spaceman(&f);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	expect(f.sm_reads[0] == 3 && f.sm_reads[1] == 1,
	    "checkpoint peeks, checksums both blocks and rereads the first");
	first_reads = f.sm_reads[0];
	second_reads = f.sm_reads[1];
	CHECK(apfs_sm_pager_open(&f.container, &pager, &f.error) == FSIMG_OK);
	CHECK(walk_chunks(&f, pager));
	expect(f.sm_reads[0] == first_reads,
	    "chunk walk never reads first-block pool arrays");
	expect(f.sm_reads[1] == second_reads + 1,
	    "both CIB addresses reuse one cached second-block read");
	CHECK(apfs_sm_pager_pool_bitmap(pager, 0, &block, &bits,
	    &f.error) == FSIMG_OK);
	expect(f.sm_reads[0] == first_reads + 1 &&
	    f.sm_reads[1] == second_reads + 1,
	    "pool slot and XID reuse one first-block read");
	apfs_sm_pager_free(pager);
	fixture_close(&f);
}

static void
test_pager_walks_small_and_large_sparse_containers(void)
{
	const uint64_t sizes[] = { SMALL_BLOCKS, LARGE_BLOCKS };
	fixture_t f;
	apfs_sm_pager_t *pager;
	const unsigned char *bits;
	uint64_t block, before, memory_before, reads_before, expected_reads;
	fsimg_stats_t stats;
	uint64_t peak_growth[ARRAY_COUNT(sizes)] = { 0 };

	for (unsigned i = 0; i < ARRAY_COUNT(sizes); i++) {
		scenario = i == 0 ? "pager walks partial and fully free "
		    "chunks" :
		    "pager walks two CABs with bounded memory and linear work";
		fixture_init(&f, sizes[i], DAMAGE_NONE);
		if (!status_is(checkpoint_open(&f), FSIMG_OK, &f.error,
		    "space manager")) {
			fixture_close(&f);
			continue;
		}
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		memory_before = stats.memory_bytes;
		before = f.reader.session.progress.work;
		reads_before = f.reads;
		if (status_is(apfs_sm_pager_open(&f.container, &pager,
		    &f.error),
		    FSIMG_OK, &f.error, "space manager")) {
			expect(f.reads == reads_before,
			    "pager open reads nothing");
			expect(f.reader.session.progress.work == before,
			    "pager open charges no work");
			status_is(apfs_sm_pager_bitmap(pager, &bits, &f.error),
			    FSIMG_EINVAL, &f.error,
			    "no space-manager chunk visited");
			status_is(apfs_sm_pager_pool_bitmap(pager,
			    f.sm.ip_bm_size_in_blocks, &block, &bits, &f.error),
			    FSIMG_EINVAL, &f.error, "out of range");
			if (!walk_chunks(&f, pager)) {
				apfs_sm_pager_free(pager);
				fixture_close(&f);
				continue;
			}
			for (uint32_t k = 0; k < f.sm.ip_bm_size_in_blocks;
			    k++) {
				CHECK(apfs_sm_pager_pool_bitmap(pager, k,
				    &block, &bits,
				    &f.error) == FSIMG_OK);
				expect(block ==
				    RING_BASE + f.sm.ip_bm_size_in_blocks + k,
				    "pool bitmap follows index array");
				expect(bits[0] == (unsigned char)(0x50 +
				    f.sm.ip_bm_size_in_blocks + k),
				    "selected ring slot bytes");
			}
			expected_reads = f.sm.devices[0].cib_count +
			    f.sm.devices[0].cab_count + 3 +
			    f.sm.ip_bm_size_in_blocks;
			expect(f.reads - reads_before == expected_reads,
			    "arrays, each CIB, CAB and selected bitmap read "
			    "once");
			/*
			 * Bounded I/O charges one unit per read, besides the
			 * pager's.
			 */
			expect(f.reader.session.progress.work - before ==
			    f.sm.devices[0].chunk_count + 2 * expected_reads,
			    "work equals chunks plus metadata and I/O read "
			    "charges");
			CHECK(fsimg_ctx_get_stats(f.ctx, &stats,
			    NULL) == FSIMG_OK);
			/*
			 * No reset API: compare peak growth from the open
			 * container.
			 */
			peak_growth[i] = stats.memory_peak - memory_before;
			expect(peak_growth[i] >= 4 * BLOCK &&
			    peak_growth[i] <= 32 * 1024,
			    "peak covers four block buffers and bounded pager "
			    "overhead");
			apfs_sm_pager_free(pager);
		}
		fixture_close(&f);
	}
	expect(peak_growth[0] == peak_growth[1],
	    "small and 8 TiB walks have the same peak memory growth");
}

static void
test_pager_rejects_damaged_chunk_structures(void)
{
	static const struct {
		const char *name;
		pager_damage_t damage;
		const char *message;
		uint64_t blocks;
	} cases[] = {
		{ .name = "CIB index wrong", .damage = DAMAGE_CIB_INDEX,
		    .message = "out of order", .blocks = SMALL_BLOCKS },
		{ .name = "CIB listed twice directly",
		    .damage = DAMAGE_CIB_REPEAT,
		    .message = "out of order",
		    .blocks = 127 * BLOCKS_PER_CHUNK },
		{ .name = "CIB listed twice in CAB",
		    .damage = DAMAGE_CIB_REPEAT,
		    .message = "out of order", .blocks = LARGE_BLOCKS },
		{ .name = "CIB entry count wrong", .damage = DAMAGE_CIB_COUNT,
		    .message = "wrong entry count", .blocks = SMALL_BLOCKS },
		{ .name = "CAB listed twice", .damage = DAMAGE_CAB_REPEAT,
		    .message = "out of order", .blocks = LARGE_BLOCKS },
		{ .name = "CAB index wrong", .damage = DAMAGE_CAB_INDEX,
		    .message = "out of order", .blocks = LARGE_BLOCKS },
		{ .name = "CAB entry count wrong", .damage = DAMAGE_CAB_COUNT,
		    .message = "wrong entry count", .blocks = LARGE_BLOCKS },
		{ .name = "CIB address zero", .damage = DAMAGE_CIB_ZERO,
		    .message = "outside the container",
		    .blocks = SMALL_BLOCKS },
		{ .name = "CIB address outside device",
		    .damage = DAMAGE_CIB_OUTSIDE,
		    .message = "outside the container",
		    .blocks = SMALL_BLOCKS },
		{ .name = "chunk first block wrong",
		    .damage = DAMAGE_CHUNK_FIRST,
		    .message = "does not cover the blocks",
		    .blocks = SMALL_BLOCKS },
		{ .name = "chunk block count wrong",
		    .damage = DAMAGE_CHUNK_BLOCKS,
		    .message = "does not cover the blocks",
		    .blocks = SMALL_BLOCKS },
		{ .name = "chunk free count above block count",
		    .damage = DAMAGE_CHUNK_FREE,
		    .message = "more free blocks than blocks",
		    .blocks = SMALL_BLOCKS },
		{ .name = "allocated chunk omits bitmap",
		    .damage = DAMAGE_CHUNK_NO_BITMAP,
		    .message = "bitmap address its counts do not allow",
		    .blocks = SMALL_BLOCKS },
		{ .name = "chunk bitmap outside device",
		    .damage = DAMAGE_CHUNK_BITMAP_OUTSIDE,
		    .message = "bitmap address its counts do not allow",
		    .blocks = SMALL_BLOCKS },
		{ .name = "chunk newer than checkpoint",
		    .damage = DAMAGE_CHUNK_XID,
		    .message = "newer than the checkpoint",
		    .blocks = SMALL_BLOCKS }
	};
	fixture_t f;
	apfs_sm_pager_t *pager;
	apfs_sm_chunk_t chunk;
	fsimg_status_t status;
	bool found;

	for (unsigned i = 0; i < ARRAY_COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, cases[i].blocks, cases[i].damage);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		CHECK(apfs_sm_pager_open(&f.container, &pager,
		    &f.error) == FSIMG_OK);
		do {
			status = apfs_sm_pager_next(pager, &chunk, &found,
			    &f.error);
		} while (status == FSIMG_OK && found);
		status_is(status, FSIMG_ECORRUPT, &f.error, cases[i].message);
		apfs_sm_pager_free(pager);
		fixture_close(&f);
	}
}

static void
test_bitmap_free_count_mismatch_fails_when_bitmap_read(void)
{
	fixture_t f;
	apfs_sm_pager_t *pager;
	apfs_sm_chunk_t chunk;
	const unsigned char *bits;
	bool found;

	scenario = "chunk free count disagrees with bitmap";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_BITMAP_COUNT);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	CHECK(apfs_sm_pager_open(&f.container, &pager, &f.error) == FSIMG_OK);
	CHECK(apfs_sm_pager_next(pager, &chunk, &found, &f.error) == FSIMG_OK);
	expect(found, "chunk metadata accepted before bitmap read");
	status_is(apfs_sm_pager_bitmap(pager, &bits, &f.error), FSIMG_ECORRUPT,
	    &f.error, "free count differs from its bitmap");
	apfs_sm_pager_free(pager);
	fixture_close(&f);
}

static void
test_device_free_count_mismatch_fails_at_end(void)
{
	fixture_t f;
	apfs_sm_pager_t *pager;
	apfs_sm_chunk_t chunk;
	bool found;

	scenario = "device free count disagrees with sum of chunks";
	fixture_init(&f, SMALL_BLOCKS, DAMAGE_DEVICE_FREE);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	CHECK(apfs_sm_pager_open(&f.container, &pager, &f.error) == FSIMG_OK);
	for (uint64_t k = 0; k < f.sm.devices[0].chunk_count; k++) {
		CHECK(apfs_sm_pager_next(pager, &chunk, &found,
		    &f.error) == FSIMG_OK);
		expect(found, "every chunk accepted before final sum check");
	}
	status_is(apfs_sm_pager_next(pager, &chunk, &found, &f.error),
	    FSIMG_ECORRUPT, &f.error, "free count differs from its chunks");
	expect(!found, "end-of-device call reports found false");
	apfs_sm_pager_free(pager);
	fixture_close(&f);
}

static void
test_pool_bitmap_rejects_invalid_slot_and_future_xid(void)
{
	const pager_damage_t damages[] = { DAMAGE_POOL_SLOT, DAMAGE_POOL_XID };
	fixture_t f;
	apfs_sm_pager_t *pager;
	const unsigned char *bits;
	uint64_t block;

	for (unsigned i = 0; i < ARRAY_COUNT(damages); i++) {
		scenario = i == 0 ? "pool bitmap slot outside ring" :
		    "pool bitmap XID newer than checkpoint";
		fixture_init(&f, SMALL_BLOCKS, damages[i]);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		CHECK(apfs_sm_pager_open(&f.container, &pager,
		    &f.error) == FSIMG_OK);
		status_is(apfs_sm_pager_pool_bitmap(pager, 0, &block, &bits,
		    &f.error),
		    FSIMG_ECORRUPT, &f.error, i == 0 ? "outside its ring" :
		    "newer than the checkpoint");
		apfs_sm_pager_free(pager);
		fixture_close(&f);
	}
}

static void
test_chunk_bitmap_outside_pool_is_passed_through(void)
{
	fixture_t f;
	apfs_sm_pager_t *pager;
	apfs_sm_chunk_t chunk;
	const unsigned char *bits;
	bool found;

	scenario = "chunk bitmap inside container outside internal pool "
	    "accepted";
	fixture_init(&f, SMALL_BLOCKS, BITMAP_OUTSIDE_POOL);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	CHECK(apfs_sm_pager_open(&f.container, &pager, &f.error) == FSIMG_OK);
	CHECK(apfs_sm_pager_next(pager, &chunk, &found, &f.error) == FSIMG_OK);
	expect(found && chunk.info.bitmap_address == EXTERNAL_BITMAP_BLOCK,
	    "external bitmap address preserved for verification");
	if (status_is(apfs_sm_pager_bitmap(pager, &bits, &f.error), FSIMG_OK,
	    &f.error, "chunk"))
		expect(bits != NULL && bits[0] == 1,
		    "external bitmap bytes read");
	apfs_sm_pager_free(pager);
	fixture_close(&f);
}

int
main(void)
{
	test_native_geometry_matches_embedded_captures();
	test_json_integer_extractor_is_bounded();
	test_spaceman_rejects_inconsistent_geometry_and_arrays();
	test_older_spaceman_accepts_supported_block_sizes();
	test_older_spaceman_rejects_invalid_arrays();
	test_older_xid_high_half_selects_current_structure();
	test_current_spaceman_distinguishes_invalid_and_unknown_sizes();
	test_wrong_cab_count_depends_on_proven_block_size();
	test_array_end_determines_implied_size();
	test_checkpoint_keeps_verified_spaceman();
	test_mapped_size_mismatch_rejected_before_extra_block_read();
	test_checkpoint_rejects_hostile_first_block_before_object_read();
	test_checkpoint_rejects_fusion();
	test_pager_caches_second_spaceman_block();
	test_pager_walks_small_and_large_sparse_containers();
	test_pager_rejects_damaged_chunk_structures();
	test_bitmap_free_count_mismatch_fails_when_bitmap_read();
	test_device_free_count_mismatch_fails_at_end();
	test_pool_bitmap_rejects_invalid_slot_and_future_xid();
	test_chunk_bitmap_outside_pool_is_passed_through();
	test_native_dumps_match_geometry_and_regions();
	return (failures == 0 ? 0 : 1);
}
