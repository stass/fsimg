/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <zlib.h>

#include "fsimg.h"
#include "host.h"
#include "io.h"
#include "layer.h"
#include "meta.h"
#include "test.h"

/*
 * The container read side: DMG, fixed VHD and Android sparse images, and
 * the chunk cache behind them. Images are built here by encoders
 * independent of the library's decoders: each writes its format from the
 * published field tables, with its own CRC-32, base64 and byte order; only
 * the zlib streams come from the system zlib, as hdiutil's do. Every
 * passing case has seeded failures beside it that must fail with the named
 * check, never fall through to a raw-disk read.
 */

#define KIB		UINT64_C(1024)
#define MIB		(UINT64_C(1024) * 1024)
#define SECTOR		512u

#define RUN_ZERO	0x00000000u
#define RUN_RAW		0x00000001u
#define RUN_IGNORE	0x00000002u
#define RUN_ZLIB	0x80000005u
#define RUN_BZIP2	0x80000006u
#define RUN_COMMENT	0x7ffffffeu
#define RUN_TERMINATOR	0xffffffffu

#define SIMG_RAW	0xcac1u
#define SIMG_FILL	0xcac2u
#define SIMG_DONT_CARE	0xcac3u
#define SIMG_CRC32	0xcac4u

/* The test disks hold no partition table or filesystem, so the scheme slot
 * is forced empty while the container slot is left to detection.
 */
#define DETECT		"fat12/-"

#define MAX_RUNS	2100
#define MAX_TABLES	4
#define MAX_SIMG_CHUNKS	8

/* A growable byte string. */
typedef struct buffer {
	unsigned char	*bytes;
	size_t		 length;
	size_t		 capacity;
} buffer_t;

/* An image file in memory, counting the bytes the library reads from it. */
typedef struct memory_file {
	unsigned char	*bytes;
	size_t		 length;
	uint64_t	 read_bytes;
	uint64_t	 reads;
	uint64_t	 fail_at;	/* The read that fails with EIO; 0 never */
	size_t		 fail_bytes;	/* Bytes confirmed by the failing read */
	uint64_t	 cut;		/* Nonzero: reads end here, as after a
					   truncation behind an open image */
} memory_file_t;

/* One DMG run; sector is relative to its table. offset_bias moves the
 * run's compressed offset away from the bytes the encoder wrote.
 */
typedef struct dmg_run {
	uint32_t	 type;
	uint64_t	 sector;
	uint64_t	 sectors;
	uint64_t	 offset_bias;
} dmg_run_t;

typedef struct dmg_table {
	uint64_t	 first;
	uint64_t	 count;
	unsigned	 runs;
	dmg_run_t	 run[MAX_RUNS];
} dmg_table_t;

/*
 * What the DMG encoder writes. The disk supplies the bytes of raw and zlib
 * runs and the table checksums. The remaining fields seed failures: extra
 * plist text in the root dict, zlib streams encoding zlib_delta bytes more
 * (or fewer) than their runs, a data fork declared fork_extra bytes longer
 * than written, and a first table checksum xored with table_crc_flip.
 */
typedef struct dmg_spec {
	const unsigned char *disk;
	uint64_t	 sectors;
	unsigned	 tables;
	dmg_table_t	 table[MAX_TABLES];
	uint32_t	 segment_number;
	uint32_t	 segment_count;
	const char	*extra;
	int		 zlib_delta;
	uint64_t	 fork_extra;
	uint32_t	 table_crc_flip;
} dmg_spec_t;

/* One sparse-image chunk. total overrides the chunk's total size when
 * nonzero; a CRC32 chunk's value is computed unless crc_flip spoils it.
 */
typedef struct simg_chunk {
	uint16_t	 type;
	uint32_t	 blocks;
	uint32_t	 value;
	uint32_t	 total;
	uint32_t	 crc_flip;
} simg_chunk_t;

typedef struct simg_spec {
	const unsigned char *disk;
	uint32_t	 block_bytes;
	unsigned	 chunks;
	simg_chunk_t	 chunk[MAX_SIMG_CHUNKS];
	uint16_t	 major;
	int		 total_blocks_delta;
	int		 total_chunks_delta;
} simg_spec_t;

/* A test's view of the library: one context with scratch and limits, and
 * one image over a memory file.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_image_t	*image;
	uint64_t	 baseline_memory;
	char		 directory[1024];
	int		 scratch;
	uint64_t	 work;		/* The most work any session reported */
	const char	*compression;	/* compose_disk's, NULL for none */
} fixture_t;

static const unsigned char segment_id[16] = { 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
static const unsigned char vhd_id[16] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf };

static uint32_t crc32_update(uint32_t, const unsigned char *, size_t);
static void put_be16(unsigned char *, uint16_t);
static void put_be32(unsigned char *, uint32_t);
static void put_be64(unsigned char *, uint64_t);
static void put_le16(unsigned char *, uint16_t);
static void put_le32(unsigned char *, uint32_t);
static void buffer_append(buffer_t *, const void *, size_t);
static void buffer_text(buffer_t *, const char *, ...);
static void buffer_base64(buffer_t *, const unsigned char *, size_t);
static unsigned char *disk_new(uint64_t);
static void disk_pattern(unsigned char *, uint64_t, uint32_t);
static memory_file_t file_adopt(buffer_t *);
static fsimg_status_t file_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t file_size(void *, uint64_t *, fsimg_error_t *);
static uint32_t dmg_run_bytes(const dmg_spec_t *, const dmg_table_t *,
    const dmg_run_t *, buffer_t *, uint32_t *);
static void dmg_table_encode(const dmg_spec_t *, unsigned, buffer_t *,
    buffer_t *, uint32_t *);
static memory_file_t dmg_encode(const dmg_spec_t *);
static dmg_spec_t dmg_whole_disk(const unsigned char *, uint64_t);
static void vhd_footer(unsigned char *, uint64_t, uint32_t);
static memory_file_t vhd_encode(const unsigned char *, uint64_t, uint64_t);
static memory_file_t simg_encode(const simg_spec_t *);
static void fixture_init(fixture_t *, uint64_t, uint64_t);
static void fixture_free(fixture_t *);
static fsimg_status_t fixture_open(fixture_t *, memory_file_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t read_exact(fsimg_io_t *, uint64_t, void *, size_t,
    fsimg_error_t *);
static fsimg_status_t read_disk(fixture_t *, uint64_t, void *, size_t,
    fsimg_error_t *);
static void expect_disk(fixture_t *, const unsigned char *, uint64_t);
static void expect_open_fails(memory_file_t, const char *, fsimg_status_t,
    const char *);
static void expect_dmg_fails(const dmg_spec_t *, fsimg_status_t, const char *);
static void expect_payload_fails(memory_file_t, bool, const char *);
static void test_crc_oracle(void);
static void test_dmg_read(void);
static void test_dmg_table_shapes(void);
static void test_dmg_seeded_failures(void);
static void test_dmg_plist_limits(void);
static void test_dmg_checksums(void);
static uint64_t verify_read_bytes(fixture_t *);
static void test_verify_declared_zeros(void);
static void test_stray_trailer(void);
static void test_vhd(void);
static simg_spec_t simg_sample(unsigned char *);
static void test_simg(void);
static void test_raw_chunk_partial_error(void);
static void test_simg_seeded_failures(void);
static memory_file_t five_chunk_dmg(uint64_t, unsigned char **);
static void test_cache_sequential(void);
static void test_cache_alternation(void);
static void test_cache_decode_bound(void);
static void test_cache_memory(void);
static void test_tampered_record(void);
static fsimg_status_t exercise_image(fixture_t *);
static void fault_sweep(memory_file_t);
static void test_fault_sweeps(void);
static memory_file_t three_run_dmg(uint32_t, unsigned char **);
static fsimg_status_t compose_disk(fixture_t *, const char *,
    unsigned char **, uint64_t *, fsimg_error_t *);
static fsimg_status_t compose_to_sink(fixture_t *, const fsimg_sink_t *,
    fsimg_error_t *);
static void progress_work(void *, const fsimg_progress_t *);
static void test_container_zero_runs(void);
static void test_container_index_faults(void);
static void test_vast_declared_zeros(void);
static memory_file_t tail_dmg(uint32_t, unsigned char **);
static void test_truncated_container(void);
static void test_many_zero_records(void);
static void write_seed(const char *, memory_file_t);

/* CRC-32 (ISO-HDLC) continued from crc, which starts at zero; a byte
 * table built on first use keeps the 80 MiB cache fixtures quick.
 */
static uint32_t
crc32_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
	static uint32_t table[256];
	uint32_t entry;

	if (table[1] == 0) {
		for (uint32_t byte = 0; byte < 256; byte++) {
			entry = byte;
			for (int bit = 0; bit < 8; bit++)
				entry = (entry >> 1) ^
				    (0xedb88320u & (0u - (entry & 1u)));
			table[byte] = entry;
		}
	}
	crc ^= 0xffffffffu;
	for (size_t i = 0; i < length; i++)
		crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xff];
	return (crc ^ 0xffffffffu);
}

static void
put_be16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void
put_be32(unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (24 - 8 * i));
}

static void
put_be64(unsigned char *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (56 - 8 * i));
}

static void
put_le16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put_le32(unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static void
buffer_append(buffer_t *buffer, const void *bytes, size_t length)
{
	if (buffer->capacity - buffer->length < length) {
		while (buffer->capacity - buffer->length < length)
			buffer->capacity = buffer->capacity == 0 ? 4096 :
			    buffer->capacity * 2;
		buffer->bytes = realloc(buffer->bytes, buffer->capacity);
		CHECK(buffer->bytes != NULL);
	}
	if (length != 0)
		bcopy(bytes, buffer->bytes + buffer->length, length);
	buffer->length += length;
}

static void
buffer_text(buffer_t *buffer, const char *format, ...)
{
	char text[8192];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	CHECK(length >= 0 && (size_t)length < sizeof(text));
	buffer_append(buffer, text, (size_t)length);
}

/* Base64 as hdiutil lays it out: 52-character lines, each indented. */
static void
buffer_base64(buffer_t *buffer, const unsigned char *bytes, size_t length)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char quad[4];
	uint32_t group;
	size_t column = 0;

	for (size_t i = 0; i < length; i += 3) {
		group = (uint32_t)bytes[i] << 16;
		if (i + 1 < length)
			group |= (uint32_t)bytes[i + 1] << 8;
		if (i + 2 < length)
			group |= bytes[i + 2];
		quad[0] = alphabet[group >> 18 & 63];
		quad[1] = alphabet[group >> 12 & 63];
		quad[2] = i + 1 < length ? alphabet[group >> 6 & 63] : '=';
		quad[3] = i + 2 < length ? alphabet[group & 63] : '=';
		if (column == 0)
			buffer_text(buffer, "\t\t\t\t");
		buffer_append(buffer, quad, sizeof(quad));
		column += 4;
		if (column == 52) {
			buffer_text(buffer, "\n");
			column = 0;
		}
	}
	if (column != 0)
		buffer_text(buffer, "\n");
}

