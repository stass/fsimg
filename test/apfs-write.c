/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "fsimg.h"
#include "format.h"
#include "internal.h"
#include "layer.h"
#include "meta.h"
#include "sha256.h"
#include "test.h"
#include "write-apfs.h"
#include "writer.h"

#define BLOCK_BYTES UINT64_C(4096)
#define EPOCH INT64_C(1700000000)

static const char *scenario;
static unsigned failures;

#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
		    scenario, #condition); \
		failures++; \
	} \
} while (0)

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	apfs_plan_t	*plan;
	fsimg_error_t	 error;
	uint64_t	 memory_baseline;
	uint64_t	 descriptor_baseline;
} fixture_t;

static void base_options(fsimg_write_options_t *);
static fsimg_status_t create_writer(fixture_t *, const fsimg_write_options_t *);
static void open_writer(fixture_t *, const fsimg_write_options_t *);
static void close_writer(fixture_t *);
static void add_entry(fixture_t *, const char *, fsimg_entry_kind_t, uint64_t);
static fsimg_status_t plan_writer(fixture_t *);
static apfs_plan_t *plan_ok(fixture_t *);
static void expect_validation(const fsimg_write_options_t *, fsimg_status_t);
static void expect_names(const char *, const char *, bool, fsimg_status_t);
static void check_summary(const apfs_plan_t *);
static void compare_tree(const apfs_tree_plan_t *, const apfs_tree_plan_t *);
static void compare_plans(apfs_plan_t *, apfs_plan_t *);
static fsimg_status_t link_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t link_read(void *, uint64_t, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);

static void
base_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->flags = FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = EPOCH;
}

static fsimg_status_t
create_writer(fixture_t *fixture, const fsimg_write_options_t *options)
{
	fsimg_stats_t stats;

	bzero(fixture, sizeof(*fixture));
	CHECK(fsimg_ctx_new(NULL, &fixture->ctx, &fixture->error) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	fixture->memory_baseline = stats.memory_bytes;
	fixture->descriptor_baseline = stats.descriptors;
	return (fsimg_writer_new_variant(fixture->ctx,
	    &fsimg_apfs_write_format, &fsimg_apfs_write_format.variants[0],
	    options, &fixture->writer, &fixture->error));
}

static void
open_writer(fixture_t *fixture, const fsimg_write_options_t *options)
{
	fsimg_status_t status;

	status = create_writer(fixture, options);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: creating writer: %s\n", scenario,
		    fixture->error.message);
	CHECK(status == FSIMG_OK);
}

static void
close_writer(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	EXPECT(stats.memory_bytes == fixture->memory_baseline);
	EXPECT(stats.scratch_bytes == 0);
	EXPECT(stats.spool_bytes == 0);
	EXPECT(stats.descriptors == fixture->descriptor_baseline);
	fsimg_ctx_free(fixture->ctx);
}

static void
add_entry(fixture_t *fixture, const char *path, fsimg_entry_kind_t kind,
    uint64_t size)
{
	fsimg_entry_t entry = { .path = path, .kind = kind, .size = size };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };

	if (kind == FSIMG_ENTRY_SYMLINK)
		source.kind = FSIMG_SOURCE_PROVIDER;
	CHECK(fsimg_writer_add(fixture->writer, &entry,
	    kind == FSIMG_ENTRY_FILE || kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, &fixture->error) == FSIMG_OK);
}

static fsimg_status_t
plan_writer(fixture_t *fixture)
{
	const fsimg_plan_t *plan;
	fsimg_status_t status;

	status = fsimg_writer_plan(fixture->writer, &plan, &fixture->error);
	if (status == FSIMG_OK)
		fixture->plan = (apfs_plan_t *)plan;
	return (status);
}

static apfs_plan_t *
plan_ok(fixture_t *fixture)
{
	fsimg_status_t status;

	status = plan_writer(fixture);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: planning: %s (%s)\n", scenario,
		    fixture->error.message, fixture->error.path);
	CHECK(status == FSIMG_OK);
	return (fixture->plan);
}

static void
expect_validation(const fsimg_write_options_t *options,
    fsimg_status_t expected)
{
	fixture_t fixture;
	fsimg_status_t status;

	status = create_writer(&fixture, options);
	if (status != expected)
		fprintf(stderr, "%s: validation returned %d, expected %d: %s\n",
		    scenario, status, expected, fixture.error.message);
	EXPECT(status == expected);
	close_writer(&fixture);
}

static void
test_sector_and_allocation_units_are_4096_bytes(void)
{
	static const uint32_t sizes[] = { 0, 512, 2048, 4096, 8192 };
	fsimg_write_options_t options;
	fsimg_status_t expected;

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		expected = sizes[i] == 0 || sizes[i] == 4096 ?
		    FSIMG_OK : FSIMG_EINVAL;
		base_options(&options);
		options.sector_bytes = sizes[i];
		expect_validation(&options, expected);
		base_options(&options);
		options.allocation_unit_bytes = sizes[i];
		expect_validation(&options, expected);
	}
}

static void
test_label_is_utf8_of_at_most_255_bytes(void)
{
	fsimg_write_options_t options;
	char label[257];

	base_options(&options);
	memset(label, 'a', 256);
	label[256] = '\0';
	options.label = label;
	expect_validation(&options, FSIMG_EINVAL);
	label[255] = '\0';
	expect_validation(&options, FSIMG_OK);
	options.label = "\xc0\xaf";
	expect_validation(&options, FSIMG_EINVAL);
}

static void
test_null_label_selects_untitled(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	base_options(&options);
	options.label = NULL;
	open_writer(&fixture, &options);
	EXPECT(strcmp(fixture.writer->options.label, "Untitled") == 0);
	close_writer(&fixture);
}

static void
test_empty_label_plans_with_untitled(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	base_options(&options);
	options.label = "";
	open_writer(&fixture, &options);
	plan_ok(&fixture);
	EXPECT(strcmp(fixture.writer->options.label, "Untitled") == 0);
	close_writer(&fixture);
}

static void
test_container_size_has_block_alignment_and_bounds(void)
{
	fsimg_write_options_t options;

	base_options(&options);
	options.volume_bytes = 1024 * BLOCK_BYTES + 1;
	expect_validation(&options, FSIMG_EINVAL);
	options.volume_bytes = 1023 * BLOCK_BYTES;
	expect_validation(&options, FSIMG_EINVAL);
	options.volume_bytes = 1024 * BLOCK_BYTES;
	expect_validation(&options, FSIMG_OK);
	options.volume_bytes = (UINT64_C(34359738368) + 1) * BLOCK_BYTES;
	expect_validation(&options, FSIMG_ERANGE);
}

static void
test_zero_uuid_requires_reproducible_mode(void)
{
	fsimg_write_options_t options;

	base_options(&options);
	options.flags = 0;
	expect_validation(&options, FSIMG_EINVAL);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	expect_validation(&options, FSIMG_OK);
}

static void
test_common_settings_refuse_placement(void)
{
	fsimg_placement_t placement = { .path = "/file", .offset = 4096 };
	fsimg_write_options_t options;

	base_options(&options);
	options.placements = &placement;
	options.placement_count = 1;
	expect_validation(&options, FSIMG_EINVAL);
}

/* Catalog IDs, including collision diagnostics, follow path order. */
static void
expect_names(const char *first, const char *second, bool sensitive,
    fsimg_status_t expected)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	fsimg_status_t status;
	const char *later;

	base_options(&options);
	options.case_sensitive = sensitive;
	open_writer(&fixture, &options);
	add_entry(&fixture, first, FSIMG_ENTRY_FILE, 0);
	if (second != NULL)
		add_entry(&fixture, second, FSIMG_ENTRY_FILE, 0);
	status = plan_writer(&fixture);
	EXPECT(status == expected);
	if (expected == FSIMG_EINVAL && status == expected) {
		later = second != NULL && strcmp(first, second) < 0 ?
		    second : first;
		if (strlen(later) < sizeof(fixture.error.path))
			EXPECT(strcmp(fixture.error.path, later) == 0);
		else
			EXPECT(fixture.error.path_truncated);
	}
	close_writer(&fixture);
}

static void
test_unassigned_and_noncharacter_names_name_the_entry(void)
{
	expect_names("/unassigned-\xcd\xb8", NULL, false, FSIMG_EINVAL);
	expect_names("/noncharacter-\xef\xbf\xbf", NULL, false, FSIMG_EINVAL);
}

static void
test_case_collisions_follow_volume_personality(void)
{
	expect_names("/Foo", "/foo", false, FSIMG_EINVAL);
	expect_names("/Foo", "/foo", true, FSIMG_OK);
}

static void
test_canonical_equivalents_collide_in_both_personalities(void)
{
	expect_names("/\xc3\xa9", "/e\xcc\x81", false, FSIMG_EINVAL);
	expect_names("/\xc3\xa9", "/e\xcc\x81", true, FSIMG_EINVAL);
}

static void
test_name_limit_counts_utf16_units(void)
{
	char path[1 + 128 * 4 + 1];

	/* A supplementary character uses four UTF-8 bytes and two UTF-16
	 * units: 127 copies and 'a' fit, 128 copies do not.
	 */
	path[0] = '/';
	for (size_t i = 0; i < 128; i++)
		bcopy("\xf0\x9f\x98\x80", path + 1 + i * 4, 4);
	path[513] = '\0';
	expect_names(path, NULL, false, FSIMG_EINVAL);
	path[509] = 'a';
	path[510] = '\0';
	expect_names(path, NULL, false, FSIMG_OK);
}

static void
check_summary(const apfs_plan_t *plan)
{
	const fsimg_plan_info_t *info = &plan->common.info;
	uint64_t prefix_end = 0;

	for (size_t i = 0; i < APFS_FIXED_REGIONS; i++) {
		EXPECT(plan->regions[i].first == prefix_end);
		prefix_end += plan->regions[i].count;
	}
	EXPECT(plan->run_first == prefix_end);
	EXPECT(plan->allocated_blocks == plan->run_first +
	    plan->meta_blocks + plan->data_blocks);
	EXPECT(info->allocated_bytes == plan->allocated_blocks * BLOCK_BYTES);
	EXPECT(plan->pool_bitmaps == (plan->allocated_blocks +
	    plan->sm.blocks_per_chunk - 1) / plan->sm.blocks_per_chunk);
	EXPECT(apfs_run_block(plan, plan->comap_run) == plan->run_first);

	EXPECT(info->volume_bytes == plan->block_count * BLOCK_BYTES);
	EXPECT(info->allocated_bytes + info->free_bytes == info->volume_bytes);
	EXPECT(info->metadata_bytes == info->allocated_bytes -
	    plan->data_blocks * BLOCK_BYTES);
	EXPECT(info->allocation_units == plan->block_count);
}

static void
test_empty_tree_has_native_records_and_minimum_container(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	apfs_plan_t *plan;

	base_options(&options);
	open_writer(&fixture, &options);
	plan = plan_ok(&fixture);
	EXPECT(plan->fs_tree.shape.key_count == 4);
	EXPECT(plan->fs_tree.shape.node_count == 1);
	EXPECT(plan->comap_tree.shape.node_count == 1);
	EXPECT(plan->vomap_tree.shape.node_count == 1);
	EXPECT(plan->extentref_tree.shape.node_count == 1);
	EXPECT(plan->snapshot_tree.shape.node_count == 1);
	EXPECT(plan->vomap_tree.shape.key_count == plan->fs_tree.shape.node_count);
	EXPECT(plan->meta_blocks == 8);
	EXPECT(plan->data_blocks == 0);
	EXPECT(plan->next_ino == 16);
	EXPECT(plan->next_oid == 1027 + plan->fs_tree.shape.node_count);
	EXPECT(plan->block_count == 1024);
	EXPECT(plan->files == 0 && plan->directories == 0 &&
	    plan->symlinks == 0 && plan->others == 0);
	EXPECT(plan->data_files == 0);
	EXPECT(plan->format_time == EPOCH * INT64_C(1000000000));
	check_summary(plan);
	close_writer(&fixture);
}

