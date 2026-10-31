/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "io.h"
#include "layer.h"
#include "unicode.h"

/*
 * GUID Partition Table. With S the sector size (512 or 4096) and L the
 * disk's last LBA, LBA 0 holds a protective MBR, LBA 1 the primary header,
 * which points at the primary entry array, and LBA L the backup header,
 * which points at the backup array just before it. A table is accepted
 * only when both copies are intact and agree: a disk whose copies
 * disagree, including a truncated disk whose backup is gone, is reported
 * as corrupt rather than repaired from one copy. A written table has the
 * standard 128 entries, so its array fills 16 KiB at either sector size.
 */
#define GPT_SIGNATURE		"EFI PART"
#define GPT_SIGNATURE_BYTES	8
#define GPT_HEADER_MIN_BYTES	92
#define GPT_REVISION		UINT32_C(0x00010000)
#define GPT_ENTRY_BYTES		128
#define GPT_WRITE_ENTRIES	128u
#define GPT_ARRAY_BYTES		(GPT_WRITE_ENTRIES * GPT_ENTRY_BYTES)
#define GPT_PROBE_BYTES		(4096 + GPT_SIGNATURE_BYTES)
#define MBR_SIGNATURE_OFFSET	510
#define MBR_ENTRIES_OFFSET	446
#define MBR_PROTECTIVE_TYPE	0xee

/* Header fields, as UEFI lays them out. */
enum {
	HEADER_REVISION = 8,
	HEADER_SIZE = 12,
	HEADER_CRC = 16,
	HEADER_MY_LBA = 24,
	HEADER_ALTERNATE_LBA = 32,
	HEADER_FIRST_USABLE = 40,
	HEADER_LAST_USABLE = 48,
	HEADER_DISK_GUID = 56,
	HEADER_ENTRIES_LBA = 72,
	HEADER_ENTRY_COUNT = 80,
	HEADER_ENTRY_SIZE = 84,
	HEADER_ENTRIES_CRC = 88
};

/* Entry fields. */
enum {
	ENTRY_TYPE_GUID = 0,
	ENTRY_GUID = 16,
	ENTRY_FIRST_LBA = 32,
	ENTRY_LAST_LBA = 40,
	ENTRY_NAME = 56
};

/* A decoded header copy; its GUID is in textual byte order. */
typedef struct gpt_header {
	unsigned char	 disk_guid[16];
	uint64_t	 my_lba;
	uint64_t	 alternate_lba;
	uint64_t	 first_usable;
	uint64_t	 last_usable;
	uint64_t	 entries_lba;
	uint32_t	 entry_count;
	uint32_t	 entry_size;
	uint32_t	 entries_crc;
} gpt_header_t;

/* The disk as the table sees it: the sector size and the last LBA. */
typedef struct gpt_geometry {
	uint64_t	 last_lba;
	uint32_t	 sector_bytes;
} gpt_geometry_t;

/* A used entry's LBA range, sorted to find overlaps. */
typedef struct gpt_extent {
	uint64_t	 first;
	uint64_t	 last;
	uint32_t	 index;
} gpt_extent_t;

