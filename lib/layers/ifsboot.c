/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "io.h"
#include "layer.h"

/*
 * QNX boot images (specs/design-layers.md#qnx-boot-image-ifsboot). A boot
 * image is an optional boot prefix, the startup section and
 * the stored body, and nothing after; the body is the ImageFS the
 * container carries, stored raw or as a framed sequence of LZO1X or NRV2B
 * chunks.
 *
 * Let B be the prefix length, the file offset of the 256-byte
 * little-endian startup header. B comes from a closed table of boot
 * prefixes, and a header claims only where its preboot_size equals B, so
 * a signature anywhere else, such as inside startup code, never claims.
 * Every size below counts from B:
 *
 *   [0, B)                       the opaque boot prefix
 *   [B, B + startup_size)        the startup section: the header, opaque
 *                                startup code and the startup checksum word
 *   [B + startup_size,
 *    B + stored_size)            the body: the ImageFS itself, or chunks
 *                                (a 16-bit big-endian stored length and
 *                                that many bytes each), a zero length, zero
 *                                padding and the 4-byte stored trailer
 *
 * A raw body is a slice of the file. A framed body states no decoded
 * lengths, so the open learns its chunk index: it decodes every chunk once,
 * in order, into a bounded buffer it discards, and appends one compressed
 * record per chunk to the common chunk index, whose adapter then serves
 * reads through its cache. Every framing rule is enforced in that one pass,
 * and its bounds bound the index: a non-final chunk decodes to at least
 * 4096 bytes and decoding stops at imagefs_size, so the pass appends at
 * most imagefs_size / 4096 + 1 records whatever the stored length.
 *
 * Verification adds the two additive sums, both zero in every native
 * image, and the ram_size relation where the machine table checks it.
 * Nothing inside the prefix or the startup code is interpreted or
 * reported; the header's target addresses are reported, never used.
 *
 * Writing packages a disk, the ImageFS, behind a synthesized header, the
 * caller's startup code and boot prefix copied in as opaque bytes. The
 * plan checks every argument before any sink opens and reads the bounded
 * prefix whole; the encoder writes the file in order: the prefix, the
 * header, the code and the startup checksum word, then the body, raw
 * under compress=none, or under lzo as chunks of the project's LZO1X
 * encoder in the framing above, after a first pass that learns the framed
 * length stored_size states. Before its first byte the encoder decodes
 * the header it built with the reader's own decode_header, which must
 * tie at the prefix it wrote, so the writer and the reader share every
 * table and rule. Sections, for unwrap, are the prefix and the code.
 */
#define STARTUP_SIGNATURE	UINT32_C(0x00ff7eeb)
#define HEADER_BYTES		256u
#define STARTUP_MIN_BYTES	260u	/* The header and the checksum word */
#define BIOS_PREFIX_BYTES	1088u
#define PROBE_BYTES		(BIOS_PREFIX_BYTES + HEADER_BYTES)
#define LENGTH_WORD_BYTES	2u
#define TRAILER_BYTES		4u
/* The terminator, its smallest padding and the trailer. */
#define FRAMING_MIN_BYTES	8u
/* The smallest non-final decoded chunk any native image shows. */
#define NONFINAL_MIN_DECODED	4096u
#define SUM_PIECE_BYTES		(64u * 1024)
/* The header's opaque tail, which the writer zeroes and info counts. */
#define HEADER_TAIL		0x32u
/* The writer's copy unit for the startup code. */
#define COPY_PIECE_BYTES	(64u * 1024)

/* The offsets of the startup header's fields, from B, named as in QNX's
 * struct startup_header; HEADER_HEADER_SIZE is the offset of header_size.
 */
enum {
	HEADER_SIGNATURE = 0x00,
	HEADER_VERSION = 0x04,
	HEADER_FLAGS1 = 0x06,
	HEADER_FLAGS2 = 0x07,
	HEADER_HEADER_SIZE = 0x08,
	HEADER_MACHINE = 0x0a,
	HEADER_STARTUP_VADDR = 0x0c,
	HEADER_PADDR_BIAS = 0x10,
	HEADER_IMAGE_PADDR = 0x14,
	HEADER_RAM_PADDR = 0x18,
	HEADER_RAM_SIZE = 0x1c,
	HEADER_STARTUP_SIZE = 0x20,
	HEADER_STORED_SIZE = 0x24,
	HEADER_IMAGEFS_PADDR = 0x28,
	HEADER_IMAGEFS_SIZE = 0x2c,
	HEADER_PREBOOT_SIZE = 0x30
};

/* flags1: the virtual bit, the big-endian bit, the compression value and
 * reserved bits.
 */
#define FLAGS1_VIRTUAL		0x01u
#define FLAGS1_BIG_ENDIAN	0x02u
#define FLAGS1_COMPRESSION	0x1cu
#define FLAGS1_RESERVED		0xe0u

/* The compression values of flags1. */
#define COMPRESSION_NONE	0x00u
#define COMPRESSION_ZLIB	0x04u
#define COMPRESSION_LZO		0x08u
#define COMPRESSION_UCL		0x0cu

/* The closed prefix table, in the order the probe tests it: no prefix, and
 * the x86 BIOS loader.
 */
static const uint32_t boot_prefixes[] = { 0, BIOS_PREFIX_BYTES };

/* The ifsboot:compress modes, as bits of a machine's writable modes. */
#define WRITES_NONE		0x01u
#define WRITES_LZO		0x02u

/* The closed machine table: each machine, the prefix it was observed
 * behind, whether its ram_size is startup_size + imagefs_size, and the
 * modes the writer accepts for it, exactly those a native boot of a
 * written image has passed. A ram_sum machine's ram_size is checked on
 * read and written as the sum. Machine 0 is nonbootable mkifs output:
 * mkifs writes ram_size 0 there (E7), which the reader reports without a
 * check, and no boot gates its modes.
 */
static const struct boot_machine {
	uint16_t	 machine;
	uint32_t	 prefix;
	bool		 ram_sum;
	unsigned	 writes;
} boot_machines[] = {
	{ 42, 0, true, 0 },			/* SH-4 (S1) */
	{ 3, BIOS_PREFIX_BYTES, true, 0 },	/* x86 (S2) */
	{ 0, 0, false, WRITES_NONE | WRITES_LZO } /* nonbootable (E7) */
};

/* A validated startup header. prefix is B, the header's file offset; the
 * sizes count from it. codec is the chunk codec of a framed body, zero for
 * a raw one. flags2 and tail_nonzero, the count of nonzero bytes in the
 * header's tail, are the opaque fields info reports.
 */
typedef struct boot_header {
	uint64_t	 prefix;
	uint32_t	 startup_vaddr;
	uint32_t	 paddr_bias;
	uint32_t	 image_paddr;
	uint32_t	 ram_paddr;
	uint32_t	 ram_size;
	uint32_t	 startup_size;
	uint32_t	 stored_size;
	uint32_t	 imagefs_paddr;
	uint32_t	 imagefs_size;
	uint16_t	 version;
	uint16_t	 machine;
	uint16_t	 preboot_size;
	uint8_t		 flags1;
	uint8_t		 flags2;
	uint8_t		 compression;
	uint32_t	 tail_nonzero;
	uint32_t	 codec;
	bool		 ram_sum;
} boot_header_t;

/* What the learning pass found, for info: the chunks, the decoded and
 * stored extremes.
 */
typedef struct chunk_stats {
	uint64_t	 chunks;
	uint64_t	 min_decoded;
	uint64_t	 max_decoded;
	uint64_t	 max_stored;
} chunk_stats_t;

/* One decoded chunk whose record waits until the next length word says
 * whether it was the final one.
 */
