/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write-iso9660.h"

/*
 * Primary and Joliet identifiers (specs/design-iso9660.md#primary-names
 * and #joliet-names). Each child of a directory gets its identifier in
 * one hierarchy in two steps, independent of host order and budgets:
 *
 * 1. Natural. A component that the hierarchy can spell exactly, within
 *    the level's limits, has a natural identifier: its uppercase
 *    d-character spelling in the primary hierarchy, its own UCS-2 units in
 *    Joliet. Among children of one directory with the same natural
 *    identifier, the first in catalog order keeps it.
 * 2. Alias. Every other child takes an alias: its component with every
 *    unrepresentable character replaced by '_', cut to fit, followed by
 *    '_' and a seven-digit uppercase hexadecimal number. Numbers count
 *    from 1 in catalog order within each directory and skip every number
 *    a kept natural identifier of the same shape reserves.
 *
 * An alias therefore equals no other identifier: two aliases differ in
 * their numbers, and a natural identifier of the alias shape reserved its
 * number. Files carry ";1" and directories none, so identifiers stay
 * distinct after a reader removes ";1".
 *
 * The passes run over scratch streams, never an in-memory directory: the
 * natural identifiers sorted by parent, identifier and ID give the kept
 * ones; the children needing an alias, sorted by parent and ID, merge
 * with the reservations sorted by parent and number. A final stream of
 * every child's identifier, sorted by parent and the hierarchy's order,
 * is the plan's name stream, and each directory's row records where its
 * children start in it and how many there are.
 */

#define ALIAS_DIGITS		7u
#define ALIAS_SUFFIX		(1u + ALIAS_DIGITS)	/* "_" and the digits */
#define NATURAL_RECORD_MAX	(16u + ISO_JOLIET_IDENTIFIER_MAX)

/* Stream records: a natural identifier is (parent, ID, identifier), an
 * alias request (parent, ID) and a reservation (parent, number), each
 * field a little-endian u64.
 */
typedef struct names_pass {
	iso_plan_t	*plan;
	iso_hierarchy_t	 hierarchy;
	fsimg_stream_t	*naturals;
	fsimg_stream_t	*aliases;
	fsimg_stream_t	*reservations;
} names_pass_t;

/* The reservation cursor of the alias merge: the next reservation not
 * yet consumed, both streams being grouped by increasing parent.
 */
typedef struct reservation_cursor {
	fsimg_stream_t	*stream;
	uint64_t	 offset;
	uint64_t	 parent;
	uint64_t	 number;
	bool		 available;
} reservation_cursor_t;

static void alias_digits(uint32_t, char *);
static bool d_character(unsigned char);
static unsigned char d_upper(unsigned char);
static bool primary_natural(const char *, size_t, bool, unsigned,
    unsigned char *, size_t *);
static size_t primary_alias(const char *, size_t, bool, unsigned, uint32_t,
    unsigned char *);
static bool joliet_forbidden(uint16_t);
static bool joliet_natural(const char *, size_t, bool, unsigned char *,
    size_t *);
static size_t joliet_alias(const char *, size_t, bool, uint32_t,
    unsigned char *);
static bool alias_shape(const unsigned char *, size_t, size_t, uint32_t *);
static bool reserved_number(iso_hierarchy_t, const unsigned char *, size_t,
    bool, uint32_t *);
static bool eligible(iso_hierarchy_t, uint32_t);
static int pair_compare(const void *, size_t, const void *, size_t, void *);
static int natural_compare(const void *, size_t, const void *, size_t,
    void *);
static int padded_units_compare(const unsigned char *, size_t,
    const unsigned char *, size_t);
static int joliet_order(const unsigned char *, size_t, bool,
    const unsigned char *, size_t, bool);
static int name_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t sort_stream(fsimg_stream_t **, fsimg_compare_fn_t,
    void *, fsimg_error_t *);