static uint32_t gpt_crc32(const unsigned char *, size_t);
static bool protective_mbr(const unsigned char *, size_t);
static bool signature_at(const unsigned char *, size_t, size_t);
static fsimg_status_t gpt_probe(const unsigned char *, size_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t gpt_corrupt(fsimg_error_t *, const char *,
    const char *);
static void guid_swap(const unsigned char *, unsigned char *);
static fsimg_status_t read_bytes(const fsimg_scheme_input_t *, uint64_t,
    void *, size_t, fsimg_error_t *);
static fsimg_status_t read_signature(const fsimg_scheme_input_t *, uint64_t,
    bool *, fsimg_error_t *);
static fsimg_status_t choose_geometry(const fsimg_scheme_input_t *,
    gpt_geometry_t *, fsimg_error_t *);
static fsimg_status_t read_header(const fsimg_scheme_input_t *,
    const gpt_geometry_t *, uint64_t, const char *, gpt_header_t *,
    fsimg_error_t *);
static uint64_t entry_array_lbas(const gpt_header_t *, const gpt_geometry_t *);
static fsimg_status_t check_primary(const gpt_header_t *,
    const gpt_geometry_t *, fsimg_error_t *);
static fsimg_status_t check_backup(const gpt_header_t *, const gpt_header_t *,
    const gpt_geometry_t *, fsimg_error_t *);
static fsimg_status_t read_entry_array(const fsimg_scheme_input_t *,
    const gpt_header_t *, const gpt_geometry_t *, const char *,
    unsigned char **, fsimg_error_t *);
static fsimg_status_t decode_entries(const unsigned char *,
    const gpt_header_t *, const gpt_geometry_t *, fsimg_partition_record_t *,
    fsimg_error_t *);
static int compare_extents(const void *, const void *);
static fsimg_status_t check_overlap(fsimg_ctx_t *,
    const fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t gpt_open(const fsimg_scheme_input_t *,
    fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t gpt_verify(const fsimg_scheme_input_t *,
    const fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t gpt_plan_disk(const fsimg_scheme_plan_args_t *,
    fsimg_scheme_table_t *, fsimg_scheme_plan_facts_t *, fsimg_error_t *);
static fsimg_status_t plan_name(fsimg_partition_record_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t gpt_plan_partition(const fsimg_scheme_partition_args_t *,
    fsimg_partition_record_t *, fsimg_error_t *);
static fsimg_status_t gpt_validate_plan(const fsimg_scheme_table_t *,
    uint64_t, fsimg_error_t *);
static void gpt_report_plan(const fsimg_scheme_table_t *, uint32_t,
    fsimg_image_info_t *, fsimg_partition_info_t *);
static void gpt_footprint(uint32_t, uint64_t *, uint64_t *);
static void encode_protective_mbr(unsigned char *, uint64_t);
static void encode_entries(unsigned char *, const fsimg_scheme_table_t *);
static void encode_header(unsigned char *, const unsigned char *,
    const gpt_header_t *);
static fsimg_status_t gpt_encode(fsimg_ctx_t *, const fsimg_scheme_table_t *,
    uint64_t, fsimg_scheme_tables_t *, fsimg_error_t *);

const fsimg_scheme_ops_t fsimg_gpt_scheme = {
	.probe_bytes = GPT_PROBE_BYTES,
	.probe = gpt_probe,
	.open = gpt_open,
	.verify = gpt_verify,
	.slots = GPT_WRITE_ENTRIES,
	.max_disk_sectors = UINT64_MAX,
	.plan_disk = gpt_plan_disk,
	.plan_partition = gpt_plan_partition,
	.validate_plan = gpt_validate_plan,
	.report_plan = gpt_report_plan,
	.footprint = gpt_footprint,
	.encode = gpt_encode
};

/* CRC-32 as UEFI and zlib define it: reflected polynomial 0xEDB88320, all
 * ones initially and inverted at the end. "123456789" gives 0xCBF43926.
 */
static uint32_t
gpt_crc32(const unsigned char *bytes, size_t length)
{
	uint32_t crc;

	crc = UINT32_C(0xffffffff);
	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 1) != 0 ? crc >> 1 ^ UINT32_C(0xedb88320) :
			    crc >> 1;
	}
	return (~crc);
}

/* A sector with the MBR signature and an entry of GPT's protective type. */
static bool
protective_mbr(const unsigned char *sector, size_t length)
{
	if (length < 512 || sector[MBR_SIGNATURE_OFFSET] != 0x55 ||
	    sector[MBR_SIGNATURE_OFFSET + 1] != 0xaa)
		return (false);
	for (unsigned i = 0; i < 4; i++) {
		if (sector[MBR_ENTRIES_OFFSET + i * 16 + 4] == MBR_PROTECTIVE_TYPE)
			return (true);
	}
	return (false);
}

static bool
signature_at(const unsigned char *prefix, size_t length, size_t offset)
{
	return (length >= offset + GPT_SIGNATURE_BYTES &&
	    bcmp(prefix + offset, GPT_SIGNATURE, GPT_SIGNATURE_BYTES) == 0);
}

/* The identity is a protective MBR or a header signature at LBA 1 of
 * either sector size. A protective MBR without a header is a GPT whose
 * header is missing, so it is a corrupt claim rather than something else.
 */
static fsimg_status_t
gpt_probe(const unsigned char *prefix, size_t length,
    uint64_t disk_bytes FSIMG_UNUSED, fsimg_error_t *error)
{
	bool protective, header;

	protective = protective_mbr(prefix, length);
	header = signature_at(prefix, length, 512) ||
	    signature_at(prefix, length, 4096);
	if (!protective && !header)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no GPT signature"));
	if (!header)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
		    "GPT protective MBR has no GPT header at LBA 1"));
	return (FSIMG_OK);
}

/* A corrupt table, naming the copy ("primary" or "backup") and the check. */
static fsimg_status_t
gpt_corrupt(fsimg_error_t *error, const char *copy, const char *check)
{
	char message[sizeof(error->message)];

	snprintf(message, sizeof(message), "GPT %s%s%s", copy,
	    copy[0] != '\0' ? " " : "", check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/* GPT stores the first three GUID groups little-endian and the rest in
 * text order; settings and reports use the text order throughout. The
 * reordering is its own inverse, so it both decodes and encodes.
 */
static void
guid_swap(const unsigned char *from, unsigned char *to)
{
	static const unsigned char order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
	    8, 9, 10, 11, 12, 13, 14, 15 };

	for (unsigned i = 0; i < 16; i++)
		to[i] = from[order[i]];
}

static fsimg_status_t
read_bytes(const fsimg_scheme_input_t *input, uint64_t offset, void *buffer,
    size_t length, fsimg_error_t *error)
{
	size_t transferred;

	return (fsimg_io_read_session(input->disk, input->session, offset,
	    buffer, length, &transferred, error));
}

/* Whether the disk holds a header signature at a byte offset. */
static fsimg_status_t
read_signature(const fsimg_scheme_input_t *input, uint64_t offset, bool *found,
    fsimg_error_t *error)
{
	unsigned char signature[GPT_SIGNATURE_BYTES];
	fsimg_status_t status;

	*found = false;
	if (input->disk_bytes < offset + GPT_SIGNATURE_BYTES)
		return (FSIMG_OK);
	status = read_bytes(input, offset, signature, sizeof(signature), error);
	if (status != FSIMG_OK)
		return (status);
	*found = bcmp(signature, GPT_SIGNATURE, GPT_SIGNATURE_BYTES) == 0;
	return (FSIMG_OK);
}

/* The sector size is where the primary signature is, LBA 1 at 512 or 4096
 * bytes, or the forced gpt:sector. Signatures at both offsets cannot be
 * told apart from the bytes, so they need the setting.
 */
static fsimg_status_t
choose_geometry(const fsimg_scheme_input_t *input, gpt_geometry_t *geometry,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	bool at_512, at_4096;

	status = read_signature(input, 512, &at_512, error);
	if (status == FSIMG_OK)
		status = read_signature(input, 4096, &at_4096, error);
	if (status != FSIMG_OK)
		return (status);
	geometry->sector_bytes = input->options->gpt_sector_bytes;
	if (geometry->sector_bytes == 0) {
		if (at_512 && at_4096)
			return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
			    FSIMG_OP_IMAGE, "GPT headers at both 512 and 4096 bytes "
			    "are ambiguous; force gpt with gpt:sector"));
		if (!at_512 && !at_4096)
			return (gpt_corrupt(error, "primary", "header is missing"));
		geometry->sector_bytes = at_512 ? 512 : 4096;
	}
	if (input->disk_bytes / geometry->sector_bytes < 3)
		return (gpt_corrupt(error, "", "disk is too small for a table"));
	geometry->last_lba = input->disk_bytes / geometry->sector_bytes - 1;
	return (FSIMG_OK);
}

