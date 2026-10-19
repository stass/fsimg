/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_FAT_H
#define FSIMG_FAT_H

#include "reader.h"

#define FAT_NAME_BYTES 1021u
/* A name key: four bytes per scalar of at most 255 UTF-16 units. */
#define FAT_KEY_BYTES FSIMG_DIRECTORY_KEY_BYTES
#define FAT_DIRECTORY_LIMIT (2u * 1024u * 1024u)

typedef struct fat_geometry {
	uint64_t	 volume_bytes;
	uint64_t	 fat_offset;
	uint64_t	 fat_bytes;
	uint64_t	 root_offset;
	uint64_t	 data_offset;
	uint32_t	 sector_bytes;
	uint32_t	 cluster_bytes;
	uint32_t	 clusters;
	uint32_t	 root_cluster;
	uint32_t	 root_entries;
	uint32_t	 reserved_sectors;
	uint32_t	 fsinfo_sector;
	uint32_t	 backup_sector;
	uint32_t	 bad_value;
	uint32_t	 eoc_value;
	unsigned	 variant;
	unsigned	 tables;
	unsigned	 active_table;
	unsigned char	 media;
	bool		 mirrored;
} fat_geometry_t;

/* Brent's detector and the geometry step limit bound every forward chain.
 * expected is zero for directories, exact allocation length for files.
 */
typedef struct fat_chain {
	uint64_t	 steps;
	uint64_t	 expected;
	uint64_t	 power;
	uint64_t	 distance;
	uint32_t	 cluster;
	uint32_t	 checkpoint;
} fat_chain_t;

typedef struct fat_lfn {
	uint16_t	 units[260];
	unsigned	 slots;
	unsigned	 remaining;
	unsigned char	 checksum;
	bool		 active;
} fat_lfn_t;

/* Driver state behind the reader's private pointer, from open to close. */
typedef struct fsimg_fat_reader fsimg_fat_reader_t;

/* The current locator is the entry's first cluster and the cluster its
 * ".." record names, set by replay; content state belongs to it.
 */
struct fsimg_fat_reader {
	fat_geometry_t	 geometry;
	fsimg_bitset_t	*directory_clusters;
	fsimg_stream_t	*owners;
	fsimg_extent_index_t *index;
	fat_chain_t	 file_chain;
	fsimg_extent_t	 extent;
	uint64_t	 table_window_offset;
	size_t		 table_window_length;
	uint32_t	 current_cluster;
	uint32_t	 current_parent;
	bool		 file_started;
	unsigned char	 table_window[2][4096];
	unsigned char	 directory_window[4096];
	char		 root_label[48];
	bool		 root_label_seen;
};

uint16_t fsimg_fat_u16(const unsigned char *);
uint32_t fsimg_fat_u32(const unsigned char *);
void fsimg_fat_put16(unsigned char *, uint16_t);
void fsimg_fat_put32(unsigned char *, uint32_t);
fsimg_status_t fsimg_fat_corrupt(fsimg_error_t *, const char *);
bool fsimg_fat_cluster_valid(const fat_geometry_t *, uint32_t);
uint64_t fsimg_fat_cluster_offset(const fat_geometry_t *, uint32_t);
fsimg_status_t fsimg_fat_geometry_decode(const unsigned char *, uint64_t,
    fat_geometry_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_short_name(const unsigned char *, char *, fsimg_error_t *);
fsimg_status_t fsimg_fat_name_validate(const char *, fsimg_error_t *);
fsimg_status_t fsimg_fat_label(const unsigned char *, char *, fsimg_error_t *);
fsimg_status_t fsimg_fat_lfn_slot(fat_lfn_t *, const unsigned char *, fsimg_error_t *);
fsimg_status_t fsimg_fat_entry_name(fat_lfn_t *, const unsigned char *, char *,
    fsimg_error_t *);
fsimg_status_t fsimg_fat_name_key(const char *, unsigned char *, size_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_fat_entry_decode(const fat_geometry_t *, const unsigned char *,
    fsimg_entry_t *, uint32_t *, fsimg_error_t *);
fsimg_fat_reader_t *fsimg_fat_state(const fsimg_reader_t *);
const fsimg_format_variant_t *fsimg_fat_variant(unsigned);
fsimg_status_t fsimg_fat_open(fsimg_reader_t *, const fsimg_format_variant_t *,
    fsimg_error_t *);
void fsimg_fat_close(fsimg_reader_t *);
fsimg_status_t fsimg_fat_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_fat_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t fsimg_fat_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_read(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_range(fsimg_reader_t *, uint64_t, fsimg_range_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_fat_verify(fsimg_reader_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);

fsimg_status_t fsimg_fat_table_get(fsimg_reader_t *, uint32_t, uint32_t *,
    fsimg_error_t *);
void fsimg_fat_chain_init(fat_chain_t *, uint32_t, uint64_t);
fsimg_status_t fsimg_fat_chain_run(fsimg_reader_t *, fat_chain_t *,
    fsimg_extent_t *, fsimg_error_t *);
fsimg_status_t fsimg_fat_owner_append(fsimg_reader_t *, const fsimg_extent_t *,
    fsimg_error_t *);

#endif
