/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs-gen.h"
#include "fsimg.h"
#include "host.h"
#include "reader.h"
#include "test.h"

#define CONTENT_BLOCK 4096u
#define CONTENT_BLOCKS 8192u
#define CONTENT_BYTES ((size_t)CONTENT_BLOCK * CONTENT_BLOCKS)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* A record owns its bytes, so sorting descriptors never invalidates spans. */
typedef struct content_record {
	unsigned char key[1100];
	unsigned char value[1400];
	uint16_t key_size;
	uint16_t value_size;
} content_record_t;

typedef struct content_fixture {
	content_record_t *records;
	size_t count;
	size_t capacity;
	unsigned char *image;
	uint64_t available;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_error_t error;
} content_fixture_t;

static void expect(bool condition, const char *message);
static bool status_is(content_fixture_t *, fsimg_status_t, fsimg_status_t);
static uint64_t integer(const unsigned char *raw, unsigned width);
static unsigned char *block(content_fixture_t *f, uint64_t number);
static content_record_t *record_new(content_fixture_t *f);
static void fixture_init(content_fixture_t *, size_t);
static void fixture_free(content_fixture_t *f);
static size_t fields_build(unsigned char *raw, const apfs_gen_xfield_t *fields,
    uint16_t count);
static apfs_gen_inode_val_t inode_value(uint64_t parent, uint8_t type);
static void add_inode(content_fixture_t *f, uint64_t oid,
    const apfs_gen_inode_val_t *inode);
static void add_named_inode(content_fixture_t *f, uint64_t oid, uint64_t parent,
    uint8_t type, const char *name);
static void add_root(content_fixture_t *f);
static void add_drec(content_fixture_t *f, uint64_t parent, uint64_t child,
    const char *name, uint8_t type);
static void add_xattr(content_fixture_t *f, uint64_t oid, const char *name,
    const unsigned char *bytes, uint16_t size, uint16_t flags);
static int record_compare(const void *left, const void *right);
static void image_build(content_fixture_t *f);
static fsimg_status_t image_read(void *argument, uint64_t offset, void *bytes,
    size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED);
static fsimg_status_t image_size(void *argument, uint64_t *size,
    fsimg_error_t *error FSIMG_UNUSED);
static bool content_open(content_fixture_t *f);
static void add_file(content_fixture_t *f, uint64_t oid, uint64_t private_id,
    const char *name, uint64_t size, uint64_t allocated);
static void add_extent(content_fixture_t *f, uint64_t private_id, uint64_t logical,
    const apfs_gen_extent_val_t *value);
static bool open_file(content_fixture_t *f, const char *path, uint64_t size);
static void expect_read(content_fixture_t *f, uint64_t offset, size_t request,
    const unsigned char *expected, size_t prefix, fsimg_status_t wanted,
    const char *message);
static void test_holes_and_unrecorded_tail_read_as_zero(void);
static void test_extent_lengths_and_random_boundaries(void);
static void test_refused_extents_preserve_only_the_valid_prefix(void);
static void test_extent_reaching_file_size_requires_contiguous_successor(void);
static void test_first_extent_starts_at_zero(void);
static void test_allocated_bytes_allow_extents_past_rounded_size(void);
static void test_sparse_file_offsets_above_four_gib(void);
static void test_clones_use_their_own_private_extent_records(void);
static void test_truncated_mapped_extent_requires_salvage_to_keep_prefix(void);
static void test_compressed_content_is_refused_before_delivery(void);

static const char *scenario;
static unsigned failures, checks;

static void
expect(bool condition, const char *message)
{
	checks++;
	if (!condition) {
		fprintf(stderr, "%s: %s\n", scenario, message);
		failures++;
	}
}

static bool
status_is(content_fixture_t *f, fsimg_status_t actual, fsimg_status_t wanted)
{
	checks++;
	if (actual == wanted)
		return (true);
	fprintf(stderr, "%s: expected status %d, got %d (%s; %s)\n",
	    scenario, wanted, actual, f->error.message, f->error.path);
	failures++;
	return (false);
}

static uint64_t
integer(const unsigned char *raw, unsigned width)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < width; i++)
		value |= (uint64_t)raw[i] << (i * 8);
	return (value);
}

