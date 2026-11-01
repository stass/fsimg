/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "iso9660.h"
#include "unicode.h"

/*
 * Pure decoders for ISO 9660 structures: both-byte-order fields, the two
 * date forms, volume descriptors and directory records. They perform no
 * I/O and fill their outputs with validated values only; every failure
 * names its class (specs/design-iso9660.md).
 */

/* Byte offsets within a volume descriptor. */
#define VD_TYPE			0u
#define VD_IDENTIFIER		1u
#define VD_VERSION		6u
#define VD_FLAGS		7u
#define VD_LABEL		40u
#define VD_ESCAPES		88u
#define VD_SPACE_SIZE		80u
#define VD_SET_SIZE		120u
#define VD_SEQUENCE		124u
#define VD_BLOCK_SIZE		128u
#define VD_PATH_TABLE_SIZE	132u
#define VD_PATH_TABLE_L		140u
#define VD_PATH_TABLE_L_COPY	144u
#define VD_PATH_TABLE_M		148u
#define VD_PATH_TABLE_M_COPY	152u
#define VD_ROOT			156u
#define VD_DATES		813u	/* four 17-byte dates */
#define VD_STRUCTURE_VERSION	881u

/* Byte offsets within a directory record. */
#define DR_LENGTH		0u
#define DR_EA_BLOCKS		1u
#define DR_EXTENT		2u
#define DR_DATA_LENGTH		10u
#define DR_DATE			18u
#define DR_FLAGS		25u
#define DR_UNIT_SIZE		26u
#define DR_GAP_SIZE		27u
#define DR_SEQUENCE		28u
#define DR_IDENTIFIER_LENGTH	32u
#define DR_IDENTIFIER		33u

#define DATE7_BYTES		7u
#define DATE17_BYTES		17u
#define DATE17_DIGITS		16u
#define OFFSET_MIN		(-48)	/* 15-minute units west of UTC */
#define OFFSET_MAX		52

bool
iso_bytes_zero(const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (bytes[i] != 0)
			return (false);
	}
	return (true);
}

/* A both-byte-order field: a little-endian copy, then a big-endian one. The
 * copies are the format's only consistency check, so a mismatch is
 * corruption.
 */
fsimg_status_t
iso_u16both(const unsigned char *field, uint16_t *value, fsimg_error_t *error)
{
	*value = fsimg_load_le16(field);
	if (*value != fsimg_load_be16(field + 2))
		return (iso_corrupt(error,
		    "ISO 9660 field copies in the two byte orders differ"));
	return (FSIMG_OK);
}

fsimg_status_t
iso_u32both(const unsigned char *field, uint32_t *value, fsimg_error_t *error)
{
	*value = fsimg_load_le32(field);
	if (*value != fsimg_load_be32(field + 4))
		return (iso_corrupt(error,
		    "ISO 9660 field copies in the two byte orders differ"));
	return (FSIMG_OK);
}

/* Convert validated calendar fields at a UTC offset of offset 15-minute
 * units into a timestamp with that offset, or fail as corrupt.
 */
static fsimg_status_t
date_timestamp(unsigned year, unsigned month, unsigned day, unsigned hour,
    unsigned minute, unsigned second, int offset, fsimg_timestamp_t *out,
    fsimg_error_t *error)
{
	int64_t days;

	if (hour > 23 || minute > 59 || second > 59 || offset < OFFSET_MIN ||
	    offset > OFFSET_MAX || !fsimg_civil_days(year, month, day, &days))
		return (iso_corrupt(error, "invalid ISO 9660 date"));
	out->seconds = days * 86400 + hour * 3600 + minute * 60 + second -
	    (int64_t)offset * 15 * 60;
	out->nanoseconds = 0;
	out->basis = FSIMG_TIME_OFFSET;
	out->offset_minutes = offset * 15;
	return (FSIMG_OK);
}

/* The seven-byte recording date of directory records: years since 1900,
 * month, day, hour, minute, second and a signed offset. Seven zero bytes
 * mean absent.
 */
