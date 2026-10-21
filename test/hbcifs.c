/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
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
#include "format.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

/*
 * The hbcifs container (specs/design-layers.md, the hbcifs profile). This
 * file builds its own containers without the library: a small ImageFS, an
 * LZO1X stream written instruction by instruction from the stream
 * description in specs/design-codecs.md (literal runs, long and far
 * matches and the end marker, placed where the test says, never searched
 * for), and the 64-byte header around it, so every header field and every
 * stream byte can be set or damaged. Each passing case has seeded failures
 * beside it that must fail with the named error. The one native image is
 * compared with its oracle decode in test/ifs-integration.sh, through
 * --cksum below.
 */
#define KIB		1024u
#define MIB		(1024u * KIB)

#define HEADER_BYTES	64u
/* The test's values of the two fields reported but never checked. */
#define FIELD_14	0x1234abcdu
#define FIELD_1C	0x89abcdefu
/* The bytes the closed table requires at 0x18. */
static const unsigned char field_18[4] = { 2, 1, 1, 0 };

/* The ImageFS's member file: FILE_BYTES bytes repeating every
 * FILE_PERIOD, which a far match copies across the decoder's windows.
 */
#define FILE_BYTES	(300u * KIB)
#define FILE_PERIOD	40000u

typedef struct buffer {
	unsigned char	*bytes;
	size_t		 length;
	size_t		 capacity;
} buffer_t;

/* One instruction run of a stream: literals copied from the plain bytes,
 * or a match of length bytes from distance bytes back.
 */
typedef struct segment {
	size_t		 distance;	/* Zero for literals */
	size_t		 length;
} segment_t;

/* A container under construction: the plain bytes it decodes to, the
 * stream, and the header fields, which tests may set to any value.
 */
typedef struct container {
	unsigned char	*plain;
	size_t		 plain_length;
	buffer_t	 stream;
	unsigned char	 header[HEADER_BYTES];
} container_t;

/* A file read through callbacks; reads after fail_at fail with EIO, and
 * piece, when nonzero, caps each callback's transfer.
 */
typedef struct memory_file {
	const unsigned char *bytes;
	size_t		 length;
	uint64_t	 reads;
	uint64_t	 fail_at;
	size_t		 piece;
} memory_file_t;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_image_t	*image;
	uint64_t	 baseline_memory;
	uint64_t	 fail_allocation;	/* Injected into the open; 0 none */
	char		 directory[PATH_MAX];
	int		 directory_fd;
} fixture_t;

static void append(buffer_t *, const void *, size_t);
static void append_byte(buffer_t *, unsigned);
static void put_le16(unsigned char *, uint32_t);
static void put_le32(unsigned char *, uint32_t);
static uint32_t get_le32(const unsigned char *);
static unsigned char *imagefs_build(size_t *);
static void lzo_extension(buffer_t *, size_t);
static void lzo_literals(buffer_t *, const unsigned char *, size_t);
static void lzo_match(buffer_t *, size_t, size_t);
static void lzo_end(buffer_t *);
static void container_init(container_t *, unsigned char *, size_t,
    const segment_t *, size_t);
static void container_imagefs(container_t *);
static void container_zeros(container_t *, size_t);
static buffer_t container_file(const container_t *);
static void container_free(container_t *);
static buffer_t gzip_of(const buffer_t *);
static fsimg_status_t file_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t file_size(void *, uint64_t *, fsimg_error_t *);
static void fixture_init(fixture_t *, uint64_t, uint64_t);
static void fixture_free(fixture_t *);
static int spool_new(fixture_t *);
static fsimg_status_t open_file(fixture_t *, memory_file_t *, const char *,
    int, fsimg_error_t *);
static fsimg_status_t open_buffer(fixture_t *, memory_file_t *,
    const buffer_t *, const char *, fsimg_error_t *);
static void expect_disk(fixture_t *, const unsigned char *, size_t);
static void expect_disk_bytes(const buffer_t *, const unsigned char *, size_t);
static fsimg_error_t expect_failure(const buffer_t *, fsimg_status_t,
    const char *);
static fsimg_error_t expect_variant(const container_t *, uint32_t, size_t,
    size_t, fsimg_status_t, const char *);
static uint32_t cksum_update(uint32_t, const unsigned char *, size_t);
static void test_open(void);
static void test_header(void);
static void test_stream(void);
static void test_carried(void);
static void test_spool_limit(void);
static void test_second_spool(void);
static void test_work_limit(void);
static void test_history_reach(void);
static void test_profile_facts(void);
static void test_faults(void);
static void test_capability(void);
static int disk_cksum(const char *);
static void write_file(const char *, const unsigned char *, size_t);
static int write_seed(const char *, const char *);

static void
append(buffer_t *buffer, const void *bytes, size_t length)
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
append_byte(buffer_t *buffer, unsigned byte)
{
	unsigned char value = (unsigned char)byte;

	append(buffer, &value, 1);
}

