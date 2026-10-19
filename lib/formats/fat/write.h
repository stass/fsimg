/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_FAT_WRITE_H
#define FSIMG_FAT_WRITE_H

#include "fat.h"
#include "writer.h"

#define FAT_LAYOUT_BYTES 64u
#define FAT_WRITE_DIRECTORY_MAX (2u * 1024u * 1024u)
#define FAT_AUTO_TYPE_MARGIN 16u

/* Layout wire record: length, slots, parent slot (u64), first/count (u32),
 * then the encoded 32-byte short entry. The root has no parent slot.
 * length is payload bytes for files, occupied slot bytes for directories.
 * slots == 0 denotes a regular file; directory slots include their headers
 * and zero terminator, so even an empty directory has a positive slot count.
 */
typedef struct fat_layout {
	uint64_t	 length;
	uint64_t	 slots;
	uint64_t	 parent_slot;
	uint32_t	 first;
	uint32_t	 clusters;
	unsigned char	 entry[32];
} fat_layout_t;

typedef struct fat_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*layout;
	uint64_t	 fat_offset;
	uint64_t	 fat_bytes;
	uint64_t	 root_offset;
	uint64_t	 data_offset;
	uint32_t	 clusters;
	uint32_t	 allocated;
	uint32_t	 root_entries;
	uint32_t	 reserved_sectors;
	unsigned	 variant;
} fat_plan_t;

bool fsimg_fat_write_short_name(const char *, unsigned char *);
fsimg_status_t fsimg_fat_layout_get(fat_plan_t *, uint64_t, fat_layout_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_fat_layout_set(fat_plan_t *, uint64_t,
    const fat_layout_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_write_names(fat_plan_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_write_metadata(fat_plan_t *, const fsimg_entry_t *,
    unsigned char *, fsimg_error_t *);
fsimg_status_t fsimg_fat_write_plan(fsimg_writer_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);
void fsimg_fat_write_free(fsimg_plan_t *);
fsimg_status_t fsimg_fat_write_validate(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);

#endif
