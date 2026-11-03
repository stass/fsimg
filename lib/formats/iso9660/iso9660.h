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
#define ISO_RECORD_IDENTIFIER	33u	/* a record's identifier's offset */
#define ISO_SECTIONS_MAX	65535u
#define ISO_NAME_MAX		1024u
#define ISO_LABEL_BYTES		32u
#define ISO_LABEL_TEXT_MAX	48u	/* 16 UTF-16 units as UTF-8 */
#define ISO_ESCAPE_BYTES	32u
#define ISO_SUSP_AREAS_MAX	16u	/* continuation areas per stream */
#define ISO_DATE7_BYTES		7u
#define ISO_DATE17_BYTES	17u
/* The UTC seconds of 1900-01-01 00:00:00 and 2155-12-31 23:59:59, the
 * span of the seven-byte recording date.
 */
#define ISO_DATE7_MIN		INT64_C(-2208988800)
#define ISO_DATE7_MAX		INT64_C(5869583999)
/* The UTC seconds of 0001-01-01 00:00:00 and 9999-12-31 23:59:59, the span
 * of the seventeen-byte descriptor date.
 */
#define ISO_DATE17_MIN		INT64_C(-62135596800)
#define ISO_DATE17_MAX		INT64_C(253402300799)

/* A rendered descriptor date, YYYY-MM-DD HH:MM:SS, and its terminator. */
#define ISO_DATE_TEXT_BYTES	20u

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

/* One path-table record (specs/design-iso9660.md#path-tables): an
 * ISO_PATH_HEADER_BYTES header, then length identifier bytes and a zero pad
 * byte when length is odd. parent is the parent's record number.
 */
#define ISO_PATH_HEADER_BYTES	8u

typedef struct iso_path_record {
	uint32_t	 extent;
	uint16_t	 parent;
	uint8_t		 ea_length;
	uint8_t		 length;		/* LEN_DI */
	unsigned char	 identifier[255];
} iso_path_record_t;

/* What the writer encodes into one directory record (codec.c):
 * identifier_length bytes of identifier and system_use_length bytes of
 * system use, the extent, data length, flags and recording date.
 */
typedef struct iso_record_fields {
	const unsigned char *identifier;
	const unsigned char *system_use;
	uint32_t	 extent;
	uint32_t	 data_length;
	size_t		 identifier_length;
	size_t		 system_use_length;
	uint8_t		 flags;
	unsigned char	 date[ISO_DATE7_BYTES];
} iso_record_fields_t;

/*
 * The descriptor text and date fields (specs/design-iso9660.md, descriptor
 * text and dates): eleven fields from offset 190 through 880, each a row of
 * one closed table of three classes. Every rule is a rule of a class; the
 * writer encodes and the reader decodes each row through its class's one
 * function. offset is from the first field, so the fields lie contiguously
 * in a ISO_DESCRIPTOR_BYTES image of bytes 190 to 880. key names the field
 * in the reader's report and setting is the writer's -o key, which names the
 * row of the settings table that holds its value. slot is the row's index
 * among the rows of its kind, the text rows or the date rows, which indexes
 * the reader's decoded dates. The writer's defaults are the row's: an unset
 * text is unset_text, NULL for none, and an unset date is the build date when unset_dated
 * and absent otherwise.
 */
#define ISO_DESCRIPTOR_FIELD_BASE	190u
#define ISO_DESCRIPTOR_BYTES		691u
#define ISO_DESCRIPTOR_TEXTS		7u	/* the rows before the dates */
#define ISO_DESCRIPTOR_DATES		4u
#define ISO_DESCRIPTOR_FIELDS \
	(ISO_DESCRIPTOR_TEXTS + ISO_DESCRIPTOR_DATES)

typedef enum iso_field_class {
	ISO_CLASS_A_CHARACTERS,
	ISO_CLASS_FILE_IDENTIFIER,
	ISO_CLASS_DATE
} iso_field_class_t;

typedef struct iso_descriptor_field {
	const char	*key;
	const char	*setting;
	unsigned	 slot;
	unsigned	 offset;
	unsigned	 width;
	iso_field_class_t class;
	const char	*unset_text;
	bool		 unset_dated;
} iso_descriptor_field_t;

extern const iso_descriptor_field_t iso_descriptor_fields[ISO_DESCRIPTOR_FIELDS];

/* What the writer encodes into its primary or Joliet descriptor: the
 * encoded label field, the geometry, the root directory and its recording
 * date, the path tables and the descriptor image of the text and date
 * fields, whose texts the Joliet descriptor mirrors.
 */
