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

#ifdef FSIMG_XZ
#include <lzma.h>
#endif
#include <zlib.h>
#ifdef FSIMG_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

/*
 * The gzip, xz and zstd profiles and the spool (specs/design-layers.md#gzip and
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
	bool		 overstate_compression_length;
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
static void test_zstd_registry_reports_build_support(void);
static void test_xz_registry_reports_build_support(void);
static void write_seed_buffer(const char *, const char *, const buffer_t *);
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
	/* A registered compression refuses either its signature or its capability. */
	fixture_init(&f, 0);
#ifdef FSIMG_ZSTD
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f),
	    &error) == FSIMG_EFORMAT);
#else
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f),
	    &error) == FSIMG_EUNSUPPORTED);
#endif
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

/* The reserved xz row and its defaults exist even without liblzma. */
static void
test_xz_registry_reports_build_support(void)
{
	fsimg_layer_options_t layers;
	fsimg_layer_info_t info;
#ifndef FSIMG_XZ
	fsimg_image_info_t image_info;
#endif
	fsimg_error_t error;
	unsigned char raw[64 * KIB] = { 0 };
	memory_file_t file = { .bytes = raw, .length = sizeof(raw) };
	fsimg_status_t status;
	fixture_t f;
	size_t cursor = 0;
	bool found = false;

	fsimg_layer_options_init(&layers);
	CHECK(layers.xz_level == 6 && layers.xz_check == FSIMG_XZ_CRC64);
	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		if (strcmp(info.name, "xz") != 0)
			continue;
		found = true;
#ifdef FSIMG_XZ
		char version[64];

		CHECK(info.capabilities ==
		    (FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY));
		snprintf(version, sizeof(version), "liblzma %s", lzma_version_string());
		CHECK(strcmp(info.codec_version, version) == 0);
#else
		CHECK(info.capabilities == 0 && info.codec_version[0] == '\0');
#endif
	}
	CHECK(found);
	fixture_init(&f, 0);
#ifdef FSIMG_XZ
	CHECK(open_file(&f, &file, "xz", NULL, spool_new(&f), &error) ==
	    FSIMG_EFORMAT);
#else
	CHECK(open_file(&f, &file, "xz", NULL, spool_new(&f), &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strstr(error.message,
	    "reading this compression is not supported") != NULL);
#endif
	CHECK(strcmp(error.path, "xz") == 0);
	fixture_free(&f);

	raw[0] = 0xfd;
	bcopy("7zXZ", raw + 1, 4);
	file.reads = 0;
	fixture_init(&f, 0);
	status = open_file(&f, &file, "", NULL, spool_new(&f), &error);
#ifdef FSIMG_XZ
	CHECK(status == FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "xz") == 0);
#else
	/* Without operations the row is not surveyed. Open retains the
	 * raw bytes; asking for image information must reject them.
	 */
	CHECK(status == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &image_info, &error) ==
	    FSIMG_EFORMAT);
#endif
	fixture_free(&f);
}

#if defined(FSIMG_XZ) || defined(FSIMG_ZSTD)
static void codec_context(fixture_t *, uint64_t, uint64_t);
static fsimg_status_t compressed_output(fixture_t *, memory_file_t *,
    unsigned, fsimg_xz_check_t, const char *, bool, int, fsimg_error_t *);
static buffer_t compressed_written(const unsigned char *, size_t, unsigned,
    fsimg_xz_check_t, uint64_t, const char *, bool);

/* Replace the fixture's idle context so every case states its own budget. */
static void
codec_context(fixture_t *f, uint64_t memory, uint64_t work)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	fsimg_ctx_free(f->ctx);
	fsimg_options_init(&options);
	options.memory_bytes = memory;
	options.max_work = work;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Transcode arbitrary disk bytes through the planned compression into a
 * stream. No filesystem fixture or raw intermediate is needed.
 */
static fsimg_status_t
compressed_output(fixture_t *f, memory_file_t *file, unsigned level,
    fsimg_xz_check_t check, const char *name, bool container, int fd,
    fsimg_error_t *error)
{
	fsimg_layer_options_t layers;
	fsimg_disk_request_t request;
	fsimg_disk_plan_t plan;
	fsimg_session_t session;
	fsimg_stack_t stack;
	fsimg_io_ops_t ops;
	fsimg_sink_t sink;
	fsimg_status_t status;
	uint64_t disk_bytes = file->length, written;

	fsimg_io_ops_init(&ops);
	ops.read_at = file_read;
	ops.get_size = file_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, file, &f->io, NULL) == FSIMG_OK);
	fsimg_layer_options_init(&layers);
	if (strcmp(name, "zstd") == 0)
		layers.zstd_level = level;
	else {
		layers.xz_level = level;
		layers.xz_check = check;
	}
	CHECK(fsimg_stack_parse(container ? "fat12/dmg" : "fat12/-/-", &stack, NULL) == FSIMG_OK);
	fsimg_session_init(&session, f->ctx, "compressed test output", 0);
	request = (fsimg_disk_request_t){ .session = &session,
	    .stack = &stack, .layers = &layers, .compression = name,
	    .flags = FSIMG_WRITE_REPRODUCIBLE, .partitions = 1 };
	CHECK(fsimg_disk_plan_init(f->ctx, &request, &plan, error) == FSIMG_OK);
	CHECK(fsimg_disk_plan_place(&plan, &disk_bytes, error) == FSIMG_OK);
	/* Fault injection: transcode still delivers the actual disk length. */
	if (f->overstate_compression_length)
		plan.compression_settings.length++;
	CHECK(fsimg_disk_plan_budget(f->ctx, &plan, false, error) == FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = fd;
	fsimg_test_fail_alloc_after(f->ctx, f->fail_allocation);
	status = fsimg_disk_output(&session, f->io, &plan, &sink, false,
	    &written, error);
	fsimg_test_fail_alloc_after(f->ctx, 0);
	fsimg_disk_plan_free(&plan);
	return (status);
}

static buffer_t
compressed_written(const unsigned char *data, size_t length, unsigned level,
    fsimg_xz_check_t check, uint64_t budget, const char *name, bool container)
{
	memory_file_t file = { .bytes = data, .length = length };
	fsimg_error_t error;
	struct stat st;
	buffer_t out = { 0 };
	fixture_t f;
	int fd;

	fixture_init(&f, 0);
	codec_context(&f, budget, 0);
	fd = spool_new(&f);
	if (compressed_output(&f, &file, level, check, name, container, fd,
	    &error) != FSIMG_OK) {
		fprintf(stderr, "%s level %u length %zu: %s\n",
		    name, level, length, error.message);
		CHECK(false);
	}
	CHECK(fstat(fd, &st) == 0 && st.st_size > 0);
	out.length = out.capacity = (size_t)st.st_size;
	out.bytes = malloc(out.length);
	CHECK(out.bytes != NULL);
	CHECK(pread(fd, out.bytes, out.length, 0) == (ssize_t)out.length);
	CHECK(close(fd) == 0);
	fixture_free(&f);
	return (out);
}
#endif

#ifdef FSIMG_XZ
static buffer_t xz_of(const unsigned char *, size_t, lzma_check, bool);
static buffer_t xz_written(const unsigned char *, size_t, unsigned,
    fsimg_xz_check_t, uint64_t);
static void xz_expect_bytes(const buffer_t *, const unsigned char *, size_t);
static void xz_expect_corrupt(const buffer_t *);
static void test_xz_presets_and_checks_roundtrip_and_reject_damage(void);
static void test_xz_joined_streams_require_four_byte_padding(void);
static void test_xz_unchecked_blocks_report_incomplete_verification(void);
static void test_xz_headers_indexes_and_checks_reject_damage(void);
static void test_xz_detection_excludes_alone_and_peels_once(void);
static void test_xz_bomb_stops_before_crossing_spool_limit(void);
static void test_xz_large_dictionary_fails_before_spooling(void);
static void test_xz_allocation_failures_release_codec_state(void);
static void test_xz_decode_charges_compressed_and_decoded_bytes(void);
static void test_xz_preset_nine_exceeds_default_memory(void);
static void test_xz_plan_rejects_invalid_level_and_check(void);
static void test_xz_bound_covers_liblzma_bound_and_rejects_overflow(void);
static void xz_expect_stream_within_bound(const unsigned char *, size_t,
    unsigned, const char *);
static void
    test_xz_bound_covers_incompressible_and_barely_compressible_data(void);
static void test_xz_reproducible_bytes_ignore_memory_budget(void);
static void test_xz_plan_names_compression_and_container_libraries(void);
static void seed_xz(const char *);

static buffer_t
xz_written(const unsigned char *data, size_t length, unsigned level,
    fsimg_xz_check_t check, uint64_t budget)
{
	return (compressed_written(data, length, level, check, budget, "xz", false));
}

/* Independent liblzma fixtures include unchecked and multi-block streams.
 * FULL_FLUSH ends a block without ending the stream; the second half then
 * starts another block with its own check and index entry.
 */