/* Read and decode one header copy, checking what identifies it as intact:
 * its signature, a header size UEFI allows, and its CRC32, computed with
 * the CRC field zero.
 */
static fsimg_status_t
read_header(const fsimg_scheme_input_t *input, const gpt_geometry_t *geometry,
    uint64_t lba, const char *copy, gpt_header_t *header, fsimg_error_t *error)
{
	unsigned char raw[4096];
	uint32_t header_bytes, recorded_crc;
	fsimg_status_t status;

	status = read_bytes(input, lba * geometry->sector_bytes, raw,
	    geometry->sector_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (bcmp(raw, GPT_SIGNATURE, GPT_SIGNATURE_BYTES) != 0)
		return (gpt_corrupt(error, copy, "header signature is missing"));
	header_bytes = fsimg_load_le32(raw + HEADER_SIZE);
	if (header_bytes < GPT_HEADER_MIN_BYTES ||
	    header_bytes > geometry->sector_bytes)
		return (gpt_corrupt(error, copy, "header size is out of range"));
	recorded_crc = fsimg_load_le32(raw + HEADER_CRC);
	bzero(raw + HEADER_CRC, 4);
	if (gpt_crc32(raw, header_bytes) != recorded_crc)
		return (gpt_corrupt(error, copy, "header CRC32 mismatch"));

	guid_swap(raw + HEADER_DISK_GUID, header->disk_guid);
	header->my_lba = fsimg_load_le64(raw + HEADER_MY_LBA);
	header->alternate_lba = fsimg_load_le64(raw + HEADER_ALTERNATE_LBA);
	header->first_usable = fsimg_load_le64(raw + HEADER_FIRST_USABLE);
	header->last_usable = fsimg_load_le64(raw + HEADER_LAST_USABLE);
	header->entries_lba = fsimg_load_le64(raw + HEADER_ENTRIES_LBA);
	header->entry_count = fsimg_load_le32(raw + HEADER_ENTRY_COUNT);
	header->entry_size = fsimg_load_le32(raw + HEADER_ENTRY_SIZE);
	header->entries_crc = fsimg_load_le32(raw + HEADER_ENTRIES_CRC);
	return (FSIMG_OK);
}

/* LBAs one entry array occupies; the count is already at most 1024. */
static uint64_t
entry_array_lbas(const gpt_header_t *header, const gpt_geometry_t *geometry)
{
	uint64_t bytes;

	bytes = (uint64_t)header->entry_count * GPT_ENTRY_BYTES;
	return ((bytes + geometry->sector_bytes - 1) / geometry->sector_bytes);
}

/*
 * The primary header must describe this disk: it sits at LBA 1, its
 * alternate is the last LBA, its entries have the standard size and a
 * bounded count, its array lies between the header and the usable range,
 * and the usable range ends before the last LBA. Each comparison is of
 * values at most the last LBA plus the array length, so none overflows.
 */
static fsimg_status_t
check_primary(const gpt_header_t *header, const gpt_geometry_t *geometry,
    fsimg_error_t *error)
{
	if (header->my_lba != 1)
		return (gpt_corrupt(error, "primary", "header does not record LBA 1"));
	if (header->alternate_lba != geometry->last_lba)
		return (gpt_corrupt(error, "primary",
		    "alternate LBA is not the disk's last LBA"));
	if (header->entry_size != GPT_ENTRY_BYTES)
		return (gpt_corrupt(error, "primary", "entry size is not 128 bytes"));
	if (header->entry_count == 0 || header->entry_count > FSIMG_GPT_MAX_ENTRIES)
		return (gpt_corrupt(error, "primary",
		    "entry count is outside 1 to 1024"));
	if (header->first_usable > header->last_usable ||
	    header->last_usable >= geometry->last_lba)
		return (gpt_corrupt(error, "primary", "usable range is invalid"));
	if (header->entries_lba < 2 || header->entries_lba > header->first_usable ||
	    entry_array_lbas(header, geometry) >
	    header->first_usable - header->entries_lba)
		return (gpt_corrupt(error, "primary",
		    "entry array overlaps the header or the usable range"));
	return (FSIMG_OK);
}

/* The backup header must mirror the primary at the last LBA, pointing back
 * to LBA 1, with its own array after the usable range and before itself.
 */
static fsimg_status_t
check_backup(const gpt_header_t *primary, const gpt_header_t *backup,
    const gpt_geometry_t *geometry, fsimg_error_t *error)
{
	if (backup->my_lba != geometry->last_lba || backup->alternate_lba != 1)
		return (gpt_corrupt(error, "backup",
		    "header does not point back to the primary"));
	if (bcmp(backup->disk_guid, primary->disk_guid, 16) != 0 ||
	    backup->first_usable != primary->first_usable ||
	    backup->last_usable != primary->last_usable ||
	    backup->entry_count != primary->entry_count ||
	    backup->entry_size != primary->entry_size)
		return (gpt_corrupt(error, "backup", "header differs from the primary"));
	if (backup->entries_lba <= primary->last_usable ||
	    backup->entries_lba >= geometry->last_lba ||
	    entry_array_lbas(backup, geometry) >
	    geometry->last_lba - backup->entries_lba)
		return (gpt_corrupt(error, "backup",
		    "entry array overlaps the usable range or the header"));
	return (FSIMG_OK);
}

/* Read one copy's entry array into charged memory and check its CRC32. The
 * array's length comes from the validated count, never from a size the
 * table declares beyond it.
 */
static fsimg_status_t
read_entry_array(const fsimg_scheme_input_t *input, const gpt_header_t *header,
    const gpt_geometry_t *geometry, const char *copy, unsigned char **out,
    fsimg_error_t *error)
{
	unsigned char *array;
	size_t bytes;
	fsimg_status_t status;

	*out = NULL;
	bytes = (size_t)header->entry_count * GPT_ENTRY_BYTES;
	array = fsimg_alloc(input->ctx, bytes, error);
	if (array == NULL)
		return (error->status);
	status = read_bytes(input, header->entries_lba * geometry->sector_bytes,
	    array, bytes, error);
	if (status == FSIMG_OK && gpt_crc32(array, bytes) != header->entries_crc)
		status = gpt_corrupt(error, copy, "entry array CRC32 mismatch");
	if (status != FSIMG_OK) {
		fsimg_dealloc(array);
		return (status);
	}
	*out = array;
	return (FSIMG_OK);
}

/* One record per entry. A used entry, one with a nonzero type, must lie
 * within the usable range; its bytes then lie within the disk.
 */
static fsimg_status_t
decode_entries(const unsigned char *array, const gpt_header_t *header,
    const gpt_geometry_t *geometry, fsimg_partition_record_t *records,
    fsimg_error_t *error)
{
	static const unsigned char unused[16];
	fsimg_partition_record_t *record;
	const unsigned char *raw;
	uint64_t first, last;
	char check[64];

	for (uint32_t i = 0; i < header->entry_count; i++) {
		raw = array + (size_t)i * GPT_ENTRY_BYTES;
		record = &records[i];
		bzero(record, sizeof(*record));
		record->index = i + 1;
		if (bcmp(raw + ENTRY_TYPE_GUID, unused, sizeof(unused)) == 0)
			continue;
		first = fsimg_load_le64(raw + ENTRY_FIRST_LBA);
		last = fsimg_load_le64(raw + ENTRY_LAST_LBA);
		if (first > last || first < header->first_usable ||
		    last > header->last_usable) {
			snprintf(check, sizeof(check),
			    "partition %u lies outside the usable range",
			    (unsigned)record->index);
			return (gpt_corrupt(error, "", check));
		}
		record->used = true;
		guid_swap(raw + ENTRY_TYPE_GUID, record->type_guid);
		guid_swap(raw + ENTRY_GUID, record->guid);
		for (unsigned unit = 0; unit < FSIMG_GPT_NAME_UNITS; unit++)
			record->name[unit] = fsimg_load_le16(raw + ENTRY_NAME +
			    unit * 2);
		record->offset = first * geometry->sector_bytes;
		record->length = (last - first + 1) * geometry->sector_bytes;
	}
	return (FSIMG_OK);
}

static int
compare_extents(const void *left, const void *right)
{
	const gpt_extent_t *a = left, *b = right;

	if (a->first != b->first)
		return (a->first < b->first ? -1 : 1);
	return (a->index < b->index ? -1 : a->index > b->index);
}

/* Sort the used entries by first LBA, in memory charged to the context,
 * so each can only overlap its predecessor.
 */
static fsimg_status_t
check_overlap(fsimg_ctx_t *ctx, const fsimg_scheme_table_t *table,
    fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	gpt_extent_t *extents;
	uint32_t count;
	char check[64];
	fsimg_status_t status;

	extents = fsimg_alloc(ctx, (size_t)table->count * sizeof(*extents),
	    error);
	if (extents == NULL)
		return (error->status);
	count = 0;
	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		if (!record->used)
			continue;
		extents[count].first = record->offset / table->sector_bytes;
		extents[count].last = extents[count].first - 1 +
		    record->length / table->sector_bytes;
		extents[count].index = record->index;
		count++;
	}
	qsort(extents, count, sizeof(*extents), compare_extents);
	status = FSIMG_OK;
	for (uint32_t i = 1; i < count; i++) {
		if (extents[i].first > extents[i - 1].last)
			continue;
		snprintf(check, sizeof(check), "partitions %u and %u overlap",
		    (unsigned)extents[i - 1].index, (unsigned)extents[i].index);
		status = gpt_corrupt(error, "", check);
		break;
	}
	fsimg_dealloc(extents);
	return (status);
}

