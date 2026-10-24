/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <zlib.h>

#include "internal.h"
#include "test.h"
#include "zlib-memory.h"

/*
 * The context-backed zlib allocator (specs/design-binary-primitives.md,
 * "Context-backed zlib allocation"). Direct cases call the installed
 * callbacks through a bound stream; lifetime cases run the system zlib.
 * With 64-bit size_t and 32-bit uInt the callback's product overflow is
 * unreachable, so that case is inapplicable here; the context tests keep
 * the allocator's own header-overflow coverage.
 */

#define MIB	(UINT64_C(1024) * 1024)

static fsimg_ctx_t *
context_new(void)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * MIB;
	options.scratch_directory_fd = -1;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	return (ctx);
}

static uint64_t
memory_used(fsimg_ctx_t *ctx)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

static void
bind_fresh(fsimg_zlib_memory_t *memory, fsimg_ctx_t *ctx, z_stream *stream)
{
	bzero(stream, sizeof(*stream));
	bzero(memory, sizeof(*memory));
	fsimg_zlib_memory_bind(memory, ctx, stream);
	CHECK(stream->zalloc != NULL && stream->zfree != NULL);
	CHECK(stream->opaque == memory);
}

/* Allocations are charged with the context's fixed header and released
 * by free; a zero-byte request is a distinct freeable allocation.
 */
static void
test_charges(void)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_ctx_t *ctx;
	uint64_t baseline, header;
	voidpf small, zero;

	ctx = context_new();
	bind_fresh(&memory, ctx, &stream);
	baseline = memory_used(ctx);
	small = stream.zalloc(stream.opaque, 10, 10);
	CHECK(small != Z_NULL);
	header = memory_used(ctx) - baseline - 100;
	CHECK(header > 0);
	zero = stream.zalloc(stream.opaque, 0, 4);
	CHECK(zero != Z_NULL && zero != small);
	CHECK(memory_used(ctx) == baseline + 100 + 2 * header);
	stream.zfree(stream.opaque, small);
	stream.zfree(stream.opaque, zero);
	stream.zfree(stream.opaque, Z_NULL);
	CHECK(memory_used(ctx) == baseline);
	CHECK(memory.error.status == FSIMG_OK);
	fsimg_ctx_free(ctx);
}

/* A request beyond the budget keeps the exact limit diagnostic, and an
 * injected failure keeps FSIMG_ENOMEM; either stays the first error
 * through later attempts, successful frees and other causes.
 */
static void
test_first_error(void)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uint64_t baseline;
	voidpf kept;

	ctx = context_new();
	bind_fresh(&memory, ctx, &stream);
	kept = stream.zalloc(stream.opaque, 1, 64);
	CHECK(kept != Z_NULL);
	baseline = memory_used(ctx);
	CHECK(stream.zalloc(stream.opaque, 1024, 32 * 1024) == Z_NULL);
	CHECK(memory_used(ctx) == baseline);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(error.limit == 16 * MIB && error.observed > error.limit);
	/* Later small requests fail without replacing the cause. */
	CHECK(stream.zalloc(stream.opaque, 1, 8) == Z_NULL);
	stream.zfree(stream.opaque, kept);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);

	bind_fresh(&memory, ctx, &stream);
	baseline = memory_used(ctx);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(stream.zalloc(stream.opaque, 1, 8) == Z_NULL);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(stream.zalloc(stream.opaque, 1, 8) == Z_NULL);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
	CHECK(error.operation == FSIMG_OP_ALLOCATE);
	CHECK(memory_used(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/* Z_MEM_ERROR without a recorded callback failure is FSIMG_ENOMEM, never
 * a limit or success.
 */
static void
test_fallback(void)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;

	ctx = context_new();
	bind_fresh(&memory, ctx, &stream);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
	CHECK(error.status == FSIMG_ENOMEM && error.resource ==
	    FSIMG_RESOURCE_NONE);
	fsimg_ctx_free(ctx);
}

/*
 * Every allocation of deflate and inflate initialization fails in turn
 * until initialization succeeds without one: each failure is Z_MEM_ERROR
 * with the captured FSIMG_ENOMEM, and zlib's own unwinding returns the
 * context to its baseline. deflateInit allocates several buffers before
 * testing them, so a later success in the same call must not erase the
 * first failure.
 */
static void
test_init_ordinals(void)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uint64_t baseline, position;
	int result;

	ctx = context_new();
	baseline = memory_used(ctx);
	for (int inflating = 0; inflating < 2; inflating++) {
		for (position = 1; position < 64; position++) {
			bind_fresh(&memory, ctx, &stream);
			fsimg_test_fail_alloc_after(ctx, position);
			result = inflating ? inflateInit(&stream) :
			    deflateInit(&stream, 6);
			fsimg_test_fail_alloc_after(ctx, 0);
			if (result == Z_OK)
				break;
			CHECK(result == Z_MEM_ERROR);
			CHECK(fsimg_zlib_memory_error(&memory, &error) ==
			    FSIMG_ENOMEM);
			CHECK(memory_used(ctx) == baseline);
		}
		CHECK(result == Z_OK && position > 1);
		if (inflating)
			CHECK(inflateEnd(&stream) == Z_OK);
		else
			CHECK(deflateEnd(&stream) == Z_OK);
		CHECK(memory_used(ctx) == baseline);
	}
	fsimg_ctx_free(ctx);
}

