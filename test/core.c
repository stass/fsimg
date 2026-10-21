/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "digest.h"
#include "host.h"
#include "internal.h"
#include "test.h"

typedef struct sha256_vector {
	const char	*message;
	const char	*expected;
} sha256_vector_t;

typedef struct callback_state {
	fsimg_ctx_t	*ctx;
	unsigned	 calls;
	bool		 cancel;
} callback_state_t;

static void test_arithmetic(void);
static void test_context(void);
static void test_descriptor_cleanup(void);
static void test_sha256(void);
static bool cancel_callback(void *);
static void progress_callback(void *, const fsimg_progress_t *);

static void
test_arithmetic(void)
{
	const uint64_t values[] = { 0, 1, 2, 7, UINT32_MAX, INT64_MAX,
	    UINT64_MAX - 1, UINT64_MAX };
	uint64_t result;
	off_t offset;
	size_t size;
	bool valid;

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
			result = 19;
			valid = fsimg_add_u64(values[i], values[j], &result);
			if (valid) {
				CHECK(result >= values[i]);
				CHECK(result - values[i] == values[j]);
			} else {
				CHECK(values[i] > UINT64_MAX - values[j]);
				CHECK(result == 19);
			}
			result = 19;
			valid = fsimg_mul_u64(values[i], values[j], &result);
			if (valid) {
				if (values[i] == 0 || values[j] == 0)
					CHECK(result == 0);
				else {
					CHECK(result / values[i] == values[j]);
					CHECK(result % values[i] == 0);
				}
			} else {
				CHECK(values[i] != 0);
				CHECK(values[j] > UINT64_MAX / values[i]);
				CHECK(result == 19);
			}
		}
	}
	CHECK(fsimg_host_offset(INT64_MAX, &offset));
	CHECK(offset == INT64_MAX);
	CHECK(!fsimg_host_offset((uint64_t)INT64_MAX + 1, &offset));
	CHECK(fsimg_to_size(SIZE_MAX, &size));
	CHECK(size == SIZE_MAX);
	CHECK(fsimg_range_fits(UINT64_MAX, 0, UINT64_MAX));
	CHECK(!fsimg_range_fits(UINT64_MAX, 1, UINT64_MAX));
	result = UINT64_MAX - 1;
	fsimg_stat_add(&result, 2);
	CHECK(result == UINT64_MAX);
}

static void
test_descriptor_cleanup(void)
{
	struct rlimit limit;
	pid_t child;
	int fd, high_fd, minimum, result, wait_status;

	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
		minimum = limit.rlim_cur > 64 ? 32 : (int)(limit.rlim_cur / 2);
		if (minimum < 6)
			_exit(77);
		fd = open(".", O_RDONLY);
		CHECK(fd != -1);
		high_fd = fcntl(fd, F_DUPFD_CLOEXEC, minimum);
		CHECK(high_fd >= minimum);
		CHECK(close(fd) == 0);
		limit.rlim_cur = (rlim_t)minimum;
		CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
		result = fsimg_host_closefrom(6);
		if (result == 0)
			CHECK(fcntl(high_fd, F_GETFD) == -1 && errno == EBADF);
		/* Failure is safe: the worker must refuse startup, not proceed. */
		_exit(0);
	}
	CHECK(waitpid(child, &wait_status, 0) == child);
	if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 77) {
		puts("SKIP: descriptor cleanup regression needs at least 12 FD slots");
		return;
	}
	CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
}

static bool
cancel_callback(void *argument)
{
	callback_state_t *state = argument;

	return (state->cancel);
}

static void
progress_callback(void *argument, const fsimg_progress_t *progress)
{
	callback_state_t *state = argument;
	fsimg_stats_t stats;
	fsimg_error_t error;

	CHECK(progress->phase != NULL);
	CHECK(fsimg_ctx_get_stats(state->ctx, &stats, &error) == FSIMG_ESTATE);
	/* A mistaken destructor in a callback must not destroy active storage. */
	fsimg_ctx_free(state->ctx);
	state->calls++;
}

