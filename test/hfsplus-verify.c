/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg.h"
#include "hfsplus-gen.h"
#include "reader.h"
#include "meta.h"
#include "test.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct fixture {
	gen_image_t *image;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	uint64_t length;
	uint64_t fail_offset;
	uint64_t reads;
	uint64_t bitmap_reads;
	uint64_t watched_first;
	uint64_t watched_end;
	uint64_t watched_reads;
	uint64_t watched_bytes;
	uint64_t cancel_at;
	uint64_t polls;
	int scratch_fd;
	char scratch_path[64];
} fixture_t;

typedef struct corruption {
	const char *name;
	void (*build)(gen_image_t *);
	fsimg_status_t status;
	const char *message;
} corruption_t;

typedef enum compression_damage {
	COMPRESSION_SOUND,
	COMPRESSION_OUTSIDE,
	COMPRESSION_OVERLAP,
	COMPRESSION_DESCENDING,
	COMPRESSION_DECODE_SHORT,
	COMPRESSION_BAD_STREAM
} compression_damage_t;

typedef enum link_damage {
	LINK_SOUND,
	LINK_DANGLING,
	LINK_ZERO_REFERENCE,
	LINK_COUNT_LOW,
	LINK_COUNT_HIGH,
	LINK_UNREFERENCED_INODE,
	LINK_NONEMPTY_STUB,
	LINK_HIDDEN_NONEMPTY_STUB,
	LINK_HIDDEN_STUB,
	LINK_THREE_STUBS
} link_damage_t;

typedef enum attribute_damage {
	ATTRIBUTE_SOUND,
	ATTRIBUTE_CONTINUATION_GAP,
	ATTRIBUTE_CONTINUATION_SHORT
} attribute_damage_t;

typedef enum compression_sibling {
	COMPRESSION_EMPTY_SIBLING,
	COMPRESSION_PLAIN_SIBLING,
	COMPRESSION_INLINE_SIBLING
} compression_sibling_t;

static void expect(bool condition, const char *description);
static void expect_status(fsimg_status_t actual, fsimg_status_t wanted,
    const fsimg_error_t *error);
static unsigned char *header(gen_image_t *image);
static unsigned char *catalog_data(gen_image_t *image, uint32_t id, bool thread);
static void build_clean_variant(gen_image_t *image, bool hfsx, bool journal);
static void build_clean(gen_image_t *image);
static void build_empty(gen_image_t *image);
static fsimg_status_t memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error);
static fsimg_status_t memory_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED);
static bool cancelled(void *argument);
static fsimg_status_t fixture_open(fixture_t *fixture, gen_image_t *image, uint64_t scratch,
    uint64_t missing, fsimg_error_t *error);
static void fixture_close(fixture_t *fixture);
static void check_failure(fixture_t *fixture, unsigned flags, fsimg_status_t wanted,
    const char *message);
static void check_clean(gen_image_t *image, uint64_t files, uint64_t directories,
    uint64_t bytes, uint64_t bad_blocks);
static void next_id_below_used(gen_image_t *image);
static void alternate_field(gen_image_t *image, unsigned offset);
static void alternate_signature(gen_image_t *image);
static void alternate_version(gen_image_t *image);
static void alternate_block_size(gen_image_t *image);
static void alternate_total_blocks(gen_image_t *image);
static void alternate_bitmap(gen_image_t *image);
static void alternate_extents(gen_image_t *image);
static void alternate_catalog(gen_image_t *image);
static void alternate_attributes(gen_image_t *image);
static void alternate_startup(gen_image_t *image);
static void alternate_zero(gen_image_t *image);
static void referenced_clear_block(gen_image_t *image);
static void allocated_orphan(gen_image_t *image);
static void shared_block(gen_image_t *image);
static void bitmap_short(gen_image_t *image);
static void free_blocks_delta(gen_image_t *image, int delta);
static void free_blocks_low(gen_image_t *image);
static void free_blocks_high(gen_image_t *image);
static void listed_empty_fork_stray_extent(gen_image_t *image);
static void private_empty_fork_stray_extent(gen_image_t *image);
static void fork_mutation(gen_image_t *image, unsigned offset, uint64_t value, size_t width);
static void extent_zero_count(gen_image_t *image);
static void extent_after_unused(gen_image_t *image);
static void extent_reserved(gen_image_t *image);
static void extent_past_volume(gen_image_t *image);
static void extent_across_tail(gen_image_t *image);
static void logical_above_allocation(gen_image_t *image);
static void wrong_thread_parent(gen_image_t *image);
static void wrong_thread_name(gen_image_t *image);
static void wrong_thread_type(gen_image_t *image);
static void wrong_valence(gen_image_t *image);
static void wrong_folder_count(gen_image_t *image);
static void header_file_count(gen_image_t *image);
static void header_folder_count(gen_image_t *image);
static void attribute_flag_clear(gen_image_t *image);
static void attribute_flag_without_record(gen_image_t *image);
static void attribute_owner_above_every_primary(gen_image_t *image);
static void attribute_unknown_type(gen_image_t *image);
static void absent_label(gen_image_t *image);
static void private_directory_nonempty(gen_image_t *image);
static void refused_entry_name(gen_image_t *image);
static void build_deep_tree(gen_image_t *image);
static unsigned char *extent_node(gen_image_t *image, uint32_t number);
static unsigned char *node_record(unsigned char *node, uint16_t size, unsigned record);
static void extent_keys_fault(gen_image_t *image, bool siblings, bool descending);
static void equal_keys_within(gen_image_t *image);
static void descending_keys_within(gen_image_t *image);
static void equal_keys_across(gen_image_t *image);
static void descending_keys_across(gen_image_t *image);
static void index_key_differs(gen_image_t *image);
static void index_child_fault(gen_image_t *image, unsigned which);
static void child_wrong_height(gen_image_t *image);
static void child_is_header(gen_image_t *image);
static void child_is_self(gen_image_t *image);
static void sibling_fault(gen_image_t *image, bool index, bool cycle);
static void leaf_sibling_cycle_fails_back_link(gen_image_t *image);
static void leaf_backlink(gen_image_t *image);
static void index_sibling_cycle_fails_back_link(gen_image_t *image);
static void index_backlink(gen_image_t *image);
static void tree_header_delta(gen_image_t *image, unsigned offset, size_t width);
static void leaf_records_off(gen_image_t *image);
static void first_leaf_off(gen_image_t *image);
static void last_leaf_off(gen_image_t *image);
static void tree_depth_off(gen_image_t *image);
static void free_nodes_off(gen_image_t *image);
static void unmarked_used_node(gen_image_t *image);
static void map_bit_past_total(gen_image_t *image);
static void build_map_chain(gen_image_t *image, unsigned maps);
static void marked_free_node(gen_image_t *image);
static void map_chain_loop(gen_image_t *image);
static void map_chain_short(gen_image_t *image);
static void bitmap_padding_set(gen_image_t *image);
static void overflow_start_fault(gen_image_t *image, uint32_t start);
static void overflow_gap(gen_image_t *image);
static void overflow_overlap(gen_image_t *image);
static void bad_blocks_gap(gen_image_t *image);
static void bad_blocks_resource_orphan(gen_image_t *image);
static void build_slack_tail(gen_image_t *image, uint32_t block_size, bool marked);
static void tail_orphan_4096(gen_image_t *image);
static void tail_orphan_512(gen_image_t *image);
static void missing_thread(gen_image_t *image);
static void duplicate_thread(gen_image_t *image);
static void thread_without_primary(gen_image_t *image);
static void build_parent_fault(gen_image_t *image, uint32_t parent);
static void parent_missing(gen_image_t *image);
static void parent_is_file(gen_image_t *image);
static void disconnected_cycle(gen_image_t *image);
static void folder_second_parent(gen_image_t *image);
static void duplicate_node_id(gen_image_t *image);
static void private_folder_child(gen_image_t *image);
static void build_reserved_id(gen_image_t *image, uint32_t id);
static void reserved_id_9(gen_image_t *image);
static void reserved_id_10(gen_image_t *image);
static void reserved_id_11(gen_image_t *image);
static void reserved_id_12(gen_image_t *image);
static void reserved_id_13(gen_image_t *image);
static void reserved_id_14(gen_image_t *image);
static void reserved_id_15(gen_image_t *image);
static void repeat_overflow(gen_image_t *image);
static void extra_overflow(gen_image_t *image);
static void build_attribute_fork(gen_image_t *image, unsigned extents, attribute_damage_t fault);
static void attribute_continuation_gap(gen_image_t *image);
static void attribute_continuation_short(gen_image_t *image);
static void build_links(gen_image_t *image, link_damage_t fault);
static void link_dangling(gen_image_t *image);
static void link_zero_reference(gen_image_t *image);
static void link_count_low(gen_image_t *image);
static void link_count_high(gen_image_t *image);
static void link_unreferenced_inode(gen_image_t *image);
static void link_nonempty_stub(gen_image_t *image);
static void store_le(unsigned char *bytes, uint64_t value, size_t width);
static void build_compressed_variant(gen_image_t *image, unsigned type, compression_damage_t fault,
    compression_sibling_t sibling);
static void build_compressed(gen_image_t *image, unsigned type, compression_damage_t fault);
static void compressed4_outside(gen_image_t *image);
static void compressed4_overlap(gen_image_t *image);
static void compressed4_descending(gen_image_t *image);
static void compressed8_outside(gen_image_t *image);
static void compressed8_overlap(gen_image_t *image);
static void compressed8_descending(gen_image_t *image);
static void compressed12_outside(gen_image_t *image);
static void compressed12_overlap(gen_image_t *image);
static void compressed12_descending(gen_image_t *image);
static void test_compression_scopes(void);
static void test_compressed_only_payload_counts_no_initialized_bytes(void);
static void test_clean_variants_and_reused_ids(void);
static void test_additional_acceptance(void);
static void test_corruption_table(void);
static void test_stray_empty_forks_do_not_prevent_listing_or_reading(void);
static void test_preflight_refuses_before_scanning(void);
static void test_alternate_read_failure_keeps_io_status(void);
static void test_truncation_precedes_driver_work(void);
static void test_resource_and_cancellation_statuses(void);
static int write_fixture(const char *name, const char *path);

static unsigned failures;
static const char *scenario;
static const uint16_t name_a[] = { 'a' };
static const uint16_t name_b[] = { 'b' };
static const uint16_t name_c[] = { 'c' };
static const uint16_t private_data[] = { 0, 0, 0, 0, 'H', 'F', 'S', '+',
    ' ', 'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'a', 't', 'a' };
static const uint16_t private_directory[] = { '.', 'H', 'F', 'S', '+',
    ' ', 'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'i', 'r', 'e',
    'c', 't', 'o', 'r', 'y', ' ', 'D', 'a', 't', 'a', '\r' };

static void
expect(bool condition, const char *description)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", scenario, description);
		failures++;
	}
}

static void
expect_status(fsimg_status_t actual, fsimg_status_t wanted,
    const fsimg_error_t *error)
{
	if (actual != wanted) {
		fprintf(stderr, "%s: expected %s, got %s (%s)\n", scenario,
		    fsimg_status_string(wanted), fsimg_status_string(actual),
		    error->message);
		failures++;
	}
}

static unsigned char *
header(gen_image_t *image)
{
	return (image->bytes + image->header);
}

static unsigned char *
catalog_data(gen_image_t *image, uint32_t id, bool thread)
{
	gen_tree_t *tree = &image->trees[GEN_CATALOG];

	for (size_t i = 0; i < tree->record_count; i++) {
		gen_record_t *r = &tree->records[i];
		unsigned char *key = image->bytes + r->offset;
		unsigned char *data = key + r->key_length;
		uint64_t type = gen_load(data, 2);

		if (thread && type >= 3 && gen_load(key + 2, 4) == id)
			return (data);
		if (!thread && type <= 2 && gen_load(data + 8, 4) == id)
			return (data);
	}
	CHECK(false);
	return (NULL);
}

