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
static void test_final_diagnostics(void);
static void test_verification_completion(void);
static void test_volume_controls(void);
static void test_posix_options(void);
static void test_posix_listing(bool);
static void test_posix_volume(bool);
static void test_skipped_summary(void);
static void test_format_masks(bool);
static size_t list_entry(fsimg_cli_display_t *, const fsimg_entry_t *, char *,
    size_t);

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
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "endian=middle", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "uuid=1-2-3-4-5", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O",
		    "uuid=11111111-2222-3333-4444-55555555555g", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O",
		    "uuid=11111111+2222-3333-4444-555555555555", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O",
		    "uuid=11111111-2222-3333-4444-5555555555550", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "inodes=0", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "inodes=4294967296", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "reserve=101", ".", NULL },
		{ "fsimg", "create", "-n", "-t", "qnx6", "-O", "reserve=1K", ".", NULL },
		{ "fsimg", "info", "-i", "image", "-O", "endian=big", NULL },
		{ "fsimg", "formats", "-m", "16M", NULL },
		{ "fsimg", "info", "-i", "image", "operand", "-j", NULL }
	};
	char *dry_run[] = { "fsimg", "create", "-n", "-R", "-t", "exfat", ".", NULL };
	char *defaults[] = { "fsimg", "info", "-i", "image", "-m", "0", "-l", "fds=0", NULL };
	char *listing[] = { "fsimg", "ls", "-j", "-r", "-i", "image", "--", "/A", NULL };
	char *qnx6[] = { "fsimg", "create", "-n", "-t", "qnx6", "-O", "endian=big",
	    "-O", "uuid=89ABCDEF-0123-4567-89ab-cdef01234567", "-O",
	    "inodes=1000", "-O", "reserve=0", ".", NULL };
	static const unsigned char uuid[16] = {
		0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67
	};
	fsimg_cli_options_t options;
	char label[50];
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
	CHECK(parse(qnx6, &options) == FSIMG_OK);
	CHECK(options.request.write.byte_order == FSIMG_BYTE_ORDER_BIG);
	CHECK(bcmp(options.request.write.volume_uuid, uuid, 16) == 0);
	CHECK(options.request.write.inode_capacity == 1000);
	CHECK(options.request.write.reserve_percent == 0);
	/* One byte more than the longest label any format stores. */
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
	if (json)
		CHECK(strstr(output, "9007199254740993") != NULL);
	else
		CHECK(output[0] == '"');
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
test_volume_controls(void)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO, .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_worker_text_t text;

	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_INFO;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	bcopy("fat\033\n", frame.payload + FSIMG_WIRE_INFO_FORMAT, 6);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(memchr(text.data, '\033', text.length) == NULL);
	CHECK(strstr(display.buffer, "FAT\\u001b\\u000a") != NULL);
}

/* -p imports POSIX metadata on creation, -k skips unsupported kinds on
 * extraction; each is rejected by the commands it does not apply to. The
 * default label applies only to a format whose record has label capacity.
 */
static void
test_posix_options(void)
{
	char *import[] = { "fsimg", "create", "-n", "-p", "-t", "fat32", ".", NULL };
	char *import_read[] = { "fsimg", "ls", "-p", "-i", "image", NULL };
	char *skip[] = { "fsimg", "extract", "-k", "-i", "image", "-C", "out", NULL };
	char *skip_list[] = { "fsimg", "ls", "-k", "-i", "image", NULL };
	char *skip_create[] = { "fsimg", "create", "-n", "-k", "-t", "fat32", ".", NULL };
	char *labelled[] = { "fsimg", "create", "-n", "-L", "DATA", "-t", "exfat", ".", NULL };
	char *reader[] = { "fsimg", "info", "-i", "image", NULL };
	fsimg_cli_options_t options;

	CHECK(parse(import, &options) == FSIMG_OK);
	CHECK((options.request.write.flags & FSIMG_WRITE_IMPORT_POSIX) != 0);
	CHECK(strcmp(options.request.write.label, "FSIMG") == 0);
	CHECK(parse(import_read, &options) == FSIMG_EINVAL);
	CHECK(parse(skip, &options) == FSIMG_OK);
	CHECK((options.request.flags & FSIMG_JOB_SKIP_UNSUPPORTED) != 0);
	CHECK(parse(skip_list, &options) == FSIMG_EINVAL);
	CHECK(parse(skip_create, &options) == FSIMG_EINVAL);
	CHECK(parse(labelled, &options) == FSIMG_OK);
	CHECK(strcmp(options.request.write.label, "DATA") == 0);
	CHECK(parse(reader, &options) == FSIMG_OK);
	CHECK((options.request.write.flags & FSIMG_WRITE_IMPORT_POSIX) == 0);
}

