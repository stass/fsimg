/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_LAYER_H
#define FSIMG_LAYER_H

#include "host.h"
#include "internal.h"
#include "settings.h"

/*
 * Every layer function takes a non-NULL error record, which receives the
 * failure's status; the public entry points that reach this module (the
 * image, the image operations and the writer) substitute a local record
 * when their caller passes none.
 */

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
 * The planning side of a scheme (specs/design-scheme-planning.md). Every
 * argument is borrowed for the call alone, and no operation allocates.
 *
 * plan_disk reads the scheme's own disk settings from checked layer
 * options: it fills the table's kind, sector size and disk identity, which
 * it takes as given, derives from the epoch under FSIMG_WRITE_REPRODUCIBLE
 * or refuses, and states the requested fixed disk length (zero to size the
 * disk from its contents) and the static key ("gpt:disk") that names it.
 */
typedef struct fsimg_scheme_plan_args {
	const fsimg_layer_options_t *layers;
	int64_t		 epoch;
	uint32_t	 flags;		/* FSIMG_WRITE_* mode */
} fsimg_scheme_plan_args_t;

typedef struct fsimg_scheme_plan_facts {
	uint64_t	 requested_disk_bytes;
	const char	*disk_key;
} fsimg_scheme_plan_facts_t;

/* One compose part's metadata: type and name text as gpt:type, mbr:type
 * and gpt:name spell them, NULL or empty for the defaults, and a unique
 * GUID, NULL or all zero for unset.
 */
typedef struct fsimg_partition_request {
	const char	*type;
	const char	*name;
	const unsigned char *guid;
} fsimg_partition_request_t;

/*
 * plan_partition resolves the metadata of the partition in slot index
 * (from 1) into a zeroed record whose index is set, from exactly one of
 * two sources: the layer options of a create or wrap, of which the scheme
 * reads only its own partition settings, or a compose part. filesystem
 * names the volume's filesystem, whose type-table entry gives the default
 * type, or is NULL when none is known. A failure names the setting
 * ("gpt:type") or the part ("partition 2") through fsimg_partition_key;
 * the record is marked used only once every field has resolved.
 */
typedef struct fsimg_scheme_partition_args {
	const fsimg_layer_options_t *layers;
	const fsimg_partition_request_t *part;
	const char	*filesystem;
	int64_t		 epoch;
	uint32_t	 flags;		/* FSIMG_WRITE_* mode */
	uint32_t	 index;
} fsimg_scheme_partition_args_t;

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
 * The write set is plan_disk, plan_partition, validate_plan, report_plan,
 * footprint and encode, with nonzero slots and max_disk_sectors: a
 * scheme has all of them or none (fsimg_scheme_write_set_valid), and
 * writes exactly when it has plan_disk. A written table has slots
 * entries; at sector size S, one plan_disk accepted, its leading tables
 * fill the first leading sectors of the disk and its trailing tables the
 * last trailing sectors. max_disk_sectors states the profile's disk
 * bound, which the scheme's own plan_disk and validate_plan enforce with
 * their diagnostics; common planning does not check it.
 * validate_plan checks a placed table against the final disk length:
 * identities that must differ and fields the profile cannot represent,
 * FSIMG_EINVAL with the scheme's own diagnostic. report_plan fills the
 * scheme's fields of an image report and the report of slot (from 1) of
 * a validated table. encode turns a validated table into its regions for
 * a disk of the given length; every byte of a region is defined.
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
	fsimg_status_t	(*plan_disk)(const fsimg_scheme_plan_args_t *,
			    fsimg_scheme_table_t *, fsimg_scheme_plan_facts_t *,
			    fsimg_error_t *);
	fsimg_status_t	(*plan_partition)(const fsimg_scheme_partition_args_t *,
			    fsimg_partition_record_t *, fsimg_error_t *);
	fsimg_status_t	(*validate_plan)(const fsimg_scheme_table_t *, uint64_t,
			    fsimg_error_t *);
	void		(*report_plan)(const fsimg_scheme_table_t *, uint32_t,
			    fsimg_image_info_t *, fsimg_partition_info_t *);
	void		(*footprint)(uint32_t, uint64_t *, uint64_t *);
	fsimg_status_t	(*encode)(fsimg_ctx_t *, const fsimg_scheme_table_t *,
			    uint64_t, fsimg_scheme_tables_t *, fsimg_error_t *);
} fsimg_scheme_ops_t;

/* Whether a scheme writes: it has the write set. */
bool fsimg_scheme_writable(const fsimg_scheme_ops_t *);
/* Whether a scheme's write set is complete or absent, as every registered
 * scheme's must be.
 */
bool fsimg_scheme_write_set_valid(const fsimg_scheme_ops_t *);
/* The key a partition planner's failure names is the setting when the
 * layer options supplied the partition, else "partition N".
 * fsimg_partition_error names that key in error.path and returns the
 * failure's status; fsimg_partition_setting_error fails FSIMG_EINVAL with
 * the message.
 */
fsimg_status_t fsimg_partition_error(const fsimg_scheme_partition_args_t *,
    const char *, fsimg_status_t, fsimg_error_t *);
