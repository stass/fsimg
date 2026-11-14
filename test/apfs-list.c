/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "apfs-gen.h"
#include "selection.h"
#include "test.h"

#define LIST_BLOCK 4096u
#define LIST_BLOCKS 8192u
#define LIST_BYTES ((size_t)LIST_BLOCK * LIST_BLOCKS)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* A record owns its bytes, so sorting descriptors never invalidates spans. */
typedef struct list_record {
	unsigned char key[1100];
	unsigned char value[1400];
	uint16_t key_size, value_size;
} list_record_t;

typedef struct list_fixture {
	list_record_t *records;
	size_t count, capacity;
	unsigned char *image;
	uint64_t personality;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_error_t error;
} list_fixture_t;

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
status_is(list_fixture_t *f, fsimg_status_t actual, fsimg_status_t wanted)
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
block(list_fixture_t *f, uint64_t number)
{
	CHECK(number < LIST_BLOCKS);
	return (f->image + number * LIST_BLOCK);
}

static list_record_t *
record_new(list_fixture_t *f)
{
	CHECK(f->count < f->capacity);
	return (&f->records[f->count++]);
}

static void
fixture_init(list_fixture_t *f, size_t capacity, uint64_t personality)
{
	bzero(f, sizeof(*f));
	f->capacity = capacity;
	f->personality = personality;
	f->records = calloc(capacity, sizeof(*f->records));
	f->image = calloc(1, LIST_BYTES);
	CHECK(f->records != NULL && f->image != NULL);
}

static void
fixture_free(list_fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	if (f->ctx != NULL) {
		CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
		expect(stats.scratch_bytes == 0, "listing scratch released");
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
add_inode(list_fixture_t *f, uint64_t oid,
    const apfs_gen_inode_val_t *inode)
{
	list_record_t *r = record_new(f);

	r->key_size = 8;
	r->value_size = (uint16_t)(92 + inode->xfields_len);
	CHECK(apfs_gen_jkey_build(r->key, r->key_size, oid, 3));
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, inode));
}

static void
add_named_inode(list_fixture_t *f, uint64_t oid, uint64_t parent,
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
add_root(list_fixture_t *f)
{
	add_named_inode(f, 2, 1, 4, "root");
}

static void
add_drec(list_fixture_t *f, uint64_t parent, uint64_t child,
    const char *name, uint8_t type, uint32_t hash, uint64_t sibling)
{
	unsigned char raw[16], id[8];
	list_record_t *r = record_new(f);
	apfs_gen_drec_key_t key = { .parent = parent, .name = name,
	    .name_len = (uint16_t)strlen(name), .hash = hash,
	    .hashed = f->personality != 0 };
	apfs_gen_drec_val_t value = { .child = child, .flags = type };
	apfs_gen_xfield_t field = { .type = 1, .data = id, .size = 8 };

	if (sibling != 0) {
		for (unsigned i = 0; i < 8; i++)
			id[i] = (unsigned char)(sibling >> (8 * i));
		value.xfields_len = fields_build(raw, &field, 1);
		value.xfields = raw;
	}
	r->key_size = (uint16_t)((key.hashed ? 12 : 10) +
	    key.name_len + 1);
	r->value_size = (uint16_t)(18 + value.xfields_len);
	CHECK(apfs_gen_drec_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_drec_val_build(r->value, r->value_size, &value));
}

static void
add_xattr(list_fixture_t *f, uint64_t oid, const char *name,
    const unsigned char *bytes, uint16_t size, uint16_t flags)
{
	list_record_t *r = record_new(f);
	apfs_gen_name_key_t key = { .oid = oid, .name = name,
	    .name_len = (uint16_t)strlen(name) };
	apfs_gen_xattr_val_t value = { .data = bytes,
	    .data_len = size, .flags = flags };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = (uint16_t)(4 + size);
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &value));
}

static void
add_stream_xattr(list_fixture_t *f, uint64_t oid, const char *name,
    uint64_t size)
{
	list_record_t *r = record_new(f);
	apfs_gen_name_key_t key = { .oid = oid, .name = name,
	    .name_len = (uint16_t)strlen(name) };
	apfs_gen_xattr_val_t value = { .flags = 1, .data_len = 48,
	    .stream_oid = 100, .dstream = { .size = size } };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = 4 + GEN_XATTR_STREAM_BYTES;
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &value));
}

static void
add_sibling(list_fixture_t *f, uint64_t id, uint64_t parent,
    const char *name)
{
	list_record_t *r = record_new(f);
	apfs_gen_sibling_val_t value = { .parent = parent, .name = name,
	    .name_len = (uint16_t)strlen(name) };

	r->key_size = 16;
	r->value_size = (uint16_t)(11 + value.name_len);
	CHECK(apfs_gen_sibling_key_build(r->key, 16, 16, id));
	CHECK(apfs_gen_sibling_val_build(r->value, r->value_size, &value));
}

/* Sort by the format's owner/kind tuple, then stored suffix, independently
 * of the reader's comparator. Plain keys have a different name offset.
 */
static int
record_compare(const void *left, const void *right)
{
	const list_record_t *a = left, *b = right;
	uint64_t ak = integer(a->key, 8), bk = integer(b->key, 8);
	uint64_t ao = ak & GEN_JKEY_OID_MASK, bo = bk & GEN_JKEY_OID_MASK;
	unsigned kind = (unsigned)(ak >> 60);

	if (ao != bo)
		return (ao < bo ? -1 : 1);
	if (kind != bk >> 60)
		return (kind < bk >> 60 ? -1 : 1);
	if (kind == 5) {
		ak = integer(a->key + 8, 8);
		bk = integer(b->key + 8, 8);
		return (ak < bk ? -1 : ak > bk);
	}
	if (kind == 4)
		return (strcmp((const char *)a->key + 10,
		    (const char *)b->key + 10));
	return (0);
}

static int
plain_compare(const void *left, const void *right)
{
	const list_record_t *a = left, *b = right;
	int order = record_compare(left, right);

	if (order != 0 || integer(a->key, 8) != integer(b->key, 8) ||
	    a->key[7] >> 4 != 9)
		return (order);
	return (strcmp((const char *)a->key + 10,
	    (const char *)b->key + 10));
}

static int
hashed_compare(const void *left, const void *right)
{
	const list_record_t *a = left, *b = right;
	uint64_t ah, bh;
	int order = record_compare(left, right);

	if (order != 0 || integer(a->key, 8) != integer(b->key, 8) ||
	    a->key[7] >> 4 != 9)
		return (order);
	ah = integer(a->key + 8, 4) >> 10;
	bh = integer(b->key + 8, 4) >> 10;
	if (ah != bh)
		return (ah < bh ? -1 : 1);
	return (strcmp((const char *)a->key + 12,
	    (const char *)b->key + 12));
}