typedef struct pending_chunk {
	fsimg_chunk_t	 record;
	bool		 valid;
} pending_chunk_t;

/* The learning pass's state: the file it reads, the index it fills, and
 * the two buffers it decodes through, a stored chunk and its decoded
 * bytes, both owned and released after the pass.
 */
typedef struct learning {
	const fsimg_container_input_t *input;
	const boot_header_t *header;
	fsimg_chunk_index_t *index;
	unsigned char	*stored;
	unsigned char	*decoded;
	chunk_stats_t	 stats;
} learning_t;

/*
 * A running sum of little-endian 32-bit words over a byte sequence fed in
 * pieces of any length: position is the sequence's length so far, which
 * places each byte in its word. Zero bytes add nothing and only advance
 * the position.
 */
typedef struct word_sum {
	uint32_t	 sum;
	uint64_t	 position;
} word_sum_t;

/*
 * The encoder's state. Under lzo, framed[pass] counts each pass's chunks,
 * length words included, the one number the first pass hands the second;
 * zeros is a zero unit to encode in place of a NULL chunk and stored one
 * chunk's stream. plain sums the disk's words in the current pass and
 * body the stored body's words as they are written, from B +
 * startup_size; copy is the startup code's copy buffer.
 */
typedef struct boot_encoder {
	fsimg_session_t	*session;
	fsimg_seq_sink_t *sink;
	const fsimg_container_settings_t *settings;
	fsimg_io_t	*startup;
	fsimg_lzo1x_encoder_t *lzo;
	unsigned char	*zeros;
	unsigned char	*stored;
	unsigned char	*copy;
	size_t		 stored_capacity;
	uint64_t	 disk_bytes;
	uint64_t	 framed[2];
	word_sum_t	 plain;
	word_sum_t	 body;
	bool		 compressed;
	bool		 verified_disk;
	bool		 misplace;	/* Test control */
} boot_encoder_t;

static fsimg_status_t boot_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t boot_unsupported(fsimg_error_t *, const char *);
static bool header_at(const unsigned char *, size_t, uint32_t);
static fsimg_status_t decode_header(const unsigned char *, size_t, uint64_t,
    boot_header_t *, fsimg_error_t *);
