/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fsimg.h"
#include "internal.h"
#include "test.h"

/*
 * QNX boot image (ifsboot) container tests.
 *
 * A generator builds boot images in memory: an opaque boot prefix (none,
 * or the 1088-byte x86 BIOS loader), a startup section of random opaque
 * bytes with the header fields set and the startup checksum word fixed,
 * and a stored body: the ImageFS itself, or chunks encoded by small
 * literal-and-run LZO1X and NRV2B writers, framed with 16-bit big-endian
 * lengths, the terminator, zero padding and the stored trailer. The body
 * is a small ImageFS this file also builds, so a boot image opens all the
 * way to its entries. Tests then mutate the finished bytes: every header
 * field, the framing, the padding and both sums, each failing with its
 * class and check.
 *
 * --native compares the library with the native evidence named by the
 * environment (FSIMG_IFS_NATIVE, FSIMG_IFS_IMAGES), printing counts only;
 * the seed and emit modes write generated images for the fuzzer and the
 * integration script.
 */

#define KIB		UINT64_C(1024)
#define MIB		(KIB * 1024)
#define BIOS_PREFIX	1088u
#define HEADER_BYTES	256u
#define MAX_CHUNKS	2048u
#define DATA_BYTES	135000u
#define SLOT_BYTES	524288u	/* The E10 disk's slot stride */
#define E10_BOOT_BYTES	49184u	/* Its compressed slots' length */

/* Startup header fields, from the prefix. */
enum {
	H_SIGNATURE = 0x00,
	H_VERSION = 0x04,
	H_FLAGS1 = 0x06,
	H_FLAGS2 = 0x07,
	H_HEADER_SIZE = 0x08,
	H_MACHINE = 0x0a,
	H_STARTUP_VADDR = 0x0c,
	H_PADDR_BIAS = 0x10,
	H_IMAGE_PADDR = 0x14,
	H_RAM_PADDR = 0x18,
	H_RAM_SIZE = 0x1c,
	H_STARTUP_SIZE = 0x20,
	H_STORED_SIZE = 0x24,
	H_IMAGEFS_PADDR = 0x28,
	H_IMAGEFS_SIZE = 0x2c,
	H_PREBOOT_SIZE = 0x30,
	H_TAIL = 0x32
};

#define COMPRESS_NONE	0x00u
#define COMPRESS_LZO	0x08u
#define COMPRESS_UCL	0x0cu

/* A growable byte string. */
typedef struct buffer {
	unsigned char	*bytes;
	size_t		 length;
	size_t		 capacity;
} buffer_t;

/* An image file in memory, counting the bytes the library reads. */
typedef struct memory_file {
	unsigned char	*bytes;
	size_t		 length;
	uint64_t	 read_bytes;
	uint64_t	 reads;
	uint64_t	 fail_at;	/* The read that fails with EIO; 0 never */
} memory_file_t;

/* How one chunk is encoded: its literal bytes as they are, or as one
 * literal and a repeat of it, which compresses any length to a few bytes.
 */
typedef enum chunk_form {
	FORM_LITERAL,
	FORM_RUN
} chunk_form_t;

typedef struct chunk_plan {
	uint32_t	 decoded;	/* Bytes of the body this chunk encodes */
	chunk_form_t	 form;
} chunk_plan_t;

/*
 * What the boot-image generator writes. The body is encoded raw or as the
 * planned chunks, which must cover it; compression is the flags1 value.
 * extra_padding adds zero words before the trailer and tail appends bytes
 * after the stored image. omit_terminator leaves out the zero length word
 * and fills the padding to a word boundary with pad_byte instead of zeros.
 * startup_bytes is the startup section's length, at least 260.
 */
typedef struct boot_spec {
	const unsigned char *body;
	size_t		 body_bytes;
	uint32_t	 prefix;
	uint16_t	 machine;
	uint8_t		 compression;
	bool		 virtual_bit;
	uint32_t	 startup_bytes;
	const chunk_plan_t *chunks;
	unsigned	 chunk_count;
	uint32_t	 extra_padding;
	uint32_t	 tail;
	uint32_t	 seed;
	bool		 omit_terminator;
	uint8_t		 pad_byte;
} boot_spec_t;

/* A generated image and where its parts lie in the file. */
typedef struct boot_image {
	buffer_t	 file;
	uint64_t	 header;	/* B */
	uint64_t	 body;		/* B + startup_size */
	uint64_t	 terminator;	/* The zero length word; 0 when raw */
	uint64_t	 trailer;	/* B + stored_size - 4 */
	uint64_t	 chunk_offsets[MAX_CHUNKS];	/* Length words */
	unsigned	 chunks;
} boot_image_t;

/* A context with a scratch directory, and an image opened in it; with
 * spool, the open receives a spool in that directory, so a spooled
 * container such as hbcifs opens too.
 */
typedef struct fixture {
	char		 directory[256];
	int		 scratch;
	bool		 spool;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_image_t	*image;
	uint64_t	 baseline_memory;
} fixture_t;

/* What reading one native file through the library found. */
typedef struct native_result {
	bool		 claimed;	/* ifsboot peeled the file */
	fsimg_status_t	 open;
	fsimg_status_t	 verify;
	uint64_t	 entries;
	uint64_t	 chunks;
	uint64_t	 tail_nonzero;	/* Nonzero bytes in the header's tail */
	char		 codecs[32];
} native_result_t;

static void buffer_append(buffer_t *, const void *, size_t);
static void buffer_byte(buffer_t *, uint8_t);
static void put_le16(unsigned char *, uint16_t);
static void put_le32(unsigned char *, uint32_t);
static uint32_t get_le32(const unsigned char *);
static uint32_t next_random(uint32_t *);
static void fill_random(unsigned char *, size_t, uint32_t);
static uint32_t word_sum(const unsigned char *, size_t);
static buffer_t imagefs_build(size_t);
static void lzo_extension(buffer_t *, size_t);
static void lzo_encode(buffer_t *, const unsigned char *, size_t,
    chunk_form_t);
static void nrv2b_bit(buffer_t *, size_t *, unsigned *, unsigned);
static void nrv2b_number(buffer_t *, size_t *, unsigned *, uint32_t);
static void nrv2b_encode(buffer_t *, const unsigned char *, size_t,
    chunk_form_t);
static boot_image_t boot_build(const boot_spec_t *);
static void boot_reseal(boot_image_t *);
static memory_file_t boot_file(boot_image_t *);
static fsimg_status_t file_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t file_size(void *, uint64_t *, fsimg_error_t *);
static void fixture_init(fixture_t *, uint64_t, uint64_t);
static void fixture_free(fixture_t *);
static fsimg_status_t fixture_open(fixture_t *, memory_file_t *,
    const char *, fsimg_error_t *);
static void expect_message(const fsimg_error_t *, const char *);
static void expect_open(boot_image_t *, const char *, fsimg_status_t,
    const char *);
static void expect_verify(boot_image_t *, fsimg_status_t, const char *);
static void expect_disk(fixture_t *, const unsigned char *, size_t);
static uint64_t detail(const fsimg_image_info_t *, const char *);
static uint64_t list_volume(fixture_t *, const unsigned char *, size_t);
static boot_spec_t sample_spec(const unsigned char *, size_t, uint32_t,
    uint8_t);
static void test_each_prefix_and_codec(void);
static void test_probe_offsets(void);
static void test_header_tables(void);
static void test_header_bounds(void);
static void set_length_word(boot_image_t *, uint64_t, uint32_t);
static uint32_t length_word(const boot_image_t *, uint64_t);
static void test_framing(void);
static void test_decoded_copy(void);
static void test_sums(void);
static void test_index_bound(void);
static void test_cache_budget(void);
static void test_carried_stack(void);
static void test_changed_source(void);
static fsimg_status_t read_whole_disk(fixture_t *, fsimg_error_t *);
static fsimg_status_t open_verify_read(fixture_t *, memory_file_t *);
static void test_fault_sweep(void);
static void write_file(const char *, const void *, size_t);
static int seed(const char *, uint8_t);
static int emit(const char *);
static unsigned char *read_whole(const char *, size_t *);
static native_result_t native_read(unsigned char *, size_t,
    const unsigned char *, size_t);
static int native(void);

static const chunk_plan_t lzo_chunks[] = {
	{ 61440, FORM_LITERAL }, { 4096, FORM_LITERAL },
	{ 61440, FORM_LITERAL }, { 61440, FORM_LITERAL }
};
static const chunk_plan_t nrv2b_chunks[] = {
	{ 32768, FORM_LITERAL }, { 4096, FORM_LITERAL },
	{ 32768, FORM_LITERAL }, { 32768, FORM_LITERAL },
	{ 32768, FORM_LITERAL }, { 32768, FORM_LITERAL }
};

static void
buffer_append(buffer_t *buffer, const void *bytes, size_t length)
{
	if (buffer->length + length > buffer->capacity) {
		buffer->capacity = (buffer->length + length) * 2 + 64;
		buffer->bytes = realloc(buffer->bytes, buffer->capacity);
		CHECK(buffer->bytes != NULL);
	}
	if (length != 0)
		memcpy(buffer->bytes + buffer->length, bytes, length);
	buffer->length += length;
}

static void
buffer_byte(buffer_t *buffer, uint8_t byte)
{
	buffer_append(buffer, &byte, 1);
}

static void
put_le16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

static void
put_le32(unsigned char *p, uint32_t value)
{
	put_le16(p, (uint16_t)value);
	put_le16(p + 2, (uint16_t)(value >> 16));
}

static uint32_t
get_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static uint32_t
next_random(uint32_t *state)
{
	*state = *state * 1103515245u + 12345u;
	return (*state >> 8);
}

