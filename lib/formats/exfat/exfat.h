/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_EXFAT_H
#define FSIMG_EXFAT_H

#include "reader.h"
#include "writer.h"

#define EXFAT_BUFFER_BYTES 65536u
#define EXFAT_NAME_BYTES 1021u
#define EXFAT_RECORD_BYTES (64u + EXFAT_NAME_BYTES)
#define EXFAT_SET_BYTES 8192u
#define EXFAT_DIRECTORY_BYTES (UINT64_C(256) * 1024 * 1024)
#define EXFAT_CLUSTER_MAX UINT32_C(0xfffffff5)
/* Smallest supported volume; it holds both boot regions at any sector size. */
#define EXFAT_VOLUME_MIN UINT64_C(1048576)
#define EXFAT_CLUSTER_RECOMMENDED UINT32_C(0x00fffffe)
#define EXFAT_UPCASE_BYTES 131072u

typedef struct exfat_geometry {
	uint64_t	 volume_bytes;
	uint64_t	 fat_offset;
	uint64_t	 fat_bytes;
	uint64_t	 heap_offset;
	uint32_t	 sector_bytes;
	uint32_t	 cluster_bytes;
	uint32_t	 clusters;
	uint32_t	 root_cluster;
	uint16_t	 flags;
} exfat_geometry_t;

/* The FAT describes allocation for chained streams. DataLength only bounds
 * readable payload; it does not discard additional owned chain clusters.
 * For contiguous streams allocation instead follows rounded-up DataLength.
 */
typedef struct exfat_stream {
	uint64_t	 length;
	uint64_t	 valid_length;
	uint32_t	 first;
	bool		 contiguous;
} exfat_stream_t;

/* Receives one validated cluster run (first cluster, cluster count). */
typedef fsimg_status_t (*exfat_run_fn_t)(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);

typedef enum exfat_length_policy {
	EXFAT_LENGTH_COVER_DATA,
	EXFAT_LENGTH_EXACT_ALLOCATION
} exfat_length_policy_t;

/* Bootstrap metadata discovery and named traversal share one framing owner.
 * It validates entry boundaries, file/benign-set checksums, and all terminators
 * before yielding a primary entry. Concrete root metadata has no set checksum.
 * Names need the later validated upcase table.
 */
typedef struct exfat_directory_cursor {
	fsimg_reader_t	*reader;
	fsimg_extent_index_t *index;
	uint64_t	 offset;
	size_t		 window_position;
	size_t		 window_length;
	size_t		 set_length;
	bool		 root;
	bool		 ended;
	unsigned char	 window[4096];
	unsigned char	 set[EXFAT_SET_BYTES];
} exfat_directory_cursor_t;

typedef struct exfat_directory {
	fsimg_stream_t	*entries;
	uint64_t	 cursor;
	size_t		 path_length;
} exfat_directory_t;

/* Driver state behind the reader's private pointer, from open to close.
 * Each allocation is a shared extent index over the cluster heap, never a
 * resident chain. The file index belongs to the current entry.
 */
typedef struct fsimg_exfat_reader fsimg_exfat_reader_t;

struct fsimg_exfat_reader {
	exfat_geometry_t geometry;
	fsimg_extent_index_t *root;
	fsimg_extent_index_t *bitmap;
	fsimg_extent_index_t *upcase_index;
	fsimg_extent_index_t *file;
	exfat_stream_t	 current;
	exfat_directory_t *directories;
	fsimg_bitset_t	*directory_clusters;
	fsimg_stream_t	*owners;
	uint16_t	*upcase;
	uint64_t	 bitmap_length;
	uint64_t	 upcase_length;
	uint64_t	 table_window_offset;
	size_t		 table_window_length;
	uint32_t	 upcase_checksum;
	uint32_t	 depth;
	bool		 started;
	bool		 pending_directory;
	bool		 bitmap_ready;
	unsigned char	 table_window[4096];
	unsigned char	 record[EXFAT_RECORD_BYTES];
};

extern const fsimg_reader_ops_t fsimg_exfat_reader_ops;
extern const fsimg_writer_ops_t fsimg_exfat_writer_ops;

fsimg_exfat_reader_t *exfat_state(const fsimg_reader_t *);

uint16_t exfat_u16(const unsigned char *);
uint32_t exfat_u32(const unsigned char *);
void exfat_put16(unsigned char *, uint16_t);
void exfat_put32(unsigned char *, uint32_t);
uint32_t exfat_checksum32(uint32_t, unsigned char);
uint16_t exfat_checksum16(uint16_t, unsigned char);
uint16_t exfat_set_checksum(const unsigned char *, size_t);
uint16_t exfat_name_hash(const uint16_t *, size_t, const uint16_t *);
fsimg_status_t exfat_corrupt(fsimg_error_t *, const char *);
bool exfat_cluster_valid(const exfat_geometry_t *, uint32_t);
uint64_t exfat_cluster_offset(const exfat_geometry_t *, uint32_t);
bool exfat_character_valid(uint16_t);
fsimg_status_t exfat_name_units(const char *, uint16_t *, size_t *,
    fsimg_error_t *);
fsimg_status_t exfat_key_append(fsimg_stream_t *, uint64_t, const uint16_t *,
    size_t, const uint16_t *, fsimg_error_t *);
fsimg_status_t exfat_keys_unique(fsimg_stream_t *, bool, fsimg_error_t *);
fsimg_status_t exfat_entry_decode(const unsigned char *, fsimg_entry_t *,
    exfat_stream_t *, fsimg_error_t *);
fsimg_status_t exfat_stream_validate(const exfat_geometry_t *,
    const exfat_stream_t *, bool, fsimg_error_t *);
fsimg_status_t exfat_metadata_encode(fsimg_plan_t *, const fsimg_entry_t *,
    unsigned char *, fsimg_error_t *);
fsimg_status_t exfat_boot_read(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t exfat_table_get(fsimg_reader_t *, uint32_t, uint32_t *,
    fsimg_error_t *);
fsimg_status_t exfat_index_build(fsimg_reader_t *, const exfat_stream_t *,
    exfat_length_policy_t, fsimg_extent_index_t **, fsimg_error_t *);
fsimg_status_t exfat_index_read(fsimg_reader_t *, fsimg_extent_index_t *,
    uint64_t, void *, size_t, fsimg_error_t *);
fsimg_status_t exfat_index_runs(fsimg_reader_t *, fsimg_extent_index_t *,
    exfat_run_fn_t, fsimg_error_t *);
fsimg_status_t exfat_index_check(fsimg_reader_t *, fsimg_extent_index_t *,
    fsimg_error_t *);
fsimg_status_t exfat_index_claim(fsimg_reader_t *, fsimg_extent_index_t *,
    fsimg_error_t *);
fsimg_status_t exfat_bitmap_bit(fsimg_reader_t *, uint32_t, bool *,
    fsimg_error_t *);
fsimg_status_t exfat_next(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t exfat_directory_set_next(exfat_directory_cursor_t *,
    fsimg_error_t *);
fsimg_status_t exfat_verify(fsimg_reader_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);

#endif
