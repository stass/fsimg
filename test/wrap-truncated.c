/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "format.h"
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
	/* Zero disables the injected failure beyond this readable prefix. */
	uint64_t read_limit;
} memory_t;

typedef struct volume_case {
	const char *format;
	uint64_t bytes;
	uint32_t unit;
	uint64_t payload_bytes;
	const char *gpt_type;
} volume_case_t;

typedef struct wrap_stack {
	/* Empty or slash-prefixed layers follow the named input filesystem.
	 * Other strings are literal stacks with an empty filesystem slot.
	 */
	const char *layers;
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

/* Literal GPT stacks supply a type even when opening a cut cannot infer one.
 * A fixture's explicit type takes precedence.
 */
static const wrap_stack_t stacks[] = {
	{ .layers = "" },
	{ .layers = "/gpt" },
	{ .layers = "/vhd" },
	{ .layers = "", .compression = "gzip" },
	{ .layers = "dmg" },
	{ .layers = "gpt", .gpt_type = "linux" }
};

/* Exercise named bare/GPT stacks and literal DMG/GPT stacks. */
static const wrap_stack_t bare_and_scheme[] = {
	{ .layers = "" },
	{ .layers = "/gpt" },
	{ .layers = "dmg" },
	{ .layers = "gpt", .gpt_type = "linux" }
};

static const char *schemes[] = { "gpt", "mbr" };

static const wrap_stack_t bare = { .layers = "" };
static const wrap_stack_t dmg = { .layers = "dmg" };
static const wrap_stack_t raw_stacks[] = {
	{ .layers = "dmg" },
	{ .layers = "gpt", .gpt_type = "linux" }
};

typedef struct wrap_expectation {
	fsimg_status_t status;
	const char *partition_type;
	const char *error_path;
	const char *message;
	/* Driver wording is its own; check only the relevant diagnosis. */
	const char *driver_message_contains;
} wrap_expectation_t;

static unsigned failures;

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	if (memory->read_limit != 0 &&
	    (offset > memory->read_limit || length > memory->read_limit - offset)) {
		*transferred = 0;
		if (error != NULL) {
			error->status = FSIMG_EIO;
			snprintf(error->message, sizeof(error->message),
			    "injected read failure beyond probe prefix");
		}
		return (FSIMG_EIO);
	}
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

static void
check_volume_bytes(fsimg_io_t *disk, const memory_t *volume,
    uint64_t expected_length)
{
	unsigned char buffer[8192];
	uint64_t length, offset;
	size_t got, count, payload;

	CHECK(fsimg_io_get_size(disk, &length, NULL) == FSIMG_OK);
	CHECK(length == expected_length);
	for (offset = 0; offset < length; offset += got) {
		count = length - offset < sizeof(buffer) ?
		    (size_t)(length - offset) : sizeof(buffer);
		CHECK(fsimg_io_read_at(disk, offset, buffer, count, &got, NULL) ==
		    FSIMG_OK);
		CHECK(got == count);
		payload = offset >= volume->length ? 0 :
		    (volume->length - offset < got ?
		    (size_t)(volume->length - offset) : got);
		if (payload != 0)
			CHECK(bcmp(buffer, volume->bytes + offset, payload) == 0);
		for (size_t i = payload; i < got; i++)
			CHECK(buffer[i] == 0);
	}
}

/* Select the decoded disk and compare every volume byte. */
static void
check_disk(fsimg_ctx_t *ctx, memory_t *output, const memory_t *volume,
    uint64_t expected_length)
{
	fsimg_image_t *image;
	fsimg_image_options_t options;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_io_t *input = memory_io(ctx, output), *disk;

	fsimg_image_options_init(&options);
	CHECK(fsimg_image_open(ctx, input, &options, &image, NULL) == FSIMG_OK);
	CHECK(fsimg_image_select(image, &selector, &disk, NULL) == FSIMG_OK);
	check_volume_bytes(disk, volume, expected_length);
	fsimg_io_free(disk);
	fsimg_image_free(image);
	fsimg_io_free(input);
}

/* A literal scheme carries exactly one partition with the input bytes. */
static void
check_partition(fsimg_ctx_t *ctx, memory_t *output, const memory_t *volume,
    const char *expected_type)
{
	fsimg_image_t *image;
	fsimg_partition_info_t partition;
	fsimg_image_options_t options;
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_INDEX, .index = 1 };
	fsimg_io_t *input = memory_io(ctx, output), *selected;
	unsigned used = 0;
	fsimg_status_t status;

	fsimg_image_options_init(&options);
	CHECK(fsimg_image_open(ctx, input, &options, &image, NULL) == FSIMG_OK);
	while ((status = fsimg_image_next_partition(image, &partition, NULL)) ==
	    FSIMG_OK) {
		if (!partition.used)
			continue;
		used++;
		CHECK(partition.index == 1);
		if (expected_type != NULL)
			CHECK(strcmp(partition.type, expected_type) == 0);
	}
	CHECK(status == FSIMG_DONE);
	CHECK(used == 1);
	CHECK(fsimg_image_select(image, &selector, &selected, NULL) == FSIMG_OK);
	/* Partitions occupy whole sectors; the remainder must be zero. */
	check_volume_bytes(selected, volume, (volume->length + 511) / 512 * 512);
	fsimg_io_free(selected);
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
check_result(const char *scenario, fsimg_status_t status,
    const fsimg_error_t *error, const memory_t *out,
    wrap_expectation_t expected)
{
	bool path_ok, message_ok = true, sink_ok;

	path_ok = expected.error_path == NULL ||
	    strcmp(error->path, expected.error_path) == 0;
	if (expected.message != NULL)
		message_ok = strcmp(error->message, expected.message) == 0;
	if (expected.driver_message_contains != NULL)
		message_ok = message_ok && strstr(error->message,
		    expected.driver_message_contains) != NULL;
	sink_ok = expected.status == FSIMG_OK || (out->writes == 0 && out->length == 0);
	if (status != expected.status || !path_ok || !message_ok || !sink_ok) {
		fprintf(stderr, "%s: expected status=%d, path=%s "
		    "message=%s sink=%s; observed status=%d path='%s' "
		    "message='%s' writes=%llu bytes=%llu\n", scenario, expected.status,
		    path_ok ? "ok" : "FAIL", message_ok ? "ok" : "FAIL",
		    sink_ok ? "ok" : "FAIL", status, error->path, error->message,
		    (unsigned long long)out->writes, (unsigned long long)out->length);
		failures++;
	}
}

/* input_name supplies the filesystem for named stacks.
 * expected_bytes == NULL checks only status, diagnostics and sink inactivity;
 * callers with a byte fixture also check successful output contents.
 */
static void
check_wrap_io(fsimg_ctx_t *ctx, const char *input_name, fsimg_io_t *input,
    memory_t *expected_bytes, const wrap_stack_t *stack, const char *scenario,
    wrap_expectation_t expected)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs = { 0 };
	fsimg_sink_t sink;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	fsimg_io_t *output;
	memory_t out = { 0 };
	unsigned char code[4001];
	memory_t startup = { .bytes = code, .length = sizeof(code) };
	char stack_name[64], label[192];

	fsimg_layer_options_init(&layers);
	if (stack->layers[0] == '\0' || stack->layers[0] == '/')
		snprintf(stack_name, sizeof(stack_name), "%s%s", input_name,
		    stack->layers);
	else
		snprintf(stack_name, sizeof(stack_name), "%s", stack->layers);
	if (stack->gpt_type != NULL &&
	    (strcmp(stack->layers, "/gpt") == 0 || strcmp(stack_name, "gpt") == 0))
		snprintf(layers.gpt_type, sizeof(layers.gpt_type), "%s",
		    stack->gpt_type);
	if (strcmp(stack->layers, "/ifsboot") == 0)
		setup_ifsboot(ctx, &startup, &layers, &inputs);
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	options.layers = &layers;
	options.inputs = &inputs;
	if (stack->compression != NULL)
		snprintf(options.compression, sizeof(options.compression), "%s",
		    stack->compression);
	output = memory_io(ctx, &out);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;

	status = fsimg_image_wrap(ctx, input, stack_name, &options, &sink, &error);
	snprintf(label, sizeof(label), "%s %s %s%s", scenario, input_name,
	    stack_name, stack->compression == NULL ? "" : "+gzip");
	check_result(label, status, &error, &out, expected);
	if (expected.status == FSIMG_OK && status == FSIMG_OK &&
	    expected_bytes != NULL) {
		if (strcmp(stack_name, "gpt") == 0)
			check_partition(ctx, &out, expected_bytes,
			    expected.partition_type);
		else if (strcmp(stack_name, "dmg") == 0 ||
		    (stack->layers[0] == '\0' && stack->compression == NULL))
			check_disk(ctx, &out, expected_bytes,
			    strcmp(stack_name, "dmg") == 0 ?
			    (expected_bytes->length + 511) / 512 * 512 :
			    expected_bytes->length);
	}

	fsimg_io_free(inputs.ifsboot_startup);
	fsimg_io_free(output);
	free(out.bytes);
}