/* One checkpoint, one volume, a physical volume map and a virtual tree.
 * Distinct block ranges keep the wide tree and both maps independent.
 */
static void
image_build(list_fixture_t *f)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = LIST_BLOCK, .oid = 100,
		    .address = 32 },
		{ .type = 0x80000011, .size = LIST_BLOCK, .oid = 102,
		    .address = 33 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
	    .mappings = mappings, .mapping_count = 2, .mapping_blocks = 1,
	    .data_len = 4, .data_next = 4, .desc_next = 2 };
	apfs_gen_container_t container = {
		.nx = { .magic = 0x4253584e, .block_size = LIST_BLOCK,
		.block_count = LIST_BLOCKS, .incompat_features = 2,
		.uuid = { 1 }, .desc_base = 1,
		.desc_blocks = 8, .data_base = 32, .data_blocks = 128,
		.max_volumes = 100, .volume_oids = { 2000 },
		.spaceman_oid = 100, .reaper_oid = 102, .omap_oid = 16 },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	apfs_gen_omap_record_t volume = { .key = { .oid = 2000, .xid = 10 },
	    .value = { .size = LIST_BLOCK, .address = 180 } };
	apfs_gen_tree_t map_tree = { .records = &volume, .count = 1,
	    .base = 200, .xid = 10, .block_size = LIST_BLOCK,
	    .records_per_leaf = 16, .children_per_index = 8 };
	apfs_gen_omap_t map = { .object = { .oid = 16, .xid = 10,
	    .type = 0x4000000b }, .flags = 1, .tree_type = 0x40000002 };
	apfs_gen_fs_t fs = { .object = { .oid = 2000, .xid = 10,
	    .type = 13 }, .magic = 0x42535041, .uuid = { 2 },
	    .name = "Listing", .flags = 1, .root_tree_type = 2,
	    .extentref_tree_type = 0x40000002,
	    .snapshot_tree_type = 0x40000002, .omap_oid = 181 };
	apfs_gen_fs_tree_t tree = { .count = f->count, .first_oid = 3000,
	    .base = 1536, .xid = 10, .block_size = LIST_BLOCK,
	    .records_per_leaf = 8, .children_per_index = 8 };
	apfs_gen_record_t *records;
	apfs_gen_omap_record_t *nodes;
	uint64_t root, blocks, map_root, map_blocks;

	CHECK(apfs_gen_container_build(f->image, LIST_BYTES, &container));
	CHECK(apfs_gen_tree_build(f->image, LIST_BYTES, &map_tree,
	    &map.tree_oid, &blocks));
	CHECK(apfs_gen_omap_build(block(f, 16), LIST_BLOCK, &map));
	qsort(f->records, f->count, sizeof(*f->records),
	    f->personality ? hashed_compare : plain_compare);
	records = calloc(f->count, sizeof(*records));
	nodes = calloc(f->count + 1, sizeof(*nodes));
	CHECK(records != NULL && nodes != NULL);
	for (size_t i = 0; i < f->count; i++)
		records[i] = (apfs_gen_record_t){ .key = f->records[i].key,
		    .value = f->records[i].value,
		    .key_size = f->records[i].key_size,
		    .value_size = f->records[i].value_size };
	tree.records = records;
	CHECK(apfs_gen_fs_tree_build(f->image, LIST_BYTES, &tree, nodes,
	    f->count + 1, &root, &blocks));
	map_tree.records = nodes;
	map_tree.count = blocks;
	map_tree.base = 256;
	CHECK(apfs_gen_tree_build(f->image, LIST_BYTES, &map_tree,
	    &map_root, &map_blocks));
	CHECK(256 + map_blocks < 1536);
	map.object.oid = 181;
	map.flags = 0;
	map.tree_oid = map_root;
	CHECK(apfs_gen_omap_build(block(f, 181), LIST_BLOCK, &map));
	fs.root_tree_oid = root;
	fs.incompat_features = f->personality;
	CHECK(apfs_gen_fs_build(block(f, 180), LIST_BLOCK, &fs));
	free(nodes);
	free(records);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *bytes, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	list_fixture_t *f = argument;

	CHECK(offset <= LIST_BYTES && length <= LIST_BYTES - offset);
	bcopy(f->image + offset, bytes, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument FSIMG_UNUSED, uint64_t *size,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*size = LIST_BYTES;
	return (FSIMG_OK);
}

static bool
listing_open(list_fixture_t *f)
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

static fsimg_status_t
listing_drain(list_fixture_t *f, size_t *count)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	*count = 0;
	while ((status = fsimg_reader_next(f->reader, &entry, &f->error)) ==
	    FSIMG_OK)
		(*count)++;
	return (status);
}

static void
expect_listing(list_fixture_t *f, fsimg_status_t wanted, size_t entries)
{
	size_t count;

	if (listing_open(f) && status_is(f, listing_drain(f, &count), wanted)
	    && wanted == FSIMG_DONE)
		expect(count == entries, "exact listing entry count");
	fixture_free(f);
}

static void
expect_time(const fsimg_timestamp_t *time, int64_t seconds, uint32_t ns)
{
	expect(time->seconds == seconds && time->nanoseconds == ns &&
	    time->basis == FSIMG_TIME_UTC && time->precision_ns == 1 &&
	    time->offset_minutes == 0, "signed nanoseconds, floor seconds, UTC");
}

static void
expect_metadata(const fsimg_entry_t *entry)
{
	const fsimg_metadata_t *m = &entry->metadata;
	uint32_t valid = FSIMG_META_OWNER | FSIMG_META_MODE |
	    FSIMG_META_CREATED | FSIMG_META_MODIFIED | FSIMG_META_CHANGED |
	    FSIMG_META_ACCESSED;

	expect((m->valid & valid) == valid, "owner, mode and four times valid");
	expect(m->uid == 12345 && m->gid == 54321, "owner and group preserved");
	expect(m->mode == 06751, "all low twelve mode bits preserved");
	expect_time(&m->created, -2, 999999999);
	expect_time(&m->modified, 1234567890, 123456789);
	expect_time(&m->changed, 987654321, 12345678);
	expect_time(&m->accessed, -1, 999999999);
}

