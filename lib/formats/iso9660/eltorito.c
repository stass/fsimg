/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "iso9660.h"

/*
 * El Torito (specs/design-iso9660.md#el-torito): the boot record found by
 * the descriptor scan and the boot catalog it names, decoded and validated
 * at open into the reader's boot summary. Boot entries are volume facts
 * for info and verification; nothing here touches a hierarchy, so a boot
 * catalog or boot image that is also a file stays that file's ordinary
 * data. This file and codec.c are the only code that knows field offsets.
 */

/* Byte offsets within an El Torito boot record descriptor. */
#define BR_SYSTEM_ID		7u	/* boot system identifier */
#define BR_BOOT_ID		39u	/* boot identifier, zero */
#define BR_CATALOG		71u	/* boot catalog block, u32le */
#define BR_ID_BYTES		32u

/* The boot catalog: 32-byte entries in its one block. */
#define ENTRY_BYTES		32u
#define CATALOG_ENTRIES		(ISO_BLOCK_BYTES / ENTRY_BYTES)	/* 64 */

/* Validation entry bytes and values. */
#define VALIDATION_HEADER_ID	0x01u
#define VALIDATION_PLATFORM	1u
#define VALIDATION_RESERVED	2u	/* two zero bytes */
#define VALIDATION_KEY		30u	/* 55 AA */
#define VALIDATION_CHECKSUM	28u

/* Initial and section entry bytes and values. */
#define BOOT_INDICATOR		0u
#define BOOT_MEDIA		1u
#define BOOT_LOAD_SEGMENT	2u
#define BOOT_SYSTEM_TYPE	4u
#define BOOT_UNUSED		5u	/* one zero byte */
#define BOOT_SECTOR_COUNT	6u
#define BOOT_IMAGE_BLOCK	8u
#define BOOTABLE		0x88u
#define NOT_BOOTABLE		0x00u
#define MEDIA_TYPE_MAX		4u	/* 0 no emulation to 4 hard disk */
#define MEDIA_EXTENSION		0x20u	/* an extension entry follows */
#define MEDIA_SECTION_FLAGS	0xe0u	/* extension and driver flags */

/* Section header bytes and values. */
#define SECTION_MORE		0x90u
#define SECTION_LAST		0x91u
#define SECTION_PLATFORM	1u
#define SECTION_COUNT		2u

/* Platform IDs besides x86 and EFI. */
#define PLATFORM_POWERPC	1u
#define PLATFORM_MAC		2u

static const char eltorito_id[BR_ID_BYTES] = "EL TORITO SPECIFICATION";

/* A boot descriptor naming the writer's hidden catalog. */
void
iso_boot_record_encode(uint32_t catalog, unsigned char *out)
{
	bzero(out, ISO_BLOCK_BYTES);
	bcopy("CD001", out + 1, 5);
	out[6] = 1;
	bcopy(eltorito_id, out + BR_SYSTEM_ID, BR_ID_BYTES);
	fsimg_store_le32(out + BR_CATALOG, catalog);
}

/* The writer packages every image unchanged, as no-emulation media. */
static void
boot_entry_encode(const iso_boot_entry_t *entry, unsigned char *out)
{
	out[BOOT_INDICATOR] = BOOTABLE;
	fsimg_store_le16(out + BOOT_SECTOR_COUNT, entry->sector_count);
	fsimg_store_le32(out + BOOT_IMAGE_BLOCK, entry->image_block);
}

/* One or both images are present. BIOS takes the initial entry; EFI is
 * either that entry alone or one entry in a final EFI section. The caller
 * supplies a block; zeroing it initializes all unused and reserved bytes.
 */
void
iso_boot_catalog_encode(const iso_boot_entry_t *bios,
    const iso_boot_entry_t *efi, unsigned char *out)
{
	uint32_t sum = 0;

	bzero(out, ISO_BLOCK_BYTES);
	out[0] = VALIDATION_HEADER_ID;
	out[VALIDATION_PLATFORM] = bios != NULL ? ISO_BOOT_X86 : ISO_BOOT_EFI;
	out[VALIDATION_KEY] = 0x55;
	out[VALIDATION_KEY + 1] = 0xaa;
	for (unsigned i = 0; i < ENTRY_BYTES; i += 2)
		sum += fsimg_load_le16(out + i);
	fsimg_store_le16(out + VALIDATION_CHECKSUM, (uint16_t)(0u - sum));
	boot_entry_encode(bios != NULL ? bios : efi, out + ENTRY_BYTES);
	if (bios != NULL && efi != NULL) {
		out[2 * ENTRY_BYTES] = SECTION_LAST;
		out[2 * ENTRY_BYTES + SECTION_PLATFORM] = ISO_BOOT_EFI;
		fsimg_store_le16(out + 2 * ENTRY_BYTES + SECTION_COUNT, 1);
		boot_entry_encode(efi, out + 3 * ENTRY_BYTES);
	}
}

