/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "apfs-gen.h"
#include "test.h"
#include "fsimg-endian.h"

typedef struct fs_xfield_failure {
	const char *name;
	size_t length;
	unsigned offset;
	unsigned width;
	uint64_t value;
} fs_xfield_failure_t;

typedef struct native_run {
	size_t		offset;
	const char	*hex;
} native_run_t;

typedef struct native_block {
	unsigned	number;
	const native_run_t *runs;
	size_t		count;
	uint64_t	oid;
	uint32_t	kind;
	uint32_t	flags;
	uint32_t	subtype;
} native_block_t;

typedef struct native_node {
	unsigned	block;
	uint16_t	value_size;
	uint16_t	toc_length;
	uint16_t	free_length;
	uint32_t	info_flags;
	uint32_t	subtype;
} native_node_t;

static const native_node_t native_nodes[] = {
	{ .block = 92, .value_size = 16, .toc_length = 448,
	  .free_length = 3520, .info_flags = 0x12, .subtype = APFS_KIND_OMAP },
	{ .block = 13, .value_size = 8, .toc_length = 576,
	  .free_length = 3400, .info_flags = 0xe, .subtype = 9 }
};

typedef struct node_failure {
	const char	*name;
	size_t		length;
	uint32_t	count;
	uint16_t	flags;
	apfs_nloc_t	toc;
	apfs_nloc_t	free;
} node_failure_t;

typedef struct entry_failure {
	const char	*name;
	apfs_toc_t	toc;
} entry_failure_t;

typedef struct node_case {
	const char	*name;
	uint16_t	flags;
	uint16_t	level;
} node_case_t;

static void	expect(bool, const char *);
static void	check_entry_spans(const apfs_entry_t *,
    const apfs_entry_t *);
static void	put_le(unsigned char *, uint64_t, unsigned);
static void	expand_native(const native_block_t *, unsigned char *);
static apfs_toc_t decode_variable_toc(const unsigned char *);
static void	reencode(const unsigned char *, size_t, unsigned char *);
static void	check_nx(const unsigned char *, size_t,
    const apfs_gen_nx_t *);
static void	check_omap(const unsigned char *, size_t,
    const apfs_gen_omap_t *);
static void	check_spaceman(const unsigned char *, size_t,
    const apfs_gen_spaceman_t *);
static void	check_reaper(const unsigned char *, size_t,
    const apfs_gen_reaper_t *);
static apfs_gen_nx_t	native_superblock(unsigned);
static void	check_native_node(const unsigned char *, const native_node_t *);
static void	test_native_blocks(void);
static void	test_fletcher(void);
static void	test_truncated_records(void);
static void	test_object_lengths(void);
static void	test_nx_area_counts(void);
static void	test_counts_and_indexes(void);
static void	test_node_framing(void);
static void	test_entry_spans(void);
static void	generated_roundtrip(const unsigned char *, size_t,
    const apfs_gen_object_t *);
static void	test_generated_fixed_objects(void);
static void	test_generated_arrays(void);
static void	test_generated_queue_records(void);
static void	test_omap_records(void);
static void	build_node_fixture(unsigned char *, size_t,
    const node_case_t *, apfs_gen_node_t *, apfs_gen_record_t [3]);
static void	check_generated_node(const unsigned char *, size_t,
    const apfs_gen_node_t *, const char *);
static void	test_generated_nodes(void);
static void	test_patterned_encodes(void);
static void	test_generator_rejections(void);

static const char *scenario = "initialization";

static void
expect(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "APFS case=%s: %s\n", scenario, message);
	CHECK(condition);
}

static void
check_entry_spans(const apfs_entry_t *encoded, const apfs_entry_t *decoded)
{
	expect(encoded->key_off == decoded->key_off &&
	    encoded->key_len == decoded->key_len &&
	    encoded->val_off == decoded->val_off &&
	    encoded->val_len == decoded->val_len &&
	    encoded->ghost == decoded->ghost,
	    "encoder and decoder return equal spans");
}