static fsimg_status_t
link_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
link_read(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    uint64_t offset, void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	static const char target[] = "dir/file";

	CHECK(offset <= sizeof(target) - 1);
	if (length > sizeof(target) - 1 - offset)
		length = sizeof(target) - 1 - offset;
	bcopy(target + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

/* Expected values come from the fixture paths and Writer values, not rows. */
typedef struct expected_entry {
	const char	*name;
	uint64_t	 ino;
	uint64_t	 parent;
	uint64_t	 size;
	uint64_t	 allocated;
	int32_t		 children;
	uint32_t	 generation;
	uint32_t	 protection;
	uint16_t	 type;
	unsigned	 records;
} expected_entry_t;

static const expected_entry_t mixed_entries[] = {
	{ .name = "root", .ino = 2, .parent = 1, .children = 4,
	  .generation = 5, .type = 4, .records = (1u << 3) | (1u << 9) },
	{ .name = "private-dir", .ino = 3, .parent = 1,
	  .generation = 1, .type = 4, .records = (1u << 3) | (1u << 9) },
	{ .name = "dir", .ino = 16, .parent = 2, .children = 2,
	  .generation = 3, .type = 4, .records = (1u << 3) | (1u << 9) },
	{ .name = "empty", .ino = 17, .parent = 16,
	  .generation = 1, .type = 8, .records = (1u << 3) | (1u << 9) },
	{ .name = "file", .ino = 18, .parent = 16, .size = 4097,
	  .allocated = 8192, .generation = 2, .type = 8,
	  .records = (1u << 3) | (1u << 6) | (1u << 8) | (1u << 9) },
	{ .name = "file", .ino = 19, .parent = 2, .size = 1,
	  .allocated = 4096, .generation = 2, .type = 8,
	  .records = (1u << 3) | (1u << 6) | (1u << 8) | (1u << 9) },
	{ .name = "link", .ino = 20, .parent = 2, .size = 8,
	  .generation = 1, .type = 10,
	  .records = (1u << 3) | (1u << 4) | (1u << 9) },
	{ .name = "pipe", .ino = 21, .parent = 2, .protection = 4,
	  .generation = 1, .type = 1, .records = (1u << 3) | (1u << 9) }
};

static void
check_mixed_inode(const apfs_write_record_t *record,
    const expected_entry_t *expected)
{
	apfs_inode_val_t inode;
	apfs_xfield_t field;
	apfs_dstream_t stream;
	uint64_t ino;
	bool has_stream;

	CHECK(apfs_bare_key_decode(record->key, record->key_len,
	    APFS_J_INODE, &ino));
	EXPECT(ino == expected->ino);
	CHECK(apfs_inode_val_decode(record->val, record->val_len, &inode));
	EXPECT(inode.parent == expected->parent);
	EXPECT(inode.private_id == expected->ino);
	EXPECT(inode.count == (expected->type == 4 ? expected->children : 1));
	EXPECT(inode.internal_flags == 0x8000);
	EXPECT(inode.protection_class == expected->protection);
	EXPECT(inode.write_generation == expected->generation);
	EXPECT(inode.uncompressed_size == 0);
	CHECK(apfs_xfield_find(&inode.xfields, 4, &field));
	EXPECT(field.type == 4 && field.flags == 0x2);
	CHECK(field.size == strlen(expected->name) + 1);
	EXPECT(bcmp(field.data, expected->name, field.size) == 0);
	has_stream = expected->allocated != 0;
	EXPECT(inode.xfields.count == (has_stream ? 2 : 1));
	EXPECT(apfs_xfield_find(&inode.xfields, 8, &field) == has_stream);
	if (has_stream) {
		CHECK(apfs_xfield_find(&inode.xfields, 8, &field));
		EXPECT(field.type == 8 && field.flags == 0x20);
		CHECK(apfs_dstream_decode(field.data, field.size, &stream));
		EXPECT(stream.size == expected->size);
		EXPECT(stream.alloced_size == expected->allocated);
		EXPECT(stream.default_crypto_id == 0);
		EXPECT(stream.total_bytes_written == expected->size);
		EXPECT(stream.total_bytes_read == 0);
	}
}

static void
check_mixed_records(apfs_plan_t *plan)
{
	unsigned char frame[APFS_WRITE_RECORD_MAX];
	unsigned seen[sizeof(mixed_entries) / sizeof(mixed_entries[0])] = { 0 };
	const expected_entry_t *expected;
	apfs_write_record_t record;
	apfs_jkey_t key;
	apfs_drec_key_t dkey;
	apfs_drec_val_t dval;
	apfs_extent_key_t ekey;
	apfs_extent_val_t eval;
	apfs_name_key_t xkey;
	apfs_xattr_val_t xval;
	apfs_name_form_t form;
	apfs_name_rule_t rule;
	uint64_t cursor = 0, ino;
	uint32_t refcount;
	size_t length, index;
	char context[128];
	const char *saved_scenario = scenario;

	rule = plan->case_sensitive ? APFS_NAMES_NFD : APFS_NAMES_CASELESS;
	for (uint64_t i = 0; i < plan->records->count; i++) {
		CHECK(fsimg_stream_next(plan->records, &cursor, frame,
		    sizeof(frame), &length, NULL) == FSIMG_OK);
		CHECK(apfs_write_record_decode(frame, length, &record));
		CHECK(apfs_jkey_decode(record.key, record.key_len, &key));
		ino = key.oid;
		if (key.kind == APFS_J_DREC) {
			CHECK(apfs_drec_val_decode(record.val, record.val_len, &dval));
			ino = dval.child;
		}
		snprintf(context, sizeof(context), "mixed inode %llu kind %u",
		    (unsigned long long)ino, key.kind);
		scenario = context;
		for (index = 0; index < sizeof(mixed_entries) /
		    sizeof(mixed_entries[0]); index++) {
			if (mixed_entries[index].ino == ino)
				break;
		}
		CHECK(index < sizeof(mixed_entries) / sizeof(mixed_entries[0]));
		expected = &mixed_entries[index];
		CHECK(key.kind < 32);
		EXPECT((expected->records & (1u << key.kind)) != 0);
		EXPECT((seen[index] & (1u << key.kind)) == 0);
		seen[index] |= 1u << key.kind;
		switch (key.kind) {
		case APFS_J_INODE:
			check_mixed_inode(&record, expected);
			break;
		case APFS_J_DREC:
			CHECK(apfs_drec_key_decode(record.key, record.key_len,
			    true, &dkey));
			EXPECT(dkey.parent == expected->parent);
			CHECK(dkey.name_len == strlen(expected->name));
			EXPECT(bcmp(dkey.name, expected->name, dkey.name_len + 1) == 0);
			CHECK(apfs_name_form(rule, expected->name,
			    strlen(expected->name), &form) == FSIMG_OK);
			EXPECT(dkey.hash == apfs_name_hash(&form));
			EXPECT(dval.child == expected->ino);
			EXPECT((dval.flags & 0xf) == expected->type);
			EXPECT(dval.date_added == plan->format_time);
			break;
		case APFS_J_DSTREAM_ID:
			CHECK(apfs_bare_key_decode(record.key, record.key_len,
			    APFS_J_DSTREAM_ID, &ino));
			EXPECT(ino == expected->ino);
			CHECK(apfs_dstream_id_val_decode(record.val,
			    record.val_len, &refcount));
			EXPECT(refcount == 1);
			break;
		case APFS_J_FILE_EXTENT:
			CHECK(apfs_extent_key_decode(record.key, record.key_len, &ekey));
			CHECK(apfs_extent_val_decode(record.val, record.val_len, &eval));
			EXPECT(ekey.private_id == expected->ino);
			EXPECT(ekey.logical == 0);
			EXPECT(eval.length == expected->allocated);
			EXPECT(eval.phys_block == 0);
			EXPECT(eval.flags == 0 && eval.crypto_id == 0);
			break;
		case APFS_J_XATTR:
			CHECK(apfs_xattr_key_decode(record.key, record.key_len, &xkey));
			CHECK(apfs_xattr_val_decode(record.val, record.val_len, &xval));
			EXPECT(xkey.oid == 20);
			CHECK(xkey.name_len == sizeof("com.apple.fs.symlink") - 1);
			EXPECT(bcmp(xkey.name, "com.apple.fs.symlink",
			    sizeof("com.apple.fs.symlink")) == 0);
			EXPECT(xval.flags == (0x2 | 0x4));
			CHECK(xval.data_len == sizeof("dir/file"));
			CHECK(xval.data != NULL);
			EXPECT(bcmp(xval.data, "dir/file", sizeof("dir/file")) == 0);
			break;
		default:
			EXPECT(false);
			break;
		}
	}
	for (index = 0; index < sizeof(mixed_entries) /
	    sizeof(mixed_entries[0]); index++) {
		snprintf(context, sizeof(context), "mixed inode %llu records",
		    (unsigned long long)mixed_entries[index].ino);
		scenario = context;
		EXPECT(seen[index] == mixed_entries[index].records);
	}
	scenario = saved_scenario;
}

static void
test_mixed_catalog_counts_records_children_and_contiguous_data(void)
{
	fsimg_write_options_t options;
	fsimg_source_ops_t sources;
	fixture_t fixture;
	apfs_plan_t *plan;
	apfs_row_t row;
	uint64_t blocks_before = 0, blocks;

	base_options(&options);
	open_writer(&fixture, &options);
	fsimg_source_ops_init(&sources);
	sources.validate = link_validate;
	sources.read_at = link_read;
	CHECK(fsimg_writer_set_sources(fixture.writer, &sources, NULL,
	    &fixture.error) == FSIMG_OK);
	add_entry(&fixture, "/dir", FSIMG_ENTRY_DIRECTORY, 0);
	add_entry(&fixture, "/dir/empty", FSIMG_ENTRY_FILE, 0);
	add_entry(&fixture, "/dir/file", FSIMG_ENTRY_FILE, 4097);
	add_entry(&fixture, "/file", FSIMG_ENTRY_FILE, 1);
	add_entry(&fixture, "/link", FSIMG_ENTRY_SYMLINK, 8);
	add_entry(&fixture, "/pipe", FSIMG_ENTRY_FIFO, 0);
	plan = plan_ok(&fixture);

	EXPECT(plan->files == 3 && plan->directories == 1 &&
	    plan->symlinks == 1 && plan->others == 1);
	EXPECT(plan->data_blocks == 3);
	EXPECT(plan->data_files == 2);
	/* Six inode/name pairs, two stream/extent pairs, one link attribute,
	 * and the four native records. Empty files have no stream records.
	 */
	EXPECT(plan->fs_tree.shape.key_count == 4 + 6 * 2 + 2 * 2 + 1);
	CHECK(apfs_row_get(plan, 0, &row, NULL) == FSIMG_OK);
	EXPECT(row.children == 4);
	CHECK(apfs_row_get(plan, 1, &row, NULL) == FSIMG_OK);
	EXPECT(row.kind == FSIMG_ENTRY_DIRECTORY && row.children == 2);
	for (uint64_t id = 1; id <= 6; id++) {
		CHECK(apfs_row_get(plan, id, &row, NULL) == FSIMG_OK);
		blocks = apfs_row_blocks(&row);
		if (blocks == 0)
			continue;
		EXPECT(row.blocks_before == blocks_before);
		EXPECT(row.first_block == plan->run_first + plan->meta_blocks +
		    blocks_before);
		EXPECT(row.first_block < plan->block_count);
		EXPECT(blocks <= plan->block_count - row.first_block);
		blocks_before += blocks;
	}
	EXPECT(blocks_before == plan->data_blocks);
	check_summary(plan);
	check_mixed_records(plan);
	close_writer(&fixture);
}

/* Literal little-endian keys are independent of the writer's encoders.
 * The high nibble of byte 7 is the kind; the other header bits are owner.
 */
static void
test_hashed_key_order_uses_owner_kind_hash_name_and_logical_offset(void)
{
	apfs_compare_fn_t compare = apfs_fs_tree_profile(true)->compare;
	static const unsigned char inode_255[] = {
		0xff, 0, 0, 0, 0, 0, 0, 0x30
	};
	static const unsigned char inode_256[] = {
		0, 1, 0, 0, 0, 0, 0, 0x30
	};
	static const unsigned char inode_2[] = {
		2, 0, 0, 0, 0, 0, 0, 0x30
	};
	static const unsigned char inode_3[] = {
		3, 0, 0, 0, 0, 0, 0, 0x30
	};
	static const unsigned char drec_2[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 2, 4, 0, 0, 'a', 0
	};
	/* Hash 1, length 3 precedes hash 2, length 2. The lengths sort
	 * the other way, so they must not participate in the hash order.
	 */
	static const unsigned char hash_1_long_name[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 3, 4, 0, 0, 'z', 'z', 0
	};
	static const unsigned char hash_2_short_name[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 2, 8, 0, 0, 'a', 0
	};
	/* Same hash: compare the stored names, including the ending NUL. */
	static const unsigned char hash_1_prefix[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 2, 4, 0, 0, 'a', 0
	};
	static const unsigned char hash_1_extension[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 3, 4, 0, 0, 'a', 'b', 0
	};
	/* A shorter name may still sort later: the length is not a key. */
	static const unsigned char hash_1_short_later[] = {
		2, 0, 0, 0, 0, 0, 0, 0x90, 2, 4, 0, 0, 'z', 0
	};
	static const unsigned char extent_255[] = {
		2, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0, 0, 0, 0, 0, 0, 0
	};
	static const unsigned char extent_256[] = {
		2, 0, 0, 0, 0, 0, 0, 0x80, 0, 1, 0, 0, 0, 0, 0, 0
	};

#define EXPECT_BEFORE(a, b) do { \
	EXPECT(compare(a, sizeof(a), b, sizeof(b)) < 0); \
	EXPECT(compare(b, sizeof(b), a, sizeof(a)) > 0); \
} while (0)
	EXPECT_BEFORE(inode_255, inode_256);
	EXPECT_BEFORE(inode_2, drec_2);
	EXPECT_BEFORE(drec_2, inode_3);
	EXPECT_BEFORE(hash_1_long_name, hash_2_short_name);
	EXPECT_BEFORE(hash_1_prefix, hash_1_extension);
	EXPECT_BEFORE(hash_1_extension, hash_1_short_later);
	EXPECT_BEFORE(extent_255, extent_256);
#undef EXPECT_BEFORE
	EXPECT(compare(hash_1_extension,
	    sizeof(hash_1_extension), hash_1_extension,
	    sizeof(hash_1_extension)) == 0);
}

static void
test_one_byte_above_largest_container_is_range_error(void)
{
	fsimg_write_options_t options;

	base_options(&options);
	options.volume_bytes = UINT64_C(140737488355329);
	expect_validation(&options, FSIMG_ERANGE);
}

static void
test_wide_directory_has_multiple_leaves_and_strictly_sorted_records(void)
{
	unsigned char frame[APFS_WRITE_RECORD_MAX], previous[APFS_WRITE_KEY_MAX];
	fsimg_write_options_t options;
	apfs_write_record_t record;
	fixture_t fixture;
	apfs_plan_t *plan;
	uint64_t cursor = 0, nodes = 0;
	size_t length, previous_length = 0;
	char path[32];

	base_options(&options);
	open_writer(&fixture, &options);
	add_entry(&fixture, "/wide", FSIMG_ENTRY_DIRECTORY, 0);
	for (unsigned i = 0; i < 3000; i++) {
		snprintf(path, sizeof(path), "/wide/file-%04u", i);
		add_entry(&fixture, path, FSIMG_ENTRY_FILE, 0);
	}
	plan = plan_ok(&fixture);
	EXPECT(plan->fs_tree.shape.levels > 1);
	for (unsigned i = 0; i < plan->fs_tree.shape.levels; i++)
		nodes += plan->fs_tree.shape.level_nodes[i];
	EXPECT(nodes == plan->fs_tree.shape.node_count);
	EXPECT(plan->vomap_tree.shape.key_count == nodes);
	EXPECT(plan->records->count == 4 + 3001 * 2);
	for (uint64_t i = 0; i < plan->records->count; i++) {
		CHECK(fsimg_stream_next(plan->records, &cursor, frame,
		    sizeof(frame), &length, NULL) == FSIMG_OK);
		CHECK(apfs_write_record_decode(frame, length, &record));
		if (previous_length != 0)
			EXPECT(apfs_fs_tree_profile(true)->compare(previous,
			    previous_length, record.key, record.key_len) < 0);
		CHECK(record.key_len <= sizeof(previous));
		bcopy(record.key, previous, record.key_len);
		previous_length = record.key_len;
	}
	close_writer(&fixture);
}

static void
compare_tree(const apfs_tree_plan_t *a, const apfs_tree_plan_t *b)
{
	EXPECT(a->first_run == b->first_run);
	EXPECT(a->shape.levels == b->shape.levels);
	EXPECT(a->shape.node_count == b->shape.node_count);
	EXPECT(a->shape.key_count == b->shape.key_count);
	EXPECT(a->shape.root_oid == b->shape.root_oid);
	for (unsigned i = 0; i < APFS_BTREE_LEVELS_MAX; i++)
		EXPECT(a->shape.level_nodes[i] == b->shape.level_nodes[i]);
}

/* Compare fields explicitly: native struct padding is not a plan value. */
static void
compare_plans(apfs_plan_t *a, apfs_plan_t *b)
{
	unsigned char one[APFS_WRITE_RECORD_MAX], two[APFS_WRITE_RECORD_MAX];
	apfs_row_t ar, br;
	uint64_t ac = 0, bc = 0;
	size_t an, bn;

	EXPECT(a->block_count == b->block_count);
	compare_tree(&a->comap_tree, &b->comap_tree);
	compare_tree(&a->vomap_tree, &b->vomap_tree);
	compare_tree(&a->fs_tree, &b->fs_tree);
	compare_tree(&a->extentref_tree, &b->extentref_tree);
	compare_tree(&a->snapshot_tree, &b->snapshot_tree);
	EXPECT(a->run_first == b->run_first);
	EXPECT(a->allocated_blocks == b->allocated_blocks);
	for (size_t i = 0; i < APFS_FIXED_REGIONS; i++) {
		EXPECT(a->regions[i].first == b->regions[i].first);
		EXPECT(a->regions[i].count == b->regions[i].count);
	}
	CHECK(a->common.writer->count == b->common.writer->count);
	for (uint64_t id = 0; id <= a->common.writer->count; id++) {
		CHECK(apfs_row_get(a, id, &ar, NULL) == FSIMG_OK);
		CHECK(apfs_row_get(b, id, &br, NULL) == FSIMG_OK);
		EXPECT(ar.first_block == br.first_block);
		EXPECT(ar.blocks_before == br.blocks_before);
	}
	CHECK(a->records->count == b->records->count);
	for (uint64_t i = 0; i < a->records->count; i++) {
		CHECK(fsimg_stream_next(a->records, &ac, one, sizeof(one),
		    &an, NULL) == FSIMG_OK);
		CHECK(fsimg_stream_next(b->records, &bc, two, sizeof(two),
		    &bn, NULL) == FSIMG_OK);
		EXPECT(an == bn);
		if (an == bn)
			EXPECT(bcmp(one, two, an) == 0);
	}
}

static void
test_automatic_size_fits_and_minimize_uses_same_layout(void)
{
	fsimg_write_options_t options;
	fixture_t automatic, exact, too_small, minimized;
	apfs_plan_t *plan;

	base_options(&options);
	open_writer(&automatic, &options);
	add_entry(&automatic, "/large", FSIMG_ENTRY_FILE, 3000 * BLOCK_BYTES);
	plan = plan_ok(&automatic);
	EXPECT(plan->block_count > 1024);
	check_summary(plan);

	options.volume_bytes = plan->block_count * BLOCK_BYTES;
	open_writer(&exact, &options);
	add_entry(&exact, "/large", FSIMG_ENTRY_FILE, 3000 * BLOCK_BYTES);
	plan_ok(&exact);
	compare_plans(plan, exact.plan);
	close_writer(&exact);

	/* The fixed-region prefix never shrinks as N grows for the native
	 * geometry. Thus the first fit is the smallest fitting block count
	 * (design-apfs.md, Layout), and one fewer block must be too small.
	 */
	options.volume_bytes = (plan->block_count - 1) * BLOCK_BYTES;
	open_writer(&too_small, &options);
	add_entry(&too_small, "/large", FSIMG_ENTRY_FILE, 3000 * BLOCK_BYTES);
	EXPECT(plan_writer(&too_small) == FSIMG_ERANGE);
	close_writer(&too_small);

	options.volume_bytes = 0;
	options.flags |= FSIMG_WRITE_MINIMIZE;
	open_writer(&minimized, &options);
	add_entry(&minimized, "/large", FSIMG_ENTRY_FILE, 3000 * BLOCK_BYTES);
	plan_ok(&minimized);
	compare_plans(plan, minimized.plan);
	close_writer(&minimized);
	close_writer(&automatic);
}

static void
test_exact_container_too_small_refuses_content(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;

	base_options(&options);
	options.volume_bytes = 1024 * BLOCK_BYTES;
	open_writer(&fixture, &options);
	add_entry(&fixture, "/large", FSIMG_ENTRY_FILE, 3000 * BLOCK_BYTES);
	EXPECT(plan_writer(&fixture) == FSIMG_ERANGE);
	close_writer(&fixture);
}

static void
test_minimize_refuses_exact_volume_size(void)
{
	fsimg_write_options_t options;

	base_options(&options);
	options.flags |= FSIMG_WRITE_MINIMIZE;
	options.volume_bytes = 1024 * BLOCK_BYTES;
	expect_validation(&options, FSIMG_EINVAL);
}

static void
test_minimum_free_bytes_raises_automatic_size(void)
{
	fsimg_write_options_t options;
	fixture_t automatic, reserved;

	base_options(&options);
	open_writer(&automatic, &options);
	plan_ok(&automatic);
	options.min_free_bytes = 3000 * BLOCK_BYTES + 1;
	open_writer(&reserved, &options);
	plan_ok(&reserved);
	EXPECT(reserved.plan->block_count > automatic.plan->block_count);
	EXPECT(reserved.plan->common.info.free_bytes >= options.min_free_bytes);
	check_summary(reserved.plan);
	close_writer(&reserved);
	close_writer(&automatic);
}

static void
test_source_order_does_not_change_plan(void)
{
	fsimg_write_options_t options;
	fixture_t forward, reverse;
	char path[32];

	base_options(&options);
	open_writer(&forward, &options);
	open_writer(&reverse, &options);
	for (unsigned i = 0; i < 100; i++) {
		snprintf(path, sizeof(path), "/file-%03u", i);
		add_entry(&forward, path, FSIMG_ENTRY_FILE, i * 111);
		snprintf(path, sizeof(path), "/file-%03u", 99 - i);
		add_entry(&reverse, path, FSIMG_ENTRY_FILE, (99 - i) * 111);
	}
	plan_ok(&forward);
	plan_ok(&reverse);
	compare_plans(forward.plan, reverse.plan);
	close_writer(&reverse);
	close_writer(&forward);
}

static void
test_reproducible_time_equal_to_epoch_is_exact(void)
{
	fsimg_write_options_t options;
	fsimg_timestamp_t timestamp = { .seconds = EPOCH, .nanoseconds = 0,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
	fsimg_entry_t entry = { .path = "/file", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
	fixture_t fixture;
	fsimg_status_t status;
	apfs_row_t row;

	base_options(&options);
	options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
	entry.metadata.valid = FSIMG_META_CREATED | FSIMG_META_MODIFIED |
	    FSIMG_META_ACCESSED | FSIMG_META_CHANGED;
	entry.metadata.created = timestamp;
	entry.metadata.modified = timestamp;
	entry.metadata.accessed = timestamp;
	entry.metadata.changed = timestamp;
	open_writer(&fixture, &options);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source,
	    &fixture.error) == FSIMG_OK);
	status = plan_writer(&fixture);
	EXPECT(status == FSIMG_OK);
	if (status == FSIMG_OK) {
		EXPECT(fixture.plan->common.info.metadata_losses == 0);
		CHECK(apfs_row_get(fixture.plan, 1, &row, NULL) == FSIMG_OK);
		EXPECT(row.created == EPOCH * INT64_C(1000000000));
		EXPECT(row.modified == EPOCH * INT64_C(1000000000));
		EXPECT(row.accessed == EPOCH * INT64_C(1000000000));
		EXPECT(row.changed == EPOCH * INT64_C(1000000000));
	}
	close_writer(&fixture);

	/* A present value different from the epoch loses one field. */
	entry.metadata.valid = FSIMG_META_MODIFIED;
	entry.metadata.modified.seconds = EPOCH + 1;
	open_writer(&fixture, &options);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source,
	    &fixture.error) == FSIMG_OK);
	EXPECT(plan_writer(&fixture) == FSIMG_EUNSUPPORTED);
	close_writer(&fixture);

	options.flags |= FSIMG_WRITE_ALLOW_LOSS;
	open_writer(&fixture, &options);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source,
	    &fixture.error) == FSIMG_OK);
	status = plan_writer(&fixture);
	EXPECT(status == FSIMG_OK);
	if (status == FSIMG_OK)
		EXPECT(fixture.plan->common.info.metadata_losses == 1);
	close_writer(&fixture);
}

