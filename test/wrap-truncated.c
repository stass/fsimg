/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"
#include "test.h"

#define MIB (UINT64_C(1024) * 1024)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Inputs borrow their bytes; only writer and wrap sinks own allocations. */
typedef struct memory {
	unsigned char *bytes;
	uint64_t length;
	uint64_t writes;
} memory_t;

typedef struct volume_case {
	const char *format;
	uint64_t bytes;
	uint32_t unit;
	uint64_t payload_bytes;
	const char *gpt_type;
} volume_case_t;

typedef struct wrap_stack {
	const char *suffix;
	const char *compression;
	const char *gpt_type;
} wrap_stack_t;

static const volume_case_t volumes[] = {
	{ .format = "fat12", .bytes = MIB, .unit = 512 },
	{ .format = "fat16", .bytes = 4 * MIB, .unit = 512 },
	{ .format = "fat32", .bytes = 34 * MIB, .unit = 512 },
	{ .format = "exfat", .bytes = 2 * MIB, .unit = 512 },
	{ .format = "qnx4", .bytes = MIB, .gpt_type = "linux" },
	{ .format = "qnx6", .bytes = MIB + 16384, .unit = 512 },
	{ .format = "ifs" },
	{ .format = "iso9660", .bytes = MIB, .gpt_type = "linux" }
};

static const wrap_stack_t stacks[] = {
	{ .suffix = "" },
	{ .suffix = "/gpt" },
	{ .suffix = "/vhd" },
	{ .suffix = "", .compression = "gzip" }
};

/* These cases exercise both a bare volume and a partition scheme. */
static const wrap_stack_t bare_and_scheme[] = {
	{ .suffix = "" },
	{ .suffix = "/gpt" }
};

static const wrap_stack_t bare = { .suffix = "" };

typedef struct wrap_expectation {
	fsimg_status_t status;
	const char *error_path;
	const char *message;
	/* Driver wording is its own; check only the relevant diagnosis. */
	const char *driver_message_contains;
} wrap_expectation_t;

static unsigned failures;

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
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	bcopy(buffer, memory->bytes + offset, length);
	memory->writes++;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;
	unsigned char *bytes;

	CHECK(length <= SIZE_MAX);
	bytes = realloc(memory->bytes, length == 0 ? 1 : (size_t)length);
	CHECK(bytes != NULL);
	if (length > memory->length)
		memset(bytes + memory->length, 0xa5,
		    (size_t)(length - memory->length));
	memory->bytes = bytes;
	memory->length = length;
	return (FSIMG_OK);
}

static fsimg_io_t *
memory_io(fsimg_ctx_t *ctx, memory_t *memory)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, memory, &io, NULL) == FSIMG_OK);
	return (io);
}

/* A file gives exact-size ImageFS enough payload to cut after its table. */
static memory_t
build_volume(fsimg_ctx_t *ctx, const volume_case_t *fixture)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_io_t *io;
	fsimg_error_t error;
	fsimg_status_t status;
	fsimg_entry_t entry = { .path = "/payload", .kind = FSIMG_ENTRY_FILE,
	    .size = fixture->payload_bytes == 0 ? 8192 : fixture->payload_bytes };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
	memory_t volume = { 0 };

	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_ALLOW_LOSS;
	options.epoch = 1700000000;
	options.volume_bytes = fixture->bytes;
	options.allocation_unit_bytes = fixture->unit;
	CHECK(fsimg_writer_new(ctx, fixture->format, &options, &writer,
	    &error) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &entry, &source, &error) == FSIMG_OK);
	io = memory_io(ctx, &volume);
	status = fsimg_writer_emit(writer, io, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s fixture: %s\n", fixture->format, error.message);
	CHECK(status == FSIMG_OK);
	fsimg_io_free(io);
	fsimg_writer_free(writer);
	CHECK(fixture->bytes == 0 || volume.length == fixture->bytes);
	return (volume);
}

/* The oracle is the writer's intact byte length minus the chosen cut,
 * independent of the reader's geometry and missing-byte calculation.
 */
