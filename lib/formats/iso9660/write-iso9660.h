/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_ISO9660_H
#define FSIMG_WRITE_ISO9660_H

#include "emit.h"
#include "format.h"
#include "iso9660.h"

/*
 * The ISO 9660 writer. One
 * profile: a primary hierarchy at interchange level 1, 2 or 3 carrying
 * Rock Ridge entries, and a Joliet level 3 hierarchy, both over the same
 * file data. Planning (write-plan.c) runs fixed passes over the frozen
 * catalog; write-names.c derives both hierarchies' identifiers and sorts
 * them into name streams; write-susp.c encodes Rock Ridge entries and
 * splits them between a record and its continuation fragments, for
 * sizing and emission alike; write-emit.c writes every structure once.
 *
 * Block layout, in order: the system area (zero), the primary descriptor
 * at block 16, an optional boot descriptor, the Joliet descriptor, the
 * terminator, the primary type-L and type-M path tables and the Joliet
 * ones, an optional boot catalog, the primary directories in
 * path-table order, the continuation blocks, the Joliet directories in
 * path-table order, each file's data in catalog ID order, then zero
 * blocks up to a requested volume size.
 */

/* The two hierarchies the writer builds. */
typedef enum iso_hierarchy {
	ISO_PRIMARY = 0,
	ISO_JOLIET = 1
} iso_hierarchy_t;
#define ISO_HIERARCHIES		2u

#define ISO_RECORD_MAX		254u	/* the longest even LEN_DR */
#define ISO_CE_BYTES		28u
#define ISO_FRAGMENTS_MAX	3u	/* continuation fragments per record */
/* Rock Ridge entries of one record before the split: PX, TF, NM entries
 * for a 255-byte name and SL entries for a 1023-byte target, whose
 * components take at most two bytes per target byte, with headers.
 */
#define ISO_RR_ENTRIES_MAX	4096u
#define ISO_RR_NAME_MAX		255u	/* a Rock Ridge name on the writer */
/* The longest link target and target component the writer stores: the
 * most libisofs reads back, below the reader's 1024-byte bound.
 */
#define ISO_LINK_TARGET_MAX	1023u
#define ISO_LINK_COMPONENT_MAX	255u
/* The largest whole-block section below 2^32 bytes. */
#define ISO_SECTION_MAX		UINT64_C(0xfffff800)
#define ISO_DIRECTORIES_MAX	65535u	/* path-table numbers are 16 bits */
#define ISO_PRIMARY_IDENTIFIER_MAX 33u	/* 30 + ".", ";1" */
#define ISO_JOLIET_UNITS_MAX	64u
#define ISO_JOLIET_IDENTIFIER_MAX (2u * ISO_JOLIET_UNITS_MAX)
#define ISO_ALIAS_MAX		UINT32_C(0x0fffffff)	/* seven hex digits */
#define ISO_PHYSICAL_LEVEL_MAX	8u
#define ISO_PATH_MAX		255u
#define ISO_LABEL_UNITS_MAX	16u

/* Default permissions for an entry whose catalog record has no mode. */
#define ISO_DIRECTORY_PERMISSIONS 0755u
#define ISO_FILE_PERMISSIONS	0644u
#define ISO_OTHER_PERMISSIONS	0777u

/* POSIX file types in a Rock Ridge PX mode. */
#define ISO_PX_FIFO		0010000u
#define ISO_PX_DIRECTORY	0040000u
#define ISO_PX_FILE		0100000u
#define ISO_PX_SYMLINK		0120000u

/* Which of a row's optional TF times are present: the modification time
 * always is, since an absent one takes the fallback.
 */
#define ISO_TIME_ACCESSED	0x01u
#define ISO_TIME_CHANGED	0x02u

/* A primary record's continuation pattern and first section's placement.
 * Every section repeats the pattern; emission advances through its areas.
 * A relocated directory has separate physical and logical placeholder plans.
 */
typedef struct iso_record_plan {
	uint64_t	 fragment_position;
	uint16_t	 fragment_bytes[ISO_FRAGMENTS_MAX];
	uint8_t		 record_entries;
	uint8_t		 fragments;
} iso_record_plan_t;

