/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cli.h"
#include "internal.h"
#include "test.h"

static void test_numbers(void);
static void test_options(void);
static void test_names(bool);
static void test_private_cleanup(void);
static void test_staging_cleanup(void);
static void test_progress(void);

static void
test_numbers(void)
{
	static const char *const invalid[] = { "", "-1", "+1", " 1", "1 ",
	    "1.0", "1%", "1KB", "1k", "1Mtail", "18446744073709551616", "16384P" };
	uint64_t number;
	int64_t epoch;

	CHECK(fsimg_cli_number("18446744073709551615", false, &number, NULL) == FSIMG_OK);
	CHECK(number == UINT64_MAX);
	CHECK(fsimg_cli_number("128T", true, &number, NULL) == FSIMG_OK);
	CHECK(number == UINT64_C(128) * 1024 * 1024 * 1024 * 1024);
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK(fsimg_cli_number(invalid[i], true, &number, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_cli_number("1K", false, &number, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_cli_epoch("-9223372036854775808", &epoch, NULL) == FSIMG_OK);
	CHECK(epoch == INT64_MIN);
	CHECK(fsimg_cli_epoch("9223372036854775807", &epoch, NULL) == FSIMG_OK);
	CHECK(epoch == INT64_MAX);
	CHECK(fsimg_cli_epoch("-9223372036854775809", &epoch, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_cli_epoch("9223372036854775808", &epoch, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_cli_epoch("+1", &epoch, NULL) == FSIMG_EINVAL);
	CHECK(fsimg_cli_epoch("1K", &epoch, NULL) == FSIMG_EINVAL);
}

static fsimg_status_t
parse(char *const *arguments, fsimg_cli_options_t *options)
{
	char *argv[32];
	int count;

	count = 0;
	while (arguments[count] != NULL) {
		CHECK(count < 31);
		argv[count] = arguments[count];
		count++;
	}
	argv[count] = NULL;
	return (fsimg_cli_parse(count, argv, options, NULL));
}

static void
test_options(void)
{
	char *invalid[][12] = {
		{ "fsimg", "info", "-n", "-t", "exfat", ".", NULL },
		{ "fsimg", "info", "-i", "image", "-j", "-j", NULL },
		{ "fsimg", "info", "-i", "-", NULL },
		{ "fsimg", "info", "-i", "image", "-z", "0", NULL },
		{ "fsimg", "info", "-i", "image", "-m", "15M", NULL },
		{ "fsimg", "info", "-i", "image", "-l", "entries=1K", NULL },
		{ "fsimg", "info", "-i", "image", "-l", "fds=4294967296", NULL },
		{ "fsimg", "info", "-i", "image", "-l", "work=1", "-l", "work=1", NULL },
		{ "fsimg", "info", "-i", "image", "-O", "oem=latin1", NULL },
		{ "fsimg", "info", "-i", "image", "-O", "oem=cp437", "-O", "oem=cp437", NULL },
		{ "fsimg", "info", "-i", "image", "-O", "sector=512", NULL },
		{ "fsimg", "cat", "-i", "image", "-j", "/file", NULL },
		{ "fsimg", "ls", "-i", "image", "-d", NULL },
		{ "fsimg", "extract", "-i", "image", NULL },
		{ "fsimg", "create", "-t", "exfat", "-o", "image", "-e", "315532800", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "exfat", "-O", "cluster=3K", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "fat16", "-O", "cluster=64K", ".", NULL },
		{ "fsimg", "formats", "-m", "16M", NULL },
		{ "fsimg", "info", "-i", "image", "operand", "-j", NULL }
	};
	char *dry_run[] = { "fsimg", "create", "-n", "-R", "-t", "exfat", ".", NULL };
	char *defaults[] = { "fsimg", "info", "-i", "image", "-m", "0", "-l", "fds=0", NULL };
	char *listing[] = { "fsimg", "ls", "-j", "-r", "-i", "image", "--", "/A", NULL };
	fsimg_cli_options_t options;
	char label[49];
	char *large_label[] = { "fsimg", "create", "-n", "-t", "exfat", "-L", label, ".", NULL };

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK(parse(invalid[i], &options) == FSIMG_EINVAL);
	CHECK(parse(dry_run, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_PLAN && options.output == NULL);
	CHECK(strcmp(options.request.write.label, "FSIMG") == 0);
	CHECK(parse(defaults, &options) == FSIMG_OK);
	CHECK(options.request.limits.memory_bytes == UINT64_C(256) * 1024 * 1024);
	CHECK(options.request.limits.max_open_files == 128);
	CHECK(parse(listing, &options) == FSIMG_OK);
	CHECK(options.json && (options.request.flags & FSIMG_JOB_RECURSIVE) != 0);
	CHECK(strcmp(options.request.selection, "/A") == 0);
	memset(label, 'A', sizeof(label) - 1);
	label[sizeof(label) - 1] = '\0';
	CHECK(parse(large_label, &options) == FSIMG_EINVAL);
}

static void
test_names(bool json)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY, .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_worker_text_t text;
	fsimg_error_t error;
	char output[2048];
	size_t used;

	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_LIST;
	options.scratch = "-";
	options.json = json;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	fsimg_wire_put64(frame.payload + FSIMG_WIRE_ENTRY_KIND, FSIMG_ENTRY_FILE);
	fsimg_wire_put64(frame.payload + FSIMG_WIRE_ENTRY_LENGTH, UINT64_C(9007199254740993));
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	bcopy(text.data, output, text.length);
	used = text.length;
	frame.type = FSIMG_FRAME_NAME;
	frame.length = 3;
	bcopy("/x\342", frame.payload, frame.length);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	bcopy(text.data, output + used, text.length);
	used += text.length;
	CHECK(display.utf8_used == 1);
	frame.length = 4;
	bcopy("\202\254\"\033", frame.payload, frame.length);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	bcopy(text.data, output + used, text.length);
	used += text.length;
	frame.type = FSIMG_FRAME_ENTRY_END;
	frame.length = 0;
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	bcopy(text.data, output + used, text.length);
	used += text.length;
	output[used] = '\0';
	CHECK(strstr(output, "9007199254740993") != NULL);
	CHECK(strstr(output, "/x\342\202\254\\\"\\u001b") != NULL);
	CHECK(memchr(output, '\033', used) == NULL);
	CHECK(!display.entry_open && display.utf8_used == 0);

	/* One maximum-size control-only frame expands sixfold, independently of
	 * the total pathname length. Multiple frames never accumulate in RAM.
	 */
	frame.type = FSIMG_FRAME_NAME;
	frame.length = FSIMG_FRAME_BYTES;
	memset(frame.payload, 0x7f, frame.length);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(text.length == FSIMG_FRAME_BYTES * 6);
	CHECK(text.length < FSIMG_WORKER_TEXT_BYTES);
	frame.length = 3;
	bcopy("\342\200\256", frame.payload, frame.length);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(text.length == 6 && bcmp(text.data, "\\u202e", 6) == 0);

	frame.type = FSIMG_FRAME_ENTRY;
	frame.length = FSIMG_WIRE_ENTRY_BYTES;
	bzero(frame.payload, frame.length);
	fsimg_wire_put64(frame.payload + FSIMG_WIRE_ENTRY_KIND, FSIMG_ENTRY_FILE);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	frame.type = FSIMG_FRAME_NAME;
	frame.length = 1;
	frame.payload[0] = 0xc0;
	CHECK(fsimg_cli_event(&display, &frame, &text, &error) == FSIMG_EIO);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(memchr(text.data, 0xc0, text.length) == NULL);
	if (json)
		CHECK(strstr(display.buffer, "\"complete\":false") != NULL);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_DONE);
}

static void
test_private_cleanup(void)
{
	char root[] = "cli-cleanup-XXXXXX";
	char input[PATH_MAX];
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths;
	fsimg_error_t error;
	fsimg_status_t status;
	struct stat st;
	int parent, fd, retained;

	CHECK(mkdtemp(root) != NULL);
	parent = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent != -1);
	fd = openat(parent, "image", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1 && close(fd) == 0);
	fd = openat(parent, "sentinel", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1 && write(fd, "safe", 4) == 4 && close(fd) == 0);
	snprintf(input, sizeof(input), "%s/image", root);
	for (unsigned scenario = 0; scenario < 2; scenario++) {
		bzero(&options, sizeof(options));
		fsimg_worker_request_init(&options.request);
		options.request.kind = FSIMG_JOB_INFO;
		options.input = input;
		options.scratch = root;
		CHECK(fsimg_cli_prepare(&options, &paths, &error) == FSIMG_OK);
		if (scenario == 0)
			CHECK(symlinkat("../sentinel", options.request.scratch_fd, "pivot") == 0);
		else
			CHECK(mkdirat(options.request.scratch_fd, "keep", 0700) == 0);
		status = fsimg_cli_cleanup(&options, &paths, FSIMG_OK, &error);
		CHECK(fstatat(parent, "sentinel", &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_size == 4);
		if (scenario == 0) {
			CHECK(status == FSIMG_OK && paths.retained_scratch[0] == '\0');
			CHECK(fstatat(parent, paths.scratch_name, &st, AT_SYMLINK_NOFOLLOW) == -1);
			CHECK(errno == ENOENT);
		} else {
			CHECK(status == FSIMG_EIO && error.cleanup_error != 0);
			CHECK(paths.retained_scratch[0] != '\0');
			retained = openat(parent, paths.scratch_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			CHECK(retained != -1);
			CHECK(unlinkat(retained, "keep", AT_REMOVEDIR) == 0);
			CHECK(close(retained) == 0);
			CHECK(unlinkat(parent, paths.scratch_name, AT_REMOVEDIR) == 0);
		}
	}
	CHECK(unlinkat(parent, "sentinel", 0) == 0);
	CHECK(unlinkat(parent, "image", 0) == 0);
	CHECK(close(parent) == 0 && rmdir(root) == 0);
}

static void
test_progress(void)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_frame_t frame = { .type = FSIMG_FRAME_PROGRESS, .length = 24 };
	fsimg_worker_text_t text;

	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_VERIFY;
	options.scratch = "-";
	options.json = true;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(text.length != 0);
	for (unsigned counter = 0; counter < 3; counter++) {
		for (unsigned bit = 0; bit < 64; bit++) {
			fsimg_wire_put64(frame.payload + counter * 8, UINT64_C(1) << bit);
			CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
			CHECK(text.length != 0);
		}
	}
	CHECK(display.progress_records == 193);
	/* New sessions may restart counters, and a hostile sender can repeat
	 * values. Neither can reset the command's already reported milestones.
	 */
	for (unsigned repeat = 0; repeat < 10; repeat++) {
		bzero(frame.payload, frame.length);
		CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
		CHECK(text.length == 0);
		for (unsigned counter = 0; counter < 3; counter++)
			fsimg_wire_put64(frame.payload + counter * 8, UINT64_MAX);
		CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
		CHECK(text.length == 0);
	}
	CHECK(display.progress_records == 193);
}

static void
test_staging_cleanup(void)
{
	char root[] = "cli-staging-XXXXXX";
	char output[PATH_MAX], expected[PATH_MAX];
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths;
	fsimg_error_t error;
	int parent, retained, fd;

	CHECK(mkdtemp(root) != NULL);
	parent = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent != -1);
	snprintf(output, sizeof(output), "%s/final", root);
	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_CREATE;
	options.source = root;
	options.output = output;
	options.scratch = root;
	CHECK(fsimg_cli_prepare(&options, &paths, &error) == FSIMG_OK);
	fd = openat(options.request.output_fd, "image", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd != -1 && close(fd) == 0);
	fd = openat(options.request.output_fd, "extra", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd != -1 && close(fd) == 0);

	/* Removing the image does not imply that its private directory can be
	 * removed. A retained-directory diagnostic must name an existing object.
	 */
	CHECK(fsimg_cli_cleanup(&options, &paths, FSIMG_OK, &error) == FSIMG_EIO);
	snprintf(expected, sizeof(expected), "%s/%s", root, paths.staging_name);
	CHECK(strcmp(paths.retained_output, expected) == 0);
	retained = open(paths.retained_output, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(retained != -1);
	CHECK(unlinkat(retained, "extra", 0) == 0);
	CHECK(close(retained) == 0);
	CHECK(unlinkat(parent, paths.staging_name, AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0 && rmdir(root) == 0);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--private-cleanup") == 0) {
		test_private_cleanup();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--staging-cleanup") == 0) {
		test_staging_cleanup();
		return (0);
	}
	CHECK(argc == 1);
	test_numbers();
	test_options();
	test_names(false);
	test_names(true);
	test_private_cleanup();
	test_staging_cleanup();
	test_progress();
	return (0);
}