static void
test_reproducible_uuids_use_apfs_role_and_distinct_container(void)
{
	unsigned char expected[16];
	fsimg_write_options_t options;
	fixture_t fixture;
	apfs_plan_t *plan;

	base_options(&options);
	open_writer(&fixture, &options);
	plan = plan_ok(&fixture);
	fsimg_identifier_derive(EPOCH, FSIMG_ROLE_APFS, 0, expected);
	EXPECT(bcmp(plan->volume_uuid, expected, sizeof(expected)) == 0);
	EXPECT((plan->container_uuid[6] & 0xf0) == 0x80);
	EXPECT((plan->container_uuid[8] & 0xc0) == 0x80);
	EXPECT(bcmp(plan->container_uuid, plan->volume_uuid, 16) != 0);
	close_writer(&fixture);
}

/* Emission fixtures describe the source independently of the plan's rows.
 * Files use an offset-dependent pattern, so shifted or repeated copies fail.
 */
#define EMIT_ENTRIES_MAX 4096u
#define EMIT_PATH_MAX 512u
#define EMIT_META (FSIMG_META_MODE | FSIMG_META_OWNER | FSIMG_META_CREATED | \
    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED | FSIMG_META_CHANGED)

typedef struct emit_entry {
	fsimg_entry_t	 entry;
	char		 path[EMIT_PATH_MAX];
	char		 target[1024];
} emit_entry_t;