/* Two files, one folder, one attribute, a fragmented nine-block fork, and
 * two bad blocks. All counts below come from this declared tree.
 */
static void
build_clean_variant(gen_image_t *image, bool hfsx, bool journal)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2,
		    .data_size = 9 * 4096, .fragmented = true, .flags = 4 },
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_c, .units = 1, .id = 34, .parent = 33,
		    .type = 2, .data = (const unsigned char *)"abc", .data_size = 3 }
	};
	gen_attr_value_t value = { .id = 32, .type = 0x10,
	    .name = name_a, .units = 1 };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.hfsx = hfsx;
	plan.compare_type = hfsx ? 0xbc : 0xcf;
	plan.journal = journal;
	plan.journal_info_file = journal;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	plan.attributes = true;
	plan.values = &value;
	plan.value_count = 1;
	plan.bad_blocks = 2;
	gen_build(&plan, image);
}

static void
build_clean(gen_image_t *image)
{
	build_clean_variant(image, false, false);
}

static void
build_empty(gen_image_t *image)
{
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	gen_build(&plan, image);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *fixture = argument;
	uint64_t bitmap = gen_load(header(fixture->image) +
	    GEN_BITMAP_FORK + 16, 4) * fixture->image->block_size;

	fixture->reads++;
	if (fixture->watched_end > offset &&
	    fixture->watched_first < offset + length)
		fixture->watched_reads++;
	*transferred = 0;
	if (fixture->fail_offset >= offset &&
	    fixture->fail_offset - offset < length)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected alternate read failure"));
	if (offset <= bitmap && bitmap - offset < length)
		fixture->bitmap_reads++;
	if (offset >= fixture->length)
		return (FSIMG_OK);
	if (length > fixture->length - offset)
		length = (size_t)(fixture->length - offset);
	if (fixture->watched_end > offset &&
	    fixture->watched_first < offset + length) {
		uint64_t first = offset > fixture->watched_first ?
		    offset : fixture->watched_first;
		uint64_t end = offset + length < fixture->watched_end ?
		    offset + length : fixture->watched_end;

		fixture->watched_bytes += end - first;
	}
	bcopy(fixture->image->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static bool
cancelled(void *argument)
{
	fixture_t *fixture = argument;

	fixture->polls++;
	return (fixture->cancel_at != 0 && fixture->polls >= fixture->cancel_at);
}

static fsimg_status_t
fixture_open(fixture_t *fixture, gen_image_t *image, uint64_t scratch,
    uint64_t missing, fsimg_error_t *error)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_status_t status;

	bzero(fixture, sizeof(*fixture));
	fixture->image = image;
	fixture->length = image->length - missing;
	fixture->fail_offset = UINT64_MAX;
	strcpy(fixture->scratch_path, "/tmp/fsimg-verify-XXXXXX");
	CHECK(mkdtemp(fixture->scratch_path) != NULL);
	fixture->scratch_fd = open(fixture->scratch_path,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->scratch_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = fixture->scratch_fd;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, error) == FSIMG_OK);
	if (scratch != 0)
		CHECK(fsimg_test_meta_cache(fixture->ctx, 2, error) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(fixture->ctx, &ops, fixture,
	    &fixture->io, error) == FSIMG_OK);
	status = fsimg_reader_open(fixture->ctx, fixture->io,
	    NULL, &fixture->reader, error);

	/* Reserve the open's live scratch, then give verification this headroom. */
	if (status == FSIMG_OK && scratch != 0)
		fixture->ctx->options.scratch_bytes =
		    fixture->ctx->stats.scratch_bytes + scratch;
	return (status);
}

static void
fixture_close(fixture_t *fixture)
{
	fsimg_reader_free(fixture->reader);
	fsimg_io_free(fixture->io);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->scratch_fd) == 0);
	CHECK(rmdir(fixture->scratch_path) == 0);
}

static void
check_failure(fixture_t *fixture, unsigned flags, fsimg_status_t wanted,
    const char *message)
{
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	status = fsimg_verify(fixture->reader, flags, &report, &error);
	expect_status(status, wanted, &error);
	/* Full diagnostics compare exactly; short expectations match fragments. */
	if ((strncmp(message, "HFS Plus ", 9) == 0 ?
	    strcmp(error.message, message) != 0 :
	    strstr(error.message, message) == NULL)) {
		fprintf(stderr, "%s: expected message '%s', got '%s'\n",
		    scenario, message, error.message);
		failures++;
	}
	expect(!report.complete, "a failed report must be incomplete");
	/* Failed readers refuse get_info; inspect the retained info record. */
	expect((fixture->reader->info.valid & FSIMG_VOLUME_FREE) == 0,
	    "failure must not publish free space");
	expect_status(fsimg_verify(fixture->reader, flags, &report, &error),
	    FSIMG_ESTATE, &error);
	expect(!report.complete, "a repeated verification stays incomplete");
}

static void
check_clean_bytes(gen_image_t *image, uint64_t files, uint64_t directories,
    uint64_t bytes, uint64_t initialized_bytes, uint64_t bad_blocks)
{
	for (unsigned payload = 0; payload < 2; payload++) {
		fixture_t fixture;
		fsimg_verify_report_t report;
		fsimg_volume_info_t info;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;

		status = fixture_open(&fixture, image, 0, 0, &error);
		expect_status(status, FSIMG_OK, &error);
		if (status == FSIMG_OK) {
			expect((fixture.reader->info.valid & FSIMG_VOLUME_FREE) == 0,
			    "free space is unknown before verification");
			status = fsimg_verify(fixture.reader,
			    payload ? FSIMG_VERIFY_PAYLOAD : 0, &report, &error);
			expect_status(status, FSIMG_OK, &error);
			expect(report.complete, "clean report is complete");
			expect(report.scope == (payload ? FSIMG_VALIDATION_PAYLOAD :
			    FSIMG_VALIDATION_STRUCTURAL), "report names requested scope");
			expect(report.files == files, "exact file count");
			expect(report.directories == directories, "exact directory count");
			expect(report.logical_bytes == bytes, "exact logical byte count");
			if (report.payload_bytes_read != (payload ? initialized_bytes : 0)) {
				fprintf(stderr, "%s: got %s (%s), payload_bytes_read=%llu, expected %llu\n",
				    scenario, fsimg_status_string(status), error.message,
				    (unsigned long long)report.payload_bytes_read,
				    (unsigned long long)(payload ? initialized_bytes : 0));
				failures++;
			}
			expect(report.free_clusters == gen_load(header(image) + 48, 4),
			    "exact free-block count");
			expect(report.allocated_clusters ==
			    gen_load(header(image) + 44, 4) -
			    gen_load(header(image) + 48, 4) - bad_blocks,
			    "exact allocated-block count including formatter tail");
			expect(report.bad_clusters == bad_blocks, "exact bad-block count");
			expect(report.allocated_clusters + report.free_clusters +
			    report.bad_clusters == gen_load(header(image) + 44, 4),
			    "allocation accounts for every volume block");
			if (status == FSIMG_OK) {
				CHECK(fsimg_reader_get_info(fixture.reader, &info,
				    &error) == FSIMG_OK);
				expect((info.valid & FSIMG_VOLUME_FREE) != 0,
				    "success publishes free space");
				expect(info.free_bytes == report.free_clusters *
				    image->block_size, "published free space agrees");
			}
		}
		fixture_close(&fixture);
	}
}

static void
check_clean(gen_image_t *image, uint64_t files, uint64_t directories,
    uint64_t bytes, uint64_t bad_blocks)
{
	check_clean_bytes(image, files, directories, bytes, bytes, bad_blocks);
}

/* Each builder starts with its own volume; no mutation is shared by cases. */
static void
next_id_below_used(gen_image_t *image)
{
	build_clean(image);
	gen_store(header(image) + 64, 32, 4);
}

static void
alternate_field(gen_image_t *image, unsigned offset)
{
	build_clean(image);
	image->bytes[image->length - 1024 + offset] ^= 1;
}

static void
alternate_signature(gen_image_t *image)
{
	alternate_field(image, 1);
}
static void
alternate_version(gen_image_t *image)
{
	alternate_field(image, 3);
}
static void
alternate_block_size(gen_image_t *image)
{
	alternate_field(image, 43);
}
static void
alternate_total_blocks(gen_image_t *image)
{
	alternate_field(image, 47);
}
static void
alternate_bitmap(gen_image_t *image)
{
	alternate_field(image, GEN_BITMAP_FORK + 7);
}
static void
alternate_extents(gen_image_t *image)
{
	alternate_field(image, GEN_EXTENTS_FORK + 7);
}
static void
alternate_catalog(gen_image_t *image)
{
	alternate_field(image, GEN_CATALOG_FORK + 7);
}
static void
alternate_attributes(gen_image_t *image)
{
	alternate_field(image, GEN_ATTRIBUTES_FORK + 7);
}
static void
alternate_startup(gen_image_t *image)
{
	alternate_field(image, GEN_STARTUP_FORK + 7);
}

static void
alternate_zero(gen_image_t *image)
{
	build_clean(image);
	bzero(image->bytes + image->length - 1024, 512);
}

static void
referenced_clear_block(gen_image_t *image)
{
	uint64_t bitmap;
	uint32_t block;

	build_clean(image);
	bitmap = gen_load(header(image) + GEN_BITMAP_FORK + 16, 4) * image->block_size;
	block = (uint32_t)gen_load(catalog_data(image, 32, false) + 104, 4);
	image->bytes[bitmap + block / 8] &= (unsigned char)~(0x80u >> (block % 8));
}

static void
allocated_orphan(gen_image_t *image)
{
	uint64_t bitmap;

	build_clean(image);
	bitmap = gen_load(header(image) + GEN_BITMAP_FORK + 16, 4) * image->block_size;
	image->bytes[bitmap + 400 / 8] |= 0x80;
}

static void
shared_block(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, false) + 104,
	    gen_load(catalog_data(image, 32, false) + 104, 4), 4);
	gen_rebuild_bitmap(image);
	bcopy(header(image), image->bytes + image->length - 1024, 512);
}

static void
bitmap_short(gen_image_t *image)
{
	build_clean(image);
	gen_store(header(image) + GEN_BITMAP_FORK, 63, 8);
}

static void
free_blocks_delta(gen_image_t *image, int delta)
{
	build_clean(image);
	gen_store(header(image) + 48, gen_load(header(image) + 48, 4) + delta, 4);
}

static void
free_blocks_low(gen_image_t *image)
{
	free_blocks_delta(image, -1);
}
static void
free_blocks_high(gen_image_t *image)
{
	free_blocks_delta(image, 1);
}

static void
listed_empty_fork_stray_extent(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, false) + 168 + 16, 100, 4);
	gen_store(catalog_data(image, 34, false) + 168 + 20, 1, 4);
}

static void
private_empty_fork_stray_extent(gen_image_t *image)
{
	gen_object_t objects[] = {
		{ .name = private_data, .units = COUNT(private_data), .id = 40, .type = 1 },
		{ .name = name_a, .units = 1, .id = 41, .parent = 40, .type = 2 }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
	gen_store(catalog_data(image, 41, false) + 104, 100, 4);
	gen_store(catalog_data(image, 41, false) + 108, 1, 4);
}

/* The generator supplies threads, valence, header counts and fork allocation.
 * The optional stub lives in a listed folder and shares the private folder's
 * creation date, as required for a hard-link reference.
 */
static void
build_hidden_record(gen_image_t *image, gen_object_t hidden,
    const gen_attr_value_t *attribute, bool listed_stub)
{
	static const uint16_t temporary[] = { 't', 'e', 'm', 'p', '1', '0', '0' };
	gen_object_t objects[] = {
		{ .name = private_data, .units = COUNT(private_data),
		    .id = 40, .type = 1, .dates = { 42 } },
		hidden,
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_a, .units = 1, .id = 32, .parent = 33,
		    .type = 2, .special = 7, .finder = "hlnkhfs+",
		    .dates = { 42 } }
	};
	gen_plan_t plan;

	if (objects[1].name == NULL) {
		objects[1].name = temporary;
		objects[1].units = COUNT(temporary);
	}
	objects[1].id = 41;
	objects[1].parent = 40;
	objects[1].type = 2;
	objects[1].flags = attribute == NULL ? 0 : 0x0004;
	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = listed_stub ? COUNT(objects) : 2;
	plan.attributes = attribute != NULL;
	plan.values = attribute;
	plan.value_count = attribute == NULL ? 0 : 1;
	gen_build(&plan, image);
}

