/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_APFS_H
#define FSIMG_APFS_H

#include "internal.h"

/* APFS (specs/design-apfs.md and specs/design-apfs-driver.md). */

/*
 * Layout constants. Every APFS integer is little-endian. Offsets and sizes
 * are in bytes, block numbers and counts in container blocks of B bytes.
 */

/* Container geometry: the block sizes the reader accepts. */
#define APFS_BLOCK_MIN			4096u
#define APFS_BLOCK_MAX			65536u

/*
 * The object header that starts every object. The type word holds the kind
 * in its low 16 bits and flags in its high 16; the storage class is two of
 * those flags.
 */
#define APFS_OBJ_HEADER_BYTES		32u
#define APFS_OBJ_CHECKSUM_BYTES		8u
#define APFS_OBJ_KIND_MASK		0x0000ffffu
#define APFS_OBJ_FLAGS_MASK		0xffff0000u
#define APFS_OBJ_STORAGE_MASK		0xc0000000u
#define APFS_OBJ_VIRTUAL		0x00000000u
#define APFS_OBJ_EPHEMERAL		0x80000000u
#define APFS_OBJ_PHYSICAL		0x40000000u
#define APFS_OBJ_NOHEADER		0x20000000u
#define APFS_OBJ_ENCRYPTED		0x10000000u
#define APFS_OBJ_NONPERSISTENT		0x08000000u

/* Object kinds, named by role. */
#define APFS_KIND_NX_SUPERBLOCK		1u
#define APFS_KIND_BTREE_ROOT		2u
#define APFS_KIND_BTREE_NODE		3u
#define APFS_KIND_SPACEMAN		5u
#define APFS_KIND_SPACEMAN_CAB		6u	/* chunk-information address */
#define APFS_KIND_SPACEMAN_CIB		7u	/* chunk information */
#define APFS_KIND_SPACEMAN_BITMAP	8u
#define APFS_KIND_OMAP			11u
#define APFS_KIND_CHECKPOINT_MAP	12u
#define APFS_KIND_FS			13u	/* volume superblock */
#define APFS_KIND_NX_REAPER		17u
#define APFS_KIND_NX_REAP_LIST		18u

/* Object subtypes: the tree a node belongs to. */
#define APFS_SUBTYPE_NONE		0u
#define APFS_SUBTYPE_FREE_QUEUE		9u
#define APFS_SUBTYPE_OMAP		11u

/* The container superblock. APFS_NX_BYTES ends its last decoded field. */
#define APFS_NX_MAGIC			0x4253584eu	/* "NXSB" */
#define APFS_NX_BYTES			1408u
#define APFS_NX_VOLUMES_MAX		100u
#define APFS_NX_COUNTERS		32u
#define APFS_NX_EPHEMERAL_INFO		4u
#define APFS_NX_AREA_NONCONTIGUOUS	0x80000000u	/* in an area's count */
#define APFS_NX_AREA_COUNT_MASK		0x7fffffffu
#define APFS_UUID_BYTES			16u

#define APFS_NX_FEATURE_DEFRAG		0x1u	/* optional features */
#define APFS_NX_FEATURE_LCFD		0x2u	/* low-capacity Fusion mode */
#define APFS_NX_INCOMPAT_VERSION1	0x1u
#define APFS_NX_INCOMPAT_VERSION2	0x2u
#define APFS_NX_INCOMPAT_FUSION		0x100u
#define APFS_NX_FLAG_CRYPTO_SW		0x4u

/* A checkpoint-mapping block: flags and count, then 40-byte mappings. */
#define APFS_CPM_HEADER_BYTES		40u
#define APFS_CPM_ENTRY_BYTES		40u
#define APFS_CPM_LAST			0x1u

/* The object-map object, its tree's key and value, and their flags. */
#define APFS_OMAP_BYTES			88u
#define APFS_OMAP_KEY_BYTES		16u
#define APFS_OMAP_VAL_BYTES		16u

#define APFS_OMAP_MANUALLY_MANAGED	0x01u	/* object-map flags */
#define APFS_OMAP_ENCRYPTING		0x02u
#define APFS_OMAP_DECRYPTING		0x04u
#define APFS_OMAP_KEYROLLING		0x08u
#define APFS_OMAP_CRYPTO_GENERATION	0x10u

#define APFS_OMAP_VAL_DELETED		0x01u	/* object-map value flags */
#define APFS_OMAP_VAL_SAVED		0x02u
#define APFS_OMAP_VAL_ENCRYPTED		0x04u
#define APFS_OMAP_VAL_NOHEADER		0x08u
#define APFS_OMAP_VAL_CRYPTO_GENERATION	0x10u

/*
 * B-tree nodes. The node header ends at APFS_NODE_HEADER_BYTES, where the
 * table of contents starts; a root node ends with the information footer.
 * A table-of-contents entry is two u16 offsets in a fixed-size tree and
 * four u16 fields (offset and length each) otherwise.
 */
#define APFS_NODE_HEADER_BYTES		56u
#define APFS_BTREE_INFO_BYTES		40u
#define APFS_TOC_FIXED_BYTES		4u
#define APFS_TOC_VARIABLE_BYTES		8u
#define APFS_TOC_GHOST			0xffffu	/* value offset of a ghost */
#define APFS_BTREE_LEVELS_MAX		16u
#define APFS_CHILD_OID_BYTES		8u	/* an index node's value */