static unsigned char *
disk_new(uint64_t length)
{
	unsigned char *disk = calloc(1, (size_t)length);

	CHECK(disk != NULL);
	return (disk);
}

/* Bytes that vary with their offset and the seed, compressible but not
 * trivially so.
 */
static void
disk_pattern(unsigned char *bytes, uint64_t length, uint32_t seed)
{
	uint32_t state = seed * 2654435761u + 1;

	for (uint64_t i = 0; i < length; i++) {
		if (i % 64 == 0)
			state = state * 1103515245u + 12345u;
		bytes[i] = (unsigned char)((state >> 16) + (i % 64 < 32 ? 0 : i));
	}
}

static memory_file_t
file_adopt(buffer_t *buffer)
{
	memory_file_t file = { .bytes = buffer->bytes, .length = buffer->length };

	bzero(buffer, sizeof(*buffer));
	return (file);
}

static fsimg_status_t
file_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_file_t *file = argument;

	CHECK(offset <= file->length && length <= file->length - offset);
	if (++file->reads == file->fail_at) {
		if (file->fail_bytes < length)
			length = file->fail_bytes;
		bcopy(file->bytes + offset, buffer, length);
		file->read_bytes += length;
		*transferred = length;
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected read failure"));
	}
	if (file->cut != 0 && offset + length > file->cut)
		length = offset < file->cut ? (size_t)(file->cut - offset) : 0;
	bcopy(file->bytes + offset, buffer, length);
	file->read_bytes += length;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
file_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((memory_file_t *)argument)->length;
	return (FSIMG_OK);
}

/*
 * Append a run's encoded bytes to the data fork and return their length;
 * crc collects the run's decoded bytes as hdiutil's table checksum does,
 * every run but ignored ones. A run the disk cannot back writes nothing.
 */
static uint32_t
dmg_run_bytes(const dmg_spec_t *spec, const dmg_table_t *table,
    const dmg_run_t *run, buffer_t *fork, uint32_t *crc)
{
	static const unsigned char other[16] = { 'B', 'Z', 'h', '9' };
	const unsigned char *bytes;
	unsigned char *source, *encoded;
	uint64_t first, length;
	uLongf encoded_length;
	size_t source_length;

	first = (table->first + run->sector) * SECTOR;
	length = run->sectors * SECTOR;
	/* Free space names no bytes and no checksum, so a vast one costs
	 * nothing here.
	 */
	if (run->type == RUN_COMMENT || run->type == RUN_IGNORE ||
	    first > spec->sectors * SECTOR ||
	    length > spec->sectors * SECTOR - first)
		return (0);
	bytes = spec->disk + first;
	*crc = crc32_update(*crc, bytes, (size_t)length);
	switch (run->type) {
	case RUN_ZERO:
		return (0);
	case RUN_RAW:
		buffer_append(fork, bytes, (size_t)length);
		return ((uint32_t)length);
	case RUN_ZLIB:
		/* The stream may encode more or fewer bytes than the run. */
		source_length = (size_t)((int64_t)length + spec->zlib_delta);
		source = malloc(source_length + 1);
		CHECK(source != NULL);
		memset(source, 0xaa, source_length + 1);
		bcopy(bytes, source, source_length < length ? source_length :
		    (size_t)length);
		encoded_length = compressBound((uLong)source_length);
		encoded = malloc(encoded_length);
		CHECK(encoded != NULL);
		CHECK(compress2(encoded, &encoded_length, source,
		    (uLong)source_length, 9) == Z_OK);
		buffer_append(fork, encoded, encoded_length);
		free(source);
		free(encoded);
		return ((uint32_t)encoded_length);
	default:
		buffer_append(fork, other, sizeof(other));
		return (sizeof(other));
	}
}

/* One block table: its runs' bytes join the data fork and its mish
 * structure goes to mish; *checksum receives the table's checksum value.
 */
static void
dmg_table_encode(const dmg_spec_t *spec, unsigned index, buffer_t *fork,
    buffer_t *mish, uint32_t *checksum)
{
	const dmg_table_t *table = &spec->table[index];
	const dmg_run_t terminator = { RUN_TERMINATOR, table->count, 0, 0 };
	const dmg_run_t *run;
	unsigned char header[204], entry[40];
	uint32_t crc = 0, encoded;
	uint64_t offset;

	bzero(header, sizeof(header));
	buffer_append(mish, header, sizeof(header));
	for (unsigned i = 0; i <= table->runs; i++) {
		run = i < table->runs ? &table->run[i] : &terminator;
		offset = fork->length;
		encoded = run->type == RUN_TERMINATOR ? 0 :
		    dmg_run_bytes(spec, table, run, fork, &crc);
		bzero(entry, sizeof(entry));
		put_be32(entry, run->type);
		put_be64(entry + 8, run->sector);
		put_be64(entry + 16, run->sectors);
		put_be64(entry + 24, offset + run->offset_bias);
		put_be64(entry + 32, encoded);
		buffer_append(mish, entry, sizeof(entry));
	}
	if (index == 0)
		crc ^= spec->table_crc_flip;
	bcopy("mish", header, 4);
	put_be32(header + 4, 1);
	put_be64(header + 8, table->first);
	put_be64(header + 16, table->count);
	put_be64(header + 24, 0);
	put_be32(header + 32, 0x208);
	put_be32(header + 36, index);
	put_be32(header + 64, 2);
	put_be32(header + 68, 32);
	put_be32(header + 72, crc);
	put_be32(header + 200, table->runs + 1);
	bcopy(header, mish->bytes, sizeof(header));
	*checksum = crc;
}

/*
 * A DMG as hdiutil lays it out: the data fork from offset 0, the XML plist
 * after it, and the koly trailer. The plist carries the blkx tables in the
 * order given, then a plst resource the reader must skip.
 */