static void
hidden_socket_data(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0140000, .data_size = 1 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_character_resource(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0020000, .resource_size = 1 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_block_data(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0060000, .data_size = 1 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_symlink_resource(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0120000,
	    .data = (const unsigned char *)"hello", .data_size = 5,
	    .resource_size = 1 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_symlink_empty(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0120000 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_symlink_1024(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0120000, .data_size = 1024 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_compressed_no_attribute(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0100000, .owner_flags = 0x20 };

	build_hidden_record(image, hidden, NULL, false);
}

/* Type 3 stores the three uncompressed bytes inline after its marker. */
static void
build_hidden_compressed(gen_image_t *image, gen_object_t hidden,
    unsigned type, bool bad_signature, bool listed_stub)
{
	static const uint16_t compression_name[] = { 'c', 'o', 'm', '.',
	    'a', 'p', 'p', 'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };
	unsigned char bytes[20] = { 'f', 'p', 'm', 'c' };
	gen_attr_value_t attribute = { .id = 41, .type = 0x10,
	    .name = compression_name, .units = COUNT(compression_name),
	    .value = bytes, .size = type == 3 ? sizeof(bytes) : 16 };

	store_le(bytes + 4, type, 4);
	store_le(bytes + 8, 3, 8);
	bytes[16] = 0xff;
	bcopy("abc", bytes + 17, 3);
	if (bad_signature)
		bytes[0] = 0;
	hidden.mode = 0100000;
	hidden.owner_flags = 0x20;
	build_hidden_record(image, hidden, &attribute, listed_stub);
}

static void
hidden_compressed_bad_signature(gen_image_t *image)
{
	gen_object_t hidden = { 0 };

	build_hidden_compressed(image, hidden, 3, true, false);
}

static void
hidden_compressed_data(gen_image_t *image)
{
	gen_object_t hidden = { .data_size = 1 };

	build_hidden_compressed(image, hidden, 3, false, false);
}

static void
hidden_compressed_unknown_type(gen_image_t *image)
{
	gen_object_t hidden = { 0 };

	build_hidden_compressed(image, hidden, 255, false, false);
}

static void
hidden_fifo_data(gen_image_t *image)
{
	gen_object_t hidden = { .mode = 0010000, .data_size = 1 };

	build_hidden_record(image, hidden, NULL, false);
}

static const uint16_t hidden_inode_name[] = { 'i', 'N', 'o', 'd', 'e', '7' };

static void
hidden_inode_fifo(gen_image_t *image)
{
	gen_object_t hidden = { .name = hidden_inode_name,
	    .units = COUNT(hidden_inode_name), .mode = 0010000 };

	build_hidden_record(image, hidden, NULL, false);
}

static void
hidden_inode_compressed_no_attribute(gen_image_t *image)
{
	gen_object_t hidden = { .name = hidden_inode_name,
	    .units = COUNT(hidden_inode_name), .mode = 0100000,
	    .owner_flags = 0x20 };

	build_hidden_record(image, hidden, NULL, false);
}

/* A three-byte type-8 file needs two offset words. A first offset of four
 * places the end of its table inside those words, before any chunk decoding.
 */
static void
build_hidden_inode_bad_table(gen_image_t *image, bool listed_stub)
{
	unsigned char resource[12] = { 0 };
	gen_object_t hidden = { .name = hidden_inode_name,
	    .units = COUNT(hidden_inode_name), .special = listed_stub ? 1 : 0,
	    .resource = resource, .resource_size = sizeof(resource) };

	store_le(resource, 4, 4);
	store_le(resource + 4, sizeof(resource), 4);
	resource[8] = 6;
	bcopy("abc", resource + 9, 3);
	build_hidden_compressed(image, hidden, 8, false, listed_stub);
}

static void
hidden_inode_bad_table_referenced(gen_image_t *image)
{
	build_hidden_inode_bad_table(image, true);
}

static void
hidden_inode_bad_table_unreferenced(gen_image_t *image)
{
	build_hidden_inode_bad_table(image, false);
}

static void
hidden_inode_compressed_valid(gen_image_t *image)
{
	gen_object_t hidden = { .name = hidden_inode_name,
	    .units = COUNT(hidden_inode_name), .special = 1 };

	build_hidden_compressed(image, hidden, 3, false, true);
}

static void
test_hidden_primary_acceptance(void)
{
	gen_image_t image;

	scenario = "hidden_fifo_data";
	hidden_fifo_data(&image);
	check_clean(&image, 0, 1, 0, 0);
	gen_free(&image);

	scenario = "hidden_inode_bad_table_unreferenced";
	hidden_inode_bad_table_unreferenced(&image);
	check_clean(&image, 0, 1, 0, 0);
	gen_free(&image);

	scenario = "hidden_inode_compressed_valid";
	hidden_inode_compressed_valid(&image);
	check_clean_bytes(&image, 1, 2, 3, 0, 0);
	gen_free(&image);
}

static void
fork_mutation(gen_image_t *image, unsigned offset, uint64_t value, size_t width)
{
	build_clean(image);
	gen_store(catalog_data(image, 32, false) + 88 + offset, value, width);
}

static void
extent_zero_count(gen_image_t *image)
{
	fork_mutation(image, 20, 0, 4);
}
static void
extent_after_unused(gen_image_t *image)
{
	fork_mutation(image, 16, 0, 8);
}
static void
extent_reserved(gen_image_t *image)
{
	fork_mutation(image, 16, 0, 4);
}
static void
extent_past_volume(gen_image_t *image)
{
	fork_mutation(image, 16, 512, 4);
}
static void
extent_across_tail(gen_image_t *image)
{
	fork_mutation(image, 16, 511, 4);
}
static void
logical_above_allocation(gen_image_t *image)
{
	fork_mutation(image, 0, 10 * 4096, 8);
}

static void
wrong_thread_parent(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, true) + 4, 2, 4);
}

static void
wrong_thread_name(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, true) + 10, 'd', 2);
}

static void
wrong_thread_type(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, true), 3, 2);
}

static void
wrong_valence(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 33, false) + 4, 2, 4);
}

static void
wrong_folder_count(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 33, false) + 84, 1, 4);
	gen_store(catalog_data(image, 33, false) + 2, 0x10, 2);
}

static void
header_file_count(gen_image_t *image)
{
	build_clean(image);
	gen_store(header(image) + 32, 3, 4);
}
static void
header_folder_count(gen_image_t *image)
{
	build_clean(image);
	gen_store(header(image) + 36, 2, 4);
}
static void
attribute_flag_clear(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 32, false) + 2, 0, 2);
}
static void
attribute_flag_without_record(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, false) + 2, 4, 2);
}

/* A single attribute keeps key order valid for every owner ID. Clearing the
 * former owner's flag leaves only the unmatched owner as the fault.
 */
static void
build_attribute_owner(gen_image_t *image, uint32_t owner)
{
	gen_record_t *r;

	build_clean(image);
	gen_store(catalog_data(image, 32, false) + 2, 0, 2);
	r = &image->trees[GEN_ATTRIBUTES].records[0];
	gen_store(image->bytes + r->offset + 4, owner, 4);
}

static void
attribute_owner_id_zero(gen_image_t *image)
{
	build_attribute_owner(image, 0);
}

static void
attribute_owner_between_primaries(gen_image_t *image)
{
	build_attribute_owner(image, 31);
}

static void
attribute_owner_above_every_primary(gen_image_t *image)
{
	build_attribute_owner(image, 99);
}

static void
attribute_unknown_type(gen_image_t *image)
{
	gen_record_t *r;

	build_clean(image);
	r = &image->trees[GEN_ATTRIBUTES].records[0];
	gen_store(image->bytes + r->offset + r->key_length, 0x40, 4);
}

static void
absent_label(gen_image_t *image)
{
	static const uint16_t refused[] = { 0x00e9 };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.label = refused;
	plan.label_units = COUNT(refused);
	gen_build(&plan, image);
}

static void
private_directory_nonempty(gen_image_t *image)
{
	gen_object_t objects[] = {
		{ .name = private_directory, .units = COUNT(private_directory),
		    .id = 40, .type = 1 },
		{ .name = name_a, .units = 1, .id = 41, .parent = 40, .type = 2 }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
}

static void
refused_entry_name(gen_image_t *image)
{
	static const uint16_t refused[] = { 0x00e9 };
	gen_object_t object = { .name = refused, .units = COUNT(refused),
	    .id = 32, .type = 2 };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, image);
}

/* A narrow fanout forces both leaf and index sibling levels. Extents keys
 * have fixed size, so mutations below cannot change record framing.
 */
static void
build_deep_tree(gen_image_t *image)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data_size = 65 * 4096, .fragmented = true };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	plan.records_per_node = 2;
	plan.bad_blocks = 2;
	gen_build(&plan, image);
}

static unsigned char *
extent_node(gen_image_t *image, uint32_t number)
{
	return (image->bytes + gen_node_offset(image, GEN_EXTENTS, number));
}

static unsigned char *
node_record(unsigned char *node, uint16_t size, unsigned record)
{
	return (node + gen_load(node + size - 2 * (record + 1), 2));
}

static void
extent_keys_fault(gen_image_t *image, bool siblings, bool descending)
{
	gen_tree_t *tree;
	unsigned char *left, *right;

	build_deep_tree(image);
	tree = &image->trees[GEN_EXTENTS];
	left = extent_node(image, tree->first_leaf);
	if (siblings) {
		right = extent_node(image, (uint32_t)gen_load(left, 4));
		left = node_record(left, tree->node_size, 1);
		right = node_record(right, tree->node_size, 0);
	} else {
		right = node_record(left, tree->node_size, 1);
		left = node_record(left, tree->node_size, 0);
	}
	bcopy(left, right, 12);
	if (descending)
		gen_store(right + 4, gen_load(left + 4, 4) - 1, 4);
}

static void
equal_keys_within(gen_image_t *image)
{
	extent_keys_fault(image, false, false);
}
static void
descending_keys_within(gen_image_t *image)
{
	extent_keys_fault(image, false, true);
}
static void
equal_keys_across(gen_image_t *image)
{
	extent_keys_fault(image, true, false);
}
static void
descending_keys_across(gen_image_t *image)
{
	extent_keys_fault(image, true, true);
}

static void
index_key_differs(gen_image_t *image)
{
	unsigned char *root;

	build_deep_tree(image);
	root = extent_node(image, image->trees[GEN_EXTENTS].root);
	gen_store(node_record(root, 512, 0) + 8, 1, 4);
}

static void
index_child_fault(gen_image_t *image, unsigned which)
{
	gen_tree_t *tree;
	unsigned char *root, *record;
	uint32_t child;

	build_deep_tree(image);
	tree = &image->trees[GEN_EXTENTS];
	root = extent_node(image, tree->root);
	record = node_record(root, 512, 0);
	child = which == 0 ? tree->first_leaf : (which == 1 ? 0 : tree->root);
	gen_store(record + 12, child, 4);
}

static void
child_wrong_height(gen_image_t *image)
{
	index_child_fault(image, 0);
}
static void
child_is_header(gen_image_t *image)
{
	index_child_fault(image, 1);
}
static void
child_is_self(gen_image_t *image)
{
	index_child_fault(image, 2);
}

