/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <zlib.h>

#include "host.h"
#include "internal.h"
#include "test.h"
#include "zlib-memory.h"

/*
 * The zlib allocation adapter against the installed zlib and the context's
 * own budget, statistics and allocation-failure injection. Direct cases
 * call the callbacks the adapter installs in a bound stream; lifetime
 * cases run real inflate and deflate streams. No expectation depends on
 * how many allocations a zlib release makes or how large they are: init
 * failures are enumerated until an attempt succeeds, and budgets are set
 * from what one stream was measured to need.
 *
 * A product of two uInt factors that overflows size_t cannot be formed
 * here: with a 64-bit size_t and a 32-bit uInt both factors and their
 * product fit, so that case is inapplicable on the supported hosts. The
 * context tests cover the allocator's own header-overflow check.
 */

#define MIB		(1024u * 1024)
#define BUDGET_BYTES	(16u * MIB)	/* The smallest a context accepts */
#define PAYLOAD_BYTES	(256u * 1024)

static fsimg_ctx_t *context_new(uint64_t);
static uint64_t memory_charged(fsimg_ctx_t *);
static size_t header_bytes(fsimg_ctx_t *);
static void *reserve_all_but(fsimg_ctx_t *, uint64_t);
static void payload_fill(unsigned char *, size_t);
static size_t compress_payload(fsimg_ctx_t *, const unsigned char *, size_t,
    unsigned char *, size_t);
static int deflate_start(z_stream *);
static int inflate_start(z_stream *);
static void test_direct_allocations(bool);
static void test_budget_failure(void);
static void test_injected_failure_is_sticky(void);
static void test_unexplained_memory_error(void);
static void test_init_ordinals(bool);
static void test_delayed_window(void);
static void test_reset_reuse(void);
static void test_fresh_bind_after_failure(void);
static void test_shared_context(void);
static void test_poisoned_output(void);
static fsimg_status_t volume_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t volume_size(void *, uint64_t *, fsimg_error_t *);
static void test_encoder_sweeps(void);
static void test_encoder_budgets(void);
static fsimg_status_t wrap_to(fsimg_ctx_t *, fsimg_io_t *, const char *,
    const char *, int, fsimg_error_t *);


static fsimg_ctx_t *
context_new(uint64_t memory_bytes)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	options.memory_bytes = memory_bytes;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	return (ctx);
}

static uint64_t
memory_charged(fsimg_ctx_t *ctx)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

/* What the context charges beyond the payload: a zero-byte allocation. */
static size_t
header_bytes(fsimg_ctx_t *ctx)
{
	uint64_t before, charged;
	void *allocation;

	before = memory_charged(ctx);
	allocation = fsimg_alloc(ctx, 0, NULL);
	CHECK(allocation != NULL);
	charged = memory_charged(ctx) - before;
	fsimg_dealloc(allocation);
	CHECK(memory_charged(ctx) == before);
	return ((size_t)charged);
}

/* Charge everything in the budget except room bytes, in one allocation. */
static void *
reserve_all_but(fsimg_ctx_t *ctx, uint64_t room)
{
	void *filler;
	uint64_t charged;

	charged = memory_charged(ctx);
	CHECK(charged + header_bytes(ctx) + room <= BUDGET_BYTES);
	filler = fsimg_alloc(ctx, (size_t)(BUDGET_BYTES - charged -
	    header_bytes(ctx) - room), NULL);
	CHECK(filler != NULL);
	CHECK(memory_charged(ctx) == BUDGET_BYTES - room);
	return (filler);
}

/* Compressible but not trivial: a repeating counter with a varying byte. */
static void
payload_fill(unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)((i % 251) ^ (i / 4096));
}

/* A complete zlib stream of the payload, through an adapter. */
static size_t
compress_payload(fsimg_ctx_t *ctx, const unsigned char *payload,
    size_t length, unsigned char *out, size_t capacity)
{
	fsimg_zlib_memory_t memory;
	z_stream stream;
	size_t produced;

	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(deflateInit(&stream, Z_DEFAULT_COMPRESSION) == Z_OK);
	stream.next_in = (Bytef *)(uintptr_t)payload;
	stream.avail_in = (uInt)length;
	stream.next_out = out;
	stream.avail_out = (uInt)capacity;
	CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	produced = (size_t)stream.total_out;
	CHECK(deflateEnd(&stream) == Z_OK);
	CHECK(memory.error.status == FSIMG_OK);
	return (produced);
}

