/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "crc32c.h"
#include "directory.h"
#include "fsimg-endian.h"
#include "unicode.h"
#include "unicode-tables.h"

/*
 * APFS names. A comparison form is built in place in its caller's form:
 * decode the UTF-8 and append each point's canonical decomposition, sort
 * each run of nonzero combining classes stably (canonical reordering), and
 * for a case-insensitive volume replace each point by its decomposed
 * folding and reorder again. The tables' foldings are already decomposed,
 * so the second pass yields the NFD of the folding. Every append checks the
 * form's capacity, although the generator's expansion bound means no
 * accepted name can reach it.
 */

_Static_assert(APFS_UNICODE_EXPANSION <= APFS_NAME_EXPANSION,
    "the tables expand a name beyond the form's capacity");
_Static_assert(APFS_NAME_KEY_BYTES <= FSIMG_DIRECTORY_KEY_BYTES,
    "a namespace key does not fit the common key buffer");

#define HANGUL_FIRST	0xac00u
#define HANGUL_COUNT	11172u
#define HANGUL_L	0x1100u
#define HANGUL_V	0x1161u
#define HANGUL_T	0x11a7u
#define HANGUL_V_T	588u	/* vowels times tails, 21 * 28 */
#define HANGUL_TAILS	28u

/* The most points a table entry or a Hangul syllable maps to. */
#define MAP_POINTS	APFS_MAP_COUNT_MASK

/* Points per CRC-32C update in apfs_name_hash(). */
#define HASH_CHUNK	64u

static unsigned	combining_class(uint32_t);
static size_t	map_point(uint32_t, const uint8_t *, size_t,
		    const uint16_t (*)[256], const uint32_t *, uint32_t *);
static size_t	decompose(uint32_t, uint32_t *);
static size_t	fold(uint32_t, uint32_t *);
static void	reorder(apfs_name_form_t *);
static bool	fold_form(apfs_name_form_t *);
static bool	dot_name(const char *, size_t);
static bool	assigned(uint32_t);

static unsigned
combining_class(uint32_t point)
{
	size_t page;
	unsigned i;

	page = point >> 8;
	if (page >= sizeof(apfs_class_index))
		return (0);
	i = apfs_class_index[page];
	return (i == 0 ? 0 : apfs_class_pages[i - 1][point & 0xff]);
}

/* Store the points a paged mapping table gives point, or point itself when
 * its entry is zero, and return their count, at most MAP_POINTS.
 */
static size_t
map_point(uint32_t point, const uint8_t *index, size_t index_length,
    const uint16_t (*pages)[256], const uint32_t *pool, uint32_t *out)
{
	size_t page, count;
	uint16_t entry;
	unsigned i;

	page = point >> 8;
	i = page < index_length ? index[page] : 0;
	entry = i == 0 ? 0 : pages[i - 1][point & 0xff];
	if (entry == 0) {
		out[0] = point;
		return (1);
	}
	count = entry & APFS_MAP_COUNT_MASK;
	bcopy(pool + (entry >> APFS_MAP_OFFSET_SHIFT), out,
	    count * sizeof(*out));
	return (count);
}

/* The full canonical decomposition of point; Hangul syllables by rule. */
static size_t
decompose(uint32_t point, uint32_t *out)
{
	uint32_t s;

	/* Unsigned: a point below HANGUL_FIRST wraps far above the count. */
	if (point - HANGUL_FIRST < HANGUL_COUNT) {
		s = point - HANGUL_FIRST;
		out[0] = HANGUL_L + s / HANGUL_V_T;
		out[1] = HANGUL_V + s % HANGUL_V_T / HANGUL_TAILS;
		if (s % HANGUL_TAILS == 0)
			return (2);
		out[2] = HANGUL_T + s % HANGUL_TAILS;
		return (3);
	}
	return (map_point(point, apfs_decomp_index, sizeof(apfs_decomp_index),
	    apfs_decomp_pages, apfs_decomp_points, out));
}

/* The full case folding of point, decomposed. */
static size_t
fold(uint32_t point, uint32_t *out)
{
	return (map_point(point, apfs_fold_index, sizeof(apfs_fold_index),
	    apfs_fold_pages, apfs_fold_points, out));
}

/* Sort each run of points of nonzero combining class by class, keeping
 * the order of equal classes. A starter, class 0, never moves, and no mark
 * moves past one, so an insertion sort over the whole form sorts each run.
 */
static void
reorder(apfs_name_form_t *form)
{
	uint32_t point;
	unsigned class;
	size_t j;

	for (size_t i = 1; i < form->count; i++) {
		point = form->points[i];
		class = combining_class(point);
		if (class == 0)
			continue;
		for (j = i; j > 0 && combining_class(form->points[j - 1]) > class;
		    j--)
			form->points[j] = form->points[j - 1];
		form->points[j] = point;
	}
}