typedef struct emit_catalog {
	emit_entry_t	*entries;
	size_t		 count;
	bool		 changed_link;
} emit_catalog_t;

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	uint64_t	 initialized_bytes;
	uint64_t	 zeroing_writes;
	uint64_t	 first_driver_offset;
	uint64_t	 last_write_offset;
	uint64_t	 fail_write_at;
	bool		 fail_flush;
	bool		 zero_callback;
} memory_sink_t;

static unsigned char
content_byte(uint64_t offset)
{
	return ((unsigned char)((offset * 29 + offset / 251 + 17) % 256));
}

static void
catalog_init(emit_catalog_t *catalog)
{
	bzero(catalog, sizeof(*catalog));
	catalog->entries = calloc(EMIT_ENTRIES_MAX, sizeof(*catalog->entries));
	CHECK(catalog->entries != NULL);
}

static void
catalog_add(emit_catalog_t *catalog, const char *path,
    fsimg_entry_kind_t kind, uint64_t size)
{
	emit_entry_t *spec;
	fsimg_metadata_t *metadata;
	fsimg_timestamp_t timestamp = { .seconds = EPOCH,
	    .precision_ns = 1, .basis = FSIMG_TIME_UTC };

	CHECK(catalog->count < EMIT_ENTRIES_MAX);
	spec = &catalog->entries[catalog->count++];
	CHECK(strlen(path) < sizeof(spec->path));
	strcpy(spec->path, path);
	spec->entry = (fsimg_entry_t){ .path = spec->path, .kind = kind,
	    .size = size };
	metadata = &spec->entry.metadata;
	metadata->valid = EMIT_META;
	metadata->uid = 70000;
	metadata->gid = 80000;
	metadata->mode = kind == FSIMG_ENTRY_SYMLINK ? 0777 :
	    kind == FSIMG_ENTRY_DIRECTORY ? 0750 : 0640;
	metadata->created = timestamp;
	metadata->modified = timestamp;
	metadata->accessed = timestamp;
	metadata->changed = timestamp;
	if (kind == FSIMG_ENTRY_SYMLINK) {
		CHECK(size > 0 && size < sizeof(spec->target));
		memset(spec->target, 'x', (size_t)size);
	}
}

static void
catalog_mixed(emit_catalog_t *catalog)
{
	static const uint64_t sizes[] = { 0, 1, 4095, 4096, 4097,
	    3 * 1024 * 1024 + 137 };
	char path[EMIT_PATH_MAX] = "";
	size_t used;

	catalog_add(catalog, "/files", FSIMG_ENTRY_DIRECTORY, 0);
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(path, sizeof(path), "/files/size-%llu",
		    (unsigned long long)sizes[i]);
		catalog_add(catalog, path, FSIMG_ENTRY_FILE, sizes[i]);
	}
	catalog_add(catalog, "/link-one", FSIMG_ENTRY_SYMLINK, 1);
	catalog_add(catalog, "/link-long", FSIMG_ENTRY_SYMLINK, 1023);
	catalog_add(catalog, "/pipe", FSIMG_ENTRY_FIFO, 0);
	path[0] = '\0';
	for (unsigned i = 0; i < 20; i++) {
		used = strlen(path);
		snprintf(path + used, sizeof(path) - used, "/depth-%02u", i);
		catalog_add(catalog, path, FSIMG_ENTRY_DIRECTORY, 0);
	}
	strcat(path, "/bottom");
	catalog_add(catalog, path, FSIMG_ENTRY_FILE, 37);
}

static void
catalog_many(emit_catalog_t *catalog, bool wide)
{
	char path[256];
	unsigned count = wide ? 3000 : 2000;

	if (wide)
		catalog_add(catalog, "/wide", FSIMG_ENTRY_DIRECTORY, 0);
	for (unsigned i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "%s/file-%04u", wide ? "/wide" :
		    "", i);
		catalog_add(catalog, path, FSIMG_ENTRY_FILE, i % 7 + 1);
	}
}

static fsimg_status_t
catalog_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	const emit_catalog_t *catalog = argument;
	const emit_entry_t *spec;
	unsigned char *out = buffer;

	CHECK(id < catalog->count);
	spec = &catalog->entries[id];
	CHECK(offset <= spec->entry.size);
	if (length > spec->entry.size - offset)
		length = (size_t)(spec->entry.size - offset);
	if (spec->entry.kind == FSIMG_ENTRY_SYMLINK) {
		bcopy(spec->target + offset, out, length);
		if (catalog->changed_link && length != 0)
			out[0] = 'y';
	} else {
		for (size_t i = 0; i < length; i++)
			out[i] = content_byte(offset + i);
	}
	*transferred = length;
	return (FSIMG_OK);
}

/* A one-page scratch cache is selected only by the identity variants. */
static void
prepare_catalog(fixture_t *fixture, const emit_catalog_t *catalog,
    const fsimg_write_options_t *options, const fsimg_options_t *context,
    bool reverse)
{
	fsimg_source_ops_t sources;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER };
	fsimg_stats_t stats;
	size_t index;

	bzero(fixture, sizeof(*fixture));
	CHECK(fsimg_ctx_new(context, &fixture->ctx, &fixture->error) == FSIMG_OK);
	if (context != NULL && context->scratch_directory_fd != -1)
		CHECK(fsimg_test_meta_cache(fixture->ctx, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	fixture->memory_baseline = stats.memory_bytes;
	fixture->descriptor_baseline = stats.descriptors;
	CHECK(fsimg_writer_new_variant(fixture->ctx, &fsimg_apfs_write_format,
	    &fsimg_apfs_write_format.variants[0], options, &fixture->writer,
	    &fixture->error) == FSIMG_OK);
	fsimg_source_ops_init(&sources);
	sources.validate = link_validate;
	sources.read_at = catalog_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &sources,
	    (void *)catalog, &fixture->error) == FSIMG_OK);
	for (size_t i = 0; i < catalog->count; i++) {
		const fsimg_entry_t *entry;

		index = reverse ? catalog->count - 1 - i : i;
		entry = &catalog->entries[index].entry;
		source.id = index;
		CHECK(fsimg_writer_add(fixture->writer, entry,
		    entry->kind == FSIMG_ENTRY_FILE ||
		    entry->kind == FSIMG_ENTRY_SYMLINK ? &source : NULL,
		    &fixture->error) == FSIMG_OK);
	}
}

