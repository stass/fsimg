/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apfs.h"
#include "apfs-gen.h"
#include "fsimg-endian.h"
#include "reader.h"
#include "test.h"

#define BLOCK 4096u
#define BLOCKS 1024u
#define IMAGE_BYTES ((size_t)BLOCK * BLOCKS)
#define VOLUME_BASE 100u
#define DATA_BLOCK 200u
#define CIB_BLOCK 77u
#define BITMAP_BLOCK 78u
#define POOL_QUEUE_BLOCK 79u
#define PHYSICAL_TREE 0x40000002u
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Records own their spans; descriptor sorting leaves their storage stable. */
typedef struct fs_record {
	unsigned char key[128];
	unsigned char value[256];
	uint16_t key_size;
	uint16_t value_size;
} fs_record_t;

typedef struct fs_tree {
	fs_record_t records[260];
	size_t count;
} fs_tree_t;

/* Fixed regions occupy 0..82. Each volume owns six blocks at 100 + 10*i:
 * superblock, object map, map root, virtual fs root, snapshots, extents.
 * The CIB and main bitmap live inside the separately accounted pool.
 */
#define FIXTURE_TREES 3

typedef struct fixture {
	unsigned char *bytes;
	uint64_t	 length;
	apfs_gen_spaceman_t sm;
	/* FIXTURE_TREES trees, about 100 KiB each, owned like bytes: kept off
	 * the stack, where every test's fixture would otherwise add up in an
	 * inlined, sanitized main.
	 */
	fs_tree_t	*trees;
	uint64_t expected_files;
	uint64_t expected_directories;
	uint64_t expected_logical_bytes;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
} fixture_t;

static unsigned failures, checks, native_verified, native_refused;
static unsigned native_complete, native_pending, native_encrypted;
static unsigned native_snapshots;
static unsigned native_refused_encrypted, native_refused_empty;
static const char *scenario;

static void
expect(bool condition, const char *what)
{
	checks++;
	if (!condition) {
		fprintf(stderr, "%s: %s\n", scenario, what);
		failures++;
	}
}

static bool
status_is(fsimg_status_t actual, fsimg_status_t wanted,
    const fsimg_error_t *error, const char *message)
{
	checks++;
	if (actual == wanted && strstr(error->message, message) != NULL)
		return (true);
	fprintf(stderr, "%s: expected %d containing '%s', got %d (%s)\n",
	    scenario, wanted, message, actual, error->message);
	failures++;
	return (false);
}

static unsigned char *
block_at(fixture_t *f, uint64_t block)
{
	CHECK(block < BLOCKS);
	return (f->bytes + block * BLOCK);
}

static void
bit_set(unsigned char *bits, uint64_t bit, bool set)
{
	if (set)
		bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
	else
		bits[bit / 8] &= (unsigned char)~(1u << (bit % 8));
}

static fsimg_status_t
memory_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = arg;

	CHECK(offset <= f->length && length <= f->length - offset);
	bcopy(f->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *arg, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((fixture_t *)arg)->length;
	return (FSIMG_OK);
}

static void
node_build(fixture_t *f, uint64_t address, uint64_t oid, uint32_t type,
    uint32_t subtype, const apfs_gen_record_t *records, uint32_t count,
    uint32_t key_size, uint32_t value_size)
{
	apfs_gen_node_t node = {
		.object = { .oid = oid, .xid = 10, .type = type,
		.subtype = subtype },
		.flags = GEN_APFS_ROOT | GEN_APFS_LEAF |
		    (key_size != 0 ? GEN_APFS_FIXED : 0),
		.info = { .flags = (type == 0x80000002 ? 0x8 :
		    type == PHYSICAL_TREE ? 0x10 : 0) |
		    (subtype == 9 ? 0x4 : 0) |
		    (key_size != 0 ? 0 : 0x40),
		.node_size = BLOCK, .key_size = key_size,
		.val_size = value_size, .key_count = count, .node_count = 1 },
		.records = records, .count = count
	};

	bzero(block_at(f, address), BLOCK);
	CHECK(apfs_gen_node_build(block_at(f, address), BLOCK, &node));
}

static void
map_build(fixture_t *f, uint64_t address,
    const apfs_gen_omap_record_t *records, size_t count)
{
	apfs_gen_tree_t tree = { .records = records, .count = count,
		.base = address + 1, .xid = 10, .block_size = BLOCK,
		.records_per_leaf = 4, .children_per_index = 2 };
	apfs_gen_omap_t map = {
		.object = { .oid = address, .xid = 10, .type = 0x4000000b },
		.flags = address == 90 ? 1 : 0,
		.tree_type = PHYSICAL_TREE, .tree_oid = address + 1
	};
	uint64_t root, blocks;

	CHECK(apfs_gen_tree_build(f->bytes, IMAGE_BYTES, &tree, &root, &blocks));
	CHECK(root == address + 1 && blocks == 1);
	CHECK(apfs_gen_omap_build(block_at(f, address), BLOCK, &map));
}

static void
extents_build(fixture_t *f, uint64_t address, uint64_t first, uint64_t count)
{
	unsigned char key[GEN_JKEY_BYTES], value[GEN_PEXT_VAL_BYTES];
	apfs_gen_pext_val_t extent = { .length = count, .owner = 42,
		.refcnt = 1, .kind = 1 };
	apfs_gen_record_t record = { .key = key, .key_size = sizeof(key),
		.value = value, .value_size = sizeof(value) };

	CHECK(apfs_gen_jkey_build(key, sizeof(key), first, 2));
	CHECK(apfs_gen_pext_val_build(value, sizeof(value), &extent));
	node_build(f, address, address, PHYSICAL_TREE, 15, &record, 1, 0, 0);
}

static fs_record_t *
record_add(fs_tree_t *tree)
{
	CHECK(tree->count < ARRAY_COUNT(tree->records));
	return (&tree->records[tree->count++]);
}

static fs_record_t *
record_find(fs_tree_t *tree, uint64_t owner, unsigned kind)
{
	for (size_t i = 0; i < tree->count; i++) {
		uint64_t key = fsimg_load_le64(tree->records[i].key);

		if ((key & GEN_JKEY_OID_MASK) == owner && key >> 60 == kind)
			return (&tree->records[i]);
	}
	CHECK(false);
	return (NULL);
}

/* Ordinary fixture names are ASCII; collision tests supply Unicode scalars. */
static uint32_t
ascii_hash(const char *name)
{
	uint32_t scalars[128];
	size_t length = strlen(name);

	CHECK(length <= ARRAY_COUNT(scalars));
	for (size_t i = 0; i < length; i++)
		scalars[i] = name[i] >= 'A' && name[i] <= 'Z' ?
		    (uint32_t)(name[i] + 'a' - 'A') : (unsigned char)name[i];
	return (apfs_gen_name_hash(scalars, length));
}

static void
inode_build(fs_tree_t *tree, uint64_t oid, uint64_t parent, const char *name,
    uint8_t type, int32_t count, uint64_t size)
{
	unsigned char fields[128], stream[GEN_DSTREAM_BYTES];
	apfs_gen_xfield_t xf[] = {
		{ .type = 4, .flags = 2,
		.data = (const unsigned char *)name,
		.size = (uint16_t)(strlen(name) + 1) },
		{ .type = 8, .data = stream, .size = sizeof(stream) }
	};
	apfs_gen_dstream_t ds = { .size = size, .alloced_size = size };
	apfs_gen_inode_val_t inode = { .parent = parent, .private_id = oid,
		.mode = (uint16_t)((type << 12) | 0755), .count = count };
	fs_record_t *r = record_add(tree);
	uint16_t nfields = size != 0 ? 2 : 1;
	size_t length = 4 + 4 * nfields + (xf[0].size + 7u) / 8u * 8u;

	if (size != 0) {
		CHECK(apfs_gen_dstream_build(stream, sizeof(stream), &ds));
		length += sizeof(stream);
	}
	CHECK(length <= sizeof(fields));
	CHECK(apfs_gen_xfields_build(fields, length, xf, nfields));
	inode.xfields = fields;
	inode.xfields_len = length;
	r->key_size = GEN_JKEY_BYTES;
	r->value_size = (uint16_t)(GEN_INODE_VAL_BYTES + length);
	CHECK(apfs_gen_jkey_build(r->key, r->key_size, oid, 3));
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
}

static void
drec_build(fs_tree_t *tree, uint64_t parent, uint64_t child,
    const char *name, uint8_t type, uint32_t hash, uint64_t sibling)
{
	unsigned char fields[16], id[8];
	apfs_gen_drec_key_t key = { .parent = parent, .name = name,
		.name_len = (uint16_t)strlen(name), .hash = hash, .hashed = true };
	apfs_gen_drec_val_t value = { .child = child, .flags = type };
	apfs_gen_xfield_t xf = { .type = 1, .data = id, .size = sizeof(id) };
	fs_record_t *r = record_add(tree);

	if (sibling != 0) {
		fsimg_store_le64(id, sibling);
		CHECK(apfs_gen_xfields_build(fields, sizeof(fields), &xf, 1));
		value.xfields = fields;
		value.xfields_len = sizeof(fields);
	}
	r->key_size = (uint16_t)(GEN_DREC_HASHED_KEY_BYTES + key.name_len + 1);
	r->value_size = (uint16_t)(GEN_DREC_VAL_BYTES + value.xfields_len);
	CHECK(apfs_gen_drec_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_drec_val_build(r->value, r->value_size, &value));
}

static void
stream_id_build(fs_tree_t *tree, uint32_t references)
{
	fs_record_t *r = record_add(tree);

	r->key_size = GEN_JKEY_BYTES;
	r->value_size = GEN_DSTREAM_ID_VAL_BYTES;
	CHECK(apfs_gen_jkey_build(r->key, r->key_size, 16, 6));
	CHECK(apfs_gen_dstream_id_val_build(r->value, r->value_size, references));
}

static void
file_extent_build(fs_tree_t *tree, uint64_t offset, uint64_t physical,
    uint64_t length)
{
	fs_record_t *r = record_add(tree);
	apfs_gen_extent_val_t value = { .length = length, .phys_block = physical };

	r->key_size = GEN_EXTENT_KEY_BYTES;
	r->value_size = GEN_EXTENT_VAL_BYTES;
	CHECK(apfs_gen_extent_key_build(r->key, r->key_size, 16, offset));
	CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &value));
}

/* Compare the stored owner/kind tuple, then the kind's stored suffix. */
static int
fs_record_compare(const void *left, const void *right)
{
	const apfs_gen_record_t *a = left, *b = right;
	uint64_t ak = fsimg_load_le64(a->key), bk = fsimg_load_le64(b->key);
	uint64_t ao = ak & GEN_JKEY_OID_MASK, bo = bk & GEN_JKEY_OID_MASK;
	unsigned kind = (unsigned)(ak >> 60);

	if (ao != bo)
		return (ao < bo ? -1 : 1);
	if (kind != bk >> 60)
		return (kind < bk >> 60 ? -1 : 1);
	if (kind == 9) {
		uint32_t ah = fsimg_load_le32(a->key + 8) >> 10;
		uint32_t bh = fsimg_load_le32(b->key + 8) >> 10;

		if (ah != bh)
			return (ah < bh ? -1 : 1);
		return (strcmp((const char *)a->key + 12,
		    (const char *)b->key + 12));
	}
	if (kind == 4)
		return (strcmp((const char *)a->key + 10,
		    (const char *)b->key + 10));
	if (kind == 5 || kind == 8) {
		ak = fsimg_load_le64(a->key + 8);
		bk = fsimg_load_le64(b->key + 8);
		return (ak < bk ? -1 : ak > bk);
	}
	return (0);
}

static void
filesystem_build(fixture_t *f, unsigned volume)
{
	fs_tree_t *tree = &f->trees[volume];
	apfs_gen_record_t records[24];
	apfs_gen_omap_record_t map;
	apfs_gen_fs_tree_t layout = { .records = records, .count = tree->count,
		.first_oid = 2000 + volume, .base = VOLUME_BASE + 10 * volume + 3,
		.xid = 10, .block_size = BLOCK, .records_per_leaf = 24,
		.children_per_index = 2 };
	uint64_t root, blocks;

	for (size_t i = 0; i < tree->count; i++)
		records[i] = (apfs_gen_record_t){ .key = tree->records[i].key,
		    .key_size = tree->records[i].key_size,
		    .value = tree->records[i].value,
		    .value_size = tree->records[i].value_size };
	qsort(records, tree->count, sizeof(records[0]), fs_record_compare);
	CHECK(apfs_gen_fs_tree_build(f->bytes, IMAGE_BYTES, &layout, &map, 1,
	    &root, &blocks));
	CHECK(root == 2000 + volume && blocks == 1);
}

