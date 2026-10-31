/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"
#include "fsimg-endian.h"
#include "unicode.h"

/* The BPB fields, packed FAT interpretation and long-name checksum follow the
 * author's libarchive msdosfs reader. Geometry, resources, traversal and time
 * conversion use fsimg's independent contracts rather than libarchive state.
 */
#include "name-tables.h"

static fsimg_status_t entry_timestamp(fsimg_metadata_t *, uint32_t,
    uint16_t, uint16_t, unsigned, fsimg_error_t *);
fsimg_status_t
fsimg_fat_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER, message));
}

bool
fsimg_fat_cluster_valid(const fat_geometry_t *g, uint32_t cluster)
{
	return (cluster >= 2 && cluster - 2 < g->clusters);
}

uint64_t
fsimg_fat_cluster_offset(const fat_geometry_t *g, uint32_t cluster)
{
	return (g->data_offset + (uint64_t)(cluster - 2) * g->cluster_bytes);
}

fsimg_status_t
fsimg_fat_geometry_decode(const unsigned char *boot, uint64_t input_bytes,
    fat_geometry_t *g, fsimg_error_t *error)
{
	static const unsigned char exfat_reserved[53] = { 0 };
	uint64_t total_sectors, fat_sectors, root_sectors, data_sector, capacity;
	unsigned sectors_per_cluster;
	uint16_t flags;

	bzero(g, sizeof(*g));
	/* The identity signature is the jump, the boot signature and a legal
	 * fixed BPB. Without it this is not a FAT volume (FSIMG_EFORMAT), which
	 * keeps other boot records that carry a BPB, such as an MBR or NTFS,
	 * from being reported as corrupt FAT. Every later check is corruption
	 * of a recognized FAT volume.
	 */
	if (!((boot[0] == 0xeb && boot[2] == 0x90) || boot[0] == 0xe9) ||
	    boot[510] != 0x55 || boot[511] != 0xaa)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "unrecognized volume boot record"));
	/* exFAT zeroes the legacy BPB region through byte 63. OEM text alone is
	 * insufficient: a FAT formatter may legally use the same eight bytes.
	 */
	if (bcmp(boot + 3, "EXFAT   ", 8) == 0 &&
	    bcmp(boot + 11, exfat_reserved, sizeof(exfat_reserved)) == 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "exFAT boot record is not a FAT12/16/32 volume"));
	g->sector_bytes = fsimg_load_le16(boot + 11);
	sectors_per_cluster = boot[13];
	g->reserved_sectors = fsimg_load_le16(boot + 14);
	g->tables = boot[16];
	g->root_entries = fsimg_load_le16(boot + 17);
	g->media = boot[21];
	if ((g->sector_bytes != 512 && g->sector_bytes != 1024 &&
	    g->sector_bytes != 2048 && g->sector_bytes != 4096) ||
	    sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
	    (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
	    g->reserved_sectors == 0 || g->tables == 0 ||
	    (g->media != 0xf0 && g->media < 0xf8))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "boot record has no legal FAT BPB"));
	if (fsimg_load_le16(boot + 19) != 0 && fsimg_load_le32(boot + 32) != 0)
		return (fsimg_fat_corrupt(error, "conflicting FAT sector counts"));
	total_sectors = fsimg_load_le16(boot + 19);
	if (total_sectors == 0)
		total_sectors = fsimg_load_le32(boot + 32);
	fat_sectors = fsimg_load_le16(boot + 22);
	if (fat_sectors == 0)
		fat_sectors = fsimg_load_le32(boot + 36);
	g->volume_bytes = total_sectors * g->sector_bytes;
	g->fat_bytes = fat_sectors * g->sector_bytes;
	g->cluster_bytes = sectors_per_cluster * g->sector_bytes;
	root_sectors = ((uint64_t)g->root_entries * 32 + g->sector_bytes - 1) /
	    g->sector_bytes;
	data_sector = g->reserved_sectors + g->tables * fat_sectors + root_sectors;
	/* The BPB fields are at most 32 bits and sectors at most 4096 bytes.
	 * Keep region arithmetic in 64 bits, subtract metadata only after proving
	 * it fits, and classify by data clusters rather than the label string.
	 */
	if (fat_sectors == 0 || total_sectors <= data_sector ||
	    g->volume_bytes > input_bytes)
		return (fsimg_fat_corrupt(error, "FAT regions exceed the bounded volume"));
	g->clusters = (uint32_t)((total_sectors - data_sector) / sectors_per_cluster);
	if (g->clusters == 0)
		return (fsimg_fat_corrupt(error, "FAT volume has no data clusters"));
	g->variant = g->clusters < 4085 ? 12 : g->clusters < 65525 ? 16 : 32;
	g->bad_value = g->variant == 12 ? 0xff7 :
	    g->variant == 16 ? 0xfff7 : 0x0ffffff7;
	g->eoc_value = g->bad_value + 1;
	/* MAX is CountOfClusters + 1. Only links above MAX and below the bad
	 * marker are reserved; near a type threshold, 0xff0/0xfff0 can be valid
	 * cluster numbers. The bad marker itself can never be allocated.
	 */
	capacity = ((uint64_t)g->clusters + 2) * g->variant;
	if ((capacity + 7) / 8 > g->fat_bytes ||
	    g->clusters > g->bad_value - 2)
		return (fsimg_fat_corrupt(error, "FAT cannot describe its data region"));
	/* Any nonzero table count is legal FAT, and the regions above already
	 * account for every table. The supported profile reads one or two, so a
	 * larger count is a recognized volume this reader does not support. The
	 * same bound sizes the reader's mirrored table window.
	 */
	if (g->tables > FAT_READ_TABLES)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "FAT table count exceeds the supported profile"));
	g->fat_offset = (uint64_t)g->reserved_sectors * g->sector_bytes;
	g->root_offset = (g->reserved_sectors + g->tables * fat_sectors) *
	    g->sector_bytes;
	g->data_offset = data_sector * g->sector_bytes;
	g->mirrored = true;
	if (g->variant != 32) {
		if (g->root_entries == 0 || fsimg_load_le16(boot + 22) == 0)
			return (fsimg_fat_corrupt(error, "FAT12/16 requires a fixed root and FAT"));
		return (FSIMG_OK);
	}
	if (g->root_entries != 0 || fsimg_load_le16(boot + 22) != 0 ||
	    fsimg_load_le16(boot + 19) != 0)
		return (fsimg_fat_corrupt(error, "inconsistent FAT32 BPB fields"));
	if (fsimg_load_le16(boot + 42) != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported FAT32 revision"));
	flags = fsimg_load_le16(boot + 40);
	/* FAT32 can explicitly disable mirroring and select a single active FAT.
	 * Inactive tables are then neither allocation authority nor copy oracles.
	 */
	if ((flags & ~0x008fu) != 0)
		return (fsimg_fat_corrupt(error, "invalid FAT32 table flags"));
	g->mirrored = (flags & 0x80) == 0;
	g->active_table = g->mirrored ? 0 : flags & 0x0f;
	g->root_cluster = fsimg_load_le32(boot + 44);
	g->fsinfo_sector = fsimg_load_le16(boot + 48);
	g->backup_sector = fsimg_load_le16(boot + 50);
	if (g->active_table >= g->tables || !fsimg_fat_cluster_valid(g, g->root_cluster) ||
	    (g->fsinfo_sector != 0xffff &&
	    (g->fsinfo_sector == 0 || g->fsinfo_sector >= g->reserved_sectors)) ||
	    (g->backup_sector != 0 && g->backup_sector != 0xffff &&
	    g->backup_sector >= g->reserved_sectors))
		return (fsimg_fat_corrupt(error, "invalid FAT32 root or reserved structures"));
	for (unsigned i = 52; i < 64; i++) {
		if (boot[i] != 0)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_READER, "unsupported FAT32 reserved fields"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_name_validate(const char *name, fsimg_error_t *error)
{
	size_t length, units;
	fsimg_status_t status;

	length = strlen(name);
	if (length == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
	    name[0] == ' ' || name[length - 1] == '.' || name[length - 1] == ' ')
		return (fsimg_fat_corrupt(error, "invalid FAT filename component"));
	/* FAT lookup ignores surrounding spaces and trailing periods. Reject
	 * noncanonical spellings so displayed paths and lookup keys cannot diverge.
	 * Revalidate encoding here because replay also consumes private records.
	 */
	status = fsimg_utf8_to_utf16(name, length, NULL, 0, &units, error);
	if (status != FSIMG_OK || units > 255)
		return (fsimg_fat_corrupt(error, "invalid FAT filename encoding or length"));
	for (size_t i = 0; i < length; i++) {
		if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f ||
		    strchr("\"*/:<>?\\|", name[i]) != NULL)
			return (fsimg_fat_corrupt(error, "forbidden character in FAT filename"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_short_name(const unsigned char *raw, char *name, fsimg_error_t *error)
{
	uint16_t units[12];
	size_t count, end, written;
	unsigned char byte;
	fsimg_status_t status;

	if (raw[0] == ' ')
		return (fsimg_fat_corrupt(error, "FAT short name starts with a space"));
	if ((raw[12] & ~0x18u) != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported FAT short-name flags"));
	count = 0;
	for (unsigned group = 0; group < 2; group++) {
		unsigned start = group == 0 ? 0 : 8;
		end = group == 0 ? 8 : 11;
		while (end > start && raw[end - 1] == ' ')
			end--;
		if (group == 0 && end == 0)
			return (fsimg_fat_corrupt(error, "empty FAT short basename"));
		if (group == 1 && end != start)
			units[count++] = '.';
		for (size_t i = start; i < end; i++) {
			byte = raw[i];
			if (i == 0 && byte == 0x05)
				byte = 0xe5;
			if (byte < 0x20 || byte == 0x7f ||
			    strchr("\"*+,./:;<=>?[\\]|", byte) != NULL)
				return (fsimg_fat_corrupt(error, "invalid FAT short-name byte"));
			units[count++] = (raw[12] & (group == 0 ? 0x08 : 0x10)) != 0 ?
			    fat_cp437_lower[byte] : fat_cp437[byte];
		}
	}
	status = fsimg_utf16_to_utf8(units, count, name, FAT_NAME_BYTES - 1,
	    &written, error);
	if (status != FSIMG_OK)
		return (fsimg_fat_corrupt(error, "invalid FAT short-name encoding"));
	name[written] = '\0';
	return (fsimg_fat_name_validate(name, error));
}

fsimg_status_t
fsimg_fat_lfn_slot(fat_lfn_t *lfn, const unsigned char *raw, fsimg_error_t *error)
{
	static const unsigned offsets[13] = {
		1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
	};
	unsigned ordinal;

	ordinal = raw[0] & 0x1f;
	if ((raw[0] & ~0x5fu) != 0 || ordinal == 0 || ordinal > 20 ||
	    raw[12] != 0 || fsimg_load_le16(raw + 26) != 0)
		return (fsimg_fat_corrupt(error, "invalid FAT long-name slot"));
	if ((raw[0] & 0x40) != 0) {
		if (lfn->active)
			return (fsimg_fat_corrupt(error, "overlapping FAT long-name sequences"));
		lfn->active = true;
		lfn->slots = ordinal;
		lfn->remaining = ordinal;
		lfn->checksum = raw[13];
	}
	if (!lfn->active || ordinal != lfn->remaining || raw[13] != lfn->checksum)
		return (fsimg_fat_corrupt(error, "disordered FAT long-name sequence"));
	for (unsigned i = 0; i < 13; i++)
		lfn->units[(ordinal - 1) * 13 + i] = fsimg_load_le16(raw + offsets[i]);
	lfn->remaining--;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_entry_name(fat_lfn_t *lfn, const unsigned char *raw, char *name,
    fsimg_error_t *error)
{
	size_t length, total, written;
	unsigned char checksum;
	fsimg_status_t status;

	if (!lfn->active)
		return (fsimg_fat_short_name(raw, name, error));
	checksum = 0;
	for (unsigned i = 0; i < 11; i++)
		checksum = (unsigned char)((checksum >> 1) | (checksum << 7)) + raw[i];
	if (lfn->remaining != 0 || checksum != lfn->checksum)
		return (fsimg_fat_corrupt(error, "FAT long-name checksum or count mismatch"));
	total = lfn->slots * 13;
	/* Full slots need no terminator. A partial final slot has one NUL followed
	 * only by 0xffff; accepting embedded padding would create alternate names.
	 */
	length = 0;
	while (length < total && lfn->units[length] != 0) {
		if (lfn->units[length] == 0xffff)
			return (fsimg_fat_corrupt(error, "unterminated FAT long-name padding"));
		length++;
	}
	if (length == 0 || length > 255 || length <= (lfn->slots - 1) * 13)
		return (fsimg_fat_corrupt(error, "invalid FAT long-name length"));
	for (size_t i = length + 1; i < total; i++) {
		if (lfn->units[i] != 0xffff)
			return (fsimg_fat_corrupt(error, "invalid FAT long-name padding"));
	}
	status = fsimg_utf16_to_utf8(lfn->units, length, name, FAT_NAME_BYTES - 1,
	    &written, error);
	if (status != FSIMG_OK)
		return (fsimg_fat_corrupt(error, "invalid FAT long-name UTF-16"));
	name[written] = '\0';
	lfn->active = false;
	return (fsimg_fat_name_validate(name, error));
}

fsimg_status_t
fsimg_fat_name_key(const char *name, unsigned char *key, size_t *length,
    fsimg_error_t *error)
{
	uint16_t units[255];
	uint32_t scalar;
	size_t count, written;
	fsimg_status_t status;

	status = fsimg_utf8_to_utf16(name, strlen(name), units, 255, &count, error);
	if (status != FSIMG_OK)
		return (fsimg_fat_corrupt(error, "invalid FAT name in collision stream"));
	written = 0;
	/* Conversion validates surrogate pairs. Combine each pair into a scalar
	 * before case mapping so supplementary names have the same comparison rule.
	 */
	for (size_t i = 0; i < count; i++) {
		scalar = units[i];
		if (scalar >= 0xd800 && scalar <= 0xdbff) {
			scalar = 0x10000 + ((scalar - 0xd800) << 10) +
			    (units[i + 1] - 0xdc00);
			i++;
		}
		scalar = fsimg_unicode_upper(scalar);
		/* Fixed-width big-endian scalars give an unambiguous byte key. */
		for (unsigned j = 0; j < 4; j++)
			key[written++] = (unsigned char)(scalar >> (24 - j * 8));
	}
	*length = written;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_entry_decode(const fat_geometry_t *g, const unsigned char *raw,
    fsimg_entry_t *entry, uint32_t *cluster, fsimg_error_t *error)
{
	fsimg_metadata_t *metadata;
	fsimg_status_t status;

	bzero(entry, sizeof(*entry));
	if ((raw[11] & 0xc0) != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported FAT entry attributes"));
	if ((raw[11] & 0x08) != 0)
		return (fsimg_fat_corrupt(error, "volume label used as an ordinary entry"));
	*cluster = fsimg_load_le16(raw + 26);
	if (g->variant == 32) {
		if ((fsimg_load_le16(raw + 20) & 0xf000) != 0)
			return (fsimg_fat_corrupt(error, "reserved FAT32 first-cluster bits"));
		*cluster |= (uint32_t)fsimg_load_le16(raw + 20) << 16;
	} else if (fsimg_load_le16(raw + 20) != 0) {
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "FAT extended attributes are unsupported"));
	}
	entry->kind = (raw[11] & 0x10) != 0 ?
	    FSIMG_ENTRY_DIRECTORY : FSIMG_ENTRY_FILE;
	entry->size = fsimg_load_le32(raw + 28);
	if ((entry->kind == FSIMG_ENTRY_DIRECTORY && entry->size != 0) ||
	    ((entry->kind == FSIMG_ENTRY_DIRECTORY || entry->size != 0) &&
	    !fsimg_fat_cluster_valid(g, *cluster)) ||
	    (entry->kind == FSIMG_ENTRY_FILE && entry->size == 0 && *cluster != 0) ||
	    (entry->size + g->cluster_bytes - 1) / g->cluster_bytes > g->clusters)
		return (fsimg_fat_corrupt(error, "invalid FAT entry size or first cluster"));
	metadata = &entry->metadata;
	metadata->valid = FSIMG_META_ATTRIBUTES;
	metadata->attributes = (raw[11] & 7) | ((raw[11] & 0x20) != 0 ? 8 : 0);

	status = entry_timestamp(metadata, FSIMG_META_CREATED, fsimg_load_le16(raw + 16),
	    fsimg_load_le16(raw + 14), raw[13], error);
	if (status != FSIMG_OK)
		return (status);
	status = entry_timestamp(metadata, FSIMG_META_MODIFIED, fsimg_load_le16(raw + 24),
	    fsimg_load_le16(raw + 22), 0, error);
	if (status != FSIMG_OK)
		return (status);
	return (entry_timestamp(metadata, FSIMG_META_ACCESSED, fsimg_load_le16(raw + 18),
	    0, 0, error));
}

static fsimg_status_t
entry_timestamp(fsimg_metadata_t *metadata, uint32_t field, uint16_t date,
    uint16_t time, unsigned subseconds, fsimg_error_t *error)
{
	fsimg_timestamp_t *timestamp;
	uint64_t precision;

	if (date == 0 && time == 0 && subseconds == 0)
		return (FSIMG_OK);
	switch (field) {
	case FSIMG_META_CREATED:
		timestamp = &metadata->created;
		precision = 10000000;
		break;
	case FSIMG_META_MODIFIED:
		timestamp = &metadata->modified;
		precision = 2000000000;
		break;
	case FSIMG_META_ACCESSED:
		timestamp = &metadata->accessed;
		precision = UINT64_C(86400000000000);
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "invalid FAT timestamp field"));
	}
	if (!fsimg_dos_time_decode(date, time, subseconds, precision, timestamp))
		return (fsimg_fat_corrupt(error, "invalid FAT calendar timestamp"));
	metadata->valid |= field;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_label(const unsigned char *raw, char *label, fsimg_error_t *error)
{
	uint16_t units[11];
	size_t length, written;
	fsimg_status_t status;

	length = 11;
	while (length != 0 && raw[length - 1] == ' ')
		length--;
	for (size_t i = 0; i < length; i++) {
		if (raw[i] < 0x20 || raw[i] == 0x7f)
			return (fsimg_fat_corrupt(error, "invalid FAT volume label"));
		units[i] = fat_cp437[raw[i]];
	}
	status = fsimg_utf16_to_utf8(units, length, label, 47, &written, error);
	if (status != FSIMG_OK)
		return (status);
	label[written] = '\0';
	return (FSIMG_OK);
}