static void
test_inode_types_and_metadata(void)
{
	static const struct {
		const char *name;
		uint8_t type;
		fsimg_entry_kind_t kind;
	} cases[] = {
		{ "regular file", 8, FSIMG_ENTRY_FILE },
		{ "directory", 4, FSIMG_ENTRY_DIRECTORY },
		{ "symbolic link", 10, FSIMG_ENTRY_SYMLINK },
		{ "FIFO", 1, FSIMG_ENTRY_FIFO },
		{ "character device", 2, FSIMG_ENTRY_CHAR_DEVICE },
		{ "block device", 6, FSIMG_ENTRY_BLOCK_DEVICE },
		{ "socket", 12, FSIMG_ENTRY_SOCKET }
	};
	list_fixture_t f;
	const fsimg_entry_t *entry;
	apfs_gen_inode_val_t inode;
	unsigned char raw[1024];
	apfs_gen_xfield_t field = { .type = 4, .size = 6,
	    .data = (const unsigned char *)"child" };
	size_t count;
	char label[96];

	for (size_t i = 0; i < COUNT(cases); i++) {
		for (unsigned mismatch = 0; mismatch < 2; mismatch++) {
			CHECK(snprintf(label, sizeof(label), "%s: %s mode",
			    cases[i].name, mismatch ? "mismatched" : "matching") <
			    (int)sizeof(label));
			scenario = label;
			fixture_init(&f, 8, 0);
			add_root(&f);
			inode = inode_value(2, mismatch ? 0 : cases[i].type);
			inode.xfields_len = fields_build(raw, &field, 1);
			inode.xfields = raw;
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, "child", cases[i].type, 0, 0);
			if (cases[i].type == 10)
				add_xattr(&f, 16, "com.apple.fs.symlink",
				    (const unsigned char *)"x", 2, 6);
			if (listing_open(&f)) {
				if (status_is(&f, fsimg_reader_next(f.reader,
				    &entry, &f.error), mismatch ?
				    FSIMG_ECORRUPT : FSIMG_OK) && !mismatch) {
					expect(entry->kind == cases[i].kind,
					    "mode and record map to entry kind");
					expect(strcmp(entry->path, "/child") == 0,
					    "stored name preserved");
					expect(entry->size == (cases[i].type == 10 ?
					    1u : 0u), "kind's default size");
					expect_metadata(entry);
					status_is(&f, listing_drain(&f, &count),
					    FSIMG_DONE);
					expect(count == 0, "no extra entries");
				}
			}
			fixture_free(&f);
		}
	}
}

static void
test_inode_record_failures(void)
{
	static const struct {
		const char *name;
		uint64_t child, internal;
		uint32_t bsd;
		uint8_t type;
		bool missing;
		fsimg_status_t wanted;
	} cases[] = {
		{ .name = "ordinary inode", .child = 16, .type = 8,
		    .wanted = FSIMG_DONE },
		{ .name = "unknown directory record type", .child = 16,
		    .type = 3, .wanted = FSIMG_ECORRUPT },
		{ .name = "reserved child inode", .child = 15, .type = 8,
		    .wanted = FSIMG_ECORRUPT },
		{ .name = "missing child inode", .child = 16, .type = 8,
		    .missing = true, .wanted = FSIMG_ECORRUPT },
		{ .name = "unknown internal flag", .child = 16, .type = 8,
		    .internal = 0x20, .wanted = FSIMG_EUNSUPPORTED },
		{ .name = "dataless inode", .child = 16, .type = 8,
		    .bsd = 0x40000000, .wanted = FSIMG_EUNSUPPORTED }
	};
	list_fixture_t f;
	apfs_gen_inode_val_t inode;

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 3, 0);
		add_root(&f);
		inode = inode_value(2, 8);
		inode.internal_flags = cases[i].internal;
		inode.bsd_flags = cases[i].bsd;
		if (!cases[i].missing)
			add_inode(&f, cases[i].child, &inode);
		add_drec(&f, 2, cases[i].child, "file", cases[i].type, 0, 0);
		expect_listing(&f, cases[i].wanted, 1);
	}
}

static void
test_known_extended_field_sizes_and_unknown_fields(void)
{
	static const struct { uint8_t type; uint16_t size; } known[] = {
		{ 1, 8 }, { 2, 8 }, { 3, 4 }, { 5, 8 }, { 7, 32 },
		{ 8, 40 }, { 10, 32 }, { 11, 16 }, { 13, 8 },
		{ 14, 4 }, { 16, 8 }
	};
	unsigned char data[40] = { 0 }, raw[1024];
	apfs_gen_xfield_t field;
	apfs_gen_inode_val_t inode;
	list_fixture_t f;
	char label[96];

	for (size_t i = 0; i < COUNT(known); i++) {
		for (unsigned wrong = 0; wrong < 2; wrong++) {
			CHECK(snprintf(label, sizeof(label),
			    "inode field %u: %s size", known[i].type,
			    wrong ? "wrong" : "valid") < (int)sizeof(label));
			scenario = label;
			fixture_init(&f, 3, 0);
			add_root(&f);
			field = (apfs_gen_xfield_t){ .type = known[i].type,
			    .size = (uint16_t)(known[i].size - wrong),
			    .data = data };
			inode = inode_value(2, known[i].type == 14 ? 2 : 8);
			inode.xfields_len = fields_build(raw, &field, 1);
			inode.xfields = raw;
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, "file",
			    known[i].type == 14 ? 2 : 8, 0, 0);
			expect_listing(&f, wrong ? FSIMG_ECORRUPT :
			    FSIMG_DONE, 1);
		}
	}
	for (unsigned dependent = 0; dependent < 2; dependent++) {
		scenario = dependent ? "unknown data-dependent inode field" :
		    "unknown independent inode field ignored";
		fixture_init(&f, 3, 0);
		add_root(&f);
		field = (apfs_gen_xfield_t){ .type = 200, .size = 3,
		    .flags = (uint8_t)dependent, .data = data };
		inode = inode_value(2, 8);
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		expect_listing(&f, dependent ? FSIMG_EUNSUPPORTED : FSIMG_DONE, 1);
	}
}

static void
test_data_stream_sizes(void)
{
	static const uint64_t sizes[] = { 0, 123, UINT64_C(0x100000123),
		UINT64_MAX };
	list_fixture_t f;
	unsigned char stream[40], raw[1024];
	apfs_gen_xfield_t field = { .type = 8, .size = 40, .data = stream };
	apfs_gen_inode_val_t inode;
	const fsimg_entry_t *entry;

	for (size_t i = 0; i < COUNT(sizes); i++) {
		scenario = "data stream logical size including beyond four GiB";
		fixture_init(&f, 3, 0);
		add_root(&f);
		CHECK(apfs_gen_dstream_build(stream, sizeof(stream),
		    &(apfs_gen_dstream_t){ .size = sizes[i] }));
		inode = inode_value(2, 8);
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		if (listing_open(&f) && status_is(&f,
		    fsimg_reader_next(f.reader, &entry, &f.error), sizes[i] ==
		    UINT64_MAX ? FSIMG_ECORRUPT : FSIMG_OK) && sizes[i] != UINT64_MAX) {
			expect(entry->size == sizes[i], "64-bit logical size");
			expect_metadata(entry);
			status_is(&f, fsimg_reader_next(f.reader, &entry, &f.error),
			    FSIMG_DONE);
		}
		fixture_free(&f);
	}
}

/* Normalization forms are supplied as literal scalars; the independent
 * CRC oracle must never call the driver's Unicode or hashing functions.
 */