static void
filesystem_init(fixture_t *f, unsigned volume)
{
	fs_tree_t *tree = &f->trees[volume];

	drec_build(tree, 1, 2, "root", 4, ascii_hash("root"), 0);
	drec_build(tree, 1, 3, "private", 4, ascii_hash("private"), 0);
	inode_build(tree, 2, 1, "root", 4, 1, 0);
	inode_build(tree, 3, 1, "private", 4, 0, 0);
	drec_build(tree, 2, 16, "file", 8, ascii_hash("file"), 0);
	inode_build(tree, 16, 2, "file", 8, 1, 2 * BLOCK);
	stream_id_build(tree, 1);
	file_extent_build(tree, 0, DATA_BLOCK + 10 * volume, 2 * BLOCK);
	filesystem_build(f, volume);
}

/* Counts come from the explicit bitmap, independently of claim collection. */
static void
allocation_build(fixture_t *f)
{
	apfs_gen_chunk_t chunk = { .xid = 10, .block_count = BLOCKS,
		.bitmap_address = BITMAP_BLOCK };
	apfs_gen_chunks_t cib = {
		.object = { .oid = CIB_BLOCK, .xid = 10, .type = 0x40000007 },
		.chunks = &chunk, .count = 1
	};
	unsigned char *raw = block_at(f, 9);
	uint64_t free_count = 0;

	for (unsigned i = 0; i < BLOCKS; i++)
		free_count += (block_at(f, BITMAP_BLOCK)[i / 8] &
		    (1u << (i % 8))) == 0;
	chunk.free_count = (uint32_t)free_count;
	f->sm.devices[0].free_count = free_count;
	CHECK(apfs_gen_chunks_build(block_at(f, CIB_BLOCK), BLOCK, &cib));
	bzero(raw, BLOCK);
	CHECK(apfs_gen_spaceman_build(raw, BLOCK, &f->sm));
	fsimg_store_le64(raw + f->sm.ip_bm_xid_offset, 10);
	fsimg_store_le16(raw + f->sm.ip_bitmap_offset, 0);
	for (unsigned i = 0; i < f->sm.ip_bm_block_count; i++)
		fsimg_store_le16(raw + f->sm.ip_bm_free_next_offset + 2 * i,
		    i == 0 || i + 1 == f->sm.ip_bm_block_count ?
		    65535 : (uint16_t)(i + 1));
	fsimg_store_le64(raw + f->sm.devices[0].addr_offset, CIB_BLOCK);
	CHECK(apfs_gen_seal(raw, BLOCK));
}

static void
fixture_init_layout(fixture_t *f, unsigned volumes,
    apfs_gen_layout_t layout)
{
	apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = BLOCK, .oid = 500, .address = 9 },
		{ .type = 0x80000011, .size = BLOCK, .oid = 501, .address = 10 },
		{ .type = 0x80000002, .subtype = 9, .size = BLOCK,
		.oid = 502, .address = 11 },
		{ .type = 0x80000002, .subtype = 9, .size = BLOCK,
		.oid = 503, .address = 12 }
	};
	apfs_gen_run_t run = { .xid = 10, .next_xid = 11,
		.mappings = mappings, .mapping_count = ARRAY_COUNT(mappings),
		.mapping_blocks = 1, .desc_next = 2, .data_len = 4,
		.data_next = 4 };
	apfs_gen_container_t image = {
		.nx = { .magic = 0x4253584e, .block_size = BLOCK,
		.block_count = BLOCKS, .uuid = { 1 }, .incompat_features = 2,
		.desc_base = 1, .desc_blocks = 8, .data_base = 9,
		.data_blocks = 52, .max_volumes = 3, .spaceman_oid = 500,
		.reaper_oid = 501, .omap_oid = 90 },
		.runs = &run, .count = 1, .block0_xid = 10
	};
	apfs_gen_omap_record_t maps[3];
	fsimg_options_t options;
	fsimg_io_ops_t ops;

	bzero(f, sizeof(*f));
	f->length = IMAGE_BYTES;
	f->expected_files = 1;
	f->expected_directories = 1;
	f->expected_logical_bytes = 2 * BLOCK;
	f->bytes = calloc(1, IMAGE_BYTES);
	CHECK(f->bytes != NULL);
	f->trees = calloc(FIXTURE_TREES, sizeof(*f->trees));
	CHECK(f->trees != NULL);
	for (unsigned i = 0; i < volumes; i++)
		image.nx.volume_oids[i] = 1000 + i;
	CHECK(apfs_gen_container_build(f->bytes, IMAGE_BYTES, &image));
	if (layout == APFS_GEN_LAYOUT_OLDER)
		CHECK(apfs_gen_spaceman_geometry_older(BLOCK, BLOCKS,
		    &f->sm) == BLOCK);
	else
		CHECK(apfs_gen_spaceman_geometry(BLOCK, BLOCKS,
		    &f->sm) == BLOCK);
	f->sm.object = (apfs_gen_object_t){ .oid = 500, .xid = 10,
		.type = 0x80000005 };
	f->sm.ip_bm_base = 61;
	f->sm.ip_base = CIB_BLOCK;
	f->sm.ip_bm_free_head = 1;
	f->sm.ip_bm_free_tail = 15;
	for (unsigned i = 0; i < 83; i++)
		bit_set(block_at(f, BITMAP_BLOCK), i, true);
	bit_set(block_at(f, BITMAP_BLOCK), 90, true);
	bit_set(block_at(f, BITMAP_BLOCK), 91, true);
	bit_set(block_at(f, 61), 0, true);
	bit_set(block_at(f, 61), 1, true);
	for (unsigned i = 0; i < volumes; i++) {
		uint64_t base = VOLUME_BASE + 10 * i;
		apfs_gen_fs_t fs = {
			.object = { .oid = 1000 + i, .xid = 10, .type = 13 },
			.magic = 0x42535041, .index = i, .uuid = { 2, i + 1 },
			.name = "Verify", .flags = 1, .incompat_features = 9,
			.alloc_blocks = 7, .files = 1,
			.root_tree_type = 2, .extentref_tree_type = PHYSICAL_TREE,
			.snapshot_tree_type = PHYSICAL_TREE, .omap_oid = base + 1,
			.root_tree_oid = 2000 + i, .snapshot_tree_oid = base + 4,
			.extentref_tree_oid = base + 5
		};
		apfs_gen_omap_record_t fs_map = {
			.key = { .oid = 2000 + i, .xid = 10 },
			.value = { .size = BLOCK, .address = base + 3 }
		};

		maps[i] = (apfs_gen_omap_record_t){
			.key = { .oid = 1000 + i, .xid = 10 },
			.value = { .size = BLOCK, .address = base } };
		CHECK(apfs_gen_fs_build(block_at(f, base), BLOCK, &fs));
		map_build(f, base + 1, &fs_map, 1);
		filesystem_init(f, i);
		node_build(f, base + 4, base + 4, PHYSICAL_TREE, 16,
		    NULL, 0, 0, 0);
		extents_build(f, base + 5, DATA_BLOCK + 10 * i, 2);
		for (unsigned j = 0; j < 6; j++)
			bit_set(block_at(f, BITMAP_BLOCK), base + j, true);
		bit_set(block_at(f, BITMAP_BLOCK), DATA_BLOCK + 10 * i, true);
		bit_set(block_at(f, BITMAP_BLOCK), DATA_BLOCK + 10 * i + 1, true);
	}
	map_build(f, 90, maps, volumes);
	allocation_build(f);
	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

static void
fixture_init(fixture_t *f, unsigned volumes)
{
	fixture_init_layout(f, volumes, APFS_GEN_LAYOUT_CURRENT);
}

static void
fixture_close(fixture_t *f)
{
	fsimg_io_free(f->io);
	fsimg_ctx_free(f->ctx);
	free(f->trees);
	free(f->bytes);
}

static uint64_t
verify_fixture(fixture_t *f, unsigned flags, fsimg_status_t wanted,
    const char *message)
{
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_reader_t *reader = NULL;
	fsimg_io_t *slice = NULL;
	fsimg_status_t status;
	uint64_t work = 0;

	fsimg_read_options_init(&options);
	options.format = "apfs";
	options.flags = flags;
	options.volume_index_set = true;
	options.volume_index = 0;
	CHECK(fsimg_io_slice(f->io, 0, f->length, &slice, &error) == FSIMG_OK);
	status = fsimg_reader_open(f->ctx, slice, &options, &reader, &error);
	if (status_is(status, FSIMG_OK, &error, "")) {
		status = fsimg_verify(reader, 0, &report, &error);
		if (status_is(status, wanted, &error, message)) {
			if (wanted == FSIMG_EUNSUPPORTED)
				expect(!report.complete, "container report is incomplete");
			if (wanted == FSIMG_OK) {
				fsimg_volume_info_t info;

				expect(report.complete, "verification is complete");
				expect(report.files == f->expected_files,
				    "report counts non-directory entries");
				expect(report.directories == f->expected_directories,
				    "report counts directories including root");
				expect(report.logical_bytes == f->expected_logical_bytes,
				    "report counts logical bytes");
				expect(report.free_clusters == f->sm.devices[0].free_count,
				    "report gives main device free count");
				expect(report.allocated_clusters == BLOCKS -
				    f->sm.devices[0].free_count,
				    "allocated count is N minus free count");
				CHECK(fsimg_reader_get_info(reader, &info, &error) == FSIMG_OK);
				expect((info.valid & FSIMG_VOLUME_FREE) != 0,
				    "reader publishes verified free space");
			}
		}
	}
	if (reader != NULL)
		work = reader->session.progress.work;
	fsimg_reader_free(reader);
	fsimg_io_free(slice);
	return (work);
}

static void
test_valid_container_counts(void)
{
	fixture_t f;

	scenario = "valid container counts";
	fixture_init(&f, 1);
	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

static void
test_older_layout_container_verifies(void)
{
	fixture_t f;

	scenario = "older layout container verifies";
	fixture_init_layout(&f, 1, APFS_GEN_LAYOUT_OLDER);
	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

static void
test_allocation_damage(void)
{
	static const struct {
		const char *name;
		uint64_t block;
		bool allocated;
		const char *message;
	} cases[] = {
		{ "leaked block", 300, true, "allocated but claimed by nothing" },
		{ "claimed data is free", DATA_BLOCK, false, "claimed but free" },
		{ "claimed metadata is free", 103, false, "claimed but free" }
	};
	fixture_t f;

	for (unsigned i = 0; i < ARRAY_COUNT(cases); i++) {
		scenario = cases[i].name;
		fixture_init(&f, 1);
		bit_set(block_at(&f, BITMAP_BLOCK), cases[i].block,
		    cases[i].allocated);
		allocation_build(&f);
		verify_fixture(&f, 0, FSIMG_ECORRUPT, cases[i].message);
		fixture_close(&f);
	}
}

static void
test_double_claims(void)
{
	fixture_t f;

	scenario = "data overlaps object-map root";
	fixture_init(&f, 1);
	extents_build(&f, 105, 102, 2);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "claimed by both");
	fixture_close(&f);

	scenario = "data shared across volumes";
	fixture_init(&f, 2);
	extents_build(&f, 115, DATA_BLOCK, 2);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "claimed by both");
	fixture_close(&f);
}

/* The live and snapshot trees have separate nodes but share their data run. */
static void
test_snapshot_shares_data_with_live_volume(void)
{
	fixture_t f;
	unsigned char key[8], value[GEN_SNAP_META_VAL_BYTES + 5];
	apfs_gen_snap_meta_val_t meta = {
		.extentref_tree_oid = 300, .sblock_oid = 301,
		.extentref_tree_type = PHYSICAL_TREE, .name = "snap", .name_len = 4
	};
	apfs_gen_record_t record = { .key = key, .key_size = sizeof(key),
		.value = value, .value_size = sizeof(value) };
	unsigned char *raw;

	scenario = "snapshot shares data with live volume";
	fixture_init(&f, 1);
	CHECK(apfs_gen_jkey_build(key, sizeof(key), 9, 1));
	CHECK(apfs_gen_snap_meta_val_build(value, sizeof(value), &meta));
	node_build(&f, 104, 104, PHYSICAL_TREE, 16, &record, 1, 0, 0);
	extents_build(&f, 300, DATA_BLOCK, 2);
	bcopy(block_at(&f, 100), block_at(&f, 301), BLOCK);
	bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
	bit_set(block_at(&f, BITMAP_BLOCK), 301, true);
	raw = block_at(&f, 101);
	fsimg_store_le32(raw + 36, 1);
	fsimg_store_le64(raw + 64, 9);
	CHECK(apfs_gen_seal(raw, BLOCK));
	allocation_build(&f);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "has snapshots");
	fixture_close(&f);
}

