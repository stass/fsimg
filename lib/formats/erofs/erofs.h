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
/* The probe reads through the base superblock and no further. */
#define EROFS_PROBE_BYTES		(EROFS_SUPER_OFFSET + EROFS_SUPER_BASE_BYTES)
#define EROFS_NAME_FIELD_BYTES		16u

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

/* The mode's type bits, by their traditional POSIX values. */
#define EROFS_MODE_TYPE_MASK		0170000u
#define EROFS_MODE_FIFO			0010000u
#define EROFS_MODE_CHAR_DEVICE		0020000u
#define EROFS_MODE_DIRECTORY		0040000u
#define EROFS_MODE_BLOCK_DEVICE		0060000u
#define EROFS_MODE_REGULAR		0100000u
#define EROFS_MODE_SYMLINK		0120000u
#define EROFS_MODE_SOCKET		0140000u
#define EROFS_MODE_PERMISSIONS		07777u

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
 * The two metadata spaces a NID can address: the volume itself, inside the
 * address domain, and the metadata box inode's content. Offsets in the
 * primary space are volume byte offsets.
 */
typedef enum erofs_space {
	EROFS_SPACE_PRIMARY,
	EROFS_SPACE_BOX
} erofs_space_t;

/*
 * The decoded fixed part of an inode. layout_offset is where its layout
 * bytes (the inline tail or the chunk index) start and body_bytes where
 * they end, both from the inode's first byte, so [0, body_bytes) is all the
 * inode itself occupies.
 * layout_word is the word at 0x10: a start block, a chunk format or a
 * device number by the inode's layout and kind. layout_high holds start-block
 * bits 32 to 47 under the 48-bit layout and is zero otherwise.
 * nid, space and offset say where the inode is: erofs_inode_load() sets
 * them, and erofs_inode_decode() alone leaves them zero.
 */
