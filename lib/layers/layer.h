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

/*
 * A container's disk as a chunk index (lib/layers/chunks.c): records that
 * tile the disk, each a disk range and how to obtain its bytes. Raw bytes
 * lie at a file offset, encoded_length equal to length; zero and fill
 * chunks name no file bytes, a fill chunk repeating argument every four
 * bytes little-endian; a compressed chunk has encoded_length bytes of the
 * codec argument names, decoding to length bytes, at most 16 MiB.
 */
typedef enum fsimg_chunk_kind {
	FSIMG_CHUNK_RAW = 1,
	FSIMG_CHUNK_ZERO = 2,
	FSIMG_CHUNK_FILL = 3,
	FSIMG_CHUNK_COMPRESSED = 4
} fsimg_chunk_kind_t;

typedef enum fsimg_chunk_codec {
	FSIMG_CODEC_ZLIB = 1
} fsimg_chunk_codec_t;

#define FSIMG_CHUNK_MAX_DECODED	(UINT64_C(16) * 1024 * 1024)

typedef struct fsimg_chunk {
	uint64_t	 disk_offset;
	uint64_t	 length;
	uint64_t	 file_offset;
	uint64_t	 encoded_length;
	fsimg_chunk_kind_t kind;
	uint32_t	 argument;
} fsimg_chunk_t;

/* What an index holds: its record count, its largest compressed chunk's
 * decoded length (zero without one), and a bit (1u << kind) per kind.
 */
typedef struct fsimg_chunk_summary {
	uint64_t	 count;
	uint64_t	 max_decoded;
	unsigned	 kinds;
} fsimg_chunk_summary_t;

typedef struct fsimg_chunk_index fsimg_chunk_index_t;

/*
 * new makes an empty index for a disk of disk_bytes whose encoded bytes
 * lie in the file range [data_offset, data_offset + data_bytes); owner
 * names the container in messages and must outlive the index. Records are
 * charged to the session's scratch. append accepts records in any order
 * and fails with FSIMG_ECORRUPT for a record outside the disk or the data
 * or whose lengths disagree with its kind. seal sorts the records and
 * keeps them only when they tile the disk exactly, FSIMG_ECORRUPT naming
 * an overlap or an uncovered range otherwise. lookup returns the sealed
 * record containing a disk offset. On any failure discard the index.
 */
fsimg_status_t fsimg_chunk_index_new(fsimg_session_t *, const char *,
    uint64_t, uint64_t, uint64_t, fsimg_chunk_index_t **, fsimg_error_t *);
fsimg_status_t fsimg_chunk_index_append(fsimg_chunk_index_t *,
    const fsimg_chunk_t *, fsimg_error_t *);
fsimg_status_t fsimg_chunk_index_seal(fsimg_chunk_index_t *, fsimg_error_t *);
fsimg_status_t fsimg_chunk_index_lookup(fsimg_chunk_index_t *, uint64_t,
    fsimg_chunk_t *, fsimg_error_t *);
void fsimg_chunk_index_summary(const fsimg_chunk_index_t *,
    fsimg_chunk_summary_t *);
void fsimg_chunk_index_free(fsimg_chunk_index_t *);
/* Validation control: the sealed index's private record store. */
struct fsimg_store *fsimg_test_chunk_records(const fsimg_chunk_index_t *);
/* The kinds of a summary as text in a fixed order: "raw,zero,fill,zlib". */
void fsimg_chunk_kinds_text(unsigned, char *, size_t);
/*
 * The container adapter: a read-only object over a sealed index's disk,
 * reading raw and compressed bytes from file through session, which both
 * outlive it, and carrying file's identity. It adopts the index on every
 * outcome. Its chunk cache is allocated at open, so a memory budget that
 * cannot hold four of the largest compressed chunk fails here with
 * FSIMG_ELIMIT naming the chunk cache. Decoding charges the compressed and
 * decoded bytes to session's work counter.
 */
fsimg_status_t fsimg_chunk_disk_open(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, fsimg_chunk_index_t *, fsimg_io_t **, fsimg_error_t *);

/*
 * The bounded XML property-list parser (lib/layers/plist.c), as DMG uses
 * it: it streams the file range [offset, offset + length) and reports each
 * element to a visitor. open announces an element once its place is
 * valid: a plist root holding one value, a dict holding keys each followed
 * by one value, an array holding values. A key, string or integer then
 * delivers its whole text, entities replaced, at most 4 KiB; a data
 * element delivers its base64 content decoded in pieces; close follows.
 * Nesting is at most 32 elements and names at most 16 bytes. Malformed
 * text is FSIMG_ECORRUPT; a construct beyond the subset or its limits
 * (CDATA, numeric references, another element, a deeper nesting) is
 * FSIMG_EUNSUPPORTED. A visitor's failure stops the parse with its status.
 * Every element charges one unit of work to session.
 */