static fsimg_status_t check_tables(boot_header_t *, fsimg_error_t *);
static const struct boot_machine *find_machine(uint32_t);
static fsimg_status_t check_bounds(const boot_header_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t ifsboot_probe(const unsigned char *, size_t,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static fsimg_status_t read_header(const fsimg_container_input_t *,
    boot_header_t *, fsimg_error_t *);
static fsimg_status_t read_bytes(const fsimg_container_input_t *, uint64_t,
    void *, size_t, fsimg_error_t *);
static fsimg_status_t decode_chunk(learning_t *, uint64_t, size_t, size_t,
    size_t *, fsimg_error_t *);
static void count_chunk(chunk_stats_t *, size_t, size_t);
static fsimg_status_t keep_chunk(learning_t *, pending_chunk_t *,
    fsimg_error_t *);
static fsimg_status_t check_padding(learning_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t learn_chunks(learning_t *, fsimg_error_t *);
static fsimg_status_t open_framed(const fsimg_container_input_t *,
    const boot_header_t *, fsimg_container_t *, chunk_stats_t *,
    fsimg_error_t *);
static void report_details(const boot_header_t *, const chunk_stats_t *,
    fsimg_container_t *);
static fsimg_status_t ifsboot_open(const fsimg_container_input_t *,
    fsimg_container_t *, fsimg_error_t *);
static fsimg_status_t ifsboot_verify(const fsimg_container_input_t *,
    const fsimg_container_t *, bool, fsimg_error_t *);
static fsimg_status_t ifsboot_section(const fsimg_container_input_t *,
    fsimg_file_setting_t, uint64_t *, uint64_t *, fsimg_error_t *);
static uint64_t word_padded(uint64_t);
static uint64_t startup_bytes(uint64_t);
static fsimg_status_t read_setting(fsimg_io_t *, uint64_t, unsigned char *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t setting_short(fsimg_error_t *, const char *);
static fsimg_status_t buffer_prefix(const fsimg_container_plan_args_t *,
    const fsimg_planned_file_t *, uint64_t, fsimg_container_settings_t *,
    fsimg_error_t *);
static fsimg_status_t ifsboot_plan(const fsimg_container_plan_args_t *,
    fsimg_container_settings_t *, fsimg_error_t *);
static fsimg_status_t field_overflow(fsimg_error_t *, const char *,
    const char *);
static fsimg_status_t ifsboot_bound(const fsimg_container_settings_t *,
    uint64_t, uint64_t *, fsimg_error_t *);
static void sum_bytes(word_sum_t *, const unsigned char *, size_t);
static void put_header(unsigned char *, const fsimg_container_settings_t *,
    uint32_t, uint32_t, uint32_t);
static void plant_misplaced_header(unsigned char *, const unsigned char *,
    size_t);
static fsimg_status_t check_emission(const boot_encoder_t *,
    const unsigned char *, size_t, uint64_t, fsimg_error_t *);
static fsimg_status_t copy_code(boot_encoder_t *, word_sum_t *,
    fsimg_error_t *);
static fsimg_status_t emit_leading(boot_encoder_t *, uint64_t,
    fsimg_error_t *);
static uint64_t framed_body(uint64_t);
static fsimg_status_t boot_changed(fsimg_error_t *, const char *);
static fsimg_status_t write_body(boot_encoder_t *, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t put_chunk(boot_encoder_t *, unsigned,
    const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t boot_encode_open(const fsimg_encode_t *, void **,
    fsimg_error_t *);
static fsimg_status_t boot_encode_chunk(void *, unsigned,
    const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t boot_encode_finish(void *, unsigned, fsimg_error_t *);
static void boot_encode_close(void *);

static const fsimg_encoder_ops_t boot_encoder = {
	.open = boot_encode_open,
	.chunk = boot_encode_chunk,
	.finish = boot_encode_finish,
	.close = boot_encode_close
};

const fsimg_container_ops_t fsimg_ifsboot_container = {
	.probe_bytes = PROBE_BYTES,
	.probe = ifsboot_probe,
	.open = ifsboot_open,
	.verify = ifsboot_verify,
	.plan = ifsboot_plan,
	.bound = ifsboot_bound,
	.encoder = &boot_encoder,
	.section = ifsboot_section,
	.codecs = "raw,lzo1x,nrv2b",
	.carries = "ifs",
	.payload_checks = false,
	.zlib = false
};

static fsimg_status_t
boot_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "QNX boot image %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

static fsimg_status_t
boot_unsupported(fsimg_error_t *error, const char *what)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "QNX boot image %s", what);
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
	    message));
}

/* Whether the identity sits at offset prefix: the signature, header_size
 * 256 and preboot_size tying the header to that offset.
 */
static bool
header_at(const unsigned char *bytes, size_t length, uint32_t prefix)
{
	const unsigned char *header;

	if (length < (size_t)prefix + HEADER_BYTES)
		return (false);
	header = bytes + prefix;
	return (fsimg_layer_le32(header + HEADER_SIGNATURE) == STARTUP_SIGNATURE &&
	    fsimg_layer_le16(header + HEADER_HEADER_SIZE) == HEADER_BYTES &&
	    fsimg_layer_le16(header + HEADER_PREBOOT_SIZE) == prefix);
}

/*
 * The closed tables, each value outside them unsupported and named: the
 * version, the big-endian and reserved bits of flags1, the compression
 * value, and the machine with the prefix it was observed behind. The
 * virtual bit takes either value. A header within the tables gains what
 * they decide: its compression value, the chunk codec that value names
 * and whether its machine checks ram_size.
 */
static fsimg_status_t
check_tables(boot_header_t *header, fsimg_error_t *error)
{
	const struct boot_machine *row;
	char what[64];

	if (header->version != 1)
		return (boot_unsupported(error, "startup header version is not 1"));
	if ((header->flags1 & FLAGS1_BIG_ENDIAN) != 0)
		return (boot_unsupported(error,
		    "flags1 marks a big-endian header under a little-endian signature"));
	if ((header->flags1 & FLAGS1_RESERVED) != 0)
		return (boot_unsupported(error, "flags1 has reserved bits set"));
	header->compression = header->flags1 & FLAGS1_COMPRESSION;
	switch (header->compression) {
	case COMPRESSION_NONE:
		header->codec = 0;
		break;
	case COMPRESSION_LZO:
		header->codec = FSIMG_CODEC_LZO1X;
		break;
	case COMPRESSION_UCL:
		header->codec = FSIMG_CODEC_NRV2B;
		break;
	default:
		snprintf(what, sizeof(what),
		    "compression value 0x%02x is not supported%s",
		    (unsigned)header->compression,
		    header->compression == COMPRESSION_ZLIB ? " (zlib)" : "");
		return (boot_unsupported(error, what));
	}
	row = find_machine(header->machine);
	if (row == NULL) {
		snprintf(what, sizeof(what), "machine %u is not supported",
		    (unsigned)header->machine);
		return (boot_unsupported(error, what));
	}
	if (row->prefix != header->prefix) {
		snprintf(what, sizeof(what),
		    "machine %u is not supported behind this boot prefix",
		    (unsigned)header->machine);
		return (boot_unsupported(error, what));
	}
	header->ram_sum = row->ram_sum;
	return (FSIMG_OK);
}

/* The machine table's row for machine, NULL when the table lacks it. */
static const struct boot_machine *
find_machine(uint32_t machine)
{
	for (size_t i = 0; i < sizeof(boot_machines) / sizeof(boot_machines[0]);
	    i++) {
		if (boot_machines[i].machine == machine)
			return (&boot_machines[i]);
	}
	return (NULL);
}

/*
 * The bounds, each violation corrupt: a startup section of at least the
 * header and its checksum word, sizes in words, a stored image that ends
 * exactly at the end of the file, and a body that is the ImageFS when raw
 * or has room for the terminator, its padding and the trailer when framed,
 * so that every later range is computed without underflow.
 */
static fsimg_status_t
check_bounds(const boot_header_t *header, uint64_t file_bytes,
    fsimg_error_t *error)
{
	uint64_t startup = header->startup_size, stored = header->stored_size;

	if (startup < STARTUP_MIN_BYTES || startup % 4 != 0)
		return (boot_corrupt(error,
		    "startup size is below 260 or not a multiple of 4"));
	if (stored % 4 != 0 || stored < startup)
		return (boot_corrupt(error,
		    "stored size is below the startup size or not a multiple of 4"));
	if (header->prefix > file_bytes || stored != file_bytes - header->prefix)
		return (boot_corrupt(error,
		    "stored image does not end at the end of the file"));
	if (header->compression == COMPRESSION_NONE) {
		if (stored != startup + header->imagefs_size)
			return (boot_corrupt(error,
			    "stored size is not the startup size plus imagefs_size"));
	} else if (stored - startup < FRAMING_MIN_BYTES)
		return (boot_corrupt(error,
		    "stored size leaves no room for the terminator and the trailer"));
	return (FSIMG_OK);
}

/*
 * Find and validate the startup header in the file's prefix: the identity
 * at the first boot-prefix offset that ties it, then the closed tables and
 * the bounds. Without the identity it is no claim.
 */
static fsimg_status_t
decode_header(const unsigned char *prefix, size_t length, uint64_t file_bytes,
    boot_header_t *header, fsimg_error_t *error)
{
	const unsigned char *raw;
	size_t i;
	fsimg_status_t status;

	bzero(header, sizeof(*header));
	for (i = 0; i < sizeof(boot_prefixes) / sizeof(boot_prefixes[0]); i++) {
		if (header_at(prefix, length, boot_prefixes[i]))
			break;
	}
	if (i == sizeof(boot_prefixes) / sizeof(boot_prefixes[0]))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no QNX startup header at a boot-prefix offset"));
	header->prefix = boot_prefixes[i];
	raw = prefix + boot_prefixes[i];
	header->version = fsimg_layer_le16(raw + HEADER_VERSION);
	header->flags1 = raw[HEADER_FLAGS1];
	header->machine = fsimg_layer_le16(raw + HEADER_MACHINE);
	header->startup_vaddr = fsimg_layer_le32(raw + HEADER_STARTUP_VADDR);
	header->paddr_bias = fsimg_layer_le32(raw + HEADER_PADDR_BIAS);
	header->image_paddr = fsimg_layer_le32(raw + HEADER_IMAGE_PADDR);
	header->ram_paddr = fsimg_layer_le32(raw + HEADER_RAM_PADDR);
	header->ram_size = fsimg_layer_le32(raw + HEADER_RAM_SIZE);
	header->startup_size = fsimg_layer_le32(raw + HEADER_STARTUP_SIZE);
	header->stored_size = fsimg_layer_le32(raw + HEADER_STORED_SIZE);
	header->imagefs_paddr = fsimg_layer_le32(raw + HEADER_IMAGEFS_PADDR);
	header->imagefs_size = fsimg_layer_le32(raw + HEADER_IMAGEFS_SIZE);
	header->preboot_size = fsimg_layer_le16(raw + HEADER_PREBOOT_SIZE);
	header->flags2 = raw[HEADER_FLAGS2];
	for (i = HEADER_TAIL; i < HEADER_BYTES; i++)
		header->tail_nonzero += raw[i] != 0;

	status = check_tables(header, error);
	if (status != FSIMG_OK)
		return (status);
	return (check_bounds(header, file_bytes, error));
}

static fsimg_status_t
ifsboot_probe(const unsigned char *prefix, size_t prefix_length,
    const unsigned char *tail FSIMG_UNUSED, size_t tail_length FSIMG_UNUSED,
    uint64_t file_bytes, fsimg_error_t *error)
{
	boot_header_t header;

	return (decode_header(prefix, prefix_length, file_bytes, &header, error));
}

static fsimg_status_t
read_bytes(const fsimg_container_input_t *input, uint64_t offset,
    void *buffer, size_t length, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_read_session(input->file, input->session, offset,
	    buffer, length, &transferred, error));
}

/* Read the file's prefix again and validate the header in it. */
static fsimg_status_t
read_header(const fsimg_container_input_t *input, boot_header_t *header,
    fsimg_error_t *error)
{
	unsigned char prefix[PROBE_BYTES];
	size_t length;
	fsimg_status_t status;

	length = input->file_bytes < sizeof(prefix) ?
	    (size_t)input->file_bytes : sizeof(prefix);
	status = read_bytes(input, 0, prefix, length, error);
	if (status != FSIMG_OK)
		return (status);
	return (decode_header(prefix, length, input->file_bytes, header, error));
}

/*
 * Decode the chunk of stored_length bytes at file_offset into the decoded
 * buffer, producing at most bound bytes: the profile's 65536, or less when
 * the chunk would otherwise pass imagefs_size. fsimg_framed_decode(), which
 * the cache decodes through as well, requires the stream to end exactly at
 * its stored length within the bound, the decoder naming a stream that
 * would pass it. The stored bytes read and the bytes produced are charged
 * to the work counter, which also polls cancellation.
 */
static fsimg_status_t
decode_chunk(learning_t *learning, uint64_t file_offset, size_t stored_length,
    size_t bound, size_t *produced, fsimg_error_t *error)
{
	fsimg_status_t status;

	*produced = 0;
	status = read_bytes(learning->input, file_offset, learning->stored,
	    stored_length, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_framed_decode(
	    (fsimg_chunk_codec_t)learning->header->codec, learning->stored,
	    stored_length, file_offset, learning->decoded, bound, produced,
	    error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_session_charge(learning->input->session, 0, 0,
	    LENGTH_WORD_BYTES + stored_length + *produced, error));
}

/* Count a decoded chunk in the statistics info reports. */
static void
count_chunk(chunk_stats_t *stats, size_t stored_length, size_t produced)
{
	stats->chunks++;
	if (stats->chunks == 1 || produced < stats->min_decoded)
		stats->min_decoded = produced;
	if (produced > stats->max_decoded)
		stats->max_decoded = produced;
	if (stored_length > stats->max_stored)
		stats->max_stored = stored_length;
}

/* Append a decoded chunk's record once it is known to stand: as the final
 * chunk, or as a non-final one of at least the smallest observed size.
 */
static fsimg_status_t
keep_chunk(learning_t *learning, pending_chunk_t *pending,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (!pending->valid)
		return (FSIMG_OK);
	status = fsimg_chunk_index_append(learning->index, &pending->record,
	    error);
	if (status != FSIMG_OK)
		return (status);
	pending->valid = false;
	return (FSIMG_OK);
}

/* The bytes from the terminator's end to the trailer must be zero. */
static fsimg_status_t
check_padding(learning_t *learning, uint64_t offset, uint64_t trailer,
    fsimg_error_t *error)
{
	size_t piece;
	fsimg_status_t status;

	while (offset < trailer) {
		piece = trailer - offset < FSIMG_FRAMED_MAX_DECODED ?
		    (size_t)(trailer - offset) : (size_t)FSIMG_FRAMED_MAX_DECODED;
		status = read_bytes(learning->input, offset, learning->decoded,
		    piece, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < piece; i++) {
			if (learning->decoded[i] != 0)
				return (boot_corrupt(error,
				    "padding before the stored trailer is not zero"));
		}
		status = fsimg_session_charge(learning->input->session, 0, 0,
		    piece, error);
		if (status != FSIMG_OK)
			return (status);
		offset += piece;
	}
	return (FSIMG_OK);
}

/*
 * The learning pass over a framed body. Each length word and each chunk's
 * stored bytes lie before the trailer; each chunk decodes within its bound
 * and records its stored range and the decoded range it learned. A chunk's
 * record is appended when the next length word arrives: a nonzero word
 * makes it non-final, which requires 4096 decoded bytes, and the zero
 * terminator makes it final. Once the decoded total reaches imagefs_size,
 * any further chunk is corrupt, so at most imagefs_size / 4096 + 1 records
 * are ever appended. The total must then equal imagefs_size, and the
 * padding must be zero.
 */
static fsimg_status_t
learn_chunks(learning_t *learning, fsimg_error_t *error)
{
	const boot_header_t *header = learning->header;
	unsigned char word[LENGTH_WORD_BYTES];
	pending_chunk_t pending;
	uint64_t offset, trailer, disk_offset, left;
	size_t stored_length, bound, produced;
	fsimg_status_t status;

	/* The bounds proved the body holds at least the framing minimum. */
	offset = header->prefix + header->startup_size;
	trailer = header->prefix + header->stored_size - TRAILER_BYTES;
	disk_offset = 0;
	bzero(&pending, sizeof(pending));
	for (;;) {
		if (trailer - offset < LENGTH_WORD_BYTES)
			return (boot_corrupt(error,
			    "length word reaches the stored trailer"));
		status = read_bytes(learning->input, offset, word, sizeof(word),
		    error);
		if (status != FSIMG_OK)
			return (status);
		offset += LENGTH_WORD_BYTES;
		stored_length = (size_t)word[0] << 8 | word[1];
		if (stored_length == 0)
			break;

		/* The chunk before this one was not the final one. */
		if (pending.valid &&
		    pending.record.length < NONFINAL_MIN_DECODED)
			return (boot_unsupported(error,
			    "non-final chunk decodes to fewer than 4096 bytes"));
		status = keep_chunk(learning, &pending, error);
		if (status != FSIMG_OK)
			return (status);

		left = header->imagefs_size - disk_offset;
		if (left == 0)
			return (boot_corrupt(error,
			    "chunks continue past imagefs_size"));
		if (stored_length > trailer - offset)
			return (boot_corrupt(error, "chunk reaches the stored trailer"));
		bound = left < FSIMG_FRAMED_MAX_DECODED ? (size_t)left :
		    (size_t)FSIMG_FRAMED_MAX_DECODED;
		status = decode_chunk(learning, offset, stored_length, bound,
		    &produced, error);
		if (status != FSIMG_OK)
			return (status);

		pending.record = (fsimg_chunk_t){
			.disk_offset = disk_offset,
			.length = produced,
			.file_offset = offset,
			.encoded_length = stored_length,
			.kind = FSIMG_CHUNK_COMPRESSED,
			.argument = header->codec
		};
		pending.valid = true;
		count_chunk(&learning->stats, stored_length, produced);
		offset += stored_length;
		disk_offset += produced;
	}
	if (disk_offset != header->imagefs_size)
		return (boot_corrupt(error,
		    "chunks decode to fewer bytes than imagefs_size"));
	status = keep_chunk(learning, &pending, error);
	if (status != FSIMG_OK)
		return (status);
	return (check_padding(learning, offset, trailer, error));
}

/*
 * A framed body's disk: the learned index, sealed, behind the adapter. The
 * learning pass's two buffers live only for the pass; the index is freed
 * on any failure until the adapter adopts it.
 */
static fsimg_status_t
open_framed(const fsimg_container_input_t *input, const boot_header_t *header,
    fsimg_container_t *out, chunk_stats_t *stats, fsimg_error_t *error)
{
	fsimg_chunk_summary_t summary;
	learning_t learning;
	uint64_t body;
	fsimg_status_t status;

	bzero(&learning, sizeof(learning));
	learning.input = input;
	learning.header = header;
	body = header->prefix + header->startup_size;
	status = fsimg_chunk_index_new(input->session, "QNX boot image",
	    "ifsboot", header->imagefs_size, body,
	    header->stored_size - header->startup_size - TRAILER_BYTES,
	    &learning.index, error);
	if (status != FSIMG_OK)
		return (status);
	learning.stored = fsimg_alloc(input->ctx, FSIMG_FRAMED_MAX_ENCODED,
	    error);
	if (learning.stored == NULL)
		status = error->status;
	if (status == FSIMG_OK) {
		learning.decoded = fsimg_alloc(input->ctx,
		    FSIMG_FRAMED_MAX_DECODED, error);
		if (learning.decoded == NULL)
			status = error->status;
	}
	if (status == FSIMG_OK)
		status = learn_chunks(&learning, error);
	fsimg_dealloc(learning.stored);
	fsimg_dealloc(learning.decoded);
	if (status == FSIMG_OK)
		status = fsimg_chunk_index_seal(learning.index, error);
	if (status != FSIMG_OK) {
		fsimg_chunk_index_free(learning.index);
		return (status);
	}
	fsimg_chunk_index_summary(learning.index, &summary);
	*stats = learning.stats;
	out->chunks = summary.count;
	out->kinds = summary.kinds;
	out->codecs = summary.codecs;
	/* The adapter adopts the index on every outcome. */
	return (fsimg_chunk_disk_open(input->ctx, input->file, input->session,
	    learning.index, &out->disk, error));
}

/*
 * Info's details, in the order specs/design-layers.md documents: the
 * prefix length, the decoded header fields but the opaque ones, the body's
 * stored length and the chunk statistics. The compression value's name is
 * the image's codecs text and the chunk count its chunk_count.
 */
static void
report_details(const boot_header_t *header, const chunk_stats_t *stats,
    fsimg_container_t *out)
{
	const struct {
		const char *name;
		uint64_t value;
	} details[] = {
		{ "prefix_bytes", header->prefix },
		{ "version", header->version },
		{ "virtual", (header->flags1 & FLAGS1_VIRTUAL) != 0 },
		{ "compression", header->compression },
		{ "machine", header->machine },
		{ "startup_vaddr", header->startup_vaddr },
		{ "paddr_bias", header->paddr_bias },
		{ "image_paddr", header->image_paddr },
		{ "ram_paddr", header->ram_paddr },
		{ "ram_size", header->ram_size },
		{ "startup_size", header->startup_size },
		{ "stored_size", header->stored_size },
		{ "imagefs_paddr", header->imagefs_paddr },
		{ "imagefs_size", header->imagefs_size },
		{ "preboot_size", header->preboot_size },
		{ "flags2", header->flags2 },
		{ "tail_nonzero", header->tail_nonzero },
		{ "body_bytes", header->stored_size - header->startup_size },
		{ "min_chunk_bytes", stats->min_decoded },
		{ "max_chunk_bytes", stats->max_decoded },
		{ "max_stored_chunk_bytes", stats->max_stored }
	};

	_Static_assert(sizeof(details) / sizeof(details[0]) <=
	    FSIMG_IMAGE_DETAILS_MAX, "the details fit the image record");
	for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); i++) {
		snprintf(out->details[i].name, sizeof(out->details[i].name), "%s",
		    details[i].name);
		out->details[i].value = details[i].value;
	}
	out->detail_count = sizeof(details) / sizeof(details[0]);
}

/* A raw body is a slice of the file, one raw chunk; a framed body is
 * learned. Either way the disk is imagefs_size bytes.
 */
static fsimg_status_t
ifsboot_open(const fsimg_container_input_t *input, fsimg_container_t *out,
    fsimg_error_t *error)
{
	boot_header_t header;
	chunk_stats_t stats;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	status = read_header(input, &header, error);
	if (status != FSIMG_OK)
		return (status);
	if (header.compression == COMPRESSION_NONE) {
		status = fsimg_io_read_slice(input->file,
		    header.prefix + header.startup_size, header.imagefs_size,
		    &out->disk, error);
		stats = (chunk_stats_t){ .chunks = 1,
		    .min_decoded = header.imagefs_size,
		    .max_decoded = header.imagefs_size,
		    .max_stored = header.imagefs_size };
		out->chunks = 1;
		out->kinds = 1u << FSIMG_CHUNK_RAW;
	} else
		status = open_framed(input, &header, out, &stats, error);
	if (status != FSIMG_OK)
		return (status);
	out->disk_bytes = header.imagefs_size;
	report_details(&header, &stats, out);
	return (FSIMG_OK);
}

/*
 * The two sums in one sequential pass over the stored image: the
 * little-endian words of the startup section, and of the whole stored
 * image, each sum to zero modulo 2^32. Then ram_size where the machine
 * table checks it. The header is read and validated again first.
 */
static fsimg_status_t
ifsboot_verify(const fsimg_container_input_t *input,
    const fsimg_container_t *container FSIMG_UNUSED,
    bool payload FSIMG_UNUSED, fsimg_error_t *error)
{
	boot_header_t header;
	unsigned char *piece;
	uint64_t offset, end, startup_end;
	uint32_t startup_sum, stored_sum, word;
	size_t length;
	fsimg_status_t status;

	status = read_header(input, &header, error);
	if (status != FSIMG_OK)
		return (status);
	piece = fsimg_alloc(input->ctx, SUM_PIECE_BYTES, error);
	if (piece == NULL)
		return (error->status);
	startup_sum = 0;
	stored_sum = 0;
	startup_end = header.prefix + header.startup_size;
	end = header.prefix + header.stored_size;
	status = FSIMG_OK;
	/* Both sizes are multiples of 4 from B, so pieces hold whole words. */
	for (offset = header.prefix; offset < end; offset += length) {
		length = end - offset < SUM_PIECE_BYTES ? (size_t)(end - offset) :
		    SUM_PIECE_BYTES;
		status = read_bytes(input, offset, piece, length, error);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(input->session, 0, 0, length,
			    error);
		if (status != FSIMG_OK)
			break;
		for (size_t i = 0; i < length; i += 4) {
			word = fsimg_layer_le32(piece + i);
			stored_sum += word;
			if (offset + i < startup_end)
				startup_sum += word;
		}
	}
	fsimg_dealloc(piece);
	if (status != FSIMG_OK)
		return (status);
	if (startup_sum != 0)
		return (boot_corrupt(error,
		    "startup checksum: the startup section does not sum to zero"));
	if (stored_sum != 0)
		return (boot_corrupt(error,
		    "stored checksum: the stored image does not sum to zero"));
	if (header.ram_sum &&
	    (uint64_t)header.ram_size !=
	    (uint64_t)header.startup_size + header.imagefs_size)
		return (boot_corrupt(error,
		    "ram_size is not startup_size plus imagefs_size"));
	return (FSIMG_OK);
}

/*
 * Unwrap's sections: the prefix [0, B), empty without one, and the startup
 * code [B + 256, B + startup_size - 4), between the header and the startup
 * checksum word. The header is read and validated again, so both ranges
 * lie inside the file.
 */
static fsimg_status_t
ifsboot_section(const fsimg_container_input_t *input,
    fsimg_file_setting_t setting, uint64_t *offset, uint64_t *length,
    fsimg_error_t *error)
{
	boot_header_t header;
	fsimg_status_t status;

	status = read_header(input, &header, error);
	if (status != FSIMG_OK)
		return (status);
	switch (setting) {
	case FSIMG_FILE_IFSBOOT_PREFIX:
		*offset = 0;
		*length = header.prefix;
		return (FSIMG_OK);
	case FSIMG_FILE_IFSBOOT_STARTUP:
		*offset = header.prefix + HEADER_BYTES;
		*length = header.startup_size - STARTUP_MIN_BYTES;
		return (FSIMG_OK);
	}
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    "QNX boot image has no such section"));
}