static void
put_le16(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

static void
put_le32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t
get_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

/*
 * A little-endian ImageFS as mkifs lays one out (specs/design-ifs.md): the
 * 88-byte header with the mountpoint "/", a root directory record, a file
 * "data" of FILE_BYTES bytes repeating every FILE_PERIOD, a link "link" to
 * it, the table's zero terminator, the file's bytes, zero padding to a
 * multiple of 4 and the trailer that makes the 32-bit word sum zero.
 */
static unsigned char *
imagefs_build(size_t *length)
{
	enum { DIR_OFFSET = 0x5c, ROOT = DIR_OFFSET, FILE_RECORD = ROOT + 0x1c,
	    LINK_RECORD = FILE_RECORD + 0x28, TABLE_END = LINK_RECORD + 0x28,
	    DATA = TABLE_END + 4 };
	unsigned char *image;
	uint32_t image_size, sum;

	image_size = (uint32_t)((DATA + FILE_BYTES + 3) & ~(size_t)3) + 4;
	image = calloc(1, image_size);
	CHECK(image != NULL);
	memcpy(image, "imagefs", 7);
	image[0x07] = 0x04;
	put_le32(image + 0x08, image_size);
	put_le32(image + 0x0c, DATA);
	put_le32(image + 0x10, DIR_OFFSET);
	image[0x58] = '/';

	put_le16(image + ROOT, 0x1c);
	put_le32(image + ROOT + 0x04, 1);
	put_le32(image + ROOT + 0x08, 0040755);
	put_le32(image + ROOT + 0x14, 0x6ad7c93a);

	put_le16(image + FILE_RECORD, 0x28);
	put_le32(image + FILE_RECORD + 0x04, 2);
	put_le32(image + FILE_RECORD + 0x08, 0100644);
	put_le32(image + FILE_RECORD + 0x14, 0x6ad7c93a);
	put_le32(image + FILE_RECORD + 0x18, DATA);
	put_le32(image + FILE_RECORD + 0x1c, FILE_BYTES);
	memcpy(image + FILE_RECORD + 0x20, "data", 4);

	put_le16(image + LINK_RECORD, 0x28);
	put_le32(image + LINK_RECORD + 0x04, 3);
	put_le32(image + LINK_RECORD + 0x08, 0120777);
	put_le32(image + LINK_RECORD + 0x14, 0x6ad7c93a);
	put_le16(image + LINK_RECORD + 0x18, 5);
	put_le16(image + LINK_RECORD + 0x1a, 4);
	memcpy(image + LINK_RECORD + 0x1c, "link", 4);
	memcpy(image + LINK_RECORD + 0x21, "data", 4);

	for (size_t i = 0; i < FILE_BYTES; i++)
		image[DATA + i] = (unsigned char)((i % FILE_PERIOD) * 7 +
		    (i % FILE_PERIOD) / 251);
	sum = 0;
	for (uint32_t o = 0; o + 4 < image_size; o += 4)
		sum += get_le32(image + o);
	put_le32(image + image_size - 4, 0 - sum);
	*length = image_size;
	return (image);
}

/* A length extension: one zero byte per 255, then the rest, 1 to 255. */
static void
lzo_extension(buffer_t *stream, size_t value)
{
	size_t zeros;

	CHECK(value >= 1);
	zeros = (value - 1) / 255;
	for (size_t i = 0; i < zeros; i++)
		append_byte(stream, 0);
	append_byte(stream, (unsigned)(value - 255 * zeros));
}

/*
 * A literal run with no literals before it, at the stream's start or after
 * a match that carries none: 0000 LLLL for 3 + L literals, or a zero byte
 * and an extension for 18 or more.
 */
static void
lzo_literals(buffer_t *stream, const unsigned char *bytes, size_t count)
{
	CHECK(count >= 4);
	if (count <= 18) {
		append_byte(stream, (unsigned)(count - 3));
	} else {
		append_byte(stream, 0);
		lzo_extension(stream, count - 18);
	}
	append(stream, bytes, count);
}

/*
 * A match carrying no literals: 001L LLLL, from 1 + D for a distance up to
 * 16384, or 0001 HLLL from 16384 + 16384H + D up to 49151, each with the
 * little-endian word 4D after its length.
 */
static void
lzo_match(buffer_t *stream, size_t distance, size_t length)
{
	size_t value;

	CHECK(length >= 3 && distance >= 1 && distance <= 49151);
	if (distance <= 16384) {
		value = distance - 1;
		if (length <= 33) {
			append_byte(stream, (unsigned)(0x20 | (length - 2)));
		} else {
			append_byte(stream, 0x20);
			lzo_extension(stream, length - 33);
		}
	} else {
		value = distance - 16384;
		if (length <= 9) {
			append_byte(stream, (unsigned)(0x10 | (value >> 14) << 3 |
			    (length - 2)));
		} else {
			append_byte(stream, (unsigned)(0x10 | (value >> 14) << 3));
			lzo_extension(stream, length - 9);
		}
		value &= 0x3fff;
	}
	append_byte(stream, (unsigned)((4 * value) & 0xff));
	append_byte(stream, (unsigned)((4 * value) >> 8));
}

/* The end marker: a far match at distance 16384. */
static void
lzo_end(buffer_t *stream)
{
	append_byte(stream, 0x11);
	append_byte(stream, 0);
	append_byte(stream, 0);
}

/*
 * A container of plain, which it adopts, encoded by segments in order and
 * ended; each match must copy what plain holds. The header carries the
 * closed table's values and the test's values of the unchecked fields.
 */
static void
container_init(container_t *c, unsigned char *plain, size_t length,
    const segment_t *segments, size_t count)
{
	size_t position = 0;

	bzero(c, sizeof(*c));
	c->plain = plain;
	c->plain_length = length;
	for (size_t i = 0; i < count; i++) {
		if (segments[i].distance == 0) {
			lzo_literals(&c->stream, plain + position,
			    segments[i].length);
		} else {
			CHECK(segments[i].distance <= position);
			for (size_t j = 0; j < segments[i].length; j++)
				CHECK(plain[position + j] == plain[position + j -
				    segments[i].distance]);
			lzo_match(&c->stream, segments[i].distance,
			    segments[i].length);
		}
		position += segments[i].length;
	}
	CHECK(position == length);
	lzo_end(&c->stream);
	memcpy(c->header, "hbcifs\0", 8);
	put_le32(c->header + 0x08, (uint32_t)length);
	put_le32(c->header + 0x0c, (uint32_t)c->stream.length);
	put_le32(c->header + 0x10, 32);
	put_le32(c->header + 0x14, FIELD_14);
	memcpy(c->header + 0x18, field_18, 4);
	put_le32(c->header + 0x1c, FIELD_1C);
}

/* The test ImageFS: literals up to the file's first period, one far match
 * for the rest of the file, and literals for the padding and trailer.
 */
static void
container_imagefs(container_t *c)
{
	unsigned char *plain;
	size_t length, data;

	plain = imagefs_build(&length);
	data = get_le32(plain + 0x0c);
	segment_t segments[] = {
		{ .length = data + FILE_PERIOD },
		{ .distance = FILE_PERIOD, .length = FILE_BYTES - FILE_PERIOD },
		{ .length = length - data - FILE_BYTES }
	};
	container_init(c, plain, length, segments,
	    sizeof(segments) / sizeof(segments[0]));
}

/* length zero bytes: four literals, then matches at distance 1 of at most
 * 16 MiB each, so the stream is about length / 255 bytes.
 */
static void
container_zeros(container_t *c, size_t length)
{
	segment_t *segments;
	size_t count, left;

	CHECK(length > 4);
	segments = calloc(length / (16 * MIB) + 2, sizeof(*segments));
	CHECK(segments != NULL);
	segments[0].length = 4;
	count = 1;
	for (left = length - 4; left != 0; left -= segments[count++].length) {
		segments[count].distance = 1;
		segments[count].length = left < 16 * MIB ? left : 16 * MIB;
	}
	container_init(c, calloc(1, length), length, segments, count);
	CHECK(c->plain != NULL);
	free(segments);
}

/* The container file: the header, then the stream. */
static buffer_t
container_file(const container_t *c)
{
	buffer_t file = { 0 };

	append(&file, c->header, HEADER_BYTES);
	append(&file, c->stream.bytes, c->stream.length);
	return (file);
}

static void
container_free(container_t *c)
{
	free(c->plain);
	free(c->stream.bytes);
	bzero(c, sizeof(*c));
}

/* One gzip member of the file's bytes, as RFC 1952 lays it out. */
static buffer_t
gzip_of(const buffer_t *file)
{
	static const unsigned char header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0,
	    0, 0, 3 };
	unsigned char trailer[8];
	buffer_t out = { 0 };
	z_stream stream;
	unsigned char *deflated;
	size_t bound;

	append(&out, header, sizeof(header));
	bzero(&stream, sizeof(stream));
	CHECK(deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8,
	    Z_DEFAULT_STRATEGY) == Z_OK);
	bound = deflateBound(&stream, (uLong)file->length);
	deflated = malloc(bound);
	CHECK(deflated != NULL);
	stream.next_in = file->bytes;
	stream.avail_in = (uInt)file->length;
	stream.next_out = deflated;
	stream.avail_out = (uInt)bound;
	CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	append(&out, deflated, bound - stream.avail_out);
	CHECK(deflateEnd(&stream) == Z_OK);
	free(deflated);
	put_le32(trailer, (uint32_t)crc32(0, file->bytes, (uInt)file->length));
	put_le32(trailer + 4, (uint32_t)file->length);
	append(&out, trailer, sizeof(trailer));
	return (out);
}