/* Bytes no codec compresses: the generator stores them as literals. */
static void
fill_random(unsigned char *bytes, size_t length, uint32_t seed)
{
	uint32_t state = seed * 2654435761u + 1;

	for (size_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)next_random(&state);
}

/* The 32-bit little-endian word sum of length bytes, a multiple of 4. */
static uint32_t
word_sum(const unsigned char *bytes, size_t length)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < length; i += 4)
		sum += get_le32(bytes + i);
	return (sum);
}

/*
 * A small little-endian ImageFS: mountpoint "/", the root, a directory
 * "etc", a file "etc/data" of data_bytes random bytes, a file "hello" and
 * a link "link" to it, then the terminator, the data and the checksum
 * trailer. The record layout is the driver design's
 * (specs/design-ifs.md); test/ifs.c tests the reader against it fully.
 */
static buffer_t
imagefs_build(size_t data_bytes)
{
	static const char hello[] = "hello, boot image\n";
	struct record {
		const char *path;
		uint32_t mode;
		uint32_t path_offset;
		const unsigned char *data;
		size_t data_length;
		const char *target;
	} records[] = {
		{ "", 040755, 0x18, NULL, 0, NULL },
		{ "etc", 040755, 0x18, NULL, 0, NULL },
		{ "etc/data", 0100644, 0x20, NULL, 0, NULL },
		{ "hello", 0100644, 0x20, (const unsigned char *)hello,
		    sizeof(hello) - 1, NULL },
		{ "link", 0120777, 0x1c, NULL, 0, "hello" }
	};
	unsigned char *data;
	buffer_t image = { 0 };
	uint32_t table, length[5], data_offset[5], hdr_dir_size, image_size,
	    offset;
	unsigned char *r;

	data = malloc(data_bytes);
	CHECK(data != NULL);
	fill_random(data, data_bytes, 7);
	records[2].data = data;
	records[2].data_length = data_bytes;

	table = 0x5c;	/* The 88-byte header, "/" and its NUL, padded */
	offset = table;
	for (unsigned i = 0; i < 5; i++) {
		length[i] = records[i].path_offset +
		    (uint32_t)strlen(records[i].path) + 1;
		if (records[i].target != NULL)
			length[i] += (uint32_t)strlen(records[i].target) + 1;
		length[i] = (length[i] + 3) & ~3u;
		offset += length[i];
	}
	hdr_dir_size = offset + 4;
	offset = hdr_dir_size;
	for (unsigned i = 0; i < 5; i++) {
		data_offset[i] = (offset + 3) & ~3u;
		if (records[i].data != NULL)
			offset = data_offset[i] + (uint32_t)records[i].data_length;
	}
	image_size = ((offset + 3) & ~3u) + 4;

	image.bytes = calloc(1, image_size);
	CHECK(image.bytes != NULL);
	image.length = image.capacity = image_size;
	memcpy(image.bytes, "imagefs", 7);
	image.bytes[7] = 0x04;
	put_le32(image.bytes + 0x08, image_size);
	put_le32(image.bytes + 0x0c, hdr_dir_size);
	put_le32(image.bytes + 0x10, table);
	image.bytes[0x58] = '/';
	offset = table;
	for (unsigned i = 0; i < 5; i++) {
		r = image.bytes + offset;
		put_le16(r + 0x00, (uint16_t)length[i]);
		put_le32(r + 0x04, i + 1);
		put_le32(r + 0x08, records[i].mode);
		put_le32(r + 0x14, 0x6ad7c93au);
		memcpy(r + records[i].path_offset, records[i].path,
		    strlen(records[i].path));
		if (records[i].data != NULL) {
			put_le32(r + 0x18, data_offset[i]);
			put_le32(r + 0x1c, (uint32_t)records[i].data_length);
			memcpy(image.bytes + data_offset[i], records[i].data,
			    records[i].data_length);
		}
		if (records[i].target != NULL) {
			put_le16(r + 0x18, (uint16_t)strlen(records[i].path) + 1);
			put_le16(r + 0x1a, (uint16_t)strlen(records[i].target));
			memcpy(r + records[i].path_offset +
			    strlen(records[i].path) + 1, records[i].target,
			    strlen(records[i].target));
		}
		offset += length[i];
	}
	put_le32(image.bytes + image_size - 4,
	    0 - word_sum(image.bytes, image_size - 4));
	free(data);
	return (image);
}

/* An LZO1X length extension: a zero byte per 255, then the rest. */
static void
lzo_extension(buffer_t *out, size_t value)
{
	size_t zeros = (value - 1) / 255;

	for (size_t i = 0; i < zeros; i++)
		buffer_byte(out, 0);
	buffer_byte(out, (uint8_t)(value - 255 * zeros));
}

/*
 * One LZO1X stream of length bytes. A literal chunk is one literal run:
 * the first-byte form 17 + n up to 238 literals, else 0000 0000 and an
 * extension of n - 18. A run chunk is the first byte as one literal and a
 * long match at distance 1 repeating it, 001L LLLL with L = 0 and an
 * extension of n - 1 - 33 (n - 1 at least 34). Both end with the
 * canonical end marker 11 00 00.
 */
static void
lzo_encode(buffer_t *out, const unsigned char *bytes, size_t length,
    chunk_form_t form)
{
	if (form == FORM_RUN) {
		CHECK(length >= 35);
		buffer_byte(out, 18);
		buffer_byte(out, bytes[0]);
		buffer_byte(out, 0x20);
		lzo_extension(out, length - 1 - 33);
		buffer_byte(out, 0);
		buffer_byte(out, 0);
	} else if (length <= 238) {
		CHECK(length >= 1);
		buffer_byte(out, (uint8_t)(17 + length));
		buffer_append(out, bytes, length);
	} else {
		buffer_byte(out, 0);
		lzo_extension(out, length - 18);
		buffer_append(out, bytes, length);
	}
	buffer_append(out, "\x11\x00\x00", 3);
}

/* NRV2B control bits, eight to a byte, most significant first, from a
 * control byte placed in the stream when its first bit is written.
 */
static void
nrv2b_bit(buffer_t *out, size_t *control, unsigned *free_bits, unsigned bit)
{
	if (*free_bits == 0) {
		*control = out->length;
		buffer_byte(out, 0);
		*free_bits = 8;
	}
	(*free_bits)--;
	if (bit != 0)
		out->bytes[*control] |= (unsigned char)(1u << *free_bits);
}

/* The number code: each bit below the top one, then 1 after the last. */
static void
nrv2b_number(buffer_t *out, size_t *control, unsigned *free_bits,
    uint32_t value)
{
	int top;

	for (top = 31; (value >> top) == 0; top--)
		;
	for (int bit = top - 1; bit >= 0; bit--) {
		nrv2b_bit(out, control, free_bits, (value >> bit) & 1);
		nrv2b_bit(out, control, free_bits, bit == 0);
	}
}

/*
 * One NRV2B stream of length bytes: each literal a 1 bit and its byte, or
 * for a run chunk one literal and a match at offset 1 copying length - 1
 * bytes (high part 3 and raw byte 0; the length field length - 2 coded as
 * 00 and the number length - 4). Both end with the end marker: a 0 bit,
 * the number 0x1000002 and the byte 0xff.
 */
static void
nrv2b_encode(buffer_t *out, const unsigned char *bytes, size_t length,
    chunk_form_t form)
{
	size_t control = 0, literals;
	unsigned free_bits = 0;

	literals = form == FORM_RUN ? 1 : length;
	for (size_t i = 0; i < literals; i++) {
		nrv2b_bit(out, &control, &free_bits, 1);
		buffer_byte(out, bytes[i]);
	}
	if (form == FORM_RUN) {
		CHECK(length >= 6);
		nrv2b_bit(out, &control, &free_bits, 0);
		nrv2b_number(out, &control, &free_bits, 3);
		buffer_byte(out, 0);
		nrv2b_bit(out, &control, &free_bits, 0);
		nrv2b_bit(out, &control, &free_bits, 0);
		nrv2b_number(out, &control, &free_bits, (uint32_t)(length - 4));
	}
	nrv2b_bit(out, &control, &free_bits, 0);
	nrv2b_number(out, &control, &free_bits, 0x1000002u);
	buffer_byte(out, 0xff);
}

/*
 * Build a boot image from spec: prefix bytes, the startup section with the
 * header and opaque random code, its checksum word last; then the body,
 * raw or framed, and the stored trailer; then any tail. A run chunk
 * repeats the body byte it starts at, so the generator overwrites that
 * range of a private copy of the body to match what it encodes.
 */