static unsigned char *
block(content_fixture_t *f, uint64_t number)
{
	CHECK(number < CONTENT_BLOCKS);
	return (f->image + number * CONTENT_BLOCK);
}

static content_record_t *
record_new(content_fixture_t *f)
{
	CHECK(f->count < f->capacity);
	return (&f->records[f->count++]);
}

static void
fixture_init(content_fixture_t *f, size_t capacity)
{
	bzero(f, sizeof(*f));
	f->capacity = capacity;
	f->available = CONTENT_BYTES;
	f->records = calloc(capacity, sizeof(*f->records));
	f->image = calloc(1, CONTENT_BYTES);
	CHECK(f->records != NULL && f->image != NULL);
}

static void
fixture_free(content_fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	if (f->ctx != NULL) {
		CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
		expect(stats.scratch_bytes == 0, "content scratch released");
		fsimg_ctx_free(f->ctx);
	}
	free(f->records);
	free(f->image);
}

static size_t
fields_build(unsigned char *raw, const apfs_gen_xfield_t *fields,
    uint16_t count)
{
	size_t size = 4 + 4 * count;

	for (uint16_t i = 0; i < count; i++)
		size += ((size_t)fields[i].size + 7) / 8 * 8;
	CHECK(size <= 1024);
	CHECK(apfs_gen_xfields_build(raw, size, fields, count));
	return (size);
}

static apfs_gen_inode_val_t
inode_value(uint64_t parent, uint8_t type)
{
	return ((apfs_gen_inode_val_t){ .parent = parent, .private_id = 16,
	    .create_time = INT64_C(-1000000001),
	    .mod_time = INT64_C(1234567890123456789),
	    .change_time = INT64_C(987654321012345678),
	    .access_time = INT64_C(-1), .count = 1, .owner = 12345,
	    .group = 54321, .mode = (uint16_t)((type << 12) | 06751) });
}

static void
add_inode(content_fixture_t *f, uint64_t oid,
    const apfs_gen_inode_val_t *inode)
{
	content_record_t *r = record_new(f);

	r->key_size = 8;
	r->value_size = (uint16_t)(92 + inode->xfields_len);
	CHECK(apfs_gen_jkey_build(r->key, r->key_size, oid, 3));
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, inode));
}

static void
add_named_inode(content_fixture_t *f, uint64_t oid, uint64_t parent,
    uint8_t type, const char *name)
{
	unsigned char raw[1024];
	apfs_gen_inode_val_t inode = inode_value(parent, type);
	apfs_gen_xfield_t field = { .type = 4, .flags = 2,
	    .data = (const unsigned char *)name,
	    .size = (uint16_t)(strlen(name) + 1) };

	inode.xfields_len = fields_build(raw, &field, 1);
	inode.xfields = raw;
	add_inode(f, oid, &inode);
}

static void
add_root(content_fixture_t *f)
{
	add_named_inode(f, 2, 1, 4, "root");
}

static void
add_drec(content_fixture_t *f, uint64_t parent, uint64_t child,
    const char *name, uint8_t type)
{
	content_record_t *r = record_new(f);
	apfs_gen_drec_key_t key = { .parent = parent, .name = name,
	    .name_len = (uint16_t)strlen(name) };
	apfs_gen_drec_val_t value = { .child = child, .flags = type };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = (uint16_t)(18 + value.xfields_len);
	CHECK(apfs_gen_drec_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_drec_val_build(r->value, r->value_size, &value));
}

static void
add_xattr(content_fixture_t *f, uint64_t oid, const char *name,
    const unsigned char *bytes, uint16_t size, uint16_t flags)
{
	content_record_t *r = record_new(f);
	apfs_gen_name_key_t key = { .oid = oid, .name = name,
	    .name_len = (uint16_t)strlen(name) };
	apfs_gen_xattr_val_t value = { .data = bytes,
	    .data_len = size, .flags = flags };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = (uint16_t)(4 + size);
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &value));
}

static int
record_compare(const void *left, const void *right)
{
	const content_record_t *a = left, *b = right;
	uint64_t ak = integer(a->key, 8), bk = integer(b->key, 8);
	uint64_t ao = ak & GEN_JKEY_OID_MASK, bo = bk & GEN_JKEY_OID_MASK;
	unsigned kind = (unsigned)(ak >> 60);

	if (ao != bo)
		return (ao < bo ? -1 : 1);
	if (kind != bk >> 60)
		return (kind < bk >> 60 ? -1 : 1);
	if (kind == 5 || kind == 8) {
		ak = integer(a->key + 8, 8);
		bk = integer(b->key + 8, 8);
		return (ak < bk ? -1 : ak > bk);
	}
	if (kind == 4 || kind == 9)
		return (strcmp((const char *)a->key + 10,
		    (const char *)b->key + 10));
	return (0);
}

