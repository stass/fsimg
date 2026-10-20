/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cli.h"
#include "host.h"
#include "internal.h"

typedef enum cli_limit_key {
	CLI_LIMIT_SCRATCH, CLI_LIMIT_ENTRIES, CLI_LIMIT_DEPTH, CLI_LIMIT_PATH,
	CLI_LIMIT_FDS, CLI_LIMIT_OUTPUT, CLI_LIMIT_WORK, CLI_LIMIT_SECONDS,
	CLI_LIMIT_COUNT
} cli_limit_key_t;

typedef enum cli_setting_key {
	CLI_SETTING_SECTOR, CLI_SETTING_CLUSTER, CLI_SETTING_OEM,
	CLI_SETTING_ENDIAN, CLI_SETTING_UUID, CLI_SETTING_INODES,
	CLI_SETTING_RESERVE, CLI_SETTING_COUNT
} cli_setting_key_t;

static fsimg_status_t syntax(fsimg_error_t *, const char *);
static fsimg_status_t parse_limit(fsimg_cli_options_t *, const char *, unsigned *,
    fsimg_error_t *);
static fsimg_status_t parse_uuid(const char *, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t parse_setting(fsimg_cli_options_t *, const char *,
    unsigned *, fsimg_error_t *);
static fsimg_status_t validate_options(fsimg_cli_options_t *, const bool *,
    int, char **, fsimg_error_t *);
static fsimg_status_t find_format(const char *, fsimg_format_info_t *,
    fsimg_error_t *);
static fsimg_status_t validate_creation(fsimg_cli_options_t *, const bool *,
    fsimg_error_t *);

static fsimg_status_t
syntax(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT, message));
}

/* Decimal accumulation rejects overflow before multiplication. A suffix is
 * exactly one uppercase binary unit; counts never accept suffixes.
 */