static boot_image_t
boot_build(const boot_spec_t *spec)
{
	boot_image_t image;
	buffer_t *file = &image.file;
	unsigned char *startup, *body, word[2];
	uint64_t position;
	uint32_t startup_bytes, stored_size, flags1;
	size_t length, before;
	unsigned i;

	bzero(&image, sizeof(image));
	startup_bytes = spec->startup_bytes == 0 ? 260 : spec->startup_bytes;
	CHECK(startup_bytes >= 260 && startup_bytes % 4 == 0);
	body = malloc(spec->body_bytes);
	CHECK(body != NULL);
	memcpy(body, spec->body, spec->body_bytes);

	startup = malloc(spec->prefix + startup_bytes);
	CHECK(startup != NULL);
	fill_random(startup, spec->prefix + startup_bytes, spec->seed + 1);
	buffer_append(file, startup, spec->prefix + startup_bytes);
	free(startup);
	image.header = spec->prefix;
	image.body = spec->prefix + startup_bytes;

	if (spec->compression == COMPRESS_NONE)
		buffer_append(file, body, spec->body_bytes);
	else {
		position = 0;
		for (i = 0; i < spec->chunk_count; i++) {
			length = spec->chunks[i].decoded;
			CHECK(position + length <= spec->body_bytes);
			if (spec->chunks[i].form == FORM_RUN)
				memset(body + position, body[position], length);
			CHECK(image.chunks < MAX_CHUNKS);
			image.chunk_offsets[image.chunks++] = file->length;
			buffer_append(file, "\0\0", 2);
			before = file->length;
			if (spec->compression == COMPRESS_LZO)
				lzo_encode(file, body + position, length,
				    spec->chunks[i].form);
			else
				nrv2b_encode(file, body + position, length,
				    spec->chunks[i].form);
			CHECK(file->length - before <= 0xffff);
			word[0] = (unsigned char)((file->length - before) >> 8);
			word[1] = (unsigned char)(file->length - before);
			memcpy(file->bytes + before - 2, word, 2);
			position += length;
		}
		image.terminator = file->length;
		if (!spec->omit_terminator)
			buffer_append(file, "\0\0", 2);
		while ((file->length - spec->prefix) % 4 != 0)
			buffer_byte(file, spec->pad_byte);
		for (i = 0; i < spec->extra_padding; i++)
			buffer_append(file, "\0\0\0\0", 4);
		image.trailer = file->length;
		buffer_append(file, "\0\0\0\0", 4);
	}
	stored_size = (uint32_t)(file->length - spec->prefix);
	if (spec->compression == COMPRESS_NONE)
		image.trailer = file->length - 4;

	/* The header over the random startup bytes; its tail stays random. */
	startup = file->bytes + spec->prefix;
	flags1 = spec->compression | (spec->virtual_bit ? 0x01u : 0);
	put_le32(startup + H_SIGNATURE, 0x00ff7eebu);
	put_le16(startup + H_VERSION, 1);
	startup[H_FLAGS1] = (unsigned char)flags1;
	put_le16(startup + H_HEADER_SIZE, HEADER_BYTES);
	put_le16(startup + H_MACHINE, spec->machine);
	put_le32(startup + H_RAM_SIZE, spec->machine == 0 ? 0 :
	    startup_bytes + (uint32_t)spec->body_bytes);
	put_le32(startup + H_STARTUP_SIZE, startup_bytes);
	put_le32(startup + H_STORED_SIZE, stored_size);
	put_le32(startup + H_IMAGEFS_SIZE, (uint32_t)spec->body_bytes);
	put_le16(startup + H_PREBOOT_SIZE, (uint16_t)spec->prefix);
	for (i = 0; i < spec->tail; i++)
		buffer_byte(file, 0x5a);
	boot_reseal(&image);
	free(body);
	return (image);
}

/* Fix both checksum words after a change: the startup section's last word,
 * then the stored trailer of a framed body (a raw body's ImageFS checksum
 * closes the stored sum by itself).
 */
static void
boot_reseal(boot_image_t *image)
{
	unsigned char *startup = image->file.bytes + image->header;
	uint32_t startup_size = get_le32(startup + H_STARTUP_SIZE);

	put_le32(startup + startup_size - 4, 0);
	put_le32(startup + startup_size - 4, 0 - word_sum(startup, startup_size));
	if (image->terminator == 0)
		return;
	put_le32(image->file.bytes + image->trailer, 0);
	put_le32(image->file.bytes + image->trailer, 0 -
	    word_sum(image->file.bytes + image->body,
	    (size_t)(image->trailer - image->body)));
}

/* The image's bytes as a file the test then owns. */
static memory_file_t
boot_file(boot_image_t *image)
{
	memory_file_t file = { .bytes = image->file.bytes,
	    .length = image->file.length };

	bzero(&image->file, sizeof(image->file));
	return (file);
}

static fsimg_status_t
file_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_file_t *file = argument;

	CHECK(offset <= file->length && length <= file->length - offset);
	if (++file->reads == file->fail_at) {
		bzero(error, sizeof(*error));
		error->status = FSIMG_EIO;
		error->os_error = EIO;
		error->operation = FSIMG_OP_IO;
		snprintf(error->message, sizeof(error->message),
		    "injected read failure");
		return (FSIMG_EIO);
	}
	memcpy(buffer, file->bytes + offset, length);
	file->read_bytes += length;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
file_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((memory_file_t *)argument)->length;
	return (FSIMG_OK);
}

static void
fixture_init(fixture_t *f, uint64_t memory, uint64_t work)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-ifsboot.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = memory;
	options.max_work = work;
	options.scratch_directory_fd = f->scratch;
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
	fsimg_status_t status;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	f->image = NULL;
	f->io = NULL;
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	/* Only an injected allocation failure can fail it. */
	status = fsimg_io_from_callbacks(f->ctx, &ops, file, &f->io, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_image_options_init(&options);
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	if (f->spool) {
		options.spool_fd = openat(f->scratch, "spool", O_RDWR | O_CREAT |
		    O_EXCL | O_CLOEXEC, 0600);
		CHECK(options.spool_fd != -1);
		CHECK(unlinkat(f->scratch, "spool", 0) == 0);
	}
	return (fsimg_image_open(f->ctx, f->io, &options, &f->image, error));
}

/* The failure names the check; a NULL message accepts any, such as a
 * codec's own, which names the check inside the stream.
 */
static void
expect_message(const fsimg_error_t *error, const char *message)
{
	if (message != NULL && strcmp(error->message, message) != 0) {
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message,
		    error->message);
		CHECK(!"the failure names another check");
	}
}

/* Opening fails with the named check, attributed to ifsboot, or for a
 * claim the probe refuses, with the probe's reason; FSIMG_OK expects the
 * open to succeed. The image is released either way.
 */
static void
expect_open(boot_image_t *image, const char *stack, fsimg_status_t status,
    const char *message)
{
	memory_file_t file = boot_file(image);
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 16 * MIB, 0);
	if (fixture_open(&f, &file, stack, &error) != status) {
		fprintf(stderr, "open: %s\n", error.message);
		CHECK(!"the open ended with another status");
	}
	if (status != FSIMG_OK) {
		expect_message(&error, message);
		if (status != FSIMG_EFORMAT && strcmp(error.path, "ifsboot") != 0) {
			fprintf(stderr, "\"%s\" names \"%s\"\n", error.message,
			    error.path);
			CHECK(!"the failure names another layer");
		}
	}
	fixture_free(&f);
	free(file.bytes);
}

/* The image opens; layer verification ends with status and the check. */
static void
expect_verify(boot_image_t *image, fsimg_status_t status, const char *message)
{
	memory_file_t file = boot_file(image);
	fsimg_layer_report_t report;
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    &error) == status);
	if (status == FSIMG_OK) {
		CHECK(report.complete);
		CHECK(strcmp(report.container, "ifsboot") == 0);
		CHECK(report.scope == FSIMG_VALIDATION_STRUCTURAL);
	} else {
		expect_message(&error, message);
		CHECK(strcmp(error.path, "ifsboot") == 0);
	}
	fixture_free(&f);
	free(file.bytes);
}

/* The container's disk reads as the expected bytes, in pieces that
 * straddle chunk boundaries.
 */
static void
expect_disk(fixture_t *f, const unsigned char *expected, size_t length)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char piece[7000];
	fsimg_io_t *disk;
	uint64_t size;
	size_t count, transferred;

	CHECK(fsimg_image_select(f->image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(disk, &size, NULL) == FSIMG_OK && size == length);
	for (size_t offset = 0; offset < length; offset += count) {
		count = length - offset < sizeof(piece) ? length - offset :
		    sizeof(piece);
		CHECK(fsimg_io_read_exact(disk, offset, piece, count, &transferred,
		    NULL) == FSIMG_OK);
		CHECK(memcmp(piece, expected + offset, count) == 0);
	}
	fsimg_io_free(disk);
}

static uint64_t
detail(const fsimg_image_info_t *info, const char *name)
{
	for (uint32_t i = 0; i < info->detail_count; i++) {
		if (strcmp(info->details[i].name, name) == 0)
			return (info->details[i].value);
	}
	fprintf(stderr, "no detail %s\n", name);
	CHECK(!"the detail is missing");
	return (0);
}

/*
 * Select the volume automatically, open it with the ImageFS reader and
 * list it, reading every file; verify it too. The generated body's files
 * read back as written: data is the random file, NULL skips that check.
 * Returns the number of entries.
 */
static uint64_t
list_volume(fixture_t *f, const unsigned char *data, size_t data_bytes)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_AUTOMATIC };
	const fsimg_entry_t *entry;
	fsimg_verify_report_t verified;
	fsimg_reader_t *reader;
	unsigned char *content;
	fsimg_io_t *volume;
	uint64_t entries = 0;
	size_t count;
	fsimg_status_t status;

	CHECK(fsimg_image_select(f->image, &selector, &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f->ctx, volume, NULL, &reader, NULL) == FSIMG_OK);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		entries++;
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		content = malloc((size_t)entry->size + 1);
		CHECK(content != NULL);
		for (uint64_t done = 0; done < entry->size; done += count) {
			CHECK(fsimg_reader_read_at(reader, done, content + done,
			    (size_t)(entry->size - done), &count, NULL) == FSIMG_OK);
			CHECK(count != 0);
		}
		if (data != NULL && strcmp(entry->path, "/etc/data") == 0)
			CHECK(entry->size == data_bytes &&
			    memcmp(content, data, data_bytes) == 0);
		free(content);
	}
	CHECK(status == FSIMG_DONE);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(f->ctx, volume, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &verified, NULL) ==
	    FSIMG_OK);
	CHECK(verified.complete);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
	return (entries);
}

