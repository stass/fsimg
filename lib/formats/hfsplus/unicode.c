/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "hfsplus.h"
#include "unicode.h"
#include "unicode-tables.h"

/* HFS Plus names. A stored name is a sequence of UTF-16 units, not a C
 * string; on disk the units are big-endian, and the helpers that convert or
 * decompose work on host-order copies in stack buffers.
 *
 * Three forms of a name meet here. The stored units are what the catalog
 * holds and its comparators order. The image path component is their UTF-8
 * with the two exchanges, / as : and U+0000 as U+2400, so it holds neither a
 * separator nor a NUL. The namespace key is what selects an entry: the
 * decomposed units, folded on a folded volume, as big-endian bytes.
 *
 * A name is admitted when the mapping from stored units to path and back
 * reproduces them. That one round trip excludes what each exchange leaves
 * without a spelling, and every name that decomposition would change, with no
 * rule of its own for each.
 */

_Static_assert(HFSPLUS_DECOMP_MAX_UNITS == HFSPLUS_UNIT_DECOMP_MAX,
    "the generated table's longest decomposition");

/* Hangul syllables decompose by formula rather than from the table. */
#define HANGUL_FIRST		0xac00u
#define HANGUL_LAST		0xd7a3u
#define HANGUL_LEADING		0x1100u
#define HANGUL_VOWEL		0x1161u
#define HANGUL_TRAILING		0x11a7u
#define HANGUL_VOWEL_SPAN	588u	/* syllables per leading consonant */
#define HANGUL_TRAILING_SPAN	28u	/* syllables per vowel */

#define UNIT_SLASH	0x002fu
#define UNIT_COLON	0x003au
#define UNIT_NUL	0x0000u
#define UNIT_NUL_SYMBOL	0x2400u

static void sort_marks(uint16_t *, size_t);
static void units_load(const unsigned char *, size_t, uint16_t *);
static uint16_t next_folded(const unsigned char *, size_t, size_t *);
static int compare_folded(const unsigned char *, size_t,
    const unsigned char *, size_t);
static int compare_binary(const unsigned char *, size_t,
    const unsigned char *, size_t);
static size_t key_encode(hfsplus_compare_t, const uint16_t *, size_t,
    unsigned char *);

uint16_t
hfsplus_fold_unit(uint16_t unit)
{
	unsigned page;

	page = hfsplus_fold_index[unit >> 8];
	if (page == 0)
		return (unit);
	return (hfsplus_fold_pages[page - 1][unit & 0xff]);
}

uint8_t
hfsplus_combining_class(uint16_t unit)
{
	unsigned page;

	page = hfsplus_class_index[unit >> 8];
	if (page == 0)
		return (0);
	return (hfsplus_class_pages[page - 1][unit & 0xff]);
}

size_t
hfsplus_unit_decompose(uint16_t unit, uint16_t *parts)
{
	unsigned page;
	uint16_t entry;
	size_t count;

	if (unit >= HANGUL_FIRST && unit <= HANGUL_LAST) {
		unsigned syllable, trailing;

		syllable = unit - HANGUL_FIRST;
		trailing = syllable % HANGUL_TRAILING_SPAN;
		parts[0] = (uint16_t)(HANGUL_LEADING +
		    syllable / HANGUL_VOWEL_SPAN);
		parts[1] = (uint16_t)(HANGUL_VOWEL +
		    syllable % HANGUL_VOWEL_SPAN / HANGUL_TRAILING_SPAN);
		if (trailing == 0)
			return (2);
		parts[2] = (uint16_t)(HANGUL_TRAILING + trailing);
		return (3);
	}

	page = hfsplus_decomp_index[unit >> 8];
	entry = page == 0 ? 0 : hfsplus_decomp_pages[page - 1][unit & 0xff];
	if (entry == 0) {
		parts[0] = unit;
		return (1);
	}
	count = entry & HFSPLUS_DECOMP_COUNT_MASK;
	bcopy(&hfsplus_decomp_units[entry >> HFSPLUS_DECOMP_OFFSET_SHIFT],
	    parts, count * sizeof(parts[0]));
	return (count);
}

/* A stable insertion sort of each maximal run of nonzero classes. A class 0
 * unit never moves and stops every mark that reaches it, since its class is
 * not above the mark's, so the runs need no explicit detection. Names are
 * bounded to 255 units, which bounds the quadratic worst case.
 */
