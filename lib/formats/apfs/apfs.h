/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_APFS_H
#define FSIMG_APFS_H

#include "internal.h"
#include "reader.h"

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
#define APFS_SUBTYPE_FSTREE		14u	/* a volume's filesystem tree */

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
#define APFS_FREE_LIST_EMPTY		0xffffu	/* offset of an empty free list */
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
#define APFS_BTREE_FLAGS_KNOWN		0x1ffu
/* Sealed and nonpersistent trees, which fsimg neither reads nor writes. */
#define APFS_BTREE_FLAGS_REFUSED	(APFS_BTREE_HASHED | APFS_BTREE_NOHEADER | \
				    APFS_BTREE_NONPERSISTENT)

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

/* Volume superblock (specs/design-apfs.md, "Volume superblock"). */
#define APFS_FS_MAGIC			0x42535041u	/* "APSB" */
#define APFS_FS_BYTES			1044u
#define APFS_FS_NAME_BYTES		256u	/* with its terminator */
#define APFS_FS_CRYPTO_BYTES		20u
#define APFS_FS_CHANGE_ID_BYTES		32u
#define APFS_FS_CHANGES			8u	/* modification records */
#define APFS_FS_TREE_VIRTUAL		APFS_KIND_BTREE_ROOT
#define APFS_FS_TREE_PHYSICAL		(APFS_OBJ_PHYSICAL | APFS_KIND_BTREE_ROOT)

#define APFS_FS_INCOMPAT_CASE_INSENSITIVE	0x01u
#define APFS_FS_INCOMPAT_DATALESS_SNAPSHOTS	0x02u
#define APFS_FS_INCOMPAT_ENCRYPTION_ROLLED	0x04u
#define APFS_FS_INCOMPAT_NORM_INSENSITIVE	0x08u
#define APFS_FS_INCOMPAT_INCOMPLETE_RESTORE	0x10u
#define APFS_FS_INCOMPAT_SEALED			0x20u
#define APFS_FS_FLAG_UNENCRYPTED		0x01u
#define APFS_FS_FLAG_ONE_KEY			0x08u
#define APFS_FS_FLAG_SPILLED_OVER		0x10u
#define APFS_FS_FLAG_SPILLOVER_CLEANER		0x20u
#define APFS_FS_FLAG_CHECK_EXTENT_REFS		0x40u

/*
 * The information profile: "apfs-unicode-" and the Unicode version of the
 * name tables, APFS_UNICODE_VERSION in unicode-tables.h, which
 * test/apfs-unicode.c checks it against.
 */
#define APFS_PROFILE			"apfs-unicode-17.0.0"

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
 * first block, so apfs_spaceman_check() proves each array's span inside
 * the object before the pager reads one.
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

/* A formatting or modification record of a volume superblock. */
typedef struct apfs_fs_change {
	unsigned char	id[APFS_FS_CHANGE_ID_BYTES];	/* software, raw */
	uint64_t	time;
	uint64_t	last_xid;
} apfs_fs_change_t;

/*
 * A volume superblock. The name is the raw field: the volume module checks
 * its terminator and encoding. Tree types are whole type words, storage
 * flags included.
 */
typedef struct apfs_fs {
	apfs_fs_change_t formatted_by;
	apfs_fs_change_t modified_by[APFS_FS_CHANGES];
	unsigned char	name[APFS_FS_NAME_BYTES];
	unsigned char	uuid[APFS_UUID_BYTES];
	unsigned char	group_uuid[APFS_UUID_BYTES];
	unsigned char	crypto[APFS_FS_CRYPTO_BYTES];
	uint64_t	features;
	uint64_t	ro_compat_features;
	uint64_t	incompat_features;
	uint64_t	unmount_time;
	uint64_t	reserve_blocks;
	uint64_t	quota_blocks;
	uint64_t	alloc_blocks;
	uint64_t	omap_oid;		/* physical */
	uint64_t	root_tree_oid;		/* virtual */
	uint64_t	extentref_tree_oid;	/* physical */
	uint64_t	snapshot_tree_oid;	/* physical */
	uint64_t	revert_to_xid;
	uint64_t	revert_to_oid;
	uint64_t	next_obj_id;
	uint64_t	files;
	uint64_t	directories;
	uint64_t	symlinks;
	uint64_t	other_objects;
	uint64_t	snapshots;
	uint64_t	blocks_allocated;
	uint64_t	blocks_freed;
	uint64_t	modified_time;
	uint64_t	flags;
	uint64_t	root_to_xid;
	uint64_t	rolling_state_oid;
	uint64_t	clone_epoch;
	uint64_t	clone_xid;
	uint64_t	snapshot_ext_oid;
	uint64_t	integrity_oid;
	uint64_t	fext_tree_oid;
	uint32_t	magic;
	uint32_t	index;		/* slot in the container's array */
	uint32_t	root_tree_type;
	uint32_t	extentref_tree_type;
	uint32_t	snapshot_tree_type;
	uint32_t	fext_tree_type;
	uint32_t	next_doc_id;
	uint16_t	role;
} apfs_fs_t;

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
bool	apfs_fs_decode(const unsigned char *, size_t, apfs_fs_t *);
bool	apfs_fs_encode(unsigned char *, size_t, const apfs_fs_t *);

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
	apfs_spaceman_t	 spaceman;	/* the selected checkpoint's, checked */
	uint64_t	 spaceman_address; /* its first block */
	uint32_t	 block_size;
	bool		 clean;		/* no newer checkpoint was torn */
} apfs_container_t;