/* A boot image of body under each codec's chunk plan. */
static boot_spec_t
sample_spec(const unsigned char *body, size_t body_bytes, uint32_t prefix,
    uint8_t compression)
{
	boot_spec_t spec = {
		.body = body, .body_bytes = body_bytes, .prefix = prefix,
		.machine = prefix == BIOS_PREFIX ? 3 : 42,
		.compression = compression, .virtual_bit = true,
		.startup_bytes = 4096, .seed = prefix + compression
	};
	const chunk_plan_t *plan = compression == COMPRESS_UCL ? nrv2b_chunks :
	    lzo_chunks;
	size_t count = compression == COMPRESS_UCL ?
	    sizeof(nrv2b_chunks) / sizeof(nrv2b_chunks[0]) :
	    sizeof(lzo_chunks) / sizeof(lzo_chunks[0]);
	static chunk_plan_t chunks[16];
	uint64_t covered = 0;

	/* The plan's chunks, then a final chunk for the rest. */
	spec.chunk_count = 0;
	for (size_t i = 0; i < count && covered + plan[i].decoded < body_bytes;
	    i++) {
		chunks[spec.chunk_count++] = plan[i];
		covered += plan[i].decoded;
	}
	chunks[spec.chunk_count].decoded = (uint32_t)(body_bytes - covered);
	chunks[spec.chunk_count++].form = FORM_LITERAL;
	spec.chunks = chunks;
	return (spec);
}

/*
 * Every prefix with every compression value reads: detected with no
 * stack, the stack and info name ifs/ifsboot and the startup fields, the
 * disk is the ImageFS byte for byte, the volume lists and verifies, and
 * the layer verifies. A nonbootable machine-0 image reads as mkifs writes
 * it, with ram_size zero and unchecked.
 */
static void
test_each_prefix_and_codec(void)
{
	static const uint32_t prefixes[] = { 0, BIOS_PREFIX };
	static const uint8_t compressions[] = { COMPRESS_NONE, COMPRESS_LZO,
	    COMPRESS_UCL };
	static const char *const codecs[] = { "raw", "lzo1x", "nrv2b" };
	buffer_t body = imagefs_build(DATA_BYTES);
	fsimg_image_info_t info;
	boot_image_t image;
	memory_file_t file;
	boot_spec_t spec;
	fixture_t f;
	uint64_t smallest, largest, nonzero;

	for (unsigned p = 0; p < 2; p++) {
		for (unsigned c = 0; c < 3; c++) {
			spec = sample_spec(body.bytes, body.length, prefixes[p],
			    compressions[c]);
			smallest = largest = body.length;
			if (compressions[c] != COMPRESS_NONE) {
				smallest = largest = spec.chunks[0].decoded;
				for (unsigned i = 1; i < spec.chunk_count; i++) {
					if (spec.chunks[i].decoded < smallest)
						smallest = spec.chunks[i].decoded;
					if (spec.chunks[i].decoded > largest)
						largest = spec.chunks[i].decoded;
				}
			}
			image = boot_build(&spec);
			file = boot_file(&image);
			fixture_init(&f, 16 * MIB, 0);
			CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
			CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
			CHECK(strcmp(info.stack, "ifs/ifsboot") == 0);
			CHECK(strcmp(info.container, "ifsboot") == 0);
			CHECK(strcmp(info.codecs, codecs[c]) == 0);
			CHECK(info.disk_bytes == body.length);
			CHECK(info.file_bytes == file.length);
			CHECK(info.chunk_count == (c == 0 ? 1 : spec.chunk_count));
			CHECK(info.detail_count == 21);
			/* The opaque fields follow preboot_size, as the profile
			 * orders them; the generator's tail is random bytes.
			 */
			CHECK(strcmp(info.details[15].name, "flags2") == 0 &&
			    strcmp(info.details[16].name, "tail_nonzero") == 0 &&
			    strcmp(info.details[17].name, "body_bytes") == 0);
			nonzero = 0;
			for (unsigned t = H_TAIL; t < HEADER_BYTES; t++)
				nonzero += file.bytes[prefixes[p] + t] != 0;
			CHECK(detail(&info, "flags2") ==
			    file.bytes[prefixes[p] + H_FLAGS2]);
			CHECK(detail(&info, "tail_nonzero") == nonzero);
			CHECK(detail(&info, "prefix_bytes") == prefixes[p]);
			CHECK(detail(&info, "preboot_size") == prefixes[p]);
			CHECK(detail(&info, "version") == 1);
			CHECK(detail(&info, "virtual") == 1);
			CHECK(detail(&info, "compression") == compressions[c]);
			CHECK(detail(&info, "machine") == spec.machine);
			CHECK(detail(&info, "startup_size") == 4096);
			CHECK(detail(&info, "imagefs_size") == body.length);
			CHECK(detail(&info, "ram_size") == 4096 + body.length);
			CHECK(detail(&info, "stored_size") ==
			    file.length - prefixes[p]);
			CHECK(detail(&info, "body_bytes") ==
			    file.length - prefixes[p] - 4096);
			CHECK(detail(&info, "max_chunk_bytes") == largest);
			CHECK(detail(&info, "min_chunk_bytes") == smallest);
			CHECK(detail(&info, "max_stored_chunk_bytes") >= (c == 0 ?
			    body.length : largest));
			expect_disk(&f, body.bytes, body.length);
			CHECK(list_volume(&f, NULL, 0) == 4);
			fixture_free(&f);
			image.file.bytes = file.bytes;
			image.file.length = file.length;
			expect_verify(&image, FSIMG_OK, NULL);
		}
	}

	/* mkifs's nonbootable compressed output: machine 0, 260 bytes. */
	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_LZO);
	spec.machine = 0;
	spec.startup_bytes = 260;
	spec.virtual_bit = false;
	image = boot_build(&spec);
	file = boot_file(&image);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, "ifs/-/ifsboot", NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(detail(&info, "machine") == 0 && detail(&info, "ram_size") == 0);
	CHECK(detail(&info, "virtual") == 0);
	CHECK(list_volume(&f, NULL, 0) == 4);
	fixture_free(&f);
	image.file.bytes = file.bytes;
	image.file.length = file.length;
	expect_verify(&image, FSIMG_OK, NULL);
	free(body.bytes);
}

/*
 * The probe looks only at the table's offsets, and a header there claims
 * only when preboot_size ties it to its offset; the first tie decides.
 */
static void
test_probe_offsets(void)
{
	buffer_t body = imagefs_build(1000);
	fsimg_image_info_t info;
	memory_file_t file;
	boot_spec_t spec;
	boot_image_t image, moved;
	fixture_t f;

	/* A header at an offset outside the table, tied to it: no claim, so
	 * the file is the disk, and a forced ifsboot finds no identity. */
	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_NONE);
	image = boot_build(&spec);
	bzero(&moved, sizeof(moved));
	buffer_append(&moved.file, "\0\0\0\0", 4);
	buffer_append(&moved.file, image.file.bytes, image.file.length);
	put_le16(moved.file.bytes + 4 + H_PREBOOT_SIZE, 4);
	file = boot_file(&moved);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, "ifs/-", NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.container[0] == '\0' && info.disk_bytes == file.length);
	fixture_free(&f);
	moved.file.bytes = file.bytes;
	moved.file.length = file.length;
	expect_open(&moved, "ifs/-/ifsboot", FSIMG_EFORMAT,
	    "no QNX startup header at a boot-prefix offset");

	/* preboot_size not equal to the header's offset: no claim. */
	put_le16(image.file.bytes + H_PREBOOT_SIZE, BIOS_PREFIX);
	expect_open(&image, "ifs/-/ifsboot", FSIMG_EFORMAT,
	    "no QNX startup header at a boot-prefix offset");
	spec = sample_spec(body.bytes, body.length, BIOS_PREFIX, COMPRESS_NONE);
	image = boot_build(&spec);
	put_le16(image.file.bytes + BIOS_PREFIX + H_PREBOOT_SIZE, 0);
	expect_open(&image, "ifs/-/ifsboot", FSIMG_EFORMAT,
	    "no QNX startup header at a boot-prefix offset");

	/* A signature and header size inside the BIOS loader, untied, do not
	 * claim; the tied header behind them does. */
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_SIGNATURE, 0x00ff7eebu);
	put_le16(image.file.bytes + H_HEADER_SIZE, HEADER_BYTES);
	put_le16(image.file.bytes + H_PREBOOT_SIZE, 64);
	expect_open(&image, NULL, FSIMG_OK, NULL);
	/* The first tie decides: a header tied at 0 claims there, and its
	 * bounds then fail against this file. */
	image = boot_build(&spec);
	memcpy(image.file.bytes, image.file.bytes + BIOS_PREFIX, HEADER_BYTES);
	put_le16(image.file.bytes + H_PREBOOT_SIZE, 0);
	expect_open(&image, NULL, FSIMG_EUNSUPPORTED,
	    "QNX boot image machine 3 is not supported behind this boot prefix");

	/* The ImageFS signature inside the startup code, as in SH-4 startup
	 * programs, is no claim: detection finds the startup header alone,
	 * and without that header nothing reads the file as an ImageFS. */
	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_LZO);
	image = boot_build(&spec);
	memcpy(image.file.bytes + 512, "imagefs", 7);
	memcpy(image.file.bytes + 1024, "imagefs", 7);
	boot_reseal(&image);
	file = boot_file(&image);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.stack, "ifs/ifsboot") == 0);
	fixture_free(&f);
	file.bytes[H_SIGNATURE] ^= 0xff;
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
	fixture_free(&f);
	free(file.bytes);
	free(body.bytes);
}

