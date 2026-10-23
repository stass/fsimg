/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "mtree.h"
#include "unicode.h"

/* A manifest is read as logical lines. A physical line ends at LF, and an
 * unescaped backslash ending a physical line joins the next one, both
 * bytes removed, except in a comment, which never continues. A logical
 * line is blank, a comment (its first word begins with '#'; any byte but
 * NUL may follow), or words of bytes 0x21 through 0x7E separated by spaces
 * and tabs: a first word and keyword=value words.
 *
 * The first word selects the line. "/set" and "/unset" change the default
 * keywords; ".." leaves the current directory; anything else names an
 * entry. A name whose decoded bytes hold a '/' is a full entry, a path from
 * the image root with an optional leading "." component; any other name is
 * a relative entry in the current directory, and a relative directory
 * becomes the current directory. The relative "." at the image root opens
 * the root itself, so the ".." that mtree -c writes after the root's
 * children closes it; a ".." with nothing open is refused.
 *
 * An entry's keywords are the line's own over the /set defaults. A keyword
 * that does not apply to the entry (a link target on a file, a seed on host
 * content) is refused when the line gives it and ignored when it is only a
 * default, so one /set serves entries of every kind. A size default, too,
 * describes files: a link checks only a size its own line gives.
 */

#define MTREE_WORDS_MAX		64u
#define MTREE_KEYWORD_NAME_MAX	32u
#define MTREE_DECIMAL_DIGITS_MAX 20u
#define MTREE_OCTAL_DIGITS_MAX	22u
#define MTREE_FRACTION_DIGITS_MAX 9u
#define MTREE_READ_BUFFER_SIZE	8192u
#define MTREE_ESCAPE_LENGTH	4u
#define MTREE_NO_WORD		SIZE_MAX

/* Keywords in their classes: carried into the entry, about the host object
 * and only validated, and refused as unsupported. A keyword's bit in a
 * values mask is 1 << keyword.
 */
typedef enum mtree_keyword {
	KEYWORD_TYPE,
	KEYWORD_UID,
	KEYWORD_GID,
	KEYWORD_MODE,
	KEYWORD_TIME,
	KEYWORD_SIZE,
	KEYWORD_LINK,
	KEYWORD_CONTENTS,
	KEYWORD_SHA256,
	KEYWORD_CONTENT,
	KEYWORD_SEED,
	KEYWORD_HOLES,
	KEYWORD_NLINK,
	KEYWORD_INODE,
	KEYWORD_DEVICE,
	KEYWORD_RESDEVICE,
	KEYWORD_TAGS,
	KEYWORD_UNAME,
	KEYWORD_GNAME,
	KEYWORD_FLAGS,
	KEYWORD_UNSUPPORTED,
	KEYWORD_UNKNOWN
} mtree_keyword_t;

#define KEYWORD_BIT(keyword)	(UINT32_C(1) << (keyword))

typedef struct mtree_keyword_name {
	const char	*name;
	mtree_keyword_t	 keyword;
} mtree_keyword_name_t;

static const mtree_keyword_name_t keyword_names[] = {
	{ "type", KEYWORD_TYPE },
	{ "uid", KEYWORD_UID },
	{ "gid", KEYWORD_GID },
	{ "mode", KEYWORD_MODE },
	{ "time", KEYWORD_TIME },
	{ "size", KEYWORD_SIZE },
	{ "link", KEYWORD_LINK },
	{ "contents", KEYWORD_CONTENTS },
	{ "sha256digest", KEYWORD_SHA256 },
	{ "sha256", KEYWORD_SHA256 },
	{ "fsimg.content", KEYWORD_CONTENT },
	{ "fsimg.seed", KEYWORD_SEED },
	{ "fsimg.holes", KEYWORD_HOLES },
	{ "nlink", KEYWORD_NLINK },
	{ "inode", KEYWORD_INODE },
	{ "device", KEYWORD_DEVICE },
	{ "resdevice", KEYWORD_RESDEVICE },
	{ "tags", KEYWORD_TAGS },
	{ "uname", KEYWORD_UNAME },
	{ "gname", KEYWORD_GNAME },
	{ "flags", KEYWORD_FLAGS },
	/* Claims fsimg cannot verify, and mtree(8)'s own run controls. */
	{ "cksum", KEYWORD_UNSUPPORTED },
	{ "md5", KEYWORD_UNSUPPORTED },
	{ "md5digest", KEYWORD_UNSUPPORTED },
	{ "sha1", KEYWORD_UNSUPPORTED },
	{ "sha1digest", KEYWORD_UNSUPPORTED },
	{ "rmd160", KEYWORD_UNSUPPORTED },
	{ "rmd160digest", KEYWORD_UNSUPPORTED },
	{ "ripemd160digest", KEYWORD_UNSUPPORTED },
	{ "sha384", KEYWORD_UNSUPPORTED },
	{ "sha384digest", KEYWORD_UNSUPPORTED },
	{ "sha512", KEYWORD_UNSUPPORTED },
	{ "sha512digest", KEYWORD_UNSUPPORTED },
	{ "optional", KEYWORD_UNSUPPORTED },
	{ "ignore", KEYWORD_UNSUPPORTED },
	{ "nochange", KEYWORD_UNSUPPORTED }
};

/* The spelling an error uses for each keyword that can be present. An
 * alias is reported by this canonical name: a refused sha256 reads as
 * sha256digest.
 */
static const char *const keyword_spellings[] = {
	[KEYWORD_TYPE] = "type",
	[KEYWORD_UID] = "uid",
	[KEYWORD_GID] = "gid",
	[KEYWORD_MODE] = "mode",
	[KEYWORD_TIME] = "time",
	[KEYWORD_SIZE] = "size",
	[KEYWORD_LINK] = "link",
	[KEYWORD_CONTENTS] = "contents",
	[KEYWORD_SHA256] = "sha256digest",
	[KEYWORD_CONTENT] = "fsimg.content",
	[KEYWORD_SEED] = "fsimg.seed",
	[KEYWORD_HOLES] = "fsimg.holes",
	[KEYWORD_NLINK] = "nlink",
	[KEYWORD_INODE] = "inode",
	[KEYWORD_DEVICE] = "device",
	[KEYWORD_RESDEVICE] = "resdevice",
	[KEYWORD_TAGS] = "tags",
	[KEYWORD_UNAME] = "uname",
	[KEYWORD_GNAME] = "gname",
	[KEYWORD_FLAGS] = "flags"
};

_Static_assert(sizeof(keyword_spellings) / sizeof(keyword_spellings[0]) ==
    KEYWORD_UNSUPPORTED, "every keyword with a value has a spelling");
_Static_assert(KEYWORD_UNKNOWN < 32, "keyword bits fit a uint32_t mask");

/* One word of the current line, borrowed from the line buffer. */
typedef struct mtree_word {
	const unsigned char *bytes;
	size_t		 length;
} mtree_word_t;

/* The values of the keywords one line or the /set defaults give. Only the
 * fields of keywords whose bit is in present are meaningful. contents
 * points at a path-limit buffer the parser owns; link and holes are
 * bounded by the format, so they are held inline.
 */
typedef struct mtree_values {
	char		*contents;
	fsimg_mtree_hole_t holes[FSIMG_MTREE_HOLES_MAX];
	char		 link[FSIMG_SYMLINK_TARGET_MAX + 1];
	unsigned char	 digest[FSIMG_SHA256_DIGEST_LENGTH];
	int64_t		 time_seconds;
	uint64_t	 seed;
	uint64_t	 size;
	size_t		 contents_length;
	size_t		 hole_count;
	size_t		 link_length;
	fsimg_entry_kind_t kind;
	fsimg_mtree_content_t content;
	uint32_t	 gid;
	uint32_t	 mode;
	uint32_t	 present;
	uint32_t	 time_nanoseconds;
	uint32_t	 uid;
} mtree_values_t;