/*
 * apfs_block_size_valid(b) is the block size rule: a power of two from
 * APFS_BLOCK_MIN through APFS_BLOCK_MAX.
 *
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
bool		apfs_block_size_valid(uint32_t);
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
 * space manager is the one object of several blocks: apfs_spaceman_read()
 * passes the size its geometry implies, so a mapping stating another size
 * fails before its blocks are read. It streams its blocks through window,
 * the checksum running over them; window holds its first block on return.
 * The encrypted and nonpersistent flags are FSIMG_EUNSUPPORTED.
 * apfs_block_read() is the one read of a block by number, refusing one
 * outside [1, N).
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
 * The space manager (spaceman.c; specs/design-apfs.md, "Ephemeral objects
 * and the space manager").
 *
 * apfs_spaceman_check(sm, block_size, block_count, blocks, error) applies
 * the rules that tie a decoded space manager to the container's B and N:
 * the chunk geometry and the main device's counts (an address-block count
 * that breaks the relation is FSIMG_EUNSUPPORTED at a block size other than
 * 4096 and 16384, whose threshold no capture settles), an empty tier-2
 * device, a pool bitmap covering the pool, and arrays that lie past the
 * fixed structure, naturally aligned, disjoint, and ending no later than
 * the native packing of the same counts ends. blocks receives the size
 * those arrays imply, the smallest whole number of blocks holding the
 * structure and them, which the geometry therefore bounds. An array past
 * that packing is FSIMG_EUNSUPPORTED, a provisional rule; everything else
 * is FSIMG_ECORRUPT.
 *
 * apfs_spaceman_read(container, window, error) is the one read of the
 * selected checkpoint's space manager, which the checkpoint load makes: a
 * Fusion container is FSIMG_EUNSUPPORTED; the fixed structure is decoded
 * from the first block, unverified, and checked, which gives the size its
 * arrays imply within the geometry's bound; the object is read through
 * apfs_object_read() at that size, so the mapped size must equal it before
 * any further block is read; and the verified copy is checked again and
 * kept in container->spaceman,
 * its first block in container->spaceman_address. window holds that block
 * on return.
 *
 * apfs_fixed_regions(nx, sm, regions, error) lists the fixed regions of a
 * container, the block ranges its superblock and space manager state by
 * position and size: block 0, the checkpoint descriptor and data areas, the
 * pool's bitmap ring and the internal pool. It is a pure function of those
 * fields, the one list both sides use: verification passes the decoded
 * selected superblock and space manager and claims each region; the writer
 * passes the fields it is about to encode and lays out the blocks outside
 * them. regions receives them in ascending block order; a region that is
 * empty, leaves [0, N) or overlaps another is FSIMG_ECORRUPT, named by role,
 * which the writer, whose fields are its own, treats as an internal error.
 *
 * apfs_native_geometry(block_count, nx, sm) zeroes nx and sm and fills the
 * fields the native formatter derives from N alone for 4096-byte blocks
 * (specs/design-apfs.md, "Writer values"): B and N, the maximum volume
 * count, the checkpoint areas, the ephemeral-information word, and the
 * space manager's chunk geometry, main-device counts, internal pool, bitmap
 * ring's size, packed array offsets and free-queue node limits; the ring's
 * slots, counts, XIDs and identifiers are the writer's. It returns false for
 * an N of zero or above APFS_WRITER_BLOCKS_MAX. Its space manager passes
 * apfs_spaceman_check(), and its fields give apfs_fixed_regions() the
 * native layout. Below about 83 blocks that layout does not fit in N, which
 * apfs_fixed_regions() reports; the writer's minimum container size, which
 * planning applies, lies far above.
 */
#define APFS_WRITER_BLOCK_SIZE		4096u
#define APFS_WRITER_BLOCKS_MAX		34359738368ull

typedef enum apfs_region_kind {
	APFS_REGION_BLOCK0,
	APFS_REGION_DESCRIPTORS,
	APFS_REGION_DATA,
	APFS_REGION_POOL_RING,
	APFS_REGION_POOL,
	APFS_FIXED_REGIONS
} apfs_region_kind_t;

typedef struct apfs_region {
	uint64_t		first;
	uint64_t		count;
	apfs_region_kind_t	kind;
} apfs_region_t;

fsimg_status_t	apfs_spaceman_check(const apfs_spaceman_t *, uint32_t,
		    uint64_t, uint64_t *, fsimg_error_t *);
fsimg_status_t	apfs_spaceman_read(apfs_container_t *, unsigned char *,
		    fsimg_error_t *);
fsimg_status_t	apfs_fixed_regions(const apfs_nx_t *, const apfs_spaceman_t *,
		    apfs_region_t[APFS_FIXED_REGIONS], fsimg_error_t *);
const char	*apfs_region_role(apfs_region_kind_t);
bool		apfs_native_geometry(uint64_t, apfs_nx_t *, apfs_spaceman_t *);

