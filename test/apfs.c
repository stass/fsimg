/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <glob.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "apfs.h"
#include "apfs-gen.h"
#include "format.h"
#include "fsimg-endian.h"
#include "reader.h"
#include "test.h"

#define BLOCK 4096u
#define BLOCKS 1024u
#define IMAGE_BYTES ((size_t)BLOCK * BLOCKS)
#define TREE_BASE 400u
#define MAP_BLOCK 16u

/* build_checkpoints: two descriptor slots and four data slots per run. */
enum {
	BLOCK_ZERO = 0,
	NEWEST_MAP = 3,
	NEWEST_NX = 4,
	EXTENDED_NX = 5,
	NEWEST_OMAP = 17,
	DATA_SPACEMAN = 32,
	DATA_REAPER = 33,
	DATA_REAP_LIST = 34,
	NEWEST_SPACEMAN = 36,
	DESCRIPTOR_SLOTS = 8,
	OBJECT_BLOCK = 800
};

/* Independent byte positions used to damage one named field at a time. */
enum {
	OBJECT_OID_OFFSET = 8,
	OBJECT_XID_OFFSET = 16,
	OBJECT_TYPE_OFFSET = 24,
	OBJECT_SUBTYPE_OFFSET = 28,
	CP_FLAGS_OFFSET = 32,
	NODE_FLAGS_OFFSET = 32,
	OMAP_FLAGS_OFFSET = 32,
	REAP_LIST_NEXT_OFFSET = 32,
	NODE_LEVEL_OFFSET = 34,
	NODE_COUNT_OFFSET = 36,
	NX_BLOCK_SIZE_OFFSET = 36,
	OMAP_SNAPSHOT_COUNT_OFFSET = 36,
	NX_BLOCK_COUNT_OFFSET = 40,
	OMAP_TREE_TYPE_OFFSET = 40,
	REAPER_HEAD_OFFSET = 48,
	NATIVE_VOLUME_INCOMPAT_OFFSET = 56,
	NODE_KEY0_OFFSET = 56,
	REAPER_TAIL_OFFSET = 56,
	NODE_VALUE0_OFFSET = 58,
	REAPER_COUNT_OFFSET = 68,
	CP_FIRST_ADDRESS_OFFSET = 72,
	NODE_KEY0_OID_OFFSET = 72,
	NX_UUID_OFFSET = 72,
	OMAP_REVERT_MIN_OFFSET = 72,
	CP_SECOND_TYPE_OFFSET = 80,
	OMAP_REVERT_MAX_OFFSET = 80,
	NODE_KEY1_OID_OFFSET = 88,
	NX_NEXT_XID_OFFSET = 96,
	CP_SECOND_OID_OFFSET = 104,
	NX_DATA_BLOCKS_OFFSET = 108,
	REAPER_STATE_SIZE_OFFSET = 108,
	CP_SECOND_ADDRESS_OFFSET = 112,
	NX_DESC_BASE_OFFSET = 112,
	NODE_KEY3_OID_OFFSET = 120,
	NX_DATA_BASE_OFFSET = 120,
	NX_DESC_NEXT_OFFSET = 128,
	NATIVE_VOLUME_ROOT_OID_OFFSET = 136,
	NX_DESC_INDEX_OFFSET = 136,
	NX_DESC_LEN_OFFSET = 140,
	NX_DATA_LEN_OFFSET = 148,
	NX_SPACEMAN_OID_OFFSET = 152,
	NX_OMAP_OID_OFFSET = 160,
	NX_MAX_VOLUMES_OFFSET = 180,
	OMAP_VALUE_FLAGS_OFFSET = 4040,
	OMAP_VALUE_SIZE_OFFSET = 4044,
	ROOT_CHILD0_OFFSET = 4048,
	OMAP_VALUE_ADDRESS_OFFSET = 4048,
	INFO_FLAGS_OFFSET = 4056,
	INFO_NODE_SIZE_OFFSET = 4060,
	INFO_KEY_SIZE_OFFSET = 4064,
	INFO_VALUE_SIZE_OFFSET = 4068,
	INDEX_CHILD0_OFFSET = 4088
};

typedef struct fixture {
	unsigned	char *bytes;
	uint64_t	length;
	uint32_t	block_size;
	uint64_t	baseline;
	uint64_t	read_calls;
	const struct mutation *reread_mutation;
	uint64_t	reread_after;
	bool		reread_changed;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t reader;
	apfs_container_t container;
	fsimg_error_t error;
} fixture_t;

typedef enum {
	MUTATE_FIELD,
	MUTATE_CHECKSUM,
	MUTATE_ZERO
} mutation_kind_t;

typedef struct mutation {
	const char *name;
	uint32_t	block;
	uint32_t	offset;
	uint64_t	value;
	unsigned	width;
	fsimg_status_t status;
	mutation_kind_t kind;
	bool		forbid_ghost;
} mutation_t;

static unsigned failures, scenarios;
static const char *scenario;

static void
expect(bool condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", scenario, what);
		failures++;
	}
}

static bool
status_is(fixture_t *f, fsimg_status_t actual, fsimg_status_t wanted)
{
	scenarios++;
	if (actual == wanted)
		return (true);
	fprintf(stderr, "%s: expected status %d, got %d (%s)\n",
	    scenario, wanted, actual, f->error.message);
	failures++;
	return (false);
}

static unsigned char *
block_at(fixture_t *f, uint64_t block)
{
	CHECK(block < BLOCKS);
	return (f->bytes + block * f->block_size);
}

static void
mutate(fixture_t *f, const mutation_t *m)
{
	unsigned char *raw = block_at(f, m->block);

	if (m->kind == MUTATE_CHECKSUM) {
		raw[0] ^= 1;
		return;
	}
	if (m->kind == MUTATE_ZERO) {
		bzero(raw, BLOCK);
		return;
	}
	CHECK(m->offset + m->width <= BLOCK);
	for (unsigned i = 0; i < m->width; i++)
		raw[m->offset + i] = (unsigned char)(m->value >> (8 * i));
	CHECK(apfs_gen_seal(raw, BLOCK));
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	CHECK(offset <= f->length && length <= f->length - offset);
	f->read_calls++;
	if (f->reread_mutation != NULL &&
	    f->read_calls > f->reread_after &&
	    offset == (uint64_t)f->reread_mutation->block * BLOCK &&
	    !f->reread_changed) {
		mutate(f, f->reread_mutation);
		f->reread_changed = true;
	}
	bcopy(f->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static void
reader_init(fixture_t *f, uint64_t work)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	if (work != 0)
		options.max_work = work;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline = stats.memory_bytes;
	f->reader.ctx = f->ctx;
	fsimg_read_options_init(&f->reader.options);
	fsimg_session_init(&f->reader.session, f->ctx, "apfs test", 0);
	f->reader.state = FSIMG_READER_OPENING;
}

static void
fixture_init_block_size(fixture_t *f, uint64_t work, uint32_t block_size)
{
	fsimg_io_ops_t ops;

	bzero(f, sizeof(*f));
	f->block_size = block_size;
	f->length = (uint64_t)block_size * BLOCKS;
	f->bytes = calloc(1, f->length);
	CHECK(f->bytes != NULL);
	reader_init(f, work);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io,
	    NULL) == FSIMG_OK);
	f->reader.input = f->io;
	f->reader.slice_bytes = f->length;
}

static void
fixture_init(fixture_t *f, uint64_t work)
{
	fixture_init_block_size(f, work, BLOCK);
}

static void
fixture_close(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_apfs_reader_ops.close(&f->reader);
	apfs_container_close(&f->container);
	fsimg_io_free(f->reader.narrowed);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	expect(stats.memory_bytes == f->baseline, "memory returns to baseline");
	expect(stats.scratch_bytes == 0, "scratch released");
	fsimg_ctx_free(f->ctx);
	free(f->bytes);
}

static void
fixture_truncate(fixture_t *f, uint64_t length)
{
	CHECK(fsimg_io_slice(f->io, 0, length, &f->reader.narrowed,
	    &f->error) == FSIMG_OK);
	f->reader.input = f->reader.narrowed;
	f->reader.slice_bytes = length;
	f->length = length;
}

static fsimg_status_t
checkpoint_open(fixture_t *f)
{
	apfs_obj_t header;
	apfs_nx_t nx;

	CHECK(apfs_obj_decode(f->bytes, BLOCK, &header));
	CHECK(apfs_nx_decode(f->bytes, BLOCK, &nx));
	f->reader.slice_bytes = f->length;
	return (apfs_checkpoint_open(&f->reader, &header, &nx,
	    &f->container, &f->error));
}

static void
build_container_map(fixture_t *f)
{
	apfs_gen_omap_t omap = {
		.object = { .oid = MAP_BLOCK, .xid = 1, .type = 0x4000000b },
		.flags = 1, .tree_type = 0x40000002, .tree_oid = TREE_BASE
	};

	CHECK(apfs_gen_omap_build(block_at(f, MAP_BLOCK), f->block_size,
	    &omap));
}

/* Each checkpoint has private data so damage cannot affect its predecessor. */
static void
build_checkpoints(fixture_t *f, unsigned count, uint64_t block0_xid)
{
	apfs_gen_mapping_t mappings[3][2];
	apfs_gen_run_t runs[3];
	apfs_gen_container_t image = {
		.nx = { .magic = 0x4253584e, .block_size = f->block_size,
		.block_count = BLOCKS, .uuid = { 1, 2, 3 },
		.desc_base = 1, .desc_blocks = DESCRIPTOR_SLOTS,
		    .data_base = DATA_SPACEMAN,
		.data_blocks = 300, .max_volumes = 100,
		.spaceman_oid = 100, .reaper_oid = 102, .omap_oid = MAP_BLOCK },
		.runs = runs, .count = count, .block0_run = count - 1,
		.block0_xid = block0_xid
	};

	CHECK(count >= 1 && count <= 3);
	for (unsigned i = 0; i < count; i++) {
		mappings[i][0] = (apfs_gen_mapping_t){ .type = 0x80000005,
		    .size = f->block_size, .oid = 100,
		    .address = DATA_SPACEMAN + 4 * i };
		mappings[i][1] = (apfs_gen_mapping_t){ .type = 0x80000011,
		    .size = f->block_size, .oid = 102,
		    .address = DATA_REAPER + 4 * i };
		runs[i] = (apfs_gen_run_t){ .xid = 10 * (i + 1),
		    .next_xid = 10 * (i + 1) + 1, .mappings = mappings[i],
		    .mapping_count = 2, .mapping_blocks = 1,
		    .desc_index = 2 * i, .desc_next = 2 * i + 2,
		    .data_index = 4 * i, .data_len = 4,
		    .data_next = 4 * i + 4 };
	}
	CHECK(apfs_gen_container_build(f->bytes, f->length, &image));
	build_container_map(f);
}

/* Full-open fixtures keep the older checkpoint to catch accidental fallback.
 * occupied_slots is a bitmask of volume-array indexes.
 * Three blocks per volume: APSB, physical object map, empty map root.
 */
#define VOLUME_BASE 500u
#define VOLUME_OID 2000u
#define VOLUME_SLOT 2u
#define VOLUME_MAX 4u

static void
build_openable_layout(fixture_t *f, unsigned occupied_slots,
    apfs_gen_layout_t layout)
{
	apfs_gen_omap_record_t records[VOLUME_MAX];
	apfs_gen_tree_t tree;
	apfs_gen_fs_t fs;
	apfs_gen_omap_t map;
	apfs_gen_node_t node;
	apfs_gen_spaceman_t spaceman;
	uint64_t root, blocks;
	size_t count = 0;

	build_checkpoints(f, 2, 10);
	if (layout == APFS_GEN_LAYOUT_OLDER)
		CHECK(apfs_gen_spaceman_geometry_older(f->block_size,
		    BLOCKS, &spaceman) == f->block_size);
	else
		CHECK(apfs_gen_spaceman_geometry(f->block_size, BLOCKS,
		    &spaceman) == f->block_size);
	spaceman.object = (apfs_gen_object_t){ .oid = 100, .xid = 20,
	    .type = 0x80000005 };
	spaceman.devices[0].free_count = 700;
	for (unsigned b = 0; b <= NEWEST_NX; b += 2) {
		fsimg_store_le64(block_at(f, b) + 64, 2);
		fsimg_store_le32(block_at(f, b) + 180, VOLUME_MAX);
		for (unsigned slot = 0; slot < VOLUME_MAX; slot++)
			fsimg_store_le64(block_at(f, b) + 184 + 8 * slot,
			    (occupied_slots & (1u << slot)) ?
			    VOLUME_OID + slot : 0);
		CHECK(apfs_gen_seal(block_at(f, b), f->block_size));
	}
	/*
	 * The container builder left a current-layout version and size at
	 * 336, where the older layout's XID array lies; start it from zero.
	 */
	if (layout == APFS_GEN_LAYOUT_OLDER)
		bzero(block_at(f, NEWEST_SPACEMAN), f->block_size);
	CHECK(apfs_gen_spaceman_build(block_at(f, NEWEST_SPACEMAN),
	    f->block_size, &spaceman));
	for (unsigned slot = 0; slot < VOLUME_MAX; slot++) {
		unsigned address = VOLUME_BASE + 3 * slot;

		/* Map every slot so the no-volume case still has a valid
		    tree. */
		records[count++] = (apfs_gen_omap_record_t){
		    .key = { .oid = VOLUME_OID + slot, .xid = 10 },
		    .value = { .size = f->block_size, .address = address } };
		fs = (apfs_gen_fs_t){
		    .object = { .oid = VOLUME_OID + slot, .xid = 10,
		    .type = 13 },
		    .magic = 0x42535041, .index = slot,
		    .uuid = { 1, 2, 3, (unsigned char)(slot + 1) },
		    .name = "Selected volume", .flags = 1,
		    .incompat_features = 9, .root_tree_type = 2,
		    .extentref_tree_type = 0x40000002,
		    .snapshot_tree_type = 0x40000002, .omap_oid = address + 1,
		    .files = 11, .directories = 12, .symlinks = 13,
		    .other_objects = 14, .snapshots = 15, .alloc_blocks = 16,
		    .role = 64 };
		CHECK(apfs_gen_fs_build(block_at(f, address), f->block_size,
		    &fs));
		map = (apfs_gen_omap_t){ .object = { .oid = address + 1,
		    .xid = 10, .type = 0x4000000b },
		    .tree_type = 0x40000002, .tree_oid = address + 2 };
		CHECK(apfs_gen_omap_build(block_at(f, address + 1),
		    f->block_size, &map));
		node = (apfs_gen_node_t){ .object = { .oid = address + 2,
		    .xid = 10, .type = 0x40000002, .subtype = 11 },
		    .flags = GEN_APFS_ROOT | GEN_APFS_LEAF | GEN_APFS_FIXED,
		    .info = { .flags = 0x17, .node_size = f->block_size,
		    .key_size = 16, .val_size = 16, .node_count = 1 } };
		CHECK(apfs_gen_node_build(block_at(f, address + 2),
		    f->block_size, &node));
	}
	tree = (apfs_gen_tree_t){ .records = records, .count = count,
	    .base = TREE_BASE, .xid = 10, .block_size = f->block_size,
	    .records_per_leaf = VOLUME_MAX, .children_per_index = 2 };
	CHECK(apfs_gen_tree_build(f->bytes, f->length, &tree, &root, &blocks));
	map = (apfs_gen_omap_t){ .object = { .oid = MAP_BLOCK,
	    .xid = 10, .type = 0x4000000b }, .flags = 1,
	    .tree_type = 0x40000002, .tree_oid = root };
	CHECK(apfs_gen_omap_build(block_at(f, MAP_BLOCK), f->block_size, &map));
}

static void
build_openable(fixture_t *f, unsigned occupied_slots)
{
	build_openable_layout(f, occupied_slots, APFS_GEN_LAYOUT_CURRENT);
}

/* A plain-name root with one regular file backed by one physical extent. */
static void
build_openable_file(fixture_t *f, apfs_gen_layout_t layout,
    const unsigned char *bytes, size_t length)
{
	enum {
		NAME_KEY_BYTES = GEN_DREC_KEY_BYTES + sizeof("file"),
		STREAM_FIELDS_BYTES = GEN_XF_BLOB_BYTES +
		    GEN_XF_HEADER_BYTES + GEN_DSTREAM_BYTES,
		FILE_INODE_BYTES = GEN_INODE_VAL_BYTES + STREAM_FIELDS_BYTES
	};
	unsigned char keys[4][32] = { { 0 } }, values[4][160] = { { 0 } };
	unsigned char fields[STREAM_FIELDS_BYTES], stream[GEN_DSTREAM_BYTES];
	apfs_gen_record_t records[4];
	apfs_gen_omap_record_t mapping;
	apfs_gen_fs_tree_t tree = { .records = records, .count = 4,
	    .first_oid = 2100, .base = 600, .xid = 10, .block_size = BLOCK,
	    .records_per_leaf = 4, .children_per_index = 2 };
	apfs_gen_tree_t map_tree = { .records = &mapping, .count = 1,
	    .base = VOLUME_BASE + 3 * VOLUME_SLOT + 2, .xid = 10,
	    .block_size = BLOCK, .records_per_leaf = 1,
	    .children_per_index = 2 };
	apfs_gen_drec_key_t name = { .parent = 2, .name = "file",
	    .name_len = 4 };
	apfs_gen_drec_val_t drec = { .child = 16, .flags = 8 };
	apfs_gen_dstream_t data = { .size = length, .alloced_size = BLOCK };
	apfs_gen_extent_val_t extent = { .length = BLOCK, .phys_block = 700 };
	apfs_gen_xfield_t field = { .type = 8, .data = stream,
	    .size = sizeof(stream) };
	apfs_gen_inode_val_t inode = { .parent = 1, .private_id = 2,
	    .mode = 0040755, .count = 1 };
	unsigned char *volume;
	uint64_t root, blocks;

	CHECK(length <= BLOCK);
	build_openable_layout(f, 1u << VOLUME_SLOT, layout);
	CHECK(apfs_gen_jkey_build(keys[0], GEN_JKEY_BYTES, 2, 3));
	CHECK(apfs_gen_inode_val_build(values[0], GEN_INODE_VAL_BYTES, &inode));
	records[0] = (apfs_gen_record_t){ .key = keys[0],
	    .key_size = GEN_JKEY_BYTES,
	    .value = values[0], .value_size = GEN_INODE_VAL_BYTES };
	CHECK(apfs_gen_drec_key_build(keys[1], NAME_KEY_BYTES, &name));
	CHECK(apfs_gen_drec_val_build(values[1], GEN_DREC_VAL_BYTES, &drec));
	records[1] = (apfs_gen_record_t){ .key = keys[1],
	    .key_size = NAME_KEY_BYTES,
	    .value = values[1], .value_size = GEN_DREC_VAL_BYTES };
	CHECK(apfs_gen_dstream_build(stream, sizeof(stream), &data));
	CHECK(apfs_gen_xfields_build(fields, STREAM_FIELDS_BYTES, &field, 1));
	inode = (apfs_gen_inode_val_t){ .parent = 2, .private_id = 16,
	    .mode = 0100644, .count = 1, .xfields = fields,
	    .xfields_len = STREAM_FIELDS_BYTES };
	CHECK(apfs_gen_jkey_build(keys[2], GEN_JKEY_BYTES, 16, 3));
	CHECK(apfs_gen_inode_val_build(values[2], FILE_INODE_BYTES, &inode));
	records[2] = (apfs_gen_record_t){ .key = keys[2],
	    .key_size = GEN_JKEY_BYTES,
	    .value = values[2], .value_size = FILE_INODE_BYTES };
	CHECK(apfs_gen_jkey_build(keys[3], GEN_JKEY_BYTES, 16, 8));
	CHECK(apfs_gen_extent_val_build(values[3], GEN_EXTENT_VAL_BYTES,
	    &extent));
	records[3] = (apfs_gen_record_t){ .key = keys[3],
	    .key_size = GEN_EXTENT_KEY_BYTES,
	    .value = values[3], .value_size = GEN_EXTENT_VAL_BYTES };
	CHECK(apfs_gen_fs_tree_build(f->bytes, f->length, &tree, &mapping,
	    1, &root, &blocks));
	CHECK(apfs_gen_tree_build(f->bytes, f->length, &map_tree,
	    &root, &blocks));
	volume = block_at(f, VOLUME_BASE + 3 * VOLUME_SLOT);
	fsimg_store_le64(volume + NATIVE_VOLUME_INCOMPAT_OFFSET, 0);
	fsimg_store_le64(volume + NATIVE_VOLUME_ROOT_OID_OFFSET, 2100);
	CHECK(apfs_gen_seal(volume, BLOCK));
	bcopy(bytes, block_at(f, 700), length);
}

