/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
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
#include "internal.h"
#include "test.h"

/*
 * The gzip read side and the spool (specs/design-layers.md#gzip and
 * #stream-input-and-the-spool). This file builds its own gzip members, so
 * every header field can be set or damaged, and opens them as files
 * through callbacks and as streams through pipes. Each passing case has
 * seeded failures beside it that must fail with the named error. The
 * independent gzip and pigz oracles run in test/compression-integration.sh.
 */
#define KIB		1024u
#define MIB		(1024u * KIB)

#define FLAG_FHCRC	0x02u
#define FLAG_FEXTRA	0x04u
#define FLAG_FNAME	0x08u
#define FLAG_FCOMMENT	0x10u

typedef struct buffer {
	unsigned char	*bytes;
	size_t		 length;
	size_t		 capacity;
} buffer_t;

/* One member: its decoded bytes and the header fields and damage to apply.
 * A name or comment of name_length bytes (without the terminator) is
 * written as that many 'n' or 'c' bytes.
 */
typedef struct member {
	const unsigned char *data;
	size_t		 length;
	unsigned	 flags;
	size_t		 name_length;
	size_t		 comment_length;
	unsigned	 extra_length;
	bool		 bad_header_crc;
	bool		 bad_crc;
	bool		 bad_length;
} member_t;

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
static void append_le32(buffer_t *, uint32_t);
static void member_append(buffer_t *, const member_t *);
static buffer_t gzip_of(const unsigned char *, size_t);
static unsigned char *pattern(size_t, unsigned);
static fsimg_status_t file_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t file_size(void *, uint64_t *, fsimg_error_t *);
static void fixture_init(fixture_t *, uint64_t);
static void fixture_free(fixture_t *);
static int spool_new(fixture_t *);
static fsimg_status_t open_file(fixture_t *, memory_file_t *, const char *,
    const char *, int, fsimg_error_t *);
static fsimg_status_t open_pipe(fixture_t *, int, const char *, int,
    fsimg_error_t *);
static pid_t feed(const unsigned char *, size_t, int *);
static void reap(pid_t);
static void expect_disk(fixture_t *, const unsigned char *, size_t);
static void expect_corrupt(const buffer_t *, const char *);
static void test_members(void);
static void test_member_failures(void);
static void test_detection(void);
static void test_spool_limit(void);
static void test_spool_holes(void);
static void test_spool_descriptor(void);
static void test_zeroed_options(void);
static void test_decode_work(void);
static void test_streams(void);
static void test_faults(void);
static void test_spool_write_failure(void);

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
append_le32(buffer_t *buffer, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		append_byte(buffer, (value >> (8 * i)) & 0xff);
}

/* A member as RFC 1952 lays it out, around zlib's raw DEFLATE. */
static void
member_append(buffer_t *out, const member_t *member)
{
	z_stream stream;
	unsigned char *deflated;
	size_t header_start, bound;
	uLong crc;

	header_start = out->length;
	append_byte(out, 0x1f);
	append_byte(out, 0x8b);
	append_byte(out, 8);
	append_byte(out, member->flags);
	append_le32(out, 1700000000);
	append_byte(out, 0);
	append_byte(out, 3);
	if ((member->flags & FLAG_FEXTRA) != 0) {
		append_byte(out, member->extra_length & 0xff);
		append_byte(out, member->extra_length >> 8);
		for (unsigned i = 0; i < member->extra_length; i++)
			append_byte(out, 'x');
	}
	if ((member->flags & FLAG_FNAME) != 0) {
		for (size_t i = 0; i < member->name_length; i++)
			append_byte(out, 'n');
		append_byte(out, 0);
	}
	if ((member->flags & FLAG_FCOMMENT) != 0) {
		for (size_t i = 0; i < member->comment_length; i++)
			append_byte(out, 'c');
		append_byte(out, 0);
	}
	if ((member->flags & FLAG_FHCRC) != 0) {
		crc = crc32(0, out->bytes + header_start,
		    (uInt)(out->length - header_start));
		if (member->bad_header_crc)
			crc ^= 1;
		append_byte(out, crc & 0xff);
		append_byte(out, (crc >> 8) & 0xff);
	}

	bzero(&stream, sizeof(stream));
	CHECK(deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8,
	    Z_DEFAULT_STRATEGY) == Z_OK);
	bound = deflateBound(&stream, (uLong)member->length);
	deflated = malloc(bound);
	CHECK(deflated != NULL);
	stream.next_in = (unsigned char *)(uintptr_t)member->data;
	stream.avail_in = (uInt)member->length;
	stream.next_out = deflated;
	stream.avail_out = (uInt)bound;
	CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	append(out, deflated, bound - stream.avail_out);
	CHECK(deflateEnd(&stream) == Z_OK);
	free(deflated);

	crc = crc32(0, member->data, (uInt)member->length);
	append_le32(out, (uint32_t)crc ^ (member->bad_crc ? 1u : 0u));
	append_le32(out, (uint32_t)member->length + (member->bad_length ? 1 : 0));
}