/* Where an entry line's path lies: the image root (the relative "." while
 * the current directory is the root), a relative name appended to the
 * current directory, or a full path from the root.
 */
typedef enum mtree_entry_form {
	ENTRY_ROOT,
	ENTRY_RELATIVE,
	ENTRY_FULL
} mtree_entry_form_t;

/* The whole parse state, one allocation. The buffers at the end are carved
 * from storage: the logical line (line_limit bytes plus one for a pending
 * continuation backslash), the current directory and the full-entry path
 * (each the path limit plus room for a leading '/' and a NUL), and the
 * decoded contents= paths of the defaults and of the line.
 *
 * directory holds the current directory's absolute path, directory_length
 * bytes long, NUL-terminated between lines. While one entry line is
 * parsed, a relative entry's path is that path with "/name" appended in
 * place, which costs the name's length rather than a copy of the
 * directory; parse_entry keeps the append when the entry becomes the
 * current directory and cuts it off on every other outcome.
 *
 * first_word is the index in line of the line's first kept byte that is
 * not a blank, or MTREE_NO_WORD while there is none; it alone decides
 * whether a line is blank, a comment or words.
 */
typedef struct mtree_parser {
	fsimg_session_t	*session;
	const fsimg_mtree_input_t *input;
	unsigned char	*line;
	char		*directory;
	char		*full_path;
	mtree_values_t	 defaults;
	mtree_values_t	 line_values;
	mtree_word_t	 words[MTREE_WORDS_MAX];
	uint64_t	 input_offset;
	uint64_t	 line_offset;
	uint64_t	 line_number;
	uint64_t	 next_line_number;
	size_t		 directory_length;
	size_t		 first_word;
	size_t		 line_capacity;
	size_t		 line_length;
	size_t		 line_limit;
	size_t		 path_limit;
	size_t		 read_cursor;
	size_t		 read_used;
	size_t		 word_count;
	bool		 input_ended;
	bool		 root_open;
	unsigned char	 read_buffer[MTREE_READ_BUFFER_SIZE];
	unsigned char	 storage[];
} mtree_parser_t;

