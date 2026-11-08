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

/* The smallest MicroLZMA dictionary a record may declare. */
#define EROFS_LZMA_DICT_MIN	UINT32_C(4096)

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

/*
 * How an encoded-extent inode's records are found, by their width: a
 * binary search over the explicit logical offsets of 16- and 32-byte
 * records (SEARCHED); the logical cluster's record, which names its
 * physical offset in 8 bytes (INDEXED); or the logical cluster's record of
 * 4 bytes, whose physical offset is the running sum of the stored lengths
 * before it (RUNNING). NONE is an indexed inode.
 */
typedef enum erofs_extents_form {
	EROFS_EXTENTS_NONE,
	EROFS_EXTENTS_SEARCHED,
	EROFS_EXTENTS_INDEXED,
	EROFS_EXTENTS_RUNNING
} erofs_extents_form_t;

/*
 * A decoded map header. A whole-file fragment has no index and no advice:
 * the header is the 63-bit offset of the file's content in the packed
 * inode. Otherwise the first word is the fragment offset's low 32 bits
 * under advice EROFS_ADVICE_FRAGMENT, or holds the compressed inline
 * tail's stored length under EROFS_ADVICE_INLINE_TAIL; never both.
 *
 * An encoded-extent inode, a full-index inode of a 48-bit volume with
 * advice EROFS_ADVICE_EXTENTS, has extent_bytes records of 4, 8, 16 or 32
 * bytes in place of an index, and no algorithms, inline tail or fragment
 * offset in its header: each record names its own format, and the last
 * one is the fragment under EROFS_ADVICE_FRAGMENT. extent_count is the
 * header's count for 16- and 32-byte records; for 4- and 8-byte records,
 * each covering one logical cluster of 2^lclusterbits bytes, the header has
 * none and erofs_inode_load() derives it from the size. extents_form is
 * EROFS_EXTENTS_NONE and extent_bytes 0 for an index.
 */
typedef struct erofs_zheader {
	uint64_t	 fragment_offset;	/* whole or fragment */
	uint64_t	 extent_count;		/* encoded extents */
	erofs_extents_form_t extents_form;
	uint16_t	 advice;
	uint16_t	 tail_bytes;		/* inline tail */
	uint8_t		 head1_algorithm;
	uint8_t		 head2_algorithm;
	uint8_t		 extent_bytes;		/* encoded extents */
	uint8_t		 lclusterbits;		/* 4- and 8-byte extents */
	bool		 whole_fragment;
} erofs_zheader_t;

/*
 * The decoded fixed part of an inode. layout_offset is where its layout
 * bytes (the inline tail, the chunk index or the compressed map header)
 * start and body_bytes where they end, both from the inode's first byte, so
 * [0, body_bytes) is all the inode itself occupies.
 * layout_word is the word at 0x10: a start block, a chunk format, a
 * compressed inode's block count or a device number by the inode's layout
 * and kind. layout_high holds start-block
 * bits 32 to 47 under the 48-bit layout and is zero otherwise.
 * chunk_format is a chunk-based inode's, decoded with the body length it
 * determines, and zero for every other layout.
 * nid, space and offset say where the inode is: erofs_inode_load() sets
 * them, and erofs_inode_decode() alone leaves them zero. erofs_inode_load()
 * also decodes a compressed inode's map header into zheader, since the
 * body's length depends on it: a whole-file fragment's body ends at its header, and an
 * inline tail's stored bytes, from tail_offset to body_bytes, follow the
 * index.
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
	erofs_chunk_format_t chunk_format;
	uint32_t	 time_nsec;
	uint32_t	 fixed_bytes;		/* 32 or 64 */
	uint32_t	 xattr_bytes;		/* the attribute region */
	uint32_t	 layout_word;
	uint32_t	 nlink;
	erofs_zheader_t	 zheader;		/* compressed layouts */
	uint64_t	 tail_offset;		/* in the body: an inline tail */
	uint32_t	 serial;		/* reported; never an identity */
	uint32_t	 uid;
	uint32_t	 gid;
	uint16_t	 layout_high;
	uint16_t	 permissions;		/* the mode's low 12 bits */
	bool		 extended;
	/* A 48-bit directory whose "." record is absent. */
	bool		 omits_dot;
} erofs_inode_t;

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
 * Compressed layouts. An inode of either compressed layout starts its
 * layout bytes with an 8-byte map header at its attribute region's end,
 * rounded up to 8, and follows it with an index of one record per logical
 * cluster of B bytes. A full index holds 8-byte records from 16 bytes after
 * the header's start; a compact index packs records into groups from the
 * header's end.
 */
