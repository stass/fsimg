/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_HFSPLUS_H
#define FSIMG_HFSPLUS_H

#include "reader.h"

/* HFS Plus and HFSX (specs/design-hfsplus.md and
 * specs/design-hfsplus-driver.md). Disk integers are big-endian unless a
 * comment says otherwise. Offsets and sizes are in bytes, block counts in
 * allocation blocks, and a name's length in UTF-16 units.
 */
#define HFSPLUS_HEADER_BYTES		512u
#define HFSPLUS_FORK_BYTES		80u
#define HFSPLUS_EXTENTS_PER_RECORD	8u
#define HFSPLUS_EXTENT_RECORD_BYTES	64u
#define HFSPLUS_NODE_DESCRIPTOR_BYTES	14u
#define HFSPLUS_BTREE_HEADER_BYTES	106u
#define HFSPLUS_EKEY_BYTES		12u
#define HFSPLUS_EKEY_LENGTH		10u	/* its keyLength, always this */
#define HFSPLUS_CKEY_FIXED_BYTES	8u	/* keyLength, parent, nameLength */
#define HFSPLUS_AKEY_FIXED_BYTES	14u	/* before the attribute's name */
#define HFSPLUS_FOLDER_BYTES		88u
#define HFSPLUS_FILE_BYTES		248u
#define HFSPLUS_THREAD_FIXED_BYTES	10u
#define HFSPLUS_ATTR_INLINE_FIXED_BYTES	16u
#define HFSPLUS_ATTR_FORK_BYTES		88u
#define HFSPLUS_ATTR_EXTENTS_BYTES	72u
#define HFSPLUS_JIB_BYTES		180u
#define HFSPLUS_JOURNAL_BYTES		44u
#define HFSPLUS_MDB_BYTES		130u	/* through the embedded extent */
#define HFSPLUS_CMPF_BYTES		16u
#define HFSPLUS_RSRC_BYTES		16u
#define HFSPLUS_RSRC_TABLE_BYTES	8u
#define HFSPLUS_CHUNK_PAIR_BYTES	8u
#define HFSPLUS_CHUNK_WORD_BYTES	4u
#define HFSPLUS_CHUNK_BYTES		65536u	/* a chunk's decoded length */
#define HFSPLUS_CHUNK_STORED_MAX	69632u	/* its stored bound */

/* A name holds at most 255 units; each is at most three UTF-8 bytes, a
 * surrogate pair four for its two.
 */
#define HFSPLUS_NAME_UNITS_MAX		255u
#define HFSPLUS_NAME_UTF8_MAX		765u
#define HFSPLUS_ATTR_NAME_UNITS_MAX	127u
/* A namespace key holds at most a name's units, big-endian. */
#define HFSPLUS_KEY_BYTES_MAX		510u
/* The most units one unit decomposes into. */
#define HFSPLUS_UNIT_DECOMP_MAX		4u

/* The longest symbolic-link target the reader holds evidence for. */
#define HFSPLUS_LINK_TARGET_MAX		1023u

/* The catalog flags and BSD owner flag the reader acts on. */
#define HFSPLUS_FLAG_ATTRIBUTES		0x0004u
#define HFSPLUS_FLAG_FOLDER_COUNT	0x0010u
#define HFSPLUS_OWNER_COMPRESSED	0x20u

/* Catalog record types, the first two bytes of a leaf record's data. */
#define HFSPLUS_RECORD_FOLDER		1u
#define HFSPLUS_RECORD_FILE		2u
#define HFSPLUS_RECORD_FOLDER_THREAD	3u
#define HFSPLUS_RECORD_FILE_THREAD	4u

/* Attribute record types, the first four bytes of a leaf record's data. */
#define HFSPLUS_ATTR_INLINE		0x10u
#define HFSPLUS_ATTR_FORK		0x20u
#define HFSPLUS_ATTR_EXTENTS		0x30u

/* One run of allocation blocks. */
typedef struct hfsplus_extent {
	uint32_t	 start;
	uint32_t	 count;
} hfsplus_extent_t;

typedef struct hfsplus_fork {
	uint64_t		 logical_size;
	uint32_t		 total_blocks;
	hfsplus_extent_t	 extents[HFSPLUS_EXTENTS_PER_RECORD];
} hfsplus_fork_t;

typedef struct hfsplus_header {
	uint16_t	 signature;
	uint16_t	 version;
	uint32_t	 attributes;
	uint32_t	 last_mounted_version;
	uint32_t	 journal_info_block;
	uint32_t	 create_date;
	uint32_t	 modify_date;
	uint32_t	 backup_date;
	uint32_t	 checked_date;
	uint32_t	 file_count;
	uint32_t	 folder_count;
	uint32_t	 block_size;
	uint32_t	 total_blocks;
	uint32_t	 free_blocks;
	uint32_t	 next_allocation;
	uint32_t	 rsrc_clump_size;
	uint32_t	 data_clump_size;
	uint32_t	 next_catalog_id;
	uint32_t	 write_count;
	uint64_t	 encodings_bitmap;
	uint32_t	 finder_info[8];
	hfsplus_fork_t	 allocation_file;
	hfsplus_fork_t	 extents_file;
	hfsplus_fork_t	 catalog_file;
	hfsplus_fork_t	 attributes_file;
	hfsplus_fork_t	 startup_file;
} hfsplus_header_t;

/* A node's 14-byte descriptor. */
typedef struct hfsplus_node {
	uint32_t	 forward_link;
	uint32_t	 backward_link;
	int8_t		 kind;		/* -1 leaf, 0 index, 1 header, 2 map */
	uint8_t		 height;
	uint16_t	 records;
} hfsplus_node_t;