static void
check_wrap(fsimg_ctx_t *ctx, const char *input_name, memory_t *volume,
    const wrap_stack_t *stack, const char *scenario,
    wrap_expectation_t expected)
{
	fsimg_io_t *input = memory_io(ctx, volume);

	check_wrap_io(ctx, input_name, input, volume, stack, scenario, expected);
	fsimg_io_free(input);
}

/* A retained output lets fixture builders inspect the same compose path. */
static void
check_compose(fsimg_ctx_t *ctx, const char *scheme,
    const fsimg_part_t *parts, size_t count, const char *scenario,
    wrap_expectation_t expected, memory_t *retained)
{
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	memory_t out = { 0 };
	fsimg_io_t *output = memory_io(ctx, &out);
	char label[192];

	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	status = fsimg_image_compose(ctx, scheme, parts, count, &options,
	    &sink, &error);
	snprintf(label, sizeof(label), "%s compose %s", scenario, scheme);
	check_result(label, status, &error, &out, expected);
	fsimg_io_free(output);
	if (retained != NULL) {
		CHECK(status == FSIMG_OK);
		*retained = out;
	} else
		free(out.bytes);
}

static void
check_compose_volume(fsimg_ctx_t *ctx, memory_t *volume, const char *type,
    const char *scenario, wrap_expectation_t expected)
{
	fsimg_io_t *input = memory_io(ctx, volume);
	fsimg_part_t part = { .volume = input, .type = type };
	memory_t output;

	check_compose(ctx, "gpt", &part, 1, scenario, expected,
	    expected.status == FSIMG_OK ? &output : NULL);
	if (expected.status == FSIMG_OK) {
		check_partition(ctx, &output, volume, NULL);
		free(output.bytes);
	}
	fsimg_io_free(input);
}