static void
image_build(content_fixture_t *f)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = CONTENT_BLOCK, .oid = 100,
		    .address = 32 },
		{ .type = 0x80000011, .size = CONTENT_BLOCK, .oid = 102,
		    .address = 33 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
	    .mappings = mappings, .mapping_count = 2, .mapping_blocks = 1,
	    .data_len = 4, .data_next = 4, .desc_next = 2 };
	apfs_gen_container_t container = {
		.nx = { .magic = 0x4253584e, .block_size = CONTENT_BLOCK,
		.block_count = CONTENT_BLOCKS, .incompat_features = 2,
		.uuid = { 1 }, .desc_base = 1,
		.desc_blocks = 8, .data_base = 32, .data_blocks = 128,
		.max_volumes = 100, .volume_oids = { 2000 },
		.spaceman_oid = 100, .reaper_oid = 102, .omap_oid = 16 },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	apfs_gen_omap_record_t volume = { .key = { .oid = 2000, .xid = 10 },
	    .value = { .size = CONTENT_BLOCK, .address = 180 } };
	apfs_gen_tree_t map_tree = { .records = &volume, .count = 1,
	    .base = 200, .xid = 10, .block_size = CONTENT_BLOCK,
	    .records_per_leaf = 16, .children_per_index = 8 };
	apfs_gen_omap_t map = { .object = { .oid = 16, .xid = 10,
	    .type = 0x4000000b }, .flags = 1, .tree_type = 0x40000002 };
	apfs_gen_fs_t fs = { .object = { .oid = 2000, .xid = 10,
	    .type = 13 }, .magic = 0x42535041, .uuid = { 2 },
	    .name = "Content", .flags = 1, .root_tree_type = 2,
	    .extentref_tree_type = 0x40000002,
	    .snapshot_tree_type = 0x40000002, .omap_oid = 181 };
	apfs_gen_fs_tree_t tree = { .count = f->count, .first_oid = 3000,
	    .base = 1536, .xid = 10, .block_size = CONTENT_BLOCK,
	    .records_per_leaf = 8, .children_per_index = 8 };
	apfs_gen_record_t *records;
	apfs_gen_omap_record_t *nodes;
	uint64_t root, blocks, map_root, map_blocks;

	CHECK(apfs_gen_container_build(f->image, CONTENT_BYTES, &container));
	CHECK(apfs_gen_tree_build(f->image, CONTENT_BYTES, &map_tree,
	    &map.tree_oid, &blocks));
	CHECK(apfs_gen_omap_build(block(f, 16), CONTENT_BLOCK, &map));
	qsort(f->records, f->count, sizeof(*f->records),
	    record_compare);
	records = calloc(f->count, sizeof(*records));
	nodes = calloc(f->count + 1, sizeof(*nodes));
	CHECK(records != NULL && nodes != NULL);
	for (size_t i = 0; i < f->count; i++)
		records[i] = (apfs_gen_record_t){ .key = f->records[i].key,
		    .value = f->records[i].value,
		    .key_size = f->records[i].key_size,
		    .value_size = f->records[i].value_size };
	tree.records = records;
	CHECK(apfs_gen_fs_tree_build(f->image, CONTENT_BYTES, &tree, nodes,
	    f->count + 1, &root, &blocks));
	map_tree.records = nodes;
	map_tree.count = blocks;
	map_tree.base = 256;
	CHECK(apfs_gen_tree_build(f->image, CONTENT_BYTES, &map_tree,
	    &map_root, &map_blocks));
	CHECK(256 + map_blocks < 1536);
	map.object.oid = 181;
	map.flags = 0;
	map.tree_oid = map_root;
	CHECK(apfs_gen_omap_build(block(f, 181), CONTENT_BLOCK, &map));
	fs.root_tree_oid = root;
	CHECK(apfs_gen_fs_build(block(f, 180), CONTENT_BLOCK, &fs));
	free(nodes);
	free(records);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *bytes, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	content_fixture_t *f = argument;

	CHECK(offset <= CONTENT_BYTES);
	if (offset >= f->available)
		length = 0;
	else if (length > f->available - offset)
		length = (size_t)(f->available - offset);
	bcopy(f->image + offset, bytes, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument, uint64_t *size,
    fsimg_error_t *error FSIMG_UNUSED)
{
	content_fixture_t *f = argument;

	*size = f->available;
	return (FSIMG_OK);
}

static bool
content_open(content_fixture_t *f)
{
	fsimg_io_ops_t ops;

	image_build(f);
	CHECK(fsimg_ctx_new(NULL, &f->ctx, &f->error) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io,
	    &f->error) == FSIMG_OK);
	return (status_is(f, fsimg_reader_open(f->ctx, f->io, NULL,
	    &f->reader, &f->error), FSIMG_OK));
}

