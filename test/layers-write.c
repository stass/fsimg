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
#include <unistd.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

/*
 * The GPT and MBR write side: the layer settings, the disk plan, the
 * tables the writer and the image operations emit around their volumes,
 * and the identifiers reproducible mode derives. Written disks are decoded
 * here independently of the library, with this file's own CRC-32, GUID
 * byte order and CHS arithmetic, and read back through the library only
 * as a second opinion. Every passing case has a seeded failure beside it.
 */

#define MIB		(UINT64_C(1024) * 1024)
#define EPOCH		INT64_C(1700000000)	/* 0x6553f100 */
#define OTHER_EPOCH	INT64_C(1800000000)
#define GPT_ENTRY_BYTES	128u
#define GPT_ENTRIES	128u

#define BASIC_GUID	"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define ESP_GUID	"C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define LINUX_GUID	"0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define QNX6_GUID	"CEF5A9AD-73BC-4601-89F3-CDEEEEE321A1"
#define DISK_GUID	"11111111-2222-4333-8444-555555555555"
#define PART_GUID	"66666666-7777-4888-8999-aaaaaaaaaaaa"

/* A growable object in memory, as the library's sink or input. reads
 * counts the bytes read through the callbacks, so a test can tell how
 * often an input was read. A nonzero flip_offset makes the input change
 * under its reader: the byte there reads as stored the first time a read
 * covers it and as 0x5a every later time.
 */
typedef struct memory {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 reads;
	uint64_t	 flip_offset;
	unsigned	 flip_reads;
} memory_t;

/* One context with a private scratch and output directory. */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	uint64_t	 baseline_memory;
	char		 directory[1024];
	int		 directory_fd;
	unsigned	 polls;		/* Cancellation polls observed */
	unsigned	 cancel_after;	/* Cancel at this poll; zero never */
} fixture_t;

static uint32_t crc32_oracle(const unsigned char *, size_t);
static uint32_t get32(const unsigned char *);
static uint64_t get64(const unsigned char *);
static void guid_text(const char *, unsigned char *);
static void guid_mixed(const char *, unsigned char *);
static void chs_oracle(uint64_t, unsigned char *);
static bool all_zero(const unsigned char *, uint64_t);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_io_t *memory_io(fixture_t *, memory_t *);
static void memory_free(memory_t *);
static bool cancel_at(void *);
static void fixture_init(fixture_t *);
static void fixture_free(fixture_t *);
static void test_stream_budget(void);
static void test_simg_changed_input(void);
static void write_options(fsimg_write_options_t *, unsigned, int64_t,
    const fsimg_layer_options_t *);
static fsimg_status_t create(fixture_t *, const char *,
    const fsimg_write_options_t *, memory_t *, fsimg_error_t *);
static fsimg_status_t plan_only(fixture_t *, const char *,
    const fsimg_write_options_t *, fsimg_plan_info_t *, fsimg_error_t *);
static void make_volume(fixture_t *, const char *, uint64_t, memory_t *);
static void expect_refused(fixture_t *, const char *, uint64_t,
    const fsimg_layer_options_t *, unsigned, fsimg_status_t, const char *);
static const char *gpt_check(const memory_t *, uint32_t);
static const unsigned char *gpt_entry(const memory_t *, uint32_t, unsigned);
static bool name_is(const unsigned char *, const char *);
static fsimg_status_t open_disk(fixture_t *, memory_t *, uint32_t,
    fsimg_io_t **, fsimg_image_t **, fsimg_error_t *);
static unsigned leftover_staging(const fixture_t *);
static void test_settings(void);
static void test_derived_defaults(void);
static void test_gpt_layout(void);
static void test_gpt_4kn(void);
static void test_mbr_layout(void);
static void test_mbr_chs_and_limits(void);
static void test_identifiers(void);
static void test_epochs_change_only_identifiers(void);
static void test_wrap_unwrap(void);
static void test_sinks(void);
static void test_compose(void);
static void test_compose_refusals(void);
static void test_aliases(void);
static void test_fixed_disk_stays_sparse(void);
static void expect_cancelled(fixture_t *, fsimg_status_t,
    const fsimg_error_t *, const char *);
static void test_cancellation(void);
static void test_allocation_failures(void);

static uint32_t
crc32_oracle(const unsigned char *bytes, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return (crc ^ 0xffffffffu);
}

static uint32_t
get32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static uint64_t
get64(const unsigned char *p)
{
	return ((uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32);
}

/* A GUID's sixteen bytes in text order. */
static void
guid_text(const char *text, unsigned char *bytes)
{
	unsigned value;

	for (unsigned i = 0, position = 0; i < 16; i++) {
		if (text[position] == '-')
			position++;
		CHECK(sscanf(text + position, "%2x", &value) == 1);
		bytes[i] = (unsigned char)value;
		position += 2;
	}
}

/* GPT's mixed byte order: the 32-, 16- and 16-bit leading groups
 * little-endian, the last eight bytes as written.
 */
static void
guid_mixed(const char *text, unsigned char *raw)
{
	unsigned char bytes[16];

	guid_text(text, bytes);
	for (unsigned i = 0; i < 4; i++)
		raw[i] = bytes[3 - i];
	raw[4] = bytes[5];
	raw[5] = bytes[4];
	raw[6] = bytes[7];
	raw[7] = bytes[6];
	bcopy(bytes + 8, raw + 8, 8);
}

/* CHS under 255 heads and 63 sectors, saturated at 1023/254/63: head,
 * sector with the cylinder's high bits, cylinder's low byte.
 */
static void
chs_oracle(uint64_t lba, unsigned char *raw)
{
	unsigned cylinder, head, sector;

	if (lba >= UINT64_C(1024) * 255 * 63) {
		cylinder = 1023;
		head = 254;
		sector = 63;
	} else {
		cylinder = (unsigned)(lba / (255 * 63));
		head = (unsigned)(lba / 63 % 255);
		sector = (unsigned)(lba % 63 + 1);
	}
	raw[0] = (unsigned char)head;
	raw[1] = (unsigned char)(sector | (cylinder >> 8) << 6);
	raw[2] = (unsigned char)cylinder;
}

static bool
all_zero(const unsigned char *bytes, uint64_t length)
{
	for (uint64_t i = 0; i < length; i++) {
		if (bytes[i] != 0)
			return (false);
	}
	return (true);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	bcopy(memory->bytes + offset, buffer, length);
	memory->reads += length;
	if (memory->flip_offset != 0 && offset <= memory->flip_offset &&
	    memory->flip_offset - offset < length &&
	    memory->flip_reads++ != 0)
		((unsigned char *)buffer)[memory->flip_offset - offset] = 0x5a;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	bcopy(buffer, memory->bytes + offset, length);
	*transferred = length;
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

static fsimg_io_t *
memory_io(fixture_t *f, memory_t *memory)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, memory, &io, NULL) == FSIMG_OK);
	return (io);
}

static void
memory_free(memory_t *memory)
{
	free(memory->bytes);
	bzero(memory, sizeof(*memory));
}

static bool
cancel_at(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	return (f->cancel_after != 0 && f->polls >= f->cancel_after);
}

static void
fixture_init(fixture_t *f)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-layers-write.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	options.scratch_directory_fd = f->directory_fd;
	options.cancelled = cancel_at;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Every charge is returned, and the directory is empty once the test has
 * removed what it published: no staging or scratch is left behind.
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
	rewinddir(directory);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			fprintf(stderr, "left behind: %s\n", entry->d_name);
	}
	CHECK(closedir(directory) == 0);
	CHECK(rmdir(f->directory) == 0);
}

static void
write_options(fsimg_write_options_t *options, unsigned flags, int64_t epoch,
    const fsimg_layer_options_t *layers)
{
	fsimg_write_options_init(options);
	options->flags = flags;
	options->epoch = epoch;
	options->layers = layers;
}

