/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <getopt.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cli.h"
#include "host.h"
#include "internal.h"
#include "layer.h"
#include "settings.h"

/*
 * Command parsing turns argv into a validated fsimg_cli_options_t in five
 * steps, each finishing before the next starts:
 *
 * 1. A dashless first argument (tar's "xvf") is rewritten into one "-X" word
 *    per letter, argument letters taking the following words in order.
 * 2. getopt_long records every option word. Options precede operands on
 *    every host: the option string begins with "+", which stops parsing at
 *    the first operand instead of permuting argv.
 * 3. The mode words select exactly one mode, or c and W together.
 * 4. Every recorded option is checked against the modes that accept it and
 *    against repetition, then interpreted; operands follow.
 * 5. Recognized features that are not implemented fail last, so a command
 *    line is reported as invalid syntax (exit 2) before it is reported as
 *    unsupported (exit 1).
 *
 * Nothing here opens a file: preparation opens images, sources and
 * directories only after the whole command line is known to be valid.
 */

/* Mode bits. An option's table entry names the modes that accept it. */
#define MODE_CREATE		0x01u
#define MODE_CREATE_VERIFY	0x02u
#define MODE_EXTRACT		0x04u
#define MODE_LIST		0x08u
#define MODE_VERIFY		0x10u
#define MODE_INFO		0x20u
#define MODE_FORMATS		0x40u
#define MODES_CREATE		(MODE_CREATE | MODE_CREATE_VERIFY)
#define MODES_READ		(MODE_EXTRACT | MODE_LIST | MODE_VERIFY | MODE_INFO)
#define MODES_IMAGE		(MODES_CREATE | MODES_READ)
#define MODES_ALL		(MODES_IMAGE | MODE_FORMATS)

/*
 * The default descriptor limit is the depth limit plus this many. A source
 * scan or an extraction holds one descriptor per directory level along its
 * path, so a tree as deep as the depth limit needs that many; these cover
 * everything else a command holds at once: its operand roots, two for a
 * manifest with a content directory, scratch, the staging directory and
 * staged file, the raw intermediate, spilled metadata stores and the file
 * being read or written, with room for about a hundred operands.
 */
#define CLI_WORKING_DESCRIPTORS	128u

/* getopt_long values of the long-only options, above every short letter. */
enum {
	OPT_FORMATS = 256,
	OPT_FREE_SPACE,
	OPT_INFO,
	OPT_JSON,
	OPT_LENGTH,
	OPT_MTREE,
	OPT_NO_RECURSION,
	OPT_OFFSET,
	OPT_PARTITION
};

typedef struct cli_option {
	int		 key;		/* getopt_long return value */
	const char	*spelling;	/* As the user writes it, for diagnostics */
	unsigned	 modes;		/* Modes accepting the option */
	unsigned	 repeat_modes;	/* Modes in which it may repeat */
} cli_option_t;

/* Every option fsimg recognizes. Mode words (c, x, t, W, i, --info,
 * --formats) and -h and -V are listed so they are counted like any option;
 * mode selection, not this table, decides where they are legal.
 */
static const cli_option_t option_table[] = {
	{ 'a', "-a", MODES_CREATE, 0 },
	{ 'C', "-C", MODES_CREATE | MODE_EXTRACT, MODES_CREATE },
	{ 'c', "-c", MODES_ALL, 0 },
	{ 'd', "-d", MODE_VERIFY | MODE_CREATE_VERIFY, 0 },
	{ 'E', "-E", MODE_EXTRACT, 0 },
	{ 'e', "-e", MODES_CREATE, 0 },
	{ 'F', "-F", MODES_IMAGE, 0 },
	{ 'f', "-f", MODES_IMAGE, 0 },
	{ 'h', "-h", MODES_ALL, 0 },
	{ 'i', "-i", MODES_ALL, 0 },
	{ 'k', "-k", MODE_EXTRACT, 0 },
	{ 'L', "-L", MODES_CREATE, 0 },
	{ 'l', "-l", MODES_IMAGE, MODES_IMAGE },
	{ 'M', "-M", MODES_CREATE, 0 },
	{ 'm', "-m", MODES_IMAGE, 0 },
	{ 'n', "-n", MODE_CREATE, 0 },
	{ 'O', "-O", MODE_EXTRACT, 0 },
	{ 'o', "-o", MODES_IMAGE, MODES_IMAGE },
	{ 'p', "-p", MODES_CREATE, 0 },
	{ 'R', "-R", MODES_CREATE, 0 },
	{ 'S', "-S", MODES_IMAGE, 0 },
	{ 's', "-s", MODES_CREATE, 0 },
	{ 'T', "-T", MODES_IMAGE, 0 },
	{ 't', "-t", MODES_ALL, 0 },
	{ 'V', "-V", MODES_ALL, 0 },
	{ 'v', "-v", MODES_ALL, MODES_ALL },
	{ 'W', "-W", MODES_ALL, 0 },
	{ 'x', "-x", MODES_ALL, 0 },
	{ 'z', "-z", MODES_IMAGE, 0 },
	{ OPT_FORMATS, "--formats", MODES_ALL, 0 },
	{ OPT_FREE_SPACE, "--free-space", MODES_CREATE, 0 },
	{ OPT_INFO, "--info", MODES_ALL, 0 },
	{ OPT_JSON, "--json", MODES_ALL, 0 },
	{ OPT_LENGTH, "--length", MODES_READ, 0 },
	{ OPT_MTREE, "--mtree", MODE_EXTRACT, 0 },
	{ OPT_NO_RECURSION, "--no-recursion", MODE_LIST, 0 },
	{ OPT_OFFSET, "--offset", MODES_READ, 0 },
	{ OPT_PARTITION, "--partition", MODES_READ, 0 }
};
#define OPTION_COUNT (sizeof(option_table) / sizeof(option_table[0]))

static const struct option long_options[] = {
	{ "formats", no_argument, NULL, OPT_FORMATS },
	{ "free-space", required_argument, NULL, OPT_FREE_SPACE },
	{ "info", no_argument, NULL, OPT_INFO },
	{ "json", no_argument, NULL, OPT_JSON },
	{ "length", required_argument, NULL, OPT_LENGTH },
	{ "mtree", required_argument, NULL, OPT_MTREE },
	{ "no-recursion", no_argument, NULL, OPT_NO_RECURSION },
	{ "offset", required_argument, NULL, OPT_OFFSET },
	{ "partition", required_argument, NULL, OPT_PARTITION },
	{ NULL, 0, NULL, 0 }
};

/* The short letters, and those of them taking an argument. A dashless
 * bundle may contain exactly these letters.
 */
static const char short_options[] = "+:aC:cdEe:F:f:hikL:l:Mm:nOo:pRS:s:T:tVvWxz";

/* The mode words, each naming one mode bit. */
static const struct {
	int		 key;
	unsigned	 mode;
} mode_words[] = {
	{ 'c', MODE_CREATE }, { 'i', MODE_INFO }, { 't', MODE_LIST },
	{ 'W', MODE_VERIFY }, { 'x', MODE_EXTRACT }, { OPT_INFO, MODE_INFO },
	{ OPT_FORMATS, MODE_FORMATS }
};

/* The only image suffixes the command keeps itself. */
static const char *const generic_suffixes[] = { ".bin", ".dd", ".img", ".raw" };

typedef enum cli_limit_key {
	CLI_LIMIT_SCRATCH, CLI_LIMIT_SPOOL, CLI_LIMIT_ENTRIES, CLI_LIMIT_DEPTH,
	CLI_LIMIT_PATH, CLI_LIMIT_FDS, CLI_LIMIT_OUTPUT, CLI_LIMIT_WORK,
	CLI_LIMIT_SECONDS, CLI_LIMIT_COUNT
} cli_limit_key_t;

/* Parsing state. Option arguments are borrowed from the command words;
 * the repeatable -l and -o arguments are kept in order until the mode,
 * which decides their meaning, is known. setting_rows holds, for each -o
 * argument parsed so far, its catalogue row, or NULL for a key left
 * uninterpreted, so a repeated key is found by identity.
 */