/* A length padded with zeros to a whole number of words. */
static uint64_t
word_padded(uint64_t length)
{
	return ((length + 3) & ~(uint64_t)3);
}

/* The startup section's length for code of code_bytes bytes: the header,
 * the padded code and the startup checksum word.
 */
static uint64_t
startup_bytes(uint64_t code_bytes)
{
	return (STARTUP_MIN_BYTES + word_padded(code_bytes));
}

/*
 * Read length bytes of a file setting from offset into buffer, across as
 * many transfers as the object needs; *got falls short of length only at
 * the object's end.
 */
static fsimg_status_t
read_setting(fsimg_io_t *io, uint64_t offset, unsigned char *buffer,
    size_t length, size_t *got, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	for (*got = 0; *got < length; *got += transferred) {
		status = fsimg_io_read_at(io, offset + *got, buffer + *got,
		    length - *got, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (transferred == 0)
			break;
	}
	return (FSIMG_OK);
}

/* A file setting that ended before the length the plan recorded. */
static fsimg_status_t
setting_short(fsimg_error_t *error, const char *key)
{
	fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0, FSIMG_OP_SOURCE,
	    "file setting ended before its planned length");
	fsimg_error_path(error, key);
	return (FSIMG_ESOURCE_CHANGED);
}

/*
 * Read the boot prefix whole into the settings' buffer: B bytes, the
 * file's bytes followed by its padding, which is zeroed here rather than
 * left to the allocator. The bytes must not begin with the startup
 * signature, so that the reader, which takes the first tie in prefix-table
 * order, can claim the written header only at B. On failure nothing is
 * left allocated.
 */