static fsimg_status_t apply_set(mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t apply_unset(mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t build_entry_path(mtree_parser_t *,
    mtree_entry_form_t *, const char **, size_t *, fsimg_error_t *);
static fsimg_status_t callback_status(fsimg_status_t, fsimg_error_t *);
static bool contents_path_valid(const char *, size_t);
static fsimg_status_t decode_word(const unsigned char *, size_t, char *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t deliver_entry(mtree_parser_t *, mtree_entry_form_t,
    const char *, fsimg_mtree_entry_t *, fsimg_error_t *);
static const mtree_values_t *find_keyword(const mtree_parser_t *,
    mtree_keyword_t, uint32_t);
static uint32_t inapplicable_keywords(fsimg_entry_kind_t,
    fsimg_mtree_content_t);
static fsimg_status_t leave_directory(mtree_parser_t *, fsimg_error_t *);
static bool line_is_comment(const mtree_parser_t *);
static mtree_keyword_t lookup_keyword(const unsigned char *, size_t);
static mtree_keyword_t lowest_keyword(uint32_t);
static void name_failed_line(const mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t parse_decimal(const unsigned char *, size_t,
    uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t parse_device(const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t parse_digest(const unsigned char *, size_t,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t parse_entry(mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t parse_holes(const unsigned char *, size_t,
    mtree_values_t *, fsimg_error_t *);
static fsimg_status_t parse_keyword(mtree_parser_t *, mtree_values_t *,
    const mtree_word_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t parse_line(mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t parse_mode(const unsigned char *, size_t, uint32_t *,
    fsimg_error_t *);
static fsimg_status_t parse_time(const unsigned char *, size_t,
    mtree_values_t *, fsimg_error_t *);
static fsimg_status_t parse_value(mtree_parser_t *, mtree_values_t *,
    mtree_keyword_t, const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t read_logical_line(mtree_parser_t *, bool *,
    fsimg_error_t *);
static fsimg_status_t refill(mtree_parser_t *, fsimg_error_t *);
static fsimg_status_t refuse(fsimg_error_t *, fsimg_status_t, const char *);
static fsimg_status_t refuse_keyword(fsimg_error_t *, fsimg_status_t,
    const char *, const char *, size_t);
static fsimg_status_t resolve_contents(const mtree_parser_t *, uint32_t,
    fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t resolve_entry(const mtree_parser_t *, const char *,
    fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t resolve_metadata(const mtree_parser_t *,
    fsimg_mtree_entry_t *, fsimg_error_t *);
static fsimg_status_t split_words(mtree_parser_t *, size_t,
    fsimg_error_t *);
static bool word_is(const mtree_word_t *, const char *);

uint64_t
fsimg_mtree_line_limit(const fsimg_ctx_t *ctx)
{
	/* Two encoded paths (the entry's name and a contents= path), an
	 * encoded link target and 4096 bytes of other keywords; each decoded
	 * byte encodes in at most four ("\ooo"). The path limit is a
	 * uint32_t, so the sum cannot overflow.
	 */
	return (UINT64_C(8) * ctx->options.max_path_bytes +
	    UINT64_C(4) * FSIMG_SYMLINK_TARGET_MAX + 4096);
}

fsimg_status_t
fsimg_mtree_parse(fsimg_session_t *session, const fsimg_mtree_input_t *input,
    fsimg_error_t *error)
{
	mtree_parser_t *parser;
	fsimg_error_t detail;
	uint64_t line_capacity, path_buffer, storage_bytes, total;
	size_t allocation_size;
	bool found;
	fsimg_status_t status;

	if (session == NULL || session->ctx == NULL || input == NULL ||
	    input->read == NULL || input->entry == NULL)
		return (refuse(error, FSIMG_EINVAL,
		    "missing manifest session, input or callbacks"));

	/* Size the buffers in uint64_t first: a 32-bit host may not hold them. */
	line_capacity = fsimg_mtree_line_limit(session->ctx) + 1;
	path_buffer = (uint64_t)session->ctx->options.max_path_bytes + 2;
	if (!fsimg_mul_u64(path_buffer, 4, &storage_bytes) ||
	    !fsimg_add_u64(storage_bytes, line_capacity, &storage_bytes) ||
	    !fsimg_add_u64(storage_bytes, sizeof(*parser), &total) ||
	    !fsimg_to_size(total, &allocation_size))
		return (refuse(error, FSIMG_ERANGE,
		    "manifest parser buffers exceed the host range"));
	parser = fsimg_alloc(session->ctx, allocation_size, &detail);
	if (parser == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}

	parser->session = session;
	parser->input = input;
	parser->line_capacity = (size_t)line_capacity;
	parser->line_limit = parser->line_capacity - 1;
	parser->path_limit = session->ctx->options.max_path_bytes;
	parser->line = parser->storage;
	parser->directory = (char *)parser->line + parser->line_capacity;
	parser->full_path = parser->directory + (size_t)path_buffer;
	parser->defaults.contents = parser->full_path + (size_t)path_buffer;
	parser->line_values.contents = parser->defaults.contents +
	    (size_t)path_buffer;
	parser->directory[0] = '/';
	parser->directory[1] = '\0';
	parser->directory_length = 1;
	parser->next_line_number = 1;

	for (;;) {
		status = read_logical_line(parser, &found, error);
		if (status != FSIMG_OK || !found)
			break;
		status = parse_line(parser, error);
		if (status != FSIMG_OK)
			break;
	}
	if (status != FSIMG_OK)
		name_failed_line(parser, error);
	fsimg_dealloc(parser);
	return (status);
}

static fsimg_status_t
refuse(fsimg_error_t *error, fsimg_status_t status, const char *message)
{
	return (fsimg_error_set(error, status, 0, FSIMG_OP_SOURCE, message));
}

/* Name a keyword in the message. Callers pass a keyword name of at most
 * MTREE_KEYWORD_NAME_MAX bytes of 0x21 through 0x7E, so the quote is
 * bounded and printable.
 */
static fsimg_status_t
refuse_keyword(fsimg_error_t *error, fsimg_status_t status, const char *reason,
    const char *name, size_t name_length)
{
	char message[MTREE_KEYWORD_NAME_MAX + 96];

	snprintf(message, sizeof(message), "%s \"%.*s\"", reason,
	    (int)name_length, name);
	return (refuse(error, status, message));
}

/* A callback's status, with an error record that carries it: a callback
 * that failed without filling the record gets a generic one, so the record
 * is never read as the caller left it. Both callbacks return through here.
 */
static fsimg_status_t
callback_status(fsimg_status_t status, fsimg_error_t *error)
{
	if (status != FSIMG_OK && error != NULL && error->status != status)
		fsimg_error_set(error, status, 0, FSIMG_OP_SOURCE,
		    "manifest callback failed without an error record");
	return (status);
}

/* Name the line being parsed in a failure's record. */
static void
name_failed_line(const mtree_parser_t *parser, fsimg_error_t *error)
{
	fsimg_mtree_name_line(error, parser->line_number, parser->line_offset);
}

/* Prefix the failure with the line it concerns: the logical line's byte
 * offset in offset and its first physical line's number in the message.
 * The record carries the failure's status, as every failure path sets it.
 */
void
fsimg_mtree_name_line(fsimg_error_t *error, uint64_t line, uint64_t offset)
{
	char message[sizeof(error->message)];
	int count;

	if (error == NULL)
		return;
	/* The terminator is forced rather than assumed of the record. */
	bcopy(error->message, message, sizeof(message));
	message[sizeof(message) - 1] = '\0';
	error->offset = offset;
	error->offset_valid = true;
	count = snprintf(error->message, sizeof(error->message),
	    "manifest line %ju: %s", (uintmax_t)line, message);
	if (count < 0 || (size_t)count >= sizeof(error->message))
		error->message_truncated = true;
}

static fsimg_status_t
refill(mtree_parser_t *parser, fsimg_error_t *error)
{
	size_t length;
	fsimg_status_t status;

	length = 0;
	status = parser->input->read(parser->input->read_argument,
	    parser->read_buffer, sizeof(parser->read_buffer), &length, error);
	if (status != FSIMG_OK)
		return (callback_status(status, error));
	if (length > sizeof(parser->read_buffer))
		return (refuse(error, FSIMG_EINVAL,
		    "manifest read returned more than it was asked"));
	parser->read_cursor = 0;
	parser->read_used = length;
	parser->input_ended = length == 0;
	return (FSIMG_OK);
}

/* Assemble the next logical line into parser->line, or report the end of
 * the manifest with *found false.
 *
 * A physical line continues into the next when it ends with an unescaped
 * backslash, that is, with an odd run of backslashes ("\\" is an escaped
 * one), and the logical line is not a comment. A comment never continues,
 * because mtree -c writes raw paths into comments: the comment before a
 * directory named "d\" ends in a backslash and must not swallow the
 * directory's line. Whether the line is a comment is decided by its first
 * kept byte that is not a blank, parser->first_word, which parse_line uses
 * as well; a continuation that removes that very byte unsets it, so the
 * joined text alone decides. The run of backslashes is counted as bytes
 * arrive, so each byte is looked at once.
 *
 * A manifest ends after an LF: input that stops inside a line was cut
 * short, and a line that might have lost keywords is refused rather than
 * read. The byte offset counts bytes read, one at a time, so it cannot
 * overflow.
 */
static fsimg_status_t
read_logical_line(mtree_parser_t *parser, bool *found, fsimg_error_t *error)
{
	size_t backslash_run;
	unsigned char byte;
	fsimg_status_t status;

	*found = false;
	parser->line_length = 0;
	parser->line_offset = parser->input_offset;
	parser->line_number = parser->next_line_number;
	parser->first_word = MTREE_NO_WORD;
	backslash_run = 0;
	for (;;) {
		if (parser->read_cursor == parser->read_used) {
			if (!parser->input_ended) {
				status = refill(parser, error);
				if (status != FSIMG_OK)
					return (status);
			}
			if (parser->input_ended) {
				if (parser->input_offset == parser->line_offset)
					return (FSIMG_OK);
				return (refuse(error, FSIMG_EINVAL,
				    "manifest ends inside a line"));
			}
		}
		byte = parser->read_buffer[parser->read_cursor++];
		parser->input_offset++;
		if (byte == '\0')
			return (refuse(error, FSIMG_EINVAL,
			    "NUL byte in manifest"));
		if (byte != '\n') {
			/* One byte past the limit stays room for a backslash
			 * that an LF may still remove.
			 */
			if (parser->line_length == parser->line_capacity)
				return (fsimg_limit_error(error,
				    FSIMG_RESOURCE_PATH, parser->line_limit,
				    parser->line_length));
			if (parser->first_word == MTREE_NO_WORD &&
			    byte != ' ' && byte != '\t')
				parser->first_word = parser->line_length;
			parser->line[parser->line_length++] = byte;
			backslash_run = byte == '\\' ? backslash_run + 1 : 0;
			continue;
		}
		/* Each physical line is one unit of work, which also polls
		 * cancellation: lines that only continue grow no logical line,
		 * so the line bound alone would not stop a stream of them.
		 */
		parser->next_line_number++;
		status = fsimg_session_charge(parser->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (!line_is_comment(parser) && backslash_run % 2 == 1) {
			parser->line_length--;
			if (parser->first_word == parser->line_length)
				parser->first_word = MTREE_NO_WORD;
			backslash_run = 0;
			continue;
		}
		if (parser->line_length > parser->line_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    parser->line_limit, parser->line_length));
		*found = true;
		return (FSIMG_OK);
	}
}

/* Whether the line so far is a comment: its first kept byte that is not a
 * blank is '#'. A blank line has no such byte and is not a comment.
 */
static bool
line_is_comment(const mtree_parser_t *parser)
{
	return (parser->first_word != MTREE_NO_WORD &&
	    parser->line[parser->first_word] == '#');
}

/* Parse one logical line by its first word: blank and comment lines are
 * skipped, and the rest are /set, /unset, ".." or an entry.
 */
static fsimg_status_t
parse_line(mtree_parser_t *parser, fsimg_error_t *error)
{
	const mtree_word_t *first;
	fsimg_status_t status;

	if (parser->first_word == MTREE_NO_WORD || line_is_comment(parser))
		return (FSIMG_OK);

	status = split_words(parser, parser->first_word, error);
	if (status != FSIMG_OK)
		return (status);
	first = &parser->words[0];
	if (word_is(first, "/set"))
		return (apply_set(parser, error));
	if (word_is(first, "/unset"))
		return (apply_unset(parser, error));
	if (first->bytes[0] == '/')
		return (refuse(error, FSIMG_EINVAL,
		    "unknown special line: only /set and /unset begin with '/'"));
	if (word_is(first, ".."))
		return (leave_directory(parser, error));
	return (parse_entry(parser, error));
}

/* Split a line that is neither blank nor a comment into words. Every byte
 * of a word is printable ASCII other than '#'.
 */
static fsimg_status_t
split_words(mtree_parser_t *parser, size_t start, fsimg_error_t *error)
{
	const unsigned char *line;
	size_t cursor, word_start;

	line = parser->line;
	parser->word_count = 0;
	cursor = start;
	for (;;) {
		while (cursor < parser->line_length &&
		    (line[cursor] == ' ' || line[cursor] == '\t'))
			cursor++;
		if (cursor == parser->line_length)
			return (FSIMG_OK);
		word_start = cursor;
		while (cursor < parser->line_length && line[cursor] != ' ' &&
		    line[cursor] != '\t') {
			if (line[cursor] < 0x21 || line[cursor] > 0x7E)
				return (refuse(error, FSIMG_EINVAL,
				    "byte outside printable ASCII in a word"));
			if (line[cursor] == '#')
				return (refuse(error, FSIMG_EINVAL,
				    "'#' outside a comment"));
			cursor++;
		}
		if (parser->word_count == MTREE_WORDS_MAX)
			return (refuse(error, FSIMG_EINVAL,
			    "line has more than 64 words"));
		parser->words[parser->word_count].bytes = line + word_start;
		parser->words[parser->word_count].length = cursor - word_start;
		parser->word_count++;
	}
}

static bool
word_is(const mtree_word_t *word, const char *text)
{
	return (word->length == strlen(text) &&
	    bcmp(word->bytes, text, word->length) == 0);
}

static fsimg_status_t
apply_set(mtree_parser_t *parser, fsimg_error_t *error)
{
	uint32_t seen;
	fsimg_status_t status;

	seen = 0;
	for (size_t i = 1; i < parser->word_count; i++) {
		status = parse_keyword(parser, &parser->defaults,
		    &parser->words[i], &seen, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* "/unset all" clears every default; "/unset name" clears one. The names
 * follow the keyword classes as on any line: an unknown name is invalid,
 * an unsupported one unsupported, and a repeat, "all" included, refused.
 */
static fsimg_status_t
apply_unset(mtree_parser_t *parser, fsimg_error_t *error)
{
	const mtree_word_t *word;
	mtree_keyword_t keyword;
	uint32_t seen;
	bool all_seen;

	seen = 0;
	all_seen = false;
	for (size_t i = 1; i < parser->word_count; i++) {
		word = &parser->words[i];
		if (word->length > MTREE_KEYWORD_NAME_MAX)
			return (refuse(error, FSIMG_EINVAL,
			    "keyword name exceeds 32 bytes"));
		keyword = lookup_keyword(word->bytes, word->length);
		if (keyword == KEYWORD_UNKNOWN && word_is(word, "all")) {
			if (all_seen)
				return (refuse(error, FSIMG_EINVAL,
				    "repeated keyword \"all\""));
			all_seen = true;
			parser->defaults.present = 0;
			continue;
		}
		if (keyword == KEYWORD_UNKNOWN)
			return (refuse_keyword(error, FSIMG_EINVAL,
			    "unknown keyword", (const char *)word->bytes,
			    word->length));
		if (keyword == KEYWORD_UNSUPPORTED)
			return (refuse_keyword(error, FSIMG_EUNSUPPORTED,
			    "unsupported keyword", (const char *)word->bytes,
			    word->length));
		if ((seen & KEYWORD_BIT(keyword)) != 0)
			return (refuse_keyword(error, FSIMG_EINVAL,
			    "repeated keyword", (const char *)word->bytes,
			    word->length));
		seen |= KEYWORD_BIT(keyword);
		parser->defaults.present &= ~KEYWORD_BIT(keyword);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
leave_directory(mtree_parser_t *parser, fsimg_error_t *error)
{
	char *separator;

	if (parser->word_count != 1)
		return (refuse(error, FSIMG_EINVAL, "keywords on \"..\""));
	if (parser->directory_length > 1) {
		separator = strrchr(parser->directory, '/');
		parser->directory_length = separator == parser->directory ? 1 :
		    (size_t)(separator - parser->directory);
		parser->directory[parser->directory_length] = '\0';
		return (FSIMG_OK);
	}
	if (!parser->root_open)
		return (refuse(error, FSIMG_EINVAL,
		    "\"..\" with no directory to leave"));
	parser->root_open = false;
	return (FSIMG_OK);
}

/* Parse one entry line: place its path, then read, combine and deliver
 * the entry. Whatever the outcome, the current directory is settled here
 * and only here: a relative directory entry that was delivered becomes
 * it, and every other outcome cuts off the name build_entry_path may have
 * appended. A failure names the entry's path, taken before that cut.
 */
static fsimg_status_t
parse_entry(mtree_parser_t *parser, fsimg_error_t *error)
{
	fsimg_mtree_entry_t entry;
	mtree_entry_form_t form;
	const char *path;
	size_t path_length;
	bool becomes_directory;
	fsimg_status_t status;

	form = ENTRY_FULL;
	path = NULL;
	path_length = 0;
	status = build_entry_path(parser, &form, &path, &path_length, error);
	if (status == FSIMG_OK)
		status = deliver_entry(parser, form, path, &entry, error);

	if (status != FSIMG_OK && path != NULL)
		fsimg_error_path(error, path);
	becomes_directory = status == FSIMG_OK && form == ENTRY_RELATIVE &&
	    entry.entry.kind == FSIMG_ENTRY_DIRECTORY;
	if (becomes_directory)
		parser->directory_length = path_length;
	parser->directory[parser->directory_length] = '\0';
	return (status);
}

/* Read the line's own keywords, combine them with the defaults into one
 * entry at path, and hand it to the entry callback. The root is implicit
 * and carries no entry, as a host root scanned at "/" does; its line only
 * opens it as the current directory.
 */
static fsimg_status_t
deliver_entry(mtree_parser_t *parser, mtree_entry_form_t form,
    const char *path, fsimg_mtree_entry_t *entry, fsimg_error_t *error)
{
	uint32_t seen;
	fsimg_status_t status;

	/* seen collects this line's keywords for the repeat check, as it does
	 * on a /set line, where present cannot serve because the defaults
	 * persist from line to line.
	 */
	parser->line_values.present = 0;
	seen = 0;
	for (size_t i = 1; i < parser->word_count; i++) {
		status = parse_keyword(parser, &parser->line_values,
		    &parser->words[i], &seen, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = resolve_entry(parser, path, entry, error);
	if (status != FSIMG_OK)
		return (status);

	if (form == ENTRY_ROOT) {
		if (entry->entry.kind != FSIMG_ENTRY_DIRECTORY)
			return (refuse(error, FSIMG_EINVAL,
			    "the image root \".\" must be type=dir"));
		parser->root_open = true;
		return (FSIMG_OK);
	}
	entry->line = parser->line_number;
	entry->line_offset = parser->line_offset;
	status = parser->input->entry(parser->input->entry_argument, entry,
	    error);
	return (callback_status(status, error));
}

/* Decode the first word and place the entry's absolute path in *path: a
 * full entry in full_path, "/" followed by the decoded bytes, less a
 * leading "./"; the "." entry at the current directory itself; any other
 * relative entry by appending "/name" to parser->directory in place, which
 * the caller must keep or cut. *path is set once the path is placed, so a
 * failure of the validation that follows can name it. That validation is
 * the one collection applies to every image path: UTF-8, no empty, "." or
 * ".." component, within the path and depth limits.
 *
 * Bounds: the decoded name is at most path_limit bytes, stored from
 * full_path + 1 with its NUL at most at full_path[path_limit + 1], inside
 * the path_limit + 2 bytes of full_path. Each placement then requires its
 * path length below path_limit, which leaves room for the NUL in
 * directory, whose buffer has the same size.
 */
static fsimg_status_t
build_entry_path(mtree_parser_t *parser, mtree_entry_form_t *form,
    const char **path_out, size_t *path_length, fsimg_error_t *error)
{
	const mtree_word_t *name_word;
	char *name, *path;
	size_t decoded_length, length, separator;
	uint32_t depth;
	fsimg_status_t status;

	name_word = &parser->words[0];
	name = parser->full_path + 1;
	status = decode_word(name_word->bytes, name_word->length, name,
	    parser->path_limit, &decoded_length, error);
	if (status != FSIMG_OK)
		return (status);
	/* Every placement keeps at least decoded_length - 1 bytes, so a
	 * longer name is too long wherever it goes.
	 */
	if (decoded_length > parser->path_limit)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    parser->path_limit, decoded_length));
	name[decoded_length] = '\0';

	if (memchr(name, '/', decoded_length) != NULL) {
		*form = ENTRY_FULL;
		parser->full_path[0] = '/';
		path = parser->full_path;
		length = decoded_length + 1;
		if (decoded_length >= 2 && name[0] == '.' && name[1] == '/') {
			path = name + 1;
			length = decoded_length - 1;
		}
		if (length >= parser->path_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    parser->path_limit, (uint64_t)length + 1));
	} else if (decoded_length == 1 && name[0] == '.') {
		/* The current directory itself, which already fits. */
		*form = parser->directory_length == 1 ? ENTRY_ROOT :
		    ENTRY_RELATIVE;
		path = parser->directory;
		length = parser->directory_length;
	} else {
		*form = ENTRY_RELATIVE;
		separator = parser->directory_length == 1 ? 0 : 1;
		length = parser->directory_length + separator + decoded_length;
		if (length >= parser->path_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    parser->path_limit, (uint64_t)length + 1));
		path = parser->directory;
		if (separator != 0)
			path[parser->directory_length] = '/';
		bcopy(name, path + parser->directory_length + separator,
		    decoded_length + 1);
	}

	*path_out = path;
	*path_length = length;
	return (fsimg_path_validate(parser->session->ctx, path,
	    *form == ENTRY_ROOT, &length, &depth, error));
}

/* Decode a vis(3) word: "\ooo" with exactly three octal digits is that
 * byte and "\\" a backslash; any other backslash, a value above 0377 and
 * a decoded NUL refuse it. Bytes beyond capacity are counted in
 * *decoded_length but not stored, so the caller can name the length.
 */
static fsimg_status_t
decode_word(const unsigned char *encoded, size_t encoded_length, char *out,
    size_t capacity, size_t *decoded_length, fsimg_error_t *error)
{
	unsigned value;
	size_t count, cursor;
	unsigned char byte;

	count = 0;
	cursor = 0;
	while (cursor < encoded_length) {
		byte = encoded[cursor];
		if (byte != '\\') {
			cursor++;
		} else if (encoded_length - cursor >= 2 &&
		    encoded[cursor + 1] == '\\') {
			cursor += 2;
		} else {
			if (encoded_length - cursor < MTREE_ESCAPE_LENGTH)
				return (refuse(error, FSIMG_EINVAL,
				    "escape is not \\ooo or \\\\"));
			value = 0;
			for (size_t i = 1; i < MTREE_ESCAPE_LENGTH; i++) {
				if (encoded[cursor + i] < '0' ||
				    encoded[cursor + i] > '7')
					return (refuse(error, FSIMG_EINVAL,
					    "escape is not \\ooo or \\\\"));
				value = value * 8 + (encoded[cursor + i] - '0');
			}
			if (value > 0377)
				return (refuse(error, FSIMG_EINVAL,
				    "escape value exceeds one byte"));
			if (value == 0)
				return (refuse(error, FSIMG_EINVAL,
				    "escape decodes to a NUL byte"));
			byte = (unsigned char)value;
			cursor += MTREE_ESCAPE_LENGTH;
		}
		if (count < capacity)
			out[count] = (char)byte;
		count++;
	}
	*decoded_length = count;
	return (FSIMG_OK);
}

static mtree_keyword_t
lookup_keyword(const unsigned char *name, size_t length)
{
	const mtree_keyword_name_t *entry;

	for (size_t i = 0; i < sizeof(keyword_names) / sizeof(keyword_names[0]);
	    i++) {
		entry = &keyword_names[i];
		if (strlen(entry->name) == length &&
		    bcmp(entry->name, name, length) == 0)
			return (entry->keyword);
	}
	return (KEYWORD_UNKNOWN);
}

/* Parse one keyword=value word into values. seen collects the keywords of
 * this line so that a repeat, including an alias such as sha256 beside
 * sha256digest, is refused.
 */
static fsimg_status_t
parse_keyword(mtree_parser_t *parser, mtree_values_t *values,
    const mtree_word_t *word, uint32_t *seen, fsimg_error_t *error)
{
	const unsigned char *equals, *value;
	const char *name;
	mtree_keyword_t keyword;
	size_t name_length, value_length;
	fsimg_status_t status;

	name = (const char *)word->bytes;
	equals = memchr(word->bytes, '=', word->length);
	name_length = equals == NULL ? word->length :
	    (size_t)(equals - word->bytes);
	if (name_length > MTREE_KEYWORD_NAME_MAX)
		return (refuse(error, FSIMG_EINVAL,
		    "keyword name exceeds 32 bytes"));
	keyword = lookup_keyword(word->bytes, name_length);
	if (keyword == KEYWORD_UNKNOWN)
		return (refuse_keyword(error, FSIMG_EINVAL, "unknown keyword",
		    name, name_length));
	if (keyword == KEYWORD_UNSUPPORTED)
		return (refuse_keyword(error, FSIMG_EUNSUPPORTED,
		    "unsupported keyword", name, name_length));
	if (equals == NULL)
		return (refuse_keyword(error, FSIMG_EINVAL,
		    "keyword without a value", name, name_length));
	if ((*seen & KEYWORD_BIT(keyword)) != 0)
		return (refuse_keyword(error, FSIMG_EINVAL, "repeated keyword",
		    name, name_length));
	*seen |= KEYWORD_BIT(keyword);

	value = equals + 1;
	value_length = word->length - name_length - 1;
	if (value_length == 0)
		return (refuse_keyword(error, FSIMG_EINVAL,
		    "empty value for keyword", name, name_length));
	/* Only names are vis-encoded; every other value is plain ASCII. */
	if (keyword != KEYWORD_LINK && keyword != KEYWORD_CONTENTS &&
	    memchr(value, '\\', value_length) != NULL)
		return (refuse_keyword(error, FSIMG_EINVAL,
		    "backslash in the value of keyword", name, name_length));
	status = parse_value(parser, values, keyword, value, value_length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	values->present |= KEYWORD_BIT(keyword);
	return (FSIMG_OK);
}

static fsimg_status_t
parse_value(mtree_parser_t *parser, mtree_values_t *values,
    mtree_keyword_t keyword, const unsigned char *value, size_t length,
    fsimg_error_t *error)
{
	mtree_word_t text = { .bytes = value, .length = length };
	uint64_t number;
	size_t decoded_length;
	fsimg_status_t status;

	switch (keyword) {
	case KEYWORD_TYPE:
		if (word_is(&text, "dir"))
			values->kind = FSIMG_ENTRY_DIRECTORY;
		else if (word_is(&text, "file"))
			values->kind = FSIMG_ENTRY_FILE;
		else if (word_is(&text, "link"))
			values->kind = FSIMG_ENTRY_SYMLINK;
		else if (word_is(&text, "fifo"))
			values->kind = FSIMG_ENTRY_FIFO;
		else if (word_is(&text, "block") || word_is(&text, "char") ||
		    word_is(&text, "socket"))
			return (refuse(error, FSIMG_EUNSUPPORTED,
			    "unsupported keyword \"type\" value: block, char "
			    "and socket entries are not stored"));
		else
			return (refuse(error, FSIMG_EINVAL, "unknown type"));
		return (FSIMG_OK);
	case KEYWORD_UID:
	case KEYWORD_GID:
		status = parse_decimal(value, length, UINT32_MAX, &number,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (keyword == KEYWORD_UID)
			values->uid = (uint32_t)number;
		else
			values->gid = (uint32_t)number;
		return (FSIMG_OK);
	case KEYWORD_MODE:
		return (parse_mode(value, length, &values->mode, error));
	case KEYWORD_TIME:
		return (parse_time(value, length, values, error));
	case KEYWORD_SIZE:
		return (parse_decimal(value, length, UINT64_MAX, &values->size,
		    error));
	case KEYWORD_LINK:
		status = decode_word(value, length, values->link,
		    FSIMG_SYMLINK_TARGET_MAX, &decoded_length, error);
		if (status != FSIMG_OK)
			return (status);
		if (decoded_length > FSIMG_SYMLINK_TARGET_MAX)
			return (refuse(error, FSIMG_EINVAL,
			    "link target exceeds 1024 bytes"));
		values->link[decoded_length] = '\0';
		values->link_length = decoded_length;
		return (FSIMG_OK);
	case KEYWORD_CONTENTS:
		status = decode_word(value, length, values->contents,
		    parser->path_limit - 1, &decoded_length, error);
		if (status != FSIMG_OK)
			return (status);
		if (decoded_length >= parser->path_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    parser->path_limit, (uint64_t)decoded_length + 1));
		values->contents[decoded_length] = '\0';
		values->contents_length = decoded_length;
		if (!contents_path_valid(values->contents, decoded_length))
			return (refuse(error, FSIMG_EINVAL,
			    "contents path is absolute or has an empty, . or .. component"));
		return (FSIMG_OK);
	case KEYWORD_SHA256:
		return (parse_digest(value, length, values->digest, error));
	case KEYWORD_CONTENT:
		if (word_is(&text, "host"))
			values->content = FSIMG_MTREE_CONTENT_HOST;
		else if (word_is(&text, "zero"))
			values->content = FSIMG_MTREE_CONTENT_ZERO;
		else if (word_is(&text, "seeded"))
			values->content = FSIMG_MTREE_CONTENT_SEEDED;
		else if (word_is(&text, "positional"))
			values->content = FSIMG_MTREE_CONTENT_POSITIONAL;
		else
			return (refuse(error, FSIMG_EINVAL,
			    "unknown fsimg.content"));
		return (FSIMG_OK);
	case KEYWORD_SEED:
		return (parse_decimal(value, length, UINT64_MAX, &values->seed,
		    error));
	case KEYWORD_HOLES:
		return (parse_holes(value, length, values, error));
	case KEYWORD_NLINK:
	case KEYWORD_INODE:
		return (parse_decimal(value, length, UINT64_MAX, &number,
		    error));
	case KEYWORD_DEVICE:
	case KEYWORD_RESDEVICE:
		return (parse_device(value, length, error));
	case KEYWORD_TAGS:
	case KEYWORD_UNAME:
	case KEYWORD_GNAME:
		/* Any nonempty plain value, which the caller checked. */
		return (FSIMG_OK);
	case KEYWORD_FLAGS:
		if (!word_is(&text, "none"))
			return (refuse(error, FSIMG_EUNSUPPORTED,
			    "unsupported keyword \"flags\" other than none"));
		return (FSIMG_OK);
	case KEYWORD_UNSUPPORTED:
	case KEYWORD_UNKNOWN:
		break;
	}
	return (refuse(error, FSIMG_EINVAL, "keyword has no value parser"));
}

/* A decimal number of at most 20 digits, accumulated with checked
 * arithmetic and at most maximum. Digits are checked before the count, so
 * a non-digit is malformed whatever the length.
 */
static fsimg_status_t
parse_decimal(const unsigned char *digits, size_t length, uint64_t maximum,
    uint64_t *out, fsimg_error_t *error)
{
	uint64_t value;

	if (length == 0)
		return (refuse(error, FSIMG_EINVAL, "missing decimal number"));
	for (size_t i = 0; i < length; i++) {
		if (digits[i] < '0' || digits[i] > '9')
			return (refuse(error, FSIMG_EINVAL,
			    "malformed decimal number"));
	}
	if (length > MTREE_DECIMAL_DIGITS_MAX)
		return (refuse(error, FSIMG_ERANGE,
		    "decimal number has more than 20 digits"));
	value = 0;
	for (size_t i = 0; i < length; i++) {
		if (!fsimg_mul_u64(value, 10, &value) ||
		    !fsimg_add_u64(value, (uint64_t)(digits[i] - '0'), &value))
			return (refuse(error, FSIMG_ERANGE,
			    "decimal number out of range"));
	}
	if (value > maximum)
		return (refuse(error, FSIMG_ERANGE,
		    "decimal number out of range"));
	*out = value;
	return (FSIMG_OK);
}

/* An octal mode of at most 22 digits and 07777. A value that is not a
 * number but reads as a symbolic mode, such as u+rwx, is unsupported.
 */
static fsimg_status_t
parse_mode(const unsigned char *digits, size_t length, uint32_t *mode,
    fsimg_error_t *error)
{
	uint64_t value;

	if (digits[0] < '0' || digits[0] > '9') {
		for (size_t i = 0; i < length; i++) {
			if (strchr("ugoa+-=rwxXst,", digits[i]) == NULL)
				return (refuse(error, FSIMG_EINVAL,
				    "malformed mode"));
		}
		return (refuse(error, FSIMG_EUNSUPPORTED,
		    "unsupported symbolic mode"));
	}
	for (size_t i = 0; i < length; i++) {
		if (digits[i] < '0' || digits[i] > '7')
			return (refuse(error, FSIMG_EINVAL,
			    "mode is not octal"));
	}
	if (length > MTREE_OCTAL_DIGITS_MAX)
		return (refuse(error, FSIMG_ERANGE,
		    "mode has more than 22 digits"));
	value = 0;
	for (size_t i = 0; i < length; i++) {
		if (!fsimg_mul_u64(value, 8, &value) ||
		    !fsimg_add_u64(value, (uint64_t)(digits[i] - '0'), &value))
			return (refuse(error, FSIMG_ERANGE,
			    "mode out of range"));
	}
	if (value > FSIMG_MODE_BITS)
		return (refuse(error, FSIMG_ERANGE, "mode exceeds 07777"));
	*mode = (uint32_t)value;
	return (FSIMG_OK);
}

/* seconds[.fraction]: seconds a signed decimal within int64_t, the
 * fraction one to nine digits, right-padded to nanoseconds. As in the
 * struct timespec mtree(8) prints, the fraction counts forward from the
 * seconds even when they are negative: "-1.5" is -1 s plus 0.5 s.
 */
static fsimg_status_t
parse_time(const unsigned char *text, size_t length, mtree_values_t *values,
    fsimg_error_t *error)
{
	const unsigned char *dot;
	uint64_t magnitude, maximum;
	size_t fraction_length, seconds_length, start;
	uint32_t nanoseconds;
	bool negative;
	fsimg_status_t status;

	negative = text[0] == '-';
	start = negative ? 1 : 0;
	dot = memchr(text, '.', length);
	seconds_length = dot == NULL ? length - start :
	    (size_t)(dot - text) - start;
	maximum = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	status = parse_decimal(text + start, seconds_length, maximum,
	    &magnitude, error);
	if (status != FSIMG_OK)
		return (status);

	nanoseconds = 0;
	if (dot != NULL) {
		fraction_length = length - (size_t)(dot - text) - 1;
		if (fraction_length == 0 ||
		    fraction_length > MTREE_FRACTION_DIGITS_MAX)
			return (refuse(error, FSIMG_EINVAL,
			    "time fraction is not one to nine digits"));
		for (size_t i = 0; i < MTREE_FRACTION_DIGITS_MAX; i++) {
			nanoseconds *= 10;
			if (i >= fraction_length)
				continue;
			if (dot[1 + i] < '0' || dot[1 + i] > '9')
				return (refuse(error, FSIMG_EINVAL,
				    "malformed time fraction"));
			nanoseconds += (uint32_t)(dot[1 + i] - '0');
		}
	}
	if (!negative)
		values->time_seconds = (int64_t)magnitude;
	else if (magnitude == (uint64_t)INT64_MAX + 1)
		values->time_seconds = INT64_MIN;
	else
		values->time_seconds = -(int64_t)magnitude;
	values->time_nanoseconds = nanoseconds;
	return (FSIMG_OK);
}

static fsimg_status_t
parse_digest(const unsigned char *text, size_t length, unsigned char *digest,
    fsimg_error_t *error)
{
	unsigned nibble;
	unsigned char byte;

	if (length != 2 * FSIMG_SHA256_DIGEST_LENGTH)
		return (refuse(error, FSIMG_EINVAL,
		    "sha256digest is not 64 hexadecimal digits"));
	for (size_t i = 0; i < length; i++) {
		byte = text[i];
		if (byte >= '0' && byte <= '9')
			nibble = byte - '0';
		else if (byte >= 'a' && byte <= 'f')
			nibble = byte - 'a' + 10;
		else if (byte >= 'A' && byte <= 'F')
			nibble = byte - 'A' + 10;
		else
			return (refuse(error, FSIMG_EINVAL,
			    "sha256digest is not 64 hexadecimal digits"));
		if (i % 2 == 0)
			digest[i / 2] = (unsigned char)(nibble << 4);
		else
			digest[i / 2] |= (unsigned char)nibble;
	}
	return (FSIMG_OK);
}

/* offset:length[,offset:length]...: at most 64 runs, each of positive
 * length, in increasing order, neither touching nor overlapping the one
 * before. Whether the last ends within the entry's size waits for the
 * entry, whose size may be a default.
 */
static fsimg_status_t
parse_holes(const unsigned char *text, size_t length, mtree_values_t *values,
    fsimg_error_t *error)
{
	fsimg_mtree_hole_t *hole;
	const unsigned char *colon, *comma;
	uint64_t end, previous_end;
	size_t cursor, segment_length;
	fsimg_status_t status;

	values->hole_count = 0;
	previous_end = 0;
	cursor = 0;
	for (;;) {
		comma = memchr(text + cursor, ',', length - cursor);
		segment_length = comma == NULL ? length - cursor :
		    (size_t)(comma - (text + cursor));
		colon = memchr(text + cursor, ':', segment_length);
		if (colon == NULL)
			return (refuse(error, FSIMG_EINVAL,
			    "hole is not offset:length"));
		if (values->hole_count == FSIMG_MTREE_HOLES_MAX)
			return (refuse(error, FSIMG_EINVAL,
			    "more than 64 holes"));
		hole = &values->holes[values->hole_count];
		status = parse_decimal(text + cursor,
		    (size_t)(colon - (text + cursor)), UINT64_MAX,
		    &hole->offset, error);
		if (status != FSIMG_OK)
			return (status);
		status = parse_decimal(colon + 1,
		    segment_length - (size_t)(colon - (text + cursor)) - 1,
		    UINT64_MAX, &hole->length, error);
		if (status != FSIMG_OK)
			return (status);
		if (hole->length == 0)
			return (refuse(error, FSIMG_EINVAL,
			    "hole has zero length"));
		if (!fsimg_add_u64(hole->offset, hole->length, &end))
			return (refuse(error, FSIMG_ERANGE,
			    "hole ends beyond the 64-bit range"));
		if (values->hole_count > 0 && hole->offset <= previous_end)
			return (refuse(error, FSIMG_EINVAL,
			    "holes out of order, touching or overlapping"));
		previous_end = end;
		values->hole_count++;
		if (comma == NULL)
			return (FSIMG_OK);
		cursor += segment_length + 1;
	}
}

/* A device number as mtree(8) and bsdtar write it, either a number or
 * format,major,minor[,subunit]: one to four comma-separated fields of
 * letters and digits. It describes the host object and is not consulted.
 */
static fsimg_status_t
parse_device(const unsigned char *text, size_t length, fsimg_error_t *error)
{
	size_t field_length, fields;
	unsigned char byte;

	fields = 1;
	field_length = 0;
	for (size_t i = 0; i < length; i++) {
		byte = text[i];
		if (byte == ',') {
			if (field_length == 0 || ++fields > 4)
				return (refuse(error, FSIMG_EINVAL,
				    "malformed device"));
			field_length = 0;
			continue;
		}
		if (!((byte >= '0' && byte <= '9') ||
		    (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')))
			return (refuse(error, FSIMG_EINVAL, "malformed device"));
		field_length++;
	}
	if (field_length == 0)
		return (refuse(error, FSIMG_EINVAL, "malformed device"));
	return (FSIMG_OK);
}

/* A contents= path names a file beneath the content directory: relative,
 * with no empty, "." or ".." component. Decoding already refused a NUL.
 */
static bool
contents_path_valid(const char *path, size_t length)
{
	size_t component, start;

	start = 0;
	for (size_t i = 0; i <= length; i++) {
		if (i != length && path[i] != '/')
			continue;
		component = i - start;
		if (component == 0 ||
		    (component == 1 && path[start] == '.') ||
		    (component == 2 && path[start] == '.' &&
		    path[start + 1] == '.'))
			return (false);
		start = i + 1;
	}
	return (true);
}

/* The keywords that do not apply to an entry of this kind and content: a
 * link target on anything but a link, contents= on anything but a host
 * file, a digest or fsimg.content on anything but a file, and a seed or
 * holes on anything but seeded or positional content.
 */
static uint32_t
inapplicable_keywords(fsimg_entry_kind_t kind, fsimg_mtree_content_t content)
{
	uint32_t keywords;

	keywords = 0;
	if (kind != FSIMG_ENTRY_SYMLINK)
		keywords |= KEYWORD_BIT(KEYWORD_LINK);
	if (kind != FSIMG_ENTRY_FILE)
		keywords |= KEYWORD_BIT(KEYWORD_SHA256) |
		    KEYWORD_BIT(KEYWORD_CONTENT);
	if (kind != FSIMG_ENTRY_FILE || content != FSIMG_MTREE_CONTENT_HOST)
		keywords |= KEYWORD_BIT(KEYWORD_CONTENTS);
	if (kind != FSIMG_ENTRY_FILE ||
	    (content != FSIMG_MTREE_CONTENT_SEEDED &&
	    content != FSIMG_MTREE_CONTENT_POSITIONAL))
		keywords |= KEYWORD_BIT(KEYWORD_SEED) |
		    KEYWORD_BIT(KEYWORD_HOLES);
	return (keywords);
}

/* The first keyword in a nonempty mask of present keywords. Only keywords
 * with a value can be present, since parse_keyword refuses the rest before
 * setting a bit, so the search stays within keyword_spellings.
 */
static mtree_keyword_t
lowest_keyword(uint32_t keywords)
{
	mtree_keyword_t keyword;

	for (keyword = KEYWORD_TYPE; keyword < KEYWORD_UNSUPPORTED; keyword++) {
		if ((keywords & KEYWORD_BIT(keyword)) != 0)
			break;
	}
	return (keyword < KEYWORD_UNSUPPORTED ? keyword : KEYWORD_TYPE);
}

/* The values that give keyword to the current entry: the line's own, else
 * the defaults' unless the keyword does not apply, else none. The line's
 * inapplicable keywords were refused before any lookup.
 */
static const mtree_values_t *
find_keyword(const mtree_parser_t *parser, mtree_keyword_t keyword,
    uint32_t inapplicable)
{
	uint32_t bit;

	bit = KEYWORD_BIT(keyword);
	if ((parser->line_values.present & bit) != 0)
		return (&parser->line_values);
	if ((parser->defaults.present & bit & ~inapplicable) != 0)
		return (&parser->defaults);
	return (NULL);
}

/* Combine the line's keywords with the defaults into one entry. The kind
 * and, for a file, the content decide which keywords apply; the line may
 * not give one that does not, and the defaults' are passed over. Then the
 * entry's bytes and its metadata are filled in.
 */
static fsimg_status_t
resolve_entry(const mtree_parser_t *parser, const char *path,
    fsimg_mtree_entry_t *out, fsimg_error_t *error)
{
	const mtree_values_t *content, *type;
	const char *spelling;
	uint32_t inapplicable, refused;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	out->entry.path = path;
	type = find_keyword(parser, KEYWORD_TYPE, 0);
	if (type == NULL)
		return (refuse(error, FSIMG_EINVAL, "entry has no type"));
	out->entry.kind = type->kind;
	out->content = FSIMG_MTREE_CONTENT_HOST;
	if (out->entry.kind == FSIMG_ENTRY_FILE) {
		content = find_keyword(parser, KEYWORD_CONTENT, 0);
		if (content != NULL)
			out->content = content->content;
	}

	inapplicable = inapplicable_keywords(out->entry.kind, out->content);
	refused = parser->line_values.present & inapplicable;
	if (refused != 0) {
		spelling = keyword_spellings[lowest_keyword(refused)];
		return (refuse_keyword(error, FSIMG_EINVAL,
		    "keyword does not apply to this entry", spelling,
		    strlen(spelling)));
	}

	status = resolve_contents(parser, inapplicable, out, error);
	if (status != FSIMG_OK)
		return (status);
	return (resolve_metadata(parser, out, error));
}

/* What the entry's bytes are: a link's target, or a file's size, digest
 * and either its host contents path or its generator's seed and holes.
 * Generated content has no host file to supply a length, so it needs size.
 */
static fsimg_status_t
resolve_contents(const mtree_parser_t *parser, uint32_t inapplicable,
    fsimg_mtree_entry_t *out, fsimg_error_t *error)
{
	const mtree_values_t *contents, *digest, *holes, *link, *seed, *size;
	const fsimg_mtree_hole_t *last;

	link = find_keyword(parser, KEYWORD_LINK, inapplicable);
	size = find_keyword(parser, KEYWORD_SIZE, inapplicable);
	if (out->entry.kind == FSIMG_ENTRY_SYMLINK) {
		if (link == NULL)
			return (refuse(error, FSIMG_EINVAL,
			    "type=link without link"));
		/* A size default describes files; a link's own size, as
		 * mtree -c writes it, must be its target's length.
		 */
		if ((parser->line_values.present &
		    KEYWORD_BIT(KEYWORD_SIZE)) != 0 &&
		    parser->line_values.size != link->link_length)
			return (refuse(error, FSIMG_EINVAL,
			    "size differs from the link target's length"));
		out->link = link->link;
		out->link_length = link->link_length;
		out->entry.size = link->link_length;
		return (FSIMG_OK);
	}
	/* A directory's or FIFO's size describes the host object only. */
	if (out->entry.kind != FSIMG_ENTRY_FILE)
		return (FSIMG_OK);

	if (size != NULL) {
		out->entry.size = size->size;
		out->size_given = true;
	} else if (out->content != FSIMG_MTREE_CONTENT_HOST) {
		return (refuse(error, FSIMG_EINVAL,
		    "generated content without size"));
	}
	digest = find_keyword(parser, KEYWORD_SHA256, inapplicable);
	if (digest != NULL) {
		bcopy(digest->digest, out->digest, sizeof(out->digest));
		out->digest_given = true;
	}
	contents = find_keyword(parser, KEYWORD_CONTENTS, inapplicable);
	if (contents != NULL) {
		out->contents = contents->contents;
		out->contents_length = contents->contents_length;
	}

	/* Only seeded and positional content takes a seed and holes. */
	if (out->content != FSIMG_MTREE_CONTENT_SEEDED &&
	    out->content != FSIMG_MTREE_CONTENT_POSITIONAL)
		return (FSIMG_OK);
	seed = find_keyword(parser, KEYWORD_SEED, inapplicable);
	if (seed == NULL)
		return (refuse(error, FSIMG_EINVAL,
		    "seeded or positional content without fsimg.seed"));
	out->seed = seed->seed;
	holes = find_keyword(parser, KEYWORD_HOLES, inapplicable);
	if (holes != NULL) {
		/* The runs are ordered, so the last ends furthest, and its
		 * end was computed without overflow when it was parsed.
		 */
		last = &holes->holes[holes->hole_count - 1];
		if (last->offset + last->length > out->entry.size)
			return (refuse(error, FSIMG_EINVAL,
			    "hole ends past size"));
		out->holes = holes->holes;
		out->hole_count = holes->hole_count;
	}
	return (FSIMG_OK);
}

/* Owner, mode and modification time. uid and gid are one catalog field,
 * so they come together; a user or group name is accepted only beside the
 * number, since fsimg consults no user database.
 */
static fsimg_status_t
resolve_metadata(const mtree_parser_t *parser, fsimg_mtree_entry_t *out,
    fsimg_error_t *error)
{
	const mtree_values_t *gid, *mode, *time, *uid;
	fsimg_metadata_t *metadata;

	uid = find_keyword(parser, KEYWORD_UID, 0);
	gid = find_keyword(parser, KEYWORD_GID, 0);
	if (find_keyword(parser, KEYWORD_UNAME, 0) != NULL && uid == NULL)
		return (refuse(error, FSIMG_EUNSUPPORTED,
		    "unsupported keyword \"uname\" without a numeric uid"));
	if (find_keyword(parser, KEYWORD_GNAME, 0) != NULL && gid == NULL)
		return (refuse(error, FSIMG_EUNSUPPORTED,
		    "unsupported keyword \"gname\" without a numeric gid"));
	if ((uid == NULL) != (gid == NULL))
		return (refuse(error, FSIMG_EINVAL,
		    "uid and gid must be given together"));

	metadata = &out->entry.metadata;
	if (uid != NULL) {
		metadata->valid |= FSIMG_META_OWNER;
		metadata->uid = uid->uid;
		metadata->gid = gid->gid;
	}
	mode = find_keyword(parser, KEYWORD_MODE, 0);
	if (mode != NULL) {
		metadata->valid |= FSIMG_META_MODE;
		metadata->mode = mode->mode;
	}
	time = find_keyword(parser, KEYWORD_TIME, 0);
	if (time != NULL) {
		metadata->valid |= FSIMG_META_MODIFIED;
		metadata->modified.seconds = time->time_seconds;
		metadata->modified.nanoseconds = time->time_nanoseconds;
		metadata->modified.precision_ns = 1;
		metadata->modified.basis = FSIMG_TIME_UTC;
	}
	return (FSIMG_OK);
}

