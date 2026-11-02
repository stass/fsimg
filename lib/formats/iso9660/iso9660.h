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
 * (read.c, volume.c) scans the descriptors, checks the geometry, decodes
 * the El Torito boot catalog (eltorito.c) and picks the view's root; the
 * walker's prepare (directory.c) frames each
 * directory's records and admits every entry with a payload locating its
 * first record; replay (directory.c) reads that run again, checks it
 * against the payload and indexes a file's sections; resolve (read.c)
 * looks offsets up in that index. codec.c decodes every structure, names.c
 * derives displayed names and keys. In the rockridge view susp.c iterates
 * each record's System Use Sharing Protocol entries and rockridge.c
 * decodes them into the entry's name, kind, metadata, link target and
 * relocation pointers.
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
#define ISO_SUSP_AREAS_MAX	16u	/* continuation areas per stream */

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

/* The rockridge detail: what the handshake at the primary root found. */
#define ISO_ROCKRIDGE_ABSENT	0u
#define ISO_ROCKRIDGE_PRESENT	1u
#define ISO_ROCKRIDGE_SKIP	2u	/* present, unsupported skip count */
#define ISO_ROCKRIDGE_UNKNOWN	3u	/* the handshake did not run */

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

/* El Torito (eltorito.c). One catalog block holds 64 entries; less the
 * validation entry and one section header, at most 62 are boot entries.
 */
#define ISO_BOOT_ENTRIES_MAX	62u
#define ISO_BOOT_X86		0x00u	/* platform IDs info counts */
#define ISO_BOOT_EFI		0xefu

/* One validated El Torito boot entry, initial or section: its platform
 * from the validation entry or its section header, and the image's
 * extent, image_block for at least iso_boot_image_blocks(sector_count)
 * blocks. The image starts after the descriptor set; it is never read and
 * may run past the volume, as an EFI image in an appended partition does,
 * so a claim on it covers only its part within the volume.
 */
typedef struct iso_boot_entry {
	uint32_t	 image_block;
	uint16_t	 load_segment;
	uint16_t	 sector_count;		/* 512-byte virtual sectors */
	uint8_t		 platform;
	uint8_t		 media_type;		/* 0 to 4 */
	uint8_t		 system_type;
	bool		 bootable;
} iso_boot_entry_t;

/* The boot summary: present when the descriptor set holds an El Torito
 * boot record, whose boot catalog is the one block catalog_block within
 * the volume and the slice, holding entry_count entries.
 */
typedef struct iso_boot {
	iso_boot_entry_t entries[ISO_BOOT_ENTRIES_MAX];
	uint32_t	 catalog_block;
	unsigned	 entry_count;
	bool		 present;
} iso_boot_t;

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

/* One cached volume block, block naming it while valid is set. */
typedef struct iso_window {
	uint64_t	 block;
	bool		 valid;
	unsigned char	 bytes[ISO_BLOCK_BYTES];
} iso_window_t;

/* An area of a SUSP entry stream: length bytes at offset of volume
 * block number block.
 */
typedef struct iso_area {
	uint32_t	 block;
	uint16_t	 offset;
	uint16_t	 length;
} iso_area_t;

/* One SUSP entry: its signature and version, and the length content bytes
 * after its four-byte header, which stay valid until the next entry.
 */
typedef struct iso_entry {
	const unsigned char *data;
	size_t		 length;
	char		 signature[2];
	uint8_t		 version;
} iso_entry_t;

/* The SUSP cursor over one record's entry stream (susp.c): the current
 * area's bytes and the position of its next entry, every area entered so
 * far, the record's own first, and the continuation area a CE entry named
 * for when the current area ends.
 */
typedef struct iso_susp {
	const unsigned char *bytes;
	size_t		 length;
	size_t		 position;
	iso_area_t	 areas[1 + ISO_SUSP_AREAS_MAX];
	unsigned	 count;			/* areas entered */
	iso_area_t	 next;
	bool		 has_next;		/* the current area held a CE */
	bool		 ended;			/* ST, or the last area ended */
} iso_susp_t;

/* The Rock Ridge entries of one record's stream (rockridge.c). entries
 * holds an RR_ bit for each kind of entry seen. type is the PX file type
 * bits and metadata the PX owner and mode and the TF times, each with its
 * valid bit. name holds name_length bytes and a NUL, and target holds
 * target_length bytes, both assembled only for a record that is not a dot
 * record. child and parent are the CL and PL blocks. refusal names what
 * the profile refuses, reported after every corruption check.
 */
#define RR_PX	0x001u
#define RR_PN	0x002u
#define RR_SL	0x004u
#define RR_NM	0x008u
#define RR_CL	0x010u
#define RR_PL	0x020u
#define RR_RE	0x040u
#define RR_TF	0x080u

typedef struct iso_rr {
	fsimg_metadata_t metadata;
	uint32_t	 entries;
	uint32_t	 type;
	uint32_t	 child;
	uint32_t	 parent;
	size_t		 name_length;
	size_t		 target_length;
	const char	*refusal;
	char		 name[ISO_NAME_MAX + 1];
	unsigned char	 target[ISO_NAME_MAX];
} iso_rr_t;

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
 * regular file's section index. boot is the El Torito boot summary.
 * window caches the block of descriptors, system area, boot catalog or
 * directory records being read, continuation the last SUSP continuation
 * block, so a record's system use area and its continuation areas are
 * readable together. rockridge is the handshake's ISO_ROCKRIDGE_ value.
 */
typedef struct fsimg_iso9660_reader {
	iso_descriptor_t primary;
	iso_descriptor_t joliet;
	iso_record_t	 view_root;
	iso_payload_t	 current;
	iso_boot_t	 boot;
	fsimg_extent_index_t *file;
	fsimg_iso_view_t view;
	uint64_t	 readable_blocks;
	uint32_t	 descriptors;		/* before the terminator */
	uint32_t	 terminator_block;
	unsigned	 joliet_level;
	unsigned	 rockridge;
	bool		 enhanced;
	bool		 system_area_used;
	iso_window_t	 window;
	iso_window_t	 continuation;
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
fsimg_status_t iso_window_read(fsimg_reader_t *, iso_window_t *, uint64_t,
    fsimg_error_t *);
fsimg_status_t iso_block_read(fsimg_reader_t *, uint64_t, fsimg_error_t *);

/* eltorito.c */
uint64_t iso_boot_image_blocks(uint16_t);
fsimg_status_t iso_boot_record_decode(const unsigned char *, iso_boot_t *,
    fsimg_error_t *);
fsimg_status_t iso_boot_open(fsimg_reader_t *, fsimg_error_t *);

/* susp.c */
void iso_susp_start(iso_susp_t *, const unsigned char *, uint32_t, uint32_t,
    const iso_record_t *);
fsimg_status_t iso_susp_next(fsimg_reader_t *, iso_susp_t *, iso_entry_t *,
    fsimg_error_t *);
fsimg_status_t iso_susp_frame(fsimg_reader_t *, iso_susp_t *,
    fsimg_error_t *);
fsimg_status_t iso_handshake(fsimg_reader_t *, fsimg_error_t *);

/* rockridge.c */
fsimg_status_t iso_rr_decode(fsimg_reader_t *, iso_susp_t *, bool,
    iso_rr_t *, fsimg_error_t *);
fsimg_status_t iso_rr_kind(const iso_rr_t *, const iso_record_t *,
    fsimg_entry_kind_t *, fsimg_error_t *);

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
