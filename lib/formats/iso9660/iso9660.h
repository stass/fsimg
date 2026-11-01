/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_ISO9660_H
#define FSIMG_ISO9660_H

#include "reader.h"

/* ISO 9660 (specs/design-iso9660.md and specs/design-iso9660-driver.md).
 * A volume is volume_space_size 2048-byte logical blocks; block n starts at
 * byte n * 2048 of the slice. Blocks 0 to 15 are the system area and the
 * volume descriptor set starts at block 16.
 *
 * The data flow: the probe (descriptor.c) claims CD001 at block 16; open
 * (read.c, volume.c) scans the descriptors, checks the geometry and picks
 * the view's root; the walker's prepare (directory.c) frames each
 * directory's records and admits every entry with a payload locating its
 * first record; replay (directory.c) reads that run again, checks it
 * against the payload and indexes a file's sections; resolve (read.c)
 * looks offsets up in that index. codec.c decodes every structure, names.c
 * derives displayed names and keys.
 */
#define ISO_BLOCK_BYTES		2048u
#define ISO_SYSTEM_BLOCKS	16u
#define ISO_PROBE_BYTES		((ISO_SYSTEM_BLOCKS + 1) * ISO_BLOCK_BYTES)
#define ISO_DESCRIPTORS_MAX	64u	/* before the terminator */
#define ISO_VOLUME_BLOCKS_MAX	UINT64_C(0xffffffff)
#define ISO_RECORD_MIN		34u	/* LEN_DR with a one-byte identifier */
#define ISO_IDENTIFIER_MAX	222u	/* 255 - 33, the longest LEN_FI */
#define ISO_SECTIONS_MAX	65535u
#define ISO_NAME_MAX		1024u
#define ISO_LABEL_BYTES		32u
#define ISO_ESCAPE_BYTES	32u

/* Volume descriptor types. */
#define ISO_VD_BOOT		0u
#define ISO_VD_PRIMARY		1u
#define ISO_VD_SUPPLEMENTARY	2u
#define ISO_VD_PARTITION	3u
#define ISO_VD_TERMINATOR	255u

/* Directory record flags. */
#define ISO_FLAG_HIDDEN		0x01u
#define ISO_FLAG_DIRECTORY	0x02u
#define ISO_FLAG_ASSOCIATED	0x04u
#define ISO_FLAG_RECORD		0x08u
#define ISO_FLAG_PROTECTION	0x10u
#define ISO_FLAG_RESERVED	0x60u
#define ISO_FLAG_MULTI_EXTENT	0x80u

/* Blocks covering n bytes, in 64 bits so no 32-bit length can wrap. */
#define ISO_BLOCKS(n)	(((uint64_t)(n) + ISO_BLOCK_BYTES - 1) / ISO_BLOCK_BYTES)

/* One decoded directory record, every field validated by
 * iso_record_decode(): the framing rule, both copies of each both-order
 * field, the flag table and the recording date. The identifier is
 * identifier_length bytes at byte 33 of the record, the system use area
 * system_use_length bytes at system_use_offset.
 */
typedef struct iso_record {
	fsimg_timestamp_t date;
	uint32_t	 extent;
	uint32_t	 data_length;
	uint8_t		 length;		/* LEN_DR */
	uint8_t		 ea_blocks;		/* extended attribute record */
	uint8_t		 flags;
	uint8_t		 identifier_length;	/* LEN_FI */
	uint8_t		 system_use_offset;
	uint8_t		 system_use_length;
	bool		 date_present;
} iso_record_t;

/* A decoded volume descriptor. block, type, version and joliet_level are
 * set for every descriptor; the other fields only for the primary
 * descriptor and a Joliet supplementary descriptor (joliet_level 1 to 3),
 * any other descriptor being framed and ignored. A primary descriptor's
 * geometry (volume_space_size, block_size, set_size, sequence) has passed
 * the reader profile when decoding succeeds; a Joliet descriptor's is only
 * decoded, and is trusted once volume.c finds it equal to the primary's.
 */
typedef struct iso_descriptor {
	iso_record_t	 root;
	uint32_t	 block;
	uint32_t	 volume_space_size;
	uint32_t	 path_table_bytes;
	uint32_t	 path_table_l;
	uint32_t	 path_table_l_copy;	/* zero for none */
	uint32_t	 path_table_m;
	uint32_t	 path_table_m_copy;	/* zero for none */
	uint16_t	 block_size;		/* logical block size */
	uint16_t	 set_size;		/* volume set size */
	uint16_t	 sequence;		/* volume sequence number */
	uint8_t		 type;
	uint8_t		 version;
	uint8_t		 joliet_level;		/* zero unless Joliet */
	unsigned char	 label[ISO_LABEL_BYTES];
} iso_descriptor_t;