static fsimg_status_t
file_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_file_t *file = argument;

	CHECK(offset <= file->length && length <= file->length - offset);
	if (++file->reads == file->fail_at)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected read failure"));
	if (file->piece != 0 && length > file->piece)
		length = file->piece;
	memcpy(buffer, file->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
file_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((memory_file_t *)argument)->length;
	return (FSIMG_OK);
}

/* A context with a 16 MiB memory budget and the given spool and work
 * limits, zero selecting the defaults.
 */
static void
fixture_init(fixture_t *f, uint64_t spool_bytes, uint64_t max_work)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-hbcifs.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * MIB;
	if (spool_bytes != 0)
		options.spool_bytes = spool_bytes;
	if (max_work != 0)
		options.max_work = max_work;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Everything the image charged, the spool included, is released with it. */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline_memory);
	CHECK(stats.descriptors == 0);
	CHECK(stats.spool_bytes == 0 && stats.spool_physical_bytes == 0);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->directory_fd) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* A spool as the coordinator makes one: exclusive, empty, unlinked. */
static int
spool_new(fixture_t *f)
{
	int fd;

	fd = openat(f->directory_fd, "spool", O_RDWR | O_CREAT | O_EXCL |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(unlinkat(f->directory_fd, "spool", 0) == 0);
	return (fd);
}

static fsimg_status_t
open_file(fixture_t *f, memory_file_t *file, const char *stack, int spool,
    fsimg_error_t *error)
{
	fsimg_image_options_t options;
	fsimg_io_ops_t ops;
	fsimg_status_t status;

	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, file, &f->io, NULL) == FSIMG_OK);
	fsimg_image_options_init(&options);
	options.spool_fd = spool;
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	fsimg_test_fail_alloc_after(f->ctx, f->fail_allocation);
	status = fsimg_image_open(f->ctx, f->io, &options, &f->image, error);
	fsimg_test_fail_alloc_after(f->ctx, 0);
	return (status);
}