fsimg_status_t
iso_date7_decode(const unsigned char *raw, bool *present,
    fsimg_timestamp_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	*present = false;
	bzero(out, sizeof(*out));
	if (iso_bytes_zero(raw, DATE7_BYTES))
		return (FSIMG_OK);
	status = date_timestamp(1900u + raw[0], raw[1], raw[2], raw[3], raw[4],
	    raw[5], (int8_t)raw[6], out, error);
	if (status != FSIMG_OK)
		return (status);
	out->precision_ns = 1000000000u;
	*present = true;
	return (FSIMG_OK);
}

static unsigned
digits(const unsigned char *raw, size_t count)
{
	unsigned value = 0;

	for (size_t i = 0; i < count; i++)
		value = value * 10 + (unsigned)(raw[i] - '0');
	return (value);
}

/* The seventeen-byte descriptor date: sixteen ASCII digits
 * YYYYMMDDHHMMSScc, cc in hundredths of a second, then the offset byte.
 * Sixteen '0' digits or sixteen zero bytes with a zero offset mean absent.
 */
fsimg_status_t
iso_date17_decode(const unsigned char *raw, bool *present,
    fsimg_timestamp_t *out, fsimg_error_t *error)
{
	static const char unset[] = "0000000000000000";
	unsigned year;
	fsimg_status_t status;

	*present = false;
	bzero(out, sizeof(*out));
	if (raw[DATE17_DIGITS] == 0 &&
	    (bcmp(raw, unset, DATE17_DIGITS) == 0 ||
	    iso_bytes_zero(raw, DATE17_DIGITS)))
		return (FSIMG_OK);
	for (size_t i = 0; i < DATE17_DIGITS; i++) {
		if (raw[i] < '0' || raw[i] > '9')
			return (iso_corrupt(error, "invalid ISO 9660 date"));
	}
	year = digits(raw, 4);
	if (year == 0)
		return (iso_corrupt(error, "invalid ISO 9660 date"));
	status = date_timestamp(year, digits(raw + 4, 2), digits(raw + 6, 2),
	    digits(raw + 8, 2), digits(raw + 10, 2), digits(raw + 12, 2),
	    (int8_t)raw[DATE17_DIGITS], out, error);
	if (status != FSIMG_OK)
		return (status);
	out->nanoseconds = digits(raw + 14, 2) * 10000000u;
	out->precision_ns = 10000000u;
	*present = true;
	return (FSIMG_OK);
}

/* The identity signature at the start of block 16: the standard identifier
 * CD001 after a type in the profile table.
 */
bool
iso_identity(const unsigned char *descriptor)
{
	uint8_t type = descriptor[VD_TYPE];

	return (bcmp(descriptor + VD_IDENTIFIER, "CD001", 5) == 0 &&
	    (type <= ISO_VD_PARTITION || type == ISO_VD_TERMINATOR));
}

/* The probe's check of a primary descriptor: both copies of the logical
 * block size and the volume space size, a nonzero size, and the one block
 * size the profile accepts.
 */