static memory_file_t
dmg_encode(const dmg_spec_t *spec)
{
	buffer_t file = { 0 }, xml = { 0 }, mish = { 0 };
	unsigned char koly[512], value[4];
	uint32_t checksum, master = 0;
	uint64_t fork_length;

	buffer_text(&xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n<dict>\n");
	if (spec->extra != NULL)
		buffer_append(&xml, spec->extra, strlen(spec->extra));
	buffer_text(&xml, "\t<key>resource-fork</key>\n\t<dict>\n"
	    "\t\t<key>blkx</key>\n\t\t<array>\n");
	for (unsigned i = 0; i < spec->tables; i++) {
		mish.length = 0;
		dmg_table_encode(spec, i, &file, &mish, &checksum);
		put_be32(value, checksum);
		master = crc32_update(master, value, sizeof(value));
		buffer_text(&xml, "\t\t\t<dict>\n\t\t\t\t<key>Attributes</key>\n"
		    "\t\t\t\t<string>0x0050</string>\n\t\t\t\t<key>CFName</key>\n"
		    "\t\t\t\t<string>table %u &amp; &lt;more&gt;</string>\n"
		    "\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n", i);
		buffer_base64(&xml, mish.bytes, mish.length);
		buffer_text(&xml, "\t\t\t\t</data>\n\t\t\t\t<key>ID</key>\n"
		    "\t\t\t\t<string>%u</string>\n\t\t\t\t<key>Name</key>\n"
		    "\t\t\t\t<string></string>\n\t\t\t</dict>\n", i);
	}
	buffer_text(&xml, "\t\t</array>\n\t\t<key>plst</key>\n\t\t<array>\n"
	    "\t\t\t<dict>\n\t\t\t\t<key>Data</key>\n\t\t\t\t<data>AAAA</data>\n"
	    "\t\t\t</dict>\n\t\t</array>\n\t</dict>\n</dict>\n</plist>\n");

	fork_length = file.length;
	bzero(koly, sizeof(koly));
	bcopy("koly", koly, 4);
	put_be32(koly + 4, 4);
	put_be32(koly + 8, 512);
	put_be32(koly + 12, 1);
	put_be64(koly + 32, fork_length + spec->fork_extra);
	put_be32(koly + 56, spec->segment_number);
	put_be32(koly + 60, spec->segment_count);
	bcopy(segment_id, koly + 64, sizeof(segment_id));
	put_be32(koly + 80, 2);
	put_be32(koly + 84, 32);
	put_be32(koly + 88, crc32_update(0, file.bytes, (size_t)fork_length));
	put_be64(koly + 216, fork_length);
	put_be64(koly + 224, xml.length);
	put_be32(koly + 352, 2);
	put_be32(koly + 356, 32);
	put_be32(koly + 360, master);
	put_be32(koly + 488, 1);
	put_be64(koly + 492, spec->sectors);
	buffer_append(&file, xml.bytes, xml.length);
	buffer_append(&file, koly, sizeof(koly));
	free(xml.bytes);
	free(mish.bytes);
	return (file_adopt(&file));
}

/*
 * One table over a 1 MiB disk with every run kind the reader accepts:
 * raw bytes, zeros, a zlib run, free space, a comment and a second zlib
 * run. The zeroed ranges must hold zeros on the disk.
 */
static dmg_spec_t
dmg_whole_disk(const unsigned char *disk, uint64_t sectors)
{
	dmg_spec_t spec = { .disk = disk, .sectors = sectors, .tables = 1 };

	CHECK(sectors == 2048);
	spec.table[0] = (dmg_table_t){ 0, 2048, 6, {
		{ RUN_RAW, 0, 64, 0 },
		{ RUN_ZERO, 64, 64, 0 },
		{ RUN_ZLIB, 128, 1024, 0 },
		{ RUN_IGNORE, 1152, 128, 0 },
		{ RUN_COMMENT, 1280, 0, 0 },
		{ RUN_ZLIB, 1280, 768, 0 } } };
	return (spec);
}

/* A footer of the Microsoft layout for a disk of size bytes. */
static void
vhd_footer(unsigned char *footer, uint64_t size, uint32_t type)
{
	uint32_t sum = 0;

	bzero(footer, 512);
	bcopy("conectix", footer, 8);
	put_be32(footer + 8, 2);
	put_be32(footer + 12, 0x00010000);
	put_be64(footer + 16, type == 2 ? UINT64_MAX : 512);
	put_be32(footer + 24, 0x2a000000);
	bcopy("test", footer + 28, 4);
	put_be32(footer + 32, 0x00010000);
	bcopy("Wi2k", footer + 36, 4);
	put_be64(footer + 40, size);
	put_be64(footer + 48, size);
	put_be16(footer + 56, 120);
	footer[58] = 16;
	footer[59] = 63;
	put_be32(footer + 60, type);
	bcopy(vhd_id, footer + 68, sizeof(vhd_id));
	for (unsigned i = 0; i < 512; i++)
		sum += footer[i];
	put_be32(footer + 64, ~sum);
}

/* A fixed VHD: the disk, then a footer declaring current size bytes. */
static memory_file_t
vhd_encode(const unsigned char *disk, uint64_t length, uint64_t current)
{
	buffer_t file = { 0 };
	unsigned char footer[512];

	buffer_append(&file, disk, (size_t)length);
	vhd_footer(footer, current, 2);
	buffer_append(&file, footer, sizeof(footer));
	return (file_adopt(&file));
}

/* A sparse image as libsparse writes it, chunks in disk order. */
static memory_file_t
simg_encode(const simg_spec_t *spec)
{
	buffer_t file = { 0 };
	unsigned char header[28], chunk[16];
	uint64_t offset = 0, length;
	uint32_t blocks = 0, crc = 0, total;

	for (unsigned i = 0; i < spec->chunks; i++)
		blocks += spec->chunk[i].blocks;
	bzero(header, sizeof(header));
	put_le32(header, 0xed26ff3au);
	put_le16(header + 4, spec->major != 0 ? spec->major : 1);
	put_le16(header + 8, 28);
	put_le16(header + 10, 12);
	put_le32(header + 12, spec->block_bytes);
	put_le32(header + 16, (uint32_t)((int64_t)blocks + spec->total_blocks_delta));
	put_le32(header + 20, (uint32_t)((int64_t)spec->chunks +
	    spec->total_chunks_delta));
	buffer_append(&file, header, sizeof(header));
	for (unsigned i = 0; i < spec->chunks; i++) {
		const simg_chunk_t *c = &spec->chunk[i];
		uint32_t value = c->value;

		length = (uint64_t)c->blocks * spec->block_bytes;
		switch (c->type) {
		case SIMG_RAW:
			total = 12 + (uint32_t)length;
			break;
		case SIMG_DONT_CARE:
			total = 12;
			break;
		case SIMG_CRC32:
			value = crc ^ c->crc_flip;
			total = 16;
			break;
		default:
			total = 16;
			break;
		}
		bzero(chunk, sizeof(chunk));
		put_le16(chunk, c->type);
		put_le32(chunk + 4, c->blocks);
		put_le32(chunk + 8, c->total != 0 ? c->total : total);
		buffer_append(&file, chunk, 12);
		if (c->type == SIMG_RAW)
			buffer_append(&file, spec->disk + offset, (size_t)length);
		else if (total == 16) {
			put_le32(chunk + 12, value);
			buffer_append(&file, chunk + 12, 4);
		}
		if (c->total > total) {
			/* The declared size is honest about the file's bytes. */
			bzero(chunk, sizeof(chunk));
			buffer_append(&file, chunk, c->total - total);
		}
		crc = crc32_update(crc, spec->disk + offset, (size_t)length);
		offset += length;
	}
	return (file_adopt(&file));
}

static void
fixture_init(fixture_t *f, uint64_t memory, uint64_t work)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-containers.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = memory;
	options.max_work = work;
	options.scratch_directory_fd = f->scratch;
	options.progress = progress_work;
	options.progress_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Everything the image charged is released with it. */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline_memory);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* Open an image of file, forcing the stack text when given. */
static fsimg_status_t
fixture_open(fixture_t *f, memory_file_t *file, const char *stack,
    fsimg_error_t *error)
{
	fsimg_image_options_t options;
	fsimg_io_ops_t ops;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	f->image = NULL;
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, file, &f->io, NULL) == FSIMG_OK);
	fsimg_image_options_init(&options);
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	return (fsimg_image_open(f->ctx, f->io, &options, &f->image, error));
}

static fsimg_status_t
read_exact(fsimg_io_t *io, uint64_t offset, void *buffer, size_t length,
    fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_read_exact(io, offset, buffer, length, &transferred,
	    error));
}

/* Read a disk range through a whole-disk selection. */
static fsimg_status_t
read_disk(fixture_t *f, uint64_t offset, void *buffer, size_t length,
    fsimg_error_t *error)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_io_t *disk;
	fsimg_status_t status;

	CHECK(fsimg_image_select(f->image, &selector, &disk, NULL) == FSIMG_OK);
	status = read_exact(disk, offset, buffer, length, error);
	fsimg_io_free(disk);
	return (status);
}

/* The container's disk reads as the expected bytes, in pieces that
 * straddle chunk boundaries.
 */
static void
expect_disk(fixture_t *f, const unsigned char *expected, uint64_t length)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char piece[7000];
	fsimg_io_t *disk;
	uint64_t size;
	size_t count;

	CHECK(fsimg_image_select(f->image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(disk, &size, NULL) == FSIMG_OK && size == length);
	for (uint64_t offset = 0; offset < length; offset += count) {
		count = length - offset < sizeof(piece) ? (size_t)(length - offset) :
		    sizeof(piece);
		CHECK(read_exact(disk, offset, piece, count, NULL) == FSIMG_OK);
		CHECK(bcmp(piece, expected + offset, count) == 0);
	}
	fsimg_io_free(disk);
}

/* Opening fails with the named check; the file is released. */
static void
expect_open_fails(memory_file_t file, const char *stack, fsimg_status_t status,
    const char *message)
{
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, stack, &error) == status);
	if (strcmp(error.message, message) != 0) {
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message,
		    error.message);
		CHECK(!"the open failed with another check");
	}
	fixture_free(&f);
	free(file.bytes);
}

static void
expect_dmg_fails(const dmg_spec_t *spec, fsimg_status_t status,
    const char *message)
{
	expect_open_fails(dmg_encode(spec), DETECT, status, message);
}

/* The image opens and passes structural verification; verification with
 * read-through fails with the named check, and so does reading the disk
 * when the fault is in a chunk rather than a checksum.
 */
static void
expect_payload_fails(memory_file_t file, bool read_fails, const char *message)
{
	fsimg_layer_report_t report;
	unsigned char *bytes;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	bytes = malloc((size_t)info.disk_bytes);
	CHECK(bytes != NULL);
	if (read_fails) {
		CHECK(read_disk(&f, 0, bytes, (size_t)info.disk_bytes, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, message) == 0);
	} else
		CHECK(read_disk(&f, 0, bytes, (size_t)info.disk_bytes, NULL) ==
		    FSIMG_OK);
	free(bytes);
	fixture_free(&f);
	free(file.bytes);
}

/* The oracle's CRC is the standard one: its check value proves it. */
static void
test_crc_oracle(void)
{
	CHECK(crc32_update(0, (const unsigned char *)"123456789", 9) ==
	    0xcbf43926u);
	CHECK(crc32_update(crc32_update(0, (const unsigned char *)"1234", 4),
	    (const unsigned char *)"56789", 5) == 0xcbf43926u);
}

/*
 * A DMG with every accepted run kind opens on detection alone, reports its
 * container, chunk count, codecs and segment ID, serves its disk byte for
 * byte, and verifies structurally and with read-through. A forced dmg slot
 * opens it the same way.
 */
static void
test_dmg_read(void)
{
	unsigned char *disk = disk_new(MIB);
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	memory_file_t file;
	dmg_spec_t spec;
	fixture_t f;

	disk_pattern(disk, 64 * SECTOR, 1);
	disk_pattern(disk + 128 * SECTOR, 1024 * SECTOR, 2);
	disk_pattern(disk + 1280 * SECTOR, 768 * SECTOR, 3);
	spec = dmg_whole_disk(disk, 2048);
	file = dmg_encode(&spec);

	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.container, "dmg") == 0);
	CHECK(strcmp(info.codecs, "raw,zero,zlib") == 0);
	CHECK(info.chunk_count == 5 && info.disk_bytes == MIB);
	CHECK(bcmp(info.container_guid, segment_id, sizeof(segment_id)) == 0);
	CHECK(info.scheme[0] == '\0');
	expect_disk(&f, disk, MIB);
	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
	CHECK(strcmp(report.container, "dmg") == 0 &&
	    report.scope != FSIMG_VALIDATION_PAYLOAD);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	CHECK(report.scope == FSIMG_VALIDATION_PAYLOAD);

	CHECK(fixture_open(&f, &file, "fat12/-/dmg", NULL) == FSIMG_OK);
	expect_disk(&f, disk, MIB);
	/* Another forced container never reads a DMG as its own. */
	CHECK(fixture_open(&f, &file, "fat12/-/simg", NULL) == FSIMG_EFORMAT);
	fixture_free(&f);
	free(file.bytes);

	/* hdiutil convert writes segment 1 of 1; create writes 0 of 0. */
	spec.segment_number = 1;
	spec.segment_count = 1;
	file = dmg_encode(&spec);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	expect_disk(&f, disk, MIB);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/*
 * Both table shapes read: hdiutil's one table per partition-map region,
 * listed in any order, and a table of free space alone, whose checksum is
 * the empty CRC.
 */