fsimg_status_t fsimg_partition_setting_error(
    const fsimg_scheme_partition_args_t *, const char *, const char *,
    fsimg_error_t *);

/*
 * A container's disk as a chunk index (lib/layers/chunks.c): records that
 * tile the disk, each a disk range and how to obtain its bytes. Raw bytes
 * lie at a file offset, encoded_length equal to length; zero and fill
 * chunks name no file bytes, a fill chunk repeating argument every four
 * bytes little-endian; a compressed chunk has encoded_length bytes of the
 * codec argument names, decoding to length bytes, at most 16 MiB. A zlib
 * chunk's encoded bytes are at most zlib's bound of its length; an LZO1X
 * or NRV2B chunk is a framed boot-image chunk, at most 65535 encoded bytes
 * decoding to at most 65536.
 */
typedef enum fsimg_chunk_kind {
	FSIMG_CHUNK_RAW = 1,
	FSIMG_CHUNK_ZERO = 2,
	FSIMG_CHUNK_FILL = 3,
	FSIMG_CHUNK_COMPRESSED = 4
} fsimg_chunk_kind_t;

typedef enum fsimg_chunk_codec {
	FSIMG_CODEC_ZLIB = 1,
	FSIMG_CODEC_LZO1X = 2,
	FSIMG_CODEC_NRV2B = 3
} fsimg_chunk_codec_t;

#define FSIMG_CHUNK_MAX_DECODED	(UINT64_C(16) * 1024 * 1024)
/* A framed chunk: a 16-bit stored length and at most 64 KiB decoded. */
#define FSIMG_FRAMED_MAX_ENCODED	UINT64_C(65535)
#define FSIMG_FRAMED_MAX_DECODED	UINT64_C(65536)

/*
 * Decode one framed LZO1X or NRV2B stream, the length bytes in, into out:
 * the one decode of a framed chunk, for the container that learns its
 * index and for the cache that serves it. On FSIMG_OK the stream reached
 * its end marker exactly at its last byte within capacity bytes of
 * output, *produced of them. A stream that wants more output, leaves
 * bytes after its end marker or ends before it is FSIMG_ECORRUPT, the
 * image's failure with the decoder's message, at the file offset of the
 * failing byte, the stream lying at file_offset.
 */
fsimg_status_t fsimg_framed_decode(fsimg_chunk_codec_t, const unsigned char *,
    size_t, uint64_t, unsigned char *, size_t, size_t *, fsimg_error_t *);

typedef struct fsimg_chunk {
	uint64_t	 disk_offset;
	uint64_t	 length;
	uint64_t	 file_offset;
	uint64_t	 encoded_length;
	fsimg_chunk_kind_t kind;
	uint32_t	 argument;
} fsimg_chunk_t;

/* What an index holds: its record count, its largest compressed chunk's
 * decoded length (zero without one), a bit (1u << kind) per kind and a bit
 * (1u << codec) per codec of its compressed chunks.
 */
typedef struct fsimg_chunk_summary {
	uint64_t	 count;
	uint64_t	 max_decoded;
	unsigned	 kinds;
	unsigned	 codecs;
} fsimg_chunk_summary_t;

typedef struct fsimg_chunk_index fsimg_chunk_index_t;

/*
 * new makes an empty index for a disk of disk_bytes whose encoded bytes
 * lie in the file range [data_offset, data_offset + data_bytes); owner
 * names the container in messages and layer is its registry name, which
 * a read's failure carries in error.path; both must outlive the index. Records are
 * charged to the session's scratch. append accepts records in any order
 * and fails with FSIMG_ECORRUPT for a record outside the disk or the data
 * or whose lengths disagree with its kind. seal sorts the records and
 * keeps them only when they tile the disk exactly, FSIMG_ECORRUPT naming
 * an overlap or an uncovered range otherwise. lookup returns the sealed
 * record containing a disk offset. On any failure discard the index.
 */
fsimg_status_t fsimg_chunk_index_new(fsimg_session_t *, const char *,
    const char *, uint64_t, uint64_t, uint64_t, fsimg_chunk_index_t **,
    fsimg_error_t *);
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
/* The kinds and codecs of a summary as text in a fixed order: the kinds
 * but compressed, then the codecs ("raw,zero,fill,zlib,lzo1x,nrv2b").
 */
void fsimg_chunk_kinds_text(unsigned, unsigned, char *, size_t);
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

typedef struct fsimg_spool fsimg_spool_t;

/* What a container reads: the file, its length, and the session that
 * charges its reads and decoding. spool is the image's free spool, given
 * only to the open of a spooled container and NULL for every other call;
 * that open decodes into it and seals it for its disk, and the image
 * keeps the spool until it is freed, after the disk that reads it.
 */
typedef struct fsimg_container_input {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*file;
	fsimg_session_t	*session;
	fsimg_spool_t	*spool;
	uint64_t	 file_bytes;
} fsimg_container_input_t;

/* An opened container: the disk it yields, owned and freed with
 * fsimg_io_free, and what the image reports of it. guid is the VHD unique
 * identifier or the DMG segment ID in textual byte order, all zero for a
 * container without one. kinds and codecs are a summary's bits. details
 * are the header fields info reports, in the order the profile documents.
 */