/*
 * One plan row per catalog ID, row 0 for the root, plus a reserved row at
 * count + 1 for a synthetic relocation container. Physical streams alone
 * can reach that row; source operations stay in the catalog's ID domain.
 * A row holds what the
 * entry's records need apart from its names, which live in the name
 * streams:
 *   kind		the entry kind
 *   size		a regular file's bytes, zero otherwise
 *   mode, uid, gid, links
 *			the Rock Ridge PX fields: type and permission bits, owner,
 *			and the link count, 1, or 2 plus the subdirectories
 *   modified, accessed, changed, times
 *			the encoded seven-byte times and which optional ones are
 *			present; modified is also the recording date
 *   identifier[h], alias[h]
 *			the length in bytes of the entry's identifier in
 *			hierarchy h and its alias number, 0 for a natural one
 *   level, path_length	a directory's physical level, the root being 1, and
 *			the bytes of its physical path: identifiers and the
 *			separators between them
 *   group[h], children[h]
 *			a directory's children in hierarchy h: the frame offset
 *			of its first record in the sorted name stream, and their
 *			count
 *   number[h]		a directory's path-table record number in h
 *   block[h], bytes[h]	a directory's extent in h: first block and length
 *   first_block	a file's first data block, 0 for an empty file
 *   child, placeholder	the primary record plans: kept system-use bytes,
 *			fragment count, lengths and first byte position in the
 *			continuation region. The root uses child for its dot
 *			record. A relocated directory uses child for its physical
 *			RE record and placeholder for its logical CL record.
 *   physical_parent	the primary parent, the catalog parent unless relocated
 *   relocation_alias	nonzero for a directory moved under the container;
 *			its physical alias number, in catalog order
 *   target_bytes, target_hash
 *			a symbolic link's encoded SL entries: their length and
 *			FNV-1a hash, so emission can tell a changed target
 * Rows are stored in ISO_ROW_BYTES little-endian bytes, at the offsets
 * iso_row_get() and iso_row_set() name.
 */
typedef struct iso_row {
	uint64_t	 size;
	uint64_t	 group[ISO_HIERARCHIES];
	iso_record_plan_t child;
	iso_record_plan_t placeholder;
	uint64_t	 physical_parent;
	uint64_t	 target_hash;
	uint32_t	 relocation_alias;	/* nonzero exactly when relocated */
	uint32_t	 kind;
	uint32_t	 mode;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 links;
	uint32_t	 alias[ISO_HIERARCHIES];
	uint32_t	 children[ISO_HIERARCHIES];
	uint32_t	 block[ISO_HIERARCHIES];
	uint32_t	 bytes[ISO_HIERARCHIES];
	uint32_t	 first_block;
	uint16_t	 number[ISO_HIERARCHIES];
	uint16_t	 path_length;
	uint16_t	 target_bytes;
	uint8_t		 identifier[ISO_HIERARCHIES];
	uint8_t		 level;
	uint8_t		 times;
	unsigned char	 modified[ISO_DATE7_BYTES];
	unsigned char	 accessed[ISO_DATE7_BYTES];
	unsigned char	 changed[ISO_DATE7_BYTES];
} iso_row_t;

#define ISO_ROW_BYTES		176u	/* the last field ends at 172 */

/*
 * Name-stream records, one per entry of a hierarchy, sorted by parent
 * and the hierarchy's order: the parent ID and the entry ID as u64, a
 * record-role byte, then the identifier's bytes (UTF-16BE for Joliet).
 * Path-stream records, one per directory in path-table order: the ID as
 * u64, the parent's path-table number as u16, then the identifier.
 */
#define ISO_NAME_HEADER		17u
#define ISO_NAME_RECORD_MAX	(ISO_NAME_HEADER + ISO_JOLIET_IDENTIFIER_MAX)
#define ISO_PATH_STREAM_HEADER	10u
#define ISO_PATH_STREAM_MAX	(ISO_PATH_STREAM_HEADER + \
	ISO_JOLIET_IDENTIFIER_MAX)

/* Record roles keep identifier spelling separate from physical kind:
 * a placeholder uses a directory identifier with the ISO file flag.
 */
typedef enum iso_name_role {
	ISO_NAME_FILE,
	ISO_NAME_DIRECTORY,
	ISO_NAME_PLACEHOLDER,
	ISO_NAME_RELOCATED
} iso_name_role_t;

/* One decoded name-stream record; identifier borrows the caller's
 * record buffer.
 */
typedef struct iso_name {
	uint64_t	 parent;
	uint64_t	 id;
	const unsigned char *identifier;
	size_t		 length;
	iso_name_role_t	 role;
} iso_name_t;

/*
 * The plan. rows holds one row per catalog ID; names[h] is hierarchy h's
 * sorted name stream and paths[h] its directories in path-table order,
 * which is also the order their directories are laid out and emitted.
 * fallback is the time of absent modification times and both volume
 * dates: the epoch when reproducible, else the planning clock. The block
 * fields are the layout's; volume_blocks counts every block, padding
 * included. target and entries are planning's and emission's buffers for
 * a link target and one record's Rock Ridge entries; block and fragments
 * emission's for the directory block being filled and one record's
 * continuation fragments.
 */