static void
test_older_layout_container_lists_and_reads_like_current(void)
{
	static const unsigned char payload[] = "APFS older layout payload";
	const apfs_gen_layout_t layouts[] = { APFS_GEN_LAYOUT_CURRENT,
	    APFS_GEN_LAYOUT_OLDER };
	fixture_t f;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	unsigned char delivered[sizeof(payload)];
	size_t count;

	for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
		scenario = layouts[i] == APFS_GEN_LAYOUT_CURRENT ?
		    "current layout lists and reads file" :
		    "older layout lists and reads file";
		fixture_init(&f, 0);
		build_openable_file(&f, layouts[i], payload, sizeof(payload));
		reader = NULL;
		if (!status_is(&f, fsimg_reader_open(f.ctx, f.io, NULL,
		    &reader, &f.error), FSIMG_OK)) {
			fsimg_reader_free(reader);
			fixture_close(&f);
			continue;
		}
		if (status_is(&f, fsimg_reader_next(reader, &entry,
		    &f.error), FSIMG_OK)) {
			expect(strcmp(entry->path, "/file") == 0 &&
			    entry->kind == FSIMG_ENTRY_FILE &&
			    entry->size == sizeof(payload),
			    "tree lists one file");
			if (status_is(&f, fsimg_reader_read_at(reader, 0,
			    delivered, sizeof(delivered), &count, &f.error),
			    FSIMG_OK))
				expect(count == sizeof(payload) &&
				    bcmp(delivered, payload, count) == 0,
				    "file bytes match the shared payload");
			status_is(&f, fsimg_reader_next(reader, &entry,
			    &f.error), FSIMG_DONE);
		}
		fsimg_reader_free(reader);
		fixture_close(&f);
	}
}

static fsimg_status_t
full_open(fixture_t *f)
{
	return (fsimg_apfs_reader_ops.open(&f->reader, NULL, &f->error));
}

/* The predecessor must open independently for a fallback test to prove
 * that refusal came from the selected state rather than an unusable past.
 */
static void
test_full_open_predecessor_is_openable(void)
{
	fixture_t f;

	scenario = "full open predecessor is openable";
	fixture_init(&f, 0);
	build_openable(&f, 1u << VOLUME_SLOT);
	bzero(block_at(&f, NEWEST_NX), BLOCK);
	if (status_is(&f, full_open(&f), FSIMG_OK)) {
		apfs_reader_t *apfs = f.reader.driver;

		expect(apfs->container.xid == 10,
		    "predecessor checkpoint selected");
		expect(apfs->fs.uuid[3] == 3, "predecessor volume resolves");
	}
	fixture_close(&f);
}

/* Damage only the newest checkpoint's private map. The corrupt block
 * remains present when removing its superblock selects the predecessor.
 */
static void
test_full_open_private_map_damage_does_not_fall_back(void)
{
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	apfs_gen_omap_t map = {
		.object = { .oid = NEWEST_OMAP, .xid = 20, .type = 0x4000000b },
		.flags = 1, .tree_type = 0x40000002, .tree_oid = TREE_BASE
	};

	scenario = "full open private map damage does not fall back";
	fixture_init(&f, 0);
	build_openable(&f, 1u << VOLUME_SLOT);
	CHECK(apfs_gen_omap_build(block_at(&f, NEWEST_OMAP), BLOCK, &map));
	mutate(&f, &(mutation_t){ .block = NEWEST_NX,
	    .offset = NX_OMAP_OID_OFFSET, .value = NEWEST_OMAP, .width = 8 });
	block_at(&f, NEWEST_OMAP)[0] ^= 1;
	status_is(&f, fsimg_reader_open(f.ctx, f.io, NULL, &reader,
	    &f.error), FSIMG_ECORRUPT);
	fsimg_reader_free(reader);
	reader = NULL;

	bzero(block_at(&f, NEWEST_NX), BLOCK);
	if (status_is(&f, fsimg_reader_open(f.ctx, f.io, NULL, &reader,
	    &f.error), FSIMG_OK)) {
		apfs_reader_t *apfs = reader->driver;

		expect(apfs->container.xid == 10,
		    "predecessor opens with newest private map still corrupt");
	}
	fsimg_reader_free(reader);
	fixture_close(&f);
}

static void
test_full_open_volume_selection(void)
{
	static const struct {
		const char *name;
		unsigned slots;
		bool selected;
		uint32_t index;
		fsimg_status_t status;
		const char *message;
	} cases[] = {
		{ .name = "explicit occupied slot", .slots = 5,
		    .selected = true, .index = 2, .status = FSIMG_OK },
		{ .name = "explicit empty slot", .slots = 5,
		    .selected = true, .index = 1, .status = FSIMG_EINVAL,
		    .message = "slot 1 is empty" },
		{ .name = "index equals maximum", .slots = 5,
		    .selected = true, .index = 4, .status = FSIMG_EINVAL,
		    .message = "maximum of 4" },
		{ .name = "index exceeds maximum", .slots = 5,
		    .selected = true, .index = UINT32_MAX,
		    .status = FSIMG_EINVAL,
		    .message = "maximum of 4" },
		{ .name = "only volume in nonzero slot", .slots = 4,
		    .selected = false, .index = 0, .status = FSIMG_OK },
		{ .name = "ambiguous volumes", .slots = 5,
		    .selected = false, .index = 0, .status = FSIMG_EINVAL,
		    .message = "2 volumes in slots 0 to 2" },
		{ .name = "no volumes", .slots = 0, .status = FSIMG_EINVAL,
		    .message = "holds no volume" }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_openable(&f, cases[i].slots);
		f.reader.options.volume_index_set = cases[i].selected;
		f.reader.options.volume_index = cases[i].index;
		if (status_is(&f, full_open(&f), cases[i].status)) {
			if (cases[i].status == FSIMG_OK) {
				apfs_reader_t *apfs = f.reader.driver;

				expect(apfs->volume_index == 2,
				    "selected volume is slot 2");
				expect(apfs->fs.uuid[3] == 3,
				    "selected volume UUID");
			} else {
				expect(strstr(f.error.message,
				    cases[i].message) != NULL,
				    "selection diagnostic names count or slot");
				expect(strstr(f.error.message,
				    "Selected volume") == NULL,
				    "selection diagnostic exposes no volume name");
			}
		}
		fixture_close(&f);
	}
}

static void
test_full_open_volume_validation(void)
{
	static const mutation_t cases[] = {
		{ .name = "volume magic",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 32, .value = 0, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume slot mismatch",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 36, .value = 1, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume zero UUID",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 240, .value = 0, .width = 8,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume invalid UTF-8",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 704, .value = 128, .width = 1,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume sealed",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 32, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume incomplete restore",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 16, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume unknown incompat",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 128, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume encrypted",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 0, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume one key",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 9, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume spilled over",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 17, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume spillover cleaner",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 33, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume unknown flags",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 129, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume revert XID",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 160, .value = 1, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume revert OID",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 168, .value = 1, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume rolling OID",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 976, .value = 1, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume root-to XID",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 968, .value = 1, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "volume root tree type",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 116, .value = 1073741826, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume extentref tree type",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 120, .value = 2, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume snapshot tree type",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 124, .value = 2, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "volume case insensitive",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 1, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume normalization insensitive",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 8, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume dataless snapshots",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 2, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume encryption rolled unencrypted",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 56, .value = 4, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume extentref flag",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 264, .value = 65, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume read-only features",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 48, .value = UINT64_MAX, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "volume optional features",
		    .block = VOLUME_BASE + 3 * VOLUME_SLOT,
		    .offset = 40, .value = UINT64_MAX, .width = 8,
		    .status = FSIMG_OK },
	};
	fixture_t f;
	unsigned char *raw;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (unsigned assume = 0; assume < 2; assume++) {
			scenario = cases[i].name;
			fixture_init(&f, 0);
			build_openable(&f, 1u << VOLUME_SLOT);
			if (cases[i].offset == 240)
				bzero(block_at(&f, cases[i].block) + 240, 16);
			mutate(&f, &cases[i]);
			if (assume) {
				f.bytes[32] ^= 1;
				CHECK(apfs_gen_seal(f.bytes, BLOCK));
				f.reader.options.flags =
				    FSIMG_READ_ASSUME_IDENTITY;
			}
			status_is(&f, full_open(&f), cases[i].status);
			fixture_close(&f);
		}
	}
	scenario = "volume name without terminator";
	fixture_init(&f, 0);
	build_openable(&f, 1u << VOLUME_SLOT);
	raw = block_at(&f, VOLUME_BASE + 3 * VOLUME_SLOT);
	memset(raw + 704, 'n', 256);
	CHECK(apfs_gen_seal(raw, BLOCK));
	status_is(&f, full_open(&f), FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_full_open_information_and_prepare(void)
{
	static const struct {
		const char *name;
		uint64_t value;
	} details[] = {
		{ .name = "volume_index", .value = 2 },
		{ .name = "volume_count", .value = 1 },
		{ .name = "role", .value = 64 },
		{ .name = "case_insensitive", .value = 1 },
		{ .name = "norm_insensitive", .value = 1 },
		{ .name = "checkpoint_xid", .value = 20 },
		{ .name = "read_xid", .value = 20 },
		{ .name = "files", .value = 11 },
		{ .name = "directories", .value = 12 },
		{ .name = "symlinks", .value = 13 },
		{ .name = "other_objects", .value = 14 },
		{ .name = "snapshots", .value = 15 },
		{ .name = "volume_blocks", .value = 16 },
		{ .name = "sealed", .value = 0 }
	};
	fixture_t f;
	fsimg_volume_info_t *info;
	unsigned char *raw;

	for (unsigned long_name = 0; long_name < 2; long_name++) {
		scenario = long_name ? "255-byte volume label" :
		    "full volume information";
		fixture_init(&f, 0);
		build_openable(&f, 1u << VOLUME_SLOT);
		raw = block_at(&f, VOLUME_BASE + 3 * VOLUME_SLOT);
		if (long_name) {
			memset(raw + 704, 'n', 255);
			raw[959] = 0;
			CHECK(apfs_gen_seal(raw, BLOCK));
		}
		if (status_is(&f, full_open(&f), FSIMG_OK)) {
			info = &f.reader.info;
			expect(strcmp(info->format, "apfs") == 0,
			    "format apfs");
			expect(strcmp(info->profile, APFS_PROFILE) == 0,
			    "Unicode profile");
			expect(strcmp(info->label, (char *)raw + 704) == 0,
			    "entire label");
			expect(memcmp(info->uuid, raw + 240, 16) == 0,
			    "UUID bytes");
			expect(info->volume_bytes == IMAGE_BYTES,
			    "container bytes");
			expect(info->allocation_units == BLOCKS,
			    "container blocks");
			expect(info->allocation_unit_bytes == BLOCK,
			    "allocation unit bytes");
			expect(info->sector_bytes == BLOCK, "sector bytes");
			expect(info->free_bytes == 700 * BLOCK,
			    "main-device free bytes");
			expect(info->clean && !info->io_error,
			    "clean without I/O error");
			expect(info->time_basis == FSIMG_TIME_UTC,
			    "UTC timestamps");
			expect(info->scope == FSIMG_VALIDATION_HEADER,
			    "header validation scope");
			expect(info->detail_count == 14, "fourteen details");
			for (size_t i = 0; i < 15 && i < info->detail_count;
			    i++) {
				expect(strcmp(info->details[i].name,
				    details[i].name) == 0,
				    details[i].name);
				expect(info->details[i].value ==
				    details[i].value,
				    details[i].name);
			}
			expect(fsimg_apfs_reader_ops.prepare != NULL &&
			    fsimg_apfs_reader_ops.replay != NULL,
			    "directory listing operations are wired");
		}
		fixture_close(&f);
	}
}

static void
test_full_open_probe_and_assumed_identity(void)
{
	static const mutation_t cases[] = {
		{ .name = "block zero missing magic", .offset = 32,
		    .value = 0, .width = 4, .status = FSIMG_EFORMAT },
		{ .name = "block zero wrong kind", .offset = 24,
		    .value = 2147483650, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "block zero wrong OID", .offset = 8,
		    .value = 2, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "block zero physical storage", .offset = 24,
		    .value = 1073741825, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "block zero virtual storage", .offset = 24,
		    .value = 1, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "block zero bad block size", .offset = 36,
		    .value = 6144, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "block zero bad block count", .offset = 40,
		    .value = 255, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "block zero Fusion", .offset = 64,
		    .value = 258, .width = 8, .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero version one", .offset = 64,
		    .value = 1, .width = 8, .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero unknown incompat", .offset = 64,
		    .value = 130, .width = 8, .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero low-capacity without Fusion",
		    .offset = 48,
		    .value = 2, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "block zero zero XID", .offset = 16,
		    .value = 0, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "block zero encrypted", .offset = 24,
		    .value = 0x90000001u, .width = 4,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero nonpersistent", .offset = 24,
		    .value = 0x88000001u, .width = 4,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero unknown flag", .offset = 1264,
		    .value = 1, .width = 8, .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero software cryptography flag",
		    .offset = 1264, .value = 4, .width = 8,
		    .status = FSIMG_OK },
		{ .name = "block zero stale checksum", .kind = MUTATE_CHECKSUM,
		    .status = FSIMG_ECORRUPT }
	};
	fixture_t f;
	const fsimg_format_variant_t *variant;
	fsimg_status_t wanted;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (unsigned assume = 0; assume < 2; assume++) {
			scenario = cases[i].name;
			fixture_init(&f, 0);
			build_openable(&f, 1u << VOLUME_SLOT);
			mutate(&f, &cases[i]);
			status_is(&f, fsimg_apfs_format.probe(f.bytes, BLOCK,
			    IMAGE_BYTES, &variant, &f.error), cases[i].status);
			f.reader.options.flags =
			    assume ? FSIMG_READ_ASSUME_IDENTITY : 0;
			/* The probe's absent identity is open's failed
			 * identity check, FSIMG_ECORRUPT unless assumed
			 * (fsimg_reader_identity()).
			 */
			wanted = cases[i].status;
			if (wanted == FSIMG_EFORMAT)
				wanted = assume ? FSIMG_OK : FSIMG_ECORRUPT;
			if (status_is(&f, full_open(&f), wanted) &&
			    wanted == FSIMG_OK)
				expect((f.reader.assumed_identity != NULL) ==
				    (cases[i].status == FSIMG_EFORMAT),
				    "only a failed identity is recorded");
			fixture_close(&f);
		}
	}
	scenario = "probe prefix shorter than one block";
	fixture_init(&f, 0);
	build_openable(&f, 1u << VOLUME_SLOT);
	status_is(&f, fsimg_apfs_format.probe(f.bytes, BLOCK - 1,
	    BLOCK - 1, &variant, &f.error), FSIMG_ECORRUPT);
	fixture_truncate(&f, BLOCK - 1);
	status_is(&f, full_open(&f), FSIMG_ECORRUPT);
	fixture_close(&f);

	scenario = "assumed identity retains checksum validation";
	fixture_init(&f, 0);
	build_openable(&f, 1u << VOLUME_SLOT);
	f.bytes[32] ^= 1;
	f.reader.options.flags =
	    FSIMG_READ_ASSUME_IDENTITY;
	status_is(&f, full_open(&f), FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_probe_uses_4096_byte_prefix_and_open_checks_whole_block(void)
{
	fixture_t f;
	const fsimg_format_variant_t *variant;
	unsigned char *prefix;

	scenario = "APFS probe needs only 4096 bytes";
	expect(fsimg_apfs_format.probe_bytes == 4096, "probe_bytes is 4096");
	prefix = malloc(4096);
	CHECK(prefix != NULL);

	scenario = "16384-byte block probes from a 4096-byte allocation";
	fixture_init_block_size(&f, 0, 16384);
	build_openable(&f, 1u << VOLUME_SLOT);
	bcopy(f.bytes, prefix, 4096);
	status_is(&f, fsimg_apfs_format.probe(prefix, 4096,
	    f.length, &variant, &f.error), FSIMG_OK);

	scenario = "16384-byte block exceeds 8192-byte volume";
	status_is(&f, fsimg_apfs_format.probe(prefix, 4096,
	    8192, &variant, &f.error), FSIMG_ECORRUPT);

	scenario = "undamaged 16384-byte container opens";
	if (!status_is(&f, full_open(&f), FSIMG_OK)) {
		free(prefix);
		fixture_close(&f);
		return;
	}
	fixture_close(&f);

	for (unsigned assume = 0; assume < 2; assume++) {
		scenario = assume ?
		    "assumed identity checks beyond probe prefix" :
		    "open checks beyond probe prefix";
		fixture_init_block_size(&f, 0, 16384);
		build_openable(&f, 1u << VOLUME_SLOT);
		/* Leave block zero's checksum stale beyond the probe's
		    allocation. */
		f.bytes[8192] ^= 1;
		bcopy(f.bytes, prefix, 4096);
		status_is(&f, fsimg_apfs_format.probe(prefix, 4096,
		    f.length, &variant, &f.error), FSIMG_OK);
		f.reader.options.flags =
		    assume ? FSIMG_READ_ASSUME_IDENTITY : 0;
		if (status_is(&f, full_open(&f), FSIMG_ECORRUPT))
			expect(strstr(f.error.message, "bad checksum") != NULL,
			    "open fails on the block-0 checksum");
		fixture_close(&f);
	}
	free(prefix);
}

static void
test_full_open_checkpoint_features_and_free_count(void)
{
	static const mutation_t cases[] = {
		{ .name = "container object map checksum", .block = MAP_BLOCK,
		    .kind = MUTATE_CHECKSUM, .status = FSIMG_ECORRUPT },
		{ .name = "selected checkpoint Fusion", .block = NEWEST_NX,
		    .offset = 64, .value = 0x102, .width = 8,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "main free count above container blocks",
		    .block = NEWEST_SPACEMAN, .offset = 72,
		    .value = BLOCKS + 1, .width = 8, .status = FSIMG_ECORRUPT }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (unsigned assume = 0; assume < 2; assume++) {
			scenario = cases[i].name;
			fixture_init(&f, 0);
			build_openable(&f, 1u << VOLUME_SLOT);
			mutate(&f, &cases[i]);
			if (assume) {
				f.bytes[32] ^= 1;
				CHECK(apfs_gen_seal(f.bytes, BLOCK));
				f.reader.options.flags =
				    FSIMG_READ_ASSUME_IDENTITY;
			}
			status_is(&f, full_open(&f), cases[i].status);
			fixture_close(&f);
		}
	}
}

static void
test_full_open_truncated_container(void)
{
	static const uint64_t cuts[] = { 5 * BLOCK, 38 * BLOCK, 512 * BLOCK };
	fixture_t f;
	fsimg_reader_t *reader = NULL;
	fsimg_volume_info_t info;

	for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
		scenario = "full open truncated container";
		fixture_init(&f, 0);
		build_openable(&f, 1u << VOLUME_SLOT);
		fixture_truncate(&f, cuts[i]);
		if (i < 2) {
			if (status_is(&f, full_open(&f), FSIMG_ECORRUPT))
				expect(strcmp(f.error.message,
				    "image is truncated") == 0,
				    "truncation never selects older state");
		} else if (status_is(&f, fsimg_reader_open(f.ctx,
		    f.reader.input, NULL, &reader, &f.error), FSIMG_OK)) {
			CHECK(fsimg_reader_get_info(reader, &info, &f.error) ==
			    FSIMG_OK);
			expect(info.missing_bytes == IMAGE_BYTES - cuts[i],
			    "missing bytes");
			fsimg_reader_free(reader);
		}
		fixture_close(&f);
	}
}

static void
test_selected_descriptor_changed_while_read(void)
{
	static const mutation_t cases[] = {
		{ .name = "selected NX descriptor index changed on reread",
		    .block = NEWEST_NX, .offset = NX_DESC_INDEX_OFFSET,
		    .value = 3, .width = 4 },
		{ .name = "selected NX data length changed on reread",
		    .block = NEWEST_NX, .offset = NX_DATA_LEN_OFFSET,
		    .value = 3, .width = 4 },
		{ .name = "selected mapping XID changed on reread",
		    .block = NEWEST_MAP, .offset = OBJECT_XID_OFFSET,
		    .value = 19, .width = 8 },
		{ .name = "selected mapping last flag cleared on reread",
		    .block = NEWEST_MAP, .offset = CP_FLAGS_OFFSET,
		    .value = 0, .width = 4 }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		f.reread_mutation = &cases[i];
		/* The ring scan reads its eight slots before rereading
		    selection. */
		f.reread_after = DESCRIPTOR_SLOTS;
		if (status_is(&f, checkpoint_open(&f), FSIMG_ECORRUPT))
			expect(strstr(f.error.message,
			    "changed while read") != NULL,
			    "reread mismatch has the changed-while-read diagnostic");
		expect(f.reread_changed,
		    "modified block served after ring scan");
		expect(apfs_object_verify(block_at(&f, cases[i].block), BLOCK),
		    "changed block has a valid checksum");
		fixture_close(&f);
	}
}

static void
test_private_container_map_damage_never_falls_back(void)
{
	static const mutation_t cases[] = {
		{ .name = "private newest container map wrong kind",
		    .block = NEWEST_OMAP, .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x4000000d, .width = 4,
		    .status = FSIMG_ECORRUPT },
		{ .name = "private newest container map bad checksum",
		    .block = NEWEST_OMAP,
		    .kind = MUTATE_CHECKSUM, .status = FSIMG_ECORRUPT },
		{ .name = "private newest container map future XID",
		    .block = NEWEST_OMAP, .offset = OBJECT_XID_OFFSET,
		    .value = 21, .width = 8,
		    .status = FSIMG_ECORRUPT },
		{ .name = "private newest container map encrypted",
		    .block = NEWEST_OMAP, .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x5000000b, .width = 4,
		    .status = FSIMG_EUNSUPPORTED }
	};
	fixture_t f;
	apfs_gen_omap_t map = {
		.object = { .oid = NEWEST_OMAP, .xid = 20, .type = 0x4000000b },
		.flags = 1, .tree_type = 0x40000002, .tree_oid = TREE_BASE
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		CHECK(apfs_gen_omap_build(block_at(&f, NEWEST_OMAP), BLOCK,
		    &map));
		mutate(&f, &(mutation_t){ .block = NEWEST_NX,
		    .offset = NX_OMAP_OID_OFFSET,
		    .value = NEWEST_OMAP, .width = 8 });
		mutate(&f, &cases[i]);

		status_is(&f, checkpoint_open(&f), cases[i].status);
		expect(apfs_object_verify(block_at(&f, MAP_BLOCK), BLOCK),
		    "older checkpoint retains its undamaged private map");
		fixture_close(&f);
	}
}

static void
test_descriptor_flags_and_subtypes_exclude_newest_checkpoint(void)
{
	static const struct {
		mutation_t mutation;
		bool clean;
	} cases[] = {
		{ .mutation = { .name = "encrypted newest NX ignored",
		    .block = NEWEST_NX, .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x90000001, .width = 4 }, .clean = true },
		{ .mutation = { .name = "subtyped newest NX ignored",
		    .block = NEWEST_NX, .offset = OBJECT_SUBTYPE_OFFSET,
		    .value = 1, .width = 4 }, .clean = true },
		{ .mutation = {
		    .name = "encrypted newest mapping makes torn run",
		    .block = NEWEST_MAP, .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x5000000c, .width = 4 } },
		{ .mutation = {
		    .name = "subtyped newest mapping makes torn run",
		    .block = NEWEST_MAP, .offset = OBJECT_SUBTYPE_OFFSET,
		    .value = 1, .width = 4 } }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].mutation.name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		mutate(&f, &cases[i].mutation);

		if (status_is(&f, checkpoint_open(&f), FSIMG_OK)) {
			expect(f.container.xid == 10,
			    "older checkpoint selected");
			expect(f.container.clean == cases[i].clean,
			    "clean state reflects whether newest run was torn");
		}
		fixture_close(&f);
	}
}