typedef struct fsimg_container {
	fsimg_io_t	*disk;
	unsigned char	 guid[16];
	uint64_t	 disk_bytes;
	uint64_t	 chunks;
	unsigned	 kinds;
	unsigned	 codecs;
	uint32_t	 detail_count;
	fsimg_volume_detail_t details[FSIMG_IMAGE_DETAILS_MAX];
} fsimg_container_t;

/*
 * The file settings of fsimg_layer_inputs_t (fsimg_file_setting_t), each
 * the catalogue row of that identity in its layer's slice. name gives a
 * setting's qualified key ("ifsboot:startup"), input its I/O object in a
 * caller's record (NULL for an absent record or setting).
 */
void fsimg_file_setting_name(fsimg_file_setting_t, char *, size_t);
fsimg_io_t *fsimg_file_setting_input(const fsimg_layer_inputs_t *,
    fsimg_file_setting_t);
/* Store io as the setting's object in inputs, the inverse of _input. */
void fsimg_file_setting_set(fsimg_layer_inputs_t *, fsimg_file_setting_t,
    fsimg_io_t *);

/*
 * One file setting as the disk plan holds it: the caller's object,
 * borrowed, NULL when the setting is absent, and the host identity
 * recorded before anything read it, when the object has one (known), so
 * the plan can tell after the copy whether the file changed. length is
 * the object's length when the plan read it.
 */
typedef struct fsimg_planned_file {
	fsimg_io_t	*io;
	fsimg_host_file_t identity;
	uint64_t	 length;
	bool		 known;
} fsimg_planned_file_t;

/*
 * A container's resolved write plan (specs/design-container-plans.md).
 * Each writable container embeds this base as the first member of its own
 * private plan type, allocated whole in charged memory by its plan
 * constructor and released by its plan_free; only that container's
 * functions convert the base to the extension. The base holds the facts
 * code outside the container uses. granularity is the unit the disk
 * length is rounded up to; chunk_bytes the transcode's unit of
 * inspection; passes how many ordered passes the transcode makes over the
 * disk, one, or two for an encoder whose leading bytes depend on what the
 * first pass learns; identifier the VHD unique identifier or DMG segment
 * ID in textual byte order, which the image report states, all zero for a
 * container without one; and zlib whether the resolved encoding depends
 * on zlib, for the plan's codec report. Once constructed the plan never
 * changes: what an encoder learns stays in the encoder's own state.
 *
 * Without a container there is no plan, and fsimg_disk_plan_container
 * supplies the defaults fsimg_container_plan_alloc gives a new base: a
 * granularity of one byte, FSIMG_TRANSCODE_CHUNK_BYTES chunks, one pass,
 * no identifier and no zlib.
 */
#define FSIMG_TRANSCODE_CHUNK_BYTES	(UINT32_C(1) << 20)

typedef struct fsimg_container_plan {
	uint64_t	 granularity;
	uint32_t	 chunk_bytes;
	uint32_t	 passes;
	unsigned char	 identifier[16];
	bool		 zlib;
} fsimg_container_plan_t;

/*
 * What a container's plan is constructed from: the context that charges
 * the plan, the operation's session, which charges each read of a file
 * setting and its work, the layer options, the file settings as the disk
 * plan recorded them, the FSIMG_WRITE_* mode and the epoch, and the byte
 * order of the volume the disk carries: the writer's resolved setting, or
 * what a wrap found in its verified input. Every pointer is non-NULL. The
 * constructor retains no pointer to this record, the session or the
 * options.
 */
typedef struct fsimg_container_plan_args {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	const fsimg_layer_options_t *layers;
	const fsimg_planned_file_t *files;	/* FSIMG_FILE_SETTINGS of them */
	int64_t		 epoch;
	uint32_t	 flags;
	fsimg_byte_order_t volume_order;
} fsimg_container_plan_args_t;

/*
 * Allocate a container's private plan of size bytes, whose first member
 * is its fsimg_container_plan_t, in memory charged to ctx: every byte
 * zero, so every pointer the extension owns is NULL, and the base holding
 * the defaults. On success *out holds the base, so the plan's owner frees
 * it on every later outcome; on failure *out stays NULL.
 */
void *fsimg_container_plan_alloc(fsimg_ctx_t *, size_t,
    fsimg_container_plan_t **, fsimg_error_t *);
/* The plan_free of a container whose extension owns no allocation. */
void fsimg_container_plan_release(fsimg_container_plan_t *);
/* Validation control: an ifsboot plan's prefix buffer and its length. */
void fsimg_test_ifsboot_prefix(const fsimg_container_plan_t *,
    const unsigned char **, size_t *);

/*
 * The sequential sink at the end of a transcode (lib/layers/transcode.c):
 * the staged output written at increasing offsets, a caller's object, or a
 * stream descriptor. write appends bytes and zero appends a run of zeros,
 * which a fresh output keeps as a hole; neither ever seeks back.
 * Every byte counts against the output's planned bound, and crossing it is
 * an internal error, so an encoder that misjudged its bound cannot write
 * past what the plan charged. written is the output's length so far.
 */
