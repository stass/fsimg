/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "layer.h"
#include "unicode.h"

#define BASIC_GUID	"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define BASIC_NAME	"Microsoft basic data"

/*
 * The partition types fsimg names. A name reports the first entry whose
 * GUID or byte a slot carries, so the generic entries come first: a GPT
 * basic-data partition reports as basic and MBR 0x07 as basic, while 0x01,
 * 0x06 and 0x0C report the FAT variant their convention names.
 *
 * Sources: the GUIDs and default names are those of gptfdisk's parttypes.cc;
 * the QNX6 (Power-Safe) GUID CEF5A9AD-73BC-4601-89F3-CDEEEEE321A1 and MBR
 * types 177, 178 and 179, and the QNX4 types 77, 78 and 79, are those the
 * QNX Neutrino documentation lists (System Architecture, "Partitions";
 * io-blk.so and diskimage configuration).
 */
static const fsimg_partition_type_t partition_types[] = {
	{ "basic", BASIC_GUID, BASIC_NAME, { 0x07 } },
	{ "esp", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system partition",
	    { 0xef } },
	{ "fat12", BASIC_GUID, BASIC_NAME, { 0x01 } },
	{ "fat16", BASIC_GUID, BASIC_NAME, { 0x06, 0x04, 0x0e } },
	{ "fat32", BASIC_GUID, BASIC_NAME, { 0x0c, 0x0b } },
	{ "exfat", BASIC_GUID, BASIC_NAME, { 0x07 } },
	{ "linux", "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem",
	    { 0x83 } },
	{ "apfs", "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS", { 0 } },
	{ "hfs", "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS/HFS+",
	    { 0xaf } },
	{ "qnx6", "CEF5A9AD-73BC-4601-89F3-CDEEEEE321A1", "QNX6 Power-Safe",
	    { 0xb2, 0xb1, 0xb3 } },
	{ "qnx4", NULL, NULL, { 0x4d, 0x4e, 0x4f } }
};

#define TYPE_COUNT	(sizeof(partition_types) / sizeof(partition_types[0]))
#define MBR_TYPE_SLOTS	(sizeof(partition_types[0].mbr_types))

static bool hex_digit(char, unsigned *);
static bool type_carries_byte(const fsimg_partition_type_t *, uint8_t);
static bool type_carries_guid(const fsimg_partition_type_t *,
    const unsigned char *);
static void name_to_utf8(const uint16_t *, char *, size_t);

static bool
hex_digit(char c, unsigned *value)
{
	if (c >= '0' && c <= '9')
		*value = (unsigned)(c - '0');
	else if (c >= 'a' && c <= 'f')
		*value = (unsigned)(c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		*value = (unsigned)(c - 'A' + 10);
	else
		return (false);
	return (true);
}

/* RFC 4122 text: 32 hexadecimal digits in groups of 8, 4, 4, 4 and 12,
 * stored in textual byte order.
 */
bool
fsimg_guid_parse(const char *text, unsigned char *guid)
{
	unsigned high, low;
	size_t position;

	position = 0;
	for (unsigned i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			if (text[position] != '-')
				return (false);
			position++;
		}
		if (!hex_digit(text[position], &high) ||
		    !hex_digit(text[position + 1], &low))
			return (false);
		guid[i] = (unsigned char)(high << 4 | low);
		position += 2;
	}
	return (text[position] == '\0');
}

static bool
type_carries_byte(const fsimg_partition_type_t *type, uint8_t byte)
{
	for (size_t i = 0; i < MBR_TYPE_SLOTS && type->mbr_types[i] != 0; i++) {
		if (type->mbr_types[i] == byte)
			return (true);
	}
	return (false);
}

static bool
type_carries_guid(const fsimg_partition_type_t *type,
    const unsigned char *guid)
{
	unsigned char type_guid[16];

	return (type->gpt_guid != NULL && fsimg_guid_parse(type->gpt_guid,
	    type_guid) && bcmp(type_guid, guid, sizeof(type_guid)) == 0);
}

/* A type selector is a table name, GUID text, or "0x" and two hex digits
 * naming a nonzero MBR byte.
 */
fsimg_status_t
fsimg_partition_type_parse(const char *text, fsimg_type_selector_t *out,
    fsimg_error_t *error)
{
	unsigned high, low;

	bzero(out, sizeof(*out));
	for (size_t i = 0; i < TYPE_COUNT; i++) {
		if (strcmp(text, partition_types[i].name) == 0) {
			out->type = &partition_types[i];
			return (FSIMG_OK);
		}
	}
	if (fsimg_guid_parse(text, out->guid)) {
		out->literal_guid = true;
		return (FSIMG_OK);
	}
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X') &&
	    hex_digit(text[2], &high) && hex_digit(text[3], &low) &&
	    text[4] == '\0' && (high | low) != 0) {
		out->mbr_type = (uint8_t)(high << 4 | low);
		out->literal_byte = true;
		return (FSIMG_OK);
	}
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
	    "unknown partition type; use a type name, a GUID or an MBR byte"));
}

fsimg_status_t
fsimg_partition_type_match(const fsimg_type_selector_t *selector,
    fsimg_scheme_kind_t scheme, const fsimg_partition_record_t *record,
    bool *matches, fsimg_error_t *error)
{
	*matches = false;
	if (scheme == FSIMG_SCHEME_GPT) {
		if (selector->literal_byte)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "an MBR type byte cannot select a GPT partition"));
		if (selector->type != NULL && selector->type->gpt_guid == NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "the partition type has no GPT GUID"));
		*matches = selector->literal_guid ?
		    bcmp(selector->guid, record->type_guid, 16) == 0 :
		    type_carries_guid(selector->type, record->type_guid);
		return (FSIMG_OK);
	}
	if (selector->literal_guid)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "a GPT type GUID cannot select an MBR partition"));
	if (selector->type != NULL && selector->type->mbr_types[0] == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the partition type has no MBR byte"));
	*matches = selector->literal_byte ?
	    selector->mbr_type == record->mbr_type :
	    type_carries_byte(selector->type, record->mbr_type);
	return (FSIMG_OK);
}