static void
sibling_fault(gen_image_t *image, bool index, bool cycle)
{
	gen_tree_t *tree;
	unsigned char *first, *next;
	uint32_t number;

	build_deep_tree(image);
	tree = &image->trees[GEN_EXTENTS];
	number = tree->first_leaf;
	if (index) {
		first = extent_node(image, tree->root);
		number = (uint32_t)gen_load(node_record(first, 512, 0) + 12, 4);
	}
	first = extent_node(image, number);
	next = extent_node(image, (uint32_t)gen_load(first, 4));
	if (cycle)
		gen_store(next, number, 4);
	else
		gen_store(next + 4, 0, 4);
}

static void
leaf_sibling_cycle_fails_back_link(gen_image_t *image)
{
	sibling_fault(image, false, true);
}
static void
leaf_backlink(gen_image_t *image)
{
	sibling_fault(image, false, false);
}
static void
index_sibling_cycle_fails_back_link(gen_image_t *image)
{
	sibling_fault(image, true, true);
}
static void
index_backlink(gen_image_t *image)
{
	sibling_fault(image, true, false);
}

static void
tree_header_delta(gen_image_t *image, unsigned offset, size_t width)
{
	unsigned char *tree_header;

	build_deep_tree(image);
	tree_header = extent_node(image, 0) + 14;
	gen_store(tree_header + offset, gen_load(tree_header + offset, width) + 1, width);
}

static void
leaf_records_off(gen_image_t *image)
{
	tree_header_delta(image, 6, 4);
}
static void
first_leaf_off(gen_image_t *image)
{
	tree_header_delta(image, 10, 4);
}
static void
last_leaf_off(gen_image_t *image)
{
	tree_header_delta(image, 14, 4);
}
static void
tree_depth_off(gen_image_t *image)
{
	tree_header_delta(image, 0, 2);
}
static void
free_nodes_off(gen_image_t *image)
{
	tree_header_delta(image, 26, 4);
}

static void
unmarked_used_node(gen_image_t *image)
{
	build_deep_tree(image);
	extent_node(image, 0)[248] &= 0xbf;
}

static void
map_bit_past_total(gen_image_t *image)
{
	uint32_t total;

	build_deep_tree(image);
	total = image->trees[GEN_EXTENTS].nodes;
	extent_node(image, 0)[248 + total / 8] |= (unsigned char)(0x80u >> (total % 8));
}

static void
map_link_past_total_nodes_extents(gen_image_t *image)
{
	build_clean(image);
	gen_store(image->bytes + image->trees[GEN_EXTENTS].offset, UINT32_MAX, 4);
}

static void
map_link_past_total_nodes_catalog(gen_image_t *image)
{
	build_clean(image);
	gen_store(image->bytes + image->trees[GEN_CATALOG].offset, UINT32_MAX, 4);
}

static void
map_link_past_total_nodes_attributes(gen_image_t *image)
{
	build_clean(image);
	gen_store(image->bytes + image->trees[GEN_ATTRIBUTES].offset, UINT32_MAX, 4);
}

/* The fixed header map covers 2048 nodes. Full map nodes add 3936 bits
 * each; the final eight nodes of the two-map fixture need its second map.
 */
static void
build_map_chain(gen_image_t *image, unsigned maps)
{
	gen_plan_t plan;
	unsigned char *fork, *base, *node;
	uint32_t nodes = maps == 0 ? 2048 : (maps == 1 ? 2056 : 5992);
	uint32_t blocks = (nodes + 7) / 8;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.total_blocks = 2048;
	gen_build(&plan, image);
	fork = header(image) + GEN_EXTENTS_FORK;
	base = image->bytes + 200 * 4096;
	bcopy(image->bytes + image->trees[GEN_EXTENTS].offset, base, 512);
	gen_store(fork, nodes * 512, 8);
	gen_store(fork + 12, blocks, 4);
	gen_store(fork + 16, 200, 4);
	gen_store(fork + 20, blocks, 4);
	image->trees[GEN_EXTENTS].offset = 200 * 4096;
	image->trees[GEN_EXTENTS].nodes = nodes;
	gen_store(base, maps == 0 ? 0 : 1, 4);
	gen_store(base + 36, nodes, 4);
	gen_store(base + 40, nodes - maps - 1, 4);
	bzero(base + 248, 256);
	base[248] = (unsigned char)(0xffu << (7 - maps));
	for (unsigned i = 1; i <= maps; i++) {
		node = base + i * 512;
		gen_store(node, i == maps ? 0 : i + 1, 4);
		node[8] = 2;
		gen_store(node + 10, 1, 2);
		gen_store(node + 510, 14, 2);
		gen_store(node + 508, 506, 2);
	}
	gen_rebuild_bitmap(image);
	bcopy(header(image), image->bytes + image->length - 1024, 512);
}

static void
marked_free_node(gen_image_t *image)
{
	build_map_chain(image, 1);
	extent_node(image, 0)[248] |= 0x20;
}

static void
map_chain_loop(gen_image_t *image)
{
	build_map_chain(image, 2);
	gen_store(extent_node(image, 2), 1, 4);
}

static void
map_chain_short(gen_image_t *image)
{
	build_map_chain(image, 2);
	gen_store(extent_node(image, 1), 0, 4);
	/* The disconnected node is free, isolating insufficient map coverage. */
	extent_node(image, 0)[248] &= 0xdf;
	gen_store(extent_node(image, 0) + 40, 5990, 4);
}

static void
bitmap_padding_set(gen_image_t *image)
{
	gen_plan_t plan;
	uint64_t bitmap;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.total_blocks = 511;
	gen_build(&plan, image);
	bitmap = gen_load(header(image) + GEN_BITMAP_FORK + 16, 4) * 4096;
	image->bytes[bitmap + 63] |= 1;
}

static void
overflow_start_fault(gen_image_t *image, uint32_t start)
{
	gen_tree_t *tree;
	gen_record_t *record;

	build_clean(image);
	tree = &image->trees[GEN_EXTENTS];
	record = &tree->records[1];
	CHECK(gen_load(image->bytes + record->offset + 4, 4) == 32);
	gen_store(image->bytes + record->offset + 8, start, 4);
}

static void
overflow_gap(gen_image_t *image)
{
	overflow_start_fault(image, 9);
}
static void
overflow_overlap(gen_image_t *image)
{
	overflow_start_fault(image, 7);
}

static void
bad_blocks_gap(gen_image_t *image)
{
	build_clean(image);
	gen_store(image->bytes + image->trees[GEN_EXTENTS].records[0].offset + 8, 1, 4);
}

static void
bad_blocks_resource_orphan(gen_image_t *image)
{
	build_clean(image);
	image->bytes[image->trees[GEN_EXTENTS].records[0].offset + 2] = 255;
}

static void
move_alternate_beyond_allocation(gen_image_t *image)
{
	size_t old_length;

	old_length = image->length;
	image->length += 2048;
	image->bytes = realloc(image->bytes, image->length);
	CHECK(image->bytes != NULL);
	bzero(image->bytes + old_length - 1024, 3072);
	bcopy(header(image), image->bytes + image->length - 1024, 512);
}

/* Slack leaves the alternate outside allocation. Retain the formatter's
 * last ceil(1024 / B) bits, or clear them and adjust the independent count.
 */
static void
build_slack_tail(gen_image_t *image, uint32_t block_size, bool marked)
{
	gen_plan_t plan;
	unsigned char *bitmap;
	uint32_t tail = (1024 + block_size - 1) / block_size;
	uint32_t free_blocks;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.block_size = block_size;
	gen_build(&plan, image);
	move_alternate_beyond_allocation(image);
	bitmap = image->bytes + gen_load(header(image) +
	    GEN_BITMAP_FORK + 16, 4) * block_size;
	free_blocks = (uint32_t)gen_load(header(image) + 48, 4);
	if (!marked) {
		for (uint32_t i = 512 - tail; i < 512; i++)
			bitmap[i / 8] &= (unsigned char)~(0x80u >> (i % 8));
		gen_store(header(image) + 48, free_blocks + tail, 4);
	}
	bcopy(header(image), image->bytes + image->length - 1024, 512);
}

static void
tail_orphan_4096(gen_image_t *image)
{
	unsigned char *bitmap;

	build_slack_tail(image, 4096, true);
	bitmap = image->bytes + gen_load(header(image) +
	    GEN_BITMAP_FORK + 16, 4) * 4096;
	bitmap[510 / 8] |= 0x80u >> (510 % 8);
}

static void
tail_orphan_512(gen_image_t *image)
{
	unsigned char *bitmap;

	build_slack_tail(image, 512, true);
	bitmap = image->bytes + gen_load(header(image) +
	    GEN_BITMAP_FORK + 16, 4) * 512;
	bitmap[509 / 8] |= 0x80u >> (509 % 8);
}

static void
tail_block_owned_with_clear_bit(gen_image_t *image)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .data_size = 1 };
	gen_plan_t plan;
	unsigned char *bitmap;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, image);
	gen_store(catalog_data(image, 32, false) + 104, 511, 4);
	gen_rebuild_bitmap(image);
	move_alternate_beyond_allocation(image);
	bitmap = image->bytes + gen_load(header(image) +
	    GEN_BITMAP_FORK + 16, 4) * image->block_size;
	bitmap[511 / 8] &= (unsigned char)~(0x80u >> (511 % 8));
	gen_store(header(image) + 48, gen_load(header(image) + 48, 4) + 1, 4);
	bcopy(header(image), image->bytes + image->length - 1024, 512);
}

static void
missing_thread(gen_image_t *image)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .omit_thread = true };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, image);
}

static void
duplicate_thread(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, true) - 6, 33, 4);
}

static void
thread_without_primary(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, false) + 8, 35, 4);
}

static void
build_parent_fault(gen_image_t *image, uint32_t parent)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2 },
		{ .name = name_b, .units = 1, .id = 33, .type = 1,
		    .parent = parent }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
}

static void
parent_missing(gen_image_t *image)
{
	build_parent_fault(image, 99);
}

static void
parent_is_file(gen_image_t *image)
{
	build_parent_fault(image, 32);
}

static void
disconnected_cycle(gen_image_t *image)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1, .parent = 33 },
		{ .name = name_b, .units = 1, .id = 33, .type = 1, .parent = 32 }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
}

static void
folder_second_parent(gen_image_t *image)
{
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 1 },
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_c, .units = 1, .id = 32, .type = 1,
		    .parent = 33, .omit_thread = true }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
}

static void
duplicate_node_id(gen_image_t *image)
{
	build_clean(image);
	gen_store(catalog_data(image, 34, false) + 8, 32, 4);
}

static void
private_folder_child(gen_image_t *image)
{
	gen_object_t objects[] = {
		{ .name = private_data, .units = COUNT(private_data),
		    .id = 40, .type = 1 },
		{ .name = name_a, .units = 1, .id = 41, .parent = 40, .type = 1 }
	};
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	gen_build(&plan, image);
}

static void
build_reserved_id(gen_image_t *image, uint32_t id)
{
	gen_object_t object = { .name = name_a, .units = 1, .id = id, .type = 2 };
	gen_plan_t plan;

	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	gen_build(&plan, image);
}

static void
reserved_id_9(gen_image_t *image)
{
	build_reserved_id(image, 9);
}
static void
reserved_id_10(gen_image_t *image)
{
	build_reserved_id(image, 10);
}
static void
reserved_id_11(gen_image_t *image)
{
	build_reserved_id(image, 11);
}
static void
reserved_id_12(gen_image_t *image)
{
	build_reserved_id(image, 12);
}
static void
reserved_id_13(gen_image_t *image)
{
	build_reserved_id(image, 13);
}
static void
reserved_id_14(gen_image_t *image)
{
	build_reserved_id(image, 14);
}
static void
reserved_id_15(gen_image_t *image)
{
	build_reserved_id(image, 15);
}