typedef struct cli_parse {
	fsimg_cli_options_t *options;
	fsimg_error_t	*error;
	char		**argv;
	int		 argc;
	int		 first_operand;
	unsigned	 counts[OPTION_COUNT];
	const char	*values[OPTION_COUNT];
	const char	**limits;
	unsigned	 limit_count;
	const char	**settings;
	const fsimg_setting_t **setting_rows;
	unsigned	 setting_count;
	unsigned	 mode;		/* One MODE_* bit after selection */
	bool		 reserved;	/* The -F filesystem has no driver */
	const char	*unsupported;	/* First recognized, unimplemented feature */
	bool		 options_ended;	/* "--" ended option parsing */
} cli_parse_t;

static fsimg_status_t syntax(cli_parse_t *, const char *);
static fsimg_status_t syntax_naming(cli_parse_t *, const char *, const char *);
static size_t option_index(int);
static unsigned option_count(const cli_parse_t *, int);
static const char *option_value(const cli_parse_t *, int);
static const char *mode_name(unsigned);
static bool is_option_letter(char);
static bool takes_argument(char);
static fsimg_status_t expand_bundle(cli_parse_t *);
static fsimg_status_t read_option_words(cli_parse_t *);
static fsimg_status_t select_mode(cli_parse_t *);
static fsimg_status_t check_option_modes(cli_parse_t *);
static fsimg_status_t parse_limit(cli_parse_t *, const char *, unsigned *);
static uint32_t default_descriptor_limit(uint32_t);
static bool parse_uuid(const char *, unsigned char *);
static bool parse_hex32(const char *, uint32_t *);
static bool find_layer(const char *, size_t, fsimg_layer_info_t *);
static bool layer_selected(const cli_parse_t *, const fsimg_layer_info_t *);
static bool setting_given(const cli_parse_t *, const fsimg_setting_t *,
    unsigned);
static fsimg_status_t defer_setting(cli_parse_t *, unsigned, size_t);
static fsimg_status_t convert_number(cli_parse_t *, const fsimg_setting_t *,
    const char *, const char *, fsimg_setting_value_t *);
static fsimg_status_t convert_setting(cli_parse_t *, const fsimg_setting_t *,
    const char *, const char *, fsimg_setting_value_t *);
static fsimg_status_t parse_setting(cli_parse_t *, unsigned);
static fsimg_status_t parse_stack(cli_parse_t *, const char *);
static fsimg_status_t parse_partition(cli_parse_t *);
static fsimg_status_t parse_byte_option(cli_parse_t *, int, bool, uint64_t *);
static fsimg_status_t interpret_common(cli_parse_t *);
static fsimg_status_t interpret_create(cli_parse_t *);
static fsimg_status_t interpret_read(cli_parse_t *);
static fsimg_status_t read_create_operands(cli_parse_t *);
static fsimg_status_t read_members(cli_parse_t *);
static fsimg_status_t normalize_path(cli_parse_t *, const char *, bool, char **);
static fsimg_status_t find_format(cli_parse_t *, const char *,
    fsimg_format_info_t *);
static fsimg_status_t validate_creation(cli_parse_t *);
static fsimg_status_t check_image_name(cli_parse_t *);
static void note_unsupported(cli_parse_t *, const char *);
static bool strip_suffix(char *, const char *);
static size_t suffix_layer(const char *, fsimg_layer_kind_t,
    fsimg_layer_info_t *);
static size_t layer_suffix_length(const char *, fsimg_layer_kind_t);
static size_t compression_suffix(const char *, char *, size_t,
    fsimg_layer_info_t *);
static bool compression_selected(const cli_parse_t *, fsimg_layer_info_t *);
static fsimg_status_t apply_compression(cli_parse_t *);

static fsimg_status_t
syntax(cli_parse_t *parse, const char *message)
{
	parse->options->syntax_error = true;
	return (fsimg_error_set(parse->error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
	    message));
}

/* A syntax error quoting the offending word, rendered like every path an
 * error carries; the display escapes the rendering.
 */
static fsimg_status_t
syntax_naming(cli_parse_t *parse, const char *message, const char *word)
{
	fsimg_status_t status;

	status = syntax(parse, message);
	fsimg_error_path(parse->error, word);
	return (status);
}

static size_t
option_index(int key)
{
	size_t i;

	for (i = 0; i < OPTION_COUNT; i++) {
		if (option_table[i].key == key)
			break;
	}
	return (i);
}

static unsigned
option_count(const cli_parse_t *parse, int key)
{
	return (parse->counts[option_index(key)]);
}

static const char *
option_value(const cli_parse_t *parse, int key)
{
	return (parse->values[option_index(key)]);
}

static const char *
mode_name(unsigned mode)
{
	switch (mode) {
	case MODE_CREATE: return ("c");
	case MODE_CREATE_VERIFY: return ("cW");
	case MODE_EXTRACT: return ("x");
	case MODE_LIST: return ("t");
	case MODE_VERIFY: return ("W");
	case MODE_INFO: return ("i");
	case MODE_FORMATS: return ("formats");
	default: return ("");
	}
}

static bool
is_option_letter(char letter)
{
	return (letter != '\0' && letter != ':' && letter != '+' &&
	    strchr(short_options, letter) != NULL);
}

static bool
takes_argument(char letter)
{
	const char *found;

	found = strchr(short_options, letter);
	return (found != NULL && found[1] == ':');
}

/*
 * A first argument without a leading dash is a tar-style bundle such as
 * "xvf". Every character must be an option letter and one of them must be
 * a mode or h; the bundle becomes one "-X" word per letter, and each letter
 * taking an argument consumes the next unconsumed word after the bundle, in
 * letter order: "cfF boot.img fat32" is "-c -f boot.img -F fat32". The
 * rewritten words borrow the original arguments; the "-X" words are built
 * in the same allocation, after the word pointers.
 */
static fsimg_status_t
expand_bundle(cli_parse_t *parse)
{
	const char *bundle;
	char **words, *letter_word;
	size_t letter_count, word_count, used;
	int next;
	bool has_mode;
	char name[2];

	bundle = parse->argv[1];
	if (bundle[0] == '-')
		return (FSIMG_OK);
	parse->options->overview = true;
	has_mode = false;
	for (const char *p = bundle; *p != '\0'; p++) {
		if (!is_option_letter(*p)) {
			name[0] = *p;
			name[1] = '\0';
			return (syntax_naming(parse,
			    "unknown mode or option letter in the first argument", name));
		}
		if (strchr("chiWtx", *p) != NULL)
			has_mode = true;
	}
	if (!has_mode)
		return (syntax_naming(parse,
		    "the first argument names no mode", bundle));
	letter_count = strlen(bundle);
	word_count = (size_t)parse->argc + letter_count + 1;
	words = calloc(word_count * sizeof(*words) + letter_count * 3, 1);
	if (words == NULL)
		return (fsimg_error_set(parse->error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "command word allocation failed"));
	parse->options->words = words;
	used = 0;
	words[used++] = parse->argv[0];
	letter_word = (char *)(words + word_count);
	next = 2;
	for (const char *p = bundle; *p != '\0'; p++) {
		letter_word[0] = '-';
		letter_word[1] = *p;
		words[used++] = letter_word;
		letter_word += 3;
		if (!takes_argument(*p))
			continue;
		if (next >= parse->argc) {
			name[0] = *p;
			name[1] = '\0';
			return (syntax_naming(parse,
			    "bundled option letter lacks its argument", name));
		}
		words[used++] = parse->argv[next++];
	}
	while (next < parse->argc)
		words[used++] = parse->argv[next++];
	words[used] = NULL;
	parse->argv = words;
	parse->argc = (int)used;
	parse->options->overview = false;
	return (FSIMG_OK);
}

/*
 * Record every option word. Values of singleton options are kept for
 * interpretation after mode selection; -l and -o keep every value in
 * order. getopt_long stops at the first operand; it consumes a "--" word,
 * which is detected because only that advances optind while returning -1.
 */
