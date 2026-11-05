/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/* The includer defines FAT_BYTES (FAT fixture size), GPT_ENTRIES (entry
 * count), and DISK_GUID (textual disk UUID) before including this header.
 */
#ifndef FSIMG_TEST_LAYERS_FIXTURE_H
#define FSIMG_TEST_LAYERS_FIXTURE_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "internal.h"
#include "test.h"

/* Sparse disks store only a prefix. Fault counters are zero when disabled. */
typedef struct memory_disk {
	unsigned char *bytes;
	uint64_t stored;
	uint64_t length;
	unsigned reads;
	unsigned fail_read;
	unsigned short_read;
} memory_disk_t;

typedef struct gpt_partition {
	const char *type_guid;
	const uint16_t *name;
	uint64_t first_lba;
	uint64_t last_lba;
} gpt_partition_t;

static void mbr_entry(unsigned char *, unsigned, uint8_t, uint8_t,
    uint32_t, uint32_t);
static void gpt_reseal(memory_disk_t *, uint32_t);

/* Independent encoders use published byte offsets, not decoder structs. */
static void
put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put32(unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static void
put64(unsigned char *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t
crc32_oracle(const unsigned char *bytes, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return (crc ^ 0xffffffffu);
}

/* A GUID's sixteen bytes in text order. */
static void
guid_text(const char *text, unsigned char *bytes)
{
	unsigned value;

	for (unsigned i = 0, position = 0; i < 16; i++) {
		if (text[position] == '-')
			position++;
		CHECK(sscanf(text + position, "%2x", &value) == 1);
		bytes[i] = (unsigned char)value;
		position += 2;
	}
}

/* GPT stores its first three GUID groups little-endian. */
static void
guid_put(unsigned char *raw, const char *text)
{
	unsigned char bytes[16];

	guid_text(text, bytes);
	for (unsigned i = 0; i < 4; i++)
		raw[i] = bytes[3 - i];
	raw[4] = bytes[5];
	raw[5] = bytes[4];
	raw[6] = bytes[7];
	raw[7] = bytes[6];
	bcopy(bytes + 8, raw + 8, 8);
}

/* A 1 MiB FAT12 volume: one reserved sector, two six-sector FATs,
 * a 224-entry root and a subdirectory DIR in cluster 2.
 */
static void
make_fat12(unsigned char *volume)
{
	static const char dot[] = ".          ";
	static const char dotdot[] = "..         ";
	unsigned char *boot = volume, *root, *directory;

	bzero(volume, FAT_BYTES);
	boot[0] = 0xeb;
	boot[1] = 0x3c;
	boot[2] = 0x90;
	bcopy("MSWIN4.1", boot + 3, 8);
	put16(boot + 11, 512);
	boot[13] = 1;
	put16(boot + 14, 1);
	boot[16] = 2;
	put16(boot + 17, 224);
	put16(boot + 19, 2048);
	boot[21] = 0xf8;
	put16(boot + 22, 6);
	put16(boot + 24, 32);
	put16(boot + 26, 2);
	boot[38] = 0x29;
	bcopy("NO NAME    FAT12   ", boot + 43, 19);
	boot[510] = 0x55;
	boot[511] = 0xaa;
	for (unsigned table = 0; table < 2; table++) {
		/* Media entry, the reserved entry, and cluster 2 end of chain. */
		static const unsigned char head[] = { 0xf8, 0xff, 0xff, 0xff, 0x0f };

		bcopy(head, volume + (1 + table * 6) * 512, sizeof(head));
	}
	root = volume + 13 * 512;
	bcopy("DIR        ", root, 11);
	root[11] = 0x10;
	put16(root + 26, 2);
	directory = volume + 27 * 512;
	bcopy(dot, directory, 11);
	directory[11] = 0x10;
	put16(directory + 26, 2);
	bcopy(dotdot, directory + 32, 11);
	directory[32 + 11] = 0x10;
}

static void
mbr_entry(unsigned char *sector, unsigned slot, uint8_t status, uint8_t type,
    uint32_t first_lba, uint32_t sectors)
{
	unsigned char *entry = sector + 446 + slot * 16;

	entry[0] = status;
	entry[4] = type;
	put32(entry + 8, first_lba);
	put32(entry + 12, sectors);
	sector[510] = 0x55;
	sector[511] = 0xaa;
}

/* A protective MBR, primary header and array, usable range, backup array
 * and backup header at the last LBA, with 128 entries in each array.
 */
static void
gpt_build(memory_disk_t *disk, uint32_t sector, const gpt_partition_t *parts,
    unsigned count)
{
	uint64_t last_lba, array_lbas;
	unsigned char *mbr, *entry;

	last_lba = disk->length / sector - 1;
	array_lbas = GPT_ENTRIES * 128 / sector;
	mbr = disk->bytes;
	mbr_entry(mbr, 0, 0x00, 0xee, 1,
	    last_lba > 0xffffffffu ? 0xffffffffu : (uint32_t)last_lba);
	for (unsigned copy = 0; copy < 2; copy++) {
		uint64_t header_lba = copy == 0 ? 1 : last_lba;
		uint64_t array_lba = copy == 0 ? 2 : last_lba - array_lbas;
		unsigned char *header = disk->bytes + header_lba * sector;

		bcopy("EFI PART", header, 8);
		put32(header + 8, 0x00010000);
		put32(header + 12, 92);
		put64(header + 24, header_lba);
		put64(header + 32, copy == 0 ? last_lba : 1);
		put64(header + 40, 2 + array_lbas);
		put64(header + 48, last_lba - array_lbas - 1);
		guid_put(header + 56, DISK_GUID);
		put64(header + 72, array_lba);
		put32(header + 80, GPT_ENTRIES);
		put32(header + 84, 128);
		for (unsigned i = 0; i < count; i++) {
			entry = disk->bytes + array_lba * sector + i * 128;
			guid_put(entry, parts[i].type_guid);
			guid_put(entry + 16, "AAAAAAAA-0000-4000-8000-00000000000A");
			entry[31] = (unsigned char)i;
			put64(entry + 32, parts[i].first_lba);
			put64(entry + 40, parts[i].last_lba);
			for (unsigned unit = 0; parts[i].name[unit] != 0; unit++)
				put16(entry + 56 + unit * 2, parts[i].name[unit]);
		}
	}
	gpt_reseal(disk, sector);
}

/* Recompute both arrays' and headers' CRCs after an edit. */
static void
gpt_reseal(memory_disk_t *disk, uint32_t sector)
{
	uint64_t last_lba = disk->length / sector - 1;

	for (unsigned copy = 0; copy < 2; copy++) {
		unsigned char *header = disk->bytes + (copy == 0 ? 1 : last_lba) *
		    sector;
		uint64_t array_lba = header[72] | (uint64_t)header[73] << 8 |
		    (uint64_t)header[74] << 16;

		put32(header + 88, crc32_oracle(disk->bytes + array_lba * sector,
		    GPT_ENTRIES * 128));
		put32(header + 16, 0);
		put32(header + 16, crc32_oracle(header, 92));
	}
}

static fsimg_status_t
disk_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_disk_t *disk = argument;

	CHECK(offset <= disk->length && length <= disk->length - offset);
	if (++disk->reads == disk->fail_read) {
		*transferred = 0;
		if (disk->short_read != 0) {
			if (disk->short_read == 1 && length > 1) {
				*transferred = length - 1;
				bcopy(disk->bytes + offset, buffer, *transferred);
			}
			return (FSIMG_OK);
		}
		return (fsimg_error_set(error, FSIMG_EIO, EIO,
		    FSIMG_OP_IO, "injected disk read failure"));
	}
	bzero(buffer, length);
	if (offset < disk->stored) {
		size_t stored = disk->stored - offset < length ?
		    (size_t)(disk->stored - offset) : length;

		bcopy(disk->bytes + offset, buffer, stored);
	}
	*transferred = length;
	return (FSIMG_OK);
}

#endif