/* The gzip writer's raw-deflate parameters, which allocate the most. */
static int
deflate_start(z_stream *stream)
{
	return (deflateInit2(stream, 9, Z_DEFLATED, -MAX_WBITS, 8,
	    Z_DEFAULT_STRATEGY));
}

static int
inflate_start(z_stream *stream)
{
	return (inflateInit(stream));
}

/*
 * The callbacks charge exactly the payload and the context's header, and
 * hand back the allocator's storage as it is: zero normally, 0xa5 under
 * poisoning. Zero factors still make a distinct, freeable, charged
 * allocation. Freeing restores the baseline.
 */
static void
test_direct_allocations(bool poison)
{
	static const struct {
		uInt	items;
		uInt	size;
	} requests[] = {
		{ 1, 1 }, { 3, 1000 }, { 1000, 3 }, { 0, 16 }, { 16, 0 }, { 0, 0 }
	};
	fsimg_zlib_memory_t memory;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline;
	unsigned char *a, *b;
	size_t header, bytes;

	ctx = context_new(BUDGET_BYTES);
	fsimg_test_poison_allocations(ctx, poison);
	header = header_bytes(ctx);
	baseline = memory_charged(ctx);
	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(stream.zalloc != NULL && stream.zfree != NULL);
	CHECK(stream.opaque == &memory);

	for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
		bytes = (size_t)requests[i].items * requests[i].size;
		a = stream.zalloc(stream.opaque, requests[i].items,
		    requests[i].size);
		CHECK(a != NULL);
		CHECK(memory_charged(ctx) == baseline + header + bytes);
		for (size_t j = 0; j < bytes; j++)
			CHECK(a[j] == (poison ? 0xa5 : 0));
		b = stream.zalloc(stream.opaque, requests[i].items,
		    requests[i].size);
		CHECK(b != NULL && b != a);
		stream.zfree(stream.opaque, b);
		stream.zfree(stream.opaque, a);
		CHECK(memory_charged(ctx) == baseline);
	}
	CHECK(memory.error.status == FSIMG_OK);
	fsimg_ctx_free(ctx);
}

/* A budget that cannot hold a request: the limit with its exact figures. */
static void
test_budget_failure(void)
{
	fsimg_zlib_memory_t memory;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline;
	size_t header;
	void *filler;

	ctx = context_new(BUDGET_BYTES);
	header = header_bytes(ctx);
	filler = reserve_all_but(ctx, 64 * 1024);
	baseline = memory_charged(ctx);
	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(stream.zalloc(stream.opaque, 64, 1024) == Z_NULL);
	CHECK(memory_charged(ctx) == baseline);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ELIMIT);
	CHECK(error.status == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(error.limit == BUDGET_BYTES);
	CHECK(error.observed == baseline + header + 64 * 1024);
	fsimg_dealloc(filler);
	fsimg_ctx_free(ctx);
}

/*
 * An injected failure is recorded with its complete diagnostic. A later
 * attempt fails at once without reaching the allocator, and neither it
 * nor freeing an earlier allocation changes the record.
 */
static void
test_injected_failure_is_sticky(void)
{
	fsimg_zlib_memory_t memory;
	fsimg_error_t first, again;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline;
	void *kept;

	ctx = context_new(BUDGET_BYTES);
	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	kept = stream.zalloc(stream.opaque, 1, 100);
	CHECK(kept != NULL);
	baseline = memory_charged(ctx);

	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(stream.zalloc(stream.opaque, 1, 100) == Z_NULL);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(memory_charged(ctx) == baseline);
	CHECK(fsimg_zlib_memory_error(&memory, &first) == FSIMG_ENOMEM);
	CHECK(first.operation == FSIMG_OP_ALLOCATE);

	CHECK(stream.zalloc(stream.opaque, 1, 100) == Z_NULL);
	CHECK(memory_charged(ctx) == baseline);
	stream.zfree(stream.opaque, kept);
	CHECK(fsimg_zlib_memory_error(&memory, &again) == FSIMG_ENOMEM);
	CHECK(memcmp(&first, &again, sizeof(first)) == 0);
	fsimg_ctx_free(ctx);
}

