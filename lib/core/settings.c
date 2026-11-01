/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "internal.h"
#include "settings.h"

/*
 * Row construction. Each binding macro names a record and a member once
 * and yields the row's record, kind, offset and size, so a row's kind is
 * chosen by the macro and cannot disagree with its C type. The size term
 * also compiles a comparison between a pointer to the kind's C type and
 * the member's address, which is a constraint violation, so a build error,
 * when the member has another type; sizeof keeps it unevaluated. A text
 * member is compared as a char array of its own size, which refuses a
 * pointer member.
 *
 * RECORD_WRITE and its siblings expand to two macro arguments, the record
 * type and its fsimg_setting_record_t value; BIND() and TEXT() take
 * __VA_ARGS__ so that expansion happens before BIND_() and TEXT_() split
 * their arguments.
 */
#define MEMBER(type, member)	(((type *)NULL)->member)
#define TYPED_SIZE(type, member, ctype)	(sizeof(MEMBER(type, member)) +	\
	0 * sizeof((ctype *)NULL == &MEMBER(type, member)))
#define TEXT_SIZE(type, member)	(sizeof(MEMBER(type, member)) +	\
	0 * sizeof((char (*)[sizeof(MEMBER(type, member))])NULL ==	\
	&MEMBER(type, member)))

/*
 * Bool members are read as their one byte, so a caller's byte other than
 * 0 or 1, which is not a bool, is seen and refused rather than loaded.
 */
_Static_assert(sizeof(bool) == 1, "a bool member is one byte");

/* The C type of a 16-byte identifier member, for its binding's check. */
typedef unsigned char id16_t[16];

#define RECORD_WRITE	fsimg_write_options_t, FSIMG_RECORD_WRITE
#define RECORD_LAYERS	fsimg_layer_options_t, FSIMG_RECORD_LAYERS
#define RECORD_INPUTS	fsimg_layer_inputs_t, FSIMG_RECORD_INPUTS

#define BIND_(type, record_id, member, kind_id, ctype, flag_offset)	\
	.record = (record_id), .kind = (kind_id),			\
	.offset = offsetof(type, member),				\
	.size = TYPED_SIZE(type, member, ctype), .presence = (flag_offset)
#define BIND(...)		BIND_(__VA_ARGS__)
#define U32(rec, member)	BIND(rec, member, FSIMG_KIND_U32, uint32_t,	\
	FSIMG_NO_PRESENCE)
#define U64(rec, member)	BIND(rec, member, FSIMG_KIND_U64, uint64_t,	\
	FSIMG_NO_PRESENCE)
#define FLAG(rec, member)	BIND(rec, member, FSIMG_KIND_BOOL, bool,	\
	FSIMG_NO_PRESENCE)
#define ID16(rec, member)	BIND(rec, member, FSIMG_KIND_ID16, id16_t,	\
	FSIMG_NO_PRESENCE)
#define IO(rec, member)		BIND(rec, member, FSIMG_KIND_IO, fsimg_io_t *, \
	FSIMG_NO_PRESENCE)
#define TEXT_POINTER(rec, member)	BIND(rec, member,		\
	FSIMG_KIND_TEXT_POINTER, const char *, FSIMG_NO_PRESENCE)
#define BYTE_ORDER(rec, member)	BIND(rec, member, FSIMG_KIND_BYTE_ORDER,	\
	fsimg_byte_order_t, FSIMG_NO_PRESENCE)
#define DMG_COMPRESSION(rec, member)	BIND(rec, member,		\
	FSIMG_KIND_DMG_COMPRESSION, fsimg_dmg_compression_t, FSIMG_NO_PRESENCE)
#define IFSBOOT_COMPRESSION(rec, member)	BIND(rec, member,	\
	FSIMG_KIND_IFSBOOT_COMPRESSION, fsimg_ifsboot_compression_t,	\
	FSIMG_NO_PRESENCE)
#define XZ_CHECK(rec, member)	BIND(rec, member, FSIMG_KIND_XZ_CHECK,	\
	fsimg_xz_check_t, FSIMG_NO_PRESENCE)