static buffer_t
xz_of(const unsigned char *data, size_t length, lzma_check check, bool blocks)
{
	lzma_stream stream = LZMA_STREAM_INIT;
	unsigned char piece[64 * KIB];
	buffer_t out = { 0 };
	lzma_ret code;
	size_t half = blocks ? length / 2 : 0;

	CHECK(lzma_easy_encoder(&stream, 0, check) == LZMA_OK);
	for (unsigned phase = 0; phase < 2; phase++) {
		stream.next_in = phase == 0 ? data : data + half;
		stream.avail_in = phase == 0 ? half : length - half;
		do {
			stream.next_out = piece;
			stream.avail_out = sizeof(piece);
			code = lzma_code(&stream, phase == 0 ? LZMA_FULL_FLUSH : LZMA_FINISH);
			CHECK(code == LZMA_OK || code == LZMA_STREAM_END);
			append(&out, piece, sizeof(piece) - stream.avail_out);
		} while (code != LZMA_STREAM_END);
	}
	lzma_end(&stream);
	return (out);
}

static void
xz_expect_bytes(const buffer_t *xz, const unsigned char *data, size_t length)
{
	memory_file_t file = { .bytes = xz->bytes, .length = xz->length };
	fixture_t f;

	fixture_init(&f, 0);
	codec_context(&f, 128 * MIB, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	expect_disk(&f, data, length);
	fixture_free(&f);
}

static void
xz_expect_corrupt(const buffer_t *xz)
{
	memory_file_t file = { .bytes = xz->bytes, .length = xz->length };
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 0);
	codec_context(&f, 128 * MIB, 0);
	CHECK(open_file(&f, &file, "xz", NULL, spool_new(&f), &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "xz") == 0);
	CHECK(strstr(error.message, "compressed byte ") != NULL);
	fixture_free(&f);
}

static void
test_xz_presets_and_checks_roundtrip_and_reject_damage(void)
{
	static const unsigned levels[] = { 0, 6, 9 };
	unsigned char *data = pattern(512 * KIB, 11);
	buffer_t xz;

	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		for (unsigned check = FSIMG_XZ_CRC64; check <= FSIMG_XZ_SHA256; check++) {
			xz = xz_written(data, 512 * KIB, levels[i],
			    (fsimg_xz_check_t)check, 1024 * (uint64_t)MIB);
			xz_expect_bytes(&xz, data, 512 * KIB);
			/* Damage the block payload, retaining the signature. */
			xz.bytes[12 + ((size_t)xz.bytes[12] + 1) * 4 + 5] ^= 0x80;
			xz_expect_corrupt(&xz);
			free(xz.bytes);
		}
	}
	free(data);
}

static void
test_xz_joined_streams_require_four_byte_padding(void)
{
	unsigned char *data = pattern(128 * KIB, 12);
	buffer_t first = xz_of(data, 64 * KIB, LZMA_CHECK_CRC64, false);
	buffer_t second = xz_of(data + 64 * KIB, 64 * KIB, LZMA_CHECK_CRC32, false);
	buffer_t joined;

	for (unsigned padding = 0; padding <= 8; padding += 4) {
		joined = (buffer_t){ 0 };
		append(&joined, first.bytes, first.length);
		for (unsigned i = 0; i < padding; i++)
			append_byte(&joined, 0);
		append(&joined, second.bytes, second.length);
		xz_expect_bytes(&joined, data, 128 * KIB);
		append_byte(&joined, 0); /* A partial trailing padding group. */
		xz_expect_corrupt(&joined);
		free(joined.bytes);
	}
	joined = (buffer_t){ 0 };
	append(&joined, first.bytes, first.length);
	append(&joined, "\0\0\0\0\0", 5);
	append(&joined, second.bytes, second.length);
	xz_expect_corrupt(&joined);
	free(joined.bytes);
	free(first.bytes);
	free(second.bytes);
	free(data);
}

static void
test_xz_unchecked_blocks_report_incomplete_verification(void)
{
	unsigned char *data = calloc(1, 64 * KIB);
	fsimg_layer_report_t report;
	memory_file_t file;
	buffer_t xz;
	fixture_t f;

	CHECK(data != NULL);
	for (unsigned check = 0; check < 2; check++) {
		xz = xz_of(data, 64 * KIB, check == 0 ? LZMA_CHECK_NONE :
		    LZMA_CHECK_CRC64, true);
		file = (memory_file_t){ .bytes = xz.bytes, .length = xz.length };
		fixture_init(&f, 0);
		CHECK(open_file(&f, &file, "", "fat12/-/-", spool_new(&f),
		    NULL) == FSIMG_OK);
		expect_disk(&f, data, 64 * KIB);
		CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete == (check != 0));
		CHECK(strcmp(report.unverified, check == 0 ? "xz:check" : "") == 0);
		fixture_free(&f);
		xz.bytes[xz.length - 16] ^= 1; /* Index CRC32, even with check none. */
		xz_expect_corrupt(&xz);
		free(xz.bytes);
	}
	free(data);
}

static void
test_xz_headers_indexes_and_checks_reject_damage(void)
{
	unsigned char *data = pattern(64 * KIB, 13);
	buffer_t xz = xz_of(data, 64 * KIB, LZMA_CHECK_CRC64, false), cut;
	size_t index_size, index_start, header_end;
	size_t truncated_at[4];

	xz_expect_bytes(&xz, data, 64 * KIB);
	/* Footer backward size locates the index without assuming its length. */
	index_size = ((size_t)xz.bytes[xz.length - 8] |
	    (size_t)xz.bytes[xz.length - 7] << 8 |
	    (size_t)xz.bytes[xz.length - 6] << 16 |
	    (size_t)xz.bytes[xz.length - 5] << 24) + 1;
	index_start = xz.length - 12 - index_size * 4;
	xz.bytes[index_start - 1] ^= 1;
	xz_expect_corrupt(&xz);
	xz.bytes[index_start - 1] ^= 1;
	xz.bytes[xz.length - 16] ^= 1;
	xz_expect_corrupt(&xz);
	xz.bytes[xz.length - 16] ^= 1;
	xz.bytes[7] ^= 1;
	xz_expect_corrupt(&xz);
	xz.bytes[7] ^= 1;
	header_end = 12 + ((size_t)xz.bytes[12] + 1) * 4;
	for (size_t keep = 1; keep <= header_end; keep++) {
		cut = xz;
		cut.length = keep;
		/* Forcing xz requires its complete six-byte signature. */
		if (keep < 6) {
			CHECK(fsimg_xz_compression.probe(xz.bytes, keep, NULL) ==
			    FSIMG_EFORMAT);
			continue;
		}
		xz_expect_corrupt(&cut);
	}
	truncated_at[0] = header_end + 1;
	truncated_at[1] = index_start - 1;
	truncated_at[2] = index_start + 1;
	truncated_at[3] = xz.length - 1;

	for (size_t i = 0; i < sizeof(truncated_at) / sizeof(truncated_at[0]); i++) {
		cut = xz;
		cut.length = truncated_at[i];
		xz_expect_corrupt(&cut);
	}
	append(&xz, "garbage", 7);
	xz_expect_corrupt(&xz);
	free(xz.bytes);
	free(data);
}

static void
test_xz_detection_excludes_alone_and_peels_once(void)
{
	unsigned char *data = pattern(64 * KIB, 14);
	unsigned char alone[128 * KIB];
	lzma_stream stream = LZMA_STREAM_INIT;
	lzma_options_lzma settings;
	fsimg_image_info_t info;
	fsimg_error_t error;
	memory_file_t file;
	buffer_t gzip = gzip_of(data, 64 * KIB), inner, outer;
	fixture_t f;

	CHECK(!lzma_lzma_preset(&settings, 0));
	CHECK(lzma_alone_encoder(&stream, &settings) == LZMA_OK);
	stream.next_in = data;
	stream.avail_in = 64 * KIB;
	stream.next_out = alone;
	stream.avail_out = sizeof(alone);
	CHECK(lzma_code(&stream, LZMA_FINISH) == LZMA_STREAM_END);
	file = (memory_file_t){ .bytes = alone, .length = (size_t)stream.total_out };
	lzma_end(&stream);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", "fat12/-/-", spool_new(&f), NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.compression[0] == '\0');
	fixture_free(&f);
	fixture_init(&f, 0);
	file = (memory_file_t){ .bytes = gzip.bytes, .length = gzip.length };
	CHECK(open_file(&f, &file, "xz", NULL, spool_new(&f), &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "xz") == 0);
	fixture_free(&f);
	inner = xz_of(data, 64 * KIB, LZMA_CHECK_CRC64, false);
	outer = xz_of(inner.bytes, inner.length, LZMA_CHECK_CRC64, false);
	file = (memory_file_t){ .bytes = outer.bytes, .length = outer.length };
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	expect_disk(&f, inner.bytes, inner.length);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	fixture_free(&f);
	free(outer.bytes);
	free(inner.bytes);
	free(gzip.bytes);
	free(data);
}