fsimg_status_t
fsimg_cli_number(const char *text, bool bytes, uint64_t *out, fsimg_error_t *error)
{
	static const char units[] = "KMGTP";
	const char *p, *unit;
	uint64_t value;
	unsigned power, digit;

	if (text == NULL || text[0] < '0' || text[0] > '9')
		return (syntax(error, "expected an unsigned decimal quantity"));
	value = 0;
	for (p = text; *p >= '0' && *p <= '9'; p++) {
		digit = (unsigned)(*p - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return (syntax(error, "decimal quantity overflows uint64"));
		value = value * 10 + digit;
	}
	if (*p != '\0') {
		unit = strchr(units, *p);
		if (!bytes || unit == NULL || p[1] != '\0')
			return (syntax(error, "invalid quantity suffix or trailing characters"));
		power = (unsigned)(unit - units) + 1;
		for (unsigned i = 0; i < power; i++) {
			if (value > UINT64_MAX / 1024)
				return (syntax(error, "byte quantity overflows uint64"));
			value *= 1024;
		}
	}
	*out = value;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_cli_epoch(const char *text, int64_t *out, fsimg_error_t *error)
{
	uint64_t magnitude, ceiling;
	bool negative;
	fsimg_status_t status;

	if (text == NULL)
		return (syntax(error, "expected a signed decimal Unix epoch"));
	negative = text[0] == '-';
	status = fsimg_cli_number(text + (negative ? 1 : 0), false, &magnitude, error);
	if (status != FSIMG_OK)
		return (status);
	ceiling = negative ? UINT64_C(1) << 63 : INT64_MAX;
	if (magnitude > ceiling)
		return (syntax(error, "Unix epoch overflows int64"));
	if (negative)
		*out = magnitude == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)magnitude;
	else
		*out = (int64_t)magnitude;
	return (FSIMG_OK);
}

static fsimg_status_t
parse_limit(fsimg_cli_options_t *options, const char *text, unsigned *seen,
    fsimg_error_t *error)
{
	static const char *const keys[CLI_LIMIT_COUNT] = {
		[CLI_LIMIT_SCRATCH] = "scratch", [CLI_LIMIT_ENTRIES] = "entries",
		[CLI_LIMIT_DEPTH] = "depth", [CLI_LIMIT_PATH] = "path",
		[CLI_LIMIT_FDS] = "fds", [CLI_LIMIT_OUTPUT] = "output",
		[CLI_LIMIT_WORK] = "work", [CLI_LIMIT_SECONDS] = "seconds"
	};
	fsimg_worker_request_t *request = &options->request;
	const char *value;
	uint64_t number;
	size_t length;
	cli_limit_key_t key;
	fsimg_status_t status;

	value = strchr(text, '=');
	if (value == NULL)
		return (syntax(error, "resource limits require key=value"));
	length = (size_t)(value - text);
	for (key = 0; key < CLI_LIMIT_COUNT; key++) {
		if (strlen(keys[key]) == length && strncmp(text, keys[key], length) == 0)
			break;
	}
	if (key == CLI_LIMIT_COUNT)
		return (syntax(error, "unknown resource-limit key"));
	if ((*seen & (1u << key)) != 0)
		return (syntax(error, "resource-limit key specified more than once"));
	*seen |= 1u << key;
	status = fsimg_cli_number(value + 1, key == CLI_LIMIT_SCRATCH ||
	    key == CLI_LIMIT_PATH || key == CLI_LIMIT_OUTPUT,
	    &number, error);
	if (status != FSIMG_OK)
		return (status);
	if ((key == CLI_LIMIT_DEPTH || key == CLI_LIMIT_PATH || key == CLI_LIMIT_FDS) &&
	    number > UINT32_MAX)
		return (syntax(error, "resource limit exceeds its uint32 range"));
	/* The initialized request already owns the defaults. A zero singleton
	 * leaves that value intact rather than maintaining another default list.
	 */
	if (number == 0)
		return (FSIMG_OK);
	switch (key) {
	case CLI_LIMIT_SCRATCH: request->limits.scratch_bytes = number; break;
	case CLI_LIMIT_ENTRIES: request->limits.max_entries = number; break;
	case CLI_LIMIT_DEPTH: request->limits.max_depth = (uint32_t)number; break;
	case CLI_LIMIT_PATH: request->limits.max_path_bytes = (uint32_t)number; break;
	case CLI_LIMIT_FDS: request->limits.max_open_files = (uint32_t)number; break;
	case CLI_LIMIT_OUTPUT: request->limits.max_output_bytes = number; break;
	case CLI_LIMIT_WORK: request->limits.max_work = number; break;
	case CLI_LIMIT_SECONDS: request->seconds = number; break;
	case CLI_LIMIT_COUNT: return (syntax(error, "invalid resource-limit key"));
	}
	return (FSIMG_OK);
}

/* RFC 4122 text: 32 hexadecimal digits in groups of 8, 4, 4, 4 and 12,
 * stored in textual byte order.
 */
static fsimg_status_t
parse_uuid(const char *text, unsigned char *uuid, fsimg_error_t *error)
{
	static const char digits[] = "0123456789abcdef";
	const char *digit;
	size_t i, nibble;

	nibble = 0;
	for (i = 0; text[i] != '\0' && nibble < 32; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (text[i] != '-')
				break;
			continue;
		}
		digit = strchr(digits, text[i] >= 'A' && text[i] <= 'F' ?
		    text[i] - 'A' + 'a' : text[i]);
		if (digit == NULL || text[i] == '\0')
			break;
		if (nibble % 2 == 0)
			uuid[nibble / 2] = (unsigned char)((digit - digits) << 4);
		else
			uuid[nibble / 2] |= (unsigned char)(digit - digits);
		nibble++;
	}
	if (nibble != 32 || i != 36 || text[i] != '\0')
		return (syntax(error, "uuid setting must be RFC 4122 text"));
	return (FSIMG_OK);
}

/* Typed format settings: read-side oem, and creation-side byte quantities
 * (sector, cluster), the byte order, the volume UUID, the inode capacity
 * and the reserve percentage. The writer validates them for its format.
 */
