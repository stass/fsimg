/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/* A test-only command build redirects fsync. Only the coordinator's extraction
 * cleanup reaches the hook: workers run in fresh executables without its PID.
 * Making stdout read-only there lets all job output finish before the final
 * JSON report fails. No shipped command or library contains a fault hook.
 */
#include "config.h"

#undef main
#undef fsync

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "host.h"
#include "test.h"

typedef enum terminal_fault {
	FAULT_NONE,
	FAULT_FSYNC,
	FAULT_REPORT
} terminal_fault_t;

static pid_t coordinator;
static terminal_fault_t fault;
static bool cleanup_seen;

int fsimg_test_command_main(int, char **);
int fsimg_test_terminal_fsync(int);

int
fsimg_test_terminal_fsync(int fd)
{
	int readonly;

	if (coordinator != 0 && getpid() == coordinator) {
		cleanup_seen = true;
		if (fault == FAULT_FSYNC) {
			errno = EIO;
			return (-1);
		}
		if (fault == FAULT_REPORT) {
			readonly = open("/dev/null", O_RDONLY | O_CLOEXEC);
			CHECK(readonly != -1);
			CHECK(dup2(readonly, STDOUT_FILENO) == STDOUT_FILENO);
			CHECK(close(readonly) == 0);
		}
	}
	return (fsync(fd));
}

/* A big-endian IFS with one four-byte regular file. The header and directory
 * are intact; only two payload bytes survive. Write just those 146 bytes.
 */
static void
write_partial_image(void)
{
	enum {
		mount_flags_offset = 84,
		mountpoint_offset = 88,
		directory_offset = 100,
		inode_offset = directory_offset + 4,
		data_pointer_offset = directory_offset + 24,
		data_offset = 144,
		image_size = 152,
		readable_payload_size = 2
	};
	unsigned char image[data_offset + readable_payload_size] = { 0 };
	int fd;

	bcopy("imagefs\005", image, 8);
	fsimg_store_be32(image + 8, image_size);
	fsimg_store_be32(image + 12, data_offset);
	fsimg_store_be32(image + 16, directory_offset);
	fsimg_store_be32(image + 20, 2);
	fsimg_store_be32(image + mount_flags_offset, 3);
	bcopy("/proc/boot", image + mountpoint_offset, 11);
	fsimg_store_be16(image + directory_offset, 40);
	fsimg_store_be32(image + inode_offset, 2);
	fsimg_store_be32(image + directory_offset + 8, 0100644);
	fsimg_store_be32(image + directory_offset + 20, 86400);
	fsimg_store_be32(image + data_pointer_offset, data_offset);
	fsimg_store_be32(image + directory_offset + 28, 4);
	bcopy("file", image + directory_offset + 32, 5);
	bcopy("da", image + data_offset, readable_payload_size);
	fd = open("partial.img", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, image, sizeof(image)) == sizeof(image));
	CHECK(close(fd) == 0);
}

static int
capture_file(const char *name)
{
	int fd;

	fd = open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(unlink(name) == 0);
	return (fd);
}

typedef struct terminal_result {
	char output[16384];
	char diagnostic[16384];
	int status;
} terminal_result_t;

typedef struct terminal_case {
	terminal_fault_t injected;
	bool json;
	bool message_on_stdout;
	const char *message;
} terminal_case_t;

static void
run_partial(char *executable, const terminal_case_t *test,
    terminal_result_t *result)
{
	char *arguments[] = { executable, "x", "--salvage", "-S", "off",
	    "-T", ".", "-C", ".", "-f", "partial.img", NULL, NULL };
	ssize_t count;
	pid_t child;
	int standard_output, errors, status;

	if (test->json)
		arguments[11] = "--json";
	standard_output = capture_file("stdout");
	errors = capture_file("stderr");
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(dup2(standard_output, STDOUT_FILENO) == STDOUT_FILENO);
		CHECK(dup2(errors, STDERR_FILENO) == STDERR_FILENO);
		coordinator = getpid();
		fault = test->injected;
		status = fsimg_test_command_main(test->json ? 12 : 11, arguments);
		CHECK(cleanup_seen);
		_exit(status);
	}
	CHECK(waitpid(child, &result->status, 0) == child);
	count = pread(standard_output, result->output,
	    sizeof(result->output) - 1, 0);
	CHECK(count >= 0 && (size_t)count < sizeof(result->output) - 1);
	result->output[count] = '\0';
	count = pread(errors, result->diagnostic,
	    sizeof(result->diagnostic) - 1, 0);
	CHECK(count >= 0 && (size_t)count < sizeof(result->diagnostic) - 1);
	result->diagnostic[count] = '\0';
	CHECK(close(standard_output) == 0);
	CHECK(close(errors) == 0);
}