static void
test_checkpoint_selection_and_zero_slots(void)
{
	fixture_t f;

	for (unsigned count = 1; count <= 3; count++) {
		scenario = count == 1 ? "one checkpoint with zero slots" :
		    "largest of several checkpoint XIDs";
		fixture_init(&f, 0);
		build_checkpoints(&f, count, 10 * count);
		if (status_is(&f, checkpoint_open(&f), FSIMG_OK)) {
			expect(f.container.xid == 10 * count, "selected XID");
			expect(f.container.clean, "whole newest is clean");
			expect(f.container.mapping_count == 2, "mapping count");
			expect(f.container.block_count == BLOCKS,
			    "declared block count");
			expect(f.container.nx.desc_base == 1 &&
			    f.container.nx.desc_blocks == 8 &&
			    f.container.nx.data_base == 32 &&
			    f.container.nx.spaceman_oid == 100 &&
			    f.container.nx.reaper_oid == 102,
			    "selected geometry and OIDs");
		}
		fixture_close(&f);
	}
}

static void
test_torn_newest_checkpoint_opens_older_unclean(void)
{
	static const mutation_t cases[] = {
		{ .name = "zeroed newest mapping", .block = NEWEST_MAP,
		    .kind = MUTATE_ZERO },
		{ .name = "mapping XID differs", .block = NEWEST_MAP,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 19, .width = 8 },
		{ .name = "last-map flag missing", .block = NEWEST_MAP,
		    .offset = CP_FLAGS_OFFSET,
		    .value = 0, .width = 4 },
		{ .name = "run length one", .block = NEWEST_NX,
		    .offset = NX_DESC_LEN_OFFSET,
		    .value = 1, .width = 4 },
		{ .name = "next XID not above XID", .block = NEWEST_NX,
		    .offset = NX_NEXT_XID_OFFSET,
		    .value = 20, .width = 8 },
		{ .name = "descriptor next outside ring", .block = NEWEST_NX,
		    .offset = NX_DESC_NEXT_OFFSET,
		    .value = 8, .width = 4 }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (unsigned committed = 0; committed < 2; committed++) {
			scenario = cases[i].name;
			fixture_init(&f, 0);
			build_checkpoints(&f, 2, committed ? 20 : 10);
			mutate(&f, &cases[i]);
			if (status_is(&f, checkpoint_open(&f),
			    committed ? FSIMG_ECORRUPT : FSIMG_OK) &&
			    !committed) {
				expect(f.container.xid == 10,
				    "older checkpoint selected");
				expect(!f.container.clean,
				    "torn newest is unclean");
			}
			fixture_close(&f);
		}
	}
}

