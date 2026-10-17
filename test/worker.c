/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "io.h"
#include "test.h"
#include "worker.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FSIMG_TEST_ASAN 1
#endif
#endif

typedef enum worker_scenario {
	SCENARIO_DONE_BEFORE_READY = 0,
	SCENARIO_DONE_BEFORE_INFO,
	SCENARIO_BAD_EXIT,
	SCENARIO_DUPLICATE_DONE,
	SCENARIO_NAME_WITHOUT_ENTRY,
	SCENARIO_TRUNCATED_HEADER,
	SCENARIO_MISSING_DONE,
	SCENARIO_STALLED_PARSER,
	SCENARIO_LONG_LIST_NAME,
	SCENARIO_STARTUP_NONREADER,
	SCENARIO_PREMATURE_RAW,
	SCENARIO_BLOCKED_RAW,
	SCENARIO_BROKEN_CONSUMER,
	SCENARIO_STARTUP_ERROR,
	SCENARIO_STALLED_RELAY,
	SCENARIO_FORGED_PUBLISHED,
	SCENARIO_FORGED_UNCERTAIN,
	SCENARIO_ACK_ERROR,
	SCENARIO_INVALID_TIME_BASIS,
	SCENARIO_BLOCKED_WARNING,
	SCENARIO_SLOW_CONSUMER,
	SCENARIO_VERIFY_INVALID_SCOPE,
	SCENARIO_VERIFY_HEADER_SCOPE,
	SCENARIO_VERIFY_UNREQUESTED_PAYLOAD,
	SCENARIO_VERIFY_MISSING_PAYLOAD,
	SCENARIO_RUNTIME_FAILURE,
	SCENARIO_PARTIAL_EXTRACTION,
	SCENARIO_ASAN_FAILURE,
} worker_scenario_t;

#ifdef FSIMG_TEST_ASAN
/* A volatile release target keeps the deliberate runtime failure observable
 * under optimization. This is used only to test fatal-error classification.
 */
typedef void (*test_release_fn_t)(void *);
static test_release_fn_t volatile release_test_memory = free;
#endif

typedef struct worker_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_worker_request_t request;
	fsimg_worker_result_t result;
	char		 path[64];
	char		 executable[4096];
	uint64_t	 name_bytes;
	uint64_t	 plan_losses;
	unsigned	 info;
	unsigned	 plans;
	bool		 ready;
	bool		 refuse_ready;
	bool		 dirty_event;
	bool		 print_info;
	int		 parent;
	int		 source;
	int		 output;
	int		 scratch;
	int		 input;
} worker_fixture_t;

static void fixture_open(worker_fixture_t *);
static void fixture_close(worker_fixture_t *);
static fsimg_status_t event(void *, const fsimg_frame_t *, fsimg_worker_text_t *,
    fsimg_error_t *);
static fsimg_status_t run(worker_fixture_t *, fsimg_error_t *);
static void test_operations(void);
static void test_startup(void);
static void test_inheritance(void);
static void test_forged(void);
static void test_forged_publication(void);
static void test_text_output(void);
static void test_incomplete_extraction(void);
static void test_mount_source(const char *);
static void test_transport(worker_scenario_t);
static int fake_worker(worker_scenario_t);
static int fake_output(void);
static uint64_t fill_pipe(int);
static void drain_after_delay(int, uint64_t);