static void
test_claim_outside_container(void)
{
	fixture_t f;

	scenario = "data claim outside container";
	fixture_init(&f, 1);
	extents_build(&f, 105, BLOCKS - 1, 2);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "outside the container");
	fixture_close(&f);
}

static void
test_volume_allocated_block_count(void)
{
	fixture_t f;
	unsigned char *raw;

	for (unsigned count = 6; count <= 8; count++) {
		scenario = "volume allocated block count";
		fixture_init(&f, 1);
		raw = block_at(&f, VOLUME_BASE);
		fsimg_store_le64(raw + 88, count);
		CHECK(apfs_gen_seal(raw, BLOCK));
		verify_fixture(&f, 0,
		    count == 7 ? FSIMG_OK : FSIMG_ECORRUPT,
		    count == 7 ? "" :
		    "allocated block count differs from its trees");
		fixture_close(&f);
	}
}

static void
test_volume_uuids_must_be_distinct(void)
{
	fixture_t f;

	scenario = "two volumes have the same UUID";
	fixture_init(&f, 2);
	bcopy(block_at(&f, 100) + 240, block_at(&f, 110) + 240, 16);
	CHECK(apfs_gen_seal(block_at(&f, 110), BLOCK));
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "has the UUID of another volume");
	fixture_close(&f);
}

/* Name keys at OID 1 precede metadata at XIDs 8 and 9. */
static void
test_snapshot_names_order_by_stored_bytes(void)
{
	fixture_t f;
	unsigned char meta_keys[2][8], meta_values[2][GEN_SNAP_META_VAL_BYTES + 3];
	unsigned char name_keys[2][13], name_values[2][8];
	apfs_gen_record_t records[4], reversed[4];
	const char *const names[] = { "aa", "b" };
	unsigned char *raw;

	scenario = "snapshot names aa then b in byte order";
	fixture_init(&f, 1);
	for (unsigned i = 0; i < 2; i++) {
		apfs_gen_snap_meta_val_t meta = {
			.extentref_tree_oid = 300 + i, .sblock_oid = 302 + i,
			.extentref_tree_type = PHYSICAL_TREE,
			.name = names[i], .name_len = (uint16_t)strlen(names[i])
		};
		apfs_gen_name_key_t name = { .name = names[i], .oid = 1,
			.name_len = (uint16_t)strlen(names[i]) };
		size_t meta_size = GEN_SNAP_META_VAL_BYTES + name.name_len + 1;
		size_t name_size = 10 + name.name_len + 1;

		CHECK(apfs_gen_jkey_build(meta_keys[i], 8, 8 + i, 1));
		CHECK(apfs_gen_snap_meta_val_build(meta_values[i], meta_size, &meta));
		CHECK(apfs_gen_snap_name_key_build(name_keys[i], name_size, &name));
		CHECK(apfs_gen_snap_name_val_build(name_values[i], 8, 8 + i));
		records[2 + i] = (apfs_gen_record_t){ .key = meta_keys[i],
		    .key_size = 8, .value = meta_values[i], .value_size = meta_size };
		records[i] = (apfs_gen_record_t){ .key = name_keys[i],
		    .key_size = name_size, .value = name_values[i], .value_size = 8 };
		extents_build(&f, 300 + i, DATA_BLOCK, 2);
		bit_set(block_at(&f, BITMAP_BLOCK), 300 + i, true);
	}
	raw = block_at(&f, 101);
	fsimg_store_le32(raw + 36, 2);
	fsimg_store_le64(raw + 64, 9);
	CHECK(apfs_gen_seal(raw, BLOCK));
	allocation_build(&f);
	node_build(&f, 104, 104, PHYSICAL_TREE, 16, records, 4, 0, 0);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "has snapshots");

	scenario = "snapshot names b then aa in length order";
	bcopy(records, reversed, sizeof(records));
	reversed[0] = records[1];
	reversed[1] = records[0];
	node_build(&f, 104, 104, PHYSICAL_TREE, 16, reversed, 4, 0, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "keys out of order");
	fixture_close(&f);
}

/* Unused versions still belong to the checkpoint's object-map history. */
static void
test_object_map_versions_reject_zero_and_future_xids(void)
{
	static const uint64_t xids[] = { 11, 0 };
	fixture_t f;

	for (unsigned volume = 0; volume < 2; volume++) {
		for (unsigned i = 0; i < ARRAY_COUNT(xids); i++) {
			apfs_gen_omap_record_t maps[] = {
				{ .key = { .oid = volume ? 2000 : 1000, .xid = 10 },
				.value = { .size = BLOCK,
				.address = volume ? 103 : VOLUME_BASE } },
				{ .key = { .oid = 3000, .xid = xids[i] },
				.value = { .size = BLOCK, .address = 300 } }
			};

			scenario = volume ? (xids[i] == 0 ?
			    "volume mapping has zero XID" :
			    "volume mapping has future XID") : (xids[i] == 0 ?
			    "container mapping has zero XID" :
			    "container mapping has future XID");
			fixture_init(&f, 1);
			map_build(&f, volume ? 101 : 90, maps, ARRAY_COUNT(maps));
			bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
			if (volume) {
				fsimg_store_le64(block_at(&f, VOLUME_BASE) + 88, 8);
				CHECK(apfs_gen_seal(block_at(&f, VOLUME_BASE), BLOCK));
			}
			allocation_build(&f);
			verify_fixture(&f, 0, FSIMG_ECORRUPT, xids[i] == 0 ?
			    "maps a version of XID zero" :
			    "maps a version newer than the checkpoint");
			fixture_close(&f);
		}
	}
}

static void
test_refused_superblock_does_not_reuse_volume_uuid(void)
{
	fixture_t f;

	scenario = "two volumes refused before UUID decode";
	fixture_init(&f, 3);
	for (unsigned i = 1; i < 3; i++) {
		unsigned char *raw = block_at(&f, VOLUME_BASE + 10 * i);

		fsimg_store_le32(raw + 24, 13 | APFS_OBJ_ENCRYPTED);
		CHECK(apfs_gen_seal(raw, BLOCK));
	}
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "is encrypted or nonpersistent");
	fixture_close(&f);
}

static void
test_decoded_volume_uuid_must_not_be_zero(void)
{
	fixture_t f;

	scenario = "decoded second volume has no UUID";
	fixture_init(&f, 2);
	bzero(block_at(&f, 110) + 240, 16);
	CHECK(apfs_gen_seal(block_at(&f, 110), BLOCK));
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "has no UUID");
	fixture_close(&f);
}

static void
test_snapshot_xids_must_not_exceed_checkpoint(void)
{
	fixture_t f;
	unsigned char meta_key[8], meta_value[GEN_SNAP_META_VAL_BYTES + 5];
	unsigned char name_key[15], name_value[8];
	apfs_gen_snap_meta_val_t meta = {
		.extentref_tree_oid = 300, .sblock_oid = 301,
		.extentref_tree_type = PHYSICAL_TREE, .name = "snap", .name_len = 4
	};
	apfs_gen_name_key_t name = { .name = "snap", .oid = 1, .name_len = 4 };
	apfs_gen_record_t records[] = {
		{ .key = name_key, .key_size = sizeof(name_key),
		.value = name_value, .value_size = sizeof(name_value) },
		{ .key = meta_key, .key_size = sizeof(meta_key),
		.value = meta_value, .value_size = sizeof(meta_value) }
	};
	unsigned char *raw;

	for (unsigned metadata = 0; metadata < 2; metadata++) {
		scenario = metadata ? "snapshot metadata has future XID" :
		    "snapshot name has future XID";
		fixture_init(&f, 1);
		CHECK(apfs_gen_jkey_build(meta_key, sizeof(meta_key),
		    metadata ? 11 : 9, 1));
		CHECK(apfs_gen_snap_meta_val_build(meta_value, sizeof(meta_value),
		    &meta));
		CHECK(apfs_gen_snap_name_key_build(name_key, sizeof(name_key), &name));
		CHECK(apfs_gen_snap_name_val_build(name_value, sizeof(name_value), 11));
		/* Isolate the metadata key from the name value's XID check. */
		node_build(&f, 104, 104, PHYSICAL_TREE, 16,
		    metadata ? &records[1] : records,
		    metadata ? 1 : ARRAY_COUNT(records), 0, 0);
		extents_build(&f, 300, DATA_BLOCK, 2);
		bcopy(block_at(&f, 100), block_at(&f, 301), BLOCK);
		bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
		bit_set(block_at(&f, BITMAP_BLOCK), 301, true);
		raw = block_at(&f, 101);
		fsimg_store_le32(raw + 36, 1);
		fsimg_store_le64(raw + 64, metadata ? 11 : 9);
		CHECK(apfs_gen_seal(raw, BLOCK));
		allocation_build(&f);
		verify_fixture(&f, 0, FSIMG_ECORRUPT,
		    "names a snapshot newer than the checkpoint");
		fixture_close(&f);
	}
}

static void
test_snapshot_count_disagrees_with_tree(void)
{
	fixture_t f;
	unsigned char *raw;

	scenario = "volume object map disagrees with empty snapshot tree";
	fixture_init(&f, 1);
	raw = block_at(&f, 101);
	fsimg_store_le32(raw + 36, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "disagrees with the snapshot tree");
	fixture_close(&f);
}

static void
test_extent_records_in_one_tree_must_not_overlap(void)
{
	fixture_t f;
	unsigned char keys[2][8], values[2][GEN_PEXT_VAL_BYTES];
	apfs_gen_record_t records[2];
	apfs_gen_pext_val_t extent = { .length = 2, .owner = 42,
		.refcnt = 1, .kind = 1 };

	scenario = "extent records in one tree overlap";
	fixture_init(&f, 1);
	for (unsigned i = 0; i < 2; i++) {
		CHECK(apfs_gen_jkey_build(keys[i], sizeof(keys[i]),
		    DATA_BLOCK + i, 2));
		CHECK(apfs_gen_pext_val_build(values[i], sizeof(values[i]),
		    &extent));
		records[i] = (apfs_gen_record_t){ .key = keys[i], .key_size = 8,
		    .value = values[i], .value_size = GEN_PEXT_VAL_BYTES };
	}
	node_build(&f, 105, 105, PHYSICAL_TREE, 15, records, 2, 0, 0);
	bit_set(block_at(&f, BITMAP_BLOCK), DATA_BLOCK + 2, true);
	allocation_build(&f);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "has overlapping records");

	scenario = "overlapping extent records outrank busy reaper";
	fsimg_store_le32(block_at(&f, 10) + 64, 3);
	CHECK(apfs_gen_seal(block_at(&f, 10), BLOCK));
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "has overlapping records");
	fixture_close(&f);
}

static void
test_free_count_mismatches(void)
{
	fixture_t f;
	unsigned char *raw;

	for (unsigned device = 0; device < 2; device++) {
		scenario = device ? "device free-count mismatch" :
		    "chunk free-count mismatch";
		fixture_init(&f, 1);
		raw = block_at(&f, device ? 9 : CIB_BLOCK);
		if (device)
			fsimg_store_le64(raw + 72, f.sm.devices[0].free_count + 1);
		else
			fsimg_store_le32(raw + 60,
			    (uint32_t)f.sm.devices[0].free_count + 1);
		CHECK(apfs_gen_seal(raw, BLOCK));
		verify_fixture(&f, 0, FSIMG_ECORRUPT, "free count differs");
		fixture_close(&f);
	}
}

