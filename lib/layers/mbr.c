/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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

const fsimg_scheme_ops_t fsimg_mbr_scheme = {
	.probe_bytes = MBR_SECTOR_BYTES,
	.probe = mbr_probe,
	.open = mbr_open,
	.verify = mbr_verify
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
	entry->first_lba = fsimg_layer_le32(raw + 8);
	entry->sectors = fsimg_layer_le32(raw + 12);
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
	table->mbr_disk_id = fsimg_layer_le32(sector + MBR_DISK_ID);
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