static void
test_xz_bomb_stops_before_crossing_spool_limit(void)
{
	unsigned char *zeros = calloc(1, MIB + 1);
	fsimg_error_t error;
	memory_file_t file;
	struct stat st;
	buffer_t xz;
	fixture_t f;
	int spool, watch;

	CHECK(zeros != NULL);
	xz = xz_of(zeros, MIB + 1, LZMA_CHECK_CRC64, false);
	CHECK(xz.length < 16 * KIB);
	xz_expect_bytes(&xz, zeros, MIB + 1);
	file = (memory_file_t){ .bytes = xz.bytes, .length = xz.length };
	fixture_init(&f, MIB);
	spool = spool_new(&f);
	watch = dup(spool);
	CHECK(watch != -1);
	CHECK(open_file(&f, &file, "", NULL, spool, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL && error.limit == MIB);
	CHECK(fstat(watch, &st) == 0 && st.st_size > 0 && st.st_size <= MIB);
	CHECK(close(watch) == 0);
	fixture_free(&f);
	free(xz.bytes);
	free(zeros);
}

/* LZMA2 property 40 declares UINT32_MAX; 37 declares 1.5 GiB.
 * Change only that property and reseal the block header CRC32, so the
 * decoder must reach dictionary allocation rather than reject the header.
 */
static void
test_xz_large_dictionary_fails_before_spooling(void)
{
	unsigned char *data = pattern(64 * KIB, 15);
	buffer_t xz = xz_of(data, 64 * KIB, LZMA_CHECK_CRC64, false);
	fsimg_error_t error;
	memory_file_t file;
	struct stat st;
	fixture_t f;
	size_t header_bytes = ((size_t)xz.bytes[12] + 1) * 4;
	uint32_t crc;
	int spool, watch;

	xz_expect_bytes(&xz, data, 64 * KIB);
	CHECK(xz.bytes[13] == 0 && xz.bytes[14] == 0x21 && xz.bytes[15] == 1);
	for (unsigned property = 37; property <= 40; property += 3) {
		xz.bytes[16] = (unsigned char)property;
		crc = lzma_crc32(xz.bytes + 12, header_bytes - 4, 0);
		for (unsigned i = 0; i < 4; i++)
			xz.bytes[12 + header_bytes - 4 + i] = (unsigned char)(crc >> (i * 8));
		file = (memory_file_t){ .bytes = xz.bytes, .length = xz.length };
		fixture_init(&f, 0);
		codec_context(&f, 32 * MIB, 0);
		spool = spool_new(&f);
		watch = dup(spool);
		CHECK(watch != -1);
		CHECK(open_file(&f, &file, "", NULL, spool, &error) == FSIMG_ELIMIT);
		CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
		CHECK(strcmp(error.path, "xz") == 0);
		CHECK(fstat(watch, &st) == 0 && st.st_size == 0);
		CHECK(close(watch) == 0);
		fixture_free(&f);
	}
	free(xz.bytes);
	free(data);
}

static void
test_xz_allocation_failures_release_codec_state(void)
{
	unsigned char *data = pattern(64 * KIB, 16);
	buffer_t xz = xz_of(data, 64 * KIB, LZMA_CHECK_CRC64, true);
	fsimg_compression_settings_t settings = {
		.level = 0, .check = LZMA_CHECK_CRC64
	};
	fsimg_error_t error;
	fsimg_stats_t stats;
	fsimg_status_t status;
	memory_file_t file;
	fixture_t f;
	void *encoder;

	for (uint64_t attempt = 1;; attempt++) {
		CHECK(attempt < 256);
		fixture_init(&f, 0);
		f.fail_allocation = attempt;
		file = (memory_file_t){ .bytes = xz.bytes, .length = xz.length };
		status = open_file(&f, &file, "", NULL, spool_new(&f), &error);
		if (status == FSIMG_OK)
			expect_disk(&f, data, 64 * KIB);
		else
			CHECK(status == FSIMG_ENOMEM);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK(attempt > 1);
			break;
		}
	}
	fixture_init(&f, 0);
	for (uint64_t attempt = 1;; attempt++) {
		CHECK(attempt < 256);
		encoder = NULL;
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_xz_compression.encoder->open(f.ctx, &settings,
		    NULL, &encoder, &error);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		if (encoder != NULL)
			fsimg_xz_compression.encoder->close(encoder);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.memory_bytes == f.baseline_memory);
		if (status == FSIMG_OK) {
			CHECK(attempt > 1);
			break;
		}
		CHECK(status == FSIMG_ENOMEM);
	}
	fixture_free(&f);
	free(xz.bytes);
	free(data);
}

/* Invoke decode directly to separate codec work from image probing. */
static void
test_xz_decode_charges_compressed_and_decoded_bytes(void)
{
	unsigned char *data = pattern(MIB, 17);
	buffer_t xz = xz_of(data, MIB, LZMA_CHECK_CRC64, false);
	fsimg_spool_input_t input;
	fsimg_session_t session;
	fsimg_spool_t *spool;
	fsimg_error_t error;
	fixture_t f;
	bool unchecked;

	for (unsigned limited = 0; limited < 2; limited++) {
		fixture_init(&f, 0);
		codec_context(&f, 32 * MIB, limited != 0 ? MIB / 2 : 0);
		fsimg_session_init(&session, f.ctx, "xz", 0);
		input = (fsimg_spool_input_t){ .ctx = f.ctx, .session = &session,
		    .prefix = xz.bytes, .prefix_length = xz.length, .fd = -1 };
		/* Once prefix is consumed, use an empty file for end-of-input. */
		input.fd = open("/dev/null", O_RDONLY);
		CHECK(input.fd != -1);
		CHECK(fsimg_spool_adopt(f.ctx, spool_new(&f), &spool, NULL) == FSIMG_OK);
		if (limited == 0) {
			CHECK(fsimg_xz_compression.decode(&input, spool, &unchecked,
			    &error) == FSIMG_OK);
			CHECK(!unchecked);
			CHECK(session.progress.work == xz.length + MIB);
		} else {
			CHECK(fsimg_xz_compression.decode(&input, spool, &unchecked,
			    &error) == FSIMG_ELIMIT);
			CHECK(error.resource == FSIMG_RESOURCE_WORK);
		}
		CHECK(close(input.fd) == 0);
		fsimg_spool_free(spool);
		fixture_free(&f);
	}
	free(xz.bytes);
	free(data);
}

static void
test_xz_preset_nine_exceeds_default_memory(void)
{
	fsimg_compression_settings_t settings = {
		.level = 0, .check = LZMA_CHECK_CRC64
	};
	fsimg_error_t error;
	fixture_t f;
	void *encoder = NULL;

	fixture_init(&f, 0);
	codec_context(&f, 256 * MIB, 0);
	/* open never uses its sink: both outcomes happen before output. */
	CHECK(fsimg_xz_compression.encoder->open(f.ctx, &settings, NULL,
	    &encoder, &error) == FSIMG_OK);
	fsimg_xz_compression.encoder->close(encoder);
	encoder = NULL;

	settings.level = 9;
	CHECK(fsimg_xz_compression.encoder->open(f.ctx, &settings, NULL,
	    &encoder, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(strcmp(error.path, "xz") == 0);
	if (encoder != NULL)
		fsimg_xz_compression.encoder->close(encoder);
	fixture_free(&f);
}

static void
test_xz_plan_rejects_invalid_level_and_check(void)
{
	fsimg_layer_options_t layers;
	fsimg_compression_settings_t settings;
	fsimg_error_t error;

	fsimg_layer_options_init(&layers);
	CHECK(fsimg_xz_compression.plan(&layers, FSIMG_WRITE_REPRODUCIBLE,
	    0, &settings, &error) == FSIMG_OK);
	CHECK(settings.level == 6 && settings.check == LZMA_CHECK_CRC64);
	layers.xz_level = 10;
	CHECK(fsimg_xz_compression.plan(&layers, 0, 0, &settings, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "xz:level") == 0);
	layers.xz_level = 6;
	for (unsigned check = FSIMG_XZ_SHA256 + 1;
	    check <= FSIMG_XZ_SHA256 + 2; check++) {
		layers.xz_check = (fsimg_xz_check_t)check;
		CHECK(fsimg_xz_compression.plan(&layers, 0, 0, &settings,
		    &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "xz:check") == 0);
	}
}

/* liblzma's independent bound must fit without copying fsimg's formula. */
static void
test_xz_bound_covers_liblzma_bound_and_rejects_overflow(void)
{
	fsimg_error_t error;
	uint64_t bound, length;

	CHECK(fsimg_xz_compression.bound(0, &bound, &error) == FSIMG_OK);
	CHECK(bound >= lzma_stream_buffer_bound(0));
	for (unsigned power = 0; power <= 40; power++) {
		length = UINT64_C(1) << power;
		for (unsigned delta = 0; delta < 3; delta++) {
			if (fsimg_xz_compression.bound(length + delta, &bound,
			    &error) != FSIMG_OK || (length + delta <= SIZE_MAX &&
			    bound < lzma_stream_buffer_bound((size_t)(length + delta)))) {
				fprintf(stderr, "xz bound length %llu\n",
				    (unsigned long long)(length + delta));
				CHECK(false);
			}
		}
	}
	CHECK(fsimg_xz_compression.bound(UINT64_MAX, &bound, &error) ==
	    FSIMG_EUNSUPPORTED);
}

/* Measure the production streaming encoder, then decode independently with
 * liblzma so an omitted input piece cannot make the bound appear to hold.
 * The 1 GiB context budget holds preset 9's 674 MiB encoder.
 */
static void
xz_expect_stream_within_bound(const unsigned char *data, size_t length,
    unsigned level, const char *kind)
{
	unsigned char *decoded = malloc(length + 1);
	fsimg_error_t error;
	buffer_t xz;
	uint64_t bound, memory_limit = UINT64_MAX;
	size_t input_position = 0, output_position = 0;
	lzma_ret code;

	CHECK(decoded != NULL);
	xz = xz_written(data, length, level, FSIMG_XZ_SHA256,
	    UINT64_C(1024) * MIB);
	CHECK(fsimg_xz_compression.bound(length, &bound, &error) == FSIMG_OK);
	if (xz.length > bound) {
		fprintf(stderr, "xz %s preset %u length %zu: output %zu "
		    "exceeds bound %llu\n", kind, level, length, xz.length,
		    (unsigned long long)bound);
		CHECK(xz.length <= bound);
	}
	code = lzma_stream_buffer_decode(&memory_limit, 0, NULL, xz.bytes,
	    &input_position, xz.length, decoded, &output_position, length + 1);
	if (code != LZMA_OK || input_position != xz.length ||
	    output_position != length || bcmp(decoded, data, length) != 0) {
		fprintf(stderr, "xz %s preset %u length %zu: independent "
		    "decode must consume the stream and reproduce every byte\n",
		    kind, level, length);
		CHECK(false);
	}
	xz.bytes[xz.length - 16] ^= 1;
	xz_expect_corrupt(&xz);
	free(xz.bytes);
	free(decoded);
}

static void
test_xz_bound_covers_incompressible_and_barely_compressible_data(void)
{
	static const unsigned levels[] = { 0, 6, 9 };
	static const size_t lengths[] = { 0, 1, 100, 65535, 65536, 65537,
	    3 * MIB };
	static const char *const kinds[] = { "random", "barely compressible" };
	unsigned char *data = malloc(3 * MIB);
	uint32_t state = 0x12345678;

	CHECK(data != NULL);
	for (unsigned kind = 0; kind < 2; kind++) {
		/* Xorshift bytes resist compression; repeating the previous
		 * byte every 16 bytes makes the second input barely compressible.
		 */
		for (size_t i = 0; i < 3 * MIB; i++) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			data[i] = kind != 0 && i % 16 == 15 ? data[i - 1] :
			    (unsigned char)state;
		}
		for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
			for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); j++)
				xz_expect_stream_within_bound(data, lengths[j],
				    levels[i], kinds[kind]);
		}
	}
	free(data);
}