/* The 106-byte header record of a B-tree's node 0. */
typedef struct hfsplus_btree {
	uint32_t	 root_node;
	uint32_t	 leaf_records;
	uint32_t	 first_leaf_node;
	uint32_t	 last_leaf_node;
	uint32_t	 total_nodes;
	uint32_t	 free_nodes;
	uint32_t	 clump_size;
	uint32_t	 attributes;
	uint16_t	 tree_depth;
	uint16_t	 node_size;
	uint16_t	 max_key_length;
	uint8_t		 btree_type;
	uint8_t		 key_compare_type;
} hfsplus_btree_t;

/* The key of an extents overflow record. */
typedef struct hfsplus_ekey {
	uint32_t	 file_id;
	uint32_t	 start_block;	/* logical block within the fork */
	uint8_t		 fork_type;	/* 0x00 data, 0xff resource */
} hfsplus_ekey_t;

/* A catalog key as a record stores it. name borrows the big-endian units from
 * the decoded buffer and is valid as long as that buffer's contents are.
 */
typedef struct hfsplus_ckey {
	const unsigned char	*name;
	uint32_t		 parent;
	uint16_t		 units;
} hfsplus_ckey_t;

/* The 16-byte BSD information of a folder or file record. */
typedef struct hfsplus_bsd {
	uint32_t	 owner_id;
	uint32_t	 group_id;
	uint32_t	 special;
	uint16_t	 file_mode;
	uint8_t		 admin_flags;
	uint8_t		 owner_flags;
} hfsplus_bsd_t;

/* The five dates of a folder or file record, seconds since 1904. */
typedef struct hfsplus_dates {
	uint32_t	 created;
	uint32_t	 content_modified;
	uint32_t	 attribute_modified;
	uint32_t	 accessed;
	uint32_t	 backed_up;
} hfsplus_dates_t;

typedef struct hfsplus_folder {
	hfsplus_dates_t	 dates;
	hfsplus_bsd_t	 bsd;
	unsigned char	 finder_info[32];	/* with the extended information */
	uint32_t	 valence;
	uint32_t	 id;
	uint32_t	 text_encoding;
	uint32_t	 folder_count;
	uint16_t	 flags;
} hfsplus_folder_t;

typedef struct hfsplus_file {
	hfsplus_dates_t	 dates;
	hfsplus_bsd_t	 bsd;
	hfsplus_fork_t	 data_fork;
	hfsplus_fork_t	 resource_fork;
	unsigned char	 finder_info[32];	/* with the extended information */
	uint32_t	 newest_link;	/* reserved word, on an indirect node */
	uint32_t	 id;
	uint32_t	 text_encoding;
	uint16_t	 flags;
} hfsplus_file_t;

/* A folder or file thread. name borrows as in hfsplus_ckey_t. */
typedef struct hfsplus_thread {
	const unsigned char	*name;
	uint32_t		 parent;
	uint16_t		 type;
	uint16_t		 units;
} hfsplus_thread_t;

/* An attribute key as a record stores it. name borrows as in hfsplus_ckey_t. */
typedef struct hfsplus_akey {
	const unsigned char	*name;
	uint32_t		 file_id;
	uint32_t		 start_block;
	uint16_t		 units;
} hfsplus_akey_t;

/* An attribute record. type selects the meaningful fields: data_size and
 * data_offset, the value's position in the decoded buffer, for
 * HFSPLUS_ATTR_INLINE; fork for HFSPLUS_ATTR_FORK; extents for
 * HFSPLUS_ATTR_EXTENTS. Under any other type none is.
 */
typedef struct hfsplus_attr {
	hfsplus_fork_t		 fork;
	hfsplus_extent_t	 extents[HFSPLUS_EXTENTS_PER_RECORD];
	uint32_t		 type;
	uint32_t		 data_size;
	uint32_t		 data_offset;
} hfsplus_attr_t;

/* The journal info block; its device signature is never interpreted. */
typedef struct hfsplus_jib {
	uint64_t	 offset;
	uint64_t	 size;
	uint32_t	 flags;
} hfsplus_jib_t;

/* The journal header, in the host order of the journal's creator. */
typedef struct hfsplus_journal {
	uint64_t	 start;
	uint64_t	 end;
	uint64_t	 size;
	uint32_t	 blhdr_size;
	uint32_t	 jhdr_size;
	bool		 little_endian;
} hfsplus_journal_t;

/* The fields of the wrapper's master directory block that locate the
 * embedded volume.
 */
typedef struct hfsplus_mdb {
	uint32_t	 allocation_block_size;	/* drAlBlkSiz */
	uint16_t	 signature;
	uint16_t	 first_block_sector;	/* drAlBlSt */
	uint16_t	 embedded_signature;
	uint16_t	 embedded_start;
	uint16_t	 embedded_count;
} hfsplus_mdb_t;

/* The header of a compression attribute's value. */
typedef struct hfsplus_cmpf {
	uint64_t	 uncompressed_size;
	uint32_t	 type;
} hfsplus_cmpf_t;

/* The classic resource file header at the start of a type 4 resource fork. */
typedef struct hfsplus_rsrc {
	uint32_t	 data_offset;
	uint32_t	 map_offset;
	uint32_t	 data_length;
	uint32_t	 map_length;
} hfsplus_rsrc_t;