#define EROFS_ZHEADER_BYTES		8u
#define EROFS_FULL_INDEX_OFFSET		16u
#define EROFS_FULL_RECORD_BYTES		8u
/* Map header advice bits; any other bit is unsupported. */
#define EROFS_ADVICE_COMPACT_2B		0x0001u
#define EROFS_ADVICE_BIG_HEAD1		0x0002u
#define EROFS_ADVICE_BIG_HEAD2		0x0004u
#define EROFS_ADVICE_INLINE_TAIL	0x0008u
#define EROFS_ADVICE_INTERLACED		0x0010u
#define EROFS_ADVICE_FRAGMENT		0x0020u
#define EROFS_ADVICE_KNOWN		0x003fu
/*
 * On a full-index inode of a 48-bit volume bit 0x01 selects encoded
 * extents, and bits 1 and 2 give the record size, 4 << ((advice >> 1) & 3).
 * Records start at the header's end rounded up to their size; 4-byte
 * records follow a 64-bit physical start there.
 */
#define EROFS_ADVICE_EXTENTS		EROFS_ADVICE_COMPACT_2B
#define EROFS_EXTENT_START_BYTES	8u
#define EROFS_EXTENT_BYTES_MAX		32u
/* The 32-byte form, the one the writer emits, and the advice selecting it. */
#define EROFS_EXTENT32_BYTES		32u
#define EROFS_EXTENT32_ADVICE		(EROFS_ADVICE_EXTENTS | 3u << 1)
/* The largest physical cluster stored and logical extent decoded. */
#define EROFS_CLUSTER_STORED_MAX	(UINT32_C(1) << 20)
#define EROFS_CLUSTER_DECODED_MAX	(UINT32_C(12) << 20)
/*
 * A compact group holds 2 records in 8 bytes or 16 in 32 bytes; the full
 * index is read in units of up to 16 records.
 */
#define EROFS_GROUP_RECORDS_MAX		16u
#define EROFS_GROUP_BYTES_MAX		32u
#define EROFS_FULL_UNIT_RECORDS		16u
_Static_assert(EROFS_FULL_UNIT_RECORDS <= EROFS_GROUP_RECORDS_MAX,
    "a full-index unit fits the lookup cache's records");

/*
 * One encoded-extent record as stored: its length word, its physical byte
 * offset, and its logical byte offset, zero in the 4- and 8-byte forms,
 * which have none. The 4-byte form stores no physical offset either: the
 * decoder leaves it zero and a lookup fills in the running sum.
 */
typedef struct erofs_xrecord {
	uint64_t	 physical;
	uint64_t	 logical;
	uint32_t	 length_word;
} erofs_xrecord_t;

/*
 * An encoded extent's length word decoded: its stored bytes, its format, 0
 * for a plain extent and otherwise the algorithm plus one, and whether a
 * compressed extent is a partial reference to its stream.
 */
typedef struct erofs_xlength {
	uint32_t	 stored_bytes;
	uint8_t		 format;
	bool		 partial;
} erofs_xlength_t;

/* The type of an index record, its two type bits. */
typedef enum erofs_record_type {
	EROFS_RECORD_PLAIN = 0,
	EROFS_RECORD_HEAD1 = 1,
	EROFS_RECORD_NONHEAD = 2,
	EROFS_RECORD_HEAD2 = 3
} erofs_record_type_t;

/*
 * One index record, normalized from either index form. A head or plain
 * record starts a physical cluster at clusterofs within its logical cluster
 * and names its first block. A non-head record lies inside the cluster of
 * the nearest head before it, backward records back (one for a count
 * marker). The forward distances records may carry are not kept: the reader
 * finds a cluster's end from the records' types alone. A count marker, the
 * first non-head record of a big cluster, carries the cluster's block count.
 */