static void
test_xz_reproducible_bytes_ignore_memory_budget(void)
{
	unsigned char *data = pattern(MIB, 18);
	buffer_t first = xz_written(data, MIB, 6, FSIMG_XZ_CRC64, 128 * MIB);
	buffer_t second = xz_written(data, MIB, 6, FSIMG_XZ_CRC64, 256 * MIB);

	CHECK(first.length == second.length);
	CHECK(bcmp(first.bytes, second.bytes, first.length) == 0);
	second.bytes[second.length - 16] ^= 1;
	xz_expect_corrupt(&second);
	free(first.bytes);
	free(second.bytes);
	free(data);
}

static void
test_xz_plan_names_compression_and_container_libraries(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_error_t error;
	char version[128];
	fixture_t f;

	fixture_init(&f, 0);
	fsimg_layer_options_init(&layers);
	fsimg_write_options_init(&options);
	options.layers = &layers;
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.volume_bytes = MIB;
	strcpy(options.compression, "xz");
	for (unsigned container = 0; container < 2; container++) {
		CHECK(fsimg_writer_new(f.ctx, container != 0 ? "fat12/dmg" :
		    "fat12", &options, &writer, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		if (container != 0)
			snprintf(version, sizeof(version), "zlib %s, liblzma %s",
			    zlibVersion(), lzma_version_string());
		else
			snprintf(version, sizeof(version), "liblzma %s",
			    lzma_version_string());
		CHECK(strcmp(info.codec_version, version) == 0);
		fsimg_writer_free(writer);

		layers.xz_check = (fsimg_xz_check_t)(FSIMG_XZ_SHA256 + 1);
		CHECK(fsimg_writer_new(f.ctx, container != 0 ? "fat12/dmg" :
		    "fat12", &options, &writer, &error) == FSIMG_EINVAL);
		CHECK(writer == NULL && strcmp(error.path, "xz:check") == 0);
		layers.xz_check = FSIMG_XZ_CRC64;
	}
	fixture_free(&f);
}

static void
seed_xz(const char *directory)
{
	unsigned char *data = pattern(32 * KIB, 19);
	buffer_t xz, second;
	char name[64];

	for (unsigned level = 0; level <= 6; level += 6) {
		for (unsigned check = FSIMG_XZ_CRC64; check <= FSIMG_XZ_SHA256; check++) {
			xz = xz_written(data, 32 * KIB, level,
		    (fsimg_xz_check_t)check, 128 * MIB);
			snprintf(name, sizeof(name), "preset-%u-check-%u.seed", level, check);
			write_seed_buffer(directory, name, &xz);
			free(xz.bytes);
		}
	}
	xz = xz_of(data, 16 * KIB, LZMA_CHECK_CRC64, false);
	second = xz_of(data + 16 * KIB, 16 * KIB, LZMA_CHECK_CRC32, false);
	append(&xz, "\0\0\0\0", 4);
	append(&xz, second.bytes, second.length);
	write_seed_buffer(directory, "padded-streams.seed", &xz);
	free(xz.bytes);
	free(second.bytes);
	xz = xz_of(data, 32 * KIB, LZMA_CHECK_NONE, false);
	write_seed_buffer(directory, "check-none.seed", &xz);
	free(xz.bytes);
	xz = xz_of(data, 32 * KIB, LZMA_CHECK_CRC64, true);
	write_seed_buffer(directory, "multi-block.seed", &xz);
	free(xz.bytes);
	free(data);
}
#endif

static void
test_zstd_registry_reports_build_support(void)
{
	fsimg_layer_info_t info;
	fsimg_error_t error;
	unsigned char raw[64] = { 0 };
	memory_file_t file = { .bytes = raw, .length = sizeof(raw) };
	fixture_t f;
	size_t cursor = 0;
	bool found = false;

	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		if (strcmp(info.name, "zstd") != 0)
			continue;
		found = true;
		CHECK(strcmp(info.settings, "level") == 0);
#ifdef FSIMG_ZSTD
		char version[64];

		CHECK(info.capabilities ==
		    (FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY));
		snprintf(version, sizeof(version), "zstd %s", ZSTD_versionString());
		CHECK(strcmp(info.codec_version, version) == 0);
#else
		CHECK(info.capabilities == 0 && info.codec_version[0] == '\0');
#endif
	}
	CHECK(found);
	fixture_init(&f, 0);
#ifdef FSIMG_ZSTD
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f), &error) ==
	    FSIMG_EFORMAT);
#else
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f), &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strstr(error.message,
	    "reading this compression is not supported") != NULL);
#endif
	CHECK(strcmp(error.path, "zstd") == 0);
	fixture_free(&f);
}

#ifdef FSIMG_ZSTD
/* Keep failures in parameter sweeps tied to the input being exercised. */
#define CHECK_ZSTD_CASE(condition, value) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s = %llu: %s\n", #value, \
		    (unsigned long long)(value), #condition); \
		CHECK(false); \
	} \
} while (0)

static buffer_t zstd_raw_frame(void);
static buffer_t zstd_of(const unsigned char *, size_t, unsigned, bool, bool);
static void zstd_expect_bytes(const buffer_t *, const unsigned char *, size_t);
static uint64_t zstd_expect_failure(const buffer_t *, fsimg_status_t,
    const char *);
static void zstd_skip(buffer_t *);
static void test_zstd_levels_roundtrip_with_checksum_and_content_size(void);
static void test_zstd_concatenation_and_skippable_frames(void);
static void test_zstd_checksum_and_content_size_are_optional(void);
static void test_zstd_raw_frames_validate_sizes_headers_and_termination(void);
static void test_zstd_window_limits_precede_spooling(void);
static void test_zstd_header_refusals_at_each_frame_start(void);
static void test_zstd_legacy_frames_are_never_decoded(void);
static void test_zstd_bomb_stops_before_crossing_spool_limit(void);
static void test_zstd_detection_forces_signature_and_peels_once(void);
static void test_zstd_allocation_failures_release_codec_state(void);
static void test_zstd_decode_charges_compressed_and_decoded_bytes(void);
static void test_zstd_decode_stops_at_work_limit(void);
static void test_zstd_encoder_unknown_length_dmg_refuses_large_workspace_before_output(void);
static void test_zstd_encoder_known_size_fits_16_mib(void);
static void test_zstd_short_transcode_fails_at_encoder_end(void);
static void test_zstd_exact_transcode_finishes(void);
static void test_zstd_level_defaults_validation_and_reproducibility(void);
static void test_zstd_bound_covers_library_and_incompressible_output(void);
static void test_zstd_plan_names_compression_and_container_libraries(void);
static void seed_zstd(const char *);

