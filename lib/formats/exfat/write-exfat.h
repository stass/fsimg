/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_EXFAT_H
#define FSIMG_WRITE_EXFAT_H

#include "exfat.h"

/* Row wire layout: length, initialized length and parent slot (u64), first
 * cluster and allocation count (u32), then the 32-byte primary entry. During
 * sizing directory length counts occupied bytes; allocation rounds it once.
 */
typedef struct exfat_layout {
	uint64_t	 length;
	uint64_t	 valid_length;
	uint64_t	 parent_slot;
	uint32_t	 first;
	uint32_t	 clusters;
	unsigned char	 entry[32];
} exfat_layout_t;

typedef struct exfat_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*layout;
	unsigned char	*upcase;
	exfat_geometry_t geometry;
	uint64_t	 bitmap_length;
	uint32_t	 upcase_length;
	uint32_t	 bitmap_clusters;
	uint32_t	 upcase_clusters;
	uint32_t	 root_cluster;
	uint32_t	 allocated;
	uint32_t	 upcase_checksum;
} exfat_plan_t;

fsimg_status_t exfat_layout_get(exfat_plan_t *, uint64_t, exfat_layout_t *,
    fsimg_error_t *);
fsimg_status_t exfat_layout_set(exfat_plan_t *, uint64_t, const exfat_layout_t *,
    fsimg_error_t *);
fsimg_status_t exfat_write_emit(fsimg_writer_t *, fsimg_emit_t *, fsimg_error_t *);

#endif
