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
static void test_sha256_split_updates(void);
static void test_sha256_vectors(void);
static void check_render(const char *, const bool *, size_t, const char *,
    bool);
static void test_path_render(void);
static void test_rendered_tail(void);
static void loss_callback(void *, const char *, uint32_t);
static void test_loss_rendering(void);
static bool cancel_callback(void *);
static void progress_callback(void *, const fsimg_progress_t *);
static void sha256_hex_of(const void *, size_t, char *);
static bool sha256_hex_matches(const char *, const char *, const char *);

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

/* SHA-256 of a message absorbed in one update, as lowercase hexadecimal. */
static void
sha256_hex_of(const void *message, size_t length, char *hex)
{
	fsimg_sha256_t digest;

	fsimg_sha256_init(&digest);
	fsimg_sha256_update(&digest, message, length);
	sha256_final_hex(&digest, hex);
}

/* Compare a digest with its expectation, naming the message on mismatch. */
static bool
sha256_hex_matches(const char *what, const char *hex, const char *expected)
{
	if (strcmp(hex, expected) == 0)
		return (true);
	fprintf(stderr, "sha256 of %s = %s, expected %s\n", what, hex,
	    expected);
	return (false);
}

/* The FIPS 180-4 example messages: the empty message, "abc" in one block,
 * the 448-bit message whose padding needs a second block, the 896-bit
 * message, and one million 'a' characters absorbed in 7-byte pieces that
 * straddle block boundaries. The expected digests are NIST's. A comparison
 * that accepted a wrong digest would prove nothing, so a corrupted
 * expectation must fail it.
 */