/*
 * Validate both copies and build the table: choose the sector size, check
 * the primary header and its array, check the backup header and that its
 * array is byte for byte the primary's, then decode the entries and reject
 * overlap. Any failure is FSIMG_ECORRUPT naming the check.
 */
static fsimg_status_t
gpt_open(const fsimg_scheme_input_t *input, fsimg_scheme_table_t *table,
    fsimg_error_t *error)
{
	gpt_geometry_t geometry;
	gpt_header_t primary, backup;
	unsigned char *primary_array, *backup_array;
	size_t array_bytes;
	fsimg_status_t status;

	bzero(table, sizeof(*table));
	status = choose_geometry(input, &geometry, error);
	if (status != FSIMG_OK)
		return (status);
	status = read_header(input, &geometry, 1, "primary", &primary, error);
	if (status == FSIMG_OK)
		status = check_primary(&primary, &geometry, error);
	if (status != FSIMG_OK)
		return (status);
	status = read_entry_array(input, &primary, &geometry, "primary",
	    &primary_array, error);
	if (status != FSIMG_OK)
		return (status);

	backup_array = NULL;
	status = read_header(input, &geometry, geometry.last_lba, "backup",
	    &backup, error);
	if (status == FSIMG_OK)
		status = check_backup(&primary, &backup, &geometry, error);
	if (status == FSIMG_OK)
		status = read_entry_array(input, &backup, &geometry, "backup",
		    &backup_array, error);
	array_bytes = (size_t)primary.entry_count * GPT_ENTRY_BYTES;
	if (status == FSIMG_OK && bcmp(primary_array, backup_array,
	    array_bytes) != 0)
		status = gpt_corrupt(error, "backup",
		    "entry array differs from the primary");
	fsimg_dealloc(backup_array);
	if (status != FSIMG_OK) {
		fsimg_dealloc(primary_array);
		return (status);
	}

	table->records = fsimg_alloc(input->ctx,
	    (size_t)primary.entry_count * sizeof(*table->records), error);
	if (table->records == NULL) {
		fsimg_dealloc(primary_array);
		return (error->status);
	}
	table->count = primary.entry_count;
	table->sector_bytes = geometry.sector_bytes;
	table->kind = FSIMG_SCHEME_GPT;
	bcopy(primary.disk_guid, table->disk_guid, sizeof(table->disk_guid));
	status = decode_entries(primary_array, &primary, &geometry,
	    table->records, error);
	fsimg_dealloc(primary_array);
	if (status == FSIMG_OK)
		status = check_overlap(input->ctx, table, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(table->records);
		bzero(table, sizeof(*table));
	}
	return (status);
}