static void
check_fixture(fsimg_ctx_t *ctx, const char *format, memory_t *input,
    uint64_t intact_length)
{
	fsimg_read_options_t options;
	fsimg_reader_t *reader;
	fsimg_volume_info_t info;
	fsimg_error_t error;
	fsimg_status_t status;
	fsimg_io_t *io = memory_io(ctx, input);

	fsimg_read_options_init(&options);
	options.format = format;
	status = fsimg_reader_open(ctx, io, &options, &reader, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "%s fixture open: %s\n", format, error.message);
	CHECK(status == FSIMG_OK);
	CHECK(fsimg_reader_get_info(reader, &info, &error) == FSIMG_OK);
	CHECK(info.volume_bytes == intact_length);
	CHECK(info.missing_bytes == intact_length - input->length);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
}

/* Read the bare wrap back as a disk and compare every volume byte. */
static void
check_disk(fsimg_ctx_t *ctx, memory_t *output, const memory_t *volume)
{
	fsimg_image_t *image;
	fsimg_image_options_t options;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_io_t *input = memory_io(ctx, output), *disk;
	unsigned char buffer[8192];
	uint64_t length, offset;
	size_t got, count;

	fsimg_image_options_init(&options);
	CHECK(fsimg_image_open(ctx, input, &options, &image, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(disk, &length, NULL) == FSIMG_OK);
	CHECK(length == volume->length);
	for (offset = 0; offset < length; offset += got) {
		count = length - offset < sizeof(buffer) ?
		    (size_t)(length - offset) : sizeof(buffer);
		CHECK(fsimg_io_read_at(disk, offset, buffer, count, &got, NULL) ==
		    FSIMG_OK);
		CHECK(got == count);
		CHECK(bcmp(buffer, volume->bytes + offset, got) == 0);
	}
	fsimg_io_free(disk);
	fsimg_image_free(image);
	fsimg_io_free(input);
}

/* Any accepted startup code length and addresses suffice: wrap validates
 * them, but these tests do not depend on their values.
 */
static void
setup_ifsboot(fsimg_ctx_t *ctx, memory_t *startup,
    fsimg_layer_options_t *layers, fsimg_layer_inputs_t *inputs)
{
	for (size_t i = 0; i < startup->length; i++)
		startup->bytes[i] = (unsigned char)(i * 37 + 11);
	inputs->ifsboot_startup = memory_io(ctx, startup);
	layers->ifsboot_machine_set = true;
	layers->ifsboot_machine = 0;
	layers->ifsboot_virtual_set = true;
	layers->ifsboot_virtual = true;
	layers->ifsboot_startup_vaddr_set = true;
	layers->ifsboot_startup_vaddr = 0x00401234;
	layers->ifsboot_paddr_bias_set = true;
	layers->ifsboot_paddr_bias = 0;
	layers->ifsboot_image_paddr_set = true;
	layers->ifsboot_image_paddr = 0x00400440;
	layers->ifsboot_ram_paddr_set = true;
	layers->ifsboot_ram_paddr = 0x00400000;
	layers->ifsboot_imagefs_paddr_set = true;
	layers->ifsboot_imagefs_paddr = 0xfffffffc;
	layers->ifsboot_compress = FSIMG_IFSBOOT_NONE;
}

static void
check_wrap(fsimg_ctx_t *ctx, const char *format, memory_t *volume,
    const wrap_stack_t *stack, const char *scenario,
    wrap_expectation_t expected)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs = { 0 };
	fsimg_sink_t sink;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	fsimg_io_t *input, *output;
	memory_t out = { 0 };
	unsigned char code[4001];
	memory_t startup = { .bytes = code, .length = sizeof(code) };
	char stack_name[64];
	bool path_ok, message_ok = true, sink_ok;

	fsimg_layer_options_init(&layers);
	if (stack->gpt_type != NULL && strcmp(stack->suffix, "/gpt") == 0)
		snprintf(layers.gpt_type, sizeof(layers.gpt_type), "%s",
		    stack->gpt_type);
	if (strcmp(stack->suffix, "/ifsboot") == 0)
		setup_ifsboot(ctx, &startup, &layers, &inputs);
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	options.layers = &layers;
	options.inputs = &inputs;
	if (stack->compression != NULL)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    stack->compression);
	snprintf(stack_name, sizeof(stack_name), "%s%s", format, stack->suffix);
	input = memory_io(ctx, volume);
	output = memory_io(ctx, &out);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;

	status = fsimg_image_wrap(ctx, input, stack_name, &options, &sink, &error);
	path_ok = expected.error_path == NULL ||
	    strcmp(error.path, expected.error_path) == 0;
	if (expected.message != NULL)
		message_ok = strcmp(error.message, expected.message) == 0;
	if (expected.driver_message_contains != NULL)
		message_ok = message_ok && strstr(error.message,
		    expected.driver_message_contains) != NULL;
	sink_ok = expected.status == FSIMG_OK || (out.writes == 0 && out.length == 0);
	if (status != expected.status || !path_ok || !message_ok || !sink_ok) {
		fprintf(stderr, "%s %s%s: expected status=%d, path=%s "
		    "message=%s sink=%s; observed status=%d path='%s' "
		    "message='%s' writes=%llu bytes=%llu\n", scenario, stack_name,
		    stack->compression == NULL ? "" : "+gzip", expected.status,
		    path_ok ? "ok" : "FAIL", message_ok ? "ok" : "FAIL",
		    sink_ok ? "ok" : "FAIL", status, error.path, error.message,
		    (unsigned long long)out.writes, (unsigned long long)out.length);
		failures++;
	}
	if (expected.status == FSIMG_OK && status == FSIMG_OK &&
	    stack->suffix[0] == '\0' && stack->compression == NULL)
		check_disk(ctx, &out, volume);

	fsimg_io_free(inputs.ifsboot_startup);
	fsimg_io_free(output);
	fsimg_io_free(input);
	free(out.bytes);
}