/* Platform IDs of the reader profile: x86, PowerPC, Mac and EFI. */
static bool
platform_known(uint8_t platform)
{
	return (platform == ISO_BOOT_X86 || platform == PLATFORM_POWERPC ||
	    platform == PLATFORM_MAC || platform == ISO_BOOT_EFI);
}

/* The blocks a boot image's extent covers at least: the sector count is
 * in 512-byte virtual sectors and says what firmware loads, so the extent
 * holds that many, rounded up to whole blocks, and at least one block.
 */
uint64_t
iso_boot_image_blocks(uint16_t sector_count)
{
	uint64_t blocks = ISO_BLOCKS((uint64_t)sector_count * 512u);

	return (blocks == 0 ? 1 : blocks);
}

/* Decode a type-0 descriptor the scan framed. One whose boot system
 * identifier is the El Torito one records the boot catalog's block in
 * boot; any other boot record is ignored. A second El Torito boot record
 * is outside the reader profile.
 */
fsimg_status_t
iso_boot_record_decode(const unsigned char *raw, iso_boot_t *boot,
    fsimg_error_t *error)
{
	if (bcmp(raw + BR_SYSTEM_ID, eltorito_id, BR_ID_BYTES) != 0)
		return (FSIMG_OK);
	if (boot->present)
		return (iso_unsupported(error,
		    "ISO 9660 volume has two El Torito boot records"));
	if (!iso_bytes_zero(raw + BR_BOOT_ID, BR_ID_BYTES))
		return (iso_corrupt(error,
		    "El Torito boot record has a boot identifier"));
	boot->present = true;
	boot->catalog_block = fsimg_load_le32(raw + BR_CATALOG);
	return (FSIMG_OK);
}

/* The validation entry: header ID, a known platform, zero reserved bytes,
 * the key, and sixteen u16le words summing to zero modulo 65536.
 */
static fsimg_status_t
validation_decode(const unsigned char *entry, uint8_t *platform,
    fsimg_error_t *error)
{
	uint32_t sum = 0;

	if (entry[0] != VALIDATION_HEADER_ID ||
	    entry[VALIDATION_KEY] != 0x55 || entry[VALIDATION_KEY + 1] != 0xaa)
		return (iso_corrupt(error,
		    "El Torito validation entry lacks its signature"));
	if (!iso_bytes_zero(entry + VALIDATION_RESERVED, 2))
		return (iso_corrupt(error,
		    "El Torito validation entry has reserved bytes set"));
	for (unsigned i = 0; i < ENTRY_BYTES; i += 2)
		sum += fsimg_load_le16(entry + i);
	if ((sum & 0xffffu) != 0)
		return (iso_corrupt(error,
		    "El Torito validation entry checksum does not match"));
	*platform = entry[VALIDATION_PLATFORM];
	if (!platform_known(*platform))
		return (iso_unsupported(error,
		    "unsupported El Torito platform"));
	return (FSIMG_OK);
}

/* An initial or section entry for platform, into out. Both carry the
 * indicator, the media type in 0 to 4, a zero unused byte and the image's
 * load segment, system type, sector count and block. The initial entry's
 * byte 1 is the media type alone; a section entry's byte 1 also holds the
 * extension flag, outside the reader profile, and driver flags in bits 6
 * and 7, while its reserved bit 4 stays in the media type and fails its
 * range. Bytes 12 to 31 are not checked: they hold a section entry's
 * selection criteria, and producers write criteria into the initial entry
 * too. The image starts at or after data_start, the first block after the
 * descriptor set; its end is not bound to the volume (iso_boot_t).
 */
static fsimg_status_t
boot_entry_decode(const unsigned char *entry, uint8_t platform, bool initial,
    uint32_t data_start, iso_boot_entry_t *out, fsimg_error_t *error)
{
	uint8_t media = entry[BOOT_MEDIA];

	if (entry[BOOT_INDICATOR] != BOOTABLE &&
	    entry[BOOT_INDICATOR] != NOT_BOOTABLE)
		return (iso_corrupt(error,
		    "El Torito boot entry has an invalid boot indicator"));
	if (!initial) {
		if ((media & MEDIA_EXTENSION) != 0)
			return (iso_unsupported(error,
			    "El Torito extension entries are not supported"));
		media &= ~MEDIA_SECTION_FLAGS;
	}
	if (media > MEDIA_TYPE_MAX)
		return (iso_corrupt(error,
		    "El Torito boot entry has an invalid media type"));
	if (entry[BOOT_UNUSED] != 0)
		return (iso_corrupt(error,
		    "El Torito boot entry has an unused byte set"));
	*out = (iso_boot_entry_t){
		.image_block = fsimg_load_le32(entry + BOOT_IMAGE_BLOCK),
		.load_segment = fsimg_load_le16(entry + BOOT_LOAD_SEGMENT),
		.sector_count = fsimg_load_le16(entry + BOOT_SECTOR_COUNT),
		.platform = platform,
		.media_type = media,
		.system_type = entry[BOOT_SYSTEM_TYPE],
		.bootable = entry[BOOT_INDICATOR] == BOOTABLE
	};
	if (out->image_block < data_start)
		return (iso_corrupt(error,
		    "El Torito boot image lies before the data area"));
	return (FSIMG_OK);
}