#define TEXT_(type, record_id, member)	.record = (record_id),		\
	.kind = FSIMG_KIND_TEXT, .offset = offsetof(type, member),	\
	.size = TEXT_SIZE(type, member), .presence = FSIMG_NO_PRESENCE
#define TEXT(...)		TEXT_(__VA_ARGS__)
/* A layer value member and the bool member that marks it present. */
#define FLAG_OFFSET(flag)	(offsetof(fsimg_layer_options_t, flag) +	\
	0 * sizeof((bool *)NULL == &MEMBER(fsimg_layer_options_t, flag)))
#define U32_SET(member, flag)	BIND(RECORD_LAYERS, member, FSIMG_KIND_U32,	\
	uint32_t, FLAG_OFFSET(flag))
#define FLAG_SET(member, flag)	BIND(RECORD_LAYERS, member, FSIMG_KIND_BOOL, \
	bool, FLAG_OFFSET(flag))
#define NO_STORAGE		.record = FSIMG_RECORD_NONE,		\
	.kind = FSIMG_KIND_NONE, .presence = FSIMG_NO_PRESENCE

#define SLICE(rows)	{ (rows), sizeof(rows) / sizeof((rows)[0]) }

static const fsimg_setting_choice_t sector_choices[] = {
	{ "512", { .u32 = 512 } }, { "4096", { .u32 = 4096 } }, { NULL, { 0 } }
};
static const fsimg_setting_choice_t oem_choices[] = {
	{ "cp437", { 0 } }, { NULL, { 0 } }
};
static const fsimg_setting_choice_t endian_choices[] = {
	{ "little", { .byte_order = FSIMG_BYTE_ORDER_LITTLE } },
	{ "big", { .byte_order = FSIMG_BYTE_ORDER_BIG } }, { NULL, { 0 } }
};
static const fsimg_setting_choice_t dmg_choices[] = {
	{ "zlib", { .dmg_compression = FSIMG_DMG_ZLIB } },
	{ "none", { .dmg_compression = FSIMG_DMG_NONE } }, { NULL, { 0 } }
};
static const fsimg_setting_choice_t ifsboot_choices[] = {
	{ "lzo", { .ifsboot_compression = FSIMG_IFSBOOT_LZO } },
	{ "none", { .ifsboot_compression = FSIMG_IFSBOOT_NONE } },
	{ NULL, { 0 } }
};
static const fsimg_setting_choice_t xz_check_choices[] = {
	{ "crc32", { .xz_check = FSIMG_XZ_CRC32 } },
	{ "crc64", { .xz_check = FSIMG_XZ_CRC64 } },
	{ "sha256", { .xz_check = FSIMG_XZ_SHA256 } }, { NULL, { 0 } }
};

/*
 * The filesystem's settings: geometry, which every writer accepts and
 * validates against its own profile; oem, the read side's acknowledgement
 * of the CP437 short names every FAT read already uses; and the
 * format-specific fields, each tied to its FSIMG_SETTING_* bit. The
 * mountpoint is a borrowed pointer, NULL for the format's default and ""
 * for an explicit empty path; the command bounds it by the 1025 bytes and
 * terminator of the worker record's mountpoint field. root, the FAT12/16
 * fixed root's entry count, is bounded by its 16-bit on-disk field; the
 * writer rounds it up to whole sectors.
 */