/* Feed one entry's ENTRY, NAME and ENTRY_END frames and collect the text. */
static size_t
list_entry(fsimg_cli_display_t *display, const fsimg_entry_t *entry,
    char *output, size_t capacity)
{
	fsimg_frame_t frame = { .type = FSIMG_FRAME_ENTRY,
	    .length = FSIMG_WIRE_ENTRY_BYTES };
	fsimg_worker_text_t text;
	size_t used;

	used = 0;
	fsimg_wire_entry_encode(frame.payload, entry, strlen(entry->path));
	for (unsigned step = 0; step < 3; step++) {
		if (step == 1) {
			frame.type = FSIMG_FRAME_NAME;
			frame.length = (uint32_t)strlen(entry->path);
			bcopy(entry->path, frame.payload, frame.length);
		} else if (step == 2) {
			frame.type = FSIMG_FRAME_ENTRY_END;
			frame.length = 0;
		}
		CHECK(fsimg_cli_event(display, &frame, &text, NULL) == FSIMG_OK);
		CHECK(used + text.length < capacity);
		bcopy(text.data, output + used, text.length);
		used += text.length;
	}
	output[used] = '\0';
	return (used);
}

/* Human listings mark links and FIFOs after the quoted name, in the style of
 * ls -F; JSON names every kind and carries the POSIX fields or null.
 */
static void
test_posix_listing(bool json)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_entry_t link = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 6 };
	fsimg_entry_t fifo = { .path = "/pipe", .kind = FSIMG_ENTRY_FIFO };
	fsimg_entry_t file = { .path = "/file", .kind = FSIMG_ENTRY_FILE };
	fsimg_entry_t directory = { .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	char output[4096];

	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_LIST;
	options.scratch = "-";
	options.json = json;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	link.metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_CHANGED;
	link.metadata.uid = 1000;
	link.metadata.gid = 20;
	link.metadata.mode = 0777;
	link.metadata.changed = (fsimg_timestamp_t){ .seconds = 86400,
	    .precision_ns = 1, .basis = FSIMG_TIME_UTC };

	list_entry(&display, &link, output, sizeof(output));
	if (json) {
		CHECK(strstr(output, "\"kind\":\"symlink\"") != NULL);
		CHECK(strstr(output, "\"uid\":1000,\"gid\":20,\"mode\":511") != NULL);
		CHECK(strstr(output, "\"changed\":{\"seconds\":\"86400\"") != NULL);
	} else
		CHECK(strcmp(output, "\"/link\"@\n") == 0);
	list_entry(&display, &fifo, output, sizeof(output));
	if (json) {
		CHECK(strstr(output, "\"kind\":\"fifo\"") != NULL);
		CHECK(strstr(output, "\"changed\":null") != NULL);
		CHECK(strstr(output, "\"uid\":null,\"gid\":null,\"mode\":null") != NULL);
	} else
		CHECK(strcmp(output, "\"/pipe\"|\n") == 0);
	list_entry(&display, &file, output, sizeof(output));
	if (json)
		CHECK(strstr(output, "\"kind\":\"file\"") != NULL);
	else
		CHECK(strcmp(output, "\"/file\"\n") == 0);
	list_entry(&display, &directory, output, sizeof(output));
	if (json)
		CHECK(strstr(output, "\"kind\":\"directory\"") != NULL);
	else
		CHECK(strcmp(output, "\"/dir\"\n") == 0);
}

static void
test_posix_volume(bool json)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_frame_t frame = { .type = FSIMG_FRAME_INFO, .length = FSIMG_WIRE_INFO_BYTES };
	fsimg_volume_info_t info;
	fsimg_worker_text_t text;

	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_INFO;
	options.json = json;
	bzero(&info, sizeof(info));
	strcpy(info.format, "qnx6");
	info.valid = FSIMG_VOLUME_UUID;
	for (unsigned i = 0; i < 16; i++)
		info.uuid[i] = (unsigned char)(i * 17);
	info.time_basis = FSIMG_TIME_UTC;
	info.scope = FSIMG_VALIDATION_HEADER;
	fsimg_wire_info_encode(frame.payload, &info);
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	if (json)
		CHECK(strstr(display.buffer,
		    "\"uuid\":\"00112233-4455-6677-8899-aabbccddeeff\"") != NULL);
	else
		CHECK(strstr(display.buffer,
		    "\nUUID:         00112233-4455-6677-8899-aabbccddeeff\n") != NULL);

	info.valid = 0;
	fsimg_wire_info_encode(frame.payload, &info);
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	if (json)
		CHECK(strstr(display.buffer, "\"uuid\":null") != NULL);
	else
		CHECK(strstr(display.buffer, "UUID") == NULL);
}

/* Skipped entries are never silent: a human notice and a JSON count. */
static void
test_skipped_summary(void)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { .entries = 3, .skipped_entries = 2 };
	fsimg_cli_display_t display;
	fsimg_worker_text_t text;

	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_EXTRACT;
	options.name = "extract";
	options.scratch = "-";
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(text.diagnostic);
	CHECK(strstr(display.buffer,
	    "fsimg: warning: 2 entries of unsupported kinds were skipped\n") != NULL);
	result.skipped_entries = 1;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer,
	    "fsimg: warning: 1 entry of an unsupported kind was skipped\n") != NULL);
	result.skipped_entries = 2;

	options.json = true;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "\"skipped_entries\":\"2\"") != NULL);
}

