/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "test.h"

typedef enum script_action {
	SCRIPT_NORMAL,
	SCRIPT_SHORT,
	SCRIPT_EINTR,
	SCRIPT_PARTIAL_EINTR,
	SCRIPT_EOF,
	SCRIPT_ERROR,
	SCRIPT_PARTIAL_ERROR,
	SCRIPT_OVERREPORT
} script_action_t;

typedef struct memory_store {
	unsigned char	 data[128];
	uint64_t	 length;
	uint64_t	 last_offset;
	script_action_t	 actions[8];
	unsigned	 calls;
	unsigned	 flushes;
	bool		 flush_fails;
	bool		 virtual_storage;
	unsigned	 zero_calls;
	bool		 zero_fails;
	bool		 cancel_after_zero;
	bool		 always_interrupt;
} memory_store_t;

static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_flush(void *, fsimg_error_t *);
static fsimg_status_t memory_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t scripted_transfer(memory_store_t *, uint64_t, void *,
    size_t, size_t *, bool, fsimg_error_t *);
static void test_callbacks(void);
static void test_descriptors(void);
static void test_zero_failures(void);
static void test_interrupt_limit(void);
static fsimg_status_t memory_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static bool zero_cancelled(void *);

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_store_t *store = argument;

	*length = store->length;
	return (FSIMG_OK);
}

static fsimg_status_t
scripted_transfer(memory_store_t *store, uint64_t offset, void *buffer,
    size_t length, size_t *count, bool writing, fsimg_error_t *error)
{
	script_action_t action;

	CHECK(offset <= store->length && length <= store->length - offset);
	store->last_offset = offset;
	action = store->calls < 8 ? store->actions[store->calls] : SCRIPT_NORMAL;
	if (store->always_interrupt)
		action = SCRIPT_EINTR;
	store->calls++;
	*count = length;
	if (action == SCRIPT_OVERREPORT) {
		*count = length + 1;
		return (FSIMG_OK);
	}
	if (action == SCRIPT_SHORT || action == SCRIPT_PARTIAL_EINTR ||
	    action == SCRIPT_PARTIAL_ERROR)
		*count = length > 1 ? 1 : length;
	if (action == SCRIPT_EOF || action == SCRIPT_EINTR || action == SCRIPT_ERROR)
		*count = 0;
	if (!store->virtual_storage) {
		CHECK(offset + *count <= sizeof(store->data));
		if (writing)
			bcopy(buffer, store->data + offset, *count);
		else
			bcopy(store->data + offset, buffer, *count);
	}
	if (action == SCRIPT_EINTR || action == SCRIPT_PARTIAL_EINTR)
		return (fsimg_error_set(error, FSIMG_EIO, EINTR,
		    FSIMG_OP_IO, "scripted interruption"));
	if (action == SCRIPT_ERROR || action == SCRIPT_PARTIAL_ERROR)
		return (fsimg_error_set(error, FSIMG_EIO, ENOSPC,
		    FSIMG_OP_IO, "scripted failure"));
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	return (scripted_transfer(argument, offset, buffer, length, count,
	    false, error));
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	return (scripted_transfer(argument, offset, (void *)buffer, length,
	    count, true, error));
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error)
{
	memory_store_t *store = argument;

	store->flushes++;
	if (store->flush_fails)
		return (fsimg_error_set(error, FSIMG_EIO, EIO,
		    FSIMG_OP_IO, "scripted flush failure"));
	return (FSIMG_OK);
}

static fsimg_status_t
memory_resize(void *argument, uint64_t length, fsimg_error_t *error)
{
	memory_store_t *store = argument;

	if (length > sizeof(store->data))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
		    "memory capacity exceeded"));
	store->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_zero(void *argument, uint64_t offset FSIMG_UNUSED,
    uint64_t length FSIMG_UNUSED, fsimg_error_t *error)
{
	memory_store_t *store = argument;

	store->zero_calls++;
	if (store->zero_fails)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "zero callback failed after possible mutation"));
	return (FSIMG_OK);
}