/* A lone successful probe separates open refusals from probe refusals. */
static void
check_format_claim(const memory_t *volume, const char *format)
{
	fsimg_survey_t survey;
	size_t prefix = fsimg_format_probe_bytes(NULL);

	if (volume->length < prefix)
		prefix = (size_t)volume->length;
	CHECK(fsimg_format_identify(NULL, volume->bytes, prefix, volume->length,
	    &survey, NULL) == FSIMG_OK);
	CHECK(survey.claims == 1);
	CHECK(survey.status == FSIMG_OK);
	CHECK(strcmp(survey.variant->info.name, format) == 0);
}

static void
run_ifs_open_refusal(fsimg_ctx_t *ctx, memory_t *volume)
{
	memory_t cut;
	uint32_t sum = 0, trailer;

	/* The little-endian spare word is outside the probe's header rules.
	 * Balance the word sum in the trailer so only open's spare rule refuses.
	 */
	CHECK(volume->bytes[7] == 0x04);
	volume->bytes[0x2c] = 1;
	for (uint64_t i = 0; i < volume->length - 4; i += 4)
		sum += (uint32_t)volume->bytes[i] |
		    (uint32_t)volume->bytes[i + 1] << 8 |
		    (uint32_t)volume->bytes[i + 2] << 16 |
		    (uint32_t)volume->bytes[i + 3] << 24;
	trailer = 0u - sum;
	for (unsigned i = 0; i < 4; i++)
		volume->bytes[volume->length - 4 + i] =
		    (unsigned char)(trailer >> (8 * i));
	cut = *volume;
	cut.length -= 4096;
	check_compose_volume(ctx, volume, "linux", "spare-word-intact ifs",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "partition 1", .message =
	    "ImageFS header spare words are nonzero" });
	check_format_claim(volume, "ifs");

	check_format_claim(&cut, "ifs");
	check_wrap(ctx, "ifs", volume, &bare, "spare-word-intact",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "input", .message =
	    "ImageFS header spare words are nonzero" });
	check_wrap(ctx, "ifs", &cut, &bare, "spare-word-cut",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "input", .message =
	    "ImageFS header spare words are nonzero" });
	for (size_t i = 0; i < COUNT(raw_stacks); i++) {
		check_wrap(ctx, "ifs", volume, &raw_stacks[i], "spare-word-intact",
		    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
		    .error_path = "input", .message =
		    "ImageFS header spare words are nonzero" });
		check_wrap(ctx, "ifs", &cut, &raw_stacks[i], "spare-word-cut",
		    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
		    .error_path = "input", .message =
		    "ImageFS header spare words are nonzero" });
	}
}

static void
run_fat_open_io_refusal(fsimg_ctx_t *ctx, memory_t *volume)
{
	memory_t failing = *volume;
	fsimg_io_t *input;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error = { 0 };

	/* FAT32's second table lies beyond the shared filesystem probe prefix.
	 * Open reads the mirrored reserved entries before accepting the volume.
	 */
	failing.read_limit = fsimg_format_probe_bytes(NULL);
	check_format_claim(&failing, "fat32");
	input = memory_io(ctx, &failing);
	CHECK(fsimg_reader_open(ctx, input, NULL, &reader, &error) == FSIMG_EIO);
	CHECK(reader == NULL);
	CHECK(strcmp(error.message,
	    "injected read failure beyond probe prefix") == 0);
	fsimg_io_free(input);
	check_wrap(ctx, "fat32", &failing, &dmg, "open-read-failure",
	    (wrap_expectation_t){ .status = FSIMG_EIO, .error_path = "input",
	    .message = "injected read failure beyond probe prefix" });
}