static void
test_context(void)
{
	fsimg_options_t options, zeroed;
	fsimg_error_t error;
	fsimg_stats_t before, after;
	fsimg_session_t session, other;
	callback_state_t callbacks = { 0 };
	fsimg_ctx_t *ctx;
	void *a, *b;
	int directory;

	fsimg_options_init(&options);
	options.memory_bytes = 1;
	CHECK(fsimg_ctx_new(&options, &ctx, &error) == FSIMG_EINVAL);
	CHECK(ctx == NULL);
	options.memory_bytes = 0;
	options.reserved[0] = 1;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_EINVAL);
	options.reserved[0] = 0;
	options.size--;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_EINVAL);
	options.size++;
	/* Zero-filled options name descriptor 0 as the scratch directory; their
	 * size refuses them before it is duplicated.
	 */
	bzero(&zeroed, sizeof(zeroed));
	CHECK(fsimg_ctx_new(&zeroed, &ctx, &error) == FSIMG_EINVAL && ctx == NULL);
	CHECK(strcmp(error.message, "unsupported options size") == 0);
	options.version++;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_EINVAL);
	options.version--;
	options.flags = 1;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_EINVAL);
	options.flags = 0;
	options.max_entries = 10;
	options.max_work = 20;
	options.max_output_bytes = 64;
	options.max_open_files = 1;
	options.progress = progress_callback;
	options.progress_argument = &callbacks;
	options.cancelled = cancel_callback;
	options.cancel_argument = &callbacks;
	directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(directory != -1);
	options.scratch_directory_fd = directory;
	CHECK(fsimg_ctx_new(&options, &ctx, &error) == FSIMG_OK);
	callbacks.ctx = ctx;
	CHECK(ctx->options.scratch_directory_fd != directory);
	CHECK((fcntl(ctx->options.scratch_directory_fd, F_GETFD) & FD_CLOEXEC) != 0);
	CHECK(close(directory) == 0);
	CHECK(fsimg_ctx_get_stats(ctx, &before, &error) == FSIMG_OK);
	CHECK(before.descriptors == 1);
	CHECK(fsimg_fd_reserve(ctx, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DESCRIPTORS);

	a = fsimg_alloc(ctx, 1024, &error);
	CHECK(a != NULL);
	CHECK((uintptr_t)a % _Alignof(max_align_t) == 0);
	for (size_t i = 0; i < 1024; i++)
		CHECK(((unsigned char *)a)[i] == 0);
	fsimg_test_fail_alloc_after(ctx, 1);
	b = fsimg_alloc(ctx, 32, &error);
	CHECK(b == NULL && error.status == FSIMG_ENOMEM);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(after.memory_bytes > before.memory_bytes);
	fsimg_test_fail_alloc_after(ctx, 0);
	b = fsimg_alloc(ctx, (size_t)ctx->options.memory_bytes, &error);
	CHECK(b == NULL && error.status == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CHECK(fsimg_alloc(ctx, SIZE_MAX, &error) == NULL);
	CHECK(error.status == FSIMG_ERANGE);
	fsimg_dealloc(a);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);

	fsimg_session_init(&session, ctx, "catalog", 128);
	fsimg_session_init(&other, ctx, "other catalog", 128);
	for (unsigned i = 0; i < 10; i++)
		CHECK(fsimg_session_charge(&session, 1, 2, 1, &error) == FSIMG_OK);
	CHECK(callbacks.calls == 10);
	CHECK(fsimg_session_charge(&session, 1, 0, 0, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_ENTRIES);
	CHECK(fsimg_session_charge(&other, 1, 0, 0, &error) == FSIMG_OK);
	CHECK(fsimg_session_charge(&session, 0, 45, 0, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_OUTPUT);
	CHECK(fsimg_session_charge(&session, 0, 0, 11, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	CHECK(session.progress.entries == 10 && session.output_bytes == 20);
	callbacks.cancel = true;
	CHECK(fsimg_session_charge(&session, 0, 0, 0, &error) == FSIMG_ECANCELLED);
	CHECK(session.progress.work == 10);
	fsimg_ctx_free(ctx);
	fsimg_ctx_free(NULL);
	CHECK(fsimg_ctx_new(NULL, &ctx, &error) == FSIMG_OK);
	CHECK(error.status == FSIMG_OK);
	fsimg_session_init(&session, ctx, "extract", 128);
	CHECK(fsimg_session_charge(&session, 0, 129, 0, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_OUTPUT && error.limit == 128);
	fsimg_ctx_free(ctx);
}

/* FIPS 180-4 example vectors: one block, a 448-bit message whose padding
 * needs a second block, and a million bytes absorbed in uneven pieces that
 * straddle block boundaries.
 */
static void
test_sha256(void)
{
	static const sha256_vector_t vectors[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" }
	};
	char hex[SHA256_HEX_BYTES], chunk[7];
	sha256_t digest;
	unsigned take;

	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		sha256_init(&digest);
		sha256_update(&digest, vectors[i].message, strlen(vectors[i].message));
		sha256_final_hex(&digest, hex);
		if (strcmp(hex, vectors[i].expected) != 0)
			fprintf(stderr, "sha256(\"%s\") = %s\n", vectors[i].message, hex);
		CHECK(strcmp(hex, vectors[i].expected) == 0);
	}

	memset(chunk, 'a', sizeof(chunk));
	sha256_init(&digest);
	for (unsigned remaining = 1000000; remaining > 0; remaining -= take) {
		take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		sha256_update(&digest, chunk, take);
	}
	sha256_final_hex(&digest, hex);
	CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

int
main(void)
{
	test_arithmetic();
	test_context();
	test_descriptor_cleanup();
	test_sha256();
	return (0);
}
