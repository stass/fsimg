/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "exfat.h"

static fsimg_status_t boot_region(fsimg_reader_t *, uint64_t,
    exfat_geometry_t *, fsimg_error_t *);

static fsimg_status_t
boot_region(fsimg_reader_t *reader, uint64_t base, exfat_geometry_t *g,
    fsimg_error_t *error)
{
	unsigned char boot[512], sector[4096];
	uint64_t volume_sectors, fat_sector, fat_sectors, heap_sector, expected;
	uint64_t input_length;
	uint32_t sum;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &input_length, error);
	if (status != FSIMG_OK)
		return (status);
	if (input_length < EXFAT_VOLUME_MIN)
		return (exfat_corrupt(error, "input is smaller than an exFAT volume"));
	status = fsimg_reader_bytes(reader, base, boot, sizeof(boot), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (bcmp(boot, "\xeb\x76\x90" "EXFAT   ", 11) != 0 ||
	    boot[510] != 0x55 || boot[511] != 0xaa)
		return (exfat_corrupt(error, "invalid exFAT boot identity or signature"));
	if (boot[108] < 9 || boot[108] > 12 || boot[109] > 25 - boot[108])
		return (exfat_corrupt(error, "invalid exFAT sector or cluster shift"));
	g->sector_bytes = 1u << boot[108];
	g->cluster_bytes = 1u << (boot[108] + boot[109]);

	/* Only the bounded sector size is used before checksum validation. Every
	 * other field remains untrusted until its entire boot region is checked.
	 */
	sum = 0;
	for (unsigned i = 0; i < 12; i++) {
		status = fsimg_reader_bytes(reader, base + (uint64_t)i * g->sector_bytes,
		    sector, g->sector_bytes, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		if (i == 11) {
			for (unsigned j = 0; j < g->sector_bytes; j += 4) {
				if (exfat_u32(sector + j) != sum)
					return (exfat_corrupt(error, "invalid exFAT boot checksum"));
			}
			break;
		}
		if (i >= 1 && i <= 8 &&
		    exfat_u32(sector + g->sector_bytes - 4) != UINT32_C(0xaa550000))
			return (exfat_corrupt(error, "invalid exFAT extended boot signature"));
		for (unsigned j = 0; j < g->sector_bytes; j++) {
			if (i == 0 && (j == 106 || j == 107 || j == 112))
				continue;
			sum = exfat_checksum32(sum, sector[j]);
		}
	}
	for (unsigned i = 11; i < 64; i++) {
		if (boot[i] != 0)
			return (exfat_corrupt(error, "nonzero exFAT MustBeZero bytes"));
	}
	if (boot[110] < 1 || boot[110] > 2 ||
	    (exfat_u16(boot + 104) >> 8) == 0 ||
	    (exfat_u16(boot + 104) >> 8) > 99 ||
	    (exfat_u16(boot + 104) & 255) > 99)
		return (exfat_corrupt(error, "invalid exFAT revision or FAT count"));
	if (boot[110] == 2 || (exfat_u16(boot + 104) >> 8) != 1)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_READER, "unsupported exFAT revision or TexFAT allocation"));
	if (boot[110] != 1 || (exfat_u16(boot + 104) & 255) > 99 ||
	    (exfat_u16(boot + 106) & 1) != 0 ||
	    (boot[112] > 100 && boot[112] != 255))
		return (exfat_corrupt(error, "invalid exFAT boot flags or revision"));
	volume_sectors = fsimg_meta_get_u64(boot + 72);
	fat_sector = exfat_u32(boot + 80);
	fat_sectors = exfat_u32(boot + 84);
	heap_sector = exfat_u32(boot + 88);
	g->clusters = exfat_u32(boot + 92);
	g->root_cluster = exfat_u32(boot + 96);
	g->flags = exfat_u16(boot + 106);
	if (!fsimg_mul_u64(volume_sectors, g->sector_bytes, &g->volume_bytes) ||
	    g->volume_bytes > input_length || g->volume_bytes > INT64_MAX ||
	    g->volume_bytes < EXFAT_VOLUME_MIN || fat_sector < 24 || fat_sectors == 0 ||
	    fat_sector + fat_sectors > heap_sector || heap_sector >= volume_sectors ||
	    g->clusters < 3 || g->clusters > EXFAT_CLUSTER_MAX ||
	    !exfat_cluster_valid(g, g->root_cluster))
		return (exfat_corrupt(error, "invalid exFAT volume geometry"));
	expected = (volume_sectors - heap_sector) >> boot[109];
	if (expected > EXFAT_CLUSTER_MAX)
		expected = EXFAT_CLUSTER_MAX;
	g->fat_offset = fat_sector * g->sector_bytes;
	g->fat_bytes = fat_sectors * g->sector_bytes;
	g->heap_offset = heap_sector * g->sector_bytes;
	if (g->clusters != expected ||
	    ((uint64_t)g->clusters + 2) * 4 > g->fat_bytes)
		return (exfat_corrupt(error, "inconsistent exFAT heap or FAT capacity"));
	return (FSIMG_OK);
}

fsimg_status_t
exfat_boot_read(fsimg_reader_t *reader, fsimg_error_t *error)
{
	exfat_geometry_t main, backup;
	fsimg_status_t status;

	status = boot_region(reader, 0, &main, error);
	if (status != FSIMG_OK)
		return (status);
	status = boot_region(reader, (uint64_t)12 * main.sector_bytes, &backup, error);
	if (status != FSIMG_OK)
		return (status);
	if (main.volume_bytes != backup.volume_bytes ||
	    main.fat_offset != backup.fat_offset || main.fat_bytes != backup.fat_bytes ||
	    main.heap_offset != backup.heap_offset || main.clusters != backup.clusters ||
	    main.sector_bytes != backup.sector_bytes ||
	    main.cluster_bytes != backup.cluster_bytes ||
	    main.root_cluster != backup.root_cluster)
		return (exfat_corrupt(error, "exFAT backup geometry disagrees"));
	exfat_state(reader)->geometry = main;
	return (FSIMG_OK);
}