static void
run_fat_open_work_limit(memory_t *volume)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_io_t *input;
	fsimg_reader_t *reader = NULL;

	/* The two reserved entries each cost work; the probe does not walk FATs. */
	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	options.max_work = 1;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	check_format_claim(volume, "fat12");
	input = memory_io(ctx, volume);
	CHECK(fsimg_reader_open(ctx, input, NULL, &reader, NULL) == FSIMG_ELIMIT);
	CHECK(reader == NULL);
	fsimg_io_free(input);
	check_wrap(ctx, "fat12", volume, &dmg, "open-work-limit",
	    (wrap_expectation_t){ .status = FSIMG_ELIMIT, .error_path = "input" });
	fsimg_ctx_free(ctx);
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
	check_compose_volume(ctx, &cut, fixture->gpt_type, "below-1MiB exfat",
	    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
	    .error_path = "partition 1", .message = message });

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
	for (size_t i = 0; i < COUNT(bare_and_scheme); i++) {
		check_wrap(ctx, fixture->format, volume, &bare_and_scheme[i],
		    "bad-checksum", (wrap_expectation_t){
		    .status = FSIMG_ECORRUPT, .error_path = "input",
		    .message = "invalid exFAT boot checksum" });
	}
}

static void
run_fat_cases(fsimg_ctx_t *ctx, const volume_case_t *fixture,
    memory_t *volume)
{
	memory_t cut = *volume;
	char scenario[64];

	/* The BPB reserved-sector count locates the first FAT. Keep its first
	 * sector only, cutting inside the tables.
	 */
	cut.length = ((uint64_t)volume->bytes[14] +
	    ((uint64_t)volume->bytes[15] << 8) + 1) * 512;
	CHECK(cut.length < volume->length);
	snprintf(scenario, sizeof(scenario), "%s tables-cut", fixture->format);
	check_compose_volume(ctx, &cut, "linux", scenario,
	    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
	    .error_path = "partition 1", .driver_message_contains = "truncated" });

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
run_compose_refuses_truncated_second_part(fsimg_ctx_t *ctx, memory_t *volume)
{
	memory_t cut = *volume;
	fsimg_io_t *input = memory_io(ctx, volume), *clipped;
	fsimg_part_t parts[2] = { { .volume = input }, { 0 } };
	static const char *types[] = { NULL, "linux" };

	cut.length -= 4096;
	clipped = memory_io(ctx, &cut);
	parts[1].volume = clipped;
	for (size_t i = 0; i < COUNT(types); i++) {
		parts[1].type = types[i];
		check_compose(ctx, "gpt", parts, COUNT(parts),
		    types[i] == NULL ? "second-fat16-cut" : "typed-second-fat16-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "partition 2", .message =
		    "image is truncated: 4096 declared bytes are missing" }, NULL);
	}
	parts[1].volume = input;
	parts[1].type = NULL;
	check_compose(ctx, "mbr", parts, COUNT(parts), "two-intact-fat16-parts",
	    (wrap_expectation_t){ .status = FSIMG_OK }, NULL);
	fsimg_io_free(clipped);
	fsimg_io_free(input);
}

static void
check_unwrap_refuses_clipped_partition(fsimg_ctx_t *ctx, fsimg_image_t *image,
    const fsimg_selector_t *selector, const char *scenario,
    wrap_expectation_t expected)
{
	memory_t out = { 0 };
	fsimg_io_t *output = memory_io(ctx, &out);
	fsimg_sink_t sink;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;

	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	status = fsimg_image_unwrap(image, selector, NULL, &sink, &error);
	check_result(scenario, status, &error, &out, expected);
	fsimg_io_free(output);
	free(out.bytes);
}

typedef struct cut_disk {
	memory_t bytes;
	fsimg_io_t *io;
	fsimg_image_t *image;
	uint64_t declared_length;
	uint64_t second_offset;
	uint64_t missing;
	char message[128];
} cut_disk_t;

static memory_t
build_padded_partition(const memory_t *volume)
{
	memory_t padded = { 0 };

	/* Padding keeps the filesystem whole when the partition loses its tail;
	 * only the partition's declared extent can prove the missing count.
	 */
	CHECK(memory_resize(&padded, volume->length + 8192, NULL) == FSIMG_OK);
	bcopy(volume->bytes, padded.bytes, (size_t)volume->length);
	return (padded);
}