/*
 * The space-manager pager (spaceman.c), for verification: one pass over the
 * container's checked space manager, container->spaceman, the same copy
 * apfs_fixed_regions() is given. It holds four blocks whatever the
 * container's size: one block of the space manager's arrays at a time, the
 * current address block, the current chunk-information block and a bitmap.
 * It charges one unit of work per chunk and per block it reads. A pass has
 * no rewind: each pass opens its own pager.
 *
 * apfs_sm_pager_open(container, pager, error) allocates the pager.
 *
 * apfs_sm_pager_next(pager, chunk, found, error) visits the main device's
 * chunks in ascending order, reading each address block and
 * chunk-information block once, through the address array, as the first
 * chunk it covers is reached. The ith block of either kind must carry index
 * i and the entry count the geometry gives it, so no block is visited
 * twice; chunk k must start at block k * 8B, cover the blocks the geometry
 * gives it, have a free count not above them, an XID not above the
 * checkpoint's, and a bitmap address inside the container, or zero when the
 * chunk is entirely free. Past the last chunk found is false, and the
 * device's free count must equal the sum of the chunks' free counts.
 *
 * apfs_sm_pager_bitmap(pager, bits, error) borrows the bitmap block of the
 * chunk last visited, B bytes, bit i of byte j (least significant first)
 * standing for block 8j + i of the chunk, or NULL for an entirely free
 * chunk. Its first call per chunk reads the block and requires the clear
 * bits among the chunk's blocks to equal its free count; bits past them,
 * past N in the last chunk, are not judged.
 *
 * apfs_sm_pager_pool_bitmap(pager, k, block, bits, error) borrows block k of
 * the internal pool's current bitmap, k below the bitmap's size in blocks,
 * through the ring's bitmap-index array: the slot it names must lie inside
 * the ring and its XID not above the checkpoint's; nothing else of the ring
 * is judged, so two indexes may name one slot. block receives the block
 * read.
 *
 * A borrowed bitmap is valid until the next call on the pager. After a
 * failure other than FSIMG_EINVAL, which reports a misuse and changes
 * nothing, only apfs_sm_pager_free() may be called; it accepts NULL.
 */
typedef struct apfs_sm_pager apfs_sm_pager_t;

typedef struct apfs_sm_chunk {
	apfs_chunk_info_t info;
	uint64_t	index;		/* chunk number on the device */
	uint64_t	cib_address;	/* the chunk-information block */
	uint64_t	cab_address;	/* its address block, zero without */
	bool		first_of_cib;	/* the first chunk its CIB lists */
	bool		first_of_cab;	/* the first chunk its CAB covers */
} apfs_sm_chunk_t;

fsimg_status_t	apfs_sm_pager_open(apfs_container_t *, apfs_sm_pager_t **,
		    fsimg_error_t *);
void		apfs_sm_pager_free(apfs_sm_pager_t *);
fsimg_status_t	apfs_sm_pager_next(apfs_sm_pager_t *, apfs_sm_chunk_t *,
		    bool *, fsimg_error_t *);
fsimg_status_t	apfs_sm_pager_bitmap(apfs_sm_pager_t *,
		    const unsigned char **, fsimg_error_t *);
fsimg_status_t	apfs_sm_pager_pool_bitmap(apfs_sm_pager_t *, uint32_t,
		    uint64_t *, const unsigned char **, fsimg_error_t *);

/*
 * B-trees (btree.c): the one place that interprets a node. A tree profile
 * states what a tree's records look like: its subtype, its comparator, the
 * length bounds of its keys and of its leaf values, and its fixed key and
 * value sizes, both zero for a tree of variable-size entries. An index
 * node's value is always a child OID. A comparator orders two keys whose
 * lengths lie within the profile's bounds and must be total over any bytes
 * of such lengths. It is a weak order: keys it calls equal form classes,
 * and a class is either decided, holding at most one key of the tree, or
 * undecided, when the format leaves the order of its keys open.
 *
 * undecided, optional, tells whether a key's class is undecided; NULL means
 * every class is decided. Keys of one undecided class may lie in either
 * order, within a node and across nodes, so a child may end in keys equal
 * to the separator after it; no key of any class lies out of order. The
 * cursor does not refuse two equal keys of such a class, which is its
 * consumer's to judge. A lookup in such a class seeks its first key and
 * scans the class, since seeking cannot tell its keys apart.
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
 * (APFS_SEEK_LE) or the first at or above it (APFS_SEEK_GE), so among
 * records the comparator calls equal to key on the last or the first. It
 * descends from the deepest node on the path whose bounds strictly contain
 * key, which must have a length within the profile's key bounds; a walk from
 * a seek or first that loads more nodes than the slice has blocks, plus the
 * height, repeats nodes and is FSIMG_ECORRUPT. apfs_cursor_first()
 * on the first record; apfs_cursor_next() on the following one. Each
 * reports through found whether the cursor is on a record; a cursor that is
 * not stays so until the next seek or first. apfs_cursor_record() borrows the
 * current record's spans, valid until the cursor next moves or is freed.
 */
typedef int	(*apfs_compare_fn_t)(const unsigned char *, size_t,
		    const unsigned char *, size_t);
typedef struct apfs_record apfs_record_t;
typedef bool	(*apfs_undecided_fn_t)(const unsigned char *, size_t);
typedef fsimg_status_t (*apfs_entry_check_fn_t)(const apfs_record_t *, bool,
		    fsimg_error_t *);