static buffer_t
gzip_of(const unsigned char *data, size_t length)
{
	member_t member = { .data = data, .length = length };
	buffer_t out = { 0 };

	member_append(&out, &member);
	return (out);
}

/* Bytes that vary with their offset and the seed and compress well. */
static unsigned char *
pattern(size_t length, unsigned seed)
{
	unsigned char *bytes = malloc(length == 0 ? 1 : length);

	CHECK(bytes != NULL);
	for (size_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)((i / 512 + seed) * 31 + (i % 7));
	return (bytes);
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

static void
fixture_init(fixture_t *f, uint64_t spool_bytes)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-compression.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * MIB;
	options.spool_bytes = spool_bytes;
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
open_file(fixture_t *f, memory_file_t *file, const char *compression,
    const char *stack, int spool, fsimg_error_t *error)
{
	fsimg_image_options_t options;
	fsimg_io_ops_t ops;
	fsimg_status_t status;

	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, file, &f->io, NULL) == FSIMG_OK);
	fsimg_image_options_init(&options);
	snprintf(options.compression, sizeof(options.compression), "%s",
	    compression);
	options.spool_fd = spool;
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	fsimg_test_fail_alloc_after(f->ctx, f->fail_allocation);
	status = fsimg_image_open(f->ctx, f->io, &options, &f->image, error);
	fsimg_test_fail_alloc_after(f->ctx, 0);
	return (status);
}

static fsimg_status_t
open_pipe(fixture_t *f, int stream, const char *compression, int spool,
    fsimg_error_t *error)
{
	fsimg_image_options_t options;

	fsimg_image_options_init(&options);
	snprintf(options.compression, sizeof(options.compression), "%s",
	    compression);
	options.spool_fd = spool;
	return (fsimg_image_open_stream(f->ctx, stream, &options, &f->image,
	    error));
}

/* A child that writes bytes into a pipe and exits; *stream is its read end. */
static pid_t
feed(const unsigned char *bytes, size_t length, int *stream)
{
	ssize_t written;
	pid_t child;
	int ends[2];

	CHECK(pipe(ends) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		close(ends[0]);
		for (size_t done = 0; done < length; done += (size_t)written) {
			written = write(ends[1], bytes + done, length - done);
			if (written <= 0)
				_exit(1);
		}
		_exit(0);
	}
	CHECK(close(ends[1]) == 0);
	*stream = ends[0];
	return (child);
}

static void
reap(pid_t child)
{
	int status;

	CHECK(waitpid(child, &status, 0) == child);
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

/* The file opens as gzip and fails as corrupt with message, naming gzip. */
static void
expect_corrupt(const buffer_t *gzip, const char *message)
{
	memory_file_t file = { .bytes = gzip->bytes, .length = gzip->length };
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), &error) ==
	    FSIMG_ECORRUPT);
	if (strcmp(error.message, message) != 0)
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message,
		    error.message);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(strcmp(error.path, "gzip") == 0 &&
	    error.operation == FSIMG_OP_IMAGE);
	fixture_free(&f);
}