/* Open a container file with a fresh spool. */
static fsimg_status_t
open_buffer(fixture_t *f, memory_file_t *file, const buffer_t *bytes,
    const char *stack, fsimg_error_t *error)
{
	*file = (memory_file_t){ .bytes = bytes->bytes, .length = bytes->length };
	return (open_file(f, file, stack, spool_new(f), error));
}

/* The opened image's disk holds exactly the expected bytes. */
static void
expect_disk(fixture_t *f, const unsigned char *expected, size_t length)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	unsigned char *bytes;
	fsimg_io_t *disk;
	uint64_t size;
	size_t transferred;

	CHECK(fsimg_image_select(f->image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(disk, &size, NULL) == FSIMG_OK && size == length);
	bytes = malloc(length == 0 ? 1 : length);
	CHECK(bytes != NULL);
	CHECK(fsimg_io_read_exact(disk, 0, bytes, length, &transferred, NULL) ==
	    FSIMG_OK);
	CHECK(memcmp(bytes, expected, length) == 0);
	free(bytes);
	fsimg_io_free(disk);
}

/* The container file opens with a fresh spool, its disk exactly plain. */
static void
expect_disk_bytes(const buffer_t *bytes, const unsigned char *plain,
    size_t length)
{
	memory_file_t file;
	fixture_t f;

	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, bytes, NULL, NULL) == FSIMG_OK);
	expect_disk(&f, plain, length);
	fixture_free(&f);
}

/* The container file fails to open with status, naming hbcifs, and with a
 * message holding text; returns the error for further checks.
 */
static fsimg_error_t
expect_failure(const buffer_t *bytes, fsimg_status_t status, const char *text)
{
	memory_file_t file;
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, bytes, NULL, &error) == status);
	if (strstr(error.message, text) == NULL)
		fprintf(stderr, "expected \"%s\" in \"%s\"\n", text,
		    error.message);
	CHECK(strstr(error.message, text) != NULL);
	CHECK(strcmp(error.path, "hbcifs") == 0 &&
	    error.operation == FSIMG_OP_IMAGE);
	fixture_free(&f);
	return (error);
}

/*
 * expect_failure on a variant of the container file: the header declares
 * decoded bytes, the payload is the stream's first stream_length bytes
 * followed by extra zero bytes, and the payload length matches it.
 */
static fsimg_error_t
expect_variant(const container_t *c, uint32_t decoded, size_t stream_length,
    size_t extra, fsimg_status_t status, const char *text)
{
	unsigned char header[HEADER_BYTES];
	buffer_t file = { 0 };
	fsimg_error_t error;

	CHECK(stream_length <= c->stream.length);
	bcopy(c->header, header, HEADER_BYTES);
	put_le32(header + 0x08, decoded);
	put_le32(header + 0x0c, (uint32_t)(stream_length + extra));
	append(&file, header, HEADER_BYTES);
	append(&file, c->stream.bytes, stream_length);
	for (size_t i = 0; i < extra; i++)
		append_byte(&file, 0);
	error = expect_failure(&file, status, text);
	free(file.bytes);
	return (error);
}

/*
 * A container of the test ImageFS opens through detection alone: the
 * stack reads ifs/hbcifs, info reports the lengths and header fields, the
 * disk is the ImageFS byte for byte, decoded across the decoder's 64 KiB
 * windows by a match reaching 40000 bytes back, and charged to the spool.
 * The reader lists its members and reads the file.
 * Verification names the two unidentified fields and is incomplete; the
 * unverified fields are reported, never checked, so other values open.
 */
static void
test_open(void)
{
	static const char *const names[] = { "decoded_bytes", "payload_bytes",
	    "field_0x10", "field_0x14", "field_0x18", "field_0x1c" };
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_read_options_t read_options;
	fsimg_selector_t automatic = { .kind = FSIMG_SELECT_AUTOMATIC };
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader;
	fsimg_stats_t stats;
	fsimg_io_t *volume;
	unsigned char *bytes;
	memory_file_t file;
	container_t c;
	buffer_t bytes_file;
	fixture_t f;
	uint64_t values[6];
	size_t transferred;
	unsigned entries;

	container_imagefs(&c);
	bytes_file = container_file(&c);
	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, &bytes_file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.stack, "ifs/hbcifs") == 0);
	CHECK(strcmp(info.container, "hbcifs") == 0);
	CHECK(strcmp(info.codecs, "lzo1x") == 0 && info.chunk_count == 1);
	CHECK(info.compression[0] == '\0' && info.scheme[0] == '\0');
	CHECK(info.file_bytes == bytes_file.length);
	CHECK(info.disk_bytes == c.plain_length);
	values[0] = c.plain_length;
	values[1] = c.stream.length;
	values[2] = 32;
	values[3] = FIELD_14;
	values[4] = get_le32(field_18);
	values[5] = FIELD_1C;
	CHECK(info.detail_count == 6);
	for (unsigned i = 0; i < 6; i++)
		CHECK(strcmp(info.details[i].name, names[i]) == 0 &&
		    info.details[i].value == values[i]);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.spool_bytes == c.plain_length);
	expect_disk(&f, c.plain, c.plain_length);

	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK);
	CHECK(strcmp(report.container, "hbcifs") == 0);
	CHECK(strcmp(report.unverified, "0x14,0x1c") == 0 && !report.complete);
	CHECK(report.scope == FSIMG_VALIDATION_STRUCTURAL);

	CHECK(fsimg_image_select(f.image, &automatic, &volume, NULL) == FSIMG_OK);
	fsimg_read_options_init(&read_options);
	CHECK(fsimg_reader_open(f.ctx, volume, &read_options, &reader, NULL) ==
	    FSIMG_OK);
	bytes = malloc(FILE_BYTES);
	CHECK(bytes != NULL);
	entries = 0;
	while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
		entries++;
		if (strcmp(entry->path, "/data") != 0)
			continue;
		CHECK(entry->kind == FSIMG_ENTRY_FILE && entry->size == FILE_BYTES);
		CHECK(fsimg_reader_read_at(reader, 0, bytes, FILE_BYTES,
		    &transferred, NULL) == FSIMG_OK && transferred == FILE_BYTES);
		CHECK(memcmp(bytes, c.plain + get_le32(c.plain + 0x0c),
		    FILE_BYTES) == 0);
	}
	CHECK(entries == 2);	/* The file and the link; the root is no entry */
	free(bytes);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.stack, "ifs/hbcifs") == 0);
	fixture_free(&f);

	/* Forced, alone or with the carried filesystem. */
	for (unsigned i = 0; i < 2; i++) {
		fixture_init(&f, 0, 0);
		CHECK(open_buffer(&f, &file, &bytes_file, i == 0 ? "ifs/-/hbcifs" :
		    "ifs", NULL) == FSIMG_OK);
		expect_disk(&f, c.plain, c.plain_length);
		fixture_free(&f);
	}

	/* The unchecked fields may hold anything. */
	put_le32(bytes_file.bytes + 0x14, 0);
	put_le32(bytes_file.bytes + 0x1c, UINT32_MAX);
	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, &bytes_file, NULL, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.details[3].value == 0 && info.details[5].value == UINT32_MAX);
	fixture_free(&f);
	free(bytes_file.bytes);
	container_free(&c);
}