typedef struct erofs_zrecord {
	uint64_t	 block;			/* head or plain */
	erofs_record_type_t type;
	uint16_t	 clusterofs;		/* head or plain */
	uint16_t	 backward;		/* non-head */
	uint16_t	 marker_blocks;		/* count marker */
	bool		 marker;
	bool		 partial;		/* full head: a partial reference */
} erofs_zrecord_t;

/*
 * Where the records of a compact index lie, from the header's end: an
 * initial run of 8-byte groups up to a 32-byte boundary, then 32-byte groups,
 * then 8-byte groups. total_bytes is the index's length.
 */
typedef struct erofs_compact_layout {
	uint64_t	 records;
	uint64_t	 initial_records;	/* in the first run of 8-byte groups */
	uint64_t	 wide_records;		/* in 32-byte groups */
	uint64_t	 total_bytes;
} erofs_compact_layout_t;

/*
 * The unit of an index a lookup reads to find one record: a compact group,
 * or up to 16 consecutive full records. offset is measured from the first
 * record's position, the header's end for a compact index; slots is a
 * compact group's form.
 */
typedef struct erofs_index_unit {
	uint64_t	 offset;		/* from the first record's place */
	unsigned	 bytes;			/* 8 or 32 */
	unsigned	 slots;			/* 2 or 16 */
	unsigned	 valid;			/* slots holding records */
	unsigned	 slot;			/* the record's slot */
} erofs_index_unit_t;

/* How a physical cluster's stored bytes become its logical extent. */
typedef enum erofs_cluster_mode {
	EROFS_CLUSTER_SHIFTED,		/* plain: extent byte j at byte j */
	EROFS_CLUSTER_INTERLACED,	/* plain: rotated by rotation */
	EROFS_CLUSTER_COMPRESSED	/* a stream of algorithm */
} erofs_cluster_mode_t;

/*
 * Where a physical cluster's stored bytes lie, and so what storage_offset
 * means:
 *
 *   BLOCKS  whole blocks of the volume; a volume byte offset.
 *   INLINE  an inline tail in an inode's body; the tail's byte offset in
 *           the inode's metadata space, which the cluster's space names,
 *           never one relative to the body, so that two equal window keys
 *           name the same bytes.
 *   PACKED  a fragment: an offset into the packed inode's content. It is no
 *           cluster to decode; the mapper maps it through the packed inode.
 */
typedef enum erofs_source {
	EROFS_SOURCE_BLOCKS,
	EROFS_SOURCE_INLINE,
	EROFS_SOURCE_PACKED
} erofs_source_t;

/*
 * One normalized physical cluster: its logical extent [logical_offset,
 * logical_offset + logical_length) of the inode, and stored_bytes from
 * storage_offset of its source. A compressed cluster's stream decodes to
 * the extent, only partly when partial marks a shared cluster; a plain
 * cluster stores the extent, from its first byte or rotated so that extent
 * byte j lies at stored byte (rotation + j) mod stored_bytes. Every field
 * but logical_offset decides the decoded bytes, so those fields are the
 * decode window's key. A fragment, source EROFS_SOURCE_PACKED, has no
 * stored bytes, mode or algorithm of its own: the mapper reads the packed
 * inode's content at storage_offset instead.
 */
typedef struct erofs_cluster {
	uint64_t	 logical_offset;
	uint64_t	 logical_length;
	uint64_t	 storage_offset;
	uint32_t	 stored_bytes;
	uint32_t	 rotation;		/* interlaced */
	erofs_source_t	 source;
	erofs_space_t	 space;			/* an inline tail's */
	erofs_cluster_mode_t mode;
	uint8_t		 algorithm;		/* compressed */
	bool		 partial;		/* compressed */
} erofs_cluster_t;

/*
 * A decode window: one decoded cluster, keyed by the cluster it holds.
 * input holds the stored bytes and output the logical extent; both grow to
 * the largest cluster seen, charged to the context, and are owned until
 * close. keyed is false whenever output may not hold the key's bytes.
 */
typedef struct erofs_window {
	erofs_cluster_t	 key;
	unsigned char	*input;
	unsigned char	*output;
	size_t		 input_capacity;
	size_t		 output_capacity;
	bool		 keyed;
} erofs_window_t;