/*
 * Members follow one another as gzip(1) reads them: one with every
 * optional header field (FEXTRA, a 64 KiB FNAME with its terminator,
 * FCOMMENT and a header CRC16), a plain one, and an empty one, read in
 * whole pieces and in pieces of 7 bytes. The image reports gzip and the
 * spool's length, and verification reports the members checked.
 */
static void
test_members(void)
{
	unsigned char *first = pattern(300 * KIB, 1), *second = pattern(70 * KIB, 2);
	unsigned char *expected;
	member_t members[] = {
		{ .data = first, .length = 300 * KIB, .flags = FLAG_FEXTRA |
		    FLAG_FNAME | FLAG_FCOMMENT | FLAG_FHCRC, .extra_length = 300,
		    .name_length = 64 * KIB - 1, .comment_length = 20 },
		{ .data = second, .length = 70 * KIB },
		{ .data = second, .length = 0 }
	};
	fsimg_image_info_t info;
	fsimg_layer_report_t report;
	buffer_t gzip = { 0 };
	memory_file_t file;
	fixture_t f;

	for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++)
		member_append(&gzip, &members[i]);
	expected = malloc(370 * KIB);
	CHECK(expected != NULL);
	memcpy(expected, first, 300 * KIB);
	memcpy(expected + 300 * KIB, second, 70 * KIB);
	for (size_t piece = 0; piece <= 7; piece += 7) {
		file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length,
		    .piece = piece };
		fixture_init(&f, 0);
		CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, NULL) != FSIMG_OK);
		expect_disk(&f, expected, 370 * KIB);
		fixture_free(&f);
	}
	/* Forcing gzip reads the same; the info record names it. */
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "gzip", "fat12/-/-", spool_new(&f),
	    NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.compression, "gzip") == 0 &&
	    info.file_bytes == 370 * KIB && info.disk_bytes == 370 * KIB);
	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
	CHECK(strcmp(report.compression, "gzip") == 0);
	fixture_free(&f);
	free(gzip.bytes);
	free(expected);
	free(second);
	free(first);
}

/*
 * Each seeded damage fails as corrupt with its check: trailing bytes,
 * short or long; the CRC32 and the length; a reserved flag or another
 * method in a later member; truncation in the header, the DEFLATE data
 * and the trailer; a 65 KiB FNAME and FCOMMENT; the header CRC16; DEFLATE
 * data that is not DEFLATE.
 */
