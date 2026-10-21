/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "io.h"
#include "layer.h"

/*
 * A streaming parser for the XML property-list subset a DMG's resource fork
 * uses. The plist is untrusted and as long as the file allows, so nothing
 * of it is held whole: bytes arrive through one window buffer, an element's
 * text through one 4 KiB buffer, and base64 data is decoded in pieces
 * straight to the visitor. The open elements form an explicit stack of at
 * most 32 entries, so the parse is iterative and bounded.
 *
 * The document is a prolog of whitespace, the XML declaration, a DOCTYPE
 * without an internal subset, and comments; one plist element; then only
 * whitespace and comments. Inside, markup is a start tag (attributes
 * skipped, quotes respected), an end tag matching the innermost open
 * element, or a comment. Text is allowed only in key, string, integer and
 * data elements; elsewhere only whitespace may separate elements.
 */
#define WINDOW_BYTES		(16u * 1024)
#define DATA_PIECE_BYTES	3072u
#define NAME_BYTES		16u

/* The elements of the subset, indexed by kind. */
static const char *const element_names[] = {
	[FSIMG_PLIST_PLIST] = "plist", [FSIMG_PLIST_DICT] = "dict",
	[FSIMG_PLIST_ARRAY] = "array", [FSIMG_PLIST_KEY] = "key",
	[FSIMG_PLIST_STRING] = "string", [FSIMG_PLIST_DATA] = "data",
	[FSIMG_PLIST_INTEGER] = "integer", [FSIMG_PLIST_TRUE] = "true",
	[FSIMG_PLIST_FALSE] = "false"
};

#define ELEMENT_COUNT	(sizeof(element_names) / sizeof(element_names[0]))

/* One open element: its kind and how many children it has had, which is
 * what the content rules of plist and dict need.
 */
typedef struct open_element {
	fsimg_plist_element_t kind;
	uint32_t	 children;
} open_element_t;

typedef struct plist_parser {
	fsimg_io_t	*file;
	fsimg_session_t	*session;
	const fsimg_plist_visitor_t *visitor;
	unsigned char	*window;
	char		*text;		/* FSIMG_PLIST_MAX_TEXT bytes */
	uint64_t	 next_offset;	/* File offset the next refill reads */
	uint64_t	 end;		/* End of the plist range */
	size_t		 filled;
	size_t		 position;
	size_t		 text_length;
	/* base64 state: the current quad's values, how many padding
	 * characters ended the data, and decoded bytes awaiting delivery.
	 */
	unsigned char	 quad[4];
	unsigned	 quad_length;
	unsigned	 padding;
	unsigned char	 piece[DATA_PIECE_BYTES];
	size_t		 piece_length;
	open_element_t	 stack[FSIMG_PLIST_MAX_DEPTH];
	unsigned	 depth;
	bool		 root_seen;
} plist_parser_t;