/* Independent one-shot frames vary flags that fsimg always enables. */
static buffer_t
zstd_of(const unsigned char *data, size_t length, unsigned level,
    bool checksum, bool content_size)
{
	ZSTD_CCtx *ctx = ZSTD_createCCtx();
	buffer_t out = { 0 };
	size_t result;

	CHECK(ctx != NULL);
	CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx,
	    ZSTD_c_compressionLevel, (int)level)));
	CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx,
	    ZSTD_c_checksumFlag, checksum)));
	CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx,
	    ZSTD_c_contentSizeFlag, content_size)));
	out.capacity = ZSTD_compressBound(length);
	out.bytes = malloc(out.capacity);
	CHECK(out.bytes != NULL);
	result = ZSTD_compress2(ctx, out.bytes, out.capacity, data, length);
	CHECK(!ZSTD_isError(result));
	out.length = result;
	CHECK(ZSTD_freeCCtx(ctx) == 0);
	return (out);
}

static void
zstd_expect_bytes(const buffer_t *encoded, const unsigned char *data,
    size_t length)
{
	memory_file_t file = { .bytes = encoded->bytes, .length = encoded->length };
	fixture_t f;

	fixture_init(&f, 0);
	codec_context(&f, 256 * MIB, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	expect_disk(&f, data, length);
	fixture_free(&f);
}

/* Direct decode also exercises prefixes shorter than the magic, which
 * the public forced-image probe correctly classifies as EFORMAT. Return
 * the spooled byte count so frame refusals can check that no payload leaked.
 */
static uint64_t
zstd_expect_failure(const buffer_t *encoded, fsimg_status_t expected,
    const char *detail)
{
	fsimg_spool_input_t input;
	fsimg_session_t session;
	fsimg_spool_t *spool;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	struct stat st;
	fixture_t f;
	int fd, watch;
	bool unchecked;

	fixture_init(&f, 0);
	fsimg_session_init(&session, f.ctx, "zstd", 0);
	input = (fsimg_spool_input_t){ .ctx = f.ctx, .session = &session,
	    .prefix = encoded->bytes, .prefix_length = encoded->length };
	input.fd = open("/dev/null", O_RDONLY);
	CHECK(input.fd != -1);
	fd = spool_new(&f);
	watch = dup(fd);
	CHECK(watch != -1);
	CHECK(fsimg_spool_adopt(f.ctx, fd, &spool, NULL) == FSIMG_OK);
	status = fsimg_zstd_compression.decode(&input, spool, &unchecked, &error);
	if (status != expected || strstr(error.message, detail) == NULL) {
		fprintf(stderr, "zstd length %zu: expected %d containing '%s', got %d: %s\n",
		    encoded->length, expected, detail, status, error.message);
		CHECK(false);
	}
	CHECK(close(input.fd) == 0);
	CHECK(fstat(watch, &st) == 0);
	CHECK(close(watch) == 0);
	fsimg_spool_free(spool);
	fixture_free(&f);
	return ((uint64_t)st.st_size);
}

static void
zstd_skip(buffer_t *out)
{
	append_le32(out, UINT32_C(0x184d2a5f));
	append_le32(out, 3);
	append(out, "tag", 3);
}

static void
test_zstd_levels_roundtrip_with_checksum_and_content_size(void)
{
	static const unsigned levels[] = { 1, 3, 19 };
	unsigned char *data = pattern(512 * KIB, 20);
	ZSTD_frameHeader header;
	buffer_t encoded;

	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		encoded = compressed_written(data, 512 * KIB, levels[i],
		    FSIMG_XZ_CRC64, 256 * MIB, "zstd", false);
		CHECK_ZSTD_CASE(ZSTD_getFrameHeader(&header, encoded.bytes,
		    encoded.length) == 0, levels[i]);
		CHECK_ZSTD_CASE(header.frameContentSize == 512 * KIB &&
		    header.checksumFlag == 1, levels[i]);
		CHECK_ZSTD_CASE(ZSTD_findFrameCompressedSize(encoded.bytes,
		    encoded.length) == encoded.length, levels[i]);
		zstd_expect_bytes(&encoded, data, 512 * KIB);
		encoded.bytes[encoded.length - 1] ^= 1;
		zstd_expect_failure(&encoded, FSIMG_ECORRUPT, "checksum does not match at compressed byte");
		free(encoded.bytes);
	}
	encoded = compressed_written(data, 512 * KIB, 3, FSIMG_XZ_CRC64,
	    256 * MIB, "zstd", true);
	CHECK(ZSTD_getFrameHeader(&header, encoded.bytes, encoded.length) == 0);
	CHECK(header.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN);
	CHECK(header.checksumFlag == 1);
	zstd_expect_bytes(&encoded, data, 512 * KIB);
	encoded.bytes[encoded.length - 1] ^= 1;
	zstd_expect_failure(&encoded, FSIMG_ECORRUPT, "compressed byte");
	free(encoded.bytes);
	free(data);
}

static void
test_zstd_concatenation_and_skippable_frames(void)
{
	unsigned char *data = pattern(128 * KIB, 21);
	buffer_t first = zstd_of(data, 64 * KIB, 1, true, true);
	buffer_t second = zstd_of(data + 64 * KIB, 64 * KIB, 3, true, false);
	buffer_t joined;
	char offset[64];

	for (unsigned mode = 0; mode < 3; mode++) {
		joined = (buffer_t){ 0 };
		if (mode == 1)
			zstd_skip(&joined);
		append(&joined, first.bytes, first.length);
		if (mode == 2)
			zstd_skip(&joined);
		append(&joined, second.bytes, second.length);
		zstd_expect_bytes(&joined, data, 128 * KIB);
		snprintf(offset, sizeof(offset), "at compressed byte %zu", joined.length);
		append(&joined, "garbage", 7);
		zstd_expect_failure(&joined, FSIMG_ECORRUPT, offset);
		joined.length -= 7;
		append(&joined, "\0\0\0\0", 4);
		zstd_expect_failure(&joined, FSIMG_ECORRUPT, "compressed byte");
		free(joined.bytes);
	}
	free(first.bytes);
	free(second.bytes);
	free(data);
}

static void
test_zstd_checksum_and_content_size_are_optional(void)
{
	unsigned char data[64 * KIB] = { 0 };
	fsimg_layer_report_t report;
	memory_file_t file;
	ZSTD_frameHeader header;
	buffer_t encoded;
	fixture_t f;

	for (unsigned flags = 0; flags < 4; flags++) {
		encoded = zstd_of(data, sizeof(data), 3, (flags & 1) != 0,
		    (flags & 2) != 0);
		CHECK_ZSTD_CASE(ZSTD_getFrameHeader(&header, encoded.bytes,
		    encoded.length) == 0, flags);
		CHECK_ZSTD_CASE(header.checksumFlag == (flags & 1), flags);
		CHECK_ZSTD_CASE(header.frameContentSize == ((flags & 2) != 0 ?
		    sizeof(data) : ZSTD_CONTENTSIZE_UNKNOWN), flags);
		fixture_init(&f, 0);
		file = (memory_file_t){ .bytes = encoded.bytes, .length = encoded.length };
		CHECK_ZSTD_CASE(open_file(&f, &file, "", "fat12/-/-",
		    spool_new(&f), NULL) == FSIMG_OK, flags);
		expect_disk(&f, data, sizeof(data));
		CHECK_ZSTD_CASE(fsimg_image_verify(f.image, 0, &report,
		    NULL) == FSIMG_OK, flags);
		CHECK_ZSTD_CASE(report.complete == ((flags & 1) != 0), flags);
		CHECK_ZSTD_CASE(strcmp(report.unverified, (flags & 1) != 0 ?
		    "" : "zstd:checksum") == 0, flags);
		fixture_free(&f);
		encoded.length--;
		zstd_expect_failure(&encoded, FSIMG_ECORRUPT, "compressed byte");
		free(encoded.bytes);
	}
}

static buffer_t
zstd_raw_frame(void)
{
	static const unsigned char raw[] = {
	    0x28, 0xb5, 0x2f, 0xfd, 0x20, 3,
	    0x19, 0, 0, 'a', 'b', 'c'
	};
	buffer_t frame = { 0 };

	append(&frame, raw, sizeof(raw));
	return (frame);
}

/* RFC 8878: single segment, one-byte FCS, then a final raw block.
 * The block header is (size << 3) | last; it carries exactly three bytes.
 * No checksum masks the independent declared-size failures.
 */
static void
test_zstd_raw_frames_validate_sizes_headers_and_termination(void)
{
	buffer_t frame = zstd_raw_frame(), cut;

	zstd_expect_bytes(&frame, (const unsigned char *)"abc", 3);
	for (unsigned size = 2; size <= 4; size += 2) {
		frame.bytes[5] = (unsigned char)size;
		zstd_expect_failure(&frame, FSIMG_ECORRUPT, "compressed byte");
	}
	frame.bytes[5] = 3;
	for (size_t keep = 1; keep <= 9; keep++) {
		cut = frame;
		cut.length = keep;
		zstd_expect_failure(&cut, FSIMG_ECORRUPT, "compressed byte");
	}
	free(frame.bytes);
}