static void
build_cut_disk(fsimg_ctx_t *ctx, cut_disk_t *disk, const char *scheme,
    memory_t *first, memory_t *second)
{
	fsimg_io_t *first_io = memory_io(ctx, first);
	fsimg_io_t *second_io = memory_io(ctx, second);
	fsimg_part_t parts[] = {
		{ .volume = first_io, .type = "linux" },
		{ .volume = second_io, .type = "linux" }
	};

	/* Derive extents from compose's sector and start alignment contracts,
	 * independently of the table reader that the audit exercises.
	 */
	disk->declared_length = (second->length + 511) / 512 * 512;
	disk->second_offset = (MIB + (first->length + 511) / 512 * 512 +
	    MIB - 1) / MIB * MIB;
	disk->missing = disk->declared_length - (second->length - 4096);
	snprintf(disk->message, sizeof(disk->message),
	    "the selected partition is truncated: %llu declared bytes are missing",
	    (unsigned long long)disk->missing);
	check_compose(ctx, scheme, parts, COUNT(parts), "build-cut-disk",
	    (wrap_expectation_t){ .status = FSIMG_OK }, &disk->bytes);
	disk->bytes.length = disk->second_offset + second->length - 4096;
	disk->io = memory_io(ctx, &disk->bytes);
	CHECK(fsimg_image_open(ctx, disk->io, NULL, &disk->image, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(second_io);
	fsimg_io_free(first_io);
}

static void
check_cut_partition_table(cut_disk_t *disk)
{
	fsimg_partition_info_t partition;
	fsimg_status_t status;
	unsigned used = 0;

	while ((status = fsimg_image_next_partition(disk->image, &partition,
	    NULL)) == FSIMG_OK) {
		if (!partition.used)
			continue;
		used++;
		CHECK(partition.index == used);
		CHECK(partition.offset == (used == 1 ? MIB : disk->second_offset));
		CHECK(partition.length == disk->declared_length -
		    (used == 1 ? 0 : disk->missing));
		CHECK(partition.missing_bytes == (used == 1 ? 0 : disk->missing));
	}
	CHECK(status == FSIMG_DONE);
	CHECK(used == 2);
}

static void
check_compose_refuses_clipped_io(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const char *scenario, const char *message)
{
	fsimg_part_t part = { .volume = input, .type = "linux" };

	check_compose(ctx, "gpt", &part, 1, scenario,
	    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
	    .error_path = "partition 1", .message = message }, NULL);
}

static void
check_selected_partition_refusals(fsimg_ctx_t *ctx, cut_disk_t *disk,
    fsimg_io_t *selected, const char *format, const char *scenario)
{
	static const wrap_stack_t named[] = {
		{ .layers = "/gpt" }, { .layers = "/gpt/dmg" }
	};
	const wrap_stack_t carried = { .layers = "/ifsboot" };
	const fsimg_selector_t selector = {
		.kind = FSIMG_SELECT_INDEX, .index = 2
	};
	wrap_expectation_t expected = { .status = FSIMG_ECORRUPT,
	    .error_path = "input", .message = disk->message };

	check_compose_refuses_clipped_io(ctx, selected, scenario, disk->message);
	for (size_t i = 0; i < COUNT(raw_stacks); i++)
		check_wrap_io(ctx, format, selected, NULL, &raw_stacks[i],
		    scenario, expected);
	if (strcmp(format, "fat16") == 0) {
		for (size_t i = 0; i < COUNT(named); i++)
			check_wrap_io(ctx, format, selected, NULL, &named[i],
			    scenario, expected);
	}
	/* Raw bytes cannot verify as ImageFS: the carried count must refuse
	 * before ifsboot attempts that verification.
	 */
	check_wrap_io(ctx, "ifs", selected, NULL, &carried, scenario, expected);
	expected.error_path = "";
	check_unwrap_refuses_clipped_partition(ctx, disk->image, &selector,
	    scenario, expected);
}

static void
check_nested_selection_refusals(fsimg_ctx_t *ctx, fsimg_io_t *slice,
    const char *format, const char *scenario, const char *message)
{
	static const fsimg_selector_t selectors[] = {
		{ .kind = FSIMG_SELECT_AUTOMATIC }, { .kind = FSIMG_SELECT_DISK }
	};
	fsimg_image_options_t options;
	fsimg_image_t *nested;
	fsimg_error_t error = { 0 };
	fsimg_status_t status;
	fsimg_io_t *selected;
	char label[192];
	wrap_expectation_t expected = { .status = FSIMG_ECORRUPT,
	    .error_path = "input", .message = message };

	fsimg_image_options_init(&options);
	/* Unrecognized raw bytes need an explicit empty scheme slot so
	 * automatic selection treats the input as the entire volume.
	 */
	if (strcmp(format, "raw") == 0)
		options.stack.slots[FSIMG_LAYER_SCHEME].state = FSIMG_SLOT_EMPTY;
	CHECK(fsimg_image_open(ctx, slice, &options, &nested, NULL) == FSIMG_OK);
	for (size_t i = 0; i < COUNT(selectors); i++) {
		snprintf(label, sizeof(label), "%s nested-%s", scenario,
		    i == 0 ? "automatic" : "disk");
		status = fsimg_image_select(nested, &selectors[i], &selected, &error);
		if (status != FSIMG_OK)
			fprintf(stderr, "%s: selection status=%d path='%s' message='%s'\n",
			    label, status, error.path, error.message);
		CHECK(status == FSIMG_OK);
		check_compose_refuses_clipped_io(ctx, selected, label, message);
		expected.error_path = "input";
		check_wrap_io(ctx, "raw", selected, NULL, &dmg, label, expected);
		expected.error_path = "";
		check_unwrap_refuses_clipped_partition(ctx, nested, &selectors[i],
		    label, expected);
		fsimg_io_free(selected);
	}
	fsimg_image_free(nested);
}

static void
check_slices_carry_only_a_missing_tail(fsimg_ctx_t *ctx, fsimg_io_t *slice,
    const char *scenario, const char *message)
{
	uint64_t length;
	fsimg_io_t *view;
	fsimg_part_t part = { .type = "linux" };
	char label[192];

	CHECK(fsimg_io_get_size(slice, &length, NULL) == FSIMG_OK);
	/* A nonzero start discards the filesystem header. Tail provenance must
	 * survive independently of any filesystem a probe could recognize.
	 */
	for (size_t i = 0; i < 2; i++) {
		uint64_t start = i == 0 ? 0 : 512;

		snprintf(label, sizeof(label), "%s slice-tail-%llu", scenario,
		    (unsigned long long)start);
		CHECK(fsimg_io_slice(slice, start, length - start, &view, NULL) ==
		    FSIMG_OK);
		check_compose_refuses_clipped_io(ctx, view, label, message);
		fsimg_io_free(view);
	}
	/* This interior range contains only existing bytes and no FAT header. */
	CHECK(fsimg_io_slice(slice, 512, 512, &view, NULL) == FSIMG_OK);
	part.volume = view;
	snprintf(label, sizeof(label), "%s slice-before-tail", scenario);
	check_compose(ctx, "gpt", &part, 1, label,
	    (wrap_expectation_t){ .status = FSIMG_OK }, NULL);
	fsimg_io_free(view);
}

static void
check_intact_partition_and_disk_ranges(fsimg_ctx_t *ctx, cut_disk_t *disk,
    memory_t *volume, const char *format)
{
	static const fsimg_selector_t selectors[] = {
		{ .kind = FSIMG_SELECT_INDEX, .index = 1 },
		{ .kind = FSIMG_SELECT_RANGE }, { .kind = FSIMG_SELECT_DISK }
	};
	static const char *scenarios[] = {
		"selected-intact-before-cut", "range-of-cut-disk", "selected-cut-disk"
	};
	fsimg_io_t *selected;
	fsimg_part_t part = { .type = "linux" };
	size_t count = strcmp(format, "raw") == 0 ? COUNT(selectors) : 1;

	for (size_t i = 0; i < count; i++) {
		CHECK(fsimg_image_select(disk->image, &selectors[i], &selected,
		    NULL) == FSIMG_OK);
		if (i == 0)
			check_volume_bytes(selected, volume, disk->declared_length);
		part.volume = selected;
		check_compose(ctx, "gpt", &part, 1, scenarios[i],
		    (wrap_expectation_t){ .status = FSIMG_OK }, NULL);
		fsimg_io_free(selected);
	}
}

static void
free_cut_disk(cut_disk_t *disk)
{
	fsimg_image_free(disk->image);
	fsimg_io_free(disk->io);
	free(disk->bytes.bytes);
}

static void
run_selected_partition_refuses_clipped_bytes(fsimg_ctx_t *ctx,
    memory_t *volume, const char *format)
{
	const fsimg_selector_t selector = {
		.kind = FSIMG_SELECT_INDEX, .index = 2
	};
	cut_disk_t disk = { 0 };
	fsimg_io_t *selected;
	char scenario[128];

	for (size_t i = 0; i < COUNT(schemes); i++) {
		snprintf(scenario, sizeof(scenario), "%s %s selected-clipped",
		    schemes[i], format);
		build_cut_disk(ctx, &disk, schemes[i], volume, volume);
		check_cut_partition_table(&disk);
		CHECK(fsimg_image_select(disk.image, &selector, &selected, NULL) ==
		    FSIMG_OK);
		check_selected_partition_refusals(ctx, &disk, selected, format,
		    scenario);
		check_nested_selection_refusals(ctx, selected, format, scenario,
		    disk.message);
		check_slices_carry_only_a_missing_tail(ctx, selected, scenario,
		    disk.message);
		fsimg_io_free(selected);
		check_intact_partition_and_disk_ranges(ctx, &disk, volume, format);
		free_cut_disk(&disk);
	}
}

static void
run_automatic_selects_only_clipped_filesystem(fsimg_ctx_t *ctx,
    memory_t *volume)
{
	const fsimg_selector_t selector = { .kind = FSIMG_SELECT_AUTOMATIC };
	memory_t raw = { 0 }, expected_bytes = *volume;
	cut_disk_t disk = { 0 };
	fsimg_io_t *selected;
	char scenario[128];

	/* Only the second partition has a filesystem; automatic selection must
	 * preserve its clipped extent rather than choose the raw whole disk.
	 */
	CHECK(memory_resize(&raw, MIB, NULL) == FSIMG_OK);
	expected_bytes.length -= 4096;
	for (size_t i = 0; i < COUNT(schemes); i++) {
		build_cut_disk(ctx, &disk, schemes[i], &raw, volume);
		snprintf(scenario, sizeof(scenario), "%s automatic-clipped-fat16",
		    schemes[i]);
		CHECK(fsimg_image_select(disk.image, &selector, &selected, NULL) ==
		    FSIMG_OK);
		check_volume_bytes(selected, &expected_bytes, expected_bytes.length);
		check_compose_refuses_clipped_io(ctx, selected, scenario, disk.message);
		fsimg_io_free(selected);
		free_cut_disk(&disk);
	}
	free(raw.bytes);
}

static fsimg_status_t
fail_every_read(void *argument FSIMG_UNUSED, uint64_t offset FSIMG_UNUSED,
    void *buffer FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    size_t *transferred, fsimg_error_t *error)
{
	*transferred = 0;
	if (error != NULL) {
		error->status = FSIMG_EIO;
		snprintf(error->message, sizeof(error->message),
		    "injected probe read failure");
	}
	return (FSIMG_EIO);
}

static void
run_probe_io_failures_name_the_input(fsimg_ctx_t *ctx, memory_t *volume)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *input = memory_io(ctx, volume), *unreadable;
	fsimg_part_t parts[] = { { .volume = input }, { .type = "linux" } };
	wrap_expectation_t expected = { .status = FSIMG_EIO,
	    .error_path = "partition 2", .message = "injected probe read failure" };

	fsimg_io_ops_init(&ops);
	ops.read_at = fail_every_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, volume, &unreadable, NULL) ==
	    FSIMG_OK);
	parts[1].volume = unreadable;
	check_compose(ctx, "gpt", parts, COUNT(parts), "second-part-probe-io",
	    expected, NULL);
	expected.error_path = "input";
	check_wrap_io(ctx, "raw", unreadable, NULL, &dmg, "empty-slot-probe-io",
	    expected);
	fsimg_io_free(unreadable);
	fsimg_io_free(input);
}

