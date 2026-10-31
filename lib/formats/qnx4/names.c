/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "qnx4.h"
#include "unicode.h"

/*
 * Names by length: a direct record holds 1 to 16 bytes, a link record 1 to
 * 48, both NUL-padded and unterminated at full width. A name of 49 to 505
 * bytes lives in a .longfilenames record; its link holds a mangled prefix
 * -- the first 38 bytes, '~', the record's block as eight lowercase hex
 * digits and a NUL -- and the record's block. The record names the link
 * back by position, so a name and its record must agree both ways.
 */

static fsimg_status_t long_name(fsimg_reader_t *, const qnx4_link_t *,
    uint64_t, unsigned char *, size_t *, fsimg_error_t *);
static fsimg_status_t field_name(const unsigned char *, size_t, bool, char *,
    fsimg_error_t *);

/* Check a name against QNX4's own rules: 1 to 505 bytes without NUL or
 * slash, and neither "." nor "..". A name breaking them is corruption.
 * Any other byte is native; preparation classes a name outside the
 * QNX-family profile as omitted.
 */
fsimg_status_t
qnx4_name_native(const unsigned char *name, size_t length,
    fsimg_error_t *error)
{
	if (length == 0 || length > QNX4_NAME_MAX ||
	    memchr(name, '\0', length) != NULL ||
	    memchr(name, '/', length) != NULL ||
	    (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return (qnx4_corrupt(error, "invalid QNX4 file name"));
	return (FSIMG_OK);
}

/* A label follows the profile's byte rules: strict UTF-8 without control
 * bytes, DEL or a slash. The dot names are names, not byte rules, so a
 * label may be "." or "..". An empty label is valid and absent.
 */
bool
qnx4_label_valid(const unsigned char *label, size_t length)
{
	size_t units;

	for (size_t i = 0; i < length; i++)
		if (label[i] < 0x20 || label[i] == 0x7f || label[i] == '/')
			return (false);
	return (fsimg_utf8_to_utf16((const char *)label, length, NULL, 0,
	    &units, NULL) == FSIMG_OK);
}

/* Copy a NUL-padded name field of `length` bytes into out after checking
 * it: the padding after the name must be zero, else the field holds a NUL
 * inside a name, and the name must be native.
 */
static fsimg_status_t
field_name(const unsigned char *name, size_t length, bool padding_zero,
    char *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (!padding_zero)
		return (qnx4_corrupt(error, "QNX4 file name contains a NUL"));
	status = qnx4_name_native(name, length, error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(name, out, length);
	out[length] = '\0';
	return (FSIMG_OK);
}

/* The name of a direct record, into a buffer of at least QNX4_NAME_MAX + 1
 * bytes.
 */
fsimg_status_t
qnx4_direct_name(const qnx4_inode_t *inode, char *out, fsimg_error_t *error)
{
	return (field_name(inode->name, inode->name_length,
	    inode->name_padding_zero, out, error));
}

/* The 48-byte link name of a long name whose record is at block: its
 * first 38 bytes, '~', the block as eight lowercase hexadecimal digits and
 * a zero byte at 47. name holds at least 38 bytes.
 */
void
qnx4_long_prefix(const unsigned char *name, uint32_t block,
    unsigned char *mangled)
{
	char digits[10];

	bzero(mangled, QNX4_LINK_NAME_MAX);
	bcopy(name, mangled, QNX4_LONG_PREFIX);
	snprintf(digits, sizeof(digits), "~%08" PRIx32, block);
	bcopy(digits, mangled + QNX4_LONG_PREFIX, 9);
}

/* Read the .longfilenames record a link names and check it against the
 * link at position: state used, the back-reference to the link's own
 * block and slot, a name of 49 to 505 bytes with zeros after it and a zero
 * last byte, and the link's 48-byte field equal to the mangled prefix.
 */
static fsimg_status_t
long_name(fsimg_reader_t *reader, const qnx4_link_t *link, uint64_t position,
    unsigned char *name, size_t *length, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	unsigned char raw[QNX4_BLOCK_BYTES], mangled[QNX4_LINK_NAME_MAX];
	qnx4_longname_t record;
	uint64_t logical;
	bool found, named;
	fsimg_status_t status;

	*length = 0;
	found = false;
	if (qnx4->has_names) {
		status = qnx4_locator_find(&qnx4->names_locator,
		    link->long_block, &found, &logical, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (!found)
		return (qnx4_corrupt(error,
		    "QNX4 long-name pointer lies outside .longfilenames"));
	status = qnx4_index_read(reader, qnx4->names,
	    logical * QNX4_BLOCK_BYTES, raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	qnx4_longname_decode(raw, &record);
	if (record.state != QNX4_LONG_NAME_USED ||
	    record.block != QNX4_POSITION_BLOCK(position) ||
	    record.slot != QNX4_POSITION_SLOT(position))
		return (qnx4_corrupt(error,
		    "QNX4 long-name record does not name its link"));
	if (record.name_length < QNX4_LONG_NAME_MIN || !record.name_padding_zero ||
	    !record.tail_zero)
		return (qnx4_corrupt(error, "QNX4 long-name record is malformed"));

	qnx4_long_prefix(record.name, link->long_block, mangled);
	if (bcmp(mangled, link->name, sizeof(mangled)) != 0)
		return (qnx4_corrupt(error,
		    "QNX4 long name disagrees with its link's prefix"));

	/* Verification marks the record named. Its back-reference admits one
	 * link position, so a second mark means that link was decoded twice.
	 */
	if (qnx4->long_named != NULL) {
		status = fsimg_bitset_test_and_set(qnx4->long_named, logical,
		    &named, error);
		if (status != FSIMG_OK)
			return (status);
		if (named)
			return (qnx4_corrupt(error,
			    "QNX4 long-name record is named twice"));
	}
	bcopy(record.name, name, record.name_length);
	*length = record.name_length;
	return (FSIMG_OK);
}

/* The name of the link record at position, into a buffer of at least
 * QNX4_NAME_MAX + 1 bytes: its own field, or the long-name record its
 * pointer names.
 */
fsimg_status_t
qnx4_link_name(fsimg_reader_t *reader, const qnx4_link_t *link,
    uint64_t position, char *out, fsimg_error_t *error)
{
	unsigned char name[QNX4_NAME_MAX];
	size_t length;
	fsimg_status_t status;

	if (link->long_block == 0)
		return (field_name(link->name, link->name_length,
		    link->name_padding_zero, out, error));
	status = long_name(reader, link, position, name, &length, error);
	if (status != FSIMG_OK)
		return (status);
	return (field_name(name, length, true, out, error));
}