typedef struct apfs_tree_profile {
	const char	*role;
	apfs_compare_fn_t compare;
	apfs_undecided_fn_t undecided;		/* optional */
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
 * node is at least low and below high, or equal to high in an undecided
 * class, and a NULL bound is open.
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
	uint64_t	 loads;		/* nodes loaded since seek or first */
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
 * The filesystem tree's profile for a volume whose directory records use
 * hashed keys (true) or plain ones. Keys order by owner, kind and the
 * kind's suffix, and a key shorter than its kind's suffix, such as a header
 * alone, orders at or before every key of its owner and kind, so a GE seek
 * with it reaches their first record. In the plain
 * profile the directory records of one owner form one undecided class:
 * their names take no part in the order, so a name lookup scans the
 * directory. The entry check judges every key, in index and leaf nodes,
 * with its kind's decoder and refuses a kind the tree does not hold; values
 * are judged by the modules that decode them.
 */
const apfs_tree_profile_t *apfs_fs_tree_profile(bool);

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
 * Container superblock checks (descriptor.c). apfs_block0_check(raw, length,
 * header, nx, error) validates block 0 after its magic, the identity check
 * its caller has made: length must hold one block, the header must be the
 * container superblock's with a valid checksum over B bytes, and the
 * geometry and features must pass; on success it stores the decoded header
 * and superblock. Open runs it on the block it read. The probe runs the
 * private function behind it on its 4096-byte prefix, with the volume
 * length in place of the block's, so it verifies the checksum only when the
 * prefix holds the block, and open makes every check the probe makes.
 * apfs_nx_features(nx, role, error) applies the feature table alone; open
 * applies it to the selected checkpoint's superblock too.
 */
fsimg_status_t	apfs_block0_check(const unsigned char *, size_t, apfs_obj_t *,
		    apfs_nx_t *, fsimg_error_t *);
fsimg_status_t	apfs_nx_features(const apfs_nx_t *, const char *,
		    fsimg_error_t *);

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

/*
 * Filesystem-tree records (specs/design-apfs.md, "Filesystem records"). A
 * key starts with an 8-byte header holding the owner's object ID in its low
 * 60 bits and the record kind in its high 4; the kind selects the rest of
 * the key and the value. Sizes are each record's fixed part; a name stored
 * after a u16 or 10-bit length counts its NUL in that length.
 */
#define APFS_JKEY_BYTES			8u
#define APFS_JKEY_OID_MASK		UINT64_C(0x0fffffffffffffff)
#define APFS_JKEY_KIND_SHIFT		60u
#define APFS_JKEY_KIND_MASK		0xfu

#define APFS_J_SNAP_METADATA		1u	/* record kinds */
#define APFS_J_PHYS_EXTENT		2u
#define APFS_J_INODE			3u
#define APFS_J_XATTR			4u
#define APFS_J_SIBLING_LINK		5u
#define APFS_J_DSTREAM_ID		6u
#define APFS_J_CRYPTO_STATE		7u	/* identified, never decoded */
#define APFS_J_FILE_EXTENT		8u
#define APFS_J_DREC			9u
#define APFS_J_DIR_STATS		10u
#define APFS_J_SNAP_NAME		11u
#define APFS_J_SIBLING_MAP		12u
#define APFS_J_FILE_INFO		13u

#define APFS_INODE_VAL_BYTES		92u

/* A plain directory-record key stores a u16 name length after the header,
 * a hashed one a u32 of the length (low 10 bits) and the hash (high 22).
 * The mask and shift are E4's: the reference prints the mask wrongly.
 */
#define APFS_DREC_KEY_BYTES		10u
#define APFS_DREC_HASHED_KEY_BYTES	12u
#define APFS_DREC_LEN_MASK		0x000003ffu
#define APFS_DREC_HASH_MASK		0xfffffc00u
#define APFS_DREC_HASH_SHIFT		10u
#define APFS_DREC_VAL_BYTES		18u
#define APFS_DREC_TYPE_MASK		0x000fu	/* in the value's flags */

#define APFS_XATTR_KEY_BYTES		10u
#define APFS_XATTR_VAL_BYTES		4u
#define APFS_XATTR_STREAM_BYTES		48u	/* stream OID and data stream */
#define APFS_XATTR_DATA_STREAM		0x0001u
#define APFS_XATTR_DATA_EMBEDDED	0x0002u
#define APFS_XATTR_FS_OWNED		0x0004u

#define APFS_SIBLING_KEY_BYTES		16u
#define APFS_SIBLING_VAL_BYTES		10u
#define APFS_SIBLING_MAP_VAL_BYTES	8u
#define APFS_DSTREAM_ID_VAL_BYTES	4u

#define APFS_EXTENT_KEY_BYTES		16u
#define APFS_EXTENT_VAL_BYTES		24u
#define APFS_EXTENT_LEN_MASK		UINT64_C(0x00ffffffffffffff)
#define APFS_EXTENT_FLAGS_SHIFT		56u

#define APFS_PEXT_VAL_BYTES		20u
#define APFS_PEXT_LEN_MASK		UINT64_C(0x0fffffffffffffff)
#define APFS_PEXT_KIND_SHIFT		60u
#define APFS_PEXT_KIND_MASK		0xfu

#define APFS_DSTREAM_BYTES		40u
#define APFS_DIR_STATS_BYTES		32u

#define APFS_SNAP_META_VAL_BYTES	50u
#define APFS_SNAP_NAME_KEY_BYTES	10u
#define APFS_SNAP_NAME_VAL_BYTES	8u

/* A file-information key packs a type (high 8 bits) and a block address. */
#define APFS_FILE_INFO_KEY_BYTES	16u
#define APFS_FILE_INFO_VAL_BYTES	3u
#define APFS_FILE_INFO_LBA_MASK		UINT64_C(0x00ffffffffffffff)
#define APFS_FILE_INFO_TYPE_SHIFT	56u

/*
 * Extended fields: a 4-byte blob header (field count, used data bytes), a
 * 4-byte header per field (type, flags, data size), then each field's data
 * padded with zeros to a multiple of APFS_XF_ALIGN bytes. The used count is
 * the padded data alone (E2, E3), not the headers the reference adds.
 */
#define APFS_XF_BLOB_BYTES		4u
#define APFS_XF_HEADER_BYTES		4u
#define APFS_XF_ALIGN			8u

#define APFS_XF_INO_SNAP_XID		1u	/* inode field types */
#define APFS_XF_INO_DELTA_TREE_OID	2u
#define APFS_XF_INO_DOCUMENT_ID		3u
#define APFS_XF_INO_NAME		4u
#define APFS_XF_INO_PREV_FSIZE		5u
#define APFS_XF_INO_FINDER_INFO		7u
#define APFS_XF_INO_DSTREAM		8u
#define APFS_XF_INO_DIR_STATS		10u
#define APFS_XF_INO_FS_UUID		11u
#define APFS_XF_INO_SPARSE_BYTES	13u
#define APFS_XF_INO_RDEV		14u
#define APFS_XF_INO_PURGEABLE_FLAGS	15u
#define APFS_XF_INO_ORIG_SYNC_ROOT	16u
#define APFS_XF_DREC_SIBLING_ID		1u	/* a directory-record field */

#define APFS_XF_DATA_DEPENDENT		0x01u	/* field flags */
#define APFS_XF_DO_NOT_COPY		0x02u
#define APFS_XF_CHILDREN_INHERIT	0x08u
#define APFS_XF_USER_FIELD		0x10u
#define APFS_XF_SYSTEM_FIELD		0x20u

/*
 * Decoded filesystem-tree records. Key and value are decoded separately,
 * as the tree stores them, so a search key is built without a value; the
 * modules serving entries join them. A name or data member is a borrowed
 * span of the bytes decoded, valid while they are; a decoded name excludes
 * the NUL its stored length counts. Times count nanoseconds since 1970 as
 * signed values (specs/design-apfs.md, "Inodes").
 */
typedef struct apfs_jkey {
	uint64_t	oid;
	uint8_t		kind;
} apfs_jkey_t;

/* An extended-field blob as decoded: the bytes it spans, empty when the
 * record has none, its field count and its used data bytes.
 */
typedef struct apfs_xfields {
	const unsigned char *raw;
	size_t		length;
	uint16_t	count;
	uint16_t	used;
} apfs_xfields_t;

/* One extended field and its data, without the padding. */
typedef struct apfs_xfield {
	const unsigned char *data;
	uint16_t	size;
	uint8_t		type;
	uint8_t		flags;
} apfs_xfield_t;

typedef struct apfs_dstream {
	uint64_t	size;
	uint64_t	alloced_size;
	uint64_t	default_crypto_id;
	uint64_t	total_bytes_written;
	uint64_t	total_bytes_read;
} apfs_dstream_t;

typedef struct apfs_dir_stats {
	uint64_t	num_children;
	uint64_t	total_size;
	uint64_t	chained_key;
	uint64_t	gen_count;
} apfs_dir_stats_t;

typedef struct apfs_inode_val {
	apfs_xfields_t	xfields;
	uint64_t	parent;
	uint64_t	private_id;	/* owner of the file extents */
	int64_t		create_time;
	int64_t		mod_time;
	int64_t		change_time;
	int64_t		access_time;
	uint64_t	internal_flags;
	uint64_t	uncompressed_size;
	int32_t		count;		/* directory: children; else links */
	uint32_t	protection_class;
	uint32_t	write_generation;
	uint32_t	bsd_flags;
	uint32_t	owner;
	uint32_t	group;
	uint16_t	mode;
} apfs_inode_val_t;

typedef struct apfs_drec_key {
	const char	*name;
	uint64_t	 parent;	/* the key's OID */
	uint32_t	 hash;		/* 22 bits; zero in a plain key */
	uint16_t	 name_len;
	bool		 hashed;
} apfs_drec_key_t;

typedef struct apfs_drec_val {
	apfs_xfields_t	xfields;
	uint64_t	child;
	int64_t		date_added;
	uint16_t	flags;		/* the type in APFS_DREC_TYPE_MASK */
} apfs_drec_val_t;

typedef struct apfs_name_key {
	const char	*name;
	uint64_t	 oid;
	uint16_t	 name_len;
} apfs_name_key_t;		/* an attribute's or a snapshot name's */

/* An attribute value: inline data, or a stream (data is then NULL). */
typedef struct apfs_xattr_val {
	const unsigned char *data;
	apfs_dstream_t	dstream;
	uint64_t	stream_oid;
	uint16_t	flags;
	uint16_t	data_len;	/* as stored */
} apfs_xattr_val_t;

typedef struct apfs_sibling_key {
	uint64_t	inode;		/* the key's OID */
	uint64_t	sibling_id;
} apfs_sibling_key_t;

typedef struct apfs_sibling_val {
	const char	*name;
	uint64_t	 parent;
	uint16_t	 name_len;
} apfs_sibling_val_t;

typedef struct apfs_extent_key {
	uint64_t	private_id;	/* the key's OID */
	uint64_t	logical;	/* byte offset in the stream */
} apfs_extent_key_t;

typedef struct apfs_extent_val {
	uint64_t	length;		/* bytes, 56 bits */
	uint64_t	phys_block;	/* zero for a hole */
	uint64_t	crypto_id;
	uint8_t		flags;
} apfs_extent_val_t;

/* A physical extent; its key is a bare header whose OID is the first
 * block.
 */
typedef struct apfs_pext_val {
	uint64_t	length;		/* blocks, 60 bits */
	uint64_t	owner;
	int32_t		refcnt;
	uint8_t		kind;		/* 4 bits */
} apfs_pext_val_t;

typedef struct apfs_snap_meta_val {
	const char	*name;
	uint64_t	 extentref_tree_oid;
	uint64_t	 sblock_oid;
	int64_t		 create_time;
	int64_t		 change_time;
	uint64_t	 inum;
	uint32_t	 extentref_tree_type;
	uint32_t	 flags;
	uint16_t	 name_len;
} apfs_snap_meta_val_t;

typedef struct apfs_file_info_key {
	uint64_t	private_id;	/* the key's OID */
	uint64_t	lba;		/* 56 bits */
	uint8_t		type;
} apfs_file_info_key_t;

typedef struct apfs_file_info_val {
	const unsigned char *hash;
	uint16_t	hashed_len;
	uint8_t		hash_size;
} apfs_file_info_val_t;

/*
 * Filesystem-record decoders and encoders (codec.c), under the record codec
 * rules above. apfs_jkey_decode() reads the header of any key of at least
 * APFS_JKEY_BYTES, which selects the record's decoders. A record whose key
 * is that header alone (snapshot metadata, physical extent, inode, data
 * stream ID, crypto state, directory statistics, sibling map) decodes it
 * with apfs_bare_key_decode(), which refuses any other length or a kind
 * other than its argument; apfs_bare_key_encode() writes one. Every other
 * key decoder likewise refuses a key of another kind, and a key or value
 * decoder refuses a length its fixed part and stored lengths do not fill
 * exactly. A stored name must end in its NUL; its other bytes are the
 * caller's to judge, as are kinds, flags, types, hashes and order
 * (specs/design-apfs-driver.md, "Codecs"). Reserved bytes are written as
 * zero and not judged, as in the container records. An encoder writes the
 * key header with its record's kind and refuses an OID above
 * APFS_JKEY_OID_MASK or a field wider than its stored bits; it copies a name
 * or data span from the pointer its record holds.
 */
bool	apfs_jkey_decode(const unsigned char *, size_t, apfs_jkey_t *);
bool	apfs_jkey_encode(unsigned char *, size_t, const apfs_jkey_t *);
bool	apfs_bare_key_decode(const unsigned char *, size_t, uint8_t,
	    uint64_t *);
bool	apfs_bare_key_encode(unsigned char *, size_t, uint8_t, uint64_t);

/*
 * An inode or directory-record value ends in an extended-field blob when it
 * is longer than its fixed part. apfs_xfields_decode() accepts a blob whose
 * headers and padded data fill it exactly, with the used count equal to the
 * padded data, zero padding and no type repeated; a length of zero is no
 * blob. Padding frames the next field, so it is shape, unlike a reserved
 * field. apfs_xfield_get() returns field index of a decoded blob and
 * apfs_xfield_find() the field of a type, reporting through its return
 * whether the blob holds one; both walk the headers again, every read
 * inside the blob's span, so a decoded blob, at most 256 fields since no
 * type repeats, costs at most 256 steps per call. apfs_xfields_size()
 * computes the blob length for fields, refusing one whose used count does
 * not fit 16 bits, and apfs_xfields_encode() writes exactly that length,
 * refusing a repeated type.
 *
 * The inode and directory-record encoders copy the blob their record holds
 * after the fixed part, after proving it decodes. A writer encodes the blob
 * in place after the fixed part, decodes it into the record, then encodes
 * the record; the copy is then onto itself. A blob overlapping the fixed
 * part being written is not allowed.
 */
bool	apfs_xfields_decode(const unsigned char *, size_t, apfs_xfields_t *);
bool	apfs_xfield_get(const apfs_xfields_t *, uint16_t, apfs_xfield_t *);
bool	apfs_xfield_find(const apfs_xfields_t *, uint8_t, apfs_xfield_t *);
bool	apfs_xfields_size(const apfs_xfield_t *, uint16_t, size_t *);
bool	apfs_xfields_encode(unsigned char *, size_t, const apfs_xfield_t *,
	    uint16_t);
/* A field holding one little-endian integer of exactly its size, and a
 * field holding a name ending in its NUL, returned without it.
 */
bool	apfs_xfield_u32(const apfs_xfield_t *, uint32_t *);
bool	apfs_xfield_u64(const apfs_xfield_t *, uint64_t *);
bool	apfs_xfield_name(const apfs_xfield_t *, const char **, uint16_t *);

/* The data stream of an inode field or a stream attribute, and directory
 * statistics, as a field or a kind-10 value: exact spans.
 */
bool	apfs_dstream_decode(const unsigned char *, size_t, apfs_dstream_t *);
bool	apfs_dstream_encode(unsigned char *, size_t, const apfs_dstream_t *);
bool	apfs_dir_stats_decode(const unsigned char *, size_t,
	    apfs_dir_stats_t *);
bool	apfs_dir_stats_encode(unsigned char *, size_t,
	    const apfs_dir_stats_t *);

bool	apfs_inode_val_decode(const unsigned char *, size_t,
	    apfs_inode_val_t *);
bool	apfs_inode_val_encode(unsigned char *, size_t,
	    const apfs_inode_val_t *);

/* The bool selects the hashed key, the volume's variant. */
bool	apfs_drec_key_decode(const unsigned char *, size_t, bool,
	    apfs_drec_key_t *);
bool	apfs_drec_key_encode(unsigned char *, size_t, const apfs_drec_key_t *);
bool	apfs_drec_val_decode(const unsigned char *, size_t, apfs_drec_val_t *);
bool	apfs_drec_val_encode(unsigned char *, size_t, const apfs_drec_val_t *);

/* Exactly one of the stream and embedded flags must be set: it selects the
 * value's layout, inline data of data_len bytes or a stream reference of
 * APFS_XATTR_STREAM_BYTES. The other flags are the caller's.
 */
bool	apfs_xattr_key_decode(const unsigned char *, size_t,
	    apfs_name_key_t *);
bool	apfs_xattr_key_encode(unsigned char *, size_t,
	    const apfs_name_key_t *);
bool	apfs_xattr_val_decode(const unsigned char *, size_t,
	    apfs_xattr_val_t *);
bool	apfs_xattr_val_encode(unsigned char *, size_t,
	    const apfs_xattr_val_t *);

bool	apfs_sibling_key_decode(const unsigned char *, size_t,
	    apfs_sibling_key_t *);
bool	apfs_sibling_key_encode(unsigned char *, size_t,
	    const apfs_sibling_key_t *);
bool	apfs_sibling_val_decode(const unsigned char *, size_t,
	    apfs_sibling_val_t *);
bool	apfs_sibling_val_encode(unsigned char *, size_t,
	    const apfs_sibling_val_t *);
/* A sibling map's value is the inode the sibling ID names. */
bool	apfs_sibling_map_val_decode(const unsigned char *, size_t, uint64_t *);
bool	apfs_sibling_map_val_encode(unsigned char *, size_t, uint64_t);
/* A data-stream ID's value is the stream's reference count. */
bool	apfs_dstream_id_val_decode(const unsigned char *, size_t, uint32_t *);
bool	apfs_dstream_id_val_encode(unsigned char *, size_t, uint32_t);

bool	apfs_extent_key_decode(const unsigned char *, size_t,
	    apfs_extent_key_t *);
bool	apfs_extent_key_encode(unsigned char *, size_t,
	    const apfs_extent_key_t *);
bool	apfs_extent_val_decode(const unsigned char *, size_t,
	    apfs_extent_val_t *);
bool	apfs_extent_val_encode(unsigned char *, size_t,
	    const apfs_extent_val_t *);
bool	apfs_pext_val_decode(const unsigned char *, size_t, apfs_pext_val_t *);
bool	apfs_pext_val_encode(unsigned char *, size_t, const apfs_pext_val_t *);

bool	apfs_snap_meta_val_decode(const unsigned char *, size_t,
	    apfs_snap_meta_val_t *);
bool	apfs_snap_meta_val_encode(unsigned char *, size_t,
	    const apfs_snap_meta_val_t *);
/* A snapshot name's key holds the name; its value is the snapshot's XID. */
bool	apfs_snap_name_key_decode(const unsigned char *, size_t,
	    apfs_name_key_t *);
bool	apfs_snap_name_key_encode(unsigned char *, size_t,
	    const apfs_name_key_t *);
bool	apfs_snap_name_val_decode(const unsigned char *, size_t, uint64_t *);
bool	apfs_snap_name_val_encode(unsigned char *, size_t, uint64_t);

/* A file-information value is a data hash of hash_size bytes. */
bool	apfs_file_info_key_decode(const unsigned char *, size_t,
	    apfs_file_info_key_t *);
bool	apfs_file_info_key_encode(unsigned char *, size_t,
	    const apfs_file_info_key_t *);
bool	apfs_file_info_val_decode(const unsigned char *, size_t,
	    apfs_file_info_val_t *);
bool	apfs_file_info_val_encode(unsigned char *, size_t,
	    const apfs_file_info_val_t *);

/*
 * Inodes (specs/design-apfs.md, "Inodes"). Inode 1 is the root's parent, 2
 * the root and 3 the private directory; numbers below 16 are reserved, and
 * in a System volume with the volume-group inode space ordinary inodes
 * start at APFS_INO_VOLGRP_FIRST.
 */
#define APFS_INO_ROOT_PARENT		1u
#define APFS_INO_ROOT			2u
#define APFS_INO_FIRST			16u
#define APFS_INO_VOLGRP_FIRST		UINT64_C(0x0800000000000000)
#define APFS_FS_FEATURE_VOLGRP_INODES	0x10u	/* optional feature */
#define APFS_FS_ROLE_SYSTEM		0x1u

/* The internal flags of [A1]'s valid mask, the closed table; 0x20 is not. */
#define APFS_INODE_FLAGS_VALID		UINT64_C(0x7fffdf)
#define APFS_INODE_HAS_UNCOMPRESSED_SIZE UINT64_C(0x40000)

#define APFS_BSD_COMPRESSED		0x00000020u	/* UF_COMPRESSED */
#define APFS_BSD_DATALESS		0x40000000u	/* SF_DATALESS */

#define APFS_MODE_TYPE_MASK		0170000u
#define APFS_MODE_FIFO			0010000u
#define APFS_MODE_CHAR			0020000u
#define APFS_MODE_DIRECTORY		0040000u
#define APFS_MODE_BLOCK			0060000u
#define APFS_MODE_FILE			0100000u
#define APFS_MODE_SYMLINK		0120000u
#define APFS_MODE_SOCKET		0140000u

/* Directory-record types: the mode type shifted right by 12. */
#define APFS_DT_FIFO			1u
#define APFS_DT_CHAR			2u
#define APFS_DT_DIRECTORY		4u
#define APFS_DT_BLOCK			6u
#define APFS_DT_FILE			8u
#define APFS_DT_SYMLINK			10u
#define APFS_DT_SOCKET			12u

/* The names of the attributes the driver consumes ("Extended attributes"). */
#define APFS_XATTR_SYMLINK		"com.apple.fs.symlink"
#define APFS_XATTR_DECMPFS		"com.apple.decmpfs"
#define APFS_XATTR_RESOURCE_FORK	"com.apple.ResourceFork"
/* Flag 0x10, undefined by [A1], which native stream attributes carry. */
#define APFS_XATTR_FLAG_UNDEFINED	0x0010u
#define APFS_XATTR_FLAGS_KNOWN		(APFS_XATTR_DATA_STREAM | \
	APFS_XATTR_DATA_EMBEDDED | APFS_XATTR_FS_OWNED | \
	APFS_XATTR_FLAG_UNDEFINED)