/* Extents belong to the private ID, independently of the directory inode. */
static void
add_file(content_fixture_t *f, uint64_t oid, uint64_t private_id,
    const char *name, uint64_t size, uint64_t allocated)
{
	unsigned char stream[40], raw[1024];
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	apfs_gen_dstream_t value = { .size = size, .alloced_size = allocated };
	apfs_gen_xfield_t field = { .type = 8, .size = 40, .data = stream };

	CHECK(apfs_gen_dstream_build(stream, sizeof(stream), &value));
	inode.private_id = private_id;
	inode.xfields_len = fields_build(raw, &field, 1);
	inode.xfields = raw;
	add_inode(f, oid, &inode);
	add_drec(f, 2, oid, name, 8);
}

static void
add_extent(content_fixture_t *f, uint64_t private_id, uint64_t logical,
    const apfs_gen_extent_val_t *value)
{
	content_record_t *r = record_new(f);

	r->key_size = 16;
	r->value_size = 24;
	CHECK(apfs_gen_extent_key_build(r->key, 16, private_id, logical));
	CHECK(apfs_gen_extent_val_build(r->value, 24, value));
}

static bool
open_file(content_fixture_t *f, const char *path, uint64_t size)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	if (!content_open(f))
		return (false);
	while ((status = fsimg_reader_next(f->reader, &entry, &f->error)) ==
	    FSIMG_OK) {
		if (strcmp(entry->path, path) == 0) {
			expect(entry->size == size, "file has exact 64-bit size");
			return (true);
		}
	}
	status_is(f, status, FSIMG_OK);
	expect(false, "requested file was listed");
	return (false);
}

/* Compare independently assembled bytes; the unread suffix stays poisoned. */
static void
expect_read(content_fixture_t *f, uint64_t offset, size_t request,
    const unsigned char *expected, size_t prefix, fsimg_status_t wanted,
    const char *message)
{
	unsigned char *bytes;
	fsimg_status_t status;
	size_t count = SIZE_MAX;

	bytes = malloc(request);
	CHECK(bytes != NULL);
	memset(bytes, 0xa5, request);
	status = fsimg_reader_read_at(f->reader, offset, bytes, request,
	    &count, &f->error);
	status_is(f, status, wanted);
	if (count != prefix) {
		fprintf(stderr, "%s: expected delivered prefix %zu, got %zu\n",
		    scenario, prefix, count);
		expect(false, "delivered prefix ends exactly before refusal");
	}
	if (count == prefix)
		expect(bcmp(bytes, expected, prefix) == 0, "exact content bytes");
	for (size_t i = prefix; i < request; i++) {
		if (bytes[i] != 0xa5) {
			expect(false, "no byte delivered beyond confirmed prefix");
			break;
		}
	}
	if (wanted != FSIMG_OK) {
		expect(f->error.operation == FSIMG_OP_READER,
		    "content errors identify reader operation");
		expect(strcmp(f->error.message, message) == 0, message);
	}
	free(bytes);
}