typedef struct erofs_inode {
	uint64_t	 nid;
	uint64_t	 offset;		/* of the fixed part, in space */
	erofs_space_t	 space;
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
 * Where a range of an inode's content is stored, as erofs_map() returns it:
 * in the volume's blocks, or inline in the inode's own body. The kinds the
 * later layouts add, holes and decoded clusters, join this closed set.
 */
typedef enum erofs_extent_kind {
	EROFS_EXTENT_STORED,
	EROFS_EXTENT_INLINE
} erofs_extent_kind_t;

/*
 * One normalized extent: the logical bytes [logical_offset,
 * logical_offset + length) of an inode, stored from storage_offset, a volume
 * byte offset for a stored extent and an offset in the inode's body for an
 * inline one. A stored extent's whole blocks lie inside the content domain;
 * an inline extent lies inside the body, which erofs_inode_load() checked.
 */
typedef struct erofs_extent {
	uint64_t	 logical_offset;
	uint64_t	 storage_offset;
	uint64_t	 length;
	erofs_extent_kind_t kind;
} erofs_extent_t;

/*
 * The feature decision open makes once from the superblock's feature words.
 * The rest of the driver reads these, never the words. A feature this
 * reader refuses at open has no member, since no opened volume sets it.
 * unknown_compat records a compatible bit outside the format's table, which
 * does not prevent reading but leaves verification incomplete.
 */
typedef struct erofs_features {
	bool		 checksum;		/* SB_CHKSUM: open checked it */
	bool		 mtime;			/* MTIME: times are modifications */
	bool		 xattr_filter;		/* XATTR_FILTER */
	bool		 long_prefixes;		/* XATTR_PREFIXES */
	bool		 plain_prefixes;	/* PLAIN_XATTR_PFX */
	bool		 unknown_compat;
} erofs_features_t;

/*
 * The driver's state, from open to close. domain_bytes ends the address
 * domain [0, domain_bytes): the block count times the block size, or the
 * input's length when the count is zero. Every structure the driver reads
 * lies inside it; a part of it past the input's end is a truncation, which
 * bounded I/O reports when something there is read.
 *
 * current is the entry the walker last replayed and current_parent the NID
 * of the directory listing it: prepare reads a directory below the root
 * from them, and resolve the current file. tail_window holds an inline
 * tail for delivery, one block, charged at open and owned until close.
 */
typedef struct fsimg_erofs_reader {
	erofs_super_t	 super;
	erofs_config_t	 config;
	erofs_features_t features;
	erofs_inode_t	 current;
	uint64_t	 current_parent;
	uint64_t	 domain_bytes;
	unsigned char	*tail_window;
	uint32_t	 block_bytes;
} fsimg_erofs_reader_t;

extern const fsimg_reader_ops_t fsimg_erofs_reader_ops;

/* Driver state and error reporting (read.c). */
fsimg_erofs_reader_t *erofs_state(const fsimg_reader_t *);
fsimg_status_t	erofs_reject(fsimg_error_t *, fsimg_status_t, const char *);

/*
 * The mapper (map.c), the one place a logical offset of an inode becomes
 * storage, and the content reader (content.c) built on it, which reads a
 * logical range of any inode into a caller buffer.
 */
fsimg_status_t	erofs_map(const fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, erofs_extent_t *, fsimg_error_t *);
fsimg_status_t	erofs_content_read(fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, size_t, void *, fsimg_error_t *);
fsimg_status_t	erofs_resolve(fsimg_reader_t *, uint64_t, fsimg_extent_t *,
		    fsimg_error_t *);

/* Directory callbacks (directory.c). */
fsimg_status_t	erofs_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_replay(fsimg_reader_t *, const char *,
		    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t	erofs_keys(fsimg_reader_t *, const char *,
		    const unsigned char *, size_t, fsimg_directory_keys_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_key(fsimg_reader_t *, const char *, unsigned char *,
		    size_t *, fsimg_error_t *);

/*
 * Attribute region validation (xattr.c): whether the inode holds an
 * attribute, after the inline region's structure is checked.
 */
fsimg_status_t	erofs_xattr_check(fsimg_reader_t *, const erofs_inode_t *,
		    bool *, fsimg_error_t *);

/*
 * XXH32 over a message given in pieces (codec.c): whole 16-byte stripes go
 * through the four lane accumulators as they complete, and stripe holds
 * the bytes of the stripe still being filled, which at the end are the
 * message's last total_bytes % 16 bytes. A copy continues independently.
 */
#define EROFS_XXH32_STRIPE_BYTES	16u

typedef struct erofs_xxh32_state {
	uint64_t	 total_bytes;
	uint32_t	 accumulator[4];
	uint32_t	 seed;
	unsigned char	 stripe[EROFS_XXH32_STRIPE_BYTES];
	size_t		 stripe_bytes;
} erofs_xxh32_state_t;

/*
 * An attribute name's filter hash in progress: XXH32 seeded for the name's
 * built-in prefix by erofs_name_hash_init(), absorbing the name piece by
 * piece, a long prefix's infix first and then the suffix. A long prefix's
 * hash is absorbed once and copied for each attribute using it.
 */
typedef erofs_xxh32_state_t erofs_name_hash_t;

/*
 * Verification (verify.c). Every reachable structure claims its byte range
 * of the primary space in one class, which the ownership sweep reconciles
 * after the walk:
 *
 *	EXCLUSIVE	the superblock region, directory blocks and the plain
 *			long-prefix table: overlaps nothing
 *	BODY		an inode's body: exclusive, except that an inode
 *			reached by several names claims its one body again
 *	SHARED		a shared attribute entry: overlaps only an identical
 *			shared claim, since inodes share pool entries
 *	DATA		file and symbolic-link blocks: may overlap data,
 *			since the format permits deduplicated content
 *
 * The values order the classes in the claim stream's sort.
 */
typedef enum erofs_claim_class {
	EROFS_CLAIM_EXCLUSIVE = 0,
	EROFS_CLAIM_BODY = 1,
	EROFS_CLAIM_SHARED = 2,
	EROFS_CLAIM_DATA = 3
} erofs_claim_class_t;

/* The owner a claim names when no inode owns it, such as the superblock. */
#define EROFS_CLAIM_NO_INODE		UINT64_MAX

/* Long prefixes an index 0x80 | k can name: k has seven bits. */
#define EROFS_LONG_PREFIX_MAX		128u

/*
 * One verification's state, from the first claim to the sweep: the claim
 * stream in scratch, and for each of the first min(xattr_prefix_count,
 * 128) long prefixes its name hash, seeded with its base index and with
 * its infix absorbed.
 */
typedef struct erofs_verifier {
	fsimg_stream_t	*claims;
	erofs_name_hash_t prefixes[EROFS_LONG_PREFIX_MAX];
} erofs_verifier_t;

fsimg_status_t	erofs_verify(fsimg_reader_t *, unsigned,
		    fsimg_verify_report_t *, fsimg_error_t *);
fsimg_status_t	erofs_claim(erofs_verifier_t *, erofs_claim_class_t, uint64_t,
		    uint64_t, uint64_t, fsimg_error_t *);

/*
 * Verification's attribute checks (xattr.c): the long-prefix table, loaded
 * into the verifier and claimed once, and an inode's every attribute, its
 * shared entries claimed and every name checked against its filter.
 */
fsimg_status_t	erofs_prefixes_load(fsimg_reader_t *, erofs_verifier_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_xattr_verify(fsimg_reader_t *, const erofs_inode_t *,
		    erofs_verifier_t *, fsimg_error_t *);

/*
 * The metadata reader (meta.c), the one place a NID or a metadata offset
 * becomes bytes. Every read goes through bounded I/O after its range is
 * proved inside its space.
 */
fsimg_status_t	erofs_meta_read(fsimg_reader_t *, erofs_space_t, uint64_t,
		    size_t, void *, fsimg_error_t *);
fsimg_status_t	erofs_inode_load(fsimg_reader_t *, uint64_t, erofs_inode_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_body_read(fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, size_t, void *, fsimg_error_t *);

/*
 * Decoders (codec.c). Each reads only the bytes and length it is given,
 * writes its output only on success, and returns FSIMG_OK, or
 * FSIMG_ECORRUPT or FSIMG_EUNSUPPORTED with *what set to a static phrase
 * naming the field; erofs_dirent_decode() also returns FSIMG_EINVAL for an
 * index past the block's entries, a caller's error. The exceptions to the
 * length rule are erofs_inode_fixed_bytes(), which reads the two bytes of
 * the format word, the accessors of a structure already decoded, and the
 * attribute and long-prefix decoders, which read only their fixed head, the
 * 12-byte header or a 4-byte entry or record head, and take as length what
 * remains of the region.
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
void		erofs_name_hash_init(erofs_name_hash_t *, uint8_t);
void		erofs_name_hash_absorb(erofs_name_hash_t *,
		    const unsigned char *, size_t);
unsigned	erofs_name_hash_bit(const erofs_name_hash_t *);

#endif /* !FSIMG_EROFS_H */