typedef struct iso_volume_fields {
	uint32_t	 volume_space_size;
	uint32_t	 path_table_bytes;
	uint32_t	 path_table_l;
	uint32_t	 path_table_m;
	uint32_t	 root_extent;
	uint32_t	 root_length;
	bool		 joliet;
	unsigned char	 label[ISO_LABEL_BYTES];
	unsigned char	 root_date[ISO_DATE7_BYTES];
	unsigned char	 descriptor[ISO_DESCRIPTOR_BYTES];
} iso_volume_fields_t;

/* A decoded volume descriptor. block, type, version and joliet_level are
 * set for every descriptor; the other fields only for the primary
 * descriptor and a Joliet supplementary descriptor (joliet_level 1 to 3),
 * any other descriptor being framed and ignored. fields holds the text
 * rows' raw bytes and, per date row, whether it is present and its UTC
 * seconds; open has checked every date. A primary descriptor's
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
	unsigned char	 fields[ISO_DESCRIPTOR_BYTES];
	bool		 date_present[ISO_DESCRIPTOR_DATES];
	int64_t		 date_seconds[ISO_DESCRIPTOR_DATES];
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
 * the profile refuses (always FSIMG_EUNSUPPORTED); decoding does not
 * report it, iso_rr_refusal() does once the caller's own checks have
 * passed. A SUSP profile limit is the exception: it ends decoding and is
 * reported at once. kind is the entry's kind, valid only when decoding succeeded
 * and nothing was refused; a refused record has kind 0, which is no kind.
 */
#define RR_PX	0x001u
#define RR_PN	0x002u
#define RR_SL	0x004u
#define RR_NM	0x008u
#define RR_CL	0x010u
#define RR_PL	0x020u
#define RR_RE	0x040u
#define RR_TF	0x080u

/* Which record a stream belongs to: an entry's, or a directory's first
 * ("." itself) or second ("..", its parent) dot record.
 */
typedef enum iso_rr_role {
	ISO_RR_ENTRY,
	ISO_RR_SELF,
	ISO_RR_PARENT
} iso_rr_role_t;