/* Replace each point of form by its folding, in place: the length of the
 * result is summed first, then each folding is written from the end, where
 * it never overtakes a point still to be read, since every point folds to
 * at least one. False, with form unchanged, if the result would not fit.
 */
static bool
fold_form(apfs_name_form_t *form)
{
	uint32_t mapped[MAP_POINTS];
	size_t total, count, end;

	total = 0;
	for (size_t i = 0; i < form->count; i++)
		total += fold(form->points[i], mapped);
	if (total > APFS_NAME_FORM_POINTS)
		return (false);
	end = total;
	for (size_t i = form->count; i-- > 0;) {
		count = fold(form->points[i], mapped);
		end -= count;
		bcopy(mapped, form->points + end, count * sizeof(*mapped));
	}
	form->count = total;
	return (true);
}

static bool
dot_name(const char *name, size_t length)
{
	return ((length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'));
}

/* Whether point is a scalar value the pinned version assigns; the
 * generator checks that no noncharacter is.
 */
static bool
assigned(uint32_t point)
{
	size_t page;
	unsigned i, byte;

	page = point >> 8;
	if (page >= sizeof(apfs_assigned_index))
		return (false);
	i = apfs_assigned_index[page];
	if (i == 0)
		return (false);
	byte = apfs_assigned_pages[i - 1][(point & 0xff) >> 3];
	return (((byte >> (point & 7)) & 1) != 0);
}

fsimg_status_t
apfs_name_form(apfs_name_rule_t rule, const char *name, size_t length,
    apfs_name_form_t *form)
{
	const unsigned char *bytes;
	uint32_t point, mapped[MAP_POINTS];
	size_t used, count;
	fsimg_status_t status;

	form->count = 0;
	if (length > APFS_NAME_MAX_BYTES)
		return (FSIMG_EUNSUPPORTED);
	if (length == 0 || dot_name(name, length))
		return (FSIMG_ECORRUPT);
	bytes = (const unsigned char *)name;
	for (size_t offset = 0; offset < length; offset += used) {
		if (!fsimg_utf8_decode(bytes + offset, length - offset, &used,
		    &point) || point == 0 || point == '/') {
			status = FSIMG_ECORRUPT;
			goto fail;
		}
		if (rule == APFS_NAMES_BYTES) {
			mapped[0] = point;
			count = 1;
		} else {
			count = decompose(point, mapped);
		}
		/* Unreachable for an accepted length, as is the fold's. */
		if (count > APFS_NAME_FORM_POINTS - form->count) {
			status = FSIMG_ELIMIT;
			goto fail;
		}
		bcopy(mapped, form->points + form->count,
		    count * sizeof(*mapped));
		form->count += count;
	}
	switch (rule) {
	case APFS_NAMES_BYTES:
		break;
	case APFS_NAMES_NFD:
		reorder(form);
		break;
	case APFS_NAMES_CASELESS:
		reorder(form);
		if (!fold_form(form)) {
			status = FSIMG_ELIMIT;
			goto fail;
		}
		/*
		 * The NFD of the folding, by definition. Under the pinned
		 * tables this changes nothing: the one folding of an NFD
		 * point that touches a mark, U+0345 to U+03B9, turns a mark
		 * into a starter, which leaves both parts of its run sorted.
		 */
		reorder(form);
		break;
	}
	return (FSIMG_OK);
fail:
	form->count = 0;
	return (status);
}

uint32_t
apfs_name_hash(const apfs_name_form_t *form)
{
	unsigned char units[HASH_CHUNK * 4];
	uint32_t crc;
	size_t count;

	crc = FSIMG_CRC32C_SEED;
	for (size_t i = 0; i < form->count; i += count) {
		count = form->count - i < HASH_CHUNK ? form->count - i :
		    HASH_CHUNK;
		for (size_t j = 0; j < count; j++)
			fsimg_store_le32(units + 4 * j, form->points[i + j]);
		crc = fsimg_crc32c_update(crc, units, 4 * count);
	}
	return (crc & APFS_NAME_HASH_MASK);
}

fsimg_status_t
apfs_name_key(const apfs_name_form_t *form, char *key, size_t capacity,
    size_t *length)
{
	char encoded[4];
	size_t total;

	*length = 0;
	total = 0;
	for (size_t i = 0; i < form->count; i++)
		total += fsimg_utf8_encode(form->points[i], encoded);
	if (total > capacity)
		return (FSIMG_ERANGE);
	total = 0;
	for (size_t i = 0; i < form->count; i++)
		total += fsimg_utf8_encode(form->points[i], key + total);
	*length = total;
	return (FSIMG_OK);
}

bool
apfs_name_assigned(const char *name, size_t length)
{
	const unsigned char *bytes;
	uint32_t point;
	size_t used;

	bytes = (const unsigned char *)name;
	for (size_t offset = 0; offset < length; offset += used) {
		if (!fsimg_utf8_decode(bytes + offset, length - offset, &used,
		    &point) || !assigned(point))
			return (false);
	}
	return (true);
}