typedef struct iso_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*rows;
	fsimg_stream_t	*names[ISO_HIERARCHIES];
	fsimg_stream_t	*paths[ISO_HIERARCHIES];
	int64_t		 fallback;
	uint64_t	 continuation_bytes;	/* the next-fit cursor's end */
	uint64_t	 data_blocks;
	uint64_t	 boot_bios;		/* catalog ID, zero when absent */
	uint64_t	 boot_efi;		/* catalog ID, zero when absent */
	uint32_t	 boot_catalog_block;	/* zero without boot images */
	uint64_t	 relocation_id;		/* zero without relocation */
	fsimg_catalog_entry_t relocation_entry;	/* synthetic container only */
	uint32_t	 level;
	uint32_t	 directories[ISO_HIERARCHIES];
	uint32_t	 path_bytes[ISO_HIERARCHIES];
	uint32_t	 path_l[ISO_HIERARCHIES];
	uint32_t	 path_m[ISO_HIERARCHIES];
	uint32_t	 continuation_block;
	uint32_t	 continuation_blocks;
	uint32_t	 volume_blocks;
	unsigned char	 label[ISO_HIERARCHIES][ISO_LABEL_BYTES];
	unsigned char	 volume_date[ISO_DATE17_BYTES];
	unsigned char	 target[FSIMG_SYMLINK_TARGET_MAX];
	unsigned char	 entries[ISO_RR_ENTRIES_MAX];
	unsigned char	 block[ISO_BLOCK_BYTES];
	unsigned char	 fragments[ISO_FRAGMENTS_MAX * ISO_BLOCK_BYTES];
} iso_plan_t;

/*
 * One record's Rock Ridge entries split between the record and its
 * continuation fragments (write-susp.c): kept bytes of entries stay in
 * the record, followed by a CE when there are fragments; fragment i holds
 * the next fragment_bytes[i] bytes, the entries and, for every fragment
 * but the last, a CE to the next one.
 */
typedef struct iso_split {
	size_t		 kept;
	size_t		 record_bytes;		/* kept plus its CE */
	unsigned	 fragments;
	uint16_t	 fragment_bytes[ISO_FRAGMENTS_MAX];
} iso_split_t;

/* Which record a set of Rock Ridge entries describes. */
typedef enum iso_rr_record {
	ISO_RR_ROOT_SELF,	/* the root's first dot record */
	ISO_RR_DOT,		/* any other dot record */
	ISO_RR_CHILD,		/* an entry's record in its parent */
	ISO_RR_PLACEHOLDER,	/* logical directory record with CL */
	ISO_RR_RELOCATED,	/* physical directory record with RE */
	ISO_RR_PARENT_LINK	/* relocated directory's parent dot record */
} iso_rr_record_t;

extern const fsimg_writer_ops_t fsimg_iso9660_writer_ops;

/* write-plan.c */
fsimg_status_t iso_row_get(iso_plan_t *, uint64_t, iso_row_t *,
    fsimg_error_t *);
fsimg_status_t iso_row_set(iso_plan_t *, uint64_t, const iso_row_t *,
    fsimg_error_t *);
fsimg_status_t iso_name_next(fsimg_stream_t *, uint64_t *, unsigned char *,
    iso_name_t *, fsimg_error_t *);
bool iso_name_directory(const iso_name_t *);
fsimg_status_t iso_catalog_get(iso_plan_t *, uint64_t,
    const fsimg_catalog_entry_t **, fsimg_error_t *);
uint32_t iso_sections(uint64_t);
uint32_t iso_section_bytes(uint64_t, uint32_t);
size_t iso_record_room(size_t);
fsimg_status_t iso_child_entries(iso_plan_t *, const iso_name_t *, iso_row_t *,
    size_t *, fsimg_error_t *);
fsimg_status_t iso_check_child_entries(iso_plan_t *, const iso_name_t *,
    const iso_row_t *, size_t *, fsimg_error_t *);

/* write-names.c */
fsimg_status_t iso_write_names(iso_plan_t *, iso_hierarchy_t,
    fsimg_error_t *);
fsimg_status_t iso_write_relocated_names(iso_plan_t *, fsimg_error_t *);
bool iso_identifier(const iso_plan_t *, iso_hierarchy_t, const char *,
    size_t, bool, uint32_t, unsigned char *, size_t *);

/* write-susp.c */
typedef struct iso_rr_input {
	const iso_row_t	*row;
	const char	*name;
	const unsigned char *target;
	size_t		 name_length;
	size_t		 target_length;
	iso_rr_record_t	 record;
	uint32_t	 link_block;		/* PL's logical parent */
} iso_rr_input_t;

size_t iso_rr_encode(const iso_rr_input_t *, unsigned char *);
size_t iso_sl_encode(const unsigned char *, size_t, unsigned char *);
fsimg_status_t iso_susp_split(const unsigned char *, size_t, size_t,
    iso_split_t *, fsimg_error_t *);
void iso_susp_assemble(const unsigned char *, const iso_split_t *,
    uint32_t, uint64_t, unsigned char *, unsigned char *, uint64_t *);
uint64_t iso_hash(const unsigned char *, size_t);
uint64_t iso_place(uint64_t *, size_t);

/* write-emit.c */
fsimg_status_t iso_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

#endif
