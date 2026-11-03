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
 * Pure decoders and encoders for ISO 9660 structures: both-byte-order
 * fields, the two date forms, volume descriptors, directory records and
 * path-table records. They perform no I/O. Decoders fill their outputs
 * with validated values only, every failure naming its class
 * (specs/design-iso9660.md); encoders are their exact inverses for the
 * values the writer profile emits and initialize every byte they cover.
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
#define VD_SYSTEM		8u
#define VD_VOLUME_SET		190u
#define VD_PUBLISHER		318u
#define VD_PREPARER		446u
#define VD_APPLICATION		574u
#define VD_COPYRIGHT		702u	/* three 37-byte file identifiers */
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
#define DR_IDENTIFIER		ISO_RECORD_IDENTIFIER

#define DATE7_BYTES		ISO_DATE7_BYTES
#define DATE17_BYTES		ISO_DATE17_BYTES
#define DATE17_DIGITS		16u
#define OFFSET_MIN		(-48)	/* 15-minute units west of UTC */
#define OFFSET_MAX		52

const iso_descriptor_field_t iso_descriptor_fields[ISO_DESCRIPTOR_FIELDS] = {
	{ .key = "volume_set", .setting = "volset", .slot = 0,
	    .offset = 0,
	    .width = 128, .class = ISO_CLASS_A_CHARACTERS },
	{ .key = "publisher", .setting = "publisher", .slot = 1,
	    .offset = VD_PUBLISHER - VD_VOLUME_SET,
	    .width = 128, .class = ISO_CLASS_A_CHARACTERS },
	{ .key = "preparer", .setting = "preparer", .slot = 2,
	    .offset = VD_PREPARER - VD_VOLUME_SET,
	    .width = 128, .class = ISO_CLASS_A_CHARACTERS },
	{ .key = "application", .setting = "application", .slot = 3,
	    .offset = VD_APPLICATION - VD_VOLUME_SET,
	    .width = 128, .class = ISO_CLASS_A_CHARACTERS,
	    .unset_text = "FSIMG" },
	{ .key = "copyright_file", .setting = "copyright", .slot = 4,
	    .offset = VD_COPYRIGHT - VD_VOLUME_SET,
	    .width = 37, .class = ISO_CLASS_FILE_IDENTIFIER },
	{ .key = "abstract_file", .setting = "abstract", .slot = 5,
	    .offset = VD_COPYRIGHT + 37 - VD_VOLUME_SET,
	    .width = 37, .class = ISO_CLASS_FILE_IDENTIFIER },
	{ .key = "biblio_file", .setting = "biblio", .slot = 6,
	    .offset = VD_COPYRIGHT + 74 - VD_VOLUME_SET,
	    .width = 37, .class = ISO_CLASS_FILE_IDENTIFIER },
	{ .key = "created", .setting = "created", .slot = 0,
	    .offset = VD_DATES - VD_VOLUME_SET,
	    .width = DATE17_BYTES, .class = ISO_CLASS_DATE, .unset_dated = true },
	{ .key = "modified", .setting = "modified", .slot = 1,
	    .offset = VD_DATES + 17 - VD_VOLUME_SET,
	    .width = DATE17_BYTES, .class = ISO_CLASS_DATE, .unset_dated = true },
	{ .key = "expires", .setting = "expires", .slot = 2,
	    .offset = VD_DATES + 34 - VD_VOLUME_SET,
	    .width = DATE17_BYTES, .class = ISO_CLASS_DATE },
	{ .key = "effective", .setting = "effective", .slot = 3,
	    .offset = VD_DATES + 51 - VD_VOLUME_SET,
	    .width = DATE17_BYTES, .class = ISO_CLASS_DATE }
};
_Static_assert(ISO_DESCRIPTOR_FIELD_BASE == VD_VOLUME_SET &&
    VD_DATES + 4 * DATE17_BYTES - VD_VOLUME_SET == ISO_DESCRIPTOR_BYTES &&
    VD_STRUCTURE_VERSION - VD_VOLUME_SET == ISO_DESCRIPTOR_BYTES,
    "the descriptor fields fill bytes 190 to 880");

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