static void
run_exfat_cases_corrupting_volume(fsimg_ctx_t *ctx, const volume_case_t *fixture,
    memory_t *volume)
{
	memory_t cut = *volume;
	fsimg_io_t *io;
	fsimg_reader_t *reader = NULL;
	uint64_t intact_length = volume->length;
	char message[128];

	/* 1 MiB is exFAT's volume minimum, checked against the declared size.
	 * The probe reads only the signature, so a shorter input still claims
	 * exFAT. This cut leaves the FAT and root metadata intact.
	 */
	cut.length = 900 * 1024;
	snprintf(message, sizeof(message),
	    "image is truncated: %llu declared bytes are missing",
	    (unsigned long long)(intact_length - cut.length));
	check_fixture(ctx, fixture->format, &cut, intact_length);
	for (size_t i = 0; i < COUNT(bare_and_scheme); i++)
		check_wrap(ctx, fixture->format, &cut, &bare_and_scheme[i], "below-1MiB",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .message = message });

	/* Sector 11 holds the main boot checksum; changing it preserves the
	 * probe's signature claim. Corruption here is not a truncation.
	 */
	volume->bytes[11 * 512] ^= 1;
	io = memory_io(ctx, volume);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_ECORRUPT);
	CHECK(reader == NULL);
	fsimg_io_free(io);
	for (size_t i = 0; i < COUNT(bare_and_scheme); i++)
		check_wrap(ctx, fixture->format, volume, &bare_and_scheme[i],
		    "bad-checksum", (wrap_expectation_t){
		    .status = FSIMG_ECORRUPT, .error_path = "input",
		    .message = "invalid exFAT boot checksum" });
}

static void
run_fat_cases(fsimg_ctx_t *ctx, const volume_case_t *fixture,
    memory_t *volume)
{
	memory_t cut = *volume;

	/* The BPB reserved-sector count locates the first FAT. Keep its first
	 * sector only, cutting inside the tables.
	 */
	cut.length = ((uint64_t)volume->bytes[14] +
	    ((uint64_t)volume->bytes[15] << 8) + 1) * 512;
	CHECK(cut.length < volume->length);
	for (size_t i = 0; i < COUNT(bare_and_scheme); i++)
		check_wrap(ctx, fixture->format, &cut, &bare_and_scheme[i], "tables-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .driver_message_contains = "truncated" });
	if (strcmp(fixture->format, "fat16") == 0) {
		check_wrap(ctx, "fat12", volume, &bare, "fat16-named-fat12",
		    (wrap_expectation_t){ .status = FSIMG_EINVAL,
		    .error_path = "fat12", .message =
		    "the volume does not hold the filesystem the stack names" });
		check_wrap(ctx, "fat12", &cut, &bare, "fat16-tables-cut-named-fat12",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .driver_message_contains = "truncated" });
	}
}

