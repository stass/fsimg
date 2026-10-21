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
static fsimg_status_t parse(char *const *, fsimg_cli_options_t *);
static fsimg_status_t parse_status(char *const *, bool *);
static void check_syntax(char *const *);
static void check_unsupported(char *const *);
static void test_options(void);
static void test_modes(void);
static void test_create_operands(void);
static bool argument_starts(char *const *, const char *);
static bool every_layer_key_in(char *const *const *);
static void test_stacks(void);
static void test_extract_options(void);
static void test_stems(void);
static void test_long_listing(void);
static void test_staged_verification_failure(void);
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

/* Parse a NULL-terminated word list. The caller releases options. */
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

/* Parse, release, and report the status and whether it was a syntax error. */
static fsimg_status_t
parse_status(char *const *arguments, bool *syntax_error)
{
	fsimg_cli_options_t options;
	fsimg_status_t status;

	status = parse(arguments, &options);
	*syntax_error = options.syntax_error;
	fsimg_cli_release(&options);
	return (status);
}

/* Invalid syntax: exit 2 with the synopsis, before anything is opened. */
static void
check_syntax(char *const *arguments)
{
	bool syntax_error;

	if (parse_status(arguments, &syntax_error) != FSIMG_EINVAL || !syntax_error) {
		for (unsigned i = 0; arguments[i] != NULL; i++)
			fprintf(stderr, "%s ", arguments[i]);
		fprintf(stderr, "\n");
	}
	CHECK(syntax_error);
}

/* Recognized but not implemented: exit 1, not a syntax error. */
static void
check_unsupported(char *const *arguments)
{
	bool syntax_error;
	fsimg_status_t status;

	status = parse_status(arguments, &syntax_error);
	if (status != FSIMG_EUNSUPPORTED || syntax_error) {
		for (unsigned i = 0; arguments[i] != NULL; i++)
			fprintf(stderr, "%s ", arguments[i]);
		fprintf(stderr, "\n");
	}
	CHECK(status == FSIMG_EUNSUPPORTED && !syntax_error);
}