static void
test_member_failures(void)
{
	unsigned char *data = pattern(100 * KIB, 3);
	member_t plain = { .data = data, .length = 100 * KIB };
	member_t damaged;
	buffer_t gzip = { 0 }, cut;
	size_t member_length;

	member_append(&gzip, &plain);
	member_length = gzip.length;
	append(&gzip, "junk", 4);
	expect_corrupt(&gzip, "bytes follow the last gzip member");
	append(&gzip, "more trailing bytes", 19);
	expect_corrupt(&gzip, "bytes follow the last gzip member");
	gzip.length = member_length;
	append(&gzip, "\0\0\0\0\0\0\0\0\0\0\0\0", 12);
	expect_corrupt(&gzip, "bytes follow the last gzip member");
	gzip.length = member_length;
	member_append(&gzip, &plain);
	gzip.bytes[member_length + 3] = 0x20;
	expect_corrupt(&gzip, "bytes follow the last gzip member");
	gzip.bytes[member_length + 3] = 0;
	gzip.bytes[member_length + 2] = 7;
	expect_corrupt(&gzip, "bytes follow the last gzip member");
	free(gzip.bytes);

	damaged = plain;
	damaged.bad_crc = true;
	gzip = (buffer_t){ 0 };
	member_append(&gzip, &damaged);
	expect_corrupt(&gzip, "gzip member CRC32 mismatch");
	free(gzip.bytes);
	damaged = plain;
	damaged.bad_length = true;
	gzip = (buffer_t){ 0 };
	member_append(&gzip, &damaged);
	expect_corrupt(&gzip, "gzip member length mismatch");
	free(gzip.bytes);

	gzip = gzip_of(data, 100 * KIB);
	for (size_t keep = 6; keep < gzip.length; keep += gzip.length / 5) {
		cut = gzip;
		cut.length = keep;
		expect_corrupt(&cut, "gzip member is truncated");
	}
	cut = gzip;
	cut.length = gzip.length - 3;
	expect_corrupt(&cut, "gzip member is truncated");
	/* A final block of the reserved type 3. */
	gzip.bytes[10] = 0x07;
	expect_corrupt(&gzip, "gzip member's DEFLATE data is invalid");
	free(gzip.bytes);

	damaged = plain;
	damaged.flags = FLAG_FNAME;
	damaged.name_length = 65 * KIB;
	gzip = (buffer_t){ 0 };
	member_append(&gzip, &damaged);
	expect_corrupt(&gzip, "gzip file name exceeds 64 KiB");
	free(gzip.bytes);
	damaged.flags = FLAG_FCOMMENT;
	damaged.name_length = 0;
	damaged.comment_length = 64 * KIB;
	gzip = (buffer_t){ 0 };
	member_append(&gzip, &damaged);
	expect_corrupt(&gzip, "gzip comment exceeds 64 KiB");
	free(gzip.bytes);
	damaged = plain;
	damaged.flags = FLAG_FHCRC;
	damaged.bad_header_crc = true;
	gzip = (buffer_t){ 0 };
	member_append(&gzip, &damaged);
	expect_corrupt(&gzip, "gzip header CRC16 mismatch");
	free(gzip.bytes);
	free(data);
}

/*
 * The compression step. A first member with a reserved flag bit is no
 * gzip claim: the file is read raw, and forcing gzip fails as an absent
 * signature naming gzip, as -z does on a raw file. A prefix that passes
 * for gzip and for an MBR is ambiguous; a stack resolves it as
 * uncompressed and forced gzip as compressed, which then fails to decode.
 * A seekable uncompressed file closes its unused spool at once.
 */
static void
test_detection(void)
{
	unsigned char *data = pattern(64 * KIB, 4), *disk;
	fsimg_image_info_t info;
	fsimg_error_t error;
	memory_file_t file;
	buffer_t gzip;
	fixture_t f;
	int spool;

	gzip = gzip_of(data, 64 * KIB);
	gzip.bytes[3] = 0x80;
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, 0);
	spool = spool_new(&f);
	CHECK(open_file(&f, &file, "", "fat12/-/-", spool, NULL) == FSIMG_OK);
	CHECK(fcntl(spool, F_GETFD) == -1 && errno == EBADF);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.compression[0] == '\0' && info.disk_bytes == gzip.length);
	fixture_free(&f);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "gzip", NULL, spool_new(&f),
	    &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "gzip") == 0);
	fixture_free(&f);
	file = (memory_file_t){ .bytes = data, .length = 64 * KIB };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "gzip", NULL, spool_new(&f),
	    &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "gzip") == 0);
	fixture_free(&f);
	/* A forced name the registry cannot decode, or lacks, is refused. */
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f),
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "zstd") == 0);
	fixture_free(&f);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "dmg", NULL, spool_new(&f),
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "dmg") == 0);
	fixture_free(&f);
	free(gzip.bytes);

	/* gzip's identity in the boot code of an MBR disk. */
	disk = calloc(1, 2 * MIB);
	CHECK(disk != NULL);
	memcpy(disk, "\x1f\x8b\x08\x00", 4);
	disk[446 + 4] = 0x0c;
	disk[446 + 9] = 0x08;	/* LBA 2048 */
	disk[446 + 12] = 100;
	disk[510] = 0x55;
	disk[511] = 0xaa;
	file = (memory_file_t){ .bytes = disk, .length = 2 * MIB };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), &error) ==
	    FSIMG_EFORMAT);
	CHECK(strstr(error.message, "both a compressed and an uncompressed") !=
	    NULL);
	fixture_free(&f);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", "fat12", spool_new(&f), NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.compression[0] == '\0' && strcmp(info.scheme, "mbr") == 0);
	fixture_free(&f);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "gzip", NULL, spool_new(&f),
	    &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "gzip") == 0);
	fixture_free(&f);
	free(disk);
	free(data);
}