const fsimg_partition_type_t *
fsimg_partition_type_find(const char *name)
{
	for (size_t i = 0; i < TYPE_COUNT; i++) {
		if (strcmp(name, partition_types[i].name) == 0)
			return (&partition_types[i]);
	}
	return (NULL);
}

/*
 * A GPT record takes the selected type's GUID: a table type needs one, and
 * a literal GUID is taken as given, with the conventional name of the
 * first table type carrying it. An MBR record takes the type's written
 * byte (its first) or the literal byte; an extended partition would need
 * logical partitions inside it and 0xEE claims the disk for GPT, so
 * neither can type a written partition.
 */
fsimg_status_t
fsimg_partition_type_assign(const fsimg_type_selector_t *selector,
    fsimg_scheme_kind_t scheme, fsimg_partition_record_t *record,
    const char **name, fsimg_error_t *error)
{
	static const unsigned char nil_guid[16];
	const fsimg_partition_type_t *type;
	uint8_t byte;

	*name = "";
	if (scheme == FSIMG_SCHEME_GPT) {
		if (selector->literal_byte)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "an MBR type byte cannot type a GPT partition"));
		if (selector->literal_guid) {
			/* A nil type marks an entry unused, so readers
			 * would not see the partition.
			 */
			if (bcmp(selector->guid, nil_guid, sizeof(nil_guid)) == 0)
				return (fsimg_error_set(error, FSIMG_EINVAL, 0,
				    FSIMG_OP_IMAGE,
				    "the nil GUID marks an unused GPT entry"));
			bcopy(selector->guid, record->type_guid, 16);
			for (size_t i = 0; i < TYPE_COUNT; i++) {
				type = &partition_types[i];
				if (type_carries_guid(type, selector->guid)) {
					*name = type->gpt_name;
					break;
				}
			}
			return (FSIMG_OK);
		}
		type = selector->type;
		if (type->gpt_guid == NULL || !fsimg_guid_parse(type->gpt_guid,
		    record->type_guid))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "the partition type has no GPT GUID"));
		*name = type->gpt_name;
		return (FSIMG_OK);
	}
	if (selector->literal_guid)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "a GPT type GUID cannot type an MBR partition"));
	byte = selector->literal_byte ? selector->mbr_type :
	    selector->type->mbr_types[0];
	if (byte == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the partition type has no MBR byte"));
	if (byte == 0x05 || byte == 0x0f || byte == 0x85 || byte == 0xee)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "extended and GPT protective types cannot be written"));
	record->mbr_type = byte;
	return (FSIMG_OK);
}

/* A GPT name as UTF-8 for display. Names are untrusted, so an unpaired
 * surrogate becomes U+FFFD instead of failing the listing; selection by
 * name compares the stored units, not this text.
 */
static void
name_to_utf8(const uint16_t *units, char *text, size_t capacity)
{
	static const uint16_t replacement = 0xfffd;
	const uint16_t *span;
	size_t length, span_length, used, written;

	length = 0;
	while (length < FSIMG_GPT_NAME_UNITS && units[length] != 0)
		length++;
	used = 0;
	for (size_t i = 0; i < length; i += span_length) {
		span = &units[i];
		span_length = 1;
		if (units[i] >= 0xd800 && units[i] <= 0xdbff && i + 1 < length &&
		    units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
			span_length = 2;
		else if (units[i] >= 0xd800 && units[i] <= 0xdfff)
			span = &replacement;
		/* 36 units need at most 108 bytes, so the text always fits. */
		if (fsimg_utf16_to_utf8(span, span == &replacement ? 1 : span_length,
		    text + used, capacity - 1 - used, &written, NULL) != FSIMG_OK)
			break;
		used += written;
	}
	text[used] = '\0';
}

/* The public record of a slot; the claim is the caller's. */
void
fsimg_partition_report(fsimg_scheme_kind_t scheme,
    const fsimg_partition_record_t *record, fsimg_partition_info_t *out)
{
	const char *type;

	bzero(out, sizeof(*out));
	out->index = record->index;
	if (!record->used)
		return;
	out->used = true;
	out->offset = record->offset;
	out->length = record->length;
	type = fsimg_partition_type_name(scheme, record);
	if (type != NULL)
		snprintf(out->type, sizeof(out->type), "%s", type);
	if (scheme == FSIMG_SCHEME_GPT) {
		bcopy(record->type_guid, out->type_guid, sizeof(out->type_guid));
		bcopy(record->guid, out->guid, sizeof(out->guid));
		name_to_utf8(record->name, out->name, sizeof(out->name));
	} else {
		out->mbr_type = record->mbr_type;
		out->active = record->active;
	}
}

const char *
fsimg_partition_type_name(fsimg_scheme_kind_t scheme,
    const fsimg_partition_record_t *record)
{
	const fsimg_partition_type_t *type;

	for (size_t i = 0; i < TYPE_COUNT; i++) {
		type = &partition_types[i];
		if (scheme == FSIMG_SCHEME_GPT ?
		    type_carries_guid(type, record->type_guid) :
		    type_carries_byte(type, record->mbr_type))
			return (type->name);
	}
	return (NULL);
}