static void
test_holes_and_unrecorded_tail_read_as_zero(void)
{
	content_fixture_t f;
	unsigned char expected[7 * CONTENT_BLOCK];

	scenario = "leading middle trailing holes and unrecorded tail";
	fixture_init(&f, 16);
	add_root(&f);
	add_file(&f, 16, 100, "file", sizeof(expected), 0);
	for (unsigned i = 0; i < 6; i++) {
		apfs_gen_extent_val_t extent = { .length = CONTENT_BLOCK,
		    .phys_block = i % 2 == 0 || i == 5 ? 0 : 4000 + i };

		add_extent(&f, 100, i * CONTENT_BLOCK, &extent);
	}
	bzero(expected, sizeof(expected));
	memset(expected + CONTENT_BLOCK, 'a', CONTENT_BLOCK);
	memset(expected + 3 * CONTENT_BLOCK, 'b', CONTENT_BLOCK);
	memset(block(&f, 4001), 'a', CONTENT_BLOCK);
	memset(block(&f, 4003), 'b', CONTENT_BLOCK);
	if (open_file(&f, "/file", sizeof(expected))) {
		expect_read(&f, 0, sizeof(expected), expected, sizeof(expected),
		    FSIMG_OK, NULL);
		for (unsigned i = 7; i > 0; i--)
			expect_read(&f, (i - 1) * CONTENT_BLOCK, 17,
			    expected + (i - 1) * CONTENT_BLOCK, 17, FSIMG_OK, NULL);
	}
	fixture_free(&f);
}

static void
test_extent_lengths_and_random_boundaries(void)
{
	static const unsigned lengths[] = { 1, 3, 64 };
	static const size_t boundaries[] = {
		68 * CONTENT_BLOCK, 4 * CONTENT_BLOCK, CONTENT_BLOCK
	};
	content_fixture_t f;
	unsigned char expected[68 * CONTENT_BLOCK];
	size_t logical = 0;

	scenario = "one block several blocks and long extent boundaries";
	fixture_init(&f, 16);
	add_root(&f);
	add_file(&f, 16, 100, "file", sizeof(expected), 0);
	for (size_t i = 0; i < COUNT(lengths); i++) {
		apfs_gen_extent_val_t extent = {
		    .length = lengths[i] * CONTENT_BLOCK,
		    .phys_block = 4000 + logical / CONTENT_BLOCK };

		add_extent(&f, 100, logical, &extent);
		for (size_t j = 0; j < extent.length; j++)
			expected[logical + j] = (unsigned char)(17 * i + j % 251);
		logical += (size_t)extent.length;
	}
	bcopy(expected, block(&f, 4000), sizeof(expected));
	if (open_file(&f, "/file", sizeof(expected))) {
		expect_read(&f, 0, sizeof(expected), expected, sizeof(expected),
		    FSIMG_OK, NULL);
		/* Descending offsets force seeks rather than cursor continuation. */
		for (size_t i = 0; i < COUNT(boundaries); i++) {
			size_t boundary = boundaries[i];

			expect_read(&f, boundary - 1, 2, expected + boundary - 1,
			    boundary == sizeof(expected) ? 1 : 2, FSIMG_OK, NULL);
			expect_read(&f, boundary, 1, expected + boundary,
			    boundary == sizeof(expected) ? 0 : 1, FSIMG_OK, NULL);
			expect_read(&f, boundary - CONTENT_BLOCK / 2, 31,
			    expected + boundary - CONTENT_BLOCK / 2, 31,
			    FSIMG_OK, NULL);
		}
	}
	fixture_free(&f);
}

typedef struct refused_extent {
	const char *name;
	const char *message;
	uint64_t logical;
	apfs_gen_extent_val_t extent;
	fsimg_status_t status;
} refused_extent_t;