typedef struct fsimg_seq_sink fsimg_seq_sink_t;

fsimg_status_t fsimg_seq_write(fsimg_seq_sink_t *, const void *, size_t,
    fsimg_error_t *);
fsimg_status_t fsimg_seq_zero(fsimg_seq_sink_t *, uint64_t, fsimg_error_t *);
uint64_t fsimg_seq_written(const fsimg_seq_sink_t *);

/* What a container encoder writes: a disk of disk_bytes, encoded with the
 * immutable plan into sink, charging scratch and work to session; plan is
 * NULL only for the copy encoder of a disk without a container. files are
 * the file settings as the disk plan recorded them, which the encoder
 * copies in where its profile places them. verified_disk says the disk is
 * a wrap's input, verified before the copy, rather than a writer's staged
 * volume, so a check the disk then fails means it changed. Every pointer
 * outlives the encoder's state.
 */
typedef struct fsimg_encode {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_seq_sink_t *sink;
	const fsimg_container_plan_t *plan;
	const fsimg_planned_file_t *files;	/* FSIMG_FILE_SETTINGS of them */
	uint64_t	 disk_bytes;
	bool		 verified_disk;
} fsimg_encode_t;

/*
 * A container encoder, driven by the transcode. open makes the encoder's
 * state, which may not keep encode itself, and may write leading bytes
 * that depend on nothing a pass learns. The transcode then makes the
 * plan's passes over the disk in order; in each it hands every
 * chunk_bytes unit (the last one shorter) to chunk, bytes NULL for a unit
 * of zeros, and calls finish at the pass's end. Only the last pass writes
 * its chunks; an earlier pass, such as simg's count of its chunks,
 * inspects them, and its finish may write what that inspection completes,
 * such as simg's header, before the next pass begins. close releases the
 * state on every outcome, including whatever a failed open left in it; a
 * state still NULL is never closed. The zero decision depends only on the
 * disk's bytes, so the output does too.
 */
typedef struct fsimg_encoder_ops {
	fsimg_status_t	(*open)(const fsimg_encode_t *, void **, fsimg_error_t *);
	fsimg_status_t	(*chunk)(void *, unsigned, const unsigned char *, size_t,
			    fsimg_error_t *);
	fsimg_status_t	(*finish)(void *, unsigned, fsimg_error_t *);
	void		(*close)(void *);
} fsimg_encoder_ops_t;

/*
 * A container's write side, one contract (specs/design-container-plans.md).
 *
 * plan constructs the container's private plan: it validates the
 * container's layer settings and resolves them, its identifier under the
 * identifier rule, given the FSIMG_WRITE_* mode and the epoch, and reads
 * and checks the file settings its profile declares, a bounded one whole
 * into the plan, through the arguments' session. A failure is FSIMG_EINVAL
 * naming the key, or another class the profile names. *out starts NULL;
 * the constructor attaches its plan there through
 * fsimg_container_plan_alloc before any other fallible step, and acquires
 * every later allocation into that plan, so on failure *out is NULL or a
 * partial plan, which the caller frees with plan_free as it would a
 * complete one and never otherwise uses.
 *
 * plan_free releases a non-NULL plan, partial or complete, and everything
 * it owns, exactly once; it reads no file setting and cannot fail.
 *
 * bound states, from the plan alone, the most bytes the encoding of a
 * disk of the given length can take, headers, tables and trailers
 * included, and refuses with FSIMG_EUNSUPPORTED, naming the field, a disk
 * the format cannot describe. It reads, writes and allocates nothing, so
 * repeating it gives the same answer. Every refusal of an argument is the
 * plan's or the bound's, before any sink opens.
 *
 * encoder writes the container with the plan, which it only borrows.
 */
typedef struct fsimg_container_writer {
	fsimg_status_t	(*plan)(const fsimg_container_plan_args_t *,
			    fsimg_container_plan_t **, fsimg_error_t *);
	void		(*plan_free)(fsimg_container_plan_t *);
	fsimg_status_t	(*bound)(const fsimg_container_plan_t *, uint64_t,
			    uint64_t *, fsimg_error_t *);
	const fsimg_encoder_ops_t *encoder;
} fsimg_container_writer_t;

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
 *
 * Three profile facts (specs/design-layers.md#layer-contract). spooled
 * says the payload is one unframed stream that open decodes into the
 * spool, which the input then carries; such a container's disk is that
 * one stream in codecs. carries names the filesystem a loader's package
 * holds, NULL for a disk-image format: a stack that puts a scheme or
 * another filesystem inside it is FSIMG_EINVAL naming the container, and
 * the step after it probes for that filesystem alone. unverified names,
 * comma-separated, the fields no evidence gives a rule to check, NULL for
 * none; verification reports them and is then incomplete.
 *
 * writer is the write side, NULL for a read-only container, which the
 * disk plan refuses and whose capability lacks FSIMG_CAP_WRITE.
 *
 * section locates, in the file, the section a file setting of the
 * container's names (fsimg_layer_section), for unwrap: an offset and a
 * length checked against the file. NULL for a container without file
 * settings.
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
	const fsimg_container_writer_t *writer;
	fsimg_status_t	(*section)(const fsimg_container_input_t *,
			    fsimg_file_setting_t, uint64_t *, uint64_t *,
			    fsimg_error_t *);
	const char	*codecs;
	const char	*carries;
	const char	*unverified;
	bool		 payload_checks;
	bool		 zlib;
	bool		 spooled;
} fsimg_container_ops_t;