static void
repeat_overflow(gen_image_t *image)
{
	gen_record_t *first, *second;

	build_deep_tree(image);
	first = &image->trees[GEN_EXTENTS].records[1];
	second = &image->trees[GEN_EXTENTS].records[2];
	bcopy(image->bytes + first->offset, image->bytes + second->offset, 12);
}

static void
extra_overflow(gen_image_t *image)
{
	unsigned char *fork;

	build_clean(image);
	fork = catalog_data(image, 32, false) + 88;
	gen_store(fork, 8 * 4096, 8);
	gen_store(fork + 12, 8, 4);
}

static void
build_attribute_fork(gen_image_t *image, unsigned extents, attribute_damage_t fault)
{
	unsigned char body[80] = { 0 }, continuation[64] = { 0 };
	gen_object_t object = { .name = name_a, .units = 1, .id = 32,
	    .type = 2, .flags = 4 };
	gen_attr_value_t values[] = {
		{ .id = 32, .name = name_b, .units = 1, .type = 0x20,
		    .body = body, .body_size = sizeof(body) },
		{ .id = 32, .name = name_b, .units = 1, .type = 0x30,
		    .start = fault == ATTRIBUTE_CONTINUATION_GAP ? 9 : 8,
		    .body = continuation, .body_size = sizeof(continuation) }
	};
	gen_plan_t plan;

	gen_store(body, extents * 4096, 8);
	gen_store(body + 12, extents + (fault == ATTRIBUTE_CONTINUATION_SHORT ? 1 : 0), 4);
	for (unsigned i = 0; i < extents && i < 8; i++) {
		gen_store(body + 16 + i * 8, 400 + i * 2, 4);
		gen_store(body + 20 + i * 8, 1, 4);
	}
	gen_store(continuation, 416, 4);
	gen_store(continuation + 4, 1, 4);
	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = &object;
	plan.object_count = 1;
	plan.attributes = true;
	plan.values = values;
	plan.value_count = extents > 8 ? 2 : 1;
	gen_build(&plan, image);
}

static void
attribute_continuation_gap(gen_image_t *image)
{
	build_attribute_fork(image, 9, ATTRIBUTE_CONTINUATION_GAP);
}

static void
attribute_continuation_short(gen_image_t *image)
{
	build_attribute_fork(image, 9, ATTRIBUTE_CONTINUATION_SHORT);
}

/* Two public stubs share one indirect file; it owns the only payload. */
static void
build_links(gen_image_t *image, link_damage_t fault)
{
	static const uint16_t inode[] = { 'i', 'N', 'o', 'd', 'e', '7' };
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2,
		    .special = 7, .finder = "hlnkhfs+", .dates = { 42 } },
		{ .name = name_b, .units = 1, .id = 33, .type = 1 },
		{ .name = name_c, .units = 1, .id = 34, .parent = 33,
		    .type = 2, .special = 7, .finder = "hlnkhfs+", .dates = { 42 } },
		{ .name = private_data, .units = COUNT(private_data),
		    .id = 40, .type = 1, .dates = { 42 } },
		{ .name = inode, .units = COUNT(inode), .id = 41, .parent = 40,
		    .type = 2, .special = 2, .data = (const unsigned char *)"abc",
		    .data_size = 3 },
		{ .name = name_b, .units = 1, .id = 35, .parent = 33,
		    .type = 2, .special = 7, .finder = "hlnkhfs+", .dates = { 42 } }
	};
	gen_plan_t plan;

	switch (fault) {
	case LINK_DANGLING: objects[0].special = 6; break;
	case LINK_ZERO_REFERENCE: objects[0].special = 0; break;
	case LINK_COUNT_LOW: objects[4].special = 1; break;
	case LINK_COUNT_HIGH: objects[4].special = 3; break;
	case LINK_UNREFERENCED_INODE:
		bzero(objects[0].finder, sizeof(objects[0].finder));
		bzero(objects[2].finder, sizeof(objects[2].finder));
		break;
	case LINK_NONEMPTY_STUB: objects[0].data_size = 1; break;
	case LINK_HIDDEN_NONEMPTY_STUB:
		objects[2].parent = 40;
		objects[2].data_size = 1;
		break;
	case LINK_HIDDEN_STUB: objects[2].parent = 40; break;
	case LINK_THREE_STUBS: objects[4].special = 3; break;
	default: break;
	}
	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects) -
	    (fault == LINK_THREE_STUBS ? 0 : 1);
	gen_build(&plan, image);
}

static void
private_data_valence_low(gen_image_t *image)
{
	build_links(image, LINK_SOUND);
	gen_store(catalog_data(image, 40, false) + 4, 0, 4);
}

static void
private_data_folder_count_wrong(gen_image_t *image)
{
	build_links(image, LINK_SOUND);
	gen_store(catalog_data(image, 40, false) + 2, 0x10, 2);
	gen_store(catalog_data(image, 40, false) + 84, 1, 4);
}

static void
hidden_stub_nonempty_fork(gen_image_t *image)
{
	build_links(image, LINK_HIDDEN_NONEMPTY_STUB);
}

static void
ghost_stub_in_private_data(gen_image_t *image)
{
	build_links(image, LINK_HIDDEN_STUB);
}

static void
link_dangling(gen_image_t *image)
{
	build_links(image, LINK_DANGLING);
}
static void
link_zero_reference(gen_image_t *image)
{
	build_links(image, LINK_ZERO_REFERENCE);
}
static void
link_count_low(gen_image_t *image)
{
	build_links(image, LINK_COUNT_LOW);
}
static void
link_count_high(gen_image_t *image)
{
	build_links(image, LINK_COUNT_HIGH);
}
static void
link_unreferenced_inode(gen_image_t *image)
{
	build_links(image, LINK_UNREFERENCED_INODE);
}
static void
link_nonempty_stub(gen_image_t *image)
{
	build_links(image, LINK_NONEMPTY_STUB);
}

static void
store_le(unsigned char *bytes, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++)
		bytes[i] = (unsigned char)(value >> (8 * i));
}

/* Three independently framed chunks make the second table entry independent of the
 * first chunk's open-time checks. The plain sibling pins byte accounting.
 * Table faults damage the second entry; decode faults damage its payload.
 */
static void
build_compressed_variant(gen_image_t *image, unsigned type, compression_damage_t fault,
    compression_sibling_t sibling)
{
	static const uint16_t compression_name[] = { 'c', 'o', 'm', '.',
	    'a', 'p', 'p', 'l', 'e', '.', 'd', 'e', 'c', 'm', 'p', 'f', 's' };
	unsigned char attribute[16] = { 'f', 'p', 'm', 'c' };
	unsigned char inline_attribute[20] = { 'f', 'p', 'm', 'c' };
	unsigned char last_lzfse[] = { 'b', 'v', 'x', '-', 1, 0, 0, 0,
	    'x', 'b', 'v', 'x', '$' };
	unsigned char *raw, *resource;
	gen_compressed_chunk_t chunks[3];
	gen_object_t objects[] = {
		{ .name = name_a, .units = 1, .id = 32, .type = 2,
		    .flags = 4, .owner_flags = 0x20 },
		{ .name = name_b, .units = 1, .id = 33, .type = 2,
		    .data = (const unsigned char *)"abc", .data_size = 3 }
	};
	gen_attr_value_t values[] = {
		{ .id = 32, .type = 0x10,
		    .name = compression_name, .units = COUNT(compression_name),
		    .value = attribute, .size = sizeof(attribute) },
		{ .id = 33, .type = 0x10,
		    .name = compression_name, .units = COUNT(compression_name),
		    .value = inline_attribute, .size = sizeof(inline_attribute) }
	};
	gen_plan_t plan;
	size_t size, cursor, second;

	store_le(attribute + 4, type, 4);
	store_le(attribute + 8, 2 * 65536 + 1, 8);
	raw = malloc(65548);
	CHECK(raw != NULL);
	memset(raw, 'x', 65537);
	raw[0] = type == 4 ? 0xff : (type == 8 ? 6 : 0xff);
	chunks[0] = (gen_compressed_chunk_t){ raw, 65537 };
	chunks[1] = chunks[0];
	chunks[2] = (gen_compressed_chunk_t){ raw, 2 };
	if (type == 12) {
		bcopy("bvx-", raw, 4);
		store_le(raw + 4, 65536, 4);
		memset(raw + 8, 'x', 65536);
		bcopy("bvx$", raw + 65544, 4);
		chunks[0].length = 65548;
		chunks[1].length = 65548;
		chunks[2] = (gen_compressed_chunk_t){ last_lzfse, sizeof(last_lzfse) };
	}
	if (type == 4) {
		cursor = 256 + 8 + 3 * 8;
		size = cursor + 2 * 65537 + 2 + 50;
		resource = calloc(1, size);
		CHECK(resource != NULL);
		store_le(resource + 260, 3, 4);
		for (unsigned i = 0; i < 3; i++) {
			store_le(resource + 264 + 8 * i, cursor - 260, 4);
			store_le(resource + 268 + 8 * i, chunks[i].length, 4);
			bcopy(raw, resource + cursor, chunks[i].length);
			cursor += chunks[i].length;
		}
		gen_store(resource, 256, 4);
		gen_store(resource + 4, cursor, 4);
		gen_store(resource + 8, cursor - 256, 4);
		gen_store(resource + 12, 50, 4);
		gen_store(resource + 256, cursor - 260, 4);
		second = 288 + 65537;
		if (fault == COMPRESSION_OUTSIDE)
			store_le(resource + 272, size, 4);
		if (fault == COMPRESSION_OVERLAP)
			store_le(resource + 272, 28, 4);
		if (fault == COMPRESSION_DESCENDING)
			store_le(resource + 272, 27, 4);
		if (fault == COMPRESSION_DECODE_SHORT)
			store_le(resource + 276, 65536, 4);
	} else {
		resource = gen_compressed_resource(chunks, 3, &size);
		second = 16 + chunks[0].length;
		if (fault == COMPRESSION_OUTSIDE)
			store_le(resource + 8, size + 1, 4);
		if (fault == COMPRESSION_OVERLAP)
			store_le(resource + 8, second - 1, 4);
		if (fault == COMPRESSION_DESCENDING)
			store_le(resource + 8, 15, 4);
		if (fault == COMPRESSION_DECODE_SHORT) {
			store_le(resource + 8, second + chunks[1].length - 1, 4);
			if (type == 12) {
				store_le(resource + second + 4, 65535, 4);
				bcopy("bvx$", resource + second + 65543, 4);
			}
		}
	}
	if (fault == COMPRESSION_BAD_STREAM)
		resource[second] = 0;
	if (sibling != COMPRESSION_PLAIN_SIBLING)
		objects[1].data_size = 0;
	if (sibling == COMPRESSION_INLINE_SIBLING) {
		objects[1].flags = 4;
		objects[1].owner_flags = 0x20;
		store_le(inline_attribute + 4, 3, 4);
		store_le(inline_attribute + 8, 3, 8);
		inline_attribute[16] = 0xff;
		bcopy("abc", inline_attribute + 17, 3);
	}
	objects[0].resource = resource;
	objects[0].resource_size = size;
	gen_plan_init(&plan);
	plan.verification = true;
	plan.objects = objects;
	plan.object_count = COUNT(objects);
	plan.attributes = true;
	plan.values = values;
	plan.value_count = sibling == COMPRESSION_INLINE_SIBLING ? 2 : 1;
	gen_build(&plan, image);
	free(resource);
	free(raw);
}

static void
build_compressed(gen_image_t *image, unsigned type, compression_damage_t fault)
{
	build_compressed_variant(image, type, fault, COMPRESSION_PLAIN_SIBLING);
}