static void
put_le(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

/* Native apfs-newfs-4194304-plain/image.img, block 0. */
static const native_run_t native_0[] = {
	{ 0, "4a8c4220c92fe39201" },
	{ 16, "02" },
	{ 24, "01000080" },
	{ 32, "4e585342001000000004" },
	{ 64, "02" },
	{ 72, "5f586267651949e5a8d918aacdbe14620604" },
	{ 96, "03" },
	{ 104, "080000003400000001" },
	{ 120, "09" },
	{ 128, "040000000600000002000000020000000200000004" },
	{ 153, "04" },
	{ 160, "5b" },
	{ 168, "0104" },
	{ 180, "010000000204" },
	{ 984, "18" },
	{ 1312, "0100040001" },
	{ 1384, "c4bf9f31bcfc09" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 3. */
static const native_run_t native_3[] = {
	{ 0, "e06fc63ea03f398103" },
	{ 16, "02" },
	{ 24, "0c000040" },
	{ 32, "010000000400000005000080" },
	{ 49, "10" },
	{ 65, "04" },
	{ 72, "0b" },
	{ 80, "11000080" },
	{ 89, "10" },
	{ 104, "0104" },
	{ 112, "0c" },
	{ 120, "0200008009" },
	{ 129, "10" },
	{ 144, "0304" },
	{ 152, "0d" },
	{ 160, "0200008009" },
	{ 169, "10" },
	{ 184, "0504" },
	{ 192, "0e" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 4. */
static const native_run_t native_4[] = {
	{ 0, "6b954220ab26e39201" },
	{ 16, "02" },
	{ 24, "01000080" },
	{ 32, "4e585342001000000004" },
	{ 64, "02" },
	{ 72, "5f586267651949e5a8d918aacdbe14620604" },
	{ 96, "03" },
	{ 104, "080000003400000001" },
	{ 120, "09" },
	{ 128, "040000000600000002000000020000000200000004" },
	{ 153, "04" },
	{ 160, "5b" },
	{ 168, "0104" },
	{ 180, "010000000204" },
	{ 984, "15" },
	{ 1312, "0100040001" },
	{ 1384, "c4bf9f31bcfc09" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 91. */
static const native_run_t native_91[] = {
	{ 0, "beddfcff772103405b" },
	{ 16, "02" },
	{ 24, "0b000040" },
	{ 32, "01" },
	{ 40, "02000040020000405c" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 92. */
static const native_run_t native_92[] = {
	{ 0, "eb5a1592e17f581e5c" },
	{ 16, "02" },
	{ 24, "020000400b0000000700000001" },
	{ 42, "c0011000c00dffff0000ffff" },
	{ 58, "10" },
	{ 504, "0204" },
	{ 512, "02" },
	{ 4045, "1000005a" },
	{ 4056, "12" },
	{ 4061, "1000001000000010000000100000001000000001" },
	{ 4088, "01" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 11. */
static const native_run_t native_11[] = {
	{ 0, "1e9df02ad47fd8540004" },
	{ 16, "02" },
	{ 24, "05000080" },
	{ 33, "100000008000007e000000fb0100000004" },
	{ 56, "01" },
	{ 64, "01" },
	{ 72, "a303" },
	{ 80, "080a" },
	{ 128, "100a" },
	{ 144, "010000001000000006" },
	{ 160, "01000000100000003d" },
	{ 176, "4d" },
	{ 200, "02" },
	{ 208, "0304" },
	{ 216, "02" },
	{ 224, "01" },
	{ 240, "02" },
	{ 248, "0504" },
	{ 256, "02" },
	{ 264, "01" },
	{ 320, "02000000d8090000e0090000e809000001000000d809" },
	{ 2520, "02" },
	{ 2528, "01" },
	{ 2536, "ffffffff03000400050006000700080009000a000b000c000d000e000f" },
	{ 2568, "4f" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 12. */
static const native_run_t native_12[] = {
	{ 0, "14ffb27f45ed4c000104" },
	{ 16, "02" },
	{ 24, "11000080" },
	{ 32, "01" },
	{ 64, "01" },
	{ 108, "900f" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 13. */
static const native_run_t native_13[] = {
	{ 0, "b0bd7cd1972df19e0304" },
	{ 16, "02" },
	{ 24, "02000080090000000700000001" },
	{ 42, "40021000480dffff0000ffff" },
	{ 58, "08" },
	{ 632, "02" },
	{ 640, "4d" },
	{ 4048, "02" },
	{ 4056, "0e" },
	{ 4061, "1000001000000008000000100000000800000001" },
	{ 4088, "01" },
};

/* Native apfs-newfs-4194304-plain/image.img, block 79. */
static const native_run_t native_79[] = {
	{ 0, "2d33df3f84c420804f" },
	{ 16, "02" },
	{ 24, "07000040" },
	{ 36, "0100000002" },
	{ 57, "040000a303" },
	{ 64, "50" },
};

static const native_block_t native_blocks[] = {
	{ .number = 79, .runs = native_79,
	  .count = sizeof(native_79) / sizeof(native_79[0]),
	  .oid = 79, .kind = APFS_KIND_SPACEMAN_CIB,
	  .flags = 0x40000000, .subtype = 0 },
	{ .number = 0, .runs = native_0,
	  .count = sizeof(native_0) / sizeof(native_0[0]),
	  .oid = 1, .kind = 1, .flags = 0x80000000, .subtype = 0 },
	{ .number = 3, .runs = native_3,
	  .count = sizeof(native_3) / sizeof(native_3[0]),
	  .oid = 3, .kind = 12, .flags = 0x40000000, .subtype = 0 },
	{ .number = 4, .runs = native_4,
	  .count = sizeof(native_4) / sizeof(native_4[0]),
	  .oid = 1, .kind = 1, .flags = 0x80000000, .subtype = 0 },
	{ .number = 91, .runs = native_91,
	  .count = sizeof(native_91) / sizeof(native_91[0]),
	  .oid = 91, .kind = 11, .flags = 0x40000000, .subtype = 0 },
	{ .number = 92, .runs = native_92,
	  .count = sizeof(native_92) / sizeof(native_92[0]),
	  .oid = 92, .kind = 2, .flags = 0x40000000, .subtype = 11 },
	{ .number = 11, .runs = native_11,
	  .count = sizeof(native_11) / sizeof(native_11[0]),
	  .oid = 1024, .kind = 5, .flags = 0x80000000, .subtype = 0 },
	{ .number = 12, .runs = native_12,
	  .count = sizeof(native_12) / sizeof(native_12[0]),
	  .oid = 1025, .kind = 17, .flags = 0x80000000, .subtype = 0 },
	{ .number = 13, .runs = native_13,
	  .count = sizeof(native_13) / sizeof(native_13[0]),
	  .oid = 1027, .kind = 2, .flags = 0x80000000, .subtype = 9 },
};

static void
expand_native(const native_block_t *fixture, unsigned char *raw)
{
	unsigned byte;
	size_t length;

	bzero(raw, 4096);
	for (size_t i = 0; i < fixture->count; i++) {
		length = strlen(fixture->runs[i].hex);
		expect(length % 2 == 0, "complete hex byte");
		expect(fixture->runs[i].offset + length / 2 <= 4096,
		    "native run inside block");
		for (size_t j = 0; j < length / 2; j++) {
			expect(sscanf(fixture->runs[i].hex + 2 * j, "%2x",
			    &byte) == 1, "hex byte");
			raw[fixture->runs[i].offset + j] = (unsigned char)byte;
		}
	}
}

/* Read the stored TOC fields, including uninterpreted ghost lengths. */
static apfs_toc_t
decode_variable_toc(const unsigned char *raw)
{
	return ((apfs_toc_t){
		.key = { .off = fsimg_load_le16(raw),
		    .len = fsimg_load_le16(raw + 2) },
		.val = { .off = fsimg_load_le16(raw + 4),
		    .len = fsimg_load_le16(raw + 6) }
	});
}

/*
 * Reconstruct every codec-owned span. Arbitrary node payloads are copied;
 * their framing is the node codec's responsibility, their interpretation
 * belongs to the tree's record codec, checked separately below.
 */
static void
reencode(const unsigned char *raw, size_t length, unsigned char *out)
{
	apfs_obj_t object;
	apfs_nx_t nx;
	apfs_cpm_block_t checkpoint;
	apfs_cpm_t mapping;
	apfs_omap_t omap;
	apfs_node_t node;
	apfs_entry_t entry, encoded_entry;
	apfs_toc_t toc;
	apfs_kv_size_t sizes;
	apfs_spaceman_t spaceman;
	apfs_reaper_t reaper;
	apfs_cib_t chunks;
	apfs_chunk_info_t chunk;
	uint64_t address;

	bzero(out, length);
	expect(apfs_object_verify(raw, length), "valid input checksum");
	expect(apfs_obj_decode(raw, length, &object), "object decode");
	expect(apfs_obj_encode(out, length, &object), "object encode");
	switch (object.kind) {
	case APFS_KIND_NX_SUPERBLOCK:
		expect(apfs_nx_decode(raw, length, &nx), "superblock decode");
		expect(apfs_nx_encode(out, length, &nx), "superblock encode");
		break;
	case APFS_KIND_CHECKPOINT_MAP:
		expect(apfs_cpm_block_decode(raw, length, &checkpoint),
		    "checkpoint decode");
		expect(apfs_cpm_block_encode(out, length, &checkpoint),
		    "checkpoint encode");
		for (uint32_t i = 0; i < checkpoint.count; i++) {
			expect(apfs_cpm_decode(raw, length, &checkpoint, i,
			    &mapping), "mapping decode");
			expect(apfs_cpm_encode(out, length, &checkpoint, i,
			    &mapping), "mapping encode");
		}
		break;
	case APFS_KIND_OMAP:
		expect(apfs_omap_decode(raw, length, &omap), "omap decode");
		expect(apfs_omap_encode(out, length, &omap), "omap encode");
		break;
	case APFS_KIND_BTREE_ROOT:
	case APFS_KIND_BTREE_NODE:
		expect(apfs_node_decode(raw, length, &node), "node decode");
		expect(apfs_node_encode(out, length, &node), "node encode");
		sizes.key = (uint16_t)node.info.key_size;
		sizes.val = (uint16_t)node.info.val_size;
		/* Non-root fixtures have free-queue-shaped 16/8 records. */
		if ((node.flags & APFS_NODE_ROOT) == 0) {
			sizes.key = 16;
			sizes.val = 8;
		}
		for (uint32_t i = 0; i < node.key_count; i++) {
			expect(apfs_entry_decode(raw, length, &node, i, &sizes,
			    &entry), "entry decode");
			if ((node.flags & APFS_NODE_FIXED_KV) != 0) {
				toc.key.off = (uint16_t)(entry.key_off -
				    node.area.key_start);
				toc.key.len = (uint16_t)entry.key_len;
				toc.val.off = entry.ghost ? 0xffff :
				    (uint16_t)(node.area.value_end -
				    entry.val_off);
				toc.val.len = (uint16_t)entry.val_len;
			} else {
				toc = decode_variable_toc(raw +
				    node.area.toc_start +
				    APFS_TOC_VARIABLE_BYTES * i);
			}
			expect(apfs_entry_encode(out, length, &node, i, &toc,
			    &sizes, &encoded_entry), "entry encode");
			check_entry_spans(&encoded_entry, &entry);
			bcopy(raw + entry.key_off, out + entry.key_off,
			    entry.key_len);
			if (!entry.ghost)
				bcopy(raw + entry.val_off, out + entry.val_off,
				    entry.val_len);
		}
		break;
	case APFS_KIND_SPACEMAN:
		expect(apfs_spaceman_decode(raw, length, &spaceman),
		    "spaceman decode");
		expect(apfs_spaceman_encode(out, length, &spaceman),
		    "spaceman encode");
		break;
	case APFS_KIND_NX_REAPER:
		expect(apfs_reaper_decode(raw, length, &reaper),
		    "reaper decode");
		expect(apfs_reaper_encode(out, length, &reaper),
		    "reaper encode");
		break;
	case APFS_KIND_SPACEMAN_CIB:
		expect(apfs_cib_decode(raw, length, &chunks),
		    "chunk block decode");
		expect(apfs_cib_encode(out, length, &chunks),
		    "chunk block encode");
		for (uint32_t i = 0; i < chunks.count; i++) {
			expect(apfs_chunk_info_decode(raw, length, &chunks, i,
			    &chunk), "chunk decode");
			expect(apfs_chunk_info_encode(out, length, &chunks, i,
			    &chunk), "chunk encode");
		}
		break;
	case APFS_KIND_SPACEMAN_CAB:
		expect(apfs_cab_decode(raw, length, &chunks),
		    "address block decode");
		expect(apfs_cab_encode(out, length, &chunks),
		    "address block encode");
		for (uint32_t i = 0; i < chunks.count; i++) {
			expect(apfs_cab_address_decode(raw, length, &chunks, i,
			    &address), "address decode");
			expect(apfs_cab_address_encode(out, length, &chunks, i,
			    address), "address encode");
		}
		break;
	default:
		expect(object.kind == 0, "known fixture kind");
		break;
	}
	expect(apfs_object_seal(out, length), "seal reencoded object");
}

static void
check_nx(const unsigned char *raw, size_t length, const apfs_gen_nx_t *p)
{
	apfs_nx_t decoded;

	expect(apfs_nx_decode(raw, length, &decoded), "nx decode");
	expect(decoded.magic == p->magic, "nx.magic");
	expect(decoded.block_size == p->block_size, "nx.block_size");
	expect(decoded.block_count == p->block_count, "nx.block_count");
	expect(decoded.features == p->features, "nx.features");
	expect(decoded.ro_compat_features == p->ro_compat_features,
	    "nx.ro_compat_features");
	expect(decoded.incompat_features == p->incompat_features,
	    "nx.incompat_features");
	for (unsigned i = 0; i < 16; i++)
		expect(decoded.uuid[i] == p->uuid[i], "nx.uuid");
	expect(decoded.next_oid == p->next_oid, "nx.next_oid");
	expect(decoded.next_xid == p->next_xid, "nx.next_xid");
	expect(decoded.desc_blocks == (p->desc_blocks & 0x7fffffff),
	    "desc_blocks count");
	expect(decoded.desc_noncontiguous ==
	    ((p->desc_blocks & 0x80000000) != 0), "desc_blocks flag");
	expect(decoded.data_blocks == (p->data_blocks & 0x7fffffff),
	    "data_blocks count");
	expect(decoded.data_noncontiguous ==
	    ((p->data_blocks & 0x80000000) != 0), "data_blocks flag");
	expect(decoded.desc_base == p->desc_base, "nx.desc_base");
	expect(decoded.data_base == p->data_base, "nx.data_base");
	expect(decoded.desc_next == p->desc_next, "nx.desc_next");
	expect(decoded.data_next == p->data_next, "nx.data_next");
	expect(decoded.desc_index == p->desc_index, "nx.desc_index");
	expect(decoded.desc_len == p->desc_len, "nx.desc_len");
	expect(decoded.data_index == p->data_index, "nx.data_index");
	expect(decoded.data_len == p->data_len, "nx.data_len");
	expect(decoded.spaceman_oid == p->spaceman_oid, "nx.spaceman_oid");
	expect(decoded.omap_oid == p->omap_oid, "nx.omap_oid");
	expect(decoded.reaper_oid == p->reaper_oid, "nx.reaper_oid");
	expect(decoded.test_type == p->test_type, "nx.test_type");
	expect(decoded.max_volumes == p->max_volumes, "nx.max_volumes");
	for (unsigned i = 0; i < 100; i++)
		expect(decoded.volume_oids[i] == p->volume_oids[i],
		    "nx.volume_oids");
	for (unsigned i = 0; i < 32; i++)
		expect(decoded.counters[i] == p->counters[i], "nx.counters");
	expect(decoded.blocked_out.start == p->blocked_out_start,
	    "nx.blocked_out_start");
	expect(decoded.blocked_out.count == p->blocked_out_count,
	    "nx.blocked_out_count");
	expect(decoded.evict_mapping_oid == p->evict_mapping_oid,
	    "nx.evict_mapping_oid");
	expect(decoded.flags == p->flags, "nx.flags");
	expect(decoded.efi_jumpstart == p->efi_jumpstart, "nx.efi_jumpstart");
	for (unsigned i = 0; i < 16; i++)
		expect(decoded.fusion_uuid[i] == p->fusion_uuid[i],
		    "nx.fusion_uuid");
	expect(decoded.keylocker.start == p->keylocker_start,
	    "nx.keylocker_start");
	expect(decoded.keylocker.count == p->keylocker_count,
	    "nx.keylocker_count");
	for (unsigned i = 0; i < 4; i++)
		expect(decoded.ephemeral_info[i] == p->ephemeral_info[i],
		    "nx.ephemeral_info");
	expect(decoded.test_oid == p->test_oid, "nx.test_oid");
	expect(decoded.fusion_mt_oid == p->fusion_mt_oid, "nx.fusion_mt_oid");
	expect(decoded.fusion_wbc_oid == p->fusion_wbc_oid,
	    "nx.fusion_wbc_oid");
	expect(decoded.fusion_wbc.start == p->fusion_wbc_start,
	    "nx.fusion_wbc_start");
	expect(decoded.fusion_wbc.count == p->fusion_wbc_count,
	    "nx.fusion_wbc_count");
	expect(decoded.newest_mounted_version == p->newest_mounted_version,
	    "nx.newest_mounted_version");
	expect(decoded.media_keylocker.start == p->media_keylocker_start,
	    "nx.media_keylocker_start");
	expect(decoded.media_keylocker.count == p->media_keylocker_count,
	    "nx.media_keylocker_count");
}

static void
check_omap(const unsigned char *raw, size_t length, const apfs_gen_omap_t *p)
{
	apfs_omap_t decoded;

	expect(apfs_omap_decode(raw, length, &decoded), "omap decode");
	expect(decoded.flags == p->flags, "omap.flags");
	expect(decoded.snapshot_count == p->snapshot_count,
	    "omap.snapshot_count");
	expect(decoded.tree_type == p->tree_type, "omap.tree_type");
	expect(decoded.snapshot_tree_type == p->snapshot_tree_type,
	    "omap.snapshot_tree_type");
	expect(decoded.tree_oid == p->tree_oid, "omap.tree_oid");
	expect(decoded.snapshot_tree_oid == p->snapshot_tree_oid,
	    "omap.snapshot_tree_oid");
	expect(decoded.most_recent_snapshot == p->most_recent_snapshot,
	    "omap.most_recent_snapshot");
	expect(decoded.pending_revert_min == p->pending_revert_min,
	    "omap.pending_revert_min");
	expect(decoded.pending_revert_max == p->pending_revert_max,
	    "omap.pending_revert_max");
}

static void
check_spaceman(const unsigned char *raw, size_t length,
    const apfs_gen_spaceman_t *p)
{
	apfs_spaceman_t decoded;

	expect(apfs_spaceman_decode(raw, length, &decoded), "spaceman decode");
	expect(decoded.block_size == p->block_size, "spaceman.block_size");
	expect(decoded.blocks_per_chunk == p->blocks_per_chunk,
	    "spaceman.blocks_per_chunk");
	expect(decoded.chunks_per_cib == p->chunks_per_cib,
	    "spaceman.chunks_per_cib");
	expect(decoded.cibs_per_cab == p->cibs_per_cab,
	    "spaceman.cibs_per_cab");
	expect(decoded.flags == p->flags, "spaceman.flags");
	expect(decoded.ip_bm_tx_multiplier == p->ip_bm_tx_multiplier,
	    "spaceman.ip_bm_tx_multiplier");
	expect(decoded.ip_block_count == p->ip_block_count,
	    "spaceman.ip_block_count");
	expect(decoded.ip_bm_size_in_blocks == p->ip_bm_size_in_blocks,
	    "spaceman.ip_bm_size_in_blocks");
	expect(decoded.ip_bm_block_count == p->ip_bm_block_count,
	    "spaceman.ip_bm_block_count");
	expect(decoded.ip_bm_base == p->ip_bm_base, "spaceman.ip_bm_base");
	expect(decoded.ip_base == p->ip_base, "spaceman.ip_base");
	expect(decoded.fs_reserve_block_count == p->fs_reserve_block_count,
	    "spaceman.fs_reserve_block_count");
	expect(decoded.fs_reserve_alloc_count == p->fs_reserve_alloc_count,
	    "spaceman.fs_reserve_alloc_count");
	expect(decoded.ip_bm_free_head == p->ip_bm_free_head,
	    "spaceman.ip_bm_free_head");
	expect(decoded.ip_bm_free_tail == p->ip_bm_free_tail,
	    "spaceman.ip_bm_free_tail");
	expect(decoded.ip_bm_xid_offset == p->ip_bm_xid_offset,
	    "spaceman.ip_bm_xid_offset");
	expect(decoded.ip_bitmap_offset == p->ip_bitmap_offset,
	    "spaceman.ip_bitmap_offset");
	expect(decoded.ip_bm_free_next_offset == p->ip_bm_free_next_offset,
	    "spaceman.ip_bm_free_next_offset");
	expect(decoded.version == p->version, "spaceman.version");
	expect(decoded.struct_size == p->struct_size, "spaceman.struct_size");
	for (unsigned i = 0; i < 2; i++) {
		expect(decoded.devices[i].block_count ==
		    p->devices[i].block_count, "devices.block_count");
		expect(decoded.devices[i].chunk_count ==
		    p->devices[i].chunk_count, "devices.chunk_count");
		expect(decoded.devices[i].cib_count == p->devices[i].cib_count,
		    "devices.cib_count");
		expect(decoded.devices[i].cab_count == p->devices[i].cab_count,
		    "devices.cab_count");
		expect(decoded.devices[i].free_count ==
		    p->devices[i].free_count, "devices.free_count");
		expect(decoded.devices[i].addr_offset ==
		    p->devices[i].addr_offset, "devices.addr_offset");
	}
	for (unsigned i = 0; i < 3; i++) {
		expect(decoded.free_queues[i].count == p->free_queues[i].count,
		    "free_queues.count");
		expect(decoded.free_queues[i].tree_oid ==
		    p->free_queues[i].tree_oid, "free_queues.tree_oid");
		expect(decoded.free_queues[i].oldest_xid ==
		    p->free_queues[i].oldest_xid, "free_queues.oldest_xid");
		expect(decoded.free_queues[i].node_limit ==
		    p->free_queues[i].node_limit, "free_queues.node_limit");
	}
}

static void
check_reaper(const unsigned char *raw, size_t length,
    const apfs_gen_reaper_t *p)
{
	apfs_reaper_t decoded;

	expect(apfs_reaper_decode(raw, length, &decoded), "reaper decode");
	expect(decoded.next_reap_id == p->next_reap_id, "reaper.next_reap_id");
	expect(decoded.completed_id == p->completed_id, "reaper.completed_id");
	expect(decoded.head == p->head, "reaper.head");
	expect(decoded.tail == p->tail, "reaper.tail");
	expect(decoded.flags == p->flags, "reaper.flags");
	expect(decoded.rlcount == p->rlcount, "reaper.rlcount");
	expect(decoded.reaping_type == p->reaping_type, "reaper.reaping_type");
	expect(decoded.reaping_size == p->reaping_size, "reaper.reaping_size");
	expect(decoded.reaping_volume_oid == p->reaping_volume_oid,
	    "reaper.reaping_volume_oid");
	expect(decoded.reaping_oid == p->reaping_oid, "reaper.reaping_oid");
	expect(decoded.reaping_xid == p->reaping_xid, "reaper.reaping_xid");
	expect(decoded.reaping_entry_flags == p->reaping_entry_flags,
	    "reaper.reaping_entry_flags");
	expect(decoded.state_size == p->state_size, "reaper.state_size");
}

static apfs_gen_nx_t
native_superblock(unsigned block)
{
	apfs_gen_nx_t nx = {
		.magic = 0x4253584e, .block_size = 4096, .block_count = 1024,
		.incompat_features = 2,
		.uuid = { 0x5f, 0x58, 0x62, 0x67, 0x65, 0x19, 0x49, 0xe5,
		    0xa8, 0xd9, 0x18, 0xaa, 0xcd, 0xbe, 0x14, 0x62 },
		.next_oid = 1030, .next_xid = 3,
		.desc_blocks = 8, .data_blocks = 52,
		.desc_base = 1, .data_base = 9, .desc_next = 4, .data_next = 6,
		.desc_index = 2, .desc_len = 2, .data_index = 2, .data_len = 4,
		.spaceman_oid = 1024, .omap_oid = 91, .reaper_oid = 1025,
		.max_volumes = 1, .volume_oids = { 1026 },
		.ephemeral_info = { UINT64_C(4295229441) },
		.newest_mounted_version = UINT64_C(2811160007000004)
	};

	nx.counters[0] = block == 0 ? 24 : 21;
	return (nx);
}

static void
check_native_node(const unsigned char *raw, const native_node_t *fixture)
{
	apfs_node_t node;
	apfs_entry_t entry;
	apfs_omap_key_t omap_key;
	apfs_omap_val_t omap_value;
	apfs_fq_key_t queue_key;
	apfs_btree_info_t info;
	apfs_kv_size_t sizes = { .key = 16, .val = fixture->value_size };
	unsigned char encoded[40] = {0};
	uint64_t count;
	uint32_t toc_length = fixture->toc_length;

	expect(apfs_node_decode(raw, 4096, &node), "native node");
	expect(node.flags == 7 && node.level == 0 && node.key_count == 1,
	    "root leaf with one fixed record");
	expect(node.toc.off == 0 && node.toc.len == toc_length, "native TOC");
	expect(node.free.off == 16 && node.free.len ==
	    fixture->free_length, "native free space");
	expect(node.key_free.off == 0xffff && node.key_free.len == 0 &&
	    node.val_free.off == 0xffff && node.val_free.len == 0,
	    "empty free lists");
	expect(node.area.toc_start == 56 && node.area.key_start == 56 +
	    toc_length &&
	    node.area.free_start == 72 + toc_length && node.area.value_end ==
	    4056 &&
	    node.area.free_end == 4056u - sizes.val, "absolute node areas");
	expect(apfs_btree_info_decode(raw + 4056, 40, &info), "native footer");
	expect(info.flags == fixture->info_flags &&
	    info.node_size == 4096 && info.key_size == 16 &&
	    info.val_size == sizes.val && info.longest_key == 16 &&
	    info.longest_val == sizes.val && info.key_count == 1 &&
	    info.node_count == 1, "native footer fields");
	expect(apfs_btree_info_encode(encoded, 40, &info),
	    "btree info encode");
	expect(memcmp(encoded, raw + 4056, 40) == 0, "footer reencoding");
	expect(apfs_entry_decode(raw, 4096, &node, 0, &sizes, &entry),
	    "native TOC entry");
	expect(entry.key_off == node.area.key_start && entry.key_len == 16 &&
	    entry.val_off == 4056u - sizes.val && entry.val_len == sizes.val &&
	    !entry.ghost, "native record absolute spans");
	if (fixture->subtype == APFS_KIND_OMAP) {
		expect(apfs_omap_key_decode(raw + entry.key_off, 16, &omap_key),
		    "native omap key");
		expect(omap_key.oid == 1026 && omap_key.xid == 2, "volume key");
		expect(apfs_omap_val_decode(raw + entry.val_off, 16,
		    &omap_value), "native omap value");
		expect(omap_value.flags == 0 && omap_value.size == 4096 &&
		    omap_value.address == 90, "volume mapping");
		expect(apfs_omap_key_encode(encoded, 16, &omap_key),
		    "omap key encode");
		expect(memcmp(encoded, raw + entry.key_off, 16) == 0,
		    "omap key reencoding");
		expect(apfs_omap_val_encode(encoded, 16, &omap_value),
		    "omap val encode");
		expect(memcmp(encoded, raw + entry.val_off, 16) == 0,
		    "omap value reencoding");
	} else {
		expect(apfs_fq_key_decode(raw + entry.key_off, 16, &queue_key),
		    "native free queue key");
		expect(queue_key.xid == 2 && queue_key.address == 77,
		    "queued run");
		expect(apfs_fq_val_decode(raw + entry.val_off, 8, &count),
		    "native free queue value");
		expect(count == 2, "two queued blocks");
		expect(apfs_fq_key_encode(encoded, 16, &queue_key),
		    "fq key encode");
		expect(memcmp(encoded, raw + entry.key_off, 16) == 0,
		    "queue key reencoding");
		expect(apfs_fq_val_encode(encoded, 8, count),
		    "fq val encode");
		expect(memcmp(encoded, raw + entry.val_off, 8) == 0,
		    "queue value reencoding");
	}
}

static void
test_native_blocks(void)
{
	const apfs_gen_spaceman_t sm = {
		.block_size = 4096, .blocks_per_chunk = 32768,
		.chunks_per_cib = 126, .cibs_per_cab = 507, .flags = 1,
		.ip_bm_tx_multiplier = 16, .ip_block_count = 6,
		.ip_bm_size_in_blocks = 1, .ip_bm_block_count = 16,
		.ip_bm_base = 61, .ip_base = 77, .ip_bm_free_head = 2,
		.ip_bm_xid_offset = 2520, .ip_bitmap_offset = 2528,
		.ip_bm_free_next_offset = 2536, .version = 1, .struct_size =
		    2520,
		.devices = {
			{ .block_count = 1024, .chunk_count = 1, .cib_count = 1,
			  .free_count = 931, .addr_offset = 2568 },
			{ .addr_offset = 2576 }
		},
		.free_queues = {
			{ .count = 2, .tree_oid = 1027, .oldest_xid = 2,
			  .node_limit = 1 },
			{ .count = 2, .tree_oid = 1029, .oldest_xid = 2,
			  .node_limit = 1 }, {0}
		}
	};
	const apfs_gen_omap_t omap = { .flags = 1, .tree_type = 0x40000002,
		.snapshot_tree_type = 0x40000002, .tree_oid = 92 };
	const apfs_gen_reaper_t reaper = { .next_reap_id = 1, .flags = 1,
		.state_size = 3984 };
	const apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = 4096, .oid = 1024, .address =
		    11 },
		{ .type = 0x80000011, .size = 4096, .oid = 1025, .address =
		    12 },
		{ .type = 0x80000002, .subtype = 9, .size = 4096,
		  .oid = 1027, .address = 13 },
		{ .type = 0x80000002, .subtype = 9, .size = 4096,
		  .oid = 1029, .address = 14 }
	};
	const uint64_t checksums[] = {
		UINT64_C(0x8020c4843fdf332d),
		UINT64_C(0x92e32fc920428c4a), UINT64_C(0x81393fa03ec66fe0),
		UINT64_C(0x92e326ab2042956b), UINT64_C(0x40032177fffcddbe),
		UINT64_C(0x1e587fe192155aeb), UINT64_C(0x54d87fd42af09d1e),
		UINT64_C(0x004ced457fb2ff14), UINT64_C(0x9ef12d97d17cbdb0)
	};
	unsigned char raw[4096], encoded[4096];
	apfs_obj_t object;
	apfs_cpm_block_t checkpoint;
	apfs_cpm_t mapping;
	apfs_cib_t chunks;
	apfs_chunk_info_t chunk;
	apfs_gen_nx_t nx;
	char name[64];
	unsigned block;

	for (size_t i = 0; i < sizeof(native_blocks) / sizeof(native_blocks[0]);
	    i++) {
		block = native_blocks[i].number;
		snprintf(name, sizeof(name), "native block %u", block);
		scenario = name;
		expand_native(&native_blocks[i], raw);
		expect(apfs_object_verify(raw, sizeof(raw)), "native checksum");
		expect(apfs_gen_checksum(raw, sizeof(raw)) == checksums[i],
		    "independent native checksum");
		raw[1000] ^= 1;
		expect(!apfs_object_verify(raw, sizeof(raw)),
		    "corruption detected");
		raw[1000] ^= 1;
		expect(apfs_obj_decode(raw, sizeof(raw), &object),
		    "native header");
		expect(object.checksum == checksums[i] && object.xid == 2,
		    "header checksum and transaction");
		expect(object.oid == native_blocks[i].oid,
		    "native object identifier");
		expect(object.kind == native_blocks[i].kind, "native kind");
		expect(object.flags == native_blocks[i].flags,
		    "native storage class");
		expect(object.subtype == native_blocks[i].subtype,
		    "native subtype");
		switch (block) {
		case 0:
		case 4:
			nx = native_superblock(block);
			check_nx(raw, sizeof(raw), &nx);
			break;
		case 3:
			expect(apfs_cpm_block_decode(raw, sizeof(raw),
			    &checkpoint), "native checkpoint mapping header");
			expect(checkpoint.flags == 1 && checkpoint.count == 4,
			    "four mappings in last mapping block");
			for (uint32_t j = 0; j < 4; j++) {
				expect(apfs_cpm_decode(raw, sizeof(raw),
				    &checkpoint, j, &mapping),
				    "native mapping");
				expect(mapping.kind ==
				    (mappings[j].type & 0xffff) &&
				    mapping.flags ==
				    (mappings[j].type & 0xffff0000) &&
				    mapping.subtype == mappings[j].subtype &&
				    mapping.size == mappings[j].size &&
				    mapping.volume_oid == 0 &&
				    mapping.oid == mappings[j].oid &&
				    mapping.address == mappings[j].address,
				    "native mapping fields");
			}
			break;
		case 79:
			expect(apfs_cib_decode(raw, sizeof(raw), &chunks),
			    "native chunk header");
			expect(chunks.index == 0 && chunks.count == 1,
			    "native chunk index and count");
			expect(apfs_chunk_info_decode(raw, sizeof(raw), &chunks,
			    0, &chunk), "native chunk record");
			expect(chunk.xid == 2 && chunk.address == 0 &&
			    chunk.block_count == 1024 &&
			    chunk.free_count == 931 &&
			    chunk.bitmap_address == 80, "native chunk fields");
			break;
		case 91:
			/* tree_oid at byte 48 locates physical root 92. */
			check_omap(raw, sizeof(raw), &omap);
			break;
		case 92:
		case 13:
			for (size_t j = 0; j < sizeof(native_nodes) /
			    sizeof(native_nodes[0]); j++) {
				if (native_nodes[j].block == block)
					check_native_node(raw,
					    &native_nodes[j]);
			}
			break;
		case 11:
			check_spaceman(raw, sizeof(raw), &sm);
			break;
		case 12:
			check_reaper(raw, sizeof(raw), &reaper);
			break;
		}
		reencode(raw, sizeof(raw), encoded);
		if (block == 11) {
			/*
			 * Pool-ring and address arrays after byte 2520 lie
			 * outside the fixed codec. Their omission changes the
			 * checksum.
			 */
			expect(memcmp(raw + 8, encoded + 8, 2520 - 8) == 0,
			    "spaceman codec-owned bytes");
		} else {
			expect(memcmp(raw, encoded, sizeof(raw)) == 0,
			    "native byte-for-byte reencoding");
		}
	}
}

static void
test_fletcher(void)
{
	const size_t lengths[] = { 4096, 16384, 65536 };
	unsigned char raw[65536] = {0};
	const char *patterns[] = { "zero", "all bits set", "stride 37",
	    "stride 173" };
	apfs_fletcher_t sum;
	uint64_t checksum;
	unsigned stride;
	char name[96];

	scenario = "zero block Fletcher-64";
	expect(apfs_object_checksum(raw, 4096) == UINT64_MAX, "zero checksum");
	expect(!apfs_object_verify(raw, 4096),
	    "unwritten block never verifies");
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		for (unsigned pattern = 0; pattern < 4; pattern++) {
			snprintf(name, sizeof(name), "Fletcher %s, %zu bytes",
			    patterns[pattern], lengths[i]);
			scenario = name;
			stride = pattern == 2 ? 37 : 173;
			for (size_t j = 0; j < lengths[i]; j++) {
				if (pattern < 2)
					raw[j] = pattern == 0 ? 0 : 0xff;
				else
					raw[j] = (unsigned char)(j * stride +
					    j / 7);
			}

			checksum = apfs_object_checksum(raw, lengths[i]);
			expect(checksum == apfs_gen_checksum(raw, lengths[i]),
			    "weighted sum equals codec checksum");
			apfs_fletcher_init(&sum);
			apfs_fletcher_update(&sum, raw + 8, 4);
			apfs_fletcher_update(&sum, raw + 12, 0);
			apfs_fletcher_update(&sum, raw + 12, 100);
			apfs_fletcher_update(&sum, raw + 112, lengths[i] - 112);
			expect(apfs_fletcher_value(&sum) == checksum,
			    "incremental checksum");
			expect(apfs_gen_seal(raw, lengths[i]),
			    "independent sealing");
			expect(apfs_object_verify(raw, lengths[i]),
			    "sealed checksum");
		}
	}
}

/* Each rejection checks the complete destination, including struct padding. */
#define REJECT_OUTPUT(type, output, call) do { \
	type output, saved; \
	memset(&output, 0xa5, sizeof(output)); \
	bcopy(&output, &saved, sizeof(output)); \
	expect(!(call), "decoder rejects: " #call); \
	expect(memcmp(&output, &saved, sizeof(output)) == 0, \
	    "rejected decoder preserves output: " #call); \
} while (0)

#define REJECT_WRITE(buffer, call) do { \
	unsigned char saved[sizeof(buffer)]; \
	memset(buffer, 0xa5, sizeof(buffer)); \
	bcopy(buffer, saved, sizeof(buffer)); \
	expect(!(call), "encoder rejects: " #call); \
	expect(memcmp(buffer, saved, sizeof(buffer)) == 0, \
	    "rejected encoder preserves buffer: " #call); \
} while (0)

static void
test_spaceman_decodes_both_layouts_and_encodes_only_current(void)
{
	apfs_gen_spaceman_t generated;
	apfs_spaceman_t decoded;
	unsigned char raw[4096] = { 0 }, encoded[4096];

	scenario = "generated current space manager structure end";
	expect(apfs_gen_spaceman_geometry(4096, 1024, &generated) == 4096,
	    "current geometry");
	expect(apfs_gen_spaceman_build(raw, sizeof(raw), &generated),
	    "current builder");
	expect(apfs_spaceman_decode(raw, sizeof(raw), &decoded),
	    "current decode");
	expect(decoded.structure_end == APFS_SM_BYTES,
	    "current structure ends at 2520");

	scenario = "generated older space manager cannot encode";
	expect(apfs_gen_spaceman_geometry_older(4096, 1024, &generated) ==
	    4096, "older geometry");
	bzero(raw, sizeof(raw));
	/* These bytes belong to the older XID array, not version and size. */
	fsimg_store_le64(raw + 336, (UINT64_C(1) << 32) | 10);
	expect(apfs_gen_spaceman_build(raw, sizeof(raw), &generated),
	    "older builder");
	expect(apfs_spaceman_decode(raw, sizeof(raw), &decoded),
	    "older decode");
	expect(decoded.structure_end == APFS_SM_OLDER_BYTES,
	    "older structure ends at 336");
	expect(decoded.version == 0 && decoded.struct_size == 0,
	    "older layout has no version or structure size");
	REJECT_WRITE(encoded, apfs_spaceman_encode(encoded, sizeof(encoded),
	    &decoded));
}

static void
test_truncated_records(void)
{
	unsigned char raw[4096] = {0}, encoded[4096];
	apfs_obj_t object = {0};
	apfs_nx_t nx = {0};
	apfs_omap_t omap = {0};
	apfs_spaceman_t spaceman = { .structure_end = APFS_SM_BYTES };
	apfs_reaper_t reaper = {0};
	apfs_cpm_block_t checkpoint = { .count = 1 };
	apfs_cpm_t mapping = {0};
	apfs_cib_t chunks = { .count = 1 };
	apfs_chunk_info_t chunk = {0};
	apfs_node_t node = {0};
	apfs_omap_key_t omap_key = {0};
	apfs_omap_val_t omap_value = {0};
	apfs_fq_key_t queue_key = {0};
	apfs_btree_info_t info = {0};
	static char name[80];	/* scenario points here */

	scenario = "fixed structures one byte short";
	REJECT_OUTPUT(apfs_obj_t, decoded, apfs_obj_decode(raw, 31, &decoded));
	REJECT_WRITE(encoded, apfs_obj_encode(encoded, 31, &object));
	REJECT_OUTPUT(apfs_nx_t, decoded, apfs_nx_decode(raw, 1407, &decoded));
	REJECT_WRITE(encoded, apfs_nx_encode(encoded, 1407, &nx));
	REJECT_OUTPUT(apfs_omap_t, decoded, apfs_omap_decode(raw, 87,
	    &decoded));
	REJECT_WRITE(encoded, apfs_omap_encode(encoded, 87, &omap));
	REJECT_OUTPUT(apfs_spaceman_t, decoded, apfs_spaceman_decode(raw, 2519,
	    &decoded));
	REJECT_WRITE(encoded, apfs_spaceman_encode(encoded, 2519, &spaceman));
	REJECT_OUTPUT(apfs_reaper_t, decoded, apfs_reaper_decode(raw, 111,
	    &decoded));
	REJECT_WRITE(encoded, apfs_reaper_encode(encoded, 111, &reaper));
	REJECT_OUTPUT(apfs_cpm_block_t, decoded, apfs_cpm_block_decode(raw, 39,
	    &decoded));
	REJECT_WRITE(encoded, apfs_cpm_block_encode(encoded, 39, &checkpoint));
	REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cib_decode(raw, 39, &decoded));
	REJECT_WRITE(encoded, apfs_cib_encode(encoded, 39, &chunks));
	REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cab_decode(raw, 39, &decoded));
	REJECT_WRITE(encoded, apfs_cab_encode(encoded, 39, &chunks));
	REJECT_OUTPUT(apfs_node_t, decoded, apfs_node_decode(raw, 55,
	    &decoded));
	REJECT_WRITE(encoded, apfs_node_encode(encoded, 55, &node));
	REJECT_OUTPUT(apfs_cpm_t, decoded, apfs_cpm_decode(raw, 79, &checkpoint,
	    0, &decoded));
	REJECT_WRITE(encoded, apfs_cpm_encode(encoded, 79, &checkpoint, 0,
	    &mapping));
	REJECT_OUTPUT(apfs_chunk_info_t, decoded, apfs_chunk_info_decode(raw,
	    71, &chunks, 0, &decoded));
	REJECT_WRITE(encoded, apfs_chunk_info_encode(encoded, 71, &chunks, 0,
	    &chunk));
	REJECT_OUTPUT(uint64_t, decoded, apfs_cab_address_decode(raw, 47,
	    &chunks, 0, &decoded));
	REJECT_WRITE(encoded, apfs_cab_address_encode(encoded, 47, &chunks, 0,
	    123));

	for (size_t length = 0; length <= 41; length++) {
		snprintf(name, sizeof(name),
		    "exact record lengths, supplied %zu bytes", length);
		scenario = name;
		if (length != 16) {
			REJECT_OUTPUT(apfs_omap_key_t, decoded,
			    apfs_omap_key_decode(raw, length, &decoded));
			REJECT_OUTPUT(apfs_omap_val_t, decoded,
			    apfs_omap_val_decode(raw, length, &decoded));
			REJECT_OUTPUT(apfs_fq_key_t, decoded,
			    apfs_fq_key_decode(raw, length, &decoded));
			REJECT_WRITE(encoded, apfs_omap_key_encode(encoded,
			    length, &omap_key));
			REJECT_WRITE(encoded, apfs_omap_val_encode(encoded,
			    length, &omap_value));
			REJECT_WRITE(encoded, apfs_fq_key_encode(encoded,
			    length, &queue_key));
		}
		if (length != 8) {
			REJECT_OUTPUT(uint64_t, decoded, apfs_fq_val_decode(raw,
			    length, &decoded));
			REJECT_WRITE(encoded, apfs_fq_val_encode(encoded,
			    length, 123));
		}
		if (length != 40) {
			REJECT_OUTPUT(apfs_btree_info_t, decoded,
			    apfs_btree_info_decode(raw, length, &decoded));
			REJECT_WRITE(encoded, apfs_btree_info_encode(encoded,
			    length, &info));
		}
	}
}

static void
test_object_lengths(void)
{
	const size_t lengths[] = { 0, 28, 31, 33, 34, 35, 4095 };
	unsigned char raw[4096] = {0}, encoded[4096];
	char name[80];

	scenario = "minimum object length is accepted";
	expect(apfs_object_seal(raw, 32), "seal a 32-byte object");
	expect(apfs_object_verify(raw, 32), "verify a 32-byte object");

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		snprintf(name, sizeof(name), "invalid object length %zu",
		    lengths[i]);
		scenario = name;
		put_le(raw, apfs_gen_checksum(raw, lengths[i]), 8);

		expect(!apfs_object_verify(raw, lengths[i]),
		    "verify rejects length");
		REJECT_WRITE(encoded, apfs_object_seal(encoded, lengths[i]));
	}
}

static void
test_nx_area_counts(void)
{
	unsigned char encoded[4096];
	apfs_nx_t nx = {0}, decoded;

	scenario = "descriptor area count uses only 31 bits";
	nx.desc_blocks = 0x80000000;
	REJECT_WRITE(encoded, apfs_nx_encode(encoded, sizeof(encoded), &nx));

	scenario = "data area count uses only 31 bits";
	nx.desc_blocks = 0;
	nx.data_blocks = 0x80000000;
	REJECT_WRITE(encoded, apfs_nx_encode(encoded, sizeof(encoded), &nx));

	scenario = "maximum area counts preserve separate noncontiguous flags";
	nx.desc_blocks = 0x7fffffff;
	nx.data_blocks = 0x7fffffff;
	nx.desc_noncontiguous = true;
	nx.data_noncontiguous = true;

	expect(apfs_nx_encode(encoded, sizeof(encoded), &nx),
	    "encode maximum counts");
	expect(apfs_nx_decode(encoded, sizeof(encoded), &decoded),
	    "decode maximum counts");
	expect(decoded.desc_blocks == nx.desc_blocks &&
	    decoded.data_blocks == nx.data_blocks &&
	    decoded.desc_noncontiguous && decoded.data_noncontiguous,
	    "counts and flags remain distinct");
}

static void
test_counts_and_indexes(void)
{
	unsigned char raw[4096] = {0}, encoded[4096];
	const uint32_t counts[] = { 508, 0x80000000, UINT32_MAX - 1,
	    UINT32_MAX };
	const uint32_t indexes[] = { 1024, UINT32_MAX };
	apfs_cpm_block_t checkpoint = {0};
	apfs_cpm_t mapping = {0};
	apfs_cib_t chunks = {0};
	apfs_chunk_info_t chunk = {0};
	apfs_reaper_t reaper = {0};
	apfs_obj_t object = { .flags = 1 };
	char name[96];

	scenario = "object flags must not contain kind bits";
	REJECT_WRITE(encoded, apfs_obj_encode(encoded, sizeof(encoded),
	    &object));
	scenario = "mapping flags must not contain kind bits";
	checkpoint.count = 1;
	mapping.flags = 1;
	REJECT_WRITE(encoded, apfs_cpm_encode(encoded, sizeof(encoded),
	    &checkpoint, 0, &mapping));
	mapping.flags = 0;

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		snprintf(name, sizeof(name),
		    "count %u cannot overflow its block", counts[i]);
		scenario = name;
		put_le(raw + 36, counts[i], 4);
		checkpoint.count = counts[i];
		chunks.count = counts[i];
		REJECT_OUTPUT(apfs_cpm_block_t, decoded,
		    apfs_cpm_block_decode(raw, sizeof(raw), &decoded));
		REJECT_WRITE(encoded, apfs_cpm_block_encode(encoded,
		    sizeof(encoded), &checkpoint));
		REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cib_decode(raw,
		    sizeof(raw), &decoded));
		REJECT_WRITE(encoded, apfs_cib_encode(encoded, sizeof(encoded),
		    &chunks));
		REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cab_decode(raw,
		    sizeof(raw), &decoded));
		REJECT_WRITE(encoded, apfs_cab_encode(encoded, sizeof(encoded),
		    &chunks));
	}
	scenario = "two entries cannot fit a one-entry buffer";
	put_le(raw + 36, 2, 4);
	checkpoint.count = 2;
	chunks.count = 2;
	REJECT_OUTPUT(apfs_cpm_block_t, decoded, apfs_cpm_block_decode(raw, 80,
	    &decoded));
	REJECT_WRITE(encoded, apfs_cpm_block_encode(encoded, 80, &checkpoint));
	REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cib_decode(raw, 72, &decoded));
	REJECT_WRITE(encoded, apfs_cib_encode(encoded, 72, &chunks));
	REJECT_OUTPUT(apfs_cib_t, decoded, apfs_cab_decode(raw, 48, &decoded));
	REJECT_WRITE(encoded, apfs_cab_encode(encoded, 48, &chunks));

	checkpoint.count = UINT32_MAX;
	chunks.count = UINT32_MAX;
	for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
		snprintf(name, sizeof(name), "entry index %u cannot wrap",
		    indexes[i]);
		scenario = name;
		REJECT_OUTPUT(apfs_cpm_t, decoded,
		    apfs_cpm_decode(raw, sizeof(raw), &checkpoint, indexes[i],
		    &decoded));
		REJECT_WRITE(encoded, apfs_cpm_encode(encoded, sizeof(encoded),
		    &checkpoint, indexes[i], &mapping));
		REJECT_OUTPUT(apfs_chunk_info_t, decoded,
		    apfs_chunk_info_decode(raw, sizeof(raw), &chunks,
		    indexes[i],
		    &decoded));
		REJECT_WRITE(encoded, apfs_chunk_info_encode(encoded,
		    sizeof(encoded), &chunks, indexes[i], &chunk));
		REJECT_OUTPUT(uint64_t, decoded,
		    apfs_cab_address_decode(raw, sizeof(raw), &chunks,
		    indexes[i],
		    &decoded));
		REJECT_WRITE(encoded, apfs_cab_address_encode(encoded,
		    sizeof(encoded), &chunks, indexes[i], 99));
	}
	scenario = "index one lies past a single entry";
	REJECT_OUTPUT(apfs_cpm_t, decoded, apfs_cpm_decode(raw, 80, &checkpoint,
	    1, &decoded));
	REJECT_WRITE(encoded, apfs_cpm_encode(encoded, 80, &checkpoint, 1,
	    &mapping));
	REJECT_OUTPUT(apfs_chunk_info_t, decoded, apfs_chunk_info_decode(raw,
	    72, &chunks, 1, &decoded));
	REJECT_WRITE(encoded, apfs_chunk_info_encode(encoded, 72, &chunks, 1,
	    &chunk));
	REJECT_OUTPUT(uint64_t, decoded, apfs_cab_address_decode(raw, 48,
	    &chunks, 1, &decoded));
	REJECT_WRITE(encoded, apfs_cab_address_encode(encoded, 48, &chunks, 1,
	    99));

	scenario = "entry index equals count despite room in the buffer";
	put_le(raw + 36, 1, 4);
	expect(apfs_cpm_block_decode(raw, sizeof(raw), &checkpoint),
	    "single mapping header");
	expect(apfs_cib_decode(raw, sizeof(raw), &chunks),
	    "single chunk header");
	REJECT_OUTPUT(apfs_cpm_t, decoded,
	    apfs_cpm_decode(raw, sizeof(raw), &checkpoint, 1, &decoded));
	REJECT_WRITE(encoded, apfs_cpm_encode(encoded, sizeof(encoded),
	    &checkpoint, 1, &mapping));
	REJECT_OUTPUT(apfs_chunk_info_t, decoded,
	    apfs_chunk_info_decode(raw, sizeof(raw), &chunks, 1, &decoded));
	REJECT_WRITE(encoded, apfs_chunk_info_encode(encoded, sizeof(encoded),
	    &chunks, 1, &chunk));
	expect(apfs_cab_decode(raw, sizeof(raw), &chunks),
	    "single address header");
	REJECT_OUTPUT(uint64_t, decoded,
	    apfs_cab_address_decode(raw, sizeof(raw), &chunks, 1, &decoded));
	REJECT_WRITE(encoded, apfs_cab_address_encode(encoded, sizeof(encoded),
	    &chunks, 1, 99));

	scenario = "reaper state buffer extends past object";
	put_le(raw + 108, 3985, 4);
	reaper.state_size = 3985;
	REJECT_OUTPUT(apfs_reaper_t, decoded, apfs_reaper_decode(raw,
	    sizeof(raw), &decoded));
	REJECT_WRITE(encoded, apfs_reaper_encode(encoded, sizeof(encoded),
	    &reaper));
	put_le(raw + 108, UINT32_MAX, 4);
	reaper.state_size = UINT32_MAX;
	REJECT_OUTPUT(apfs_reaper_t, decoded, apfs_reaper_decode(raw,
	    sizeof(raw), &decoded));
	REJECT_WRITE(encoded, apfs_reaper_encode(encoded, sizeof(encoded),
	    &reaper));
}

static void
test_node_framing(void)
{
	const node_failure_t failures[] = {
		{ .name = "TOC extends beyond values", .length = 4096,
		  .count = 1, .toc = { .off = 4000, .len = 64 } },
		{ .name = "variable TOC shorter than key count", .length = 4096,
		  .count = 2, .toc = { .len = 8 } },
		{ .name = "fixed TOC shorter than key count", .length = 4096,
		  .flags = 4, .count = 2, .toc = { .len = 4 } },
		{ .name = "key count multiplication cannot wrap",
		    .length = 4096,
		  .count = UINT32_MAX, .toc = { .len = 8 } },
		{ .name = "free space past value end", .length = 4096,
		  .count = 1, .toc = { .len = 8 },
		  .free = { .off = 4000, .len = 64 } },
		{ .name = "root too short for footer", .length = 95,
		  .flags = 1, .count = 1, .toc = { .len = 8 } },
		{ .name = "node exceeds maximum length", .length = 65537,
		  .count = 1, .toc = { .len = 8 } },
		{ .name = "TOC offset cannot wrap u16", .length = 65536,
		  .count = 1, .toc = { .off = 65535, .len = 64 } },
		{ .name = "TOC length cannot wrap u16", .length = 65536,
		  .count = 1, .toc = { .off = 64, .len = 65535 } },
		{ .name = "free offset cannot wrap u16", .length = 65536,
		  .count = 1, .toc = { .len = 8 },
		  .free = { .off = 65535, .len = 64 } },
		{ .name = "free length cannot wrap u16", .length = 65536,
		  .count = 1, .toc = { .len = 8 },
		  .free = { .off = 64, .len = 65535 } }
	};
	unsigned char raw[65537], encoded[65537];
	apfs_node_t node;
	apfs_entry_t entry, encoded_entry;
	apfs_kv_size_t sizes = { .key = 16, .val = 8 };
	apfs_toc_t toc = {0};

	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		scenario = failures[i].name;
		bzero(raw, sizeof(raw));
		bzero(&node, sizeof(node));
		node.flags = failures[i].flags;
		node.key_count = failures[i].count;
		node.toc = failures[i].toc;
		node.free = failures[i].free;
		put_le(raw + 32, node.flags, 2);
		put_le(raw + 36, node.key_count, 4);
		put_le(raw + 40, node.toc.off, 2);
		put_le(raw + 42, node.toc.len, 2);
		put_le(raw + 44, node.free.off, 2);
		put_le(raw + 46, node.free.len, 2);
		REJECT_OUTPUT(apfs_node_t, decoded,
		    apfs_node_decode(raw, failures[i].length, &decoded));
		REJECT_WRITE(encoded, apfs_node_encode(encoded,
		    failures[i].length, &node));
		REJECT_OUTPUT(apfs_entry_t, decoded, apfs_entry_decode(raw,
		    failures[i].length, &node, 0, &sizes, &decoded));
		REJECT_WRITE(encoded, apfs_entry_encode(encoded,
		    failures[i].length, &node, 0, &toc, &sizes, &entry));
	}

	scenario = "node derived boundaries cannot be forged";
	bzero(raw, sizeof(raw));
	bzero(&node, sizeof(node));
	node.key_count = 1;
	node.flags = 4;
	node.toc.len = 4;
	node.free.off = 16;
	node.free.len = 4012;
	expect(apfs_node_encode(raw, 4096, &node), "valid fixed node header");
	expect(apfs_node_decode(raw, 4096, &node), "valid fixed node layout");
	toc.val.off = 8;
	expect(apfs_entry_encode(raw, 4096, &node, 0, &toc,
	    &sizes, &entry), "valid entry");

	scenario = "fixed entry requires external record sizes";
	REJECT_OUTPUT(apfs_entry_t, decoded, apfs_entry_decode(raw, 4096, &node,
	    0, NULL, &decoded));
	REJECT_WRITE(encoded, apfs_entry_encode(encoded, 4096, &node, 0, &toc,
	    NULL, &entry));

	scenario = "fixed entry index must be below key count";
	REJECT_OUTPUT(apfs_entry_t, decoded, apfs_entry_decode(raw, 4096, &node,
	    1, &sizes, &decoded));
	REJECT_OUTPUT(apfs_entry_t, decoded, apfs_entry_decode(raw, 4096, &node,
	    UINT32_MAX, &sizes, &decoded));
	REJECT_WRITE(encoded, apfs_entry_encode(encoded, 4096, &node, 1, &toc,
	    &sizes, &entry));
	REJECT_WRITE(encoded, apfs_entry_encode(encoded, 4096, &node,
	    UINT32_MAX, &toc, &sizes, &entry));

	scenario = "fixed entry value overlaps free space in shorter buffer";
	REJECT_OUTPUT(apfs_entry_t, decoded,
	    apfs_entry_decode(raw, 4095, &node, 0, &sizes, &decoded));
	/* The layout fits, but the value starts before the free area's end. */
	REJECT_WRITE(encoded, apfs_entry_encode(encoded, 4095, &node, 0, &toc,
	    &sizes, &entry));

	scenario = "fixed entry derives boundaries despite caller edits";
	node.area = (apfs_node_area_t){ .toc_start = UINT32_MAX,
		.key_start = 0, .free_start = UINT32_MAX, .free_end = 0,
		.value_end = UINT32_MAX };
	expect(apfs_entry_encode(encoded, 4096, &node, 0, &toc,
	    &sizes, &encoded_entry), "encoder derives its own area boundaries");
	REJECT_OUTPUT(apfs_entry_t, decoded,
	    apfs_entry_decode(encoded, 4095, &node, 0, &sizes, &decoded));
	REJECT_WRITE(encoded, apfs_entry_encode(encoded, 4095, &node, 0, &toc,
	    &sizes, &entry));
	expect(apfs_entry_decode(raw, 4096, &node, 0, &sizes, &entry),
	    "entry derives its own area boundaries");
	check_entry_spans(&encoded_entry, &entry);
	expect(entry.key_off == 60 && entry.key_len == 16 &&
	    entry.val_off == 4088 && entry.val_len == 8 && !entry.ghost,
	    "fixed entry absolute spans");
}

static void
test_entry_spans(void)
{
	const entry_failure_t failures[] = {
		{ .name = "key past free start",
		  .toc = { .key = { .off = 1, .len = 16 },
		    .val = { .off = 8, .len = 8 } } },
		{ .name = "value before free end",
		  .toc = { .key = { .len = 16 },
		    .val = { .off = 9, .len = 8 } } },
		{ .name = "value length exceeds backward offset",
		  .toc = { .key = { .len = 16 },
		    .val = { .off = 7, .len = 8 } } },
		{ .name = "key u16 sum cannot wrap",
		  .toc = { .key = { .off = 65535, .len = 16 } } },
		{ .name = "value offset near ghost sentinel",
		  .toc = { .key = { .len = 16 },
		    .val = { .off = 65534, .len = 8 } } }
	};
	unsigned char raw[4096] = {0}, encoded[4096];
	apfs_node_t node = { .key_count = 1, .toc = { .len = 8 },
		.free = { .off = 16, .len = 4008 } };
	apfs_toc_t toc = { .key = { .len = 16 },
		.val = { .off = 8, .len = 8 } };
	apfs_entry_t entry, encoded_entry;

	scenario = "variable entry absolute offsets";
	expect(apfs_node_encode(raw, sizeof(raw), &node), "valid node");
	expect(apfs_node_decode(raw, sizeof(raw), &node),
	    "derive node boundaries");

	expect(apfs_entry_encode(raw, sizeof(raw), &node, 0, &toc,
	    NULL, &encoded_entry), "valid TOC");
	expect(apfs_entry_decode(raw, sizeof(raw), &node, 0, NULL, &entry),
	    "valid variable entry");
	check_entry_spans(&encoded_entry, &entry);
	expect(entry.key_off == 64 && entry.key_len == 16 &&
	    entry.val_off == 4088 && entry.val_len == 8 && !entry.ghost,
	    "variable entry spans");

	/* Forging wider derived areas cannot admit any of the invalid spans. */
	node.area = (apfs_node_area_t){ .key_start = 0,
		.free_start = UINT32_MAX, .free_end = 0,
		.value_end = UINT32_MAX };
	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		scenario = failures[i].name;
		toc = failures[i].toc;
		/* Malformed TOC bytes are independent of the encoder. */
		put_le(raw + 56, toc.key.off, 2);
		put_le(raw + 58, toc.key.len, 2);
		put_le(raw + 60, toc.val.off, 2);
		put_le(raw + 62, toc.val.len, 2);

		REJECT_WRITE(encoded, apfs_entry_encode(encoded,
		    sizeof(encoded), &node, 0, &toc, NULL, &encoded_entry));
		REJECT_OUTPUT(apfs_entry_t, decoded,
		    apfs_entry_decode(raw, sizeof(raw), &node, 0, NULL,
		    &decoded));
	}

	scenario = "ghost ignores stored value length";
	toc = (apfs_toc_t){ .key = { .len = 16 },
		.val = { .off = 0xffff, .len = 0xffff } };

	expect(apfs_entry_encode(raw, sizeof(raw), &node, 0, &toc,
	    NULL, &encoded_entry), "ghost TOC");
	expect(apfs_entry_decode(raw, sizeof(raw), &node, 0, NULL, &entry),
	    "ghost decode");
	check_entry_spans(&encoded_entry, &entry);
	expect(entry.ghost && entry.val_off == 0 && entry.val_len == 0 &&
	    entry.key_off == 64 && entry.key_len == 16,
	    "ghost has no value span");
}

static void
generated_roundtrip(const unsigned char *raw, size_t length,
    const apfs_gen_object_t *expected)
{
	unsigned char encoded[65536];
	apfs_obj_t object;

	expect(apfs_object_verify(raw, length), "generated object verifies");
	expect(apfs_obj_decode(raw, length, &object),
	    "generated object header");
	expect(object.oid == expected->oid && object.xid == expected->xid &&
	    object.kind == (expected->type & 0xffff) &&
	    object.flags == (expected->type & 0xffff0000) &&
	    object.subtype == expected->subtype, "generated header parameters");
	reencode(raw, length, encoded);
	expect(memcmp(raw, encoded, length) == 0,
	    "generated byte-for-byte roundtrip");
}

static void
test_generated_fixed_objects(void)
{
	unsigned char raw[4096];
	apfs_gen_object_t object = { .oid = UINT64_C(0x123456789abcdef0),
		.xid = UINT64_C(0xfedcba9876543210), .type = 0xc8000000,
		.subtype = 0xdeadbeef };
	apfs_gen_nx_t nx = {0};
	apfs_gen_omap_t omap = {0};
	apfs_gen_spaceman_t spaceman = {0};
	apfs_gen_reaper_t reaper = {0};

	scenario = "generated object header";
	bzero(raw, sizeof(raw));

	expect(apfs_gen_object_build(raw, sizeof(raw), &object),
	    "object builder");
	generated_roundtrip(raw, sizeof(raw), &object);

	/* Distinct byte patterns exercise codecs without driver policy. */
	scenario = "generated nx all fields";
	nx.object = object;
	nx.object.type = 0x80000001;
	nx.magic = 0x89340000u + 32;
	nx.block_size = 0x89340000u + 36;
	nx.block_count = UINT64_C(0x8123456700000000) + 40;
	nx.features = UINT64_C(0x8123456700000000) + 48;
	nx.ro_compat_features = UINT64_C(0x8123456700000000) + 56;
	nx.incompat_features = UINT64_C(0x8123456700000000) + 64;
	for (unsigned i = 0; i < 16; i++)
		nx.uuid[i] = (uint8_t)(0x80 + i);
	nx.next_oid = UINT64_C(0x8123456700000000) + 88;
	nx.next_xid = UINT64_C(0x8123456700000000) + 96;
	nx.desc_blocks = 0x89340000u + 104;
	nx.data_blocks = 0x89340000u + 108;
	nx.desc_base = UINT64_C(0x8123456700000000) + 112;
	nx.data_base = UINT64_C(0x8123456700000000) + 120;
	nx.desc_next = 0x89340000u + 128;
	nx.data_next = 0x89340000u + 132;
	nx.desc_index = 0x89340000u + 136;
	nx.desc_len = 0x89340000u + 140;
	nx.data_index = 0x89340000u + 144;
	nx.data_len = 0x89340000u + 148;
	nx.spaceman_oid = UINT64_C(0x8123456700000000) + 152;
	nx.omap_oid = UINT64_C(0x8123456700000000) + 160;
	nx.reaper_oid = UINT64_C(0x8123456700000000) + 168;
	nx.test_type = 0x89340000u + 176;
	nx.max_volumes = 0x89340000u + 180;
	for (unsigned i = 0; i < 100; i++)
		nx.volume_oids[i] = UINT64_C(0x8123456700000000) + 184 + i;
	for (unsigned i = 0; i < 32; i++)
		nx.counters[i] = UINT64_C(0x8123456700000000) + 984 + i;
	nx.blocked_out_start = UINT64_C(0x8123456700000000) + 1240;
	nx.blocked_out_count = UINT64_C(0x8123456700000000) + 1248;
	nx.evict_mapping_oid = UINT64_C(0x8123456700000000) + 1256;
	nx.flags = UINT64_C(0x8123456700000000) + 1264;
	nx.efi_jumpstart = UINT64_C(0x8123456700000000) + 1272;
	for (unsigned i = 0; i < 16; i++)
		nx.fusion_uuid[i] = (uint8_t)(0x80 + i);
	nx.keylocker_start = UINT64_C(0x8123456700000000) + 1296;
	nx.keylocker_count = UINT64_C(0x8123456700000000) + 1304;
	for (unsigned i = 0; i < 4; i++)
		nx.ephemeral_info[i] = UINT64_C(0x8123456700000000) + 1312 + i;
	nx.test_oid = UINT64_C(0x8123456700000000) + 1344;
	nx.fusion_mt_oid = UINT64_C(0x8123456700000000) + 1352;
	nx.fusion_wbc_oid = UINT64_C(0x8123456700000000) + 1360;
	nx.fusion_wbc_start = UINT64_C(0x8123456700000000) + 1368;
	nx.fusion_wbc_count = UINT64_C(0x8123456700000000) + 1376;
	nx.newest_mounted_version = UINT64_C(0x8123456700000000) + 1384;
	nx.media_keylocker_start = UINT64_C(0x8123456700000000) + 1392;
	nx.media_keylocker_count = UINT64_C(0x8123456700000000) + 1400;
	bzero(raw, sizeof(raw));

	expect(apfs_gen_nx_build(raw, sizeof(raw), &nx), "nx builder");
	check_nx(raw, sizeof(raw), &nx);
	generated_roundtrip(raw, sizeof(raw), &nx.object);

	scenario = "generated omap all fields";
	omap.object = object;
	omap.object.type = 0x4000000b;
	omap.flags = 0x89340000u + 32;
	omap.snapshot_count = 0x89340000u + 36;
	omap.tree_type = 0x89340000u + 40;
	omap.snapshot_tree_type = 0x89340000u + 44;
	omap.tree_oid = UINT64_C(0x8123456700000000) + 48;
	omap.snapshot_tree_oid = UINT64_C(0x8123456700000000) + 56;
	omap.most_recent_snapshot = UINT64_C(0x8123456700000000) + 64;
	omap.pending_revert_min = UINT64_C(0x8123456700000000) + 72;
	omap.pending_revert_max = UINT64_C(0x8123456700000000) + 80;
	bzero(raw, sizeof(raw));

	expect(apfs_gen_omap_build(raw, sizeof(raw), &omap), "omap builder");
	check_omap(raw, sizeof(raw), &omap);
	generated_roundtrip(raw, sizeof(raw), &omap.object);

	scenario = "generated spaceman all fields";
	spaceman.object = object;
	spaceman.object.type = 0x80000005;
	spaceman.block_size = 0x89340000u + 32;
	spaceman.blocks_per_chunk = 0x89340000u + 36;
	spaceman.chunks_per_cib = 0x89340000u + 40;
	spaceman.cibs_per_cab = 0x89340000u + 44;
	spaceman.flags = 0x89340000u + 144;
	spaceman.ip_bm_tx_multiplier = 0x89340000u + 148;
	spaceman.ip_block_count = UINT64_C(0x8123456700000000) + 152;
	spaceman.ip_bm_size_in_blocks = 0x89340000u + 160;
	spaceman.ip_bm_block_count = 0x89340000u + 164;
	spaceman.ip_bm_base = UINT64_C(0x8123456700000000) + 168;
	spaceman.ip_base = UINT64_C(0x8123456700000000) + 176;
	spaceman.fs_reserve_block_count = UINT64_C(0x8123456700000000) + 184;
	spaceman.fs_reserve_alloc_count = UINT64_C(0x8123456700000000) + 192;
	spaceman.ip_bm_free_head = 0x8100u + 13;
	spaceman.ip_bm_free_tail = 0x8100u + 14;
	spaceman.ip_bm_xid_offset = 0x89340000u + 324;
	spaceman.ip_bitmap_offset = 0x89340000u + 328;
	spaceman.ip_bm_free_next_offset = 0x89340000u + 332;
	spaceman.version = 0x89340000u + 336;
	spaceman.struct_size = 0x89340000u + 340;
	for (unsigned i = 0; i < 2; i++) {
		spaceman.devices[i].block_count = UINT64_C(0xfedcba9800000000) +
		    i;
		spaceman.devices[i].chunk_count = UINT64_C(0xfedcba9800000000) +
		    8 + i;
		spaceman.devices[i].cib_count = 0xa1230000u + 16 + i;
		spaceman.devices[i].cab_count = 0xa1230000u + 20 + i;
		spaceman.devices[i].free_count = UINT64_C(0xfedcba9800000000) +
		    24 + i;
		spaceman.devices[i].addr_offset = 0xa1230000u + 32 + i;
	}
	for (unsigned i = 0; i < 3; i++) {
		spaceman.free_queues[i].count = UINT64_C(0xfedcba9800000000) +
		    i;
		spaceman.free_queues[i].tree_oid =
		    UINT64_C(0xfedcba9800000000) + 8 + i;
		spaceman.free_queues[i].oldest_xid =
		    UINT64_C(0xfedcba9800000000) + 16 + i;
		spaceman.free_queues[i].node_limit = (uint16_t)(0x1234 + i);
	}
	bzero(raw, sizeof(raw));

	expect(apfs_gen_spaceman_build(raw, sizeof(raw), &spaceman),
	    "spaceman builder");
	check_spaceman(raw, sizeof(raw), &spaceman);
	generated_roundtrip(raw, sizeof(raw), &spaceman.object);

	scenario = "generated reaper all fields";
	reaper.object = object;
	reaper.object.type = 0x80000011;
	reaper.next_reap_id = UINT64_C(0x8123456700000000) + 32;
	reaper.completed_id = UINT64_C(0x8123456700000000) + 40;
	reaper.head = UINT64_C(0x8123456700000000) + 48;
	reaper.tail = UINT64_C(0x8123456700000000) + 56;
	reaper.flags = 0x89340000u + 64;
	reaper.rlcount = 0x89340000u + 68;
	reaper.reaping_type = 0x89340000u + 72;
	reaper.reaping_size = 0x89340000u + 76;
	reaper.reaping_volume_oid = UINT64_C(0x8123456700000000) + 80;
	reaper.reaping_oid = UINT64_C(0x8123456700000000) + 88;
	reaper.reaping_xid = UINT64_C(0x8123456700000000) + 96;
	reaper.reaping_entry_flags = 0x89340000u + 104;
	reaper.state_size = 1024;
	bzero(raw, sizeof(raw));

	expect(apfs_gen_reaper_build(raw, sizeof(raw), &reaper),
	    "reaper builder");
	check_reaper(raw, sizeof(raw), &reaper);
	generated_roundtrip(raw, sizeof(raw), &reaper.object);
}

static void
test_generated_arrays(void)
{
	unsigned char raw[4096];
	const apfs_gen_mapping_t mappings[] = {
		{ .type = 0x80000005, .size = 4096, .oid = 1024, .address =
		    11 },
		{ .type = 0x80000002, .subtype = 9, .size = 8192,
		  .volume_oid = UINT64_C(0x123456789abcdef0),
		  .oid = 1027, .address = 13 },
		{ .type = 0x80000011, .size = 16384, .oid = 1025, .address =
		    17 }
	};
	const apfs_gen_chunk_t records[] = {
		{ .xid = UINT64_C(0xfedcba9876543210), .address = 0,
		  .block_count = 32768, .free_count = 30000, .bitmap_address =
		      80 },
		{ .xid = 3, .address = 32768, .block_count = 32768,
		  .free_count = 32768, .bitmap_address = 0 },
		{ .xid = 4, .address = 65536, .block_count = 1024,
		  .free_count = 991, .bitmap_address =
		      UINT64_C(0x8123456789abcdef) }
	};
	const uint64_t addresses[] = { 79, UINT64_C(0xabcdef9876543210), 81 };
	apfs_gen_checkpoint_t checkpoint = {
		.object = { .oid = 3, .xid = 2, .type = 0x4000000c },
		.mappings = mappings, .flags = 1, .count = 3
	};
	apfs_gen_chunks_t chunks = {
		.object = { .oid = 79, .xid = 2, .type = 0x40000007 },
		.chunks = records, .index = 0x12345678, .count = 3
	};
	apfs_gen_addresses_t cab = {
		.object = { .oid = 80, .xid = 2, .type = 0x40000006 },
		.addresses = addresses, .index = 0x87654321, .count = 3
	};
	apfs_cpm_block_t cp_header;
	apfs_cpm_t mapping;
	apfs_cib_t header;
	apfs_chunk_info_t chunk;
	uint64_t address;

	scenario = "generated checkpoint mappings";
	bzero(raw, sizeof(raw));

	expect(apfs_gen_checkpoint_build(raw, sizeof(raw), &checkpoint),
	    "checkpoint builder");
	expect(apfs_cpm_block_decode(raw, sizeof(raw), &cp_header),
	    "checkpoint header");
	expect(cp_header.count == 3 && cp_header.flags == 1,
	    "checkpoint parameters");
	for (uint32_t i = 0; i < 3; i++) {
		expect(apfs_cpm_decode(raw, sizeof(raw), &cp_header, i,
		    &mapping), "mapping decode");
		expect(mapping.kind == (mappings[i].type & 0xffff) &&
		    mapping.flags == (mappings[i].type & 0xffff0000) &&
		    mapping.subtype == mappings[i].subtype &&
		    mapping.size == mappings[i].size &&
		    mapping.volume_oid == mappings[i].volume_oid &&
		    mapping.oid == mappings[i].oid &&
		    mapping.address == mappings[i].address,
		    "mapping parameters");
	}
	generated_roundtrip(raw, sizeof(raw), &checkpoint.object);

	scenario = "generated chunk information";
	bzero(raw, sizeof(raw));

	expect(apfs_gen_chunks_build(raw, sizeof(raw), &chunks),
	    "chunk block builder");
	expect(apfs_cib_decode(raw, sizeof(raw), &header),
	    "chunk block header");
	expect(header.index == chunks.index && header.count == 3,
	    "chunk block parameters");
	for (uint32_t i = 0; i < 3; i++) {
		expect(apfs_chunk_info_decode(raw, sizeof(raw), &header, i,
		    &chunk), "chunk decode");
		expect(chunk.xid == records[i].xid && chunk.address ==
		    records[i].address &&
		    chunk.block_count == records[i].block_count &&
		    chunk.free_count == records[i].free_count &&
		    chunk.bitmap_address == records[i].bitmap_address,
		    "chunk parameters");
	}
	generated_roundtrip(raw, sizeof(raw), &chunks.object);

	scenario = "generated chunk address block";
	bzero(raw, sizeof(raw));

	expect(apfs_gen_addresses_build(raw, sizeof(raw), &cab),
	    "address block builder");
	expect(apfs_cab_decode(raw, sizeof(raw), &header),
	    "address block header");
	expect(header.index == cab.index && header.count == 3,
	    "address block parameters");
	for (uint32_t i = 0; i < 3; i++) {
		expect(apfs_cab_address_decode(raw, sizeof(raw), &header, i,
		    &address), "address decode");
		expect(address == addresses[i], "address parameters");
	}
	generated_roundtrip(raw, sizeof(raw), &cab.object);
}

static void
test_generated_queue_records(void)
{
	unsigned char key[16] = {0}, value[8] = {0}, encoded[16] = {0};
	const apfs_gen_fq_key_t params = { .xid = UINT64_C(0x123456789abcdef0),
		.address = UINT64_C(0xfedcba9876543210) };
	const uint64_t count = UINT64_C(0x87654321abcdef09);
	apfs_fq_key_t decoded;
	uint64_t decoded_count;

	scenario = "generated free queue key and value";
	expect(apfs_gen_fq_key_build(key, sizeof(key), &params),
	    "free queue key builder");
	expect(apfs_gen_fq_value_build(value, sizeof(value), count),
	    "free queue value builder");
	expect(apfs_fq_key_decode(key, sizeof(key), &decoded), "key decode");
	expect(decoded.xid == params.xid && decoded.address == params.address,
	    "free queue key parameters");
	expect(apfs_fq_val_decode(value, sizeof(value), &decoded_count),
	    "value decode");
	expect(decoded_count == count, "free queue count parameter");
	expect(apfs_fq_key_encode(encoded, 16, &decoded),
	    "fq key encode");
	expect(memcmp(key, encoded, sizeof(key)) == 0, "key roundtrip");
	expect(apfs_fq_val_encode(encoded, 8, decoded_count),
	    "fq val encode");
	expect(memcmp(value, encoded, sizeof(value)) == 0, "value roundtrip");
}

/* Backing bytes are static so record parameters remain valid after return. */
static void
build_node_fixture(unsigned char *raw, size_t length,
    const node_case_t *fixture, apfs_gen_node_t *params,
    apfs_gen_record_t records[3])
{
	static const unsigned char keys[3][16] = {
		{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 },
		{ 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
		    36 },
		{ 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
		    56 }
	};
	static const unsigned char values[3][8] = {
		{ 81, 82, 83, 84, 85, 86, 87, 88 },
		{ 91, 92, 93, 94, 95, 96, 97, 98 },
		{ 101, 102, 103, 104, 105, 106, 107, 108 }
	};
	bool root = (fixture->flags & GEN_APFS_ROOT) != 0;
	bool fixed = (fixture->flags & GEN_APFS_FIXED) != 0;

	/* A middle ghost exercises values before and after an absent value. */
	bzero(params, sizeof(*params));
	params->object = (apfs_gen_object_t){
	    .oid = 42, .xid = 9,
	    .type = 0x80000000 | (root ? APFS_KIND_BTREE_ROOT :
	    APFS_KIND_BTREE_NODE), .subtype = 9 };
	params->flags = fixture->flags;
	params->level = fixture->level;
	params->records = records;
	params->count = 3;
	params->toc_capacity = 128;
	params->info = (apfs_gen_info_t){ .flags = 0xe,
	    .node_size = (uint32_t)length, .key_size = fixed ? 16 : 0,
	    .val_size = fixed ? 8 : 0, .longest_key = 16, .longest_val = 8,
	    .key_count = 3, .node_count = 1 };
	for (unsigned i = 0; i < 3; i++) {
		records[i] = (apfs_gen_record_t){
		    .key = keys[i], .value = values[i], .ghost = i == 1,
		    .key_size = fixed ? 16 :
		    (uint16_t)(5 + i * 3), .value_size = fixed ? 8 :
		    (uint16_t)(4 + i) };
	}
	bzero(raw, length);
	expect(apfs_gen_node_build(raw, length, params), "node packer");
}

static void
check_generated_node(const unsigned char *raw, size_t length,
    const apfs_gen_node_t *params, const char *name)
{
	const apfs_gen_record_t *records = params->records;
	apfs_node_t node;
	apfs_entry_t entry;
	apfs_kv_size_t sizes = { .key = 16, .val = 8 };
	size_t key_cursor, value_cursor, entry_size;
	size_t key_total = 0, value_total = 0, free_length;
	char record_name[160];
	bool root = (params->flags & GEN_APFS_ROOT) != 0;
	bool fixed = (params->flags & GEN_APFS_FIXED) != 0;

	for (unsigned i = 0; i < params->count; i++) {
		key_total += records[i].key_size;
		if (!records[i].ghost)
			value_total += records[i].value_size;
	}
	/* Subtract header, TOC, record bytes and root footer from the block. */
	free_length = length - GEN_APFS_NODE_HEADER - params->toc_capacity;
	free_length -= key_total + value_total;
	if (root)
		free_length -= GEN_APFS_FOOTER;
	expect(apfs_node_decode(raw, length, &node),
	    "packed node decode");
	expect(node.flags == params->flags &&
	    node.level == params->level &&
	    node.key_count == 3 && node.toc.off == 0 &&
	    node.toc.len == 128, "packed header parameters");
	expect(node.free.off == key_total && node.free.len ==
	    free_length, "packed free space");
	if (root) {
		expect(node.info.flags == params->info.flags &&
		    node.info.node_size ==
		    params->info.node_size &&
		    node.info.key_size ==
		    params->info.key_size &&
		    node.info.val_size ==
		    params->info.val_size &&
		    node.info.longest_key ==
		    params->info.longest_key &&
		    node.info.longest_val ==
		    params->info.longest_val &&
		    node.info.key_count ==
		    params->info.key_count &&
		    node.info.node_count ==
		    params->info.node_count, "packed root footer parameters");
	}
	key_cursor = 56 + 128;
	value_cursor = length - (root ? 40 : 0);
	entry_size = fixed ? 4 : 8;
	expect(node.area.toc_start == 56 &&
	    node.area.key_start == key_cursor &&
	    node.area.free_start == key_cursor + key_total &&
	    node.area.free_end == value_cursor - value_total &&
	    node.area.value_end == value_cursor, "packed absolute areas");
	for (unsigned i = 0; i < 3; i++) {
		snprintf(record_name, sizeof(record_name),
		    "%s, record %u", name, i);
		scenario = record_name;
		expect(apfs_entry_decode(raw, length, &node,
		    i, &sizes, &entry), "packed record span");
		expect(entry.key_off == key_cursor &&
		    entry.key_len == records[i].key_size &&
		    entry.ghost == records[i].ghost, "packed key and ghost");
		expect(memcmp(raw + entry.key_off, records[i].key,
		    entry.key_len) == 0, "packed key bytes");
		key_cursor += records[i].key_size;
		if (entry.ghost) {
			expect(entry.val_off == 0 &&
			    entry.val_len == 0,
			    "packed ghost consumes no value");
			if (!fixed)
				expect(fsimg_load_le16(raw +
				    node.area.toc_start + 8 * i + 6) == 0,
				    "variable ghost stores zero value length");
		} else {
			value_cursor -= records[i].value_size;
			expect(entry.val_off == value_cursor &&
			    entry.val_len ==
			    records[i].value_size,
			    "packed value absolute span");
			expect(memcmp(raw + entry.val_off,
			    records[i].value, entry.val_len) == 0,
			    "packed value bytes");
		}
	}
	scenario = name;
	for (size_t i = node.area.toc_start + 3 * entry_size;
	    i < node.area.key_start; i++)
		expect(raw[i] == 0, "unused TOC zeroed");
	for (size_t i = node.area.free_start;
	    i < node.area.free_end; i++)
		expect(raw[i] == 0, "free space zeroed");
	generated_roundtrip(raw, length, &params->object);
}

static void
test_generated_nodes(void)
{
	const node_case_t cases[] = {
		{ .name = "variable non-root index", .level = 2 },
		{ .name = "variable root leaf", .flags = GEN_APFS_ROOT |
		    GEN_APFS_LEAF },
		{ .name = "fixed non-root leaf", .flags = GEN_APFS_FIXED |
		    GEN_APFS_LEAF },
		{ .name = "fixed root leaf",
		  .flags = GEN_APFS_ROOT | GEN_APFS_LEAF | GEN_APFS_FIXED }
	};
	const size_t lengths[] = { 4096, 16384, 65536 };
	apfs_gen_record_t records[3];
	apfs_gen_node_t params;
	unsigned char raw[65536];
	char name[128];

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for (size_t b = 0; b < sizeof(lengths) / sizeof(lengths[0]);
		    b++) {
			snprintf(name, sizeof(name), "generated %s, %zu bytes",
			    cases[c].name, lengths[b]);
			scenario = name;
			build_node_fixture(raw, lengths[b], &cases[c], &params,
			    records);
			check_generated_node(raw, lengths[b], &params, name);
		}
	}
}

static void
test_generator_rejections(void)
{
	unsigned char encoded[4096];
	apfs_gen_object_t object = {0};
	apfs_gen_nx_t nx = {0};
	apfs_gen_omap_t omap = {0};
	apfs_gen_spaceman_t spaceman = {0};
	apfs_gen_reaper_t reaper = {0};
	apfs_gen_checkpoint_t checkpoint = { .count = UINT32_MAX };
	apfs_gen_chunks_t chunks = { .count = UINT32_MAX };
	apfs_gen_addresses_t addresses = { .count = UINT32_MAX };
	apfs_gen_fq_key_t key = {0};
	apfs_gen_omap_key_t omap_key = {0};
	apfs_gen_omap_value_t omap_value = {0};
	apfs_gen_node_t node = { .count = UINT32_MAX };
	const unsigned char data[16] = {0};
	apfs_gen_record_t records[] = {
		{ .key = data, .key_size = 16, .value = data, .value_size = 8 },
		{ .key = data, .key_size = 16, .value = data, .value_size = 8 },
		{ .key = data, .key_size = 16, .value = data, .value_size = 8 }
	};

	scenario = "generator refuses insufficient buffers without writing";
	REJECT_WRITE(encoded, apfs_gen_object_build(encoded, 31, &object));
	REJECT_WRITE(encoded, apfs_gen_object_build(encoded, 33, &object));
	REJECT_WRITE(encoded, apfs_gen_nx_build(encoded, 1407, &nx));
	REJECT_WRITE(encoded, apfs_gen_omap_build(encoded, 87, &omap));
	REJECT_WRITE(encoded, apfs_gen_spaceman_build(encoded, 2519,
	    &spaceman));
	REJECT_WRITE(encoded, apfs_gen_reaper_build(encoded, 111, &reaper));
	REJECT_WRITE(encoded, apfs_gen_checkpoint_build(encoded,
	    sizeof(encoded), &checkpoint));
	REJECT_WRITE(encoded, apfs_gen_chunks_build(encoded, sizeof(encoded),
	    &chunks));
	REJECT_WRITE(encoded, apfs_gen_addresses_build(encoded, sizeof(encoded),
	    &addresses));
	REJECT_WRITE(encoded, apfs_gen_fq_key_build(encoded, 15, &key));
	REJECT_WRITE(encoded, apfs_gen_omap_key_build(encoded, 15, &omap_key));
	REJECT_WRITE(encoded, apfs_gen_omap_value_build(encoded, 15,
	    &omap_value));
	REJECT_WRITE(encoded, apfs_gen_fq_value_build(encoded, 7, 1));
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, sizeof(encoded),
	    &node));
	REJECT_WRITE(encoded, apfs_gen_seal(encoded, 31));

	scenario = "reaper state exceeds the object";
	reaper.state_size = 3985;
	REJECT_WRITE(encoded, apfs_gen_reaper_build(encoded, sizeof(encoded),
	    &reaper));

	scenario = "node packer validates all records before writing";
	node.count = 3;
	node.records = records;
	/* Only records 0 and 1 fit the 48 bytes after the header and TOC. */
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, 128, &node));

	scenario = "root node has no room for a footer";
	node.flags = GEN_APFS_ROOT;
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, 92, &node));

	scenario = "TOC capacity is smaller than three entries";
	node.toc_capacity = 16;
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, sizeof(encoded),
	    &node));

	scenario = "last record is missing its key bytes";
	node.toc_capacity = 0;
	records[2].key = NULL;
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, sizeof(encoded),
	    &node));

	scenario = "last record is missing its value bytes";
	records[2].key = data;
	records[2].value = NULL;
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, sizeof(encoded),
	    &node));

	scenario = "fixed records disagree on key length";
	records[2].value = data;
	node.flags = GEN_APFS_FIXED;
	records[2].key_size = 15;
	REJECT_WRITE(encoded, apfs_gen_node_build(encoded, sizeof(encoded),
	    &node));
}