/* The eight bytes at a type 4 resource fork's data offset. */
typedef struct hfsplus_rsrc_table {
	uint32_t	 resource_length;
	uint32_t	 chunk_count;
} hfsplus_rsrc_table_t;

/* One entry of a type 4 chunk table; offset counts from the chunk count. */
typedef struct hfsplus_chunk {
	uint32_t	 offset;
	uint32_t	 length;
} hfsplus_chunk_t;

/* A volume's name order. */
typedef enum hfsplus_compare {
	HFSPLUS_COMPARE_FOLDED,
	HFSPLUS_COMPARE_BINARY
} hfsplus_compare_t;

/* Record decoders (codec.c). Each takes bytes and their length, returns false
 * when the length or a field that frames the record is wrong, and then leaves
 * its output untouched. A decoder reads nothing past length. The false return
 * is FSIMG_ECORRUPT in every caller. Field values are otherwise the caller's
 * to validate.
 *
 * length means one of three things:
 * - The exact record: the extents, folder, file and thread records and the
 *   fork and extents attribute records are refused at any other length. An
 *   inline attribute record's value must fit, and bytes after it are accepted.
 * - A minimum buffer: the header, fork data, node descriptor, B-tree header,
 *   journal info block, journal header, master directory block, catalog type,
 *   compression header, resource header and chunk table header read a fixed
 *   prefix of what may be a longer buffer.
 * - The span a key must fit: the three key decoders, whose own length field
 *   says how many bytes the key takes.
 */
bool hfsplus_header_decode(const unsigned char *, size_t, hfsplus_header_t *);
bool hfsplus_fork_decode(const unsigned char *, size_t, hfsplus_fork_t *);
bool hfsplus_extents_decode(const unsigned char *, size_t,
    hfsplus_extent_t *);
bool hfsplus_node_decode(const unsigned char *, size_t, hfsplus_node_t *);
bool hfsplus_btree_decode(const unsigned char *, size_t, hfsplus_btree_t *);
bool hfsplus_ekey_decode(const unsigned char *, size_t, hfsplus_ekey_t *);
bool hfsplus_ckey_decode(const unsigned char *, size_t, hfsplus_ckey_t *);
bool hfsplus_catalog_type(const unsigned char *, size_t, uint16_t *);
bool hfsplus_folder_decode(const unsigned char *, size_t, hfsplus_folder_t *);
bool hfsplus_file_decode(const unsigned char *, size_t, hfsplus_file_t *);
bool hfsplus_thread_decode(const unsigned char *, size_t, hfsplus_thread_t *);
bool hfsplus_akey_decode(const unsigned char *, size_t, hfsplus_akey_t *);
bool hfsplus_attr_decode(const unsigned char *, size_t, hfsplus_attr_t *);
bool hfsplus_jib_decode(const unsigned char *, size_t, hfsplus_jib_t *);
bool hfsplus_journal_decode(const unsigned char *, size_t,
    hfsplus_journal_t *);
/* Whether the buffer starts with the older journal magic in either byte order:
 * a header form the reader refuses rather than decodes.
 */
bool hfsplus_journal_is_older_form(const unsigned char *, size_t);
bool hfsplus_mdb_decode(const unsigned char *, size_t, hfsplus_mdb_t *);
bool hfsplus_cmpf_decode(const unsigned char *, size_t, hfsplus_cmpf_t *);
bool hfsplus_rsrc_decode(const unsigned char *, size_t, hfsplus_rsrc_t *);
bool hfsplus_rsrc_table_decode(const unsigned char *, size_t,
    hfsplus_rsrc_table_t *);
/* hfsplus_chunk_pair_decode() and hfsplus_chunk_word_decode(), given (table,
 * table_bytes, index, out), decode entry `index` of a chunk table, false when
 * it lies outside table_bytes. table is the first entry: for a
 * pair, of a type 4 table, the byte after hfsplus_rsrc_table_t; for a word, of
 * a type 8 or 12 table, the fork's start.
 */
bool hfsplus_chunk_pair_decode(const unsigned char *, size_t, uint32_t,
    hfsplus_chunk_t *);
bool hfsplus_chunk_word_decode(const unsigned char *, size_t, uint32_t,
    uint32_t *);

/* Per-unit lookups (unicode.c). hfsplus_fold_unit() is the folded
 * comparator's value, zero for an ignorable unit, and
 * hfsplus_combining_class() a unit's class. hfsplus_unit_decompose() stores
 * the unit's decomposition, the unit itself when it has none, in at most
 * HFSPLUS_UNIT_DECOMP_MAX units and returns their count.
 */
uint16_t hfsplus_fold_unit(uint16_t);
uint8_t hfsplus_combining_class(uint16_t);
size_t hfsplus_unit_decompose(uint16_t, uint16_t *);

/* hfsplus_decompose(input, count, output, capacity, written) decomposes
 * host-order units and sorts the marks. It returns false when the result
 * exceeds capacity. Input and output do not overlap.
 */
bool hfsplus_decompose(const uint16_t *, size_t, uint16_t *, size_t,
    size_t *);

/* The comparators take stored names as big-endian units and their counts,
 * and return a negative, zero or positive value as the first name sorts
 * before, equal to or after the second.
 */
int hfsplus_name_compare(hfsplus_compare_t, const unsigned char *, size_t,
    const unsigned char *, size_t);
int hfsplus_ckey_compare(hfsplus_compare_t, const hfsplus_ckey_t *,
    const hfsplus_ckey_t *);

/* hfsplus_name_to_path(name, units, path, path_bytes, error) writes a stored
 * name's image path component, at most HFSPLUS_NAME_UTF8_MAX bytes without a
 * terminator. An empty name, one over 255 units or one with an unpaired
 * surrogate is FSIMG_ECORRUPT.
 */