/* The writer's whole output for a stack with no entries, in memory. */
static fsimg_status_t
create(fixture_t *f, const char *stack, const fsimg_write_options_t *options,
    memory_t *out, fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	fsimg_io_t *io;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	status = fsimg_writer_new(f->ctx, stack, options, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	io = memory_io(f, out);
	status = fsimg_writer_emit(writer, io, error);
	fsimg_io_free(io);
	fsimg_writer_free(writer);
	return (status);
}

static fsimg_status_t
plan_only(fixture_t *f, const char *stack, const fsimg_write_options_t *options,
    fsimg_plan_info_t *info, fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_status_t status;

	status = fsimg_writer_new(f->ctx, stack, options, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_writer_plan(writer, &plan, error);
	if (status == FSIMG_OK)
		status = fsimg_plan_get_info(plan, info, error);
	fsimg_writer_free(writer);
	return (status);
}

/* A bare reproducible volume of the given length. */
static void
make_volume(fixture_t *f, const char *format, uint64_t length, memory_t *out)
{
	fsimg_write_options_t options;

	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = length;
	CHECK(create(f, format, &options, out, NULL) == FSIMG_OK);
	CHECK(out->length == length);
}

/* The settings fail planning a 1 MiB volume (or the given length) with
 * the status and the key or partition named.
 */
static void
expect_refused(fixture_t *f, const char *stack, uint64_t volume_bytes,
    const fsimg_layer_options_t *layers, unsigned flags, fsimg_status_t status,
    const char *path)
{
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fsimg_error_t error;

	write_options(&options, flags, EPOCH, layers);
	options.volume_bytes = volume_bytes;
	bzero(&error, sizeof(error));
	CHECK(plan_only(f, stack, &options, &info, &error) == status);
	if (path != NULL && strcmp(error.path, path) != 0) {
		fprintf(stderr, "%s: path \"%s\", message \"%s\"\n", stack, error.path,
		    error.message);
		CHECK(!"the refusal names the setting");
	}
}

/*
 * The GPT at both ends of a disk, checked field by field against the UEFI
 * layout: the protective MBR, each header's fields and CRC, the entry
 * array's CRC, and the backup array equal to the primary. Returns the name
 * of the first failed check, or NULL.
 */
static const char *
gpt_check(const memory_t *disk, uint32_t sector)
{
	const unsigned char *mbr = disk->bytes, *primary, *backup;
	unsigned char header[92];
	uint64_t last, entry_sectors;

	if (disk->length % sector != 0 || disk->length / sector < 68)
		return ("disk geometry");
	last = disk->length / sector - 1;
	entry_sectors = GPT_ENTRIES * GPT_ENTRY_BYTES / sector;
	if (mbr[446] != 0 || mbr[447] != 0x00 || mbr[448] != 0x02 ||
	    mbr[449] != 0x00 || mbr[450] != 0xee || mbr[451] != 0xff ||
	    mbr[452] != 0xff || mbr[453] != 0xff || get32(mbr + 454) != 1 ||
	    get32(mbr + 458) != (last > UINT32_MAX ? UINT32_MAX : last) ||
	    !all_zero(mbr + 462, 48) || mbr[510] != 0x55 || mbr[511] != 0xaa ||
	    !all_zero(mbr, 446))
		return ("protective MBR");
	for (int copy = 0; copy < 2; copy++) {
		primary = disk->bytes + (copy == 0 ? sector : last * sector);
		bcopy(primary, header, sizeof(header));
		bzero(header + 16, 4);
		if (memcmp(primary, "EFI PART", 8) != 0 ||
		    get32(primary + 8) != 0x00010000 || get32(primary + 12) != 92 ||
		    get32(primary + 20) != 0)
			return (copy == 0 ? "primary signature" : "backup signature");
		if (get32(primary + 16) != crc32_oracle(header, sizeof(header)))
			return (copy == 0 ? "primary header CRC" : "backup header CRC");
		if (get64(primary + 24) != (copy == 0 ? 1 : last) ||
		    get64(primary + 32) != (copy == 0 ? last : 1) ||
		    get64(primary + 40) != 2 + entry_sectors ||
		    get64(primary + 48) != last - entry_sectors - 1 ||
		    get64(primary + 72) != (copy == 0 ? 2 : last - entry_sectors) ||
		    get32(primary + 80) != GPT_ENTRIES ||
		    get32(primary + 84) != GPT_ENTRY_BYTES ||
		    !all_zero(primary + 92, sector - 92))
			return (copy == 0 ? "primary header fields" : "backup header fields");
		if (get32(primary + 88) != crc32_oracle(disk->bytes +
		    get64(primary + 72) * sector, GPT_ENTRIES * GPT_ENTRY_BYTES))
			return (copy == 0 ? "primary entries CRC" : "backup entries CRC");
	}
	primary = disk->bytes + sector;
	backup = disk->bytes + last * sector;
	if (memcmp(primary + 56, backup + 56, 16) != 0)
		return ("disk GUIDs differ");
	if (memcmp(disk->bytes + 2 * sector, disk->bytes + (last -
	    entry_sectors) * sector, GPT_ENTRIES * GPT_ENTRY_BYTES) != 0)
		return ("backup entries differ");
	return (NULL);
}

static const unsigned char *
gpt_entry(const memory_t *disk, uint32_t sector, unsigned slot)
{
	return (disk->bytes + 2 * sector + (slot - 1) * GPT_ENTRY_BYTES);
}

/* An entry's 72-byte name holds ASCII text as UTF-16LE, zero-padded. */
static bool
name_is(const unsigned char *entry, const char *text)
{
	size_t length = strlen(text);

	for (size_t i = 0; i < 36; i++) {
		if (entry[56 + 2 * i] != (i < length ? (unsigned char)text[i] : 0) ||
		    entry[57 + 2 * i] != 0)
			return (false);
	}
	return (true);
}

/* The library's view of a written disk, forcing the GPT sector size. */
static fsimg_status_t
open_disk(fixture_t *f, memory_t *disk, uint32_t gpt_sector, fsimg_io_t **io,
    fsimg_image_t **image, fsimg_error_t *error)
{
	fsimg_image_options_t options;

	*io = memory_io(f, disk);
	fsimg_image_options_init(&options);
	if (gpt_sector != 0) {
		CHECK(fsimg_stack_parse("gpt", &options.stack, NULL) == FSIMG_OK);
		options.layers.gpt_sector_bytes = gpt_sector;
	}
	return (fsimg_image_open(f->ctx, *io, &options, image, error));
}

/* Private output staging directories left in the fixture directory. */
static unsigned
leftover_staging(const fixture_t *f)
{
	struct dirent *entry;
	unsigned count;
	DIR *directory;
	int fd;

	fd = dup(f->directory_fd);
	CHECK(fd != -1);
	directory = fdopendir(fd);
	CHECK(directory != NULL);
	/* The duplicate shares its offset with the fixture's descriptor. */
	rewinddir(directory);
	count = 0;
	while ((entry = readdir(directory)) != NULL)
		count += strncmp(entry->d_name, ".fsimg-output-", 14) == 0;
	CHECK(closedir(directory) == 0);
	return (count);
}

/*
 * Each layer key at its bounds, and each refusal naming the key: a key of
 * a layer the stack lacks, a type the scheme cannot store, the 36-unit
 * name limit, a disk too small or misaligned, an MBR disk past 2^32
 * sectors, an identifier unset outside reproducible mode, and a container.
 */
static void
test_settings(void)
{
	/* U+1F600 takes two UTF-16 units, so 18 of them fill a GPT name. */
	static const char face[] = "\xf0\x9f\x98\x80";
	static const char *const refused_mbr[] = { "0x05", "0x0f", "0x85", "0xee",
	    "0x00", "apfs", BASIC_GUID, "fat99" };
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	memory_t disk;
	fixture_t f;
	char name[128];

	fixture_init(&f);
	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_type, sizeof(layers.gpt_type), "esp");
	expect_refused(&f, "fat12", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	expect_refused(&f, "fat12/mbr", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	fsimg_layer_options_init(&layers);
	layers.mbr_active = true;
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "mbr:active");
	fsimg_layer_options_init(&layers);
	layers.reserved[3] = 1;
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, NULL);
	fsimg_layer_options_init(&layers);
	layers.mbr_id = 7;
	expect_refused(&f, "fat12/mbr", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "mbr:id");

	/* The name: 36 units in, 37 out, and invalid UTF-8 out. */
	fsimg_layer_options_init(&layers);
	for (int i = 0; i < 18; i++)
		bcopy(face, name + 4 * i, 4);
	name[72] = '\0';
	snprintf(layers.gpt_name, sizeof(layers.gpt_name), "%s", name);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.partition.name, name) == 0);
	layers.gpt_name[72] = 'x';
	layers.gpt_name[73] = '\0';
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:name");
	memset(layers.gpt_name, 'n', 36);
	layers.gpt_name[36] = '\0';
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	layers.gpt_name[36] = 'n';
	layers.gpt_name[37] = '\0';
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:name");
	snprintf(layers.gpt_name, sizeof(layers.gpt_name), "\xc3");
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:name");

	/* Types: a GPT type has a GUID, an MBR type a writable byte. */
	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_type, sizeof(layers.gpt_type), "0x0c");
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	snprintf(layers.gpt_type, sizeof(layers.gpt_type), "qnx4");
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	expect_refused(&f, "qnx4/gpt", MIB, NULL, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	/* The nil type marks an unused entry, so no partition carries it. */
	snprintf(layers.gpt_type, sizeof(layers.gpt_type),
	    "00000000-0000-0000-0000-000000000000");
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:type");
	snprintf(layers.gpt_type, sizeof(layers.gpt_type), "%s", LINUX_GUID);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "qnx4/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.partition.type, "linux") == 0);
	for (size_t i = 0; i < sizeof(refused_mbr) / sizeof(refused_mbr[0]); i++) {
		fsimg_layer_options_init(&layers);
		snprintf(layers.mbr_type, sizeof(layers.mbr_type), "%s", refused_mbr[i]);
		expect_refused(&f, "fat12/mbr", MIB, &layers,
		    FSIMG_WRITE_REPRODUCIBLE, FSIMG_EINVAL, "mbr:type");
	}
	fsimg_layer_options_init(&layers);
	snprintf(layers.mbr_type, sizeof(layers.mbr_type), "0x83");
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/mbr", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.partition.mbr_type == 0x83);

	/* The disk length: exact fit, one sector short, misaligned, past the
	 * MBR's 2^32 sectors, and a fixed disk fitting the volume.
	 */
	fsimg_layer_options_init(&layers);
	layers.gpt_disk_bytes = 2 * MIB + 33 * 512;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.image.disk_bytes == 2 * MIB + 33 * 512);
	layers.gpt_disk_bytes -= 512;
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:disk");
	layers.gpt_disk_bytes = 8 * MIB + 1;
	expect_refused(&f, "fat12/gpt", 0, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:disk");
	layers.gpt_disk_bytes = 8 * MIB + 512;
	layers.gpt_sector_bytes = 4096;
	expect_refused(&f, "fat12/gpt", 0, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:disk");
	layers.gpt_sector_bytes = 1024;
	layers.gpt_disk_bytes = 0;
	expect_refused(&f, "fat12/gpt", MIB, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:sector");
	fsimg_layer_options_init(&layers);
	layers.gpt_disk_bytes = MIB;
	expect_refused(&f, "fat12/gpt", 0, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "gpt:disk");
	layers.gpt_disk_bytes = 8 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.volume_bytes == 8 * MIB - MIB - 33 * 512);
	CHECK(info.image.disk_bytes == 8 * MIB && info.partition.offset == MIB);
	fsimg_layer_options_init(&layers);
	layers.mbr_disk_bytes = 16 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	CHECK(plan_only(&f, "fat16/mbr", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.volume_bytes == 15 * MIB && info.image.disk_bytes == 16 * MIB);
	/* The fit rounds to the sector the format resolves, here 4096 bytes,
	 * which does not divide the partition's usable length.
	 */
	fsimg_layer_options_init(&layers);
	layers.gpt_disk_bytes = 8 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.sector_bytes = 4096;
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.sector_bytes == 4096 &&
	    info.volume_bytes == (7 * MIB - 33 * 512) / 4096 * 4096);
	fsimg_layer_options_init(&layers);
	layers.mbr_disk_bytes = (UINT64_C(1) << 32) * 512 + 512;
	expect_refused(&f, "fat16/mbr", 0, &layers, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, "mbr:disk");
	/* A volume longer than a fixed disk holds fails at construction,
	 * before any source is read.
	 */
	layers.mbr_disk_bytes = 2 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB + 512;
	bzero(&error, sizeof(error));
	writer = NULL;
	CHECK(fsimg_writer_new(f.ctx, "fat12/mbr", &options, &writer,
	    &error) == FSIMG_EINVAL && writer == NULL);
	CHECK(strcmp(error.path, "mbr:disk") == 0);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/mbr", &options, &info, NULL) == FSIMG_OK);

	/* A fixed disk longer than its volume: zeros up to the backup table,
	 * which ends at the disk's last sector.
	 */
	fsimg_layer_options_init(&layers);
	layers.gpt_disk_bytes = 4 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(create(&f, "fat12/gpt", &options, &disk, NULL) == FSIMG_OK);
	CHECK(disk.length == 4 * MIB && gpt_check(&disk, 512) == NULL);
	CHECK(get64(disk.bytes + 512 + 32) == 4 * MIB / 512 - 1);
	CHECK(all_zero(disk.bytes + 2 * MIB, 2 * MIB - 33 * 512));
	memory_free(&disk);

	/* Identifiers: unset outside -R is refused naming the key. */
	expect_refused(&f, "fat12/gpt", MIB, NULL, 0, FSIMG_EINVAL, "gpt:disk-guid");
	fsimg_layer_options_init(&layers);
	guid_text(DISK_GUID, layers.gpt_disk_guid);
	expect_refused(&f, "fat12/gpt", MIB, &layers, 0, FSIMG_EINVAL, "gpt:guid");
	guid_text(DISK_GUID, layers.gpt_guid);
	expect_refused(&f, "fat12/gpt", MIB, &layers, 0, FSIMG_EINVAL, NULL);
	guid_text(PART_GUID, layers.gpt_guid);
	write_options(&options, 0, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	expect_refused(&f, "fat12/mbr", MIB, NULL, 0, FSIMG_EINVAL, "mbr:id");
	fsimg_layer_options_init(&layers);
	layers.mbr_id_set = true;
	write_options(&options, 0, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/mbr", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.image.mbr_disk_id == 0);

	/* A container's identifier follows the same rule. */
	expect_refused(&f, "fat12/vhd", MIB, NULL, 0, FSIMG_EINVAL, "vhd:uuid");
	expect_refused(&f, "fat12/dmg", MIB, NULL, 0, FSIMG_EINVAL, "dmg:uuid");
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = MIB;
	CHECK(plan_only(&f, "fat12/gpt/vhd", &options, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.image.container, "vhd") == 0 &&
	    info.output_bytes == info.image.disk_bytes + 512);
	expect_refused(&f, "gpt", MIB, NULL, FSIMG_WRITE_REPRODUCIBLE,
	    FSIMG_EINVAL, NULL);
	fixture_free(&f);
}

/* Without settings each filesystem gets its type and the type's name; a
 * bare stack reports the volume as the whole output.
 */
static void
test_derived_defaults(void)
{
	static const struct {
		const char	*format;
		uint32_t	 mbr_type;
		const char	*gpt_guid;
		const char	*gpt_name;
		uint64_t	 volume_bytes;
	} cases[] = {
		{ "fat12", 0x01, BASIC_GUID, "Microsoft basic data", MIB },
		{ "fat16", 0x06, BASIC_GUID, "Microsoft basic data", 8 * MIB },
		{ "fat32", 0x0c, BASIC_GUID, "Microsoft basic data", 64 * MIB },
		{ "exfat", 0x07, BASIC_GUID, "Microsoft basic data", MIB },
		{ "qnx6", 0xb2, QNX6_GUID, "QNX6 Power-Safe", MIB },
		{ "qnx4", 0x4d, NULL, NULL, MIB }
	};
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	unsigned char guid[16];
	char stack[32];
	fixture_t f;

	fixture_init(&f);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
		options.volume_bytes = cases[i].volume_bytes;
		CHECK(plan_only(&f, cases[i].format, &options, &info, NULL) == FSIMG_OK);
		CHECK(info.image.disk_bytes == info.volume_bytes);
		CHECK(info.image.scheme[0] == '\0');
		snprintf(stack, sizeof(stack), "%s/mbr", cases[i].format);
		CHECK(plan_only(&f, stack, &options, &info, NULL) == FSIMG_OK);
		CHECK(info.partition.mbr_type == cases[i].mbr_type);
		CHECK(info.partition.offset == MIB && info.partition.index == 1);
		CHECK(info.image.disk_bytes == MIB + cases[i].volume_bytes);
		CHECK(strcmp(info.image.stack, stack) == 0);
		CHECK(strcmp(info.partition.format, cases[i].format) == 0);
		if (cases[i].gpt_guid == NULL)
			continue;
		snprintf(stack, sizeof(stack), "%s/gpt", cases[i].format);
		CHECK(plan_only(&f, stack, &options, &info, NULL) == FSIMG_OK);
		guid_text(cases[i].gpt_guid, guid);
		CHECK(bcmp(info.partition.type_guid, guid, 16) == 0);
		CHECK(strcmp(info.partition.name, cases[i].gpt_name) == 0);
		CHECK(info.image.disk_bytes == MIB + cases[i].volume_bytes + 33 * 512);
	}
	fixture_free(&f);
}

/*
 * A GPT disk around a FAT12 volume, decoded field by field: both headers
 * and arrays, the one entry with its type, bounds and name, the volume at
 * 1 MiB equal to the bare volume, and zeros everywhere else. The library
 * reads and verifies it. A flipped bit in the entry array fails the
 * decoder's CRC and the library's read as a table error, and a flipped bit
 * in the backup header fails its CRC.
 */
static void
test_gpt_layout(void)
{
	fsimg_write_options_t options;
	fsimg_layer_report_t report;
	fsimg_image_info_t image_info;
	fsimg_partition_info_t partition;
	fsimg_image_t *image;
	fsimg_error_t error;
	fsimg_io_t *io;
	memory_t disk, volume;
	unsigned char guid[16];
	const unsigned char *entry;
	uint64_t last;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = MIB;
	CHECK(create(&f, "fat12/gpt", &options, &disk, NULL) == FSIMG_OK);
	CHECK(disk.length == 2 * MIB + 33 * 512);
	CHECK(gpt_check(&disk, 512) == NULL);
	last = disk.length / 512 - 1;
	entry = gpt_entry(&disk, 512, 1);
	guid_mixed(BASIC_GUID, guid);
	CHECK(bcmp(entry, guid, 16) == 0);
	CHECK(get64(entry + 32) == 2048 && get64(entry + 40) == 4095);
	CHECK(get64(entry + 48) == 0 && name_is(entry, "Microsoft basic data"));
	CHECK(all_zero(entry + GPT_ENTRY_BYTES, (GPT_ENTRIES - 1) * GPT_ENTRY_BYTES));
	CHECK(bcmp(disk.bytes + MIB, volume.bytes, MIB) == 0);
	CHECK(all_zero(disk.bytes + 34 * 512, MIB - 34 * 512));
	CHECK(all_zero(disk.bytes + 2 * MIB, (last - 32) * 512 - 2 * MIB));

	CHECK(open_disk(&f, &disk, 0, &io, &image, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(image, &image_info, NULL) == FSIMG_OK);
	CHECK(fsimg_image_next_partition(image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.offset == MIB && partition.length == MIB &&
	    strcmp(partition.format, "fat12") == 0);
	CHECK(fsimg_image_verify(image, 0, &report, NULL) == FSIMG_OK);
	CHECK(strcmp(report.scheme, "gpt") == 0 && report.partitions == 1);
	fsimg_image_free(image);
	fsimg_io_free(io);

	disk.bytes[2 * 512 + 40] ^= 0x10;
	CHECK(strcmp(gpt_check(&disk, 512), "primary entries CRC") == 0);
	bzero(&error, sizeof(error));
	CHECK(open_disk(&f, &disk, 0, &io, &image, &error) == FSIMG_OK);
	CHECK(fsimg_image_get_info(image, &image_info, &error) == FSIMG_ECORRUPT);
	CHECK(error.operation == FSIMG_OP_IMAGE);
	fsimg_image_free(image);
	fsimg_io_free(io);
	disk.bytes[2 * 512 + 40] ^= 0x10;
	disk.bytes[last * 512 + 24] ^= 1;
	CHECK(strcmp(gpt_check(&disk, 512), "backup header CRC") == 0);
	memory_free(&disk);
	memory_free(&volume);
	fixture_free(&f);
}

/* gpt:sector=4096: the headers at LBA 1 and the last LBA of 4096 bytes,
 * four array sectors, the partition at LBA 256, and the library reading it
 * with the size forced. Decoded at 512 bytes, its protective entry
 * covers the wrong number of sectors.
 */
static void
test_gpt_4kn(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_image_info_t info;
	fsimg_image_t *image;
	fsimg_io_t *io, *volume;
	memory_t disk;
	const unsigned char *entry;
	fixture_t f;

	fixture_init(&f);
	fsimg_layer_options_init(&layers);
	layers.gpt_sector_bytes = 4096;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(create(&f, "exfat/gpt", &options, &disk, NULL) == FSIMG_OK);
	CHECK(disk.length == 2 * MIB + 5 * 4096);
	CHECK(gpt_check(&disk, 4096) == NULL);
	CHECK(strcmp(gpt_check(&disk, 512), "protective MBR") == 0);
	entry = gpt_entry(&disk, 4096, 1);
	CHECK(get64(entry + 32) == 256 && get64(entry + 40) == 511);
	CHECK(open_disk(&f, &disk, 4096, &io, &image, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(image, &info, NULL) == FSIMG_OK);
	CHECK(info.sector_bytes == 4096 && info.disk_bytes == disk.length);
	CHECK(fsimg_image_select(image, &(fsimg_selector_t){ .kind =
	    FSIMG_SELECT_AUTOMATIC }, &volume, NULL) == FSIMG_OK);
	fsimg_io_free(volume);
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&disk);
	fixture_free(&f);
}

/*
 * An MBR disk: the identifier at 440, the one entry with its status, CHS
 * bounds, type, first sector and count, three zero entries and the
 * signature; the volume at 1 MiB and nothing after it. QNX6 takes 0xB2.
 */
static void
test_mbr_layout(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	memory_t disk, volume;
	unsigned char chs[3];
	const unsigned char *entry;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	fsimg_layer_options_init(&layers);
	layers.mbr_active = true;
	layers.mbr_id = 0x0badcafe;
	layers.mbr_id_set = true;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	CHECK(create(&f, "fat12/mbr", &options, &disk, NULL) == FSIMG_OK);
	CHECK(disk.length == 2 * MIB);
	CHECK(all_zero(disk.bytes, 440) && get32(disk.bytes + 440) == 0x0badcafe);
	CHECK(disk.bytes[444] == 0 && disk.bytes[445] == 0);
	entry = disk.bytes + 446;
	CHECK(entry[0] == 0x80 && entry[4] == 0x01);
	chs_oracle(2048, chs);
	CHECK(bcmp(entry + 1, chs, 3) == 0 && chs[0] == 32 && chs[1] == 33);
	chs_oracle(4095, chs);
	CHECK(bcmp(entry + 5, chs, 3) == 0);
	CHECK(get32(entry + 8) == 2048 && get32(entry + 12) == 2048);
	CHECK(all_zero(entry + 16, 48));
	CHECK(disk.bytes[510] == 0x55 && disk.bytes[511] == 0xaa);
	CHECK(all_zero(disk.bytes + 512, MIB - 512));
	CHECK(bcmp(disk.bytes + MIB, volume.bytes, MIB) == 0);
	memory_free(&disk);

	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = MIB;
	CHECK(create(&f, "qnx6/mbr", &options, &disk, NULL) == FSIMG_OK);
	CHECK(disk.bytes[446] == 0x00 && disk.bytes[450] == 0xb2);
	memory_free(&disk);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The MBR encoder's arithmetic on records no small test disk reaches: CHS
 * saturation past cylinder 1023 against an independent oracle, and a
 * partition ending at sector 2^32, the last an entry addresses. One sector
 * more is an internal error, never an encoded table, and so is a disk
 * past 2^32 sectors.
 */
static void
test_mbr_chs_and_limits(void)
{
	static const uint64_t ends[] = { 2048, 2110, 2111, 16064, 16065,
	    UINT64_C(1024) * 255 * 63 - 1, UINT64_C(1024) * 255 * 63,
	    (UINT64_C(1) << 32) - 1 };
	fsimg_partition_record_t record;
	fsimg_scheme_table_t table;
	fsimg_scheme_tables_t tables;
	fsimg_error_t error;
	unsigned char chs[3];
	const unsigned char *entry;
	fixture_t f;

	fixture_init(&f);
	bzero(&table, sizeof(table));
	table.kind = FSIMG_SCHEME_MBR;
	table.sector_bytes = 512;
	table.count = 1;
	table.records = &record;
	bzero(&record, sizeof(record));
	record.index = 1;
	record.used = true;
	record.mbr_type = 0x0c;
	record.offset = 2048 * 512;
	for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
		record.length = (ends[i] - 2048 + 1) * 512;
		CHECK(fsimg_mbr_scheme.encode(f.ctx, &table, UINT64_C(1) << 41,
		    &tables, NULL) == FSIMG_OK);
		entry = tables.regions[0].bytes + 446;
		chs_oracle(2048, chs);
		CHECK(bcmp(entry + 1, chs, 3) == 0);
		chs_oracle(ends[i], chs);
		CHECK(bcmp(entry + 5, chs, 3) == 0);
		CHECK(get32(entry + 8) == 2048 &&
		    get32(entry + 12) == (uint32_t)(ends[i] - 2047));
		fsimg_scheme_tables_free(&tables);
	}
	chs_oracle((UINT64_C(1) << 32) - 1, chs);
	CHECK(chs[0] == 0xfe && chs[1] == 0xff && chs[2] == 0xff);
	chs_oracle(16065, chs);
	CHECK(chs[0] == 0 && chs[1] == 1 && chs[2] == 1);

	record.length += 512;
	CHECK(fsimg_mbr_scheme.encode(f.ctx, &table, UINT64_C(1) << 41, &tables,
	    &error) == FSIMG_ESTATE);
	CHECK(fsimg_mbr_scheme.encode(f.ctx, &table, (UINT64_C(1) << 41) + 512,
	    &tables, &error) == FSIMG_ESTATE);
	fixture_free(&f);
}

/*
 * Reproducible identifiers, byte for byte: the epoch's eight little-endian
 * bytes, the role (1 disk, 2 partition) and the slot, six zeros, then the
 * version 4 and RFC 4122 variant bits; GPT stores them in its mixed order
 * and MBR takes the epoch's low 32 bits. A negative epoch fills the
 * epoch bytes with its two's complement.
 */
static void
test_identifiers(void)
{
	/* As GPT stores 00f15365-0000-4000-8100-000000000000 and its
	 * partition twin 00f15365-0000-4000-8201-...: each leading group
	 * reversed. Epoch -1 is ffffffff-ffff-4fff-8100-....
	 */
	static const unsigned char disk_guid[16] = { 0x65, 0x53, 0xf1, 0x00,
	    0x00, 0x00, 0x00, 0x40, 0x81, 0x00 };
	static const unsigned char partition_guid[16] = { 0x65, 0x53, 0xf1, 0x00,
	    0x00, 0x00, 0x00, 0x40, 0x82, 0x01 };
	static const unsigned char negative_guid[16] = { 0xff, 0xff, 0xff, 0xff,
	    0xff, 0xff, 0xff, 0x4f, 0x81, 0x00 };
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	fsimg_sink_t sink;
	fsimg_io_t *input, *output;
	memory_t disk, again, volume;
	fixture_t f;

	fixture_init(&f);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = MIB;
	CHECK(create(&f, "fat12/gpt", &options, &disk, NULL) == FSIMG_OK);
	CHECK(bcmp(disk.bytes + 512 + 56, disk_guid, 16) == 0);
	CHECK(bcmp(gpt_entry(&disk, 512, 1) + 16, partition_guid, 16) == 0);
	/* Text order: version 4 in byte 6's high nibble, variant 10 in 8's. */
	CHECK(disk_guid[7] >> 4 == 4 && disk_guid[8] >> 6 == 2);
	CHECK(plan_only(&f, "fat12/gpt", &options, &info, NULL) == FSIMG_OK);
	CHECK(info.image.disk_guid[0] == 0x00 && info.image.disk_guid[3] == 0x65 &&
	    info.image.disk_guid[6] == 0x40 && info.image.disk_guid[8] == 0x81);
	CHECK(info.partition.guid[8] == 0x82 && info.partition.guid[9] == 0x01);
	CHECK(create(&f, "fat12/gpt", &options, &again, NULL) == FSIMG_OK);
	CHECK(again.length == disk.length &&
	    bcmp(again.bytes, disk.bytes, disk.length) == 0);
	memory_free(&again);
	memory_free(&disk);

	CHECK(create(&f, "fat12/mbr", &options, &disk, NULL) == FSIMG_OK);
	CHECK(get32(disk.bytes + 440) == 0x6553f100);
	memory_free(&disk);

	/* No FAT timestamp holds 1969, so a wrapped volume carries epoch -1. */
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, -1, NULL);
	for (int scheme = 0; scheme < 2; scheme++) {
		bzero(&disk, sizeof(disk));
		output = memory_io(&f, &disk);
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = output;
		CHECK(fsimg_image_wrap(f.ctx, input, scheme == 0 ? "gpt" : "mbr",
		    &options, &sink, NULL) == FSIMG_OK);
		if (scheme == 0)
			CHECK(bcmp(disk.bytes + 512 + 56, negative_guid, 16) == 0);
		else
			CHECK(get32(disk.bytes + 440) == UINT32_MAX);
		fsimg_io_free(output);
		memory_free(&disk);
	}
	fsimg_io_free(input);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * Wrapping one volume at two epochs: the disks differ only in the derived
 * identifiers and the CRCs over them, each of which does differ. On MBR
 * only the disk identifier differs.
 */
static void
test_epochs_change_only_identifiers(void)
{
	/* Offsets within a 512-byte-sector GPT: each header's CRC, disk GUID
	 * and entry CRC, and the entry's unique GUID, in each copy.
	 */
	static const struct {
		uint64_t	 offset;
		uint64_t	 length;
	} primary[] = { { 512 + 16, 4 }, { 512 + 56, 16 }, { 512 + 88, 4 },
	    { 1024 + 16, 16 } };
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_io_t *input, *output;
	memory_t volume, disks[2];
	uint64_t last, changed[8][2];
	size_t fields;
	bool expected;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	for (int scheme = 0; scheme < 2; scheme++) {
		for (int i = 0; i < 2; i++) {
			bzero(&disks[i], sizeof(disks[i]));
			output = memory_io(&f, &disks[i]);
			write_options(&options, FSIMG_WRITE_REPRODUCIBLE,
			    i == 0 ? EPOCH : OTHER_EPOCH, NULL);
			fsimg_sink_init(&sink, FSIMG_SINK_IO);
			sink.io = output;
			CHECK(fsimg_image_wrap(f.ctx, input, scheme == 0 ? "gpt" : "mbr",
			    &options, &sink, NULL) == FSIMG_OK);
			fsimg_io_free(output);
		}
		CHECK(disks[0].length == disks[1].length);
		fields = 0;
		if (scheme == 0) {
			last = disks[0].length - 512;
			for (size_t i = 0; i < 4; i++) {
				changed[fields][0] = primary[i].offset;
				changed[fields++][1] = primary[i].length;
				changed[fields][0] = primary[i].offset +
				    (primary[i].offset < 1024 ? last - 512 :
				    last - 32 * 512 - 1024);
				changed[fields++][1] = primary[i].length;
			}
		} else {
			changed[fields][0] = 440;
			changed[fields++][1] = 4;
		}
		for (uint64_t byte = 0; byte < disks[0].length; byte++) {
			expected = false;
			for (size_t i = 0; i < fields; i++)
				expected |= byte >= changed[i][0] &&
				    byte < changed[i][0] + changed[i][1];
			if (!expected)
				CHECK(disks[0].bytes[byte] == disks[1].bytes[byte]);
		}
		for (size_t i = 0; i < fields; i++)
			CHECK(bcmp(disks[0].bytes + changed[i][0], disks[1].bytes +
			    changed[i][0], (size_t)changed[i][1]) != 0);
		memory_free(&disks[0]);
		memory_free(&disks[1]);
	}
	fsimg_io_free(input);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * Wrap puts a volume into a scheme, typed from its filesystem, and unwrap
 * returns it byte for byte. A stack naming the filesystem must match the
 * volume; an unrecognised volume needs a type; a bare stack copies the
 * volume; and every filesystem setting must hold its default.
 */
static void
test_wrap_unwrap(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_image_t *image;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input, *output, *io;
	memory_t volume, blank, disk, copy;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	bzero(&disk, sizeof(disk));
	output = memory_io(&f, &disk);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	CHECK(fsimg_image_wrap(f.ctx, input, "fat12/gpt", &options, &sink,
	    NULL) == FSIMG_OK);
	fsimg_io_free(output);
	CHECK(gpt_check(&disk, 512) == NULL);
	CHECK(name_is(gpt_entry(&disk, 512, 1), "Microsoft basic data"));
	CHECK(bcmp(disk.bytes + MIB, volume.bytes, MIB) == 0);
	CHECK(open_disk(&f, &disk, 0, &io, &image, NULL) == FSIMG_OK);
	bzero(&copy, sizeof(copy));
	output = memory_io(&f, &copy);
	sink.io = output;
	CHECK(fsimg_image_unwrap(image, &(fsimg_selector_t){ .kind =
	    FSIMG_SELECT_AUTOMATIC }, NULL, &sink, NULL) == FSIMG_OK);
	CHECK(copy.length == MIB && bcmp(copy.bytes, volume.bytes, MIB) == 0);
	memory_free(&copy);
	CHECK(fsimg_image_unwrap(image, &(fsimg_selector_t){ .kind =
	    FSIMG_SELECT_RANGE, .offset = MIB, .length = 512 }, NULL, &sink,
	    NULL) == FSIMG_OK);
	CHECK(copy.length == 512 && bcmp(copy.bytes, volume.bytes, 512) == 0);
	fsimg_io_free(output);
	memory_free(&copy);
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&disk);

	/* A bare stack copies the volume; MBR takes the volume's type or the
	 * setting's; the stack's filesystem must be the volume's.
	 */
	output = memory_io(&f, &disk);
	sink.io = output;
	CHECK(fsimg_image_wrap(f.ctx, input, "fat12", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(disk.length == MIB && bcmp(disk.bytes, volume.bytes, MIB) == 0);
	CHECK(fsimg_image_wrap(f.ctx, input, "mbr", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(disk.bytes[450] == 0x01 && disk.length == 2 * MIB);
	fsimg_layer_options_init(&layers);
	snprintf(layers.mbr_type, sizeof(layers.mbr_type), "0x0e");
	options.layers = &layers;
	CHECK(fsimg_image_wrap(f.ctx, input, "mbr", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(disk.bytes[450] == 0x0e);
	options.layers = NULL;
	bzero(&error, sizeof(error));
	CHECK(fsimg_image_wrap(f.ctx, input, "fat16/gpt", &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "fat16") == 0);
	options.label = "LABEL";
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    &error) == FSIMG_EINVAL);
	options.label = NULL;
	fsimg_io_free(output);
	memory_free(&disk);

	/* An unrecognised volume has no filesystem to type it. */
	bzero(&blank, sizeof(blank));
	blank.length = MIB;
	blank.bytes = calloc(1, MIB);
	CHECK(blank.bytes != NULL);
	fsimg_io_free(input);
	input = memory_io(&f, &blank);
	output = memory_io(&f, &disk);
	sink.io = output;
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:type") == 0);
	snprintf(layers.mbr_type, sizeof(layers.mbr_type), "linux");
	options.layers = &layers;
	CHECK(fsimg_image_wrap(f.ctx, input, "mbr", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(disk.bytes[450] == 0x83);
	fsimg_io_free(output);
	fsimg_io_free(input);
	memory_free(&disk);
	memory_free(&blank);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The sinks: a file sink publishes under a new name and refuses an
 * existing one, leaving no staging behind either way; a raw intermediate
 * descriptor is refused and closed; a stream is not supported. A writer
 * takes the same sinks.
 */
static void
test_sinks(void)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input;
	memory_t volume;
	unsigned char *bytes;
	struct stat status;
	int fd;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	/* An initialized sink hands over no descriptor, and a zeroed one,
	 * whose raw_fd reads as descriptor 0, is refused without closing it.
	 */
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	CHECK(sink.size == sizeof(sink) && sink.kind == FSIMG_SINK_FILE &&
	    sink.parent_fd == -1 && sink.stream_fd == -1 && sink.raw_fd == -1 &&
	    sink.name == NULL && sink.io == NULL);
	bzero(&sink, sizeof(sink));
	sink.kind = FSIMG_SINK_FILE;
	sink.parent_fd = f.directory_fd;
	sink.name = "zeroed.img";
	/* With descriptor 0 closed, the lowest free descriptor fills it. */
	if (fcntl(0, F_GETFD) == -1)
		CHECK(open("/dev/null", O_RDONLY) == 0);
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    NULL) == FSIMG_EINVAL);
	CHECK(fcntl(0, F_GETFD) != -1);
	CHECK(faccessat(f.directory_fd, "zeroed.img", F_OK, 0) == -1);
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "wrapped.img";
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(fstatat(f.directory_fd, "wrapped.img", &status, 0) == 0);
	CHECK((uint64_t)status.st_size == 2 * MIB + 33 * 512);
	fd = openat(f.directory_fd, "wrapped.img", O_RDONLY);
	CHECK(fd != -1);
	bytes = malloc(MIB);
	CHECK(bytes != NULL && pread(fd, bytes, MIB, (off_t)MIB) == (ssize_t)MIB);
	CHECK(bcmp(bytes, volume.bytes, MIB) == 0);
	free(bytes);
	CHECK(close(fd) == 0);
	bzero(&error, sizeof(error));
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    &error) == FSIMG_EEXISTS);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(leftover_staging(&f) == 0);

	fd = open("/dev/null", O_RDONLY);
	CHECK(fd != -1);
	sink.raw_fd = fd;
	sink.name = "raw.img";
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    NULL) == FSIMG_EINVAL);
	CHECK(fcntl(fd, F_GETFD) == -1);
	/* A stream receives the file's bytes, written in order. */
	fd = openat(f.directory_fd, "streamed.img", O_RDWR | O_CREAT | O_EXCL,
	    0600);
	CHECK(fd != -1);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = fd;
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(fstat(fd, &status) == 0 &&
	    (uint64_t)status.st_size == 2 * MIB + 33 * 512);
	bytes = malloc(MIB);
	CHECK(bytes != NULL && pread(fd, bytes, MIB, (off_t)MIB) == (ssize_t)MIB);
	CHECK(bcmp(bytes, volume.bytes, MIB) == 0);
	free(bytes);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(f.directory_fd, "streamed.img", 0) == 0);
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, NULL,
	    NULL) == FSIMG_EINVAL);

	options.volume_bytes = MIB;
	CHECK(fsimg_writer_new(f.ctx, "fat12/mbr", &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "written.img";
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fstatat(f.directory_fd, "written.img", &status, 0) == 0);
	CHECK((uint64_t)status.st_size == 2 * MIB);
	CHECK(unlinkat(f.directory_fd, "wrapped.img", 0) == 0);
	CHECK(unlinkat(f.directory_fd, "written.img", 0) == 0);
	fsimg_io_free(input);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * Three volumes composed into one GPT: each at the next 1 MiB boundary
 * with its type, name and GUID, the untyped one typed from its
 * filesystem, zeros in every gap over a sink that held other bytes, each
 * typed input read exactly once, and the library listing the three.
 */
static void
test_compose(void)
{
	/* Derived unique GUIDs of slots 1 and 3 as GPT stores them: the
	 * partition role 0x82 and the slot in bytes 8 and 9.
	 */
	static const unsigned char slot_guid[2][16] = {
	    { 0x65, 0x53, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x40, 0x82, 0x01 },
	    { 0x65, 0x53, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x40, 0x82, 0x03 } };
	/* The middle part ends 100 bytes into a sector. */
	static const uint64_t odd_bytes = MIB / 2 + 100;
	fsimg_part_t parts[3];
	fsimg_write_options_t options;
	fsimg_partition_info_t partition;
	fsimg_image_t *image;
	fsimg_sink_t sink;
	fsimg_io_t *volumes[3], *output, *io;
	memory_t inputs[3], disk;
	unsigned char guid[16];
	const unsigned char *entry;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &inputs[0]);
	bzero(&inputs[1], sizeof(inputs[1]));
	inputs[1].length = odd_bytes;
	inputs[1].bytes = malloc(odd_bytes);
	CHECK(inputs[1].bytes != NULL);
	memset(inputs[1].bytes, 0x5a, odd_bytes);
	make_volume(&f, "fat12", MIB, &inputs[2]);
	for (int i = 0; i < 3; i++) {
		volumes[i] = memory_io(&f, &inputs[i]);
		inputs[i].reads = 0;
	}
	bzero(parts, sizeof(parts));
	parts[0] = (fsimg_part_t){ .volume = volumes[0] };
	parts[1] = (fsimg_part_t){ .volume = volumes[1], .type = "linux",
	    .name = "data" };
	guid_text(PART_GUID, parts[1].guid);
	parts[2] = (fsimg_part_t){ .volume = volumes[2], .type = "esp",
	    .name = "EFI" };
	bzero(&disk, sizeof(disk));
	disk.length = 5 * MIB;
	disk.bytes = malloc(disk.length);
	CHECK(disk.bytes != NULL);
	memset(disk.bytes, 0xa5, disk.length);
	output = memory_io(&f, &disk);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 3, &options, &sink,
	    NULL) == FSIMG_OK);
	fsimg_io_free(output);
	CHECK(inputs[1].reads == odd_bytes && inputs[2].reads == MIB);
	CHECK(inputs[0].reads >= MIB);
	CHECK(disk.length == 4 * MIB + 33 * 512);
	CHECK(gpt_check(&disk, 512) == NULL);
	entry = gpt_entry(&disk, 512, 1);
	guid_mixed(BASIC_GUID, guid);
	CHECK(bcmp(entry, guid, 16) == 0 && get64(entry + 32) == 2048);
	CHECK(bcmp(entry + 16, slot_guid[0], 16) == 0);
	/* The partition holds the odd tail's whole sector. */
	entry = gpt_entry(&disk, 512, 2);
	guid_mixed(LINUX_GUID, guid);
	CHECK(bcmp(entry, guid, 16) == 0 && get64(entry + 32) == 4096 &&
	    get64(entry + 40) == 4096 + 1024 && name_is(entry, "data"));
	guid_mixed(PART_GUID, guid);
	CHECK(bcmp(entry + 16, guid, 16) == 0);
	entry = gpt_entry(&disk, 512, 3);
	guid_mixed(ESP_GUID, guid);
	CHECK(bcmp(entry, guid, 16) == 0 && get64(entry + 32) == 6144 &&
	    name_is(entry, "EFI"));
	CHECK(bcmp(entry + 16, slot_guid[1], 16) == 0);
	CHECK(all_zero(disk.bytes + 34 * 512, MIB - 34 * 512));
	CHECK(bcmp(disk.bytes + 2 * MIB, inputs[1].bytes, odd_bytes) == 0);
	CHECK(all_zero(disk.bytes + 2 * MIB + odd_bytes, MIB - odd_bytes));
	CHECK(bcmp(disk.bytes + 3 * MIB, inputs[2].bytes, MIB) == 0);
	CHECK(all_zero(disk.bytes + 4 * MIB, 0));

	CHECK(open_disk(&f, &disk, 0, &io, &image, NULL) == FSIMG_OK);
	for (int i = 0; i < 3; i++) {
		CHECK(fsimg_image_next_partition(image, &partition, NULL) == FSIMG_OK);
		CHECK(partition.used && partition.index == (uint32_t)i + 1);
		CHECK(partition.claim == (i == 1 ? FSIMG_CLAIM_NONE :
		    FSIMG_CLAIM_FORMAT));
	}
	fsimg_image_free(image);
	fsimg_io_free(io);
	for (int i = 0; i < 3; i++) {
		fsimg_io_free(volumes[i]);
		memory_free(&inputs[i]);
	}
	memory_free(&disk);
	fixture_free(&f);
}

