/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "iso9660.h"
#include "unicode.h"

/*
 * Names of the primary and joliet views
 * (specs/design-iso9660.md#displayed-names-and-keys). An identifier is
 * checked natively, Joliet's decoded from UTF-16BE to UTF-8, then
 * displayed without a final ";1": other versions and a trailing '.' stay,
 * so two identifiers that differ only there stay apart, and two that
 * become equal collide in namespace validation. Bytes outside the portable
 * name profile are not refused here; preparation omits such an entry.
 */

#define ISO_VERSION_MAX 32767u

_Static_assert(ISO_NAME_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "an ISO 9660 name key fits a directory key buffer");

/* Whether a ';'-separated file version is decimal 1 through 32767 without
 * leading zeros.
 */
static bool
version_valid(const unsigned char *digits, size_t count)
{
	unsigned value = 0;

	if (count == 0 || count > 5 || digits[0] == '0')
		return (false);
	for (size_t i = 0; i < count; i++) {
		if (digits[i] < '0' || digits[i] > '9')
			return (false);
		value = value * 10 + (unsigned)(digits[i] - '0');
	}
	return (value <= ISO_VERSION_MAX);
}

/* Bytes that end in ";1", the one version the display rule removes. */
static size_t
without_version_one(const unsigned char *bytes, size_t length)
{
	if (length >= 2 && bytes[length - 2] == ';' && bytes[length - 1] == '1')
		return (length - 2);
	return (length);
}

/* Finish the displayed name of an identifier whose length bytes are in
 * out: remove a final ";1" and NUL-terminate it. An identifier displaying
 * as nothing, "." or ".." names no entry, and a query loses a final ";1"
 * as the display rule does, so a displayed name still ending in one could
 * never be selected and its spelling would select another entry; both are
 * corrupt, and leave out empty.
 */
static fsimg_status_t
display_finish(char *out, size_t length, size_t *shown, fsimg_error_t *error)
{
	const unsigned char *bytes = (const unsigned char *)out;

	length = without_version_one(bytes, length);
	out[length] = '\0';
	if (length == 0 || (bytes[0] == '.' && (length == 1 ||
	    (length == 2 && bytes[1] == '.')))) {
		out[0] = '\0';
		return (iso_corrupt(error,
		    "ISO 9660 identifier displays as no name"));
	}
	if (without_version_one(bytes, length) != length) {
		out[0] = '\0';
		return (iso_corrupt(error,
		    "ISO 9660 identifier displays with a final ;1"));
	}
	*shown = length;
	return (FSIMG_OK);
}

/* Check a primary identifier of length bytes and write its displayed name,
 * NUL-terminated, to out. The native checks: no 0x00 or '/' byte, and in
 * a file identifier holding ';' a valid version after the last one. Each
 * failure is corrupt. A misplaced dot identifier never arrives here:
 * run_add() refuses it in every view.
 */
fsimg_status_t
iso_primary_name(const unsigned char *identifier, size_t length,
    bool directory, char *out, size_t *shown, fsimg_error_t *error)
{
	const unsigned char *version;

	*shown = 0;
	out[0] = '\0';
	if (length == 0 || length > ISO_IDENTIFIER_MAX)
		return (iso_corrupt(error, "invalid ISO 9660 identifier length"));
	if (memchr(identifier, '\0', length) != NULL ||
	    memchr(identifier, '/', length) != NULL)
		return (iso_corrupt(error,
		    "ISO 9660 identifier holds a NUL or a slash"));
	if (!directory) {
		version = NULL;
		for (size_t i = 0; i < length; i++) {
			if (identifier[i] == ';')
				version = identifier + i + 1;
		}
		if (version != NULL && !version_valid(version,
		    (size_t)(identifier + length - version)))
			return (iso_corrupt(error,
			    "invalid ISO 9660 file version"));
	}
	bcopy(identifier, out, length);
	return (display_finish(out, length, shown, error));
}

/* Decode count big-endian UTF-16 code units at field to WTF-8 in out of
 * capacity bytes and store the length written. A surrogate pair becomes its
 * scalar and an unpaired surrogate the generalized three-byte form, so the
 * result holds no 0x00 or '/' byte the units lack, keeps distinct
 * identifiers distinct, and is not strict UTF-8 when a surrogate is
 * unpaired. False when count exceeds an identifier's units or the output
 * exceeds capacity; three bytes per unit always fit.
 */
static bool
utf16be_decode(const unsigned char *field, size_t count, char *out,
    size_t capacity, size_t *length)
{
	uint16_t units[ISO_IDENTIFIER_MAX / 2];

	*length = 0;
	if (count > sizeof(units) / sizeof(units[0]))
		return (false);
	for (size_t i = 0; i < count; i++)
		units[i] = fsimg_load_be16(field + 2 * i);
	return (fsimg_utf16_to_wtf8(units, count, out, capacity, length,
	    NULL) == FSIMG_OK);
}