static fsimg_status_t
read_option_words(cli_parse_t *parse)
{
	size_t index;
	int key, before;
	char name[3];

	parse->limits = calloc((size_t)parse->argc, sizeof(*parse->limits));
	parse->settings = calloc((size_t)parse->argc, sizeof(*parse->settings));
	parse->setting_rows = calloc((size_t)parse->argc,
	    sizeof(*parse->setting_rows));
	if (parse->limits == NULL || parse->settings == NULL ||
	    parse->setting_rows == NULL)
		return (fsimg_error_set(parse->error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "command word allocation failed"));
	fsimg_host_getopt_reset();
	for (;;) {
		before = optind;
		key = getopt_long(parse->argc, parse->argv, short_options,
		    long_options, NULL);
		if (key == -1)
			break;
		if (key == '?' || key == ':') {
			/* A short option names its letter; a long one its word. */
			if (optopt > 0 && optopt < 256) {
				name[0] = '-';
				name[1] = (char)optopt;
				name[2] = '\0';
				return (syntax_naming(parse, key == '?' ?
				    "unknown option" : "option requires an argument", name));
			}
			return (syntax_naming(parse, key == '?' ?
			    "unknown option" : "option requires an argument",
			    parse->argv[optind - 1]));
		}
		index = option_index(key);
		if (index == OPTION_COUNT)
			return (syntax(parse, "unrecognized option value"));
		parse->counts[index]++;
		parse->values[index] = optarg;
		if (key == 'l')
			parse->limits[parse->limit_count++] = optarg;
		else if (key == 'o')
			parse->settings[parse->setting_count++] = optarg;
	}
	parse->options_ended = optind == before + 1 &&
	    strcmp(parse->argv[before], "--") == 0;
	parse->first_operand = optind;
	return (FSIMG_OK);
}

/* Exactly one mode, except that c and W combine. -h and -V are commands of
 * their own: help for the named mode, and the version alone.
 */
static fsimg_status_t
select_mode(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	unsigned named, count;

	named = 0;
	for (size_t i = 0; i < sizeof(mode_words) / sizeof(mode_words[0]); i++) {
		count = option_count(parse, mode_words[i].key);
		if (count > 1 || (count == 1 && (named & mode_words[i].mode) != 0))
			return (syntax(parse, "a mode is given more than once"));
		if (count == 1)
			named |= mode_words[i].mode;
	}
	if (option_count(parse, 'V') != 0) {
		if (parse->argc != 2)
			return (syntax(parse, "-V takes no other option or operand"));
		options->command = FSIMG_CLI_VERSION;
		return (FSIMG_OK);
	}
	if (named == (MODE_CREATE | MODE_VERIFY))
		named = MODE_CREATE_VERIFY;
	if ((named & (named - 1)) != 0)
		return (syntax(parse, "two modes are given; only c and W combine"));
	parse->mode = named;
	options->mode = mode_name(named);
	if (option_count(parse, 'h') != 0) {
		if (parse->first_operand != parse->argc)
			return (syntax_naming(parse, "unexpected operand",
			    parse->argv[parse->first_operand]));
		options->command = FSIMG_CLI_HELP;
		return (FSIMG_OK);
	}
	if (named == 0) {
		options->overview = true;
		return (syntax(parse, "a mode is required"));
	}
	if (named == MODE_FORMATS)
		options->command = FSIMG_CLI_FORMATS;
	return (FSIMG_OK);
}

static fsimg_status_t
check_option_modes(cli_parse_t *parse)
{
	const cli_option_t *option;
	char message[96];

	for (size_t i = 0; i < OPTION_COUNT; i++) {
		option = &option_table[i];
		if (parse->counts[i] == 0)
			continue;
		if ((option->modes & parse->mode) == 0) {
			snprintf(message, sizeof(message),
			    "option does not apply to mode %s", mode_name(parse->mode));
			return (syntax_naming(parse, message, option->spelling));
		}
		if (parse->counts[i] > 1 && (option->repeat_modes & parse->mode) == 0)
			return (syntax_naming(parse, "option is given more than once",
			    option->spelling));
	}
	if (option_count(parse, 'v') > 2)
		return (syntax(parse, "verbosity has two levels, -v and -vv"));
	return (FSIMG_OK);
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
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "expected an unsigned decimal quantity"));
	value = 0;
	for (p = text; *p >= '0' && *p <= '9'; p++) {
		digit = (unsigned)(*p - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "decimal quantity overflows uint64"));
		value = value * 10 + digit;
	}
	if (*p != '\0') {
		unit = strchr(units, *p);
		if (!bytes || unit == NULL || p[1] != '\0')
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "invalid quantity suffix or trailing characters"));
		power = (unsigned)(unit - units) + 1;
		for (unsigned i = 0; i < power; i++) {
			if (value > UINT64_MAX / 1024)
				return (fsimg_error_set(error, FSIMG_EINVAL, 0,
				    FSIMG_OP_CONTEXT, "byte quantity overflows uint64"));
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
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "expected a signed decimal Unix epoch"));
	negative = text[0] == '-';
	status = fsimg_cli_number(text + (negative ? 1 : 0), false, &magnitude, error);
	if (status != FSIMG_OK)
		return (status);
	ceiling = negative ? UINT64_C(1) << 63 : INT64_MAX;
	if (magnitude > ceiling)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "Unix epoch overflows int64"));
	if (negative)
		*out = magnitude == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)magnitude;
	else
		*out = (int64_t)magnitude;
	return (FSIMG_OK);
}

static fsimg_status_t
parse_limit(cli_parse_t *parse, const char *text, unsigned *seen)
{
	static const char *const keys[CLI_LIMIT_COUNT] = {
		[CLI_LIMIT_SCRATCH] = "scratch", [CLI_LIMIT_SPOOL] = "spool",
		[CLI_LIMIT_ENTRIES] = "entries", [CLI_LIMIT_DEPTH] = "depth",
		[CLI_LIMIT_PATH] = "path", [CLI_LIMIT_FDS] = "fds",
		[CLI_LIMIT_OUTPUT] = "output", [CLI_LIMIT_WORK] = "work",
		[CLI_LIMIT_SECONDS] = "seconds"
	};
	fsimg_worker_request_t *request = &parse->options->request;
	const char *value;
	uint64_t number;
	size_t length;
	cli_limit_key_t key;
	bool bytes;

	value = strchr(text, '=');
	if (value == NULL)
		return (syntax_naming(parse, "resource limits require key=value", text));
	length = (size_t)(value - text);
	for (key = 0; key < CLI_LIMIT_COUNT; key++) {
		if (strlen(keys[key]) == length && strncmp(text, keys[key], length) == 0)
			break;
	}
	if (key == CLI_LIMIT_COUNT)
		return (syntax_naming(parse, "unknown resource-limit key", text));
	if ((*seen & (1u << key)) != 0)
		return (syntax_naming(parse, "resource-limit key is given more than once",
		    text));
	*seen |= 1u << key;
	bytes = key == CLI_LIMIT_SCRATCH || key == CLI_LIMIT_SPOOL ||
	    key == CLI_LIMIT_PATH || key == CLI_LIMIT_OUTPUT;
	if (fsimg_cli_number(value + 1, bytes, &number, parse->error) != FSIMG_OK)
		return (syntax_naming(parse, bytes ?
		    "resource limit requires an unsigned byte quantity" :
		    "resource limit requires an unsigned decimal count", text));
	if ((key == CLI_LIMIT_DEPTH || key == CLI_LIMIT_PATH || key == CLI_LIMIT_FDS) &&
	    number > UINT32_MAX)
		return (syntax_naming(parse, "resource limit exceeds its uint32 range",
		    text));
	/* The initialized request already owns the defaults, except the
	 * descriptor limit's, which is derived once every key is read. A zero
	 * value leaves the default rather than maintaining another list.
	 */
	if (number == 0)
		return (FSIMG_OK);
	switch (key) {
	case CLI_LIMIT_SCRATCH: request->limits.scratch_bytes = number; break;
	case CLI_LIMIT_SPOOL: request->limits.spool_bytes = number; break;
	case CLI_LIMIT_ENTRIES: request->limits.max_entries = number; break;
	case CLI_LIMIT_DEPTH: request->limits.max_depth = (uint32_t)number; break;
	case CLI_LIMIT_PATH: request->limits.max_path_bytes = (uint32_t)number; break;
	case CLI_LIMIT_FDS: request->limits.max_open_files = (uint32_t)number; break;
	case CLI_LIMIT_OUTPUT: request->limits.max_output_bytes = number; break;
	case CLI_LIMIT_WORK: request->limits.max_work = number; break;
	case CLI_LIMIT_SECONDS: request->seconds = number; break;
	case CLI_LIMIT_COUNT: return (syntax(parse, "invalid resource-limit key"));
	}
	return (FSIMG_OK);
}

/* The descriptor limit a command gets unless it gives one: the depth limit
 * plus CLI_WORKING_DESCRIPTORS, saturating at the field's range.
 */
static uint32_t
default_descriptor_limit(uint32_t depth)
{
	if (depth > UINT32_MAX - CLI_WORKING_DESCRIPTORS)
		return (UINT32_MAX);
	return (depth + CLI_WORKING_DESCRIPTORS);
}