static uint32_t
ascii_hash(const char *name, bool fold)
{
	uint32_t scalars[128];
	size_t length = strlen(name);

	CHECK(length <= COUNT(scalars));
	for (size_t i = 0; i < length; i++) {
		scalars[i] = (unsigned char)name[i];
		if (fold && scalars[i] >= 'A' && scalars[i] <= 'Z')
			scalars[i] += 'a' - 'A';
	}
	return (apfs_gen_name_hash(scalars, length));
}

static void
test_invalid_names_and_hash_mismatch_classes(void)
{
	static const struct {
		const char *name, *stored;
		fsimg_status_t wanted;
	} cases[] = {
		{ "empty name", "", FSIMG_ECORRUPT },
		{ "dot name", ".", FSIMG_ECORRUPT },
		{ "dot dot name", "..", FSIMG_ECORRUPT },
		{ "invalid UTF-8", "\xc0\xaf", FSIMG_ECORRUPT },
		{ "slash in name", "a/b", FSIMG_ECORRUPT },
		{ "assigned name hash mismatch", "hello", FSIMG_ECORRUPT },
		{ "unassigned U+0378 hash mismatch", "\xcd\xb8",
		    FSIMG_EUNSUPPORTED },
		{ "noncharacter U+FFFF hash mismatch", "\xef\xbf\xbf",
		    FSIMG_EUNSUPPORTED }
	};
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);

	for (unsigned personality = 0; personality < 3; personality++) {
		for (size_t i = 0; i < COUNT(cases); i++) {
			if (personality == 0 && i >= 5)
				continue;
			scenario = cases[i].name;
			fixture_init(&f, 3, personality == 2 ? 8 : personality);
			add_root(&f);
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, cases[i].stored, 8, 0, 0);
			expect_listing(&f, cases[i].wanted, 1);
		}
	}
	/* Each hash mismatch has the same name paired with its correct hash. */
	for (unsigned personality = 1; personality <= 8; personality += 7) {
		static const uint32_t unassigned[] = { 0x378 };
		static const uint32_t noncharacter[] = { 0xffff };
		for (size_t i = 5; i < COUNT(cases); i++) {
			uint32_t hash = i == 5 ? ascii_hash("hello", true) :
			    apfs_gen_name_hash(i == 6 ? unassigned : noncharacter, 1);

			scenario = cases[i].name;
			fixture_init(&f, 3, personality);
			add_root(&f);
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, cases[i].stored, 8, hash, 0);
			expect_listing(&f, FSIMG_DONE, 1);
		}
	}
}

static void
test_equivalent_names_collide_only_in_their_personality(void)
{
	static const char *pairs[][2] = { { "a", "A" },
		{ "\xc3\xa9", "e\xcc\x81" } };
	static const uint32_t decomposed[] = { 'e', 0x301 };
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned seen, count;
	char label[96];

	for (size_t pair = 0; pair < COUNT(pairs); pair++) {
		for (unsigned personality = 0; personality < 3; personality++) {
			uint64_t rule = personality == 2 ? 8 : personality;
			bool collision = rule != 0 && (pair != 0 || rule == 1);

			CHECK(snprintf(label, sizeof(label), "%s equivalent names, rule %u",
			    pair == 0 ? "case" : "normalization", (unsigned)rule) <
			    (int)sizeof(label));
			scenario = label;
			fixture_init(&f, 5, rule);
			add_root(&f);
			for (unsigned j = 0; j < 2; j++) {
				uint32_t hash = pair == 0 ?
				    ascii_hash(pairs[pair][j], rule == 1) :
				    apfs_gen_name_hash(decomposed, 2);

				add_inode(&f, 16 + j, &inode);
				add_drec(&f, 2, 16 + j, pairs[pair][j], 8, hash, 0);
			}
			seen = count = 0;
			if (listing_open(&f)) {
				while ((status = fsimg_reader_next(f.reader, &entry,
				    &f.error)) == FSIMG_OK) {
					for (unsigned j = 0; j < 2; j++)
						if (entry->path[0] == '/' &&
						    strcmp(entry->path + 1, pairs[pair][j]) == 0)
							seen |= 1u << j;
					count++;
				}
				status_is(&f, status, collision ? FSIMG_ECORRUPT : FSIMG_DONE);
				if (!collision)
					expect(count == 2 && seen == 3,
					    "both distinct stored spellings list exactly once");
				else
					expect(count == 0,
					    "ambiguous namespace exposes no child");
			}
			fixture_free(&f);
		}
	}
}

static void
test_case_insensitive_selection_returns_stored_spelling(void)
{
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	const char *query = "/README";

	scenario = "case insensitive selection preserves readme spelling";
	fixture_init(&f, 3, 1);
	add_root(&f);
	add_inode(&f, 16, &inode);
	add_drec(&f, 2, 16, "readme", 8, ascii_hash("readme", true), 0);
	if (listing_open(&f)) {
		CHECK(fsimg_selection_new(f.reader, &query, 1,
		    FSIMG_SELECTION_FILE, &selection, &f.error) == FSIMG_OK);
		if (status_is(&f, fsimg_selection_next(selection, &entry,
		    &f.error), FSIMG_OK))
			expect(strcmp(entry->path, "/readme") == 0,
			    "selection delivers stored spelling");
		status_is(&f, fsimg_selection_next(selection, &entry, &f.error),
		    FSIMG_DONE);
		fsimg_selection_free(selection);
	}
	fixture_free(&f);
}

static void
test_hard_links_and_extra_data_on_every_name(void)
{
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned files, directories, paths;
	unsigned char stream[40], raw[1024];
	apfs_gen_xfield_t field = { .type = 8, .size = 40, .data = stream };

	for (unsigned extra = 0; extra < 2; extra++) {
		scenario = extra ? "hard links share metadata and extra-data bit" :
		    "hard links in different directories share metadata";
		fixture_init(&f, 12, 0);
		add_root(&f);
		inode.count = 2;
		CHECK(apfs_gen_dstream_build(stream, sizeof(stream),
		    &(apfs_gen_dstream_t){ .size = UINT64_C(0x100000123) }));
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_named_inode(&f, 17, 2, 4, "dir");
		add_drec(&f, 2, 17, "dir", 4, 0, 0);
		add_drec(&f, 2, 16, "first", 8, 0, 100);
		add_drec(&f, 17, 16, "second", 8, 0, 101);
		add_sibling(&f, 100, 2, "first");
		add_sibling(&f, 101, 17, "second");
		if (extra)
			add_xattr(&f, 16, "user.note",
			    (const unsigned char *)"x", 1, 2);
		files = directories = paths = 0;
		if (listing_open(&f)) {
			while ((status = fsimg_reader_next(f.reader, &entry,
			    &f.error)) == FSIMG_OK) {
				if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
					directories++;
					continue;
				}
				files++;
				if (strcmp(entry->path, "/first") == 0)
					paths |= 1;
				if (strcmp(entry->path, "/dir/second") == 0)
					paths |= 2;
				expect_metadata(entry);
				expect(entry->size == UINT64_C(0x100000123),
				    "both hard-link names have the inode's stream size");
				expect(((entry->metadata.valid &
				    FSIMG_META_EXTRA_DATA) != 0) == (extra != 0),
				    "each name has inode's extra-data bit");
			}
			status_is(&f, status, FSIMG_DONE);
			expect(files == 2 && directories == 1 && paths == 3,
			    "both hard-link paths and their directory list");
		}
		fixture_free(&f);
	}
}