static fsimg_status_t append_pair(fsimg_stream_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t collect_naturals(names_pass_t *, fsimg_error_t *);
static fsimg_status_t keep_naturals(names_pass_t *, fsimg_error_t *);
static fsimg_status_t next_reservation(reservation_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t assign_aliases(names_pass_t *, fsimg_error_t *);
static fsimg_status_t build_name_stream(names_pass_t *, fsimg_error_t *);
static fsimg_status_t finish_group(names_pass_t *, uint64_t, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t record_groups(names_pass_t *, fsimg_error_t *);

/*
 * Identifiers.
 */

/* An alias number, at most ISO_ALIAS_MAX, as seven uppercase hexadecimal
 * digits.
 */
static void
alias_digits(uint32_t number, char *digits)
{
	static const char hex[] = "0123456789ABCDEF";

	for (unsigned i = ALIAS_DIGITS; i > 0; i--) {
		digits[i - 1] = hex[number & 0xf];
		number >>= 4;
	}
}

/* A d-character before uppercasing: an ASCII letter, digit or '_'. */
static bool
d_character(unsigned char c)
{
	return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9') || c == '_');
}

static unsigned char
d_upper(unsigned char c)
{
	return (c >= 'a' && c <= 'z' ? (unsigned char)(c - 'a' + 'A') : c);
}

/* The natural primary identifier of a component: only d-characters and,
 * for a file, at most one '.', within the level's limits once uppercased:
 * at level 1 a name of 8 and an extension of 3 characters, a directory of
 * 8; at levels 2 and 3 a name and extension of 30 together, a directory
 * of 31. A file's identifier is NAME.EXT;1, with the '.' even when the
 * extension is empty.
 */
static bool
primary_natural(const char *name, size_t length, bool directory,
    unsigned level, unsigned char *out, size_t *out_length)
{
	const unsigned char *bytes = (const unsigned char *)name;
	size_t dot, base, extension, at;

	dot = length;
	for (size_t i = 0; i < length; i++) {
		if (d_character(bytes[i]))
			continue;
		if (bytes[i] != '.' || directory || dot != length)
			return (false);
		dot = i;
	}
	base = dot;
	extension = dot == length ? 0 : length - dot - 1;
	if (directory) {
		if (length == 0 || length > (level == 1 ? 8u : 31u))
			return (false);
	} else if (level == 1) {
		if (base > 8 || extension > 3 || base + extension == 0)
			return (false);
	} else if (base + extension > 30 || base + extension == 0) {
		return (false);
	}
	at = 0;
	for (size_t i = 0; i < base; i++)
		out[at++] = d_upper(bytes[i]);
	if (!directory) {
		out[at++] = '.';
		for (size_t i = 0; i < extension; i++)
			out[at++] = d_upper(bytes[dot + 1 + i]);
		out[at++] = ';';
		out[at++] = '1';
	}
	*out_length = at;
	return (true);
}

/* The primary alias with number: the component's bytes with every one
 * outside the d-characters replaced by '_' and letters uppercased; for a
 * file split at its last '.', the extension cut to 3 characters at level
 * 1 and 5 otherwise; the name cut to leave room for "_" and seven hex
 * digits within the level's limits, which leaves none at level 1.
 */
static size_t
primary_alias(const char *name, size_t length, bool directory,
    unsigned level, uint32_t number, unsigned char *out)
{
	const unsigned char *bytes = (const unsigned char *)name;
	char digits[ALIAS_DIGITS];
	size_t base, extension, extension_start, room, at;

	base = length;
	extension = 0;
	extension_start = length;
	if (!directory) {
		for (size_t i = length; i > 0; i--) {
			if (bytes[i - 1] == '.') {
				base = i - 1;
				extension_start = i;
				extension = length - i;
				break;
			}
		}
		if (extension > (level == 1 ? 3u : 5u))
			extension = level == 1 ? 3u : 5u;
	}
	if (level == 1)
		room = 0;
	else if (directory)
		room = 31 - ALIAS_SUFFIX;
	else
		room = 30 - ALIAS_SUFFIX - extension;
	if (base > room)
		base = room;

	at = 0;
	for (size_t i = 0; i < base; i++)
		out[at++] = d_character(bytes[i]) ? d_upper(bytes[i]) : '_';
	alias_digits(number, digits);
	out[at++] = '_';
	bcopy(digits, out + at, ALIAS_DIGITS);
	at += ALIAS_DIGITS;
	if (!directory) {
		out[at++] = '.';
		for (size_t i = 0; i < extension; i++) {
			unsigned char c = bytes[extension_start + i];

			out[at++] = d_character(c) ? d_upper(c) : '_';
		}
		out[at++] = ';';
		out[at++] = '1';
	}
	return (at);
}

/* A character Joliet cannot hold in an identifier: a control character,
 * a surrogate, or one of * / : ; ? and backslash.
 */
static bool
joliet_forbidden(uint16_t unit)
{
	return (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f) ||
	    (unit >= 0xd800 && unit <= 0xdfff) || unit == '*' ||
	    unit == '/' || unit == ':' || unit == ';' || unit == '?' ||
	    unit == '\\');
}

/* The natural Joliet identifier: the component's own UCS-2 units, when
 * none is forbidden and they fit 64 units for a directory or 62 for a
 * file, which then adds ";1"; written big-endian into out.
 */
static bool
joliet_natural(const char *name, size_t length, bool directory,
    unsigned char *out, size_t *out_length)
{
	uint16_t units[ISO_RR_NAME_MAX];
	size_t count;

	if (fsimg_utf8_to_utf16(name, length, units, ISO_RR_NAME_MAX, &count,
	    NULL) != FSIMG_OK)
		return (false);
	if (count > (directory ? ISO_JOLIET_UNITS_MAX :
	    ISO_JOLIET_UNITS_MAX - 2))
		return (false);
	for (size_t i = 0; i < count; i++) {
		if (joliet_forbidden(units[i]))
			return (false);
		fsimg_store_be16(out + 2 * i, units[i]);
	}
	if (!directory) {
		fsimg_store_be16(out + 2 * count, ';');
		fsimg_store_be16(out + 2 * count + 2, '1');
		count += 2;
	}
	*out_length = 2 * count;
	return (true);
}

/* The Joliet alias with number, the primary rule over characters: each
 * forbidden character, a non-BMP one written as a surrogate pair
 * included, becomes one '_'; a file splits at its last '.', its extension
 * cut to 5 units; the name is cut to leave room for "_" and seven hex
 * digits, the '.' and extension when there was one, and a file's ";1".
 */
static size_t
joliet_alias(const char *name, size_t length, bool directory,
    uint32_t number, unsigned char *out)
{
	uint16_t units[ISO_RR_NAME_MAX], mapped[ISO_RR_NAME_MAX];
	char digits[ALIAS_DIGITS];
	size_t count, mapped_count, base, extension, extension_start, room, at;
	bool dot;

	count = 0;
	if (fsimg_utf8_to_utf16(name, length, units, ISO_RR_NAME_MAX, &count,
	    NULL) != FSIMG_OK)
		count = 0;
	mapped_count = 0;
	for (size_t i = 0; i < count; i++) {
		if (units[i] >= 0xd800 && units[i] <= 0xdbff &&
		    i + 1 < count && units[i + 1] >= 0xdc00 &&
		    units[i + 1] <= 0xdfff) {
			mapped[mapped_count++] = '_';
			i++;
			continue;
		}
		mapped[mapped_count++] = joliet_forbidden(units[i]) ? '_' :
		    units[i];
	}

	base = mapped_count;
	extension = 0;
	extension_start = mapped_count;
	dot = false;
	if (!directory) {
		for (size_t i = mapped_count; i > 0; i--) {
			if (mapped[i - 1] == '.') {
				dot = true;
				base = i - 1;
				extension_start = i;
				extension = mapped_count - i;
				break;
			}
		}
		if (extension > 5)
			extension = 5;
	}
	if (directory)
		room = ISO_JOLIET_UNITS_MAX - ALIAS_SUFFIX;
	else
		room = ISO_JOLIET_UNITS_MAX - 2 - ALIAS_SUFFIX -
		    (dot ? 1 + extension : 0);
	if (base > room)
		base = room;

	at = 0;
	for (size_t i = 0; i < base; i++, at += 2)
		fsimg_store_be16(out + at, mapped[i]);
	alias_digits(number, digits);
	fsimg_store_be16(out + at, '_');
	at += 2;
	for (size_t i = 0; i < ALIAS_DIGITS; i++, at += 2)
		fsimg_store_be16(out + at, (unsigned char)digits[i]);
	if (dot) {
		fsimg_store_be16(out + at, '.');
		at += 2;
		for (size_t i = 0; i < extension; i++, at += 2)
			fsimg_store_be16(out + at, mapped[extension_start + i]);
	}
	if (!directory) {
		fsimg_store_be16(out + at, ';');
		fsimg_store_be16(out + at + 2, '1');
		at += 4;
	}
	return (at);
}

/* Whether the identifier's name part, its first name_length bytes in
 * units of unit bytes (1 primary, 2 Joliet), ends in '_' and seven
 * uppercase hexadecimal digits, the alias shape; *number is their value.
 */
static bool
alias_shape(const unsigned char *identifier, size_t name_length, size_t unit,
    uint32_t *number)
{
	size_t characters = name_length / unit, first;
	uint32_t value = 0, c;

	if (characters < ALIAS_SUFFIX)
		return (false);
	first = characters - ALIAS_SUFFIX;
	for (size_t i = 0; i < ALIAS_SUFFIX; i++) {
		c = unit == 1 ? identifier[first + i] :
		    fsimg_load_be16(identifier + 2 * (first + i));
		if (i == 0) {
			if (c != '_')
				return (false);
		} else if (c >= '0' && c <= '9') {
			value = value * 16 + c - '0';
		} else if (c >= 'A' && c <= 'F') {
			value = value * 16 + c - 'A' + 10;
		} else {
			return (false);
		}
	}
	*number = value;
	return (true);
}

/* The alias number a kept natural identifier reserves, if any: its name
 * part, before a file's ";1" and last '.', or a directory's whole
 * identifier, has the alias shape.
 */
static bool
reserved_number(iso_hierarchy_t hierarchy, const unsigned char *identifier,
    size_t length, bool directory, uint32_t *number)
{
	size_t unit = hierarchy == ISO_PRIMARY ? 1 : 2, name;

	name = length;
	if (!directory) {
		name -= 2 * unit;		/* ";1" */
		for (size_t i = name / unit; i > 0; i--) {
			uint32_t c = unit == 1 ? identifier[i - 1] :
			    fsimg_load_be16(identifier + 2 * (i - 1));

			if (c == '.') {
				name = (i - 1) * unit;
				break;
			}
		}
	}
	return (alias_shape(identifier, name, unit, number));
}

/* The identifier of a component in a hierarchy: its natural identifier
 * when alias is 0, else its alias with that number. out holds
 * ISO_JOLIET_IDENTIFIER_MAX bytes. False when alias is 0 and the
 * component has no natural identifier, which a consistent plan never
 * asks for.
 */
bool
iso_identifier(const iso_plan_t *plan, iso_hierarchy_t hierarchy,
    const char *name, size_t length, bool directory, uint32_t alias,
    unsigned char *out, size_t *out_length)
{
	if (hierarchy == ISO_PRIMARY) {
		if (alias == 0)
			return (primary_natural(name, length, directory,
			    plan->level, out, out_length));
		*out_length = primary_alias(name, length, directory,
		    plan->level, alias, out);
		return (true);
	}
	if (alias == 0)
		return (joliet_natural(name, length, directory, out,
		    out_length));
	*out_length = joliet_alias(name, length, directory, alias, out);
	return (true);
}

/* Joliet holds files and directories; the primary hierarchy every kind. */
static bool
eligible(iso_hierarchy_t hierarchy, uint32_t kind)
{
	return (hierarchy == ISO_PRIMARY || kind == FSIMG_ENTRY_FILE ||
	    kind == FSIMG_ENTRY_DIRECTORY);
}

/*
 * Stream orders.
 */

/* Two little-endian u64 fields: parent, then ID or number. */
static int
pair_compare(const void *a, size_t a_length FSIMG_UNUSED, const void *b,
    size_t b_length FSIMG_UNUSED, void *argument FSIMG_UNUSED)
{
	uint64_t av, bv;

	for (unsigned offset = 0; offset < 16; offset += 8) {
		av = fsimg_load_le64((const unsigned char *)a + offset);
		bv = fsimg_load_le64((const unsigned char *)b + offset);
		if (av != bv)
			return (av < bv ? -1 : 1);
	}
	return (0);
}

/* Natural records: parent, identifier bytes, then ID, so the first of
 * each equal identifier in a directory is the first in catalog order.
 */
static int
natural_compare(const void *a, size_t a_length, const void *b,
    size_t b_length, void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	uint64_t av, bv;
	size_t common;
	int order;

	av = fsimg_load_le64(left);
	bv = fsimg_load_le64(right);
	if (av != bv)
		return (av < bv ? -1 : 1);
	common = a_length < b_length ? a_length : b_length;
	order = memcmp(left + 16, right + 16, common - 16);
	if (order != 0)
		return (order);
	if (a_length != b_length)
		return (a_length < b_length ? -1 : 1);
	av = fsimg_load_le64(left + 8);
	bv = fsimg_load_le64(right + 8);
	return ((av > bv) - (av < bv));
}

/* Compare UTF-16BE unit strings, the shorter padded with U+0020. */
static int
padded_units_compare(const unsigned char *a, size_t a_units,
    const unsigned char *b, size_t b_units)
{
	size_t units = a_units > b_units ? a_units : b_units;
	uint16_t ca, cb;

	for (size_t i = 0; i < units; i++) {
		ca = i < a_units ? fsimg_load_be16(a + 2 * i) : 0x20;
		cb = i < b_units ? fsimg_load_be16(b + 2 * i) : 0x20;
		if (ca != cb)
			return (ca < cb ? -1 : 1);
	}
	return (0);
}

/* Joliet's order, the primary rule over UTF-16 units: the name before the
 * first '.', then the extension up to ";1", each padded with U+0020; a
 * directory's whole identifier is its name. Every writer file carries
 * version 1, so versions never decide.
 */
static int
joliet_order(const unsigned char *a, size_t a_length, bool a_directory,
    const unsigned char *b, size_t b_length, bool b_directory)
{
	const unsigned char *side[2] = { a, b };
	size_t length[2] = { a_length / 2, b_length / 2 };
	size_t name[2], extension_start[2], extension[2];
	bool directory[2] = { a_directory, b_directory };
	int order;

	for (unsigned s = 0; s < 2; s++) {
		size_t units = directory[s] ? length[s] : length[s] - 2;

		name[s] = units;
		extension_start[s] = units;
		extension[s] = 0;
		for (size_t i = 0; !directory[s] && i < units; i++) {
			if (fsimg_load_be16(side[s] + 2 * i) == '.') {
				name[s] = i;
				extension_start[s] = i + 1;
				extension[s] = units - i - 1;
				break;
			}
		}
	}
	order = padded_units_compare(a, name[0], b, name[1]);
	if (order == 0)
		order = padded_units_compare(a + 2 * extension_start[0],
		    extension[0], b + 2 * extension_start[1], extension[1]);
	return (order);
}

/* Name-stream records: parent, then the hierarchy's order, then the
 * identifier's bytes, the writer's tie-break for identifiers the order
 * finds equal, such as a directory FOO and a file FOO.;1. Identifiers in
 * one directory are distinct, so the order is total.
 */
static int
name_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument)
{
	const iso_hierarchy_t *hierarchy = argument;
	const unsigned char *left = a, *right = b;
	const unsigned char *ai = left + ISO_NAME_HEADER,
	    *bi = right + ISO_NAME_HEADER;
	size_t an = a_length - ISO_NAME_HEADER, bn = b_length - ISO_NAME_HEADER,
	    common;
	uint64_t av, bv;
	int order;

	av = fsimg_load_le64(left);
	bv = fsimg_load_le64(right);
	if (av != bv)
		return (av < bv ? -1 : 1);
	if (*hierarchy == ISO_PRIMARY)
		order = iso_primary_order(ai, an, left[16] != 0, bi, bn,
		    right[16] != 0);
	else
		order = joliet_order(ai, an, left[16] != 0, bi, bn,
		    right[16] != 0);
	if (order != 0)
		return (order);
	common = an < bn ? an : bn;
	order = memcmp(ai, bi, common);
	if (order != 0)
		return (order);
	return ((an > bn) - (an < bn));
}

/* Seal *stream, sort it into a new stream and replace it. */
static fsimg_status_t
sort_stream(fsimg_stream_t **stream, fsimg_compare_fn_t compare,
    void *argument, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	fsimg_status_t status;

	status = fsimg_stream_seal(*stream, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(*stream, compare, argument, NULL,
		    &sorted, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_stream_free(*stream);
	*stream = sorted;
	return (FSIMG_OK);
}

static fsimg_status_t
append_pair(fsimg_stream_t *stream, uint64_t first, uint64_t second,
    fsimg_error_t *error)
{
	unsigned char raw[16];

	fsimg_store_le64(raw, first);
	fsimg_store_le64(raw + 8, second);
	return (fsimg_stream_append(stream, raw, sizeof(raw), NULL, error));
}

/* Decode the name-stream record at *cursor into name, borrowing raw,
 * which holds ISO_NAME_RECORD_MAX bytes, and advance the cursor.
 */
fsimg_status_t
iso_name_next(fsimg_stream_t *stream, uint64_t *cursor, unsigned char *raw,
    iso_name_t *name, fsimg_error_t *error)
{
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(stream, cursor, raw, ISO_NAME_RECORD_MAX,
	    &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length <= ISO_NAME_HEADER)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid ISO 9660 name record"));
	name->parent = fsimg_load_le64(raw);
	name->id = fsimg_load_le64(raw + 8);
	name->directory = raw[16] != 0;
	name->identifier = raw + ISO_NAME_HEADER;
	name->length = length - ISO_NAME_HEADER;
	return (FSIMG_OK);
}

/*
 * The passes.
 */

/* Step 1: every eligible child's natural identifier, or an alias request
 * for a child without one. In the primary hierarchy a root directory
 * naturally named RR_MOVED takes an alias, leaving that name to the
 * relocation directory.
 */
static fsimg_status_t
collect_naturals(names_pass_t *pass, fsimg_error_t *error)
{
	iso_plan_t *plan = pass->plan;
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	unsigned char raw[NATURAL_RECORD_MAX];
	const char *name;
	size_t length;
	bool directory, natural;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		if (!eligible(pass->hierarchy, catalog->entry.kind))
			continue;
		name = strrchr(catalog->entry.path, '/') + 1;
		directory = catalog->entry.kind == FSIMG_ENTRY_DIRECTORY;
		natural = iso_identifier(plan, pass->hierarchy, name,
		    strlen(name), directory, 0, raw + 16, &length);
		if (natural && pass->hierarchy == ISO_PRIMARY &&
		    catalog->parent_id == 0 && directory && length == 8 &&
		    bcmp(raw + 16, "RR_MOVED", 8) == 0)
			natural = false;
		if (natural) {
			fsimg_store_le64(raw, catalog->parent_id);
			fsimg_store_le64(raw + 8, id);
			status = fsimg_stream_append(pass->naturals, raw,
			    16 + length, NULL, error);
		} else {
			status = append_pair(pass->aliases, catalog->parent_id,
			    id, error);
		}
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Step 2: over the sorted naturals, the first of each equal identifier in
 * a directory keeps it and reserves its number when it has the alias
 * shape; every later one requests an alias.
 */
static fsimg_status_t
keep_naturals(names_pass_t *pass, fsimg_error_t *error)
{
	iso_plan_t *plan = pass->plan;
	unsigned char raw[NATURAL_RECORD_MAX], previous[NATURAL_RECORD_MAX];
	iso_row_t row;
	uint64_t cursor, parent, id;
	size_t length, previous_length;
	uint32_t number;
	fsimg_status_t status;

	cursor = 0;
	previous_length = 0;
	for (;;) {
		status = fsimg_stream_next(pass->naturals, &cursor, raw,
		    sizeof(raw), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(
			    &plan->common.writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (length <= 16)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "invalid ISO 9660 name record"));
		parent = fsimg_load_le64(raw);
		id = fsimg_load_le64(raw + 8);
		if (previous_length == length &&
		    fsimg_load_le64(previous) == parent &&
		    bcmp(previous + 16, raw + 16, length - 16) == 0) {
			status = append_pair(pass->aliases, parent, id, error);
			if (status != FSIMG_OK)
				return (status);
			continue;
		}
		bcopy(raw, previous, length);
		previous_length = length;

		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		row.alias[pass->hierarchy] = 0;
		row.identifier[pass->hierarchy] = (uint8_t)(length - 16);
		status = iso_row_set(plan, id, &row, error);
		if (status == FSIMG_OK && reserved_number(pass->hierarchy,
		    raw + 16, length - 16, row.kind == FSIMG_ENTRY_DIRECTORY,
		    &number))
			status = append_pair(pass->reservations, parent,
			    number, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

static fsimg_status_t
next_reservation(reservation_cursor_t *cursor, fsimg_error_t *error)
{
	unsigned char raw[16];
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(cursor->stream, &cursor->offset, raw,
	    sizeof(raw), &length, error);
	cursor->available = status == FSIMG_OK;
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != sizeof(raw))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid ISO 9660 alias reservation"));
	cursor->parent = fsimg_load_le64(raw);
	cursor->number = fsimg_load_le64(raw + 8);
	return (FSIMG_OK);
}

/* Step 3: the alias requests in parent and ID order take increasing
 * numbers from 1 per directory, skipping reserved ones. The reservation
 * cursor only moves forward: reservations below the candidate, or of an
 * earlier parent, are passed; one equal to it bumps the candidate.
 */
static fsimg_status_t
assign_aliases(names_pass_t *pass, fsimg_error_t *error)
{
	iso_plan_t *plan = pass->plan;
	fsimg_writer_t *writer = plan->common.writer;
	reservation_cursor_t reservation = { .stream = pass->reservations };
	const fsimg_catalog_entry_t *catalog;
	unsigned char raw[16], identifier[ISO_JOLIET_IDENTIFIER_MAX];
	iso_row_t row;
	uint64_t cursor, parent, id, current, candidate;
	size_t length;
	const char *name;
	fsimg_status_t status;

	status = next_reservation(&reservation, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	current = UINT64_MAX;
	candidate = 1;
	for (;;) {
		status = fsimg_stream_next(pass->aliases, &cursor, raw,
		    sizeof(raw), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&writer->session, 0, 0,
			    1, error);
		if (status != FSIMG_OK)
			return (status);
		if (length != sizeof(raw))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "invalid ISO 9660 alias request"));
		parent = fsimg_load_le64(raw);
		id = fsimg_load_le64(raw + 8);
		if (parent != current) {
			current = parent;
			candidate = 1;
		}
		while (reservation.available &&
		    (reservation.parent < parent ||
		    (reservation.parent == parent &&
		    reservation.number <= candidate))) {
			if (reservation.parent == parent &&
			    reservation.number == candidate)
				candidate++;
			status = next_reservation(&reservation, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (candidate > ISO_ALIAS_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "ISO 9660 alias numbers exhausted"));

		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		name = strrchr(catalog->entry.path, '/') + 1;
		iso_identifier(plan, pass->hierarchy, name, strlen(name),
		    row.kind == FSIMG_ENTRY_DIRECTORY, (uint32_t)candidate,
		    identifier, &length);
		row.alias[pass->hierarchy] = (uint32_t)candidate;
		row.identifier[pass->hierarchy] = (uint8_t)length;
		status = iso_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		candidate++;
	}
}

/* Step 4: every eligible child's final identifier into the name stream,
 * sorted by parent and the hierarchy's order.
 */
static fsimg_status_t
build_name_stream(names_pass_t *pass, fsimg_error_t *error)
{
	iso_plan_t *plan = pass->plan;
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	unsigned char raw[ISO_NAME_RECORD_MAX];
	iso_row_t row;
	const char *name;
	size_t length;
	bool directory;
	fsimg_status_t status;

	status = fsimg_stream_new(&writer->session, ISO_NAME_RECORD_MAX,
	    &plan->names[pass->hierarchy], error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count;
	    id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (!eligible(pass->hierarchy, row.kind))
			continue;
		name = strrchr(catalog->entry.path, '/') + 1;
		directory = row.kind == FSIMG_ENTRY_DIRECTORY;
		if (!iso_identifier(plan, pass->hierarchy, name, strlen(name),
		    directory, row.alias[pass->hierarchy],
		    raw + ISO_NAME_HEADER, &length) ||
		    length != row.identifier[pass->hierarchy])
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER,
			    "ISO 9660 identifier disagrees with its plan row"));
		fsimg_store_le64(raw, catalog->parent_id);
		fsimg_store_le64(raw + 8, id);
		raw[16] = directory ? 1 : 0;
		status = fsimg_stream_append(plan->names[pass->hierarchy], raw,
		    ISO_NAME_HEADER + length, NULL, error);
	}
	if (status != FSIMG_OK)
		return (status);
	return (sort_stream(&plan->names[pass->hierarchy], name_compare,
	    &pass->hierarchy, error));
}

/* Record one directory's group in hierarchy h: the frame offset of its
 * first child in the sorted name stream and the count.
 */
static fsimg_status_t
finish_group(names_pass_t *pass, uint64_t parent, uint64_t offset,
    uint64_t count, fsimg_error_t *error)
{
	iso_row_t row;
	fsimg_status_t status;

	if (count > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "ISO 9660 directory has too many entries"));
	status = iso_row_get(pass->plan, parent, &row, error);
	if (status != FSIMG_OK)
		return (status);
	row.group[pass->hierarchy] = offset;
	row.children[pass->hierarchy] = (uint32_t)count;
	return (iso_row_set(pass->plan, parent, &row, error));
}

/* Step 5: each directory's group in the sorted name stream. A directory
 * without children keeps the empty group planning gave it.
 */
static fsimg_status_t
record_groups(names_pass_t *pass, fsimg_error_t *error)
{
	unsigned char raw[ISO_NAME_RECORD_MAX];
	iso_name_t name;
	uint64_t cursor, offset, parent, group, count;
	fsimg_status_t status;

	cursor = 0;
	parent = UINT64_MAX;
	group = count = 0;
	for (;;) {
		offset = cursor;
		status = iso_name_next(pass->plan->names[pass->hierarchy],
		    &cursor, raw, &name, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		if (name.parent != parent) {
			if (parent != UINT64_MAX) {
				status = finish_group(pass, parent, group,
				    count, error);
				if (status != FSIMG_OK)
					return (status);
			}
			parent = name.parent;
			group = offset;
			count = 0;
		}
		count++;
	}
	if (parent == UINT64_MAX)
		return (FSIMG_OK);
	return (finish_group(pass, parent, group, count, error));
}

/* Give every eligible child its identifier in the hierarchy and build
 * the hierarchy's sorted name stream and group table.
 */
fsimg_status_t
iso_write_names(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_error_t *error)
{
	fsimg_session_t *session = &plan->common.writer->session;
	names_pass_t pass = { .plan = plan, .hierarchy = hierarchy };
	fsimg_status_t status;

	status = fsimg_stream_new(session, NATURAL_RECORD_MAX, &pass.naturals,
	    error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, 16, &pass.aliases, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, 16, &pass.reservations,
		    error);
	if (status == FSIMG_OK)
		status = collect_naturals(&pass, error);
	if (status == FSIMG_OK)
		status = sort_stream(&pass.naturals, natural_compare, NULL,
		    error);
	if (status == FSIMG_OK)
		status = keep_naturals(&pass, error);
	if (status == FSIMG_OK)
		status = sort_stream(&pass.aliases, pair_compare, NULL, error);
	if (status == FSIMG_OK)
		status = sort_stream(&pass.reservations, pair_compare, NULL,
		    error);
	if (status == FSIMG_OK)
		status = assign_aliases(&pass, error);
	fsimg_stream_free(pass.naturals);
	fsimg_stream_free(pass.aliases);
	fsimg_stream_free(pass.reservations);
	if (status == FSIMG_OK)
		status = build_name_stream(&pass, error);
	if (status == FSIMG_OK)
		status = record_groups(&pass, error);
	return (status);
}