/* Incomplete ownership skips allocation judgments; structural damage wins. */
static void
test_incomplete_containers_preserve_corruption_errors(void)
{
	static const struct {
		const char *name;
		fsimg_status_t status;
		const char *message;
	} damage_cases[] = {
		{ "intact", FSIMG_EUNSUPPORTED, NULL },
		{ "leak", FSIMG_EUNSUPPORTED, NULL },
		{ "free claim", FSIMG_EUNSUPPORTED, NULL },
		{ "double claim", FSIMG_EUNSUPPORTED, NULL },
		{ "out-of-domain claim", FSIMG_ECORRUPT,
		    "outside the container" },
		{ "snapshot count disagreement", FSIMG_ECORRUPT,
		    "disagrees with the snapshot tree" },
		{ "malformed extent", FSIMG_ECORRUPT,
		    "has a value of the wrong length" }
	};
	static const struct {
		const char *name;
		const char *message;
		uint64_t block;
		unsigned offset;
		uint64_t value;
	} reasons[] = {
		{ "busy reaper", "pending reap", 10, 64, 3 },
		{ "encrypted volume", "is encrypted", 110, 264, 0 },
		{ "sealed volume", "is sealed", 110, 56, 0x29 },
		{ "snapshot extension", "holds structures", 110, 1000, 300 },
		{ "integrity tree", "holds structures", 110, 1024, 300 },
		{ "fext tree", "holds structures", 110, 1032, 300 },
		{ "unsupported volume map", "has unsupported flags",
		    111, 32, 0x80000000 },
		{ "EFI jumpstart", "has an EFI jumpstart", 2, 1272, 300 }
	};
	fixture_t f;
	unsigned char *raw;
	char label[128];
	const char *message;

	for (unsigned reason = 0; reason < ARRAY_COUNT(reasons); reason++) {
		for (unsigned damage = 0; damage < ARRAY_COUNT(damage_cases);
		    damage++) {
			CHECK(snprintf(label, sizeof(label), "%s with %s",
			    reasons[reason].name, damage_cases[damage].name) <
			    (int)sizeof(label));
			scenario = label;
			fixture_init(&f, reason == 0 ? 1 : 2);
			raw = block_at(&f, reasons[reason].block);
			if (reasons[reason].block == 10 ||
			    reasons[reason].block == 111)
				fsimg_store_le32(raw + reasons[reason].offset,
				    (uint32_t)reasons[reason].value);
			else
				fsimg_store_le64(raw + reasons[reason].offset,
				    reasons[reason].value);
			CHECK(apfs_gen_seal(raw, BLOCK));
			if (reasons[reason].block == 2) {
				fsimg_store_le64(block_at(&f, 0) + 1272, 300);
				CHECK(apfs_gen_seal(block_at(&f, 0), BLOCK));
			}
			if (damage == 1)
				bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
			if (damage == 2)
				bit_set(block_at(&f, BITMAP_BLOCK), DATA_BLOCK, false);
			if (damage == 3)
				extents_build(&f, 105, 102, 2);
			if (damage == 4)
				extents_build(&f, 105, BLOCKS - 1, 2);
			if (damage == 5) {
				raw = block_at(&f, 101);
				fsimg_store_le32(raw + 36, 1);
				CHECK(apfs_gen_seal(raw, BLOCK));
			}
			if (damage == 6) {
				unsigned char key[8], value[1] = { 0 };
				apfs_gen_record_t record = { .key = key,
				    .key_size = sizeof(key), .value = value,
				    .value_size = sizeof(value) };

				CHECK(apfs_gen_jkey_build(key, sizeof(key),
				    DATA_BLOCK, 2));
				node_build(&f, 105, 105, PHYSICAL_TREE, 15,
				    &record, 1, 0, 0);
			}
			allocation_build(&f);
			message = damage_cases[damage].message;
			if (message == NULL)
				message = reasons[reason].message;
			verify_fixture(&f, 0, damage_cases[damage].status, message);
			fixture_close(&f);
		}
	}
}

static void
queue_build(fixture_t *f, unsigned queue, unsigned records, uint64_t count,
    uint64_t first)
{
	unsigned char key[16], value[8];
	apfs_gen_fq_key_t fq_key = { .xid = 10, .address = first };
	apfs_gen_record_t record = { .key = key, .key_size = 16,
		.value = value, .value_size = 8 };

	CHECK(apfs_gen_fq_key_build(key, sizeof(key), &fq_key));
	CHECK(apfs_gen_fq_value_build(value, sizeof(value), 1));
	node_build(f, queue == 0 ? 12 : 11, queue == 0 ? 503 : 502,
	    0x80000002, 9, &record, records, 16, 8);
	f->sm.free_queues[queue].tree_oid = queue == 0 ? 503 : 502;
	f->sm.free_queues[queue].count = count;
	f->sm.free_queues[queue].oldest_xid = 10;
	allocation_build(f);
}

static void
test_empty_free_queues_reject_future_oldest_xid(void)
{
	fixture_t f;

	for (unsigned queue = 0; queue < 2; queue++) {
		for (unsigned tree = 0; tree < 2; tree++) {
			scenario = queue == 0 ? (tree ?
			    "empty pool queue tree has future oldest XID" :
			    "zero pool queue OID has future oldest XID") : (tree ?
			    "empty main queue tree has future oldest XID" :
			    "zero main queue OID has future oldest XID");
			fixture_init(&f, 1);
			if (tree)
				queue_build(&f, queue, 0, 0, 300);
			f.sm.free_queues[queue].oldest_xid = 11;
			allocation_build(&f);
			verify_fixture(&f, 0, FSIMG_ECORRUPT,
			    "has an oldest XID newer than the checkpoint");
			fixture_close(&f);
		}
	}
}

static void
test_free_queue_records_reject_future_xids(void)
{
	fixture_t f;
	unsigned char key[16], value[8];
	apfs_gen_fq_key_t fq_key = { .xid = 11 };
	apfs_gen_record_t record = { .key = key, .key_size = sizeof(key),
		.value = value, .value_size = sizeof(value) };

	for (unsigned queue = 0; queue < 2; queue++) {
		for (unsigned ghost = 0; ghost < 2; ghost++) {
			scenario = queue == 0 ? (ghost ?
			    "pool ghost record has future XID" :
			    "pool record has future XID") : (ghost ?
			    "main ghost record has future XID" :
			    "main record has future XID");
			fixture_init(&f, 1);
			fq_key.address = queue == 0 ? POOL_QUEUE_BLOCK : 300;
			CHECK(apfs_gen_fq_key_build(key, sizeof(key), &fq_key));
			CHECK(apfs_gen_fq_value_build(value, sizeof(value), 1));
			record.ghost = ghost != 0;
			if (queue == 0)
				bit_set(block_at(&f, 61),
				    POOL_QUEUE_BLOCK - CIB_BLOCK, true);
			else
				bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
			queue_build(&f, queue, 1, 1, fq_key.address);
			node_build(&f, queue == 0 ? 12 : 11,
			    queue == 0 ? 503 : 502, 0x80000002, 9,
			    &record, 1, 16, 8);
			verify_fixture(&f, 0, FSIMG_ECORRUPT,
			    "has a record newer than the checkpoint");
			fixture_close(&f);
		}
	}
}

static void
test_unsupported_container_mapping_preserves_queue_damage(void)
{
	apfs_gen_omap_record_t maps[] = {
		{ .key = { .oid = 1000, .xid = 10 },
		.value = { .size = BLOCK, .address = VOLUME_BASE } },
		{ .key = { .oid = 3000, .xid = 10 },
		.value = { .flags = APFS_OMAP_VAL_ENCRYPTED,
		.size = BLOCK, .address = 300 } }
	};
	unsigned char key[16], value[8];
	apfs_gen_fq_key_t fq_key = { .xid = 11, .address = 301 };
	apfs_gen_record_t record = { .key = key, .key_size = sizeof(key),
		.value = value, .value_size = sizeof(value) };
	fixture_t f;

	scenario = "unsupported container mapping without queue damage";
	fixture_init(&f, 1);
	map_build(&f, 90, maps, ARRAY_COUNT(maps));
	bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
	allocation_build(&f);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "maps an encrypted or headerless object");

	scenario = "unsupported container mapping preserves future queue record";
	bit_set(block_at(&f, BITMAP_BLOCK), 301, true);
	queue_build(&f, 1, 1, 1, 301);
	CHECK(apfs_gen_fq_key_build(key, sizeof(key), &fq_key));
	CHECK(apfs_gen_fq_value_build(value, sizeof(value), 1));
	node_build(&f, 11, 502, 0x80000002, 9, &record, 1, 16, 8);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "has a record newer than the checkpoint");
	fixture_close(&f);
}

static void
test_free_queues(void)
{
	fixture_t f;

	scenario = "empty main free-queue leaf";
	fixture_init(&f, 1);
	queue_build(&f, 1, 0, 0, 300);
	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);

	scenario = "nonempty main free queue";
	fixture_init(&f, 1);
	bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
	queue_build(&f, 1, 1, 1, 300);
	verify_fixture(&f, 0, FSIMG_OK, "");
	queue_build(&f, 1, 1, 2, 300);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "count differs from its records");
	fixture_close(&f);

	scenario = "free queue counts blocks without tree";
	fixture_init(&f, 1);
	f.sm.free_queues[1].count = 1;
	allocation_build(&f);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "counts blocks but has no tree");
	fixture_close(&f);
}

/* A refused mapped value leaves this volume incomplete, then reading continues. */
static void
test_unsupported_mapped_object_preserves_later_damage(void)
{
	apfs_gen_omap_record_t maps[] = {
		{ .key = { .oid = 2000, .xid = 10 },
		.value = { .size = BLOCK, .address = 103 } },
		{ .key = { .oid = 2001, .xid = 10 },
		.value = { .flags = APFS_OMAP_VAL_ENCRYPTED,
		.size = BLOCK, .address = 300 } }
	};
	fixture_t f;
	unsigned char *raw;

	scenario = "unsupported mapped object suppresses allocation judgments";
	fixture_init(&f, 2);
	map_build(&f, 101, maps, ARRAY_COUNT(maps));
	/* The encrypted mapping owns one additional volume block. */
	fsimg_store_le64(block_at(&f, VOLUME_BASE) + 88, 8);
	CHECK(apfs_gen_seal(block_at(&f, VOLUME_BASE), BLOCK));
	bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
	bit_set(block_at(&f, BITMAP_BLOCK), DATA_BLOCK, false);
	extents_build(&f, 115, 112, 2);
	allocation_build(&f);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "maps an encrypted or headerless object");

	scenario = "unsupported mapped object preserves later volume damage";
	extents_build(&f, 115, BLOCKS - 1, 2);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "outside the container");

	scenario = "busy reaper is the first incomplete reason";
	extents_build(&f, 115, 112, 2);
	raw = block_at(&f, 10);
	fsimg_store_le32(raw + 64, 3);
	CHECK(apfs_gen_seal(raw, BLOCK));
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "pending reap");

	scenario = "free queue damage outranks busy reaper";
	queue_build(&f, 1, 1, 2, 300);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "count differs from its records");
	fixture_close(&f);
}

static void
test_refused_volume_map_preserves_extent_damage(void)
{
	fixture_t f;
	unsigned char *raw;

	scenario = "refused volume map leaves verification incomplete";
	fixture_init(&f, 2);
	/* Reader open needs the selected volume's map, so refuse the other. */
	raw = block_at(&f, 111);
	fsimg_store_le32(raw + 32, APFS_OMAP_ENCRYPTING);
	CHECK(apfs_gen_seal(raw, BLOCK));
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "has unsupported flags");

	scenario = "refused volume map preserves its live extent damage";
	extents_build(&f, 115, BLOCKS - 1, 2);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "outside the container");
	fixture_close(&f);
}

static void
test_pool_claims(void)
{
	fixture_t f;

	scenario = "internal pool queue allocated";
	fixture_init(&f, 1);
	bit_set(block_at(&f, 61), POOL_QUEUE_BLOCK - CIB_BLOCK, true);
	queue_build(&f, 0, 1, 1, POOL_QUEUE_BLOCK);
	verify_fixture(&f, 0, FSIMG_OK, "");
	bit_set(block_at(&f, 61), POOL_QUEUE_BLOCK - CIB_BLOCK, false);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "claimed but free");
	fixture_close(&f);

	scenario = "internal pool space-manager block free";
	fixture_init(&f, 1);
	bit_set(block_at(&f, 61), 0, false);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "claimed but free");
	fixture_close(&f);
}