fsimg_status_t hfsplus_name_to_path(const unsigned char *, size_t, char *,
    size_t *, fsimg_error_t *);
/* Apply the format design's name table to a stored name: FSIMG_ECORRUPT as
 * above, FSIMG_EUNSUPPORTED for "." and ".." and for a name its image path
 * does not map back to.
 */
fsimg_status_t hfsplus_name_admit(const unsigned char *, size_t,
    fsimg_error_t *);
/* Namespace keys, written to at most HFSPLUS_KEY_BYTES_MAX bytes. An admitted
 * stored name always has one; any other that exceeds a name's bounds is
 * FSIMG_ECORRUPT. A query component that is not valid UTF-8 or decomposes
 * past 255 units is FSIMG_EINVAL.
 */
fsimg_status_t hfsplus_stored_key(hfsplus_compare_t, const unsigned char *,
    size_t, unsigned char *, size_t *, fsimg_error_t *);
fsimg_status_t hfsplus_query_key(hfsplus_compare_t, const char *, size_t,
    unsigned char *, size_t *, fsimg_error_t *);

/* Header identity and boot area (specs/design-hfsplus.md, Probe identity). */
#define HFSPLUS_PROBE_BYTES		1536u
#define HFSPLUS_BOOT_BYTES		1024u
#define HFSPLUS_SIGNATURE_PLUS		0x482bu		/* "H+" */
#define HFSPLUS_SIGNATURE_X		0x4858u		/* "HX" */
#define HFSPLUS_SIGNATURE_WRAPPER	0x4244u		/* "BD" */
#define HFSPLUS_VERSION_PLUS		4u
#define HFSPLUS_VERSION_X		5u

/* The public variants of the descriptor, in its order. */
#define HFSPLUS_VARIANT_PLUS		0u
#define HFSPLUS_VARIANT_X		1u

/* Special files' node IDs. */
#define HFSPLUS_ID_ROOT_PARENT		1u
#define HFSPLUS_ID_ROOT			2u
#define HFSPLUS_ID_EXTENTS		3u
#define HFSPLUS_ID_CATALOG		4u
#define HFSPLUS_ID_ALLOCATION		6u
#define HFSPLUS_ID_STARTUP		7u
#define HFSPLUS_ID_ATTRIBUTES		8u
#define HFSPLUS_ID_FIRST_USER		16u

#define HFSPLUS_FORK_DATA		0x00u
#define HFSPLUS_FORK_RESOURCE		0xffu

/* Node kinds, as the descriptor's signed byte stores them. */
#define HFSPLUS_NODE_LEAF		(-1)
#define HFSPLUS_NODE_INDEX		0
#define HFSPLUS_NODE_HEADER		1
#define HFSPLUS_NODE_MAP		2

#define HFSPLUS_BTREE_DEPTH_MAX		16u

/* The B-tree header's bad-close flag; an accepted one makes the volume
 * unclean in the information record.
 */
#define HFSPLUS_BTREE_BAD_CLOSE		0x01u

/* The four format-owned children of the root as stored units, big-endian:
 * the one table directory.c recognizes them by and write-plan.c refuses
 * them by (specs/design-hfsplus.md, Private directories and format-owned
 * files). Comparison is by exact stored units.
 */
typedef enum hfsplus_owned {
	HFSPLUS_OWNED_PRIVATE_DATA,
	HFSPLUS_OWNED_PRIVATE_DIRECTORY,
	HFSPLUS_OWNED_JOURNAL,
	HFSPLUS_OWNED_JOURNAL_INFO,
	HFSPLUS_OWNED_COUNT
} hfsplus_owned_t;

typedef struct hfsplus_owned_name {
	const unsigned char	*units;
	uint16_t		 count;
} hfsplus_owned_name_t;

#define HFSPLUS_U(c)	"\0" c
static const hfsplus_owned_name_t hfsplus_owned_names[HFSPLUS_OWNED_COUNT]
    FSIMG_UNUSED = {
	[HFSPLUS_OWNED_PRIVATE_DATA] = {
		(const unsigned char *)"\0\0\0\0\0\0\0\0"
		    HFSPLUS_U("H") HFSPLUS_U("F") HFSPLUS_U("S") HFSPLUS_U("+")
		    HFSPLUS_U(" ") HFSPLUS_U("P") HFSPLUS_U("r") HFSPLUS_U("i")
		    HFSPLUS_U("v") HFSPLUS_U("a") HFSPLUS_U("t") HFSPLUS_U("e")
		    HFSPLUS_U(" ") HFSPLUS_U("D") HFSPLUS_U("a") HFSPLUS_U("t")
		    HFSPLUS_U("a"), 21
	},
	[HFSPLUS_OWNED_PRIVATE_DIRECTORY] = {
		(const unsigned char *)
		    HFSPLUS_U(".") HFSPLUS_U("H") HFSPLUS_U("F") HFSPLUS_U("S")
		    HFSPLUS_U("+") HFSPLUS_U(" ") HFSPLUS_U("P") HFSPLUS_U("r")
		    HFSPLUS_U("i") HFSPLUS_U("v") HFSPLUS_U("a") HFSPLUS_U("t")
		    HFSPLUS_U("e") HFSPLUS_U(" ") HFSPLUS_U("D") HFSPLUS_U("i")
		    HFSPLUS_U("r") HFSPLUS_U("e") HFSPLUS_U("c") HFSPLUS_U("t")
		    HFSPLUS_U("o") HFSPLUS_U("r") HFSPLUS_U("y") HFSPLUS_U(" ")
		    HFSPLUS_U("D") HFSPLUS_U("a") HFSPLUS_U("t") HFSPLUS_U("a")
		    HFSPLUS_U("\r"), 29
	},
	[HFSPLUS_OWNED_JOURNAL] = {
		(const unsigned char *)
		    HFSPLUS_U(".") HFSPLUS_U("j") HFSPLUS_U("o") HFSPLUS_U("u")
		    HFSPLUS_U("r") HFSPLUS_U("n") HFSPLUS_U("a") HFSPLUS_U("l"),
		8
	},
	[HFSPLUS_OWNED_JOURNAL_INFO] = {
		(const unsigned char *)
		    HFSPLUS_U(".") HFSPLUS_U("j") HFSPLUS_U("o") HFSPLUS_U("u")
		    HFSPLUS_U("r") HFSPLUS_U("n") HFSPLUS_U("a") HFSPLUS_U("l")
		    HFSPLUS_U("_") HFSPLUS_U("i") HFSPLUS_U("n") HFSPLUS_U("f")
		    HFSPLUS_U("o") HFSPLUS_U("_") HFSPLUS_U("b") HFSPLUS_U("l")
		    HFSPLUS_U("o") HFSPLUS_U("c") HFSPLUS_U("k"), 19
	}
};
#undef HFSPLUS_U