static void
test_zstd_window_limits_precede_spooling(void)
{
	/* Non-single-segment header, followed by a final raw block. */
	unsigned char raw[] = { 0x28, 0xb5, 0x2f, 0xfd, 0, 0x80,
	    0x19, 0, 0, 'a', 'b', 'c' };
	unsigned char dictionary[] = { 0x28, 0xb5, 0x2f, 0xfd,
	    0x21, 1, 3, 0x19, 0, 0, 'a', 'b', 'c' };
	buffer_t frame = { 0 };
	buffer_t dict = { .bytes = dictionary, .length = sizeof(dictionary) };
	memory_file_t file;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	struct stat st;
	fixture_t f;
	int spool, watch;
	static const unsigned char allowed_windows[] = { 0x80, 0x88 };
	static const unsigned char refused_windows[] = { 0x89, 0x90 };

	append(&frame, raw, sizeof(raw));
	zstd_expect_bytes(&frame, (const unsigned char *)"abc", 3);
	/* 64 MiB and exactly 128 MiB are allowed, but exceed this budget. */
	for (size_t i = 0; i < sizeof(allowed_windows); i++) {
		error = (fsimg_error_t){ 0 };
		frame.bytes[5] = allowed_windows[i];
		file = (memory_file_t){ .bytes = frame.bytes, .length = frame.length };
		fixture_init(&f, 0);
		codec_context(&f, 32 * MIB, 0);
		spool = spool_new(&f);
		watch = dup(spool);
		CHECK_ZSTD_CASE(watch != -1, allowed_windows[i]);
		status = open_file(&f, &file, "", NULL, spool, &error);
		if (status != FSIMG_ELIMIT || error.resource != FSIMG_RESOURCE_MEMORY) {
			fprintf(stderr, "window descriptor 0x%02x: status %d, resource %d\n",
			    allowed_windows[i], status, error.resource);
			CHECK_ZSTD_CASE(false, allowed_windows[i]);
		}
		CHECK_ZSTD_CASE(strcmp(error.path, "zstd") == 0, allowed_windows[i]);
		CHECK_ZSTD_CASE(fstat(watch, &st) == 0 && st.st_size == 0, allowed_windows[i]);
		CHECK_ZSTD_CASE(close(watch) == 0, allowed_windows[i]);
		fixture_free(&f);
	}
	/* 144 MiB and 256 MiB exceed the format profile's 128 MiB cap. */
	for (size_t i = 0; i < sizeof(refused_windows); i++) {
		frame.bytes[5] = refused_windows[i];
		zstd_expect_failure(&frame, FSIMG_EUNSUPPORTED, "window exceeds 128 MiB");
	}
	zstd_expect_failure(&dict, FSIMG_EUNSUPPORTED, "needs a dictionary");
	free(frame.bytes);
}

static void
test_zstd_header_refusals_at_each_frame_start(void)
{
	typedef struct header_case {
		const char *name;
		unsigned char descriptor;
		unsigned char size_or_window;
		const char *detail;
	} header_case_t;
	static const header_case_t cases[] = {
		{ "windowLog 33", 0, 0xb8, "window exceeds 128 MiB" },
		{ "reserved FHD bit", 0x28, 3,
		    "parameter this libzstd does not decode" }
	};
	buffer_t valid = zstd_raw_frame();
	buffer_t refused = { 0 }, joined = { 0 };
	uint64_t spooled;

	zstd_expect_bytes(&valid, (const unsigned char *)"abc", 3);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		refused.length = 0;
		append(&refused, valid.bytes, valid.length);
		refused.bytes[4] = cases[i].descriptor;
		refused.bytes[5] = cases[i].size_or_window;
		spooled = zstd_expect_failure(&refused, FSIMG_EUNSUPPORTED,
		    cases[i].detail);
		if (spooled != 0) {
			fprintf(stderr, "%s at first frame: spooled %llu\n",
			    cases[i].name, (unsigned long long)spooled);
			CHECK_ZSTD_CASE(false, i);
		}
		joined.length = 0;
		append(&joined, valid.bytes, valid.length);
		append(&joined, refused.bytes, refused.length);
		spooled = zstd_expect_failure(&joined, FSIMG_EUNSUPPORTED,
		    cases[i].detail);
		if (spooled != 3) {
			fprintf(stderr, "%s at second frame: spooled %llu\n",
			    cases[i].name, (unsigned long long)spooled);
			CHECK_ZSTD_CASE(false, i);
		}
	}
	free(joined.bytes);
	free(refused.bytes);
	free(valid.bytes);
}

