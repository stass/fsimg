/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_LAYER_H
#define FSIMG_LAYER_H

#include "internal.h"

/*
 * Every layer function takes a non-NULL error record, which receives the
 * failure's status; the public entry points that reach this module (the
 * image, the image operations and the writer) substitute a local record
 * when their caller passes none.
 */

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

static inline void
fsimg_layer_put_le16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

static inline void
fsimg_layer_put_le32(unsigned char *p, uint32_t value)
{
	fsimg_layer_put_le16(p, (uint16_t)value);
	fsimg_layer_put_le16(p + 2, (uint16_t)(value >> 16));
}

static inline void
fsimg_layer_put_le64(unsigned char *p, uint64_t value)
{
	fsimg_layer_put_le32(p, (uint32_t)value);
	fsimg_layer_put_le32(p + 4, (uint32_t)(value >> 32));
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

/* One run of encoded table bytes at a disk offset, in charged memory. */
typedef struct fsimg_table_region {
	unsigned char	*bytes;
	uint64_t	 offset;
	size_t		 length;
} fsimg_table_region_t;

/* The tables a scheme writes, in disk order: GPT's leading run (protective
 * MBR, primary header and entry array) and trailing run (backup entry
 * array and header), or the one MBR sector. Together they hold at most
 * 44 KiB, the two GPT runs at 4096-byte sectors.
 */
#define FSIMG_TABLE_REGIONS	2
typedef struct fsimg_scheme_tables {
	fsimg_table_region_t regions[FSIMG_TABLE_REGIONS];
	unsigned	 count;
} fsimg_scheme_tables_t;

void fsimg_scheme_tables_free(fsimg_scheme_tables_t *);

/*
 * A partition scheme.
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
 *
 * The write side describes the tables' footprint and encodes them. A
 * written table has slots entries; at sector size S its leading tables
 * fill the first leading sectors of the disk and its trailing tables the
 * last trailing sectors, and the disk holds at most max_disk_sectors.
 * encode turns a placed table, whose records are the used slots in order
 * and lie between those tables, into its regions for a disk of the given
 * length; every byte of a region is defined.
 */
typedef struct fsimg_scheme_ops {
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t, uint64_t,
			    fsimg_error_t *);
	fsimg_status_t	(*open)(const fsimg_scheme_input_t *,
			    fsimg_scheme_table_t *, fsimg_error_t *);
	fsimg_status_t	(*verify)(const fsimg_scheme_input_t *,
			    const fsimg_scheme_table_t *, fsimg_error_t *);
	uint32_t	 slots;
	uint64_t	 max_disk_sectors;
	void		(*footprint)(uint32_t, uint64_t *, uint64_t *);
	fsimg_status_t	(*encode)(fsimg_ctx_t *, const fsimg_scheme_table_t *,
			    uint64_t, fsimg_scheme_tables_t *, fsimg_error_t *);
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
/* The layer named in a stack slot, NULL when the slot names none. */
const fsimg_layer_t *fsimg_stack_layer(const fsimg_stack_t *,
    fsimg_layer_kind_t);
/*
 * Check layer options against a stack: reserved words zero, text fields
 * terminated, every setting of a layer the stack does not name at its
 * default, and on read (writing false) every setting but gpt:sector at its
 * default. A failure is FSIMG_EINVAL naming the key in error.path.
 */
fsimg_status_t fsimg_layer_options_check(const fsimg_stack_t *,
    const fsimg_layer_options_t *, bool, fsimg_error_t *);
/* Every partition setting (gpt:type, gpt:name, gpt:guid, mbr:type,
 * mbr:active) at its default, as compose requires; a failure is
 * FSIMG_EINVAL naming the key.
 */
fsimg_status_t fsimg_layer_options_disk_only(const fsimg_layer_options_t *,
    fsimg_error_t *);

/*
 * The partition type table shared by selection, reporting and writing.
 * A type matches a GPT entry by its GUID and an MBR entry by any of its
 * bytes; the first byte is the one a writer records.
 */
typedef struct fsimg_partition_type {
	const char	*name;
	const char	*gpt_guid;	/* RFC 4122 text, or NULL */
	const char	*gpt_name;	/* sgdisk's default partition name */
	uint8_t		 mbr_types[3];	/* Written type first; zero-terminated */
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
/* The type-table entry of that name, or NULL. */
const fsimg_partition_type_t *fsimg_partition_type_find(const char *);
/* Give a record to be written the selected type: its GPT type GUID, or its
 * MBR byte, which must not be an extended or GPT protective type. A
 * selector the scheme cannot express fails with FSIMG_EINVAL. name
 * receives the type's conventional GPT name, empty for none.
 */
fsimg_status_t fsimg_partition_type_assign(const fsimg_type_selector_t *,
    fsimg_scheme_kind_t, fsimg_partition_record_t *, const char **,
    fsimg_error_t *);
/* Fill a public partition record from a decoded or planned one. */
void fsimg_partition_report(fsimg_scheme_kind_t,
    const fsimg_partition_record_t *, fsimg_partition_info_t *);
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

/*
 * A disk to write, built in three steps. init resolves the stack's scheme
 * and its disk-level settings (sector size, disk length, disk identifier)
 * and makes count empty records; partition resolves each record's type,
 * name, unique GUID and status; place lays the records out in slot order,
 * each at the first 1 MiB boundary after the previous one, and sets the
 * disk length. Identifiers left unset are derived from the epoch under
 * FSIMG_WRITE_REPRODUCIBLE and are otherwise FSIMG_EINVAL naming the key.
 * Without a scheme the plan has no records and the volume is the disk.
 * Records are charged to the context; free releases them.
 */
typedef struct fsimg_disk_plan {
	const fsimg_layer_t *scheme;
	fsimg_scheme_table_t table;
	uint64_t	 disk_bytes;	/* A fixed length until placed, else 0 */
	int64_t		 epoch;
	uint32_t	 flags;		/* FSIMG_WRITE_* mode */
	bool		 fixed;		/* gpt:disk or mbr:disk set the length */
} fsimg_disk_plan_t;

/* What one partition asks for: type and name text as gpt_type, mbr_type
 * and gpt_name spell them, NULL or empty for the defaults; a unique GUID
 * (NULL or all zero unset); the MBR status; and the filesystem whose
 * type-table entry supplies the default type. A failure names the setting
 * ("gpt:type") for a partition the layer options describe, and the
 * partition ("partition 2") for one of a compose.
 */
typedef struct fsimg_partition_request {
	const char	*type;
	const char	*name;
	const unsigned char *guid;
	const char	*filesystem;
	bool		 active;
	bool		 composed;
} fsimg_partition_request_t;

fsimg_status_t fsimg_disk_plan_init(fsimg_ctx_t *, const fsimg_stack_t *,
    const fsimg_layer_options_t *, uint32_t, int64_t, uint32_t,
    fsimg_disk_plan_t *, fsimg_error_t *);
fsimg_status_t fsimg_disk_plan_partition(fsimg_disk_plan_t *, uint32_t,
    const fsimg_partition_request_t *, fsimg_error_t *);
/* Fit a volume to a fixed disk's one partition: a zero *volume_bytes
 * becomes the longest multiple of granule bytes the partition holds, and a
 * requested length longer than that is FSIMG_EINVAL naming the disk key.
 */
fsimg_status_t fsimg_disk_plan_fit(const fsimg_disk_plan_t *, uint32_t,
    uint64_t *, fsimg_error_t *);
/* Place records whose volumes have the given lengths; with a fixed disk
 * they must fit within it. Every unique GUID must differ from the others
 * and from the disk GUID.
 */
fsimg_status_t fsimg_disk_plan_place(fsimg_disk_plan_t *, const uint64_t *,
    fsimg_error_t *);
/* The placed plan as an opened image of it would report its disk and the
 * partition in slot (from 1).
 */
void fsimg_disk_plan_report(const fsimg_disk_plan_t *, uint32_t,
    fsimg_image_info_t *, fsimg_partition_info_t *);
/* Fill a disk object of the placed plan's length (lib/layers/output.c):
 * zeros wherever no table or volume lies, the volume of volume_bytes that
 * emit writes into its partition's slice, and the encoded tables, written
 * last through the session after cancellation polls.
 */
typedef fsimg_status_t (*fsimg_volume_emit_t)(void *, fsimg_io_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_disk_plan_emit(const fsimg_disk_plan_t *, uint64_t,
    fsimg_io_t *, fsimg_session_t *, fsimg_volume_emit_t, void *,
    fsimg_error_t *);
void fsimg_disk_plan_free(fsimg_disk_plan_t *);

/* What the filesystem probes, the one forced by name or all of them, say
 * about the volume at a byte range of an I/O object; format receives the
 * identified format's name, empty without a match.
 */
fsimg_status_t fsimg_volume_claim(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, uint64_t, uint64_t, const char *, fsimg_claim_t *,
    char [static 16], fsimg_error_t *);

/* Identifier roles of the derivation (specs/design-layers.md#identifiers). */
typedef enum fsimg_identifier_role {
	FSIMG_ROLE_DISK = 1,
	FSIMG_ROLE_PARTITION = 2,
	FSIMG_ROLE_VHD = 3,
	FSIMG_ROLE_DMG = 4
} fsimg_identifier_role_t;

/* The reproducible GUID of a role and index at an epoch, in textual byte
 * order.
 */
void fsimg_identifier_derive(int64_t, fsimg_identifier_role_t, uint8_t,
    unsigned char [static 16]);

#endif