/* inflate allocates its window when a stream continues across calls;
 * failing that allocation after a successful init is Z_MEM_ERROR with the
 * captured cause, and End releases everything. A fresh pair then decodes
 * the same bytes, four output bytes per call.
 */
static void
test_delayed_window(void)
{
	static const char text[] = "a window is allocated only when needed";
	unsigned char packed[128], plain[128];
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uLongf packed_length = sizeof(packed);
	uint64_t baseline;
	int result;

	CHECK(compress(packed, &packed_length, (const Bytef *)text,
	    sizeof(text)) == Z_OK);
	ctx = context_new();
	baseline = memory_used(ctx);
	for (int attempt = 0; attempt < 2; attempt++) {
		bind_fresh(&memory, ctx, &stream);
		CHECK(inflateInit(&stream) == Z_OK);
		stream.next_in = packed;
		stream.avail_in = (uInt)packed_length;
		stream.next_out = plain;
		if (attempt == 0)
			fsimg_test_fail_alloc_after(ctx, 1);
		do {
			stream.avail_out = 4;
			result = inflate(&stream, Z_NO_FLUSH);
		} while (result == Z_OK);
		fsimg_test_fail_alloc_after(ctx, 0);
		if (attempt == 0) {
			CHECK(result == Z_MEM_ERROR);
			CHECK(fsimg_zlib_memory_error(&memory, &error) ==
			    FSIMG_ENOMEM);
		} else {
			CHECK(result == Z_STREAM_END);
			CHECK(stream.total_out == sizeof(text) &&
			    memcmp(plain, text, sizeof(text)) == 0);
			CHECK(memory.error.status == FSIMG_OK);
		}
		CHECK(inflateEnd(&stream) == Z_OK);
		CHECK(memory_used(ctx) == baseline);
	}
	fsimg_ctx_free(ctx);
}

/* Two streams share one budget but keep separate errors, and ending one
 * leaves the other's storage charged.
 */
