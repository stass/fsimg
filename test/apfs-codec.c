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
	    node.area.free_end == 4056 - sizes.val, "absolute node areas");
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
	    entry.val_off == 4056 - sizes.val && entry.val_len == sizes.val &&
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
test_truncated_records(void)
{
	unsigned char raw[4096] = {0}, encoded[4096];
	apfs_obj_t object = {0};
	apfs_nx_t nx = {0};
	apfs_omap_t omap = {0};
	apfs_spaceman_t spaceman = {0};
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
	char name[80];

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

int
main(void)
{
	test_native_blocks();
	test_patterned_encodes();
	test_fletcher();
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