/*
 * The compressed lookup's memory of inode nid: the last index unit it read,
 * a compact group or up to 16 full records, decoded; and for 4-byte encoded
 * extents, whose physical offsets are running sums, the physical offset at
 * which record run_record starts. A lookup of another inode discards both,
 * so a hit returns what a fresh read and decode would.
 */
typedef struct erofs_zcache {
	erofs_zrecord_t	 records[EROFS_GROUP_RECORDS_MAX];
	uint64_t	 nid;
	uint64_t	 unit_offset;		/* in the body; the unit's start */
	uint64_t	 run_record;		/* 4-byte encoded extents */
	uint64_t	 run_physical;
	bool		 unit_valid;
	bool		 run_valid;
} erofs_zcache_t;

/*
 * Where a range of an inode's content is stored, as erofs_map() returns it:
 * in the volume's blocks, in a physical cluster that must be decoded or
 * rearranged, which includes an inline tail kept in the inode's body, or
 * nowhere, for a chunk hole, which reads as zeros.
 */
typedef enum erofs_extent_kind {
	EROFS_EXTENT_STORED,
	EROFS_EXTENT_CLUSTER,
	EROFS_EXTENT_ZERO
} erofs_extent_kind_t;

/*
 * One normalized extent: the logical bytes [logical_offset,
 * logical_offset + length) of an inode. A stored extent's bytes lie from
 * the volume byte storage_offset inside whole blocks of the content domain;
 * a zero extent has none. A cluster extent's bytes are cluster's decoded
 * bytes from cluster_skip on; cluster's stored bytes lie inside the content
 * domain, or inside an inode's body that erofs_inode_load() checked.
 *
 * fragment marks an extent of a file's fragment: its bytes are a range of
 * the packed inode's content, and the stored or cluster extent describes
 * where that range lies, clipped to both the fragment and the packed
 * inode's extent holding it. Its storage belongs to the packed inode, so
 * the verifier claims it there. Only a fragment's cluster extent starts
 * inside its cluster, with a nonzero cluster_skip.
 */