static void
test_refused_extents_preserve_only_the_valid_prefix(void)
{
	static const refused_extent_t cases[] = {
		{ .name = "gap",
		    .message = "APFS file extent leaves a gap after the extent before it",
		    .logical = 3 * CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "overlap",
		    .message = "APFS file extent overlaps the extent before it",
		    .logical = CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "physical start outside",
		    .message = "APFS file extent lies outside the container",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = CONTENT_BLOCKS },
		    .status = FSIMG_ECORRUPT },
		{ .name = "physical end outside",
		    .message = "APFS file extent lies outside the container",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = 2 * CONTENT_BLOCK, .phys_block = CONTENT_BLOCKS - 1 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "zero length",
		    .message = "APFS file extent is not in whole blocks",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "partial block length",
		    .message = "APFS file extent is not in whole blocks",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK - 1, .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "partial block offset",
		    .message = "APFS file extent is not in whole blocks",
		    .logical = 2 * CONTENT_BLOCK + 1,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "end past stream",
		    .message = "APFS file extent runs past its data stream",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = 3 * CONTENT_BLOCK, .phys_block = 4002 },
		    .status = FSIMG_ECORRUPT },
		{ .name = "unsupported flags",
		    .message = "APFS file extent has unsupported flags",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = 4002, .flags = 1 },
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "encrypted extent",
		    .message = "APFS file extent is encrypted",
		    .logical = 2 * CONTENT_BLOCK,
		    .extent = { .length = CONTENT_BLOCK, .phys_block = 4002, .crypto_id = 1 },
		    .status = FSIMG_EUNSUPPORTED }
	};
	content_fixture_t f;
	unsigned char expected[4 * CONTENT_BLOCK];

	memset(expected, 'p', sizeof(expected));
	for (size_t i = 0; i < COUNT(cases); i++) {
		for (unsigned damaged = 0; damaged < 2; damaged++) {
			apfs_gen_extent_val_t first = { .length = 2 * CONTENT_BLOCK,
			    .phys_block = 4000 };
			apfs_gen_extent_val_t valid = first;

			scenario = cases[i].name;
			fixture_init(&f, 8);
			add_root(&f);
			add_file(&f, 16, 100, "file", sizeof(expected), 0);
			add_extent(&f, 100, 0, &first);
			valid.phys_block = 4002;
			add_extent(&f, 100, damaged ? cases[i].logical :
			    2 * CONTENT_BLOCK, damaged ? &cases[i].extent : &valid);
			bcopy(expected, block(&f, 4000), sizeof(expected));
			if (open_file(&f, "/file", sizeof(expected)))
				expect_read(&f, 0, sizeof(expected), expected,
				    damaged ? 2 * CONTENT_BLOCK : sizeof(expected),
				    damaged ? cases[i].status : FSIMG_OK,
				    cases[i].message);
			fixture_free(&f);
		}
	}
}

static void
test_extent_reaching_file_size_requires_contiguous_successor(void)
{
	content_fixture_t f;
	unsigned char expected[4 * CONTENT_BLOCK];
	apfs_gen_extent_val_t first = { .length = sizeof(expected),
	    .phys_block = 4000 };

	/* Whenever the extent returned reaches the size, the record after it
	 * is checked before any byte is delivered; the reads here start at
	 * offset zero.
	 */
	memset(expected, 'p', sizeof(expected));
	for (unsigned gap = 0; gap < 2; gap++) {
		size_t offset = 0;
		size_t request = sizeof(expected);

		for (unsigned damaged = 0; damaged < 2; damaged++) {
			apfs_gen_extent_val_t next = {
			    .length = (damaged ? 1 : 2) * CONTENT_BLOCK,
			    .phys_block = 4004 };
			uint64_t logical = damaged ?
			    (gap ? 6 : 1) * CONTENT_BLOCK : sizeof(expected);
			const char *message = gap ?
			    "APFS file extent leaves a gap after the extent before it" :
			    "APFS file extent overlaps the extent before it";

			scenario = damaged ? (gap ? "gap after file-sized extent" :
			    "overlap after file-sized extent") :
			    "contiguous preallocated successor past file size";
			fixture_init(&f, 8);
			add_root(&f);
			add_file(&f, 16, 100, "file", sizeof(expected),
			    (damaged && gap ? 8 : 6) * CONTENT_BLOCK);
			add_extent(&f, 100, 0, &first);
			add_extent(&f, 100, logical, &next);
			bcopy(expected, block(&f, 4000), sizeof(expected));
			memset(block(&f, 4004), 's', 2 * CONTENT_BLOCK);
			if (open_file(&f, "/file", sizeof(expected)))
				expect_read(&f, offset, request, expected + offset,
				    damaged ? 0 : request,
				    damaged ? FSIMG_ECORRUPT : FSIMG_OK, message);
			fixture_free(&f);
		}
	}
}