static void
test_options(void)
{
	char *invalid[][16] = {
		{ "fsimg", "-i", "-n", "-f", "image", NULL },
		{ "fsimg", "-i", "-f", "image", "--json", "--json", NULL },
		{ "fsimg", "-i", "-f", "image", "-m", "15M", NULL },
		{ "fsimg", "-i", "-f", "image", "-l", "entries=1K", NULL },
		{ "fsimg", "-i", "-f", "image", "-l", "fds=4294967296", NULL },
		{ "fsimg", "-i", "-f", "image", "-l", "work=1", "-l", "work=1", NULL },
		{ "fsimg", "-i", "-f", "image", "-l", "spool=1", "-l", "spool=2", NULL },
		{ "fsimg", "-i", "-f", "image", "-o", "oem=latin1", NULL },
		{ "fsimg", "-i", "-f", "image", "-o", "oem=cp437", "-o", "oem=cp437", NULL },
		{ "fsimg", "-i", "-f", "image", "-o", "sector=512", NULL },
		{ "fsimg", "-i", "-f", "image", "-o", "endian=big", NULL },
		{ "fsimg", "-i", "-f", "image", "operand", NULL },
		{ "fsimg", "-i", NULL },
		{ "fsimg", "-i", "-f", "", NULL },
		{ "fsimg", "-t", "-f", "image", "-d", NULL },
		{ "fsimg", "-c", "-F", "exfat", "-f", "image", "-e", "315532800", ".", NULL },
		{ "fsimg", "-c", "-F", "exfat", ".", NULL },
		{ "fsimg", "-cn", ".", NULL },
		{ "fsimg", "-cn", "-F", "exfat", "-o", "cluster=3K", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat16", "-o", "cluster=64K", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "endian=middle", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "uuid=1-2-3-4-5", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o",
		    "uuid=11111111-2222-3333-4444-55555555555g", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o",
		    "uuid=11111111+2222-3333-4444-555555555555", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o",
		    "uuid=11111111-2222-3333-4444-5555555555550", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "inodes=0", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "inodes=4294967296", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "reserve=101", ".", NULL },
		{ "fsimg", "-cn", "-F", "qnx6", "-o", "reserve=1K", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-M", "-s", "64M", ".", NULL },
		{ "fsimg", "--formats", "-m", "16M", NULL }
	};
	char *dry_run[] = { "fsimg", "-cnR", "-F", "exfat", ".", NULL };
	char *defaults[] = { "fsimg", "-i", "-f", "image", "-m", "0", "-l", "fds=0",
	    "-l", "spool=1M", NULL };
	char *listing[] = { "fsimg", "tf", "image", "--json", "--", "/A", NULL };
	char *flat[] = { "fsimg", "-t", "--no-recursion", "-f", "image", "A", NULL };
	char *qnx6[] = { "fsimg", "-cn", "-F", "qnx6", "-o", "endian=big",
	    "-o", "uuid=89ABCDEF-0123-4567-89ab-cdef01234567", "-o",
	    "inodes=1000", "-o", "reserve=0", ".", NULL };
	char *window[] = { "fsimg", "-i", "-f", "image", "--offset=512",
	    "--length", "1M", NULL };
	static const unsigned char uuid[16] = {
		0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67
	};
	fsimg_cli_options_t options;
	char label[50];
	char *large_label[] = { "fsimg", "-cn", "-F", "exfat", "-L", label, ".", NULL };

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_syntax(invalid[i]);
	CHECK(parse(dry_run, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_PLAN && options.image == NULL);
	CHECK(strcmp(options.mode, "c") == 0);
	CHECK(strcmp(options.request.write.label, "FSIMG") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(defaults, &options) == FSIMG_OK);
	CHECK(options.request.limits.memory_bytes == UINT64_C(256) * 1024 * 1024);
	CHECK(options.request.limits.max_open_files == 128);
	CHECK(options.request.limits.spool_bytes == UINT64_C(1024) * 1024);
	CHECK(options.request.kind == FSIMG_JOB_INFO && strcmp(options.mode, "i") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(listing, &options) == FSIMG_OK);
	CHECK(options.json && (options.request.flags & FSIMG_JOB_RECURSIVE) != 0);
	CHECK(strcmp(options.request.selection, "/A") == 0);
	CHECK(strcmp(options.image, "image") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(flat, &options) == FSIMG_OK);
	CHECK((options.request.flags & FSIMG_JOB_RECURSIVE) == 0);
	CHECK(strcmp(options.request.selection, "/A") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(qnx6, &options) == FSIMG_OK);
	CHECK(options.request.write.byte_order == FSIMG_BYTE_ORDER_BIG);
	CHECK(bcmp(options.request.write.volume_uuid, uuid, 16) == 0);
	CHECK(options.request.write.inode_capacity == 1000);
	CHECK(options.request.write.reserve_percent == 0);
	fsimg_cli_release(&options);
	CHECK(parse(window, &options) == FSIMG_OK);
	CHECK(options.request.offset == 512);
	CHECK(options.request.length == UINT64_C(1024) * 1024);
	fsimg_cli_release(&options);
	/* One byte more than the longest label any format stores. */
	memset(label, 'A', sizeof(label) - 1);
	label[sizeof(label) - 1] = '\0';
	check_syntax(large_label);
}

/* Mode selection: exactly one mode, or c with W; a dashless first argument
 * is a bundle whose argument letters take the following words in order.
 */
static void
test_modes(void)
{
	char *invalid[][12] = {
		{ "fsimg", NULL },
		{ "fsimg", "-f", "image", NULL },
		{ "fsimg", "-c", "-x", "-f", "image", ".", NULL },
		{ "fsimg", "-t", "-W", "-f", "image", NULL },
		{ "fsimg", "--info", "-W", "-f", "image", NULL },
		{ "fsimg", "-V", "-v", NULL },
		{ "fsimg", "-c", "-V", NULL },
		{ "fsimg", "-h", "operand", NULL },
		{ "fsimg", "--formats", "operand", NULL },
		{ "fsimg", "cf", NULL },
		{ "fsimg", "cqf", "image", ".", NULL },
		{ "fsimg", "x-f", "image.img", NULL },
		{ "fsimg", "xvvvf", "image.img", NULL },
		{ "fsimg", "-t", "--bogus", "-f", "image", NULL },
		{ "fsimg", "-t", "-f", "image", "", NULL },
		{ "fsimg", "-tf", NULL }
	};
	char *version[] = { "fsimg", "-V", NULL };
	char *help[] = { "fsimg", "x", "-h", NULL };
	char *overview_help[] = { "fsimg", "-h", NULL };
	char *formats[] = { "fsimg", "--formats", "-v", NULL };
	char *bundle[] = { "fsimg", "cvvfF", "out.img", "fat32", "-L", "BOOT", ".", NULL };
	char *combined[] = { "fsimg", "-W", "-c", "-F", "fat32", "-f", "out.img", ".", NULL };
	char *dashed[] = { "fsimg", "-cWf", "out.img", "-F", "fat32", ".", NULL };
	char *second_word[] = { "fsimg", "-t", "-f", "image", "xf", NULL };
	char *late_option[] = { "fsimg", "-t", "-f", "image", "/A", "--json", NULL };
	char *info[] = { "fsimg", "--info", "-v", "-f", "image", NULL };
	char *verify[] = { "fsimg", "Wdf", "image", NULL };
	fsimg_cli_options_t options;
	bool syntax_error;

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_syntax(invalid[i]);
	CHECK(parse(invalid[0], &options) == FSIMG_EINVAL);
	CHECK(options.overview && options.command == FSIMG_CLI_USAGE);
	fsimg_cli_release(&options);
	CHECK(parse(version, &options) == FSIMG_OK);
	CHECK(options.command == FSIMG_CLI_VERSION);
	fsimg_cli_release(&options);
	CHECK(parse(help, &options) == FSIMG_OK);
	CHECK(options.command == FSIMG_CLI_HELP && strcmp(options.mode, "x") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(overview_help, &options) == FSIMG_OK);
	CHECK(options.command == FSIMG_CLI_HELP && options.mode[0] == '\0');
	fsimg_cli_release(&options);
	CHECK(parse(formats, &options) == FSIMG_OK);
	CHECK(options.command == FSIMG_CLI_FORMATS && options.verbosity == 1);
	fsimg_cli_release(&options);

	CHECK(parse(bundle, &options) == FSIMG_OK);
	CHECK(strcmp(options.mode, "c") == 0 && strcmp(options.image, "out.img") == 0);
	CHECK(strcmp(fsimg_job_format(&options.request), "fat32") == 0);
	CHECK(strcmp(options.request.write.label, "BOOT") == 0);
	CHECK(options.verbosity == 2 && (options.request.flags & FSIMG_JOB_VERBOSE) != 0);
	CHECK(options.operand_count == 1 && options.request.kind == FSIMG_JOB_CREATE);
	fsimg_cli_release(&options);
	CHECK(parse(combined, &options) == FSIMG_OK);
	CHECK(strcmp(options.mode, "cW") == 0 && options.verify_staged);
	fsimg_cli_release(&options);
	CHECK(parse(dashed, &options) == FSIMG_OK);
	CHECK(strcmp(options.mode, "cW") == 0 && strcmp(options.image, "out.img") == 0);
	fsimg_cli_release(&options);
	/* Only the first argument is a bundle; later dashless words are operands. */
	CHECK(parse(second_word, &options) == FSIMG_OK);
	CHECK(strcmp(options.member, "/xf") == 0);
	fsimg_cli_release(&options);
	/* Options end at the first operand, so a later option word is a member. */
	CHECK(parse_status(late_option, &syntax_error) == FSIMG_EUNSUPPORTED);
	CHECK(!syntax_error);
	CHECK(parse(info, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_INFO && strcmp(options.mode, "i") == 0);
	CHECK((options.request.flags & FSIMG_JOB_VERBOSE) != 0);
	fsimg_cli_release(&options);
	CHECK(parse(verify, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_VERIFY &&
	    (options.request.flags & FSIMG_JOB_PAYLOAD) != 0);
	fsimg_cli_release(&options);
}

/* Create operands: each is opened relative to the -C in force when it
 * appeared, resolved against the working directory, and populates the image
 * path of its own normalized components.
 */
static void
test_create_operands(void)
{
	char *invalid[][12] = {
		{ "fsimg", "-cn", "-F", "fat32", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-C", "dir", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a", "-C", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a", "-C", "dir", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a", "-C", "x", "-C", "y", "b", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-C", "x", "-C", "y", "a", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a/../b", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "..", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a", "-Cdir", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-C", "", "a", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "a", "-C", "", "b", NULL }
	};
	char *operands[] = { "fsimg", "-cn", "-F", "fat32", "-C", "dir", "a//./b/",
	    "-Cother", "c", "/abs", ".", "--", "-C", "x", NULL };
	fsimg_cli_options_t options;

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_syntax(invalid[i]);
	CHECK(parse(operands, &options) == FSIMG_OK);
	CHECK(options.operand_count == 6);
	CHECK(strcmp(options.operands[0].directory, "dir") == 0);
	CHECK(strcmp(options.operands[0].host_path, "a//./b/") == 0);
	CHECK(strcmp(options.operands[0].image_path, "/a/b") == 0);
	CHECK(strcmp(options.operands[1].directory, "other") == 0);
	CHECK(strcmp(options.operands[1].image_path, "/c") == 0);
	CHECK(strcmp(options.operands[2].image_path, "/abs") == 0);
	CHECK(strcmp(options.operands[3].image_path, "/") == 0);
	CHECK(strcmp(options.operands[3].directory, "other") == 0);
	/* After "--" a -C word is an operand like any other. */
	CHECK(strcmp(options.operands[4].host_path, "-C") == 0);
	CHECK(strcmp(options.operands[4].image_path, "/-C") == 0);
	CHECK(strcmp(options.operands[5].image_path, "/x") == 0);
	fsimg_cli_release(&options);
}

/* Whether an argument of a command line begins with prefix. */
static bool
argument_starts(char *const *arguments, const char *prefix)
{
	for (; *arguments != NULL; arguments++) {
		if (strncmp(*arguments, prefix, strlen(prefix)) == 0)
			return (true);
	}
	return (false);
}

/* Whether every "layer:key" the layer registry lists is set, as
 * "layer:key=...", by one of the NULL-terminated list of command lines.
 */
static bool
every_layer_key_in(char *const *const *commands)
{
	bool found;

	fsimg_layer_info_t info;
	const char *key, *end;
	size_t cursor;
	char prefix[64];

	cursor = 0;
	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		for (key = info.settings; *key != '\0'; key = end) {
			end = key + strcspn(key, ",");
			snprintf(prefix, sizeof(prefix), "%s:%.*s=", info.name,
			    (int)(end - key), key);
			found = false;
			for (unsigned i = 0; commands[i] != NULL; i++)
				found |= argument_starts(commands[i], prefix);
			if (!found) {
				fprintf(stderr, "no command line sets %s\n", prefix);
				return (false);
			}
			if (*end == ',')
				end++;
		}
	}
	return (true);
}

/* The -F stack: a filesystem name with optional empty slots and the
 * qualified settings of each named layer. Layers the grammar recognizes
 * but this build does not provide fail with exit 1.
 */
#define LONG_TEXT "0123456789012345678901234567890123456789"
static void
test_stacks(void)
{
	char *invalid[][12] = {
		{ "fsimg", "-cn", "-F", "fat32/fat32", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/-/-/-", ".", NULL },
		{ "fsimg", "-cn", "-F", "dmg/gpt", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-o", "gpt:align=1M", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-o", "nolayer:key=1", ".", NULL },
		{ "fsimg", "-cza", "-F", "fat32", "-f", "out.img", ".", NULL },
		{ "fsimg", "-ca", "-F", "fat32", "-f", "-", ".", NULL },
		{ "fsimg", "cW", "-F", "fat32", "-f", "-", ".", NULL },
		{ "fsimg", "-c", "--json", "-F", "fat32", "-f", "-", ".", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "1", "--offset", "0", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-o", "gzip:level=9", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "mbr:x=1", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32", "-o", "oem=cp437", ".", NULL },
		{ "fsimg", "-t", "-f", "image", "-o", "gpt:sector=4096", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "fat32/gpt", "-o", "gpt:sector=1024", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "fat32/gpt", "-o", "gpt:type=esp", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "gpt", "-o", "gpt:sector=512",
		    "-o", "gpt:sector=512", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "0", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "4294967296", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "name=", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "type=bogus", NULL },
		{ "fsimg", "-t", "-f", "image", "--partition", "label=EFI", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "fat32/-", "--partition", "1", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "fat32/gpt", "--offset", "1M", NULL },
		{ "fsimg", "-t", "-f", "image", "-F", "gpt", "-o", "gpt:sector=512",
		    "--offset", "0", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "gpt:align=1M", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "gpt:disk=0", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o",
		    "gpt:guid=00000000-0000-0000-0000-000000000000", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "gpt:disk-guid=1234", ".",
		    NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "gpt:type=" LONG_TEXT, ".",
		    NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "mbr:id=12", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "mbr:id=0x123456789", ".",
		    NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "mbr:id=0x", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "mbr:active=maybe", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "mbr:type=0x0c", "-o",
		    "mbr:type=0x0b", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/mbr", "-o", "gpt:sector=4096", ".", NULL },
		{ "fsimg", "-cn", "-F", "exfat/gpt/vhd", "-o", "vhd:x=1", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/dmg", "-o", "dmg:compress=lzfse", ".",
		    NULL },
		{ "fsimg", "-cn", "-F", "fat32/dmg", "-o", "dmg:level=0", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/dmg", "-o", "dmg:level=6K", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/dmg", "-o", "dmg:chunk=8G", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/dmg", "-o", "dmg:uuid=nil", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/vhd", "-o", "vhd:align=0", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/simg", "-o", "simg:crc=1", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt", "-o", "dmg:level=1", ".", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32/gpt", "-f", "out.dmg", ".", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32/vhd", "-f", "out.dmg.gz", ".",
		    NULL },
		{ "fsimg", "-cz", "-a", "-F", "fat32", "-f", "out.img.zst", ".",
		    NULL },
		{ "fsimg", "-cW", "-F", "fat32/dmg", "-f", "-", ".", NULL },
		{ "fsimg", "-c", "--json", "-F", "fat32/dmg", "-f", "-", ".", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32/dmg", "-f", "-", ".", NULL },
		{ "fsimg", "-c", "-T", "-", "-F", "fat32/dmg", "-f", "-", ".", NULL },
		{ "fsimg", "-cz", "-F", "fat32", "-o", "gzip:level=0", "-f", "o.gz", ".",
		    NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32", "-o", "gzip:level=9", "-f",
		    "out.img", ".", NULL },
		{ "fsimg", "-cWz", "-T", "-", "-F", "fat32", "-f", "out.img.gz", ".",
		    NULL }
	};
	char *unsupported[][12] = {
		{ "fsimg", "-cn", "-F", "hfs", ".", NULL },
		{ "fsimg", "-cM", "-F", "fat32", "-f", "out.img", ".", NULL },
		{ "fsimg", "-t", "-f", "image", "/a", "/b", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32", "-f", "out.img.zst", ".", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32", "-f", "out.img.XZ", ".", NULL }
	};
	/* Containers, gzip and streams are written; with -a a container
	 * suffix must name the stack's container and selects nothing more.
	 */
	char *accepted[][12] = {
		{ "fsimg", "-cn", "-F", "fat32/-/vhd", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/gpt/dmg", ".", NULL },
		{ "fsimg", "-cn", "-F", "fat32/-/simg", ".", NULL },
		{ "fsimg", "-cn", "-F", "exfat/gpt/vhd", "-o", "gpt:type=esp", ".", NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32/gpt/dmg", "-f", "out.DMG", ".",
		    NULL },
		{ "fsimg", "-c", "-a", "-F", "fat32", "-f", "out.img", ".", NULL },
		{ "fsimg", "-cz", "-F", "fat32", "-f", "-", ".", NULL },
		{ "fsimg", "-cWz", "-a", "-F", "fat32/dmg", "-f", "out.dmg.gz", ".",
		    NULL }
	};
	char *gzip_settings[] = { "fsimg", "-cnz", "-F", "fat32", "-o",
	    "gzip:level=9", ".", NULL };
	char *gzip_suffix[] = { "fsimg", "-c", "-a", "-F", "fat32/gpt", "-f",
	    "disk.img.gz", ".", NULL };
	char *stream[] = { "fsimg", "-c", "-F", "fat32/vhd", "-f", "-", ".", NULL };
	char *dmg_settings[] = { "fsimg", "-cn", "-F", "fat32/dmg", "-o",
	    "dmg:compress=zlib", "-o", "dmg:level=9", "-o", "dmg:chunk=512K", "-o",
	    "dmg:uuid=11111111-2222-4333-8444-555555555555", ".", NULL };
	char *vhd_settings[] = { "fsimg", "-cn", "-F", "fat32/vhd", "-o",
	    "vhd:align=1M", "-o", "vhd:uuid=66666666-7777-4888-8999-aaaaaaaaaaaa",
	    ".", NULL };
	char *simg_settings[] = { "fsimg", "-cn", "-F", "fat32/simg", "-o",
	    "simg:block=64K", "-o", "simg:crc=yes", ".", NULL };
	char *empty_slots[] = { "fsimg", "-cn", "-F", "fat32/-/-", ".", NULL };
	char *gpt_settings[] = { "fsimg", "-cn", "-F", "fat32/gpt", "-o",
	    "gpt:type=esp", "-o", "gpt:name=EFI", "-o", "gpt:disk=64M", "-o",
	    "gpt:disk-guid=11111111-2222-4333-8444-555555555555", "-o",
	    "gpt:guid=66666666-7777-4888-8999-aaaaaaaaaaaa", "-o",
	    "gpt:sector=4096", ".", NULL };
	char *mbr_settings[] = { "fsimg", "-cn", "-F", "fat16/mbr", "-o",
	    "mbr:id=0xDEADbeef", "-o", "mbr:active=yes", "-o", "mbr:type=0x0e",
	    "-o", "mbr:disk=1G", ".", NULL };
	fsimg_layer_options_t *layers;
	char *sector[] = { "fsimg", "-t", "-f", "image", "-F", "gpt", "-o",
	    "gpt:sector=4096", NULL };
	char *by_index[] = { "fsimg", "-t", "-f", "image", "--partition", "2", NULL };
	char *by_name[] = { "fsimg", "-x", "-f", "disk.img", "--partition",
	    "name=EFI system", NULL };
	char *by_type[] = { "fsimg", "-W", "-f", "image", "--partition", "type=0x0c",
	    NULL };
	char *by_range[] = { "fsimg", "-i", "-f", "image", "--offset", "1M", NULL };
	char *container[] = { "fsimg", "-t", "-f", "image", "-F", "fat32/gpt/dmg",
	    NULL };
	fsimg_cli_options_t options;

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_syntax(invalid[i]);
	for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++)
		check_unsupported(unsupported[i]);
	CHECK(parse(gpt_settings, &options) == FSIMG_OK);
	layers = &options.request.layers;
	CHECK(strcmp(layers->gpt_type, "esp") == 0 &&
	    strcmp(layers->gpt_name, "EFI") == 0);
	CHECK(layers->gpt_disk_bytes == UINT64_C(64) << 20 &&
	    layers->gpt_sector_bytes == 4096);
	CHECK(layers->gpt_disk_guid[0] == 0x11 && layers->gpt_disk_guid[15] == 0x55);
	CHECK(layers->gpt_guid[0] == 0x66 && layers->gpt_guid[15] == 0xaa);
	CHECK(!layers->mbr_id_set && layers->mbr_disk_bytes == 0);
	fsimg_cli_release(&options);
	/* Every key the library's settings table lists is one the two
	 * commands above set, so a library setting the CLI cannot parse fails
	 * here.
	 */
	CHECK(every_layer_key_in((char *const *const []){ gpt_settings,
	    mbr_settings, dmg_settings, vhd_settings, simg_settings,
	    gzip_settings, NULL }));
	CHECK(parse(mbr_settings, &options) == FSIMG_OK);
	layers = &options.request.layers;
	CHECK(layers->mbr_id == 0xdeadbeef && layers->mbr_id_set &&
	    layers->mbr_active && strcmp(layers->mbr_type, "0x0e") == 0);
	CHECK(layers->mbr_disk_bytes == UINT64_C(1) << 30);
	fsimg_cli_release(&options);
	CHECK(parse(dmg_settings, &options) == FSIMG_OK);
	layers = &options.request.layers;
	CHECK(layers->dmg_compress == FSIMG_DMG_ZLIB && layers->dmg_level == 9 &&
	    layers->dmg_chunk_bytes == 512 * 1024 &&
	    layers->dmg_uuid[0] == 0x11 && layers->dmg_uuid[15] == 0x55);
	fsimg_cli_release(&options);
	CHECK(parse(vhd_settings, &options) == FSIMG_OK);
	layers = &options.request.layers;
	CHECK(layers->vhd_align_bytes == 1024 * 1024 &&
	    layers->vhd_uuid[0] == 0x66 && layers->vhd_uuid[15] == 0xaa);
	fsimg_cli_release(&options);
	CHECK(parse(simg_settings, &options) == FSIMG_OK);
	layers = &options.request.layers;
	CHECK(layers->simg_block_bytes == 64 * 1024 && layers->simg_crc);
	fsimg_cli_release(&options);
	/* -z, or -a with a .gz name, selects gzip and its level. */
	CHECK(parse(gzip_settings, &options) == FSIMG_OK);
	CHECK(options.request.layers.gzip_level == 9 &&
	    (options.request.write.flags & FSIMG_WRITE_GZIP) != 0);
	fsimg_cli_release(&options);
	CHECK(parse(gzip_suffix, &options) == FSIMG_OK);
	CHECK((options.request.write.flags & FSIMG_WRITE_GZIP) != 0 &&
	    fsimg_job_needs_raw(&options.request));
	fsimg_cli_release(&options);
	for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
		CHECK(parse(accepted[i], &options) == FSIMG_OK);
		fsimg_cli_release(&options);
	}
	/* A creation on standard output streams; nothing is staged. */
	CHECK(parse(stream, &options) == FSIMG_OK);
	CHECK((options.request.flags & FSIMG_JOB_STDOUT) != 0);
	fsimg_cli_release(&options);
	CHECK(parse(empty_slots, &options) == FSIMG_OK);
	CHECK(strcmp(fsimg_job_format(&options.request), "fat32") == 0);
	CHECK(options.request.stack.slots[FSIMG_LAYER_SCHEME].state == FSIMG_SLOT_EMPTY);
	fsimg_cli_release(&options);
	CHECK(parse(sector, &options) == FSIMG_OK);
	CHECK(options.request.layers.gpt_sector_bytes == 4096 &&
	    strcmp(options.request.stack.slots[FSIMG_LAYER_SCHEME].name, "gpt") == 0 &&
	    options.request.stack.slots[FSIMG_LAYER_FILESYSTEM].state ==
	    FSIMG_SLOT_UNSPECIFIED);
	fsimg_cli_release(&options);
	CHECK(parse(by_index, &options) == FSIMG_OK);
	CHECK(options.request.selector == FSIMG_SELECT_INDEX &&
	    options.request.partition_index == 2);
	fsimg_cli_release(&options);
	CHECK(parse(by_name, &options) == FSIMG_OK);
	CHECK(options.request.selector == FSIMG_SELECT_NAME &&
	    strcmp(options.request.partition_text, "EFI system") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(by_type, &options) == FSIMG_OK);
	CHECK(options.request.selector == FSIMG_SELECT_TYPE &&
	    strcmp(options.request.partition_text, "0x0c") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(by_range, &options) == FSIMG_OK);
	CHECK(options.request.selector == FSIMG_SELECT_RANGE &&
	    options.request.offset == 1048576);
	fsimg_cli_release(&options);
	/* A forced container is read; only writing one is refused. */
	CHECK(parse(container, &options) == FSIMG_OK);
	CHECK(strcmp(options.request.stack.slots[FSIMG_LAYER_CONTAINER].name,
	    "dmg") == 0);
	fsimg_cli_release(&options);
}

/* x creates a fresh directory named after the image unless -E or -O. */
static void
test_extract_options(void)
{
	char *invalid[][12] = {
		{ "fsimg", "xf", "image", NULL },
		{ "fsimg", "xf", ".img", NULL },
		{ "fsimg", "xOf", "image", NULL },
		{ "fsimg", "xOf", "image", "/a", "/b", NULL },
		{ "fsimg", "xOf", "image", "-C", "out", "/a", NULL },
		{ "fsimg", "xOf", "image", "--json", "/a", NULL },
		{ "fsimg", "xOEf", "image", "/a", NULL },
		{ "fsimg", "xOkf", "image", "/a", NULL },
		{ "fsimg", "xf", "image.img", "-C", "a", "-C", "b", NULL },
		{ "fsimg", "tf", "image", "-C", "out", NULL },
		{ "fsimg", "xf", "image.img", "-W", NULL },
		{ "fsimg", "xf", "image.img", "../x", NULL },
		{ "fsimg", "tf", "image", "EFI/../x", NULL },
		{ "fsimg", "xOf", "image", "", NULL },
		{ "fsimg", "xEf", "image", "-C", "", NULL }
	};
	char *fresh[] = { "fsimg", "xkf", "dir/Boot.IMG.gz", "-C", "out", NULL };
	char *here[] = { "fsimg", "xEf", "image", NULL };
	char *standard_output[] = { "fsimg", "xOf", "image", "EFI//./a.txt", NULL };
	fsimg_cli_options_t options;

	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_syntax(invalid[i]);
	CHECK(parse(fresh, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_EXTRACT && strcmp(options.mode, "x") == 0);
	CHECK(strcmp(options.stem, "Boot") == 0 && !options.extract_here);
	CHECK(strcmp(options.directory, "out") == 0);
	CHECK((options.request.flags & FSIMG_JOB_SKIP_UNSUPPORTED) != 0);
	fsimg_cli_release(&options);
	CHECK(parse(here, &options) == FSIMG_OK);
	CHECK(options.extract_here && options.directory == NULL);
	fsimg_cli_release(&options);
	CHECK(parse(standard_output, &options) == FSIMG_OK);
	CHECK(options.request.kind == FSIMG_JOB_CAT);
	CHECK(strcmp(options.member, "/EFI/a.txt") == 0);
	CHECK(strcmp(options.request.selection, "/EFI/a.txt") == 0);
	fsimg_cli_release(&options);
}

/* The extraction stem strips one compression, one container and one generic
 * suffix, outside in, each only when it is outermost at its turn.
 */
static void
test_stems(void)
{
	static const struct {
		const char	*image;
		const char	*stem;
	} cases[] = {
		{ "boot.img", "boot" },
		{ "dir/boot.img.gz", "boot" },
		{ "BOOT.IMG.GZ", "BOOT" },
		{ "disk.raw.dmg", "disk" },
		{ "disk.dmg.gz", "disk" },
		{ "rootfs.vhd.img", "rootfs.vhd" },
		{ "boot.gz.img", "boot.gz" },
		{ "a.img.img", "a.img" },
		{ "card.simg", "card" },
		{ "part.bin", "part" },
		{ "part.dd", "part" },
		{ "image", NULL },
		{ ".img", NULL },
		{ "..img", NULL },
		{ "...img", ".." },
		{ "dir/", NULL },
		{ "-", NULL },
		{ "x.tar", NULL }
	};
	char stem[NAME_MAX + 1];
	char small[4];

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].stem == NULL ||
		    strcmp(cases[i].stem, "..") == 0) {
			CHECK(!fsimg_cli_stem(cases[i].image, stem, sizeof(stem)));
			continue;
		}
		CHECK(fsimg_cli_stem(cases[i].image, stem, sizeof(stem)));
		CHECK(strcmp(stem, cases[i].stem) == 0);
	}
	CHECK(!fsimg_cli_stem("abcd.img", small, sizeof(small)));
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
		options.image = input;
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
	char *import[] = { "fsimg", "-cnp", "-F", "fat32", ".", NULL };
	char *import_read[] = { "fsimg", "-tp", "-f", "image", NULL };
	char *skip_list[] = { "fsimg", "-tk", "-f", "image", NULL };
	char *skip_create[] = { "fsimg", "-cnk", "-F", "fat32", ".", NULL };
	char *labelled[] = { "fsimg", "-cn", "-L", "DATA", "-F", "exfat", ".", NULL };
	char *reader[] = { "fsimg", "-i", "-f", "image", NULL };
	fsimg_cli_options_t options;

	CHECK(parse(import, &options) == FSIMG_OK);
	CHECK((options.request.write.flags & FSIMG_WRITE_IMPORT_POSIX) != 0);
	CHECK(strcmp(options.request.write.label, "FSIMG") == 0);
	fsimg_cli_release(&options);
	check_syntax(import_read);
	check_syntax(skip_list);
	check_syntax(skip_create);
	CHECK(parse(labelled, &options) == FSIMG_OK);
	CHECK(strcmp(options.request.write.label, "DATA") == 0);
	fsimg_cli_release(&options);
	CHECK(parse(reader, &options) == FSIMG_OK);
	CHECK((options.request.write.flags & FSIMG_WRITE_IMPORT_POSIX) == 0);
	fsimg_cli_release(&options);
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

/* -v adds the mode, size and modification-time columns before the name,
 * with no locale or host timezone; absent fields print '?' or '-'.
 */
static void
test_long_listing(void)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_entry_t file = { .path = "/file", .kind = FSIMG_ENTRY_FILE,
	    .size = 1234 };
	fsimg_entry_t link = { .path = "/link", .kind = FSIMG_ENTRY_SYMLINK,
	    .size = 6 };
	fsimg_entry_t directory = { .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY };
	fsimg_entry_t extreme = { .path = "/x", .kind = FSIMG_ENTRY_FIFO };
	char output[4096];

	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_LIST;
	options.scratch = "-";
	options.verbosity = 1;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	file.metadata.valid = FSIMG_META_MODE | FSIMG_META_MODIFIED;
	file.metadata.mode = 0644;
	file.metadata.modified = (fsimg_timestamp_t){ .seconds = 951782400,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
	list_entry(&display, &file, output, sizeof(output));
	CHECK(strcmp(output,
	    "-rw-r--r--         1234 2000-02-29 00:00:00 \"/file\"\n") == 0);
	link.metadata.valid = FSIMG_META_MODE | FSIMG_META_MODIFIED;
	link.metadata.mode = 0777;
	link.metadata.modified = (fsimg_timestamp_t){ .seconds = -1,
	    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
	list_entry(&display, &link, output, sizeof(output));
	CHECK(strcmp(output,
	    "lrwxrwxrwx            6 1969-12-31 23:59:59 \"/link\"@\n") == 0);
	list_entry(&display, &directory, output, sizeof(output));
	CHECK(strcmp(output,
	    "d?????????            - -                   \"/dir\"\n") == 0);
	extreme.metadata.valid = FSIMG_META_MODIFIED;
	for (unsigned i = 0; i < 2; i++) {
		extreme.metadata.modified = (fsimg_timestamp_t){
		    .seconds = i == 0 ? INT64_MIN : INT64_MAX,
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
		list_entry(&display, &extreme, output, sizeof(output));
		CHECK(strcmp(output, i == 0 ?
		    "p?????????            - -292277022657-01-27 08:29:52 \"/x\"|\n" :
		    "p?????????            - 292277026596-12-04 15:30:07 \"/x\"|\n") == 0);
	}
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
	options.mode = "x";
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
	options.mode = "formats";
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
	options.image = "image";
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
	options.image = oversized;
	error.output_state = FSIMG_OUTPUT_PUBLISHED;
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "output was published") != NULL);
	CHECK(strstr(display.buffer, "[path truncated]") != NULL);
	CHECK(strstr(display.buffer, "retained scratch:") != NULL);

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

/* cW: a staged image failing verification is reported as unpublished. */
static void
test_staged_verification_failure(void)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths = { 0 };
	fsimg_worker_result_t result = { 0 };
	fsimg_cli_display_t display;
	fsimg_worker_text_t text;
	fsimg_error_t error;

	bzero(&options, sizeof(options));
	fsimg_worker_request_init(&options.request);
	options.request.kind = FSIMG_JOB_CREATE;
	CHECK(fsimg_stack_parse("fat32", &options.request.stack, NULL) == FSIMG_OK);
	options.mode = "cW";
	options.image = "out.img";
	options.scratch = "-";
	paths.verification_failed = true;
	fsimg_error_set(&error, FSIMG_ECORRUPT, 0, FSIMG_OP_CONTEXT,
	    "FAT copies differ");
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "FAT copies differ") != NULL);
	CHECK(strstr(display.buffer, "fsimg: the staged fat32 volume failed "
	    "verification; nothing was published\n") != NULL);
	CHECK(strstr(display.buffer, "output was published") == NULL);

	/* A layer's failure names the layer, which the library puts in
	 * error.path: a table failure the scheme, a container failure the
	 * container; a filesystem failure names the volume.
	 */
	CHECK(fsimg_stack_parse("fat32/gpt/dmg", &options.request.stack, NULL) ==
	    FSIMG_OK);
	fsimg_error_set(&error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    "GPT primary entry array CRC32 mismatch");
	snprintf(error.path, sizeof(error.path), "gpt");
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "fsimg: GPT primary entry array CRC32 "
	    "mismatch \"gpt\"\n") != NULL);
	CHECK(strstr(display.buffer, "fsimg: the staged gpt partition table "
	    "failed verification; nothing was published\n") != NULL);
	fsimg_error_set(&error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    "DMG data fork checksum does not match");
	snprintf(error.path, sizeof(error.path), "dmg");
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "fsimg: the staged dmg container "
	    "failed verification; nothing was published\n") != NULL);
	fsimg_error_set(&error, FSIMG_ECORRUPT, 0, FSIMG_OP_VERIFY,
	    "FAT copies differ");
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.failure = &error;
	CHECK(fsimg_cli_final(&display, &text, NULL) == FSIMG_OK);
	CHECK(strstr(display.buffer, "fsimg: the staged fat32 volume failed "
	    "verification; nothing was published\n") != NULL);
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
	char image_root[] = "/";
	fsimg_cli_operand_t operand = { .host_path = root, .image_path = image_root };
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
	options.operands = &operand;
	options.operand_count = 1;
	options.image = output;
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
	test_modes();
	test_create_operands();
	test_stacks();
	test_extract_options();
	test_stems();
	test_long_listing();
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
	test_staged_verification_failure();
	return (0);
}