static void
test_truncation_and_assumed_identity(void)
{
	fixture_t f;

	scenario = "truncated container";
	fixture_init(&f, 1);
	f.length -= BLOCK;
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "truncated");
	fixture_close(&f);

	scenario = "assumed APFS identity";
	fixture_init(&f, 1);
	fsimg_store_le32(block_at(&f, 0) + 32, 0);
	CHECK(apfs_gen_seal(block_at(&f, 0), BLOCK));
	verify_fixture(&f, FSIMG_READ_ASSUME_IDENTITY, FSIMG_ECORRUPT,
	    "apfs identity was assumed: NXSB magic at offset 32 of block 0");
	fixture_close(&f);
}

/* Count mutations target the inode value's documented count field. */
static void
inode_count_set(fs_tree_t *tree, uint64_t oid, uint32_t count)
{
	fs_record_t *r = record_find(tree, oid, 3);

	fsimg_store_le32(r->value + 56, count);
}

static void
add_second_file_name(fixture_t *f)
{
	fs_tree_t *tree = &f->trees[0];

	drec_build(tree, 2, 16, "alias", 8, ascii_hash("alias"), 0);
	inode_count_set(tree, 2, 2);
}

static void
test_link_count_differs_from_names_is_corrupt(void)
{
	fixture_t f;

	scenario = "file with two names and link count one";
	fixture_init(&f, 1);
	add_second_file_name(&f);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "link count differs from its names");
	fixture_close(&f);
}

static void
test_child_count_differs_from_records_is_corrupt(void)
{
	fixture_t f;

	scenario = "root child count differs from its directory records";
	fixture_init(&f, 1);
	inode_count_set(&f.trees[0], 2, 2);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "child count differs from its directory records");
	fixture_close(&f);
}

static void
test_stream_reference_count_differs_from_inode_users_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;

	scenario = "stream reference count two with one inode user";
	fixture_init(&f, 1);
	r = record_find(&f.trees[0], 16, 6);
	CHECK(apfs_gen_dstream_id_val_build(r->value, r->value_size, 2));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "reference count differs from its inodes");
	fixture_close(&f);
}

static void
test_multiple_names_without_sibling_links_is_corrupt(void)
{
	fixture_t f;

	scenario = "file with two names but no sibling links";
	fixture_init(&f, 1);
	add_second_file_name(&f);
	inode_count_set(&f.trees[0], 16, 2);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "links differ from the inode's names");
	fixture_close(&f);
}

static void
test_name_with_sibling_id_without_link_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *name;

	scenario = "single name carries a sibling ID without a link";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	name = record_find(tree, 2, 9);
	drec_build(tree, 2, 16, "file", 8, ascii_hash("file"), 500);
	*name = tree->records[--tree->count];
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "carries a sibling ID its inode has no link for");
	fixture_close(&f);
}

static void
test_sibling_link_without_name_sibling_id_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;
	apfs_gen_sibling_val_t link = { .parent = 2, .name = "file",
		.name_len = 4 };

	scenario = "sibling link names a directory record without its sibling ID";
	fixture_init(&f, 1);
	r = record_add(&f.trees[0]);
	r->key_size = GEN_SIBLING_KEY_BYTES;
	r->value_size = GEN_SIBLING_VAL_BYTES + link.name_len + 1;
	CHECK(apfs_gen_sibling_key_build(r->key, r->key_size, 16, 500));
	CHECK(apfs_gen_sibling_val_build(r->value, r->value_size, &link));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "APFS sibling link links differ from the inode's names");
	fixture_close(&f);
}

static void
test_sibling_link_name_differs_from_directory_record_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *name, *r;
	apfs_gen_sibling_val_t link = { .parent = 2, .name = "alias",
		.name_len = 5 };

	scenario = "sibling link and directory record share an ID but differ in name";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	name = record_find(tree, 2, 9);
	drec_build(tree, 2, 16, "file", 8, ascii_hash("file"), 500);
	*name = tree->records[--tree->count];
	r = record_add(tree);
	r->key_size = GEN_SIBLING_KEY_BYTES;
	r->value_size = GEN_SIBLING_VAL_BYTES + link.name_len + 1;
	CHECK(apfs_gen_sibling_key_build(r->key, r->key_size, 16, 500));
	CHECK(apfs_gen_sibling_val_build(r->value, r->value_size, &link));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "differs from its directory record");
	fixture_close(&f);
}

static void
test_name_sibling_id_differs_from_link_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *name, *r;
	apfs_gen_sibling_val_t link = { .parent = 2, .name = "file",
		.name_len = 4 };

	scenario = "directory record carries an ID below its inode's sibling link ID";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	name = record_find(tree, 2, 9);
	drec_build(tree, 2, 16, "file", 8, ascii_hash("file"), 499);
	*name = tree->records[--tree->count];
	r = record_add(tree);
	r->key_size = GEN_SIBLING_KEY_BYTES;
	r->value_size = GEN_SIBLING_VAL_BYTES + link.name_len + 1;
	CHECK(apfs_gen_sibling_key_build(r->key, r->key_size, 16, 500));
	CHECK(apfs_gen_sibling_val_build(r->value, r->value_size, &link));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "carries a sibling ID no sibling link has");
	fixture_close(&f);
}

/* Separate inodes keep link reconciliation valid until the collision sort. */
static void
add_collision(fixture_t *f, unsigned volume, const char *first,
    const char *second, uint32_t hash)
{
	fs_tree_t *tree = &f->trees[volume];
	unsigned char *raw = block_at(f, VOLUME_BASE + 10 * volume);

	inode_count_set(tree, 2, 3);
	inode_build(tree, 17, 2, first, 8, 1, 0);
	inode_build(tree, 18, 2, second, 8, 1, 0);
	drec_build(tree, 2, 17, first, 8, hash, 0);
	drec_build(tree, 2, 18, second, 8, hash, 0);
	fsimg_store_le64(raw + 184, 3);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(f, volume);
}

static void
test_case_equivalent_names_in_one_directory_is_corrupt(void)
{
	fixture_t f;

	scenario = "case insensitive directory contains a and A with correct hashes";
	fixture_init(&f, 1);
	add_collision(&f, 0, "a", "A", ascii_hash("a"));
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "directory holds names the volume treats as equal");
	fixture_close(&f);
}

static void
test_normalization_equivalent_stored_spellings_is_corrupt(void)
{
	static const uint32_t nfd[] = { 'e', 0x301 };
	fixture_t f;
	unsigned char *raw;

	/* NFC and NFD differ as bytes but both hash their NFD scalar form. */
	scenario = "normalization insensitive directory contains NFC and NFD names";
	fixture_init(&f, 1);
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 56, 8);
	CHECK(apfs_gen_seal(raw, BLOCK));
	add_collision(&f, 0, "\xc3\xa9", "e\xcc\x81",
	    apfs_gen_name_hash(nfd, ARRAY_COUNT(nfd)));
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "directory holds names the volume treats as equal");
	fixture_close(&f);
}

static void
test_collision_in_nonselected_volume_is_corrupt(void)
{
	fixture_t f;

	scenario = "nonselected volume contains equivalent sibling names";
	fixture_init(&f, 2);
	add_collision(&f, 1, "a", "A", ascii_hash("a"));
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "directory holds names the volume treats as equal");
	fixture_close(&f);
}

static void
test_extent_gap_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;
	apfs_gen_extent_val_t extent = { .length = BLOCK, .phys_block = DATA_BLOCK };

	scenario = "gap between file extents";
	fixture_init(&f, 1);
	r = record_find(&f.trees[0], 16, 8);
	CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &extent));
	file_extent_build(&f.trees[0], 2 * BLOCK, DATA_BLOCK + 1, BLOCK);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "extents do not tile their stream");
	fixture_close(&f);
}

static void
test_first_extent_after_zero_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;

	scenario = "first file extent starts after zero";
	fixture_init(&f, 1);
	r = record_find(&f.trees[0], 16, 8);
	CHECK(apfs_gen_extent_key_build(r->key, r->key_size, 16, BLOCK));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT, "extents do not tile their stream");
	fixture_close(&f);
}

static void
test_file_extent_outside_physical_records_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;
	apfs_gen_extent_val_t extent = { .length = 2 * BLOCK, .phys_block = 300 };

	scenario = "file extent outside every physical extent record";
	fixture_init(&f, 1);
	r = record_find(&f.trees[0], 16, 8);
	CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &extent));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "file extent outside every physical-extent record");
	fixture_close(&f);
}

static void
test_file_inode_parent_differs_from_its_only_name_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	unsigned char *raw;

	scenario = "file inode parent differs from its only root directory record";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_build(tree, 17, 2, "subdir", 4, 0, 0);
	drec_build(tree, 2, 17, "subdir", 4, ascii_hash("subdir"), 0);
	inode_count_set(tree, 2, 2);
	r = record_find(tree, 16, 3);
	fsimg_store_le64(r->value, 17);
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "parent or name field differs from its names");
	fixture_close(&f);
}

static void
test_file_inode_name_differs_from_its_only_name_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;

	scenario = "file inode name differs from its only directory record";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	r = record_find(tree, 16, 3);
	inode_build(tree, 16, 2, "other", 8, 1, 2 * BLOCK);
	*r = tree->records[--tree->count];
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "parent or name field differs from its names");
	fixture_close(&f);
}

static void
test_symlink_named_only_in_private_directory_is_incomplete(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	unsigned char *raw;
	apfs_gen_name_key_t key = { .oid = 18, .name = "com.apple.fs.symlink",
		.name_len = sizeof("com.apple.fs.symlink") - 1 };
	apfs_gen_xattr_val_t target = { .flags = 6,
		.data = (const unsigned char *)"file", .data_len = 5 };

	scenario = "symlink named only in the private directory";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_count_set(tree, APFS_INO_PRIVATE, 1);
	inode_build(tree, 18, APFS_INO_PRIVATE, "link", 10, 1, 0);
	drec_build(tree, APFS_INO_PRIVATE, 18, "link", 10,
	    ascii_hash("link"), 0);
	r = record_add(tree);
	r->key_size = GEN_NAME_KEY_BYTES + key.name_len + 1;
	r->value_size = GEN_XATTR_VAL_BYTES + target.data_len;
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &target));
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 200, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "private directory holds entries the walk from the root does not reach");
	fixture_close(&f);
}

static void
test_file_inode_under_private_directory_without_a_name_is_incomplete(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;

	scenario = "file inode under the private directory has no directory record";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	r = record_find(tree, APFS_INO_ROOT, 9);
	*r = tree->records[--tree->count];
	r = record_find(tree, 16, 3);
	fsimg_store_le64(r->value, APFS_INO_PRIVATE);
	inode_count_set(tree, APFS_INO_ROOT, 0);
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "private directory holds entries the walk from the root does not reach");
	fixture_close(&f);
}

static void
verify_hard_link_inode_name(uint64_t parent, const char *name)
{
	static const struct {
		uint64_t parent;
		uint64_t sibling;
		const char *name;
	} links[] = {
		{ .parent = 2, .sibling = 500, .name = "file" },
		{ .parent = 17, .sibling = 501, .name = "alias" }
	};
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	unsigned char *raw;

	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_build(tree, 17, 2, "subdir", 4, 1, 0);
	drec_build(tree, 2, 17, "subdir", 4, ascii_hash("subdir"), 0);
	inode_count_set(tree, 2, 2);
	r = record_find(tree, 16, 3);
	inode_build(tree, 16, parent, name, 8, 2, 2 * BLOCK);
	*r = tree->records[--tree->count];
	/* Replace the original untagged name with its sibling-tagged record. */
	r = record_find(tree, 2, 9);
	*r = tree->records[--tree->count];
	for (size_t i = 0; i < ARRAY_COUNT(links); i++) {
		apfs_gen_sibling_val_t link = { .parent = links[i].parent,
			.name = links[i].name,
			.name_len = (uint16_t)strlen(links[i].name) };

		drec_build(tree, link.parent, 16, link.name, 8,
		    ascii_hash(link.name), links[i].sibling);
		r = record_add(tree);
		r->key_size = GEN_SIBLING_KEY_BYTES;
		r->value_size = GEN_SIBLING_VAL_BYTES + link.name_len + 1;
		CHECK(apfs_gen_sibling_key_build(r->key, r->key_size, 16,
		    links[i].sibling));
		CHECK(apfs_gen_sibling_val_build(r->value, r->value_size, &link));
		r = record_add(tree);
		r->key_size = GEN_JKEY_BYTES;
		r->value_size = GEN_SIBLING_MAP_VAL_BYTES;
		CHECK(apfs_gen_jkey_build(r->key, r->key_size,
		    links[i].sibling, 12));
		CHECK(apfs_gen_sibling_map_val_build(r->value, r->value_size, 16));
	}
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);
	f.expected_files = 2;
	f.expected_directories = 2;
	f.expected_logical_bytes = 4 * BLOCK;

	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