/* A directory being read record by record, the one framing walk shared by
 * preparation and replay. The directory is length bytes at block extent,
 * spanning blocks blocks. The next record is read at byte offset of the
 * directory's block number block, counted from zero. Records come through
 * the reader's directory window, and the position of the record last
 * returned is kept for the payload.
 */
typedef struct iso_cursor {
	uint32_t	 extent;
	uint32_t	 length;
	uint32_t	 blocks;
	uint32_t	 block;
	uint32_t	 offset;
	uint32_t	 record_block;		/* the last record's volume block */
	uint32_t	 record_offset;		/* and its offset in that block */
} iso_cursor_t;

/* The prepared payload of one admitted entry (directory.c), which replay
 * decodes again from scratch storage: the containing directory, the
 * position of the entry's first record, its section count and kind, and
 * for a directory the directory it opens.
 */
typedef struct iso_payload {
	uint32_t	 directory_extent;
	uint32_t	 directory_length;
	uint32_t	 record_block;
	uint32_t	 opened_extent;		/* zero for a file */
	uint32_t	 opened_length;
	uint16_t	 record_offset;
	uint16_t	 sections;
	fsimg_entry_kind_t kind;
	bool		 relocated;		/* reached through CL */
} iso_payload_t;

/* Driver state behind the reader's private pointer, from open to close.
 * primary is the first primary descriptor and joliet the Joliet
 * descriptor when joliet_level is nonzero; view_root is the root record of
 * the selected view's hierarchy. readable_blocks is the smaller of the
 * volume and the whole blocks of the slice; nothing past it is read.
 * current is the payload replay made current, and file the current
 * regular file's section index. window caches one directory block,
 * window_block naming it while window_valid is set.
 */
typedef struct fsimg_iso9660_reader {
	iso_descriptor_t primary;
	iso_descriptor_t joliet;
	iso_record_t	 view_root;
	iso_payload_t	 current;
	fsimg_extent_index_t *file;
	fsimg_iso_view_t view;
	uint64_t	 readable_blocks;
	uint64_t	 window_block;
	uint32_t	 descriptors;		/* before the terminator */
	uint32_t	 terminator_block;
	unsigned	 joliet_level;
	bool		 enhanced;
	bool		 system_area_used;
	bool		 window_valid;
	unsigned char	 window[ISO_BLOCK_BYTES];
} fsimg_iso9660_reader_t;

extern const fsimg_reader_ops_t fsimg_iso9660_reader_ops;

/* read.c */
fsimg_iso9660_reader_t *iso_state(const fsimg_reader_t *);
fsimg_status_t iso_corrupt(fsimg_error_t *, const char *);
fsimg_status_t iso_unsupported(fsimg_error_t *, const char *);

/* codec.c: pure decoders over byte buffers, the only code that knows
 * field offsets.
 */
bool iso_bytes_zero(const unsigned char *, size_t);
fsimg_status_t iso_u16both(const unsigned char *, uint16_t *,
    fsimg_error_t *);
fsimg_status_t iso_u32both(const unsigned char *, uint32_t *,
    fsimg_error_t *);
fsimg_status_t iso_date7_decode(const unsigned char *, bool *,
    fsimg_timestamp_t *, fsimg_error_t *);
fsimg_status_t iso_date17_decode(const unsigned char *, bool *,
    fsimg_timestamp_t *, fsimg_error_t *);
bool iso_identity(const unsigned char *);
fsimg_status_t iso_geometry_decode(const unsigned char *, uint32_t *,
    fsimg_error_t *);
fsimg_status_t iso_descriptor_decode(const unsigned char *, uint32_t,
    iso_descriptor_t *, fsimg_error_t *);
fsimg_status_t iso_record_decode(const unsigned char *, size_t,
    iso_record_t *, fsimg_error_t *);

/* volume.c */
fsimg_status_t iso_volume_open(fsimg_reader_t *, fsimg_error_t *);
bool iso_in_volume(const fsimg_iso9660_reader_t *, uint64_t, uint64_t);
fsimg_status_t iso_extent_check(const fsimg_iso9660_reader_t *, uint64_t,
    uint64_t, fsimg_error_t *);
fsimg_status_t iso_block_read(fsimg_reader_t *, uint64_t, fsimg_error_t *);

/* names.c: identifier checks, displayed names and namespace keys. A
 * displayed-name buffer holds ISO_IDENTIFIER_MAX bytes and a NUL.
 */
fsimg_status_t iso_primary_name(const unsigned char *, size_t, bool, char *,
    size_t *, fsimg_error_t *);
bool iso_label_decode(const unsigned char *, char *);
fsimg_status_t iso_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t iso_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);

/* directory.c */
fsimg_status_t iso_payload_decode(const fsimg_iso9660_reader_t *,
    const unsigned char *, size_t, iso_payload_t *, fsimg_error_t *);
fsimg_status_t iso_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t iso_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);

#endif