static void
test_omap_records(void)
{
	unsigned char raw_key[16], raw_value[16], encoded[16];
	const apfs_gen_omap_key_t key_params = {
		.oid = UINT64_C(0x123456789abcdef0),
		.xid = UINT64_C(0xfedcba9876543210)
	};
	const apfs_gen_omap_value_t value_params = {
		.flags = 0x81234567, .size = 0xfedcba98,
		.address = UINT64_C(0xabcdef9876543210)
	};
	apfs_omap_key_t key;
	apfs_omap_val_t value;

	scenario = "object map records preserve every bit";
	expect(apfs_gen_omap_key_build(raw_key, sizeof(raw_key), &key_params),
	    "object map key builder");
	expect(apfs_gen_omap_value_build(raw_value, sizeof(raw_value),
	    &value_params), "object map value builder");
	expect(apfs_omap_key_decode(raw_key, sizeof(raw_key), &key),
	    "object map key decode");
	expect(key.oid == UINT64_C(0x123456789abcdef0) &&
	    key.xid == UINT64_C(0xfedcba9876543210), "object map key fields");
	expect(apfs_omap_val_decode(raw_value, sizeof(raw_value), &value),
	    "object map value decode");
	expect(value.flags == 0x81234567 && value.size == 0xfedcba98 &&
	    value.address == UINT64_C(0xabcdef9876543210),
	    "object map value fields are independent of driver policy");
	expect(apfs_omap_key_encode(encoded, 16, &key),
	    "omap key encode");
	expect(memcmp(raw_key, encoded, sizeof(encoded)) == 0,
	    "object map key bytes");
	expect(apfs_omap_val_encode(encoded, 16, &value),
	    "omap val encode");
	expect(memcmp(raw_value, encoded, sizeof(encoded)) == 0,
	    "object map value bytes");
}