/* The volume's allocation geometry, derived once at open: V bytes, N
 * allocation blocks of B bytes, r reserved leading blocks and t reserved
 * trailing ones. Forks occupy only blocks r to N - t, the content domain.
 */
typedef struct hfsplus_geometry {
	uint64_t	 volume_bytes;	/* V, past the slice when it is truncated */
	uint64_t	 slice_bytes;	/* input length at open; only caps leaves */
	uint32_t	 block_size;	/* B */
	uint32_t	 blocks;	/* N */
	uint32_t	 first_block;	/* r */
	uint32_t	 end_block;	/* N - t, or N on a truncated volume */
} hfsplus_geometry_t;

/* Which on-disk keys a tree holds: a closed set btree.c dispatches on. */
typedef enum hfsplus_tree_kind {
	HFSPLUS_TREE_EXTENTS,
	HFSPLUS_TREE_CATALOG,
	HFSPLUS_TREE_ATTRIBUTES
} hfsplus_tree_kind_t;

/* The search keys of the named trees own their big-endian units, so nothing a
 * search reads can change the key it is looking for. A key is filled only by
 * hfsplus_catalog_key_init() or hfsplus_attribute_key_init().
 */
typedef struct hfsplus_catalog_search {
	unsigned char	 name[2 * HFSPLUS_NAME_UNITS_MAX];
	uint32_t	 parent;
	uint16_t	 units;
} hfsplus_catalog_search_t;

typedef struct hfsplus_attribute_search {
	unsigned char	 name[2 * HFSPLUS_ATTR_NAME_UNITS_MAX];
	uint32_t	 file_id;
	uint32_t	 start_block;
	uint16_t	 units;
} hfsplus_attribute_search_t;

/* A search key; the member is the one the tree's kind selects. */
typedef union hfsplus_key {
	hfsplus_ekey_t			 extents;
	hfsplus_catalog_search_t	 catalog;
	hfsplus_attribute_search_t	 attribute;
} hfsplus_key_t;

/* One tree: the fork index its nodes are read through, its validated header
 * record and the buffer of one node that searches reuse. A tree is absent while
 * present is false, as an attributes tree can be, and then nothing else in it
 * is meaningful. Both pointers are owned and freed by hfsplus_tree_free().
 * max_leaves is the most leaves a cursor enters from one position: totalNodes,
 * or the nodes the slice can hold when that is fewer, since a leaf is read whole
 * from the slice.
 */
typedef struct hfsplus_tree {
	fsimg_reader_t		*reader;
	fsimg_extent_index_t	*fork;
	unsigned char		*search_node;
	hfsplus_btree_t		 header;
	hfsplus_tree_kind_t	 kind;
	hfsplus_compare_t	 compare;	/* catalog name order */
	uint32_t		 max_leaves;
	bool			 present;
} hfsplus_tree_t;

/* A position in a tree's leaf chain, whose current node is held in the
 * borrowed buffer of one node. Set by a search and moved by
 * hfsplus_cursor_next(). While valid is false the cursor stands on nothing:
 * the tree holds no record at or after the position. leaves counts the leaves
 * this position has entered, the visit bound on a looping sibling chain.
 */
typedef struct hfsplus_cursor {
	hfsplus_tree_t	*tree;
	unsigned char	*buffer;
	hfsplus_node_t	 node;
	uint32_t	 number;
	uint32_t	 leaves;
	uint16_t	 record;
	bool		 valid;
} hfsplus_cursor_t;

/* A record the cursor stands on, borrowed from its buffer: key is the whole
 * key field with its two-byte keyLength, and data the bytes from the next even
 * offset to the record's end.
 */
typedef struct hfsplus_record {
	const unsigned char	*key;
	const unsigned char	*data;
	size_t			 key_bytes;
	size_t			 data_bytes;
} hfsplus_record_t;

/* What the info block says of the journal; in the order of the "journal"
 * detail's values.
 */
typedef enum hfsplus_journal_state {
	HFSPLUS_JOURNAL_NONE,
	HFSPLUS_JOURNAL_CLEAN,
	HFSPLUS_JOURNAL_UNINITIALIZED
} hfsplus_journal_state_t;