static void
test_dmg_table_shapes(void)
{
	unsigned char *disk = disk_new(MIB);
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	memory_file_t file;
	dmg_spec_t spec = { .disk = disk, .sectors = 2048, .tables = 3 };
	fixture_t f;

	disk_pattern(disk, 1024 * SECTOR, 4);
	spec.table[0] = (dmg_table_t){ 1024, 1024, 1, {
		{ RUN_IGNORE, 0, 1024, 0 } } };
	spec.table[1] = (dmg_table_t){ 0, 1, 1, { { RUN_RAW, 0, 1, 0 } } };
	spec.table[2] = (dmg_table_t){ 1, 1023, 2, {
		{ RUN_ZLIB, 0, 1000, 0 }, { RUN_RAW, 1000, 23, 0 } } };
	file = dmg_encode(&spec);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.chunk_count == 4 && strcmp(info.codecs, "raw,zero,zlib") == 0);
	expect_disk(&f, disk, MIB);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/* Each seeded table, run or trailer fault fails the open with its check. */
static void
test_dmg_seeded_failures(void)
{
	unsigned char *disk = disk_new(MIB), *large;
	dmg_spec_t base, spec;

	disk_pattern(disk, 64 * SECTOR, 1);
	disk_pattern(disk + 128 * SECTOR, 1024 * SECTOR, 2);
	disk_pattern(disk + 1280 * SECTOR, 768 * SECTOR, 3);
	base = dmg_whole_disk(disk, 2048);

	/* Two tables claiming the same sectors. */
	spec = (dmg_spec_t){ .disk = disk, .sectors = 2048, .tables = 2 };
	spec.table[0] = (dmg_table_t){ 0, 1024, 1, { { RUN_RAW, 0, 1024, 0 } } };
	spec.table[1] = (dmg_table_t){ 1000, 1048, 1, { { RUN_RAW, 0, 1048, 0 } } };
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG chunk ranges overlap");

	/* Tables that stop short of the declared sector count. */
	spec.tables = 1;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG chunk ranges leave part of the disk uncovered");
	spec.table[0] = (dmg_table_t){ 1024, 1024, 1, { { RUN_RAW, 0, 1024, 0 } } };
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG chunk ranges leave part of the disk uncovered");

	spec = base;
	spec.table[0].run[0].offset_bias = MIB;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG run lies outside the data fork");

	spec = base;
	spec.table[0].run[0].offset_bias = UINT64_MAX - 100;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG run lies outside the data fork");

	spec = base;
	spec.table[0].run[0].sectors = 0;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG run covers zero sectors");

	spec = base;
	spec.table[0].run[5].sectors = 769;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG run lies outside its block table");

	spec = base;
	spec.table[0].run[1].sectors = 32;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG block table runs do not add up to its sectors");

	spec = base;
	spec.table[0].count = 4096;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG block table lies outside the disk");

	spec = base;
	spec.table[0].run[0].type = RUN_BZIP2;
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG run uses bzip2 compression, which is not supported");

	spec = base;
	spec.table[0].run[0].type = 0x12345678u;
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG run type 0x12345678 is not supported");

	spec = base;
	spec.segment_number = 1;
	spec.segment_count = 2;
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "multi-segment DMG images are not supported");

	spec = base;
	spec.fork_extra = 1;
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG koly data fork overlaps its plist");

	/* A zlib run whose disk range exceeds what a cache slot holds. */
	large = disk_new(16 * MIB + SECTOR);
	spec = (dmg_spec_t){ .disk = large, .sectors = 32769, .tables = 1 };
	spec.table[0] = (dmg_table_t){ 0, 32769, 1, { { RUN_ZLIB, 0, 32769, 0 } } };
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG zlib run decodes to more than 16 MiB");
	free(large);

	/* Inflate producing fewer or more bytes than the run declares is
	 * found when the chunk is decoded, never served short or padded.
	 */
	spec = base;
	spec.zlib_delta = -1;
	expect_payload_fails(dmg_encode(&spec), true,
	    "compressed chunk decodes to fewer bytes than declared");
	spec.zlib_delta = 1;
	expect_payload_fails(dmg_encode(&spec), true,
	    "compressed chunk decodes to more bytes than declared");
	free(disk);
}

/*
 * The plist parser's limits: 32 levels of nesting read and 33 do not, a
 * key longer than 4 KiB is beyond the subset, and an entity XML does not
 * predefine is malformed.
 */
static void
test_dmg_plist_limits(void)
{
	unsigned char *disk = disk_new(MIB);
	buffer_t extra = { 0 };
	unsigned char *data;
	dmg_spec_t spec;
	memory_file_t file;
	fixture_t f;
	char *key;

	spec = dmg_whole_disk(disk, 2048);
	/* plist and dict are two levels; 29 arrays and a leaf make 32. */
	buffer_text(&extra, "\t<key>deep</key>\n");
	for (unsigned i = 0; i < 29; i++)
		buffer_text(&extra, "<array>");
	buffer_text(&extra, "<true/>");
	for (unsigned i = 0; i < 29; i++)
		buffer_text(&extra, "</array>");
	buffer_text(&extra, "\n");
	buffer_append(&extra, "", 1);
	spec.extra = (const char *)extra.bytes;
	file = dmg_encode(&spec);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	expect_disk(&f, disk, MIB);
	fixture_free(&f);
	free(file.bytes);

	extra.length = 0;
	buffer_text(&extra, "\t<key>deep</key>\n");
	for (unsigned i = 0; i < 30; i++)
		buffer_text(&extra, "<array>");
	buffer_text(&extra, "<true/>");
	for (unsigned i = 0; i < 30; i++)
		buffer_text(&extra, "</array>");
	buffer_append(&extra, "", 1);
	spec.extra = (const char *)extra.bytes;
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG plist uses nesting deeper than 32 elements, which is not supported");

	spec.extra = "\t<key>a&bogus;</key><string>x</string>\n";
	expect_dmg_fails(&spec, FSIMG_ECORRUPT, "DMG plist has an undeclared entity");

	spec.extra = "\t<key>a&#65;</key><string>x</string>\n";
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG plist uses a numeric character reference, which is not supported");

	spec.extra = "\t<key>a</key><string><![CDATA[x]]></string>\n";
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG plist uses a CDATA section, which is not supported");

	spec.extra = "\t<key>a</key><real>1.5</real>\n";
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG plist uses an element outside the UDIF subset, which is not supported");

	spec.extra = "\t<key>a</key><key>b</key>\n";
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG plist dict does not alternate keys and values");

	spec.extra = "\t<key>a</key><string>x</key>\n";
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG plist has an end tag that does not match its element");

	spec.extra = "\t<!-- a -- b -->\n";
	expect_dmg_fails(&spec, FSIMG_ECORRUPT,
	    "DMG plist has \"--\" inside a comment");

	key = malloc(5 * KIB + 64);
	CHECK(key != NULL);
	memset(key, 'k', 5 * KIB + 64);
	bcopy("\t<key>", key, 6);
	snprintf(key + 6 + 5 * KIB, 58, "</key><string>x</string>\n");
	spec.extra = key;
	expect_dmg_fails(&spec, FSIMG_EUNSUPPORTED,
	    "DMG plist uses text longer than 4 KiB, which is not supported");
	free(key);

	/* A blkx Data value that is not base64 data. */
	spec.extra = NULL;
	file = dmg_encode(&spec);
	data = memmem(file.bytes, file.length, "<data>\n\t\t\t\t", 11);
	CHECK(data != NULL);
	data[11] = '*';
	expect_open_fails(file, DETECT, FSIMG_ECORRUPT,
	    "DMG plist data holds a byte outside base64");
	free(extra.bytes);
	free(disk);
}

/* The trailer's data fork and master checksums are checked by verification;
 * a table checksum, which only decoded bytes can confirm, under
 * read-through alone.
 */
static void
test_dmg_checksums(void)
{
	unsigned char *disk = disk_new(MIB);
	fsimg_layer_report_t report;
	fsimg_error_t error;
	memory_file_t file;
	dmg_spec_t spec;
	fixture_t f;

	disk_pattern(disk, 64 * SECTOR, 1);
	spec = dmg_whole_disk(disk, 2048);
	file = dmg_encode(&spec);
	file.bytes[file.length - 512 + 88] ^= 1;
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "DMG data fork checksum does not match") == 0);
	free(file.bytes);

	file = dmg_encode(&spec);
	file.bytes[file.length - 512 + 360] ^= 1;
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "DMG master checksum does not match") == 0);
	fixture_free(&f);
	free(file.bytes);

	spec.table_crc_flip = 1;
	expect_payload_fails(dmg_encode(&spec), false,
	    "DMG block table 1 checksum does not match its decoded bytes");
	free(disk);
}

/* The bytes verification reads, through any I/O object of f's context. */
static uint64_t
verify_read_bytes(fixture_t *f)
{
	fsimg_layer_report_t report;
	fsimg_stats_t before, after;

	CHECK(fsimg_ctx_get_stats(f->ctx, &before, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f->image, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &after, NULL) == FSIMG_OK);
	return (after.read_bytes - before.read_bytes);
}

/*
 * Read-through verification folds a DMG zero run or an simg DONT_CARE
 * chunk into its CRC32 arithmetically: the bytes it reads follow the
 * stored data, not the 64 MiB of declared zeros.
 */