/*
 * The spool limit. Decoded bytes exactly at the limit pass and are
 * charged while the image lives. A bomb, 64 MiB of zeros in about 64 KiB,
 * fails with FSIMG_RESOURCE_SPOOL at the step that crosses an 8 MiB
 * limit, and the spool file, watched through a duplicate descriptor,
 * never grew past the limit.
 */
static void
test_spool_limit(void)
{
	unsigned char *data = pattern(MIB, 5), *zeros = calloc(1, 64 * MIB);
	fsimg_error_t error;
	fsimg_stats_t stats;
	memory_file_t file;
	struct stat st;
	buffer_t gzip;
	fixture_t f;
	int spool, watch;

	CHECK(zeros != NULL);
	gzip = gzip_of(data, MIB);
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, MIB);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.spool_bytes == MIB && stats.spool_peak == MIB);
	fixture_free(&f);
	fixture_init(&f, MIB - 1);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), &error) ==
	    FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL && error.limit == MIB - 1);
	fixture_free(&f);
	free(gzip.bytes);

	gzip = gzip_of(zeros, 64 * MIB);
	CHECK(gzip.length < 128 * KIB);
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, 8 * MIB);
	spool = spool_new(&f);
	watch = dup(spool);
	CHECK(watch != -1);
	CHECK(open_file(&f, &file, "", NULL, spool, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL &&
	    error.limit == 8 * MIB && error.observed > 8 * MIB);
	CHECK(fstat(watch, &st) == 0 && st.st_size > 0 &&
	    (uint64_t)st.st_size <= 8 * MIB);
	CHECK(close(watch) == 0);
	fixture_free(&f);
	free(gzip.bytes);
	free(zeros);
	free(data);
}

/*
 * All-zero input costs no storage: 32 MiB of zeros, decoded or copied
 * from a stream, is charged in full as logical bytes while the sampled
 * physical bytes stay below 1 MiB, the zero blocks left as holes.
 */
static void
test_spool_holes(void)
{
	unsigned char *zeros = calloc(1, 32 * MIB);
	fsimg_stats_t stats;
	memory_file_t file;
	buffer_t gzip;
	fixture_t f;
	pid_t child;
	int stream;

	CHECK(zeros != NULL);
	gzip = gzip_of(zeros, 32 * MIB);
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.spool_bytes == 32 * MIB &&
	    stats.spool_physical_bytes < MIB);
	expect_disk(&f, zeros, 32 * MIB);
	fixture_free(&f);
	free(gzip.bytes);

	fixture_init(&f, 0);
	child = feed(zeros, 32 * MIB, &stream);
	CHECK(open_pipe(&f, stream, "", spool_new(&f), NULL) == FSIMG_OK);
	reap(child);
	CHECK(close(stream) == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.spool_bytes == 32 * MIB &&
	    stats.spool_physical_bytes < MIB);
	fixture_free(&f);
	free(zeros);
}

/*
 * The spool descriptor. Without one, a gzip file and any stream fail as a
 * spool limit of zero, the stream before a byte of it is read. A
 * descriptor that is not a fresh unlinked read/write regular file without
 * O_APPEND fails with FSIMG_EINVAL naming the spool before the stream is
 * read, and is closed.
 */