#define APFS_NODE_ROOT			0x0001u	/* node flags */
#define APFS_NODE_LEAF			0x0002u
#define APFS_NODE_FIXED_KV		0x0004u
#define APFS_NODE_HASHED		0x0008u
#define APFS_NODE_NOHEADER		0x0010u
#define APFS_NODE_TRANSIENT		0x8000u

#define APFS_BTREE_UINT64_KEYS		0x001u	/* tree flags, in the footer */
#define APFS_BTREE_SEQUENTIAL_INSERT	0x002u
#define APFS_BTREE_ALLOW_GHOSTS		0x004u
#define APFS_BTREE_EPHEMERAL		0x008u
#define APFS_BTREE_PHYSICAL		0x010u
#define APFS_BTREE_NONPERSISTENT	0x020u
#define APFS_BTREE_KV_NONALIGNED	0x040u
#define APFS_BTREE_HASHED		0x080u
#define APFS_BTREE_NOHEADER		0x100u

/*
 * The space manager. APFS_SM_BYTES ends its fixed structure, through the
 * data-zone state that starts at APFS_SM_DATAZONE_OFFSET; its arrays follow
 * at the offsets its fields name. Device and free-queue indexes select the
 * entries of its two device and three free-queue arrays.
 */
#define APFS_SM_BYTES			2520u
#define APFS_SM_DATAZONE_OFFSET		344u
#define APFS_SM_DEVICE_MAIN		0u
#define APFS_SM_DEVICE_TIER2		1u
#define APFS_SM_DEVICES			2u
#define APFS_SM_FQ_INTERNAL_POOL	0u
#define APFS_SM_FQ_MAIN			1u
#define APFS_SM_FQ_TIER2		2u
#define APFS_SM_FREE_QUEUES		3u
#define APFS_SM_RING_FREE_END		0xffffu	/* ends the free-slot list */

/* Chunk-information and chunk-information address blocks. */
#define APFS_CIB_HEADER_BYTES		40u
#define APFS_CHUNK_INFO_BYTES		32u
#define APFS_CAB_HEADER_BYTES		40u
#define APFS_CAB_ENTRY_BYTES		8u

/* A free queue's key, (XID, first block), and value, a block count. */
#define APFS_FQ_KEY_BYTES		16u
#define APFS_FQ_VAL_BYTES		8u

/* The reaper; its state buffer starts at APFS_REAPER_BYTES. */
#define APFS_REAPER_BYTES		112u
#define APFS_REAPER_FLAG_RESERVED	0x1u	/* always set */
#define APFS_REAPER_FLAG_CONTINUE	0x2u	/* an object is being reaped */

/* A reap-list object; its entries start at APFS_REAP_LIST_BYTES. */
#define APFS_REAP_LIST_BYTES		64u
#define APFS_REAP_LIST_ENTRY_BYTES	40u
#define APFS_REAP_LIST_NONE		0xffffffffu	/* no entry index */

/*
 * Container structures: decoded records holding every field of their fixed
 * structure except reserved bytes. An object's header is decoded on its own
 * (apfs_obj_t); the other records describe the bytes that follow it.
 */

typedef struct apfs_obj {
	uint64_t	checksum;
	uint64_t	oid;
	uint64_t	xid;
	uint32_t	flags;		/* type's high half, with the storage */
	uint32_t	subtype;
	uint16_t	kind;		/* type's low half */
} apfs_obj_t;

/* A block range: a first block and a count, as the superblock stores it. */
typedef struct apfs_range {
	uint64_t	start;
	uint64_t	count;
} apfs_range_t;

/*
 * The container superblock. An area's stored count carries the
 * noncontiguous bit, which is split out into its own field.
 */
typedef struct apfs_nx {
	uint64_t	volume_oids[APFS_NX_VOLUMES_MAX];
	uint64_t	counters[APFS_NX_COUNTERS];
	uint64_t	ephemeral_info[APFS_NX_EPHEMERAL_INFO];
	unsigned char	uuid[APFS_UUID_BYTES];
	unsigned char	fusion_uuid[APFS_UUID_BYTES];
	apfs_range_t	blocked_out;
	apfs_range_t	keylocker;
	apfs_range_t	fusion_wbc;
	apfs_range_t	media_keylocker;
	uint64_t	block_count;
	uint64_t	features;
	uint64_t	ro_compat_features;
	uint64_t	incompat_features;
	uint64_t	next_oid;
	uint64_t	next_xid;
	uint64_t	desc_base;
	uint64_t	data_base;
	uint64_t	spaceman_oid;
	uint64_t	omap_oid;
	uint64_t	reaper_oid;
	uint64_t	evict_mapping_oid;
	uint64_t	flags;
	uint64_t	efi_jumpstart;
	uint64_t	test_oid;
	uint64_t	fusion_mt_oid;
	uint64_t	fusion_wbc_oid;
	uint64_t	newest_mounted_version;
	uint32_t	magic;
	uint32_t	block_size;
	uint32_t	desc_blocks;	/* without the noncontiguous bit */
	uint32_t	data_blocks;
	uint32_t	desc_next;
	uint32_t	data_next;
	uint32_t	desc_index;	/* this checkpoint's descriptor run */
	uint32_t	desc_len;
	uint32_t	data_index;	/* this checkpoint's data run */
	uint32_t	data_len;
	uint32_t	test_type;
	uint32_t	max_volumes;
	bool		desc_noncontiguous;
	bool		data_noncontiguous;
} apfs_nx_t;