static void
test_directory_parent_and_name_prevent_sharing(void)
{
	static const struct {
		const char *name, *inode_name;
		uint64_t parent;
		bool second_name;
		fsimg_status_t wanted;
	} cases[] = {
		{ "single directory name", "dir", 2, false, FSIMG_DONE },
		{ "directory second name", "dir", 2, true, FSIMG_ECORRUPT },
		{ "directory wrong parent", "dir", 99, false, FSIMG_ECORRUPT },
		{ "directory wrong name field", "other", 2, false, FSIMG_ECORRUPT }
	};
	list_fixture_t f;

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 4, 0);
		add_root(&f);
		add_named_inode(&f, 16, cases[i].parent, 4, cases[i].inode_name);
		add_drec(&f, 2, 16, "dir", 4, 0, 0);
		if (cases[i].second_name)
			add_drec(&f, 2, 16, "other", 4, 0, 0);
		expect_listing(&f, cases[i].wanted, 1);
	}
}

static void
test_symlink_target_boundaries_and_delivery(void)
{
	static const struct {
		const char *name;
		uint16_t size, flags;
		bool missing, unterminated, interior;
		fsimg_status_t wanted;
	} cases[] = {
		{ .name = "one byte target", .size = 2, .flags = 6,
		    .wanted = FSIMG_OK },
		{ .name = "1023 byte target", .size = 1024, .flags = 6,
		    .wanted = FSIMG_OK },
		{ .name = "1024 byte target", .size = 1025, .flags = 6,
		    .wanted = FSIMG_EUNSUPPORTED },
		{ .name = "empty target", .size = 1, .flags = 6,
		    .wanted = FSIMG_ECORRUPT },
		{ .name = "unterminated target", .size = 3, .flags = 6,
		    .unterminated = true, .wanted = FSIMG_ECORRUPT },
		{ .name = "interior NUL target", .size = 4, .flags = 6,
		    .interior = true, .wanted = FSIMG_ECORRUPT },
		{ .name = "missing target", .missing = true,
		    .wanted = FSIMG_ECORRUPT },
		{ .name = "target not filesystem owned", .size = 2, .flags = 2,
		    .wanted = FSIMG_ECORRUPT }
	};
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 10);
	const fsimg_entry_t *entry;
	unsigned char target[1025], delivered[1025];
	size_t count;

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 4, 0);
		add_root(&f);
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "link", 10, 0, 0);
		memset(target, 'x', sizeof(target));
		if (!cases[i].missing) {
			if (!cases[i].unterminated)
				target[cases[i].size - 1] = 0;
			if (cases[i].interior)
				target[1] = 0;
			add_xattr(&f, 16, "com.apple.fs.symlink", target,
			    cases[i].size, cases[i].flags);
		}
		if (listing_open(&f) && status_is(&f,
		    fsimg_reader_next(f.reader, &entry, &f.error), cases[i].wanted)
		    && cases[i].wanted == FSIMG_OK) {
			expect(entry->kind == FSIMG_ENTRY_SYMLINK &&
			    entry->size == cases[i].size - 1u, "target length is size");
			expect((entry->metadata.valid & FSIMG_META_EXTRA_DATA) == 0,
			    "consumed target is not extra data");
			if (status_is(&f, fsimg_reader_read_at(f.reader, 0,
			    delivered, sizeof(delivered), &count, &f.error), FSIMG_OK))
				expect(count == cases[i].size - 1u &&
				    bcmp(delivered, target, count) == 0,
				    "delivers exact target bytes without NUL");
			status_is(&f, fsimg_reader_next(f.reader, &entry, &f.error),
			    FSIMG_DONE);
		}
		fixture_free(&f);
	}
}

static void
test_device_numbers_are_optional_and_only_on_devices(void)
{
	static const uint8_t types[] = { 1, 12, 2, 6, 8 };
	unsigned char device[] = { 0x56, 0x34, 0x12, 0xab }, raw[1024];
	apfs_gen_xfield_t field = { .type = 14, .size = 4, .data = device };
	apfs_gen_inode_val_t inode;
	const fsimg_entry_t *entry;
	list_fixture_t f;
	char label[96];

	for (size_t i = 0; i < COUNT(types); i++) {
		for (unsigned present = 0; present < 2; present++) {
			bool device_kind = types[i] == 2 || types[i] == 6;

			CHECK(snprintf(label, sizeof(label),
			    "node type %u, device field %s", types[i],
			    present ? "present" : "absent") < (int)sizeof(label));
			scenario = label;
			fixture_init(&f, 3, 0);
			add_root(&f);
			inode = inode_value(2, types[i]);
			if (present) {
				inode.xfields_len = fields_build(raw, &field, 1);
				inode.xfields = raw;
			}
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, "node", types[i], 0, 0);
			if (listing_open(&f) && status_is(&f,
			    fsimg_reader_next(f.reader, &entry, &f.error),
			    present && !device_kind ? FSIMG_ECORRUPT : FSIMG_OK)
			    && (!present || device_kind)) {
				expect(((entry->metadata.valid & FSIMG_META_DEVICE) != 0)
				    == (present != 0), "device metadata validity");
				if (present)
					expect(entry->metadata.dev_major == 0xab &&
					    entry->metadata.dev_minor == 0x123456,
					    "device major high byte and minor low 24 bits");
				status_is(&f, fsimg_reader_next(f.reader, &entry,
				    &f.error), FSIMG_DONE);
			}
			fixture_free(&f);
		}
	}
}