/* The reader profile's geometry: a nonzero volume of 2048-byte blocks. */
static fsimg_status_t
geometry_profile_check(uint32_t space_size, uint16_t block_size,
    fsimg_error_t *error)
{
	if (space_size == 0)
		return (iso_corrupt(error, "ISO 9660 volume space size is zero"));
	if (block_size != ISO_BLOCK_BYTES)
		return (iso_unsupported(error,
		    "ISO 9660 logical block size is not 2048 bytes"));
	return (FSIMG_OK);
}

/* The probe's check of a primary descriptor: both copies of the logical
 * block size and the volume space size, and the profile's geometry.
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
	return (geometry_profile_check(*space_size, block_size, error));
}

/* The geometry fields a primary and a Joliet descriptor share, both copies
 * of each agreeing; whether the values are acceptable is judged by the
 * caller.
 */
static fsimg_status_t
geometry_decode(const unsigned char *raw, iso_descriptor_t *out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = iso_u32both(raw + VD_SPACE_SIZE, &out->volume_space_size,
	    error);
	if (status == FSIMG_OK)
		status = iso_u16both(raw + VD_BLOCK_SIZE, &out->block_size,
		    error);
	if (status == FSIMG_OK)
		status = iso_u16both(raw + VD_SET_SIZE, &out->set_size, error);
	if (status == FSIMG_OK)
		status = iso_u16both(raw + VD_SEQUENCE, &out->sequence, error);
	return (status);
}

/* The primary descriptor's geometry against the reader profile, before
 * anything else of the descriptor is decoded: the profile's geometry and a
 * volume set of one. A Joliet descriptor is held to agreement with these
 * values instead (volume.c), so its departure is a contradiction rather
 * than an unsupported profile.
 */
static fsimg_status_t
primary_geometry_check(const iso_descriptor_t *primary, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = geometry_profile_check(primary->volume_space_size,
	    primary->block_size, error);
	if (status != FSIMG_OK)
		return (status);
	if (primary->set_size != 1 || primary->sequence != 1)
		return (iso_unsupported(error,
		    "ISO 9660 volume belongs to a multi-volume set"));
	return (FSIMG_OK);
}

/* The rest of a primary or Joliet descriptor after its geometry: path
 * tables, the file structure version, the label, the four dates and the
 * root record.
 */