/* A checkpoint-mapping block's header. */
typedef struct apfs_cpm_block {
	uint32_t	flags;
	uint32_t	count;
} apfs_cpm_block_t;

/* One checkpoint mapping: an ephemeral object and where it lies. */
typedef struct apfs_cpm {
	uint64_t	volume_oid;	/* zero for a container object */
	uint64_t	oid;
	uint64_t	address;
	uint32_t	flags;		/* as in apfs_obj_t */
	uint32_t	subtype;
	uint32_t	size;		/* bytes */
	uint16_t	kind;
} apfs_cpm_t;

typedef struct apfs_omap {
	uint64_t	tree_oid;
	uint64_t	snapshot_tree_oid;
	uint64_t	most_recent_snapshot;
	uint64_t	pending_revert_min;
	uint64_t	pending_revert_max;
	uint32_t	flags;
	uint32_t	snapshot_count;
	uint32_t	tree_type;
	uint32_t	snapshot_tree_type;
} apfs_omap_t;

typedef struct apfs_omap_key {
	uint64_t	oid;
	uint64_t	xid;
} apfs_omap_key_t;

typedef struct apfs_omap_val {
	uint64_t	address;
	uint32_t	flags;
	uint32_t	size;
} apfs_omap_val_t;

/* A location in a node: a u16 offset and length. */
typedef struct apfs_nloc {
	uint16_t	off;
	uint16_t	len;
} apfs_nloc_t;

/* The information footer that ends a root node. */
typedef struct apfs_btree_info {
	uint64_t	key_count;
	uint64_t	node_count;
	uint32_t	flags;
	uint32_t	node_size;
	uint32_t	key_size;	/* fixed sizes, zero when variable */
	uint32_t	val_size;
	uint32_t	longest_key;
	uint32_t	longest_val;
} apfs_btree_info_t;

/*
 * The areas a node header implies, as offsets from the start of the node:
 * the table of contents is [toc_start, key_start), the key area
 * [key_start, free_start), the free space [free_start, free_end), and the
 * value area [free_end, value_end). value_end is the node's end, or the
 * footer's start in a root node.
 */
typedef struct apfs_node_area {
	uint32_t	toc_start;
	uint32_t	key_start;
	uint32_t	free_start;
	uint32_t	free_end;
	uint32_t	value_end;
} apfs_node_area_t;

/*
 * A node header: its stored fields, the footer of a root node, and the areas
 * the decoder derived from the stored fields. The codec derives the areas
 * again from the stored fields whenever it uses them, so a caller cannot
 * widen a span by editing area.
 */
typedef struct apfs_node {
	apfs_btree_info_t info;		/* root nodes only */
	apfs_node_area_t area;		/* derived; ignored by the encoder */
	apfs_nloc_t	toc;		/* as stored */
	apfs_nloc_t	free;
	apfs_nloc_t	key_free;
	apfs_nloc_t	val_free;
	uint32_t	key_count;
	uint16_t	flags;
	uint16_t	level;
} apfs_node_t;

/*
 * The fixed key and value sizes of a node's records, which a fixed-size
 * table-of-contents entry does not store: the footer's sizes for a leaf, the
 * key size and a child OID (plus any digest) for an index node. The footer
 * stores 32-bit sizes; btree.c refuses one above UINT16_MAX before it
 * narrows it here.
 */
typedef struct apfs_kv_size {
	uint16_t	key;
	uint16_t	val;
} apfs_kv_size_t;

/*
 * A table-of-contents entry as stored. A fixed-size entry stores only the
 * two offsets; its lengths are zero here.
 */
typedef struct apfs_toc {
	apfs_nloc_t	key;	/* offset from the key area's start */
	apfs_nloc_t	val;	/* offset back from the value area's end */
} apfs_toc_t;

/*
 * One table-of-contents entry resolved to spans of the node, as offsets from
 * the node's start. A ghost has no value: val_off and val_len are zero.
 */
typedef struct apfs_entry {
	uint32_t	key_off;
	uint32_t	key_len;
	uint32_t	val_off;
	uint32_t	val_len;
	bool		ghost;
} apfs_entry_t;

/* One space-manager device: the main device or the Fusion tier 2. */
typedef struct apfs_sm_device {
	uint64_t	block_count;
	uint64_t	chunk_count;
	uint64_t	free_count;
	uint32_t	cib_count;
	uint32_t	cab_count;
	uint32_t	addr_offset;	/* its address array, in the object */
} apfs_sm_device_t;

typedef struct apfs_sm_fq {
	uint64_t	count;
	uint64_t	tree_oid;
	uint64_t	oldest_xid;
	uint16_t	node_limit;
} apfs_sm_fq_t;

/*
 * The space manager's fixed structure. The data-zone allocation state is
 * not decoded: nothing reads it, and the encoder writes it as zero, its
 * value in every native capture. The array offsets (addr_offset and the
 * pool ring's three) are returned as stored: the arrays may lie past the
 * first block, so the pager in spaceman.c proves each array's span inside
 * the object before it reads one.
 */