static void
test_verify_declared_zeros(void)
{
	const uint64_t zeros = 64 * MIB;
	unsigned char *disk = disk_new(zeros + MIB);
	memory_file_t file;
	dmg_spec_t dmg = { .disk = disk, .sectors = (zeros + MIB) / SECTOR,
	    .tables = 1 };
	simg_spec_t simg = { .disk = disk, .block_bytes = 4096, .chunks = 4 };
	fixture_t f;

	disk_pattern(disk, 32 * 1024, 8);
	disk_pattern(disk + 32 * 1024 + zeros, MIB - 32 * 1024, 9);
	dmg.table[0] = (dmg_table_t){ 0, dmg.sectors, 4, {
		{ RUN_RAW, 0, 64, 0 },
		{ RUN_ZERO, 64, zeros / SECTOR, 0 },
		{ RUN_ZLIB, 64 + zeros / SECTOR, 1024, 0 },
		{ RUN_ZLIB, 1088 + zeros / SECTOR, 960, 0 } } };
	file = dmg_encode(&dmg);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(verify_read_bytes(&f) < 4 * MIB);
	fixture_free(&f);
	free(file.bytes);

	simg.chunk[0] = (simg_chunk_t){ SIMG_RAW, 8, 0, 0, 0 };
	simg.chunk[1] = (simg_chunk_t){ SIMG_DONT_CARE, zeros / 4096, 0, 0, 0 };
	simg.chunk[2] = (simg_chunk_t){ SIMG_RAW, 248, 0, 0, 0 };
	simg.chunk[3] = (simg_chunk_t){ SIMG_CRC32, 0, 0, 0, 0 };
	file = simg_encode(&simg);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(verify_read_bytes(&f) < 4 * MIB);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/*
 * A bare disk whose last sector happens to hold a koly trailer of another
 * file's length draws no claim and opens as a bare disk; forcing dmg on it
 * fails as unrecognized.
 */
static void
test_stray_trailer(void)
{
	unsigned char *disk = disk_new(MIB);
	buffer_t bytes = { 0 };
	fsimg_image_info_t info;
	memory_file_t dmg, bare;
	dmg_spec_t spec;
	fsimg_error_t error;
	fixture_t f;

	spec = dmg_whole_disk(disk, 2048);
	dmg = dmg_encode(&spec);
	disk_pattern(disk, MIB, 9);
	bcopy(dmg.bytes + dmg.length - 512, disk + MIB - 512, 512);
	buffer_append(&bytes, disk, MIB);
	bare = file_adopt(&bytes);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &bare, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.container[0] == '\0' && info.chunk_count == 0);
	CHECK(info.disk_bytes == MIB);
	expect_disk(&f, disk, MIB);
	CHECK(fixture_open(&f, &bare, "fat12/-/dmg", &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no DMG trailer") == 0);
	fixture_free(&f);
	free(bare.bytes);
	free(dmg.bytes);
	free(disk);
}

/*
 * A fixed VHD is its disk and a footer; the footer's checks and the claim
 * tied to the file length.
 */
static void
test_vhd(void)
{
	unsigned char *disk = disk_new(MIB), footer[512];
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	buffer_t bytes = { 0 };
	uint32_t sum;
	memory_file_t file;
	fsimg_error_t error;
	fixture_t f;

	disk_pattern(disk, MIB, 5);
	file = vhd_encode(disk, MIB, MIB);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.container, "vhd") == 0 && strcmp(info.codecs, "raw") == 0);
	CHECK(info.chunk_count == 1 && info.disk_bytes == MIB);
	CHECK(bcmp(info.container_guid, vhd_id, sizeof(vhd_id)) == 0);
	expect_disk(&f, disk, MIB);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	/* No checksum covers a fixed VHD's disk, so nothing is read through. */
	CHECK(strcmp(report.container, "vhd") == 0 &&
	    report.scope != FSIMG_VALIDATION_PAYLOAD);
	fixture_free(&f);

	file.bytes[MIB + 64] ^= 1;
	expect_open_fails(file, DETECT, FSIMG_ECORRUPT,
	    "VHD footer checksum does not match");

	/* A footer resealed after a field change fails that field's check. */
	file = vhd_encode(disk, MIB, MIB);
	vhd_footer(footer, MIB, 2);
	put_be32(footer + 12, 0x00020000);
	put_be32(footer + 64, 0);
	sum = 0;
	for (unsigned i = 0; i < 512; i++)
		sum += footer[i];
	put_be32(footer + 64, ~sum);
	bcopy(footer, file.bytes + MIB, 512);
	expect_open_fails(file, DETECT, FSIMG_ECORRUPT,
	    "VHD footer format version is not 1.0");

	/* A dynamic or differencing image: the copy at offset 0 and a data
	 * offset inside the file identify it, and it is refused by type.
	 */
	vhd_footer(footer, 64 * MIB, 3);
	buffer_append(&bytes, footer, 512);
	buffer_append(&bytes, disk, 4096);
	buffer_append(&bytes, footer, 512);
	expect_open_fails(file_adopt(&bytes), DETECT, FSIMG_EUNSUPPORTED,
	    "dynamic VHD images are not supported");
	vhd_footer(footer, 64 * MIB, 4);
	buffer_append(&bytes, footer, 512);
	buffer_append(&bytes, disk, 4096);
	buffer_append(&bytes, footer, 512);
	expect_open_fails(file_adopt(&bytes), DETECT, FSIMG_EUNSUPPORTED,
	    "differencing VHD images are not supported");

	/* A size that does not match the file draws no claim: the file is a
	 * bare disk, and a forced vhd slot finds no footer.
	 */
	file = vhd_encode(disk, MIB, MIB + 512);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.container[0] == '\0' && info.disk_bytes == MIB + 512);
	CHECK(fixture_open(&f, &file, "fat12/-/vhd", &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no VHD footer") == 0);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/* The sparse disk the simg tests share: 64 blocks of 4 KiB. */
static simg_spec_t
simg_sample(unsigned char *disk)
{
	simg_spec_t spec = { .disk = disk, .block_bytes = 4096, .chunks = 5 };

	bzero(disk, 64 * 4096);
	disk_pattern(disk, 8 * 4096, 6);
	for (unsigned i = 0; i < 8 * 4096; i += 4)
		put_le32(disk + 8 * 4096 + i, 0x11223344u);
	disk_pattern(disk + 56 * 4096, 8 * 4096, 7);
	spec.chunk[0] = (simg_chunk_t){ SIMG_RAW, 8, 0, 0, 0 };
	spec.chunk[1] = (simg_chunk_t){ SIMG_FILL, 8, 0x11223344u, 0, 0 };
	spec.chunk[2] = (simg_chunk_t){ SIMG_DONT_CARE, 40, 0, 0, 0 };
	spec.chunk[3] = (simg_chunk_t){ SIMG_RAW, 8, 0, 0, 0 };
	spec.chunk[4] = (simg_chunk_t){ SIMG_CRC32, 0, 0, 0, 0 };
	return (spec);
}

/* RAW, FILL, DONT_CARE and CRC32 chunks expand to the disk. */
static void
test_simg(void)
{
	unsigned char *disk = disk_new(64 * 4096);
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	memory_file_t file;
	simg_spec_t spec;
	fixture_t f;

	spec = simg_sample(disk);
	file = simg_encode(&spec);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.container, "simg") == 0);
	CHECK(strcmp(info.codecs, "raw,zero,fill") == 0);
	CHECK(info.chunk_count == 4 && info.disk_bytes == 64 * 4096);
	expect_disk(&f, disk, 64 * 4096);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	CHECK(strcmp(report.container, "simg") == 0 &&
	    report.scope == FSIMG_VALIDATION_PAYLOAD);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/* A raw chunk preserves the bytes a failing backing read confirms,
 * including bytes produced by an earlier zero chunk in the same request.
 */
static void
test_raw_chunk_partial_error(void)
{
	static const size_t prefixes[] = { 0, 31 };
	unsigned char bytes[8192], output[96];
	simg_spec_t spec = { .disk = bytes, .block_bytes = 4096, .chunks = 2 };
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	memory_file_t file;
	fsimg_error_t error;
	fsimg_io_t *disk;
	size_t prefix, transferred;
	fixture_t f;

	bzero(bytes, sizeof(bytes));
	disk_pattern(bytes + 4096, 4096, 9);
	spec.chunk[0] = (simg_chunk_t){ .type = SIMG_DONT_CARE, .blocks = 1 };
	spec.chunk[1] = (simg_chunk_t){ .type = SIMG_RAW, .blocks = 1 };
	file = simg_encode(&spec);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &disk, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		prefix = prefixes[i];
		memset(output, 0xa5, sizeof(output));
		file.fail_at = file.reads + 1;
		file.fail_bytes = 7;
		CHECK(fsimg_io_read_exact(disk, 4096 - prefix, output,
		    prefix + 64, &transferred, &error) == FSIMG_EIO);
		CHECK(error.os_error == EIO && file.reads == file.fail_at);
		CHECK(transferred == prefix + 7);
		CHECK(bcmp(output, bytes + 4096 - prefix, transferred) == 0);
		for (size_t j = transferred; j < sizeof(output); j++)
			CHECK(output[j] == 0xa5);
		file.fail_at = 0;
		CHECK(fsimg_io_read_exact(disk, 4096, output, 64,
		    &transferred, NULL) == FSIMG_OK);
		CHECK(transferred == 64 && bcmp(output, bytes + 4096, 64) == 0);
	}
	fsimg_io_free(disk);
	fixture_free(&f);
	free(file.bytes);
}

static void
test_simg_seeded_failures(void)
{
	unsigned char *disk = disk_new(64 * 4096);
	simg_spec_t base, spec;

	base = simg_sample(disk);
	spec = base;
	spec.total_chunks_delta = 1;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image file holds fewer chunks than the header's total");
	spec.total_chunks_delta = -1;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image file holds more chunks than the header's total");

	spec = base;
	spec.total_blocks_delta = 1;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image chunks cover fewer blocks than the header's total");
	spec.total_blocks_delta = -1;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image chunks cover more blocks than the header's total");

	spec = base;
	spec.chunk[2].type = SIMG_RAW;
	spec.chunk[2].blocks = 0;
	spec.total_blocks_delta = 40;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image chunk covers no blocks, or a CRC32 chunk covers some");

	spec = base;
	spec.chunk[1].total = 20;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image chunk size does not match its type");

	spec = base;
	spec.block_bytes = 1022;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_ECORRUPT,
	    "sparse image block size is not a multiple of 4");

	spec = base;
	spec.major = 2;
	expect_open_fails(simg_encode(&spec), DETECT, FSIMG_EUNSUPPORTED,
	    "sparse image major version is not supported");

	spec = base;
	spec.chunk[4].crc_flip = 1;
	expect_payload_fails(simg_encode(&spec), false,
	    "sparse image CRC32 chunk does not match the expanded image");
	free(disk);
}

/*
 * Five zlib chunks of chunk_bytes each; chunk i starts with byte i + 1 and
 * is otherwise zero, so each is distinct and compresses well.
 */