/* Guard both sides of each codec-owned span, including other block fields. */
static void
test_patterned_encodes(void)
{
	unsigned char raw[4096], encoded[4112], expected[4112];
	apfs_obj_t object;
	apfs_spaceman_t spaceman;
	apfs_cpm_block_t checkpoint;
	apfs_cpm_t mapping;
	size_t start, end;

	for (size_t i = 0; i < sizeof(native_blocks) / sizeof(native_blocks[0]);
	    i++) {
		if (native_blocks[i].number != 0 &&
		    native_blocks[i].number != 3 &&
		    native_blocks[i].number != 11)
			continue;
		expand_native(&native_blocks[i], raw);
		memset(encoded, 0xa5, sizeof(encoded));
		memset(expected, 0xa5, sizeof(expected));
		switch (native_blocks[i].number) {
		case 0:
			scenario = "object header overwrites patterned bytes";
			start = 0;
			end = 32;
			expect(apfs_obj_decode(raw, sizeof(raw), &object),
			    "decode expected object fields");
			expect(apfs_obj_encode(encoded + 8, sizeof(raw),
			    &object), "encode object into pattern");
			/* Clear the checksum before sealing. */
			bzero(raw, 8);
			break;
		case 3:
			scenario = "mapping clears reserved word in pattern";
			start = 40;
			end = 80;
			expect(apfs_cpm_block_decode(raw, sizeof(raw),
			    &checkpoint), "decode expected mapping header");
			expect(apfs_cpm_decode(raw, sizeof(raw), &checkpoint, 0,
			    &mapping), "decode expected mapping fields");
			expect(apfs_cpm_encode(encoded + 8, sizeof(raw),
			    &checkpoint, 0, &mapping),
			    "encode mapping into pattern");
			break;
		case 11:
			scenario = "spaceman clears reserved bytes in pattern";
			start = 32;
			end = 2520;
			expect(apfs_spaceman_decode(raw, sizeof(raw),
			    &spaceman), "decode expected spaceman fields");
			expect(apfs_spaceman_encode(encoded + 8, sizeof(raw),
			    &spaceman), "encode spaceman into pattern");
			break;
		default:
			abort();
		}
		bcopy(raw + start, expected + 8 + start, end - start);
		expect(memcmp(encoded, expected, sizeof(encoded)) == 0,
		    "codec span matches native bytes; guard bytes remain");
	}
}