_Static_assert(ISO_IDENTIFIER_MAX / 2 * 3 <= ISO_NAME_MAX,
    "a decoded Joliet identifier fits a displayed-name buffer");

/* Check a Joliet identifier of length bytes and write its displayed name,
 * NUL-terminated, to out (specs/design-iso9660.md#joliet-identifiers). The
 * native checks: an even length, and no code unit 0x0000 or '/'. The
 * identifier is decoded as UTF-16BE; a name with an unpaired surrogate is
 * displayed, and preparation omits it under the portable name profile.
 * Joliet carries no version for any reader, so ';' is an ordinary
 * character and only the display rule's final ";1" is removed. Each
 * failure is corrupt.
 */
fsimg_status_t
iso_joliet_name(const unsigned char *identifier, size_t length, char *out,
    size_t *shown, fsimg_error_t *error)
{
	uint16_t unit;
	size_t decoded;

	*shown = 0;
	out[0] = '\0';
	if (length == 0 || length > ISO_IDENTIFIER_MAX || length % 2 != 0)
		return (iso_corrupt(error,
		    "invalid ISO 9660 Joliet identifier length"));
	for (size_t i = 0; i < length; i += 2) {
		unit = fsimg_load_be16(identifier + i);
		if (unit == 0x0000 || unit == '/')
			return (iso_corrupt(error,
			    "ISO 9660 Joliet identifier holds a NUL or a slash"));
	}
	if (!utf16be_decode(identifier, length / 2, out, ISO_NAME_MAX,
	    &decoded)) {
		out[0] = '\0';
		return (iso_corrupt(error,
		    "ISO 9660 Joliet identifier does not decode"));
	}
	return (display_finish(out, decoded, shown, error));
}

_Static_assert(ISO_LABEL_BYTES / 2 * 3 <= ISO_LABEL_TEXT_MAX &&
    ISO_LABEL_BYTES <= ISO_LABEL_TEXT_MAX,
    "a decoded label fits its buffer");

/* The label of a primary or Joliet descriptor: its 32-byte volume
 * identifier without trailing spaces or NULs, into out of
 * ISO_LABEL_TEXT_MAX + 1 bytes. A primary label is its bytes, a Joliet
 * label its 16 UTF-16BE code units as UTF-8. False, with out zeroed, when
 * the rest is not printable UTF-8, which leaves the volume without a
 * reported label.
 */
bool
iso_label_decode(const iso_descriptor_t *descriptor, char *out)
{
	const unsigned char *field = descriptor->label;
	size_t length, units;

	if (descriptor->joliet_level != 0) {
		units = ISO_LABEL_BYTES / 2;
		while (units > 0 &&
		    (fsimg_load_be16(field + 2 * (units - 1)) == ' ' ||
		    fsimg_load_be16(field + 2 * (units - 1)) == 0x0000))
			units--;
		if (!utf16be_decode(field, units, out, ISO_LABEL_TEXT_MAX,
		    &length)) {
			bzero(out, ISO_LABEL_TEXT_MAX + 1);
			return (false);
		}
	} else {
		length = ISO_LABEL_BYTES;
		while (length > 0 && (field[length - 1] == ' ' ||
		    field[length - 1] == '\0'))
			length--;
		bcopy(field, out, length);
	}
	out[length] = '\0';
	for (size_t i = 0; i < length; i++) {
		if ((unsigned char)out[i] < 0x20 || out[i] == 0x7f) {
			bzero(out, ISO_LABEL_TEXT_MAX + 1);
			return (false);
		}
	}
	if (fsimg_utf8_to_utf16(out, length, NULL, 0, &units, NULL) !=
	    FSIMG_OK) {
		bzero(out, ISO_LABEL_TEXT_MAX + 1);
		return (false);
	}
	return (true);
}

/* The namespace keys of a stored component: its exact bytes, and for a
 * directory the extent key, a zero byte and the opened extent's block in
 * big-endian order. No name holds a zero byte, so the two kinds never
 * meet, and two records opening one directory collide like two equal
 * names.
 */