static fsimg_status_t
volume_decode(const unsigned char *raw, iso_descriptor_t *out,
    fsimg_error_t *error)
{
	fsimg_timestamp_t date;
	bool present;
	fsimg_status_t status;

	status = iso_u32both(raw + VD_PATH_TABLE_SIZE, &out->path_table_bytes,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (raw[VD_STRUCTURE_VERSION] != 1)
		return (iso_unsupported(error,
		    "unsupported ISO 9660 file structure version"));
	out->path_table_l = fsimg_load_le32(raw + VD_PATH_TABLE_L);
	out->path_table_l_copy = fsimg_load_le32(raw + VD_PATH_TABLE_L_COPY);
	out->path_table_m = fsimg_load_be32(raw + VD_PATH_TABLE_M);
	out->path_table_m_copy = fsimg_load_be32(raw + VD_PATH_TABLE_M_COPY);
	bcopy(raw + VD_LABEL, out->label, ISO_LABEL_BYTES);
	bcopy(raw + ISO_DESCRIPTOR_FIELD_BASE, out->fields, ISO_DESCRIPTOR_BYTES);
	/* The four volume dates describe the volume to a person and no file
	 * depends on them, so a malformed one reads as absent rather than
	 * refusing the volume; full verification fails on malformed_date.
	 * Rock Ridge timestamps and recording dates, which are entry metadata,
	 * stay strict.
	 */
	for (unsigned i = 0; i < ISO_DESCRIPTOR_DATES; i++) {
		if (iso_date17_decode(raw + VD_DATES + i * DATE17_BYTES,
		    &present, &date, NULL) != FSIMG_OK) {
			out->malformed_date = true;
			out->date_present[i] = false;
			out->date_seconds[i] = 0;
			continue;
		}
		out->date_present[i] = present;
		out->date_seconds[i] = date.seconds;
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
	fsimg_status_t status;

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
		status = geometry_decode(raw, out, error);
		if (status == FSIMG_OK)
			status = primary_geometry_check(out, error);
		if (status != FSIMG_OK)
			return (status);
		return (volume_decode(raw, out, error));
	case ISO_VD_SUPPLEMENTARY:
		out->joliet_level = joliet_level(raw + VD_ESCAPES);
		if (out->joliet_level == 0)
			return (FSIMG_OK);
		if (raw[VD_FLAGS] != 0)
			return (iso_corrupt(error,
			    "ISO 9660 Joliet descriptor has volume flags"));
		status = geometry_decode(raw, out, error);
		if (status != FSIMG_OK)
			return (status);
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
 * the pad byte after an even LEN_FI is zero; volume sequence number 1.
 * Then both copies of the extent and data length, the recording date, and
 * the flag table, which refuses associated files, record formats and interleaving and rejects reserved bits, a
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
	if (identifier % 2 == 0 && raw[DR_IDENTIFIER + identifier] != 0)
		return (iso_corrupt(error,
		    "ISO 9660 record identifier pad byte is not zero"));
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

/* Decode the header of a path-table record, type M when big_endian, else
 * type L. Its identifier follows; the caller checks the fields.
 */
void
iso_path_header_decode(const unsigned char *raw, bool big_endian,
    iso_path_record_t *out)
{
	out->length = raw[0];
	out->ea_length = raw[1];
	out->extent = big_endian ? fsimg_load_be32(raw + 2) :
	    fsimg_load_le32(raw + 2);
	out->parent = big_endian ? fsimg_load_be16(raw + 6) :
	    fsimg_load_le16(raw + 6);
}

/*
 * Encoders.
 */

void
iso_u16both_put(unsigned char *field, uint16_t value)
{
	fsimg_store_le16(field, value);
	fsimg_store_be16(field + 2, value);
}

void
iso_u32both_put(unsigned char *field, uint32_t value)
{
	fsimg_store_le32(field, value);
	fsimg_store_be32(field + 4, value);
}

/* The proleptic Gregorian date and time of day of UTC seconds since
 * 1970-01-01, the inverse of fsimg_civil_days(): days are counted in
 * 400-year eras of 146097 days from 0000-03-01, so no loop runs over the
 * years. Valid for years 0 through 9999.
 */
static void
civil_from_seconds(int64_t seconds, unsigned *year, unsigned *month,
    unsigned *day, unsigned *clock)
{
	int64_t days, era, day_of_era, year_of_era, day_of_year, shifted;

	days = seconds / 86400;
	if (seconds % 86400 < 0)
		days--;
	*clock = (unsigned)(seconds - days * 86400);
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	day_of_era = days - era * 146097;
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
	    day_of_era / 146096) / 365;
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
	    year_of_era / 100);
	shifted = (5 * day_of_year + 2) / 153;
	*day = (unsigned)(day_of_year - (153 * shifted + 2) / 5 + 1);
	*month = (unsigned)(shifted < 10 ? shifted + 3 : shifted - 9);
	*year = (unsigned)(year_of_era + era * 400 + (*month <= 2 ? 1 : 0));
}

/* The seven-byte recording date of UTC seconds, at offset 0. False, with
 * raw untouched, outside 1900-01-01 00:00:00 through 2155-12-31 23:59:59,
 * the years the format's one-byte field counts from 1900.
 */
bool
iso_date7_encode(int64_t seconds, unsigned char *raw)
{
	unsigned year, month, day, clock;

	if (seconds < ISO_DATE7_MIN || seconds > ISO_DATE7_MAX)
		return (false);
	civil_from_seconds(seconds, &year, &month, &day, &clock);
	raw[0] = (unsigned char)(year - 1900);
	raw[1] = (unsigned char)month;
	raw[2] = (unsigned char)day;
	raw[3] = (unsigned char)(clock / 3600);
	raw[4] = (unsigned char)(clock / 60 % 60);
	raw[5] = (unsigned char)(clock % 60);
	raw[6] = 0;
	return (true);
}

/* The low width decimal digits of value, zero-padded, into out[0..width). */
static void
store_digits(unsigned char *out, unsigned value, unsigned width)
{
	for (unsigned d = width; d > 0; d--) {
		out[d - 1] = (unsigned char)('0' + value % 10);
		value /= 10;
	}
}

/* The seventeen-byte descriptor date of whole UTC seconds within years 1
 * through 9999: sixteen ASCII digits with zero hundredths, then a
 * zero offset.
 */
bool
iso_date17_encode(int64_t seconds, unsigned char *raw)
{
	unsigned year, month, day, clock;

	if (seconds < ISO_DATE17_MIN || seconds > ISO_DATE17_MAX)
		return (false);
	civil_from_seconds(seconds, &year, &month, &day, &clock);
	store_digits(raw, year, 4);
	store_digits(raw + 4, month, 2);
	store_digits(raw + 6, day, 2);
	store_digits(raw + 8, clock / 3600, 2);
	store_digits(raw + 10, clock / 60 % 60, 2);
	store_digits(raw + 12, clock % 60, 2);
	store_digits(raw + 14, 0, 2);		/* hundredths */
	raw[DATE17_DIGITS] = 0;
	return (true);
}

/* An absent descriptor date: sixteen '0' digits and a zero offset. */
void
iso_date17_absent(unsigned char *raw)
{
	memset(raw, '0', DATE17_DIGITS);
	raw[DATE17_DIGITS] = 0;
}

/* The UTC calendar form of an instant, YYYY-MM-DD HH:MM:SS, into out of
 * ISO_DATE_TEXT_BYTES: the form a listing shows a modification time in.
 * False, with out untouched, outside years 1 through 9999, where a
 * descriptor date whose offset moves its UTC year to 0 lands.
 */
bool
iso_date17_format(int64_t seconds, char *out)
{
	static const char form[] = "0000-00-00 00:00:00";
	unsigned char *text = (unsigned char *)out;
	unsigned year, month, day, clock;

	_Static_assert(sizeof(form) == ISO_DATE_TEXT_BYTES,
	    "the date text and its NUL fill ISO_DATE_TEXT_BYTES");
	if (seconds < ISO_DATE17_MIN || seconds > ISO_DATE17_MAX)
		return (false);
	civil_from_seconds(seconds, &year, &month, &day, &clock);
	bcopy(form, text, sizeof(form));
	store_digits(text, year, 4);
	store_digits(text + 5, month, 2);
	store_digits(text + 8, day, 2);
	store_digits(text + 11, clock / 3600, 2);
	store_digits(text + 14, clock / 60 % 60, 2);
	store_digits(text + 17, clock % 60, 2);
	return (true);
}

/* An identifier field of width bytes holding ASCII text padded with
 * spaces: one byte per character, or for Joliet big-endian UCS-2 with
 * U+0020 padding and, for an odd width, a final zero byte.
 */
static void
text_field_put(unsigned char *field, size_t width, const char *text,
    bool joliet)
{
	size_t length = strlen(text);

	if (!joliet) {
		memset(field, ' ', width);
		bcopy(text, field, length < width ? length : width);
		return;
	}
	for (size_t i = 0; i < width / 2; i++)
		fsimg_store_be16(field + 2 * i,
		    i < length ? (unsigned char)text[i] : ' ');
	if (width % 2 != 0)
		field[width - 1] = 0;
}

/* The length of a record with an identifier of identifier_length bytes
 * and system_use_length bytes of system use: the identifier's pad byte
 * when its length is even, then one zero byte when the total is odd, so
 * every record length is even.
 */
size_t
iso_record_length(size_t identifier_length, size_t system_use_length)
{
	size_t length;

	length = DR_IDENTIFIER + identifier_length +
	    (identifier_length % 2 == 0 ? 1 : 0) + system_use_length;
	return (length + length % 2);
}

/* Encode one directory record into out, which holds
 * iso_record_length() bytes: the extent and data length in both orders,
 * the recording date, flags, volume sequence number 1, the identifier,
 * its pad byte and the system use bytes, then a zero byte when needed
 * for an even length. Returns that length.
 */
size_t
iso_record_encode(const iso_record_fields_t *fields, unsigned char *out)
{
	size_t length, used;

	length = iso_record_length(fields->identifier_length,
	    fields->system_use_length);
	bzero(out, length);
	out[DR_LENGTH] = (unsigned char)length;
	iso_u32both_put(out + DR_EXTENT, fields->extent);
	iso_u32both_put(out + DR_DATA_LENGTH, fields->data_length);
	bcopy(fields->date, out + DR_DATE, DATE7_BYTES);
	out[DR_FLAGS] = fields->flags;
	iso_u16both_put(out + DR_SEQUENCE, 1);
	out[DR_IDENTIFIER_LENGTH] = (unsigned char)fields->identifier_length;
	bcopy(fields->identifier, out + DR_IDENTIFIER,
	    fields->identifier_length);
	used = DR_IDENTIFIER + fields->identifier_length +
	    (fields->identifier_length % 2 == 0 ? 1 : 0);
	if (fields->system_use_length != 0)
		bcopy(fields->system_use, out + used,
		    fields->system_use_length);
	return (length);
}

/* Whether c is an a-character: an uppercase letter, a digit, an underscore,
 * a space or one of ! " % & ' ( ) * + , - . / : ; < = > ?.
 */
static bool
a_character(unsigned char c)
{
	return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	    (c != '\0' && strchr("_ !\"%&'()*+,-./:;<=>?", c) != NULL));
}