static fsimg_status_t
parse_setting(fsimg_cli_options_t *options, const char *text, unsigned *seen,
    fsimg_error_t *error)
{
	static const char *const keys[CLI_SETTING_COUNT] = {
		[CLI_SETTING_SECTOR] = "sector", [CLI_SETTING_CLUSTER] = "cluster",
		[CLI_SETTING_OEM] = "oem", [CLI_SETTING_ENDIAN] = "endian",
		[CLI_SETTING_UUID] = "uuid", [CLI_SETTING_INODES] = "inodes",
		[CLI_SETTING_RESERVE] = "reserve"
	};
	fsimg_write_options_t *write = &options->request.write;
	const char *value;
	uint64_t number;
	size_t length;
	cli_setting_key_t key;
	fsimg_status_t status;

	value = strchr(text, '=');
	if (value == NULL)
		return (syntax(error, "format settings require key=value"));
	length = (size_t)(value - text);
	value++;
	for (key = 0; key < CLI_SETTING_COUNT; key++) {
		if (strlen(keys[key]) == length && strncmp(text, keys[key], length) == 0)
			break;
	}
	if (key == CLI_SETTING_COUNT)
		return (syntax(error, "unknown format-setting key"));
	if ((*seen & (1u << key)) != 0)
		return (syntax(error, "format-setting key specified more than once"));
	*seen |= 1u << key;
	if (key == CLI_SETTING_OEM) {
		if (options->request.kind >= FSIMG_JOB_CREATE || strcmp(value, "cp437") != 0)
			return (syntax(error, "read-side oem setting supports only cp437"));
		return (FSIMG_OK);
	}
	if (options->request.kind < FSIMG_JOB_CREATE)
		return (syntax(error, "geometry settings apply only to creation"));
	switch (key) {
	case CLI_SETTING_ENDIAN:
		if (strcmp(value, "little") == 0)
			write->byte_order = FSIMG_BYTE_ORDER_LITTLE;
		else if (strcmp(value, "big") == 0)
			write->byte_order = FSIMG_BYTE_ORDER_BIG;
		else
			return (syntax(error, "endian setting must be little or big"));
		return (FSIMG_OK);
	case CLI_SETTING_UUID:
		options->uuid_given = true;
		return (parse_uuid(value, write->volume_uuid, error));
	case CLI_SETTING_INODES:
	case CLI_SETTING_RESERVE:
		status = fsimg_cli_number(value, false, &number, error);
		if (status != FSIMG_OK)
			return (status);
		if (key == CLI_SETTING_RESERVE) {
			if (number > 100)
				return (syntax(error, "reserve setting is a percentage up to 100"));
			write->reserve_percent = (uint32_t)number;
		} else {
			if (number == 0 || number > UINT32_MAX)
				return (syntax(error, "inodes setting must be a positive uint32 count"));
			write->inode_capacity = (uint32_t)number;
		}
		return (FSIMG_OK);
	default:
		break;
	}
	status = fsimg_cli_number(value, true, &number, error);
	if (status != FSIMG_OK)
		return (status);
	if (number == 0 || number > UINT32_MAX)
		return (syntax(error, "geometry setting must be a positive uint32 byte quantity"));
	if (key == CLI_SETTING_SECTOR)
		write->sector_bytes = (uint32_t)number;
	else
		write->allocation_unit_bytes = (uint32_t)number;
	return (FSIMG_OK);
}

static fsimg_status_t
find_format(const char *name, fsimg_format_info_t *info, fsimg_error_t *error)
{
	size_t cursor;
	fsimg_status_t status;

	cursor = 0;
	for (;;) {
		status = fsimg_format_next(&cursor, info, error);
		if (status == FSIMG_DONE)
			return (syntax(error, "unknown format name"));
		if (status != FSIMG_OK)
			return (status);
		if (strcmp(info->name, name) == 0)
			return (FSIMG_OK);
	}
}

/* Creation settings that depend on the writer's capability record: the
 * default label applies only to a format with label capacity, so a format
 * without labels receives none unless -L asks for one (and then fails), and
 * explicit geometry must be legal for the writer profile.
 */
static fsimg_status_t
validate_creation(fsimg_cli_options_t *options, const bool *seen,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_format_info_t info;
	uint32_t sizes[2], masks[2];
	unsigned shift;
	fsimg_status_t status;

	status = find_format(request->format, &info, error);
	if (status != FSIMG_OK)
		return (status);
	if (!seen['L'])
		request->write.label = info.max_label_bytes != 0 ? "FSIMG" : NULL;
	/* Like the serial, a format's UUID is random unless reproducible or
	 * given.
	 */
	options->random_uuid = (info.write_settings & FSIMG_SETTING_VOLUME_UUID) != 0 &&
	    (request->write.flags & FSIMG_WRITE_REPRODUCIBLE) == 0 &&
	    !options->uuid_given;
	if (request->write.label != NULL &&
	    strnlen(request->write.label, sizeof(request->label_storage)) ==
	    sizeof(request->label_storage))
		return (syntax(error, "volume label exceeds the command's label representation"));
	sizes[0] = request->write.sector_bytes;
	sizes[1] = request->write.allocation_unit_bytes;
	masks[0] = info.sector_size_mask;
	masks[1] = info.write_allocation_unit_size_mask;
	for (unsigned i = 0; i < 2; i++) {
		if (sizes[i] == 0)
			continue;
		if ((sizes[i] & (sizes[i] - 1)) != 0)
			return (syntax(error, "geometry byte quantities must be powers of two"));
		shift = 0;
		while ((UINT32_C(1) << shift) != sizes[i])
			shift++;
		if ((masks[i] & (UINT32_C(1) << shift)) == 0)
			return (syntax(error, "geometry is outside the selected writer profile"));
	}
	if (sizes[1] != 0 && sizes[1] < (sizes[0] == 0 ? 512 : sizes[0]))
		return (syntax(error, "cluster size is smaller than sector size"));
	return (FSIMG_OK);
}