static fsimg_status_t
buffer_prefix(const fsimg_container_plan_args_t *args,
    const fsimg_planned_file_t *prefix, uint64_t padded,
    fsimg_container_settings_t *settings, fsimg_error_t *error)
{
	unsigned char *buffer;
	size_t length, got;
	fsimg_status_t status;

	length = (size_t)prefix->length;
	buffer = fsimg_alloc(args->ctx, (size_t)padded, error);
	if (buffer == NULL)
		return (error->status);
	status = read_setting(prefix->io, 0, buffer, length, &got, error);
	if (status != FSIMG_OK)
		fsimg_error_path(error, "ifsboot:prefix");
	else if (got != length)
		status = setting_short(error, "ifsboot:prefix");
	else if (fsimg_layer_le32(buffer) == STARTUP_SIGNATURE)
		status = fsimg_setting_error(error, "ifsboot:prefix",
		    "the boot prefix begins with the QNX startup signature");
	if (status != FSIMG_OK) {
		fsimg_dealloc(buffer);
		return (status);
	}
	bzero(buffer + length, (size_t)padded - length);
	settings->buffered = buffer;
	settings->buffered_bytes = (size_t)padded;
	return (FSIMG_OK);
}

/*
 * The plan: every argument, before any sink opens. The required settings,
 * the machine and the mode against the machine table's writable modes,
 * the body's byte order, the startup code's length, the prefix's padded
 * length against the prefix table, then the header's table fields
 * through the reader's closed-table check, which pairs each machine with
 * its prefix; last the prefix's bytes, read whole. Under lzo the
 * transcode makes two passes, the first learning the framed length; the
 * chunk is the framing's 65536 decoded bytes.
 */