static void
test_compression_attribute_consumption_and_unknown_flags(void)
{
	unsigned char header[] = { 'f', 'p', 'm', 'c', 4, 0, 0, 0,
		0x23, 1, 0, 0, 1, 0, 0, 0 };
	list_fixture_t f;
	apfs_gen_inode_val_t inode;
	const fsimg_entry_t *entry;

	for (unsigned compressed = 0; compressed < 2; compressed++) {
		for (unsigned unknown = 0; unknown < 2; unknown++) {
			scenario = "compression attribute consumption and flag validation";
			fixture_init(&f, 5, 0);
			add_root(&f);
			inode = inode_value(2, 8);
			inode.bsd_flags = compressed ? 0x20 : 0;
			add_inode(&f, 16, &inode);
			add_drec(&f, 2, 16, "file", 8, 0, 0);
			add_xattr(&f, 16, "com.apple.decmpfs", header,
			    sizeof(header), unknown ? 0x22 : 2);
			add_stream_xattr(&f, 16, "com.apple.ResourceFork", 1);
			if (listing_open(&f) && status_is(&f,
			    fsimg_reader_next(f.reader, &entry, &f.error), unknown ?
			    FSIMG_EUNSUPPORTED : FSIMG_OK) && !unknown) {
				expect(entry->size == (compressed ?
				    UINT64_C(0x100000123) : 0),
				    "compressed file uses header's 64-bit size");
				expect(((entry->metadata.valid & FSIMG_META_EXTRA_DATA)
				    != 0) == !compressed, "attribute consumed only by compression");
				status_is(&f, fsimg_reader_next(f.reader, &entry,
				    &f.error), FSIMG_DONE);
			}
			fixture_free(&f);
		}
	}
}

static void
test_regular_inode_name_encoding(void)
{
	static const struct {
		const char *name, *bytes;
		uint16_t size;
		fsimg_status_t wanted;
	} cases[] = {
		{ "regular inode UTF-8 name", "name", 5, FSIMG_DONE },
		{ "regular inode invalid UTF-8 name", "\xc0\xaf", 3,
		    FSIMG_ECORRUPT },
		{ "regular inode interior NUL name", "a\0b", 4,
		    FSIMG_ECORRUPT }
	};
	list_fixture_t f;
	apfs_gen_inode_val_t inode;
	apfs_gen_xfield_t field;
	unsigned char raw[1024];

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 3, 0);
		add_root(&f);
		inode = inode_value(2, 8);
		field = (apfs_gen_xfield_t){ .type = 4, .flags = 2,
		    .data = (const unsigned char *)cases[i].bytes,
		    .size = cases[i].size };
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "name", 8, 0, 0);
		expect_listing(&f, cases[i].wanted, 1);
	}
}

static void
test_nested_directory_parent(void)
{
	list_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned paths;

	for (unsigned grandparent = 0; grandparent < 2; grandparent++) {
		scenario = grandparent ? "nested directory names grandparent" :
		    "nested directory names parent";
		fixture_init(&f, 5, 0);
		add_root(&f);
		add_named_inode(&f, 16, 2, 4, "dir");
		add_drec(&f, 2, 16, "dir", 4, 0, 0);
		add_named_inode(&f, 17, grandparent ? 2 : 16, 4, "nested");
		add_drec(&f, 16, 17, "nested", 4, 0, 0);
		paths = 0;
		if (listing_open(&f)) {
			while ((status = fsimg_reader_next(f.reader, &entry,
			    &f.error)) == FSIMG_OK) {
				expect(entry->kind == FSIMG_ENTRY_DIRECTORY,
				    "nested entries are directories");
				if (strcmp(entry->path, "/dir") == 0) {
					expect((paths & 1) == 0, "parent listed once");
					paths |= 1;
				} else if (strcmp(entry->path, "/dir/nested") == 0) {
					expect((paths & 2) == 0, "nested directory listed once");
					paths |= 2;
				} else {
					expect(false, "expected nested directory path");
				}
			}
			status_is(&f, status, grandparent ? FSIMG_ECORRUPT :
			    FSIMG_DONE);
			expect(paths == (grandparent ? 1u : 3u),
			    "only directories with matching parents list");
		}
		fixture_free(&f);
	}
}

static void
test_dataless_directory(void)
{
	list_fixture_t f;
	apfs_gen_inode_val_t inode;
	apfs_gen_xfield_t field = { .type = 4, .flags = 2, .size = 4,
	    .data = (const unsigned char *)"dir" };
	unsigned char raw[1024];

	for (unsigned dataless = 0; dataless < 2; dataless++) {
		scenario = dataless ? "dataless directory" : "ordinary directory";
		fixture_init(&f, 3, 0);
		add_root(&f);
		inode = inode_value(2, 4);
		inode.bsd_flags = dataless ? 0x40000000 : 0;
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "dir", 4, 0, 0);
		expect_listing(&f, dataless ? FSIMG_EUNSUPPORTED : FSIMG_DONE, 1);
	}
}

static void
test_inline_compression_size_bound(void)
{
	unsigned char header[] = { 'f', 'p', 'm', 'c', 3, 0, 0, 0,
		0, 0, 1, 0, 0, 0, 0, 0 };
	list_fixture_t f;
	apfs_gen_inode_val_t inode;
	const fsimg_entry_t *entry;

	for (unsigned oversized = 0; oversized < 2; oversized++) {
		scenario = oversized ? "inline compression size 65537" :
		    "inline compression size 65536";
		fixture_init(&f, 4, 0);
		add_root(&f);
		inode = inode_value(2, 8);
		inode.bsd_flags = 0x20;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		header[8] = (unsigned char)oversized;
		add_xattr(&f, 16, "com.apple.decmpfs", header, sizeof(header), 2);
		if (listing_open(&f) && status_is(&f,
		    fsimg_reader_next(f.reader, &entry, &f.error), oversized ?
		    FSIMG_EUNSUPPORTED : FSIMG_OK) && !oversized) {
			expect(entry->size == 65536, "inline size bound lists exactly");
			status_is(&f, fsimg_reader_next(f.reader, &entry, &f.error),
			    FSIMG_DONE);
		}
		fixture_free(&f);
	}
}

