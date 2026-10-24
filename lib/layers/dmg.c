/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "fsimg-endian.h"
#include "io.h"
#include "layer.h"
#include "meta.h"
#include "zlib-memory.h"

/*
 * Apple UDIF disk images (DMG). The file is a data fork of chunks, an XML
 * property list and a 512-byte "koly" trailer, all big-endian. The trailer
 * locates the data fork and the plist and states the disk's sector count;
 * the plist's resource-fork dict holds a blkx array whose entries carry, as
 * base64 data, "mish" block tables. Each table covers a run of disk
 * sectors and lists its runs: zero fill, raw bytes, zlib chunks and the
 * like, each a sector range relative to the table and a byte range of the
 * data fork. Opening walks every table once and appends one chunk record
 * per run to the chunk index, which proves that the runs tile the disk.
 *
 * hdiutil writes one table per partition-map region; fsimg reads any
 * number of tables in any order. A file whose sectors no run covers is
 * corrupt rather than read as zeros. fsimg writes one table for the whole
 * disk.
 */
#define DMG_SECTOR_BYTES	512u
#define KOLY_VERSION		4u
#define KOLY_HEADER_BYTES	512u
#define MISH_HEADER_BYTES	204u
#define MISH_RUN_BYTES		40u
#define MISH_VERSION		1u
#define CHECKSUM_NONE		0u
#define CHECKSUM_CRC32		2u
#define VERIFY_PIECE_BYTES	(64u * 1024)
#define DMG_FLATTENED		1u
#define DMG_WHOLE_DISK		UINT32_C(0xfffffffe)
#define DMG_VARIANT		2u
#define DMG_DEFAULT_LEVEL	6u
#define DMG_BASE64_LINE		44u	/* Characters per line, as hdiutil */
#define DMG_BASE64_INDENT	4u	/* Tabs before each line */
#define DMG_REPLAY_BYTES	(MISH_RUN_BYTES * 1024u)

/* Trailer fields. */
enum {
	KOLY_SIGNATURE = 0,
	KOLY_VERSION_FIELD = 4,
	KOLY_HEADER_SIZE = 8,
	KOLY_FLAGS = 12,
	KOLY_DATA_OFFSET = 24,
	KOLY_DATA_LENGTH = 32,
	KOLY_SEGMENT_NUMBER = 56,
	KOLY_SEGMENT_COUNT = 60,
	KOLY_SEGMENT_ID = 64,
	KOLY_DATA_CHECKSUM = 80,
	KOLY_XML_OFFSET = 216,
	KOLY_XML_LENGTH = 224,
	KOLY_MASTER_CHECKSUM = 352,
	KOLY_VARIANT = 488,
	KOLY_SECTOR_COUNT = 492
};

/* Block table fields. */
enum {
	MISH_SIGNATURE = 0,
	MISH_VERSION_FIELD = 4,
	MISH_FIRST_SECTOR = 8,
	MISH_SECTOR_COUNT = 16,
	MISH_DATA_OFFSET = 24,
	MISH_BUFFERS = 32,
	MISH_DESCRIPTOR = 36,
	MISH_CHECKSUM = 64,
	MISH_RUN_COUNT = 200
};

/* Run fields and types. */
enum {
	RUN_TYPE = 0,
	RUN_SECTOR = 8,
	RUN_SECTORS = 16,
	RUN_COMPRESSED_OFFSET = 24,
	RUN_COMPRESSED_LENGTH = 32
};

#define RUN_ZERO	UINT32_C(0x00000000)
#define RUN_RAW		UINT32_C(0x00000001)
#define RUN_IGNORE	UINT32_C(0x00000002)
#define RUN_ADC		UINT32_C(0x80000004)
#define RUN_ZLIB	UINT32_C(0x80000005)
#define RUN_BZIP2	UINT32_C(0x80000006)
#define RUN_LZFSE	UINT32_C(0x80000007)
#define RUN_LZMA	UINT32_C(0x80000008)
#define RUN_COMMENT	UINT32_C(0x7ffffffe)
#define RUN_TERMINATOR	UINT32_C(0xffffffff)

/* A checksum as UDIF stores it: a type, a bit size and, for CRC32, the
 * first of its 32-bit values.
 */
typedef struct dmg_checksum {
	uint32_t	 type;
	uint32_t	 bits;
	uint32_t	 value;
} dmg_checksum_t;

/* The decoded trailer. */
typedef struct dmg_trailer {
	unsigned char	 segment_id[16];
	dmg_checksum_t	 data_checksum;
	dmg_checksum_t	 master_checksum;
	uint64_t	 data_offset;
	uint64_t	 data_bytes;
	uint64_t	 xml_offset;
	uint64_t	 xml_bytes;
	uint64_t	 disk_bytes;
	uint32_t	 segment_number;
	uint32_t	 segment_count;
} dmg_trailer_t;

/* A decoded block table header; sectors are 512 bytes. */
typedef struct dmg_table {
	dmg_checksum_t	 checksum;
	uint64_t	 first_sector;
	uint64_t	 sector_count;
	uint64_t	 data_offset;	/* Within the data fork */
	uint32_t	 run_count;	/* Terminator included */
} dmg_table_t;

/* One run; sector is relative to its table, the compressed offset to the
 * table's data offset.
 */
typedef struct dmg_run {
	uint64_t	 sector;
	uint64_t	 sectors;
	uint64_t	 compressed_offset;
	uint64_t	 compressed_length;
	uint32_t	 type;
} dmg_run_t;

/* What a walk over the block tables does with each one. table receives a
 * header, run each run in table order, and table_end the table once all
 * its runs have arrived and their sectors add up.
 */
typedef struct dmg_table_visitor {
	fsimg_status_t	(*table)(void *, const dmg_table_t *, fsimg_error_t *);
	fsimg_status_t	(*run)(void *, const dmg_table_t *, const dmg_run_t *,
			    fsimg_error_t *);
	fsimg_status_t	(*table_end)(void *, const dmg_table_t *,
			    fsimg_error_t *);
	void		*argument;
} dmg_table_visitor_t;

/*
 * Where the walk is in the plist. Each place on the path to a table is the
 * element at one depth, so a place's value is its depth: the root dict at
 * 2 (inside plist at 1), the resource-fork dict at 3, the blkx array at 4,
 * an entry's dict at 5 and its Data at 6.
 */
typedef enum walk_place {
	PLACE_DOCUMENT = 1,
	PLACE_ROOT = 2,
	PLACE_RESOURCES = 3,
	PLACE_BLKX = 4,
	PLACE_ENTRY = 5,
	PLACE_TABLE = 6
} walk_place_t;

/* The key the next value of the dict at the current place follows. */
typedef enum walk_key {
	KEY_OTHER,
	KEY_RESOURCE_FORK,
	KEY_BLKX,
	KEY_DATA
} walk_key_t;

/*
 * A walk's state: the innermost path element entered (place) and the
 * depth of the element open now, the key naming the next value of the
 * path element's dict, and the table being decoded from base64 pieces,
 * header first and then whole runs. Only depths place and place + 1
 * matter: at place + 1 an element is a key of the path element or a
 * value, which either enters the path or is passed over with everything
 * inside it, deeper than place + 1.
 */
typedef struct dmg_walk {
	const dmg_trailer_t *trailer;
	const dmg_table_visitor_t *visitor;
	walk_place_t	 place;
	walk_key_t	 key;
	unsigned	 depth;
	bool		 in_key;	/* A key of the path element is open */
	unsigned char	 header[MISH_HEADER_BYTES];
	unsigned char	 run[MISH_RUN_BYTES];
	size_t		 header_length;
	size_t		 run_length;
	dmg_table_t	 table;
	uint64_t	 table_sectors;	/* Sum of the runs' sectors so far */
	uint32_t	 runs_seen;
	bool		 terminated;
} dmg_walk_t;