static memory_file_t
five_chunk_dmg(uint64_t chunk_bytes, unsigned char **disk_out)
{
	uint64_t sectors = chunk_bytes / SECTOR;
	unsigned char *disk = disk_new(5 * chunk_bytes);
	dmg_spec_t spec = { .disk = disk, .sectors = 5 * sectors, .tables = 1 };

	spec.table[0].first = 0;
	spec.table[0].count = 5 * sectors;
	spec.table[0].runs = 5;
	for (unsigned i = 0; i < 5; i++) {
		disk[i * chunk_bytes] = (unsigned char)(i + 1);
		spec.table[0].run[i] = (dmg_run_t){ RUN_ZLIB, i * sectors, sectors, 0 };
	}
	*disk_out = disk;
	return (dmg_encode(&spec));
}

/*
 * Sequential reads decode each chunk once: the file bytes read after the
 * open are exactly the data fork, each chunk's bytes once. The first run
 * is raw and covers whatever the open's probes read, so no chunk is
 * cached before the count starts. Rereading any of the last four chunks
 * reads nothing more.
 */
static void
test_cache_sequential(void)
{
	unsigned char *disk = disk_new(MIB), piece[4096];
	dmg_spec_t spec = { .disk = disk, .sectors = 2048, .tables = 1 };
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	memory_file_t file;
	fsimg_io_t *volume;
	uint64_t fork_bytes, before;
	fixture_t f;

	disk_pattern(disk, MIB, 8);
	spec.table[0] = (dmg_table_t){ 0, 2048, 8, { { 0 } } };
	spec.table[0].run[0] = (dmg_run_t){ RUN_RAW, 0, 256, 0 };
	for (unsigned i = 1; i < 8; i++)
		spec.table[0].run[i] = (dmg_run_t){ RUN_ZLIB, i * 256u, 256, 0 };
	file = dmg_encode(&spec);
	for (fork_bytes = 0; bcmp(file.bytes + fork_bytes, "<?xml", 5) != 0;
	    fork_bytes++)
		;

	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &volume, NULL) == FSIMG_OK);
	file.read_bytes = 0;
	for (uint64_t offset = 0; offset < MIB; offset += sizeof(piece)) {
		CHECK(read_exact(volume, offset, piece, sizeof(piece),
		    NULL) == FSIMG_OK);
		CHECK(bcmp(piece, disk + offset, sizeof(piece)) == 0);
	}
	CHECK(file.read_bytes == fork_bytes);
	before = file.read_bytes;
	for (unsigned round = 0; round < 10; round++)
		for (unsigned chunk = 4; chunk < 8; chunk++)
			CHECK(read_exact(volume, chunk * 256u * SECTOR + 7,
			    piece, 100, NULL) == FSIMG_OK);
	CHECK(file.read_bytes == before);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/*
 * Five chunks of the 16 MiB maximum: access confined to four of them stays
 * cached, and alternating across all five decodes on every read until the
 * work limit stops it, the decoded bytes counted as work.
 */
static void
test_cache_alternation(void)
{
	unsigned char *disk, byte;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	memory_file_t file;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fsimg_status_t status;
	unsigned reads;
	fixture_t f;

	file = five_chunk_dmg(16 * MIB, &disk);
	fixture_init(&f, 128 * MIB, 8 * 16 * MIB);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &volume, NULL) == FSIMG_OK);
	for (unsigned round = 0; round < 100; round++)
		for (unsigned chunk = 0; chunk < 4; chunk++) {
			CHECK(read_exact(volume, chunk * 16 * MIB, &byte, 1,
			    NULL) == FSIMG_OK);
			CHECK(byte == chunk + 1);
		}
	for (reads = 0; reads < 100; reads++) {
		status = read_exact(volume, (4 + reads) % 5 * 16 * MIB,
		    &byte, 1, &error);
		if (status != FSIMG_OK)
			break;
		CHECK(byte == (4 + reads) % 5 + 1);
	}
	/* Four decodes fit the limit before the alternation starts. */
	CHECK(status == FSIMG_ELIMIT && error.resource == FSIMG_RESOURCE_WORK);
	CHECK(reads >= 2 && reads <= 4);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/*
 * Without a work limit, the decode bound of 64 times the compressed chunks'
 * decoded length plus 1 GiB stops an alternation that defeats the cache.
 * A terabyte of zero record after the five chunks costs the image nothing,
 * so it must not raise the bound.
 */
static void
test_cache_decode_bound(void)
{
	uint64_t chunk = 256 * KIB, bound = 64 * 5 * chunk + 1024 * MIB;
	unsigned char *bytes = disk_new(chunk), *stream, byte;
	uLongf encoded;
	fsimg_chunk_index_t *index;
	fsimg_session_t session;
	fsimg_chunk_t record;
	fsimg_io_ops_t ops;
	fsimg_io_t *disk;
	buffer_t streams = { 0 };
	memory_file_t file;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t reads, offsets[6];
	fixture_t f;

	stream = malloc(compressBound((uLong)chunk));
	CHECK(stream != NULL);
	for (unsigned i = 0; i < 5; i++) {
		bytes[0] = (unsigned char)(i + 1);
		encoded = compressBound((uLong)chunk);
		CHECK(compress2(stream, &encoded, bytes, (uLong)chunk, 9) == Z_OK);
		offsets[i] = streams.length;
		buffer_append(&streams, stream, encoded);
	}
	offsets[5] = streams.length;
	file = file_adopt(&streams);
	fixture_init(&f, 16 * MIB, 0);
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) == FSIMG_OK);
	fsimg_session_init(&session, f.ctx, "decode bound", 0);
	CHECK(fsimg_chunk_index_new(&session, "test", "test", 5 * chunk + (MIB << 20), 0,
	    file.length, &index, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 5; i++) {
		record = (fsimg_chunk_t){ i * chunk, chunk, offsets[i],
		    offsets[i + 1] - offsets[i], FSIMG_CHUNK_COMPRESSED,
		    FSIMG_CODEC_ZLIB };
		CHECK(fsimg_chunk_index_append(index, &record, NULL) == FSIMG_OK);
	}
	record = (fsimg_chunk_t){ 5 * chunk, MIB << 20, 0, 0, FSIMG_CHUNK_ZERO, 0 };
	CHECK(fsimg_chunk_index_append(index, &record, NULL) == FSIMG_OK);
	CHECK(fsimg_chunk_index_seal(index, NULL) == FSIMG_OK);
	CHECK(fsimg_chunk_disk_open(f.ctx, f.io, &session, index, &disk,
	    NULL) == FSIMG_OK);
	for (reads = 0;; reads++) {
		status = read_exact(disk, reads % 5 * chunk, &byte, 1, &error);
		if (status != FSIMG_OK)
			break;
		CHECK(byte == reads % 5 + 1);
	}
	CHECK(status == FSIMG_ELIMIT && error.resource == FSIMG_RESOURCE_WORK);
	CHECK(strcmp(error.message, "the chunk cache reached its decode bound") == 0);
	CHECK(reads == bound / chunk);
	fsimg_io_free(disk);
	fixture_free(&f);
	free(file.bytes);
	free(stream);
	free(bytes);
}

/* A memory budget that cannot hold four maximum chunks fails the open,
 * naming the chunk cache.
 */
static void
test_cache_memory(void)
{
	unsigned char *disk;
	memory_file_t file;
	fsimg_error_t error;
	fixture_t f;

	file = five_chunk_dmg(16 * MIB, &disk);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(strcmp(error.message, "the chunk cache exceeds the memory limit") == 0);
	fixture_free(&f);
	free(file.bytes);
	free(disk);
}

/*
 * Sealed records are reread from scratch, which is not trusted. A
 * compressed record whose decoded length grows past the cache slot after
 * sealing fails as changed in scratch instead of inflating past the slot:
 * the zlib stream here decodes to 16 MiB, the slot holds 1 MiB.
 */
static void
test_tampered_record(void)
{
	unsigned char *zeros = disk_new(16 * MIB), *stream, grown[8], byte;
	uLongf encoded = compressBound((uLong)(16 * MIB));
	fsimg_chunk_index_t *index;
	fsimg_session_t session;
	fsimg_chunk_t chunk;
	fsimg_io_ops_t ops;
	fsimg_io_t *disk;
	buffer_t bytes = { 0 };
	memory_file_t file;
	fsimg_error_t error;
	fixture_t f;

	stream = malloc(encoded);
	CHECK(stream != NULL);
	CHECK(compress2(stream, &encoded, zeros, (uLong)(16 * MIB), 9) == Z_OK);
	buffer_append(&bytes, stream, encoded);
	file = file_adopt(&bytes);
	fixture_init(&f, 128 * MIB, 0);
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) == FSIMG_OK);
	fsimg_session_init(&session, f.ctx, "tampered record", 0);
	CHECK(fsimg_chunk_index_new(&session, "test", "test", 16 * MIB, 0, file.length,
	    &index, NULL) == FSIMG_OK);
	chunk = (fsimg_chunk_t){ 0, MIB, 0, encoded, FSIMG_CHUNK_COMPRESSED,
	    FSIMG_CODEC_ZLIB };
	CHECK(fsimg_chunk_index_append(index, &chunk, NULL) == FSIMG_OK);
	chunk = (fsimg_chunk_t){ MIB, 15 * MIB, 0, 0, FSIMG_CHUNK_ZERO, 0 };
	CHECK(fsimg_chunk_index_append(index, &chunk, NULL) == FSIMG_OK);
	CHECK(fsimg_chunk_index_seal(index, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 8; i++)
		grown[i] = (unsigned char)((16 * MIB) >> (8 * i));
	CHECK(fsimg_store_write(fsimg_test_chunk_records(index), 8, grown,
	    sizeof(grown), NULL) == FSIMG_OK);
	CHECK(fsimg_chunk_disk_open(f.ctx, f.io, &session, index, &disk,
	    NULL) == FSIMG_OK);
	CHECK(read_exact(disk, 0, &byte, 1, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "test chunk record changed in scratch") == 0);
	fsimg_io_free(disk);
	fixture_free(&f);
	free(file.bytes);
	free(stream);
	free(zeros);
}

/* Open, verify with read-through and read the whole disk, as far as the
 * first failure.
 */