static void
compressed4_outside(gen_image_t *image)
{
	build_compressed(image, 4, COMPRESSION_OUTSIDE);
}
static void
compressed4_overlap(gen_image_t *image)
{
	build_compressed(image, 4, COMPRESSION_OVERLAP);
}
static void
compressed4_descending(gen_image_t *image)
{
	build_compressed(image, 4, COMPRESSION_DESCENDING);
}
static void
compressed8_outside(gen_image_t *image)
{
	build_compressed(image, 8, COMPRESSION_OUTSIDE);
}
static void
compressed8_overlap(gen_image_t *image)
{
	build_compressed(image, 8, COMPRESSION_OVERLAP);
}
static void
compressed8_descending(gen_image_t *image)
{
	build_compressed(image, 8, COMPRESSION_DESCENDING);
}
static void
compressed12_outside(gen_image_t *image)
{
	build_compressed(image, 12, COMPRESSION_OUTSIDE);
}
static void
compressed12_overlap(gen_image_t *image)
{
	build_compressed(image, 12, COMPRESSION_OVERLAP);
}
static void
compressed12_descending(gen_image_t *image)
{
	build_compressed(image, 12, COMPRESSION_DESCENDING);
}

static void
test_compression_scopes(void)
{
	static const unsigned types[] = { 4, 8, 12 };
	gen_image_t image;
	fixture_t fixture;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	char description[160];
	const char *message;
	static const compression_damage_t damages[] = {
	    COMPRESSION_SOUND, COMPRESSION_DECODE_SHORT, COMPRESSION_BAD_STREAM };
	static const char *const damage_names[] = {
	    "sound", "decodes one byte short", "corrupt stream" };

	for (size_t i = 0; i < COUNT(types); i++) {
		for (size_t damage = 0; damage < COUNT(damages); damage++) {
			compression_damage_t fault = damages[damage];
			for (unsigned payload = 0; payload < 2; payload++) {
				snprintf(description, sizeof(description),
				    "type %u %s in %s scope", types[i], damage_names[damage],
				    payload ? "payload" : "structural");
				scenario = description;
				message = "chunk";
				if (fault == COMPRESSION_BAD_STREAM && types[i] == 8)
					message = "lzvn";
				if (fault == COMPRESSION_BAD_STREAM && types[i] == 12)
					message = "lzfse";
				build_compressed(&image, types[i], fault);
				CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
				fixture.watched_first = gen_load(catalog_data(&image, 32, false) + 184, 4) *
				    image.block_size + (types[i] == 4 ? 288 : 16);
				fixture.watched_end = fixture.watched_first + 65536;
				if (payload && fault != COMPRESSION_SOUND)
					check_failure(&fixture, FSIMG_VERIFY_PAYLOAD,
					    FSIMG_ECORRUPT, message);
				else {
					status = fsimg_verify(fixture.reader,
					    payload ? FSIMG_VERIFY_PAYLOAD : 0, &report, &error);
					expect_status(status, FSIMG_OK, &error);
					expect(report.complete, "compressed report is complete");
					expect(report.files == 2 && report.directories == 1,
					    "exact compressed-fixture counts");
					expect(report.free_clusters == gen_load(header(&image) + 48, 4) &&
					    report.allocated_clusters == 512 - report.free_clusters &&
					    report.bad_clusters == 0, "exact allocation counts");
					expect((fixture.reader->info.valid & FSIMG_VOLUME_FREE) != 0,
					    "complete compressed verification publishes free space");
					expect(report.logical_bytes == 2 * 65536 + 4,
					    "logical size includes decoded compressed bytes");
					expect(report.payload_bytes_read == (payload ? 3 : 0),
					    "only the plain sibling contributes initialized bytes");
				}
				if (!payload)
					expect(fixture.watched_reads == 0,
					    "structural scope reads no resource chunk bytes");
				fixture_close(&fixture);
				gen_free(&image);
			}
		}
	}
}

static void
test_compressed_only_payload_counts_no_initialized_bytes(void)
{
	static const unsigned types[] = { 4, 8, 12 };
	gen_image_t image;
	fixture_t fixture;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };

	for (size_t i = 0; i < COUNT(types); i++) {
		scenario = "compressed-only payload contributes zero initialized bytes";
		build_compressed_variant(&image, types[i], COMPRESSION_SOUND,
		    COMPRESSION_EMPTY_SIBLING);
		CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
		expect_status(fsimg_verify(fixture.reader, FSIMG_VERIFY_PAYLOAD,
		    &report, &error), FSIMG_OK, &error);
		expect(report.complete, "compressed-only report is complete");
		expect(report.logical_bytes == 2 * 65536 + 1,
		    "compressed-only logical size is decoded size");
		expect(report.payload_bytes_read == 0,
		    "held compressed extents contribute no initialized bytes");
		fixture_close(&fixture);
		gen_free(&image);
	}
}