/* Each closed-table field outside its table is unsupported, named. */
static void
test_header_tables(void)
{
	static const struct {
		unsigned offset;
		unsigned width;
		uint32_t value;
		const char *message;
	} cases[] = {
		{ H_VERSION, 2, 2, "QNX boot image startup header version is not 1" },
		{ H_VERSION, 2, 0, "QNX boot image startup header version is not 1" },
		{ H_FLAGS1, 1, 0x0b, "QNX boot image flags1 marks a big-endian "
		    "header under a little-endian signature" },
		{ H_FLAGS1, 1, 0x29, "QNX boot image flags1 has reserved bits set" },
		{ H_FLAGS1, 1, 0x89, "QNX boot image flags1 has reserved bits set" },
		{ H_FLAGS1, 1, 0x05, "QNX boot image compression value 0x04 is "
		    "not supported (zlib)" },
		{ H_FLAGS1, 1, 0x11, "QNX boot image compression value 0x10 is "
		    "not supported" },
		{ H_FLAGS1, 1, 0x1d, "QNX boot image compression value 0x1c is "
		    "not supported" },
		{ H_MACHINE, 2, 7, "QNX boot image machine 7 is not supported" },
		{ H_MACHINE, 2, 3, "QNX boot image machine 3 is not supported "
		    "behind this boot prefix" }
	};
	buffer_t body = imagefs_build(1000);
	boot_spec_t spec;
	boot_image_t image;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		spec = sample_spec(body.bytes, body.length, 0, COMPRESS_LZO);
		image = boot_build(&spec);
		if (cases[i].width == 1)
			image.file.bytes[cases[i].offset] =
			    (unsigned char)cases[i].value;
		else
			put_le16(image.file.bytes + cases[i].offset,
			    (uint16_t)cases[i].value);
		boot_reseal(&image);
		expect_open(&image, NULL, FSIMG_EUNSUPPORTED, cases[i].message);
	}
	/* Machine 42 behind the BIOS loader is just as unobserved. */
	spec = sample_spec(body.bytes, body.length, BIOS_PREFIX, COMPRESS_LZO);
	spec.machine = 42;
	image = boot_build(&spec);
	expect_open(&image, NULL, FSIMG_EUNSUPPORTED,
	    "QNX boot image machine 42 is not supported behind this boot prefix");
	/* The virtual bit takes either value. */
	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_UCL);
	spec.virtual_bit = false;
	image = boot_build(&spec);
	expect_open(&image, NULL, FSIMG_OK, NULL);
	free(body.bytes);
}

/* Each bound violated is corrupt, named; opaque fields open, and only the
 * startup checksum notices them.
 */
static void
test_header_bounds(void)
{
	static const unsigned opaque[] = { H_FLAGS2, H_STARTUP_VADDR,
	    H_PADDR_BIAS, H_IMAGE_PADDR, H_RAM_PADDR, H_IMAGEFS_PADDR, H_TAIL,
	    0xff, 0x100 };
	buffer_t body = imagefs_build(1000);
	boot_spec_t spec, raw;
	boot_image_t image;
	unsigned char *header;
	uint32_t startup, stored;

	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_LZO);
	raw = sample_spec(body.bytes, body.length, 0, COMPRESS_NONE);

	image = boot_build(&spec);
	put_le32(image.file.bytes + H_STARTUP_SIZE, 256);
	expect_open(&image, NULL, FSIMG_ECORRUPT,
	    "QNX boot image startup size is below 260 or not a multiple of 4");
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_STARTUP_SIZE, 4098);
	expect_open(&image, NULL, FSIMG_ECORRUPT,
	    "QNX boot image startup size is below 260 or not a multiple of 4");
	image = boot_build(&spec);
	stored = get_le32(image.file.bytes + H_STORED_SIZE);
	put_le32(image.file.bytes + H_STORED_SIZE, stored - 2);
	expect_open(&image, NULL, FSIMG_ECORRUPT, "QNX boot image stored size "
	    "is below the startup size or not a multiple of 4");
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_STORED_SIZE, 4092);
	expect_open(&image, NULL, FSIMG_ECORRUPT, "QNX boot image stored size "
	    "is below the startup size or not a multiple of 4");
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_STORED_SIZE, stored - 4);
	expect_open(&image, NULL, FSIMG_ECORRUPT,
	    "QNX boot image stored image does not end at the end of the file");
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_STORED_SIZE, stored + 4);
	expect_open(&image, NULL, FSIMG_ECORRUPT,
	    "QNX boot image stored image does not end at the end of the file");
	/* Bytes after the stored image: an IFSCSA array, a signature or an
	 * S-record wrapper is not a boot image under this profile. */
	spec.tail = 4;
	image = boot_build(&spec);
	expect_open(&image, NULL, FSIMG_ECORRUPT,
	    "QNX boot image stored image does not end at the end of the file");
	spec.tail = 0;

	/* A raw body is the ImageFS exactly. */
	image = boot_build(&raw);
	put_le32(image.file.bytes + H_IMAGEFS_SIZE, (uint32_t)body.length - 4);
	expect_open(&image, NULL, FSIMG_ECORRUPT, "QNX boot image stored size "
	    "is not the startup size plus imagefs_size");
	/* A framed body has room for the terminator, padding and trailer. */
	image = boot_build(&spec);
	image.file.length = 4096 + 4;
	header = image.file.bytes;
	put_le32(header + H_STORED_SIZE, 4096 + 4);
	expect_open(&image, NULL, FSIMG_ECORRUPT, "QNX boot image stored size "
	    "leaves no room for the terminator and the trailer");

	/* The opaque fields and the target addresses: the open reads them as
	 * reported values only, and verification sees the changed sum. */
	for (size_t i = 0; i < sizeof(opaque) / sizeof(opaque[0]); i++) {
		image = boot_build(&spec);
		image.file.bytes[opaque[i]] ^= 0x40;
		expect_verify(&image, FSIMG_ECORRUPT, "QNX boot image startup "
		    "checksum: the startup section does not sum to zero");
	}
	/* ram_size against its rule, where the machine table checks it. */
	image = boot_build(&spec);
	startup = get_le32(image.file.bytes + H_STARTUP_SIZE);
	put_le32(image.file.bytes + H_RAM_SIZE, startup);
	boot_reseal(&image);
	expect_verify(&image, FSIMG_ECORRUPT,
	    "QNX boot image ram_size is not startup_size plus imagefs_size");
	spec.machine = 0;
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_RAM_SIZE, 12345);
	boot_reseal(&image);
	expect_verify(&image, FSIMG_OK, NULL);
	free(body.bytes);
}

/* The BE16 length word of a framed chunk at offset. */
static void
set_length_word(boot_image_t *image, uint64_t offset, uint32_t value)
{
	image->file.bytes[offset] = (unsigned char)(value >> 8);
	image->file.bytes[offset + 1] = (unsigned char)value;
}

static uint32_t
length_word(const boot_image_t *image, uint64_t offset)
{
	return ((uint32_t)image->file.bytes[offset] << 8 |
	    image->file.bytes[offset + 1]);
}

/*
 * The framing, for each codec, over random bodies whose chunk plans the
 * tests choose: the terminator, the chunks' reach, the decoded sizes, the
 * padding and the decoded total, and the streams themselves.
 */
static void
test_framing(void)
{
	static const chunk_plan_t small_first[] = { { 4095, FORM_LITERAL },
	    { 4096, FORM_LITERAL } };
	static const chunk_plan_t max_runs[] = { { 65536, FORM_RUN },
	    { 65536, FORM_RUN }, { 100, FORM_LITERAL } };
	static const chunk_plan_t long_run[] = { { 65537, FORM_RUN } };
	chunk_plan_t plan[2];
	const char *past_limit;
	unsigned char *data;
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	fixture_t f;
	uint64_t pad;

	data = malloc(3 * 65536);
	CHECK(data != NULL);
	for (unsigned c = 0; c < 2; c++) {
		/* Each decoder names a stream that wants more output than
		 * the chunk's bound allows. */
		past_limit = c == 0 ? "lzo1x: output longer than its limit" :
		    "nrv2b: output longer than its capacity";
		fill_random(data, 3 * 65536, 11 + c);
		spec = (boot_spec_t){ .body = data, .machine = 42,
		    .compression = c == 0 ? COMPRESS_LZO : COMPRESS_UCL,
		    .chunks = plan, .chunk_count = 2 };

		/* The terminator missing, for each padding length the last
		 * chunk leaves: under a word, the length word itself reaches
		 * the trailer; with two or three nonzero padding bytes, they
		 * are read as a length whose chunk would pass imagefs_size. */
		for (uint32_t last = 5000; last < 5004; last++) {
			plan[0] = (chunk_plan_t){ 4096, FORM_LITERAL };
			plan[1] = (chunk_plan_t){ last, FORM_LITERAL };
			spec.body_bytes = 4096 + last;
			spec.omit_terminator = true;
			spec.pad_byte = 0xa5;
			image = boot_build(&spec);
			pad = image.trailer - image.terminator;
			expect_open(&image, NULL, FSIMG_ECORRUPT, pad < 2 ?
			    "QNX boot image length word reaches the stored trailer" :
			    "QNX boot image chunks continue past imagefs_size");
		}
		spec.omit_terminator = false;
		spec.pad_byte = 0;
		plan[1] = (chunk_plan_t){ 5000, FORM_LITERAL };
		spec.body_bytes = 4096 + 5000;

		/* A nonzero word in the terminator's place, with decoded bytes
		 * still owed: its chunk would reach the trailer. */
		image = boot_build(&spec);
		set_length_word(&image, image.terminator, 0xffff);
		put_le32(image.file.bytes + H_IMAGEFS_SIZE, 4096 + 5000 + 4096);
		expect_open(&image, NULL, FSIMG_ECORRUPT,
		    "QNX boot image chunk reaches the stored trailer");

		/* Nonzero padding; the decoded total short of imagefs_size; a
		 * chunk decoding past it, which its decoder's limit stops. */
		spec.extra_padding = 2;
		image = boot_build(&spec);
		image.file.bytes[image.trailer - 3] = 1;
		expect_open(&image, NULL, FSIMG_ECORRUPT,
		    "QNX boot image padding before the stored trailer is not zero");
		spec.extra_padding = 0;
		image = boot_build(&spec);
		put_le32(image.file.bytes + H_IMAGEFS_SIZE, 4096 + 5000 + 4);
		expect_open(&image, NULL, FSIMG_ECORRUPT,
		    "QNX boot image chunks decode to fewer bytes than imagefs_size");
		image = boot_build(&spec);
		put_le32(image.file.bytes + H_IMAGEFS_SIZE, 4096 + 5000 - 4);
		expect_open(&image, NULL, FSIMG_ECORRUPT, past_limit);

		/* A stream that ends before its stored length does, and one
		 * cut short of its end marker. */
		image = boot_build(&spec);
		set_length_word(&image, image.chunk_offsets[1],
		    length_word(&image, image.chunk_offsets[1]) + 1);
		expect_open(&image, NULL, FSIMG_ECORRUPT, c == 0 ?
		    "lzo1x: trailing bytes after the end marker" :
		    "nrv2b: trailing bytes after the end marker");
		image = boot_build(&spec);
		set_length_word(&image, image.chunk_offsets[1],
		    length_word(&image, image.chunk_offsets[1]) - 1);
		expect_open(&image, NULL, FSIMG_ECORRUPT, c == 0 ?
		    "lzo1x: unterminated stream" : "nrv2b: unterminated stream");

		/* A non-final chunk below 4096 is unobserved: unsupported. */
		spec.chunks = small_first;
		spec.body_bytes = 4095 + 4096;
		image = boot_build(&spec);
		expect_open(&image, NULL, FSIMG_EUNSUPPORTED, "QNX boot image "
		    "non-final chunk decodes to fewer than 4096 bytes");

		/* Chunks of exactly 65536 bytes read; a 65537-byte chunk does
		 * not, whatever imagefs_size allows. */
		spec.chunks = max_runs;
		spec.chunk_count = 3;
		spec.body_bytes = 2 * 65536 + 100;
		image = boot_build(&spec);
		file = boot_file(&image);
		fixture_init(&f, 16 * MIB, 0);
		CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
		/* The generator repeated each run chunk's first byte. */
		memset(data, data[0], 65536);
		memset(data + 65536, data[65536], 65536);
		expect_disk(&f, data, spec.body_bytes);
		fixture_free(&f);
		free(file.bytes);
		spec.chunks = long_run;
		spec.chunk_count = 1;
		spec.body_bytes = 65537;
		image = boot_build(&spec);
		expect_open(&image, NULL, FSIMG_ECORRUPT, past_limit);
	}
	free(data);
}