static void
plan_catalog(fixture_t *fixture, const emit_catalog_t *catalog,
    const fsimg_write_options_t *options, const fsimg_options_t *context,
    bool reverse)
{
	prepare_catalog(fixture, catalog, options, context, reverse);
	plan_ok(fixture);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size,
    fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	*size = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*transferred = 0;
	sink->writes++;
	sink->last_write_offset = offset;
	if (sink->initialized_bytes < sink->length) {
		CHECK(offset == sink->initialized_bytes);
		CHECK(length <= sink->length - sink->initialized_bytes);
		sink->initialized_bytes += length;
		sink->zeroing_writes++;
	} else if (sink->writes == sink->zeroing_writes + 1)
		sink->first_driver_offset = offset;
	if (sink->writes == sink->fail_write_at)
		return (FSIMG_EIO);
	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	unsigned char zeros[8192] = { 0 };
	fsimg_status_t status;
	size_t batch, transferred;

	while (length != 0) {
		batch = length > sizeof(zeros) ? sizeof(zeros) : (size_t)length;
		status = memory_write(argument, offset, zeros, batch,
		    &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		CHECK(transferred == batch);
		offset += batch;
		length -= batch;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	const memory_sink_t *sink = argument;

	return (sink->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

static fsimg_status_t
memory_io(fixture_t *fixture, memory_sink_t *sink, fsimg_io_t **io)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.flush = memory_flush;
	if (sink->zero_callback)
		ops.zero_range = memory_zero;
	*io = NULL;
	return (fsimg_io_from_callbacks(fixture->ctx, &ops, sink, io,
	    &fixture->error));
}

static void
sink_open(const fixture_t *fixture, memory_sink_t *sink)
{
	bzero(sink, sizeof(*sink));
	sink->length = fixture->plan->block_count * BLOCK_BYTES;
	sink->bytes = malloc((size_t)sink->length);
	CHECK(sink->bytes != NULL);
	memset(sink->bytes, 0xa5, (size_t)sink->length);
}

static fsimg_status_t
emit_into(fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_io_t *io;
	fsimg_status_t status;

	status = memory_io(fixture, sink, &io);
	if (status == FSIMG_OK)
		status = fsimg_writer_emit(fixture->writer, io, &fixture->error);
	fsimg_io_free(io);
	return (status);
}

static bool
emit_ok(fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_status_t status;

	sink_open(fixture, sink);
	status = emit_into(fixture, sink);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: emit returned %d: %s (%s)\n", scenario,
		    status, fixture->error.message, fixture->error.path);
	EXPECT(status == FSIMG_OK);
	return (status == FSIMG_OK);
}

static const unsigned char *
image_block(const memory_sink_t *sink, uint64_t block)
{
	CHECK(block < sink->length / BLOCK_BYTES);
	return (sink->bytes + block * BLOCK_BYTES);
}

/* Print the first unexpected byte, including its absolute image offset. */
static void
expect_zero(const memory_sink_t *sink, uint64_t offset, uint64_t length)
{
	CHECK(offset <= sink->length && length <= sink->length - offset);
	for (uint64_t i = 0; i < length; i++) {
		if (sink->bytes[offset + i] == 0)
			continue;
		fprintf(stderr, "%s: image byte %llu: got %#x, expected zero\n",
		    scenario, (unsigned long long)(offset + i),
		    sink->bytes[offset + i]);
		failures++;
		break;
	}
}

static void
check_object(const memory_sink_t *sink, uint64_t block, uint64_t blocks)
{
	apfs_obj_t object;
	const unsigned char *bytes = image_block(sink, block);

	CHECK(blocks <= sink->length / BLOCK_BYTES - block);
	CHECK(apfs_obj_decode(bytes, (size_t)(blocks * BLOCK_BYTES), &object));
	if (!apfs_object_verify(bytes, (size_t)(blocks * BLOCK_BYTES))) {
		fprintf(stderr, "%s: object at block %llu, OID %llu: "
		    "invalid Fletcher checksum over %llu blocks\n", scenario,
		    (unsigned long long)block, (unsigned long long)object.oid,
		    (unsigned long long)blocks);
		failures++;
	}
	EXPECT(object.xid == 1);
}

static void
check_tree_bytes(const apfs_plan_t *plan, const memory_sink_t *sink,
    const apfs_tree_plan_t *tree, const char *role)
{
	apfs_node_t node;
	uint32_t toc_bytes;
	uint64_t block;
	char context[256];
	const char *saved_scenario = scenario;

	for (uint64_t k = 0; k < tree->shape.node_count; k++) {
		block = apfs_run_block(plan, tree->first_run + k);
		check_object(sink, block, 1);
		CHECK(apfs_node_decode(image_block(sink, block), BLOCK_BYTES,
		    &node));
		snprintf(context, sizeof(context), "%s: %s block %llu level %u",
		    saved_scenario, role, (unsigned long long)block, node.level);
		scenario = context;
		toc_bytes = (node.flags & APFS_NODE_FIXED_KV) != 0 ?
		    APFS_TOC_FIXED_BYTES : APFS_TOC_VARIABLE_BYTES;
		expect_zero(sink, block * BLOCK_BYTES + node.area.free_start,
		    node.area.free_end - node.area.free_start);
		expect_zero(sink, block * BLOCK_BYTES + node.area.toc_start +
		    node.key_count * toc_bytes, node.toc.len -
		    node.key_count * toc_bytes);
		if ((node.flags & APFS_NODE_ROOT) == 0)
			EXPECT(node.free.len >= APFS_BTREE_INFO_BYTES);
	}
	scenario = saved_scenario;
}

/* Decode all leaf mappings, requiring a bijection with filesystem nodes. */
static void
check_volume_node_mappings(const apfs_plan_t *plan,
    const memory_sink_t *sink)
{
	const apfs_tree_plan_t *tree = &plan->vomap_tree;
	apfs_kv_size_t sizes = { APFS_OMAP_KEY_BYTES, APFS_OMAP_VAL_BYTES };
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_entry_t entry;
	apfs_node_t node;
	const unsigned char *bytes;
	bool *seen;
	uint64_t index, count = 0;

	seen = calloc((size_t)plan->fs_tree.shape.node_count, sizeof(*seen));
	CHECK(seen != NULL);
	for (uint64_t k = 0; k < tree->shape.node_count; k++) {
		bytes = image_block(sink, apfs_run_block(plan, tree->first_run + k));
		CHECK(apfs_node_decode(bytes, BLOCK_BYTES, &node));
		if (node.level != 0)
			continue;
		for (uint32_t i = 0; i < node.key_count; i++) {
			CHECK(apfs_entry_decode(bytes, BLOCK_BYTES, &node, i,
			    &sizes, &entry));
			CHECK(apfs_omap_key_decode(bytes + entry.key_off,
			    entry.key_len, &key));
			CHECK(apfs_omap_val_decode(bytes + entry.val_off,
			    entry.val_len, &value));
			CHECK(key.oid >= APFS_WRITE_OID_FS_FIRST);
			index = key.oid - APFS_WRITE_OID_FS_FIRST;
			CHECK(index < plan->fs_tree.shape.node_count);
			EXPECT(!seen[index]);
			seen[index] = true;
			count++;
			EXPECT(key.xid == 1);
			EXPECT(value.flags == 0 && value.size == BLOCK_BYTES);
			EXPECT(value.address == apfs_run_block(plan,
			    plan->fs_tree.first_run + index));
		}
	}
	EXPECT(count == plan->fs_tree.shape.node_count);
	for (uint64_t i = 0; i < plan->fs_tree.shape.node_count; i++)
		EXPECT(seen[i]);
	free(seen);
}

static void
check_bitmap(const memory_sink_t *sink, uint64_t block, uint64_t first,
    uint64_t end)
{
	const unsigned char *bits = image_block(sink, block);
	bool actual, expected;

	for (uint64_t bit = 0; bit < BLOCK_BYTES * 8; bit++) {
		actual = (bits[bit / 8] & (1u << (bit % 8))) != 0;
		expected = first + bit < end;
		if (actual == expected)
			continue;
		fprintf(stderr, "%s: bitmap block %llu bit %llu: got %u, "
		    "expected %u\n", scenario, (unsigned long long)block,
		    (unsigned long long)bit, actual, expected);
		failures++;
		break;
	}
}

static void
check_container_bytes(const apfs_plan_t *plan, const memory_sink_t *sink)
{
	const unsigned char *bytes;
	apfs_cpm_block_t mappings;
	apfs_cpm_t mapping;
	apfs_spaceman_t sm;
	apfs_chunk_info_t chunk;
	apfs_cib_t cib;
	apfs_nx_t nx;
	uint64_t free_count = 0, block, used;

	CHECK(apfs_nx_decode(sink->bytes, BLOCK_BYTES, &nx));
	EXPECT(nx.next_oid == plan->next_oid);
	EXPECT(nx.next_xid == 2);
	EXPECT(nx.omap_oid == apfs_run_block(plan, plan->comap_run));
	EXPECT(nx.spaceman_oid == APFS_WRITE_OID_SPACEMAN);
	EXPECT(nx.reaper_oid == APFS_WRITE_OID_REAPER);
	EXPECT(bcmp(sink->bytes, image_block(sink, nx.desc_base + 1),
	    BLOCK_BYTES) == 0);
	check_object(sink, 0, 1);
	check_object(sink, nx.desc_base, 1);
	check_object(sink, nx.desc_base + 1, 1);
	expect_zero(sink, APFS_NX_BYTES, BLOCK_BYTES - APFS_NX_BYTES);
	expect_zero(sink, (nx.desc_base + 1) * BLOCK_BYTES + APFS_NX_BYTES,
	    BLOCK_BYTES - APFS_NX_BYTES);
	bytes = image_block(sink, nx.desc_base);
	CHECK(apfs_cpm_block_decode(bytes, BLOCK_BYTES, &mappings));
	EXPECT(mappings.count == APFS_EPHEMERAL_COUNT);
	expect_zero(sink, nx.desc_base * BLOCK_BYTES + APFS_CPM_HEADER_BYTES +
	    mappings.count * APFS_CPM_ENTRY_BYTES, BLOCK_BYTES -
	    APFS_CPM_HEADER_BYTES - mappings.count * APFS_CPM_ENTRY_BYTES);
	for (uint32_t i = 0; i < mappings.count; i++) {
		CHECK(i < APFS_EPHEMERAL_COUNT);
		CHECK(apfs_cpm_decode(bytes, BLOCK_BYTES, &mappings, i, &mapping));
		EXPECT(mapping.oid == plan->ephemeral[i].oid);
		EXPECT(mapping.address == nx.data_base +
		    plan->ephemeral[i].first_slot);
		EXPECT(mapping.kind == plan->ephemeral[i].kind);
		EXPECT(mapping.size == plan->ephemeral[i].bytes);
		EXPECT(mapping.volume_oid == 0);
		EXPECT(mapping.flags == APFS_OBJ_EPHEMERAL);
		check_object(sink, mapping.address, mapping.size / BLOCK_BYTES);
		if (mapping.oid == nx.reaper_oid)
			expect_zero(sink, mapping.address * BLOCK_BYTES +
			    APFS_REAPER_BYTES, mapping.size - APFS_REAPER_BYTES);
	}
	CHECK(apfs_spaceman_decode(image_block(sink, nx.data_base),
	    (size_t)(plan->sm_blocks * BLOCK_BYTES), &sm));
	EXPECT(sm.devices[0].free_count == plan->block_count -
	    plan->allocated_blocks);
	/* The four arrays are packed in this order, with alignment gaps. */
	used = APFS_SM_BYTES;
	CHECK(sm.ip_bm_xid_offset >= used);
	expect_zero(sink, nx.data_base * BLOCK_BYTES + used,
	    sm.ip_bm_xid_offset - used);
	used = sm.ip_bm_xid_offset + 8ull * sm.ip_bm_size_in_blocks;
	CHECK(sm.ip_bitmap_offset >= used);
	expect_zero(sink, nx.data_base * BLOCK_BYTES + used,
	    sm.ip_bitmap_offset - used);
	used = sm.ip_bitmap_offset + 2ull * sm.ip_bm_size_in_blocks;
	CHECK(sm.ip_bm_free_next_offset >= used);
	expect_zero(sink, nx.data_base * BLOCK_BYTES + used,
	    sm.ip_bm_free_next_offset - used);
	used = sm.ip_bm_free_next_offset + 2ull * sm.ip_bm_block_count;
	CHECK(sm.devices[0].addr_offset >= used);
	expect_zero(sink, nx.data_base * BLOCK_BYTES + used,
	    sm.devices[0].addr_offset - used);
	used = sm.devices[0].addr_offset + 8ull * (sm.devices[0].cab_count != 0 ?
	    sm.devices[0].cab_count : sm.devices[0].cib_count);
	CHECK(used <= plan->sm_blocks * BLOCK_BYTES);
	expect_zero(sink, nx.data_base * BLOCK_BYTES + used,
	    plan->sm_blocks * BLOCK_BYTES - used);
	/* These fixtures fit one address array; no CAB indirection is needed. */
	CHECK(plan->pool_cabs == 0);
	for (uint64_t k = 0; k < plan->pool_cibs; k++) {
		block = sm.ip_base + k;
		bytes = image_block(sink, block);
		check_object(sink, block, 1);
		CHECK(apfs_cib_decode(bytes, BLOCK_BYTES, &cib));
		for (uint32_t i = 0; i < cib.count; i++) {
			CHECK(apfs_chunk_info_decode(bytes, BLOCK_BYTES, &cib, i,
			    &chunk));
			free_count += chunk.free_count;
			used = chunk.block_count - chunk.free_count;
			if (used != 0) {
				EXPECT(chunk.bitmap_address != 0);
				check_bitmap(sink, chunk.bitmap_address,
				    chunk.address, plan->allocated_blocks);
			} else
				EXPECT(chunk.bitmap_address == 0);
		}
		expect_zero(sink, block * BLOCK_BYTES + APFS_CIB_HEADER_BYTES +
		    cib.count * APFS_CHUNK_INFO_BYTES, BLOCK_BYTES -
		    APFS_CIB_HEADER_BYTES - cib.count * APFS_CHUNK_INFO_BYTES);
	}
	EXPECT(free_count == sm.devices[0].free_count);
	used = plan->pool_cibs + plan->pool_cabs + plan->pool_bitmaps;
	for (uint64_t k = 0; k < sm.ip_bm_size_in_blocks; k++)
		check_bitmap(sink, sm.ip_bm_base + k, k * BLOCK_BYTES * 8, used);
	expect_zero(sink, (sm.ip_base + used) * BLOCK_BYTES,
	    (sm.ip_block_count - used) * BLOCK_BYTES);
	expect_zero(sink, (sm.ip_bm_base + sm.ip_bm_size_in_blocks) * BLOCK_BYTES,
	    (sm.ip_bm_block_count - sm.ip_bm_size_in_blocks) * BLOCK_BYTES);
	check_tree_bytes(plan, sink, &plan->fs_tree, "filesystem tree");
	check_tree_bytes(plan, sink, &plan->vomap_tree, "volume object map");
	check_tree_bytes(plan, sink, &plan->comap_tree, "container object map");
	check_tree_bytes(plan, sink, &plan->extentref_tree, "extent references");
	check_tree_bytes(plan, sink, &plan->snapshot_tree, "snapshots");
	check_object(sink, apfs_run_block(plan, plan->volume_run), 1);
	check_object(sink, apfs_run_block(plan, plan->vomap_run), 1);
	check_object(sink, apfs_run_block(plan, plan->comap_run), 1);
	expect_zero(sink, apfs_run_block(plan, plan->volume_run) * BLOCK_BYTES +
	    APFS_FS_BYTES, BLOCK_BYTES - APFS_FS_BYTES);
	expect_zero(sink, apfs_run_block(plan, plan->vomap_run) * BLOCK_BYTES +
	    APFS_OMAP_BYTES, BLOCK_BYTES - APFS_OMAP_BYTES);
	expect_zero(sink, apfs_run_block(plan, plan->comap_run) * BLOCK_BYTES +
	    APFS_OMAP_BYTES, BLOCK_BYTES - APFS_OMAP_BYTES);
	check_volume_node_mappings(plan, sink);
	expect_zero(sink, (nx.desc_base + 2) * BLOCK_BYTES,
	    (nx.desc_blocks - 2) * BLOCK_BYTES);
	expect_zero(sink, (nx.data_base + plan->sm_blocks + 1) * BLOCK_BYTES,
	    (nx.data_blocks - plan->sm_blocks - 1) * BLOCK_BYTES);
	expect_zero(sink, plan->allocated_blocks * BLOCK_BYTES,
	    (plan->block_count - plan->allocated_blocks) * BLOCK_BYTES);
}

/* Resolve an OID using only physical links in the emitted object map. */
static uint64_t
image_omap_lookup(const memory_sink_t *sink, uint64_t block, uint64_t oid,
    unsigned depth)
{
	apfs_kv_size_t sizes = { APFS_OMAP_KEY_BYTES, APFS_OMAP_VAL_BYTES };
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_entry_t entry;
	apfs_node_t node;
	const unsigned char *bytes = image_block(sink, block);
	uint64_t found = 0, child;

	CHECK(depth < APFS_BTREE_LEVELS_MAX);
	CHECK(apfs_node_decode(bytes, BLOCK_BYTES, &node));
	if (node.level != 0)
		sizes.val = APFS_CHILD_OID_BYTES;
	for (uint32_t i = 0; i < node.key_count; i++) {
		CHECK(apfs_entry_decode(bytes, BLOCK_BYTES, &node, i,
		    &sizes, &entry));
		if (node.level != 0) {
			child = image_omap_lookup(sink,
			    fsimg_load_le64(bytes + entry.val_off), oid, depth + 1);
			if (child != 0) {
				EXPECT(found == 0);
				found = child;
			}
			continue;
		}
		CHECK(apfs_omap_key_decode(bytes + entry.key_off,
		    entry.key_len, &key));
		if (key.oid != oid)
			continue;
		CHECK(apfs_omap_val_decode(bytes + entry.val_off,
		    entry.val_len, &value));
		EXPECT(found == 0);
		EXPECT(key.xid == 1 && value.flags == 0);
		EXPECT(value.size == BLOCK_BYTES);
		found = value.address;
	}
	return (found);
}

typedef struct extent_check {
	fixture_t		*fixture;
	const memory_sink_t	*sink;
	uint64_t		*covered;
	unsigned		*references;
} extent_check_t;

/* Filesystem leaf order must tile each stream, including its zero padding.
 * Content is read at the physical addresses decoded from the leaf values.
 */
static void
check_image_extent(extent_check_t *check, const unsigned char *key,
    size_t key_length, const unsigned char *value, size_t value_length)
{
	fixture_t *fixture = check->fixture;
	const fsimg_catalog_entry_t *catalog;
	apfs_extent_key_t extent_key;
	apfs_extent_val_t extent;
	apfs_pext_val_t reference;
	apfs_jkey_t header;
	apfs_row_t row;
	const unsigned char *bytes;
	uint64_t id, ino, capacity, source_length;
	const char *saved_scenario = scenario;

	CHECK(apfs_jkey_decode(key, key_length, &header));
	if (header.kind != APFS_J_FILE_EXTENT &&
	    header.kind != APFS_J_PHYS_EXTENT)
		return;
	ino = header.oid;
	if (header.kind == APFS_J_PHYS_EXTENT) {
		CHECK(apfs_pext_val_decode(value, value_length, &reference));
		ino = reference.owner;
	}
	CHECK(ino > APFS_WRITE_INO_BASE);
	id = ino - APFS_WRITE_INO_BASE;
	CHECK(id <= fixture->writer->count);
	CHECK(apfs_row_get(fixture->plan, id, &row, NULL) == FSIMG_OK);
	CHECK(fsimg_catalog_get(fixture->writer, id, &catalog, NULL) == FSIMG_OK);
	scenario = catalog->entry.path;
	CHECK(row.kind == FSIMG_ENTRY_FILE);
	capacity = apfs_row_blocks(&row) * BLOCK_BYTES;
	if (header.kind == APFS_J_PHYS_EXTENT) {
		EXPECT(key_length == APFS_JKEY_BYTES);
		EXPECT(row.length != 0);
		EXPECT(header.oid == row.first_block);
		EXPECT(reference.length == apfs_row_blocks(&row));
		EXPECT(reference.owner == apfs_write_ino(id));
		EXPECT(reference.refcnt == 1 && reference.kind == 1);
		check->references[id]++;
		scenario = saved_scenario;
		return;
	}
	CHECK(apfs_extent_key_decode(key, key_length, &extent_key));
	CHECK(apfs_extent_val_decode(value, value_length, &extent));
	EXPECT(extent_key.logical == check->covered[id]);
	EXPECT(extent_key.logical % BLOCK_BYTES == 0);
	CHECK(extent_key.logical <= capacity);
	CHECK(extent.length > 0 && extent.length % BLOCK_BYTES == 0);
	CHECK(extent.length <= capacity - extent_key.logical);
	EXPECT(extent.phys_block == row.first_block +
	    extent_key.logical / BLOCK_BYTES);
	CHECK(extent.phys_block < check->sink->length / BLOCK_BYTES);
	CHECK(extent.length <= check->sink->length -
	    extent.phys_block * BLOCK_BYTES);
	bytes = image_block(check->sink, extent.phys_block);
	source_length = extent_key.logical < row.length ?
	    row.length - extent_key.logical : 0;
	if (source_length > extent.length)
		source_length = extent.length;
	for (uint64_t i = 0; i < source_length; i++) {
		unsigned char expected = catalog->storage == FSIMG_STORAGE_ZERO ?
		    0 : content_byte(extent_key.logical + i);

		if (bytes[i] == expected)
			continue;
		fprintf(stderr, "%s: content byte %llu: got %u, expected %u\n",
		    scenario, (unsigned long long)(extent_key.logical + i),
		    bytes[i], expected);
		failures++;
		break;
	}
	expect_zero(check->sink, extent.phys_block * BLOCK_BYTES + source_length,
	    extent.length - source_length);
	check->covered[id] += extent.length;
	scenario = saved_scenario;
}

/* A nonzero object-map root resolves virtual filesystem node OIDs;
 * extent-reference nodes instead carry physical child addresses directly.
 */
static void
check_image_extent_tree(extent_check_t *check, uint64_t block,
    uint64_t omap_root, unsigned depth)
{
	apfs_entry_t entry;
	apfs_node_t node;
	const unsigned char *bytes = image_block(check->sink, block);
	uint64_t child;

	CHECK(depth < APFS_BTREE_LEVELS_MAX);
	CHECK(apfs_node_decode(bytes, BLOCK_BYTES, &node));
	for (uint32_t i = 0; i < node.key_count; i++) {
		CHECK(apfs_entry_decode(bytes, BLOCK_BYTES, &node, i, NULL, &entry));
		if (node.level == 0) {
			check_image_extent(check, bytes + entry.key_off, entry.key_len,
			    bytes + entry.val_off, entry.val_len);
			continue;
		}
		CHECK(entry.val_len == APFS_CHILD_OID_BYTES);
		child = fsimg_load_le64(bytes + entry.val_off);
		if (omap_root != 0)
			child = image_omap_lookup(check->sink, omap_root, child, 0);
		CHECK(child != 0);
		check_image_extent_tree(check, child, omap_root, depth + 1);
	}
}

static void
check_file_placement(fixture_t *fixture, const memory_sink_t *sink)
{
	extent_check_t check = { .fixture = fixture, .sink = sink };
	apfs_omap_t omap;
	apfs_nx_t nx;
	apfs_fs_t volume;
	apfs_row_t row;
	uint64_t block, omap_root;

	/* Bootstrap through the container map, then the volume's own map. */
	CHECK(apfs_nx_decode(sink->bytes, BLOCK_BYTES, &nx));
	CHECK(apfs_omap_decode(image_block(sink, nx.omap_oid), BLOCK_BYTES, &omap));
	block = image_omap_lookup(sink, omap.tree_oid, nx.volume_oids[0], 0);
	CHECK(block != 0);
	CHECK(apfs_fs_decode(image_block(sink, block), BLOCK_BYTES, &volume));
	CHECK(apfs_omap_decode(image_block(sink, volume.omap_oid),
	    BLOCK_BYTES, &omap));
	omap_root = omap.tree_oid;
	block = image_omap_lookup(sink, omap_root, volume.root_tree_oid, 0);
	CHECK(block != 0);
	check.covered = calloc((size_t)fixture->writer->count + 1,
	    sizeof(*check.covered));
	check.references = calloc((size_t)fixture->writer->count + 1,
	    sizeof(*check.references));
	CHECK(check.covered != NULL && check.references != NULL);
	check_image_extent_tree(&check, block, omap_root, 0);
	check_image_extent_tree(&check, volume.extentref_tree_oid, 0, 0);
	for (uint64_t id = 1; id <= fixture->writer->count; id++) {
		CHECK(apfs_row_get(fixture->plan, id, &row, NULL) == FSIMG_OK);
		if (row.kind != FSIMG_ENTRY_FILE)
			continue;
		EXPECT(check.covered[id] == apfs_row_blocks(&row) * BLOCK_BYTES);
		EXPECT(check.references[id] == (row.length != 0 ? 1u : 0u));
		if (row.length == 0)
			EXPECT(row.first_block == 0);
	}
	free(check.references);
	free(check.covered);
}

static void
check_time(fsimg_timestamp_t actual, fsimg_timestamp_t expected)
{
	EXPECT(actual.seconds == expected.seconds);
	EXPECT(actual.nanoseconds == expected.nanoseconds);
	EXPECT(actual.basis == FSIMG_TIME_UTC);
}

static void
check_reader_catalog(fixture_t *fixture, memory_sink_t *sink,
    const emit_catalog_t *catalog)
{
	const fsimg_entry_t *entry, *expected;
	fsimg_volume_info_t info;
	fsimg_reader_t *reader = NULL;
	fsimg_io_t *io;
	fsimg_status_t status;
	unsigned char target[1024];
	bool *seen;
	size_t index, count = 0, length;
	uint64_t detail_counts[4];
	static const char *detail_names[] = { "files", "directories",
	    "symlinks", "other_objects" };
	const char *saved_scenario = scenario;

	CHECK(memory_io(fixture, sink, &io) == FSIMG_OK);
	status = fsimg_reader_open(fixture->ctx, io, NULL, &reader, &fixture->error);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s: reader open returned %d: %s\n", scenario,
		    status, fixture->error.message);
	EXPECT(status == FSIMG_OK);
	if (status != FSIMG_OK) {
		fsimg_io_free(io);
		return;
	}
	CHECK(fsimg_reader_get_info(reader, &info, NULL) == FSIMG_OK);
	EXPECT(strcmp(info.label, fixture->writer->options.label) == 0);
	EXPECT(bcmp(info.uuid, fixture->plan->volume_uuid, 16) == 0);
	EXPECT(info.allocation_unit_bytes == BLOCK_BYTES);
	EXPECT(info.sector_bytes == BLOCK_BYTES);
	EXPECT(info.allocation_units == fixture->plan->block_count);
	EXPECT(info.volume_bytes == sink->length);
	EXPECT(info.free_bytes == fixture->plan->common.info.free_bytes);
	detail_counts[0] = fixture->plan->files;
	detail_counts[1] = fixture->plan->directories;
	detail_counts[2] = fixture->plan->symlinks;
	detail_counts[3] = fixture->plan->others;
	for (size_t i = 0; i < 4; i++) {
		for (index = 0; index < info.detail_count; index++)
			if (strcmp(info.details[index].name, detail_names[i]) == 0)
				break;
		CHECK(index < info.detail_count);
		EXPECT(info.details[index].value == detail_counts[i]);
	}
	for (index = 0; index < info.detail_count; index++) {
		if (strcmp(info.details[index].name, "case_insensitive") == 0)
			EXPECT(info.details[index].value ==
			    !fixture->plan->case_sensitive);
		if (strcmp(info.details[index].name, "norm_insensitive") == 0)
			EXPECT(info.details[index].value ==
			    fixture->plan->case_sensitive);
	}
	seen = calloc(catalog->count + 1, sizeof(*seen));
	CHECK(seen != NULL);
	while ((status = fsimg_reader_next(reader, &entry, &fixture->error)) ==
	    FSIMG_OK) {
		for (index = 0; index < catalog->count; index++)
			if (strcmp(entry->path, catalog->entries[index].path) == 0)
				break;
		if (index == catalog->count) {
			fprintf(stderr, "%s: unexpected reader path %s\n", scenario,
			    entry->path);
			failures++;
			continue;
		}
		scenario = entry->path;
		expected = &catalog->entries[index].entry;
		EXPECT(!seen[index]);
		seen[index] = true;
		count++;
		EXPECT(entry->kind == expected->kind);
		EXPECT(entry->size == expected->size);
		EXPECT((entry->metadata.valid & EMIT_META) == EMIT_META);
		EXPECT(entry->metadata.mode == expected->metadata.mode);
		EXPECT(entry->metadata.uid == expected->metadata.uid);
		EXPECT(entry->metadata.gid == expected->metadata.gid);
		check_time(entry->metadata.created, expected->metadata.created);
		check_time(entry->metadata.modified, expected->metadata.modified);
		check_time(entry->metadata.accessed, expected->metadata.accessed);
		check_time(entry->metadata.changed, expected->metadata.changed);
		if (entry->kind == FSIMG_ENTRY_SYMLINK) {
			status = fsimg_reader_read_at(reader, 0, target,
			    sizeof(target), &length, &fixture->error);
			EXPECT(status == FSIMG_OK);
			EXPECT(length == expected->size);
			if (length == expected->size)
				EXPECT(bcmp(target, catalog->entries[index].target,
				    length) == 0);
		}
		scenario = saved_scenario;
	}
	if (status != FSIMG_DONE)
		fprintf(stderr, "%s: reader next returned %d: %s (%s)\n",
		    scenario, status, fixture->error.message, fixture->error.path);
	EXPECT(status == FSIMG_DONE);
	EXPECT(count == catalog->count);
	for (index = 0; index < catalog->count; index++)
		EXPECT(seen[index]);
	free(seen);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
}

static void
check_mixed_tree_emission(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;

	catalog_init(&catalog);
	catalog_mixed(&catalog);
	for (unsigned sensitive = 0; sensitive < 2; sensitive++) {
		base_options(&options);
		options.label = "Emission test";
		options.case_sensitive = sensitive != 0;
		prepare_catalog(&fixture, &catalog, &options, NULL, false);
		plan_ok(&fixture);
		if (emit_ok(&fixture, &sink)) {
			check_reader_catalog(&fixture, &sink, &catalog);
			check_file_placement(&fixture, &sink);
			check_container_bytes(fixture.plan, &sink);
		}
		free(sink.bytes);
		close_writer(&fixture);
	}
	free(catalog.entries);
}

static void
test_emitted_mixed_tree_round_trips_both_case_personalities(void)
{
	check_mixed_tree_emission();
}

static void
test_poisoned_emission_initializes_unused_metadata_and_payload_bytes(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_io_t *io;
	fsimg_status_t status;
	char directory[] =
	    "/Volumes/Optaine/fsimg-apfs/s8e/apfs-poison-XXXXXX";
	unsigned char buffer[8192];
	int root, fd;

	catalog_init(&catalog);
	catalog_mixed(&catalog);
	CHECK(mkdtemp(directory) != NULL);
	root = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	for (size_t i = 0; i < catalog.count; i++) {
		const emit_entry_t *spec = &catalog.entries[i];
		const char *path = spec->path + 1;

		switch (spec->entry.kind) {
		case FSIMG_ENTRY_DIRECTORY:
			CHECK(mkdirat(root, path, 0700) == 0);
			break;
		case FSIMG_ENTRY_FILE:
			fd = openat(root, path, O_WRONLY | O_CREAT | O_EXCL, 0600);
			CHECK(fd != -1);
			for (uint64_t offset = 0; offset < spec->entry.size;) {
				size_t length = sizeof(buffer);

				if (length > spec->entry.size - offset)
					length = (size_t)(spec->entry.size - offset);
				for (size_t j = 0; j < length; j++)
					buffer[j] = content_byte(offset + j);
				CHECK(write(fd, buffer, length) == (ssize_t)length);
				offset += length;
			}
			CHECK(close(fd) == 0);
			break;
		case FSIMG_ENTRY_SYMLINK:
			CHECK(symlinkat(spec->target, root, path) == 0);
			break;
		case FSIMG_ENTRY_FIFO:
			/* Host-tree collection does not support FIFOs. */
			break;
		default:
			CHECK(false);
		}
	}
	for (unsigned sensitive = 0; sensitive < 2; sensitive++) {
		base_options(&options);
		options.flags |= FSIMG_WRITE_ALLOW_LOSS;
		options.case_sensitive = sensitive != 0;
		open_writer(&fixture, &options);
		CHECK(fsimg_writer_add_tree(fixture.writer, root, "/",
		    &fixture.error) == FSIMG_OK);
		plan_ok(&fixture);
		if (emit_ok(&fixture, &sink)) {
			check_file_placement(&fixture, &sink);
			check_container_bytes(fixture.plan, &sink);
		}
		free(sink.bytes);
		close_writer(&fixture);

		/* Host descriptor I/O objects also require zeroed allocations.
		 * Poison only a fixture with zero sources, which allocate no source
		 * I/O objects during emission. The host fixture above covers real
		 * payloads and both inline symlink lengths without poisoning.
		 * An explicit zero callback avoids poisoning the common I/O layer's
		 * zero buffer, whose contents rely on the allocator's zeroing contract.
		 */
		open_writer(&fixture, &options);
		for (size_t i = 0; i < catalog.count; i++) {
			const emit_entry_t *spec = &catalog.entries[i];

			if (spec->entry.kind != FSIMG_ENTRY_SYMLINK)
				add_entry(&fixture, spec->path, spec->entry.kind,
				    spec->entry.size);
		}
		plan_ok(&fixture);
		sink_open(&fixture, &sink);
		sink.zero_callback = true;
		CHECK(memory_io(&fixture, &sink, &io) == FSIMG_OK);
		fsimg_test_poison_allocations(fixture.ctx, true);
		status = fsimg_writer_emit(fixture.writer, io, &fixture.error);
		fsimg_test_poison_allocations(fixture.ctx, false);
		fsimg_io_free(io);
		if (status != FSIMG_OK)
			fprintf(stderr, "%s: emit returned %d: %s (%s)\n",
			    scenario, status, fixture.error.message, fixture.error.path);
		EXPECT(status == FSIMG_OK);
		if (status == FSIMG_OK) {
			check_file_placement(&fixture, &sink);
			check_container_bytes(fixture.plan, &sink);
		}
		free(sink.bytes);
		close_writer(&fixture);
	}
	/* Children precede their parents when removing the catalog in reverse. */
	for (size_t i = catalog.count; i > 0; i--) {
		const emit_entry_t *spec = &catalog.entries[i - 1];

		if (spec->entry.kind == FSIMG_ENTRY_FIFO)
			continue;
		CHECK(unlinkat(root, spec->path + 1,
		    spec->entry.kind == FSIMG_ENTRY_DIRECTORY ? AT_REMOVEDIR : 0) == 0);
	}
	CHECK(close(root) == 0);
	CHECK(rmdir(directory) == 0);
	free(catalog.entries);
}

static void
test_emitted_metadata_preserves_distinct_nanosecond_times(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_metadata_t *metadata;

	catalog_init(&catalog);
	catalog_add(&catalog, "/metadata", FSIMG_ENTRY_FILE, 1);
	metadata = &catalog.entries[0].entry.metadata;
	metadata->created.seconds = EPOCH - 400;
	metadata->created.nanoseconds = 123456789;
	metadata->modified.seconds = EPOCH - 300;
	metadata->modified.nanoseconds = 234567891;
	metadata->accessed.seconds = EPOCH - 200;
	metadata->accessed.nanoseconds = 345678912;
	metadata->changed.seconds = EPOCH - 100;
	metadata->changed.nanoseconds = 456789123;
	base_options(&options);
	options.flags = 0;
	memset(options.volume_uuid, 0x57, sizeof(options.volume_uuid));
	plan_catalog(&fixture, &catalog, &options, NULL, false);
	if (emit_ok(&fixture, &sink))
		check_reader_catalog(&fixture, &sink, &catalog);
	free(sink.bytes);
	close_writer(&fixture);
	free(catalog.entries);
}

static void
test_emitted_empty_many_wide_and_multichunk_shapes(void)
{
	static const char *names[] = { "empty", "many", "wide", "multichunk" };
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	const char *saved_scenario = scenario;

	for (size_t shape = 0; shape < 4; shape++) {
		scenario = names[shape];
		catalog_init(&catalog);
		base_options(&options);
		if (shape == 1 || shape == 2)
			catalog_many(&catalog, shape == 2);
		if (shape == 3) {
			options.volume_bytes = 256 * 1024 * 1024;
			catalog_add(&catalog, "/cross-chunk", FSIMG_ENTRY_FILE,
			    129 * 1024 * 1024 + 17);
		}
		plan_catalog(&fixture, &catalog, &options, NULL, false);
		if (shape == 2)
			EXPECT(fixture.plan->fs_tree.shape.levels >= 2);
		if (shape == 3)
			EXPECT(fixture.plan->pool_bitmaps > 1);
		if (emit_ok(&fixture, &sink)) {
			check_container_bytes(fixture.plan, &sink);
			check_file_placement(&fixture, &sink);
			check_reader_catalog(&fixture, &sink, &catalog);
		}
		free(sink.bytes);
		close_writer(&fixture);
		free(catalog.entries);
	}
	scenario = saved_scenario;
}

static void
set_time_zone(const char *zone)
{
	if (zone == NULL)
		CHECK(unsetenv("TZ") == 0);
	else
		CHECK(setenv("TZ", zone, 1) == 0);
	tzset();
}

static void
test_emission_is_identical_across_order_budget_scratch_and_timezone(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fsimg_options_t context;
	fixture_t fixture;
	memory_sink_t reference = { 0 }, sink;
	char directory[4096], other_directory[4096];
	char path[32];
	const char *temporary = getenv("TMPDIR");
	char *saved_zone = getenv("TZ") == NULL ? NULL : strdup(getenv("TZ"));
	int fd, other_fd;

	catalog_init(&catalog);
	/* Inline 1023-byte attributes exceed the minimum budget's 2 MiB
	 * sort run, requiring more than one run and a merge.
	 */
	for (unsigned i = 0; i < 3000; i++) {
		snprintf(path, sizeof(path), "/link-%04u", i);
		catalog_add(&catalog, path, FSIMG_ENTRY_SYMLINK, 1023);
	}
	base_options(&options);
	if (temporary == NULL)
		temporary = "/tmp";
	CHECK(snprintf(directory, sizeof(directory), "%s/identity-XXXXXX",
	    temporary) < (int)sizeof(directory));
	CHECK(snprintf(other_directory, sizeof(other_directory),
	    "%s/identity-XXXXXX", temporary) < (int)sizeof(other_directory));
	CHECK(mkdtemp(directory) != NULL);
	fd = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(fd != -1);
	CHECK(mkdtemp(other_directory) != NULL);
	other_fd = open(other_directory, O_RDONLY | O_DIRECTORY);
	CHECK(other_fd != -1);
	for (unsigned variant = 0; variant < 5; variant++) {
		fsimg_options_init(&context);
		if (variant >= 2) {
			context.memory_bytes = 16 * 1024 * 1024;
			context.scratch_directory_fd = variant == 2 ? fd : other_fd;
		}
		set_time_zone(variant == 4 ? "PST8PDT" : "UTC0");
		plan_catalog(&fixture, &catalog, &options, &context, variant != 0);
		if (variant >= 2)
			EXPECT(fixture.plan->records->end >
			    fsimg_sort_run_bytes(context.memory_bytes));
		if (emit_ok(&fixture, &sink)) {
			if (variant == 0)
				reference = sink;
			else if (reference.bytes != NULL) {
				EXPECT(sink.length == reference.length);
				if (sink.length == reference.length)
					EXPECT(bcmp(sink.bytes, reference.bytes,
					    (size_t)sink.length) == 0);
			}
		}
		if (variant != 0 || reference.bytes == NULL)
			free(sink.bytes);
		close_writer(&fixture);
	}
	set_time_zone(saved_zone);
	free(saved_zone);
	free(reference.bytes);
	CHECK(close(fd) == 0);
	CHECK(close(other_fd) == 0);
	CHECK(rmdir(directory) == 0);
	CHECK(rmdir(other_directory) == 0);
	free(catalog.entries);
}

static void
test_emission_write_and_flush_failures_release_resources(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	apfs_row_t row;
	fsimg_status_t status;
	uint64_t writes = 0, zeroing_writes = 0, payload_offset, payload_end;

	catalog_init(&catalog);
	catalog_add(&catalog, "/file", FSIMG_ENTRY_FILE, 4097);
	base_options(&options);
	plan_catalog(&fixture, &catalog, &options, NULL, false);
	CHECK(apfs_row_get(fixture.plan, 1, &row, NULL) == FSIMG_OK);
	payload_offset = row.first_block * BLOCK_BYTES;
	payload_end = payload_offset + apfs_row_blocks(&row) * BLOCK_BYTES;
	if (emit_ok(&fixture, &sink)) {
		writes = sink.writes;
		zeroing_writes = sink.zeroing_writes;
		EXPECT(sink.initialized_bytes == sink.length);
		EXPECT(sink.first_driver_offset == payload_offset);
		EXPECT(sink.last_write_offset == 0);
	}
	free(sink.bytes);
	close_writer(&fixture);
	CHECK(zeroing_writes != 0 && writes > zeroing_writes + 2);
	/* Count initialization separately so every write fault reaches APFS. */
	for (unsigned fault = 0; fault < 4; fault++) {
		plan_catalog(&fixture, &catalog, &options, NULL, false);
		sink_open(&fixture, &sink);
		sink.fail_write_at = fault == 0 ? zeroing_writes + 1 :
		    fault == 1 ? zeroing_writes + (writes - zeroing_writes) / 2 :
		    fault == 2 ? writes : 0;
		sink.fail_flush = fault == 3;
		status = emit_into(&fixture, &sink);
		EXPECT(status != FSIMG_OK);
		EXPECT(status == FSIMG_EIO);
		EXPECT(sink.zeroing_writes == zeroing_writes);
		if (fault < 3)
			EXPECT(sink.writes == sink.fail_write_at);
		if (fault == 0)
			EXPECT(sink.last_write_offset == payload_offset);
		if (fault == 1) {
			EXPECT(sink.last_write_offset != 0);
			EXPECT(sink.last_write_offset < payload_offset ||
			    sink.last_write_offset >= payload_end);
		}
		if (fault == 2)
			EXPECT(sink.last_write_offset == 0);
		free(sink.bytes);
		close_writer(&fixture);
	}
	free(catalog.entries);
}

static void
test_emission_allocation_failure_at_each_allocation_is_clean(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	bool succeeded = false;
	unsigned failed = 0;

	catalog_init(&catalog);
	catalog_add(&catalog, "/file", FSIMG_ENTRY_FILE, 4097);
	catalog_add(&catalog, "/link", FSIMG_ENTRY_SYMLINK, 1);
	base_options(&options);
	for (uint64_t attempt = 1; attempt < 4096 && !succeeded; attempt++) {
		plan_catalog(&fixture, &catalog, &options, NULL, false);
		sink_open(&fixture, &sink);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = emit_into(&fixture, &sink);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status != FSIMG_ENOMEM && status != FSIMG_OK)
			fprintf(stderr, "%s: allocation %llu returned %d: %s\n",
			    scenario, (unsigned long long)attempt, status,
			    fixture.error.message);
		EXPECT(status == FSIMG_ENOMEM || status == FSIMG_OK);
		succeeded = status == FSIMG_OK;
		failed += status != FSIMG_OK;
		free(sink.bytes);
		close_writer(&fixture);
	}
	EXPECT(succeeded && failed > 0);
	free(catalog.entries);
}