static bool
zero_cancelled(void *argument)
{
	memory_store_t *store = argument;

	return ((store->cancel_after_zero && store->zero_calls != 0) ||
	    (store->always_interrupt && store->calls >= 1026));
}

static void
test_interrupt_limit(void)
{
	memory_store_t store = { .length = 128, .always_interrupt = true };
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	unsigned char byte;
	size_t count;

	/* Cancellation bounds the test independently of the retry allowance. */
	fsimg_options_init(&options);
	options.cancelled = zero_cancelled;
	options.cancel_argument = &store;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, NULL) == FSIMG_OK);
	CHECK(fsimg_io_read_exact(io, 0, &byte, 1, &count, &error) == FSIMG_EIO);
	CHECK(error.os_error == EINTR && count == 0);
	CHECK(store.calls <= 1025);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
}

static void
test_zero_failures(void)
{
	memory_store_t store = { .length = 131072, .virtual_storage = true };
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;

	fsimg_options_init(&options);
	options.cancelled = zero_cancelled;
	options.cancel_argument = &store;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.write_at = memory_write;
	ops.zero_range = memory_zero;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, NULL) == FSIMG_OK);
	store.cancel_after_zero = true;
	CHECK(fsimg_io_zero_range(io, 0, store.length, &error) == FSIMG_ECANCELLED);
	CHECK(store.zero_calls == 1);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	store.cancel_after_zero = false;
	store.zero_fails = true;
	CHECK(fsimg_io_zero_range(io, 0, 1, &error) == FSIMG_EIO);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
}