/* Whether c is a character of a file identifier field: a d-character,
 * a separator dot or semicolon, or a space.
 */
static bool
file_identifier_character(unsigned char c)
{
	return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
	    c == '.' || c == ';' || c == ' ');
}

/* Encode text into the width-byte primary field of a text class, padded
 * with spaces. False, leaving field untouched, when a character is outside
 * the class or the text is longer than the field's capacity, width / 2:
 * the Joliet descriptor mirrors the field as UCS-2 units, which hold half
 * as many characters (a 37-byte field holds 18 and a zero byte).
 */
bool
iso_text_encode(iso_field_class_t class, const char *text,
    unsigned char *field, size_t width)
{
	size_t length = strlen(text);
	bool (*member)(unsigned char) = class == ISO_CLASS_A_CHARACTERS ?
	    a_character : file_identifier_character;

	if (length > width / 2)
		return (false);
	for (size_t i = 0; i < length; i++) {
		if (!member((unsigned char)text[i]))
			return (false);
	}
	memset(field, ' ', width);
	bcopy(text, field, length);
	return (true);
}

/* Widen the first width / 2 bytes of a primary text field into big-endian
 * UCS-2 units, which the classes keep ASCII; an odd width ends in a zero
 * byte.
 */
static void
text_field_widen(const unsigned char *primary, size_t width,
    unsigned char *field)
{
	for (size_t i = 0; i < width / 2; i++)
		fsimg_store_be16(field + 2 * i, primary[i]);
	if (width % 2 != 0)
		field[width - 1] = 0;
}