static void
test_hard_link_inode_identifying_first_link_verifies_completely(void)
{
	scenario = "hard link inode identifies the first name with lower parent number";
	verify_hard_link_inode_name(2, "file");
}

static void
test_hard_link_inode_identifying_second_link_verifies_completely(void)
{
	scenario = "hard link inode identifies the second link in another directory";
	verify_hard_link_inode_name(17, "alias");
}

static void
test_directory_inode_without_name_field_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	unsigned char *raw;
	apfs_gen_inode_val_t inode = { .parent = APFS_INO_ROOT,
	    .private_id = 17, .mode = 0040755, .count = 0 };

	scenario = "ordinary directory inode has no name extended field";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	drec_build(tree, APFS_INO_ROOT, 17, "subdir", 4,
	    ascii_hash("subdir"), 0);
	inode_count_set(tree, APFS_INO_ROOT, 2);
	r = record_add(tree);
	r->key_size = GEN_JKEY_BYTES;
	r->value_size = GEN_INODE_VAL_BYTES;
	CHECK(apfs_gen_jkey_build(r->key, r->key_size, 17, 3));
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_ECORRUPT, "directory has no name field");
	fixture_close(&f);
}

static void
test_directory_in_private_directory_is_incomplete(void)
{
	fixture_t f;
	fs_tree_t *tree;
	unsigned char *raw;

	scenario = "private directory holds a directory";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_count_set(tree, APFS_INO_PRIVATE, 1);
	inode_build(tree, 17, APFS_INO_PRIVATE, "subdir", 4, 0, 0);
	drec_build(tree, APFS_INO_PRIVATE, 17, "subdir", 4,
	    ascii_hash("subdir"), 0);
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "private directory holds entries the walk from the root does not reach");
	fixture_close(&f);
}

static void
test_unreachable_directory_cycle_is_corrupt(void)
{
	fixture_t f;
	fs_tree_t *tree;
	unsigned char *raw;

	scenario = "unreachable directory cycle";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_build(tree, 17, 18, "A", 4, 1, 0);
	inode_build(tree, 18, 17, "B", 4, 1, 0);
	drec_build(tree, 18, 17, "A", 4, ascii_hash("A"), 0);
	drec_build(tree, 17, 18, "B", 4, ascii_hash("B"), 0);
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 2);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);

	verify_fixture(&f, 0, FSIMG_ECORRUPT, "unreachable from the root");
	fixture_close(&f);
}

/* The second mapping begins inside [DATA_BLOCK, DATA_BLOCK + 2) and
 * crosses its end. The optional adjacent record covers its last block.
 */
static void
record_boundary_fixture(fixture_t *f, bool adjacent)
{
	unsigned char keys[2][GEN_JKEY_BYTES], values[2][GEN_PEXT_VAL_BYTES];
	apfs_gen_record_t records[2];
	apfs_gen_pext_val_t physical = { .length = 2, .owner = 16,
		.refcnt = 1, .kind = 1 };
	apfs_gen_extent_val_t first = { .length = BLOCK,
		.phys_block = DATA_BLOCK };
	fs_record_t *r;
	unsigned char *raw;

	fixture_init(f, 1);
	r = record_find(&f->trees[0], 16, 8);
	CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &first));
	file_extent_build(&f->trees[0], BLOCK, DATA_BLOCK + 1, 2 * BLOCK);
	r = record_find(&f->trees[0], 16, 3);
	/* The dstream is the final extended field of this inode. */
	fsimg_store_le64(r->value + r->value_size - GEN_DSTREAM_BYTES, 3 * BLOCK);
	fsimg_store_le64(r->value + r->value_size - GEN_DSTREAM_BYTES + 8,
	    3 * BLOCK);
	filesystem_build(f, 0);
	for (unsigned i = 0; i < (adjacent ? 2u : 1u); i++) {
		physical.length = i == 0 ? 2 : 1;
		CHECK(apfs_gen_jkey_build(keys[i], sizeof(keys[i]),
		    DATA_BLOCK + 2 * i, 2));
		CHECK(apfs_gen_pext_val_build(values[i], sizeof(values[i]),
		    &physical));
		records[i] = (apfs_gen_record_t){ .key = keys[i],
		    .key_size = sizeof(keys[i]), .value = values[i],
		    .value_size = sizeof(values[i]) };
	}
	node_build(f, 105, 105, PHYSICAL_TREE, 15, records, adjacent ? 2 : 1,
	    0, 0);
	if (adjacent) {
		bit_set(block_at(f, BITMAP_BLOCK), DATA_BLOCK + 2, true);
		raw = block_at(f, VOLUME_BASE);
		fsimg_store_le64(raw + 88, 8);
		CHECK(apfs_gen_seal(raw, BLOCK));
		allocation_build(f);
	}
	f->expected_logical_bytes = 3 * BLOCK;
}

static void
test_file_extent_crossing_record_end_into_gap_is_corrupt(void)
{
	fixture_t f;

	scenario = "file extent starts inside a record and ends in a gap";
	record_boundary_fixture(&f, false);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "file extent outside every physical-extent record");
	fixture_close(&f);
}

static void
test_file_extent_crossing_adjacent_records_is_valid(void)
{
	fixture_t f;

	scenario = "file extent starts inside a record and crosses into its neighbor";
	record_boundary_fixture(&f, true);
	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

/* A separate region leaves the small fixtures' single-node layout intact. */
static void
clone_filesystem_build(fixture_t *f, unsigned clones, unsigned blocks)
{
	apfs_gen_record_t records[260], physical_records[64];
	apfs_gen_omap_record_t maps[64];
	unsigned char keys[64][GEN_JKEY_BYTES], values[64][GEN_PEXT_VAL_BYTES];
	apfs_gen_fs_tree_t tree = { .records = records, .first_oid = 3000,
		.base = 400, .xid = 10, .block_size = BLOCK,
		.records_per_leaf = 16, .children_per_index = 16 };
	apfs_gen_tree_t map_tree = { .records = maps, .base = 301, .xid = 10,
		.block_size = BLOCK, .records_per_leaf = 64, .children_per_index = 2 };
	apfs_gen_omap_t map = { .object = { .oid = 300, .xid = 10,
		.type = 0x4000000b }, .tree_type = PHYSICAL_TREE, .tree_oid = 301 };
	apfs_gen_pext_val_t physical = { .length = 1, .owner = 16,
		.refcnt = clones, .kind = 1 };
	fs_tree_t *fs = &f->trees[0];
	fs_record_t *r;
	unsigned char *raw;
	uint64_t root, nodes, map_root, map_nodes;
	char name[16];

	CHECK(clones <= 64 && blocks <= 64);
	fs->count = 0;
	drec_build(fs, 1, 2, "root", 4, ascii_hash("root"), 0);
	drec_build(fs, 1, 3, "private", 4, ascii_hash("private"), 0);
	inode_build(fs, 2, 1, "root", 4, (int32_t)clones, 0);
	inode_build(fs, 3, 1, "private", 4, 0, 0);
	for (unsigned i = 0; i < clones; i++) {
		uint64_t oid = 16 + i;

		CHECK(snprintf(name, sizeof(name), "clone%02u", i) > 0);
		inode_build(fs, oid, 2, name, 8, 1, blocks * BLOCK);
		drec_build(fs, 2, oid, name, 8, ascii_hash(name), 0);
		stream_id_build(fs, 1);
		r = &fs->records[fs->count - 1];
		CHECK(apfs_gen_jkey_build(r->key, r->key_size, oid, 6));
		file_extent_build(fs, 0, DATA_BLOCK, blocks * BLOCK);
		r = &fs->records[fs->count - 1];
		CHECK(apfs_gen_extent_key_build(r->key, r->key_size, oid, 0));
	}
	tree.count = fs->count;
	for (size_t i = 0; i < fs->count; i++)
		records[i] = (apfs_gen_record_t){ .key = fs->records[i].key,
		    .key_size = fs->records[i].key_size,
		    .value = fs->records[i].value,
		    .value_size = fs->records[i].value_size };
	qsort(records, fs->count, sizeof(records[0]), fs_record_compare);
	CHECK(apfs_gen_fs_tree_build(f->bytes, IMAGE_BYTES, &tree, maps,
	    ARRAY_COUNT(maps), &root, &nodes));
	map_tree.count = nodes;
	CHECK(apfs_gen_tree_build(f->bytes, IMAGE_BYTES, &map_tree,
	    &map_root, &map_nodes));
	CHECK(map_root == 301 && map_nodes == 1);
	CHECK(apfs_gen_omap_build(block_at(f, 300), BLOCK, &map));
	for (unsigned i = 0; i < blocks; i++) {
		CHECK(apfs_gen_jkey_build(keys[i], sizeof(keys[i]), DATA_BLOCK + i, 2));
		CHECK(apfs_gen_pext_val_build(values[i], sizeof(values[i]), &physical));
		physical_records[i] = (apfs_gen_record_t){ .key = keys[i],
		    .key_size = sizeof(keys[i]), .value = values[i],
		    .value_size = sizeof(values[i]) };
		bit_set(block_at(f, BITMAP_BLOCK), DATA_BLOCK + i, true);
	}
	node_build(f, 105, 105, PHYSICAL_TREE, 15, physical_records, blocks, 0, 0);
	/* Retire the original object map and filesystem root allocations. */
	for (unsigned i = 101; i <= 103; i++)
		bit_set(block_at(f, BITMAP_BLOCK), i, false);
	bit_set(block_at(f, BITMAP_BLOCK), 300, true);
	bit_set(block_at(f, BITMAP_BLOCK), 301, true);
	for (uint64_t i = 0; i < nodes; i++)
		bit_set(block_at(f, BITMAP_BLOCK), 400 + i, true);
	raw = block_at(f, VOLUME_BASE);
	fsimg_store_le64(raw + 88, nodes + 4 + blocks);
	fsimg_store_le64(raw + 128, 300);
	fsimg_store_le64(raw + 136, root);
	fsimg_store_le64(raw + 184, clones);
	CHECK(apfs_gen_seal(raw, BLOCK));
	allocation_build(f);
	f->expected_files = clones;
	f->expected_logical_bytes = (uint64_t)clones * blocks * BLOCK;
}

static void
test_many_clones_of_one_extent_fit_a_linear_work_budget(void)
{
	fixture_t f;
	const uint64_t budget = 100000;

	scenario = "64 clone streams over 64 one-block physical records";
	fixture_init(&f, 1);
	clone_filesystem_build(&f, 64, 64);
	(void)verify_fixture(&f, 0, FSIMG_OK, "");
	/*
	 * The join sorts and sweeps 2 * (64 + 64) = 256 boundaries. The
	 * 100000-unit budget leaves room for the tree scans, reconciliation
	 * and the walk, but not for a join whose work grows with extents
	 * times records (64 * 64 pieces and their sort).
	 */
	f.ctx->options.max_work = budget;
	(void)verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

static void
test_physical_reference_count_exceeds_streams_is_corrupt(void)
{
	fixture_t f;
	unsigned char key[GEN_JKEY_BYTES], value[GEN_PEXT_VAL_BYTES];
	apfs_gen_pext_val_t extent = { .length = 2, .owner = 16,
		.refcnt = 2, .kind = 1 };
	apfs_gen_record_t record = { .key = key, .key_size = sizeof(key),
		.value = value, .value_size = sizeof(value) };

	scenario = "physical extent reference count two mapped by one stream";
	fixture_init(&f, 1);
	CHECK(apfs_gen_jkey_build(key, sizeof(key), DATA_BLOCK, 2));
	CHECK(apfs_gen_pext_val_build(value, sizeof(value), &extent));
	node_build(&f, 105, 105, PHYSICAL_TREE, 15, &record, 1, 0, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "reference count differs from the streams mapping its blocks");
	fixture_close(&f);
}

static void
test_physical_record_without_file_mapping_is_corrupt(void)
{
	fixture_t f;
	fs_record_t *r;
	apfs_gen_extent_val_t hole = { .length = 2 * BLOCK };

	scenario = "physical extent record mapped by no stream";
	fixture_init(&f, 1);
	r = record_find(&f.trees[0], 16, 8);
	CHECK(apfs_gen_extent_val_build(r->value, r->value_size, &hole));
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "reference count differs from the streams mapping its blocks");
	fixture_close(&f);
}

static void
test_tree_footer_counts_differ_from_walk_is_corrupt(void)
{
	static const struct {
		const char *name;
		uint64_t block;
	} trees[] = {
		{ "container object map", 91 },
		{ "volume object map", 102 },
		{ "extent reference tree", 105 },
		{ "snapshot tree", 104 },
		{ "free queue", 11 },
		{ "filesystem tree", 103 }
	};
	fixture_t f;
	unsigned char *footer;
	char label[128];

	for (size_t i = 0; i < ARRAY_COUNT(trees); i++) {
		for (unsigned nodes = 0; nodes < 2; nodes++) {
			CHECK(snprintf(label, sizeof(label), "%s footer %s count",
			    trees[i].name, nodes ? "node" : "key") < (int)sizeof(label));
			scenario = label;
			fixture_init(&f, 1);
			if (trees[i].block == 11)
				queue_build(&f, 1, 0, 0, 300);
			footer = block_at(&f, trees[i].block) + BLOCK - GEN_APFS_FOOTER;
			footer += nodes ? 32 : 24;
			fsimg_store_le64(footer, fsimg_load_le64(footer) + 1);
			CHECK(apfs_gen_seal(block_at(&f, trees[i].block), BLOCK));
			verify_fixture(&f, 0, FSIMG_ECORRUPT, nodes ?
			    "node count differs from its footer" :
			    "key count differs from its footer");
			fixture_close(&f);
		}
	}
}

static void
test_superblock_counts_differ_from_inode_kinds_is_corrupt(void)
{
	static const struct {
		const char *name;
		unsigned offset;
	} counts[] = {
		{ "superblock file count differs from inodes", 184 },
		{ "superblock directory count differs from inodes", 192 }
	};
	fixture_t f;
	unsigned char *raw;

	for (size_t i = 0; i < ARRAY_COUNT(counts); i++) {
		scenario = counts[i].name;
		fixture_init(&f, 1);
		raw = block_at(&f, VOLUME_BASE);
		fsimg_store_le64(raw + counts[i].offset,
		    fsimg_load_le64(raw + counts[i].offset) + 1);
		CHECK(apfs_gen_seal(raw, BLOCK));
		verify_fixture(&f, 0, FSIMG_ECORRUPT,
		    "object counts differ from its inodes");
		fixture_close(&f);
	}
}

static void
test_clean_namespace_verifies_completely(void)
{
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	unsigned char *raw;
	apfs_gen_name_key_t key = { .oid = 18, .name = "com.apple.fs.symlink",
		.name_len = sizeof("com.apple.fs.symlink") - 1 };
	apfs_gen_xattr_val_t target = { .flags = 6,
		.data = (const unsigned char *)"file", .data_len = 5 };

	scenario = "clean root, subdirectory, mapped regular file and symlink";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	inode_count_set(tree, 2, 3);
	inode_build(tree, 17, 2, "subdir", 4, 0, 0);
	inode_build(tree, 18, 2, "link", 10, 1, 0);
	drec_build(tree, 2, 17, "subdir", 4, ascii_hash("subdir"), 0);
	drec_build(tree, 2, 18, "link", 10, ascii_hash("link"), 0);
	r = record_add(tree);
	r->key_size = GEN_NAME_KEY_BYTES + key.name_len + 1;
	r->value_size = GEN_XATTR_VAL_BYTES + target.data_len;
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, &target));
	raw = block_at(&f, VOLUME_BASE);
	fsimg_store_le64(raw + 192, 1);
	fsimg_store_le64(raw + 200, 1);
	CHECK(apfs_gen_seal(raw, BLOCK));
	filesystem_build(&f, 0);
	f.expected_files = 2;
	f.expected_directories = 2;
	f.expected_logical_bytes = 2 * BLOCK + 4;
	verify_fixture(&f, 0, FSIMG_OK, "");
	fixture_close(&f);
}