/*
 * The header. The payload length must be the file length minus 64 and
 * the decoded length nonzero, else the claim is corrupt; a header cut
 * short is corrupt; the fields the
 * closed table names (0x10, 0x18 and the zero bytes from 0x20) are
 * unsupported when they differ, naming the field. Without the identity
 * there is no claim, and forcing hbcifs then fails as an absent signature.
 */
static void
test_header(void)
{
	static const struct {
		size_t		 offset;
		const char	*text;
	} closed[] = {
		{ 0x10, "0x10" }, { 0x13, "0x10" }, { 0x18, "0x18" },
		{ 0x1b, "0x18" }, { 0x20, "reserved" }, { 0x3f, "reserved" }
	};
	fsimg_image_info_t info;
	memory_file_t file;
	fsimg_error_t error;
	container_t c;
	buffer_t bytes;
	fixture_t f;
	uint32_t payload;

	container_imagefs(&c);
	bytes = container_file(&c);
	payload = get_le32(bytes.bytes + 0x0c);
	put_le32(bytes.bytes + 0x0c, payload + 1);
	expect_failure(&bytes, FSIMG_ECORRUPT, "payload length");
	put_le32(bytes.bytes + 0x0c, payload - 1);
	expect_failure(&bytes, FSIMG_ECORRUPT, "payload length");
	put_le32(bytes.bytes + 0x0c, payload);
	put_le32(bytes.bytes + 0x08, 0);
	expect_failure(&bytes, FSIMG_ECORRUPT, "decoded length is zero");
	put_le32(bytes.bytes + 0x08, (uint32_t)c.plain_length);
	for (size_t i = 0; i < sizeof(closed) / sizeof(closed[0]); i++) {
		bytes.bytes[closed[i].offset] ^= 0x40;
		expect_failure(&bytes, FSIMG_EUNSUPPORTED, closed[i].text);
		bytes.bytes[closed[i].offset] ^= 0x40;
	}

	/* A header cut short: the identity and nothing after it. */
	bytes.length = 40;
	expect_failure(&bytes, FSIMG_ECORRUPT, "truncated");
	bytes.length = HEADER_BYTES + c.stream.length;

	bytes.bytes[5] = 'S';
	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, &bytes, "ifs/-/hbcifs", &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "hbcifs") == 0);
	fixture_free(&f);
	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, &bytes, NULL, &error) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(error.path[0] == '\0');
	fixture_free(&f);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * The stream must produce exactly the declared length and end exactly at
 * the end of the payload: a declared length one more or one less, bytes
 * after the end marker in the decoder's input piece or in the next one, a
 * stream cut before its end marker and an empty payload are each corrupt,
 * naming hbcifs at the file offset where the stream went wrong.
 */