static void
test_mount_source(const char *path)
{
	worker_fixture_t fixture;
	fsimg_host_file_t root_identity, alias_identity;
	fsimg_error_t error;
	uint64_t root_mount, alias_mount;
	int source, alias;

	fixture_open(&fixture);
	source = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(source != -1);
	alias = openat(source, "alias", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(alias != -1);
	CHECK(fsimg_host_file(source, &root_identity) == 0);
	CHECK(fsimg_host_file(alias, &alias_identity) == 0);
	CHECK(fsimg_host_mount_id(source, &root_mount) == 0);
	CHECK(fsimg_host_mount_id(alias, &alias_mount) == 0);
	CHECK(root_mount != alias_mount);
#if defined(__linux__)
	CHECK(root_identity.device == alias_identity.device);
#endif
	CHECK(close(alias) == 0);
	fixture.request.input_fd = -1;
	fixture.request.source_fd = source;
	fixture.request.kind = FSIMG_JOB_PLAN;
	fixture.request.sandbox = FSIMG_SANDBOX_REQUIRED;
	strcpy(fixture.request.format, "fat12");
	CHECK(run(&fixture, &error) == FSIMG_EUNSUPPORTED);
	CHECK(fixture.ready && fixture.result.confinement == FSIMG_CONFINEMENT_ACTIVE);
	CHECK(error.operation == FSIMG_OP_SOURCE && fixture.plans == 0);
	printf("mount alias rejected: devices=%llu/%llu mounts=%llu/%llu\n",
	    (unsigned long long)root_identity.device,
	    (unsigned long long)alias_identity.device,
	    (unsigned long long)root_mount, (unsigned long long)alias_mount);
	CHECK(close(source) == 0);
	fixture_close(&fixture);
}

static uint64_t
fill_pipe(int fd)
{
	unsigned char bytes[4096] = { 0 };
	uint64_t filled;
	ssize_t count;
	int flags;

	flags = fcntl(fd, F_GETFL);
	CHECK(flags != -1);
	CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
	filled = 0;
	while ((count = write(fd, bytes, sizeof(bytes))) > 0)
		filled += (uint64_t)count;
	CHECK(count == -1 && errno == EAGAIN);
	CHECK(fcntl(fd, F_SETFL, flags) == 0);
	return (filled);
}

static void
drain_after_delay(int fd, uint64_t padding)
{
	unsigned char bytes[4096];
	size_t batch, offset;
	ssize_t count;

	poll(NULL, 0, 3000);
	while (padding != 0) {
		batch = padding > sizeof(bytes) ? sizeof(bytes) : (size_t)padding;
		count = read(fd, bytes, batch);
		CHECK(count > 0);
		for (ssize_t i = 0; i < count; i++)
			CHECK(bytes[i] == 0);
		padding -= (uint64_t)count;
	}
	offset = 0;
	while (offset < 7) {
		count = read(fd, bytes + offset, 7 - offset);
		CHECK(count > 0);
		offset += (size_t)count;
	}
	CHECK(bcmp(bytes, "payload", 7) == 0);
}

static void
fixture_open(worker_fixture_t *fixture)
{
	fsimg_options_t options;
	fsimg_writer_t *writer;
	fsimg_io_t *output;
	const struct timespec source_times[2] = {
		{ .tv_sec = 315532801, .tv_nsec = 1 },
		{ .tv_sec = 315532801, .tv_nsec = 1 }
	};
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->path, sizeof(fixture->path), "worker-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->path) != NULL);
	fixture->parent = open(fixture->path, O_RDONLY | O_DIRECTORY);
	CHECK(fixture->parent != -1);
	CHECK(mkdirat(fixture->parent, "source", 0700) == 0);
	CHECK(mkdirat(fixture->parent, "output", 0700) == 0);
	CHECK(mkdirat(fixture->parent, "scratch", 0700) == 0);
	fixture->source = openat(fixture->parent, "source", O_RDONLY | O_DIRECTORY);
	fixture->output = openat(fixture->parent, "output", O_RDONLY | O_DIRECTORY);
	fixture->scratch = openat(fixture->parent, "scratch", O_RDONLY | O_DIRECTORY);
	CHECK(fixture->source != -1 && fixture->output != -1 && fixture->scratch != -1);
	fd = openat(fixture->source, "HELLO.TXT", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "payload", 7) == 7);
	CHECK(futimens(fd, source_times) == 0);
	CHECK(close(fd) == 0);
	fsimg_options_init(&options);
	options.scratch_directory_fd = fixture->scratch;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add_tree(writer, fixture->source, "/", NULL) == FSIMG_OK);
	fd = openat(fixture->parent, "image", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(fixture->ctx, fd, FSIMG_IO_WRITE, &output, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, output, NULL) == FSIMG_OK);
	CHECK(fsimg_io_close_output(output, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_writer_free(writer);
	fixture->input = openat(fixture->parent, "image", O_RDONLY);
	CHECK(fixture->input != -1);
	CHECK(fsimg_host_executable(fixture->executable, sizeof(fixture->executable)) == 0);
	fsimg_worker_request_init(&fixture->request);
	fixture->request.input_fd = fixture->input;
	fixture->request.scratch_fd = fixture->scratch;
	fixture->request.limits.memory_bytes = UINT64_C(16) * 1024 * 1024;
	fixture->request.kind = FSIMG_JOB_INFO;
}

static void
fixture_close(worker_fixture_t *fixture)
{
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->input) == 0);
	CHECK(unlinkat(fixture->parent, "image", 0) == 0);
	CHECK(unlinkat(fixture->source, "HELLO.TXT", 0) == 0);
	CHECK(close(fixture->source) == 0);
	CHECK(close(fixture->output) == 0);
	CHECK(close(fixture->scratch) == 0);
	CHECK(unlinkat(fixture->parent, "source", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture->parent, "output", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture->parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->path) == 0);
}