fsimg_status_t
iso_keys(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_directory_keys_t *keys,
    fsimg_error_t *error)
{
	iso_payload_t decoded;
	size_t bytes = strlen(name);
	fsimg_status_t status;

	keys->count = 0;
	status = iso_payload_decode(iso_state(reader), payload, length,
	    &decoded, error);
	if (status != FSIMG_OK)
		return (status);
	if (bytes == 0 || bytes > ISO_NAME_MAX || strchr(name, '/') != NULL)
		return (iso_corrupt(error, "invalid prepared ISO 9660 name"));
	bcopy(name, keys->key[0], bytes);
	keys->length[0] = bytes;
	keys->count = 1;
	if (decoded.kind == FSIMG_ENTRY_DIRECTORY) {
		keys->key[1][0] = 0x00;
		fsimg_store_be32(keys->key[1] + 1, decoded.opened_extent);
		keys->length[1] = 5;
		keys->count = 2;
	}
	return (FSIMG_OK);
}

/* A query spells a displayed name, case and all, so a component outside
 * the profile matches nothing and is refused. Outside the Rock Ridge
 * view, whose names carry no version, a final ";1" is removed as the
 * display rule removes it, so README.TXT;1 and README.TXT select one
 * entry. A key never starts with a zero byte, so no query reaches an
 * extent key.
 */
fsimg_status_t
iso_key(fsimg_reader_t *reader, const char *component, unsigned char *key,
    size_t *length, fsimg_error_t *error)
{
	size_t bytes = strlen(component);

	*length = 0;
	if (bytes <= ISO_NAME_MAX &&
	    iso_state(reader)->view != FSIMG_ISO_VIEW_ROCKRIDGE)
		bytes = without_version_one((const unsigned char *)component,
		    bytes);
	if (bytes > ISO_NAME_MAX ||
	    !fsimg_name_in_portable_profile(component, bytes))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selection component is not representable in ISO 9660"));
	bcopy(component, key, bytes);
	*length = bytes;
	return (FSIMG_OK);
}

/* The parts of a primary identifier the order compares: the file name,
 * before the first '.', or a directory's whole identifier; the extension,
 * up to the last ';', which iso_primary_name also takes as the version's
 * separator; and the version after it. A missing part is empty.
 */
typedef struct order_parts {
	const unsigned char *name;
	const unsigned char *extension;
	const unsigned char *version;
	size_t		 name_length;
	size_t		 extension_length;
	size_t		 version_length;
} order_parts_t;

static void
order_split(const unsigned char *identifier, size_t length, bool directory,
    order_parts_t *parts)
{
	const unsigned char *dot;
	size_t end;

	bzero(parts, sizeof(*parts));
	parts->name = identifier;
	parts->name_length = length;
	if (directory)
		return;
	for (end = length; end > 0 && identifier[end - 1] != ';'; end--)
		continue;
	if (end > 0) {
		parts->version = identifier + end;
		parts->version_length = length - end;
		end--;
	} else {
		end = length;
	}
	parts->name_length = end;
	dot = memchr(identifier, '.', end);
	if (dot != NULL) {
		parts->name_length = (size_t)(dot - identifier);
		parts->extension = dot + 1;
		parts->extension_length = end - parts->name_length - 1;
	}
}

/* Compare by byte, the shorter padded with spaces. */
static int
padded_compare(const unsigned char *a, size_t a_length, const unsigned char *b,
    size_t b_length)
{
	size_t length = a_length > b_length ? a_length : b_length;
	unsigned char ca, cb;

	for (size_t i = 0; i < length; i++) {
		ca = i < a_length ? a[i] : 0x20;
		cb = i < b_length ? b[i] : 0x20;
		if (ca != cb)
			return (ca < cb ? -1 : 1);
	}
	return (0);
}

/* Versions are decimal without leading zeros, so a longer one is larger
 * and equal lengths compare by byte. Without both versions, as for a
 * directory, the step finds the identifiers equal.
 */
static int
version_compare(const order_parts_t *a, const order_parts_t *b)
{
	int order;

	if (a->version_length == 0 || b->version_length == 0)
		return (0);
	if (a->version_length != b->version_length)
		return (a->version_length < b->version_length ? -1 : 1);
	order = memcmp(a->version, b->version, a->version_length);
	return ((order > 0) - (order < 0));
}

/* The order of two primary identifiers
 * (specs/design-iso9660.md#ordering): name, extension, then version
 * descending, each directory's identifier being its whole name.
 * Identifiers it finds equal, such as a directory FOO and a file FOO.;1,
 * may stand in either order.
 */
int
iso_primary_order(const unsigned char *a, size_t a_length, bool a_directory,
    const unsigned char *b, size_t b_length, bool b_directory)
{
	order_parts_t pa, pb;
	int order;

	order_split(a, a_length, a_directory, &pa);
	order_split(b, b_length, b_directory, &pb);
	order = padded_compare(pa.name, pa.name_length, pb.name,
	    pb.name_length);
	if (order == 0)
		order = padded_compare(pa.extension, pa.extension_length,
		    pb.extension, pb.extension_length);
	if (order == 0)
		order = -version_compare(&pa, &pb);
	return (order);
}