typedef struct apfs_spaceman {
	apfs_sm_device_t devices[APFS_SM_DEVICES];
	apfs_sm_fq_t	free_queues[APFS_SM_FREE_QUEUES];
	uint64_t	ip_block_count;		/* the internal pool */
	uint64_t	ip_bm_base;
	uint64_t	ip_base;
	uint64_t	fs_reserve_block_count;
	uint64_t	fs_reserve_alloc_count;
	uint32_t	block_size;
	uint32_t	blocks_per_chunk;
	uint32_t	chunks_per_cib;
	uint32_t	cibs_per_cab;
	uint32_t	flags;
	uint32_t	ip_bm_tx_multiplier;
	uint32_t	ip_bm_size_in_blocks;
	uint32_t	ip_bm_block_count;
	uint32_t	ip_bm_xid_offset;	/* the pool's bitmap ring */
	uint32_t	ip_bitmap_offset;
	uint32_t	ip_bm_free_next_offset;
	uint32_t	version;
	uint32_t	struct_size;
	uint16_t	ip_bm_free_head;
	uint16_t	ip_bm_free_tail;
} apfs_spaceman_t;

/* The header of a chunk-information or chunk-information address block. */
typedef struct apfs_cib {
	uint32_t	index;
	uint32_t	count;
} apfs_cib_t;

typedef struct apfs_chunk_info {
	uint64_t	xid;
	uint64_t	address;	/* first block of the chunk */
	uint64_t	bitmap_address;	/* zero for a chunk with no bitmap */
	uint32_t	block_count;
	uint32_t	free_count;
} apfs_chunk_info_t;

typedef struct apfs_fq_key {
	uint64_t	xid;
	uint64_t	address;
} apfs_fq_key_t;

typedef struct apfs_reaper {
	uint64_t	next_reap_id;
	uint64_t	completed_id;
	uint64_t	head;
	uint64_t	tail;
	uint64_t	reaping_volume_oid;	/* the object being reaped */
	uint64_t	reaping_oid;
	uint64_t	reaping_xid;
	uint32_t	flags;
	uint32_t	rlcount;
	uint32_t	reaping_type;
	uint32_t	reaping_size;
	uint32_t	reaping_entry_flags;
	uint32_t	state_size;
} apfs_reaper_t;

/* A reap-list object: one link of the reaper's list. */
typedef struct apfs_reap_list {
	uint64_t	next;		/* the next link's OID, zero in the last */
	uint32_t	flags;
	uint32_t	capacity;	/* entries */
	uint32_t	count;
	uint32_t	first;		/* entry indexes, or APFS_REAP_LIST_NONE */
	uint32_t	last;
	uint32_t	free;
} apfs_reap_list_t;

/*
 * Fletcher-64 (codec.c). The checksum covers an object from byte 8 to its
 * end as little-endian 32-bit words. An object of several blocks feeds its
 * bytes in pieces: apfs_fletcher_init(), apfs_fletcher_update() over each
 * piece, whose length must be a multiple of 4, then apfs_fletcher_value().
 * A block of zeros computes to all ones, so it never verifies.
 */
typedef struct apfs_fletcher {
	uint64_t	s1;
	uint64_t	s2;
} apfs_fletcher_t;

void		apfs_fletcher_init(apfs_fletcher_t *);
void		apfs_fletcher_update(apfs_fletcher_t *, const unsigned char *,
		    size_t);
uint64_t	apfs_fletcher_value(const apfs_fletcher_t *);
/* The checksum of a one-block object. apfs_object_verify() compares it with
 * the stored one and apfs_object_seal() stores it; both refuse a length
 * below APFS_OBJ_HEADER_BYTES or not a multiple of 4.
 */
uint64_t	apfs_object_checksum(const unsigned char *, size_t);
bool		apfs_object_verify(const unsigned char *, size_t);
bool		apfs_object_seal(unsigned char *, size_t);

/*
 * Record decoders and encoders (codec.c). A decoder takes bytes and their
 * length and returns false, leaving its output untouched, when the length or
 * a field that frames the record is wrong; it reads nothing past length. The
 * false return is FSIMG_ECORRUPT in every caller, which names the object's
 * role. Field values are otherwise the caller's to validate: magic, kinds,
 * geometry, flags and identifiers.
 *
 * An object decoder takes the whole object, header included, and reads its
 * fixed structure; a key or value decoder takes exactly the record. An
 * encoder is the decoder's inverse over the same span: it writes every field
 * and every reserved byte of the fixed structure, and nothing else, so the
 * caller supplies a buffer whose other bytes are already zero or written.
 * An encoder returns false and writes nothing when its decoder would refuse
 * the result for its length or framing. Encoding a header writes a zero
 * checksum, which apfs_object_seal() replaces once the object is complete.
 */
bool	apfs_obj_decode(const unsigned char *, size_t, apfs_obj_t *);
bool	apfs_obj_encode(unsigned char *, size_t, const apfs_obj_t *);

bool	apfs_nx_decode(const unsigned char *, size_t, apfs_nx_t *);
bool	apfs_nx_encode(unsigned char *, size_t, const apfs_nx_t *);