static void
run_qnx4_short_cut(fsimg_ctx_t *ctx)
{
	const volume_case_t fixture = { .format = "qnx4", .payload_bytes = MIB };
	memory_t volume = build_volume(ctx, &fixture);
	memory_t cut = volume;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	/* Pin the documented limit in specs/design-qnx4.md, not desired
	 * behaviour: a cut too short to change ceil(N / 8) proves nothing.
	 * Auto-sizing fills the tail with the file's allocated blocks; a free
	 * tail would be refused by the bitmap padding-bit check instead.
	 */
	cut.length -= 512;
	CHECK((volume.length / 512 + 7) / 8 == (cut.length / 512 + 7) / 8);
	check_wrap(ctx, fixture.format, &cut, &bare, "one-block-cut",
	    (wrap_expectation_t){ .status = FSIMG_OK });
	io = memory_io(ctx, &cut);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, &error) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(reader, &info, &error) == FSIMG_OK);
	CHECK(info.missing_bytes == 0);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_ECORRUPT) {
		fprintf(stderr, "one-block-cut qnx4: verify expected %d, "
		    "observed %d: %s\n", FSIMG_ECORRUPT, status, error.message);
		failures++;
	}
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	free(volume.bytes);
}

static void
run_volume(fsimg_ctx_t *ctx, const volume_case_t *fixture)
{
	const wrap_stack_t boot = { .suffix = "/ifsboot" };
	memory_t volume, cut;
	char message[128];
	/* ImageFS plans refuse partition schemes and padding containers. */
	bool imagefs_requires_exact_size = strcmp(fixture->format, "ifs") == 0;

	volume = build_volume(ctx, fixture);
	check_fixture(ctx, fixture->format, &volume, volume.length);
	cut = volume;
	/* QNX4 derives truncation from bitmap bytes, each covering eight
	 * 512-byte blocks. The MIB fixture's block count is a multiple of eight;
	 * cutting 4096 bytes only makes the bitmap prove the missing count.
	 */
	CHECK(cut.length > 4096);
	cut.length -= 4096;
	snprintf(message, sizeof(message),
	    "image is truncated: %llu declared bytes are missing",
	    (unsigned long long)(volume.length - cut.length));
	check_fixture(ctx, fixture->format, &cut, volume.length);
	for (size_t i = 0; i < COUNT(stacks); i++) {
		wrap_stack_t stack = stacks[i];

		stack.gpt_type = fixture->gpt_type;
		check_wrap(ctx, fixture->format, &cut, &stack, "tail-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .message = message });
		if (!imagefs_requires_exact_size || stacks[i].suffix[0] == '\0')
			check_wrap(ctx, fixture->format, &volume, &stack, "intact",
			    (wrap_expectation_t){ .status = FSIMG_OK });
		else
			check_wrap(ctx, fixture->format, &volume, &stack,
			    "intact", (wrap_expectation_t){
			    .status = FSIMG_EUNSUPPORTED });
	}
	if (imagefs_requires_exact_size) {
		check_wrap(ctx, fixture->format, &cut, &boot, "carried-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .message = message });
		check_wrap(ctx, fixture->format, &volume, &boot, "intact",
		    (wrap_expectation_t){ .status = FSIMG_OK });
	}
	if (strcmp(fixture->format, "exfat") == 0)
		run_exfat_cases_corrupting_volume(ctx, fixture, &volume);
	if (strncmp(fixture->format, "fat", 3) == 0)
		run_fat_cases(ctx, fixture, &volume);
	free(volume.bytes);
}

static void
run_qnx6_unsupported(fsimg_ctx_t *ctx)
{
	unsigned char magic[] = { 0x22, 0x11, 0x19, 0x68 };
	memory_t volume = { .bytes = magic, .length = sizeof(magic) };

	/* Magic at byte zero identifies a bootless or MMI layout. */
	check_wrap(ctx, "qnx6", &volume, &bare, "bootless",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "input" });
}

int
main(void)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < COUNT(volumes); i++)
		run_volume(ctx, &volumes[i]);
	run_qnx4_short_cut(ctx);
	run_qnx6_unsupported(ctx);
	fsimg_ctx_free(ctx);
	return (failures != 0);
}