/* The CRC32 of crc's bytes followed by length zero bytes, computed in time
 * logarithmic in length rather than by reading the zeros.
 */
unsigned long fsimg_crc32_zeros(unsigned long, uint64_t);

/*
 * The spool (specs/design-layers.md#stream-input-and-the-spool): the
 * handed-over file that holds a stream or a decompressed input. adopt
 * takes the descriptor on every outcome and accepts only an empty,
 * unlinked, read/write regular file. append reserves its length against
 * the context's spool limit, then grows the file and writes the bytes,
 * skipping blocks of zeros, which stay holes; so a decompression bomb
 * fails with FSIMG_RESOURCE_SPOOL at the append that would cross the
 * limit, before the file grows past it. seal samples the allocated
 * blocks and yields a read-only view of the bytes appended, which the
 * spool must outlive. free releases the reservation and the file.
 */
fsimg_status_t fsimg_spool_adopt(fsimg_ctx_t *, int, fsimg_spool_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_spool_append(fsimg_spool_t *, const unsigned char *,
    size_t, fsimg_error_t *);
fsimg_status_t fsimg_spool_seal(fsimg_spool_t *, fsimg_io_t **,
    fsimg_error_t *);
void fsimg_spool_free(fsimg_spool_t *);

/*
 * The input of a compression or a spool fill, read once from its start:
 * first the bytes the probes already read (prefix), then the rest of a
 * seekable file through session from offset, or of a stream descriptor
 * (fd, file NULL) until its end. A decoder charges its work to session
 * for either. read fills at most length bytes and
 * transfers zero only at the end of the input; it polls cancellation.
 */
typedef struct fsimg_spool_input {
	fsimg_ctx_t	*ctx;
	const unsigned char *prefix;
	size_t		 prefix_length;
	fsimg_io_t	*file;
	fsimg_session_t	*session;
	uint64_t	 offset;
	uint64_t	 file_bytes;
	int		 fd;
} fsimg_spool_input_t;

fsimg_status_t fsimg_spool_input_read(fsimg_spool_input_t *, unsigned char *, size_t,
    size_t *, fsimg_error_t *);
/* Copy the whole source into the spool unchanged. */
fsimg_status_t fsimg_spool_fill(fsimg_spool_t *, fsimg_spool_input_t *,
    fsimg_error_t *);

/*
 * A compression's write settings, resolved once by the disk plan: level
 * is the codec's level, and timestamp the modification time in Unix
 * seconds, the epoch under FSIMG_WRITE_REPRODUCIBLE and the host clock
 * otherwise. An encoder writes a time its format cannot hold as the
 * format's "no time" value, which for gzip is zero.
 */
typedef struct fsimg_compression_settings {
	int64_t		 timestamp;
	uint32_t	 level;
} fsimg_compression_settings_t;

/*
 * A compression encoder: a sequential sink wrapping the next one
 * (specs/design-layers.md#transcode). open makes its state over sink,
 * which outlives it, charging its memory to ctx, and may write the
 * format's fixed header, nothing that depends on input not yet seen;
 * write encodes length bytes, or length zeros when bytes is NULL; finish
 * completes the encoding and writes what ends it. close releases the
 * state on every outcome, including whatever a failed open left in it;
 * a state still NULL is never closed.
 */
typedef struct fsimg_compressor_ops {
	fsimg_status_t	(*open)(fsimg_ctx_t *, const fsimg_compression_settings_t *,
			    fsimg_seq_sink_t *, void **, fsimg_error_t *);
	fsimg_status_t	(*write)(void *, const unsigned char *, uint64_t,
			    fsimg_error_t *);
	fsimg_status_t	(*finish)(void *, fsimg_error_t *);
	void		(*close)(void *);
} fsimg_compressor_ops_t;

/*
 * A whole-file compression. probe follows the bounded probe contract on
 * at most probe_bytes of the file's prefix: FSIMG_EFORMAT without its
 * identity, otherwise a claim. decode reads the whole source and appends
 * the decoded bytes to the spool, checking every member as it ends and
 * charging every compressed byte it reads, headers and trailers included,
 * and every byte it produces to the source's session as work; a
 * corrupt input is FSIMG_ECORRUPT with FSIMG_OP_IMAGE, and any failure is
 * final.
 *
 * The write side. plan validates the compression's layer settings and
 * resolves them, given the FSIMG_WRITE_* mode and the epoch; a failure is
 * FSIMG_EINVAL naming the key. bound states the most bytes the encoding of
 * an input of the given length can take, FSIMG_EUNSUPPORTED when that
 * exceeds the 64-bit range. encoder writes the compression. zlib says the
 * codec is zlib's, whose version the capability record and the plan
 * report.
 */
typedef struct fsimg_compression_ops {
	size_t		 probe_bytes;
	fsimg_status_t	(*probe)(const unsigned char *, size_t, fsimg_error_t *);
	fsimg_status_t	(*decode)(fsimg_spool_input_t *, fsimg_spool_t *,
			    fsimg_error_t *);
	fsimg_status_t	(*plan)(const fsimg_layer_options_t *, uint32_t, int64_t,
			    fsimg_compression_settings_t *, fsimg_error_t *);
	fsimg_status_t	(*bound)(uint64_t, uint64_t *, fsimg_error_t *);
	const fsimg_compressor_ops_t *encoder;
	bool		 zlib;
} fsimg_compression_ops_t;

/* A registered layer. Every layer name the stack grammar accepts outside
 * the filesystem slot is one record; a scheme has scheme operations, a
 * container container operations and a compression compression
 * operations, all NULL for a layer this build recognizes but does not
 * implement. settings is the layer's slice of the settings catalogue, the
 * one statement of the keys it advertises and accepts, NULL for none.
 */
typedef struct fsimg_layer {
	const char	*name;
	const char	*suffix;	/* File suffix of a container or compression */
	fsimg_layer_kind_t kind;
	const fsimg_scheme_ops_t *scheme;
	const fsimg_container_ops_t *container;
	const fsimg_compression_ops_t *compression;
	const fsimg_setting_slice_t *settings;
} fsimg_layer_t;

extern const fsimg_scheme_ops_t fsimg_gpt_scheme;
extern const fsimg_scheme_ops_t fsimg_mbr_scheme;
extern const fsimg_container_ops_t fsimg_dmg_container;
extern const fsimg_container_ops_t fsimg_vhd_container;
extern const fsimg_container_ops_t fsimg_simg_container;
extern const fsimg_container_ops_t fsimg_hbcifs_container;
extern const fsimg_container_ops_t fsimg_ifsboot_container;
extern const fsimg_compression_ops_t fsimg_gzip_compression;

/* The registry, in reporting order; NULL past the end. */
const fsimg_layer_t *fsimg_layer_at(size_t);
/* The FSIMG_CAP_* bits a layer's operations support. */
uint32_t fsimg_layer_capabilities(const fsimg_layer_t *);
const fsimg_layer_t *fsimg_layer_find(const char *);
/* Whether a layer, which may be NULL, declares the section name, the
 * unqualified key of one of its file settings, and which setting it is.
 */
bool fsimg_layer_section(const fsimg_layer_t *, const char *,
    fsimg_file_setting_t *);
/* Check a caller's stack value: states in range, names terminated, each
 * named slot holding a layer of its kind or a filesystem name, and a
 * container that carries a filesystem holding nothing else.
 */
fsimg_status_t fsimg_stack_validate(const fsimg_stack_t *, fsimg_error_t *);
/* A stack against a container that carries a filesystem: FSIMG_EINVAL
 * naming the container when the stack names a scheme or a filesystem
 * other than the carried one, and FSIMG_OK for any other container or
 * none. fsimg_stack_validate() applies it to the container the stack
 * names; reading applies it again to a container it detects.
 */
fsimg_status_t fsimg_stack_carried(const fsimg_stack_t *,
    const fsimg_layer_t *, fsimg_error_t *);
/* The layer named in a stack slot, NULL when the slot names none. */
const fsimg_layer_t *fsimg_stack_layer(const fsimg_stack_t *,
    fsimg_layer_kind_t);
/*
 * The compression a caller's options name in a field of size bytes, for
 * writing or reading: NULL for an empty name, else the registered
 * compression. An unterminated name, or one the registry lacks as a
 * compression, is FSIMG_EINVAL; a registered compression without the
 * operation is FSIMG_EUNSUPPORTED; each names it in error.path.
 */
fsimg_status_t fsimg_compression_named(const char *, size_t, bool,
    const fsimg_layer_t **, fsimg_error_t *);
/*
 * Check layer options and file settings (inputs, NULL for none) against a
 * stack and the compression the operation names (NULL for none), visiting
 * every registered layer's catalogue slice in order: the options'
 * reserved words zero, each setting structurally valid, every setting of
 * a layer neither names at its default, and on read (writing false) every
 * setting outside FSIMG_SCOPE_READ at its default. A failure is
 * FSIMG_EINVAL naming the key in error.path.
 */
fsimg_status_t fsimg_layer_options_check(const fsimg_stack_t *,
    const fsimg_layer_t *, const fsimg_layer_options_t *,
    const fsimg_layer_inputs_t *, bool, fsimg_error_t *);
/* Every FSIMG_SCOPE_PARTITION setting at its default, as compose requires;
 * a failure is FSIMG_EINVAL naming the key.
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
/* The selector of a partition to be written: the type text when nonempty,
 * else the type-table entry of the filesystem, which may be NULL; with
 * neither, FSIMG_EINVAL.
 */
fsimg_status_t fsimg_partition_type_select(const char *, const char *,
    fsimg_type_selector_t *, fsimg_error_t *);
/* Give a GPT record to be written the selected type's GUID; name receives
 * the type's conventional GPT name, empty for none. A selector GPT cannot
 * express fails with FSIMG_EINVAL.
 */
fsimg_status_t fsimg_partition_type_gpt(const fsimg_type_selector_t *,
    fsimg_partition_record_t *, const char **, fsimg_error_t *);
/* Give an MBR record to be written the selected type's written byte,
 * which must not be an extended or GPT protective type; a selector MBR
 * cannot express fails with FSIMG_EINVAL.
 */
fsimg_status_t fsimg_partition_type_mbr(const fsimg_type_selector_t *,
    fsimg_partition_record_t *, fsimg_error_t *);
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
/* Whether a 16-byte identifier is all zero, the unset value. */
bool fsimg_guid_unset(const unsigned char *);

/*
 * A disk to write, built in three steps (specs/design-scheme-planning.md).
 * init resolves the stack's container, constructing its private plan, and
 * the compression its request names, a write options' compression field
 * (empty for none), with its settings; it records each file setting's
 * identity before the container's plan reads it. It then selects the
 * stack's scheme, whose plan_disk resolves the disk-level settings, and
 * makes count empty records. partition has the scheme resolve each
 * record's metadata; place lays the records out in slot order, each at
 * the first 1 MiB boundary after the previous one, sets the disk length
 * and has the scheme validate the result. Without a scheme the plan has
 * no records and the volume starts the disk. init zeroes the plan before
 * anything can fail, so a plan init refuses, which may hold a partial
 * container plan, is still safe to free. place rounds the disk up to the
 * container's granularity, puts the scheme's trailing tables at the end
 * of the rounded disk, and states the output's bound: the container's
 * bound of the disk (container_bytes), then the compression's bound of
 * that. Records and the container plan are charged to the context; free
 * releases them, the container plan through the container's plan_free,
 * and leaves the plan owning nothing. A plan has one owner and is planned
 * in place, never copied. Common planning reads no scheme's settings and
 * compares no scheme's name: only the scheme's operations set its
 * metadata.
 */
typedef struct fsimg_disk_plan {
	const fsimg_layer_t *scheme;
	const fsimg_layer_t *container;
	const fsimg_layer_t *compression;
	fsimg_scheme_table_t table;
	fsimg_container_plan_t *container_plan;	/* Owned; NULL without one */
	fsimg_compression_settings_t compression_settings;
	fsimg_planned_file_t files[FSIMG_FILE_SETTINGS];
	uint64_t	 disk_bytes;	/* A fixed length until placed, else 0 */
	uint64_t	 padding_bytes;	/* What the container's granularity added */
	uint64_t	 container_bytes; /* The disk, or its container's bound */
	uint64_t	 output_bytes;	/* container_bytes, or its compressed bound */
	int64_t		 epoch;
	uint32_t	 flags;		/* FSIMG_WRITE_* mode */
	const char	*disk_key;	/* The scheme's disk-length key, static */
	bool		 fixed;		/* The scheme's disk setting set the length */
	bool		 verified_disk;	/* The disk is a verified input */
} fsimg_disk_plan_t;

/*
 * What a disk plan is made from: the operation's session, which charges
 * the container's reads of its file settings and continues through the
 * output, the stack and its layer options, the file settings (NULL for
 * none), the compression a write options' field names, the mode and
 * epoch, the number of partitions a scheme holds, the byte order of the
 * volume a carried-volume container packages, and whether the disk is an
 * input verified before the copy (a wrap's) rather than a writer's staged
 * volume. The plan keeps no pointer to the request or the session.
 */
typedef struct fsimg_disk_request {
	fsimg_session_t	*session;
	const fsimg_stack_t *stack;
	const fsimg_layer_options_t *layers;
	const fsimg_layer_inputs_t *inputs;
	const char	*compression;
	int64_t		 epoch;
	uint32_t	 flags;
	uint32_t	 partitions;
	fsimg_byte_order_t volume_order;
	bool		 verified_disk;
} fsimg_disk_request_t;

/* Whether the output is an encoding of the disk rather than the disk: a
 * container or a compression is present, so the output is written by the
 * transcode from a raw intermediate or an assembled disk.
 */
bool fsimg_disk_plan_encoded(const fsimg_disk_plan_t *);
/* The plan's container facts: its container plan's base, or without a
 * container the defaults (fsimg_container_plan_t).
 */
const fsimg_container_plan_t *fsimg_disk_plan_container(
    const fsimg_disk_plan_t *);

fsimg_status_t fsimg_disk_plan_init(fsimg_ctx_t *, const fsimg_disk_request_t *,
    fsimg_disk_plan_t *, fsimg_error_t *);
/*
 * The scheme stage of init: give a plan that has no scheme yet the scheme
 * (NULL leaves it unpartitioned) with count partitions, its disk settings
 * resolved from layers by the scheme's plan_disk. A read-only scheme is
 * FSIMG_EUNSUPPORTED naming it before any of its operations runs, and a
 * count outside 1 to its slots FSIMG_EINVAL with the limit. init calls it
 * with the stack's scheme; a test plans an unregistered descriptor by
 * calling it on the plan of a stack without one; such a plan has not
 * passed the exact-volume rule, which init applies to the stack's scheme.
 */
fsimg_status_t fsimg_disk_plan_scheme(fsimg_ctx_t *, fsimg_disk_plan_t *,
    const fsimg_layer_t *, const fsimg_layer_options_t *, uint32_t,
    fsimg_error_t *);
/*
 * The common checks of the plan's file settings against their host files:
 * unchanged compares each recorded identity with the file's now, after the
 * copy, FSIMG_ESOURCE_CHANGED naming the key on a replacement, a size
 * change or a change-metadata difference; aliases refuses, FSIMG_EINVAL
 * naming the key, a file setting that is the output's file.
 */
fsimg_status_t fsimg_disk_plan_inputs_unchanged(const fsimg_disk_plan_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_disk_plan_input_aliases(const fsimg_disk_plan_t *,
    const fsimg_host_file_t *, fsimg_error_t *);
/*
 * Resolve the partition in slot (from 1) through the scheme's
 * plan_partition, from exactly one of the layer options of a create or
 * wrap and a compose part, with the filesystem that types it by default,
 * NULL for none. Both or neither source is FSIMG_ESTATE.
 */
fsimg_status_t fsimg_disk_plan_partition(fsimg_disk_plan_t *, uint32_t,
    const fsimg_layer_options_t *, const fsimg_partition_request_t *,
    const char *, fsimg_error_t *);
/* Fit a volume to a fixed disk's one partition: a zero *volume_bytes
 * becomes the longest multiple of granule bytes the partition holds, and a
 * requested length longer than that is FSIMG_EINVAL naming the disk key.
 */
fsimg_status_t fsimg_disk_plan_fit(const fsimg_disk_plan_t *, uint32_t,
    uint64_t *, fsimg_error_t *);
/* Place records whose volumes have the given lengths; with a fixed disk
 * they must fit within it. The final disk, once its container's bounds
 * are known, must pass the scheme's validate_plan.
 */
fsimg_status_t fsimg_disk_plan_place(fsimg_disk_plan_t *, const uint64_t *,
    fsimg_error_t *);
/* The placed plan as an opened image of it would report its disk, its
 * container and the partition in slot (from 1).
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
/* Check a placed plan against the output limit: the output's bound, plus
 * the disk when raw says a raw intermediate holds it while the output is
 * written from it, as for an encoded output or a stream
 * (specs/architecture.md#bounded-resources).
 */
fsimg_status_t fsimg_disk_plan_budget(fsimg_ctx_t *, const fsimg_disk_plan_t *,
    bool, fsimg_error_t *);

/*
 * Write a placed plan's disk, held in the positional object disk, to a sink
 * in one sequential pass (lib/layers/transcode.c): through the plan's
 * container encoder, or unchanged without one, and then through its
 * compression encoder when it has one. A FILE sink stages the
 * output and publishes it through the output coordinator, an IO sink is
 * written from offset zero and ends at the output's length, and a STREAM
 * sink receives the bytes in order. The pass reads the disk in chunks,
 * may skip reading the chunks the disk reports zero, and polls cancellation
 * at every chunk. Failure after the first mutation marks the output
 * incomplete, except that a staged file is removed and so is absent.
 * written receives the output's length. The caller has checked aliases
 * and the output budget, and adopted the sink.
 */
fsimg_status_t fsimg_disk_output(fsimg_session_t *, fsimg_io_t *,
    const fsimg_disk_plan_t *, const fsimg_sink_t *, bool, uint64_t *,
    fsimg_error_t *);
/* The compression library a plan's output depends on, as "zlib 1.3.1",
 * or empty when its bytes depend on none: DMG's zlib chunks or gzip.
 */
void fsimg_disk_plan_codec(const fsimg_disk_plan_t *, char *, size_t);

/* What the filesystem probes, the one forced by name or all of them, say
 * about the volume at a byte range of an I/O object; format receives the
 * identified format's name, empty without a match.
 */
fsimg_status_t fsimg_volume_claim(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, uint64_t, uint64_t, const char *, fsimg_claim_t *,
    char [static 16], fsimg_error_t *);
/* A read-only slice of an opened image's file over the container section
 * of that name (lib/layers/image.c), for unwrap.
 */
fsimg_status_t fsimg_image_section(fsimg_image_t *, const char *,
    fsimg_io_t **, fsimg_error_t *);
/* The byte order of a volume of length bytes that the named format claims,
 * from the format's byte-order operation on its probe prefix; little for
 * a format that has only one.
 */
fsimg_status_t fsimg_volume_byte_order(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, uint64_t, const char *, fsimg_byte_order_t *,
    fsimg_error_t *);

/* FSIMG_EINVAL naming the setting ("vhd:align") or partition it concerns
 * in error.path.
 */
fsimg_status_t fsimg_setting_error(fsimg_error_t *, const char *,
    const char *);

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
/*
 * A container identifier under the identifier rule: the setting when given
 * (not all zero), else under FSIMG_WRITE_REPRODUCIBLE the GUID derived from
 * the epoch and role, else FSIMG_EINVAL naming key.
 */
fsimg_status_t fsimg_container_identifier(const unsigned char *,
    const char *, fsimg_identifier_role_t, uint32_t, int64_t,
    unsigned char [static 16], fsimg_error_t *);

#endif