static void
native_image(const char *path)
{
	fsimg_read_options_t options;
	fsimg_image_options_t image_options;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_AUTOMATIC };
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_ctx_t *ctx = NULL;
	fsimg_io_t *io = NULL, *slice = NULL;
	fsimg_image_t *image = NULL;
	fsimg_reader_t *reader = NULL;
	apfs_nx_t nx;
	unsigned char raw[BLOCK];
	size_t transferred;
	fsimg_status_t status;
	char label[PATH_MAX + 32];
	const char *message;
	int fd;
	bool occupied = false;

	scenario = path;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0);
	CHECK(fsimg_ctx_new(NULL, &ctx, &error) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, &error) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_image_options_init(&image_options);
	status = fsimg_image_open(ctx, io, &image_options, &image, &error);
	if (status != FSIMG_OK) {
		status_is(status, FSIMG_OK, &error, "");
		goto done;
	}
	if (strstr(path, "-e18-gpt/") != NULL ||
	    strstr(path, "-e18-mounted/") != NULL ||
	    strstr(path, "-e18-large/") != NULL)
		selector = (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
		    .index = 2 };
	status = fsimg_image_select(image, &selector, &slice, &error);
	if (!status_is(status, FSIMG_OK, &error, ""))
		goto done;
	status = fsimg_io_read_exact(slice, 0, raw, sizeof(raw),
	    &transferred, &error);
	if (!status_is(status, FSIMG_OK, &error, ""))
		goto done;
	CHECK(apfs_nx_decode(raw, sizeof(raw), &nx));
	CHECK(nx.max_volumes <= 100);
	for (uint32_t slot = 0; slot < nx.max_volumes; slot++) {
		if (nx.volume_oids[slot] == 0)
			continue;
		occupied = true;
		CHECK(snprintf(label, sizeof(label), "%s volume %u", path,
		    slot) < (int)sizeof(label));
		scenario = label;
		fsimg_read_options_init(&options);
		options.volume_index_set = true;
		options.volume_index = slot;
		status = fsimg_reader_open(ctx, slice, &options, &reader, &error);
		if (status != FSIMG_OK) {
			bool encrypted_capture =
			    (slot == 1 && strstr(path,
			    "/e11-enc-addvol/e11-enc-addvol.img") != NULL) ||
			    (slot == 0 && (strstr(path,
			    "/e11-enc-newfs/e11-enc-newfs.img") != NULL ||
			    strstr(path, "/e11-enc-newfs-pop/e11-enc-newfs-pop.img") != NULL ||
			    strstr(path, "/e11-enc-encryptvol/e11-enc-encryptvol.img") != NULL));

			printf("REFUSED %s volume %u: %s\n", path, slot, error.message);
			if (encrypted_capture && status_is(status, FSIMG_EUNSUPPORTED,
			    &error, "APFS volume superblock is encrypted")) {
				native_refused_encrypted++;
				native_refused++;
			} else if (!encrypted_capture)
				status_is(status, FSIMG_OK, &error, "");
			continue;
		}

		/* Use the selected checkpoint for subsequent occupied slots. */
		nx = ((apfs_reader_t *)reader->driver)->container.nx;
		status = fsimg_verify(reader, 0, &report, &error);
		printf("native verify %s volume %u: %d %s\n", path, slot,
		    status, error.message);
		/* Captured reaper flags are 3 with two entries in 003 and
		 * 005; 007 has flags 1 and an empty list, so is idle.
		 */
		message = NULL;
		if (strstr(path, "-e17-clean/during-003.img") != NULL ||
		    strstr(path, "-e17-clean/during-005.img") != NULL)
			message = "pending reap";
		if (slot == 0 && strstr(path,
		    "/e11-enc-addvol/e11-enc-addvol.img") != NULL)
			message = "APFS volume superblock is encrypted";

		if (message == NULL && status == FSIMG_EUNSUPPORTED &&
		    strstr(error.message, "has snapshots") != NULL)
			message = "has snapshots";
		if (message == NULL) {
			if (status_is(status, FSIMG_OK, &error, "")) {
				expect(report.complete, "native verification is complete");
				native_complete++;
				native_verified++;
			}
		} else if (status_is(status, FSIMG_EUNSUPPORTED, &error, message)) {
			expect(!report.complete, "native report is incomplete");
			if (strcmp(message, "pending reap") == 0)
				native_pending++;
			else if (strcmp(message, "has snapshots") == 0)
				native_snapshots++;
			else
				native_encrypted++;
			native_verified++;
		}
		fsimg_reader_free(reader);
		reader = NULL;
	}
	if (!occupied) {
		fsimg_read_options_init(&options);
		status = fsimg_reader_open(ctx, slice, &options, &reader, &error);
		printf("REFUSED %s: %s\n", path, error.message);
		/* This newfs specimen contains no volumes and has invalid geometry. */
		if (strstr(path, "/apfs-newfs-372736-plain/image.img") != NULL) {
			if (status_is(status, FSIMG_ECORRUPT, &error,
			    "APFS container superblock has an invalid block count")) {
				native_refused_empty++;
				native_refused++;
			}
		} else
			expect(false, "unexpected capture without an occupied volume");
		fsimg_reader_free(reader);
	}
 done:
	fsimg_io_free(slice);
	fsimg_image_free(image);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
}

/* Walk capture directories rather than maintaining a list of image names. */
static void
native_directory(const char *directory, unsigned depth)
{
	struct dirent *entry;
	struct stat st;
	char path[PATH_MAX];
	DIR *dir;
	size_t length;

	dir = opendir(directory);
	CHECK(dir != NULL);
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		CHECK(snprintf(path, sizeof(path), "%s/%s", directory,
		    entry->d_name) < (int)sizeof(path));
		CHECK(lstat(path, &st) == 0);
		if (S_ISDIR(st.st_mode) && depth != 0) {
			unsigned next_depth = depth - 1;

			if (depth == 2 &&
			    strncmp(entry->d_name, "apfs-diskutil-", 14) != 0 &&
			    strncmp(entry->d_name, "e1", 2) != 0)
				next_depth = 0;
			native_directory(path, next_depth);
		}
		length = strlen(entry->d_name);
		if (S_ISREG(st.st_mode) && length >= 4 &&
		    strcmp(entry->d_name + length - 4, ".img") == 0)
			native_image(path);
	}
	CHECK(closedir(dir) == 0);
}

static void
test_native_captures(void)
{
	const char *directory = getenv("FSIMG_APFS_NATIVE");

	if (directory == NULL) {
		puts("SKIP native verification (FSIMG_APFS_NATIVE is unset)");
		return;
	}
	native_directory(directory, 2);
	scenario = "native captures";
	printf("native verification: %u verified, %u refused\n",
	    native_verified, native_refused);
	printf("native reasons: %u complete, %u pending reap, %u encrypted, "
	    "%u snapshots\n", native_complete,
	    native_pending, native_encrypted, native_snapshots);
	printf("native refusals: %u encrypted, %u empty container\n",
	    native_refused_encrypted, native_refused_empty);
	expect(native_verified != 0, "native gate verified at least one volume");
}

static void
compression_attribute_build(fs_tree_t *tree, const char *name,
    const apfs_gen_xattr_val_t *value)
{
	fs_record_t *r = record_add(tree);
	apfs_gen_name_key_t key = { .oid = 16, .name = name,
	    .name_len = (uint16_t)strlen(name) };

	r->key_size = (uint16_t)(11 + key.name_len);
	r->value_size = (uint16_t)(4 + value->data_len);
	CHECK(apfs_gen_xattr_key_build(r->key, r->key_size, &key));
	CHECK(apfs_gen_xattr_val_build(r->value, r->value_size, value));
}