static fsimg_status_t
exercise_image(fixture_t *f)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_options_t options;
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	unsigned char *bytes;
	fsimg_io_t *disk;
	fsimg_status_t status;

	fsimg_image_options_init(&options);
	CHECK(fsimg_stack_parse(DETECT, &options.stack, NULL) == FSIMG_OK);
	status = fsimg_image_open(f->ctx, f->io, &options, &f->image, NULL);
	if (status == FSIMG_OK)
		status = fsimg_image_verify(f->image, FSIMG_VERIFY_PAYLOAD, &report,
		    NULL);
	if (status == FSIMG_OK)
		status = fsimg_image_get_info(f->image, &info, NULL);
	if (status == FSIMG_OK)
		status = fsimg_image_select(f->image, &selector, &disk, NULL);
	if (status != FSIMG_OK)
		return (status);
	bytes = malloc((size_t)info.disk_bytes);
	CHECK(bytes != NULL);
	status = read_exact(disk, 0, bytes, (size_t)info.disk_bytes, NULL);
	free(bytes);
	fsimg_io_free(disk);
	return (status);
}

/*
 * Every allocation, every scratch operation and every file read on the
 * way through an image fails in turn, each sweep injecting at least one
 * failure before a pass completes: each failure is reported with its
 * status, and the image releases everything it charged (fixture_free
 * checks the memory baseline). The scratch sweep runs with a one-page
 * metadata cache, so the chunk index spills to scratch.
 */
static void
fault_sweep(memory_file_t file)
{
	static const fsimg_meta_fault_t points[] = { FSIMG_META_OPEN,
	    FSIMG_META_RESIZE, FSIMG_META_WRITE, FSIMG_META_READ };
	fsimg_io_ops_t ops;
	fsimg_status_t status;
	fixture_t f;

	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	for (uint64_t attempt = 1;; attempt++) {
		CHECK(attempt < 10000);
		fixture_init(&f, 16 * MIB, 0);
		CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) ==
		    FSIMG_OK);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = exercise_image(&f);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK(attempt > 1);
			break;
		}
		CHECK(status == FSIMG_ENOMEM);
	}
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
		for (uint64_t attempt = 1;; attempt++) {
			CHECK(attempt < 10000);
			fixture_init(&f, 16 * MIB, 0);
			CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
			CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io,
			    NULL) == FSIMG_OK);
			fsimg_test_meta_fail_after(f.ctx, points[i], attempt, EIO);
			status = exercise_image(&f);
			fsimg_test_meta_fail_after(f.ctx, points[i], 0, 0);
			fixture_free(&f);
			if (status == FSIMG_OK) {
				CHECK(attempt > 1);
				break;
			}
			CHECK(status == FSIMG_EIO);
		}
	}
	for (uint64_t attempt = 1;; attempt++) {
		CHECK(attempt < 10000);
		fixture_init(&f, 16 * MIB, 0);
		CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) ==
		    FSIMG_OK);
		file.reads = 0;
		file.fail_at = attempt;
		status = exercise_image(&f);
		file.fail_at = 0;
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK(attempt > 1);
			break;
		}
		CHECK(status == FSIMG_EIO);
	}
	free(file.bytes);
}

/* The fault sweeps over a DMG with every run kind and a sparse image. */
static void
test_fault_sweeps(void)
{
	unsigned char *disk = disk_new(MIB), *sparse = disk_new(64 * 4096);
	dmg_spec_t dmg;
	simg_spec_t simg;

	disk_pattern(disk, 64 * SECTOR, 1);
	disk_pattern(disk + 128 * SECTOR, 1024 * SECTOR, 2);
	disk_pattern(disk + 1280 * SECTOR, 768 * SECTOR, 3);
	dmg = dmg_whole_disk(disk, 2048);
	fault_sweep(dmg_encode(&dmg));
	simg = simg_sample(sparse);
	fault_sweep(simg_encode(&simg));
	free(sparse);
	free(disk);
}

static void
progress_work(void *argument, const fsimg_progress_t *progress)
{
	fixture_t *f = argument;

	if (progress->work > f->work)
		f->work = progress->work;
}

/*
 * An 18 MiB disk as one DMG table: 1 MiB of raw pattern, 16 MiB of zeros
 * as one run of the middle type, and 1 MiB of raw pattern. *disk receives
 * the disk, which the caller frees.
 */
static memory_file_t
three_run_dmg(uint32_t middle, unsigned char **disk)
{
	const uint64_t sectors = 18 * MIB / SECTOR;
	dmg_spec_t spec = { .sectors = sectors, .tables = 1 };

	*disk = disk_new(18 * MIB);
	disk_pattern(*disk, 18 * MIB, 7);
	bzero(*disk + MIB, 16 * MIB);
	spec.disk = *disk;
	spec.table[0] = (dmg_table_t){ 0, sectors, 3, {
		{ RUN_RAW, 0, MIB / SECTOR, 0 },
		{ middle, MIB / SECTOR, 16 * MIB / SECTOR, 0 },
		{ RUN_RAW, 17 * MIB / SECTOR, MIB / SECTOR, 0 } } };
	return (dmg_encode(&spec));
}

/* Compose the open image's whole disk as one partition of a GPT disk. */
static fsimg_status_t
compose_to_sink(fixture_t *f, const fsimg_sink_t *sink, fsimg_error_t *error)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_write_options_t options;
	fsimg_part_t part;
	fsimg_io_t *disk;
	fsimg_status_t status;

	CHECK(fsimg_image_select(f->image, &selector, &disk, NULL) == FSIMG_OK);
	part = (fsimg_part_t){ .volume = disk, .type = "linux" };
	fsimg_write_options_init(&options);
	if (f->compression != NULL)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    f->compression);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = INT64_C(1700000000);
	status = fsimg_image_compose(f->ctx, "gpt", &part, 1, &options, sink,
	    error);
	fsimg_io_free(disk);
	return (status);
}

/*
 * Publish the composed disk as a file under name in the scratch directory.
 * On success *out receives its bytes and *length its length, and the file
 * is removed; on failure nothing may have been published.
 */
static fsimg_status_t
compose_disk(fixture_t *f, const char *name, unsigned char **out,
    uint64_t *length, fsimg_error_t *error)
{
	fsimg_sink_t sink;
	struct stat st;
	fsimg_status_t status;
	int fd;

	*out = NULL;
	*length = 0;
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f->scratch;
	sink.name = name;
	status = compose_to_sink(f, &sink, error);
	fd = openat(f->scratch, name, O_RDONLY | O_CLOEXEC);
	if (status != FSIMG_OK) {
		CHECK(fd == -1 && errno == ENOENT);
		return (status);
	}
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	*length = (uint64_t)st.st_size;
	*out = malloc((size_t)*length);
	CHECK(*out != NULL);
	CHECK(pread(fd, *out, (size_t)*length, 0) == (ssize_t)*length);
	CHECK(close(fd) == 0 && unlinkat(f->scratch, name, 0) == 0);
	return (FSIMG_OK);
}

/*
 * A DMG's zero run is composed from its chunk index without a read or a
 * decode: the file's bytes read and the work reported stay far below the
 * run, while the same disk with the run stored as zlib-compressed zeros
 * decodes all 16 MiB. Both compose to the same bytes. Corrupting the
 * compressed counterpart fails the compose as corrupt, naming the layer,
 * which proves the decode ran.
 */
static void
test_container_zero_runs(void)
{
	memory_file_t files[2];
	unsigned char *disks[2], *composed[2];
	uint64_t lengths[2], read[2], work[2];
	fsimg_error_t error;
	fixture_t f;

	files[0] = three_run_dmg(RUN_ZERO, &disks[0]);
	files[1] = three_run_dmg(RUN_ZLIB, &disks[1]);
	for (unsigned i = 0; i < 2; i++) {
		fixture_init(&f, 256 * MIB, 0);
		CHECK(fixture_open(&f, &files[i], DETECT, NULL) == FSIMG_OK);
		files[i].read_bytes = 0;
		f.work = 0;
		CHECK(compose_disk(&f, "composed", &composed[i], &lengths[i],
		    NULL) == FSIMG_OK);
		read[i] = files[i].read_bytes;
		work[i] = f.work;
		fixture_free(&f);
	}
	CHECK(read[0] <= 2 * MIB + 64 * KIB && work[0] < MIB);
	CHECK(work[1] >= 16 * MIB);
	CHECK(lengths[0] == lengths[1] &&
	    bcmp(composed[0], composed[1], (size_t)lengths[0]) == 0);

	/* The zlib stream, whose header this spoils, starts right after the
	 * first raw run.
	 */
	free(composed[1]);
	files[1].bytes[MIB] ^= 0x55;
	fixture_init(&f, 256 * MIB, 0);
	CHECK(fixture_open(&f, &files[1], DETECT, NULL) == FSIMG_OK);
	CHECK(compose_disk(&f, "corrupt", &composed[1], &lengths[1], &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "dmg") == 0);
	fixture_free(&f);
	for (unsigned i = 0; i < 2; i++) {
		free(composed[i]);
		free(files[i].bytes);
		free(disks[i]);
	}
}

/*
 * With a one-page metadata cache the chunk index lives in scratch, and the
 * zero-range answers come from record reads through it: composing twice,
 * from a cold cache and a warm one, gives the same bytes with scratch
 * reads in the counters. A metadata read failure at every attempt in turn
 * fails the open or the compose with the store's error and publishes
 * nothing.
 */
static void
test_container_index_faults(void)
{
	memory_file_t file;
	unsigned char *disk, *composed[2];
	uint64_t lengths[2];
	fsimg_stats_t stats;
	fsimg_status_t status;
	fixture_t f;

	file = three_run_dmg(RUN_ZERO, &disk);
	fixture_init(&f, 256 * MIB, 0);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 2; i++)
		CHECK(compose_disk(&f, "composed", &composed[i], &lengths[i],
		    NULL) == FSIMG_OK);
	CHECK(lengths[0] == lengths[1] &&
	    bcmp(composed[0], composed[1], (size_t)lengths[0]) == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.read_bytes > file.read_bytes);
	fixture_free(&f);
	free(composed[1]);

	for (uint64_t attempt = 1;; attempt++) {
		CHECK(attempt < 1000);
		fixture_init(&f, 256 * MIB, 0);
		CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
		fsimg_test_meta_fail_after(f.ctx, FSIMG_META_READ, attempt, EIO);
		status = fixture_open(&f, &file, DETECT, NULL);
		if (status == FSIMG_OK)
			status = compose_disk(&f, "faulted", &composed[1],
			    &lengths[1], NULL);
		fsimg_test_meta_fail_after(f.ctx, FSIMG_META_READ, 0, 0);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK(attempt > 1 && lengths[1] == lengths[0] &&
			    bcmp(composed[0], composed[1], (size_t)lengths[0]) == 0);
			free(composed[1]);
			break;
		}
		CHECK(status == FSIMG_EIO);
	}
	free(composed[0]);
	free(file.bytes);
	free(disk);
}