/*
 * A `dumpifs -u` copy keeps the compression value over the decoded body.
 * Under this profile it is corrupt at the first chunk, and its ImageFS
 * reads as a byte slice of the file instead.
 */
static void
test_decoded_copy(void)
{
	buffer_t body = imagefs_build(DATA_BYTES);
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_RANGE,
	    .offset = 4096, .length = body.length };
	fsimg_reader_t *reader;
	fsimg_io_t *volume;
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	fixture_t f;

	for (uint8_t value = COMPRESS_LZO; value <= COMPRESS_UCL; value += 4) {
		spec = sample_spec(body.bytes, body.length, 0, COMPRESS_NONE);
		image = boot_build(&spec);
		image.file.bytes[H_FLAGS1] = (unsigned char)(value | 0x01);
		boot_reseal(&image);
		file = boot_file(&image);
		fixture_init(&f, 16 * MIB, 0);
		CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_ECORRUPT);
		CHECK(fixture_open(&f, &file, "ifs/-/-", NULL) == FSIMG_OK);
		CHECK(fsimg_image_select(f.image, &selector, &volume, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_reader_open(f.ctx, volume, NULL, &reader, NULL) ==
		    FSIMG_OK);
		fsimg_reader_free(reader);
		fsimg_io_free(volume);
		fixture_free(&f);
		free(file.bytes);
	}
	free(body.bytes);
}
/* Both sums are checked by verification alone, each named. */
static void
test_sums(void)
{
	buffer_t body = imagefs_build(DATA_BYTES);
	boot_spec_t spec;
	boot_image_t image;
	unsigned char *startup;
	uint32_t startup_size;

	for (unsigned c = 0; c < 3; c++) {
		spec = sample_spec(body.bytes, body.length, c == 2 ? BIOS_PREFIX : 0,
		    c == 0 ? COMPRESS_NONE : c == 1 ? COMPRESS_LZO : COMPRESS_UCL);
		image = boot_build(&spec);
		startup = image.file.bytes + image.header;
		startup_size = get_le32(startup + H_STARTUP_SIZE);
		startup[startup_size - 1] ^= 1;
		expect_verify(&image, FSIMG_ECORRUPT, "QNX boot image startup "
		    "checksum: the startup section does not sum to zero");
		image = boot_build(&spec);
		image.file.bytes[image.trailer] ^= 1;
		expect_verify(&image, FSIMG_ECORRUPT, "QNX boot image stored "
		    "checksum: the stored image does not sum to zero");
		/* The opaque boot prefix is no part of either sum. */
		if (spec.prefix != 0) {
			image = boot_build(&spec);
			image.file.bytes[17] ^= 1;
			expect_verify(&image, FSIMG_OK, NULL);
		}
	}
	free(body.bytes);
}

/*
 * A body of minimum-size chunks far longer than its imagefs_size allows
 * fails at the first chunk past imagefs_size, within the index bound: it
 * has read only the chunks it indexed and the scratch the records need.
 */
static void
test_index_bound(void)
{
	static chunk_plan_t chunks[1000];
	unsigned char *data;
	fsimg_error_t error;
	fsimg_stats_t stats;
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	fixture_t f;

	data = malloc(1000 * 4096);
	CHECK(data != NULL);
	fill_random(data, 1000 * 4096, 3);
	for (unsigned i = 0; i < 1000; i++)
		chunks[i] = (chunk_plan_t){ 4096, FORM_LITERAL };
	spec = (boot_spec_t){ .body = data, .body_bytes = 1000 * 4096,
	    .machine = 42, .compression = COMPRESS_LZO, .chunks = chunks,
	    .chunk_count = 1000 };
	image = boot_build(&spec);
	put_le32(image.file.bytes + H_IMAGEFS_SIZE, 3 * 4096);
	boot_reseal(&image);
	file = boot_file(&image);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, NULL, &error) == FSIMG_ECORRUPT);
	expect_message(&error, "QNX boot image chunks continue past imagefs_size");
	CHECK(file.read_bytes < 2 * 4096 + 4 * (4096 + 64));
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_peak < 64 * KIB);
	fixture_free(&f);
	free(file.bytes);
	free(data);
}

/*
 * A sequential read of the disk decodes each chunk once, reading each
 * chunk's stored stream once; reads confined to four chunks decode each
 * once, so the file is read no further after the first pass; an access
 * alternating across five chunks decodes on every read until the work
 * limit stops it.
 */
static void
test_cache_budget(void)
{
	static chunk_plan_t chunks[10];
	unsigned char *data, byte;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_error_t error;
	fsimg_io_t *disk;
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	fixture_t f;
	uint64_t before, streams;
	size_t transferred;
	fsimg_status_t status;

	data = malloc(10 * 4096);
	CHECK(data != NULL);
	fill_random(data, 10 * 4096, 4);
	for (unsigned i = 0; i < 10; i++)
		chunks[i] = (chunk_plan_t){ 4096, FORM_LITERAL };
	spec = (boot_spec_t){ .body = data, .body_bytes = 10 * 4096,
	    .machine = 42, .compression = COMPRESS_LZO, .chunks = chunks,
	    .chunk_count = 10 };
	image = boot_build(&spec);
	streams = 0;
	for (unsigned i = 0; i < 10; i++)
		streams += length_word(&image, image.chunk_offsets[i]);
	file = boot_file(&image);
	fixture_init(&f, 16 * MIB, 0);
	CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
	before = file.read_bytes;
	CHECK(read_whole_disk(&f, NULL) == FSIMG_OK);
	CHECK(file.read_bytes - before == streams);
	fixture_free(&f);
	fixture_init(&f, 16 * MIB, 1 * MIB);
	CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(f.image, &selector, &disk, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(fsimg_io_read_exact(disk, i * 4096 + 7, &byte, 1,
		    &transferred, NULL) == FSIMG_OK);
		CHECK(byte == data[i * 4096 + 7]);
	}
	before = file.read_bytes;
	for (unsigned round = 0; round < 1000; round++) {
		CHECK(fsimg_io_read_exact(disk, (round % 4) * 4096 + round % 4096,
		    &byte, 1, &transferred, NULL) == FSIMG_OK);
		CHECK(byte == data[(round % 4) * 4096 + round % 4096]);
	}
	CHECK(file.read_bytes == before);
	status = FSIMG_OK;
	for (unsigned round = 0; round < 1000 && status == FSIMG_OK; round++)
		status = fsimg_io_read_exact(disk, (round % 5) * 4096, &byte, 1,
		    &transferred, &error);
	CHECK(status == FSIMG_ELIMIT && error.resource == FSIMG_RESOURCE_WORK);
	fsimg_io_free(disk);
	fixture_free(&f);
	free(file.bytes);
	free(data);
}

/*
 * The container carries an ImageFS: a stack that names a scheme or another
 * filesystem with it is refused, named, whether ifsboot is named or
 * detected; inside it only the ImageFS probe runs. The container is read
 * only.
 */