static void
test_compression_listing_branches(void)
{
	static const struct {
		const char *name;
		uint8_t type;
		uint16_t header_size;
		bool missing_header, streamed_header, inode_size;
		uint64_t uncompressed_size, data_size;
		bool fork, streamed_fork;
		uint16_t fork_size;
		bool extra;
		fsimg_status_t wanted;
	} cases[] = {
		{ .name = "inline compression header present", .type = 3,
		    .header_size = 16, .wanted = FSIMG_OK },
		{ .name = "compressed file missing decmpfs", .missing_header = true,
		    .wanted = FSIMG_ECORRUPT },
		{ .name = "compressed inode size matches header", .type = 3,
		    .header_size = 16, .inode_size = true, .uncompressed_size = 123,
		    .wanted = FSIMG_OK },
		{ .name = "compressed inode size differs from header", .type = 3,
		    .header_size = 16, .inode_size = true, .uncompressed_size = 124,
		    .wanted = FSIMG_ECORRUPT },
		{ .name = "compressed file empty data stream", .type = 3,
		    .header_size = 16, .wanted = FSIMG_OK },
		{ .name = "compressed file nonempty data stream", .type = 3,
		    .header_size = 16, .data_size = 1, .wanted = FSIMG_ECORRUPT },
		{ .name = "resource compression 16 byte header and streamed fork",
		    .type = 4, .header_size = 16, .fork = true,
		    .streamed_fork = true, .fork_size = 1, .wanted = FSIMG_OK },
		{ .name = "resource compression 17 byte header", .type = 4,
		    .header_size = 17, .fork = true, .streamed_fork = true,
		    .fork_size = 1, .wanted = FSIMG_ECORRUPT },
		{ .name = "resource compression missing fork", .type = 4,
		    .header_size = 16, .wanted = FSIMG_ECORRUPT },
		{ .name = "resource compression inline fork", .type = 4,
		    .header_size = 16, .fork = true, .fork_size = 1,
		    .wanted = FSIMG_EUNSUPPORTED },
		{ .name = "inline compression empty inline fork consumed", .type = 3,
		    .header_size = 16, .fork = true, .wanted = FSIMG_OK },
		{ .name = "inline compression nonempty inline fork is extra", .type = 3,
		    .header_size = 16, .fork = true, .fork_size = 1, .extra = true,
		    .wanted = FSIMG_OK },
		{ .name = "inline compression empty streamed fork consumed", .type = 3,
		    .header_size = 16, .fork = true, .streamed_fork = true,
		    .wanted = FSIMG_OK },
		{ .name = "inline compression nonempty streamed fork is extra",
		    .type = 3, .header_size = 16, .fork = true,
		    .streamed_fork = true, .fork_size = 1, .extra = true,
		    .wanted = FSIMG_OK },
		{ .name = "streamed decmpfs with inode uncompressed size",
		    .type = 3, .streamed_header = true, .inode_size = true,
		    .uncompressed_size = 123, .wanted = FSIMG_OK },
		{ .name = "streamed decmpfs without inode uncompressed size",
		    .type = 3, .streamed_header = true, .wanted = FSIMG_OK },
		{ .name = "malformed streamed decmpfs header",
		    .streamed_header = true, .wanted = FSIMG_ECORRUPT }
	};
	unsigned char header[] = { 'f', 'p', 'm', 'c', 0, 0, 0, 0,
		123, 0, 0, 0, 0, 0, 0, 0, 0 };
	unsigned char stream[40], raw[1024], fork[1] = { 0 };
	apfs_gen_xfield_t field = { .type = 8, .size = 40, .data = stream };
	apfs_gen_inode_val_t inode;
	list_fixture_t f;
	const fsimg_entry_t *entry;
	uint64_t size;

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 6, 0);
		add_root(&f);
		inode = inode_value(2, 8);
		inode.bsd_flags = 0x20;
		inode.internal_flags = cases[i].inode_size ? 0x40000 : 0;
		inode.uncompressed_size = cases[i].uncompressed_size;
		CHECK(apfs_gen_dstream_build(stream, sizeof(stream),
		    &(apfs_gen_dstream_t){ .size = cases[i].data_size }));
		inode.xfields_len = fields_build(raw, &field, 1);
		inode.xfields = raw;
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		header[4] = cases[i].type;
		if (cases[i].streamed_header) {
			list_record_t *extent;

			add_stream_xattr(&f, 16, "com.apple.decmpfs", 16);
			extent = record_new(&f);
			extent->key_size = 16;
			extent->value_size = 24;
			CHECK(apfs_gen_extent_key_build(extent->key, 16, 100, 0));
			CHECK(apfs_gen_extent_val_build(extent->value, 24,
			    &(apfs_gen_extent_val_t){ .length = LIST_BLOCK,
			    .phys_block = 4000 }));
			apfs_gen_decmpfs_header(block(&f, 4000), cases[i].type, 123);
			if (cases[i].wanted == FSIMG_ECORRUPT)
				block(&f, 4000)[0] = 0;
		} else if (!cases[i].missing_header)
			add_xattr(&f, 16, "com.apple.decmpfs", header,
			    cases[i].header_size, 2);
		if (cases[i].fork) {
			if (cases[i].streamed_fork)
				add_stream_xattr(&f, 16, "com.apple.ResourceFork",
				    cases[i].fork_size);
			else
				add_xattr(&f, 16, "com.apple.ResourceFork", fork,
				    cases[i].fork_size, 2);
		}
		if (listing_open(&f) && status_is(&f,
		    fsimg_reader_next(f.reader, &entry, &f.error), cases[i].wanted)
		    && cases[i].wanted == FSIMG_OK) {
			size = 123;
			expect(entry->size == size, "compression listing logical size");
			expect(((entry->metadata.valid & FSIMG_META_EXTRA_DATA) != 0)
			    == cases[i].extra, "compression attribute consumption");
			status_is(&f, fsimg_reader_next(f.reader, &entry, &f.error),
			    FSIMG_DONE);
		}
		fixture_free(&f);
	}
}

static void
test_wide_directory_visits_every_child_once(void)
{
	const unsigned children = 4096;
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	bool seen[4096];
	char name[32], wanted[32];
	unsigned index, count;

	for (unsigned missing = 0; missing < 2; missing++) {
		scenario = missing ? "wide directory missing its last child's inode" :
		    "4096 children across multiple index and leaf levels";
		fixture_init(&f, 2 * children + 1, 0);
		add_root(&f);
		for (unsigned i = 0; i < children; i++) {
			CHECK(snprintf(name, sizeof(name), "file-%04u", i) <
			    (int)sizeof(name));
			if (!missing || i + 1 != children)
				add_inode(&f, 16 + i, &inode);
			add_drec(&f, 2, 16 + i, name, 8, 0, 0);
		}
		bzero(seen, sizeof(seen));
		count = 0;
		if (listing_open(&f)) {
			while ((status = fsimg_reader_next(f.reader, &entry,
			    &f.error)) == FSIMG_OK) {
				CHECK(sscanf(entry->path, "/file-%u", &index) == 1);
				CHECK(index < children);
				CHECK(snprintf(wanted, sizeof(wanted), "/file-%04u",
				    index) < (int)sizeof(wanted));
				expect(strcmp(entry->path, wanted) == 0,
				    "wide listing preserves exact child name");
				expect(!seen[index], "child visited only once");
				seen[index] = true;
				count++;
			}
			status_is(&f, status, missing ? FSIMG_ECORRUPT : FSIMG_DONE);
			expect(count == children - missing,
			    "every intact child returned before the missing inode");
			for (unsigned i = 0; i < children - missing; i++)
				expect(seen[i], "each expected child returned");
			if (missing) {
				expect(!seen[children - 1], "missing child is not returned");
			}
		}
		fixture_free(&f);
	}
}

/* Each inode owns one expected spelling. Check the exact returned path set
 * as well as count, so duplicated or substituted entries cannot pass.
 */