static fsimg_status_t
ifsboot_plan(const fsimg_container_plan_args_t *args,
    fsimg_container_settings_t *settings, fsimg_error_t *error)
{
	const fsimg_layer_options_t *layers = args->layers;
	const fsimg_planned_file_t *startup, *prefix;
	const struct boot_machine *row;
	const struct {
		const char *key;
		bool set;
	} required[] = {
		{ "ifsboot:machine", layers->ifsboot_machine_set },
		{ "ifsboot:virtual", layers->ifsboot_virtual_set },
		{ "ifsboot:startup_vaddr", layers->ifsboot_startup_vaddr_set },
		{ "ifsboot:paddr_bias", layers->ifsboot_paddr_bias_set },
		{ "ifsboot:image_paddr", layers->ifsboot_image_paddr_set },
		{ "ifsboot:ram_paddr", layers->ifsboot_ram_paddr_set },
		{ "ifsboot:imagefs_paddr", layers->ifsboot_imagefs_paddr_set }
	};
	boot_header_t header;
	char what[sizeof(error->message)];
	uint64_t code, padded;
	unsigned mode;
	bool lzo;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		if (!required[i].set)
			return (fsimg_setting_error(error, required[i].key,
			    "the setting is required"));
	}
	if (layers->ifsboot_compress != FSIMG_IFSBOOT_LZO &&
	    layers->ifsboot_compress != FSIMG_IFSBOOT_NONE)
		return (fsimg_setting_error(error, "ifsboot:compress",
		    "the compression must be lzo or none"));
	lzo = layers->ifsboot_compress == FSIMG_IFSBOOT_LZO;
	row = find_machine(layers->ifsboot_machine);
	if (row == NULL)
		return (fsimg_setting_error(error, "ifsboot:machine",
		    "the machine is not in the QNX boot image machine table"));
	mode = lzo ? WRITES_LZO : WRITES_NONE;
	if ((row->writes & mode) == 0 && !args->ctx->unproven_writes) {
		snprintf(what, sizeof(what),
		    "machine %u is not writable under compress=%s",
		    (unsigned)row->machine, lzo ? "lzo" : "none");
		boot_unsupported(error, what);
		fsimg_error_path(error, "ifsboot:machine");
		return (FSIMG_EUNSUPPORTED);
	}
	if (args->volume_order != FSIMG_BYTE_ORDER_LITTLE)
		return (boot_unsupported(error,
		    "holds only a little-endian ImageFS, not a big-endian one"));

	startup = &args->files[FSIMG_FILE_IFSBOOT_STARTUP];
	code = startup->io == NULL ? 0 : startup->length;
	/* Checked before rounding, so the padding cannot wrap. */
	if (code > UINT32_MAX - STARTUP_MIN_BYTES ||
	    startup_bytes(code) > UINT32_MAX)
		return (fsimg_setting_error(error, "ifsboot:startup",
		    "the startup code does not fit the 32-bit startup_size"));
	/* An absent or empty prefix is none; any other must pad to 1088. */
	prefix = &args->files[FSIMG_FILE_IFSBOOT_PREFIX];
	padded = 0;
	if (prefix->io != NULL && prefix->length != 0) {
		if (prefix->length > BIOS_PREFIX_BYTES ||
		    word_padded(prefix->length) != BIOS_PREFIX_BYTES)
			return (fsimg_setting_error(error, "ifsboot:prefix",
			    "the padded boot prefix is not a prefix-table "
			    "length, 0 or 1088 bytes"));
		padded = BIOS_PREFIX_BYTES;
	}

	bzero(&header, sizeof(header));
	header.prefix = padded;
	header.version = 1;
	header.machine = row->machine;
	header.flags1 = (layers->ifsboot_virtual ? FLAGS1_VIRTUAL : 0) |
	    (lzo ? COMPRESSION_LZO : COMPRESSION_NONE);
	status = check_tables(&header, error);
	if (status != FSIMG_OK) {
		snprintf(what, sizeof(what), "%s", error->message);
		return (fsimg_setting_error(error, "ifsboot:machine", what));
	}

	settings->boot = (fsimg_boot_settings_t){ .code_bytes = code,
	    .startup_vaddr = layers->ifsboot_startup_vaddr,
	    .paddr_bias = layers->ifsboot_paddr_bias,
	    .image_paddr = layers->ifsboot_image_paddr,
	    .ram_paddr = layers->ifsboot_ram_paddr,
	    .imagefs_paddr = layers->ifsboot_imagefs_paddr,
	    .machine = row->machine, .flags1 = header.flags1,
	    .ram_sum = row->ram_sum };
	settings->granularity = 1;
	settings->chunk_bytes = (uint32_t)FSIMG_FRAMED_MAX_DECODED;
	settings->passes = lzo ? 2 : 1;
	if (padded == 0)
		return (FSIMG_OK);
	return (buffer_prefix(args, prefix, padded, settings, error));
}

/* A disk or a derived size the header's 32-bit fields cannot describe. */
static fsimg_status_t
field_overflow(fsimg_error_t *error, const char *field, const char *what)
{
	boot_unsupported(error, what);
	fsimg_error_path(error, field);
	return (FSIMG_EUNSUPPORTED);
}

/*
 * The most the output can take: the prefix, the startup section and the
 * body's bound, the disk under none, and under lzo the sum over chunks of
 * each chunk's worst-case stored length, its length word included (at
 * most n + n / 255 + 8 for n decoded bytes unsplit, n + n / 255 + 17
 * split), the terminator, the padding and the trailer. The disk is at most
 * 2^32 - 4 bytes, a multiple of 4 under none, and ram_size and stored_size
 * at the bound must fit 32 bits.
 */