static void
test_stream(void)
{
	fsimg_error_t error;
	container_t c;
	buffer_t bytes;
	uint32_t decoded;
	size_t full;

	container_imagefs(&c);
	decoded = (uint32_t)c.plain_length;
	full = c.stream.length;
	error = expect_variant(&c, decoded + 1, full, 0, FSIMG_ECORRUPT,
	    "short of its decoded length");
	CHECK(error.offset_valid && error.offset == HEADER_BYTES + full);
	error = expect_variant(&c, decoded - 1, full, 0, FSIMG_ECORRUPT,
	    "longer than its limit");
	CHECK(error.offset_valid && error.offset > HEADER_BYTES &&
	    error.offset < HEADER_BYTES + full);
	error = expect_variant(&c, decoded, full, 1, FSIMG_ECORRUPT,
	    "after the end marker");
	CHECK(error.offset_valid && error.offset == HEADER_BYTES + full);
	error = expect_variant(&c, decoded, full - 3, 0, FSIMG_ECORRUPT,
	    "unterminated");
	CHECK(error.offset_valid && error.offset == HEADER_BYTES + full - 3);
	error = expect_variant(&c, decoded, 0, 0, FSIMG_ECORRUPT,
	    "unterminated");
	CHECK(error.offset_valid && error.offset == HEADER_BYTES);
	container_free(&c);

	/* A stream ending exactly at the decoder's first 64 KiB input piece,
	 * with a byte of payload after it in the next piece.
	 */
	for (size_t count = 60 * KIB;; count++) {
		segment_t literals = { .length = count };

		container_init(&c, calloc(1, count), count, &literals, 1);
		CHECK(c.plain != NULL && c.stream.length <= 64 * KIB);
		if (c.stream.length == 64 * KIB)
			break;
		container_free(&c);
	}
	bytes = container_file(&c);
	expect_disk_bytes(&bytes, c.plain, c.plain_length);
	free(bytes.bytes);
	error = expect_variant(&c, (uint32_t)c.plain_length, c.stream.length, 1,
	    FSIMG_ECORRUPT, "continues after the end marker");
	CHECK(error.offset_valid && error.offset == HEADER_BYTES + 64 * KIB);
	container_free(&c);
}

/*
 * hbcifs carries an ImageFS. A stack putting a scheme or another
 * filesystem inside it is FSIMG_EINVAL naming hbcifs: before any byte is
 * read when the stack names hbcifs, and at the claim, before any byte is
 * decoded, when the file's header does. Creation refuses the same stacks.
 * (test_open shows that a stack naming ifs alone reads the container.)
 */
static void
test_carried(void)
{
	static const char *const refused[] = { "fat32/-/hbcifs",
	    "ifs/mbr/hbcifs", "fat32", "ifs/gpt" };
	fsimg_write_options_t write_options;
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t plan;
	fsimg_writer_t *writer;
	fsimg_stack_t stack;
	fsimg_stats_t stats;
	memory_file_t file;
	fsimg_error_t error;
	container_t c;
	buffer_t bytes;
	fixture_t f;

	container_imagefs(&c);
	bytes = container_file(&c);
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		fixture_init(&f, 0, 0);
		CHECK(open_buffer(&f, &file, &bytes, refused[i], &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "hbcifs") == 0);
		if (i < 2)
			CHECK(file.reads == 0);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.spool_peak == 0);
		fixture_free(&f);
	}

	/* Creation applies the same rule. */
	fixture_init(&f, 0, 0);
	fsimg_write_options_init(&write_options);
	CHECK(fsimg_writer_new(f.ctx, "fat32/hbcifs", &write_options, &writer,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "hbcifs") == 0);
	/* The carried filesystem passes the rule, but the disk plan refuses
	 * a container with no writer. (The ImageFS writer is unregistered, so
	 * the plan is reached directly.)
	 */
	CHECK(fsimg_stack_parse("ifs/hbcifs", &stack, NULL) == FSIMG_OK);
	fsimg_layer_options_init(&layers);
	CHECK(fsimg_disk_plan_init(f.ctx, &stack, &layers, "", 1, 0, 0, &plan,
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "hbcifs") == 0 &&
	    strstr(error.message, "read only") != NULL);
	fsimg_disk_plan_free(&plan);
	fixture_free(&f);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * The spool limit. A decoded length exactly at the limit passes; one byte
 * over fails. A bomb, 64 MiB of zeros from a stream of about 256 KiB,
 * fails with FSIMG_RESOURCE_SPOOL at the 64 KiB step that crosses an
 * 8 MiB limit, and the spool file, watched through a duplicate
 * descriptor, never grew past the limit. Without a spool descriptor the
 * open fails as a spool limit of zero.
 */
