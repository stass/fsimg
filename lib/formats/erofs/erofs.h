/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_EROFS_H
#define FSIMG_EROFS_H

#include "reader.h"

/*
 * EROFS, the Linux enhanced read-only filesystem. Every integer is
 * little-endian. Offsets and sizes are in bytes unless a name says blocks.
 *
 * The superblock region is the bytes [1024, 4096) of the volume: the base
 * superblock, its 16-byte extension slots and the compression configuration
 * records all lie inside it, whatever the block size. Offsets into the
 * region are relative to the superblock, volume byte 1024.
 */
#define EROFS_MAGIC			UINT32_C(0xe0f5e1e2)
#define EROFS_SUPER_OFFSET		1024u
#define EROFS_SUPER_REGION_BYTES	3072u
#define EROFS_SUPER_BASE_BYTES		128u
#define EROFS_SUPER_SLOT_BYTES		16u
#define EROFS_BLKSZBITS_MIN		9u
#define EROFS_BLKSZBITS_MAX		12u

/* Compatible features: an unknown bit does not prevent reading. */
#define EROFS_COMPAT_SB_CHKSUM		UINT32_C(0x01)
#define EROFS_COMPAT_MTIME		UINT32_C(0x02)
#define EROFS_COMPAT_XATTR_FILTER	UINT32_C(0x04)
#define EROFS_COMPAT_SHARED_EA_IN_METABOX UINT32_C(0x08)
#define EROFS_COMPAT_PLAIN_XATTR_PFX	UINT32_C(0x10)
#define EROFS_COMPAT_ISHARE_XATTRS	UINT32_C(0x20)

/*
 * Incompatible features. Some bits carry two names, one per structure that
 * needs them: 0x002 is also BIG_PCLUSTER, 0x008 also COMPR_HEAD2 and 0x020
 * also DEDUPE. Any bit outside EROFS_INCOMPAT_KNOWN is unsupported.
 */
#define EROFS_INCOMPAT_LZ4_0PADDING	UINT32_C(0x001)
#define EROFS_INCOMPAT_COMPR_CFGS	UINT32_C(0x002)
#define EROFS_INCOMPAT_CHUNKED_FILE	UINT32_C(0x004)
#define EROFS_INCOMPAT_DEVICE_TABLE	UINT32_C(0x008)
#define EROFS_INCOMPAT_ZTAILPACKING	UINT32_C(0x010)
#define EROFS_INCOMPAT_FRAGMENTS	UINT32_C(0x020)
#define EROFS_INCOMPAT_XATTR_PREFIXES	UINT32_C(0x040)
#define EROFS_INCOMPAT_48BIT		UINT32_C(0x080)
#define EROFS_INCOMPAT_METABOX		UINT32_C(0x100)
#define EROFS_INCOMPAT_KNOWN		UINT32_C(0x1ff)

/* A NID with this bit set addresses the metadata box's content. */
#define EROFS_NID_BOX			(UINT64_C(1) << 63)
/* A NID counts 32-byte slots of its metadata space. */
#define EROFS_NID_SLOT_BYTES		32u

#define EROFS_INODE_COMPACT_BYTES	32u
#define EROFS_INODE_EXTENDED_BYTES	64u
/* The longest symbolic-link target the reader delivers. */
#define EROFS_SYMLINK_MAX		1024u

#define EROFS_DIRENT_BYTES		12u
#define EROFS_NAME_MAX			255u

#define EROFS_XATTR_HEADER_BYTES	12u
#define EROFS_XATTR_ENTRY_BYTES		4u
/* A name index with this bit set selects a long prefix by its low bits. */
#define EROFS_XATTR_LONG_PREFIX		0x80u

/* Algorithm identifiers, which are also bit numbers of the algorithm mask. */
#define EROFS_ALGORITHM_LZ4		0u
#define EROFS_ALGORITHM_LZMA		1u
#define EROFS_ALGORITHM_DEFLATE		2u
#define EROFS_ALGORITHM_ZSTD		3u
#define EROFS_ALGORITHM_COUNT		4u

/* The data layout, format-word bits 1 to 3; 5 to 7 are reserved. */
typedef enum erofs_layout {
	EROFS_LAYOUT_FLAT = 0,
	EROFS_LAYOUT_COMPRESSED_FULL = 1,
	EROFS_LAYOUT_FLAT_INLINE = 2,
	EROFS_LAYOUT_COMPRESSED_COMPACT = 3,
	EROFS_LAYOUT_CHUNK_BASED = 4
} erofs_layout_t;