static void
sort_marks(uint16_t *units, size_t count)
{
	for (size_t i = 1; i < count; i++) {
		uint16_t mark;
		uint8_t mark_class;
		size_t slot;

		mark = units[i];
		mark_class = hfsplus_combining_class(mark);
		if (mark_class == 0)
			continue;
		slot = i;
		while (slot > 0 &&
		    hfsplus_combining_class(units[slot - 1]) > mark_class) {
			units[slot] = units[slot - 1];
			slot--;
		}
		units[slot] = mark;
	}
}

/* Decomposition is per unit and then one sort over the whole result: the
 * order is observed, since U+0F73 yields marks with no starter that must sort
 * with those around it. A supplementary value's two surrogates have neither
 * a table entry nor a class, so they pass through as a pair.
 */
bool
hfsplus_decompose(const uint16_t *input, size_t count, uint16_t *output,
    size_t capacity, size_t *written)
{
	size_t produced;

	produced = 0;
	for (size_t i = 0; i < count; i++) {
		uint16_t parts[HFSPLUS_UNIT_DECOMP_MAX];
		size_t part_count;

		part_count = hfsplus_unit_decompose(input[i], parts);
		if (part_count > capacity - produced)
			return (false);
		bcopy(parts, output + produced, part_count * sizeof(parts[0]));
		produced += part_count;
	}
	sort_marks(output, produced);
	*written = produced;
	return (true);
}

static void
units_load(const unsigned char *raw, size_t count, uint16_t *units)
{
	for (size_t i = 0; i < count; i++)
		units[i] = fsimg_load_be16(raw + 2 * i);
}

/* The next non-ignorable folded value at or after *position, or zero at the
 * end of the name. No value it returns for a unit is zero, so the end sorts
 * before every unit, which puts a prefix first.
 */
static uint16_t
next_folded(const unsigned char *name, size_t units, size_t *position)
{
	while (*position < units) {
		uint16_t folded;

		folded = hfsplus_fold_unit(fsimg_load_be16(name +
		    2 * *position));
		(*position)++;
		if (folded != 0)
			return (folded);
	}
	return (0);
}

static int
compare_folded(const unsigned char *a, size_t a_units,
    const unsigned char *b, size_t b_units)
{
	size_t a_position, b_position;
	uint16_t a_folded, b_folded;

	a_position = 0;
	b_position = 0;
	do {
		a_folded = next_folded(a, a_units, &a_position);
		b_folded = next_folded(b, b_units, &b_position);
		if (a_folded != b_folded)
			return (a_folded < b_folded ? -1 : 1);
	} while (a_folded != 0);
	return (0);
}

/* Big-endian units order as their bytes do. The caller has handled empty
 * names, so both pointers are valid.
 */
static int
compare_binary(const unsigned char *a, size_t a_units,
    const unsigned char *b, size_t b_units)
{
	size_t common;
	int order;

	common = a_units < b_units ? a_units : b_units;
	order = memcmp(a, b, 2 * common);
	if (order != 0)
		return (order);
	if (a_units == b_units)
		return (0);
	return (a_units < b_units ? -1 : 1);
}

/* An empty name, a thread's, precedes every nonempty name under both
 * comparators, and so also precedes a name of ignorable units alone, which
 * folds to nothing.
 */
int
hfsplus_name_compare(hfsplus_compare_t compare, const unsigned char *a,
    size_t a_units, const unsigned char *b, size_t b_units)
{
	if (a_units == 0 || b_units == 0) {
		if (a_units == b_units)
			return (0);
		return (a_units == 0 ? -1 : 1);
	}
	if (compare == HFSPLUS_COMPARE_FOLDED)
		return (compare_folded(a, a_units, b, b_units));
	return (compare_binary(a, a_units, b, b_units));
}

int
hfsplus_ckey_compare(hfsplus_compare_t compare, const hfsplus_ckey_t *a,
    const hfsplus_ckey_t *b)
{
	if (a->parent != b->parent)
		return (a->parent < b->parent ? -1 : 1);
	return (hfsplus_name_compare(compare, a->name, a->units, b->name,
	    b->units));
}

/* The shared conversion refuses only an unpaired surrogate here: the output
 * bound holds any 255 units, with or without the exchange of a one-byte
 * U+0000 for a three-byte U+2400.
 */