#define APFS_XATTR_INLINE_MAX		3804u
/* A symbolic link's target, without its NUL (E14). */
#define APFS_LINK_TARGET_MAX		1023u

/*
 * One directory record as prepare admits it: the child inode, the record's
 * type, the sibling ID or zero, and the directory holding the record, which
 * replay needs for the parent rule after preparation has moved on.
 */
typedef struct apfs_child {
	uint64_t	inode;
	uint64_t	sibling_id;
	uint64_t	parent;
	uint8_t		type;
} apfs_child_t;

/*
 * What the reader knows of the current entry once replay made it current:
 * enough for prepare to enter a directory and, later, for content to find
 * a file's storage.
 */
typedef struct apfs_current {
	uint64_t	inode;
	uint64_t	private_id;	/* owner of the file extents */
	fsimg_entry_kind_t kind;
	bool		compressed;
} apfs_current_t;

/*
 * The reader's state (read.c). apfs_volume_open(reader, apfs, window, error)
 * (volume.c), given an opened container, reads the space manager's free
 * count, opens the container object map, selects the volume the read
 * options name, validates its superblock and opens its object map, all at
 * the read XID, reading through window, at least B bytes, and sets up the
 * volume's name rule and its filesystem-tree cursors, whose windows are
 * charged when first used; on failure the caller closes the state with
 * apfs_volume_close(), which also accepts a zeroed state.
 *
 * lookup serves inode and attribute lookups, scan a directory's records.
 * form is the one comparison-form buffer, too large for the stack, that
 * preparation, replay and key derivation use in turn: a form in it is valid
 * only within the function that derived it, since the next driver callback,
 * keys among them, overwrites it.
 */