static void
test_first_extent_starts_at_zero(void)
{
	content_fixture_t f;
	unsigned char expected[2 * CONTENT_BLOCK] = { 0 };
	apfs_gen_extent_val_t extent = { .length = CONTENT_BLOCK };

	for (unsigned displaced = 0; displaced < 2; displaced++) {
		scenario = "first extent starts at zero";
		fixture_init(&f, 8);
		add_root(&f);
		add_file(&f, 16, 100, "file", sizeof(expected), 0);
		add_extent(&f, 100, displaced * CONTENT_BLOCK, &extent);
		if (open_file(&f, "/file", sizeof(expected)))
			expect_read(&f, 0, sizeof(expected), expected,
			    displaced ? 0 : sizeof(expected),
			    displaced ? FSIMG_ECORRUPT : FSIMG_OK,
			    "APFS file extent does not start the stream");
		fixture_free(&f);
	}
}

static void
test_allocated_bytes_allow_extents_past_rounded_size(void)
{
	content_fixture_t f;
	unsigned char expected[CONTENT_BLOCK + 1] = { 0 };
	apfs_gen_extent_val_t extent = { .length = 3 * CONTENT_BLOCK };

	for (unsigned allocated = 0; allocated < 3; allocated++) {
		scenario = "extent end bounded by rounded size or allocated bytes";
		fixture_init(&f, 8);
		add_root(&f);
		add_file(&f, 16, 100, "file", sizeof(expected),
		    allocated == 1 ? 3 * CONTENT_BLOCK : 0);
		extent.length = allocated == 2 ? 2 * CONTENT_BLOCK :
		    3 * CONTENT_BLOCK;
		add_extent(&f, 100, 0, &extent);
		if (open_file(&f, "/file", sizeof(expected)))
			expect_read(&f, 0, sizeof(expected), expected,
			    allocated ? sizeof(expected) : 0,
			    allocated ? FSIMG_OK : FSIMG_ECORRUPT,
			    "APFS file extent runs past its data stream");
		fixture_free(&f);
	}
}

static void
test_sparse_file_offsets_above_four_gib(void)
{
	const uint64_t hole = (UINT64_C(1) << 32) + CONTENT_BLOCK;
	content_fixture_t f;
	unsigned char zero[64] = { 0 }, last[CONTENT_BLOCK];
	apfs_gen_extent_val_t extent = { .length = hole };

	scenario = "sparse file beyond four GiB";
	fixture_init(&f, 8);
	add_root(&f);
	add_file(&f, 16, 100, "file", hole + CONTENT_BLOCK, CONTENT_BLOCK);
	add_extent(&f, 100, 0, &extent);
	extent.length = CONTENT_BLOCK;
	extent.phys_block = 4000;
	add_extent(&f, 100, hole, &extent);
	for (size_t i = 0; i < sizeof(last); i++)
		last[i] = (unsigned char)(i % 251);
	bcopy(last, block(&f, 4000), sizeof(last));
	if (open_file(&f, "/file", hole + CONTENT_BLOCK)) {
		expect_read(&f, hole + CONTENT_BLOCK - 64, 64,
		    last + CONTENT_BLOCK - 64, 64, FSIMG_OK, NULL);
		expect_read(&f, (UINT64_C(1) << 32) + 17, sizeof(zero),
		    zero, sizeof(zero), FSIMG_OK, NULL);
		expect_read(&f, hole, sizeof(last), last, sizeof(last), FSIMG_OK, NULL);
	}
	fixture_free(&f);
}

static void
test_clones_use_their_own_private_extent_records(void)
{
	content_fixture_t f;
	unsigned char expected[3 * CONTENT_BLOCK];
	apfs_gen_extent_val_t extent;

	for (unsigned damaged = 0; damaged < 2; damaged++) {
		for (unsigned clone = 0; clone < 2; clone++) {
			scenario = "clones share blocks but own their extent records";
			fixture_init(&f, 16);
			add_root(&f);
			add_file(&f, 16, 100, "a", sizeof(expected), 0);
			add_file(&f, 17, 101, "b", sizeof(expected), 0);
			extent = (apfs_gen_extent_val_t){ .length = 3 * CONTENT_BLOCK,
			    .phys_block = 4000, .flags = damaged ? 1 : 0 };
			add_extent(&f, 100, 0, &extent);
			extent.length = CONTENT_BLOCK;
			extent.flags = 0;
			add_extent(&f, 101, 0, &extent);
			extent.phys_block = 0;
			add_extent(&f, 101, CONTENT_BLOCK, &extent);
			extent.phys_block = 4002;
			add_extent(&f, 101, 2 * CONTENT_BLOCK, &extent);
			memset(block(&f, 4000), 'c', sizeof(expected));
			memset(expected, 'c', sizeof(expected));
			if (clone)
				bzero(expected + CONTENT_BLOCK, CONTENT_BLOCK);
			if (open_file(&f, clone ? "/b" : "/a", sizeof(expected)))
				expect_read(&f, 0, sizeof(expected), expected,
				    damaged && !clone ? 0 : sizeof(expected),
				    damaged && !clone ? FSIMG_EUNSUPPORTED : FSIMG_OK,
				    "APFS file extent has unsupported flags");
			fixture_free(&f);
		}
	}
}