typedef enum fsimg_plist_element {
	FSIMG_PLIST_PLIST,
	FSIMG_PLIST_DICT,
	FSIMG_PLIST_ARRAY,
	FSIMG_PLIST_KEY,
	FSIMG_PLIST_STRING,
	FSIMG_PLIST_DATA,
	FSIMG_PLIST_INTEGER,
	FSIMG_PLIST_TRUE,
	FSIMG_PLIST_FALSE
} fsimg_plist_element_t;

#define FSIMG_PLIST_MAX_DEPTH	32u
#define FSIMG_PLIST_MAX_TEXT	4096u

typedef struct fsimg_plist_visitor {
	fsimg_status_t	(*open)(void *, fsimg_plist_element_t, fsimg_error_t *);
	fsimg_status_t	(*text)(void *, const char *, size_t, fsimg_error_t *);
	fsimg_status_t	(*data)(void *, const unsigned char *, size_t,
			    fsimg_error_t *);
	fsimg_status_t	(*close)(void *, fsimg_plist_element_t, fsimg_error_t *);
	void		*argument;
} fsimg_plist_visitor_t;

fsimg_status_t fsimg_plist_parse(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, uint64_t, uint64_t, const fsimg_plist_visitor_t *,
    fsimg_error_t *);

/* Container tables are big-endian in DMG and VHD. */
static inline uint32_t
fsimg_layer_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | (uint32_t)p[3]);
}

static inline uint64_t
fsimg_layer_be64(const unsigned char *p)
{
	return ((uint64_t)fsimg_layer_be32(p) << 32 | fsimg_layer_be32(p + 4));
}

/* What a container reads: the file, its length, and the session that
 * charges its reads and decoding.
 */
typedef struct fsimg_container_input {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*file;
	fsimg_session_t	*session;
	uint64_t	 file_bytes;
} fsimg_container_input_t;

/* An opened container: the disk it yields, owned and freed with
 * fsimg_io_free, and what the image reports of it. guid is the VHD unique
 * identifier or the DMG segment ID in textual byte order, all zero for a
 * container without one. kinds is a summary's kind bits.
 */
typedef struct fsimg_container {
	fsimg_io_t	*disk;
	unsigned char	 guid[16];
	uint64_t	 disk_bytes;
	uint64_t	 chunks;
	unsigned	 kinds;
} fsimg_container_t;

/*
 * A container format.
 *
 * probe is a pure function of at most probe_bytes of the file's prefix,
 * its last FSIMG_CONTAINER_TAIL_BYTES (fewer for a shorter file) and the
 * file length: FSIMG_EFORMAT without its identity, and otherwise a claim,
 * FSIMG_OK or FSIMG_ECORRUPT or FSIMG_EUNSUPPORTED with a reason. A
 * trailer claims only when its ranges end at the file's end.
 *
 * open validates every table and yields the disk; any failure is final and
 * leaves nothing allocated. verify performs the checks that read beyond
 * the tables, and with payload those that decode every chunk;
 * payload_checks is true when those check decoded bytes against a
 * checksum, so read-through verification has payload scope. codecs names
 * the chunk encodings it reads, for the capability record; zlib is true
 * when it depends on zlib.
 */
#define FSIMG_CONTAINER_TAIL_BYTES	512u
typedef struct fsimg_container_ops {
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t,
			    const unsigned char *, size_t, uint64_t,
			    fsimg_error_t *);
	fsimg_status_t	(*open)(const fsimg_container_input_t *,
			    fsimg_container_t *, fsimg_error_t *);
	fsimg_status_t	(*verify)(const fsimg_container_input_t *,
			    const fsimg_container_t *, bool, fsimg_error_t *);
	const char	*codecs;
	bool		 payload_checks;
	bool		 zlib;
} fsimg_container_ops_t;

/* A registered layer. Every layer name the stack grammar accepts outside
 * the filesystem slot is one record; a scheme has scheme operations and a
 * container container operations, and both are NULL for a layer this build
 * recognizes but does not implement.
 */
typedef struct fsimg_layer {
	const char	*name;
	const char	*suffix;	/* File suffix of a container or compression */
	fsimg_layer_kind_t kind;
	const fsimg_scheme_ops_t *scheme;
	const fsimg_container_ops_t *container;
} fsimg_layer_t;

extern const fsimg_scheme_ops_t fsimg_gpt_scheme;
extern const fsimg_scheme_ops_t fsimg_mbr_scheme;
extern const fsimg_container_ops_t fsimg_dmg_container;
extern const fsimg_container_ops_t fsimg_vhd_container;
extern const fsimg_container_ops_t fsimg_simg_container;

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