static const corruption_t corruptions[] = {
	{ "attribute_owner_id_zero",
	    attribute_owner_id_zero, FSIMG_ECORRUPT, "HFS Plus attribute names no primary record" },
	{ "attribute_owner_between_primaries",
	    attribute_owner_between_primaries, FSIMG_ECORRUPT, "HFS Plus attribute names no primary record" },
	{ "private_data_valence_low",
	    private_data_valence_low, FSIMG_ECORRUPT, "HFS Plus folder's valence disagrees with its children" },
	{ "private_data_folder_count_wrong",
	    private_data_folder_count_wrong, FSIMG_ECORRUPT, "HFS Plus folder count disagrees with its child folders" },
	{ "hidden_stub_nonempty_fork",
	    hidden_stub_nonempty_fork, FSIMG_ECORRUPT, "HFS Plus hard-link stub has a nonempty fork" },
	{ "ghost_stub_in_private_data",
	    ghost_stub_in_private_data, FSIMG_ECORRUPT, "HFS Plus private data folder holds a hard-link stub" },
	{ "map_link_past_total_nodes_extents",
	    map_link_past_total_nodes_extents, FSIMG_ECORRUPT, "HFS Plus node lies outside its tree" },
	{ "map_link_past_total_nodes_catalog",
	    map_link_past_total_nodes_catalog, FSIMG_ECORRUPT, "HFS Plus node lies outside its tree" },
	{ "map_link_past_total_nodes_attributes",
	    map_link_past_total_nodes_attributes, FSIMG_ECORRUPT, "HFS Plus node lies outside its tree" },
	{ "tail_block_owned_with_clear_bit",
	    tail_block_owned_with_clear_bit, FSIMG_ECORRUPT, "HFS Plus bitmap leaves an owned block clear" },
	{ "type 4 second chunk outside",
	    compressed4_outside, FSIMG_ECORRUPT, "outside" },
	{ "type 4 second chunk overlap",
	    compressed4_overlap, FSIMG_ECORRUPT, "overlap" },
	{ "type 4 second chunk descending",
	    compressed4_descending, FSIMG_ECORRUPT, "overlap" },
	{ "type 8 second chunk outside",
	    compressed8_outside, FSIMG_ECORRUPT, "outside" },
	{ "type 8 second chunk overlap",
	    compressed8_overlap, FSIMG_ECORRUPT, "overlap" },
	{ "type 8 second chunk descending",
	    compressed8_descending, FSIMG_ECORRUPT, "overlap" },
	{ "type 12 second chunk outside",
	    compressed12_outside, FSIMG_ECORRUPT, "outside" },
	{ "type 12 second chunk overlap",
	    compressed12_overlap, FSIMG_ECORRUPT, "overlap" },
	{ "type 12 second chunk descending",
	    compressed12_descending, FSIMG_ECORRUPT, "overlap" },

	{ "set orphan just below 4096-byte tail",
	    tail_orphan_4096, FSIMG_ECORRUPT, "nothing owns" },
	{ "set orphan just below 512-byte tail",
	    tail_orphan_512, FSIMG_ECORRUPT, "nothing owns" },
	{ "missing thread",
	    missing_thread, FSIMG_ECORRUPT, "thread record is missing" },
	{ "duplicated thread",
	    duplicate_thread, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "thread has no primary",
	    thread_without_primary, FSIMG_ECORRUPT, "thread has no primary" },
	{ "parent is missing",
	    parent_missing, FSIMG_ECORRUPT, "root does not reach" },
	{ "parent is a file",
	    parent_is_file, FSIMG_ECORRUPT, "root does not reach" },
	{ "folder cycle disconnected from root",
	    disconnected_cycle, FSIMG_ECORRUPT, "root does not reach" },
	{ "folder entered from second parent",
	    folder_second_parent, FSIMG_ECORRUPT, "thread record is missing" },
	{ "duplicate node ID",
	    duplicate_node_id, FSIMG_ECORRUPT, "thread record is missing" },
	{ "folder inside private data folder",
	    private_folder_child, FSIMG_ECORRUPT, "holds a folder" },
	{ "reserved node ID 9",
	    reserved_id_9, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 10",
	    reserved_id_10, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 11",
	    reserved_id_11, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 12",
	    reserved_id_12, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 13",
	    reserved_id_13, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 14",
	    reserved_id_14, FSIMG_ECORRUPT, "node ID" },
	{ "reserved node ID 15",
	    reserved_id_15, FSIMG_ECORRUPT, "node ID" },
	{ "repeated overflow record",
	    repeat_overflow, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "extra overflow record",
	    extra_overflow, FSIMG_ECORRUPT, "no fork consumes" },
	{ "attribute continuation has a gap",
	    attribute_continuation_gap, FSIMG_ECORRUPT, "does not continue" },
	{ "attribute continuation ends short",
	    attribute_continuation_short, FSIMG_ECORRUPT, "fall short" },
	{ "hard-link stub dangles",
	    link_dangling, FSIMG_ECORRUPT, "indirect node is missing" },
	{ "hard-link reference is zero",
	    link_zero_reference, FSIMG_ECORRUPT, "HFS Plus hard-link stub has a zero reference" },
	{ "hard-link count is one low",
	    link_count_low, FSIMG_ECORRUPT, "link count disagrees" },
	{ "hard-link count is one high",
	    link_count_high, FSIMG_ECORRUPT, "link count disagrees" },
	{ "unreferenced indirect node has nonzero count",
	    link_unreferenced_inode, FSIMG_ECORRUPT, "link count disagrees" },
	{ "hard-link stub has nonempty fork",
	    link_nonempty_stub, FSIMG_ECORRUPT, "nonempty fork" },

	{ "bitmap padding bit is set",
	    bitmap_padding_set, FSIMG_ECORRUPT, "padding bits" },
	{ "overflow record leaves a gap",
	    overflow_gap, FSIMG_ECORRUPT, "overflow" },
	{ "overflow record overlaps inline extents",
	    overflow_overlap, FSIMG_ECORRUPT, "overflow" },
	{ "bad-block record leaves a gap",
	    bad_blocks_gap, FSIMG_ECORRUPT, "bad-block" },
	{ "bad-block resource record is orphaned",
	    bad_blocks_resource_orphan, FSIMG_ECORRUPT, "no fork consumes" },
	{ "equal keys within a leaf",
	    equal_keys_within, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "descending keys within a leaf",
	    descending_keys_within, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "equal keys across siblings",
	    equal_keys_across, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "descending keys across siblings",
	    descending_keys_across, FSIMG_ECORRUPT, "keys do not ascend" },
	{ "index key differs by one unit",
	    index_key_differs, FSIMG_ECORRUPT, "index key differs" },
	{ "index child has wrong height",
	    child_wrong_height, FSIMG_ECORRUPT, "HFS Plus node has the wrong kind or height" },
	{ "index child is header",
	    child_is_header, FSIMG_ECORRUPT, "HFS Plus node has the wrong kind or height" },
	{ "index child is itself",
	    child_is_self, FSIMG_ECORRUPT, "HFS Plus node has the wrong kind or height" },
	{ "leaf_sibling_cycle_fails_back_link",
	    leaf_sibling_cycle_fails_back_link, FSIMG_ECORRUPT, "HFS Plus sibling's back link does not name its neighbour" },
	{ "leaf sibling back link is broken",
	    leaf_backlink, FSIMG_ECORRUPT, "sibling" },
	{ "index_sibling_cycle_fails_back_link",
	    index_sibling_cycle_fails_back_link, FSIMG_ECORRUPT, "HFS Plus sibling's back link does not name its neighbour" },
	{ "index sibling back link is broken",
	    index_backlink, FSIMG_ECORRUPT, "sibling" },
	{ "leafRecords is off",
	    leaf_records_off, FSIMG_ECORRUPT, "leafRecords" },
	{ "firstLeafNode is off",
	    first_leaf_off, FSIMG_ECORRUPT, "firstLeafNode" },
	{ "lastLeafNode is off",
	    last_leaf_off, FSIMG_ECORRUPT, "lastLeafNode" },
	{ "treeDepth is off",
	    tree_depth_off, FSIMG_ECORRUPT, "HFS Plus node has the wrong kind or height" },
	{ "freeNodes is off",
	    free_nodes_off, FSIMG_ECORRUPT, "freeNodes" },
	{ "used node is unmarked",
	    unmarked_used_node, FSIMG_ECORRUPT, "map leaves free" },
	{ "free node is marked",
	    marked_free_node, FSIMG_ECORRUPT, "does not use" },
	{ "map bit is set past totalNodes",
	    map_bit_past_total, FSIMG_ECORRUPT, "past the tree's end" },
	{ "map chain loops",
	    map_chain_loop, FSIMG_ECORRUPT, "twice" },
	{ "map chain is short",
	    map_chain_short, FSIMG_ECORRUPT, "does not cover" },

	{ "nextCatalogID below a used ID",
	    next_id_below_used, FSIMG_ECORRUPT, "nextCatalogID" },
	{ "alternate signature disagrees",
	    alternate_signature, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate version disagrees",
	    alternate_version, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate blockSize disagrees",
	    alternate_block_size, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate totalBlocks disagrees",
	    alternate_total_blocks, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate bitmap fork disagrees",
	    alternate_bitmap, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate extents fork disagrees",
	    alternate_extents, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate catalog fork disagrees",
	    alternate_catalog, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate attributes fork disagrees",
	    alternate_attributes, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate startup fork disagrees",
	    alternate_startup, FSIMG_ECORRUPT, "alternate header" },
	{ "alternate header zero filled",
	    alternate_zero, FSIMG_ECORRUPT, "alternate header" },
	{ "listed zero-length fork has stray extent",
	    listed_empty_fork_stray_extent, FSIMG_ECORRUPT, "extent" },
	{ "private zero-length fork has stray extent",
	    private_empty_fork_stray_extent, FSIMG_ECORRUPT, "extent" },
	{ "hidden_socket_data",
	    hidden_socket_data, FSIMG_ECORRUPT, "HFS Plus socket or device has a nonempty fork" },
	{ "hidden_character_resource",
	    hidden_character_resource, FSIMG_ECORRUPT, "HFS Plus socket or device has a nonempty fork" },
	{ "hidden_block_data",
	    hidden_block_data, FSIMG_ECORRUPT, "HFS Plus socket or device has a nonempty fork" },
	{ "hidden_symlink_resource",
	    hidden_symlink_resource, FSIMG_ECORRUPT, "HFS Plus symbolic link has a resource fork" },
	{ "hidden_symlink_empty",
	    hidden_symlink_empty, FSIMG_EUNSUPPORTED, "HFS Plus symbolic link's target length is not 1 to 1023 bytes" },
	{ "hidden_symlink_1024",
	    hidden_symlink_1024, FSIMG_EUNSUPPORTED, "HFS Plus symbolic link's target length is not 1 to 1023 bytes" },
	{ "hidden_compressed_no_attribute",
	    hidden_compressed_no_attribute, FSIMG_ECORRUPT, "HFS Plus compressed file has no compression attribute" },
	{ "hidden_compressed_bad_signature",
	    hidden_compressed_bad_signature, FSIMG_ECORRUPT, "HFS Plus compression attribute's header is malformed" },
	{ "hidden_compressed_data",
	    hidden_compressed_data, FSIMG_ECORRUPT, "HFS Plus compressed file has a nonempty data fork" },
	{ "hidden_compressed_unknown_type",
	    hidden_compressed_unknown_type, FSIMG_EUNSUPPORTED, "HFS Plus compression type is not supported" },
	{ "hidden_inode_fifo",
	    hidden_inode_fifo, FSIMG_ECORRUPT, "HFS Plus indirect node is a stub or of another kind" },
	{ "hidden_inode_compressed_no_attribute",
	    hidden_inode_compressed_no_attribute, FSIMG_ECORRUPT, "HFS Plus compressed file has no compression attribute" },
	{ "hidden_inode_bad_table_referenced",
	    hidden_inode_bad_table_referenced, FSIMG_ECORRUPT, "HFS Plus compressed file's chunk table is malformed" },
	{ "used extent has zero count",
	    extent_zero_count, FSIMG_ECORRUPT, "extent" },
	{ "used extent follows unused descriptor",
	    extent_after_unused, FSIMG_ECORRUPT, "extent" },
	{ "extent enters reserved blocks",
	    extent_reserved, FSIMG_ECORRUPT, "extent" },
	{ "extent starts past volume",
	    extent_past_volume, FSIMG_ECORRUPT, "extent" },
	{ "extent enters reserved tail",
	    extent_across_tail, FSIMG_ECORRUPT, "extent" },
	{ "logicalSize exceeds allocation",
	    logical_above_allocation, FSIMG_ECORRUPT, "allocation" },
	{ "referenced block is clear",
	    referenced_clear_block, FSIMG_ECORRUPT, "owned block clear" },
	{ "allocated block is orphaned",
	    allocated_orphan, FSIMG_ECORRUPT, "nothing owns" },
	{ "two forks share a block",
	    shared_block, FSIMG_ECORRUPT, "claimed by more than one owner" },
	{ "bitmap is short",
	    bitmap_short, FSIMG_ECORRUPT, "bitmap" },
	{ "freeBlocks is one low",
	    free_blocks_low, FSIMG_ECORRUPT, "freeBlocks" },
	{ "freeBlocks is one high",
	    free_blocks_high, FSIMG_ECORRUPT, "freeBlocks" },
	{ "thread names wrong parent",
	    wrong_thread_parent, FSIMG_ECORRUPT, "thread does not match" },
	{ "thread names wrong name",
	    wrong_thread_name, FSIMG_ECORRUPT, "thread does not match" },
	{ "thread has wrong type",
	    wrong_thread_type, FSIMG_ECORRUPT, "thread does not match" },
	{ "folder valence disagrees",
	    wrong_valence, FSIMG_ECORRUPT, "valence" },
	{ "record folderCount disagrees",
	    wrong_folder_count, FSIMG_ECORRUPT, "folder count" },
	{ "header fileCount disagrees",
	    header_file_count, FSIMG_ECORRUPT, "fileCount" },
	{ "header folderCount disagrees",
	    header_folder_count, FSIMG_ECORRUPT, "folderCount" },
	{ "attribute_owner_above_every_primary",
	    attribute_owner_above_every_primary, FSIMG_ECORRUPT, "HFS Plus attribute names no primary record" },
	{ "attribute flag is clear",
	    attribute_flag_clear, FSIMG_ECORRUPT, "HFS Plus record's attribute flag disagrees with the attributes tree" },
	{ "attribute flag has no record",
	    attribute_flag_without_record, FSIMG_ECORRUPT, "HFS Plus record's attribute flag disagrees with the attributes tree" },
	{ "attribute has unknown type",
	    attribute_unknown_type, FSIMG_EUNSUPPORTED, "attribute" },
	{ "entry name is not decomposed",
	    refused_entry_name, FSIMG_EUNSUPPORTED, "does not map back" },
	{ "volume label cannot be displayed",
	    absent_label, FSIMG_EUNSUPPORTED, "label" },
	{ "private directory folder is nonempty", private_directory_nonempty, FSIMG_EUNSUPPORTED, "directory hard links" }
};

static void
test_clean_variants_and_reused_ids(void)
{
	static const char *const map_names[] = {
	    "fixed header map covers 2048 nodes without map nodes",
	    "one map node covers 2056 nodes",
	    "two map nodes cover 5992 nodes"
	};
	gen_image_t image;

	for (unsigned variant = 0; variant < 2; variant++) {
		for (unsigned journal = 0; journal < 2; journal++) {
			scenario = variant ? "clean HFSX" : "clean HFS Plus";
			build_clean_variant(&image, variant != 0, journal != 0);
			check_clean(&image, 2, 2, 9 * 4096 + 3, 2);
			gen_free(&image);
		}
	}
	scenario = "reused node IDs allow nextCatalogID below used IDs";
	next_id_below_used(&image);
	gen_store(header(&image) + 4, 0x1100, 4);
	check_clean(&image, 2, 2, 9 * 4096 + 3, 2);
	gen_free(&image);
	for (unsigned maps = 0; maps <= 2; maps++) {
		scenario = map_names[maps];
		build_map_chain(&image, maps);
		check_clean(&image, 0, 1, 0, 0);
		gen_free(&image);
	}
	scenario = "short map-node record still covers totalNodes";
	build_map_chain(&image, 1);
	gen_store(extent_node(&image, 1) + 508, 16, 2);
	check_clean(&image, 0, 1, 0, 0);
	gen_free(&image);
	scenario = "deep tree has leaf and index siblings";
	build_deep_tree(&image);
	check_clean(&image, 1, 1, 65 * 4096, 2);
	gen_free(&image);
	scenario = "empty extents and absent attributes trees";
	build_empty(&image);
	check_clean(&image, 0, 1, 0, 0);
	gen_free(&image);
}

static void
test_additional_acceptance(void)
{
	static const unsigned extent_counts[] = { 1, 8, 9 };
	static const uint32_t block_sizes[] = { 512, 4096 };
	gen_image_t image;
	gen_plan_t plan;

	for (size_t i = 0; i < COUNT(extent_counts); i++) {
		scenario = "attribute fork has complete extents";
		build_attribute_fork(&image, extent_counts[i], ATTRIBUTE_SOUND);
		check_clean(&image, 1, 1, 0, 0);
		gen_free(&image);
	}
	scenario = "empty attributes tree";
	gen_plan_init(&plan);
	plan.verification = true;
	plan.attributes = true;
	gen_build(&plan, &image);
	check_clean(&image, 0, 1, 0, 0);
	gen_free(&image);
	scenario = "two hard-link names share one indirect node";
	build_links(&image, LINK_SOUND);
	check_clean_bytes(&image, 2, 2, 6, 3, 0);
	gen_free(&image);
	for (size_t i = 0; i < COUNT(block_sizes); i++) {
		for (unsigned marked = 0; marked < 2; marked++) {
			scenario = marked ? "slack volume counts formatter-marked tail" :
			    "slack volume permits clear formatter tail";
			build_slack_tail(&image, block_sizes[i], marked != 0);
			check_clean(&image, 0, 1, 0, 0);
			gen_free(&image);
		}
	}
}

static void
test_corruption_table(void)
{
	for (size_t i = 0; i < COUNT(corruptions); i++) {
		const corruption_t *row = &corruptions[i];

		for (unsigned payload = 0; payload < 2; payload++) {
			gen_image_t image;
			fixture_t fixture;
			fsimg_error_t error = { 0 };
			fsimg_status_t status;
			char description[160];

			snprintf(description, sizeof(description), "%s (%s)",
			    row->name, payload ? "payload" : "structural");
			scenario = description;
			row->build(&image);
			status = fixture_open(&fixture, &image, 0, 0, &error);
			expect_status(status, FSIMG_OK, &error);
			if (status == FSIMG_OK)
				check_failure(&fixture, payload ? FSIMG_VERIFY_PAYLOAD : 0,
				    row->status, row->message);
			fixture_close(&fixture);
			gen_free(&image);
		}
	}
}