typedef struct apfs_reader {
	apfs_container_t container;
	apfs_map_t	 container_map;
	apfs_map_t	 volume_map;
	apfs_cursor_t	 lookup;
	apfs_cursor_t	 scan;
	apfs_fs_t	 fs;		/* the selected volume's superblock */
	apfs_current_t	 current;
	apfs_name_form_t form;
	apfs_name_rule_t rule;
	uint64_t	 read_xid;
	uint64_t	 free_blocks;	/* the space manager's main device */
	uint64_t	 inode_min;	/* the least ordinary inode number */
	uint32_t	 volume_index;
	uint32_t	 volume_count;	/* nonzero slots of the array */
	size_t		 label_bytes;	/* the name without its terminator */
	bool		 hashed;	/* directory records use hashed keys */
} apfs_reader_t;

extern const fsimg_reader_ops_t fsimg_apfs_reader_ops;

fsimg_status_t	apfs_volume_open(fsimg_reader_t *, apfs_reader_t *,
		    unsigned char *, fsimg_error_t *);
void		apfs_volume_close(apfs_reader_t *);

/*
 * Entries (inode.c; specs/design-apfs-driver.md, "Directories").
 * apfs_type_kind(type, kind) maps a directory-record type of the format
 * design's table to its entry kind, false for any other type.
 *
 * apfs_inode_get(apfs, child, name, inode, error) looks child's inode up
 * through the lookup cursor and validates it against its directory record:
 * the inode record, its mode type against the record's type, its flags and
 * extended fields, and for a directory the parent rule and, when name is
 * not NULL, that its name field is name. It leaves the cursor on the inode
 * record. Every member of inode is owned: the extended fields are read into
 * it, and inode->val.xfields is left empty.
 *
 * apfs_entry_load(reader, child, name, error) makes the child the current
 * entry: apfs_inode_get(), then reader->entry and its metadata, then the
 * attribute records after the inode, which give a symbolic link's target,
 * a compressed file's size and the extra-data mark. It sets apfs->current;
 * for a file it sets the entry's size and reader->initialized, and for a
 * symbolic link fsimg_reader_set_link_target() sets both to the target's
 * length.
 */