static fsimg_status_t
event(void *argument, const fsimg_frame_t *frame,
	fsimg_worker_text_t *text, fsimg_error_t *error)
{
	worker_fixture_t *fixture = argument;

	if (fixture->dirty_event && error != NULL)
		error->status = FSIMG_EIO;
	if (frame->type == FSIMG_FRAME_READY) {
		fixture->ready = true;
		if (fixture->refuse_ready)
			return (FSIMG_ECANCELLED);
	} else {
		CHECK(fixture->ready);
		if (frame->type == FSIMG_FRAME_INFO)
			fixture->info++;
		if (frame->type == FSIMG_FRAME_INFO && fixture->print_info) {
			text->data = "printed information\n";
			text->length = strlen(text->data);
		}
		if (frame->type == FSIMG_FRAME_PLAN) {
			fixture->plans++;
			fixture->plan_losses = fsimg_wire_get64(frame->payload + FSIMG_WIRE_PLAN_LOSSES);
		}
		if (frame->type == FSIMG_FRAME_NAME)
			fixture->name_bytes += frame->length;
	}
	return (FSIMG_OK);
}

static void
test_text_output(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	char bytes[32];
	int fd, flags, status_flags;

	fixture_open(&fixture);
	fixture.print_info = true;
	fd = openat(fixture.parent, "text", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1);
	flags = fcntl(fd, F_GETFL);
	CHECK(flags != -1);
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_OK);
	CHECK(pread(fd, bytes, sizeof(bytes), 0) == 20);
	CHECK(bcmp(bytes, "printed information\n", 20) == 0);
	/* Compare public status flags. macOS also exposes a kernel write-history
	 * bit through F_GETFL, which an ordinary sink write legitimately sets.
	 */
	status_flags = O_ACCMODE | O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC;
	CHECK((fcntl(fd, F_GETFL) & status_flags) == (flags & status_flags));
	CHECK(run(&fixture, &error) == FSIMG_EINVAL);
	CHECK(error.status == FSIMG_EINVAL);
	fixture.request.kind = FSIMG_JOB_CAT;
	fixture.request.selection = "/hello.txt";
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_EIO);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "text", 0) == 0);
	fixture_close(&fixture);
}

static fsimg_status_t
run(worker_fixture_t *fixture, fsimg_error_t *error)
{
	fixture->ready = false;
	return (fsimg_worker_run(fixture->executable, &fixture->request, event,
	    fixture, -1, NULL, &fixture->result, error));
}

static void
test_operations(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;
	char bytes[8];
	int fd;

	fixture_open(&fixture);
	status = run(&fixture, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "worker info: %d: %s\n", status, error.message);
	CHECK(status == FSIMG_OK && fixture.info == 1);
	CHECK(error.status == FSIMG_OK);
	fixture.request.kind = FSIMG_JOB_LIST;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.entries == 1 && fixture.name_bytes == strlen("/HELLO.TXT"));
	fixture.request.kind = FSIMG_JOB_VERIFY;
	fixture.request.flags = FSIMG_JOB_PAYLOAD;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.logical_bytes == 7);
	fixture.request.flags = 0;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.output_fd = fixture.output;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.entries == 1 && fixture.result.logical_bytes == 7);
	fd = openat(fixture.output, "HELLO.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(read(fd, bytes, sizeof(bytes)) == 7);
	CHECK(bcmp(bytes, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.output, "HELLO.TXT", 0) == 0);

	fixture.request.kind = FSIMG_JOB_CAT;
	fixture.request.output_fd = -1;
	fixture.request.selection = "/HELLO.TXT";
	fd = openat(fixture.parent, "raw", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1);
	fixture.ready = false;
	CHECK(fsimg_worker_run(fixture.executable, &fixture.request, event, &fixture,
	    fd, NULL, &fixture.result, &error) == FSIMG_OK);
	CHECK(fixture.result.raw_bytes == 7);
	CHECK(pread(fd, bytes, sizeof(bytes), 0) == 7);
	CHECK(bcmp(bytes, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "raw", 0) == 0);

	fixture.request.kind = FSIMG_JOB_CREATE;
	fixture.request.selection = "/";
	fixture.request.input_fd = -1;
	fixture.request.source_fd = fixture.source;
	fixture.request.output_fd = fixture.output;
	strcpy(fixture.request.format, "fat12");
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.plans == 1 && fixture.result.image_bytes != 0);
	CHECK(fixture.plan_losses != 0 && fixture.result.metadata_losses == fixture.plan_losses);
	CHECK(unlinkat(fixture.output, "image", 0) == 0);
	fixture.request.kind = FSIMG_JOB_PLAN;
	fixture.request.output_fd = -1;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.plans == 2);
	CHECK(fixture.result.metadata_losses == fixture.plan_losses);
	fixture_close(&fixture);
}