/* Reading needs no protective MBR, so open accepts a disk without one;
 * verification requires it: the MBR signature and an entry of type 0xEE
 * starting at LBA 1.
 */
static fsimg_status_t
gpt_verify(const fsimg_scheme_input_t *input,
    const fsimg_scheme_table_t *table FSIMG_UNUSED, fsimg_error_t *error)
{
	unsigned char sector[512];
	const unsigned char *entry;
	fsimg_status_t status;

	status = read_bytes(input, 0, sector, sizeof(sector), error);
	if (status != FSIMG_OK)
		return (status);
	if (sector[MBR_SIGNATURE_OFFSET] == 0x55 &&
	    sector[MBR_SIGNATURE_OFFSET + 1] == 0xaa) {
		for (unsigned i = 0; i < 4; i++) {
			entry = sector + MBR_ENTRIES_OFFSET + i * 16;
			if (entry[4] == MBR_PROTECTIVE_TYPE &&
			    fsimg_load_le32(entry + 8) == 1)
				return (FSIMG_OK);
		}
	}
	return (gpt_corrupt(error, "",
	    "protective MBR has no 0xEE entry starting at LBA 1"));
}

/*
 * The write side's disk settings: gpt:sector, 512 or 4096 bytes with zero
 * for 512; gpt:disk, a fixed length or zero; and gpt:disk-guid, taken as
 * given, derived from the epoch in reproducible mode, or refused.
 */
