/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx6.h"
#include "unicode.h"

/* Check a name against the fsimg profile: 1..510 bytes of strict UTF-8
 * without control bytes or DEL, and neither "." nor "..". QNX itself
 * forbids NUL, slash and the dot names, so they are corruption; a name QNX
 * accepts but the profile does not is unsupported.
 */
fsimg_status_t
qnx6_name_profile(const unsigned char *name, size_t length,
    fsimg_error_t *error)
{
	size_t units;

	if (length == 0 || length > QNX6_NAME_MAX ||
	    memchr(name, '\0', length) != NULL ||
	    memchr(name, '/', length) != NULL ||
	    (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return (qnx6_corrupt(error, "invalid QNX6 file name"));
	for (size_t i = 0; i < length; i++)
		if (name[i] < 0x20 || name[i] == 0x7f)
			return (qnx6_unsupported(error,
			    "QNX6 file name contains a control byte"));
	if (fsimg_utf8_to_utf16((const char *)name, length, NULL, 0, &units,
	    NULL) != FSIMG_OK)
		return (qnx6_unsupported(error, "QNX6 file name is not UTF-8"));
	return (FSIMG_OK);
}

/* Read a long-name record: a two-byte length and the name bytes at logical
 * byte index * B of the long-name tree, within one block.
 */
static fsimg_status_t
long_name_read(fsimg_reader_t *reader, const qnx6_slot_t *slot,
    unsigned char *name, size_t *length, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	unsigned char prefix[2];
	uint64_t offset;
	uint32_t expected;
	fsimg_status_t status;

	if (!slot->long_padding_zero)
		return (qnx6_unsupported(error, "QNX6 long-name slot has extension bytes"));
	offset = (uint64_t)slot->long_index * g->block_size;
	if (offset >= fsimg_extent_index_length(qnx6->names))
		return (qnx6_corrupt(error, "QNX6 long-name index is out of range"));
	status = qnx6_index_read(reader, qnx6->names, offset, prefix,
	    sizeof(prefix), error);
	if (status != FSIMG_OK)
		return (status);
	*length = qnx6_u16(prefix, g->big_endian);
	/* QNX6_NAME_MAX + 2 <= 512 <= B, so a valid record fits its block. */
	if (*length < QNX6_LONG_NAME_MIN || *length > QNX6_NAME_MAX)
		return (qnx6_corrupt(error, "QNX6 long-name length is out of range"));
	status = qnx6_index_read(reader, qnx6->names, offset + 2, name, *length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	expected = (qnx6->header.flags & QNX6_FLAG_CHECKSUM) != 0 ?
	    qnx6_name_checksum(name, *length) : 0;
	if (slot->checksum != expected)
		return (qnx6_corrupt(error, "QNX6 long-name checksum mismatch"));
	return (FSIMG_OK);
}

/* Decode a live slot's name into a NUL-terminated buffer of at least
 * QNX6_NAME_MAX + 1 bytes and check it against the profile.
 */
fsimg_status_t
qnx6_slot_name(fsimg_reader_t *reader, const qnx6_slot_t *slot, char *out,
    fsimg_error_t *error)
{
	unsigned char name[QNX6_NAME_MAX];
	size_t length;
	fsimg_status_t status;

	if (slot->long_name) {
		status = long_name_read(reader, slot, name, &length, error);
		if (status != FSIMG_OK)
			return (status);
	} else {
		if (slot->length > QNX6_INLINE_NAME_MAX)
			return (qnx6_corrupt(error, "QNX6 inline name length is invalid"));
		length = slot->length;
		bcopy(slot->name, name, length);
	}
	status = qnx6_name_profile(name, length, error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(name, out, length);
	out[length] = '\0';
	return (FSIMG_OK);
}
