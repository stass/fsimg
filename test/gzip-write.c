/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "test.h"

/*
 * The gzip write side (specs/design-layers.md#gzip): the member's fixed
 * header, the level and time settings, gzip around every stack and every
 * image operation, the output limit, reproducibility under one zlib, and
 * faults injected at the sink, the flush, cancellation and allocation.
 * Members are decoded here with zlib's own gzip wrapper, which checks the
 * header, CRC32 and length independently of fsimg's encoder; gzip(1) and
 * zcat check them again in test/compression-integration.sh.
 */
#define MIB		(UINT64_C(1024) * 1024)
#define EPOCH		INT64_C(1700000000)

/* A growable object in memory, as the library's sink or input. Writes
 * fail with EIO once fail_at bytes have been written, after a short
 * transfer up to it; flush fails when fail_flush is set.
 */
typedef struct memory {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 written;
	uint64_t	 fail_at;	/* Zero never */
	bool		 fail_flush;
} memory_t;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	uint64_t	 baseline_memory;
	char		 directory[1024];
	int		 directory_fd;
	unsigned	 polls;
	unsigned	 cancel_after;	/* Cancel at this poll; zero never */
} fixture_t;

static fsimg_status_t memory_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t memory_flush(void *, fsimg_error_t *);
static fsimg_io_t *memory_io(fixture_t *, memory_t *);
static bool cancel_at(void *);
static void fixture_init(fixture_t *, uint64_t);
static void fixture_free(fixture_t *);
static int raw_file(fixture_t *);
static void options_for(fsimg_write_options_t *, fsimg_layer_options_t *,
    const char *, unsigned, int64_t, uint32_t);
static fsimg_status_t write_image(fixture_t *, const char *,
    const fsimg_write_options_t *, memory_t *, fsimg_error_t *);
static fsimg_status_t wrap(fixture_t *, memory_t *, const char *,
    const fsimg_write_options_t *, memory_t *, fsimg_error_t *);
static void gunzip(const memory_t *, memory_t *);
static bool same(const memory_t *, const memory_t *);
static uint32_t mtime_of(const memory_t *);
static void test_header_and_levels(void);
static void test_mtime(void);
static void test_stacks_and_operations(void);
static void test_incompressible_and_containers(void);
static void test_output_limit(void);
static void test_faults(void);
static void test_compression_names(void);

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	bcopy(memory->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	memory_t *memory = argument;
	size_t allowed = length;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	if (memory->fail_at != 0 && memory->written + length > memory->fail_at)
		allowed = (size_t)(memory->fail_at - memory->written);
	bcopy(buffer, memory->bytes + offset, allowed);
	memory->written += allowed;
	*transferred = allowed;
	if (allowed < length)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IO,
		    "injected short write"));
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((memory_t *)argument)->length;
	return (FSIMG_OK);
}

/* Growth reads as 0xa5, so only bytes the library writes read as zero. */
static fsimg_status_t
memory_resize(void *argument, uint64_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;
	unsigned char *bytes;

	bytes = realloc(memory->bytes, length == 0 ? 1 : (size_t)length);
	CHECK(bytes != NULL);
	if (length > memory->length)
		memset(bytes + memory->length, 0xa5, (size_t)(length - memory->length));
	memory->bytes = bytes;
	memory->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error)
{
	if (((memory_t *)argument)->fail_flush)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IO,
		    "injected flush failure"));
	return (FSIMG_OK);
}

static fsimg_io_t *
memory_io(fixture_t *f, memory_t *memory)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.flags = FSIMG_IO_DURABLE;
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	ops.flush = memory_flush;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, memory, &io, NULL) == FSIMG_OK);
	return (io);
}

static bool
cancel_at(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	return (f->cancel_after != 0 && f->polls >= f->cancel_after);
}