/* Partial output proves the injected fault did not stop the extraction job. */
static void
check_partial_fixture(void)
{
	char payload_bytes[64];
	int payload;

	payload = open("partial/file.partial", O_RDONLY | O_NOFOLLOW);
	CHECK(payload != -1);
	CHECK(read(payload, payload_bytes, sizeof(payload_bytes)) == 2);
	CHECK(bcmp(payload_bytes, "da", 2) == 0);
	CHECK(close(payload) == 0);
	CHECK(unlink("partial/file.partial") == 0);
	CHECK(rmdir("partial") == 0);
}

static void
check_cleanup_error_json(const terminal_result_t *result)
{
	char needle[64];

	snprintf(needle, sizeof(needle), "\"code\":%d,", FSIMG_EIO);
	CHECK(strstr(result->output, needle) != NULL);
	snprintf(needle, sizeof(needle), "\"os_error\":%d,", EIO);
	CHECK(strstr(result->output, needle) != NULL);
	CHECK(strstr(result->output, "\"output_state\":\"incomplete\"") != NULL);
	CHECK(strstr(result->output, "\"salvage_losses\":") == NULL);
}

static void
check_terminal_case(const terminal_case_t *test)
{
	char executable[PATH_MAX];
	terminal_result_t result;
	const char *message_stream;

	CHECK(fsimg_host_executable(executable, sizeof(executable)) == 0);
	run_partial(executable, test, &result);
	fprintf(stderr, "terminal fault=%d json=%d: expected %s on %s; stdout=%s; stderr=%s\n",
	    test->injected, test->json, test->message,
	    test->message_on_stdout ? "stdout" : "stderr",
	    result.output, result.diagnostic);
	check_partial_fixture();

	CHECK(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 1);
	message_stream = result.diagnostic;
	if (test->message_on_stdout)
		message_stream = result.output;
	CHECK(strstr(message_stream, test->message) != NULL);
	if (test->injected == FAULT_NONE) {
		if (test->json)
			CHECK(strstr(result.output, "\"salvage_losses\":\"1\"") != NULL);
		return;
	}
	CHECK(strstr(result.output, "salvage extraction lost part of the image") == NULL);
	CHECK(strstr(result.diagnostic, "salvage extraction lost part of the image") == NULL);
	if (test->injected == FAULT_FSYNC && test->json)
		check_cleanup_error_json(&result);
}

static void
test_cleanup_fsync_overrides_partial(void)
{
	const terminal_case_t cases[] = {
		{ .injected = FAULT_NONE, .json = false,
		    .message_on_stdout = false,
		    .message = "salvage extraction lost part of the image" },
		{ .injected = FAULT_NONE, .json = true,
		    .message_on_stdout = true,
		    .message = "salvage extraction lost part of the image" },
		{ .injected = FAULT_FSYNC, .json = false,
		    .message_on_stdout = false,
		    .message = "private capability cleanup incomplete" },
		{ .injected = FAULT_FSYNC, .json = true,
		    .message_on_stdout = true,
		    .message = "private capability cleanup incomplete" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		check_terminal_case(&cases[i]);
}

static void
test_final_report_failure_overrides_partial(void)
{
	const terminal_case_t test = {
		.injected = FAULT_REPORT, .json = true,
		.message_on_stdout = false,
		.message = "output write failed"
	};

	check_terminal_case(&test);
}

int
main(int argc, char **argv)
{
	char directory[] = "salvage-terminal-XXXXXX";
	pid_t child;
	int original, status;
	unsigned failures = 0;
	void (*const tests[])(void) = {
		test_cleanup_fsync_overrides_partial,
		test_final_report_failure_overrides_partial
	};

	if (argc != 1)
		return (fsimg_test_command_main(argc, argv));
	original = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(original != -1);
	CHECK(mkdtemp(directory) != NULL);
	CHECK(chdir(directory) == 0);
	write_partial_image();
	/* Separate processes let both independent regressions report failures. */
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		child = fork();
		CHECK(child != -1);
		if (child == 0) {
			tests[i]();
			_exit(0);
		}
		CHECK(waitpid(child, &status, 0) == child);
		failures += !WIFEXITED(status) || WEXITSTATUS(status) != 0;
	}
	CHECK(unlink("partial.img") == 0);
	CHECK(fchdir(original) == 0);
	CHECK(close(original) == 0);
	CHECK(rmdir(directory) == 0);
	return (failures == 0 ? 0 : 1);
}