static void
test_spool_descriptor(void)
{
	unsigned char *data = pattern(64 * KIB, 6), byte;
	fsimg_error_t error;
	memory_file_t file;
	buffer_t gzip;
	fixture_t f;
	pid_t child;
	int spool, stream, ends[2];

	gzip = gzip_of(data, 64 * KIB);
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, -1, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL && error.limit == 0);
	fixture_free(&f);

	for (unsigned kind = 0; kind < 7; kind++) {
		fixture_init(&f, 0);
		switch (kind) {
		case 0:		/* No spool at all */
			spool = -1;
			break;
		case 1:		/* Not empty */
			spool = spool_new(&f);
			CHECK(write(spool, "x", 1) == 1);
			break;
		case 2:		/* Still linked */
			spool = openat(f.directory_fd, "named", O_RDWR | O_CREAT |
			    O_EXCL | O_CLOEXEC, 0600);
			break;
		case 3:		/* Read-only */
			spool = openat(f.directory_fd, "readonly", O_RDONLY |
			    O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			CHECK(unlinkat(f.directory_fd, "readonly", 0) == 0);
			break;
		case 4:		/* Appending */
			spool = openat(f.directory_fd, "append", O_RDWR | O_APPEND |
			    O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			CHECK(unlinkat(f.directory_fd, "append", 0) == 0);
			break;
		case 5:		/* Not a descriptor or -1: refused, nothing closed */
			spool = -2;
			break;
		default:	/* Not a regular file */
			CHECK(pipe(ends) == 0);
			CHECK(close(ends[1]) == 0);
			spool = ends[0];
			break;
		}
		CHECK(kind == 0 || spool != -1);
		if (kind == 5) {
			stream = -1;
			CHECK(open_pipe(&f, -1, "", spool, &error) == FSIMG_EINVAL);
			CHECK(strcmp(error.message, "invalid spool descriptor") == 0);
			fixture_free(&f);
			continue;
		}
		child = feed(gzip.bytes, gzip.length, &stream);
		if (kind == 0) {
			CHECK(open_pipe(&f, stream, "", spool, &error) ==
			    FSIMG_ELIMIT);
			CHECK(error.resource == FSIMG_RESOURCE_SPOOL &&
			    error.limit == 0);
		} else {
			CHECK(open_pipe(&f, stream, "", spool, &error) ==
			    FSIMG_EINVAL);
			CHECK(strcmp(error.path, "spool") == 0);
			CHECK(fcntl(spool, F_GETFD) == -1 && errno == EBADF);
		}
		CHECK(read(stream, &byte, 1) == 1 && byte == 0x1f);
		CHECK(close(stream) == 0);
		reap(child);
		if (kind == 2)
			CHECK(unlinkat(f.directory_fd, "named", 0) == 0);
		fixture_free(&f);
	}
	free(gzip.bytes);
	free(data);
}

/*
 * Zero-filled image options name descriptor 0, standard input, as the
 * spool. Their size is not the one fsimg_image_options_init sets, so both
 * opens refuse them before the spool is examined and descriptor 0 stays
 * open.
 */
static void
test_zeroed_options(void)
{
	fsimg_image_options_t options;
	fsimg_image_t *image;
	fsimg_io_ops_t ops;
	fsimg_error_t error;
	memory_file_t file;
	unsigned char byte = 0;
	fixture_t f;
	int ends[2];

	/* With descriptor 0 closed, the lowest free descriptor fills it. */
	if (fcntl(0, F_GETFD) == -1)
		CHECK(open("/dev/null", O_RDONLY) == 0);
	bzero(&options, sizeof(options));
	file = (memory_file_t){ .bytes = &byte, .length = 1 };
	fixture_init(&f, 0);
	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &file, &f.io, NULL) == FSIMG_OK);
	CHECK(fsimg_image_open(f.ctx, f.io, &options, &image, &error) ==
	    FSIMG_EINVAL && image == NULL);
	CHECK(strcmp(error.message, "invalid image options size or version") == 0);
	CHECK(fcntl(0, F_GETFD) != -1);
	CHECK(pipe(ends) == 0);
	CHECK(fsimg_image_open_stream(f.ctx, ends[0], &options, &image,
	    &error) == FSIMG_EINVAL && image == NULL);
	CHECK(fcntl(0, F_GETFD) != -1);
	CHECK(close(ends[0]) == 0 && close(ends[1]) == 0);
	fixture_free(&f);
}