/*
 * Each filesystem-record check compares fields against an independent fixture,
 * then encodes over a pattern. Bad lengths must leave both destinations intact.
 * A private copy provides a real extra byte for the overlong-input checks.
 */
static void
check_fs_dstream(const unsigned char *bytes, size_t length,
    const apfs_dstream_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_dstream_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_dstream_decode(raw, length, &got), "dstream decode");
	expect(got.size == want->size, "dstream size");
	expect(got.alloced_size == want->alloced_size, "dstream alloced_size");
	expect(got.default_crypto_id == want->default_crypto_id, "dstream default_crypto_id");
	expect(got.total_bytes_written == want->total_bytes_written, "dstream total_bytes_written");
	expect(got.total_bytes_read == want->total_bytes_read, "dstream total_bytes_read");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_dstream_encode(encoded, length, &got),
	    "dstream encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "dstream byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_dstream_t, output,
		    apfs_dstream_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_dstream_encode(encoded, bad, &got));
	}
}

static void
check_fs_dir_stats(const unsigned char *bytes, size_t length,
    const apfs_dir_stats_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_dir_stats_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_dir_stats_decode(raw, length, &got), "dir_stats decode");
	expect(got.num_children == want->num_children, "dir_stats num_children");
	expect(got.total_size == want->total_size, "dir_stats total_size");
	expect(got.chained_key == want->chained_key, "dir_stats chained_key");
	expect(got.gen_count == want->gen_count, "dir_stats gen_count");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_dir_stats_encode(encoded, length, &got),
	    "dir_stats encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "dir_stats byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_dir_stats_t, output,
		    apfs_dir_stats_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_dir_stats_encode(encoded, bad, &got));
	}
}

static void
check_fs_inode_val(const unsigned char *bytes, size_t length,
    const apfs_inode_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_inode_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_inode_val_decode(raw, length, &got), "inode_val decode");
	expect(got.parent == want->parent, "inode_val parent");
	expect(got.private_id == want->private_id, "inode_val private_id");
	expect(got.create_time == want->create_time, "inode_val create_time");
	expect(got.mod_time == want->mod_time, "inode_val mod_time");
	expect(got.change_time == want->change_time, "inode_val change_time");
	expect(got.access_time == want->access_time, "inode_val access_time");
	expect(got.internal_flags == want->internal_flags, "inode_val internal_flags");
	expect(got.uncompressed_size == want->uncompressed_size, "inode_val uncompressed_size");
	expect(got.count == want->count, "inode_val count");
	expect(got.protection_class == want->protection_class, "inode_val protection_class");
	expect(got.write_generation == want->write_generation, "inode_val write_generation");
	expect(got.bsd_flags == want->bsd_flags, "inode_val bsd_flags");
	expect(got.owner == want->owner, "inode_val owner");
	expect(got.group == want->group, "inode_val group");
	expect(got.mode == want->mode, "inode_val mode");
	expect(got.xfields.length == want->xfields.length &&
	    got.xfields.count == want->xfields.count &&
	    got.xfields.used == want->xfields.used, "inode_val xfield framing");
	if (want->xfields.length != 0)
		expect(memcmp(got.xfields.raw, want->xfields.raw,
		    want->xfields.length) == 0, "inode_val xfield bytes");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_inode_val_encode(encoded, length, &got),
	    "inode_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "inode_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_inode_val_t, output,
		    apfs_inode_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_inode_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_drec_key(const unsigned char *bytes, size_t length,
    const apfs_drec_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_drec_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_drec_key_decode(raw, length, want->hashed, &got), "drec_key decode");
	expect(got.parent == want->parent, "drec_key parent");
	expect(got.hash == want->hash, "drec_key hash");
	expect(got.name_len == want->name_len, "drec_key name_len");
	expect(got.hashed == want->hashed, "drec_key hashed");
	expect(memcmp(got.name, want->name, want->name_len) == 0,
	    "drec_key name bytes exclude stored NUL");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_drec_key_encode(encoded, length, &got),
	    "drec_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "drec_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_drec_key_t, output,
		    apfs_drec_key_decode(raw, bad, want->hashed, &output));
		REJECT_WRITE(encoded, apfs_drec_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_drec_key_t, output,
	    apfs_drec_key_decode(raw, length, want->hashed, &output));
}

static void
check_fs_drec_val(const unsigned char *bytes, size_t length,
    const apfs_drec_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_drec_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_drec_val_decode(raw, length, &got), "drec_val decode");
	expect(got.child == want->child, "drec_val child");
	expect(got.date_added == want->date_added, "drec_val date_added");
	expect(got.flags == want->flags, "drec_val flags");
	expect(got.xfields.length == want->xfields.length &&
	    got.xfields.count == want->xfields.count &&
	    got.xfields.used == want->xfields.used, "drec_val xfield framing");
	if (want->xfields.length != 0)
		expect(memcmp(got.xfields.raw, want->xfields.raw,
		    want->xfields.length) == 0, "drec_val xfield bytes");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_drec_val_encode(encoded, length, &got),
	    "drec_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "drec_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_drec_val_t, output,
		    apfs_drec_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_drec_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_xattr_key(const unsigned char *bytes, size_t length,
    const apfs_name_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_name_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_xattr_key_decode(raw, length, &got), "xattr_key decode");
	expect(got.oid == want->oid, "xattr_key oid");
	expect(got.name_len == want->name_len, "xattr_key name_len");
	expect(memcmp(got.name, want->name, want->name_len) == 0,
	    "xattr_key name bytes exclude stored NUL");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_xattr_key_encode(encoded, length, &got),
	    "xattr_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "xattr_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_name_key_t, output,
		    apfs_xattr_key_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_xattr_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_name_key_t, output,
	    apfs_xattr_key_decode(raw, length, &output));
}

static void
check_fs_snap_name_key(const unsigned char *bytes, size_t length,
    const apfs_name_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_name_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_snap_name_key_decode(raw, length, &got), "snap_name_key decode");
	expect(got.oid == want->oid, "snap_name_key oid");
	expect(got.name_len == want->name_len, "snap_name_key name_len");
	expect(memcmp(got.name, want->name, want->name_len) == 0,
	    "snap_name_key name bytes exclude stored NUL");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_snap_name_key_encode(encoded, length, &got),
	    "snap_name_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "snap_name_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_name_key_t, output,
		    apfs_snap_name_key_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_snap_name_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_name_key_t, output,
	    apfs_snap_name_key_decode(raw, length, &output));
}

static void
check_fs_xattr_val(const unsigned char *bytes, size_t length,
    const apfs_xattr_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_xattr_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_xattr_val_decode(raw, length, &got), "xattr_val decode");
	expect(got.flags == want->flags, "xattr_val flags");
	expect(got.data_len == want->data_len, "xattr_val data_len");
	if ((want->flags & 1) != 0) {
		expect(got.stream_oid == want->stream_oid, "xattr stream OID");
		expect(got.data == NULL, "stream attribute has no inline span");
		expect(got.dstream.size == want->dstream.size,
		    "stream attribute size");
		expect(got.dstream.alloced_size == want->dstream.alloced_size,
		    "stream attribute alloced_size");
		expect(got.dstream.default_crypto_id == want->dstream.default_crypto_id,
		    "stream attribute default_crypto_id");
		expect(got.dstream.total_bytes_written == want->dstream.total_bytes_written,
		    "stream attribute total_bytes_written");
		expect(got.dstream.total_bytes_read == want->dstream.total_bytes_read,
		    "stream attribute total_bytes_read");
	} else
		expect(memcmp(got.data, want->data, want->data_len) == 0,
		    "inline attribute bytes");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_xattr_val_encode(encoded, length, &got),
	    "xattr_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "xattr_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_xattr_val_t, output,
		    apfs_xattr_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_xattr_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_sibling_key(const unsigned char *bytes, size_t length,
    const apfs_sibling_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_sibling_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_sibling_key_decode(raw, length, &got), "sibling_key decode");
	expect(got.inode == want->inode, "sibling_key inode");
	expect(got.sibling_id == want->sibling_id, "sibling_key sibling_id");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_sibling_key_encode(encoded, length, &got),
	    "sibling_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "sibling_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_sibling_key_t, output,
		    apfs_sibling_key_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_sibling_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_sibling_key_t, output,
	    apfs_sibling_key_decode(raw, length, &output));
}

static void
check_fs_sibling_val(const unsigned char *bytes, size_t length,
    const apfs_sibling_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_sibling_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_sibling_val_decode(raw, length, &got), "sibling_val decode");
	expect(got.parent == want->parent, "sibling_val parent");
	expect(got.name_len == want->name_len, "sibling_val name_len");
	expect(memcmp(got.name, want->name, want->name_len) == 0,
	    "sibling_val name bytes exclude stored NUL");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_sibling_val_encode(encoded, length, &got),
	    "sibling_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "sibling_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_sibling_val_t, output,
		    apfs_sibling_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_sibling_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_extent_key(const unsigned char *bytes, size_t length,
    const apfs_extent_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_extent_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_extent_key_decode(raw, length, &got), "extent_key decode");
	expect(got.private_id == want->private_id, "extent_key private_id");
	expect(got.logical == want->logical, "extent_key logical");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_extent_key_encode(encoded, length, &got),
	    "extent_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "extent_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_extent_key_t, output,
		    apfs_extent_key_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_extent_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_extent_key_t, output,
	    apfs_extent_key_decode(raw, length, &output));
}

static void
check_fs_extent_val(const unsigned char *bytes, size_t length,
    const apfs_extent_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_extent_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_extent_val_decode(raw, length, &got), "extent_val decode");
	expect(got.length == want->length, "extent_val length");
	expect(got.phys_block == want->phys_block, "extent_val phys_block");
	expect(got.crypto_id == want->crypto_id, "extent_val crypto_id");
	expect(got.flags == want->flags, "extent_val flags");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_extent_val_encode(encoded, length, &got),
	    "extent_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "extent_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_extent_val_t, output,
		    apfs_extent_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_extent_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_pext_val(const unsigned char *bytes, size_t length,
    const apfs_pext_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_pext_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_pext_val_decode(raw, length, &got), "pext_val decode");
	expect(got.length == want->length, "pext_val length");
	expect(got.owner == want->owner, "pext_val owner");
	expect(got.refcnt == want->refcnt, "pext_val refcnt");
	expect(got.kind == want->kind, "pext_val kind");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_pext_val_encode(encoded, length, &got),
	    "pext_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "pext_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_pext_val_t, output,
		    apfs_pext_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_pext_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_snap_meta_val(const unsigned char *bytes, size_t length,
    const apfs_snap_meta_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_snap_meta_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_snap_meta_val_decode(raw, length, &got), "snap_meta_val decode");
	expect(got.extentref_tree_oid == want->extentref_tree_oid, "snap_meta_val extentref_tree_oid");
	expect(got.sblock_oid == want->sblock_oid, "snap_meta_val sblock_oid");
	expect(got.create_time == want->create_time, "snap_meta_val create_time");
	expect(got.change_time == want->change_time, "snap_meta_val change_time");
	expect(got.inum == want->inum, "snap_meta_val inum");
	expect(got.extentref_tree_type == want->extentref_tree_type, "snap_meta_val extentref_tree_type");
	expect(got.flags == want->flags, "snap_meta_val flags");
	expect(got.name_len == want->name_len, "snap_meta_val name_len");
	expect(memcmp(got.name, want->name, want->name_len) == 0,
	    "snap_meta_val name bytes exclude stored NUL");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_snap_meta_val_encode(encoded, length, &got),
	    "snap_meta_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "snap_meta_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_snap_meta_val_t, output,
		    apfs_snap_meta_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_snap_meta_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_file_info_key(const unsigned char *bytes, size_t length,
    const apfs_file_info_key_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_file_info_key_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_file_info_key_decode(raw, length, &got), "file_info_key decode");
	expect(got.private_id == want->private_id, "file_info_key private_id");
	expect(got.lba == want->lba, "file_info_key lba");
	expect(got.type == want->type, "file_info_key type");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_file_info_key_encode(encoded, length, &got),
	    "file_info_key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "file_info_key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_file_info_key_t, output,
		    apfs_file_info_key_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_file_info_key_encode(encoded, bad, &got));
	}
	raw[7] &= 0x0f;
	REJECT_OUTPUT(apfs_file_info_key_t, output,
	    apfs_file_info_key_decode(raw, length, &output));
}

static void
check_fs_file_info_val(const unsigned char *bytes, size_t length,
    const apfs_file_info_val_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_file_info_val_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_file_info_val_decode(raw, length, &got), "file_info_val decode");
	expect(got.hashed_len == want->hashed_len, "file_info_val hashed_len");
	expect(got.hash_size == want->hash_size, "file_info_val hash_size");
	expect(memcmp(got.hash, want->hash, want->hash_size) == 0,
	    "file information hash bytes");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_file_info_val_encode(encoded, length, &got),
	    "file_info_val encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "file_info_val byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(apfs_file_info_val_t, output,
		    apfs_file_info_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_file_info_val_encode(encoded, bad, &got));
	}
}

static void
check_fs_jkey(const unsigned char *bytes, size_t length,
    const apfs_jkey_t *want)
{
	unsigned char raw[1024], encoded[1024];
	apfs_jkey_t got;

	expect(length + 1 <= sizeof(raw), "fixture fits record buffer");
	bzero(raw, sizeof(raw));
	bcopy(bytes, raw, length);
	expect(apfs_jkey_decode(raw, length, &got), "jkey decode");
	expect(got.oid == want->oid, "jkey oid");
	expect(got.kind == want->kind, "jkey kind");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_jkey_encode(encoded, length, &got),
	    "jkey encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "jkey byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "encoder preserves trailing guard");
	REJECT_WRITE(encoded, apfs_jkey_encode(encoded, length - 1, &got));
	REJECT_OUTPUT(apfs_jkey_t, output,
	    apfs_jkey_decode(raw, 7, &output));
	expect(apfs_jkey_decode(raw, length + 1, &got),
	    "key header accepts a suffix");
}

static void
check_fs_bare_key(const unsigned char *bytes, size_t length,
    const apfs_jkey_t *want)
{
	unsigned char raw[9] = {0}, encoded[9];
	const size_t bad_lengths[] = { 7, 9 };
	uint64_t oid;

	expect(length == 8, "native bare key is exactly eight bytes");
	bcopy(bytes, raw, length);
	expect(apfs_bare_key_decode(raw, length, want->kind, &oid),
	    "bare key decodes with expected kind");
	expect(oid == want->oid, "bare key OID agrees with native dump");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_bare_key_encode(encoded, length, want->kind, oid),
	    "bare key encode");
	expect(memcmp(encoded, bytes, length) == 0,
	    "bare key byte roundtrip over pattern");
	expect(encoded[length] == 0xa5, "bare key preserves trailing guard");
	for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
		REJECT_OUTPUT(uint64_t, output,
		    apfs_bare_key_decode(raw, bad_lengths[i], want->kind, &output));
		REJECT_WRITE(encoded, apfs_bare_key_encode(encoded,
		    bad_lengths[i], want->kind, oid));
	}
	REJECT_OUTPUT(uint64_t, output,
	    apfs_bare_key_decode(raw, length, want->kind ^ 1, &output));
}

static void
check_fs_sibling_map_val(const unsigned char *bytes, size_t length, uint64_t want)
{
	unsigned char raw[16] = {0}, encoded[16];
	uint64_t got;

	bcopy(bytes, raw, length);
	expect(apfs_sibling_map_val_decode(raw, length, &got), "sibling_map_val decode");
	expect(got == want, "sibling_map_val value");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_sibling_map_val_encode(encoded, length, got), "sibling_map_val encode");
	expect(memcmp(bytes, encoded, length) == 0, "sibling_map_val byte roundtrip");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(uint64_t, output,
		    apfs_sibling_map_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_sibling_map_val_encode(encoded, bad, got));
	}
}

static void
check_fs_dstream_id_val(const unsigned char *bytes, size_t length, uint32_t want)
{
	unsigned char raw[16] = {0}, encoded[16];
	uint32_t got;

	bcopy(bytes, raw, length);
	expect(apfs_dstream_id_val_decode(raw, length, &got), "dstream_id_val decode");
	expect(got == want, "dstream_id_val value");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_dstream_id_val_encode(encoded, length, got), "dstream_id_val encode");
	expect(memcmp(bytes, encoded, length) == 0, "dstream_id_val byte roundtrip");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(uint32_t, output,
		    apfs_dstream_id_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_dstream_id_val_encode(encoded, bad, got));
	}
}

static void
check_fs_snap_name_val(const unsigned char *bytes, size_t length, uint64_t want)
{
	unsigned char raw[16] = {0}, encoded[16];
	uint64_t got;

	bcopy(bytes, raw, length);
	expect(apfs_snap_name_val_decode(raw, length, &got), "snap_name_val decode");
	expect(got == want, "snap_name_val value");
	memset(encoded, 0xa5, sizeof(encoded));
	expect(apfs_snap_name_val_encode(encoded, length, got), "snap_name_val encode");
	expect(memcmp(bytes, encoded, length) == 0, "snap_name_val byte roundtrip");
	for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
		REJECT_OUTPUT(uint64_t, output,
		    apfs_snap_name_val_decode(raw, bad, &output));
		REJECT_WRITE(encoded, apfs_snap_name_val_encode(encoded, bad, got));
	}
}

/* Walk and lookup must expose the same unpadded data and field metadata. */
static void
check_fs_xfields(const unsigned char *bytes, size_t length,
    const apfs_xfield_t *want, uint16_t count, uint16_t used)
{
	unsigned char raw[1024] = {0}, encoded[1024];
	apfs_xfields_t blob;
	apfs_xfield_t field, found;
	size_t size;

	expect(length + 1 <= sizeof(raw), "blob fits fixture buffer");
	if (length != 0)
		bcopy(bytes, raw, length);
	expect(apfs_xfields_decode(raw, length, &blob), "xfield blob decode");
	expect(blob.length == length && blob.count == count && blob.used == used,
	    "xfield count, padded data count and length");
	for (uint16_t i = 0; i < count; i++) {
		expect(apfs_xfield_get(&blob, i, &field), "walk extended field");
		expect(apfs_xfield_find(&blob, want[i].type, &found),
		    "find extended field by type");
		expect(field.type == want[i].type && field.flags == want[i].flags &&
		    field.size == want[i].size &&
		    memcmp(field.data, want[i].data, field.size) == 0,
		    "extended field metadata and unpadded data");
		expect(found.type == field.type && found.flags == field.flags &&
		    found.size == field.size && found.data == field.data,
		    "lookup agrees with walk");
	}
	expect(apfs_xfields_size(want, count, &size), "extended field size");
	/* Zero fields may be represented by an empty span or a four-byte blob. */
	if (length != 0) {
		expect(size == length, "extended field computed size");
		memset(encoded, 0xa5, sizeof(encoded));
		expect(apfs_xfields_encode(encoded, length, want, count),
		    "extended field encode");
		expect(memcmp(raw, encoded, length) == 0,
		    "extended field bytes and zero padding overwrite pattern");
		for (size_t bad = length - 1; bad <= length + 1; bad += 2) {
			REJECT_OUTPUT(apfs_xfields_t, output,
			    apfs_xfields_decode(raw, bad, &output));
			REJECT_WRITE(encoded,
			    apfs_xfields_encode(encoded, bad, want, count));
		}
	}
	REJECT_OUTPUT(apfs_xfield_t, output,
	    apfs_xfield_get(&blob, count, &output));
	REJECT_OUTPUT(apfs_xfield_t, output,
	    apfs_xfield_find(&blob, 255, &output));
}