static void
run_volume(fsimg_ctx_t *ctx, const volume_case_t *fixture)
{
	const wrap_stack_t boot = { .layers = "/ifsboot" };
	memory_t volume, cut;
	char message[128], scenario[64];
	const char *compose_type;
	/* Named ImageFS plans refuse partition schemes and padding containers.
	 * Literal GPT with an explicit type accepts an intact ImageFS volume.
	 */
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
	compose_type = imagefs_requires_exact_size ? "linux" : fixture->gpt_type;
	snprintf(scenario, sizeof(scenario), "%s tail-cut", fixture->format);
	check_compose_volume(ctx, &cut, compose_type, scenario,
	    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
	    .error_path = "partition 1", .message = message });
	if (strcmp(fixture->format, "fat16") == 0)
		check_compose_volume(ctx, &cut, "linux", "typed-fat16-tail-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "partition 1", .message = message });
	snprintf(scenario, sizeof(scenario), "%s intact", fixture->format);
	check_compose_volume(ctx, &volume, compose_type, scenario,
	    (wrap_expectation_t){ .status = FSIMG_OK });

	for (size_t i = 0; i < COUNT(stacks); i++) {
		wrap_stack_t stack = stacks[i];

		if (fixture->gpt_type != NULL)
			stack.gpt_type = fixture->gpt_type;
		check_wrap(ctx, fixture->format, &cut, &stack, "tail-cut",
		    (wrap_expectation_t){ .status = FSIMG_ECORRUPT,
		    .error_path = "input", .message = message });
		if (!imagefs_requires_exact_size || stack.layers[0] != '/')
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
	if (strcmp(fixture->format, "fat16") == 0) {
		memory_t padded = build_padded_partition(&volume);

		run_compose_refuses_truncated_second_part(ctx, &volume);
		run_selected_partition_refuses_clipped_bytes(ctx, &padded, "fat16");
		run_automatic_selects_only_clipped_filesystem(ctx, &padded);
		run_probe_io_failures_name_the_input(ctx, &volume);
		free(padded.bytes);
	}
	if (strcmp(fixture->format, "exfat") == 0)
		run_exfat_cases_corrupting_volume(ctx, fixture, &volume);
	if (strncmp(fixture->format, "fat", 3) == 0)
		run_fat_cases(ctx, fixture, &volume);
	if (strcmp(fixture->format, "fat12") == 0) {
		const wrap_stack_t inferred_gpt = { .layers = "gpt" };

		/* An empty filesystem slot uses the claim's name to choose the type. */
		check_wrap(ctx, "fat12", &volume, &inferred_gpt, "inferred-type",
		    (wrap_expectation_t){ .status = FSIMG_OK,
		    .partition_type = "basic" });
		run_fat_open_work_limit(&volume);
	}
	if (strcmp(fixture->format, "fat32") == 0)
		run_fat_open_io_refusal(ctx, &volume);
	if (imagefs_requires_exact_size)
		run_ifs_open_refusal(ctx, &volume);
	free(volume.bytes);
}

static void
run_qnx6_unsupported(fsimg_ctx_t *ctx)
{
	unsigned char magic[] = { 0x22, 0x11, 0x19, 0x68 };
	memory_t volume = { .bytes = magic, .length = sizeof(magic) };
	memory_t padded = { 0 };

	/* Magic at byte zero identifies a bootless or MMI layout. */
	check_wrap(ctx, "qnx6", &volume, &bare, "bootless",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "input" });

	/* Padding keeps the unsupported magic at zero and gives DMG enough
	 * bytes to carry the input as a raw disk.
	 */
	CHECK(memory_resize(&padded, MIB, NULL) == FSIMG_OK);
	bcopy(magic, padded.bytes, sizeof(magic));
	check_wrap(ctx, "qnx6", &padded, &bare, "padded-bootless",
	    (wrap_expectation_t){ .status = FSIMG_EUNSUPPORTED,
	    .error_path = "input" });
	check_wrap(ctx, "qnx6", &padded, &dmg,
	    "unsupported-claim-carried-as-raw",
	    (wrap_expectation_t){ .status = FSIMG_OK });
	check_compose_volume(ctx, &padded, "linux", "bootless-qnx6-as-raw",
	    (wrap_expectation_t){ .status = FSIMG_OK });
	free(padded.bytes);
}