typedef struct apfs_inode {
	apfs_inode_val_t val;
	uint64_t	 size;		/* the data stream's, zero without one */
	uint32_t	 rdev;		/* bits 24-31 major, 0-23 minor */
	fsimg_entry_kind_t kind;
	bool		 has_dstream;
	bool		 has_rdev;
} apfs_inode_t;

bool		apfs_type_kind(uint8_t, fsimg_entry_kind_t *);
fsimg_status_t	apfs_inode_get(apfs_reader_t *, const apfs_child_t *,
		    const char *, apfs_inode_t *, fsimg_error_t *);
fsimg_status_t	apfs_entry_load(fsimg_reader_t *, const apfs_child_t *,
		    const char *, fsimg_error_t *);

/* Directory operations (directory.c), wired into fsimg_apfs_reader_ops. */
fsimg_status_t	apfs_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
		    fsimg_error_t *);
fsimg_status_t	apfs_replay(fsimg_reader_t *, const char *,
		    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t	apfs_keys(fsimg_reader_t *, const char *,
		    const unsigned char *, size_t, fsimg_directory_keys_t *,
		    fsimg_error_t *);
fsimg_status_t	apfs_key(fsimg_reader_t *, const char *, unsigned char *,
		    size_t *, fsimg_error_t *);

#endif /* !FSIMG_APFS_H */
