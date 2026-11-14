/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "format.h"
#include "internal.h"
#include "layer.h"
#include "meta.h"
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
	bzero(fixture, sizeof(*fixture));
	CHECK(fsimg_ctx_new(NULL, &fixture->ctx, &fixture->error) == FSIMG_OK);
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
	fsimg_writer_free(fixture->writer);
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

#define RUN(test) do { scenario = #test; test(); } while (0)

int
main(void)
{
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
	return (failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