/* Bits of a child's flags byte. */
#define HFSPLUS_CHILD_OWNER_MODE	0x01u	/* owner and mode are valid */
#define HFSPLUS_CHILD_FOLDER_COUNT	0x02u	/* folder_count is recorded */
#define HFSPLUS_CHILD_FLAGS		0x03u

/* One admitted child, what its 60-byte prepared payload encodes (the driver
 * design's Directories and names): everything replay delivers, without reading
 * a node, and the location of the effective record, the indirect node's for a
 * hard-link stub, for what must read it again.
 */
typedef struct hfsplus_child {
	uint64_t		 size;
	uint32_t		 id;		/* the effective record's node ID */
	uint32_t		 leaf;		/* its leaf node */
	uint32_t		 owner;
	uint32_t		 group;
	uint32_t		 special;	/* a device's dev_t, or zero */
	uint32_t		 valence;	/* a folder's children */
	uint32_t		 folder_count;	/* a folder's child folders */
	hfsplus_dates_t		 dates;
	fsimg_entry_kind_t	 kind;
	uint16_t		 record;	/* the record's index in leaf */
	uint16_t		 mode;		/* the permission bits */
	uint8_t			 flags;		/* HFSPLUS_CHILD_* */
	uint8_t			 compression;	/* the type, zero for none */
	bool			 extra_data;
} hfsplus_child_t;

/* The current entry's compressed content, open from its first resolve until the
 * walker advances. A fork's table is described in fork offsets: entries start at
 * table_offset, a chunk's offset counts from chunk_base, and every chunk lies in
 * [payload_start, payload_end). All but open are zero for an inline payload.
 */
typedef struct hfsplus_compressed {
	uint64_t	 chunk_base;
	uint64_t	 table_offset;
	uint64_t	 payload_start;
	uint64_t	 payload_end;
	uint64_t	 chunks;
	bool		 open;
} hfsplus_compressed_t;

typedef struct hfsplus_window hfsplus_window_t;

/* The driver's state behind fsimg_reader_t. */
typedef struct fsimg_hfsplus_reader {
	hfsplus_header_t	 header;
	hfsplus_geometry_t	 geometry;
	hfsplus_tree_t		 extents;
	hfsplus_tree_t		 catalog;
	hfsplus_tree_t		 attributes;
	/* The node a directory scan keeps while searches use catalog's own. */
	unsigned char		*catalog_scan_node;
	fsimg_extent_index_t	*bitmap;
	hfsplus_folder_t	 root;
	/* Set for an embedded volume; decides the journal table's wrapped row. */
	bool			 wrapped;
	/* The journal, for the root's format-owned files; its blocks are zero
	 * without one. journal_info_block is meaningful under the same state.
	 */
	hfsplus_journal_state_t	 journal;
	uint32_t		 journal_info_block;
	uint32_t		 journal_start;
	uint32_t		 journal_blocks;
	/* The file hard-link data folder, looked up by the first record carrying
	 * a stub's Finder codes. private_data_id and private_data_date are
	 * meaningful under private_data_present.
	 */
	bool			 private_data_known;
	bool			 private_data_present;
	uint32_t		 private_data_id;
	uint32_t		 private_data_date;
	/* The entry replay made current, and the index of the fork holding its
	 * content, built on the first resolve and released when the walker
	 * advances: the data fork, or a compressed file's resource fork.
	 */
	hfsplus_child_t		 current;
	fsimg_extent_index_t	*file;
	hfsplus_compressed_t	 compressed;
	/* The decode window, from the first compressed file resolved to close. */
	hfsplus_window_t	*window;
	/* The root's name by the display mapping, without a terminator when
	 * label_valid: a refused name leaves it absent.
	 */
	bool			 label_valid;
	size_t			 label_bytes;
	char			 label[HFSPLUS_NAME_UTF8_MAX];
} fsimg_hfsplus_reader_t;

/* Driver plumbing (read.c). */
extern const fsimg_reader_ops_t fsimg_hfsplus_reader_ops;
fsimg_hfsplus_reader_t *hfsplus_state(const fsimg_reader_t *);
fsimg_status_t hfsplus_corrupt(fsimg_error_t *, const char *);
fsimg_status_t hfsplus_unsupported(fsimg_error_t *, const char *);

/* Identity and the header fields the probe checks (descriptor.c). Open
 * repeats them, since a caller assuming the identity runs no probe.
 * hfsplus_boot_area_valid() tests the 1024 bytes before the header.
 * hfsplus_header_check() checks the signature's version pairing, blockSize
 * and totalBlocks.
 */
bool hfsplus_boot_area_valid(const unsigned char *);
fsimg_status_t hfsplus_header_check(const hfsplus_header_t *, fsimg_error_t *);

/* The fork resolver (fork.c), the only code that interprets an extent
 * descriptor or an overflow record.
 *
 * hfsplus_fork_index(reader, file_id, fork_type, fork, index, records, error)
 * builds a new extent index of the fork's blocks, inline then overflow, in
 * logical order, NULL for a fork of zero blocks. *records, which verification
 * sums and open passes NULL for, is the number of overflow records it consumed.
 * Every failure leaves no index and no records.
 * A fork that needs overflow requires the extents tree to be open. The extents
 * tree's own fork (HFSPLUS_ID_EXTENTS) must sum to totalBlocks inline, so
 * resolving it never reads it.
 */
fsimg_status_t hfsplus_fork_index(fsimg_reader_t *, uint32_t, uint8_t,
    const hfsplus_fork_t *, fsimg_extent_index_t **, uint64_t *,
    fsimg_error_t *);