static void
test_startup(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status;

	fixture_open(&fixture);
	fixture.refuse_ready = true;
	fixture.dirty_event = true;
	bzero(&error, sizeof(error));
	CHECK(run(&fixture, &error) == FSIMG_ECANCELLED);
	CHECK(error.status == FSIMG_ECANCELLED);
	CHECK(fixture.ready && fixture.info == 0);
	fixture.refuse_ready = false;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(error.status == FSIMG_OK);
	fixture.info = 0;
	fixture.dirty_event = false;
	fixture.request.sandbox = FSIMG_SANDBOX_REQUIRED;
	status = run(&fixture, &error);
#if defined(__APPLE__)
	CHECK(status == FSIMG_EUNSUPPORTED);
	CHECK(fixture.info == 0);
#else
	CHECK(status == FSIMG_OK || status == FSIMG_EUNSUPPORTED);
#endif
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_OK);
	CHECK(fixture.result.confinement == FSIMG_CONFINEMENT_DISABLED);
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.output_fd = fixture.output;
	fixture.info = 0;
	for (uint64_t limit = 1; limit <= 2; limit++) {
		fixture.request.limits.max_open_files = limit;
		CHECK(run(&fixture, &error) == FSIMG_ELIMIT);
		CHECK(!fixture.ready && fixture.info == 0);
		CHECK(fixture.result.confinement == FSIMG_CONFINEMENT_UNKNOWN);
	}
	fixture_close(&fixture);
}

static void
test_inheritance(void)
{
	worker_fixture_t fixture;
	struct rlimit original, lowered;
	int high;

	fixture_open(&fixture);
	high = fcntl(fixture.input, F_DUPFD, 200);
	CHECK(high >= 200);
	CHECK(getrlimit(RLIMIT_NOFILE, &original) == 0);
	lowered = original;
	lowered.rlim_cur = 64;
	CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
	CHECK(run(&fixture, NULL) == FSIMG_OK);
	CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
	CHECK(close(high) == 0);
	fixture_close(&fixture);
}

/* This test-only executable can act as a compromised worker. Executable mode
 * bits on its private fixture select a framing attack without adding test
 * flags or hooks to production worker code. It never parses image bytes.
 */
static int
fake_worker(worker_scenario_t mode)
{
	fsimg_frame_t frame;
	unsigned char raw[FSIMG_FRAME_BYTES], header[FSIMG_FRAME_HEADER];
	uint64_t remaining;
	struct timespec delay = { .tv_nsec = 200000000 };
	fsimg_error_t error;
	int output;
#ifdef FSIMG_TEST_ASAN
	unsigned char *allocation;
#endif

	if (mode == SCENARIO_STARTUP_NONREADER) {
		for (;;)
			pause();
	}

	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	if (mode == SCENARIO_STARTUP_ERROR || mode == SCENARIO_FORGED_PUBLISHED || mode == SCENARIO_FORGED_UNCERTAIN) {
		bzero(&error, sizeof(error));
		error.status = mode == SCENARIO_STARTUP_ERROR ? FSIMG_EINVAL : FSIMG_EIO;
		if (mode != SCENARIO_STARTUP_ERROR)
			error.output_state = mode == SCENARIO_FORGED_PUBLISHED ? FSIMG_OUTPUT_PUBLISHED : FSIMG_OUTPUT_UNCERTAIN;
		strcpy(error.message, "startup test rejection");
		fsimg_wire_error_encode(raw, &error);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ERROR,
		    raw, FSIMG_WIRE_ERROR_BYTES, NULL) == FSIMG_OK);
		return (1);
	}
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	bzero(raw, sizeof(raw));
#ifdef FSIMG_TEST_ASAN
	if (mode == SCENARIO_ASAN_FAILURE) {
		allocation = malloc(1);
		CHECK(allocation != NULL);
		release_test_memory(allocation);
		return (((volatile unsigned char *)allocation)[0]);
	}