/*
 * What compose refuses before touching its sink: five partitions on MBR,
 * none at all, an unrecognised volume without a type, a name or GUID on
 * MBR, two equal GUIDs, a stack naming a filesystem, and partition
 * settings in the layer options. Each failure names the partition or key.
 */
static void
test_compose_refusals(void)
{
	fsimg_part_t parts[5];
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *volume, *blank_io, *output;
	memory_t input, blank, disk;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &input);
	volume = memory_io(&f, &input);
	bzero(&blank, sizeof(blank));
	blank.length = MIB;
	blank.bytes = calloc(1, MIB);
	CHECK(blank.bytes != NULL);
	blank_io = memory_io(&f, &blank);
	bzero(&disk, sizeof(disk));
	output = memory_io(&f, &disk);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	for (int i = 0; i < 5; i++)
		parts[i] = (fsimg_part_t){ .volume = volume };

	bzero(&error, sizeof(error));
	CHECK(fsimg_image_compose(f.ctx, "mbr", parts, 5, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "1 to 4") != NULL);
	CHECK(fsimg_image_compose(f.ctx, "mbr", parts, 4, &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(disk.length == 5 * MIB);
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 0, &options, &sink,
	    NULL) == FSIMG_EINVAL);
	parts[1].volume = blank_io;
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 2") == 0);
	parts[1].type = "linux";
	parts[0].name = "boot";
	CHECK(fsimg_image_compose(f.ctx, "mbr", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "partition 1") == 0);
	parts[0].name = NULL;
	guid_text(PART_GUID, parts[0].guid);
	guid_text(PART_GUID, parts[1].guid);
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "same GUID") != NULL);
	bzero(parts[1].guid, sizeof(parts[1].guid));
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_image_compose(f.ctx, "fat12/gpt", parts, 2, &options, &sink,
	    NULL) == FSIMG_EINVAL);
	CHECK(fsimg_image_compose(f.ctx, "fat12", parts, 2, &options, &sink,
	    NULL) == FSIMG_EINVAL);
	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_name, sizeof(layers.gpt_name), "all");
	options.layers = &layers;
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:name") == 0);
	fsimg_layer_options_init(&layers);
	layers.mbr_active = true;
	CHECK(fsimg_image_compose(f.ctx, "mbr", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:active") == 0);
	fsimg_layer_options_init(&layers);
	guid_text(DISK_GUID, layers.gpt_disk_guid);
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    NULL) == FSIMG_OK);
	CHECK(get32(disk.bytes + 512 + 56) == 0x11111111);
	options.layers = NULL;
	options.flags = 0;
	CHECK(fsimg_image_compose(f.ctx, "gpt", parts, 2, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:disk-guid") == 0);

	fsimg_io_free(output);
	fsimg_io_free(blank_io);
	fsimg_io_free(volume);
	memory_free(&disk);
	memory_free(&blank);
	memory_free(&input);
	fixture_free(&f);
}

/*
 * A fixed disk far longer than its volume, published as a fresh file, is
 * written without its zero runs: the gaps are zeroed while the staged file
 * is still known to be zero, so they stay holes. The context's written
 * bytes count what reached the file whatever the host filesystem keeps:
 * 256 MiB of disk around a 1 MiB volume writes a few MiB.
 */
static void
test_fixed_disk_stays_sparse(void)
{
	fsimg_layer_options_t layers;
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_stats_t before, after;
	struct stat status;
	fixture_t f;

	fixture_init(&f);
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	fsimg_layer_options_init(&layers);
	layers.gpt_disk_bytes = 256 * MIB;
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, &layers);
	options.volume_bytes = MIB;
	writer = NULL;
	CHECK(fsimg_writer_new(f.ctx, "fat12/gpt", &options, &writer,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_writer_write_at(writer, f.directory_fd, "sparse.img",
	    NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fstatat(f.directory_fd, "sparse.img", &status,
	    AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(status.st_size == 256 * MIB);
	CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.written_bytes - before.written_bytes < 16 * MIB);
	CHECK(unlinkat(f.directory_fd, "sparse.img", 0) == 0);
	fixture_free(&f);
}

/*
 * An output sharing storage with an input is refused before the output is
 * touched, whether the input is the volume itself or a slice of it, and
 * so is an output in another context.
 */
static void
test_aliases(void)
{
	fsimg_write_options_t options;
	fsimg_part_t part;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input, *slice, *output;
	fsimg_ctx_t *other;
	memory_t volume;
	unsigned char *before, *after;
	int fd;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	fd = openat(f.directory_fd, "volume.img", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(pwrite(fd, volume.bytes, MIB, 0) == (ssize_t)MIB);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, &input,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(input, 0, MIB / 2, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, &output,
	    NULL) == FSIMG_OK);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	bzero(&error, sizeof(error));
	CHECK(fsimg_image_wrap(f.ctx, input, "gpt", &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "output aliases an input") == 0);
	part = (fsimg_part_t){ .volume = slice, .type = "linux" };
	CHECK(fsimg_image_compose(f.ctx, "gpt", &part, 1, &options, &sink,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "output aliases an input") == 0);
	before = volume.bytes;
	after = malloc(MIB);
	CHECK(after != NULL && pread(fd, after, MIB, 0) == (ssize_t)MIB);
	CHECK(bcmp(before, after, MIB) == 0);
	free(after);

	CHECK(fsimg_ctx_new(NULL, &other, NULL) == FSIMG_OK);
	CHECK(fsimg_image_wrap(other, input, "gpt", &options, &sink,
	    NULL) == FSIMG_EINVAL);
	fsimg_ctx_free(other);
	fsimg_io_free(output);
	fsimg_io_free(slice);
	fsimg_io_free(input);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(f.directory_fd, "volume.img", 0) == 0);
	memory_free(&volume);
	fixture_free(&f);
}

/* A cancellation before publication publishes nothing and leaves no
 * staging; one observed while the published result is made durable says
 * the output was published.
 */
static void
expect_cancelled(fixture_t *f, fsimg_status_t status,
    const fsimg_error_t *error, const char *name)
{
	if (status == FSIMG_EDURABILITY) {
		CHECK(error->output_state == FSIMG_OUTPUT_PUBLISHED);
		CHECK(unlinkat(f->directory_fd, name, 0) == 0);
	} else {
		CHECK(status == FSIMG_ECANCELLED);
		CHECK(faccessat(f->directory_fd, name, F_OK, 0) == -1);
	}
	CHECK(leftover_staging(f) == 0);
}

/*
 * Cancellation is polled while the tables are written and while the disk
 * is copied: a cancelled writer or compose fails with FSIMG_ECANCELLED and
 * publishes nothing. Every poll position up to completion is tried.
 */
static void
test_cancellation(void)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_part_t part;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input;
	fsimg_status_t status;
	memory_t volume;
	unsigned cancelled;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	part = (fsimg_part_t){ .volume = input };
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "cancelled.img";
	cancelled = 0;
	for (f.cancel_after = 1;; f.cancel_after++) {
		f.polls = 0;
		status = fsimg_image_compose(f.ctx, "gpt", &part, 1, &options, &sink,
		    &error);
		if (status == FSIMG_OK)
			break;
		expect_cancelled(&f, status, &error, "cancelled.img");
		cancelled++;
	}
	CHECK(cancelled > 16);
	CHECK(unlinkat(f.directory_fd, "cancelled.img", 0) == 0);

	options.volume_bytes = MIB;
	cancelled = 0;
	for (f.cancel_after = 1;; f.cancel_after++) {
		f.polls = 0;
		writer = NULL;
		status = fsimg_writer_new(f.ctx, "fat12/gpt", &options, &writer,
		    &error);
		if (status == FSIMG_OK)
			status = fsimg_writer_write_at(writer, f.directory_fd,
			    "written.img", &error);
		fsimg_writer_free(writer);
		if (status == FSIMG_OK)
			break;
		expect_cancelled(&f, status, &error, "written.img");
		cancelled++;
	}
	CHECK(cancelled > 16);
	CHECK(unlinkat(f.directory_fd, "written.img", 0) == 0);
	f.cancel_after = 0;
	fsimg_io_free(input);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * An allocation failure at every charged allocation of a layered write, a
 * compose and a layered read returns FSIMG_ENOMEM, releases every charge
 * and publishes nothing; past the last allocation the operation succeeds.
 */
static void
test_allocation_failures(void)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_image_info_t info;
	fsimg_image_t *image;
	fsimg_part_t part;
	fsimg_sink_t sink;
	fsimg_stats_t before, after;
	fsimg_io_t *input, *io;
	fsimg_status_t status;
	memory_t volume, disk;
	unsigned failures;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", MIB, &volume);
	input = memory_io(&f, &volume);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	part = (fsimg_part_t){ .volume = input };
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "composed.img";
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	failures = 0;
	for (uint64_t attempt = 1;; attempt++) {
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_image_compose(f.ctx, "gpt", &part, 1, &options, &sink,
		    NULL);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(faccessat(f.directory_fd, "composed.img", F_OK, 0) == -1);
		CHECK(leftover_staging(&f) == 0);
		CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.memory_bytes == before.memory_bytes);
		failures++;
	}
	CHECK(failures >= 3);

	options.volume_bytes = MIB;
	failures = 0;
	for (uint64_t attempt = 1;; attempt++) {
		writer = NULL;
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_writer_new(f.ctx, "fat12/gpt", &options, &writer, NULL);
		if (status == FSIMG_OK)
			status = fsimg_writer_write_at(writer, f.directory_fd,
			    "written.img", NULL);
		fsimg_writer_free(writer);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(faccessat(f.directory_fd, "written.img", F_OK, 0) == -1);
		CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.memory_bytes == before.memory_bytes);
		failures++;
	}
	CHECK(failures >= 3);

	/* The read side: the image open, the GPT's charged tables and the
	 * overlap check each fail cleanly.
	 */
	CHECK(create(&f, "fat12/gpt", &options, &disk, NULL) == FSIMG_OK);
	failures = 0;
	for (uint64_t attempt = 1;; attempt++) {
		image = NULL;
		io = memory_io(&f, &disk);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_image_open(f.ctx, io, NULL, &image, NULL);
		if (status == FSIMG_OK)
			status = fsimg_image_get_info(image, &info, NULL);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fsimg_image_free(image);
		fsimg_io_free(io);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.memory_bytes == before.memory_bytes);
		failures++;
	}
	CHECK(failures >= 4);
	CHECK(unlinkat(f.directory_fd, "composed.img", 0) == 0);
	CHECK(unlinkat(f.directory_fd, "written.img", 0) == 0);
	memory_free(&disk);
	fsimg_io_free(input);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * A stream holds the raw disk in its intermediate while it copies it out,
 * so it costs twice the disk of output budget. A limit between the two
 * refuses the stream before anything is written, closes the handed-over
 * descriptor and leaves the writer usable for a file, which costs the disk
 * alone. A writer that is not there closes the descriptor too.
 */