static fsimg_status_t
gpt_plan_disk(const fsimg_scheme_plan_args_t *args,
    fsimg_scheme_table_t *table, fsimg_scheme_plan_facts_t *facts,
    fsimg_error_t *error)
{
	const fsimg_layer_options_t *layers = args->layers;

	table->kind = FSIMG_SCHEME_GPT;
	table->sector_bytes = layers->gpt_sector_bytes == 0 ? 512 :
	    layers->gpt_sector_bytes;
	if (table->sector_bytes != 512 && table->sector_bytes != 4096)
		return (fsimg_setting_error(error, "gpt:sector",
		    "gpt:sector must be 512 or 4096"));
	facts->requested_disk_bytes = layers->gpt_disk_bytes;
	facts->disk_key = "gpt:disk";
	if (!fsimg_guid_unset(layers->gpt_disk_guid))
		bcopy(layers->gpt_disk_guid, table->disk_guid, 16);
	else if ((args->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		fsimg_identifier_derive(args->epoch, FSIMG_ROLE_DISK, 0,
		    table->disk_guid);
	else
		return (fsimg_setting_error(error, "gpt:disk-guid",
		    "the disk GUID must be set outside reproducible mode"));
	return (FSIMG_OK);
}

/* A GPT name: validated UTF-8 of at most 36 UTF-16 units, stored as units
 * with zero padding. Every failure is FSIMG_EINVAL.
 */
static fsimg_status_t
plan_name(fsimg_partition_record_t *record, const char *name,
    fsimg_error_t *error)
{
	size_t units;
	fsimg_status_t status;

	status = fsimg_utf8_to_utf16(name, strlen(name), NULL, 0, &units, error);
	if (status == FSIMG_OK && units > FSIMG_GPT_NAME_UNITS)
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the GPT name exceeds 36 UTF-16 units");
	if (status == FSIMG_OK)
		status = fsimg_utf8_to_utf16(name, strlen(name), record->name,
		    FSIMG_GPT_NAME_UNITS, &units, error);
	if (status != FSIMG_OK) {
		if (error != NULL)
			error->status = FSIMG_EINVAL;
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

/*
 * One partition: gpt:type, gpt:name and gpt:guid from the layer options,
 * or a compose part's type, name and GUID. The type is the given one or
 * the filesystem's; a nonempty name replaces the type's conventional one;
 * the unique GUID is taken as given, derived from the epoch and the slot
 * number in reproducible mode, or refused. GPT has no active flag, and
 * neither source can supply one.
 */
static fsimg_status_t
gpt_plan_partition(const fsimg_scheme_partition_args_t *args,
    fsimg_partition_record_t *record, fsimg_error_t *error)
{
	fsimg_type_selector_t selector;
	const char *type, *name, *default_name;
	const unsigned char *guid;
	fsimg_status_t status;

	if (args->layers != NULL) {
		type = args->layers->gpt_type;
		name = args->layers->gpt_name;
		guid = args->layers->gpt_guid;
	} else {
		type = args->part->type;
		name = args->part->name;
		guid = args->part->guid;
	}
	status = fsimg_partition_type_select(type, args->filesystem, &selector,
	    error);
	if (status == FSIMG_OK)
		status = fsimg_partition_type_gpt(&selector, record, &default_name,
		    error);
	if (status != FSIMG_OK)
		return (fsimg_partition_error(args, "gpt:type", status, error));
	status = plan_name(record, name != NULL && name[0] != '\0' ? name :
	    default_name, error);
	if (status != FSIMG_OK)
		return (fsimg_partition_error(args, "gpt:name", status, error));
	/* The slot is at most GPT_WRITE_ENTRIES, which a byte holds. */
	if (guid != NULL && !fsimg_guid_unset(guid))
		bcopy(guid, record->guid, 16);
	else if ((args->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		fsimg_identifier_derive(args->epoch, FSIMG_ROLE_PARTITION,
		    (uint8_t)args->index, record->guid);
	else
		return (fsimg_partition_setting_error(args, "gpt:guid",
		    "the partition GUID must be set outside reproducible mode",
		    error));
	record->used = true;
	return (FSIMG_OK);
}

/* Unique GUIDs differ from each other and from the disk GUID, so no two
 * GPT objects of one disk share an identity. 128 entries need no more
 * than the pairwise comparison.
 */
static fsimg_status_t
gpt_validate_plan(const fsimg_scheme_table_t *table,
    uint64_t disk_bytes FSIMG_UNUSED, fsimg_error_t *error)
{
	const fsimg_partition_record_t *records = table->records;
	char message[80];

	for (uint32_t i = 0; i < table->count; i++) {
		if (bcmp(records[i].guid, table->disk_guid, 16) == 0) {
			snprintf(message, sizeof(message),
			    "partition %u has the disk GUID", (unsigned)i + 1);
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, message));
		}
		for (uint32_t j = 0; j < i; j++) {
			if (bcmp(records[i].guid, records[j].guid, 16) != 0)
				continue;
			snprintf(message, sizeof(message),
			    "partitions %u and %u have the same GUID",
			    (unsigned)j + 1, (unsigned)i + 1);
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, message));
		}
	}
	return (FSIMG_OK);
}

/* A planned disk reports its 128 written slots and its disk GUID. */
static void
gpt_report_plan(const fsimg_scheme_table_t *table, uint32_t slot,
    fsimg_image_info_t *image, fsimg_partition_info_t *partition)
{
	image->sector_bytes = table->sector_bytes;
	image->partition_count = GPT_WRITE_ENTRIES;
	image->selected = slot;
	bcopy(table->disk_guid, image->disk_guid, sizeof(image->disk_guid));
	fsimg_partition_report(FSIMG_SCHEME_GPT, &table->records[slot - 1],
	    partition);
}

/* A written table occupies the protective MBR, the primary header and its
 * array at the start of the disk, and the backup array and header at the
 * end: 34 and 33 sectors at 512 bytes, 6 and 5 at 4096.
 */
static void
gpt_footprint(uint32_t sector_bytes, uint64_t *leading, uint64_t *trailing)
{
	uint64_t array_sectors = GPT_ARRAY_BYTES / sector_bytes;

	*leading = 2 + array_sectors;
	*trailing = array_sectors + 1;
}

/* LBA 0 as UEFI defines the protective MBR: a zero code area and disk
 * identifier, one entry of type 0xEE from LBA 1 over the rest of the disk
 * (capped at 2^32 - 1 sectors) with CHS start 0x000200 and end 0xFFFFFF,
 * the other entries zero, and the signature. The sector is zeroed.
 */
static void
encode_protective_mbr(unsigned char *sector, uint64_t last_lba)
{
	unsigned char *entry = sector + MBR_ENTRIES_OFFSET;

	entry[2] = 0x02;
	entry[4] = MBR_PROTECTIVE_TYPE;
	entry[5] = entry[6] = entry[7] = 0xff;
	fsimg_store_le32(entry + 8, 1);
	fsimg_store_le32(entry + 12, last_lba > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)last_lba);
	sector[MBR_SIGNATURE_OFFSET] = 0x55;
	sector[MBR_SIGNATURE_OFFSET + 1] = 0xaa;
}

/* The used records into a zeroed 128-entry array: type and unique GUIDs in
 * GPT byte order, first and last LBA, zero attributes, and the name's
 * units little-endian with zero padding.
 */
static void
encode_entries(unsigned char *array, const fsimg_scheme_table_t *table)
{
	const fsimg_partition_record_t *record;
	unsigned char *raw;
	uint64_t first;

	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		if (!record->used)
			continue;
		raw = array + (size_t)(record->index - 1) * GPT_ENTRY_BYTES;
		first = record->offset / table->sector_bytes;
		guid_swap(record->type_guid, raw + ENTRY_TYPE_GUID);
		guid_swap(record->guid, raw + ENTRY_GUID);
		fsimg_store_le64(raw + ENTRY_FIRST_LBA, first);
		fsimg_store_le64(raw + ENTRY_LAST_LBA, first - 1 +
		    record->length / table->sector_bytes);
		for (unsigned unit = 0; unit < FSIMG_GPT_NAME_UNITS; unit++)
			fsimg_store_le16(raw + ENTRY_NAME + unit * 2,
			    record->name[unit]);
	}
}