static bool
section_header(const unsigned char *entry)
{
	return (entry[0] == SECTION_MORE || entry[0] == SECTION_LAST);
}

/* A section header: its platform, its count of section entries, at least
 * one, and whether it is marked last.
 */
static fsimg_status_t
section_header_decode(const unsigned char *entry, uint8_t *platform,
    unsigned *count, bool *last, fsimg_error_t *error)
{
	if (!section_header(entry))
		return (iso_corrupt(error, "El Torito boot catalog "
		    "lacks a section header after a section"));
	*last = entry[0] == SECTION_LAST;
	*platform = entry[SECTION_PLATFORM];
	*count = fsimg_load_le16(entry + SECTION_COUNT);
	if (*count == 0)
		return (iso_corrupt(error,
		    "El Torito section header has no entries"));
	if (!platform_known(*platform))
		return (iso_unsupported(error,
		    "unsupported El Torito platform"));
	return (FSIMG_OK);
}

/* Decode the boot catalog block into boot's entries under the boot-catalog
 * rule: the validation and initial entries, then, when entry 2 is a
 * section header, headers each followed by its count of section entries
 * until the entries of a header marked last. Each step of the walk reads
 * the next entry: a header once the current header's entries are used up,
 * a section entry otherwise. The walk therefore ends within the block's
 * 64 entries, and a catalog that would continue past them is outside the
 * reader profile. A count running past a section's real entries reads
 * whatever follows as entries; the first that is not one fails, zero
 * bytes as an image at block 0, before the data area.
 */
static fsimg_status_t
catalog_decode(const unsigned char *block, uint32_t data_start,
    iso_boot_t *boot, fsimg_error_t *error)
{
	iso_boot_entry_t entry;
	const unsigned char *raw;
	uint8_t platform = block[VALIDATION_PLATFORM];
	unsigned remaining;
	bool last;
	fsimg_status_t status;

	/* Entry 2 is a header, so section entries occupy at most 3 to 63. */
	_Static_assert(ISO_BOOT_ENTRIES_MAX == CATALOG_ENTRIES - 2,
	    "every boot entry of one catalog block fits the boot summary");
	status = validation_decode(block, &platform, error);
	if (status == FSIMG_OK)
		status = boot_entry_decode(block + ENTRY_BYTES, platform, true,
		    data_start, &entry, error);
	if (status != FSIMG_OK)
		return (status);
	boot->entries[0] = entry;
	boot->entry_count = 1;
	if (!section_header(block + 2 * ENTRY_BYTES))
		return (FSIMG_OK);
	remaining = 0;
	last = false;
	for (unsigned index = 2; remaining > 0 || !last; index++) {
		if (index == CATALOG_ENTRIES)
			return (iso_unsupported(error, "El Torito boot catalog "
			    "continues past its first block"));
		raw = block + index * ENTRY_BYTES;
		if (remaining == 0) {
			status = section_header_decode(raw, &platform,
			    &remaining, &last, error);
			if (status != FSIMG_OK)
				return (status);
			continue;
		}
		status = boot_entry_decode(raw, platform, false, data_start,
		    &entry, error);
		if (status != FSIMG_OK)
			return (status);
		boot->entries[boot->entry_count] = entry;
		boot->entry_count++;
		remaining--;
	}
	return (FSIMG_OK);
}

/* Step 4 of open (specs/design-iso9660-driver.md#probe-and-open): when the
 * descriptor set holds an El Torito boot record, read the boot catalog's
 * one block and decode it. The catalog lies after the descriptor set,
 * where data lives, and within the volume and the slice, since it is read.
 * Boot images are not read: decoding holds each to start after the
 * descriptor set, and an image may run past the volume, as an EFI image in
 * an appended partition does.
 */
fsimg_status_t
iso_boot_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_boot_t *boot = &iso->boot;
	fsimg_status_t status;

	if (!boot->present)
		return (FSIMG_OK);
	if (boot->catalog_block <= iso->terminator_block)
		return (iso_corrupt(error,
		    "El Torito boot catalog lies before the data area"));
	status = iso_extent_check(iso, boot->catalog_block, 1, error);
	if (status == FSIMG_OK)
		status = iso_block_read(reader, boot->catalog_block, error);
	if (status != FSIMG_OK)
		return (status);
	return (catalog_decode(iso->window.bytes, iso->terminator_block + 1,
	    boot, error));
}
