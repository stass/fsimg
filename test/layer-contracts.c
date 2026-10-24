/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "contract.h"
#include "host.h"
#include "internal.h"

/*
 * Common image-layer contracts (specs/design-driver-contract-tests.md,
 * "Layer profiles and composition").
 *
 * Each registered scheme, container and compression has one coverage
 * profile: its registry name, kind, carried filesystem, capabilities and
 * an explicit list of the modes it supports, each with its own read, write
 * and verify expectations. The registry selects the obligations; the
 * profile and scenario contracts give every expected answer. A layer's
 * scenarios run on a suitable payload volume, which the common writer
 * produces; specialised checks, checksums and malformed offsets stay in
 * the layer-local tests and the integration scripts with other tools.
 *
 * Every write here is a wrap of a payload volume, the one sequential
 * transcode a writer also uses; every read opens the result as an image,
 * selects the volume and compares its bytes with the payload exactly.
 */

#define COUNT(array)	(sizeof(array) / sizeof((array)[0]))
#define KIB		UINT64_C(1024)
#define MIB		(KIB * KIB)

/* The payload a layer carries: a FAT12 volume for disk layers, an
 * ImageFS for containers that carry ImageFS.
 */
typedef enum payload {
	PAYLOAD_FAT12,
	PAYLOAD_IFS
} payload_t;

/* A mode: the settings it writes with and what it supports. */
typedef struct layer_mode {
	const char	*id;
	unsigned	 capabilities;
	/* Explicit settings, applied after fsimg_layer_options_init(). */
	uint32_t	 gpt_sector_bytes;
	fsimg_dmg_compression_t dmg_compress;
	bool		 simg_crc;
	fsimg_ifsboot_compression_t ifsboot_compress;
	bool		 ifsboot_machine0;
} layer_mode_t;

typedef struct contract_layer_profile {
	const char	*name;
	fsimg_layer_kind_t kind;
	unsigned	 expected_capabilities;
	const char	*carries;
	payload_t	 payload;
	/* The stack a wrap and a forced read name. */
	const char	*stack;
	const layer_mode_t *modes;
	size_t		 mode_count;
	/* Report fields a successful verification leaves unverified. */
	const char	*unverified;
	/* Whether reading decodes the disk into the spool. */
	bool		 spooled;
	/* The independent encoder of a layer this build reads but never
	 * writes, from a payload volume into a new encoded volume.
	 */
	void		(*materialize)(const contract_volume_t *,
			    contract_volume_t *);
} contract_layer_profile_t;

static void hbcifs_build(const contract_volume_t *, contract_volume_t *);

#define RW_VERIFY	(FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY)
#define R_VERIFY	(FSIMG_CAP_READ | FSIMG_CAP_VERIFY)

static const layer_mode_t gpt_modes[] = {
	{ .id = "sector-512", .capabilities = RW_VERIFY,
	    .gpt_sector_bytes = 512 },
	{ .id = "sector-4096", .capabilities = RW_VERIFY,
	    .gpt_sector_bytes = 4096 },
};
static const layer_mode_t mbr_modes[] = {
	{ .id = "default", .capabilities = RW_VERIFY },
};
static const layer_mode_t dmg_modes[] = {
	{ .id = "zlib", .capabilities = RW_VERIFY,
	    .dmg_compress = FSIMG_DMG_ZLIB },
	{ .id = "raw", .capabilities = RW_VERIFY,
	    .dmg_compress = FSIMG_DMG_NONE },
};
static const layer_mode_t vhd_modes[] = {
	{ .id = "fixed", .capabilities = RW_VERIFY },
};
static const layer_mode_t simg_modes[] = {
	{ .id = "no-crc", .capabilities = RW_VERIFY },
	{ .id = "crc", .capabilities = RW_VERIFY, .simg_crc = true },
};
/* Machine 0, nonbootable mkifs output, is the one machine the writer
 * accepts under both compressions (design-layers.md, the ifsboot table).
 */
static const layer_mode_t ifsboot_modes[] = {
	{ .id = "machine0-lzo", .capabilities = RW_VERIFY,
	    .ifsboot_compress = FSIMG_IFSBOOT_LZO, .ifsboot_machine0 = true },
	{ .id = "machine0-none", .capabilities = RW_VERIFY,
	    .ifsboot_compress = FSIMG_IFSBOOT_NONE, .ifsboot_machine0 = true },
};
/* hbcifs is read and verified, never written. */
static const layer_mode_t hbcifs_modes[] = {
	{ .id = "lzo1x", .capabilities = R_VERIFY },
};
static const layer_mode_t gzip_modes[] = {
	{ .id = "default-level", .capabilities = RW_VERIFY },
};

/* zstd and xz are registered names this build neither reads nor writes;
 * they have no modes, and their scenario is the specified refusal.
 */
