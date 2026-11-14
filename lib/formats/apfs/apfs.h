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

#endif /* !FSIMG_APFS_H */