static void
test_callbacks(void)
{
	memory_store_t store = { .length = 128 };
	fsimg_io_ops_t ops;
	fsimg_error_t error;
	fsimg_stats_t before, after;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io, *slice, *nested, *bad;
	unsigned char buffer[128];
	size_t count;
	uint64_t length;
	unsigned calls;

	CHECK(fsimg_ctx_new(NULL, &ctx, &error) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &before, &error) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.max_transfer = 7;
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, NULL) == FSIMG_ENOMEM);
	CHECK(io == NULL);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, &error) == FSIMG_OK);
	CHECK(fsimg_io_resize(io, 128, &error) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_flush(io, false, &error) == FSIMG_OK);
	CHECK(fsimg_io_flush(io, true, &error) == FSIMG_EUNSUPPORTED);
	CHECK(fsimg_io_slice(io, 20, 40, &slice, &error) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_io_slice(slice, 0, 1, &bad, &error) == FSIMG_ENOMEM);
	CHECK(bad == NULL);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_io_zero_range(slice, 0, 1, &error) == FSIMG_ENOMEM);
	CHECK(ctx->stats.memory_bytes == after.memory_bytes);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(fsimg_io_slice(slice, 10, 10, &nested, &error) == FSIMG_OK);
	CHECK(fsimg_io_resize(nested, 1, &error) == FSIMG_EUNSUPPORTED);
	memset(buffer, 0x91, sizeof(buffer));
	CHECK(fsimg_io_write_exact(nested, 0, buffer, 10, &count, &error) == FSIMG_OK);
	CHECK(count == 10 && store.last_offset == 37);
	CHECK(store.data[29] == 0 && store.data[30] == 0x91 && store.data[40] == 0);
	calls = store.calls;
	CHECK(fsimg_io_read_at(nested, 10, NULL, 0, &count, &error) == FSIMG_OK);
	CHECK(store.calls == calls);
	CHECK(fsimg_io_read_at(nested, 10, buffer, 1, &count, &error) == FSIMG_ERANGE);
	CHECK(count == 0 && store.calls == calls);
	CHECK(fsimg_io_read_at(nested, UINT64_MAX, buffer, 2, &count, NULL) == FSIMG_ERANGE);
	CHECK(fsimg_io_slice(nested, UINT64_MAX, 1, &bad, &error) == FSIMG_ERANGE);
	CHECK(bad == NULL && store.calls == calls);
	CHECK(fsimg_io_zero_range(nested, 1, 8, &error) == FSIMG_OK);
	CHECK(store.data[30] == 0x91 && store.data[39] == 0x91);
	for (size_t i = 31; i < 39; i++)
		CHECK(store.data[i] == 0);
	fsimg_io_free(nested);
	fsimg_io_free(slice);

	/* Exercise all confirmed-progress/error combinations with independent data. */
	for (size_t i = 0; i < sizeof(store.data); i++)
		store.data[i] = (unsigned char)i;
	store.calls = 0;
	store.actions[0] = SCRIPT_EINTR;
	store.actions[1] = SCRIPT_SHORT;
	store.actions[2] = SCRIPT_PARTIAL_EINTR;
	CHECK(fsimg_io_read_exact(io, 0, buffer, 30, &count, NULL) == FSIMG_OK);
	CHECK(count == 30 && bcmp(buffer, store.data, 30) == 0);
	bzero(store.actions, sizeof(store.actions));
	store.calls = 0;
	store.actions[0] = SCRIPT_SHORT;
	store.actions[1] = SCRIPT_PARTIAL_ERROR;
	CHECK(fsimg_io_write_exact(io, 0, buffer, 30, &count, &error) == FSIMG_EIO);
	CHECK(count == 2 && error.os_error == ENOSPC && error.offset == 2);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	store.calls = 0;
	store.actions[0] = SCRIPT_EOF;
	CHECK(fsimg_io_read_at(io, 0, buffer, 30, &count, &error) == FSIMG_OK);
	CHECK(count == 0);
	store.calls = 0;
	CHECK(fsimg_io_read_exact(io, 0, buffer, 30, &count, &error) == FSIMG_EIO);
	store.calls = 0;
	CHECK(fsimg_io_write_exact(io, 0, buffer, 30, &count, &error) == FSIMG_EIO);
	store.calls = 0;
	store.actions[0] = SCRIPT_OVERREPORT;
	CHECK(fsimg_io_read_at(io, 0, buffer, 1, &count, &error) == FSIMG_EIO);
	CHECK(count == 0);
	fsimg_io_free(io);
	ops.resize = memory_resize;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, &error) == FSIMG_OK);
	CHECK(fsimg_io_slice(io, 0, 8, &slice, &error) == FSIMG_OK);
	CHECK(fsimg_io_resize(slice, 8, &error) == FSIMG_EUNSUPPORTED);
	CHECK(store.length == 128);
	CHECK(fsimg_io_resize(io, 200, &error) == FSIMG_ERANGE);
	CHECK(fsimg_io_get_size(io, &length, &error) == FSIMG_OK && length == 128);
	CHECK(fsimg_io_resize(io, 4, &error) == FSIMG_OK);
	CHECK(fsimg_io_read_at(slice, 0, buffer, 1, &count, NULL) == FSIMG_ERANGE);
	CHECK(fsimg_io_resize(io, 128, &error) == FSIMG_OK);
	fsimg_io_free(slice);
	fsimg_io_free(io);
	ops.resize = NULL;

	bzero(store.actions, sizeof(store.actions));
	store.calls = 0;
	store.length = UINT64_MAX;
	store.virtual_storage = true;
	ops.flush = memory_flush;
	ops.flags = FSIMG_IO_DURABLE;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &store, &io, &error) == FSIMG_OK);
	CHECK(fsimg_io_slice(io, UINT64_MAX - 10, 10, &slice, &error) == FSIMG_OK);
	CHECK(fsimg_io_read_exact(slice, 9, buffer, 1, &count, &error) == FSIMG_OK);
	CHECK(store.last_offset == UINT64_MAX - 1);
	CHECK(fsimg_io_get_size(slice, &length, &error) == FSIMG_OK && length == 10);
	CHECK(fsimg_io_flush(slice, true, &error) == FSIMG_OK);
	store.flush_fails = true;
	CHECK(fsimg_io_flush(io, true, &error) == FSIMG_EIO);
	CHECK(store.flushes == 2);
	fsimg_io_free(slice);
	fsimg_io_free(io);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(before.memory_bytes == after.memory_bytes && after.descriptors == 0);
	fsimg_ctx_free(ctx);
}