/* A checkpoint-mapping block's header is accepted only when its count of
 * mappings fits the block. apfs_cpm_decode() reads mapping index of a block
 * whose header was decoded, and refuses an index at or past its count.
 */
bool	apfs_cpm_block_decode(const unsigned char *, size_t,
	    apfs_cpm_block_t *);
bool	apfs_cpm_block_encode(unsigned char *, size_t,
	    const apfs_cpm_block_t *);
bool	apfs_cpm_decode(const unsigned char *, size_t, const apfs_cpm_block_t *,
	    uint32_t, apfs_cpm_t *);
bool	apfs_cpm_encode(unsigned char *, size_t, const apfs_cpm_block_t *,
	    uint32_t, const apfs_cpm_t *);

bool	apfs_omap_decode(const unsigned char *, size_t, apfs_omap_t *);
bool	apfs_omap_encode(unsigned char *, size_t, const apfs_omap_t *);
bool	apfs_omap_key_decode(const unsigned char *, size_t, apfs_omap_key_t *);
bool	apfs_omap_key_encode(unsigned char *, size_t,
	    const apfs_omap_key_t *);
bool	apfs_omap_val_decode(const unsigned char *, size_t, apfs_omap_val_t *);
bool	apfs_omap_val_encode(unsigned char *, size_t,
	    const apfs_omap_val_t *);

/*
 * A node is accepted when its areas nest inside it in order, its table of
 * contents holds an entry for every key, and a root has room for its
 * footer. apfs_entry_decode() resolves entry index, below the key count,
 * into spans proved inside the key area and the value area; sizes gives the
 * record sizes when the node's entries are fixed-size. Order, levels, flags
 * against the tree, alignment and record lengths are btree.c's checks.
 * apfs_node_encode() writes the header and, for a root, the footer, from the
 * stored fields. apfs_entry_encode() proves an entry's spans by the same
 * rule as apfs_entry_decode(), writes entry index as stored, and returns the
 * spans, the only places its caller then writes the key and value.
 */
bool	apfs_node_decode(const unsigned char *, size_t, apfs_node_t *);
bool	apfs_node_encode(unsigned char *, size_t, const apfs_node_t *);
bool	apfs_entry_decode(const unsigned char *, size_t, const apfs_node_t *,
	    uint32_t, const apfs_kv_size_t *, apfs_entry_t *);
bool	apfs_entry_encode(unsigned char *, size_t, const apfs_node_t *,
	    uint32_t, const apfs_toc_t *, const apfs_kv_size_t *,
	    apfs_entry_t *);
bool	apfs_btree_info_decode(const unsigned char *, size_t,
	    apfs_btree_info_t *);
bool	apfs_btree_info_encode(unsigned char *, size_t,
	    const apfs_btree_info_t *);

bool	apfs_spaceman_decode(const unsigned char *, size_t, apfs_spaceman_t *);
bool	apfs_spaceman_encode(unsigned char *, size_t, const apfs_spaceman_t *);

/* Chunk-information and address blocks are accepted only when their count
 * of records fits the block; the record readers take the decoded header and
 * refuse an index at or past its count.
 */
bool	apfs_cib_decode(const unsigned char *, size_t, apfs_cib_t *);
bool	apfs_cib_encode(unsigned char *, size_t, const apfs_cib_t *);
bool	apfs_chunk_info_decode(const unsigned char *, size_t,
	    const apfs_cib_t *, uint32_t, apfs_chunk_info_t *);
bool	apfs_chunk_info_encode(unsigned char *, size_t, const apfs_cib_t *,
	    uint32_t, const apfs_chunk_info_t *);
bool	apfs_cab_decode(const unsigned char *, size_t, apfs_cib_t *);
bool	apfs_cab_encode(unsigned char *, size_t, const apfs_cib_t *);
bool	apfs_cab_address_decode(const unsigned char *, size_t,
	    const apfs_cib_t *, uint32_t, uint64_t *);
bool	apfs_cab_address_encode(unsigned char *, size_t, const apfs_cib_t *,
	    uint32_t, uint64_t);

bool	apfs_fq_key_decode(const unsigned char *, size_t, apfs_fq_key_t *);
bool	apfs_fq_key_encode(unsigned char *, size_t, const apfs_fq_key_t *);
bool	apfs_fq_val_decode(const unsigned char *, size_t, uint64_t *);
bool	apfs_fq_val_encode(unsigned char *, size_t, uint64_t);

/* The reaper is accepted only when its state buffer lies inside it. */
bool	apfs_reaper_decode(const unsigned char *, size_t, apfs_reaper_t *);
bool	apfs_reaper_encode(unsigned char *, size_t, const apfs_reaper_t *);

/* A reap-list object is accepted only when its entry array, capacity
 * entries from APFS_REAP_LIST_BYTES, lies inside it and its entry count is
 * at most its capacity. The entries are not decoded: the reader only asks
 * whether a list holds any.
 */
bool	apfs_reap_list_decode(const unsigned char *, size_t,
	    apfs_reap_list_t *);
bool	apfs_reap_list_encode(unsigned char *, size_t,
	    const apfs_reap_list_t *);

/*
 * Diagnostics (object.c). A failure names the object's role, such as
 * "container object map" or "checkpoint descriptor", and what is wrong with
 * it, never its bytes: "APFS <role> <what>", under FSIMG_OP_READER.
 */