static fsimg_status_t
validate_options(fsimg_cli_options_t *options, const bool *seen,
    int operands, char **values, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_format_info_t info;
	bool creating;
	char message[96];

	if (options->command == FSIMG_CLI_HELP)
		return (operands == 0 ? FSIMG_OK : syntax(error, "unexpected help operands"));
	if (options->command == FSIMG_CLI_FORMATS)
		return (operands == 0 ? FSIMG_OK : syntax(error, "formats takes no operands"));
	creating = request->kind >= FSIMG_JOB_CREATE;
	if (seen['e'] && !seen['R'])
		return (syntax(error, "-e requires -R"));
	if (seen['r'] && request->kind != FSIMG_JOB_LIST)
		return (syntax(error, "-r applies only to ls"));
	if (seen['d'] && request->kind != FSIMG_JOB_VERIFY)
		return (syntax(error, "-d applies only to verify"));
	if (seen['k'] && request->kind != FSIMG_JOB_EXTRACT)
		return (syntax(error, "-k applies only to extract"));
	if (options->json && request->kind == FSIMG_JOB_CAT)
		return (syntax(error, "cat rejects -j because its output is raw content"));
	if (creating) {
		if (options->input != NULL || seen['b'] || seen['z'] || seen['C'])
			return (syntax(error, "read/extraction options do not apply to create"));
		if (request->format[0] == '\0' || operands != 1 ||
		    (request->kind == FSIMG_JOB_CREATE && options->output == NULL))
			return (syntax(error, "create requires -t, one source directory, and -o unless -n"));
		options->source = values[0];
	} else {
		if (seen['L'] || seen['R'] || seen['e'] || seen['f'] ||
		    seen['n'] || seen['o'] || seen['p'] || seen['s'])
			return (syntax(error, "creation options do not apply to read commands"));
		if (options->input == NULL) {
			snprintf(message, sizeof(message), "%s requires -i image", options->name);
			return (syntax(error, message));
		}
		if (strcmp(options->input, "-") == 0)
			return (syntax(error, "-i requires a seekable regular image; stdin is not supported"));
		if ((request->kind == FSIMG_JOB_EXTRACT) != seen['C'])
			return (syntax(error, "extract requires -C; other read commands reject it"));
		if (request->kind == FSIMG_JOB_CAT) {
			if (operands != 1)
				return (syntax(error, "cat requires exactly one image path"));
		} else if (operands > (request->kind == FSIMG_JOB_LIST ||
		    request->kind == FSIMG_JOB_EXTRACT ? 1 : 0))
			return (syntax(error, "unexpected command operands"));
		if (operands == 1)
			request->selection = values[0];
		if (request->selection[0] != '/')
			return (syntax(error, "image selection must be an absolute UTF-8 path"));
	}
	if (options->output != NULL && strcmp(options->output, "-") == 0)
		return (syntax(error, "output requires a new pathname, not stdout"));
	if (request->limits.memory_bytes < UINT64_C(16) * 1024 * 1024)
		return (syntax(error, "memory budget must be at least 16M"));
	if (creating)
		return (validate_creation(options, seen, error));
	if (request->format[0] == '\0')
		return (FSIMG_OK);
	return (find_format(request->format, &info, error));
}