static void
test_truncated_mapped_extent_requires_salvage_to_keep_prefix(void)
{
	content_fixture_t f;
	unsigned char expected[2 * CONTENT_BLOCK];
	apfs_gen_extent_val_t extent = { .length = sizeof(expected),
	    .phys_block = 4000 };
	const size_t prefix = CONTENT_BLOCK + 37;
	bool damaged;

	for (unsigned salvage = 0; salvage < 2; salvage++) {
		scenario = salvage ? "salvage keeps mapped prefix at slice end" :
		    "plain read refuses mapped extent past slice end";
		fixture_init(&f, 8);
		add_root(&f);
		add_file(&f, 16, 100, "file", sizeof(expected), 0);
		add_extent(&f, 100, 0, &extent);
		for (size_t i = 0; i < sizeof(expected); i++)
			expected[i] = (unsigned char)(i % 251);
		bcopy(expected, block(&f, 4000), sizeof(expected));
		f.available = 4000 * CONTENT_BLOCK + prefix;
		if (open_file(&f, "/file", sizeof(expected))) {
			if (salvage) {
				f.reader->salvage.enabled = true;
				if (status_is(&f, fsimg_reader_salvage_plan(f.reader,
				    &damaged, &f.error), FSIMG_OK)) {
					expect(damaged, "salvage marks the file damaged");
					expect(f.reader->entry.size == prefix,
					    "salvage keeps exactly the bytes before slice end");
					expect_read(&f, 0, sizeof(expected), block(&f, 4000),
					    prefix, FSIMG_OK, NULL);
				}
			} else {
				expect_read(&f, 0, sizeof(expected), expected, 0,
				    FSIMG_ECORRUPT, "image is truncated");
			}
		}
		fixture_free(&f);
	}
}

static void
test_compressed_content_is_refused_before_delivery(void)
{
	unsigned char header[] = { 'f', 'p', 'm', 'c', 3, 0, 0, 0,
	    123, 0, 0, 0, 0, 0, 0, 0 };
	unsigned char expected[123] = { 0 };
	apfs_gen_inode_val_t inode;
	content_fixture_t f;

	for (unsigned compressed = 0; compressed < 2; compressed++) {
		scenario = "compressed content is unsupported";
		fixture_init(&f, 8);
		add_root(&f);
		if (compressed) {
			inode = inode_value(2, 8);
			inode.bsd_flags = 0x20;
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, "file", 8);
			add_xattr(&f, 16, "com.apple.decmpfs", header,
			    sizeof(header), 2);
		} else {
			add_file(&f, 16, 100, "file", sizeof(expected), 0);
		}
		if (open_file(&f, "/file", sizeof(expected)))
			expect_read(&f, 0, sizeof(expected), expected,
			    compressed ? 0 : sizeof(expected),
			    compressed ? FSIMG_EUNSUPPORTED : FSIMG_OK,
			    "APFS compressed file content is not decoded");
		fixture_free(&f);
	}
}

int
main(void)
{
	test_holes_and_unrecorded_tail_read_as_zero();
	test_extent_lengths_and_random_boundaries();
	test_refused_extents_preserve_only_the_valid_prefix();
	test_extent_reaching_file_size_requires_contiguous_successor();
	test_first_extent_starts_at_zero();
	test_allocated_bytes_allow_extents_past_rounded_size();
	test_sparse_file_offsets_above_four_gib();
	test_clones_use_their_own_private_extent_records();
	test_truncated_mapped_extent_requires_salvage_to_keep_prefix();
	test_compressed_content_is_refused_before_delivery();
	printf("APFS content: %u checks, %u failures\n", checks, failures);
	return (failures == 0 ? 0 : 1);
}