fsimg_status_t	apfs_corrupt(fsimg_error_t *, const char *, const char *);
fsimg_status_t	apfs_unsupported(fsimg_error_t *, const char *, const char *);

/*
 * The container being read (checkpoint.c): the selected checkpoint's
 * superblock and geometry, and its ephemeral mapping table, a paged array of
 * the checkpoint's mappings sorted by ephemeral OID in metadata scratch.
 * block_size and block_count are B and N; before a checkpoint is selected
 * they are block 0's, afterwards the selected superblock's.
 */
typedef struct apfs_container {
	fsimg_reader_t	*reader;
	struct fsimg_store *mappings;
	apfs_nx_t	 nx;		/* the selected superblock */
	uint64_t	 mapping_count;
	uint64_t	 block_count;
	uint64_t	 xid;		/* the selected checkpoint's */
	uint32_t	 block_size;
	bool		 clean;		/* no newer checkpoint was torn */
} apfs_container_t;

/*
 * apfs_nx_geometry(nx, role, error) applies the container geometry rules to
 * a decoded superblock: block size, block count, maximum volume count and
 * volume array, the two areas, which must be contiguous (a noncontiguous
 * area is FSIMG_EUNSUPPORTED, tested first), and the keylocker ranges.
 * Block 0 and every checkpoint candidate pass through it.
 *
 * apfs_checkpoint_open(reader, header, nx, container, error) selects the
 * checkpoint, given block 0's validated header and superblock, and fills
 * container; on failure container holds nothing to close. The steps and
 * their statuses are specs/design-apfs.md, "Checkpoint selection": every
 * failure after a checkpoint is selected is final, with no fallback to an
 * older one: FSIMG_ECORRUPT for damage, FSIMG_EUNSUPPORTED for a mapped object
 * outside the profile. The checks end with the container object map's
 * header, read as a physical object; apfs_map_open() judges its contents.
 * apfs_container_close() frees the mapping table and may be called on a
 * zeroed container.
 *
 * apfs_ephemeral_find(container, oid, found, mapping, error) looks up an
 * ephemeral OID in the mapping table.
 *
 * apfs_reaper_idle(container, window, idle, error) reads the reaper and its
 * reap list (specs/design-apfs.md, "Reaper") through window, B bytes: a list
 * that is not well formed is FSIMG_ECORRUPT, and idle reports whether no
 * object is being reaped and every list object holds no entry.
 */
fsimg_status_t	apfs_nx_geometry(const apfs_nx_t *, const char *,
		    fsimg_error_t *);
fsimg_status_t	apfs_checkpoint_open(fsimg_reader_t *, const apfs_obj_t *,
		    const apfs_nx_t *, apfs_container_t *, fsimg_error_t *);
void		apfs_container_close(apfs_container_t *);
fsimg_status_t	apfs_ephemeral_find(apfs_container_t *, uint64_t, bool *,
		    apfs_cpm_t *, fsimg_error_t *);
fsimg_status_t	apfs_reaper_idle(apfs_container_t *, unsigned char *, bool *,
		    fsimg_error_t *);

/*
 * Objects (object.c): the one place that interprets an object reference.
 * A reference names the storage class, OID, kind and subtype it demands and
 * the XID it is read at; a virtual reference also names the object map that
 * resolves it. apfs_object_read(container, ref, window, header, error)
 * resolves the reference, reads the object through window, B bytes,
 * verifies its checksum over all its blocks, and checks its header against
 * the reference (specs/design-apfs.md, "Object header and checksum"):
 *
 *   physical   the OID is the block, in [1, N); the XID is not above ref's;
 *   virtual    ref's map resolves the OID at ref's XID, and the object's XID
 *              equals the found key's; a miss is FSIMG_ECORRUPT;
 *   ephemeral  the mapping table resolves the OID, and the mapping's kind
 *              and subtype must be ref's; the XID is not above the
 *              checkpoint's, whatever ref's XID.
 *
 * The object's size must be ref's blocks, one when zero: a mapping or
 * object-map value stating another size is FSIMG_ECORRUPT before anything
 * is read, so a hostile size cannot make a read stream the container. The
 * space manager is the one object of several blocks; its reference takes
 * its size from its mapping, which only the data run bounds until its
 * reader checks it against the size its geometry implies. It streams its
 * blocks through
 * window, the checksum running over them; window holds its first block on
 * return. The encrypted and
 * nonpersistent flags are FSIMG_EUNSUPPORTED. apfs_block_read() is the one
 * read of a block by number, refusing one outside [1, N).
 */
typedef struct apfs_map apfs_map_t;

typedef struct apfs_ref {
	apfs_map_t	*map;		/* virtual only */
	const char	*role;		/* names the object in diagnostics */
	uint64_t	 oid;
	uint64_t	 xid;
	uint32_t	 blocks;	/* the size it must have; zero is one */
	uint32_t	 storage;	/* APFS_OBJ_PHYSICAL, _VIRTUAL, _EPHEMERAL */
	uint32_t	 subtype;
	uint16_t	 kind;
} apfs_ref_t;

fsimg_status_t	apfs_block_read(apfs_container_t *, uint64_t, unsigned char *,
		    const char *, fsimg_error_t *);