static void
test_private_directory_compressed_file_leaves_verification_incomplete(void)
{
	unsigned char source[37] = { 0 };
	unsigned char header[16], fields[16];
	apfs_gen_xfield_t name = { .type = 4, .flags = 2, .size = 5,
	    .data = (const unsigned char *)"file" };
	apfs_gen_inode_val_t inode = { .parent = APFS_INO_ROOT,
	    .private_id = 16, .mode = 0100755, .count = 1,
	    .bsd_flags = 0x20, .xfields = fields,
	    .xfields_len = sizeof(fields) };
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	size_t stored;

	scenario = "ordinary directory holds a compressed file";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	/* The resource fork replaces the inode's data stream. */
	r = record_find(tree, 16, 6);
	*r = tree->records[--tree->count];
	stored = apfs_gen_compressed_fork(block_at(&f, DATA_BLOCK),
	    2 * BLOCK, 4, source, sizeof(source));
	CHECK(stored != 0);
	CHECK(apfs_gen_xfields_build(fields, sizeof(fields), &name, 1));
	r = record_find(tree, 16, 3);
	r->value_size = GEN_INODE_VAL_BYTES + sizeof(fields);
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
	apfs_gen_decmpfs_header(header, 4, sizeof(source));
	compression_attribute_build(tree, "com.apple.decmpfs",
	    &(apfs_gen_xattr_val_t){ .flags = 2, .data_len = sizeof(header),
	    .data = header });
	compression_attribute_build(tree, "com.apple.ResourceFork",
	    &(apfs_gen_xattr_val_t){ .flags = 1, .data_len = 48,
	    .stream_oid = 16, .dstream = { .size = stored,
	    .alloced_size = 2 * BLOCK } });
	filesystem_build(&f, 0);
	f.expected_logical_bytes = sizeof(source);
	verify_fixture(&f, 0, FSIMG_OK, "");

	scenario = "private directory holds a compressed file";
	/* Move the file's only name and its inode parent together. */
	r = record_find(tree, APFS_INO_ROOT, 9);
	*r = tree->records[--tree->count];
	drec_build(tree, APFS_INO_PRIVATE, 16, "file", 8,
	    ascii_hash("file"), 0);
	inode.parent = APFS_INO_PRIVATE;
	r = record_find(tree, 16, 3);
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
	inode_count_set(tree, APFS_INO_ROOT, 0);
	inode_count_set(tree, APFS_INO_PRIVATE, 1);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED,
	    "private directory holds entries the walk from the root does not reach");
	fixture_close(&f);
}

static void
test_private_directory_compressed_file_with_root_parent_is_corrupt(void)
{
	unsigned char source[37] = { 0 };
	unsigned char header[16], fields[16];
	apfs_gen_xfield_t name = { .type = 4, .flags = 2, .size = 5,
	    .data = (const unsigned char *)"file" };
	apfs_gen_inode_val_t inode = { .parent = APFS_INO_ROOT,
	    .private_id = 16, .mode = 0100755, .count = 1,
	    .bsd_flags = 0x20, .xfields = fields,
	    .xfields_len = sizeof(fields) };
	fixture_t f;
	fs_tree_t *tree;
	fs_record_t *r;
	size_t stored;

	scenario = "compressed file named only in private directory has root inode parent";
	fixture_init(&f, 1);
	tree = &f.trees[0];
	/* The resource fork replaces the inode's data stream. */
	r = record_find(tree, 16, 6);
	*r = tree->records[--tree->count];
	stored = apfs_gen_compressed_fork(block_at(&f, DATA_BLOCK),
	    2 * BLOCK, 4, source, sizeof(source));
	CHECK(stored != 0);
	CHECK(apfs_gen_xfields_build(fields, sizeof(fields), &name, 1));
	r = record_find(tree, 16, 3);
	r->value_size = GEN_INODE_VAL_BYTES + sizeof(fields);
	CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
	apfs_gen_decmpfs_header(header, 4, sizeof(source));
	compression_attribute_build(tree, "com.apple.decmpfs",
	    &(apfs_gen_xattr_val_t){ .flags = 2, .data_len = sizeof(header),
	    .data = header });
	compression_attribute_build(tree, "com.apple.ResourceFork",
	    &(apfs_gen_xattr_val_t){ .flags = 1, .data_len = 48,
	    .stream_oid = 16, .dstream = { .size = stored,
	    .alloced_size = 2 * BLOCK } });

	/* Move only the directory record; the inode still names the root. */
	r = record_find(tree, APFS_INO_ROOT, 9);
	*r = tree->records[--tree->count];
	drec_build(tree, APFS_INO_PRIVATE, 16, "file", 8,
	    ascii_hash("file"), 0);
	inode_count_set(tree, APFS_INO_ROOT, 0);
	inode_count_set(tree, APFS_INO_PRIVATE, 1);
	filesystem_build(&f, 0);
	verify_fixture(&f, 0, FSIMG_ECORRUPT,
	    "parent or name field differs from its names");
	fixture_close(&f);
}

static void
test_structural_verification_checks_last_compressed_table_entry(void)
{
	const size_t size = 3 * 65536 + 37;
	unsigned char *source = calloc(1, size);
	unsigned char header[16], fields[16];
	apfs_gen_xfield_t name = { .type = 4, .flags = 2, .size = 5,
	    .data = (const unsigned char *)"file" };
	apfs_gen_inode_val_t inode = { .parent = 2, .private_id = 16,
	    .mode = 0100755, .count = 1, .bsd_flags = 0x20,
	    .xfields = fields, .xfields_len = sizeof(fields) };
	fixture_t f;
	fs_record_t *r;
	size_t stored;

	CHECK(source != NULL);
	CHECK(apfs_gen_xfields_build(fields, sizeof(fields), &name, 1));
	for (unsigned damaged = 0; damaged < 2; damaged++) {
		scenario = damaged ? "structural verify rejects damaged last compressed entry" :
		    "structural verify checks compressed tables without decoding payload";
		fixture_init(&f, 1);
		/* Attribute streams have no inode stream reference record. */
		r = record_find(&f.trees[0], 16, 6);
		*r = f.trees[0].records[--f.trees[0].count];
		stored = apfs_gen_compressed_fork(block_at(&f, DATA_BLOCK),
		    2 * BLOCK, 4, source, size);
		CHECK(stored != 0);
		r = record_find(&f.trees[0], 16, 3);
		r->value_size = GEN_INODE_VAL_BYTES + sizeof(fields);
		CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
		apfs_gen_decmpfs_header(header, 4, size);
		compression_attribute_build(&f.trees[0], "com.apple.decmpfs",
		    &(apfs_gen_xattr_val_t){ .flags = 2, .data_len = sizeof(header),
		    .data = header });
		compression_attribute_build(&f.trees[0], "com.apple.ResourceFork",
		    &(apfs_gen_xattr_val_t){ .flags = 1, .data_len = 48,
		    .stream_oid = 16, .dstream = { .size = stored,
		    .alloced_size = 2 * BLOCK } });
		/* Invalid payload proves structural verification never decodes it. */
		block_at(&f, DATA_BLOCK)[56] = 0;
		if (damaged)
			fsimg_store_le32(block_at(&f, DATA_BLOCK) + 24 + 3 * 8 + 4,
			    UINT32_MAX);
		filesystem_build(&f, 0);
		f.expected_logical_bytes = size;
		verify_fixture(&f, 0, damaged ? FSIMG_ECORRUPT : FSIMG_OK,
		    damaged ? "decmpfs chunk lies outside the resource data" : "");
		fixture_close(&f);
	}
	free(source);
}

static void
test_zero_size_compressed_file_requires_empty_resource_table(void)
{
	unsigned char source[1] = { 0 };
	unsigned char header[16], fields[16];
	apfs_gen_xfield_t name = { .type = 4, .flags = 2, .size = 5,
	    .data = (const unsigned char *)"file" };
	apfs_gen_inode_val_t inode = { .parent = 2, .private_id = 16,
	    .mode = 0100755, .count = 1, .bsd_flags = 0x20,
	    .xfields = fields, .xfields_len = sizeof(fields) };
	fixture_t f;
	fs_record_t *r;
	size_t stored;

	CHECK(apfs_gen_xfields_build(fields, sizeof(fields), &name, 1));
	for (unsigned damaged = 0; damaged < 2; damaged++) {
		scenario = damaged ? "zero-size compressed file rejects a one-chunk resource table" :
		    "zero-size compressed file accepts an empty resource table";
		fixture_init(&f, 1);
		/* Attribute streams have no inode stream reference record. */
		r = record_find(&f.trees[0], 16, 6);
		*r = f.trees[0].records[--f.trees[0].count];
		stored = apfs_gen_compressed_fork(block_at(&f, DATA_BLOCK),
		    2 * BLOCK, 4, source, damaged ? sizeof(source) : 0);
		CHECK(stored != 0);
		if (!damaged)
			expect(stored == 24, "empty resource header has zero chunk entries");
		r = record_find(&f.trees[0], 16, 3);
		r->value_size = GEN_INODE_VAL_BYTES + sizeof(fields);
		CHECK(apfs_gen_inode_val_build(r->value, r->value_size, &inode));
		apfs_gen_decmpfs_header(header, 4, 0);
		compression_attribute_build(&f.trees[0], "com.apple.decmpfs",
		    &(apfs_gen_xattr_val_t){ .flags = 2, .data_len = sizeof(header),
		    .data = header });
		compression_attribute_build(&f.trees[0], "com.apple.ResourceFork",
		    &(apfs_gen_xattr_val_t){ .flags = 1, .data_len = 48,
		    .stream_oid = 16, .dstream = { .size = stored,
		    .alloced_size = 2 * BLOCK } });
		filesystem_build(&f, 0);
		f.expected_logical_bytes = 0;
		verify_fixture(&f, 0, damaged ? FSIMG_ECORRUPT : FSIMG_OK,
		    damaged ? "decmpfs chunk table is malformed" : "");
		fixture_close(&f);
	}
}

int
main(void)
{
	test_private_directory_compressed_file_leaves_verification_incomplete();
	test_structural_verification_checks_last_compressed_table_entry();
	test_zero_size_compressed_file_requires_empty_resource_table();
	test_valid_container_counts();
	test_older_layout_container_verifies();
	test_allocation_damage();
	test_double_claims();
	test_snapshot_shares_data_with_live_volume();
	test_claim_outside_container();
	test_volume_allocated_block_count();
	test_volume_uuids_must_be_distinct();
	test_object_map_versions_reject_zero_and_future_xids();
	test_refused_superblock_does_not_reuse_volume_uuid();
	test_decoded_volume_uuid_must_not_be_zero();
	test_snapshot_xids_must_not_exceed_checkpoint();
	test_snapshot_names_order_by_stored_bytes();
	test_snapshot_count_disagrees_with_tree();
	test_extent_records_in_one_tree_must_not_overlap();
	test_free_count_mismatches();
	test_incomplete_containers_preserve_corruption_errors();
	test_free_queues();
	test_empty_free_queues_reject_future_oldest_xid();
	test_free_queue_records_reject_future_xids();
	test_unsupported_container_mapping_preserves_queue_damage();
	test_unsupported_mapped_object_preserves_later_damage();
	test_refused_volume_map_preserves_extent_damage();
	test_pool_claims();
	test_truncation_and_assumed_identity();
	test_link_count_differs_from_names_is_corrupt();
	test_child_count_differs_from_records_is_corrupt();
	test_stream_reference_count_differs_from_inode_users_is_corrupt();
	test_multiple_names_without_sibling_links_is_corrupt();
	test_name_with_sibling_id_without_link_is_corrupt();
	test_sibling_link_without_name_sibling_id_is_corrupt();
	test_sibling_link_name_differs_from_directory_record_is_corrupt();
	test_name_sibling_id_differs_from_link_is_corrupt();
	test_case_equivalent_names_in_one_directory_is_corrupt();
	test_normalization_equivalent_stored_spellings_is_corrupt();
	test_collision_in_nonselected_volume_is_corrupt();
	test_extent_gap_is_corrupt();
	test_first_extent_after_zero_is_corrupt();
	test_file_extent_outside_physical_records_is_corrupt();
	test_file_inode_parent_differs_from_its_only_name_is_corrupt();
	test_file_inode_name_differs_from_its_only_name_is_corrupt();
	test_private_directory_compressed_file_with_root_parent_is_corrupt();
	test_symlink_named_only_in_private_directory_is_incomplete();
	test_file_inode_under_private_directory_without_a_name_is_incomplete();
	test_hard_link_inode_identifying_first_link_verifies_completely();
	test_hard_link_inode_identifying_second_link_verifies_completely();
	test_directory_inode_without_name_field_is_corrupt();
	test_directory_in_private_directory_is_incomplete();
	test_unreachable_directory_cycle_is_corrupt();
	test_file_extent_crossing_record_end_into_gap_is_corrupt();
	test_file_extent_crossing_adjacent_records_is_valid();
	test_many_clones_of_one_extent_fit_a_linear_work_budget();
	test_physical_reference_count_exceeds_streams_is_corrupt();
	test_physical_record_without_file_mapping_is_corrupt();
	test_tree_footer_counts_differ_from_walk_is_corrupt();
	test_superblock_counts_differ_from_inode_kinds_is_corrupt();
	test_clean_namespace_verifies_completely();
	test_native_captures();
	printf("APFS verification: %u checks, %u failures\n", checks, failures);
	return (failures != 0);
}