/* Z_MEM_ERROR with no recorded failure is memory exhaustion, not a limit. */
static void
test_unexplained_memory_error(void)
{
	fsimg_zlib_memory_t memory;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	z_stream stream;

	ctx = context_new(BUDGET_BYTES);
	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
	CHECK(error.status == FSIMG_ENOMEM);
	CHECK(error.operation == FSIMG_OP_ALLOCATE);
	CHECK(error.message[0] != '\0');
	CHECK(memory.error.status == FSIMG_OK);
	fsimg_ctx_free(ctx);
}

/*
 * Fail each allocation an initialization makes in turn, until one run
 * makes none fail. Every failed init reports Z_MEM_ERROR with the
 * injected failure, whatever zlib allocated after it, and zlib's own
 * unwinding returns the context to its baseline without an End.
 */
static void
test_init_ordinals(bool deflating)
{
	fsimg_zlib_memory_t memory;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline, attempt;
	int result;

	ctx = context_new(BUDGET_BYTES);
	baseline = memory_charged(ctx);
	for (attempt = 1;; attempt++) {
		bzero(&stream, sizeof(stream));
		fsimg_zlib_memory_bind(&memory, ctx, &stream);
		fsimg_test_fail_alloc_after(ctx, attempt);
		result = deflating ? deflate_start(&stream) :
		    inflate_start(&stream);
		fsimg_test_fail_alloc_after(ctx, 0);
		if (result == Z_OK)
			break;
		CHECK(result == Z_MEM_ERROR);
		CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
		CHECK(error.operation == FSIMG_OP_ALLOCATE);
		CHECK(strcmp(error.message, "allocation failed") == 0);
		CHECK(memory_charged(ctx) == baseline);
	}
	CHECK(attempt > 1);
	CHECK(memory.error.status == FSIMG_OK);
	CHECK(memory_charged(ctx) > baseline);
	CHECK((deflating ? deflateEnd(&stream) : inflateEnd(&stream)) == Z_OK);
	CHECK(memory_charged(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/*
 * inflate allocates its window only once it produces output. Failing that
 * allocation after a successful init reports the injected failure, the
 * stream stays failed, and End releases everything.
 */
static void
test_delayed_window(void)
{
	static unsigned char payload[PAYLOAD_BYTES], packed[PAYLOAD_BYTES];
	static unsigned char out[PAYLOAD_BYTES];
	fsimg_zlib_memory_t memory;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline;
	size_t packed_length;

	ctx = context_new(BUDGET_BYTES);
	payload_fill(payload, sizeof(payload));
	packed_length = compress_payload(ctx, payload, sizeof(payload), packed,
	    sizeof(packed));
	baseline = memory_charged(ctx);

	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(inflate_start(&stream) == Z_OK);
	stream.next_in = packed;
	stream.avail_in = (uInt)packed_length;
	stream.next_out = out;
	stream.avail_out = 4096;
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(inflate(&stream, Z_NO_FLUSH) == Z_MEM_ERROR);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
	CHECK(error.operation == FSIMG_OP_ALLOCATE);

	/* Terminal: with injection off, even a reset stream, which allocates
	 * its window again, gets nothing more from the adapter.
	 */
	CHECK(inflateReset(&stream) == Z_OK);
	stream.next_in = packed;
	stream.avail_in = (uInt)packed_length;
	stream.next_out = out;
	stream.avail_out = 4096;
	CHECK(inflate(&stream, Z_NO_FLUSH) == Z_MEM_ERROR);
	CHECK(fsimg_zlib_memory_error(&memory, &error) == FSIMG_ENOMEM);
	CHECK(error.operation == FSIMG_OP_ALLOCATE);
	CHECK(inflateEnd(&stream) == Z_OK);
	CHECK(memory_charged(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/*
 * Reset keeps a stream, its callbacks and its live memory: two members
 * deflated and inflated through one stream each come out exactly, with
 * bounded output forcing a live inflate window. Further allocations are
 * denied during the second member, proving reset reuses that window.
 */
static void
test_reset_reuse(void)
{
	static unsigned char payload[PAYLOAD_BYTES], packed[PAYLOAD_BYTES];
	static unsigned char out[PAYLOAD_BYTES];
	fsimg_zlib_memory_t deflate_memory, inflate_memory;
	z_stream deflater, inflater;
	fsimg_ctx_t *ctx;
	uint64_t baseline, live, before_window;
	size_t length, half;
	int result;

	ctx = context_new(BUDGET_BYTES);
	payload_fill(payload, sizeof(payload));
	half = sizeof(payload) / 2;
	baseline = memory_charged(ctx);
	bzero(&deflater, sizeof(deflater));
	fsimg_zlib_memory_bind(&deflate_memory, ctx, &deflater);
	CHECK(deflateInit(&deflater, Z_DEFAULT_COMPRESSION) == Z_OK);
	bzero(&inflater, sizeof(inflater));
	fsimg_zlib_memory_bind(&inflate_memory, ctx, &inflater);
	CHECK(inflateInit(&inflater) == Z_OK);

	live = 0;
	for (unsigned member = 0; member < 2; member++) {
		const unsigned char *input = payload + member * half;

		if (member != 0)
			fsimg_test_fail_alloc_after(ctx, 1);
		CHECK(deflateReset(&deflater) == Z_OK);
		deflater.next_in = (Bytef *)(uintptr_t)input;
		deflater.avail_in = (uInt)half;
		deflater.next_out = packed;
		deflater.avail_out = sizeof(packed);
		CHECK(deflate(&deflater, Z_FINISH) == Z_STREAM_END);
		length = (size_t)deflater.total_out;

		CHECK(inflateReset(&inflater) == Z_OK);
		inflater.next_in = packed;
		inflater.avail_in = (uInt)length;
		before_window = memory_charged(ctx);
		inflater.next_out = out;
		do {
			inflater.avail_out = 4096;
			result = inflate(&inflater, Z_NO_FLUSH);
		} while (result == Z_OK);
		CHECK(result == Z_STREAM_END);
		if (member != 0) {
			CHECK(ctx->allocation_attempts == 0);
			fsimg_test_fail_alloc_after(ctx, 0);
		}
		if (member == 0)
			CHECK(memory_charged(ctx) > before_window);
		CHECK(inflater.total_out == half);
		CHECK(memcmp(out, input, half) == 0);

		/* The inflate window, allocated in the first member, stays. */
		if (member == 0)
			live = memory_charged(ctx);
		CHECK(memory_charged(ctx) == live);
		CHECK(inflater.zalloc != NULL && inflater.opaque == &inflate_memory);
	}
	CHECK(live > baseline);
	CHECK(deflate_memory.error.status == FSIMG_OK);
	CHECK(inflate_memory.error.status == FSIMG_OK);
	CHECK(deflateEnd(&deflater) == Z_OK);
	CHECK(inflateEnd(&inflater) == Z_OK);
	CHECK(memory_charged(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/* A failed stream ends; a fresh stream and adapter start without its error. */
static void
test_fresh_bind_after_failure(void)
{
	fsimg_zlib_memory_t memory;
	fsimg_ctx_t *ctx;
	z_stream stream;
	uint64_t baseline;

	ctx = context_new(BUDGET_BYTES);
	baseline = memory_charged(ctx);
	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(deflate_start(&stream) == Z_MEM_ERROR);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(memory.error.status == FSIMG_ENOMEM);
	CHECK(memory_charged(ctx) == baseline);

	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(memory.error.status == FSIMG_OK);
	CHECK(deflate_start(&stream) == Z_OK);
	CHECK(deflateEnd(&stream) == Z_OK);
	CHECK(memory.error.status == FSIMG_OK);
	CHECK(memory_charged(ctx) == baseline);
	fsimg_ctx_free(ctx);
}

/*
 * Two streams draw on one budget: ending one releases only its own
 * memory, and with room left for one deflate stream but not two, the
 * second fails its limit while the first keeps no error.
 */
static void
test_shared_context(void)
{
	fsimg_zlib_memory_t first_memory, second_memory;
	z_stream first, second;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	uint64_t baseline, one_stream;
	void *filler;

	ctx = context_new(BUDGET_BYTES);
	baseline = memory_charged(ctx);
	bzero(&first, sizeof(first));
	fsimg_zlib_memory_bind(&first_memory, ctx, &first);
	CHECK(deflate_start(&first) == Z_OK);
	one_stream = memory_charged(ctx) - baseline;
	bzero(&second, sizeof(second));
	fsimg_zlib_memory_bind(&second_memory, ctx, &second);
	CHECK(deflate_start(&second) == Z_OK);
	CHECK(memory_charged(ctx) == baseline + 2 * one_stream);
	CHECK(deflateEnd(&second) == Z_OK);
	CHECK(memory_charged(ctx) == baseline + one_stream);
	CHECK(deflateEnd(&first) == Z_OK);
	CHECK(memory_charged(ctx) == baseline);

	filler = reserve_all_but(ctx, one_stream + one_stream / 2);
	baseline = memory_charged(ctx);
	bzero(&first, sizeof(first));
	fsimg_zlib_memory_bind(&first_memory, ctx, &first);
	CHECK(deflate_start(&first) == Z_OK);
	bzero(&second, sizeof(second));
	fsimg_zlib_memory_bind(&second_memory, ctx, &second);
	CHECK(deflate_start(&second) == Z_MEM_ERROR);
	CHECK(fsimg_zlib_memory_error(&second_memory, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(error.limit == BUDGET_BYTES);
	CHECK(error.observed > BUDGET_BYTES);
	CHECK(first_memory.error.status == FSIMG_OK);
	CHECK(memory_charged(ctx) == baseline + one_stream);
	CHECK(deflateEnd(&first) == Z_OK);
	CHECK(memory_charged(ctx) == baseline);
	fsimg_dealloc(filler);
	fsimg_ctx_free(ctx);
}

/* zlib initializes what it reads: poisoned storage changes no output byte. */
static void
test_poisoned_output(void)
{
	static unsigned char payload[PAYLOAD_BYTES];
	static unsigned char plain[PAYLOAD_BYTES], poisoned[PAYLOAD_BYTES];
	static unsigned char out[PAYLOAD_BYTES];
	fsimg_zlib_memory_t memory;
	fsimg_ctx_t *ctx;
	z_stream stream;
	size_t plain_length, poisoned_length;

	payload_fill(payload, sizeof(payload));
	ctx = context_new(BUDGET_BYTES);
	plain_length = compress_payload(ctx, payload, sizeof(payload), plain,
	    sizeof(plain));
	fsimg_test_poison_allocations(ctx, true);
	poisoned_length = compress_payload(ctx, payload, sizeof(payload),
	    poisoned, sizeof(poisoned));
	CHECK(poisoned_length == plain_length);
	CHECK(memcmp(poisoned, plain, plain_length) == 0);

	bzero(&stream, sizeof(stream));
	fsimg_zlib_memory_bind(&memory, ctx, &stream);
	CHECK(inflate_start(&stream) == Z_OK);
	stream.next_in = poisoned;
	stream.avail_in = (uInt)poisoned_length;
	stream.next_out = out;
	stream.avail_out = sizeof(out);
	CHECK(inflate(&stream, Z_FINISH) == Z_STREAM_END);
	CHECK(stream.total_out == sizeof(payload));
	CHECK(memcmp(out, payload, sizeof(payload)) == 0);
	CHECK(inflateEnd(&stream) == Z_OK);
	fsimg_ctx_free(ctx);
}

static fsimg_status_t
volume_read(void *argument FSIMG_UNUSED, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	for (size_t i = 0; i < length; i++)
		((unsigned char *)buffer)[i] = (unsigned char)((offset + i) % 251);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_size(void *argument FSIMG_UNUSED, uint64_t *length,
    fsimg_error_t *error FSIMG_UNUSED)
{
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

	ctx = context_new(BUDGET_BYTES);
	fsimg_io_ops_init(&input_ops);
	input_ops.read_at = volume_read;
	input_ops.get_size = volume_size;
	CHECK(fsimg_io_from_callbacks(ctx, &input_ops, NULL, &input, NULL) ==
	    FSIMG_OK);
	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(null != -1);
	baseline = memory_charged(ctx);
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
			CHECK(memory_charged(ctx) == baseline);
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
		ctx = context_new(BUDGET_BYTES);
		CHECK(fsimg_io_from_callbacks(ctx, &input_ops, NULL, &input,
		    NULL) == FSIMG_OK);
		baseline = memory_charged(ctx);
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
			CHECK(memory_charged(ctx) == baseline);
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

int
main(void)
{
	test_direct_allocations(false);
	test_direct_allocations(true);
	test_budget_failure();
	test_injected_failure_is_sticky();
	test_unexplained_memory_error();
	test_init_ordinals(false);
	test_init_ordinals(true);
	test_delayed_window();
	test_reset_reuse();
	test_fresh_bind_after_failure();
	test_shared_context();
	test_poisoned_output();
	test_encoder_sweeps();
	test_encoder_budgets();
	return (0);
}