static void
test_native_fs_records(void)
{
	{
		/* apfs-newfs-268435456-e3-ci, node block 1007, record 2. */
		static const unsigned char key[] = {
			0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30
		};
		static const unsigned char value[] = {
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xeb, 0x6b, 0xde, 0x2b, 0xc8, 0xd7, 0xe7, 0x18,
			0x32, 0x4c, 0xae, 0x5e, 0xc8, 0xd7, 0xe7, 0x18,
			0x32, 0x4c, 0xae, 0x5e, 0xc8, 0xd7, 0xe7, 0x18,
			0x22, 0x6e, 0xc4, 0x68, 0xc8, 0xd7, 0xe7, 0x18,
			0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xf5, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
			0xed, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
			0x04, 0x02, 0x05, 0x00, 0x72, 0x6f, 0x6f, 0x74,
			0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char xf_data_0[] = {
			0x72, 0x6f, 0x6f, 0x74, 0x00
		};
		const apfs_xfield_t fields[] = {
			{ .data = xf_data_0, .size = 5,
			  .type = 4, .flags = 2 },
		};
		const apfs_inode_val_t want = {
		    .parent = 1, .private_id = 2,
		    .create_time = UINT64_C(1794640231259859947),
		    .mod_time = UINT64_C(1794640232112344114),
		    .change_time = UINT64_C(1794640232112344114),
		    .access_time = UINT64_C(1794640232281566754),
		    .internal_flags = 32768, .uncompressed_size = 0,
		    .count = 11, .protection_class = 0,
		    .write_generation = 12, .bsd_flags = 0,
		    .owner = 501, .group = 20, .mode = 16877,
		    .xfields = { .raw = value + 92, .length = 16, .count = 1, .used = 8 } };
		const apfs_jkey_t header = { .oid = 2, .kind = 3 };
		apfs_inode_val_t inode;
		apfs_xfield_t field;
		const char *name;
		uint16_t name_len;

		scenario = "native root_inode: block 1007, record 2";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_inode_val(value, sizeof(value), &want);
		check_fs_xfields(value + 92, sizeof(value) - 92,
		    fields, 1, 8);
		expect(apfs_inode_val_decode(value, sizeof(value), &inode),
		    "root inode decode");
		expect(apfs_xfield_find(&inode.xfields, 4, &field),
		    "root inode has a name field");
		expect(apfs_xfield_name(&field, &name, &name_len),
		    "root inode name decode");
		expect(name == (const char *)field.data && name_len == 4 &&
		    memcmp(name, "root", name_len) == 0,
		    "root name borrows field data and excludes terminal NUL");
	}
	{
		/* apfs-newfs-268435456-e3-ci, node block 1007, record 22. */
		static const unsigned char key[] = {
			0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30
		};
		static const unsigned char value[] = {
			0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xd0, 0x61, 0xa4, 0x3c, 0xc8, 0xd7, 0xe7, 0x18,
			0x20, 0x75, 0x06, 0x5c, 0xcb, 0xd7, 0xe7, 0x18,
			0xeb, 0x36, 0x07, 0x5c, 0xcb, 0xd7, 0xe7, 0x18,
			0x20, 0x75, 0x06, 0x5c, 0xcb, 0xd7, 0xe7, 0x18,
			0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x63, 0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0x00,
			0x80, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x38, 0x00,
			0x04, 0x02, 0x0f, 0x00, 0x08, 0x20, 0x28, 0x00,
			0x66, 0x73, 0x65, 0x76, 0x65, 0x6e, 0x74, 0x73,
			0x64, 0x2d, 0x75, 0x75, 0x69, 0x64, 0x00, 0x00,
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char xf_data_0[] = {
			0x66, 0x73, 0x65, 0x76, 0x65, 0x6e, 0x74, 0x73,
			0x64, 0x2d, 0x75, 0x75, 0x69, 0x64, 0x00
		};
		static const unsigned char xf_data_1[] = {
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_xfield_t fields[] = {
			{ .data = xf_data_0, .size = 15,
			  .type = 4, .flags = 2 },
			{ .data = xf_data_1, .size = 40,
			  .type = 8, .flags = 32 },
		};
		const apfs_inode_val_t want = {
		    .parent = 16, .private_id = 17,
		    .create_time = UINT64_C(1794640231541268944),
		    .mod_time = UINT64_C(1794640244952692000),
		    .change_time = UINT64_C(1794640244952741611),
		    .access_time = UINT64_C(1794640244952692000),
		    .internal_flags = 32768, .uncompressed_size = 0,
		    .count = 1, .protection_class = 0,
		    .write_generation = 2, .bsd_flags = 0,
		    .owner = 99, .group = 99, .mode = 33152,
		    .xfields = { .raw = value + 92, .length = 68, .count = 2, .used = 56 } };
		const apfs_jkey_t header = { .oid = 17, .kind = 3 };
		apfs_xfields_t blob;
		apfs_xfield_t field;

		scenario = "native file_inode: block 1007, record 22";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_inode_val(value, sizeof(value), &want);
		check_fs_xfields(value + 92, sizeof(value) - 92,
		    fields, 2, 56);
		expect(apfs_xfields_decode(value + 92, sizeof(value) - 92,
		    &blob), "native scalar blob");
		expect(apfs_xfield_find(&blob, 8, &field),
		    "native dstream field");
	{
			const apfs_dstream_t stream = {
			    .size = 36, .alloced_size = 4096,
			    .default_crypto_id = 0, .total_bytes_written = 36,
			    .total_bytes_read = 36 };

			check_fs_dstream(field.data, field.size, &stream);
	}
	}
	{
		/* apfs-newfs-67108864-e6-sparse, node block 239, record 22. */
		static const unsigned char key[] = {
			0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30
		};
		static const unsigned char value[] = {
			0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x86, 0x9b, 0x0f, 0x02, 0xbb, 0xd7, 0xe7, 0x18,
			0xc0, 0x69, 0x9d, 0x06, 0xbb, 0xd7, 0xe7, 0x18,
			0xc0, 0x69, 0x9d, 0x06, 0xbb, 0xd7, 0xe7, 0x18,
			0x38, 0xa7, 0xb3, 0x25, 0xbb, 0xd7, 0xe7, 0x18,
			0x00, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x63, 0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0x00,
			0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x38, 0x00,
			0x04, 0x02, 0x06, 0x00, 0x08, 0x20, 0x28, 0x00,
			0x0d, 0x28, 0x08, 0x00, 0x73, 0x74, 0x61, 0x72,
			0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char xf_data_0[] = {
			0x73, 0x74, 0x61, 0x72, 0x74, 0x00
		};
		static const unsigned char xf_data_1[] = {
			0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char xf_data_2[] = {
			0x00, 0xf0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_xfield_t fields[] = {
			{ .data = xf_data_0, .size = 6,
			  .type = 4, .flags = 2 },
			{ .data = xf_data_1, .size = 40,
			  .type = 8, .flags = 32 },
			{ .data = xf_data_2, .size = 8,
			  .type = 13, .flags = 40 },
		};
		const apfs_inode_val_t want = {
		    .parent = 18, .private_id = 19,
		    .create_time = UINT64_C(1794640174723865478),
		    .mod_time = UINT64_C(1794640174800267712),
		    .change_time = UINT64_C(1794640174800267712),
		    .access_time = UINT64_C(1794640175321818936),
		    .internal_flags = 33280, .uncompressed_size = 0,
		    .count = 1, .protection_class = 0,
		    .write_generation = 3, .bsd_flags = 0,
		    .owner = 99, .group = 99, .mode = 33188,
		    .xfields = { .raw = value + 92, .length = 72, .count = 3, .used = 56 } };
		const apfs_jkey_t header = { .oid = 19, .kind = 3 };
		apfs_xfields_t blob;
		apfs_xfield_t field;
		uint64_t scalar;

		scenario = "native sparse_inode: block 239, record 22";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_inode_val(value, sizeof(value), &want);
		check_fs_xfields(value + 92, sizeof(value) - 92,
		    fields, 3, 56);
		expect(apfs_xfields_decode(value + 92, sizeof(value) - 92,
		    &blob), "native scalar blob");
		expect(apfs_xfield_find(&blob, 8, &field),
		    "native dstream field");
	{
			const apfs_dstream_t stream = {
			    .size = 1048576, .alloced_size = 1048576,
			    .default_crypto_id = 0, .total_bytes_written = 4096,
			    .total_bytes_read = 1048576 };

			check_fs_dstream(field.data, field.size, &stream);
	}
		expect(apfs_xfield_find(&blob, 13, &field),
		    "native sparse_bytes field");
		expect(apfs_xfield_u64(&field, &scalar) &&
		    scalar == 1044480, "native sparse_bytes scalar");
	}
	{
		/* apfs-newfs-67108864-e7-compression, node block 4269, record 52. */
		static const unsigned char key[] = {
			0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30
		};
		static const unsigned char value[] = {
			0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xd8, 0x58, 0x36, 0x56, 0xed, 0xd7, 0xe7, 0x18,
			0xd8, 0x58, 0x36, 0x56, 0xed, 0xd7, 0xe7, 0x18,
			0xa7, 0x7f, 0xe1, 0xbe, 0xfd, 0xd7, 0xe7, 0x18,
			0xda, 0xcd, 0x2f, 0x2b, 0xef, 0xd7, 0xe7, 0x18,
			0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xa2, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
			0x63, 0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0x00,
			0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
			0x04, 0x02, 0x05, 0x00, 0x63, 0x31, 0x30, 0x6d,
			0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char xf_data_0[] = {
			0x63, 0x31, 0x30, 0x6d, 0x00
		};
		const apfs_xfield_t fields[] = {
			{ .data = xf_data_0, .size = 5,
			  .type = 4, .flags = 2 },
		};
		const apfs_inode_val_t want = {
		    .parent = 18, .private_id = 23,
		    .create_time = UINT64_C(1794640390884055256),
		    .mod_time = UINT64_C(1794640390884055256),
		    .change_time = UINT64_C(1794640461359579047),
		    .access_time = UINT64_C(1794640398752140762),
		    .internal_flags = 278528,
		    .uncompressed_size = 10485760, .count = 1,
		    .protection_class = 0, .write_generation = 162,
		    .bsd_flags = 32, .owner = 99, .group = 99,
		    .mode = 33188, .xfields = { .raw = value + 92, .length = 16,
		    .count = 1, .used = 8 } };
		const apfs_jkey_t header = { .oid = 23, .kind = 3 };

		scenario = "native compressed_inode: block 4269, record 52";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_inode_val(value, sizeof(value), &want);
		check_fs_xfields(value + 92, sizeof(value) - 92,
		    fields, 1, 8);
	}
	{
		/* apfs-newfs-268435456-e3-ci, node block 1007, record 0. */
		static const unsigned char key[] = {
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
			0x0c, 0x8c, 0xa6, 0xac, 0x70, 0x72, 0x69, 0x76,
			0x61, 0x74, 0x65, 0x2d, 0x64, 0x69, 0x72, 0x00
		};
		static const unsigned char value[] = {
			0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x15, 0x7d, 0xde, 0x2b, 0xc8, 0xd7, 0xe7, 0x18,
			0x04, 0x00
		};
		const apfs_drec_val_t want = {
		    .child = 3, .date_added = UINT64_C(1794640231259864341),
		    .flags = 4, .xfields = { .raw = value + 18, .length = 0,
		    .count = 0, .used = 0 } };
		const apfs_jkey_t header = { .oid = 1, .kind = 9 };
		const apfs_drec_key_t key_want = {
		    .parent = 1, .hash = 2828707, .hashed = true,
		    .name_len = 11, .name = "private-dir" };

		scenario = "native hashed_drec: block 1007, record 0";
		check_fs_jkey(key, 8, &header);
		check_fs_drec_key(key, sizeof(key), &key_want);
		check_fs_drec_val(value, sizeof(value), &want);
		check_fs_xfields(value + 18, sizeof(value) - 18,
		    NULL, 0, 0);
	}
	{
		/* apfs-newfs-67108864-e5-links, node block 238, record 25. */
		static const unsigned char key[] = {
			0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
			0x02, 0x98, 0xb4, 0x7b, 0x66, 0x00
		};
		static const unsigned char value[] = {
			0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xa3, 0xc8, 0x76, 0x69, 0xb9, 0xd7, 0xe7, 0x18,
			0x08, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x02,
			0x08, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00
		};
		static const unsigned char xf_data_0[] = {
			0x2b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_xfield_t fields[] = {
			{ .data = xf_data_0, .size = 8,
			  .type = 1, .flags = 2 },
		};
		const apfs_drec_val_t want = {
		    .child = 42, .date_added = UINT64_C(1794640167868745891),
		    .flags = 8, .xfields = { .raw = value + 18, .length = 16,
		    .count = 1, .used = 8 } };
		const apfs_jkey_t header = { .oid = 19, .kind = 9 };
		const apfs_drec_key_t key_want = {
		    .parent = 19, .hash = 2026790, .hashed = true,
		    .name_len = 1, .name = "f" };
		apfs_xfields_t blob;
		apfs_xfield_t field;
		uint64_t scalar;

		scenario = "native sibling_drec: block 238, record 25";
		check_fs_jkey(key, 8, &header);
		check_fs_drec_key(key, sizeof(key), &key_want);
		check_fs_drec_val(value, sizeof(value), &want);
		check_fs_xfields(value + 18, sizeof(value) - 18,
		    fields, 1, 8);
		expect(apfs_xfields_decode(value + 18, sizeof(value) - 18,
		    &blob), "native scalar blob");
		expect(apfs_xfield_find(&blob, 1, &field),
		    "native sibling_id field");
		expect(apfs_xfield_u64(&field, &scalar) &&
		    scalar == 43, "native sibling_id scalar");
	}
	{
		/* apfs-newfs-67108864-e7-compression, node block 4269, record 54. */
		static const unsigned char key[] = {
			0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
			0x12, 0x00, 0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x70,
			0x70, 0x6c, 0x65, 0x2e, 0x64, 0x65, 0x63, 0x6d,
			0x70, 0x66, 0x73, 0x00
		};
		static const unsigned char value[] = {
			0x02, 0x00, 0x10, 0x00, 0x66, 0x70, 0x6d, 0x63,
			0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char inline_data[] = {
			0x66, 0x70, 0x6d, 0x63, 0x08, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_xattr_val_t want = {
		    .flags = 2, .data_len = 16,
		    .stream_oid = 0, .data = inline_data };
		const apfs_jkey_t header = { .oid = 23, .kind = 4 };
		const apfs_name_key_t key_want = {
		    .oid = 23, .name_len = 17,
		    .name = "com.apple.decmpfs" };

		scenario = "native inline_xattr: block 4269, record 54";
		check_fs_jkey(key, 8, &header);
		check_fs_xattr_key(key, sizeof(key), &key_want);
		check_fs_xattr_val(value, sizeof(value), &want);
	}
	{
		/* apfs-newfs-67108864-e5-xattr, node block 1048, record 18. */
		static const unsigned char key[] = {
			0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
			0x0e, 0x00, 0x75, 0x73, 0x65, 0x72, 0x2e, 0x73,
			0x31, 0x30, 0x34, 0x38, 0x35, 0x37, 0x36, 0x00
		};
		static const unsigned char value[] = {
			0x11, 0x00, 0x30, 0x00, 0x23, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		const apfs_xattr_val_t want = {
		    .flags = 17, .data_len = 48,
		    .stream_oid = 35, .dstream = { .size = 1048576,
		    .alloced_size = 1048576, .default_crypto_id = 0,
		    .total_bytes_written = 0, .total_bytes_read = 0 } };
		const apfs_jkey_t header = { .oid = 18, .kind = 4 };
		const apfs_name_key_t key_want = {
		    .oid = 18, .name_len = 13, .name = "user.s1048576" };

		scenario = "native stream_xattr: block 1048, record 18";
		check_fs_jkey(key, 8, &header);
		check_fs_xattr_key(key, sizeof(key), &key_want);
		check_fs_xattr_val(value, sizeof(value), &want);
	}
	{
		/* apfs-newfs-67108864-e5-links, node block 235, record 22. */
		static const unsigned char key[] = {
			0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
			0x2b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char value[] = {
			0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x66, 0x00
		};
		const apfs_sibling_val_t want = {
		    .parent = 19, .name_len = 1, .name = "f" };
		const apfs_jkey_t header = { .oid = 42, .kind = 5 };
		const apfs_sibling_key_t key_want = {
		    .inode = 42, .sibling_id = 43 };

		scenario = "native sibling_link: block 235, record 22";
		check_fs_jkey(key, 8, &header);
		check_fs_sibling_key(key, sizeof(key), &key_want);
		check_fs_sibling_val(value, sizeof(value), &want);
	}
	{
		/* apfs-newfs-67108864-e5-links, node block 235, record 26. */
		static const unsigned char key[] = {
			0x2b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0
		};
		static const unsigned char value[] = {
			0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_jkey_t header = { .oid = 43, .kind = 12 };

		scenario = "native sibling_map: block 235, record 26";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_sibling_map_val(value, sizeof(value), 42);
	}
	{
		/* apfs-newfs-268435456-e3-ci, node block 1007, record 23. */
		static const unsigned char key[] = {
			0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60
		};
		static const unsigned char value[] = {
			0x01, 0x00, 0x00, 0x00
		};
		const apfs_jkey_t header = { .oid = 17, .kind = 6 };

		scenario = "native dstream_id: block 1007, record 23";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_dstream_id_val(value, sizeof(value), 1);
	}
	{
		/* apfs-newfs-67108864-e6-sparse, node block 239, record 24. */
		static const unsigned char key[] = {
			0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char value[] = {
			0x00, 0xf0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_extent_val_t want = {
		    .length = 1044480, .flags = 0,
		    .phys_block = 0, .crypto_id = 0 };
		const apfs_jkey_t header = { .oid = 19, .kind = 8 };
		const apfs_extent_key_t key_want = {
		    .private_id = 19, .logical = 0 };

		scenario = "native hole_extent: block 239, record 24";
		check_fs_jkey(key, 8, &header);
		check_fs_extent_key(key, sizeof(key), &key_want);
		check_fs_extent_val(value, sizeof(value), &want);
	}
	{
		/* apfs-newfs-268435456-e3-ci, node block 1007, record 24. */
		static const unsigned char key[] = {
			0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static const unsigned char value[] = {
			0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		const apfs_extent_val_t want = {
		    .length = 4096, .flags = 0,
		    .phys_block = 376, .crypto_id = 0 };
		const apfs_jkey_t header = { .oid = 17, .kind = 8 };
		const apfs_extent_key_t key_want = {
		    .private_id = 17, .logical = 0 };

		scenario = "native mapped_extent: block 1007, record 24";
		check_fs_jkey(key, 8, &header);
		check_fs_extent_key(key, sizeof(key), &key_want);
		check_fs_extent_val(value, sizeof(value), &want);
	}
	{
		/* apfs-newfs-67108864-e6-clone, node block 337, record 4. */
		static const unsigned char key[] = {
			0xec, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20
		};
		static const unsigned char value[] = {
			0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
			0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00
		};
		const apfs_pext_val_t want = {
		    .length = 15, .kind = 1, .owner = 23,
		    .refcnt = 2 };
		const apfs_jkey_t header = { .oid = 236, .kind = 2 };

		scenario = "native shared_pext: block 337, record 4";
		check_fs_jkey(key, 8, &header);
		check_fs_bare_key(key, sizeof(key), &header);
		check_fs_pext_val(value, sizeof(value), &want);
	}
}

/* Typed destinations let one prefix walker check every output byte. */
typedef union fs_record_output {
	apfs_jkey_t jkey;
	uint64_t bare_key;
	apfs_xfields_t xfields;
	apfs_dstream_t dstream;
	apfs_dir_stats_t dir_stats;
	apfs_inode_val_t inode_val;
	apfs_drec_key_t drec_key;
	apfs_drec_val_t drec_val;
	apfs_name_key_t xattr_key;
	apfs_xattr_val_t xattr_val;
	apfs_sibling_key_t sibling_key;
	apfs_sibling_val_t sibling_val;
	uint64_t sibling_map_val;
	uint32_t dstream_id_val;
	apfs_extent_key_t extent_key;
	apfs_extent_val_t extent_val;
	apfs_pext_val_t pext_val;
	apfs_snap_meta_val_t snap_meta_val;
	apfs_name_key_t snap_name_key;
	uint64_t snap_name_val;
	apfs_file_info_key_t file_info_key;
	apfs_file_info_val_t file_info_val;
} fs_record_output_t;

typedef struct fs_record_prefix_case {
	const char *name;
	const unsigned char *bytes;
	size_t length;
	bool (*decode)(const unsigned char *, size_t, fs_record_output_t *);
} fs_record_prefix_case_t;

static bool
decode_fs_jkey(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_jkey_decode(raw, length, &output->jkey));
}

static bool
decode_fs_bare_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_bare_key_decode(raw, length, 3, &output->bare_key));
}

static bool
decode_fs_xfields(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_xfields_decode(raw, length, &output->xfields));
}

static bool
decode_fs_dstream(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_dstream_decode(raw, length, &output->dstream));
}

static bool
decode_fs_dir_stats(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_dir_stats_decode(raw, length, &output->dir_stats));
}

static bool
decode_fs_inode_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_inode_val_decode(raw, length, &output->inode_val));
}

static bool
decode_fs_drec_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_drec_key_decode(raw, length, false, &output->drec_key));
}

static bool
decode_fs_drec_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_drec_val_decode(raw, length, &output->drec_val));
}

static bool
decode_fs_xattr_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_xattr_key_decode(raw, length, &output->xattr_key));
}

static bool
decode_fs_xattr_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_xattr_val_decode(raw, length, &output->xattr_val));
}

static bool
decode_fs_sibling_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_sibling_key_decode(raw, length, &output->sibling_key));
}

static bool
decode_fs_sibling_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_sibling_val_decode(raw, length, &output->sibling_val));
}

static bool
decode_fs_sibling_map_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_sibling_map_val_decode(raw, length, &output->sibling_map_val));
}

static bool
decode_fs_dstream_id_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_dstream_id_val_decode(raw, length, &output->dstream_id_val));
}

static bool
decode_fs_extent_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_extent_key_decode(raw, length, &output->extent_key));
}

static bool
decode_fs_extent_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_extent_val_decode(raw, length, &output->extent_val));
}

static bool
decode_fs_pext_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_pext_val_decode(raw, length, &output->pext_val));
}

static bool
decode_fs_snap_meta_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_snap_meta_val_decode(raw, length, &output->snap_meta_val));
}

static bool
decode_fs_snap_name_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_snap_name_key_decode(raw, length, &output->snap_name_key));
}

static bool
decode_fs_snap_name_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_snap_name_val_decode(raw, length, &output->snap_name_val));
}

static bool
decode_fs_file_info_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_file_info_key_decode(raw, length, &output->file_info_key));
}

static bool
decode_fs_file_info_val(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_file_info_val_decode(raw, length, &output->file_info_val));
}

static bool
decode_fs_drec_hashed_key(const unsigned char *raw, size_t length,
    fs_record_output_t *output)
{
	return (apfs_drec_key_decode(raw, length, true, &output->drec_key));
}

/*
 * Each allocation ends at the supplied span so ASan sees a read beyond it.
 * malloc(0) supplies the zero-length case without providing a readable byte.
 */
static void
check_fs_record_prefixes(const fs_record_prefix_case_t *test)
{
	fs_record_output_t output, saved;
	unsigned char *prefix;
	char description[160];

	scenario = test->name;
	expect(test->decode(test->bytes, test->length, &output),
	    "complete fixture decodes before checking its prefixes");
	for (size_t length = 0; length < test->length; length++) {
		(void)snprintf(description, sizeof(description), "%s: prefix %zu/%zu",
		    test->name, length, test->length);
		scenario = description;
		prefix = malloc(length);
		expect(prefix != NULL || length == 0, "allocate exact prefix span");
		if (length != 0)
			bcopy(test->bytes, prefix, length);
		memset(&output, 0xa5, sizeof(output));
		bcopy(&output, &saved, sizeof(output));
		/* A zero-length xfield span is the documented absent blob. */
		if (test->decode == decode_fs_xfields && length == 0) {
			expect(test->decode(prefix, length, &output),
			    "zero-length xfield prefix decodes as an absent blob");
			expect(output.xfields.raw == NULL &&
			    output.xfields.length == 0 && output.xfields.count == 0 &&
			    output.xfields.used == 0, "absent blob has no fields or span");
		} else {
			expect(!test->decode(prefix, length, &output),
			    "truncated prefix is refused");
			expect(memcmp(&output, &saved, sizeof(output)) == 0,
			    "truncated prefix preserves complete output");
		}
		free(prefix);
	}
	scenario = test->name;
}