static void
test_two_streams(void)
{
	fsimg_zlib_memory_t first_memory, second_memory;
	z_stream first, second;
	fsimg_ctx_t *ctx;
	uint64_t baseline, one;

	ctx = context_new();
	baseline = memory_used(ctx);
	bind_fresh(&first_memory, ctx, &first);
	bind_fresh(&second_memory, ctx, &second);
	CHECK(deflateInit(&first, 9) == Z_OK);
	one = memory_used(ctx) - baseline;
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(deflateInit(&second, 9) == Z_MEM_ERROR);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(first_memory.error.status == FSIMG_OK);
	CHECK(second_memory.error.status == FSIMG_ENOMEM);
	CHECK(memory_used(ctx) == baseline + one);
	bind_fresh(&second_memory, ctx, &second);
	CHECK(deflateInit(&second, 9) == Z_OK);
	CHECK(memory_used(ctx) == baseline + 2 * one);
	CHECK(deflateEnd(&second) == Z_OK);
	CHECK(memory_used(ctx) == baseline + one);
	CHECK(deflateEnd(&first) == Z_OK);
	CHECK(memory_used(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

static fsimg_status_t
volume_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	(void)argument;
	(void)error;
	for (size_t i = 0; i < length; i++)
		((unsigned char *)buffer)[i] = (unsigned char)((offset + i) % 251);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_size(void *argument, uint64_t *length, fsimg_error_t *error)
{
	(void)argument;
	(void)error;
	*length = MIB;
	return (FSIMG_OK);
}

/*
 * The encoders' owners: wrapping a volume as a zlib DMG and as a gzipped
 * VHD, each streamed to /dev/null, fails each allocation in turn, and
 * every failure, those inside deflate initialization included, is
 * FSIMG_ENOMEM, never another class.
 */
static void
test_encoder_sweeps(void)
{
	static const struct {
		const char	*stack;
		const char	*compression;
	} cases[] = {
		{ .stack = "dmg", .compression = "" },
		{ .stack = "vhd", .compression = "gzip" },
	};
	fsimg_write_options_t options;
	fsimg_io_ops_t input_ops;
	fsimg_io_t *input;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	uint64_t baseline, position;
	int null;

	ctx = context_new();
	fsimg_io_ops_init(&input_ops);
	input_ops.read_at = volume_read;
	input_ops.get_size = volume_size;
	CHECK(fsimg_io_from_callbacks(ctx, &input_ops, NULL, &input, NULL) ==
	    FSIMG_OK);
	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(null != -1);
	baseline = memory_used(ctx);
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		fsimg_write_options_init(&options);
		options.flags |= FSIMG_WRITE_REPRODUCIBLE;
		snprintf(options.compression, sizeof(options.compression), "%s",
		    cases[c].compression);
		for (position = 1; position < 4096; position++) {
			fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
			sink.stream_fd = null;
			fsimg_test_fail_alloc_after(ctx, position);
			status = fsimg_image_wrap(ctx, input, cases[c].stack,
			    &options, &sink, &error);
			fsimg_test_fail_alloc_after(ctx, 0);
			CHECK(memory_used(ctx) == baseline);
			if (status == FSIMG_OK)
				break;
			if (status != FSIMG_ENOMEM)
				fprintf(stderr, "%s%s allocation %llu: %d %s\n",
				    cases[c].stack, cases[c].compression,
				    (unsigned long long)position, status,
				    error.message);
			CHECK(status == FSIMG_ENOMEM);
		}
		CHECK(status == FSIMG_OK && position > 1);
	}
	CHECK(close(null) == 0);
	fsimg_io_free(input);
	fsimg_ctx_free(ctx);
}

/* Wrap the generated volume into stack, compressed as named, streaming
 * the output to fd.
 */
static fsimg_status_t
wrap_to(fsimg_ctx_t *ctx, fsimg_io_t *input, const char *stack,
    const char *compression, int fd, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_sink_t sink;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	snprintf(options.compression, sizeof(options.compression), "%s",
	    compression);
	fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
	sink.stream_fd = fd;
	return (fsimg_image_wrap(ctx, input, stack, &options, &sink, error));
}

/*
 * The same owners under a memory budget: a filler allocation leaves the
 * wrap headroom stepping down from its measured peak in 8 KiB steps, so
 * the budget runs out within each larger allocation, deflate's 64 KiB
 * buffers included. Every refusal is FSIMG_ELIMIT naming memory, never a
 * fabricated ENOMEM or I/O failure, and charges return.
 */
static void
test_encoder_budgets(void)
{
	static const struct {
		const char	*stack;
		const char	*compression;
	} cases[] = {
		{ .stack = "dmg", .compression = "" },
		{ .stack = "vhd", .compression = "gzip" },
	};
	fsimg_io_ops_t input_ops;
	fsimg_io_t *input;
	fsimg_stats_t stats;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	uint64_t baseline, peak, headroom;
	unsigned refusals;
	void *filler;
	int null;

	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(null != -1);
	fsimg_io_ops_init(&input_ops);
	input_ops.read_at = volume_read;
	input_ops.get_size = volume_size;
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		ctx = context_new();
		CHECK(fsimg_io_from_callbacks(ctx, &input_ops, NULL, &input,
		    NULL) == FSIMG_OK);
		baseline = memory_used(ctx);
		CHECK(wrap_to(ctx, input, cases[c].stack, cases[c].compression,
		    null, NULL) == FSIMG_OK);
		CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
		peak = stats.memory_peak - baseline;
		refusals = 0;
		for (headroom = peak; headroom >= 8192; headroom -= 8192) {
			filler = fsimg_alloc(ctx, (size_t)(16 * MIB - baseline -
			    headroom - 4096), NULL);
			CHECK(filler != NULL);
			status = wrap_to(ctx, input, cases[c].stack,
			    cases[c].compression, null, &error);
			fsimg_dealloc(filler);
			CHECK(memory_used(ctx) == baseline);
			if (status == FSIMG_OK)
				continue;
			if (status != FSIMG_ELIMIT)
				fprintf(stderr, "%s%s headroom %llu: %d %s\n",
				    cases[c].stack, cases[c].compression,
				    (unsigned long long)headroom, status,
				    error.message);
			CHECK(status == FSIMG_ELIMIT);
			CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
			refusals++;
		}
		CHECK(refusals != 0);
		fsimg_io_free(input);
		fsimg_ctx_free(ctx);
	}
	CHECK(close(null) == 0);
}

/* Poison mode's nonzero contents reach zlib unchanged: the adapter adds
 * no initialization pass of its own.
 */
static void
test_poisoned_contents(void)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	unsigned char *bytes;
	fsimg_ctx_t *ctx;

	ctx = context_new();
	fsimg_test_poison_allocations(ctx, true);
	bind_fresh(&memory, ctx, &stream);
	bytes = stream.zalloc(stream.opaque, 16, 4);
	CHECK(bytes != Z_NULL);
	for (size_t i = 0; i < 64; i++)
		CHECK(bytes[i] == 0xa5);
	stream.zfree(stream.opaque, bytes);
	fsimg_ctx_free(ctx);
}