static void
test_invalid_superblocks_and_ambiguous_checkpoints(void)
{
	static const mutation_t cases[] = {
		{ .name = "candidate UUID differs", .block = NEWEST_NX,
		    .offset = NX_UUID_OFFSET,
		    .value = 99, .width = 1, .status = FSIMG_OK },
		{ .name = "candidate block size differs", .block = NEWEST_NX,
		    .offset = NX_BLOCK_SIZE_OFFSET,
		    .value = 8192, .width = 4, .status = FSIMG_OK },
		{ .name = "candidate descriptor base differs",
		    .block = NEWEST_NX, .offset = NX_DESC_BASE_OFFSET,
		    .value = 2, .width = 8, .status = FSIMG_OK },
		{ .name = "duplicate checkpoint XID", .block = NEWEST_NX,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 10, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "block zero newer than ring", .block = BLOCK_ZERO,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 21, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "candidate data area noncontiguous",
		    .block = NEWEST_NX, .offset = NX_DATA_BLOCKS_OFFSET,
		    .value = 0x8000012c, .width = 4,
		    .status = FSIMG_EUNSUPPORTED },
		{ .name = "block zero data area noncontiguous",
		    .block = BLOCK_ZERO, .offset = NX_DATA_BLOCKS_OFFSET,
		    .value = 0x8000012c, .width = 4,
		    .status = FSIMG_EUNSUPPORTED }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		mutate(&f, &cases[i]);
		if (status_is(&f, checkpoint_open(&f), cases[i].status) &&
		    cases[i].status == FSIMG_OK) {
			expect(f.container.xid == 10, "noncandidate ignored");
			expect(f.container.clean,
			    "noncandidate leaves clean true");
		}
		fixture_close(&f);
	}
	for (unsigned zero = 0; zero < 2; zero++) {
		scenario = zero ? "zeroed newest superblock" :
		    "newest superblock checksum damaged";
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		if (zero)
			bzero(block_at(&f, NEWEST_NX), BLOCK);
		else
			block_at(&f, NEWEST_NX)[0] ^= 1;
		if (status_is(&f, checkpoint_open(&f), FSIMG_OK))
			expect(f.container.xid == 10 && f.container.clean,
			    "invalid newest leaves older checkpoint clean");
		fixture_close(&f);
	}
	scenario = "no whole checkpoint";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	bzero(block_at(&f, 1), BLOCK);
	status_is(&f, checkpoint_open(&f), FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_selected_checkpoint_damage_never_falls_back(void)
{
	static const mutation_t cases[] = {
		{ .name = "mapped bad checksum", .block = NEWEST_SPACEMAN,
		    .kind = MUTATE_CHECKSUM },
		{ .name = "selected container map bad checksum",
		    .block = MAP_BLOCK,
		    .kind = MUTATE_CHECKSUM },
		{ .name = "selected container map wrong kind",
		    .block = MAP_BLOCK,
		    .offset = OBJECT_TYPE_OFFSET, .value = 0x4000000d,
		    .width = 4 },
		{ .name = "mapped wrong kind", .block = NEWEST_SPACEMAN,
		    .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x80000006, .width = 4 },
		{ .name = "mapped wrong subtype", .block = NEWEST_SPACEMAN,
		    .offset = OBJECT_SUBTYPE_OFFSET,
		    .value = 1, .width = 4 },
		{ .name = "mapped wrong OID", .block = NEWEST_SPACEMAN,
		    .offset = OBJECT_OID_OFFSET,
		    .value = 101, .width = 8 },
		{ .name = "mapped future XID", .block = NEWEST_SPACEMAN,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 21, .width = 8 },
		{ .name = "mapping outside data run", .block = NEWEST_MAP,
		    .offset = CP_FIRST_ADDRESS_OFFSET,
		    .value = 40, .width = 8 },
		{ .name = "overlapping mappings", .block = NEWEST_MAP,
		    .offset = CP_SECOND_ADDRESS_OFFSET,
		    .value = 36, .width = 8 },
		{ .name = "duplicate ephemeral OID", .block = NEWEST_MAP,
		    .offset = CP_SECOND_OID_OFFSET,
		    .value = 100, .width = 8 },
		{ .name = "space manager not mapped", .block = NEWEST_NX,
		    .offset = NX_SPACEMAN_OID_OFFSET,
		    .value = 999, .width = 8 },
		{ .name = "reaper mapped as another kind",
		    .block = NEWEST_MAP, .offset = CP_SECOND_TYPE_OFFSET,
		    .value = 0x80000012, .width = 4 }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		mutate(&f, &cases[i]);

		status_is(&f, checkpoint_open(&f), FSIMG_ECORRUPT);
		fixture_close(&f);
	}
}

static void
test_truncated_containers_do_not_select_older_state(void)
{
	static const struct {
		const char *name;
		uint64_t length;
		fsimg_status_t status;
	} cases[] = {
		{ .name = "cut inside descriptor ring", .length = 5 * BLOCK,
		    .status = FSIMG_ECORRUPT },
		{ .name = "cut inside selected mapped objects",
		    .length = 37 * BLOCK,
		    .status = FSIMG_ECORRUPT },
		{ .name = "cut inside selected data run", .length = 38 * BLOCK,
		    .status = FSIMG_ECORRUPT },
		{ .name = "cut beyond checkpoint areas", .length = 350 * BLOCK,
		    .status = FSIMG_OK }
	};
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		fixture_truncate(&f, cases[i].length);
		if (status_is(&f, checkpoint_open(&f), cases[i].status)) {
			if (cases[i].status == FSIMG_OK)
				expect(f.container.block_count == BLOCKS,
				    "truncation preserves declared geometry");
			else
				expect(strcmp(f.error.message,
				    "image is truncated") == 0,
				    "truncation diagnostic");
		}
		fixture_close(&f);
	}
}

static void
test_foreign_uuid_precedes_noncontiguous_data_geometry(void)
{
	fixture_t f;

	for (unsigned foreign = 0; foreign < 2; foreign++) {
		scenario = foreign ? "foreign noncontiguous candidate ignored" :
		    "same-container noncontiguous candidate unsupported";
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		mutate(&f, &(mutation_t){ .block = NEWEST_NX,
		    .offset = NX_DATA_BLOCKS_OFFSET,
		    .value = 0x8000012c, .width = 4 });
		if (foreign)
			mutate(&f, &(mutation_t){ .block = NEWEST_NX,
			    .offset = NX_UUID_OFFSET,
			    .value = 99, .width = 1 });
		if (status_is(&f, checkpoint_open(&f),
		    foreign ? FSIMG_OK : FSIMG_EUNSUPPORTED) && foreign)
			expect(f.container.xid == 10 && f.container.clean,
			    "foreign candidate leaves older checkpoint clean");
		fixture_close(&f);
	}
}

static void
test_checkpoint_rejects_multiblock_reaper_and_reap_list(void)
{
	apfs_gen_mapping_t mappings[3] = {
		{ .type = 0x80000005, .size = BLOCK, .oid = 100,
		    .address = 32 },
		{ .type = 0x80000011, .size = BLOCK, .oid = 102,
		    .address = 33 },
		{ .type = 0x80000012, .size = BLOCK, .oid = 104, .address = 35 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
		.mappings = mappings, .mapping_count = 3, .mapping_blocks = 1,
		.data_len = 5, .data_next = 5, .desc_next = 2 };
	apfs_gen_container_t image = {
		.nx = { .magic = 0x4253584e, .block_size = BLOCK,
		.block_count = BLOCKS, .uuid = { 1 }, .desc_base = 1,
		.desc_blocks = DESCRIPTOR_SLOTS, .data_base = DATA_SPACEMAN,
		    .data_blocks = 300,
		.max_volumes = 100, .spaceman_oid = 100, .reaper_oid = 102,
		.omap_oid = MAP_BLOCK },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	fixture_t f;

	for (unsigned oversized = 1; oversized <= 2; oversized++) {
		scenario = oversized == 1 ? "two-block reaper refused" :
		    "two-block reap-list refused";
		fixture_init(&f, 0);
		mappings[1].size = oversized == 1 ? 2 * BLOCK : BLOCK;
		mappings[2].size = oversized == 2 ? 2 * BLOCK : BLOCK;
		CHECK(apfs_gen_container_build(f.bytes, IMAGE_BYTES, &image));
		build_container_map(&f);
		if (status_is(&f, checkpoint_open(&f), FSIMG_ECORRUPT))
			expect(strstr(f.error.message, "wrong size") != NULL,
			    "mapping size rejected before object validation");
		fixture_close(&f);
	}
}

static void
test_wrapping_runs_large_tables_and_multiblock_objects(void)
{
	static const uint64_t absent[] = { 99, 301, 999 };
	apfs_gen_mapping_t mappings[205], swap;
	apfs_gen_run_t run;
	apfs_gen_container_t image;
	apfs_cpm_t mapping;
	fixture_t f;
	bool found;

	static const struct {
		const char *name;
		uint32_t count, map_blocks, desc_index, data_index, data_len;
		uint64_t spaceman, reaper;
		uint32_t object_blocks;
		bool reverse, damage;
		fsimg_status_t status;
	} cases[] = {
		{ .name = "descriptor and data runs wrap", .count = 2,
		    .map_blocks = 1, .desc_index = 7, .data_index = 298,
		    .data_len = 4, .spaceman = 330, .reaper = 32,
		    .object_blocks = 1, .status = FSIMG_OK },
		{ .name = "three mapping blocks and 205 objects", .count = 205,
		    .map_blocks = 3, .data_len = 205, .spaceman = 32,
		    .reaper = 33,
		    .object_blocks = 1, .reverse = true, .status = FSIMG_OK },
		{ .name = "two-block ephemeral object", .count = 2,
		    .map_blocks = 1,
		    .data_len = 4, .spaceman = 32, .reaper = 34,
		    .object_blocks = 2, .status = FSIMG_OK },
		{ .name = "damaged second block of ephemeral object",
		    .count = 2,
		    .map_blocks = 1, .data_len = 4, .spaceman = 32,
		    .reaper = 34,
		    .object_blocks = 2, .damage = true,
		    .status = FSIMG_ECORRUPT },
		{ .name = "mapping straddles data ring wrap", .count = 2,
		    .map_blocks = 1, .data_index = 298, .data_len = 4,
		    .spaceman = 331, .reaper = 32, .object_blocks = 2,
		    .status = FSIMG_ECORRUPT }
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		scenario = cases[c].name;
		fixture_init(&f, 0);
		image = (apfs_gen_container_t){
		    .nx = { .magic = 0x4253584e, .block_size = BLOCK,
		    .block_count = BLOCKS, .uuid = { 1 }, .desc_base = 1,
		    .desc_blocks = DESCRIPTOR_SLOTS,
		    .data_base = DATA_SPACEMAN, .data_blocks = 300,
		    .max_volumes = 100, .spaceman_oid = 100, .reaper_oid = 102,
		    .omap_oid = MAP_BLOCK },
		    .runs = &run, .count = 1, .block0_xid = 10,
		    .spaceman_ring_blocks =
		    cases[c].object_blocks == 2 ? 1000 : 0 };
		run = (apfs_gen_run_t){ .xid = 10, .next_xid = 11,
		    .mappings = mappings, .mapping_count = cases[c].count,
		    .mapping_blocks = cases[c].map_blocks,
		    .desc_index = cases[c].desc_index, .desc_next = 2,
		    .data_len = cases[c].data_len,
		    .data_index = cases[c].data_index,
		    .data_next = 2 };
		for (uint32_t i = 0; i < run.mapping_count; i++)
			mappings[i] = (apfs_gen_mapping_t){
			    .type = i == 0 ? 0x80000005 :
			    i == 1 ? 0x80000011 : 0x80000012,
			    .size = BLOCK, .oid = 100 + 2 * i,
			    .address = 32 + i };
		mappings[0].address = cases[c].spaceman;
		mappings[0].size = cases[c].object_blocks * BLOCK;
		mappings[1].address = cases[c].reaper;
		if (cases[c].reverse) {
			for (uint32_t i = 0; i < run.mapping_count / 2; i++) {
				swap = mappings[i];
				mappings[i] =
				    mappings[run.mapping_count - 1 - i];
				mappings[run.mapping_count - 1 - i] = swap;
			}
		}
		CHECK(apfs_gen_container_build(f.bytes, IMAGE_BYTES, &image));
		build_container_map(&f);
		if (cases[c].damage)
			block_at(&f,
			    cases[c].spaceman + 1)[GEN_APFS_HEADER] ^= 1;
		if (status_is(&f, checkpoint_open(&f),
		    cases[c].status) && cases[c].reverse) {
			expect(f.container.mapping_count == 205,
			    "all mappings retained");
			for (unsigned i = 0; i < 205; i++) {
				CHECK(apfs_ephemeral_find(&f.container,
				    100 + 2 * i,
				    &found, &mapping, &f.error) == FSIMG_OK);
				expect(found && mapping.address == 32 + i,
				    "paged lookup finds each mapping");
			}
			for (size_t i = 0;
			    i < sizeof(absent) / sizeof(absent[0]); i++) {
				CHECK(apfs_ephemeral_find(&f.container,
				    absent[i],
				    &found, &mapping, &f.error) == FSIMG_OK);
				expect(!found,
				    "absent OID below, between or above mappings");
			}
		}
		fixture_close(&f);
	}
}

static void
test_last_map_flag_belongs_only_on_last_mapping_block(void)
{
	fixture_t f;
	unsigned char *raw;

	for (unsigned wrong = 0; wrong < 2; wrong++) {
		scenario = "last-map flag on wrong one of two mapping blocks";
		fixture_init(&f, 0);
		build_checkpoints(&f, 2, 10);
		/* Extend the newest run by one empty mapping block and move
		    NX. */
		bcopy(block_at(&f, NEWEST_NX), block_at(&f, EXTENDED_NX),
		    BLOCK);
		bzero(block_at(&f, NEWEST_NX), BLOCK);
		raw = block_at(&f, NEWEST_NX);
		CHECK(apfs_gen_checkpoint_build(raw, BLOCK,
		    &(apfs_gen_checkpoint_t){ .object = { .oid = 4, .xid = 20,
		    .type = 0x4000000c }, .flags = wrong ? 1 : 0 }));
		mutate(&f, &(mutation_t){ .block = EXTENDED_NX,
		    .offset = NX_DESC_LEN_OFFSET,
		    .value = 3, .width = 4 });
		if (status_is(&f, checkpoint_open(&f), FSIMG_OK))
			expect(f.container.xid == 10 && !f.container.clean,
			    "wrong last flag makes newest run torn");
		fixture_close(&f);
	}
}

static void
test_geometry_rejects_invalid_sizes_volume_slots_and_areas(void)
{
	static const mutation_t cases[] = {
		{ .name = "block size too small", .block = BLOCK_ZERO,
		    .offset = NX_BLOCK_SIZE_OFFSET,
		    .value = 2048, .width = 4 },
		{ .name = "block size too large", .block = BLOCK_ZERO,
		    .offset = NX_BLOCK_SIZE_OFFSET,
		    .value = 131072, .width = 4 },
		{ .name = "block size not power of two", .block = BLOCK_ZERO,
		    .offset = NX_BLOCK_SIZE_OFFSET,
		    .value = 6144, .width = 4 },
		{ .name = "container below one MiB", .block = BLOCK_ZERO,
		    .offset = NX_BLOCK_COUNT_OFFSET,
		    .value = 255, .width = 8 },
		{ .name = "maximum volumes zero", .block = BLOCK_ZERO,
		    .offset = NX_MAX_VOLUMES_OFFSET,
		    .value = 0, .width = 4 },
		{ .name = "maximum volumes 101", .block = BLOCK_ZERO,
		    .offset = NX_MAX_VOLUMES_OFFSET,
		    .value = 101, .width = 4 },
		{ .name = "overlapping areas", .block = BLOCK_ZERO,
		    .offset = NX_DATA_BASE_OFFSET,
		    .value = 4, .width = 8 },
		{ .name = "descriptor starts at zero", .block = BLOCK_ZERO,
		    .offset = NX_DESC_BASE_OFFSET,
		    .value = 0, .width = 8 },
		{ .name = "data area outside container", .block = BLOCK_ZERO,
		    .offset = NX_DATA_BASE_OFFSET,
		    .value = 1000, .width = 8 },
		{ .name = "descriptor outside container", .block = BLOCK_ZERO,
		    .offset = NX_DESC_BASE_OFFSET,
		    .value = BLOCKS, .width = 8 }
	};
	fixture_t f;
	apfs_nx_t nx;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		mutate(&f, &cases[i]);
		CHECK(apfs_nx_decode(f.bytes, BLOCK, &nx));
		status_is(&f, apfs_nx_geometry(&nx, "test geometry", &f.error),
		    FSIMG_ECORRUPT);
		fixture_close(&f);
	}
	scenario = "volume OID at maximum volume slot";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(apfs_nx_decode(f.bytes, BLOCK, &nx));
	nx.max_volumes = 1;
	nx.volume_oids[1] = 123;
	status_is(&f, apfs_nx_geometry(&nx, "test geometry", &f.error),
	    FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_physical_and_ephemeral_object_references(void)
{
	static const mutation_t mutations[] = {
		{ .name = "physical object success", .block = OBJECT_BLOCK,
		    .offset = OBJECT_OID_OFFSET,
		    .value = 800, .width = 8, .status = FSIMG_OK },
		{ .name = "physical wrong OID", .block = OBJECT_BLOCK,
		    .offset = OBJECT_OID_OFFSET,
		    .value = 801, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "physical wrong kind", .block = OBJECT_BLOCK,
		    .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x4000000e, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "physical wrong subtype", .block = OBJECT_BLOCK,
		    .offset = OBJECT_SUBTYPE_OFFSET,
		    .value = 1, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "physical wrong storage", .block = OBJECT_BLOCK,
		    .offset = OBJECT_TYPE_OFFSET,
		    .value = 13, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "physical zero XID", .block = OBJECT_BLOCK,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 0, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "physical future XID", .block = OBJECT_BLOCK,
		    .offset = OBJECT_XID_OFFSET,
		    .value = 11, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "physical encrypted", .block = OBJECT_BLOCK,
		    .offset = OBJECT_TYPE_OFFSET,
		    .value = 0x5000000d, .width = 4,
		    .status = FSIMG_EUNSUPPORTED }
	};
	static const struct {
		const char *name;
		uint64_t oid, slice;
		uint32_t storage;
		bool checksum;
		fsimg_status_t status;
	} references[] = {
		{ .name = "physical bad checksum", .oid = 800,
		    .storage = APFS_OBJ_PHYSICAL, .checksum = true,
		    .status = FSIMG_ECORRUPT },
		{ .name = "physical block zero", .storage = APFS_OBJ_PHYSICAL,
		    .status = FSIMG_ECORRUPT },
		{ .name = "physical block at N", .oid = BLOCKS,
		    .storage = APFS_OBJ_PHYSICAL, .status = FSIMG_ECORRUPT },
		{ .name = "physical block beyond slice", .oid = 800,
		    .storage = APFS_OBJ_PHYSICAL, .slice = 700 * BLOCK,
		    .status = FSIMG_ECORRUPT },
		{ .name = "ephemeral reference succeeds", .oid = 100,
		    .storage = APFS_OBJ_EPHEMERAL, .status = FSIMG_OK },
		{ .name = "unmapped ephemeral OID", .oid = 101,
		    .storage = APFS_OBJ_EPHEMERAL, .status = FSIMG_ECORRUPT }
	};
	unsigned char window[BLOCK];
	apfs_ref_t ref;
	apfs_obj_t header;
	fixture_t f;

	for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		scenario = mutations[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		CHECK(apfs_gen_object_build(block_at(&f, OBJECT_BLOCK), BLOCK,
		    &(apfs_gen_object_t){ .oid = 800, .xid = 10,
		    .type = 0x4000000d }));
		ref = (apfs_ref_t){ .role = "test object", .oid = 800,
		    .xid = 10, .storage = APFS_OBJ_PHYSICAL, .kind = 13 };
		mutate(&f, &mutations[i]);
		status_is(&f, apfs_object_read(&f.container, &ref, window,
		    &header, &f.error), mutations[i].status);
		fixture_close(&f);
	}
	for (size_t i = 0; i < sizeof(references) / sizeof(references[0]);
	    i++) {
		scenario = references[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		CHECK(apfs_gen_object_build(block_at(&f, OBJECT_BLOCK), BLOCK,
		    &(apfs_gen_object_t){ .oid = 800, .xid = 10,
		    .type = 0x4000000d }));
		ref = (apfs_ref_t){ .role = "test object",
		    .oid = references[i].oid,
		    .xid = 10, .storage = references[i].storage,
		    .kind = references[i].storage == APFS_OBJ_PHYSICAL ?
		    13 : 5 };
		if (references[i].checksum)
			block_at(&f, OBJECT_BLOCK)[0] ^= 1;
		if (references[i].slice != 0)
			fixture_truncate(&f, references[i].slice);
		if (status_is(&f, apfs_object_read(&f.container, &ref, window,
		    &header, &f.error),
		    references[i].status) && references[i].slice != 0)
			expect(strcmp(f.error.message,
			    "image is truncated") == 0,
			    "object past slice reports truncation");
		fixture_close(&f);
	}
}

/* build_tree(16, 4, 2): four leaves, two indexes, then the root. */
enum {
	LEAF0 = TREE_BASE,
	INDEX0 = TREE_BASE + 4,
	ROOT = TREE_BASE + 6
};

/* Record OIDs have gaps, exposing LE/GE mistakes between leaves. */
static uint64_t
build_tree(fixture_t *f, unsigned count, unsigned leaf, unsigned children,
    bool nonaligned, unsigned toc)
{
	apfs_gen_omap_record_t records[64];
	apfs_gen_tree_t tree;
	apfs_gen_omap_t map;
	uint64_t root, blocks;

	CHECK(count <= 64);
	for (unsigned i = 0; i < count; i++)
		records[i] = (apfs_gen_omap_record_t){
		    .key = { .oid = 1000 + 2 * i, .xid = 5 },
		    .value = { .size = BLOCK, .address = 800 + i } };
	tree = (apfs_gen_tree_t){ .records = records, .count = count,
	    .base = TREE_BASE, .xid = 10, .block_size = BLOCK,
	    .records_per_leaf = leaf, .children_per_index = children,
	    .toc_capacity = toc, .nonaligned = nonaligned };
	CHECK(apfs_gen_tree_build(f->bytes, IMAGE_BYTES, &tree, &root,
	    &blocks));
	bzero(block_at(f, MAP_BLOCK), BLOCK);
	map = (apfs_gen_omap_t){ .object = { .oid = MAP_BLOCK, .xid = 10,
	    .type = 0x4000000b }, .flags = 1, .tree_type = 0x40000002,
	    .tree_oid = root };
	CHECK(apfs_gen_omap_build(block_at(f, MAP_BLOCK), BLOCK, &map));
	return (root);
}

static fsimg_status_t
map_open(fixture_t *f, apfs_map_t *map, apfs_map_kind_t kind)
{
	unsigned char window[BLOCK];

	return (apfs_map_open(&f->container, kind, MAP_BLOCK, 10, window,
	    map, &f->error));
}

static void
expect_record(apfs_cursor_t *cursor, uint64_t oid)
{
	apfs_record_t record;
	apfs_omap_key_t key;
	apfs_omap_val_t value;

	apfs_cursor_record(cursor, &record);
	CHECK(record.key != NULL);
	CHECK(apfs_omap_key_decode(record.key, record.key_len, &key));
	expect(key.oid == oid && key.xid == 5, "cursor record key");
	expect(record.key_len == 16 && record.val_len == 16 && !record.ghost,
	    "cursor leaf record shape");
	CHECK(apfs_omap_val_decode(record.val, record.val_len, &value));
	expect(value.flags == 0 && value.size == BLOCK &&
	    value.address == 800 + (oid - 1000) / 2, "cursor leaf value");
}

static void
test_multilevel_cursors_seek_and_visit_in_order(void)
{
	unsigned char key[16];
	apfs_map_t map;
	fixture_t f;
	bool found;

	static const struct {
		const char *name;
		unsigned leaf, children, toc;
		bool nonaligned;
	} cases[] = {
		{ .name = "height one tree", .leaf = 16, .children = 4 },
		{ .name = "height two tree", .leaf = 4, .children = 4 },
		{ .name = "height three tree", .leaf = 4, .children = 2 },
		{ .name = "unaligned tree offsets", .leaf = 4, .children = 4,
		    .nonaligned = true },
		{ .name = "oversized TOC capacity", .leaf = 4, .children = 4,
		    .toc = 128 }
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		scenario = cases[c].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		build_tree(&f, 16, cases[c].leaf, cases[c].children,
		    cases[c].nonaligned, cases[c].toc);
		CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
		if (status_is(&f, apfs_cursor_first(&map.cursor, &found,
		    &f.error), FSIMG_OK)) {
			for (unsigned i = 0; i < 16; i++) {
				expect(found, "iteration has every record");
				if (!found)
					break;
				expect_record(&map.cursor, 1000 + 2 * i);
				if (!status_is(&f, apfs_cursor_next(&map.cursor,
				    &found, &f.error), FSIMG_OK))
					break;
			}
			expect(!found, "iteration ends after last record");
		}
		for (unsigned direction = 0; direction < 2; direction++) {
			for (uint64_t oid = 998; oid <= 1033; oid++) {
				bool wanted = direction == 0 ? oid >= 1000 :
				    oid <= 1030;
				uint64_t target = direction == 0 ?
				    (oid > 1030 ? 1030 : oid - oid % 2) :
				    (oid < 1000 ? 1000 : oid + oid % 2);

				CHECK(apfs_gen_omap_key_build(key, sizeof(key),
				    &(apfs_gen_omap_key_t){ .oid = oid,
				    .xid = 5 }));
				if (status_is(&f,
				    apfs_cursor_seek(&map.cursor, key,
				    sizeof(key), direction == 0 ? APFS_SEEK_LE :
				    APFS_SEEK_GE, &found, &f.error),
				    FSIMG_OK)) {
					expect(found == wanted,
					    "seek presence at boundary");
					if (found)
						expect_record(&map.cursor,
						    target);
				}
			}
		}
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_separator_may_precede_child_first_key(void)
{
	unsigned char key[16];
	apfs_map_t map;
	fixture_t f;
	bool found;

	scenario = "LE seek between separator and child's first key";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	CHECK(build_tree(&f, 16, 4, 4, false, 0) == 404);
	mutate(&f, &(mutation_t){ .block = INDEX0,
	    .offset = NODE_KEY1_OID_OFFSET,
	    .value = 1007, .width = 8 });
	CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
	CHECK(apfs_gen_omap_key_build(key, sizeof(key),
	    &(apfs_gen_omap_key_t){ .oid = 1007, .xid = 5 }));
	if (status_is(&f, apfs_cursor_seek(&map.cursor, key, sizeof(key),
	    APFS_SEEK_LE, &found, &f.error), FSIMG_OK)) {
		expect(found, "previous leaf supplies predecessor");
		if (found)
			expect_record(&map.cursor, 1006);
	}
	apfs_map_close(&map);
	fixture_close(&f);
}

static void
test_nodes_reject_invalid_flags_levels_bounds_and_layout(void)
{
	static const mutation_t cases[] = {
		{ .name = "node bad checksum", .block = LEAF0,
		    .kind = MUTATE_CHECKSUM, .status = FSIMG_ECORRUPT },
		{ .name = "ghost without tree permission", .block = LEAF0,
		    .offset = NODE_VALUE0_OFFSET, .value = 0xffff, .width = 2,
		    .forbid_ghost = true, .status = FSIMG_ECORRUPT },
		{ .name = "transient node", .block = ROOT,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 0x8005, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "unknown node flag", .block = ROOT,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 0x105, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "root flag missing", .block = ROOT,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 4, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "root flag on child", .block = INDEX0,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 5, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "index marked leaf", .block = ROOT,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 7, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "leaf flag missing", .block = LEAF0,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 4, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "child wrong level", .block = INDEX0,
		    .offset = NODE_LEVEL_OFFSET,
		    .value = 0, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "root level sixteen", .block = ROOT,
		    .offset = NODE_LEVEL_OFFSET,
		    .value = 16, .width = 2, .status = FSIMG_EUNSUPPORTED },
		{ .name = "child points at root", .block = ROOT,
		    .offset = ROOT_CHILD0_OFFSET,
		    .value = ROOT, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "child points at parent", .block = INDEX0,
		    .offset = INDEX_CHILD0_OFFSET,
		    .value = INDEX0, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "leaf keys decreasing", .block = LEAF0,
		    .offset = NODE_KEY1_OID_OFFSET,
		    .value = 999, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "leaf keys duplicate", .block = LEAF0,
		    .offset = NODE_KEY1_OID_OFFSET,
		    .value = 1000, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "child key below separator", .block = LEAF0,
		    .offset = NODE_KEY0_OID_OFFSET,
		    .value = 999, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "child key at next separator", .block = LEAF0,
		    .offset = NODE_KEY3_OID_OFFSET,
		    .value = 1008, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "misaligned key offset", .block = LEAF0,
		    .offset = NODE_KEY0_OFFSET,
		    .value = 1, .width = 2, .status = FSIMG_ECORRUPT },
		{ .name = "footer wrong node size", .block = ROOT,
		    .offset = INFO_NODE_SIZE_OFFSET,
		    .value = 8192, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "footer wrong key size", .block = ROOT,
		    .offset = INFO_KEY_SIZE_OFFSET,
		    .value = 8, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "footer wrong value size", .block = ROOT,
		    .offset = INFO_VALUE_SIZE_OFFSET,
		    .value = 8, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "footer physical flag missing", .block = ROOT,
		    .offset = INFO_FLAGS_OFFSET,
		    .value = 7, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "footer hashed flag", .block = ROOT,
		    .offset = INFO_FLAGS_OFFSET,
		    .value = 0x97, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "empty non-root node", .block = LEAF0,
		    .offset = NODE_COUNT_OFFSET,
		    .value = 0, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "node wrong subtype", .block = LEAF0,
		    .offset = OBJECT_SUBTYPE_OFFSET,
		    .value = 12, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "hashed node", .block = LEAF0,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 0xe, .width = 2, .status = FSIMG_EUNSUPPORTED },
		{ .name = "headerless node", .block = LEAF0,
		    .offset = NODE_FLAGS_OFFSET,
		    .value = 0x16, .width = 2, .status = FSIMG_EUNSUPPORTED },
		{ .name = "nonpersistent tree", .block = ROOT,
		    .offset = INFO_FLAGS_OFFSET,
		    .value = 0x37, .width = 4, .status = FSIMG_EUNSUPPORTED }
	};
	apfs_map_t map;
	fixture_t f;
	fsimg_status_t status;
	bool found;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		CHECK(build_tree(&f, 16, 4, 2, false, 0) == 406);
		mutate(&f, &cases[i]);
		if (cases[i].forbid_ghost)
			mutate(&f, &(mutation_t){ .block = ROOT,
			    .offset = INFO_FLAGS_OFFSET,
			    .value = 0x13, .width = 4 });
		CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
		status = apfs_cursor_first(&map.cursor, &found, &f.error);
		while (status == FSIMG_OK && found)
			status = apfs_cursor_next(&map.cursor, &found,
			    &f.error);
		status_is(&f, status, cases[i].status);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_map_lookup_selects_versions_and_deleted_entries_do_not_fall_back(void)
{
	static const uint64_t xids[] = { 4, 5, 9, 15, 100 };
	static const uint64_t expected[] = { 0, 5, 5, 10, 20 };
	apfs_gen_omap_record_t records[3];
	apfs_gen_tree_t tree;
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_map_t map;
	fixture_t f;
	uint64_t root, blocks;
	bool found;

	for (unsigned deleted = 0; deleted < 2; deleted++) {
		scenario = deleted ?
		    "deleted newest version never falls through" :
		    "map lookup chooses greatest XID not above request";
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		build_tree(&f, 1, 1, 2, false, 0);
		bzero(block_at(&f, TREE_BASE), BLOCK);
		for (unsigned i = 0; i < 3; i++)
			records[i] = (apfs_gen_omap_record_t){
			    .key = { .oid = 1026,
			    .xid = i == 0 ? 5 : i == 1 ? 10 : 20 },
			    .value = { .size = BLOCK, .address = 800 + i,
			    .flags = deleted && i == 2 ? 1 : 0 } };
		tree = (apfs_gen_tree_t){ .records = records, .count = 3,
		    .base = TREE_BASE, .xid = 10, .block_size = BLOCK,
		    .records_per_leaf = 3, .children_per_index = 2 };
		CHECK(apfs_gen_tree_build(f.bytes, IMAGE_BYTES, &tree, &root,
		    &blocks));
		CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
		for (size_t i = 0; i < sizeof(xids) / sizeof(xids[0]); i++) {
			if (status_is(&f, apfs_map_lookup(&map, 1026, xids[i],
			    &found, &key, &value, &f.error), FSIMG_OK)) {
				expect(found == (expected[i] != 0 &&
				    !(deleted && expected[i] == 20)),
				    "version lookup presence");
				if (found)
					expect(key.xid == expected[i],
					    "version lookup XID");
			}
		}
		CHECK(apfs_map_lookup(&map, 1027, 100, &found, &key,
		    &value, &f.error) == FSIMG_OK);
		expect(!found, "another OID misses");
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_map_values_enforce_flags_sizes_and_address_ranges(void)
{
	static const mutation_t cases[] = {
		{ .name = "encrypted mapping", .block = LEAF0,
		    .offset = OMAP_VALUE_FLAGS_OFFSET,
		    .value = 4, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "headerless mapping", .block = LEAF0,
		    .offset = OMAP_VALUE_FLAGS_OFFSET,
		    .value = 8, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "crypto-generation value flag", .block = LEAF0,
		    .offset = OMAP_VALUE_FLAGS_OFFSET,
		    .value = 16, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "unknown value flag", .block = LEAF0,
		    .offset = OMAP_VALUE_FLAGS_OFFSET,
		    .value = 32, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "zero mapping size", .block = LEAF0,
		    .offset = OMAP_VALUE_SIZE_OFFSET,
		    .value = 0, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "non-block mapping size", .block = LEAF0,
		    .offset = OMAP_VALUE_SIZE_OFFSET,
		    .value = BLOCK + 1, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "mapping address zero", .block = LEAF0,
		    .offset = OMAP_VALUE_ADDRESS_OFFSET,
		    .value = 0, .width = 8, .status = FSIMG_ECORRUPT },
		{ .name = "mapping range past N", .block = LEAF0,
		    .offset = OMAP_VALUE_ADDRESS_OFFSET,
		    .value = BLOCKS, .width = 8, .status = FSIMG_ECORRUPT }
	};
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_map_t map;
	fixture_t f;
	bool found;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		build_tree(&f, 1, 1, 2, false, 0);
		mutate(&f, &cases[i]);
		CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
		status_is(&f, apfs_map_lookup(&map, 1000, 10, &found,
		    &key, &value, &f.error), cases[i].status);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_map_object_flags_and_transitions(void)
{
	static const mutation_t cases[] = {
		{ .name = "container map requires manual flag",
		    .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 0, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "container map has snapshots", .block = MAP_BLOCK,
		    .offset = OMAP_SNAPSHOT_COUNT_OFFSET,
		    .value = 1, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "map wrong tree type", .block = MAP_BLOCK,
		    .offset = OMAP_TREE_TYPE_OFFSET,
		    .value = 2, .width = 4, .status = FSIMG_ECORRUPT },
		{ .name = "map encrypting", .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 3, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "map decrypting", .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 5, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "map rolling keys", .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 9, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "map unknown flag", .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 0x21, .width = 4, .status = FSIMG_EUNSUPPORTED },
		{ .name = "map pending revert minimum", .block = MAP_BLOCK,
		    .offset = OMAP_REVERT_MIN_OFFSET,
		    .value = 1, .width = 8, .status = FSIMG_EUNSUPPORTED },
		{ .name = "map pending revert maximum", .block = MAP_BLOCK,
		    .offset = OMAP_REVERT_MAX_OFFSET,
		    .value = 1, .width = 8, .status = FSIMG_EUNSUPPORTED }
	};
	apfs_map_t map;
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		build_tree(&f, 1, 1, 2, false, 0);
		mutate(&f, &cases[i]);

		status_is(&f, map_open(&f, &map, APFS_MAP_CONTAINER),
		    cases[i].status);
		apfs_map_close(&map);
		fixture_close(&f);
	}

	scenario = "volume map forbids manual flag";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	build_tree(&f, 1, 1, 2, false, 0);
	status_is(&f, map_open(&f, &map, APFS_MAP_VOLUME), FSIMG_ECORRUPT);
	apfs_map_close(&map);
	fixture_close(&f);
}

/* Compare only the OID; the XID bytes deliberately do not break ties. */
static int
profile_compare(const unsigned char *left, size_t left_len,
    const unsigned char *right, size_t right_len)
{
	uint64_t a, b;

	CHECK(left_len == 16 && right_len == 16);
	a = fsimg_load_le64(left);
	b = fsimg_load_le64(right);
	return (a < b ? -1 : a > b ? 1 : 0);
}

static const apfs_tree_profile_t test_profile = {
	.role = "test tree", .compare = profile_compare, .subtype = 11,
	.key_min = 16, .key_max = 16, .val_min = 16, .val_max = 16,
	.fixed_key = 16, .fixed_val = 16
};

static unsigned entry_calls[3];
static uint64_t rejected_key;

static fsimg_status_t
profile_entry_check(const apfs_record_t *record, bool leaf,
    fsimg_error_t *error)
{
	uint64_t oid;

	expect(leaf, "entry hook receives leaf flag");
	expect(record->key_len == 16 && record->val_len == 16 &&
	    record->val != NULL && !record->ghost,
	    "entry hook receives record");
	oid = fsimg_load_le64(record->key);
	CHECK(oid >= 1000 && oid <= 1004 && oid % 2 == 0);
	entry_calls[(oid - 1000) / 2]++;
	expect(fsimg_load_le64(record->val + 8) == 800 + (oid - 1000) / 2,
	    "entry hook receives matching value");
	if (oid == rejected_key)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "test hook rejects key"));
	return (FSIMG_OK);
}

static void
test_tree_entry_hook_checks_every_entry_and_propagates_failure(void)
{
	apfs_tree_profile_t profile = test_profile;
	apfs_cursor_t cursor;
	fixture_t f;
	uint64_t root;
	bool found;

	profile.entry_check = profile_entry_check;
	for (unsigned reject = 0; reject < 2; reject++) {
		scenario = reject ? "entry hook rejects last key" :
		    "entry hook validates every key";
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		root = build_tree(&f, 3, 3, 2, false, 0);
		bzero(entry_calls, sizeof(entry_calls));
		rejected_key = reject ? 1004 : 0;
		apfs_cursor_init(&cursor, &f.container, &profile,
		    APFS_OBJ_PHYSICAL, root, 10, NULL);
		if (status_is(&f, apfs_cursor_first(&cursor, &found, &f.error),
		    reject ? FSIMG_EUNSUPPORTED : FSIMG_OK)) {
			expect(found == !reject,
			    "hook failure leaves no record");
			if (reject)
				expect(strcmp(f.error.message,
				    "test hook rejects key") == 0,
				    "hook diagnostic preserved");
		}
		for (unsigned i = 0; i < 3; i++)
			expect(entry_calls[i] == 1,
			    "hook called once per entry");
		apfs_cursor_free(&cursor);
		fixture_close(&f);
	}
}

static unsigned index_entry_calls, leaf_entry_calls;

static fsimg_status_t
count_entry_kinds(const apfs_record_t *record, bool leaf,
    fsimg_error_t *error FSIMG_UNUSED)
{
	expect(record->key_len == 16 && record->val != NULL && !record->ghost,
	    "entry hook receives a complete record");
	if (leaf) {
		leaf_entry_calls++;
		expect(record->val_len == 16, "leaf hook receives leaf value");
	} else {
		index_entry_calls++;
		expect(record->val_len == 8,
		    "index hook receives child pointer");
	}
	return (FSIMG_OK);
}

static void
test_entry_hook_distinguishes_index_and_leaf_entries(void)
{
	apfs_tree_profile_t profile = test_profile;
	apfs_cursor_t cursor;
	fixture_t f;
	uint64_t root;
	fsimg_status_t status;
	bool found;

	scenario =
	    "entry hook receives index and leaf records in height three tree";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	root = build_tree(&f, 16, 4, 2, false, 0);
	profile.entry_check = count_entry_kinds;
	index_entry_calls = leaf_entry_calls = 0;
	apfs_cursor_init(&cursor, &f.container, &profile,
	    APFS_OBJ_PHYSICAL, root, 10, NULL);

	status = apfs_cursor_first(&cursor, &found, &f.error);
	while (status == FSIMG_OK && found)
		status = apfs_cursor_next(&cursor, &found, &f.error);
	status_is(&f, status, FSIMG_OK);
	expect(index_entry_calls == 6, "hook checks all six index entries");
	expect(leaf_entry_calls == 16, "hook checks all sixteen leaf entries");
	apfs_cursor_free(&cursor);
	fixture_close(&f);
}

static void
test_loaded_map_lookup_exhausts_work_budget(void)
{
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_map_t map;
	fixture_t f;
	fsimg_status_t status = FSIMG_OK;
	uint64_t reads;
	bool found;

	scenario = "repeated lookup in a loaded leaf exhausts work budget";
	fixture_init(&f, 256);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	build_tree(&f, 1, 1, 2, false, 0);
	CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
	CHECK(apfs_map_lookup(&map, 1000, 10, &found, &key, &value,
	    &f.error) == FSIMG_OK);
	expect(found, "first lookup succeeds before budget exhaustion");
	reads = f.read_calls;

	for (unsigned i = 0; i < 256 && status == FSIMG_OK; i++)
		status = apfs_map_lookup(&map, 1000, 10, &found, &key, &value,
		    &f.error);
	status_is(&f, status, FSIMG_ELIMIT);
	expect(f.read_calls == reads, "repeated lookups use loaded windows");
	apfs_map_close(&map);
	fixture_close(&f);
}

static void
test_cursor_seek_rejects_key_lengths_outside_profile_bounds(void)
{
	static const size_t lengths[] = { 0, 15, 17 };
	static const apfs_seek_t directions[] = { APFS_SEEK_LE, APFS_SEEK_GE };
	unsigned char key[17] = { 0 };
	apfs_cursor_t cursor;
	fixture_t f;
	uint64_t root, reads;
	bool found;

	for (unsigned loaded = 0; loaded < 2; loaded++) {
		for (size_t d = 0;
		    d < sizeof(directions) / sizeof(directions[0]); d++) {
			for (size_t i = 0;
			    i < sizeof(lengths) / sizeof(lengths[0]); i++) {
				scenario = loaded ?
				    "invalid seek length on loaded cursor" :
				    "invalid seek length before root load";
				fixture_init(&f, 0);
				build_checkpoints(&f, 1, 10);
				CHECK(checkpoint_open(&f) == FSIMG_OK);
				root = build_tree(&f, 1, 1, 2, false, 0);
				apfs_cursor_init(&cursor, &f.container,
				    &test_profile,
				    APFS_OBJ_PHYSICAL, root, 10, NULL);
				if (loaded)
					CHECK(apfs_cursor_first(&cursor,
					    &found, &f.error) ==
					    FSIMG_OK && found);
				reads = f.read_calls;
				found = true;
				status_is(&f, apfs_cursor_seek(&cursor, key,
				    lengths[i],
				    directions[d], &found,
				    &f.error), FSIMG_EINVAL);
				expect(!found && !cursor.positioned,
				    "invalid seek leaves no current record");
				expect(f.read_calls == reads,
				    "invalid seek performs no I/O");
				apfs_cursor_free(&cursor);
				fixture_close(&f);
			}
		}
	}
}

static void
test_virtual_object_size_is_rejected_before_reading(void)
{
	unsigned char window[BLOCK];
	apfs_ref_t ref;
	apfs_map_t map;
	fixture_t f;
	uint64_t reads;
	bool found;

	scenario = "two-block virtual object refused before reading past slice";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	build_tree(&f, 1, 1, 2, false, 0);
	mutate(&f, &(mutation_t){ .block = TREE_BASE,
	    .offset = OMAP_VALUE_SIZE_OFFSET,
	    .value = 2 * BLOCK, .width = 4 });
	CHECK(apfs_gen_object_build(block_at(&f, OBJECT_BLOCK), 2 * BLOCK,
	    &(apfs_gen_object_t){ .oid = 1000, .xid = 5, .type = 13 }));
	fixture_truncate(&f, 801 * BLOCK);
	CHECK(map_open(&f, &map, APFS_MAP_CONTAINER) == FSIMG_OK);
	/* Cache the map leaf so only the object's read could invoke I/O. */
	CHECK(apfs_cursor_first(&map.cursor, &found, &f.error) == FSIMG_OK);
	CHECK(found);
	reads = f.read_calls;
	ref = (apfs_ref_t){ .role = "virtual test", .oid = 1000, .xid = 10,
	    .kind = 13, .map = &map, .storage = APFS_OBJ_VIRTUAL };
	if (status_is(&f, apfs_object_read(&f.container, &ref, window,
	    NULL, &f.error), FSIMG_ECORRUPT))
		expect(strstr(f.error.message, "wrong size") != NULL,
		    "object size fails before slice truncation");
	expect(f.read_calls == reads, "wrong object size performs no I/O");
	apfs_map_close(&map);
	fixture_close(&f);
}

static void
test_virtual_objects_must_match_map_key_xid(void)
{
	unsigned char window[BLOCK];
	apfs_obj_t header;
	apfs_ref_t ref;
	apfs_map_t map;
	fixture_t f;

	static const struct {
		const char *name;
		uint64_t object_xid, oid;
		fsimg_status_t status;
	} cases[] = {
		{ .name = "virtual object matches found key XID",
		    .object_xid = 5,
		    .oid = 1000, .status = FSIMG_OK },
		{ .name = "virtual object differs from found key XID",
		    .object_xid = 4,
		    .oid = 1000, .status = FSIMG_ECORRUPT },
		{ .name = "virtual object missing from volume map",
		    .object_xid = 5,
		    .oid = 1001, .status = FSIMG_ECORRUPT }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		build_tree(&f, 1, 1, 2, false, 0);
		mutate(&f, &(mutation_t){ .block = MAP_BLOCK,
		    .offset = OMAP_FLAGS_OFFSET,
		    .value = 0, .width = 4 });
		CHECK(apfs_gen_object_build(block_at(&f, OBJECT_BLOCK), BLOCK,
		    &(apfs_gen_object_t){ .oid = 1000,
		    .xid = cases[i].object_xid,
		    .type = 13 }));
		CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
		ref = (apfs_ref_t){ .role = "virtual test",
		    .oid = cases[i].oid, .xid = 10, .kind = 13, .map = &map,
		    .storage = APFS_OBJ_VIRTUAL };
		status_is(&f, apfs_object_read(&f.container, &ref, window,
		    &header, &f.error), cases[i].status);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_reaper_idle_shapes_and_malformed_lists(void)
{
	static const struct {
		mutation_t mutation;
		uint64_t head;
		uint32_t flags, entries;
		fsimg_status_t status;
		bool idle;
	} cases[] = {
		{ .mutation = { .name = "head with zero count",
		    .block = DATA_REAPER, .offset = REAPER_COUNT_OFFSET,
		    .value = 0, .width = 4 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "count with zero head",
		    .block = DATA_REAPER, .offset = REAPER_HEAD_OFFSET,
		    .value = 0, .width = 8 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "list ends before object count",
		    .block = DATA_REAPER, .offset = REAPER_COUNT_OFFSET,
		    .value = 2, .width = 4 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "list runs past object count",
		    .block = DATA_REAP_LIST, .offset = REAP_LIST_NEXT_OFFSET,
		    .value = 104, .width = 8 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "tail is not last list object",
		    .block = DATA_REAPER, .offset = REAPER_TAIL_OFFSET,
		    .value = 100, .width = 8 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "list reference names wrong kind",
		    .block = DATA_REAPER, .offset = REAPER_HEAD_OFFSET,
		    .value = 100, .width = 8 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "reaper state extends beyond object",
		    .block = DATA_REAPER, .offset = REAPER_STATE_SIZE_OFFSET,
		    .value = BLOCK, .width = 4 },
		    .head = 104, .flags = 1, .status = FSIMG_ECORRUPT },
		{ .mutation = { .name = "zero-shape idle reaper" },
		    .flags = 1, .status = FSIMG_OK, .idle = true },
		{ .mutation = { .name = "one empty reap-list object is idle" },
		    .head = 104, .flags = 1, .status = FSIMG_OK, .idle = true },
		{ .mutation = { .name = "reap list with entries is not idle" },
		    .head = 104, .flags = 1, .entries = 1, .status = FSIMG_OK },
		{ .mutation = { .name = "reaping flag is not idle" },
		    .head = 104, .flags = 3, .status = FSIMG_OK }
	};
	unsigned char window[BLOCK];
	apfs_gen_mapping_t mappings[3] = {
		{ .type = 0x80000005, .size = BLOCK, .oid = 100,
		    .address = 32 },
		{ .type = 0x80000011, .size = BLOCK, .oid = 102,
		    .address = 33 },
		{ .type = 0x80000012, .size = BLOCK, .oid = 104, .address = 34 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
		.mappings = mappings, .mapping_count = 3, .mapping_blocks = 1,
		.data_len = 3, .data_next = 3, .desc_next = 2 };
	apfs_gen_container_t image = {
		.nx = { .magic = 0x4253584e, .block_size = BLOCK,
		.block_count = BLOCKS, .uuid = { 1 }, .desc_base = 1,
		.desc_blocks = DESCRIPTOR_SLOTS, .data_base = DATA_SPACEMAN,
		    .data_blocks = 300,
		.max_volumes = 100, .spaceman_oid = 100, .reaper_oid = 102,
		    .omap_oid = MAP_BLOCK },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	apfs_gen_reap_entry_t entry = { .next = UINT32_MAX, .kind = 13 };
	apfs_gen_reap_list_t list;
	apfs_gen_reaper_t reaper;
	fixture_t f;
	bool idle;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].mutation.name;
		fixture_init(&f, 0);
		CHECK(apfs_gen_container_build(f.bytes, IMAGE_BYTES, &image));
		build_container_map(&f);
		bzero(block_at(&f, DATA_REAPER), BLOCK);
		bzero(block_at(&f, DATA_REAP_LIST), BLOCK);
		reaper = (apfs_gen_reaper_t){ .object = { .oid = 102,
		    .xid = 10, .type = 0x80000011 }, .head = cases[i].head,
		    .tail = cases[i].head,
		    .rlcount = cases[i].head == 0 ? 0 : 1,
		    .flags = cases[i].flags };
		list = (apfs_gen_reap_list_t){ .object = { .oid = 104,
		    .xid = 10, .type = 0x80000012 }, .capacity = 8,
		    .count = cases[i].entries,
		    .first = cases[i].entries != 0 ? 0 : UINT32_MAX,
		    .last = cases[i].entries != 0 ? 0 : UINT32_MAX,
		    .entries = &entry };
		CHECK(apfs_gen_reaper_build(block_at(&f, DATA_REAPER), BLOCK,
		    &reaper));
		CHECK(apfs_gen_reap_list_build(block_at(&f, DATA_REAP_LIST),
		    BLOCK, &list));
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		if (cases[i].status != FSIMG_OK)
			mutate(&f, &cases[i].mutation);
		if (status_is(&f, apfs_reaper_idle(&f.container, window, &idle,
		    &f.error), cases[i].status) && cases[i].status == FSIMG_OK)
			expect(idle == cases[i].idle, "reaper idle result");
		fixture_close(&f);
	}
}

static void
test_reap_list_codec_and_generator_reject_out_of_bounds_arrays(void)
{
	unsigned char raw[BLOCK] = { 0 }, encoded[BLOCK] = { 0 };
	unsigned char sentinel[BLOCK];
	apfs_gen_reap_entry_t entry = { .next = 7, .flags = 3, .kind = 13,
	    .size = BLOCK, .volume_oid = 42, .oid = 99, .xid = 20 };
	apfs_gen_reap_list_t generated = {
		.object = { .oid = 104, .xid = 10, .type = 0x80000012 },
		.entries = &entry, .next = 106, .flags = 3, .capacity = 8,
		.count = 1, .first = 5, .last = 6, .free = 7
	};
	apfs_reap_list_t decoded, again;

	scenario = "reap-list codec fields, bounds and untouched failures";
	CHECK(apfs_gen_reap_list_build(raw, sizeof(raw), &generated));
	CHECK(apfs_reap_list_decode(raw, sizeof(raw), &decoded));
	expect(decoded.next == 106 && decoded.flags == 3 &&
	    decoded.capacity == 8 && decoded.count == 1 && decoded.first == 5 &&
	    decoded.last == 6 && decoded.free == 7,
	    "all reap-list fields decoded");
	CHECK(apfs_reap_list_encode(encoded, sizeof(encoded), &decoded));
	expect(memcmp(raw + 32, encoded + 32, 32) == 0, "codec header bytes");
	expect(fsimg_load_le32(raw + 64) == 7 &&
	    fsimg_load_le64(raw + 96) == 20, "independent entry layout");
	for (size_t length = 0; length < 64; length++) {
		expect(!apfs_reap_list_decode(raw, length, &again),
		    "short list rejected");
		expect(!apfs_reap_list_encode(encoded, length, &decoded),
		    "short encoding rejected");
	}
	bcopy(encoded, sentinel, sizeof(sentinel));
	decoded.count = decoded.capacity + 1;
	expect(!apfs_reap_list_encode(encoded, sizeof(encoded), &decoded),
	    "entry count above capacity rejected");
	expect(memcmp(encoded, sentinel, sizeof(encoded)) == 0,
	    "invalid encoding leaves buffer untouched");
	generated.capacity = BLOCK;
	bcopy(raw, sentinel, sizeof(sentinel));
	expect(!apfs_gen_reap_list_build(raw, sizeof(raw), &generated),
	    "generator refuses excessive capacity");
	expect(memcmp(raw, sentinel, sizeof(raw)) == 0,
	    "invalid generator parameters leave buffer untouched");
	fsimg_store_le32(raw + 44, BLOCK);
	expect(!apfs_reap_list_decode(raw, sizeof(raw), &again),
	    "decoder refuses capacity beyond object");
	fsimg_store_le32(raw + 44, 0);
	expect(!apfs_reap_list_decode(raw, sizeof(raw), &again),
	    "decoder refuses count above capacity");
}

static void
test_assemblers_leave_buffers_untouched_on_bad_parameters(void)
{
	apfs_gen_mapping_t mappings[2] = {
		{ .type = 0x80000005, .size = BLOCK, .oid = 100,
		    .address = 32 },
		{ .type = 0x80000011, .size = BLOCK + 1, .oid = 102,
		    .address = 33 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
		.mappings = mappings, .mapping_count = 2, .mapping_blocks = 1 };
	apfs_gen_container_t image = {
		.nx = { .block_size = BLOCK, .block_count = BLOCKS,
		.desc_base = 1, .desc_blocks = DESCRIPTOR_SLOTS,
		    .data_base = DATA_SPACEMAN,
		.data_blocks = 300 }, .runs = &run, .count = 1
	};
	apfs_gen_omap_record_t records[4] = {
		{ .key = { .oid = 1, .xid = 1 } },
		{ .key = { .oid = 2, .xid = 1 } },
		{ .key = { .oid = 3, .xid = 1 } },
		{ .key = { .oid = 4, .xid = 1 } }
	};
	apfs_gen_tree_t tree = { .records = records, .count = 4,
		.base = TREE_BASE, .xid = 10, .block_size = BLOCK,
		.records_per_leaf = 1, .children_per_index = 2,
		    .toc_capacity = 4 };
	unsigned char *before;
	uint64_t root = 99, blocks = 99;
	fixture_t f;

	scenario = "assembler failures preserve borrowed buffers and outputs";
	fixture_init(&f, 0);
	memset(f.bytes, 0xa5, IMAGE_BYTES);
	before = malloc(IMAGE_BYTES);
	CHECK(before != NULL);
	bcopy(f.bytes, before, IMAGE_BYTES);
	expect(!apfs_gen_container_build(f.bytes, IMAGE_BYTES, &image),
	    "invalid last mapping refused before emission");
	expect(memcmp(f.bytes, before, IMAGE_BYTES) == 0,
	    "container assembler leaves all bytes untouched");
	/* Leaf TOCs fit; the index needs eight bytes. No leaf may escape. */
	expect(!apfs_gen_tree_build(f.bytes, IMAGE_BYTES, &tree, &root,
	    &blocks),
	    "index TOC too small refuses entire tree");
	expect(memcmp(f.bytes, before,
	    IMAGE_BYTES) == 0 && root == 99 && blocks == 99,
	    "tree assembler publishes nothing on failure");
	free(before);
	fixture_close(&f);
}

static void
test_work_and_memory_limits_release_partial_open(void)
{
	fixture_t f;
	fsimg_stats_t stats;
	void *reservation;

	scenario = "checkpoint work limit";
	fixture_init(&f, 1);
	build_checkpoints(&f, 1, 10);
	status_is(&f, checkpoint_open(&f), FSIMG_ELIMIT);
	fixture_close(&f);

	scenario = "checkpoint memory budget exhausted";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	/* Contexts require 16 MiB; occupy it to leave less than one window. */
	reservation = fsimg_alloc(f.ctx,
	    16 * 1024 * 1024 - (size_t)stats.memory_bytes - 1024, &f.error);
	CHECK(reservation != NULL);
	status_is(&f, checkpoint_open(&f), FSIMG_ELIMIT);
	fsimg_dealloc(reservation);
	fixture_close(&f);
}

/* Key descriptions keep record intent separate from its byte encoding. */
typedef struct fs_key_case {
	uint64_t owner;
	uint64_t number;
	const char *name;
	uint8_t kind;
	bool header_only;
} fs_key_case_t;

static apfs_gen_record_t
fs_key_build(unsigned char *raw, const fs_key_case_t *key, bool hashed)
{
	apfs_gen_drec_key_t drec;
	apfs_gen_name_key_t name;
	size_t length = 8;

	CHECK(apfs_gen_jkey_build(raw, 8, key->owner, key->kind));
	if (!key->header_only) {
		switch (key->kind) {
		case 4:
			name = (apfs_gen_name_key_t){ .oid = key->owner,
			    .name = key->name,
			    .name_len = (uint16_t)strlen(key->name) };
			length = 10 + name.name_len + 1;
			CHECK(apfs_gen_xattr_key_build(raw, length, &name));
			break;
		case 5:
			length = 16;
			CHECK(apfs_gen_sibling_key_build(raw, length,
			    key->owner, key->number));
			break;
		case 8:
			length = 16;
			CHECK(apfs_gen_extent_key_build(raw, length,
			    key->owner, key->number));
			break;
		case 9:
			drec = (apfs_gen_drec_key_t){ .parent = key->owner,
			    .hash = (uint32_t)key->number, .name = key->name,
			    .name_len = (uint16_t)strlen(key->name),
			    .hashed = hashed };
			length = (hashed ? 12 : 10) + drec.name_len + 1;
			CHECK(apfs_gen_drec_key_build(raw, length, &drec));
			break;
		case 13:
			length = 16;
			CHECK(apfs_gen_file_info_key_build(raw, length,
			    key->owner, key->number & UINT64_C(0xffffffffffffff),
			    (uint8_t)(key->number >> 56)));
			break;
		default:
			break;
		}
	}
	return ((apfs_gen_record_t){ .key = raw, .key_size = (uint16_t)length });
}

/*
 * The oracle reads integers byte by byte and compares suffixes directly,
 * without the driver's projection structure or endian/record decoders.
 * It also accepts header-only search keys, which sort at or before stored keys.
 */
static uint64_t
fs_test_integer(const unsigned char *bytes, size_t width)
{
	uint64_t value = 0;

	for (size_t i = 0; i < width; i++)
		value += (uint64_t)bytes[i] << (8 * i);
	return (value);
}

static int
fs_test_compare(const unsigned char *a, size_t a_len,
    const unsigned char *b, size_t b_len, bool hashed)
{
	uint64_t ah = fs_test_integer(a, 8), bh = fs_test_integer(b, 8);
	uint64_t ao = ah % (UINT64_C(1) << 60);
	uint64_t bo = bh % (UINT64_C(1) << 60), av, bv;
	unsigned kind = (unsigned)(ah >> 60);
	size_t start;

	if (ao != bo)
		return (ao < bo ? -1 : 1);
	if (kind != bh >> 60)
		return (kind < (bh >> 60) ? -1 : 1);
	if (kind == 5 || kind == 8 || kind == 13) {
		av = a_len < 16 ? 0 : fs_test_integer(a + 8, 8);
		bv = b_len < 16 ? 0 : fs_test_integer(b + 8, 8);
		return (av < bv ? -1 : av > bv);
	}
	if (kind == 9 && hashed) {
		av = a_len < 12 ? 0 : fs_test_integer(a + 8, 4) / 1024;
		bv = b_len < 12 ? 0 : fs_test_integer(b + 8, 4) / 1024;
		if (av != bv)
			return (av < bv ? -1 : 1);
		start = 12;
	} else if (kind == 4) {
		start = 10;
	} else {
		return (0);
	}
	for (size_t i = start; i < a_len && i < b_len; i++)
		if (a[i] != b[i])
			return (a[i] < b[i] ? -1 : 1);
	return (a_len < b_len ? -1 : a_len > b_len);
}

static void
test_filesystem_comparator_orders_owners_kinds_and_suffixes(void)
{
	static const struct {
		const char *name;
		fs_key_case_t left;
		fs_key_case_t right;
		int hashed_order;
		int plain_order;
	} cases[] = {
		{ "owner precedes kind", { .owner = 1, .kind = 12 },
		    { .owner = 2, .kind = 3 }, -1, -1 },
		{ "sixty bit owner", { .owner = GEN_JKEY_OID_MASK - 1,
		    .kind = 12 }, { .owner = GEN_JKEY_OID_MASK, .kind = 3 }, -1, -1 },
		{ "extent offsets are numeric", { .owner = 2, .kind = 8,
		    .number = 0xff }, { .owner = 2, .kind = 8, .number = 0x100 }, -1, -1 },
		{ "sibling IDs are numeric", { .owner = 2, .kind = 5,
		    .number = 0xff }, { .owner = 2, .kind = 5, .number = 0x100 }, -1, -1 },
		{ "file info packed word is numeric", { .owner = 2, .kind = 13,
		    .number = 0xff }, { .owner = 2, .kind = 13, .number = 0x100 }, -1, -1 },
		{ "file info includes type bits", { .owner = 2, .kind = 13,
		    .number = UINT64_C(0x00ffffffffffffff) },
		    { .owner = 2, .kind = 13,
		    .number = UINT64_C(0x0100000000000000) }, -1, -1 },
		{ "sibling header first", { .owner = 2, .kind = 5,
		    .header_only = true }, { .owner = 2, .kind = 5,
		    .number = 1 }, -1, -1 },
		{ "file info header first", { .owner = 2, .kind = 13,
		    .header_only = true }, { .owner = 2, .kind = 13,
		    .number = 1 }, -1, -1 },
		{ "xattr header first", { .owner = 2, .kind = 4,
		    .header_only = true }, { .owner = 2, .kind = 4,
		    .name = "ab" }, -1, -1 },
		{ "hash precedes name", { .owner = 2, .kind = 9,
		    .number = 0xff, .name = "z" }, { .owner = 2, .kind = 9,
		    .number = 0x100, .name = "a" }, -1, 0 },
		{ "hash tie uses name", { .owner = 2, .kind = 9,
		    .number = 7, .name = "ab" }, { .owner = 2, .kind = 9,
		    .number = 7, .name = "abc" }, -1, 0 },
		{ "length bits do not precede name", { .owner = 2, .kind = 9,
		    .number = 7, .name = "aaaa" }, { .owner = 2, .kind = 9,
		    .number = 7, .name = "z" }, -1, 0 },
		{ "names use unsigned bytes", { .owner = 2, .kind = 9,
		    .number = 7, .name = "z" }, { .owner = 2, .kind = 9,
		    .number = 7, .name = "\xc3" }, -1, 0 },
		{ "xattr prefix first", { .owner = 2, .kind = 4, .name = "ab" },
		    { .owner = 2, .kind = 4, .name = "abc" }, -1, -1 },
		{ "xattr unsigned bytes", { .owner = 2, .kind = 4, .name = "z" },
		    { .owner = 2, .kind = 4, .name = "\xc3" }, -1, -1 },
		{ "extent header first", { .owner = 2, .kind = 8,
		    .header_only = true }, { .owner = 2, .kind = 8,
		    .number = 1 }, -1, -1 },
		{ "search header follows previous kind", { .owner = 2, .kind = 8,
		    .number = 1 }, { .owner = 2, .kind = 9,
		    .header_only = true }, -1, -1 },
		{ "directory header starts class", { .owner = 2, .kind = 9,
		    .header_only = true }, { .owner = 2, .kind = 9,
		    .number = 7, .name = "z" }, -1, 0 }
	};
	static const uint8_t kinds[] = { 3, 4, 5, 6, 8, 9, 10, 12, 13 };
	unsigned char keys[2 * (sizeof(cases) / sizeof(cases[0]))][64];
	apfs_gen_record_t records[sizeof(keys) / sizeof(keys[0])];
	unsigned char kind_keys[sizeof(kinds)][64];
	apfs_gen_record_t kind_records[sizeof(kinds)];
	const apfs_tree_profile_t *profile;
	char pair[256];
	int order, reverse;

	for (unsigned hashed = 0; hashed < 2; hashed++) {
		profile = apfs_fs_tree_profile(hashed != 0);
		scenario = "filesystem profile shape";
		expect(profile->subtype == 14 && profile->key_min == 8 &&
		    profile->fixed_key == 0 && profile->fixed_val == 0 &&
		    strcmp(profile->role, "filesystem tree") == 0,
		    "variable-size filesystem profile");
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			scenario = cases[i].name;
			records[2 * i] = fs_key_build(keys[2 * i],
			    &cases[i].left, hashed != 0);
			records[2 * i + 1] = fs_key_build(keys[2 * i + 1],
			    &cases[i].right, hashed != 0);
			order = profile->compare(records[2 * i].key,
			    records[2 * i].key_size, records[2 * i + 1].key,
			    records[2 * i + 1].key_size);
			order = (order > 0) - (order < 0);
			expect(order == (hashed ? cases[i].hashed_order :
			    cases[i].plain_order), "expected key order");
		}
		for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
			for (size_t j = 0; j < sizeof(records) / sizeof(records[0]); j++) {
				CHECK(snprintf(pair, sizeof(pair),
				    "%s comparator pair %zu (%s) / %zu (%s)",
				    hashed ? "hashed" : "plain", i, cases[i / 2].name,
				    j, cases[j / 2].name) < (int)sizeof(pair));
				scenario = pair;
				order = profile->compare(records[i].key,
				    records[i].key_size, records[j].key,
				    records[j].key_size);
				reverse = profile->compare(records[j].key,
				    records[j].key_size, records[i].key,
				    records[i].key_size);
				order = (order > 0) - (order < 0);
				reverse = (reverse > 0) - (reverse < 0);
				expect(order == -reverse, "pair is antisymmetric");
				expect(order == fs_test_compare(records[i].key,
				    records[i].key_size, records[j].key,
				    records[j].key_size, hashed != 0),
				    "independent oracle agrees");
			}
		}
		scenario = "kind order within one owner";
		for (size_t i = 0; i < sizeof(kinds); i++)
			kind_records[i] = fs_key_build(kind_keys[i],
			    &(fs_key_case_t){ .owner = 2, .kind = kinds[i],
			    .name = "a", .number = 1 }, hashed != 0);
		for (size_t i = 1; i < sizeof(kinds); i++)
			expect(profile->compare(kind_records[i - 1].key,
			    kind_records[i - 1].key_size, kind_records[i].key,
			    kind_records[i].key_size) < 0, "kinds increase");
	}
}

static void
test_hashed_comparator_ignores_name_length_bits(void)
{
	unsigned char original[64], different_length[64];
	apfs_gen_record_t record;
	const apfs_tree_profile_t *profile = apfs_fs_tree_profile(true);

	scenario = "hashed word length bits have no ordering significance";
	record = fs_key_build(original, &(fs_key_case_t){ .owner = 42,
	    .kind = 9, .number = 7, .name = "ab" }, true);
	bcopy(original, different_length, record.key_size);
	/* Deliberately inconsistent length: comparison is total even for search
	 * keys that the entry checker would refuse as stored records.
	 */
	different_length[8] ^= 1;
	expect(profile->compare(original, record.key_size, different_length,
	    record.key_size) == 0, "same hash and name compare equal");
	expect(profile->compare(different_length, record.key_size, original,
	    record.key_size) == 0, "length bits ignored in both directions");
}

/* Tree nodes use XID 5; the cursor and physical volume map use XID 10. */
static uint64_t
build_fs_tree(fixture_t *f, const apfs_gen_record_t *records, size_t count,
    unsigned per_leaf, bool newer_version)
{
	apfs_gen_omap_record_t mappings[128], versions[129];
	apfs_gen_fs_tree_t tree = { .records = records, .count = count,
	    .first_oid = 2000, .base = OBJECT_BLOCK, .xid = 5,
	    .block_size = BLOCK, .records_per_leaf = per_leaf,
	    .children_per_index = 2 };
	apfs_gen_tree_t map_tree;
	apfs_gen_omap_t map;
	uint64_t root, blocks, map_root, map_blocks;
	size_t n = 0;

	CHECK(apfs_gen_fs_tree_build(f->bytes, IMAGE_BYTES, &tree,
	    mappings, 128, &root, &blocks));
	for (size_t i = 0; i < blocks; i++) {
		versions[n++] = mappings[i];
		if (newer_version && i == 0) {
			versions[n] = mappings[i];
			versions[n].key.xid = 11;
			/* A zero block makes accidental use of the future version fail. */
			versions[n].value.address = OBJECT_BLOCK + 128;
			n++;
		}
	}
	map_tree = (apfs_gen_tree_t){ .records = versions, .count = n,
	    .base = TREE_BASE, .xid = 10, .block_size = BLOCK,
	    .records_per_leaf = 4, .children_per_index = 2 };
	CHECK(apfs_gen_tree_build(f->bytes, IMAGE_BYTES, &map_tree,
	    &map_root, &map_blocks));
	bzero(block_at(f, MAP_BLOCK), BLOCK);
	map = (apfs_gen_omap_t){ .object = { .oid = MAP_BLOCK, .xid = 10,
	    .type = 0x4000000b }, .tree_type = 0x40000002,
	    .tree_oid = map_root };
	CHECK(apfs_gen_omap_build(block_at(f, MAP_BLOCK), BLOCK, &map));
	return (root);
}

static void
expect_fs_record(apfs_cursor_t *cursor, const apfs_gen_record_t *wanted)
{
	apfs_record_t record;

	apfs_cursor_record(cursor, &record);
	expect(record.key != NULL && record.key_len == wanted->key_size &&
	    bcmp(record.key, wanted->key, wanted->key_size) == 0,
	    "cursor lands on exact stored key");
	expect(record.val_len == wanted->value_size && !record.ghost &&
	    (record.val_len == 0 || bcmp(record.val, wanted->value,
	    record.val_len) == 0), "cursor returns matching value");
}

static void
expect_fs_seek(fixture_t *f, apfs_cursor_t *cursor,
    const apfs_gen_record_t *key, apfs_seek_t mode,
    const apfs_gen_record_t *wanted)
{
	bool found;

	if (status_is(f, apfs_cursor_seek(cursor, key->key, key->key_size,
	    mode, &found, &f->error), FSIMG_OK)) {
		expect(found, "seek finds a record");
		if (found)
			expect_fs_record(cursor, wanted);
	}
}

static void
test_plain_directory_class_spans_leaves_and_seeks_to_both_edges(void)
{
	static const fs_key_case_t keys_in_order[] = {
		{ .owner = 42, .kind = 3 },
		{ .owner = 42, .kind = 9, .name = "z" },
		{ .owner = 42, .kind = 9, .name = "b" },
		{ .owner = 42, .kind = 9, .name = "y" },
		{ .owner = 42, .kind = 9, .name = "a" },
		{ .owner = 42, .kind = 9, .name = "x" },
		{ .owner = 42, .kind = 9, .name = "c" },
		{ .owner = 42, .kind = 9, .name = "w" },
		{ .owner = 42, .kind = 9, .name = "d" },
		{ .owner = 42, .kind = 9, .name = "v" },
		{ .owner = 42, .kind = 9, .name = "e" },
		{ .owner = 42, .kind = 10 },
	};
	static const size_t outside_directory[] = { 0, 11 };
	unsigned char keys[12][64], header[8], any_name[64];
	unsigned char values[12];
	apfs_gen_record_t records[12], search;
	apfs_cursor_t cursor;
	apfs_map_t map;
	fixture_t f;
	uint64_t root;
	size_t visited = 0;
	fsimg_status_t status;
	bool found;

	scenario = "plain directory class spans leaves at older mapped XID";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	for (size_t i = 0; i < 12; i++) {
		records[i] = fs_key_build(keys[i], &keys_in_order[i], false);
		values[i] = (unsigned char)i;
		records[i].value = &values[i];
		records[i].value_size = 1;
	}
	root = build_fs_tree(&f, records, 12, 2, true);
	CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
	apfs_cursor_init(&cursor, &f.container, apfs_fs_tree_profile(false),
	    APFS_OBJ_VIRTUAL, root, 10, &map);
	status = apfs_cursor_first(&cursor, &found, &f.error);
	while (status == FSIMG_OK && found) {
		CHECK(visited < 12);
		expect_fs_record(&cursor, &records[visited++]);
		status = apfs_cursor_next(&cursor, &found, &f.error);
	}
	status_is(&f, status, FSIMG_OK);
	expect(visited == 12, "walk visits each record once in stored order");
	expect(cursor.path[0].node.level >= 2, "tree has at least three levels");
	expect(cursor.info.key_count == 12, "root counts leaf records");
	search = fs_key_build(header, &(fs_key_case_t){ .owner = 42,
	    .kind = 9, .header_only = true }, false);
	expect_fs_seek(&f, &cursor, &search, APFS_SEEK_GE, &records[1]);
	expect_fs_seek(&f, &cursor, &search, APFS_SEEK_LE, &records[10]);
	search = fs_key_build(any_name, &(fs_key_case_t){ .owner = 42,
	    .kind = 9, .name = "any name" }, false);
	expect_fs_seek(&f, &cursor, &search, APFS_SEEK_GE, &records[1]);
	expect_fs_seek(&f, &cursor, &search, APFS_SEEK_LE, &records[10]);
	for (size_t edge = 0; edge < 2; edge++) {
		size_t i = outside_directory[edge];

		expect_fs_seek(&f, &cursor, &records[i], APFS_SEEK_GE, &records[i]);
		expect_fs_seek(&f, &cursor, &records[i], APFS_SEEK_LE, &records[i]);
	}
	apfs_cursor_free(&cursor);
	apfs_map_close(&map);
	fixture_close(&f);
}

/* Replace the two-child root without relying on encoded TOC offsets. */
static void
replace_fs_root(fixture_t *f, uint64_t root,
    const apfs_gen_record_t *left, const apfs_gen_record_t *right)
{
	unsigned char children[2][8];
	apfs_gen_record_t separators[2] = { *left, *right };
	apfs_gen_node_t node;
	unsigned char *raw = block_at(f, OBJECT_BLOCK + root - 2000);

	for (unsigned i = 0; i < 2; i++) {
		CHECK(apfs_gen_sibling_map_val_build(children[i], 8, 2000 + i));
		separators[i].value = children[i];
		separators[i].value_size = 8;
	}
	node = (apfs_gen_node_t){ .object = { .oid = root, .xid = 5,
	    .type = 2, .subtype = 14 }, .flags = GEN_APFS_ROOT, .level = 1,
	    .records = separators, .count = 2,
	    .info = { .flags = 0x40, .node_size = BLOCK,
	    .longest_key = 64, .longest_val = 8, .key_count = 4,
	    .node_count = 3 } };
	bzero(raw, BLOCK);
	CHECK(apfs_gen_node_build(raw, BLOCK, &node));
}

typedef enum fs_tree_damage {
	FS_DUPLICATE_LEAF,
	FS_DUPLICATE_BOUNDARY,
	FS_DUPLICATE_INODE,
	FS_REVERSED_LEAF,
	FS_REVERSED_BOUNDARY,
	FS_BELOW_SEPARATOR,
	FS_BAD_INDEX_KIND,
	FS_VALID_ROOT
} fs_tree_damage_t;

static void
test_filesystem_trees_refuse_duplicates_and_decreasing_keys(void)
{
	static const struct {
		const char *name;
		fs_tree_damage_t damage;
		unsigned per_leaf;
		bool hashed;
		const char *error;
		fs_key_case_t separator;
	} cases[] = {
		{ .name = "plain duplicate in one inode leaf",
		    .damage = FS_DUPLICATE_INODE, .per_leaf = 4, .hashed = false,
		    .error = "keys out of order" },
		{ .name = "plain owner decreases in one leaf",
		    .damage = FS_REVERSED_LEAF, .per_leaf = 4, .hashed = false,
		    .error = "keys out of order" },
		{ .name = "plain owner decreases across two leaves",
		    .damage = FS_REVERSED_BOUNDARY, .per_leaf = 2, .hashed = false,
		    .error = "lies outside its parent's separators" },
		{ .name = "plain left leaf below replaced separator",
		    .damage = FS_BELOW_SEPARATOR, .per_leaf = 2, .hashed = false,
		    .error = "lies outside its parent's separators",
		    .separator = { .owner = 43, .kind = 9, .name = "a", .number = 7 } },
		{ .name = "hashed duplicate in one leaf",
		    .damage = FS_DUPLICATE_LEAF, .per_leaf = 4, .hashed = true,
		    .error = "keys out of order" },
		{ .name = "hashed left leaf ends at right separator",
		    .damage = FS_DUPLICATE_BOUNDARY, .per_leaf = 2, .hashed = true,
		    .error = "lies outside its parent's separators" },
		{ .name = "hashed owner decreases in one leaf",
		    .damage = FS_REVERSED_LEAF, .per_leaf = 4, .hashed = true,
		    .error = "keys out of order" },
		{ .name = "hashed owner decreases across two leaves",
		    .damage = FS_REVERSED_BOUNDARY, .per_leaf = 2, .hashed = true,
		    .error = "lies outside its parent's separators" },
		{ .name = "hashed left leaf below replaced separator",
		    .damage = FS_BELOW_SEPARATOR, .per_leaf = 2, .hashed = true,
		    .error = "lies outside its parent's separators",
		    .separator = { .owner = 43, .kind = 9, .name = "a", .number = 7 } },
		{ .name = "plain foreign kind in two-child root",
		    .damage = FS_BAD_INDEX_KIND, .per_leaf = 2, .hashed = false,
		    .error = "kind outside the tree",
		    .separator = { .owner = 42, .kind = 2 } },
		{ .name = "hashed foreign kind in two-child root",
		    .damage = FS_BAD_INDEX_KIND, .per_leaf = 2, .hashed = true,
		    .error = "kind outside the tree",
		    .separator = { .owner = 42, .kind = 2 } },
		{ .name = "plain valid separators rebuilt over two leaves",
		    .damage = FS_VALID_ROOT, .per_leaf = 2, .hashed = false,
		    .error = NULL,
		    .separator = { .owner = 42, .kind = 9, .name = "a",
		    .number = 7 } },
		{ .name = "hashed valid separators rebuilt over two leaves",
		    .damage = FS_VALID_ROOT, .per_leaf = 2, .hashed = true,
		    .error = NULL,
		    .separator = { .owner = 42, .kind = 9, .name = "a",
		    .number = 7 } }
	};
	static const char *names[] = { "a", "b", "c", "d" };
	unsigned char keys[4][64], separator_key[64];
	apfs_gen_record_t records[4], separator;
	apfs_cursor_t cursor;
	apfs_map_t map;
	fixture_t f;
	uint64_t root;
	fsimg_status_t status;
	bool found;

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size_t visited = 0;

		scenario = cases[c].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		for (unsigned i = 0; i < 4; i++)
			records[i] = fs_key_build(keys[i], &(fs_key_case_t){
			    .owner = 42, .kind = 9, .name = names[i], .number = 7 },
			    cases[c].hashed);
		switch (cases[c].damage) {
		case FS_DUPLICATE_LEAF:
			records[1] = records[0];
			break;
		case FS_DUPLICATE_BOUNDARY:
			records[1] = records[2];
			break;
		case FS_DUPLICATE_INODE:
			records[0] = fs_key_build(keys[0], &(fs_key_case_t){
			    .owner = 42, .kind = 3 }, false);
			records[1] = records[0];
			break;
		case FS_REVERSED_LEAF:
		case FS_REVERSED_BOUNDARY:
			records[1] = fs_key_build(keys[1], &(fs_key_case_t){
			    .owner = 43, .kind = 9, .name = "b", .number = 7 },
			    cases[c].hashed);
			break;
		case FS_BELOW_SEPARATOR:
			for (unsigned i = 2; i < 4; i++)
				records[i] = fs_key_build(keys[i], &(fs_key_case_t){
				    .owner = 44, .kind = 9, .name = names[i],
				    .number = 7 }, cases[c].hashed);
			break;
		case FS_VALID_ROOT:
		case FS_BAD_INDEX_KIND:
			break;
		}
		root = build_fs_tree(&f, records, 4, cases[c].per_leaf, false);
		if (cases[c].damage == FS_BELOW_SEPARATOR ||
		    cases[c].damage == FS_BAD_INDEX_KIND ||
		    cases[c].damage == FS_VALID_ROOT) {
			separator = fs_key_build(separator_key, &cases[c].separator,
			    cases[c].hashed);
			replace_fs_root(&f, root, &separator, &records[2]);
		}
		CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
		apfs_cursor_init(&cursor, &f.container,
		    apfs_fs_tree_profile(cases[c].hashed), APFS_OBJ_VIRTUAL,
		    root, 10, &map);
		status = apfs_cursor_first(&cursor, &found, &f.error);
		while (status == FSIMG_OK && found) {
			CHECK(visited < 4);
			expect_fs_record(&cursor, &records[visited++]);
			status = apfs_cursor_next(&cursor, &found, &f.error);
		}
		if (cases[c].error != NULL) {
			status_is(&f, status, FSIMG_ECORRUPT);
			expect(strstr(f.error.message, cases[c].error) != NULL,
			    cases[c].error);
		} else {
			status_is(&f, status, FSIMG_OK);
			expect(visited == 4, "rebuilt valid root walks every record");
		}
		expect(!found && !cursor.positioned, "walk leaves no current record");
		apfs_cursor_free(&cursor);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_plain_directory_walk_accepts_identical_stored_keys(void)
{
	static const struct {
		const char *name;
		unsigned per_leaf;
		unsigned repeated;
	} cases[] = {
		{ "plain identical names adjacent in one leaf", 4, 0 },
		{ "plain child last identical to next separator", 2, 2 }
	};
	static const char *names[] = { "a", "b", "c", "d" };
	unsigned char keys[4][64], values[] = { 0, 1, 2, 3 };
	apfs_gen_record_t records[4];
	apfs_cursor_t cursor;
	apfs_map_t map;
	fixture_t f;
	uint64_t root;
	fsimg_status_t status;
	bool found;

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size_t visited = 0;

		scenario = cases[c].name;
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		for (unsigned i = 0; i < 4; i++)
			records[i] = fs_key_build(keys[i], &(fs_key_case_t){
			    .owner = 42, .kind = 9, .name = names[i] }, false);
		records[1] = records[cases[c].repeated];
		/* Distinct values prove that every stored duplicate is yielded. */
		for (unsigned i = 0; i < 4; i++) {
			records[i].value = &values[i];
			records[i].value_size = 1;
		}
		root = build_fs_tree(&f, records, 4, cases[c].per_leaf, false);
		CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
		apfs_cursor_init(&cursor, &f.container, apfs_fs_tree_profile(false),
		    APFS_OBJ_VIRTUAL, root, 10, &map);
		status = apfs_cursor_first(&cursor, &found, &f.error);
		while (status == FSIMG_OK && found) {
			CHECK(visited < 4);
			expect_fs_record(&cursor, &records[visited++]);
			status = apfs_cursor_next(&cursor, &found, &f.error);
		}
		status_is(&f, status, FSIMG_OK);
		expect(visited == 4, "walk includes both copies of the repeated name");
		apfs_cursor_free(&cursor);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

/*
 * Start with eight mapped leaves and three index levels. Replace one node
 * per level with references to the same child, under alternating a/z keys
 * of one plain directory class. Fanout two at the root repeats a leaf once;
 * fanout sixteen at every level would yield 4096 copies from four nodes.
 */
static uint64_t
build_repeated_plain_subtree(fixture_t *f, bool nested,
    const apfs_gen_record_t *leaf)
{
	static const unsigned node_indices[] = { 0, 8, 12, 14 };
	unsigned char keys[2][64], child_bytes[8];
	apfs_gen_record_t records[16], separators[2];
	apfs_gen_node_t node;
	uint64_t root, child = 2000;

	separators[0] = fs_key_build(keys[0], &(fs_key_case_t){
	    .owner = 42, .kind = 9, .name = "a" }, false);
	separators[1] = fs_key_build(keys[1], &(fs_key_case_t){
	    .owner = 42, .kind = 9, .name = "z" }, false);
	for (unsigned i = 0; i < 8; i++)
		records[i] = *leaf;
	root = build_fs_tree(f, records, 8, 1, false);
	for (unsigned level = 1; level <= 3; level++) {
		unsigned count = 1;
		uint64_t oid = 2000 + node_indices[level];
		unsigned char *raw = block_at(f, OBJECT_BLOCK + node_indices[level]);

		if (nested)
			count = 16;
		else if (level == 3)
			count = 2;
		CHECK(apfs_gen_sibling_map_val_build(child_bytes, 8, child));
		for (unsigned i = 0; i < count; i++) {
			records[i] = separators[i % 2];
			records[i].value = child_bytes;
			records[i].value_size = 8;
		}
		node = (apfs_gen_node_t){ .object = { .oid = oid, .xid = 5,
		    .type = level == 3 ? 2 : 3, .subtype = 14 },
		    .flags = level == 3 ? GEN_APFS_ROOT : 0, .level = level,
		    .records = records, .count = count,
		    .info = { .flags = 0x40, .node_size = BLOCK,
		    .longest_key = 64, .longest_val = 8,
		    .key_count = nested ? 4096 : 2, .node_count = 15 } };
		bzero(raw, BLOCK);
		CHECK(apfs_gen_node_build(raw, BLOCK, &node));
		child = oid;
	}
	return (root);
}

static void
test_plain_repeated_subtree_walk_is_bounded_by_image_blocks(void)
{
	unsigned char key[64];
	apfs_gen_record_t leaf;
	apfs_cursor_t cursor;
	apfs_map_t map;
	fixture_t f;
	uint64_t root;
	fsimg_status_t status;
	bool found;

	for (unsigned nested = 0; nested < 2; nested++) {
		size_t visited = 0;

		scenario = nested ? "nested repeated plain subtree exceeds image blocks" :
		    "single repeated plain reference walks twice within image bound";
		fixture_init(&f, 0);
		build_checkpoints(&f, 1, 10);
		CHECK(checkpoint_open(&f) == FSIMG_OK);
		leaf = fs_key_build(key, &(fs_key_case_t){ .owner = 42,
		    .kind = 9, .name = "repeated" }, false);
		root = build_repeated_plain_subtree(&f, nested != 0, &leaf);
		CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
		apfs_cursor_init(&cursor, &f.container, apfs_fs_tree_profile(false),
		    APFS_OBJ_VIRTUAL, root, 10, &map);
		status = apfs_cursor_first(&cursor, &found, &f.error);
		while (status == FSIMG_OK && found) {
			/* Also cap the test if the cursor's load guard regresses. */
			CHECK(visited < 4096);
			expect_fs_record(&cursor, &leaf);
			visited++;
			status = apfs_cursor_next(&cursor, &found, &f.error);
		}
		if (nested) {
			status_is(&f, status, FSIMG_ECORRUPT);
			expect(strstr(f.error.message,
			    "repeats nodes beyond the image's blocks") != NULL,
			    "nested repetition reaches the cursor load bound");
			expect(visited > 2 && visited < 4096,
			    "walk stops before expanding the repeated subtree fully");
		} else {
			status_is(&f, status, FSIMG_OK);
			expect(visited == 2, "single repeated reference yields both records");
		}
		expect(!found && !cursor.positioned, "completed walk has no record");
		apfs_cursor_free(&cursor);
		apfs_map_close(&map);
		fixture_close(&f);
	}
}

static void
test_filesystem_entry_check_accepts_each_filesystem_key_kind(void)
{
	static const struct {
		const char *name;
		fs_key_case_t key;
	} cases[] = {
		{ "inode", { .owner = 42, .kind = 3 } },
		{ "xattr", { .owner = 42, .kind = 4, .name = "attr" } },
		{ "sibling link", { .owner = 42, .kind = 5, .number = 7 } },
		{ "dstream ID", { .owner = 42, .kind = 6 } },
		{ "crypto state", { .owner = 42, .kind = 7 } },
		{ "file extent", { .owner = 42, .kind = 8, .number = 4096 } },
		{ "directory record", { .owner = 42, .kind = 9, .number = 7,
		    .name = "entry" } },
		{ "directory statistics", { .owner = 42, .kind = 10 } },
		{ "sibling map", { .owner = 42, .kind = 12 } },
		{ "file info", { .owner = 42, .kind = 13, .number = 1 } }
	};
	unsigned char key[64];
	apfs_gen_record_t generated;
	apfs_record_t record;
	fsimg_error_t error;
	const apfs_tree_profile_t *profile;

	for (unsigned hashed = 0; hashed < 2; hashed++) {
		profile = apfs_fs_tree_profile(hashed != 0);
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			scenario = cases[i].name;
			generated = fs_key_build(key, &cases[i].key, hashed != 0);
			record = (apfs_record_t){ .key = key,
			    .key_len = generated.key_size };
			for (unsigned leaf = 0; leaf < 2; leaf++)
				expect(profile->entry_check(&record, leaf != 0, &error) ==
				    FSIMG_OK, "valid key accepted in index and leaf");
		}
	}
}

static void
test_hashed_seeks_find_exact_separators_and_gaps_between_leaves(void)
{
	unsigned char keys[8][64], gap_key[64];
	apfs_gen_record_t records[8], gap;
	apfs_cursor_t cursor;
	apfs_map_t map;
	fixture_t f;
	uint64_t root;

	scenario = "hashed GE and LE at exact separators and between leaves";
	fixture_init(&f, 0);
	build_checkpoints(&f, 1, 10);
	CHECK(checkpoint_open(&f) == FSIMG_OK);
	for (unsigned i = 0; i < 8; i++)
		records[i] = fs_key_build(keys[i], &(fs_key_case_t){
		    .owner = 42, .kind = 9, .number = 2 * i + 2, .name = "a" }, true);
	root = build_fs_tree(&f, records, 8, 2, false);
	CHECK(map_open(&f, &map, APFS_MAP_VOLUME) == FSIMG_OK);
	apfs_cursor_init(&cursor, &f.container, apfs_fs_tree_profile(true),
	    APFS_OBJ_VIRTUAL, root, 10, &map);
	for (unsigned i = 0; i < 8; i++) {
		expect_fs_seek(&f, &cursor, &records[i], APFS_SEEK_GE, &records[i]);
		expect_fs_seek(&f, &cursor, &records[i], APFS_SEEK_LE, &records[i]);
		if (i == 0 || i % 2 != 0)
			continue;
		gap = fs_key_build(gap_key, &(fs_key_case_t){ .owner = 42,
		    .kind = 9, .number = 2 * i + 1, .name = "a" }, true);
		expect_fs_seek(&f, &cursor, &gap, APFS_SEEK_GE, &records[i]);
		expect_fs_seek(&f, &cursor, &gap, APFS_SEEK_LE, &records[i - 1]);
	}
	apfs_cursor_free(&cursor);
	apfs_map_close(&map);
	fixture_close(&f);
}

static void
test_filesystem_entry_check_refuses_foreign_kinds_and_malformed_keys(void)
{
	static const struct {
		const char *name;
		uint8_t kind;
		const char *error;
	} cases[] = {
		{ "invalid kind zero", 0, "kind outside the tree" },
		{ "snapshot metadata kind", 1, "kind outside the tree" },
		{ "physical extent kind", 2, "kind outside the tree" },
		{ "snapshot name kind", 11, "kind outside the tree" },
		{ "invalid kind fourteen", 14, "kind outside the tree" },
		{ "invalid kind fifteen", 15, "kind outside the tree" },
		{ "drec missing name and length", 9, "malformed key" },
		{ "xattr missing name and length", 4, "malformed key" },
		{ "extent missing logical offset", 8, "malformed key" }
	};
	unsigned char key[8];
	apfs_record_t record;
	fsimg_error_t error;
	const apfs_tree_profile_t *profile;

	for (unsigned hashed = 0; hashed < 2; hashed++) {
		profile = apfs_fs_tree_profile(hashed != 0);
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			scenario = cases[i].name;
			CHECK(apfs_gen_jkey_build(key, sizeof(key), 42, cases[i].kind));
			record = (apfs_record_t){ .key = key, .key_len = sizeof(key) };
			for (unsigned leaf = 0; leaf < 2; leaf++) {
				expect(profile->entry_check(&record, leaf != 0, &error) ==
				    FSIMG_ECORRUPT, "bad key refused in index and leaf");
				expect(strstr(error.message, cases[i].error) != NULL,
				    cases[i].error);
			}
		}
	}
}

typedef struct native_fs_sample native_fs_sample_t;

/* Two hashed directory records of one directory with the same 22-bit hash. */
static bool
native_hash_tie(const unsigned char *a, size_t a_len, const unsigned char *b,
    size_t b_len)
{
	return (a_len >= 12 && b_len >= 12 &&
	    fs_test_integer(a, 8) == fs_test_integer(b, 8) &&
	    (fs_test_integer(a, 8) >> 60) == 9 &&
	    fs_test_integer(a + 8, 4) >> 10 == fs_test_integer(b + 8, 4) >> 10);
}

struct native_fs_sample {
	native_fs_sample_t *next;
	size_t length;
	unsigned char key[];
};

/* Apple File System Reference: apfs_superblock_t field offsets and
 * APFS_INCOMPAT_CASE_INSENSITIVE / APFS_INCOMPAT_NORMALIZATION_INSENSITIVE.
 */
enum {
	NATIVE_VOLUME_OMAP_OID_OFFSET = 128,
	NATIVE_INCOMPAT_CASE_INSENSITIVE = 0x1,
	NATIVE_INCOMPAT_NORMALIZATION_INSENSITIVE = 0x8,
	/* The e4-names capture corpus contains at least 15000 directory entries. */
	NATIVE_NAMES_MIN_DRECS = 15000
};

static native_fs_sample_t *
walk_native_fs_tree(fixture_t *f, apfs_cursor_t *cursor, bool hashed,
    bool many_names)
{
	unsigned char *previous;
	native_fs_sample_t *samples = NULL, **tail = &samples, *sample;
	apfs_record_t record;
	uint64_t count = 0, drecs = 0, hash_ties = 0;
	size_t previous_len = 0;
	fsimg_status_t status;
	bool found;

	previous = malloc(UINT16_MAX);
	CHECK(previous != NULL);
	/* No native plain-key volume exists: strict increase is a hashed oracle. */
	status = apfs_cursor_first(cursor, &found, &f->error);
	while (status == FSIMG_OK && found) {
		apfs_cursor_record(cursor, &record);
		CHECK(record.key != NULL && record.key_len <= UINT16_MAX);
		if (hashed && count != 0) {
			expect(fs_test_compare(previous, previous_len, record.key,
			    record.key_len, hashed) < 0,
			    "native adjacent keys strictly increase under independent order");
			expect(cursor->profile->compare(previous, previous_len,
			    record.key, record.key_len) < 0,
			    "native adjacent keys strictly increase under profile order");
			if (native_hash_tie(previous, previous_len,
			    record.key, record.key_len))
				hash_ties++;
		}
		bcopy(record.key, previous, record.key_len);
		previous_len = record.key_len;
		if (count % 97 == 0) {
			sample = malloc(sizeof(*sample) + record.key_len);
			CHECK(sample != NULL);
			sample->next = NULL;
			sample->length = record.key_len;
			bcopy(record.key, sample->key, record.key_len);
			*tail = sample;
			tail = &sample->next;
		}
		if ((fs_test_integer(record.key, 8) >> 60) == 9)
			drecs++;
		count++;
		status = apfs_cursor_next(cursor, &found, &f->error);
	}
	if (status_is(f, status, FSIMG_OK)) {
		expect(count == cursor->info.key_count,
		    "native leaf record count equals root footer key count");
		if (many_names) {
			expect(drecs >= NATIVE_NAMES_MIN_DRECS,
			    "names capture holds at least 15000 drecs");
			if (hashed)
				expect(hash_ties != 0,
				    "names capture orders equal-hash drecs by name");
		}
	}
	printf("native filesystem tree: %llu records, %llu directory records\n",
	    (unsigned long long)count, (unsigned long long)drecs);
	free(previous);
	return (samples);
}

static void
seek_native_fs_samples(fixture_t *f, apfs_cursor_t *cursor,
    native_fs_sample_t *samples)
{
	native_fs_sample_t *sample;
	apfs_record_t record;
	fsimg_status_t status;
	bool found;

	for (sample = samples; sample != NULL; sample = sample->next) {
		for (unsigned direction = 0; direction < 2; direction++) {
			status = apfs_cursor_seek(cursor, sample->key,
			    sample->length,
			    direction == 0 ? APFS_SEEK_GE : APFS_SEEK_LE,
			    &found, &f->error);
			if (!status_is(f, status, FSIMG_OK))
				continue;
			expect(found, "native sampled seek finds a record");
			if (!found)
				continue;
			apfs_cursor_record(cursor, &record);
			expect(record.key_len == sample->length &&
			    bcmp(record.key, sample->key, sample->length) == 0,
			    "native sampled GE/LE lands on exact key");
		}
	}
}

static void
check_native_fs_tree(fixture_t *f, const unsigned char *volume,
    bool many_names)
{
	unsigned char *window;
	native_fs_sample_t *samples, *sample;
	apfs_cursor_t cursor;
	apfs_map_t map;
	uint64_t incompat, map_oid, root_oid;
	fsimg_status_t status;
	bool hashed;

	incompat = fs_test_integer(volume + NATIVE_VOLUME_INCOMPAT_OFFSET, 8);
	map_oid = fs_test_integer(volume + NATIVE_VOLUME_OMAP_OID_OFFSET, 8);
	root_oid = fs_test_integer(volume + NATIVE_VOLUME_ROOT_OID_OFFSET, 8);
	hashed = (incompat & (NATIVE_INCOMPAT_CASE_INSENSITIVE |
	    NATIVE_INCOMPAT_NORMALIZATION_INSENSITIVE)) != 0;
	window = malloc(f->container.block_size);
	CHECK(window != NULL);
	status = apfs_map_open(&f->container, APFS_MAP_VOLUME, map_oid,
	    f->container.xid, window, &map, &f->error);
	if (!status_is(f, status, FSIMG_OK)) {
		free(window);
		return;
	}
	apfs_cursor_init(&cursor, &f->container, apfs_fs_tree_profile(hashed),
	    APFS_OBJ_VIRTUAL, root_oid, f->container.xid, &map);
	samples = walk_native_fs_tree(f, &cursor, hashed, many_names);
	/* Exact landing is established only for hashed native volumes; no plain
	 * native volume exists, and its directory class permits equal keys.
	 */
	if (hashed)
		seek_native_fs_samples(f, &cursor, samples);
	while (samples != NULL) {
		sample = samples;
		samples = sample->next;
		free(sample);
	}
	apfs_cursor_free(&cursor);
	apfs_map_close(&map);
	free(window);
}

/* Native bytes enter exclusively through the public read-only I/O adapter. */
static void
check_native_image(const char *path, bool filesystem, bool many_names)
{
	unsigned char initial[BLOCK], *window;
	apfs_obj_t header;
	apfs_nx_t nx;
	apfs_map_t map;
	apfs_ref_t ref;
	fixture_t f;
	size_t transferred;
	int fd;
	bool idle;

	scenario = path;
	printf("native capture: %s\n", path);
	bzero(&f, sizeof(f));
	reader_init(&f, 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open native capture: %s\n",
		    path, strerror(errno));
		failures++;
		fixture_close(&f);
		return;
	}
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &f.io,
	    &f.error) == FSIMG_OK);
	CHECK(close(fd) == 0);
	f.reader.input = f.io;
	CHECK(fsimg_io_get_size(f.io, &f.length, &f.error) == FSIMG_OK);
	f.reader.slice_bytes = f.length;
	CHECK(fsimg_io_read_exact(f.io, 0, initial, sizeof(initial),
	    &transferred, &f.error) == FSIMG_OK);
	CHECK(apfs_nx_decode(initial, sizeof(initial), &nx));
	CHECK(nx.block_size >= 4096 && nx.block_size <= 65536);
	window = malloc(nx.block_size);
	CHECK(window != NULL);
	CHECK(fsimg_io_read_exact(f.io, 0, window, nx.block_size,
	    &transferred, &f.error) == FSIMG_OK);
	CHECK(apfs_obj_decode(window, nx.block_size, &header));
	CHECK(apfs_nx_decode(window, nx.block_size, &nx));
	CHECK(apfs_object_verify(window, nx.block_size));
	if (status_is(&f, apfs_checkpoint_open(&f.reader, &header, &nx,
	    &f.container, &f.error), FSIMG_OK)) {
		expect(f.container.clean, "native capture is clean");
		expect(f.container.block_count == nx.block_count,
		    "native block count");
		if (status_is(&f, apfs_map_open(&f.container,
		    APFS_MAP_CONTAINER,
		    f.container.nx.omap_oid, f.container.xid, window, &map,
		    &f.error), FSIMG_OK)) {
			for (unsigned i = 0; i < f.container.nx.max_volumes;
			    i++) {
				if (f.container.nx.volume_oids[i] == 0)
					continue;
				ref = (apfs_ref_t){ .role = "native volume",
				    .oid = f.container.nx.volume_oids[i],
				    .xid = f.container.xid, .kind = 13,
				    .storage = APFS_OBJ_VIRTUAL, .map = &map };
				if (status_is(&f, apfs_object_read(&f.container,
				    &ref, window, NULL, &f.error), FSIMG_OK) &&
				    filesystem)
					check_native_fs_tree(&f, window, many_names);
			}
			apfs_map_close(&map);
		}
		if (status_is(&f, apfs_reaper_idle(&f.container, window, &idle,
		    &f.error), FSIMG_OK))
			expect(idle, "native reaper is idle");
		for (unsigned i = 0; i < f.container.nx.max_volumes; i++) {
			fsimg_reader_t *reader = NULL;
			fsimg_read_options_t options;

			if (f.container.nx.volume_oids[i] == 0)
				continue;
			fsimg_read_options_init(&options);
			options.volume_index_set = true;
			options.volume_index = i;
			status_is(&f, fsimg_reader_open(f.ctx, f.io, &options,
			    &reader, &f.error), FSIMG_OK);
			fsimg_reader_free(reader);
		}
	}
	free(window);
	fixture_close(&f);
}

static void
test_native_captures(void)
{
	static const struct {
		const char *directory;
		const char *alternate;
	} captures[] = {
		{ .directory = "apfs-newfs-4194304-plain" },
		{ .directory = "apfs-newfs-33554432-plain" },
		{ .directory = "apfs-newfs-1073741824-plain" },
		{ .directory = "apfs-newfs-1073741824-e8-history" },
		{ .directory = "apfs-diskutil-2147483648-e17-clean",
		    .alternate = "after-detach-clean.img" }
	};
	const char *directory;
	char path[4096];
	glob_t paths;

	directory = getenv("FSIMG_APFS_NATIVE");
	if (directory == NULL)
		puts("SKIP native captures (FSIMG_APFS_NATIVE is unset)");
	else {
		for (size_t i = 0; i < sizeof(captures) / sizeof(captures[0]);
		    i++) {
			CHECK(snprintf(path, sizeof(path), "%s/%s/image.img",
			    directory,
			    captures[i].directory) < (int)sizeof(path));
			if (captures[i].alternate != NULL &&
			    access(path, F_OK) != 0 && errno == ENOENT)
				CHECK(snprintf(path, sizeof(path),
				    "%s/%s/%s", directory,
				    captures[i].directory,
				    captures[i].alternate) < (int)sizeof(path));
			check_native_image(path, false, false);
		}
	}
	directory = getenv("FSIMG_APFS_E8");
	if (directory == NULL)
		puts("SKIP E8 ring captures (FSIMG_APFS_E8 is unset)");
	else {
		CHECK(snprintf(path, sizeof(path), "%s/c0*.img", directory) <
		    (int)sizeof(path));
		bzero(&paths, sizeof(paths));
		CHECK(glob(path, 0, NULL, &paths) == 0);
		CHECK(paths.gl_pathc != 0);
		for (size_t i = 0; i < paths.gl_pathc; i++)
			check_native_image(paths.gl_pathv[i], false, false);
		globfree(&paths);
	}
}

static void
test_native_filesystem_captures_walk_and_seek_in_independent_order(void)
{
	static const char *captures[] = {
		"apfs-newfs-268435456-e3-ci",
		"apfs-newfs-268435456-e3-cs",
		"apfs-newfs-268435456-e4-names-ci",
		"apfs-newfs-268435456-e4-names-cs",
		"apfs-newfs-33554432-e4-prefix-ci",
		"apfs-newfs-33554432-e4-prefix-cs",
		"apfs-newfs-67108864-e5-xattr",
		"apfs-newfs-67108864-e5-xname",
		"apfs-newfs-67108864-e5-xlong",
		"apfs-newfs-67108864-e5-xbound",
		"apfs-newfs-67108864-e5-links"
	};
	const char *directory = getenv("FSIMG_APFS_NATIVE");
	char path[4096];

	if (directory == NULL) {
		puts("SKIP native filesystem captures (FSIMG_APFS_NATIVE is unset)");
		return;
	}
	for (size_t i = 0; i < sizeof(captures) / sizeof(captures[0]); i++) {
		CHECK(snprintf(path, sizeof(path), "%s/%s/image.img", directory,
		    captures[i]) < (int)sizeof(path));
		if (access(path, F_OK) != 0 && errno == ENOENT) {
			printf("SKIP missing native filesystem capture: %s\n", path);
			continue;
		}
		check_native_image(path, true, strstr(captures[i], "e4-names") != NULL);
	}
}

/* The fuzz seed (fuzz/apfs.c): build_openable()'s container packed into
 * SEED_BLOCKS blocks, so that most mutations land on metadata. Two
 * checkpoints each own their spaceman and reaper; the container map is an
 * index node over two leaves, so lookups descend; volumes occupy slots 0
 * and 2 of four, so the harness's slot loop meets an occupied slot, an
 * empty one and one past the array.
 */
enum {
	SEED_CONTAINER_BLOCKS = 256,	/* the smallest container, 1 MiB */
	SEED_BLOCKS = 33,	/* the blocks the seed holds */
	SEED_MAP = 9,		/* container object map */
	SEED_DATA = 10,		/* checkpoint data area, four blocks per run */
	SEED_TREE = 18,		/* container map tree, three blocks */
	SEED_VOLUMES = 21,	/* APSB, object map and map root per slot */
	SEED_OCCUPIED = 0x5	/* bitmask of occupied volume slots */
};

static void
build_seed(unsigned char *bytes, size_t length)
{
	apfs_gen_mapping_t mappings[2][2];
	apfs_gen_run_t runs[2];
	apfs_gen_container_t image = {
		.nx = { .magic = 0x4253584e, .block_size = BLOCK,
		.block_count = SEED_CONTAINER_BLOCKS, .uuid = { 1, 2, 3 },
		.desc_base = 1, .desc_blocks = DESCRIPTOR_SLOTS,
		    .data_base = SEED_DATA,
		.data_blocks = 8, .max_volumes = VOLUME_MAX,
		.spaceman_oid = 100, .reaper_oid = 102, .omap_oid = SEED_MAP },
		.runs = runs, .count = 2, .block0_run = 1, .block0_xid = 10
	};
	apfs_gen_spaceman_t spaceman;
	apfs_gen_omap_record_t records[VOLUME_MAX];
	apfs_gen_tree_t tree;
	apfs_gen_fs_t fs;
	apfs_gen_omap_t map;
	apfs_gen_node_t node;
	uint64_t root, blocks;

	for (unsigned i = 0; i < 2; i++) {
		mappings[i][0] = (apfs_gen_mapping_t){ .type = 0x80000005,
		    .size = BLOCK, .oid = 100, .address = SEED_DATA + 4 * i };
		mappings[i][1] = (apfs_gen_mapping_t){ .type = 0x80000011,
		    .size = BLOCK, .oid = 102,
		    .address = SEED_DATA + 1 + 4 * i };
		runs[i] = (apfs_gen_run_t){ .xid = 10 * (i + 1),
		    .next_xid = 10 * (i + 1) + 1, .mappings = mappings[i],
		    .mapping_count = 2, .mapping_blocks = 1,
		    .desc_index = 2 * i, .desc_next = 2 * i + 2,
		    .data_index = 4 * i, .data_len = 4,
		    .data_next = (4 * i + 4) % 8 };
	}
	CHECK(apfs_gen_spaceman_geometry(BLOCK, SEED_CONTAINER_BLOCKS,
	    &spaceman) == BLOCK);
	spaceman.object = (apfs_gen_object_t){ .oid = 100, .xid = 20,
	    .type = 0x80000005 };
	spaceman.devices[0].free_count = 8;
	CHECK(apfs_gen_container_build(bytes, length, &image));
	/* Block 0 and both checkpoint superblocks, as in build_openable(). */
	for (unsigned b = 0; b <= NEWEST_NX; b += 2) {
		fsimg_store_le64(bytes + (size_t)b * BLOCK + 64, 2);
		for (unsigned slot = 0; slot < VOLUME_MAX; slot++)
			fsimg_store_le64(bytes + (size_t)b * BLOCK + 184 +
			    8 * slot, (SEED_OCCUPIED & (1u << slot)) ?
			    VOLUME_OID + slot : 0);
		CHECK(apfs_gen_seal(bytes + (size_t)b * BLOCK, BLOCK));
	}
	CHECK(apfs_gen_spaceman_build(bytes + (size_t)(SEED_DATA + 4) * BLOCK,
	    BLOCK, &spaceman));
	for (unsigned slot = 0; slot < VOLUME_MAX; slot++) {
		unsigned address = SEED_VOLUMES + 3 * slot;
		unsigned char *apsb = bytes + (size_t)address * BLOCK;

		records[slot] = (apfs_gen_omap_record_t){
		    .key = { .oid = VOLUME_OID + slot, .xid = 10 },
		    .value = { .size = BLOCK, .address = address } };
		fs = (apfs_gen_fs_t){
		    .object = { .oid = VOLUME_OID + slot, .xid = 10,
		    .type = 13 },
		    .magic = 0x42535041, .index = slot,
		    .uuid = { 1, 2, 3, (unsigned char)(slot + 1) },
		    .name = "Seed volume", .flags = 1,
		    .incompat_features = 9, .root_tree_type = 2,
		    .extentref_tree_type = 0x40000002,
		    .snapshot_tree_type = 0x40000002, .omap_oid = address + 1,
		    .files = 11, .directories = 12, .symlinks = 13,
		    .other_objects = 14, .snapshots = 15, .alloc_blocks = 16,
		    .role = 64 };
		CHECK(apfs_gen_fs_build(apsb, BLOCK, &fs));
		map = (apfs_gen_omap_t){ .object = { .oid = address + 1,
		    .xid = 10, .type = 0x4000000b },
		    .tree_type = 0x40000002, .tree_oid = address + 2 };
		CHECK(apfs_gen_omap_build(apsb + BLOCK, BLOCK, &map));
		node = (apfs_gen_node_t){ .object = { .oid = address + 2,
		    .xid = 10, .type = 0x40000002, .subtype = 11 },
		    .flags = GEN_APFS_ROOT | GEN_APFS_LEAF | GEN_APFS_FIXED,
		    .info = { .flags = 0x17, .node_size = BLOCK,
		    .key_size = 16, .val_size = 16, .node_count = 1 } };
		CHECK(apfs_gen_node_build(apsb + 2 * BLOCK, BLOCK, &node));
	}
	tree = (apfs_gen_tree_t){ .records = records, .count = VOLUME_MAX,
	    .base = SEED_TREE, .xid = 10, .block_size = BLOCK,
	    .records_per_leaf = 2, .children_per_index = 2 };
	CHECK(apfs_gen_tree_build(bytes, length, &tree, &root, &blocks));
	CHECK(SEED_TREE + blocks <= SEED_VOLUMES);
	map = (apfs_gen_omap_t){ .object = { .oid = SEED_MAP, .xid = 10,
	    .type = 0x4000000b }, .flags = 1, .tree_type = 0x40000002,
	    .tree_oid = root };
	CHECK(apfs_gen_omap_build(bytes + (size_t)SEED_MAP * BLOCK, BLOCK,
	    &map));
}

/* Write directory/container.seed after opening each occupied slot through
 * the public reader at the newest, clean checkpoint, so a seed that no
 * longer opens fails here rather than silently starving the fuzzer. The
 * seed stops short of the container's declared end, which the reader
 * accepts and reports as missing bytes.
 */
static void
emit_seed(const char *directory)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	apfs_reader_t *apfs;
	char path[4096];
	FILE *file;

	scenario = "fuzz seed opens";
	fixture_init(&f, 0);
	build_seed(f.bytes, (size_t)SEED_CONTAINER_BLOCKS * BLOCK);
	fixture_truncate(&f, (uint64_t)SEED_BLOCKS * BLOCK);
	fsimg_read_options_init(&options);
	options.volume_index_set = true;
	for (unsigned slot = 0; slot < VOLUME_MAX; slot++) {
		if ((SEED_OCCUPIED & (1u << slot)) == 0)
			continue;
		options.volume_index = slot;
		CHECK(status_is(&f, fsimg_reader_open(f.ctx, f.reader.input,
		    &options, &reader, &f.error), FSIMG_OK));
		apfs = reader->driver;
		CHECK(apfs->container.xid == 20 && apfs->container.clean);
		CHECK(reader->info.missing_bytes ==
		    (uint64_t)(SEED_CONTAINER_BLOCKS - SEED_BLOCKS) * BLOCK);
		fsimg_reader_free(reader);
	}
	CHECK(snprintf(path, sizeof(path), "%s/container.seed", directory) <
	    (int)sizeof(path));
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(f.bytes, 1, (size_t)f.length, file) == f.length);
	CHECK(fclose(file) == 0);
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_seed(argv[2]);
		return (failures == 0 ? 0 : 1);
	}
	test_older_layout_container_lists_and_reads_like_current();
	test_full_open_predecessor_is_openable();
	test_full_open_private_map_damage_does_not_fall_back();
	test_full_open_volume_selection();
	test_full_open_volume_validation();
	test_full_open_information_and_prepare();
	test_full_open_probe_and_assumed_identity();
	test_probe_uses_4096_byte_prefix_and_open_checks_whole_block();
	test_full_open_checkpoint_features_and_free_count();
	test_full_open_truncated_container();
	test_selected_descriptor_changed_while_read();
	test_private_container_map_damage_never_falls_back();
	test_descriptor_flags_and_subtypes_exclude_newest_checkpoint();
	test_checkpoint_selection_and_zero_slots();
	test_torn_newest_checkpoint_opens_older_unclean();
	test_invalid_superblocks_and_ambiguous_checkpoints();
	test_foreign_uuid_precedes_noncontiguous_data_geometry();
	test_selected_checkpoint_damage_never_falls_back();
	test_truncated_containers_do_not_select_older_state();
	test_wrapping_runs_large_tables_and_multiblock_objects();
	test_checkpoint_rejects_multiblock_reaper_and_reap_list();
	test_last_map_flag_belongs_only_on_last_mapping_block();
	test_geometry_rejects_invalid_sizes_volume_slots_and_areas();
	test_physical_and_ephemeral_object_references();
	test_multilevel_cursors_seek_and_visit_in_order();
	test_separator_may_precede_child_first_key();
	test_nodes_reject_invalid_flags_levels_bounds_and_layout();
	test_map_lookup_selects_versions_and_deleted_entries_do_not_fall_back();
	test_map_values_enforce_flags_sizes_and_address_ranges();
	test_map_object_flags_and_transitions();
	test_tree_entry_hook_checks_every_entry_and_propagates_failure();
	test_entry_hook_distinguishes_index_and_leaf_entries();
	test_loaded_map_lookup_exhausts_work_budget();
	test_cursor_seek_rejects_key_lengths_outside_profile_bounds();
	test_virtual_object_size_is_rejected_before_reading();
	test_virtual_objects_must_match_map_key_xid();
	test_reaper_idle_shapes_and_malformed_lists();
	test_reap_list_codec_and_generator_reject_out_of_bounds_arrays();
	test_assemblers_leave_buffers_untouched_on_bad_parameters();
	test_work_and_memory_limits_release_partial_open();
	test_filesystem_comparator_orders_owners_kinds_and_suffixes();
	test_hashed_comparator_ignores_name_length_bits();
	test_plain_directory_walk_accepts_identical_stored_keys();
	test_plain_repeated_subtree_walk_is_bounded_by_image_blocks();
	test_filesystem_entry_check_accepts_each_filesystem_key_kind();
	test_plain_directory_class_spans_leaves_and_seeks_to_both_edges();
	test_filesystem_trees_refuse_duplicates_and_decreasing_keys();
	test_hashed_seeks_find_exact_separators_and_gaps_between_leaves();
	test_filesystem_entry_check_refuses_foreign_kinds_and_malformed_keys();
	test_native_captures();
	test_native_filesystem_captures_walk_and_seek_in_independent_order();
	printf("APFS: %u status checks, %u failures\n", scenarios, failures);
	return (failures == 0 ? 0 : 1);
}