static void
test_same_length_changed_link_fails_emission_and_names_path(void)
{
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;

	catalog_init(&catalog);
	catalog_add(&catalog, "/changed-link", FSIMG_ENTRY_SYMLINK, 8);
	base_options(&options);
	plan_catalog(&fixture, &catalog, &options, NULL, false);
	catalog.changed_link = true;
	sink_open(&fixture, &sink);
	status = emit_into(&fixture, &sink);
	EXPECT(status == FSIMG_ESOURCE_CHANGED);
	EXPECT(strcmp(fixture.error.path, "/changed-link") == 0);
	free(sink.bytes);
	close_writer(&fixture);
	free(catalog.entries);
}
/* Hash the source description, never the image being qualified. */
static void
source_digest(const emit_entry_t *spec, char hex[65])
{
	fsimg_sha256_t sha;
	unsigned char digest[32], buffer[4096];
	uint64_t offset = 0;
	size_t length;

	fsimg_sha256_init(&sha);
	while (offset < spec->entry.size) {
		length = spec->entry.size - offset > sizeof(buffer) ?
		    sizeof(buffer) : (size_t)(spec->entry.size - offset);
		if (spec->entry.kind == FSIMG_ENTRY_SYMLINK)
			bcopy(spec->target + offset, buffer, length);
		else
			for (size_t i = 0; i < length; i++)
				buffer[i] = content_byte(offset + i);
		fsimg_sha256_update(&sha, buffer, length);
		offset += length;
	}
	fsimg_sha256_final(digest, &sha);
	for (size_t i = 0; i < sizeof(digest); i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
}

/* Fixture paths and targets contain only plain ASCII without JSON escapes. */
static void
write_native_manifest(const char *path, const emit_catalog_t *catalog)
{
	const emit_entry_t *spec;
	const char *kind;
	char digest[65];
	FILE *file;

	file = fopen(path, "wx");
	CHECK(file != NULL);
	CHECK(fputs("[\n", file) >= 0);
	for (size_t i = 0; i < catalog->count; i++) {
		spec = &catalog->entries[i];
		switch (spec->entry.kind) {
		case FSIMG_ENTRY_FILE: kind = "file"; break;
		case FSIMG_ENTRY_DIRECTORY: kind = "directory"; break;
		case FSIMG_ENTRY_SYMLINK: kind = "symlink"; break;
		case FSIMG_ENTRY_FIFO: kind = "fifo"; break;
		default: CHECK(false); kind = "unknown"; break;
		}
		CHECK(fprintf(file, "%s{\"path\":\"%s\",\"kind\":\"%s\","
		    "\"size\":%llu,\"mode\":%u,\"btime_ns\":%lld,\"mtime_ns\":%lld",
		    i == 0 ? "" : ",\n", spec->path + 1, kind,
		    (unsigned long long)spec->entry.size, spec->entry.metadata.mode,
		    (long long)(EPOCH * INT64_C(1000000000)),
		    (long long)(EPOCH * INT64_C(1000000000))) > 0);
		if (spec->entry.kind == FSIMG_ENTRY_SYMLINK)
			CHECK(fprintf(file, ",\"target\":\"%s\"", spec->target) > 0);
		if (spec->entry.kind == FSIMG_ENTRY_FILE ||
		    spec->entry.kind == FSIMG_ENTRY_SYMLINK) {
			source_digest(spec, digest);
			CHECK(fprintf(file, ",\"sha256\":\"%s\"", digest) > 0);
		}
		CHECK(fputs("}", file) >= 0);
	}
	CHECK(fputs("\n]\n", file) >= 0);
	CHECK(fclose(file) == 0);
}

/* Run explicitly as test-apfs-write --qualify. Ordinary tests never write
 * images. The directory must be supplied and its case directories absent.
 */
static int
qualify_images(void)
{
	static const char *names[] = { "mixed-ci", "mixed-cs", "many", "wide",
	    "large", "empty", "multichunk" };
	const char *root = getenv("FSIMG_APFS_QUALIFY_DIR");
	char directory[4096], path[4096];
	emit_catalog_t catalog;
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	FILE *file;

#ifndef __APPLE__
	return (77);
#endif
	if (root == NULL)
		return (77);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		scenario = names[i];
		CHECK(snprintf(directory, sizeof(directory), "%s/%s", root,
		    names[i]) < (int)sizeof(directory));
		CHECK(mkdir(directory, 0700) == 0);
		catalog_init(&catalog);
		if (i < 2)
			catalog_mixed(&catalog);
		if (i == 2 || i == 3)
			catalog_many(&catalog, i == 3);
		if (i == 4)
			catalog_add(&catalog, "/large", FSIMG_ENTRY_FILE,
			    5 * 1024 * 1024 + 137);
		if (i == 6)
			catalog_add(&catalog, "/cross-chunk", FSIMG_ENTRY_FILE,
			    129 * 1024 * 1024 + 17);
		base_options(&options);
		options.label = "Qualification";
		options.case_sensitive = i == 1;
		options.volume_bytes = (i == 6 ? 256 : 64) * UINT64_C(1024) * 1024;
		plan_catalog(&fixture, &catalog, &options, NULL, false);
		if (i == 3)
			EXPECT(fixture.plan->fs_tree.shape.levels >= 2);
		if (emit_ok(&fixture, &sink)) {
			CHECK(snprintf(path, sizeof(path), "%s/image.img", directory)
			    < (int)sizeof(path));
			file = fopen(path, "wx");
			CHECK(file != NULL);
			CHECK(fwrite(sink.bytes, 1, (size_t)sink.length, file) ==
			    sink.length);
			CHECK(fclose(file) == 0);
			CHECK(snprintf(path, sizeof(path), "%s/native-manifest.json",
			    directory) < (int)sizeof(path));
			write_native_manifest(path, &catalog);
		}
		free(sink.bytes);
		close_writer(&fixture);
		free(catalog.entries);
	}
	return (failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

#define RUN(test) do { \
	unsigned before = failures; \
	scenario = #test; \
	test(); \
	fprintf(stderr, "%s: %s\n", failures == before ? "PASS" : "FAIL", \
	    #test); \
} while (0)

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--qualify") == 0)
		return (qualify_images());
	CHECK(argc == 1);
	RUN(test_emitted_mixed_tree_round_trips_both_case_personalities);
	RUN(test_emitted_metadata_preserves_distinct_nanosecond_times);
	RUN(test_emitted_empty_many_wide_and_multichunk_shapes);
	RUN(test_emission_is_identical_across_order_budget_scratch_and_timezone);
	RUN(test_emission_write_and_flush_failures_release_resources);
	RUN(test_emission_allocation_failure_at_each_allocation_is_clean);
	RUN(test_same_length_changed_link_fails_emission_and_names_path);
	RUN(test_sector_and_allocation_units_are_4096_bytes);
	RUN(test_label_is_utf8_of_at_most_255_bytes);
	RUN(test_null_label_selects_untitled);
	RUN(test_empty_label_plans_with_untitled);
	RUN(test_container_size_has_block_alignment_and_bounds);
	RUN(test_one_byte_above_largest_container_is_range_error);
	RUN(test_zero_uuid_requires_reproducible_mode);
	RUN(test_common_settings_refuse_placement);
	RUN(test_unassigned_and_noncharacter_names_name_the_entry);
	RUN(test_case_collisions_follow_volume_personality);
	RUN(test_canonical_equivalents_collide_in_both_personalities);
	RUN(test_name_limit_counts_utf16_units);
	RUN(test_empty_tree_has_native_records_and_minimum_container);
	RUN(test_mixed_catalog_counts_records_children_and_contiguous_data);
	RUN(test_hashed_key_order_uses_owner_kind_hash_name_and_logical_offset);
	RUN(test_wide_directory_has_multiple_leaves_and_strictly_sorted_records);
	RUN(test_automatic_size_fits_and_minimize_uses_same_layout);
	RUN(test_exact_container_too_small_refuses_content);
	RUN(test_minimize_refuses_exact_volume_size);
	RUN(test_minimum_free_bytes_raises_automatic_size);
	RUN(test_source_order_does_not_change_plan);
	RUN(test_reproducible_time_equal_to_epoch_is_exact);
	RUN(test_reproducible_uuids_use_apfs_role_and_distinct_container);
	RUN(test_poisoned_emission_initializes_unused_metadata_and_payload_bytes);
	return (failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