/* A reset stream decodes again with the same bound adapter and charges,
 * which reuse neither grows nor releases. Four output bytes per call make
 * inflate allocate its window, which the reset keeps.
 */
static void
test_reset_reuse(void)
{
	static const char text[] = "one stream, decoded twice after a reset";
	unsigned char packed[128], plain[128];
	fsimg_zlib_memory_t memory;
	z_stream stream;
	fsimg_ctx_t *ctx;
	uLongf packed_length = sizeof(packed);
	uint64_t baseline, live = 0;
	int result;

	CHECK(compress(packed, &packed_length, (const Bytef *)text,
	    sizeof(text)) == Z_OK);
	ctx = context_new();
	baseline = memory_used(ctx);
	bind_fresh(&memory, ctx, &stream);
	CHECK(inflateInit(&stream) == Z_OK);
	for (int pass = 0; pass < 2; pass++) {
		CHECK(inflateReset(&stream) == Z_OK);
		stream.next_in = packed;
		stream.avail_in = (uInt)packed_length;
		stream.next_out = plain;
		do {
			stream.avail_out = 4;
			result = inflate(&stream, Z_NO_FLUSH);
		} while (result == Z_OK);
		CHECK(result == Z_STREAM_END);
		CHECK(stream.total_out == sizeof(text) &&
		    memcmp(plain, text, sizeof(text)) == 0);
		CHECK(stream.opaque == &memory);
		if (pass == 0)
			live = memory_used(ctx);
		CHECK(memory_used(ctx) == live && live > baseline);
	}
	CHECK(inflateEnd(&stream) == Z_OK);
	CHECK(memory_used(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_charges();
	test_first_error();
	test_fallback();
	test_init_ordinals();
	test_delayed_window();
	test_two_streams();
	test_poisoned_contents();
	test_reset_reuse();
	test_encoder_sweeps();
	test_encoder_budgets();
	/* With 64-bit size_t, two 32-bit uInt factors cannot overflow. */
	if (sizeof(size_t) > sizeof(uInt))
		printf("inapplicable: callback product overflow\n");
	return (0);
}