fsimg_status_t
hfsplus_name_to_path(const unsigned char *name, size_t units, char *path,
    size_t *path_bytes, fsimg_error_t *error)
{
	uint16_t host[HFSPLUS_NAME_UNITS_MAX];

	if (units == 0 || units > HFSPLUS_NAME_UNITS_MAX)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "HFS Plus name has an invalid length"));

	units_load(name, units, host);
	for (size_t i = 0; i < units; i++) {
		if (host[i] == UNIT_SLASH)
			host[i] = UNIT_COLON;
		else if (host[i] == UNIT_NUL)
			host[i] = UNIT_NUL_SYMBOL;
	}
	if (fsimg_utf16_to_utf8(host, units, path, HFSPLUS_NAME_UTF8_MAX,
	    path_bytes, NULL) != FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "HFS Plus name has an unpaired surrogate"));
	return (FSIMG_OK);
}

bool
hfsplus_path_to_units(const char *component, size_t length,
    uint16_t *decomposed, size_t *decomposed_units)
{
	uint16_t host[HFSPLUS_NAME_UNITS_MAX];
	size_t units;

	if (fsimg_utf8_to_utf16(component, length, host,
	    HFSPLUS_NAME_UNITS_MAX, &units, NULL) != FSIMG_OK)
		return (false);

	for (size_t i = 0; i < units; i++) {
		if (host[i] == UNIT_COLON)
			host[i] = UNIT_SLASH;
		else if (host[i] == UNIT_NUL_SYMBOL)
			host[i] = UNIT_NUL;
	}
	return (hfsplus_decompose(host, units, decomposed,
	    HFSPLUS_NAME_UNITS_MAX, decomposed_units));
}

fsimg_status_t
hfsplus_name_admit(const unsigned char *name, size_t units,
    fsimg_error_t *error)
{
	char path[HFSPLUS_NAME_UTF8_MAX];
	uint16_t stored[HFSPLUS_NAME_UNITS_MAX];
	uint16_t mapped[HFSPLUS_NAME_UNITS_MAX];
	size_t path_bytes, mapped_units;
	fsimg_status_t status;

	status = hfsplus_name_to_path(name, units, path, &path_bytes, error);
	if (status != FSIMG_OK)
		return (status);

	if ((path_bytes == 1 && path[0] == '.') ||
	    (path_bytes == 2 && path[0] == '.' && path[1] == '.'))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "HFS Plus name is a dot name"));

	units_load(name, units, stored);
	if (!hfsplus_path_to_units(path, path_bytes, mapped, &mapped_units) ||
	    mapped_units != units ||
	    bcmp(mapped, stored, units * sizeof(stored[0])) != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER,
		    "HFS Plus name does not map back from its image path"));
	return (FSIMG_OK);
}

/* Encode decomposed units as big-endian bytes; on a folded volume each is
 * folded first and an ignorable one dropped. key holds count units.
 */
static size_t
key_encode(hfsplus_compare_t compare, const uint16_t *units, size_t count,
    unsigned char *key)
{
	size_t key_bytes;

	key_bytes = 0;
	for (size_t i = 0; i < count; i++) {
		uint16_t value;

		value = units[i];
		if (compare == HFSPLUS_COMPARE_FOLDED) {
			value = hfsplus_fold_unit(value);
			if (value == 0)
				continue;
		}
		fsimg_store_be16(key + key_bytes, value);
		key_bytes += 2;
	}
	return (key_bytes);
}

fsimg_status_t
hfsplus_stored_key(hfsplus_compare_t compare, const unsigned char *name,
    size_t units, unsigned char *key, size_t *key_bytes, fsimg_error_t *error)
{
	uint16_t stored[HFSPLUS_NAME_UNITS_MAX];
	uint16_t decomposed[HFSPLUS_NAME_UNITS_MAX];
	size_t decomposed_units;

	if (units > HFSPLUS_NAME_UNITS_MAX)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "HFS Plus name has an invalid length"));

	units_load(name, units, stored);
	if (!hfsplus_decompose(stored, units, decomposed,
	    HFSPLUS_NAME_UNITS_MAX, &decomposed_units))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "HFS Plus name decomposes past 255 units"));

	*key_bytes = key_encode(compare, decomposed, decomposed_units, key);
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_query_key(hfsplus_compare_t compare, const char *component,
    size_t length, unsigned char *key, size_t *key_bytes, fsimg_error_t *error)
{
	uint16_t decomposed[HFSPLUS_NAME_UNITS_MAX];
	size_t decomposed_units;

	if (!hfsplus_path_to_units(component, length, decomposed,
	    &decomposed_units))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "path component is not valid UTF-8 within 255 units"));

	*key_bytes = key_encode(compare, decomposed, decomposed_units, key);
	return (FSIMG_OK);
}
