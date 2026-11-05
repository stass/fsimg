/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "io.h"
#include "layer.h"

/*
 * Apple Partition Map, read only (specs/design-layers.md, "APM"), big-endian.
 * Block 0 holds a driver descriptor map, whose signature and block size are
 * all that is read. Entry N of the map starts at byte N * block and uses the
 * first 512 bytes of its block; start and length count blocks.
 */
#define APM_SIGNATURE_BYTES	2u
#define APM_SMALL_BLOCK		512u
#define APM_LARGE_BLOCK		2048u
#define APM_ENTRY_BYTES		512u
#define APM_MAX_ENTRIES		FSIMG_GPT_MAX_ENTRIES
#define APM_PROBE_BYTES		(APM_LARGE_BLOCK + APM_SIGNATURE_BYTES)
#define APM_TEXT_BYTES		32u

#define DDM_BLOCK_SIZE		2
#define ENTRY_MAP_COUNT		4
#define ENTRY_START		8
#define ENTRY_BLOCKS		12
#define ENTRY_NAME		16
#define ENTRY_TYPE		48

_Static_assert(sizeof(((fsimg_partition_record_t *)0)->apm_type) ==
    APM_TEXT_BYTES + 1, "a record holds a whole type and its terminator");
_Static_assert(FSIMG_GPT_NAME_UNITS >= APM_TEXT_BYTES,
    "a record's name holds a whole APM name");

static bool signature_at(const unsigned char *, size_t, size_t, const char *);
static bool map_block_bytes(const unsigned char *, size_t, fsimg_identity_t *,
    uint32_t *);
static fsimg_status_t apm_probe(const unsigned char *, size_t, uint64_t,
    fsimg_identity_t *, fsimg_error_t *);
static fsimg_status_t apm_corrupt(fsimg_error_t *, const char *);
static fsimg_status_t entry_corrupt(fsimg_error_t *, uint32_t, const char *);
static void text_field(const unsigned char *, char *);
static bool is_not_volume(const char *);
static fsimg_status_t decode_entry(const unsigned char *, uint32_t, uint32_t,
    uint32_t, fsimg_partition_record_t *, fsimg_error_t *);
static fsimg_status_t read_entry(const fsimg_scheme_input_t *, uint32_t,
    uint32_t, unsigned char *, fsimg_error_t *);