#endif
	if (mode == SCENARIO_PREMATURE_RAW) {
		CHECK(write(FSIMG_CHILD_DATA, "ATTACK", 6) == 6);
		nanosleep(&delay, NULL);
	}
	if (mode == SCENARIO_DONE_BEFORE_READY) {
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, 40, NULL);
		return (0);
	}
	if (mode == SCENARIO_TRUNCATED_HEADER) {
		CHECK(fsimg_frame_header(header, FSIMG_FRAME_READY, 8, NULL) == FSIMG_OK);
		CHECK(write(FSIMG_CHILD_CONTROL, header, 3) == 3);
		return (0);
	}
	fsimg_wire_put64(raw, 2);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_READY, raw, 8, NULL) == FSIMG_OK);
	if (mode == SCENARIO_ACK_ERROR) {
		CHECK(close(FSIMG_CHILD_JOB) == 0);
		bzero(&error, sizeof(error));
		error.status = FSIMG_ECORRUPT;
		strcpy(error.message, "acknowledgement test rejection");
		fsimg_wire_error_encode(raw, &error);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ERROR,
		    raw, FSIMG_WIRE_ERROR_BYTES, NULL) == FSIMG_OK);
		return (1);
	}
	CHECK(fsimg_frame_read(FSIMG_CHILD_JOB, &frame, NULL) == FSIMG_OK);
	if (mode == SCENARIO_DONE_BEFORE_INFO) {
		bzero(raw, 40);
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, 40, NULL);
		return (0);
	}
	if (mode == SCENARIO_NAME_WITHOUT_ENTRY) {
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME, "X", 1, NULL);
		return (0);
	}
	bzero(raw, 192);
	fsimg_wire_put64(raw + 56, mode == SCENARIO_INVALID_TIME_BASIS ? UINT64_MAX : FSIMG_TIME_WALL);
	fsimg_wire_put64(raw + 64, FSIMG_VALIDATION_HEADER);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO, raw, 192, NULL) == FSIMG_OK);
	if (mode == SCENARIO_PARTIAL_EXTRACTION) {
		output = openat(FSIMG_CHILD_OUTPUT, "partial", O_WRONLY | O_CREAT | O_EXCL, 0600);
		CHECK(output != -1);
		CHECK(write(output, "prefix", 6) == 6);
		CHECK(close(output) == 0);
		return (1);
	}
	if (mode >= SCENARIO_VERIFY_INVALID_SCOPE && mode <= SCENARIO_VERIFY_MISSING_PAYLOAD) {
		bzero(raw, 72);
		fsimg_wire_put64(raw, mode == SCENARIO_VERIFY_INVALID_SCOPE ? UINT64_MAX : mode == SCENARIO_VERIFY_HEADER_SCOPE ?
		    FSIMG_VALIDATION_HEADER : mode == SCENARIO_VERIFY_UNREQUESTED_PAYLOAD ? FSIMG_VALIDATION_PAYLOAD :
		    FSIMG_VALIDATION_STRUCTURAL);
		fsimg_wire_put64(raw + 8, 1);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_VERIFY,
		    raw, 72, NULL) == FSIMG_OK);
	}
	if (mode == SCENARIO_MISSING_DONE)
		return (0);
	if (mode == SCENARIO_STALLED_PARSER) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	if (mode == SCENARIO_LONG_LIST_NAME) {
		bzero(raw, 168);
		fsimg_wire_put64(raw, FSIMG_ENTRY_FILE);
		fsimg_wire_put64(raw + 16, 65535);
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY, raw, 168, NULL) == FSIMG_OK);
		memset(raw, 'N', sizeof(raw));
		raw[0] = '/';
		remaining = 65535;
		while (remaining != 0) {
			uint32_t batch = remaining > sizeof(raw) ? sizeof(raw) : (uint32_t)remaining;

			CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME, raw, batch, NULL) == FSIMG_OK);
			remaining -= batch;
		}
		CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY_END, NULL, 0, NULL) == FSIMG_OK);
	}
	bzero(raw, 40);
	fsimg_wire_put64(raw, mode == SCENARIO_LONG_LIST_NAME ? 1 : 0);
	CHECK(fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, 40, NULL) == FSIMG_OK);
	if (mode == SCENARIO_DUPLICATE_DONE)
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, 40, NULL);
	if (mode == SCENARIO_RUNTIME_FAILURE)
		return (FSIMG_WORKER_RUNTIME_EXIT);
	return (mode == SCENARIO_BAD_EXIT ? 1 : 0);
}

static void
test_incomplete_extraction(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	int fd;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
	CHECK(fd != -1);
	CHECK(ftruncate(fd, SCENARIO_PARTIAL_EXTRACTION) == 0);
	CHECK(fchmod(fd, 0700) == 0);
	fixture.request.input_fd = fd;
	fixture.request.output_fd = fixture.output;
	fixture.request.kind = FSIMG_JOB_EXTRACT;
	fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	CHECK(run(&fixture, &error) == FSIMG_EIO);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(unlinkat(fixture.output, "partial", 0) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	fixture_close(&fixture);
}

static void
test_forged_publication(void)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	int fd;

	fixture_open(&fixture);
	for (worker_scenario_t mode = SCENARIO_FORGED_PUBLISHED; mode <= SCENARIO_ACK_ERROR; mode++) {
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, mode) == 0);
		CHECK(fchmod(fd, 0700) == 0);
		fixture.request.input_fd = fd;
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		CHECK(run(&fixture, &error) == (mode == SCENARIO_ACK_ERROR ? FSIMG_ECORRUPT : FSIMG_EIO));
		CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
	}
	fixture_close(&fixture);
}