fsimg_status_t	apfs_object_read(apfs_container_t *, const apfs_ref_t *,
		    unsigned char *, apfs_obj_t *, fsimg_error_t *);

/*
 * B-trees (btree.c): the one place that interprets a node. A tree profile
 * states what a tree's records look like: its subtype, its comparator, the
 * length bounds of its keys and of its leaf values, and its fixed key and
 * value sizes, both zero for a tree of variable-size entries. An index
 * node's value is always a child OID. A comparator orders two keys whose
 * lengths lie within the profile's bounds and must be total over any bytes
 * of such lengths.
 *
 * entry_check, optional, serves trees whose records differ by kind: it
 * judges each entry of a node as it is loaded, after the generic checks and
 * before any entry is used, such as a key and value length per record kind;
 * its failure is the load's.
 *
 * A cursor belongs to one tree: its profile, storage class, root OID, the XID
 * its nodes are read at, and for a virtual tree the map resolving its nodes.
 * It holds the root-to-leaf path, one window of B bytes per level allocated
 * as the path deepens and kept until apfs_cursor_free(). Every node loaded
 * passes the validation of specs/design-apfs.md, "Validation", including the
 * separator bounds its parent gives it; a node repeated on the path is
 * FSIMG_ECORRUPT and a root above level 15 FSIMG_EUNSUPPORTED.
 *
 * apfs_cursor_seek() positions on the last record at or below key
 * (APFS_SEEK_LE) or the first at or above it (APFS_SEEK_GE), descending from
 * the deepest node on the path whose bounds contain key, which must have a
 * length within the profile's key bounds; apfs_cursor_first()
 * on the first record; apfs_cursor_next() on the following one. Each
 * reports through found whether the cursor is on a record; a cursor that is
 * not stays so until the next seek or first. apfs_cursor_record() borrows the
 * current record's spans, valid until the cursor next moves or is freed.
 */
typedef int	(*apfs_compare_fn_t)(const unsigned char *, size_t,
		    const unsigned char *, size_t);
typedef struct apfs_record apfs_record_t;
typedef fsimg_status_t (*apfs_entry_check_fn_t)(const apfs_record_t *, bool,
		    fsimg_error_t *);

typedef struct apfs_tree_profile {
	const char	*role;
	apfs_compare_fn_t compare;
	apfs_entry_check_fn_t entry_check;	/* optional; leaf is its bool */
	uint32_t	 subtype;
	uint16_t	 key_min;
	uint16_t	 key_max;
	uint16_t	 val_min;	/* leaf values */
	uint16_t	 val_max;
	uint16_t	 fixed_key;	/* both zero: variable-size entries */
	uint16_t	 fixed_val;
} apfs_tree_profile_t;

typedef enum {
	APFS_SEEK_LE,
	APFS_SEEK_GE
} apfs_seek_t;

struct apfs_record {
	const unsigned char *key;
	const unsigned char *val;	/* NULL for a ghost */
	size_t		 key_len;
	size_t		 val_len;
	bool		 ghost;
};

/*
 * One level of a cursor's path. low and high are the separator bounds the
 * node was entered under, spans in its ancestors' windows: every key of the
 * node is at least low and below high, and a NULL bound is open.
 */
typedef struct apfs_path_level {
	unsigned char	*window;
	const unsigned char *low;
	const unsigned char *high;
	apfs_node_t	 node;
	uint64_t	 oid;
	size_t		 low_len;
	size_t		 high_len;
	uint32_t	 index;		/* table-of-contents position */
} apfs_path_level_t;

typedef struct apfs_cursor {
	apfs_container_t *container;
	const apfs_tree_profile_t *profile;
	apfs_map_t	*map;		/* virtual trees */
	apfs_btree_info_t info;		/* the root's footer, once loaded */
	apfs_path_level_t path[APFS_BTREE_LEVELS_MAX];
	uint64_t	 root;
	uint64_t	 xid;
	uint32_t	 storage;
	unsigned	 depth;		/* levels of path loaded */
	bool		 positioned;	/* on a record */
} apfs_cursor_t;

void		apfs_cursor_init(apfs_cursor_t *, apfs_container_t *,
		    const apfs_tree_profile_t *, uint32_t, uint64_t, uint64_t,
		    apfs_map_t *);
void		apfs_cursor_free(apfs_cursor_t *);
fsimg_status_t	apfs_cursor_seek(apfs_cursor_t *, const unsigned char *,
		    size_t, apfs_seek_t, bool *, fsimg_error_t *);
fsimg_status_t	apfs_cursor_first(apfs_cursor_t *, bool *, fsimg_error_t *);
fsimg_status_t	apfs_cursor_next(apfs_cursor_t *, bool *, fsimg_error_t *);
void		apfs_cursor_record(const apfs_cursor_t *, apfs_record_t *);

/*
 * Object maps (omap.c). apfs_map_open(container, kind, oid, xid, window, map,
 * error) reads the object-map object at physical oid, read at xid, through
 * window and checks its flags against the closed table for its kind, its
 * tree type and, for the container's map, that it has no snapshots and no
 * revert range; the volume map's snapshot fields are checked against the
 * volume's snapshot tree by its caller. The map's tree nodes are read at
 * xid too. apfs_map_lookup(map, oid, xid, found, key, value, error)
 * resolves virtual oid at xid: the greatest key not above (oid, xid), a
 * miss when that key names another OID or its value is deleted, and a found
 * value checked for its flags, size and range. apfs_map_close() frees the
 * map's cursor.
 */