/* One header copy into a zeroed sector, its CRC32 computed last over the
 * 92 bytes with the CRC field still zero.
 */
static void
encode_header(unsigned char *raw, const unsigned char *disk_guid,
    const gpt_header_t *header)
{
	bcopy(GPT_SIGNATURE, raw, GPT_SIGNATURE_BYTES);
	fsimg_store_le32(raw + HEADER_REVISION, GPT_REVISION);
	fsimg_store_le32(raw + HEADER_SIZE, GPT_HEADER_MIN_BYTES);
	fsimg_store_le64(raw + HEADER_MY_LBA, header->my_lba);
	fsimg_store_le64(raw + HEADER_ALTERNATE_LBA, header->alternate_lba);
	fsimg_store_le64(raw + HEADER_FIRST_USABLE, header->first_usable);
	fsimg_store_le64(raw + HEADER_LAST_USABLE, header->last_usable);
	guid_swap(disk_guid, raw + HEADER_DISK_GUID);
	fsimg_store_le64(raw + HEADER_ENTRIES_LBA, header->entries_lba);
	fsimg_store_le32(raw + HEADER_ENTRY_COUNT, header->entry_count);
	fsimg_store_le32(raw + HEADER_ENTRY_SIZE, header->entry_size);
	fsimg_store_le32(raw + HEADER_ENTRIES_CRC, header->entries_crc);
	fsimg_store_le32(raw + HEADER_CRC,
	    gpt_crc32(raw, GPT_HEADER_MIN_BYTES));
}