static void
test_descriptors(void)
{
	char path[] = "io-fixture-XXXXXX";
	unsigned char buffer[16];
	fsimg_options_t options;
	fsimg_error_t error;
	fsimg_stats_t before, after;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io, *other;
	size_t count;
	uint64_t length;
	int fd, flags, readonly;

	fd = mkstemp(path);
	CHECK(fd != -1);
	readonly = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(readonly != -1);
	CHECK(unlink(path) == 0);
	CHECK(ftruncate(fd, 128) == 0);
	CHECK(lseek(fd, 11, SEEK_SET) == 11);
	fsimg_options_init(&options);
	options.max_open_files = 1;
	CHECK(fsimg_ctx_new(&options, &ctx, &error) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &before, &error) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, &error) == FSIMG_EINVAL);
	flags = fcntl(fd, F_GETFL);
	CHECK(fcntl(fd, F_SETFL, flags | O_APPEND) == 0);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_WRITE, &io, &error) == FSIMG_EINVAL);
	CHECK((fcntl(fd, F_GETFL) & O_APPEND) != 0);
	CHECK(fcntl(fd, F_SETFL, flags) == 0);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_WRITE, &io, NULL) == FSIMG_ENOMEM);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, &io, &error) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, readonly, FSIMG_IO_READ, &other, &error) == FSIMG_ELIMIT);
	CHECK(other == NULL);
	memset(buffer, 0xa7, sizeof(buffer));
	CHECK(fsimg_io_write_exact(io, 7, buffer, sizeof(buffer), &count, &error) == FSIMG_OK);
	CHECK(lseek(fd, 0, SEEK_CUR) == 11);
	CHECK(fsimg_io_zero_range(io, 0, 128, &error) == FSIMG_OK);
	CHECK(fsimg_io_read_exact(io, 7, buffer, sizeof(buffer), &count, &error) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(buffer); i++)
		CHECK(buffer[i] == 0);
	CHECK(fsimg_io_resize(io, UINT64_MAX, &error) == FSIMG_ERANGE);
	CHECK(fsimg_io_get_size(io, &length, &error) == FSIMG_OK && length == 128);
	CHECK(fsimg_io_resize(io, 8, &error) == FSIMG_OK);
	CHECK(fsimg_io_flush(io, true, &error) == FSIMG_OK);
	fsimg_io_free(io);
	CHECK(fsimg_fd_reserve(ctx, &error) == FSIMG_OK);
	flags = dup(readonly);
	CHECK(flags != -1);
	fsimg_test_fail_alloc_after(ctx, 1);
	CHECK(fsimg_io_adopt_fd(ctx, flags, FSIMG_IO_READ, &io, &error) == FSIMG_ENOMEM);
	CHECK(fcntl(flags, F_GETFD) == -1 && errno == EBADF);
	CHECK(ctx->stats.descriptors == 0);
	fsimg_test_fail_alloc_after(ctx, 0);
	CHECK(fsimg_fd_reserve(ctx, &error) == FSIMG_OK);
	flags = dup(readonly);
	CHECK(flags != -1);
	CHECK(fsimg_io_adopt_fd(ctx, flags, FSIMG_IO_READ, &io, &error) == FSIMG_OK);
	fsimg_io_free(io);
	CHECK(fcntl(flags, F_GETFD) == -1 && errno == EBADF);
	CHECK(fsimg_io_from_fd(ctx, readonly, FSIMG_IO_READ, &io, &error) == FSIMG_OK);
	CHECK(close(readonly) == 0);
	CHECK(fsimg_io_get_size(io, &length, &error) == FSIMG_OK && length == 8);
	CHECK(ftruncate(fd, 0) == 0);
	CHECK(fsimg_io_read_exact(io, 0, buffer, 1, &count, &error) == FSIMG_EIO);
	fsimg_io_free(io);
	CHECK(close(fd) == 0);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes && after.descriptors == 0);
	fsimg_ctx_free(ctx);
}

int
main(void)
{
	test_callbacks();
	test_descriptors();
	test_zero_failures();
	test_interrupt_limit();
	return (0);
}