/* Copy the descriptor image of the eleven text and date fields into a
 * descriptor at out: the primary descriptor takes it as it is and the
 * Joliet descriptor widens each text, with the dates unchanged.
 */
static void
descriptor_fields_put(const unsigned char *image, bool joliet,
    unsigned char *out)
{
	const iso_descriptor_field_t *row;

	for (unsigned i = 0; i < ISO_DESCRIPTOR_FIELDS; i++) {
		row = &iso_descriptor_fields[i];
		if (joliet && row->class != ISO_CLASS_DATE)
			text_field_widen(image + row->offset, row->width,
			    out + ISO_DESCRIPTOR_FIELD_BASE + row->offset);
		else
			bcopy(image + row->offset, out + ISO_DESCRIPTOR_FIELD_BASE +
			    row->offset, row->width);
	}
}

/* Encode a primary (joliet false) or Joliet level 3 supplementary
 * descriptor into the 2048 bytes at out. The text and date fields come
 * from the descriptor image; reserved bytes and the application use field
 * are zero.
 */
void
iso_volume_descriptor_encode(const iso_volume_fields_t *fields,
    unsigned char *out)
{
	iso_record_fields_t root;
	bool joliet = fields->joliet;

	bzero(out, ISO_BLOCK_BYTES);
	out[VD_TYPE] = joliet ? ISO_VD_SUPPLEMENTARY : ISO_VD_PRIMARY;
	bcopy("CD001", out + VD_IDENTIFIER, 5);
	out[VD_VERSION] = 1;
	text_field_put(out + VD_SYSTEM, 32, "", joliet);
	bcopy(fields->label, out + VD_LABEL, ISO_LABEL_BYTES);
	iso_u32both_put(out + VD_SPACE_SIZE, fields->volume_space_size);
	if (joliet) {
		out[VD_ESCAPES] = '%';
		out[VD_ESCAPES + 1] = '/';
		out[VD_ESCAPES + 2] = 'E';
	}
	iso_u16both_put(out + VD_SET_SIZE, 1);
	iso_u16both_put(out + VD_SEQUENCE, 1);
	iso_u16both_put(out + VD_BLOCK_SIZE, ISO_BLOCK_BYTES);
	iso_u32both_put(out + VD_PATH_TABLE_SIZE, fields->path_table_bytes);
	fsimg_store_le32(out + VD_PATH_TABLE_L, fields->path_table_l);
	fsimg_store_be32(out + VD_PATH_TABLE_M, fields->path_table_m);

	bzero(&root, sizeof(root));
	root.identifier = (const unsigned char *)"";
	root.identifier_length = 1;
	root.extent = fields->root_extent;
	root.data_length = fields->root_length;
	root.flags = ISO_FLAG_DIRECTORY;
	bcopy(fields->root_date, root.date, DATE7_BYTES);
	iso_record_encode(&root, out + VD_ROOT);

	descriptor_fields_put(fields->descriptor, joliet, out);
	out[VD_STRUCTURE_VERSION] = 1;
}

/* The descriptor set terminator: type 255, CD001, version 1, zeros. */
void
iso_terminator_encode(unsigned char *out)
{
	bzero(out, ISO_BLOCK_BYTES);
	out[VD_TYPE] = ISO_VD_TERMINATOR;
	bcopy("CD001", out + VD_IDENTIFIER, 5);
	out[VD_VERSION] = 1;
}

/* Encode a path-table record, type M when big_endian, else type L, into
 * out: the header, the identifier and a zero pad byte when its length is
 * odd. Returns the record's length.
 */
size_t
iso_path_record_encode(const iso_path_record_t *record, bool big_endian,
    unsigned char *out)
{
	size_t length;

	length = ISO_PATH_HEADER_BYTES + record->length + record->length % 2;
	bzero(out, length);
	out[0] = record->length;
	out[1] = record->ea_length;
	if (big_endian) {
		fsimg_store_be32(out + 2, record->extent);
		fsimg_store_be16(out + 6, record->parent);
	} else {
		fsimg_store_le32(out + 2, record->extent);
		fsimg_store_le16(out + 6, record->parent);
	}
	bcopy(record->identifier, out + ISO_PATH_HEADER_BYTES, record->length);
	return (length);
}