static void
test_fs_record_prefixes(void)
{
	/* Minimal complete records avoid optional blobs forming valid prefixes. */
	static const unsigned char jkey[8] = { [0] = 2, [7] = 0x30 };
	static const unsigned char xfields[16] = {
		[0] = 1, [2] = 8, [4] = 4, [6] = 3, [8] = 'a'
	};
	static const unsigned char dstream[40] = { 1 };
	static const unsigned char dir_stats[32] = { 2 };
	static const unsigned char inode[92] = { 3 };
	static const unsigned char drec[18] = { 4 };
	static const unsigned char drec_key[14] = {
		[0] = 2, [7] = 0x90, [8] = 4, [10] = 'a',
		[11] = 'b', [12] = 'c'
	};
	static const unsigned char hashed_key[16] = {
		[0] = 2, [7] = 0x90, [8] = 4, [9] = 4, [12] = 'a',
		[13] = 'b', [14] = 'c'
	};
	static const unsigned char xattr_key[14] = {
		[0] = 2, [7] = 0x40, [8] = 4, [10] = 'a',
		[11] = 'b', [12] = 'c'
	};
	static const unsigned char inline_xattr[7] = {
		2, 0, 3, 0, 'a', 'b', 'c'
	};
	static const unsigned char stream_xattr[52] = { 1, 0, 48 };
	static const unsigned char sibling_key[16] = { [0] = 2, [7] = 0x50 };
	static const unsigned char sibling_val[14] = {
		[0] = 2, [8] = 4, [10] = 'a', [11] = 'b', [12] = 'c'
	};
	static const unsigned char u64[8] = { 5 };
	static const unsigned char u32[4] = { 6 };
	static const unsigned char extent_key[16] = { [0] = 2, [7] = 0x80 };
	static const unsigned char extent_val[24] = { 7 };
	static const unsigned char pext[20] = { 8 };
	static const unsigned char snap_meta[55] = {
		[0] = 2, [48] = 5, [50] = 's', [51] = 'n',
		[52] = 'a', [53] = 'p'
	};
	static const unsigned char snap_name[15] = {
		[0] = 2, [7] = 0xb0, [8] = 5, [10] = 's', [11] = 'n',
		[12] = 'a', [13] = 'p'
	};
	static const unsigned char info_key[16] = { [0] = 2, [7] = 0xd0 };
	static const unsigned char info_val[7] = { 1, 0, 4, 1, 2, 3, 4 };
	const fs_record_prefix_case_t cases[] = {
		{ .name = "jkey", .bytes = jkey,
		    .length = sizeof(jkey), .decode = decode_fs_jkey },
		{ .name = "bare_key", .bytes = jkey,
		    .length = sizeof(jkey), .decode = decode_fs_bare_key },
		{ .name = "xfields", .bytes = xfields,
		    .length = sizeof(xfields), .decode = decode_fs_xfields },
		{ .name = "dstream", .bytes = dstream,
		    .length = sizeof(dstream), .decode = decode_fs_dstream },
		{ .name = "dir_stats", .bytes = dir_stats,
		    .length = sizeof(dir_stats), .decode = decode_fs_dir_stats },
		{ .name = "inode_val", .bytes = inode,
		    .length = sizeof(inode), .decode = decode_fs_inode_val },
		{ .name = "drec_key", .bytes = drec_key,
		    .length = sizeof(drec_key), .decode = decode_fs_drec_key },
		{ .name = "drec_hashed_key", .bytes = hashed_key,
		    .length = sizeof(hashed_key), .decode = decode_fs_drec_hashed_key },
		{ .name = "drec_val", .bytes = drec,
		    .length = sizeof(drec), .decode = decode_fs_drec_val },
		{ .name = "xattr_key", .bytes = xattr_key,
		    .length = sizeof(xattr_key), .decode = decode_fs_xattr_key },
		{ .name = "xattr_val_inline", .bytes = inline_xattr,
		    .length = sizeof(inline_xattr), .decode = decode_fs_xattr_val },
		{ .name = "xattr_val_stream", .bytes = stream_xattr,
		    .length = sizeof(stream_xattr), .decode = decode_fs_xattr_val },
		{ .name = "sibling_key", .bytes = sibling_key,
		    .length = sizeof(sibling_key), .decode = decode_fs_sibling_key },
		{ .name = "sibling_val", .bytes = sibling_val,
		    .length = sizeof(sibling_val), .decode = decode_fs_sibling_val },
		{ .name = "sibling_map_val", .bytes = u64,
		    .length = sizeof(u64), .decode = decode_fs_sibling_map_val },
		{ .name = "dstream_id_val", .bytes = u32,
		    .length = sizeof(u32), .decode = decode_fs_dstream_id_val },
		{ .name = "extent_key", .bytes = extent_key,
		    .length = sizeof(extent_key), .decode = decode_fs_extent_key },
		{ .name = "extent_val", .bytes = extent_val,
		    .length = sizeof(extent_val), .decode = decode_fs_extent_val },
		{ .name = "pext_val", .bytes = pext,
		    .length = sizeof(pext), .decode = decode_fs_pext_val },
		{ .name = "snap_meta_val", .bytes = snap_meta,
		    .length = sizeof(snap_meta), .decode = decode_fs_snap_meta_val },
		{ .name = "snap_name_key", .bytes = snap_name,
		    .length = sizeof(snap_name), .decode = decode_fs_snap_name_key },
		{ .name = "snap_name_val", .bytes = u64,
		    .length = sizeof(u64), .decode = decode_fs_snap_name_val },
		{ .name = "file_info_key", .bytes = info_key,
		    .length = sizeof(info_key), .decode = decode_fs_file_info_key },
		{ .name = "file_info_val", .bytes = info_val,
		    .length = sizeof(info_val), .decode = decode_fs_file_info_val },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		check_fs_record_prefixes(&cases[i]);
}

static void
test_generated_fs_records(void)
{
	unsigned char raw[1024], blob[84], stream_raw[40];
	static const unsigned char name[] = { 'a', 'b', 0 };
	static const unsigned char u64[] = { 8, 7, 6, 5, 4, 3, 2, 0x81 };
	static const unsigned char u32[] = { 4, 3, 2, 0xf1 };
	const apfs_gen_dstream_t stream = {
		.size = UINT64_C(0xfedcba9876543210), .alloced_size = 4096,
		.default_crypto_id = 19, .total_bytes_written = 123,
		.total_bytes_read = 456
	};
	const apfs_gen_xfield_t gen_fields[] = {
		{ .data = name, .size = 3, .type = 4, .flags = 2 },
		{ .data = u64, .size = 8, .type = 13, .flags = 0x28 },
		{ .data = u32, .size = 4, .type = 14, .flags = 0x10 },
		{ .data = stream_raw, .size = 40, .type = 8, .flags = 0x20 }
	};
	const apfs_xfield_t fields[] = {
		{ .data = name, .size = 3, .type = 4, .flags = 2 },
		{ .data = u64, .size = 8, .type = 13, .flags = 0x28 },
		{ .data = u32, .size = 4, .type = 14, .flags = 0x10 },
		{ .data = stream_raw, .size = 40, .type = 8, .flags = 0x20 }
	};
	apfs_xfields_t decoded_blob;
	apfs_xfield_t field;
	uint64_t decoded_u64;
	uint32_t decoded_u32;

	scenario = "generated extended fields: padded data excludes headers";
	expect(apfs_gen_dstream_build(stream_raw, sizeof(stream_raw), &stream),
	    "data stream builder");
	expect(apfs_gen_xfields_build(blob, sizeof(blob), gen_fields, 4),
	    "extended field builder");
	bzero(raw, sizeof(raw));
	bcopy(blob, raw, sizeof(blob));
	check_fs_xfields(raw, sizeof(blob), fields, 4, 64);
	expect(apfs_xfields_decode(blob, sizeof(blob), &decoded_blob),
	    "generated scalar blob");
	expect(apfs_xfield_find(&decoded_blob, 13, &field), "find u64 field");
	expect(apfs_xfield_u64(&field, &decoded_u64) &&
	    decoded_u64 == UINT64_C(0x8102030405060708), "u64 field endian");
	REJECT_OUTPUT(uint32_t, output, apfs_xfield_u32(&field, &output));
	expect(apfs_xfield_find(&decoded_blob, 14, &field), "find u32 field");
	expect(apfs_xfield_u32(&field, &decoded_u32) &&
	    decoded_u32 == 0xf1020304, "u32 field endian");
	REJECT_OUTPUT(uint64_t, output, apfs_xfield_u64(&field, &output));
	check_fs_xfields(raw, 0, NULL, 0, 0);
	{
		const apfs_dstream_t want = {
		    .size = UINT64_C(0xfedcba9876543210), .alloced_size = 4096,
		    .default_crypto_id = 19, .total_bytes_written = 123,
		    .total_bytes_read = 456 };
		const apfs_gen_dstream_t params = {
		    .size = UINT64_C(0xfedcba9876543210), .alloced_size = 4096,
		    .default_crypto_id = 19, .total_bytes_written = 123,
		    .total_bytes_read = 456 };

		scenario = "generated dstream";
		expect(apfs_gen_dstream_build(raw, 40, &params),
		    "dstream builder");
		check_fs_dstream(raw, 40, &want);
	}
	{
		const apfs_dir_stats_t want = {
		    .num_children = UINT64_C(0x8123456789abcdef),
		    .total_size = UINT64_C(0xfedcba9876543210),
		    .chained_key = 42, .gen_count = 19 };
		const apfs_gen_dir_stats_t params = {
		    .num_children = UINT64_C(0x8123456789abcdef),
		    .total_size = UINT64_C(0xfedcba9876543210),
		    .chained_key = 42, .gen_count = 19 };

		scenario = "generated dir_stats";
		expect(apfs_gen_dir_stats_build(raw, 32, &params),
		    "dir_stats builder");
		check_fs_dir_stats(raw, 32, &want);
	}
	{
		const apfs_inode_val_t want = {
		    .parent = UINT64_C(0x8123456789abcdef),
		    .private_id = UINT64_C(0xfedcba9876543210),
		    .create_time = (-INT64_C(1)),
		    .mod_time = (-INT64_C(9223372036854775807)),
		    .change_time = UINT64_C(0x7fffffffffffffff),
		    .access_time = (-INT64_C(1234567890)),
		    .internal_flags = UINT64_C(0xf1e2d3c4b5a69788),
		    .count = (-INT64_C(2147483647)),
		    .protection_class = 0x87654321u,
		    .write_generation = 0xfedcba98u,
		    .bsd_flags = 0xf1234567u, .owner = 0xf1e2d3c4u,
		    .group = 0x8a9bacbdu, .mode = 64188,
		    .uncompressed_size = UINT64_C(0x8877665544332211),
		    .xfields = { .raw = blob, .length = 84, .count = 4, .used = 64 } };
		const apfs_gen_inode_val_t params = {
		    .parent = UINT64_C(0x8123456789abcdef),
		    .private_id = UINT64_C(0xfedcba9876543210),
		    .create_time = (-INT64_C(1)),
		    .mod_time = (-INT64_C(9223372036854775807)),
		    .change_time = UINT64_C(0x7fffffffffffffff),
		    .access_time = (-INT64_C(1234567890)),
		    .internal_flags = UINT64_C(0xf1e2d3c4b5a69788),
		    .count = (-INT64_C(2147483647)),
		    .protection_class = 0x87654321u,
		    .write_generation = 0xfedcba98u,
		    .bsd_flags = 0xf1234567u, .owner = 0xf1e2d3c4u,
		    .group = 0x8a9bacbdu, .mode = 64188,
		    .uncompressed_size = UINT64_C(0x8877665544332211), .xfields = blob,
		    .xfields_len = 84 };
		unsigned char encoded[184], expected[184];
		apfs_inode_val_t record = want;

		scenario = "generated inode_val";
		expect(apfs_gen_inode_val_build(raw, 176, &params),
		    "inode_val builder");
		check_fs_inode_val(raw, 176, &want);

		scenario = "inode writer encodes xfields in place before record";
		memset(encoded, 0xa5, sizeof(encoded));
		memset(expected, 0xa5, sizeof(expected));
		bcopy(raw, expected, 176);
		expect(apfs_xfields_encode(encoded + 92, sizeof(blob),
		    fields, 4), "encode blob after fixed record part");
		expect(apfs_xfields_decode(encoded + 92, sizeof(blob),
		    &record.xfields), "record borrows its in-place blob");
		expect(apfs_inode_val_encode(encoded, 176, &record),
		    "encode record with blob copied onto itself");
		expect(memcmp(encoded, expected, sizeof(encoded)) == 0,
		    "complete record matches builder and preserves trailing guards");
	}
	{
		const apfs_drec_val_t want = {
		    .child = UINT64_C(0xfedcba9876543210),
		    .date_added = (-INT64_C(1234567890123456789)), .flags = 65244,
		    .xfields = { .raw = blob, .length = 84, .count = 4, .used = 64 } };
		const apfs_gen_drec_val_t params = {
		    .child = UINT64_C(0xfedcba9876543210),
		    .date_added = (-INT64_C(1234567890123456789)), .flags = 65244,
		    .xfields = blob, .xfields_len = 84 };
		unsigned char encoded[110], expected[110];
		apfs_drec_val_t record = want;

		scenario = "generated drec_val";
		expect(apfs_gen_drec_val_build(raw, 102, &params),
		    "drec_val builder");
		check_fs_drec_val(raw, 102, &want);

		scenario = "drec writer encodes xfields in place before record";
		memset(encoded, 0xa5, sizeof(encoded));
		memset(expected, 0xa5, sizeof(expected));
		bcopy(raw, expected, 102);
		expect(apfs_xfields_encode(encoded + 18, sizeof(blob),
		    fields, 4), "encode blob after fixed record part");
		expect(apfs_xfields_decode(encoded + 18, sizeof(blob),
		    &record.xfields), "record borrows its in-place blob");
		expect(apfs_drec_val_encode(encoded, 102, &record),
		    "encode record with blob copied onto itself");
		expect(memcmp(encoded, expected, sizeof(encoded)) == 0,
		    "complete record matches builder and preserves trailing guards");
	}
	{
		const apfs_extent_val_t want = {
		    .length = UINT64_C(0xfedcba98765432), .flags = 169,
		    .phys_block = UINT64_C(0xfedcba9876543210),
		    .crypto_id = UINT64_C(0x87654321abcdef01) };
		const apfs_gen_extent_val_t params = {
		    .length = UINT64_C(0x00fedcba98765432), .flags = 0xa9,
		    .phys_block = UINT64_C(0xfedcba9876543210),
		    .crypto_id = UINT64_C(0x87654321abcdef01) };

		scenario = "generated extent_val";
		expect(apfs_gen_extent_val_build(raw, 24, &params),
		    "extent_val builder");
		check_fs_extent_val(raw, 24, &want);
	}
	{
		const apfs_pext_val_t want = {
		    .length = UINT64_C(0xfedcba987654321), .kind = 15,
		    .owner = UINT64_C(0xfedcba9876543210),
		    .refcnt = (-INT64_C(2147483647)) };
		const apfs_gen_pext_val_t params = {
		    .length = UINT64_C(0x0fedcba987654321), .kind = 15,
		    .owner = UINT64_C(0xfedcba9876543210),
		    .refcnt = (-INT64_C(2147483647)) };

		scenario = "generated pext_val";
		expect(apfs_gen_pext_val_build(raw, 20, &params),
		    "pext_val builder");
		check_fs_pext_val(raw, 20, &want);
	}
	{
		const apfs_snap_meta_val_t want = {
		    .extentref_tree_oid = UINT64_C(0xfedcba9876543210),
		    .sblock_oid = UINT64_C(0x8123456789abcdef),
		    .create_time = (-INT64_C(1234)), .change_time = (-INT64_C(5678)),
		    .inum = UINT64_C(0xf123456789abcdef),
		    .extentref_tree_type = 0x87654321u,
		    .flags = 0xfedcba98u, .name = "snap", .name_len = 4 };
		const apfs_gen_snap_meta_val_t params = {
		    .extentref_tree_oid = UINT64_C(0xfedcba9876543210),
		    .sblock_oid = UINT64_C(0x8123456789abcdef),
		    .create_time = (-INT64_C(1234)), .change_time = (-INT64_C(5678)),
		    .inum = UINT64_C(0xf123456789abcdef),
		    .extentref_tree_type = 0x87654321u,
		    .flags = 0xfedcba98u, .name = "snap", .name_len = 4 };

		scenario = "generated snap_meta_val";
		expect(apfs_gen_snap_meta_val_build(raw, 55, &params),
		    "snap_meta_val builder");
		check_fs_snap_meta_val(raw, 55, &want);
	}
	{
		const apfs_file_info_val_t want = {
		    .hashed_len = 65244, .hash_size = 4, .hash = u32 };
		const apfs_gen_file_info_val_t params = {
		    .hashed_len = 65244, .hash_size = 4, .hash = u32 };

		scenario = "generated file_info_val";
		expect(apfs_gen_file_info_val_build(raw, 7, &params),
		    "file_info_val builder");
		check_fs_file_info_val(raw, 7, &want);
	}
	{
		const apfs_drec_key_t want = {
		    .parent = UINT64_C(0xfedcba987654321), .hash = 0,
		    .name = "file", .name_len = 4, .hashed = false };
		const apfs_gen_drec_key_t params = {
		    .parent = UINT64_C(0xfedcba987654321), .hash = 0,
		    .name = "file", .name_len = 4, .hashed = false };

		scenario = "generated drec_key";
		expect(apfs_gen_drec_key_build(raw, 15, &params),
		    "drec_key builder");
		check_fs_drec_key(raw, 15, &want);
	}
	{
		const apfs_name_key_t want = {
		    .oid = UINT64_C(0xfedcba987654321), .name = "attr",
		    .name_len = 4 };
		const apfs_gen_name_key_t params = {
		    .oid = UINT64_C(0xfedcba987654321), .name = "attr",
		    .name_len = 4 };

		scenario = "generated xattr_key";
		expect(apfs_gen_xattr_key_build(raw, 15, &params),
		    "xattr_key builder");
		check_fs_xattr_key(raw, 15, &want);
	}
	{
		const apfs_name_key_t want = {
		    .oid = UINT64_C(0xfedcba987654321), .name = "snap",
		    .name_len = 4 };
		const apfs_gen_name_key_t params = {
		    .oid = UINT64_C(0xfedcba987654321), .name = "snap",
		    .name_len = 4 };

		scenario = "generated snap_name_key";
		expect(apfs_gen_snap_name_key_build(raw, 15, &params),
		    "snap_name_key builder");
		check_fs_snap_name_key(raw, 15, &want);
	}
	{
		const apfs_sibling_val_t want = {
		    .parent = UINT64_C(0xfedcba9876543210), .name = "link",
		    .name_len = 4 };
		const apfs_gen_sibling_val_t params = {
		    .parent = UINT64_C(0xfedcba9876543210), .name = "link",
		    .name_len = 4 };

		scenario = "generated sibling_val";
		expect(apfs_gen_sibling_val_build(raw, 15, &params),
		    "sibling_val builder");
		check_fs_sibling_val(raw, 15, &want);
	}
	{
		const apfs_sibling_key_t want = {
		    .inode = UINT64_C(0xfedcba987654321),
		    .sibling_id = UINT64_C(0xfedcba9876543210) };

		scenario = "generated sibling_key";
		expect(apfs_gen_sibling_key_build(raw, 16, UINT64_C(0xfedcba987654321),
		    UINT64_C(0xfedcba9876543210)),
		    "sibling_key builder");
		check_fs_sibling_key(raw, 16, &want);
	}
	{
		const apfs_extent_key_t want = {
		    .private_id = UINT64_C(0xfedcba987654321),
		    .logical = UINT64_C(0xfedcba9876543210) };

		scenario = "generated extent_key";
		expect(apfs_gen_extent_key_build(raw, 16, UINT64_C(0xfedcba987654321),
		    UINT64_C(0xfedcba9876543210)),
		    "extent_key builder");
		check_fs_extent_key(raw, 16, &want);
	}
	{
		const apfs_file_info_key_t want = {
		    .private_id = UINT64_C(0xfedcba987654321),
		    .lba = UINT64_C(0xfedcba98765432), .type = 241 };

		scenario = "generated file_info_key";
		expect(apfs_gen_file_info_key_build(raw, 16, UINT64_C(0xfedcba987654321),
		    UINT64_C(0xfedcba98765432), 241),
		    "file_info_key builder");
		check_fs_file_info_key(raw, 16, &want);
	}
	{
		const apfs_jkey_t want = {
		    .oid = UINT64_C(0xfedcba987654321), .kind = 15 };

		scenario = "generated jkey";
		expect(apfs_gen_jkey_build(raw, 8, UINT64_C(0xfedcba987654321), 15),
		    "jkey builder");
		check_fs_jkey(raw, 8, &want);
	}
	{
		const apfs_xattr_val_t want = {
		    .data = u32, .flags = 61442, .data_len = 4,
		    .stream_oid = 0 };
		const apfs_gen_xattr_val_t params = {
		    .data = u32, .flags = 61442, .data_len = 4,
		    .stream_oid = 0 };

		scenario = "generated xattr_val";
		expect(apfs_gen_xattr_val_build(raw, 8, &params),
		    "xattr_val builder");
		check_fs_xattr_val(raw, 8, &want);
	}
	scenario = "generated sibling_map_val";
	expect(apfs_gen_sibling_map_val_build(raw, 8, UINT64_C(0xfedcba9876543210)),
	    "sibling_map_val builder");
	check_fs_sibling_map_val(raw, 8, UINT64_C(0xfedcba9876543210));
	scenario = "generated dstream_id_val";
	expect(apfs_gen_dstream_id_val_build(raw, 4, 0xfedcba98u),
	    "dstream_id_val builder");
	check_fs_dstream_id_val(raw, 4, 0xfedcba98u);
	scenario = "generated snap_name_val";
	expect(apfs_gen_snap_name_val_build(raw, 8, UINT64_C(0xfedcba9876543210)),
	    "snap_name_val builder");
	check_fs_snap_name_val(raw, 8, UINT64_C(0xfedcba9876543210));
	{
		const apfs_gen_inode_val_t params = { .count = 1 };
		const apfs_inode_val_t want = { .count = 1 };

		scenario = "fixed inode has no extended fields";
		expect(apfs_gen_inode_val_build(raw, 92, &params), "inode builder");
		check_fs_inode_val(raw, 92, &want);
	}
	{
		const apfs_gen_drec_val_t params = { .child = 42, .flags = 8 };
		const apfs_drec_val_t want = { .child = 42, .flags = 8 };

		scenario = "fixed directory record has no extended fields";
		expect(apfs_gen_drec_val_build(raw, 18, &params), "drec builder");
		check_fs_drec_val(raw, 18, &want);
	}
	{
		const apfs_gen_drec_key_t params = { .name = "ab", .parent = 2,
		    .name_len = 2, .hashed = true, .hash = 0x3fffff };
		const apfs_drec_key_t want = { .name = "ab", .parent = 2,
		    .name_len = 2, .hashed = true, .hash = 0x3fffff };

		scenario = "generated hashed key uses all 22 hash bits";
		expect(apfs_gen_drec_key_build(raw, 15, &params), "hashed builder");
		check_fs_drec_key(raw, 15, &want);
	}
	{
		const apfs_gen_xattr_val_t params = { .flags = 0x8001,
		    .stream_oid = UINT64_C(0xfedcba9876543210), .data_len = 48,
		    .dstream = stream };
		const apfs_xattr_val_t want = { .flags = 0x8001,
		    .stream_oid = UINT64_C(0xfedcba9876543210), .data_len = 48,
		    .dstream = { .size = UINT64_C(0xfedcba9876543210),
		    .alloced_size = 4096, .default_crypto_id = 19,
		    .total_bytes_written = 123, .total_bytes_read = 456 } };

		scenario = "generated stream attribute retains unrelated flag bits";
		expect(apfs_gen_xattr_val_build(raw, 52, &params), "stream builder");
		check_fs_xattr_val(raw, 52, &want);
	}
}

static void
test_fs_edited_xfield_spans(void)
{
	static const unsigned char data[] = { 1, 2, 3, 4 };
	const apfs_gen_xfield_t fields[] = {
		{ .type = 4, .size = 3, .data = data },
		{ .type = 13, .size = 4, .data = data }
	};
	unsigned char *raw, *shortened;
	apfs_xfields_t blob;

	for (unsigned edit = 0; edit < 2; edit++) {
		scenario = edit == 0 ? "decoded blob count exceeds its span" :
		    "decoded blob span ends inside last field";
		raw = malloc(28);
		expect(raw != NULL, "allocate exact blob span");
		expect(apfs_gen_xfields_build(raw, 28, fields, 2),
		    "build two distinct extended fields");
		expect(apfs_xfields_decode(raw, 28, &blob),
		    "decode complete blob before editing its description");
		if (edit == 0) {
			blob.count = 8;
		} else {
			/* Keep the heap boundary at the edited span for ASan. */
			shortened = realloc(raw, 23);
			expect(shortened != NULL, "shrink allocation to edited span");
			raw = shortened;
			blob.raw = raw;
			blob.length = 23;
		}
		REJECT_OUTPUT(apfs_xfield_t, output,
		    apfs_xfield_get(&blob, blob.count - 1, &output));
		REJECT_OUTPUT(apfs_xfield_t, output,
		    apfs_xfield_find(&blob, 13, &output));
		free(raw);
	}
}

static void
test_fs_xfield_rejections(void)
{
	static const unsigned char data[] = { 1, 2, 3, 4 };
	const apfs_gen_xfield_t fields[] = {
		{ .type = 4, .flags = 2, .size = 3, .data = data },
		{ .type = 4, .flags = 2, .size = 4, .data = data }
	};
	const fs_xfield_failure_t cases[] = {
		{ .name = "headers exceed blob",
		    .length = 16, .offset = 0, .width = 2, .value = 65535 },
		{ .name = "used is smaller than padded data",
		    .length = 16, .offset = 2, .width = 2, .value = 7 },
		{ .name = "used is larger than padded data",
		    .length = 16, .offset = 2, .width = 2, .value = 9 },
		{ .name = "field data exceeds blob",
		    .length = 16, .offset = 6, .width = 2, .value = 17 },
		{ .name = "last field lacks padding",
		    .length = 11, .offset = 2, .width = 2, .value = 8 },
		{ .name = "padding contains a nonzero byte",
		    .length = 16, .offset = 11, .width = 1, .value = 1 },
		{ .name = "one-byte blob",
		    .length = 1, .offset = 0, .width = 1, .value = 0 },
		{ .name = "two-byte blob",
		    .length = 2, .offset = 0, .width = 1, .value = 0 },
		{ .name = "three-byte blob",
		    .length = 3, .offset = 0, .width = 1, .value = 0 }
	};
	unsigned char raw[64], encoded[256], record[256];
	apfs_inode_val_t inode = {0};
	apfs_drec_val_t drec = {0};
	apfs_xfields_t blob;
	apfs_xfield_t huge = { .data = data, .type = 4, .size = 65535 };
	apfs_xfield_t repeated[] = {
		{ .data = data, .type = 4, .size = 3 },
		{ .data = data, .type = 4, .size = 4 }
	};
	const size_t bad_inode_lengths[] = { 92, 107, 109 };

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scenario = cases[i].name;
		expect(apfs_gen_xfields_build(raw, 16, fields, 1), "blob builder");
		put_le(raw + cases[i].offset, cases[i].value, cases[i].width);
		REJECT_OUTPUT(apfs_xfields_t, output,
		    apfs_xfields_decode(raw, cases[i].length, &output));
		/* Record codecs must validate a borrowed blob before copying it. */
		blob = (apfs_xfields_t){ .raw = raw, .length = cases[i].length };
		inode.xfields = blob;
		drec.xfields = blob;
		REJECT_WRITE(encoded, apfs_inode_val_encode(encoded,
		    92 + cases[i].length, &inode));
		REJECT_WRITE(encoded, apfs_drec_val_encode(encoded,
		    18 + cases[i].length, &drec));
		bzero(record, sizeof(record));
		bcopy(raw, record + 92, cases[i].length);
		REJECT_OUTPUT(apfs_inode_val_t, output,
		    apfs_inode_val_decode(record, 92 + cases[i].length, &output));
		bzero(record, sizeof(record));
		bcopy(raw, record + 18, cases[i].length);
		REJECT_OUTPUT(apfs_drec_val_t, output,
		    apfs_drec_val_decode(record, 18 + cases[i].length, &output));
	}
	scenario = "extended-field types repeat";
	expect(apfs_gen_xfields_build(raw, 28, fields, 2), "repeated builder");
	REJECT_OUTPUT(apfs_xfields_t, output,
	    apfs_xfields_decode(raw, 28, &output));
	REJECT_WRITE(encoded, apfs_xfields_encode(encoded, 28, repeated, 2));
	inode.xfields = (apfs_xfields_t){ .raw = raw, .length = 28 };
	drec.xfields = inode.xfields;
	REJECT_WRITE(encoded, apfs_inode_val_encode(encoded, 120, &inode));
	REJECT_WRITE(encoded, apfs_drec_val_encode(encoded, 46, &drec));

	scenario = "extended-field padded data exceeds u16 used count";
	REJECT_OUTPUT(size_t, output, apfs_xfields_size(&huge, 1, &output));
	REJECT_WRITE(encoded, apfs_xfields_encode(encoded, sizeof(encoded),
	    &huge, 1));

	scenario = "inode encoded length must equal fixed part plus blob";
	expect(apfs_gen_xfields_build(raw, 16, fields, 1), "valid blob builder");
	expect(apfs_xfields_decode(raw, 16, &inode.xfields), "valid blob decode");
	for (size_t i = 0;
	    i < sizeof(bad_inode_lengths) / sizeof(bad_inode_lengths[0]); i++) {
		REJECT_WRITE(encoded,
		    apfs_inode_val_encode(encoded, bad_inode_lengths[i], &inode));
	}
}