static void
test_forged(void)
{
	const worker_scenario_t cases[] = {
		SCENARIO_DONE_BEFORE_READY, SCENARIO_DONE_BEFORE_INFO,
		SCENARIO_BAD_EXIT, SCENARIO_DUPLICATE_DONE, SCENARIO_NAME_WITHOUT_ENTRY,
		SCENARIO_TRUNCATED_HEADER, SCENARIO_MISSING_DONE,
		SCENARIO_STALLED_PARSER, SCENARIO_LONG_LIST_NAME,
		SCENARIO_INVALID_TIME_BASIS, SCENARIO_VERIFY_INVALID_SCOPE,
		SCENARIO_VERIFY_HEADER_SCOPE, SCENARIO_VERIFY_UNREQUESTED_PAYLOAD,
		SCENARIO_VERIFY_MISSING_PAYLOAD, SCENARIO_RUNTIME_FAILURE
#ifdef FSIMG_TEST_ASAN
		, SCENARIO_ASAN_FAILURE
#endif
	};
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_status_t status, expected;
	worker_scenario_t mode;
	int fd;

	for (size_t scenario = 0; scenario < sizeof(cases) / sizeof(cases[0]); scenario++) {
		mode = cases[scenario];
		fixture_open(&fixture);
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1);
		CHECK(ftruncate(fd, mode) == 0);
		CHECK(fchmod(fd, 0700) == 0);
		fixture.request.input_fd = fd;
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
		fixture.request.kind = FSIMG_JOB_INFO;
		fixture.request.seconds = 5;
		expected = FSIMG_EIO;
		if (mode == SCENARIO_LONG_LIST_NAME) {
			fixture.request.kind = FSIMG_JOB_LIST;
			expected = FSIMG_OK;
		}
		if (mode >= SCENARIO_VERIFY_INVALID_SCOPE && mode <= SCENARIO_VERIFY_MISSING_PAYLOAD)
			fixture.request.kind = FSIMG_JOB_VERIFY;
		if (mode == SCENARIO_VERIFY_MISSING_PAYLOAD)
			fixture.request.flags = FSIMG_JOB_PAYLOAD;
		if (mode == SCENARIO_STALLED_PARSER) {
			fixture.request.seconds = 1;
			expected = FSIMG_ECANCELLED;
		}
		if (mode == SCENARIO_RUNTIME_FAILURE || mode == SCENARIO_ASAN_FAILURE)
			expected = FSIMG_ESTATE;

		status = run(&fixture, &error);
		if (status != expected)
			fprintf(stderr, "forged worker scenario %d: expected %d, received %d: %s\n",
			    mode, expected, status, error.message);
		CHECK(status == expected);
		if (mode == SCENARIO_LONG_LIST_NAME)
			CHECK(fixture.name_bytes == 65535);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(fixture.parent, "fake", 0) == 0);
		fixture_close(&fixture);
	}
}

/* A separate process group bounds regressions that deliberately stop draining
 * a pipe. Killing that group also reaps the fixture's worker on a broken tree.
 */
