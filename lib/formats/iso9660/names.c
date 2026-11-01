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
 * Names of the primary view (specs/design-iso9660.md#displayed-names-and-keys).
 * An identifier is checked natively, then displayed without a final ";1":
 * other versions and a trailing '.' stay, so two identifiers that differ
 * only there stay apart, and two that become equal collide in namespace
 * validation. Bytes outside the portable name profile are not refused
 * here; preparation omits such an entry.
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

/* Check a primary identifier of length bytes and write its displayed name,
 * NUL-terminated, to out. The native checks: no 0x00 or '/' byte, and in
 * a file identifier holding ';' a valid version after the last one. A
 * one-byte 0x00 or 0x01 identifier is a dot record out of place, and an
 * identifier displaying as nothing, "." or ".." names no entry; all are
 * corrupt.
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
	if (length == 1 && identifier[0] <= 0x01)
		return (iso_corrupt(error, "misplaced ISO 9660 dot record"));
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
	length = without_version_one(identifier, length);
	if (length == 0 || (identifier[0] == '.' && (length == 1 ||
	    (length == 2 && identifier[1] == '.'))))
		return (iso_corrupt(error,
		    "ISO 9660 identifier displays as no name"));
	/* A query loses a final ";1" as the display rule does, so a displayed
	 * name ending in one could never be selected and its spelling would
	 * select another entry.
	 */
	if (without_version_one(identifier, length) != length)
		return (iso_corrupt(error,
		    "ISO 9660 identifier displays with a final ;1"));
	bcopy(identifier, out, length);
	out[length] = '\0';
	*shown = length;
	return (FSIMG_OK);
}

/* The label: the 32-byte volume identifier without trailing spaces or NUL
 * bytes, into out of ISO_LABEL_BYTES + 1 bytes. False when the rest is not
 * printable UTF-8, which leaves the volume without a reported label.
 */
bool
iso_label_decode(const unsigned char *field, char *out)
{
	size_t length, units;

	out[0] = '\0';
	length = ISO_LABEL_BYTES;
	while (length > 0 && (field[length - 1] == ' ' ||
	    field[length - 1] == '\0'))
		length--;
	for (size_t i = 0; i < length; i++) {
		if (field[i] < 0x20 || field[i] == 0x7f)
			return (false);
	}
	if (fsimg_utf8_to_utf16((const char *)field, length, NULL, 0, &units,
	    NULL) != FSIMG_OK)
		return (false);
	bcopy(field, out, length);
	out[length] = '\0';
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