static void
test_fs_xfield_name_rejections(void)
{
	static const unsigned char data[] = { 'r', 'o', 'o', 't' };
	const uint16_t sizes[] = { sizeof(data), 0 };
	const char *name, *saved_name = "unchanged";
	uint16_t name_len;
	apfs_xfield_t field;

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		scenario = sizes[i] == 0 ? "name field has zero size" :
		    "name field lacks terminal NUL";
		field = (apfs_xfield_t){ .data = data, .size = sizes[i], .type = 4 };
		name = saved_name;
		name_len = 0xa5a5;
		expect(!apfs_xfield_name(&field, &name, &name_len),
		    "malformed name field is refused");
		expect(name == saved_name && name_len == 0xa5a5,
		    "rejected name field preserves both outputs");
	}
}

static void
test_fs_name_rejections(void)
{
	unsigned char raw[128], encoded[1200];
	static const char long_name[1023] = { 'x' };
	const apfs_gen_drec_key_t drec = { .parent = 2, .name = "abc",
	    .name_len = 3 };
	const apfs_gen_drec_key_t hashed = { .parent = 2, .name = "abc",
	    .name_len = 3, .hashed = true, .hash = 0x123456 };
	const apfs_gen_name_key_t name = { .oid = 2, .name = "abc",
	    .name_len = 3 };
	const apfs_gen_sibling_val_t sibling = { .parent = 2, .name = "abc",
	    .name_len = 3 };
	const apfs_gen_snap_meta_val_t snap = { .name = "abc", .name_len = 3 };

	for (unsigned defect = 0; defect < 3; defect++) {
		scenario = defect == 0 ? "stored name has zero length" :
		    defect == 1 ? "stored name lacks terminal NUL" :
		    "stored name length runs past record";
		expect(apfs_gen_drec_key_build(raw, 14, &drec), "plain key builder");
		if (defect == 1)
			raw[13] = 'x';
		else
			put_le(raw + 8, defect == 0 ? 0 : 5, 2);
		REJECT_OUTPUT(apfs_drec_key_t, output,
		    apfs_drec_key_decode(raw, 14, false, &output));
		expect(apfs_gen_drec_key_build(raw, 16, &hashed), "hashed builder");
		if (defect == 1)
			raw[15] = 'x';
		else
			put_le(raw + 8, (0x123456u << 10) |
			    (defect == 0 ? 0 : 5), 4);
		REJECT_OUTPUT(apfs_drec_key_t, output,
		    apfs_drec_key_decode(raw, 16, true, &output));
		expect(apfs_gen_xattr_key_build(raw, 14, &name), "xattr key builder");
		if (defect == 1)
			raw[13] = 'x';
		else
			put_le(raw + 8, defect == 0 ? 0 : 5, 2);
		REJECT_OUTPUT(apfs_name_key_t, output,
		    apfs_xattr_key_decode(raw, 14, &output));
		expect(apfs_gen_snap_name_key_build(raw, 14, &name),
		    "snapshot key builder");
		if (defect == 1)
			raw[13] = 'x';
		else
			put_le(raw + 8, defect == 0 ? 0 : 5, 2);
		REJECT_OUTPUT(apfs_name_key_t, output,
		    apfs_snap_name_key_decode(raw, 14, &output));
		expect(apfs_gen_sibling_val_build(raw, 14, &sibling),
		    "sibling value builder");
		if (defect == 1)
			raw[13] = 'x';
		else
			put_le(raw + 8, defect == 0 ? 0 : 5, 2);
		REJECT_OUTPUT(apfs_sibling_val_t, output,
		    apfs_sibling_val_decode(raw, 14, &output));
		expect(apfs_gen_snap_meta_val_build(raw, 54, &snap),
		    "snapshot value builder");
		if (defect == 1)
			raw[53] = 'x';
		else
			put_le(raw + 48, defect == 0 ? 0 : 5, 2);
		REJECT_OUTPUT(apfs_snap_meta_val_t, output,
		    apfs_snap_meta_val_decode(raw, 54, &output));
	}
	scenario = "hashed length is smaller than the key span";
	expect(apfs_gen_drec_key_build(raw, 16, &hashed), "hashed builder");
	put_le(raw + 8, (0x123456u << 10) | 3, 4);
	/* The shortened name also ends in NUL: only the span check can reject. */
	raw[14] = 0;
	REJECT_OUTPUT(apfs_drec_key_t, output,
	    apfs_drec_key_decode(raw, 16, true, &output));

	/* Encoders receive names without NUL; the stored length includes it. */
	{
		apfs_drec_key_t key = { .parent = 2, .hashed = true,
		    .name = long_name, .name_len = 1023 };

		scenario = "hashed name length plus NUL exceeds ten bits";
		REJECT_WRITE(encoded, apfs_drec_key_encode(encoded, 1036, &key));
		scenario = "directory hash exceeds 22 bits";
		key.name_len = 3;
		key.hash = 0x400000;
		REJECT_WRITE(encoded, apfs_drec_key_encode(encoded, 16, &key));
		scenario = "plain directory key cannot hold a hash";
		key.hashed = false;
		key.hash = 1;
		REJECT_WRITE(encoded, apfs_drec_key_encode(encoded, 14, &key));
	}
}

static void
test_fs_value_rejections(void)
{
	unsigned char raw[128], encoded[128];
	static const unsigned char data[] = { 1, 2, 3, 4 };
	apfs_gen_xattr_val_t params = { .data = data, .data_len = 4 };
	apfs_xattr_val_t value = { .data = data, .data_len = 4 };
	const apfs_gen_file_info_val_t info = { .hash = data,
	    .hash_size = 4, .hashed_len = 4096 };

	for (unsigned flags = 0; flags <= 3; flags += 3) {
		scenario = flags == 0 ? "attribute has neither storage flag" :
		    "attribute has both storage flags";
		params.flags = 2;
		expect(apfs_gen_xattr_val_build(raw, 8, &params), "xattr builder");
		put_le(raw, flags, 2);
		REJECT_OUTPUT(apfs_xattr_val_t, output,
		    apfs_xattr_val_decode(raw, 8, &output));
		value.flags = flags;
		REJECT_WRITE(encoded, apfs_xattr_val_encode(encoded, 8, &value));
	}
	scenario = "inline attribute data length disagrees with value span";
	params.flags = 2;
	expect(apfs_gen_xattr_val_build(raw, 8, &params), "inline builder");
	put_le(raw + 2, 3, 2);
	REJECT_OUTPUT(apfs_xattr_val_t, output,
	    apfs_xattr_val_decode(raw, 8, &output));
	value.flags = 2;
	value.data_len = 3;
	REJECT_WRITE(encoded, apfs_xattr_val_encode(encoded, 8, &value));

	scenario = "stream attribute data length is not 48";
	bzero(raw, sizeof(raw));
	put_le(raw, 1, 2);
	put_le(raw + 2, 47, 2);
	REJECT_OUTPUT(apfs_xattr_val_t, output,
	    apfs_xattr_val_decode(raw, 51, &output));
	value.flags = 1;
	value.data_len = 47;
	REJECT_WRITE(encoded, apfs_xattr_val_encode(encoded, 51, &value));

	scenario = "file information hash size disagrees with value span";
	expect(apfs_gen_file_info_val_build(raw, 7, &info), "file info builder");
	raw[2] = 3;
	REJECT_OUTPUT(apfs_file_info_val_t, output,
	    apfs_file_info_val_decode(raw, 7, &output));
	{
		const apfs_file_info_val_t bad = { .hash = data, .hash_size = 3 };

		REJECT_WRITE(encoded, apfs_file_info_val_encode(encoded, 7, &bad));
	}
}

static void
test_fs_encoder_ranges(void)
{
	unsigned char encoded[128];
	const uint64_t large_oid = UINT64_C(0x1000000000000000);
	const apfs_jkey_t header = { .oid = large_oid, .kind = 3 };
	const apfs_jkey_t kind = { .kind = 16 };
	const apfs_drec_key_t drec = { .parent = large_oid,
	    .name = "a", .name_len = 1 };
	const apfs_name_key_t name = { .oid = large_oid,
	    .name = "a", .name_len = 1 };
	const apfs_sibling_key_t sibling = { .inode = large_oid };
	const apfs_extent_key_t extent = { .private_id = large_oid };
	apfs_file_info_key_t info = { .private_id = large_oid };
	const apfs_extent_val_t length = { .length = UINT64_C(1) << 56 };
	apfs_pext_val_t pext = { .length = UINT64_C(1) << 60 };

	scenario = "key OID exceeds sixty bits";
	REJECT_WRITE(encoded, apfs_bare_key_encode(encoded, 8, 3, large_oid));
	REJECT_WRITE(encoded, apfs_jkey_encode(encoded, 8, &header));
	REJECT_WRITE(encoded, apfs_drec_key_encode(encoded, 12, &drec));
	REJECT_WRITE(encoded, apfs_xattr_key_encode(encoded, 12, &name));
	REJECT_WRITE(encoded, apfs_snap_name_key_encode(encoded, 12, &name));
	REJECT_WRITE(encoded, apfs_sibling_key_encode(encoded, 16, &sibling));
	REJECT_WRITE(encoded, apfs_extent_key_encode(encoded, 16, &extent));
	REJECT_WRITE(encoded, apfs_file_info_key_encode(encoded, 16, &info));
	scenario = "key kind exceeds four bits";
	REJECT_WRITE(encoded, apfs_bare_key_encode(encoded, 8, 16, 2));
	REJECT_WRITE(encoded, apfs_jkey_encode(encoded, 8, &kind));
	scenario = "file extent length exceeds fifty-six bits";
	REJECT_WRITE(encoded, apfs_extent_val_encode(encoded, 24, &length));
	scenario = "physical extent length exceeds sixty bits";
	REJECT_WRITE(encoded, apfs_pext_val_encode(encoded, 20, &pext));
	scenario = "physical extent kind exceeds four bits";
	pext.length = 1;
	pext.kind = 16;
	REJECT_WRITE(encoded, apfs_pext_val_encode(encoded, 20, &pext));
	scenario = "file information LBA exceeds fifty-six bits";
	info.private_id = 2;
	info.lba = UINT64_C(1) << 56;
	REJECT_WRITE(encoded, apfs_file_info_key_encode(encoded, 16, &info));
}

static void
test_volume_superblock_fields_and_roundtrip(void)
{
	unsigned char raw[4096] = {0}, encoded[4096] = {0};
	apfs_gen_fs_t source = { .object = { .oid = 901, .xid = 10,
	    .type = 13 } };
	apfs_fs_t decoded;

	scenario = "volume superblock fields and roundtrip";
	source.magic = 0x12345600u + 1;
	source.index = 0x12345600u + 2;
	source.features = UINT64_C(0x1234567800000000) + 3;
	source.ro_compat_features = UINT64_C(0x1234567800000000) + 4;
	source.incompat_features = UINT64_C(0x1234567800000000) + 5;
	source.unmount_time = UINT64_C(0x1234567800000000) + 6;
	source.reserve_blocks = UINT64_C(0x1234567800000000) + 7;
	source.quota_blocks = UINT64_C(0x1234567800000000) + 8;
	source.alloc_blocks = UINT64_C(0x1234567800000000) + 9;
	source.root_tree_type = 0x12345600u + 10;
	source.extentref_tree_type = 0x12345600u + 11;
	source.snapshot_tree_type = 0x12345600u + 12;
	source.omap_oid = UINT64_C(0x1234567800000000) + 13;
	source.root_tree_oid = UINT64_C(0x1234567800000000) + 14;
	source.extentref_tree_oid = UINT64_C(0x1234567800000000) + 15;
	source.snapshot_tree_oid = UINT64_C(0x1234567800000000) + 16;
	source.revert_to_xid = UINT64_C(0x1234567800000000) + 17;
	source.revert_to_oid = UINT64_C(0x1234567800000000) + 18;
	source.next_obj_id = UINT64_C(0x1234567800000000) + 19;
	source.files = UINT64_C(0x1234567800000000) + 20;
	source.directories = UINT64_C(0x1234567800000000) + 21;
	source.symlinks = UINT64_C(0x1234567800000000) + 22;
	source.other_objects = UINT64_C(0x1234567800000000) + 23;
	source.snapshots = UINT64_C(0x1234567800000000) + 24;
	source.blocks_allocated = UINT64_C(0x1234567800000000) + 25;
	source.blocks_freed = UINT64_C(0x1234567800000000) + 26;
	source.modified_time = UINT64_C(0x1234567800000000) + 27;
	source.flags = UINT64_C(0x1234567800000000) + 28;
	source.next_doc_id = 0x12345600u + 29;
	source.role = 0xabcd;
	source.root_to_xid = UINT64_C(0x1234567800000000) + 31;
	source.rolling_state_oid = UINT64_C(0x1234567800000000) + 32;
	source.clone_epoch = UINT64_C(0x1234567800000000) + 33;
	source.clone_xid = UINT64_C(0x1234567800000000) + 34;
	source.snapshot_ext_oid = UINT64_C(0x1234567800000000) + 35;
	source.integrity_oid = UINT64_C(0x1234567800000000) + 36;
	source.fext_tree_oid = UINT64_C(0x1234567800000000) + 37;
	source.fext_tree_type = 0x12345600u + 38;
	source.reserved_tree_type = 0x12345600u + 39;
	for (unsigned i = 0; i < 20; i++)
		source.crypto[i] = (unsigned char)(i + 20);
	for (unsigned i = 0; i < 16; i++)
		source.uuid[i] = (unsigned char)(i + 16);
	for (unsigned i = 0; i < 256; i++)
		source.name[i] = (unsigned char)(i + 256);
	for (unsigned i = 0; i < 16; i++)
		source.group_uuid[i] = (unsigned char)(i + 80);
	for (unsigned i = 0; i < 32; i++)
		source.formatted_by.id[i] = (unsigned char)(i + 1);
	source.formatted_by.time = UINT64_C(0x123456789abcdef0);
	source.formatted_by.last_xid = 500;
	for (unsigned i = 0; i < 8; i++) {
		for (unsigned j = 0; j < 32; j++)
			source.modified_by[i].id[j] = (unsigned char)(32 * i + j);
		source.modified_by[i].time = UINT64_C(0xfedcba9800000000) + i;
		source.modified_by[i].last_xid = UINT64_C(0x9876543200000000) + i;
	}
	expect(apfs_gen_fs_build(raw, sizeof(raw), &source), "build APSB");
	expect(apfs_fs_decode(raw, sizeof(raw), &decoded), "decode APSB");
	expect(decoded.magic == source.magic,
	    "magic at offset 32");
	expect(decoded.index == source.index,
	    "index at offset 36");
	expect(decoded.features == source.features,
	    "features at offset 40");
	expect(decoded.ro_compat_features == source.ro_compat_features,
	    "ro_compat_features at offset 48");
	expect(decoded.incompat_features == source.incompat_features,
	    "incompat_features at offset 56");
	expect(decoded.unmount_time == source.unmount_time,
	    "unmount_time at offset 64");
	expect(decoded.reserve_blocks == source.reserve_blocks,
	    "reserve_blocks at offset 72");
	expect(decoded.quota_blocks == source.quota_blocks,
	    "quota_blocks at offset 80");
	expect(decoded.alloc_blocks == source.alloc_blocks,
	    "alloc_blocks at offset 88");
	expect(decoded.root_tree_type == source.root_tree_type,
	    "root_tree_type at offset 116");
	expect(decoded.extentref_tree_type == source.extentref_tree_type,
	    "extentref_tree_type at offset 120");
	expect(decoded.snapshot_tree_type == source.snapshot_tree_type,
	    "snapshot_tree_type at offset 124");
	expect(decoded.omap_oid == source.omap_oid,
	    "omap_oid at offset 128");
	expect(decoded.root_tree_oid == source.root_tree_oid,
	    "root_tree_oid at offset 136");
	expect(decoded.extentref_tree_oid == source.extentref_tree_oid,
	    "extentref_tree_oid at offset 144");
	expect(decoded.snapshot_tree_oid == source.snapshot_tree_oid,
	    "snapshot_tree_oid at offset 152");
	expect(decoded.revert_to_xid == source.revert_to_xid,
	    "revert_to_xid at offset 160");
	expect(decoded.revert_to_oid == source.revert_to_oid,
	    "revert_to_oid at offset 168");
	expect(decoded.next_obj_id == source.next_obj_id,
	    "next_obj_id at offset 176");
	expect(decoded.files == source.files,
	    "files at offset 184");
	expect(decoded.directories == source.directories,
	    "directories at offset 192");
	expect(decoded.symlinks == source.symlinks,
	    "symlinks at offset 200");
	expect(decoded.other_objects == source.other_objects,
	    "other_objects at offset 208");
	expect(decoded.snapshots == source.snapshots,
	    "snapshots at offset 216");
	expect(decoded.blocks_allocated == source.blocks_allocated,
	    "blocks_allocated at offset 224");
	expect(decoded.blocks_freed == source.blocks_freed,
	    "blocks_freed at offset 232");
	expect(decoded.modified_time == source.modified_time,
	    "modified_time at offset 256");
	expect(decoded.flags == source.flags,
	    "flags at offset 264");
	expect(decoded.next_doc_id == source.next_doc_id,
	    "next_doc_id at offset 960");
	expect(decoded.role == source.role,
	    "role at offset 964");
	expect(decoded.root_to_xid == source.root_to_xid,
	    "root_to_xid at offset 968");
	expect(decoded.rolling_state_oid == source.rolling_state_oid,
	    "rolling_state_oid at offset 976");
	expect(decoded.clone_epoch == source.clone_epoch,
	    "clone_epoch at offset 984");
	expect(decoded.clone_xid == source.clone_xid,
	    "clone_xid at offset 992");
	expect(decoded.snapshot_ext_oid == source.snapshot_ext_oid,
	    "snapshot_ext_oid at offset 1000");
	expect(decoded.integrity_oid == source.integrity_oid,
	    "integrity_oid at offset 1024");
	expect(decoded.fext_tree_oid == source.fext_tree_oid,
	    "fext_tree_oid at offset 1032");
	expect(decoded.fext_tree_type == source.fext_tree_type,
	    "fext_tree_type at offset 1040");
	expect(decoded.reserved_tree_type == source.reserved_tree_type,
	    "reserved_tree_type at offset 1044");
	expect(memcmp(decoded.crypto, source.crypto, 20) == 0,
	    "crypto at offset 96");
	expect(memcmp(decoded.uuid, source.uuid, 16) == 0,
	    "uuid at offset 240");
	expect(memcmp(decoded.name, source.name, 256) == 0,
	    "name at offset 704");
	expect(memcmp(decoded.group_uuid, source.group_uuid, 16) == 0,
	    "group_uuid at offset 1008");
	expect(memcmp(decoded.formatted_by.id, source.formatted_by.id, 32) == 0 &&
	    decoded.formatted_by.time == source.formatted_by.time &&
	    decoded.formatted_by.last_xid == source.formatted_by.last_xid,
	    "formatting record at offset 272");
	for (unsigned i = 0; i < 8; i++)
		expect(memcmp(decoded.modified_by[i].id,
		    source.modified_by[i].id, 32) == 0 &&
		    decoded.modified_by[i].time == source.modified_by[i].time &&
		    decoded.modified_by[i].last_xid == source.modified_by[i].last_xid,
		    "modification records at offset 320, stride 48");
	encoded[966] = 0xa5;
	encoded[967] = 0x5a;
	expect(apfs_fs_encode(encoded, sizeof(encoded), &decoded), "encode APSB");
	expect(encoded[966] == 0 && encoded[967] == 0,
	    "encoder initializes the reserved field");
	expect(memcmp(raw + 32, encoded + 32, sizeof(raw) - 32) == 0,
	    "encoded payload matches independent encoder including reserved bytes");
	expect(apfs_fs_decode(raw, 1048, &decoded), "exact-length decode accepted");
	expect(apfs_fs_encode(encoded, 1048, &decoded), "exact-length encode accepted");
	for (size_t length = 0; length < 1048; length++) {
		expect(!apfs_fs_decode(raw, length, &decoded), "short decode refused");
		memset(encoded, 0xa5, sizeof(encoded));
		expect(!apfs_fs_encode(encoded, length, &decoded), "short encode refused");
		for (size_t i = 0; i < sizeof(encoded); i++)
			expect(encoded[i] == 0xa5, "short encode leaves buffer untouched");
	}
}

int
main(void)
{
	test_volume_superblock_fields_and_roundtrip();
	test_native_fs_records();
	test_generated_fs_records();
	test_fs_record_prefixes();
	test_fs_edited_xfield_spans();
	test_fs_xfield_rejections();
	test_fs_xfield_name_rejections();
	test_fs_name_rejections();
	test_fs_value_rejections();
	test_fs_encoder_ranges();
	test_native_blocks();
	test_patterned_encodes();
	test_fletcher();
	test_spaceman_decodes_both_layouts_and_encodes_only_current();
	test_truncated_records();
	test_object_lengths();
	test_nx_area_counts();
	test_counts_and_indexes();
	test_node_framing();
	test_entry_spans();
	test_generated_fixed_objects();
	test_generated_arrays();
	test_generated_queue_records();
	test_omap_records();
	test_generated_nodes();
	test_generator_rejections();
	puts("APFS codec tests passed");
	return (0);
}
