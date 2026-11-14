/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>

#include "apfs-gen.h"
#include "fsimg-endian.h"
#include "fsimg.h"
#include "host.h"
#include "lzfse-vectors.h"
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
		if (message != NULL)
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

/* Stream OIDs and physical regions are distinct from inode and tree storage. */
static void
add_compression_stream(content_fixture_t *f, const char *name,
    uint64_t oid, uint64_t physical, const unsigned char *bytes, size_t size)
{
	content_record_t *r = record_new(f);
	apfs_gen_name_key_t key = { .oid = 16, .name = name,
	    .name_len = (uint16_t)strlen(name) };
	uint64_t allocated = (size + CONTENT_BLOCK - 1) /
	    CONTENT_BLOCK * CONTENT_BLOCK;
	apfs_gen_xattr_val_t value = { .flags = 1, .data_len = 48,
	    .stream_oid = oid, .dstream = { .size = size,
	    .alloced_size = allocated } };
	apfs_gen_extent_val_t extent = { .length = allocated,
	    .phys_block = physical };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = 52;
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &value));
	add_extent(f, oid, 0, &extent);
	bcopy(bytes, block(f, physical), size);
}

static void
compressed_fixture(content_fixture_t *f, const unsigned char *value,
    size_t value_size, bool streamed, const unsigned char *fork,
    size_t fork_size)
{
	apfs_gen_inode_val_t inode = inode_value(2, 8);

	fixture_init(f, 12);
	add_root(f);
	inode.bsd_flags = 0x20;
	add_inode(f, 16, &inode);
	add_drec(f, 2, 16, "file", 8);
	if (streamed)
		add_compression_stream(f, "com.apple.decmpfs", 100, 4000,
		    value, value_size);
	else {
		CHECK(value_size + 4 <= sizeof(f->records[0].value));
		add_xattr(f, 16, "com.apple.decmpfs", value,
		    (uint16_t)value_size, 2);
	}
	if (fork != NULL)
		add_compression_stream(f, "com.apple.ResourceFork", 101, 4100,
		    fork, fork_size);
}

#define COMPRESS_CHUNK 65536u
#define COMPRESS_SIZE (3 * COMPRESS_CHUNK + 37)
#define COMPRESS_CAPACITY (COMPRESS_SIZE + 4096)

static void
source_fill(unsigned char *source, size_t size)
{
	for (size_t i = 0; i < size; i++)
		source[i] = (unsigned char)((i / COMPRESS_CHUNK * 71 + i) % 251);
}