fsimg_status_t
fsimg_cli_parse(int argc, char **argv, fsimg_cli_options_t *options,
    fsimg_error_t *error)
{
	static const char *const names[] = {
		"info", "ls", "cat", "extract", "verify", "create"
	};
	bool seen[256] = { false };
	fsimg_worker_request_t *request;
	fsimg_status_t status;
	unsigned limit_keys, setting_keys;
	uint64_t number;
	int flag;
	const char *flags;
	char message[96];

	bzero(options, sizeof(*options));
	fsimg_worker_request_init(&options->request);
	request = &options->request;
	options->scratch = getenv("TMPDIR");
	if (options->scratch == NULL)
		options->scratch = "/tmp";
	options->name = argc > 1 ? argv[1] : "";
	if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "-V") == 0)) {
		options->command = argv[1][1] == 'h' ? FSIMG_CLI_HELP : FSIMG_CLI_VERSION;
		return (FSIMG_OK);
	}
	if (argc < 2) {
		options->command = FSIMG_CLI_USAGE;
		return (syntax(error, "a command is required"));
	}
	if (strcmp(argv[1], "formats") == 0)
		options->command = FSIMG_CLI_FORMATS;
	else {
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if (strcmp(argv[1], names[i]) == 0)
				request->kind = (fsimg_job_kind_t)(i + 1);
		}
		if (request->kind == 0) {
			status = syntax(error, "unknown command");
			if (error != NULL) {
				flag = snprintf(error->path, sizeof(error->path), "%s", argv[1]);
				error->path_truncated = flag < 0 || (size_t)flag >= sizeof(error->path);
			}
			return (status);
		}
	}
	/* BSD getopt stops at the first operand. GNU's leading '+' selects the
	 * same behavior without changing the process environment.
	 */
	flags = options->command == FSIMG_CLI_FORMATS ? FSIMG_HOST_GETOPT_PREFIX ":hjv" :
	    FSIMG_HOST_GETOPT_PREFIX ":C:L:O:RS:T:b:de:f:hi:jkl:m:no:prs:t:vz:";
	limit_keys = setting_keys = 0;
	fsimg_host_getopt_reset();
	while ((flag = getopt(argc - 1, argv + 1, flags)) != -1) {
		if (flag == '?' || flag == ':') {
			snprintf(message, sizeof(message), flag == '?' ?
			    "unknown option -%c" : "option -%c requires an argument", optopt);
			return (syntax(error, message));
		}
		if (flag != 'l' && flag != 'O' && seen[(unsigned char)flag]) {
			snprintf(message, sizeof(message), "option -%c specified more than once", flag);
			return (syntax(error, message));
		}
		seen[(unsigned char)flag] = true;
		status = FSIMG_OK;
		switch (flag) {
		case 'h': options->command = FSIMG_CLI_HELP; break;
		case 'j': options->json = true; break;
		case 'k': request->flags |= FSIMG_JOB_SKIP_UNSUPPORTED; break;
		case 'p': request->write.flags |= FSIMG_WRITE_IMPORT_POSIX; break;
		case 'v': request->flags |= FSIMG_JOB_VERBOSE; break;
		case 'r': request->flags |= FSIMG_JOB_RECURSIVE; break;
		case 'd': request->flags |= FSIMG_JOB_PAYLOAD; break;
		case 'i': options->input = optarg; break;
		case 'o': case 'C': options->output = optarg; break;
		case 'T': options->scratch = optarg; break;
		case 'L': request->write.label = optarg; break;
		case 'R': request->write.flags |= FSIMG_WRITE_REPRODUCIBLE; break;
		case 'n':
			if (request->kind < FSIMG_JOB_CREATE)
				return (syntax(error, "-n applies only to create"));
			request->kind = FSIMG_JOB_PLAN;
			break;
		case 't':
			if (optarg[0] == '\0' || strlen(optarg) >= sizeof(request->format))
				return (syntax(error, "format name is empty or too long"));
			strcpy(request->format, optarg);
			break;
		case 'S':
			if (strcmp(optarg, "auto") == 0) request->sandbox = FSIMG_SANDBOX_AUTO;
			else if (strcmp(optarg, "required") == 0) request->sandbox = FSIMG_SANDBOX_REQUIRED;
			else if (strcmp(optarg, "off") == 0) request->sandbox = FSIMG_SANDBOX_OFF;
			else return (syntax(error, "sandbox policy must be auto, required or off"));
			break;
		case 'l': status = parse_limit(options, optarg, &limit_keys, error); break;
		case 'O': status = parse_setting(options, optarg, &setting_keys, error); break;
		case 'e': status = fsimg_cli_epoch(optarg, &request->write.epoch, error); break;
		case 'm': case 'b': case 'z': case 'f': case 's':
			status = fsimg_cli_number(optarg, true, &number, error);
			if (status != FSIMG_OK)
				break;
			if ((flag == 's' || flag == 'z') && number == 0)
				return (syntax(error, "explicit volume size and slice length must be positive"));
			if (flag == 'm' && number != 0) request->limits.memory_bytes = number;
			if (flag == 'b') request->offset = number;
			if (flag == 'z') request->length = number;
			if (flag == 'f') request->write.min_free_bytes = number;
			if (flag == 's') request->write.volume_bytes = number;
			break;
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (validate_options(options, seen, argc - 1 - optind,
	    argv + 1 + optind, error));
}
