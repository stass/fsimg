/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_LAYER_H
#define FSIMG_LAYER_H

#include "internal.h"

/* Table fields are little-endian at fixed offsets of untrusted bytes. */
static inline uint16_t
fsimg_layer_le16(const unsigned char *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static inline uint32_t
fsimg_layer_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static inline uint64_t
fsimg_layer_le64(const unsigned char *p)
{
	return ((uint64_t)fsimg_layer_le32(p) |
	    (uint64_t)fsimg_layer_le32(p + 4) << 32);
}

/* GPT reads at most this many entries of 128 bytes per table copy. */
#define FSIMG_GPT_MAX_ENTRIES	1024u
#define FSIMG_GPT_NAME_UNITS	36u

/*
 * One table slot as a scheme decodes it. A used slot's offset and length
 * are disk bytes already checked against the disk and the other used
 * slots, so a slice of them cannot fail its bounds. GUIDs are in textual
 * byte order whatever the table stores. name holds a GPT entry's UTF-16
 * units as stored; the name ends at the first zero unit or at 36 units.
 */
typedef struct fsimg_partition_record {
	unsigned char	 type_guid[16];
	unsigned char	 guid[16];
	uint16_t	 name[FSIMG_GPT_NAME_UNITS];
	uint64_t	 offset;
	uint64_t	 length;
	uint32_t	 index;
	uint8_t		 mbr_type;
	bool		 used;
	bool		 active;
	bool		 extended;	/* An MBR container of logical partitions */
} fsimg_partition_record_t;

typedef enum fsimg_scheme_kind {
	FSIMG_SCHEME_GPT,
	FSIMG_SCHEME_MBR
} fsimg_scheme_kind_t;

/* A validated partition table: one record per slot in slot order, held in
 * memory charged to the context, and the disk identity it states.
 */
typedef struct fsimg_scheme_table {
	fsimg_partition_record_t *records;
	unsigned char	 disk_guid[16];
	uint32_t	 count;
	uint32_t	 sector_bytes;
	uint32_t	 mbr_disk_id;
	fsimg_scheme_kind_t kind;
} fsimg_scheme_table_t;

/* What a scheme's open reads from: the disk, the session that charges its
 * reads, and the read-side settings.
 */
typedef struct fsimg_scheme_input {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*disk;
	fsimg_session_t	*session;
	const fsimg_layer_options_t *options;
	uint64_t	 disk_bytes;
} fsimg_scheme_input_t;

/*
 * A partition scheme's read side.
 *
 * probe follows the filesystem probe contract: a pure function of at most
 * probe_bytes of the disk prefix and the disk length, FSIMG_EFORMAT without
 * the identity signature, and otherwise a claim: FSIMG_OK, or
 * FSIMG_ECORRUPT or FSIMG_EUNSUPPORTED with a reason.
 *
 * open validates every table check the profile names and fills the table;
 * any failure is final, never a fallback to another decoder. On failure it
 * leaves nothing allocated.
 *
 * verify performs the checks that are not needed to read the table, such
 * as GPT's protective MBR, on a table open accepted.
 */
typedef struct fsimg_scheme_ops {
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t, uint64_t,
			    fsimg_error_t *);
	fsimg_status_t	(*open)(const fsimg_scheme_input_t *,
			    fsimg_scheme_table_t *, fsimg_error_t *);
	fsimg_status_t	(*verify)(const fsimg_scheme_input_t *,
			    const fsimg_scheme_table_t *, fsimg_error_t *);
} fsimg_scheme_ops_t;

/* A registered layer. Every layer name the stack grammar accepts outside
 * the filesystem slot is one record; scheme is NULL for a layer this build
 * recognizes but does not implement.
 */
typedef struct fsimg_layer {
	const char	*name;
	const char	*suffix;	/* File suffix of a container or compression */
	fsimg_layer_kind_t kind;
	const fsimg_scheme_ops_t *scheme;
} fsimg_layer_t;

extern const fsimg_scheme_ops_t fsimg_gpt_scheme;
extern const fsimg_scheme_ops_t fsimg_mbr_scheme;

/* The registry, in reporting order; NULL past the end. */
const fsimg_layer_t *fsimg_layer_at(size_t);
const fsimg_layer_t *fsimg_layer_find(const char *);
/* Check a caller's stack value: states in range, names terminated, each
 * named slot holding a layer of its kind or a filesystem name.
 */
fsimg_status_t fsimg_stack_validate(const fsimg_stack_t *, fsimg_error_t *);

/*
 * The partition type table shared by selection, reporting and writing.
 * A type matches a GPT entry by its GUID and an MBR entry by any of its
 * bytes; the first byte is the one a writer records.
 */
typedef struct fsimg_partition_type {
	const char	*name;
	const char	*gpt_guid;	/* RFC 4122 text, or NULL */
	const char	*gpt_name;	/* sgdisk's default partition name */
	uint8_t		 mbr_types[3];	/* Zero-terminated when shorter */
} fsimg_partition_type_t;

/* A parsed type selector: a table entry, a literal GUID or a literal byte. */
typedef struct fsimg_type_selector {
	const fsimg_partition_type_t *type;
	unsigned char	 guid[16];
	uint8_t		 mbr_type;
	bool		 literal_guid;
	bool		 literal_byte;
} fsimg_type_selector_t;

fsimg_status_t fsimg_partition_type_parse(const char *,
    fsimg_type_selector_t *, fsimg_error_t *);
/* Whether a used record of the named scheme carries the selected type. A
 * selector the scheme cannot express fails with FSIMG_EINVAL.
 */
fsimg_status_t fsimg_partition_type_match(const fsimg_type_selector_t *,
    fsimg_scheme_kind_t, const fsimg_partition_record_t *, bool *,
    fsimg_error_t *);
/* The table name of a record's type, or NULL for a type the table lacks. */
const char *fsimg_partition_type_name(fsimg_scheme_kind_t,
    const fsimg_partition_record_t *);
bool fsimg_guid_parse(const char *, unsigned char *);

#endif