static void
test_carried_stack(void)
{
	static const char *const no_table =
	    "the container holds a volume, not a partition table";
	static const char *const other_fs =
	    "the container holds a different filesystem";
	static const struct {
		const char *stack;
		const char *message;
	} refused[] = {
		{ "fat32/ifsboot", "the container holds a different filesystem" },
		{ "fat12/-/ifsboot", "the container holds a different filesystem" },
		{ "ifs/gpt/ifsboot",
		    "the container holds a volume, not a partition table" },
		{ "ifs/mbr/ifsboot",
		    "the container holds a volume, not a partition table" }
	};
	buffer_t body = imagefs_build(1000);
	fsimg_layer_info_t layer;
	fsimg_write_options_t write;
	fsimg_options_t options;
	fsimg_writer_t *writer;
	memory_file_t bare;
	fsimg_io_ops_t ops;
	fsimg_io_t *input;
	fsimg_sink_t sink;
	fsimg_error_t error;
	boot_spec_t spec;
	boot_image_t image;
	fsimg_ctx_t *ctx;
	size_t cursor;

	/* A named ifsboot, checked with the stack before any read. */
	spec = sample_spec(body.bytes, body.length, 0, COMPRESS_LZO);
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		image = boot_build(&spec);
		expect_open(&image, refused[i].stack, FSIMG_EINVAL,
		    refused[i].message);
	}
	image = boot_build(&spec);
	expect_open(&image, "ifs/-/ifsboot", FSIMG_OK, NULL);

	/* A detected ifsboot under a forced foreign filesystem or scheme. */
	image = boot_build(&spec);
	expect_open(&image, "fat32", FSIMG_EINVAL, other_fs);
	image = boot_build(&spec);
	expect_open(&image, "ifs/gpt", FSIMG_EINVAL, no_table);
	image = boot_build(&spec);
	expect_open(&image, "ifs", FSIMG_OK, NULL);

	/* A stack value built by hand is validated the same way. */
	{
		fsimg_image_options_t options;
		fsimg_image_t *opened;
		memory_file_t file;
		fixture_t f;
		fsimg_io_ops_t ops;

		image = boot_build(&spec);
		file = boot_file(&image);
		fixture_init(&f, 16 * MIB, 0);
		fsimg_io_ops_init(&ops);
		ops.read_at = file_read;
		ops.get_size = file_size;
		CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) ==
		    FSIMG_OK);
		fsimg_image_options_init(&options);
		options.stack.slots[FSIMG_LAYER_FILESYSTEM].state =
		    FSIMG_SLOT_NAMED;
		strcpy(options.stack.slots[FSIMG_LAYER_FILESYSTEM].name, "fat32");
		options.stack.slots[FSIMG_LAYER_CONTAINER].state =
		    FSIMG_SLOT_NAMED;
		strcpy(options.stack.slots[FSIMG_LAYER_CONTAINER].name, "ifsboot");
		CHECK(fsimg_image_open(f.ctx, f.io, &options, &opened, &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "ifsboot") == 0);
		fixture_free(&f);
		free(file.bytes);
	}

	/* The capability record: read, write and verify, the codecs, with no
	 * zlib version since the codecs are the project's own, the carried
	 * filesystem and the settings, file settings first. */
	cursor = 0;
	while (fsimg_layer_next(&cursor, &layer, NULL) == FSIMG_OK &&
	    strcmp(layer.name, "ifsboot") != 0)
		;
	CHECK(strcmp(layer.name, "ifsboot") == 0);
	CHECK(layer.kind == FSIMG_LAYER_CONTAINER);
	CHECK(layer.capabilities ==
	    (FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY));
	CHECK(strcmp(layer.codecs, "raw,lzo1x,nrv2b") == 0);
	CHECK(layer.codec_version[0] == '\0');
	CHECK(layer.suffix[0] == '\0' && strcmp(layer.carries, "ifs") == 0);
	CHECK(strncmp(layer.settings, "startup,prefix,machine,", 23) == 0);

	/* Creating names the container: a foreign filesystem inside it is
	 * invalid, and wrapping an ImageFS in it without the required
	 * settings is refused naming the first. */
	fsimg_options_init(&options);
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&write);
	CHECK(fsimg_writer_new(ctx, "fat32/ifsboot", &write, &writer, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot") == 0);
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	bare.bytes = body.bytes;
	bare.length = body.length;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &bare, &input, NULL) ==
	    FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(sink.stream_fd != -1);
	CHECK(fsimg_image_wrap(ctx, input, "ifs/ifsboot", &write, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot:machine") == 0);
	CHECK(strcmp(error.message, "the setting is required") == 0);
	CHECK(close(sink.stream_fd) == 0);
	fsimg_io_free(input);
	fsimg_ctx_free(ctx);
	free(body.bytes);
}

/* Read the whole disk in pieces; the first failure's status. */
static fsimg_status_t
read_whole_disk(fixture_t *f, fsimg_error_t *error)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char piece[7000];
	fsimg_io_t *disk;
	uint64_t size;
	size_t count, transferred;
	fsimg_status_t status;

	status = fsimg_image_select(f->image, &selector, &disk, error);
	if (status != FSIMG_OK)
		return (status);
	CHECK(fsimg_io_get_size(disk, &size, NULL) == FSIMG_OK);
	for (uint64_t offset = 0; offset < size && status == FSIMG_OK;
	    offset += count) {
		count = size - offset < sizeof(piece) ? (size_t)(size - offset) :
		    sizeof(piece);
		status = fsimg_io_read_exact(disk, offset, piece, count,
		    &transferred, error);
	}
	fsimg_io_free(disk);
	return (status);
}

/* Open, verify with read-through and read the whole disk; the first
 * failure's status.
 */
static fsimg_status_t
open_verify_read(fixture_t *f, memory_file_t *file)
{
	fsimg_layer_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;

	status = fixture_open(f, file, NULL, &error);
	if (status == FSIMG_OK)
		status = fsimg_image_verify(f->image, FSIMG_VERIFY_PAYLOAD,
		    &report, &error);
	if (status == FSIMG_OK)
		status = read_whole_disk(f, &error);
	return (status);
}

/* Every read failing in turn fails the open, verification or reading the
 * disk with the I/O error, and every allocation failing in turn with
 * FSIMG_ENOMEM, the learning pass's buffers and the cache among them;
 * neither leaks anything, as fixture_free() checks.
 */
static void
test_fault_sweep(void)
{
	buffer_t body = imagefs_build(20000);
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	fixture_t f;
	fsimg_status_t status;

	for (unsigned c = 0; c < 3; c++) {
		spec = sample_spec(body.bytes, body.length, c == 2 ? BIOS_PREFIX : 0,
		    c == 0 ? COMPRESS_NONE : c == 1 ? COMPRESS_LZO : COMPRESS_UCL);
		image = boot_build(&spec);
		file = boot_file(&image);
		for (uint64_t attempt = 1;; attempt++) {
			file.reads = 0;
			file.fail_at = attempt;
			fixture_init(&f, 16 * MIB, 0);
			status = open_verify_read(&f, &file);
			fixture_free(&f);
			if (file.reads < attempt) {
				CHECK(status == FSIMG_OK);
				break;
			}
			CHECK(status == FSIMG_EIO);
		}
		file.fail_at = 0;
		for (uint64_t attempt = 1;; attempt++) {
			CHECK(attempt < 10000);
			fixture_init(&f, 16 * MIB, 0);
			fsimg_test_fail_alloc_after(f.ctx, attempt);
			status = open_verify_read(&f, &file);
			fixture_free(&f);
			if (status == FSIMG_OK) {
				CHECK(attempt > 1);
				break;
			}
			CHECK(status == FSIMG_ENOMEM);
		}
		free(file.bytes);
	}
	free(body.bytes);
}

/*
 * The learned index describes the bytes the learning pass read. A chunk
 * whose stream changes after the open no longer decodes to its record:
 * the cache's decode fails corrupt rather than serving other bytes.
 */
static void
test_changed_source(void)
{
	/* Large enough for at least two chunks under either codec's plan. */
	buffer_t body = imagefs_build(100000);
	fsimg_error_t error;
	boot_spec_t spec;
	boot_image_t image;
	memory_file_t file;
	uint64_t stream;
	size_t length;
	fixture_t f;

	for (uint8_t value = COMPRESS_LZO; value <= COMPRESS_UCL; value += 4) {
		spec = sample_spec(body.bytes, body.length, 0, value);
		image = boot_build(&spec);
		/* The second chunk's stream is the one changed. */
		CHECK(image.chunks >= 2);
		stream = image.chunk_offsets[1] + 2;
		length = length_word(&image, image.chunk_offsets[1]);
		file = boot_file(&image);
		fixture_init(&f, 16 * MIB, 0);
		CHECK(fixture_open(&f, &file, NULL, NULL) == FSIMG_OK);
		bzero(file.bytes + stream, length);
		CHECK(read_whole_disk(&f, &error) == FSIMG_ECORRUPT);
		fixture_free(&f);
		free(file.bytes);
	}
	free(body.bytes);
}

static void
write_file(const char *path, const void *bytes, size_t length)
{
	FILE *out;

	out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite(bytes, 1, length, out) == length);
	CHECK(fclose(out) == 0);
}

/* A small boot image for the container fuzzer's corpus. */
static int
seed(const char *path, uint8_t compression)
{
	buffer_t body = imagefs_build(9000);
	/* A literal and a run chunk of 4096 bytes, then the rest literal. */
	chunk_plan_t plan[] = { { 4096, FORM_LITERAL }, { 4096, FORM_RUN },
	    { (uint32_t)body.length - 8192, FORM_LITERAL } };
	boot_spec_t spec = { .body = body.bytes, .body_bytes = body.length,
	    .machine = 42, .compression = compression, .virtual_bit = true,
	    .chunks = plan, .chunk_count = 3 };
	boot_image_t image;

	image = boot_build(&spec);
	write_file(path, image.file.bytes, image.file.length);
	free(image.file.bytes);
	free(body.bytes);
	return (0);
}