static void
test_stream_budget(void)
{
	fsimg_options_t context_options;
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	struct stat status;
	int raw, stream;
	fixture_t f;

	fixture_init(&f);
	fsimg_options_init(&context_options);
	context_options.memory_bytes = 64 * MIB;
	context_options.scratch_directory_fd = f.directory_fd;
	context_options.max_output_bytes = MIB + MIB / 2;
	CHECK(fsimg_ctx_new(&context_options, &ctx, NULL) == FSIMG_OK);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	options.volume_bytes = MIB;
	CHECK(fsimg_writer_new(ctx, "fat12", &options, &writer, NULL) ==
	    FSIMG_OK);
	stream = openat(f.directory_fd, "stream.img", O_RDWR | O_CREAT | O_EXCL,
	    0600);
	raw = openat(f.directory_fd, "raw", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(stream != -1 && raw != -1);
	CHECK(unlinkat(f.directory_fd, "raw", 0) == 0);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = stream;
	sink.raw_fd = raw;
	bzero(&error, sizeof(error));
	CHECK(fsimg_writer_output(writer, &sink, &error) == FSIMG_ELIMIT);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(fcntl(raw, F_GETFD) == -1);
	CHECK(fstat(stream, &status) == 0 && status.st_size == 0);
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = f.directory_fd;
	sink.name = "file.img";
	CHECK(fsimg_writer_output(writer, &sink, NULL) == FSIMG_OK);
	CHECK(fstatat(f.directory_fd, "file.img", &status, 0) == 0 &&
	    (uint64_t)status.st_size == MIB);
	fsimg_writer_free(writer);

	raw = openat(f.directory_fd, "raw", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(raw != -1);
	CHECK(unlinkat(f.directory_fd, "raw", 0) == 0);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = stream;
	sink.raw_fd = raw;
	CHECK(fsimg_writer_output(NULL, &sink, NULL) != FSIMG_OK);
	CHECK(fcntl(raw, F_GETFD) == -1);
	CHECK(close(stream) == 0);
	CHECK(unlinkat(f.directory_fd, "stream.img", 0) == 0);
	CHECK(unlinkat(f.directory_fd, "file.img", 0) == 0);
	fsimg_ctx_free(ctx);
	fixture_free(&f);
}

/*
 * simg counts its chunks on a first pass, writes the count in its header,
 * and emits the chunks on a second. Wrap reads its input on both, so an
 * input whose middle zero unit turns to data between them would emit a
 * chunk the header does not count: the second pass refuses it as a
 * changed source, leaving the output it touched incomplete.
 */
static void
test_simg_changed_input(void)
{
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input, *output;
	memory_t volume, image;
	fixture_t f;

	fixture_init(&f);
	make_volume(&f, "fat12", 3 * MIB, &volume);
	CHECK(all_zero(volume.bytes + MIB, 2 * MIB));
	volume.flip_offset = MIB + MIB / 2;
	input = memory_io(&f, &volume);
	bzero(&image, sizeof(image));
	output = memory_io(&f, &image);
	write_options(&options, FSIMG_WRITE_REPRODUCIBLE, EPOCH, NULL);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	bzero(&error, sizeof(error));
	CHECK(fsimg_image_wrap(f.ctx, input, "simg", &options, &sink,
	    &error) == FSIMG_ESOURCE_CHANGED);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(volume.flip_reads == 2);
	fsimg_io_free(output);
	fsimg_io_free(input);
	memory_free(&image);
	memory_free(&volume);
	fixture_free(&f);
}

int
main(void)
{
	test_settings();
	test_derived_defaults();
	test_gpt_layout();
	test_gpt_4kn();
	test_mbr_layout();
	test_mbr_chs_and_limits();
	test_identifiers();
	test_epochs_change_only_identifiers();
	test_wrap_unwrap();
	test_sinks();
	test_stream_budget();
	test_simg_changed_input();
	test_compose();
	test_compose_refusals();
	test_aliases();
	test_fixed_disk_stays_sparse();
	test_cancellation();
	test_allocation_failures();
	printf("layer write tests passed\n");
	return (0);
}