static fsimg_status_t
ifsboot_bound(const fsimg_container_settings_t *settings, uint64_t disk_bytes,
    uint64_t *out, fsimg_error_t *error)
{
	uint64_t startup, body;
	bool lzo;

	lzo = (settings->boot.flags1 & FLAGS1_COMPRESSION) == COMPRESSION_LZO;
	if (disk_bytes > UINT32_MAX - 3 || (!lzo && disk_bytes % 4 != 0))
		return (field_overflow(error, "imagefs_size",
		    "body must be at most 2^32 - 4 bytes, a multiple of 4 when "
		    "stored raw"));
	startup = startup_bytes(settings->boot.code_bytes);
	if (startup + disk_bytes > UINT32_MAX)
		return (field_overflow(error, "ram_size",
		    "ram_size would pass 32 bits"));
	body = !lzo ? disk_bytes : disk_bytes + disk_bytes / 255 +
	    20 * (disk_bytes / FSIMG_FRAMED_MAX_DECODED + 1) + 12;
	if (startup + body > UINT32_MAX)
		return (field_overflow(error, "stored_size",
		    "stored_size would pass 32 bits"));
	*out = settings->buffered_bytes + startup + body;
	return (FSIMG_OK);
}

static void
sum_bytes(word_sum_t *sum, const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++, sum->position++)
		sum->sum += (uint32_t)bytes[i] << (8 * (sum->position & 3));
}

/* The whole header, from the settings and the derived sizes. */
static void
put_header(unsigned char *raw, const fsimg_container_settings_t *settings,
    uint32_t startup_size, uint32_t stored_size, uint32_t imagefs_size)
{
	const fsimg_boot_settings_t *boot = &settings->boot;

	bzero(raw, HEADER_BYTES);
	fsimg_layer_put_le32(raw + HEADER_SIGNATURE, STARTUP_SIGNATURE);
	fsimg_layer_put_le16(raw + HEADER_VERSION, 1);
	raw[HEADER_FLAGS1] = boot->flags1;
	fsimg_layer_put_le16(raw + HEADER_HEADER_SIZE, HEADER_BYTES);
	fsimg_layer_put_le16(raw + HEADER_MACHINE, boot->machine);
	fsimg_layer_put_le32(raw + HEADER_STARTUP_VADDR, boot->startup_vaddr);
	fsimg_layer_put_le32(raw + HEADER_PADDR_BIAS, boot->paddr_bias);
	fsimg_layer_put_le32(raw + HEADER_IMAGE_PADDR, boot->image_paddr);
	fsimg_layer_put_le32(raw + HEADER_RAM_PADDR, boot->ram_paddr);
	fsimg_layer_put_le32(raw + HEADER_RAM_SIZE,
	    boot->ram_sum ? startup_size + imagefs_size : 0);
	fsimg_layer_put_le32(raw + HEADER_STARTUP_SIZE, startup_size);
	fsimg_layer_put_le32(raw + HEADER_STORED_SIZE, stored_size);
	fsimg_layer_put_le32(raw + HEADER_IMAGEFS_PADDR, boot->imagefs_paddr);
	fsimg_layer_put_le32(raw + HEADER_IMAGEFS_SIZE, imagefs_size);
	fsimg_layer_put_le16(raw + HEADER_PREBOOT_SIZE,
	    (uint16_t)settings->buffered_bytes);
}

/*
 * The test control's fault in the copy check_emission decodes: the header
 * also planted at offset 0, with no prefix and a stored size reaching the
 * same end, so that the decode ties at 0, as a prefix beginning with the
 * signature would if the plan did not refuse it.
 */
static void
plant_misplaced_header(unsigned char *decoded, const unsigned char *lead,
    size_t prefix)
{
	bcopy(lead + prefix, decoded, HEADER_BYTES);
	fsimg_layer_put_le16(decoded + HEADER_PREBOOT_SIZE, 0);
	fsimg_layer_put_le32(decoded + HEADER_STORED_SIZE,
	    fsimg_layer_le32(lead + prefix + HEADER_STORED_SIZE) +
	    (uint32_t)prefix);
}

/*
 * The emission invariant: the prefix and header about to be written, B +
 * 256 bytes, decode with the reader's decode_header against the real
 * output length and tie at B. The plan checked every input to them, so a
 * failure is a defect, FSIMG_ESTATE.
 */
static fsimg_status_t
check_emission(const boot_encoder_t *encoder, const unsigned char *lead,
    size_t length, uint64_t output_bytes, fsimg_error_t *error)
{
	unsigned char decoded[PROBE_BYTES];
	size_t prefix = encoder->settings->buffered_bytes;
	boot_header_t header;
	fsimg_status_t status;

	bcopy(lead, decoded, length);
	if (encoder->misplace && prefix >= HEADER_BYTES)
		plant_misplaced_header(decoded, lead, prefix);
	status = decode_header(decoded, length, output_bytes, &header, error);
	if (status == FSIMG_OK && header.prefix == prefix)
		return (FSIMG_OK);
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
	    "QNX boot image header the writer built does not decode at its "
	    "prefix"));
}

/*
 * The startup code and its padding, copied in bounded pieces and summed
 * into section as they pass. A copy that ends short of the length the
 * plan recorded fails before writing any padding.
 */
static fsimg_status_t
copy_code(boot_encoder_t *encoder, word_sum_t *section, fsimg_error_t *error)
{
	uint64_t code = encoder->settings->boot.code_bytes;
	size_t piece, got;
	fsimg_status_t status;

	for (uint64_t offset = 0; offset < code; offset += piece) {
		piece = code - offset < COPY_PIECE_BYTES ?
		    (size_t)(code - offset) : COPY_PIECE_BYTES;
		status = read_setting(encoder->startup, offset, encoder->copy,
		    piece, &got, error);
		if (status != FSIMG_OK) {
			fsimg_error_path(error, "ifsboot:startup");
			return (status);
		}
		if (got != piece)
			return (setting_short(error, "ifsboot:startup"));
		status = fsimg_session_charge(encoder->session, 0, 0, piece,
		    error);
		if (status == FSIMG_OK)
			status = fsimg_seq_write(encoder->sink, encoder->copy, piece,
			    error);
		if (status != FSIMG_OK)
			return (status);
		sum_bytes(section, encoder->copy, piece);
	}
	section->position += word_padded(code) - code;
	return (fsimg_seq_zero(encoder->sink, word_padded(code) - code, error));
}

/*
 * The leading bytes, once stored_size is known: the buffered prefix, the
 * header, checked by the emission invariant first, then the code and the
 * startup checksum word, which makes the section's words sum to zero.
 */
static fsimg_status_t
emit_leading(boot_encoder_t *encoder, uint64_t body_bytes,
    fsimg_error_t *error)
{
	const fsimg_container_settings_t *settings = encoder->settings;
	unsigned char lead[PROBE_BYTES], checksum[4];
	size_t prefix = settings->buffered_bytes;
	uint64_t startup, stored;
	word_sum_t section;
	fsimg_status_t status;

	startup = startup_bytes(settings->boot.code_bytes);
	stored = startup + body_bytes;
	/* The bound kept every size within 32 bits at the worst case. */
	if (stored > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "QNX boot image body exceeds its planned bound"));
	if (prefix != 0)
		bcopy(settings->buffered, lead, prefix);
	put_header(lead + prefix, settings, (uint32_t)startup, (uint32_t)stored,
	    (uint32_t)encoder->disk_bytes);
	status = check_emission(encoder, lead, prefix + HEADER_BYTES,
	    prefix + stored, error);
	if (status == FSIMG_OK)
		status = fsimg_seq_write(encoder->sink, lead, prefix + HEADER_BYTES,
		    error);
	if (status != FSIMG_OK)
		return (status);
	section = (word_sum_t){ 0 };
	sum_bytes(&section, lead + prefix, HEADER_BYTES);
	status = copy_code(encoder, &section, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_layer_put_le32(checksum, 0u - section.sum);
	return (fsimg_seq_write(encoder->sink, checksum, sizeof(checksum),
	    error));
}