static void
check_unclaimed_input(fsimg_ctx_t *ctx, memory_t *volume)
{
	fsimg_io_t *io = memory_io(ctx, volume);
	fsimg_reader_t *reader = NULL;

	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_EFORMAT);
	CHECK(reader == NULL);
	fsimg_io_free(io);
}

static void
run_unclaimed_input_passes_as_raw(fsimg_ctx_t *ctx, memory_t *volume)
{
	memory_t cut = *volume;

	/* Without a filesystem claim, the shorter length proves no truncation. */
	cut.length -= 4096;
	check_unclaimed_input(ctx, volume);
	check_unclaimed_input(ctx, &cut);
	check_compose_volume(ctx, volume, "linux", "unclaimed-intact",
	    (wrap_expectation_t){ .status = FSIMG_OK });
	check_compose_volume(ctx, &cut, "linux", "unclaimed-cut",
	    (wrap_expectation_t){ .status = FSIMG_OK });

	for (size_t i = 0; i < COUNT(raw_stacks); i++) {
		check_wrap(ctx, "raw", volume, &raw_stacks[i], "unclaimed-intact",
		    (wrap_expectation_t){ .status = FSIMG_OK });
		check_wrap(ctx, "raw", &cut, &raw_stacks[i], "unclaimed-cut",
		    (wrap_expectation_t){ .status = FSIMG_OK });
	}
}