/* RFC 4122 text: 32 hexadecimal digits in groups of 8, 4, 4, 4 and 12,
 * stored in textual byte order.
 */
static bool
parse_uuid(const char *text, unsigned char *uuid)
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
	return (nibble == 32 && i == 36 && text[i] == '\0');
}

/* The registry record of a layer name that need not be terminated. */
static bool
find_layer(const char *name, size_t length, fsimg_layer_info_t *info)
{
	size_t cursor;

	cursor = 0;
	while (fsimg_layer_next(&cursor, info, NULL) == FSIMG_OK) {
		if (strlen(info->name) == length &&
		    strncmp(name, info->name, length) == 0)
			return (true);
	}
	return (false);
}

/* Whether the command selects a layer: the scheme or container its -F
 * stack names, or the compression -z or -a selects.
 */
static bool
layer_selected(const cli_parse_t *parse, const fsimg_layer_info_t *layer)
{
	const fsimg_stack_slot_t *slot;
	fsimg_layer_info_t compression;

	if (layer->kind == FSIMG_LAYER_COMPRESSION)
		return (compression_selected(parse, &compression) &&
		    strcmp(compression.name, layer->name) == 0);
	slot = &parse->options->request.stack.slots[layer->kind];
	return (slot->state == FSIMG_SLOT_NAMED && strcmp(slot->name, layer->name) == 0);
}

/* Whether an -o argument before index named the row. */
static bool
setting_given(const cli_parse_t *parse, const fsimg_setting_t *row,
    unsigned index)
{
	for (unsigned i = 0; i < index; i++) {
		if (parse->setting_rows[i] == row)
			return (true);
	}
	return (false);
}

/*
 * A qualified key of a selected layer this build cannot write: the
 * command is refused as unsupported once the rest of it is valid, so its
 * key is not looked up and its value not read. Only its raw key, the
 * key_length bytes before "=", is compared with the earlier uninterpreted
 * keys, so a repetition is still a syntax error.
 */
static fsimg_status_t
defer_setting(cli_parse_t *parse, unsigned index, size_t key_length)
{
	const char *text, *other;

	text = parse->settings[index];
	for (unsigned i = 0; i < index; i++) {
		other = parse->settings[i];
		if (parse->setting_rows[i] == NULL &&
		    strcspn(other, "=") == key_length &&
		    strncmp(text, other, key_length) == 0)
			return (syntax_naming(parse,
			    "setting key is given more than once", text));
	}
	parse->setting_rows[index] = NULL;
	return (FSIMG_OK);
}

/* "0x" and one to eight hexadecimal digits. */
static bool
parse_hex32(const char *text, uint32_t *out)
{
	static const char digits[] = "0123456789abcdef";
	const char *digit;
	size_t i;

	*out = 0;
	if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X') ||
	    text[2] == '\0')
		return (false);
	for (i = 2; text[i] != '\0'; i++) {
		digit = strchr(digits, text[i] >= 'A' && text[i] <= 'F' ?
		    text[i] - 'A' + 'a' : text[i]);
		if (digit == NULL || i >= 10)
			return (false);
		*out = *out << 4 | (uint32_t)(digit - digits);
	}
	return (true);
}

/*
 * A byte quantity or decimal count: unsigned, without a sign, a unit only
 * for a byte quantity, within the row's maximum and, for a row that
 * refuses zero, positive; then held in the row's integer kind. A row with
 * choices accepts only their numbers.
 */
static fsimg_status_t
convert_number(cli_parse_t *parse, const fsimg_setting_t *row,
    const char *value, const char *word, fsimg_setting_value_t *out)
{
	char choices[64], message[96];
	uint64_t number;
	bool bytes;

	bytes = row->syntax == FSIMG_SYNTAX_BYTES;
	if (fsimg_cli_number(value, bytes, &number, parse->error) != FSIMG_OK ||
	    number > row->max || (row->nonzero && number == 0)) {
		if (bytes)
			return (syntax_naming(parse, row->nonzero ?
			    "setting requires a nonzero byte quantity in range" :
			    "setting requires a byte quantity in range", word));
		return (syntax_naming(parse, row->nonzero ?
		    "setting requires a positive decimal count in range" :
		    "setting requires a decimal count in range", word));
	}
	if (row->kind == FSIMG_KIND_U32)
		out->u32 = (uint32_t)number;
	else
		out->u64 = number;
	if (row->choices != NULL && fsimg_setting_choice_of(row, out) == NULL) {
		fsimg_setting_choices_text(row, choices, sizeof(choices));
		snprintf(message, sizeof(message), "setting must be %s", choices);
		return (syntax_naming(parse, message, word));
	}
	return (FSIMG_OK);
}

/*
 * Convert a value by its row's lexical class into a typed temporary. word
 * is the whole argument, which a diagnostic names. A text keeps pointing
 * into the command words, which outlive the parsed command; a path is
 * kept as given, for the coordinator to open.
 */
static fsimg_status_t
convert_setting(cli_parse_t *parse, const fsimg_setting_t *row,
    const char *value, const char *word, fsimg_setting_value_t *out)
{
	static const unsigned char nil[16];
	const fsimg_setting_choice_t *choice;
	char choices[64], message[96];
	size_t capacity;

	bzero(out, sizeof(*out));
	switch (row->syntax) {
	case FSIMG_SYNTAX_BYTES:
	case FSIMG_SYNTAX_COUNT:
		return (convert_number(parse, row, value, word, out));
	case FSIMG_SYNTAX_HEX32:
		if (!parse_hex32(value, &out->u32))
			return (syntax_naming(parse,
			    "setting requires 0x and at most eight hex digits",
			    word));
		return (FSIMG_OK);
	case FSIMG_SYNTAX_BOOL:
		if (strcmp(value, "yes") != 0 && strcmp(value, "no") != 0)
			return (syntax_naming(parse, "setting must be yes or no",
			    word));
		out->flag = strcmp(value, "yes") == 0;
		return (FSIMG_OK);
	case FSIMG_SYNTAX_CHOICE:
		for (choice = row->choices; choice->spelling != NULL; choice++) {
			if (strcmp(value, choice->spelling) == 0) {
				*out = choice->value;
				return (FSIMG_OK);
			}
		}
		fsimg_setting_choices_text(row, choices, sizeof(choices));
		snprintf(message, sizeof(message), "setting must be %s", choices);
		return (syntax_naming(parse, message, word));
	case FSIMG_SYNTAX_UUID:
		if (!parse_uuid(value, out->id) ||
		    (row->nonzero && bcmp(out->id, nil, sizeof(nil)) == 0))
			return (syntax_naming(parse, row->nonzero ?
			    "GUID setting must be RFC 4122 text other than nil" :
			    "setting must be RFC 4122 text", word));
		return (FSIMG_OK);
	case FSIMG_SYNTAX_TEXT:
		capacity = row->text_bytes != 0 ? row->text_bytes : row->size;
		if (strnlen(value, capacity) == capacity)
			return (syntax_naming(parse,
			    "setting value exceeds its text capacity", word));
		out->text = value;
		return (FSIMG_OK);
	case FSIMG_SYNTAX_PATH:
		return (FSIMG_OK);
	}
	return (syntax_naming(parse, "unknown setting syntax", word));
}

/*
 * One -o argument, key=value, against the settings catalogue. An
 * unqualified key always names a filesystem setting. A qualified key
 * "layer:key" names a setting of a layer the command selects: its -F
 * stack's scheme or container, or the compression -z or -a selects; on
 * creation a selected layer this build cannot write takes defer_setting
 * instead. The key's row then decides repetition, the modes it applies to
 * and its value's syntax. A converted value is committed, with its
 * presence flag when the row has one, only after it converted; a file
 * setting's value is a host path the coordinator opens once the command
 * line is known to be valid. The library validates the settings as a
 * whole.
 */