static void
test_transport(worker_scenario_t mode)
{
	worker_fixture_t fixture;
	fsimg_error_t error;
	fsimg_writer_t *writer;
	fsimg_entry_t entry;
	fsimg_source_t zero;
	fsimg_io_t *image;
	fsimg_status_t status;
	struct stat st;
	char *selection;
	pid_t child, waited, consumer;
	uint64_t padding;
	int fd, output, channels[2], outcome;
	bool finished;

	fixture_open(&fixture);
	fd = -1;
	channels[0] = channels[1] = -1;
	selection = NULL;
	padding = 0;
	if (mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_PREMATURE_RAW || mode == SCENARIO_STARTUP_ERROR) {
		fd = openat(fixture.parent, "fake", O_CREAT | O_EXCL | O_RDWR, 0700);
		CHECK(fd != -1 && ftruncate(fd, mode) == 0 && fchmod(fd, 0700) == 0);
		fixture.request.input_fd = fd;
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	}
	if (mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_STARTUP_ERROR) {
		selection = malloc(65536);
		CHECK(selection != NULL);
		memset(selection, 'N', 65535);
		selection[0] = '/';
		selection[65535] = '\0';
		fixture.request.selection = selection;
	}
	if (mode == SCENARIO_BLOCKED_RAW) {
		CHECK(fsimg_writer_new(fixture.ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
		bzero(&entry, sizeof(entry));
		bzero(&zero, sizeof(zero));
		entry.path = "/BIG";
		entry.kind = FSIMG_ENTRY_FILE;
		entry.size = 1024 * 1024;
		zero.kind = FSIMG_SOURCE_ZERO;
		CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
		fd = openat(fixture.parent, "large", O_RDWR | O_CREAT | O_EXCL, 0600);
		CHECK(fd != -1);
		CHECK(fsimg_io_from_fd(fixture.ctx, fd, FSIMG_IO_WRITE, &image, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(writer, image, NULL) == FSIMG_OK);
		CHECK(fsimg_io_close_output(image, NULL) == FSIMG_OK);
		CHECK(close(fd) == 0);
		fsimg_writer_free(writer);
		fd = openat(fixture.parent, "large", O_RDONLY);
		CHECK(fd != -1);
		fixture.request.input_fd = fd;
		fixture.request.selection = "/BIG";
		CHECK(pipe(channels) == 0);
		output = channels[1];
	} else if (mode == SCENARIO_BROKEN_CONSUMER) {
		CHECK(pipe(channels) == 0);
		CHECK(close(channels[0]) == 0);
		channels[0] = -1;
		output = channels[1];
		fixture.request.selection = "/HELLO.TXT";
	} else if (mode == SCENARIO_BLOCKED_WARNING || mode == SCENARIO_SLOW_CONSUMER) {
		CHECK(pipe(channels) == 0);
		output = channels[1];
		padding = fill_pipe(output);
		fixture.request.selection = mode == SCENARIO_BLOCKED_WARNING ? "/" : "/HELLO.TXT";
		fixture.request.sandbox = FSIMG_SANDBOX_OFF;
	} else {
		output = openat(fixture.parent, "raw", O_CREAT | O_EXCL | O_RDWR, 0600);
		CHECK(output != -1);
		if (mode == SCENARIO_STALLED_RELAY) {
			CHECK(fchmod(output, 0700) == 0);
			fixture.request.selection = "/HELLO.TXT";
		}
	}
	fixture.request.kind = mode == SCENARIO_STARTUP_NONREADER || mode == SCENARIO_STARTUP_ERROR || mode == SCENARIO_BLOCKED_WARNING ?
	    FSIMG_JOB_INFO : FSIMG_JOB_CAT;
	fixture.request.seconds = mode == SCENARIO_STALLED_RELAY || mode == SCENARIO_SLOW_CONSUMER ? 0 : 1;
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(setpgid(0, 0) == 0);
		consumer = -1;
		if (mode == SCENARIO_BLOCKED_WARNING)
			CHECK(dup2(output, STDERR_FILENO) == STDERR_FILENO);
		if (mode == SCENARIO_SLOW_CONSUMER) {
			consumer = fork();
			CHECK(consumer != -1);
			if (consumer == 0) {
				drain_after_delay(channels[0], padding);
				_exit(0);
			}
		}
		for (unsigned attempt = 0; attempt < (mode == SCENARIO_STARTUP_ERROR ? 8u : 1u); attempt++) {
			status = fsimg_worker_run(fixture.executable, &fixture.request, event,
			    &fixture, output, NULL, &fixture.result, &error);
			if (mode == SCENARIO_STARTUP_ERROR) {
				CHECK(status == FSIMG_EINVAL);
				CHECK(strcmp(error.message, "startup test rejection") == 0);
			}
		}
		if (mode == SCENARIO_PREMATURE_RAW) {
			CHECK(status == FSIMG_EIO);
			CHECK(fstat(output, &st) == 0 && st.st_size == 0);
		} else if (mode == SCENARIO_BROKEN_CONSUMER) {
			CHECK(status == FSIMG_EIO && error.os_error == EPIPE);
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		} else if (mode == SCENARIO_STALLED_RELAY) {
			CHECK(status == FSIMG_EIO);
		} else if (mode == SCENARIO_SLOW_CONSUMER) {
			CHECK(status == FSIMG_OK && fixture.result.raw_bytes == 7);
			CHECK(waitpid(consumer, &outcome, 0) == consumer);
			CHECK(WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
		} else if (mode != SCENARIO_STARTUP_ERROR) {
			CHECK(status == FSIMG_ECANCELLED);
			if (mode == SCENARIO_BLOCKED_RAW)
				CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
			if (mode == SCENARIO_BLOCKED_WARNING)
				CHECK(!fixture.ready && fixture.info == 0);
		}
		_exit(0);
	}
	CHECK(setpgid(child, child) == 0 || errno == EACCES);
	finished = false;
	for (unsigned i = 0; i < 400; i++) {
		waited = waitpid(child, &outcome, WNOHANG);
		CHECK(waited != -1);
		if (waited == child) {
			finished = true;
			break;
		}
		poll(NULL, 0, 20);
	}
	if (!finished) {
		CHECK(kill(-child, SIGKILL) == 0);
		CHECK(waitpid(child, &outcome, 0) == child);
	}
	/* Cleanup precedes assertions so a failing regression leaves no fixtures. */
	CHECK(close(output) == 0);
	if (channels[0] != -1)
		CHECK(close(channels[0]) == 0);
	else if (mode != SCENARIO_BLOCKED_RAW && mode != SCENARIO_BROKEN_CONSUMER)
		CHECK(unlinkat(fixture.parent, "raw", 0) == 0);
	if (fd != -1)
		CHECK(close(fd) == 0);
	if (mode != SCENARIO_BROKEN_CONSUMER && mode != SCENARIO_STALLED_RELAY && mode != SCENARIO_BLOCKED_WARNING && mode != SCENARIO_SLOW_CONSUMER)
		CHECK(unlinkat(fixture.parent, mode == SCENARIO_BLOCKED_RAW ? "large" : "fake", 0) == 0);
	free(selection);
	fixture_close(&fixture);
	CHECK(finished && WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
}

/* Consume complete relay records, acknowledge their counts, then deliberately
 * keep the process alive after its valid final record and status EOF.
 */
static int
fake_output(void)
{
	unsigned char header[16], buffer[4096], report[32];
	uint64_t confirmed[2] = { 0, 0 }, stream, remaining;
	size_t offset, batch;
	ssize_t count;

	for (;;) {
		offset = 0;
		while (offset < sizeof(header)) {
			count = read(3, header + offset, sizeof(header) - offset);
			CHECK(count >= 0);
			if (count == 0) {
				CHECK(offset == 0);
				goto complete;
			}
			offset += (size_t)count;
		}
		stream = fsimg_wire_get64(header);
		remaining = fsimg_wire_get64(header + 8);
		CHECK(stream == 1 || stream == 2);
		while (remaining != 0) {
			batch = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
			count = read(3, buffer, batch);
			CHECK(count > 0);
			confirmed[stream - 1] += (uint64_t)count;
			remaining -= (uint64_t)count;
		}
		bzero(report, sizeof(report));
		fsimg_wire_put64(report, confirmed[0]);
		fsimg_wire_put64(report + 8, confirmed[1]);
		fsimg_wire_put64(report + 24, 1);
		CHECK(write(4, report, sizeof(report)) == sizeof(report));
	}
complete:
	bzero(report, sizeof(report));
	fsimg_wire_put64(report, confirmed[0]);
	fsimg_wire_put64(report + 8, confirmed[1]);
	fsimg_wire_put64(report + 24, 2);
	CHECK(write(4, report, sizeof(report)) == sizeof(report));
	CHECK(close(4) == 0);
	signal(SIGTERM, SIG_IGN);
	for (;;)
		pause();
}

int
main(int argc, char **argv)
{
	const struct rlimit no_core = { 0, 0 };
	struct stat st;

	if (argc == 2 && strcmp(argv[1], "--internal-output") == 0) {
		if (fstat(STDOUT_FILENO, &st) == 0 && (st.st_mode & 0100) != 0)
			return (fake_output());
		return (fsimg_output_main());
	}
	if (argc == 2 && strcmp(argv[1], "--internal-worker") == 0) {
		CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
		CHECK(fcntl(200, F_GETFD) == -1 && errno == EBADF);
		if (fstat(FSIMG_CHILD_INPUT, &st) == 0 && (st.st_mode & 0100) != 0)
			return (fake_worker((worker_scenario_t)st.st_size));
		return (fsimg_worker_main());
	}
	if (argc == 3 && strcmp(argv[1], "--mount-source") == 0) {
		signal(SIGPIPE, SIG_IGN);
		test_mount_source(argv[2]);
		return (0);
	}
	CHECK(argc == 1);

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
	puts("SKIP: leak scanning inside fresh children requires tracing unavailable under confinement");
#endif
#endif
	signal(SIGPIPE, SIG_IGN);
	if (getenv("FSIMG_TEST_TRANSPORT") != NULL) {
		test_transport((worker_scenario_t)atoi(getenv("FSIMG_TEST_TRANSPORT")));
		return (0);
	}
	test_operations();
	test_text_output();
	test_startup();
	test_inheritance();
	test_forged();
	test_forged_publication();
	test_incomplete_extraction();
	{
		test_transport(SCENARIO_STARTUP_NONREADER);
		test_transport(SCENARIO_PREMATURE_RAW);
		test_transport(SCENARIO_BLOCKED_RAW);
		test_transport(SCENARIO_BROKEN_CONSUMER);
		test_transport(SCENARIO_STARTUP_ERROR);
		test_transport(SCENARIO_STALLED_RELAY);
		test_transport(SCENARIO_BLOCKED_WARNING);
		test_transport(SCENARIO_SLOW_CONSUMER);
	}
	return (0);
}