static void
check_hash_batch(list_fixture_t *f, char names[][1024], unsigned rows,
    const char *path, unsigned batch)
{
	char label[4300];
	bool seen[128] = { false };
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	size_t count = 0;

	CHECK(snprintf(label, sizeof(label), "%s batch %u", path, batch) <
	    (int)sizeof(label));
	scenario = label;
	if (listing_open(f)) {
		while ((status = fsimg_reader_next(f->reader, &entry,
		    &f->error)) == FSIMG_OK) {
			bool found = false;

			for (unsigned i = 0; i < rows; i++) {
				if (entry->path[0] == '/' &&
				    strcmp(entry->path + 1, names[i]) == 0) {
					expect(!seen[i], "captured spelling returned once");
					seen[i] = true;
					found = true;
					break;
				}
			}
			expect(found, "listed name belongs to captured vector batch");
			count++;
		}
		status_is(f, status, FSIMG_DONE);
		expect(count == rows, "all captured hashes list");
		for (unsigned i = 0; i < rows; i++)
			expect(seen[i], "each captured spelling returned");
	}
	fixture_free(f);
}

/* Replay captured hashes in bounded batches, preserving both personalities. */
static void
test_hash_vectors_file(const char *path)
{
	FILE *file;
	char line[4096], hex[2048], personality[3], names[128][1024];
	unsigned length, hash, byte, rows = 0, batches = 0, total = 0;
	uint64_t rule;
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);

	file = fopen(path, "rb");
	CHECK(file != NULL);
	while (fgets(line, sizeof(line), file) != NULL) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		CHECK(sscanf(line, "%2047s %2s %u %u", hex, personality,
		    &length, &hash) == 4);
		CHECK(strlen(hex) % 2 == 0 && length == strlen(hex) / 2 + 1);
		CHECK(length <= sizeof(names[0]) && hash < (1u << 22));
		rule = strcmp(personality, "ci") == 0 ? 1 : 8;
		CHECK(strcmp(personality, "ci") == 0 ||
		    strcmp(personality, "cs") == 0);
		if (rows != 0 && (f.personality != rule || rows == 128)) {
			check_hash_batch(&f, names, rows, path, ++batches);
			rows = 0;
		}
		if (rows == 0) {
			fixture_init(&f, 257, rule);
			add_root(&f);
		}
		for (unsigned i = 0; i + 1 < length; i++) {
			CHECK(sscanf(hex + 2 * i, "%2x", &byte) == 1);
			names[rows][i] = (char)byte;
		}
		names[rows][length - 1] = '\0';
		CHECK(strlen(names[rows]) == length - 1);
		add_inode(&f, 16 + rows, &inode);
		add_drec(&f, 2, 16 + rows, names[rows], 8, hash, 0);
		rows++;
		total++;
	}
	CHECK(!ferror(file));
	CHECK(fclose(file) == 0);
	if (rows != 0)
		check_hash_batch(&f, names, rows, path, ++batches);
	scenario = path;
	expect(total != 0, "hash vector file contains records");
	printf("APFS captured hash vectors: %s, %u names, %u batches\n",
	    path, total, batches);
}

static void
test_inode_fixed_part_cannot_be_truncated(void)
{
	list_fixture_t f;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	char label[96];

	for (unsigned truncated = 0; truncated < 2; truncated++) {
		CHECK(snprintf(label, sizeof(label), "inode fixed part: %u bytes",
		    92 - truncated) < (int)sizeof(label));
		scenario = label;
		fixture_init(&f, 3, 0);
		add_root(&f);
		add_inode(&f, 16, &inode);
		f.records[f.count - 1].value_size -= truncated;
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		expect_listing(&f, truncated ? FSIMG_ECORRUPT : FSIMG_DONE, 1);
	}
}

static void
test_directory_record_extended_fields(void)
{
	static const struct {
		const char *name;
		uint8_t type, flags;
		uint16_t size;
		fsimg_status_t wanted;
	} cases[] = {
		{ "eight byte sibling ID", 1, 0, 8, FSIMG_DONE },
		{ "short sibling ID", 1, 0, 7, FSIMG_ECORRUPT },
		{ "unknown independent record field", 200, 0, 8, FSIMG_DONE },
		{ "unknown dependent record field", 200, 1, 8,
		    FSIMG_EUNSUPPORTED }
	};
	list_fixture_t f;
	list_record_t *record;
	apfs_gen_inode_val_t inode = inode_value(2, 8);
	apfs_gen_drec_val_t value;
	apfs_gen_xfield_t field;
	unsigned char id[8] = { 100 }, raw[1024];

	for (size_t i = 0; i < COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 4, 0);
		add_root(&f);
		add_inode(&f, 16, &inode);
		add_drec(&f, 2, 16, "file", 8, 0, 0);
		record = &f.records[f.count - 1];
		field = (apfs_gen_xfield_t){ .type = cases[i].type,
		    .flags = cases[i].flags, .size = cases[i].size, .data = id };
		value = (apfs_gen_drec_val_t){ .child = 16, .flags = 8,
		    .xfields = raw };
		value.xfields_len = fields_build(raw, &field, 1);
		record->value_size = (uint16_t)(18 + value.xfields_len);
		CHECK(apfs_gen_drec_val_build(record->value,
		    record->value_size, &value));
		if (cases[i].type == 1)
			add_sibling(&f, 100, 2, "file");
		expect_listing(&f, cases[i].wanted, 1);
	}
}

int
main(void)
{
	const char *native = getenv("FSIMG_APFS_NATIVE");
	char path[4096];
	int length;

	test_inode_types_and_metadata();
	test_regular_inode_name_encoding();
	test_inode_fixed_part_cannot_be_truncated();
	test_directory_record_extended_fields();
	test_inode_record_failures();
	test_known_extended_field_sizes_and_unknown_fields();
	test_data_stream_sizes();
	test_invalid_names_and_hash_mismatch_classes();
	test_equivalent_names_collide_only_in_their_personality();
	test_case_insensitive_selection_returns_stored_spelling();
	test_hard_links_and_extra_data_on_every_name();
	test_directory_parent_and_name_prevent_sharing();
	test_nested_directory_parent();
	test_dataless_directory();
	test_symlink_target_boundaries_and_delivery();
	test_device_numbers_are_optional_and_only_on_devices();
	test_compression_attribute_consumption_and_unknown_flags();
	test_inline_compression_size_bound();
	test_compression_listing_branches();
	test_wide_directory_visits_every_child_once();
	test_hash_vectors_file(APFS_NAME_HASHES);
	if (native == NULL) {
		printf("SKIP: FSIMG_APFS_NATIVE unset; E4 vectors not checked\n");
	} else {
		length = snprintf(path, sizeof(path), "%s/e4-vectors.txt", native);
		CHECK(length >= 0 && (size_t)length < sizeof(path));
		test_hash_vectors_file(path);
	}
	printf("APFS listing: %u checks, %u failures\n", checks, failures);
	return (failures == 0 ? 0 : 1);
}