static fsimg_status_t
parse_setting(cli_parse_t *parse, unsigned index)
{
	fsimg_worker_request_t *request = &parse->options->request;
	const fsimg_setting_targets_t targets = { .write = &request->write,
	    .layers = &request->layers };
	const fsimg_setting_slice_t *slice;
	const fsimg_setting_t *row;
	fsimg_setting_value_t converted;
	fsimg_layer_info_t layer;
	const char *text, *value, *qualifier, *key;
	size_t length, key_length;
	fsimg_status_t status;
	bool creating;

	creating = (parse->mode & MODES_CREATE) != 0;
	text = parse->settings[index];
	value = strchr(text, '=');
	if (value == NULL)
		return (syntax_naming(parse, "settings require key=value", text));
	length = (size_t)(value - text);
	value++;

	/* Find the key's row: the filesystem's, or the selected layer's. */
	slice = &fsimg_filesystem_settings;
	key = text;
	key_length = length;
	qualifier = memchr(text, ':', length);
	if (qualifier != NULL) {
		if (!find_layer(text, (size_t)(qualifier - text), &layer))
			return (syntax_naming(parse, "unknown setting layer", text));
		if (!layer_selected(parse, &layer))
			return (syntax_naming(parse,
			    "setting names a layer the stack does not contain",
			    text));
		if (creating && (layer.capabilities & FSIMG_CAP_WRITE) == 0)
			return (defer_setting(parse, index, length));
		slice = fsimg_layer_find(layer.name)->settings;
		key = qualifier + 1;
		key_length = length - (size_t)(key - text);
	}
	row = slice == NULL ? NULL : fsimg_setting_find(slice, key, key_length);
	if (row == NULL)
		return (syntax_naming(parse, qualifier != NULL ?
		    "unknown layer setting" : "unknown setting key", text));

	/* The row's repetition and mode, whatever its value would be. */
	if (setting_given(parse, row, index))
		return (syntax_naming(parse, "setting key is given more than once",
		    text));
	parse->setting_rows[index] = row;
	if (creating && (row->scope & FSIMG_SCOPE_WRITE) == 0)
		return (syntax_naming(parse, "setting applies only to reading",
		    text));
	if (!creating && (row->scope & FSIMG_SCOPE_READ) == 0)
		return (syntax_naming(parse, "setting applies only to creation",
		    text));

	status = convert_setting(parse, row, value, text, &converted);
	if (status != FSIMG_OK)
		return (status);
	if (row->syntax == FSIMG_SYNTAX_PATH)
		parse->options->setting_paths[row->file] = value;
	else
		fsimg_setting_store(row, &targets, &converted);
	return (FSIMG_OK);
}

/* The -F stack, parsed by the library's one grammar. A filesystem name
 * the grammar reserves has no driver; it is refused as unsupported once
 * the rest of the command line is known to be valid.
 */