/* A context over a private directory, with an output limit (zero none). */
static void
fixture_init(fixture_t *f, uint64_t max_output)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-gzip-write.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	options.scratch_directory_fd = f->directory_fd;
	options.max_output_bytes = max_output;
	options.cancelled = cancel_at;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Every charge is returned and nothing is left in the directory: no
 * staging, raw intermediate or published image.
 */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;
	struct dirent *entry;
	DIR *directory;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline_memory);
	fsimg_ctx_free(f->ctx);
	directory = fdopendir(f->directory_fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL)
		CHECK(strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0);
	CHECK(closedir(directory) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* A raw intermediate: created exclusively and unlinked at once. */
static int
raw_file(fixture_t *f)
{
	int fd;

	fd = openat(f->directory_fd, "raw", O_RDWR | O_CREAT | O_EXCL |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(unlinkat(f->directory_fd, "raw", 0) == 0);
	return (fd);
}

static void
options_for(fsimg_write_options_t *options, fsimg_layer_options_t *layers,
    const char *compression, unsigned flags, int64_t epoch, uint32_t level)
{
	fsimg_layer_options_init(layers);
	layers->gzip_level = level;
	fsimg_write_options_init(options);
	snprintf(options->compression, sizeof(options->compression), "%s",
	    compression);
	options->flags = flags;
	options->epoch = epoch;
	options->layers = layers;
}

/* A writer's 1 MiB volume with no entries, through an IO sink, and a raw
 * intermediate when the output is compressed or a DMG.
 */
static fsimg_status_t
write_image(fixture_t *f, const char *stack,
    const fsimg_write_options_t *options, memory_t *out, fsimg_error_t *error)
{
	fsimg_write_options_t sized = *options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	sized.volume_bytes = MIB;
	status = fsimg_writer_new(f->ctx, stack, &sized, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = memory_io(f, out);
	if (options->compression[0] != '\0' ||
	    strstr(stack, "dmg") != NULL)
		sink.raw_fd = raw_file(f);
	status = fsimg_writer_output(writer, &sink, error);
	fsimg_io_free(sink.io);
	fsimg_writer_free(writer);
	return (status);
}

/*
 * Fill the stack below the caller with a nonzero pattern, so a callee that
 * reads an uninitialized local sees garbage rather than a lucky zero.
 */
static void
dirty_stack(void)
{
	volatile unsigned char bytes[64 * 1024];

	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = 0x41;
}

/*
 * The compression is a registry name: one the registry lacks as a
 * compression is FSIMG_EINVAL, one it cannot write FSIMG_EUNSUPPORTED, and
 * an unterminated one FSIMG_EINVAL. The writer and wrap refuse each at
 * construction, before anything is planned or written.
 */
static void
test_compression_names(void)
{
	static const struct {
		const char	*name;		/* NULL: 16 bytes, no NUL */
		fsimg_status_t	 status;
	} cases[] = {
		{ "nosuch", FSIMG_EINVAL },
		{ "dmg", FSIMG_EINVAL },
		{ "zstd", FSIMG_EUNSUPPORTED },
		{ NULL, FSIMG_EINVAL },
	};
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	memory_t volume, image;
	fixture_t f;

	fixture_init(&f, 0);
	options_for(&options, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	CHECK(write_image(&f, "fat12", &options, &volume, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		options_for(&options, &layers, cases[i].name == NULL ? "" :
		    cases[i].name, FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
		if (cases[i].name == NULL)
			memset(options.compression, 'x', sizeof(options.compression));
		writer = NULL;
		dirty_stack();
		CHECK(fsimg_writer_new(f.ctx, "fat12/gpt", &options, &writer,
		    &error) == cases[i].status && writer == NULL);
		if (cases[i].name != NULL)
			CHECK(strcmp(error.path, cases[i].name) == 0);
		bzero(&image, sizeof(image));
		dirty_stack();
		CHECK(wrap(&f, &volume, "fat12/gpt", &options, &image, NULL) ==
		    cases[i].status);
		free(image.bytes);
	}
	free(volume.bytes);
	fixture_free(&f);
}

static fsimg_status_t
wrap(fixture_t *f, memory_t *volume, const char *stack,
    const fsimg_write_options_t *options, memory_t *out, fsimg_error_t *error)
{
	fsimg_sink_t sink;
	fsimg_io_t *input;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	input = memory_io(f, volume);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = memory_io(f, out);
	status = fsimg_image_wrap(f->ctx, input, stack, options, &sink, error);
	fsimg_io_free(sink.io);
	fsimg_io_free(input);
	return (status);
}

/* zlib's gzip wrapper checks the header, the CRC32 and the length; the
 * member must end exactly at the end of the output.
 */
static void
gunzip(const memory_t *member, memory_t *out)
{
	unsigned char buffer[65536];
	z_stream stream;
	int code;

	bzero(out, sizeof(*out));
	bzero(&stream, sizeof(stream));
	CHECK(inflateInit2(&stream, 16 + MAX_WBITS) == Z_OK);
	stream.next_in = member->bytes;
	stream.avail_in = (uInt)member->length;
	do {
		stream.next_out = buffer;
		stream.avail_out = sizeof(buffer);
		code = inflate(&stream, Z_NO_FLUSH);
		CHECK(code == Z_OK || code == Z_STREAM_END);
		out->bytes = realloc(out->bytes, out->length + sizeof(buffer));
		CHECK(out->bytes != NULL);
		bcopy(buffer, out->bytes + out->length,
		    sizeof(buffer) - stream.avail_out);
		out->length += sizeof(buffer) - stream.avail_out;
	} while (code != Z_STREAM_END);
	CHECK(stream.avail_in == 0);
	inflateEnd(&stream);
}

static bool
same(const memory_t *a, const memory_t *b)
{
	return (a->length == b->length && bcmp(a->bytes, b->bytes,
	    (size_t)a->length) == 0);
}

static uint32_t
mtime_of(const memory_t *member)
{
	const unsigned char *p = member->bytes + 4;

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

/*
 * The header bytes RFC 1952 and the gzip profile fix: identification,
 * method 8, flags 0, the epoch as the time, extra flags 4 at level 1, 0 at
 * the default 6 and 2 at level 9, OS 3; the member decodes to the raw
 * image, and the plan names gzip, its bound and zlib. Levels outside 1 to
 * 9, and a level without gzip, are refused naming gzip:level.
 */
static void
test_header_and_levels(void)
{
	static const struct {
		uint32_t level;
		unsigned char xfl;
	} levels[] = { { 1, 4 }, { 0, 0 }, { 9, 2 } };
	static const uint32_t refused[] = { 10, UINT32_MAX };
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	memory_t raw, image, decoded;
	fsimg_io_t *io;
	unsigned char header[10] = { 0x1f, 0x8b, 8, 0, 0x00, 0xf1, 0x53, 0x65,
	    0, 3 };
	fixture_t f;

	fixture_init(&f, 0);
	options_for(&options, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	options.volume_bytes = MIB;
	bzero(&raw, sizeof(raw));
	CHECK(fsimg_writer_new(f.ctx, "fat12", &options, &writer, NULL) == FSIMG_OK);
	io = memory_io(&f, &raw);
	CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
	fsimg_io_free(io);
	fsimg_writer_free(writer);
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		options_for(&options, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE,
		    EPOCH, levels[i].level);
		options.volume_bytes = MIB;
		CHECK(write_image(&f, "fat12", &options, &image, NULL) == FSIMG_OK);
		header[8] = levels[i].xfl;
		CHECK(image.length > sizeof(header) &&
		    bcmp(image.bytes, header, sizeof(header)) == 0);
		gunzip(&image, &decoded);
		CHECK(same(&decoded, &raw));
		CHECK(fsimg_writer_new(f.ctx, "fat12", &options, &writer,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		CHECK(strcmp(info.image.compression, "gzip") == 0);
		CHECK(info.image.file_bytes == MIB && info.image.disk_bytes == MIB);
		CHECK(info.output_bytes >= image.length && info.output_bytes > MIB);
		CHECK(strncmp(info.codec_version, "zlib ", 5) == 0);
		fsimg_writer_free(writer);
		free(image.bytes);
		free(decoded.bytes);
	}
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		options_for(&options, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE,
		    EPOCH, refused[i]);
		CHECK(fsimg_writer_new(f.ctx, "fat12", &options, &writer,
		    &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "gzip:level") == 0);
	}
	options_for(&options, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 9);
	CHECK(fsimg_writer_new(f.ctx, "fat12", &options, &writer, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gzip:level") == 0);
	free(raw.bytes);
	fixture_free(&f);
}

/*
 * The time field is the epoch under reproducible mode, zero for an epoch
 * of zero or one it cannot hold (below one or above 2^32 - 1), and the
 * host clock otherwise; it is the only byte an epoch changes.
 */
static void
test_mtime(void)
{
	static const struct {
		int64_t epoch;
		uint32_t mtime;
	} cases[] = {
		{ -1, 0 }, { 0, 0 }, { 1, 1 },
		{ INT64_C(4294967295), UINT32_MAX }, { INT64_C(4294967296), 0 }
	};
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	memory_t volume, image, first;
	time_t before, after;
	fixture_t f;

	fixture_init(&f, 0);
	options_for(&options, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	CHECK(write_image(&f, "fat12", &options, &volume, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		options_for(&options, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE,
		    cases[i].epoch, 0);
		CHECK(wrap(&f, &volume, "fat12", &options, &image, NULL) ==
		    FSIMG_OK);
		CHECK(mtime_of(&image) == cases[i].mtime);
		/* Epochs change the time field and no other byte. */
		if (i == 0)
			first = image;
		else {
			CHECK(image.length == first.length &&
			    bcmp(image.bytes, first.bytes, 4) == 0 &&
			    bcmp(image.bytes + 8, first.bytes + 8,
			    (size_t)image.length - 8) == 0);
			free(image.bytes);
		}
	}
	free(first.bytes);
	options_for(&options, &layers, "gzip", 0, EPOCH, 0);
	before = time(NULL);
	CHECK(wrap(&f, &volume, "fat12", &options, &image, NULL) == FSIMG_OK);
	after = time(NULL);
	CHECK(mtime_of(&image) >= (uint32_t)before &&
	    mtime_of(&image) <= (uint32_t)after);
	free(image.bytes);
	free(volume.bytes);
	fixture_free(&f);
}

/*
 * gzip wraps whatever the stack writes: a DMG inside a GPT disk decodes to
 * the DMG written without gzip, for the writer and for wrap; unwrap and
 * compose compress their output too. Every output is byte-identical when
 * repeated, without hole discovery, and on a stream.
 */
static void
test_stacks_and_operations(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t plain, gzip, sized;
	fsimg_image_options_t open_options;
	fsimg_image_t *image;
	fsimg_part_t part;
	fsimg_sink_t sink;
	fsimg_writer_t *writer;
	fsimg_io_t *io, *output;
	memory_t volume, dmg, packed, again, decoded, disk, streamed;
	struct stat status;
	fixture_t f;
	int stream;

	fixture_init(&f, 0);
	options_for(&plain, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	options_for(&gzip, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	CHECK(write_image(&f, "fat12/gpt/dmg", &plain, &dmg, NULL) == FSIMG_OK);
	CHECK(write_image(&f, "fat12/gpt/dmg", &gzip, &packed, NULL) == FSIMG_OK);
	gunzip(&packed, &decoded);
	CHECK(same(&decoded, &dmg));
	free(decoded.bytes);
	fsimg_test_hole_discovery(f.ctx, false);
	CHECK(write_image(&f, "fat12/gpt/dmg", &gzip, &again, NULL) == FSIMG_OK);
	fsimg_test_hole_discovery(f.ctx, true);
	CHECK(same(&again, &packed));
	free(again.bytes);

	/* The same image on a stream. */
	sized = gzip;
	sized.volume_bytes = MIB;
	CHECK(fsimg_writer_new(f.ctx, "fat12/gpt/dmg", &sized, &writer, NULL) ==
	    FSIMG_OK);
	stream = openat(f.directory_fd, "stream", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(stream != -1);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = stream;
	sink.raw_fd = raw_file(&f);
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fstat(stream, &status) == 0 && (uint64_t)status.st_size == packed.length);
	streamed.length = packed.length;
	streamed.bytes = malloc((size_t)packed.length);
	CHECK(streamed.bytes != NULL && pread(stream, streamed.bytes,
	    (size_t)packed.length, 0) == (ssize_t)packed.length);
	CHECK(same(&streamed, &packed));
	CHECK(close(stream) == 0 && unlinkat(f.directory_fd, "stream", 0) == 0);
	free(streamed.bytes);
	free(packed.bytes);
	free(dmg.bytes);

	/* Wrap, then unwrap and compose, each with gzip. */
	CHECK(write_image(&f, "fat12", &plain, &volume, NULL) == FSIMG_OK);
	CHECK(wrap(&f, &volume, "fat12/gpt/dmg", &plain, &dmg, NULL) == FSIMG_OK);
	CHECK(wrap(&f, &volume, "fat12/gpt/dmg", &gzip, &packed, NULL) ==
	    FSIMG_OK);
	gunzip(&packed, &decoded);
	CHECK(same(&decoded, &dmg));
	free(decoded.bytes);
	free(packed.bytes);
	free(dmg.bytes);
	CHECK(wrap(&f, &volume, "fat12/gpt", &plain, &disk, NULL) == FSIMG_OK);
	io = memory_io(&f, &disk);
	fsimg_image_options_init(&open_options);
	CHECK(fsimg_image_open(f.ctx, io, &open_options, &image, NULL) ==
	    FSIMG_OK);
	bzero(&packed, sizeof(packed));
	output = memory_io(&f, &packed);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	CHECK(fsimg_image_unwrap(image, &(fsimg_selector_t){ .kind =
	    FSIMG_SELECT_AUTOMATIC }, &gzip, &sink, NULL) == FSIMG_OK);
	fsimg_io_free(output);
	gunzip(&packed, &decoded);
	CHECK(same(&decoded, &volume));
	free(decoded.bytes);
	free(packed.bytes);
	fsimg_image_free(image);
	fsimg_io_free(io);

	free(disk.bytes);
	part = (fsimg_part_t){ .volume = memory_io(&f, &volume) };
	bzero(&disk, sizeof(disk));
	sink.io = memory_io(&f, &disk);
	CHECK(fsimg_image_compose(f.ctx, "gpt", &part, 1, &plain, &sink, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(sink.io);
	bzero(&packed, sizeof(packed));
	sink.io = memory_io(&f, &packed);
	CHECK(fsimg_image_compose(f.ctx, "gpt", &part, 1, &gzip, &sink, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(sink.io);
	fsimg_io_free(part.volume);
	gunzip(&packed, &decoded);
	CHECK(same(&decoded, &disk));
	free(decoded.bytes);
	free(packed.bytes);
	free(disk.bytes);
	free(volume.bytes);
	fixture_free(&f);
}

/*
 * Incompressible bytes at the fastest and slowest levels stay within the
 * planned bound and decode exactly. gzip wraps simg's two passes and VHD's
 * zero runs as it wraps a DMG. A compressed output without a raw
 * intermediate is refused before anything is written.
 */
static void
test_incompressible_and_containers(void)
{
	static const char *const stacks[] = { "fat12/simg", "fat12/vhd" };
	static const uint32_t levels[] = { 1, 9 };
	fsimg_layer_options_t layers;
	fsimg_write_options_t plain, gzip;
	fsimg_writer_t *writer;
	fsimg_part_t part;
	fsimg_sink_t sink;
	memory_t noise, volume, expected, packed, decoded;
	fixture_t f;

	fixture_init(&f, 0);
	noise.length = 4 * MIB;
	noise.bytes = malloc((size_t)noise.length);
	CHECK(noise.bytes != NULL);
	srandom(1);
	for (uint64_t i = 0; i < noise.length; i++)
		noise.bytes[i] = (unsigned char)random();
	options_for(&plain, &layers, "", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	part = (fsimg_part_t){ .volume = memory_io(&f, &noise), .type = "basic" };
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	bzero(&expected, sizeof(expected));
	sink.io = memory_io(&f, &expected);
	CHECK(fsimg_image_compose(f.ctx, "gpt", &part, 1, &plain, &sink, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(sink.io);
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		options_for(&gzip, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE,
		    EPOCH, levels[i]);
		bzero(&packed, sizeof(packed));
		sink.io = memory_io(&f, &packed);
		CHECK(fsimg_image_compose(f.ctx, "gpt", &part, 1, &gzip, &sink,
		    NULL) == FSIMG_OK);
		fsimg_io_free(sink.io);
		CHECK(packed.length > noise.length);
		gunzip(&packed, &decoded);
		CHECK(same(&decoded, &expected));
		free(decoded.bytes);
		free(packed.bytes);
	}
	fsimg_io_free(part.volume);
	free(expected.bytes);
	free(noise.bytes);

	options_for(&gzip, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE, EPOCH, 0);
	CHECK(write_image(&f, "fat12", &plain, &volume, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++) {
		CHECK(wrap(&f, &volume, stacks[i], &plain, &expected, NULL) ==
		    FSIMG_OK);
		CHECK(wrap(&f, &volume, stacks[i], &gzip, &packed, NULL) ==
		    FSIMG_OK);
		gunzip(&packed, &decoded);
		CHECK(same(&decoded, &expected));
		free(decoded.bytes);
		free(packed.bytes);
		free(expected.bytes);
	}
	free(volume.bytes);

	CHECK(fsimg_writer_new(f.ctx, "fat12", &gzip, &writer, NULL) == FSIMG_OK);
	bzero(&packed, sizeof(packed));
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = memory_io(&f, &packed);
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_EINVAL);
	CHECK(packed.length == 0);
	fsimg_io_free(sink.io);
	fsimg_writer_free(writer);
	fixture_free(&f);
}

/*
 * The output limit covers the raw intermediate and the compressed output
 * together: the disk plus gzip's bound of the container's bound. One byte
 * less fails before anything is written, naming the encoding.
 */
static void
test_output_limit(void)
{
	static const struct {
		const char *stack;
		const char *encoding;
	} cases[] = { { "fat12", "gzip encoding" },
	    { "fat12/dmg", "dmg/gzip encoding" } };
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	memory_t image;
	uint64_t needed;
	fixture_t f;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_init(&f, 0);
		options_for(&options, &layers, "gzip", FSIMG_WRITE_REPRODUCIBLE,
		    EPOCH, 0);
		options.volume_bytes = MIB;
		CHECK(fsimg_writer_new(f.ctx, cases[i].stack, &options, &writer,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		needed = info.image.disk_bytes + info.output_bytes;
		fsimg_writer_free(writer);
		fixture_free(&f);

		fixture_init(&f, needed - 1);
		CHECK(write_image(&f, cases[i].stack, &options, &image, &error) ==
		    FSIMG_ELIMIT);
		CHECK(error.resource == FSIMG_RESOURCE_OUTPUT &&
		    error.observed == needed);
		CHECK(strstr(error.message, cases[i].encoding) != NULL);
		CHECK(image.length == 0);
		free(image.bytes);
		fixture_free(&f);

		fixture_init(&f, needed);
		CHECK(write_image(&f, cases[i].stack, &options, &image, NULL) ==
		    FSIMG_OK);
		free(image.bytes);
		fixture_free(&f);
	}
}

/*
 * A short write at the header, inside the data and at the trailer, and a
 * failed durable flush, leave an IO sink incomplete. A FILE sink
 * cancelled at every poll, or failing at every allocation, publishes
 * nothing and leaves no staging.
 */
static void
test_faults(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_status_t status;
	memory_t image, failing;
	uint64_t cuts[3];
	unsigned outcomes;
	fixture_t f;

	fixture_init(&f, 0);
	options_for(&options, &layers, "gzip",
	    FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_DURABLE,
	    EPOCH, 0);
	options.volume_bytes = MIB;
	CHECK(write_image(&f, "fat12", &options, &image, NULL) == FSIMG_OK);
	cuts[0] = 1;
	cuts[1] = image.length / 2;
	cuts[2] = image.length - 1;
	for (size_t i = 0; i <= sizeof(cuts) / sizeof(cuts[0]); i++) {
		CHECK(fsimg_writer_new(f.ctx, "fat12", &options, &writer, NULL) ==
		    FSIMG_OK);
		bzero(&failing, sizeof(failing));
		if (i < sizeof(cuts) / sizeof(cuts[0]))
			failing.fail_at = cuts[i];
		else
			failing.fail_flush = true;
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = memory_io(&f, &failing);
		sink.raw_fd = raw_file(&f);
		bzero(&error, sizeof(error));
		CHECK(fsimg_writer_output(writer, &sink, &error) == FSIMG_EIO);
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		fsimg_io_free(sink.io);
		fsimg_writer_free(writer);
		free(failing.bytes);
	}
	free(image.bytes);

	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "image.gz";
	outcomes = 0;
	for (f.cancel_after = 1;; f.cancel_after++) {
		f.polls = 0;
		writer = NULL;
		sink.raw_fd = raw_file(&f);
		status = fsimg_writer_new(f.ctx, "fat12", &options, &writer,
		    &error);
		if (status == FSIMG_OK)
			status = fsimg_writer_output(writer, &sink, &error);
		else
			CHECK(close(sink.raw_fd) == 0);
		fsimg_writer_free(writer);
		if (status == FSIMG_OK)
			break;
		/* Cancelled once published, the image stays, durability unsure. */
		if (status == FSIMG_EDURABILITY) {
			CHECK(error.output_state == FSIMG_OUTPUT_PUBLISHED);
			CHECK(unlinkat(f.directory_fd, "image.gz", 0) == 0);
			continue;
		}
		CHECK(status == FSIMG_ECANCELLED);
		CHECK(faccessat(f.directory_fd, "image.gz", F_OK, 0) == -1);
		outcomes++;
	}
	CHECK(outcomes > 8);
	CHECK(unlinkat(f.directory_fd, "image.gz", 0) == 0);
	f.cancel_after = 0;

	outcomes = 0;
	for (uint64_t attempt = 1;; attempt++) {
		writer = NULL;
		sink.raw_fd = raw_file(&f);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_writer_new(f.ctx, "fat12", &options, &writer,
		    NULL);
		if (status == FSIMG_OK)
			status = fsimg_writer_output(writer, &sink, NULL);
		else
			CHECK(close(sink.raw_fd) == 0);
		fsimg_writer_free(writer);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(faccessat(f.directory_fd, "image.gz", F_OK, 0) == -1);
		outcomes++;
	}
	CHECK(outcomes > 4);
	CHECK(unlinkat(f.directory_fd, "image.gz", 0) == 0);
	fixture_free(&f);
}

int
main(void)
{
	test_header_and_levels();
	test_mtime();
	test_stacks_and_operations();
	test_incompressible_and_containers();
	test_output_limit();
	test_faults();
	test_compression_names();
	printf("gzip write tests passed\n");
	return (0);
}