static void
run_cut_partitioned_disks_pass_as_raw(fsimg_ctx_t *ctx, memory_t *volume)
{
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_sink_t sink;
	fsimg_io_t *input, *output;
	memory_t disk;

	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	options.layers = &layers;
	for (size_t i = 0; i < COUNT(schemes); i++) {
		fsimg_layer_options_init(&layers);
		if (strcmp(schemes[i], "gpt") == 0)
			snprintf(layers.gpt_type, sizeof(layers.gpt_type), "linux");
		else
			snprintf(layers.mbr_type, sizeof(layers.mbr_type), "linux");
		disk = (memory_t){ 0 };
		input = memory_io(ctx, volume);
		output = memory_io(ctx, &disk);
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = output;
		CHECK(fsimg_image_wrap(ctx, input, schemes[i], &options,
		    &sink, NULL) == FSIMG_OK);
		CHECK(disk.length > 4096);
		/* The documented scope is the input volume's filesystem claim.
		 * A cut partitioned disk draws no such claim and passes as raw.
		 */
		disk.length -= 4096;
		check_unclaimed_input(ctx, &disk);
		check_wrap(ctx, schemes[i], &disk, &dmg,
		    "cut-partitioned-disk", (wrap_expectation_t){ .status = FSIMG_OK });
		fsimg_io_free(output);
		fsimg_io_free(input);
		free(disk.bytes);
	}
}

int
main(void)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	memory_t raw = { 0 };

	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < COUNT(volumes); i++)
		run_volume(ctx, &volumes[i]);
	run_qnx4_short_cut(ctx);
	run_qnx6_unsupported(ctx);
	/* The resize helper's nonzero 0xa5 fill has no filesystem signature. */
	CHECK(memory_resize(&raw, MIB, NULL) == FSIMG_OK);
	run_unclaimed_input_passes_as_raw(ctx, &raw);
	run_selected_partition_refuses_clipped_bytes(ctx, &raw, "raw");
	run_cut_partitioned_disks_pass_as_raw(ctx, &raw);
	free(raw.bytes);
	fsimg_ctx_free(ctx);
	return (failures != 0);
}