static void
test_spool_limit(void)
{
	memory_file_t file;
	fsimg_error_t error;
	struct stat st;
	container_t c;
	buffer_t bytes;
	fixture_t f;
	int spool, watch;

	container_imagefs(&c);
	bytes = container_file(&c);
	fixture_init(&f, c.plain_length, 0);
	CHECK(open_buffer(&f, &file, &bytes, NULL, NULL) == FSIMG_OK);
	fixture_free(&f);
	fixture_init(&f, c.plain_length - 1, 0);
	CHECK(open_buffer(&f, &file, &bytes, NULL, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL &&
	    error.limit == c.plain_length - 1);
	fixture_free(&f);
	fixture_init(&f, 0, 0);
	file = (memory_file_t){ .bytes = bytes.bytes, .length = bytes.length };
	CHECK(open_file(&f, &file, NULL, -1, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL && error.limit == 0);
	fixture_free(&f);
	free(bytes.bytes);
	container_free(&c);

	container_zeros(&c, 64 * MIB);
	bytes = container_file(&c);
	CHECK(bytes.length < 300 * KIB);
	file = (memory_file_t){ .bytes = bytes.bytes, .length = bytes.length };
	fixture_init(&f, 8 * MIB, 0);
	spool = spool_new(&f);
	watch = dup(spool);
	CHECK(watch != -1);
	CHECK(open_file(&f, &file, NULL, spool, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL &&
	    error.limit == 8 * MIB && error.observed > 8 * MIB &&
	    error.observed <= 8 * MIB + 64 * KIB);
	CHECK(fstat(watch, &st) == 0 && st.st_size > 0 &&
	    (uint64_t)st.st_size <= 8 * MIB);
	CHECK(close(watch) == 0);
	fixture_free(&f);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * The one spool already holds the file when the container is gzip-encoded
 * or arrives as a stream, and the container would need a second: each is
 * FSIMG_EUNSUPPORTED naming both layers, decided at the container's claim.
 */
static void
test_second_spool(void)
{
	fsimg_image_options_t options;
	fsimg_stats_t stats;
	memory_file_t file;
	fsimg_error_t error;
	container_t c;
	buffer_t bytes, gzip;
	fixture_t f;
	ssize_t written;
	pid_t child;
	int ends[2], status;

	container_imagefs(&c);
	bytes = container_file(&c);
	gzip = gzip_of(&bytes);
	fixture_init(&f, 0, 0);
	CHECK(open_buffer(&f, &file, &gzip, NULL, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "hbcifs") == 0);
	CHECK(strstr(error.message, "hbcifs") != NULL &&
	    strstr(error.message, "gzip") != NULL);
	/* The spool held the decompressed file and nothing more. */
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.spool_peak == bytes.length);
	fixture_free(&f);

	CHECK(pipe(ends) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		close(ends[0]);
		for (size_t done = 0; done < bytes.length; done += (size_t)written) {
			written = write(ends[1], bytes.bytes + done,
			    bytes.length - done);
			if (written <= 0)
				_exit(1);
		}
		_exit(0);
	}
	CHECK(close(ends[1]) == 0);
	fixture_init(&f, 0, 0);
	fsimg_image_options_init(&options);
	options.spool_fd = spool_new(&f);
	CHECK(fsimg_image_open_stream(f.ctx, ends[0], &options, &f.image,
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "hbcifs") == 0);
	CHECK(strstr(error.message, "hbcifs") != NULL &&
	    strstr(error.message, "stream") != NULL);
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(close(ends[0]) == 0);
	fixture_free(&f);
	free(gzip.bytes);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * Decoding charges the stream's bytes and the decoded bytes to the work
 * counter. A stream of literals is about as long as its output, so a limit
 * of one and a half times the output stops the open only when both are
 * charged, and twice the output with room for the probes' reads passes.
 */
static void
test_work_limit(void)
{
	const size_t length = 256 * KIB;
	segment_t literals = { .length = length };
	memory_file_t file;
	fsimg_error_t error;
	container_t c;
	buffer_t bytes;
	fixture_t f;

	container_init(&c, calloc(1, length), length, &literals, 1);
	CHECK(c.plain != NULL && c.stream.length > length);
	bytes = container_file(&c);
	fixture_init(&f, 0, length + length / 2);
	CHECK(open_buffer(&f, &file, &bytes, NULL, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	fixture_free(&f);
	fixture_init(&f, 0, 2 * length + 64 * KIB);
	CHECK(open_buffer(&f, &file, &bytes, NULL, NULL) == FSIMG_OK);
	fixture_free(&f);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * A match at the farthest distance LZO1X allows, 49151 bytes, longer than
 * the decoder's 64 KiB window, so it is still copying when a window fills
 * and the next window must keep at least that much history. The bytes
 * around it are pseudo-random, so nothing nearer would copy them.
 */
static void
test_history_reach(void)
{
	const size_t lead = 100 * KIB, reach = 49151, match = 150 * KIB;
	const size_t tail = 16, length = lead + match + tail;
	segment_t segments[] = { { .length = lead },
	    { .distance = reach, .length = match }, { .length = tail } };
	unsigned char *plain;
	uint32_t state = 1;
	container_t c;
	buffer_t bytes;

	plain = malloc(length);
	CHECK(plain != NULL);
	for (size_t i = 0; i < length; i++) {
		state = state * 1103515245u + 12345u;
		plain[i] = i >= lead && i < lead + match ? plain[i - reach] :
		    (unsigned char)(state >> 16);
	}
	container_init(&c, plain, length, segments,
	    sizeof(segments) / sizeof(segments[0]));
	bytes = container_file(&c);
	expect_disk_bytes(&bytes, plain, length);
	free(bytes.bytes);
	container_free(&c);
}

/*
 * Every container's profile facts agree with its operations: the write
 * side is all there or all absent, the unverified list fits the layer
 * report, a carried filesystem has a reader, and a spooled container
 * names its stream's codec.
 */
static void
test_profile_facts(void)
{
	const fsimg_container_ops_t *ops;
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	const fsimg_layer_t *layer;
	fsimg_layer_report_t report;
	fsimg_layer_info_t info;
	size_t cursor = 0;

	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		layer = fsimg_layer_find(info.name);
		CHECK(layer != NULL);
		if (layer->container == NULL)
			continue;
		ops = layer->container;
		CHECK((ops->plan != NULL) == (ops->bound != NULL) &&
		    (ops->plan != NULL) == (ops->encoder != NULL));
		CHECK(ops->unverified == NULL ||
		    strlen(ops->unverified) < sizeof(report.unverified));
		if (ops->carries != NULL) {
			format = fsimg_format_find(ops->carries, &variant);
			CHECK(format != NULL && format->reader != NULL);
		}
		CHECK(!ops->spooled || ops->codecs != NULL);
	}
}

/*
 * Every allocation and every input read of an open fails in turn, each
 * reported with its status and every charge and the spool released, until
 * one attempt passes.
 */
static void
test_faults(void)
{
	memory_file_t file;
	container_t c;
	buffer_t bytes;
	fsimg_status_t status;
	fixture_t f;

	container_imagefs(&c);
	bytes = container_file(&c);
	for (unsigned sweep = 0; sweep < 2; sweep++) {
		for (uint64_t attempt = 1;; attempt++) {
			CHECK(attempt < 10000);
			file = (memory_file_t){ .bytes = bytes.bytes,
			    .length = bytes.length, .piece = 16 * KIB };
			fixture_init(&f, 0, 0);
			if (sweep == 0)
				f.fail_allocation = attempt;
			else
				file.fail_at = attempt;
			status = open_file(&f, &file, NULL, spool_new(&f), NULL);
			if (status == FSIMG_OK)
				expect_disk(&f, c.plain, c.plain_length);
			fixture_free(&f);
			if (status == FSIMG_OK) {
				CHECK(attempt > 1);
				break;
			}
			CHECK(status == (sweep == 0 ? FSIMG_ENOMEM : FSIMG_EIO));
		}
	}
	free(bytes.bytes);
	container_free(&c);
}

/* The capability record: a container read and verified, never written,
 * with no suffix and the LZO1X codec.
 */
static void
test_capability(void)
{
	fsimg_layer_info_t info;
	size_t cursor = 0;
	bool found = false;

	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		if (strcmp(info.name, "hbcifs") != 0)
			continue;
		found = true;
		CHECK(info.kind == FSIMG_LAYER_CONTAINER);
		CHECK(info.capabilities == (FSIMG_CAP_READ | FSIMG_CAP_VERIFY));
		CHECK(info.suffix[0] == '\0' && strcmp(info.codecs, "lzo1x") == 0);
		CHECK(info.codec_version[0] == '\0');
	}
	CHECK(found);
}

/* The POSIX cksum CRC over bytes, without the length. */
static uint32_t
cksum_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		crc ^= (uint32_t)bytes[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000u) != 0 ? crc << 1 ^ 0x04c11db7u :
			    crc << 1;
	}
	return (crc);
}

/*
 * --cksum IMAGE: open the image file through its layers, with a spool,
 * and print the POSIX cksum and length of the disk its container yields,
 * as cksum(1) prints them for a file, for test/ifs-integration.sh to
 * compare with an oracle's decode. Only the two numbers are printed.
 */
static int
disk_cksum(const char *path)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_options_t options;
	fsimg_options_t context;
	unsigned char *piece;
	fsimg_io_t *input, *disk;
	fsimg_status_t status;
	fixture_t f;
	uint64_t size, length;
	size_t count, transferred;
	uint32_t crc;
	int fd;

	bzero(&f, sizeof(f));
	snprintf(f.directory, sizeof(f.directory), "%s/fsimg-hbcifs.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f.directory) != NULL);
	f.directory_fd = open(f.directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f.directory_fd != -1);
	fsimg_options_init(&context);
	context.memory_bytes = 64 * MIB;
	CHECK(fsimg_ctx_new(&context, &f.ctx, NULL) == FSIMG_OK);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &input, NULL) == FSIMG_OK);
	fsimg_image_options_init(&options);
	options.spool_fd = spool_new(&f);
	disk = NULL;
	status = fsimg_image_open(f.ctx, input, &options, &f.image, NULL);
	if (status == FSIMG_OK)
		status = fsimg_image_select(f.image, &selector, &disk, NULL);
	if (status == FSIMG_OK)
		status = fsimg_io_get_size(disk, &size, NULL);
	piece = malloc(MIB);
	CHECK(piece != NULL);
	crc = 0;
	for (uint64_t done = 0; status == FSIMG_OK && done < size; done += count) {
		count = size - done < MIB ? (size_t)(size - done) : MIB;
		status = fsimg_io_read_exact(disk, done, piece, count,
		    &transferred, NULL);
		if (status == FSIMG_OK)
			crc = cksum_update(crc, piece, count);
	}
	if (status == FSIMG_OK) {
		for (length = size; length != 0; length >>= 8) {
			unsigned char byte = (unsigned char)length;

			crc = cksum_update(crc, &byte, 1);
		}
		printf("%" PRIu32 " %" PRIu64 "\n", ~crc, size);
	} else
		fprintf(stderr, "cksum: status %d\n", status);
	free(piece);
	fsimg_io_free(disk);
	fsimg_image_free(f.image);
	fsimg_io_free(input);
	fsimg_ctx_free(f.ctx);
	CHECK(close(f.directory_fd) == 0);
	CHECK(rmdir(f.directory) == 0);
	CHECK(fflush(stdout) == 0);
	return (status == FSIMG_OK ? 0 : 1);
}

static void
write_file(const char *path, const unsigned char *bytes, size_t length)
{
	FILE *out;

	out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite(bytes, 1, length, out) == length);
	CHECK(fclose(out) == 0);
}

/*
 * --seed-hbcifs PATH [DECODED]: the test container, which seeds the
 * container fuzzer and checks test/ifs-native.py's container command, and
 * the ImageFS it decodes to.
 */
static int
write_seed(const char *path, const char *decoded)
{
	container_t c;
	buffer_t bytes;

	container_imagefs(&c);
	bytes = container_file(&c);
	write_file(path, bytes.bytes, bytes.length);
	if (decoded != NULL)
		write_file(decoded, c.plain, c.plain_length);
	free(bytes.bytes);
	container_free(&c);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--cksum") == 0)
		return (disk_cksum(argv[2]));
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "--seed-hbcifs") == 0)
		return (write_seed(argv[2], argc == 4 ? argv[3] : NULL));
	CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
	test_capability();
	test_profile_facts();
	test_open();
	test_header();
	test_stream();
	test_history_reach();
	test_carried();
	test_spool_limit();
	test_second_spool();
	test_work_limit();
	test_faults();
	printf("hbcifs tests passed\n");
	return (0);
}