static fsimg_status_t plist_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t plist_unsupported(fsimg_error_t *, const char *);
static fsimg_status_t next_byte(plist_parser_t *, int *, fsimg_error_t *);
static bool is_space(int);
static bool takes_text(fsimg_plist_element_t);
static bool is_value(fsimg_plist_element_t);
static fsimg_status_t skip_until(plist_parser_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t skip_comment(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t skip_declaration(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t read_name(plist_parser_t *, int, char *, int *,
    fsimg_error_t *);
static fsimg_status_t skip_attributes(plist_parser_t *, int, bool *,
    fsimg_error_t *);
static fsimg_status_t open_element(plist_parser_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t close_element(plist_parser_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t start_tag(plist_parser_t *, int, fsimg_error_t *);
static fsimg_status_t end_tag(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t markup(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t text_byte(plist_parser_t *, int, fsimg_error_t *);
static fsimg_status_t text_append(plist_parser_t *, char, fsimg_error_t *);
static fsimg_status_t entity(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t data_byte(plist_parser_t *, int, fsimg_error_t *);
static fsimg_status_t data_flush(plist_parser_t *, fsimg_error_t *);
static fsimg_status_t parse(plist_parser_t *, fsimg_error_t *);

static fsimg_status_t
plist_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "DMG plist %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

static fsimg_status_t
plist_unsupported(fsimg_error_t *error, const char *construct)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "DMG plist uses %s, which is not "
	    "supported", construct);
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
	    message));
}

/* The next byte of the range in *c, or -1 at its end. */
static fsimg_status_t
next_byte(plist_parser_t *parser, int *c, fsimg_error_t *error)
{
	size_t want, transferred;
	fsimg_status_t status;

	if (parser->position == parser->filled) {
		if (parser->next_offset == parser->end) {
			*c = -1;
			return (FSIMG_OK);
		}
		want = parser->end - parser->next_offset < WINDOW_BYTES ?
		    (size_t)(parser->end - parser->next_offset) : WINDOW_BYTES;
		status = fsimg_io_read_session(parser->file, parser->session,
		    parser->next_offset, parser->window, want, &transferred,
		    error);
		if (status != FSIMG_OK)
			return (status);
		parser->next_offset += want;
		parser->filled = want;
		parser->position = 0;
	}
	*c = parser->window[parser->position++];
	return (FSIMG_OK);
}

static bool
is_space(int c)
{
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static bool
takes_text(fsimg_plist_element_t kind)
{
	return (kind == FSIMG_PLIST_KEY || kind == FSIMG_PLIST_STRING ||
	    kind == FSIMG_PLIST_INTEGER || kind == FSIMG_PLIST_DATA);
}

static bool
is_value(fsimg_plist_element_t kind)
{
	return (kind != FSIMG_PLIST_PLIST && kind != FSIMG_PLIST_KEY);
}

/* Skip to just past the first occurrence of a two-byte terminator. */
static fsimg_status_t
skip_until(plist_parser_t *parser, const char *terminator,
    fsimg_error_t *error)
{
	int c, previous;
	fsimg_status_t status;

	previous = -1;
	for (;;) {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
		if (c < 0)
			return (plist_corrupt(error, "ends inside markup"));
		if (previous == terminator[0] && c == terminator[1])
			return (FSIMG_OK);
		previous = c;
	}
}

/* After "<!--": XML forbids "--" inside a comment except before its '>'. */
static fsimg_status_t
skip_comment(plist_parser_t *parser, fsimg_error_t *error)
{
	int c, previous;
	fsimg_status_t status;

	previous = -1;
	for (;;) {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
		if (c < 0)
			return (plist_corrupt(error, "ends inside a comment"));
		if (previous == '-' && c == '-')
			break;
		previous = c;
	}
	status = next_byte(parser, &c, error);
	if (status != FSIMG_OK)
		return (status);
	if (c != '>')
		return (plist_corrupt(error, "has \"--\" inside a comment"));
	return (FSIMG_OK);
}

/*
 * After "<!": a comment anywhere, or before the root a DOCTYPE, skipped
 * without interpretation. A DOCTYPE's internal subset could declare
 * entities, so it is refused rather than skipped; so is CDATA.
 */
static fsimg_status_t
skip_declaration(plist_parser_t *parser, fsimg_error_t *error)
{
	static const char doctype[] = "DOCTYPE";
	int c;
	fsimg_status_t status;

	status = next_byte(parser, &c, error);
	if (status != FSIMG_OK)
		return (status);
	if (c == '-') {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
		if (c != '-')
			return (plist_corrupt(error, "has a malformed comment"));
		return (skip_comment(parser, error));
	}
	if (c == '[')
		return (plist_unsupported(error, "a CDATA section"));
	for (size_t i = 0; i < sizeof(doctype) - 1; i++) {
		if (c != doctype[i])
			return (plist_corrupt(error, "has a malformed declaration"));
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (parser->root_seen)
		return (plist_corrupt(error, "has a DOCTYPE after its root"));
	while (c != '>') {
		if (c < 0)
			return (plist_corrupt(error, "ends inside markup"));
		if (c == '[')
			return (plist_unsupported(error, "a DOCTYPE internal subset"));
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* An element name starting with first, at most 16 bytes; *after receives
 * the byte that ended it.
 */
static fsimg_status_t
read_name(plist_parser_t *parser, int first, char *name, int *after,
    fsimg_error_t *error)
{
	size_t length;
	int c;
	fsimg_status_t status;

	length = 0;
	c = first;
	while ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '.' ||
	    c == '-') {
		if (length == NAME_BYTES)
			return (plist_unsupported(error,
			    "an element name longer than 16 bytes"));
		name[length++] = (char)c;
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (length == 0)
		return (plist_corrupt(error, "has a tag without a name"));
	name[length] = '\0';
	*after = c;
	return (FSIMG_OK);
}

/* Skip a start tag's attributes up to its '>', noting a "/>". */
static fsimg_status_t
skip_attributes(plist_parser_t *parser, int c, bool *empty,
    fsimg_error_t *error)
{
	int quote;
	fsimg_status_t status;

	*empty = false;
	for (;;) {
		if (c < 0)
			return (plist_corrupt(error, "ends inside markup"));
		if (c == '>')
			return (FSIMG_OK);
		if (c == '<')
			return (plist_corrupt(error, "has a malformed tag"));
		if (c == '/') {
			status = next_byte(parser, &c, error);
			if (status != FSIMG_OK)
				return (status);
			if (c != '>')
				return (plist_corrupt(error, "has a malformed tag"));
			*empty = true;
			return (FSIMG_OK);
		}
		if (c == '"' || c == '\'') {
			quote = c;
			do {
				status = next_byte(parser, &c, error);
				if (status != FSIMG_OK)
					return (status);
				if (c < 0)
					return (plist_corrupt(error,
					    "ends inside markup"));
			} while (c != quote);
		}
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/*
 * Open an element after checking that it may stand where it does: the
 * root is the one plist element, a plist holds one value, a dict
 * alternates keys and values, an array holds values, and elements holding
 * text hold no elements.
 */
static fsimg_status_t
open_element(plist_parser_t *parser, const char *name, fsimg_error_t *error)
{
	fsimg_plist_element_t kind;
	open_element_t *parent;
	size_t i;
	fsimg_status_t status;

	for (i = 0; i < ELEMENT_COUNT; i++) {
		if (strcmp(name, element_names[i]) == 0)
			break;
	}
	if (i == ELEMENT_COUNT)
		return (plist_unsupported(error, "an element outside the UDIF subset"));
	kind = (fsimg_plist_element_t)i;
	if (parser->depth == FSIMG_PLIST_MAX_DEPTH)
		return (plist_unsupported(error, "nesting deeper than 32 elements"));

	if (parser->depth == 0) {
		if (kind != FSIMG_PLIST_PLIST || parser->root_seen)
			return (plist_corrupt(error, "root is not one plist element"));
		parser->root_seen = true;
	} else {
		parent = &parser->stack[parser->depth - 1];
		switch (parent->kind) {
		case FSIMG_PLIST_PLIST:
			if (!is_value(kind) || parent->children != 0)
				return (plist_corrupt(error,
				    "plist element holds other than one value"));
			break;
		case FSIMG_PLIST_DICT:
			if ((parent->children % 2 == 0) !=
			    (kind == FSIMG_PLIST_KEY) ||
			    kind == FSIMG_PLIST_PLIST)
				return (plist_corrupt(error,
				    "dict does not alternate keys and values"));
			break;
		case FSIMG_PLIST_ARRAY:
			if (!is_value(kind))
				return (plist_corrupt(error, "array holds a key"));
			break;
		default:
			return (plist_corrupt(error, "has an element inside text"));
		}
		parent->children++;
	}
	status = fsimg_session_charge(parser->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	parser->stack[parser->depth].kind = kind;
	parser->stack[parser->depth].children = 0;
	parser->depth++;
	parser->text_length = 0;
	parser->quad_length = 0;
	parser->padding = 0;
	parser->piece_length = 0;
	return (parser->visitor->open(parser->visitor->argument, kind,
	    error));
}

/* Close the innermost element, which name must match, delivering its
 * text or the rest of its data first.
 */
static fsimg_status_t
close_element(plist_parser_t *parser, const char *name, fsimg_error_t *error)
{
	open_element_t *element;
	fsimg_status_t status;

	if (parser->depth == 0)
		return (plist_corrupt(error, "has an end tag without an element"));
	element = &parser->stack[parser->depth - 1];
	if (strcmp(name, element_names[element->kind]) != 0)
		return (plist_corrupt(error,
		    "has an end tag that does not match its element"));
	switch (element->kind) {
	case FSIMG_PLIST_KEY:
	case FSIMG_PLIST_STRING:
	case FSIMG_PLIST_INTEGER:
		status = parser->visitor->text(parser->visitor->argument,
		    parser->text, parser->text_length, error);
		if (status != FSIMG_OK)
			return (status);
		break;
	case FSIMG_PLIST_DATA:
		if (parser->quad_length != 0)
			return (plist_corrupt(error,
			    "data is not a whole number of base64 quads"));
		status = data_flush(parser, error);
		if (status != FSIMG_OK)
			return (status);
		break;
	case FSIMG_PLIST_DICT:
		if (element->children % 2 != 0)
			return (plist_corrupt(error, "dict has a key without a value"));
		break;
	case FSIMG_PLIST_PLIST:
		if (element->children != 1)
			return (plist_corrupt(error,
			    "plist element holds other than one value"));
		break;
	default:
		break;
	}
	parser->depth--;
	return (parser->visitor->close(parser->visitor->argument, element->kind,
	    error));
}

/* After '<' and the name's first byte. */
static fsimg_status_t
start_tag(plist_parser_t *parser, int first, fsimg_error_t *error)
{
	char name[NAME_BYTES + 1];
	int after;
	bool empty;
	fsimg_status_t status;

	status = read_name(parser, first, name, &after, error);
	if (status != FSIMG_OK)
		return (status);
	if (!is_space(after) && after != '>' && after != '/')
		return (plist_corrupt(error, "has a malformed tag"));
	status = skip_attributes(parser, after, &empty, error);
	if (status != FSIMG_OK)
		return (status);
	status = open_element(parser, name, error);
	if (status != FSIMG_OK || !empty)
		return (status);
	return (close_element(parser, name, error));
}

/* After "</". */
static fsimg_status_t
end_tag(plist_parser_t *parser, fsimg_error_t *error)
{
	char name[NAME_BYTES + 1];
	int c;
	fsimg_status_t status;

	status = next_byte(parser, &c, error);
	if (status != FSIMG_OK)
		return (status);
	status = read_name(parser, c, name, &c, error);
	if (status != FSIMG_OK)
		return (status);
	while (is_space(c)) {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (c != '>')
		return (plist_corrupt(error, "has a malformed end tag"));
	return (close_element(parser, name, error));
}

/* After '<': a declaration, a processing instruction, an end tag or a
 * start tag.
 */
static fsimg_status_t
markup(plist_parser_t *parser, fsimg_error_t *error)
{
	int c;
	fsimg_status_t status;

	status = next_byte(parser, &c, error);
	if (status != FSIMG_OK)
		return (status);
	switch (c) {
	case '!':
		return (skip_declaration(parser, error));
	case '?':
		/* The XML declaration and other instructions of the prolog. */
		if (parser->root_seen)
			return (plist_unsupported(error,
			    "a processing instruction after the prolog"));
		return (skip_until(parser, "?>", error));
	case '/':
		return (end_tag(parser, error));
	case -1:
		return (plist_corrupt(error, "ends inside markup"));
	default:
		return (start_tag(parser, c, error));
	}
}

static fsimg_status_t
text_append(plist_parser_t *parser, char c, fsimg_error_t *error)
{
	if (parser->text_length == FSIMG_PLIST_MAX_TEXT)
		return (plist_unsupported(error, "text longer than 4 KiB"));
	parser->text[parser->text_length++] = c;
	return (FSIMG_OK);
}

/* After '&': only the five predefined entities. */
static fsimg_status_t
entity(plist_parser_t *parser, fsimg_error_t *error)
{
	static const struct {
		const char *name;
		char replacement;
	} entities[] = {
		{ "lt", '<' }, { "gt", '>' }, { "amp", '&' }, { "quot", '"' },
		{ "apos", '\'' }
	};
	char name[8];
	size_t length;
	int c;
	fsimg_status_t status;

	length = 0;
	for (;;) {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
		if (c == ';')
			break;
		if (c == '#' && length == 0)
			return (plist_unsupported(error, "a numeric character reference"));
		if (c < 0 || length == sizeof(name) - 1)
			return (plist_corrupt(error, "has an undeclared entity"));
		name[length++] = (char)c;
	}
	name[length] = '\0';
	for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
		if (strcmp(name, entities[i].name) == 0)
			return (text_append(parser, entities[i].replacement, error));
	}
	return (plist_corrupt(error, "has an undeclared entity"));
}

/* One byte of a key, string or integer. XML forbids control characters
 * other than whitespace.
 */
static fsimg_status_t
text_byte(plist_parser_t *parser, int c, fsimg_error_t *error)
{
	if (c == '&')
		return (entity(parser, error));
	if (c < 0x20 && !is_space(c))
		return (plist_corrupt(error, "text holds a control character"));
	return (text_append(parser, (char)c, error));
}

static fsimg_status_t
data_flush(plist_parser_t *parser, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (parser->piece_length == 0)
		return (FSIMG_OK);
	status = parser->visitor->data(parser->visitor->argument, parser->piece,
	    parser->piece_length, error);
	parser->piece_length = 0;
	return (status);
}

/*
 * One byte of base64 data. ASCII whitespace is skipped, as hdiutil wraps
 * the text; any other byte outside the alphabet is corrupt. '=' pads only
 * the last quad, as its last one or two characters, and nothing but
 * whitespace follows it.
 */
static fsimg_status_t
data_byte(plist_parser_t *parser, int c, fsimg_error_t *error)
{
	unsigned value;
	size_t produced;
	fsimg_status_t status;

	if (is_space(c))
		return (FSIMG_OK);
	if (c == '=') {
		if (parser->quad_length < 2)
			return (plist_corrupt(error, "data has misplaced base64 padding"));
		parser->padding++;
		value = 0;
	} else {
		if (parser->padding != 0)
			return (plist_corrupt(error, "data continues after base64 padding"));
		if (c >= 'A' && c <= 'Z')
			value = (unsigned)(c - 'A');
		else if (c >= 'a' && c <= 'z')
			value = (unsigned)(c - 'a') + 26;
		else if (c >= '0' && c <= '9')
			value = (unsigned)(c - '0') + 52;
		else if (c == '+')
			value = 62;
		else if (c == '/')
			value = 63;
		else
			return (plist_corrupt(error, "data holds a byte outside base64"));
	}
	parser->quad[parser->quad_length++] = (unsigned char)value;
	if (parser->quad_length < 4)
		return (FSIMG_OK);

	/* A whole quad: three bytes, fewer by its padding. */
	parser->quad_length = 0;
	produced = 3 - parser->padding;
	if (parser->piece_length + 3 > sizeof(parser->piece)) {
		status = data_flush(parser, error);
		if (status != FSIMG_OK)
			return (status);
	}
	parser->piece[parser->piece_length] = (unsigned char)(parser->quad[0] << 2 |
	    parser->quad[1] >> 4);
	parser->piece[parser->piece_length + 1] = (unsigned char)(parser->quad[1] << 4 |
	    parser->quad[2] >> 2);
	parser->piece[parser->piece_length + 2] = (unsigned char)(parser->quad[2] << 6 |
	    parser->quad[3]);
	parser->piece_length += produced;
	return (FSIMG_OK);
}

/*
 * The document loop: between markup, whitespace is skipped where no text
 * belongs and every byte goes to the open element where text does. The
 * parse ends successfully only at the end of the range, after the root.
 */
static fsimg_status_t
parse(plist_parser_t *parser, fsimg_error_t *error)
{
	int c;
	fsimg_status_t status;

	for (;;) {
		status = next_byte(parser, &c, error);
		if (status != FSIMG_OK)
			return (status);
		if (c < 0)
			break;
		if (c == '<') {
			status = markup(parser, error);
		} else if (parser->depth == 0 ||
		    !takes_text(parser->stack[parser->depth - 1].kind)) {
			if (!is_space(c))
				return (plist_corrupt(error,
				    "has text outside a text element"));
			status = FSIMG_OK;
		} else if (parser->stack[parser->depth - 1].kind ==
		    FSIMG_PLIST_DATA) {
			status = data_byte(parser, c, error);
		} else {
			status = text_byte(parser, c, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	if (!parser->root_seen)
		return (plist_corrupt(error, "has no plist element"));
	if (parser->depth != 0)
		return (plist_corrupt(error, "ends inside an element"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_plist_parse(fsimg_ctx_t *ctx, fsimg_io_t *file, fsimg_session_t *session,
    uint64_t offset, uint64_t length, const fsimg_plist_visitor_t *visitor,
    fsimg_error_t *error)
{
	plist_parser_t *parser;
	uint64_t end;
	fsimg_status_t status;

	if (!fsimg_add_u64(offset, length, &end))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "plist range exceeds the file"));
	parser = fsimg_alloc(ctx, sizeof(*parser), error);
	if (parser == NULL)
		return (error->status);
	bzero(parser, sizeof(*parser));
	parser->file = file;
	parser->session = session;
	parser->visitor = visitor;
	parser->next_offset = offset;
	parser->end = end;
	status = FSIMG_OK;
	parser->window = fsimg_alloc(ctx, WINDOW_BYTES, error);
	if (parser->window != NULL)
		parser->text = fsimg_alloc(ctx, FSIMG_PLIST_MAX_TEXT, error);
	if (parser->text == NULL)
		status = error->status;
	else
		status = parse(parser, error);
	fsimg_dealloc(parser->text);
	fsimg_dealloc(parser->window);
	fsimg_dealloc(parser);
	return (status);
}