/* Require a fork to be exactly one extent in the content domain, inline. */
fsimg_status_t hfsplus_fork_one_extent(fsimg_reader_t *,
    const hfsplus_fork_t *, hfsplus_extent_t *, fsimg_error_t *);
/* Read fork bytes [offset, offset + length) through an index. */
fsimg_status_t hfsplus_fork_read(fsimg_reader_t *, fsimg_extent_index_t *,
    uint64_t, void *, size_t, fsimg_error_t *);

/* The B-tree reader (btree.c), the only code that interprets node framing.
 *
 * hfsplus_tree_open(reader, tree, kind, id, fork, error) sets every field of
 * the tree but a catalog's compare, which is the volume's choice from the
 * header: it resolves the data fork of file `id`, reads node 0, checks it
 * against the tree parameter table and that totalNodes * M is the fork's
 * logical_size, and allocates the search buffer. A fork of zero blocks leaves
 * the tree absent, which is not a failure; the caller decides whether it may
 * be.
 * hfsplus_tree_free() releases the buffer and the fork index of a tree in any
 * state.
 */
fsimg_status_t hfsplus_tree_open(fsimg_reader_t *, hfsplus_tree_t *,
    hfsplus_tree_kind_t, uint32_t, const hfsplus_fork_t *, fsimg_error_t *);
void hfsplus_tree_free(hfsplus_tree_t *);

/* Read and validate node `number` into a buffer of the tree's node size: its
 * descriptor must show the given kind and height and its offset table must be
 * well formed. hfsplus_node_record() then gives record `index`, below
 * node->records, with the length the validated table states.
 */
fsimg_status_t hfsplus_node_read(hfsplus_tree_t *, uint32_t, unsigned char *,
    int, unsigned, hfsplus_node_t *, fsimg_error_t *);
void hfsplus_node_record(const hfsplus_tree_t *, const unsigned char *,
    uint16_t, const unsigned char **, size_t *);

/* Attribute keys in the tree's order, for a caller reading forward. */
int hfsplus_akey_compare(const hfsplus_akey_t *, const hfsplus_akey_t *);

/* hfsplus_cursor_init(cursor, tree, buffer) binds a cursor to a tree and a
 * buffer of its node size. It stands on nothing until a search.
 *
 * hfsplus_btree_search() is the lower bound, the one search: it positions the
 * cursor on the first leaf record whose key is at or above the key, crossing
 * into the next leaf when the one reached holds none, or leaves it invalid
 * when no record is. Height strictly decreases on the descent, so it ends
 * within the tree's depth.
 *
 * hfsplus_btree_find_equivalent() is that search, with *found saying whether
 * the record reached ranks equal to the key in the tree's order. On a folded
 * catalog that is a name folding equal, possibly another spelling, which is
 * what a path component selects.
 *
 * hfsplus_catalog_find_exact(catalog, key, record, found, error) is the one
 * lookup of exactly the stored units, for the format-owned files and threads:
 * *found says whether the record reached carries them. It reads into the
 * catalog's search buffer, from which *record borrows until the next search.
 * hfsplus_catalog_locate() is that lookup through the caller's cursor, which
 * then stands on the record for a caller that keeps its location.
 *
 * hfsplus_catalog_key_init(key, parent, name, units, error) and
 * hfsplus_attribute_key_init(key, file_id, start_block, name, units, error)
 * copy a stored name into a search key, so the name may then be overwritten.
 * name may be NULL with no units. A name over its tree's bound is
 * FSIMG_ECORRUPT and leaves the key untouched.
 *
 * hfsplus_cursor_seek(cursor, leaf, record, error) stands the cursor on a record
 * by a location an earlier position gave: the leaf is read and validated again,
 * and a record index past its last is FSIMG_ECORRUPT.
 *
 * hfsplus_cursor_next() moves to the next record, along fLink past a leaf's
 * last, requiring each next leaf's bLink to name the one left and refusing more
 * leaves than the tree's max_leaves, so a looping chain ends whatever the caller
 * checks. The caller reading forward checks key order. hfsplus_cursor_record()
 * frames the current record.
 */
void hfsplus_cursor_init(hfsplus_cursor_t *, hfsplus_tree_t *, unsigned char *);
fsimg_status_t hfsplus_btree_search(hfsplus_cursor_t *, const hfsplus_key_t *,
    fsimg_error_t *);
fsimg_status_t hfsplus_btree_find_equivalent(hfsplus_cursor_t *,
    const hfsplus_key_t *, bool *, fsimg_error_t *);
fsimg_status_t hfsplus_catalog_find_exact(hfsplus_tree_t *,
    const hfsplus_key_t *, hfsplus_record_t *, bool *, fsimg_error_t *);
fsimg_status_t hfsplus_catalog_locate(hfsplus_cursor_t *,
    const hfsplus_key_t *, hfsplus_record_t *, bool *, fsimg_error_t *);