static void
test_sha256_vectors(void)
{
	static const sha256_vector_t vectors[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
		{ "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
		    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
		    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" }
	};
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH], chunk[7];
	fsimg_sha256_t digest;
	unsigned take;

	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		sha256_hex_of(vectors[i].message, strlen(vectors[i].message),
		    hex);
		CHECK(sha256_hex_matches(vectors[i].message, hex,
		    vectors[i].expected));
	}

	sha256_hex_of("abc", 3, hex);
	CHECK(!sha256_hex_matches("abc (this mismatch is expected)", hex,
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae"));

	memset(chunk, 'a', sizeof(chunk));
	fsimg_sha256_init(&digest);
	for (unsigned remaining = 1000000; remaining > 0; remaining -= take) {
		take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		fsimg_sha256_update(&digest, chunk, take);
	}
	sha256_final_hex(&digest, hex);
	CHECK(sha256_hex_matches("one million 'a'", hex,
	    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

/* A multi-block message (bytes 0, 1, 2, ...) absorbed in two updates, cut
 * at every offset 0..64. Each cut leaves a different number of bytes
 * pending in the block buffer, so the second update completes that block,
 * compresses whole blocks straight from its input and buffers the rest.
 * The 192-byte message is three whole blocks, so the second update ends
 * exactly on a block boundary; the 200-byte message leaves an 8-byte tail.
 * An empty update passing NULL between the pieces must change nothing.
 * Every cut must give the digest Python's hashlib computes for the whole
 * message.
 */
static void
test_sha256_split_updates(void)
{
	static const struct {
		size_t		 length;
		const char	*expected;
	} messages[] = {
		{ 192, "8b4a544837a1a0280fa8a7c82865c27a1064b3cc6281fda0753566b9bb104a87" },
		{ 200, "1901da1c9f699b48f6b2636e65cbf73abf99d0441ef67f5c540a42f7051dec6f" }
	};
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH], what[48];
	unsigned char message[200];
	fsimg_sha256_t digest;
	size_t length;

	for (size_t i = 0; i < sizeof(message); i++)
		message[i] = (unsigned char)i;

	for (size_t m = 0; m < sizeof(messages) / sizeof(messages[0]); m++) {
		length = messages[m].length;
		CHECK(length <= sizeof(message));
		for (size_t cut = 0; cut <= FSIMG_SHA256_BLOCK_LENGTH; cut++) {
			fsimg_sha256_init(&digest);
			fsimg_sha256_update(&digest, message, cut);
			fsimg_sha256_update(&digest, NULL, 0);
			fsimg_sha256_update(&digest, message + cut,
			    length - cut);
			sha256_final_hex(&digest, hex);
			snprintf(what, sizeof(what), "bytes 0..%zu cut at %zu",
			    length - 1, cut);
			CHECK(sha256_hex_matches(what, hex,
			    messages[m].expected));
		}
	}
}

/* Render path, with components classed by omitted, into a buffer of size
 * bytes and expect exactly the rendering and truncation flag given. The
 * buffer is poisoned first, so a byte written past the terminator shows.
 */
static void
check_render(const char *path, const bool *omitted, size_t size,
    const char *expected, bool truncated)
{
	char out[64];

	CHECK(size <= sizeof(out));
	memset(out, 0x5a, sizeof(out));
	if (fsimg_path_render(path, omitted, out, size) != truncated ||
	    strcmp(out, expected) != 0) {
		fprintf(stderr, "rendering into %zu bytes: expected \"%s\"%s, "
		    "got \"%s\"\n", size, expected, truncated ?
		    " truncated" : "", out);
		CHECK(!"rendering differs");
	}
	for (size_t i = strlen(expected) + 1; i < sizeof(out); i++)
		CHECK(out[i] == 0x5a);
}

/* The one rendering every library path passes through. A backslash is
 * always doubled; an omitted component also writes every byte outside
 * 0x20 to 0x7e as \xNN; a delivered component is otherwise unchanged. The
 * expected strings are written out by hand from that rule, independently
 * of the implementation's unit scanner.
 */
static void
test_path_render(void)
{
	static const bool second_omitted[] = { false, true };
	static const bool first_omitted[] = { true, false };

	/* A plain path is all delivered: only backslashes change. */
	check_render("/dir/name", NULL, 64, "/dir/name", false);
	check_render("/a\\b", NULL, 64, "/a\\\\b", false);
	check_render("/caf\xc3\xa9/\x01", NULL, 64, "/caf\xc3\xa9/\x01", false);
	check_render("relative/name", NULL, 64, "relative/name", false);

	/* Omitted components escape everything outside printable ASCII,
	 * DEL included; printable bytes and later components stay as they
	 * are.
	 */
	check_render("/dir/a\xe9", second_omitted, 64, "/dir/a\\xe9", false);
	check_render("/\x01\x7f~ /next", first_omitted, 64,
	    "/\\x01\\x7f~ /next", false);
	check_render("/a\\\xff", first_omitted, 64, "/a\\\\\\xff", false);

	/* Injectivity: a delivered name spelling "a\xe9" and an omitted
	 * name "a" followed by the byte 0xe9 render apart.
	 */
	check_render("/a\\xe9", NULL, 64, "/a\\\\xe9", false);
	check_render("/a\xe9", first_omitted, 64, "/a\\xe9", false);

	/* A rendering too long keeps its tail and drops whole units from
	 * the front: never part of an escape or of a UTF-8 sequence. Here
	 * "/\x01\x02" is nine bytes; six bytes hold five and the NUL, so
	 * the separator and the first escape go.
	 */
	check_render("/\x01\x02", first_omitted, 10, "/\\x01\\x02", false);
	check_render("/\x01\x02", first_omitted, 9, "\\x01\\x02", true);
	check_render("/\x01\x02", first_omitted, 8, "\\x02", true);
	check_render("/\x01\x02", first_omitted, 6, "\\x02", true);
	check_render("/\x01\x02", first_omitted, 5, "\\x02", true);
	check_render("/\x01\x02", first_omitted, 4, "", true);
	check_render("/\xc3\xa9\xc3\xa9\xc3\xa9", NULL, 4, "\xc3\xa9", true);
	check_render("/\xc3\xa9\xc3\xa9\xc3\xa9", NULL, 5, "\xc3\xa9\xc3\xa9",
	    true);
	check_render("/\\\\", NULL, 4, "\\\\", true);
	check_render("/\\\\", NULL, 3, "\\\\", true);
	check_render("/\\\\", NULL, 2, "", true);
	check_render("/x", NULL, 1, "", true);
	check_render("/ab/\x01", second_omitted, 6, "/\\x01", true);
	check_render("/\x80\x80\x80\x80\x80", NULL, 3, "\x80", true);
}

/* A rendered path's tail starts on a whole unit: never inside "\\\\",
 * "\\xNN" or a UTF-8 sequence.
 */
static void
test_rendered_tail(void)
{
	CHECK(fsimg_rendered_tail("/ab", 3, 3) == 0);
	CHECK(fsimg_rendered_tail("/ab", 3, 2) == 1);
	CHECK(fsimg_rendered_tail("/a\\\\b", 5, 3) == 2);
	CHECK(fsimg_rendered_tail("/a\\\\b", 5, 2) == 4);
	CHECK(fsimg_rendered_tail("/\\x01z", 6, 4) == 5);
	CHECK(fsimg_rendered_tail("/\\x01z", 6, 5) == 1);
	CHECK(fsimg_rendered_tail("/\xc3\xa9\xc3\xa9", 5, 3) == 3);
	CHECK(fsimg_rendered_tail("/\xc3\xa9\xc3\xa9", 5, 0) == 5);
}

typedef struct loss_record {
	char		 path[64];
	unsigned	 calls;
} loss_record_t;

static void
loss_callback(void *argument, const char *path, uint32_t fields)
{
	loss_record_t *record = argument;

	CHECK(fields == FSIMG_META_OWNER);
	CHECK(strlen(path) < sizeof(record->path));
	strcpy(record->path, path);
	record->calls++;
}

/* The loss callback's path is rendered into a buffer for the worst case of
 * the longest path, so a path of nothing but backslashes at the path limit
 * still arrives whole; the error path, 256 bytes, keeps the tail of a
 * longer rendering instead.
 */
static void
test_loss_rendering(void)
{
	fsimg_options_t options;
	fsimg_error_t error;
	loss_record_t record = { .calls = 0 };
	fsimg_ctx_t *ctx;
	char path[300], expected[64];

	fsimg_options_init(&options);
	options.max_path_bytes = 16;
	options.loss = loss_callback;
	options.loss_argument = &record;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	path[0] = '/';
	memset(path + 1, '\\', 14);
	path[15] = '\0';
	expected[0] = '/';
	memset(expected + 1, '\\', 28);
	expected[29] = '\0';
	CHECK(fsimg_report_loss(ctx, path, FSIMG_META_OWNER) == 1);
	CHECK(record.calls == 1 && strcmp(record.path, expected) == 0);
	fsimg_ctx_free(ctx);

	memset(path, 'p', sizeof(path) - 1);
	path[0] = '/';
	path[sizeof(path) - 1] = '\0';
	fsimg_error_path(&error, path);
	CHECK(error.path_truncated);
	CHECK(strlen(error.path) == sizeof(error.path) - 1);
	CHECK(strcmp(error.path, path + sizeof(path) - sizeof(error.path)) == 0);
	fsimg_error_path(&error, "/short\\name");
	CHECK(!error.path_truncated && strcmp(error.path, "/short\\\\name") == 0);
}

int
main(void)
{
	test_arithmetic();
	test_context();
	test_descriptor_cleanup();
	test_sha256_vectors();
	test_sha256_split_updates();
	test_path_render();
	test_rendered_tail();
	test_loss_rendering();
	return (0);
}