/* Reopen for extraction: verification and enumeration consume reader state. */
static void
expect_extracted_bytes(content_fixture_t *f, const unsigned char *source,
    size_t size)
{
	char directory[] = "/tmp/fsimg-apfs-compress-XXXXXX";
	fsimg_extract_options_t options;
	unsigned char bytes[4096];
	int parent, output, file;
	size_t offset = 0, request;
	ssize_t count;
	fsimg_status_t status;

	CHECK(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	fsimg_reader_free(f->reader);
	f->reader = NULL;
	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader,
	    &f->error) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	status = fsimg_extract_at(f->reader, parent, "out", &options, &f->error);
	if (status_is(f, status, FSIMG_OK)) {
		output = openat(parent, "out", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(output != -1);
		file = openat(output, "file", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		CHECK(file != -1);
		while (offset < size) {
			request = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
			count = read(file, bytes, request);
			if (count <= 0) {
				expect(false, "extracted file contains the complete source");
				break;
			}
			expect(bcmp(bytes, source + offset, (size_t)count) == 0,
			    "extracted bytes equal source bytes");
			offset += (size_t)count;
		}
		expect(read(file, bytes, 1) == 0, "extracted file ends at source size");
		CHECK(close(file) == 0);
		CHECK(unlinkat(output, "file", 0) == 0);
		CHECK(close(output) == 0);
		CHECK(unlinkat(parent, "out", AT_REMOVEDIR) == 0);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
}

static void
test_supported_compression_types_deliver_exact_bytes(void)
{
	static const uint32_t types[] = { 3, 4, 7, 8, 9, 10, 11, 12 };
	unsigned char *source = malloc(COMPRESS_SIZE);
	unsigned char *value = malloc(COMPRESS_CAPACITY);
	unsigned char *fork = malloc(COMPRESS_CAPACITY);
	content_fixture_t f;
	char label[128];
	size_t size, stored;
	bool resource;

	CHECK(source != NULL && value != NULL && fork != NULL);
	source_fill(source, COMPRESS_SIZE);
	for (size_t i = 0; i < COUNT(types); i++) {
		resource = types[i] % 2 == 0;
		for (unsigned large = 0; large < 2; large++) {
			size = large ? (resource ? COMPRESS_SIZE : COMPRESS_CHUNK) : 123;
			CHECK(snprintf(label, sizeof(label),
			    "type %u %s payload %zu bytes", types[i],
			    resource ? "fork" : large ? "streamed inline" : "record inline",
			    size) < (int)sizeof(label));
			scenario = label;
			apfs_gen_decmpfs_header(value, types[i], size);
			if (resource) {
				stored = apfs_gen_compressed_fork(fork, COMPRESS_CAPACITY,
				    types[i], source, size);
				CHECK(stored != 0);
				compressed_fixture(&f, value, 16, false, fork, stored);
			} else {
				stored = apfs_gen_compressed_payload(value + 16,
				    COMPRESS_CAPACITY - 16, types[i], source, size);
				CHECK(stored != 0);
				compressed_fixture(&f, value, 16 + stored, large != 0, NULL, 0);
			}
			if (open_file(&f, "/file", size)) {
				expect_read(&f, 0, size, source, size, FSIMG_OK, NULL);
				/* Revisit chunks in reverse order to invalidate the held window. */
				for (size_t end = size; end > 0;) {
					size_t start = (end - 1) / COMPRESS_CHUNK * COMPRESS_CHUNK;

					expect_read(&f, start, end - start, source + start,
					    end - start, FSIMG_OK, NULL);
					end = start;
				}
			}
			expect_extracted_bytes(&f, source, size);
			fixture_free(&f);
		}
	}
	free(fork);
	free(value);
	free(source);
}

/* The LZVN vector's 12-byte block header and 4-byte end block wrap the
 * standalone opcode stream that decmpfs types 7 and 8 store directly.
 */
static void
test_encoded_lzvn_and_lzfse_deliver_exact_bytes_and_reject_size_mismatch(void)
{
	static const struct {
		uint32_t type;
		const lzfse_vector_t *vector;
		size_t skip;
		size_t trim;
	} cases[] = {
		{ .type = 7, .vector = &lzfse_vectors[1], .skip = 12, .trim = 4 },
		{ .type = 8, .vector = &lzfse_vectors[1], .skip = 12, .trim = 4 },
		{ .type = 11, .vector = &lzfse_vectors[2] },
		{ .type = 12, .vector = &lzfse_vectors[2] }
	};
	unsigned char source[8192], value[1024], fork[1024];
	content_fixture_t f;
	char label[128];
	size_t stored, declared_size;
	bool resource;

	for (size_t i = 0; i < COUNT(cases); i++) {
		const lzfse_vector_t *vector = cases[i].vector;
		const unsigned char *encoded = vector->encoded + cases[i].skip;

		stored = vector->encoded_size - cases[i].skip - cases[i].trim;
		CHECK(vector->plain_size <= sizeof(source));
		CHECK(stored + 16 <= sizeof(value) && stored + 8 <= sizeof(fork));
		for (size_t j = 0; j < vector->plain_size; j++)
			source[j] = lzfse_plain_byte(vector, j);
		resource = cases[i].type % 2 == 0;
		for (int delta = -1; delta <= 1; delta++) {
			declared_size = (size_t)((int)vector->plain_size + delta);
			CHECK(snprintf(label, sizeof(label),
			    "encoded type %u decodes %zu bytes, declares %zu",
			    cases[i].type, vector->plain_size, declared_size) <
			    (int)sizeof(label));
			scenario = label;
			apfs_gen_decmpfs_header(value, cases[i].type, declared_size);
			if (resource) {
				/* One chunk: table end followed by payload end. */
				fsimg_store_le32(fork, 8);
				fsimg_store_le32(fork + 4, (uint32_t)(8 + stored));
				bcopy(encoded, fork + 8, stored);
				compressed_fixture(&f, value, 16, false, fork, 8 + stored);
			} else {
				bcopy(encoded, value + 16, stored);
				compressed_fixture(&f, value, 16 + stored, false, NULL, 0);
			}
			if (open_file(&f, "/file", declared_size))
				expect_read(&f, 0, declared_size, source,
				    delta == 0 ? vector->plain_size : 0,
				    delta == 0 ? FSIMG_OK : FSIMG_ECORRUPT, NULL);
			if (delta == 0)
				expect_extracted_bytes(&f, source, vector->plain_size);
			fixture_free(&f);
		}
	}
}

static void
test_unsupported_compression_lists_but_refuses_content(void)
{
	static const uint32_t types[] = { 1, 13, 14 };
	unsigned char header[16], fork[8] = { 0 }, expected = 0;
	content_fixture_t f;
	char label[80];

	for (size_t i = 0; i < COUNT(types); i++) {
		CHECK(snprintf(label, sizeof(label), "type %u lists but refuses content",
		    types[i]) < (int)sizeof(label));
		scenario = label;
		apfs_gen_decmpfs_header(header, types[i], 123);
		compressed_fixture(&f, header, sizeof(header), false,
		    types[i] == 14 ? fork : NULL, sizeof(fork));
		if (open_file(&f, "/file", 123))
			expect_read(&f, 0, 1, &expected, 0, FSIMG_EUNSUPPORTED,
			    "APFS compressed file compression type is not supported");
		fixture_free(&f);
	}
}

static void
test_truncated_decmpfs_headers_fail_listing(void)
{
	unsigned char header[16];
	const fsimg_entry_t *entry;
	content_fixture_t f;
	char label[80];

	apfs_gen_decmpfs_header(header, 3, 123);
	for (unsigned streamed = 0; streamed < 2; streamed++) {
		for (size_t size = 0; size < 16; size++) {
			/* A zero-byte stream has no extent to emit. */
			if (streamed && size == 0)
				continue;
			CHECK(snprintf(label, sizeof(label), "truncated %s header %zu bytes",
			    streamed ? "streamed" : "record", size) < (int)sizeof(label));
			scenario = label;
			compressed_fixture(&f, header, size, streamed != 0, NULL, 0);
			if (content_open(&f))
				status_is(&f, fsimg_reader_next(f.reader, &entry, &f.error),
				    FSIMG_ECORRUPT);
			fixture_free(&f);
		}
	}
}

typedef enum fork_damage {
	FORK_HEADER_TRUNCATED,
	FORK_CHUNKS_OVERLAP,
	FORK_ENTRY_SHORT,
	FORK_RAW_SHORT,
	FORK_RAW_LONG,
	FORK_ZLIB_SIZE
} fork_damage_t;

static void
test_corrupt_forks_refuse_unconfirmed_bytes(void)
{
	static const struct {
		const char *name;
		uint32_t type;
		fork_damage_t damage;
		size_t decoded_size;
		const char *message;
	} cases[] = {
		{ .name = "truncated resource header", .type = 4,
		    .damage = FORK_HEADER_TRUNCATED,
		    .message = "decmpfs resource fork is too short" },
		{ .name = "overlapping resource chunks", .type = 4,
		    .damage = FORK_CHUNKS_OVERLAP,
		    .message = "decmpfs chunks overlap" },
		{ .name = "short resource chunk entry", .type = 4,
		    .damage = FORK_ENTRY_SHORT,
		    .message = "APFS compressed file zlib chunk does not decode to its length" },
		{ .name = "raw chunk produces fewer bytes", .type = 10,
		    .damage = FORK_RAW_SHORT,
		    .message = "decmpfs uncompressed payload or chunk has the wrong length" },
		{ .name = "raw chunk produces more bytes", .type = 10,
		    .damage = FORK_RAW_LONG,
		    .message = "decmpfs uncompressed payload or chunk has the wrong length" },
		{ .name = "zlib chunk produces fewer bytes", .type = 4,
		    .damage = FORK_ZLIB_SIZE, .decoded_size = COMPRESS_CHUNK - 1,
		    .message = "APFS compressed file zlib chunk does not decode to its length" },
		{ .name = "zlib chunk produces more bytes", .type = 4,
		    .damage = FORK_ZLIB_SIZE, .decoded_size = COMPRESS_CHUNK + 1,
		    .message = "APFS compressed file zlib chunk does not decode to its length" },
		{ .name = "zlib expansion bomb", .type = 4,
		    .damage = FORK_ZLIB_SIZE, .decoded_size = 4 * 1024 * 1024,
		    .message = "APFS compressed file zlib chunk does not decode to its length" }
	};
	unsigned char *source = malloc(4 * 1024 * 1024);
	unsigned char *fork = malloc(COMPRESS_CAPACITY);
	unsigned char header[16];
	content_fixture_t f;
	size_t stored, length, entry, expected_size;

	CHECK(source != NULL && fork != NULL);
	source_fill(source, 4 * 1024 * 1024);
	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		expected_size = cases[i].damage == FORK_ZLIB_SIZE ?
		    COMPRESS_CHUNK : COMPRESS_SIZE;
		apfs_gen_decmpfs_header(header, cases[i].type, expected_size);
		stored = apfs_gen_compressed_fork(fork, COMPRESS_CAPACITY,
		    cases[i].type, source, COMPRESS_SIZE);
		CHECK(stored != 0);
		entry = 24;
		if (cases[i].damage == FORK_HEADER_TRUNCATED)
			stored = 15;
		else if (cases[i].damage == FORK_CHUNKS_OVERLAP)
			/* Second entry starts at the first entry's offset. */
			bcopy(fork + entry, fork + entry + 8, 4);
		else if (cases[i].damage == FORK_ENTRY_SHORT) {
			length = (size_t)integer(fork + entry + 4, 4);
			for (unsigned j = 0; j < 4; j++)
				fork[entry + 4 + j] = (unsigned char)((length - 1) >> (8 * j));
		} else if (cases[i].damage == FORK_RAW_SHORT ||
		    cases[i].damage == FORK_RAW_LONG) {
			length = (size_t)integer(fork + 4, 4);
			length = cases[i].damage == FORK_RAW_SHORT ? length - 1 : length + 1;
			for (unsigned j = 0; j < 4; j++)
				fork[4 + j] = (unsigned char)(length >> (8 * j));
		} else {
			const size_t data_offset = 16, chunk_base = 20;
			const size_t count_offset = 20, entry_offset = 24;
			const size_t payload_offset = 32;

			length = apfs_gen_compressed_payload(fork + payload_offset,
			    COMPRESS_CAPACITY - payload_offset, 4,
			    source, cases[i].decoded_size);
			CHECK(length != 0);
			/* The header declares 64 KiB; this chunk decodes decoded_size. */
			stored = payload_offset + length;
			bzero(fork, payload_offset);
			fsimg_store_be32(fork, (uint32_t)data_offset);
			fsimg_store_be32(fork + 4, (uint32_t)stored);
			fsimg_store_be32(fork + 8, (uint32_t)(stored - data_offset));
			fsimg_store_be32(fork + data_offset,
			    (uint32_t)(stored - chunk_base));
			fsimg_store_le32(fork + count_offset, 1);
			fsimg_store_le32(fork + entry_offset,
			    (uint32_t)(payload_offset - chunk_base));
			fsimg_store_le32(fork + entry_offset + 4, (uint32_t)length);
		}
		compressed_fixture(&f, header, sizeof(header), false, fork, stored);
		if (open_file(&f, "/file", expected_size)) {
			expect_read(&f, 0, COMPRESS_CHUNK, source, 0,
			    FSIMG_ECORRUPT, cases[i].message);
			if (cases[i].decoded_size == 4 * 1024 * 1024) {
				fsimg_stats_t stats;

				CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
				expect(stats.memory_peak < 2 * 1024 * 1024,
				    "expansion bomb uses less memory than its 4 MiB output");
				expect(f.reader->session.progress.work < 1024 * 1024,
				    "expansion bomb stops within one chunk of decode work");
			}
		}
		fixture_free(&f);
	}
	free(fork);
	free(source);
}

static void
test_damaged_last_entry_preserves_earlier_random_reads(void)
{
	unsigned char *source = malloc(COMPRESS_SIZE);
	unsigned char *fork = malloc(COMPRESS_CAPACITY);
	unsigned char header[16];
	content_fixture_t f;
	size_t stored;

	scenario = "damaged last word-table entry preserves earlier chunks";
	CHECK(source != NULL && fork != NULL);
	source_fill(source, COMPRESS_SIZE);
	apfs_gen_decmpfs_header(header, 10, COMPRESS_SIZE);
	stored = apfs_gen_compressed_fork(fork, COMPRESS_CAPACITY, 10,
	    source, COMPRESS_SIZE);
	CHECK(stored != 0);
	memset(fork + 16, 0xff, 4);
	compressed_fixture(&f, header, sizeof(header), false, fork, stored);
	if (open_file(&f, "/file", COMPRESS_SIZE)) {
		expect_read(&f, COMPRESS_CHUNK, COMPRESS_CHUNK,
		    source + COMPRESS_CHUNK, COMPRESS_CHUNK, FSIMG_OK, NULL);
		expect_read(&f, 0, COMPRESS_CHUNK, source, COMPRESS_CHUNK, FSIMG_OK, NULL);
		expect_read(&f, 3 * COMPRESS_CHUNK, 37,
		    source + 3 * COMPRESS_CHUNK, 0, FSIMG_ECORRUPT, NULL);
	}
	fixture_free(&f);
	free(fork);
	free(source);
}

static void
test_salvage_stops_before_bad_compressed_chunk(void)
{
	unsigned char *source = malloc(COMPRESS_SIZE);
	unsigned char *fork = malloc(COMPRESS_CAPACITY);
	unsigned char header[16];
	content_fixture_t f;
	size_t stored, offset;
	bool damaged;

	scenario = "salvage retains two complete chunks before bad chunk 2";
	CHECK(source != NULL && fork != NULL);
	source_fill(source, COMPRESS_SIZE);
	apfs_gen_decmpfs_header(header, 10, COMPRESS_SIZE);
	stored = apfs_gen_compressed_fork(fork, COMPRESS_CAPACITY, 10,
	    source, COMPRESS_SIZE);
	CHECK(stored != 0);
	offset = (size_t)integer(fork + 8, 4);
	fork[offset] = 0;
	compressed_fixture(&f, header, sizeof(header), false, fork, stored);
	if (open_file(&f, "/file", COMPRESS_SIZE)) {
		f.reader->salvage.enabled = true;
		if (status_is(&f, fsimg_reader_salvage_plan(f.reader, &damaged,
		    &f.error), FSIMG_OK)) {
			expect(damaged, "salvage marks compressed file damaged");
			expect(f.reader->entry.size == 2 * COMPRESS_CHUNK,
			    "salvage ends exactly before chunk 2");
			expect_read(&f, 0, COMPRESS_SIZE, source,
			    2 * COMPRESS_CHUNK, FSIMG_OK, NULL);
		}
	}
	fixture_free(&f);
	free(fork);
	free(source);
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
	test_supported_compression_types_deliver_exact_bytes();
	test_encoded_lzvn_and_lzfse_deliver_exact_bytes_and_reject_size_mismatch();
	test_unsupported_compression_lists_but_refuses_content();
	test_truncated_decmpfs_headers_fail_listing();
	test_corrupt_forks_refuse_unconfirmed_bytes();
	test_damaged_last_entry_preserves_earlier_random_reads();
	test_salvage_stops_before_bad_compressed_chunk();
	printf("APFS content: %u checks, %u failures\n", checks, failures);
	return (failures == 0 ? 0 : 1);
}