fsimg_status_t hfsplus_catalog_key_init(hfsplus_key_t *, uint32_t,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t hfsplus_attribute_key_init(hfsplus_key_t *, uint32_t, uint32_t,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t hfsplus_cursor_seek(hfsplus_cursor_t *, uint32_t, uint16_t,
    fsimg_error_t *);
fsimg_status_t hfsplus_cursor_next(hfsplus_cursor_t *, fsimg_error_t *);
fsimg_status_t hfsplus_cursor_record(const hfsplus_cursor_t *,
    hfsplus_record_t *, fsimg_error_t *);

/* Open (volume.c): the fixed step order of the driver design, with the journal
 * step apart. hfsplus_journal_open() applies the info block's flags table to
 * the open catalog. It is external, and the reader's wrapped field settable,
 * only so tests can reach the table's wrapped row, which open cannot while it
 * refuses every wrapper at its first step.
 */
fsimg_status_t hfsplus_volume_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
fsimg_status_t hfsplus_journal_open(fsimg_reader_t *, fsimg_error_t *);

/* What the stub tests decide of a file record. */
typedef enum hfsplus_link {
	HFSPLUS_LINK_NONE,
	HFSPLUS_LINK_FILE,
	HFSPLUS_LINK_DIRECTORY
} hfsplus_link_t;

/* A validated folder or file record: kind says which of the two records is
 * meaningful, zero for a stub, whose fileMode is not its kind.
 */
typedef struct hfsplus_primary {
	hfsplus_folder_t	 folder;
	hfsplus_file_t		 file;
	fsimg_entry_kind_t	 kind;
	hfsplus_link_t		 link;
} hfsplus_primary_t;

/* Catalog records and links (catalog.c).
 *
 * hfsplus_record_validate() checks a primary record against the format design's
 * record, flag and kind tables, a file record's stub tests first. A thread or a
 * record of another type is FSIMG_ECORRUPT.
 *
 * hfsplus_link_resolve() turns a file stub into its indirect node by the keyed
 * search for its name in the private data folder: primary then holds the indirect
 * node's record and the cursor, which reads through the catalog's search buffer,
 * stands on it. The node's own kind decides, a regular file or a symbolic link.
 *
 * hfsplus_thread_lookup() searches (id, "") through the cursor and requires a
 * thread of the given type; *thread borrows its name from the cursor's buffer,
 * on which the cursor stands.
 *
 * hfsplus_child_fill() maps a validated record, not a stub, to its child's
 * kind, size, owner, mode and dates, the kind's rules checked; it sets the
 * location and extra data to none.
 */
fsimg_status_t hfsplus_record_validate(fsimg_reader_t *,
    const hfsplus_record_t *, hfsplus_primary_t *, fsimg_error_t *);
/* The flag and kind tables for the root, which no other folder's rules fit: its
 * ID is 2 and nothing lists it.
 */
fsimg_status_t hfsplus_root_validate(const hfsplus_folder_t *, fsimg_error_t *);
fsimg_status_t hfsplus_link_resolve(fsimg_reader_t *, hfsplus_primary_t *,
    hfsplus_cursor_t *, fsimg_error_t *);
fsimg_status_t hfsplus_thread_lookup(hfsplus_cursor_t *, uint32_t, uint16_t,
    hfsplus_thread_t *, fsimg_error_t *);
fsimg_status_t hfsplus_child_fill(const hfsplus_primary_t *, hfsplus_child_t *,
    fsimg_error_t *);

/* Attributes (attributes.c). hfsplus_attr_scan(reader, id, compressed, payload,
 * scan, error) validates the attribute records of one node ID. compressed says
 * the object is a compressed file, whose com.apple.decmpfs is then its content,
 * not a stored attribute: scan->stored reports the rest and scan->compression
 * the attribute, whose header scan->cmpf holds. An attribute of that name held
 * in a fork record is FSIMG_EUNSUPPORTED. scan->payload_bytes is the length of
 * what follows the header. payload, when not NULL, is a buffer of
 * HFSPLUS_CHUNK_STORED_MAX bytes that receives those bytes, since the scan's
 * later nodes overwrite the node holding them; a listing passes NULL and reads
 * no payload.
 */
typedef struct hfsplus_attr_scan {
	hfsplus_cmpf_t	 cmpf;
	size_t		 payload_bytes;
	bool		 stored;
	bool		 compression;
} hfsplus_attr_scan_t;

fsimg_status_t hfsplus_attr_scan(fsimg_reader_t *, uint32_t, bool,
    unsigned char *, hfsplus_attr_scan_t *, fsimg_error_t *);

/* Transparent compression (compress.c).
 *
 * hfsplus_compress_check() is the one decision on a compressed file's record and
 * attribute, made at listing and again on first resolve: the attribute exists,
 * the data fork is empty, and the type and inline size are ones this reader
 * decodes. hfsplus_compress_known() says a type passed it, and
 * hfsplus_compress_consumes_fork() that its content is the resource fork.
 *
 * hfsplus_compress_open() opens the current entry's content from its record,
 * which the caller has read again. hfsplus_compress_resolve() then returns the
 * held extent containing an offset, valid until the next resolve or advance.
 * hfsplus_compress_reset() forgets the current entry at the walker's advance and
 * hfsplus_compress_free() releases the window at close.
 */
fsimg_status_t hfsplus_compress_check(const hfsplus_file_t *,
    const hfsplus_attr_scan_t *, fsimg_error_t *);
bool hfsplus_compress_known(uint8_t);
bool hfsplus_compress_consumes_fork(uint8_t);
fsimg_status_t hfsplus_compress_open(fsimg_reader_t *, const hfsplus_file_t *,
    fsimg_error_t *);
fsimg_status_t hfsplus_compress_resolve(fsimg_reader_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);
void hfsplus_compress_reset(fsimg_hfsplus_reader_t *);
void hfsplus_compress_free(fsimg_hfsplus_reader_t *);

/* The directory callbacks (directory.c). */
fsimg_status_t hfsplus_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t hfsplus_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t hfsplus_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t hfsplus_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);

#endif