/*
 * Decoding counts work as a container's chunk decoding does: every
 * compressed byte read, headers included, and every byte produced, for a
 * file and a stream alike. A work limit of half a MiB stops either the
 * decode of 1 MiB of data or the read of 1 MiB of member headers around
 * nearly empty members, with FSIMG_RESOURCE_WORK.
 */
static void
test_decode_work(void)
{
	unsigned char *data = pattern(MIB, 8);
	member_t member = { .data = data, .length = 16, .flags = FLAG_FNAME |
	    FLAG_FCOMMENT, .name_length = 64 * KIB - 1,
	    .comment_length = 64 * KIB - 1 };
	fsimg_options_t options;
	fsimg_stats_t stats;
	fsimg_error_t error;
	memory_file_t file;
	buffer_t inputs[2] = { { 0 } };
	fixture_t f;
	pid_t child;
	int stream;

	inputs[0] = gzip_of(data, MIB);
	for (unsigned i = 0; i < 8; i++)
		member_append(&inputs[1], &member);
	for (unsigned run = 0; run < 4; run++) {
		const buffer_t *input = &inputs[run / 2];

		fixture_init(&f, 0);
		fsimg_ctx_free(f.ctx);
		fsimg_options_init(&options);
		options.memory_bytes = 16 * MIB;
		options.max_work = MIB / 2;
		CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		f.baseline_memory = stats.memory_bytes;
		if (run % 2 != 0) {
			child = feed(input->bytes, input->length, &stream);
			CHECK(open_pipe(&f, stream, "", spool_new(&f), &error) ==
			    FSIMG_ELIMIT);
			CHECK(close(stream) == 0);
			reap(child);
		} else {
			file = (memory_file_t){ .bytes = input->bytes,
			    .length = input->length };
			CHECK(open_file(&f, &file, "", NULL, spool_new(&f),
			    &error) == FSIMG_ELIMIT);
		}
		CHECK(error.resource == FSIMG_RESOURCE_WORK);
		fixture_free(&f);
	}
	free(inputs[0].bytes);
	free(inputs[1].bytes);
	free(data);
}

/*
 * Streams. An uncompressed stream is copied into the spool unchanged and
 * a gzip stream decoded, both read to their end through a pipe; a stream
 * cut inside a member is corrupt.
 */
static void
test_streams(void)
{
	unsigned char *data = pattern(3 * MIB + 17, 7);
	fsimg_image_info_t info;
	fsimg_error_t error;
	buffer_t gzip;
	fixture_t f;
	pid_t child;
	int stream;

	fixture_init(&f, 0);
	child = feed(data, 3 * MIB + 17, &stream);
	CHECK(open_pipe(&f, stream, "", spool_new(&f), NULL) == FSIMG_OK);
	reap(child);
	CHECK(close(stream) == 0);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) != FSIMG_OK);
	expect_disk(&f, data, 3 * MIB + 17);
	fixture_free(&f);

	gzip = gzip_of(data, 3 * MIB + 17);
	fixture_init(&f, 0);
	child = feed(gzip.bytes, gzip.length, &stream);
	CHECK(open_pipe(&f, stream, "", spool_new(&f), NULL) == FSIMG_OK);
	reap(child);
	CHECK(close(stream) == 0);
	expect_disk(&f, data, 3 * MIB + 17);
	fixture_free(&f);

	fixture_init(&f, 0);
	child = feed(gzip.bytes, gzip.length / 2, &stream);
	CHECK(open_pipe(&f, stream, "", spool_new(&f), &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "gzip") == 0);
	reap(child);
	CHECK(close(stream) == 0);
	fixture_free(&f);
	free(gzip.bytes);
	free(data);
}