static void
test_zstd_legacy_frames_are_never_decoded(void)
{
	/* Hand-built v0.7: single-segment FHD, content size 3, a raw block
	 * (type 1 in the top two bits, big-endian size), then an end block.
	 * Its payload differs from the current-format frame's payload.
	 */
	unsigned char raw[] = { 0x27, 0xb5, 0x2f, 0xfd, 0x20, 3,
	    0x40, 0, 3, 'x', 'y', 'z', 0xc0, 0, 0 };
	buffer_t legacy = { .bytes = raw, .length = sizeof(raw) };
	buffer_t valid = zstd_raw_frame(), joined = { 0 };
	memory_file_t file = { .bytes = raw, .length = sizeof(raw) };
	fsimg_error_t error;
	fixture_t f;

	zstd_expect_bytes(&valid, (const unsigned char *)"abc", 3);
	append(&joined, valid.bytes, valid.length);
	append(&joined, legacy.bytes, legacy.length);
	CHECK(zstd_expect_failure(&joined, FSIMG_ECORRUPT,
	    "trailing bytes that are not a frame at compressed byte 12") == 3);
	CHECK(fsimg_zstd_compression.probe(raw, sizeof(raw), &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no zstd signature") == 0);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f), &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "zstd") == 0);
	CHECK(strcmp(error.message, "no zstd signature") == 0);
	fixture_free(&f);
	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	expect_disk(&f, raw, sizeof(raw));
	fixture_free(&f);
	free(joined.bytes);
	free(valid.bytes);
}

static void
test_zstd_bomb_stops_before_crossing_spool_limit(void)
{
	unsigned char *zeros = calloc(1, MIB + 1);
	fsimg_error_t error;
	memory_file_t file;
	struct stat st;
	buffer_t encoded;
	fixture_t f;
	int spool, watch;

	CHECK(zeros != NULL);
	encoded = zstd_of(zeros, MIB + 1, 3, true, true);
	zstd_expect_bytes(&encoded, zeros, MIB + 1);
	fixture_init(&f, MIB);
	file = (memory_file_t){ .bytes = encoded.bytes, .length = encoded.length };
	spool = spool_new(&f);
	watch = dup(spool);
	CHECK(watch != -1);
	CHECK(open_file(&f, &file, "", NULL, spool, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_SPOOL && error.limit == MIB);
	CHECK(fstat(watch, &st) == 0 && st.st_size > 0 && st.st_size <= MIB);
	CHECK(close(watch) == 0);
	fixture_free(&f);
	free(encoded.bytes);
	free(zeros);
}

static void
test_zstd_detection_forces_signature_and_peels_once(void)
{
	unsigned char *data = pattern(64 * KIB, 22);
	buffer_t gzip = gzip_of(data, 64 * KIB);
	buffer_t inner = zstd_of(data, 64 * KIB, 3, true, true);
	buffer_t outer = zstd_of(inner.bytes, inner.length, 3, true, true);
	memory_file_t file = { .bytes = gzip.bytes, .length = gzip.length };
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f, 0);
	CHECK(open_file(&f, &file, "zstd", NULL, spool_new(&f), &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "zstd") == 0);
	fixture_free(&f);
	fixture_init(&f, 0);
	file = (memory_file_t){ .bytes = outer.bytes, .length = outer.length };
	CHECK(open_file(&f, &file, "", NULL, spool_new(&f), NULL) == FSIMG_OK);
	expect_disk(&f, inner.bytes, inner.length);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	fixture_free(&f);
	free(gzip.bytes);
	free(inner.bytes);
	free(outer.bytes);
	free(data);
}

static void
test_zstd_allocation_failures_release_codec_state(void)
{
	unsigned char *data = pattern(64 * KIB, 23);
	buffer_t encoded = zstd_of(data, 64 * KIB, 3, true, true);
	fsimg_status_t status;
	fsimg_error_t error;
	memory_file_t file;
	fixture_t f;
	int fd;

	for (uint64_t attempt = 1;; attempt++) {
		CHECK_ZSTD_CASE(attempt < 256, attempt);
		fixture_init(&f, 0);
		f.fail_allocation = attempt;
		file = (memory_file_t){ .bytes = encoded.bytes, .length = encoded.length };
		status = open_file(&f, &file, "", NULL, spool_new(&f), &error);
		if (status == FSIMG_OK)
			expect_disk(&f, data, 64 * KIB);
		else
			CHECK_ZSTD_CASE(status == FSIMG_ENOMEM, attempt);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK_ZSTD_CASE(attempt > 1, attempt);
			break;
		}
	}
	file = (memory_file_t){ .bytes = data, .length = 64 * KIB };
	for (uint64_t attempt = 1;; attempt++) {
		CHECK_ZSTD_CASE(attempt < 256, attempt);
		fixture_init(&f, 0);
		f.fail_allocation = attempt;
		fd = spool_new(&f);
		status = compressed_output(&f, &file, 3, FSIMG_XZ_CRC64,
		    "zstd", false, fd, &error);
		CHECK_ZSTD_CASE(close(fd) == 0, attempt);
		fixture_free(&f);
		if (status == FSIMG_OK) {
			CHECK_ZSTD_CASE(attempt > 1, attempt);
			break;
		}
		CHECK_ZSTD_CASE(status == FSIMG_ENOMEM, attempt);
	}
	free(encoded.bytes);
	free(data);
}

static void
test_zstd_decode_charges_compressed_and_decoded_bytes(void)
{
	unsigned char *data = pattern(MIB, 24);
	buffer_t encoded = zstd_of(data, MIB, 3, true, true);
	fsimg_spool_input_t input;
	fsimg_session_t session;
	fsimg_spool_t *spool;
	fsimg_error_t error;
	fixture_t f;
	bool unchecked;

	fixture_init(&f, 0);
	codec_context(&f, 32 * MIB, 0);
	fsimg_session_init(&session, f.ctx, "zstd", 0);
	input = (fsimg_spool_input_t){ .ctx = f.ctx, .session = &session,
	    .prefix = encoded.bytes, .prefix_length = encoded.length };
	input.fd = open("/dev/null", O_RDONLY);
	CHECK(input.fd != -1);
	CHECK(fsimg_spool_adopt(f.ctx, spool_new(&f), &spool, NULL) == FSIMG_OK);
	CHECK(fsimg_zstd_compression.decode(&input, spool, &unchecked,
	    &error) == FSIMG_OK);
	CHECK(!unchecked);
	CHECK(session.progress.work == encoded.length + MIB);
	CHECK(close(input.fd) == 0);
	fsimg_spool_free(spool);
	fixture_free(&f);
	free(encoded.bytes);
	free(data);
}

static void
test_zstd_decode_stops_at_work_limit(void)
{
	unsigned char *data = pattern(MIB, 24);
	buffer_t encoded = zstd_of(data, MIB, 3, true, true);
	fsimg_spool_input_t input;
	fsimg_session_t session;
	fsimg_spool_t *spool;
	fsimg_error_t error;
	fixture_t f;
	bool unchecked;

	fixture_init(&f, 0);
	codec_context(&f, 32 * MIB, MIB / 2);
	fsimg_session_init(&session, f.ctx, "zstd", 0);
	input = (fsimg_spool_input_t){ .ctx = f.ctx, .session = &session,
	    .prefix = encoded.bytes, .prefix_length = encoded.length };
	input.fd = open("/dev/null", O_RDONLY);
	CHECK(input.fd != -1);
	CHECK(fsimg_spool_adopt(f.ctx, spool_new(&f), &spool, NULL) == FSIMG_OK);
	CHECK(fsimg_zstd_compression.decode(&input, spool, &unchecked,
	    &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	CHECK(close(input.fd) == 0);
	fsimg_spool_free(spool);
	fixture_free(&f);
	free(encoded.bytes);
	free(data);
}

static void
test_zstd_encoder_known_size_fits_16_mib(void)
{
	unsigned char data[64 * KIB] = { 0 };
	memory_file_t file = { .bytes = data, .length = sizeof(data) };
	fsimg_error_t error;
	struct stat st;
	fixture_t f;
	int fd;

	fixture_init(&f, 0);
	codec_context(&f, 16 * MIB, 0);
	fd = spool_new(&f);
	CHECK(compressed_output(&f, &file, 19, FSIMG_XZ_CRC64,
	    "zstd", false, fd, &error) == FSIMG_OK);
	CHECK(fstat(fd, &st) == 0 && st.st_size > 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
}

static void
test_zstd_encoder_unknown_length_dmg_refuses_large_workspace_before_output(void)
{
	unsigned char data[64 * KIB] = { 0 };
	memory_file_t file = { .bytes = data, .length = sizeof(data) };
	fsimg_error_t error;
	struct stat st;
	fixture_t f;
	int fd;

	fixture_init(&f, 0);
	codec_context(&f, 16 * MIB, 0);
	fd = spool_new(&f);
	CHECK(compressed_output(&f, &file, 19, FSIMG_XZ_CRC64,
	    "zstd", true, fd, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(strcmp(error.path, "zstd") == 0);
	CHECK(fstat(fd, &st) == 0 && st.st_size == 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
}

static void
test_zstd_exact_transcode_finishes(void)
{
	unsigned char data[64 * KIB] = { 0 };
	memory_file_t file = { .bytes = data, .length = sizeof(data) };
	fsimg_error_t error;
	fixture_t f;
	int fd;

	fixture_init(&f, 0);
	f.overstate_compression_length = false;
	fd = spool_new(&f);
	CHECK(compressed_output(&f, &file, 3, FSIMG_XZ_CRC64,
	    "zstd", false, fd, &error) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fixture_free(&f);
}

static void
test_zstd_short_transcode_fails_at_encoder_end(void)
{
	unsigned char data[64 * KIB] = { 0 };
	memory_file_t file = { .bytes = data, .length = sizeof(data) };
	fsimg_error_t error;
	fixture_t f;
	int fd;

	fixture_init(&f, 0);
	f.overstate_compression_length = true;
	fd = spool_new(&f);
	CHECK(compressed_output(&f, &file, 3, FSIMG_XZ_CRC64,
	    "zstd", false, fd, &error) == FSIMG_ESTATE);
	CHECK(strstr(error.message, "ZSTD_compressStream2") != NULL);
	CHECK(close(fd) == 0);
	fixture_free(&f);
}

static void
test_zstd_level_defaults_validation_and_reproducibility(void)
{
	unsigned char *data = pattern(MIB, 25);
	buffer_t first = compressed_written(data, MIB, 0, FSIMG_XZ_CRC64,
	    32 * MIB, "zstd", false);
	buffer_t second = compressed_written(data, MIB, 3, FSIMG_XZ_CRC64,
	    64 * MIB, "zstd", false);
	fsimg_compression_settings_t settings;
	fsimg_layer_options_t layers;
	fsimg_error_t error;

	CHECK(first.length == second.length);
	CHECK(bcmp(first.bytes, second.bytes, first.length) == 0);
	zstd_expect_bytes(&first, data, MIB);
	second.bytes[second.length - 1] ^= 1;
	zstd_expect_failure(&second, FSIMG_ECORRUPT, "compressed byte");
	fsimg_layer_options_init(&layers);
	layers.zstd_level = 0;
	CHECK(fsimg_zstd_compression.plan(&layers, 0, 0, &settings, &error) == FSIMG_OK);
	CHECK(settings.level == 3);
	layers.zstd_level = 20;
	CHECK(fsimg_zstd_compression.plan(&layers, 0, 0, &settings, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "zstd:level") == 0);
	free(first.bytes);
	free(second.bytes);
	free(data);
}

static void
test_zstd_bound_covers_library_and_incompressible_output(void)
{
	unsigned char *data = malloc(3 * MIB), *decoded = malloc(3 * MIB);
	uint64_t bound, length;
	uint32_t state = 0x12345678;
	buffer_t encoded;

	CHECK(data != NULL && decoded != NULL);
	CHECK(fsimg_zstd_compression.bound(0, &bound, NULL) == FSIMG_OK);
	CHECK(bound == 534 && bound >= ZSTD_compressBound(0));
	for (unsigned power = 0; power <= 40; power++) {
		length = UINT64_C(1) << power;
		for (unsigned delta = 0; delta < 3; delta++) {
			CHECK_ZSTD_CASE(fsimg_zstd_compression.bound(length + delta,
			    &bound, NULL) == FSIMG_OK, length + delta);
			CHECK_ZSTD_CASE(bound == length + delta +
			    (length + delta) / 256 + 534, length + delta);
			if (length + delta <= SIZE_MAX)
				CHECK_ZSTD_CASE(bound >= ZSTD_compressBound(
				    (size_t)(length + delta)), length + delta);
		}
	}
	CHECK(fsimg_zstd_compression.bound(UINT64_MAX, &bound, NULL) == FSIMG_EUNSUPPORTED);
	for (size_t i = 0; i < 3 * MIB; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data[i] = (unsigned char)state;
	}
	CHECK(fsimg_zstd_compression.bound(3 * MIB, &bound, NULL) == FSIMG_OK);
	for (unsigned level = 1; level <= 19; level += 18) {
		encoded = compressed_written(data, 3 * MIB, level, FSIMG_XZ_CRC64,
		    256 * MIB, "zstd", false);
		CHECK_ZSTD_CASE(encoded.length <= bound, level);
		CHECK_ZSTD_CASE(ZSTD_decompress(decoded, 3 * MIB, encoded.bytes,
		    encoded.length) == 3 * MIB, level);
		CHECK_ZSTD_CASE(bcmp(decoded, data, 3 * MIB) == 0, level);
		encoded.bytes[encoded.length - 1] ^= 1;
		zstd_expect_failure(&encoded, FSIMG_ECORRUPT, "compressed byte");
		free(encoded.bytes);
	}
	free(decoded);
	free(data);
}

static void
test_zstd_plan_names_compression_and_container_libraries(void)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	char version[128];
	fixture_t f;

	fixture_init(&f, 0);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.volume_bytes = MIB;
	strcpy(options.compression, "zstd");
	for (unsigned container = 0; container < 2; container++) {
		CHECK_ZSTD_CASE(fsimg_writer_new(f.ctx,
		    container != 0 ? "fat12/dmg" :
		    "fat12", &options, &writer, NULL) == FSIMG_OK, container);
		CHECK_ZSTD_CASE(fsimg_writer_plan(writer, &plan, NULL) ==
		    FSIMG_OK, container);
		CHECK_ZSTD_CASE(fsimg_plan_get_info(plan, &info, NULL) ==
		    FSIMG_OK, container);
		if (container != 0)
			snprintf(version, sizeof(version), "zlib %s, zstd %s",
			    zlibVersion(), ZSTD_versionString());
		else
			snprintf(version, sizeof(version), "zstd %s", ZSTD_versionString());
		CHECK_ZSTD_CASE(strcmp(info.codec_version, version) == 0, container);
		fsimg_writer_free(writer);
	}
	fixture_free(&f);
}

static void
seed_zstd(const char *directory)
{
	static const unsigned levels[] = { 1, 3, 19 };
	unsigned char *data = pattern(32 * KIB, 26);
	buffer_t frame, first, second, joined = { 0 };
	char name[64];

	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		for (unsigned flags = 0; flags < 4; flags++) {
			frame = zstd_of(data, 32 * KIB, levels[i],
			    (flags & 1) != 0, (flags & 2) != 0);
			snprintf(name, sizeof(name), "level-%u-flags-%u.seed", levels[i], flags);
			write_seed_buffer(directory, name, &frame);
			free(frame.bytes);
		}
	}
	first = zstd_of(data, 16 * KIB, 1, true, true);
	second = zstd_of(data + 16 * KIB, 16 * KIB, 3, true, false);
	append(&joined, first.bytes, first.length);
	append(&joined, second.bytes, second.length);
	write_seed_buffer(directory, "concatenated.seed", &joined);
	free(joined.bytes);
	joined = (buffer_t){ 0 };
	zstd_skip(&joined);
	append(&joined, first.bytes, first.length);
	zstd_skip(&joined);
	append(&joined, second.bytes, second.length);
	write_seed_buffer(directory, "skippable.seed", &joined);
	free(joined.bytes);
	frame = zstd_raw_frame();
	joined = (buffer_t){ 0 };
	append(&joined, frame.bytes, frame.length);
	/* v0.7: single segment, three-byte raw block, then an end block. */
	append(&joined, "\x27\xb5\x2f\xfd\x20\x03\x40\0\x03xyz\xc0\0\0", 15);
	write_seed_buffer(directory, "current-then-legacy-v07.seed", &joined);
	free(joined.bytes);
	for (unsigned size = 2; size <= 4; size++) {
		frame.bytes[5] = (unsigned char)size;
		snprintf(name, sizeof(name), "raw-size-%u.seed", size);
		write_seed_buffer(directory, name, &frame);
	}
	frame.bytes[5] = 3;
	for (size_t keep = 1; keep <= 9; keep++) {
		frame.length = keep;
		snprintf(name, sizeof(name), "truncated-%zu.seed", keep);
		write_seed_buffer(directory, name, &frame);
	}
	frame.length = 12;
	frame.bytes[4] |= 0x08;
	write_seed_buffer(directory, "reserved-header-bit.seed", &frame);
	frame.bytes[4] = 0; /* Non-single-segment: byte 5 is a window descriptor. */
	frame.bytes[5] = 0x80;
	write_seed_buffer(directory, "window-64m.seed", &frame);
	frame.bytes[5] = 0x88;
	write_seed_buffer(directory, "window-128m.seed", &frame);
	frame.bytes[5] = 0x89;
	write_seed_buffer(directory, "window-144m.seed", &frame);
	frame.bytes[5] = 0x90;
	write_seed_buffer(directory, "window-256m.seed", &frame);
	frame.bytes[5] = 0xb8;
	write_seed_buffer(directory, "window-log-33.seed", &frame);
	free(frame.bytes);
	frame = zstd_of(second.bytes, second.length, 3, true, true);
	write_seed_buffer(directory, "nested.seed", &frame);
	free(frame.bytes);
	first.bytes[first.length - 1] ^= 1;
	write_seed_buffer(directory, "bad-checksum.seed", &first);
	first.bytes[first.length - 1] ^= 1;
	append(&first, "garbage", 7);
	write_seed_buffer(directory, "trailing-garbage.seed", &first);
	free(first.bytes);
	free(second.bytes);
	free(data);
}
#endif

/* Corpus destinations are existing directories; never replace a seed. */
static void
write_seed_buffer(const char *directory, const char *name, const buffer_t *buffer)
{
	int directory_fd, fd;
	FILE *out;

	directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(directory_fd != -1);
	fd = openat(directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	out = fdopen(fd, "wb");
	CHECK(out != NULL);
	CHECK(fwrite(buffer->bytes, 1, buffer->length, out) == buffer->length);
	CHECK(fclose(out) == 0);
	CHECK(close(directory_fd) == 0);
}

int
main(int argc, char **argv)
{
	unsigned char *data;
	member_t members[2];
	buffer_t gzip = { 0 };

	/* A small two-member file seeds the gzip fuzzer's corpus. */
	if (argc == 3 && strcmp(argv[1], "--seed-gzip") == 0) {
		data = pattern(16 * KIB, 10);
		members[0] = (member_t){ .data = data, .length = 16 * KIB,
		    .flags = FLAG_FEXTRA | FLAG_FNAME | FLAG_FCOMMENT | FLAG_FHCRC,
		    .extra_length = 6, .name_length = 8, .comment_length = 4 };
		members[1] = (member_t){ .data = data, .length = 4 * KIB };
		member_append(&gzip, &members[0]);
		member_append(&gzip, &members[1]);
		write_seed_buffer(argv[2], "members.seed", &gzip);
		free(gzip.bytes);
		free(data);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-xz") == 0) {
#ifdef FSIMG_XZ
		seed_xz(argv[2]);
		return (0);
#else
		printf("SKIP: xz seeds require liblzma (FSIMG_XZ)\n");
		return (77);
#endif
	}
	if (argc == 3 && strcmp(argv[1], "--seed-zstd") == 0) {
#ifdef FSIMG_ZSTD
		seed_zstd(argv[2]);
		return (0);
#else
		printf("SKIP: zstd seeds require libzstd (FSIMG_ZSTD)\n");
		return (77);
#endif
	}
	CHECK(argc == 1);
	CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
	test_zstd_registry_reports_build_support();
#ifdef FSIMG_ZSTD
	test_zstd_levels_roundtrip_with_checksum_and_content_size();
	test_zstd_concatenation_and_skippable_frames();
	test_zstd_checksum_and_content_size_are_optional();
	test_zstd_raw_frames_validate_sizes_headers_and_termination();
	test_zstd_window_limits_precede_spooling();
	test_zstd_header_refusals_at_each_frame_start();
	test_zstd_legacy_frames_are_never_decoded();
	test_zstd_bomb_stops_before_crossing_spool_limit();
	test_zstd_detection_forces_signature_and_peels_once();
	test_zstd_allocation_failures_release_codec_state();
	test_zstd_decode_charges_compressed_and_decoded_bytes();
	test_zstd_decode_stops_at_work_limit();
	test_zstd_encoder_unknown_length_dmg_refuses_large_workspace_before_output();
	test_zstd_encoder_known_size_fits_16_mib();
	test_zstd_short_transcode_fails_at_encoder_end();
	test_zstd_exact_transcode_finishes();
	test_zstd_level_defaults_validation_and_reproducibility();
	test_zstd_bound_covers_library_and_incompressible_output();
	test_zstd_plan_names_compression_and_container_libraries();
#else
	printf("SKIP: zstd profile tests require libzstd (FSIMG_ZSTD)\n");
#endif
	test_xz_registry_reports_build_support();
#ifdef FSIMG_XZ
	test_xz_presets_and_checks_roundtrip_and_reject_damage();
	test_xz_joined_streams_require_four_byte_padding();
	test_xz_unchecked_blocks_report_incomplete_verification();
	test_xz_headers_indexes_and_checks_reject_damage();
	test_xz_detection_excludes_alone_and_peels_once();
	test_xz_bomb_stops_before_crossing_spool_limit();
	test_xz_large_dictionary_fails_before_spooling();
	test_xz_allocation_failures_release_codec_state();
	test_xz_decode_charges_compressed_and_decoded_bytes();
	test_xz_preset_nine_exceeds_default_memory();
	test_xz_plan_rejects_invalid_level_and_check();
	test_xz_bound_covers_liblzma_bound_and_rejects_overflow();
	test_xz_bound_covers_incompressible_and_barely_compressible_data();
	test_xz_reproducible_bytes_ignore_memory_budget();
	test_xz_plan_names_compression_and_container_libraries();
#else
	printf("SKIP: xz profile tests require liblzma (FSIMG_XZ)\n");
#endif

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