/* formats reports kinds and fields as masks in JSON and by name in detail. */
static void
test_format_masks(bool json)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_worker_text_t text;
	bool details;

	fsimg_worker_request_init(&options.request);
	options.command = FSIMG_CLI_FORMATS;
	options.name = "formats";
	options.scratch = "-";
	options.json = json;
	options.request.flags = FSIMG_JOB_VERBOSE;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	if (json) {
		CHECK(strstr(display.buffer, "\"name\":\"fat12\"") != NULL);
		CHECK(strstr(display.buffer, "\"entry_kinds\":3,\"metadata_fields\":15,") != NULL);
		CHECK(strstr(display.buffer, "\"max_label_bytes\":11") != NULL);
		return;
	}
	details = false;
	while (fsimg_cli_final(&display, &text, NULL) == FSIMG_OK) {
		if (strstr(display.buffer, "fat12:") == NULL)
			continue;
		CHECK(strstr(display.buffer, "\n  Entry kinds: file, directory\n") != NULL);
		CHECK(strstr(display.buffer, "\n  Maximum label: 11 bytes\n") != NULL);
		details = true;
	}
	CHECK(details);
}

static void
test_verification_completion(void)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_frame_t frame = { .type = FSIMG_FRAME_VERIFY, .length = FSIMG_WIRE_VERIFY_BYTES };
	fsimg_worker_text_t text;
	fsimg_error_t error;

	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_VERIFY;
	options.input = "image";
	options.scratch = "-";
	fsimg_wire_put64(frame.payload + FSIMG_WIRE_VERIFY_SCOPE, FSIMG_VALIDATION_STRUCTURAL);
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(text.length == 0);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "filesystem structure verified") != NULL);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_DONE);

	/* A result frame alone does not certify successful worker exit. */
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_event(&display, &frame, &text, NULL) == FSIMG_OK);
	CHECK(text.length == 0);
	fsimg_error_set(&error, FSIMG_EIO, 0, FSIMG_OP_IO, "worker failed");
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(text.diagnostic && strstr(display.buffer, "verified") == NULL);
}

static void
test_final_diagnostics(void)
{
	fsimg_cli_options_t options = { 0 };
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { .metadata_losses = 7 };
	fsimg_cli_display_t display;
	fsimg_worker_text_t text;
	fsimg_error_t error;
	char oversized[24001];

	fsimg_worker_request_init(&options.request);
	options.scratch = "-";
	fsimg_error_set(&error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER, "cannot plan image");
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strcmp(display.buffer, "fsimg: cannot plan image\n") == 0);
	for (unsigned state = FSIMG_OUTPUT_INCOMPLETE; state <= FSIMG_OUTPUT_UNCERTAIN; state++) {
		error.output_state = (fsimg_output_state_t)state;
		error.cleanup_error = EACCES;
		strcpy(paths.retained_output, "private\033output");
		strcpy(paths.retained_scratch, "private\nscratch");
		fsimg_cli_display_init(&display, &options, &result, &paths);
		display.failure = &error;
		CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
		CHECK(text.diagnostic && strstr(display.buffer, "cleanup failed: Permission denied") != NULL);
		CHECK(strstr(display.buffer, "retained output:") != NULL);
		CHECK(strstr(display.buffer, "retained scratch:") != NULL);
		CHECK(strstr(display.buffer, "\\u001b") != NULL);
		CHECK(memchr(text.data, '\033', text.length) == NULL);
		if (state == FSIMG_OUTPUT_INCOMPLETE)
			CHECK(strstr(display.buffer, "output is incomplete") != NULL);
		if (state == FSIMG_OUTPUT_PUBLISHED)
			CHECK(strstr(display.buffer, "output was published") != NULL);
		if (state == FSIMG_OUTPUT_UNCERTAIN)
			CHECK(strstr(display.buffer, "publication or durability is uncertain") != NULL);
	}
	/* Mandatory outcome reporting has its own bound, independent of argv. */
	memset(oversized, '\033', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	options.output = oversized;
	error.output_state = FSIMG_OUTPUT_PUBLISHED;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "output was published") != NULL);
	CHECK(strstr(display.buffer, "[path truncated]") != NULL);
	CHECK(strstr(display.buffer, "retained scratch:") != NULL);

	options.input = oversized;
	options.request.kind = FSIMG_JOB_VERIFY;
	result.metadata_losses = 0;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "[path truncated]") != NULL);
	CHECK(strstr(display.buffer, "verified") != NULL);
	oversized[256] = '\0';
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "[path truncated]") == NULL);
	oversized[256] = '\033';
	oversized[257] = '\0';
	fsimg_cli_display_init(&display, &options, &result, &paths);
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "[path truncated]") != NULL);
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
	test_volume_controls();
	test_posix_options();
	test_posix_listing(false);
	test_posix_listing(true);
	test_posix_volume(false);
	test_posix_volume(true);
	test_skipped_summary();
	test_format_masks(false);
	test_format_masks(true);
	test_final_diagnostics();
	test_verification_completion();
	test_names(false);
	test_names(true);
	test_private_cleanup();
	test_staging_cleanup();
	test_progress();
	return (0);
}