/* Directory entry file types; any other value is corrupt. */
#define EROFS_FT_UNKNOWN		0u
#define EROFS_FT_REGULAR		1u
#define EROFS_FT_DIRECTORY		2u
#define EROFS_FT_CHAR_DEVICE		3u
#define EROFS_FT_BLOCK_DEVICE		4u
#define EROFS_FT_FIFO			5u
#define EROFS_FT_SOCKET			6u
#define EROFS_FT_SYMLINK		7u

/*
 * The decoded superblock, published only by erofs_region_decode(), so every
 * field holds a value open validated or one the format leaves to the
 * caller's interpretation, such as the advisory inode count. root_nid and
 * block_count are already widened under the 48-bit layout. box_nid is zero
 * without the metadata box.
 */
typedef struct erofs_super {
	uint8_t		 uuid[16];
	uint8_t		 name[16];		/* raw; name_length bytes used */
	uint64_t	 root_nid;
	uint64_t	 inode_count;		/* advisory */
	uint64_t	 block_count;		/* zero: the volume's length */
	uint64_t	 packed_nid;
	uint64_t	 box_nid;
	int64_t		 epoch;			/* seconds since 1970 */
	uint32_t	 compat;
	uint32_t	 incompat;
	uint32_t	 checksum;		/* as stored */
	uint32_t	 epoch_nsec;
	uint32_t	 meta_blkaddr;
	uint32_t	 xattr_blkaddr;
	uint32_t	 xattr_prefix_start;	/* in 4-byte units */
	uint32_t	 build_time;		/* seconds after the epoch */
	/* Under COMPR_CFGS the algorithm mask, else LZ4's maximum distance. */
	uint16_t	 compression_word;
	uint8_t		 blkszbits;
	uint8_t		 extension_slots;
	uint8_t		 name_length;
	uint8_t		 xattr_prefix_count;
	uint8_t		 ishare_prefix;		/* reported, not interpreted */
	bool		 filter_disabled;
	/* A reserved byte is nonzero: reported, and verification incomplete. */
	bool		 reserved_nonzero;
} erofs_super_t;

/*
 * The compression configuration. algorithms is the mask of algorithms the
 * volume may use, by EROFS_ALGORITHM_* bit. Without COMPR_CFGS it is LZ4
 * alone with the superblock's maximum distance and the other fields zero;
 * with it, each field comes from its algorithm's record, zero when the
 * algorithm is absent. zstd_window_log is the window's base-2 logarithm,
 * the record's field plus 10.
 */
typedef struct erofs_config {
	uint32_t	 lzma_dict_size;
	uint16_t	 lz4_max_distance;
	uint16_t	 lz4_max_pcluster_blocks;
	uint8_t		 algorithms;
	uint8_t		 deflate_window_bits;
	uint8_t		 zstd_window_log;
	/* A record's reserved byte is nonzero, as for the superblock. */
	bool		 reserved_nonzero;
} erofs_config_t;

/*
 * The decoded fixed part of an inode. layout_offset is where its layout
 * bytes (the inline tail or the chunk index) start and body_bytes where
 * they end, both from the inode's first byte, so [0, body_bytes) is all the
 * inode itself occupies.
 * layout_word is the word at 0x10: a start block, a chunk format or a
 * device number by the inode's layout and kind. layout_high holds start-block
 * bits 32 to 47 under the 48-bit layout and is zero otherwise.
 */
typedef struct erofs_inode {
	uint64_t	 size;
	uint64_t	 layout_offset;
	uint64_t	 body_bytes;
	int64_t		 time;			/* seconds since 1970 */
	fsimg_entry_kind_t kind;
	erofs_layout_t	 layout;
	uint32_t	 time_nsec;
	uint32_t	 fixed_bytes;		/* 32 or 64 */
	uint32_t	 xattr_bytes;		/* the attribute region */
	uint32_t	 layout_word;
	uint32_t	 nlink;
	uint32_t	 serial;		/* reported; never an identity */
	uint32_t	 uid;
	uint32_t	 gid;
	uint16_t	 layout_high;
	uint16_t	 permissions;		/* the mode's low 12 bits */
	bool		 extended;
	/* A 48-bit directory whose "." record is absent. */
	bool		 omits_dot;
} erofs_inode_t;

/*
 * A chunk-based inode's chunk format, the low 16 bits of its layout word.
 * Index entries are entry_bytes (4 or 8) wide; wide allows 48-bit block
 * numbers in 8-byte entries.
 */
typedef struct erofs_chunk_format {
	uint64_t	 chunk_bytes;
	uint8_t		 chunkbits;
	uint8_t		 entry_bytes;
	bool		 wide;
} erofs_chunk_format_t;