static fsimg_status_t read_entries(const fsimg_scheme_input_t *,
    fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t apm_open(const fsimg_scheme_input_t *,
    fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t apm_verify(const fsimg_scheme_input_t *,
    const fsimg_scheme_table_t *, fsimg_error_t *);

const fsimg_scheme_ops_t fsimg_apm_scheme = {
	.probe_bytes = APM_PROBE_BYTES,
	.probe = apm_probe,
	.open = apm_open,
	.verify = apm_verify,
	.large_sector = APM_LARGE_BLOCK,
	.yields = true
};

static bool
signature_at(const unsigned char *prefix, size_t length, size_t offset,
    const char *signature)
{
	return (length >= offset + APM_SIGNATURE_BYTES &&
	    bcmp(prefix + offset, signature, APM_SIGNATURE_BYTES) == 0);
}

/*
 * The block size the first entry's location gives, or false when the prefix
 * is no map. An entry at 512 decides whatever sbBlkSize says, because install
 * discs state 2048 and hold a whole map at 512 while their 2048-byte map starts
 * only at block 3. sbBlkSize is consulted for the map that has no 512-byte
 * twin. The DDM signature is the one identity check; PM locates what is decoded
 * and is never waived.
 */
static bool
map_block_bytes(const unsigned char *prefix, size_t length,
    fsimg_identity_t *identity, uint32_t *block)
{
	if (!fsimg_identity_holds(identity, signature_at(prefix, length, 0, "ER"),
	    "the disk has no driver descriptor map"))
		return (false);
	if (signature_at(prefix, length, APM_SMALL_BLOCK, "PM")) {
		*block = APM_SMALL_BLOCK;
		return (true);
	}
	if (signature_at(prefix, length, APM_LARGE_BLOCK, "PM") &&
	    fsimg_load_be16(prefix + DDM_BLOCK_SIZE) == APM_LARGE_BLOCK) {
		*block = APM_LARGE_BLOCK;
		return (true);
	}
	return (false);
}

static fsimg_status_t
apm_probe(const unsigned char *prefix, size_t length,
    uint64_t disk_bytes FSIMG_UNUSED, fsimg_identity_t *identity,
    fsimg_error_t *error)
{
	uint32_t block;

	if (!map_block_bytes(prefix, length, identity, &block))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no Apple Partition Map"));
	return (FSIMG_OK);
}

static fsimg_status_t
apm_corrupt(fsimg_error_t *error, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "Apple Partition Map %s", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

static fsimg_status_t
entry_corrupt(fsimg_error_t *error, uint32_t index, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "APM partition %u %s",
	    (unsigned)index, check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/* A name or type field as text: the bytes before the first zero, each one
 * outside printable ASCII replaced, so an image's bytes cannot reach a report
 * or terminal as they stand. text holds APM_TEXT_BYTES and a terminator, and
 * every byte of it is defined, since a record's type is reported whole.
 */
static void
text_field(const unsigned char *raw, char *text)
{
	size_t i;

	bzero(text, APM_TEXT_BYTES + 1);
	for (i = 0; i < APM_TEXT_BYTES && raw[i] != 0; i++)
		text[i] = raw[i] >= 0x20 && raw[i] < 0x7f ? (char)raw[i] : '?';
}

/* The types that describe the disk's own machinery and never a volume. */
static bool
is_not_volume(const char *type)
{
	return (strcmp(type, "Apple_partition_map") == 0 ||
	    strcmp(type, "Apple_Patches") == 0 ||
	    strncmp(type, "Apple_Driver", strlen("Apple_Driver")) == 0);
}

/*
 * Decode entry index of a map of count entries into its record. An entry
 * of zero length or a free or void type stays an unused slot and is not judged
 * further: a disc's Apple_Free may run past its image and Apple_Void starts at 0.
 * Extents are at most 2^32 blocks of at most 2^11 bytes, so neither they nor
 * their sum can overflow.
 */
static fsimg_status_t
decode_entry(const unsigned char *raw, uint32_t index, uint32_t count,
    uint32_t block, fsimg_partition_record_t *record, fsimg_error_t *error)
{
	char type[APM_TEXT_BYTES + 1], name[APM_TEXT_BYTES + 1];
	uint32_t start, blocks;

	bzero(record, sizeof(*record));
	record->index = index;
	if (!signature_at(raw, APM_ENTRY_BYTES, 0, "PM"))
		return (entry_corrupt(error, index, "has no PM signature"));
	if (fsimg_load_be32(raw + ENTRY_MAP_COUNT) != count)
		return (entry_corrupt(error, index,
		    "states another entry count than the first"));
	start = fsimg_load_be32(raw + ENTRY_START);
	blocks = fsimg_load_be32(raw + ENTRY_BLOCKS);
	text_field(raw + ENTRY_TYPE, type);
	if (blocks == 0 || strcmp(type, "Apple_Free") == 0 ||
	    strcmp(type, "Apple_Void") == 0)
		return (FSIMG_OK);
	if (type[0] == '\0')
		return (entry_corrupt(error, index, "has no type"));
	if (start == 0)
		return (entry_corrupt(error, index,
		    "contains the driver descriptor map"));
	if (start <= count && strcmp(type, "Apple_partition_map") != 0)
		return (entry_corrupt(error, index,
		    "overlaps the partition map"));
	text_field(raw + ENTRY_NAME, name);
	for (size_t i = 0; name[i] != '\0'; i++)
		record->name[i] = (uint16_t)name[i];
	bcopy(type, record->apm_type, sizeof(type));
	record->used = true;
	record->not_volume = is_not_volume(type);
	record->offset = (uint64_t)start * block;
	record->length = (uint64_t)blocks * block;
	return (FSIMG_OK);
}

static fsimg_status_t
read_entry(const fsimg_scheme_input_t *input, uint32_t index, uint32_t block,
    unsigned char *raw, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_read_session(input->disk, input->session,
	    (uint64_t)index * block, raw, APM_ENTRY_BYTES, &transferred, error));
}

/* Size the table from the first entry and decode every entry into it. A map
 * must lie wholly within the disk: what is cut from it cannot be told from
 * damage, and the partitions it would describe are unknown.
 */
static fsimg_status_t
read_entries(const fsimg_scheme_input_t *input, fsimg_scheme_table_t *table,
    fsimg_error_t *error)
{
	unsigned char raw[APM_ENTRY_BYTES];
	uint32_t block = table->sector_bytes, count;
	fsimg_status_t status;

	if (input->disk_bytes < UINT64_C(2) * block)
		return (apm_corrupt(error, "runs past the end of the disk"));
	status = read_entry(input, 1, block, raw, error);
	if (status != FSIMG_OK)
		return (status);
	count = fsimg_load_be32(raw + ENTRY_MAP_COUNT);
	if (count == 0 || count > APM_MAX_ENTRIES)
		return (apm_corrupt(error, "entry count is out of range"));
	if (((uint64_t)count + 1) * block > input->disk_bytes)
		return (apm_corrupt(error, "runs past the end of the disk"));
	table->records = fsimg_alloc(input->ctx,
	    (size_t)count * sizeof(*table->records), error);
	if (table->records == NULL)
		return (error->status);
	table->count = count;
	for (uint32_t i = 1; i <= count; i++) {
		status = read_entry(input, i, block, raw, error);
		if (status == FSIMG_OK)
			status = decode_entry(raw, i, count, block,
			    &table->records[i - 1], error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * The whole map, checked against what its own entries declare; fitting it to a
 * shorter disk is the image's (lib/layers/image.c, clip_table). The table states
 * no disk length, because sbBlkCount is unreliable on real images.
 */
static fsimg_status_t
apm_open(const fsimg_scheme_input_t *input, fsimg_scheme_table_t *table,
    fsimg_error_t *error)
{
	unsigned char prefix[APM_PROBE_BYTES];
	uint32_t block, first, second;
	size_t length, transferred;
	char check[64];
	fsimg_status_t status;

	bzero(table, sizeof(*table));
	length = input->disk_bytes < sizeof(prefix) ?
	    (size_t)input->disk_bytes : sizeof(prefix);
	status = fsimg_io_read_session(input->disk, input->session, 0, prefix,
	    length, &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (!map_block_bytes(prefix, length, input->identity, &block))
		return (apm_corrupt(error, "changed after it was identified"));
	table->kind = FSIMG_SCHEME_APM;
	table->sector_bytes = block;
	status = read_entries(input, table, error);
	if (status == FSIMG_OK)
		status = fsimg_scheme_find_overlap(input->ctx, table, &first,
		    &second, error);
	if (status == FSIMG_OK && first != 0) {
		snprintf(check, sizeof(check), "partitions %u and %u overlap",
		    (unsigned)first, (unsigned)second);
		status = apm_corrupt(error, check);
	}
	if (status != FSIMG_OK) {
		fsimg_dealloc(table->records);
		bzero(table, sizeof(*table));
	}
	return (status);
}

/* Open checks every field the profile reads; nothing remains. */
static fsimg_status_t
apm_verify(const fsimg_scheme_input_t *input FSIMG_UNUSED,
    const fsimg_scheme_table_t *table FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}