static const fsimg_setting_t filesystem_rows[] = {
	{ .key = "sector", U32(RECORD_WRITE, sector_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .key = "cluster", U32(RECORD_WRITE, allocation_unit_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .key = "oem", NO_STORAGE, .syntax = FSIMG_SYNTAX_CHOICE,
	    .choices = oem_choices, .scope = FSIMG_SCOPE_READ },
	{ .key = "endian", BYTE_ORDER(RECORD_WRITE, byte_order), .syntax = FSIMG_SYNTAX_CHOICE,
	    .choices = endian_choices,
	    .initial = { .byte_order = FSIMG_BYTE_ORDER_LITTLE },
	    .scope = FSIMG_SCOPE_WRITE, .support = FSIMG_SETTING_BYTE_ORDER },
	{ .key = "uuid", ID16(RECORD_WRITE, volume_uuid),
	    .syntax = FSIMG_SYNTAX_UUID, .scope = FSIMG_SCOPE_WRITE,
	    .support = FSIMG_SETTING_VOLUME_UUID },
	{ .key = "inodes", U32(RECORD_WRITE, inode_capacity),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE, .support = FSIMG_SETTING_INODE_CAPACITY },
	{ .key = "reserve", U32(RECORD_WRITE, reserve_percent),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = 100, .initial = { .u32 = 3 },
	    .scope = FSIMG_SCOPE_WRITE, .support = FSIMG_SETTING_RESERVE },
	{ .key = "mountpoint", TEXT_POINTER(RECORD_WRITE, mountpoint),
	    .syntax = FSIMG_SYNTAX_TEXT, .text_bytes = 1025 + 1,
	    .scope = FSIMG_SCOPE_WRITE, .support = FSIMG_SETTING_MOUNTPOINT },
	{ .key = "root", U32(RECORD_WRITE, fat_root_entries),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT16_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE, .support = FSIMG_SETTING_ROOT_ENTRIES }
};

/*
 * The layer settings. Every layer default is zero, empty or false, which
 * asks the layer for its profile's own choice; the two compression enums
 * spell that default FSIMG_DMG_ZLIB and FSIMG_IFSBOOT_LZO. A command-line
 * quantity is never zero, since zero would request that default. The
 * identifiers refuse the nil UUID, which means "unset". gpt:sector is the
 * one setting reading also takes.
 */
static const fsimg_setting_t gpt_rows[] = {
	{ .owner = "gpt", .key = "sector", U32(RECORD_LAYERS, gpt_sector_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .choices = sector_choices,
	    .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_READ | FSIMG_SCOPE_WRITE },
	{ .owner = "gpt", .key = "disk", U64(RECORD_LAYERS, gpt_disk_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT64_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "gpt", .key = "type", TEXT(RECORD_LAYERS, gpt_type),
	    .syntax = FSIMG_SYNTAX_TEXT,
	    .scope = FSIMG_SCOPE_WRITE | FSIMG_SCOPE_PARTITION },
	{ .owner = "gpt", .key = "name", TEXT(RECORD_LAYERS, gpt_name),
	    .syntax = FSIMG_SYNTAX_TEXT,
	    .scope = FSIMG_SCOPE_WRITE | FSIMG_SCOPE_PARTITION },
	{ .owner = "gpt", .key = "disk-guid", ID16(RECORD_LAYERS, gpt_disk_guid),
	    .syntax = FSIMG_SYNTAX_UUID, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "gpt", .key = "guid", ID16(RECORD_LAYERS, gpt_guid),
	    .syntax = FSIMG_SYNTAX_UUID, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE | FSIMG_SCOPE_PARTITION }
};

static const fsimg_setting_t mbr_rows[] = {
	{ .owner = "mbr", .key = "disk", U64(RECORD_LAYERS, mbr_disk_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT64_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "mbr", .key = "type", TEXT(RECORD_LAYERS, mbr_type),
	    .syntax = FSIMG_SYNTAX_TEXT,
	    .scope = FSIMG_SCOPE_WRITE | FSIMG_SCOPE_PARTITION },
	{ .owner = "mbr", .key = "active", FLAG(RECORD_LAYERS, mbr_active),
	    .syntax = FSIMG_SYNTAX_BOOL,
	    .scope = FSIMG_SCOPE_WRITE | FSIMG_SCOPE_PARTITION },
	{ .owner = "mbr", .key = "id",
	    U32_SET(mbr_id, mbr_id_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE }
};

static const fsimg_setting_t dmg_rows[] = {
	{ .owner = "dmg", .key = "compress", DMG_COMPRESSION(RECORD_LAYERS,
	    dmg_compress),
	    .syntax = FSIMG_SYNTAX_CHOICE, .choices = dmg_choices,
	    .initial = { .dmg_compression = FSIMG_DMG_ZLIB },
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "dmg", .key = "level", U32(RECORD_LAYERS, dmg_level),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "dmg", .key = "chunk", U32(RECORD_LAYERS, dmg_chunk_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "dmg", .key = "uuid", ID16(RECORD_LAYERS, dmg_uuid),
	    .syntax = FSIMG_SYNTAX_UUID, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE }
};

static const fsimg_setting_t vhd_rows[] = {
	{ .owner = "vhd", .key = "align", U32(RECORD_LAYERS, vhd_align_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "vhd", .key = "uuid", ID16(RECORD_LAYERS, vhd_uuid),
	    .syntax = FSIMG_SYNTAX_UUID, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE }
};

static const fsimg_setting_t simg_rows[] = {
	{ .owner = "simg", .key = "block", U32(RECORD_LAYERS, simg_block_bytes),
	    .syntax = FSIMG_SYNTAX_BYTES, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "simg", .key = "crc", FLAG(RECORD_LAYERS, simg_crc),
	    .syntax = FSIMG_SYNTAX_BOOL, .scope = FSIMG_SCOPE_WRITE }
};

/*
 * The QNX boot image: its two file settings first, then the startup
 * header fields no derivation gives, each stored verbatim and supplied
 * only with its presence flag, so a zero machine or address is a value.
 */
static const fsimg_setting_t ifsboot_rows[] = {
	{ .owner = "ifsboot", .key = "startup",
	    IO(RECORD_INPUTS, ifsboot_startup), .syntax = FSIMG_SYNTAX_PATH,
	    .file = FSIMG_FILE_IFSBOOT_STARTUP, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "prefix",
	    IO(RECORD_INPUTS, ifsboot_prefix), .syntax = FSIMG_SYNTAX_PATH,
	    .file = FSIMG_FILE_IFSBOOT_PREFIX, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "machine",
	    U32_SET(ifsboot_machine, ifsboot_machine_set),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT32_MAX,
	    .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "virtual",
	    FLAG_SET(ifsboot_virtual, ifsboot_virtual_set),
	    .syntax = FSIMG_SYNTAX_BOOL, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "startup_vaddr",
	    U32_SET(ifsboot_startup_vaddr, ifsboot_startup_vaddr_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "paddr_bias",
	    U32_SET(ifsboot_paddr_bias, ifsboot_paddr_bias_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "image_paddr",
	    U32_SET(ifsboot_image_paddr, ifsboot_image_paddr_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "ram_paddr",
	    U32_SET(ifsboot_ram_paddr, ifsboot_ram_paddr_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "imagefs_paddr",
	    U32_SET(ifsboot_imagefs_paddr, ifsboot_imagefs_paddr_set),
	    .syntax = FSIMG_SYNTAX_HEX32, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "ifsboot", .key = "compress", IFSBOOT_COMPRESSION(
	    RECORD_LAYERS, ifsboot_compress),
	    .syntax = FSIMG_SYNTAX_CHOICE, .choices = ifsboot_choices,
	    .initial = { .ifsboot_compression = FSIMG_IFSBOOT_LZO },
	    .scope = FSIMG_SCOPE_WRITE }
};

static const fsimg_setting_t gzip_rows[] = {
	{ .owner = "gzip", .key = "level", U32(RECORD_LAYERS, gzip_level),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT32_MAX, .nonzero = true,
	    .scope = FSIMG_SCOPE_WRITE }
};

/*
 * The optional compressions' rows, present in every build so their keys
 * are recognized whether or not the build has the library, which the
 * creation then refuses as unsupported. Preset 0 is a real xz
 * preset, so the initializer stores the default 6 instead of zero
 * selecting it; the profile refuses a preset above 9.
 */
static const fsimg_setting_t xz_rows[] = {
	{ .owner = "xz", .key = "level", U32(RECORD_LAYERS, xz_level),
	    .syntax = FSIMG_SYNTAX_COUNT, .max = UINT32_MAX,
	    .initial = { .u32 = 6 }, .scope = FSIMG_SCOPE_WRITE },
	{ .owner = "xz", .key = "check", XZ_CHECK(RECORD_LAYERS, xz_check),
	    .syntax = FSIMG_SYNTAX_CHOICE, .choices = xz_check_choices,
	    .initial = { .xz_check = FSIMG_XZ_CRC64 },
	    .scope = FSIMG_SCOPE_WRITE }
};

const fsimg_setting_slice_t fsimg_filesystem_settings = SLICE(filesystem_rows);
const fsimg_setting_slice_t fsimg_gpt_settings = SLICE(gpt_rows);
const fsimg_setting_slice_t fsimg_mbr_settings = SLICE(mbr_rows);
const fsimg_setting_slice_t fsimg_dmg_settings = SLICE(dmg_rows);
const fsimg_setting_slice_t fsimg_vhd_settings = SLICE(vhd_rows);
const fsimg_setting_slice_t fsimg_simg_settings = SLICE(simg_rows);
const fsimg_setting_slice_t fsimg_ifsboot_settings = SLICE(ifsboot_rows);
const fsimg_setting_slice_t fsimg_gzip_settings = SLICE(gzip_rows);
const fsimg_setting_slice_t fsimg_xz_settings = SLICE(xz_rows);

static const void *record_of(const fsimg_setting_t *,
    const fsimg_setting_records_t *);
static void *target_of(const fsimg_setting_t *,
    const fsimg_setting_targets_t *);
static void load_member(const fsimg_setting_t *, const unsigned char *,
    fsimg_setting_value_t *);
static void store_member(const fsimg_setting_t *, unsigned char *,
    const fsimg_setting_value_t *);
static bool values_equal(const fsimg_setting_t *,
    const fsimg_setting_value_t *, const fsimg_setting_value_t *);
static bool presence_flag(const fsimg_setting_t *, const unsigned char *);
static bool bool_byte(const unsigned char *);
static fsimg_status_t row_error(const fsimg_setting_t *, fsimg_operation_t,
    fsimg_error_t *, const char *);
static fsimg_status_t check_row(const fsimg_setting_t *,
    const fsimg_setting_records_t *, fsimg_operation_t, fsimg_error_t *);

const fsimg_setting_t *
fsimg_setting_find(const fsimg_setting_slice_t *slice, const char *key,
    size_t length)
{
	const fsimg_setting_t *row;

	for (size_t i = 0; i < slice->count; i++) {
		row = &slice->rows[i];
		if (strlen(row->key) == length &&
		    strncmp(row->key, key, length) == 0)
			return (row);
	}
	return (NULL);
}

void
fsimg_setting_key(const fsimg_setting_t *row, char *buffer, size_t capacity)
{
	if (row->owner == NULL)
		snprintf(buffer, capacity, "%s", row->key);
	else
		snprintf(buffer, capacity, "%s:%s", row->owner, row->key);
}

void
fsimg_setting_choices_text(const fsimg_setting_t *row, char *buffer,
    size_t capacity)
{
	const fsimg_setting_choice_t *choice;
	size_t used;
	int written;

	used = 0;
	if (capacity != 0)
		buffer[0] = '\0';
	for (choice = row->choices; choice != NULL && choice->spelling != NULL;
	    choice++) {
		if (used >= capacity)
			break;
		written = snprintf(buffer + used, capacity - used, "%s%s",
		    choice == row->choices ? "" :
		    choice[1].spelling == NULL ? " or " : ", ",
		    choice->spelling);
		if (written < 0)
			break;
		used += (size_t)written;
	}
}

const fsimg_setting_choice_t *
fsimg_setting_choice_of(const fsimg_setting_t *row,
    const fsimg_setting_value_t *value)
{
	const fsimg_setting_choice_t *choice;

	for (choice = row->choices; choice != NULL && choice->spelling != NULL;
	    choice++) {
		if (values_equal(row, &choice->value, value))
			return (choice);
	}
	return (NULL);
}

/* The record a row's member lives in, NULL when the caller has none. */
static const void *
record_of(const fsimg_setting_t *row, const fsimg_setting_records_t *records)
{
	switch (row->record) {
	case FSIMG_RECORD_WRITE:
		return (records->write);
	case FSIMG_RECORD_LAYERS:
		return (records->layers);
	case FSIMG_RECORD_INPUTS:
		return (records->inputs);
	case FSIMG_RECORD_NONE:
		break;
	}
	return (NULL);
}

static void *
target_of(const fsimg_setting_t *row, const fsimg_setting_targets_t *targets)
{
	switch (row->record) {
	case FSIMG_RECORD_WRITE:
		return (targets->write);
	case FSIMG_RECORD_LAYERS:
		return (targets->layers);
	case FSIMG_RECORD_INPUTS:
		return (targets->inputs);
	case FSIMG_RECORD_NONE:
		break;
	}
	return (NULL);
}

/*
 * The one generic access: copy the member at its offset into a temporary
 * of its exact type, or back. bcopy keeps the access free of alignment and
 * aliasing assumptions about the record.
 */
static void
load_member(const fsimg_setting_t *row, const unsigned char *member,
    fsimg_setting_value_t *value)
{
	bzero(value, sizeof(*value));
	switch (row->kind) {
	case FSIMG_KIND_NONE:
		break;
	case FSIMG_KIND_U32:
		bcopy(member, &value->u32, sizeof(value->u32));
		break;
	case FSIMG_KIND_U64:
		bcopy(member, &value->u64, sizeof(value->u64));
		break;
	case FSIMG_KIND_BOOL:
		value->flag = bool_byte(member);
		break;
	case FSIMG_KIND_BYTE_ORDER:
		bcopy(member, &value->byte_order, sizeof(value->byte_order));
		break;
	case FSIMG_KIND_DMG_COMPRESSION:
		bcopy(member, &value->dmg_compression,
		    sizeof(value->dmg_compression));
		break;
	case FSIMG_KIND_IFSBOOT_COMPRESSION:
		bcopy(member, &value->ifsboot_compression,
		    sizeof(value->ifsboot_compression));
		break;
	case FSIMG_KIND_XZ_CHECK:
		bcopy(member, &value->xz_check, sizeof(value->xz_check));
		break;
	case FSIMG_KIND_TEXT:
		value->text = (const char *)member;
		break;
	case FSIMG_KIND_ID16:
		bcopy(member, value->id, sizeof(value->id));
		break;
	case FSIMG_KIND_TEXT_POINTER:
		bcopy(member, &value->text, sizeof(value->text));
		break;
	case FSIMG_KIND_IO:
		bcopy(member, &value->io, sizeof(value->io));
		break;
	}
}

static void
store_member(const fsimg_setting_t *row, unsigned char *member,
    const fsimg_setting_value_t *value)
{
	size_t length;

	switch (row->kind) {
	case FSIMG_KIND_NONE:
		break;
	case FSIMG_KIND_U32:
		bcopy(&value->u32, member, sizeof(value->u32));
		break;
	case FSIMG_KIND_U64:
		bcopy(&value->u64, member, sizeof(value->u64));
		break;
	case FSIMG_KIND_BOOL:
		bcopy(&value->flag, member, sizeof(value->flag));
		break;
	case FSIMG_KIND_BYTE_ORDER:
		bcopy(&value->byte_order, member, sizeof(value->byte_order));
		break;
	case FSIMG_KIND_DMG_COMPRESSION:
		bcopy(&value->dmg_compression, member,
		    sizeof(value->dmg_compression));
		break;
	case FSIMG_KIND_IFSBOOT_COMPRESSION:
		bcopy(&value->ifsboot_compression, member,
		    sizeof(value->ifsboot_compression));
		break;
	case FSIMG_KIND_XZ_CHECK:
		bcopy(&value->xz_check, member, sizeof(value->xz_check));
		break;
	case FSIMG_KIND_TEXT:
		/* The whole member is defined: the text, then zeros. A NULL
		 * text is the empty default. Copy before clearing the tail,
		 * since a loaded text can point into this same member.
		 */
		length = value->text == NULL ? 0 :
		    strnlen(value->text, row->size - 1);
		if (length != 0)
			bcopy(value->text, member, length);
		bzero(member + length, row->size - length);
		break;
	case FSIMG_KIND_ID16:
		bcopy(value->id, member, sizeof(value->id));
		break;
	case FSIMG_KIND_TEXT_POINTER:
		bcopy(&value->text, member, sizeof(value->text));
		break;
	case FSIMG_KIND_IO:
		bcopy(&value->io, member, sizeof(value->io));
		break;
	}
}

/*
 * Typed equality. Pointers compare as pointers, never as object
 * representations. The one fixed text ever compared is the empty default,
 * so texts compare by emptiness alone: the first byte decides, which
 * reads nothing past a member whose termination is not yet checked.
 */
static bool
values_equal(const fsimg_setting_t *row, const fsimg_setting_value_t *a,
    const fsimg_setting_value_t *b)
{
	switch (row->kind) {
	case FSIMG_KIND_NONE:
		return (true);
	case FSIMG_KIND_U32:
		return (a->u32 == b->u32);
	case FSIMG_KIND_U64:
		return (a->u64 == b->u64);
	case FSIMG_KIND_BOOL:
		return (a->flag == b->flag);
	case FSIMG_KIND_BYTE_ORDER:
		return (a->byte_order == b->byte_order);
	case FSIMG_KIND_DMG_COMPRESSION:
		return (a->dmg_compression == b->dmg_compression);
	case FSIMG_KIND_IFSBOOT_COMPRESSION:
		return (a->ifsboot_compression == b->ifsboot_compression);
	case FSIMG_KIND_XZ_CHECK:
		return (a->xz_check == b->xz_check);
	case FSIMG_KIND_TEXT:
		return ((a->text == NULL || a->text[0] == '\0') ==
		    (b->text == NULL || b->text[0] == '\0'));
	case FSIMG_KIND_ID16:
		return (bcmp(a->id, b->id, sizeof(a->id)) == 0);
	case FSIMG_KIND_TEXT_POINTER:
		return (a->text == b->text);
	case FSIMG_KIND_IO:
		return (a->io == b->io);
	}
	return (false);
}

/* A bool byte as a truth value: any nonzero byte is true. */
static bool
bool_byte(const unsigned char *member)
{
	return (member[0] != 0);
}

static bool
presence_flag(const fsimg_setting_t *row, const unsigned char *record)
{
	return (bool_byte(record + row->presence));
}

void
fsimg_setting_load(const fsimg_setting_t *row,
    const fsimg_setting_records_t *records, fsimg_setting_value_t *value)
{
	const unsigned char *record;

	record = record_of(row, records);
	if (record == NULL) {
		*value = row->initial;
		return;
	}
	load_member(row, record + row->offset, value);
}

void
fsimg_setting_store(const fsimg_setting_t *row,
    const fsimg_setting_targets_t *targets, const fsimg_setting_value_t *value)
{
	unsigned char *record;
	bool present;

	record = target_of(row, targets);
	if (record == NULL)
		return;
	store_member(row, record + row->offset, value);
	if (row->presence != FSIMG_NO_PRESENCE) {
		present = true;
		bcopy(&present, record + row->presence, sizeof(present));
	}
}

bool
fsimg_setting_is_default(const fsimg_setting_t *row,
    const fsimg_setting_records_t *records)
{
	const unsigned char *record;
	fsimg_setting_value_t value;

	record = record_of(row, records);
	if (record == NULL)
		return (true);
	if (row->presence != FSIMG_NO_PRESENCE)
		return (!presence_flag(row, record));
	load_member(row, record + row->offset, &value);
	return (values_equal(row, &value, &row->initial));
}

/* FSIMG_EINVAL naming the row's canonical key. */
static fsimg_status_t
row_error(const fsimg_setting_t *row, fsimg_operation_t operation,
    fsimg_error_t *error, const char *message)
{
	char key[48];

	fsimg_error_set(error, FSIMG_EINVAL, 0, operation, message);
	fsimg_setting_key(row, key, sizeof(key));
	fsimg_error_path(error, key);
	return (FSIMG_EINVAL);
}

fsimg_status_t
fsimg_setting_check(const fsimg_setting_t *row,
    const fsimg_setting_records_t *records, fsimg_error_t *error)
{
	return (check_row(row, records, FSIMG_OP_IMAGE, error));
}

static fsimg_status_t
check_row(const fsimg_setting_t *row, const fsimg_setting_records_t *records,
    fsimg_operation_t operation, fsimg_error_t *error)
{
	const unsigned char *record;
	fsimg_setting_value_t value;
	char choices[64], message[96];

	record = record_of(row, records);
	if (record == NULL)
		return (FSIMG_OK);
	if (row->kind == FSIMG_KIND_TEXT &&
	    memchr(record + row->offset, '\0', row->size) == NULL)
		return (row_error(row, operation, error,
		    "setting text is not terminated"));
	if ((row->kind == FSIMG_KIND_BOOL && record[row->offset] > 1) ||
	    (row->presence != FSIMG_NO_PRESENCE && record[row->presence] > 1))
		return (row_error(row, operation, error,
		    "setting flag is neither false nor true"));
	load_member(row, record + row->offset, &value);
	/* An enum holds one of its named values. */
	if (row->syntax == FSIMG_SYNTAX_CHOICE && row->kind != FSIMG_KIND_NONE &&
	    fsimg_setting_choice_of(row, &value) == NULL) {
		fsimg_setting_choices_text(row, choices, sizeof(choices));
		snprintf(message, sizeof(message), "the value must be %s",
		    choices);
		return (row_error(row, operation, error, message));
	}
	/* A value means something only once its flag is set. */
	if (row->presence != FSIMG_NO_PRESENCE && !presence_flag(row, record) &&
	    !values_equal(row, &value, &row->initial))
		return (row_error(row, operation, error,
		    "setting has a value but is not set"));
	return (FSIMG_OK);
}

void
fsimg_settings_init(const fsimg_setting_slice_t *slice,
    const fsimg_setting_targets_t *targets, fsimg_setting_record_t record)
{
	const fsimg_setting_t *row;
	unsigned char *base;
	bool absent;

	for (size_t i = 0; i < slice->count; i++) {
		row = &slice->rows[i];
		if (row->record != record)
			continue;
		base = target_of(row, targets);
		if (base == NULL)
			continue;
		store_member(row, base + row->offset, &row->initial);
		if (row->presence != FSIMG_NO_PRESENCE) {
			absent = false;
			bcopy(&absent, base + row->presence, sizeof(absent));
		}
	}
}

fsimg_status_t
fsimg_write_settings_check(const fsimg_write_options_t *options,
    const fsimg_format_info_t *writer, fsimg_error_t *error)
{
	const fsimg_setting_records_t records = { .write = options };
	const fsimg_setting_t *row;
	fsimg_operation_t operation;
	fsimg_status_t status;
	bool applies;

	operation = writer != NULL ? FSIMG_OP_WRITER : FSIMG_OP_IMAGE;
	for (size_t i = 0; i < fsimg_filesystem_settings.count; i++) {
		row = &fsimg_filesystem_settings.rows[i];
		status = check_row(row, &records, operation, error);
		if (status != FSIMG_OK)
			return (status);
		if (fsimg_setting_is_default(row, &records))
			continue;
		applies = writer != NULL && (row->support == 0 ||
		    (writer->write_settings & row->support) != 0);
		if (!applies)
			return (row_error(row, operation, error, writer == NULL ?
			    "filesystem settings must hold their defaults "
			    "without a filesystem writer" :
			    "write setting does not apply to the image format"));
	}
	return (FSIMG_OK);
}