static const contract_layer_profile_t layer_profiles[] = {
	{ .name = "gpt", .kind = FSIMG_LAYER_SCHEME,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12/gpt",
	    .modes = gpt_modes, .mode_count = COUNT(gpt_modes) },
	{ .name = "mbr", .kind = FSIMG_LAYER_SCHEME,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12/mbr",
	    .modes = mbr_modes, .mode_count = COUNT(mbr_modes) },
	{ .name = "dmg", .kind = FSIMG_LAYER_CONTAINER,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12/dmg",
	    .modes = dmg_modes, .mode_count = COUNT(dmg_modes) },
	{ .name = "vhd", .kind = FSIMG_LAYER_CONTAINER,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12/vhd",
	    .modes = vhd_modes, .mode_count = COUNT(vhd_modes) },
	{ .name = "simg", .kind = FSIMG_LAYER_CONTAINER,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12/simg",
	    .modes = simg_modes, .mode_count = COUNT(simg_modes) },
	{ .name = "ifsboot", .kind = FSIMG_LAYER_CONTAINER,
	    .expected_capabilities = RW_VERIFY, .carries = "ifs",
	    .payload = PAYLOAD_IFS, .stack = "ifs/ifsboot",
	    .modes = ifsboot_modes, .mode_count = COUNT(ifsboot_modes) },
	{ .name = "hbcifs", .kind = FSIMG_LAYER_CONTAINER,
	    .expected_capabilities = R_VERIFY, .carries = "ifs",
	    .payload = PAYLOAD_IFS, .stack = "ifs/hbcifs",
	    .modes = hbcifs_modes, .mode_count = COUNT(hbcifs_modes),
	    .unverified = "0x14,0x1c", .materialize = hbcifs_build,
	    .spooled = true },
	{ .name = "gzip", .kind = FSIMG_LAYER_COMPRESSION,
	    .expected_capabilities = RW_VERIFY, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12",
	    .modes = gzip_modes, .mode_count = COUNT(gzip_modes),
	    .spooled = true },
	{ .name = "zstd", .kind = FSIMG_LAYER_COMPRESSION,
	    .expected_capabilities = 0, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12" },
	{ .name = "xz", .kind = FSIMG_LAYER_COMPRESSION,
	    .expected_capabilities = 0, .carries = "",
	    .payload = PAYLOAD_FAT12, .stack = "fat12" },
};

/*
 * A growable memory object: the sink of every output. It resizes, so a
 * container's or compression's encoded length can be set at the end;
 * faults count calls from one and zero disables each.
 */
typedef struct memory_file {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	uint64_t	 fail_write;
	bool		 fault_fired;
} memory_file_t;

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_file_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_file_t *file = argument;

	CONTRACT_CHECK(offset <= file->length &&
	    length <= file->length - offset);
	bcopy(file->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_file_t *file = argument;

	*transferred = 0;
	CONTRACT_CHECK(offset <= file->length &&
	    length <= file->length - offset);
	file->writes++;
	if (file->writes == file->fail_write) {
		file->fault_fired = true;
		return (FSIMG_EIO);
	}
	bcopy(buffer, file->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument, uint64_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_file_t *file = argument;
	unsigned char *bytes;

	CONTRACT_CHECK(length <= 256 * MIB);
	bytes = realloc(file->bytes, length == 0 ? 1 : (size_t)length);
	CONTRACT_CHECK(bytes != NULL);
	if (length > file->length)
		memset(bytes + file->length, 0xa5, (size_t)(length - file->length));
	file->bytes = bytes;
	file->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_io_t *
memory_io(fsimg_ctx_t *ctx, memory_file_t *file)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	fsimg_error_t error;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.resize = memory_resize;
	ops.flush = memory_flush;
	CONTRACT_STATUS(fsimg_io_from_callbacks(ctx, &ops, file, &io, &error),
	    FSIMG_OK, &error);
	return (io);
}

static void
memory_free(memory_file_t *file)
{
	free(file->bytes);
	bzero(file, sizeof(*file));
}

/* One case's directory, which holds spools, and its context. */
typedef struct fixture {
	contract_dir_t	 dir;
	fsimg_ctx_t	*ctx;
	fsimg_stats_t	 baseline;
} fixture_t;

static void
fixture_open(fixture_t *fixture, uint64_t spool_bytes)
{
	fsimg_options_t options;
	fsimg_error_t error;

	bzero(fixture, sizeof(*fixture));
	contract_dir_open(&fixture->dir);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	options.scratch_directory_fd = fixture->dir.fd;
	if (spool_bytes != 0)
		options.spool_bytes = spool_bytes;
	CONTRACT_STATUS(fsimg_ctx_new(&options, &fixture->ctx, &error),
	    FSIMG_OK, &error);
	contract_baseline(fixture->ctx, &fixture->baseline);
}

static void
fixture_close(fixture_t *fixture)
{
	contract_phase("cleanup", contract_run.fault);
	contract_check_released(fixture->ctx, &fixture->baseline);
	fsimg_ctx_free(fixture->ctx);
	contract_dir_close(&fixture->dir);
}

/* An exclusively created, unlinked spool in the fixture directory. */
static int
spool_new(fixture_t *fixture)
{
	int fd;

	fd = openat(fixture->dir.fd, "spool", O_RDWR | O_CREAT | O_EXCL |
	    O_CLOEXEC | O_NOFOLLOW, 0600);
	CONTRACT_CHECK(fd != -1);
	CONTRACT_CHECK(unlinkat(fixture->dir.fd, "spool", 0) == 0);
	return (fd);
}

/*
 * Payload volumes, written by the common writer: a reproducible FAT12 or
 * ImageFS holding a directory, an empty file and two pattern files.
 */
#define PAYLOAD_FILE_BYTES	150000u

static fsimg_status_t
pattern_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
pattern_read(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	unsigned char *bytes = buffer;

	for (size_t i = 0; i < length; i++)
		bytes[i] = contract_pattern(id, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

static void
payload_build(payload_t payload, contract_volume_t *volume)
{
	static const struct {
		const char	*path;
		fsimg_entry_kind_t kind;
		uint64_t	 size;
	} tree[] = {
		{ "/dir", FSIMG_ENTRY_DIRECTORY, 0 },
		{ "/empty", FSIMG_ENTRY_FILE, 0 },
		{ "/one", FSIMG_ENTRY_FILE, PAYLOAD_FILE_BYTES },
		{ "/dir/two", FSIMG_ENTRY_FILE, PAYLOAD_FILE_BYTES + 7 },
	};
	fixture_t fixture;
	fsimg_write_options_t options;
	fsimg_source_ops_t source_ops;
	fsimg_writer_t *writer;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *backing, *slice;
	fsimg_entry_t entry;
	fsimg_source_t source;
	fsimg_error_t error;

	contract_phase("payload", 0);
	fixture_open(&fixture, 0);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CONTRACT_STATUS(fsimg_writer_new(fixture.ctx, payload == PAYLOAD_IFS ?
	    "ifs" : "fat12", &options, &writer, &error), FSIMG_OK, &error);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = pattern_validate;
	source_ops.read_at = pattern_read;
	CONTRACT_STATUS(fsimg_writer_set_sources(writer, &source_ops, NULL,
	    &error), FSIMG_OK, &error);
	for (size_t i = 0; i < COUNT(tree); i++) {
		bzero(&entry, sizeof(entry));
		entry.path = tree[i].path;
		entry.kind = tree[i].kind;
		entry.size = tree[i].size;
		source = (fsimg_source_t){ .kind = FSIMG_SOURCE_PROVIDER,
		    .id = i + 1 };
		CONTRACT_STATUS(fsimg_writer_add(writer, &entry,
		    tree[i].kind == FSIMG_ENTRY_FILE ? &source : NULL, &error),
		    FSIMG_OK, &error);
	}
	CONTRACT_STATUS(fsimg_writer_plan(writer, &plan, &error), FSIMG_OK,
	    &error);
	CONTRACT_STATUS(fsimg_plan_get_info(plan, &info, &error), FSIMG_OK,
	    &error);
	CONTRACT_CHECK(info.volume_bytes <= 8 * MIB);
	contract_volume_init(volume, info.volume_bytes, 0);
	slice = contract_volume_open(fixture.ctx, volume, true, &backing);
	CONTRACT_STATUS(fsimg_writer_emit(writer, slice, &error), FSIMG_OK,
	    &error);
	contract_volume_close(slice, backing);
	fsimg_writer_free(writer);
	fixture_close(&fixture);
	contract_volume_reset(volume);
}

/*
 * The hbcifs materializer, independent of the library: the Harman-Becker
 * header (design-layers.md, "Harman-Becker IFS container") around an
 * LZO1X stream (design-codecs.md) of one literal run and the end marker.
 */
#define HBCIFS_HEADER	64u
/* The header's closed-table values of the unidentified field at 0x10 and
 * the four bytes at 0x18; and arbitrary values of the two fields at 0x14
 * and 0x1c, which verification names as unverified. The stream itself
 * starts after the 64-byte header.
 */
#define HBCIFS_FIELD_10		32u
#define HBCIFS_FIELD_14		0x1234abcdu
#define HBCIFS_FIELD_1C		0x89abcdefu

static void
put_le32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static void
hbcifs_build(const contract_volume_t *payload, contract_volume_t *out)
{
	static const unsigned char field_18[4] = { 2, 1, 1, 0 };
	const unsigned char *plain = payload->bytes + CONTRACT_GUARD;
	unsigned char *bytes;
	uint64_t stream, value, zeros;
	size_t at;

	CONTRACT_CHECK(payload->length > 18 && payload->length < UINT32_MAX);
	/* 0x00, then one zero per 255 of length - 18, then the remainder:
	 * a literal run of the whole payload; then the end marker.
	 */
	value = payload->length - 18;
	zeros = value == 0 ? 0 : (value - 1) / 255;
	stream = 1 + zeros + 1 + payload->length + 3;
	contract_volume_init(out, HBCIFS_HEADER + stream, 0);
	bytes = contract_volume_bytes(out);
	memcpy(bytes, "hbcifs\0", 8);
	put_le32(bytes + 0x08, (uint32_t)payload->length);
	put_le32(bytes + 0x0c, (uint32_t)stream);
	put_le32(bytes + 0x10, HBCIFS_FIELD_10);
	put_le32(bytes + 0x14, HBCIFS_FIELD_14);
	memcpy(bytes + 0x18, field_18, 4);
	put_le32(bytes + 0x1c, HBCIFS_FIELD_1C);
	at = HBCIFS_HEADER;
	bytes[at++] = 0;
	for (uint64_t i = 0; i < zeros; i++)
		bytes[at++] = 0;
	bytes[at++] = (unsigned char)(value - 255 * zeros);
	memcpy(bytes + at, plain, (size_t)payload->length);
	at += (size_t)payload->length;
	bytes[at++] = 0x11;
	bytes[at++] = 0;
	bytes[at++] = 0;
	CONTRACT_CHECK(at == HBCIFS_HEADER + stream);
}

/* Startup code for ifsboot: opaque bytes the container stores verbatim. */
static unsigned char startup_code[512];

static void
layer_options(const layer_mode_t *mode, fsimg_layer_options_t *layers)
{
	fsimg_layer_options_init(layers);
	layers->gpt_sector_bytes = mode->gpt_sector_bytes;
	layers->dmg_compress = mode->dmg_compress;
	layers->simg_crc = mode->simg_crc;
	if (mode->ifsboot_machine0) {
		layers->ifsboot_compress = mode->ifsboot_compress;
		layers->ifsboot_machine = 0;
		layers->ifsboot_machine_set = true;
		layers->ifsboot_virtual = false;
		layers->ifsboot_virtual_set = true;
		layers->ifsboot_startup_vaddr = 0x00401000;
		layers->ifsboot_startup_vaddr_set = true;
		layers->ifsboot_paddr_bias = 0;
		layers->ifsboot_paddr_bias_set = true;
		layers->ifsboot_image_paddr = 0x00400000;
		layers->ifsboot_image_paddr_set = true;
		layers->ifsboot_ram_paddr = 0x00400000;
		layers->ifsboot_ram_paddr_set = true;
		layers->ifsboot_imagefs_paddr = 0x00500000;
		layers->ifsboot_imagefs_paddr_set = true;
	}
}

static fsimg_status_t
startup_size(void *argument FSIMG_UNUSED, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*length = sizeof(startup_code);
	return (FSIMG_OK);
}

static fsimg_status_t
startup_read(void *argument FSIMG_UNUSED, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	CONTRACT_CHECK(offset <= sizeof(startup_code) &&
	    length <= sizeof(startup_code) - offset);
	bcopy(startup_code + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

/*
 * Wrap the payload volume into the profile's stack in one mode, writing
 * the sink sequentially. Gzip compresses the bare volume. An allocation
 * failure is armed at fail_alloc for the wrap alone. Returns the wrap's
 * status; out holds what the sink received.
 */
static fsimg_status_t
layer_wrap(fixture_t *fixture, const contract_layer_profile_t *profile,
    const layer_mode_t *mode, contract_volume_t *payload, uint64_t fail_alloc,
    memory_file_t *out, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_io_ops_t startup_ops;
	fsimg_io_t *backing, *input, *sink_io, *startup = NULL;
	fsimg_sink_t sink;
	fsimg_status_t status;

	layer_options(mode, &layers);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.layers = &layers;
	if (profile->kind == FSIMG_LAYER_COMPRESSION)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    profile->name);
	if (mode->ifsboot_machine0) {
		fsimg_io_ops_init(&startup_ops);
		startup_ops.get_size = startup_size;
		startup_ops.read_at = startup_read;
		CONTRACT_STATUS(fsimg_io_from_callbacks(fixture->ctx,
		    &startup_ops, NULL, &startup, error), FSIMG_OK, error);
		inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = startup };
		options.inputs = &inputs;
	}
	input = contract_volume_open(fixture->ctx, payload, false, &backing);
	sink_io = memory_io(fixture->ctx, out);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = sink_io;
	contract_arm_alloc(fixture->ctx, fail_alloc);
	status = fsimg_image_wrap(fixture->ctx, input, profile->stack, &options,
	    &sink, error);
	fsimg_io_free(sink_io);
	contract_volume_close(input, backing);
	fsimg_io_free(startup);
	return (status);
}

/* The read-side observations of one layer image. */
typedef struct layer_read {
	fsimg_image_info_t info;
	fsimg_partition_info_t partition;
	fsimg_layer_report_t report;
} layer_read_t;

/*
 * Open the encoded bytes as an image with the profile's stack forced,
 * select the payload (partition 1 of a scheme, the disk otherwise), compare
 * every byte with the payload, then verify with payload checks, with an
 * allocation failure armed at fail_alloc (zero for none). Returns the
 * first failure.
 */
static fsimg_status_t
layer_read(fixture_t *fixture, const contract_layer_profile_t *profile,
    contract_volume_t *encoded, const contract_volume_t *payload,
    uint64_t fail_alloc, layer_read_t *read, fsimg_error_t *error)
{
	fsimg_image_options_t options;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_t *image = NULL;
	fsimg_io_t *backing, *input, *volume = NULL;
	unsigned char buffer[65536];
	uint64_t length, offset;
	size_t count;
	fsimg_status_t status;

	bzero(read, sizeof(*read));
	input = contract_volume_open(fixture->ctx, encoded, false, &backing);
	fsimg_image_options_init(&options);
	options.spool_fd = spool_new(fixture);
	if (profile->kind == FSIMG_LAYER_COMPRESSION)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    profile->name);
	status = fsimg_stack_parse(profile->stack, &options.stack, error);
	CONTRACT_STATUS(status, FSIMG_OK, error);
	/* A fault is armed for the library's operations, not the adapter. */
	contract_arm_alloc(fixture->ctx, fail_alloc);
	status = fsimg_image_open(fixture->ctx, input, &options, &image, error);
	if (status == FSIMG_OK)
		status = fsimg_image_get_info(image, &read->info, error);
	if (status == FSIMG_OK && profile->kind == FSIMG_LAYER_SCHEME) {
		status = fsimg_image_next_partition(image, &read->partition,
		    error);
		selector = (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
		    .index = 1 };
	}
	if (status == FSIMG_OK)
		status = fsimg_image_select(image, &selector, &volume, error);
	if (status == FSIMG_OK)
		status = fsimg_io_get_size(volume, &length, error);
	if (status == FSIMG_OK)
		CONTRACT_CHECK(length >= payload->length);
	for (offset = 0; status == FSIMG_OK && offset < length;
	    offset += count) {
		count = length - offset < sizeof(buffer) ?
		    (size_t)(length - offset) : sizeof(buffer);
		status = fsimg_io_read_exact(volume, offset, buffer, count,
		    &count, error);
		if (status != FSIMG_OK)
			break;
		/* The payload, then any whole-sector slack as zeros. */
		for (size_t i = 0; i < count; i++)
			CONTRACT_CHECK(buffer[i] == (offset + i < payload->length ?
			    payload->bytes[CONTRACT_GUARD + offset + i] : 0));
	}
	fsimg_io_free(volume);
	if (status == FSIMG_OK)
		status = fsimg_image_verify(image, FSIMG_VERIFY_PAYLOAD,
		    &read->report, error);
	fsimg_image_free(image);
	contract_volume_close(input, backing);
	return (status);
}

/* Copy a sink's bytes into a canaried volume to read back. */
static void
encoded_volume(const memory_file_t *out, contract_volume_t *encoded)
{
	contract_volume_init(encoded, out->length, 0);
	bcopy(out->bytes, contract_volume_bytes(encoded), (size_t)out->length);
}

/* Produce the encoded image of a mode: a wrap, or the independent
 * materializer for a layer this build only reads.
 */
static void
layer_encode(const contract_layer_profile_t *profile, const layer_mode_t *mode,
    contract_volume_t *payload, contract_volume_t *encoded)
{
	fixture_t fixture;
	memory_file_t out;
	fsimg_error_t error;

	contract_phase("encode", 0);
	if ((mode->capabilities & FSIMG_CAP_WRITE) == 0) {
		CONTRACT_CHECK(profile->materialize != NULL);
		profile->materialize(payload, encoded);
		return;
	}
	bzero(&out, sizeof(out));
	fixture_open(&fixture, 0);
	CONTRACT_STATUS(layer_wrap(&fixture, profile, mode, payload, 0, &out,
	    &error), FSIMG_OK, &error);
	fixture_close(&fixture);
	encoded_volume(&out, encoded);
	memory_free(&out);
}

/* Bytes of a volume in [start, end) are all zero. */
static bool
zero_between(contract_volume_t *volume, uint64_t start, uint64_t end)
{
	const unsigned char *bytes = contract_volume_bytes(volume);

	CONTRACT_CHECK(start <= end && end <= volume->length);
	for (uint64_t i = start; i < end; i++) {
		if (bytes[i] != 0)
			return (false);
	}
	return (true);
}

/*
 * The family assertions on a successful read. A scheme reports its name,
 * sector size and one used partition holding the payload at a sector
 * boundary, with zero between its primary tables and the partition. A
 * container or compression reports itself and a disk of exactly the
 * payload's length; verification covers the payload and is complete
 * unless the profile names fields it cannot verify.
 */
static void
check_family(const contract_layer_profile_t *profile, const layer_mode_t *mode,
    contract_volume_t *encoded, const contract_volume_t *payload,
    const layer_read_t *read)
{
	uint64_t tables;

	if (profile->kind == FSIMG_LAYER_SCHEME) {
		CONTRACT_CHECK(strcmp(read->info.scheme, profile->name) == 0);
		CONTRACT_CHECK(strcmp(read->report.scheme, profile->name) == 0);
		CONTRACT_CHECK(read->report.partitions == 1);
		CONTRACT_CHECK(read->partition.index == 1 && read->partition.used);
		CONTRACT_CHECK(read->partition.claim == FSIMG_CLAIM_FORMAT);
		CONTRACT_CHECK(strcmp(read->partition.format, "fat12") == 0);
		CONTRACT_CHECK(read->partition.length >= payload->length);
		CONTRACT_CHECK(read->info.sector_bytes != 0 &&
		    read->partition.offset % read->info.sector_bytes == 0);
		if (strcmp(profile->name, "gpt") == 0) {
			CONTRACT_CHECK(read->info.sector_bytes ==
			    mode->gpt_sector_bytes);
			/* Protective MBR, header, then 16 KiB of entries. */
			tables = 2 * (uint64_t)mode->gpt_sector_bytes + 16 * KIB;
		} else {
			CONTRACT_CHECK(read->info.sector_bytes == 512);
			tables = 512;
		}
		CONTRACT_CHECK(read->partition.offset >= tables);
		CONTRACT_CHECK(zero_between(encoded, tables,
		    read->partition.offset));
		CONTRACT_CHECK(read->report.complete);
		return;
	}
	CONTRACT_CHECK(read->info.disk_bytes == payload->length);
	if (profile->kind == FSIMG_LAYER_CONTAINER) {
		CONTRACT_CHECK(strcmp(read->info.container, profile->name) == 0);
		CONTRACT_CHECK(strcmp(read->report.container, profile->name) == 0);
		CONTRACT_CHECK(read->info.codecs[0] != '\0');
	} else {
		CONTRACT_CHECK(strcmp(read->info.compression, profile->name) == 0);
		CONTRACT_CHECK(strcmp(read->report.compression,
		    profile->name) == 0);
	}
	if (profile->unverified != NULL) {
		CONTRACT_CHECK(strcmp(read->report.unverified,
		    profile->unverified) == 0);
		CONTRACT_CHECK(!read->report.complete);
	} else {
		CONTRACT_CHECK(read->report.unverified[0] == '\0');
		CONTRACT_CHECK(read->report.complete);
	}
}

/*
 * Scenario: read and decode. The encoded image of each mode opens with
 * the profile's stack forced, yields the payload's bytes exactly through
 * a bounded slice, and verifies with its family's report; every access to
 * the encoded input stays inside it and resources return to the baseline.
 */
static void
layer_decode(const contract_layer_profile_t *profile, const layer_mode_t *mode,
    const char *case_id, contract_volume_t *payload)
{
	fixture_t fixture;
	contract_volume_t encoded;
	layer_read_t read;
	fsimg_error_t error;

	if (!contract_begin(case_id, "decode"))
		return;
	layer_encode(profile, mode, payload, &encoded);
	contract_phase("read", 0);
	fixture_open(&fixture, 0);
	CONTRACT_STATUS(layer_read(&fixture, profile, &encoded, payload, 0, &read,
	    &error), FSIMG_OK, &error);
	check_family(profile, mode, &encoded, payload, &read);
	CONTRACT_CHECK(contract_volume_guards_intact(&encoded));
	fixture_close(&fixture);
	contract_volume_free(&encoded);
	contract_end();
}

/*
 * Scenario: the spool budget. A compressed input, or a container that
 * decodes its disk (hbcifs), fills the spool, whose logical bytes are
 * reserved against spool_bytes before each step,
 * so a budget below the decoded disk is refused naming the spool, and the
 * spool's charge is released with the image.
 */
static void
layer_spool_limit(const contract_layer_profile_t *profile,
    const layer_mode_t *mode, const char *case_id, contract_volume_t *payload)
{
	fixture_t fixture;
	contract_volume_t encoded;
	layer_read_t read;
	fsimg_error_t error;

	if (!contract_begin(case_id, "spool-limit"))
		return;
	layer_encode(profile, mode, payload, &encoded);
	contract_phase("read", 0);
	fixture_open(&fixture, payload->length / 2);
	CONTRACT_STATUS(layer_read(&fixture, profile, &encoded, payload, 0,
	    &read, &error), FSIMG_ELIMIT, &error);
	CONTRACT_CHECK(error.resource == FSIMG_RESOURCE_SPOOL);
	CONTRACT_CHECK(error.limit == payload->length / 2 &&
	    error.observed > error.limit);
	fixture_close(&fixture);
	contract_volume_free(&encoded);
	contract_end();
}

/*
 * Scenario: read transport faults. A failing read of the encoded input,
 * first, midway or last, is FSIMG_EIO from whichever operation meets it,
 * never a malformed-image result.
 */
static void
layer_transport(const contract_layer_profile_t *profile,
    const layer_mode_t *mode, const char *case_id, contract_volume_t *payload)
{
	fixture_t fixture;
	contract_volume_t encoded;
	layer_read_t read;
	fsimg_error_t error;
	uint64_t reads, points[3];

	if (!contract_begin(case_id, "transport"))
		return;
	layer_encode(profile, mode, payload, &encoded);
	fixture_open(&fixture, 0);
	CONTRACT_STATUS(layer_read(&fixture, profile, &encoded, payload, 0, &read,
	    &error), FSIMG_OK, &error);
	reads = encoded.reads;
	fixture_close(&fixture);
	/* A spooled input can arrive in a single read; equal points repeat. */
	CONTRACT_CHECK(reads >= 1);
	points[0] = 1;
	points[1] = (reads + 1) / 2;
	points[2] = reads;
	for (size_t i = 0; i < 3; i++) {
		if (i > 0 && points[i] == points[i - 1])
			continue;
		contract_phase("read failure", points[i]);
		contract_volume_reset(&encoded);
		encoded.fail_read = points[i];
		fixture_open(&fixture, 0);
		CONTRACT_STATUS(layer_read(&fixture, profile, &encoded, payload,
		    0, &read, &error), FSIMG_EIO, &error);
		CONTRACT_CHECK(encoded.fault_fired);
		fixture_close(&fixture);
	}
	contract_volume_free(&encoded);
	contract_end();
}

/*
 * Scenario: allocation failure across the read operations. Each fired
 * position is FSIMG_ENOMEM, and the first unreached position is the final
 * run, whose every byte and report layer_read and check_family check.
 */
static void
layer_read_allocation(const contract_layer_profile_t *profile,
    const layer_mode_t *mode, const char *case_id, contract_volume_t *payload)
{
	fixture_t fixture;
	contract_volume_t encoded;
	layer_read_t read;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t position;
	bool fired;

	if (!contract_begin(case_id, "read-allocation"))
		return;
	layer_encode(profile, mode, payload, &encoded);
	for (position = 1; position <= CONTRACT_SWEEP_MAX; position++) {
		contract_phase("read", position);
		fixture_open(&fixture, 0);
		status = layer_read(&fixture, profile, &encoded, payload,
		    position, &read, &error);
		fired = contract_alloc_fired(fixture.ctx);
		contract_arm_alloc(fixture.ctx, 0);
		fixture_close(&fixture);
		if (!fired) {
			CONTRACT_STATUS(status, FSIMG_OK, &error);
			check_family(profile, mode, &encoded, payload, &read);
			break;
		}
		/* A fired fault fails; see writer_sweep() on fallbacks. */
		CONTRACT_STATUS(status, FSIMG_ENOMEM, &error);
	}
	CONTRACT_CHECK(position > 1 && position <= CONTRACT_SWEEP_MAX);
	contract_volume_free(&encoded);
	contract_end();
}

/*
 * Scenario: writing. A wrap is reproducible byte for byte and reads back
 * as the payload; a sink write failing first or midway is FSIMG_EIO with
 * an incomplete output; and each allocation failure in the wrap is
 * FSIMG_ENOMEM with every charge released.
 */
static void
layer_write(const contract_layer_profile_t *profile, const layer_mode_t *mode,
    const char *case_id, contract_volume_t *payload)
{
	fixture_t fixture;
	memory_file_t first, out;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t writes, position, points[2];
	bool fired;

	if (!contract_begin(case_id, "write"))
		return;
	contract_phase("reproducible", 0);
	bzero(&first, sizeof(first));
	bzero(&out, sizeof(out));
	fixture_open(&fixture, 0);
	CONTRACT_STATUS(layer_wrap(&fixture, profile, mode, payload, 0, &first,
	    &error), FSIMG_OK, &error);
	writes = first.writes;
	CONTRACT_STATUS(layer_wrap(&fixture, profile, mode, payload, 0, &out,
	    &error), FSIMG_OK, &error);
	CONTRACT_CHECK(out.length == first.length &&
	    bcmp(out.bytes, first.bytes, (size_t)first.length) == 0);
	CONTRACT_CHECK(contract_volume_guards_intact(payload));
	memory_free(&out);
	fixture_close(&fixture);
	CONTRACT_CHECK(writes >= 1);

	points[0] = 1;
	points[1] = writes / 2 + 1;
	for (size_t i = 0; i < 2; i++) {
		contract_phase("sink failure", points[i]);
		fixture_open(&fixture, 0);
		bzero(&out, sizeof(out));
		out.fail_write = points[i];
		status = layer_wrap(&fixture, profile, mode, payload, 0, &out,
		    &error);
		CONTRACT_STATUS(status, FSIMG_EIO, &error);
		CONTRACT_CHECK(out.fault_fired);
		CONTRACT_CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		memory_free(&out);
		fixture_close(&fixture);
	}

	for (position = 1; position <= CONTRACT_SWEEP_MAX; position++) {
		contract_phase("wrap allocation", position);
		fixture_open(&fixture, 0);
		bzero(&out, sizeof(out));
		status = layer_wrap(&fixture, profile, mode, payload, position,
		    &out, &error);
		fired = contract_alloc_fired(fixture.ctx);
		contract_arm_alloc(fixture.ctx, 0);
		fixture_close(&fixture);
		if (!fired) {
			CONTRACT_STATUS(status, FSIMG_OK, &error);
			CONTRACT_CHECK(out.length == first.length &&
			    bcmp(out.bytes, first.bytes,
			    (size_t)first.length) == 0);
			memory_free(&out);
			break;
		}
		CONTRACT_STATUS(status, FSIMG_ENOMEM, &error);
		memory_free(&out);
	}
	CONTRACT_CHECK(position > 1 && position <= CONTRACT_SWEEP_MAX);
	memory_free(&first);
	contract_end();
}

/*
 * Scenario: a layer this build registers but neither reads nor writes is
 * refused by the public API: a forced read names it and fails before any
 * byte is decoded, and so does a write naming it.
 */
static void
layer_unsupported(const contract_layer_profile_t *profile,
    contract_volume_t *payload)
{
	fixture_t fixture;
	fsimg_image_options_t options;
	fsimg_write_options_t write;
	fsimg_writer_t *writer = NULL;
	fsimg_image_t *image = NULL;
	fsimg_io_t *backing, *input;
	fsimg_error_t error;

	if (!contract_begin(profile->name, "unsupported"))
		return;
	CONTRACT_CHECK(profile->kind == FSIMG_LAYER_COMPRESSION);
	fixture_open(&fixture, 0);
	contract_phase("forced read", 0);
	contract_volume_reset(payload);
	input = contract_volume_open(fixture.ctx, payload, false, &backing);
	fsimg_image_options_init(&options);
	snprintf(options.compression, sizeof(options.compression), "%s",
	    profile->name);
	CONTRACT_STATUS(fsimg_image_open(fixture.ctx, input, &options, &image,
	    &error), FSIMG_EUNSUPPORTED, &error);
	CONTRACT_CHECK(image == NULL);
	CONTRACT_CHECK(strcmp(error.path, profile->name) == 0);
	CONTRACT_CHECK(payload->reads == 0);
	contract_volume_close(input, backing);

	contract_phase("write", 0);
	fsimg_write_options_init(&write);
	snprintf(write.compression, sizeof(write.compression), "%s",
	    profile->name);
	CONTRACT_STATUS(fsimg_writer_new(fixture.ctx, "fat12", &write, &writer,
	    &error), FSIMG_EUNSUPPORTED, &error);
	CONTRACT_CHECK(writer == NULL);
	CONTRACT_CHECK(strcmp(error.path, profile->name) == 0);
	fixture_close(&fixture);
	contract_end();
}

/*
 * Scenario: a read-only container refuses creation, naming itself, before
 * any output exists.
 */
static void
layer_write_refused(const contract_layer_profile_t *profile)
{
	fixture_t fixture;
	fsimg_write_options_t options;
	fsimg_writer_t *writer = NULL;
	fsimg_error_t error;

	if (!contract_begin(profile->name, "write-refused"))
		return;
	fixture_open(&fixture, 0);
	fsimg_write_options_init(&options);
	CONTRACT_STATUS(fsimg_writer_new(fixture.ctx, profile->stack, &options,
	    &writer, &error), FSIMG_EUNSUPPORTED, &error);
	CONTRACT_CHECK(writer == NULL);
	CONTRACT_CHECK(strstr(error.path, profile->name) != NULL ||
	    strstr(error.message, profile->name) != NULL);
	fixture_close(&fixture);
	contract_end();
}

/*
 * Registry reconciliation in both directions: each registered layer has
 * exactly one profile with its kind, carried filesystem and capabilities;
 * each profile names a registered layer; each mode ID is unique within
 * its profile and asks only for operations the layer advertises, and a
 * layer advertising any operation has at least one mode witnessing it.
 */
static void
check_registry(void)
{
	fsimg_layer_info_t info;
	fsimg_error_t error;
	size_t cursor = 0, matches;
	unsigned witnessed;
	fsimg_status_t status;

	if (!contract_begin("registry", "reconcile"))
		return;
	while ((status = fsimg_layer_next(&cursor, &info, &error)) == FSIMG_OK) {
		matches = 0;
		for (size_t i = 0; i < COUNT(layer_profiles); i++) {
			const contract_layer_profile_t *p = &layer_profiles[i];

			if (strcmp(p->name, info.name) != 0)
				continue;
			matches++;
			CONTRACT_CHECK(p->kind == info.kind);
			CONTRACT_CHECK(strcmp(p->carries, info.carries) == 0);
			if (p->expected_capabilities != info.capabilities)
				fprintf(stderr, "layer %s advertises 0x%x\n",
				    info.name, info.capabilities);
			CONTRACT_CHECK(p->expected_capabilities ==
			    info.capabilities);
		}
		if (matches != 1)
			fprintf(stderr, "layer %s has %zu profiles\n", info.name,
			    matches);
		CONTRACT_CHECK(matches == 1);
	}
	CONTRACT_STATUS(status, FSIMG_DONE, &error);

	for (size_t i = 0; i < COUNT(layer_profiles); i++) {
		const contract_layer_profile_t *p = &layer_profiles[i];
		bool found = false;

		cursor = 0;
		while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK)
			found = found || strcmp(info.name, p->name) == 0;
		CONTRACT_CHECK(found);
		witnessed = 0;
		for (size_t m = 0; m < p->mode_count; m++) {
			for (size_t n = 0; n < m; n++)
				CONTRACT_CHECK(strcmp(p->modes[n].id,
				    p->modes[m].id) != 0);
			CONTRACT_CHECK((p->modes[m].capabilities &
			    ~p->expected_capabilities) == 0);
			witnessed |= p->modes[m].capabilities;
		}
		CONTRACT_CHECK(witnessed == p->expected_capabilities);
		/* A read-only mode needs an independent materializer. */
		for (size_t m = 0; m < p->mode_count; m++)
			CONTRACT_CHECK((p->modes[m].capabilities &
			    FSIMG_CAP_WRITE) != 0 || p->materialize != NULL);
	}
	contract_end();
}

/*
 * Composition seams, each crossing one ownership boundary with an exact
 * expected result rather than multiplying every layer by every other.
 */

/* A writer's output through its raw intermediate into a memory sink. */
static fsimg_status_t
writer_output(fixture_t *fixture, const char *stack, const char *compression,
    memory_file_t *out, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_source_ops_t source_ops;
	fsimg_writer_t *writer;
	fsimg_entry_t entry = { .path = "/one", .kind = FSIMG_ENTRY_FILE,
	    .size = PAYLOAD_FILE_BYTES };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
	fsimg_sink_t sink;
	fsimg_status_t status;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	if (compression != NULL)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    compression);
	status = fsimg_writer_new(fixture->ctx, stack, &options, &writer, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = pattern_validate;
	source_ops.read_at = pattern_read;
	status = fsimg_writer_set_sources(writer, &source_ops, NULL, error);
	if (status == FSIMG_OK)
		status = fsimg_writer_add(writer, &entry, &source, error);
	if (status == FSIMG_OK) {
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = memory_io(fixture->ctx, out);
		sink.raw_fd = spool_new(fixture);
		status = fsimg_writer_output(writer, &sink, error);
		fsimg_io_free(sink.io);
	}
	fsimg_writer_free(writer);
	return (status);
}

/* Open an encoded image automatically, select partition 1 (or the disk)
 * and read its one file through the filesystem reader.
 */
static void
check_composed(fixture_t *fixture, memory_file_t *out, bool partitioned,
    const char *compression)
{
	fsimg_image_options_t options;
	fsimg_selector_t selector = { .kind = partitioned ?
	    FSIMG_SELECT_INDEX : FSIMG_SELECT_DISK, .index = 1 };
	fsimg_image_t *image;
	fsimg_io_t *input, *volume;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	unsigned char buffer[4096];
	uint64_t offset;
	size_t length;
	fsimg_error_t error;

	input = memory_io(fixture->ctx, out);
	fsimg_image_options_init(&options);
	options.spool_fd = spool_new(fixture);
	CONTRACT_STATUS(fsimg_image_open(fixture->ctx, input, &options, &image,
	    &error), FSIMG_OK, &error);
	{
		fsimg_image_info_t info;

		CONTRACT_STATUS(fsimg_image_get_info(image, &info, &error),
		    FSIMG_OK, &error);
		CONTRACT_CHECK(strcmp(info.compression, compression == NULL ?
		    "" : compression) == 0);
	}
	CONTRACT_STATUS(fsimg_image_select(image, &selector, &volume, &error),
	    FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_reader_open(fixture->ctx, volume, NULL, &reader,
	    &error), FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_reader_next(reader, &entry, &error), FSIMG_OK,
	    &error);
	CONTRACT_CHECK(strcmp(entry->path, "/one") == 0 &&
	    entry->size == PAYLOAD_FILE_BYTES);
	for (offset = 0; offset < entry->size; offset += length) {
		CONTRACT_STATUS(fsimg_reader_read_at(reader, offset, buffer,
		    sizeof(buffer), &length, &error), FSIMG_OK, &error);
		CONTRACT_CHECK(length > 0);
		for (size_t i = 0; i < length; i++)
			CONTRACT_CHECK(buffer[i] == contract_pattern(1,
			    offset + i));
	}
	CONTRACT_STATUS(fsimg_reader_next(reader, &entry, &error), FSIMG_DONE,
	    &error);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
	fsimg_image_free(image);
	fsimg_io_free(input);
}

static void
seams(void)
{
	static const struct {
		const char	*id;
		const char	*stack;
		const char	*compression;
		bool		 partitioned;
		fsimg_status_t	 expected;
		const char	*named;	/* the layer a refusal names */
	} cases[] = {
		/* A scheme's slice translated through a container's disk. */
		{ .id = "gpt-in-dmg", .stack = "fat12/gpt/dmg",
		    .partitioned = true, .expected = FSIMG_OK },
		/* gzip around a container's encoding. */
		{ .id = "dmg-in-gzip", .stack = "fat12/dmg",
		    .compression = "gzip", .expected = FSIMG_OK },
		{ .id = "mbr-in-gzip", .stack = "fat12/mbr",
		    .compression = "gzip", .partitioned = true,
		    .expected = FSIMG_OK },
		/* An exact volume refuses a layer that pads its slot. */
		{ .id = "ifs-exact-in-gpt", .stack = "ifs/gpt",
		    .expected = FSIMG_EUNSUPPORTED, .named = "gpt" },
		{ .id = "ifs-exact-in-vhd", .stack = "ifs/vhd",
		    .expected = FSIMG_EUNSUPPORTED, .named = "vhd" },
		/* Compression never pads, so the exact volume is written. */
		{ .id = "ifs-exact-in-gzip", .stack = "ifs",
		    .compression = "gzip", .expected = FSIMG_OK },
	};
	fixture_t fixture;
	memory_file_t out;
	fsimg_error_t error;
	fsimg_status_t status;

	for (size_t i = 0; i < COUNT(cases); i++) {
		if (!contract_begin("seam", cases[i].id))
			continue;
		fixture_open(&fixture, 0);
		bzero(&out, sizeof(out));
		contract_phase("output", 0);
		status = writer_output(&fixture, cases[i].stack,
		    cases[i].compression, &out, &error);
		CONTRACT_STATUS(status, cases[i].expected, &error);
		if (status == FSIMG_OK) {
			contract_phase("read back", 0);
			check_composed(&fixture, &out, cases[i].partitioned,
			    cases[i].compression);
		} else {
			CONTRACT_CHECK(out.writes == 0 && out.length == 0);
			CONTRACT_CHECK(strstr(error.path, cases[i].named) !=
			    NULL || strstr(error.message, cases[i].named) !=
			    NULL);
		}
		memory_free(&out);
		fixture_close(&fixture);
		contract_end();
	}

	/* A failure during the final transcode leaves the sink incomplete. */
	if (contract_begin("seam", "final-transcode-failure")) {
		uint64_t writes;

		fixture_open(&fixture, 0);
		bzero(&out, sizeof(out));
		CONTRACT_STATUS(writer_output(&fixture, "fat12/gpt/dmg", NULL,
		    &out, &error), FSIMG_OK, &error);
		writes = out.writes;
		memory_free(&out);
		CONTRACT_CHECK(writes >= 1);
		contract_phase("last transcode write", writes);
		out.fail_write = writes;
		CONTRACT_STATUS(writer_output(&fixture, "fat12/gpt/dmg", NULL, &out,
		    &error), FSIMG_EIO, &error);
		CONTRACT_CHECK(out.fault_fired);
		CONTRACT_CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		memory_free(&out);
		fixture_close(&fixture);
		contract_end();
	}

	/* A carried-filesystem container around anything else is refused
	 * naming the container, before any byte is read.
	 */
	if (contract_begin("seam", "carried-filesystem-required")) {
		fsimg_image_options_t options;
		fsimg_image_t *image = NULL;
		contract_volume_t volume;
		fsimg_io_t *backing, *input;

		contract_volume_init(&volume, 64 * KIB, 0);
		fixture_open(&fixture, 0);
		input = contract_volume_open(fixture.ctx, &volume, false,
		    &backing);
		fsimg_image_options_init(&options);
		CONTRACT_STATUS(fsimg_stack_parse("fat12/ifsboot", &options.stack,
		    &error), FSIMG_OK, &error);
		CONTRACT_STATUS(fsimg_image_open(fixture.ctx, input, &options,
		    &image, &error), FSIMG_EINVAL, &error);
		CONTRACT_CHECK(strcmp(error.path, "ifsboot") == 0);
		CONTRACT_CHECK(volume.reads == 0);
		contract_volume_close(input, backing);
		fixture_close(&fixture);
		contract_volume_free(&volume);
		contract_end();
	}
}

static void
run_profile(const contract_layer_profile_t *profile,
    contract_volume_t *payloads)
{
	contract_volume_t *payload = &payloads[profile->payload];
	char case_id[64];

	if (profile->expected_capabilities == 0) {
		layer_unsupported(profile, payload);
		return;
	}
	if ((profile->expected_capabilities & FSIMG_CAP_WRITE) == 0)
		layer_write_refused(profile);
	for (size_t m = 0; m < profile->mode_count; m++) {
		const layer_mode_t *mode = &profile->modes[m];

		snprintf(case_id, sizeof(case_id), "%s-%s", profile->name,
		    mode->id);
		if ((mode->capabilities & FSIMG_CAP_READ) != 0) {
			layer_decode(profile, mode, case_id, payload);
			if (profile->spooled)
				layer_spool_limit(profile, mode, case_id,
				    payload);
			else
				contract_not_applicable(case_id, "spool-limit",
				    "the layer reads its input in place");
			layer_transport(profile, mode, case_id, payload);
			layer_read_allocation(profile, mode, case_id, payload);
		}
		if ((mode->capabilities & FSIMG_CAP_WRITE) != 0)
			layer_write(profile, mode, case_id, payload);
		else
			contract_not_applicable(case_id, "write",
			    "mode is read-only");
	}
}

int
main(int argc, char **argv)
{
	contract_volume_t payloads[2];

	contract_init(argc, argv);
	for (size_t i = 0; i < sizeof(startup_code); i++)
		startup_code[i] = contract_pattern(99, i);
	check_registry();
	payload_build(PAYLOAD_FAT12, &payloads[PAYLOAD_FAT12]);
	payload_build(PAYLOAD_IFS, &payloads[PAYLOAD_IFS]);
	for (size_t i = 0; i < COUNT(layer_profiles); i++)
		run_profile(&layer_profiles[i], payloads);
	seams();
	contract_volume_free(&payloads[PAYLOAD_FAT12]);
	contract_volume_free(&payloads[PAYLOAD_IFS]);
	return (contract_finish("layer contracts"));
}