static fsimg_status_t
parse_stack(cli_parse_t *parse, const char *text)
{
	fsimg_stack_t *stack = &parse->options->request.stack;
	fsimg_format_info_t info;
	size_t cursor;
	fsimg_status_t status;

	status = fsimg_stack_parse(text, stack, parse->error);
	if (status == FSIMG_EINVAL)
		parse->options->syntax_error = true;
	if (status != FSIMG_OK ||
	    stack->slots[FSIMG_LAYER_FILESYSTEM].state != FSIMG_SLOT_NAMED)
		return (status);
	parse->reserved = true;
	cursor = 0;
	while ((status = fsimg_format_next(&cursor, &info, parse->error)) == FSIMG_OK) {
		if (strcmp(info.name, stack->slots[FSIMG_LAYER_FILESYSTEM].name) == 0)
			parse->reserved = false;
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/*
 * --partition=N, name=TEXT or type=NAME. A type must be one the library's
 * type table, a GUID or an MBR byte expresses; whether the disk carries
 * it is the worker's question.
 */
static fsimg_status_t
parse_partition(cli_parse_t *parse)
{
	fsimg_worker_request_t *request = &parse->options->request;
	fsimg_type_selector_t type;
	const char *value, *text;
	uint64_t number;

	value = option_value(parse, OPT_PARTITION);
	if (strncmp(value, "name=", 5) == 0 || strncmp(value, "type=", 5) == 0) {
		text = value + 5;
		request->selector = value[0] == 'n' ? FSIMG_SELECT_NAME :
		    FSIMG_SELECT_TYPE;
		if (text[0] == '\0' || strlen(text) >= sizeof(request->partition_text))
			return (syntax_naming(parse,
			    "partition selector text must be 1 to 111 bytes", value));
		if (request->selector == FSIMG_SELECT_TYPE &&
		    fsimg_partition_type_parse(text, &type, parse->error) != FSIMG_OK)
			return (syntax_naming(parse, "unknown partition type", text));
		snprintf(request->partition_text, sizeof(request->partition_text),
		    "%s", text);
		return (FSIMG_OK);
	}
	if (fsimg_cli_number(value, false, &number, parse->error) != FSIMG_OK ||
	    number == 0 || number > UINT32_MAX)
		return (syntax_naming(parse,
		    "--partition requires N, name=TEXT or type=NAME", value));
	request->selector = FSIMG_SELECT_INDEX;
	request->partition_index = (uint32_t)number;
	return (FSIMG_OK);
}

static fsimg_status_t
parse_byte_option(cli_parse_t *parse, int key, bool positive, uint64_t *out)
{
	const char *value;

	value = option_value(parse, key);
	if (fsimg_cli_number(value, true, out, parse->error) != FSIMG_OK)
		return (syntax_naming(parse, "option requires an unsigned byte quantity",
		    option_table[option_index(key)].spelling));
	if (positive && *out == 0)
		return (syntax_naming(parse, "option requires a positive byte quantity",
		    option_table[option_index(key)].spelling));
	return (FSIMG_OK);
}

/* Remember the first recognized feature this build does not implement. It
 * is reported only after every syntax check passes.
 */
static void
note_unsupported(cli_parse_t *parse, const char *message)
{
	if (parse->unsupported == NULL)
		parse->unsupported = message;
}

/* Options every image mode shares: image, sandbox, scratch, budgets, stack
 * and settings. The stack precedes settings, which may name its layers.
 */
static fsimg_status_t
interpret_common(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_worker_request_t *request = &options->request;
	fsimg_layer_info_t layer;
	const char *value;
	uint64_t number;
	unsigned limit_keys;
	fsimg_status_t status;

	options->image = option_value(parse, 'f');
	options->json = option_count(parse, OPT_JSON) != 0;
	options->verbosity = option_count(parse, 'v');
	value = option_value(parse, 'S');
	if (value != NULL) {
		if (strcmp(value, "auto") == 0)
			request->sandbox = FSIMG_SANDBOX_AUTO;
		else if (strcmp(value, "required") == 0)
			request->sandbox = FSIMG_SANDBOX_REQUIRED;
		else if (strcmp(value, "off") == 0)
			request->sandbox = FSIMG_SANDBOX_OFF;
		else
			return (syntax(parse, "sandbox policy must be auto, required or off"));
	}
	if (option_value(parse, 'T') != NULL)
		options->scratch = option_value(parse, 'T');
	if (option_count(parse, 'm') != 0) {
		status = parse_byte_option(parse, 'm', false, &number);
		if (status != FSIMG_OK)
			return (status);
		if (number != 0)
			request->limits.memory_bytes = number;
		if (request->limits.memory_bytes < UINT64_C(16) * 1024 * 1024)
			return (syntax(parse, "memory budget must be at least 16M"));
	}
	/* Zero marks the descriptor limit unset while the -l keys are read, so
	 * that its default can follow the depth limit they settle.
	 */
	request->limits.max_open_files = 0;
	limit_keys = 0;
	for (unsigned i = 0; i < parse->limit_count; i++) {
		status = parse_limit(parse, parse->limits[i], &limit_keys);
		if (status != FSIMG_OK)
			return (status);
	}
	if (request->limits.max_open_files == 0)
		request->limits.max_open_files =
		    default_descriptor_limit(request->limits.max_depth);
	if (option_value(parse, 'F') != NULL) {
		status = parse_stack(parse, option_value(parse, 'F'));
		if (status != FSIMG_OK)
			return (status);
	}
	for (unsigned i = 0; i < parse->setting_count; i++) {
		status = parse_setting(parse, i);
		if (status != FSIMG_OK)
			return (status);
	}

	if (parse->reserved)
		note_unsupported(parse, "the named filesystem is reserved and not supported");
	if ((parse->mode & MODES_CREATE) == 0 && compression_selected(parse,
	    &layer))
		snprintf(request->compression, sizeof(request->compression),
		    "%s", layer.name);
	return (FSIMG_OK);
}

/*
 * The compression suffix outermost in an image name's basename, whose
 * remainder is left in name: its registry record and length, zero when it
 * has none.
 */
static size_t
compression_suffix(const char *image, char *name, size_t capacity,
    fsimg_layer_info_t *layer)
{
	const char *base;
	size_t length;

	base = strrchr(image, '/');
	base = base == NULL ? image : base + 1;
	snprintf(name, capacity, "%s", base);
	length = suffix_layer(name, FSIMG_LAYER_COMPRESSION, layer);
	name[strlen(name) - length] = '\0';
	return (length);
}

/* The compression -z selects, as tar's -z does. */
#define CLI_Z_COMPRESSION	"gzip"

/*
 * Whether a command selects a compression, and which one in layer: -z's,
 * and on creation under -a whatever the image name's outermost suffix
 * declares in the layer registry, which includes the reserved
 * compressions.
 */
static bool
compression_selected(const cli_parse_t *parse, fsimg_layer_info_t *layer)
{
	const char *image = parse->options->image;
	char name[NAME_MAX + 1];

	if (option_count(parse, 'z') != 0)
		return (find_layer(CLI_Z_COMPRESSION, strlen(CLI_Z_COMPRESSION),
		    layer));
	return ((parse->mode & MODES_CREATE) != 0 && option_count(parse, 'a') != 0 &&
	    image != NULL &&
	    compression_suffix(image, name, sizeof(name), layer) != 0);
}

/*
 * Apply the compression a creation selects. With -z and -a the image name
 * must end in the suffix of -z's compression. Under -a a container suffix
 * beneath the compression's selects nothing but must name the stack's
 * container, so the name cannot promise a container the image lacks. The
 * job carries the compression's name; one this build cannot write is
 * refused as unsupported once the command line is otherwise valid. cW
 * opens the compressed image through a spool, which needs scratch.
 */
static fsimg_status_t
apply_compression(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	const fsimg_stack_slot_t *container;
	fsimg_layer_info_t layer, suffix;
	char name[NAME_MAX + 1];
	size_t length;

	if (option_count(parse, 'a') != 0 && options->image != NULL) {
		length = compression_suffix(options->image, name, sizeof(name),
		    &suffix);
		if (option_count(parse, 'z') != 0 &&
		    (length == 0 || strcmp(suffix.name, CLI_Z_COMPRESSION) != 0))
			return (syntax(parse,
			    "-z with -a requires the image name to end in its suffix"));
		length = suffix_layer(name, FSIMG_LAYER_CONTAINER, &layer);
		container = &options->request.stack.slots[FSIMG_LAYER_CONTAINER];
		if (length != 0 && (container->state != FSIMG_SLOT_NAMED ||
		    strcmp(container->name, layer.name) != 0))
			return (syntax_naming(parse,
			    "-a: the image suffix names a container the stack does not",
			    options->image));
	}
	if (!compression_selected(parse, &layer))
		return (FSIMG_OK);
	if ((layer.capabilities & FSIMG_CAP_WRITE) == 0) {
		note_unsupported(parse, "writing this compression is not supported");
		return (FSIMG_OK);
	}
	snprintf(options->request.compression,
	    sizeof(options->request.compression), "%s", layer.name);
	if (options->verify_staged && strcmp(options->scratch, "-") == 0)
		return (syntax(parse,
		    "cW of a compressed image needs scratch for its spool, not -T -"));
	return (FSIMG_OK);
}

static fsimg_status_t
interpret_create(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_worker_request_t *request = &options->request;
	uint64_t number;
	fsimg_status_t status;

	request->kind = option_count(parse, 'n') != 0 ? FSIMG_JOB_PLAN : FSIMG_JOB_CREATE;
	options->verify_staged = parse->mode == MODE_CREATE_VERIFY;
	if (option_count(parse, 'd') != 0)
		request->flags |= FSIMG_JOB_PAYLOAD;
	if (request->stack.slots[FSIMG_LAYER_FILESYSTEM].state != FSIMG_SLOT_NAMED)
		return (syntax(parse, "c requires -F with a filesystem"));
	if (option_count(parse, 'e') != 0 && option_count(parse, 'R') == 0)
		return (syntax(parse, "-e requires -R"));
	if (option_count(parse, 'M') != 0 && option_count(parse, 's') != 0)
		return (syntax(parse, "-M conflicts with -s"));
	if (option_count(parse, 'R') != 0)
		request->write.flags |= FSIMG_WRITE_REPRODUCIBLE;
	if (option_count(parse, 'p') != 0)
		request->write.flags |= FSIMG_WRITE_IMPORT_POSIX;
	if (option_count(parse, 'M') != 0)
		request->write.flags |= FSIMG_WRITE_MINIMIZE;
	if (option_count(parse, 'L') != 0)
		request->write.label = option_value(parse, 'L');
	if (option_count(parse, 'e') != 0) {
		status = fsimg_cli_epoch(option_value(parse, 'e'), &request->write.epoch,
		    parse->error);
		if (status != FSIMG_OK)
			return (syntax_naming(parse, "-e requires a signed decimal Unix epoch",
			    option_value(parse, 'e')));
	}
	if (option_count(parse, 's') != 0) {
		status = parse_byte_option(parse, 's', true, &request->write.volume_bytes);
		if (status != FSIMG_OK)
			return (status);
	}
	if (option_count(parse, OPT_FREE_SPACE) != 0) {
		status = parse_byte_option(parse, OPT_FREE_SPACE, false, &number);
		if (status != FSIMG_OK)
			return (status);
		request->write.min_free_bytes = number;
	}
	if (options->image == NULL && request->kind == FSIMG_JOB_CREATE)
		return (syntax(parse, "c requires -f image unless -n is given"));
	if (options->image != NULL && strcmp(options->image, "-") == 0) {
		if (options->verify_staged)
			return (syntax(parse, "cW cannot verify an image written to standard output"));
		if (options->json)
			return (syntax(parse, "--json conflicts with an image on standard output"));
		if (option_count(parse, 'a') != 0)
			return (syntax(parse, "-a needs an image name, not standard output"));
		if (strcmp(options->scratch, "-") == 0)
			return (syntax(parse, "-T - conflicts with an image on standard output"));
		if (request->kind == FSIMG_JOB_CREATE)
			request->flags |= FSIMG_JOB_STDOUT;
	}
	status = apply_compression(parse);
	if (status != FSIMG_OK)
		return (status);
	status = read_create_operands(parse);
	if (status != FSIMG_OK)
		return (status);
	return (validate_creation(parse));
}

static fsimg_status_t
interpret_read(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_worker_request_t *request = &options->request;
	fsimg_format_info_t info;
	fsimg_status_t status;

	switch (parse->mode) {
	case MODE_EXTRACT:
		request->kind = option_count(parse, 'O') != 0 ? FSIMG_JOB_CAT :
		    FSIMG_JOB_EXTRACT;
		if (option_count(parse, 'k') != 0)
			request->flags |= FSIMG_JOB_SKIP_UNSUPPORTED;
		options->extract_here = option_count(parse, 'E') != 0;
		options->directory = option_value(parse, 'C');
		options->mtree = option_value(parse, OPT_MTREE);
		if (options->mtree != NULL && options->mtree[0] == '\0')
			return (syntax(parse, "--mtree requires a file name"));
		break;
	case MODE_LIST:
		request->kind = FSIMG_JOB_LIST;
		if (option_count(parse, OPT_NO_RECURSION) == 0)
			request->flags |= FSIMG_JOB_RECURSIVE;
		break;
	case MODE_VERIFY:
		request->kind = FSIMG_JOB_VERIFY;
		if (option_count(parse, 'd') != 0)
			request->flags |= FSIMG_JOB_PAYLOAD;
		break;
	default:
		request->kind = FSIMG_JOB_INFO;
		break;
	}
	if (request->kind == FSIMG_JOB_CAT && (options->json ||
	    option_count(parse, 'C') != 0 || option_count(parse, 'E') != 0 ||
	    option_count(parse, 'k') != 0 || options->mtree != NULL))
		return (syntax(parse, "-O rejects --json, --mtree, -C, -E and -k"));
	if (option_count(parse, OPT_PARTITION) != 0 &&
	    (option_count(parse, OPT_OFFSET) != 0 || option_count(parse, OPT_LENGTH) != 0))
		return (syntax(parse, "--partition conflicts with --offset and --length"));
	if (option_count(parse, OPT_PARTITION) != 0) {
		status = parse_partition(parse);
		if (status != FSIMG_OK)
			return (status);
	}
	if (option_count(parse, OPT_OFFSET) != 0 || option_count(parse, OPT_LENGTH) != 0)
		request->selector = FSIMG_SELECT_RANGE;
	/* A partition needs a table to be read; a range never reads one. */
	if (option_count(parse, OPT_PARTITION) != 0 &&
	    request->stack.slots[FSIMG_LAYER_SCHEME].state == FSIMG_SLOT_EMPTY)
		return (syntax(parse,
		    "--partition requires a partition scheme, which -F forces empty"));
	if (request->selector == FSIMG_SELECT_RANGE &&
	    request->layers.gpt_sector_bytes != 0)
		return (syntax(parse, "gpt:sector conflicts with --offset and --length"));
	if (request->selector == FSIMG_SELECT_RANGE &&
	    request->stack.slots[FSIMG_LAYER_SCHEME].state == FSIMG_SLOT_NAMED)
		return (syntax(parse,
		    "a partition scheme in -F conflicts with --offset and --length"));
	if (option_count(parse, OPT_OFFSET) != 0) {
		status = parse_byte_option(parse, OPT_OFFSET, false, &request->offset);
		if (status != FSIMG_OK)
			return (status);
	}
	if (option_count(parse, OPT_LENGTH) != 0) {
		status = parse_byte_option(parse, OPT_LENGTH, true, &request->length);
		if (status != FSIMG_OK)
			return (status);
	}
	if (options->image == NULL)
		return (syntax(parse, "-f image is required"));
	status = read_members(parse);
	if (status != FSIMG_OK)
		return (status);
	if (request->kind == FSIMG_JOB_EXTRACT && !options->extract_here &&
	    !fsimg_cli_stem(options->image, options->stem, sizeof(options->stem)))
		return (syntax_naming(parse,
		    "cannot name the extraction directory after the image; use -E",
		    options->image));
	if (strcmp(options->image, "-") == 0) {
		if (strcmp(options->scratch, "-") == 0)
			return (syntax(parse, "-T - conflicts with an image on standard input"));
		request->input.kind = FSIMG_INPUT_STREAM;
	}
	if (fsimg_job_format(request)[0] == '\0' || parse->reserved)
		return (FSIMG_OK);
	return (find_format(parse, fsimg_job_format(request), &info));
}

/*
 * Create operands are host paths. getopt has stopped at the first one, and
 * from there "-C dir" or "-Cdir" changes the directory for the operands
 * after it, until a "--" makes every later word an operand. Every -C must
 * apply to at least one operand, so a misplaced one is not silently lost.
 * An operand "@name" is a manifest (specs/design-mtree.md, "Command line").
 */
static fsimg_status_t
read_create_operands(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_cli_operand_t *operand;
	const char *directory, *word;
	fsimg_status_t status;
	bool literal, pending;

	if (option_count(parse, 'C') > 1)
		return (syntax(parse, "-C applies to no operand"));
	options->operands = calloc((size_t)(parse->argc - parse->first_operand) + 1,
	    sizeof(*options->operands));
	if (options->operands == NULL)
		return (fsimg_error_set(parse->error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "operand allocation failed"));
	directory = option_value(parse, 'C');
	pending = directory != NULL;
	literal = parse->options_ended;
	for (int i = parse->first_operand; i < parse->argc; i++) {
		word = parse->argv[i];
		if (!literal && strcmp(word, "--") == 0) {
			literal = true;
			continue;
		}
		if (!literal && strncmp(word, "-C", 2) == 0) {
			if (pending)
				return (syntax(parse, "-C applies to no operand"));
			if (word[2] != '\0')
				directory = word + 2;
			else if (i + 1 < parse->argc)
				directory = parse->argv[++i];
			else
				return (syntax_naming(parse, "option requires an argument", "-C"));
			if (directory[0] == '\0')
				return (syntax(parse, "-C requires a nonempty directory name"));
			pending = true;
			continue;
		}
		operand = &options->operands[options->operand_count];
		operand->directory = directory;
		if (word[0] == '@') {
			/* A manifest, in every position, after "--" too, as
			 * tar reads an archive operand; "./@name" names a
			 * host object.
			 */
			if (word[1] == '\0')
				return (syntax(parse, "@ requires a manifest name"));
			operand->manifest = true;
			operand->host_path = word + 1;
		} else {
			operand->host_path = word;
			status = normalize_path(parse, word, false,
			    &operand->image_path);
			if (status != FSIMG_OK)
				return (status);
		}
		options->operand_count++;
		pending = false;
	}
	if (pending)
		return (syntax(parse, "-C applies to no operand"));
	if (options->operand_count == 0)
		return (syntax(parse, "c requires at least one operand"));
	return (FSIMG_OK);
}

/* The image path a create operand or a member names: its components
 * without empty and "." ones, under the image root. ".." is refused rather
 * than resolved, since stripping it would silently change which host object
 * populates the name, or which entry a member selects.
 */
static fsimg_status_t
normalize_path(cli_parse_t *parse, const char *word, bool member, char **out)
{
	const char *component, *end;
	char *path;
	size_t length, used;

	*out = NULL;
	length = strlen(word);
	if (length == 0)
		return (syntax(parse, member ? "a member is empty" :
		    "an operand is empty"));
	path = malloc(length + 2);
	if (path == NULL)
		return (fsimg_error_set(parse->error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "operand allocation failed"));
	*out = path;
	path[0] = '/';
	used = 1;
	for (component = word; *component != '\0'; component = end) {
		end = strchr(component, '/');
		if (end == NULL)
			end = component + strlen(component);
		length = (size_t)(end - component);
		if (*end == '/')
			end++;
		if (length == 0 || (length == 1 && component[0] == '.'))
			continue;
		if (length == 2 && component[0] == '.' && component[1] == '.')
			return (syntax_naming(parse, member ?
			    "a member contains a .. component" :
			    "an operand contains a .. component", word));
		if (used > 1)
			path[used++] = '/';
		bcopy(component, path + used, length);
		used += length;
	}
	path[used] = '\0';
	return (FSIMG_OK);
}

/* Members are image paths, relative or absolute; both name the same entry.
 * The worker validates encoding and resolves components with the format's
 * rules, so members that name one entry, by repetition, nesting or a
 * format's aliases, are left for it to select once.
 */
static fsimg_status_t
read_members(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_status_t status;
	int count;

	count = parse->argc - parse->first_operand;
	if (parse->mode == MODE_VERIFY || parse->mode == MODE_INFO) {
		if (count != 0)
			return (syntax_naming(parse, "unexpected operand",
			    parse->argv[parse->first_operand]));
		return (FSIMG_OK);
	}
	if (options->request.kind == FSIMG_JOB_CAT && count != 1)
		return (syntax(parse, "-O requires exactly one member"));
	if (count == 0)
		return (FSIMG_OK);
	/* One slot more, as for operands: a failed normalization leaves its
	 * allocation in the slot after the last member.
	 */
	options->members = calloc((size_t)count + 1, sizeof(*options->members));
	if (options->members == NULL)
		return (fsimg_error_set(parse->error, FSIMG_ENOMEM, 0,
		    FSIMG_OP_CONTEXT, "member allocation failed"));
	for (int i = 0; i < count; i++) {
		status = normalize_path(parse,
		    parse->argv[parse->first_operand + i], true,
		    &options->members[i]);
		if (status != FSIMG_OK)
			return (status);
		options->member_count++;
	}
	options->request.members = (const char *const *)options->members;
	options->request.member_count = options->member_count;
	return (FSIMG_OK);
}

static fsimg_status_t
find_format(cli_parse_t *parse, const char *name, fsimg_format_info_t *info)
{
	size_t cursor;
	fsimg_status_t status;

	cursor = 0;
	for (;;) {
		status = fsimg_format_next(&cursor, info, parse->error);
		if (status == FSIMG_DONE)
			return (syntax_naming(parse, "unknown filesystem name", name));
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
validate_creation(cli_parse_t *parse)
{
	fsimg_cli_options_t *options = parse->options;
	fsimg_worker_request_t *request = &options->request;
	fsimg_format_info_t info;
	uint32_t sizes[2], masks[2];
	unsigned shift;
	fsimg_status_t status;

	if (parse->reserved)
		return (FSIMG_OK);
	status = find_format(parse, fsimg_job_format(request), &info);
	if (status != FSIMG_OK)
		return (status);
	if (option_count(parse, 'L') == 0)
		request->write.label = info.max_label_bytes != 0 ? "FSIMG" : NULL;
	/* Like the serial, a format's UUID is random unless reproducible or
	 * given; a given nil UUID is kept.
	 */
	options->random_uuid = (info.write_settings & FSIMG_SETTING_VOLUME_UUID) != 0 &&
	    (request->write.flags & FSIMG_WRITE_REPRODUCIBLE) == 0 &&
	    !setting_given(parse, fsimg_setting_find(&fsimg_filesystem_settings,
	    "uuid", strlen("uuid")), parse->setting_count);
	if (request->write.label != NULL &&
	    strnlen(request->write.label, sizeof(request->label_storage)) ==
	    sizeof(request->label_storage))
		return (syntax(parse, "volume label exceeds the command's label representation"));
	sizes[0] = request->write.sector_bytes;
	sizes[1] = request->write.allocation_unit_bytes;
	masks[0] = info.sector_size_mask;
	masks[1] = info.write_allocation_unit_size_mask;
	for (unsigned i = 0; i < 2; i++) {
		if (sizes[i] == 0)
			continue;
		if ((sizes[i] & (sizes[i] - 1)) != 0)
			return (syntax(parse, "geometry byte quantities must be powers of two"));
		shift = 0;
		while ((UINT32_C(1) << shift) != sizes[i])
			shift++;
		if ((masks[i] & (UINT32_C(1) << shift)) == 0)
			return (syntax(parse, "geometry is outside the selected writer profile"));
	}
	/* Without a sector size, the profile's cluster mask already holds no
	 * cluster below its smallest sector, and the writer pairs the two.
	 */
	if (sizes[0] != 0 && sizes[1] != 0 && sizes[1] < sizes[0])
		return (syntax(parse, "cluster size is smaller than sector size"));
	return (FSIMG_OK);
}

/* Remove suffix from the end of name, ASCII case-insensitively, when what
 * remains is nonempty.
 */
static bool
strip_suffix(char *name, const char *suffix)
{
	size_t length, suffix_length;

	length = strlen(name);
	suffix_length = strlen(suffix);
	if (length <= suffix_length ||
	    strcasecmp(name + length - suffix_length, suffix) != 0)
		return (false);
	name[length - suffix_length] = '\0';
	return (true);
}

/* The layer of the given kind whose registered suffix ends name, ASCII
 * case-insensitively, leaving a nonempty remainder, and that suffix's
 * length; zero when none does. The layer registry owns these suffixes.
 */
static size_t
suffix_layer(const char *name, fsimg_layer_kind_t kind,
    fsimg_layer_info_t *layer)
{
	size_t cursor, length, suffix_length;

	length = strlen(name);
	cursor = 0;
	while (fsimg_layer_next(&cursor, layer, NULL) == FSIMG_OK) {
		suffix_length = strlen(layer->suffix);
		if (layer->kind == kind && suffix_length != 0 &&
		    length > suffix_length &&
		    strcasecmp(name + length - suffix_length, layer->suffix) == 0)
			return (suffix_length);
	}
	return (0);
}

static size_t
layer_suffix_length(const char *name, fsimg_layer_kind_t kind)
{
	fsimg_layer_info_t layer;

	return (suffix_layer(name, kind, &layer));
}

/*
 * The extraction stem: the basename with suffixes removed from the outside
 * in, each class at most once and in this order: one compression suffix,
 * one container suffix, one generic image suffix. Each step looks only at
 * the current outermost suffix, so "disk.img.gz" yields "disk" and
 * "rootfs.vhd.img" yields "rootfs.vhd". A name with nothing stripped, or
 * whose remainder is not a single usable host name, yields no stem.
 */
bool
fsimg_cli_stem(const char *image, char *stem, size_t capacity)
{
	static const fsimg_layer_kind_t outer_layers[] = {
		FSIMG_LAYER_COMPRESSION, FSIMG_LAYER_CONTAINER
	};
	const char *base;
	size_t length;
	bool stripped;

	if (image == NULL || strcmp(image, "-") == 0)
		return (false);
	base = strrchr(image, '/');
	base = base == NULL ? image : base + 1;
	if (strlen(base) >= capacity)
		return (false);
	strcpy(stem, base);
	stripped = false;
	for (size_t i = 0; i < sizeof(outer_layers) / sizeof(outer_layers[0]); i++) {
		length = layer_suffix_length(stem, outer_layers[i]);
		stem[strlen(stem) - length] = '\0';
		stripped |= length != 0;
	}
	for (size_t i = 0; i < sizeof(generic_suffixes) / sizeof(generic_suffixes[0]); i++) {
		if (strip_suffix(stem, generic_suffixes[i])) {
			stripped = true;
			break;
		}
	}
	return (stripped && strcmp(stem, ".") != 0 && strcmp(stem, "..") != 0);
}

/* Reject an image or -C name that cannot be passed to the host at all;
 * an empty -C would otherwise read as the working directory.
 */
static fsimg_status_t
check_image_name(cli_parse_t *parse)
{
	const char *image = parse->options->image;
	const char *directory = option_value(parse, 'C');

	if (image != NULL && image[0] == '\0')
		return (syntax(parse, "-f requires a nonempty image name"));
	if (directory != NULL && directory[0] == '\0')
		return (syntax(parse, "-C requires a nonempty directory name"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_cli_parse(int argc, char **argv, fsimg_cli_options_t *options,
    fsimg_error_t *error)
{
	cli_parse_t parse;
	fsimg_status_t status;

	bzero(options, sizeof(*options));
	fsimg_worker_request_init(&options->request);
	options->mode = "";
	options->scratch = getenv("TMPDIR");
	if (options->scratch == NULL)
		options->scratch = "/tmp";
	bzero(&parse, sizeof(parse));
	parse.options = options;
	parse.error = error;
	parse.argc = argc;
	parse.argv = argv;
	if (argc < 2) {
		options->command = FSIMG_CLI_USAGE;
		options->overview = true;
		return (syntax(&parse, "a mode is required"));
	}

	status = expand_bundle(&parse);
	if (status == FSIMG_OK)
		status = read_option_words(&parse);
	if (status == FSIMG_OK)
		status = select_mode(&parse);
	if (status == FSIMG_OK && (options->command == FSIMG_CLI_PROCESS ||
	    options->command == FSIMG_CLI_FORMATS))
		status = check_option_modes(&parse);
	if (status == FSIMG_OK && options->command == FSIMG_CLI_FORMATS) {
		options->json = option_count(&parse, OPT_JSON) != 0;
		options->verbosity = option_count(&parse, 'v');
		if (parse.first_operand != parse.argc)
			status = syntax_naming(&parse, "unexpected operand",
			    parse.argv[parse.first_operand]);
	} else if (status == FSIMG_OK && options->command == FSIMG_CLI_PROCESS) {
		status = interpret_common(&parse);
		if (status == FSIMG_OK)
			status = check_image_name(&parse);
		if (status == FSIMG_OK && (parse.mode & MODES_CREATE) != 0)
			status = interpret_create(&parse);
		else if (status == FSIMG_OK)
			status = interpret_read(&parse);
	}
	/* Details on stderr: -v for W, i and --formats, whose -v has no other
	 * meaning; -vv for c, x and t, whose -v is the entry stream or listing.
	 */
	if (options->verbosity >= ((parse.mode & (MODES_CREATE | MODE_EXTRACT |
	    MODE_LIST)) != 0 ? 2u : 1u))
		options->request.flags |= FSIMG_JOB_VERBOSE;
	if (status == FSIMG_OK && parse.unsupported != NULL)
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_CONTEXT, parse.unsupported);
	free(parse.limits);
	free(parse.settings);
	free(parse.setting_rows);
	return (status);
}

void
fsimg_cli_release(fsimg_cli_options_t *options)
{
	for (unsigned i = 0; i < options->operand_count; i++)
		free(options->operands[i].image_path);
	/* A failed normalization leaves its allocation in the next slot. */
	if (options->operands != NULL)
		free(options->operands[options->operand_count].image_path);
	free(options->operands);
	for (uint32_t i = 0; i < options->member_count; i++)
		free(options->members[i]);
	if (options->members != NULL)
		free(options->members[options->member_count]);
	free(options->members);
	free(options->words);
	options->operands = NULL;
	options->operand_count = 0;
	options->members = NULL;
	options->member_count = 0;
	options->words = NULL;
}
