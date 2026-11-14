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

/* Fixed regions occupy 0..82. Each volume owns six blocks at 100 + 10*i:
 * superblock, object map, map root, virtual fs root, snapshots, extents.
 * The CIB and main bitmap live inside the separately accounted pool.
 */
typedef struct fixture {
	unsigned char *bytes;
	uint64_t	 length;
	apfs_gen_spaceman_t sm;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
} fixture_t;

static unsigned failures, checks, native_verified, native_refused;
static unsigned native_container, native_pending, native_encrypted;
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
fixture_init(fixture_t *f, unsigned volumes)
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
	f->bytes = calloc(1, IMAGE_BYTES);
	CHECK(f->bytes != NULL);
	for (unsigned i = 0; i < volumes; i++)
		image.nx.volume_oids[i] = 1000 + i;
	CHECK(apfs_gen_container_build(f->bytes, IMAGE_BYTES, &image));
	CHECK(apfs_gen_spaceman_geometry(BLOCK, BLOCKS, &f->sm) == BLOCK);
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
			.alloc_blocks = 7,
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
		node_build(f, base + 3, 2000 + i, 2, 14, NULL, 0, 0, 0);
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
fixture_close(fixture_t *f)
{
	fsimg_io_free(f->io);
	fsimg_ctx_free(f->ctx);
	free(f->bytes);
}

static void
verify_fixture(fixture_t *f, unsigned flags, fsimg_status_t wanted,
    const char *message)
{
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_reader_t *reader = NULL;
	fsimg_io_t *slice = NULL;
	fsimg_status_t status;

	fsimg_read_options_init(&options);
	options.format = "apfs";
	options.flags = flags;
	options.volume_index_set = true;
	options.volume_index = 0;
	CHECK(fsimg_io_slice(f->io, 0, f->length, &slice, &error) == FSIMG_OK);
	status = fsimg_reader_open(f->ctx, slice, &options, &reader, &error);
	if (status_is(status, FSIMG_OK, &error, "")) {
		status = fsimg_verify(reader, 0, &report, &error);
		if (status_is(status, wanted, &error, message) &&
		    wanted == FSIMG_EUNSUPPORTED) {
			expect(!report.complete, "container report is incomplete");
			if (strcmp(message, "covers the container alone") == 0) {
				expect(report.free_clusters ==
				    f->sm.devices[0].free_count,
				    "report gives main device free count");
				expect(report.allocated_clusters ==
				    BLOCKS - f->sm.devices[0].free_count,
				    "allocated count is N minus free count");
			}
		}
	}
	fsimg_reader_free(reader);
	fsimg_io_free(slice);
}

static void
test_valid_container_counts(void)
{
	fixture_t f;

	scenario = "valid container counts";
	fixture_init(&f, 1);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "covers the container alone");
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
		    count == 7 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT,
		    count == 7 ? "covers the container alone" :
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
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "covers the container alone");
	fixture_close(&f);

	scenario = "nonempty main free queue";
	fixture_init(&f, 1);
	bit_set(block_at(&f, BITMAP_BLOCK), 300, true);
	queue_build(&f, 1, 1, 1, 300);
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "covers the container alone");
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
	verify_fixture(&f, 0, FSIMG_EUNSUPPORTED, "covers the container alone");
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
			printf("REFUSED %s volume %u: %s\n", path, slot, error.message);
			native_refused++;
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
		message = "covers the container alone";
		if (strstr(path, "-e17-clean/during-003.img") != NULL ||
		    strstr(path, "-e17-clean/during-005.img") != NULL)
			message = "pending reap";
		if (slot == 0 && strstr(path,
		    "/e11-enc-addvol/e11-enc-addvol.img") != NULL)
			message = "APFS volume superblock is encrypted";
		if (status_is(status, FSIMG_EUNSUPPORTED, &error, message)) {
			expect(!report.complete, "native report is incomplete");
			if (strcmp(message, "covers the container alone") == 0)
				native_container++;
			else if (strcmp(message, "pending reap") == 0)
				native_pending++;
			else {
				expect(strcmp(error.message, message) == 0,
				    "encrypted sibling has the exact diagnostic");
				native_encrypted++;
			}
			native_verified++;
		}
		fsimg_reader_free(reader);
		reader = NULL;
	}
	if (!occupied) {
		fsimg_read_options_init(&options);
		status = fsimg_reader_open(ctx, slice, &options, &reader, &error);
		expect(status != FSIMG_OK, "empty container is refused");
		printf("REFUSED %s: %s\n", path, error.message);
		native_refused++;
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
	printf("native reasons: %u container alone, %u pending reap, "
	    "%u encrypted\n", native_container, native_pending, native_encrypted);
	expect(native_verified != 0, "native gate verified at least one volume");
}

int
main(void)
{
	test_valid_container_counts();
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
	test_native_captures();
	printf("APFS verification: %u checks, %u failures\n", checks, failures);
	return (failures != 0);
}