/*
 * Every allocation and every input read of a gzip open fails in turn,
 * each reported with its status and every charge released, until one
 * attempt passes.
 */
static void
test_faults(void)
{
	unsigned char *data = pattern(600 * KIB, 8);
	memory_file_t file;
	buffer_t gzip = { 0 };
	member_t member = { .data = data, .length = 300 * KIB,
	    .flags = FLAG_FNAME | FLAG_FHCRC, .name_length = 10 };
	fsimg_status_t status;
	fixture_t f;

	member_append(&gzip, &member);
	member.data = data + 300 * KIB;
	member_append(&gzip, &member);
	for (unsigned sweep = 0; sweep < 2; sweep++) {
		for (uint64_t attempt = 1;; attempt++) {
			CHECK(attempt < 10000);
			file = (memory_file_t){ .bytes = gzip.bytes,
			    .length = gzip.length, .piece = 4096 };
			fixture_init(&f, 0);
			if (sweep == 0)
				f.fail_allocation = attempt;
			else
				file.fail_at = attempt;
			status = open_file(&f, &file, "", NULL, spool_new(&f), NULL);
			if (status == FSIMG_OK)
				expect_disk(&f, data, 600 * KIB);
			fixture_free(&f);
			if (status == FSIMG_OK) {
				CHECK(attempt > 1);
				break;
			}
			CHECK(status == (sweep == 0 ? FSIMG_ENOMEM : FSIMG_EIO));
		}
	}
	free(gzip.bytes);
	free(data);
}

/*
 * A spool that cannot grow: under a 1 MiB file-size limit, decoding
 * 4 MiB fails as an I/O error and releases everything. The limit applies
 * to the whole process, so a child runs the case.
 */
static void
test_spool_write_failure(void)
{
	struct rlimit limit = { .rlim_cur = MIB, .rlim_max = RLIM_INFINITY };
	unsigned char *data;
	fsimg_error_t error;
	memory_file_t file;
	buffer_t gzip;
	fixture_t f;
	pid_t child;
	int status;

	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		data = pattern(4 * MIB, 9);
		gzip = gzip_of(data, 4 * MIB);
		file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
		CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
		CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
		fixture_init(&f, 0);
		CHECK(open_file(&f, &file, "", NULL, spool_new(&f), &error) ==
		    FSIMG_EIO);
		CHECK(error.os_error == EFBIG);
		fixture_free(&f);
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int
main(int argc, char **argv)
{
	unsigned char *data;
	member_t members[2];
	buffer_t gzip = { 0 };
	FILE *out;

	/* A small two-member file seeds the gzip fuzzer's corpus. */
	if (argc == 3 && strcmp(argv[1], "--seed-gzip") == 0) {
		data = pattern(16 * KIB, 10);
		members[0] = (member_t){ .data = data, .length = 16 * KIB,
		    .flags = FLAG_FEXTRA | FLAG_FNAME | FLAG_FCOMMENT | FLAG_FHCRC,
		    .extra_length = 6, .name_length = 8, .comment_length = 4 };
		members[1] = (member_t){ .data = data, .length = 4 * KIB };
		member_append(&gzip, &members[0]);
		member_append(&gzip, &members[1]);
		out = fopen(argv[2], "wb");
		CHECK(out != NULL);
		CHECK(fwrite(gzip.bytes, 1, gzip.length, out) == gzip.length);
		CHECK(fclose(out) == 0);
		free(gzip.bytes);
		free(data);
		return (0);
	}
	CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
	test_members();
	test_member_failures();
	test_detection();
	test_spool_limit();
	test_spool_holes();
	test_spool_descriptor();
	test_zeroed_options();
	test_decode_work();
	test_streams();
	test_faults();
	test_spool_write_failure();
	printf("compression tests passed\n");
	return (0);
}