/* Ordinary listing and content reads do not open a size-zero resource fork
 * or a hidden private file. Verification must still inspect both forks.
 */
static void
test_stray_empty_forks_do_not_prevent_listing_or_reading(void)
{
	void (*const builders[])(gen_image_t *) = {
	    listed_empty_fork_stray_extent, private_empty_fork_stray_extent };
	gen_image_t image;
	fixture_t fixture;
	const fsimg_entry_t *entry;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	unsigned char bytes[4096];
	size_t transferred, count;

	for (size_t i = 0; i < COUNT(builders); i++) {
		scenario = i == 0 ? "listing and reading ignore size-zero resource extents" :
		    "listing and reading omit hidden private forks";
		builders[i](&image);
		CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
		count = 0;
		while ((status = fsimg_reader_next(fixture.reader, &entry,
		    &error)) == FSIMG_OK) {
			count++;
			if (entry->kind != FSIMG_ENTRY_FILE)
				continue;
			for (uint64_t offset = 0; offset < entry->size;) {
				expect_status(fsimg_reader_read_at(fixture.reader, offset,
				    bytes, sizeof(bytes), &transferred, &error), FSIMG_OK,
				    &error);
				expect(transferred != 0, "content read makes progress");
				if (transferred == 0)
					break;
				offset += transferred;
			}
		}
		expect_status(status, FSIMG_DONE, &error);
		expect(count == (i == 0 ? 3 : 0), "exact visible entry count");
		fixture_close(&fixture);
		gen_free(&image);
	}
}

static void
test_preflight_refuses_before_scanning(void)
{
	void (*const builders[])(gen_image_t *) = {
	    absent_label, private_directory_nonempty };

	for (size_t i = 0; i < COUNT(builders); i++) {
		gen_image_t image;
		fixture_t fixture;
		fsimg_error_t error = { 0 };
		uint64_t reads, bitmap_reads;

		scenario = i == 0 ? "absent label performs no verification reads" :
		    "private directory preflight reads only its search path";
		builders[i](&image);
		CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
		reads = fixture.reads;
		bitmap_reads = fixture.bitmap_reads;
		check_failure(&fixture, 0, FSIMG_EUNSUPPORTED,
		    i == 0 ? "label" : "directory hard links");
		expect(fixture.reads - reads <= (i == 0 ? 0 :
		    (uint64_t)image.trees[GEN_CATALOG].depth + 1),
		    "preflight stays within its catalog search read bound");
		expect(fixture.bitmap_reads == bitmap_reads,
		    "preflight never reads the allocation bitmap");
		fixture_close(&fixture);
		gen_free(&image);
	}
}

static void
test_alternate_read_failure_keeps_io_status(void)
{
	gen_image_t image;
	fixture_t fixture;
	fsimg_error_t error = { 0 };

	scenario = "alternate header transport failure";
	build_clean(&image);
	CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
	fixture.fail_offset = image.length - 1024;
	check_failure(&fixture, 0, FSIMG_EIO, "injected alternate read failure");
	fixture_close(&fixture);
	gen_free(&image);
}

static void
test_truncation_precedes_driver_work(void)
{
	gen_image_t image;
	fixture_t fixture;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	uint64_t reads;

	scenario = "truncated volume never enters driver verification";
	build_clean(&image);
	CHECK(fixture_open(&fixture, &image, 0, 4096, &error) == FSIMG_OK);
	reads = fixture.reads;
	expect_status(fsimg_verify(fixture.reader, 0, &report, &error),
	    FSIMG_ECORRUPT, &error);
	expect(strstr(error.message, "truncated") != NULL, "common truncation error");
	expect(!report.complete && report.scope == 0, "driver scope remains untouched");
	expect(fixture.reads == reads, "truncation performs no reads");
	expect((fixture.reader->info.valid & FSIMG_VOLUME_FREE) == 0,
	    "truncation does not publish free space");
	fixture_close(&fixture);
	gen_free(&image);
}

static void
test_resource_and_cancellation_statuses(void)
{
	static const uint64_t limits[] = { 1, 64, 1024 };
	gen_image_t image;
	fixture_t fixture;
	fsimg_error_t error = { 0 };

	build_clean(&image);
	for (size_t i = 0; i < COUNT(limits); i++) {
		scenario = "verification scratch exhaustion keeps resource status";
		CHECK(fixture_open(&fixture, &image, limits[i], 0, &error) == FSIMG_OK);
		check_failure(&fixture, 0, FSIMG_ELIMIT, "resource limit");
		fixture_close(&fixture);
	}
	scenario = "cancellation during verification keeps cancellation status";
	CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
	fixture.cancel_at = fixture.polls + 8;
	check_failure(&fixture, 0, FSIMG_ECANCELLED, "cancel");
	fixture_close(&fixture);
	gen_free(&image);
}

static void
test_cross_linked_records_read_no_payload(void)
{
	gen_image_t image;
	fixture_t fixture;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	uint64_t block;
	char message[128];

	scenario = "cross_linked_records_read_no_payload";
	shared_block(&image);
	block = gen_load(catalog_data(&image, 32, false) + 104, 4);
	snprintf(message, sizeof(message), "HFS Plus allocation block %llu is claimed by more than one owner",
	    (unsigned long long)block);
	CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
	fixture.watched_first = block * image.block_size;
	fixture.watched_end = fixture.watched_first + image.block_size;

	status = fsimg_verify(fixture.reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	expect_status(status, FSIMG_ECORRUPT, &error);
	expect(strcmp(error.message, message) == 0, message);
	expect(!report.complete, "cross-linked volume is incomplete");
	if (report.payload_bytes_read != 0 || fixture.watched_reads != 0 ||
	    fixture.watched_bytes != 0)
		fprintf(stderr, "%s: got %s (%s), payload_bytes_read=%llu, watched_reads=%llu, watched_bytes=%llu\n",
		    scenario, fsimg_status_string(status), error.message,
		    (unsigned long long)report.payload_bytes_read,
		    (unsigned long long)fixture.watched_reads,
		    (unsigned long long)fixture.watched_bytes);
	expect(report.payload_bytes_read == 0, "cross-linked volume reads no payload bytes");
	expect(fixture.watched_reads == 0, "shared extent is never read");
	expect(fixture.watched_bytes == 0, "shared extent supplies no bytes");

	fixture_close(&fixture);
	gen_free(&image);
}

static void
test_stubs_read_their_indirect_node_once(void)
{
	gen_image_t image;
	fixture_t fixture;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	uint64_t size, block;

	scenario = "stubs_read_their_indirect_node_once";
	build_links(&image, LINK_THREE_STUBS);
	size = gen_load(catalog_data(&image, 41, false) + 88, 8);
	block = gen_load(catalog_data(&image, 41, false) + 104, 4);
	CHECK(size == 3);
	CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
	fixture.watched_first = block * image.block_size;
	fixture.watched_end = fixture.watched_first + size;

	status = fsimg_verify(fixture.reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	expect_status(status, FSIMG_OK, &error);
	expect(report.complete, "shared indirect node's report is complete");
	expect(report.files == 3, "each visible stub counts as a file");
	expect(report.logical_bytes == 3 * size, "each stub contributes its logical size");
	if (report.payload_bytes_read != size || fixture.watched_reads != 1 ||
	    fixture.watched_bytes != size)
		fprintf(stderr, "%s: got %s (%s), payload_bytes_read=%llu, watched_reads=%llu, watched_bytes=%llu\n",
		    scenario, fsimg_status_string(status), error.message,
		    (unsigned long long)report.payload_bytes_read,
		    (unsigned long long)fixture.watched_reads,
		    (unsigned long long)fixture.watched_bytes);
	expect(report.payload_bytes_read == size, "indirect payload counts once");
	expect(fixture.watched_reads == 1, "indirect content is read once");
	expect(fixture.watched_bytes == size, "indirect content supplies its bytes once");

	fixture_close(&fixture);
	gen_free(&image);
}

static void
test_tail_one_of_two_bits_set_block_512(void)
{
	gen_image_t image;
	unsigned char *bitmap;

	scenario = "tail_one_of_two_bits_set_block_512";
	build_slack_tail(&image, 512, false);
	bitmap = image.bytes + gen_load(header(&image) +
	    GEN_BITMAP_FORK + 16, 4) * 512;
	bitmap[510 / 8] |= 0x80u >> (510 % 8);
	gen_store(header(&image) + 48, gen_load(header(&image) + 48, 4) - 1, 4);
	bcopy(header(&image), image.bytes + image.length - 1024, 512);
	/* Three header blocks, one bitmap block, one extents header, sixteen
	 * catalog blocks, and one unowned formatter-tail bit use 22 blocks.
	 */
	CHECK(gen_load(header(&image) + 48, 4) == 490);
	for (unsigned payload = 0; payload < 2; payload++) {
		fixture_t fixture;
		fsimg_verify_report_t report;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;

		CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);
		status = fsimg_verify(fixture.reader, payload ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, &error);
		expect_status(status, FSIMG_OK, &error);
		expect(report.complete, "partly marked formatter tail is complete");
		expect(report.free_clusters == 490, "one clear tail bit counts as free");
		expect(report.allocated_clusters == 22, "one set tail bit counts as allocated");
		fixture_close(&fixture);
	}
	gen_free(&image);
}

static void
test_inline_compressed_after_fork_compressed(void)
{
	for (unsigned payload = 0; payload < 2; payload++) {
		gen_image_t image;
		fixture_t fixture;
		fsimg_verify_report_t report;
		fsimg_error_t error = { 0 };
		fsimg_status_t status;

		scenario = payload ? "inline_compressed_after_fork_compressed (payload)" :
		    "inline_compressed_after_fork_compressed (structural)";
		build_compressed_variant(&image, 4, COMPRESSION_SOUND,
		    COMPRESSION_INLINE_SIBLING);
		CHECK(fixture_open(&fixture, &image, 0, 0, &error) == FSIMG_OK);

		status = fsimg_verify(fixture.reader, payload ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, &error);
		expect_status(status, FSIMG_OK, &error);
		expect(report.complete, "fork and inline compressed files complete verification");

		fixture_close(&fixture);
		gen_free(&image);
	}
}

static int
write_fixture(const char *name, const char *path)
{
	gen_image_t image;
	FILE *file;

	if (strcmp(name, "clean") == 0)
		build_clean(&image);
	else if (strcmp(name, "corrupt") == 0)
		alternate_zero(&image);
	else if (strcmp(name, "refused") == 0)
		absent_label(&image);
	else
		return (2);
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(image.bytes, 1, image.length, file) == image.length);
	CHECK(fclose(file) == 0);
	gen_free(&image);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc == 4 && strcmp(argv[1], "--write-fixture") == 0)
		return (write_fixture(argv[2], argv[3]));
	CHECK(argc == 1);
	test_cross_linked_records_read_no_payload();
	test_stubs_read_their_indirect_node_once();
	test_tail_one_of_two_bits_set_block_512();
	test_inline_compressed_after_fork_compressed();
	test_compressed_only_payload_counts_no_initialized_bytes();
	test_compression_scopes();
	test_additional_acceptance();
	test_clean_variants_and_reused_ids();
	test_hidden_primary_acceptance();
	test_corruption_table();
	test_stray_empty_forks_do_not_prevent_listing_or_reading();
	test_preflight_refuses_before_scanning();
	test_alternate_read_failure_keeps_io_status();
	test_truncation_precedes_driver_work();
	test_resource_and_cancellation_statuses();
	printf("HFS Plus verification: %zu corruption cases, %u failures\n",
	    COUNT(corruptions), failures);
	return (failures == 0 ? 0 : 1);
}