/*
 * Both copies of a 128-entry table for a disk of disk_bytes: the leading
 * run of protective MBR, primary header and array, and the trailing run of
 * backup array and backup header, which mirrors the primary with the two
 * LBAs exchanged. The usable range is what sgdisk writes, from the sector
 * after the primary array to the one before the backup array. The plan
 * placed every record inside it; a record outside is an internal error,
 * never an encoded table.
 */
static fsimg_status_t
gpt_encode(fsimg_ctx_t *ctx, const fsimg_scheme_table_t *table,
    uint64_t disk_bytes, fsimg_scheme_tables_t *out, fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	gpt_header_t header;
	unsigned char *leading, *trailing, *array;
	uint64_t leading_sectors, trailing_sectors, last_lba, first, end;
	uint32_t sector;

	bzero(out, sizeof(*out));
	sector = table->sector_bytes;
	gpt_footprint(sector, &leading_sectors, &trailing_sectors);
	if ((sector != 512 && sector != 4096) || disk_bytes % sector != 0 ||
	    disk_bytes / sector < leading_sectors + trailing_sectors + 1 ||
	    table->count > GPT_WRITE_ENTRIES)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "GPT table geometry was not planned"));
	last_lba = disk_bytes / sector - 1;
	bzero(&header, sizeof(header));
	header.first_usable = leading_sectors;
	header.last_usable = last_lba - trailing_sectors;
	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		first = record->offset / sector;
		end = first + record->length / sector;
		if (record->index == 0 || record->index > GPT_WRITE_ENTRIES ||
		    (record->used && (record->offset % sector != 0 ||
		    record->length % sector != 0 || record->length == 0 ||
		    first < header.first_usable || end - 1 > header.last_usable)))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_IMAGE, "GPT partition was not placed"));
	}

	leading = fsimg_alloc(ctx, (size_t)(leading_sectors * sector), error);
	if (leading == NULL)
		return (error->status);
	trailing = fsimg_alloc(ctx, (size_t)(trailing_sectors * sector), error);
	if (trailing == NULL) {
		fsimg_dealloc(leading);
		return (error->status);
	}
	array = leading + 2 * (size_t)sector;
	encode_entries(array, table);
	encode_protective_mbr(leading, last_lba);
	header.entry_count = GPT_WRITE_ENTRIES;
	header.entry_size = GPT_ENTRY_BYTES;
	header.entries_crc = gpt_crc32(array, GPT_ARRAY_BYTES);

	header.my_lba = 1;
	header.alternate_lba = last_lba;
	header.entries_lba = 2;
	encode_header(leading + sector, table->disk_guid, &header);
	bcopy(array, trailing, GPT_ARRAY_BYTES);
	header.my_lba = last_lba;
	header.alternate_lba = 1;
	header.entries_lba = last_lba - (trailing_sectors - 1);
	encode_header(trailing + GPT_ARRAY_BYTES, table->disk_guid, &header);

	out->regions[0] = (fsimg_table_region_t){ .bytes = leading,
	    .offset = 0, .length = (size_t)(leading_sectors * sector) };
	out->regions[1] = (fsimg_table_region_t){ .bytes = trailing,
	    .offset = header.entries_lba * sector,
	    .length = (size_t)(trailing_sectors * sector) };
	out->count = 2;
	return (FSIMG_OK);
}