/* The framed body's length after the chunks: the zero terminator, its
 * padding to a word and the stored trailer.
 */
static uint64_t
framed_body(uint64_t chunks)
{
	return (word_padded(chunks + LENGTH_WORD_BYTES) + TRAILER_BYTES);
}

static fsimg_status_t
boot_changed(fsimg_error_t *error, const char *what)
{
	fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0, FSIMG_OP_SOURCE, what);
	fsimg_error_path(error, "input");
	return (FSIMG_ESOURCE_CHANGED);
}

/* Write bytes into the stored body, summing its words as they pass. */
static fsimg_status_t
write_body(boot_encoder_t *encoder, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	sum_bytes(&encoder->body, bytes, length);
	return (fsimg_seq_write(encoder->sink, bytes, length, error));
}

/*
 * One chunk of n decoded bytes under lzo: counted in the first pass,
 * written as its big-endian length word and stream in the second. A
 * stream longer than a length word holds is emitted instead as two chunks
 * of ceil(n / 2) and floor(n / 2) bytes; the encoder's bound keeps each
 * half within 65535, so the recursion is one level deep. The second pass
 * may never pass the length the first learned, which stored_size states.
 */
static fsimg_status_t
put_chunk(boot_encoder_t *encoder, unsigned pass, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	unsigned char word[LENGTH_WORD_BYTES];
	size_t stream, half;
	fsimg_status_t status;

	status = fsimg_lzo1x_encode(encoder->lzo, bytes, length,
	    encoder->stored, encoder->stored_capacity, &stream, error);
	if (status == FSIMG_OK)
		status = fsimg_session_charge(encoder->session, 0, 0,
		    length + stream, error);
	if (status != FSIMG_OK)
		return (status);
	if (stream > FSIMG_FRAMED_MAX_ENCODED) {
		half = length - length / 2;
		status = put_chunk(encoder, pass, bytes, half, error);
		if (status != FSIMG_OK)
			return (status);
		return (put_chunk(encoder, pass, bytes + half, length - half,
		    error));
	}
	encoder->framed[pass] += LENGTH_WORD_BYTES + stream;
	if (pass == 0)
		return (FSIMG_OK);
	if (encoder->framed[1] > encoder->framed[0])
		return (boot_changed(error,
		    "input changed between the two passes: the second encodes "
		    "longer"));
	word[0] = (unsigned char)(stream >> 8);
	word[1] = (unsigned char)stream;
	status = write_body(encoder, word, sizeof(word), error);
	if (status == FSIMG_OK)
		status = write_body(encoder, encoder->stored, stream, error);
	return (status);
}

static fsimg_status_t
boot_encode_open(const fsimg_encode_t *encode, void **state,
    fsimg_error_t *error)
{
	boot_encoder_t *encoder;
	fsimg_status_t status;

	encoder = fsimg_alloc(encode->ctx, sizeof(*encoder), error);
	if (encoder == NULL)
		return (error->status);
	*encoder = (boot_encoder_t){ .session = encode->session,
	    .sink = encode->sink, .settings = encode->settings,
	    .startup = encode->files[FSIMG_FILE_IFSBOOT_STARTUP].io,
	    .disk_bytes = encode->disk_bytes,
	    .compressed = (encode->settings->boot.flags1 &
	    FLAGS1_COMPRESSION) == COMPRESSION_LZO,
	    .verified_disk = encode->verified_disk,
	    .misplace = encode->ctx->misplace_boot_header };
	*state = encoder;
	if (encoder->settings->boot.code_bytes != 0) {
		encoder->copy = fsimg_alloc(encode->ctx, COPY_PIECE_BYTES, error);
		if (encoder->copy == NULL)
			return (error->status);
	}
	if (!encoder->compressed)
		return (emit_leading(encoder, encoder->disk_bytes, error));
	status = fsimg_lzo1x_encoder_new(encode->ctx, &encoder->lzo, error);
	if (status != FSIMG_OK)
		return (status);
	encoder->zeros = fsimg_alloc(encode->ctx,
	    (size_t)FSIMG_FRAMED_MAX_DECODED, error);
	if (encoder->zeros == NULL)
		return (error->status);
	bzero(encoder->zeros, (size_t)FSIMG_FRAMED_MAX_DECODED);
	encoder->stored_capacity = fsimg_lzo1x_encode_bound(
	    (size_t)FSIMG_FRAMED_MAX_DECODED);
	encoder->stored = fsimg_alloc(encode->ctx, encoder->stored_capacity,
	    error);
	if (encoder->stored == NULL)
		return (error->status);
	return (FSIMG_OK);
}

/*
 * One unit of the disk. Its bytes join the plain sum in every pass; under
 * none they are the body, a unit of zeros a run of zeros, and under lzo
 * the unit is one chunk, a NULL one encoded from the zero unit.
 */
static fsimg_status_t
boot_encode_chunk(void *state, unsigned pass, const unsigned char *bytes,
    size_t length, fsimg_error_t *error)
{
	boot_encoder_t *encoder = state;

	if (bytes != NULL)
		sum_bytes(&encoder->plain, bytes, length);
	else
		encoder->plain.position += length;
	if (encoder->compressed)
		return (put_chunk(encoder, pass,
		    bytes != NULL ? bytes : encoder->zeros, length, error));
	if (bytes == NULL)
		return (fsimg_seq_zero(encoder->sink, length, error));
	return (fsimg_seq_write(encoder->sink, bytes, length, error));
}

/*
 * The end of a pass. The disk's words must sum to zero, the ImageFS's own
 * checksum property: a create's staged volume that fails it is corrupt, a
 * wrap's verified input that fails it changed. Under lzo the first pass's
 * end writes the leading bytes, stored_size now known, and the second's
 * checks its framed length against the first's and closes the body: the
 * terminator, the padding and the trailer that makes the stored image sum
 * to zero, which is the negated sum of the body, since the startup
 * section sums to zero on its own.
 */
static fsimg_status_t
boot_encode_finish(void *state, unsigned pass, fsimg_error_t *error)
{
	boot_encoder_t *encoder = state;
	unsigned char trailer[TRAILER_BYTES];
	uint64_t zeros;
	fsimg_status_t status;

	if (encoder->plain.sum != 0) {
		if (encoder->verified_disk)
			return (boot_changed(error,
			    "input changed after verification: its words no longer "
			    "sum to zero"));
		fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
		    "QNX boot image body: the ImageFS's words do not sum to zero");
		fsimg_error_path(error, "body");
		return (FSIMG_ECORRUPT);
	}
	encoder->plain = (word_sum_t){ 0 };
	if (!encoder->compressed)
		return (FSIMG_OK);
	if (pass == 0)
		return (emit_leading(encoder, framed_body(encoder->framed[0]),
		    error));
	if (encoder->framed[1] != encoder->framed[0])
		return (boot_changed(error,
		    "input changed between the two passes: the second encodes "
		    "shorter"));
	/* The terminator and its padding are zeros, which add nothing. */
	zeros = word_padded(encoder->framed[1] + LENGTH_WORD_BYTES) -
	    encoder->framed[1];
	encoder->body.position += zeros;
	status = fsimg_seq_zero(encoder->sink, zeros, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_layer_put_le32(trailer, 0u - encoder->body.sum);
	return (fsimg_seq_write(encoder->sink, trailer, sizeof(trailer), error));
}

static void
boot_encode_close(void *state)
{
	boot_encoder_t *encoder = state;

	fsimg_lzo1x_encoder_free(encoder->lzo);
	fsimg_dealloc(encoder->zeros);
	fsimg_dealloc(encoder->stored);
	fsimg_dealloc(encoder->copy);
	fsimg_dealloc(encoder);
}