/* One chunk index entry: a hole, or a chunk stored from block. */
typedef struct erofs_chunk {
	uint64_t	 block;			/* zero for a hole */
	bool		 hole;
} erofs_chunk_t;

/*
 * A directory block erofs_dirblock_decode() accepted, every entry valid:
 * bytes is borrowed from the caller and length is the block's valid
 * length, B bytes except in a directory's last block.
 */
typedef struct erofs_dirblock {
	const unsigned char *bytes;
	size_t		 length;
	size_t		 count;			/* entries in the array */
} erofs_dirblock_t;

/* One directory entry; its name is name_length bytes at name_offset. */
typedef struct erofs_dirent {
	uint64_t	 nid;
	uint16_t	 name_offset;
	uint16_t	 name_length;
	uint8_t		 file_type;
} erofs_dirent_t;

/*
 * An inline attribute region's header. Shared indexes start at offset 12;
 * the inline entries start at entries_offset and run to the region's end.
 */
typedef struct erofs_xattr_header {
	uint32_t	 filter;
	uint32_t	 entries_offset;
	uint8_t		 shared_count;
} erofs_xattr_header_t;

/*
 * One attribute entry, inline or shared. The name suffix starts at offset
 * 4 and the value follows it; entry_bytes includes the padding to 4.
 */
typedef struct erofs_xattr {
	uint32_t	 entry_bytes;
	uint16_t	 value_length;
	uint8_t		 name_index;
	uint8_t		 suffix_length;
} erofs_xattr_t;

/*
 * One long-prefix record. The infix starts at offset 3; record_bytes
 * includes the padding to 4.
 */
typedef struct erofs_prefix {
	uint32_t	 record_bytes;
	uint16_t	 infix_length;
	uint8_t		 base_index;
} erofs_prefix_t;

/*
 * Decoders (codec.c). Each reads only the bytes and length it is given,
 * writes its output only on success, and returns FSIMG_OK, or
 * FSIMG_ECORRUPT or FSIMG_EUNSUPPORTED with *what set to a static phrase
 * naming the field; erofs_dirent_decode() also returns FSIMG_EINVAL for an
 * index past the block's entries, a caller's error. The exceptions to the
 * length rule are erofs_inode_fixed_bytes(), which reads the two bytes of
 * the format word, and the accessors of a structure already decoded.
 * Rules that relate one structure to another, such as an address inside
 * the volume, belong to their callers.
 */
bool		erofs_magic_matches(const unsigned char *, size_t);
fsimg_status_t	erofs_super_probe(const unsigned char *, size_t,
		    const char **);
fsimg_status_t	erofs_region_decode(const unsigned char *, size_t,
		    erofs_super_t *, erofs_config_t *, const char **);
bool		erofs_super_checksum(const unsigned char *, size_t, unsigned,
		    uint32_t *);
size_t		erofs_inode_fixed_bytes(const unsigned char *);
fsimg_status_t	erofs_inode_decode(const unsigned char *, size_t,
		    const erofs_super_t *, erofs_inode_t *, const char **);
void		erofs_device_decode(uint32_t, uint32_t *, uint32_t *);
fsimg_status_t	erofs_chunk_format_decode(uint32_t, const erofs_super_t *,
		    erofs_chunk_format_t *, const char **);
fsimg_status_t	erofs_chunk_decode(const unsigned char *, size_t,
		    const erofs_chunk_format_t *, erofs_chunk_t *, const char **);
fsimg_status_t	erofs_dirblock_decode(const unsigned char *, size_t,
		    erofs_dirblock_t *, const char **);
fsimg_status_t	erofs_dirent_decode(const erofs_dirblock_t *, size_t,
		    erofs_dirent_t *, const char **);
fsimg_status_t	erofs_xattr_header_decode(const unsigned char *, size_t,
		    erofs_xattr_header_t *, const char **);
bool		erofs_xattr_shared_id(const unsigned char *,
		    const erofs_xattr_header_t *, unsigned, uint32_t *);
uint64_t	erofs_xattr_shared_offset(const erofs_super_t *, uint32_t);
fsimg_status_t	erofs_xattr_decode(const unsigned char *, size_t,
		    const erofs_super_t *, erofs_xattr_t *, const char **);
fsimg_status_t	erofs_prefix_decode(const unsigned char *, size_t,
		    erofs_prefix_t *, const char **);
uint32_t	erofs_xxh32(const unsigned char *, size_t, const unsigned char *,
		    size_t, uint32_t);
unsigned	erofs_xattr_filter_bit(uint8_t, const unsigned char *, size_t,
		    const unsigned char *, size_t);

#endif /* !FSIMG_EROFS_H */