/* The integration script's images: the bare body and a boot image of it
 * for each prefix and compression value.
 */
static int
emit(const char *directory)
{
	static const struct {
		const char *name;
		uint32_t prefix;
		uint8_t compression;
	} images[] = {
		{ "raw.ifs", 0, COMPRESS_NONE }, { "lzo.ifs", 0, COMPRESS_LZO },
		{ "ucl.ifs", 0, COMPRESS_UCL },
		{ "bios-raw.ifs", BIOS_PREFIX, COMPRESS_NONE },
		{ "bios-lzo.ifs", BIOS_PREFIX, COMPRESS_LZO },
		{ "bios-ucl.ifs", BIOS_PREFIX, COMPRESS_UCL }
	};
	buffer_t body = imagefs_build(DATA_BYTES);
	char path[1024];
	boot_spec_t spec;
	boot_image_t image;

	snprintf(path, sizeof(path), "%s/body.imagefs", directory);
	write_file(path, body.bytes, body.length);
	for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
		spec = sample_spec(body.bytes, body.length, images[i].prefix,
		    images[i].compression);
		image = boot_build(&spec);
		snprintf(path, sizeof(path), "%s/%s", directory, images[i].name);
		write_file(path, image.file.bytes, image.file.length);
		free(image.file.bytes);
	}
	free(body.bytes);
	return (0);
}

static unsigned char *
read_whole(const char *path, size_t *length)
{
	unsigned char *bytes;
	struct stat st;
	FILE *in;

	in = fopen(path, "rb");
	if (in == NULL)
		return (NULL);
	CHECK(fstat(fileno(in), &st) == 0);
	*length = (size_t)st.st_size;
	bytes = malloc(*length + 1);
	CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, *length, in) == *length);
	CHECK(fclose(in) == 0);
	return (bytes);
}

/* Open a native file through the library and report the outcome: the
 * status of opening, of layer verification and of the volume's listing and
 * verification, and the entry count. A disk the caller supplies must read
 * as those bytes.
 */
static native_result_t
native_read(unsigned char *bytes, size_t length, const unsigned char *decoded,
    size_t decoded_length)
{
	memory_file_t file = { .bytes = bytes, .length = length };
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	native_result_t result;
	fixture_t f;

	bzero(&result, sizeof(result));
	fixture_init(&f, 64 * MIB, 0);
	f.spool = true;
	result.open = fixture_open(&f, &file, NULL, NULL);
	/* Info fails on a file no container claims that holds no volume. */
	if (result.open == FSIMG_OK &&
	    fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK)
		result.claimed = strcmp(info.container, "ifsboot") == 0;
	if (result.claimed) {
		result.chunks = info.chunk_count;
		result.tail_nonzero = detail(&info, "tail_nonzero");
		snprintf(result.codecs, sizeof(result.codecs), "%s", info.codecs);
		if (decoded != NULL)
			expect_disk(&f, decoded, decoded_length);
		result.verify = fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD,
		    &report, NULL);
		if (result.verify == FSIMG_OK)
			result.entries = list_volume(&f, NULL, 0);
	}
	fixture_free(&f);
	return (result);
}

/*
 * The native evidence. Under FSIMG_IFS_NATIVE: the E7 nonbootable LZO and
 * UCL images open, verify and list alike, and the big-endian one draws no
 * claim; the E10 disk's compressed slots 12 to 15 are the unchanged image,
 * a flipped chunk byte, a flipped stored trailer and a flipped header-tail
 * byte, which fail with their classes. Under FSIMG_IFS_IMAGES with
 * FSIMG_IFS_NATIVE: each boot image the decoded/ directory has a body for
 * (h3-NAME.imagefs or x1-NAME.imagefs) reads, without any decoding step of
 * the caller's, as that body byte for byte, verifies, and lists, and its
 * header's tail holds six nonzero bytes in an H3 image and none in an X1
 * one. Only counts and file names are printed.
 */
static int
native(void)
{
	/* The E7 images and the entries their buildfiles name (the native
	 * dumpifs transcript lists the same counts). */
	static const struct {
		const char *name;
		const char *codecs;
		uint64_t entries;
	} e7[] = {
		{ "e7-compress-2.ifs", "lzo1x", 2 },
		{ "e7-compress-3.ifs", "nrv2b", 2 },
		{ "e7-compress-plus.ifs", "nrv2b", 1 }
	};
	const char *captures = getenv("FSIMG_IFS_NATIVE");
	const char *images = getenv("FSIMG_IFS_IMAGES");
	unsigned char *bytes, *decoded, *slot;
	native_result_t result, first;
	char path[2048];
	struct dirent *dirent;
	size_t length, decoded_length;
	unsigned read_images = 0;
	DIR *directory;
	bool h3;
	int status = 77;

	if (captures == NULL || captures[0] == '\0') {
		printf("SKIP: FSIMG_IFS_NATIVE is not set\n");
		return (77);
	}
	bzero(&first, sizeof(first));
	for (size_t i = 0; i < sizeof(e7) / sizeof(e7[0]); i++) {
		snprintf(path, sizeof(path), "%s/cases/ifs-extra/%s", captures,
		    e7[i].name);
		bytes = read_whole(path, &length);
		CHECK(bytes != NULL);
		result = native_read(bytes, length, NULL, 0);
		free(bytes);
		CHECK(result.claimed && result.verify == FSIMG_OK);
		CHECK(strcmp(result.codecs, e7[i].codecs) == 0);
		CHECK(result.entries == e7[i].entries);
		if (i == 0)
			first = result;
		printf("%s: %llu chunks, %llu entries, verified\n", e7[i].name,
		    (unsigned long long)result.chunks,
		    (unsigned long long)result.entries);
	}
	snprintf(path, sizeof(path), "%s/cases/ifs-extra/e7-compress-2-be.ifs",
	    captures);
	bytes = read_whole(path, &length);
	CHECK(bytes != NULL);
	result = native_read(bytes, length, NULL, 0);
	CHECK(!result.claimed);
	free(bytes);
	printf("e7-compress-2-be.ifs: no claim, as a big-endian header\n");

	snprintf(path, sizeof(path), "%s/inputs/e10.img", captures);
	bytes = read_whole(path, &length);
	CHECK(bytes != NULL && length >= 16 * SLOT_BYTES);
	for (unsigned i = 12; i < 16; i++) {
		slot = bytes + (size_t)i * SLOT_BYTES;
		result = native_read(slot, E10_BOOT_BYTES, NULL, 0);
		switch (i) {
		case 12:
			CHECK(result.claimed && result.verify == FSIMG_OK);
			CHECK(result.entries == first.entries);
			break;
		case 13:
			/* The learning pass decodes the flipped chunk. */
			CHECK(result.open == FSIMG_ECORRUPT);
			break;
		default:
			CHECK(result.claimed && result.verify == FSIMG_ECORRUPT);
			break;
		}
		printf("e10 slot %u: open %d, verify %d\n", i, result.open,
		    result.verify);
	}
	free(bytes);
	status = 0;

	if (images == NULL || images[0] == '\0') {
		printf("SKIP: FSIMG_IFS_IMAGES is not set\n");
		return (status);
	}
	directory = opendir(images);
	CHECK(directory != NULL);
	while ((dirent = readdir(directory)) != NULL) {
		const char *name = dirent->d_name;
		size_t name_length = strlen(name);

		if (name_length < 5 || strcmp(name + name_length - 4, ".ifs") != 0)
			continue;
		decoded = NULL;
		h3 = false;
		for (unsigned family = 0; family < 2 && decoded == NULL; family++) {
			snprintf(path, sizeof(path), "%s/decoded/%s-%.*s.imagefs",
			    captures, family == 0 ? "h3" : "x1",
			    (int)(name_length - 4), name);
			decoded = read_whole(path, &decoded_length);
			h3 = family == 0;
		}
		if (decoded == NULL)
			continue;
		snprintf(path, sizeof(path), "%s/%s", images, name);
		bytes = read_whole(path, &length);
		CHECK(bytes != NULL);
		result = native_read(bytes, length, decoded, decoded_length);
		if (result.open == FSIMG_OK && !result.claimed) {
			/* Another container's image, such as hbcifs's. */
			free(bytes);
			free(decoded);
			continue;
		}
		CHECK(result.claimed && result.verify == FSIMG_OK);
		CHECK(result.tail_nonzero == (h3 ? 6 : 0));
		printf("%s: %s, %llu chunks, %zu decoded bytes as the oracle's, "
		    "%llu entries, verified\n", name, result.codecs,
		    (unsigned long long)result.chunks, decoded_length,
		    (unsigned long long)result.entries);
		free(bytes);
		free(decoded);
		read_images++;
	}
	CHECK(closedir(directory) == 0);
	printf("native boot images read directly: %u\n", read_images);
	return (status);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed-lzo") == 0)
		return (seed(argv[2], COMPRESS_LZO));
	if (argc == 3 && strcmp(argv[1], "--seed-ucl") == 0)
		return (seed(argv[2], COMPRESS_UCL));
	if (argc == 3 && strcmp(argv[1], "--seed-raw") == 0)
		return (seed(argv[2], COMPRESS_NONE));
	if (argc == 3 && strcmp(argv[1], "--emit") == 0)
		return (emit(argv[2]));
	if (argc == 2 && strcmp(argv[1], "--native") == 0)
		return (native());
	CHECK(argc == 1);
	test_each_prefix_and_codec();
	test_probe_offsets();
	test_header_tables();
	test_header_bounds();
	test_framing();
	test_decoded_copy();
	test_sums();
	test_index_bound();
	test_cache_budget();
	test_carried_stack();
	test_fault_sweep();
	test_changed_source();
	return (0);
}
