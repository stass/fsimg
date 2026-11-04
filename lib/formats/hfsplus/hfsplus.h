/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_HFSPLUS_H
#define FSIMG_HFSPLUS_H

#include "internal.h"

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

/* A catalog key. name borrows the big-endian units from the decoded buffer
 * and is valid as long as that buffer's contents are.
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

/* An attribute key. name borrows as in hfsplus_ckey_t. */
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

/* Record decoders (codec.c). Each takes a record's bytes and their length,
 * returns false when the length or a field that frames the record is wrong,
 * and then leaves its output untouched. A decoder reads nothing past length.
 * The false return is FSIMG_ECORRUPT in every caller. Field values are
 * otherwise the caller's to validate.
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
bool hfsplus_mdb_decode(const unsigned char *, size_t, hfsplus_mdb_t *);
bool hfsplus_cmpf_decode(const unsigned char *, size_t, hfsplus_cmpf_t *);
bool hfsplus_rsrc_decode(const unsigned char *, size_t, hfsplus_rsrc_t *);
bool hfsplus_rsrc_table_decode(const unsigned char *, size_t,
    hfsplus_rsrc_table_t *);
/* Entry index of a table whose bytes and length are given: the pairs of a
 * type 4 table, which begin after hfsplus_rsrc_table_t, or the words of a
 * type 8 or 12 table, which begin at the fork's start.
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

#endif