typedef struct iso_rr {
	fsimg_metadata_t metadata;
	uint32_t	 entries;
	uint32_t	 type;
	uint32_t	 child;
	uint32_t	 parent;
	size_t		 name_length;
	size_t		 target_length;
	const char	*refusal;
	fsimg_entry_kind_t kind;
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
 * descriptor when joliet_level is nonzero; iso_hierarchy() gives the
 * descriptor of the selected view's hierarchy. readable_blocks is the
 * smaller of the volume and the whole blocks of the slice; nothing past it
 * is read.
 * current is the payload replay made current, and file the current
 * regular file's section index. boot is the El Torito boot summary.
 * window caches the block of descriptors, system area, boot catalog or
 * directory records being read, continuation the last SUSP continuation
 * block, so a record's system use area and its continuation areas are
 * readable together. rockridge is the handshake's ISO_ROCKRIDGE_ value;
 * susp tells that the handshake found SP at the primary root, so the
 * volume's system use areas are SUSP streams, and susp_skip its skip
 * count.
 * verify is full verification's state (verify.c) while fsimg_verify runs,
 * otherwise NULL.
 */
typedef struct iso_verify iso_verify_t;

typedef struct fsimg_iso9660_reader {
	iso_descriptor_t primary;
	iso_descriptor_t joliet;
	iso_payload_t	 current;
	iso_boot_t	 boot;
	fsimg_extent_index_t *file;
	fsimg_iso_view_t view;
	uint64_t	 readable_blocks;
	uint32_t	 descriptors;		/* before the terminator */
	uint32_t	 terminator_block;
	unsigned	 joliet_level;
	unsigned	 rockridge;
	uint8_t		 susp_skip;
	bool		 susp;
	bool		 enhanced;
	bool		 system_area_used;
	iso_window_t	 window;
	iso_window_t	 continuation;
	iso_verify_t	*verify;
} fsimg_iso9660_reader_t;

extern const fsimg_reader_ops_t fsimg_iso9660_reader_ops;

/* read.c */
fsimg_iso9660_reader_t *iso_state(const fsimg_reader_t *);
fsimg_status_t iso_corrupt(fsimg_error_t *, const char *);
fsimg_status_t iso_unsupported(fsimg_error_t *, const char *);

/* codec.c: pure decoders and encoders over byte buffers, the only code
 * that knows field offsets besides eltorito.c.
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
void iso_path_header_decode(const unsigned char *, bool, iso_path_record_t *);
void iso_u16both_put(unsigned char *, uint16_t);
void iso_u32both_put(unsigned char *, uint32_t);
bool iso_date7_encode(int64_t, unsigned char *);
bool iso_date17_encode(int64_t, unsigned char *);
void iso_date17_absent(unsigned char *);
bool iso_date17_format(int64_t, char *);
bool iso_text_encode(iso_field_class_t, const char *, unsigned char *,
    size_t);
size_t iso_record_length(size_t, size_t);
size_t iso_record_encode(const iso_record_fields_t *, unsigned char *);
void iso_volume_descriptor_encode(const iso_volume_fields_t *,
    unsigned char *);
void iso_terminator_encode(unsigned char *);
size_t iso_path_record_encode(const iso_path_record_t *, bool,
    unsigned char *);

/* volume.c */
fsimg_status_t iso_volume_open(fsimg_reader_t *, fsimg_error_t *);
bool iso_in_volume(const fsimg_iso9660_reader_t *, uint64_t, uint64_t);
fsimg_status_t iso_extent_check(const fsimg_iso9660_reader_t *, uint64_t,
    uint64_t, fsimg_error_t *);
fsimg_status_t iso_window_read(fsimg_reader_t *, iso_window_t *, uint64_t,
    fsimg_error_t *);
fsimg_status_t iso_block_read(fsimg_reader_t *, uint64_t, fsimg_error_t *);
const iso_descriptor_t *iso_hierarchy(const fsimg_iso9660_reader_t *);

/* eltorito.c */
void iso_boot_record_encode(uint32_t, unsigned char *);
void iso_boot_catalog_encode(const iso_boot_entry_t *,
    const iso_boot_entry_t *, unsigned char *);
uint64_t iso_boot_image_blocks(uint16_t);
fsimg_status_t iso_boot_record_decode(const unsigned char *, iso_boot_t *,
    fsimg_error_t *);
fsimg_status_t iso_boot_open(fsimg_reader_t *, fsimg_error_t *);

/* susp.c */
void iso_susp_start(iso_susp_t *, const unsigned char *, uint32_t, uint32_t,
    const iso_record_t *);
fsimg_status_t iso_susp_next(fsimg_reader_t *, iso_susp_t *, iso_entry_t *,
    fsimg_error_t *);
fsimg_status_t iso_susp_frame(fsimg_reader_t *, const unsigned char *,
    uint32_t, uint32_t, const iso_record_t *, fsimg_error_t *);
fsimg_status_t iso_handshake(fsimg_reader_t *, fsimg_error_t *);

/* rockridge.c */
fsimg_status_t iso_rr_decode(fsimg_reader_t *, const unsigned char *,
    uint32_t, uint32_t, const iso_record_t *, iso_rr_role_t, iso_rr_t *,
    fsimg_error_t *);
fsimg_status_t iso_rr_refusal(const iso_rr_t *, fsimg_error_t *);

/* names.c: identifier checks, displayed names and namespace keys. A
 * displayed-name buffer holds ISO_NAME_MAX bytes and a NUL, a label buffer
 * ISO_LABEL_TEXT_MAX bytes and a NUL.
 */
fsimg_status_t iso_primary_name(const unsigned char *, size_t, bool, char *,
    size_t *, fsimg_error_t *);
fsimg_status_t iso_joliet_name(const unsigned char *, size_t, char *,
    size_t *, fsimg_error_t *);
bool iso_label_decode(const iso_descriptor_t *, char *);
void iso_texts_report(const iso_descriptor_t *, fsimg_volume_info_t *);
fsimg_status_t iso_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t iso_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);
int iso_primary_order(const unsigned char *, size_t, bool,
    const unsigned char *, size_t, bool);
bool iso_primary_versioned(const unsigned char *, size_t, bool);

/* directory.c */
fsimg_status_t iso_payload_decode(const fsimg_iso9660_reader_t *,
    const unsigned char *, size_t, iso_payload_t *, fsimg_error_t *);
fsimg_status_t iso_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t iso_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);

/* verify.c: full verification, and the two hooks through which
 * iso_prepare reports, while verify is set, each directory it prepares and
 * each later record of it; outside verification they do nothing.
 */
fsimg_status_t iso_verify(fsimg_reader_t *, unsigned, fsimg_verify_report_t *,
    fsimg_error_t *);
fsimg_status_t iso_verify_directory(fsimg_reader_t *, const iso_cursor_t *,
    bool, fsimg_error_t *);
fsimg_status_t iso_verify_record(fsimg_reader_t *, const iso_cursor_t *,
    const iso_record_t *, const unsigned char *, fsimg_error_t *);

#endif