/*
 * A DMG declaring a 1 PiB disk of free space in one record opens and
 * answers that whole disk zero in record reads, never in bytes. Composing
 * it still owes its encoder every chunk, which the work limit bounds: the
 * compose to a stream fails with FSIMG_RESOURCE_WORK at the limit, plain
 * and under gzip, without requiring a host file that can hold 1 PiB. It
 * reads only the table chunks of the assembled disk, where
 * the same compose with hole knowledge ignored reads every chunk it
 * reaches. A 64 MiB disk of the same kind composes within the limit.
 */
static void
test_vast_declared_zeros(void)
{
	const uint64_t sectors = UINT64_C(1) << 41;
	dmg_spec_t spec = { .sectors = sectors, .tables = 1 };
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char *composed;
	memory_file_t file, small;
	fsimg_error_t error;
	fsimg_sink_t sink;
	fsimg_stats_t stats;
	fsimg_io_t *disk;
	uint64_t start, end, length, before, read[2];
	fixture_t f;
	int fd;

	spec.table[0] = (dmg_table_t){ 0, sectors, 1, {
		{ RUN_IGNORE, 0, sectors, 0 } } };
	file = dmg_encode(&spec);
	fixture_init(&f, 64 * MIB, 4096);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &disk, NULL) == FSIMG_OK);
	file.read_bytes = 0;
	CHECK(fsimg_io_next_zero_range(disk, 0, &start, &end, NULL) == FSIMG_OK);
	CHECK(start == 0 && end == sectors * SECTOR && file.read_bytes == 0);
	fsimg_io_free(disk);
	fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = fd;
	for (unsigned gzip = 0; gzip < 2; gzip++) {
		f.compression = gzip ? "gzip" : NULL;
		for (unsigned consult = 0; consult < 2; consult++) {
			fsimg_test_hole_discovery(f.ctx, consult == 1);
			CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
			before = stats.read_bytes;
			CHECK(compose_to_sink(&f, &sink, &error) ==
			    FSIMG_ELIMIT);
			CHECK(error.resource == FSIMG_RESOURCE_WORK);
			CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
			read[consult] = stats.read_bytes - before;
		}
		CHECK(read[1] < 4 * MIB && read[0] >= 64 * MIB);
	}
	CHECK(close(fd) == 0);
	f.compression = NULL;

	spec.sectors = 64 * MIB / SECTOR;
	spec.table[0] = (dmg_table_t){ 0, spec.sectors, 1, {
		{ RUN_IGNORE, 0, spec.sectors, 0 } } };
	small = dmg_encode(&spec);
	CHECK(fixture_open(&f, &small, DETECT, NULL) == FSIMG_OK);
	CHECK(compose_disk(&f, "small", &composed, &length, NULL) == FSIMG_OK);
	free(composed);
	fixture_free(&f);
	free(small.bytes);
	free(file.bytes);
}

/*
 * One zero answer joins every consecutive zero record, however small: 2100
 * one-sector zero runs answer as one range, so composing them skips the
 * whole transcode chunk they cover, and the compose reads at least 1 MiB
 * less than with hole knowledge ignored. Each record read is a charged
 * unit of work in the index store, which bounds the walk.
 */
static void
test_many_zero_records(void)
{
	static dmg_spec_t spec = { .sectors = 2100, .tables = 1 };
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char *bytes, *composed;
	memory_file_t file;
	fsimg_stats_t stats;
	fsimg_io_t *disk;
	uint64_t start, end, length, before, read[2];
	fixture_t f;

	bytes = disk_new(2100 * SECTOR);
	spec.disk = bytes;
	spec.table[0].first = 0;
	spec.table[0].count = 2100;
	spec.table[0].runs = 2100;
	for (unsigned i = 0; i < 2100; i++)
		spec.table[0].run[i] = (dmg_run_t){ RUN_ZERO, i, 1, 0 };
	file = dmg_encode(&spec);
	fixture_init(&f, 64 * MIB, 0);
	CHECK(fixture_open(&f, &file, DETECT, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_next_zero_range(disk, 0, &start, &end, NULL) == FSIMG_OK);
	CHECK(start == 0 && end == 2100 * SECTOR);
	fsimg_io_free(disk);
	for (unsigned consult = 0; consult < 2; consult++) {
		fsimg_test_hole_discovery(f.ctx, consult == 1);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		before = stats.read_bytes;
		CHECK(compose_disk(&f, "many", &composed, &length, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		read[consult] = stats.read_bytes - before;
		free(composed);
	}
	CHECK(read[1] + MIB <= read[0]);
	fixture_free(&f);
	free(file.bytes);
	free(bytes);
}

/* A 17 MiB disk as one DMG table: 1 MiB of raw pattern, whose bytes start
 * the file, and 16 MiB of zeros as one run of the tail type.
 */
static memory_file_t
tail_dmg(uint32_t tail, unsigned char **disk)
{
	const uint64_t sectors = 17 * MIB / SECTOR;
	dmg_spec_t spec = { .sectors = sectors, .tables = 1 };

	*disk = disk_new(17 * MIB);
	disk_pattern(*disk, MIB, 9);
	spec.disk = *disk;
	spec.table[0] = (dmg_table_t){ 0, sectors, 2, {
		{ RUN_RAW, 0, MIB / SECTOR, 0 },
		{ tail, MIB / SECTOR, 16 * MIB / SECTOR, 0 } } };
	return (dmg_encode(&spec));
}

/*
 * A DMG input whose file is cut short after the image opened. Cut behind
 * a zero record, its tail needs no bytes, so the compose publishes the
 * declared zeros, the image the intact file composes to. Cut under the raw
 * record, or under a compressed record never decoded, the chunk read
 * fails with the exact-read premature-end error, FSIMG_EIO, and nothing
 * is published.
 */
static void
test_truncated_container(void)
{
	memory_file_t files[2];
	unsigned char *disks[2], *composed[2];
	uint64_t lengths[2];
	fixture_t f;

	files[0] = tail_dmg(RUN_ZERO, &disks[0]);
	files[1] = tail_dmg(RUN_ZLIB, &disks[1]);
	fixture_init(&f, 256 * MIB, 0);
	CHECK(fixture_open(&f, &files[0], DETECT, NULL) == FSIMG_OK);
	CHECK(compose_disk(&f, "intact", &composed[0], &lengths[0], NULL) ==
	    FSIMG_OK);
	files[0].cut = MIB;
	CHECK(compose_disk(&f, "cut", &composed[1], &lengths[1], NULL) ==
	    FSIMG_OK);
	CHECK(lengths[0] == lengths[1] &&
	    bcmp(composed[0], composed[1], (size_t)lengths[0]) == 0);
	free(composed[1]);
	files[0].cut = MIB / 2;
	CHECK(compose_disk(&f, "cut", &composed[1], &lengths[1], NULL) ==
	    FSIMG_EIO);
	CHECK(fixture_open(&f, &files[1], DETECT, NULL) == FSIMG_OK);
	files[1].cut = MIB + 1;
	CHECK(compose_disk(&f, "cut", &composed[1], &lengths[1], NULL) ==
	    FSIMG_EIO);
	fixture_free(&f);
	for (unsigned i = 0; i < 2; i++) {
		free(files[i].bytes);
		free(disks[i]);
	}
	free(composed[0]);
}

static void
write_seed(const char *path, memory_file_t file)
{
	FILE *out;

	out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite(file.bytes, 1, file.length, out) == file.length);
	CHECK(fclose(out) == 0);
	free(file.bytes);
}

int
main(int argc, char **argv)
{
	unsigned char *disk;
	dmg_spec_t dmg;
	simg_spec_t simg;
	memory_file_t file;
	char *xml;

	/* Small images seed the container and plist fuzzers' corpora. */
	if (argc == 3 && strcmp(argv[1], "--seed-dmg") == 0) {
		disk = disk_new(MIB);
		disk_pattern(disk, 64 * SECTOR, 1);
		dmg = dmg_whole_disk(disk, 2048);
		write_seed(argv[2], dmg_encode(&dmg));
		free(disk);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-plist") == 0) {
		disk = disk_new(MIB);
		dmg = dmg_whole_disk(disk, 2048);
		file = dmg_encode(&dmg);
		xml = memmem(file.bytes, file.length, "<?xml", 5);
		CHECK(xml != NULL);
		file.length -= 512;
		file.length -= (size_t)((unsigned char *)xml - file.bytes);
		bcopy(xml, file.bytes, file.length);
		write_seed(argv[2], file);
		free(disk);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-vhd") == 0) {
		disk = disk_new(64 * KIB);
		disk_pattern(disk, 64 * KIB, 5);
		write_seed(argv[2], vhd_encode(disk, 64 * KIB, 64 * KIB));
		free(disk);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-simg") == 0) {
		disk = disk_new(64 * 4096);
		simg = simg_sample(disk);
		simg.block_bytes = 512;
		write_seed(argv[2], simg_encode(&simg));
		free(disk);
		return (0);
	}
	CHECK(argc == 1);
	test_crc_oracle();
	test_dmg_read();
	test_dmg_table_shapes();
	test_dmg_seeded_failures();
	test_dmg_plist_limits();
	test_dmg_checksums();
	test_verify_declared_zeros();
	test_stray_trailer();
	test_vhd();
	test_simg();
	test_raw_chunk_partial_error();
	test_simg_seeded_failures();
	test_cache_sequential();
	test_cache_alternation();
	test_cache_decode_bound();
	test_cache_memory();
	test_tampered_record();
	test_fault_sweeps();
	test_container_zero_runs();
	test_container_index_faults();
	test_vast_declared_zeros();
	test_truncated_container();
	test_many_zero_records();
	return (0);
}