fsimg_status_t
iso_geometry_decode(const unsigned char *descriptor, uint32_t *space_size,
    fsimg_error_t *error)
{
	uint16_t block_size;
	fsimg_status_t status;

	status = iso_u32both(descriptor + VD_SPACE_SIZE, space_size, error);
	if (status == FSIMG_OK)
		status = iso_u16both(descriptor + VD_BLOCK_SIZE, &block_size,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (*space_size == 0)
		return (iso_corrupt(error, "ISO 9660 volume space size is zero"));
	if (block_size != ISO_BLOCK_BYTES)
		return (iso_unsupported(error,
		    "ISO 9660 logical block size is not 2048 bytes"));
	return (FSIMG_OK);
}

/* The fields a primary descriptor and a Joliet descriptor share: geometry,
 * path tables, the root record, the label and the four dates.
 */
static fsimg_status_t
volume_decode(const unsigned char *raw, iso_descriptor_t *out,
    fsimg_error_t *error)
{
	fsimg_timestamp_t date;
	uint16_t set_size, sequence;
	bool present;
	fsimg_status_t status;

	status = iso_geometry_decode(raw, &out->volume_space_size, error);
	if (status == FSIMG_OK)
		status = iso_u16both(raw + VD_SET_SIZE, &set_size, error);
	if (status == FSIMG_OK)
		status = iso_u16both(raw + VD_SEQUENCE, &sequence, error);
	if (status == FSIMG_OK)
		status = iso_u32both(raw + VD_PATH_TABLE_SIZE,
		    &out->path_table_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (set_size != 1 || sequence != 1)
		return (iso_unsupported(error,
		    "ISO 9660 volume belongs to a multi-volume set"));
	if (raw[VD_STRUCTURE_VERSION] != 1)
		return (iso_unsupported(error,
		    "unsupported ISO 9660 file structure version"));
	out->path_table_l = fsimg_load_le32(raw + VD_PATH_TABLE_L);
	out->path_table_l_copy = fsimg_load_le32(raw + VD_PATH_TABLE_L_COPY);
	out->path_table_m = fsimg_load_be32(raw + VD_PATH_TABLE_M);
	out->path_table_m_copy = fsimg_load_be32(raw + VD_PATH_TABLE_M_COPY);
	bcopy(raw + VD_LABEL, out->label, ISO_LABEL_BYTES);
	for (unsigned i = 0; i < 4; i++) {
		status = iso_date17_decode(raw + VD_DATES + i * DATE17_BYTES,
		    &present, &date, error);
		if (status != FSIMG_OK)
			return (status);
	}

	/* The root record: 34 bytes, the identifier 0x00, a directory. */
	if (raw[VD_ROOT + DR_LENGTH] != ISO_RECORD_MIN)
		return (iso_corrupt(error,
		    "ISO 9660 root directory record is not 34 bytes"));
	status = iso_record_decode(raw, VD_ROOT, &out->root, error);
	if (status != FSIMG_OK)
		return (status);
	if (out->root.identifier_length != 1 ||
	    raw[VD_ROOT + DR_IDENTIFIER] != 0x00 ||
	    (out->root.flags & ISO_FLAG_DIRECTORY) == 0)
		return (iso_corrupt(error,
		    "ISO 9660 root record does not name a directory"));
	return (FSIMG_OK);
}

/* The Joliet level a supplementary descriptor's escape sequences declare:
 * %/@, %/C or %/E followed by zeros. Zero for any other supplementary
 * descriptor, which is framed and ignored.
 */
static unsigned
joliet_level(const unsigned char *escapes)
{
	static const unsigned char finals[] = { '@', 'C', 'E' };

	if (escapes[0] != '%' || escapes[1] != '/' ||
	    !iso_bytes_zero(escapes + 3, ISO_ESCAPE_BYTES - 3))
		return (0);
	for (unsigned i = 0; i < sizeof(finals); i++) {
		if (escapes[2] == finals[i])
			return (i + 1);
	}
	return (0);
}

/* Decode the volume descriptor at block number under the descriptor scan
 * rule and the profile's type and version tables. A primary and a Joliet
 * descriptor are decoded in full; a boot record, a partition descriptor,
 * an enhanced or other supplementary descriptor is framed and ignored.
 */
fsimg_status_t
iso_descriptor_decode(const unsigned char *raw, uint32_t block,
    iso_descriptor_t *out, fsimg_error_t *error)
{
	bzero(out, sizeof(*out));
	out->block = block;
	out->type = raw[VD_TYPE];
	out->version = raw[VD_VERSION];
	if (bcmp(raw + VD_IDENTIFIER, "CD001", 5) != 0)
		return (iso_corrupt(error,
		    "ISO 9660 volume descriptor lacks CD001"));
	if (out->type == ISO_VD_SUPPLEMENTARY && out->version == 2)
		return (FSIMG_OK);	/* enhanced */
	if (out->version != 1 || (out->type > ISO_VD_PARTITION &&
	    out->type != ISO_VD_TERMINATOR))
		return (iso_unsupported(error,
		    "unsupported ISO 9660 volume descriptor type or version"));
	switch (out->type) {
	case ISO_VD_PRIMARY:
		return (volume_decode(raw, out, error));
	case ISO_VD_SUPPLEMENTARY:
		out->joliet_level = joliet_level(raw + VD_ESCAPES);
		if (out->joliet_level == 0)
			return (FSIMG_OK);
		if (raw[VD_FLAGS] != 0)
			return (iso_corrupt(error,
			    "ISO 9660 Joliet descriptor has volume flags"));
		return (volume_decode(raw, out, error));
	case ISO_VD_TERMINATOR:
		if (!iso_bytes_zero(raw + VD_FLAGS, ISO_BLOCK_BYTES - VD_FLAGS))
			return (iso_corrupt(error,
			    "ISO 9660 descriptor set terminator is not zero"));
		return (FSIMG_OK);
	default:
		return (FSIMG_OK);
	}
}

/* Decode the directory record at offset of a 2048-byte block, whose first
 * byte the caller found nonzero. The framing rule: LEN_DR even, at least
 * 34 and 33 + LEN_FI + pad, ending within the block; LEN_FI at least 1;
 * volume sequence number 1. Then both copies of the extent and data
 * length, the recording date, and the flag table, which refuses associated
 * files, record formats and interleaving and rejects reserved bits, a
 * multi-extent directory and an extended attribute record on a directory.
 */
fsimg_status_t
iso_record_decode(const unsigned char *block, size_t offset,
    iso_record_t *out, fsimg_error_t *error)
{
	const unsigned char *raw = block + offset;
	unsigned length, identifier, used;
	uint16_t sequence;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	if (offset >= ISO_BLOCK_BYTES)
		return (iso_corrupt(error, "ISO 9660 record is outside its block"));
	length = raw[DR_LENGTH];
	if (length % 2 != 0 || length < ISO_RECORD_MIN ||
	    length > ISO_BLOCK_BYTES - offset)
		return (iso_corrupt(error,
		    "ISO 9660 directory record is misframed"));
	identifier = raw[DR_IDENTIFIER_LENGTH];
	/* An even identifier is followed by one pad byte. */
	used = DR_IDENTIFIER + identifier + (identifier % 2 == 0 ? 1 : 0);
	if (identifier == 0 || used > length)
		return (iso_corrupt(error,
		    "ISO 9660 record identifier does not fit its record"));
	status = iso_u16both(raw + DR_SEQUENCE, &sequence, error);
	if (status == FSIMG_OK)
		status = iso_u32both(raw + DR_EXTENT, &out->extent, error);
	if (status == FSIMG_OK)
		status = iso_u32both(raw + DR_DATA_LENGTH, &out->data_length,
		    error);
	if (status == FSIMG_OK)
		status = iso_date7_decode(raw + DR_DATE, &out->date_present,
		    &out->date, error);
	if (status != FSIMG_OK)
		return (status);
	if (sequence != 1)
		return (iso_unsupported(error,
		    "ISO 9660 record names another volume of a set"));

	out->length = (uint8_t)length;
	out->ea_blocks = raw[DR_EA_BLOCKS];
	out->flags = raw[DR_FLAGS];
	out->identifier_length = (uint8_t)identifier;
	out->system_use_offset = (uint8_t)used;
	out->system_use_length = (uint8_t)(length - used);
	if ((out->flags & ISO_FLAG_RESERVED) != 0)
		return (iso_corrupt(error, "ISO 9660 record sets reserved flags"));
	if ((out->flags & (ISO_FLAG_RECORD | ISO_FLAG_PROTECTION)) != 0 &&
	    out->ea_blocks == 0)
		return (iso_corrupt(error, "ISO 9660 record format or protection "
		    "flag without an extended attribute record"));
	if ((out->flags & (ISO_FLAG_RECORD | ISO_FLAG_PROTECTION)) != 0)
		return (iso_unsupported(error,
		    "ISO 9660 record format or protection is unsupported"));
	if ((out->flags & ISO_FLAG_ASSOCIATED) != 0)
		return (iso_unsupported(error,
		    "ISO 9660 associated files are unsupported"));
	if (raw[DR_UNIT_SIZE] != 0 || raw[DR_GAP_SIZE] != 0)
		return (iso_unsupported(error,
		    "ISO 9660 interleaved files are unsupported"));
	if ((out->flags & ISO_FLAG_DIRECTORY) != 0) {
		if ((out->flags & ISO_FLAG_MULTI_EXTENT) != 0)
			return (iso_corrupt(error,
			    "ISO 9660 directory record has the multi-extent flag"));
		if (out->ea_blocks != 0)
			return (iso_unsupported(error, "ISO 9660 directory "
			    "with an extended attribute record"));
	}
	return (FSIMG_OK);
}