/* The open's table visitor: every run becomes a chunk record. */
typedef struct dmg_open {
	const dmg_trailer_t *trailer;
	fsimg_chunk_index_t *index;
} dmg_open_t;

/* The verification's table visitor: per-table CRC32s over the decoded
 * bytes under read-through, and the master checksum over the tables'
 * checksum values.
 */
typedef struct dmg_verify {
	fsimg_io_t	*disk;
	fsimg_session_t	*session;
	unsigned char	*piece;
	uLong		 master;
	uLong		 table_crc;
	uint64_t	 tables;
	bool		 payload;
} dmg_verify_t;

/* The private plan: the zlib level, zero for raw chunks. The base holds
 * the chunk, which is dmg:chunk, and the segment ID.
 */
typedef struct dmg_plan {
	fsimg_container_plan_t base;
	uint32_t	 level;
} dmg_plan_t;

/*
 * The encoder's state. The next run starts at sector, after the pending
 * zero run of zero_sectors that ends the disk described so far; runs holds
 * the run_count recorded runs, encoded. table_crc covers the disk's bytes
 * and fork_crc the data fork's. Under zlib, stream deflates one chunk at a
 * time into packed, and allocates through zlib_memory. carry and line hold
 * the base64 text of the table not yet written.
 */
typedef struct dmg_encoder {
	fsimg_ctx_t	*ctx;
	fsimg_seq_sink_t *sink;
	const dmg_plan_t *plan;
	uint64_t	 disk_bytes;
	fsimg_store_t	*runs;
	uint64_t	 run_count;
	uint64_t	 sector;
	uint64_t	 zero_sectors;
	uLong		 table_crc;
	uLong		 fork_crc;
	z_stream	 stream;
	fsimg_zlib_memory_t zlib_memory;
	bool		 stream_ready;
	unsigned char	*packed;
	unsigned char	 carry[3];
	unsigned	 carried;
	char		 line[DMG_BASE64_LINE];
	size_t		 column;
} dmg_encoder_t;

/*
 * The plist template around the base64 table, in hdiutil's layout: tab
 * indentation, one blkx entry whose name says it describes the whole disk,
 * and nothing that varies between runs.
 */
