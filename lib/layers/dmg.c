/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "io.h"
#include "layer.h"

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
 * corrupt rather than read as zeros.
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

/* Trailer fields. */
enum {
	KOLY_SIGNATURE = 0,
	KOLY_VERSION_FIELD = 4,
	KOLY_HEADER_SIZE = 8,
	KOLY_DATA_OFFSET = 24,
	KOLY_DATA_LENGTH = 32,
	KOLY_SEGMENT_NUMBER = 56,
	KOLY_SEGMENT_COUNT = 60,
	KOLY_SEGMENT_ID = 64,
	KOLY_DATA_CHECKSUM = 80,
	KOLY_XML_OFFSET = 216,
	KOLY_XML_LENGTH = 224,
	KOLY_MASTER_CHECKSUM = 352,
	KOLY_SECTOR_COUNT = 492
};

/* Block table fields. */
enum {
	MISH_SIGNATURE = 0,
	MISH_VERSION_FIELD = 4,
	MISH_FIRST_SECTOR = 8,
	MISH_SECTOR_COUNT = 16,
	MISH_DATA_OFFSET = 24,
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

const fsimg_container_ops_t fsimg_dmg_container = {
	.probe_bytes = 0,
	.probe = dmg_probe,
	.open = dmg_open,
	.verify = dmg_verify,
	.codecs = "raw,zero,zlib",
	.payload_checks = true,
	.zlib = true
};

static void
checksum_decode(const unsigned char *raw, dmg_checksum_t *checksum)
{
	checksum->type = fsimg_layer_be32(raw);
	checksum->bits = fsimg_layer_be32(raw + 4);
	checksum->value = fsimg_layer_be32(raw + 8);
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
	    fsimg_layer_be32(tail + KOLY_VERSION_FIELD) != KOLY_VERSION ||
	    fsimg_layer_be32(tail + KOLY_HEADER_SIZE) != KOLY_HEADER_BYTES ||
	    !fsimg_add_u64(fsimg_layer_be64(tail + KOLY_XML_OFFSET),
	    fsimg_layer_be64(tail + KOLY_XML_LENGTH), &xml_end) ||
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
	trailer->data_offset = fsimg_layer_be64(raw + KOLY_DATA_OFFSET);
	trailer->data_bytes = fsimg_layer_be64(raw + KOLY_DATA_LENGTH);
	trailer->xml_offset = fsimg_layer_be64(raw + KOLY_XML_OFFSET);
	trailer->xml_bytes = fsimg_layer_be64(raw + KOLY_XML_LENGTH);
	trailer->segment_number = fsimg_layer_be32(raw + KOLY_SEGMENT_NUMBER);
	trailer->segment_count = fsimg_layer_be32(raw + KOLY_SEGMENT_COUNT);
	bcopy(raw + KOLY_SEGMENT_ID, trailer->segment_id,
	    sizeof(trailer->segment_id));
	checksum_decode(raw + KOLY_DATA_CHECKSUM, &trailer->data_checksum);
	checksum_decode(raw + KOLY_MASTER_CHECKSUM, &trailer->master_checksum);
	sectors = fsimg_layer_be64(raw + KOLY_SECTOR_COUNT);

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
	    fsimg_layer_be32(walk->header + MISH_VERSION_FIELD) != MISH_VERSION)
		return (dmg_corrupt(error, "block table has no mish version 1 header"));
	table->first_sector = fsimg_layer_be64(walk->header + MISH_FIRST_SECTOR);
	table->sector_count = fsimg_layer_be64(walk->header + MISH_SECTOR_COUNT);
	table->data_offset = fsimg_layer_be64(walk->header + MISH_DATA_OFFSET);
	table->run_count = fsimg_layer_be32(walk->header + MISH_RUN_COUNT);
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

	run.type = fsimg_layer_be32(walk->run + RUN_TYPE);
	run.sector = fsimg_layer_be64(walk->run + RUN_SECTOR);
	run.sectors = fsimg_layer_be64(walk->run + RUN_SECTORS);
	run.compressed_offset = fsimg_layer_be64(walk->run + RUN_COMPRESSED_OFFSET);
	run.compressed_length = fsimg_layer_be64(walk->run + RUN_COMPRESSED_LENGTH);
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
	status = fsimg_chunk_index_new(input->session, "DMG", trailer.disk_bytes,
	    trailer.data_offset, trailer.data_bytes, &index, error);
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
