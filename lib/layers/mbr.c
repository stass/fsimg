/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "io.h"
#include "layer.h"

/*
 * Master boot record, read side: a 512-byte sector with a 32-bit disk
 * identifier at 440, four 16-byte entries at 446 and the signature 0x55AA
 * at 510. An entry is a status byte (0x00, or 0x80 when active), a CHS
 * start, a type byte, a CHS end, and the first LBA and sector count as
 * little-endian 32-bit values; CHS fields are ignored on read. Sectors are
 * 512 bytes and an entry can address sectors 0 to 2^32 - 1.
 */
#define MBR_SECTOR_BYTES	512u
#define MBR_DISK_ID		440
#define MBR_ENTRIES		446
#define MBR_ENTRY_BYTES		16
#define MBR_SLOTS		4u
#define MBR_SIGNATURE		510
#define MBR_PROTECTIVE_TYPE	0xee
#define MBR_ADDRESSABLE_SECTORS	(UINT64_C(1) << 32)

/* One decoded entry; an all-zero entry is unused. */
typedef struct mbr_entry {
	uint64_t	 first_lba;
	uint64_t	 sectors;
	uint8_t		 status;
	uint8_t		 type;
	bool		 used;
} mbr_entry_t;

static bool decode_entry(const unsigned char *, mbr_entry_t *);
static bool partition_table_shape(const unsigned char *);
static fsimg_status_t mbr_probe(const unsigned char *, size_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t mbr_corrupt(fsimg_error_t *, const char *, unsigned,
    unsigned);
static fsimg_status_t mbr_open(const fsimg_scheme_input_t *,
    fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t mbr_verify(const fsimg_scheme_input_t *,
    const fsimg_scheme_table_t *, fsimg_error_t *);
static fsimg_status_t mbr_plan_disk(const fsimg_scheme_plan_args_t *,
    fsimg_scheme_table_t *, fsimg_scheme_plan_facts_t *, fsimg_error_t *);
static fsimg_status_t mbr_plan_partition(const fsimg_scheme_partition_args_t *,
    fsimg_partition_record_t *, fsimg_error_t *);
static fsimg_status_t mbr_validate_plan(const fsimg_scheme_table_t *,
    uint64_t, fsimg_error_t *);
static void mbr_report_plan(const fsimg_scheme_table_t *, uint32_t,
    fsimg_image_info_t *, fsimg_partition_info_t *);
static void mbr_footprint(uint32_t, uint64_t *, uint64_t *);
static void encode_chs(uint64_t, unsigned char *);
static fsimg_status_t mbr_encode(fsimg_ctx_t *, const fsimg_scheme_table_t *,
    uint64_t, fsimg_scheme_tables_t *, fsimg_error_t *);

const fsimg_scheme_ops_t fsimg_mbr_scheme = {
	.probe_bytes = MBR_SECTOR_BYTES,
	.probe = mbr_probe,
	.open = mbr_open,
	.verify = mbr_verify,
	.slots = MBR_SLOTS,
	.max_disk_sectors = MBR_ADDRESSABLE_SECTORS,
	.plan_disk = mbr_plan_disk,
	.plan_partition = mbr_plan_partition,
	.validate_plan = mbr_validate_plan,
	.report_plan = mbr_report_plan,
	.footprint = mbr_footprint,
	.encode = mbr_encode
};

/* Decode one entry. False when it is neither all zero nor legal: a status
 * of 0x00 or 0x80, a nonzero type and a nonzero sector count.
 */
static bool
decode_entry(const unsigned char *raw, mbr_entry_t *entry)
{
	static const unsigned char unused[MBR_ENTRY_BYTES];

	bzero(entry, sizeof(*entry));
	if (bcmp(raw, unused, sizeof(unused)) == 0)
		return (true);
	entry->status = raw[0];
	entry->type = raw[4];
	entry->first_lba = fsimg_load_le32(raw + 8);
	entry->sectors = fsimg_load_le32(raw + 12);
	entry->used = true;
	return ((entry->status == 0x00 || entry->status == 0x80) &&
	    entry->type != 0 && entry->sectors != 0);
}

/* The identity of a partition table, since a FAT boot sector carries the
 * same signature: the signature, every entry unused or legal, at least
 * one used, and none of GPT's protective type, so a hybrid MBR is a GPT
 * disk.
 */
static bool
partition_table_shape(const unsigned char *sector)
{
	mbr_entry_t entry;
	unsigned used;

	if (sector[MBR_SIGNATURE] != 0x55 || sector[MBR_SIGNATURE + 1] != 0xaa)
		return (false);
	used = 0;
	for (unsigned i = 0; i < MBR_SLOTS; i++) {
		if (!decode_entry(sector + MBR_ENTRIES + i * MBR_ENTRY_BYTES, &entry))
			return (false);
		if (!entry.used)
			continue;
		if (entry.type == MBR_PROTECTIVE_TYPE)
			return (false);
		used++;
	}
	return (used != 0);
}

static fsimg_status_t
mbr_probe(const unsigned char *prefix, size_t length,
    uint64_t disk_bytes FSIMG_UNUSED, fsimg_error_t *error)
{
	if (length < MBR_SECTOR_BYTES || !partition_table_shape(prefix))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no MBR partition table"));
	return (FSIMG_OK);
}

/* A corrupt table naming the check and the slots it concerns; a second
 * slot of zero names one.
 */
static fsimg_status_t
mbr_corrupt(fsimg_error_t *error, const char *check, unsigned slot,
    unsigned other)
{
	char message[sizeof(error->message)];

	if (other == 0)
		snprintf(message, sizeof(message), "MBR partition %u %s", slot,
		    check);
	else
		snprintf(message, sizeof(message), "MBR partitions %u and %u %s",
		    slot, other, check);
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * Decode the four slots and require each used one to start after the MBR
 * sector, to end within the disk and within the 2^32 sectors an entry can
 * address, and not to overlap another used slot. Extended partitions are
 * listed with their type; they hold logical partitions this profile does
 * not read.
 */
static fsimg_status_t
mbr_open(const fsimg_scheme_input_t *input, fsimg_scheme_table_t *table,
    fsimg_error_t *error)
{
	unsigned char sector[MBR_SECTOR_BYTES];
	mbr_entry_t entries[MBR_SLOTS];
	fsimg_partition_record_t *record;
	uint64_t disk_sectors, end;
	size_t transferred;
	fsimg_status_t status;

	bzero(table, sizeof(*table));
	status = fsimg_io_read_session(input->disk, input->session, 0, sector,
	    sizeof(sector), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (!partition_table_shape(sector))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_IMAGE,
		    "MBR partition table changed after it was identified"));
	disk_sectors = input->disk_bytes / MBR_SECTOR_BYTES;
	for (unsigned i = 0; i < MBR_SLOTS; i++) {
		decode_entry(sector + MBR_ENTRIES + i * MBR_ENTRY_BYTES, &entries[i]);
		if (!entries[i].used)
			continue;
		end = entries[i].first_lba + entries[i].sectors;
		if (entries[i].first_lba == 0)
			return (mbr_corrupt(error, "overlaps the master boot record",
			    i + 1, 0));
		if (end > MBR_ADDRESSABLE_SECTORS)
			return (mbr_corrupt(error,
			    "ends beyond the sectors an entry can address", i + 1, 0));
		if (end > disk_sectors)
			return (mbr_corrupt(error, "extends beyond the disk", i + 1, 0));
	}
	for (unsigned i = 0; i < MBR_SLOTS; i++) {
		for (unsigned j = i + 1; j < MBR_SLOTS; j++) {
			if (!entries[i].used || !entries[j].used)
				continue;
			if (entries[i].first_lba < entries[j].first_lba +
			    entries[j].sectors && entries[j].first_lba <
			    entries[i].first_lba + entries[i].sectors)
				return (mbr_corrupt(error, "overlap", i + 1, j + 1));
		}
	}

	table->records = fsimg_alloc(input->ctx,
	    MBR_SLOTS * sizeof(*table->records), error);
	if (table->records == NULL)
		return (error->status);
	table->count = MBR_SLOTS;
	table->sector_bytes = MBR_SECTOR_BYTES;
	table->mbr_disk_id = fsimg_load_le32(sector + MBR_DISK_ID);
	table->kind = FSIMG_SCHEME_MBR;
	for (unsigned i = 0; i < MBR_SLOTS; i++) {
		record = &table->records[i];
		bzero(record, sizeof(*record));
		record->index = i + 1;
		if (!entries[i].used)
			continue;
		record->used = true;
		record->active = entries[i].status == 0x80;
		record->mbr_type = entries[i].type;
		record->extended = entries[i].type == 0x05 ||
		    entries[i].type == 0x0f || entries[i].type == 0x85;
		record->offset = entries[i].first_lba * MBR_SECTOR_BYTES;
		record->length = entries[i].sectors * MBR_SECTOR_BYTES;
	}
	return (FSIMG_OK);
}

/* Open checks every field the profile defines; nothing remains. */
static fsimg_status_t
mbr_verify(const fsimg_scheme_input_t *input FSIMG_UNUSED,
    const fsimg_scheme_table_t *table FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/*
 * The write side's disk settings: 512-byte sectors; mbr:disk, a fixed
 * length or zero, of at most the 2^32 sectors an entry addresses; and
 * mbr:id, taken as given when set, zero included, the epoch's low 32 bits
 * in reproducible mode, or refused.
 */
static fsimg_status_t
mbr_plan_disk(const fsimg_scheme_plan_args_t *args,
    fsimg_scheme_table_t *table, fsimg_scheme_plan_facts_t *facts,
    fsimg_error_t *error)
{
	const fsimg_layer_options_t *layers = args->layers;

	table->kind = FSIMG_SCHEME_MBR;
	table->sector_bytes = MBR_SECTOR_BYTES;
	facts->requested_disk_bytes = layers->mbr_disk_bytes;
	facts->disk_key = "mbr:disk";
	if (layers->mbr_disk_bytes / MBR_SECTOR_BYTES > MBR_ADDRESSABLE_SECTORS)
		return (fsimg_setting_error(error, "mbr:disk",
		    "an MBR disk holds at most 2^32 sectors"));
	if (layers->mbr_id_set)
		table->mbr_disk_id = layers->mbr_id;
	else if ((args->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		table->mbr_disk_id = (uint32_t)(uint64_t)args->epoch;
	else
		return (fsimg_setting_error(error, "mbr:id",
		    "the disk identifier must be set outside reproducible mode"));
	return (FSIMG_OK);
}

/*
 * One partition: mbr:type and mbr:active from the layer options, or a
 * compose part's type, inactive, whose name and GUID must be absent since
 * an entry stores neither. The type is the given one or the filesystem's.
 */
static fsimg_status_t
mbr_plan_partition(const fsimg_scheme_partition_args_t *args,
    fsimg_partition_record_t *record, fsimg_error_t *error)
{
	fsimg_type_selector_t selector;
	const char *type, *name;
	const unsigned char *guid;
	bool active;
	fsimg_status_t status;

	if (args->layers != NULL) {
		type = args->layers->mbr_type;
		name = NULL;
		guid = NULL;
		active = args->layers->mbr_active;
	} else {
		type = args->part->type;
		name = args->part->name;
		guid = args->part->guid;
		active = false;
	}
	status = fsimg_partition_type_select(type, args->filesystem, &selector,
	    error);
	if (status == FSIMG_OK)
		status = fsimg_partition_type_mbr(&selector, record, error);
	if (status != FSIMG_OK)
		return (fsimg_partition_error(args, "mbr:type", status, error));
	if ((name != NULL && name[0] != '\0') ||
	    (guid != NULL && !fsimg_guid_unset(guid)))
		return (fsimg_partition_setting_error(args, "mbr:name",
		    "MBR partitions have no name or GUID", error));
	record->active = active;
	record->used = true;
	return (FSIMG_OK);
}

/*
 * The final disk within the 2^32 sectors an entry addresses, and so every
 * placed entry's first LBA and count, which start after the MBR sector and
 * end within the disk, within their 32-bit fields. The per-entry check
 * follows from the disk's and is kept as the stated guard before the
 * encoder narrows each field.
 */
static fsimg_status_t
mbr_validate_plan(const fsimg_scheme_table_t *table, uint64_t disk_bytes,
    fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;

	if (disk_bytes / MBR_SECTOR_BYTES > MBR_ADDRESSABLE_SECTORS)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the disk exceeds the 2^32 sectors an MBR addresses"));
	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		if (record->used &&
		    (record->offset / MBR_SECTOR_BYTES > UINT32_MAX ||
		    record->length / MBR_SECTOR_BYTES > UINT32_MAX))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "an MBR entry cannot address the "
			    "partition"));
	}
	return (FSIMG_OK);
}

/* A planned disk reports its four slots and its disk identifier. */
static void
mbr_report_plan(const fsimg_scheme_table_t *table, uint32_t slot,
    fsimg_image_info_t *image, fsimg_partition_info_t *partition)
{
	image->sector_bytes = table->sector_bytes;
	image->partition_count = MBR_SLOTS;
	image->selected = slot;
	image->mbr_disk_id = table->mbr_disk_id;
	fsimg_partition_report(FSIMG_SCHEME_MBR, &table->records[slot - 1],
	    partition);
}

/* The table is the first sector; nothing trails the partitions. */
static void
mbr_footprint(uint32_t sector_bytes FSIMG_UNUSED, uint64_t *leading,
    uint64_t *trailing)
{
	*leading = 1;
	*trailing = 0;
}

/* A CHS address under the 255-head, 63-sector convention, saturating at
 * cylinder 1023, head 254, sector 63 as fdisk writes it: the head, then
 * the sector with the cylinder's two high bits above it, then the
 * cylinder's low byte.
 */
static void
encode_chs(uint64_t lba, unsigned char *raw)
{
	uint64_t cylinder, head, sector;

	cylinder = lba / (255 * 63);
	head = lba / 63 % 255;
	sector = lba % 63 + 1;
	if (cylinder > 1023) {
		cylinder = 1023;
		head = 254;
		sector = 63;
	}
	raw[0] = (unsigned char)head;
	raw[1] = (unsigned char)(sector | (cylinder >> 2 & 0xc0));
	raw[2] = (unsigned char)(cylinder & 0xff);
}

/*
 * The one sector: a zero code area, the disk identifier, the placed
 * records in their slots with status, CHS start, type, CHS end, first LBA
 * and count, unused slots zero, and the signature. The plan placed every
 * record after the sector and within the 2^32 sectors an entry
 * addresses; anything else is an internal error, never an encoded table.
 */
static fsimg_status_t
mbr_encode(fsimg_ctx_t *ctx, const fsimg_scheme_table_t *table,
    uint64_t disk_bytes, fsimg_scheme_tables_t *out, fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	unsigned char *sector, *raw;
	uint64_t first, count;

	bzero(out, sizeof(*out));
	if (table->sector_bytes != MBR_SECTOR_BYTES || table->count > MBR_SLOTS ||
	    disk_bytes % MBR_SECTOR_BYTES != 0 ||
	    disk_bytes / MBR_SECTOR_BYTES > MBR_ADDRESSABLE_SECTORS)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "MBR table geometry was not planned"));
	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		first = record->offset / MBR_SECTOR_BYTES;
		count = record->length / MBR_SECTOR_BYTES;
		if (record->index == 0 || record->index > MBR_SLOTS ||
		    (record->used && (record->offset % MBR_SECTOR_BYTES != 0 ||
		    record->length % MBR_SECTOR_BYTES != 0 || first == 0 ||
		    count == 0 || first + count > disk_bytes / MBR_SECTOR_BYTES ||
		    record->mbr_type == 0)))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_IMAGE, "MBR partition was not placed"));
	}

	sector = fsimg_alloc(ctx, MBR_SECTOR_BYTES, error);
	if (sector == NULL)
		return (error->status);
	fsimg_store_le32(sector + MBR_DISK_ID, table->mbr_disk_id);
	for (uint32_t i = 0; i < table->count; i++) {
		record = &table->records[i];
		if (!record->used)
			continue;
		raw = sector + MBR_ENTRIES + (record->index - 1) * MBR_ENTRY_BYTES;
		first = record->offset / MBR_SECTOR_BYTES;
		count = record->length / MBR_SECTOR_BYTES;
		raw[0] = record->active ? 0x80 : 0x00;
		encode_chs(first, raw + 1);
		raw[4] = record->mbr_type;
		encode_chs(first + count - 1, raw + 5);
		fsimg_store_le32(raw + 8, (uint32_t)first);
		fsimg_store_le32(raw + 12, (uint32_t)count);
	}
	sector[MBR_SIGNATURE] = 0x55;
	sector[MBR_SIGNATURE + 1] = 0xaa;
	out->regions[0] = (fsimg_table_region_t){ .bytes = sector, .offset = 0,
	    .length = MBR_SECTOR_BYTES };
	out->count = 1;
	return (FSIMG_OK);
}