typedef struct erofs_extent {
	uint64_t	 logical_offset;
	uint64_t	 storage_offset;	/* a stored extent */
	uint64_t	 length;
	uint64_t	 cluster_skip;		/* a cluster extent */
	erofs_extent_kind_t kind;
	erofs_cluster_t	 cluster;		/* a cluster extent */
	bool		 fragment;
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
	bool		 box;			/* METABOX */
	bool		 pool_in_box;		/* SHARED_EA_IN_METABOX */
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
 * from them, and resolve the current file. packed is the packed inode,
 * loaded at open when has_packed says the volume names one. content_window
 * holds the cluster resolve last handed to delivery and meta_window the one
 * the driver's own readers, directories, symbolic links and the long-prefix
 * table, last copied from, so neither evicts the other's bytes; zcache
 * holds the compressed lookup's last index unit.
 *
 * box is the metadata box inode, loaded at open when has_box says the
 * volume has one; its content is the second metadata space. A box read
 * happens in the middle of another inode's lookup or window fill, when that
 * inode lives in the box, so the box has a window and a lookup cache of its
 * own, box_window and box_zcache, which nothing else touches: a box read
 * never evicts the state of the lookup it serves. The box inode lives in
 * the primary space and uses no fragment, so a box read never needs another
 * box read.
 */
typedef struct fsimg_erofs_reader {
	erofs_super_t	 super;
	erofs_config_t	 config;
	erofs_features_t features;
	erofs_inode_t	 current;
	uint64_t	 current_parent;
	uint64_t	 domain_bytes;
	erofs_inode_t	 packed;
	erofs_inode_t	 box;
	erofs_window_t	 content_window;
	erofs_window_t	 meta_window;
	erofs_window_t	 box_window;
	erofs_zcache_t	 zcache;
	erofs_zcache_t	 box_zcache;
	uint32_t	 block_bytes;
	bool		 has_packed;
	bool		 has_box;
} fsimg_erofs_reader_t;

extern const fsimg_reader_ops_t fsimg_erofs_reader_ops;

/* Driver state and error reporting (read.c). */
fsimg_erofs_reader_t *erofs_state(const fsimg_reader_t *);
void		erofs_reject_report(fsimg_error_t *, fsimg_status_t, const char *);

/*
 * Record a rejection and return its status, which is a failure; it is
 * returned unchanged. Inline, so the compiler sees that a helper's
 * `return (erofs_reject(...))` never returns FSIMG_OK and that its caller,
 * which returns on failure, never reads the outputs a failed helper left
 * unset.
 */
static inline fsimg_status_t
erofs_reject(fsimg_error_t *error, fsimg_status_t status, const char *what)
{
	erofs_reject_report(error, status, what);
	return (status);
}

/*
 * The mapper (map.c), the one place a logical offset of an inode becomes
 * storage, and the content reader (content.c) built on it, which reads a
 * logical range of any inode into a caller buffer.
 */
fsimg_status_t	erofs_map(fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, erofs_extent_t *, fsimg_error_t *);

/*
 * Compressed lookups (zmap.c): the physical cluster of a compressed inode
 * holding a logical offset below its size.
 */
fsimg_status_t	erofs_zmap(fsimg_reader_t *, const erofs_inode_t *, uint64_t,
		    erofs_cluster_t *, fsimg_error_t *);
fsimg_status_t	erofs_extents_check(fsimg_reader_t *, const erofs_inode_t *,
		    fsimg_error_t *);
uint64_t	erofs_extents_offset(const erofs_inode_t *);

/*
 * Decode windows (cluster.c). erofs_cluster_readable() says whether this
 * build decodes a cluster, FSIMG_EUNSUPPORTED naming what it lacks;
 * erofs_window_fill() leaves a cluster's logical extent in a window's output.
 */
void		erofs_codecs(char *, size_t);
fsimg_status_t	erofs_cluster_readable(const fsimg_reader_t *,
		    const erofs_cluster_t *, fsimg_error_t *);
fsimg_status_t	erofs_window_fill(fsimg_reader_t *, erofs_window_t *,
		    const erofs_cluster_t *, fsimg_error_t *);
void		erofs_window_free(erofs_window_t *);
fsimg_status_t	erofs_content_copy(fsimg_reader_t *, erofs_window_t *,
		    const erofs_inode_t *, uint64_t, size_t, void *, size_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_content_read(fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, size_t, void *, size_t *, fsimg_error_t *);
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
 * of the metadata space it lies in, in one class, which the ownership sweep
 * of that space reconciles after the walk:
 *
 *	EXCLUSIVE	the superblock region, inode bodies, directory blocks,
 *			the metadata box's blocks and a long-prefix table
 *			outside the packed inode: overlaps nothing
 *	SHARED		a shared attribute entry: overlaps only an identical
 *			shared claim, since inodes share pool entries
 *	DATA		file and symbolic-link blocks: may overlap data,
 *			since the format permits deduplicated content
 *
 * The values order the classes in the claim stream's sort.
 */
typedef enum erofs_claim_class {
	EROFS_CLAIM_EXCLUSIVE = 0,
	EROFS_CLAIM_SHARED = 1,
	EROFS_CLAIM_DATA = 2
} erofs_claim_class_t;

/* The owner a claim names when no inode owns it, such as the superblock. */
#define EROFS_CLAIM_NO_INODE		UINT64_MAX

/* Long prefixes an index 0x80 | k can name: k has seven bits. */
#define EROFS_LONG_PREFIX_MAX		128u

/*
 * One verification's state, from the first claim to the sweep: a claim stream
 * in scratch per metadata space, the box's only under the box, and for each of
 * the first min(xattr_prefix_count, 128) long prefixes its name hash, seeded
 * with its base index and with its infix absorbed.
 */
typedef struct erofs_verifier {
	fsimg_stream_t	*claims[EROFS_SPACE_BOX + 1];	/* by space */
	erofs_name_hash_t prefixes[EROFS_LONG_PREFIX_MAX];
} erofs_verifier_t;

fsimg_status_t	erofs_verify(fsimg_reader_t *, unsigned,
		    fsimg_verify_report_t *, fsimg_error_t *);
fsimg_status_t	erofs_claim(erofs_verifier_t *, erofs_space_t,
		    erofs_claim_class_t, uint64_t, uint64_t, uint64_t,
		    fsimg_error_t *);

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
uint64_t	erofs_space_bytes(const fsimg_erofs_reader_t *, erofs_space_t);
fsimg_status_t	erofs_meta_read(fsimg_reader_t *, erofs_space_t, uint64_t,
		    size_t, void *, fsimg_error_t *);
fsimg_status_t	erofs_inode_load(fsimg_reader_t *, uint64_t, erofs_inode_t *,
		    fsimg_error_t *);
fsimg_status_t	erofs_body_read(fsimg_reader_t *, const erofs_inode_t *,
		    uint64_t, size_t, void *, fsimg_error_t *);

/*
 * Decoders and encoders (codec.c). Each decoder reads only the bytes and
 * length it is given, writes its output only on success, and returns
 * FSIMG_OK, or FSIMG_ECORRUPT or FSIMG_EUNSUPPORTED with *what set to a
 * static phrase naming the field; erofs_dirent_decode() also returns
 * FSIMG_EINVAL for an index past the block's entries, a caller's error.
 * The exceptions to the length rule are erofs_inode_fixed_bytes(), which
 * reads the two bytes of the format word, the accessors of a structure
 * already decoded, and the attribute and long-prefix decoders, which read
 * only their fixed head, the 12-byte header or a 4-byte entry or record
 * head, and take as length what remains of the region. Rules that relate
 * one structure to another, such as an address inside the volume, belong
 * to their callers. The encoders, erofs_super_encode(),
 * erofs_inode_encode(), erofs_zheader_encode(), erofs_xrecord_encode(),
 * erofs_xlength_encode() and erofs_full_record_encode(), write a decoder's
 * exact inverse into a buffer of the structure's fixed length, the
 * superblock's including its extension slots, and return
 * false, leaving the buffer unspecified, for a record the encoding cannot
 * represent.
 */
bool		erofs_magic_matches(const unsigned char *, size_t);
fsimg_status_t	erofs_super_probe(const unsigned char *, size_t,
		    const char **);
fsimg_status_t	erofs_region_decode(const unsigned char *, size_t,
		    erofs_super_t *, erofs_config_t *, const char **);
size_t		erofs_super_covered_bytes(unsigned);
bool		erofs_super_checksum(const unsigned char *, size_t, unsigned,
		    uint32_t *);
bool		erofs_super_encode(const erofs_super_t *, unsigned char *);
size_t		erofs_inode_fixed_bytes(const unsigned char *);
fsimg_status_t	erofs_inode_decode(const unsigned char *, size_t,
		    const erofs_super_t *, erofs_inode_t *, const char **);
bool		erofs_inode_encode(const erofs_inode_t *, const erofs_super_t *,
		    unsigned char *);
void		erofs_device_decode(uint32_t, uint32_t *, uint32_t *);
fsimg_status_t	erofs_chunk_format_decode(uint32_t, const erofs_super_t *,
		    erofs_chunk_format_t *, const char **);
fsimg_status_t	erofs_chunk_decode(const unsigned char *, size_t,
		    const erofs_chunk_format_t *, erofs_chunk_t *, const char **);
fsimg_status_t	erofs_zheader_decode(const unsigned char *,
		    const erofs_super_t *, erofs_layout_t, erofs_zheader_t *,
		    const char **);
fsimg_status_t	erofs_full_record_decode(const unsigned char *, bool,
		    erofs_zrecord_t *, const char **);
fsimg_status_t	erofs_xrecord_decode(const unsigned char *, unsigned,
		    erofs_xrecord_t *, const char **);
fsimg_status_t	erofs_xlength_decode(uint32_t, erofs_xlength_t *,
		    const char **);
bool		erofs_zheader_encode(const erofs_zheader_t *, unsigned char *);
bool		erofs_xrecord_encode(const erofs_xrecord_t *, unsigned,
		    unsigned char *);
bool		erofs_xlength_encode(const erofs_xlength_t *, uint32_t *);
bool		erofs_full_record_encode(const erofs_zrecord_t *, uint16_t,
		    unsigned char *);
void		erofs_compact_layout(uint64_t, uint64_t,
		    erofs_compact_layout_t *);
void		erofs_compact_locate(const erofs_compact_layout_t *, uint64_t,
		    erofs_index_unit_t *);
fsimg_status_t	erofs_compact_group_decode(const unsigned char *,
		    const erofs_index_unit_t *, bool, erofs_zrecord_t *,
		    const char **);
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
