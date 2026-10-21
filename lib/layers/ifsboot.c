/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "layer.h"

/*
 * QNX boot images (specs/design-layers.md#qnx-boot-image-ifsboot), read
 * only. A boot image is an optional boot prefix, the startup section and
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

/* The offsets of the startup header's fields, from B, named as in QNX's
 * struct startup_header; HEADER_HEADER_SIZE is the offset of header_size.
 */
enum {
	HEADER_SIGNATURE = 0x00,
	HEADER_VERSION = 0x04,
	HEADER_FLAGS1 = 0x06,
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

/* The closed machine table: each machine, the prefix it was observed
 * behind, and whether ram_size is checked against startup_size +
 * imagefs_size. Machine 0 is nonbootable mkifs output, whose ram_size no
 * boot image constrains.
 */
static const struct boot_machine {
	uint16_t	 machine;
	uint32_t	 prefix;
	bool		 ram_checked;
} boot_machines[] = {
	{ 42, 0, true },			/* SH-4 (S1) */
	{ 3, BIOS_PREFIX_BYTES, true },		/* x86 (S2) */
	{ 0, 0, false }				/* nonbootable (E7) */
};

/* A validated startup header. prefix is B, the header's file offset; the
 * sizes count from it. codec is the chunk codec of a framed body, zero for
 * a raw one.
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
	uint8_t		 compression;
	uint32_t	 codec;
	bool		 ram_checked;
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

static fsimg_status_t boot_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t boot_unsupported(fsimg_error_t *, const char *);
static bool header_at(const unsigned char *, size_t, uint32_t);
static fsimg_status_t decode_header(const unsigned char *, size_t, uint64_t,
    boot_header_t *, fsimg_error_t *);
static fsimg_status_t check_tables(boot_header_t *, fsimg_error_t *);
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

const fsimg_container_ops_t fsimg_ifsboot_container = {
	.probe_bytes = PROBE_BYTES,
	.probe = ifsboot_probe,
	.open = ifsboot_open,
	.verify = ifsboot_verify,
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
	char what[64];
	size_t i;

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
	for (i = 0; i < sizeof(boot_machines) / sizeof(boot_machines[0]); i++) {
		if (boot_machines[i].machine == header->machine)
			break;
	}
	if (i == sizeof(boot_machines) / sizeof(boot_machines[0])) {
		snprintf(what, sizeof(what), "machine %u is not supported",
		    (unsigned)header->machine);
		return (boot_unsupported(error, what));
	}
	if (boot_machines[i].prefix != header->prefix) {
		snprintf(what, sizeof(what),
		    "machine %u is not supported behind this boot prefix",
		    (unsigned)header->machine);
		return (boot_unsupported(error, what));
	}
	header->ram_checked = boot_machines[i].ram_checked;
	return (FSIMG_OK);
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
	if (header.ram_checked &&
	    (uint64_t)header.ram_size !=
	    (uint64_t)header.startup_size + header.imagefs_size)
		return (boot_corrupt(error,
		    "ram_size is not startup_size plus imagefs_size"));
	return (FSIMG_OK);
}