typedef enum {
	APFS_MAP_CONTAINER,
	APFS_MAP_VOLUME
} apfs_map_kind_t;

struct apfs_map {
	apfs_cursor_t	 cursor;
	apfs_omap_t	 omap;
	apfs_map_kind_t	 kind;
};

fsimg_status_t	apfs_map_open(apfs_container_t *, apfs_map_kind_t, uint64_t,
		    uint64_t, unsigned char *, apfs_map_t *, fsimg_error_t *);
void		apfs_map_close(apfs_map_t *);
fsimg_status_t	apfs_map_lookup(apfs_map_t *, uint64_t, uint64_t, bool *,
		    apfs_omap_key_t *, apfs_omap_val_t *, fsimg_error_t *);

/*
 * Names (unicode.c; specs/design-apfs.md, "Directory records and names").
 * A name is the stored bytes without the NUL. The reader accepts
 * APFS_NAME_MAX_BYTES UTF-8 bytes, the most a hashed key's 10-bit length
 * holds with the NUL. The writer accepts APFS_WRITE_NAME_MAX_UNITS UTF-16
 * code units, the limit macOS applies when it creates a name (E14), in the
 * name as given and in its NFD, at most 765 UTF-8 bytes, and refuses a name
 * holding a noncharacter or a code point unassigned in Unicode 17.0.0 (E4).
 *
 * A name's comparison form is a sequence of code points derived by the
 * volume's rule. It is at most APFS_NAME_EXPANSION times its name's length
 * in UTF-8 bytes, which the table generator checks for every scalar value,
 * so its UTF-8 encoding, the namespace key, fits APFS_NAME_KEY_BYTES, and
 * the form fits APFS_NAME_FORM_POINTS points, since a point takes at least
 * one UTF-8 byte.
 */
#define APFS_NAME_MAX_BYTES		1022u
#define APFS_WRITE_NAME_MAX_UNITS	255u
#define APFS_NAME_EXPANSION		3u
#define APFS_NAME_FORM_POINTS		(APFS_NAME_MAX_BYTES * APFS_NAME_EXPANSION)
#define APFS_NAME_KEY_BYTES		(APFS_NAME_MAX_BYTES * APFS_NAME_EXPANSION)
#define APFS_NAME_HASH_BITS		22u
#define APFS_NAME_HASH_MASK		((UINT32_C(1) << APFS_NAME_HASH_BITS) - 1)

typedef enum {
	APFS_NAMES_BYTES,	/* the form is the name itself */
	APFS_NAMES_NFD,		/* the NFD of the name */
	APFS_NAMES_CASELESS	/* the NFD of the full case folding of the NFD */
} apfs_name_rule_t;

typedef struct apfs_name_form {
	size_t		count;
	uint32_t	points[APFS_NAME_FORM_POINTS];
} apfs_name_form_t;

/* apfs_name_form(rule, name, length, form) validates a stored name and
 * stores its comparison form under rule. A name that is empty, "." or "..",
 * not strict UTF-8, or holds a NUL or '/' is FSIMG_ECORRUPT; one longer than
 * APFS_NAME_MAX_BYTES is FSIMG_EUNSUPPORTED. A form beyond its capacity,
 * which the tables' expansion bound rules out, is FSIMG_ELIMIT. On failure
 * form is empty. The statuses describe a stored name; a caller converting a
 * selection query maps them to the query's own (FSIMG_EINVAL).
 */
fsimg_status_t	apfs_name_form(apfs_name_rule_t, const char *, size_t,
		    apfs_name_form_t *);
/* The 22-bit name hash of a comparison form: CRC-32C over its points as
 * little-endian 32-bit units, from the register value FSIMG_CRC32C_SEED,
 * without a final complement or a terminating NUL, keeping the low 22 bits.
 * This is the framing macOS uses (E4); test/apfs-unicode.c checks it
 * against test/fixtures/apfs/name-hashes.txt.
 */
uint32_t	apfs_name_hash(const apfs_name_form_t *);
/* apfs_name_key(form, key, capacity, length) stores the UTF-8 encoding of
 * a comparison form, the namespace key, without a NUL. A capacity of
 * APFS_NAME_KEY_BYTES always suffices; a smaller one that does not is
 * FSIMG_ERANGE with nothing stored.
 */
fsimg_status_t	apfs_name_key(const apfs_name_form_t *, char *, size_t,
		    size_t *);
/* apfs_name_assigned(name, length), for a name apfs_name_form accepted, is
 * true when every code point of the name is assigned in Unicode 17.0.0
 * (APFS_UNICODE_VERSION), private use included. Unicode never assigns a
 * noncharacter, so a name holding one is false. The writer refuses a name
 * for which it is false; the reader classifies a stored hash mismatch on
 * such a name as FSIMG_EUNSUPPORTED rather than FSIMG_ECORRUPT. Bytes that
 * are not strict UTF-8 are false, which is no classification: the reader
 * has already refused them as corrupt.
 */
bool		apfs_name_assigned(const char *, size_t);

#endif /* !FSIMG_APFS_H */