static const char plist_head[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>resource-fork</key>\n"
    "\t<dict>\n"
    "\t\t<key>blkx</key>\n"
    "\t\t<array>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>Attributes</key>\n"
    "\t\t\t\t<string>0x0050</string>\n"
    "\t\t\t\t<key>CFName</key>\n"
    "\t\t\t\t<string>whole disk (fsimg : 0)</string>\n"
    "\t\t\t\t<key>Data</key>\n"
    "\t\t\t\t<data>\n";
static const char plist_tail[] =
    "\t\t\t\t</data>\n"
    "\t\t\t\t<key>ID</key>\n"
    "\t\t\t\t<string>0</string>\n"
    "\t\t\t\t<key>Name</key>\n"
    "\t\t\t\t<string>whole disk (fsimg : 0)</string>\n"
    "\t\t\t</dict>\n"
    "\t\t</array>\n"
    "\t</dict>\n"
    "</dict>\n"
    "</plist>\n";

static void checksum_decode(const unsigned char *, dmg_checksum_t *);
static fsimg_status_t dmg_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t dmg_probe(const unsigned char *, size_t,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static fsimg_status_t trailer_decode(const unsigned char *, uint64_t,
    dmg_trailer_t *, fsimg_error_t *);
static fsimg_status_t read_trailer(const fsimg_container_input_t *,
    dmg_trailer_t *, fsimg_error_t *);
static fsimg_status_t table_header(dmg_walk_t *, fsimg_error_t *);
static fsimg_status_t table_run(dmg_walk_t *, fsimg_error_t *);
static fsimg_status_t table_finish(dmg_walk_t *, fsimg_error_t *);
static fsimg_status_t walk_open(void *, fsimg_plist_element_t,
    fsimg_error_t *);
static fsimg_status_t walk_text(void *, const char *, size_t,
    fsimg_error_t *);
static fsimg_status_t walk_data(void *, const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t walk_close(void *, fsimg_plist_element_t,
    fsimg_error_t *);
static fsimg_status_t walk_tables(const fsimg_container_input_t *,
    const dmg_trailer_t *, const dmg_table_visitor_t *, fsimg_error_t *);
static const char *unsupported_codec(uint32_t);
static fsimg_status_t open_table(void *, const dmg_table_t *,
    fsimg_error_t *);
static fsimg_status_t open_run(void *, const dmg_table_t *,
    const dmg_run_t *, fsimg_error_t *);
static fsimg_status_t open_table_end(void *, const dmg_table_t *,
    fsimg_error_t *);
static fsimg_status_t dmg_open(const fsimg_container_input_t *,
    fsimg_container_t *, fsimg_error_t *);
static fsimg_status_t verify_table(void *, const dmg_table_t *,
    fsimg_error_t *);
static fsimg_status_t verify_run(void *, const dmg_table_t *,
    const dmg_run_t *, fsimg_error_t *);
static fsimg_status_t verify_table_end(void *, const dmg_table_t *,
    fsimg_error_t *);
static fsimg_status_t verify_data_fork(const fsimg_container_input_t *,
    const dmg_trailer_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t dmg_verify(const fsimg_container_input_t *,
    const fsimg_container_t *, bool, fsimg_error_t *);
static fsimg_status_t dmg_plan(const fsimg_container_plan_args_t *,
    fsimg_container_plan_t **, fsimg_error_t *);
static uint64_t plist_bytes(uint64_t);
static fsimg_status_t dmg_bound(const fsimg_container_plan_t *, uint64_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t dmg_zlib_failed(const fsimg_zlib_memory_t *, int,
    const char *, fsimg_error_t *);
static fsimg_status_t dmg_encode_open(const fsimg_encode_t *, void **,
    fsimg_error_t *);
static fsimg_status_t record_run(dmg_encoder_t *, uint32_t, uint64_t,
    uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t record_zero_run(dmg_encoder_t *, fsimg_error_t *);
static fsimg_status_t deflate_chunk(dmg_encoder_t *, const unsigned char *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t dmg_encode_chunk(void *, unsigned,
    const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t base64_line(dmg_encoder_t *, fsimg_error_t *);
static fsimg_status_t base64_group(dmg_encoder_t *, fsimg_error_t *);
static fsimg_status_t base64_put(dmg_encoder_t *, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t base64_end(dmg_encoder_t *, fsimg_error_t *);
static fsimg_status_t write_table(dmg_encoder_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t write_trailer(dmg_encoder_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t dmg_encode_finish(void *, unsigned, fsimg_error_t *);
static void dmg_encode_close(void *);

static const fsimg_encoder_ops_t dmg_encoder = {
	.open = dmg_encode_open,
	.chunk = dmg_encode_chunk,
	.finish = dmg_encode_finish,
	.close = dmg_encode_close
};

static const fsimg_container_writer_t dmg_writer = {
	.plan = dmg_plan,
	.plan_free = fsimg_container_plan_release,
	.bound = dmg_bound,
	.encoder = &dmg_encoder
};

const fsimg_container_ops_t fsimg_dmg_container = {
	.probe_bytes = 0,
	.probe = dmg_probe,
	.open = dmg_open,
	.verify = dmg_verify,
	.writer = &dmg_writer,
	.codecs = "raw,zero,zlib",
	.payload_checks = true,
	.zlib = true
};

static void
checksum_decode(const unsigned char *raw, dmg_checksum_t *checksum)
{
	checksum->type = fsimg_load_be32(raw);
	checksum->bits = fsimg_load_be32(raw + 4);
	checksum->value = fsimg_load_be32(raw + 8);
}

static fsimg_status_t
dmg_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "DMG %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * The identity: "koly", version 4 and header size 512 in the last 512
 * bytes, and the plist ending exactly where the trailer begins, which ties
 * the trailer to this file's length. A bare volume whose last sector holds
 * a stray trailer of another file's length draws no claim.
 */
static fsimg_status_t
dmg_probe(const unsigned char *prefix FSIMG_UNUSED, size_t prefix_length
    FSIMG_UNUSED, const unsigned char *tail, size_t tail_length,
    uint64_t file_bytes, fsimg_error_t *error)
{
	uint64_t xml_end;

	if (tail_length < KOLY_HEADER_BYTES || file_bytes < KOLY_HEADER_BYTES ||
	    bcmp(tail + KOLY_SIGNATURE, "koly", 4) != 0 ||
	    fsimg_load_be32(tail + KOLY_VERSION_FIELD) != KOLY_VERSION ||
	    fsimg_load_be32(tail + KOLY_HEADER_SIZE) != KOLY_HEADER_BYTES ||
	    !fsimg_add_u64(fsimg_load_be64(tail + KOLY_XML_OFFSET),
	    fsimg_load_be64(tail + KOLY_XML_LENGTH), &xml_end) ||
	    xml_end != file_bytes - KOLY_HEADER_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no DMG trailer"));
	return (FSIMG_OK);
}

/*
 * Decode a trailer the probe claimed and validate what reading relies on:
 * the data fork lies before the plist, the image is one segment, the
 * sector count gives a disk length, and the checksums are of a known type.
 * hdiutil create writes segment number and count 0 and hdiutil convert
 * writes 1; both describe a single-segment image.
 */
static fsimg_status_t
trailer_decode(const unsigned char *raw, uint64_t file_bytes,
    dmg_trailer_t *trailer, fsimg_error_t *error)
{
	uint64_t data_end, sectors;
	fsimg_status_t status;

	status = dmg_probe(NULL, 0, raw, KOLY_HEADER_BYTES, file_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(trailer, sizeof(*trailer));
	trailer->data_offset = fsimg_load_be64(raw + KOLY_DATA_OFFSET);
	trailer->data_bytes = fsimg_load_be64(raw + KOLY_DATA_LENGTH);
	trailer->xml_offset = fsimg_load_be64(raw + KOLY_XML_OFFSET);
	trailer->xml_bytes = fsimg_load_be64(raw + KOLY_XML_LENGTH);
	trailer->segment_number = fsimg_load_be32(raw + KOLY_SEGMENT_NUMBER);
	trailer->segment_count = fsimg_load_be32(raw + KOLY_SEGMENT_COUNT);
	bcopy(raw + KOLY_SEGMENT_ID, trailer->segment_id,
	    sizeof(trailer->segment_id));
	checksum_decode(raw + KOLY_DATA_CHECKSUM, &trailer->data_checksum);
	checksum_decode(raw + KOLY_MASTER_CHECKSUM, &trailer->master_checksum);
	sectors = fsimg_load_be64(raw + KOLY_SECTOR_COUNT);

	if (!fsimg_add_u64(trailer->data_offset, trailer->data_bytes, &data_end) ||
	    data_end > trailer->xml_offset)
		return (dmg_corrupt(error, "koly data fork overlaps its plist"));
	if (trailer->segment_count > 1 ||
	    trailer->segment_number != trailer->segment_count)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "multi-segment DMG images are not supported"));
	if (!fsimg_mul_u64(sectors, DMG_SECTOR_BYTES, &trailer->disk_bytes))
		return (dmg_corrupt(error, "koly sector count overflows"));
	if ((trailer->data_checksum.type != CHECKSUM_NONE &&
	    trailer->data_checksum.type != CHECKSUM_CRC32) ||
	    (trailer->master_checksum.type != CHECKSUM_NONE &&
	    trailer->master_checksum.type != CHECKSUM_CRC32))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "DMG checksum type is not supported"));
	if ((trailer->data_checksum.type == CHECKSUM_CRC32 &&
	    trailer->data_checksum.bits != 32) ||
	    (trailer->master_checksum.type == CHECKSUM_CRC32 &&
	    trailer->master_checksum.bits != 32))
		return (dmg_corrupt(error, "koly CRC32 is not 32 bits"));
	return (FSIMG_OK);
}

static fsimg_status_t
read_trailer(const fsimg_container_input_t *input, dmg_trailer_t *trailer,
    fsimg_error_t *error)
{
	unsigned char raw[KOLY_HEADER_BYTES];
	size_t transferred;
	fsimg_status_t status;

	if (input->file_bytes < KOLY_HEADER_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no DMG trailer"));
	status = fsimg_io_read_session(input->file, input->session,
	    input->file_bytes - KOLY_HEADER_BYTES, raw, sizeof(raw),
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	return (trailer_decode(raw, input->file_bytes, trailer, error));
}

/* A table's header is complete: decode it and check that it lies in the
 * disk.
 */
static fsimg_status_t
table_header(dmg_walk_t *walk, fsimg_error_t *error)
{
	dmg_table_t *table = &walk->table;
	uint64_t end;

	if (bcmp(walk->header + MISH_SIGNATURE, "mish", 4) != 0 ||
	    fsimg_load_be32(walk->header + MISH_VERSION_FIELD) != MISH_VERSION)
		return (dmg_corrupt(error, "block table has no mish version 1 header"));
	table->first_sector = fsimg_load_be64(walk->header + MISH_FIRST_SECTOR);
	table->sector_count = fsimg_load_be64(walk->header + MISH_SECTOR_COUNT);
	table->data_offset = fsimg_load_be64(walk->header + MISH_DATA_OFFSET);
	table->run_count = fsimg_load_be32(walk->header + MISH_RUN_COUNT);
	checksum_decode(walk->header + MISH_CHECKSUM, &table->checksum);
	if (!fsimg_add_u64(table->first_sector, table->sector_count, &end) ||
	    end > walk->trailer->disk_bytes / DMG_SECTOR_BYTES)
		return (dmg_corrupt(error, "block table lies outside the disk"));
	walk->table_sectors = 0;
	walk->runs_seen = 0;
	walk->terminated = false;
	return (walk->visitor->table(walk->visitor->argument, table, error));
}

/*
 * A run is complete: decode it and check what every reader of the table
 * relies on. Runs end with one terminator; a comment or the terminator
 * covers no sectors and every other run covers at least one, within its
 * table.
 */
static fsimg_status_t
table_run(dmg_walk_t *walk, fsimg_error_t *error)
{
	dmg_run_t run;
	uint64_t end;

	run.type = fsimg_load_be32(walk->run + RUN_TYPE);
	run.sector = fsimg_load_be64(walk->run + RUN_SECTOR);
	run.sectors = fsimg_load_be64(walk->run + RUN_SECTORS);
	run.compressed_offset = fsimg_load_be64(walk->run + RUN_COMPRESSED_OFFSET);
	run.compressed_length = fsimg_load_be64(walk->run + RUN_COMPRESSED_LENGTH);
	if (walk->runs_seen == walk->table.run_count)
		return (dmg_corrupt(error, "block table has more runs than it declares"));
	if (walk->terminated)
		return (dmg_corrupt(error, "block table has runs after its terminator"));
	walk->runs_seen++;
	if (run.type == RUN_TERMINATOR || run.type == RUN_COMMENT) {
		if (run.sectors != 0)
			return (dmg_corrupt(error,
			    "comment or terminator run covers sectors"));
		walk->terminated = run.type == RUN_TERMINATOR;
		return (FSIMG_OK);
	}
	if (run.sectors == 0)
		return (dmg_corrupt(error, "run covers zero sectors"));
	if (!fsimg_add_u64(run.sector, run.sectors, &end) ||
	    end > walk->table.sector_count)
		return (dmg_corrupt(error, "run lies outside its block table"));
	if (!fsimg_add_u64(walk->table_sectors, run.sectors, &walk->table_sectors))
		return (dmg_corrupt(error,
		    "block table runs do not add up to its sectors"));
	return (walk->visitor->run(walk->visitor->argument, &walk->table, &run,
	    error));
}

/* A table's data ended: its runs must all be present, terminated and
 * cover the table's sectors exactly.
 */
static fsimg_status_t
table_finish(dmg_walk_t *walk, fsimg_error_t *error)
{
	if (walk->header_length != MISH_HEADER_BYTES || walk->run_length != 0 ||
	    walk->runs_seen != walk->table.run_count)
		return (dmg_corrupt(error, "block table is truncated"));
	if (!walk->terminated)
		return (dmg_corrupt(error, "block table has no terminator run"));
	if (walk->table_sectors != walk->table.sector_count)
		return (dmg_corrupt(error,
		    "block table runs do not add up to its sectors"));
	return (walk->visitor->table_end(walk->visitor->argument, &walk->table,
	    error));
}

/*
 * An element opens. Only a child of the path element matters: a key is
 * noted, and a value either continues the path (resource-fork, then blkx,
 * then an entry's Data) or is passed over.
 */
static fsimg_status_t
walk_open(void *argument, fsimg_plist_element_t kind, fsimg_error_t *error)
{
	dmg_walk_t *walk = argument;
	walk_place_t next;

	walk->depth++;
	if (walk->depth != walk->place + 1)
		return (FSIMG_OK);
	if (kind == FSIMG_PLIST_KEY) {
		walk->in_key = true;
		walk->key = KEY_OTHER;
		return (FSIMG_OK);
	}

	next = walk->place;
	switch (walk->place) {
	case PLACE_DOCUMENT:
		if (kind != FSIMG_PLIST_DICT)
			return (dmg_corrupt(error, "plist root is not a dict"));
		next = PLACE_ROOT;
		break;
	case PLACE_ROOT:
		if (walk->key == KEY_RESOURCE_FORK && kind == FSIMG_PLIST_DICT)
			next = PLACE_RESOURCES;
		break;
	case PLACE_RESOURCES:
		if (walk->key == KEY_BLKX && kind == FSIMG_PLIST_ARRAY)
			next = PLACE_BLKX;
		break;
	case PLACE_BLKX:
		if (kind != FSIMG_PLIST_DICT)
			return (dmg_corrupt(error, "blkx entry is not a dict"));
		next = PLACE_ENTRY;
		break;
	case PLACE_ENTRY:
		if (walk->key == KEY_DATA) {
			if (kind != FSIMG_PLIST_DATA)
				return (dmg_corrupt(error, "blkx Data is not data"));
			walk->header_length = 0;
			walk->run_length = 0;
			next = PLACE_TABLE;
		}
		break;
	default:
		break;
	}
	walk->key = KEY_OTHER;
	walk->place = next;
	return (FSIMG_OK);
}

/* A key on the path names the value that follows it. */
static fsimg_status_t
walk_text(void *argument, const char *text, size_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	static const struct {
		walk_place_t place;
		const char *name;
		walk_key_t key;
	} keys[] = {
		{ PLACE_ROOT, "resource-fork", KEY_RESOURCE_FORK },
		{ PLACE_RESOURCES, "blkx", KEY_BLKX },
		{ PLACE_ENTRY, "Data", KEY_DATA }
	};
	dmg_walk_t *walk = argument;

	if (!walk->in_key)
		return (FSIMG_OK);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (walk->place == keys[i].place &&
		    length == strlen(keys[i].name) &&
		    bcmp(text, keys[i].name, length) == 0)
			walk->key = keys[i].key;
	}
	return (FSIMG_OK);
}

/* Base64-decoded bytes of a table: its header, then whole runs. */
static fsimg_status_t
walk_data(void *argument, const unsigned char *bytes, size_t length,
    fsimg_error_t *error)
{
	dmg_walk_t *walk = argument;
	fsimg_status_t status;

	if (walk->place != PLACE_TABLE || walk->depth != PLACE_TABLE)
		return (FSIMG_OK);
	for (size_t i = 0; i < length; i++) {
		if (walk->header_length < MISH_HEADER_BYTES) {
			walk->header[walk->header_length++] = bytes[i];
			if (walk->header_length < MISH_HEADER_BYTES)
				continue;
			status = table_header(walk, error);
		} else {
			walk->run[walk->run_length++] = bytes[i];
			if (walk->run_length < MISH_RUN_BYTES)
				continue;
			walk->run_length = 0;
			status = table_run(walk, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* An element closes: a key of the path element ends, or the path steps
 * back out of the element it entered, finishing a table's data.
 */
static fsimg_status_t
walk_close(void *argument, fsimg_plist_element_t kind, fsimg_error_t *error)
{
	dmg_walk_t *walk = argument;
	unsigned depth;

	depth = walk->depth--;
	if (kind == FSIMG_PLIST_KEY && depth == walk->place + 1) {
		walk->in_key = false;
		return (FSIMG_OK);
	}
	if (depth != walk->place || walk->place == PLACE_DOCUMENT)
		return (FSIMG_OK);
	walk->place--;
	if (depth == PLACE_TABLE)
		return (table_finish(walk, error));
	return (FSIMG_OK);
}

/* Parse the plist and hand every block table to the visitor. */
static fsimg_status_t
walk_tables(const fsimg_container_input_t *input,
    const dmg_trailer_t *trailer, const dmg_table_visitor_t *visitor,
    fsimg_error_t *error)
{
	fsimg_plist_visitor_t plist;
	dmg_walk_t walk;

	bzero(&walk, sizeof(walk));
	walk.trailer = trailer;
	walk.visitor = visitor;
	walk.place = PLACE_DOCUMENT;
	plist.open = walk_open;
	plist.text = walk_text;
	plist.data = walk_data;
	plist.close = walk_close;
	plist.argument = &walk;
	return (fsimg_plist_parse(input->ctx, input->file, input->session,
	    trailer->xml_offset, trailer->xml_bytes, &plist, error));
}

/* The name of a chunk codec fsimg recognizes but does not decode. */
static const char *
unsupported_codec(uint32_t type)
{
	switch (type) {
	case RUN_ADC: return ("ADC");
	case RUN_BZIP2: return ("bzip2");
	case RUN_LZFSE: return ("LZFSE");
	case RUN_LZMA: return ("LZMA");
	default: return (NULL);
	}
}

static fsimg_status_t
open_table(void *argument FSIMG_UNUSED, const dmg_table_t *table FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/*
 * Turn a run into a chunk record. Its bytes, if any, lie in the data fork
 * at the table's data offset plus the run's: a raw run's as many as its
 * sectors, a zlib run's at least one and at most zlib's bound for its
 * decoded length, which may not exceed the 16 MiB a cache slot holds.
 * Zero and ignored runs read nothing. Any other codec is unsupported,
 * never read as zeros.
 */
static fsimg_status_t
open_run(void *argument, const dmg_table_t *table, const dmg_run_t *run,
    fsimg_error_t *error)
{
	dmg_open_t *open = argument;
	fsimg_chunk_t chunk;
	char message[sizeof(error->message)];
	uint64_t fork_offset, fork_end;

	bzero(&chunk, sizeof(chunk));
	chunk.disk_offset = (table->first_sector + run->sector) * DMG_SECTOR_BYTES;
	chunk.length = run->sectors * DMG_SECTOR_BYTES;
	switch (run->type) {
	case RUN_ZERO:
	case RUN_IGNORE:
		if (run->compressed_length != 0)
			return (dmg_corrupt(error, "zero run has compressed bytes"));
		chunk.kind = FSIMG_CHUNK_ZERO;
		return (fsimg_chunk_index_append(open->index, &chunk, error));
	case RUN_RAW:
		if (run->compressed_length != chunk.length)
			return (dmg_corrupt(error,
			    "raw run length differs from its sectors"));
		chunk.kind = FSIMG_CHUNK_RAW;
		break;
	case RUN_ZLIB:
		if (chunk.length > FSIMG_CHUNK_MAX_DECODED)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE,
			    "DMG zlib run decodes to more than 16 MiB"));
		if (run->compressed_length == 0 ||
		    run->compressed_length > compressBound((uLong)chunk.length))
			return (dmg_corrupt(error,
			    "zlib run length is outside zlib's bounds"));
		chunk.kind = FSIMG_CHUNK_COMPRESSED;
		chunk.argument = FSIMG_CODEC_ZLIB;
		break;
	default:
		if (unsupported_codec(run->type) != NULL)
			snprintf(message, sizeof(message), "DMG run uses %s "
			    "compression, which is not supported",
			    unsupported_codec(run->type));
		else
			snprintf(message, sizeof(message), "DMG run type 0x%08x "
			    "is not supported", (unsigned)run->type);
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, message));
	}

	/* The table's sectors are within the disk, whose byte length fits,
	 * so the disk range above cannot overflow; the fork range can.
	 */
	if (!fsimg_add_u64(table->data_offset, run->compressed_offset,
	    &fork_offset) ||
	    !fsimg_add_u64(fork_offset, run->compressed_length, &fork_end) ||
	    fork_end > open->trailer->data_bytes)
		return (dmg_corrupt(error, "run lies outside the data fork"));
	chunk.file_offset = open->trailer->data_offset + fork_offset;
	chunk.encoded_length = run->compressed_length;
	return (fsimg_chunk_index_append(open->index, &chunk, error));
}

static fsimg_status_t
open_table_end(void *argument FSIMG_UNUSED,
    const dmg_table_t *table FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* Validate the trailer and every block table, and yield the disk. */
static fsimg_status_t
dmg_open(const fsimg_container_input_t *input, fsimg_container_t *out,
    fsimg_error_t *error)
{
	dmg_table_visitor_t visitor;
	fsimg_chunk_summary_t summary;
	fsimg_chunk_index_t *index;
	dmg_trailer_t trailer;
	dmg_open_t open;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	status = read_trailer(input, &trailer, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_chunk_index_new(input->session, "DMG", "dmg",
	    trailer.disk_bytes, trailer.data_offset, trailer.data_bytes, &index,
	    error);
	if (status != FSIMG_OK)
		return (status);

	open.trailer = &trailer;
	open.index = index;
	visitor.table = open_table;
	visitor.run = open_run;
	visitor.table_end = open_table_end;
	visitor.argument = &open;
	status = walk_tables(input, &trailer, &visitor, error);
	if (status == FSIMG_OK)
		status = fsimg_chunk_index_seal(index, error);
	if (status != FSIMG_OK) {
		fsimg_chunk_index_free(index);
		return (status);
	}

	fsimg_chunk_index_summary(index, &summary);
	status = fsimg_chunk_disk_open(input->ctx, input->file, input->session,
	    index, &out->disk, error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(trailer.segment_id, out->guid, sizeof(out->guid));
	out->disk_bytes = trailer.disk_bytes;
	out->chunks = summary.count;
	out->kinds = summary.kinds;
	out->codecs = summary.codecs;
	return (FSIMG_OK);
}

static fsimg_status_t
verify_table(void *argument, const dmg_table_t *table,
    fsimg_error_t *error FSIMG_UNUSED)
{
	dmg_verify_t *verify = argument;
	unsigned char value[4];

	value[0] = (unsigned char)(table->checksum.value >> 24);
	value[1] = (unsigned char)(table->checksum.value >> 16);
	value[2] = (unsigned char)(table->checksum.value >> 8);
	value[3] = (unsigned char)table->checksum.value;
	verify->master = crc32(verify->master, value, sizeof(value));
	verify->table_crc = crc32(0, Z_NULL, 0);
	verify->tables++;
	return (FSIMG_OK);
}

/*
 * Under read-through, a run's decoded bytes join its table's CRC32. hdiutil
 * computes a table's checksum over every run but the ignored ones, which
 * contribute nothing: a table of free space alone has the empty CRC, zero.
 * A zero run joins arithmetically, so its declared length costs no reads.
 */
static fsimg_status_t
verify_run(void *argument, const dmg_table_t *table, const dmg_run_t *run,
    fsimg_error_t *error)
{
	dmg_verify_t *verify = argument;
	uint64_t offset, remaining;
	size_t piece, transferred;
	fsimg_status_t status;

	if (!verify->payload || run->type == RUN_IGNORE)
		return (FSIMG_OK);
	if (run->type == RUN_ZERO) {
		verify->table_crc = fsimg_crc32_zeros(verify->table_crc,
		    run->sectors * DMG_SECTOR_BYTES);
		return (FSIMG_OK);
	}
	offset = (table->first_sector + run->sector) * DMG_SECTOR_BYTES;
	remaining = run->sectors * DMG_SECTOR_BYTES;
	while (remaining != 0) {
		piece = remaining < VERIFY_PIECE_BYTES ? (size_t)remaining :
		    VERIFY_PIECE_BYTES;
		status = fsimg_io_read_session(verify->disk, verify->session,
		    offset, verify->piece, piece, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		verify->table_crc = crc32(verify->table_crc, verify->piece,
		    (uInt)piece);
		offset += piece;
		remaining -= piece;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
verify_table_end(void *argument, const dmg_table_t *table,
    fsimg_error_t *error)
{
	dmg_verify_t *verify = argument;
	char message[sizeof(error->message)];

	if (!verify->payload || table->checksum.type == CHECKSUM_NONE)
		return (FSIMG_OK);
	if (table->checksum.type != CHECKSUM_CRC32 || table->checksum.bits != 32)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "DMG block table checksum type is not supported"));
	if (verify->table_crc != table->checksum.value) {
		snprintf(message, sizeof(message), "block table %llu checksum "
		    "does not match its decoded bytes",
		    (unsigned long long)verify->tables);
		return (dmg_corrupt(error, message));
	}
	return (FSIMG_OK);
}

/* One sequential pass over the data fork for its CRC32. */
static fsimg_status_t
verify_data_fork(const fsimg_container_input_t *input,
    const dmg_trailer_t *trailer, unsigned char *buffer, fsimg_error_t *error)
{
	uint64_t offset, remaining;
	size_t piece, transferred;
	uLong crc;
	fsimg_status_t status;

	crc = crc32(0, Z_NULL, 0);
	offset = trailer->data_offset;
	remaining = trailer->data_bytes;
	while (remaining != 0) {
		piece = remaining < VERIFY_PIECE_BYTES ? (size_t)remaining :
		    VERIFY_PIECE_BYTES;
		status = fsimg_io_read_session(input->file, input->session,
		    offset, buffer, piece, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		crc = crc32(crc, buffer, (uInt)piece);
		offset += piece;
		remaining -= piece;
	}
	if (crc != trailer->data_checksum.value)
		return (dmg_corrupt(error, "data fork checksum does not match"));
	return (FSIMG_OK);
}

/*
 * The checks that read beyond what opening needs: the data fork's CRC32
 * where the trailer declares one, the master checksum over the tables'
 * checksum values where it declares one, and under read-through each
 * table's CRC32 over its decoded runs, every chunk decoded once.
 */
static fsimg_status_t
dmg_verify(const fsimg_container_input_t *input,
    const fsimg_container_t *container, bool payload, fsimg_error_t *error)
{
	dmg_table_visitor_t visitor;
	dmg_trailer_t trailer;
	dmg_verify_t verify;
	fsimg_status_t status;

	status = read_trailer(input, &trailer, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&verify, sizeof(verify));
	verify.disk = container->disk;
	verify.session = input->session;
	verify.payload = payload;
	verify.master = crc32(0, Z_NULL, 0);
	verify.piece = fsimg_alloc(input->ctx, VERIFY_PIECE_BYTES, error);
	if (verify.piece == NULL)
		return (error->status);

	status = FSIMG_OK;
	if (trailer.data_checksum.type == CHECKSUM_CRC32)
		status = verify_data_fork(input, &trailer, verify.piece, error);
	if (status == FSIMG_OK) {
		visitor.table = verify_table;
		visitor.run = verify_run;
		visitor.table_end = verify_table_end;
		visitor.argument = &verify;
		status = walk_tables(input, &trailer, &visitor, error);
	}
	fsimg_dealloc(verify.piece);
	if (status != FSIMG_OK)
		return (status);
	if (trailer.master_checksum.type == CHECKSUM_CRC32 &&
	    verify.master != trailer.master_checksum.value)
		return (dmg_corrupt(error, "master checksum does not match"));
	return (FSIMG_OK);
}

/*
 * The write side: one block table covering the whole disk, one run per
 * dmg:chunk of the disk, the last shorter. A data chunk is deflated at
 * dmg:level and stored as a zlib run when that makes it smaller, and raw
 * otherwise or under dmg:compress=none; consecutive zero chunks merge into
 * one zero-fill run. Runs are recorded in a metadata scratch store as they
 * are decided, since the plist that carries them follows the data fork,
 * and are replayed into its base64 text from there. The data fork's CRC32,
 * the table's CRC32 over the disk's bytes and the master CRC32 over the
 * table's go into the table header and the trailer.
 */
static fsimg_status_t
dmg_plan(const fsimg_container_plan_args_t *args, fsimg_container_plan_t **out,
    fsimg_error_t *error)
{
	const fsimg_layer_options_t *layers = args->layers;
	uint32_t chunk, level;
	dmg_plan_t *plan;

	level = layers->dmg_level;
	switch (layers->dmg_compress) {
	case FSIMG_DMG_ZLIB:
		if (level == 0)
			level = DMG_DEFAULT_LEVEL;
		if (level > 9)
			return (fsimg_setting_error(error, "dmg:level",
			    "the zlib level must be from 1 to 9"));
		break;
	case FSIMG_DMG_NONE:
		if (level != 0)
			return (fsimg_setting_error(error, "dmg:level",
			    "a level applies only to zlib compression"));
		break;
	default:
		return (fsimg_setting_error(error, "dmg:compress",
		    "the compression must be zlib or none"));
	}
	chunk = layers->dmg_chunk_bytes == 0 ? FSIMG_TRANSCODE_CHUNK_BYTES :
	    layers->dmg_chunk_bytes;
	if (chunk % DMG_SECTOR_BYTES != 0 || chunk > FSIMG_CHUNK_MAX_DECODED)
		return (fsimg_setting_error(error, "dmg:chunk",
		    "the chunk must be a multiple of 512 bytes up to 16 MiB"));
	plan = fsimg_container_plan_alloc(args->ctx, sizeof(*plan), out, error);
	if (plan == NULL)
		return (error->status);
	plan->base.granularity = DMG_SECTOR_BYTES;
	plan->base.chunk_bytes = chunk;
	plan->base.zlib = level != 0;
	plan->level = level;
	return (fsimg_container_identifier(layers->dmg_uuid, "dmg:uuid",
	    FSIMG_ROLE_DMG, args->flags, args->epoch, plan->base.identifier,
	    error));
}

/* The plist's length around a block table of table_bytes: the template,
 * and the table in base64, in lines of DMG_BASE64_LINE characters each
 * indented by four tabs.
 */
static uint64_t
plist_bytes(uint64_t table_bytes)
{
	uint64_t text, lines;

	text = (table_bytes + 2) / 3 * 4;
	lines = (text + DMG_BASE64_LINE - 1) / DMG_BASE64_LINE;
	return (strlen(plist_head) + strlen(plist_tail) + text +
	    lines * (DMG_BASE64_INDENT + 1));
}

/*
 * The most a disk's encoding can take: every chunk stored raw, which is
 * never shorter than a zlib run because a chunk deflate cannot shrink is
 * stored raw; every chunk its own run, plus the terminator; the plist; and
 * the trailer. A disk whose runs a table's 32-bit run count cannot hold is
 * refused, naming dmg:chunk, which sets the run count.
 */
static fsimg_status_t
dmg_bound(const fsimg_container_plan_t *plan, uint64_t disk_bytes,
    uint64_t *out, fsimg_error_t *error)
{
	uint64_t runs;

	runs = disk_bytes / plan->chunk_bytes + 2;
	if (runs > UINT32_MAX) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
		    "the disk needs more runs than a DMG block table holds");
		fsimg_error_path(error, "dmg:chunk");
		return (FSIMG_EUNSUPPORTED);
	}
	if (!fsimg_add_u64(disk_bytes, plist_bytes(MISH_HEADER_BYTES +
	    runs * MISH_RUN_BYTES) + KOLY_HEADER_BYTES, out))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "the disk is too large for a DMG"));
	return (FSIMG_OK);
}

/*
 * A zlib result other than compression progress: an allocation failure as
 * the adapter recorded it, or a fault of fsimg or of the zlib library in
 * the named call.
 */
static fsimg_status_t
dmg_zlib_failed(const fsimg_zlib_memory_t *memory, int result,
    const char *call, fsimg_error_t *error)
{
	char message[64];

	if (result == Z_MEM_ERROR)
		return (fsimg_zlib_memory_error(memory, error));
	if (result == Z_VERSION_ERROR)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the zlib library does not match its header"));
	snprintf(message, sizeof(message), "zlib %s returned %d", call, result);
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    message));
}

/* The encoder: its run store, and under zlib the deflate state and an
 * output buffer of one chunk, all charged to the context.
 */
static fsimg_status_t
dmg_encode_open(const fsimg_encode_t *encode, void **state,
    fsimg_error_t *error)
{
	dmg_encoder_t *encoder;
	fsimg_status_t status;
	int result;

	*state = NULL;
	encoder = fsimg_alloc(encode->ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	bzero(encoder, sizeof(*encoder));
	encoder->ctx = encode->ctx;
	encoder->sink = encode->sink;
	encoder->plan = (const dmg_plan_t *)encode->plan;
	encoder->disk_bytes = encode->disk_bytes;
	encoder->table_crc = crc32(0, Z_NULL, 0);
	encoder->fork_crc = crc32(0, Z_NULL, 0);
	*state = encoder;
	status = fsimg_store_new(encode->session, &encoder->runs, error);
	if (status != FSIMG_OK || encoder->plan->level == 0)
		return (status);
	encoder->packed = fsimg_alloc(encode->ctx,
	    encoder->plan->base.chunk_bytes, error);
	if (encoder->packed == NULL)
		return (error->status);
	fsimg_zlib_memory_bind(&encoder->zlib_memory, encode->ctx,
	    &encoder->stream);
	result = deflateInit(&encoder->stream, (int)encoder->plan->level);
	if (result != Z_OK)
		return (dmg_zlib_failed(&encoder->zlib_memory, result,
		    "deflateInit", error));
	encoder->stream_ready = true;
	return (FSIMG_OK);
}

/* Record one run, its sector range from the encoder's next sector. */
static fsimg_status_t
record_run(dmg_encoder_t *encoder, uint32_t type, uint64_t sectors,
    uint64_t offset, uint64_t length, fsimg_error_t *error)
{
	unsigned char run[MISH_RUN_BYTES];
	uint64_t slot;
	fsimg_status_t status;

	/* The store grows a replay piece at a time. */
	slot = encoder->run_count * MISH_RUN_BYTES;
	if (slot + MISH_RUN_BYTES > fsimg_store_size(encoder->runs)) {
		status = fsimg_store_grow(encoder->runs, slot + DMG_REPLAY_BYTES,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	bzero(run, sizeof(run));
	fsimg_store_be32(run + RUN_TYPE, type);
	fsimg_store_be64(run + RUN_SECTOR, encoder->sector);
	fsimg_store_be64(run + RUN_SECTORS, sectors);
	fsimg_store_be64(run + RUN_COMPRESSED_OFFSET, offset);
	fsimg_store_be64(run + RUN_COMPRESSED_LENGTH, length);
	status = fsimg_store_write(encoder->runs, slot, run, sizeof(run),
	    error);
	if (status != FSIMG_OK)
		return (status);
	encoder->run_count++;
	encoder->sector += sectors;
	return (FSIMG_OK);
}

/* Record the pending zero run, which names no data fork bytes, and fold
 * its zeros into the image CRC at once. Its offset is the data fork's end
 * so far, as hdiutil writes it, so the run is recorded before the next
 * data chunk extends the fork.
 */
static fsimg_status_t
record_zero_run(dmg_encoder_t *encoder, fsimg_error_t *error)
{
	uint64_t sectors = encoder->zero_sectors;

	if (sectors == 0)
		return (FSIMG_OK);
	encoder->zero_sectors = 0;
	encoder->table_crc = fsimg_crc32_zeros(encoder->table_crc,
	    sectors * DMG_SECTOR_BYTES);
	return (record_run(encoder, RUN_ZERO, sectors,
	    fsimg_seq_written(encoder->sink), 0, error));
}

/*
 * Deflate one chunk into the packed buffer, which holds no more than the
 * chunk itself: *packed_length is the zlib stream's length when it is
 * shorter than the chunk, and zero when the chunk does not shrink. Only a
 * complete stream that fits, or output space running out, is a choice
 * between the two; any other zlib result fails the encode.
 */
static fsimg_status_t
deflate_chunk(dmg_encoder_t *encoder, const unsigned char *bytes,
    size_t length, size_t *packed_length, fsimg_error_t *error)
{
	z_stream *stream = &encoder->stream;
	int result;

	*packed_length = 0;
	result = deflateReset(stream);
	if (result != Z_OK)
		return (dmg_zlib_failed(&encoder->zlib_memory, result,
		    "deflateReset", error));
	stream->next_in = (Bytef *)(uintptr_t)bytes;
	stream->avail_in = (uInt)length;
	stream->next_out = encoder->packed;
	stream->avail_out = (uInt)length;
	result = deflate(stream, Z_FINISH);
	if (result == Z_STREAM_END && stream->total_out < length)
		*packed_length = (size_t)stream->total_out;
	else if (result != Z_OK && result != Z_STREAM_END &&
	    result != Z_BUF_ERROR)
		return (dmg_zlib_failed(&encoder->zlib_memory, result,
		    "deflate", error));
	return (FSIMG_OK);
}

static fsimg_status_t
dmg_encode_chunk(void *state, unsigned pass FSIMG_UNUSED,
    const unsigned char *bytes, size_t length, fsimg_error_t *error)
{
	dmg_encoder_t *encoder = state;
	const unsigned char *stored;
	uint64_t offset;
	size_t packed_length;
	uint32_t type;
	fsimg_status_t status;

	if (bytes == NULL) {
		encoder->zero_sectors += length / DMG_SECTOR_BYTES;
		return (FSIMG_OK);
	}
	status = record_zero_run(encoder, error);
	if (status != FSIMG_OK)
		return (status);
	encoder->table_crc = crc32(encoder->table_crc, bytes, (uInt)length);
	packed_length = 0;
	if (encoder->plan->level != 0) {
		status = deflate_chunk(encoder, bytes, length, &packed_length,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	type = packed_length != 0 ? RUN_ZLIB : RUN_RAW;
	stored = packed_length != 0 ? encoder->packed : bytes;
	if (packed_length == 0)
		packed_length = length;
	offset = fsimg_seq_written(encoder->sink);
	status = fsimg_seq_write(encoder->sink, stored, packed_length, error);
	if (status != FSIMG_OK)
		return (status);
	encoder->fork_crc = crc32(encoder->fork_crc, stored,
	    (uInt)packed_length);
	return (record_run(encoder, type, length / DMG_SECTOR_BYTES, offset,
	    packed_length, error));
}

/* Write one line of base64 text, indented, once it is full or the text
 * ends.
 */
static fsimg_status_t
base64_line(dmg_encoder_t *encoder, fsimg_error_t *error)
{
	char line[DMG_BASE64_INDENT + DMG_BASE64_LINE + 1];
	size_t length;

	if (encoder->column == 0)
		return (FSIMG_OK);
	memset(line, '\t', DMG_BASE64_INDENT);
	bcopy(encoder->line, line + DMG_BASE64_INDENT, encoder->column);
	length = DMG_BASE64_INDENT + encoder->column;
	line[length++] = '\n';
	encoder->column = 0;
	return (fsimg_seq_write(encoder->sink, line, length, error));
}

/* Encode the carried group of one to three bytes, padded with '='. */
static fsimg_status_t
base64_group(dmg_encoder_t *encoder, fsimg_error_t *error)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char *carry = encoder->carry;
	uint32_t group;
	char text[4];

	group = (uint32_t)carry[0] << 16 | (uint32_t)carry[1] << 8 | carry[2];
	for (unsigned i = 0; i < 4; i++)
		text[i] = i <= encoder->carried ?
		    alphabet[(group >> (18 - 6 * i)) & 0x3f] : '=';
	bzero(encoder->carry, sizeof(encoder->carry));
	encoder->carried = 0;
	bcopy(text, encoder->line + encoder->column, sizeof(text));
	encoder->column += sizeof(text);
	if (encoder->column < DMG_BASE64_LINE)
		return (FSIMG_OK);
	return (base64_line(encoder, error));
}

/* Append bytes to the table's base64 text. */
static fsimg_status_t
base64_put(dmg_encoder_t *encoder, const unsigned char *bytes, size_t length,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = FSIMG_OK;
	for (size_t i = 0; status == FSIMG_OK && i < length; i++) {
		encoder->carry[encoder->carried++] = bytes[i];
		if (encoder->carried == sizeof(encoder->carry))
			status = base64_group(encoder, error);
	}
	return (status);
}

/* End the base64 text: the partial group padded, the partial line. */
static fsimg_status_t
base64_end(dmg_encoder_t *encoder, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = FSIMG_OK;
	if (encoder->carried != 0)
		status = base64_group(encoder, error);
	if (status == FSIMG_OK)
		status = base64_line(encoder, error);
	return (status);
}

/*
 * The block table in base64: its header, the recorded runs replayed from
 * the store in pieces, and the terminator, which ends the disk and names
 * the data fork's end.
 */
static fsimg_status_t
write_table(dmg_encoder_t *encoder, uint64_t fork_bytes, fsimg_error_t *error)
{
	unsigned char header[MISH_HEADER_BYTES], run[MISH_RUN_BYTES];
	unsigned char *piece;
	uint64_t sectors, total, done;
	size_t step;
	fsimg_status_t status;

	sectors = encoder->disk_bytes / DMG_SECTOR_BYTES;
	bzero(header, sizeof(header));
	bcopy("mish", header + MISH_SIGNATURE, 4);
	fsimg_store_be32(header + MISH_VERSION_FIELD, MISH_VERSION);
	fsimg_store_be64(header + MISH_SECTOR_COUNT, sectors);
	fsimg_store_be32(header + MISH_BUFFERS,
	    encoder->plan->base.chunk_bytes / DMG_SECTOR_BYTES + 8);
	fsimg_store_be32(header + MISH_DESCRIPTOR, DMG_WHOLE_DISK);
	fsimg_store_be32(header + MISH_CHECKSUM, CHECKSUM_CRC32);
	fsimg_store_be32(header + MISH_CHECKSUM + 4, 32);
	fsimg_store_be32(header + MISH_CHECKSUM + 8,
	    (uint32_t)encoder->table_crc);
	fsimg_store_be32(header + MISH_RUN_COUNT,
	    (uint32_t)(encoder->run_count + 1));
	status = base64_put(encoder, header, sizeof(header), error);
	if (status != FSIMG_OK)
		return (status);

	piece = fsimg_alloc(encoder->ctx, DMG_REPLAY_BYTES, error);
	if (piece == NULL)
		return (error->status);
	total = encoder->run_count * MISH_RUN_BYTES;
	for (done = 0; status == FSIMG_OK && done < total; done += step) {
		step = total - done > DMG_REPLAY_BYTES ? DMG_REPLAY_BYTES :
		    (size_t)(total - done);
		status = fsimg_store_read(encoder->runs, done, piece, step,
		    error);
		if (status == FSIMG_OK)
			status = base64_put(encoder, piece, step, error);
	}
	fsimg_dealloc(piece);
	if (status != FSIMG_OK)
		return (status);

	bzero(run, sizeof(run));
	fsimg_store_be32(run + RUN_TYPE, RUN_TERMINATOR);
	fsimg_store_be64(run + RUN_SECTOR, sectors);
	fsimg_store_be64(run + RUN_COMPRESSED_OFFSET, fork_bytes);
	status = base64_put(encoder, run, sizeof(run), error);
	if (status == FSIMG_OK)
		status = base64_end(encoder, error);
	return (status);
}

/*
 * The trailer of a single-segment image whose data fork starts the file:
 * flattened, its segment ID the dmg:uuid, CRC32 data and master
 * checksums, and variant 2. hdiutil writes variant 1 beside its
 * per-region tables, but refuses to attach a variant 1 image whose one
 * table spans a partitioned disk; it attaches variant 2 with or without a
 * partition map.
 */
static fsimg_status_t
write_trailer(dmg_encoder_t *encoder, uint64_t fork_bytes, uint64_t xml_bytes,
    fsimg_error_t *error)
{
	unsigned char koly[KOLY_HEADER_BYTES], value[4];
	uLong master;

	fsimg_store_be32(value, (uint32_t)encoder->table_crc);
	master = crc32(crc32(0, Z_NULL, 0), value, sizeof(value));
	bzero(koly, sizeof(koly));
	bcopy("koly", koly + KOLY_SIGNATURE, 4);
	fsimg_store_be32(koly + KOLY_VERSION_FIELD, KOLY_VERSION);
	fsimg_store_be32(koly + KOLY_HEADER_SIZE, KOLY_HEADER_BYTES);
	fsimg_store_be32(koly + KOLY_FLAGS, DMG_FLATTENED);
	fsimg_store_be64(koly + KOLY_DATA_LENGTH, fork_bytes);
	fsimg_store_be32(koly + KOLY_SEGMENT_NUMBER, 1);
	fsimg_store_be32(koly + KOLY_SEGMENT_COUNT, 1);
	bcopy(encoder->plan->base.identifier, koly + KOLY_SEGMENT_ID, 16);
	fsimg_store_be32(koly + KOLY_DATA_CHECKSUM, CHECKSUM_CRC32);
	fsimg_store_be32(koly + KOLY_DATA_CHECKSUM + 4, 32);
	fsimg_store_be32(koly + KOLY_DATA_CHECKSUM + 8,
	    (uint32_t)encoder->fork_crc);
	fsimg_store_be64(koly + KOLY_XML_OFFSET, fork_bytes);
	fsimg_store_be64(koly + KOLY_XML_LENGTH, xml_bytes);
	fsimg_store_be32(koly + KOLY_MASTER_CHECKSUM, CHECKSUM_CRC32);
	fsimg_store_be32(koly + KOLY_MASTER_CHECKSUM + 4, 32);
	fsimg_store_be32(koly + KOLY_MASTER_CHECKSUM + 8, (uint32_t)master);
	fsimg_store_be32(koly + KOLY_VARIANT, DMG_VARIANT);
	fsimg_store_be64(koly + KOLY_SECTOR_COUNT,
	    encoder->disk_bytes / DMG_SECTOR_BYTES);
	return (fsimg_seq_write(encoder->sink, koly, sizeof(koly), error));
}

/* After the data fork: the plist from its fixed template, then the
 * trailer.
 */
static fsimg_status_t
dmg_encode_finish(void *state, unsigned pass FSIMG_UNUSED,
    fsimg_error_t *error)
{
	dmg_encoder_t *encoder = state;
	uint64_t fork_bytes;
	fsimg_status_t status;

	status = record_zero_run(encoder, error);
	if (status != FSIMG_OK)
		return (status);
	fork_bytes = fsimg_seq_written(encoder->sink);
	status = fsimg_seq_write(encoder->sink, plist_head, strlen(plist_head),
	    error);
	if (status == FSIMG_OK)
		status = write_table(encoder, fork_bytes, error);
	if (status == FSIMG_OK)
		status = fsimg_seq_write(encoder->sink, plist_tail,
		    strlen(plist_tail), error);
	if (status == FSIMG_OK)
		status = write_trailer(encoder, fork_bytes,
		    fsimg_seq_written(encoder->sink) - fork_bytes, error);
	return (status);
}

static void
dmg_encode_close(void *state)
{
	dmg_encoder_t *encoder = state;

	if (encoder->stream_ready)
		deflateEnd(&encoder->stream);
	fsimg_store_free(encoder->runs);
	fsimg_dealloc(encoder->packed);
	fsimg_dealloc(encoder);
}
